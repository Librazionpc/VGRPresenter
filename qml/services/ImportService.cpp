#include "services/ImportService.h"

#include "modules/import/ImportEngine.hpp"
#include "modules/presentation/PresentationDocument.hpp"
#include "services/DesignLibraryService.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/SearchService.h"
#include "services/ShowConverter.h"
#include "services/ShowService.h"
#include "services/SlideBuilder.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QFileInfo>
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
        emit SearchService::instance().bibleChanged();   // the Scripture tab lists the new Bible

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
        for (const bi::ImportedShow &show : result.value().shows) {
            QString why;
            const QString path = saveImportedShow(show, /*applyTemplate=*/false, libraryFolder, &why);
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
                                         files = total, formatName, libraryFolder]() mutable {
            // GUI thread: republish the library once and tell the user.
            if (saved > 0)
                ShowService::instance().refreshLibrary();
            if (bibles > 0)
                emit SearchService::instance().bibleChanged();   // the Scripture tab lists the new Bible

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

            QVariantMap answer = { { QStringLiteral("ok"), saved > 0 || bibles > 0 }, { QStringLiteral("error"), QString() }, { QStringLiteral("shows"), saved },
                                   { QStringLiteral("bibles"), bibles }, { QStringLiteral("files"), files },
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

QVariantMap ImportService::importClipboard()
{
    return importText(QGuiApplication::clipboard()->text());
}
