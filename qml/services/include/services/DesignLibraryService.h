#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace bps::library {
class DesignLibrary;
struct Design;
}

class QQmlEngine;
class QJSEngine;

class DesignLibraryService : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList categories READ categories NOTIFY changed)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY changed)
    Q_PROPERTY(int unlabeledCount READ unlabeledCount NOTIFY changed)
    // The filter value that means "in no category" (pass it to designs()).
    Q_PROPERTY(QString unlabeledFilter READ unlabeledFilter CONSTANT)
    // The icon names a category can be given (all drawn by IconGlyph).
    Q_PROPERTY(QStringList categoryIcons READ categoryIcons CONSTANT)
    // "overlay" or "template" - what this instance manages (used in messages and titles).
    Q_PROPERTY(QString noun READ noun CONSTANT)
    // Block kinds this library's designs may hold beyond the ones every slide has ("vignette", "corners" for overlays):
    // declared by the ENGINE, so the Edit screen's Add menu follows the library and not a name check in QML.
    Q_PROPERTY(QStringList extraBlockKinds READ extraBlockKinds CONSTANT)

public:
    static DesignLibraryService &overlays();
    static DesignLibraryService &templates();

    ~DesignLibraryService() override;

    QString noun() const;
    QStringList extraBlockKinds() const;

    QVariantList categories() const;
    int totalCount() const;
    int unlabeledCount() const;
    QString unlabeledFilter() const;
    QStringList categoryIcons() const;

    // The designs in name order. `filter`: "" = all, unlabeledFilter = in no category, else a
    // category id. With `query`, only those whose name matches it (every word, any order, best first).
    Q_INVOKABLE QVariantList designs(const QString &filter = QString(), const QString &query = QString()) const;
    Q_INVOKABLE QVariantMap design(const QString &id) const;

    // Each returns false (and shows a toast saying why) when the engine refuses.
    // createDesign / duplicateDesign return the new design's id, "" when refused.
    Q_INVOKABLE QString createDesign(const QString &name = QString(), const QString &categoryId = QString());
    Q_INVOKABLE QString duplicateDesign(const QString &id);
    Q_INVOKABLE bool renameDesign(const QString &id, const QString &name);
    Q_INVOKABLE bool setDesignColor(const QString &id, const QString &color);
    Q_INVOKABLE bool setDesignCategory(const QString &id, const QString &categoryId);
    // Templates: the kind of show category it fits ("song", "notes", "scripture"...).
    Q_INVOKABLE bool setDesignContentType(const QString &id, const QString &contentType);
    Q_INVOKABLE bool setDesignLocked(const QString &id, bool locked);
    Q_INVOKABLE bool setDesignPlaceUnderSlide(const QString &id, bool under);
    Q_INVOKABLE bool setDesignDuration(const QString &id, double seconds);
    Q_INVOKABLE bool deleteDesign(const QString &id);
    Q_INVOKABLE QString createCategory(const QString &name, const QString &icon = QString());
    Q_INVOKABLE bool renameCategory(const QString &id, const QString &name);
    Q_INVOKABLE bool deleteCategory(const QString &id);
    // Brings back the shipped designs that were deleted. Returns how many came back (the caller says so).
    Q_INVOKABLE int restoreDefaults();
    // Re-reads the library from its file — used after a backup was restored over
    // it. No-op before the library was ever built.
    void reload();

    // ---- content: what the Edit screen's canvas edits while this design is open ----
    // Replaces the whole canvas (background + blocks) - the flush when an edit settles.
    Q_INVOKABLE bool setDesignBlocks(const QString &id, const QString &background, const QVariantList &blocks);
    // Engine-first block edits: the ENGINE issues the block id; addBlock / duplicateBlock return it ("" when refused).
    Q_INVOKABLE QString addBlock(const QString &id, const QVariantMap &block, const QString &above = QString());
    Q_INVOKABLE QString duplicateBlock(const QString &id, const QString &blockKey);
    Q_INVOKABLE bool removeBlock(const QString &id, const QString &blockKey);
    Q_INVOKABLE QVariantMap block(const QString &id, const QString &blockKey) const;

signals:
    void changed();

protected:
    explicit DesignLibraryService(QObject *parent = nullptr);
    void open(bool forTemplates, QString noun, QString toastChannel);

private:
    // Builds the library from the engine's platform layer and reads it (the boot-retry target).
    void loadLibrary(bool forTemplates);
    // A toast on this library's channel.
    void report(const QString &message, const QString &level = QStringLiteral("error")) const;
    // Runs an engine edit that returns Result<void>: tells the UI it changed, or reports why it did not.
    template <typename Edit>
    bool apply(Edit &&edit);
    // A design as the QML shape.
    static QVariantMap toMap(const bps::library::Design &design);

    std::unique_ptr<bps::library::DesignLibrary> library_;
    QString noun_;
    QString toastChannel_;
};

// The two QML singletons the dock's tabs bind to.
class OverlayLibraryService : public DesignLibraryService
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    friend class DesignLibraryService;

public:
    static OverlayLibraryService &instance();
    static OverlayLibraryService *create(QQmlEngine *engine, QJSEngine *jsEngine);

private:
    OverlayLibraryService();
};

class TemplateLibraryService : public DesignLibraryService
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    friend class DesignLibraryService;

public:
    static TemplateLibraryService &instance();
    static TemplateLibraryService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    Q_INVOKABLE bool metadataTokenResolves(const QString &contentType, const QString &token) const;

private:
    TemplateLibraryService();
};