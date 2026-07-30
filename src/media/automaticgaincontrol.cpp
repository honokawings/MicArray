#include "media/automaticgaincontrol.h"

#include <algorithm>
#include <cmath>

namespace MicArray {
namespace {

double smoothingCoefficient(double milliseconds, int sampleRate)
{
    return std::exp(
        -1.0 / (milliseconds * 0.001 * static_cast<double>(sampleRate)));
}

bool finiteInRange(
    double value, double minimum, double maximum)
{
    return std::isfinite(value)
        && value >= minimum && value <= maximum;
}

} // namespace

bool AutomaticGainControl::configure(
    int sampleRate,
    const AutomaticGainControlSettings &settings)
{
    if (sampleRate <= 0
        || !finiteInRange(settings.targetLevelDbfs, -60.0, 0.0)
        || !finiteInRange(settings.maximumGainDb, 0.0, 60.0)
        || !finiteInRange(settings.maximumAttenuationDb, -60.0, 0.0)
        || !finiteInRange(settings.attackMilliseconds, 1.0, 1000.0)
        || !finiteInRange(settings.releaseMilliseconds, 1.0, 5000.0)
        || !finiteInRange(settings.limiterLevelDbfs, -20.0, 0.0)) {
        m_configured = false;
        reset();
        return false;
    }

    m_settings = settings;
    m_sampleRate = sampleRate;
    m_attackCoefficient =
        smoothingCoefficient(settings.attackMilliseconds, sampleRate);
    m_releaseCoefficient =
        smoothingCoefficient(settings.releaseMilliseconds, sampleRate);
    constexpr double DecibelsToNaturalLog = 0.11512925464970228420;
    m_targetAmplitude =
        std::exp(settings.targetLevelDbfs * DecibelsToNaturalLog);
    m_minimumGain =
        std::exp(settings.maximumAttenuationDb * DecibelsToNaturalLog);
    m_maximumGain =
        std::exp(settings.maximumGainDb * DecibelsToNaturalLog);
    m_limiterAmplitude =
        std::exp(settings.limiterLevelDbfs * DecibelsToNaturalLog);
    m_configured = true;
    reset();
    return true;
}

void AutomaticGainControl::reset()
{
    m_envelopePower = 0.0;
    m_gain = 1.0;
}

QVector<float> AutomaticGainControl::process(
    const QVector<float> &samples)
{
    if (!m_configured || samples.isEmpty()) {
        return m_configured ? samples : QVector<float> {};
    }

    QVector<float> output;
    output.reserve(samples.size());
    for (float inputSample : samples) {
        const double sample = std::isfinite(inputSample)
            ? static_cast<double>(inputSample) : 0.0;
        const double power = sample * sample;
        m_envelopePower =
            m_attackCoefficient * m_envelopePower
            + (1.0 - m_attackCoefficient) * power;
        const double desiredGain = std::clamp(
            m_targetAmplitude
                / std::sqrt(std::max(m_envelopePower, 1.0e-12)),
            m_minimumGain, m_maximumGain);
        const double gainCoefficient =
            desiredGain < m_gain
                ? m_attackCoefficient : m_releaseCoefficient;
        m_gain =
            gainCoefficient * m_gain
            + (1.0 - gainCoefficient) * desiredGain;
        const double controlled = std::clamp(
            std::isfinite(m_gain) ? sample * m_gain : 0.0,
            -m_limiterAmplitude, m_limiterAmplitude);
        output.append(static_cast<float>(controlled));
    }
    return output;
}

} // namespace MicArray
