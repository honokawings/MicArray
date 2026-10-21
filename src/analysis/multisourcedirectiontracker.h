#pragma once

#include "analysis/localizationtypes.h"

#include <QVector>

namespace MicArray {

enum class DirectionTrackMode {
    AssociateByAngle,
    PreserveInputIds,
};

struct MultiSourceDirectionTrackerConfig {
    int maximumTracks = 3;
    int acquisitionHits = 3;
    int jumpConfirmationHits = 3;
    double associationDistanceDegrees = 14.0;
    double relocationDistanceDegrees = 45.0;
    double acquisitionConsistencyDegrees = 7.0;
    double jumpDistanceDegrees = 8.0;
    double pendingConsistencyDegrees = 7.0;
    double smoothingFactor = 0.16;
    double confidenceSmoothingFactor = 0.25;
    double maximumStepDegrees = 2.2;
    double confirmedJumpStepDegrees = 4.0;
    qint64 holdMicroseconds = 800'000;
    qint64 timeoutMicroseconds = 1'500'000;
    bool publishUnmeasuredTracks = true;
};

class MultiSourceDirectionTracker
{
public:
    explicit MultiSourceDirectionTracker(
        MultiSourceDirectionTrackerConfig configuration = {});

    QVector<SoundSourceDirection> update(
        const QVector<SoundSourceDirection> &measurements,
        qint64 timestampMicroseconds,
        DirectionTrackMode mode = DirectionTrackMode::AssociateByAngle);
    void reset();

private:
    struct Track {
        bool allocated = false;
        bool published = false;
        bool measuredThisUpdate = false;
        int sourceId = -1;
        int acquisitionHits = 0;
        SoundSourceDirection direction;
        qint64 lastMeasurementMicroseconds = 0;
        bool pendingJump = false;
        int pendingJumpHits = 0;
        SoundSourceDirection pendingDirection;
    };

    void applyMeasurement(Track *track,
                          const SoundSourceDirection &measurement,
                          qint64 timestampMicroseconds);
    int firstFreeTrack() const;
    QVector<SoundSourceDirection> currentDirections(qint64 timestampMicroseconds);

    MultiSourceDirectionTrackerConfig m_configuration;
    QVector<Track> m_tracks;
    qint64 m_lastTimestampMicroseconds = 0;
};

} // namespace MicArray
