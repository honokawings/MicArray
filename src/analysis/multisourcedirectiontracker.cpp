#include "analysis/multisourcedirectiontracker.h"

#include "analysis/arraygeometry.h"

#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace MicArray {
namespace {

double angularDistance(const SoundSourceDirection &left,
                       const SoundSourceDirection &right)
{
    const QVector3D a = directionFromAngles(left.azimuthDegrees,
                                            left.elevationDegrees);
    const QVector3D b = directionFromAngles(right.azimuthDegrees,
                                            right.elevationDegrees);
    const double cosine = std::clamp(
        static_cast<double>(QVector3D::dotProduct(a, b)), -1.0, 1.0);
    return qRadiansToDegrees(std::acos(cosine));
}

void moveTowards(SoundSourceDirection *current,
                 const SoundSourceDirection &target,
                 double factor,
                 double maximumStepDegrees)
{
    const QVector3D from = directionFromAngles(current->azimuthDegrees,
                                                current->elevationDegrees);
    const QVector3D to = directionFromAngles(target.azimuthDegrees,
                                              target.elevationDegrees);
    const double cosine = std::clamp(
        static_cast<double>(QVector3D::dotProduct(from, to)), -1.0, 1.0);
    const double angleRadians = std::acos(cosine);
    if (angleRadians <= 1.0e-9) {
        return;
    }
    const double angleDegrees = qRadiansToDegrees(angleRadians);
    const double stepDegrees = std::min(angleDegrees * factor,
                                        maximumStepDegrees);
    const double interpolation = std::clamp(stepDegrees / angleDegrees,
                                            0.0, 1.0);
    const double sine = std::sin(angleRadians);
    QVector3D moved;
    if (std::abs(sine) > 1.0e-9) {
        moved = from * static_cast<float>(
                    std::sin((1.0 - interpolation) * angleRadians) / sine)
            + to * static_cast<float>(
                    std::sin(interpolation * angleRadians) / sine);
    } else {
        moved = from * static_cast<float>(1.0 - interpolation)
            + to * static_cast<float>(interpolation);
    }
    const SoundSourceDirection angles = anglesFromDirection(moved.normalized());
    current->azimuthDegrees = angles.azimuthDegrees;
    current->elevationDegrees = angles.elevationDegrees;
}

bool validMeasurement(const SoundSourceDirection &direction)
{
    return std::isfinite(direction.azimuthDegrees)
        && std::isfinite(direction.elevationDegrees)
        && std::isfinite(direction.confidence)
        && direction.confidence > 0.0;
}

} // namespace

MultiSourceDirectionTracker::MultiSourceDirectionTracker(
    MultiSourceDirectionTrackerConfig configuration)
    : m_configuration(std::move(configuration))
{
    m_configuration.maximumTracks = std::clamp(
        m_configuration.maximumTracks, 1, 3);
    m_configuration.acquisitionHits = std::max(
        1, m_configuration.acquisitionHits);
    m_configuration.jumpConfirmationHits = std::max(
        1, m_configuration.jumpConfirmationHits);
    m_configuration.timeoutMicroseconds = std::max(
        m_configuration.holdMicroseconds + 1,
        m_configuration.timeoutMicroseconds);
    m_tracks.resize(m_configuration.maximumTracks);
    for (int index = 0; index < m_tracks.size(); ++index) {
        m_tracks[index].sourceId = index;
    }
}

QVector<SoundSourceDirection> MultiSourceDirectionTracker::update(
    const QVector<SoundSourceDirection> &measurements,
    qint64 timestampMicroseconds,
    DirectionTrackMode mode)
{
    if (timestampMicroseconds <= m_lastTimestampMicroseconds) {
        timestampMicroseconds = m_lastTimestampMicroseconds + 1;
    }
    m_lastTimestampMicroseconds = timestampMicroseconds;
    for (Track &track : m_tracks) {
        // Expire before association so a stale track cannot be revived by the
        // first measurement after a long gap, or block a fresh acquisition.
        if (track.allocated
            && timestampMicroseconds - track.lastMeasurementMicroseconds
                > m_configuration.timeoutMicroseconds) {
            const int sourceId = track.sourceId;
            track = {};
            track.sourceId = sourceId;
        }
        track.measuredThisUpdate = false;
    }

    QVector<SoundSourceDirection> candidates;
    candidates.reserve(std::min(measurements.size(),
                                static_cast<qsizetype>(m_configuration.maximumTracks)));
    for (const SoundSourceDirection &measurement : measurements) {
        if (validMeasurement(measurement)) {
            candidates.append(measurement);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const SoundSourceDirection &left,
                 const SoundSourceDirection &right) {
                  return left.confidence > right.confidence;
              });
    if (candidates.size() > m_configuration.maximumTracks) {
        candidates.resize(m_configuration.maximumTracks);
    }

    QVector<int> assignments(candidates.size(), -1);
    QVector<bool> trackAssigned(m_tracks.size(), false);
    if (mode == DirectionTrackMode::PreserveInputIds) {
        for (int candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
            const int sourceId = candidates.at(candidateIndex).sourceId;
            if (sourceId >= 0 && sourceId < m_tracks.size()
                && !trackAssigned.at(sourceId)) {
                assignments[candidateIndex] = sourceId;
                trackAssigned[sourceId] = true;
            }
        }
    } else {
        struct Pair {
            int candidate = -1;
            int track = -1;
            double distance = std::numeric_limits<double>::infinity();
        };
        QVector<Pair> pairs;
        for (int candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
            for (int trackIndex = 0; trackIndex < m_tracks.size(); ++trackIndex) {
                if (m_tracks.at(trackIndex).allocated) {
                    pairs.append({candidateIndex, trackIndex,
                                  angularDistance(candidates.at(candidateIndex),
                                                  m_tracks.at(trackIndex).direction)});
                }
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair &left, const Pair &right) {
            return left.distance < right.distance;
        });
        for (double limit : {m_configuration.associationDistanceDegrees,
                             m_configuration.relocationDistanceDegrees}) {
            for (const Pair &pair : std::as_const(pairs)) {
                if (pair.distance > limit
                    || assignments.at(pair.candidate) >= 0
                    || trackAssigned.at(pair.track)) {
                    continue;
                }
                assignments[pair.candidate] = pair.track;
                trackAssigned[pair.track] = true;
            }
        }
    }

    for (int candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
        int trackIndex = assignments.at(candidateIndex);
        if (trackIndex < 0) {
            trackIndex = firstFreeTrack();
        }
        if (trackIndex < 0 || trackAssigned.at(trackIndex)) {
            continue;
        }
        trackAssigned[trackIndex] = true;
        applyMeasurement(&m_tracks[trackIndex], candidates.at(candidateIndex),
                         timestampMicroseconds);
    }
    for (int candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
        const int trackIndex = assignments.at(candidateIndex);
        if (trackIndex >= 0) {
            applyMeasurement(&m_tracks[trackIndex], candidates.at(candidateIndex),
                             timestampMicroseconds);
        }
    }

    return currentDirections(timestampMicroseconds);
}

void MultiSourceDirectionTracker::reset()
{
    m_tracks.clear();
    m_tracks.resize(m_configuration.maximumTracks);
    for (int index = 0; index < m_tracks.size(); ++index) {
        m_tracks[index].sourceId = index;
    }
    m_lastTimestampMicroseconds = 0;
}

void MultiSourceDirectionTracker::applyMeasurement(
    Track *track,
    const SoundSourceDirection &measurement,
    qint64 timestampMicroseconds)
{
    if (track == nullptr || track->measuredThisUpdate) {
        return;
    }
    track->measuredThisUpdate = true;
    SoundSourceDirection candidate = measurement;
    candidate.sourceId = track->sourceId;

    if (!track->allocated) {
        track->allocated = true;
        track->published = m_configuration.acquisitionHits <= 1;
        track->acquisitionHits = 1;
        track->direction = candidate;
        track->lastMeasurementMicroseconds = timestampMicroseconds;
        return;
    }

    if (!track->published) {
        if (angularDistance(track->direction, candidate)
            <= m_configuration.acquisitionConsistencyDegrees) {
            moveTowards(&track->direction, candidate, 0.5,
                        m_configuration.confirmedJumpStepDegrees);
            ++track->acquisitionHits;
        } else {
            track->direction = candidate;
            track->acquisitionHits = 1;
        }
        track->direction.confidence = candidate.confidence;
        track->lastMeasurementMicroseconds = timestampMicroseconds;
        if (track->acquisitionHits >= m_configuration.acquisitionHits) {
            track->published = true;
        }
        return;
    }

    const double distance = angularDistance(track->direction, candidate);
    if (distance <= m_configuration.jumpDistanceDegrees) {
        track->pendingJump = false;
        track->pendingJumpHits = 0;
        moveTowards(&track->direction, candidate,
                    m_configuration.smoothingFactor,
                    m_configuration.maximumStepDegrees);
    } else {
        if (track->pendingJump
            && angularDistance(track->pendingDirection, candidate)
                <= m_configuration.pendingConsistencyDegrees) {
            moveTowards(&track->pendingDirection, candidate, 0.4,
                        m_configuration.confirmedJumpStepDegrees);
            ++track->pendingJumpHits;
        } else {
            track->pendingJump = true;
            track->pendingJumpHits = 1;
            track->pendingDirection = candidate;
        }
        if (track->pendingJumpHits >= m_configuration.jumpConfirmationHits) {
            moveTowards(&track->direction, track->pendingDirection, 1.0,
                        m_configuration.confirmedJumpStepDegrees);
        }
    }
    const double confidenceFactor = m_configuration.confidenceSmoothingFactor;
    track->direction.confidence = std::clamp(
        track->direction.confidence * (1.0 - confidenceFactor)
            + candidate.confidence * confidenceFactor,
        0.0, 1.0);
    track->direction.residualSamples = candidate.residualSamples;
    track->direction.validPairCount = candidate.validPairCount;
    track->direction.centerSample = candidate.centerSample;
    track->direction.sourceId = track->sourceId;
    track->lastMeasurementMicroseconds = timestampMicroseconds;
}

int MultiSourceDirectionTracker::firstFreeTrack() const
{
    for (int index = 0; index < m_tracks.size(); ++index) {
        if (!m_tracks.at(index).allocated) {
            return index;
        }
    }
    return -1;
}

QVector<SoundSourceDirection> MultiSourceDirectionTracker::currentDirections(
    qint64 timestampMicroseconds)
{
    QVector<SoundSourceDirection> result;
    for (Track &track : m_tracks) {
        if (!track.allocated) {
            continue;
        }
        const qint64 age = timestampMicroseconds
            - track.lastMeasurementMicroseconds;
        if (age > m_configuration.timeoutMicroseconds) {
            const int sourceId = track.sourceId;
            track = {};
            track.sourceId = sourceId;
            continue;
        }
        if (!track.published
            || (!m_configuration.publishUnmeasuredTracks
                && !track.measuredThisUpdate)) {
            continue;
        }
        SoundSourceDirection direction = track.direction;
        if (age > m_configuration.holdMicroseconds) {
            const double decay = 1.0
                - static_cast<double>(age - m_configuration.holdMicroseconds)
                    / static_cast<double>(m_configuration.timeoutMicroseconds
                                          - m_configuration.holdMicroseconds);
            direction.confidence *= std::clamp(decay, 0.0, 1.0);
        }
        direction.sourceId = track.sourceId;
        result.append(direction);
    }
    std::sort(result.begin(), result.end(),
              [](const SoundSourceDirection &left,
                 const SoundSourceDirection &right) {
                  return left.sourceId < right.sourceId;
              });
    return result;
}

} // namespace MicArray
