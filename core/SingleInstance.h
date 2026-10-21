#ifndef SINGLEINSTANCE_H
#define SINGLEINSTANCE_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <functional>

class QLocalServer;

// Одно приложение на пользователя — повторный запуск не открывает второе окно, а просит работающее показаться
// и завершается. Канал — локальный Qt (именованный канал Windows, сокет Unix в Linux), только для своей учётной
// записи. Тот же канал исполняет короткие команды статуса ("status", "scan", "enable", "disable") — это и есть
// точка, через которую NameLess UI (или любой другой скрипт) узнаёт состояние Orbit и управляет им без
// отдельного окна: см. обработчик команд в main.cpp.
class SingleInstance : public QObject
{
    Q_OBJECT
public:
    explicit SingleInstance(const QString &name, QObject *parent = nullptr);

    static bool ask(const QString &name, const QByteArray &command, QByteArray *reply, int timeoutMs = 2000);
    bool listen();
    void setHandler(std::function<QByteArray(const QByteArray &)> handler) { m_handler = std::move(handler); }

private:
    QString m_name;
    QLocalServer *m_server = nullptr;
    std::function<QByteArray(const QByteArray &)> m_handler;
};

#endif // SINGLEINSTANCE_H
