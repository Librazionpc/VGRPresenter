#include <QGuiApplication>
#include <QQuickWindow>
#include <QDateTime>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QStandardPaths>
#include <QStringList>

#include <cstdio>
#include <mutex>

#include "services/CrashHandler.h"
#include "services/EngineBridge.h"
#include "services/SearchService.h"
#include "services/MediaLibraryService.h"
#include "services/LiveOutputService.h"
#include "services/MediaThumbnailProvider.h"
#include "services/EventBus.h"
#ifdef VGR_ENABLE_SELFTEST
#include "SelfTestDriver.h"
#endif

namespace {

// Replaces Qt's default message handler. Keeps printing to stderr exactly as
// before (this app's established `QT_FORCE_STDERR_LOGGING=1` diagnostic
// workflow depends on that), and additionally republishes warnings/errors
// through EventBus so they can surface as a toast instead of only being
// visible to someone tailing a log. qDebug/qInfo stay console-only — every
// binding re-evaluation would otherwise spam the UI.
void ForwardToEventBus(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
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
    // A warning raised WHILE a warning is being forwarded (a toast delegate
    // whose own binding errors, say) would otherwise feed itself forever.
    static thread_local bool forwarding = false;
    if (forwarding)
        return;

    // The same binding error can fire on every re-evaluation — show it once,
    // and again only after a quiet spell. (stderr above always prints.)
    // (Leaked on purpose and mutex-guarded: engine threads can log too, and this
    // handler may run after static destructors.)
    struct Dedupe { std::mutex m; QString lastMsg; qint64 lastAt = 0; };
    static Dedupe *dedupe = new Dedupe;
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        std::lock_guard<std::mutex> lock(dedupe->m);
        if (msg == dedupe->lastMsg && now - dedupe->lastAt < 3000)
            return;
        dedupe->lastMsg = msg;
        dedupe->lastAt = now;
    }

    // QML engine diagnostics arrive here — this is the ONE path for them (the
    // engine's warnings() signal used to be forwarded too, doubling every QML
    // error). Binding/TypeErrors reported by the engine itself carry no "qml" category —
    // recognise them by the "<url>:<line>:" prefix they always start with.
    const bool isQml = (context.category && qstrncmp(context.category, "qml", 3) == 0)
        || msg.startsWith(QLatin1String("qrc:/")) || msg.startsWith(QLatin1String("file:///"));

    // The same line goes to the engine log, so one file holds everything that went wrong (QML errors included).
    EngineBridge::write(level, isQml ? QStringLiteral("QML") : QStringLiteral("Qt"), msg);

    // Qt's own Windows font backend, probing a legacy system font ("Fixedsys") while resolving a monospace-style hint: a harmless
    // fallback-candidate miss (the font that actually gets used renders fine), not anything wrong in the app - logged above, but never
    // a toast, so it stops looking like an application error.
    if (msg.contains(QLatin1String("DirectWrite: CreateFontFaceFromHDC")))
        return;

    forwarding = true;
    EventBus::instance().publish(isQml ? QStringLiteral("log.qml") : topic, QVariantMap{
        {QStringLiteral("level"), level},
        {QStringLiteral("title"), isQml ? QStringLiteral("QML") : QStringLiteral("Application")},
        {QStringLiteral("message"), msg},
    });
    forwarding = false;
}

} // namespace

int main(int argc, char *argv[])
{
    // VGR_SANDBOX=1 points every QStandardPaths location (engine data, logs, crash
    // records) at Qt's throwaway test area, so a diagnostic/CI run can never touch —
    // or fight over the database of — a real installation running at the same time.
    if (qEnvironmentVariableIsSet("VGR_SANDBOX"))
        QStandardPaths::setTestModeEnabled(true);

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
        // A Bible import may still be running on its own thread; let it finish before
        // the engine's systems are torn down under it.
        SearchService::instance().shutdown();
        MediaLibraryService::instance().shutdown();   // folder scans in progress
        EngineBridge::instance().shutdown();
    });

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, [] {
            qWarning() << "Failed to create the QML application window.";
            QCoreApplication::exit(-1);
        }, Qt::QueuedConnection);
    // (No QQmlApplicationEngine::warnings hookup: the engine already routes every
    // QML error through the message handler above, so hooking the signal too
    // raised each error twice.)

    // Crash notification, UI side: if the previous run went down in flames,
    // its CrashHandler could only leave a log file (firing toasts from inside
    // a dying process is unsafe by design). Consume that record BEFORE the QML
    // loads and expose it as a context property — Main.qml's
    // Component.onCompleted turns it into a standard "Recovered from a crash"
    // toast on the normal EventBus pipeline. Rotated aside = reported exactly
    // once.
    engine.rootContext()->setContextProperty(
        QStringLiteral("pendingCrashSummary"), ConsumePendingCrashSummary());

    // Video preview frames for the Media tab (image://mediathumb/<path>).
    engine.addImageProvider(QStringLiteral("mediathumb"), new MediaThumbnailProvider);

    // Live-output preview frames (image://livepreview?v=<frameRev>) — the
    // QQuickImageProvider half of LiveOutputService, registered up-front so
    // the QML Image resolves the URL before the singleton exists.
    engine.addImageProvider(QStringLiteral("livepreview"), new LivePreviewProvider);

    engine.loadFromModule("VGRPresenterUI", "Main");

    // TEMPORARY diagnostic (kept, env-gated): UI self-test driver. With
    // VGR_SELFTEST=1 the Main.qml scenario can drive the real app with REAL
    // cursor moves (genuine OS hover events) + synthetic clicks, and grab
    // any named item to a PNG for offline pixel sampling — pixel truth for
    // rendering-layer bug reports that property traces can never see.
    // Inert (not even instantiated) without the env var.
#ifdef VGR_ENABLE_SELFTEST
    SelfTestDriver selfTestDriver;
    if (qEnvironmentVariableIsSet("VGR_SELFTEST") && !engine.rootObjects().isEmpty()) {
        selfTestDriver.setWindow(qobject_cast<QQuickWindow *>(engine.rootObjects().first()));
        engine.rootContext()->setContextProperty(QStringLiteral("SelfTest"), &selfTestDriver);
    }
#endif

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
