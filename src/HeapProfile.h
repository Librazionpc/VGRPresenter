#pragma once
// Opt-in allocation profiler for development builds — see HeapProfile.cpp. A no-op unless VGR_HEAP_PROFILE=1.
namespace heapprofile {
#if defined(VGR_ENABLE_SELFTEST) && defined(_WIN32)
void start();
#else
inline void start() {}
#endif
}
