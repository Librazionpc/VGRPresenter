#include "modules/content/AssetWatcher.hpp"

#include <algorithm>

namespace bps::content {

Result<void> AssetWatcher::AddRoot(
    std::string_view dir,
    const std::function<std::optional<std::vector<std::string>>(std::string_view)>& list) {
    std::lock_guard<std::mutex> lock(mutex_);
    Root r;
    r.listFn = list;
    auto entries = list ? list(dir) : std::nullopt;
    if (entries) r.last = *entries;
    std::sort(r.last.begin(), r.last.end());
    roots_[std::string(dir)] = std::move(r);
    return Ok();
}

void AssetWatcher::RemoveRoot(std::string_view dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    roots_.erase(std::string(dir));
}

std::vector<WatchChange> AssetWatcher::Poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WatchChange> changes;
    for (auto& [dir, root] : roots_) {
        auto entries = root.listFn ? root.listFn(dir) : std::nullopt;
        if (!entries) continue;
        std::vector<std::string> cur = *entries;
        std::sort(cur.begin(), cur.end());

        // Diff cur vs last.
        size_t i = 0, j = 0;
        std::vector<WatchChange> added, removed;
        while (i < cur.size() && j < root.last.size()) {
            if (cur[i] < root.last[j]) {
                added.push_back({WatchChange::Kind::Added, cur[i], {}});
                ++i;
            } else if (cur[i] > root.last[j]) {
                removed.push_back({WatchChange::Kind::Removed, root.last[j], {}});
                ++j;
            } else {
                ++i;
                ++j;
            }
        }
        for (; i < cur.size(); ++i) added.push_back({WatchChange::Kind::Added, cur[i], {}});
        for (; j < root.last.size(); ++j)
            removed.push_back({WatchChange::Kind::Removed, root.last[j], {}});

        // Heuristic rename detection: pair removed+added with equal basename.
        for (auto& rm : removed) {
            auto base = rm.path.substr(rm.path.find_last_of('/') + 1);
            for (auto& ad : added) {
                if (ad.kind == WatchChange::Kind::Added) {
                    auto adBase = ad.path.substr(ad.path.find_last_of('/') + 1);
                    if (adBase == base) {
                        changes.push_back({WatchChange::Kind::Moved, rm.path, ad.path});
                        ad.kind = WatchChange::Kind::Added;   // consumed marker
                        ad.path.clear();
                        rm.path.clear();
                        break;
                    }
                }
            }
        }
        for (auto& ad : added)
            if (!ad.path.empty()) changes.push_back(ad);
        for (auto& rm : removed)
            if (!rm.path.empty()) changes.push_back(rm);

        root.last = std::move(cur);
    }
    if (callback_) {
        for (const auto& c : changes) callback_(c);
    }
    return changes;
}

size_t AssetWatcher::RootCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return roots_.size();
}

} // namespace bps::content
