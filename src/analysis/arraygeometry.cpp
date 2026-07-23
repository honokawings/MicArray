#include "analysis/arraygeometry.h"

#include <QtMath>

#include <cmath>

namespace MicArray {

QVector<MicrophonePosition> createDefaultArrayGeometry()
{
    QVector<MicrophonePosition> microphones;
    microphones.reserve(48);
    for (int channel = 0; channel < 48; ++channel) {
        const int row = channel / 8;
        const int column = channel % 8;
        microphones.append({(static_cast<double>(column) - 3.5) * 0.05,
                            (2.5 - static_cast<double>(row)) * 0.05,
                            0.0});
    }
    return microphones;
}

QVector3D directionFromAngles(double azimuthDegrees, double elevationDegrees)
{
    const double azimuth = qDegreesToRadians(azimuthDegrees);
    const double elevation = qDegreesToRadians(elevationDegrees);
    return QVector3D(static_cast<float>(std::sin(azimuth) * std::cos(elevation)),
                     static_cast<float>(std::sin(elevation)),
                     static_cast<float>(std::cos(azimuth) * std::cos(elevation)));
}

SoundSourceDirection anglesFromDirection(const QVector3D &direction)
{
    SoundSourceDirection result;
    const QVector3D normalized = direction.normalized();
    result.azimuthDegrees = qRadiansToDegrees(std::atan2(static_cast<double>(normalized.x()),
                                                         static_cast<double>(normalized.z())));
    result.elevationDegrees = qRadiansToDegrees(
        std::atan2(static_cast<double>(normalized.y()),
                   std::hypot(static_cast<double>(normalized.x()),
                              static_cast<double>(normalized.z()))));
    return result;
}

} // namespace MicArray
