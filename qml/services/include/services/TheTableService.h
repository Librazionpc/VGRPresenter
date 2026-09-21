#pragma once

// The UI's window onto the ENGINE's sermon library (bps::library::TableLibrary)
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
    // (LibraryBrowserPane's Table shape; empty collections until the engine boots.)
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

    // "New sermon": asks for a .txt/.pdf and imports it through the engine.
    // Returns true when a sermon landed in the library.
    Q_INVOKABLE bool newSermon();

signals:
    void changed();

private:
    explicit TheTableService(QObject *parent = nullptr);

    void loadLibrary();
    QVariantMap documentFromEngine() const;

    void *library_ = nullptr;   // bps::library::TheTableLibrary* (void* keeps the engine header out of QML builds)
    bool loading_ = false;
};
