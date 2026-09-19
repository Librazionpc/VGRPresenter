#include "platform/linux/LinuxMonitor.hpp"

#include "platform/PlatformAccessor.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <regex>

namespace bps::platform {

namespace {

std::string ReadFile(std::string_view path) {
    std::ifstream in{std::string(path)};
    if (!in) return {};
    std::string out, line;
    while (std::getline(in, line)) out += line + "\n";
    return out;
}

// Parse "1920x1080", "1920x1080@60.00", "1920x1080i" style mode lines.
bool ParseMode(const std::string& mode, int& w, int& h, int& refresh) {
    std::smatch m;
    if (std::regex_match(mode, m, std::regex(R"(^(\d+)x(\d+)(?:@([0-9.]+))?[a-zA-Z]*$)"))) {
        w = std::stoi(m[1].str());
        h = std::stoi(m[2].str());
        if (m[3].matched) refresh = static_cast<int>(std::stod(m[3].str()));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Runtime-loaded libdrm — fills the orientation/HDR best-effort gaps that
// sysfs alone cannot see (panel orientation + HDR_OUTPUT_METADATA blob are DRM
// connector properties). Mirrors the LinuxAudio libasound loader: resolved
// through the PAL ILibrary seam; cleanly degrades when libdrm is absent.
// ---------------------------------------------------------------------------

// Minimal ABI-compatible views of the xf86drmMode structs (field order/layout
// follows the public header so pointer access stays valid).
struct DrmRes {
    int count_fbs;
    uint32_t* fbs;
    int count_crtcs;
    uint32_t* crtcs;
    int count_connectors;
    uint32_t* connectors;
    int count_encoders;
    uint32_t* encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
};
struct DrmConnector {
    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mmWidth, mmHeight;
    uint32_t subpixel;
    int count_modes;
    void* modes;
    int count_props;
    uint32_t* props;
    uint64_t* prop_values;
    int count_encoders;
    uint32_t* encoders;
};
struct DrmProperty {
    uint32_t prop_id;
    uint32_t flags;
    char name[33];   // DRM_PROP_NAME_LEN = 32 (+ NUL)
    uint32_t count_values;
    uint64_t* values;
    int count_enums;
    void* enums;
    uint32_t count_blobs;
    void* blobs;
};

struct DrmApi {
    DrmRes* (*get_resources)(int fd);
    DrmConnector* (*get_connector)(int fd, uint32_t id);
    DrmProperty* (*get_property)(int fd, uint32_t id);
    void (*free_resources)(DrmRes*);
    void (*free_connector)(DrmConnector*);
    void (*free_property)(DrmProperty*);
};

class DrmLoader {
public:
    static const DrmApi& Api() {
        static DrmLoader loader;
        return loader.api_;
    }

    bool Available() const { return api_.get_resources != nullptr; }

private:
    DrmLoader() {
        auto& loader = platform::PlatformAccessor::Get().Library();
        auto h = loader.Load("libdrm.so.2");
        if (!h.ok()) return;
        handle_ = h.value();
        auto sym = [&](void** out, const char* name) {
            auto s = loader.Symbol(handle_, name);
            if (s.ok()) *out = s.value();
        };
        sym((void**)&api_.get_resources, "drmModeGetResources");
        sym((void**)&api_.get_connector, "drmModeGetConnector");
        sym((void**)&api_.get_property, "drmModeGetProperty");
        sym((void**)&api_.free_resources, "drmModeFreeResources");
        sym((void**)&api_.free_connector, "drmModeFreeConnector");
        sym((void**)&api_.free_property, "drmModeFreeProperty");
        if (!api_.get_resources || !api_.get_connector || !api_.get_property ||
            !api_.free_resources || !api_.free_connector || !api_.free_property) {
            (void)loader.Unload(handle_);
            handle_ = nullptr;
            api_ = DrmApi{};
        }
    }

    ~DrmLoader() {
        if (handle_)
            (void)platform::PlatformAccessor::Get().Library().Unload(handle_);
    }

    platform::ILibrary::Handle handle_ = nullptr;
    DrmApi api_{};
};

// DRM connector_type -> sysfs connector name prefix (e.g. 14 -> "eDP").
const char* ConnectorTypeName(uint32_t type) {
    switch (type) {
        case 1: return "VGA";
        case 2: return "DVI-I";
        case 3: return "DVI-D";
        case 4: return "DVI-A";
        case 5: return "Composite";
        case 6: return "SVIDEO";
        case 7: return "LVDS";
        case 8: return "Component";
        case 9: return "DIN";
        case 10: return "DisplayPort";
        case 11: return "HDMI-A";
        case 12: return "HDMI-B";
        case 13: return "TV";
        case 14: return "eDP";
        case 15: return "Virtual";
        case 16: return "DSI";
        case 17: return "DPI";
        default: return nullptr;
    }
}

// Fills orientation (0/90/180/270) and hdrSupported from DRM connector
// properties. Best effort: any failure leaves the sysfs-derived defaults.
void DrmEnrich(std::vector<MonitorInfo>& monitors) {
    const DrmApi& api = DrmLoader::Api();
    if (!api.get_resources) return;

    for (int card = 0; card < 8; ++card) {
        std::string path = "/dev/dri/card" + std::to_string(card);
        int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;

        DrmRes* res = api.get_resources(fd);
        if (!res) {
            ::close(fd);
            continue;
        }
        for (int i = 0; i < res->count_connectors; ++i) {
            DrmConnector* conn = api.get_connector(fd, res->connectors[i]);
            if (!conn) continue;
            if (conn->connection != 1) {   // DRM_MODE_CONNECTED
                api.free_connector(conn);
                continue;
            }
            const char* typeName = ConnectorTypeName(conn->connector_type);
            if (typeName) {
                std::string id =
                    std::string(typeName) + "-" + std::to_string(conn->connector_type_id);
                for (auto& m : monitors) {
                    if (m.id != id) continue;
                    for (int p = 0; p < conn->count_props; ++p) {
                        DrmProperty* prop = api.get_property(fd, conn->props[p]);
                        if (!prop) continue;
                        const char* name = prop->name;
                        if (std::string(name) == "panel orientation" &&
                            conn->prop_values[p] < prop->count_values) {
                            // DRM_MODE_PANEL_ORIENTATION_*: NORMAL=0, BOTTOM_UP=1,
                            // LEFT_UP=2 (270 deg), RIGHT_UP=3 (90 deg).
                            switch (conn->prop_values[p]) {
                                case 1: m.orientation = 180; break;
                                case 2: m.orientation = 270; break;
                                case 3: m.orientation = 90; break;
                                default: m.orientation = 0; break;
                            }
                        } else if (std::string(name) == "HDR_OUTPUT_METADATA" &&
                                   conn->prop_values[p] != 0) {
                            // A non-zero blob id means HDR metadata is attached.
                            m.hdrSupported = true;
                        }
                        api.free_property(prop);
                    }
                    break;
                }
            }
            api.free_connector(conn);
        }
        api.free_resources(res);
        ::close(fd);
    }
}
} // namespace

std::vector<MonitorInfo> LinuxMonitor::Enumerate() const {
    std::vector<MonitorInfo> out;

    // /sys/class/drm/card*-<CONNECTOR>: e.g. card0-eDP-1, card0-HDMI-A-1
    std::error_code ec;
    std::filesystem::path drm("/sys/class/drm");
    std::filesystem::directory_iterator it(drm, ec);
    if (ec) return out;

    bool havePrimary = false;
    for (const auto& entry : it) {
        std::string name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0) continue;
        size_t dash = name.find('-');
        if (dash == std::string::npos) continue;

        std::string base = entry.path().string();
        std::string status = ReadFile(base + "/status");
        // strip trailing newline
        if (!status.empty() && status.back() == '\n') status.pop_back();
        if (status != "connected") continue;

        MonitorInfo m;
        m.id = name.substr(dash + 1); // "eDP-1"
        m.name = name;                // "card0-eDP-1"
        m.connected = true;
        m.dpi = 96;

        std::string enabled = ReadFile(base + "/enabled");
        if (!enabled.empty() && enabled.front() == 'd') m.connected = false;

        // First mode line is the preferred mode.
        std::string modes = ReadFile(base + "/modes");
        if (!modes.empty()) {
            size_t nl = modes.find('\n');
            std::string first = modes.substr(0, nl);
            ParseMode(first, m.widthPx, m.heightPx, m.refreshRateHz);
        }

        // Embedded panels (eDP/LVDS/DSI) are conventionally the primary.
        bool embedded = m.id.rfind("eDP", 0) == 0 || m.id.rfind("LVDS", 0) == 0 ||
                        m.id.rfind("DSI", 0) == 0;
        if (!havePrimary && embedded) {
            m.primary = true;
            havePrimary = true;
        }
        out.push_back(std::move(m));
    }
    if (!havePrimary && !out.empty()) out.front().primary = true;

    // Best-effort enrichment from DRM connector properties (orientation/HDR).
    DrmEnrich(out);
    return out;
}

Result<MonitorInfo> LinuxMonitor::Primary() const {
    auto monitors = Enumerate();
    for (const auto& m : monitors)
        if (m.primary) return m;
    if (!monitors.empty()) return monitors.front();
    return Error::Make(Err::NotFound, "Monitor", "no monitors connected");
}

std::string LinuxMonitor::DefaultMonitorId() const {
    auto monitors = Enumerate();
    for (const auto& m : monitors)
        if (m.primary) return m.id;
    return monitors.empty() ? "" : monitors.front().id;
}

} // namespace bps::platform
