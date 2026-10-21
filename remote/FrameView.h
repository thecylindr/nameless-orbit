#ifndef FRAMEVIEW_H
#define FRAMEVIEW_H

#include <QQuickItem>
#include <QPointer>

class RemoteDesktopController;

// Экран показывающего устройства, нарисованный прямо из собранной картинки RemoteDesktopController текстурой сцены.
// Раньше кадр шёл через «Image + адрес image://…»: на каждый кадр новый адрес, асинхронная загрузка, новая текстура и выброс прежней —
// отсюда рывки на каждом кадре. Здесь текстура создаётся по готовой картинке в потоке отрисовки, только когда картинка изменилась,
// а подряд идущие обновления между кадрами экрана сливаются сами (рисуется всегда последнее состояние)
class FrameView : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject *source READ source WRITE setSource NOTIFY sourceChanged)

public:
    explicit FrameView(QQuickItem *parent = nullptr);

    QObject *source() const;
    void setSource(QObject *source);

signals:
    void sourceChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;

private:
    QPointer<RemoteDesktopController> m_ctrl;
    quint64 m_serial = 0;       // версия картинки, уже загруженной в текстуру
    bool m_hasTexture = false;
};

#endif // FRAMEVIEW_H
