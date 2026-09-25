#include "services/SearchService.h"

#include "services/EngineBridge.h"
#include "services/MediaLibraryService.h"
#include "services/EventBus.h"
#include "services/ShowService.h"

#include "modules/bible/BibleEngine.hpp"
#include "modules/library/TheTableLibrary.hpp"
#include "modules/search/SearchEngine.hpp"
#include "modules/search/TextMatching.hpp"
#include "modules/songs/SongEngine.hpp"
#include "services/TheTableService.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QFileInfo>
#include <QJSEngine>
#include <QQmlEngine>
#include <QCoreApplication>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#include <atomic>
#include <memory>

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace bb = bps::bible;
namespace bl = bps::library;
namespace bs = bps::search;
namespace sg = bps::song;
namespace ts = bps::search::textmatch;

namespace {

using Scored = std::pair<int, QVariantMap>;   // lower score = better match

QVariantMap makeResult(const QString &kind, const QString &title, const QString &subtitle,
                       const QString &id, const QString &path = QString(),
                       const QString &text = QString(), const QString &key = QString())
{
    return {
        { QStringLiteral("kind"), kind },
        { QStringLiteral("title"), title },
        { QStringLiteral("subtitle"), subtitle },
        { QStringLiteral("text"), text },
        { QStringLiteral("id"), id },
        { QStringLiteral("path"), path },
        { QStringLiteral("key"), key },
    };
}

// The query's words, lower-cased (word = letter/digit run).
QStringList splitWords(const QString &s)
{
    QStringList out;
    QString cur;
    for (const QChar &c : s) {
        if (c.isLetterOrNumber()) cur += c.toLower();
        else if (!cur.isEmpty()) { out << cur; cur.clear(); }
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}

// Word-level matching for the Quick search's local surfaces (slides, settings,
// show names) — the same semantics the engine gives songs/Bible/The Table:
// EVERY query word must land in a field, words may sit anywhere in it, and a
// MISSPELLED word is corrected against the field's own words (bounded edit
// distance, first letter kept). "setings" finds "Settings", "outpt colr"
// finds "Outputs". Quality: name starts with the query word 0 < word start 1
// < inside a word 2 < typo-corrected 3; -1 = no match.
int matchScore(const QString &needle, std::initializer_list<QString> fields)
{
    const QStringList words = splitWords(needle);
    if (words.isEmpty()) return needle.trimmed().isEmpty() ? 0 : -1;
    int best = -1;
    for (const QString &f : fields) {
        if (f.isEmpty()) continue;
        // Exact pass: every word present.
        bool all = true;
        int worst = 0;
        for (const QString &w : words) {
            const int at = f.indexOf(w, 0, Qt::CaseInsensitive);
            if (at < 0) { all = false; break; }
            const int ws = at == 0 ? 0 : (!f.at(at - 1).isLetterOrNumber() ? 1 : 2);
            worst = qMax(worst, ws);
        }
        if (all) {
            if (best < 0 || worst < best) best = worst;
            continue;
        }
        // Fuzzy pass: each missing word must correct to a word of this field.
        std::vector<std::string> vocab;
        for (const QString &w : splitWords(f))
            vocab.push_back(w.toStdString());
        all = true;
        for (const QString &w : words) {
            if (f.indexOf(w, 0, Qt::CaseInsensitive) >= 0) continue;
            const std::string q = w.toLower().toStdString();
            const size_t maxDist = q.size() <= 4 ? size_t(1) : size_t(2);
            bool fixed = false;
            for (const std::string &v : vocab)
                if (!v.empty() && v[0] == q[0]
                    && bs::textmatch::EditDistanceBounded(q, v, maxDist) <= maxDist) { fixed = true; break; }
            if (!fixed) { all = false; break; }
        }
        if (all && (best < 0 || 3 < best)) best = 3;
    }
    return best;
}

// Appends the best `limit` of `found` (stable, so ties keep their source order).
void take(QVariantList &out, std::vector<Scored> &found, int limit)
{
    std::stable_sort(found.begin(), found.end(),
                     [](const Scored &a, const Scored &b) { return a.first < b.first; });
    for (int i = 0; i < int(found.size()) && i < limit; ++i)
        out.append(found[size_t(i)].second);
}

QString qstr(const std::string &s) { return QString::fromStdString(s); }

// Every searchable entry of the Settings popup: what it is called, the section it is
// filed under, and the popup's navigation key for that section. The popup's own search
// box and the app-wide search both read this one list (through SearchService).
struct SettingEntry { const char *label; const char *section; const char *key; };
constexpr SettingEntry kSettings[] = {
    { "General", "Settings", "general" },
    { "Smart Config", "Settings", "smart" },
    { "Outputs", "Settings", "outputs" },
    { "Styles", "Settings", "styles" },
    { "Audio & Video", "Settings", "av" },
    { "Recording", "Settings", "recording" },
    { "Plugins", "Settings", "plugins" },
    { "Resource profile", "General", "general" },
    { "Appearance", "General", "general" },
    { "Accent color", "General", "general" },
    { "Lock In Mode", "General", "general" },
    { "Autosave", "General", "general" },
    { "Backups & recovery", "General", "general" },
    { "Crash recovery", "General", "general" },
    { "Notifications & logs", "General", "general" },
    { "Configuration mode", "Smart Config", "smart" },
    { "Hardware detected", "Smart Config", "smart" },
    { "Resource budgets", "Smart Config", "smart" },
    { "Stream platform", "Recording", "recording" },
    { "Stream key", "Recording", "recording" },
    { "Video bitrate", "Recording", "recording" },
    { "Encoder", "Recording", "recording" },
    { "Screens to record", "Recording", "recording" },
    { "Recording & Streaming", "Recording", "recording" },
    { "Installed plugins", "Plugins", "plugins" },
    { "Browse plugin store", "Plugins", "plugins" },
};

// The Settings entries that match `text` (name first, then section), best first.
QVariantList settingsMatches(const QString &text, int limit)
{
    std::vector<Scored> found;
    for (const SettingEntry &e : kSettings) {
        const QString label = QString::fromUtf8(e.label);
        const QString section = QString::fromUtf8(e.section);
        int score = matchScore(text, { label });
        if (score < 0) {
            score = matchScore(text, { section });
            if (score < 0) continue;
            score += 3;   // an entry that only matches through its section comes after real name matches
        }
        QVariantMap r = makeResult(QStringLiteral("setting"), label,
                                   QObject::tr("Settings · %1").arg(section), QString(), QString(),
                                   QString(), QString::fromUtf8(e.key));
        r.insert(QStringLiteral("section"), section);
        found.emplace_back(score, r);
    }
    QVariantList out;
    take(out, found, limit);
    return out;
}

QString slideNumberLabel(int index) { return QStringLiteral("Slide %1").arg(index + 1); }

// Verse text as the Bible files carry it has typesetting marks: the paragraph sign, the
// red-letter quote marks, and [brackets] around the translators' added words. None of
// that belongs in a search row or on a slide.
QString cleanVerse(QString s)
{
    s.remove(QChar(0x00B6)).remove(QChar(0x2039)).remove(QChar(0x203A)).remove(QLatin1Char('[')).remove(QLatin1Char(']'));
    return s.simplified();
}

// The Bible's own name for a book ("Psalms"), falling back to a capitalised id/alias.
QString bookNameFor(const bb::BibleEngine &bible, const std::string &bibleId, const std::string &bookId,
                    const std::string &fallback)
{
    auto book = bible.GetBook(bibleId, bookId);
    if (book.ok() && !book.value().name.empty())
        return qstr(book.value().name);
    QString name = qstr(fallback.empty() ? bookId : fallback);
    if (!name.isEmpty()) name[0] = name[0].toUpper();
    return name;
}

// "John 3:16" / "Psalms 23" / "Genesis 1:1-3" from a resolved reference.
QString referenceLabel(const bb::PassageRef &ref, const QString &book)
{
    if (ref.IsWholeChapter())
        return QStringLiteral("%1 %2").arg(book).arg(ref.chapter);
    QString label = QStringLiteral("%1 %2:%3").arg(book).arg(ref.chapter).arg(ref.verseStart);
    if (ref.verseEnd > ref.verseStart)
        label += QStringLiteral("-%1").arg(ref.verseEnd);
    return label;
}

QString bibleLabel(const std::string &bibleId) { return qstr(bibleId).toUpper(); }

// Reads one Bible file into the engine. Format comes from the file extension.
bool importOne(const QString &path, QString *why)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (why) *why = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    const QByteArray data = file.readAll();
    QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("sfm")) ext = QStringLiteral("usfm");
    auto r = bb::BibleEngine::Instance().Import(std::string_view(data.constData(), size_t(data.size())),
                                                ext.toStdString(), {});
    if (!r.ok()) {
        if (why) *why = qstr(r.error().message);
        return false;
    }
    return true;
}

} // namespace

SearchService::SearchService(QObject *parent)
    : QObject(parent)
{
}

SearchService::~SearchService() = default;

SearchService &SearchService::instance()
{
    static SearchService s;
    return s;
}

SearchService *SearchService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

QStringList SearchService::bibles() const
{
    QStringList out;
    if (!EngineBridge::instance().booted())
        return out;
    for (const std::string &id : bb::BibleEngine::Instance().BibleIds())
        out << qstr(id);
    return out;
}

QVariantList SearchService::resolveWords(const QString &text) const
{
    QVariantList out;
    const auto words = bs::textmatch::TokenizeWords(text.toStdString());
    if (words.empty()) return out;
    auto resolved = bs::SearchEngine::Instance().ResolveTermsWithSplits(words);
    if (!resolved.ok()) return out;
    // EVERY word comes back, spelled right or not (`changed` says which were
    // fixed). Callers build the yellow highlight from ALL the resolved words —
    // returning only the corrections dropped the words the user spelled
    // correctly ("then frend" highlighted "friend" and lost "then").
    for (size_t i = 0; i < resolved.value().size() && i < words.size(); ++i) {
        const std::string &r = resolved.value()[i].term;
        const std::string &typed = words[i];
        if (r.empty()) continue;
        QVariantMap m;
        m.insert(QStringLiteral("typed"), QString::fromStdString(typed));
        m.insert(QStringLiteral("resolved"), QString::fromStdString(r));
        m.insert(QStringLiteral("weight"), resolved.value()[i].weight);
        m.insert(QStringLiteral("changed"), r != typed);
        out.append(m);
    }
    return out;
}

QVariantList SearchService::searchSettings(const QString &text, int limit) const
{
    const QString q = text.trimmed();
    return q.isEmpty() ? QVariantList{} : settingsMatches(q, limit);
}

// One verse's full text — the Quick search hover preview (see the header).
QString SearchService::fullVerse(const QString &bibleId, const QString &bookId,
                                 int chapter, int verse) const
{
    if (!EngineBridge::instance().booted())
        return {};
    auto &bible = bb::BibleEngine::Instance();
    auto v = bible.GetVerse(bibleId.toStdString(), bookId.toStdString(), chapter, verse);
    return v.ok() ? cleanVerse(qstr(v.value().text)) : QString();
}

// ===========================================================================
// Search
// ===========================================================================

QVariantList SearchService::search(const QString &rawText, int perKind) const
{
    QVariantList out;
    const QString text = rawText.trimmed();
    if (text.isEmpty() || !EngineBridge::instance().booted())
        return out;
    const int limit = qMax(1, perKind);

    // ---- A typed Bible reference goes first ("john 3:16", "ps 23") ------------
    const std::vector<std::string> bibleIds = bb::BibleEngine::Instance().BibleIds();
    bool referenceFound = false;
    if (!bibleIds.empty()) {
        auto &bible = bb::BibleEngine::Instance();
        auto ref = bible.ResolveReference(text.toStdString(), bibleIds.front());
        if (ref.ok() && ref.value().Valid() && !ref.value().IsWholeBook()) {
            auto verses = bible.GetPassage(bibleIds.front(), ref.value());
            if (verses.ok() && !verses.value().empty()) {
                QStringList lines;
                const auto &vs = verses.value();
                const bool many = vs.size() > 1;
                for (size_t i = 0; i < vs.size() && i < 3; ++i)
                    lines << (many ? QStringLiteral("%1 ").arg(vs[i].verse) : QString()) + cleanVerse(qstr(vs[i].text));
                QString body = lines.join(QLatin1Char(' '));
                if (vs.size() > 3) body += QStringLiteral(" …");
                // What a slide gets: the passage itself (capped, so a whole chapter can't flood one slide).
                QStringList all;
                for (size_t i = 0; i < vs.size() && i < 8; ++i)
                    all << (many ? QStringLiteral("%1 ").arg(vs[i].verse) : QString()) + cleanVerse(qstr(vs[i].text));
                const QString bookName = bookNameFor(bible, bibleIds.front(), ref.value().bookId, ref.value().bookName);
                QVariantMap r = makeResult(QStringLiteral("bible"), referenceLabel(ref.value(), bookName),
                                           bibleLabel(bibleIds.front()),
                                           qstr(ref.value().ToString()), QString(), body);
                r.insert(QStringLiteral("bibleId"), qstr(bibleIds.front()));
                r.insert(QStringLiteral("fullText"), all.join(QLatin1Char(' ')));
                out.append(r);
                referenceFound = true;
            }
        }
    }

    // ---- Shows in the library ---------------------------------------------------
    {
        std::vector<Scored> found;
        for (const QVariant &v : ShowService::instance().searchLibrary(text)) {
            const QVariantMap s = v.toMap();
            const QString category = s.value(QStringLiteral("category")).toString();
            const int slides = s.value(QStringLiteral("slideCount")).toInt();
            found.emplace_back(matchScore(text, { s.value(QStringLiteral("name")).toString(), category }),
                               makeResult(QStringLiteral("show"), s.value(QStringLiteral("name")).toString(),
                                          (category.isEmpty() ? QString() : category + QStringLiteral(" · "))
                                              + QObject::tr("%n slide(s)", "", slides),
                                          s.value(QStringLiteral("id")).toString(),
                                          s.value(QStringLiteral("path")).toString()));
        }
        // matchScore is -1 when only the engine's fuzzier matching hit: keep those, last.
        for (auto &f : found) if (f.first < 0) f.first = 3;
        take(out, found, limit);
    }

    // ---- The working show: slides, templates, overlays, categories ------------------
    ShowService &shows = ShowService::instance();
    if (shows.hasShow()) {
        const QVariantMap show = shows.currentShow();

        std::vector<Scored> slides;
        const QVariantList slideList = show.value(QStringLiteral("slides")).toList();
        for (int i = 0; i < slideList.size(); ++i) {
            const QVariantMap s = slideList[i].toMap();
            const QString title = s.value(QStringLiteral("title")).toString();
            const QString line1 = s.value(QStringLiteral("line1")).toString();
            const QString line2 = s.value(QStringLiteral("line2")).toString();
            const QString ref = s.value(QStringLiteral("ref")).toString();
            const int score = matchScore(text, { title, line1, line2, ref });
            if (score < 0) continue;
            slides.emplace_back(score,
                makeResult(QStringLiteral("slide"), title.isEmpty() ? slideNumberLabel(i) : title,
                           (!line1.isEmpty() ? line1 : (!ref.isEmpty() ? ref : slideNumberLabel(i)))
                               + QStringLiteral(" · ") + show.value(QStringLiteral("name")).toString(),
                           s.value(QStringLiteral("id")).toString()));
        }
        take(out, slides, limit);

        auto named = [&](const QString &listKey, const QString &kind,
                         const std::function<QString(const QVariantMap &)> &subtitle) {
            std::vector<Scored> found;
            for (const QVariant &v : show.value(listKey).toList()) {
                const QVariantMap m = v.toMap();
                const QString name = m.value(QStringLiteral("name")).toString();
                const int score = matchScore(text, { name });
                if (score < 0) continue;
                found.emplace_back(score, makeResult(kind, name, subtitle(m),
                                                     m.value(QStringLiteral("id")).toString()));
            }
            take(out, found, limit);
        };
        named(QStringLiteral("templates"), QStringLiteral("template"), [](const QVariantMap &m) {
            const QString type = m.value(QStringLiteral("contentType")).toString();
            return type.isEmpty() ? QObject::tr("Template") : QObject::tr("Template · %1").arg(type);
        });
        named(QStringLiteral("overlays"), QStringLiteral("overlay"), [](const QVariantMap &m) {
            return QObject::tr("Overlay · %1%2").arg(m.value(QStringLiteral("scope")).toString(),
                m.value(QStringLiteral("enabled")).toBool() ? QString() : QObject::tr(" · off"));
        });
        named(QStringLiteral("categories"), QStringLiteral("category"), [](const QVariantMap &m) {
            const QString type = m.value(QStringLiteral("contentType")).toString();
            return type.isEmpty() ? QObject::tr("Category") : QObject::tr("Category · %1").arg(type);
        });
    }

    // ---- Settings -------------------------------------------------------------------
    out += settingsMatches(text, limit);

    // ---- Media: images and videos by name (the media library's folders) ------------------------
    {
        int n = 0;
        for (const QVariant &v : MediaLibraryService::instance().items(QString(), text)) {
            if (n++ >= limit) break;
            const QVariantMap m = v.toMap();
            const QString mediaKind = m.value(QStringLiteral("kind")).toString();
            out.append(makeResult(QStringLiteral("media"), m.value(QStringLiteral("name")).toString(),
                                  (mediaKind == QLatin1String("video") ? QObject::tr("Video")
                                   : mediaKind == QLatin1String("audio") ? QObject::tr("Audio") : QObject::tr("Image")) + QStringLiteral(" \u00b7 ")
                                      + QFileInfo(m.value(QStringLiteral("folder")).toString()).fileName(),
                                  m.value(QStringLiteral("id")).toString(), m.value(QStringLiteral("path")).toString()));
        }
    }

    // ---- Songs (engine) -------------------------------------------------------------
    if (text.size() >= 2) {
        auto songs = sg::SongEngine::Instance().Search(text.toStdString());
        if (songs.ok()) {
            int n = 0;
            for (const sg::Song &s : songs.value()) {
                if (n++ >= limit) break;
                QStringList authors;
                for (const std::string &a : s.metadata.authors) authors << qstr(a);
                out.append(makeResult(QStringLiteral("song"), qstr(s.metadata.title),
                                      authors.isEmpty() ? QObject::tr("Song") : authors.join(QStringLiteral(", ")),
                                      qstr(s.id)));
            }
        }
    }

    // ---- Bible text (engine, every installed Bible) ---------------------------------
    if (!bibleIds.empty() && !referenceFound && text.size() >= 3) {
        auto &bible = bb::BibleEngine::Instance();
        auto hits = bible.Search(text.toStdString());
        if (hits.ok()) {
            int n = 0;
            for (const bb::BibleSearchHit &h : hits.value()) {
                if (n++ >= qMax(limit, 6)) break;
                const QString name = bookNameFor(bible, h.bibleId, h.bookId, std::string());
                QString body = cleanVerse(qstr(h.snippet));
                if (body.isEmpty()) {
                    auto verse = bible.GetVerse(h.bibleId, h.bookId, h.chapter, h.verse);
                    if (verse.ok()) body = cleanVerse(qstr(verse.value().text));
                }
                QVariantMap r = makeResult(QStringLiteral("bible"),
                                           QStringLiteral("%1 %2:%3").arg(name).arg(h.chapter).arg(h.verse),
                                           bibleLabel(h.bibleId), qstr(h.reference), QString(), body);
                // (the verse text rides the `text` slot — an earlier version passed
                // it as `path`, so the dialog showed no text and every fuzzy match
                // looked wrong; the reference-resolution result below had it right)
                r.insert(QStringLiteral("bibleId"), qstr(h.bibleId));
                // Where the verse lives — the dialog's hover preview fetches the FULL
                // text through fullVerse() with these (the snippet is only an excerpt).
                r.insert(QStringLiteral("bookId"), qstr(h.bookId));
                r.insert(QStringLiteral("chapter"), h.chapter);
                r.insert(QStringLiteral("verse"), h.verse);
                out.append(r);
            }
        }
    }

    // ---- Sermon paragraphs (The Table's library) -------------------------------------
    // (2 chars on: the library's own Search() takes two-char words — "my" —
    // and matches terms spread across up to two consecutive paragraphs.)
    // A code-shaped query ("47-", "47-0412", "63-0628e") is naming a SERMON:
    // the library now answers with sermon-level rows, and THE TABLE leads the
    // list (user call: "when i type 47- the filter should have known to filter
    // from the table" — Bible verses that merely contain 47 are noise here).
    // STRICT code shape: every token digits-with-optional-letter-suffix
    // ("47", "0412", "1100x") AND a dash or 3+ digits. A bare "47" stays a
    // verse/year query, and a word beside a number ("genesis 47") is NOT a
    // code — it must not steal the query from the Bible rows.
    bool codeShaped = false;
    {
        const QStringList tokens = text.split(QRegularExpression("[^A-Za-z0-9]+"), Qt::SkipEmptyParts);
        QRegularExpression codeTok(QStringLiteral("^\\d+[a-z]*$"));
        bool digits = false, allCode = !tokens.isEmpty();
        int digitCount = 0;
        for (const QString &t : tokens) {
            if (!t.contains(codeTok)) { allCode = false; break; }
            for (const QChar &c : t) if (c.isDigit()) { digits = true; ++digitCount; }
        }
        codeShaped = allCode && digits
                         && (text.contains(QLatin1Char('-')) || digitCount >= 3);
    }
    QVariantList tableRows;
    if (text.size() >= 2) {
        // A code listing IS The Table's answer ("47-" = every 1947 sermon): the
        // per-kind cap (4) would truncate the sermon list to a few rows.
        const int tableLimit = codeShaped ? qMax(limit, 30) : qMax(limit, 4);
        const QVariantList hits = TheTableService::instance().search(text, tableLimit);
        for (const QVariant &v : hits) {
            const QVariantMap h = v.toMap();
            // Title = the sermon's citation line minus the paragraph — the same
            // label the pane's own match rows show ("Faith Is The Substance"),
            // with the code in the subtitle. The old scripture-shaped title
            // ("1953 12 · ¶3") matched nothing when picked (the pane searches
            // citation lines), so picking a sermon landed on an empty tab.
            const QString cite = h.value(QStringLiteral("citation")).toString();
            const QString title = cite.isEmpty()
                                      ? h.value(QStringLiteral("reference")).toString()
                                      : cite.mid(cite.indexOf(QStringLiteral(" - ")) + 3);
            // Sermon-level rows (a code match: verse 0) are the sermon itself —
            // no paragraph marker.
            const int verseNo = h.value(QStringLiteral("verse")).toInt();
            QVariantMap r = makeResult(QStringLiteral("table"),
                                       verseNo > 0
                                           ? QStringLiteral("%1 · ¶%2").arg(title).arg(verseNo)
                                           : title,
                                       cite.isEmpty() ? tr("The Table") : cite,
                                       h.value(QStringLiteral("bookId")).toString(),
                                       QString(), h.value(QStringLiteral("snippet")).toString());
            r.insert(QStringLiteral("bookId"), h.value(QStringLiteral("bookId")));
            r.insert(QStringLiteral("chapter"), h.value(QStringLiteral("chapter")));
            r.insert(QStringLiteral("verse"), h.value(QStringLiteral("verse")));
            r.insert(QStringLiteral("spanned"), h.value(QStringLiteral("spanned")));
            // The library's ranking score rides along: the dialog's pick-float
            // lets a previously-picked row keep its boost only over rows it
            // actually outscores (a picked ¶ must not bury the verbatim match).
            if (h.contains(QStringLiteral("score")))
                r.insert(QStringLiteral("score"), h.value(QStringLiteral("score")));
            out.append(r);
            tableRows.append(r);
        }
    }
    if (codeShaped && !tableRows.isEmpty())
        return tableRows;   // a sermon-code query names a SERMON: The Table only —
                            // Bible verses that merely contain "47" are noise here
    return out;
}

// The same aggregation off the GUI thread. The dialog passes a token (its
// request counter): stale responses (an older keystroke finishing late) are
// dropped here, so the UI only ever sees the LATEST query's rows.
void SearchService::searchAsync(const QString &text, int perKind, int token)
{
    auto *runner = QThread::create([this, text, perKind, token] {
        const QVariantList rows = search(text, perKind);
        QMetaObject::invokeMethod(this, [this, token, rows] {
            if (token == latestToken_)
                emit resultsReady(token, rows);
        }, Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    latestToken_ = token;   // newest request wins
    runner->start();
}

// ===========================================================================
// Bible files
// ===========================================================================

QStringList SearchService::candidateBibleFiles()
{
    QStringList files;
    const QString env = qEnvironmentVariable("VGR_BIBLE_FILE");
    if (!env.isEmpty() && QFileInfo::exists(env))
        files << env;

    const QDir userDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                       + QStringLiteral("/VGR Presenter/Bibles"));
    QSet<QString> bases;
    for (const QFileInfo &fi : userDir.entryInfoList(
             { "*.json", "*.xml", "*.osis", "*.usfm", "*.sfm", "*.txt" }, QDir::Files, QDir::Name)) {
        files << fi.absoluteFilePath();
        bases << fi.completeBaseName().toLower();
    }

#ifdef VGR_DEV_DATA_DIR
    // Development builds: the KJV that ships in the source tree, unless the user
    // already has one of their own.
    const QString dev = QStringLiteral(VGR_DEV_DATA_DIR "/kjv.json");
    if (QFileInfo::exists(dev) && !bases.contains(QStringLiteral("kjv")))
        files << dev;
#endif
    return files;
}

void SearchService::loadBibles()
{
    if (loadStarted_ || !EngineBridge::instance().booted())
        return;
    loadStarted_ = true;
    const QStringList files = candidateBibleFiles();
    if (files.isEmpty())
        return;

    loading_ = true;
    emit bibleChanged();

    QThread *thread = QThread::create([files] {
        for (const QString &path : files) {
            QString why;
            if (!importOne(path, &why))
                qWarning().noquote() << "Bible import skipped:" << path << "-" << why;
        }
    });
    loader_ = thread;
    connect(thread, &QThread::finished, this, [this] {
        loading_ = false;
        emit bibleChanged();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

bool SearchService::importBibleFile(const QString &path)
{
    if (!EngineBridge::instance().booted() || importing_)
        return false;

    // Reading the file is cheap — done here; the parse + verse-level index is
    // the slow part and runs on a worker thread (the same shape as The Table's
    // folder import), so the UI stays live and its progress bar can move.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        EventBus::instance().notify(QStringLiteral("cannot open %1").arg(path), QStringLiteral("error"),
                                    QObject::tr("Couldn't install the Bible"), QStringLiteral("bible.import"));
        return false;
    }
    const QByteArray data = file.readAll();
    QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("sfm"))
        ext = QStringLiteral("usfm");

    importing_ = true;
    progress_ = { { QStringLiteral("done"), 0 }, { QStringLiteral("total"), 0 },
                  { QStringLiteral("current"), QFileInfo(path).fileName() } };
    emit bibleImportingChanged();
    emit bibleProgressChanged();

    auto *worker = QThread::create([this, data, ext = ext.toStdString(),
                                    name = QFileInfo(path).fileName()] {
        // The bar moves every 50th verse (and at the end) — a queued event per
        // one of 31k verses would flood the GUI loop for nothing.
        const auto ticks = std::make_shared<std::atomic<int>>(0);
        bb::ImportOptions options;
        options.onProgress = [this, ticks](size_t done, size_t total, std::string_view book) {
            if (done != total && ticks->fetch_add(1) % 50 != 0)
                return;
            const QString bookName = QString::fromUtf8(book.data(), int(book.size()));
            QMetaObject::invokeMethod(this, [this, done, total, bookName] {
                progress_ = { { QStringLiteral("done"), qulonglong(done) },
                              { QStringLiteral("total"), qulonglong(total) },
                              { QStringLiteral("current"), bookName } };
                emit bibleProgressChanged();
            }, Qt::QueuedConnection);
        };
        auto r = bb::BibleEngine::Instance().Import(
            std::string_view(data.constData(), size_t(data.size())), ext, options);
        QMetaObject::invokeMethod(this, [this, r, name] {
            importing_ = false;
            progress_ = {};
            emit bibleImportingChanged();
            emit bibleProgressChanged();
            if (!r.ok()) {
                EventBus::instance().notify(qstr(r.error().message), QStringLiteral("error"),
                                            QObject::tr("Couldn't install the Bible"),
                                            QStringLiteral("bible.import"));
                return;
            }
            emit bibleChanged();
            EventBus::instance().notify(name, QStringLiteral("success"),
                                        QObject::tr("Bible installed"), QStringLiteral("bible.import"));
        }, Qt::QueuedConnection);
    });
    loader_ = worker;
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    // Quitting mid-import must not destroy a running thread (same as The Table's import).
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, worker,
            [worker] { worker->wait(20000); });
    worker->start();
    return true;
}

void SearchService::shutdown()
{
    if (loader_)
        loader_->wait(20000);
}
