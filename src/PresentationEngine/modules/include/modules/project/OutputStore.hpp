#pragma once

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
    bool stayOnTop = false;
    bool fullscreenOutput = true;
    bool active = false;
    bool enabled = true;
    std::string styleId;                       // "" = no style
    // "direct" shows this output's routed video bus full-frame; "compose"
    // renders the design and its independently assigned bus windows.
    std::string videoMode = "direct";
    bool showOverlay = false;
    bool showMedia = false;
    std::string mediaFit;
    std::string audioDevice;
    std::string presetId;
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