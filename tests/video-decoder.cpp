#include "../remote/VideoDecoder.h"
#include "../remote/H264Stream.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
extern "C" {
#include <libavutil/log.h>
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    av_log_set_level(AV_LOG_QUIET);
    QProcess encoder;
    encoder.start(QStringLiteral("ffmpeg"), {"-v", "error", "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=30",
                  "-frames:v", "3", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
                  "-x264-params", "aud=1:repeat-headers=1:keyint=30", "-f", "h264", "pipe:1"});
    if (!encoder.waitForFinished(10000) || encoder.exitCode()) qFatal("Test encoder failed");
    QByteArray stream = encoder.readAllStandardOutput();
    auto units = H264Stream::takeComplete(stream);
    if (units.size() < 2) qFatal("Missing test access units");
    VideoDecoder decoder;
    int frames = 0;
    QObject::connect(&decoder, &VideoDecoder::frameDecoded, &app, [&](const QVideoFrame &frame, qint64) {
        if (frame.size() != QSize(128, 96)) qFatal("Wrong decoded geometry");
        ++frames;
    });
    auto waitFrame = [&]() {
        QElapsedTimer timer;
        timer.start();
        const int before = frames;
        while (frames == before && timer.elapsed() < 3000) { app.processEvents(); QThread::msleep(1); }
        if (frames == before) qFatal("Decoder did not recover on IDR");
    };
    if (decoder.feed(units[1], false, 1)) qFatal("Accepted delta before IDR");
    if (!decoder.feed(units[0], true, 2)) qFatal("Rejected initial IDR");
    waitFrame();
    bool dropped = false;
    for (int i = 0; i < 10000 && !dropped; ++i) dropped = !decoder.feed(units[1], false, 3 + i);
    if (!dropped) qFatal("Queue overload fixture failed");
    for (int i = 0; i < 10; ++i)
        if (decoder.feed(units[1], false, 20000 + i)) qFatal("Accepted dependent frame after queue loss");
    if (!decoder.feed(units[0], true, 30000)) qFatal("Rejected recovery IDR");
    waitFrame();
    decoder.stop();
    const int before = frames;
    app.processEvents();
    if (frames != before) qFatal("Stopped session emitted queued frames");
    if (decoder.feed(units[1], false, 30001)) qFatal("Accepted delta after stop");
    if (!decoder.feed(units[0], true, 30002)) qFatal("Failed to restart decoder");
    waitFrame();
}
