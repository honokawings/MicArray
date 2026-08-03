#include "media/fractionaldelaybeamformer.h"

#include "analysis/arraygeometry.h"
#include "media/audioframedecoder.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>

namespace MicArray {
namespace {

constexpr double SoundSpeedMetresPerSecond = 343.0;
constexpr int InterpolationGuardSamples = 2;

double sanitizedAngle(double degrees)
{
    return std::isfinite(degrees)
        ? std::clamp(degrees, -90.0, 90.0)
        : 0.0;
}

} // namespace

void FractionalDelayBeamformer::configure(
    int sampleRate,
    const ContinuousDirectionTarget &enhancedA,
    const ContinuousDirectionTarget &suppressionB1,
    const ContinuousDirectionTarget &suppressionB2)
{
    const int effectiveSampleRate = sampleRate > 0
        ? std::clamp(sampleRate, 8000, 192000)
        : AudioFrameDecoder::DefaultSampleRate;
    if (!m_configured || effectiveSampleRate != m_sampleRate) {
        initializeForSampleRate(effectiveSampleRate);
    }

    m_directionTaps[0] = tapsForDirection(enhancedA);
    m_directionTaps[1] = tapsForDirection(suppressionB1);
    m_directionTaps[2] = tapsForDirection(suppressionB2);
    m_configured = true;
}

BeamformedDirectionFrame FractionalDelayBeamformer::process(
    const QByteArray &payload)
{
    BeamformedDirectionFrame frame;
    if (payload.size() != AudioFrameDecoder::FramePayloadBytes) {
        frame.error = QStringLiteral(
            "Expected 38400-byte 48-channel x 200-sample beamformer payload, got %1 bytes")
                          .arg(payload.size());
        return frame;
    }
    if (!m_configured) {
        configure(AudioFrameDecoder::DefaultSampleRate, {}, {}, {});
    }

    frame.firstElement.reserve(AudioFrameDecoder::SamplesPerFrame);
    frame.enhancedA.reserve(AudioFrameDecoder::SamplesPerFrame);
    frame.suppressionB1.reserve(AudioFrameDecoder::SamplesPerFrame);
    frame.suppressionB2.reserve(AudioFrameDecoder::SamplesPerFrame);

    for (int sample = 0; sample < AudioFrameDecoder::SamplesPerFrame; ++sample) {
        HistorySample &current = m_history[m_writeIndex];
        const int sampleOffset = sample * AudioFrameDecoder::BytesPerSamplePoint;
        for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
            const int offset = sampleOffset
                + channel * AudioFrameDecoder::BytesPerChannel;
            const qint32 raw = qFromLittleEndian<qint32>(
                reinterpret_cast<const uchar *>(payload.constData() + offset));
            current[static_cast<size_t>(channel)] =
                std::clamp(static_cast<float>(raw >> 8) / 32768.0f,
                           -1.0f,
                           1.0f);
        }
        frame.firstElement.append(current[0]);

        if (m_totalSamples >= m_startupSamples) {
            frame.enhancedA.append(beamformedSample(m_directionTaps[0]));
            frame.suppressionB1.append(beamformedSample(m_directionTaps[1]));
            frame.suppressionB2.append(beamformedSample(m_directionTaps[2]));
        } else {
            frame.enhancedA.append(0.0f);
            frame.suppressionB1.append(0.0f);
            frame.suppressionB2.append(0.0f);
        }

        m_writeIndex = (m_writeIndex + 1) % m_history.size();
        ++m_totalSamples;
    }

    frame.valid = true;
    return frame;
}

void FractionalDelayBeamformer::reset()
{
    for (HistorySample &sample : m_history) {
        sample.fill(0.0f);
    }
    m_writeIndex = 0;
    m_totalSamples = 0;
}

void FractionalDelayBeamformer::initializeForSampleRate(int sampleRate)
{
    m_sampleRate = sampleRate;
    const QVector<MicrophonePosition> microphones = createDefaultArrayGeometry();
    double maximumDistanceMetres = 0.0;
    for (const MicrophonePosition &microphone : microphones) {
        maximumDistanceMetres = std::max(
            maximumDistanceMetres,
            std::sqrt(microphone.xMeters * microphone.xMeters
                      + microphone.yMeters * microphone.yMeters
                      + microphone.zMeters * microphone.zMeters));
    }
    const double maximumPropagationSamples =
        maximumDistanceMetres * m_sampleRate / SoundSpeedMetresPerSecond;
    m_referenceDelaySamples = std::ceil(maximumPropagationSamples)
        + InterpolationGuardSamples;
    m_startupSamples = static_cast<int>(
        std::ceil(m_referenceDelaySamples + maximumPropagationSamples))
        + InterpolationGuardSamples;
    m_history.resize(m_startupSamples + 1);
    reset();
}

FractionalDelayBeamformer::DirectionDelayTaps
FractionalDelayBeamformer::tapsForDirection(
    const ContinuousDirectionTarget &target) const
{
    const QVector3D direction = directionFromAngles(
        sanitizedAngle(target.azimuthDegrees),
        sanitizedAngle(target.elevationDegrees));
    const QVector<MicrophonePosition> microphones = createDefaultArrayGeometry();
    DirectionDelayTaps taps;
    for (int channel = 0; channel < microphones.size(); ++channel) {
        const MicrophonePosition &microphone = microphones.at(channel);
        const double propagationSamples =
            (microphone.xMeters * static_cast<double>(direction.x())
             + microphone.yMeters * static_cast<double>(direction.y())
             + microphone.zMeters * static_cast<double>(direction.z()))
            * m_sampleRate / SoundSpeedMetresPerSecond;
        const double delay = m_referenceDelaySamples - propagationSamples;
        DelayTap &tap = taps[static_cast<size_t>(channel)];
        tap.integerDelay = static_cast<int>(std::floor(delay));
        const double fraction = delay - tap.integerDelay;
        tap.coefficients[0] =
            -fraction * (fraction - 1.0) * (fraction - 2.0) / 6.0;
        tap.coefficients[1] =
            (fraction + 1.0) * (fraction - 1.0) * (fraction - 2.0) / 2.0;
        tap.coefficients[2] =
            -(fraction + 1.0) * fraction * (fraction - 2.0) / 2.0;
        tap.coefficients[3] =
            (fraction + 1.0) * fraction * (fraction - 1.0) / 6.0;
    }
    return taps;
}

float FractionalDelayBeamformer::delayedSample(
    int channel, const DelayTap &tap) const
{
    double interpolated = 0.0;
    for (int coefficient = 0; coefficient < 4; ++coefficient) {
        const int delay = tap.integerDelay + coefficient - 1;
        int historyIndex = m_writeIndex - delay;
        if (historyIndex < 0) {
            historyIndex += m_history.size();
        }
        interpolated += tap.coefficients[static_cast<size_t>(coefficient)]
            * m_history.at(historyIndex)[static_cast<size_t>(channel)];
    }
    return static_cast<float>(interpolated);
}

float FractionalDelayBeamformer::beamformedSample(
    const DirectionDelayTaps &taps) const
{
    double sum = 0.0;
    for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
        sum += delayedSample(channel, taps[static_cast<size_t>(channel)]);
    }
    return static_cast<float>(
        sum / static_cast<double>(AudioFrameDecoder::ChannelCount));
}

} // namespace MicArray
