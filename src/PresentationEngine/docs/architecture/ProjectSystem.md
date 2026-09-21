# Project & Data System — Architecture

Phase 4. Everything the user creates lives here, independent of rendering.

```
              DataManager (facade, IService)
   ┌──────────┬──────────┬──────────┬──────────┬───────────┐
   ▼          ▼          ▼          ▼          ▼           ▼
Project    Workspace  Document   Session    History   UndoRedo
Manager    Manager    Manager    Manager    Manager   Manager
   │          │          │          │          │           │
   ▼          ▼          ▼          ▼          ▼           ▼
Snapshot   Recovery   Backup     Package    Dependency  Reference
Manager    Manager    Manager    Manager    Manager     Manager
   │          │          │          │          │           │
   ▼          ▼          ▼          ▼          ▼           ▼
Recent     Favorites  Template   Profile    ProjectRegistry
Manager    Manager    Manager    Manager
```

## Layering

- **Facade**: `DataManager` — IService lifecycle, EventBus wiring, notification
  integration, boot step 15 in the Kernel.
- **Project core**: `ProjectManager` (registry of `Project` records) +
  `WorkspaceManager` (serializable UI state) + `SessionManager` (runtime).
- **Editing**: `DocumentManager` (IDocumentHandler registry — the provider
  pattern: `DocumentManager → DocumentRegistry → IDocumentHandler →
  PresentationDocument/…`), `UndoRedoManager` (ICommand), `HistoryManager`.
- **Safety**: `SnapshotManager`, `RecoveryManager`, `BackupManager`.
- **Analysis**: `DependencyManager` + `ReferenceManager` (built on CAMS
  `AssetDatabase` + VFS existence checks).
- **UX state**: `RecentManager`, `FavoritesManager`, `TemplateManager`,
  `ProfileManager`.

## Project file format

`.bpsproj` — a JSON document written through the PAL filesystem:

```json
{
  "schemaVersion": 1,
  "id": "…", "name": "Sunday Service", "version": 3,
  "createdAtMs": …, "modifiedAtMs": …,
  "settings": { … },
  "documents": [{"id":"…","type":"presentation"}],
  "dependencies": [{"assetId":"…","kind":"image"}]
}
```

Project metadata is also indexed in the DatabaseManager `projects` collection.

## Document lifecycle

`Open → (Dirty ⇄ Saved) → Close`, with `Lock`, `ReadOnly`, dirty tracking, and
events (`project.document_*`). Handlers implement `IDocumentHandler`; adding a
document type = register a handler, no engine change.

### Show library

`ShowLibrary` (modules/presentation) is a folder of `.vgr` shows: library categories are
sub-folders, shows are files. It lists/searches shows from their headers, creates and
renames categories, and does show CRUD in the engine: `RenameShow` (stored name + file
name), `DuplicateShow` (fresh identity), `DeleteShow` (recoverable — moved to
`<root>/.deleted/`), `MoveShow`. Unreadable files are reported via `Problems()`, never
silently dropped.

### Registered document types

| Type | Handler | File |
|---|---|---|
| `presentation` | `PresentationDocument` (modules/presentation), registered by `PresentationEngine::Initialize` | `.vgr` (`Show`) |

## Undo/Redo

Every editing operation is an `ICommand { Name(), Execute(), Undo() }`.
`UndoRedoManager::Execute` pushes onto the undo stack, clears redo, and
optionally groups commands into transactions. Depth is configurable.

## Recovery

- Auto-save writes periodic snapshots (DocumentManager auto-save).
- On startup `CheckRecovery()` compares auto-save timestamps vs last clean save.
- `Recover(projectId)` restores the newest auto-save and publishes
  `project.recovery_available`.

## Packages

`PackageManager::ExportPackage` produces a portable `.zip`:
`manifest.json` (metadata + dependency manifest) + all referenced asset bytes,
built with the CAMS `ZipWriter`. `ImportPackage` reverses it.

## Integration

- **CAMS**: package bytes, dependency/reference checks via `AssetDatabase`.
- **Notification Service**: project events → "Project saved" etc.
- **PAL**: all file I/O. **EventBus**: `project.*` events.
- **ConfigurationManager**: `project.autoSaveMs`, `project.backup.*`,
  `project.history.maxDepth`, `project.recovery.enabled`.
