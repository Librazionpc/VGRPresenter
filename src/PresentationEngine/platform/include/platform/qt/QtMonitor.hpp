#pragma once

// DISPLAYS THROUGH QScreen — written, NOT YET COMPOSED INTO ANY PLATFORM.
//
// This is the display half of the Qt platform services (see QtServices.hpp). It is kept in its own
// header, and out of the build, until the HDMI output work wires it in together with the output
// windows: the two have to agree on one source of truth for "which displays exist", and swapping
// that underneath the current output code separately would be a second moving part.
//
// THE THREADING RULE IT IMPLEMENTS. QScreen may only be read on the interface thread, and the
// engine asks for displays from its own threads. So nothing here reads a QScreen on the caller's
// thread: a watcher object that LIVES on the interface thread keeps a snapshot current (Qt tells it
// when a display is plugged, unplugged, resized, rotated or made primary), and Enumerate() returns a
// copy of that snapshot under a short lock. No caller ever waits on the interface thread, so a busy
// interface cannot stall the engine and the engine cannot deadlock against it.
//
// Before a QGuiApplication exists (unit tests, a headless run) there are no QScreens to watch and
// the native adapter answers instead (or nothing, without one).

#include "platform/IMonitor.hpp"

#include <memory>
#include <string>
#include <vector>

namespace bps::platform {

class QtMonitor final : public IMonitor {
public:
    explicit QtMonitor(const IMonitor* native = nullptr);
    ~QtMonitor() override;
    std::vector<MonitorInfo> Enumerate() const override;
    Result<MonitorInfo> Primary() const override;
    std::string DefaultMonitorId() const override;

    // True once the interface-thread watcher has published a snapshot (diagnostics + tests).
    bool WatchingScreens() const;

    struct State;   // shared with the watcher, which may outlive this object

private:
    void EnsureWatcher() const;

    const IMonitor* native_;
    std::shared_ptr<State> state_;
};

} // namespace bps::platform
