#include "ui/noisereductionvideowidget.h"

#include "analysis/directionprojection.h"
#include "control/directioncontrolmapper.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <array>
#include <limits>

namespace MicArray {

NoiseReductionVideoWidget::NoiseReductionVideoWidget(QWidget *parent)
    : VideoPreviewWidget(parent)
{
    qRegisterMetaType<NoiseDirectionSelection>(
        "MicArray::NoiseDirectionSelection");
    setObjectName(QStringLiteral("noiseReductionVideoView"));
}

void NoiseReductionVideoWidget::setCameraView(
    const CameraViewSettings &camera)
{
    m_camera = camera;
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
        const auto hit = hitTarget(event->position());
        if (hit.has_value()) {
            m_dragSelection = *hit;
            m_selection = *hit;
            emit directionDragStarted(*hit);
            update();
            event->accept();
            return;
        }
        const auto normalized = normalizedVideoPoint(event->position());
        if (normalized.has_value()) {
            emit videoPointClicked(*normalized);
            event->accept();
            return;
        }
    }
    VideoPreviewWidget::mousePressEvent(event);
}

void NoiseReductionVideoWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragSelection.has_value()
        && (event->buttons() & Qt::LeftButton)) {
        const auto normalized =
            clampedNormalizedPoint(event->position());
        if (normalized.has_value()) {
            emit directionDragged(*m_dragSelection, *normalized);
        }
        event->accept();
        return;
    }
    VideoPreviewWidget::mouseMoveEvent(event);
}

void NoiseReductionVideoWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton
        && m_dragSelection.has_value()) {
        const NoiseDirectionSelection selection = *m_dragSelection;
        m_dragSelection.reset();
        const auto normalized =
            clampedNormalizedPoint(event->position());
        if (normalized.has_value()) {
            emit directionDragFinished(selection, *normalized);
        }
        event->accept();
        return;
    }
    VideoPreviewWidget::mouseReleaseEvent(event);
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
    drawTarget(painter, QStringLiteral("A"), QColor(70, 225, 125),
               m_enhancedA, NoiseDirectionSelection::EnhancedA);
    drawTarget(painter, QStringLiteral("B1"), QColor(255, 165, 55),
               m_suppressionB1, NoiseDirectionSelection::SuppressionB1);
    drawTarget(painter, QStringLiteral("B2"), QColor(225, 75, 205),
               m_suppressionB2, NoiseDirectionSelection::SuppressionB2);
}

std::optional<QPointF> NoiseReductionVideoWidget::targetPoint(
    const ContinuousDirectionTarget &target) const
{
    const QRectF rectangle = videoRect();
    if (rectangle.isEmpty()) {
        return std::nullopt;
    }
    SoundSourceDirection direction;
    direction.azimuthDegrees = target.azimuthDegrees;
    direction.elevationDegrees = target.elevationDegrees;
    const ProjectionResult projected =
        projectDirectionAngular(direction, m_camera);
    if (!projected.valid || !projected.inFront) {
        return std::nullopt;
    }
    const QPointF normalized =
        projected.inView
            ? projected.normalizedPoint : projected.edgePoint;
    return QPointF(
        rectangle.left() + normalized.x() * rectangle.width(),
        rectangle.top() + normalized.y() * rectangle.height());
}

std::optional<NoiseDirectionSelection>
NoiseReductionVideoWidget::hitTarget(const QPointF &point) const
{
    if (!m_markersVisible || !m_mapping.isValid()) {
        return std::nullopt;
    }
    const std::array<std::pair<NoiseDirectionSelection,
                               const ContinuousDirectionTarget *>, 3>
        targets {{
            {NoiseDirectionSelection::EnhancedA, &m_enhancedA},
            {NoiseDirectionSelection::SuppressionB1, &m_suppressionB1},
            {NoiseDirectionSelection::SuppressionB2, &m_suppressionB2},
        }};
    constexpr double HitRadiusSquared = 16.0 * 16.0;
    double closestDistance = std::numeric_limits<double>::max();
    std::optional<NoiseDirectionSelection> closest;
    for (const auto &[selection, target] : targets) {
        const auto marker = targetPoint(*target);
        if (!marker.has_value()) {
            continue;
        }
        const double dx = marker->x() - point.x();
        const double dy = marker->y() - point.y();
        const double distance = dx * dx + dy * dy;
        if (distance > HitRadiusSquared) {
            continue;
        }
        if (distance < closestDistance - 1.0e-6
            || (qAbs(distance - closestDistance) <= 1.0e-6
                && selection == m_selection)) {
            closestDistance = distance;
            closest = selection;
        }
    }
    return closest;
}

std::optional<QPointF>
NoiseReductionVideoWidget::clampedNormalizedPoint(
    const QPointF &point) const
{
    const QRectF rectangle = videoRect();
    if (rectangle.isEmpty()) {
        return std::nullopt;
    }
    const QPointF clamped(
        std::clamp(point.x(), rectangle.left(), rectangle.right()),
        std::clamp(point.y(), rectangle.top(), rectangle.bottom()));
    return normalizedVideoPoint(clamped);
}

void NoiseReductionVideoWidget::drawTarget(
    QPainter &painter,
    const QString &name,
    const QColor &color,
    const ContinuousDirectionTarget &target,
    NoiseDirectionSelection selection)
{
    const auto projectedPoint = targetPoint(target);
    if (!projectedPoint.has_value()) {
        return;
    }
    const QPointF point = *projectedPoint;
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
            .arg(target.azimuthDegrees, 0, 'f', 1)
            .arg(target.elevationDegrees, 0, 'f', 1)
            .arg(azimuthCode)
            .arg(elevationCode));
}

} // namespace MicArray
