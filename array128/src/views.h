#pragma once

#include "pipeline.h"

#include <QImage>
#include <QWidget>

namespace MicArray::Array128 {

class ChannelGrid final : public QWidget
{
    Q_OBJECT
public:
    explicit ChannelGrid(QWidget *parent = nullptr);
    void setData(const PipelineSnapshot &snapshot, int selected);
signals:
    void channelSelected(int channel);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
private:
    std::array<ChannelQuality, Channels> m_quality;
    int m_selected = 0;
    bool m_hasData = false;
};

class WaveView final : public QWidget
{
public:
    explicit WaveView(QWidget *parent = nullptr);
    void setSamples(const QVector<float> &samples);
    void setSpectrum(bool spectrum) { m_spectrum = spectrum; }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QVector<float> m_samples;
    bool m_spectrum = false;
};

class FieldView final : public QWidget
{
    Q_OBJECT
public:
    explicit FieldView(QWidget *parent = nullptr);
    void setImage(const QImage &image);
    void setResponse(const LocalizationResult &result, double focus);
    void setCameraView(const CameraViewSettings &camera);
    void setTargets(const std::array<ScanDirection, 3> &targets, bool show, int active);
    void setCalibrationPoints(const QVector<VideoCalibrationSample> &points);
    QSize imageSize() const { return m_image.isNull() ? QSize(1280, 720) : m_image.size(); }
signals:
    void pointClicked(const QPointF &normalized);
    void targetMoved(int target, double azimuth, double elevation, bool final);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
private:
    QRectF imageRect() const;
    void moveTarget(const QPointF &position, bool final);
    QImage m_image;
    QImage m_heat;
    CameraViewSettings m_camera;
    LocalizationResult m_result;
    std::array<ScanDirection, 3> m_targets;
    QVector<VideoCalibrationSample> m_calibration;
    bool m_showTargets = false;
    int m_active = 0;
    bool m_dragging = false;
};

} // namespace MicArray::Array128
