#include "FrameView.h"
#include "RemoteDesktopController.h"
#include <QQuickWindow>
#include <QSGSimpleTextureNode>

FrameView::FrameView(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

QObject *FrameView::source() const { return m_ctrl; }

void FrameView::setSource(QObject *source)
{
    auto *c = qobject_cast<RemoteDesktopController *>(source);
    if (c == m_ctrl) return;
    if (m_ctrl) disconnect(m_ctrl, nullptr, this, nullptr);
    m_ctrl = c;
    if (m_ctrl) connect(m_ctrl, &RemoteDesktopController::frameUpdated, this, [this]() { update(); });
    emit sourceChanged();
    update();
}

QSGNode *FrameView::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGSimpleTextureNode *>(old);
    QImage image;
    QRectF region;
    quint64 serial = 0;
    if (!m_ctrl || !window() || !m_ctrl->canvasSnapshot(&image, &region, &serial) || image.isNull()) {
        delete node;
        m_hasTexture = false;
        return nullptr;
    }
    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
        m_hasTexture = false;
    }
    if (!m_hasTexture || serial != m_serial) {
        // загрузка текстуры — только при изменении картинки; предыдущая освобождается узлом
        node->setTexture(window()->createTextureFromImage(image, QQuickWindow::TextureIsOpaque));
        m_serial = serial;
        m_hasTexture = true;
    }
    // картинка лежит в том месте экрана, которое она показывает (увеличенный участок — только его часть)
    node->setRect(QRectF(region.x() * width(), region.y() * height(), region.width() * width(), region.height() * height()));
    return node;
}
