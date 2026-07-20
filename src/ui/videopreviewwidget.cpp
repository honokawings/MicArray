#include "ui/videopreviewwidget.h"

#include <QMouseEvent>

namespace MicArray {

VideoPreviewWidget::VideoPreviewWidget(QWidget *parent)
    : QLabel(QStringLiteral("等待视频帧（双击进入演示全屏）"), parent)
{
    setAlignment(Qt::AlignCenter);
    setMinimumSize(640, 360);
    setStyleSheet(QStringLiteral("QLabel { background:#07111c; color:#9fb3c8; border:1px solid #29445d; border-radius:6px; }"));
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

} // namespace MicArray
