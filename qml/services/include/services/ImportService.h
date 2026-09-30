#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

// The UI's window onto the ENGINE's importing (bps::import - FreeShow's import formats): the format list the Import dialog is
// drawn from, and "read these files as format X". The engine converts the files into shows (or installs Bibles); this hands it the
// file bytes, saves each imported show as a .vgr in the shows library, and reports what came of it.
//
// importFiles runs ASYNCHRONOUSLY on a worker thread (a 700-file import takes
// a minute or more — on the GUI thread it froze the whole window, AppHang):
// the dialog shows a progress overlay bound to `busy`/`progress`, and the
// `imported` signal delivers the outcome when the sweep completes.
class ImportService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Every format, in the engine's order: [{ id, name, description, extensions, kind, available, section, primary, tutorial, icon, filter }]
    // `filter` is the file picker's name filter for it ("ChordPro (*.cho *.crd ...)").
    Q_PROPERTY(QVariantList formats READ formats CONSTANT)
    // True while a file import is running on the worker thread (drives the
    // Import dialog's progress overlay; also guards against double-starts).
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // 0..1 progress of the running import (the saved fraction of the batch).
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    // Human status line for the overlay ("Importing 214 of 741 — song name").
    Q_PROPERTY(QString status READ status NOTIFY progressChanged)

public:
    static ImportService &instance();
    static ImportService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    QVariantList formats() const;

    bool busy() const { return m_busy; }
    qreal progress() const { return m_progress; }
    QString status() const { return m_status; }

    // Reads `files` (local paths or file:// urls) as format `formatId`. Each imported show is saved into the shows library, each
    // Bible is installed; a file that could not be read is reported. Returns
    //   { ok: true, started: true } immediately ("" result = nothing started);
    // the real outcome { ok, shows, bibles, files, warnings, error, firstShow }
    // arrives later via the `finished(answer)` signal (and `imported(shows, bibles)`).
    // A FreeShow library import saves into a CATEGORY NAMED AFTER THE SOURCE
    // FOLDER (the imported .show files' parent directory, "FreeShow" when it
    // cannot be told), replacing the previous batch in that category first.
    Q_INVOKABLE QVariantMap importFiles(const QString &formatId, const QStringList &files);
    // "Paste from clipboard": the text as one show (FreeShow's "Quick lyrics").
    // `name`/`category` override what the text itself implies ("" = keep the
    // parser's own guess — see SongTextOptions: category defaults to "song").
    Q_INVOKABLE QVariantMap importText(const QString &text, const QString &name = QString(),
                                       const QString &category = QString());
    // The same, reading the text from the clipboard.
    Q_INVOKABLE QVariantMap importClipboard();

signals:
    void imported(int shows, int bibles);
    // The full answer of a finished importFiles run (same map finish() builds).
    void finished(const QVariantMap &answer);
    // One per show saved during the sweep (drives the progress bar).
    void showSaved(int savedSoFar);
    void busyChanged();
    void progressChanged();

private:
    explicit ImportService(QObject *parent = nullptr);
    void setProgress(qreal fraction, const QString &status);

    bool m_busy = false;
    qreal m_progress = 0.0;
    QString m_status;
};
