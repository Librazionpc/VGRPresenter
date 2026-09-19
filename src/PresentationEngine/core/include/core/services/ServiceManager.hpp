#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IService.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace bps {

// Service manifest metadata (00 §6 Versioning).
struct ServiceInfo {
    std::string name;
    Version version;
    std::string author;
    std::vector<std::string> dependencies;
    Version requiredCoreVersion;
};

// Dependency injection container (docs/specs/04).
// v1 semantics: registration is non-owning for raw singletons; lazy factories and
// scoped/transient instances are owned by this manager and released on DestroyAll.
// Resolution is by interface type (typeid), so mocking and replacement are easy.
class ServiceManager final : public IService {
public:
    static ServiceManager& Instance();

    // Lifetimes (04 §3): Singleton = one process-wide instance; Scoped = cached
    // per scope token; Transient = a fresh instance per Resolve.
    enum class Lifetime : int { Singleton = 0, Scoped, Transient };

    // --- Registration ---
    template <class I>
    Result<void> Register(I* instance, std::vector<std::string> dependencies = {}) {
        if (!instance)
            return Error::Make(Err::InvalidArgument, "ServiceManager", "null service instance");
        return RegisterEntry(std::type_index(typeid(I)), Lifetime::Singleton, {},
                             std::move(dependencies), instance, instance->ServiceName());
    }

    template <class I>
    Result<void> RegisterShared(std::shared_ptr<I> instance,
                                std::vector<std::string> dependencies = {}) {
        if (!instance)
            return Error::Make(Err::InvalidArgument, "ServiceManager", "null service instance");
        owned_.push_back(instance);
        return RegisterEntry(std::type_index(typeid(I)), Lifetime::Singleton, {},
                             std::move(dependencies), instance.get(), instance->ServiceName());
    }

    // Lazy registration: the factory runs on the first Resolve (04 §3 Lazy).
    // `name` is the ServiceName used by the dependency graph (defaults to the
    // factory result's ServiceName once created; pass it explicitly for static
    // cycle detection of not-yet-created services).
    template <class I>
    Result<void> RegisterLazy(std::function<std::shared_ptr<I>()> factory,
                              Lifetime lifetime = Lifetime::Singleton,
                              std::vector<std::string> dependencies = {},
                              std::string name = {}) {
        if (!factory)
            return Error::Make(Err::InvalidArgument, "ServiceManager", "null factory");
        std::function<std::shared_ptr<IService>()> boxed = [factory]() -> std::shared_ptr<IService> {
            return factory();
        };
        return RegisterEntry(std::type_index(typeid(I)), lifetime, std::move(boxed),
                             std::move(dependencies), nullptr, std::move(name));
    }

    // --- Resolution ---
    template <class I>
    I* Get() const {
        auto r = const_cast<ServiceManager*>(this)->Resolve<I>();
        return r.ok() ? r.value() : nullptr;
    }

    template <class I>
    Result<I*> Resolve(std::string_view scope = {}) {
        std::type_index key(typeid(I));
        auto r = ResolveEntry(key, std::string(scope));
        if (!r.ok()) return r.error();
        return static_cast<I*>(r.value());
    }

    template <class I>
    bool IsRegistered() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.count(std::type_index(typeid(I))) > 0;
    }

    // --- Removal / replacement ---
    template <class I>
    Result<void> Remove() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(std::type_index(typeid(I)));
        if (it == entries_.end())
            return Error::Make(Err::NotFound, "ServiceManager", "service not registered");
        entries_.erase(it);
        return Ok();
    }

    Result<void> Replace(std::type_index key, IService* instance);

    // --- Dependency graph (04 §4) ---
    // Validates the declared dependency graph: unknown dependencies are reported,
    // cycles are rejected. No-op when nothing is registered.
    Result<void> CheckCycles();

    // --- Diagnostics ---
    std::vector<std::pair<std::string, ServiceInfo>> Snapshot() const;
    Result<void> DestroyAll();   // releases owned_ (lazy/scoped/transient) instances

    const char* ServiceName() const noexcept override { return "ServiceManager"; }
    HealthReport GetHealth() const override;

private:
    ServiceManager() = default;

    struct Entry {
        Lifetime lifetime = Lifetime::Singleton;
        std::function<std::shared_ptr<IService>()> factory;   // lazy (04 §3)
        std::vector<std::string> dependencies;                // interface names (04 §4)
        IService* instance = nullptr;                         // singleton raw
        std::shared_ptr<IService> owned;                      // singleton owned (lazy-created)
        std::unordered_map<std::string, std::shared_ptr<IService>> scoped;  // per scope
        std::string name;                                     // ServiceName for the graph
        bool resolving = false;                               // cycle guard
    };

    Result<void> RegisterEntry(std::type_index key, Lifetime lifetime,
                               std::function<std::shared_ptr<IService>()> factory,
                               std::vector<std::string> dependencies, IService* instance,
                               std::string name);
    Result<IService*> ResolveEntry(std::type_index key, std::string scope);
    std::shared_ptr<IService> CreateInstance(Entry& e);       // caller holds mutex_
    static std::string EntryName(const Entry& e, std::type_index key);

    mutable std::mutex mutex_;
    std::unordered_map<std::type_index, Entry> entries_;      // guarded by mutex_
    std::vector<std::shared_ptr<IService>> owned_;            // guarded by mutex_
    std::vector<std::shared_ptr<IService>> transientOwned_;   // guarded by mutex_
};

} // namespace bps
