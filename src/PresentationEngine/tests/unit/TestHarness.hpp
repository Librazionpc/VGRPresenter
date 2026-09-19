#pragma once
// Shared unit-test harness (docs/specs/00 §8). Split from tests/unit/main.cpp
// so each phase compiles and runs independently: ./bps_unit_tests [phase]
// Engine core unit tests (docs/specs/00 §8: unit + failure coverage).
// Build: `cmake -B build && cmake --build build && ./build/bps_unit_tests`

#include "core/common/Common.hpp"
#include "core/config/ConfigurationManager.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/kernel/Kernel.hpp"
#include "core/lifecycle/LifecycleManager.hpp"
#include "core/logging/Logger.hpp"
#include "core/memory/MemoryManager.hpp"
#include "core/modules/ModuleManager.hpp"
#include "core/plugins/PluginManager.hpp"
#include "core/resources/ResourceManager.hpp"
#include "core/services/ServiceManager.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"
#include "core/threading/ThreadPool.hpp"
#include "core/assets/AssetManager.hpp"
#include "core/database/DatabaseManager.hpp"
#include "core/display/DisplayManager.hpp"
#include "core/drivers/DriverManager.hpp"
#include "core/ipc/IpcClient.hpp"
#include "core/ipc/IpcServer.hpp"
#include "core/rendering/RendererManager.hpp"
#include "interfaces/IDisplay.hpp"
#include "interfaces/IDriver.hpp"
#include "interfaces/IRenderer.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"
#include "platform/OsTag.hpp"
#include "modules/media/MediaModule.hpp"
#include "modules/presentation/PresentationModule.hpp"
#include "modules/songs/SongsModule.hpp"
#include "modules/content/AssetCache.hpp"
#include "modules/content/AssetCompressor.hpp"
#include "modules/content/AssetDatabase.hpp"
#include "modules/content/AssetIndexer.hpp"
#include "modules/content/AssetLoader.hpp"
#include "modules/content/AssetManager.hpp"
#include "modules/content/AssetSerializer.hpp"
#include "modules/notification/NotificationFactory.hpp"
#include "modules/notification/NotificationInterfaces.hpp"
#include "modules/notification/NotificationQueue.hpp"
#include "modules/notification/NotificationRules.hpp"
#include "modules/notification/NotificationProviders.hpp"
#include "modules/notification/NotificationService.hpp"
#include "modules/adaptive/AdaptiveRuntime.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/display/DisplayEngine.hpp"
#include "modules/display/DisplayTest.hpp"
#include "modules/display/OutputRouter.hpp"
#include "modules/display/Providers.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationCues.hpp"
#include "modules/presentation/PresentationNavigator.hpp"
#include "modules/presentation/PresentationStateMachine.hpp"
#include "modules/presentation/PresentationTimeline.hpp"
#include "modules/presentation/PresentationValidator.hpp"
#include "modules/search/SearchEngine.hpp"
#include "modules/media/MediaEngine.hpp"
#include "modules/vgr/VgrFormat.hpp"
#include "modules/scene/SceneCompositionEngine.hpp"
#include "modules/automation/FlowEngine.hpp"
#include "modules/bible/BibleEngine.hpp"
#include "modules/bible/ReferenceResolver.hpp"
#include "modules/songs/Chord.hpp"
#include "modules/songs/SongEngine.hpp"
#include "modules/project/DataManager.hpp"
#include "modules/project/BackupManager.hpp"
#include "modules/project/DependencyManager.hpp"
#include "modules/project/DocumentManager.hpp"
#include "modules/project/FavoritesManager.hpp"
#include "modules/project/HistoryManager.hpp"
#include "modules/project/IDocumentHandler.hpp"
#include "modules/project/PackageManager.hpp"
#include "modules/project/ProfileManager.hpp"
#include "modules/project/ProjectManager.hpp"
#include "modules/project/RecentManager.hpp"
#include "modules/project/RecoveryManager.hpp"
#include "modules/project/ReferenceManager.hpp"
#include "modules/project/SessionManager.hpp"
#include "modules/project/SnapshotManager.hpp"
#include "modules/project/TemplateManager.hpp"
#include "modules/project/UndoRedoManager.hpp"
#include "modules/project/WorkspaceManager.hpp"
#include "modules/content/AssetValidator.hpp"
#include "modules/content/AssetWatcher.hpp"
#include "modules/content/ContentManager.hpp"
#include "modules/content/IExporter.hpp"
#include "modules/content/IImporter.hpp"
#include "modules/content/ImportManager.hpp"
#include "modules/content/ThumbnailManager.hpp"
#include "modules/content/Uuid.hpp"
#include "modules/content/Vfs.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace bps;

inline int g_checks = 0;
inline int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

// --- Fakes for the driver/display/renderer registries ---
class FakeDriver final : public IDriver {
public:
    int probeCalls = 0, enableCalls = 0, disableCalls = 0;
    const char* ServiceName() const noexcept override { return "FakeDriver"; }
    Result<void> Probe() override { ++probeCalls; return Ok(); }
    Result<void> Enable() override { ++enableCalls; return Ok(); }
    Result<void> Disable() override { ++disableCalls; return Ok(); }
};
class FakeDisplay final : public IDisplay {
public:
    int setOutputCalls = 0;
    explicit FakeDisplay(int count) {
        for (int i = 0; i < count; ++i)
            infos_.push_back(DisplayInfo{i, 1920 + i, 1080, 60, i == 0});
    }
    const char* ServiceName() const noexcept override { return "FakeDisplay"; }
    std::vector<DisplayInfo> Displays() const override { return infos_; }
    Result<void> SetOutput(int index, bool enabled) override {
        (void)index;
        (void)enabled;
        ++setOutputCalls;
        return Ok();
    }

private:
    std::vector<DisplayInfo> infos_;
};
class FakeRenderer final : public IRenderer {
public:
    int beginCalls = 0, endCalls = 0, presentCalls = 0;
    const char* ServiceName() const noexcept override { return "FakeRenderer"; }
    Result<void> BeginFrame() override { ++beginCalls; return Ok(); }
    Result<void> EndFrame() override { ++endCalls; return Ok(); }
    Result<void> Present() override { ++presentCalls; return Ok(); }
};

// ---------------------------------------------------------------------------
namespace content = bps::content;
namespace r = bps::rendering;
namespace d = bps::display;
namespace p = bps::presentation;
namespace s = bps::search;
namespace m = bps::media;
namespace v = bps::vgr;
namespace sc = bps::scene;
namespace bb = bps::bible;
namespace sn = bps::song;
namespace fa = bps::automation;
