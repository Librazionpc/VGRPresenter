#pragma once

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

    void runTransportOffThread(const QString &what);

    // Re-reads the engine's flow registry (also called after a Load). The read
    // runs on the engine worker; the list lands here and emits flowsChanged.
    Q_INVOKABLE void refreshFlows();
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
    // Fire one action right now (the pane's quick-action buttons). True =
    // accepted for firing; a refusal arrives as its toast, from the worker.
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