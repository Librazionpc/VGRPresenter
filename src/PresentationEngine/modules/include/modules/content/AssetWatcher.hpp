#pragma once

#include "modules/content/AssetMetadata.hpp"
#include "core/common/Common.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bps::content {
struct WatchChange {
    enum class Kind : int { Added, Removed, Changed, Moved } kind = Kind::Added;
    std::string path;      // VFS path
    std::string toPath;    // for Moved
};

using WatchCallback = std::function<void(const WatchChange&)>;

class AssetWatcher {
public:
    AssetWatcher() = default;

    // Begin watching a directory (VFS path). Registers a baseline snapshot.
    Result<void> AddRoot(std::string_view dir,
                         const std::function<std::optional<std::vector<std::string>>(std::string_view)>& list);

    void RemoveRoot(std::string_view dir);

    // Poll all roots and report changes since the last poll (or since AddRoot).
    std::vector<WatchChange> Poll();

    void SetCallback(WatchCallback cb) { callback_ = std::move(cb); }

    size_t RootCount() const;

private:
    struct Root {
        std::vector<std::string> last;   // sorted entry paths
        std::function<std::optional<std::vector<std::string>>(std::string_view)> listFn;
    };

    mutable std::mutex mutex_;
    std::map<std::string, Root> roots_;
    WatchCallback callback_;
};
} // namespace bps::content