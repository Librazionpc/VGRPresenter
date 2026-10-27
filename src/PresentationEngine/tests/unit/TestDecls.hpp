#pragma once
// Generated declarations for every phase test suite.
void TestResult();
void TestPressureLatch();
void TestVersion();
void TestJson();
void TestLogger();
void TestConfig();
void TestServices();
void TestEventBus();
void TestThreadPool();
void TestJobCancellation();
void TestScheduler();
void TestMemory();
void TestResource();
void TestResourceDispatchOutsideLock();
void TestPlatform();
void TestPal();
void TestPalPerfStress();
void TestAtomicWrite();
void TestAssets();
void TestDrivers();
void TestDatabase();
void TestDisplay();
void TestRenderer();
void TestFeedHealth();
void TestIpc();
void TestLifecycle();
void TestModules();
void TestKernel();
void TestKernelDegrade();
void TestAutoplay();
void TestSongs();
void TestMedia();
void TestCamsUuid();
void TestCamsVfs();
void TestCamsZip();
void TestCamsDatabase();
void TestCamsRegistryCache();
void TestCamsLoader();
void TestCamsImportExport();
void TestCamsIndexerSearch();
void TestCamsWatcherValidator();
void TestCamsSerializerCompressor();
void TestCamsThumbnail();
void TestCamsContentManager();
void TestPluginManager();
void TestIpcLogStream();
void TestNotifyQueue();
void TestNotifyRules();
void TestNotifyPipeline();
void TestNotifyWebhook();
void TestProjectManager();
void TestStyleStore();
void TestStylePortableImagePaths();
void TestStyleBuilder();
void TestSceneBuilderStyles();
void TestSetActiveOutputStyle();
void TestGoLivePaths();
void TestStyleBackgroundImagePixels();
void TestBlankCarrierCompositorPixels();
// The file's own Contain/Cover/Fill beats the output's default, and a take
// clears the style's background in the composition (ComposeSlide::clearedByTake).
void TestMediaFitOnTheFileWins();
void TestStyleBackgroundImage();
// Clear all's carrier (Slide::clearStage): the engine paints NOTHING for it,
// while the GO-LIVE-with-nothing carrier beside it keeps the style painting.
void TestClearCarrierPaintsNothing();
void TestPlacedImageBlock();
void TestBlockScenes();
void TestBlockTextBox();
void TestBlockCornerRadius();
void TestAutoSizeVocabulary();
void TestPlaceholderBinding();
void TestPlaceholderBodyNotFirstSlot();
void TestFontMetricsFitParity();
void TestLineHeightObeysTheTemplate();
void TestTemplateStyleScenes();
void TestBlankTemplateFallsBackToSlideBlocks();
void TestShowSlideKeepsItsOwnDesign();
// A "bus" block's live plane on the real output frame: the block's own rect,
// a new frame through a cache hit, and a stopped feed going invisible.
void TestBusWindowPlane();
void TestTransparentBackground();
void TestFrameRule();
void TestImageScale();
void TestOutputStore();
void TestShippedConfig();
void TestProjectUndoRedo();
void TestProjectWorkspace();
void TestProjectSessionSnapshot();
void TestProjectPackage();
void TestDataManager();
void TestAdaptiveHardware();
void TestAdaptiveProfiler();
void TestAdaptiveFeatures();
void TestAdaptiveQuality();
void TestAdaptiveBudgets();
void TestAdaptiveOptimizer();
void TestAdaptivePressure();
void TestAdaptiveLearning();
void TestAdaptiveRuntime();
void TestGlyphAtlasTextureIdentity();
void TestRenderTypes();
void TestRenderBackends();
void TestRenderSceneGraph();
void TestRenderObjects();
void TestRenderText();
void TestRenderAnimation();
void TestRenderEffects();
void TestRenderPipeline();
void TestRenderGpu();
void TestRenderOutputs();
void TestVideoLayerFrameRefresh();
// Two template bands authored flush on the stage must be flush on the frame:
// no one-pixel seam of the live video between them (a shared fractional edge
// rounded independently for each box used to open one).
void TestShapeEdgeSeams();
void TestRenderEngine();
// The D3D11 GPU backend behind the same IGraphicsBackend seam: the CPU frame and
// the GPU frame of one scene compared, plus the software fallback's contract.
void TestD3D11Backend();
// The same GPU path at the STAGE size every output renders (a styled 1920x1080
// frame with a cover-fit backdrop and a large text run) — the app's own
// "hardware acceleration makes the output black" case.
void TestD3D11StageFrame();
// Which renderer a run picks: the app's hardware-acceleration switch (software
// by default) against an explicit `render.backend`, and the fallback on a
// machine that has no usable GPU.
void TestRenderBackendPreference();
void TestRenderStress();
void TestPngCodec();
void TestDisplayProviders();
void TestDisplayDevices();
void TestDisplayOutputs();
void TestDisplayScaling();
void TestDisplaySelfTest();
void TestDisplayRecovery();
void TestDisplayNdi();
void TestPresentationStateMachine();
void TestPresentationNavigator();
void TestPresentationTimeline();
void TestPresentationEngine();
void TestPresentationSerializer();
void TestPresentationDocument();
// The reading step's arithmetic (LineWalk): monotonic progress, page boundaries,
// run-window shifts and the empty-page edges.
void TestLineWalkMonotonic();
void TestLineWalkBoundaries();
void TestLineWalkRunShift();
void TestLineWalkEdges();
void TestShowStructure();
void TestTemplateFile();
void TestVgrWriteCreatesItsFolder();
void TestShowLibrary();
void TestShowEditor();
void TestNextTimer();
void TestSlideMenu();
void TestTheTableRealPdfs();
void TestDocumentEdit();
void TestShowLibraryCrud();
void TestSearchIndexing();
void TestSearchRanking();
void TestSearchFuzzyResolution();
void TestTypingTailResolution();
void TestTypingTailSearch();
void TestMediaEngine();
void TestPngEncode();
void TestMediaLibraryFolders();
void TestMediaLibraryNestedFolders();
// The Media pane's item menu: rename / move / delete a real file, and the index
// that lists it afterwards (see tests_media.cpp).
void TestMediaLibraryItemEdit();
// A file's own fill (Contain/Cover/Stretch) lives on the file: it survives a
// rescan, a rename and a restart, and "" means "follow the output's default".
void TestMediaLibraryItemFit();
// The operator's Delete is undoable: the file goes to the library's trash and a
// restore puts it back in its own folder, in its own place (see tests_media.cpp).
void TestMediaLibraryUndoDelete();
void TestEmptyPathSafety();
void TestThumbnailCache();
void TestThumbnailCacheConcurrency();
void TestOverlayLibrary();
void TestDesignCatalogRevision();
void TestTemplateLibrary();
void TestTheTableLibrary();
void TestTheTableUserData();
void TestTheTableSearchIndexing();
void TestTheTableTypingTail();
void TestTheTableCitationSuffixCode();
void TestTextListFormat();
void TestBlockValidator();
void TestDropRules();
void TestProjectLibrary();
void TestAppSettings();
void TestAppSettingsShippedBaseline();
void TestBibleOutline();
void TestBiblePersistence();
void TestBibleUserDataPersist();
void TestBibleMarkWritesOnlyTheSidecar();
void TestBibleMarkTiming();
void TestBibleUserDataMigration();
void TestBookFill();
void TestSmartConfig();
void TestDataProtection();
void TestImportFormats();
void TestSongText();
void TestImportFiles();
void TestShowFromImported();
void TestVgrFormat();
void TestSceneComposition();
void TestBibleResolver();
void TestBibleProviders();
void TestBibleEngine();
void TestSongChords();
void TestSongProviders();
void TestSongEngine();
void TestFlowModel();
void TestFlowEngine();
void TestProductionGraph();
void TestProductionEngine();
void TestProductionDispatchOutsideLock();
void TestProductionAudioMixer();
void TestProductionPersistence();
void TestProductionRoutingPolicy();
void TestProductionGraphSavedToDisk();
void TestRecordingEngine();
void TestRecordingDispatchOutsideLock();
void TestTelemetry();
void TestBroadcastEngine();
void TestBroadcastSenders();
void TestBroadcastFeatures();
void TestBroadcastNdiRuntime();
void TestLineSpacingAndLineBands();
