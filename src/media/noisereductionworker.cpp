#include "media/noisereductionworker.h"

#include "analysis/radix2fft.h"

#include <QMediaDevices>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace MicArray {

NoiseReductionWorker::NoiseReductionWorker(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<NoiseReductionSettings>("MicArray::NoiseReductionSettings");
    qRegisterMetaType<NoiseReductionPlotFrame>(
        "MicArray::NoiseReductionPlotFrame");
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
    const double clampedGainDb = std::clamp(gainDb, -60.0, 24.0);
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
    const bool sampleRateChanged = m_sampleRate != sampleRate;
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
    const bool enabledChanged = m_settings.enabled != settings.enabled;
    const bool monitorEnabledChanged =
        m_settings.monitorEnabled != settings.monitorEnabled;
    const bool monitorChanged =
        m_settings.monitorSource != settings.monitorSource;
    m_settings = settings;
    m_settings.monitorGainDb =
        std::clamp(m_settings.monitorGainDb, -60.0, 24.0);
    m_sampleRate = std::clamp(sampleRate, 8000, 192000);
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
    if (sampleRateChanged || enabledChanged || algorithmChanged
        || modeChanged || coefficientChanged || bandPassChanged) {
        resetProcessing();
    }
    if (!m_settings.enabled || !m_settings.monitorEnabled
        || sampleRateChanged || monitorChanged || monitorEnabledChanged) {
        closeAudioOutput();
    }
    emit statusChanged(m_settings.enabled
                           ? QStringLiteral("降噪处理已启用")
                           : QStringLiteral("降噪处理已关闭"));
}

void NoiseReductionWorker::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (!m_active) {
        closeAudioOutput();
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

    const NoiseReductionInputFrame input = m_decoder.decode(payload);
    if (!input.valid) {
        emit warningRaised(input.error);
        emit frameConsumed();
        return;
    }

    appendHistory(NoiseSignalSource::FirstElement, input.channels.at(0));
    appendHistory(NoiseSignalSource::EnhancedA, input.channels.at(1));
    appendHistory(NoiseSignalSource::SuppressionB1, input.channels.at(2));
    appendHistory(NoiseSignalSource::SuppressionB2, input.channels.at(3));
    QVector<float> denoised =
        m_reducer.process(input.channels.at(1),
                          input.channels.at(2),
                          input.channels.at(3));
    if (m_settings.bandPass.enabled && !denoised.isEmpty()) {
        denoised = m_bandPassFilter.process(denoised);
    }
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
    ensureAudioOutput();
    if (m_audioOutput == nullptr) {
        return;
    }
    const QByteArray pcm = pcm16WithGain(samples, m_settings.monitorGainDb);
    const qint64 written = m_audioOutput->write(pcm);
    if (written < pcm.size()) {
        emit warningRaised(
            QStringLiteral("降噪监听缓冲区已满，部分音频未播放"));
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
    m_audioSink->setBufferSize(m_audioFormat.bytesForDuration(100000));
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
}

void NoiseReductionWorker::resetProcessing()
{
    m_reducer.reset();
    m_bandPassFilter.reset();
    for (QVector<float> &history : m_history) {
        history.clear();
    }
    m_lastPlotTimestamp = -1;
}

} // namespace MicArray
