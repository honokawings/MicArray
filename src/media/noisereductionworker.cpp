#include "media/noisereductionworker.h"

#include "analysis/radix2fft.h"
#include "media/audioframedecoder.h"

#include <QMediaDevices>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace MicArray {
namespace {

bool isValidRecordingStage(NoiseReductionDataFlowStage stage)
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

int recordingStageIndex(NoiseReductionDataFlowStage stage)
{
    return static_cast<int>(stage);
}

NoiseReductionDataFlowStage dataFlowStage(NoisePostProcessingNode node)
{
    switch (node) {
    case NoisePostProcessingNode::BandPass:
        return NoiseReductionDataFlowStage::BandPass;
    case NoisePostProcessingNode::LogMmse:
        return NoiseReductionDataFlowStage::LogMmse;
    case NoisePostProcessingNode::AutomaticGainControl:
        return NoiseReductionDataFlowStage::AutomaticGainControl;
    }
    return NoiseReductionDataFlowStage::Suppression;
}

} // namespace

NoiseReductionWorker::NoiseReductionWorker(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<NoiseReductionSettings>("MicArray::NoiseReductionSettings");
    qRegisterMetaType<NoiseReductionPlotFrame>(
        "MicArray::NoiseReductionPlotFrame");
    qRegisterMetaType<NoiseReductionRecordingFrame>(
        "MicArray::NoiseReductionRecordingFrame");
    qRegisterMetaType<QVector<NoiseReductionDataFlowStage>>(
        "QVector<MicArray::NoiseReductionDataFlowStage>");
    m_monitorBuffer.configure(
        m_sampleRate, m_settings.monitorBufferMilliseconds);
}

QVector<float> NoiseReductionWorker::powerSpectrumDbfs(
    const QVector<float> &samples, int sampleRate)
{
    Q_UNUSED(sampleRate)
    QVector<std::complex<double>> spectrum(PlotFftSize);
    const int available =
        std::min(static_cast<int>(samples.size()), PlotFftSize);
    const int inputStart = samples.size() - available;
    const int destinationStart = PlotFftSize - available;
    double windowSum = 0.0;
    for (int index = 0; index < PlotFftSize; ++index) {
        const double window = 0.5
            - 0.5 * std::cos(2.0 * std::numbers::pi * index / PlotFftSize);
        windowSum += window;
        const int inputIndex = index - destinationStart;
        const double sample = inputIndex >= 0
            ? samples.at(inputStart + inputIndex) : 0.0;
        spectrum[index] = {sample * window, 0.0};
    }
    if (!Radix2Fft::transform(&spectrum, false)) {
        return {};
    }

    QVector<float> result(PlotFftSize / 2 + 1);
    const double normalization = std::max(1.0, windowSum * 0.5);
    for (int bin = 0; bin < result.size(); ++bin) {
        const double amplitude = std::abs(spectrum.at(bin)) / normalization;
        const double dbfs = 20.0 * std::log10(std::max(amplitude, 1.0e-6));
        result[bin] =
            static_cast<float>(std::clamp(dbfs, -120.0, 0.0));
    }
    return result;
}

QByteArray NoiseReductionWorker::pcm16WithGain(
    const QVector<float> &samples, double gainDb)
{
    const double clampedGainDb = std::clamp(gainDb, -60.0, 48.0);
    const double gain = std::pow(10.0, clampedGainDb / 20.0);
    QByteArray pcm;
    pcm.resize(samples.size() * 2);
    for (int index = 0; index < samples.size(); ++index) {
        const double value =
            std::clamp(static_cast<double>(samples.at(index)) * gain,
                       -1.0,
                       32767.0 / 32768.0);
        const qint16 converted =
            static_cast<qint16>(std::lround(value * 32768.0));
        qToLittleEndian(converted,
                        reinterpret_cast<uchar *>(pcm.data() + index * 2));
    }
    return pcm;
}

void NoiseReductionWorker::configure(
    const NoiseReductionSettings &settings, int sampleRate)
{
    const int sanitizedSampleRate =
        std::clamp(sampleRate, 8000, 192000);
    const int sanitizedMonitorBufferMilliseconds =
        std::clamp(settings.monitorBufferMilliseconds, 50, 1000);
    const bool sampleRateChanged =
        m_sampleRate != sanitizedSampleRate;
    const bool inputModeChanged =
        m_settings.inputMode != settings.inputMode;
    const bool algorithmChanged =
        m_settings.algorithm != settings.algorithm;
    const bool modeChanged = m_settings.mode != settings.mode;
    const bool coefficientChanged =
        m_settings.singleSuppressionCoefficient
            != settings.singleSuppressionCoefficient
        || m_settings.dualB1SuppressionCoefficient
            != settings.dualB1SuppressionCoefficient
        || m_settings.dualB2SuppressionCoefficient
            != settings.dualB2SuppressionCoefficient;
    const bool bandPassChanged =
        m_settings.bandPass != settings.bandPass;
    const bool logMmseChanged =
        m_settings.logMmse != settings.logMmse;
    const bool automaticGainControlChanged =
        m_settings.automaticGainControl
            != settings.automaticGainControl;
    const bool postProcessingOrderChanged =
        m_settings.postProcessingOrder
            != settings.postProcessingOrder;
    const bool enabledChanged = m_settings.enabled != settings.enabled;
    const bool monitorEnabledChanged =
        m_settings.monitorEnabled != settings.monitorEnabled;
    const bool monitorChanged =
        m_settings.monitorSource != settings.monitorSource;
    const bool monitorGainChanged =
        m_settings.monitorGainDb != settings.monitorGainDb;
    const bool monitorBufferChanged =
        m_settings.monitorBufferMilliseconds
            != sanitizedMonitorBufferMilliseconds;
    m_settings = settings;
    m_settings.postProcessingOrder =
        normalizedPostProcessingOrder(
            m_settings.postProcessingOrder);
    m_settings.monitorGainDb =
        std::clamp(m_settings.monitorGainDb, -60.0, 48.0);
    m_settings.monitorBufferMilliseconds =
        sanitizedMonitorBufferMilliseconds;
    m_sampleRate = sanitizedSampleRate;
    m_beamformer.configure(m_sampleRate,
                           m_settings.enhancedA,
                           m_settings.suppressionB1,
                           m_settings.suppressionB2);
    if (sampleRateChanged || inputModeChanged || enabledChanged) {
        m_beamformer.reset();
        m_raw48InvalidWarningActive = false;
    }
    if (algorithmChanged) {
        m_reducer.setAlgorithm(m_settings.algorithm);
    }
    if (modeChanged) {
        m_reducer.setMode(m_settings.mode);
    }
    if (coefficientChanged) {
        m_reducer.setSuppressionCoefficients(
            m_settings.singleSuppressionCoefficient,
            m_settings.dualB1SuppressionCoefficient,
            m_settings.dualB2SuppressionCoefficient);
    }
    if (m_settings.bandPass.enabled
        && (sampleRateChanged || bandPassChanged)) {
        const double maximumCutoff = m_sampleRate * 0.5 * 0.99;
        const double highCutoff =
            std::min(m_settings.bandPass.highCutoffHz, maximumCutoff);
        if (!m_bandPassFilter.configure(
                m_sampleRate,
                m_settings.bandPass.lowCutoffHz,
                highCutoff)) {
            m_settings.bandPass.enabled = false;
            emit warningRaised(
                QStringLiteral("带通滤波参数无效，已自动关闭"));
        }
    } else if (!m_settings.bandPass.enabled && bandPassChanged) {
        m_bandPassFilter.reset();
    }
    if (m_settings.logMmse.enabled
        && (sampleRateChanged || logMmseChanged
            || postProcessingOrderChanged)) {
        if (!m_logMmseProcessor.configure(
                m_sampleRate, m_settings.logMmse)) {
            m_settings.logMmse.enabled = false;
            emit warningRaised(
                QStringLiteral("LogMMSE 参数无效，已自动旁路"));
        }
    } else if (!m_settings.logMmse.enabled && logMmseChanged) {
        m_logMmseProcessor.reset();
    }
    if (m_settings.automaticGainControl.enabled
        && (sampleRateChanged || automaticGainControlChanged
            || postProcessingOrderChanged)) {
        if (!m_automaticGainControl.configure(
                m_sampleRate,
                m_settings.automaticGainControl)) {
            m_settings.automaticGainControl.enabled = false;
            emit warningRaised(
                QStringLiteral("AGC 参数无效，已自动旁路"));
        }
    } else if (!m_settings.automaticGainControl.enabled
               && automaticGainControlChanged) {
        m_automaticGainControl.reset();
    }
    const bool processingChanged =
        sampleRateChanged || inputModeChanged || enabledChanged
        || algorithmChanged
        || modeChanged || coefficientChanged || bandPassChanged
        || logMmseChanged || automaticGainControlChanged
        || postProcessingOrderChanged;
    if (processingChanged) {
        resetProcessing();
    }
    if (processingChanged || monitorChanged || monitorEnabledChanged
        || monitorGainChanged || monitorBufferChanged) {
        closeAudioOutput();
        m_monitorBuffer.configure(
            m_sampleRate, m_settings.monitorBufferMilliseconds);
    }
    emit statusChanged(m_settings.enabled
                           ? QStringLiteral("降噪处理已启用")
                           : QStringLiteral("降噪处理已关闭"));
}

void NoiseReductionWorker::setRecordingStages(
    const QVector<NoiseReductionDataFlowStage> &stages)
{
    QVector<NoiseReductionDataFlowStage> normalized;
    QSet<NoiseReductionDataFlowStage> selected;
    normalized.reserve(stages.size());
    for (NoiseReductionDataFlowStage stage : stages) {
        if (!isValidRecordingStage(stage) || selected.contains(stage)) {
            continue;
        }
        selected.insert(stage);
        normalized.append(stage);
    }
    if (m_recordingStages == normalized) {
        return;
    }
    m_recordingStages = std::move(normalized);
    m_recordingStageSet = std::move(selected);
    clearRecordingBuffers();
}

void NoiseReductionWorker::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (!m_active) {
        closeAudioOutput();
        m_beamformer.reset();
        m_raw48InvalidWarningActive = false;
        resetProcessing();
    }
}

void NoiseReductionWorker::processRawFrame(
    const QByteArray &payload,
    quint32 timestampSeconds,
    quint32 timestampMicros)
{
    if (m_shutdown || !m_active || !m_settings.enabled) {
        emit frameConsumed();
        return;
    }

    const bool raw48Input = m_settings.inputMode
        == NoiseReductionInputMode::Raw48SoftwareBeamforming;
    NoiseReductionInputFrame input;
    if (raw48Input) {
        BeamformedDirectionFrame beamformed = m_beamformer.process(payload);
        input.valid = beamformed.valid;
        input.error = std::move(beamformed.error);
        input.channels[0] = std::move(beamformed.firstElement);
        input.channels[1] = std::move(beamformed.enhancedA);
        input.channels[2] = std::move(beamformed.suppressionB1);
        input.channels[3] = std::move(beamformed.suppressionB2);
    } else {
        input = m_decoder.decode(payload);
    }
    if (!input.valid) {
        if (!raw48Input || !m_raw48InvalidWarningActive) {
            emit warningRaised(input.error);
        }
        if (raw48Input) {
            m_raw48InvalidWarningActive = true;
        }
        emit frameConsumed();
        return;
    }
    if (raw48Input) {
        m_raw48InvalidWarningActive = false;
    }
    appendInputTimeline(
        timestampSeconds,
        timestampMicros,
        input.channels.at(1).size());

    appendHistory(NoiseSignalSource::FirstElement, input.channels.at(0));
    appendHistory(NoiseSignalSource::EnhancedA, input.channels.at(1));
    appendHistory(NoiseSignalSource::SuppressionB1, input.channels.at(2));
    appendHistory(NoiseSignalSource::SuppressionB2, input.channels.at(3));
    QVector<float> denoised =
        m_reducer.process(input.channels.at(1),
                          input.channels.at(2),
                          input.channels.at(3));
    const std::deque<SampleTimestampSegment> capturedTimeline =
        takeInputTimeline(denoised.size());
    std::array<QVector<float>, 4> capturedStages;
    if (m_recordingStageSet.contains(
            NoiseReductionDataFlowStage::Suppression)) {
        capturedStages.at(static_cast<size_t>(recordingStageIndex(
            NoiseReductionDataFlowStage::Suppression))) = denoised;
    }
    denoised = applyPostProcessing(
        std::move(denoised), &capturedStages);
    publishRecordingFrame(capturedStages, capturedTimeline);
    appendHistory(NoiseSignalSource::Denoised, denoised);
    writeMonitorSamples(input, denoised);

    const qint64 timestamp =
        static_cast<qint64>(timestampSeconds) * 1000000
        + static_cast<qint64>(timestampMicros);
    if (!denoised.isEmpty()
        && (m_lastPlotTimestamp < 0
            || timestamp - m_lastPlotTimestamp >= PlotIntervalMicroseconds)) {
        NoiseReductionPlotFrame frame;
        frame.first = plotSeries(m_settings.firstPlotSource);
        frame.second = plotSeries(m_settings.secondPlotSource);
        frame.sampleRate = m_sampleRate;
        frame.timestampMicroseconds = timestamp;
        m_lastPlotTimestamp = timestamp;
        emit plotFrameReady(frame);
    }
    emit frameConsumed();
}

void NoiseReductionWorker::shutdown()
{
    m_shutdown = true;
    m_active = false;
    closeAudioOutput();
    m_beamformer.reset();
    m_raw48InvalidWarningActive = false;
    resetProcessing();
}

int NoiseReductionWorker::sourceIndex(NoiseSignalSource source)
{
    return static_cast<int>(source);
}

const QVector<float> &NoiseReductionWorker::sourceSamples(
    NoiseSignalSource source,
    const NoiseReductionInputFrame &input,
    const QVector<float> &denoised)
{
    switch (source) {
    case NoiseSignalSource::FirstElement:
        return input.channels.at(0);
    case NoiseSignalSource::EnhancedA:
        return input.channels.at(1);
    case NoiseSignalSource::SuppressionB1:
        return input.channels.at(2);
    case NoiseSignalSource::SuppressionB2:
        return input.channels.at(3);
    case NoiseSignalSource::Denoised:
        return denoised;
    }
    return denoised;
}

void NoiseReductionWorker::appendHistory(
    NoiseSignalSource source, const QVector<float> &samples)
{
    QVector<float> &history =
        m_history.at(static_cast<size_t>(sourceIndex(source)));
    history.reserve(std::min(
        MaximumWaveformSamples,
        static_cast<int>(history.size() + samples.size())));
    history += samples;
    if (history.size() > MaximumWaveformSamples) {
        history.erase(history.begin(),
                      history.begin()
                          + (history.size() - MaximumWaveformSamples));
    }
}

NoiseReductionPlotSeries NoiseReductionWorker::plotSeries(
    NoiseSignalSource source) const
{
    NoiseReductionPlotSeries series;
    series.source = source;
    series.waveform =
        m_history.at(static_cast<size_t>(sourceIndex(source)));
    series.spectrumDbfs = powerSpectrumDbfs(series.waveform, m_sampleRate);
    return series;
}

void NoiseReductionWorker::writeMonitorSamples(
    const NoiseReductionInputFrame &input,
    const QVector<float> &denoised)
{
    if (!m_settings.monitorEnabled) {
        return;
    }
    const QVector<float> &samples =
        sourceSamples(m_settings.monitorSource, input, denoised);
    if (samples.isEmpty()) {
        return;
    }
    const QByteArray pcm = pcm16WithGain(samples, m_settings.monitorGainDb);
    const qsizetype dropped = m_monitorBuffer.append(pcm);
    if (dropped > 0 && !m_monitorOverflowWarningActive) {
        emit warningRaised(
            QStringLiteral("降噪监听缓冲区已满，部分音频未播放"));
        m_monitorOverflowWarningActive = true;
    } else if (dropped == 0) {
        m_monitorOverflowWarningActive = false;
    }

    if (m_audioOutput == nullptr && m_monitorBuffer.readyToStart()) {
        ensureAudioOutput();
    }
    if (m_audioOutput == nullptr || m_audioSink == nullptr) {
        return;
    }

    const qsizetype bytesToWrite = std::min(
        m_monitorBuffer.availableBytes(), m_audioSink->bytesFree());
    if (bytesToWrite <= 0) {
        return;
    }
    const QByteArray queued = m_monitorBuffer.peek(bytesToWrite);
    const qint64 written = m_audioOutput->write(queued);
    if (written > 0) {
        m_monitorBuffer.discard(written);
    }
}

void NoiseReductionWorker::ensureAudioOutput()
{
    if (m_audioOutput != nullptr || m_audioOutputAttempted) {
        return;
    }
    m_audioOutputAttempted = true;
    m_audioFormat.setSampleRate(m_sampleRate);
    m_audioFormat.setChannelCount(1);
    m_audioFormat.setSampleFormat(QAudioFormat::Int16);
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull() || !device.isFormatSupported(m_audioFormat)) {
        emit warningRaised(
            QStringLiteral("默认音频输出设备不支持降噪监听格式"));
        return;
    }
    m_audioSink = std::make_unique<QAudioSink>(device, m_audioFormat);
    m_audioSink->setBufferSize(static_cast<qsizetype>(
        m_audioFormat.bytesForDuration(
            static_cast<qint64>(m_settings.monitorBufferMilliseconds)
            * 1000)));
    m_audioOutput = m_audioSink->start();
    if (m_audioOutput == nullptr) {
        emit warningRaised(QStringLiteral("无法启动降噪监听设备"));
        m_audioSink.reset();
    }
}

void NoiseReductionWorker::closeAudioOutput()
{
    m_audioOutput = nullptr;
    m_audioSink.reset();
    m_audioOutputAttempted = false;
    m_monitorBuffer.clear();
    m_monitorOverflowWarningActive = false;
}

void NoiseReductionWorker::resetProcessing()
{
    m_reducer.reset();
    m_bandPassFilter.reset();
    m_logMmseProcessor.reset();
    m_automaticGainControl.reset();
    for (QVector<float> &history : m_history) {
        history.clear();
    }
    m_inputTimeline.clear();
    clearRecordingBuffers();
    m_lastPlotTimestamp = -1;
}

QVector<float> NoiseReductionWorker::applyPostProcessing(
    QVector<float> samples,
    std::array<QVector<float>, 4> *capturedStages)
{
    for (NoisePostProcessingNode node
         : m_settings.postProcessingOrder) {
        if (samples.isEmpty()) {
            break;
        }
        switch (node) {
        case NoisePostProcessingNode::BandPass:
            if (m_settings.bandPass.enabled) {
                samples = m_bandPassFilter.process(samples);
            }
            break;
        case NoisePostProcessingNode::LogMmse:
            if (m_settings.logMmse.enabled) {
                samples = m_logMmseProcessor.process(samples);
            }
            break;
        case NoisePostProcessingNode::AutomaticGainControl:
            if (m_settings.automaticGainControl.enabled) {
                samples = m_automaticGainControl.process(samples);
            }
            break;
        }
        const NoiseReductionDataFlowStage stage = dataFlowStage(node);
        if (capturedStages != nullptr
            && m_recordingStageSet.contains(stage)) {
            capturedStages->at(static_cast<size_t>(
                recordingStageIndex(stage))) = samples;
        }
    }
    return samples;
}

void NoiseReductionWorker::publishRecordingFrame(
    const std::array<QVector<float>, 4> &capturedStages,
    const std::deque<SampleTimestampSegment> &capturedTimeline)
{
    if (m_recordingStages.isEmpty()) {
        return;
    }
    const qsizetype capturedSampleCount =
        timelineSampleCount(capturedTimeline);
    if (capturedSampleCount <= 0) {
        return;
    }
    for (NoiseReductionDataFlowStage stage : m_recordingStages) {
        const size_t index =
            static_cast<size_t>(recordingStageIndex(stage));
        if (capturedStages.at(index).size() != capturedSampleCount) {
            return;
        }
        m_recordingBuffers.at(index) += capturedStages.at(index);
    }
    m_recordingTimeline.insert(
        m_recordingTimeline.end(),
        capturedTimeline.cbegin(), capturedTimeline.cend());

    while (timelineSampleCount(m_recordingTimeline)
           >= AudioFrameDecoder::SamplesPerFrame) {
        bool tracksReady = true;
        for (NoiseReductionDataFlowStage stage : m_recordingStages) {
            if (m_recordingBuffers.at(static_cast<size_t>(
                    recordingStageIndex(stage))).size()
                < AudioFrameDecoder::SamplesPerFrame) {
                tracksReady = false;
                break;
            }
        }
        if (!tracksReady) {
            return;
        }

        NoiseReductionRecordingFrame frame;
        frame.sampleRate = m_sampleRate;
        const qint64 timestamp =
            segmentTimestampMicroseconds(m_recordingTimeline.front());
        frame.timestampSeconds =
            static_cast<quint32>(timestamp / 1000000);
        frame.timestampMicros =
            static_cast<quint32>(timestamp % 1000000);
        frame.stages.reserve(m_recordingStages.size());
        for (NoiseReductionDataFlowStage stage : m_recordingStages) {
            QVector<float> &buffer = m_recordingBuffers.at(
                static_cast<size_t>(recordingStageIndex(stage)));
            NoiseReductionStageSamples stageSamples;
            stageSamples.stage = stage;
            stageSamples.samples =
                buffer.first(AudioFrameDecoder::SamplesPerFrame);
            buffer.remove(0, AudioFrameDecoder::SamplesPerFrame);
            frame.stages.append(std::move(stageSamples));
        }
        discardTimelineSamples(
            &m_recordingTimeline,
            AudioFrameDecoder::SamplesPerFrame);
        emit recordingFrameReady(frame);
    }
}

std::deque<NoiseReductionWorker::SampleTimestampSegment>
NoiseReductionWorker::takeInputTimeline(qsizetype sampleCount)
{
    std::deque<SampleTimestampSegment> taken;
    qsizetype remaining = sampleCount;
    while (remaining > 0 && !m_inputTimeline.empty()) {
        SampleTimestampSegment &front = m_inputTimeline.front();
        const qsizetype count = std::min(remaining, front.sampleCount);
        taken.push_back({
            front.originTimestampMicroseconds,
            front.sampleOffset,
            count,
        });
        front.sampleOffset += count;
        front.sampleCount -= count;
        remaining -= count;
        if (front.sampleCount == 0) {
            m_inputTimeline.pop_front();
        }
    }
    return taken;
}

void NoiseReductionWorker::appendInputTimeline(
    quint32 timestampSeconds,
    quint32 timestampMicros,
    qsizetype sampleCount)
{
    if (sampleCount <= 0) {
        return;
    }
    const qint64 timestamp =
        static_cast<qint64>(timestampSeconds) * 1000000
        + static_cast<qint64>(timestampMicros);
    if (!m_inputTimeline.empty()) {
        SampleTimestampSegment &last = m_inputTimeline.back();
        SampleTimestampSegment end = last;
        end.sampleOffset += end.sampleCount;
        end.sampleCount = 0;
        const qint64 expectedTimestamp =
            segmentTimestampMicroseconds(end);
        const qint64 difference = timestamp >= expectedTimestamp
            ? timestamp - expectedTimestamp
            : expectedTimestamp - timestamp;
        const qint64 oneSampleMicroseconds = std::max<qint64>(
            1, (1000000 + m_sampleRate - 1) / m_sampleRate);
        if (difference <= oneSampleMicroseconds) {
            last.sampleCount += sampleCount;
            return;
        }
    }
    m_inputTimeline.push_back({timestamp, 0, sampleCount});
}

qsizetype NoiseReductionWorker::timelineSampleCount(
    const std::deque<SampleTimestampSegment> &timeline)
{
    qsizetype count = 0;
    for (const SampleTimestampSegment &segment : timeline) {
        count += segment.sampleCount;
    }
    return count;
}

void NoiseReductionWorker::discardTimelineSamples(
    std::deque<SampleTimestampSegment> *timeline,
    qsizetype sampleCount)
{
    if (timeline == nullptr) {
        return;
    }
    qsizetype remaining = sampleCount;
    while (remaining > 0 && !timeline->empty()) {
        SampleTimestampSegment &front = timeline->front();
        const qsizetype count = std::min(remaining, front.sampleCount);
        front.sampleOffset += count;
        front.sampleCount -= count;
        remaining -= count;
        if (front.sampleCount == 0) {
            timeline->pop_front();
        }
    }
}

qint64 NoiseReductionWorker::segmentTimestampMicroseconds(
    const SampleTimestampSegment &segment) const
{
    const qint64 scaledOffset = segment.sampleOffset * 1000000;
    const qint64 roundedOffset =
        (scaledOffset + m_sampleRate / 2) / m_sampleRate;
    return segment.originTimestampMicroseconds + roundedOffset;
}

void NoiseReductionWorker::clearRecordingBuffers()
{
    for (QVector<float> &buffer : m_recordingBuffers) {
        buffer.clear();
    }
    m_recordingTimeline.clear();
}

} // namespace MicArray
