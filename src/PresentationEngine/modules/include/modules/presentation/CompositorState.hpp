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
// A "bus" block's feed joins those layers as a PUSHED frame too (the bus may be
// fed by an NDI receiver, which no PAL tap exposes — see BusFeed below), and a
// bus plane is the one compositor layer whose GEOMETRY comes from the slide
// itself: SceneBuilder paints it at the block's own rect instead of full-frame.
//
// Media-file frames, bus frames and overlay blocks ARE copied in here: media frames
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
#include <map>
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
        // THIS FILE's own fit choice ("contain" | "cover" | "fill", empty =
        // none), which WINS over the output's default when set — the operator
        // saying "this picture, on every output, like this". It travels with
        // the frame because the frame is what is on air; the per-output default
        // stays in OutputStyleSpec::mediaFit (Settings · Outputs → Fill).
        std::string fit;
        // THE PICTURE'S OWN TRANSPARENCY — does it carry a pixel with alpha < 255?
        // It travels with the frame for the same reason `fit` does: the take is
        // what is on air, and this is a fact about that take. The scan happens
        // ONCE, where the file was decoded (LiveOutputService::preflightMediaTake
        // for a still; the first decoded frame for a video), never per frame.
        //
        // The composition asks THIS rather than guessing from a file extension
        // (SceneBuilder::SlideComposition::frameTransparent), which is what keeps
        // the monitor tile's checkerboard and the real outputs answering the same
        // question about the same pixels.
        bool hasAlpha = false;
        bool empty() const { return width == 0 || height == 0 || rgba.empty(); }
    };
    void SetMediaFrame(MediaFrame frame);
    // HOW MANY FRAMES HAVE BEEN PUSHED — bumped once per SetMediaFrame, so a
    // consumer that runs at a DIFFERENT rate than the decoder (the composition
    // builds up to once per output per tick, a 30 fps file pushes half as
    // often) can tell "nothing new arrived" without copying the pixel buffer to
    // find out. See SceneBuilder::LayerFrameUnchanged — that copy was 3 x 8 MB
    // per build per output for a picture that had not changed.
    uint64_t GetMediaSeq() const;
    // Changes ONLY the fit of the frame already on air: the operator picking
    // Contain/Cover/Fill while the picture is up must not cost a re-push of its
    // pixels (the decoder will push the next frame regardless). A no-op when
    // nothing is on air — there is no frame whose layout could change.
    void SetMediaFit(std::string fit);
    void ClearMedia();
    MediaFrame GetMediaFrame() const;
    // Same truth as !GetMediaFrame().empty(), without copying the pixel
    // buffer — for callers that only need to know whether media is on air
    // (e.g. RenderOnce's per-tick liveness check), not read it.
    bool HasMediaFrame() const;
    // Same truth as GetMediaFrame().hasAlpha, without copying the pixel buffer —
    // the question SceneBuilder::ComposeSlide asks on every composition
    // (frameTransparent) and the one every preview surface reads through it.
    bool MediaFrameIsTransparent() const;

    // ---- Bus video planes (a "bus" block on the slide) ---------------------
    // A design block of kind "bus" is a WINDOW onto one of the mix buses: the
    // canvas draws the bus's live feed inside it, so every real output has to
    // draw that same feed or NDI/HDMI show an empty hole where the operator
    // sees video. The UI decides WHICH feed (the bus's first routed video
    // source) and holds its tap; the frames arrive here as already-decoded
    // pixels because a bus can be fed by a camera, a screen OR an NDI source,
    // and only the first two live in a PAL tap SceneBuilder could read itself
    // (IVideo::PreviewFramePixels) — NDI frames come through the engine's own
    // receiver on the UI side. One push path, so a bus renders the same
    // whatever is routed into it.
    //
    // Keyed by the bus's STABLE number (the block's meta busId), never by a
    // roster row: rows move, ids do not.
    struct BusFeed {
        int busId = 0;
        std::string deviceId;   // the resolved PAL device id / NDI source name, for the log
        std::string kind;       // "camera" | "screen" | "ndi"
    };
    // The feeds the on-air composition needs, replaced wholesale on every
    // change (that is a diff the caller already has: the slide's bus blocks).
    // Frames of a bus that is no longer listed are dropped with it — a stale
    // picture in a hole nobody asked for is worse than no picture.
    void SetBusFeeds(std::vector<BusFeed> feeds);
    std::vector<BusFeed> GetBusFeeds() const;
    void SetBusFrame(int busId, MediaFrame frame);
    // The same "nothing new arrived" counter, per bus (see GetMediaSeq). 0 means
    // this bus has never had a frame pushed; a bus that was dropped from the
    // feed list starts from 0 again, so the plane that names it cannot mistake
    // its old picture for a current one.
    uint64_t GetBusFrameSeq(int busId) const;
    // Empty = no frame (the bus's source is not delivering): the renderer
    // clears that plane rather than freezing on the last picture it had.
    MediaFrame GetBusFrame(int busId) const;
    // Any bus plane with pixels — the render loop's "this frame is live"
    // question, answering without copying a single pixel buffer.
    bool HasBusFrames() const;
    void ClearBusFrames();

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
    // Same truth as !GetActiveOverlays().empty(), without copying every
    // overlay's resolved block list — see HasMediaFrame() above.
    bool HasActiveOverlays() const;

private:
    CompositorState() = default;

    mutable std::mutex mutex_;
    TakenInput takenInput_;
    MediaFrame mediaFrame_;
    std::vector<BusFeed> busFeeds_;
    std::map<int, MediaFrame> busFrames_;
    // Push counters (see GetMediaSeq / GetBusFrameSeq): bumped under the same
    // lock as the frame they describe, so a reader that takes a seq and then a
    // frame cannot pair an old picture with a new number.
    uint64_t mediaSeq_ = 0;
    std::map<int, uint64_t> busFrameSeqs_;
    std::vector<ActiveOverlay> activeOverlays_;
};

} // namespace bps::presentation
