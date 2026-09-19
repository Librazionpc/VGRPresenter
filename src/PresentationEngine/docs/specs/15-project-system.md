# 15 — Project & Data System

Phase 4. The heart of user data: everything the user creates is stored here.
Projects, workspaces, documents, history, undo/redo, snapshots, recovery,
backups, packages, dependencies, references, recents, favorites, templates,
profiles.

**Status**: Implemented — `modules/project/` (namespace `bps::project`).
Architecture: `docs/architecture/ProjectSystem.md`. Acceptance: Conformance §18.

## Purpose

- Make the engine finally understand **projects** (not just presentations).
- Own the full lifecycle of user work, independent of how it is rendered.
- Integrate with CAMS (assets/dependencies/references/packages), the EventBus,
  the Notification Service, Logger, ConfigurationManager, DatabaseManager, and
  the PAL.

## Manager → Registry → Interface → Provider pattern

Every subsystem follows the engine-wide pattern:

```
ProjectManager → ProjectRegistry → (JSON project files)
DocumentManager → DocumentRegistry → IDocumentHandler → PresentationDocument/…
```

## Components

| Component | Role |
|---|---|
| DataManager | Facade + IService lifecycle (boot step 15) |
| ProjectManager | Create/open/save/save-as/close/rename/duplicate/delete/archive |
| WorkspaceManager | Open documents, selected displays, current theme, zoom — serializable |
| DocumentManager | Open/save/close/lock/dirty/read-only via IDocumentHandler |
| SessionManager | Current user, active project, runtime, crash restore |
| HistoryManager | Edit history, project history, save history, checkpoints |
| UndoRedoManager | Command-based undo/redo, transaction grouping, depth cap |
| SnapshotManager | Manual + auto snapshots, restore any snapshot |
| RecoveryManager | Unsaved work, crashed sessions, recovery suggestions |
| BackupManager | Scheduled/incremental/full backups + retention |
| PackageManager | Portable project packages (.zip + manifest) via CAMS |
| DependencyManager | Project → asset dependency graph, missing/broken detection |
| ReferenceManager | Unused/broken/duplicate/shared asset analysis |
| RecentManager | Recently opened projects/assets/templates/searches |
| FavoritesManager | Favorites by kind |
| TemplateManager | Reusable project templates (create/version/apply/default) |
| ProfileManager | Display/import/notification profiles (reusable presets) |
| ProjectRegistry | Project metadata index |
| WorkspaceRegistry | Workspace state index |

## Public API (DataManager facade)

- `Initialize/Start/Stop/Shutdown/Reload/Reset/GetHealth/MetricsSnapshot`
- Project: `Create/Open/Save/SaveAs/Close/Rename/Duplicate/Delete/Archive/List/Get`
- Workspace: `Load/Save/SetCurrentProject/SetOpenDocuments/…`
- Documents: `Open/Save/Close/Lock/Unlock/Dirty/ReadOnly`
- Undo/Redo: `Execute(command)/Undo()/Redo()/CanUndo()/CanRedo()/Depth()`
- Snapshots: `CreateSnapshot/RestoreSnapshot/ListSnapshots`
- Recovery: `CheckRecovery()/Recover(projectId)`
- Backups: `BackupNow(projectId)/ListBackups/RetentionPolicy`
- Packages: `ExportPackage(projectId, path)/ImportPackage(path)`
- Dependencies: `CheckDependencies(projectId)/DependencyGraph(projectId)`
- References: `AnalyzeReferences(projectId)` → unused/broken/duplicate/shared
- Recents/Favorites/Templates/Profiles: CRUD + queries

## Events

Publishes `project.opened/saved/closed/created/deleted`,
`project.document_opened/saved/closed/dirty`,
`project.undo_performed/redo_performed`, `project.snapshot_created`,
`project.backup_completed`, `project.package_exported`,
`project.recovery_available`, `project.dependency_issue`.

## Integration

Core (Result, health, metrics, lifecycle), PAL (all file I/O), CAMS (packages via
`ZipWriter`, dependency checks via `AssetDatabase`/VFS), EventBus, Notification
Service (project events → user notifications), Logger, ConfigurationManager
(`project.*` keys), DatabaseManager (`projects`/`workspace`/`history` collections).

## Persistence

- Project metadata → DatabaseManager `projects` collection + `.bpsproj` JSON file.
- Workspace state → DatabaseManager `workspace` collection.
- History → DatabaseManager `history` collection.
- Backups/snapshots → host filesystem via PAL.

## Failure Modes

- Corrupt project file → `Project_NotFound`/parse error; RecoveryAvailable event.
- Locked document write → `Project_DocumentLocked`.
- Undo on empty stack → `Project_UndoEmpty` (no-op).
- Missing dependency → `Project_DependencyMissing` + DependencyIssueFound event.
- Package failure → `Project_PackageFailed`.

## Testing Expectations

Open/save cycles, crash recovery, undo/redo correctness, package creation,
dependency validation, backup/snapshot restore (`TestProject*`).
