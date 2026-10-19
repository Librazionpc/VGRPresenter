#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

// The UI's window onto the TEXT RENDERER's font resolution.
//
// Every QML surface that draws a design's text — the Edit canvas, the library
// cards, the monitor tiles, the Text panel's sample — has to name the SAME
// family the engine will paint with. Qt substitutes a missing family on its own
// and picks whatever it likes, so before this service a template naming a font
// the machine did not have showed one face in the canvas and another (or, before
// the engine's own fallback, pixel blocks) on air.
//
// resolve() answers from the engine's FontManager — the one place that decides
// what an output draws — so the preview and the frame cannot disagree. There is
// no font name in here: the engine's answer for an unavailable family is the
// HOST's own UI face (FontManager::SystemUiFamily), and "" (a block that names
// no family at all) resolves to that same face, exactly like SceneBuilder's
// default.
//
// Answers are memoised: this is called from bindings, and the installed font set
// does not change under a running process. GUI THREAD ONLY (QML bindings).
class FontService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // The face a design gets when it names none — the host's UI font. UI chrome
    // (Theme.fontFamily) binds to this too, so the app's own type follows the
    // platform instead of a name compiled in here.
    Q_PROPERTY(QString defaultFamily READ defaultFamily NOTIFY changed)
    // The host's fixed-width UI face (Qt's FixedFont) — the log and code
    // surfaces read this instead of naming a monospace font of their own.
    Q_PROPERTY(QString monoFamily READ monoFamily NOTIFY changed)

public:
    static FontService &instance();
    static FontService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    // The family `requested` must be DRAWN with, as the renderer resolves it:
    // the requested family when the host has it, the host's UI face when it does
    // not, and `defaultFamily` when nothing was requested.
    Q_INVOKABLE QString resolve(const QString &requested) const;

    QString defaultFamily() const;
    QString monoFamily() const;

signals:
    void changed();

private:
    explicit FontService(QObject *parent = nullptr);

    mutable QHash<QString, QString> cache_;
};
