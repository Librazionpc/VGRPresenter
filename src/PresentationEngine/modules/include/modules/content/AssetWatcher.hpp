#pragma once

// AssetWatcher (docs/specs/13 §Asset Watcher): monitors library folders for
// file changes, renames, deletes, and new assets. Uses the PAL IFilesystem
// Watch where supported; otherwise falls back to a TaskScheduler heartbeat
// poll. Publishes content.asset_added/removed/changed and keeps the metadata
// database synchronized with the underlying files.

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

// Poll-based watcher: a snapshot diff over a folder listing. Cheap and
// dependency-free; the heartbeat is driven externally by the ContentManager
// (via the Core TaskScheduler) so no module owns a thread.
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
