#include "media/audioframedecoder.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>

namespace MicArray {

void AudioFrameDecoder::setOutputChannel(int channel)
{
    m_outputChannel = std::clamp(channel, 0, ChannelCount - 1);
}

void AudioFrameDecoder::setAverageWindowFrames(int frames)
{
    m_averageWindowFrames = std::clamp(frames, 1, 10000);
    while (m_amplitudeHistory.size() > m_averageWindowFrames) {
        m_amplitudeHistory.removeFirst();
    }
}

DecodedAudioFrame AudioFrameDecoder::decode(const QByteArray &payload, quint32 timestampSeconds, quint32 timestampMicros)
{
    DecodedAudioFrame frame;
    frame.timestampSeconds = timestampSeconds;
    frame.timestampMicros = timestampMicros;
    frame.outputChannel = m_outputChannel;
    frame.samplePoints = SamplesPerFrame;

    if (payload.size() != FramePayloadBytes) {
        frame.error = QStringLiteral("Expected 38400-byte 48-channel x 200-sample audio payload, got %1 bytes").arg(payload.size());
        return frame;
    }

    frame.valid = true;
    frame.samples.reserve(ChannelCount);
    frame.amplitudes.reserve(ChannelCount);
    frame.outputPcm16.reserve(SamplesPerFrame * 2);

    QList<double> amplitudeSums;
    amplitudeSums.fill(0.0, ChannelCount);
    QList<qint32> latestSamples;
    latestSamples.fill(0, ChannelCount);

    for (int samplePoint = 0; samplePoint < SamplesPerFrame; ++samplePoint) {
        const int sampleBase = samplePoint * BytesPerSamplePoint;
        for (int channel = 0; channel < ChannelCount; ++channel) {
            const int offset = sampleBase + channel * BytesPerChannel;
            const qint32 sample = decodeSigned32InLittleEndian(payload, offset);
            latestSamples[channel] = sample;
            amplitudeSums[channel] += std::abs(static_cast<double>(sample));

            if (channel == m_outputChannel) {
                const qint16 pcm = toPcm16(sample);
                char bytes[2];
                qToLittleEndian(pcm, bytes);
                frame.outputPcm16.append(bytes, 2);
            }
        }
    }

    for (int channel = 0; channel < ChannelCount; ++channel) {
        frame.samples.append(latestSamples.at(channel));
        frame.amplitudes.append(amplitudeSums.at(channel) / static_cast<double>(SamplesPerFrame));
    }

    m_amplitudeHistory.append(frame.amplitudes);
    while (m_amplitudeHistory.size() > m_averageWindowFrames) {
        m_amplitudeHistory.removeFirst();
    }

    frame.averageAmplitudes = calculateAverages();
    frame.normalizedAmplitudes.reserve(ChannelCount);

    double maxAverage = 0.0;
    double minAverage = std::numeric_limits<double>::max();
    for (int channel = 0; channel < ChannelCount; ++channel) {
        const double value = frame.averageAmplitudes.at(channel);
        if (value > maxAverage) {
            maxAverage = value;
            frame.maxAverageChannel = channel;
        }
        if (value < minAverage) {
            minAverage = value;
            frame.minAverageChannel = channel;
        }
    }
    frame.maxAverageAmplitude = maxAverage;
    frame.minAverageAmplitude = minAverage == std::numeric_limits<double>::max() ? 0.0 : minAverage;

    for (double value : frame.averageAmplitudes) {
        frame.normalizedAmplitudes.append(maxAverage > 0.0 ? value / maxAverage : 0.0);
    }

    return frame;
}

void AudioFrameDecoder::reset()
{
    m_amplitudeHistory.clear();
}

QList<double> AudioFrameDecoder::calculateAverages() const
{
    QList<double> averages;
    averages.fill(0.0, ChannelCount);
    if (m_amplitudeHistory.isEmpty()) {
        return averages;
    }

    for (const QList<double> &frame : m_amplitudeHistory) {
        for (int channel = 0; channel < ChannelCount; ++channel) {
            averages[channel] += frame.at(channel);
        }
    }

    for (double &value : averages) {
        value /= static_cast<double>(m_amplitudeHistory.size());
    }
    return averages;
}

qint32 AudioFrameDecoder::decodeSigned32InLittleEndian(const QByteArray &payload, int offset)
{
    return qFromLittleEndian<qint32>(reinterpret_cast<const uchar *>(payload.constData() + offset));
}

qint16 AudioFrameDecoder::toPcm16(qint32 sample)
{
    const qint32 shifted = sample >> 8;
    return static_cast<qint16>(std::clamp(shifted, -32768, 32767));
}

} // namespace MicArray
