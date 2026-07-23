#pragma once

#include <QLabel>
#include <QImage>
#include <QRectF>

#include <optional>

namespace MicArray {

class VideoPreviewWidget : public QLabel
{
    Q_OBJECT

public:
    explicit VideoPreviewWidget(QWidget *parent = nullptr);

    void setFrame(const QImage &image);
    QImage frame() const { return m_frame; }
    QRectF videoRect() const;
    std::optional<QPointF> normalizedVideoPoint(const QPointF &widgetPoint) const;

signals:
    void fullScreenRequested();

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void refreshPixmap();

    QImage m_frame;
};

} // namespace MicArray
