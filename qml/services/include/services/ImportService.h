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
class ImportService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Every format, in the engine's order: [{ id, name, description, extensions, kind, available, section, primary, tutorial, icon, filter }]
    // `filter` is the file picker's name filter for it ("ChordPro (*.cho *.crd ...)").
    Q_PROPERTY(QVariantList formats READ formats CONSTANT)

public:
    static ImportService &instance();
    static ImportService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    QVariantList formats() const;

    // Reads `files` (local paths or file:// urls) as format `formatId`. Each imported show is saved into the shows library, each
    // Bible is installed; a file that could not be read is reported. Returns
    //   { ok, shows: n, bibles: n, files: n, warnings: [...], error, firstShow }   (`firstShow` = the first saved show's path)
    // and tells the user with a toast.
    Q_INVOKABLE QVariantMap importFiles(const QString &formatId, const QStringList &files);
    // "Paste from clipboard": the text as one show.
    Q_INVOKABLE QVariantMap importText(const QString &text);
    // The same, reading the text from the clipboard.
    Q_INVOKABLE QVariantMap importClipboard();

signals:
    void imported(int shows, int bibles);

private:
    explicit ImportService(QObject *parent = nullptr);
};
