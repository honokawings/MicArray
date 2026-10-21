#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QVector>

#include <array>

namespace MicArray {

struct PlDenoiseStatus
{
    quint16 activeConfigId = 0;
    quint16 ackTransaction = 0;
    quint16 ackField = 0;
    quint8 flags = 0;
    quint32 latestSampleIndex = 0;
    quint32 overflowCount = 0;
    quint32 invalidControlCount = 0;
    quint32 buildId = 0;
    quint16 errorCode = 0;
    quint8 stage = 1;
    quint16 streamId = 0;
    quint32 wienerFrames = 0;
    quint32 profileFingerprint = 0;
    bool ready() const { return (flags & 1U) != 0; }
    bool busy() const { return (flags & 2U) != 0; }
};

struct PlBeamFrame
{
    std::array<QVector<float>, 3> beams;
    quint32 firstSampleIndex = 0;
    quint16 configId = 0;
    quint32 timestampSeconds = 0;
    quint32 timestampMicros = 0;
};

struct PlWienerFrame
{
    QVector<float> samples;
    quint32 firstSampleIndex = 0;
    quint16 streamId = 0;
    quint32 timestampSeconds = 0;
    quint32 timestampMicros = 0;
};

struct PlFinalFrame
{
    QVector<float> samples;
    quint32 firstSampleIndex = 0;
    quint16 streamId = 0;
    quint32 timestampSeconds = 0;
    quint32 timestampMicros = 0;
};

// AUD3/DNS1 own only CH7..CH47 high bytes. Raw low24 words are never modified.
class PlDenoiseTransport : public QObject
{
    Q_OBJECT
public:
    static constexpr quint32 FullProfileFingerprint = 0xb2c8f065U;
    explicit PlDenoiseTransport(QObject *parent = nullptr);
    static quint8 crc8(const QByteArray &bytes);
    void reset();
    void setExpectedStage(int stage);
    quint16 latestStreamId() const { return m_latestStatus.streamId; }
    void authorizeConfiguration(quint16 configId, bool restartStream);
    void flushBufferedBeforeConfiguration();
    void consume(const QByteArray &payload, quint32 seconds, quint32 micros);
    void invalidate(const QString &reason);
    void setControlTransactionActive(bool active) { m_controlTransactionActive = active; }
    void setPendingConfiguration(quint16 configId);
    bool faulted() const { return m_faulted; }
    quint64 receivedSamples() const { return m_receivedSamples; }
    quint64 rejectedPackets() const { return m_rejectedPackets; }
    qint64 lastAudioAgeMs() const { return m_lastAudio.isValid() ? m_lastAudio.elapsed() : -1; }

signals:
    void statusReceived(const PlDenoiseStatus &status);
    void frameReady(const PlBeamFrame &frame);
    void wienerFrameReady(const PlWienerFrame &frame);
    void finalFrameReady(const PlFinalFrame &frame);
    void staleStreamDiscarded(quint16 streamId);
    void faultRaised(const QString &reason);

private:
    void consumePacket(const QByteArray &packet, qint64 timestamp);
    void consumeWienerPacket(const QByteArray &packet, qint64 timestamp);
    void publishBuffered();
    void clearStream();
    bool m_authorized = false;
    bool m_deviceReady = false;
    bool m_faulted = false;
    bool m_haveSequence = false;
    bool m_controlTransactionActive = false;
    quint16 m_authorizedConfig = 0;
    quint16 m_streamConfig = 0;
    quint32 m_nextSample = 0;
    quint32 m_bufferFirst = 0;
    qint64 m_originTimestamp = 0;
    quint32 m_originSample = 0;
    std::array<QVector<float>, 3> m_buffer;
    quint64 m_receivedSamples = 0;
    quint64 m_rejectedPackets = 0;
    QElapsedTimer m_lastAudio;
    struct PendingPacket { QByteArray bytes; qint64 timestamp = 0; };
    QVector<PendingPacket> m_pendingPackets;
    quint16 m_pendingConfig = 0;
    bool m_havePendingConfig = false;
    quint32 m_pendingNextSample = 0;
    int m_pendingSamples = 0;
    int m_expectedStage = 1;
    PlDenoiseStatus m_latestStatus;
};

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::PlDenoiseStatus)
Q_DECLARE_METATYPE(MicArray::PlBeamFrame)
Q_DECLARE_METATYPE(MicArray::PlWienerFrame)
Q_DECLARE_METATYPE(MicArray::PlFinalFrame)
