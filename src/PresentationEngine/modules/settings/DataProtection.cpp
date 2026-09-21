#include "modules/settings/DataProtection.hpp"

#include "core/config/Json.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>

namespace bps::settings {

namespace {

constexpr const char* kModule = "DataProtection";
constexpr std::string_view kSeparator = "__";

std::string FileName(std::string_view path) {
    const size_t cut = path.find_last_of("/\\");
    return std::string(cut == std::string_view::npos ? path : path.substr(cut + 1));
}

// "Sunday.vgr" -> "Sunday", ".vgr"
void SplitName(const std::string& name, std::string& stem, std::string& ext) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) { stem = name; ext.clear(); return; }
    stem = name.substr(0, dot);
    ext = name.substr(dot);
}

bool LooksLikeStamp(std::string_view s) {
    // yyyymmdd-hhmmss, optionally followed by -<n>
    if (s.size() < 15 || s[8] != '-') return false;
    for (size_t i = 0; i < 15; ++i)
        if (i != 8 && !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    return s.size() == 15 || (s[15] == '-' && s.size() > 16);
}

} // namespace

std::string NowStamp() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &local);
    return buf;
}

// ---------------------------------------------------------------------------
// BackupStore
// ---------------------------------------------------------------------------

Result<std::string> BackupStore::Create(std::string_view sourceFile, std::string stamp) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (!fs.IsRegularFile(sourceFile))
        return Error::Make(Err::NotFound, kModule, std::format("there is no file to back up at '{}'", sourceFile));
    if (auto made = fs.CreateDirectories(directory_); !made.ok()) return made.error();

    std::string stem, ext;
    SplitName(FileName(sourceFile), stem, ext);
    if (stamp.empty()) stamp = NowStamp();

    std::string target = fs.Join(directory_, std::format("{}{}{}{}", stem, kSeparator, stamp, ext));
    for (int n = 2; fs.Exists(target); ++n)
        target = fs.Join(directory_, std::format("{}{}{}-{}{}", stem, kSeparator, stamp, n, ext));
    if (auto copied = fs.Copy(sourceFile, target); !copied.ok()) return copied.error();
    return target;
}

Result<std::string> BackupStore::CreateIfChanged(std::string_view sourceFile, std::string stamp) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    std::string stem, ext;
    SplitName(FileName(sourceFile), stem, ext);
    if (const auto newest = List(stem); !newest.empty()) {
        auto a = fs.ReadBinary(sourceFile);
        auto b = fs.ReadBinary(newest.front().path);
        if (a.ok() && b.ok() && a.value() == b.value()) return std::string();
    }
    return Create(sourceFile, std::move(stamp));
}

std::vector<BackupInfo> BackupStore::List(std::string_view show) const {
    std::vector<BackupInfo> out;
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (!fs.IsDirectory(directory_)) return out;
    auto entries = fs.Enumerate(directory_);
    if (!entries.ok()) return out;

    for (const std::string& path : entries.value()) {
        if (!fs.IsRegularFile(path)) continue;
        std::string stem, ext;
        SplitName(FileName(path), stem, ext);
        const size_t sep = stem.rfind(kSeparator);
        if (sep == std::string::npos || sep == 0) continue;
        BackupInfo info;
        info.show = stem.substr(0, sep);
        info.stamp = stem.substr(sep + kSeparator.size());
        if (!LooksLikeStamp(info.stamp)) continue;
        if (!show.empty() && info.show != show) continue;
        info.path = path;
        if (auto size = fs.FileSize(path); size.ok()) info.sizeBytes = size.value();
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) {
        // the stamp's own text sorts by time; a "-2" suffix (same second) sorts after its original
        return a.stamp != b.stamp ? a.stamp > b.stamp : a.path > b.path;
    });
    return out;
}

size_t BackupStore::Prune(std::string_view show, size_t keepLast) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::vector<BackupInfo> all = List(show);
    size_t removed = 0;
    for (size_t i = keepLast; i < all.size(); ++i)
        if (fs.Remove(all[i].path).ok()) ++removed;
    return removed;
}

// ---------------------------------------------------------------------------
// RecoveryStore
// ---------------------------------------------------------------------------

std::string RecoveryStore::Path() const {
    return platform::PlatformAccessor::Get().Filesystem().Join(directory_, "unsaved-show.vgr");
}

Result<void> RecoveryStore::Note(std::string_view showName, std::string_view originalPath) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (auto made = fs.CreateDirectories(directory_); !made.ok()) return made;
    json::Value::Object o;
    o["showName"] = json::Value::String(std::string(showName));
    o["originalPath"] = json::Value::String(std::string(originalPath));
    return fs.Write(fs.Join(directory_, "unsaved-show.json"), json::Value(std::move(o)).ToString());
}

std::optional<RecoveryInfo> RecoveryStore::Info() const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (!fs.IsRegularFile(Path())) return std::nullopt;
    RecoveryInfo info;
    info.path = Path();
    const std::string note = fs.Join(directory_, "unsaved-show.json");
    if (fs.IsRegularFile(note))
        if (auto text = fs.ReadText(note); text.ok())
            if (auto parsed = json::Parse(text.value()); parsed.ok()) {
                if (const auto* n = parsed.value().Find("showName")) info.showName = std::string(n->asString());
                if (const auto* p = parsed.value().Find("originalPath")) info.originalPath = std::string(p->asString());
            }
    if (info.showName.empty()) info.showName = "Untitled show";
    return info;
}

Result<void> RecoveryStore::RestoreTo(std::string_view target) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (!fs.IsRegularFile(Path()))
        return Error::Make(Err::NotFound, kModule, "there is nothing to recover");
    if (fs.Exists(target))
        return Error::Make(Err::AlreadyExists, kModule, std::format("'{}' already exists", target));
    if (auto r = fs.Copy(Path(), target); !r.ok()) return r;
    return Clear();
}

Result<void> RecoveryStore::Clear() const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    for (const std::string& p : { Path(), fs.Join(directory_, "unsaved-show.json") })
        if (fs.Exists(p))
            if (auto r = fs.Remove(p); !r.ok()) return r;
    return {};
}

} // namespace bps::settings
