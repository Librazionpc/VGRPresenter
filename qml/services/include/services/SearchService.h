#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <atomic>
#include <functional>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;
class QThread;

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

    Q_INVOKABLE QVariantList search(const QString &text, int perKind = 5,
                                    bool exactWords = false) const;
    Q_INVOKABLE void searchAsync(const QString &text, int perKind, int token,
                                 bool exactWords = false);

    Q_INVOKABLE QVariantList resolveWords(const QString &text) const;
    // ASYNC resolveWords: answers as wordsResolved(token, rows); only the
    // LATEST request's token is answered (same contract as searchAsync).
    Q_INVOKABLE void resolveWordsAsync(const QString &text, int token);

    Q_INVOKABLE QString fullVerse(const QString &bibleId, const QString &bookId,
                                  int chapter, int verse) const;

    Q_INVOKABLE QVariantList searchSettings(const QString &text, int limit = 50) const;

    Q_INVOKABLE void loadBibles();

    Q_INVOKABLE void refreshBibles();

    Q_INVOKABLE bool importBibleFile(const QString &path);

    // Waits for a running Bible import to finish. Call before the engine shuts down.
    void shutdown();

signals:
    void bibleChanged();
    void bibleImportingChanged();
    void bibleProgressChanged();
    // An async search (searchAsync) finished: the token matches the request.
    void resultsReady(int token, const QVariantList &rows);
    // Categories that finished before the full-library Table scan completes.
    void resultsProgress(int token, const QVariantList &rows);
    // An async word resolution (resolveWordsAsync) finished: the token matches.
    void wordsResolved(int token, const QVariantList &rows);
    // A hover preview's verse text landed (see fullVerse): the dialog re-asks for
    // the row it is still showing, and now reads it from the published cache.
    void verseCached(const QString &bibleId, const QString &bookId, int chapter, int verse);
    void searchCached(const QString &text);
    void wordsCached(const QString &text);

private:
    explicit SearchService(QObject *parent = nullptr);
    static QStringList candidateBibleFiles();

    // ---- THE PUBLISHED BIBLE READS -----------------------------------------
    QStringList readBibles() const;
    QVariantList readResolveWords(const QString &text) const;
    QString readFullVerse(const QString &bibleId, const QString &bookId, int chapter, int verse) const;
    QVariantList readSearch(const QString &rawText, int perKind, bool exactWords,
                            const std::function<bool()>& cancelled = {},
                            const std::function<void(const QVariantList&)>& progress = {}) const;

    static QString verseCacheKey(const QString &bibleId, const QString &bookId, int chapter, int verse);
    void refreshBiblesOffThread() const;
    void refreshResolveWordsOffThread(const QString &text) const;
    void refreshFullVerseOffThread(const QString &bibleId, const QString &bookId, int chapter, int verse) const;
    void refreshAggregateOffThread(const QString &text, int perKind, bool exactWords) const;

    mutable QStringList biblesCache_;
    mutable bool biblesInFlight_ = false;
    mutable QHash<QString, QVariantList> wordsCache_;      // typed text -> resolved words
    mutable QSet<QString> wordsInFlight_;
    mutable QHash<QString, QString> verseCache_;           // bible/book/chapter:verse -> full text
    mutable QSet<QString> verseInFlight_;
    mutable QHash<QString, QVariantList> aggregateCache_;  // the whole query, as its key
    mutable QSet<QString> aggregateInFlight_;
    std::atomic<int> latestResolveToken_{0};   // resolveWordsAsync: newest request wins

    QPointer<QThread> loader_;
    std::atomic<int> latestToken_{0};   // searchAsync: only the newest request emits
    bool loading_ = false;
    bool loadStarted_ = false;
    bool importing_ = false;
    QVariantMap progress_;
};