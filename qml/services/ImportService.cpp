#include "services/ImportService.h"
#include "services/DataRoot.h"

#include "modules/import/ImportEngine.hpp"
#include "modules/presentation/PresentationDocument.hpp"
#include "services/DesignLibraryService.h"
#include "platform/PlatformAccessor.hpp"
#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/MediaLibraryService.h"
#include "services/SearchService.h"
#include "services/ShowConverter.h"
#include "services/ShowService.h"
#include "services/SlideBuilder.h"

#include <QClipboard>
#include <QDateTime>
#include <QMimeData>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QFileInfo>
#include <QThread>
#include <QJSEngine>
#include <QMetaObject>
#include <QQmlEngine>
#include <QThreadPool>
#include <QUrl>

namespace bi = bps::import;

namespace {

QString qstr(const std::string &s) { return QString::fromStdString(s); }

// The template Quick Lyrics' fresh-typed slides look like — the same
// overridable "song" category pick Scripture/Table already use
// (SlideBuilder::templateId reads the "song.template" setting, falling back
// to this plain default when unset or the chosen template was deleted).
constexpr const char *kSongTemplate = "tpl-default";
const SlideBuilder::Profile kSongProfile{ "song", "song.template", kSongTemplate };

// A path from the file dialog: a file:// url or a plain path.
QString localPath(const QString &file)
{
    const QUrl url(file);
    return url.isLocalFile() ? url.toLocalFile() : file;
}

// A folder name for the imported media: the source folder, made safe for a path.
QString safeFolderName(QString name)
{
    static const QString invalid = QStringLiteral("/\\:*?\"<>|");
    for (QChar &c : name)
        if (invalid.contains(c) || c.unicode() < 0x20)
            c = QLatin1Char('-');
    while (!name.isEmpty() && (name.back() == QLatin1Char(' ') || name.back() == QLatin1Char('.')))
        name.chop(1);
    return name;
}

void pointImportedMediaAtCopy(bi::ImportedShow &show, const QString &source, const QString &target)
{
    const QByteArray sourceBytes = source.toUtf8();
    const QString targetPath = QDir::toNativeSeparators(target);
    for (bi::ImportedSection &section : show.sections) {
        for (bi::ImportedSlide &slide : section.slides) {
            for (bps::presentation::ContentBlock &block : slide.blocks) {
                QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(block.metaJson));
                if (!doc.isObject()) continue;
                QJsonObject meta = doc.object();
                if (meta.value(QStringLiteral("src")).toString().toUtf8() != sourceBytes)
                    continue;
                meta.insert(QStringLiteral("src"), targetPath);
                block.metaJson = QJsonDocument(meta).toJson(QJsonDocument::Compact).toStdString();
            }
        }
    }
}

QVariantMap failure(const QString &why)
{
    return { { QStringLiteral("ok"), false }, { QStringLiteral("error"), why }, { QStringLiteral("shows"), 0 }, { QStringLiteral("bibles"), 0 },
             { QStringLiteral("files"), 0 }, { QStringLiteral("warnings"), QStringList() }, { QStringLiteral("firstShow"), QString() } };
}

// Saves one imported show into the shows library as its own .vgr; returns the path ("" when it could not be saved).
// `libraryFolder`: the library CATEGORY sub-folder to save into ("" = the
// show's own category, the historical behavior). A FreeShow library import
// passes "FreeShow" so the whole batch lands in one manageable folder instead
// of scattering into the root.
// `applyTemplate`: Quick Lyrics (fresh-typed content, nothing to lose) gets
// the configured song template's layout; a real FILE import (ChordPro/
// OpenSong/OpenLP/ProPresenter/EasyWorship) does NOT — forcing our own
// template's fixed text-box layout onto an already-arranged imported show
// would overwrite whatever structure it came in with. Leaving
// ShowBuildOptions.templateBlocks empty falls back to ShowFromImported's own
// neutral single-textbox-per-slide layout instead.
QString saveImportedShow(const bi::ImportedShow &imported, bool applyTemplate, const QString &libraryFolder, QString *why)
{
    bi::ShowBuildOptions options;
    if (applyTemplate) {
        const QVariantMap design = TemplateLibraryService::instance().design(SlideBuilder::templateId(kSongProfile));
        for (const QVariant &b : design.value(QStringLiteral("blocks")).toList())
            options.templateBlocks.push_back(ShowConverter::blockFromVariant(b.toMap()));
        if (const QString bg = design.value(QStringLiteral("background")).toString(); !bg.isEmpty())
            options.background = bg.toStdString();
    }

    bps::presentation::Presentation show = bi::ShowFromImported(imported, options);
    const QString folder = !libraryFolder.isEmpty() ? libraryFolder : qstr(imported.category);
    const QString path = ShowService::instance().newLibraryShowPath(folder, qstr(show.name));
    if (path.isEmpty()) {
        if (why) *why = QObject::tr("the shows library is not ready");
        return {};
    }
    bps::presentation::PresentationDocument doc;
    doc.New(show.name);
    doc.Replace(std::move(show));
    if (auto saved = doc.Save(path.toStdString()); !saved.ok()) {
        if (why) *why = qstr(saved.error().message);
        return {};
    }
    return path;
}

// Saves what the engine imported and builds the answer + the toast.
QVariantMap finish(const bi::ImportResult &result, const QString &formatName, bool applyTemplate, const QString &libraryFolder = QString())
{
    QStringList warnings;
    for (const std::string &w : result.warnings)
        warnings.append(qstr(w));

    int saved = 0;
    QString firstPath;
    for (const bi::ImportedShow &show : result.shows) {
        QString why;
        const QString path = saveImportedShow(show, applyTemplate, libraryFolder, &why);
        if (path.isEmpty()) {
            warnings.append(QObject::tr("%1: %2").arg(qstr(show.name), why));
            continue;
        }
        if (firstPath.isEmpty())
            firstPath = path;
        ++saved;
        emit ImportService::instance().showSaved(saved);   // Qt 6: signals are public; the sweep reports progress as it goes
    }
    const int bibles = static_cast<int>(result.bibles.size());
    if (saved > 0)
        ShowService::instance().refreshLibrary();
    if (bibles > 0)
        // And with it the published id list is re-walked, so the new Bible is in
        // `bibles` when bibleChanged arrives (see refreshBibles — hand-emitting the
        // signal here announced a list that still held the ids from before).
        SearchService::instance().refreshBibles();

    QString summary;
    if (saved > 0 && bibles > 0) summary = QObject::tr("Imported %n show(s) and %1 Bible(s).", "", saved).arg(bibles);
    else if (saved > 0) summary = QObject::tr("Imported %n show(s) from %1.", "", saved).arg(formatName);
    else if (bibles > 0) summary = QObject::tr("Installed %n Bible(s).", "", bibles);
    else summary = QObject::tr("Nothing could be imported from %1.").arg(formatName);
    if (!warnings.isEmpty())
        summary += QLatin1Char(' ') + QObject::tr("%n file(s) had a problem: %1", "", static_cast<int>(warnings.size())).arg(warnings.first());
    EngineBridge::write((saved > 0 || bibles > 0) && warnings.isEmpty() ? QStringLiteral("info") : QStringLiteral("warning"), QStringLiteral("Import"),
                        summary + (warnings.isEmpty() ? QString() : QStringLiteral(" [") + warnings.join(QStringLiteral("; ")) + QLatin1Char(']')));
    EventBus::instance().notify(summary, (saved > 0 || bibles > 0) ? (warnings.isEmpty() ? QStringLiteral("success") : QStringLiteral("warning")) : QStringLiteral("error"),
                                QObject::tr("Import"), QStringLiteral("import.done"));

    return { { QStringLiteral("ok"), saved > 0 || bibles > 0 }, { QStringLiteral("error"), QString() }, { QStringLiteral("shows"), saved },
             { QStringLiteral("bibles"), bibles }, { QStringLiteral("files"), static_cast<int>(result.files) },
             { QStringLiteral("warnings"), warnings }, { QStringLiteral("firstShow"), firstPath } };
}

// A screenshot on the clipboard becomes a still image MEDIA SOURCE. Media comes
// into this app as FOLDERS ("adding a folder is how media is brought in" —
// MediaLibraryService's contract), so the bitmap is written as a PNG into an
// app-owned clipboard folder that is then added to the library exactly like a
// folder the user picked: the picture stays a real file the operator can put on
// air, and nothing has to learn a second way of holding media.
// `folder` receives the folder to add; "" means the picture could not be saved.
QString saveClipboardImage(const bps::platform::ClipboardImage &image, QString *folder)
{
    const QString base = dataroot::appDataRoot();
    if (base.isEmpty())
        return {};
    const QString dir = base + QStringLiteral("/media/clipboard");
    if (!QDir().mkpath(dir))
        return {};
    // One file per pickup, named for when it was taken — pasting three
    // screenshots in a row must not overwrite the first two.
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString path = dir + QStringLiteral("/screenshot-") + stamp + QStringLiteral(".png");
    // The PAL hands back tightly packed RGBA8, which QImage wraps IN PLACE (no
    // conversion, no copy) — the buffer outlives the save below.
    const QImage picture(image.rgba.data(), image.width, image.height,
                         image.width * 4, QImage::Format_RGBA8888);
    if (picture.isNull() || !picture.save(path, "PNG"))
        return {};
    if (folder)
        *folder = dir;
    return path;
}

} // namespace

ImportService &ImportService::instance()
{
    static ImportService s;
    return s;
}

ImportService *ImportService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

ImportService::ImportService(QObject *parent) : QObject(parent) {}

void ImportService::setProgress(qreal fraction, const QString &status)
{
    m_progress = fraction;
    m_status = status;
    emit progressChanged();
}

QVariantList ImportService::formats() const
{
    QVariantList out;
    for (const bi::ImportFormat &f : bi::ImportFormats()) {
        QStringList extensions;
        QStringList patterns;
        for (const std::string &e : f.extensions) {
            extensions.append(qstr(e));
            patterns.append(QStringLiteral("*.") + qstr(e));
        }
        // A format with no extension (OpenSong's files have none) opens any file.
        const QString filter = patterns.isEmpty() ? QStringLiteral("All files (*)") : QStringLiteral("%1 (%2)").arg(qstr(f.name), patterns.join(QLatin1Char(' ')));
        out.append(QVariantMap{
            { QStringLiteral("id"), qstr(f.id) }, { QStringLiteral("name"), qstr(f.name) }, { QStringLiteral("description"), qstr(f.description) },
            { QStringLiteral("extensions"), extensions }, { QStringLiteral("kind"), QString::fromLatin1(bi::ToString(f.kind)) },
            { QStringLiteral("available"), f.available }, { QStringLiteral("section"), qstr(f.section) }, { QStringLiteral("primary"), f.primary },
            { QStringLiteral("tutorial"), qstr(f.tutorial) }, { QStringLiteral("icon"), qstr(f.icon) }, { QStringLiteral("filter"), filter },
        });
    }
    return out;
}

QVariantMap ImportService::importFiles(const QString &formatId, const QStringList &files)
{
    if (m_busy)
        return failure(tr("An import is already running."));
    if (!EngineBridge::instance().booted())
        return failure(tr("The engine is not running yet."));
    const bi::ImportFormat *format = bi::FindImportFormat(formatId.toStdString());
    if (!format)
        return failure(tr("There is no import format '%1'.").arg(formatId));

    // File reading stays on the caller's thread (fast, local); the parse +
    // save sweep below moves to a worker so the window keeps breathing.
    std::vector<bi::ImportFile> input;
    QStringList unreadable;
    for (const QString &raw : files) {
        const QString path = localPath(raw);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            unreadable.append(QFileInfo(path).fileName());
            continue;
        }
        bi::ImportFile f;
        const QFileInfo info(path);
        f.name = info.completeBaseName().toStdString();
        f.extension = info.suffix().toLower().toStdString();
        f.path = path.toStdString();
        const QByteArray bytes = file.readAll();
        f.content.assign(bytes.constData(), static_cast<size_t>(bytes.size()));
        input.push_back(std::move(f));
    }
    if (input.empty())
        return failure(tr("None of the files could be opened."));

    // A FreeShow LIBRARY import lands in a category named after the imported
    // files' own folder ("FreeShow" when it cannot be told) — one manageable
    // batch per source folder, not hundreds of loose shows in the library
    // root. Re-importing clears that category first (to the recoverable
    // .deleted bin), so a fresh import REPLACES the old batch.
    const bool isFreeShow = formatId == QLatin1String("freeshow") || formatId == QLatin1String("freeshow_project");
    QString libraryFolder;
    if (isFreeShow) {
        libraryFolder = QFileInfo(localPath(files.first())).dir().dirName();
        if (libraryFolder.isEmpty() || libraryFolder == ".")
            libraryFolder = QStringLiteral("FreeShow");
    }

    m_busy = true;
    emit busyChanged();
    setProgress(0.0, tr("Reading %n file(s)…", "", static_cast<int>(input.size())));

    const QString formatName = qstr(format->name);
    QThreadPool::globalInstance()->start([this, formatId, formatName, libraryFolder, isFreeShow,
                                          input = std::move(input), unreadable = std::move(unreadable), total = files.size()]() mutable {
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Import"),
                            QStringLiteral("Reading %1 file(s) as %2").arg(input.size()).arg(formatName));
        auto result = bi::ImportFiles(formatId.toStdString(), input);
        if (!result.ok()) {
            const QString why = qstr(result.error().message);
            QMetaObject::invokeMethod(this, [this, why]() mutable {
                m_busy = false;
                emit busyChanged();
                setProgress(0.0, QString());
                EventBus::instance().notify(why, QStringLiteral("error"), tr("Import"), QStringLiteral("import.failed"));
                emit finished(failure(why));
            }, Qt::QueuedConnection);
            return;
        }
        for (const QString &name : unreadable)
            result.value().warnings.push_back(name.toStdString() + ": could not be opened");
        result.value().files = total;

        // THE WHOLE SAVE SWEEP runs here on the worker: parse and save are the
        // slow parts (a 700-file batch takes a minute or more), and running
        // them on the GUI thread is exactly the AppHang this threading fixed.
        // Everything the sweep touches is thread-safe or moved to the worker:
        // ShowLibrary is mutex-protected, EventBus marshals cross-thread, and
        // progress hops back via QueuedConnection. Only the final library
        // republish + toast run on the GUI thread below.
        const int showCount = static_cast<int>(result.value().shows.size());
        int saved = 0;
        QStringList warnings;
        for (const std::string &w : result.value().warnings)
            warnings.append(qstr(w));
        QString firstPath;
        // The category CLEAR and per-show saves run on the worker: the engine
        // ShowLibrary is mutex-protected. The one GUI-side effect inside
        // clearLibraryCategory (republishing the QML-facing lists) is skipped
        // here — publishLibrary() writing QStringList members + emitting on a
        // worker thread would race the GUI thread's reads; the sweep ends with
        // one refreshLibrary() on the GUI thread anyway.
        if (isFreeShow && showCount > 0)
            ShowService::instance().clearLibraryCategoryNoPublish(libraryFolder);

        // ---- the artwork the documents reference ----------------------------
        // A ProPresenter deck names its pictures in Media/Assets; the engine has
        // already resolved those to files that exist here. They are COPIED into
        // the app's own media folder for this source library (one folder per
        // library, so two libraries' "logo.png" cannot collide), and the folders
        // are added to the media library below on the interface thread: adding a
        // folder is how media comes into the app, so the deck's art turns up in
        // the Media dock as real files the operator can put on air.
        const QString mediaRoot = dataroot::appDataRoot() + QStringLiteral("/media/imported");
        QStringList mediaFolders;
        int mediaCopied = 0;
        for (bi::ImportedShow &show : result.value().shows) {
            if (show.mediaRefs.empty()) continue;
            QString folder = safeFolderName(!libraryFolder.isEmpty() ? libraryFolder : qstr(show.sourceFolder));
            if (folder.isEmpty()) folder = safeFolderName(formatName);
            const QString dir = mediaRoot + QLatin1Char('/') + folder;
            if (!QDir().mkpath(dir)) continue;
            for (const std::string &ref : show.mediaRefs) {
                const QString source = qstr(ref);
                const QString target = dir + QLatin1Char('/') + QFileInfo(source).fileName();
                // Already brought in by an earlier show of this batch (or an
                // earlier import): the same asset serves both, and a second copy
                // would only double the disk.
                if (!QFileInfo::exists(target)) {
                    if (!QFile::copy(source, target)) continue;
                    ++mediaCopied;
                }
                pointImportedMediaAtCopy(show, source, target);
                if (!mediaFolders.contains(dir)) mediaFolders.append(dir);
            }
        }
        for (const bi::ImportedShow &show : result.value().shows) {
            QString why;
            // Each show is filed under the folder ITS OWN file came from, so an
            // import that spans several source folders keeps their structure
            // ("Only Believe SongBook", "Tabernacle Songs", ...) instead of
            // piling every song into one category. FreeShow's library import is
            // one batch by design, and keeps its own folder.
            const QString folder = !libraryFolder.isEmpty() ? libraryFolder : qstr(show.sourceFolder);
            const QString path = saveImportedShow(show, /*applyTemplate=*/false, folder, &why);
            ++saved;   // progress counts attempts, so the bar always reaches the end
            QMetaObject::invokeMethod(this, [this, saved, showCount]() {
                if (showCount > 0)
                    setProgress(static_cast<qreal>(saved) / showCount, tr("Importing %1 of %2").arg(saved).arg(showCount));
            }, Qt::QueuedConnection);
            if (path.isEmpty()) {
                warnings.append(QObject::tr("%1: %2").arg(qstr(show.name), why));
                continue;
            }
            if (firstPath.isEmpty())
                firstPath = path;
        }
        const int bibles = static_cast<int>(result.value().bibles.size());

        QMetaObject::invokeMethod(this, [this, warnings = std::move(warnings), firstPath, saved, bibles,
                                         files = total, formatName, libraryFolder,
                                         mediaFolders = std::move(mediaFolders), mediaCopied]() mutable {
            // GUI thread: republish the library once and tell the user.
            if (saved > 0)
                ShowService::instance().refreshLibrary();
            if (bibles > 0)
                SearchService::instance().refreshBibles();   // the new Bible lands in the published list (see the sync path above)
            // The artwork's folders join the media library (a folder already in it
            // answers false; the rescan picks the new files up either way).
            for (const QString &dir : mediaFolders) {
                MediaLibraryService::instance().addFolderPath(dir);
                MediaLibraryService::instance().rescan(dir);
            }

            QString summary;
            if (saved > 0 && bibles > 0) summary = QObject::tr("Imported %n show(s) and %1 Bible(s).", "", saved).arg(bibles);
            else if (saved > 0) summary = QObject::tr("Imported %n show(s) from %1.", "", saved).arg(formatName);
            else if (bibles > 0) summary = QObject::tr("Installed %n Bible(s).", "", bibles);
            else summary = QObject::tr("Nothing could be imported from %1.").arg(formatName);
            if (mediaCopied > 0)
                summary += QLatin1Char(' ') + QObject::tr("Brought in %n media file(s).", "", mediaCopied);
            if (!warnings.isEmpty())
                summary += QLatin1Char(' ') + QObject::tr("%n file(s) had a problem: %1", "", static_cast<int>(warnings.size())).arg(warnings.first());
            EngineBridge::write((saved > 0 || bibles > 0) && warnings.isEmpty() ? QStringLiteral("info") : QStringLiteral("warning"), QStringLiteral("Import"),
                                summary + (warnings.isEmpty() ? QString() : QStringLiteral(" [") + warnings.join(QStringLiteral("; ")) + QLatin1Char(']')));
            EventBus::instance().notify(summary, (saved > 0 || bibles > 0) ? (warnings.isEmpty() ? QStringLiteral("success") : QStringLiteral("warning")) : QStringLiteral("error"),
                                        QObject::tr("Import"), QStringLiteral("import.done"));

            QVariantMap answer = { { QStringLiteral("ok"), saved > 0 || bibles > 0 }, { QStringLiteral("error"), QString() }, { QStringLiteral("shows"), saved },
                                   { QStringLiteral("bibles"), bibles }, { QStringLiteral("files"), files },
                                   { QStringLiteral("media"), mediaCopied },
                                   { QStringLiteral("warnings"), warnings }, { QStringLiteral("firstShow"), firstPath } };
            m_busy = false;
            emit busyChanged();
            setProgress(1.0, QString());
            emit imported(answer.value(QStringLiteral("shows")).toInt(), answer.value(QStringLiteral("bibles")).toInt());
            emit finished(answer);
        }, Qt::QueuedConnection);
    });
    return { { QStringLiteral("ok"), true }, { QStringLiteral("started"), true } };
}

QVariantMap ImportService::importText(const QString &text, const QString &name, const QString &category)
{
    if (!EngineBridge::instance().booted())
        return failure(tr("The engine is not running yet."));
    bi::ImportResult result;
    result.files = 1;
    bi::ImportedShow show = bi::ShowFromClipboardText(text.toStdString());
    if (show.sections.empty())
        return failure(tr("There is no text to import."));
    if (!name.trimmed().isEmpty())
        show.name = name.trimmed().toStdString();
    if (!category.trimmed().isEmpty())
        show.category = category.trimmed().toStdString();
    result.shows.push_back(std::move(show));
    // Fresh-typed/pasted content (Quick Lyrics, "Paste from clipboard") — the
    // configured song template gives it a real layout, nothing to preserve.
    QVariantMap answer = finish(result, tr("the clipboard"), /*applyTemplate=*/true);
    emit imported(answer.value(QStringLiteral("shows")).toInt(), 0);
    return answer;
}

struct ImportService::ClipboardPick {
    enum class Kind { Text, Image, NoImageSupport, Empty } kind = Kind::Empty;
    QString text;         // Kind::Text
    QString imagePath;    // Kind::Image: the saved screenshot
    QString imageFolder;  // ... and the folder it landed in
};

// ASYNC, and it has to be. READ THROUGH THE PLATFORM LAYER, not QClipboard:
// the PAL's clipboard open WAITS OUT another process's write (a screenshot
// posts a full-screen bitmap, which takes a moment, so a first-attempt read
// fails on a machine doing nothing wrong), and Qt's own probing is what emits
// "Retrying to obtain clipboard." when that happens — the amber "Application
// — ..." notice the notification area had to learn to suppress. Qt is
// consulted only when there is no platform layer at all (headless/test).
//
// The read runs on a worker because that wait belongs to somebody else's
// process: the interface thread has no business in it, and neither has the
// screenshot's PNG encode (tens of megabytes of pixels). Only the library
// notify and the emits need the interface thread — applyClipboardRead below.
void ImportService::importClipboard()
{
    auto *runner = QThread::create([this] {
        ClipboardPick pick;
        if (bps::platform::PlatformAccessor::Installed()) {
            auto &clipboard = bps::platform::PlatformAccessor::Get().Clipboard();
            // Text first: after copying a verse the clipboard holds words, and
            // words are what this dialog's text import is for.
            if (auto text = clipboard.ReadText(); text.ok() && !text.value().empty()) {
                pick.kind = ClipboardPick::Kind::Text;
                pick.text = qstr(text.value());
            } else {
                // A SCREENSHOT is the ordinary clipboard content on Windows —
                // Win+Shift+S posts a bitmap and no text at all — so an image
                // is not an error, it is the thing to import: it lands in the
                // media library as a still source the operator can put on air.
                auto image = clipboard.ReadImage();
                if (image.ok() && !image.value().empty()) {
                    pick.kind = ClipboardPick::Kind::Image;
                    QString folder;
                    pick.imagePath = saveClipboardImage(image.value(), &folder);
                    pick.imageFolder = folder;
                } else if (image.error().code == bps::Err::Unsupported) {
                    pick.kind = ClipboardPick::Kind::NoImageSupport;
                }
            }
        } else {
            // No platform layer (headless): Qt's own clipboard, best effort.
            QClipboard *clipboard = QGuiApplication::clipboard();
            const QString text = clipboard == nullptr ? QString() : clipboard->text();
            if (!text.trimmed().isEmpty()) {
                pick.kind = ClipboardPick::Kind::Text;
                pick.text = text;
            } else {
                const QMimeData *data = clipboard == nullptr ? nullptr : clipboard->mimeData();
                pick.kind = (data != nullptr && data->hasImage())
                                ? ClipboardPick::Kind::NoImageSupport
                                : ClipboardPick::Kind::Empty;
            }
        }
        QMetaObject::invokeMethod(this, [this, pick] { applyClipboardRead(pick); },
                                  Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    runner->start();
}

// The interface thread's half: what the pick MEANS, said out loud and handed
// back to the dialog. Everything here touches app state (the media library,
// the notice bus, the emits), which is why the half that must stay there.
void ImportService::applyClipboardRead(const ClipboardPick &pick)
{
    switch (pick.kind) {
    case ClipboardPick::Kind::Text:
        emit clipboardImported(importText(pick.text));
        return;
    case ClipboardPick::Kind::Image: {
        if (pick.imagePath.isEmpty()) {
            emit clipboardImported(failure(tr("The screenshot on the clipboard could not be saved.")));
            return;
        }
        // Already-added folders answer false; the rescan is what picks the new
        // file up either way.
        MediaLibraryService::instance().addFolderPath(pick.imageFolder);
        MediaLibraryService::instance().rescan(pick.imageFolder);
        const QString summary = tr("Screenshot added to your media library.");
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Import"),
                            summary + QLatin1Char(' ') + pick.imagePath);
        EventBus::instance().notify(summary, QStringLiteral("success"), tr("Import"),
                                    QStringLiteral("import.done"));
        emit imported(0, 0);
        QVariantMap answer;
        answer.insert(QStringLiteral("ok"), true);
        answer.insert(QStringLiteral("error"), QString());
        answer.insert(QStringLiteral("shows"), 0);
        answer.insert(QStringLiteral("bibles"), 0);
        answer.insert(QStringLiteral("files"), 1);
        answer.insert(QStringLiteral("warnings"), QStringList());
        answer.insert(QStringLiteral("firstShow"), QString());
        emit clipboardImported(answer);
        return;
    }
    case ClipboardPick::Kind::NoImageSupport:
        emit clipboardImported(failure(tr("The clipboard holds no text, and this platform cannot "
                                          "read clipboard images.")));
        return;
    case ClipboardPick::Kind::Empty:
    default:
        emit clipboardImported(failure(tr("There is nothing on the clipboard to import.")));
        return;
    }
}
