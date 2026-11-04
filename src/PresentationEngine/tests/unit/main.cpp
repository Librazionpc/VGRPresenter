#include "TestHarness.hpp"
#include "TestDecls.hpp"

#include "core/task_scheduler/TaskScheduler.hpp"
#include "core/threading/ThreadPool.hpp"
#include "modules/recording/RecordingEngine.hpp"

#include <cstdio>
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
    {"core", "job-cancel", TestJobCancellation},
    {"core", "scheduler", TestScheduler},
    {"core", "memory", TestMemory},
    {"core", "resource", TestResource},
    {"core", "platform", TestPlatform},
    {"core", "pal", TestPal},
    {"core", "palstress", TestPalPerfStress},
    {"core", "atomic-write", TestAtomicWrite},
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
    {"styles", "style-store", TestStyleStore},
    {"styles", "style-paths", TestStylePortableImagePaths},
    {"styles", "output-store", TestOutputStore},
    {"styles", "shipped-config", TestShippedConfig},
    {"styles", "style-builder", TestStyleBuilder},
    {"styles", "scene-styles", TestSceneBuilderStyles},
    {"styles", "active-style", TestSetActiveOutputStyle},
    {"styles", "go-live-paths", TestGoLivePaths},
    {"styles", "style-bg-image", TestStyleBackgroundImage},
    {"styles", "style-bg-pixels", TestStyleBackgroundImagePixels},
    {"styles", "blank-carrier-pixels", TestBlankCarrierCompositorPixels},
    {"styles", "media-fit-on-file", TestMediaFitOnTheFileWins},
    {"styles", "clear-carrier-paints-nothing", TestClearCarrierPaintsNothing},
    {"styles", "placed-image-block", TestPlacedImageBlock},
    {"styles", "block-scenes", TestBlockScenes},
    {"styles", "block-text-box", TestBlockTextBox},
    {"styles", "block-corner-radius", TestBlockCornerRadius},
    {"styles", "auto-size-vocabulary", TestAutoSizeVocabulary},
    {"styles", "placeholder-binding", TestPlaceholderBinding},
    {"styles", "placeholder-body-not-first-slot", TestPlaceholderBodyNotFirstSlot},
    {"styles", "font-metrics-fit", TestFontMetricsFitParity},
    {"styles", "line-height-obeys-template", TestLineHeightObeysTheTemplate},
    {"styles", "template-style-scenes", TestTemplateStyleScenes},
    {"styles", "blank-template-blocks", TestBlankTemplateFallsBackToSlideBlocks},
    {"styles", "show-slide-keeps-own-design", TestShowSlideKeepsItsOwnDesign},
    {"styles", "transparent-background", TestTransparentBackground},
    {"styles", "frame-rule", TestFrameRule},
    {"styles", "bus-window-plane", TestBusWindowPlane},
    {"styles", "template-bus-windows", TestTemplateBusWindows},
    {"adaptive", "adaptive-hw", TestAdaptiveHardware},
    {"adaptive", "adaptive-prof", TestAdaptiveProfiler},
    {"adaptive", "adaptive-feat", TestAdaptiveFeatures},
    {"adaptive", "adaptive-qual", TestAdaptiveQuality},
    {"adaptive", "adaptive-bud", TestAdaptiveBudgets},
    {"adaptive", "adaptive-opt", TestAdaptiveOptimizer},
    {"adaptive", "adaptive-pres", TestAdaptivePressure},
    {"adaptive", "adaptive-learn", TestAdaptiveLearning},
    {"adaptive", "adaptive-rt", TestAdaptiveRuntime},
    {"rendering", "glyph-atlas-identity", TestGlyphAtlasTextureIdentity},
    {"rendering", "line-spacing-bands", TestLineSpacingAndLineBands},
    {"rendering", "reading-flow-draw-cost", TestReadingFlowDrawCost},
    {"rendering", "reading-flow-window-refresh", TestReadingFlowWindowRefresh},
    {"rendering", "flow-scroll-math", TestFlowScrollMath},
    {"rendering", "forced-line-breaks", TestForcedLineBreaks},
    {"rendering", "show-category-override", TestShowCategoryOverrideWins},
    {"rendering", "reading-page-keeps-pick", TestReadingPageKeepsPick},
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
    {"rendering", "imgscale", TestImageScale},
    {"rendering", "vframe", TestVideoLayerFrameRefresh},
    {"rendering", "seam", TestShapeEdgeSeams},
    {"rendering", "rengine", TestRenderEngine},
    {"rendering", "d3d11-backend", TestD3D11Backend},
    {"rendering", "d3d11-stage-frame", TestD3D11StageFrame},
    {"rendering", "backend-preference", TestRenderBackendPreference},
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
    {"presentation", "present", TestLineWalkMonotonic},
    {"presentation", "present", TestLineWalkBoundaries},
    {"presentation", "present", TestLineWalkRunShift},
    {"presentation", "present", TestLineWalkEdges},
    {"presentation", "present", TestShowStructure},
    {"presentation", "present", TestTemplateFile},
    {"presentation", "vgr-mkdir", TestVgrWriteCreatesItsFolder},
    {"presentation", "present", TestShowLibrary},
    {"presentation", "present", TestShowEditor},
    {"presentation", "present", TestNextTimer},
    {"presentation", "present", TestSlideMenu},
    {"presentation", "present", TestDocumentEdit},
    {"presentation", "present", TestShowLibraryCrud},
    {"search", "search", TestSearchIndexing},
    {"search", "index-storage-postings", TestIndexStoragePostings},
    {"search", "search", TestSearchRanking},
    {"search", "search", TestSearchFuzzyResolution},
    {"search", "typing-tail", TestTypingTailResolution},
    {"search", "typing-tail", TestTypingTailSearch},
    {"media", "media", TestMediaEngine},
    {"media", "media-library", TestPngEncode},
    {"media", "media-library", TestMediaLibraryFolders},
    {"media", "media-library", TestMediaLibraryNestedFolders},
    {"media", "media-library", TestMediaLibraryItemEdit},
    {"media", "media-item-fit", TestMediaLibraryItemFit},
    {"media", "media-library", TestMediaLibraryUndoDelete},
    {"media", "media-library", TestEmptyPathSafety},
    {"media", "media-library", TestThumbnailCache},
    {"media", "media-library", TestThumbnailCacheConcurrency},
    {"library", "overlay-library", TestOverlayLibrary},
        {"library", "catalog-revision", TestDesignCatalogRevision},
    {"library", "template-library", TestTemplateLibrary},
    {"library", "the-table-library", TestTheTableLibrary},
    {"library", "the-table-user-data", TestTheTableUserData},
    {"library", "the-table-search-index", TestTheTableSearchIndexing},
    {"library", "the-table-typing-tail", TestTheTableTypingTail},
    {"library", "the-table-citation-code", TestTheTableCitationSuffixCode},
    {"library", "the-table-real-pdfs", TestTheTableRealPdfs},
    {"library", "text-list-format", TestTextListFormat},
    {"library", "block-validator", TestBlockValidator},
    {"library", "drop-rules", TestDropRules},
    {"library", "project-library", TestProjectLibrary},
    {"settings", "app-settings", TestAppSettings},
    {"settings", "app-settings-shipped", TestAppSettingsShippedBaseline},
    {"settings", "smart-config", TestSmartConfig},
    {"settings", "data-protection", TestDataProtection},
    {"import", "import-formats", TestImportFormats},
    {"import", "song-text", TestSongText},
    {"import", "import-files", TestImportFiles},
    {"import", "show-from-imported", TestShowFromImported},
    {"vgr", "vgr", TestVgrFormat},
    {"scene", "scene", TestSceneComposition},
    {"bible", "bible", TestBibleResolver},
    {"bible", "bible", TestBibleProviders},
    {"bible", "bible", TestBibleEngine},
    {"bible", "bible-outline", TestBibleOutline},
    {"bible", "bible-persist", TestBiblePersistence},
    {"bible", "bible-userdata-persist", TestBibleUserDataPersist},
    {"bible", "bible-mark-sidecar", TestBibleMarkWritesOnlyTheSidecar},
    {"bible", "bible-mark-timing", TestBibleMarkTiming},
    {"bible", "bible-userdata-migration", TestBibleUserDataMigration},
    {"bible", "book-fill", TestBookFill},
    {"song", "song", TestSongChords},
    {"song", "song", TestSongProviders},
    {"song", "song", TestSongEngine},
    {"flow", "flow", TestFlowModel},
    {"flow", "flow", TestFlowEngine},
    {"production", "graph", TestProductionGraph},
    {"production", "engine", TestProductionEngine},
    {"production", "audio-mixer", TestProductionAudioMixer},
    {"production", "output-signals", TestProductionOutputSignalEdges},
    {"production", "display-output-audio", TestProductionDisplayOutputAudioPlays},
    {"production", "audio-dsp", TestProductionAudioDsp},
    {"production", "persistence", TestProductionPersistence},
    {"production", "routing-policy", TestProductionRoutingPolicy},
    {"production", "graph-durable", TestProductionGraphSavedToDisk},
    {"recording", "recording", TestRecordingEngine},
    {"telemetry", "telemetry", TestTelemetry},
    // The automatic stall blackout: the per-output present heartbeat and the
    // opaque black frame handed to an output whose feed stopped moving.
    {"rendering", "feed-health", TestFeedHealth},
    {"broadcast", "broadcast", TestBroadcastEngine},
    {"broadcast", "broadcast", TestBroadcastSenders},
    {"broadcast", "broadcast", TestBroadcastFeatures},
    {"broadcast", "broadcast-ndi", TestBroadcastNdiRuntime},
    {"core", "kernel", TestKernel},
    // Dispatch scope: the lock-scope guard reads source, these three prove at
    // runtime that a bus dispatch runs with the engine lock released.
    {"production", "dispatch-scope", TestProductionDispatchOutsideLock},
    {"recording", "dispatch-scope", TestRecordingDispatchOutsideLock},
    {"recording", "real-encode", TestRecordingRealEncode},
    {"core", "resource-dispatch-scope", TestResourceDispatchOutsideLock},
    {"core", "kernel-degrade", TestKernelDegrade},
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

extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
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
        bps::recording::RecordingEngine::Instance().SetSimulatedFreeBytes(kTestFreeBytes);
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