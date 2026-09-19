# 11 — MemoryManager Specification

| Field | Value |
|---|---|
| **System** | MemoryManager |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Implementation status** | **Implemented** — full implementation with unit tests (`tests/unit/main.cpp`); audited in `docs/architecture/Conformance.md` |
| **Source files** | `core/include/core/memory/MemoryManager.hpp`, `core/memory/MemoryManager.cpp` |
| **Depends on** | Logger (02), ConfigurationManager (03), ServiceManager (04), ThreadPool (06) |
| **Inherits** | 00 Cross-System Rules |

## 1. Purpose

The MemoryManager **owns memory** in the engine. **Modules never allocate randomly** —
they allocate from the engine's allocators, tagged with their identity. This gives the
engine control over fragmentation, leak detection, per-module limits, allocation
graphics/snapshots, and debug-mode safety, and it is the foundation the ResourceManager
(10) uses to reason about actual memory use.

## 2. Responsibilities

- Provide allocators: **Pools, Arena, Stack, Heap, Small-Object, Large-Object, Shared
  memory**.
- Route allocations by owner and allocation size class through the right allocator.
- Track every allocation with a **memory tag** (module/plugin/system + purpose).
- Expose **monitoring**: leaks, fragmentation, allocation graph, snapshots, statistics.
- Enforce per-owner **limits** and detect runaway allocations (cooperates with 10).
- Provide **debug mode** (guard bytes, allocation stack capture, double-free detection).
- **Automatic cleanup** on owner teardown (module unload, plugin unload, scope end).
- Own shared-memory regions for IPC and cross-module zero-copy buffers.

## 3. Public API

```cpp
class MemoryManager final {
public:
    static MemoryManager& Instance();

    // Allocation (tagged)
    template <class T> Result<T*> New(MemoryTag tag, AllocatorKind kind = Auto, Args&&...);
    template <class T> Result<void> Delete(MemoryTag tag, T* ptr);
    Result<void*> Allocate(MemoryTag tag, size_t bytes, size_t alignment = alignof(std::max_align_t),
                           AllocatorKind kind = Auto);
    Result<void>  Release(MemoryTag tag, void* ptr, size_t bytes = 0);
    template <class T> Result<T*> NewArray(MemoryTag tag, size_t count);
    template <class T> Result<void> DeleteArray(MemoryTag tag, T* ptr);

    // Allocator acquisition (for containers / modules)
    Result<std::shared_ptr<Allocator>> GetAllocator(MemoryTag tag, AllocatorKind kind);
    Result<void> CreatePool(MemoryTag tag, size_t blockSize, size_t prealloc);
    Result<void> CreateArena(MemoryTag tag, size_t initialSize);
    Result<void> CreateSharedRegion(MemoryTag tag, const SharedRegionDesc& desc);   // name, size, flags

    // Limits & cleanup
    Result<void> SetLimit(MemoryTag tag, size_t maxBytes);
    Result<void> Cleanup(MemoryTag tag, bool force);          // release pooled/recyclable memory
    Result<void> SuspendOwner(std::string_view ownerId);      // trim caches, keep state (10 smart rule)
    Result<void> ResumeOwner(std::string_view ownerId);

    // Monitoring
    MemoryStats Stats(MemoryTag tag = All) const noexcept;    // total, by-kind, by-tag
    std::vector<AllocationSample> Snapshot() const;           // allocation graph dump
    Result<void> DumpLeaks() const;                           // leak report (debug)
    size_t       FragmentationRatio(AllocatorKind kind) const noexcept;

    // Diagnostics
    HealthReport GetHealth() const noexcept;                  // 00 §7
    MemoryMetrics Metrics() const noexcept;                   // 00 §4
};
```

`AllocatorKind`: `Auto` (size-class routed), `Pool`, `Arena`, `Stack`, `Heap`,
`SmallObject`, `LargeObject`, `Shared`.

## 4. Internal Components

| Component | Role |
|---|---|
| `MemoryManager` | Facade; tag routing + limits |
| `Allocator` | Base; each kind implements `Allocate/Release` |
| `PoolAllocator` | Fixed-size blocks, O(1) free lists |
| `ArenaAllocator` | Bump allocation, bulk release |
| `StackAllocator` | LIFO, ideal for scoped/frame work |
| `HeapAllocator` | Wrapper over system allocator with tracking |
| `SmallObjectAllocator` | Size-classed (16 B–256 B) with TLS caches |
| `LargeObjectAllocator` | mmap/VirtualAlloc-backed, page-aligned |
| `SharedMemory` | Named cross-process regions |
| `TagRegistry` | MemoryTag ↔ owner metadata (00 §6 manifests) |
| `Tracker` | Per-tag/per-kind accounting; leak and fragmentation monitors |
| `GuardPages` | Debug-mode red zones + canaries |

## 5. State Machine

```
Uninitialized --> Active --> DebugActive (opt-in) --> Active
                    |              |
                    +--> Throttled (limits/pressure) --> Active
                    +--> Cleaning --> Active
```

| State | Meaning |
|---|---|
| `Uninitialized` | Pre-boot; allocations via fallback are logged (should not happen) |
| `Active` | Normal tracking + allocation |
| `DebugActive` | Guard bytes, canaries, stack capture enabled |
| `Throttled` | Under pressure (10): `Cleanup()` on idle owners, new large allocations fail fast |
| `Cleaning` | Bulk release pass in progress |

## 6. Threading Model

- **Multi-threaded, per-kind strategy.** Small allocations use **TLS caches + per-size
  lock-free slabs** (fast path: no global lock). Arena/stack/pool allocators are
  documented as **owner-thread-confined** (single writer) unless created with
  `ThreadSafe` flag. Heap/large-object allocators use a global lock with short critical
  sections.
- **Tracking is lock-free** where possible (per-thread counters merged on demand);
  snapshots take a brief global read lock.
- Owners allocate from their own tags; the tracker attributes memory per owner without
  cross-owner locking.

## 7. Events Published

| Event | Payload | When |
|---|---|---|
| `engine.memory.leak_detected` | `{tag, bytes, stack[]}` | Debug-mode leak found |
| `engine.memory.limit_violated` | `{tag, limit, actual}` | Owner exceeded its limit |
| `engine.memory.pressure` | `{usage_pct, rss, fragmentation}` | Pre-pressure signal to 10 |
| `engine.memory.fragmentation_high` | `{kind, ratio}` | Fragmentation above threshold |
| `engine.memory.cleanup_completed` | `{freedBytes, owner}` | `Cleanup()` pass finished |

## 8. Events Consumed

| Event | Action |
|---|---|
| `engine.resource.pressure_high` (10) | Enter `Throttled`: trim pools of idle owners, fail-fast large allocs |
| `engine.module.unloaded` (08) / `engine.plugin.unloaded` (09) | **Automatic cleanup** of that owner's tags; leak report if residual |
| `engine.config.hot_reload` (03) | Adjust limits, arena sizes, debug mode live |

## 9. Dependencies

- **Depends on:** Logger (02), ConfigurationManager (03), ServiceManager (04),
  ThreadPool (06) (for TLS-cache-aware worker accounting).
- **Uses after init:** EventBus (05) to publish; ResourceManager (10) to report and
  receive pressure.
- **Provides:** all memory for every other system, module, and plugin; the leak and
  limit data the ResourceManager (10) and tests rely on.

## 10. Failure Modes

| Failure | Behavior |
|---|---|
| Owner exceeds its limit | Allocation fails with `Error` (`MemoryManager_LimitExceeded`); event published; owner should degrade (10 escalates to suspend if not) |
| OOM (system-level) | Pre-allocated emergency reserve (configurable, e.g. 64 MB) for fatal diagnostics; attempt ordered cleanup via 10; if unresolvable → Kernel panic path (01) |
| Double-free / bad-free (debug) | Immediate `Fatal` via Logger (02) + abort with precise report; never silent corruption |
| Leak on owner teardown | `Cleanup()` marks residual; `leak_detected` with tag + stack; teardown continues (the leak is attributed, not fatal) |
| Arena misuse (allocation after bulk release) | Documented `Error`; debug mode catches with guard pages |
| Tracking overhead too high | Adaptive: drop stack capture in release, sample tracking (configurable) |

## 11. Performance Goals

| Goal | Target |
|---|---|
| Small allocation (≤ 256 B) | **< 100 ns** (TLS slab fast path) |
| Large allocation (≥ 64 KB) | **< 2 µs** (page-aligned, tracked) |
| Tracking overhead | **< 5%** total allocator cost; stack capture off in release by default |
| Fragmentation (long session) | **< 10%** for typical presentation workloads |
| Snapshot of 1 M allocations | **< 10 ms** |

## 12. Future Extensions

- **Allocation replay / deterministic mode** for reproducible bugs.
- **Memory profiler UI** (allocation flame graphs) fed by snapshots.
- **GPU/VRAM memory routing** (allocation API extended to device memory, aligned with
  10's VRAM tracking).
- **Memory-tier awareness** (huge pages, NUMA placement) behind the same API.
- **Snapshot diffing** for test assertions ("this flow leaks N bytes").

## 13. Testing Requirements

- **Unit:** every allocator kind (pool/arena/stack/heap/small/large/shared), limits,
  cleanup, tags, alignment guarantees.
- **Stress:** 10 M mixed-size allocations from 16 threads — no corruption, no leaks
  (tracker-verified), bounded fragmentation.
- **Performance:** assert §11 budgets (small/large allocation cost).
- **Failure:** limit violations, double-free/bad-free injection, OOM simulation,
  arena misuse — verify containment, attribution, and panic path.
