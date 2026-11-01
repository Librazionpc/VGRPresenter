#pragma once

// OutputStore — the OUTPUT ROSTER in the kernel's DatabaseManager document
// store (the sibling of StyleStore, which keeps the styles roster). An
// output row is everything OutputListModel's UI state carries that must
// survive a restart:
//
//   { "id": "out-1", "name": "Main Output", "badge": "LIVE 1", "kind": "HDMI",
//     "res": "1536×960", "refresh": "60 Hz", "testPattern": "none",
//     "screenName": "\\\\.\\DISPLAY1", "boundsLocked": false, "active": true,
//     "enabled": true, "styleId": "s1", "content": {...} }
//
// The style ASSIGNMENT (styleId) is the reason this store exists: without it
// the output roster rebuilt itself fresh every launch ("Main Output", no
// style) and the engine's boot-time style push carried an empty spec — the
// saved styles were on disk, but nothing pointed at one.

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"

#include <array>
#include <string>
#include <vector>

namespace bps::project {

struct StoredOutput {
    std::string id;
    std::string name;
    std::string badge;
    std::string kind = "HDMI";
    std::string res;
    std::string refresh;
    std::string testPattern = "none";
    std::string screenName;
    bool boundsLocked = false;
    // Whether this output's REAL destination window (a physical/HDMI
    // screen, not the small in-app preview tile) stays above other
    // windows. Off by default — an always-on-top borderless window with
    // no dismiss affordance was found to bury the app itself when testing
    // without a dedicated second monitor.
    bool stayOnTop = false;
    // Whether the real destination window covers its whole bound screen
    // (a real projector/HDMI output's actual job) or starts at half-size,
    // centered and movable (safe for testing on a single dev monitor,
    // where a fullscreen always-on-top window would bury the app itself).
    // On by default — the half-size start is the exception, not the norm.
    bool fullscreenOutput = true;
    bool active = false;
    bool enabled = true;
    std::string styleId;                       // "" = no style
    // "direct" shows this output's routed video bus full-frame; "compose"
    // renders the design and its independently assigned bus windows.
    std::string videoMode = "direct";
    // THIS OUTPUT's own content gates. They are PER OUTPUT, not per style: two
    // screens wearing one theme may disagree about whether media or the overlay
    // layer paints, and allowing one must not allow the other. The style's own
    // gates still apply ON TOP (both must allow), so a theme that forbids media
    // cannot be opened by a row.
    //
    // THEY ARE WIRES NOW (Settings · Routing): the Overlays and Media cards at the
    // head of the Styles column each carry an output dot, and a wire from one of
    // them to an output IS the permission — exactly as a bus wire is. No wire,
    // nothing paints. So the stored default is "not allowed": the flag on the row
    // is that wire, and setting it is what draws the line.
    bool showOverlay = false;
    bool showMedia = false;
    // How a taken media file sits on THIS output — "contain" (default, whole
    // picture, own aspect kept), "cover" (cropped to fill) or "fill"
    // (stretched). Settings · Outputs' Fill picker; empty = default.
    std::string mediaFit;
    // Which OUTPUT endpoint this row's audio plays on, as the audio roster's own
    // id ("waveout:<n>"). Empty = the program: this row's mix is summed into the
    // engine's single playout, exactly as before. Set = the row's audio leaves on
    // that device's own stream, which is what makes an HDMI output's sound come
    // out of that monitor instead of the booth speakers. Settings · Outputs picks
    // it; RoutingService opens and closes the stream from the wires.
    std::string audioDevice;
    // A REC / STREAM row's preset (Settings · Recording): the stable id of the
    // recording or streaming PRESET this output records / streams with. Empty =
    // none picked yet (the row exists but has nothing to record with).
    std::string presetId;
    // On-air-only transport: this output carries ONLY its own feed (NDI etc.)
    // — no physical-screen window, no slot in the screen pipeline. The QML
    // model's transport mutual-exclusion switch (OutputListModel's
    // isScreenTransport: !onAirOnly && kind=="HDMI"); forced true on hydrate
    // for legacy NDI rows persisted before this field existed.
    bool onAirOnly = false;
    // The Edit dialog's six item-kind toggles, in display order:
    // text, camera, media, clock, timer, shape.
    std::array<bool, 6> contentToggles{};
    std::string category;
};

class OutputStore {
public:
    static OutputStore& Instance();

    std::vector<StoredOutput> Get() const;
    Result<void> Save(const std::vector<StoredOutput>& outputs);

    static json::Value OutputToJson(const StoredOutput& o);
    static Result<StoredOutput> OutputFromJson(const json::Value& v);
};

} // namespace bps::project
