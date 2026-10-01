#include "platform/linux/LinuxNetwork.hpp"
#include "platform/linux/LinuxExec.hpp"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace bps::platform {

std::string LinuxNetwork::Hostname() const {
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf) - 1) != 0) return {};
    return buf;
}

std::vector<NetworkAdapterInfo> LinuxNetwork::Adapters() const {
    std::vector<NetworkAdapterInfo> out;
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return out;

    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_name || !p->ifa_addr) continue;
        bool isLoopback = (p->ifa_flags & IFF_LOOPBACK) != 0;
        bool isUp = (p->ifa_flags & IFF_UP) != 0;

        NetworkAdapterInfo* slot = nullptr;
        for (auto& a : out) {
            if (a.name == p->ifa_name) {
                slot = &a;
                break;
            }
        }
        if (!slot) {
            out.push_back(NetworkAdapterInfo{});
            slot = &out.back();
            slot->name = p->ifa_name;
            slot->isLoopback = isLoopback;
            slot->isUp = isUp;
            // MAC from sysfs
            std::ifstream macFile("/sys/class/net/" + slot->name + "/address");
            if (macFile) std::getline(macFile, slot->mac);
        }
        slot->isUp = slot->isUp || isUp;

        if (p->ifa_addr->sa_family == AF_INET) {
            char buf[INET_ADDRSTRLEN] = {0};
            auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
            if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
                slot->ipv4.emplace_back(buf);
        } else if (p->ifa_addr->sa_family == AF_INET6) {
            char buf[INET6_ADDRSTRLEN] = {0};
            auto* sin6 = reinterpret_cast<sockaddr_in6*>(p->ifa_addr);
            if (inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf)))
                slot->ipv6.emplace_back(buf);
        }
    }
    freeifaddrs(ifa);
    return out;
}

Result<std::string> LinuxNetwork::IpAddress() const {
    auto adapters = Adapters();
    for (const auto& a : adapters) {
        if (a.isLoopback || !a.isUp) continue;
        if (!a.ipv4.empty()) return a.ipv4.front();
    }
    return Error::Make(Err::NotFound, "Network", "no non-loopback IPv4 address found");
}

Result<bool> LinuxNetwork::InternetAvailable() const {
    return Gateway().ok();
}

Result<std::string> LinuxNetwork::Gateway() const {
    // /proc/net/route: destination 00000000 with a non-zero gateway is the
    // default route; the gateway field is a little-endian hex IPv4.
    std::ifstream route("/proc/net/route");
    if (!route) return Error::Make(Err::IoError, "Network", "cannot read /proc/net/route");
    std::string line;
    std::getline(route, line); // header
    while (std::getline(route, line)) {
        std::istringstream ss(line);
        std::string iface, dest, gateway;
        if (!(ss >> iface >> dest >> gateway)) continue;
        if (dest != "00000000" || gateway == "00000000") continue;
        unsigned long raw = std::stoul(gateway, nullptr, 16);
        char buf[INET_ADDRSTRLEN] = {0};
        in_addr addr{};
        addr.s_addr = static_cast<uint32_t>(raw);
        if (inet_ntop(AF_INET, &addr, buf, sizeof(buf))) return std::string(buf);
    }
    return Error::Make(Err::NotFound, "Network", "no default gateway configured");
}

std::vector<std::string> LinuxNetwork::DnsServers() const {
    std::vector<std::string> out;
    std::ifstream resolv("/etc/resolv.conf");
    std::string line;
    while (std::getline(resolv, line)) {
        std::istringstream ss(line);
        std::string kw, addr;
        if (!(ss >> kw >> addr)) continue;
        if (kw == "nameserver") out.push_back(addr);
    }
    return out;
}

std::string LinuxNetwork::Proxy() const {
    for (const char* var : {"https_proxy", "http_proxy", "all_proxy"}) {
        if (const char* v = std::getenv(var); v && *v) return v;
    }
    return {};
}

// Unlike Windows Firewall, ufw/firewalld rules aren't naturally scoped to
// one application path — they're port/service based, and NDI has no single
// fixed port to name (mDNS discovery + a dynamic range). So this backend
// only answers the question it CAN answer honestly: is any firewall even
// active? Most desktop Linux installs ship with none enabled, which is
// genuinely "nothing blocking inbound" — not a guess dressed up as one.
// When a firewall IS active, this reports Unavailable rather than silently
// opening a broad port range on the user's behalf.
namespace {
bool FirewallActive() {
    using namespace linux_backend;
    if (CommandAvailable("ufw")) {
        const CmdResult r = RunCapture("ufw status 2>/dev/null");
        if (r.exitCode == 0 && r.stdoutText.find("Status: active") != std::string::npos)
            return true;
    }
    if (CommandAvailable("firewall-cmd")) {
        const CmdResult r = RunCapture("firewall-cmd --state 2>/dev/null");
        if (r.exitCode == 0 && r.stdoutText.find("running") != std::string::npos)
            return true;
    }
    return false;
}
} // namespace

INetwork::FirewallAccess LinuxNetwork::ProbeInboundAccess(const std::string& appPath,
                                                           const std::string& ruleName) const {
    (void)appPath;
    (void)ruleName;
    return FirewallActive() ? FirewallAccess::Unavailable : FirewallAccess::Allowed;
}

INetwork::FirewallRequestOutcome LinuxNetwork::RequestInboundAccess(
    const std::string& appPath, const std::string& ruleName, const std::string& description,
    std::string* diag) {
    (void)ruleName;
    (void)description;
    (void)diag;
    if (ProbeInboundAccess(appPath, ruleName) == FirewallAccess::Allowed)
        return FirewallRequestOutcome::Added;
    // No per-app rule automation for ufw/firewalld (see ProbeInboundAccess's
    // own comment on why) — an active firewall here is a terminal Unavailable,
    // not a Denied (nothing was actually refused; nothing was attempted).
    return FirewallRequestOutcome::Unavailable;
}

} // namespace bps::platform
