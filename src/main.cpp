#include <QElapsedTimer>
#include <QGuiApplication>
#include <QIcon>
#include <QQuickWindow>
#include <QDateTime>
#include <QDebug>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QVariant>
#include <QQmlError>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <atomic>
#include <chrono>
#include <iterator>
#include <mutex>
#include <thread>
#include <condition_variable>
#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "services/CrashHandler.h"
#include "services/EngineBridge.h"
#include "services/SearchService.h"
#include "services/MediaLibraryService.h"
#include "services/TheTableService.h"
#include "services/LiveOutputService.h"
#include "services/RecordingService.h"
#include "services/SettingsService.h"
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

    // "vgr.*.progress" categories are the app narrating its own normal
    // operation — the NDI feed's "sending started / frames flowing / 2
    // monitor(s) connected" run, the kind of line that belongs in the engine
    // log and not in a toast. Five of them fired on every GO LIVE and buried
    // the screen the operator was working in.
    //
    // Recognised by CATEGORY, not by matching message text: a new progress
    // line added at a call site marked qCWarning(lcSomeProgress, ...) is
    // quiet automatically, and there is no prefix list here to keep in sync.
    // Actionable lines keep qWarning and therefore keep toasting — "NDI
    // SendFrame FAILED", "NDI is not usable", "media player error", every
    // selftest FAIL.
    const bool isProgress = context.category
        && qstrncmp(context.category, "vgr.", 4) == 0
        && QLatin1String(context.category).contains(QLatin1String(".progress"));

    // The same line goes to the engine log, so one file holds everything that went wrong (QML errors included).
    EngineBridge::write(level, isQml ? QStringLiteral("QML") : QStringLiteral("Qt"), msg);

    // Qt's own Windows font backend, probing a legacy system font ("Fixedsys") while resolving a monospace-style hint: a harmless
    // fallback-candidate miss (the font that actually gets used renders fine), not anything wrong in the app - logged above, but never
    // a toast, so it stops looking like an application error.
    if (msg.contains(QLatin1String("DirectWrite: CreateFontFaceFromHDC")))
        return;

    // Logged, never published (see isProgress above).
    if (isProgress)
        return;

    forwarding = true;
    EventBus::instance().publish(isQml ? QStringLiteral("log.qml") : topic, QVariantMap{
        {QStringLiteral("level"), level},
        {QStringLiteral("title"), isQml ? QStringLiteral("QML") : QStringLiteral("Application")},
        {QStringLiteral("message"), msg},
    });
    forwarding = false;
}

// The Windows splash is a plain Win32 window on its own message-loop thread.
// That keeps its animation running while EngineBridge performs synchronous
// kernel boot on the Qt thread; the QML module is loaded only afterwards.
#ifdef Q_OS_WIN

namespace {
constexpr wchar_t kSplashClass[] = L"VGRPresenterBootSplash";
constexpr UINT_PTR kSplashTimer = 1;
constexpr int kSplashWidth = 678;
constexpr int kSplashHeight = 378;
constexpr qint64 kSplashMessageDurationMs = 3000;
std::atomic<int> gSplashProgress{0};
std::atomic<int> gSplashStage{0}; // 0 = engine boot, 1 = interface load, 2 = ready
std::atomic<qint64> gSplashStageStartMs{0};

qint64 splashClockMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

LRESULT CALLBACK splashWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_TIMER && wParam == kSplashTimer) {
        const int stage = gSplashStage.load(std::memory_order_relaxed);
        const qint64 elapsed = splashClockMs() - gSplashStageStartMs.load(std::memory_order_relaxed);
        const int progress = stage == 0 ? qMin(64, 4 + static_cast<int>(elapsed / 45))
                            : stage == 1 ? qMin(97, 65 + static_cast<int>(elapsed / 100)) : 100;
        gSplashProgress.store(progress, std::memory_order_relaxed);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC target = BeginPaint(hwnd, &ps);
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        const int clientWidth = bounds.right - bounds.left;
        const int clientHeight = bounds.bottom - bounds.top;
        HDC dc = CreateCompatibleDC(target);
        HBITMAP frame = CreateCompatibleBitmap(target, clientWidth, clientHeight);
        HGDIOBJ previousBitmap = SelectObject(dc, frame);
        SetMapMode(dc, MM_ANISOTROPIC);
        SetWindowExtEx(dc, kSplashWidth, kSplashHeight, nullptr);
        SetViewportExtEx(dc, clientWidth, clientHeight, nullptr);
        bounds = RECT{0, 0, kSplashWidth, kSplashHeight};
        HBRUSH background = CreateSolidBrush(RGB(17, 19, 27));
        FillRect(dc, &bounds, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);

        const wchar_t *fontFamily = L"Segoe UI"; // Theme.fontFamily
        HFONT bold = CreateFontW(52, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, fontFamily);
        HFONT regular = CreateFontW(52, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH, fontFamily);
        HGDIOBJ oldFont = SelectObject(dc, bold);
        SIZE vgrSize{};
        GetTextExtentPoint32W(dc, L"VGR", 3, &vgrSize);
        SelectObject(dc, regular);
        SIZE presenterSize{};
        GetTextExtentPoint32W(dc, L"Presenter", 9, &presenterSize);
        const int lockupWidth = 80 + 20 + vgrSize.cx + 3 + presenterSize.cx;
        const int tileLeft = (kSplashWidth - lockupWidth) / 2;
        const int logoX = tileLeft + 40;
        const int logoY = 164;
        HBRUSH tile = CreateSolidBrush(RGB(10, 11, 18));
        HPEN tileOutline = CreatePen(PS_SOLID, 1, RGB(54, 43, 91));
        HGDIOBJ oldBrush = SelectObject(dc, tile);
        HGDIOBJ oldPen = SelectObject(dc, tileOutline);
        RoundRect(dc, logoX - 40, logoY - 40, logoX + 40, logoY + 40, 22, 22);
        HBRUSH purple = CreateSolidBrush(RGB(124, 92, 231));
        HPEN noPen = CreatePen(PS_NULL, 0, RGB(124, 92, 231));
        SelectObject(dc, purple);
        SelectObject(dc, noPen);
        POINT hexagon[] = {{logoX, logoY - 21}, {logoX + 19, logoY - 10},
                           {logoX + 19, logoY + 11}, {logoX, logoY + 22},
                           {logoX - 19, logoY + 11}, {logoX - 19, logoY - 10}};
        Polygon(dc, hexagon, 6);
        HBRUSH white = CreateSolidBrush(RGB(242, 243, 247));
        SelectObject(dc, white);
        POINT play[] = {{logoX - 4, logoY - 9}, {logoX + 9, logoY}, {logoX - 4, logoY + 9}};
        Polygon(dc, play, 3);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(tile); DeleteObject(tileOutline); DeleteObject(purple); DeleteObject(noPen); DeleteObject(white);

        SelectObject(dc, bold);
        SetTextColor(dc, RGB(242, 243, 247));
        const int textX = tileLeft + 100;
        TextOutW(dc, textX, logoY - 32, L"VGR", 3);
        SelectObject(dc, regular);
        SetTextColor(dc, RGB(138, 143, 160));
        TextOutW(dc, textX + vgrSize.cx + 3, logoY - 32, L"Presenter", 9);
        HFONT statusFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
        SelectObject(dc, statusFont);
        SetTextColor(dc, RGB(160, 164, 176));
        RECT status{0, 328, kSplashWidth, 350};
        const int progress = gSplashProgress.load(std::memory_order_relaxed);
        static const wchar_t *const bootMessages[] = {
            L"Starting the presentation engine...", L"Initializing core services...",
            L"Preparing your workspace...", L"Checking connected devices..."
        };
        static const wchar_t *const interfaceMessages[] = {
            L"Building the main window...", L"Loading interface components...",
            L"Preparing your workspace...", L"Finishing startup..."
        };
        const int stage = gSplashStage.load(std::memory_order_relaxed);
        const qint64 elapsed = splashClockMs() - gSplashStageStartMs.load(std::memory_order_relaxed);
        const wchar_t *statusText = stage == 2 ? L"Ready"
            : stage == 1 ? interfaceMessages[(elapsed / kSplashMessageDurationMs) % std::size(interfaceMessages)]
                         : bootMessages[(elapsed / kSplashMessageDurationMs) % std::size(bootMessages)];
        DrawTextW(dc, statusText, -1, &status, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
        DeleteObject(bold); DeleteObject(regular); DeleteObject(statusFont);

        const int trackX = (kSplashWidth - 238) / 2;
        RECT track{trackX, 363, trackX + 238, 367};
        HBRUSH trackBrush = CreateSolidBrush(RGB(29, 30, 40));
        FillRect(dc, &track, trackBrush);
        DeleteObject(trackBrush);
        HRGN clip = CreateRectRgn(track.left, track.top, track.right, track.bottom);
        SelectClipRgn(dc, clip);
        RECT fill{trackX, 363, trackX + (238 * progress / 100), 367};
        HBRUSH pillBrush = CreateSolidBrush(RGB(124, 92, 231));
        FillRect(dc, &fill, pillBrush);
        DeleteObject(pillBrush);
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
        wchar_t percent[8]{};
        wsprintfW(percent, L"%d%%", progress);
        RECT percentRect{trackX + 245, 355, trackX + 295, 375};
        SetTextColor(dc, RGB(170, 151, 255));
        DrawTextW(dc, percent, -1, &percentRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        BitBlt(target, 0, 0, kSplashWidth, kSplashHeight, dc, 0, 0, SRCCOPY);
        SelectObject(dc, previousBitmap);
        DeleteObject(frame);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) { KillTimer(hwnd, kSplashTimer); PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
} // namespace
#endif

class BootSplashWindow
{
public:
    BootSplashWindow()
    {
#ifdef Q_OS_WIN
        gSplashProgress.store(4, std::memory_order_relaxed);
        gSplashStage.store(0, std::memory_order_relaxed);
        gSplashStageStartMs.store(splashClockMs(), std::memory_order_relaxed);
        worker_ = std::thread([this] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = splashWindowProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = kSplashClass;
            RegisterClassW(&wc);
            RECT area{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
            MONITORINFO monitorInfo{sizeof(MONITORINFO)};
            HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
            if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
                area = monitorInfo.rcWork;
            const double scale = qMin(1.0, qMin(double(area.right - area.left) / kSplashWidth,
                                                double(area.bottom - area.top) / kSplashHeight));
            const int windowWidth = qMax(1, qRound(kSplashWidth * scale));
            const int windowHeight = qMax(1, qRound(kSplashHeight * scale));
            const int x = area.left + (area.right - area.left - windowWidth) / 2;
            const int y = area.top + (area.bottom - area.top - windowHeight) / 2;
            HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kSplashClass,
                L"VGR Presenter", WS_POPUP, x, y, windowWidth, windowHeight,
                nullptr, nullptr, wc.hInstance, nullptr);
            if (hwnd) {
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                UpdateWindow(hwnd);
                SetTimer(hwnd, kSplashTimer, 30, nullptr);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                threadId_ = GetCurrentThreadId();
                ready_ = true;
            }
            readyChanged_.notify_one();
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
        });
        std::unique_lock<std::mutex> lock(mutex_);
        readyChanged_.wait(lock, [this] { return ready_; });
#endif
    }

    ~BootSplashWindow() { close(); }

    void close()
    {
#ifdef Q_OS_WIN
        if (worker_.joinable()) {
            DWORD threadId;
            { std::lock_guard<std::mutex> lock(mutex_); threadId = threadId_; }
            if (threadId) PostThreadMessageW(threadId, WM_QUIT, 0, 0);
            worker_.join();
        }
#endif
    }

    void engineReady()
    {
#ifdef Q_OS_WIN
        gSplashProgress.store(65, std::memory_order_relaxed);
        gSplashStageStartMs.store(splashClockMs(), std::memory_order_relaxed);
        gSplashStage.store(1, std::memory_order_relaxed);
#endif
    }

    void complete()
    {
#ifdef Q_OS_WIN
        gSplashProgress.store(100, std::memory_order_relaxed);
        gSplashStageStartMs.store(splashClockMs(), std::memory_order_relaxed);
        gSplashStage.store(2, std::memory_order_relaxed);
#endif
    }

private:
#ifdef Q_OS_WIN
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable readyChanged_;
    DWORD threadId_ = 0;
    bool ready_ = false;
#endif
};

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
    const QString sourceSerifResource = QStringLiteral(":/fonts/SourceSerif4-VariableFont_opsz,wght.ttf");
    const QString sourceSerifItalicResource = QStringLiteral(":/fonts/SourceSerif4-Italic-VariableFont_opsz,wght.ttf");
    const int sourceSerifFontId = QFontDatabase::addApplicationFont(
        sourceSerifResource);
    const int sourceSerifItalicFontId = QFontDatabase::addApplicationFont(sourceSerifItalicResource);
    Q_UNUSED(sourceSerifItalicFontId)
    if (sourceSerifFontId < 0)
        qWarning() << "Bundled Source Serif 4 font could not be loaded";
    app.setFont(QFont(QStringLiteral("Segoe UI")));
#ifdef Q_OS_WIN
    // Qt's font database serves QML. Add the same bundled faces to this
    // process's GDI font collection too, which the presentation renderer uses.
    QByteArray sourceSerifGdiData;
    QByteArray sourceSerifItalicGdiData;
    HANDLE sourceSerifGdiHandle = nullptr;
    HANDLE sourceSerifItalicGdiHandle = nullptr;
    DWORD sourceSerifFontsAdded = 0;
    QFile sourceSerifFile(sourceSerifResource);
    if (sourceSerifFile.open(QIODevice::ReadOnly)) {
        sourceSerifGdiData = sourceSerifFile.readAll();
        sourceSerifGdiHandle = AddFontMemResourceEx(sourceSerifGdiData.data(),
                                                    static_cast<DWORD>(sourceSerifGdiData.size()),
                                                    nullptr, &sourceSerifFontsAdded);
    }
    QFile sourceSerifItalicFile(sourceSerifItalicResource);
    if (sourceSerifItalicFile.open(QIODevice::ReadOnly)) {
        sourceSerifItalicGdiData = sourceSerifItalicFile.readAll();
        sourceSerifItalicGdiHandle = AddFontMemResourceEx(sourceSerifItalicGdiData.data(),
                                                          static_cast<DWORD>(sourceSerifItalicGdiData.size()),
                                                          nullptr, &sourceSerifFontsAdded);
    }
#endif
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

    // The native splash runs on its own Win32 message thread. Boot remains on
    // the Qt thread as EngineBridge requires, but its animation keeps moving.
    // Main.qml is not loaded until this synchronous boot has completely ended.
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
#ifdef Q_OS_WIN
    QObject::connect(&app, &QGuiApplication::aboutToQuit,
                     [sourceSerifGdiHandle, sourceSerifItalicGdiHandle] {
        if (sourceSerifGdiHandle)
            RemoveFontMemResourceEx(sourceSerifGdiHandle);
        if (sourceSerifItalicGdiHandle)
            RemoveFontMemResourceEx(sourceSerifItalicGdiHandle);
    });
#endif

    BootSplashWindow bootSplash;
    // SettingsService must exist before bootedChanged is emitted: it loads
    // persisted engine settings synchronously on that signal, and EngineBridge
    // also replays persisted plugin switches during its boot finalization.
    (void)SettingsService::instance();
    QElapsedTimer bootClock;
    bootClock.start();
    (void)EngineBridge::instance().boot();
    bootSplash.engineReady();
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("App"),
                        QStringLiteral("engine boot finished in %1 ms (native splash; interface loads after boot)")
                            .arg(bootClock.elapsed()));

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

    QElapsedTimer qmlLoadClock;
    qmlLoadClock.start();
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                        QStringLiteral("QML load begin: VGRPresenterUI.Main"));
    engine.loadFromModule("VGRPresenterUI", "Main");
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                        QStringLiteral("QML load returned after %1 ms; root objects=%2")
                            .arg(qmlLoadClock.elapsed())
                            .arg(engine.rootObjects().size()));

    // The window's own taskbar/alt-tab icon: a QQuickWindow does NOT reliably
    // adopt QGuiApplication::setWindowIcon() on Windows — the exe carried the
    // icon (RC_ICONS) while the taskbar still showed the generic placeholder
    // (this main window is frameless, so the taskbar button is its only
    // OS-drawn chrome). QWindow::setIcon() is what reaches WM_SETICON. There
    // is no Window.icon property in the QML API, so this has to happen here.
    auto *rootWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    if (rootWindow)
        rootWindow->setIcon(appIcon);

    // Keep the native splash over the first QML frame, then hand over. The
    // short event-loop beat lets the newly-created scene render underneath it.
    bootSplash.complete();
#ifdef Q_OS_WIN
    QEventLoop splashHandoff;
    QTimer::singleShot(350, &splashHandoff, &QEventLoop::quit);
    splashHandoff.exec();
#endif
    bootSplash.close();
    if (rootWindow) {
        rootWindow->show();
        rootWindow->raise();
        rootWindow->requestActivate();
    }
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                        QStringLiteral("main window shown; visible=%1")
                            .arg(rootWindow && rootWindow->isVisible()));

    // Temporary hang diagnostic: if the GUI thread stops processing events,
    // this timestamped line is the last confirmed event-loop progress in
    // engine.log. Kept at a low rate so a normal development run stays legible.
    QElapsedTimer uiTraceClock;
    uiTraceClock.start();
    QTimer uiHeartbeat;
    uiHeartbeat.setInterval(5000);
    QObject::connect(&uiHeartbeat, &QTimer::timeout, &app, [&] {
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                            QStringLiteral("UI event-loop heartbeat at %1 ms; visible=%2; active=%3")
                                .arg(uiTraceClock.elapsed())
                                .arg(rootWindow && rootWindow->isVisible())
                                .arg(rootWindow && rootWindow->isActive()));
    });
    uiHeartbeat.start();

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
    // The Settings-inset scenario is armed by its OWN env var on top of
    // VGR_SELFTEST, so running the app with only VGR_SELFTEST=1 (how the NDI
    // probes launch it) never opens Settings or quits early. Main.qml's
    // scenario gates on this boolean instead of on the driver alone.
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestInsets"),
        QVariant(qEnvironmentVariableIsSet("VGR_INSETS_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
#endif

    // A safety net for exceptions that escape the event loop (e.g. from a
    // future engine-bridge call) — logs and exits cleanly instead of letting
    // the OS's own unhandled-exception dialog appear. A true memory-access
    // crash still goes through CrashHandler.cpp's SEH/signal path, which
    // this can't catch.
    try {
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                            QStringLiteral("entering app event loop"));
        const int code = app.exec();
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                            QStringLiteral("app event loop returned with code %1").arg(code));
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
