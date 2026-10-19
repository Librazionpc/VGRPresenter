#pragma once

// ShippedConfig — the build's OWN configuration reaching an install that already
// exists.
//
// <install>/assets/kernel.shipped.json (written at package time by
// tools/make_seed_config.mjs) holds the styles, outputs and profiles this build
// ships — what a fresh install starts with. The installer only copies seed files
// that do not exist yet, which is right for user data but wrong for the app's own
// look: an install that had ever run kept its rosters forever, so a fix to a
// shipped style or output reached nobody who was already past first boot.
//
// This applies that file to the store ONCE per shipped FINGERPRINT. The rule:
//
//   * an entry whose id the build ships is THE SHIPPED ENTRY: it is refreshed in
//     place (or appended, when the id is new to the install). That is how a fixed
//     style or a new output arrives without anyone reconfiguring.
//   * an entry with an id the build does not ship is the USER'S OWN: untouched,
//     in its own place, never reordered.
//   * nothing is ever removed. A shipped entry the operator deleted by hand comes
//     back only if the build ships it again with a different fingerprint.
//
// The fingerprint is derived from the file's CONTENT (FNV-1a over the canonical
// JSON), so a build that ships different styles has a different fingerprint
// without anyone remembering to bump a number — the lesson the design catalogs
// learned when a hand-kept revision drifted from the content. The applied
// fingerprint is kept in the store as `config.shipped`.

#include "core/config/Json.hpp"

#include <cstddef>
#include <string>

namespace bps::project {

struct ShippedMergeReport {
    bool applied = false;        // false: no shipped file here, or it is already in
    std::string fingerprint;     // "" when nothing was applied
    size_t added = 0;            // shipped entries this install did not have
    size_t refreshed = 0;        // shipped entries whose definition changed
};

// The build's shipped configuration beside the exe (assets/kernel.shipped.json).
// Applied at most once per process and at most once per fingerprint; safe to call
// from any thread (the interface thread reads the rosters first).
ShippedMergeReport ApplyShippedConfiguration();

// The same merge against an explicit file — the seam the tests use.
ShippedMergeReport ApplyShippedConfigurationFrom(const std::string& filePath);

// The rule for ONE list of entries, exposed so a test can drive it without a
// store: shipped entries are upserted by id (in place), every other entry keeps
// its position, and entries with no id cannot be matched so they are left alone.
// A non-array `shipped` means "nothing to merge".
json::Value MergeShippedEntries(const json::Value& stored, const json::Value& shipped,
                                size_t& added, size_t& refreshed);

} // namespace bps::project
