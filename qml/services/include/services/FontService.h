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
// what an output draws — so the preview and the frame cannot disagree. An
// UNAVAILABLE family (named, but not installed) falls back to the HOST's own
// UI face (FontManager::SystemUiFamily — Qt will draw SOMETHING). An EMPTY
// family — "" a block that names none at all — is a different question: that
// resolves to defaultContentFamily (Cambria), exactly like SceneBuilder's own
// literal, NOT the UI face Theme.fontFamily binds to — content and chrome
// stay two different answers even though both start from "nothing was asked
// for".
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
    // The face a NEW show/template text item starts with — Cambria, always,
    // deliberately NOT defaultFamily (user call: "make the app[']s shows or
    // default template['s font] be Cambria, Segoe UI is for the app['s] UI").
    // Before this, a fresh text block's font fell back to defaultFamily too —
    // the exact same value Theme.fontFamily binds UI chrome to — so content
    // silently matched whatever the HOST's UI font happened to be rather than
    // a chosen, consistent default. Content and chrome are two different
    // questions; this answers the content one. CONSTANT (not engine-queried,
    // not memoised): a fixed name picked for shows, unlike defaultFamily's
    // platform-dependent answer.
    Q_PROPERTY(QString defaultContentFamily READ defaultContentFamily CONSTANT)

public:
    static FontService &instance();
    static FontService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    // The family `requested` must be DRAWN with, as the renderer resolves it:
    // the requested family when the host has it, the host's UI face when it does
    // not, and `defaultContentFamily` (Cambria, not the UI face) when nothing
    // was requested — must match SceneBuilder.cpp's own literal, the real
    // render path this one has to agree with.
    Q_INVOKABLE QString resolve(const QString &requested) const;

    QString defaultFamily() const;
    QString monoFamily() const;
    static QString defaultContentFamily();

signals:
    void changed();

private:
    explicit FontService(QObject *parent = nullptr);

    mutable QHash<QString, QString> cache_;
};
