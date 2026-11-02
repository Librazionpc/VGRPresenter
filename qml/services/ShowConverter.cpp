#include "services/ShowConverter.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QVariantList>

namespace bp = bps::presentation;

namespace {
std::string utf8(const QString &s) { return s.toStdString(); }
QString qstr(const std::string &s) { return QString::fromStdString(s); }

// JS object -> compact JSON string (what the engine stores as opaque meta).
std::string metaToJson(const QVariant &meta)
{
    const QVariantMap map = meta.toMap();
    if (map.isEmpty())
        return "{}";
    return QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact).toStdString();
}

QVariantMap metaFromJson(const std::string &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    return doc.isObject() ? doc.object().toVariantMap() : QVariantMap{};
}

std::vector<std::string> stringList(const QVariant &v)
{
    std::vector<std::string> out;
    for (const QVariant &e : v.toList())
        out.push_back(utf8(e.toString()));
    return out;
}

QVariantList toQList(const std::vector<std::string> &in)
{
    QVariantList out;
    for (const std::string &s : in)
        out.append(qstr(s));
    return out;
}

bp::ContentBlock blockFromVariant(const QVariantMap &b)
{
    bp::ContentBlock out;
    out.id = utf8(b.value(QStringLiteral("key")).toString());
    out.kind = utf8(b.value(QStringLiteral("kind"), QStringLiteral("text")).toString());
    out.text = utf8(b.value(QStringLiteral("text")).toString());
    out.x = b.value(QStringLiteral("x")).toDouble();
    out.y = b.value(QStringLiteral("y")).toDouble();
    out.width = b.value(QStringLiteral("width"), 160).toDouble();
    out.height = b.value(QStringLiteral("height"), 40).toDouble();
    out.bind = utf8(b.value(QStringLiteral("bind")).toString());
    out.metaJson = metaToJson(b.value(QStringLiteral("meta")));

    const QVariantMap s = b.value(QStringLiteral("style")).toMap();
    out.style.padding = s.value(QStringLiteral("padding")).toDouble();
    out.style.backgroundColor = utf8(s.value(QStringLiteral("backgroundColor"), QStringLiteral("transparent")).toString());
    out.style.cornerRadius = s.value(QStringLiteral("cornerRadius")).toDouble();
    out.style.borderEnabled = s.value(QStringLiteral("borderEnabled")).toBool();
    out.style.borderWidth = s.value(QStringLiteral("borderWidth"), 2).toDouble();
    out.style.borderStyle = utf8(s.value(QStringLiteral("borderStyle"), QStringLiteral("line")).toString());
    out.style.borderColor = utf8(s.value(QStringLiteral("borderColor"), QStringLiteral("#ffffff")).toString());
    return out;
}

QVariantMap blockToVariant(const bp::ContentBlock &b)
{
    QVariantMap style{
        { QStringLiteral("padding"), b.style.padding },
        { QStringLiteral("backgroundColor"), qstr(b.style.backgroundColor) },
        { QStringLiteral("cornerRadius"), b.style.cornerRadius },
        { QStringLiteral("borderEnabled"), b.style.borderEnabled },
        { QStringLiteral("borderWidth"), b.style.borderWidth },
        { QStringLiteral("borderStyle"), qstr(b.style.borderStyle) },
        { QStringLiteral("borderColor"), qstr(b.style.borderColor) },
    };
    const QVariantMap media = metaFromJson(b.metaJson).value(QStringLiteral("media")).toMap();
    if (media.contains(QStringLiteral("fit")))
        style.insert(QStringLiteral("mediaFit"), media.value(QStringLiteral("fit")));
    if (media.contains(QStringLiteral("opacity")))
        style.insert(QStringLiteral("opacity"), media.value(QStringLiteral("opacity")));

    return {
        { QStringLiteral("key"), qstr(b.id) },
        { QStringLiteral("kind"), qstr(b.kind) },
        { QStringLiteral("text"), qstr(b.text) },
        { QStringLiteral("x"), b.x },
        { QStringLiteral("y"), b.y },
        { QStringLiteral("width"), b.width },
        { QStringLiteral("height"), b.height },
        { QStringLiteral("bind"), qstr(b.bind) },
        { QStringLiteral("meta"), metaFromJson(b.metaJson) },
        { QStringLiteral("style"), style },
    };
}

std::vector<bp::ContentBlock> blocksFromVariant(const QVariant &list)
{
    std::vector<bp::ContentBlock> out;
    for (const QVariant &bv : list.toList())
        out.push_back(blockFromVariant(bv.toMap()));
    return out;
}

QString transitionName(bp::TransitionKind kind)
{
    switch (kind) {
    case bp::TransitionKind::Fade: return QStringLiteral("fade");
    case bp::TransitionKind::Slide: return QStringLiteral("slide");
    case bp::TransitionKind::Push: return QStringLiteral("push");
    case bp::TransitionKind::Zoom: return QStringLiteral("zoom");
    case bp::TransitionKind::Wipe: return QStringLiteral("wipe");
    case bp::TransitionKind::Crossfade: return QStringLiteral("crossfade");
    case bp::TransitionKind::Custom: return QStringLiteral("custom");
    }
    return QStringLiteral("fade");
}

QString scopeToString(bp::OverlayScope scope)
{
    switch (scope) {
    case bp::OverlayScope::All:      return QStringLiteral("all");
    case bp::OverlayScope::Category: return QStringLiteral("category");
    case bp::OverlayScope::Slide:    return QStringLiteral("slide");
    }
    return QStringLiteral("all");
}

bp::OverlayScope scopeFromString(const QString &s)
{
    if (s == QLatin1String("category")) return bp::OverlayScope::Category;
    if (s == QLatin1String("slide"))    return bp::OverlayScope::Slide;
    return bp::OverlayScope::All;
}
} // namespace

QVariantList ShowConverter::blocksToVariant(const std::vector<bp::ContentBlock> &blocks)
{
    QVariantList out;
    for (const bp::ContentBlock &b : blocks)
        out.append(ShowConverter::blockToVariant(b));
    return out;
}

bp::SlideTemplate ShowConverter::templateFromVariant(const QVariantMap &t)
{
    bp::SlideTemplate out;
    out.id = utf8(t.value(QStringLiteral("id")).toString());
    out.name = utf8(t.value(QStringLiteral("name")).toString());
    out.contentType = utf8(t.value(QStringLiteral("contentType")).toString());
    out.background = utf8(t.value(QStringLiteral("background"), QStringLiteral("transparent")).toString());
    out.blocks = blocksFromVariant(t.value(QStringLiteral("blocks")));
    out.metaJson = metaToJson(t.value(QStringLiteral("meta")));
    return out;
}

QVariantMap ShowConverter::templateToVariant(const bp::SlideTemplate &t)
{
    return {
        { QStringLiteral("id"), qstr(t.id) },
        { QStringLiteral("name"), qstr(t.name) },
        { QStringLiteral("contentType"), qstr(t.contentType) },
        { QStringLiteral("background"), qstr(t.background) },
        { QStringLiteral("blocks"), blocksToVariant(t.blocks) },
        { QStringLiteral("meta"), metaFromJson(t.metaJson) },
    };
}

QVariantMap ShowConverter::fillFromMeta(const QVariantMap &meta)
{
    static const char *const kKeys[] = {
        "ref", "refFull", "refLast",
        "tableCode", "tableYear", "tableTitle", "tableNumbers",
        "book", "bookAbbr", "bible", "chapter", "copyright",
    };
    QVariantMap out;
    for (const char *k : kKeys) {
        const QString key = QString::fromLatin1(k);
        if (meta.contains(key))
            out.insert(key, meta.value(key));
    }
    return out;
}

QVariantMap ShowConverter::resolvedToVariant(const bp::ResolvedSlide &r)
{
    return {
        { QStringLiteral("slideId"), qstr(r.slideId) },
        { QStringLiteral("categoryId"), qstr(r.categoryId) },
        { QStringLiteral("templateId"), qstr(r.templateId) },
        { QStringLiteral("visible"), r.visible },
        { QStringLiteral("background"), qstr(r.background) },
        { QStringLiteral("blocks"), blocksToVariant(r.blocks) },
        { QStringLiteral("overlayBlocks"), blocksToVariant(r.overlayBlocks) },
    };
}

QVariantMap ShowConverter::metaOf(const bp::Slide &slide)
{
    return metaFromJson(slide.metaJson);
}

std::string ShowConverter::metaToJsonString(const QVariantMap &meta)
{
    return metaToJson(meta);
}

bp::Category ShowConverter::categoryFromVariant(const QVariantMap &c)
{
    bp::Category cat;
    cat.id = utf8(c.value(QStringLiteral("id")).toString());
    cat.name = utf8(c.value(QStringLiteral("name")).toString());
    cat.contentType = utf8(c.value(QStringLiteral("contentType")).toString());
    cat.templateId = utf8(c.value(QStringLiteral("templateId")).toString());
    cat.outputs = stringList(c.value(QStringLiteral("outputs")));
    cat.metaJson = metaToJson(c.value(QStringLiteral("meta")));
    return cat;
}

bp::Overlay ShowConverter::overlayFromVariant(const QVariantMap &o)
{
    bp::Overlay overlay;
    overlay.id = utf8(o.value(QStringLiteral("id")).toString());
    overlay.name = utf8(o.value(QStringLiteral("name")).toString());
    overlay.scope = scopeFromString(o.value(QStringLiteral("scope")).toString());
    overlay.targetId = utf8(o.value(QStringLiteral("targetId")).toString());
    overlay.outputs = stringList(o.value(QStringLiteral("outputs")));
    overlay.enabled = o.value(QStringLiteral("enabled"), true).toBool();
    overlay.blocks = blocksFromVariant(o.value(QStringLiteral("blocks")));
    overlay.metaJson = metaToJson(o.value(QStringLiteral("meta")));
    return overlay;
}

bp::Slide ShowConverter::slideFromVariant(const QVariantMap &s)
{
    bp::Slide slide;
    slide.id = utf8(s.value(QStringLiteral("id")).toString());
    slide.title = utf8(s.value(QStringLiteral("title")).toString());
    const QString tag = s.value(QStringLiteral("tag")).toString();
    if (!tag.isEmpty())
        slide.tags.push_back(utf8(tag));
    const QString line1 = s.value(QStringLiteral("line1")).toString();
    const QString line2 = s.value(QStringLiteral("line2")).toString();
    const QString body = s.value(QStringLiteral("body")).toString();
    slide.text = utf8(!body.isEmpty() ? body
                                      : (line2.isEmpty() ? line1 : line1 + QLatin1Char('\n') + line2));
    slide.flowText = utf8(s.value(QStringLiteral("flowText")).toString());
    slide.flowChar = s.contains(QStringLiteral("flowChar"))
                         ? static_cast<float>(s.value(QStringLiteral("flowChar")).toDouble())
                         : -1.0f;
    // The passage's line breaks per output style (see Slide::flowBreaks): {styleId: [line starts]}.
    {
        const QVariantMap breaks = s.value(QStringLiteral("flowBreaks")).toMap();
        for (auto it = breaks.constBegin(); it != breaks.constEnd(); ++it) {
            std::vector<int> starts;
            const QVariantList list = it.value().toList();
            starts.reserve(static_cast<size_t>(list.size()));
            for (const QVariant &start : list)
                starts.push_back(start.toInt());
            slide.flowBreaks[utf8(it.key())] = std::move(starts);
        }
        const QVariantMap sizes = s.value(QStringLiteral("flowSizes")).toMap();
        for (auto it = sizes.constBegin(); it != sizes.constEnd(); ++it)
            slide.flowSizes[utf8(it.key())] = static_cast<float>(it.value().toDouble());
    }
    slide.background = utf8(s.value(QStringLiteral("background"), QStringLiteral("transparent")).toString());
    // Slide-wide filters (see applyFilters — the same door the Edit panel's live
    // patch comes through).
    if (s.contains(QStringLiteral("filters")))
        ShowConverter::applyFilters(slide, s.value(QStringLiteral("filters")).toMap());
    slide.categoryId = utf8(s.value(QStringLiteral("categoryId")).toString());
    slide.clearStage = s.value(QStringLiteral("clearStage"), false).toBool();
    QVariantMap meta{
        { QStringLiteral("tagColor"), s.value(QStringLiteral("tagColor")) },
        { QStringLiteral("line1"), line1 },
        { QStringLiteral("line2"), line2 },
        { QStringLiteral("ref"), s.value(QStringLiteral("ref")) },
        { QStringLiteral("contentType"), s.value(QStringLiteral("contentType")) },
        { QStringLiteral("showCategory"), s.value(QStringLiteral("showCategory")) },
        { QStringLiteral("templateOverride"), s.value(QStringLiteral("templateOverride"), false) },
    };
    const QVariantMap fill = s.value(QStringLiteral("fill")).toMap();
    for (auto it = fill.constBegin(); it != fill.constEnd(); ++it)
        meta.insert(it.key(), it.value());
    slide.metaJson = metaToJson(meta);
    slide.blocks = blocksFromVariant(s.value(QStringLiteral("blocks")));
    // Seconds the slide stays up before the show moves on by itself (0 = it waits for you).
    if (s.contains(QStringLiteral("nextTimer")))
        slide.durationMs = s.value(QStringLiteral("nextTimer")).toDouble() * 1000.0;
    return slide;
}

void ShowConverter::applyFilters(bp::Slide &slide, const QVariantMap &filters)
{
    const auto num = [&filters](const char *k, double fallback) {
        const auto it = filters.constFind(QLatin1String(k));
        return it == filters.constEnd() ? fallback : it.value().toDouble();
    };
    slide.filters.hueRotateDeg = num("hueRotate", 0.0);
    slide.filters.invert = num("invert", 0.0);
    slide.filters.blurPx = num("blur", 0.0);
    slide.filters.grayscale = num("grayscale", 0.0);
    slide.filters.brightness = num("brightness", 1.0);
    slide.filters.contrast = num("contrast", 1.0);
    slide.filters.saturate = num("saturate", 1.0);
}

bp::ContentBlock ShowConverter::blockFromVariant(const QVariantMap &b)
{
    return ::blockFromVariant(b);
}

QVariantMap ShowConverter::blockToVariant(const bp::ContentBlock &b)
{
    return ::blockToVariant(b);
}

bp::Presentation ShowConverter::fromVariant(const QVariantMap &show)
{
    bp::Presentation out;
    out.id = utf8(show.value(QStringLiteral("id")).toString());
    out.name = utf8(show.value(QStringLiteral("name")).toString());
    out.metaJson = metaToJson(show.value(QStringLiteral("meta")));

    for (const QVariant &cv : show.value(QStringLiteral("categories")).toList())
        out.categories.push_back(categoryFromVariant(cv.toMap()));

    for (const QVariant &tv : show.value(QStringLiteral("templates")).toList())
        out.templates.push_back(templateFromVariant(tv.toMap()));

    for (const QVariant &ov : show.value(QStringLiteral("overlays")).toList())
        out.overlays.push_back(overlayFromVariant(ov.toMap()));

    for (const QVariant &sv : show.value(QStringLiteral("slides")).toList())
        out.slides.push_back(slideFromVariant(sv.toMap()));
    return out;
}

QVariantMap ShowConverter::toVariant(const bp::Presentation &p)
{
    QVariantList categories;
    for (const bp::Category &c : p.categories)
        categories.append(QVariantMap{
            { QStringLiteral("id"), qstr(c.id) },
            { QStringLiteral("name"), qstr(c.name) },
            { QStringLiteral("contentType"), qstr(c.contentType) },
            { QStringLiteral("templateId"), qstr(c.templateId) },
            { QStringLiteral("outputs"), toQList(c.outputs) },
            { QStringLiteral("meta"), metaFromJson(c.metaJson) },
        });

    QVariantList templates;
    for (const bp::SlideTemplate &t : p.templates)
        templates.append(templateToVariant(t));

    QVariantList overlays;
    for (const bp::Overlay &o : p.overlays)
        overlays.append(QVariantMap{
            { QStringLiteral("id"), qstr(o.id) },
            { QStringLiteral("name"), qstr(o.name) },
            { QStringLiteral("scope"), scopeToString(o.scope) },
            { QStringLiteral("targetId"), qstr(o.targetId) },
            { QStringLiteral("outputs"), toQList(o.outputs) },
            { QStringLiteral("enabled"), o.enabled },
            { QStringLiteral("blocks"), blocksToVariant(o.blocks) },
            { QStringLiteral("meta"), metaFromJson(o.metaJson) },
        });

    QVariantList slides;
    for (const bp::Slide &s : p.slides) {
        const QVariantMap meta = metaFromJson(s.metaJson);
        slides.append(QVariantMap{
            { QStringLiteral("id"), qstr(s.id) },
            { QStringLiteral("title"), qstr(s.title) },
            { QStringLiteral("tag"), s.tags.empty() ? QString() : qstr(s.tags.front()) },
            { QStringLiteral("tagColor"), meta.value(QStringLiteral("tagColor")).toString() },
            { QStringLiteral("line1"), meta.value(QStringLiteral("line1")).toString() },
            { QStringLiteral("line2"), meta.value(QStringLiteral("line2")).toString() },
            { QStringLiteral("ref"), meta.value(QStringLiteral("ref")).toString() },
            { QStringLiteral("showCategory"), meta.value(QStringLiteral("showCategory")).toString() },
            { QStringLiteral("templateOverride"), meta.value(QStringLiteral("templateOverride")).toBool() },
            { QStringLiteral("background"), qstr(s.background) },
            { QStringLiteral("filters"), QVariantMap{
                { QStringLiteral("hueRotate"), s.filters.hueRotateDeg },
                { QStringLiteral("invert"), s.filters.invert },
                { QStringLiteral("blur"), s.filters.blurPx },
                { QStringLiteral("grayscale"), s.filters.grayscale },
                { QStringLiteral("brightness"), s.filters.brightness },
                { QStringLiteral("contrast"), s.filters.contrast },
                { QStringLiteral("saturate"), s.filters.saturate } } },
            { QStringLiteral("categoryId"), qstr(s.categoryId) },
            { QStringLiteral("blocks"), blocksToVariant(s.blocks) },
            { QStringLiteral("body"), qstr(s.text) },
            { QStringLiteral("fill"), fillFromMeta(meta) },
            { QStringLiteral("nextTimer"), s.durationMs / 1000.0 },
            { QStringLiteral("hidden"), s.hidden },                                   // disabled: skipped when played
            { QStringLiteral("transitionKind"), transitionName(s.transitionIn) },
            { QStringLiteral("transitionMs"), s.transitionMs },
            { QStringLiteral("outputs"), meta.value(QStringLiteral("outputs")).toList() },   // the outputs it is for (empty = all)
        });
    }
    return {
        { QStringLiteral("id"), qstr(p.id) },
        { QStringLiteral("name"), qstr(p.name) },
        { QStringLiteral("categories"), categories },
        { QStringLiteral("templates"), templates },
        { QStringLiteral("overlays"), overlays },
        { QStringLiteral("meta"), metaFromJson(p.metaJson) },
        { QStringLiteral("slides"), slides },
    };
}