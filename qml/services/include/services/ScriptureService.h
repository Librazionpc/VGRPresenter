#pragma once

#include <atomic>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

// The UI's window onto the ENGINE's scripture: the installed Bibles (bps::bible::BibleEngine), their books / chapters / verses,
// reference lookup and search, and - the FreeShow way - the slides a chosen passage becomes when a scripture template is
// filled with it (bps::presentation::BuildScriptureSlides). The rules (how a reference is written, how many verses a slide
// takes, the placeholders a template may use, what the options do) are the engine's; this hands the results to QML.
//
// Blocks come out in the Edit screen's block shape, so the Scripture tab's preview is drawn by the same DesignPreview as
// every other slide and design.
class ScriptureService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Installed Bibles: [{ id, name, abbreviation, language, verseCount }] (a Bible still being read is not listed yet).
    Q_PROPERTY(QVariantList bibles READ bibles NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)

public:
    static ScriptureService &instance();
    static ScriptureService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    QVariantList bibles() const;
    bool loading() const;

    // A Bible's books in order: [{ id, name, testament, chapters: [1, 2, ...], verseCounts: [31, 25, ...] }]
    Q_INVOKABLE QVariantList books(const QString &bibleId) const;
    // One chapter: [{ number, text, heading }]
    Q_INVOKABLE QVariantList chapter(const QString &bibleId, const QString &bookId, int chapter) const;
    // "John 3:16", "Ps 23" -> { bookId, book, chapter, verseStart, verseEnd }, or {} when it is not a reference.
    Q_INVOKABLE QVariantMap resolve(const QString &text, const QString &bibleId) const;
    // Searches the verse text: [{ reference, bookId, book, chapter, verse, snippet }], best first.
    Q_INVOKABLE QVariantList search(const QString &text, const QString &bibleId, int limit = 60) const;
    // ASYNC search (the Quick-search pattern): the same rows on a worker thread
    // — the pill search pane runs this per keystroke and the GUI never blocks.
    // `token` = newest request wins; answers arrive as searchResultsReady(token, rows).
    Q_INVOKABLE void searchAsync(const QString &text, const QString &bibleId, int limit, int token);
    // "Genesis 1:1-3, 5" (the engine's way of writing it). No verses = the whole chapter.
    Q_INVOKABLE QString reference(const QString &book, int chapter, const QVariantList &verses) const;

    // ---- scripture templates ----
    // The templates made for scripture: [{ id, name, color }] (the engine's own set plus the user's).
    Q_INVOKABLE QVariantList templates() const;
    // The template in use: the one chosen in the tab's options, else the engine's default "Scripture".
    Q_INVOKABLE QString templateId() const;
    Q_INVOKABLE QString templateName(const QString &id) const;
    // Does the template use the scripture placeholders? (No: it is an old-style template, and the tab says so.)
    Q_INVOKABLE bool templateHasValues(const QString &id) const;
    Q_INVOKABLE QString defaultTemplateId() const;

    // ---- a passage on slides ----
    // The preview: the picked verses (of one chapter) on ONE slide of the template in use, plus what the panel shows about it:
    //   { blocks, background, reference, hasValues, slideCount }
    // `verses` = the verse numbers picked. `blocks` is empty when nothing is picked.
    Q_INVOKABLE QVariantMap preview(const QString &bibleId, const QString &bookId, int chapter, const QVariantList &verses) const;
    // Every slide the passage needs (verses shared out by the options): [{ title, blocks }] - what "Convert to show" builds.
    Q_INVOKABLE QVariantList slides(const QString &bibleId, const QString &bookId, int chapter, const QVariantList &verses) const;

    // ---- user data (REAL, engine-stored — docs/specs/24 §User data) -------
    // Notes and highlights live in the ENGINE (AddNote/SetHighlight, stored
    // separately from Scripture text and persisted with it) — never in QML
    // state. A note replaces the reference's previous one ("" text removes).
    // All calls resolve `reference` through the engine ("John 3:16"); a bad
    // reference is a false return / empty list, never a crash.
    Q_INVOKABLE bool setNote(const QString &bibleId, const QString &reference, const QString &text);
    // One reference's note text ("" = none).
    Q_INVOKABLE QString note(const QString &bibleId, const QString &reference) const;
    // Every note: [{ reference, bookId, book, chapter, verseStart, verseEnd, text, modifiedMs }], newest first.
    Q_INVOKABLE QVariantList notes(const QString &bibleId) const;
    // Highlight on/off (a re-toggle removes it).
    Q_INVOKABLE bool setHighlighted(const QString &bibleId, const QString &reference, bool on);
    Q_INVOKABLE bool isHighlighted(const QString &bibleId, const QString &reference) const;
    // Every highlighted reference: ["JHN 3:16", ...] (the engine's canonical form).
    Q_INVOKABLE QVariantList highlights(const QString &bibleId) const;

    // ---- installing ----
    // Asks for a Bible file (json / xml / osis / usfm / txt) and installs it. Returns true when one was installed.
    Q_INVOKABLE bool importBible();

signals:
    void changed();
    // A note or highlight was written — verse rows re-read their marks.
    void userDataChanged();
    // searchAsync's answer: token matches the request, rows are the hits
    // (search()'s shape). Stale tokens never emit.
    void searchResultsReady(int token, const QVariantList &rows);

private:
    std::atomic<int> latestSearchToken_{0};   // searchAsync: only the newest emits
    explicit ScriptureService(QObject *parent = nullptr);
};
