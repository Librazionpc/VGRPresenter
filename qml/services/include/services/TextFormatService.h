#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class TextFormatService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    static TextFormatService &instance();
    static TextFormatService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    // `text` with every non-blank line marked by `style`'s marker ("disc" -> "• ", "decimal" -> "1. " ...). "", "none" or an
    // unknown style returns the text unchanged.
    Q_INVOKABLE QString applyList(const QString &text, const QString &style) const;
    // Every list style, "none" first: [{ key, label, sample }] (`sample` = its first marker, for a picker chip).
    Q_INVOKABLE QVariantList listStyles() const;

private:
    explicit TextFormatService(QObject *parent = nullptr) : QObject(parent) {}
};