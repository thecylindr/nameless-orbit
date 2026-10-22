#ifndef VIDEODECODER_H
#define VIDEODECODER_H

#include <QObject>
#include <QByteArray>
#include <QVideoFrame>
#include <QString>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>

// Разжатие видео H.264 на настольном Linux через libavcodec (зритель — ПК: показ экрана телефона). Кадры приходят целыми единицами
// (Annex B), разжимаются в рабочем потоке в режиме малой задержки, готовый YUV 4:2:0 уходит в QVideoFrame — цвета переводит видеокарта
// при отрисовке (VideoOutput), процессор картинку не пересчитывает. Собирается, только если в системе есть libavcodec (ORBIT_HAVE_FFMPEG)
class VideoDecoder : public QObject
{
    Q_OBJECT
public:
    explicit VideoDecoder(QObject *parent = nullptr);
    ~VideoDecoder() override;

    static bool available();                                    // умеет ли эта сборка разжимать H.264
    // кадр в разжатие; false — не принят (ждёт ключевой кадр или очередь переполнена): отправителя стоит попросить ключевой
    bool feed(const QByteArray &au, bool key, qint64 ptsUs);
    void stop();

signals:
    void frameDecoded(const QVideoFrame &frame, qint64 ptsUs);
    void failed(const QString &reason);

private:
    struct Unit { QByteArray data; qint64 pts; bool key; };
    void run();

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Unit> m_queue;
    std::atomic<bool> m_quit{false};
    bool m_waitKey = true;
    std::atomic<quint64> m_generation{0};
    bool m_started = false;          // получен ключевой кадр, поток разжатия работает
    bool m_reported = false;
};

#endif // VIDEODECODER_H
