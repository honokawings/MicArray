#include "media/audiorecorder.h"

#include "media/audioframedecoder.h"

#include <QMetaObject>
#include <QPointer>
#include <QSemaphore>
#include <QtEndian>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

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

struct RecordingTrack {
    int channel = 0;
    NoiseReductionDataFlowStage stage = NoiseReductionDataFlowStage::Suppression;
    QString key;
    QString name;
};

bool isKnownNoiseReductionStage(NoiseReductionDataFlowStage stage)
{
    switch (stage) {
    case NoiseReductionDataFlowStage::Suppression:
    case NoiseReductionDataFlowStage::BandPass:
    case NoiseReductionDataFlowStage::LogMmse:
    case NoiseReductionDataFlowStage::AutomaticGainControl:
        return true;
    }
    return false;
}

bool createWriterSettings(const RecordingSettings &settings,
                          QVector<RecordingSettings> *writerSettings,
                          QString *error)
{
    const bool explicitKeys = !settings.trackKeys.isEmpty();
    const bool explicitNames = !settings.trackNames.isEmpty();
    const int trackCount = settings.inputSource == RecordingInputSource::Array
        ? settings.channels.size() : settings.noiseReductionStages.size();
    if ((explicitKeys && settings.trackKeys.size() != trackCount)
        || (explicitNames && settings.trackNames.size() != trackCount)) {
        *error = QStringLiteral("Recording track metadata count must match selected track count");
        return false;
    }

    QVector<RecordingTrack> tracks;
    tracks.reserve(trackCount);
    if (settings.inputSource == RecordingInputSource::Array) {
        if (settings.channels.isEmpty()) {
            *error = QStringLiteral("At least one recording channel must be selected");
            return false;
        }
        if (!settings.noiseReductionStages.isEmpty()) {
            *error = QStringLiteral("Array recording cannot select noise-reduction stages");
            return false;
        }
        QVector<int> seenChannels;
        for (int index = 0; index < settings.channels.size(); ++index) {
            const int channel = settings.channels.at(index);
            if (channel < 0 || channel >= AudioFrameDecoder::ChannelCount
                || seenChannels.contains(channel)) {
                *error = QStringLiteral("Array recording channels must be unique values from 0 to 47");
                return false;
            }
            seenChannels.append(channel);
            tracks.append({
                channel,
                NoiseReductionDataFlowStage::Suppression,
                explicitKeys
                    ? settings.trackKeys.at(index).trimmed()
                    : QStringLiteral("ch%1").arg(channel, 2, 10, QLatin1Char('0')),
                explicitNames
                    ? settings.trackNames.at(index).trimmed()
                    : QStringLiteral("CH%1").arg(channel),
            });
        }
        std::sort(tracks.begin(), tracks.end(), [](const RecordingTrack &left,
                                                   const RecordingTrack &right) {
            return left.channel < right.channel;
        });
    } else if (settings.inputSource == RecordingInputSource::NoiseReductionStages) {
        if (!settings.channels.isEmpty()) {
            *error = QStringLiteral("Noise-reduction recording cannot select array channels");
            return false;
        }
        if (settings.noiseReductionStages.isEmpty()) {
            *error = QStringLiteral("At least one noise-reduction stage must be selected");
            return false;
        }
        QVector<NoiseReductionDataFlowStage> seenStages;
        for (int index = 0; index < settings.noiseReductionStages.size(); ++index) {
            const NoiseReductionDataFlowStage stage = settings.noiseReductionStages.at(index);
            if (!isKnownNoiseReductionStage(stage) || seenStages.contains(stage)) {
                *error = QStringLiteral("Noise-reduction recording stages must be known and unique");
                return false;
            }
            seenStages.append(stage);
            tracks.append({
                index,
                stage,
                explicitKeys
                    ? settings.trackKeys.at(index).trimmed()
                    : noiseReductionDataFlowStageKey(stage),
                explicitNames
                    ? settings.trackNames.at(index).trimmed()
                    : noiseReductionDataFlowStageName(stage),
            });
        }
    } else {
        *error = QStringLiteral("Unknown recording input source");
        return false;
    }

    QVector<QString> seenKeys;
    for (const RecordingTrack &track : tracks) {
        if (track.key.isEmpty() || track.name.isEmpty() || seenKeys.contains(track.key)) {
            *error = QStringLiteral("Recording track keys must be unique and track names must not be empty");
            return false;
        }
        seenKeys.append(track.key);
    }

    writerSettings->clear();
    if (settings.fileLayout == RecordingFileLayout::SingleMultichannel) {
        RecordingSettings combined = settings;
        combined.channels.clear();
        combined.noiseReductionStages.clear();
        combined.trackKeys.clear();
        combined.trackNames.clear();
        for (const RecordingTrack &track : tracks) {
            combined.channels.append(track.channel);
            combined.trackKeys.append(track.key);
            combined.trackNames.append(track.name);
            if (settings.inputSource == RecordingInputSource::NoiseReductionStages) {
                combined.noiseReductionStages.append(track.stage);
            }
        }
        writerSettings->append(std::move(combined));
        return true;
    }
    if (settings.fileLayout != RecordingFileLayout::SeparateTracks) {
        *error = QStringLiteral("Unknown recording file layout");
        return false;
    }

    writerSettings->reserve(tracks.size());
    for (const RecordingTrack &track : tracks) {
        RecordingSettings separate = settings;
        separate.baseName += QStringLiteral("_%1").arg(track.key);
        separate.channels = {track.channel};
        separate.noiseReductionStages.clear();
        if (settings.inputSource == RecordingInputSource::NoiseReductionStages) {
            separate.noiseReductionStages.append(track.stage);
        }
        separate.trackKeys = {track.key};
        separate.trackNames = {track.name};
        writerSettings->append(std::move(separate));
    }
    return true;
}

bool packNoiseReductionFrame(const NoiseReductionRecordingFrame &frame,
                             const QVector<NoiseReductionDataFlowStage> &selectedStages,
                             QByteArray *payload,
                             QString *error)
{
    payload->fill(char(0), AudioFrameDecoder::FramePayloadBytes);
    for (int channel = 0; channel < selectedStages.size(); ++channel) {
        const NoiseReductionDataFlowStage selectedStage = selectedStages.at(channel);
        const NoiseReductionStageSamples *selected = nullptr;
        for (const NoiseReductionStageSamples &candidate : frame.stages) {
            if (candidate.stage == selectedStage) {
                if (selected != nullptr) {
                    *error = QStringLiteral("Selected noise-reduction stage occurs more than once");
                    return false;
                }
                selected = &candidate;
            }
        }
        if (selected == nullptr
            || selected->samples.size() != AudioFrameDecoder::SamplesPerFrame) {
            *error = QStringLiteral("Selected noise-reduction stage is missing or is not exactly 200 samples");
            return false;
        }
        for (int samplePoint = 0; samplePoint < AudioFrameDecoder::SamplesPerFrame; ++samplePoint) {
            const double sample = std::clamp(
                static_cast<double>(selected->samples.at(samplePoint)),
                -1.0,
                32767.0 / 32768.0);
            const qint64 rounded = qRound64(sample * 32768.0);
            const qint32 packed = static_cast<qint32>(rounded * 256);
            const int offset = samplePoint * AudioFrameDecoder::BytesPerSamplePoint
                + channel * AudioFrameDecoder::BytesPerChannel;
            qToLittleEndian(static_cast<quint32>(packed),
                            reinterpret_cast<uchar *>(payload->data() + offset));
        }
    }
    return true;
}

} // namespace

class AudioRecorder::Worker : public QObject
{
public:
    explicit Worker(AudioRecorder *owner,
                    MultiChannelWavWriter::FileFactory writerFileFactory)
        : m_owner(owner)
        , m_writerFileFactory(std::move(writerFileFactory))
    {
    }

    void start(const RecordingSettings &settings)
    {
        m_terminalFailure = false;
        m_terminalError.clear();
        m_terminalSummary = {};
        m_settings = settings;
        QVector<RecordingSettings> settingsPerWriter;
        QString error;
        bool success = createWriterSettings(settings, &settingsPerWriter, &error);
        m_writers = QVector<std::unique_ptr<MultiChannelWavWriter>>(settingsPerWriter.size());
        for (int index = 0; index < settingsPerWriter.size(); ++index) {
            // Qt's copy-on-write QVector cannot grow a move-only element, so populate
            // pre-sized, uniquely owned slots through their const access path.
            auto &slot = const_cast<std::unique_ptr<MultiChannelWavWriter> &>(
                std::as_const(m_writers).at(index));
            slot = std::make_unique<MultiChannelWavWriter>(
                MultiChannelWavWriter::DefaultSegmentDataLimit,
                m_writerFileFactory);
        }
        for (int index = 0; index < settingsPerWriter.size(); ++index) {
            if (!success) {
                break;
            }
            success = std::as_const(m_writers).at(index)->open(
                settingsPerWriter.at(index), &error);
        }
        if (!success) {
            QString closeError;
            closeAll(QStringLiteral("start-error"), &closeError);
            if (!closeError.isEmpty() && !error.contains(closeError)) {
                error += QStringLiteral("; %1").arg(closeError);
            }
            latchTerminalFailure(error, aggregateSummary());
        }
        m_acceptingFrames = success;
        const RecordingSummary summary = aggregateSummary();
        post([success, error, summary](AudioRecorder *owner) {
            owner->handleStarted(success, error, summary);
        });
    }

    void append(const QByteArray &payload,
                const std::optional<GainConfiguration> &gainConfiguration)
    {
        const quint64 inputBytes = static_cast<quint64>(payload.size());
        appendPayload(payload, gainConfiguration, inputBytes);
    }

    void appendNoise(const NoiseReductionRecordingFrame &frame, quint64 inputBytes)
    {
        if (!m_acceptingFrames || m_writers.isEmpty()) {
            post([inputBytes](AudioRecorder *owner) {
                owner->handleFrameDiscarded(inputBytes);
            });
            return;
        }

        QByteArray payload;
        QString error;
        if (!packNoiseReductionFrame(frame, m_settings.noiseReductionStages, &payload, &error)) {
            failFrame(inputBytes, QStringLiteral("format-error"), error);
            return;
        }
        appendPayload(payload, std::nullopt, inputBytes);
    }

    StopOutcome stop(const QString &reason)
    {
        StopOutcome outcome;
        outcome.reason = reason;
        m_acceptingFrames = false;
        if (m_terminalFailure) {
            outcome.success = false;
            outcome.error = m_terminalError;
            outcome.summary = m_terminalSummary;
            return outcome;
        }
        outcome.success = closeAll(reason, &outcome.error);
        outcome.summary = aggregateSummary();
        if (!outcome.success) {
            latchTerminalFailure(outcome.error, outcome.summary);
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
    RecordingSummary aggregateSummary() const
    {
        RecordingSummary aggregate;
        bool first = true;
        for (const auto &writer : std::as_const(m_writers)) {
            const RecordingSummary current = writer->summary();
            if (first) {
                aggregate.manifestPath = current.manifestPath;
                aggregate.inputFrames = current.inputFrames;
                aggregate.sampleFrames = current.sampleFrames;
                aggregate.stopReason = current.stopReason;
                first = false;
            }
            aggregate.segmentPaths += current.segmentPaths;
            aggregate.audioFileCount += current.audioFileCount;
            aggregate.dataBytes += current.dataBytes;
            aggregate.fileBytes += current.fileBytes;
        }
        return aggregate;
    }

    bool closeAll(const QString &reason, QString *error)
    {
        bool success = true;
        for (const auto &writer : std::as_const(m_writers)) {
            if (!writer->isOpen()) {
                continue;
            }
            QString writerError;
            if (!writer->close(reason, &writerError)) {
                success = false;
                if (!writerError.isEmpty()
                    && (error->isEmpty() || !error->contains(writerError))) {
                    if (!error->isEmpty()) {
                        *error += QStringLiteral("; ");
                    }
                    *error += writerError;
                }
            }
        }
        return success;
    }

    void failFrame(quint64 inputBytes, const QString &reason, QString error)
    {
        m_acceptingFrames = false;
        QString closeError;
        closeAll(reason, &closeError);
        if (!closeError.isEmpty() && !error.contains(closeError)) {
            error += QStringLiteral("; %1").arg(closeError);
        }
        const RecordingSummary summary = aggregateSummary();
        latchTerminalFailure(error, summary);
        post([inputBytes, error, summary](AudioRecorder *owner) {
            owner->handleFrameFailed(inputBytes, error, summary);
        });
    }

    void latchTerminalFailure(const QString &error,
                              const RecordingSummary &summary)
    {
        if (m_terminalFailure) {
            return;
        }
        m_terminalFailure = true;
        m_terminalError = error;
        m_terminalSummary = summary;
    }

    void appendPayload(const QByteArray &payload,
                       const std::optional<GainConfiguration> &gainConfiguration,
                       quint64 inputBytes)
    {
        if (!m_acceptingFrames || m_writers.isEmpty()) {
            post([inputBytes](AudioRecorder *owner) {
                owner->handleFrameDiscarded(inputBytes);
            });
            return;
        }

        QString error;
        for (const auto &writer : std::as_const(m_writers)) {
            if (!writer->appendFrame(payload, &error)) {
                failFrame(inputBytes, QStringLiteral("io-error"), error);
                return;
            }
        }
        if (gainConfiguration.has_value()) {
            for (const auto &writer : std::as_const(m_writers)) {
                if (!writer->updateFinalGainConfiguration(*gainConfiguration, &error)) {
                    failFrame(inputBytes, QStringLiteral("gain-metadata-error"), error);
                    return;
                }
            }
        }

        const RecordingSummary summary = aggregateSummary();
        post([inputBytes, summary](AudioRecorder *owner) {
            owner->handleFrameConsumed(inputBytes, summary);
        });
    }

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
    QVector<std::unique_ptr<MultiChannelWavWriter>> m_writers;
    MultiChannelWavWriter::FileFactory m_writerFileFactory;
    RecordingSettings m_settings;
    bool m_acceptingFrames = false;
    bool m_terminalFailure = false;
    QString m_terminalError;
    RecordingSummary m_terminalSummary;
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
    : AudioRecorder(MaxPendingBytes, MaxPendingFrames, {}, parent)
{
}

AudioRecorder::AudioRecorder(quint64 maxPendingBytes, QObject *parent)
    : AudioRecorder(maxPendingBytes, MaxPendingFrames, {}, parent)
{
}

AudioRecorder::AudioRecorder(quint64 maxPendingBytes,
                             quint64 maxPendingFrames,
                             QObject *parent)
    : AudioRecorder(maxPendingBytes, maxPendingFrames, {}, parent)
{
}

AudioRecorder::AudioRecorder(quint64 maxPendingBytes,
                             quint64 maxPendingFrames,
                             MultiChannelWavWriter::FileFactory writerFileFactory,
                             QObject *parent)
    : QObject(parent)
    , m_maxPendingBytes(std::max<quint64>(1, maxPendingBytes))
    , m_maxPendingFrames(std::max<quint64>(1, maxPendingFrames))
{
    qRegisterMetaType<RecordingState>("MicArray::RecordingState");
    qRegisterMetaType<RecordingStatistics>("MicArray::RecordingStatistics");
    m_worker = new Worker(this, std::move(writerFileFactory));
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
    QString selectionError;
    if (settings.inputSource == RecordingInputSource::Array
        && settings.channels.isEmpty()) {
        selectionError = QStringLiteral("At least one recording channel must be selected");
    } else if (settings.inputSource == RecordingInputSource::Array
               && !settings.noiseReductionStages.isEmpty()) {
        selectionError = QStringLiteral("Array recording cannot select noise-reduction stages");
    } else if (settings.inputSource == RecordingInputSource::NoiseReductionStages
               && settings.noiseReductionStages.isEmpty()) {
        selectionError = QStringLiteral("At least one noise-reduction stage must be selected");
    } else if (settings.inputSource == RecordingInputSource::NoiseReductionStages
               && !settings.channels.isEmpty()) {
        selectionError = QStringLiteral("Noise-reduction recording cannot select array channels");
    }
    if (!selectionError.isEmpty()) {
        m_statistics = {};
        m_statistics.error = selectionError;
        setState(RecordingState::Error);
        emit recordingError(m_statistics.error);
        emit statisticsChanged(m_statistics);
        return false;
    }

    m_pendingBytes = 0;
    m_pendingFrames = 0;
    m_statistics = {};
    m_recordingSettings = settings;
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
    return enqueueFrameInternal(payload, nullptr);
}

bool AudioRecorder::enqueueFrame(const QByteArray &payload,
                                 const GainConfiguration &gainConfiguration)
{
    return enqueueFrameInternal(payload, &gainConfiguration);
}

bool AudioRecorder::enqueueFrameInternal(const QByteArray &payload,
                                         const GainConfiguration *gainConfiguration)
{
    if (m_state != RecordingState::Recording) {
        return false;
    }
    if (m_recordingSettings.inputSource != RecordingInputSource::Array) {
        return rejectFrame(QStringLiteral("Array payload cannot be queued for noise-reduction recording"));
    }
    if (payload.size() != AudioFrameDecoder::FramePayloadBytes) {
        return rejectFrame(QStringLiteral("Expected 38400-byte audio payload, got %1 bytes")
                               .arg(payload.size()));
    }
    if (gainConfiguration != nullptr) {
        QString validationError;
        if (!GainConfiguration::validate(*gainConfiguration, &validationError)) {
            m_statistics.error = QStringLiteral("Invalid recording gain configuration: %1")
                                     .arg(validationError);
            emit recordingError(m_statistics.error);
            emit statisticsChanged(m_statistics);
            return false;
        }
    }
    const quint64 payloadBytes = static_cast<quint64>(payload.size());
    if (!reservePendingFrame(payloadBytes)) {
        return false;
    }
    const std::optional<GainConfiguration> queuedGain = gainConfiguration != nullptr
        ? std::optional<GainConfiguration>(*gainConfiguration)
        : std::nullopt;
    QMetaObject::invokeMethod(m_worker,
                              [worker = m_worker, payload, queuedGain]() {
                                  worker->append(payload, queuedGain);
                              },
                              Qt::QueuedConnection);
    return true;
}

bool AudioRecorder::enqueueNoiseReductionFrame(const NoiseReductionRecordingFrame &frame)
{
    if (m_state != RecordingState::Recording) {
        return false;
    }
    if (m_recordingSettings.inputSource != RecordingInputSource::NoiseReductionStages) {
        return rejectFrame(QStringLiteral("Noise-reduction frame cannot be queued for array recording"));
    }
    if (frame.sampleRate != m_recordingSettings.sampleRate) {
        return rejectFrame(QStringLiteral("Expected %1 Hz noise-reduction frame, got %2 Hz")
                               .arg(m_recordingSettings.sampleRate)
                               .arg(frame.sampleRate));
    }
    NoiseReductionRecordingFrame compactFrame;
    compactFrame.sampleRate = frame.sampleRate;
    compactFrame.timestampSeconds = frame.timestampSeconds;
    compactFrame.timestampMicros = frame.timestampMicros;
    compactFrame.stages.reserve(m_recordingSettings.noiseReductionStages.size());
    for (NoiseReductionDataFlowStage selectedStage
         : m_recordingSettings.noiseReductionStages) {
        int occurrences = 0;
        const NoiseReductionStageSamples *stageSamples = nullptr;
        for (const NoiseReductionStageSamples &candidate : frame.stages) {
            if (candidate.stage == selectedStage) {
                ++occurrences;
                stageSamples = &candidate;
            }
        }
        if (occurrences != 1) {
            return rejectFrame(QStringLiteral("Each selected noise-reduction stage must occur exactly once"));
        }
        if (stageSamples->samples.size() != AudioFrameDecoder::SamplesPerFrame) {
            return rejectFrame(QStringLiteral("Each selected noise-reduction stage must contain exactly 200 samples"));
        }
        for (float sample : stageSamples->samples) {
            if (!std::isfinite(sample)) {
                return rejectFrame(QStringLiteral("Noise-reduction recording samples must be finite"));
            }
        }
        compactFrame.stages.append({selectedStage, stageSamples->samples});
    }

    const quint64 inputBytes = static_cast<quint64>(m_recordingSettings.noiseReductionStages.size())
        * static_cast<quint64>(AudioFrameDecoder::SamplesPerFrame) * 4ULL;
    if (!reservePendingFrame(inputBytes)) {
        return false;
    }
    QMetaObject::invokeMethod(m_worker,
                              [worker = m_worker,
                               compactFrame = std::move(compactFrame),
                               inputBytes]() {
                                  worker->appendNoise(compactFrame, inputBytes);
                              },
                              Qt::QueuedConnection);
    return true;
}

bool AudioRecorder::reservePendingFrame(quint64 inputBytes)
{
    const bool byteLimitExceeded = inputBytes > m_maxPendingBytes
        - std::min(m_pendingBytes, m_maxPendingBytes);
    const bool frameLimitExceeded = m_pendingFrames >= m_maxPendingFrames;
    if (byteLimitExceeded || frameLimitExceeded) {
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

    m_pendingBytes += inputBytes;
    ++m_pendingFrames;
    m_statistics.pendingBytes = m_pendingBytes;
    m_statistics.pendingFrames = m_pendingFrames;
    return true;
}

bool AudioRecorder::rejectFrame(const QString &error)
{
    m_statistics.error = error;
    ++m_statistics.discardedFrames;
    emit recordingError(error);
    emit statisticsChanged(m_statistics);
    return false;
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
    m_pendingFrames = 0;
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
    releasePendingFrame(inputBytes);
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
    releasePendingFrame(inputBytes);
    m_terminalFailure = true;
    m_statistics.error = error;
    updateSummary(summary, true);
    setState(RecordingState::Error);
    emit recordingError(error);
    emit statisticsChanged(m_statistics);
}

void AudioRecorder::handleFrameDiscarded(quint64 inputBytes)
{
    releasePendingFrame(inputBytes);
    ++m_statistics.discardedFrames;
    m_statistics.pendingBytes = m_pendingBytes;
    m_statistics.pendingFrames = m_pendingFrames;
    emit statisticsChanged(m_statistics);
}

void AudioRecorder::handleStopped(bool success,
                                  const QString &reason,
                                  const QString &error,
                                  const RecordingSummary &summary)
{
    m_pendingBytes = 0;
    m_pendingFrames = 0;
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
    m_statistics.audioFileCount = summary.audioFileCount;
    m_statistics.pendingBytes = m_pendingBytes;
    m_statistics.pendingFrames = m_pendingFrames;
    if (forceEmit) {
        emit statisticsChanged(m_statistics);
    }
}

void AudioRecorder::releasePendingFrame(quint64 inputBytes)
{
    m_pendingBytes = inputBytes >= m_pendingBytes ? 0 : m_pendingBytes - inputBytes;
    if (m_pendingFrames > 0) {
        --m_pendingFrames;
    }
    m_statistics.pendingBytes = m_pendingBytes;
    m_statistics.pendingFrames = m_pendingFrames;
}

} // namespace MicArray
