#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

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

    Q_INVOKABLE QVariantMap importFiles(const QString &formatId, const QStringList &files);
    Q_INVOKABLE QVariantMap importText(const QString &text, const QString &name = QString(),
                                       const QString &category = QString());
    Q_INVOKABLE void importClipboard();

signals:
    void imported(int shows, int bibles);
    // The answer to importClipboard: the same map importText/finish build,
    // delivered when the worker's read (and any screenshot save) is done.
    void clipboardImported(const QVariantMap &result);
    // The full answer of a finished importFiles run (same map finish() builds).
    void finished(const QVariantMap &answer);
    // One per show saved during the sweep (drives the progress bar).
    void showSaved(int savedSoFar);
    void busyChanged();
    void progressChanged();

private:
    explicit ImportService(QObject *parent = nullptr);
    // What the clipboard read produced (defined in the .cpp): the worker's
    // whole answer, carried by value to the interface thread.
    struct ClipboardPick;
    void applyClipboardRead(const ClipboardPick &pick);
    void setProgress(qreal fraction, const QString &status);

    bool m_busy = false;
    qreal m_progress = 0.0;
    QString m_status;
};