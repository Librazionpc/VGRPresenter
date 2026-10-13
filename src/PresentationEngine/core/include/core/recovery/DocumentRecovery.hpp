#pragma once

// Self-healing loads for the engine's INTERNAL documents.
//
// Every internal document — settings.json, the project registry, the design and
// media libraries, the bible store, the engine config, search/cache indexes — is
// read back on the next launch. A damaged one used to be a dead end: the loader
// returned Config_ParseFailed/ParseError, the boot step failed (or the screen
// showed an error), and the user was left with a file they could neither read
// nor get past. The bytes were then at risk twice over: nothing preserved them
// as evidence, and the next save overwrote the very thing worth looking at.
//
// The settings database already recovers this way — DatabaseManager::Open()
// moves a corrupt kernel.json to `<path>.corrupt-<stamp>`, adopts the original
// path and starts empty, so persistence keeps working and the operator's bytes
// survive under a name that says what they are. This is that same policy, once,
// for every other document.
//
// The contract a caller follows:
//   * on a parse/validation failure, call SetAsideDamagedDocument(...);
//   * then return Ok with defaults/empty state — the read NEVER fails a boot or
//     a screen because one file was damaged;
//   * keep saving to the SAME path, so the document is rebuilt by the next save.
//
// Only INTERNAL documents belong here. A user's own authored file (an exported
// package, a render output) is not rebuilt behind their back; those callers
// decide for themselves, and may still use the set-aside half.

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"

#include <string>
#include <string_view>

namespace bps::recovery {

// What happened to one damaged document.
struct DamageReport {
    std::string path;         // the document that was damaged
    std::string quarantine;   // where its bytes were set aside ("" = none)
    std::string reason;       // the failure that condemned it (parse error text)
    bool preserved = false;   // the original bytes are on disk under `quarantine`

    // True when there was nothing on disk to preserve — a first run, or a file
    // that vanished — in which case the caller is simply rebuilding from
    // defaults and no evidence was lost.
    bool nothingToPreserve() const { return quarantine.empty(); }
};

// Why a parsed document cannot be read, or "" when it can.
//
// A file that parses is not automatically OUR document. Every internal document
// is written by this engine with a known top-level key (`projects`, `designs`,
// `settings`, `folders`, `books`) — a file that parses but lacks it holds
// nothing this engine can load, and reading it as "empty" then saving over it
// destroys whatever the operator actually had. That is the same loss as a parse
// failure, so it is condemned and set aside the same way, with a reason naming
// the missing key instead of a parse offset.
std::string DocumentShapeProblem(const json::Value& root, std::string_view requiredKey,
                                 json::Value::Type requiredType);

// Move a damaged document aside as evidence and report where it went.
//
// The file is renamed to `<path>.corrupt-<unix-seconds>` (with a `-N` suffix if
// that name is somehow taken). If the rename cannot be done — a lock/sync filter
// holding it — the bytes are COPIED to the quarantine name first and the
// original is then removed; a failed copy leaves everything in place and the
// report says the bytes could not be preserved, so the loss is stated rather
// than hidden. Never deletes a document without first trying to keep the bytes.
//
// ALWAYS logs (the log is the record the operator reads) and never throws.
DamageReport SetAsideDamagedDocument(std::string_view path, std::string_view module,
                                     std::string_view reason);

} // namespace bps::recovery
