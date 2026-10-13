#pragma once

// Render Outputs (docs/specs/17 §Render Outputs). The Render Engine generates
// frames; each IRenderOutput decides where they go. One scene can feed audience,
// stage, preview, thumbnail, stream and screenshot outputs simultaneously with
// zero duplicated rendering logic. This is the seam Phase 6 (Display Engine)
// uses to route to monitors / NDI / OBS.

#include "core/common/Common.hpp"
#include "modules/rendering/IGraphicsBackend.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::rendering {

enum class OutputKind : int {
    Audience = 0,
    Stage,
    Preview,
    Thumbnail,
    Stream,
    Screenshot,
};

inline const char* ToString(OutputKind k) {
    switch (k) {
        case OutputKind::Audience:   return "Audience";
        case OutputKind::Stage:      return "Stage";
        case OutputKind::Preview:    return "Preview";
        case OutputKind::Thumbnail:  return "Thumbnail";
        case OutputKind::Stream:     return "Stream";
        case OutputKind::Screenshot: return "Screenshot";
    }
    return "Unknown";
}

// A frame handed to outputs (CPU-side RGBA8 — the universal frame format).
struct Frame {
    std::string sceneId;
    uint64_t frame = 0;
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;    // Color::Pack
    double timestampMs = 0.0;

    bool empty() const { return pixels.empty(); }
};

class IRenderOutput {
public:
    virtual ~IRenderOutput() = default;
    virtual OutputKind Kind() const noexcept = 0;
    virtual const char* Name() const noexcept = 0;

    // Consume a frame (scaled to the output's target size by the engine).
    virtual Result<void> Present(const Frame& frame) = 0;
    virtual Frame LastFrame() const = 0;
    virtual uint64_t FramesReceived() const = 0;
    virtual Size TargetSize() const = 0;
    virtual void SetTargetSize(Size s) = 0;
    virtual bool Enabled() const = 0;
    virtual void SetEnabled(bool enabled) = 0;

    // May the main pass's distribute step write this output? A PER-OUTPUT-PASS
    // output (one the live loop rasterizes its own styled scene into — see
    // LiveOutputController::RenderPerOutputPasses) says NO: two writers on one
    // buffer means the NDI/HDMI consumer samples whichever of the two landed
    // last, i.e. the feed visibly alternating between the program scene and
    // the output's own scene. Every other output (preview, screenshot, real
    // display) keeps the default.
    virtual bool AcceptsDistributedFrames() const { return true; }
};

// --- Concrete outputs --------------------------------------------------------
class FrameBufferOutput : public IRenderOutput {
public:
    explicit FrameBufferOutput(OutputKind kind, std::string name, Size target)
        : kind_(kind), name_(std::move(name)), target_(target) {}

    OutputKind Kind() const noexcept override { return kind_; }
    const char* Name() const noexcept override { return name_.c_str(); }

    Result<void> Present(const Frame& frame) override;
    Frame LastFrame() const override;
    // Retain the immutable current frame without copying its pixel vector.
    // Live consumers use this to hand frames to worker threads cheaply.
    std::shared_ptr<const Frame> LastFrameSnapshot() const;
    uint64_t FramesReceived() const override { return received_.load(); }
    Size TargetSize() const override { return target_; }
    void SetTargetSize(Size s) override { target_ = s; }
    bool Enabled() const override { return enabled_.load(); }
    void SetEnabled(bool enabled) override { enabled_.store(enabled); }
    // See IRenderOutput::AcceptsDistributedFrames — a per-output pass turns
    // this OFF for its own buffer (and back ON if it cannot build its scene,
    // so the output falls back to the program feed instead of going stale).
    bool AcceptsDistributedFrames() const override { return acceptsDistributed_.load(); }
    void SetAcceptsDistributedFrames(bool accepts) { acceptsDistributed_.store(accepts); }

protected:
    mutable std::mutex mutex_;
    OutputKind kind_;
    std::string name_;
    Size target_;
    std::shared_ptr<const Frame> lastFrame_;
    std::atomic<uint64_t> received_{0};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> acceptsDistributed_{true};
};

// Screenshot output: captures a frame and exposes it for saving (PAL file I/O).
class ScreenshotOutput final : public FrameBufferOutput {
public:
    ScreenshotOutput(std::string name, Size target)
        : FrameBufferOutput(OutputKind::Screenshot, std::move(name), target) {}
    // Saves the last frame as PPM (simple, dependency-free) via the PAL.
    Result<void> SavePpm(std::string_view path) const;
};

// --- Output manager ----------------------------------------------------------
class OutputManager {
public:
    Result<void> Add(std::shared_ptr<IRenderOutput> output);
    Result<std::shared_ptr<IRenderOutput>> Get(std::string_view name) const;
    Result<void> Remove(std::string_view name);
    std::vector<std::shared_ptr<IRenderOutput>> All() const;
    size_t Count() const { return outputs_.size(); }

    // Distributes a frame to all enabled outputs (scaled to their targets).
    void Distribute(const Frame& frame);
    void Clear();

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IRenderOutput>> outputs_;
};

} // namespace bps::rendering
