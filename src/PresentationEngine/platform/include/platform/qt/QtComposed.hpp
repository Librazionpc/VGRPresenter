#pragma once

#include "platform/IEnvironment.hpp"
#include "platform/IInput.hpp"
#include "platform/INetwork.hpp"
#include "platform/INotifications.hpp"

namespace bps::platform {

// QT FIRST, NATIVE FOR THE REMAINDER.
//
// These four services used to exist once per OS (Windows IP Helper / Linux /proc + sysfs / macOS
// getifaddrs + AF_LINK; three notification mechanisms; three input enumerators; three environment
// readers). Qt answers most of each question identically on every OS, so the answer now comes from ONE
// Qt implementation, and the per-OS class (passed in as `native`) is asked only for what Qt cannot say:
//
//   QtNetwork         adapters, MACs, addresses, hostname, internet reachability      <- QNetworkInterface
//                     default gateway, DNS servers, firewall rule, proxy from the env  <- native
//   QtEnvironment     OS name/version/arch, hostname, user, locale, time zone, cores    <- QSysInfo, QLocale
//                     CPU model, GPU name, total RAM, OS build id                       <- native
//   QtInput           keyboards, mice, touch, pen (with Qt's own names)                 <- QInputDevice
//                     gamepads and /dev/input device nodes                              <- native
//   QtNotifications   the desktop notification                                          <- the platform theme's tray icon
//                     (toast / Notification Center / freedesktop)  — native when the theme has no tray
//
// If Qt has nothing to say (no GUI application yet, an empty list) the native answer is returned
// unchanged, so a service never reports LESS than it did before.

class QtNetwork final : public INetwork {
public:
    explicit QtNetwork(INetwork& native) : native_(native) {}
    std::string Hostname() const override;
    std::vector<NetworkAdapterInfo> Adapters() const override;
    Result<std::string> IpAddress() const override;
    Result<bool> InternetAvailable() const override;
    Result<std::string> Gateway() const override { return native_.Gateway(); }
    std::vector<std::string> DnsServers() const override { return native_.DnsServers(); }
    std::string Proxy() const override { return native_.Proxy(); }
    FirewallAccess ProbeInboundAccess(const std::string& appPath, const std::string& ruleName) const override {
        return native_.ProbeInboundAccess(appPath, ruleName);
    }
    FirewallRequestOutcome RequestInboundAccess(const std::string& appPath, const std::string& ruleName,
                                                const std::string& description, std::string* diag) override {
        return native_.RequestInboundAccess(appPath, ruleName, description, diag);
    }

private:
    INetwork& native_;
};

class QtEnvironment final : public IEnvironment {
public:
    explicit QtEnvironment(IEnvironment& native) : native_(native) {}
    EnvironmentInfo Current() const override;

private:
    IEnvironment& native_;
};

class QtInput final : public IInput {
public:
    explicit QtInput(IInput& native) : native_(native) {}
    std::vector<InputDeviceInfo> Enumerate() const override;
    bool HasKeyboard() const override;
    bool HasPointer() const override;

private:
    IInput& native_;
};

class QtNotifications final : public INotifications {
public:
    explicit QtNotifications(INotifications& native) : native_(native) {}
    bool Supported() const override;
    Result<void> Show(std::string_view title, std::string_view body) override;

private:
    INotifications& native_;
};

} // namespace bps::platform
