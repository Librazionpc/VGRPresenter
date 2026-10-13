#include "platform/windows/WindowsClipboard.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>   // DROPFILES lives here, not in shellapi.h

#include <cwchar>
#include <vector>

namespace bps::platform {

namespace {
// Plain Error, not Result<void> — this is returned from functions with
// several different Result<T> return types (string, vector<string>, void),
// and Error converts implicitly to any of them; a concrete Result<void>
// would only convert to itself.
Error ClipboardError(std::string_view op) {
    return Error::Make(Err::IoError, "Clipboard",
                       std::string(op) + " failed: " + win::LastErrorString(GetLastError()));
}

// Opening the clipboard is a LOAN, not a read: Windows hands it to one process at
// a time, and every other program on the machine takes it whenever it copies. A
// screenshot is the worst case — the snipping tool writes a bitmap of the whole
// screen into it — and the owner holds the clipboard for as long as that write
// takes, so a read that gives up on the first ERROR_ACCESS_DENIED fails on a
// machine that is doing nothing wrong. Qt probes the same clipboard on every
// change (that is what produced the app's amber "Retrying to obtain clipboard."
// notice), and the Windows guidance for this is a bounded retry: keep asking for
// about a second, which covers a screenshot's write, and return the honest error
// when the clipboard is genuinely stuck (a hung owner never releases it).
//
// Sleeps are on the clipboard's own millisecond scale and hold no lock: this file
// owns none (see tools/check_lock_scope.mjs, which fails the build on a lock held
// across a wait).
bool OpenClipboardRetrying(HWND owner) {
    constexpr int kAttempts = 12;   // 12 x 80 ms ≈ 1 s of patience
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (OpenClipboard(owner))
            return true;
        if (attempt + 1 < kAttempts)
            Sleep(80);
    }
    return false;
}

// ---- bitmaps --------------------------------------------------------------
//
// Windows moves bitmaps as DIBs: a BITMAPINFOHEADER (or a V4/V5 header — the
// 40-byte one with the same geometry and extra colour fields trailing) followed
// by the pixel rows. Rows are BOTTOM-UP unless the header's height is negative;
// 24/32 bpp BI_RGB is BGR / BGRX; and a screenshot's 32-bit pixels usually
// carry no alpha at all (Windows' screenshot path writes zero there, which
// would paste fully transparent) — so an entirely zero alpha channel is read
// back as opaque, which is what the picture actually is.
uint32_t ReadU32(const uint8_t* p, size_t offset) {
    return static_cast<uint32_t>(p[offset]) | (static_cast<uint32_t>(p[offset + 1]) << 8)
        | (static_cast<uint32_t>(p[offset + 2]) << 16) | (static_cast<uint32_t>(p[offset + 3]) << 24);
}

// Tightly packed BGRA (or BGRX) → ClipboardImage's RGBA. `fixZeroAlpha` turns
// an all-transparent image opaque (see the note above).
ClipboardImage RgbaFrom(const std::vector<uint8_t>& bgra, int width, int height, bool fixZeroAlpha) {
    ClipboardImage out;
    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    bool anyAlpha = false;
    for (size_t i = 0; i < out.rgba.size(); i += 4) {
        out.rgba[i + 0] = bgra[i + 2];   // R
        out.rgba[i + 1] = bgra[i + 1];   // G
        out.rgba[i + 2] = bgra[i + 0];   // B
        out.rgba[i + 3] = bgra[i + 3];   // A
        if (bgra[i + 3] != 0)
            anyAlpha = true;
    }
    if (fixZeroAlpha && !anyAlpha)
        for (size_t i = 3; i < out.rgba.size(); i += 4)
            out.rgba[i] = 255;
    return out;
}

Result<ClipboardImage> ImageFromDib(const std::vector<uint8_t>& dib) {
    if (dib.size() < sizeof(BITMAPINFOHEADER))
        return Error::Make(Err::IoError, "Clipboard", "clipboard bitmap is too small to be a DIB");
    const uint8_t* p = dib.data();
    const uint32_t headerSize = ReadU32(p, 0);
    const int32_t width = static_cast<int32_t>(ReadU32(p, 4));
    const int32_t rawHeight = static_cast<int32_t>(ReadU32(p, 8));
    const uint16_t bitCount = static_cast<uint16_t>(p[14] | (static_cast<uint16_t>(p[15]) << 8));
    const uint32_t compression = ReadU32(p, 16);
    if (width <= 0 || rawHeight == 0)
        return Error::Make(Err::IoError, "Clipboard", "clipboard bitmap has no area");
    if (bitCount != 24 && bitCount != 32)
        return Error::Make(Err::Unsupported, "Clipboard", "clipboard bitmap is not 24/32-bit");
    if (compression != BI_RGB && compression != BI_BITFIELDS)
        return Error::Make(Err::Unsupported, "Clipboard", "clipboard bitmap uses a compressed format");

    const int height = rawHeight < 0 ? -rawHeight : rawHeight;
    const bool topDown = rawHeight < 0;
    // A BITFIELDS header may store its three colour masks right after a 40-byte
    // header (a V4/V5 header carries them inside its own size instead).
    size_t offset = headerSize > sizeof(BITMAPINFOHEADER) ? headerSize : sizeof(BITMAPINFOHEADER);
    if (compression == BI_BITFIELDS && headerSize <= sizeof(BITMAPINFOHEADER))
        offset += 12;

    const size_t bytesPerPixel = bitCount / 8;
    const size_t stride = (static_cast<size_t>(width) * bitCount + 31) / 32 * 4;
    if (offset + stride * static_cast<size_t>(height) > dib.size())
        return Error::Make(Err::IoError, "Clipboard", "clipboard bitmap is truncated");

    std::vector<uint8_t> bgra(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        const int srcRow = topDown ? y : height - 1 - y;
        const uint8_t* row = p + offset + stride * static_cast<size_t>(srcRow);
        uint8_t* dst = bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = row[static_cast<size_t>(x) * bytesPerPixel + 0];
            dst[x * 4 + 1] = row[static_cast<size_t>(x) * bytesPerPixel + 1];
            dst[x * 4 + 2] = row[static_cast<size_t>(x) * bytesPerPixel + 2];
            dst[x * 4 + 3] = bytesPerPixel == 4 ? row[static_cast<size_t>(x) * 4 + 3] : 255;
        }
    }
    return RgbaFrom(bgra, width, height, /*fixZeroAlpha=*/bitCount == 32);
}

// A CF_BITMAP handle (an HBITMAP, not a DIB) — ask GDI for its pixels as a
// top-down 32-bit DIB so both paths end in the same BGRA→RGBA conversion.
Result<ClipboardImage> ImageFromBitmap(HBITMAP bitmap) {
    BITMAP info{};
    if (GetObject(bitmap, sizeof(info), &info) == 0 || info.bmWidth <= 0 || info.bmHeight <= 0)
        return Error::Make(Err::IoError, "Clipboard", "clipboard bitmap could not be measured");
    const int width = info.bmWidth;
    const int height = info.bmHeight;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;   // negative → top-down rows
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> bgra(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    HDC screen = GetDC(nullptr);
    const int copied = screen ? GetDIBits(screen, bitmap, 0, static_cast<UINT>(height),
                                          bgra.data(), &bi, DIB_RGB_COLORS)
                              : 0;
    if (screen) ReleaseDC(nullptr, screen);
    if (copied == 0)
        return Error::Make(Err::IoError, "Clipboard", "clipboard bitmap could not be read");
    return RgbaFrom(bgra, width, height, /*fixZeroAlpha=*/true);
}
} // namespace

Result<std::string> WindowsClipboard::ReadText() const {
    if (!OpenClipboardRetrying(nullptr)) return ClipboardError("OpenClipboard");
    std::string text;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h)))
                text = win::Utf8(w);
            if (h) GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return text;
}

Result<void> WindowsClipboard::WriteText(std::string_view text) {
    if (!OpenClipboardRetrying(nullptr)) return ClipboardError("OpenClipboard");
    if (!EmptyClipboard()) {
        CloseClipboard();
        return ClipboardError("EmptyClipboard");
    }
    std::wstring wide = win::Wide(text);
    size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) {
        CloseClipboard();
        return ClipboardError("GlobalAlloc");
    }
    void* dst = GlobalLock(h);
    if (!dst) {
        GlobalFree(h);
        CloseClipboard();
        return ClipboardError("GlobalLock");
    }
    memcpy(dst, wide.c_str(), bytes);
    GlobalUnlock(h);
    if (SetClipboardData(CF_UNICODETEXT, h) == nullptr) {
        GlobalFree(h);
        CloseClipboard();
        return ClipboardError("SetClipboardData");
    }
    CloseClipboard();
    return Ok();
}

Result<ClipboardImage> WindowsClipboard::ReadImage() const {
    // CF_DIB first: it IS the pixels (a screenshot posts one), where CF_BITMAP
    // is a GDI object that has to be converted back to a DIB to be read.
    if (!OpenClipboardRetrying(nullptr)) return ClipboardError("OpenClipboard");
    Result<ClipboardImage> out =
        Error::Make(Err::NotFound, "Clipboard", "the clipboard holds no image");
    if (IsClipboardFormatAvailable(CF_DIB)) {
        if (HANDLE h = GetClipboardData(CF_DIB)) {
            if (const void* locked = GlobalLock(h)) {
                const uint8_t* first = static_cast<const uint8_t*>(locked);
                out = ImageFromDib(std::vector<uint8_t>(first, first + GlobalSize(h)));
                GlobalUnlock(h);
            }
        }
    } else if (IsClipboardFormatAvailable(CF_BITMAP)) {
        if (HANDLE h = GetClipboardData(CF_BITMAP)) {
            if (HBITMAP bitmap = static_cast<HBITMAP>(h))
                out = ImageFromBitmap(bitmap);
        }
    }
    CloseClipboard();
    return out;
}

Result<void> WindowsClipboard::WriteImage(const ClipboardImage& image) {
    if (image.empty())
        return Error::Make(Err::InvalidArgument, "Clipboard", "there are no pixels to copy");
    // A 32-bit BI_RGB DIB with bottom-up rows — the format every Windows
    // program that accepts a bitmap (Paint, Word, the Snipping Tool) reads.
    const size_t stride = static_cast<size_t>(image.width) * 4;
    const size_t pixelBytes = stride * static_cast<size_t>(image.height);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixelBytes);
    if (!h) return ClipboardError("GlobalAlloc");
    uint8_t* base = static_cast<uint8_t*>(GlobalLock(h));
    if (!base) {
        GlobalFree(h);
        return ClipboardError("GlobalLock");
    }
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = image.width;
    header.biHeight = image.height;   // positive → bottom-up
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(pixelBytes);
    memcpy(base, &header, sizeof(header));
    for (int y = 0; y < image.height; ++y) {
        const uint8_t* src = image.rgba.data()
            + static_cast<size_t>(image.height - 1 - y) * stride;
        uint8_t* row = base + sizeof(header) + static_cast<size_t>(y) * stride;
        for (int x = 0; x < image.width; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2];   // B
            row[x * 4 + 1] = src[x * 4 + 1];   // G
            row[x * 4 + 2] = src[x * 4 + 0];   // R
            row[x * 4 + 3] = src[x * 4 + 3];   // A
        }
    }
    GlobalUnlock(h);

    if (!OpenClipboardRetrying(nullptr)) {
        GlobalFree(h);
        return ClipboardError("OpenClipboard");
    }
    if (!EmptyClipboard()) {
        CloseClipboard();
        GlobalFree(h);
        return ClipboardError("EmptyClipboard");
    }
    // On success the clipboard owns the memory — freeing it here would corrupt
    // it (same contract as the text and file-list writers above).
    if (SetClipboardData(CF_DIB, h) == nullptr) {
        GlobalFree(h);
        CloseClipboard();
        return ClipboardError("SetClipboardData");
    }
    CloseClipboard();
    return Ok();
}

Result<std::vector<std::string>> WindowsClipboard::GetFiles() const {
    std::vector<std::string> files;
    if (!OpenClipboardRetrying(nullptr)) return ClipboardError("OpenClipboard");
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HANDLE h = GetClipboardData(CF_HDROP);
        if (h) {
            HDROP drop = static_cast<HDROP>(h);
            UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
                if (len == 0) continue;
                std::wstring path(len, L'\0');
                ::DragQueryFileW(drop, i, path.data(), len + 1);
                files.push_back(win::Utf8(path));
            }
        }
    }
    CloseClipboard();
    return files;
}

Result<void> WindowsClipboard::SetFiles(const std::vector<std::string>& paths) {
    if (!OpenClipboardRetrying(nullptr)) return ClipboardError("OpenClipboard");
    if (!EmptyClipboard()) {
        CloseClipboard();
        return ClipboardError("EmptyClipboard");
    }

    // DROPFILES header + double-null-terminated UTF-16 path list.
    size_t bytes = sizeof(DROPFILES);
    std::vector<std::wstring> wide;
    wide.reserve(paths.size());
    for (const auto& p : paths) {
        std::wstring w = win::Wide(p);
        wide.push_back(w);
        bytes += (w.size() + 1) * sizeof(wchar_t);
    }
    bytes += sizeof(wchar_t);   // final terminator
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) {
        CloseClipboard();
        return ClipboardError("GlobalAlloc");
    }
    char* dst = static_cast<char*>(GlobalLock(h));
    if (!dst) {
        GlobalFree(h);
        CloseClipboard();
        return ClipboardError("GlobalLock");
    }
    DROPFILES* df = reinterpret_cast<DROPFILES*>(dst);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* cursor = reinterpret_cast<wchar_t*>(dst + sizeof(DROPFILES));
    for (const auto& w : wide) {
        std::wcscpy(cursor, w.c_str());
        cursor += w.size() + 1;
    }
    *cursor = L'\0';
    GlobalUnlock(h);
    if (SetClipboardData(CF_HDROP, h) == nullptr) {
        GlobalFree(h);
        CloseClipboard();
        return ClipboardError("SetClipboardData");
    }
    CloseClipboard();
    return Ok();
}

} // namespace bps::platform
