#include "media/bandpassfilter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace MicArray {
namespace {

constexpr double ButterworthQ = 0.70710678118654752440;

} // namespace

BandPassFilter::Biquad BandPassFilter::normalized(
    double b0,
    double b1,
    double b2,
    double a0,
    double a1,
    double a2)
{
    BandPassFilter::Biquad result;
    result.b0 = b0 / a0;
    result.b1 = b1 / a0;
    result.b2 = b2 / a0;
    result.a1 = a1 / a0;
    result.a2 = a2 / a0;
    return result;
}

bool BandPassFilter::configure(
    int sampleRate, double lowCutoffHz, double highCutoffHz)
{
    const double nyquist = sampleRate * 0.5;
    if (sampleRate <= 0
        || !std::isfinite(lowCutoffHz)
        || !std::isfinite(highCutoffHz)
        || lowCutoffHz <= 0.0
        || lowCutoffHz >= highCutoffHz
        || highCutoffHz >= nyquist) {
        m_configured = false;
        reset();
        return false;
    }
    m_highPass = highPass(sampleRate, lowCutoffHz);
    m_lowPass = lowPass(sampleRate, highCutoffHz);
    m_configured = true;
    reset();
    return true;
}

void BandPassFilter::reset()
{
    m_highPass.reset();
    m_lowPass.reset();
}

QVector<float> BandPassFilter::process(const QVector<float> &samples)
{
    if (!m_configured || samples.isEmpty()) {
        return m_configured ? samples : QVector<float> {};
    }
    QVector<float> output;
    output.reserve(samples.size());
    for (float sample : samples) {
        const double highPassed = m_highPass.process(sample);
        const double filtered = m_lowPass.process(highPassed);
        output.append(static_cast<float>(
            std::isfinite(filtered)
                ? std::clamp(filtered, -1.0, 1.0) : 0.0));
    }
    return output;
}

double BandPassFilter::Biquad::process(double sample)
{
    const double output = b0 * sample + delay1;
    delay1 = b1 * sample - a1 * output + delay2;
    delay2 = b2 * sample - a2 * output;
    return output;
}

void BandPassFilter::Biquad::reset()
{
    delay1 = 0.0;
    delay2 = 0.0;
}

BandPassFilter::Biquad BandPassFilter::highPass(
    int sampleRate, double cutoffHz)
{
    const double angularFrequency =
        2.0 * std::numbers::pi * cutoffHz / sampleRate;
    const double cosine = std::cos(angularFrequency);
    const double alpha =
        std::sin(angularFrequency) / (2.0 * ButterworthQ);
    return normalized(
        (1.0 + cosine) * 0.5,
        -(1.0 + cosine),
        (1.0 + cosine) * 0.5,
        1.0 + alpha,
        -2.0 * cosine,
        1.0 - alpha);
}

BandPassFilter::Biquad BandPassFilter::lowPass(
    int sampleRate, double cutoffHz)
{
    const double angularFrequency =
        2.0 * std::numbers::pi * cutoffHz / sampleRate;
    const double cosine = std::cos(angularFrequency);
    const double alpha =
        std::sin(angularFrequency) / (2.0 * ButterworthQ);
    return normalized(
        (1.0 - cosine) * 0.5,
        1.0 - cosine,
        (1.0 - cosine) * 0.5,
        1.0 + alpha,
        -2.0 * cosine,
        1.0 - alpha);
}

} // namespace MicArray
