#pragma once

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
    struct MediaFrame {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;
        std::string fit;
        bool hasAlpha = false;
        bool empty() const { return width == 0 || height == 0 || rgba.empty(); }
    };
    void SetMediaFrame(MediaFrame frame);
    uint64_t GetMediaSeq() const;
    void SetMediaFit(std::string fit);
    void ClearMedia();
    MediaFrame GetMediaFrame() const;
    bool HasMediaFrame() const;
    bool MediaFrameIsTransparent() const;

    // ---- Bus video planes (a "bus" block on the slide) ---------------------
    struct BusFeed {
        int busId = 0;
        std::string deviceId;   // the resolved PAL device id / NDI source name, for the log
        std::string kind;       // "camera" | "screen" | "ndi"
    };
    void SetBusFeeds(std::vector<BusFeed> feeds);
    std::vector<BusFeed> GetBusFeeds() const;
    void SetBusFrame(int busId, MediaFrame frame);
    uint64_t GetBusFrameSeq(int busId) const;
    // Empty = no frame (the bus's source is not delivering): the renderer
    // clears that plane rather than freezing on the last picture it had.
    MediaFrame GetBusFrame(int busId) const;
    // ---- A TEMPLATE BUS's WINDOW PLANES --------------------------------
    static constexpr int kTemplateWindowPlaneBase = 1000000;
    static constexpr int kTemplateWindowPlaneStride = 1000;
    static constexpr int TemplateWindowPlaneKey(int busNumber, int windowIndex)
    {
        if (busNumber <= 0 || windowIndex < 0)
            return 0;
        return kTemplateWindowPlaneBase + busNumber * kTemplateWindowPlaneStride + windowIndex;
    }
    // Any bus plane with pixels — the render loop's "this frame is live"
    // question, answering without copying a single pixel buffer.
    bool HasBusFrames() const;
    void ClearBusFrames();

    // ---- Overlays on air (multiple, stacked) -------------------------------
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
    uint64_t mediaSeq_ = 0;
    std::map<int, uint64_t> busFrameSeqs_;
    std::vector<ActiveOverlay> activeOverlays_;
};
} // namespace bps::presentation