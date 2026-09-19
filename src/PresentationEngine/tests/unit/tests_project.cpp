// Unit tests: Project & Data system (docs/specs/15).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests project
#include "TestHarness.hpp"

class TestDocumentHandler final : public project::IDocumentHandler {
public:
    const char* DocumentType() const noexcept override { return "test-doc"; }
    Result<void> Open(const std::string&) override {
        dirty_ = true;
        return Ok();
    }
    Result<void> Save(const std::string&) override {
        dirty_ = false;
        return Ok();
    }
    Result<void> Close() override { return Ok(); }
    bool IsDirty() const override { return dirty_; }

private:
    bool dirty_ = false;
};
void TestProjectManager() {
    namespace fs = std::filesystem;
    const std::string dir = "/tmp/bps_proj_data";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    CHECK(DatabaseManager::Instance().Open("").ok());   // in-memory metadata store

    auto& pm = project::ProjectManager::Instance();
    pm.SetDataDir(dir);

    // Create + save (writes the .bpsproj file through the PAL).
    auto created = pm.Create("Sunday Service");
    CHECK(created.ok());
    const std::string id = created.value().id;
    CHECK(!id.empty());
    CHECK(pm.Save(id, false).ok());
    std::string path = pm.Get(id).value().path;
    CHECK(!path.empty());
    CHECK(fs::exists(path));

    // Rename + duplicate.
    CHECK(pm.Rename(id, "Sunday Service 2026").ok());
    CHECK(pm.Get(id).value().name == "Sunday Service 2026");
    auto dup = pm.Duplicate(id);
    CHECK(dup.ok());
    CHECK(pm.OpenProjects().size() >= 2);

    // Active project.
    CHECK(pm.SetActive(id).ok());
    CHECK(pm.Active().ok());

    // Close + reopen from the on-disk file.
    CHECK(pm.Close(id).ok());
    auto reopened = pm.Open(path);
    CHECK(reopened.ok());
    CHECK(reopened.value().name == "Sunday Service 2026");
    CHECK(reopened.value().id == id);

    // Archive flag.
    CHECK(pm.Archive(id, true).ok());
    CHECK(pm.Get(id).value().archived);

    // Delete all.
    CHECK(pm.Delete(reopened.value().id).ok());
    CHECK(pm.Delete(dup.value().id).ok());
    CHECK(pm.OpenProjects().empty());
}
void TestProjectUndoRedo() {
    auto& ur = project::UndoRedoManager::Instance();
    int value = 0;

    auto inc = std::make_shared<project::LambdaCommand>(
        "inc", [&]() { ++value; }, [&]() { --value; });
    CHECK(ur.ExecuteCommand(inc).ok());
    CHECK(value == 1);
    CHECK(ur.CanUndo());
    CHECK(ur.Undo().ok());
    CHECK(value == 0);
    CHECK(ur.CanRedo());
    CHECK(ur.Redo().ok());
    CHECK(value == 1);

    // Transaction grouping: two commands undo/redo as one unit.
    CHECK(ur.BeginGroup("multi").ok());
    auto a = std::make_shared<project::LambdaCommand>(
        "add2", [&]() { value += 2; }, [&]() { value -= 2; });
    auto b = std::make_shared<project::LambdaCommand>(
        "mul3", [&]() { value *= 3; }, [&]() { value /= 3; });
    CHECK(ur.ExecuteCommand(a).ok());
    CHECK(ur.ExecuteCommand(b).ok());
    CHECK(ur.EndGroup().ok());
    CHECK(value == 9);
    CHECK(ur.Undo().ok());   // reverses b then a
    CHECK(value == 1);
    CHECK(ur.Redo().ok());
    CHECK(value == 9);

    // Undo-empty failure path.
    ur.Clear();
    CHECK(!ur.Undo().ok());
    CHECK(!ur.Redo().ok());
}
void TestProjectWorkspace() {
    auto& ws = project::WorkspaceManager::Instance();
    ws.SetOpenDocuments({"doc-1", "doc-2"});
    ws.SetSelectedDisplays({"mon-1"});
    ws.SetCurrentPresentation("pres-uuid");
    ws.SetCurrentTheme("dark");
    ws.SetZoom(1.25);
    ws.AddRecentlyUsed("project", "p1", "Sunday");
    CHECK(ws.OpenDocuments().size() == 2);
    CHECK(ws.SelectedDisplays().size() == 1);
    CHECK(ws.CurrentPresentation() == "pres-uuid");
    CHECK(ws.CurrentTheme() == "dark");
    CHECK(ws.Zoom() == 1.25);
    CHECK(ws.RecentlyUsed("project").size() == 1);

    // Serializable round-trip through the DatabaseManager.
    CHECK(ws.Save().ok());
    ws.SetOpenDocuments({});
    ws.SetZoom(0.5);
    CHECK(ws.Load().ok());
    CHECK(ws.OpenDocuments().size() == 2);
    CHECK(ws.Zoom() == 1.25);

    // Document lifecycle: handler registry → open/save/close/lock/read-only.
    auto& dm = project::DocumentManager::Instance();
    auto handler = std::make_shared<TestDocumentHandler>();
    CHECK(dm.RegisterHandler(handler).ok());
    CHECK(dm.RegisterHandler(handler).error().code == Err::AlreadyExists);
    auto docId = dm.Open("test-doc", "/tmp/bps_doc.txt");
    CHECK(docId.ok());
    CHECK(!dm.IsDirty(docId.value()));            // clean until an edit is recorded
    CHECK(dm.SetDirty(docId.value(), true).ok()); // an edit marks it dirty
    CHECK(dm.IsDirty(docId.value()));
    CHECK(dm.Save(docId.value()).ok());
    CHECK(!dm.IsDirty(docId.value()));
    CHECK(dm.Lock(docId.value(), true).ok());
    CHECK(dm.IsLocked(docId.value()));
    CHECK(dm.SetReadOnly(docId.value(), true).ok());
    CHECK(dm.IsReadOnly(docId.value()));
    CHECK(!dm.Save(docId.value()).ok());   // read-only blocks save
    CHECK(dm.Close(docId.value()).ok());
    CHECK(dm.OpenCount() == 0);
    CHECK(dm.UnregisterHandler("test-doc").ok());

    // Unsupported document type.
    auto bad = dm.Open("nope", "/tmp/x.txt");
    CHECK(!bad.ok());
    CHECK(bad.error().code == Err::Unsupported);
}
void TestProjectSessionSnapshot() {
    auto& sm = project::SessionManager::Instance();
    CHECK(sm.Begin("tester").ok());
    CHECK(sm.User() == "tester");
    sm.SetActiveProject("p-snap");
    sm.NotePresentationOpened();
    CHECK(sm.Save().ok());

    // Crash detection: the saved session is unclean until End().
    auto& rec = project::RecoveryManager::Instance();
    CHECK(rec.DetectCrashedSession());
    CHECK(rec.Suggestions().size() >= 1);
    CHECK(rec.Count() >= 1);

    // Unsaved-work recovery items.
    CHECK(rec.RecordUnsavedWork("p-snap", "doc-1", "slide edits").ok());
    CHECK(rec.Suggest("p-snap").ok());
    CHECK(rec.Resolve("unsaved-work:p-snap:doc-1").ok());
    CHECK(rec.ClearProject("p-snap").ok());   // also clears the crashed-session item
    CHECK(rec.Count() == 0);

    // Snapshots: create → list → restore → remove.
    auto& snap = project::SnapshotManager::Instance();
    json::Value::Object projDoc;
    projDoc["name"] = json::Value::String("snapped");
    auto snapId = snap.Create("p-snap", "manual", json::Value(std::move(projDoc)), false);
    CHECK(snapId.ok());
    CHECK(snap.List("p-snap").size() == 1);
    CHECK(snap.Restore(snapId.value()).ok());
    CHECK(snap.Remove(snapId.value()).ok());
    CHECK(snap.Count() == 0);

    // Clean end marks the session shutdown as clean.
    CHECK(sm.End().ok());
    CHECK(sm.WasCleanShutdown());
    CHECK(!rec.DetectCrashedSession());
}
void TestProjectPackage() {
    auto& cm = content::ContentManager::Instance();
    // The CAMS tests Reset() the library; bring up a fresh in-memory library.
    if (cm.AssetCount() == 0) {
        CHECK(cm.Initialize().ok());
        CHECK(cm.MountMemory("lib").ok());
        CHECK(cm.Start().ok());
    }
    auto asset = cm.CreateText("pkg-script.txt", content::AssetType::Text,
                               "stage notes", {});
    CHECK(asset.ok());

    auto& pm = project::ProjectManager::Instance();
    auto proj = pm.Create("Packaged Project");
    CHECK(proj.ok());
    CHECK(pm.SetAssetReferences(proj.value().id, {asset.value().ToString()}).ok());
    CHECK(pm.Save(proj.value().id, false).ok());

    auto& pkg = project::PackageManager::Instance();
    const std::string outPath = "/tmp/bps_test.bpspkg";
    CHECK(pkg.Export(proj.value().id, outPath).ok());

    // Inspect the package manifest without importing.
    auto manifest = pkg.Inspect(outPath);
    CHECK(manifest.ok());
    CHECK(manifest.value().projectName == "Packaged Project");
    CHECK(manifest.value().assetUuids.size() == 1);

    // Import creates a fresh project with rehydrated assets.
    auto importedId = pkg.Import(outPath);
    CHECK(importedId.ok());
    CHECK(pkg.PackagesImported() >= 1);
    auto imported = pm.Get(importedId.value());
    CHECK(imported.ok());
    CHECK(imported.value().assetUuids.size() == 1);

    // Backup + retention.
    auto& bk = project::BackupManager::Instance();
    auto bakId = bk.Backup(importedId.value(), "db", true);
    CHECK(bakId.ok());
    CHECK(bk.List(importedId.value()).size() == 1);
    CHECK(bk.ApplyRetention(importedId.value(), 1, 5).ok());
    CHECK(bk.List(importedId.value()).size() == 1);
    CHECK(bk.Restore(bakId.value(), "/tmp/bps_backup_restore.json").ok());
    CHECK(bk.ClearProject(importedId.value()).ok());

    // Cleanup.
    CHECK(pm.Delete(proj.value().id).ok());
    CHECK(pm.Delete(importedId.value()).ok());
    CHECK(cm.Delete(asset.value()).ok());
}
void TestDataManager() {
    auto& data = project::DataManager::Instance();
    CHECK(data.Initialize().ok());
    CHECK(data.Start().ok());
    CHECK(data.GetHealth().IsHealthy());
    CHECK(data.OpenProjectCount() == 0);

    // Recents.
    auto& recents = data.Recents();
    CHECK(recents.Record("project", "p-recent", "Recent Project").ok());
    CHECK(recents.List("project").size() == 1);
    CHECK(recents.Save().ok());

    // Favorites.
    auto& favs = data.Favorites();
    CHECK(favs.Add("song", "s1", "Amazing Grace").ok());
    CHECK(favs.Add("song", "s1", "Amazing Grace").ok());   // idempotent
    CHECK(favs.IsFavorite("song", "s1"));
    CHECK(favs.List("song").size() == 1);
    CHECK(favs.Save().ok());
    CHECK(favs.Remove("song", "s1").ok());

    // Templates: create from a project, apply to seed a new project.
    auto src = data.Projects().Create("Template Source");
    CHECK(src.ok());
    auto& tpls = data.Templates();
    auto tplId = tpls.Create("Sunday Template", src.value().id, "seed");
    CHECK(tplId.ok());
    CHECK(tpls.List().size() == 1);
    auto applied = tpls.Apply(tplId.value(), "New From Template");
    CHECK(applied.ok());
    CHECK(data.Projects().Delete(applied.value().id).ok());
    CHECK(data.Projects().Delete(src.value().id).ok());
    CHECK(tpls.Remove(tplId.value()).ok());
    CHECK(tpls.List().empty());

    // Profiles: create → apply (writes ConfigurationManager keys) → remove.
    auto& profs = data.Profiles();
    json::Value::Object settings;
    settings["display.mode"] = json::Value::String("extended");
    auto profId = profs.Create("display", "Worship Display",
                               json::Value(std::move(settings)), "stage");
    CHECK(profId.ok());
    CHECK(profs.List("display").size() == 1);
    CHECK(profs.Apply(profId.value()).ok());
    CHECK(profs.Remove(profId.value()).ok());
    CHECK(profs.List().empty());

    // Dependencies: dedup + CAMS validation.
    auto& deps = data.Dependencies();
    CHECK(deps.Add("p-dep", "pres-1", "img-1", "image").ok());
    CHECK(deps.Add("p-dep", "pres-1", "img-1", "image").ok());   // dedup
    CHECK(deps.EdgeCount() == 1);
    auto issues = deps.Validate("p-dep");
    CHECK(issues.size() == 1);   // img-1 is not a library asset
    CHECK(issues[0].reason == "missing");
    CHECK(deps.ClearProject("p-dep").ok());
    CHECK(deps.EdgeCount() == 0);

    // References: analyze against the CAMS library (no broken refs for open
    // projects — none reference assets).
    auto& refs = data.References();
    auto report = refs.AnalyzeAll();
    CHECK(report.broken.empty());
    CHECK(refs.AnalyzeCount() >= 1);

    // History manager records.
    auto& hist = data.History();
    CHECK(hist.Record("p-hist", "text.edit", "changed slide").ok());
    CHECK(hist.EditHistory("p-hist").size() == 1);
    hist.RecordSave("p-hist", "/tmp/p.bpsproj", false);
    CHECK(hist.SaveHistory("p-hist").size() == 1);
    CHECK(hist.Checkpoint("p-hist", "before service").ok());
    CHECK(hist.Checkpoints("p-hist").size() == 1);

    // Shutdown persists everything + ends the session cleanly.
    CHECK(data.Stop().ok());
    CHECK(data.Shutdown().ok());
}

// ===========================================================================
// Phase 5 — Adaptive Runtime System (docs/specs/16)
// ===========================================================================
