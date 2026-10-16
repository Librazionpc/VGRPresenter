// FIXTURE (never compiled — see README.md). The other half of the cross-TU pair:
// this is where the blocking call actually sits, one translation unit away from
// the locked line that pays for it.
#include <mutex>

namespace fixture {

class CrossTuEngine
{
public:
    void SaveStoreLocked();

private:
    std::mutex mutex_;
};

void CrossTuEngine::SaveStoreLocked()
{
    // The guard has to follow a call across the file boundary to see this.
    auto &platform = PlatformAccessor::Get();
    (void)platform;
}

} // namespace fixture
