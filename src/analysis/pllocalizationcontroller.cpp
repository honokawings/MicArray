#include "analysis/pllocalizationcontroller.h"

#include "analysis/arraygeometry.h"

#include <QtMath>

#include <algorithm>
#include <cmath>
#include <utility>

namespace MicArray {
namespace {

MultiSourceDirectionTrackerConfig trackerConfiguration()
{
    MultiSourceDirectionTrackerConfig configuration;
    configuration.relocationDistanceDegrees = configuration.associationDistanceDegrees;
    configuration.publishUnmeasuredTracks = false;
    return configuration;
}

bool samePacket(const PlLocationResult &a, const PlLocationResult &b)
{
    return a.recordType == b.recordType && a.version == b.version
        && a.sequence == b.sequence && a.locatorFrameCounter == b.locatorFrameCounter;
}

double angularDistance(const SoundSourceDirection &a, const SoundSourceDirection &b)
{
    const double cosine = QVector3D::dotProduct(
        directionFromAngles(a.azimuthDegrees, a.elevationDegrees),
        directionFromAngles(b.azimuthDegrees, b.elevationDegrees));
    return qRadiansToDegrees(std::acos(std::clamp(cosine, -1.0, 1.0)));
}

} // namespace

PlLocalizationController::PlLocalizationController(QObject *parent)
    : QObject(parent)
    , m_tracker(trackerConfiguration())
{
    m_clock.start();
    m_expiryTimer.setInterval(100);
    connect(&m_expiryTimer, &QTimer::timeout,
            this, &PlLocalizationController::expireObservation);
    m_expiryTimer.start();
}

void PlLocalizationController::accept(const PlLocationResult &location)
{
    if (location.recordType != 3)
    {
        m_pending = {location};
        finalizePacket();
        return;
    }
    if (location.sourceId >= 3)
    {
        return;
    }
    if (!m_pending.isEmpty() && !samePacket(m_pending.first(), location))
    {
        m_pending.clear();
    }
    for (PlLocationResult &pending : m_pending)
    {
        if (pending.sourceId == location.sourceId)
        {
            pending = location;
            return;
        }
    }
    m_pending.append(location);
    if (m_pending.size() == 3)
    {
        finalizePacket();
    }
}

void PlLocalizationController::finalizePacket()
{
    const PlLocationResult packet = m_pending.first();
    const bool loc3 = packet.recordType == 3;
    const quint32 mask = loc3 ? 0xffffu : 0xffffffffu;
    const quint32 halfRange = loc3 ? 0x8000u : 0x80000000u;
    const quint32 frame = loc3 ? packet.locatorFrameCounter : packet.sequence;
    const quint32 lastFrame = loc3 ? m_lastPacket.locatorFrameCounter : m_lastPacket.sequence;
    if (m_hasPacket && packet.recordType == m_lastPacket.recordType
        && packet.version == m_lastPacket.version)
    {
        const quint32 frameDelta = (frame - lastFrame) & mask;
        const quint32 sequenceDelta = (packet.sequence - m_lastPacket.sequence) & mask;
        if (frameDelta == 0 || frameDelta >= halfRange
            || (loc3 && (sequenceDelta == 0 || sequenceDelta >= halfRange)))
        {
            // A heartbeat cannot refresh the age of the last acoustic observation.
            m_pending.clear();
            expireObservation();
            return;
        }
    }
    m_lastPacket = packet;
    m_hasPacket = true;
    m_expired = false;
    m_lastObservationMs = m_clock.elapsed();
    const qint64 now = m_lastObservationMs * 1000 + 1;
    QVector<SoundSourceDirection> measurements;
    for (const PlLocationResult &location : std::as_const(m_pending))
    {
        if (location.locationValid && location.confidenceQ15 != 0
            && location.confidence() >= m_threshold)
        {
            measurements.append(location.physicalDirection());
        }
    }
    if (measurements.isEmpty())
    {
        clearDirections();
    }
    else
    {
        m_directions = m_tracker.update(measurements, now,
                                         DirectionTrackMode::AssociateByAngle);
        if (measurements.size() == 1)
        {
            m_history.append({now, measurements.first()});
        }
        else
        {
            m_history.clear();
        }
        while (!m_history.isEmpty() && now - m_history.first().timestamp > 2'000'000)
        {
            m_history.removeFirst();
        }
        emit directionsChanged(m_directions);
    }
    const QString status = measurements.isEmpty()
        ? QStringLiteral("PL LOC3：本帧无可信声源，标记已清除；帧 %1").arg(frame)
        : QStringLiteral("PL LOC3 v%1：原始方位 %2° / 俯仰 %3°，置信度 %4%；显示 %5 个跟踪峰；帧 %6")
            .arg(packet.version)
            .arg(measurements.first().azimuthDegrees, 0, 'f', 2)
            .arg(measurements.first().elevationDegrees, 0, 'f', 2)
            .arg(measurements.first().confidence * 100.0, 0, 'f', 0)
            .arg(m_directions.size()).arg(frame);
    emit statusChanged(status);
    m_pending.clear();
}

void PlLocalizationController::clearDirections()
{
    m_tracker.reset();
    m_history.clear();
    m_directions.clear();
    emit directionsChanged({});
}

void PlLocalizationController::expireObservation()
{
    if (!m_hasPacket || m_expired || m_clock.elapsed() - m_lastObservationMs < 1000)
    {
        return;
    }
    m_expired = true;
    m_pending.clear();
    clearDirections();
    emit statusChanged(QStringLiteral("PL 定位超过 1 秒没有新帧，标记已清除"));
}

void PlLocalizationController::reset()
{
    m_pending.clear();
    m_hasPacket = false;
    m_expired = false;
    m_lastObservationMs = -1;
    clearDirections();
    emit statusChanged(QStringLiteral("PL 定位：等待 LOC3 新帧"));
}

void PlLocalizationController::setConfidenceThreshold(double threshold)
{
    threshold = std::clamp(threshold, 0.0, 1.0);
    if (m_threshold != threshold)
    {
        m_threshold = threshold;
        clearDirections();
    }
}

bool PlLocalizationController::stableCalibrationSample(
    VideoCalibrationSample *sample, QString *error) const
{
    // The current PL pipeline updates at only several Hz; do not require 5 frames/s.
    QVector<Observation> recent;
    const qint64 now = m_clock.elapsed() * 1000 + 1;
    for (const Observation &entry : m_history)
    {
        if (now - entry.timestamp <= 2'000'000) { recent.append(entry); }
    }
    if (recent.size() < 3 || m_lastObservationMs < 0
        || m_clock.elapsed() - m_lastObservationMs >= 1000)
    {
        *error = QStringLiteral("保持单一声源稳定，等待最近两秒至少 3 个独立 PL 定位帧");
        return false;
    }
    QVector3D sum;
    double confidence = 0.0;
    for (const Observation &entry : recent)
    {
        sum += directionFromAngles(entry.direction.azimuthDegrees,
                                   entry.direction.elevationDegrees)
            * static_cast<float>(entry.direction.confidence);
        confidence += entry.direction.confidence;
    }
    const SoundSourceDirection mean = anglesFromDirection(sum);
    double squaredDeviation = 0.0;
    for (const Observation &entry : recent)
    {
        const double distance = angularDistance(mean, entry.direction);
        squaredDeviation += distance * distance;
    }
    sample->direction = {mean.azimuthDegrees, mean.elevationDegrees};
    sample->confidence = confidence / recent.size();
    sample->angularDeviationDegrees = std::sqrt(squaredDeviation / recent.size());
    if (sample->angularDeviationDegrees > 5.0)
    {
        *error = QStringLiteral("PL 声源方向不稳定，请保持单一声源位置不动");
        return false;
    }
    return true;
}

} // namespace MicArray
