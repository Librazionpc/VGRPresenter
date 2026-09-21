#include "services/TheTableService.h"

#include "services/EngineBridge.h"
#include "services/EventBus.h"

#include "modules/library/TheTableLibrary.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJSEngine>

namespace bl = bps::library;

namespace {

QString qstr(const std::string &s) { return QString::fromStdString(s); }

bl::TheTableLibrary *tableLibrary(void *opaque)
{
    return static_cast<bl::TheTableLibrary *>(opaque);
}

void report(const QString &message, const QString &level)
{
    EventBus::instance().notify(message, level, QObject::tr("The Table"),
                                QStringLiteral("table.library"));
}

} // namespace

TheTableService::TheTableService(QObject *parent)
    : QObject(parent)
{
    // Boot may already have run (tests, reload) or may still be pending (the
    // singleton is created when QML first touches it) — build the library the
    // moment the platform layer exists, the same contract as the design
    // libraries (a service created pre-boot stayed empty without the retry).
    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged, this, [this] {
        if (EngineBridge::instance().booted())
            loadLibrary();
    });
    if (EngineBridge::instance().booted())
        loadLibrary();
}

TheTableService &TheTableService::instance()
{
    static TheTableService s;
    return s;
}

TheTableService *TheTableService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

void TheTableService::loadLibrary()
{
    if (library_)
        return;
    auto &platform = bps::platform::PlatformAccessor::Get();
    const std::string file = platform.Filesystem().Join(platform.Paths().UserDataDir(),
                                                        "the_table.json");
    // The Open'd shared_ptr is deliberately released into the service's
    // lifetime: the singleton lives for the whole process (CppOwnership), so
    // the raw pointer is safe and keeps the engine header out of this one.
    static std::shared_ptr<bl::TheTableLibrary> keepAlive = bl::TheTableLibrary::Open(file);
    library_ = keepAlive.get();
    emit changed();
}

bool TheTableService::loading() const
{
    return loading_;
}

qlonglong TheTableService::verseCount() const
{
    if (!library_ || !EngineBridge::instance().booted())
        return 0;
    return static_cast<qlonglong>(tableLibrary(library_)->VerseCount());
}

QVariantMap TheTableService::documentFromEngine() const
{
    QVariantMap doc;
    doc.insert(QStringLiteral("name"), QStringLiteral("The Table"));
    QVariantList collections;
    if (library_ && EngineBridge::instance().booted()) {
        QVariantMap collection;
        collection.insert(QStringLiteral("name"), QStringLiteral("Sermons"));
        QVariantList books;
        for (const bl::TheTableBook &book : tableLibrary(library_)->Books()) {
            QVariantMap b;
            b.insert(QStringLiteral("name"), qstr(book.name));
            QVariantList sections;
            for (const bl::TheTableChapter &ch : book.chapters) {
                QVariantMap s;
                s.insert(QStringLiteral("title"), qstr(ch.title));
                QVariantList verses;
                for (const bl::TheTableVerse &v : ch.verses)
                    verses.append(qstr(v.text));
                s.insert(QStringLiteral("verses"), verses);
                sections.append(s);
            }
            b.insert(QStringLiteral("sections"), sections);
            books.append(b);
        }
        collection.insert(QStringLiteral("books"), books);
        collections.append(collection);
    }
    doc.insert(QStringLiteral("collections"), collections);
    return doc;
}

QVariantMap TheTableService::document() const
{
    return documentFromEngine();
}

QVariantList TheTableService::chapter(const QString &bookId, int chapter) const
{
    QVariantList out;
    if (!library_ || !EngineBridge::instance().booted())
        return out;
    auto ch = tableLibrary(library_)->GetChapter(bookId.toStdString(), chapter);
    if (!ch.ok())
        return out;
    for (const bl::TheTableVerse &v : ch.value().verses) {
        QVariantMap row;
        row.insert(QStringLiteral("number"), v.number);
        row.insert(QStringLiteral("text"), qstr(v.text));
        row.insert(QStringLiteral("heading"), qstr(v.heading));
        out.append(row);
    }
    return out;
}

QVariantList TheTableService::search(const QString &text, int limit) const
{
    QVariantList out;
    if (!library_ || !EngineBridge::instance().booted())
        return out;
    auto hits = tableLibrary(library_)->Search(text.toStdString(), size_t(limit));
    if (!hits.ok())
        return out;
    for (const bl::TheTableSearchHit &h : hits.value()) {
        QVariantMap row;
        row.insert(QStringLiteral("reference"), qstr(h.reference));
        row.insert(QStringLiteral("bookId"), qstr(h.bookId));
        row.insert(QStringLiteral("chapter"), h.chapter);
        row.insert(QStringLiteral("verse"), h.verse);
        row.insert(QStringLiteral("snippet"), qstr(h.snippet));
        out.append(row);
    }
    return out;
}

bool TheTableService::newSermon()
{
    if (!EngineBridge::instance().booted()) {
        report(tr("The engine is still starting — try again in a moment."), QStringLiteral("warning"));
        return false;
    }
    const std::string title = tr("Add a sermon").toStdString();
    auto picked = bps::platform::PlatformAccessor::Get().Dialogs().OpenFileDialog(
        title, { std::string("Sermons (*.txt *.pdf)"), std::string("All files (*.*)") });
    if (!picked.ok() || !picked.value())
        return false;
    const std::string path = *picked.value();

    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        report(tr("Couldn't read %1.").arg(qstr(path)), QStringLiteral("error"));
        return false;
    }
    const QByteArray bytes = file.readAll();

    auto r = tableLibrary(library_)->ImportSermon(path,
                                                  std::string_view(bytes.constData(), size_t(bytes.size())));
    if (!r.ok()) {
        report(qstr(r.error().message), QStringLiteral("error"));
        return false;
    }
    report(tr("Added %1 · %2").arg(QFileInfo(QString::fromStdString(path)).fileName(),
                                   qstr(r.value())),
           QStringLiteral("success"));
    emit changed();
    return true;
}
