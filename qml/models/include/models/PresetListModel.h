#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QVariantMap>
#include <qqml.h>

// One recording / streaming PRESET (Settings · Recording). A preset is a named,
// reusable configuration of ONE of two kinds:
//
//   "recording"  container (MKV/MP4), encoder, video + audio bitrate, frame rate,
//                resolution, output folder — how a take is written to disk.
//   "streaming"  platform, RTMP server URL + stream key, video + audio bitrate,
//                frame rate, resolution, encoder — where and how a feed is pushed.
//
// Settings · Routing wires an output to a preset (OutputItem::presetId, by the
// preset's STABLE id, exactly how an output wears a style), and that output then
// records / streams with THAT preset's configuration instead of one global page of
// settings. Several presets live side by side.
//
// `config` is a flat map of strings (the same shapes the old page persisted), the
// field set of its kind — see defaultConfig(). The roster is persisted through the
// kernel's PresetStore (DatabaseManager "presets"/"roster") after every mutation,
// and re-read when the engine finishes booting (the kernel store only OPENS during
// Boot, which is after this singleton exists — the same boot gap StyleListModel
// documents).
//
// C++ readers (RoutingService, RecordingService) do NOT go through the singleton:
// QML builds its own instance of these models, so a C++ read of instance() could
// land on a different roster. They use the static lookup() below, which reads the
// kernel store — the one place both sides agree on.
struct PresetItem
{
    QString id;
    QString name;
    QString kind = QStringLiteral("recording");   // "recording" | "streaming"
    QVariantMap config;
};

class PresetListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        KindRole,
        SummaryRole,
        ConfigRole,
    };
    Q_ENUM(Role)

    explicit PresetListModel(QObject *parent = nullptr);
    ~PresetListModel() override;

    static PresetListModel *instance();
    static PresetListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The field set a NEW preset of `kind` starts from.
    Q_INVOKABLE QVariantMap defaultConfig(const QString &kind) const;
    // "" when `config` is acceptable for `kind`, else the sentence that says why
    // (the dialog shows it; add/update refuse with the same text).
    Q_INVOKABLE QString validate(const QString &kind, const QVariantMap &config) const;
    // The same checks per field: { videoBitrate: "...", serverUrl: "..." } — empty when
    // fine. The dialog prints each reason under the field it is about.
    Q_INVOKABLE QVariantMap fieldErrors(const QString &kind, const QVariantMap &config) const;
    // "" when `name` can be used (for the preset at `exceptRow`, -1 when adding), else
    // why not — the dialog shows it next to the field instead of a toast the modal hides.
    Q_INVOKABLE QString validateName(const QString &name, int exceptRow) const;

    // Create a preset. false + a toast when the name is empty / taken or the
    // config is refused — never a silent no-op.
    Q_INVOKABLE bool addPreset(const QString &kind, const QString &name, const QVariantMap &config);
    // Rename + re-configure a preset. The KIND never changes (an output wired to
    // a recording preset must not turn into a stream under it).
    Q_INVOKABLE bool updatePreset(int index, const QString &name, const QVariantMap &config);
    Q_INVOKABLE void duplicatePreset(int index);
    // Removes the preset; every output that wore it goes back to "no preset"
    // (model-side backstop, like StyleListModel::removeStyle).
    Q_INVOKABLE void removePreset(int index);

    // Reads, by row or by stable id. An unknown id answers an empty map / "" / 0.
    Q_INVOKABLE QVariantMap getPreset(int index) const;
    Q_INVOKABLE int rowOfId(const QString &presetId) const;
    Q_INVOKABLE QVariantMap configOf(const QString &presetId) const;
    Q_INVOKABLE QString kindOf(const QString &presetId) const;
    Q_INVOKABLE QString nameOf(const QString &presetId) const;
    Q_INVOKABLE QString summaryOf(const QString &presetId) const;
    // Rows of one kind, as { presetId, name, summary } — what a picker lists.
    Q_INVOKABLE QVariantList presetsOfKind(const QString &kind) const;
    // How many outputs are wired to this preset (Settings · Routing rows).
    Q_INVOKABLE int usageCount(const QString &presetId) const;

    // The one-line description a row / picker prints under the name.
    static QString summaryFor(const QString &kind, const QVariantMap &config);
    // The kernel-store read C++ callers use: { id, name, kind, config } or an
    // empty map when no preset has that id. In-memory map read (no PAL).
    static QVariantMap lookup(const QString &presetId);

public slots:
    // Re-reads the roster from the kernel store (boot finished / store reopened).
    void reloadFromStore();

private:
    void save();
    int nextIdNumber() const;
    bool nameTaken(const QString &name, int exceptRow) const;
    QString uniqueName(const QString &wanted) const;
    // Keeps only the fields the kind owns, fills the rest from the defaults.
    static QVariantMap normalized(const QString &kind, const QVariantMap &config);
    static QVariantMap defaults(const QString &kind);
    // Re-asks Settings · Routing for its nodes (a stream preset's target is part
    // of its output node).
    void refreshRouting();

    QList<PresetItem> m_presets;
    static QPointer<PresetListModel> s_instance;
};
