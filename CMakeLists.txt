cmake_minimum_required(VERSION 3.21)
project(VGRPresenterUI VERSION 0.1.0 LANGUAGES CXX)

# C++23 project-wide — the Qt app and the engine (src/PresentationEngine/)
# build as one project now, one CMakeLists.txt, so they share one dialect
# instead of two files trying to agree with each other. CMAKE_CXX_STANDARD
# here is inherited by every target below, Qt and engine alike; nothing else
# needs to set it again.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.11 REQUIRED COMPONENTS Quick QuickControls2 QuickShapes)

qt_standard_project_setup(REQUIRES 6.11)

# =============================================================================
# Backend engine (src/PresentationEngine/) — was its own separate project
# with its own CMakeLists.txt; folded directly into this one 2026-09-19 so
# there's a single build description for the whole app, not two files that
# can drift out of sync. Its own cmake/ helpers (warnings profile, the
# Threads dependency) still live under src/PresentationEngine/cmake/ and are
# included below, just from this file instead of that one.
#
# Cross-platform by design: the platform PAL sources branch WIN32/APPLE/
# linux below, all three fully implemented (not stubs) — this app ships on
# every platform Qt6 does, not just Windows, so nothing here assumes
# otherwise.
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
    ${BPS_ROOT}/platform/common/Filesystem.cpp
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
    ${BPS_ROOT}/modules/display/OutputRouter.cpp
    ${BPS_ROOT}/modules/display/DisplayProfiles.cpp
    ${BPS_ROOT}/modules/display/DisplayEngine.cpp
    ${BPS_ROOT}/modules/display/DisplayTest.cpp
    # Presentation Engine (Phase 8, docs/specs/19)
    ${BPS_ROOT}/modules/presentation/PresentationStateMachine.cpp
    ${BPS_ROOT}/modules/presentation/PresentationCues.cpp
    ${BPS_ROOT}/modules/presentation/PresentationTimeline.cpp
    ${BPS_ROOT}/modules/presentation/PresentationNavigator.cpp
    ${BPS_ROOT}/modules/presentation/ScriptureSlides.cpp
    ${BPS_ROOT}/modules/presentation/PresentationValidator.cpp
    ${BPS_ROOT}/modules/presentation/PresentationCompiler.cpp
    ${BPS_ROOT}/modules/presentation/SceneBuilder.cpp
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
    ${BPS_ROOT}/modules/songs/Chord.cpp
    # Service Flow, Playlist & Automation Engine (Phase 14, docs/specs/26)
    ${BPS_ROOT}/modules/automation/FlowEngine.cpp
    ${BPS_ROOT}/modules/production/ProductionGraph.cpp
    ${BPS_ROOT}/modules/production/ProductionEngine.cpp
    # Recording, Replay & Media Capture Engine (Phase 16, docs/specs/28)
    ${BPS_ROOT}/modules/recording/RecordingEngine.cpp
    ${BPS_ROOT}/modules/broadcast/BroadcastEngine.cpp
    ${BPS_ROOT}/modules/broadcast/BroadcastProviders.cpp
    # Project & Data System (Phase 4, docs/specs/15)
    ${BPS_ROOT}/modules/project/Project.cpp
    ${BPS_ROOT}/modules/project/ProjectRegistry.cpp
    ${BPS_ROOT}/modules/project/ProjectManager.cpp
    ${BPS_ROOT}/modules/project/WorkspaceManager.cpp
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

# Platform-specific PAL backends (headers live under platform/include — added
# to the include path below; .cpp files stay in their backend directory).
if(WIN32)
    list(APPEND BPS_CORE_SOURCES
        ${BPS_ROOT}/platform/windows/WindowsPlatform.cpp
        ${BPS_ROOT}/platform/windows/WindowsGpu.cpp
        ${BPS_ROOT}/platform/windows/WindowsPaths.cpp
        ${BPS_ROOT}/platform/windows/WindowsThreading.cpp
        ${BPS_ROOT}/platform/windows/WindowsProcess.cpp
        ${BPS_ROOT}/platform/windows/WindowsLibrary.cpp
        ${BPS_ROOT}/platform/windows/WindowsMonitor.cpp
        ${BPS_ROOT}/platform/windows/WindowsAudio.cpp
        ${BPS_ROOT}/platform/windows/WindowsVideo.cpp
        ${BPS_ROOT}/platform/windows/WindowsNetwork.cpp
        ${BPS_ROOT}/platform/windows/WindowsPower.cpp
        ${BPS_ROOT}/platform/windows/WindowsClipboard.cpp
        ${BPS_ROOT}/platform/windows/WindowsEnvironment.cpp
        ${BPS_ROOT}/platform/windows/WindowsLocale.cpp
        ${BPS_ROOT}/platform/windows/WindowsSocket.cpp
        ${BPS_ROOT}/platform/windows/WindowsInput.cpp
        ${BPS_ROOT}/platform/windows/WindowsDialogs.cpp
        ${BPS_ROOT}/platform/windows/WindowsNotifications.cpp)
elseif(APPLE)
    list(APPEND BPS_CORE_SOURCES ${BPS_ROOT}/platform/macos/MacOSPlatform.cpp)
else()
    list(APPEND BPS_CORE_SOURCES
        ${BPS_ROOT}/platform/linux/LinuxPlatform.cpp
        ${BPS_ROOT}/platform/linux/LinuxPaths.cpp
        ${BPS_ROOT}/platform/linux/LinuxThreading.cpp
        ${BPS_ROOT}/platform/linux/LinuxProcess.cpp
        ${BPS_ROOT}/platform/linux/LinuxLibrary.cpp
        ${BPS_ROOT}/platform/linux/LinuxMonitor.cpp
        ${BPS_ROOT}/platform/linux/LinuxAudio.cpp
        ${BPS_ROOT}/platform/linux/LinuxNetwork.cpp
        ${BPS_ROOT}/platform/linux/LinuxPower.cpp
        ${BPS_ROOT}/platform/linux/LinuxClipboard.cpp
        ${BPS_ROOT}/platform/linux/LinuxEnvironment.cpp
        ${BPS_ROOT}/platform/linux/LinuxLocale.cpp
        ${BPS_ROOT}/platform/linux/LinuxInput.cpp
        ${BPS_ROOT}/platform/linux/LinuxDialogs.cpp
        ${BPS_ROOT}/platform/linux/LinuxNotifications.cpp
        ${BPS_ROOT}/platform/linux/LinuxExec.cpp
        ${BPS_ROOT}/platform/common/PosixSocket.cpp)
endif()

add_library(engine_core STATIC ${BPS_CORE_SOURCES})
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
target_link_libraries(engine_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
# zlib powers the DeflateCompressor (AssetCompressor) when available; the
# compressor degrades to Stored/RLE on hosts without zlib. zlib is the one
# deliberate hard link in the engine (NDI, libasound and libdrm are
# runtime-loaded through the PAL ILibrary seam) — it is near-universal,
# header-only on our side, and required for the deflate backend; everything
# optional stays runtime-loaded.
#
# This MinGW-w64 toolchain actually ships zlib (it's what Qt's own
# QtZlib/mingw builds link against) — find_package(ZLIB) just doesn't know
# to look inside the MinGW sysroot by default. Hinting it here finds the
# real thing instead of silently falling back to the stored-only path.
if(MINGW AND NOT ZLIB_ROOT)
    get_filename_component(BPS_MINGW_SYSROOT "${CMAKE_CXX_COMPILER}/../.." ABSOLUTE)
    if(EXISTS "${BPS_MINGW_SYSROOT}/x86_64-w64-mingw32/include/zlib.h")
        set(ZLIB_ROOT "${BPS_MINGW_SYSROOT}/x86_64-w64-mingw32")
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
    # + runtimeobject for the activation factory).
    target_link_libraries(engine_core PUBLIC ws2_32 iphlpapi winmm shell32 user32 shcore ole32 mfplat mf mfuuid mfreadwrite runtimeobject gdi32 uuid windowscodecs)
    # _WIN32_WINNT/WINVER gate which Win32 APIs <windows.h> and friends
    # actually declare — left undefined, MinGW's headers default to an old
    # baseline and silently omit Vista+ APIs the Windows PAL backends use
    # (GetIfTable2/MIB_IF_TABLE2 in WindowsPlatform.cpp, GetAdaptersAddresses
    # in WindowsNetwork.cpp). 0x0A00 = Windows 10, the floor Qt 6.11 itself
    # targets, so this doesn't narrow the app's own supported range.
    target_compile_definitions(engine_core PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
bps_enable_warnings(engine_core)

# ---- CLI demo app (apps/cli) ----
add_executable(bps_cli ${BPS_ROOT}/apps/cli/main.cpp)
target_link_libraries(bps_cli PRIVATE engine_core)
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
    ${BPS_ROOT}/tests/unit/tests_recording.cpp
    ${BPS_ROOT}/tests/unit/tests_broadcast.cpp
    ${BPS_ROOT}/tests/unit/tests_thetable.cpp
    ${BPS_ROOT}/tests/unit/tests_telemetry.cpp
)
add_executable(bps_unit_tests ${BPS_UNIT_TEST_SOURCES})
target_link_libraries(bps_unit_tests PRIVATE engine_core)
bps_enable_warnings(bps_unit_tests)
add_test(NAME unit COMMAND bps_unit_tests)
foreach(phase core cams modules notify project adaptive rendering display
              presentation search media library settings import vgr scene bible song flow production recording broadcast)
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
    qml/services/EventBus.cpp
    qml/services/include/services/CrashHandler.h
    qml/services/CrashHandler.cpp
    qml/services/include/services/EngineBridge.h
    qml/services/EngineBridge.cpp
    qml/services/include/services/ShowConverter.h
    qml/services/ShowConverter.cpp
    qml/services/include/services/LiveOutputService.h
    qml/services/LiveOutputService.cpp
    qml/services/include/services/TelemetryService.h
    qml/services/TelemetryService.cpp
    qml/services/include/services/ShowService.h
    qml/services/ShowService.cpp
    qml/services/include/services/SearchService.h
    qml/services/SearchService.cpp
    qml/services/include/services/MediaLibraryService.h
    qml/services/MediaLibraryService.cpp
    qml/services/include/services/MediaThumbnailProvider.h
    qml/services/MediaThumbnailProvider.cpp
    qml/services/include/services/TextFormatService.h
    qml/services/TextFormatService.cpp
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
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/settings/AppCursor.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/components/NotificationCenter.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)

qt_add_qml_module(appVGRPresenterUI
    URI VGRPresenterUI
    VERSION 1.0
    QML_FILES
        ${APP_QML_FILES}
    RESOURCES
        ${APP_ASSET_FILES}
    RESOURCE_PREFIX /qt/qml
)

set_target_properties(appVGRPresenterUI PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

target_link_libraries(appVGRPresenterUI
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::QuickShapes engine_core
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

# Development builds find the KJV that ships in the source tree (SearchService).
target_compile_definitions(appVGRPresenterUI PRIVATE
    VGR_DEV_DATA_DIR="${CMAKE_SOURCE_DIR}/src/VGR_XML_JSON")

target_include_directories(appVGRPresenterUI PRIVATE
    src
    qml/services/include qml/services/include/services
    qml/models/include qml/models/include/models)
