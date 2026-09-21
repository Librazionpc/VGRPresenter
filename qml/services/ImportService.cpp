#include "services/ImportService.h"

#include "modules/import/ImportEngine.hpp"
#include "modules/presentation/PresentationDocument.hpp"
#include "services/DesignLibraryService.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/SearchService.h"
#include "services/ShowConverter.h"
#include "services/ShowService.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QFileInfo>
#include <QJSEngine>
#include <QQmlEngine>
#include <QUrl>

namespace bi = bps::import;

namespace {

QString qstr(const std::string &s) { return QString::fromStdString(s); }

// The template the imported songs look like: the engine's default song template.
constexpr const char *kSongTemplate = "tpl-default";

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
QString saveImportedShow(const bi::ImportedShow &imported, QString *why)
{
    bi::ShowBuildOptions options;
    const QVariantMap design = TemplateLibraryService::instance().design(QString::fromLatin1(kSongTemplate));
    for (const QVariant &b : design.value(QStringLiteral("blocks")).toList())
        options.templateBlocks.push_back(ShowConverter::blockFromVariant(b.toMap()));
    if (const QString bg = design.value(QStringLiteral("background")).toString(); !bg.isEmpty())
        options.background = bg.toStdString();

    bps::presentation::Presentation show = bi::ShowFromImported(imported, options);
    const QString path = ShowService::instance().newLibraryShowPath(QString(), qstr(show.name));
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
QVariantMap finish(const bi::ImportResult &result, const QString &formatName)
{
    QStringList warnings;
    for (const std::string &w : result.warnings)
        warnings.append(qstr(w));

    int saved = 0;
    QString firstPath;
    for (const bi::ImportedShow &show : result.shows) {
        QString why;
        const QString path = saveImportedShow(show, &why);
        if (path.isEmpty()) {
            warnings.append(QObject::tr("%1: %2").arg(qstr(show.name), why));
            continue;
        }
        if (firstPath.isEmpty())
            firstPath = path;
        ++saved;
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
    if (!EngineBridge::instance().booted())
        return failure(tr("The engine is not running yet."));
    const bi::ImportFormat *format = bi::FindImportFormat(formatId.toStdString());
    if (!format)
        return failure(tr("There is no import format '%1'.").arg(formatId));

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

    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Import"), QStringLiteral("Reading %1 file(s) as %2").arg(input.size()).arg(qstr(format->name)));
    auto result = bi::ImportFiles(formatId.toStdString(), input);
    if (!result.ok()) {
        EventBus::instance().notify(qstr(result.error().message), QStringLiteral("error"), tr("Import"), QStringLiteral("import.failed"));
        return failure(qstr(result.error().message));
    }
    for (const QString &name : unreadable)
        result.value().warnings.push_back(name.toStdString() + ": could not be opened");
    result.value().files = files.size();
    QVariantMap answer = finish(result.value(), qstr(format->name));
    emit imported(answer.value(QStringLiteral("shows")).toInt(), answer.value(QStringLiteral("bibles")).toInt());
    return answer;
}

QVariantMap ImportService::importText(const QString &text)
{
    if (!EngineBridge::instance().booted())
        return failure(tr("The engine is not running yet."));
    bi::ImportResult result;
    result.files = 1;
    bi::ImportedShow show = bi::ShowFromClipboardText(text.toStdString());
    if (show.sections.empty())
        return failure(tr("There is no text to import."));
    result.shows.push_back(std::move(show));
    QVariantMap answer = finish(result, tr("the clipboard"));
    emit imported(answer.value(QStringLiteral("shows")).toInt(), 0);
    return answer;
}

QVariantMap ImportService::importClipboard()
{
    return importText(QGuiApplication::clipboard()->text());
}
