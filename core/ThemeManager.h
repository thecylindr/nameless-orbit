#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QColor>

// Палитра интерфейса — одна тёмная «стеклянная» тема, в цветах значка приложения (синий → фиолетовый,
// как орбита на appicon.svg) и в духе остальной сборки NameLess UI, хоть это и отдельное Qt-приложение
// без доступа к её Quickshell-синглтону Theme.qml. Отдаётся в QML как контекстное свойство "Theme" —
// так же, как ClientManager и остальные классы ядра в drones, без импорта QML-модуля для каждого файла.
class ThemeManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QColor base READ base CONSTANT)
    Q_PROPERTY(QColor surface READ surface CONSTANT)
    Q_PROPERTY(QColor surfaceHigh READ surfaceHigh CONSTANT)
    Q_PROPERTY(QColor accent READ accent CONSTANT)
    Q_PROPERTY(QColor accent2 READ accent2 CONSTANT)
    Q_PROPERTY(QColor text READ text CONSTANT)
    Q_PROPERTY(QColor subtext READ subtext CONSTANT)
    Q_PROPERTY(QColor line READ line CONSTANT)
    Q_PROPERTY(QColor good READ good CONSTANT)
    Q_PROPERTY(QColor warn READ warn CONSTANT)
    Q_PROPERTY(QColor danger READ danger CONSTANT)
    Q_PROPERTY(QString fontFamily READ fontFamily CONSTANT)
    Q_PROPERTY(qreal radius READ radius CONSTANT)

public:
    explicit ThemeManager(QObject *parent = nullptr) : QObject(parent) {}

    QColor base() const { return QColor(0x05, 0x07, 0x0f); }
    QColor surface() const { return QColor(0x12, 0x16, 0x26); }
    QColor surfaceHigh() const { return QColor(0x1b, 0x20, 0x40); }
    QColor accent() const { return QColor(0x0a, 0x84, 0xff); }
    QColor accent2() const { return QColor(0x5e, 0x5c, 0xe6); }
    QColor text() const { return QColor(0xea, 0xf2, 0xff); }
    QColor subtext() const { return QColor(0x9a, 0xa4, 0xc8); }
    QColor line() const { return QColor(255, 255, 255, 30); }
    QColor good() const { return QColor(0x34, 0xc7, 0x59); }
    QColor warn() const { return QColor(0xff, 0xd6, 0x0a); }
    QColor danger() const { return QColor(0xff, 0x45, 0x5a); }
    QString fontFamily() const { return QStringLiteral("Inter"); }
    qreal radius() const { return 18.0; }
};

#endif // THEMEMANAGER_H
