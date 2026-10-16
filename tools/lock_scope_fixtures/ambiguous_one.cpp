// FIXTURE (never compiled — see README.md). One of the two definitions of
// TouchStore(); this one reaches the PAL (so "which one does the call bind to?"
// is a question with consequences, and therefore one the scan must not answer).
#include <string>

namespace fixture {

void TouchStore()
{
    auto &platform = PlatformAccessor::Get();
    (void)platform;
}

} // namespace fixture
