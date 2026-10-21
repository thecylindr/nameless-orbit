#ifndef CRYPTOUTILS_H
#define CRYPTOUTILS_H

#include <QByteArray>
#include <QString>
#include <QList>

// Защищённый канал между двумя устройствами Orbit (симметричный, оба — равноправные узлы, в отличие от
// клиент/сервер в drones): X25519, оба статических ключа участвуют в выводе общего секрета — это и есть
// доказательство владения приватным ключом с обеих сторон (а не только с одной, как в TOFU-к-серверу).
//   рукопожатие: инициатор -> "ORBIT-HELLO1 <static_hex> <ephemeral_hex> <device_id> <name_b64> <kind>\n"
//                 отвечающий -> "ORBIT-HELLO2 <static_hex> <ephemeral_hex> <device_id> <name_b64> <kind>\n"
//   секрет:      master = SHA256("ORBIT-KEX1" || dh_ee || dh_es || dh_se || SHA256(transcript))
//                 dh_ee — эфемерный×эфемерный; dh_es/dh_se — эфемерный одной стороны × статический другой
//                 (каждая пара считается обеими сторонами одинаково благодаря коммутативности X25519 —
//                 совпадение возможно только если обе стороны владеют приватными ключами, которые назвали)
//   кадр:        [2 байта длина BE][16 байт nonce][шифртекст (8 байт счётчик + данные)][32 байта HMAC-SHA256]
struct SecureChannel
{
    QByteArray sendEncKey, sendMacKey;
    QByteArray recvEncKey, recvMacKey;
    quint64 sendSeq = 0;
    quint64 recvSeq = 0;
    bool ready = false;
    QString sas;             // короткий код сверки «123 456»: выводится из общего секрета, поэтому на обоих экранах один и тот же,
                             // а посредник в середине (у него другие ключи с каждой стороной) получит у каждой стороны разный код
    QByteArray lineBuffer;   // расшифрованные байты до ближайшего '\n'

    void reset() { *this = SecureChannel(); }
};

class CryptoUtils
{
public:
    static constexpr int NONCE_SIZE = 16;
    static constexpr int HMAC_SIZE  = 32;   // SHA-256
    static constexpr int HEADER_SIZE = 2;   // длина блока (big-endian)
    static constexpr int KEY_SIZE   = 32;
    static constexpr int SEQ_SIZE   = 8;
    static constexpr int MAX_CHUNK  = 60000;      // полезная нагрузка одного кадра
    static constexpr int MAX_LINE   = 8 * 1024 * 1024;  // предел незавершённой строки (защита памяти)

    // X25519 (RFC 7748), арифметика в постоянное время — реализация по мотивам TweetNaCl (public domain)
    static QByteArray randomBytes(int count);
    static QByteArray x25519(const QByteArray &scalar32, const QByteArray &point32);
    static QByteArray x25519Public(const QByteArray &secret32);

    // взаимная аутентификация: обе стороны владеют своим статическим приватным ключом
    static bool deriveChannel(SecureChannel &ch, const QByteArray &dhEE, const QByteArray &dhES,
                               const QByteArray &dhSE, const QByteArray &transcript, bool isInitiator);

    // короткий отпечаток публичного ключа для сверки человеком («AB12-CD34-…»)
    static QString fingerprint(const QByteArray &publicKey32);
    // ещё короче — 6 цифр, для устного сравнения (как номер в KDE Connect / Bluetooth-сопряжении)
    static QString shortCode(const QByteArray &publicKey32);

    static QByteArray seal(SecureChannel &ch, const QByteArray &plain);
    // false — подделка/повтор/переполнение; соединение рвётся
    static bool open(SecureChannel &ch, QByteArray &buffer, QList<QByteArray> &lines);

    static QByteArray hmacSha256(const QByteArray &key, const QByteArray &data);
    static bool constantTimeEquals(const QByteArray &a, const QByteArray &b);

private:
    static QByteArray keyStream(const QByteArray &nonce, const QByteArray &key, qint64 length);
};

#endif // CRYPTOUTILS_H
