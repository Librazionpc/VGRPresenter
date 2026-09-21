// Engine unit test runner (docs/specs/00 §8).
// Usage:
//   ./bps_unit_tests                 run every phase
//   ./bps_unit_tests flow            run only the 'flow' phase
//   ./bps_unit_tests bible song      run suites in phases/labels matching 'bible' or 'song'
//   ./bps_unit_tests proj-mgr        run a single suite label
//
// Build: `cmake -B build && cmake --build build && ./build/bps_unit_tests [phase]`
//
// Phase fixture: because the tests are split into per-phase files, a single
// phase may run alone (./bps_unit_tests media). Modules such as MediaModule and
// AdaptiveRuntime schedule work on the TaskScheduler/ThreadPool singletons, and
// RenderEngine/AdaptiveRuntime resize the ThreadPool, so those singletons are
// booted here before any suite runs and torn down afterwards. This guarantees
// every phase gets the same environment the full suite gets from TestKernel's
// boot/shutdown (scheduler running, workers joined at exit), without ordering
// the phases by accident. Initialize/Shutdown are idempotent, so this is a no-op
// for suites that manage their own lifecycle (TestThreadPool/TestScheduler/
// TestKernel).
#include "TestHarness.hpp"
#include "TestDecls.hpp"

#include "core/task_scheduler/TaskScheduler.hpp"
#include "core/threading/ThreadPool.hpp"

#include <cstring>

struct Suite {
    const char* phase;
    const char* name;
    void (*fn)();
};

static const Suite kSuites[] = {
    {"core", "result", TestResult},
    {"core", "pressure-latch", TestPressureLatch},
    {"core", "version", TestVersion},
    {"core", "json", TestJson},
    {"core", "logger", TestLogger},
    {"core", "config", TestConfig},
    {"core", "services", TestServices},
    {"core", "eventbus", TestEventBus},
    {"core", "threadpool", TestThreadPool},
    {"core", "scheduler", TestScheduler},
    {"core", "memory", TestMemory},
    {"core", "resource", TestResource},
    {"core", "platform", TestPlatform},
    {"core", "pal", TestPal},
    {"core", "palstress", TestPalPerfStress},
    {"core", "assets", TestAssets},
    {"core", "drivers", TestDrivers},
    {"core", "database", TestDatabase},
    {"core", "display", TestDisplay},
    {"core", "renderer", TestRenderer},
    {"core", "ipc", TestIpc},
    {"core", "lifecycle", TestLifecycle},
    {"core", "modules", TestModules},
    {"core", "autoplay", TestAutoplay},
    {"core", "songs", TestSongs},
    {"core", "media", TestMedia},
    {"cams", "cams", TestCamsUuid},
    {"cams", "cams", TestCamsVfs},
    {"cams", "cams", TestCamsZip},
    {"cams", "cams", TestCamsDatabase},
    {"cams", "cams", TestCamsRegistryCache},
    {"cams", "cams", TestCamsLoader},
    {"cams", "cams", TestCamsImportExport},
    {"cams", "cams", TestCamsIndexerSearch},
    {"cams", "cams", TestCamsWatcherValidator},
    {"cams", "cams", TestCamsSerializerCompressor},
    {"cams", "cams", TestCamsThumbnail},
    {"cams", "cams", TestCamsContentManager},
    {"modules", "plugins", TestPluginManager},
#if !defined(_WIN32)
    // TestIpcLogStream itself is #if !defined(_WIN32) in tests_modules.cpp
    // (not yet verified on Windows) — the registration has to match or this
    // is an undefined reference on a Windows link.
    {"modules", "ipcstream", TestIpcLogStream},
#endif
    {"notify", "notify", TestNotifyQueue},
    {"notify", "notify", TestNotifyRules},
    {"notify", "notify", TestNotifyPipeline},
    {"notify", "notify", TestNotifyWebhook},
    {"project", "proj-mgr", TestProjectManager},
    {"project", "proj-undo", TestProjectUndoRedo},
    {"project", "proj-ws", TestProjectWorkspace},
    {"project", "proj-sess", TestProjectSessionSnapshot},
    {"project", "proj-pkg", TestProjectPackage},
    {"project", "proj-data", TestDataManager},
    {"adaptive", "adaptive-hw", TestAdaptiveHardware},
    {"adaptive", "adaptive-prof", TestAdaptiveProfiler},
    {"adaptive", "adaptive-feat", TestAdaptiveFeatures},
    {"adaptive", "adaptive-qual", TestAdaptiveQuality},
    {"adaptive", "adaptive-bud", TestAdaptiveBudgets},
    {"adaptive", "adaptive-opt", TestAdaptiveOptimizer},
    {"adaptive", "adaptive-pres", TestAdaptivePressure},
    {"adaptive", "adaptive-learn", TestAdaptiveLearning},
    {"adaptive", "adaptive-rt", TestAdaptiveRuntime},
    {"rendering", "rtypes", TestRenderTypes},
    {"rendering", "rbackends", TestRenderBackends},
    {"rendering", "rscene", TestRenderSceneGraph},
    {"rendering", "robjs", TestRenderObjects},
    {"rendering", "rtext", TestRenderText},
    {"rendering", "ranim", TestRenderAnimation},
    {"rendering", "reffects", TestRenderEffects},
    {"rendering", "rpipeline", TestRenderPipeline},
    {"rendering", "rgpu", TestRenderGpu},
    {"rendering", "routputs", TestRenderOutputs},
    {"rendering", "rengine", TestRenderEngine},
    {"rendering", "rstress", TestRenderStress},
    {"rendering", "png", TestPngCodec},
    {"display", "display", TestDisplayProviders},
    {"display", "display", TestDisplayDevices},
    {"display", "display", TestDisplayOutputs},
    {"display", "display", TestDisplayScaling},
    {"display", "display", TestDisplaySelfTest},
    {"display", "display", TestDisplayRecovery},
    {"display", "display", TestDisplayNdi},
    {"presentation", "present", TestPresentationStateMachine},
    {"presentation", "present", TestPresentationNavigator},
    {"presentation", "present", TestPresentationTimeline},
    {"presentation", "present", TestPresentationEngine},
    {"presentation", "present", TestPresentationSerializer},
    {"presentation", "present", TestPresentationDocument},
    {"presentation", "present", TestShowStructure},
    {"presentation", "present", TestTemplateFile},
    {"presentation", "present", TestShowLibrary},
    {"presentation", "present", TestShowEditor},
    {"presentation", "present", TestDocumentEdit},
    {"presentation", "present", TestShowLibraryCrud},
    {"search", "search", TestSearchIndexing},
    {"search", "search", TestSearchRanking},
    {"media", "media", TestMediaEngine},
    {"media", "media-library", TestPngEncode},
    {"media", "media-library", TestMediaLibraryFolders},
    {"media", "media-library", TestMediaLibraryNestedFolders},
    {"media", "media-library", TestThumbnailCache},
    {"media", "media-library", TestThumbnailCacheConcurrency},
    {"library", "overlay-library", TestOverlayLibrary},
    {"library", "template-library", TestTemplateLibrary},
    {"library", "text-list-format", TestTextListFormat},
    {"library", "block-validator", TestBlockValidator},
    {"settings", "app-settings", TestAppSettings},
    {"settings", "smart-config", TestSmartConfig},
    {"settings", "data-protection", TestDataProtection},
    {"vgr", "vgr", TestVgrFormat},
    {"scene", "scene", TestSceneComposition},
    {"bible", "bible", TestBibleResolver},
    {"bible", "bible", TestBibleProviders},
    {"bible", "bible", TestBibleEngine},
    {"song", "song", TestSongChords},
    {"song", "song", TestSongProviders},
    {"song", "song", TestSongEngine},
    {"flow", "flow", TestFlowModel},
    {"flow", "flow", TestFlowEngine},
    {"production", "graph", TestProductionGraph},
    {"production", "engine", TestProductionEngine},
    {"production", "persistence", TestProductionPersistence},
    {"recording", "recording", TestRecordingEngine},
    {"telemetry", "telemetry", TestTelemetry},
    {"broadcast", "broadcast", TestBroadcastEngine},
    {"broadcast", "broadcast", TestBroadcastSenders},
    {"broadcast", "broadcast", TestBroadcastFeatures},
    {"broadcast", "broadcast-ndi", TestBroadcastNdiRuntime},
    {"core", "kernel", TestKernel},
};
static bool Matches(const char* phase, const char* name,
                    const std::vector<std::string>& filters) {
    if (filters.empty()) return true;
    for (const auto& f : filters) {
        if (std::string(phase).find(f) != std::string::npos) return true;
        if (std::string(name).find(f) != std::string::npos) return true;
    }
    return false;
}

// Boot the shared schedulers the suites depend on, mirroring Kernel::Boot for
// the systems that module/engine tests exercise directly.
static void BootPhaseFixture() {
    (void)TaskScheduler::Instance().Initialize();
    (void)ThreadPool::Instance().Initialize(2);
}

// Tear down and join every worker so a standalone phase never aborts at exit
// with a joinable std::thread (e.g. ThreadPool workers spawned by Resize).
static void TearDownPhaseFixture() {
    (void)ThreadPool::Instance().Shutdown();
    (void)TaskScheduler::Instance().Shutdown();
}

int main(int argc, char** argv) {
    std::vector<std::string> filters;
    for (int i = 1; i < argc; ++i) filters.emplace_back(argv[i]);
    if (!filters.empty()) {
        std::fprintf(stderr, "running suites matching");
        for (const auto& f : filters) std::fprintf(stderr, " '%s'", f.c_str());
        std::fprintf(stderr, "\n");
    }
    size_t ran = 0;
    const char* lastPhase = "";
    const char* last = "";
    BootPhaseFixture();
    for (const auto& s : kSuites) {
        if (!Matches(s.phase, s.name, filters)) continue;
        if (std::strcmp(s.phase, lastPhase) != 0) {
            std::fprintf(stderr, "[phase] %s\n", s.phase);
            lastPhase = s.phase;
            last = "";
        }
        if (std::strcmp(last, s.name) != 0) {
            std::fprintf(stderr, "[mark] %s\n", s.name);
            last = s.name;
        }
        s.fn();
        ++ran;
    }
    TearDownPhaseFixture();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (ran == 0) {
        std::fprintf(stderr, "no suites matched; available phases:");
        lastPhase = "";
        for (const auto& s : kSuites)
            if (std::strcmp(s.phase, lastPhase) != 0) {
                std::fprintf(stderr, " %s", s.phase);
                lastPhase = s.phase;
            }
        std::fprintf(stderr, "\n");
        return 2;
    }
    return g_failures == 0 ? 0 : 1;
}
