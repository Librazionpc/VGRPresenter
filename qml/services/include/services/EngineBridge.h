#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJSValue>
#include <QVariantList>
#include <qqml.h>

#include "core/events/EventBus.hpp"

class QJSEngine;
class QQmlEngine;

// The bridge between the Qt UI process and the real PresentationEngine
// (src/PresentationEngine). Two independent things live here:
//
//  1. Kernel boot — boot()/shutdown() drive bps::Kernel::Instance(), the
//     29-system engine (see Kernel.hpp/.cpp). This is the heavy, side-
//     effectful step: it opens a real database, mounts content directories,
//     starts a thread pool + scheduler, and initializes Render/Display/
//     Media/Broadcast (all lazy/software-backed by default — verified by
//     reading their Initialize() before wiring this — no GPU context, no
//     hardware opened up front). Called once from main.cpp, after
//     QGuiApplication exists (needed for QStandardPaths) and before the
//     QML engine loads. NOT implicitly triggered by using the pieces below.
//
//  2. UndoRedoManager — wraps bps::project::UndoRedoManager
//     (modules/include/modules/project/UndoRedoManager.hpp), the engine's
//     command-based undo/redo stack. This one is a plain, self-contained
//     Meyers singleton with NO dependency on Kernel::Boot() (no disk I/O,
//     no threads) — usable whether or not boot() has run. See
//     docs/architecture/ProjectSystem.md's "Undo/Redo".
//
// pushCommand's doFn/undoFn are plain QML functions — the ENGINE owns
// ordering/grouping/depth-capping, QML owns what each step actually DOES.
// Both run synchronously on the calling thread; never call pushCommand/
// undo/redo/boot/shutdown from a worker thread.
class EngineBridge : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stackChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stackChanged)
    Q_PROPERTY(bool booted READ booted NOTIFY bootedChanged)

public:
    static EngineBridge &instance();
    static EngineBridge *create(QQmlEngine *engine, QJSEngine *jsEngine);

    bool canUndo() const;
    bool canRedo() const;
    bool booted() const;

    // Boots the real Kernel with a sandboxed, real-world-safe configuration:
    // an isolated per-app data/log directory under the OS's standard app
    // data location, no plugin directories scanned, no IPC port opened.
    // Idempotent — a second call is a harmless no-op (Kernel::Boot() itself
    // refuses a re-boot from Running state). Returns false and leaves a
    // human-readable reason in bootError() on failure.
    bool boot();
    Q_INVOKABLE bool isBooted() const { return booted(); }
    Q_INVOKABLE QString bootError() const;
    Q_INVOKABLE QStringList bootLog() const;
    Q_INVOKABLE QString health() const;
    // Called from main.cpp on QGuiApplication::aboutToQuit — orderly
    // teardown of the 29 systems (threads joined, database flushed) instead
    // of letting process exit cut them off mid-flight.
    void shutdown();

    // GUI-thread-only entry point the relay's marshalled lambdas call (see
    // startRelay in EngineBridge.cpp): appends to the recent-events ring,
    // emits engineEvent, publishes to the UI EventBus. Public because the
    // EngineLogSink (a free class, not a member) invokes it via
    // QMetaObject::invokeMethod after hopping off the Logger's engine thread.
    void ingestEngineEvent(const QString &topic, const QVariantMap &payload);

    // Registers one reversible step — doFn runs now (ExecuteCommand's own
    // contract), undoFn runs on undo(), doFn runs again on redo() (ICommand's
    // default Redo() == Execute()). Clears the redo branch, same as any
    // fresh edit after an undo.
    Q_INVOKABLE void pushCommand(const QString &label, const QJSValue &doFn, const QJSValue &undoFn);
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    Q_INVOKABLE QString undoLabel() const;
    Q_INVOKABLE QString redoLabel() const;
    Q_INVOKABLE void clearHistory();

signals:
    void stackChanged();
    void bootedChanged();
    // Fired for every ENGINE-side event relayed into the UI process — kernel
    // state changes, display/recording/render/media/broadcast failures and
    // recoveries, undo/redo performed, ... (the curated set in
    // EngineBridge.cpp's startRelay). topic is the engine's own kTopic string
    // verbatim ("recording.failed", "presentation.slide_changed"), so QML can
    // filter with `if (topic === "...")`. Emitted on the GUI thread no matter
    // which engine thread produced the event. EventBus.eventPosted carries the
    // same payload for the toast pipeline; this signal is the typed,
    // engine-specific feed for screens that want to react without seeing UI-
    // internal traffic.
    void engineEvent(const QString &topic, const QVariantMap &payload);

private:
    explicit EngineBridge(QObject *parent = nullptr);
    Q_DISABLE_COPY(EngineBridge)

    QString bootError_;

    // ---- Engine → UI relay (see startRelay in EngineBridge.cpp) ----------
    // The kernel drives everything; the UI only relays. Attaches a Logger
    // sink (engine warnings/errors/fatals) and engine-EventBus subscriptions
    // (the curated user-facing event set); each hops to the GUI thread via a
    // queued invoke before touching the UI-side EventBus.
    void startRelay();
    void stopRelay();

    std::vector<bps::Subscription> relaySubs_;
    QVariantList recentEngineEvents_;   // ring capped at kMaxRecentEvents
    static constexpr int kMaxRecentEvents = 200;

public:
    // The most recent relayed engine events (newest LAST), for diagnostics
    // surfaces (a future debug console / activity log). level/title/message
    // keys match the EventBus toast convention; "topic" is the engine topic.
    Q_INVOKABLE QVariantList recentEngineEvents(int max = 50) const;
};
