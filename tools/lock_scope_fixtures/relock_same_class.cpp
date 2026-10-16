// FIXTURE (never compiled — see README.md). A locked method calls a sibling that
// locks the SAME mutex object: the same thread, so this waits forever on a lock
// it already holds.
//
// expect: self-deadlock: 'Refresh' re-locks 'mutex_'
#include <mutex>

namespace fixture {

class RelockEngine
{
public:
    void Publish();
    void Refresh();

private:
    std::mutex mutex_;
    int revision_ = 0;
};

void RelockEngine::Refresh()
{
    // The re-lock: legal-looking, and fatal when called from Publish().
    std::lock_guard<std::mutex> lock(mutex_);
    revision_++;
}

void RelockEngine::Publish()
{
    std::lock_guard<std::mutex> lock(mutex_);
    Refresh();          // <- self-deadlock (same class, same mutex_)
}

} // namespace fixture
