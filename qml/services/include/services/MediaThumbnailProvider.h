#pragma once

#include <QQuickImageProvider>

// Serves the media library's pictures to QML as
//
//   image://mediathumb/<size>/<step>/<percent-encoded file path>
//
// size = the width to ask for (rounded to a cache size by the engine); step = -1 for the
// still, 0..MediaLibraryService.frameSteps-1 for a video's moving frames. The pictures are
// made and cached by the engine (bps::media::ThumbnailCache); this class only loads the
// cached PNG. Load with `asynchronous: true`: Qt then calls requestImage on its image-loading
// thread, and the engine may need a moment to decode a video. A picture that cannot be made comes
// back as a 1x1 transparent image (see MediaTile: that means "show the placeholder").
class MediaThumbnailProvider : public QQuickImageProvider
{
public:
    MediaThumbnailProvider();

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
