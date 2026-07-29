#include "ui/noisereductionvideowidget.h"

#include "analysis/directionprojection.h"
#include "control/directioncontrolmapper.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace MicArray {

NoiseReductionVideoWidget::NoiseReductionVideoWidget(QWidget *parent)
    : VideoPreviewWidget(parent)
{
    setObjectName(QStringLiteral("noiseReductionVideoView"));
}

void NoiseReductionVideoWidget::setCameraView(
    const CameraViewSettings &camera)
{
    m_camera = camera;
    m_camera.horizontalFovDegrees =
        std::min(m_camera.horizontalFovDegrees, 179.0);
    m_camera.verticalFovDegrees =
        std::min(m_camera.verticalFovDegrees, 179.0);
    update();
}

void NoiseReductionVideoWidget::setControlMapping(
    const DirectionControlMapping &mapping)
{
    m_mapping = mapping;
    update();
}

void NoiseReductionVideoWidget::setDirectionTargets(
    const ContinuousDirectionTarget &enhancedA,
    const ContinuousDirectionTarget &suppressionB1,
    const ContinuousDirectionTarget &suppressionB2)
{
    m_enhancedA = enhancedA;
    m_suppressionB1 = suppressionB1;
    m_suppressionB2 = suppressionB2;
    update();
}

void NoiseReductionVideoWidget::setCurrentSelection(
    NoiseDirectionSelection selection)
{
    m_selection = selection;
    update();
}

void NoiseReductionVideoWidget::setMarkersVisible(bool visible)
{
    m_markersVisible = visible;
    update();
}

void NoiseReductionVideoWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const auto normalized = normalizedVideoPoint(event->position());
        if (normalized.has_value()) {
            emit videoPointClicked(*normalized);
            event->accept();
            return;
        }
    }
    VideoPreviewWidget::mousePressEvent(event);
}

void NoiseReductionVideoWidget::paintEvent(QPaintEvent *event)
{
    VideoPreviewWidget::paintEvent(event);
    if (!m_markersVisible || !m_mapping.isValid()) {
        return;
    }
    const QRectF target = videoRect();
    if (target.isEmpty()) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    drawTarget(painter, target, QStringLiteral("A"), QColor(70, 225, 125),
               m_enhancedA, NoiseDirectionSelection::EnhancedA);
    drawTarget(painter, target, QStringLiteral("B1"), QColor(255, 165, 55),
               m_suppressionB1, NoiseDirectionSelection::SuppressionB1);
    drawTarget(painter, target, QStringLiteral("B2"), QColor(225, 75, 205),
               m_suppressionB2, NoiseDirectionSelection::SuppressionB2);
}

void NoiseReductionVideoWidget::drawTarget(
    QPainter &painter,
    const QRectF &videoRectangle,
    const QString &name,
    const QColor &color,
    const ContinuousDirectionTarget &target,
    NoiseDirectionSelection selection)
{
    SoundSourceDirection direction;
    direction.azimuthDegrees = target.azimuthDegrees;
    direction.elevationDegrees = target.elevationDegrees;
    const ProjectionResult projected = projectDirection(direction, m_camera);
    if (!projected.valid || !projected.inFront) {
        return;
    }
    const QPointF normalized =
        projected.inView ? projected.normalizedPoint : projected.edgePoint;
    const QPointF point(
        videoRectangle.left() + normalized.x() * videoRectangle.width(),
        videoRectangle.top() + normalized.y() * videoRectangle.height());
    painter.setPen(QPen(Qt::white,
                        selection == m_selection ? 3.0 : 1.5));
    painter.setBrush(QColor(color.red(), color.green(), color.blue(), 210));
    painter.drawEllipse(point, 9, 9);
    painter.setPen(QPen(color, 2));
    painter.drawLine(point + QPointF(-14, 0), point + QPointF(14, 0));
    painter.drawLine(point + QPointF(0, -14), point + QPointF(0, 14));
    painter.setPen(Qt::white);
    const qint16 azimuthCode =
        DirectionControlMapper::azimuthCode(
            target.azimuthDegrees, m_mapping);
    const qint16 elevationCode =
        DirectionControlMapper::elevationCode(
            target.elevationDegrees, m_mapping);
    painter.drawText(
        point + QPointF(13, -10),
        QStringLiteral("%1  %2°/%3°  [%4,%5]")
            .arg(name)
            .arg(direction.azimuthDegrees, 0, 'f', 1)
            .arg(direction.elevationDegrees, 0, 'f', 1)
            .arg(azimuthCode)
            .arg(elevationCode));
}

} // namespace MicArray
