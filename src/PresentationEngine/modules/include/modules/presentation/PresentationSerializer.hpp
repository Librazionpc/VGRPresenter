#pragma once

// IPresentationSerializer / PresentationSerializer (docs/specs/19 §Interfaces,
// docs/architecture/PresentationEngine.md file map): presentation <-> JSON.
//
// This is the DOCUMENT BODY only — the JSON that goes inside a .vgr container's
// "document" section (see PresentationDocument for the container/file side).
// Pure data conversion: no file I/O, no engine state, no runtime state (a
// presentation's PresentationState is a runtime concern and is not persisted).
//
// Schema (schemaVersion 1):
//   { "schemaVersion": 1, "id", "name", "path", "playbackMode": int,
//     "defaultTransitionMs", "createdAtMs", "modifiedAtMs",
//     "categories": [ { "id", "name", "contentType", "templateId",
//                       "outputs": [], "meta": {} } ],
//     "templates":  [ { "id", "name", "contentType", "background", "meta": {},
//                       "blocks": [ block... ] } ],
//     "overlays":   [ { "id", "name", "scope": int, "targetId", "outputs": [],
//                       "enabled", "meta": {}, "blocks": [ block... ] } ],
//     "slides": [ { "id", "title", "text", "notes", "tags": [], "sections": [],
//                   "assetIds": [], "hidden", "transitionIn": int,
//                   "transitionMs", "durationMs", "background", "categoryId",
//                   "meta": {},
//                   "blocks": [ { "id", "kind", "text", "x", "y", "width",
//                                 "height", "bind", "meta": {},
//                                 "style": { ... } } ] } ] }
// Unknown fields are ignored on read (forward compatibility); missing optional
// fields take the type's defaults. Adding a field never needs a version bump —
// only a change of meaning does.

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <string>
#include <string_view>

namespace bps::presentation {

class IPresentationSerializer {
public:
    virtual ~IPresentationSerializer() = default;
    virtual Result<std::string> Serialize(const Presentation& presentation) const = 0;
    virtual Result<Presentation> Deserialize(std::string_view json) const = 0;
};

class PresentationSerializer final : public IPresentationSerializer {
public:
    static constexpr int kSchemaVersion = 1;

    Result<std::string> Serialize(const Presentation& presentation) const override;
    // Rejects: malformed JSON, a missing/newer schemaVersion, a non-array
    // "slides", or a slide/block that is not an object. Slides or blocks with no
    // id get a generated one ("slide-N"/"block-N") — ids are stable identities,
    // so a hand-edited file missing them still loads.
    Result<Presentation> Deserialize(std::string_view json) const override;

    // A standalone slide template (the body of a .vgr Template document):
    //   { "schemaVersion": 1, "template": { id, name, contentType, background,
    //                                        meta, blocks: [ ... ] } }
    Result<std::string> SerializeTemplate(const SlideTemplate& tmpl) const;
    Result<SlideTemplate> DeserializeTemplate(std::string_view json) const;
};

} // namespace bps::presentation
