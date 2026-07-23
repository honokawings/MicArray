#include "analysis/livelocalizationworker.h"

#include "analysis/arraygeometry.h"
#include "media/audioframedecoder.h"

#include <QElapsedTimer>
#include <QScopeGuard>
#include <QtEndian>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <utility>

namespace MicArray {

namespace {

constexpr qint64 CalibrationFreshnessMicroseconds = 250'000;

} // namespace

LiveLocalizationWorker::LiveLocalizationWorker(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<LiveLocalizationSettings>("MicArray::LiveLocalizationSettings");
    qRegisterMetaType<SpatialResponseGrid>("MicArray::SpatialResponseGrid");
    qRegisterMetaType<LiveLocalizationResult>("MicArray::LiveLocalizationResult");
    qRegisterMetaType<LiveSpatialResponse>("MicArray::LiveSpatialResponse");
    qRegisterMetaType<VideoCalibrationSample>("MicArray::VideoCalibrationSample");
    registerBuiltInSoundSourceLocators(&m_registry);
    m_channels.resize(AudioFrameDecoder::ChannelCount);
    rebuildLocator();
}

void LiveLocalizationWorker::configure(const LiveLocalizationSettings &settings)
{
    const bool locatorChanged = settings.locatorId != m_settings.locatorId
        || settings.windowSamples != m_settings.windowSamples
        || settings.soundSpeedMetersPerSecond != m_settings.soundSpeedMetersPerSecond
        || settings.smoothingFactor != m_settings.smoothingFactor
        || settings.confidenceThreshold != m_settings.confidenceThreshold;
    m_settings = settings;
    if (locatorChanged || m_locator == nullptr) {
        rebuildLocator();
        clearStreamState();
    }
}

void LiveLocalizationWorker::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    clearStreamState();
    emit statusChanged(active ? QStringLiteral("正在积累实时阵列音频")
                              : QStringLiteral("实时定位已暂停"));
}

void LiveLocalizationWorker::setSpatialGrid(const SpatialResponseGrid &grid,
                                            quint64 generation)
{
    m_grid = grid;
    m_gridGeneration = generation;
    m_smoothedSpatialValues.clear();
}

void LiveLocalizationWorker::pushAdjustedFrame(const QByteArray &payload,
                                               quint32 timestampSeconds,
                                               quint32 timestampMicros)
{
    const auto consumed = qScopeGuard([this] {
        emit audioFrameConsumed();
    });
    if (!m_active) {
        return;
    }
    if (payload.size() != AudioFrameDecoder::FramePayloadBytes) {
        emit statusChanged(QStringLiteral("实时音频帧长度无效"));
        return;
    }
    for (int sample = 0; sample < AudioFrameDecoder::SamplesPerFrame; ++sample) {
        for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
            const int offset =
                (sample * AudioFrameDecoder::ChannelCount + channel) * 4;
            const qint32 value = qFromLittleEndian<qint32>(
                reinterpret_cast<const uchar *>(payload.constData() + offset));
            m_channels[channel].append(
                std::clamp(static_cast<float>(value / 8388608.0), -1.0f, 1.0f));
        }
    }
    m_totalSamples += AudioFrameDecoder::SamplesPerFrame;
    const int capacity = std::max(48000, m_settings.windowSamples * 2);
    const int excess = m_channels.first().size() - capacity;
    if (excess > 0) {
        for (QVector<float> &channel : m_channels) {
            channel.remove(0, excess);
        }
    }
    if (m_channels.first().size() < m_settings.windowSamples) {
        return;
    }
    const qint64 timestamp = static_cast<qint64>(timestampSeconds) * 1'000'000
        + timestampMicros;
    m_latestFrameTimestamp = timestamp;
    if (m_lastAnalysisTimestamp >= 0
        && timestamp - m_lastAnalysisTimestamp
            < static_cast<qint64>(m_settings.updateIntervalMs) * 1000) {
        return;
    }
    m_lastAnalysisTimestamp = timestamp;
    analyzeLatest(timestamp);
}

void LiveLocalizationWorker::captureStableDirection(quint64 requestId)
{
    VideoCalibrationSample sample;
    if (m_directionHistory.size() < 5
        || m_latestFrameTimestamp < 0
        || m_latestFrameTimestamp - m_directionHistory.last().timestampMicroseconds
            > CalibrationFreshnessMicroseconds) {
        emit calibrationSampleReady(requestId, false, sample,
                                    QStringLiteral("最近一秒的有效定位样本不足"));
        return;
    }
    QVector3D weighted;
    double totalWeight = 0.0;
    for (const TimedDirection &timed : std::as_const(m_directionHistory)) {
        const double weight = std::clamp(timed.direction.confidence, 0.0, 1.0);
        weighted += directionFromAngles(timed.direction.azimuthDegrees,
                                        timed.direction.elevationDegrees)
            * static_cast<float>(weight);
        totalWeight += weight;
    }
    if (totalWeight <= 0.0 || weighted.lengthSquared() < 1.0e-12f) {
        emit calibrationSampleReady(requestId, false, sample,
                                    QStringLiteral("定位置信度不足"));
        return;
    }
    weighted.normalize();
    const SoundSourceDirection mean = anglesFromDirection(weighted);
    double squaredDeviation = 0.0;
    double meanConfidence = 0.0;
    for (const TimedDirection &timed : std::as_const(m_directionHistory)) {
        const QVector3D direction = directionFromAngles(timed.direction.azimuthDegrees,
                                                        timed.direction.elevationDegrees);
        const double cosine = std::clamp(
            static_cast<double>(QVector3D::dotProduct(weighted, direction)),
            -1.0, 1.0);
        const double angle = qRadiansToDegrees(std::acos(cosine));
        squaredDeviation += angle * angle;
        meanConfidence += timed.direction.confidence;
    }
    const double deviation = std::sqrt(squaredDeviation / m_directionHistory.size());
    meanConfidence /= m_directionHistory.size();
    if (meanConfidence < m_settings.confidenceThreshold || deviation > 5.0) {
        emit calibrationSampleReady(
            requestId, false, sample,
            QStringLiteral("声源方向不够稳定（波动 %1°）").arg(deviation, 0, 'f', 1));
        return;
    }
    sample.direction = {mean.azimuthDegrees, mean.elevationDegrees};
    sample.confidence = meanConfidence;
    sample.angularDeviationDegrees = deviation;
    emit calibrationSampleReady(requestId, true, sample, QString());
}

void LiveLocalizationWorker::shutdown()
{
    m_active = false;
    clearStreamState();
    m_locator.reset();
    m_spatialProvider = nullptr;
    emit shutdownFinished();
}

void LiveLocalizationWorker::clearStreamState()
{
    for (QVector<float> &channel : m_channels) {
        channel.clear();
    }
    m_directionHistory.clear();
    m_smoothedSpatialValues.clear();
    m_totalSamples = 0;
    m_lastAnalysisTimestamp = -1;
    m_latestFrameTimestamp = -1;
}

bool LiveLocalizationWorker::rebuildLocator()
{
    m_locator = m_registry.create(m_settings.locatorId);
    m_spatialProvider = dynamic_cast<ISpatialResponseProvider *>(m_locator.get());
    if (m_locator == nullptr) {
        emit spatialResponseSupported(false);
        emit statusChanged(QStringLiteral("实时定位算法不存在"));
        return false;
    }
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    configuration.sampleRate = AudioFrameDecoder::DefaultSampleRate;
    configuration.windowSamples = m_settings.windowSamples;
    configuration.soundSpeedMetersPerSecond = m_settings.soundSpeedMetersPerSecond;
    configuration.confidenceThreshold = m_settings.confidenceThreshold;
    configuration.smoothingFactor = m_settings.smoothingFactor;
    QString error;
    if (!m_locator->configure(configuration, &error)) {
        m_locator.reset();
        m_spatialProvider = nullptr;
        emit statusChanged(error);
        emit spatialResponseSupported(false);
        return false;
    }
    emit spatialResponseSupported(m_spatialProvider != nullptr);
    return true;
}

MultiChannelAudioWindow LiveLocalizationWorker::latestWindow() const
{
    MultiChannelAudioWindow window;
    window.sampleRate = AudioFrameDecoder::DefaultSampleRate;
    window.centerSample = m_totalSamples - m_settings.windowSamples / 2;
    window.channels.resize(m_channels.size());
    const int start = m_channels.first().size() - m_settings.windowSamples;
    for (int channel = 0; channel < m_channels.size(); ++channel) {
        window.channels[channel] =
            m_channels.at(channel).mid(start, m_settings.windowSamples);
    }
    return window;
}

void LiveLocalizationWorker::analyzeLatest(qint64 timestampMicroseconds)
{
    if (m_locator == nullptr && !rebuildLocator()) {
        return;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    const MultiChannelAudioWindow window = latestWindow();
    const QVector<SoundSourceDirection> directions = m_locator->locate(window);
    LiveLocalizationResult result;
    result.directions = directions;
    result.timestampMicroseconds = timestampMicroseconds;
    result.elapsedMilliseconds = elapsed.elapsed();
    emit localizationReady(result);
    if (directions.isEmpty()) {
        emit statusChanged(QStringLiteral("未检测到可信声源"));
        return;
    }
    m_directionHistory.append({timestampMicroseconds, directions.first()});
    while (!m_directionHistory.isEmpty()
           && timestampMicroseconds - m_directionHistory.first().timestampMicroseconds
               > 1'000'000) {
        m_directionHistory.removeFirst();
    }
    if (m_spatialProvider == nullptr || m_grid.directions.isEmpty()) {
        emit statusChanged(QStringLiteral("实时声源定位正常"));
        return;
    }
    const SpatialResponseResult spatial =
        m_spatialProvider->evaluateSpatialResponse(window, m_grid);
    if (spatial.values.size() != m_grid.directions.size()
        || spatial.reliability <= 0.0) {
        emit statusChanged(QStringLiteral("空间响应不足"));
        return;
    }
    const auto [minimum, maximum] = std::minmax_element(
        spatial.values.cbegin(), spatial.values.cend());
    const double range = *maximum - *minimum;
    if (range <= 1.0e-12) {
        return;
    }
    QVector<float> normalized;
    normalized.reserve(spatial.values.size());
    for (float value : spatial.values) {
        normalized.append(static_cast<float>((value - *minimum) / range));
    }
    if (m_smoothedSpatialValues.size() == normalized.size()) {
        const float factor = static_cast<float>(m_settings.smoothingFactor);
        for (int index = 0; index < normalized.size(); ++index) {
            normalized[index] = m_smoothedSpatialValues.at(index) * (1.0f - factor)
                + normalized.at(index) * factor;
        }
    }
    m_smoothedSpatialValues = normalized;
    LiveSpatialResponse response;
    response.columns = m_grid.columns;
    response.rows = m_grid.rows;
    response.normalizedValues = normalized;
    response.timestampMicroseconds = timestampMicroseconds;
    response.generation = m_gridGeneration;
    response.reliability = spatial.reliability;
    emit spatialResponseReady(response);
    emit statusChanged(QStringLiteral("实时声源定位正常"));
}

} // namespace MicArray
