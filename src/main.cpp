#include <QElapsedTimer>
#include <QGuiApplication>
#include <QIcon>
#include <QQuickWindow>
#include <QDateTime>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <mutex>

#include "services/CrashHandler.h"
#include "services/EngineBridge.h"
#include "services/SearchService.h"
#include "services/MediaLibraryService.h"
#include "services/TheTableService.h"
#include "services/LiveOutputService.h"
#include "services/RecordingService.h"
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
    // The running app's title-bar/taskbar icon: the same multi-size .ico the
    // build embeds as the executable's own resource (RC_ICONS in
    // CMakeLists.txt — Explorer/taskbar-pinned icon). QML's ApplicationWindow
    // ALSO sets it on the window itself (Main.qml icon.source) — a
    // QQuickWindow does not reliably adopt the application icon on Windows,
    // and the taskbar reads the WINDOW's icon. This app-level one covers
    // any native dialog windows that don't go through Main.qml.
    const QIcon appIcon(QStringLiteral(":/app/app-icon.ico"));
    if (appIcon.isNull())
        qWarning() << "App icon failed to load from ':/app/app-icon.ico' — the taskbar will show the default icon";
    app.setWindowIcon(appIcon);

    // Needs QStandardPaths, which needs the QGuiApplication constructed above.
    InstallCrashHandler();

    // The engine boots DEFERRED — see the singleShot after the QML load.
    // Booting here (synchronously, before any QML exists) left the screen
    // BLANK for the whole kernel boot; now the branded boot splash is the
    // visible face of the same call. A failure is reported as a toast (via
    // EventBus, already wired above) and the splash still comes down — the
    // UI keeps working against its existing mock data either way.
    QObject::connect(&app, &QGuiApplication::aboutToQuit, [] {
        // A Bible import may still be running on its own thread; let it finish before
        // the engine's systems are torn down under it.
        SearchService::instance().shutdown();
        MediaLibraryService::instance().shutdown();   // folder scans in progress
        // The Table's search indexer: cancel + join BEFORE the kernel shutdown —
        // a still-running upsert pass into the Search Engine mid-teardown was the
        // 0xc0000005 on quit (the pass now stops between documents and joins here).
        TheTableService::instance().shutdownIndexing();
        // The NDI send worker: stop + join BEFORE the engine teardown — the
        // SDK/provider must never be torn down under an in-flight frame send
        // (same quit-crash class as the indexer above).
        LiveOutputService::instance().shutdownNdiWorker();
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

    // Camera preview frames (image://videopreview/<label>?<nonce>) — the
    // QQuickImageProvider half of EngineBridge's video preview taps, same
    // up-front registration convention.
    engine.addImageProvider(QStringLiteral("videopreview"),
                            new EngineBridge::VideoPreviewProvider(&EngineBridge::instance()));

    // Taken-media frames (image://mediaplay?v=<mediaRev>) — LiveOutputService's
    // media-on-air player (a video/image file composited into every monitor
    // tile under the on-air content), same up-front registration convention.
    engine.addImageProvider(QStringLiteral("mediaplay"), new LiveMediaFrameProvider);

    engine.loadFromModule("VGRPresenterUI", "Main");

    // The window's own taskbar/alt-tab icon: a QQuickWindow does NOT reliably
    // adopt QGuiApplication::setWindowIcon() on Windows — the exe carried the
    // icon (RC_ICONS) while the taskbar still showed the generic placeholder
    // (this main window is frameless, so the taskbar button is its only
    // OS-drawn chrome). QWindow::setIcon() is what reaches WM_SETICON. There
    // is no Window.icon property in the QML API, so this has to happen here.
    if (auto *rootWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)))
        rootWindow->setIcon(appIcon);

    // DEFERRED ENGINE BOOT — the beat that closes the boot-splash contract:
    // by now the window is up and the splash has painted (the 200 ms covers
    // its fade-in), so the kernel boots behind the splash instead of behind
    // a blank desktop. Boot() is synchronous ON THE GUI THREAD by contract
    // (EngineBridge.h forbids a worker-thread boot) — the splash holds still
    // for its duration, then Main.qml's BootSplash reveals on bootedChanged
    // (or bootError, on failure).
    QTimer::singleShot(200, &app, [] {
        QElapsedTimer bootClock;
        bootClock.start();
        (void)EngineBridge::instance().boot();
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("App"),
                            QStringLiteral("engine boot finished in %1 ms (deferred — the boot splash was on screen first)")
                                .arg(bootClock.elapsed()));
    });

    // Env-gated taken-input self-test: takes a real window through the
    // whole output-preview chain ~2.5s after launch and logs PASS/FAIL.
    // Inert without VGR_OUTPUT_INPUT_TEST=1 (the static itself checks).
    LiveOutputService::runEnvSelfTest();

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
        const int code = app.exec();
        // Finalize any still-running recording BEFORE the kernel tears the
        // engine down — the file on disk must be complete (a planned exit
        // has no excuse for a half-written recording; a CRASHED one recovers
        // through the engine's journal on the next boot).
        RecordingService::instance().shutdown();
        return code;
    } catch (const std::exception &e) {
        qCritical() << "Unhandled exception:" << e.what();
        RecordingService::instance().shutdown();
        return 1;
    } catch (...) {
        qCritical() << "Unhandled unknown exception.";
        RecordingService::instance().shutdown();
        return 1;
    }
}
