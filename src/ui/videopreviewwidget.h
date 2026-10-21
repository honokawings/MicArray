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
    void setLocationMarker(const QPointF &normalizedPoint, const QString &label);
    void clearLocationMarker();
    bool locationMarkerVisible() const { return m_locationMarkerVisible; }
    QPointF locationMarkerPoint() const { return m_locationMarkerPoint; }
    QImage frame() const { return m_frame; }
    QRectF videoRect() const;
    std::optional<QPointF> normalizedVideoPoint(const QPointF &widgetPoint) const;

signals:
    void fullScreenRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void refreshPixmap();

    QImage m_frame;
    QPointF m_locationMarkerPoint;
    QString m_locationMarkerLabel;
    bool m_locationMarkerVisible = false;
};

} // namespace MicArray
