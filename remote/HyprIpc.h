#ifndef HYPRIPC_H
#define HYPRIPC_H

#include <QByteArray>
#include <QLocalSocket>
#include <QString>
#include <QDir>
#include <QFileInfo>
#include <QtGlobal>

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

// Orbit запущен без HYPRLAND_INSTANCE_SIGNATURE (автозапуск до того, как сессия выставила переменную, запуск из другого окружения, перезапуск Hyprland) —
// без неё Orbit считал бы, что Hyprland нет, и предлагал «окно driftwm» вместо настоящего виртуального монитора. Тогда ищется живой сокет Hyprland
// в $XDG_RUNTIME_DIR/hypr (самый свежий, к которому можно подключиться), и переменная выставляется — её наследуют и дочерние hyprctl, wf-recorder.
// Возвращает true, если переменная была восстановлена
inline bool recoverSignature()
{
    if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) return false;
    const QString root = qEnvironmentVariable("XDG_RUNTIME_DIR") + QStringLiteral("/hypr");
    const QFileInfoList dirs = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);      // самые свежие — первыми
    for (const QFileInfo &d : dirs) {
        const QString sock = d.absoluteFilePath() + QStringLiteral("/.socket.sock");
        if (!QFileInfo::exists(sock)) continue;
        QLocalSocket s;
        s.connectToServer(sock);
        if (!s.waitForConnected(200)) continue;                 // осталась от упавшего Hyprland — пропускается
        s.disconnectFromServer();
        qputenv("HYPRLAND_INSTANCE_SIGNATURE", d.fileName().toLocal8Bit());
        return true;
    }
    return false;
}

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
