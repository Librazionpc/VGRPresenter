# 03 — ConfigurationManager Specification

| Field | Value |
|---|---|
| **System** | ConfigurationManager |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Implementation status** | **Implemented** — full implementation with unit tests (`tests/unit/main.cpp`); audited in `docs/architecture/Conformance.md` |
| **Source files** | `core/include/core/config/ConfigurationManager.hpp`, `core/config/ConfigurationManager.cpp` |
| **Depends on** | Logger (02) |
| **Inherits** | 00 Cross-System Rules |

## 1. Purpose

The ConfigurationManager is the **single source of truth for every setting** in the
engine. It owns the layered, versioned, validated configuration model (global, user,
workspace, project, temporary, runtime), the file formats, hot reload, profiles,
migration, and optional encryption — so no subsystem ever parses config files or
duplicates settings logic on its own.

## 2. Responsibilities

- Own the **configuration scope stack** and precedence resolution.
- Load/save/reload/reset configuration for all scopes and formats.
- Validate every value against its declared schema; reject invalid configs with precise
  errors.
- Migrate configs across **schema versions** automatically.
- Watch config files on disk and **hot reload** changes.
- Provide typed, keyed access with defaults and change notifications.
- Support **profiles** (e.g. `presentation`, `studio`, `kiosk`) and per-scope overrides.
- Encrypt sensitive sections (API keys, credentials) at rest.
- Feed the Kernel's boot settings and all systems' `Reload()` paths (00 §5).

## 3. Public API

```cpp
class ConfigurationManager final {
public:
    static ConfigurationManager& Instance();

    // Scopes
    Result<void> Load(Scope scope);                          // from its source(s)
    Result<void> Save(Scope scope);
    Result<void> Reload(Scope scope);                        // hot reload entry point
    Result<void> Reset(Scope scope, const std::string& profile = {});

    // Typed access — resolves through scope stack + profile
    Result<T>    Get<T>(std::string_view key, ScopeHint hint = AnyScope) const;
    Result<void> Set<T>(std::string_view key, const T& value, Scope scope);
    Result<bool> Has(std::string_view key) const noexcept;
    Result<void> Remove(std::string_view key, Scope scope);

    // Validation & migration
    Result<void> Validate() const;                           // whole tree vs schemas
    Result<void> Validate(std::string_view key) const;
    Result<void> Migrate(Scope scope, SchemaVersion target);  // versioned migrations

    // Profiles
    Result<void> ApplyProfile(const std::string& name);
    std::vector<std::string> AvailableProfiles() const;

    // Watching / notifications
    Result<void> Watch(Scope scope, bool enable);            // file watching
    Subscription Subscribe(std::string_view keyPattern, std::function<void(ConfigChange)> cb);

    // Diagnostics
    HealthReport GetHealth() const noexcept;                 // 00 §7
    ConfigMetrics Metrics() const noexcept;                  // 00 §4
};
```

`Scope` enum: `Global` → `User` → `Workspace` → `Project` → `Temporary` → `Runtime`
(higher overrides lower). `ConfigChange` = `{scope, key, oldValue, newValue, kind}`.

## 4. Internal Components

| Component | Role |
|---|---|
| `ConfigurationManager` | Facade; scope stack, API |
| `ScopeStore` | One layered store per scope; precedence resolution |
| `ConfigNode` | Typed tree node (scalar/object/array) with schema ref and provenance |
| `SchemaRegistry` | Declared schemas (types, ranges, enums, required, defaults) per section |
| `Loader/Saver` | JSON (default), TOML, YAML backends |
| `MigrationEngine` | Versioned migration chain `schema_version` → target |
| `FileWatcher` | Inotify/ReadDirectoryChangesW-based hot reload with debounce |
| `ProfileManager` | Profile definitions + active profile resolution |
| `CryptoStore` | Optional encryption (AES-GCM) for sensitive sections |
| `SubscriptionRegistry` | Key-pattern subscriptions for change events |

## 5. State Machine

```
Uninitialized --> Loading --> Ready <--> Reloading
                    |            |
                    +--> Failed -+  (validation/migration error; stays usable in LastKnownGood)
```

| State | Meaning |
|---|---|
| `Uninitialized` | No scopes loaded |
| `Loading` | Parsing/validating/migrating one or more scopes |
| `Ready` | All requested scopes valid and active |
| `Reloading` | Hot reload in progress (reads are served from the old tree until commit) |
| `Failed` | Load/validation error; serves `LastKnownGood` snapshot + error report |

## 6. Threading Model

- **Multi-threaded reads, single-writer.** Reads are served from an immutable
  `ConfigNode` tree behind an RW-lock (readers are lock-free after acquiring the shared
  lock; the tree itself is immutable once published — copy-on-write on mutation).
- **Writes and reloads are serialized** on a single mutation queue; a reload never
  happens mid-read (readers finish on the old tree).
- Hot-reload detection runs on a low-frequency watcher task owned by the
  TaskScheduler (07) — never on a config-owned thread.

## 7. Events Published

| Event | Payload | When |
|---|---|---|
| `engine.config.loaded` | `{scope}` | Scope load completes |
| `engine.config.hot_reload` | `{scope, changedKeys[]}` | File change detected & applied |
| `engine.config.changed` | `ConfigChange` | Any `Set`/`Remove`/reload delta |
| `engine.config.validation_error` | `{scope, key, message}` | Validation failure (non-fatal) |
| `engine.config.migrated` | `{scope, from, to}` | Schema migration performed |

## 8. Events Consumed

| Event | Action |
|---|---|
| `engine.kernel.booted` (01) | Publish merged view of boot-relevant config to subscribers |
| `engine.resource.pressure_high` (10) | Stop file watching if requested, defer saves |

## 9. Dependencies

- **Depends on:** Logger (02) only.
- **Uses after init:** TaskScheduler (07) for the watch task; EventBus (05) to publish
  change events; ResourceManager (10) for disk pressure; MemoryManager (11) for config
  tree allocations.
- **Provides:** config values to *every* system (via keys, not direct coupling).

## 10. Failure Modes

| Failure | Behavior |
|---|---|
| Parse error in a config file | Load `LastKnownGood` for that scope, log the offending file/line, surface `engine.config.validation_error`; do **not** crash |
| Validation failure (out of range, wrong type) | Reject the value, keep the old value, report precisely which key/schema failed |
| Migration missing for a schema version | Refuse to load that scope, serve `LastKnownGood`, mark scope `Failed` with a clear upgrade path |
| Hot-reload race (file changes mid-write) | Debounce + compare-and-swap on the file; retry once; log a warning |
| Encryption key unavailable | Degrade: keep sensitive section locked and report; never crash at startup |
| Unwritable config dir | `Save` returns `Error`; changes remain in `Runtime` scope for the session |

## 11. Performance Goals

| Goal | Target |
|---|---|
| `Get<T>()` hot path | **< 500 ns** (lock-free read of immutable tree) |
| Load of a full project config (10k keys) | **< 10 ms** |
| Hot reload apply | **< 5 ms** end-to-end (watcher tick → `engine.config.hot_reload`) |
| Memory | tree ≈ 100 B/key; bounded by config size, not process size |

## 12. Future Extensions

- **Config-as-code** (defaults in headers generated from schemas).
- **Cloud-synced profiles** (config sync service) via the same scopes/profiles model.
- **A/B experiment overrides** as a new `Experiment` scope above `Runtime`.
- **Live inspector UI** (config editor) driven entirely by the schema registry.
- **Config diff/audit trail** for multi-operator environments (church teams).

## 13. Testing Requirements

- **Unit:** scope precedence, profiles, defaults, validation, subscription patterns,
  migration chains, each format (JSON/TOML/YAML round-trip).
- **Stress:** 1 M key reads from 8 threads during continuous reload — no torn reads,
  no deadlock.
- **Performance:** assert §11 budgets (Get latency, load time).
- **Failure:** corrupt files (truncated, wrong type, bad UTF-8), missing migrations,
  unwritable dirs, watcher races — verify `LastKnownGood` behavior.
