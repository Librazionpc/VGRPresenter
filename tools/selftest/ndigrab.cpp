// ndigrab: find an NDI source whose name contains argv[1], save one video frame as PNG (argv[2]).
// Loads the NDI runtime dynamically (same DLL the app uses). Stored-block PNG, no zlib needed.
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct Source { const char* name; const char* url; };
struct FindCreate { bool local; const char* groups; const char* ips; };
struct RecvCreate { Source src; int color; int bw; bool fields; const char* name; };
struct Video {
    int xres, yres; int fourcc; int frN, frD; float aspect; int fmt; int64_t timecode;
    uint8_t* data; int stride; const char* meta; int64_t ts;
};

static uint32_t crcTable[256];
static void initCrc() { for (uint32_t n = 0; n < 256; n++) { uint32_t c = n; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; crcTable[n] = c; } }
static uint32_t crc(uint32_t c, const uint8_t* b, size_t n) { c = ~c; for (size_t i = 0; i < n; i++) c = crcTable[(c ^ b[i]) & 255] ^ (c >> 8); return ~c; }
static void be32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); }
static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> h; be32(h, (uint32_t)data.size()); fwrite(h.data(), 1, 4, f);
    std::vector<uint8_t> td(type, type + 4); td.insert(td.end(), data.begin(), data.end());
    fwrite(td.data(), 1, td.size(), f);
    std::vector<uint8_t> c; be32(c, crc(0, td.data(), td.size())); fwrite(c.data(), 1, 4, f);
}
static bool writePng(const char* path, int w, int h, const std::vector<uint8_t>& rgb) {
    initCrc();
    FILE* f = fopen(path, "wb"); if (!f) return false;
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr; be32(ihdr, w); be32(ihdr, h); ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(f, "IHDR", ihdr);
    std::vector<uint8_t> raw; raw.reserve((size_t)h * (w * 3 + 1));
    for (int y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), rgb.begin() + (size_t)y * w * 3, rgb.begin() + (size_t)(y + 1) * w * 3); }
    std::vector<uint8_t> z; z.push_back(0x78); z.push_back(0x01);
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < raw.size();) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0); z.push_back(n & 255); z.push_back(n >> 8); z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n); pos += n;
    }
    be32(z, (b << 16) | a);
    chunk(f, "IDAT", z); chunk(f, "IEND", {}); fclose(f); return true;
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: ndigrab <name-substring> <out.png>\n"); return 2; }
    HMODULE lib = LoadLibraryA("C:\\Program Files\\NDI\\NDI 6 Tools\\Runtime\\Processing.NDI.Lib.x64.dll");
    if (!lib) { printf("no NDI dll\n"); return 3; }
    auto init = (bool (*)())GetProcAddress(lib, "NDIlib_initialize");
    auto findCreate = (void* (*)(const FindCreate*))GetProcAddress(lib, "NDIlib_find_create_v2");
    auto findWait = (bool (*)(void*, uint32_t))GetProcAddress(lib, "NDIlib_find_wait_for_sources");
    auto findGet = (const Source* (*)(void*, uint32_t*))GetProcAddress(lib, "NDIlib_find_get_current_sources");
    auto recvCreate = (void* (*)(const RecvCreate*))GetProcAddress(lib, "NDIlib_recv_create_v3");
    auto capture = (int (*)(void*, Video*, void*, void*, uint32_t))GetProcAddress(lib, "NDIlib_recv_capture_v3");
    auto freeVideo = (void (*)(void*, Video*))GetProcAddress(lib, "NDIlib_recv_free_video_v2");
    if (!init || !init()) { printf("init failed\n"); return 4; }
    FindCreate fc{true, nullptr, nullptr};
    void* find = findCreate(&fc);
    Source chosen{nullptr, nullptr}; std::string pick;
    for (int t = 0; t < 12 && pick.empty(); t++) {
        findWait(find, 1000);
        uint32_t n = 0; const Source* s = findGet(find, &n);
        for (uint32_t i = 0; i < n; i++) {
            printf("source: %s\n", s[i].name);
            if (std::string(s[i].name).find(argv[1]) != std::string::npos) { pick = s[i].name; chosen = s[i]; break; }
        }
    }
    if (pick.empty()) { printf("source not found\n"); return 5; }
    RecvCreate rc{chosen, 0 /*BGRX_BGRA*/, 100 /*highest*/, false, "ndigrab"};
    void* recv = recvCreate(&rc);
    for (int t = 0; t < 120; t++) {
        Video v{};
        int type = capture(recv, &v, nullptr, nullptr, 1000);
        if (type == 1 && v.data && v.xres > 0) {
            printf("frame %dx%d fourcc=%08x stride=%d\n", v.xres, v.yres, v.fourcc, v.stride);
            std::vector<uint8_t> rgb((size_t)v.xres * v.yres * 3);
            // Pixels the frame actually carries, so a caller can tell "the output is transparent"
            // from "the output has the design" without decoding the PNG: opaque = alpha > 2%,
            // ink = composited away from the mid-grey a transparent pixel becomes, bright = white-ish.
            long opaque = 0, ink = 0, bright = 0;
            for (int y = 0; y < v.yres; y++) for (int x = 0; x < v.xres; x++) {
                const uint8_t* p = v.data + (size_t)y * v.stride + x * 4; uint8_t* o = &rgb[((size_t)y * v.xres + x) * 3];
                float al = p[3] / 255.0f;   // composite over mid-grey so transparency is visible
                for (int c = 0; c < 3; c++) o[c] = (uint8_t)(p[2 - c] * al + 90 * (1 - al));
                if (al > 0.02f) ++opaque;
                if (std::abs((int)o[0] - 90) > 12 || std::abs((int)o[1] - 90) > 12
                    || std::abs((int)o[2] - 90) > 12) ++ink;
                if ((o[0] + o[1] + o[2]) / 3 > 200) ++bright;
            }
            printf("stats opaque=%ld ink=%ld bright=%ld total=%d\n", opaque, ink, bright, v.xres * v.yres);
            bool ok = writePng(argv[2], v.xres, v.yres, rgb);
            freeVideo(recv, &v);
            printf(ok ? "saved %s\n" : "save failed %s\n", argv[2]);
            return ok ? 0 : 6;
        }
    }
    printf("no frame\n");
    return 7;
}
