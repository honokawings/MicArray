#include "ui/livelocalizationview.h"

#include "analysis/directionprojection.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace MicArray {

LiveLocalizationView::LiveLocalizationView(QWidget *parent)
    : VideoPreviewWidget(parent)
{
    setObjectName(QStringLiteral("liveLocalizationView"));
}

void LiveLocalizationView::setCameraView(const CameraViewSettings &camera)
{
    m_camera = camera;
    update();
}

void LiveLocalizationView::setDirections(
    const QVector<SoundSourceDirection> &directions)
{
    m_directions = directions;
    update();
}

void LiveLocalizationView::setSpatialResponse(const LiveSpatialResponse &response)
{
    m_response = response;
    update();
}

void LiveLocalizationView::setDisplayMode(const QString &mode)
{
    m_displayMode = mode;
    update();
}

void LiveLocalizationView::setHeatmapOpacity(double opacity)
{
    m_heatmapOpacity = std::clamp(opacity, 0.0, 1.0);
    update();
}

void LiveLocalizationView::setCalibrationMode(bool enabled)
{
    m_calibrationMode = enabled;
    update();
}

void LiveLocalizationView::setCalibrationSamples(
    const QVector<VideoCalibrationSample> &samples,
    const QVector<QPointF> &predictedPoints)
{
    m_calibrationSamples = samples;
    m_predictedPoints = predictedPoints;
    update();
}

void LiveLocalizationView::setStatusText(const QString &text)
{
    m_statusText = text;
    update();
}

void LiveLocalizationView::mousePressEvent(QMouseEvent *event)
{
    if (m_calibrationMode && event->button() == Qt::LeftButton) {
        const auto point = normalizedVideoPoint(event->position());
        if (point.has_value()) {
            emit calibrationPointClicked(*point);
            event->accept();
            return;
        }
    }
    VideoPreviewWidget::mousePressEvent(event);
}

void LiveLocalizationView::paintEvent(QPaintEvent *event)
{
    VideoPreviewWidget::paintEvent(event);
    const QRectF target = videoRect();
    if (target.isEmpty()) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_displayMode == QStringLiteral("heatmap")) {
        const QImage heatmap = heatmapImage();
        if (!heatmap.isNull()) {
            painter.setOpacity(m_heatmapOpacity);
            painter.drawImage(target, heatmap);
            painter.setOpacity(1.0);
        }
    }
    for (const SoundSourceDirection &direction : m_directions) {
        if (m_displayMode != QStringLiteral("marker")) {
            break;
        }
        const ProjectionResult projected = projectDirection(direction, m_camera);
        if (!projected.valid || !projected.inFront) {
            continue;
        }
        const QPointF normalized = projected.inView
            ? projected.normalizedPoint : projected.edgePoint;
        const QPointF point(target.left() + normalized.x() * target.width(),
                            target.top() + normalized.y() * target.height());
        painter.setPen(QPen(Qt::white, 2));
        painter.setBrush(QColor(255, 65, 65, 210));
        painter.drawEllipse(point, 9, 9);
        painter.drawLine(point + QPointF(-15, 0), point + QPointF(15, 0));
        painter.drawLine(point + QPointF(0, -15), point + QPointF(0, 15));
        painter.drawText(point + QPointF(13, -10),
                         QStringLiteral("%1° / %2°  %3%")
                             .arg(direction.azimuthDegrees, 0, 'f', 1)
                             .arg(direction.elevationDegrees, 0, 'f', 1)
                             .arg(direction.confidence * 100.0, 0, 'f', 0));
        break;
    }
    for (int index = 0; index < m_calibrationSamples.size(); ++index) {
        const QPointF normalized = m_calibrationSamples.at(index).normalizedVideoPoint;
        const QPointF point(target.left() + normalized.x() * target.width(),
                            target.top() + normalized.y() * target.height());
        painter.setPen(QPen(QColor(0, 220, 255), 2));
        painter.setBrush(QColor(0, 90, 120, 180));
        painter.drawEllipse(point, 7, 7);
        painter.drawText(point + QPointF(9, -7), QString::number(index + 1));
        if (index < m_predictedPoints.size()) {
            const QPointF predicted(target.left()
                                        + m_predictedPoints.at(index).x() * target.width(),
                                    target.top()
                                        + m_predictedPoints.at(index).y() * target.height());
            painter.setPen(QPen(QColor(255, 220, 70), 2));
            painter.drawLine(point, predicted);
            painter.drawRect(QRectF(predicted - QPointF(5, 5), QSizeF(10, 10)));
        }
    }
    if (!m_statusText.isEmpty()) {
        painter.setPen(Qt::white);
        painter.setBrush(QColor(0, 0, 0, 150));
        const QRectF label = painter.boundingRect(
            target.adjusted(12, 12, -12, -12),
            Qt::AlignLeft | Qt::AlignBottom, m_statusText);
        painter.drawRoundedRect(label.adjusted(-7, -4, 7, 4), 4, 4);
        painter.drawText(target.adjusted(12, 12, -12, -12),
                         Qt::AlignLeft | Qt::AlignBottom, m_statusText);
    }
}

QImage LiveLocalizationView::heatmapImage() const
{
    if (m_response.columns <= 0 || m_response.rows <= 0
        || m_response.normalizedValues.size()
            != m_response.columns * m_response.rows) {
        return {};
    }
    QImage image(m_response.columns, m_response.rows, QImage::Format_ARGB32);
    for (int row = 0; row < m_response.rows; ++row) {
        auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(row));
        for (int column = 0; column < m_response.columns; ++column) {
            const float value = std::clamp(
                m_response.normalizedValues.at(row * m_response.columns + column),
                0.0f, 1.0f);
            const int hue = static_cast<int>((1.0f - value) * 240.0f);
            pixels[column] = QColor::fromHsv(hue, 255, 255, 230).rgba();
        }
    }
    return image;
}

} // namespace MicArray
