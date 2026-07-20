#pragma once

#include <QLabel>

namespace MicArray {

class VideoPreviewWidget : public QLabel
{
    Q_OBJECT

public:
    explicit VideoPreviewWidget(QWidget *parent = nullptr);

signals:
    void fullScreenRequested();

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
};

} // namespace MicArray
