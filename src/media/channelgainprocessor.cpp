#include "media/channelgainprocessor.h"

#include "media/audioframedecoder.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>

namespace MicArray {

namespace {

constexpr double FullScaleMagnitude = 2147483648.0;
static_assert(GainConfiguration::ChannelCount == AudioFrameDecoder::ChannelCount);

quint64 sampleMagnitude(qint32 sample)
{
    const qint64 widened = static_cast<qint64>(sample);
    return static_cast<quint64>(sample < 0 ? -widened : widened);
}

qint32 applyShift(qint32 sample, int shift, quint64 *clipCount)
{
    if (shift < 0) {
        return sample >> -shift;
    }

    const qint64 magnitude = sample < 0
        ? -static_cast<qint64>(sample)
        : static_cast<qint64>(sample);
    const qint64 shiftedMagnitude = magnitude << shift;
    const qint64 widened = sample < 0 ? -shiftedMagnitude : shiftedMagnitude;
    const qint64 saturated = std::clamp(widened,
                                        qint64(std::numeric_limits<qint32>::min()),
                                        qint64(std::numeric_limits<qint32>::max()));
    if (saturated != widened) {
        ++(*clipCount);
    }
    return static_cast<qint32>(saturated);
}

qint32 readSample(const QByteArray &payload, int offset)
{
    return qFromLittleEndian<qint32>(
        reinterpret_cast<const uchar *>(payload.constData() + offset));
}

void writeSample(QByteArray &payload, int offset, qint32 sample)
{
    qToLittleEndian(sample, reinterpret_cast<uchar *>(payload.data() + offset));
}

} // namespace

bool ChannelGainProcessor::setConfiguration(const GainConfiguration &configuration, QString *error)
{
    QString validationError;
    if (!GainConfiguration::validate(configuration, &validationError)) {
        if (error) {
            *error = validationError;
        }
        return false;
    }

    const bool agcParametersChanged = configuration.upperPeakRatio != m_configuration.upperPeakRatio
        || configuration.lowerPeakRatio != m_configuration.lowerPeakRatio
        || configuration.releaseHoldMs != m_configuration.releaseHoldMs
        || configuration.releaseStepMs != m_configuration.releaseStepMs;
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        if (agcParametersChanged
            || configuration.shifts.at(channel) != m_configuration.shifts.at(channel)
            || configuration.automatic.at(channel) != m_configuration.automatic.at(channel)) {
            m_lowDurationMicros[channel] = 0;
            m_releaseDurationMicros[channel] = 0;
        }
    }
    m_configuration = configuration;
    m_snapshot.shifts = m_configuration.shifts;
    m_snapshot.automatic = m_configuration.automatic;
    if (error) {
        error->clear();
    }
    return true;
}

GainProcessResult ChannelGainProcessor::process(const QByteArray &payload, int sampleRate)
{
    GainProcessResult result;
    if (payload.size() != AudioFrameDecoder::FramePayloadBytes) {
        result.error = QStringLiteral("Expected 38400-byte 48-channel x 200-sample audio payload, got %1 bytes")
                           .arg(payload.size());
        return result;
    }
    if (sampleRate <= 0) {
        result.error = QStringLiteral("Sample rate must be positive");
        return result;
    }
    const qint64 frameDurationMicros = static_cast<qint64>(AudioFrameDecoder::SamplesPerFrame)
        * 1'000'000 / sampleRate;
    if (frameDurationMicros <= 0) {
        result.error = QStringLiteral("Frame duration must be at least one microsecond for sample rate %1")
                           .arg(sampleRate);
        return result;
    }

    QVector<quint64> inputPeaks(GainConfiguration::ChannelCount, 0);
    for (int samplePoint = 0; samplePoint < AudioFrameDecoder::SamplesPerFrame; ++samplePoint) {
        const int sampleBase = samplePoint * AudioFrameDecoder::BytesPerSamplePoint;
        for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
            const int offset = sampleBase + channel * AudioFrameDecoder::BytesPerChannel;
            inputPeaks[channel] = std::max(inputPeaks.at(channel),
                                           sampleMagnitude(readSample(payload, offset)));
        }
    }

    updateAutomaticGain(inputPeaks, frameDurationMicros);

    result.payload.resize(payload.size());
    QVector<quint64> outputPeaks(GainConfiguration::ChannelCount, 0);
    for (int samplePoint = 0; samplePoint < AudioFrameDecoder::SamplesPerFrame; ++samplePoint) {
        const int sampleBase = samplePoint * AudioFrameDecoder::BytesPerSamplePoint;
        for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
            const int offset = sampleBase + channel * AudioFrameDecoder::BytesPerChannel;
            const qint32 shifted = applyShift(readSample(payload, offset),
                                               m_configuration.shifts.at(channel),
                                               &m_clipCounts[channel]);
            outputPeaks[channel] = std::max(outputPeaks.at(channel), sampleMagnitude(shifted));
            writeSample(result.payload, offset, shifted);
        }
    }

    m_snapshot.shifts = m_configuration.shifts;
    m_snapshot.automatic = m_configuration.automatic;
    m_snapshot.clipCounts = m_clipCounts;
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        m_snapshot.inputPeakRatios[channel] = inputPeaks.at(channel) / FullScaleMagnitude;
        m_snapshot.outputPeakRatios[channel] = outputPeaks.at(channel) / FullScaleMagnitude;
    }

    result.valid = true;
    result.snapshot = m_snapshot;
    return result;
}

bool GainConfiguration::validate(const GainConfiguration &configuration, QString *error)
{
    if (configuration.shifts.size() != GainConfiguration::ChannelCount) {
        if (error) {
            *error = QStringLiteral("Gain shifts must contain exactly 48 entries");
        }
        return false;
    }
    if (configuration.automatic.size() != GainConfiguration::ChannelCount) {
        if (error) {
            *error = QStringLiteral("Automatic modes must contain exactly 48 entries");
        }
        return false;
    }
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        const int shift = configuration.shifts.at(channel);
        if (shift < GainConfiguration::MinimumShift || shift > GainConfiguration::MaximumShift) {
            if (error) {
                *error = QStringLiteral("Channel %1 shift must be between -8 and +8").arg(channel);
            }
            return false;
        }
    }
    if (!std::isfinite(configuration.upperPeakRatio)
        || configuration.upperPeakRatio <= 0.0
        || configuration.upperPeakRatio > 1.0) {
        if (error) {
            *error = QStringLiteral("AGC upper peak ratio must be in (0, 1]");
        }
        return false;
    }
    if (!std::isfinite(configuration.lowerPeakRatio)
        || configuration.lowerPeakRatio < 0.0
        || configuration.lowerPeakRatio >= configuration.upperPeakRatio) {
        if (error) {
            *error = QStringLiteral("AGC lower peak ratio must be in [0, upper peak ratio)");
        }
        return false;
    }
    if (configuration.releaseHoldMs <= 0) {
        if (error) {
            *error = QStringLiteral("AGC release hold must be positive");
        }
        return false;
    }
    if (configuration.releaseStepMs <= 0) {
        if (error) {
            *error = QStringLiteral("AGC release step must be positive");
        }
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

void ChannelGainProcessor::updateAutomaticGain(const QVector<quint64> &inputPeaks,
                                               qint64 frameDurationMicros)
{
    const qint64 holdMicros = static_cast<qint64>(m_configuration.releaseHoldMs) * 1000;
    const qint64 stepMicros = static_cast<qint64>(m_configuration.releaseStepMs) * 1000;

    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        if (!m_configuration.automatic.at(channel)) {
            m_lowDurationMicros[channel] = 0;
            m_releaseDurationMicros[channel] = 0;
            continue;
        }

        int &shift = m_configuration.shifts[channel];
        const double inputRatio = inputPeaks.at(channel) / FullScaleMagnitude;
        double predictedRatio = std::ldexp(inputRatio, shift);
        bool attacked = false;
        while (predictedRatio > m_configuration.upperPeakRatio
               && shift > GainConfiguration::MinimumShift) {
            --shift;
            predictedRatio *= 0.5;
            attacked = true;
        }

        if (attacked || predictedRatio >= m_configuration.lowerPeakRatio) {
            m_lowDurationMicros[channel] = 0;
            m_releaseDurationMicros[channel] = 0;
            continue;
        }

        const qint64 previousLowDuration = m_lowDurationMicros.at(channel);
        m_lowDurationMicros[channel] += frameDurationMicros;
        if (previousLowDuration >= holdMicros) {
            m_releaseDurationMicros[channel] += frameDurationMicros;
        } else if (m_lowDurationMicros.at(channel) > holdMicros) {
            m_releaseDurationMicros[channel] += m_lowDurationMicros.at(channel) - holdMicros;
        }

        while (m_releaseDurationMicros.at(channel) >= stepMicros
               && shift < GainConfiguration::MaximumShift) {
            const double candidateRatio = predictedRatio * 2.0;
            if (candidateRatio > m_configuration.upperPeakRatio) {
                m_releaseDurationMicros[channel] = stepMicros;
                break;
            }
            ++shift;
            m_releaseDurationMicros[channel] -= stepMicros;
            predictedRatio = candidateRatio;
            if (predictedRatio >= m_configuration.lowerPeakRatio) {
                m_lowDurationMicros[channel] = 0;
                m_releaseDurationMicros[channel] = 0;
                break;
            }
        }
    }
}

} // namespace MicArray
