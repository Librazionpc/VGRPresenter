// FIXTURE (never compiled — see README.md). `TouchStore()` is defined TWICE in
// the scanned scope (ambiguous_one.cpp and ambiguous_two.cpp) and one of those
// two reaches the PAL. A name-based scan cannot say which definition this call
// binds to, so it must stay QUIET here and say so in its summary line
// (`N helper call(s) left unresolved`) instead of guessing — guessing is how a
// guard grows findings nobody can act on.
//
// expect: NOTHING (and the summary reports 1 unresolved helper call)
#include <mutex>

namespace fixture {

class AmbiguousEngine
{
public:
    void Publish();

private:
    std::mutex mutex_;
};

void AmbiguousEngine::Publish()
{
    std::lock_guard<std::mutex> lock(mutex_);
    TouchStore();       // two definitions in scope: unresolved, not reported
}

} // namespace fixture
