#pragma once

#include "analysis/localizationtypes.h"
#include "ui/videopreviewwidget.h"

#include <limits>

namespace MicArray {

class LiveLocalizationView : public VideoPreviewWidget
{
    Q_OBJECT

public:
    explicit LiveLocalizationView(QWidget *parent = nullptr);

    void setCameraView(const CameraViewSettings &camera);
    void setDirections(const QVector<SoundSourceDirection> &directions);
    void setSpatialResponseActive(bool active);
    void setExpectedSpatialResponseGeneration(quint64 generation);
    void setSpatialResponse(const LiveSpatialResponse &response);
    void setDisplayMode(const QString &mode);
    void setHeatmapOpacity(double opacity);
    void setHeatmapFocusExponent(double exponent);
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
    void invalidateHeatmapCache();
    QImage heatmapImage() const;

    CameraViewSettings m_camera;
    QVector<SoundSourceDirection> m_directions;
    LiveSpatialResponse m_response;
    QVector<VideoCalibrationSample> m_calibrationSamples;
    QVector<QPointF> m_predictedPoints;
    QString m_displayMode = QStringLiteral("marker");
    QString m_statusText;
    quint64 m_expectedSpatialResponseGeneration = 0;
    quint64 m_heatmapRevision = 0;
    mutable quint64 m_cachedHeatmapRevision =
        std::numeric_limits<quint64>::max();
    mutable QSize m_cachedHeatmapSize;
    mutable QImage m_cachedHeatmap;
    double m_heatmapOpacity = 0.45;
    double m_heatmapFocusExponent = 1.5;
    bool m_spatialResponseActive = true;
    bool m_calibrationMode = false;
};

} // namespace MicArray
