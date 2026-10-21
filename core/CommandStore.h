#ifndef COMMANDSTORE_H
#define COMMANDSTORE_H

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>

// Список своих команд для RunCommandPlugin: что можно запустить по просьбе доверенного устройства.
// Главное свойство этого списка — он живёт только здесь. Удалённая сторона видит id и название, никогда —
// сам текст команды; попросить выполнить что-то, чего нет в этом списке, с другого устройства нельзя
// в принципе (обработчик плагина ищет по id и не принимает произвольную строку).
struct OrbitCommand
{
    QString id;
    QString name;
    QString icon;       // имя значка Nerd Font (как в NameLess UI) — для единообразия с остальной сборкой
    QString command;    // то, что реально выполнится; наружу не отдаётся
    bool confirmLocally = false;  // перед запуском показать уведомление с кнопкой «Отменить» (5 с)
};

class CommandStore : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList commandsForQml READ commandsForQml NOTIFY changed)

public:
    explicit CommandStore(QObject *parent = nullptr);

    QVector<OrbitCommand> commands() const { return m_commands; }
    const OrbitCommand *find(const QString &id) const;
    QVariantList commandsForQml() const;      // без поля command — то же самое, что видит удалённая сторона

    Q_INVOKABLE void addCommand(const QString &name, const QString &icon, const QString &command, bool confirm);
    Q_INVOKABLE void updateCommand(const QString &id, const QString &name, const QString &icon, const QString &command, bool confirm);
    Q_INVOKABLE void removeCommand(const QString &id);
    Q_INVOKABLE QString commandText(const QString &id) const;   // только для локального редактора; наружу по сети не уходит

signals:
    void changed();

private:
    static QString displayIcon(const OrbitCommand &c);
    QString storePath() const;
    void load();
    void save() const;
    void seedDefaults();      // первый запуск: пара безопасных примеров (заблокировать экран, скриншот)

    QVector<OrbitCommand> m_commands;
};

#endif // COMMANDSTORE_H
