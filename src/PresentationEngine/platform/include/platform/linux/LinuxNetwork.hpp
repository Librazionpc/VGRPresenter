#pragma once

// Linux PAL backend for the network subsystem (getifaddrs, /proc/net/route).

#include "../INetwork.hpp"

namespace bps::platform {

class LinuxNetwork final : public INetwork {
public:
    std::string Hostname() const override;
    std::vector<NetworkAdapterInfo> Adapters() const override;
    Result<std::string> IpAddress() const override;
    Result<bool> InternetAvailable() const override;
    Result<std::string> Gateway() const override;
    std::vector<std::string> DnsServers() const override;
    std::string Proxy() const override;
    FirewallAccess ProbeInboundAccess(const std::string& appPath,
                                      const std::string& ruleName) const override;
    FirewallRequestOutcome RequestInboundAccess(const std::string& appPath,
                                                const std::string& ruleName,
                                                const std::string& description,
                                                std::string* diag) override;
};

} // namespace bps::platform
