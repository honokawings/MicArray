#include "media/audioprocessor.h"

#include <QMediaDevices>

#include <algorithm>
#include <cmath>

namespace MicArray {

AudioProcessor::AudioProcessor(QObject *parent)
    : QObject(parent)
{
    configure(AudioSettings {});
}

void AudioProcessor::configure(const AudioSettings &settings)
{
    m_playbackEnabled = settings.playbackEnabled;
    m_decoder.setOutputChannel(settings.outputChannel);
    m_decoder.setAverageWindowFrames(settings.averageWindowFrames);
    m_format.setSampleRate(settings.sampleRate);
    m_format.setChannelCount(1);
    m_format.setSampleFormat(QAudioFormat::Int16);

    m_sink.reset();
    m_output = nullptr;

    if (!m_playbackEnabled) {
        return;
    }

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull() || !device.isFormatSupported(m_format)) {
        emit audioWarning(QStringLiteral("Configured PCM format is not supported by the default audio output"));
        return;
    }

    m_sink = std::make_unique<QAudioSink>(device, m_format);
    m_sink->setBufferSize(m_format.bytesForDuration(200000));
    m_output = m_sink->start();
}

void AudioProcessor::processPcmFrame(const QByteArray &payload, quint32 timestampSeconds, quint32 timestampMicros)
{
    m_bytesReceived += payload.size();

    double peak = 0.0;
    const qsizetype sampleCount = payload.size() / 2;
    const auto *samples = reinterpret_cast<const qint16 *>(payload.constData());
    for (qsizetype i = 0; i < sampleCount; ++i) {
        peak = std::max(peak, std::abs(static_cast<double>(samples[i])) / 32768.0);
    }
    m_peakLevel = peak;

    if (m_playbackEnabled && m_output != nullptr) {
        const qint64 written = m_output->write(payload);
        if (written < payload.size()) {
            ++m_droppedFrames;
            emit audioWarning(QStringLiteral("Audio output buffer is full; frame was partially written"));
        }
    }

    emit levelChanged(m_peakLevel, m_bytesReceived, timestampSeconds, timestampMicros);
}

void AudioProcessor::processArrayFrame(const QByteArray &payload, quint32 timestampSeconds, quint32 timestampMicros)
{
    m_bytesReceived += payload.size();
    const DecodedAudioFrame frame = m_decoder.decode(payload, timestampSeconds, timestampMicros);
    if (!frame.valid) {
        ++m_droppedFrames;
        emit audioWarning(frame.error);
        emit frameProcessed(false);
        return;
    }

    m_peakLevel = frame.maxAverageAmplitude > 0.0 ? frame.amplitudes.at(frame.outputChannel) / 2147483647.0 : 0.0;

    if (m_playbackEnabled && m_output != nullptr) {
        const qint64 written = m_output->write(frame.outputPcm16);
        if (written < frame.outputPcm16.size()) {
            ++m_droppedFrames;
            emit audioWarning(QStringLiteral("Audio output buffer is full; selected-channel sample was not fully written"));
        }
    }

    emit arrayFrameDecoded(frame);
    emit levelChanged(m_peakLevel, m_bytesReceived, timestampSeconds, timestampMicros);
    emit frameProcessed(true);
}

} // namespace MicArray
