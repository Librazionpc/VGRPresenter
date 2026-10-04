#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <atomic>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;
class QThread;

// The app's one search: the header's Search button, Ctrl+K, the Projects panel's
// "Quick search" and the menu item all ask this service, and it asks the engine.
//
//   shows ........ the engine's ShowLibrary (name / category)
//   slides ....... the working show's slides (title, lines, reference)
//   templates,
//   overlays,
//   categories ... the working show's own templates / overlays / categories
//   songs ........ the engine's SongEngine (title, authors, lyrics)
//   Bible ........ the engine's BibleEngine: a typed reference ("John 3:16",
//                  "Ps 23") is resolved to its verses, and any other words are
//                  searched in the verse text of every installed Bible
//   settings ..... the Settings entries (the service owns that list; the Settings
//                  popup's own search box asks searchSettings())
//
// Every result is { kind, title, subtitle, text, id, path, section, key } — what to
// do with it (open the show, go to the slide, open the settings section...) is the
// UI's call, the service only finds things.
class SearchService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // True while the Bible files are being read into the engine (a few seconds,
    // on a background thread).
    Q_PROPERTY(bool bibleLoading READ bibleLoading NOTIFY bibleChanged)
    // Ids of the Bibles the engine has installed ("kjv", ...).
    Q_PROPERTY(QStringList bibles READ bibles NOTIFY bibleChanged)
    // True while an import (startup load or a "New scripture" install) runs on
    // the worker thread; `bibleProgress` is { done, total, current } — verses
    // indexed so far, verses in the file, and the book being indexed (or the
    // file name between files).
    Q_PROPERTY(bool bibleImporting READ bibleImporting NOTIFY bibleImportingChanged)
    Q_PROPERTY(QVariantMap bibleProgress READ bibleProgress NOTIFY bibleProgressChanged)

public:
    static SearchService &instance();
    static SearchService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    ~SearchService() override;

    bool bibleLoading() const { return loading_; }
    bool bibleImporting() const { return importing_; }
    QVariantMap bibleProgress() const { return progress_; }
    QStringList bibles() const;

    // Searches everything. At most `perKind` results per kind, best first; results
    // are grouped by kind in a fixed order. Empty/blank text -> {}.
    Q_INVOKABLE QVariantList search(const QString &text, int perKind = 5) const;
    // ASYNC search: the same aggregation on a worker thread (the GUI never
    // blocks on a big query — "the" has ~1,200 candidates and a synchronous
    // call froze typing for hundreds of ms). `token` is the caller's identity:
    // only the LATEST request's results are emitted (a fast typist's older
    // keystrokes answer into the void), delivered as resultsReady(token, rows).
    Q_INVOKABLE void searchAsync(const QString &text, int perKind, int token);

    // What the engine's word resolution would change about `text` — the same
    // resolution every search applies internally: incomplete words completed
    // ("friend" -> "friends"), typos corrected ("thn" -> "then"), typed-together
    // words split ("holyspirit" -> "holy spirit"). One { typed, resolved, weight }
    // entry per word that DIFFERS from its resolution, in order; empty when the
    // query is already well-spelled (the common case: zero UI, zero cost).
    // SYNCHRONOUS (kept for callers that already sit on a worker); the GUI-facing
    // path is resolveWordsAsync below — each misspelled token scans the whole
    // ~91k-word vocabulary, a multi-token garble on the GUI thread was the
    // search-freeze.
    Q_INVOKABLE QVariantList resolveWords(const QString &text) const;
    // ASYNC resolveWords: answers as wordsResolved(token, rows); only the
    // LATEST request's token is answered (same contract as searchAsync).
    Q_INVOKABLE void resolveWordsAsync(const QString &text, int token);

    // One verse's FULL text (typesetting marks cleaned) — the Quick search dialog's
    // hover preview: a result row only carries a capped snippet, hovering fetches
    // the whole verse with this. "" when the bible/book/chapter/verse is unknown.
    Q_INVOKABLE QString fullVerse(const QString &bibleId, const QString &bookId,
                                  int chapter, int verse) const;

    // Only the Settings entries (the Settings popup's search box): best first, at most
    // `limit`. Each is { kind: "setting", title, section, subtitle, key } - `key` is the
    // settings section to open. Matches the entry's name or its section's.
    Q_INVOKABLE QVariantList searchSettings(const QString &text, int limit = 50) const;

    // Reads the Bible files into the engine on a background thread, once: the file
    // named by VGR_BIBLE_FILE, every Bible in <Documents>/VGR Presenter/Bibles, and
    // (development builds) the KJV shipped in the source tree. Needs the engine booted.
    Q_INVOKABLE void loadBibles();

    // Installs one Bible file now (json / xml / osis / usfm / txt). Returns false
    // (and toasts the error) when the import fails or another one is already
    // running; the work happens on a worker thread — bibleImporting/
    // bibleProgress follow it, and bibleChanged fires when it lands.
    Q_INVOKABLE bool importBibleFile(const QString &path);

    // Waits for a running Bible import to finish. Call before the engine shuts down.
    void shutdown();

signals:
    void bibleChanged();
    void bibleImportingChanged();
    void bibleProgressChanged();
    // An async search (searchAsync) finished: the token matches the request.
    void resultsReady(int token, const QVariantList &rows);
    // An async word resolution (resolveWordsAsync) finished: the token matches.
    void wordsResolved(int token, const QVariantList &rows);

private:
    explicit SearchService(QObject *parent = nullptr);
    static QStringList candidateBibleFiles();
    std::atomic<int> latestResolveToken_{0};   // resolveWordsAsync: newest request wins

    QPointer<QThread> loader_;
    std::atomic<int> latestToken_{0};   // searchAsync: only the newest request emits
    bool loading_ = false;
    bool loadStarted_ = false;
    bool importing_ = false;
    QVariantMap progress_;
};
