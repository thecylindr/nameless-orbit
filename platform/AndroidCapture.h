#ifndef ANDROIDCAPTURE_H
#define ANDROIDCAPTURE_H

class RemoteDesktopController;

// Мост между Java-службой захвата экрана телефона (OrbitCaptureService) и RemoteDesktopController. Кадры приходят из потока
// Java и передаются в поток интерфейса через очередь; число «можно ли слать» и параметры сжатия читаются Java-стороной из атомарных
// значений. На остальных системах все функции пустые — захват там делает QScreenCapture
namespace AndroidCapture {

void attach(RemoteDesktopController *controller);
void setParams(int jpegQuality, int fps, bool canSend, bool tiles, bool video, int bitrateKbps);
void requestKeyFrame();    // зритель потерял картинку — кодеру телефона нужен ключевой кадр
void request();     // системное окно «Начать демонстрацию экрана»; ответ приходит в onExternalState
void stop();

}

#endif // ANDROIDCAPTURE_H
