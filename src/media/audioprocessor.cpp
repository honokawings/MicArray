#include "media/audioprocessor.h"

#include <QMediaDevices>

#include <algorithm>
#include <cmath>

namespace MicArray {

AudioProcessor::AudioProcessor(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<GainConfiguration>("MicArray::GainConfiguration");
    qRegisterMetaType<GainSnapshot>("MicArray::GainSnapshot");
    qRegisterMetaType<PlLocationResult>("MicArray::PlLocationResult");
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

void AudioProcessor::resetSpatialMaps(quint64 epoch)
{
    m_spatialEpoch = epoch;
    m_spatialAssembler.reset();
}

void AudioProcessor::processArrayFrame(const QByteArray &payload, quint32 timestampSeconds, quint32 timestampMicros)
{
    m_bytesReceived += payload.size();
    const auto sideband = AudioLocationSidebandDecoder::decode(payload, timestampSeconds, timestampMicros);
    if (!sideband.valid) {
        ++m_droppedFrames;
        emit audioWarning(sideband.error);
        emit frameProcessed(false);
        return;
    }
    for (const QString &warning : sideband.warnings) {
        emit audioWarning(warning);
    }
    for (PlLocationResult location : sideband.locations) {
        location.streamEpoch = m_spatialEpoch;
        emit plLocationDecoded(location);
    }
    QStringList mapWarnings;
    auto maps = m_spatialAssembler.consume(payload, timestampSeconds, timestampMicros, &mapWarnings);
    for (PlSpatialMap &map : maps)
    {
        map.streamEpoch = m_spatialEpoch;
        emit plSpatialMapDecoded(map);
    }
    for (const QString &warning : mapWarnings)
    {
        emit audioWarning(warning);
    }
    emit rawFrameReady(sideband.pcmPayload, timestampSeconds, timestampMicros);
    const GainProcessResult adjusted = m_gainProcessor.process(sideband.pcmPayload, m_format.sampleRate());
    if (!adjusted.valid) {
        ++m_droppedFrames;
        emit audioWarning(adjusted.error);
        emit frameProcessed(false);
        return;
    }

    emit adjustedFrameReady(adjusted.payload,
                            timestampSeconds,
                            timestampMicros,
                            adjusted.snapshot);

    const DecodedAudioFrame frame = m_decoder.decode(adjusted.payload,
                                                     timestampSeconds,
                                                     timestampMicros);
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

void AudioProcessor::setGainConfiguration(const GainConfiguration &configuration)
{
    QString error;
    if (!m_gainProcessor.setConfiguration(configuration, &error)) {
        emit audioWarning(QStringLiteral("Invalid gain configuration: %1").arg(error));
    }
}

void AudioProcessor::setChannelShift(int channel, int shift)
{
    GainConfiguration configuration = m_gainProcessor.configuration();
    if (channel < 0 || channel >= GainConfiguration::ChannelCount
        || shift < GainConfiguration::MinimumShift
        || shift > GainConfiguration::MaximumShift) {
        emit audioWarning(QStringLiteral("Invalid gain shift request: channel=%1 shift=%2")
                              .arg(channel)
                              .arg(shift));
        return;
    }
    configuration.shifts[channel] = shift;
    configuration.automatic[channel] = false;
    setGainConfiguration(configuration);
}

void AudioProcessor::setChannelAutomatic(int channel, bool automatic)
{
    if (channel < 0 || channel >= GainConfiguration::ChannelCount) {
        emit audioWarning(QStringLiteral("Invalid automatic gain channel: %1").arg(channel));
        return;
    }
    GainConfiguration configuration = m_gainProcessor.configuration();
    configuration.automatic[channel] = automatic;
    setGainConfiguration(configuration);
}

void AudioProcessor::setAllAutomatic(bool automatic)
{
    GainConfiguration configuration = m_gainProcessor.configuration();
    configuration.automatic.fill(automatic);
    setGainConfiguration(configuration);
}

void AudioProcessor::resetGain()
{
    setGainConfiguration(GainConfiguration {});
}

} // namespace MicArray
