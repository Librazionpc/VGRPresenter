#include "platform/windows/WindowsNetwork.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstdio>
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

// Runs `cmd` capturing its stdout, and returns the command's own exit code.
//
// This MUST NOT be _popen: a GUI-subsystem process has no console, so every
// _popen (and cmd.exe it spawns) MATERIALIZES A NEW CONSOLE WINDOW — the
// "Windows terminal flashes + asks for permission every time an output is
// opened" report. netsh probing ran 3+ times per output-open (probe, silent
// add, re-probe) plus a 2 s retry timer, each with its own visible console.
// Instead: CreateProcessW on cmd.exe with CREATE_NO_WINDOW (child console
// created but never shown), stdout+stderr redirected to one pipe, and a
// timeout kill so a wedged netsh can never hang the UI thread that called
// in (the probe runs synchronously).
struct ShellResult {
    std::string output;
    int exitCode = -1;
};
// One netsh line for diagnostics: its errors arrive as a blank line, the
// message, then a full usage dump — the caller's log only needs the message.
std::string FirstLine(const std::string& text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = text.find('\r', start);
    if (end == std::string::npos) end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    if (line.size() > 200) line.resize(200);
    return line;
}

ShellResult RunShellCapture(const std::string& cmd) {
    ShellResult result;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;   // the child needs the write end
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return result;
    // The child inherits the write end; ours must NOT stay inheritable or a
    // grandchild could keep the pipe open and stall the reader after exit.
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;             // belt: hidden regardless of window
    si.hStdOutput = si.hStdError = writeEnd;   // braces: `2>&1` in one pipe
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    // cmd.exe /c: netsh.exe alone would not wrap quoting the same way the
    // previous shell line did; /c preserves the exact command string (and
    // its escaped inner quotes) that the rules add/probe relied on.
    std::wstring cmdline = win::Wide("cmd.exe /c " + cmd);   // mutable: CreateProcessW may write to it
    const BOOL created = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                                        TRUE,   // inherit the pipe handles
                                        CREATE_NO_WINDOW,   // THE fix: no console ever
                                        nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);   // reader side: done with it after spawn
    if (!created) {
        CloseHandle(readEnd);
        return result;
    }
    // Read to EOF (child exit closes its end) with a hard timeout — a
    // synchronous probe must never hang its caller (QML open paths).
    constexpr DWORD kTimeoutMs = 8000;
    const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
    char buf[256];
    DWORD n = 0;
    for (;;) {
        if (!ReadFile(readEnd, buf, sizeof(buf), &n, nullptr)) break;   // EOF or broken pipe
        result.output.append(buf, n);
        if (GetTickCount64() > deadline) break;
    }
    CloseHandle(readEnd);
    // Wait briefly for exit; kill on timeout so nothing wedges the caller.
    if (WaitForSingleObject(pi.hProcess, 1000) == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 1);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exitCode = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return result;
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

// Read-only: `netsh advfirewall firewall show rule name="..."` prints
// "No rules match the specified criteria." when the rule is absent, and the
// rule's own fields (Enabled/Direction/Action/...) when it exists. Scanning
// for that exact phrase is the documented behavior of the command (there is
// no machine-readable exit code for "found vs not found" — both are exit 0),
// so it is the correct probe here, not a fragile parse of the wrong thing.
INetwork::FirewallAccess WindowsNetwork::ProbeInboundAccess(const std::string& appPath,
                                                             const std::string& ruleName) const {
    (void)appPath;   // netsh looks the rule up by name, not by program path
    const std::string cmd = std::format(
        "netsh advfirewall firewall show rule name=\"{}\"", ruleName);
    const ShellResult r = RunShellCapture(cmd);
    if (r.exitCode != 0)
        return FirewallAccess::Unavailable;   // netsh itself unusable
    if (r.output.find("No rules match") != std::string::npos)
        return FirewallAccess::NotAllowed;
    // A rule by this name exists; "Enabled: No" means it was added once and
    // then disabled by hand — treat that as NotAllowed so RequestInboundAccess
    // still offers to (re)add an active one rather than reporting false
    // safety. Column spacing in netsh's own output isn't a documented
    // contract, so find the label and inspect the next non-space token
    // instead of matching a fixed-width string.
    if (const auto pos = r.output.find("Enabled:"); pos != std::string::npos) {
        auto valuePos = r.output.find_first_not_of(" \t", pos + 8);
        if (valuePos != std::string::npos
            && r.output.compare(valuePos, 2, "No") == 0)
            return FirewallAccess::NotAllowed;
    }
    return FirewallAccess::Allowed;
}

// Idempotent: probes first and returns Added without touching anything when
// already allowed — repeat calls (e.g. every app launch) never pile up
// duplicate rules or re-raise a UAC prompt once access is confirmed. Scoped
// to `appPath` (program=) rather than a bare port, so only this executable
// gets opened up, not every app on the machine.
//
// Two-step, same as a normal Windows installer: try a SILENT, non-elevated
// add first (succeeds only when this process already runs elevated — quick,
// no UAC); when that's refused (the expected outcome for a normal user
// session), raise ONE real UAC consent dialog via ShellExecuteEx's "runas"
// verb targeting netsh.exe directly — the standard, documented way to
// elevate a single command (no PowerShell wrapper, no nested quoting to get
// wrong). The elevation is NOT waited on: it returns Prompted immediately
// so the caller's UI thread never blocks on a human, and the caller re-probes
// later to learn the verdict once the user answers the dialog.
INetwork::FirewallRequestOutcome WindowsNetwork::RequestInboundAccess(
    const std::string& appPath, const std::string& ruleName, const std::string& description,
    std::string* diag) {
    const auto note = [diag](const std::string& line) {
        if (!diag) return;
        if (!diag->empty()) diag->push_back('\n');
        *diag += line;
    };
    if (ProbeInboundAccess(appPath, ruleName) == FirewallAccess::Allowed)
        return FirewallRequestOutcome::Added;

    // netsh's parser rejects forward slashes in program= with "The
    // application contains invalid characters, or is an invalid length." —
    // and that refusal fires even in an ELEVATED run, so the UAC-approved
    // add silently did nothing. Qt hands us "C:/..." paths; netsh wants
    // "C:\\...".
    std::string program = appPath;
    std::replace(program.begin(), program.end(), '/', '\\');

    const std::string addCmd = std::format(
        "netsh advfirewall firewall add rule name=\"{}\" dir=in action=allow "
        "program=\"{}\" enable=yes profile=any description=\"{}\"",
        ruleName, program, description);

    // Step 1 — silent, non-elevated attempt.
    if (const ShellResult r = RunShellCapture(addCmd); r.exitCode == 0) {
        note(std::format("silent add: netsh accepted it — {}", FirstLine(r.output)));
        if (ProbeInboundAccess(appPath, ruleName) == FirewallAccess::Allowed)
            return FirewallRequestOutcome::Added;
    } else {
        note(std::format("silent add refused (netsh exit {}): {}", r.exitCode, FirstLine(r.output)));
    }

    // Step 2 — elevate netsh itself directly (no shell/PowerShell in
    // between): lpParameters is ONE level of argument quoting, the same
    // string netsh's own CLI expects.
    const std::wstring params = win::Wide(std::format(
        "advfirewall firewall add rule name=\"{}\" dir=in action=allow "
        "program=\"{}\" enable=yes profile=any description=\"{}\"",
        ruleName, program, description));

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = L"netsh.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        note(std::format("elevated add: ShellExecuteEx failed (Win32 error {})", err));
        // ERROR_CANCELLED: the user clicked "No" on the UAC dialog itself —
        // a real, deliberate refusal, distinct from the prompt failing to
        // raise at all.
        if (err == ERROR_CANCELLED)
            return FirewallRequestOutcome::Denied;
        return FirewallRequestOutcome::Failed;
    }
    note("elevated add: UAC consent raised — verdict lands on the next probe");
    if (sei.hProcess) CloseHandle(sei.hProcess);   // fire-and-forget; poll via ProbeInboundAccess
    return FirewallRequestOutcome::Prompted;
}

} // namespace bps::platform
