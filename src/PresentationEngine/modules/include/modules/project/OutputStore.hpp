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
    bool active = false;
    bool enabled = true;
    std::string styleId;                       // "" = no style
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
