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
//   move(x, y)          — real cursor move to window-local logical (x, y)
//   click(x, y)         — move + left down/up at that point
//   grab(itemName, path)— window.findItem<objectName=itemName> -> PNG
//   quit()              — clean Qt.quit()
// (x, y) are in window coordinates at the app's 1440x900 logical size —
// the same space every fixed layout coordinate in this app is written in.

#include <QObject>
#include <QPointF>
#include <QString>

class QQuickWindow;

class SelfTestDriver : public QObject
{
    Q_OBJECT
public:
    explicit SelfTestDriver(QObject *parent = nullptr);

    // The window whose local coordinate space move/click/grab speak. Set
    // right after engine load, from main.cpp.
    void setWindow(QQuickWindow *window);

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
    Q_INVOKABLE void clickItem(const QString &objectName);
    Q_INVOKABLE bool grab(const QString &itemName, const QString &pngPath);
    // Resolve an item by objectName (no action) — for scenarios that poke
    // properties the invokables don't cover (e.g. a Flickable's contentY).
    Q_INVOKABLE QObject *findItem(const QString &objectName);
    Q_INVOKABLE void quit();

private:
    QQuickWindow *m_window = nullptr;
};
