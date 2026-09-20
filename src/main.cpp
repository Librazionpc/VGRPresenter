#include <QGuiApplication>
#include <QQuickWindow>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QStringList>

#include <cstdio>

#include "services/CrashHandler.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "SelfTestDriver.h"

namespace {

// Replaces Qt's default message handler. Keeps printing to stderr exactly as
// before (this app's established `QT_FORCE_STDERR_LOGGING=1` diagnostic
// workflow depends on that), and additionally republishes warnings/errors
// through EventBus so they can surface as a toast instead of only being
// visible to someone tailing a log. qDebug/qInfo stay console-only — every
// binding re-evaluation would otherwise spam the UI.
void ForwardToEventBus(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)
    std::fprintf(stderr, "%s\n", qUtf8Printable(msg));
    std::fflush(stderr);

    QString level;
    QString topic;
    switch (type) {
    case QtWarningMsg:  level = QStringLiteral("warning"); topic = QStringLiteral("log.warning");  break;
    case QtCriticalMsg: level = QStringLiteral("error");   topic = QStringLiteral("log.critical"); break;
    case QtFatalMsg:    level = QStringLiteral("error");   topic = QStringLiteral("log.fatal");    break;
    default:
        return;
    }
    EventBus::instance().publish(topic, QVariantMap{
        {QStringLiteral("level"), level},
        {QStringLiteral("title"), QStringLiteral("Application")},
        {QStringLiteral("message"), msg},
    });
}

} // namespace

int main(int argc, char *argv[])
{
    qInstallMessageHandler(ForwardToEventBus);

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("VGRPresenter"));
    app.setOrganizationName(QStringLiteral("VGR"));

    // Needs QStandardPaths, which needs the QGuiApplication constructed above.
    InstallCrashHandler();

    // Boots the real PresentationEngine before any QML loads, so every
    // screen has it available from its very first frame. See EngineBridge.h
    // for exactly what this does and doesn't touch — sandboxed data/log
    // dirs, no plugins, no network listener. A failure here is reported as
    // a toast (via EventBus, already wired above) rather than aborting
    // startup — the UI still works against its existing mock data either way.
    (void)EngineBridge::instance().boot();
    QObject::connect(&app, &QGuiApplication::aboutToQuit, [] {
        EngineBridge::instance().shutdown();
    });

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, [] {
            qWarning() << "Failed to create the QML application window.";
            QCoreApplication::exit(-1);
        }, Qt::QueuedConnection);
    QObject::connect(
        &engine, &QQmlApplicationEngine::warnings,
        &app, [](const QList<QQmlError> &warnings) {
            QStringList lines;
            for (const QQmlError &e : warnings)
                lines << e.toString();
            EventBus::instance().publish(QStringLiteral("log.qml"), QVariantMap{
                {QStringLiteral("level"), QStringLiteral("warning")},
                {QStringLiteral("title"), QStringLiteral("QML")},
                {QStringLiteral("message"), lines.join(QStringLiteral("\n"))},
            });
        });

    // Crash notification, UI side: if the previous run went down in flames,
    // its CrashHandler could only leave a log file (firing toasts from inside
    // a dying process is unsafe by design). Consume that record BEFORE the QML
    // loads and expose it as a context property — Main.qml's
    // Component.onCompleted turns it into a standard "Recovered from a crash"
    // toast on the normal EventBus pipeline. Rotated aside = reported exactly
    // once.
    engine.rootContext()->setContextProperty(
        QStringLiteral("pendingCrashSummary"), ConsumePendingCrashSummary());

    engine.loadFromModule("VGRPresenterUI", "Main");

    // TEMPORARY diagnostic (kept, env-gated): UI self-test driver. With
    // VGR_SELFTEST=1 the Main.qml scenario can drive the real app with REAL
    // cursor moves (genuine OS hover events) + synthetic clicks, and grab
    // any named item to a PNG for offline pixel sampling — pixel truth for
    // rendering-layer bug reports that property traces can never see.
    // Inert (not even instantiated) without the env var.
    SelfTestDriver selfTestDriver;
    if (qEnvironmentVariableIsSet("VGR_SELFTEST") && !engine.rootObjects().isEmpty()) {
        selfTestDriver.setWindow(qobject_cast<QQuickWindow *>(engine.rootObjects().first()));
        engine.rootContext()->setContextProperty(QStringLiteral("SelfTest"), &selfTestDriver);
    }

    // A safety net for exceptions that escape the event loop (e.g. from a
    // future engine-bridge call) — logs and exits cleanly instead of letting
    // the OS's own unhandled-exception dialog appear. A true memory-access
    // crash still goes through CrashHandler.cpp's SEH/signal path, which
    // this can't catch.
    try {
        return app.exec();
    } catch (const std::exception &e) {
        qCritical() << "Unhandled exception:" << e.what();
        return 1;
    } catch (...) {
        qCritical() << "Unhandled unknown exception.";
        return 1;
    }
}
