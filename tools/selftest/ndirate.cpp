// ndirate: connect to an NDI source whose name contains argv[1] and, for argv[2] seconds, count the video frames it
// delivers each second and the longest gap between two frames. Loads the NDI runtime dynamically (the DLL the app uses).
//   usage: ndirate <name-substring> <seconds>
// Prints one line per second ("t=3 frames=15 maxGapMs=72") and a final summary line:
//   NDIRATE: frames=<n> seconds=<s> maxGapMs=<ms> dropouts=<seconds with no frame>
// A picture the app has left unchanged is sent at a slower "still" beat; this proves a real receiver keeps receiving it.
#include <windows.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

struct Source { const char* name; const char* url; };
struct FindCreate { bool local; const char* groups; const char* ips; };
struct RecvCreate { Source src; int color; int bw; bool fields; const char* name; };
struct Video {
    int xres, yres; int fourcc; int frN, frD; float aspect; int fmt; int64_t timecode;
    uint8_t* data; int stride; const char* meta; int64_t ts;
};

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: ndirate <name-substring> <seconds>\n"); return 2; }
    const int seconds = std::atoi(argv[2]);
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
    for (int t = 0; t < 20 && pick.empty(); t++) {
        findWait(find, 1000);
        uint32_t n = 0; const Source* s = findGet(find, &n);
        for (uint32_t i = 0; i < n; i++)
            if (std::string(s[i].name).find(argv[1]) != std::string::npos) { pick = s[i].name; chosen = s[i]; break; }
    }
    if (pick.empty()) { printf("source not found\n"); return 5; }
    printf("receiving '%s'\n", pick.c_str());
    // Lowest bandwidth would drop to a proxy stream; ask for the full picture like a real switcher does.
    RecvCreate rc{chosen, 0 /*BGRX_BGRA*/, 100 /*highest*/, false, "ndirate"};
    void* recv = recvCreate(&rc);

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto lastFrame = start;
    double maxGapMs = 0.0;
    long total = 0, thisSecond = 0;
    int second = 0, dropouts = 0;
    double secondMaxGap = 0.0;
    while (second < seconds) {
        Video v{};
        const int type = capture(recv, &v, nullptr, nullptr, 100);
        const auto now = clock::now();
        if (type == 1 && v.data) {
            const double gap = std::chrono::duration<double, std::milli>(now - lastFrame).count();
            if (total > 0) { if (gap > maxGapMs) maxGapMs = gap; if (gap > secondMaxGap) secondMaxGap = gap; }
            lastFrame = now;
            ++total; ++thisSecond;
            freeVideo(recv, &v);
        }
        const int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (elapsed > second) {
            printf("t=%d frames=%ld maxGapMs=%.0f\n", elapsed, thisSecond, secondMaxGap);
            if (thisSecond == 0) ++dropouts;
            thisSecond = 0; secondMaxGap = 0.0; second = elapsed;
        }
    }
    printf("NDIRATE: frames=%ld seconds=%d maxGapMs=%.0f dropouts=%d\n", total, seconds, maxGapMs, dropouts);
    return 0;
}
