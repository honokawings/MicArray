#include "analysis/cameraviewcalibrator.h"

#include "analysis/directionprojection.h"

#include <QLineF>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace MicArray {
namespace {

using Parameters = std::array<double, 5>;
using Matrix = std::array<std::array<double, 5>, 5>;

CameraViewSettings cameraFromParameters(const Parameters &parameters,
                                        const CameraViewSettings &base)
{
    CameraViewSettings camera = base;
    camera.centerAzimuthDegrees = parameters[0];
    camera.centerElevationDegrees = parameters[1];
    camera.horizontalFovDegrees = parameters[2];
    camera.verticalFovDegrees = parameters[3];
    camera.rollDegrees = parameters[4];
    return camera;
}

Parameters parametersFromCamera(const CameraViewSettings &camera)
{
    return {camera.centerAzimuthDegrees,
            camera.centerElevationDegrees,
            camera.horizontalFovDegrees,
            camera.verticalFovDegrees,
            camera.rollDegrees};
}

void clampParameters(Parameters *parameters)
{
    (*parameters)[0] = std::clamp((*parameters)[0], -180.0, 180.0);
    (*parameters)[1] = std::clamp((*parameters)[1], -89.0, 89.0);
    (*parameters)[2] = std::clamp((*parameters)[2], 2.0, 178.0);
    (*parameters)[3] = std::clamp((*parameters)[3], 2.0, 178.0);
    (*parameters)[4] = std::clamp((*parameters)[4], -180.0, 180.0);
}

bool residualsFor(const QVector<VideoCalibrationSample> &samples,
                  const Parameters &parameters,
                  const CameraViewSettings &base,
                  QVector<double> *residuals,
                  double *cost)
{
    residuals->clear();
    *cost = 0.0;
    const CameraViewSettings camera = cameraFromParameters(parameters, base);
    if (!camera.isValid()) {
        return false;
    }
    for (const VideoCalibrationSample &sample : samples) {
        SoundSourceDirection direction;
        direction.azimuthDegrees = sample.direction.azimuthDegrees;
        direction.elevationDegrees = sample.direction.elevationDegrees;
        const ProjectionResult projected = projectDirection(direction, camera);
        if (!projected.valid || !projected.inFront
            || !std::isfinite(projected.normalizedPoint.x())
            || !std::isfinite(projected.normalizedPoint.y())) {
            return false;
        }
        const double weight = std::clamp(sample.confidence, 0.1, 1.0);
        const double dx = projected.normalizedPoint.x() - sample.normalizedVideoPoint.x();
        const double dy = projected.normalizedPoint.y() - sample.normalizedVideoPoint.y();
        residuals->append(dx * std::sqrt(weight));
        residuals->append(dy * std::sqrt(weight));
        *cost += weight * (dx * dx + dy * dy);
    }
    return true;
}

bool solveLinear(Matrix matrix, Parameters rhs, Parameters *solution)
{
    for (int pivot = 0; pivot < 5; ++pivot) {
        int best = pivot;
        for (int row = pivot + 1; row < 5; ++row) {
            if (std::abs(matrix[row][pivot]) > std::abs(matrix[best][pivot])) {
                best = row;
            }
        }
        if (std::abs(matrix[best][pivot]) < 1.0e-12) {
            return false;
        }
        std::swap(matrix[pivot], matrix[best]);
        std::swap(rhs[pivot], rhs[best]);
        const double divisor = matrix[pivot][pivot];
        for (int column = pivot; column < 5; ++column) {
            matrix[pivot][column] /= divisor;
        }
        rhs[pivot] /= divisor;
        for (int row = 0; row < 5; ++row) {
            if (row == pivot) {
                continue;
            }
            const double factor = matrix[row][pivot];
            for (int column = pivot; column < 5; ++column) {
                matrix[row][column] -= factor * matrix[pivot][column];
            }
            rhs[row] -= factor * rhs[pivot];
        }
    }
    *solution = rhs;
    return true;
}

bool observationsAreValid(const QVector<VideoCalibrationSample> &samples)
{
    if (samples.size() != 4) {
        return false;
    }
    for (int first = 0; first < samples.size(); ++first) {
        const auto &sample = samples.at(first);
        if (!std::isfinite(sample.direction.azimuthDegrees)
            || !std::isfinite(sample.direction.elevationDegrees)
            || !std::isfinite(sample.normalizedVideoPoint.x())
            || !std::isfinite(sample.normalizedVideoPoint.y())
            || sample.normalizedVideoPoint.x() < 0.0
            || sample.normalizedVideoPoint.x() > 1.0
            || sample.normalizedVideoPoint.y() < 0.0
            || sample.normalizedVideoPoint.y() > 1.0) {
            return false;
        }
        for (int second = first + 1; second < samples.size(); ++second) {
            const auto &other = samples.at(second);
            const double angleDistance = std::hypot(
                sample.direction.azimuthDegrees - other.direction.azimuthDegrees,
                sample.direction.elevationDegrees - other.direction.elevationDegrees);
            if (angleDistance < 2.0
                || QLineF(sample.normalizedVideoPoint,
                          other.normalizedVideoPoint).length() < 0.02) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

CameraViewFitResult fitCameraView(const QVector<VideoCalibrationSample> &samples,
                                  const CameraViewSettings &initial,
                                  const QSize &videoSize)
{
    CameraViewFitResult result;
    if (initial.lens.enabled && !initial.lens.isValid())
    {
        result.error = QStringLiteral("镜头参数无效");
        return result;
    }
    if (!observationsAreValid(samples) || videoSize.width() <= 0 || videoSize.height() <= 0) {
        result.error = QStringLiteral("四点校准数据不足或位置退化");
        return result;
    }

    CameraViewSettings neutral;
    neutral.lens = initial.lens;
    QVector<CameraViewSettings> starts {initial, neutral};
    CameraViewSettings wider = initial;
    wider.horizontalFovDegrees = std::clamp(initial.horizontalFovDegrees + 20.0, 10.0, 170.0);
    wider.verticalFovDegrees = std::clamp(initial.verticalFovDegrees + 15.0, 10.0, 170.0);
    starts.append(wider);

    double bestCost = std::numeric_limits<double>::infinity();
    Parameters bestParameters {};
    for (const CameraViewSettings &start : std::as_const(starts)) {
        Parameters parameters = parametersFromCamera(start.isValid() ? start : neutral);
        clampParameters(&parameters);
        double lambda = 1.0e-3;
        QVector<double> residuals;
        double cost = 0.0;
        if (!residualsFor(samples, parameters, initial, &residuals, &cost)) {
            continue;
        }
        for (int iteration = 0; iteration < 100; ++iteration) {
            QVector<std::array<double, 5>> jacobian(residuals.size());
            for (int parameter = 0; parameter < 5; ++parameter) {
                if (initial.lens.enabled && (parameter == 2 || parameter == 3))
                {
                    continue;
                }
                Parameters shifted = parameters;
                const double step = parameter >= 2 && parameter <= 3 ? 0.02 : 0.01;
                shifted[parameter] += step;
                clampParameters(&shifted);
                QVector<double> shiftedResiduals;
                double shiftedCost = 0.0;
                if (!residualsFor(samples, shifted, initial, &shiftedResiduals, &shiftedCost)
                    || shiftedResiduals.size() != residuals.size()) {
                    continue;
                }
                const double actualStep = shifted[parameter] - parameters[parameter];
                if (std::abs(actualStep) < 1.0e-12) {
                    continue;
                }
                for (int row = 0; row < residuals.size(); ++row) {
                    jacobian[row][parameter] =
                        (shiftedResiduals.at(row) - residuals.at(row)) / actualStep;
                }
            }
            Matrix normal {};
            Parameters rhs {};
            for (int row = 0; row < residuals.size(); ++row) {
                for (int left = 0; left < 5; ++left) {
                    rhs[left] -= jacobian.at(row)[left] * residuals.at(row);
                    for (int right = 0; right < 5; ++right) {
                        normal[left][right] +=
                            jacobian.at(row)[left] * jacobian.at(row)[right];
                    }
                }
            }
            for (int diagonal = 0; diagonal < 5; ++diagonal) {
                normal[diagonal][diagonal] += lambda;
            }
            Parameters delta {};
            if (!solveLinear(normal, rhs, &delta)) {
                break;
            }
            Parameters candidate = parameters;
            double deltaNorm = 0.0;
            for (int parameter = 0; parameter < 5; ++parameter) {
                candidate[parameter] += delta[parameter];
                deltaNorm += delta[parameter] * delta[parameter];
            }
            clampParameters(&candidate);
            QVector<double> candidateResiduals;
            double candidateCost = 0.0;
            if (residualsFor(samples, candidate, initial, &candidateResiduals, &candidateCost)
                && candidateCost < cost) {
                parameters = candidate;
                residuals = candidateResiduals;
                cost = candidateCost;
                lambda = std::max(1.0e-9, lambda * 0.3);
                if (deltaNorm < 1.0e-12) {
                    break;
                }
            } else {
                lambda = std::min(1.0e9, lambda * 10.0);
            }
        }
        if (cost < bestCost) {
            bestCost = cost;
            bestParameters = parameters;
        }
    }

    if (!std::isfinite(bestCost)) {
        result.error = QStringLiteral("视频视角参数拟合未收敛");
        return result;
    }
    result.camera = cameraFromParameters(bestParameters, initial);
    double squaredPixels = 0.0;
    double maximumResidual = 0.0;
    for (const VideoCalibrationSample &sample : samples) {
        SoundSourceDirection direction;
        direction.azimuthDegrees = sample.direction.azimuthDegrees;
        direction.elevationDegrees = sample.direction.elevationDegrees;
        const ProjectionResult projected = projectDirection(direction, result.camera);
        const double dx = (projected.normalizedPoint.x() - sample.normalizedVideoPoint.x())
            * videoSize.width();
        const double dy = (projected.normalizedPoint.y() - sample.normalizedVideoPoint.y())
            * videoSize.height();
        const double residual = std::hypot(dx, dy);
        result.residualPixels.append(residual);
        squaredPixels += residual * residual;
        maximumResidual = std::max(maximumResidual, residual);
    }
    result.rmsPixels = std::sqrt(squaredPixels / samples.size());
    result.valid = result.camera.isValid()
        && result.rmsPixels <= 25.0 && maximumResidual <= 50.0;
    if (!result.valid) {
        result.error = QStringLiteral("四点拟合误差过大：RMS %1 px")
                           .arg(result.rmsPixels, 0, 'f', 1);
    }
    return result;
}

} // namespace MicArray
