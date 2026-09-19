#include "core/memory/MemoryManager.hpp"

#include "core/logging/Logger.hpp"

#include <cstdlib>
#include <format>

namespace bps {

// ---------------------------------------------------------------------------
// PoolAllocator
// ---------------------------------------------------------------------------
PoolAllocator::PoolAllocator(size_t blockSize, size_t prealloc)
    : blockSize_(blockSize), prealloc_(prealloc < 1 ? 1 : prealloc) {}

PoolAllocator::~PoolAllocator() {
    for (char* c : chunks_) std::free(c);
}

void* PoolAllocator::Allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!freeList_) Grow();
    Node* n = freeList_;
    freeList_ = n->next;
    return n;
}

void PoolAllocator::Release(void* ptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    Node* n = static_cast<Node*>(ptr);
    n->next = freeList_;
    freeList_ = n;
}

void PoolAllocator::Grow() {
    const size_t chunkBytes = prealloc_ * blockSize_;
    char* chunk = static_cast<char*>(std::malloc(chunkBytes));
    if (!chunk) return;
    chunks_.push_back(chunk);
    for (size_t i = 0; i < prealloc_; ++i) {
        Node* n = reinterpret_cast<Node*>(chunk + i * blockSize_);
        n->next = freeList_;
        freeList_ = n;
    }
}

// ---------------------------------------------------------------------------
// ArenaAllocator
// ---------------------------------------------------------------------------
ArenaAllocator::ArenaAllocator(size_t initialSize) : initialSize_(initialSize) {}

ArenaAllocator::~ArenaAllocator() {
    for (auto& c : chunks_) std::free(c.data);
}

void* ArenaAllocator::AllocInChunk(Chunk& c, size_t bytes, size_t align) {
    uintptr_t p = reinterpret_cast<uintptr_t>(c.data) + c.offset;
    uintptr_t aligned = (p + align - 1) & ~(align - 1);
    uintptr_t used = aligned - reinterpret_cast<uintptr_t>(c.data);
    if (used + bytes > c.size) return nullptr;
    c.offset = used + bytes;
    return reinterpret_cast<void*>(aligned);
}

void* ArenaAllocator::Allocate(size_t bytes, size_t align) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : chunks_) {
        if (void* p = AllocInChunk(c, bytes, align)) return p;
    }
    size_t sz = bytes + align > initialSize_ ? bytes + align : initialSize_;
    char* data = static_cast<char*>(std::malloc(sz));
    if (!data) return nullptr;
    chunks_.push_back(Chunk{sz, 0, data});
    return AllocInChunk(chunks_.back(), bytes, align);
}

void ArenaAllocator::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : chunks_) std::free(c.data);
    chunks_.clear();
}

size_t ArenaAllocator::Used() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t used = 0;
    for (const auto& c : chunks_) used += c.offset;
    return used;
}

size_t ArenaAllocator::Capacity() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t cap = 0;
    for (const auto& c : chunks_) cap += c.size;
    return cap;
}

size_t ArenaAllocator::ChunkCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunks_.size();
}

// Releases trailing chunks that are completely empty (11 §Cleanup policies).
size_t ArenaAllocator::TrimEmpty() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t freed = 0;
    for (size_t i = chunks_.size(); i > 1; --i) {   // keep the first chunk to avoid churn
        Chunk& c = chunks_[i - 1];
        if (c.offset == 0) {
            std::free(c.data);
            freed += c.size;
            chunks_.erase(chunks_.begin() + static_cast<std::ptrdiff_t>(i - 1));
        }
    }
    return freed;
}

// ---------------------------------------------------------------------------
// StackAllocator
// ---------------------------------------------------------------------------
StackAllocator::StackAllocator(size_t capacity)
    : capacity_(capacity), data_(static_cast<char*>(std::malloc(capacity))) {}

StackAllocator::~StackAllocator() { std::free(data_); }

void* StackAllocator::Allocate(size_t bytes, size_t align) {
    std::lock_guard<std::mutex> lock(mutex_);
    uintptr_t p = reinterpret_cast<uintptr_t>(data_) + top_;
    uintptr_t aligned = (p + align - 1) & ~(align - 1);
    if (aligned + bytes > reinterpret_cast<uintptr_t>(data_) + capacity_) return nullptr;
    top_ = static_cast<size_t>(aligned - reinterpret_cast<uintptr_t>(data_)) + bytes;
    return reinterpret_cast<void*>(aligned);
}

void StackAllocator::FreeTo(size_t marker) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (marker <= top_) top_ = marker;
}

void StackAllocator::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    top_ = 0;
}

size_t StackAllocator::Marker() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return top_;
}

size_t StackAllocator::Used() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return top_;
}

// ---------------------------------------------------------------------------
// CacheAllocator
// ---------------------------------------------------------------------------
CacheAllocator::CacheAllocator(size_t blockSize, size_t maxCachedBlocks)
    : blockSize_(blockSize < 16 ? 16 : blockSize), maxCached_(maxCachedBlocks) {}

CacheAllocator::~CacheAllocator() {
    std::lock_guard<std::mutex> lock(mutex_);
    Node* n = freeList_;
    while (n) {
        Node* next = n->next;
        std::free(n);
        n = next;
    }
}

void* CacheAllocator::Allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (freeList_) {
        Node* n = freeList_;
        freeList_ = n->next;
        --cached_;
        inUse_.fetch_add(1);
        return n;
    }
    inUse_.fetch_add(1);
    return std::malloc(blockSize_);
}

void CacheAllocator::Release(void* ptr) {
    if (!ptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (cached_ < maxCached_) {
        Node* n = static_cast<Node*>(ptr);
        n->next = freeList_;
        freeList_ = n;
        ++cached_;
    } else {
        std::free(ptr);
    }
    inUse_.fetch_sub(1);
}

size_t CacheAllocator::Cached() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cached_;
}

// ---------------------------------------------------------------------------
// MemoryManager
// ---------------------------------------------------------------------------
MemoryManager& MemoryManager::Instance() {
    static MemoryManager instance;
    return instance;
}

Result<void> MemoryManager::Initialize() {
    initialized_.store(true);
    return Ok();
}

Result<void*> MemoryManager::Allocate(MemoryTag tag, size_t bytes, size_t alignment,
                                      AllocatorKind kind) {
    if (!initialized_.load())
        return Error::Make(Err::InvalidState, "MemoryManager", "not initialized");
    if (bytes == 0) bytes = 1;
    if (alignment < 16) alignment = 16;
    if ((alignment & (alignment - 1)) != 0)
        return Error::Make(Err::InvalidArgument, "MemoryManager", "alignment must be a power of two");

    switch (kind) {
        case AllocatorKind::Pool:
            if (void* p = pool_.Allocate()) return p;
            return Error::Make(Err::OutOfMemory, "MemoryManager", "pool allocation failed");
        case AllocatorKind::Arena:
            if (void* p = arena_.Allocate(bytes, alignment)) return p;
            return Error::Make(Err::OutOfMemory, "MemoryManager", "arena allocation failed");
        case AllocatorKind::Stack:
            if (void* p = stack_.Allocate(bytes, alignment)) return p;
            return Error::Make(Err::OutOfMemory, "MemoryManager", "stack allocation failed");
        default:
            break;  // Auto/Heap/SmallObject/LargeObject -> tracked malloc path
    }

    // The block carries no embedded header; `live_` records {tag, size, raw} for the
    // user pointer, so `raw` can be recovered on release. (Writing a std::string into
    // raw malloc'd memory via operator= without constructing it is UB, so we avoid
    // an in-block header entirely.)
    const size_t total = bytes + alignment;
    void* raw = std::malloc(total);
    if (!raw)
        return Error::Make(Err::OutOfMemory, "MemoryManager", "malloc failed for tag " + tag);

    uintptr_t addr = reinterpret_cast<uintptr_t>(raw);
    uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);
    void* user = reinterpret_cast<void*>(aligned);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto lim = limits_.find(tag);
        if (lim != limits_.end() && stats_[tag].liveBytes + bytes > lim->second) {
            std::free(raw);
            return Error::Make(Err::Memory_LimitExceeded, "MemoryManager",
                               std::format("tag '{}' exceeds its limit of {} bytes", tag,
                                            lim->second));
        }
        live_.emplace(user, BlockHeader{tag, bytes, raw});
        auto& st = stats_[tag];
        st.liveBytes += bytes;
        st.allocations++;
    }
    return user;
}

Result<void> MemoryManager::Release(MemoryTag tag, void* ptr, size_t) {
    if (!ptr) return Error::Make(Err::InvalidArgument, "MemoryManager", "null pointer");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = live_.find(ptr);
    if (it == live_.end())
        return Error::Make(Err::Memory_BadFree, "MemoryManager",
                           "pointer not tracked (released twice, or allocated via a concrete allocator)");
    const BlockHeader& h = it->second;
    if (h.tag != tag)
        return Error::Make(Err::Memory_BadFree, "MemoryManager",
                           "tag mismatch: expected '" + h.tag + "' got '" + tag + "'");
    std::free(h.raw);
    auto& st = stats_[h.tag];
    st.liveBytes -= h.size;
    st.frees++;
    live_.erase(it);
    return Ok();
}

Result<void> MemoryManager::SetLimit(MemoryTag tag, size_t maxBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    limits_[tag] = maxBytes;
    return Ok();
}

std::optional<size_t> MemoryManager::Limit(MemoryTag tag) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = limits_.find(tag);
    return it == limits_.end() ? std::nullopt : std::optional<size_t>(it->second);
}

Result<void> MemoryManager::Cleanup(MemoryTag) {
    // v1 cleanup policy (11 §Cleanup): release fully-empty arena chunks to the OS.
    (void)arena_.TrimEmpty();
    return Ok();
}

MemoryManager::FragmentationStats MemoryManager::Fragmentation() const {
    FragmentationStats s;
    size_t used = arena_.Used();
    size_t cap = arena_.Capacity();
    s.arenaChunks = arena_.ChunkCount();
    s.wastedBytes = cap > used ? cap - used : 0;
    s.ratio = cap > 0 ? 1.0 - static_cast<double>(used) / static_cast<double>(cap) : 0.0;
    return s;
}

MemoryManager::TagStats MemoryManager::Stats(MemoryTag tag) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = stats_.find(tag);
    return it == stats_.end() ? TagStats{} : it->second;
}

uint64_t MemoryManager::TotalLiveBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = 0;
    for (const auto& [tag, st] : stats_) total += st.liveBytes;
    return total;
}

std::vector<std::pair<MemoryTag, MemoryManager::TagStats>> MemoryManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<MemoryTag, TagStats>> out;
    out.reserve(stats_.size());
    for (const auto& [tag, st] : stats_) out.emplace_back(tag, st);
    return out;
}

Result<void> MemoryManager::CheckLeaks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (live_.empty()) return Ok();
    std::map<MemoryTag, uint64_t> byTag;
    for (const auto& [ptr, h] : live_) byTag[h.tag] += h.size;
    std::string msg = "leaks detected: ";
    for (const auto& [t, b] : byTag) msg += std::format("{}={}B, ", t, b);
    return Error::Make(Err::Memory_Leak, "MemoryManager", msg);
}

HealthReport MemoryManager::GetHealth() const {
    HealthReport r;
    r.state = initialized_.load() ? HealthState::Healthy : HealthState::Failing;
    r.detail = std::format("{} live bytes across {} allocations", TotalLiveBytes(),
                           live_.size());
    return r;
}

Metrics MemoryManager::MetricsSnapshot() const {
    Metrics m;
    m.ramBytes = TotalLiveBytes();
    m.health = GetHealth().state;
    return m;
}

} // namespace bps
