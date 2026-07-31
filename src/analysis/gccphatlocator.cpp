#include "analysis/gccphatlocator.h"

#include "analysis/arraygeometry.h"

#include <algorithm>
#include <cmath>

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

} // namespace

GccPhatLocator::~GccPhatLocator() = default;

LocatorDescriptor GccPhatLocator::descriptor() const
{
    return {
        QStringLiteral("gcc-phat-tdoa"),
        QStringLiteral("GCC-PHAT 时差定位"),
        QStringLiteral(
            "多基线相干加权 SRP、连续峰值精化与鲁棒远场方向估计"),
    };
}

bool GccPhatLocator::configure(
    const LocatorConfiguration &configuration,
    QString *errorMessage)
{
    const bool valid = configuration.microphones.size() == 48
        && configuration.sampleRate >= 8000
        && configuration.sampleRate <= 192000
        && configuration.windowSamples >= 512
        && configuration.windowSamples <= 8192
        && isPowerOfTwo(configuration.windowSamples)
        && std::isfinite(
            configuration.soundSpeedMetersPerSecond)
        && configuration.soundSpeedMetersPerSecond >= 300.0
        && configuration.soundSpeedMetersPerSecond <= 380.0
        && std::isfinite(configuration.confidenceThreshold)
        && configuration.confidenceThreshold >= 0.0
        && configuration.confidenceThreshold <= 1.0
        && std::isfinite(configuration.smoothingFactor)
        && configuration.smoothingFactor >= 0.0
        && configuration.smoothingFactor <= 1.0
        && std::all_of(
            configuration.microphones.cbegin(),
            configuration.microphones.cend(),
            finitePosition);
    if (!valid) {
        if (errorMessage != nullptr) {
            *errorMessage =
                QStringLiteral("GCC-PHAT 定位配置无效");
        }
        return false;
    }

    QString scorerError;
    if (!m_scorer.configure(configuration, &scorerError)) {
        if (errorMessage != nullptr) {
            *errorMessage = scorerError;
        }
        return false;
    }
    m_configuration = configuration;
    m_configured = true;
    invalidateWindowCache();
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

QVector<SoundSourceDirection> GccPhatLocator::locate(
    const MultiChannelAudioWindow &window)
{
    if (!ensureWindow(window)
        || !m_cachedEstimate.hasDirection
        || m_cachedEstimate.reliability
            < m_configuration.confidenceThreshold) {
        return {};
    }
    return {m_cachedEstimate.direction};
}

void GccPhatLocator::prepareSpatialResponseGrid(
    const SpatialResponseGrid &grid)
{
    if (m_configured) {
        m_scorer.prepareGrid(grid);
    }
}

SpatialResponseResult GccPhatLocator::evaluateSpatialResponse(
    const MultiChannelAudioWindow &window,
    const SpatialResponseGrid &grid)
{
    SpatialResponseResult result;
    if (grid.columns <= 0 || grid.rows <= 0
        || grid.directions.size()
            != grid.columns * grid.rows
        || !ensureWindow(window)) {
        return result;
    }

    const RobustSpatialEstimate spatial =
        m_scorer.estimate(&grid);
    result.values = spatial.gridValues;
    result.reliability = m_cachedEstimate.reliability;
    if (m_cachedEstimate.hasDirection
        && m_cachedEstimate.reliability
            >= m_configuration.confidenceThreshold) {
        result.hasRefinedPeak = true;
        result.refinedPeak = {
            m_cachedEstimate.direction.azimuthDegrees,
            m_cachedEstimate.direction.elevationDegrees,
        };
        result.azimuthStandardDeviationDegrees =
            m_cachedEstimate.azimuthStandardDeviationDegrees;
        result.elevationStandardDeviationDegrees =
            m_cachedEstimate.elevationStandardDeviationDegrees;
        result.azimuthElevationCorrelation =
            m_cachedEstimate.azimuthElevationCorrelation;
        result.peakToSidelobeRatio =
            m_cachedEstimate.peakToSidelobeRatio;
    }
    return result;
}

bool GccPhatLocator::ensureWindow(
    const MultiChannelAudioWindow &window)
{
    if (!m_configured) {
        return false;
    }
    if (window.sampleRate != m_configuration.sampleRate
        || window.channels.size()
            != m_configuration.microphones.size()) {
        invalidateWindowCache();
        (void)m_scorer.prepareWindow(window);
        return false;
    }
    if (windowCacheMatches(window)) {
        return true;
    }

    invalidateWindowCache();
    if (!m_scorer.prepareWindow(window)) {
        return false;
    }
    m_cachedEstimate = m_scorer.estimate();

    m_cachedCenterSample = window.centerSample;
    m_cachedSampleRate = window.sampleRate;
    m_cachedSampleCount =
        m_configuration.windowSamples;
    m_cachedChannelCount = window.channels.size();
    m_cachedFirstChannelData = window.channels.isEmpty()
        ? nullptr : window.channels.first().constData();
    return true;
}

bool GccPhatLocator::windowCacheMatches(
    const MultiChannelAudioWindow &window) const
{
    return !window.channels.isEmpty()
        && m_cachedCenterSample == window.centerSample
        && m_cachedSampleRate == window.sampleRate
        && m_cachedSampleCount
            == m_configuration.windowSamples
        && m_cachedChannelCount == window.channels.size()
        && m_cachedFirstChannelData
            == window.channels.first().constData();
}

void GccPhatLocator::invalidateWindowCache()
{
    m_cachedEstimate = {};
    m_cachedCenterSample =
        std::numeric_limits<qint64>::min();
    m_cachedSampleRate = 0;
    m_cachedSampleCount = 0;
    m_cachedChannelCount = 0;
    m_cachedFirstChannelData = nullptr;
}

} // namespace MicArray
