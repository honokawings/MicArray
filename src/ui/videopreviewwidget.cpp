#include "ui/videopreviewwidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>

namespace MicArray {

VideoPreviewWidget::VideoPreviewWidget(QWidget *parent)
    : QLabel(QStringLiteral("等待视频帧（双击进入演示全屏）"), parent)
{
    setAlignment(Qt::AlignCenter);
    setMinimumSize(640, 360);
    setStyleSheet(QStringLiteral("QLabel { background:#07111c; color:#9fb3c8; border:1px solid #29445d; border-radius:6px; }"));
}

void VideoPreviewWidget::setFrame(const QImage &image)
{
    m_frame = image;
    refreshPixmap();
}

void VideoPreviewWidget::setLocationMarker(const QPointF &normalizedPoint, const QString &label)
{
    m_locationMarkerPoint = normalizedPoint;
    m_locationMarkerLabel = label;
    m_locationMarkerVisible = true;
    update();
}

void VideoPreviewWidget::clearLocationMarker()
{
    m_locationMarkerVisible = false;
    update();
}

void VideoPreviewWidget::paintEvent(QPaintEvent *event)
{
    QLabel::paintEvent(event);
    const QRectF target = videoRect();
    if (!m_locationMarkerVisible || target.isEmpty())
    {
        return;
    }
    const QPointF point(target.left() + m_locationMarkerPoint.x() * target.width(),
                        target.top() + m_locationMarkerPoint.y() * target.height());
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setClipRect(target);
    painter.setPen(QPen(Qt::white, 2));
    painter.setBrush(QColor(255, 65, 65, 210));
    painter.drawEllipse(point, 9, 9);
    painter.drawLine(point + QPointF(-15, 0), point + QPointF(15, 0));
    painter.drawLine(point + QPointF(0, -15), point + QPointF(0, 15));
    QRectF label = painter.boundingRect(target, Qt::AlignLeft, m_locationMarkerLabel);
    label.moveTopLeft(point + QPointF(13, -label.height() - 8));
    if (label.right() > target.right()) { label.moveRight(target.right() - 4); }
    if (label.top() < target.top()) { label.moveTop(target.top() + 4); }
    painter.fillRect(label.adjusted(-4, -2, 4, 2), QColor(0, 0, 0, 170));
    painter.drawText(label, Qt::AlignLeft, m_locationMarkerLabel);
}

QRectF VideoPreviewWidget::videoRect() const
{
    if (m_frame.isNull()) {
        return {};
    }
    const QSizeF fitted = QSizeF(m_frame.size()).scaled(size(), Qt::KeepAspectRatio);
    return {(width() - fitted.width()) * 0.5,
            (height() - fitted.height()) * 0.5,
            fitted.width(),
            fitted.height()};
}

std::optional<QPointF> VideoPreviewWidget::normalizedVideoPoint(
    const QPointF &widgetPoint) const
{
    const QRectF target = videoRect();
    if (target.isEmpty() || !target.contains(widgetPoint)) {
        return std::nullopt;
    }
    return QPointF((widgetPoint.x() - target.left()) / target.width(),
                   (widgetPoint.y() - target.top()) / target.height());
}

void VideoPreviewWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        emit fullScreenRequested();
        event->accept();
        return;
    }
    QLabel::mouseDoubleClickEvent(event);
}

void VideoPreviewWidget::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    refreshPixmap();
}

void VideoPreviewWidget::refreshPixmap()
{
    if (m_frame.isNull()) {
        return;
    }
    setPixmap(QPixmap::fromImage(m_frame).scaled(
        size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

} // namespace MicArray
