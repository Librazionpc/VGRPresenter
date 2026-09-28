#pragma once

// The QML face of the engine's FlowEngine (Phase 14, docs/specs/26) — the
// Functions tab's service-flow automation. The ENGINE runs entire services:
// a Flow is an ordered list of executable nodes (actions, conditions, delays,
// waits on engine events) that runs the countdown → welcome → worship →
// sermon sequence with the operator always able to pause/skip/jump/stop.
//
// This service:
//   • REGISTERS the app's domain actions into the engine's action registry
//     ("go_live", "next_slide", "previous_slide", "clear_air", "start_rec",
//     "stop_rec", "take_ndi_off") — real commands against LiveOutputService /
//     RecordingService, so a flow's steps actually drive the service. The
//     engine core stays decoupled (its own docs' plugin pattern).
//   • LOADS built-in flow documents (.vgr JSON, the engine's native format)
//     on boot: a starter Sunday-Service flow the user can run immediately.
//   • RUNS/DRIVES executions: Start → an execution id; pause/resume/skip/stop
//     + jumpTo are one call each. A 250 ms Poll pump advances delay nodes
//     (the engine is externally-clocked, same contract RecordingService's
//     pump honors) and refreshes the live state view.
//   • PUBLISHES state: flows (id/name/node list) and the active execution's
//     node timeline with per-node states — the Functions pane draws both.

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace bps::automation {
class FlowEngine;
}

class QTimer;
class QQmlEngine;
class QJSEngine;

class FlowService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Every loaded flow: { id, name, nodeCount, nodes: [{ id, label, kind }] }.
    Q_PROPERTY(QVariantList flows READ flows NOTIFY flowsChanged)
    // The execution currently shown in the pane ("" = none). Runs one at a
    // time in the UI (the engine itself allows many).
    Q_PROPERTY(QString executionId READ executionId NOTIFY executionChanged)
    // Live state of the shown execution: { state, flowId, currentNodeId,
    // waitingFor, nodes: [{ id, label, kind, state, result }] }.
    Q_PROPERTY(QVariantMap execution READ execution NOTIFY executionChanged)
    // True while the shown execution is Running (the transport shows Stop).
    Q_PROPERTY(bool running READ running NOTIFY executionChanged)

public:
    static FlowService &instance();
    static FlowService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    ~FlowService() override;

    QVariantList flows() const { return flows_; }
    QString executionId() const { return executionId_; }
    QVariantMap execution() const { return execution_; }
    bool running() const { return running_; }

    // Re-reads the engine's flow registry (also called after a Load).
    Q_INVOKABLE void refreshFlows();
    // Loads a .vgr/json flow document; the new id toasts on success.
    Q_INVOKABLE bool loadFlow(const QString &document);
    // Loads the built-in starter flow (idempotent — a re-load replaces it).
    Q_INVOKABLE void loadStarterFlow();
    // Removes a flow from the registry.
    Q_INVOKABLE void removeFlow(const QString &flowId);

    // ---- Transport (docs/specs/26 §Manual Override — the operator wins) ----
    Q_INVOKABLE void start(const QString &flowId);
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void skip();          // skip the current/waiting node
    Q_INVOKABLE void stop();
    Q_INVOKABLE void jumpTo(const QString &nodeId);
    // Fire one action right now (the pane's quick-action buttons).
    Q_INVOKABLE bool executeAction(const QString &actionType, const QString &payload);

    // Waits out the poll pump before engine shutdown.
    void shutdown();

signals:
    void flowsChanged();
    void executionChanged();

private:
    explicit FlowService(QObject *parent = nullptr);
    static FlowService *s_instance;

    void registerDomainActions();
    void refreshExecution();
    // The 250 ms pump: engine Poll() (delay/timer nodes) + state refresh.
    void pumpTick();

    QVariantList flows_;
    QString executionId_;
    QVariantMap execution_;
    bool running_ = false;
    QTimer *pump_ = nullptr;
    bool engineWired_ = false;   // actions registered / starter loaded once
};
