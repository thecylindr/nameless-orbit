#pragma once
#include <QByteArray>
#include <QVector>

namespace H264Stream {
inline qsizetype findAud(const QByteArray &bytes, qsizetype from = 0)
{
    for (;;) {
        const qsizetype p = bytes.indexOf(QByteArrayLiteral("\x00\x00\x01"), from);
        if (p < 0 || p + 3 >= bytes.size()) return -1;
        if ((quint8(bytes[p + 3]) & 0x1f) == 9)
            return p > 0 && bytes[p - 1] == 0 ? p - 1 : p;
        from = p + 3;
    }
}

// Keep the last AU until the next AUD proves it is complete. Pipe reads and
// elapsed time cannot establish an H.264 access-unit boundary.
inline QVector<QByteArray> takeComplete(QByteArray &buffer)
{
    QVector<QByteArray> units;
    qsizetype first = findAud(buffer);
    if (first < 0) {
        if (buffer.size() > 8 * 1024 * 1024) buffer.clear();
        return units;
    }
    if (first > 0) {
        // Some encoders put parameter sets before the first AUD. Preserve
        // those headers with the first picture; losing SPS/PPS prevents decode.
        const qsizetype nal = buffer.indexOf(QByteArrayLiteral("\x00\x00\x01"));
        const int type = nal >= 0 && nal + 3 < first ? quint8(buffer[nal + 3]) & 0x1f : 0;
        if (type != 7 && type != 8 && type != 6) { buffer.remove(0, first); first = 0; }
    }
    for (;;) {
        const qsizetype next = findAud(buffer, first + 5);
        if (next < 0) break;
        units.append(buffer.left(next));
        buffer.remove(0, next);
        first = 0;
    }
    if (buffer.size() > 8 * 1024 * 1024) buffer.clear();
    return units;
}
}
