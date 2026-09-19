# Documentation Index

This directory is the knowledge base for the Believers Presentation Software engine.

## Start here

| Document                                                                 | What it answers                                                                                                                                                                                                                                                               |
| ------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **[api/README.md](api/README.md)**                                       | **Every public API that exists + how a UI consumes it.** The contract between frontends and the engine: full catalog of core systems, feature engines, the platform PAL, and every EventBus topic, plus copy-paste consumption recipes. **Read this before building any UI.** |
| [architecture/SystemArchitecture.md](architecture/SystemArchitecture.md) | The overall system design, layers, and communication paths.                                                                                                                                                                                                                   |
| [architecture/Conformance.md](architecture/Conformance.md)               | Audit trail: every architecture expectation vs. what is implemented (✅/◑/○).                                                                                                                                                                                                 |
| [architecture/StartupSequence.md](architecture/StartupSequence.md)       | The Kernel boot sequence (29 systems) in detail.                                                                                                                                                                                                                              |

## Specifications (behavior — source of truth)

| Spec                                                                         | System                            |
| ---------------------------------------------------------------------------- | --------------------------------- |
| [specs/01-kernel.md](specs/01-kernel.md)                                     | Kernel, lifecycle, panic/recovery |
| [specs/02-logger.md](specs/02-logger.md)                                     | Structured logging                |
| [specs/03-configuration-manager.md](specs/03-configuration-manager.md)       | Config, profiles, migration       |
| [specs/04-service-manager.md](specs/04-service-manager.md)                   | DI / service registry             |
| [specs/05-event-bus.md](specs/05-event-bus.md)                               | Typed pub/sub event bus           |
| [specs/06-thread-pool.md](specs/06-thread-pool.md)                           | Async task execution              |
| [specs/07-task-scheduler.md](specs/07-task-scheduler.md)                     | Timers, cron, retries             |
| [specs/08-module-manager.md](specs/08-module-manager.md)                     | Feature modules                   |
| [specs/09-plugin-manager.md](specs/09-plugin-manager.md)                     | Plugin discovery/isolation        |
| [specs/10-resource-manager.md](specs/10-resource-manager.md)                 | Machine telemetry, pressure       |
| [specs/11-memory-manager.md](specs/11-memory-manager.md)                     | Allocation budgets                |
| [specs/12-lifecycle-manager.md](specs/12-lifecycle-manager.md)               | Entity state machine              |
| [specs/13-cams.md](specs/13-cams.md)                                         | Content & asset management        |
| [specs/14-notifications.md](specs/14-notifications.md)                       | Notification service              |
| [specs/15-project-system.md](specs/15-project-system.md)                     | .vgr projects & documents         |
| [specs/16-adaptive-runtime.md](specs/16-adaptive-runtime.md)                 | Adaptive quality                  |
| [specs/17-rendering-engine.md](specs/17-rendering-engine.md)                 | Rendering                         |
| [specs/18-display-engine.md](specs/18-display-engine.md)                     | Displays & outputs                |
| [specs/19-presentation-engine.md](specs/19-presentation-engine.md)           | Presentations                     |
| [specs/20-search-engine.md](specs/20-search-engine.md)                       | Full-text search                  |
| [specs/21-media-engine.md](specs/21-media-engine.md)                         | Media                             |
| [specs/22-vgr-format.md](specs/22-vgr-format.md)                             | Native project format             |
| [specs/23-scene-composition-engine.md](specs/23-scene-composition-engine.md) | Scene composition                 |
| [specs/24-bible-engine.md](specs/24-bible-engine.md)                         | Bible engine                      |
| [specs/25-song-engine.md](specs/25-song-engine.md)                           | Song & lyrics engine              |
| [specs/26-service-flow-automation.md](specs/26-service-flow-automation.md)   | Flow/automation                   |
| [specs/27-production-engine.md](specs/27-production-engine.md)               | Production graph                  |
| [specs/28-recording-engine.md](specs/28-recording-engine.md)                 | Recording/replay/capture          |
| [specs/29-broadcast-ndi-sdi.md](specs/29-broadcast-ndi-sdi.md)               | NDI + SDI broadcast               |

## Architecture notes

- [architecture/RenderingEngine.md](architecture/RenderingEngine.md)
- [architecture/RenderingPipeline.md](architecture/RenderingPipeline.md)
- [architecture/DisplayEngine.md](architecture/DisplayEngine.md)
- [architecture/PresentationEngine.md](architecture/PresentationEngine.md)
- [architecture/ModuleLifecycle.md](architecture/ModuleLifecycle.md)
- [architecture/PluginSystem.md](architecture/PluginSystem.md)
- [architecture/AdaptiveRuntime.md](architecture/AdaptiveRuntime.md)
- [architecture/ProjectSystem.md](architecture/ProjectSystem.md)
- [architecture/PAL.md](architecture/PAL.md) — platform abstraction layer
- [architecture/EventSystem.md](architecture/EventSystem.md)
- [architecture/CAMS.md](architecture/CAMS.md)
- [architecture/ResourceManager.md](architecture/ResourceManager.md)
- [architecture/CoreArchitecture.md](architecture/CoreArchitecture.md)
- [architecture/Notifications.md](architecture/Notifications.md)
- [architecture/Conformance.md](architecture/Conformance.md)

## Quick orientation

- **Headers = the contract.** Everything public lives under `core/include/`,
  `modules/include/`, `interfaces/include/`, and `platform/include/`.
- **The API guide** (`api/README.md`) is the UI-facing map of all of it.
- **`apps/cli/main.cpp`** is a complete reference consumer: it boots the Kernel and
  calls every engine's public API end-to-end.
- **Tests** in `tests/unit/tests_*.cpp` are per-phase behavioral verification.
