#pragma once

#include <QObject>
#include <QPointF>
#include <QString>
#include <QVariantList>

class QQuickWindow;

class SelfTestDriver : public QObject
{
    Q_OBJECT
public:
    explicit SelfTestDriver(QObject *parent = nullptr);

    // The window whose local coordinate space move/click/grab speak. Set
    // right after engine load, from main.cpp.
    void setWindow(QQuickWindow *window);

    // Raise this window and make it foreground — what a synthetic click needs
    // to actually be DELIVERED to this window's controls (see the .cpp).
    Q_INVOKABLE void activate();
    Q_INVOKABLE void move(double x, double y);
    Q_INVOKABLE void click(double x, double y);
    Q_INVOKABLE void press(double x, double y);
    Q_INVOKABLE void release(double x, double y);
    // Real key events (press+release) to the focused object — types into whatever
    // TextInput has focus, exercising the app's own key/text pipeline.
    Q_INVOKABLE void type(const QString &text);
    // Deterministic focus: calls forceActiveFocus() on the named item (the synthetic
    // OS click needs window-foreground luck; this does not).
    Q_INVOKABLE bool focusItem(const QString &objectName);
    Q_INVOKABLE QPointF itemCenter(const QString &objectName);
    Q_INVOKABLE QString cursorAt(double x, double y);
    Q_INVOKABLE bool itemVisible(const QString &objectName);
    Q_INVOKABLE void clickItem(const QString &objectName);
    Q_INVOKABLE bool grab(const QString &itemName, const QString &pngPath);
    // Resolve an item by objectName (no action) — for scenarios that poke
    // properties the invokables don't cover (e.g. a Flickable's contentY).
    Q_INVOKABLE QObject *findItem(const QString &objectName);
    Q_INVOKABLE QVariantList windows() const;
    Q_INVOKABLE void quit();

private:
    QQuickWindow *m_window = nullptr;
};