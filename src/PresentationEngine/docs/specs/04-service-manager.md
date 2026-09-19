# 04 — ServiceManager Specification

| Field | Value |
|---|---|
| **System** | ServiceManager |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Implementation status** | **Implemented** — full implementation with unit tests (`tests/unit/main.cpp`); audited in `docs/architecture/Conformance.md` |
| **Source files** | `core/include/core/services/ServiceManager.hpp`, `core/services/ServiceManager.cpp` |
| **Depends on** | Logger (02), ConfigurationManager (03) |
| **Inherits** | 00 Cross-System Rules |

## 1. Purpose

The ServiceManager is the engine's **dependency injection container**. It is the *only*
way systems, modules, and plugins obtain collaborators. **No module manually creates
another module**; everything is registered here and resolved through interfaces, which
enforces the interface-only dependency rule (00 §2) and makes the entire engine
replaceable, mockable, and hot-reloadable.

## 2. Responsibilities

- **Register** service definitions (interface → factory/lifetime).
- **Resolve** services by interface type; construct lazily per lifetime rules.
- **Replace** implementations at runtime (e.g. swapping a renderer, a mock in tests).
- **Destroy** services cleanly in dependency order.
- **Reload** services without restarting the engine (00 §5).
- Manage **lifetimes**: `Singleton`, `Scoped`, `Transient`, `Lazy`.
- Honor **dependencies** and **priorities** between services (topological resolution).
- Detect and reject **circular dependencies** at registration/resolution time.

## 3. Public API

```cpp
class ServiceManager final {
public:
    static ServiceManager& Instance();

    // Registration
    template <class I, class Impl, class... Args>
    Result<void> Register(Lifetime lifetime = Lifetime::Singleton, int priority = 0, Args&&... args);

    template <class I>
    Result<void> RegisterFactory(Lifetime lifetime, std::function<std::shared_ptr<I>()> factory, int priority = 0);

    // Resolution
    template <class I> std::shared_ptr<I> Resolve();           // returns nullptr on failure + logs via 02; use TryResolve for the error
    template <class I> Result<std::shared_ptr<I>> TryResolve();
    template <class I> bool IsRegistered() const noexcept;

    // Replacement / lifecycle
    template <class I> Result<void> Replace(std::shared_ptr<I> instance);   // hot swap
    template <class I> Result<void> Destroy();                              // tear down one service
    template <class I> Result<void> Reload();                               // re-init from factory
    Result<void> DestroyAll();                                              // full teardown (shutdown order)

    // Introspection (diagnostics/tests)
    std::vector<ServiceInfo> RegistrySnapshot() const;        // 00 §6 manifests
    HealthReport GetHealth() const noexcept;                  // 00 §7
    ServiceMetrics Metrics() const noexcept;                  // 00 §4
};
```

`Lifetime` semantics:

| Lifetime | Semantics |
|---|---|
| `Singleton` | One instance per engine lifetime; resolved on first use, destroyed at shutdown |
| `Scoped` | One instance per named scope (e.g. per module, per workspace) |
| `Transient` | New instance per `Resolve()` |
| `Lazy` | Like Singleton, but construction deferred until first `Resolve()` |

## 4. Internal Components

| Component | Role |
|---|---|
| `ServiceManager` | Facade + lifetime registry |
| `ServiceRegistry` | Interface-keyed definitions (typeid/string keys), lifetime, priority, factory |
| `InstanceCache` | Live instances per lifetime scope |
| `DependencyResolver` | Topological resolution; cycle detection; priority ordering |
| `LifetimeScope` | Tracks scoped instances for teardown in registration order |
| `ServiceInfo` | Manifest: name, version, author, deps, required core version, capabilities (00 §6) |

## 5. State Machine

```
Idle --> Registering --> Ready --> Replacing/Reloading --> Ready
                             |              |
                             +--> Destroying -> Idle
```

| State | Meaning |
|---|---|
| `Idle` | Empty registry (pre-boot) |
| `Registering` | Core systems registering during boot |
| `Ready` | Registry complete; resolution enabled |
| `Replacing/Reloading` | A service is being hot-swapped; that service's consumers see a quiesced instance |
| `Destroying` | Teardown in dependency order |

## 6. Threading Model

- **Multi-threaded.** Registration is boot-time (single-threaded) but `Resolve()` is
  called from any thread at runtime.
- **Locking:** registry reads under a shared lock; resolution of an unconstructed
  Singleton uses a per-service mutex with double-checked construction. Instance caches
  are protected per lifetime scope.
- **Replace/Reload** take an exclusive lock on the affected service and *all* its
  dependents' resolution paths are quiesced first (drain rule, 00 §5), then the swap is
  atomic.

## 7. Events Published

| Event | Payload | When |
|---|---|---|
| `engine.services.registered` | `ServiceInfo` | Service registered |
| `engine.services.resolved` | `{interface, lifetime}` | First resolution of a lazy/singleton service |
| `engine.services.replaced` | `{interface, oldVersion, newVersion}` | Hot swap completed |
| `engine.services.destroyed` | `{interface}` | Service destroyed |

## 8. Events Consumed

| Event | Action |
|---|---|
| `engine.config.hot_reload` (03) | Re-resolve services whose config dependency changed (e.g. renderer settings) |
| `engine.module.state_changed` (08) | Create/destroy scoped services for that module |
| `engine.kernel.shutdown_started` (01) | Begin `DestroyAll()` in dependency order |

## 9. Dependencies

- **Depends on:** Logger (02), ConfigurationManager (03).
- **Uses after init:** EventBus (05) for publish; LifecycleManager (12) conventions for
  teardown ordering.
- **Provides:** every service in the engine. All core systems, modules, and plugins
  register through it; all cross-system access goes through it.

## 10. Failure Modes

| Failure | Behavior |
|---|---|
| Service not registered | `TryResolve` returns `Error` with interface name; `Resolve` logs and returns `nullptr`; no crash |
| Circular dependency detected | Registration or resolution fails with a `Result` listing the cycle path |
| Factory throws / returns null | Wrapped into `Error`; the service is marked `Failed` and excluded from resolution |
| Replace() during active use | Quiesce consumers (drain), swap, resume; if quiesce times out (configurable), abort the swap with `Error` |
| Construction failure of a Singleton | Return `Error`, mark the type `Failed`; subsequent resolves return the same error (no retry storm) |
| DestroyAll() ordering violation | LifecycleManager (12) rollback hooks fire; teardown continues in manifest dependency order |

## 11. Performance Goals

| Goal | Target |
|---|---|
| `Resolve()` of a cached singleton | **< 100 ns** (lock-free read of the instance cache) |
| First resolution of a lazy service | construction cost + **< 1 µs** overhead |
| `Replace()` quiesce-to-swap | **< 1 ms** typical service |
| Registry memory | ~64 B per registration |

## 12. Future Extensions

- **Service graphs / visual dependency inspector** (diagnostics UI) from
  `RegistrySnapshot()`.
- **Per-request scoping** (transient scopes tied to event handlers) — same lifetime
  engine, new scope kind.
- **Remote service proxies** (resolve a service that lives on another machine via the
  remote module) — resolved as a regular interface.
- **Aspect hooks** (tracing/metering around resolve) without changing the API.

## 13. Testing Requirements

- **Unit:** all lifetimes, priorities, replace/reload, destroy ordering, cycle
  detection, factory failure paths.
- **Stress:** 100 k resolves from 16 threads across 1 k services — no deadlock, no
  duplicate singletons.
- **Performance:** assert §11 budgets.
- **Failure:** factory throwing, mid-use `Replace()`, double destroy — verify
  quiesce/drain and error surfacing.
