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