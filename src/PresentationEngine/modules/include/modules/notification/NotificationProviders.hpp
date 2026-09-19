#pragma once

// Built-in notification providers (docs/specs/14 §Notification Providers).
// Each is an independent INotificationProvider; a UI frontend adds its own
// Toast/Banner providers without touching the service.

#include "modules/notification/INotificationProvider.hpp"

#include <mutex>
#include <string>

namespace bps::notification {

// Logs every notification through the engine Logger (category "Notify").
// Channel: "console".
class ConsoleNotificationProvider final : public INotificationProvider {
public:
    const char* Name() const noexcept override { return "Console"; }
    std::vector<std::string> SupportedChannels() const override { return {"console"}; }
    ProviderCapabilities Capabilities() const override {
        ProviderCapabilities c;
        c.supportsProgress = true;
        return c;
    }
    Result<void> Show(const Notification& n) override;
    Result<void> Update(const Notification& n) override;
    Result<void> Dismiss(uint64_t) override { return Ok(); }
};

// The in-engine Notification Center. Channels: "center". Retains the latest
// notification for headless inspection (tests, remote apps).
class CenterNotificationProvider final : public INotificationProvider {
public:
    const char* Name() const noexcept override { return "Center"; }
    std::vector<std::string> SupportedChannels() const override { return {"center"}; }
    ProviderCapabilities Capabilities() const override {
        ProviderCapabilities c;
        c.supportsProgress = true;
        c.supportsActions = true;
        c.supportsAutoDismiss = false;
        c.supportsPersistence = true;
        c.supportsGrouping = true;
        return c;
    }
    Result<void> Show(const Notification& n) override;
    Result<void> Update(const Notification& n) override;
    Result<void> Dismiss(uint64_t id) override;

    Notification Latest() const;
    std::vector<Notification> Recent(size_t max = 20) const;

private:
    mutable std::mutex mutex_;
    std::vector<Notification> recent_;   // newest first, bounded
    std::vector<uint64_t> dismissed_;
};

// Headless Status Bar: retains the latest message for the UI layer to consume.
// Channel: "statusbar".
class StatusBarNotificationProvider final : public INotificationProvider {
public:
    const char* Name() const noexcept override { return "StatusBar"; }
    std::vector<std::string> SupportedChannels() const override { return {"statusbar"}; }
    ProviderCapabilities Capabilities() const override {
        ProviderCapabilities c;
        c.supportsProgress = true;
        return c;
    }
    Result<void> Show(const Notification& n) override;
    Result<void> Update(const Notification& n) override;
    Result<void> Dismiss(uint64_t) override { return Ok(); }

    Notification Latest() const;
    std::string LatestText() const;

private:
    mutable std::mutex mutex_;
    Notification latest_;
    bool hasLatest_ = false;
};

// HTTP webhook provider (docs/specs/14 §Notification Providers): POSTs each
// notification as a JSON payload to a configured endpoint using the PAL
// ISocket transport (no curl dependency). Configure with a URL such as
// "http://host:port/hooks/engine" (https/TLS is a documented future backend).
// Without a configured URL every Show degrades to Err::Unsupported so the
// service keeps running.
class WebhookNotificationProvider final : public INotificationProvider {
public:
    const char* Name() const noexcept override { return "Webhook"; }
    std::vector<std::string> SupportedChannels() const override { return {"webhook"}; }
    ProviderCapabilities Capabilities() const override {
        ProviderCapabilities c;
        c.supportsProgress = true;
        c.supportsGrouping = true;
        return c;
    }
    Result<void> Show(const Notification& n) override;
    Result<void> Update(const Notification& n) override { return Show(n); }
    Result<void> Dismiss(uint64_t) override { return Ok(); }
    Result<void> Configure(const json::Value&) override;

    // Test/inspection hooks.
    void SetEndpoint(std::string host, uint16_t port, std::string path);
    std::string LastRequest() const;   // the raw HTTP request last sent
    size_t Delivered() const;
    size_t Failed() const;

private:
    mutable std::mutex mutex_;
    std::string host_ = "127.0.0.1";
    uint16_t port_ = 0;                 // 0 = not configured
    std::string path_ = "/";
    std::string lastRequest_;
    size_t delivered_ = 0;
    size_t failed_ = 0;
};

// Test/headless recorder: captures everything (any channel). Used by the test
// suite and available for remote/automation consumers.
class RecordingNotificationProvider final : public INotificationProvider {
public:
    const char* Name() const noexcept override { return "Recorder"; }
    std::vector<std::string> SupportedChannels() const override {
        return {"console", "center", "statusbar", "toast", "banner", "remote"};
    }
    ProviderCapabilities Capabilities() const override {
        ProviderCapabilities c;
        c.supportsProgress = true;
        c.supportsActions = true;
        c.supportsGrouping = true;
        return c;
    }
    Result<void> Show(const Notification& n) override;
    Result<void> Update(const Notification& n) override;
    Result<void> Dismiss(uint64_t id) override;

    size_t Count() const;
    std::vector<Notification> All() const;
    Notification Last() const;
    void Clear();

private:
    mutable std::mutex mutex_;
    std::vector<Notification> seen_;
    std::vector<uint64_t> dismissed_;
};

} // namespace bps::notification
