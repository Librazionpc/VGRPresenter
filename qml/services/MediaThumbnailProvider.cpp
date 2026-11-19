#include "services/MediaThumbnailProvider.h"

#include "services/MediaLibraryService.h"

#include <QUrl>

namespace {
QImage noPicture(QSize *size)
{
    QImage blank(1, 1, QImage::Format_ARGB32);
    blank.fill(Qt::transparent);
    if (size)
        *size = blank.size();
    return blank;
}
} // namespace

MediaThumbnailProvider::MediaThumbnailProvider(QQuickImageProvider::ImageType type,
                                              QQuickImageProvider::Flags flags)
    : QQuickImageProvider(type, flags)
{
}

QImage MediaThumbnailProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    Q_UNUSED(requestedSize)

    // "<size>/<step>/<percent-encoded path>"
    const int firstCut = id.indexOf(QLatin1Char('/'));
    const int secondCut = firstCut < 0 ? -1 : id.indexOf(QLatin1Char('/'), firstCut + 1);
    if (secondCut < 0)
        return noPicture(size);
    bool sizeOk = false, stepOk = false;
    const int pictureSize = id.left(firstCut).toInt(&sizeOk);
    const int step = id.mid(firstCut + 1, secondCut - firstCut - 1).toInt(&stepOk);
    if (!sizeOk || !stepOk)
        return noPicture(size);
    const QString path = QUrl::fromPercentEncoding(id.mid(secondCut + 1).toUtf8());

    // The ENGINE makes (or finds) the picture; this only loads the cached PNG it points at.
    const QString file = MediaLibraryService::instance().thumbnailFile(path, pictureSize, step);
    if (file.isEmpty())
        return noPicture(size);
    QImage image(file);
    if (image.isNull())
        return noPicture(size);
    if (size)
        *size = image.size();
    return image;
}