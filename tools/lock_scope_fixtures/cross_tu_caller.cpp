// FIXTURE (never compiled — see README.md). The caller holds its mutex and calls
// a helper that is DEFINED IN ANOTHER TRANSLATION UNIT and reaches the PAL, so
// the blocking work is invisible from this line and from this file.
//
// expect: helper 'SaveStoreLocked' (<other file>:<line>) wraps PAL (PlatformAccessor)
#include <mutex>

namespace fixture {

class CrossTuEngine
{
public:
    void Import();
    int Count() const;

private:
    void SaveStoreLocked();
    std::mutex mutex_;
    int notes_ = 0;
};

void CrossTuEngine::Import()
{
    // The caller looks innocent: a lock and a call to its own helper.
    std::lock_guard<std::mutex> lock(mutex_);
    notes_++;
    SaveStoreLocked();   // <- the PAL write lives inside this, in cross_tu_helper.cpp
}

int CrossTuEngine::Count() const
{
    // No lock: nothing here can be a lock-scope finding.
    return notes_;
}

} // namespace fixture
