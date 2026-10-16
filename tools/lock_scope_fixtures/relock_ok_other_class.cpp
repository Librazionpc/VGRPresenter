// FIXTURE (never compiled — see README.md). The caller holds ITS `mutex_` and
// calls another class's method that locks ITS OWN `mutex_`. Same NAME, different
// OBJECT — nothing deadlocks, and this is one of the shapes that made the first
// attempt at the re-lock rule unusable (5 findings, 0 real).
//
// expect: NOTHING
#include <mutex>

namespace fixture {

class Ledger
{
public:
    void Add();

private:
    std::mutex mutex_;
    int entries_ = 0;
};

class OtherClassEngine
{
public:
    void Publish();

private:
    std::mutex mutex_;
    Ledger ledger_;
};

void Ledger::Add()
{
    std::lock_guard<std::mutex> lock(mutex_);   // Ledger::mutex_, not ours
    entries_++;
}

void OtherClassEngine::Publish()
{
    std::lock_guard<std::mutex> lock(mutex_);   // OtherClassEngine::mutex_
    ledger_.Add();      // a DIFFERENT object's lock — not a self-deadlock
}

} // namespace fixture
