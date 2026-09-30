#pragma once

// CompositorState — the engine-side mirror of "what's currently composited
// on top of the on-air slide" (a taken camera/screen input, a playing media
// file, active overlays). These three layers are owned and decided by the
// QML/UI layer today (LiveOutputService — the real taps, the real
// QMediaPlayer decoder, the overlay pane's take/clear gestures), which is
// correct and stays that way; this class is just the thread-safe hand-off
// so the ENGINE's own renderer (SceneBuilder, running on its own worker
// thread — see LiveOutputController::Loop) can see that state too, instead
// of it being visible only to the small in-app QML preview tile the way it
// was before this existed.
//
// Camera/screen frame PIXELS are deliberately NOT pushed here — those are
// pulled directly from the PAL's own already-thread-safe tap
// (bps::platform::IVideo::PreviewFramePixels), which is cheaper than a
// second copy-through and keeps this class from needing its own frame
// buffer for that case. Only the taken input's IDENTITY (device id + kind)
// is mirrored, since the PAL has no notion of "which tap the UI currently
// intends as the output's background".
//
// Media-file frames and overlay blocks ARE copied in here: media frames
// because LiveOutputService already owns a real decoder and converts each
// frame once (a second read of the same decoded QImage costs nothing extra
// to hand off); overlay blocks because they're small, resolved-once-per-
// change QML data (OverlayLibraryService::design(id)) that the engine must
// never call back into a QML service to fetch — SceneBuilder only ever
// reads plain data here, never reaches back into the UI layer.
//
// Mutex-guarded plain data only. Every getter returns a COPY under the
// lock — same "copy under lock, use outside it" contract WindowsAudio's
// MeterTap and WindowsVideo's preview taps already use throughout this
// codebase.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "modules/presentation/PresentationTypes.hpp"

namespace bps::presentation {

class CompositorState {
public:
    static CompositorState& Instance();

    // ---- Taken input (camera/screen) ------------------------------------
    struct TakenInput {
        std::string deviceId;   // "" = none taken
        std::string kind;       // "camera" | "screen"
    };
    void SetTakenInput(std::string deviceId, std::string kind);
    void ClearTakenInput();
    TakenInput GetTakenInput() const;

    // ---- Media file on air ------------------------------------------------
    // Already-decoded pixels (LiveOutputService owns the real QMediaPlayer/
    // QVideoSink decoder) — RGBA8, row-major, row 0 at the top. Audio-only
    // media has no frame; callers just never call SetMediaFrame for it.
    struct MediaFrame {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;
        bool empty() const { return width == 0 || height == 0 || rgba.empty(); }
    };
    void SetMediaFrame(MediaFrame frame);
    void ClearMedia();
    MediaFrame GetMediaFrame() const;

    // ---- Overlays on air (multiple, stacked) -------------------------------
    // A resolved SNAPSHOT per active overlay, in take order (index 0 =
    // taken first = bottom of its group) — blocks/background already
    // pulled from OverlayLibraryService and converted via
    // ShowConverter::blockFromVariant by the caller.
    struct ActiveOverlay {
        std::string id;
        bool placeUnderSlide = false;
        std::vector<ContentBlock> blocks;
        std::string background;   // hex color or "" / "transparent"
    };
    void SetActiveOverlays(std::vector<ActiveOverlay> overlays);
    std::vector<ActiveOverlay> GetActiveOverlays() const;

private:
    CompositorState() = default;

    mutable std::mutex mutex_;
    TakenInput takenInput_;
    MediaFrame mediaFrame_;
    std::vector<ActiveOverlay> activeOverlays_;
};

} // namespace bps::presentation
