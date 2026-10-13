#pragma once

#include <QObject>
#include <QString>
#include <qqml.h>

class QJSEngine;
class QQmlEngine;

// The OS-level half of the app's cursor layer: an application OVERRIDE cursor,
// held while AppCursor's override stack is non-empty.
//
// WHY THIS EXISTS, next to AppCursorCatcher (which already renders the pushed
// shape through a full-window HoverHandler): measured with the app's own
// cursor scenario, the shape AppCursor holds is correct while the OS still
// shows an arrow. Parked on a window-frame grip the stack reports
// `shape=6 (SizeHor)` with the grip hovered, and Win32 GetCursorInfo still
// returns ARROW — so the per-item cursor path (a QML cursor claim, applied by
// QQuickWindowPrivate::updateCursor on the NEXT pointer event) is not enough on
// the window's frame: the OS paints that strip itself and Qt's claim never
// reaches it. An override cursor is applied by Qt ahead of any item cursor and
// ahead of the OS's own non-client cursor painting, on the same event, so
// "the cursor just flashes and never shows" (dragging the app's border to
// rescale it) becomes a steady size arrow.
//
// Held/cleared from ONE place: AppCursorCatcher.syncShape(), which already
// mirrors AppCursor's winner into the catcher. AppCursor's stack is
// owner-scoped and self-healing, so the override can no longer be held by an
// owner that went away — the same invariant the stack itself relies on.
class CursorService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    // Plain C++ access (works without a QQmlEngine).
    static CursorService &instance();

    // QML singleton factory — the SAME instance as instance().
    static CursorService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    // Show `shape` (a Qt::CursorShape) as the OS cursor from now on. Idempotent
    // per shape: re-applying a DIFFERENT shape swaps it in place rather than
    // stacking overrides (a stack would need exactly-matched restores, which is
    // how an override ends up stuck on screen forever).
    Q_INVOKABLE void apply(int shape);

    // Release the override (nothing held → no-op), so the normal per-item
    // cursors apply again.
    Q_INVOKABLE void clear();

    // True while an override is held — what a probe/scenario reads to tell
    // "the app never asked for a cursor" apart from "it asked and the OS did
    // not show it".
    Q_INVOKABLE bool held() const { return m_held; }

private:
    explicit CursorService(QObject *parent = nullptr);
    Q_DISABLE_COPY(CursorService)

    bool m_held = false;
};
