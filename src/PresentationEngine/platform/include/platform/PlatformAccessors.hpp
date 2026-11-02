#pragma once

#include "platform/IPlatform.hpp"

namespace bps::platform {

// The subsystem accessors every backend implements identically: each hands back its own
// member. The member types are per-OS, so the base asks its backend for them by name and
// keeps the 19 bodies in one place; a backend that loses a member is a compile error here
// instead of a silent divergence between three copies.
template <class Derived>
class PlatformAccessors : public IPlatform {
public:
    IFilesystem& Filesystem() override { return self().filesystem_; }
    IPaths& Paths() override { return self().paths_; }
    ITimer& Timer() override { return self().timer_; }
    IThreading& Threading() override { return self().threading_; }
    IProcess& Process() override { return self().process_; }
    ILibrary& Library() override { return self().library_; }
    IMonitor& Monitor() override { return self().monitor_; }
    IAudio& Audio() override { return self().audio_; }
    IVideo& Video() override { return self().video_; }
    INetwork& Network() override { return self().network_; }
    IPower& Power() override { return self().power_; }
    IClipboard& Clipboard() override { return self().clipboard_; }
    IEnvironment& Environment() override { return self().environment_; }
    ILocale& Locale() override { return self().locale_; }
    ISocketFactory& Sockets() override { return self().sockets_; }
    IInput& Input() override { return self().input_; }
    IDialogs& Dialogs() override { return self().dialogs_; }
    INotifications& Notifications() override { return self().notifications_; }

private:
    Derived& self() { return *static_cast<Derived*>(this); }
};

} // namespace bps::platform
