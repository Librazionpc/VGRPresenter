// QtClipboard — see QtClipboard.hpp.

#include "platform/qt/QtClipboard.hpp"

#include "platform/qt/QtGuiThread.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <cstring>

namespace bps::platform {
namespace {

QClipboard* Board() {
    return qobject_cast<QGuiApplication*>(QCoreApplication::instance()) ? QGuiApplication::clipboard() : nullptr;
}

Error NoClipboard() {
    return Error::Make(Err::Unsupported, "Clipboard", "no GUI application is running to own the clipboard");
}

} // namespace

Result<std::string> QtClipboard::ReadText() const {
    return OnGuiThread([]() -> Result<std::string> {
        QClipboard* board = Board();
        if (!board) return NoClipboard();
        const QMimeData* data = board->mimeData();
        if (!data || !data->hasText()) return Error::Make(Err::NotFound, "Clipboard", "clipboard has no text");
        return board->text().toStdString();
    });
}

Result<void> QtClipboard::WriteText(std::string_view text) {
    return OnGuiThread([&]() -> Result<void> {
        QClipboard* board = Board();
        if (!board) return NoClipboard();
        board->setText(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
        return Ok();
    });
}

Result<std::vector<std::string>> QtClipboard::GetFiles() const {
    return OnGuiThread([]() -> Result<std::vector<std::string>> {
        QClipboard* board = Board();
        if (!board) return NoClipboard();
        const QMimeData* data = board->mimeData();
        std::vector<std::string> paths;
        if (data && data->hasUrls())
            for (const QUrl& url : data->urls())
                if (url.isLocalFile()) paths.push_back(url.toLocalFile().toStdString());   // paths, not file:// URIs
        if (paths.empty()) return Error::Make(Err::NotFound, "Clipboard", "clipboard has no file list");
        return paths;
    });
}

Result<void> QtClipboard::SetFiles(const std::vector<std::string>& paths) {
    return OnGuiThread([&]() -> Result<void> {
        QClipboard* board = Board();
        if (!board) return NoClipboard();
        QList<QUrl> urls;
        for (const std::string& p : paths) urls << QUrl::fromLocalFile(QString::fromStdString(p));
        auto* data = new QMimeData;
        data->setUrls(urls);
        board->setMimeData(data);   // the clipboard owns it from here
        return Ok();
    });
}

Result<ClipboardImage> QtClipboard::ReadImage() const {
    return OnGuiThread([]() -> Result<ClipboardImage> {
        QClipboard* board = Board();
        if (!board) return NoClipboard();
        const QMimeData* data = board->mimeData();
        if (!data || !data->hasImage()) return Error::Make(Err::NotFound, "Clipboard", "clipboard has no image");
        QImage image = board->image();
        if (image.isNull()) return Error::Make(Err::InvalidArgument, "Clipboard", "image data could not be decoded");
        image = image.convertToFormat(QImage::Format_RGBA8888);
        ClipboardImage out;
        out.width = image.width();
        out.height = image.height();
        const size_t rowBytes = static_cast<size_t>(out.width) * 4;
        out.rgba.resize(rowBytes * static_cast<size_t>(out.height));
        for (int y = 0; y < out.height; ++y)   // bytesPerLine can be padded: copy row by row
            std::copy_n(image.constScanLine(y), rowBytes, out.rgba.data() + rowBytes * static_cast<size_t>(y));
        return out;
    });
}

Result<void> QtClipboard::WriteImage(const ClipboardImage& image) {
    if (image.empty() || image.rgba.size() < static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4)
        return Error::Make(Err::InvalidArgument, "Clipboard", "invalid RGBA image");
    return OnGuiThread([&]() -> Result<void> {
        QClipboard* board = Board();
        if (!board) return NoClipboard();
        // copy(): the QImage must not reference the caller's buffer once this returns.
        const QImage view(image.rgba.data(), image.width, image.height, image.width * 4, QImage::Format_RGBA8888);
        board->setImage(view.copy());
        return Ok();
    });
}

} // namespace bps::platform
