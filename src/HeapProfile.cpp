// HeapProfile — "where did the memory go?" for a development build (VGR_HEAP_PROFILE=1, Windows only).
//
// Idle memory is mostly ordinary heap, and a heap hides its owners: Task Manager says 1 GB, a region walk says "heap
// segments", and neither names a function. This hooks HeapAlloc / HeapReAlloc / HeapFree (and the ntdll Rtl* twins) in
// every loaded module by patching their import tables — so Qt, our engine, the C++ runtime AND the GPU / NDI / codec DLLs
// are all seen — records the call stack of each allocation that is still alive, and at the requested moments writes the
// stacks holding the most live bytes to <cwd>/logs/heapprofile_<seconds>s.txt.
//
//   VGR_HEAP_PROFILE=1                  start at main()
//   VGR_HEAP_PROFILE_AT=3,10,30         seconds after start at which to write a report (default 10)
//
// Frames are written as `module+0xRVA`; tools/heap_profile_report.mjs turns the exe's into function names with nm.
// It slows the app (a stack walk per allocation) — use it to find a number, never to measure speed.
#ifdef VGR_ENABLE_SELFTEST
#ifdef _WIN32

#include "HeapProfile.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kFrames = 26;
constexpr size_t kPtrCap = 1u << 24;        // 16M live allocations tracked
constexpr size_t kStackCap = 1u << 16;      // 65k distinct stacks

struct PtrSlot { uintptr_t ptr; uint32_t size; uint32_t stack; };   // ptr 0 = empty, 1 = tombstone
struct StackRec {
    uint64_t hash;
    void* frames[kFrames];
    uint32_t depth;
    uint64_t liveBytes, liveCount;
};

PtrSlot* gPtrs = nullptr;
StackRec* gStacks = nullptr;
uint32_t gStackCount = 0;
SRWLOCK gLock = SRWLOCK_INIT;
DWORD gTls = TLS_OUT_OF_INDEXES;
std::atomic<bool> gReady{false};
uint64_t gTrackedLive = 0, gOverflow = 0;
std::atomic<DWORD> gOwner{0};
std::atomic<bool> gStuck{false};

// A lock that gives up: a profiler must never be the reason the app freezes. After two seconds it records who held it
// (logs/heapprofile_stuck.txt) and the caller skips its bookkeeping.
bool LockBooks() {
    if (TryAcquireSRWLockExclusive(&gLock)) { gOwner.store(GetCurrentThreadId()); return true; }
    const ULONGLONG t0 = GetTickCount64();
    while (!TryAcquireSRWLockExclusive(&gLock)) {
        if (GetTickCount64() - t0 > 2000) {
            if (!gStuck.exchange(true)) {
                if (FILE* f = std::fopen("logs\\heapprofile_stuck.txt", "w")) {
                    std::fprintf(f, "lock held by thread %lu, waited on by thread %lu\n", static_cast<unsigned long>(gOwner.load()),
                                 static_cast<unsigned long>(GetCurrentThreadId()));
                    std::fclose(f);
                }
            }
            return false;
        }
        YieldProcessor();
        SwitchToThread();
    }
    gOwner.store(GetCurrentThreadId());
    return true;
}
void UnlockBooks() { gOwner.store(0); ReleaseSRWLockExclusive(&gLock); }

using AllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
using ReallocFn = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
using FreeFn = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID);
using RtlAllocFn = PVOID(NTAPI*)(PVOID, ULONG, SIZE_T);
using RtlReallocFn = PVOID(NTAPI*)(PVOID, ULONG, PVOID, SIZE_T);
using RtlFreeFn = BOOLEAN(NTAPI*)(PVOID, ULONG, PVOID);
using VAFn = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
using VFFn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);
AllocFn gRealAlloc;
ReallocFn gRealRealloc;
FreeFn gRealFree;
RtlAllocFn gRealRtlAlloc;
RtlReallocFn gRealRtlRealloc;
RtlFreeFn gRealRtlFree;
VAFn gRealVA;
VFFn gRealVF;

size_t PtrHash(uintptr_t p) { return static_cast<size_t>((p >> 4) * 0x9E3779B97F4A7C15ull >> 20) & (kPtrCap - 1); }

uint32_t StackId(void** frames, uint32_t depth) {   // lock held
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < depth; ++i) { h ^= reinterpret_cast<uintptr_t>(frames[i]); h *= 1099511628211ull; }
    // open addressing over the stack table by hash
    size_t idx = static_cast<size_t>(h) & (kStackCap - 1);
    for (size_t probe = 0; probe < kStackCap; ++probe, idx = (idx + 1) & (kStackCap - 1)) {
        StackRec& s = gStacks[idx];
        if (s.depth == 0) {
            s.hash = h; s.depth = depth ? depth : 1;
            std::memcpy(s.frames, frames, sizeof(void*) * depth);
            ++gStackCount;
            return static_cast<uint32_t>(idx);
        }
        if (s.hash == h) return static_cast<uint32_t>(idx);
    }
    return 0;
}

void Track(void* p, SIZE_T size, bool virtualAlloc = false) {
    if (!p || !gReady.load(std::memory_order_acquire)) return;
    if (TlsGetValue(gTls)) return;                 // an allocation made by the profiler itself
    TlsSetValue(gTls, reinterpret_cast<LPVOID>(1));
    void* frames[kFrames + 2];
    USHORT n = CaptureStackBackTrace(2, kFrames - 1, frames + 1, nullptr);
    frames[0] = virtualAlloc ? reinterpret_cast<void*>(uintptr_t{1}) : frames[1];   // 1 = a VirtualAlloc block, not a heap block
    if (!virtualAlloc) { std::memmove(frames, frames + 1, sizeof(void*) * n); }
    else ++n;
    if (!LockBooks()) { TlsSetValue(gTls, nullptr); return; }
    const uint32_t sid = StackId(frames, n);
    size_t i = PtrHash(reinterpret_cast<uintptr_t>(p));
    size_t placed = kPtrCap;
    for (size_t probe = 0; probe < 4096; ++probe, i = (i + 1) & (kPtrCap - 1)) {
        if (gPtrs[i].ptr == 0 || gPtrs[i].ptr == 1 || gPtrs[i].ptr == reinterpret_cast<uintptr_t>(p)) { placed = i; break; }
    }
    if (placed == kPtrCap) { ++gOverflow; }
    else {
        gPtrs[placed] = { reinterpret_cast<uintptr_t>(p), static_cast<uint32_t>(std::min<SIZE_T>(size, 0xFFFFFFFFu)), sid };
        gStacks[sid].liveBytes += size; ++gStacks[sid].liveCount; gTrackedLive += size;
    }
    UnlockBooks();
    TlsSetValue(gTls, nullptr);
}

void Untrack(void* p) {
    if (!p || !gReady.load(std::memory_order_acquire)) return;
    if (TlsGetValue(gTls)) return;                 // the report thread frees its own vectors while it holds the lock
    if (!LockBooks()) return;
    size_t i = PtrHash(reinterpret_cast<uintptr_t>(p));
    for (size_t probe = 0; probe < 4096; ++probe, i = (i + 1) & (kPtrCap - 1)) {
        if (gPtrs[i].ptr == reinterpret_cast<uintptr_t>(p)) {
            StackRec& s = gStacks[gPtrs[i].stack];
            s.liveBytes -= gPtrs[i].size; --s.liveCount; gTrackedLive -= gPtrs[i].size;
            gPtrs[i].ptr = 1;
            break;
        }
        if (gPtrs[i].ptr == 0) break;
    }
    UnlockBooks();
}

LPVOID WINAPI HookAlloc(HANDLE h, DWORD f, SIZE_T n) { LPVOID p = gRealAlloc(h, f, n); Track(p, n); return p; }
LPVOID WINAPI HookRealloc(HANDLE h, DWORD f, LPVOID old, SIZE_T n) {
    LPVOID p = gRealRealloc(h, f, old, n);
    if (p) { Untrack(old); Track(p, n); }
    return p;
}
BOOL WINAPI HookFree(HANDLE h, DWORD f, LPVOID p) { Untrack(p); return gRealFree(h, f, p); }
PVOID NTAPI HookRtlAlloc(PVOID h, ULONG f, SIZE_T n) { PVOID p = gRealRtlAlloc(h, f, n); Track(p, n); return p; }
PVOID NTAPI HookRtlRealloc(PVOID h, ULONG f, PVOID old, SIZE_T n) {
    PVOID p = gRealRtlRealloc(h, f, old, n);
    if (p) { Untrack(old); Track(p, n); }
    return p;
}
BOOLEAN NTAPI HookRtlFree(PVOID h, ULONG f, PVOID p) { Untrack(p); return gRealRtlFree(h, f, p); }
// Memory a DLL commits straight from the system (a graphics driver's buffers, big Qt images), not through a heap.
LPVOID WINAPI HookVA(LPVOID a, SIZE_T n, DWORD t, DWORD pr) {
    LPVOID p = gRealVA(a, n, t, pr);
    if (p && (t & MEM_COMMIT)) Track(p, n, true);
    return p;
}
BOOL WINAPI HookVF(LPVOID a, SIZE_T n, DWORD t) {
    if (a && (t & (MEM_RELEASE | MEM_DECOMMIT))) Untrack(a);
    return gRealVF(a, n, t);
}

// Point `*slot` at `hook`.
void Patch(void** slot, void* hook) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

void PatchModule(HMODULE mod) {
    auto* base = reinterpret_cast<uint8_t*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        auto* orig = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (; orig->u1.AddressOfData; ++orig, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
            const char* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData)->Name;
            void** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            if (!std::strcmp(name, "HeapAlloc") && *slot != reinterpret_cast<void*>(HookAlloc)) Patch(slot, reinterpret_cast<void*>(HookAlloc));
            else if (!std::strcmp(name, "HeapReAlloc") && *slot != reinterpret_cast<void*>(HookRealloc)) Patch(slot, reinterpret_cast<void*>(HookRealloc));
            else if (!std::strcmp(name, "HeapFree") && *slot != reinterpret_cast<void*>(HookFree)) Patch(slot, reinterpret_cast<void*>(HookFree));
            else if (!std::strcmp(name, "RtlAllocateHeap") && *slot != reinterpret_cast<void*>(HookRtlAlloc)) Patch(slot, reinterpret_cast<void*>(HookRtlAlloc));
            else if (!std::strcmp(name, "RtlReAllocateHeap") && *slot != reinterpret_cast<void*>(HookRtlRealloc)) Patch(slot, reinterpret_cast<void*>(HookRtlRealloc));
            else if (!std::strcmp(name, "VirtualAlloc") && *slot != reinterpret_cast<void*>(HookVA)) Patch(slot, reinterpret_cast<void*>(HookVA));
            else if (!std::strcmp(name, "VirtualFree") && *slot != reinterpret_cast<void*>(HookVF)) Patch(slot, reinterpret_cast<void*>(HookVF));
            else if (!std::strcmp(name, "RtlFreeHeap") && *slot != reinterpret_cast<void*>(HookRtlFree)) Patch(slot, reinterpret_cast<void*>(HookRtlFree));
        }
    }
}

void PatchAllModules() {
    HMODULE mods[2048];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    const size_t n = std::min<size_t>(need / sizeof(HMODULE), 2048);
    for (size_t i = 0; i < n; ++i) PatchModule(mods[i]);
}

std::string ModuleOf(void* addr, uint32_t* rva) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &m) || !m) { *rva = 0; return "?"; }
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* slash = std::strrchr(path, '\\');
    *rva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(m));
    return slash ? slash + 1 : path;
}

void WriteReport(double seconds) {
    TlsSetValue(gTls, reinterpret_cast<LPVOID>(1));   // the report's own allocations are not tracked
    // Copy what is needed under the lock and RELEASE it before touching the loader (GetModuleHandleEx) or the file system:
    // a thread inside a DLL load holds the loader lock and wants this lock to record its allocation.
    std::vector<StackRec> top;
    size_t stacks = 0;
    double liveMb = 0;
    unsigned long long overflow = 0;
    const bool locked = LockBooks();   // if it cannot be had, read the books anyway: a diagnostic, and better than no report
    {
        std::vector<uint32_t> order;
        for (uint32_t i = 0; i < kStackCap; ++i)
            if (gStacks[i].depth && gStacks[i].liveBytes) order.push_back(i);
        std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return gStacks[a].liveBytes > gStacks[b].liveBytes; });
        stacks = order.size();
        for (size_t k = 0; k < std::min<size_t>(order.size(), 400); ++k) top.push_back(gStacks[order[k]]);
        liveMb = static_cast<double>(gTrackedLive) / 1048576.0;
        overflow = gOverflow;
    }
    if (locked) UnlockBooks();
    char name[64];
    std::snprintf(name, sizeof name, "logs\\heapprofile_%gs.txt", seconds);
    CreateDirectoryA("logs", nullptr);
    if (FILE* f = std::fopen(name, "w")) {
        std::fprintf(f, "# heap profile at %g s: tracked live %.1f MB in %zu stacks, overflow=%llu\n", seconds, liveMb, stacks, overflow);
        for (const StackRec& s : top) {
            std::fprintf(f, "S %.2f MB %llu blocks\n", static_cast<double>(s.liveBytes) / 1048576.0,
                         static_cast<unsigned long long>(s.liveCount));
            for (uint32_t d = 0; d < s.depth; ++d) {
                uint32_t rva = 0;
                const std::string m = ModuleOf(s.frames[d], &rva);
                std::fprintf(f, "  %s+0x%X\n", m.c_str(), rva);
            }
        }
        std::fclose(f);
    }
    TlsSetValue(gTls, nullptr);
}

} // namespace

namespace heapprofile {

void start() {
    const char* on = std::getenv("VGR_HEAP_PROFILE");
    if (!on || !*on || *on == '0') return;
    gTls = TlsAlloc();
    gPtrs = static_cast<PtrSlot*>(VirtualAlloc(nullptr, kPtrCap * sizeof(PtrSlot), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    gStacks = static_cast<StackRec*>(VirtualAlloc(nullptr, kStackCap * sizeof(StackRec), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!gPtrs || !gStacks || gTls == TLS_OUT_OF_INDEXES) return;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    // The REAL functions come from ntdll for all six hooks (HeapAlloc(h, f, n) IS RtlAllocateHeap(h, f, n)): kernel32 and
    // kernelbase are themselves patched, so their HeapFree would jump straight back into the hook.
    gRealAlloc = reinterpret_cast<AllocFn>(GetProcAddress(nt, "RtlAllocateHeap"));
    gRealRealloc = reinterpret_cast<ReallocFn>(GetProcAddress(nt, "RtlReAllocateHeap"));
    gRealFree = reinterpret_cast<FreeFn>(GetProcAddress(nt, "RtlFreeHeap"));
    gRealRtlAlloc = reinterpret_cast<RtlAllocFn>(GetProcAddress(nt, "RtlAllocateHeap"));
    gRealRtlRealloc = reinterpret_cast<RtlReallocFn>(GetProcAddress(nt, "RtlReAllocateHeap"));
    gRealRtlFree = reinterpret_cast<RtlFreeFn>(GetProcAddress(nt, "RtlFreeHeap"));
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    gRealVA = reinterpret_cast<VAFn>(GetProcAddress(kb, "VirtualAlloc"));
    gRealVF = reinterpret_cast<VFFn>(GetProcAddress(kb, "VirtualFree"));
    if (!gRealVA || !gRealVF) return;
    if (!gRealAlloc || !gRealRealloc || !gRealFree || !gRealRtlAlloc || !gRealRtlRealloc || !gRealRtlFree) return;
    PatchAllModules();
    gReady.store(true, std::memory_order_release);

    std::vector<double> at;
    if (const char* list = std::getenv("VGR_HEAP_PROFILE_AT")) {
        for (const char* p = list; *p;) { at.push_back(std::atof(p)); while (*p && *p != ',') ++p; if (*p == ',') ++p; }
    }
    if (at.empty()) at.push_back(10);
    std::sort(at.begin(), at.end());
    // One watcher thread: re-patches modules that load later (the GPU drivers arrive seconds in) and writes the reports.
    struct Args { std::vector<double> at; };
    auto* args = new Args{at};
    CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* a = static_cast<Args*>(p);
        TlsSetValue(gTls, reinterpret_cast<LPVOID>(1));   // this thread's own allocations stay out of the books
        const ULONGLONG t0 = GetTickCount64();
        size_t next = 0;
        while (next < a->at.size()) {
            Sleep(100);
            PatchAllModules();
            const double secs = static_cast<double>(GetTickCount64() - t0) / 1000.0;
            if (secs >= a->at[next]) { WriteReport(a->at[next]); ++next; }
        }
        return 0;
    }, args, 0, nullptr);
}

} // namespace heapprofile

#endif // _WIN32
#endif // VGR_ENABLE_SELFTEST
