#include "core/services/ServiceManager.hpp"

namespace bps {

ServiceManager& ServiceManager::Instance() {
    static ServiceManager instance;
    return instance;
}

Result<void> ServiceManager::RegisterEntry(std::type_index key, Lifetime lifetime,
                                           std::function<std::shared_ptr<IService>()> factory,
                                           std::vector<std::string> dependencies,
                                           IService* instance, std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry e;
    e.lifetime = lifetime;
    e.factory = std::move(factory);
    e.dependencies = std::move(dependencies);
    e.instance = instance;
    e.name = std::move(name);
    auto [it, inserted] = entries_.emplace(key, std::move(e));
    if (!inserted) it->second = std::move(e);   // re-registration replaces (04 §2 Replace)
    return Ok();
}

std::shared_ptr<IService> ServiceManager::CreateInstance(Entry& e) {
    if (!e.factory) return nullptr;
    try {
        return e.factory();
    } catch (...) {
        return nullptr;
    }
}

std::string ServiceManager::EntryName(const Entry& e, std::type_index key) {
    if (!e.name.empty()) return e.name;
    if (e.instance) return e.instance->ServiceName();
    return key.name();
}

Result<IService*> ServiceManager::ResolveEntry(std::type_index key, std::string scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end())
        return Error::Make(Err::Services_NotRegistered, "ServiceManager",
                           "service not registered: " + std::string(key.name()));

    Entry& e = it->second;
    if (e.resolving)
        return Error::Make(Err::Services_CyclicDependency, "ServiceManager",
                           "cyclic dependency while resolving: " + std::string(key.name()));

    switch (e.lifetime) {
        case Lifetime::Singleton: {
            if (!e.instance) {
                e.resolving = true;
                e.owned = CreateInstance(e);
                e.resolving = false;
                e.instance = e.owned.get();
            }
            return e.instance;
        }
        case Lifetime::Scoped: {
            auto& inst = e.scoped[scope];
            if (!inst) {
                e.resolving = true;
                inst = CreateInstance(e);
                e.resolving = false;
            }
            return inst.get();
        }
        case Lifetime::Transient: {
            e.resolving = true;
            auto created = CreateInstance(e);
            e.resolving = false;
            if (created) transientOwned_.push_back(created);
            return created.get();
        }
    }
    return nullptr;
}

Result<void> ServiceManager::Replace(std::type_index key, IService* instance) {
    if (!instance)
        return Error::Make(Err::InvalidArgument, "ServiceManager", "null service instance");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end())
        return Error::Make(Err::NotFound, "ServiceManager", "service not registered: " + std::string(key.name()));
    it->second.instance = instance;
    it->second.name = instance->ServiceName();
    it->second.owned.reset();
    return Ok();
}

Result<void> ServiceManager::CheckCycles() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Build the declared graph: node = service name, edges = declared dependencies.
    std::unordered_map<std::string, std::vector<std::string>> adj;
    for (const auto& [key, e] : entries_) adj[EntryName(e, key)] = e.dependencies;
    if (adj.empty()) return Ok();

    // Unknown dependencies (not a cycle, but a graph defect).
    std::vector<std::string> unknown;
    for (const auto& [n, deps] : adj)
        for (const auto& d : deps)
            if (adj.count(d) == 0 && !d.empty()) unknown.push_back(n + " -> " + d);

    // DFS with colors (0 white, 1 gray, 2 black) to find back edges.
    std::unordered_map<std::string, int> color;
    std::function<bool(const std::string&)> dfs = [&](const std::string& n) -> bool {
        color[n] = 1;
        for (const auto& dep : adj[n]) {
            if (adj.count(dep) == 0) continue;
            if (color[dep] == 1) return true;   // back edge -> cycle
            if (color[dep] == 0 && dfs(dep)) return true;
        }
        color[n] = 2;
        return false;
    };
    for (const auto& [n, _] : adj)
        if (color[n] == 0 && dfs(n))
            return Error::Make(Err::Services_CyclicDependency, "ServiceManager",
                               "dependency cycle detected in service graph");
    if (!unknown.empty()) {
        std::string msg = "undeclared service dependencies: ";
        for (size_t i = 0; i < unknown.size() && i < 5; ++i) msg += unknown[i] + "; ";
        return Error::Make(Err::Services_NotRegistered, "ServiceManager", msg);
    }
    return Ok();
}

std::vector<std::pair<std::string, ServiceInfo>> ServiceManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, ServiceInfo>> out;
    out.reserve(entries_.size());
    for (const auto& [key, e] : entries_) {
        ServiceInfo info;
        info.name = EntryName(e, key);
        info.version = e.instance ? e.instance->ServiceVersion() : kEngineVersion;
        info.dependencies = e.dependencies;
        out.emplace_back(info.name, std::move(info));
    }
    return out;
}

Result<void> ServiceManager::DestroyAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    owned_.clear();
    transientOwned_.clear();
    entries_.clear();
    return Ok();
}

HealthReport ServiceManager::GetHealth() const {
    HealthReport r;
    std::lock_guard<std::mutex> lock(mutex_);
    r.detail = std::format("{} services registered", entries_.size());
    return r;
}

} // namespace bps
