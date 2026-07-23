#include "analysis/gccphatlocator.h"

#include "analysis/arraygeometry.h"
#include "analysis/radix2fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>

namespace MicArray {
namespace {

bool isPowerOfTwo(int value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

bool finitePosition(const MicrophonePosition &position)
{
    return std::isfinite(position.xMeters)
        && std::isfinite(position.yMeters)
        && std::isfinite(position.zMeters);
}

double correlationAtLag(const QVector<std::complex<double>> &correlation, int lag)
{
    const int index = lag >= 0 ? lag : correlation.size() + lag;
    return correlation.at(index).real();
}

} // namespace

LocatorDescriptor GccPhatLocator::descriptor() const
{
    return {QStringLiteral("gcc-phat-tdoa"),
            QStringLiteral("GCC-PHAT 时差定位"),
            QStringLiteral("宽带互相关、亚采样时延与鲁棒远场方向拟合")};
}

bool GccPhatLocator::configure(const LocatorConfiguration &configuration,
                               QString *errorMessage)
{
    const bool valid = configuration.microphones.size() == 48
        && configuration.sampleRate >= 8000 && configuration.sampleRate <= 192000
        && configuration.windowSamples >= 512 && configuration.windowSamples <= 8192
        && isPowerOfTwo(configuration.windowSamples)
        && std::isfinite(configuration.soundSpeedMetersPerSecond)
        && configuration.soundSpeedMetersPerSecond >= 300.0
        && configuration.soundSpeedMetersPerSecond <= 380.0
        && std::isfinite(configuration.confidenceThreshold)
        && configuration.confidenceThreshold >= 0.0
        && configuration.confidenceThreshold <= 1.0
        && std::isfinite(configuration.smoothingFactor)
        && configuration.smoothingFactor >= 0.0
        && configuration.smoothingFactor <= 1.0
        && std::all_of(configuration.microphones.cbegin(),
                       configuration.microphones.cend(),
                       finitePosition);
    if (!valid) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("GCC-PHAT 定位配置无效");
        }
        return false;
    }
    m_configuration = configuration;
    m_configured = true;
    m_hasSmoothedDirection = false;
    rebuildPairs();
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

QVector<SoundSourceDirection> GccPhatLocator::locate(const MultiChannelAudioWindow &window)
{
    if (!m_configured
        || window.sampleRate != m_configuration.sampleRate
        || window.channels.size() != m_configuration.microphones.size()) {
        return {};
    }
    const int sampleCount = m_configuration.windowSamples;
    const int fftSize = sampleCount * 2;
    QVector<QVector<std::complex<double>>> spectra;
    spectra.reserve(window.channels.size());
    QVector<double> energies;
    energies.reserve(window.channels.size());

    double totalEnergy = 0.0;
    for (const QVector<float> &channel : window.channels) {
        if (channel.size() != sampleCount) {
            return {};
        }
        double mean = 0.0;
        for (float sample : channel) {
            if (!std::isfinite(sample)) {
                return {};
            }
            mean += sample;
        }
        mean /= sampleCount;

        QVector<std::complex<double>> spectrum(fftSize);
        double energy = 0.0;
        for (int sample = 0; sample < sampleCount; ++sample) {
            const double centered = static_cast<double>(channel.at(sample)) - mean;
            const double hann = 0.5 - 0.5
                * std::cos(2.0 * std::numbers::pi * sample / (sampleCount - 1));
            const double windowed = centered * hann;
            spectrum[sample] = {windowed, 0.0};
            energy += windowed * windowed;
        }
        energy /= sampleCount;
        energies.append(energy);
        totalEnergy += energy;
        if (!Radix2Fft::transform(&spectrum, false)) {
            return {};
        }
        spectra.append(std::move(spectrum));
    }
    totalEnergy /= window.channels.size();
    if (!std::isfinite(totalEnergy) || totalEnergy < 1.0e-10) {
        m_hasSmoothedDirection = false;
        return {};
    }

    QVector<Measurement> measurements;
    measurements.reserve(m_pairs.size());
    QVector<std::complex<double>> crossSpectrum(fftSize);
    for (const Pair &pair : m_pairs) {
        for (int bin = 0; bin < fftSize; ++bin) {
            const std::complex<double> cross = spectra.at(pair.first).at(bin)
                * std::conj(spectra.at(pair.second).at(bin));
            const double magnitude = std::abs(cross);
            crossSpectrum[bin] = magnitude > 1.0e-12
                ? cross / magnitude
                : std::complex<double>(0.0, 0.0);
        }
        QVector<std::complex<double>> correlation = crossSpectrum;
        if (!Radix2Fft::transform(&correlation, true)) {
            return {};
        }

        const int maxLag = std::min(fftSize / 2 - 2,
                                    static_cast<int>(std::ceil(
                                        pair.distanceMeters
                                        / m_configuration.soundSpeedMetersPerSecond
                                        * m_configuration.sampleRate))
                                        + 1);
        int bestLag = 0;
        double bestValue = -std::numeric_limits<double>::infinity();
        for (int lag = -maxLag; lag <= maxLag; ++lag) {
            const double value = correlationAtLag(correlation, lag);
            if (value > bestValue) {
                bestValue = value;
                bestLag = lag;
            }
        }
        double secondValue = -std::numeric_limits<double>::infinity();
        for (int lag = -maxLag; lag <= maxLag; ++lag) {
            if (std::abs(lag - bestLag) <= 1) {
                continue;
            }
            secondValue = std::max(secondValue, correlationAtLag(correlation, lag));
        }
        if (!std::isfinite(bestValue) || bestValue <= 1.0e-12) {
            continue;
        }
        if (!std::isfinite(secondValue)) {
            secondValue = 0.0;
        }
        const double quality = std::clamp((bestValue - secondValue)
                                              / (std::abs(bestValue) + 1.0e-12),
                                          0.0,
                                          1.0);
        if (quality < 0.03) {
            continue;
        }
        const double left = correlationAtLag(correlation, bestLag - 1);
        const double center = correlationAtLag(correlation, bestLag);
        const double right = correlationAtLag(correlation, bestLag + 1);
        const double denominator = left - 2.0 * center + right;
        const double peakOffset = std::abs(denominator) > 1.0e-12
            ? std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5)
            : 0.0;
        const double lagSamples = bestLag + peakOffset;
        const double energyWeight = std::sqrt(std::max(0.0,
            std::min(energies.at(pair.first), energies.at(pair.second)) / totalEnergy));
        const double weight = quality * std::clamp(energyWeight, 0.0, 1.0);
        if (weight < 0.02) {
            continue;
        }
        measurements.append({pair.dxMeters,
                             pair.dyMeters,
                             lagSamples * m_configuration.soundSpeedMetersPerSecond
                                 / m_configuration.sampleRate,
                             weight,
                             weight,
                             0.0});
    }

    if (measurements.size() < 8) {
        m_hasSmoothedDirection = false;
        return {};
    }
    double ux = 0.0;
    double uy = 0.0;
    if (!solveDirection(&measurements, &ux, &uy)) {
        m_hasSmoothedDirection = false;
        return {};
    }

    double planarSquared = ux * ux + uy * uy;
    if (!std::isfinite(planarSquared) || planarSquared > 1.05) {
        m_hasSmoothedDirection = false;
        return {};
    }
    if (planarSquared > 1.0) {
        const double scale = 1.0 / std::sqrt(planarSquared);
        ux *= scale;
        uy *= scale;
        planarSquared = 1.0;
    }
    QVector3D current(static_cast<float>(ux),
                      static_cast<float>(uy),
                      static_cast<float>(std::sqrt(std::max(0.0, 1.0 - planarSquared))));
    current.normalize();
    if (m_hasSmoothedDirection) {
        const float factor = static_cast<float>(m_configuration.smoothingFactor);
        current = (m_smoothedDirection * (1.0f - factor) + current * factor).normalized();
    }
    m_smoothedDirection = current;
    m_hasSmoothedDirection = true;

    int inlierCount = 0;
    double qualitySum = 0.0;
    double squaredResidualSamples = 0.0;
    for (const Measurement &measurement : measurements) {
        const double residualSamples = measurement.residualMeters
            * m_configuration.sampleRate / m_configuration.soundSpeedMetersPerSecond;
        if (std::abs(residualSamples) <= 2.0) {
            ++inlierCount;
            qualitySum += measurement.baseWeight;
            squaredResidualSamples += residualSamples * residualSamples;
        }
    }
    if (inlierCount < 4) {
        m_hasSmoothedDirection = false;
        return {};
    }
    const double rmsResidual = std::sqrt(squaredResidualSamples / inlierCount);
    const double validRatio = static_cast<double>(inlierCount) / m_pairs.size();
    const double meanQuality = qualitySum / inlierCount;
    const double inlierRatio = static_cast<double>(inlierCount) / measurements.size();
    const double residualScore = std::exp(-rmsResidual / 2.0);
    const double energyScore = std::clamp(std::sqrt(totalEnergy) * 4.0, 0.0, 1.0);
    const double confidence = std::clamp(0.25 * validRatio
                                             + 0.30 * meanQuality
                                             + 0.20 * inlierRatio
                                             + 0.20 * residualScore
                                             + 0.05 * energyScore,
                                         0.0,
                                         1.0);

    SoundSourceDirection result = anglesFromDirection(current);
    result.confidence = confidence;
    result.residualSamples = rmsResidual;
    result.validPairCount = inlierCount;
    result.centerSample = window.centerSample;
    return {result};
}

void GccPhatLocator::rebuildPairs()
{
    m_pairs.clear();
    const auto appendPair = [this](int first, int second) {
        const MicrophonePosition &a = m_configuration.microphones.at(first);
        const MicrophonePosition &b = m_configuration.microphones.at(second);
        const double dx = a.xMeters - b.xMeters;
        const double dy = a.yMeters - b.yMeters;
        const double dz = a.zMeters - b.zMeters;
        m_pairs.append({first, second, dx, dy, std::sqrt(dx * dx + dy * dy + dz * dz)});
    };
    for (int row = 0; row < 6; ++row) {
        for (int offset : {1, 4}) {
            for (int column = 0; column + offset < 8; ++column) {
                appendPair(row * 8 + column + offset, row * 8 + column);
            }
        }
    }
    for (int column = 0; column < 8; ++column) {
        for (int offset : {1, 4}) {
            for (int row = 0; row + offset < 6; ++row) {
                appendPair((row + offset) * 8 + column, row * 8 + column);
            }
        }
    }
}

bool GccPhatLocator::solveDirection(QVector<Measurement> *measurements,
                                    double *ux,
                                    double *uy) const
{
    const auto medianAbsoluteResidual = [](const QVector<Measurement> &values) {
        QVector<double> residuals;
        residuals.reserve(values.size());
        for (const Measurement &measurement : values) {
            residuals.append(std::abs(measurement.residualMeters));
        }
        const int middle = residuals.size() / 2;
        std::nth_element(residuals.begin(), residuals.begin() + middle, residuals.end());
        return residuals.at(middle);
    };
    for (int iteration = 0; iteration < 4; ++iteration) {
        double xx = 0.0;
        double xy = 0.0;
        double yy = 0.0;
        double xb = 0.0;
        double yb = 0.0;
        for (const Measurement &measurement : *measurements) {
            xx += measurement.weight * measurement.ax * measurement.ax;
            xy += measurement.weight * measurement.ax * measurement.ay;
            yy += measurement.weight * measurement.ay * measurement.ay;
            xb += measurement.weight * measurement.ax * measurement.targetMeters;
            yb += measurement.weight * measurement.ay * measurement.targetMeters;
        }
        const double determinant = xx * yy - xy * xy;
        if (std::abs(determinant) < 1.0e-12) {
            return false;
        }
        *ux = (xb * yy - yb * xy) / determinant;
        *uy = (yb * xx - xb * xy) / determinant;
        for (Measurement &measurement : *measurements) {
            measurement.residualMeters = measurement.ax * *ux + measurement.ay * *uy
                - measurement.targetMeters;
        }
        if (iteration == 3) {
            break;
        }
        const double scale = std::max(0.0005,
                                      medianAbsoluteResidual(*measurements) * 1.5);
        for (Measurement &measurement : *measurements) {
            const double absoluteResidual = std::abs(measurement.residualMeters);
            const double huber = absoluteResidual <= scale ? 1.0 : scale / absoluteResidual;
            measurement.weight = measurement.baseWeight * huber;
        }
    }
    return std::isfinite(*ux) && std::isfinite(*uy);
}

} // namespace MicArray
