#pragma once

#include "analysis/localizationtypes.h"
#include "ui/videopreviewwidget.h"

namespace MicArray {

class LiveLocalizationView : public VideoPreviewWidget
{
    Q_OBJECT

public:
    explicit LiveLocalizationView(QWidget *parent = nullptr);

    void setCameraView(const CameraViewSettings &camera);
    void setDirections(const QVector<SoundSourceDirection> &directions);
    void setSpatialResponse(const LiveSpatialResponse &response);
    void setDisplayMode(const QString &mode);
    void setHeatmapOpacity(double opacity);
    void setCalibrationMode(bool enabled);
    void setCalibrationSamples(const QVector<VideoCalibrationSample> &samples,
                               const QVector<QPointF> &predictedPoints = {});
    void setStatusText(const QString &text);

signals:
    void calibrationPointClicked(const QPointF &normalizedPoint);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    QImage heatmapImage() const;

    CameraViewSettings m_camera;
    QVector<SoundSourceDirection> m_directions;
    LiveSpatialResponse m_response;
    QVector<VideoCalibrationSample> m_calibrationSamples;
    QVector<QPointF> m_predictedPoints;
    QString m_displayMode = QStringLiteral("marker");
    QString m_statusText;
    double m_heatmapOpacity = 0.45;
    bool m_calibrationMode = false;
};

} // namespace MicArray
