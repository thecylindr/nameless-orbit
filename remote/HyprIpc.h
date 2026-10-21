#ifndef HYPRIPC_H
#define HYPRIPC_H

#include <QByteArray>
#include <QLocalSocket>
#include <QString>

// Запрос к Hyprland напрямую через его сокет — около миллисекунды вместо запуска процесса hyprctl (десятки мс и fork на каждый вызов).
// Нужен там, где Hyprland спрашивают часто: положение курсора, мониторы, окна при показе, перестановка курсора при управлении.
//   request("j/monitors"), request("j/cursorpos"), request("dispatch hl.dsp.cursor.move({ x = 10, y = 20 })")
// Нет Hyprland или сокет не ответил — пустой ответ
namespace HyprIpc {

inline QString socketPath()
{
    const QByteArray sig = qgetenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (sig.isEmpty()) return {};
    return qEnvironmentVariable("XDG_RUNTIME_DIR") + QStringLiteral("/hypr/") + QString::fromLocal8Bit(sig) + QStringLiteral("/.socket.sock");
}

inline bool available() { return !socketPath().isEmpty(); }

inline QByteArray request(const QByteArray &req, int timeoutMs = 300)
{
    const QString path = socketPath();
    if (path.isEmpty()) return {};
    QLocalSocket s;
    s.connectToServer(path);
    if (!s.waitForConnected(timeoutMs)) return {};
    s.write(req);
    if (!s.waitForBytesWritten(timeoutMs)) return {};
    QByteArray out;
    // Hyprland отвечает и закрывает соединение: читаем до закрытия
    while (s.state() == QLocalSocket::ConnectedState && s.waitForReadyRead(timeoutMs)) out += s.readAll();
    out += s.readAll();
    return out;
}

} // namespace HyprIpc

#endif // HYPRIPC_H
