#include "modules/project/ShippedConfig.hpp"

#include "core/database/DatabaseManager.hpp"
#include "core/logging/Logger.hpp"
#include "platform/PlatformAccessor.hpp"

#include <cstdint>
#include <format>
#include <mutex>
#include <string>

namespace bps::project {
namespace {
constexpr const char* kModule = "ShippedConfig";
constexpr const char* kBaselineName = "kernel.shipped.json";
// Where the applied fingerprint lives in the store. One document, one key: the
// roster collections stay exactly as the app has always written them.
constexpr const char* kStateCollection = "config";
constexpr const char* kStateKey = "shipped";

std::string ContentFingerprint(const json::Value& collections) {
    const std::string text = collections.ToString();
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return std::format("{:016x}", hash);
}

std::string EntryId(const json::Value& entry) {
    if (const json::Value* id = entry.Find("id"))
        return std::string(id->asString());
    return {};
}
} // namespace

json::Value MergeShippedEntries(const json::Value& stored, const json::Value& shipped,
                                size_t& added, size_t& refreshed) {
    const json::Value::Array* shippedEntries = shipped.asArray();
    if (!shippedEntries) return stored;

    json::Value::Array merged;
    if (const json::Value::Array* storedEntries = stored.asArray())
        merged = *storedEntries;   // the user's entries keep their place, in order

    for (const json::Value& entry : *shippedEntries) {
        const std::string id = EntryId(entry);
        // An entry with no id cannot be matched to anything: adopting it would
        // duplicate it on every merge, so it is not ours to merge.
        if (id.empty()) continue;

        bool found = false;
        for (json::Value& existing : merged) {
            if (EntryId(existing) != id) continue;
            found = true;
            // Same definition as the build ships: nothing to do (and nothing to
            // report — a refresh count that moves without a change is noise).
            if (existing.ToString() != entry.ToString()) {
                existing = entry;   // THE SHIPPED ENTRY, as this build defines it
                ++refreshed;
            }
            break;
        }
        if (!found) {
            merged.push_back(entry);   // new to this install
            ++added;
        }
    }
    return json::Value(std::move(merged));
}

ShippedMergeReport ApplyShippedConfigurationFrom(const std::string& filePath) {
    ShippedMergeReport report;
    auto& platform = platform::PlatformAccessor::Get();
    if (filePath.empty() || !platform.Filesystem().Exists(filePath))
        return report;   // a dev build has no shipped file: nothing to do

    auto text = platform.Filesystem().ReadText(filePath);
    if (!text.ok()) {
        Logger::Instance().Warning(
            std::format("cannot read the shipped configuration: {}", text.error().message), kModule);
        return report;
    }
    auto parsed = json::Parse(text.value());
    if (!parsed.ok()) {
        Logger::Instance().Warning(
            std::format("shipped configuration is not valid JSON: {}", parsed.error().message), kModule);
        return report;
    }

    const json::Value* collections = parsed.value().Find("collections");
    if (collections == nullptr || collections->asObject() == nullptr) {
        Logger::Instance().Warning("shipped configuration has no collections object", kModule);
        return report;
    }

    const std::string fingerprint = ContentFingerprint(*collections);
    auto& db = DatabaseManager::Instance();
    if (auto state = db.Get(kStateCollection, kStateKey); state.ok()) {
        if (const json::Value* applied = state.value().Find("fingerprint");
            applied != nullptr && std::string(applied->asString()) == fingerprint) {
            return report;   // this build's configuration is already in
        }
    }

    for (const auto& [collection, shippedDocument] : *collections->asObject()) {
        const json::Value::Object* members = shippedDocument.asObject();
        if (members == nullptr) continue;
        for (const auto& [key, shippedValue] : *members) {
            if (shippedValue.asArray() == nullptr) continue;
            json::Value storedValue;
            if (auto got = db.Get(collection, key); got.ok())
                storedValue = got.value();
            json::Value merged =
                MergeShippedEntries(storedValue, shippedValue, report.added, report.refreshed);
            if (auto put = db.Put(collection, key, std::move(merged)); !put.ok()) {
                Logger::Instance().Warning(
                    std::format("cannot merge shipped {}.{}: {}", collection, key, put.error().message),
                    kModule);
            }
        }
    }

    json::Value::Object state;
    state["fingerprint"] = json::Value::String(fingerprint);
    state["added"] = json::Value::Number(static_cast<double>(report.added));
    state["refreshed"] = json::Value::Number(static_cast<double>(report.refreshed));
    (void)db.Put(kStateCollection, kStateKey, json::Value(std::move(state)));
    (void)db.Flush();

    report.applied = true;
    report.fingerprint = fingerprint;
    Logger::Instance().Info(
        std::format("applied shipped configuration {} ({} added, {} refreshed)",
                    fingerprint, report.added, report.refreshed),
        kModule);
    return report;
}

ShippedMergeReport ApplyShippedConfiguration() {
    static std::once_flag once;
    static ShippedMergeReport report;
    std::call_once(once, [] {
        const std::string file =
            platform::PlatformAccessor::Get().Paths().ExecutableDir() + "/assets/" + kBaselineName;
        report = ApplyShippedConfigurationFrom(file);
    });
    return report;
}
} // namespace bps::project