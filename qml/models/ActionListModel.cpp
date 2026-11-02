#include "ActionListModel.h"

#include "services/EngineBridge.h"
#include "modules/project/ActionStore.hpp"

#include <QJSEngine>
#include <QStringList>
#include <algorithm>
#include <iterator>

QPointer<ActionListModel> ActionListModel::s_instance = nullptr;

ActionListModel *ActionListModel::instance()
{
    static ActionListModel s;
    return &s;
}

ActionListModel *ActionListModel::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    ActionListModel *s = instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}

ActionListModel::ActionListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    Q_ASSERT_X(!s_instance || s_instance == this, "ActionListModel",
               "a second ActionListModel was constructed — instance() would "
               "silently rebind and every C++ reader would hold the wrong one");
    s_instance = this;

    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged, this, [this] {
        if (EngineBridge::instance().booted())
            reloadFromStore();
    });
    reloadFromStore();
}

ActionListModel::~ActionListModel()
{
    if (s_instance == this)
        s_instance = nullptr;
}

void ActionListModel::reloadFromStore()
{
    beginResetModel();
    m_actions.clear();
    for (const bps::project::StoredAction &a : bps::project::ActionStore::Instance().Get()) {
        ActionItem item;
        item.id = QString::fromStdString(a.id);
        item.name = QString::fromStdString(a.name);
        item.icon = QString::fromStdString(a.icon);
        item.effectId = QString::fromStdString(a.effectId);
        item.effectValue = QString::fromStdString(a.effectValue);
        item.shortcut = QString::fromStdString(a.shortcut);
        item.trigger = QString::fromStdString(a.trigger);
        item.visibleInFunctions = a.visibleInFunctions;
        for (const auto& step : a.steps) {
            item.steps.append(QVariantMap{
                { "effectId", QString::fromStdString(step.effectId) },
                { "effectValue", QString::fromStdString(step.effectValue) },
            });
        }
        m_actions.append(item);
    }
    endResetModel();
    emit rosterChanged();
}

void ActionListModel::save()
{
    std::vector<bps::project::StoredAction> stored;
    stored.reserve(m_actions.size());
    for (const ActionItem &item : m_actions) {
        bps::project::StoredAction a;
        a.id = item.id.toStdString();
        a.name = item.name.toStdString();
        a.icon = item.icon.toStdString();
        a.effectId = item.effectId.toStdString();
        a.effectValue = item.effectValue.toStdString();
        a.shortcut = item.shortcut.toStdString();
        a.trigger = item.trigger.toStdString();
        a.visibleInFunctions = item.visibleInFunctions;
        for (const QVariant& value : item.steps) {
            const QVariantMap step = value.toMap();
            const QString effectId = step.value(QStringLiteral("effectId")).toString();
            if (effectId.isEmpty())
                continue;
            a.steps.push_back({ effectId.toStdString(),
                                step.value(QStringLiteral("effectValue")).toString().toStdString() });
        }
        if (!a.steps.empty()) {
            a.effectId = a.steps.front().effectId;
            a.effectValue = a.steps.front().effectValue;
        }
        stored.push_back(std::move(a));
    }
    (void)bps::project::ActionStore::Instance().Save(stored);
    emit rosterChanged();
}

int ActionListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_actions.size();
}

QVariant ActionListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_actions.size())
        return {};

    const ActionItem &item = m_actions.at(index.row());
    switch (role) {
    case IdRole: return item.id;
    case NameRole: return item.name;
    case IconRole: return item.icon;
    case EffectIdRole: return item.effectId;
    case EffectValueRole: return item.effectValue;
    case ShortcutRole: return item.shortcut;
    case TriggerRole: return item.trigger;
    case StepsRole: return item.steps;
    case VisibleInFunctionsRole: return item.visibleInFunctions;
    default: return {};
    }
}

QHash<int, QByteArray> ActionListModel::roleNames() const
{
    return {
        { IdRole, "id" },
        { NameRole, "name" },
        { IconRole, "icon" },
        { EffectIdRole, "effectId" },
        { EffectValueRole, "effectValue" },
        { ShortcutRole, "shortcut" },
        { TriggerRole, "trigger" },
        { StepsRole, "steps" },
        { VisibleInFunctionsRole, "visibleInFunctions" },
    };
}

int ActionListModel::addAction()
{
    const int row = m_actions.size();
    beginInsertRows(QModelIndex(), row, row);
    ActionItem item;
    int max = 0;
    for (const ActionItem &a : m_actions) {
        if (!a.id.startsWith(QLatin1String("a")))
            continue;
        bool ok = false;
        const int n = a.id.mid(1).toInt(&ok);
        if (ok)
            max = std::max(max, n);
    }
    item.id = QStringLiteral("a%1").arg(max + 1);
    m_actions.append(item);
    endInsertRows();
    save();
    return row;
}

void ActionListModel::removeAction(int index)
{
    if (index < 0 || index >= m_actions.size())
        return;
    beginRemoveRows(QModelIndex(), index, index);
    m_actions.removeAt(index);
    endRemoveRows();
    save();
}

void ActionListModel::setName(int index, const QString &name)
{
    if (index < 0 || index >= m_actions.size())
        return;
    const QString trimmed = name.trimmed();
    if (m_actions[index].name == trimmed)
        return;
    m_actions[index].name = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { NameRole });
    save();
}

void ActionListModel::setIcon(int index, const QString &icon)
{
    if (index < 0 || index >= m_actions.size())
        return;
    if (m_actions[index].icon == icon)
        return;
    m_actions[index].icon = icon;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { IconRole });
    save();
}

void ActionListModel::setEffect(int index, const QString &effectId, const QString &effectValue)
{
    if (index < 0 || index >= m_actions.size())
        return;
    QVariantList steps = m_actions[index].steps;
    if (steps.isEmpty())
        steps.append(QVariantMap{{ "effectId", effectId }, { "effectValue", effectValue }});
    else
        steps[0] = QVariantMap{{ "effectId", effectId }, { "effectValue", effectValue }};
    setSteps(index, steps);
}

void ActionListModel::setSteps(int index, const QVariantList& steps)
{
    if (index < 0 || index >= m_actions.size())
        return;
    QVariantList clean;
    clean.reserve(steps.size());
    for (const QVariant& value : steps) {
        const QVariantMap step = value.toMap();
        const QString effectId = step.value(QStringLiteral("effectId")).toString();
        if (effectId.isEmpty())
            continue;
        clean.append(QVariantMap{
            { "effectId", effectId },
            { "effectValue", step.value(QStringLiteral("effectValue")).toString() },
        });
    }
    m_actions[index].steps = clean;
    m_actions[index].effectId = clean.isEmpty()
        ? QString{} : clean.front().toMap().value(QStringLiteral("effectId")).toString();
    m_actions[index].effectValue = clean.isEmpty()
        ? QString{} : clean.front().toMap().value(QStringLiteral("effectValue")).toString();
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { StepsRole, EffectIdRole, EffectValueRole });
    save();
}

void ActionListModel::setVisibleInFunctions(int index, bool visible)
{
    if (index < 0 || index >= m_actions.size()
        || m_actions[index].visibleInFunctions == visible)
        return;
    m_actions[index].visibleInFunctions = visible;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { VisibleInFunctionsRole });
    save();
}

void ActionListModel::setShortcut(int index, const QString &shortcut)
{
    if (index < 0 || index >= m_actions.size())
        return;
    const QString trimmed = shortcut.trimmed();
    for (int i = 0; i < m_actions.size(); ++i) {
        if (i != index && !trimmed.isEmpty() && m_actions.at(i).shortcut == trimmed)
            return;
    }
    if (m_actions[index].shortcut == trimmed)
        return;
    m_actions[index].shortcut = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ShortcutRole });
    save();
}

void ActionListModel::setTrigger(int index, const QString &triggerId)
{
    if (index < 0 || index >= m_actions.size())
        return;
    const QString id = triggerId.trimmed();
    if (m_actions[index].trigger == id)
        return;
    m_actions[index].trigger = id;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { TriggerRole });
    save();
}

QVariantMap ActionListModel::getAction(int index) const
{
    if (index < 0 || index >= m_actions.size())
        return {};
    const ActionItem &item = m_actions.at(index);
    return {
        { "id", item.id },
        { "name", item.name },
        { "icon", item.icon },
        { "effectId", item.effectId },
        { "effectValue", item.effectValue },
        { "shortcut", item.shortcut },
        { "trigger", item.trigger },
        { "steps", item.steps },
        { "visibleInFunctions", item.visibleInFunctions },
    };
}

int ActionListModel::rowForId(const QString &id) const
{
    for (int i = 0; i < m_actions.size(); ++i)
        if (m_actions.at(i).id == id)
            return i;
    return -1;
}

QVariantMap ActionListModel::shortcutMap() const
{
    QVariantMap out;
    for (const ActionItem &item : m_actions)
        if (!item.shortcut.isEmpty())
            out.insert(item.shortcut, item.id);
    return out;
}

namespace {
struct CatalogEntry {
    const char *id;
    const char *name;
    const char *icon;
    const char *section;
    bool needsGroup;
};
constexpr CatalogEntry kCatalog[] = {
    { "go_live",            "Go live",              "play",         "GO LIVE", false },
    { "stop",                "Stop",                 "stop",         "GO LIVE", false },
    { "blackout",            "Blackout",             "ban",          "GO LIVE", false },
    { "end_blackout",        "End blackout",         "unlocked",     "GO LIVE", false },

    { "clear_on_air_slide",  "Clear slide",          "trash",        "CLEAR", false },
    { "clear_all",           "Clear all",            "trash",        "CLEAR", false },
    { "clear_media",         "Clear background",     "trash",        "CLEAR", false },
    { "clear_overlays",      "Clear overlays",       "trash",        "CLEAR", false },
    { "restore_cleared",     "Restore output",        "cloudSync",   "CLEAR", false },

    { "next_slide",          "Next slide",           "chevronRight", "NAVIGATE", false },
    { "previous_slide",      "Previous slide",       "chevronLeft",  "NAVIGATE", false },
    { "jump_to_group",       "Play specific group",  "listBullets",  "NAVIGATE", true },

    { "media_toggle_play",   "Toggle playing media", "playCircle",   "MEDIA", false },
    { "media_toggle_muted",  "Mute",                 "volume2",      "MEDIA", false },
};

struct CatalogAlias { const char *referenceId; const char *realId; };
constexpr CatalogAlias kCatalogAliases[] = {
    { "clear_slide",          "clear_on_air_slide" },
    { "clear_background",     "clear_media" },
    { "restore_output",       "restore_cleared" },
    { "toggle_playing_media", "media_toggle_play" },
    { "id_select_group",      "jump_to_group" },
    { "mute_output",          "media_toggle_muted" },
};

// Preserve the reference catalog's action ids and section order; actions
// without a local dispatcher remain visible but unavailable until implemented.
struct ReferenceCatalogEntry { const char *id; const char *section; bool common; };
constexpr ReferenceCatalogEntry kReferenceCatalog[] = {
    {"id_select_project","PROJECT",false},{"index_select_project","PROJECT",false},{"next_project_item","PROJECT",false},{"previous_project_item","PROJECT",false},{"index_select_project_item","PROJECT",false},{"name_start_project_item","PROJECT",false},{"mark_active_as_played","PROJECT",false},
    {"id_select_show","SHOW",false},{"name_select_show","SHOW",false},{"start_show","SHOW",true},{"set_template","SHOW",false},{"set_next_slide_timer","SHOW",false},{"change_slide_timeline_speed","SHOW",false},{"transpose_show_up","SHOW",false},{"transpose_show_down","SHOW",false},
    {"next_slide","SLIDE",false},{"previous_slide","SLIDE",false},{"random_slide","SLIDE",false},{"index_select_slide","SLIDE",false},{"name_select_slide","SLIDE",false},{"id_select_group","SLIDE",true},{"disable_slide","SLIDE",false},
    {"restore_output","CLEAR",true},{"clear_all","CLEAR",true},{"clear_background","CLEAR",true},{"clear_slide","CLEAR",true},{"clear_overlays","CLEAR",true},{"clear_overlay","CLEAR",false},{"clear_audio","CLEAR",true},{"clear_next_timer","CLEAR",false},{"clear_scene","CLEAR",false},{"clear_drawing","CLEAR",false},
    {"start_camera","MEDIA",true},{"start_screen","MEDIA",true},{"toggle_playing_media","MEDIA",true},{"id_start_effect","MEDIA",true},{"id_select_overlay","MEDIA",true},
    {"start_scene","OUTPUT",false},{"start_recording","OUTPUT",true},{"stop_recording","OUTPUT",false},{"toggle_output_lock","OUTPUT",false},{"toggle_output_windows","OUTPUT",false},{"toggle_output_window","OUTPUT",false},{"change_output_style","OUTPUT",true},{"change_stage_output_layout","OUTPUT",true},{"change_stage_connections_layout","OUTPUT",false},
    {"change_volume","AUDIO",false},{"mute_output","AUDIO",true},{"start_audio_stream","AUDIO",true},{"toggle_audio_recording","AUDIO",false},{"toggle_icecast","AUDIO",false},{"start_playlist","AUDIO",true},{"playlist_next","AUDIO",false},{"start_metronome","AUDIO",false},{"start_audio_effect","AUDIO",true},{"start_microphone","AUDIO",true},{"stop_microphone","AUDIO",true},
    {"id_start_timer","TIMERS",true},{"pause_active_timers","TIMERS",true},{"stop_active_timers","TIMERS",true},
    {"start_timeline","TIMELINE",false},{"stop_timeline","TIMELINE",false},{"pause_timeline","TIMELINE",false},{"set_timeline_time","TIMELINE",false},
    {"change_variable","FUNCTIONS",true},{"change_draw_zoom","DRAW",false},{"emit_data","EMIT DATA",true},
    {"interaction_start","INTERACTIONS",false},{"interaction_stop","INTERACTIONS",false},{"interaction_next","INTERACTIONS",false},{"interaction_previous","INTERACTIONS",false},
    {"toggle_log_song_usage","OTHER",false},
    {"run_action","ACTION",false},{"toggle_action","ACTION",false},{"wait","ACTION",true},
};

QString catalogName(const QString &id)
{
    if (id == QLatin1String("mark_active_as_played")) return QStringLiteral("Mark as played");
    if (id == QLatin1String("transpose_show_up")) return QStringLiteral("Transpose up");
    if (id == QLatin1String("transpose_show_down")) return QStringLiteral("Transpose down");
    if (id == QLatin1String("change_slide_timeline_speed")) return QStringLiteral("Change slide timeline speed");
    if (id == QLatin1String("toggle_icecast")) return QStringLiteral("Toggle Icecast");
    if (id == QLatin1String("random_slide")) return QStringLiteral("Play random slide");
    if (id == QLatin1String("index_select_slide")) return QStringLiteral("Play slide at index");
    if (id == QLatin1String("name_select_slide")) return QStringLiteral("Play slide with name");
    if (id == QLatin1String("id_select_overlay")) return QStringLiteral("Start overlay");
    if (id == QLatin1String("clear_next_timer")) return QStringLiteral("Clear next slide timer");
    if (id == QLatin1String("clear_overlay")) return QStringLiteral("Clear specific overlay");
    if (id == QLatin1String("interaction_start")) return QStringLiteral("Interactions: Start");
    if (id == QLatin1String("interaction_stop")) return QStringLiteral("Interactions: Stop");
    if (id == QLatin1String("interaction_next")) return QStringLiteral("Interactions: Next");
    if (id == QLatin1String("interaction_previous")) return QStringLiteral("Interactions: Previous");
    if (id == QLatin1String("toggle_log_song_usage")) return QStringLiteral("Log song usage to a file");
    if (id.startsWith(QLatin1String("index_select_")))
        return QStringLiteral("Open %1 at index").arg(catalogName(id.mid(13)));
    if (id.startsWith(QLatin1String("id_select_")))
        return QStringLiteral("Open %1").arg(catalogName(id.mid(10)));
    if (id.startsWith(QLatin1String("name_select_")))
        return QStringLiteral("Open %1 by name").arg(catalogName(id.mid(12)));
    if (id.startsWith(QLatin1String("name_start_")))
        return QStringLiteral("Start %1 by name").arg(catalogName(id.mid(11)));
    if (id.startsWith(QLatin1String("id_start_")))
        return QStringLiteral("Start %1").arg(catalogName(id.mid(9)));
    QString name = id;
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    QStringList words = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (QString &word : words) {
        if (!word.isEmpty()) word[0] = word.at(0).toUpper();
    }
    return words.join(QLatin1Char(' '));
}

QVariantMap catalogMap(const QString &id, const QString &section, bool common)
{
    QString realId = id;
    for (const CatalogAlias &a : kCatalogAliases) {
        if (id == QLatin1String(a.referenceId)) {
            realId = QString::fromUtf8(a.realId);
            break;
        }
    }
    for (const CatalogEntry &e : kCatalog) {
        if (realId == QLatin1String(e.id)) {
            return {{"id", realId}, {"name", QString::fromUtf8(e.name)},
                    {"icon", QString::fromUtf8(e.icon)}, {"section", section},
                    {"needsGroup", e.needsGroup}, {"supported", true}, {"common", common}};
        }
    }
    return {{"id", id}, {"name", catalogName(id)}, {"icon", QStringLiteral("flag")},
            {"section", section}, {"supported", false}, {"common", common}};
}
} // namespace

namespace {
struct TriggerEntry {
    const char *id;
    const char *name;
    const char *icon;
    bool common;
};
constexpr TriggerEntry kTriggers[] = {
    { "startup",                  "Activate on startup",                       "next",        false },
    { "save",                     "Activate on save",                          "download",    false },
    { "output_change",            "Activate when output changes",               "presentation", true },
    { "show_created",             "Activate when show is created",              "fileText",    false },
    { "show_opened",              "Activate when show is opened",               "fileText",    false },
    { "slide_click",              "Activate on slide click",                    "fileText",    false },
    { "group_start",              "Activate when group is started",             "listBullets", true },
    { "pdf_start",                "Activate when PDF is started",               "fileText",    false },
    { "slide_cleared",            "Activate when slide is cleared",             "fileText",    true },
    { "background_cleared",       "Activate when background is cleared",        "image",       true },
    { "scripture_start",          "Activate when scripture is started",         "bookOpen",    true },
    { "video_start_any",          "Activate when video is starting: Any",       "play",        true },
    { "video_start_foreground",   "Activate when video is starting: Foreground", "play",        false },
    { "video_start_background",   "Activate when video is starting: Background", "play",        false },
    { "video_end",                "Activate when video is ending",              "play",        true },
    { "audio_start",              "Activate when audio is starting",            "music",       true },
    { "audio_end",                "Activate when audio is ending",              "music",       false },
    { "audio_playlist_end",       "Activate when audio playlist has ended",     "music",       true },
    { "timer_start",              "Activate when timer is starting",            "clock",       true },
    { "timer_end",                "Activate when timer is ending",              "clock",       true },
    { "midi",                     "Activate by MIDI signal",                    "activity",    true },
};
} // namespace

QVariantList ActionListModel::triggerEntries() const
{
    QVariantList out;
    out.reserve(int(std::size(kTriggers)));
    for (const TriggerEntry &t : kTriggers) {
        out.append(QVariantMap{
            { "id", QString::fromUtf8(t.id) },
            { "name", QString::fromUtf8(t.name) },
            { "icon", QString::fromUtf8(t.icon) },
            { "common", t.common },
        });
    }
    return out;
}

QVariantList ActionListModel::catalogEntries() const
{
    QVariantList out;
    for (const ReferenceCatalogEntry &e : kReferenceCatalog) {
        out.append(catalogMap(QString::fromUtf8(e.id), QString::fromUtf8(e.section), e.common));
    }
    out.append(catalogMap(QStringLiteral("go_live"), QStringLiteral("VGRPRESENTER"), true));
    out.append(catalogMap(QStringLiteral("stop"), QStringLiteral("VGRPRESENTER"), true));
    out.append(catalogMap(QStringLiteral("blackout"), QStringLiteral("VGRPRESENTER"), true));
    out.append(catalogMap(QStringLiteral("end_blackout"), QStringLiteral("VGRPRESENTER"), true));
    return out;
}

QVariantMap ActionListModel::catalogEntry(const QString &effectId) const
{
    if (effectId.isEmpty())
        return {};
    for (const ReferenceCatalogEntry &e : kReferenceCatalog) {
        if (effectId == QLatin1String(e.id))
            return catalogMap(effectId, QString::fromUtf8(e.section), e.common);
    }
    for (const CatalogEntry &e : kCatalog)
        if (effectId == QLatin1String(e.id))
            return catalogMap(effectId, QString::fromUtf8(e.section), true);
    return {};
}