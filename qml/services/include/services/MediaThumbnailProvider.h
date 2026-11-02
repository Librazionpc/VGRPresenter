#pragma once

#include <QQuickImageProvider>

class MediaThumbnailProvider : public QQuickImageProvider
{
public:
    MediaThumbnailProvider();

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};