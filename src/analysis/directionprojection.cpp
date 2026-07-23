#include "analysis/directionprojection.h"

#include "analysis/arraygeometry.h"

#include <QVector3D>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace MicArray {
namespace {

bool finite(double value)
{
    return std::isfinite(value);
}

QPointF edgeIntersection(const QPointF &point)
{
    const double dx = point.x() - 0.5;
    const double dy = point.y() - 0.5;
    if (std::abs(dx) < 1.0e-12 && std::abs(dy) < 1.0e-12) {
        return {0.5, 0.5};
    }
    double scale = std::numeric_limits<double>::infinity();
    if (std::abs(dx) >= 1.0e-12) {
        scale = std::min(scale, 0.5 / std::abs(dx));
    }
    if (std::abs(dy) >= 1.0e-12) {
        scale = std::min(scale, 0.5 / std::abs(dy));
    }
    return {std::clamp(0.5 + dx * scale, 0.0, 1.0),
            std::clamp(0.5 + dy * scale, 0.0, 1.0)};
}

} // namespace

bool CameraViewSettings::isValid() const
{
    return finite(centerAzimuthDegrees)
        && centerAzimuthDegrees >= -180.0 && centerAzimuthDegrees <= 180.0
        && finite(centerElevationDegrees)
        && centerElevationDegrees >= -90.0 && centerElevationDegrees <= 90.0
        && finite(horizontalFovDegrees)
        && horizontalFovDegrees > 1.0 && horizontalFovDegrees < 179.0
        && finite(verticalFovDegrees)
        && verticalFovDegrees > 1.0 && verticalFovDegrees < 179.0
        && finite(rollDegrees)
        && rollDegrees >= -180.0 && rollDegrees <= 180.0;
}

ProjectionResult projectDirection(const SoundSourceDirection &direction,
                                  const CameraViewSettings &camera)
{
    ProjectionResult result;
    if (!camera.isValid()
        || !finite(direction.azimuthDegrees)
        || !finite(direction.elevationDegrees)) {
        return result;
    }
    result.valid = true;

    const QVector3D source = directionFromAngles(direction.azimuthDegrees,
                                                 direction.elevationDegrees).normalized();
    const QVector3D forward = directionFromAngles(camera.centerAzimuthDegrees,
                                                  camera.centerElevationDegrees).normalized();
    QVector3D right = QVector3D::crossProduct(QVector3D(0.0f, 1.0f, 0.0f), forward);
    if (right.lengthSquared() < 1.0e-12f) {
        right = QVector3D(1.0f, 0.0f, 0.0f);
    } else {
        right.normalize();
    }
    QVector3D up = QVector3D::crossProduct(forward, right).normalized();

    const double roll = qDegreesToRadians(camera.rollDegrees);
    const float cosine = static_cast<float>(std::cos(roll));
    const float sine = static_cast<float>(std::sin(roll));
    const QVector3D rolledRight = right * cosine + up * sine;
    const QVector3D rolledUp = up * cosine - right * sine;

    const double xc = QVector3D::dotProduct(source, rolledRight);
    const double yc = QVector3D::dotProduct(source, rolledUp);
    const double zc = QVector3D::dotProduct(source, forward);
    result.inFront = zc > 1.0e-9;
    if (!result.inFront) {
        return result;
    }

    const double horizontalScale = 2.0
        * std::tan(qDegreesToRadians(camera.horizontalFovDegrees) / 2.0);
    const double verticalScale = 2.0
        * std::tan(qDegreesToRadians(camera.verticalFovDegrees) / 2.0);
    const QPointF point(0.5 + xc / (zc * horizontalScale),
                        0.5 - yc / (zc * verticalScale));
    result.normalizedPoint = point;
    constexpr double EdgeTolerance = 1.0e-6;
    result.inView = point.x() >= -EdgeTolerance && point.x() <= 1.0 + EdgeTolerance
        && point.y() >= -EdgeTolerance && point.y() <= 1.0 + EdgeTolerance;
    if (result.inView) {
        result.normalizedPoint.setX(std::clamp(point.x(), 0.0, 1.0));
        result.normalizedPoint.setY(std::clamp(point.y(), 0.0, 1.0));
        result.edgePoint = result.normalizedPoint;
    } else {
        result.edgePoint = edgeIntersection(point);
    }
    return result;
}

} // namespace MicArray
