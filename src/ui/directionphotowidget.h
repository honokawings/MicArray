#pragma once

#include "analysis/localizationtypes.h"

#include <QImage>
#include <QRectF>
#include <QWidget>

namespace MicArray {

class DirectionPhotoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DirectionPhotoWidget(QWidget *parent = nullptr);

    void setImage(const QImage &image);
    void setCameraView(const CameraViewSettings &camera);
    void setDirections(const QVector<SoundSourceDirection> &directions);
    void setStatusText(const QString &text);
    void setConfidenceThreshold(double threshold);

    QRectF imageRect() const;
    QVector<QPointF> markerPoints() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_image;
    CameraViewSettings m_camera;
    QVector<SoundSourceDirection> m_directions;
    QString m_statusText = QStringLiteral("请选择背景照片并打开 48 通道 WAV 文件");
    double m_confidenceThreshold = 0.45;
};

} // namespace MicArray
