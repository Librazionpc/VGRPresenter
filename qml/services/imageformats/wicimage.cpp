#include <QByteArray>
#include <QImage>
#include <QImageIOHandler>
#include <QImageIOPlugin>
#include <QIODevice>

#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincodec.h>
#endif

namespace {
// RIFF<size>WEBP — the container's own magic, so a JPEG renamed to .webp is not
// claimed by this plugin (Qt's own handler answers it instead).
bool looksLikeWebp(QIODevice *device)
{
    if (!device || !device->isReadable())
        return false;
    const QByteArray head = device->peek(12);
    return head.size() >= 12 && head.startsWith("RIFF") && head.mid(8, 4) == "WEBP";
}

#ifdef Q_OS_WIN

// A minimal COM ref: the plugin must not pull in a COM wrapper header, and this
// three-line scope is all it needs.
template <typename T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;

    T **put()
    {
        reset();
        return &p_;
    }
    T *get() const { return p_; }
    T *operator->() const { return p_; }
    void reset()
    {
        if (p_)
            p_->Release();
        p_ = nullptr;
    }

private:
    T *p_ = nullptr;
};

class ComScope {
public:
    ComScope() { uninit_ = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComScope()
    {
        if (uninit_)
            ::CoUninitialize();
    }
    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;

private:
    bool uninit_ = false;
};

// WIC sniffs the container itself, so this is a real "can I decode this?" and
// not an extension check.
QImage decodeWic(QIODevice *device)
{
    if (!device || !device->isReadable())
        return {};
    const QByteArray bytes = device->readAll();
    if (bytes.isEmpty())
        return {};

    ComScope com;
    Com<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.put()))))
        return {};

    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(bytes.size()));
    if (!memory)
        return {};
    void *target = ::GlobalLock(memory);
    if (!target) {
        ::GlobalFree(memory);
        return {};
    }
    std::memcpy(target, bytes.constData(), static_cast<size_t>(bytes.size()));
    ::GlobalUnlock(memory);
    Com<IStream> stream;
    if (FAILED(::CreateStreamOnHGlobal(memory, TRUE, stream.put()))) {
        ::GlobalFree(memory);
        return {};
    }

    Com<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                               decoder.put())))
        return {};
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put())))
        return {};
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || !width || !height)
        return {};

    Com<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return {};

    QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32);
    if (image.isNull())
        return {};
    if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(image.bytesPerLine()),
                                     static_cast<UINT>(image.sizeInBytes()), image.bits())))
        return {};
    return image;
}

#endif // Q_OS_WIN

// The handler Qt asks for a .webp. canRead() is the cheap sniff; read() is the
// one decode, on whatever thread Qt called us from.
class WicImageHandler : public QImageIOHandler
{
public:
    explicit WicImageHandler(QIODevice *device) : device_(device) {}

    bool canRead() const override { return looksLikeWebp(device_); }

    bool read(QImage *image) override
    {
        if (!device_ || !device_->isReadable() || !device_->seek(0))
            return false;
#ifdef Q_OS_WIN
        const QImage decoded = decodeWic(device_);
#else
        const QImage decoded;
#endif
        if (decoded.isNull())
            return false;
        *image = decoded;
        return true;
    }

private:
    QIODevice *device_ = nullptr;
};
} // namespace

class WicImagePlugin : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QImageIOHandlerFactoryInterface_iid FILE "wicimage.json")

public:
    Capabilities capabilities(QIODevice *device, const QByteArray &format) const override
    {
        if (format.isEmpty())
            return looksLikeWebp(device) ? CanRead : Capabilities();
        return format.compare("webp", Qt::CaseInsensitive) == 0 ? CanRead : Capabilities();
    }

    QImageIOHandler *create(QIODevice *device, const QByteArray &format = QByteArray()) const override
    {
        // The format argument can be empty (Qt probes by content), so the
        // handler answers canRead() from the bytes rather than trusting it.
        if (!format.isEmpty() && format.compare("webp", Qt::CaseInsensitive) != 0)
            return nullptr;
        auto *handler = new WicImageHandler(device);
        if (!handler->canRead()) {
            delete handler;
            return nullptr;
        }
        return handler;
    }
};

#include "wicimage.moc"