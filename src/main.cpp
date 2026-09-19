#include <QGuiApplication>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQmlContext>
#include <QStringList>

#include <QCommandLineParser>
#include <QPoint>
#include <QPointF>
#include <QTimer>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>

#include <cstdio>

#include "services/CrashHandler.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"

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
    // TEMP A/B: hide the window-root cursor catcher to test whether its
    // HoverHandler is what suppresses MouseArea hover-exit delivery.
    // REMOVE WITH THE A/B RESULT.
    engine.rootContext()->setContextProperty(
        QStringLiteral("hoverProbeNoCatcher"),
        qEnvironmentVariableIsSet("VGR_NO_CATCHER"));
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

    // ---- TEMP hover A/B probe driver (--hoverprobe logicalX,logicalY;...) ----
    // REMOVE WITH THE A/B RESULT. Synthesizes mouse input through
    // QWindowSystemInterface — the exact pipeline real OS pointer events
    // enter through.
    {
        QCommandLineParser parser;
        parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
        QCommandLineOption probeOpt("hoverprobe", "Run hover probe", "hoverprobe");
        parser.addOption(probeOpt);
        parser.process(app);
        if (parser.isSet(probeOpt)) {
            static const QStringList steps = parser.value(probeOpt).split(';');
            auto *probeTimer = new QTimer(&app);
            probeTimer->setInterval(600);
            static int step = 0;
            QObject::connect(probeTimer, &QTimer::timeout, [&]() {
                QWindow *win = QGuiApplication::topLevelWindows().isEmpty()
                    ? nullptr : QGuiApplication::topLevelWindows().first();
                if (!win || !win->isVisible()) return;
                if (step >= steps.size()) { probeTimer->stop(); return; }
                const QString s = steps[step++];
                const int comma = s.indexOf(',');
                if (comma < 0) return;
                const double lx = s.left(comma).toDouble();
                const double ly = s.mid(comma + 1).toDouble();
                const double dsf = win->devicePixelRatio();
                const double designW = 1440.0;
                const double scale = (win->width() / dsf) / designW;
                const QPointF local(lx * scale * dsf, ly * scale * dsf);
                QWindowSystemInterface::handleMouseEvent(win, local, win->mapToGlobal(local),
                                                         Qt::NoButton, Qt::NoButton, QEvent::MouseMove);
            });
            probeTimer->start();
        }
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
