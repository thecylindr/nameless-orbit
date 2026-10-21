#include "CryptoUtils.h"
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QDebug>
#include <cstring>

// Реализация X25519 (RFC 7748) по мотивам TweetNaCl (public domain), та же, что в drokclient/drokserver:
// арифметика в поле 2^255-19, постоянное время (без ветвлений по секрету).
namespace x25519_detail {
typedef long long i64;
typedef i64 gf[16];

static const gf k121665 = {0xDB41, 1};

static void car25519(i64 *o)
{
    for (int i = 0; i < 16; ++i) {
        o[i] += (i64(1) << 16);
        const i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}

static void sel25519(i64 *p, i64 *q, int b)
{
    const i64 c = ~(i64(b) - 1);
    for (int i = 0; i < 16; ++i) {
        const i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(unsigned char *o, const i64 *n)
{
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int b = int((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = static_cast<unsigned char>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<unsigned char>((t[i] >> 8) & 0xff);
    }
}

static void unpack25519(i64 *o, const unsigned char *n)
{
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + (i64(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}

static void fadd(i64 *o, const i64 *a, const i64 *b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
static void fsub(i64 *o, const i64 *a, const i64 *b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }

static void fmul(i64 *o, const i64 *a, const i64 *b)
{
    i64 t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o); car25519(o);
}

static void fsqr(i64 *o, const i64 *a) { fmul(o, a, a); }

static void inv25519(i64 *o, const i64 *in)
{
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = in[a];
    for (int a = 253; a >= 0; --a) {
        fsqr(c, c);
        if (a != 2 && a != 4) fmul(c, c, in);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

// q = n * p (скаляр n, точка p — по 32 байта)
static void scalarmult(unsigned char *q, const unsigned char *n, const unsigned char *p)
{
    unsigned char z[32];
    i64 x[80];
    gf a, b, c, d, e, f;
    for (int i = 0; i < 31; ++i) z[i] = n[i];
    z[31] = static_cast<unsigned char>((n[31] & 127) | 64);
    z[0] &= 248;
    unpack25519(x, p);
    for (int i = 0; i < 16; ++i) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        const int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        fadd(e, a, c); fsub(a, a, c); fadd(c, b, d); fsub(b, b, d);
        fsqr(d, e); fsqr(f, a); fmul(a, c, a); fmul(c, b, e);
        fadd(e, a, c); fsub(a, a, c); fsqr(b, a); fsub(c, d, f);
        fmul(a, c, k121665); fadd(a, a, d); fmul(c, c, a); fmul(a, d, f);
        fmul(d, b, x); fsqr(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    for (int i = 0; i < 16; ++i) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    inv25519(x + 32, x + 32);
    fmul(x + 16, x + 16, x + 32);
    pack25519(q, x + 16);
}
} // namespace x25519_detail

QByteArray CryptoUtils::randomBytes(int count)
{
    QByteArray out(count, Qt::Uninitialized);
    QList<quint32> words((count + 3) / 4);
    QRandomGenerator::system()->generate(words.begin(), words.end());  // системный CSPRNG (getrandom/BCrypt/arc4random)
    std::memcpy(out.data(), words.constData(), size_t(count));
    return out;
}

QByteArray CryptoUtils::x25519(const QByteArray &scalar32, const QByteArray &point32)
{
    if (scalar32.size() != KEY_SIZE || point32.size() != KEY_SIZE) return {};
    QByteArray out(KEY_SIZE, 0);
    x25519_detail::scalarmult(reinterpret_cast<unsigned char *>(out.data()),
                              reinterpret_cast<const unsigned char *>(scalar32.constData()),
                              reinterpret_cast<const unsigned char *>(point32.constData()));
    return out;
}

QByteArray CryptoUtils::x25519Public(const QByteArray &secret32)
{
    QByteArray base(KEY_SIZE, 0);
    base[0] = 9;
    return x25519(secret32, base);
}

static QByteArray sha256(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

bool CryptoUtils::deriveChannel(SecureChannel &ch, const QByteArray &dhEE, const QByteArray &dhES,
                                const QByteArray &dhSE, const QByteArray &transcript, bool isInitiator)
{
    // вырожденный результат (публичный ключ малого порядка) — отказ; любая из трёх пар обязана быть ненулевой
    const QByteArray zero(KEY_SIZE, 0);
    for (const QByteArray &dh : {dhEE, dhES, dhSE})
        if (dh.size() != KEY_SIZE || constantTimeEquals(dh, zero)) return false;

    const QByteArray master = sha256(QByteArrayLiteral("ORBIT-KEX3-master") + dhEE + dhES + dhSE + sha256(transcript));
    // a2b/b2a вместо client/server: оба узла равноправны, инициатор — тот, кто открыл TCP-соединение
    const QByteArray a2bEnc = hmacSha256(master, QByteArrayLiteral("a2b-enc"));
    const QByteArray a2bMac = hmacSha256(master, QByteArrayLiteral("a2b-mac"));
    const QByteArray b2aEnc = hmacSha256(master, QByteArrayLiteral("b2a-enc"));
    const QByteArray b2aMac = hmacSha256(master, QByteArrayLiteral("b2a-mac"));

    // код сверки: 20 бит из HMAC общего секрета → шесть цифр. Подобрать «нужный» код атакующему можно только
    // перебором ключей до начала сверки (шанс 1 из миллиона на попытку), а повторить попытку без ведома людей нельзя
    const QByteArray sasRaw = hmacSha256(master, QByteArrayLiteral("sas"));
    const quint32 v = (quint8(sasRaw[0]) << 16) | (quint8(sasRaw[1]) << 8) | quint8(sasRaw[2]);
    const QString digits = QString::number(v % 1000000).rightJustified(6, QLatin1Char('0'));

    ch.reset();
    ch.sas = digits.left(3) + QLatin1Char(' ') + digits.mid(3);
    ch.sendEncKey = isInitiator ? a2bEnc : b2aEnc;
    ch.sendMacKey = isInitiator ? a2bMac : b2aMac;
    ch.recvEncKey = isInitiator ? b2aEnc : a2bEnc;
    ch.recvMacKey = isInitiator ? b2aMac : a2bMac;
    ch.ready = true;
    return true;
}

QString CryptoUtils::fingerprint(const QByteArray &publicKey32)
{
    const QByteArray h = sha256(publicKey32).toHex().toUpper();
    QString out;
    for (int i = 0; i < h.size(); i += 4) {
        if (!out.isEmpty()) out += QLatin1Char('-');
        out += QString::fromLatin1(h.mid(i, 4));
    }
    return out;
}

QString CryptoUtils::shortCode(const QByteArray &publicKey32)
{
    // первые 4 байта хэша как число 0..999999 — короткий код для устного сравнения при сопряжении
    const QByteArray h = sha256(publicKey32);
    const quint32 v = (quint8(h[0]) << 24) | (quint8(h[1]) << 16) | (quint8(h[2]) << 8) | quint8(h[3]);
    return QString::number(v % 1000000).rightJustified(6, QLatin1Char('0'));
}

QByteArray CryptoUtils::hmacSha256(const QByteArray &key, const QByteArray &data)
{
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

bool CryptoUtils::constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    return diff == 0;
}

// Поточный шифр — ChaCha20 (RFC 8439): ключ 32 байта, nonce — первые 12 байт случайного nonce блока, счётчик с нуля. Заменил прежний поток
// Blake2b-512 по 64 байта через QCryptographicHash: тот на каждый кусок сбрасывал хэш и выделял память, и на телефоне шифрование
// тяжёлого потока (тайлы, JPEG, видео) занимало заметную долю главного потока — интерфейс подлагивал. ChaCha20 без выделений на порядок быстрее
namespace chacha {
inline quint32 rotl(quint32 v, int c) { return (v << c) | (v >> (32 - c)); }
inline quint32 le32(const uchar *p) { return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24); }
#define ORBIT_QR(a, b, c, d) a += b; d ^= a; d = rotl(d, 16); c += d; b ^= c; b = rotl(b, 12); a += b; d ^= a; d = rotl(d, 8); c += d; b ^= c; b = rotl(b, 7);

void block(const quint32 in[16], uchar out[64])
{
    quint32 x[16];
    for (int i = 0; i < 16; ++i) x[i] = in[i];
    for (int r = 0; r < 10; ++r) {
        ORBIT_QR(x[0], x[4], x[8], x[12]) ORBIT_QR(x[1], x[5], x[9], x[13]) ORBIT_QR(x[2], x[6], x[10], x[14]) ORBIT_QR(x[3], x[7], x[11], x[15])
        ORBIT_QR(x[0], x[5], x[10], x[15]) ORBIT_QR(x[1], x[6], x[11], x[12]) ORBIT_QR(x[2], x[7], x[8], x[13]) ORBIT_QR(x[3], x[4], x[9], x[14])
    }
    for (int i = 0; i < 16; ++i) {
        const quint32 v = x[i] + in[i];
        out[4 * i] = uchar(v); out[4 * i + 1] = uchar(v >> 8); out[4 * i + 2] = uchar(v >> 16); out[4 * i + 3] = uchar(v >> 24);
    }
}
#undef ORBIT_QR

// данные XOR поток ChaCha20 на месте
void xorStream(uchar *data, qint64 len, const QByteArray &key, const QByteArray &nonce)
{
    const uchar *k = reinterpret_cast<const uchar *>(key.constData());
    const uchar *n = reinterpret_cast<const uchar *>(nonce.constData());
    quint32 st[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                      le32(k), le32(k + 4), le32(k + 8), le32(k + 12), le32(k + 16), le32(k + 20), le32(k + 24), le32(k + 28),
                      0, le32(n), le32(n + 4), le32(n + 8)};
    uchar ks[64];
    for (qint64 off = 0; off < len; off += 64) {
        block(st, ks);
        const int m = int(qMin<qint64>(64, len - off));
        for (int i = 0; i < m; ++i) data[off + i] ^= ks[i];
        ++st[12];
    }
}
}

QByteArray CryptoUtils::keyStream(const QByteArray &nonce, const QByteArray &key, qint64 length)
{
    QByteArray stream(int(length), 0);
    if (key.size() >= 32 && nonce.size() >= 12) chacha::xorStream(reinterpret_cast<uchar *>(stream.data()), length, key, nonce);
    return stream;
}

QByteArray CryptoUtils::seal(SecureChannel &ch, const QByteArray &plain)
{
    if (!ch.ready) return {};
    QByteArray out;
    int offset = 0;
    do {
        const QByteArray chunk = plain.mid(offset, MAX_CHUNK);
        offset += chunk.size();

        QByteArray body(SEQ_SIZE, 0);
        const quint64 seq = ch.sendSeq++;
        for (int i = 0; i < 8; ++i) body[7 - i] = char((seq >> (8 * i)) & 0xFF);
        body.append(chunk);

        const QByteArray nonce = randomBytes(NONCE_SIZE);
        chacha::xorStream(reinterpret_cast<uchar *>(body.data()), body.size(), ch.sendEncKey, nonce);
        const int blockLen = NONCE_SIZE + body.size() + HMAC_SIZE;
        QByteArray header(HEADER_SIZE, 0);
        header[0] = char((blockLen >> 8) & 0xFF);
        header[1] = char(blockLen & 0xFF);
        // подпись покрывает и заголовок длины: подмена длины блока на лету больше не «сдвигает» разбор потока, а сразу рвёт канал
        const QByteArray mac = hmacSha256(ch.sendMacKey, header + nonce + body);

        out.append(header);
        out.append(nonce);
        out.append(body);
        out.append(mac);
    } while (offset < plain.size());
    return out;
}

bool CryptoUtils::open(SecureChannel &ch, QByteArray &buffer, QList<QByteArray> &lines)
{
    lines.clear();
    if (!ch.ready) return false;

    while (buffer.size() >= HEADER_SIZE) {
        const int blockLen = (static_cast<unsigned char>(buffer[0]) << 8) |
                             static_cast<unsigned char>(buffer[1]);
        if (blockLen < NONCE_SIZE + SEQ_SIZE + HMAC_SIZE) {
            buffer.clear();
            return false;
        }
        if (buffer.size() < HEADER_SIZE + blockLen)
            break;

        const QByteArray header = buffer.left(HEADER_SIZE);
        const QByteArray block = buffer.mid(HEADER_SIZE, blockLen);
        buffer.remove(0, HEADER_SIZE + blockLen);

        const QByteArray nonce = block.left(NONCE_SIZE);
        QByteArray body = block.mid(NONCE_SIZE, blockLen - NONCE_SIZE - HMAC_SIZE);
        const QByteArray mac = block.right(HMAC_SIZE);
        if (!constantTimeEquals(hmacSha256(ch.recvMacKey, header + nonce + body), mac)) {
            qWarning() << "CryptoUtils::open: HMAC не совпал — данные повреждены или подделаны";
            return false;
        }
        chacha::xorStream(reinterpret_cast<uchar *>(body.data()), body.size(), ch.recvEncKey, nonce);

        quint64 seq = 0;
        for (int i = 0; i < 8; ++i) seq = (seq << 8) | static_cast<unsigned char>(body[i]);
        if (seq != ch.recvSeq) {
            qWarning() << "CryptoUtils::open: нарушен порядок кадров (повтор/подмена)";
            return false;
        }
        ++ch.recvSeq;

        ch.lineBuffer.append(body.constData() + SEQ_SIZE, body.size() - SEQ_SIZE);
        int nl;
        while ((nl = ch.lineBuffer.indexOf('\n')) >= 0) {
            const QByteArray line = ch.lineBuffer.left(nl).trimmed();
            ch.lineBuffer.remove(0, nl + 1);
            if (!line.isEmpty()) lines.append(line);
        }
        if (ch.lineBuffer.size() > MAX_LINE) {
            qWarning() << "CryptoUtils::open: слишком длинная строка без перевода строки";
            return false;
        }
    }
    return true;
}
