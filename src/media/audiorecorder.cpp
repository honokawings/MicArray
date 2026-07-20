#include "media/audiorecorder.h"

#include "media/audioframedecoder.h"

#include <QMetaObject>
#include <QPointer>
#include <QSemaphore>

#include <algorithm>
#include <memory>

namespace MicArray {
namespace {

struct StopOutcome {
    bool success = true;
    QString reason;
    QString error;
    RecordingSummary summary;
};

struct BlockingStopState {
    QSemaphore completed;
    StopOutcome outcome;
};

} // namespace

class AudioRecorder::Worker : public QObject
{
public:
    explicit Worker(AudioRecorder *owner)
        : m_owner(owner)
    {
    }

    void start(const RecordingSettings &settings)
    {
        m_writer = std::make_unique<MultiChannelWavWriter>();
        QString error;
        const bool success = m_writer->open(settings, &error);
        m_acceptingFrames = success;
        const RecordingSummary summary = m_writer->summary();
        post([success, error, summary](AudioRecorder *owner) {
            owner->handleStarted(success, error, summary);
        });
    }

    void append(const QByteArray &payload)
    {
        const quint64 inputBytes = static_cast<quint64>(payload.size());
        if (!m_acceptingFrames || m_writer == nullptr) {
            post([inputBytes](AudioRecorder *owner) {
                owner->handleFrameDiscarded(inputBytes);
            });
            return;
        }

        QString error;
        if (payload.size() != AudioFrameDecoder::FramePayloadBytes) {
            error = QStringLiteral("Expected 38400-byte audio payload, got %1 bytes").arg(payload.size());
            m_acceptingFrames = false;
            QString closeError;
            m_writer->close(QStringLiteral("format-error"), &closeError);
            if (!closeError.isEmpty() && !error.contains(closeError)) {
                error += QStringLiteral("; %1").arg(closeError);
            }
            const RecordingSummary summary = m_writer->summary();
            post([inputBytes, error, summary](AudioRecorder *owner) {
                owner->handleFrameFailed(inputBytes, error, summary);
            });
            return;
        }
        if (!m_writer->appendFrame(payload, &error)) {
            m_acceptingFrames = false;
            QString closeError;
            m_writer->close(QStringLiteral("io-error"), &closeError);
            if (!closeError.isEmpty() && !error.contains(closeError)) {
                error += QStringLiteral("; %1").arg(closeError);
            }
            const RecordingSummary summary = m_writer->summary();
            post([inputBytes, error, summary](AudioRecorder *owner) {
                owner->handleFrameFailed(inputBytes, error, summary);
            });
            return;
        }

        const RecordingSummary summary = m_writer->summary();
        post([inputBytes, summary](AudioRecorder *owner) {
            owner->handleFrameConsumed(inputBytes, summary);
        });
    }

    StopOutcome stop(const QString &reason)
    {
        StopOutcome outcome;
        outcome.reason = reason;
        m_acceptingFrames = false;
        if (m_writer != nullptr && m_writer->isOpen()) {
            outcome.success = m_writer->close(reason, &outcome.error);
        }
        if (m_writer != nullptr) {
            outcome.summary = m_writer->summary();
        }
        return outcome;
    }

    void stopAsync(const QString &reason)
    {
        const StopOutcome outcome = stop(reason);
        post([outcome](AudioRecorder *owner) {
            owner->handleStopped(outcome.success,
                                 outcome.reason,
                                 outcome.error,
                                 outcome.summary);
        });
    }

private:
    template<typename Callback>
    void post(Callback callback)
    {
        const QPointer<AudioRecorder> owner = m_owner;
        if (owner.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(owner,
                                  [owner, callback]() {
                                      if (!owner.isNull()) {
                                          callback(owner.data());
                                      }
                                  },
                                  Qt::QueuedConnection);
    }

    QPointer<AudioRecorder> m_owner;
    std::unique_ptr<MultiChannelWavWriter> m_writer;
    bool m_acceptingFrames = false;
};

QString recordingStateName(RecordingState state)
{
    switch (state) {
    case RecordingState::Idle:
        return QStringLiteral("空闲");
    case RecordingState::Starting:
        return QStringLiteral("正在启动");
    case RecordingState::Recording:
        return QStringLiteral("录制中");
    case RecordingState::Stopping:
        return QStringLiteral("正在停止");
    case RecordingState::Error:
        return QStringLiteral("错误");
    }
    return QStringLiteral("未知");
}

AudioRecorder::AudioRecorder(QObject *parent)
    : AudioRecorder(MaxPendingBytes, parent)
{
}

AudioRecorder::AudioRecorder(quint64 maxPendingBytes, QObject *parent)
    : QObject(parent)
    , m_maxPendingBytes(std::max<quint64>(1, maxPendingBytes))
{
    qRegisterMetaType<RecordingState>("MicArray::RecordingState");
    qRegisterMetaType<RecordingStatistics>("MicArray::RecordingStatistics");
    m_worker = new Worker(this);
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.setObjectName(QStringLiteral("MicArrayRecordingThread"));
    m_thread.start();
}

AudioRecorder::~AudioRecorder()
{
    stopAndWait();
    m_thread.quit();
    m_thread.wait();
}

bool AudioRecorder::startRecording(const RecordingSettings &settings)
{
    if (m_state != RecordingState::Idle && m_state != RecordingState::Error) {
        return false;
    }
    if (settings.channels.isEmpty()) {
        m_statistics = {};
        m_statistics.error = QStringLiteral("At least one recording channel must be selected");
        setState(RecordingState::Error);
        emit recordingError(m_statistics.error);
        emit statisticsChanged(m_statistics);
        return false;
    }

    m_pendingBytes = 0;
    m_statistics = {};
    m_terminalFailure = false;
    m_recordingTimer.start();
    m_statisticsEmitTimer.start();
    setState(RecordingState::Starting);
    QMetaObject::invokeMethod(m_worker,
                              [worker = m_worker, settings]() {
                                  worker->start(settings);
                              },
                              Qt::QueuedConnection);
    return true;
}

bool AudioRecorder::enqueueFrame(const QByteArray &payload)
{
    if (m_state != RecordingState::Recording) {
        return false;
    }
    const quint64 payloadBytes = static_cast<quint64>(payload.size());
    if (payloadBytes > m_maxPendingBytes - std::min(m_pendingBytes, m_maxPendingBytes)) {
        m_statistics.error = QStringLiteral("Recording queue overflow; recording stopped before dropping a frame");
        m_terminalFailure = true;
        emit recordingError(m_statistics.error);
        setState(RecordingState::Stopping);
        QMetaObject::invokeMethod(m_worker,
                                  [worker = m_worker]() {
                                      worker->stopAsync(QStringLiteral("queue-overflow"));
                                  },
                                  Qt::QueuedConnection);
        return false;
    }

    m_pendingBytes += payloadBytes;
    m_statistics.pendingBytes = m_pendingBytes;
    QMetaObject::invokeMethod(m_worker,
                              [worker = m_worker, payload]() {
                                  worker->append(payload);
                              },
                              Qt::QueuedConnection);
    return true;
}

void AudioRecorder::stopRecording(const QString &reason)
{
    if (m_state != RecordingState::Recording && m_state != RecordingState::Starting) {
        return;
    }
    setState(RecordingState::Stopping);
    QMetaObject::invokeMethod(m_worker,
                              [worker = m_worker, reason]() {
                                  worker->stopAsync(reason);
                              },
                              Qt::QueuedConnection);
}

bool AudioRecorder::stopAndWait(int timeoutMs)
{
    if (!m_thread.isRunning() || m_worker == nullptr) {
        return true;
    }
    if (m_state == RecordingState::Idle) {
        return true;
    }

    const auto stopState = std::make_shared<BlockingStopState>();
    const bool invoked = QMetaObject::invokeMethod(m_worker,
                                                    [worker = m_worker, stopState]() {
                                                        stopState->outcome = worker->stop(QStringLiteral("application-exit"));
                                                        stopState->completed.release();
                                                    },
                                                    Qt::QueuedConnection);
    if (!invoked) {
        m_statistics.error = QStringLiteral("Failed to stop the recording worker");
        setState(RecordingState::Error);
        return false;
    }
    if (!stopState->completed.tryAcquire(1, std::max(0, timeoutMs))) {
        m_statistics.error = QStringLiteral("Recording worker did not stop within %1 ms").arg(timeoutMs);
        setState(RecordingState::Error);
        emit recordingError(m_statistics.error);
        emit statisticsChanged(m_statistics);
        return false;
    }
    m_pendingBytes = 0;
    handleStopped(stopState->outcome.success,
                  stopState->outcome.reason,
                  stopState->outcome.error,
                  stopState->outcome.summary);
    return stopState->outcome.success;
}

void AudioRecorder::setState(RecordingState state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    m_statistics.state = state;
    emit stateChanged(state);
}

void AudioRecorder::handleStarted(bool success, const QString &error, const RecordingSummary &summary)
{
    if (m_state != RecordingState::Starting && m_state != RecordingState::Stopping) {
        return;
    }
    if (!success) {
        m_terminalFailure = true;
        m_statistics.error = error;
        updateSummary(summary, true);
        setState(RecordingState::Error);
        emit recordingError(error);
        emit statisticsChanged(m_statistics);
        return;
    }
    updateSummary(summary, true);
    if (m_state == RecordingState::Starting) {
        setState(RecordingState::Recording);
    }
    emit statisticsChanged(m_statistics);
}

void AudioRecorder::handleFrameConsumed(quint64 inputBytes, const RecordingSummary &summary)
{
    m_pendingBytes = inputBytes >= m_pendingBytes ? 0 : m_pendingBytes - inputBytes;
    if (m_state == RecordingState::Idle) {
        return;
    }
    const bool forceEmit = !m_statisticsEmitTimer.isValid() || m_statisticsEmitTimer.elapsed() >= 200;
    updateSummary(summary, forceEmit);
    if (forceEmit) {
        m_statisticsEmitTimer.restart();
    }
}

void AudioRecorder::handleFrameFailed(quint64 inputBytes,
                                      const QString &error,
                                      const RecordingSummary &summary)
{
    m_pendingBytes = inputBytes >= m_pendingBytes ? 0 : m_pendingBytes - inputBytes;
    m_terminalFailure = true;
    m_statistics.error = error;
    updateSummary(summary, true);
    setState(RecordingState::Error);
    emit recordingError(error);
    emit statisticsChanged(m_statistics);
}

void AudioRecorder::handleFrameDiscarded(quint64 inputBytes)
{
    m_pendingBytes = inputBytes >= m_pendingBytes ? 0 : m_pendingBytes - inputBytes;
    ++m_statistics.discardedFrames;
    m_statistics.pendingBytes = m_pendingBytes;
    emit statisticsChanged(m_statistics);
}

void AudioRecorder::handleStopped(bool success,
                                  const QString &reason,
                                  const QString &error,
                                  const RecordingSummary &summary)
{
    m_pendingBytes = 0;
    updateSummary(summary, true);
    if (m_terminalFailure || !success || reason == QStringLiteral("queue-overflow")) {
        if (!error.isEmpty()) {
            m_statistics.error = error;
        } else if (m_statistics.error.isEmpty()) {
            m_statistics.error = QStringLiteral("Recording stopped because the queue overflowed");
        }
        setState(RecordingState::Error);
        if (!m_statistics.error.isEmpty()) {
            emit recordingError(m_statistics.error);
        }
    } else {
        setState(RecordingState::Idle);
    }
    emit statisticsChanged(m_statistics);
}

void AudioRecorder::updateSummary(const RecordingSummary &summary, bool forceEmit)
{
    m_statistics.outputPath = summary.manifestPath;
    m_statistics.elapsedMs = m_recordingTimer.isValid() ? m_recordingTimer.elapsed() : 0;
    m_statistics.inputFrames = summary.inputFrames;
    m_statistics.dataBytes = summary.dataBytes;
    m_statistics.fileBytes = summary.fileBytes;
    m_statistics.pendingBytes = m_pendingBytes;
    if (forceEmit) {
        emit statisticsChanged(m_statistics);
    }
}

} // namespace MicArray
