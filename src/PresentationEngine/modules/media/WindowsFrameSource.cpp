// Windows FrameSource: a video frame at any position from Media Foundation, and the
// shell's own preview (the picture Explorer shows for a file) for photos and for any
// video Media Foundation cannot open.

#ifdef _WIN32

#include "modules/media/FrameSource.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlobj.h>
#include <wincodec.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace bps::media {

namespace {

constexpr const char* kModule = "WindowsFrameSource";

// Minimal COM smart pointer (no ATL/WRL dependency).
template <class T>
class Com {
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p_) p_->Release(); }
    T** put() { return &p_; }
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_ = nullptr;
};

// COM and Media Foundation are per-thread/per-process state; the cache calls from its
// own worker threads, so each request sets them up and tears them down again.
class ComScope {
public:
    ComScope() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // S_OK / S_FALSE need a matching CoUninitialize; RPC_E_CHANGED_MODE does not.
        coBalanced_ = SUCCEEDED(co);
        mfStarted_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~ComScope() {
        if (mfStarted_) MFShutdown();
        if (coBalanced_) CoUninitialize();
    }
    bool mediaFoundation() const { return mfStarted_; }
private:
    bool coBalanced_ = false;
    bool mfStarted_ = false;
};

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    std::replace(out.begin(), out.end(), L'/', L'\\');
    return out;
}

// Scales BGRA rows (row 0 first, `stride` bytes apart) to `targetWidth` wide - never larger
// than the source - and returns tightly packed RGBA with the alpha forced opaque (a preview
// is opaque; the alpha of decoded video and of shell bitmaps is not reliable).
Picture ScaleToRgba(const uint8_t* bgra, int sw, int sh, size_t stride, int targetWidth) {
    Picture out;
    if (!bgra || sw <= 0 || sh <= 0) return out;
    int tw = std::min(sw, std::max(1, targetWidth));
    int th = std::max(1, static_cast<int>(std::lround(static_cast<double>(sh) * tw / sw)));
    out.width = tw;
    out.height = th;
    out.rgba.resize(static_cast<size_t>(tw) * static_cast<size_t>(th) * 4);

    // Bilinear sampling; the box average of a big reduction is approximated by sampling the
    // centre of each target pixel's source area, which is plenty for a thumbnail.
    const double xr = static_cast<double>(sw) / tw;
    const double yr = static_cast<double>(sh) / th;
    for (int y = 0; y < th; ++y) {
        const double fy = std::clamp((y + 0.5) * yr - 0.5, 0.0, static_cast<double>(sh - 1));
        const int y0 = static_cast<int>(fy);
        const int y1 = std::min(y0 + 1, sh - 1);
        const double wy = fy - y0;
        for (int x = 0; x < tw; ++x) {
            const double fx = std::clamp((x + 0.5) * xr - 0.5, 0.0, static_cast<double>(sw - 1));
            const int x0 = static_cast<int>(fx);
            const int x1 = std::min(x0 + 1, sw - 1);
            const double wx = fx - x0;
            const uint8_t* p00 = bgra + static_cast<size_t>(y0) * stride + static_cast<size_t>(x0) * 4;
            const uint8_t* p10 = bgra + static_cast<size_t>(y0) * stride + static_cast<size_t>(x1) * 4;
            const uint8_t* p01 = bgra + static_cast<size_t>(y1) * stride + static_cast<size_t>(x0) * 4;
            const uint8_t* p11 = bgra + static_cast<size_t>(y1) * stride + static_cast<size_t>(x1) * 4;
            uint8_t* dst = out.rgba.data() + (static_cast<size_t>(y) * tw + x) * 4;
            for (int c = 0; c < 3; ++c) {   // B, G, R -> R, G, B
                const double top = p00[c] * (1 - wx) + p10[c] * wx;
                const double bottom = p01[c] * (1 - wx) + p11[c] * wx;
                dst[2 - c] = static_cast<uint8_t>(std::lround(top * (1 - wy) + bottom * wy));
            }
            dst[3] = 255;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Video: Media Foundation
// ---------------------------------------------------------------------------

Result<Picture> GrabVideo(const std::string& path, double fraction, int targetWidth) {
    ComScope scope;
    if (!scope.mediaFoundation())
        return Error::Make(Err::Unsupported, kModule, "Media Foundation is not available");

    // Let the reader convert whatever the codec produces (NV12...) to RGB32 for us.
    Com<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(attrs.put(), 1)))
        return Error::Make(Err::IoError, kModule, "MFCreateAttributes failed");
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    Com<IMFSourceReader> reader;
    const std::wstring wide = Widen(path);
    if (FAILED(MFCreateSourceReaderFromURL(wide.c_str(), attrs.get(), reader.put())))
        return Error::Make(Err::Unsupported, kModule, "Media Foundation cannot open this video");

    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE)))
        return Error::Make(Err::Unsupported, kModule, "the file has no video stream");

    Com<IMFMediaType> wanted;
    if (FAILED(MFCreateMediaType(wanted.put())))
        return Error::Make(Err::IoError, kModule, "MFCreateMediaType failed");
    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                                           wanted.get())))
        return Error::Make(Err::Unsupported, kModule, "cannot decode this video to RGB");

    Com<IMFMediaType> actual;
    if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), actual.put())))
        return Error::Make(Err::IoError, kModule, "no decoded media type");
    UINT32 width = 0, height = 0;
    if (FAILED(MFGetAttributeSize(actual.get(), MF_MT_FRAME_SIZE, &width, &height)) || !width || !height)
        return Error::Make(Err::IoError, kModule, "unknown frame size");
    LONG stride = static_cast<LONG>(MFGetAttributeUINT32(actual.get(), MF_MT_DEFAULT_STRIDE, 0));
    if (stride == 0) stride = static_cast<LONG>(width) * 4;

    // Seek to `fraction` of the way through.
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                   MF_PD_DURATION, &duration))
        && duration.vt == VT_UI8 && duration.uhVal.QuadPart > 0 && fraction > 0.0) {
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = static_cast<LONGLONG>(static_cast<double>(duration.uhVal.QuadPart) * fraction);
        reader->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
    }
    PropVariantClear(&duration);

    // The first sample(s) after a seek can be empty (stream ticks, gaps): read until a frame.
    Com<IMFSample> sample;
    for (int attempt = 0; attempt < 40 && !sample; ++attempt) {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &streamIndex,
                                      &flags, &timestamp, sample.put())))
            return Error::Make(Err::IoError, kModule, "ReadSample failed");
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) break;
    }
    if (!sample)
        return Error::Make(Err::IoError, kModule, "no frame could be read");

    Com<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())))
        return Error::Make(Err::IoError, kModule, "no frame buffer");
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length)) || !data)
        return Error::Make(Err::IoError, kModule, "cannot lock the frame");

    // Re-pack to a top-down BGRA image (a negative stride means the rows are bottom-up).
    const size_t absStride = static_cast<size_t>(stride < 0 ? -stride : stride);
    std::vector<uint8_t> topDown(static_cast<size_t>(width) * height * 4);
    bool ok = (absStride * height <= length) || (absStride * (height - 1) + width * 4 <= length);
    if (ok) {
        for (UINT32 y = 0; y < height; ++y) {
            const size_t srcRow = stride < 0 ? (height - 1 - y) : y;
            std::memcpy(topDown.data() + static_cast<size_t>(y) * width * 4, data + srcRow * absStride,
                        static_cast<size_t>(width) * 4);
        }
    }
    buffer->Unlock();
    if (!ok)
        return Error::Make(Err::IoError, kModule, "the frame buffer is smaller than its size");

    Picture frame = ScaleToRgba(topDown.data(), static_cast<int>(width), static_cast<int>(height),
                              static_cast<size_t>(width) * 4, targetWidth);
    if (frame.empty()) return Error::Make(Err::IoError, kModule, "empty frame");
    return Result<Picture>{std::move(frame)};
}

// ---------------------------------------------------------------------------
// Images: WIC, the Windows Imaging Component
// ---------------------------------------------------------------------------

// Decodes any format WIC has a codec for (JPEG, PNG, BMP, GIF, TIFF, ICO, and HEIC / WebP / AVIF
// where the Windows extension is installed), scaled with WIC's own high-quality filter. Unlike the
// shell preview this keeps the picture's transparency.
Result<Picture> GrabImage(const std::string& path, int targetWidth) {
    ComScope scope;
    Com<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))))
        return Error::Make(Err::Unsupported, kModule, "WIC is not available");
    const std::wstring wide = Widen(path);
    Com<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(wide.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, decoder.put())))
        return Error::Make(Err::Unsupported, kModule, "Windows has no decoder for this image");
    Com<IWICBitmapFrameDecode> first;
    if (FAILED(decoder->GetFrame(0, first.put())))
        return Error::Make(Err::IoError, kModule, "no image frame");
    UINT width = 0, height = 0;
    if (FAILED(first->GetSize(&width, &height)) || !width || !height)
        return Error::Make(Err::IoError, kModule, "unknown image size");

    const UINT tw = std::min<UINT>(width, static_cast<UINT>(std::max(1, targetWidth)));
    const UINT th = std::max<UINT>(1, static_cast<UINT>(std::lround(static_cast<double>(height) * tw / width)));
    Com<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(first.get(), tw, th, WICBitmapInterpolationModeFant)))
        return Error::Make(Err::IoError, kModule, "cannot scale the image");
    Com<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(scaler.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return Error::Make(Err::IoError, kModule, "cannot convert the image");

    std::vector<uint8_t> bgra(static_cast<size_t>(tw) * th * 4);
    if (FAILED(converter->CopyPixels(nullptr, tw * 4, static_cast<UINT>(bgra.size()), bgra.data())))
        return Error::Make(Err::IoError, kModule, "cannot read the image pixels");

    Picture out;
    out.width = static_cast<int>(tw);
    out.height = static_cast<int>(th);
    out.rgba.resize(bgra.size());
    for (size_t i = 0; i < bgra.size(); i += 4) {   // BGRA -> RGBA, alpha kept
        out.rgba[i] = bgra[i + 2];
        out.rgba[i + 1] = bgra[i + 1];
        out.rgba[i + 2] = bgra[i];
        out.rgba[i + 3] = bgra[i + 3];
    }
    return Result<Picture>{std::move(out)};
}

// ---------------------------------------------------------------------------
// Shell preview (photos, and videos Media Foundation cannot open)
// ---------------------------------------------------------------------------

Result<Picture> GrabShell(const std::string& path, bool videoNeedsThumbnail, int targetWidth) {
    ComScope scope;
    const std::wstring wide = Widen(path);
    Com<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_PPV_ARGS(factory.put()))))
        return Error::Make(Err::NotFound, kModule, "the shell cannot open this file");

    const int wantW = std::max(1, targetWidth);
    const SIZE want = { wantW, std::max(1, wantW * 9 / 16) };
    // A file icon is not a preview: for a video the shell must have a real thumbnail; a photo
    // can always be drawn.
    const int flags = SIIGBF_BIGGERSIZEOK | (videoNeedsThumbnail ? SIIGBF_THUMBNAILONLY : 0);
    HBITMAP bitmap = nullptr;
    if (FAILED(factory->GetImage(want, static_cast<SIIGBF>(flags), &bitmap)) || !bitmap)
        return Error::Make(Err::Unsupported, kModule, "the shell has no preview for this file");

    BITMAP info = {};
    Result<Picture> result = Error::Make(Err::IoError, kModule, "cannot read the shell bitmap");
    if (GetObject(bitmap, sizeof(info), &info) && info.bmWidth > 0 && info.bmHeight > 0) {
        std::vector<uint8_t> bgra(static_cast<size_t>(info.bmWidth) * info.bmHeight * 4);
        BITMAPINFO header = {};
        header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        header.bmiHeader.biWidth = info.bmWidth;
        header.bmiHeader.biHeight = -info.bmHeight;   // top-down
        header.bmiHeader.biPlanes = 1;
        header.bmiHeader.biBitCount = 32;
        header.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        const int lines = GetDIBits(dc, bitmap, 0, static_cast<UINT>(info.bmHeight), bgra.data(), &header,
                                    DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        if (lines > 0) {
            Picture frame = ScaleToRgba(bgra.data(), info.bmWidth, info.bmHeight,
                                      static_cast<size_t>(info.bmWidth) * 4, targetWidth);
            if (!frame.empty()) result = Result<Picture>{std::move(frame)};
        }
    }
    DeleteObject(bitmap);
    return result;
}

class WindowsFrameSource final : public IFrameSource {
public:
    Result<Picture> Grab(const std::string& path, MediaKind kind, double fraction, int targetWidth) override {
        // Audio: the cover art the shell shows for the file (an embedded album picture), if there is one.
        if (kind == MediaKind::Audio)
            return GrabShell(path, true, targetWidth);
        if (kind == MediaKind::Video) {
            auto frame = GrabVideo(path, fraction, targetWidth);
            if (frame.ok()) return frame;
            // Not decodable by Media Foundation (odd codec / container): the shell may still have
            // a poster for it. That is a still, so it only stands in for the still (the middle),
            // never for a specific position - moving frames would all show the same picture.
            if (fraction != 0.5) return frame.error();
            return GrabShell(path, true, targetWidth);
        }
        // Photos: WIC first (any installed codec, transparency kept); the shell's preview is the
        // fallback for formats WIC has no codec for but Explorer can still show.
        auto image = GrabImage(path, targetWidth);
        if (image.ok()) return image;
        return GrabShell(path, false, targetWidth);
    }
};

} // namespace

std::shared_ptr<IFrameSource> MakeWindowsFrameSource() {
    return std::make_shared<WindowsFrameSource>();
}

} // namespace bps::media

#endif // _WIN32
