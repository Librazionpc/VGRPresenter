#pragma once

// The UI's window onto the ENGINE's sermon library (bps::library::TheTableLibrary)
// behind "The Table" tab. The same reference architecture as scripture —
// books (years) -> chapters (sermons) -> verses (paragraphs) — but a SEPARATE
// engine module with its own importer: "New sermon" takes a .txt or .pdf, the
// engine parses it (year from the folder, title from the file name, paragraphs
// as verses) and updates the library. The pane owns no data; this hands over
// the engine's documents and search results.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class TheTableService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // The library document for the browser pane:
    // { name, collections: [ { name, books: [ { title, sections: [ { verses: [...] } ] } ] } ] }
    // (ReferencePane browses years/sermons/paragraphs from this adapter.)
    Q_PROPERTY(QVariantMap document READ document NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    // Total paragraph count (the sidebar's count badge).
    Q_PROPERTY(qlonglong verseCount READ verseCount NOTIFY changed)

public:
    static TheTableService &instance();
    static TheTableService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    QVariantMap document() const;
    bool loading() const;
    qlonglong verseCount() const;

    // One sermon's paragraphs: [{ number, text, heading }]
    Q_INVOKABLE QVariantList chapter(const QString &bookId, int chapter) const;
    // Whole-library search: [{ reference, bookId, chapter, verse, snippet }]
    Q_INVOKABLE QVariantList search(const QString &text, int limit = 60) const;

    // ---- the shared reference-pane adapter (the same calls ScriptureService
    // answers, so ONE pane UI serves both tabs) ----
    // The one collection ("Sermons") + the years as books: [{ id, name }]
    // (the pane's sidebar rows — the library is a single source).
    Q_INVOKABLE QVariantList sources() const;
    // A source's books in order: [{ id, name, chapters: [1, 2, ...] }]
    Q_INVOKABLE QVariantList books(const QString &sourceId) const;
    // A passage written the engine's way: "1953 12" + picked verse numbers ->
    // "1953 12:3-5" ("1953 12" when nothing is picked).
    Q_INVOKABLE QString reference(const QString &book, int chapter, const QVariantList &verses) const;
    // "1953 12:3" typed into search -> { bookId, book, chapter, verseStart, verseEnd }, else {}.
    Q_INVOKABLE QVariantMap resolve(const QString &text) const;
    // The preview slide for the picked paragraphs, in the Edit screen's block
    // shape (DesignPreview draws it): { blocks, background, reference, slideCount }.
    Q_INVOKABLE QVariantMap preview(const QString &bookId, int chapter, const QVariantList &verses) const;
    // The picked paragraphs as show slides ({ title, background, blocks }) —
    // the chosen template split by the tab's own "table." options.
    Q_INVOKABLE QVariantList slides(const QString &bookId, int chapter, const QVariantList &verses) const;

    // The Table's template chrome (the shared pane's template card + picker).
    // The SAME template library Scripture uses; only "table"-type layouts are
    // listed and the choice is stored under its own settings key.
    Q_INVOKABLE QString templateId() const;
    Q_INVOKABLE QString defaultTemplateId() const;
    Q_INVOKABLE QString templateName(const QString &id) const;
    Q_INVOKABLE QVariantList templates() const;
    Q_INVOKABLE bool templateHasValues(const QString &id) const;
    Q_INVOKABLE void setTemplate(const QString &id) const;

    // "New sermon": asks for a .txt/.pdf and imports it through the engine.
    // Returns true when a sermon landed in the library.
    Q_INVOKABLE bool newSermon();

    // "Add sermons folder": picks a folder and imports every .pdf/.txt under
    // it (recursively) on a WORKER THREAD — the UI stays live; `progress`
    // carries { done, total, current } per file and one final `imported`
    // ({ imported, skipped, failed }) toast lands when it finishes.
    Q_INVOKABLE bool newSermonFolder();
    Q_PROPERTY(bool importing READ importing NOTIFY importingChanged)
    bool importing() const { return importing_; }
    Q_PROPERTY(QVariantMap progress READ progress NOTIFY progressChanged)
    QVariantMap progress() const { return progress_; }

signals:
    void importingChanged();
    void progressChanged();
    void changed();

private:
    explicit TheTableService(QObject *parent = nullptr);

    void loadLibrary();
    QVariantMap documentFromEngine() const;
    void reindex();   // re-upserts the sermons into the platform Search Engine (after an import)

    void *library_ = nullptr;   // bps::library::TheTableLibrary* (void* keeps the engine header out of QML builds)
    bool loading_ = false;
    bool importing_ = false;
    QVariantMap progress_;
};
