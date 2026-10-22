#include "../remote/H264Stream.h"
#include <QCoreApplication>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QByteArray a = QByteArray::fromHex("0000000109f0000000016742c01f000001658884");
    const QByteArray b = QByteArray::fromHex("00000109f0000001419abc");
    const QByteArray c = QByteArray::fromHex("0000000109f0000001411234");
    const QByteArray input = a + b + c;
    // Every possible pipe fragmentation, including splitting the start code.
    for (qsizetype chunk = 1; chunk <= input.size(); ++chunk) {
        QByteArray buffer;
        QVector<QByteArray> units;
        for (qsizetype i = 0; i < input.size(); i += chunk) {
            buffer += input.mid(i, chunk);
            units += H264Stream::takeComplete(buffer);
        }
        if (units != QVector<QByteArray>{a, b} || buffer != c)
            qFatal("Fragmented H.264 access units were corrupted");
    }
    QByteArray prefixed = QByteArray::fromHex("000000016742c01f0000000168abcd") + input;
    const QByteArray prefix = prefixed.left(prefixed.size() - input.size());
    const auto prefixedUnits = H264Stream::takeComplete(prefixed);
    if (prefixedUnits != QVector<QByteArray>{prefix + a, b} || prefixed != c)
        qFatal("Lost SPS/PPS before first AUD");
    QByteArray partial = a.left(a.size() - 2);
    if (!H264Stream::takeComplete(partial).isEmpty()) qFatal("Incomplete AU emitted");
    QByteArray garbage(9 * 1024 * 1024, 'x');
    H264Stream::takeComplete(garbage);
    if (!garbage.isEmpty()) qFatal("Unbounded malformed stream");
    return 0;
}
