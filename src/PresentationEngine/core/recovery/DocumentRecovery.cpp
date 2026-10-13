#include "core/recovery/DocumentRecovery.hpp"

#include "core/logging/Logger.hpp"
#include "platform/PlatformAccessor.hpp"

#include <ctime>
#include <format>

namespace bps::recovery {

namespace {

// `<path>.corrupt-<unix-seconds>`, with `-N` appended only if that exact name is
// already taken (two failures inside one second, or an operator's own file).
std::string QuarantineName(const platform::IFilesystem& fs, const std::string& path) {
    const std::string base = std::format(
        "{}.corrupt-{}", path, static_cast<long long>(std::time(nullptr)));
    std::string candidate = base;
    for (int n = 2; n < 1000 && fs.Exists(candidate); ++n)
        candidate = std::format("{}-{}", base, n);
    return candidate;
}

const char* TypeName(json::Value::Type t) {
    switch (t) {
    case json::Value::Type::Null: return "null";
    case json::Value::Type::Bool: return "a boolean";
    case json::Value::Type::Number: return "a number";
    case json::Value::Type::String: return "a string";
    case json::Value::Type::Array: return "an array";
    case json::Value::Type::Object: return "an object";
    }
    return "a value";
}

} // namespace

std::string DocumentShapeProblem(const json::Value& root, std::string_view requiredKey,
                                 json::Value::Type requiredType) {
    if (root.type() != json::Value::Type::Object)
        return std::format("the document is {} where an object was expected", TypeName(root.type()));
    const json::Value* found = root.Find(requiredKey);
    if (!found)
        return std::format("the document has no '{}' section", requiredKey);
    if (found->type() != requiredType)
        return std::format("the '{}' section is {} where {} was expected", requiredKey,
                           TypeName(found->type()), TypeName(requiredType));
    return {};
}

DamageReport SetAsideDamagedDocument(std::string_view path, std::string_view module,
                                     std::string_view reason) {
    DamageReport report;
    report.path = std::string(path);
    report.reason = std::string(reason);
    if (path.empty())
        return report;   // no file configured: nothing was read, nothing is damage

    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (!fs.IsRegularFile(path)) {
        // First run, or the file is already gone. Nothing to preserve, and the
        // caller rebuilds from defaults exactly as it would on a fresh install.
        Logger::Instance().Info(
            std::format("{}: {} was not readable as a document — rebuilding from defaults", module,
                        report.path),
            std::string(module));
        return report;
    }

    report.quarantine = QuarantineName(fs, report.path);
    // Rename first: it is the one operation that both preserves the bytes and
    // frees the original path in a single step, and it keeps the evidence on the
    // same volume as the document.
    if (auto moved = fs.RenameRetry(report.path, report.quarantine); moved.ok()) {
        report.preserved = true;
    } else if (auto copied = fs.CopyAtomic(report.path, report.quarantine); copied.ok()) {
        // The rename was refused (a lock or a sync filter holds the file): keep
        // the copy as the evidence, then free the path for the rebuild. A failed
        // Remove still leaves the copy, so the bytes are never lost here.
        report.preserved = true;
        (void)fs.Remove(report.path);
    }

    if (report.preserved) {
        Logger::Instance().Warning(
            std::format("{}: damaged {} set aside as {} ({}) — rebuilt from defaults", module,
                        report.path, report.quarantine, report.reason),
            std::string(module));
    } else {
        Logger::Instance().Warning(
            std::format("{}: damaged {} could not be set aside ({}) — rebuilding from defaults "
                        "will overwrite it",
                        module, report.path, report.reason),
            std::string(module));
    }
    return report;
}

} // namespace bps::recovery
