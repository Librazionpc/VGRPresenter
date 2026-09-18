#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <qqml.h>

// One output style — the presentation "theme" an output renders with.
// FreeShow models these in Settings · Styles (Styles.svelte + types/Settings):
// name + resolution, selectable per output. Ours adds just enough to back a
// real Edit Style dialog (VGRPresenter_Settings_Outputs_Edit.qml's
// style_dialog reference): a content type, which layout template applies,
// and a background color. Deliberately NOT video/audio bus routing or
// per-output window behavior (always-on-top, locked, transparent, geometry/
// cropping) — those are per-OUTPUT concerns (a shared style applied to two
// outputs shouldn't force them to share window behavior), so they live on
// OutputListModel instead, not duplicated here.
struct StyleItem
{
    QString name;
    QString res;
    // "shows" | "media" | "scripture" — VGRPresenter's own reduced set,
    // not FreeShow's full Shows/Songs/Scripture/PPT/Video.
    QString contentType = QStringLiteral("shows");
    // "lowerThird" | "title" | "sidebar" | "bottomBar" | "fullscreen" — see
    // TemplatePickerModal.qml's own `templates` list, the single source of
    // truth for keys/names/descriptions this just stores a key into.
    QString templateKey = QStringLiteral("lowerThird");
    // Same "transparent" convention as BackgroundColorModal's
    // transparentValue / CanvasItemStyle.backgroundColor.
    QString backgroundColor = QStringLiteral("transparent");
};

// QML singleton backing the Settings · Outputs "Styles" card. Singleton so a
// future second consumer (output monitor, presenter view) reads the same
// roster without threading a property through the tree.
class StyleListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        ResRole,
        ContentTypeRole,
        TemplateKeyRole,
        BackgroundColorRole,
    };
    Q_ENUM(Role)

    explicit StyleListModel(QObject *parent = nullptr);
    ~StyleListModel() override;

    // OutputListModel resolves style names through this; QML_SINGLETON gives
    // us exactly one engine-owned instance, so remember it here.
    static StyleListModel *instance() { return s_instance; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addStyle();
    Q_INVOKABLE void removeStyle(int index);
    Q_INVOKABLE void renameStyle(int index, const QString &name);
    Q_INVOKABLE void setResolution(int index, const QString &res);
    Q_INVOKABLE void setContentType(int index, const QString &contentType);
    Q_INVOKABLE void setTemplateKey(int index, const QString &templateKey);
    Q_INVOKABLE void setBackgroundColor(int index, const QString &color);

    // Snapshot for the Edit Style dialog — one QVariantMap with the same
    // keys as the role names, so QML doesn't keep parallel state (same
    // convention as OutputListModel::getOutput).
    Q_INVOKABLE QVariantMap getStyle(int index) const;

private:
    QList<StyleItem> m_styles;

    static QPointer<StyleListModel> s_instance;
};
