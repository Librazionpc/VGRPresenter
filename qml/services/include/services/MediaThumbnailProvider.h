#pragma once

#include <QQuickImageProvider>

class MediaThumbnailProvider : public QQuickImageProvider
{
public:
    MediaThumbnailProvider(QQuickImageProvider::ImageType type = QQuickImageProvider::Image,
                          QQuickImageProvider::Flags flags = QQuickImageProvider::Flags());

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};