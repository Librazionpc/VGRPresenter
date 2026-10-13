#pragma once

// TEMPORARY diagnostic machinery (kept, env-gated): the UI self-test driver.
// Lets an env-gated QML scenario drive the REAL app — real OS cursor moves
// (QCursor::setPos, so genuine WM hover/enter/exit events deliver exactly as
// they do for a user's hand) and real synthetic clicks — then grab any QML
// item to a PNG for offline pixel sampling.
//
// Why: rendering-layer bug reports ("shape canvas items paint nothing", "chip
// row highlight flickers") survived every property-level trace with clean
// data; what those traces can never see is PIXELS. This driver produces
// them: drive the flow, grab the item, sample the PNG offline.
//
// Hard-gated behind VGR_SELFTEST=1 — main.cpp only instantiates/exports the
// driver when that env var is set, so a normal launch never sees it.
// Registered into QML as `SelfTest` with invokables:
//   activate()          — bring this window to the foreground (real clicks need it)
//   move(x, y)          — real cursor move to window-local logical (x, y)
//   click(x, y)         — move + left down/up at that point
//   grab(itemName, path)— window.findItem<objectName=itemName> -> PNG
//   quit()              — clean Qt.quit()
// (x, y) are in window coordinates at the app's 1440x900 logical size —
// the same space every fixed layout coordinate in this app is written in.

#include <QObject>
#include <QPointF>
#include <QString>
#include <QVariantList>

class QQuickWindow;

class SelfTestDriver : public QObject
{
    Q_OBJECT
public:
    explicit SelfTestDriver(QObject *parent = nullptr);

    // The window whose local coordinate space move/click/grab speak. Set
    // right after engine load, from main.cpp.
    void setWindow(QQuickWindow *window);

    // Raise this window and make it foreground — what a synthetic click needs
    // to actually be DELIVERED to this window's controls (see the .cpp).
    Q_INVOKABLE void activate();
    Q_INVOKABLE void move(double x, double y);
    Q_INVOKABLE void click(double x, double y);
    // Split press/release (click() does both at once, no move in between —
    // no good for a DRAG). Real synthetic left-button down/up at whatever
    // the cursor's CURRENT position is — pair with move() calls between
    // press() and release() to drag: press(x1,y1); move(x2,y2); release().
    Q_INVOKABLE void press(double x, double y);
    Q_INVOKABLE void release(double x, double y);
    // Real key events (press+release) to the focused object — types into whatever
    // TextInput has focus, exercising the app's own key/text pipeline.
    Q_INVOKABLE void type(const QString &text);
    // Deterministic focus: calls forceActiveFocus() on the named item (the synthetic
    // OS click needs window-foreground luck; this does not).
    Q_INVOKABLE bool focusItem(const QString &objectName);
    // Self-locating variants: resolve an item's center by objectName in
    // window coordinates, so the scenario never depends on hand-typed
    // pixel positions that drift when the UI layout changes.
    Q_INVOKABLE QPointF itemCenter(const QString &objectName);
    // Park the REAL cursor at window-local (x, y), let the app consume the
    // hover, and report the shape the OS is then showing — "ARROW", "HAND",
    // "SIZEVER", ... (see the .cpp for the name table). This is the only
    // measurement of the app's cursors that is both end-to-end (Win32
    // GetCursorInfo, the same call a user's eye makes) and deterministic:
    // a separate process reading the live cursor races the human at the
    // keyboard, while this reads it in the same tick that parked it.
    Q_INVOKABLE QString cursorAt(double x, double y);
    // Is the item actually ON SCREEN: effective visibility (every ancestor
    // shown too) AND laid out (a non-zero size). An item that fails this is not
    // a click target — an unlaid-out one reports (0,0), which is how a scenario
    // used to click a card's top-left corner instead of the pill it named.
    Q_INVOKABLE bool itemVisible(const QString &objectName);
    Q_INVOKABLE void clickItem(const QString &objectName);
    Q_INVOKABLE bool grab(const QString &itemName, const QString &pngPath);
    // Resolve an item by objectName (no action) — for scenarios that poke
    // properties the invokables don't cover (e.g. a Flickable's contentY).
    Q_INVOKABLE QObject *findItem(const QString &objectName);
    // Every TOP-LEVEL window the app has, as { objectName, title, visible,
    // color } maps (color as "#aarrggbb", Qt's own name for it).
    //
    // The OUTPUT windows are separate top-level windows, so the lookups above —
    // which walk THIS window's item tree — cannot see them at all, and the one
    // thing about them that no other diagnostic covers is whether they composite
    // with alpha: a keyable frame is alpha 0 out of the engine, and a window
    // that fills that in black is the whole "it only shows on the tile" fault.
    // objectName is how a scenario tells an output window from the main one
    // (OutputWindow.qml sets "outputWindow").
    Q_INVOKABLE QVariantList windows() const;
    Q_INVOKABLE void quit();

private:
    QQuickWindow *m_window = nullptr;
};
