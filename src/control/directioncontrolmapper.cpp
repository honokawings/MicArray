#include "control/directioncontrolmapper.h"

#include <algorithm>
#include <cmath>

namespace MicArray {
namespace {

qint16 angleToCode(double degrees,
                   double minimumDegrees,
                   double maximumDegrees,
                   int minimumCode,
                   int maximumCode)
{
    if (!std::isfinite(degrees)
        || !std::isfinite(minimumDegrees)
        || !std::isfinite(maximumDegrees)
        || minimumDegrees >= maximumDegrees) {
        return 0;
    }
    const double clamped = std::clamp(degrees, minimumDegrees, maximumDegrees);
    const double normalized =
        (clamped - minimumDegrees) / (maximumDegrees - minimumDegrees);
    return static_cast<qint16>(std::clamp(
        std::lround(minimumCode
                    + normalized * static_cast<double>(maximumCode - minimumCode)),
        static_cast<long>(minimumCode),
        static_cast<long>(maximumCode)));
}

double codeToAngle(qint16 code,
                   double minimumDegrees,
                   double maximumDegrees,
                   int minimumCode,
                   int maximumCode)
{
    if (!std::isfinite(minimumDegrees)
        || !std::isfinite(maximumDegrees)
        || minimumDegrees >= maximumDegrees) {
        return 0.0;
    }
    const int clamped = std::clamp(static_cast<int>(code),
                                   minimumCode,
                                   maximumCode);
    const double normalized =
        static_cast<double>(clamped - minimumCode)
        / static_cast<double>(maximumCode - minimumCode);
    return minimumDegrees + normalized * (maximumDegrees - minimumDegrees);
}

} // namespace

bool DirectionControlMapping::isValid() const
{
    return std::isfinite(azimuthMinimumDegrees)
        && std::isfinite(azimuthMaximumDegrees)
        && std::isfinite(elevationMinimumDegrees)
        && std::isfinite(elevationMaximumDegrees)
        && azimuthMinimumDegrees >= -180.0
        && azimuthMaximumDegrees <= 180.0
        && azimuthMinimumDegrees < azimuthMaximumDegrees
        && elevationMinimumDegrees >= -90.0
        && elevationMaximumDegrees <= 90.0
        && elevationMinimumDegrees < elevationMaximumDegrees;
}

qint16 DirectionControlMapper::azimuthCode(
    double degrees, const DirectionControlMapping &mapping)
{
    if (!mapping.isValid()) {
        return 0;
    }
    return angleToCode(degrees,
                       mapping.azimuthMinimumDegrees,
                       mapping.azimuthMaximumDegrees,
                       -4,
                       4);
}

qint16 DirectionControlMapper::elevationCode(
    double degrees, const DirectionControlMapping &mapping)
{
    if (!mapping.isValid()) {
        return 0;
    }
    return angleToCode(degrees,
                       mapping.elevationMinimumDegrees,
                       mapping.elevationMaximumDegrees,
                       -3,
                       3);
}

double DirectionControlMapper::azimuthDegrees(
    qint16 code, const DirectionControlMapping &mapping)
{
    if (!mapping.isValid()) {
        return 0.0;
    }
    return codeToAngle(code,
                       mapping.azimuthMinimumDegrees,
                       mapping.azimuthMaximumDegrees,
                       -4,
                       4);
}

double DirectionControlMapper::elevationDegrees(
    qint16 code, const DirectionControlMapping &mapping)
{
    if (!mapping.isValid()) {
        return 0.0;
    }
    return codeToAngle(code,
                       mapping.elevationMinimumDegrees,
                       mapping.elevationMaximumDegrees,
                       -3,
                       3);
}

ContinuousDirectionTarget DirectionControlMapper::clampDirection(
    const ContinuousDirectionTarget &target,
    const DirectionControlMapping &mapping)
{
    if (!mapping.isValid()) {
        return {};
    }
    ContinuousDirectionTarget result;
    result.azimuthDegrees = std::isfinite(target.azimuthDegrees)
        ? std::clamp(target.azimuthDegrees,
                     mapping.azimuthMinimumDegrees,
                     mapping.azimuthMaximumDegrees)
        : 0.0;
    result.elevationDegrees = std::isfinite(target.elevationDegrees)
        ? std::clamp(target.elevationDegrees,
                     mapping.elevationMinimumDegrees,
                     mapping.elevationMaximumDegrees)
        : 0.0;
    return result;
}

ControlValues DirectionControlMapper::controlValues(
    const ContinuousDirectionTarget &enhancedA,
    const ContinuousDirectionTarget &suppressionB1,
    const ContinuousDirectionTarget &suppressionB2,
    const DirectionControlMapping &mapping)
{
    ControlValues values;
    values.setChannelValue(
        0, azimuthCode(enhancedA.azimuthDegrees, mapping));
    values.setChannelValue(
        1, elevationCode(enhancedA.elevationDegrees, mapping));
    values.setChannelValue(
        2, azimuthCode(suppressionB1.azimuthDegrees, mapping));
    values.setChannelValue(
        3, elevationCode(suppressionB1.elevationDegrees, mapping));
    values.setChannelValue(
        4, azimuthCode(suppressionB2.azimuthDegrees, mapping));
    values.setChannelValue(
        5, elevationCode(suppressionB2.elevationDegrees, mapping));
    return values;
}

} // namespace MicArray
