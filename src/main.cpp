#include <QGuiApplication>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QStringList>

#include <cstdio>

#include "CrashHandler.h"
#include "EventBus.h"

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

    engine.loadFromModule("VGRPresenterUI", "Main");

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
