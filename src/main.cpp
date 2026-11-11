#include <QLoggingCategory>
#include <algorithm>
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
#include <QCoreApplication>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QDir>
#include <QFileInfo>

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

#include "services/CallTrace.h"
#include "services/CrashHandler.h"
#include "services/FontService.h"
#include "services/GuardedApplication.h"
#include "services/EngineBridge.h"
#include "services/SearchService.h"
#include "services/MediaLibraryService.h"
#include "services/TheTableService.h"
#include "services/EngineWorker.h"
#include "services/LiveOutputService.h"
#include "services/TelemetryService.h"
#include "services/RecordingService.h"
#include "services/SettingsService.h"
#include "services/MediaThumbnailProvider.h"
#include "services/EventBus.h"

#include "models/AudioInputListModel.h"
#include "models/BusListModel.h"
#include "models/OutputListModel.h"
#include "models/VideoSourceListModel.h"

#include "core/logging/ChangeGate.hpp"
#include "core/logging/QtMessagePolicy.hpp"
#include "modules/rendering/RenderEngine.hpp"
#ifdef VGR_ENABLE_SELFTEST
#include "SelfTestDriver.h"
#endif

namespace {
// A Qt message's level in the engine log's own vocabulary, so a QML console.log, a qInfo()
// and an EngineBridge::write all read the same way in engine.log and in the console — one
// mapping, one formatter, one level gate instead of a path per origin.
QString LogLevelNameForQt(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return QStringLiteral("debug");
    case QtInfoMsg:     return QStringLiteral("info");
    case QtWarningMsg:  return QStringLiteral("warning");
    case QtCriticalMsg: return QStringLiteral("error");
    case QtFatalMsg:    return QStringLiteral("error");
    }
    return QStringLiteral("info");
}

void ForwardToEventBus(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    const QString level = LogLevelNameForQt(type);
    const bool raised = type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg;

    QString topic;
    switch (type) {
    case QtWarningMsg:  topic = QStringLiteral("log.warning");  break;
    case QtCriticalMsg: topic = QStringLiteral("log.critical"); break;
    case QtFatalMsg:    topic = QStringLiteral("log.fatal");    break;
    default: break;
    }
    // A warning raised WHILE a warning is being forwarded (a toast delegate
    // whose own binding errors, say) would otherwise feed itself forever.
    static thread_local bool forwarding = false;
    if (forwarding)
        return;

    if (raised) {
        struct Dedupe { std::mutex m; QString lastMsg; qint64 lastAt = 0; };
        static Dedupe *dedupe = new Dedupe;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        std::lock_guard<std::mutex> lock(dedupe->m);
        if (msg == dedupe->lastMsg && now - dedupe->lastAt < 3000)
            return;
        dedupe->lastMsg = msg;
        dedupe->lastAt = now;
    }

    const bool isQml = (context.category && qstrncmp(context.category, "qml", 3) == 0)
        || msg.startsWith(QLatin1String("qrc:/")) || msg.startsWith(QLatin1String("file:///"));

    const bool isProgress = context.category
        && qstrncmp(context.category, "vgr.", 4) == 0
        && QLatin1String(context.category).contains(QLatin1String(".progress"));

    // Qt's DirectWrite backend retries a font face on every layout pass for the legacy
    // bitmap fonts it cannot open (Fixedsys, MS Sans Serif, 8514oem). It falls back
    // correctly and the app can do nothing about it, so one line per distinct failure is
    // the record; the repeats are the platform narrating its own fallback, at the rate the
    // text is laid out.
    if (!isQml && msg.contains(QLatin1String("DirectWrite: CreateFontFaceFromHDC"))) {
        static bps::logging::ChangeGate fontFaceFailures;
        if (!fontFaceFailures.Changed("DirectWrite", msg.toStdString()))
            return;
    }

    QString logged = msg;
    if (raised) {
        QString where = calltrace::current();
        if (where.isEmpty())
            where = calltrace::guiChain();
        if (!where.isEmpty())
            logged += QStringLiteral(" [in flight: ") + where + QLatin1Char(']');
    }
    if (!raised && !bps::logging::QtMessageBelongsToApplication(context.category))
        return;
    EngineBridge::write(level, isQml ? QStringLiteral("QML") : QStringLiteral("Qt"), logged);

    if (!raised)
        return;
    if (!bps::logging::QtMessageIsUserFacing(isQml, context.category, msg.toStdString()))
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

const wchar_t *SystemUiFace()
{
    static wchar_t face[LF_FACESIZE] = {};
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0) &&
            ncm.lfMessageFont.lfFaceName[0] != L'\0') {
            lstrcpynW(face, ncm.lfMessageFont.lfFaceName, LF_FACESIZE);
        }
    }
    return face[0] ? face : nullptr;
}

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

        const wchar_t *fontFamily = SystemUiFace();
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
                                       DEFAULT_PITCH, SystemUiFace());
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

static QElapsedTimer gExitClock;

void timeExitStep(const char *name, const std::function<void()> &fn, bool engineGone = false)
{
    QElapsedTimer clock;
    clock.start();
    if (engineGone) {
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Exit"),
                            QStringLiteral("aboutToQuit: %1 starting (this step tears the engine down)")
                                .arg(QString::fromLatin1(name)));
        fn();
        return;
    }
    fn();
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Exit"),
                        QStringLiteral("aboutToQuit: %1 took %2 ms")
                            .arg(QString::fromLatin1(name)).arg(clock.elapsed()));
}

void copyTree(const QString &from, const QString &to)
{
    QDir().mkpath(to);
    const QDir source(from);
    for (const QFileInfo &entry : source.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString target = to + QLatin1Char('/') + entry.fileName();
        if (entry.isDir())
            copyTree(entry.absoluteFilePath(), target);
        else if (!QFileInfo::exists(target))
            QFile::copy(entry.absoluteFilePath(), target);
    }
}

// FIRST RUN OF AN INSTALLED LINUX / macOS PACKAGE. On Windows the Inno installer lays the shipped seed
// data (design catalogs, the operator's configuration, the Shows library) onto a fresh machine, only
// where nothing exists. A Linux AppImage or a macOS .app has no installer step, so the package carries
// the same data next to the executable and the app lays it down itself, once — with the same rule:
// copyTree never overwrites a file that is already there, and nothing happens on a profile that already
// has a settings.json. Needs the application and organisation names (AppDataLocation depends on them).
void seedFirstRun()
{
#ifndef Q_OS_WIN
    if (qEnvironmentVariableIsSet("VGR_PROFILE_DIR") || qEnvironmentVariableIsSet("VGR_SANDBOX"))
        return;                                // a dev/test profile is its own business
    const QString appDir = QCoreApplication::applicationDirPath();
    // AppImage: <AppDir>/usr/bin/<exe> → ../share/vgrpresenter/seed-data;  macOS: Contents/MacOS → ../Resources/seed-data
    const QString candidates[] = {
        QDir::cleanPath(appDir + QStringLiteral("/../share/vgrpresenter/seed-data")),
        QDir::cleanPath(appDir + QStringLiteral("/../Resources/seed-data")),
    };
    QString seed;
    for (const QString &c : candidates)
        if (QFileInfo(c).isDir()) { seed = c; break; }
    if (seed.isEmpty())
        return;                                // not an installed package (a dev build, a plain binary)
    // The same three homes the Windows installer seeds, in this OS's own places: the per-user data
    // folder (…/bps — IPaths::UserDataDir), the application data folder (…/VGR/VGRPresenter) and the
    // Shows library under Documents.
    const QString local = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                          + QStringLiteral("/bps");
    if (QFileInfo::exists(local + QStringLiteral("/settings.json")))
        return;                                // not a fresh profile: never touch what a user already has
    copyTree(seed + QStringLiteral("/local"), local);
    copyTree(seed + QStringLiteral("/roaming"),
             QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    copyTree(seed + QStringLiteral("/shows"),
             QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                 + QStringLiteral("/VGR Presenter/Shows"));
#endif
}

void selectProfile()
{
#ifdef Q_OS_WIN
    wchar_t exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return;
    const QString exeDir = QFileInfo(QString::fromWCharArray(exePath)).absolutePath();
    if (qEnvironmentVariableIsSet("VGR_PROFILE_DIR")) {
        if (qEnvironmentVariableIsEmpty("VGR_PROFILE_DIR"))
            qunsetenv("VGR_PROFILE_DIR");     // asked for the real profile
        return;
    }
    if (QFileInfo::exists(exeDir + QStringLiteral("/assets/settings.shipped.json")))
        return;                               // an installed app: its own profile
    const QString root = QDir::cleanPath(exeDir + QStringLiteral("/../dev-profile"));
    qputenv("VGR_PROFILE_DIR", QDir::toNativeSeparators(root).toLocal8Bit());
    if (!QFileInfo::exists(root + QStringLiteral("/local/settings.json"))) {
        const QString seed = QDir::cleanPath(exeDir + QStringLiteral("/../installer/seed-data"));
        if (QFileInfo(seed).isDir()) {
            copyTree(seed + QStringLiteral("/local"), root + QStringLiteral("/local"));
            copyTree(seed + QStringLiteral("/roaming"), root + QStringLiteral("/roaming"));
        }
    }
#endif
}
} // namespace

#ifdef Q_OS_WIN
// Windows laptops with two GPUs: these two exported symbols are how the NVIDIA and AMD drivers are asked to
// run this process on the discrete GPU. `__declspec` is a Windows-compiler keyword — unguarded it stopped
// the Linux and macOS builds — and no other OS has anything to ask for.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

int main(int argc, char *argv[])
{
    selectProfile();
    if (qEnvironmentVariableIsSet("VGR_SANDBOX"))
        QStandardPaths::setTestModeEnabled(true);

    qInstallMessageHandler(ForwardToEventBus);
    // Windows still lists its legacy raster fonts (System, Fixedsys, Terminal ...) and DirectWrite refuses every one
    // of them: ~11 harmless "CreateFontFaceFromHDC() failed" warnings on EVERY start, each of which was also shown
    // to the operator as a toast. None of them is a font the app can use. (QT_LOGGING_RULES still wins.)
    if (!qEnvironmentVariableIsSet("QT_LOGGING_RULES"))
        QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.fonts.warning=false"));

    GuardedApplication app(argc, argv);
    calltrace::markGuiThread();   // before anything can publish from another thread
    app.setApplicationName(QStringLiteral("VGRPresenter"));
    app.setOrganizationName(QStringLiteral("VGR"));
    seedFirstRun();   // an installed Linux/macOS package's shipped seed data, once, before anything reads a profile
    const QString sourceSerifResource = QStringLiteral(":/fonts/SourceSerif4-VariableFont_opsz,wght.ttf");
    const QString sourceSerifItalicResource = QStringLiteral(":/fonts/SourceSerif4-Italic-VariableFont_opsz,wght.ttf");
    const int sourceSerifFontId = QFontDatabase::addApplicationFont(
        sourceSerifResource);
    const int sourceSerifItalicFontId = QFontDatabase::addApplicationFont(sourceSerifItalicResource);
    Q_UNUSED(sourceSerifItalicFontId)
    if (sourceSerifFontId < 0)
        qWarning() << "Bundled Source Serif 4 font could not be loaded";
    {
        // The interface font is THIS OS's own face, chosen in code (the engine's per-OS list), at the size Qt's
        // system font has — not whatever name another OS's theme happens to hand back.
        QFont uiFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
        const QString family = FontService::instance().defaultFamily();
        if (!family.isEmpty() && family != QLatin1String("builtin"))
            uiFont.setFamily(family);
        app.setFont(uiFont);
    }
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
    const QIcon appIcon(QStringLiteral(":/app/app-icon.ico"));
    if (appIcon.isNull())
        qWarning() << "App icon failed to load from ':/app/app-icon.ico' — the taskbar will show the default icon";
    app.setWindowIcon(appIcon);

    // Needs QStandardPaths, which needs the QGuiApplication constructed above.
    InstallCrashHandler();

    QObject::connect(&app, &QGuiApplication::aboutToQuit, [] {
        QElapsedTimer stepsClock;
        stepsClock.start();
        // A Bible import may still be running on its own thread; let it finish before
        // the engine's systems are torn down under it.
        timeExitStep("SearchService.shutdown", [] { SearchService::instance().shutdown(); });
        timeExitStep("MediaLibraryService.shutdown", [] { MediaLibraryService::instance().shutdown(); });   // folder scans in progress
        timeExitStep("TheTableService.shutdownIndexing", [] { TheTableService::instance().shutdownIndexing(); });
        timeExitStep("LiveOutputService.shutdownMedia", [] { LiveOutputService::instance().shutdownMedia(); });
        timeExitStep("LiveOutputService.shutdownNdiWorker", [] { LiveOutputService::instance().shutdownNdiWorker(); });
        timeExitStep("EngineWorker.shutdown", [] { vgr::EngineWorker::instance().shutdown(); });
        timeExitStep("TelemetryService.shutdownSampler", [] { TelemetryService::shutdownSampler(); });
        // The hang watchdog's thread, so a late stall report can never touch a
        // service that is already being torn down.
        timeExitStep("calltrace.shutdown", [] { calltrace::shutdown(); });
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Exit"),
                            QStringLiteral("aboutToQuit: %1 ms across the steps above; the engine teardown starts now")
                                .arg(stepsClock.elapsed()));
        timeExitStep("EngineBridge.shutdown", [] { EngineBridge::instance().shutdown(); }, /*engineGone=*/true);
    });
#ifdef Q_OS_WIN
    QObject::connect(&app, &QGuiApplication::aboutToQuit,
                     [sourceSerifGdiHandle, sourceSerifItalicGdiHandle] {
        timeExitStep("RemoveFontMemResourceEx", [sourceSerifGdiHandle, sourceSerifItalicGdiHandle] {
            if (sourceSerifGdiHandle)
                RemoveFontMemResourceEx(sourceSerifGdiHandle);
            if (sourceSerifItalicGdiHandle)
                RemoveFontMemResourceEx(sourceSerifItalicGdiHandle);
        });
    });
#endif

    BootSplashWindow bootSplash;
    (void)SettingsService::instance();
    QElapsedTimer bootClock;
    bootClock.start();
    (void)EngineBridge::instance().boot();
    bootSplash.engineReady();
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("App"),
                        QStringLiteral("engine boot finished in %1 ms (native splash; interface loads after boot)")
                            .arg(bootClock.elapsed()));

    QQuickWindow::setDefaultAlphaBuffer(true);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, [] {
            qWarning() << "Failed to create the QML application window.";
            QCoreApplication::exit(-1);
        }, Qt::QueuedConnection);

    const QString crashSummary = ConsumePendingCrashSummary();
    if (!crashSummary.isEmpty())
        EngineBridge::write(QStringLiteral("error"), QStringLiteral("CrashRecovery"),
                            QStringLiteral("the previous run ended in a crash — ") + crashSummary);
    engine.rootContext()->setContextProperty(QStringLiteral("pendingCrashSummary"), crashSummary);

    VerifyForensicsTooling();

    const QString alphaInspectValue =
        qEnvironmentVariable("VGR_ALPHA_INSPECT").trimmed().toLower();
    const bool alphaInspect = alphaInspectValue == QLatin1String("1")
        || alphaInspectValue == QLatin1String("true") || alphaInspectValue == QLatin1String("yes")
        || alphaInspectValue == QLatin1String("on");
    engine.rootContext()->setContextProperty(QStringLiteral("alphaInspectEnabled"), QVariant(alphaInspect));

    if (alphaInspect) {
        bps::rendering::RenderEngine::Instance().SetAlphaInspect(true);
        // "warning": EngineBridge::write() maps an unknown level down to Info,
        // so "warn" here would have logged a degraded-output notice as INFO.
        EngineBridge::write(QStringLiteral("warning"), QStringLiteral("Render"),
                            QStringLiteral("alpha inspection ON — transparent pixels are drawn as a checkerboard on every output "
                                           "(NDI/HDMI/output window); the feed is NOT keyable while this is on"));
    }

    // Video preview frames for the Media tab (image://mediathumb/<path>).
    engine.addImageProvider(QStringLiteral("mediathumb"), new MediaThumbnailProvider);

    engine.addImageProvider(QStringLiteral("livepreview"), new LivePreviewProvider);

    engine.addImageProvider(QStringLiteral("videopreview"),
                            new EngineBridge::VideoPreviewProvider(&EngineBridge::instance()));

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

    // ---- ONE OBJECT PER ROUTING MODEL -------------------------------------
    auto *buses = engine.singletonInstance<BusListModel *>(
        QStringLiteral("VGRPresenterUI"), QStringLiteral("BusListModel"));
    if (buses) {
        buses->adoptRoutingModels(
            engine.singletonInstance<AudioInputListModel *>(
                QStringLiteral("VGRPresenterUI"), QStringLiteral("AudioInputListModel")),
            engine.singletonInstance<VideoSourceListModel *>(
                QStringLiteral("VGRPresenterUI"), QStringLiteral("VideoSourceListModel")));
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                            QStringLiteral("routing models adopted from the QML engine "
                                           "(one object per model)"));
    } else {
        EngineBridge::write(QStringLiteral("warning"), QStringLiteral("Startup"),   // "warn" would log as INFO (see EngineBridge::write)
                            QStringLiteral("routing models: the QML singletons could not be "
                                           "resolved, so the board and the engine face may "
                                           "hold different instances of them"));
    }

    // ---- THE OUTPUT ROSTER, same one-object rule ---------------------------
    auto *outputs = engine.singletonInstance<OutputListModel *>(
        QStringLiteral("VGRPresenterUI"), QStringLiteral("OutputListModel"));
    OutputListModel::adoptEngineInstance(outputs);
    if (!outputs) {
        EngineBridge::write(QStringLiteral("warning"), QStringLiteral("Startup"),
                            QStringLiteral("output roster: the QML singleton could not be "
                                           "resolved; an output added while live may not "
                                           "reach the outputs that C++ feeds"));
    }

    auto *rootWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    if (rootWindow)
        rootWindow->setIcon(appIcon);

    // SHOW FIRST, THEN HAND OVER. This used to hold the window back for a fixed
    // 350 ms beat (a nested event loop under the splash) "so the scene could
    // render underneath it" — but the beat was pure clock time: the frame it
    // waited for only starts once show() runs, and show() is what takes the
    // time. Now the window is shown immediately and the splash closes on the
    // first frame it actually paints (or a short cap, so a machine that cannot
    // paint one is not left staring at the splash).
    bootSplash.complete();
    QElapsedTimer handoverClock;
    handoverClock.start();
    bool splashClosed = false;
    auto closeSplash = [&splashClosed, &handoverClock, &bootSplash](const char *why) {
        if (splashClosed)
            return;
        splashClosed = true;
        bootSplash.close();
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                            QStringLiteral("splash handed over after %1 ms (%2)")
                                .arg(handoverClock.elapsed())
                                .arg(QString::fromLatin1(why)));
    };
    if (rootWindow) {
        QObject::connect(rootWindow, &QQuickWindow::frameSwapped, &app,
                         [closeSplash] { closeSplash("first frame published"); },
                         Qt::QueuedConnection);
        rootWindow->show();
        rootWindow->raise();
        rootWindow->requestActivate();
        // A "never paints" guard, NOT the normal path: normally frameSwapped
        // closes the splash. 600 ms is above a slow first frame on the GPU, so
        // the cap can only win when the scene really is not painting — it must
        // never expose a window that has nothing on it yet.
        QTimer::singleShot(600, &app, [closeSplash] { closeSplash("cap reached"); });
    } else {
        closeSplash("no window");
    }
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                        QStringLiteral("main window shown; visible=%1")
                            .arg(rootWindow && rootWindow->isVisible()));

    // ---- TEMPORARY FRAME PROBE (deleted once the layout work is measured) ----
    struct FrameProbe {
        std::atomic<qint64> lastNs{0};
        std::atomic<qint64> frames{0};
        std::atomic<qint64> worstNs{0};
        std::atomic<qint64> over33{0};
        std::atomic<qint64> over50{0};
        std::atomic<qint64> over100{0};
        void sample()
        {
            const qint64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
            const qint64 prev = lastNs.exchange(now);
            frames.fetch_add(1);
            if (prev == 0)
                return;
            const qint64 gap = now - prev;
            if (gap > worstNs.load())
                worstNs.store(gap);
            if (gap > 33'000'000)
                over33.fetch_add(1);
            if (gap > 50'000'000)
                over50.fetch_add(1);
            if (gap > 100'000'000)
                over100.fetch_add(1);
        }
    };
    FrameProbe frameProbe;
    if (qEnvironmentVariableIsSet("VGR_FRAMEPROBE") && rootWindow != nullptr) {
        QObject::connect(rootWindow, &QQuickWindow::afterRendering, &app,
                         [&frameProbe] { frameProbe.sample(); }, Qt::DirectConnection);
        QElapsedTimer frameProbeTimer;
        frameProbeTimer.start();
        auto *frameProbeReport = new QTimer(&app);
        frameProbeReport->setInterval(1000);
        QObject::connect(frameProbeReport, &QTimer::timeout, &app, [&frameProbe, &frameProbeTimer] {
            const qint64 frames = frameProbe.frames.exchange(0);
            if (frames == 0)
                return;
            const double worst = double(frameProbe.worstNs.exchange(0)) / 1'000'000.0;
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("FrameProbe"),
                                QStringLiteral("frames=%1 avg=%2ms worst=%3ms >33ms=%4 >50ms=%5 >100ms=%6 timerLate=%7ms")
                                    .arg(frames)
                                    .arg(QString::number(1000.0 / double(frames), 'f', 1))
                                    .arg(QString::number(worst, 'f', 1))
                                    .arg(frameProbe.over33.exchange(0))
                                    .arg(frameProbe.over50.exchange(0))
                                    .arg(frameProbe.over100.exchange(0))
                                    .arg(frameProbeTimer.restart()));
        });
        frameProbeReport->start();
    }

    QElapsedTimer uiTraceClock;
    uiTraceClock.start();
    QTimer uiHeartbeat;
    uiHeartbeat.setInterval(5000);
    QObject::connect(&uiHeartbeat, &QTimer::timeout, &app, [&] {
        // A startup probe, not a runtime one: boot is over long before a minute, and a
        // heartbeat that never stops is 720 lines an hour in engine.log that nobody reads.
        // After that, liveness is the hang watchdog's job (calltrace::heartbeat below).
        if (uiTraceClock.elapsed() > 60'000) {
            uiHeartbeat.stop();
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                                QStringLiteral("UI event-loop heartbeat: startup trace complete after %1 ms; "
                                               "visible=%2")
                                    .arg(uiTraceClock.elapsed())
                                    .arg(rootWindow && rootWindow->isVisible()));
            return;
        }
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                            QStringLiteral("UI event-loop heartbeat at %1 ms; visible=%2; active=%3")
                                .arg(uiTraceClock.elapsed())
                                .arg(rootWindow && rootWindow->isVisible())
                                .arg(rootWindow && rootWindow->isActive()));
    });
    uiHeartbeat.start();

    QTimer callTraceHeartbeat;
    // THE STALL SAMPLER IS ALWAYS ON (every OS): any time the interface thread goes silent for 100 ms the log
    // gets stack samples of where it was and a line with how long it lasted ("GuiStall:"). BPS_GUI_STALL_MS
    // changes the threshold; 0 turns it off. The tick has to be fine enough to see a stall of that size.
    const int stallHuntMs = qEnvironmentVariableIsSet("BPS_GUI_STALL_MS")
                                ? qEnvironmentVariableIntValue("BPS_GUI_STALL_MS") : 100;
    callTraceHeartbeat.setInterval(stallHuntMs > 0 ? std::clamp(stallHuntMs / 4, 5, 250) : 250);
    QObject::connect(&callTraceHeartbeat, &QTimer::timeout, &app, [] { calltrace::heartbeat(); });
    callTraceHeartbeat.start();
    calltrace::heartbeat();   // arm it now: the watchdog stays quiet until this is set
    calltrace::installHangWatchdog(5);
    if (stallHuntMs > 0)
        StartGuiStallSampler(stallHuntMs);
    if (qEnvironmentVariableIsSet("BPS_LIVE_PROFILE"))
        StartLoopProfiler();
    // Env-gated (VGR_CALLTRACE_SELFTEST=1): proves the breadcrumb wiring on a
    // real run — an annotated warning and a reported exception in engine.log.
    calltrace::runEnvSelfTest();

    LiveOutputService::runEnvSelfTest();

#ifdef VGR_ENABLE_SELFTEST
    SelfTestDriver selfTestDriver;
    if (qEnvironmentVariableIsSet("VGR_SELFTEST") && !engine.rootObjects().isEmpty()) {
        selfTestDriver.setWindow(qobject_cast<QQuickWindow *>(engine.rootObjects().first()));
        engine.rootContext()->setContextProperty(QStringLiteral("SelfTest"), &selfTestDriver);
    }
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestInsets"),
        QVariant(qEnvironmentVariableIsSet("VGR_INSETS_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestLineStep"),
        QVariant(qEnvironmentVariableIsSet("VGR_LINESTEP_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestModalFit"),
        QVariant(qEnvironmentVariableIsSet("VGR_MODALFIT_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    // The split-drag scenario: it drags a sidebar divider and quits, so it gets its
    // own env var for the same reason as the two above.
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestSplit"),
        QVariant(qEnvironmentVariableIsSet("VGR_SPLIT_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    // The "open a search result" scenario: it drives the app-wide search, picks a
    // sermon result and quits, so it gets its own env var like the three above.
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestOpen"),
        QVariant(qEnvironmentVariableIsSet("VGR_OPEN_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    // The exact-word lock scenario: it drives the app-wide search, clicks the
    // padlock and quits, so it gets its own env var like the four above.
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestExact"),
        QVariant(qEnvironmentVariableIsSet("VGR_EXACT_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestOutputsLayout"),
        QVariant(qEnvironmentVariableIsSet("VGR_OUTS_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestOutputsLive"),
        QVariant(qEnvironmentVariableIsSet("VGR_OUTSLIVE_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestTableWalk"),
        QVariant(qEnvironmentVariableIsSet("VGR_TABLEWALK_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestTableWalkQuery"),
        qEnvironmentVariableIsSet("VGR_TABLEWALK_QUERY")
            ? qEnvironmentVariable("VGR_TABLEWALK_QUERY")
            : QStringLiteral("47-1100X"));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestTileFacts"),
        QVariant(qEnvironmentVariableIsSet("VGR_TILEFACTS_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestRank"),
        QVariant(qEnvironmentVariableIsSet("VGR_RANK_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestPaneHighlight"),
        QVariant(qEnvironmentVariableIsSet("VGR_PANEHL_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestShowIo"),
        QVariant(qEnvironmentVariableIsSet("VGR_SHOWIO_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestQuit"),
        QVariant(qEnvironmentVariableIsSet("VGR_QUIT_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestLiveLat"),
        QVariant(qEnvironmentVariableIsSet("VGR_LIVELAT_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    const QString liveLatLadder = qEnvironmentVariable("VGR_LIVELAT_LADDER");
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestLiveLatLadder"),
        QVariant(liveLatLadder.isEmpty() ? QStringLiteral("2:2:1,8:3:1,20:5:1,20:5:4")
                                         : liveLatLadder));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestLiveLatDriveMs"),
        QVariant(qEnvironmentVariable("VGR_LIVELAT_DRIVE_MS", "150")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestLiveLatWindowMs"),
        QVariant(qEnvironmentVariable("VGR_LIVELAT_WINDOW_MS", "8000")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestScripture"),
        QVariant(qEnvironmentVariableIsSet("VGR_SCRIPTURE_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestPickLat"),
        QVariant(qEnvironmentVariableIsSet("VGR_PICKLAT_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestPaneRows"),
        QVariant(qEnvironmentVariableIsSet("VGR_PANEROWS_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestSeed"),
        QVariant(qEnvironmentVariableIsSet("VGR_SEED_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestCanvasCursor"),
        QVariant(qEnvironmentVariableIsSet("VGR_CANVAS_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestScopeLock"),
        QVariant(qEnvironmentVariableIsSet("VGR_SCOPELOCK_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestMark"),
        QVariant(qEnvironmentVariableIsSet("VGR_MARK_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestPreSplit"),
        QVariant(qEnvironmentVariableIsSet("VGR_PRESPLIT_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestLayout"),
        QVariant(qEnvironmentVariableIsSet("VGR_LAYOUT_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestOutputSave"),
        QVariant(qEnvironmentVariableIsSet("VGR_OUTSAVE_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    // The style-background update scenario: it restyles a style with and
    // without a background image and quits, so it owns an env var like the rest.
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestStyleBg"),
        QVariant(qEnvironmentVariableIsSet("VGR_STYLEBG_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    // VGR_QUIETTIME_TEST: the QML side photographs the MONITOR WALL after the C++ scenario has
    // staged the Quiet Time pick and pressed GO LIVE — the app's own tile pixels, grabbed with the
    // same SelfTest.grab the tablewalk scenario uses.
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestQuietTime"),
        QVariant(qEnvironmentVariableIsSet("VGR_QUIETTIME_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestForensics"),
        QVariant(qEnvironmentVariableIsSet("VGR_FORENSICS_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestNdiLate"),
        QVariant(qEnvironmentVariableIsSet("VGR_NDILATE_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestBlankLive"),
        QVariant(qEnvironmentVariableIsSet("VGR_BLANKLIVE_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestInputRate"),
        QVariant(qEnvironmentVariableIsSet("VGR_INPUTRATE_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestVideoPerf"),
        QVariant(qEnvironmentVariableIsSet("VGR_VIDEOPERF_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    // The video RATE test: the integrated camera, then a video file (VGR_VIDEOFPS_FILE), each measured
    // against its own source rate, optionally while the machine is loaded (the runner script does that).
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestVideoFps"),
        QVariant(qEnvironmentVariableIsSet("VGR_VIDEOFPS_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestCardPreview"),
        QVariant(qEnvironmentVariableIsSet("VGR_CARDPREVIEW_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestVideoGpu"),
        QVariant(qEnvironmentVariableIsSet("VGR_VIDEOFPS_GPU")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestVideoFpsFile"),
        QVariant(qEnvironmentVariable("VGR_VIDEOFPS_FILE")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestTileFactsMedia"),
        QVariant(qEnvironmentVariable("VGR_TILEFACTS_MEDIA")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestBlackout"),
        QVariant(qEnvironmentVariableIsSet("VGR_BLACKOUT_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestCursorScale"),
        QVariant(qEnvironmentVariableIsSet("VGR_CURSORSCALE_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestUiWarn"),
        QVariant(qEnvironmentVariableIsSet("VGR_UIWARN_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestPill"),
        QVariant(qEnvironmentVariableIsSet("VGR_PILL_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestNewBus"),
        QVariant(qEnvironmentVariableIsSet("VGR_NEWBUS_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestGpu"),
        QVariant(qEnvironmentVariableIsSet("VGR_GPU_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestBusLight"),
        QVariant(qEnvironmentVariableIsSet("VGR_BUSLIGHT_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestMediaMeter"),
        QVariant(qEnvironmentVariableIsSet("VGR_MEDIAMETER_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestMediaMeterFile"),
        QVariant(qEnvironmentVariable("VGR_MEDIAMETER_FILE")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestHeader"),
        QVariant(qEnvironmentVariableIsSet("VGR_HEADER_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestMetaPalette"),
        QVariant(qEnvironmentVariableIsSet("VGR_METAPAL_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestItemColor"),
        QVariant(qEnvironmentVariableIsSet("VGR_ITEMCOLOR_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestTableFolder"),
        QVariant(qEnvironmentVariableIsSet("VGR_TABLEFOLDER_TEST")
                 && qEnvironmentVariableIsSet("VGR_SELFTEST")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("SelfTestTableFolderDir"),
        QVariant(qEnvironmentVariable("VGR_TABLEFOLDER_DIR")));
#endif

    try {
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                            QStringLiteral("entering app event loop"));
        const int code = app.exec();
        gExitClock.start();
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                            QStringLiteral("app event loop returned with code %1").arg(code));
        timeExitStep("RecordingService.shutdown", [] { RecordingService::instance().shutdown(); });
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Exit"),
                            QStringLiteral("main: leaving at +%1 ms — the stack teardown (QML engine, windows, Qt) is next")
                                .arg(gExitClock.elapsed()));
        qAddPostRoutine([] {
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("Exit"),
                                QStringLiteral("QCoreApplication teardown reached at +%1 ms — the QML engine and the windows are gone; Qt's own statics are next")
                                    .arg(gExitClock.elapsed()));
        });
        return code;
    } catch (const std::exception &e) {
        qCritical() << "Unhandled exception:" << e.what();
        // Names the call in flight in engine.log — an exception unwinds and pops
        // every breadcrumb, so the reporter reads the sticky last-entered slot.
        calltrace::reportException(e.what(), "escaped the application event loop");
        RecordingService::instance().shutdown();
        return 1;
    } catch (...) {
        qCritical() << "Unhandled unknown exception.";
        calltrace::reportException("non-std exception", "escaped the application event loop");
        RecordingService::instance().shutdown();
        return 1;
    }
}