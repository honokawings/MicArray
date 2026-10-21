#pragma once

#include "analysis/multisourcedirectiontracker.h"
#include "media/audiolocationsideband.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace MicArray {

// LOC3 ranks are observations, not persistent source identities.
class PlLocalizationController : public QObject
{
    Q_OBJECT

public:
    explicit PlLocalizationController(QObject *parent = nullptr);
    void accept(const PlLocationResult &location);
    void reset();
    void setConfidenceThreshold(double threshold);
    QVector<SoundSourceDirection> directions() const { return m_directions; }
    bool hasObservation() const { return m_hasPacket; }
    quint16 currentFrame() const { return quint16(m_lastPacket.locatorFrameCounter); }
    bool isCurrentFrame(quint16 frame, quint8 mask) const
    {
        return m_hasPacket && !m_expired
            && m_lastPacket.recordType == 3 && m_lastPacket.version == 4
            && quint16(m_lastPacket.locatorFrameCounter) == frame
            && (m_lastPacket.flags & 7u) == mask;
    }
    bool stableCalibrationSample(VideoCalibrationSample *sample, QString *error) const;

signals:
    void directionsChanged(const QVector<SoundSourceDirection> &directions);
    void statusChanged(const QString &status);

private:
    struct Observation
    {
        qint64 timestamp = 0;
        SoundSourceDirection direction;
    };

    void finalizePacket();
    void expireObservation();
    void clearDirections();

    MultiSourceDirectionTracker m_tracker;
    QTimer m_expiryTimer;
    QElapsedTimer m_clock;
    QList<PlLocationResult> m_pending;
    QVector<SoundSourceDirection> m_directions;
    QVector<Observation> m_history;
    PlLocationResult m_lastPacket;
    qint64 m_lastObservationMs = -1;
    bool m_hasPacket = false;
    bool m_expired = false;
    double m_threshold = 0.45;
};

} // namespace MicArray
