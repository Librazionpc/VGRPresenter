#pragma once

// PAL network subsystem (Phase 2): identity (hostname/IP/MAC) and connectivity
// probes. Used by the remote/NDI/streaming modules to describe the machine and
// by the ResourceManager to sample network pressure.

#include "core/common/Common.hpp"

#include <string>
#include <vector>

namespace bps::platform {

struct NetworkAdapterInfo {
    std::string name;              // e.g. "eth0", "wlan0", "lo"
    std::string mac;               // "aa:bb:cc:dd:ee:ff" ("" if none)
    std::vector<std::string> ipv4; // addresses
    std::vector<std::string> ipv6;
    bool isUp = false;
    bool isLoopback = false;
};

class INetwork {
public:
    virtual ~INetwork() = default;

    virtual std::string Hostname() const = 0;
    virtual std::vector<NetworkAdapterInfo> Adapters() const = 0;

    // First non-loopback IPv4 address, or an Error.
    virtual Result<std::string> IpAddress() const = 0;

    // Best-effort internet availability (default-route present / DNS reachable).
    virtual Result<bool> InternetAvailable() const = 0;

    // First default-route gateway IPv4 address, or an Error when none is set.
    virtual Result<std::string> Gateway() const = 0;

    // DNS resolver addresses (system resolv.conf), best effort; empty when none.
    virtual std::vector<std::string> DnsServers() const = 0;

    // HTTP(S) proxy from the environment (http_proxy / https_proxy / all_proxy),
    // or an empty string when unset.
    virtual std::string Proxy() const = 0;
};

} // namespace bps::platform
