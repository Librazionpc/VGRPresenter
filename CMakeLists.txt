cmake_minimum_required(VERSION 3.21)

# THE app version has ONE source: kAppVersion in qml/services/SettingsService.cpp (the same line the
# installer scripts read on every OS). The project version, the Windows file-properties resource and the
# macOS bundle all take it from there — they used to repeat the literal, and a release bump had four
# places to forget. A bump re-runs CMake by itself (CMAKE_CONFIGURE_DEPENDS below).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/qml/services/SettingsService.cpp" _bps_version_line
     REGEX "kAppVersion = \"[0-9]+\\.[0-9]+\\.[0-9]+\"" LIMIT_COUNT 1)
string(REGEX REPLACE ".*kAppVersion = \"([0-9]+\\.[0-9]+\\.[0-9]+)\".*" "\\1" BPS_APP_VERSION "${_bps_version_line}")
if(NOT BPS_APP_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "cannot read kAppVersion from qml/services/SettingsService.cpp")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/qml/services/SettingsService.cpp")
project(VGRPresenterUI VERSION ${BPS_APP_VERSION} LANGUAGES CXX)
string(REPLACE "." "," BPS_RC_VERSION "${BPS_APP_VERSION}")
set(BPS_RC_VERSION "${BPS_RC_VERSION},0")

# C++23 project-wide — the Qt app and the engine (src/PresentationEngine/)
# build as one project now, one CMakeLists.txt, so they share one dialect
# instead of two files trying to agree with each other. CMAKE_CXX_STANDARD
# here is inherited by every target below, Qt and engine alike; nothing else
# needs to set it again.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# A build tree configured by an IDE kit can arrive here with EMPTY Release flags — `-std=c++26` and
# nothing else, i.e. NO optimisation at all. The live video path (the software rasteriser, the scaled
# blits, the readback conversions, the compositor) then runs ~10-30x slower than the shipped build, and a
# "video lags under stress" report is really a "-O0" report. When the active Release/RelWithDebInfo flags
# carry no optimisation, the ENGINE (the pixel work; see the target below) gets -O2. A tree that sets its
# own flags (-O3 in build-release) is left alone. Only the engine: the interface layer is Qt glue, and
# optimising its ~225 translation units as well tripled the dev build time for nothing the user can feel.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _bps_active_cfg)
set(BPS_ENGINE_FALLBACK_OPT "")
if(_bps_active_cfg STREQUAL "RELEASE" OR _bps_active_cfg STREQUAL "RELWITHDEBINFO")
    if(NOT CMAKE_CXX_FLAGS_${_bps_active_cfg} MATCHES "-O[0-9sg]" AND NOT CMAKE_CXX_FLAGS MATCHES "-O[0-9sg]")
        set(BPS_ENGINE_FALLBACK_OPT "-O2")
        message(STATUS "No optimisation flag in the ${_bps_active_cfg} flags: the engine is built with -O2")
    endif()
endif()

# ---------------------------------------------------------------------------
# AddressSanitizer (opt-in, OFF by default) — the app has been crashing
# repeatedly with Windows STATUS_HEAP_CORRUPTION (0xc0000374: confirmed via
# Windows' own Application Error log, dozens of times since 2026-10-24), and
# that exception is a Windows fail-fast — it bypasses normal SEH, so the
# app's own CrashHandler never runs and build/logs's crash.log is left with
# just a BOOT line and nothing after it. Worse, the crash always reports at
# ntdll.dll (the SAME offset every time) because that is only where Windows
# NOTICES the corrupted heap metadata, on some later unrelated allocation —
# not where the actual bad write happened.
#
# ASan instruments every read/write against its own allocator and aborts
# WITH A STACK TRACE at the exact bad access, so it turns "the app vanished
# with no clue why" into a real bug report. It is diagnostic, not the
# everyday build: it roughly doubles memory use and slows the app down, and
# this toolchain is MinGW (ucrt64) g++, which needs libasan on PATH at
# runtime (MSYS2's ucrt64-gcc package ships it — same /c/msys64/ucrt64/bin
# dev_cycle.sh already puts on PATH to launch).
#
#   cmake -DBPS_ENABLE_ASAN=ON -B build
#   cmake --build build
#   (then reproduce the crash — do NOT mix an ASan and a non-ASan build in
#   the same build/ directory; reconfigure a fresh one, or OFF again and
#   reconfigure, when switching)
option(BPS_ENABLE_ASAN
    "Build with AddressSanitizer (diagnostic; localizes heap corruption at the real bad access)" OFF)
if(BPS_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

# Multimedia: the media-on-air player (LiveOutputService's QMediaPlayer decode
# of a taken video/image — the engine's render pipeline composites rasters only).
# QuickEffects: MultiEffect's live blur, behind the Settings scrim
# (ModalScrim's optional blurSource — see its own comment).
# ShaderTools: qsb, which compiles the edit canvas's slide-filter shader
# (qml/shaders/slidefilter.frag) into the .qsb package a Qt 6 ShaderEffect
# requires — Qt 6 dropped inline GLSL source strings, so the effect has to be
# built at build time like every other shader in a Qt 6 app.
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui GuiPrivate Quick QuickControls2 QuickShapes QuickEffects Multimedia Network ShaderTools)

qt_standard_project_setup(REQUIRES 6.11)

# =============================================================================
# Backend engine (src/PresentationEngine/) — was its own separate project
# with its own CMakeLists.txt; folded directly into this one 2026-09-19 so
# there's a single build description for the whole app, not two files that
# can drift out of sync. Its own cmake/ helpers (warnings profile, the
# Threads dependency) still live under src/PresentationEngine/cmake/ and are
# included below, just from this file instead of that one.
#
# The shared engine and Qt-backed generic services are platform-independent.
# Windows is the production target; Linux has a PAL backend, and macOS now
# configures with Apple-native services. Media capture/playback parity is still
# reported per platform through IPlatform::Capabilities().
# =============================================================================

include(src/PresentationEngine/cmake/Warnings.cmake)
include(src/PresentationEngine/cmake/Dependencies.cmake)

set(BPS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src/PresentationEngine)

if(CMAKE_SIZEOF_VOID_P)
    math(EXPR BPS_POINTER_BITS "${CMAKE_SIZEOF_VOID_P} * 8")
else()
    set(BPS_POINTER_BITS "?")
endif()
message(STATUS "PresentationEngine target: ${CMAKE_SYSTEM_NAME} "
                "${CMAKE_SYSTEM_PROCESSOR} (${BPS_POINTER_BITS}-bit)")

# ---- Engine core (all systems from src/PresentationEngine/docs/specs/) ----
set(BPS_CORE_SOURCES
    ${BPS_ROOT}/core/logging/Logger.cpp
    ${BPS_ROOT}/core/recovery/DocumentRecovery.cpp
    ${BPS_ROOT}/core/config/ConfigurationManager.cpp
    ${BPS_ROOT}/core/services/ServiceManager.cpp
    ${BPS_ROOT}/core/events/EventBus.cpp
    ${BPS_ROOT}/core/threading/ThreadPool.cpp
    ${BPS_ROOT}/core/task_scheduler/TaskScheduler.cpp
    ${BPS_ROOT}/core/resources/ResourceManager.cpp
    ${BPS_ROOT}/core/memory/MemoryManager.cpp
    ${BPS_ROOT}/core/plugins/PluginManager.cpp
    ${BPS_ROOT}/core/modules/ModuleManager.cpp
    ${BPS_ROOT}/core/lifecycle/LifecycleManager.cpp
    ${BPS_ROOT}/core/kernel/Kernel.cpp
    ${BPS_ROOT}/core/assets/AssetManager.cpp
    ${BPS_ROOT}/core/drivers/DriverManager.cpp
    ${BPS_ROOT}/core/database/DatabaseManager.cpp
    ${BPS_ROOT}/core/display/DisplayManager.cpp
    ${BPS_ROOT}/core/rendering/RendererManager.cpp
    ${BPS_ROOT}/core/ipc/IpcServer.cpp
    ${BPS_ROOT}/core/ipc/IpcClient.cpp
    ${BPS_ROOT}/platform/Platform.cpp
    ${BPS_ROOT}/platform/PlatformAccessor.cpp
    ${BPS_ROOT}/platform/common/Timer.cpp
    ${BPS_ROOT}/modules/presentation/PresentationModule.cpp
    ${BPS_ROOT}/modules/songs/SongsModule.cpp
    ${BPS_ROOT}/modules/media/MediaModule.cpp
    # Content & Asset Management System (Phase 3, docs/specs/13)
    ${BPS_ROOT}/modules/content/Vfs.cpp
    ${BPS_ROOT}/modules/content/AssetDatabase.cpp
    ${BPS_ROOT}/modules/content/AssetRegistry.cpp
    ${BPS_ROOT}/modules/content/AssetCache.cpp
    ${BPS_ROOT}/modules/content/AssetLoader.cpp
    ${BPS_ROOT}/modules/content/AssetManager.cpp
    ${BPS_ROOT}/modules/content/AssetSerializer.cpp
    ${BPS_ROOT}/modules/content/AssetCompressor.cpp
    ${BPS_ROOT}/modules/content/ImportManager.cpp
    ${BPS_ROOT}/modules/content/IExporter.cpp
    ${BPS_ROOT}/modules/content/AssetIndexer.cpp
    ${BPS_ROOT}/modules/content/AssetValidator.cpp
    ${BPS_ROOT}/modules/content/AssetWatcher.cpp
    ${BPS_ROOT}/modules/content/ThumbnailManager.cpp
    ${BPS_ROOT}/modules/content/ContentManager.cpp
    # Notification Service (Phase 4, docs/specs/14)
    ${BPS_ROOT}/modules/notification/NotificationInterfaces.cpp
    ${BPS_ROOT}/modules/notification/NotificationQueue.cpp
    ${BPS_ROOT}/modules/notification/NotificationScheduler.cpp
    ${BPS_ROOT}/modules/notification/NotificationHistory.cpp
    ${BPS_ROOT}/modules/notification/NotificationManager.cpp
    ${BPS_ROOT}/modules/notification/NotificationProviders.cpp
    ${BPS_ROOT}/modules/notification/NotificationRules.cpp
    ${BPS_ROOT}/modules/notification/NotificationFactory.cpp
    ${BPS_ROOT}/modules/notification/NotificationService.cpp
    # Adaptive Runtime System (Phase 5, docs/specs/16)
    ${BPS_ROOT}/modules/adaptive/Hardware.cpp
    ${BPS_ROOT}/modules/adaptive/PerformanceProfiler.cpp
    ${BPS_ROOT}/modules/adaptive/Contracts.cpp
    ${BPS_ROOT}/modules/adaptive/Quality.cpp
    ${BPS_ROOT}/modules/adaptive/Budgets.cpp
    ${BPS_ROOT}/modules/adaptive/Optimizer.cpp
    ${BPS_ROOT}/modules/adaptive/Pressure.cpp
    ${BPS_ROOT}/modules/adaptive/Learning.cpp
    ${BPS_ROOT}/modules/adaptive/AdaptiveRuntime.cpp
    # Rendering Engine (Phase 6, docs/specs/17)
    ${BPS_ROOT}/modules/rendering/GraphicsBackends.cpp
    # The D3D11 backend: the GPU rasterizer behind the same IGraphicsBackend seam
    # (the whole file is _WIN32-gated; elsewhere the factory returns nullptr and the
    # engine keeps the software backend). Links d3d11 + d3dcompiler on Windows only.
    ${BPS_ROOT}/modules/rendering/D3D11Backend.cpp
    ${BPS_ROOT}/platform/qt/QtRhiBackend.cpp
    ${BPS_ROOT}/platform/qt/QtClipboard.cpp   # QClipboard: one clipboard class on every OS
    ${BPS_ROOT}/platform/qt/QtComposed.cpp    # Qt-first network / environment / input / notifications
    # The scale filter both picture paths share (ImageScale.hpp): the rasterizer's
    # scaled blits and the per-output frame resize.
    ${BPS_ROOT}/modules/rendering/ImageScale.cpp
    ${BPS_ROOT}/modules/rendering/Scene.cpp
    ${BPS_ROOT}/modules/rendering/RenderObject.cpp
    ${BPS_ROOT}/modules/rendering/TextEngine.cpp
    ${BPS_ROOT}/modules/rendering/Animation.cpp
    ${BPS_ROOT}/modules/rendering/Effects.cpp
    ${BPS_ROOT}/modules/rendering/Pipeline.cpp
    ${BPS_ROOT}/modules/rendering/GpuResources.cpp
    ${BPS_ROOT}/modules/rendering/RenderOutputs.cpp
    ${BPS_ROOT}/modules/rendering/RenderEngine.cpp
    ${BPS_ROOT}/modules/rendering/PngCodec.cpp
    # Display & Output Engine (Phase 7, docs/specs/18)
    ${BPS_ROOT}/modules/display/Providers.cpp
    ${BPS_ROOT}/modules/display/HtmlOutputServer.cpp
    ${BPS_ROOT}/modules/display/OutputRouter.cpp
    ${BPS_ROOT}/modules/display/DisplayProfiles.cpp
    ${BPS_ROOT}/modules/display/DisplayEngine.cpp
    ${BPS_ROOT}/modules/display/DisplayTest.cpp
    # Presentation Engine (Phase 8, docs/specs/19)
    ${BPS_ROOT}/modules/presentation/PresentationStateMachine.cpp
    ${BPS_ROOT}/modules/presentation/PresentationCues.cpp
    ${BPS_ROOT}/modules/presentation/PresentationTimeline.cpp
    ${BPS_ROOT}/modules/presentation/PresentationNavigator.cpp
    ${BPS_ROOT}/modules/presentation/BookFill.cpp
    ${BPS_ROOT}/modules/presentation/PresentationValidator.cpp
    ${BPS_ROOT}/modules/presentation/PresentationCompiler.cpp
    ${BPS_ROOT}/modules/presentation/SceneBuilder.cpp
    ${BPS_ROOT}/modules/presentation/MediaPixels.cpp
    ${BPS_ROOT}/modules/presentation/ScenePaint.cpp
    ${BPS_ROOT}/modules/presentation/SceneBuild.cpp
    ${BPS_ROOT}/modules/presentation/SlideBinding.cpp
    ${BPS_ROOT}/modules/presentation/SlideComposer.cpp
    ${BPS_ROOT}/modules/presentation/SlideRules.cpp
    ${BPS_ROOT}/modules/presentation/CompositorState.cpp
    ${BPS_ROOT}/modules/presentation/PresentationRuntime.cpp
    ${BPS_ROOT}/modules/presentation/PresentationSerializer.cpp
    ${BPS_ROOT}/modules/presentation/TextFormat.cpp
    ${BPS_ROOT}/modules/presentation/BlockValidator.cpp
    ${BPS_ROOT}/modules/presentation/PresentationDocument.cpp
    ${BPS_ROOT}/modules/presentation/PresentationTemplates.cpp
    ${BPS_ROOT}/modules/presentation/ShowLibrary.cpp
    ${BPS_ROOT}/modules/presentation/ShowEditor.cpp
    ${BPS_ROOT}/modules/presentation/PresentationSession.cpp
    ${BPS_ROOT}/modules/presentation/PresentationEngine.cpp
    # Search & Indexing Engine (Phase 9, docs/specs/20)
    ${BPS_ROOT}/modules/search/IndexStorage.cpp
    ${BPS_ROOT}/modules/search/SearchEngine.cpp
    # Media Engine (Phase 10, docs/specs/21)
    ${BPS_ROOT}/modules/media/MediaEngine.cpp
    # Media library: folders of images/videos + the on-disk thumbnail cache (FreeShow's rules)
    ${BPS_ROOT}/modules/media/MediaLibrary.cpp
    ${BPS_ROOT}/modules/media/ThumbnailCache.cpp
    ${BPS_ROOT}/modules/media/FrameSource.cpp
    ${BPS_ROOT}/modules/media/WindowsFrameSource.cpp
    # Design libraries: the user's overlays and templates, filed in categories (the dock's Overlays / Templates tabs)
    ${BPS_ROOT}/modules/library/DesignLibrary.cpp
    ${BPS_ROOT}/modules/library/DesignCatalogs.cpp
    ${BPS_ROOT}/modules/library/TheTableLibrary.cpp
    # Projects (FreeShow's Projects panel): folders, projects, their items, and what may be dropped where
    ${BPS_ROOT}/modules/library/ProjectLibrary.cpp
    # App settings (Settings > General / Smart Config), the hardware report, backups and crash recovery
    ${BPS_ROOT}/modules/settings/AppSettings.cpp
    ${BPS_ROOT}/modules/settings/SmartConfig.cpp
    ${BPS_ROOT}/modules/settings/DataProtection.cpp
    # Importing (FreeShow's formats): the format list, the plain-text lyric converter, and turning imports into shows
    ${BPS_ROOT}/modules/import/SongText.cpp
    ${BPS_ROOT}/modules/import/ImportEngine.cpp
    ${BPS_ROOT}/modules/import/Archive.cpp
    ${BPS_ROOT}/modules/presentation/LiveOutputController.cpp
    ${BPS_ROOT}/modules/settings/Telemetry.cpp
    # Native .vgr format (docs/specs/22)
    ${BPS_ROOT}/modules/vgr/VgrFormat.cpp
    ${BPS_ROOT}/modules/vgr/VgrFile.cpp
    # Scene Composition Engine (Phase 11, docs/specs/23)
    ${BPS_ROOT}/modules/scene/SceneCompositionEngine.cpp
    # Shared XML utility (Bible + Song providers)
    ${BPS_ROOT}/modules/xml/Xml.cpp
    # Bible Engine (Phase 12, docs/specs/24)
    ${BPS_ROOT}/modules/bible/BibleEngine.cpp
    ${BPS_ROOT}/modules/bible/BibleProviders.cpp
    ${BPS_ROOT}/modules/bible/ReferenceResolver.cpp
    ${BPS_ROOT}/modules/bible/BibleFormatter.cpp
    # Song & Lyrics Engine (Phase 13, docs/specs/25)
    ${BPS_ROOT}/modules/songs/SongEngine.cpp
    ${BPS_ROOT}/modules/songs/SongProviders.cpp
    ${BPS_ROOT}/modules/songs/ProPresenterFile.cpp
    ${BPS_ROOT}/modules/songs/Chord.cpp
    # Service Flow, Playlist & Automation Engine (Phase 14, docs/specs/26)
    ${BPS_ROOT}/modules/automation/FlowEngine.cpp
    ${BPS_ROOT}/modules/production/ProductionGraph.cpp
    ${BPS_ROOT}/modules/production/ProductionEngine.cpp
    ${BPS_ROOT}/modules/production/AudioMixer.cpp
    # Playout engine: renders the graph's audio buses to the default output device
    ${BPS_ROOT}/modules/audio/AudioEngine.cpp
    # Recording, Replay & Media Capture Engine (Phase 16, docs/specs/28)
    ${BPS_ROOT}/modules/recording/RecordingEngine.cpp
    ${BPS_ROOT}/modules/recording/WaveAudioContainer.cpp
    ${BPS_ROOT}/modules/broadcast/BroadcastEngine.cpp
    ${BPS_ROOT}/modules/broadcast/BroadcastProviders.cpp
    # Project & Data System (Phase 4, docs/specs/15)
    ${BPS_ROOT}/modules/project/Project.cpp
    ${BPS_ROOT}/modules/project/ProjectRegistry.cpp
    ${BPS_ROOT}/modules/project/ProjectManager.cpp
    ${BPS_ROOT}/modules/project/WorkspaceManager.cpp
    ${BPS_ROOT}/modules/project/StyleStore.cpp
    ${BPS_ROOT}/modules/project/GroupStore.cpp
    ${BPS_ROOT}/modules/project/PresetStore.cpp
    ${BPS_ROOT}/modules/project/ActionStore.cpp
    ${BPS_ROOT}/modules/project/OutputStore.cpp
    ${BPS_ROOT}/modules/project/ShippedConfig.cpp
    ${BPS_ROOT}/modules/project/DocumentManager.cpp
    ${BPS_ROOT}/modules/project/UndoRedoManager.cpp
    ${BPS_ROOT}/modules/project/HistoryManager.cpp
    ${BPS_ROOT}/modules/project/SessionManager.cpp
    ${BPS_ROOT}/modules/project/SnapshotManager.cpp
    ${BPS_ROOT}/modules/project/RecoveryManager.cpp
    ${BPS_ROOT}/modules/project/BackupManager.cpp
    ${BPS_ROOT}/modules/project/PackageManager.cpp
    ${BPS_ROOT}/modules/project/DependencyManager.cpp
    ${BPS_ROOT}/modules/project/ReferenceManager.cpp
    ${BPS_ROOT}/modules/project/RecentManager.cpp
    ${BPS_ROOT}/modules/project/FavoritesManager.cpp
    ${BPS_ROOT}/modules/project/TemplateManager.cpp
    ${BPS_ROOT}/modules/project/ProfileManager.cpp
    ${BPS_ROOT}/modules/project/DataManager.cpp
)

# Qt-backed PAL implementations are compiled once and shared by each native
# platform composition. They depend only on the platform contracts and Qt Core.
set(BPS_QT_PLATFORM_SOURCES
    ${BPS_ROOT}/platform/qt/QtFilesystem.cpp
    ${BPS_ROOT}/platform/qt/QtPaths.cpp
    ${BPS_ROOT}/platform/qt/QtThreading.cpp
    ${BPS_ROOT}/platform/qt/QtLibrary.cpp
    ${BPS_ROOT}/platform/qt/QtLocale.cpp)
set(BPS_QT_MEDIA_SOURCES
    ${BPS_ROOT}/platform/qt/QtMediaDevices.cpp
    ${BPS_ROOT}/platform/qt/QtFrameSource.cpp
    ${BPS_ROOT}/platform/qt/QtRecordingContainer.cpp
    ${BPS_ROOT}/platform/qt/QtDialogs.cpp
    ${BPS_ROOT}/platform/qt/QtAudio.cpp)

# Platform-specific PAL backends (headers live under platform/include — added
# to the include path below; .cpp files stay in their backend directory).
if(WIN32)
    list(APPEND BPS_CORE_SOURCES
        # These recording containers and encoders are Media Foundation based.
        # Keep them out of non-Windows builds; other platforms must register
        # their own video providers before advertising video recording.
        ${BPS_ROOT}/modules/recording/MediaFoundationMp4.cpp
        ${BPS_ROOT}/modules/recording/MediaFoundationEncode.cpp
        ${BPS_ROOT}/modules/recording/MatroskaContainer.cpp
        ${BPS_ROOT}/modules/recording/AudioFileContainer.cpp
        ${BPS_ROOT}/platform/windows/WindowsPlatform.cpp
        ${BPS_ROOT}/platform/windows/WindowsGpu.cpp
        ${BPS_ROOT}/platform/windows/WindowsThreading.cpp
        ${BPS_ROOT}/platform/windows/WindowsProcess.cpp
        ${BPS_ROOT}/platform/windows/WindowsMonitor.cpp
        ${BPS_ROOT}/platform/windows/WindowsAudio.cpp
        ${BPS_ROOT}/platform/windows/WindowsVideo.cpp
        ${BPS_ROOT}/platform/windows/WindowsNetwork.cpp
        ${BPS_ROOT}/platform/windows/WindowsPower.cpp
        ${BPS_ROOT}/platform/windows/WindowsEnvironment.cpp
        ${BPS_ROOT}/platform/windows/WindowsSocket.cpp
        ${BPS_ROOT}/platform/windows/WindowsInput.cpp
        ${BPS_ROOT}/platform/windows/WindowsDialogs.cpp
        ${BPS_ROOT}/platform/windows/WindowsNotifications.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    enable_language(OBJCXX)
    list(APPEND BPS_CORE_SOURCES
        ${BPS_ROOT}/platform/common/PosixProcess.cpp
        ${BPS_ROOT}/platform/common/PosixSocket.cpp
        ${BPS_ROOT}/platform/macos/MacOSPlatform.mm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND BPS_CORE_SOURCES
        ${BPS_ROOT}/platform/common/PosixProcess.cpp
        ${BPS_ROOT}/platform/linux/LinuxPlatform.cpp
        ${BPS_ROOT}/platform/linux/LinuxThreading.cpp
        ${BPS_ROOT}/platform/linux/LinuxMonitor.cpp
        ${BPS_ROOT}/platform/linux/LinuxNetwork.cpp
        ${BPS_ROOT}/platform/linux/LinuxPower.cpp
        ${BPS_ROOT}/platform/linux/LinuxEnvironment.cpp
        ${BPS_ROOT}/platform/linux/LinuxInput.cpp
        ${BPS_ROOT}/platform/linux/LinuxDialogs.cpp
        ${BPS_ROOT}/platform/linux/LinuxNotifications.cpp
        ${BPS_ROOT}/platform/linux/LinuxExec.cpp
        ${BPS_ROOT}/platform/common/PosixSocket.cpp)
else()
    message(FATAL_ERROR
        "VGR Presenter supports Windows, Linux, and macOS; "
        "CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}' is not supported.")
endif()

add_library(engine_core STATIC ${BPS_CORE_SOURCES})
add_library(platform_qt_services STATIC ${BPS_QT_PLATFORM_SOURCES})
target_include_directories(platform_qt_services PUBLIC ${BPS_ROOT}
    ${BPS_ROOT}/core/include ${BPS_ROOT}/modules/include
    ${BPS_ROOT}/interfaces/include ${BPS_ROOT}/platform/include)
target_link_libraries(platform_qt_services PUBLIC Threads::Threads Qt6::Core)
bps_enable_warnings(platform_qt_services)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(platform_qt_media STATIC ${BPS_QT_MEDIA_SOURCES})
    target_include_directories(platform_qt_media PUBLIC ${BPS_ROOT}
        ${BPS_ROOT}/core/include ${BPS_ROOT}/modules/include
        ${BPS_ROOT}/interfaces/include ${BPS_ROOT}/platform/include)
    # QtDialogs reaches the platform theme's dialog helpers (Qt's semi-public QPA API: private Gui headers).
    target_link_libraries(platform_qt_media PUBLIC Qt6::Multimedia PRIVATE Qt6::GuiPrivate)
    set_source_files_properties(${BPS_ROOT}/platform/qt/QtDialogs.cpp PROPERTIES COMPILE_OPTIONS "-Wno-pedantic")
    bps_enable_warnings(platform_qt_media)
endif()
if(BPS_ENGINE_FALLBACK_OPT)
    target_compile_options(engine_core PRIVATE ${BPS_ENGINE_FALLBACK_OPT})
endif()
# The executables are PIE on modern distros; the static library must be PIC
# so vtables/late-bound relocations link cleanly.
set_property(TARGET engine_core PROPERTY POSITION_INDEPENDENT_CODE ON)
# Include roots: every component keeps its headers in an include/ tree, never
# beside their .cpp: core/include, modules/include, interfaces/include,
# platform/include. All includes inside the engine are root-relative
# (core/..., modules/..., interfaces/..., platform/...).
target_include_directories(engine_core PUBLIC ${BPS_ROOT}
                                                ${BPS_ROOT}/core/include
                                                ${BPS_ROOT}/modules/include
                                                ${BPS_ROOT}/interfaces/include
                                                ${BPS_ROOT}/platform/include)
# The engine's shared Qt-backed PAL services use Qt Core. Linux and macOS also
# link Qt Multimedia adapters for audio I/O and camera preview; Windows keeps
# its existing native WASAPI and Media Foundation paths.
#
# Qt6::Gui is the ENGINE's own dependency, separate from the platform services: the text engine
# (rendering/TextEngine, presentation/SceneBuilder) lays out text with QFont/QTextLayout/QImage, and the
# image loaders use QImage. It was linked here before the platform services were split out; dropping it
# left engine_core with only Qt Core's include path ("QFont: No such file or directory"). The recorded
# Qt-header sites in tools/platform_boundary_baseline.json are exactly these.
target_link_libraries(engine_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS}
    Qt6::Gui Qt6::Network platform_qt_services)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(engine_core PUBLIC platform_qt_media)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_property(TARGET engine_core PROPERTY OBJCXX_STANDARD 23)
    set_property(TARGET engine_core PROPERTY OBJCXX_STANDARD_REQUIRED ON)
    find_library(BPS_APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(BPS_COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(BPS_IOKIT_FRAMEWORK IOKit REQUIRED)
    target_link_libraries(engine_core PUBLIC
        ${BPS_APPKIT_FRAMEWORK} ${BPS_COREGRAPHICS_FRAMEWORK} ${BPS_IOKIT_FRAMEWORK})
endif()

# zlib powers the DeflateCompressor (AssetCompressor) when available; the
# compressor degrades to Stored/RLE on hosts without zlib. zlib is the one
# deliberate hard link in the engine (NDI and libdrm are
# runtime-loaded through the PAL ILibrary seam) — it is near-universal,
# header-only on our side, and required for the deflate backend; everything
# optional stays runtime-loaded.
#
# This MinGW-w64 toolchain actually ships zlib (it's what Qt's own
# QtZlib/mingw builds link against) — find_package(ZLIB) just doesn't know
# to look inside the MinGW sysroot by default. Hinting it here finds the
# real thing instead of silently falling back to the stored-only path.
if(MINGW AND NOT ZLIB_ROOT)
    get_filename_component(BPS_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(BPS_MINGW_SYSROOT "${BPS_MINGW_BIN_DIR}/.." ABSOLUTE)
    if(EXISTS "${BPS_MINGW_SYSROOT}/include/zlib.h")
        set(ZLIB_ROOT "${BPS_MINGW_SYSROOT}")
    endif()
endif()
find_package(ZLIB)
if(ZLIB_FOUND)
    target_compile_definitions(engine_core PUBLIC BPS_HAVE_ZLIB=1)
    target_link_libraries(engine_core PUBLIC ZLIB::ZLIB)
    message(STATUS "PresentationEngine: zlib found — DeflateCompressor enabled")
else()
    message(STATUS "PresentationEngine: zlib not found — DeflateCompressor disabled (stored fallback)")
endif()
if(WIN32)
    # Win32 subsystem libraries used by the Windows PAL backends (COM dialogs
    # need ole32; IPC needs ws2_32; MF video enumeration needs mfplat/mfuuid
    # + runtimeobject for the activation factory; the live health counters in
    # WindowsPlatform.cpp need pdh).
    target_link_libraries(engine_core PUBLIC ws2_32 iphlpapi winmm shell32 user32 shcore ole32 mfplat mf mfuuid mfreadwrite runtimeobject gdi32 gdiplus dwmapi uuid windowscodecs pdh cfgmgr32)
    # The D3D11 backend's two system imports: d3d11 (the device; both DLLs ship
    # with Windows 10+) and d3dcompiler (the HLSL the backend compiles at device
    # creation). Nothing else in the engine links a graphics API.
    target_link_libraries(engine_core PUBLIC d3d11 d3dcompiler)
    # _WIN32_WINNT/WINVER gate which Win32 APIs <windows.h> and friends
    # actually declare — left undefined, MinGW's headers default to an old
    # baseline and silently omit Vista+ APIs the Windows PAL backends use
    # (GetIfTable2/MIB_IF_TABLE2 in WindowsPlatform.cpp, GetAdaptersAddresses
    # in WindowsNetwork.cpp). 0x0A00 = Windows 10, the floor Qt 6.11 itself
    # targets, so this doesn't narrow the app's own supported range.
    target_compile_definitions(engine_core PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
bps_enable_warnings(engine_core)

# ---- The cross-platform GPU backend (QRhi) --------------------------------------------------------
# One implementation of the engine's draw commands over Qt's rendering hardware interface: Direct3D 11
# on Windows, Metal on macOS, Vulkan / OpenGL on Linux (platform/qt/QtRhiBackend.cpp). Its shaders are
# GLSL, compiled by Qt Shader Tools into one .qsb per stage that carries every API's format, and embedded
# in the engine (loaded as :/bps/shaders/engine.{vert,frag}.qsb). QRhi is Qt's semi-public API: it needs
# the private Gui headers, so the build must use the same Qt it runs against (always true here).
# Off = the backend answers "unavailable" and the engine falls back to the software rasterizer.
option(BPS_ENABLE_RHI "Build the QRhi GPU backend (Metal / Vulkan / OpenGL / Direct3D 11 through Qt)" ON)
if(BPS_ENABLE_RHI)
    target_compile_definitions(engine_core PRIVATE BPS_HAVE_RHI=1)
    target_link_libraries(engine_core PRIVATE Qt6::GuiPrivate)
    qt_add_shaders(engine_core "bps_rhi_shaders"
        PREFIX "/bps/shaders"
        BASE "${BPS_ROOT}/platform/qt/shaders"
        FILES
            ${BPS_ROOT}/platform/qt/shaders/engine.vert
            ${BPS_ROOT}/platform/qt/shaders/engine.frag
    )
endif()

# ---- CLI demo app (apps/cli) ----
add_executable(bps_cli ${BPS_ROOT}/apps/cli/main.cpp)
target_link_libraries(bps_cli PRIVATE engine_core)
# The msys2 GCC 15.2 MinGW libstdc++ DLL lacks the terminal exports std::print
# needs (see the shim in the CLI's main) — force the shim on this toolchain.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
    target_compile_definitions(bps_cli PRIVATE BPS_PRINT_TERMINAL_BROKEN=1)
endif()
bps_enable_warnings(bps_cli)

# ---- Remote control app (apps/remote) — the "Remote App" frontend over the
# TCP + JSON IPC transport. ----
add_executable(bps_remote ${BPS_ROOT}/apps/remote/main.cpp)
target_link_libraries(bps_remote PRIVATE engine_core)
bps_enable_warnings(bps_remote)

# ---- Unit tests (tests/unit) ----
enable_testing()
# Unit tests are split per phase (tests/unit/tests_<phase>.cpp) so a single
# phase compiles and runs independently:
#   ./build/bps_unit_tests flow      run only the Flow/Automation suites
#   ctest -R unit_flow               same, through CTest
set(BPS_UNIT_TEST_SOURCES
    ${BPS_ROOT}/tests/unit/main.cpp
    ${BPS_ROOT}/tests/unit/tests_core.cpp
    ${BPS_ROOT}/tests/unit/tests_cams.cpp
    ${BPS_ROOT}/tests/unit/tests_modules.cpp
    ${BPS_ROOT}/tests/unit/tests_notify.cpp
    ${BPS_ROOT}/tests/unit/tests_project.cpp
    ${BPS_ROOT}/tests/unit/tests_styles.cpp
    ${BPS_ROOT}/tests/unit/tests_adaptive.cpp
    ${BPS_ROOT}/tests/unit/tests_rendering.cpp
    ${BPS_ROOT}/tests/unit/tests_display.cpp
    ${BPS_ROOT}/tests/unit/tests_presentation.cpp
    ${BPS_ROOT}/tests/unit/tests_search.cpp
    ${BPS_ROOT}/tests/unit/tests_media.cpp
    ${BPS_ROOT}/tests/unit/tests_library.cpp
    ${BPS_ROOT}/tests/unit/tests_projects.cpp
    ${BPS_ROOT}/tests/unit/tests_settings.cpp
    ${BPS_ROOT}/tests/unit/tests_import.cpp
    ${BPS_ROOT}/tests/unit/tests_vgr.cpp
    ${BPS_ROOT}/tests/unit/tests_scene.cpp
    ${BPS_ROOT}/tests/unit/tests_bible.cpp
    ${BPS_ROOT}/tests/unit/tests_song.cpp
    ${BPS_ROOT}/tests/unit/tests_flow.cpp
    ${BPS_ROOT}/tests/unit/tests_production.cpp
    ${BPS_ROOT}/tests/unit/tests_broadcast.cpp
    ${BPS_ROOT}/tests/unit/tests_thetable.cpp
    ${BPS_ROOT}/tests/unit/tests_telemetry.cpp
    ${BPS_ROOT}/tests/unit/tests_platform.cpp
    ${BPS_ROOT}/tests/unit/tests_platform_contract.cpp
)
if(WIN32)
    list(APPEND BPS_UNIT_TEST_SOURCES ${BPS_ROOT}/tests/unit/tests_recording.cpp)
endif()
add_executable(bps_unit_tests ${BPS_UNIT_TEST_SOURCES})
target_link_libraries(bps_unit_tests PRIVATE engine_core)
bps_enable_warnings(bps_unit_tests)
add_test(NAME unit COMMAND bps_unit_tests)
set(BPS_TEST_PHASES core cams modules notify project adaptive rendering display
    presentation search media library settings import vgr scene bible song flow production broadcast styles platform)
if(WIN32)
    list(APPEND BPS_TEST_PHASES recording)
endif()
foreach(phase IN LISTS BPS_TEST_PHASES)
    add_test(NAME unit_${phase} COMMAND bps_unit_tests ${phase})
endforeach()

# =============================================================================
# Qt6/QML desktop app (qml/ + src/*.cpp)
# =============================================================================

file(GLOB_RECURSE APP_QML_FILES
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.qml")

file(GLOB_RECURSE APP_ASSET_FILES
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.png")

# UI self-test driver (env-gated at runtime by VGR_SELFTEST=1, and inert without it).
# A diagnostic aid, not product code — configure with -DVGR_ENABLE_SELFTEST=OFF for
# a release build to leave it out of the binary entirely.
option(VGR_ENABLE_SELFTEST "Compile the VGR_SELFTEST UI self-test driver" ON)

qt_add_executable(appVGRPresenterUI
    src/main.cpp
    qml/services/include/services/EventBus.h
    qml/services/include/services/NameRules.h
    qml/services/EventBus.cpp
    qml/services/include/services/CursorService.h
    qml/services/CursorService.cpp
    qml/services/include/services/CallTrace.h
    qml/services/CallTrace.cpp
    qml/services/include/services/CrashHandler.h
    qml/services/CrashHandler.cpp
    qml/services/include/services/EngineBridge.h
    qml/services/EngineBridge.cpp
    qml/services/include/services/EngineWorker.h
    qml/services/EngineWorker.cpp
    qml/services/include/services/ShowConverter.h
    qml/services/ShowConverter.cpp
    qml/services/include/services/StyleSpecBuilder.h
    qml/services/StyleSpecBuilder.cpp
    qml/services/include/services/LiveOutputService.h
    qml/services/LiveOutputService.cpp
    # The live service, split by concern (see docs/long_file_audit.md):
    #   Reading = scroll mode + the measured line table it steps by
    #   Inputs  = taken camera/screen/NDI feeds, card previews, bus windows
    #   Media   = a media file on air (decode, fit, transport)
    #   Ndi     = the NDI program sender
    #   PreviewMap = the engine composition -> QML preview shape (shared)
    qml/services/LiveOutputReading.cpp
    qml/services/LiveOutputInputs.cpp
    qml/services/LiveOutputMedia.cpp
    qml/services/LiveOutputNdi.cpp
    qml/services/include/services/SlidePreviewMap.h
    qml/services/SlidePreviewMap.cpp
    qml/services/include/services/MediaOnAirState.h
    qml/services/include/services/PressTimeline.h
    qml/services/include/services/TelemetryService.h
    qml/services/TelemetryService.cpp
    qml/services/include/services/ShowService.h
    qml/services/ShowService.cpp
    qml/services/include/services/SearchService.h
    qml/services/SearchService.cpp
    qml/services/include/services/MediaLibraryService.h
    qml/services/MediaLibraryService.cpp
    qml/services/include/services/RecordingService.h
    qml/services/RecordingService.cpp
    qml/services/include/services/FlowService.h
    qml/services/FlowService.cpp
    qml/services/include/services/RoutingService.h
    qml/services/RoutingService.cpp
    qml/services/include/services/MediaThumbnailProvider.h
    qml/services/MediaThumbnailProvider.cpp
    qml/services/include/services/TextFormatService.h
    qml/services/TextFormatService.cpp
    qml/services/include/services/FontService.h
    qml/services/FontService.cpp
    qml/services/include/services/TheTableService.h
    qml/services/TheTableService.cpp
    qml/services/include/services/DesignLibraryService.h
    qml/services/DesignLibraryService.cpp
    qml/services/include/services/SettingsService.h
    qml/services/SettingsService.cpp
    qml/services/include/services/ScriptureService.h
    qml/services/ScriptureService.cpp
    qml/services/SlideBuilder.cpp
    qml/services/include/services/ImportService.h
    qml/services/ImportService.cpp
    qml/services/include/services/ProjectService.h
    qml/services/ProjectService.cpp
    qml/models/include/models/SlideListModel.h
    qml/models/SlideListModel.cpp
    qml/models/include/models/OutputListModel.h
    qml/models/OutputListModel.cpp
    qml/models/include/models/StyleListModel.h
    qml/models/StyleListModel.cpp
    qml/models/include/models/PresetListModel.h
    qml/models/PresetListModel.cpp
    qml/models/include/models/GroupListModel.h
    qml/models/GroupListModel.cpp
    qml/models/include/models/ActionListModel.h
    qml/models/ActionListModel.cpp
    qml/models/include/models/ChannelTrim.h
    qml/models/include/models/AudioInputListModel.h
    qml/models/AudioInputListModel.cpp
    qml/models/include/models/VideoSourceListModel.h
    qml/models/VideoSourceListModel.cpp
    qml/models/include/models/BusListModel.h
    qml/models/BusListModel.cpp
)

# Theme.qml has `pragma Singleton`, but qt_add_qml_module doesn't infer that
# from the file content on its own — without this, the generated qmldir
# lists it as an ordinary type ("Theme 1.0 ...") instead of a singleton
# ("singleton Theme 1.0 ..."), which is exactly what made Theme.* resolve to
# undefined at deeper nesting and forced literal hex colors everywhere.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/settings/Theme.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
# One striping rule for every list (StripedListPanel and the virtualized views
# that cannot host it) — see ListRows.qml.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/settings/ListRows.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/settings/AppCursor.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/settings/Db.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/settings/AppScale.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/settings/ScreenHelpers.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
# The Edit stage's grid (754x428) and the mapping from it to a target frame — the
# ONE definition every surface that draws a design block reads, so the canvas, a
# library card, a thumbnail and a monitor tile cannot disagree about where a block
# lands. The engine's half is in DesignLibrary.hpp (kStageWidth/kStageHeight).
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/settings/StageGrid.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/settings/MetadataFields.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
# The app's keyboard map + the Esc/Enter scope stack (qml/shortcuts/Shortcut.qml
# is the component that reads it): a hotkey is added here, and a surface names
# its action instead of spelling a chord.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/settings/Hotkeys.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
# The hover tooltip's one source (who wants a tip / what it says) — the L/R
# pills and the gain knob publish into it, the window-level ToolTipLayer draws it.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/settings/TipService.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/components/NotificationCenter.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
# Shared ModalCard stacking-order counter — see ModalStack.qml's own comment.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/settings/ModalStack.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
# WORKAROUND, remove once the link is clean without it: the compiled-QML (AOT) C++ that
# Qt generates for the Edit screen is ~47,000 lines, and linking it with this MinGW
# toolchain fails with "undefined reference to typeinfo name for ...aotBuiltFunctions
# {lambda}...::Storage". Skipping the generated code for this one file loads it from
# QML at runtime instead (a slightly slower first open of the Edit screen); every other
# file keeps its compiled form.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/screens/edit/EditScreen.qml" PROPERTIES QT_QML_SKIP_CACHEGEN TRUE)

qt_add_qml_module(appVGRPresenterUI
    URI VGRPresenterUI
    VERSION 1.0
    QML_FILES
        ${APP_QML_FILES}
    RESOURCES
        ${APP_ASSET_FILES}
    RESOURCE_PREFIX /qt/qml
)

# The Edit canvas's slide-filter shader (qml/shaders/slidefilter.frag, used by
# qml/components/SlideFilterEffect.qml). Qt 6's ShaderEffect takes a .qsb
# package, not GLSL text, so qsb compiles it here and it is referenced as
# qrc:/shaders/slidefilter.frag.qsb. One shader, one resource path — the effect
# file names it exactly once.
qt_add_shaders(appVGRPresenterUI "qml_slide_filter"
    PREFIX "/shaders"
    BASE "qml/shaders"
    FILES qml/shaders/slidefilter.frag
)

# The executable's File-Explorer/taskbar icon + version resource. RC_ICONS was
# supposed to do this but SILENTLY produced nothing in this build (the exe's
# .rsrc section carried only the manifest — no windres step was ever emitted),
# so Explorer/taskbar/pinned shortcuts showed the default icon. The explicit
# .rc below is configure-time generated (absolute .ico path substituted) and
# linked into the target — windres on MinGW, rc.exe on MSVC — which works.
# The same file is embedded below as a Qt resource so the RUNNING app's
# title-bar and taskbar icon (QGuiApplication::setWindowIcon in src/main.cpp)
# reads it back too — one file, so the exe's icon and the window's icon can't
# drift apart.
set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/app-icon.ico")
# Qt's resource system refuses absolute source paths without an explicit
# (relative) name INSIDE the resource bundle.
set_source_files_properties("${APP_ICON}" PROPERTIES QT_RESOURCE_ALIAS "app-icon.ico")

set_target_properties(appVGRPresenterUI PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    # This is a standalone application, not a DLL host.  In particular,
    # MinGW's --out-implib export pass fails on this large static Qt/QML link
    # with "0-bit reloc in dll" and the generated import library is unused.
    ENABLE_EXPORTS FALSE
)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_target_properties(appVGRPresenterUI PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/assets/Info-macos.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.vgr.presenter"
        MACOSX_BUNDLE_BUNDLE_NAME "VGR Presenter"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${BPS_APP_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${BPS_APP_VERSION}")
endif()

if(WIN32)
    # ${APP_ICON} inside the .rc must be an absolute path (windres runs with an
    # arbitrary working directory) — configure_file substitutes it verbatim.
    set(APP_RC_IN "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.rc.in")
    set(APP_RC "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
    configure_file("${APP_RC_IN}" "${APP_RC}" @ONLY)
    target_sources(appVGRPresenterUI PRIVATE "${APP_RC}")
endif()

qt_add_resources(appVGRPresenterUI "appicon"
    PREFIX "/app"
    FILES "${APP_ICON}")

# Bundle Source Serif 4 for template text and the Edit tab font picker.
set(APP_FONT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
set(APP_FONT_REGULAR "${APP_FONT_DIR}/SourceSerif4-VariableFont_opsz,wght.ttf")
set(APP_FONT_ITALIC "${APP_FONT_DIR}/SourceSerif4-Italic-VariableFont_opsz,wght.ttf")
set(APP_FONT_LICENSE "${APP_FONT_DIR}/OFL.txt")
set_source_files_properties("${APP_FONT_REGULAR}" PROPERTIES QT_RESOURCE_ALIAS "SourceSerif4-VariableFont_opsz,wght.ttf")
set_source_files_properties("${APP_FONT_ITALIC}" PROPERTIES QT_RESOURCE_ALIAS "SourceSerif4-Italic-VariableFont_opsz,wght.ttf")
set_source_files_properties("${APP_FONT_LICENSE}" PROPERTIES QT_RESOURCE_ALIAS "OFL.txt")
qt_add_resources(appVGRPresenterUI "appfonts"
    PREFIX "/fonts"
    FILES "${APP_FONT_REGULAR}" "${APP_FONT_ITALIC}" "${APP_FONT_LICENSE}")

target_link_libraries(appVGRPresenterUI
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::QuickShapes Qt6::QuickEffects Qt6::Multimedia engine_core
)

# The generated qmltyperegistrations.cpp does `#if __has_include(<SlideListModel.h>)`
# — a BARE filename check, so every QML_ELEMENT header's own directory has to
# be a search root directly, not just an ancestor of it. qml/models/include/
# models covers the List models; qml/services/include/services covers EngineBridge/EventBus
# (whose headers live in services/include/services/*.h, one level deeper
# than services/include itself — that outer path is kept too so
# `#include "services/EngineBridge.h"` from main.cpp/EngineBridge.cpp still
# resolves). Without either, the include is skipped and the generated
# registration code fails with "EventBus was not declared in this scope".
if(VGR_ENABLE_SELFTEST)
    target_sources(appVGRPresenterUI PRIVATE src/SelfTestDriver.h src/SelfTestDriver.cpp)
    target_compile_definitions(appVGRPresenterUI PRIVATE VGR_ENABLE_SELFTEST)
endif()

# C++26 toolchain (GCC 15.2) + Qt 6.11's prebuilt MinGW bits: Qt's entry-point
# shim (Qt6EntryPoint.a, built with GCC 13's mingw-w64) imports __imp___argc,
# which the GCC 15 UCRT import lib no longer exports (it exposes __p___argc()
# instead). One tiny compat object bridges the symbol — compiled INTO the
# target (not a helper archive: an archive earlier on the link line than
# libQt6EntryPoint.a gets discarded before its symbol is needed).
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
    # (.cpp with extern "C": this project enables only CXX, so a .c source
    # would be silently skipped; extern "C" keeps the symbol name unmangled.)
    file(WRITE "${CMAKE_BINARY_DIR}/ep_compat.cpp" [[
extern "C" {
int *__p___argc(void);
int *__imp___argc = 0;
}
__attribute__((constructor)) static void bps_ep_compat() {
    __imp___argc = __p___argc();
}
]])
    target_sources(appVGRPresenterUI PRIVATE "${CMAKE_BINARY_DIR}/ep_compat.cpp")
endif()

# Development builds find the KJV that ships in the source tree (SearchService).
target_compile_definitions(appVGRPresenterUI PRIVATE
    VGR_DEV_DATA_DIR="${CMAKE_SOURCE_DIR}/src/VGR_XML_JSON")

target_include_directories(appVGRPresenterUI PRIVATE
    src
    qml/services/include qml/services/include/services
    qml/models/include qml/models/include/models)

# =============================================================================
# Image formats Qt does not ship (WebP today) — a WIC-backed image-format plugin
# =============================================================================
# Qt's official MinGW build carries only qgif/qico/qjpeg/qsvg, so every Qt
# image loader in this app refused a .webp: QML's Image logged "Unsupported
# image format" over the Show Center preview, and QImageReader answered empty
# for a still being taken on air — while the ENGINE drew the same file fine
# (its decoder has always been WIC). One plugin closes the gap for every Qt
# path at once instead of a fallback per call site.
#
# It lands in <build>/imageformats/, which is exactly where Qt looks for
# plugins beside the executable (applicationDirPath is always a library path),
# so a dev run and a portable deploy behave the same. windeployqt cannot know
# about it — tools/make_portable.sh copies the directory.
#
# The plugin does nothing on a machine without the WebP codec installed (no
# capability reported), so this adds no failure mode to a PC that never had
# WebP support at all.
if(WIN32)
    qt_add_plugin(qwicimage CLASS_NAME WicImagePlugin
        qml/services/imageformats/wicimage.cpp
        qml/services/imageformats/wicimage.json)
    target_link_libraries(qwicimage PRIVATE Qt6::Gui windowscodecs ole32)
    set_target_properties(qwicimage PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/imageformats"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/imageformats"
        # No ARCHIVE/import lib: nothing links against a plugin.
        ENABLE_EXPORTS FALSE)
    add_dependencies(appVGRPresenterUI qwicimage)
endif()

# =============================================================================
# Lock-scope guard (tools/check_lock_scope.mjs) — part of the default build
# =============================================================================
# A lock held across a PAL/provider/cross-engine call — or across a
# callback/event dispatch (EventBus::Publish, a listener notification, a stored
# std::function) — is what turned a slow NDI/encoder/disk call into an app-wide
# lock pile-up. The scan covers the feature modules, core/resources AND the
# platform layer, whose waits are the OS's own (a device read, a Win32 wait
# object, a socket, a capture callback) and are unbounded. The scan is a heuristic source pass but it is cheap (well under
# a second over the ~290 module sources with node), so it runs on every build and
# a NEW finding FAILS the build with `file:line (lock at :N)` plus the offending
# source line — the offending file is printed, not buried in the middle of a
# parallel build log.
#
# Waive a site (justification required, see AGENTS.md "Lock-scope guard") with
# `// lock-ok: <reason>` on the flagged line. Disabling the guard for a whole
# build is a last resort: -DBPS_ENABLE_LOCK_SCOPE_CHECK=OFF. The dispatch sites
# that already existed are recorded in tools/lock_scope_baseline.json, so they
# are reported as known debt while only NEW findings fail; after hoisting one out
# of its lock, shrink the ledger with `node tools/check_lock_scope.mjs
# --update-baseline`. Stale waivers (a lock-ok that no longer waives anything)
# fail too.
option(BPS_ENABLE_LOCK_SCOPE_CHECK
    "Fail the build when a module lock is held across a blocking call or a dispatch" ON)

if(BPS_ENABLE_LOCK_SCOPE_CHECK)
    # node is wanted only by this guard; like the dev scripts' CMAKE_BIN/
    # MINGW_BIN/QT_BIN, allow the checkout to override it without editing here.
    find_program(BPS_NODE_EXE
        NAMES node node.exe
        HINTS ENV NODE_BIN
        DOC "node runtime used by the lock-scope guard")
    if(NOT BPS_NODE_EXE)
        message(FATAL_ERROR
            "check_lock_scope needs node, which was not found on PATH. Install node, "
            "pass -DBPS_NODE_EXE=/path/to/node (or set NODE_BIN), or build once without "
            "the guard: -DBPS_ENABLE_LOCK_SCOPE_CHECK=OFF")
    endif()
    # One scope variable, shared by the build gate and the CTest entry, so the
    # two can never drift apart. Three roots: the feature modules,
    # core/resources — ResourceManager's sampler held its own mutex across an
    # EventBus dispatch whose subscriber re-enters the manager, which froze the
    # GUI thread (a settings write waits on the AdaptiveRuntime lock the
    # deadlocked sampler owns) far away from the bug — and the PLATFORM layer,
    # where the waits are the operating system's own (a device read, a Win32 wait
    # object, a socket, a capture callback) and have no bound at all: a lock held
    # across one blocks every thread that needs it for as long as the driver
    # takes. A list expands to one argument per root.
    set(BPS_LOCK_SCOPE_TARGET
        "${BPS_ROOT}/modules" "${BPS_ROOT}/core/resources" "${BPS_ROOT}/platform")

    # Two commands, in this order, and the order is the point: the self-test
    # proves the guard's two SILENT rules still fire (a locked line calling a
    # helper that reaches a blocking call — in this file or another translation
    # unit — and a locked method calling something that re-locks the same mutex),
    # and only then is the scan on the real tree worth reading. A clean scan from
    # a rule that no longer works looks exactly like a clean tree, so the
    # fixtures in tools/lock_scope_fixtures are checked first and a broken rule
    # fails the build here rather than passing silently forever.
    add_custom_target(lock_scope_check ALL
        COMMAND "${BPS_NODE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_lock_scope.mjs" --self-test
        COMMAND "${BPS_NODE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_lock_scope.mjs"
                ${BPS_LOCK_SCOPE_TARGET}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "check_lock_scope: its own rules (fixtures), then locks held across a blocking call or a dispatch"
        VERBATIM)
    # Named targets get the same gate, so `--target bps_unit_tests` or a release
    # build of the app cannot skip the guard by not building `all`.
    add_dependencies(appVGRPresenterUI lock_scope_check)
    add_dependencies(bps_unit_tests lock_scope_check)

    # Also registered with CTest, so a violation lands in the test report as a
    # failing `lock_scope` test and CI can run the scan on its own —
    # `ctest -R lock_scope` — without depending on the build graph. The
    # WORKING_DIRECTORY matters: the guard resolves its baseline ledger relative
    # to the current directory, so running from build/ would miss it and fail
    # every baselined site. The entry follows the same ON/OFF as the build gate,
    # so the documented "skip the guard for one build" escape hatch disables it in
    # both places at once. TIMEOUT is a backstop: the scan takes well under a
    # second, so anything near it means node is wedged, not that the tree is slow.
    add_test(NAME lock_scope
        COMMAND "${BPS_NODE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_lock_scope.mjs"
                ${BPS_LOCK_SCOPE_TARGET}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(lock_scope PROPERTIES
        LABELS "lock-scope"
        TIMEOUT 120)
    # The guard's own rules, as a test of their own (`ctest -R lock_scope_selftest`),
    # so a rule that rots is named as such instead of showing up as "the scan
    # passed". Cheap: it is the same scan over eight short fixtures.
    add_test(NAME lock_scope_selftest
        COMMAND "${BPS_NODE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_lock_scope.mjs" --self-test
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(lock_scope_selftest PROPERTIES
        LABELS "lock-scope"
        TIMEOUT 120)
endif()

# =============================================================================
# GUI-thread blocking guard (tools/check_gui_thread_blocks.mjs) — default build
# =============================================================================
# The lock-scope guard above asks whether a lock is held across a blocking call.
# This one asks the complementary question the hang dumps kept answering: can a
# QML BINDING or signal handler — i.e. the interface thread — reach a call that
# parks it? Recorded stalls name the cost: TelemetryService::poll ->
# WindowsPlatform::Sample (GetIfTable2/GetSystemTimes), pumpMeterSnapshot ->
# WindowsAudio::InputLevels (a contended mutex), SettingsService::setValue ->
# ApplyToEngine -> AdaptiveRuntime::ApplyOptimization, EngineBridge::write ->
# Logger::Log. Each is a Q_INVOKABLE or bound property, and each froze the
# window until the call returned.
#
# The scan walks the QML-facing layer's headers for Q_INVOKABLE / Q_PROPERTY
# READ getters, resolves each to its body (in the .cpp, or inline), and follows
# private helpers transitively — the entry point is always the innocent-looking
# one, and the parking happens three calls down. An entry point that hops off
# the interface thread first (Qt::QueuedConnection, a worker thread) is exempt:
# that is the fix pattern, and the guard rewards it.
#
# HARD findings (an unbounded wait in the body) fail the build — the OS, a
# device, a file, a sleep — so a NEW one cannot reach main by accident. Existing
# sites live in tools/gui_thread_baseline.json as known debt; shrink it as they
# are fixed (`--update-baseline`). SOFT findings (a lock acquire, a cross-engine
# singleton) are an inventory for the audit, printed on demand with `--all`, and
# never fail a build on their own — most such sites are uncontended, and gating
# on them would train everyone to ignore the guard. Waive a site with
# `// gui-ok: <reason>` on its declaration; a stale waiver fails too.
option(BPS_ENABLE_GUI_THREAD_CHECK
    "Fail the build when a QML-exposed entry point can park the interface thread" ON)

if(BPS_ENABLE_GUI_THREAD_CHECK)
    if(NOT BPS_NODE_EXE)
        # Declared by the lock-scope guard above; without it, find node here so
        # either guard can be enabled on its own.
        find_program(BPS_NODE_EXE
            NAMES node node.exe
            HINTS ENV NODE_BIN
            DOC "node runtime used by the interface-thread guard")
    endif()
    if(NOT BPS_NODE_EXE)
        message(FATAL_ERROR
            "check_gui_thread_blocks needs node, which was not found on PATH. Install "
            "node, pass -DBPS_NODE_EXE=/path/to/node (or set NODE_BIN), or build once "
            "without the guard: -DBPS_ENABLE_GUI_THREAD_CHECK=OFF")
    endif()

    # The QML-facing layer: these are the functions a binding or handler can
    # call, and therefore the only ones that can park the interface thread.
    set(BPS_GUI_THREAD_TARGET "${CMAKE_CURRENT_SOURCE_DIR}/qml/services"
                              "${CMAKE_CURRENT_SOURCE_DIR}/qml/models")

    add_custom_target(gui_thread_check ALL
        COMMAND "${BPS_NODE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_gui_thread_blocks.mjs"
                ${BPS_GUI_THREAD_TARGET}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "check_gui_thread_blocks: QML-reachable calls that park the interface thread"
        VERBATIM)
    add_dependencies(appVGRPresenterUI gui_thread_check)
    add_dependencies(bps_unit_tests gui_thread_check)

    # Also a CTest entry (`ctest -R gui_thread`), same ON/OFF switch as the
    # build gate, and the same WORKING_DIRECTORY note as the lock-scope guard:
    # the baseline ledger resolves relative to the current directory.
    add_test(NAME gui_thread
        COMMAND "${BPS_NODE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_gui_thread_blocks.mjs"
                ${BPS_GUI_THREAD_TARGET}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(gui_thread PROPERTIES
        LABELS "gui-thread"
        TIMEOUT 120)
endif()

# =============================================================================
# Settings-key guard (tools/check_settings_keys.mjs) — default build
# =============================================================================
# AppSettings refuses a key it does not declare ("there is no setting '…'"), and
# SettingsService turns that into a RED TOAST in front of the operator while the
# write is simply lost. The interface could therefore name a key that exists
# nowhere in the engine and nothing noticed until the control was touched:
# Settings · Outputs' "Fill" wrote `outputs.defaultMediaFit` for months with no
# descriptor behind it, and every pick toasted and forgot itself.
#
# This gate reads BOTH sides instead of restating them — the keys qml/ passes to
# SettingsService (call form and the `values[…]` map) and every descriptor in
# AppSettings.cpp, including the per-tab keys generated from the kFillTabs table
# — then fails on a key that is used and not declared.
option(BPS_ENABLE_SETTINGS_KEYS_CHECK
    "Fail the build when the interface names a settings key AppSettings does not declare" ON)

if(BPS_ENABLE_SETTINGS_KEYS_CHECK)
    if(NOT BPS_NODE_EXE)
        # Declared by the guards above; find node here so this one can also be
        # enabled on its own.
        find_program(BPS_NODE_EXE
            NAMES node node.exe
            HINTS ENV NODE_BIN
            DOC "node runtime used by the settings-key guard")
    endif()
    if(NOT BPS_NODE_EXE)
        message(FATAL_ERROR
            "check_settings_keys needs node, which was not found on PATH. Install "
            "node, pass -DBPS_NODE_EXE=/path/to/node (or set NODE_BIN), or build once "
            "without the guard: -DBPS_ENABLE_SETTINGS_KEYS_CHECK=OFF")
    endif()

    add_custom_target(settings_keys_check ALL
        COMMAND "${BPS_NODE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_settings_keys.mjs"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "check_settings_keys: settings keys the interface uses, declared in AppSettings"
        VERBATIM)
    add_dependencies(appVGRPresenterUI settings_keys_check)
    add_dependencies(bps_unit_tests settings_keys_check)

    # Also a CTest entry (`ctest -R settings_keys`), same ON/OFF switch.
    add_test(NAME settings_keys
        COMMAND "${BPS_NODE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_settings_keys.mjs"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(settings_keys PROPERTIES
        LABELS "settings-keys"
        TIMEOUT 120)
endif()

# ============================================================================
# Platform boundary guard (tools/check_platform_boundary.mjs) — default build
# ============================================================================
# The cage (docs/platform/qt_first_platform.md) is only real if it is enforced:
# an OS header belongs in platform/ and nowhere else, and the production engine
# (core / modules / interfaces) may not reach for Qt either — it asks the PAL.
# A NEW violation fails the build; the sites that predate the rule are listed in
# tools/platform_boundary_baseline.json with the reason they still exist.
# Independent of the other guards: it keeps running when one of them is turned off.
if(NOT BPS_NODE_EXE)
    find_program(BPS_NODE_EXE
        NAMES node node.exe
        HINTS ENV NODE_BIN
        DOC "node runtime used by the build guards")
endif()
if(BPS_NODE_EXE)
    add_custom_target(platform_boundary_check ALL
        COMMAND "${BPS_NODE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_platform_boundary.mjs"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "check_platform_boundary: OS/Qt headers outside the platform cage"
        VERBATIM)
    add_dependencies(appVGRPresenterUI platform_boundary_check)
    add_dependencies(bps_unit_tests platform_boundary_check)
    add_test(NAME platform_boundary
        COMMAND "${BPS_NODE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_platform_boundary.mjs"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(platform_boundary PROPERTIES
        LABELS "platform"
        TIMEOUT 120)
endif()

# =============================================================================
# Singleton-ownership guard (tools/check_singleton_ownership.mjs) — default build
# =============================================================================
# A QML_SINGLETON's create() result is deleted by the QML engine when it is destroyed. Returning
# a function-local static without QJSEngine::CppOwnership frees non-heap memory: the app died on
# every exit with STATUS_HEAP_CORRUPTION (FontService did, ~130 Windows crash reports and nothing in
# the app's own log). This gate fails the build on a create() that returns without setting ownership.
option(BPS_ENABLE_SINGLETON_OWNERSHIP_CHECK
    "Fail the build when a QML singleton's create() hands the engine an object it will delete" ON)
if(BPS_ENABLE_SINGLETON_OWNERSHIP_CHECK)
    if(NOT BPS_NODE_EXE)
        find_program(BPS_NODE_EXE
            NAMES node node.exe
            HINTS ENV NODE_BIN
            DOC "node runtime used by the build guards")
    endif()
    if(NOT BPS_NODE_EXE)
        message(FATAL_ERROR
            "check_singleton_ownership needs node, which was not found on PATH. Install node, "
            "pass -DBPS_NODE_EXE=/path/to/node (or set NODE_BIN), or build once without the "
            "guard: -DBPS_ENABLE_SINGLETON_OWNERSHIP_CHECK=OFF")
    endif()

    add_custom_target(singleton_ownership_check ALL
        COMMAND "${BPS_NODE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_singleton_ownership.mjs"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "check_singleton_ownership: QML singletons the engine would delete"
        VERBATIM)
    add_dependencies(appVGRPresenterUI singleton_ownership_check)
    add_dependencies(bps_unit_tests singleton_ownership_check)

    add_test(NAME singleton_ownership
        COMMAND "${BPS_NODE_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_singleton_ownership.mjs"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(singleton_ownership PROPERTIES
        LABELS "singleton-ownership"
        TIMEOUT 120)
endif()
