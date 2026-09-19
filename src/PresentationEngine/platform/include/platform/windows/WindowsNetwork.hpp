#pragma once

// Windows PAL backend for the network subsystem (gethostname,
// GetAdaptersAddresses — no raw sockets here; those live in WindowsSocket).

#include "platform/INetwork.hpp"

namespace bps::platform {

class WindowsNetwork final : public INetwork {
public:
    std::string Hostname() const override;
    std::vector<NetworkAdapterInfo> Adapters() const override;
    Result<std::string> IpAddress() const override;
    Result<bool> InternetAvailable() const override;
    Result<std::string> Gateway() const override;
    std::vector<std::string> DnsServers() const override;
    std::string Proxy() const override;
};

} // namespace bps::platform
