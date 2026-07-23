#include "ui/videopreviewwidget.h"

#include <QMouseEvent>
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
