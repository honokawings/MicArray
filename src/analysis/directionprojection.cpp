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

double distortedAngle(double theta, const CameraLensCalibration &lens)
{
    const double squared = theta * theta;
    const auto &k = lens.coefficients;
    return theta * (1.0 + squared * (k[0] + squared
        * (k[1] + squared * (k[2] + squared * k[3]))));
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

bool angularCameraIsValid(const CameraViewSettings &camera)
{
    return finite(camera.centerAzimuthDegrees)
        && camera.centerAzimuthDegrees >= -180.0
        && camera.centerAzimuthDegrees <= 180.0
        && finite(camera.centerElevationDegrees)
        && camera.centerElevationDegrees >= -90.0
        && camera.centerElevationDegrees <= 90.0
        && finite(camera.horizontalFovDegrees)
        && camera.horizontalFovDegrees > 1.0
        && camera.horizontalFovDegrees <= 180.0
        && finite(camera.verticalFovDegrees)
        && camera.verticalFovDegrees > 1.0
        && camera.verticalFovDegrees <= 180.0
        && finite(camera.rollDegrees)
        && camera.rollDegrees >= -180.0
        && camera.rollDegrees <= 180.0;
}

} // namespace

bool CameraLensCalibration::isValid() const
{
    if (!finite(fx) || !finite(fy) || fx <= 0.0 || fy <= 0.0
        || fx > 10.0 || fy > 10.0
        || !finite(cx) || !finite(cy) || cx < 0.0 || cx > 1.0
        || cy < 0.0 || cy > 1.0
        || referenceWidth <= 0 || referenceHeight <= 0
        || referenceWidth > 65535 || referenceHeight > 65535)
    {
        return false;
    }
    // A conservative analytic lower bound on d(theta_d)/d(theta).
    // Reject folded mappings throughout the forward hemisphere, including
    // polynomials whose finite coefficients would otherwise look acceptable.
    double lowerDerivative = 1.0;
    const double limitSquared = (M_PI / 2.0) * (M_PI / 2.0);
    double power = limitSquared;
    for (int index = 0; index < 4; ++index)
    {
        const double k = coefficients[index];
        if (!finite(k) || std::abs(k) > 100.0)
        {
            return false;
        }
        lowerDerivative += std::min(0.0, (2 * index + 3) * k * power);
        power *= limitSquared;
    }
    return lowerDerivative > 1.0e-6;
}

bool CameraViewSettings::isValid() const
{
    return finite(centerAzimuthDegrees)
        && centerAzimuthDegrees >= -180.0 && centerAzimuthDegrees <= 180.0
        && finite(centerElevationDegrees)
        && centerElevationDegrees >= -90.0 && centerElevationDegrees <= 90.0
        && finite(horizontalFovDegrees)
        && horizontalFovDegrees > 1.0 && horizontalFovDegrees <= 179.0
        && finite(verticalFovDegrees)
        && verticalFovDegrees > 1.0 && verticalFovDegrees <= 179.0
        && finite(rollDegrees)
        && rollDegrees >= -180.0 && rollDegrees <= 180.0
        && (!lens.enabled || lens.isValid());
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
    QPointF point(0.5 + xc / (zc * horizontalScale),
                  0.5 - yc / (zc * verticalScale));
    if (camera.lens.enabled)
    {
        const double radius = std::hypot(xc, yc);
        const double scale = radius > 1.0e-12
            ? distortedAngle(std::atan2(radius, zc), camera.lens) / radius
            : 1.0 / zc;
        point = {camera.lens.cx + camera.lens.fx * xc * scale,
                 camera.lens.cy - camera.lens.fy * yc * scale};
    }
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

ScanDirection unprojectVideoPoint(const QPointF &normalizedPoint,
                                  const CameraViewSettings &camera)
{
    const double invalid = std::numeric_limits<double>::quiet_NaN();
    if (!camera.isValid()
        || !finite(normalizedPoint.x()) || !finite(normalizedPoint.y())) {
        return {invalid, invalid};
    }
    const QVector3D forward = directionFromAngles(camera.centerAzimuthDegrees,
                                                  camera.centerElevationDegrees).normalized();
    QVector3D right = QVector3D::crossProduct(QVector3D(0.0f, 1.0f, 0.0f), forward);
    if (right.lengthSquared() < 1.0e-12f) {
        right = QVector3D(1.0f, 0.0f, 0.0f);
    } else {
        right.normalize();
    }
    const QVector3D up = QVector3D::crossProduct(forward, right).normalized();
    const double roll = qDegreesToRadians(camera.rollDegrees);
    const float cosine = static_cast<float>(std::cos(roll));
    const float sine = static_cast<float>(std::sin(roll));
    const QVector3D rolledRight = right * cosine + up * sine;
    const QVector3D rolledUp = up * cosine - right * sine;
    if (camera.lens.enabled)
    {
        const double x = (normalizedPoint.x() - camera.lens.cx) / camera.lens.fx;
        const double y = (camera.lens.cy - normalizedPoint.y()) / camera.lens.fy;
        const double radius = std::hypot(x, y);
        double low = 0.0;
        double high = M_PI / 2.0 - 1.0e-9;
        if (!finite(radius) || radius >= distortedAngle(high, camera.lens))
        {
            return {invalid, invalid};
        }
        for (int iteration = 0; iteration < 48; ++iteration)
        {
            const double middle = (low + high) / 2.0;
            if (distortedAngle(middle, camera.lens) < radius)
            {
                low = middle;
            }
            else
            {
                high = middle;
            }
        }
        const double theta = (low + high) / 2.0;
        const double radialScale = radius > 1.0e-12 ? std::sin(theta) / radius : 1.0;
        const QVector3D ray = (forward * static_cast<float>(std::cos(theta))
            + rolledRight * static_cast<float>(x * radialScale)
            + rolledUp * static_cast<float>(y * radialScale)).normalized();
        const auto angles = anglesFromDirection(ray);
        return {angles.azimuthDegrees, angles.elevationDegrees};
    }
    const double horizontalScale = 2.0
        * std::tan(qDegreesToRadians(camera.horizontalFovDegrees) / 2.0);
    const double verticalScale = 2.0
        * std::tan(qDegreesToRadians(camera.verticalFovDegrees) / 2.0);
    const double xc = (normalizedPoint.x() - 0.5) * horizontalScale;
    const double yc = (0.5 - normalizedPoint.y()) * verticalScale;
    const QVector3D direction =
        (forward + rolledRight * static_cast<float>(xc)
         + rolledUp * static_cast<float>(yc)).normalized();
    const SoundSourceDirection angles = anglesFromDirection(direction);
    return {angles.azimuthDegrees, angles.elevationDegrees};
}

ProjectionResult projectDirectionAngular(
    const SoundSourceDirection &direction,
    const CameraViewSettings &camera)
{
    if (camera.lens.enabled)
    {
        return projectDirection(direction, camera);
    }
    ProjectionResult result;
    if (!angularCameraIsValid(camera)
        || !finite(direction.azimuthDegrees)
        || !finite(direction.elevationDegrees)) {
        return result;
    }
    result.valid = true;

    const QVector3D source =
        directionFromAngles(direction.azimuthDegrees,
                            direction.elevationDegrees).normalized();
    const QVector3D forward =
        directionFromAngles(camera.centerAzimuthDegrees,
                            camera.centerElevationDegrees).normalized();
    QVector3D right = QVector3D::crossProduct(
        QVector3D(0.0f, 1.0f, 0.0f), forward);
    if (right.lengthSquared() < 1.0e-12f) {
        right = QVector3D(1.0f, 0.0f, 0.0f);
    } else {
        right.normalize();
    }
    const QVector3D up =
        QVector3D::crossProduct(forward, right).normalized();
    const double roll = qDegreesToRadians(camera.rollDegrees);
    const float cosine = static_cast<float>(std::cos(roll));
    const float sine = static_cast<float>(std::sin(roll));
    const QVector3D rolledRight = right * cosine + up * sine;
    const QVector3D rolledUp = up * cosine - right * sine;

    const double xc = QVector3D::dotProduct(source, rolledRight);
    const double yc = QVector3D::dotProduct(source, rolledUp);
    const double zc = QVector3D::dotProduct(source, forward);
    const double horizontalDegrees =
        qRadiansToDegrees(std::atan2(xc, zc));
    const double verticalDegrees =
        qRadiansToDegrees(std::atan2(yc, std::hypot(xc, zc)));
    constexpr double EdgeTolerance = 1.0e-6;
    result.inFront =
        std::abs(horizontalDegrees) <= 90.0 + EdgeTolerance;
    if (!result.inFront) {
        return result;
    }

    const QPointF point(
        0.5 + horizontalDegrees / camera.horizontalFovDegrees,
        0.5 - verticalDegrees / camera.verticalFovDegrees);
    result.normalizedPoint = point;
    result.inView =
        point.x() >= -EdgeTolerance
        && point.x() <= 1.0 + EdgeTolerance
        && point.y() >= -EdgeTolerance
        && point.y() <= 1.0 + EdgeTolerance;
    if (result.inView) {
        result.normalizedPoint.setX(
            std::clamp(point.x(), 0.0, 1.0));
        result.normalizedPoint.setY(
            std::clamp(point.y(), 0.0, 1.0));
        result.edgePoint = result.normalizedPoint;
    } else {
        result.edgePoint = edgeIntersection(point);
    }
    return result;
}

ScanDirection unprojectVideoPointAngular(
    const QPointF &normalizedPoint,
    const CameraViewSettings &camera)
{
    if (camera.lens.enabled)
    {
        return unprojectVideoPoint(normalizedPoint, camera);
    }
    const double invalid =
        std::numeric_limits<double>::quiet_NaN();
    if (!angularCameraIsValid(camera)
        || !finite(normalizedPoint.x())
        || !finite(normalizedPoint.y())) {
        return {invalid, invalid};
    }

    const QVector3D forward =
        directionFromAngles(camera.centerAzimuthDegrees,
                            camera.centerElevationDegrees).normalized();
    QVector3D right = QVector3D::crossProduct(
        QVector3D(0.0f, 1.0f, 0.0f), forward);
    if (right.lengthSquared() < 1.0e-12f) {
        right = QVector3D(1.0f, 0.0f, 0.0f);
    } else {
        right.normalize();
    }
    const QVector3D up =
        QVector3D::crossProduct(forward, right).normalized();
    const double roll = qDegreesToRadians(camera.rollDegrees);
    const float rollCosine = static_cast<float>(std::cos(roll));
    const float rollSine = static_cast<float>(std::sin(roll));
    const QVector3D rolledRight =
        right * rollCosine + up * rollSine;
    const QVector3D rolledUp =
        up * rollCosine - right * rollSine;

    const double horizontal = qDegreesToRadians(
        (normalizedPoint.x() - 0.5)
        * camera.horizontalFovDegrees);
    const double vertical = qDegreesToRadians(
        (0.5 - normalizedPoint.y())
        * camera.verticalFovDegrees);
    const float horizontalCosine =
        static_cast<float>(std::cos(horizontal));
    const float horizontalSine =
        static_cast<float>(std::sin(horizontal));
    const float verticalCosine =
        static_cast<float>(std::cos(vertical));
    const float verticalSine =
        static_cast<float>(std::sin(vertical));
    const QVector3D direction =
        (forward * (verticalCosine * horizontalCosine)
         + rolledRight * (verticalCosine * horizontalSine)
         + rolledUp * verticalSine).normalized();
    const SoundSourceDirection angles =
        anglesFromDirection(direction);
    return {angles.azimuthDegrees, angles.elevationDegrees};
}

} // namespace MicArray
