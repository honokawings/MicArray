#include "ui/directionphotowidget.h"

#include "analysis/directionprojection.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace MicArray {

DirectionPhotoWidget::DirectionPhotoWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(360, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void DirectionPhotoWidget::setImage(const QImage &image)
{
    m_image = image;
    update();
}

void DirectionPhotoWidget::setCameraView(const CameraViewSettings &camera)
{
    m_camera = camera;
    update();
}

void DirectionPhotoWidget::setDirections(const QVector<SoundSourceDirection> &directions)
{
    m_directions = directions;
    update();
}

void DirectionPhotoWidget::setStatusText(const QString &text)
{
    m_statusText = text;
    update();
}

void DirectionPhotoWidget::setConfidenceThreshold(double threshold)
{
    m_confidenceThreshold = std::clamp(threshold, 0.0, 1.0);
    update();
}

QRectF DirectionPhotoWidget::imageRect() const
{
    if (m_image.isNull() || width() <= 0 || height() <= 0) {
        return QRectF(rect());
    }
    const QSizeF fitted = QSizeF(m_image.size()).scaled(size(), Qt::KeepAspectRatio);
    return QRectF((width() - fitted.width()) * 0.5,
                  (height() - fitted.height()) * 0.5,
                  fitted.width(),
                  fitted.height());
}

QVector<QPointF> DirectionPhotoWidget::markerPoints() const
{
    QVector<QPointF> points;
    const QRectF target = imageRect();
    for (const SoundSourceDirection &direction : m_directions) {
        if (direction.confidence < m_confidenceThreshold) {
            continue;
        }
        const ProjectionResult result = projectDirection(direction, m_camera);
        if (!result.valid || !result.inFront) {
            continue;
        }
        const QPointF normalized = result.inView ? result.normalizedPoint : result.edgePoint;
        points.append(QPointF(target.left() + normalized.x() * target.width(),
                              target.top() + normalized.y() * target.height()));
    }
    return points;
}

void DirectionPhotoWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(18, 22, 28));
    const QRectF target = imageRect();
    if (!m_image.isNull()) {
        painter.drawImage(target, m_image);
    } else {
        painter.setPen(QColor(105, 117, 132));
        painter.drawRect(target.adjusted(0, 0, -1, -1));
    }

    int markerIndex = 0;
    for (const SoundSourceDirection &direction : m_directions) {
        if (direction.confidence < m_confidenceThreshold) {
            continue;
        }
        const ProjectionResult projection = projectDirection(direction, m_camera);
        if (!projection.valid || !projection.inFront) {
            continue;
        }
        const QPointF normalized = projection.inView
                                       ? projection.normalizedPoint
                                       : projection.edgePoint;
        const QPointF point(target.left() + normalized.x() * target.width(),
                            target.top() + normalized.y() * target.height());
        const QColor color = markerIndex == 0 ? QColor(255, 70, 70) : QColor(255, 190, 55);
        painter.setPen(QPen(Qt::white, 2));
        painter.setBrush(QColor(color.red(), color.green(), color.blue(), 190));
        if (projection.inView) {
            painter.drawEllipse(point, 11, 11);
            painter.drawLine(point + QPointF(-18, 0), point + QPointF(18, 0));
            painter.drawLine(point + QPointF(0, -18), point + QPointF(0, 18));
        } else {
            const QPointF center = target.center();
            const QPointF vector = point - center;
            const double length = std::max(1.0, std::hypot(vector.x(), vector.y()));
            const QPointF unit = vector / length;
            const QPointF perpendicular(-unit.y(), unit.x());
            QPainterPath arrow;
            arrow.moveTo(point);
            arrow.lineTo(point - unit * 24.0 + perpendicular * 10.0);
            arrow.lineTo(point - unit * 24.0 - perpendicular * 10.0);
            arrow.closeSubpath();
            painter.drawPath(arrow);
        }
        painter.setPen(Qt::white);
        painter.drawText(point + QPointF(15, -12),
                         QStringLiteral("方位 %1°  俯仰 %2°  %3%")
                             .arg(direction.azimuthDegrees, 0, 'f', 1)
                             .arg(direction.elevationDegrees, 0, 'f', 1)
                             .arg(direction.confidence * 100.0, 0, 'f', 0));
        ++markerIndex;
    }

    if (!m_statusText.isEmpty()) {
        const QRectF statusRect = target.adjusted(12, 12, -12, -12);
        painter.setPen(Qt::white);
        painter.setBrush(QColor(0, 0, 0, 145));
        const QRectF textBounds = painter.boundingRect(statusRect,
                                                       Qt::AlignLeft | Qt::AlignBottom,
                                                       m_statusText);
        painter.drawRoundedRect(textBounds.adjusted(-8, -5, 8, 5), 4, 4);
        painter.drawText(statusRect, Qt::AlignLeft | Qt::AlignBottom, m_statusText);
    }
}

} // namespace MicArray
