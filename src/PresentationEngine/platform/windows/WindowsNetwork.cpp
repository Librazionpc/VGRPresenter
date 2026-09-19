#include "platform/windows/WindowsNetwork.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdlib>
#include <format>
#include <vector>

namespace bps::platform {

namespace {
std::string IpToString(const SOCKET_ADDRESS& addr) {
    if (!addr.lpSockaddr) return {};
    char buf[INET6_ADDRSTRLEN] = {0};
    DWORD len = sizeof(buf);
    if (WSAAddressToStringA(addr.lpSockaddr, addr.iSockaddrLength, nullptr, buf, &len) == 0)
        return buf;
    return {};
}
} // namespace

std::string WindowsNetwork::Hostname() const {
    char buf[256] = {0};
    DWORD len = sizeof(buf);
    if (GetComputerNameA(buf, &len) == 0) return {};
    return buf;
}

std::vector<NetworkAdapterInfo> WindowsNetwork::Adapters() const {
    std::vector<NetworkAdapterInfo> out;
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) ==
            ERROR_BUFFER_OVERFLOW &&
        size > 0) {
        std::vector<IP_ADAPTER_ADDRESSES> buf(size / sizeof(IP_ADAPTER_ADDRESSES) + 2);
        auto* p = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        ULONG rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, p, &size);
        if (rc != ERROR_SUCCESS) return out;
        for (auto* a = p; a; a = a->Next) {
            NetworkAdapterInfo info;
            info.name = a->AdapterName ? a->AdapterName : "";
            info.isLoopback = (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
            info.isUp = (a->OperStatus == IfOperStatusUp);
            if (a->PhysicalAddressLength > 0) {
                std::string mac;
                mac.reserve(static_cast<size_t>(a->PhysicalAddressLength) * 3);
                for (ULONG i = 0; i < a->PhysicalAddressLength && i < 8; ++i) {
                    mac += std::format("{:02x}", a->PhysicalAddress[i]);
                    if (i + 1 < a->PhysicalAddressLength) mac.push_back(':');
                }
                info.mac = mac;
            }
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                std::string ip = IpToString(u->Address);
                if (ip.empty()) continue;
                if (u->Address.lpSockaddr->sa_family == AF_INET)
                    info.ipv4.push_back(ip);
                else if (u->Address.lpSockaddr->sa_family == AF_INET6)
                    info.ipv6.push_back(ip);
            }
            out.push_back(std::move(info));
        }
    }
    return out;
}

Result<std::string> WindowsNetwork::IpAddress() const {
    auto adapters = Adapters();
    for (const auto& a : adapters) {
        if (a.isLoopback || !a.isUp) continue;
        if (!a.ipv4.empty()) return a.ipv4.front();
    }
    return Error::Make(Err::NotFound, "Network", "no non-loopback IPv4 address found");
}

Result<std::string> WindowsNetwork::Gateway() const {
    // First adapter with a default gateway wins (best effort).
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, nullptr, &size) !=
        ERROR_BUFFER_OVERFLOW)
        return Error::Make(Err::NotFound, "Network", "no default gateway configured");
    std::vector<IP_ADAPTER_ADDRESSES> buf(size / sizeof(IP_ADAPTER_ADDRESSES) + 2);
    auto* p = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, p, &size);
    if (rc != ERROR_SUCCESS)
        return Error::Make(Err::NotFound, "Network", "no default gateway configured");
    for (auto* a = p; a; a = a->Next) {
        for (auto* g = a->FirstGatewayAddress; g; g = g->Next) {
            std::string gw = IpToString(g->Address);
            if (!gw.empty()) return gw;
        }
    }
    return Error::Make(Err::NotFound, "Network", "no default gateway configured");
}

std::vector<std::string> WindowsNetwork::DnsServers() const {
    std::vector<std::string> out;
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) !=
        ERROR_BUFFER_OVERFLOW)
        return out;
    std::vector<IP_ADAPTER_ADDRESSES> buf(size / sizeof(IP_ADAPTER_ADDRESSES) + 2);
    auto* p = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, p, &size) !=
        ERROR_SUCCESS)
        return out;
    for (auto* a = p; a; a = a->Next)
        for (auto* d = a->FirstDnsServerAddress; d; d = d->Next) {
            std::string ip = IpToString(d->Address);
            if (!ip.empty()) out.push_back(ip);
        }
    return out;
}

std::string WindowsNetwork::Proxy() const {
    for (const char* var : {"https_proxy", "http_proxy", "all_proxy"}) {
        if (const char* v = std::getenv(var); v && *v) return v;
    }
    return {};
}

Result<bool> WindowsNetwork::InternetAvailable() const { return Gateway().ok(); }

} // namespace bps::platform
