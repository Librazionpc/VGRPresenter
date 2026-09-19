#include "EngineBridge.h"
#include "EventBus.h"

#include "core/kernel/Kernel.hpp"
#include "core/logging/Logger.hpp"
#include "modules/project/UndoRedoManager.hpp"

#include <QDebug>
#include <QDir>
#include <QJSEngine>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QVariantMap>

namespace {

// Wraps two QML callables as one bps::project::ICommand — the engine's
// stack only ever sees a Label()/Execute()/Undo(), never that the actual
// steps are QML functions.
class QmlCommand final : public bps::project::ICommand
{
public:
    QmlCommand(QString label, QJSValue doFn, QJSValue undoFn)
        : label_(std::move(label)), do_(std::move(doFn)), undo_(std::move(undoFn))
    {
    }

    void Execute() override
    {
        if (do_.isCallable())
            do_.call();
    }

    void Undo() override
    {
        if (undo_.isCallable())
            undo_.call();
    }

    std::string Label() const override { return label_.toStdString(); }

private:
    QString label_;
    QJSValue do_;
    QJSValue undo_;
};

bps::project::UndoRedoManager &Manager()
{
    return bps::project::UndoRedoManager::Instance();
}

} // namespace

EngineBridge::EngineBridge(QObject *parent)
    : QObject(parent)
{
}

EngineBridge &EngineBridge::instance()
{
    static EngineBridge bridge;
    return bridge;
}

EngineBridge *EngineBridge::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

bool EngineBridge::canUndo() const
{
    return Manager().CanUndo();
}

bool EngineBridge::canRedo() const
{
    return Manager().CanRedo();
}

bool EngineBridge::booted() const
{
    return bps::Kernel::Instance().State() == bps::KernelState::Running;
}

bool EngineBridge::boot()
{
    if (booted())
        return true;

    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString logDir = base + QStringLiteral("/logs");
    const QString dataDir = base + QStringLiteral("/enginedata");
    QDir().mkpath(logDir);
    QDir().mkpath(dataDir);

    // Installed BEFORE Boot() so Logger::Initialize()'s own default-sink path
    // (a relative "logs/engine.log", resolved against the process's current
    // working directory — fine for our own build-dir test runs, fragile for
    // a real double-clicked/shortcut launch where the CWD is unpredictable)
    // never gets added: Initialize() only seeds defaults `if (sinks_.empty())`.
    (void)bps::Logger::Instance().AddSink(
        std::make_shared<bps::FileSink>((logDir + QStringLiteral("/engine.log")).toStdString()));

    bps::BootOptions options;
    options.dataDir = dataDir.toStdString();
    options.logLevel = bps::LogLevel::Info;
    // pluginDirs left empty, ipcPort left 0 (its BootOptions default) —
    // no plugin loading, no network listener opened.

    auto result = bps::Kernel::Instance().Boot(options);
    emit bootedChanged();

    if (!result.ok()) {
        bootError_ = QString::fromStdString(result.error().message);
        EventBus::instance().publish(QStringLiteral("engine.boot"), QVariantMap{
            {QStringLiteral("level"), QStringLiteral("error")},
            {QStringLiteral("title"), QStringLiteral("Engine")},
            {QStringLiteral("message"), QStringLiteral("Engine failed to boot: ") + bootError_},
        });
        return false;
    }

    bootError_.clear();
    EventBus::instance().publish(QStringLiteral("engine.boot"), QVariantMap{
        {QStringLiteral("level"), QStringLiteral("success")},
        {QStringLiteral("title"), QStringLiteral("Engine")},
        {QStringLiteral("message"), QStringLiteral("Presentation engine booted (%1 systems).")
            .arg(bps::Kernel::Instance().BootLog().size())},
    });
    return true;
}

QString EngineBridge::bootError() const
{
    return bootError_;
}

QStringList EngineBridge::bootLog() const
{
    QStringList out;
    for (const auto &s : bps::Kernel::Instance().BootLog())
        out << QString::fromStdString(s);
    return out;
}

QString EngineBridge::health() const
{
    return QString::fromStdString(bps::Kernel::Instance().GetHealth().detail);
}

void EngineBridge::shutdown()
{
    if (!booted())
        return;
    (void)bps::Kernel::Instance().Shutdown();
    emit bootedChanged();
}

void EngineBridge::pushCommand(const QString &label, const QJSValue &doFn, const QJSValue &undoFn)
{
    auto cmd = std::make_shared<QmlCommand>(label, doFn, undoFn);
    (void)Manager().ExecuteCommand(cmd);
    emit stackChanged();
}

bool EngineBridge::undo()
{
    const bool ok = Manager().Undo().ok();
    emit stackChanged();
    return ok;
}

bool EngineBridge::redo()
{
    const bool ok = Manager().Redo().ok();
    emit stackChanged();
    return ok;
}

QString EngineBridge::undoLabel() const
{
    return QString::fromStdString(Manager().UndoLabel());
}

QString EngineBridge::redoLabel() const
{
    return QString::fromStdString(Manager().RedoLabel());
}

void EngineBridge::clearHistory()
{
    Manager().Clear();
    emit stackChanged();
}
