// FIXTURE (never compiled — see README.md). The same shape as
// relock_same_class.cpp, on a RECURSIVE mutex — which exists precisely to let the
// owning thread lock it again. A guard that reported this would be wrong, and a
// first attempt at the rule did exactly that.
//
// expect: NOTHING
#include <mutex>

namespace fixture {

class RecursiveEngine
{
public:
    void Publish();
    void Refresh();

private:
    std::recursive_mutex mutex_;   // re-entrant by construction
    int revision_ = 0;
};

void RecursiveEngine::Refresh()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    revision_++;
}

void RecursiveEngine::Publish()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Refresh();          // legal: the same thread may re-lock a recursive_mutex
}

} // namespace fixture
