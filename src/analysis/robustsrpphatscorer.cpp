#include "analysis/robustsrpphatscorer.h"

#include "analysis/arraygeometry.h"
#include "analysis/neuralsrppeakrefiner.h"
#include "analysis/radix2fft.h"

#include <QtConcurrent>
#include <QScopeGuard>
#include <QThread>
#include <QThreadPool>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <tuple>

namespace MicArray {

namespace {

constexpr int MinimumValidChannelCount = 32;
constexpr int RealtimeGroupLimit = 32;
constexpr double MinimumFrequencyHz = 100.0;
constexpr double PassbandLowHz = 250.0;
constexpr double PassbandHighHz = 8000.0;
constexpr double MaximumFrequencyHz = 8150.0;

struct BaselineKey {
    qint64 x = 0;
    qint64 y = 0;
    qint64 z = 0;

    bool operator<(const BaselineKey &other) const
    {
        return std::tie(x, y, z)
            < std::tie(other.x, other.y, other.z);
    }
};

qint64 quantizedMeters(double value)
{
    return qRound64(value * 1.0e9);
}

double smoothStep(double lower, double upper, double value)
{
    const double normalized = std::clamp(
        (value - lower) / (upper - lower), 0.0, 1.0);
    return normalized * normalized * (3.0 - 2.0 * normalized);
}

double bandTaper(double frequencyHz)
{
    if (frequencyHz < MinimumFrequencyHz
        || frequencyHz > MaximumFrequencyHz) {
        return 0.0;
    }
    if (frequencyHz < PassbandLowHz) {
        return smoothStep(MinimumFrequencyHz,
                          PassbandLowHz,
                          frequencyHz);
    }
    if (frequencyHz > PassbandHighHz) {
        return 1.0 - smoothStep(PassbandHighHz,
                                MaximumFrequencyHz,
                                frequencyHz);
    }
    return 1.0;
}

int wrappedIndex(int index, int size)
{
    const int wrapped = index % size;
    return wrapped >= 0 ? wrapped : wrapped + size;
}

double cubic(double y0,
             double y1,
             double y2,
             double y3,
             double fraction)
{
    const double a0 = -0.5 * y0 + 1.5 * y1
        - 1.5 * y2 + 0.5 * y3;
    const double a1 = y0 - 2.5 * y1
        + 2.0 * y2 - 0.5 * y3;
    const double a2 = -0.5 * y0 + 0.5 * y2;
    return ((a0 * fraction + a1) * fraction + a2)
        * fraction + y1;
}

QThreadPool *spatialThreadPool()
{
    static QThreadPool pool;
    static const bool initialized = [] {
        pool.setObjectName(
            QStringLiteral("RobustSrpPhatSpatialResponsePool"));
        pool.setMaxThreadCount(
            std::max(1, QThread::idealThreadCount() - 1));
        pool.setExpiryTimeout(30'000);
        return true;
    }();
    Q_UNUSED(initialized);
    return &pool;
}

const std::array<std::array<double, 8>, 1025> &
normalizedSincWeights()
{
    static const auto table = [] {
        std::array<std::array<double, 8>, 1025> result {};
        for (int fractionIndex = 0;
             fractionIndex <= 1024;
             ++fractionIndex) {
            const double fraction =
                fractionIndex / 1024.0;
            double sum = 0.0;
            for (int tap = 0; tap < 8; ++tap) {
                const double distance =
                    fraction + 3.0 - tap;
                const double sinc =
                    std::abs(distance) < 1.0e-12
                    ? 1.0
                    : std::sin(
                          std::numbers::pi * distance)
                        / (std::numbers::pi * distance);
                const double window =
                    0.5 + 0.5 * std::cos(
                        std::numbers::pi
                        * distance / 4.0);
                result[fractionIndex][tap] =
                    sinc * window;
                sum += result[fractionIndex][tap];
            }
            if (std::abs(sum) > 1.0e-12) {
                for (double &weight
                     : result[fractionIndex]) {
                    weight /= sum;
                }
            }
        }
        return result;
    }();
    return table;
}

} // namespace

void sharpenNeuralSrpHeatmap(
    const SpatialResponseGrid &grid,
    const RobustSpatialEstimate &estimate,
    double strength,
    QVector<float> *normalizedValues)
{
    if (normalizedValues == nullptr
        || normalizedValues->size()
            != grid.directions.size()
        || !estimate.hasDirection
        || !estimate.neuralPeakApplied
        || !std::isfinite(strength)
        || strength <= 0.0
        || !std::isfinite(
            estimate
                .azimuthStandardDeviationDegrees)
        || !std::isfinite(
            estimate
                .elevationStandardDeviationDegrees)
        || estimate
                .azimuthStandardDeviationDegrees
            <= 0.0
        || estimate
                .elevationStandardDeviationDegrees
            <= 0.0) {
        return;
    }

    const double azimuthSigma =
        estimate.azimuthStandardDeviationDegrees;
    const double elevationSigma =
        estimate.elevationStandardDeviationDegrees;
    const double correlation = std::clamp(
        estimate.azimuthElevationCorrelation,
        -0.95, 0.95);
    const double inverseCorrelationScale =
        1.0 / (1.0 - correlation * correlation);
    const double exponent =
        1.0 + 2.0
            * std::clamp(strength, 0.0, 1.0)
            * std::clamp(
                estimate.neuralPeakConfidence,
                0.0, 1.0);
    double peakAmplitude = 0.0;
    for (float value : std::as_const(
             *normalizedValues)) {
        if (!std::isfinite(value)) {
            return;
        }
        peakAmplitude = std::max(
            peakAmplitude,
            std::clamp(
                static_cast<double>(value),
                0.0, 1.0));
    }
    if (peakAmplitude <= 0.0) {
        return;
    }
    constexpr double EdgeBlendStartSquared = 4.0;
    constexpr double MaximumMahalanobisSquared = 9.0;
    for (int index = 0;
         index < normalizedValues->size();
         ++index) {
        const ScanDirection &candidate =
            grid.directions.at(index);
        const double normalizedAzimuth =
            (candidate.azimuthDegrees
             - estimate.direction.azimuthDegrees)
            / azimuthSigma;
        const double normalizedElevation =
            (candidate.elevationDegrees
             - estimate.direction.elevationDegrees)
            / elevationSigma;
        const double mahalanobisSquared = (
            normalizedAzimuth * normalizedAzimuth
            - 2.0 * correlation
                * normalizedAzimuth
                * normalizedElevation
            + normalizedElevation
                * normalizedElevation)
            * inverseCorrelationScale;
        if (!std::isfinite(mahalanobisSquared)
            || mahalanobisSquared
                > MaximumMahalanobisSquared) {
            continue;
        }
        const double value = std::clamp(
            static_cast<double>(
                normalizedValues->at(index)),
            0.0, 1.0);
        const double shoulder =
            std::pow(value, exponent)
            * std::exp(
                -0.5 * (exponent - 1.0)
                * mahalanobisSquared);
        const double kernel =
            peakAmplitude
            * std::exp(
                -0.5 * mahalanobisSquared);
        const double enhanced = std::clamp(
            std::max(shoulder, kernel), 0.0, 1.0);
        const double edgeBlend = smoothStep(
            EdgeBlendStartSquared,
            MaximumMahalanobisSquared,
            mahalanobisSquared);
        (*normalizedValues)[index] =
            static_cast<float>(
                std::clamp(
                    enhanced * (1.0 - edgeBlend)
                        + value * edgeBlend,
                    0.0, 1.0));
    }
}

bool RobustSrpPhatScorer::configure(
    const LocatorConfiguration &configuration,
    QString *errorMessage)
{
    reset();
    if (configuration.microphones.size() < 2
        || configuration.sampleRate <= 0
        || configuration.windowSamples <= 0
        || !std::isfinite(configuration.soundSpeedMetersPerSecond)
        || configuration.soundSpeedMetersPerSecond <= 0.0
        || !std::isfinite(
            configuration.neuralPeakEnhancementStrength)
        || configuration.neuralPeakEnhancementStrength < 0.0
        || configuration.neuralPeakEnhancementStrength > 1.0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Invalid SRP-PHAT configuration");
        }
        return false;
    }

    m_configuration = configuration;
    std::map<BaselineKey, int> groupByDisplacement;
    for (int first = 0; first < configuration.microphones.size(); ++first) {
        for (int second = first + 1;
             second < configuration.microphones.size();
             ++second) {
            int canonicalFirst = first;
            int canonicalSecond = second;
            const MicrophonePosition &firstPosition =
                configuration.microphones.at(first);
            const MicrophonePosition &secondPosition =
                configuration.microphones.at(second);
            double dx = secondPosition.xMeters - firstPosition.xMeters;
            double dy = secondPosition.yMeters - firstPosition.yMeters;
            double dz = secondPosition.zMeters - firstPosition.zMeters;
            const bool reverse = dy < 0.0
                || (qFuzzyIsNull(dy) && dx < 0.0)
                || (qFuzzyIsNull(dy) && qFuzzyIsNull(dx) && dz < 0.0);
            if (reverse) {
                std::swap(canonicalFirst, canonicalSecond);
                dx = -dx;
                dy = -dy;
                dz = -dz;
            }

            const BaselineKey key {
                quantizedMeters(dx),
                quantizedMeters(dy),
                quantizedMeters(dz),
            };
            auto [iterator, inserted] =
                groupByDisplacement.try_emplace(
                    key, m_baselineGroups.size());
            if (inserted) {
                BaselineGroup group;
                group.displacement = QVector3D(
                    static_cast<float>(dx),
                    static_cast<float>(dy),
                    static_cast<float>(dz));
                m_baselineGroups.append(group);
            }
            m_baselineGroups[iterator->second].pairs.append(
                {canonicalFirst, canonicalSecond});
        }
    }

    if (m_baselineGroups.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Array has no nonzero baselines");
        }
        reset();
        return false;
    }

    double minimumLength = std::numeric_limits<double>::max();
    double maximumLength = 0.0;
    for (const BaselineGroup &group : std::as_const(m_baselineGroups)) {
        const double length = group.displacement.length();
        minimumLength = std::min(minimumLength, length);
        maximumLength = std::max(maximumLength, length);
    }

    std::array<QVector<int>, 36> buckets;
    for (int groupIndex = 0;
         groupIndex < m_baselineGroups.size();
         ++groupIndex) {
        const BaselineGroup &group = m_baselineGroups.at(groupIndex);
        double angle = std::atan2(
            static_cast<double>(group.displacement.y()),
            static_cast<double>(group.displacement.x()));
        if (angle < 0.0) {
            angle += std::numbers::pi;
        }
        const int angleBucket = std::clamp(
            static_cast<int>(angle / std::numbers::pi * 12.0),
            0, 11);
        const double normalizedLength = maximumLength > minimumLength
            ? (group.displacement.length() - minimumLength)
                / (maximumLength - minimumLength)
            : 0.0;
        const int lengthBucket = std::clamp(
            static_cast<int>(normalizedLength * 3.0), 0, 2);
        buckets.at(lengthBucket * 12 + angleBucket).append(groupIndex);
    }

    for (QVector<int> &bucket : buckets) {
        std::sort(bucket.begin(), bucket.end(),
                  [this](int first, int second) {
            const BaselineGroup &firstGroup = m_baselineGroups.at(first);
            const BaselineGroup &secondGroup = m_baselineGroups.at(second);
            if (firstGroup.pairs.size() != secondGroup.pairs.size()) {
                return firstGroup.pairs.size() > secondGroup.pairs.size();
            }
            const QVector3D &firstDisplacement = firstGroup.displacement;
            const QVector3D &secondDisplacement = secondGroup.displacement;
            return std::tuple(firstDisplacement.y(),
                              firstDisplacement.x(),
                              firstDisplacement.z())
                < std::tuple(secondDisplacement.y(),
                             secondDisplacement.x(),
                             secondDisplacement.z());
        });
    }

    for (int depth = 0;
         m_realtimeGroupIndices.size() < RealtimeGroupLimit;
         ++depth) {
        bool selectedAny = false;
        for (const QVector<int> &bucket : buckets) {
            if (depth < bucket.size()) {
                m_realtimeGroupIndices.append(bucket.at(depth));
                selectedAny = true;
                if (m_realtimeGroupIndices.size()
                    == RealtimeGroupLimit) {
                    break;
                }
            }
        }
        if (!selectedAny) {
            break;
        }
    }

    m_channelQualities.fill(0.0, configuration.microphones.size());
    m_configured = true;
    return true;
}

void RobustSrpPhatScorer::prepareGrid(const SpatialResponseGrid &grid)
{
    m_preparedGrid = {};
    m_gridSteering.clear();
    m_validGridDirections.clear();
    if (!m_configured
        || grid.columns <= 0 || grid.rows <= 0
        || grid.directions.size() != grid.columns * grid.rows) {
        return;
    }

    const int fftSize = m_configuration.windowSamples * 2;
    m_gridSteering.resize(m_baselineGroups.size());
    m_validGridDirections.fill(
        0, grid.directions.size());
    QVector<quint8> preparedGroups(
        m_baselineGroups.size(), 0);
    if (m_configuration.spatialResolutionMode
        == SpatialResolutionMode::HighPrecision) {
        preparedGroups.fill(1);
    } else {
        for (int groupIndex : m_realtimeGroupIndices) {
            preparedGroups[groupIndex] = 1;
        }
    }
    for (int groupIndex = 0;
         groupIndex < m_gridSteering.size();
         ++groupIndex) {
        GridSteering &steering =
            m_gridSteering[groupIndex];
        if (preparedGroups.at(groupIndex) != 0) {
            steering.lowerIndices.resize(
                grid.directions.size());
            steering.fractions.resize(
                grid.directions.size());
        }
    }

    for (int directionIndex = 0;
         directionIndex < grid.directions.size();
         ++directionIndex) {
        const ScanDirection &scan = grid.directions.at(directionIndex);
        if (!std::isfinite(scan.azimuthDegrees)
            || !std::isfinite(scan.elevationDegrees)) {
            m_gridSteering.clear();
            return;
        }
        const QVector3D direction = directionFromAngles(
            scan.azimuthDegrees, scan.elevationDegrees);
        if (direction.z() < 0.0f) {
            continue;
        }
        m_validGridDirections[directionIndex] = 1;
        for (int groupIndex = 0;
             groupIndex < m_baselineGroups.size();
             ++groupIndex) {
            if (preparedGroups.at(groupIndex) == 0) {
                continue;
            }
            const double lag = QVector3D::dotProduct(
                m_baselineGroups.at(groupIndex).displacement,
                direction)
                * m_configuration.sampleRate
                / m_configuration.soundSpeedMetersPerSecond;
            const int lower = static_cast<int>(std::floor(lag));
            GridSteering &steering = m_gridSteering[groupIndex];
            steering.lowerIndices[directionIndex] =
                static_cast<quint16>(wrappedIndex(lower, fftSize));
            steering.fractions[directionIndex] =
                static_cast<float>(lag - lower);
        }
    }
    m_preparedGrid = grid;
}

bool RobustSrpPhatScorer::prepareWindow(
    const MultiChannelAudioWindow &window)
{
    m_windowPrepared = false;
    m_hasContinuousEstimate = false;
    m_continuousEstimate = {};
    m_validChannelCount = 0;
    bool preparedSuccessfully = false;
    const auto clearFailedWindow = qScopeGuard(
        [this, &preparedSuccessfully] {
        if (!preparedSuccessfully) {
            clearTemporalCorrelations();
        }
    });
    if (!m_configured
        || window.sampleRate != m_configuration.sampleRate
        || window.channels.size() != m_configuration.microphones.size()) {
        return false;
    }

    m_validGroupIndices.clear();
    m_channelUnitSpectra.clear();
    const bool blendPrevious =
        m_hasPreviousCorrelations
        && window.centerSample > m_centerSample
        && window.centerSample - m_centerSample
            <= m_configuration.sampleRate;
    for (BaselineGroup &group : m_baselineGroups) {
        group.canBlendPreviousCorrelation =
            blendPrevious && group.valid
            && !group.correlation.isEmpty();
        if (!group.canBlendPreviousCorrelation) {
            group.correlation.clear();
        }
        group.reliableBinFraction = 0.0;
        group.valid = false;
    }

    QVector<double> energies;
    energies.reserve(window.channels.size());
    for (const QVector<float> &channel : window.channels) {
        if (channel.size() != m_configuration.windowSamples) {
            return false;
        }
        double mean = 0.0;
        for (float value : channel) {
            if (!std::isfinite(value)) {
                return false;
            }
            mean += value;
        }
        mean /= channel.size();

        double energy = 0.0;
        for (int sample = 0; sample < channel.size(); ++sample) {
            const float value = channel.at(sample);
            const double windowValue = channel.size() > 1
                ? 0.5 - 0.5 * std::cos(
                    2.0 * std::numbers::pi * sample
                    / static_cast<double>(channel.size() - 1))
                : 1.0;
            const double weighted = (value - mean) * windowValue;
            energy += weighted * weighted;
        }
        energies.append(energy
                        / static_cast<double>(channel.size()));
    }

    QVector<double> sortedEnergies = energies;
    std::nth_element(
        sortedEnergies.begin(),
        sortedEnergies.begin() + sortedEnergies.size() / 2,
        sortedEnergies.end());
    const double medianEnergy =
        sortedEnergies.at(sortedEnergies.size() / 2);
    if (!std::isfinite(medianEnergy) || medianEnergy <= 1.0e-12) {
        return false;
    }

    for (int channel = 0; channel < energies.size(); ++channel) {
        const double energy = energies.at(channel);
        if (!std::isfinite(energy) || energy <= 1.0e-12) {
            m_channelQualities[channel] = 0.0;
            continue;
        }
        const double relativeDb =
            10.0 * std::log10(energy / medianEnergy);
        if (relativeDb < -24.0 || relativeDb > 18.0) {
            m_channelQualities[channel] = 0.0;
            continue;
        }
        const double normalizedDeviation =
            std::min(1.0, std::abs(relativeDb) / 24.0);
        m_channelQualities[channel] =
            1.0 - 0.75 * normalizedDeviation;
        ++m_validChannelCount;
    }

    m_windowPrepared =
        m_validChannelCount >= MinimumValidChannelCount;
    if (!m_windowPrepared) {
        return false;
    }
    if (!buildChannelSpectra(window)) {
        m_windowPrepared = false;
        return false;
    }
    if (!blendPrevious
        || m_smoothedChannelUnitSpectra.size()
            != m_channelUnitSpectra.size()) {
        m_smoothedChannelUnitSpectra =
            m_channelUnitSpectra;
    } else {
        const double factor =
            m_configuration.smoothingFactor;
        for (int channel = 0;
             channel < m_channelUnitSpectra.size();
             ++channel) {
            const QVector<std::complex<double>> &current =
                m_channelUnitSpectra.at(channel);
            QVector<std::complex<double>> &smoothed =
                m_smoothedChannelUnitSpectra[channel];
            if (current.isEmpty()) {
                smoothed.clear();
                continue;
            }
            if (smoothed.size() != current.size()) {
                smoothed = current;
                continue;
            }
            for (int bin = 0; bin < current.size(); ++bin) {
                smoothed[bin] =
                    smoothed.at(bin) * (1.0 - factor)
                    + current.at(bin) * factor;
            }
        }
    }
    if (!buildGroupedCorrelations()) {
        m_windowPrepared = false;
        return false;
    }
    m_centerSample = window.centerSample;
    m_hasPreviousCorrelations = true;
    preparedSuccessfully = true;
    return true;
}

RobustSpatialEstimate RobustSrpPhatScorer::estimate(
    const SpatialResponseGrid *grid)
{
    RobustSpatialEstimate result;
    result.validChannelCount = m_validChannelCount;
    if (!m_windowPrepared) {
        return result;
    }

    QVector<int> activeGroups;
    const QVector<int> &requestedGroups =
        m_configuration.spatialResolutionMode
            == SpatialResolutionMode::HighPrecision
        ? m_validGroupIndices : m_realtimeGroupIndices;
    activeGroups.reserve(requestedGroups.size());
    for (int groupIndex : requestedGroups) {
        if (m_baselineGroups.at(groupIndex).valid) {
            activeGroups.append(groupIndex);
        }
    }
    result.validBaselineGroupCount = activeGroups.size();
    if (activeGroups.size() < 12) {
        return result;
    }
    if (m_hasContinuousEstimate) {
        result = m_continuousEstimate;
    } else {
        refineContinuousPeak(activeGroups, &result);
        applyNeuralPeakRefinement(&result);
        m_continuousEstimate = result;
        m_hasContinuousEstimate = true;
    }

    const SpatialResponseGrid *requestedGrid = grid;
    if (requestedGrid == nullptr) {
        return result;
    }
    if (!preparedGridMatches(*requestedGrid)) {
        prepareGrid(*requestedGrid);
    }
    if (!preparedGridMatches(*requestedGrid)
        || m_gridSteering.size() != m_baselineGroups.size()
        || m_validGridDirections.size()
            != requestedGrid->directions.size()) {
        return result;
    }

    result.gridValues.fill(0.0f, requestedGrid->directions.size());
    float *output = result.gridValues.data();
    const auto scanRange =
        [this, output, &activeGroups](const QPair<int, int> &range) {
        const int fftSize =
            m_configuration.windowSamples * 2;
        for (int groupIndex : activeGroups) {
            const BaselineGroup &group =
                m_baselineGroups.at(groupIndex);
            const GridSteering &steering =
                m_gridSteering.at(groupIndex);
            const auto *correlation =
                group.correlation.constData();
            const auto *lowerIndices =
                steering.lowerIndices.constData();
            const auto *fractions =
                steering.fractions.constData();
            for (int directionIndex = range.first;
                 directionIndex < range.second;
                 ++directionIndex) {
                if (m_validGridDirections.at(
                        directionIndex) == 0) {
                    continue;
                }
                const int lower =
                    lowerIndices[directionIndex];
                output[directionIndex] +=
                    static_cast<float>(cubic(
                        correlation[wrappedIndex(
                            lower - 1, fftSize)].real(),
                        correlation[lower].real(),
                        correlation[wrappedIndex(
                            lower + 1, fftSize)].real(),
                        correlation[wrappedIndex(
                            lower + 2, fftSize)].real(),
                        fractions[directionIndex]));
            }
        }
        const float inverseGroupCount =
            1.0f / activeGroups.size();
        for (int directionIndex = range.first;
             directionIndex < range.second;
             ++directionIndex) {
            output[directionIndex] *=
                inverseGroupCount;
        }
    };
    if (requestedGrid->directions.size() < 4096
        || requestedGrid->rows < 2) {
        scanRange({0, requestedGrid->directions.size()});
    } else {
        QThreadPool *pool = spatialThreadPool();
        const int chunkCount = std::min(
            pool->maxThreadCount(), requestedGrid->rows);
        const int rowsPerChunk =
            (requestedGrid->rows + chunkCount - 1) / chunkCount;
        QVector<QPair<int, int>> ranges;
        for (int firstRow = 0;
             firstRow < requestedGrid->rows;
             firstRow += rowsPerChunk) {
            ranges.append({
                firstRow * requestedGrid->columns,
                std::min(requestedGrid->rows,
                         firstRow + rowsPerChunk)
                    * requestedGrid->columns,
            });
        }
        QtConcurrent::blockingMap(pool, ranges, scanRange);
    }

    if (result.hasDirection) {
        double azimuthSpacing = 0.0;
        double elevationSpacing = 0.0;
        if (requestedGrid->columns > 1) {
            azimuthSpacing = std::abs(
                requestedGrid->directions.at(1).azimuthDegrees
                - requestedGrid->directions.at(0).azimuthDegrees);
        }
        if (requestedGrid->rows > 1) {
            elevationSpacing = std::abs(
                requestedGrid->directions.at(
                    requestedGrid->columns).elevationDegrees
                - requestedGrid->directions.at(0).elevationDegrees);
        }
        const double patchRadius = std::max(
            6.0,
            3.0 * std::max(
                azimuthSpacing, elevationSpacing));
        for (int directionIndex = 0;
             directionIndex < requestedGrid->directions.size();
             ++directionIndex) {
            const ScanDirection &candidate =
                requestedGrid->directions.at(directionIndex);
            if (m_validGridDirections.at(
                    directionIndex) == 0) {
                continue;
            }
            const double azimuthDistance =
                candidate.azimuthDegrees
                - result.direction.azimuthDegrees;
            const double elevationDistance =
                candidate.elevationDegrees
                - result.direction.elevationDegrees;
            if (std::hypot(
                    azimuthDistance,
                    elevationDistance)
                <= patchRadius) {
                result.gridValues[directionIndex] =
                    static_cast<float>(scoreDirectionSinc(
                        candidate, m_validGroupIndices));
            }
        }
    }

    QVector<float> ordered = result.gridValues;
    const qsizetype middleIndex =
        (ordered.size() - 1) / 2;
    const qsizetype upperIndex = std::clamp<qsizetype>(
        std::max(
            middleIndex + 1,
            static_cast<qsizetype>(
                std::floor(
                    (ordered.size() - 1) * 0.995))),
        0, ordered.size() - 1);
    std::nth_element(ordered.begin(),
                     ordered.begin() + middleIndex,
                     ordered.end());
    const float median = ordered.at(middleIndex);
    std::nth_element(ordered.begin(),
                     ordered.begin() + upperIndex,
                     ordered.end());
    const float upper = ordered.at(upperIndex);
    const float maximum = *std::max_element(
        ordered.cbegin(), ordered.cend());
    const double range = static_cast<double>(upper) - median;
    if (!std::isfinite(median) || !std::isfinite(upper)
        || range <= 1.0e-9) {
        result.gridValues.clear();
        return result;
    }

    double normalizedSum = 0.0;
    for (float &value : result.gridValues) {
        if (value <= upper || maximum <= upper) {
            value = static_cast<float>(
                0.98 * std::pow(
                    std::clamp((value - median) / range,
                               0.0, 1.0),
                    0.75));
        } else {
            value = static_cast<float>(
                0.98 + 0.02 * std::clamp(
                    (value - upper)
                        / static_cast<double>(maximum - upper),
                    0.0, 1.0));
        }
        normalizedSum += value;
    }
    if (!result.hasDirection) {
        result.reliability = std::clamp(
            1.0 - normalizedSum / result.gridValues.size(),
            0.0, 1.0);
    }
    sharpenNeuralSrpHeatmap(
        *requestedGrid,
        result,
        m_configuration
            .neuralPeakEnhancementStrength,
        &result.gridValues);
    return result;
}

std::optional<SrpLocalPeakPatch>
RobustSrpPhatScorer::sampleLocalPeakPatch(
    const RobustSpatialEstimate &physicalEstimate) const
{
    constexpr int PatchSize = 9;
    constexpr int PatchCenter = PatchSize / 2;
    if (!m_windowPrepared
        || !physicalEstimate.hasDirection
        || m_validGroupIndices.size() < 12
        || !std::isfinite(
            physicalEstimate
                .azimuthStandardDeviationDegrees)
        || !std::isfinite(
            physicalEstimate
                .elevationStandardDeviationDegrees)
        || physicalEstimate
                .azimuthStandardDeviationDegrees
            <= 0.0
        || physicalEstimate
                .elevationStandardDeviationDegrees
            <= 0.0) {
        return std::nullopt;
    }

    SrpLocalPeakPatch patch;
    patch.azimuthStepDegrees = std::clamp(
        physicalEstimate
                .azimuthStandardDeviationDegrees
            / 2.5,
        0.25, 1.5);
    patch.elevationStepDegrees = std::clamp(
        physicalEstimate
                .elevationStandardDeviationDegrees
            / 2.5,
        0.25, 1.5);
    QVector<double> rawValues;
    rawValues.reserve(PatchSize * PatchSize);
    for (int row = 0; row < PatchSize; ++row) {
        for (int column = 0;
             column < PatchSize;
             ++column) {
            const ScanDirection candidate {
                std::clamp(
                    physicalEstimate
                            .direction.azimuthDegrees
                        + (column - PatchCenter)
                            * patch.azimuthStepDegrees,
                    -90.0, 90.0),
                std::clamp(
                    physicalEstimate
                            .direction.elevationDegrees
                        + (PatchCenter - row)
                            * patch.elevationStepDegrees,
                    -90.0, 90.0),
            };
            const double score = scoreDirectionSinc(
                candidate, m_validGroupIndices);
            if (!std::isfinite(score)) {
                return std::nullopt;
            }
            rawValues.append(score);
        }
    }

    patch.centerPhysicalScore =
        rawValues.at(
            PatchCenter * PatchSize + PatchCenter);
    QVector<double> ordered = rawValues;
    const qsizetype medianIndex =
        ordered.size() / 2;
    const qsizetype p95Index =
        static_cast<qsizetype>(std::floor(
            0.95 * (ordered.size() - 1)));
    std::nth_element(
        ordered.begin(),
        ordered.begin() + medianIndex,
        ordered.end());
    const double median = ordered.at(medianIndex);
    std::nth_element(
        ordered.begin(),
        ordered.begin() + p95Index,
        ordered.end());
    const double p95 = ordered.at(p95Index);
    const double maximum = *std::max_element(
        rawValues.cbegin(), rawValues.cend());
    patch.dynamicRange = p95 - median;
    if (!std::isfinite(median)
        || !std::isfinite(p95)
        || !std::isfinite(maximum)
        || patch.dynamicRange <= 1.0e-9) {
        return std::nullopt;
    }

    patch.normalizedValues.reserve(
        rawValues.size());
    for (double value : std::as_const(rawValues)) {
        patch.normalizedValues.append(
            static_cast<float>(std::clamp(
                (value - median)
                    / patch.dynamicRange,
                0.0, 1.0)));
    }
    const double scoreScale = std::max(
        std::abs(maximum), 1.0e-9);
    patch.diagnostics = {
        static_cast<float>(std::clamp(
            physicalEstimate
                    .direction.azimuthDegrees
                / 90.0,
            -1.0, 1.0)),
        static_cast<float>(std::clamp(
            physicalEstimate
                    .direction.elevationDegrees
                / 90.0,
            -1.0, 1.0)),
        static_cast<float>(std::clamp(
            patch.dynamicRange / scoreScale,
            0.0, 1.0)),
        static_cast<float>(
            patch.azimuthStepDegrees / 1.5),
        static_cast<float>(
            patch.elevationStepDegrees / 1.5),
        static_cast<float>(std::clamp(
            physicalEstimate
                    .azimuthStandardDeviationDegrees
                / 10.0,
            0.0, 1.0)),
        static_cast<float>(std::clamp(
            physicalEstimate
                    .elevationStandardDeviationDegrees
                / 10.0,
            0.0, 1.0)),
        static_cast<float>(std::clamp(
            physicalEstimate
                .azimuthElevationCorrelation,
            -0.99, 0.99)),
        static_cast<float>(std::clamp(
            physicalEstimate.reliability,
            0.0, 1.0)),
        static_cast<float>(std::clamp(
            std::log1p(std::max(
                physicalEstimate
                    .peakToSidelobeRatio,
                0.0))
                / 4.0,
            0.0, 1.0)),
        static_cast<float>(std::clamp(
            physicalEstimate
                .steeredChannelCoherence,
            0.0, 1.0)),
        static_cast<float>(std::clamp(
            physicalEstimate.validChannelCount
                / static_cast<double>(
                    m_configuration
                        .microphones.size()),
            0.0, 1.0)),
    };
    return patch;
}

double RobustSrpPhatScorer::scorePreparedDirectionSinc(
    const ScanDirection &direction) const
{
    if (!m_windowPrepared
        || !std::isfinite(direction.azimuthDegrees)
        || !std::isfinite(direction.elevationDegrees)
        || direction.azimuthDegrees < -90.0
        || direction.azimuthDegrees > 90.0
        || direction.elevationDegrees < -90.0
        || direction.elevationDegrees > 90.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return scoreDirectionSinc(
        direction, m_validGroupIndices);
}

void RobustSrpPhatScorer::applyNeuralPeakRefinement(
    RobustSpatialEstimate *result) const
{
    if (result == nullptr || !result->hasDirection) {
        return;
    }

    result->physicalPeak = {
        result->direction.azimuthDegrees,
        result->direction.elevationDegrees,
    };
    if (!m_configuration.neuralPeakEnhancementEnabled
        || m_configuration.neuralPeakEnhancementStrength
            <= 0.0
        || result->reliability
            < m_configuration.confidenceThreshold) {
        return;
    }

    const std::optional<SrpLocalPeakPatch> patch =
        sampleLocalPeakPatch(*result);
    if (!patch.has_value()) {
        return;
    }
    result->neuralPeakAttempted = true;

    NeuralSrpPeakInput input;
    input.normalizedPatch = patch->normalizedValues;
    input.diagnostics = patch->diagnostics;
    const NeuralSrpPeakOutput output =
        NeuralSrpPeakRefiner {}.infer(input);
    if (!output.valid) {
        return;
    }
    result->neuralPeakConfidence =
        output.confidence;
    constexpr double MinimumConfidence = 0.55;
    if (output.confidence < MinimumConfidence) {
        return;
    }

    const double strength = std::clamp(
        m_configuration.neuralPeakEnhancementStrength
            * output.confidence
            * result->reliability,
        0.0, 1.0);
    double azimuthCorrection =
        output.deltaAzimuthCells
        * patch->azimuthStepDegrees * strength;
    double elevationCorrection =
        output.deltaElevationCells
        * patch->elevationStepDegrees * strength;
    constexpr double MaximumCorrectionDegrees = 1.5;
    const double correctionMagnitude = std::hypot(
        azimuthCorrection, elevationCorrection);
    if (correctionMagnitude > MaximumCorrectionDegrees) {
        return;
    }

    const double azimuthSigma = std::max(
        result->azimuthStandardDeviationDegrees,
        1.0e-6);
    const double elevationSigma = std::max(
        result->elevationStandardDeviationDegrees,
        1.0e-6);
    const double correlation = std::clamp(
        result->azimuthElevationCorrelation,
        -0.95, 0.95);
    const double normalizedAzimuth =
        azimuthCorrection / azimuthSigma;
    const double normalizedElevation =
        elevationCorrection / elevationSigma;
    const double mahalanobisSquared = (
        normalizedAzimuth * normalizedAzimuth
        - 2.0 * correlation
            * normalizedAzimuth
            * normalizedElevation
        + normalizedElevation * normalizedElevation)
        / (1.0 - correlation * correlation);
    constexpr double MaximumMahalanobisDistance = 2.5;
    if (!std::isfinite(mahalanobisSquared)
        || mahalanobisSquared
            > MaximumMahalanobisDistance
                * MaximumMahalanobisDistance) {
        return;
    }

    const ScanDirection corrected {
        result->physicalPeak.azimuthDegrees
            + azimuthCorrection,
        result->physicalPeak.elevationDegrees
            + elevationCorrection,
    };
    if (corrected.azimuthDegrees < -90.0
        || corrected.azimuthDegrees > 90.0
        || corrected.elevationDegrees < -90.0
        || corrected.elevationDegrees > 90.0) {
        return;
    }
    const double acceptedAzimuthCorrection =
        corrected.azimuthDegrees
        - result->physicalPeak.azimuthDegrees;
    const double acceptedElevationCorrection =
        corrected.elevationDegrees
        - result->physicalPeak.elevationDegrees;
    const double correctedScore = scoreDirectionSinc(
        corrected, m_validGroupIndices);
    if (!std::isfinite(correctedScore)
        || correctedScore
            < 0.97 * patch->centerPhysicalScore) {
        return;
    }

    result->direction.azimuthDegrees =
        corrected.azimuthDegrees;
    result->direction.elevationDegrees =
        corrected.elevationDegrees;
    result->azimuthStandardDeviationDegrees *=
        std::lerp(
            1.0,
            output.azimuthSigmaScale,
            strength);
    result->elevationStandardDeviationDegrees *=
        std::lerp(
            1.0,
            output.elevationSigmaScale,
            strength);
    result->azimuthElevationCorrelation =
        std::clamp(
            result->azimuthElevationCorrelation
                + output.correlationResidual
                    * strength,
            -0.99, 0.99);
    result->direction.residualSamples = std::hypot(
        result->azimuthStandardDeviationDegrees,
        result->elevationStandardDeviationDegrees);
    result->neuralPeakApplied = true;
    result->neuralAzimuthCorrectionDegrees =
        acceptedAzimuthCorrection;
    result->neuralElevationCorrectionDegrees =
        acceptedElevationCorrection;
}

void RobustSrpPhatScorer::reset()
{
    m_configuration = {};
    m_baselineGroups.clear();
    m_realtimeGroupIndices.clear();
    m_validGroupIndices.clear();
    m_channelUnitSpectra.clear();
    m_smoothedChannelUnitSpectra.clear();
    m_channelQualities.clear();
    m_preparedGrid = {};
    m_gridSteering.clear();
    m_validGridDirections.clear();
    m_configured = false;
    m_windowPrepared = false;
    m_hasContinuousEstimate = false;
    m_hasPreviousCorrelations = false;
    m_validChannelCount = 0;
    m_centerSample = 0;
    m_continuousEstimate = {};
}

void RobustSrpPhatScorer::clearTemporalCorrelations()
{
    m_hasPreviousCorrelations = false;
    m_smoothedChannelUnitSpectra.clear();
    for (BaselineGroup &group : m_baselineGroups) {
        group.correlation.clear();
        group.reliableBinFraction = 0.0;
        group.canBlendPreviousCorrelation = false;
        group.valid = false;
    }
}

int RobustSrpPhatScorer::baselineGroupCount() const
{
    return m_baselineGroups.size();
}

int RobustSrpPhatScorer::realtimeBaselineGroupCount() const
{
    return m_realtimeGroupIndices.size();
}

bool RobustSrpPhatScorer::preparedGridMatches(
    const SpatialResponseGrid &grid) const
{
    return m_preparedGrid.columns == grid.columns
        && m_preparedGrid.rows == grid.rows
        && m_preparedGrid.directions == grid.directions;
}

bool RobustSrpPhatScorer::buildChannelSpectra(
    const MultiChannelAudioWindow &window)
{
    const int sampleCount = m_configuration.windowSamples;
    const int fftSize = sampleCount * 2;
    m_channelUnitSpectra.resize(window.channels.size());
    for (int channelIndex = 0;
         channelIndex < window.channels.size();
         ++channelIndex) {
        if (m_channelQualities.at(channelIndex) <= 0.0) {
            continue;
        }
        const QVector<float> &channel =
            window.channels.at(channelIndex);
        const double mean = std::accumulate(
            channel.cbegin(), channel.cend(), 0.0)
            / sampleCount;
        QVector<std::complex<double>> spectrum(fftSize);
        for (int sample = 0; sample < sampleCount; ++sample) {
            const double hann = 0.5 - 0.5 * std::cos(
                2.0 * std::numbers::pi * sample
                / static_cast<double>(sampleCount - 1));
            spectrum[sample] =
                {(channel.at(sample) - mean) * hann, 0.0};
        }
        if (!Radix2Fft::transform(&spectrum, false)) {
            return false;
        }
        const double binWidth =
            static_cast<double>(m_configuration.sampleRate)
            / fftSize;
        const int firstRetainedBin = std::max(
            1, static_cast<int>(std::ceil(
                MinimumFrequencyHz / binWidth)));
        const int lastRetainedBin = std::min(
            fftSize / 2 - 1,
            static_cast<int>(std::floor(
                MaximumFrequencyHz / binWidth)));
        double maximumMagnitude = 0.0;
        for (int bin = firstRetainedBin;
             bin <= lastRetainedBin;
             ++bin) {
            maximumMagnitude = std::max(
                maximumMagnitude,
                std::abs(spectrum.at(bin)));
        }
        const double magnitudeThreshold = std::max(
            1.0e-12, maximumMagnitude * 1.0e-3);
        for (std::complex<double> &value : spectrum) {
            const double magnitude = std::abs(value);
            value = magnitude > magnitudeThreshold
                ? value / magnitude
                : std::complex<double>();
        }
        m_channelUnitSpectra[channelIndex] =
            std::move(spectrum);
    }
    return true;
}

bool RobustSrpPhatScorer::buildGroupedCorrelations()
{
    const int fftSize = m_configuration.windowSamples * 2;
    const double binWidth =
        static_cast<double>(m_configuration.sampleRate) / fftSize;
    const int firstBin = std::max(
        1, static_cast<int>(std::ceil(
            MinimumFrequencyHz / binWidth)));
    const int lastBin = std::min(
        fftSize / 2 - 1,
        static_cast<int>(std::floor(
            MaximumFrequencyHz / binWidth)));
    const int retainedBinCount = lastBin - firstBin + 1;
    if (retainedBinCount <= 0) {
        return false;
    }
    double retainedBandWeight = 0.0;
    for (int bin = firstBin; bin <= lastBin; ++bin) {
        retainedBandWeight += bandTaper(bin * binWidth);
    }
    if (retainedBandWeight <= 1.0e-12) {
        return false;
    }

    for (int groupIndex = 0;
         groupIndex < m_baselineGroups.size();
         ++groupIndex) {
        BaselineGroup &group = m_baselineGroups[groupIndex];
        QVector<std::complex<double>> spectrum(fftSize);
        double reliabilitySum = 0.0;
        for (int bin = firstBin; bin <= lastBin; ++bin) {
            std::complex<double> weightedPhaseSum;
            double pairWeightSum = 0.0;
            for (const MicrophonePair &pair : group.pairs) {
                const double pairWeight =
                    m_channelQualities.at(pair.firstChannel)
                    * m_channelQualities.at(pair.secondChannel);
                if (pairWeight <= 0.0) {
                    continue;
                }
                const auto &firstSpectrum =
                    m_channelUnitSpectra.at(pair.firstChannel);
                const auto &secondSpectrum =
                    m_channelUnitSpectra.at(pair.secondChannel);
                if (firstSpectrum.isEmpty()
                    || secondSpectrum.isEmpty()) {
                    continue;
                }
                weightedPhaseSum += pairWeight
                    * secondSpectrum.at(bin)
                    * std::conj(firstSpectrum.at(bin));
                pairWeightSum += pairWeight;
            }
            if (pairWeightSum <= 0.0) {
                continue;
            }
            const std::complex<double> average =
                weightedPhaseSum / pairWeightSum;
            const double coherence = std::abs(average);
            if (!std::isfinite(coherence)
                || coherence <= 1.0e-12) {
                continue;
            }
            const double reliability =
                smoothStep(0.15, 0.65, coherence)
                * bandTaper(bin * binWidth);
            if (reliability <= 0.0) {
                continue;
            }
            spectrum[bin] =
                reliability * average / coherence;
            spectrum[fftSize - bin] =
                std::conj(spectrum.at(bin));
            reliabilitySum += reliability;
        }

        group.reliableBinFraction =
            reliabilitySum / retainedBandWeight;
        if (group.reliableBinFraction < 0.15
            || reliabilitySum <= 1.0e-12
            || !Radix2Fft::transform(&spectrum, true)) {
            group.correlation.clear();
            group.canBlendPreviousCorrelation = false;
            continue;
        }
        const double normalization = fftSize / reliabilitySum;
        for (std::complex<double> &value : spectrum) {
            value *= normalization;
        }
        if (group.canBlendPreviousCorrelation
            && group.correlation.size() == spectrum.size()) {
            const double factor =
                m_configuration.smoothingFactor;
            for (int index = 0; index < spectrum.size(); ++index) {
                spectrum[index] =
                    group.correlation.at(index) * (1.0 - factor)
                    + spectrum.at(index) * factor;
            }
        }
        group.correlation = std::move(spectrum);
        group.canBlendPreviousCorrelation = false;
        group.valid = true;
        m_validGroupIndices.append(groupIndex);
    }
    return m_validGroupIndices.size() >= 12;
}

double RobustSrpPhatScorer::scoreDirectionCubic(
    const ScanDirection &direction,
    const QVector<int> &groupIndices) const
{
    if (groupIndices.isEmpty()
        || !std::isfinite(direction.azimuthDegrees)
        || !std::isfinite(direction.elevationDegrees)) {
        return -std::numeric_limits<double>::infinity();
    }
    const QVector3D unitDirection = directionFromAngles(
        direction.azimuthDegrees,
        direction.elevationDegrees);
    if (unitDirection.z() < 0.0f) {
        return -std::numeric_limits<double>::infinity();
    }

    const int fftSize = m_configuration.windowSamples * 2;
    double response = 0.0;
    for (int groupIndex : groupIndices) {
        const BaselineGroup &group =
            m_baselineGroups.at(groupIndex);
        const double lag = QVector3D::dotProduct(
            group.displacement, unitDirection)
            * m_configuration.sampleRate
            / m_configuration.soundSpeedMetersPerSecond;
        const int lower = static_cast<int>(std::floor(lag));
        const double fraction = lag - lower;
        response += cubic(
            group.correlation.at(
                wrappedIndex(lower - 1, fftSize)).real(),
            group.correlation.at(
                wrappedIndex(lower, fftSize)).real(),
            group.correlation.at(
                wrappedIndex(lower + 1, fftSize)).real(),
            group.correlation.at(
                wrappedIndex(lower + 2, fftSize)).real(),
            fraction);
    }
    return response / groupIndices.size();
}

double RobustSrpPhatScorer::scoreDirectionSinc(
    const ScanDirection &direction,
    const QVector<int> &groupIndices) const
{
    if (groupIndices.isEmpty()
        || !std::isfinite(direction.azimuthDegrees)
        || !std::isfinite(direction.elevationDegrees)) {
        return -std::numeric_limits<double>::infinity();
    }
    const QVector3D unitDirection = directionFromAngles(
        direction.azimuthDegrees,
        direction.elevationDegrees);
    if (unitDirection.z() < 0.0f) {
        return -std::numeric_limits<double>::infinity();
    }

    const int fftSize = m_configuration.windowSamples * 2;
    double response = 0.0;
    for (int groupIndex : groupIndices) {
        const BaselineGroup &group =
            m_baselineGroups.at(groupIndex);
        const double lag = QVector3D::dotProduct(
            group.displacement, unitDirection)
            * m_configuration.sampleRate
            / m_configuration.soundSpeedMetersPerSecond;
        const int lower = static_cast<int>(std::floor(lag));
        const double fraction = lag - lower;
        const int fractionIndex = std::clamp(
            static_cast<int>(
                std::lround(fraction * 1024.0)),
            0, 1024);
        const auto &weights =
            normalizedSincWeights().at(fractionIndex);
        double interpolated = 0.0;
        for (int tap = 0; tap < 8; ++tap) {
            interpolated += weights.at(tap)
                * group.correlation.at(
                    wrappedIndex(
                        lower - 3 + tap,
                        fftSize)).real();
        }
        response += interpolated;
    }
    return response / groupIndices.size();
}

double RobustSrpPhatScorer::steeredChannelCoherence(
    const ScanDirection &direction) const
{
    const QVector3D unitDirection = directionFromAngles(
        direction.azimuthDegrees,
        direction.elevationDegrees);
    if (unitDirection.z() < 0.0f) {
        return 0.0;
    }
    const int fftSize = m_configuration.windowSamples * 2;
    const double binWidth =
        static_cast<double>(m_configuration.sampleRate) / fftSize;
    const int firstBin = std::max(
        1, static_cast<int>(std::ceil(
            MinimumFrequencyHz / binWidth)));
    const int lastBin = std::min(
        fftSize / 2 - 1,
        static_cast<int>(std::floor(
            MaximumFrequencyHz / binWidth)));
    double coherentWeight = 0.0;
    double bandWeightSum = 0.0;
    for (int bin = firstBin; bin <= lastBin; ++bin) {
        const double frequencyWeight =
            bandTaper(bin * binWidth);
        if (frequencyWeight <= 0.0) {
            continue;
        }
        std::complex<double> alignedSum;
        double channelWeightSum = 0.0;
        int contributingChannels = 0;
        for (int channel = 0;
             channel < m_smoothedChannelUnitSpectra.size();
             ++channel) {
            const double channelWeight =
                m_channelQualities.at(channel);
            if (channelWeight <= 0.0
                || m_smoothedChannelUnitSpectra.at(channel).isEmpty()) {
                continue;
            }
            const std::complex<double> unitPhase =
                m_smoothedChannelUnitSpectra.at(channel).at(bin);
            if (std::abs(unitPhase) <= 1.0e-12) {
                continue;
            }
            const MicrophonePosition &microphone =
                m_configuration.microphones.at(channel);
            const double delaySamples = (
                microphone.xMeters * unitDirection.x()
                + microphone.yMeters * unitDirection.y()
                + microphone.zMeters * unitDirection.z())
                * m_configuration.sampleRate
                / m_configuration.soundSpeedMetersPerSecond;
            const double phase =
                2.0 * std::numbers::pi * bin
                * delaySamples / fftSize;
            alignedSum += channelWeight * unitPhase
                * std::polar(1.0, phase);
            channelWeightSum += channelWeight;
            ++contributingChannels;
        }
        bandWeightSum += frequencyWeight;
        if (contributingChannels < MinimumValidChannelCount
            || channelWeightSum <= 1.0e-12) {
            continue;
        }
        const double coherence = std::clamp(
            std::abs(alignedSum) / channelWeightSum,
            0.0, 1.0);
        coherentWeight += frequencyWeight
            * smoothStep(0.15, 0.65, coherence);
    }
    return bandWeightSum > 1.0e-12
        ? std::clamp(
              coherentWeight / bandWeightSum, 0.0, 1.0)
        : 0.0;
}

bool RobustSrpPhatScorer::refineContinuousPeak(
    const QVector<int> &seedGroupIndices,
    RobustSpatialEstimate *result) const
{
    if (result == nullptr || seedGroupIndices.size() < 12
        || m_validGroupIndices.size() < 12) {
        return false;
    }

    const double seedStep =
        m_configuration.spatialResolutionMode
            == SpatialResolutionMode::HighPrecision
        ? 2.0 : 5.0;
    QVector<ScanDirection> seedDirections;
    const int seedCount =
        static_cast<int>(180.0 / seedStep) + 1;
    seedDirections.reserve(seedCount * seedCount);
    ScanDirection bestDirection;
    double bestSeedScore =
        -std::numeric_limits<double>::infinity();
    for (double elevation = -90.0;
         elevation <= 90.0 + 1.0e-9;
         elevation += seedStep) {
        for (double azimuth = -90.0;
             azimuth <= 90.0 + 1.0e-9;
             azimuth += seedStep) {
            const ScanDirection candidate {
                azimuth, elevation,
            };
            const double score = scoreDirectionCubic(
                candidate, seedGroupIndices);
            seedDirections.append(candidate);
            if (score > bestSeedScore) {
                bestSeedScore = score;
                bestDirection = candidate;
            }
        }
    }
    if (!std::isfinite(bestSeedScore)) {
        return false;
    }

    double step = std::clamp(seedStep, 2.0, 6.0);
    double bestScore = scoreDirectionSinc(
        bestDirection, m_validGroupIndices);
    int smallImprovementCount = 0;
    for (int iteration = 0;
         iteration < 40 && step >= 0.05;
         ++iteration) {
        ScanDirection iterationBest = bestDirection;
        double iterationBestScore = bestScore;
        for (int elevationOffset = -1;
             elevationOffset <= 1;
             ++elevationOffset) {
            for (int azimuthOffset = -1;
                 azimuthOffset <= 1;
                 ++azimuthOffset) {
                if (azimuthOffset == 0
                    && elevationOffset == 0) {
                    continue;
                }
                const ScanDirection candidate {
                    std::clamp(
                        bestDirection.azimuthDegrees
                            + azimuthOffset * step,
                        -90.0, 90.0),
                    std::clamp(
                        bestDirection.elevationDegrees
                            + elevationOffset * step,
                        -90.0, 90.0),
                };
                const double score = scoreDirectionSinc(
                    candidate, m_validGroupIndices);
                if (score > iterationBestScore) {
                    iterationBestScore = score;
                    iterationBest = candidate;
                }
            }
        }

        const double improvement =
            iterationBestScore - bestScore;
        if (iterationBest == bestDirection) {
            step *= 0.5;
            smallImprovementCount = 0;
            continue;
        }
        bestDirection = iterationBest;
        bestScore = iterationBestScore;
        smallImprovementCount = improvement < 1.0e-5
            ? smallImprovementCount + 1 : 0;
        if (smallImprovementCount >= 2) {
            break;
        }
    }
    if (!std::isfinite(bestScore)) {
        return false;
    }

    double sidelobeScore =
        -std::numeric_limits<double>::infinity();
    const QVector3D peakVector = directionFromAngles(
        bestDirection.azimuthDegrees,
        bestDirection.elevationDegrees);
    for (int index = 0; index < seedDirections.size(); ++index) {
        const ScanDirection &candidate =
            seedDirections.at(index);
        const QVector3D candidateVector = directionFromAngles(
            candidate.azimuthDegrees,
            candidate.elevationDegrees);
        const double cosine = std::clamp(
            static_cast<double>(QVector3D::dotProduct(
                peakVector, candidateVector)),
            -1.0, 1.0);
        const double angularDistance =
            qRadiansToDegrees(std::acos(cosine));
        if (angularDistance >= 5.0) {
            sidelobeScore = std::max(
                sidelobeScore,
                scoreDirectionSinc(
                    candidate, m_validGroupIndices));
        }
    }
    if (!std::isfinite(sidelobeScore)) {
        sidelobeScore = bestScore;
    }
    const double peakContrast = std::clamp(
        (bestScore - sidelobeScore)
            / std::max(std::abs(bestScore), 1.0e-9),
        0.0, 1.0);
    const double psrScore = smoothStep(
        0.01, 0.15, peakContrast);
    result->peakContrast = peakContrast;

    constexpr double CurvatureStep = 0.25;
    const auto offsetScore =
        [this, &bestDirection](double azimuthOffset,
                              double elevationOffset) {
        return scoreDirectionSinc(
            {std::clamp(
                 bestDirection.azimuthDegrees
                     + azimuthOffset,
                 -90.0, 90.0),
             std::clamp(
                 bestDirection.elevationDegrees
                     + elevationOffset,
                 -90.0, 90.0)},
            m_validGroupIndices);
    };
    const double xMinus = offsetScore(-CurvatureStep, 0.0);
    const double xPlus = offsetScore(CurvatureStep, 0.0);
    const double yMinus = offsetScore(0.0, -CurvatureStep);
    const double yPlus = offsetScore(0.0, CurvatureStep);
    const double xyPlusPlus =
        offsetScore(CurvatureStep, CurvatureStep);
    const double xyPlusMinus =
        offsetScore(CurvatureStep, -CurvatureStep);
    const double xyMinusPlus =
        offsetScore(-CurvatureStep, CurvatureStep);
    const double xyMinusMinus =
        offsetScore(-CurvatureStep, -CurvatureStep);
    const double inverseStepSquared =
        1.0 / (CurvatureStep * CurvatureStep);
    const double hxx = -(xPlus - 2.0 * bestScore + xMinus)
        * inverseStepSquared;
    const double hyy = -(yPlus - 2.0 * bestScore + yMinus)
        * inverseStepSquared;
    const double hxy = -(
        xyPlusPlus - xyPlusMinus
        - xyMinusPlus + xyMinusMinus)
        / (4.0 * CurvatureStep * CurvatureStep);
    const double determinant = hxx * hyy - hxy * hxy;
    double curvatureScore = 0.0;
    if (std::isfinite(hxx) && std::isfinite(hyy)
        && std::isfinite(hxy)
        && hxx > 0.0 && hyy > 0.0
        && determinant > 1.0e-12) {
        const double trace = hxx + hyy;
        const double discriminant = std::sqrt(std::max(
            0.0, trace * trace - 4.0 * determinant));
        const double minimumEigenvalue =
            0.5 * (trace - discriminant);
        const double normalizedCurvature =
            minimumEigenvalue
            / std::max(std::abs(bestScore), 1.0e-9);
        curvatureScore = smoothStep(
            1.0e-5, 5.0e-4, normalizedCurvature);
        const double residualFloor = std::max(
            1.0e-8,
            0.02 * std::max(
                bestScore - sidelobeScore,
                std::abs(bestScore) * 0.01));
        result->azimuthStandardDeviationDegrees =
            std::sqrt(std::max(
                residualFloor * hyy / determinant,
                1.0e-8));
        result->elevationStandardDeviationDegrees =
            std::sqrt(std::max(
                residualFloor * hxx / determinant,
                1.0e-8));
        const double covariance =
            -residualFloor * hxy / determinant;
        result->azimuthElevationCorrelation =
            std::clamp(
                covariance / (
                    result->azimuthStandardDeviationDegrees
                    * result->elevationStandardDeviationDegrees),
                -0.99, 0.99);
    }

    const double channelCoverage = std::clamp(
        m_validChannelCount
            / static_cast<double>(
                m_configuration.microphones.size()),
        0.0, 1.0);
    double coherentCoverage = 0.0;
    for (int groupIndex : m_validGroupIndices) {
        coherentCoverage +=
            m_baselineGroups.at(
                groupIndex).reliableBinFraction;
    }
    coherentCoverage = std::clamp(
        coherentCoverage
            / static_cast<double>(m_baselineGroups.size()),
        0.0, 1.0);
    result->coherentCoverage = coherentCoverage;
    result->curvatureScore = curvatureScore;
    result->steeredChannelCoherence =
        steeredChannelCoherence(bestDirection);
    result->reliability = std::pow(
        channelCoverage * coherentCoverage
            * result->steeredChannelCoherence
            * psrScore * curvatureScore,
        0.2);
    if (!std::isfinite(result->reliability)) {
        result->reliability = 0.0;
        return false;
    }

    result->hasDirection =
        result->reliability > 0.0
        && result->azimuthStandardDeviationDegrees > 0.0
        && result->elevationStandardDeviationDegrees > 0.0;
    result->peakToSidelobeRatio =
        bestScore / std::max(
            std::abs(sidelobeScore), 1.0e-9);
    result->direction.azimuthDegrees =
        bestDirection.azimuthDegrees;
    result->direction.elevationDegrees =
        bestDirection.elevationDegrees;
    result->direction.confidence = result->reliability;
    result->direction.residualSamples =
        std::hypot(
            result->azimuthStandardDeviationDegrees,
            result->elevationStandardDeviationDegrees);
    result->direction.validPairCount =
        m_validGroupIndices.size();
    result->direction.centerSample = m_centerSample;
    return result->hasDirection;
}

} // namespace MicArray
