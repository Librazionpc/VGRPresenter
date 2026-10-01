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

    // Inbound firewall access for a feature that needs other machines to
    // CONNECT TO this one (NDI sending is the first caller: receiving is
    // outbound-only and works through any default firewall, but sending
    // needs an inbound-allow rule, which every desktop OS blocks new apps
    // for by default). Best effort and platform-specific under the hood
    // (Windows: the Windows Firewall rule set, with a UAC-elevated fallback
    // when a silent add isn't possible; Linux: ufw/firewalld, detected only
    // — no per-app rule automation, see RequestInboundAccess's own comment;
    // macOS: no PAL firewall backend exists yet, so this honestly reports
    // Unavailable there rather than pretending to check).
    enum class FirewallAccess { Allowed, NotAllowed, Unavailable };

    // Read-only: does an inbound-allow rule for `appPath` already exist?
    // Never prompts/elevates — safe to call on every launch.
    virtual FirewallAccess ProbeInboundAccess(const std::string& appPath,
                                              const std::string& ruleName) const = 0;

    // Outcome of RequestInboundAccess: Added = access confirmed right now
    // (already had it, or a silent/non-elevated add just worked). Prompted
    // = the platform's own interactive elevation (Windows UAC) was raised
    // but hasn't resolved yet — the caller polls ProbeInboundAccess to learn
    // the verdict once the user answers it. Denied/Failed/Unavailable are
    // terminal: nothing further will happen without the user acting outside
    // this app (their own firewall settings).
    enum class FirewallRequestOutcome { Added, Prompted, Denied, Failed, Unavailable };

    // Adds the inbound-allow rule ONLY when the probe says it's missing —
    // genuinely idempotent, never duplicates a rule or re-raises a prompt
    // on repeat calls once access is confirmed. `diag` (optional) collects
    // one line per attempt with what it actually did — the elevated step's
    // OS-level refusals are fire-and-forget, so without this the caller
    // can't tell why a request didn't land.
    virtual FirewallRequestOutcome RequestInboundAccess(const std::string& appPath,
                                                        const std::string& ruleName,
                                                        const std::string& description,
                                                        std::string* diag = nullptr) = 0;
};

} // namespace bps::platform
