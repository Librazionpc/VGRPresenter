#include "platform/PlatformAccessor.hpp"
#include "platform/IPlatform.hpp"

#include <mutex>
#include <stdexcept>

namespace bps::platform {

namespace {
std::shared_ptr<IPlatform>& PlatformSlot() {
    static std::shared_ptr<IPlatform> slot;
    return slot;
}

// Guards every access to the slot. Lifetime contract: Install() is called once
// by the Kernel at boot, before any worker threads exist; afterwards the slot
// is effectively read-only (Get/Shared/Installed). Replacing the backend while
// systems hold Get() references is only safe if no references are live — callers
// that must outlive a swap should hold the Shared() pointer instead.
std::mutex& SlotMutex() {
    static std::mutex m;
    return m;
}
} // namespace

IPlatform& PlatformAccessor::Get() {
    std::lock_guard<std::mutex> lock(SlotMutex());
    auto& slot = PlatformSlot();
    if (!slot) slot = CreatePlatform();   // lazy install on first use
    if (!slot)
        throw std::logic_error("bps::platform: no PAL backend available on this OS");
    return *slot;
}

std::shared_ptr<IPlatform> PlatformAccessor::Shared() {
    std::lock_guard<std::mutex> lock(SlotMutex());
    auto& slot = PlatformSlot();
    if (!slot) slot = CreatePlatform();
    return slot;
}

void PlatformAccessor::Install(std::shared_ptr<IPlatform> platform) {
    std::lock_guard<std::mutex> lock(SlotMutex());
    PlatformSlot() = std::move(platform);
}

bool PlatformAccessor::Installed() {
    std::lock_guard<std::mutex> lock(SlotMutex());
    return PlatformSlot() != nullptr;
}

} // namespace bps::platform
