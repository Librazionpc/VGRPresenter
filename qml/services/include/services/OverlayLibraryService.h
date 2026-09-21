#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace bps::overlays {
class OverlayLibrary;
}

class QQmlEngine;
class QJSEngine;

// The UI's window onto the OVERLAY LIBRARY - the overlays the user can lay over slides and the categories
// they are filed under. The ENGINE owns all of it (bps::overlays::OverlayLibrary: the defaults, the rules,
// the JSON file); this service only turns engine results into QML shapes and reports refusals as toasts.
//
// An overlay is { id, name, color, category ("" = unlabeled), isDefault, locked, displayDuration, elements }.
// Its elements are drawn on a 1920 x 1080 canvas: { kind ("box" | "text" | "clock" | "vignette" | "corners"),
// x, y, width, height, background, radius, borderWidth, borderColor, text, textColor, fontSize, bold,
// uppercase, align, analog, inset }.
//
// A category is { id, name, icon, isDefault, count }.
class OverlayLibraryService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QVariantList categories READ categories NOTIFY changed)
    // Every overlay / the ones in no category.
    Q_PROPERTY(int totalCount READ totalCount NOTIFY changed)
    Q_PROPERTY(int unlabeledCount READ unlabeledCount NOTIFY changed)
    // The filter value that means "in no category" (pass it to overlays()).
    Q_PROPERTY(QString unlabeledFilter READ unlabeledFilter CONSTANT)
    // The icon names a category can be given (all drawn by IconGlyph).
    Q_PROPERTY(QStringList categoryIcons READ categoryIcons CONSTANT)

public:
    static OverlayLibraryService &instance();
    static OverlayLibraryService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    ~OverlayLibraryService() override;

    QVariantList categories() const;
    int totalCount() const;
    int unlabeledCount() const;
    QString unlabeledFilter() const;
    QStringList categoryIcons() const;

    // The overlays in name order. `filter`: "" = all, unlabeledFilter = in no category, else a category id.
    // With `query`, only those whose name matches it (every word, any order, best first).
    Q_INVOKABLE QVariantList overlays(const QString &filter = QString(), const QString &query = QString()) const;
    Q_INVOKABLE QVariantMap overlay(const QString &id) const;

    // Each returns false (and shows a toast saying why) when the engine refuses.
    // createOverlay / duplicateOverlay return the new overlay's id, "" when refused.
    Q_INVOKABLE QString createOverlay(const QString &name = QString(), const QString &categoryId = QString());
    Q_INVOKABLE QString duplicateOverlay(const QString &id);
    Q_INVOKABLE bool renameOverlay(const QString &id, const QString &name);
    Q_INVOKABLE bool setOverlayColor(const QString &id, const QString &color);
    Q_INVOKABLE bool setOverlayCategory(const QString &id, const QString &categoryId);
    Q_INVOKABLE bool setOverlayLocked(const QString &id, bool locked);
    Q_INVOKABLE bool deleteOverlay(const QString &id);
    Q_INVOKABLE QString createCategory(const QString &name, const QString &icon = QString());
    Q_INVOKABLE bool renameCategory(const QString &id, const QString &name);
    Q_INVOKABLE bool deleteCategory(const QString &id);
    // Brings back the default overlays that were deleted. Returns how many.
    Q_INVOKABLE int restoreDefaults();

signals:
    void changed();

private:
    explicit OverlayLibraryService(QObject *parent = nullptr);

    std::unique_ptr<bps::overlays::OverlayLibrary> library_;
};
