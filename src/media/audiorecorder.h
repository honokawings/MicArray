#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QThread>

#include "media/multichannelwavwriter.h"

namespace MicArray {

enum class RecordingState {
    Idle,
    Starting,
    Recording,
    Stopping,
    Error,
};

QString recordingStateName(RecordingState state);

struct RecordingStatistics {
    RecordingState state = RecordingState::Idle;
    QString outputPath;
    qint64 elapsedMs = 0;
    quint64 inputFrames = 0;
    quint64 dataBytes = 0;
    quint64 fileBytes = 0;
    quint64 pendingBytes = 0;
    quint64 pendingFrames = 0;
    quint64 discardedFrames = 0;
    QString error;
};

class AudioRecorder : public QObject
{
    Q_OBJECT

public:
    static constexpr quint64 MaxPendingBytes = 64ULL * 1024 * 1024;
    static constexpr quint64 MaxPendingFrames = 2048;

    explicit AudioRecorder(QObject *parent = nullptr);
    explicit AudioRecorder(quint64 maxPendingBytes, QObject *parent = nullptr);
    AudioRecorder(quint64 maxPendingBytes, quint64 maxPendingFrames, QObject *parent = nullptr);
    ~AudioRecorder() override;

    bool startRecording(const RecordingSettings &settings);
    bool enqueueFrame(const QByteArray &payload);
    bool enqueueFrame(const QByteArray &payload, const GainConfiguration &gainConfiguration);
    void stopRecording(const QString &reason = QStringLiteral("user"));
    bool stopAndWait(int timeoutMs = 5000);

    RecordingState state() const { return m_state; }
    RecordingStatistics statistics() const { return m_statistics; }

signals:
    void stateChanged(MicArray::RecordingState state);
    void statisticsChanged(const MicArray::RecordingStatistics &statistics);
    void recordingError(const QString &message);

private:
    class Worker;

    void setState(RecordingState state);
    void handleStarted(bool success, const QString &error, const RecordingSummary &summary);
    void handleFrameConsumed(quint64 inputBytes, const RecordingSummary &summary);
    void handleFrameFailed(quint64 inputBytes, const QString &error, const RecordingSummary &summary);
    void handleFrameDiscarded(quint64 inputBytes);
    void handleStopped(bool success,
                       const QString &reason,
                       const QString &error,
                       const RecordingSummary &summary);
    void updateSummary(const RecordingSummary &summary, bool forceEmit);
    bool enqueueFrameInternal(const QByteArray &payload,
                              const GainConfiguration *gainConfiguration);
    void releasePendingFrame(quint64 inputBytes);

    QThread m_thread;
    Worker *m_worker = nullptr;
    RecordingState m_state = RecordingState::Idle;
    RecordingStatistics m_statistics;
    QElapsedTimer m_recordingTimer;
    QElapsedTimer m_statisticsEmitTimer;
    quint64 m_pendingBytes = 0;
    quint64 m_pendingFrames = 0;
    quint64 m_maxPendingBytes = MaxPendingBytes;
    quint64 m_maxPendingFrames = MaxPendingFrames;
    bool m_terminalFailure = false;
};

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::RecordingState)
Q_DECLARE_METATYPE(MicArray::RecordingStatistics)
