#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace bps {

using MemoryTag = std::string;

enum class AllocatorKind : int {
    Auto = 0,       // routed by size class (v1: tracked malloc)
    Pool,           // fixed-size blocks
    Arena,          // bump allocation
    Stack,          // LIFO
    Heap,           // general tracked heap
    SmallObject,    // reserved (v1: routed to tracked heap)
    LargeObject,    // reserved (v1: routed to tracked heap)
    Shared          // reserved (IPC regions; v1: not yet available)
};

// ---------------------------------------------------------------------------
// Concrete allocators (docs/specs/11 §4)
// ---------------------------------------------------------------------------
class PoolAllocator {
public:
    explicit PoolAllocator(size_t blockSize, size_t prealloc = 1024);
    ~PoolAllocator();
    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    void* Allocate();
    void Release(void* ptr);
    size_t BlockSize() const noexcept { return blockSize_; }

private:
    struct Node { Node* next; };
    void Grow();
    size_t blockSize_;
    size_t prealloc_;
    std::mutex mutex_;
    std::vector<char*> chunks_;
    Node* freeList_ = nullptr;
};

class ArenaAllocator {
public:
    explicit ArenaAllocator(size_t initialSize = 64 * 1024);
    ~ArenaAllocator();
    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    void* Allocate(size_t bytes, size_t align = 16);
    void Reset();
    size_t Used() const noexcept;
    size_t Capacity() const noexcept;
    size_t ChunkCount() const noexcept;
    size_t TrimEmpty();      // frees trailing empty chunks; returns freed bytes

private:
    struct Chunk { size_t size; size_t offset; char* data; };
    void* AllocInChunk(Chunk& c, size_t bytes, size_t align);
    std::vector<Chunk> chunks_;
    size_t initialSize_;
    mutable std::mutex mutex_;
};

class StackAllocator {
public:
    explicit StackAllocator(size_t capacity = 1024 * 1024);
    ~StackAllocator();
    StackAllocator(const StackAllocator&) = delete;
    StackAllocator& operator=(const StackAllocator&) = delete;

    void* Allocate(size_t bytes, size_t align = 16);
    void FreeTo(size_t marker);   // LIFO rollback
    void Reset();
    size_t Marker() const noexcept;
    size_t Used() const noexcept;

private:
    size_t capacity_;
    size_t top_ = 0;
    char* data_;
    mutable std::mutex mutex_;
};

// Fixed-block cache allocator (11 §Cache): reuses blocks of one size class to
// avoid churn on the global heap.
class CacheAllocator {
public:
    explicit CacheAllocator(size_t blockSize = 256, size_t maxCachedBlocks = 4096);
    ~CacheAllocator();
    CacheAllocator(const CacheAllocator&) = delete;
    CacheAllocator& operator=(const CacheAllocator&) = delete;

    void* Allocate();
    void Release(void* ptr);
    size_t InUse() const noexcept { return inUse_.load(); }
    size_t Cached() const;
    size_t BlockSize() const noexcept { return blockSize_; }

private:
    struct Node { Node* next; };
    size_t blockSize_;
    size_t maxCached_;
    mutable std::mutex mutex_;
    Node* freeList_ = nullptr;
    size_t cached_ = 0;
    std::atomic<size_t> inUse_{0};
};

// ---------------------------------------------------------------------------
// MemoryManager (docs/specs/11)
// ---------------------------------------------------------------------------
class MemoryManager final : public IService {
public:
    static MemoryManager& Instance();

    Result<void> Initialize() override;

    // Tagged allocation; returns the aligned user pointer. null on failure.
    Result<void*> Allocate(MemoryTag tag, size_t bytes, size_t alignment = 16,
                           AllocatorKind kind = AllocatorKind::Auto);
    Result<void> Release(MemoryTag tag, void* ptr, size_t bytes = 0);

    template <class T, class... Args>
    T* New(MemoryTag tag, Args&&... args) {
        auto res = Allocate(tag, sizeof(T), alignof(T), AllocatorKind::Auto);
        if (!res.ok()) return nullptr;
        return new (res.value()) T(std::forward<Args>(args)...);
    }
    template <class T>
    void Delete(MemoryTag tag, T* ptr) {
        if (!ptr) return;
        ptr->~T();
        Release(tag, ptr);
    }

    Result<void> SetLimit(MemoryTag tag, size_t maxBytes);
    std::optional<size_t> Limit(MemoryTag tag) const;

    // Cleanup policies (11 §Cleanup): releases empty arena chunks back to the OS.
    Result<void> Cleanup(MemoryTag tag);

    // Fragmentation analysis (11 §Fragmentation).
    struct FragmentationStats {
        double ratio = 0.0;        // 1 - used/capacity across the managed arenas
        size_t arenaChunks = 0;
        size_t wastedBytes = 0;
    };
    FragmentationStats Fragmentation() const;

    struct TagStats {
        uint64_t liveBytes = 0;
        uint64_t allocations = 0;
        uint64_t frees = 0;
    };
    TagStats Stats(MemoryTag tag) const;
    uint64_t TotalLiveBytes() const;
    std::vector<std::pair<MemoryTag, TagStats>> Snapshot() const;
    Result<void> CheckLeaks() const;       // Error(Memory_Leak) if live allocations remain

    PoolAllocator& Pool() { return pool_; }
    ArenaAllocator& Arena() { return arena_; }
    StackAllocator& Stack() { return stack_; }
    CacheAllocator& Cache() { return cache_; }

    const char* ServiceName() const noexcept override { return "MemoryManager"; }
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const;

private:
    MemoryManager() = default;

    struct BlockHeader {
        MemoryTag tag;
        size_t size;
        void* raw;
    };

    mutable std::mutex mutex_;
    std::unordered_map<void*, BlockHeader> live_;                     // guarded by mutex_
    std::unordered_map<MemoryTag, TagStats> stats_;                   // guarded by mutex_
    std::unordered_map<MemoryTag, size_t> limits_;                    // guarded by mutex_
    std::atomic<bool> initialized_{false};

    PoolAllocator pool_{128, 4096};
    ArenaAllocator arena_{64 * 1024};
    StackAllocator stack_{1024 * 1024};
    CacheAllocator cache_{256, 4096};
    static constexpr size_t kLargeThreshold = 1024 * 1024;
};

} // namespace bps
