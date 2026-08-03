#pragma once

#include "media/audiomonitorbuffer.h"
#include "media/fractionaldelaybeamformer.h"
#include "media/noisereductionframedecoder.h"
#include "media/noisereductiontypes.h"
#include "media/automaticgaincontrol.h"
#include "media/bandpassfilter.h"
#include "media/logmmseprocessor.h"
#include "media/wienernoisereducer.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QObject>
#include <QSet>

#include <array>
#include <deque>
#include <memory>

namespace MicArray {

class NoiseReductionWorker : public QObject
{
    Q_OBJECT

public:
    explicit NoiseReductionWorker(QObject *parent = nullptr);

    static QVector<float> powerSpectrumDbfs(const QVector<float> &samples,
                                            int sampleRate);
    static QByteArray pcm16WithGain(const QVector<float> &samples,
                                    double gainDb);

public slots:
    void configure(const NoiseReductionSettings &settings, int sampleRate);
    void setActive(bool active);
    void setRecordingStages(
        const QVector<NoiseReductionDataFlowStage> &stages);
    void processRawFrame(const QByteArray &payload,
                         quint32 timestampSeconds,
                         quint32 timestampMicros);
    void shutdown();

signals:
    void frameConsumed();
    void plotFrameReady(const NoiseReductionPlotFrame &frame);
    void recordingFrameReady(const NoiseReductionRecordingFrame &frame);
    void warningRaised(const QString &message);
    void statusChanged(const QString &message);

private:
    struct SampleTimestampSegment {
        qint64 originTimestampMicroseconds = 0;
        qint64 sampleOffset = 0;
        qsizetype sampleCount = 0;
    };

    static constexpr int PlotFftSize = 2048;
    static constexpr int MaximumWaveformSamples = 4096;
    static constexpr qint64 PlotIntervalMicroseconds = 50000;

    static int sourceIndex(NoiseSignalSource source);
    static const QVector<float> &sourceSamples(
        NoiseSignalSource source,
        const NoiseReductionInputFrame &input,
        const QVector<float> &denoised);
    void appendHistory(NoiseSignalSource source, const QVector<float> &samples);
    NoiseReductionPlotSeries plotSeries(NoiseSignalSource source) const;
    void writeMonitorSamples(const NoiseReductionInputFrame &input,
                             const QVector<float> &denoised);
    void ensureAudioOutput();
    void closeAudioOutput();
    void resetProcessing();
    QVector<float> applyPostProcessing(
        QVector<float> samples,
        std::array<QVector<float>, 4> *capturedStages);
    void publishRecordingFrame(
        const std::array<QVector<float>, 4> &capturedStages,
        const std::deque<SampleTimestampSegment> &capturedTimeline);
    void appendInputTimeline(
        quint32 timestampSeconds,
        quint32 timestampMicros,
        qsizetype sampleCount);
    std::deque<SampleTimestampSegment> takeInputTimeline(
        qsizetype sampleCount);
    static qsizetype timelineSampleCount(
        const std::deque<SampleTimestampSegment> &timeline);
    static void discardTimelineSamples(
        std::deque<SampleTimestampSegment> *timeline,
        qsizetype sampleCount);
    qint64 segmentTimestampMicroseconds(
        const SampleTimestampSegment &segment) const;
    void clearRecordingBuffers();

    NoiseReductionSettings m_settings;
    int m_sampleRate = 48000;
    bool m_active = false;
    bool m_shutdown = false;
    qint64 m_lastPlotTimestamp = -1;
    NoiseReductionFrameDecoder m_decoder;
    FractionalDelayBeamformer m_beamformer;
    WienerNoiseReducer m_reducer;
    BandPassFilter m_bandPassFilter;
    LogMmseProcessor m_logMmseProcessor;
    AutomaticGainControl m_automaticGainControl;
    std::array<QVector<float>, 5> m_history;
    QSet<NoiseReductionDataFlowStage> m_recordingStageSet;
    QVector<NoiseReductionDataFlowStage> m_recordingStages;
    std::array<QVector<float>, 4> m_recordingBuffers;
    std::deque<SampleTimestampSegment> m_inputTimeline;
    std::deque<SampleTimestampSegment> m_recordingTimeline;
    AudioMonitorBuffer m_monitorBuffer;
    QAudioFormat m_audioFormat;
    std::unique_ptr<QAudioSink> m_audioSink;
    QIODevice *m_audioOutput = nullptr;
    bool m_audioOutputAttempted = false;
    bool m_monitorOverflowWarningActive = false;
    bool m_raw48InvalidWarningActive = false;
};

} // namespace MicArray
