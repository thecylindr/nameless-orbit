#ifndef VIRTUALDISPLAY_H
#define VIRTUALDISPLAY_H

#include <QRect>
#include <QString>
#include <QVariantList>

// «Второй экран»: другое устройство (телефон, второй компьютер) становится ещё одним монитором этого компьютера.
//   Hyprland — настоящий виртуальный монитор (headless-выход): создаётся командой hyprctl, получает разрешение, частоту и место на общей раскладке
//              (слева, справа, сверху, снизу от основного), курсор переходит на него сам, как на обычный монитор; снимается видеокартой как любой выход.
//   driftwm  — БЕТА: виртуальных мониторов в нём нет, поэтому вторым экраном становится окно приложения: оно подгоняется под размер экрана
//              другого устройства, ставится в центр обзора, и снимается его область. Работает через driftwm msg (state / resize / move)
namespace VirtualDisplay {

enum class Backend { None, Hyprland, Driftwm };
Backend backend();
QString backendName();                               // «Hyprland», «driftwm (бета)» или пусто

struct Spec
{
    int width = 1920, height = 1080, fps = 60;
    QString workspace;                               // номер рабочего стола 1–10 на новом мониторе; пусто — отдельный «orbit»
    QString anchor;                                  // имя основного монитора, рядом с которым ставится новый (пусто — с фокусом)
    double scale = 1.0;                              // масштаб монитора (интерфейс на нём крупнее при большем числе)
    QString side = QStringLiteral("right");          // left | right | top | bottom — где стоит новый монитор относительно основного
};

// Hyprland: создать монитор; outName — его имя (HEADLESS-N). false — error содержит причину
QStringList monitorNames();                          // настоящие мониторы Hyprland (без виртуальных) — для выбора, где ставить второй экран
bool create(const Spec &spec, QString *outName, QString *error);
// удалить созданный монитор (повторный вызов безвреден)
void destroy(const QString &outName);
// держит на виртуальном мониторе только выбранный стол (пусто — «orbit»): чужой стол, попавший на него, возвращается на настоящий монитор
void keepWorkspace(const QString &outName, const QString &workspace);
// монитор остался от прошлого запуска (Orbit упал) — убрать: вызывается при старте
void cleanupStale();
// геометрия монитора на общей раскладке (логические пиксели); пусто — монитора нет
QRect outputRect(const QString &outName);

// driftwm: окна, которые можно сделать вторым экраном: [{ id, label }]
QVariantList driftwmWindows();
// driftwm: подогнать окно под размер экрана зрителя и поставить в центр обзора; rect — где оно теперь на раскладке (логические пиксели)
bool driftwmPrepare(int windowId, int width, int height, QRect *rect, QString *outputName, QString *error);
// driftwm: где окно сейчас (для слежения за ним); false — окна нет
bool driftwmWindowRect(int windowId, QRect *rect);

}

#endif // VIRTUALDISPLAY_H
