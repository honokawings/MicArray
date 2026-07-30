#include "media/logmmseprocessor.h"

#include "analysis/radix2fft.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace MicArray {
namespace {

constexpr double EulerMascheroni = 0.57721566490153286061;
constexpr double PowerFloor = 1.0e-12;

bool isPowerOfTwo(int value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

} // namespace

bool LogMmseProcessor::configure(
    int sampleRate, const LogMmseSettings &settings)
{
    const int hopSize = static_cast<int>(
        std::lround(settings.fftSize * (1.0 - settings.overlapRatio)));
    if (sampleRate <= 0
        || settings.fftSize < 256 || settings.fftSize > 2048
        || !isPowerOfTwo(settings.fftSize)
        || !std::isfinite(settings.overlapRatio)
        || settings.overlapRatio < 0.0
        || settings.overlapRatio >= 1.0
        || hopSize <= 0 || hopSize > settings.fftSize
        || !std::isfinite(settings.priorSnrSmoothing)
        || settings.priorSnrSmoothing < 0.0
        || settings.priorSnrSmoothing >= 1.0
        || !std::isfinite(settings.noisePowerSmoothing)
        || settings.noisePowerSmoothing < 0.0
        || settings.noisePowerSmoothing >= 1.0
        || !std::isfinite(settings.minimumGainDb)
        || settings.minimumGainDb < -80.0
        || settings.minimumGainDb > 0.0) {
        m_configured = false;
        reset();
        return false;
    }

    m_settings = settings;
    m_sampleRate = sampleRate;
    m_hopSize = hopSize;
    m_configured = true;
    m_window.resize(m_settings.fftSize);
    for (int index = 0; index < m_window.size(); ++index) {
        m_window[index] =
            0.5 - 0.5 * std::cos(
                2.0 * std::numbers::pi * index
                / static_cast<double>(m_window.size() - 1));
    }
    reset();
    return true;
}

void LogMmseProcessor::reset()
{
    m_noiseInitialized = false;
    m_inputQueue.clear();
    m_outputQueue.clear();
    m_overlapOutput.clear();
    m_overlapNormalization.clear();
    m_noisePower.clear();
    m_previousPosteriorSnr.clear();
    m_previousGain.clear();
    if (!m_configured) {
        return;
    }

    m_outputQueue.resize(m_settings.fftSize);
    std::fill(m_outputQueue.begin(), m_outputQueue.end(), 0.0f);
    m_overlapOutput.resize(m_settings.fftSize);
    m_overlapNormalization.resize(m_settings.fftSize);
    std::fill(m_overlapOutput.begin(), m_overlapOutput.end(), 0.0);
    std::fill(m_overlapNormalization.begin(),
              m_overlapNormalization.end(), 0.0);
    const int binCount = m_settings.fftSize / 2 + 1;
    m_noisePower.resize(binCount);
    m_previousPosteriorSnr.resize(binCount);
    m_previousGain.resize(binCount);
    std::fill(m_noisePower.begin(), m_noisePower.end(), PowerFloor);
    std::fill(m_previousPosteriorSnr.begin(),
              m_previousPosteriorSnr.end(), 1.0);
    std::fill(m_previousGain.begin(), m_previousGain.end(), 1.0);
}

QVector<float> LogMmseProcessor::process(const QVector<float> &samples)
{
    if (!m_configured || samples.isEmpty()) {
        return m_configured ? samples : QVector<float> {};
    }

    m_inputQueue += samples;
    processAvailableFrames();

    QVector<float> output;
    output.reserve(samples.size());
    const int available = std::min(samples.size(), m_outputQueue.size());
    output.append(m_outputQueue.cbegin(),
                  m_outputQueue.cbegin() + available);
    m_outputQueue.remove(0, available);
    if (output.size() < samples.size()) {
        output.resize(samples.size());
    }
    return output;
}

double LogMmseProcessor::exponentialIntegralE1(double value)
{
    const double x = std::max(value, 1.0e-12);
    if (x <= 1.0) {
        double sum = 0.0;
        double term = 1.0;
        for (int index = 1; index <= 100; ++index) {
            term *= -x / static_cast<double>(index);
            const double addition = term / static_cast<double>(index);
            sum += addition;
            if (std::abs(addition)
                < std::abs(sum) * std::numeric_limits<double>::epsilon()) {
                break;
            }
        }
        return -EulerMascheroni - std::log(x) - sum;
    }

    constexpr double Tiny = 1.0e-300;
    double b = x + 1.0;
    double c = 1.0 / Tiny;
    double d = 1.0 / b;
    double result = d;
    for (int index = 1; index <= 100; ++index) {
        const double a = -static_cast<double>(index * index);
        b += 2.0;
        d = a * d + b;
        if (std::abs(d) < Tiny) {
            d = Tiny;
        }
        c = b + a / c;
        if (std::abs(c) < Tiny) {
            c = Tiny;
        }
        d = 1.0 / d;
        const double delta = c * d;
        result *= delta;
        if (std::abs(delta - 1.0) < 1.0e-12) {
            break;
        }
    }
    return result * std::exp(-x);
}

void LogMmseProcessor::processAvailableFrames()
{
    while (m_inputQueue.size() >= m_settings.fftSize) {
        processFrame();
        m_inputQueue.remove(0, m_hopSize);
    }
}

void LogMmseProcessor::processFrame()
{
    const int fftSize = m_settings.fftSize;
    const int binCount = fftSize / 2 + 1;
    QVector<std::complex<double>> spectrum(fftSize);
    for (int index = 0; index < fftSize; ++index) {
        spectrum[index] = {
            static_cast<double>(m_inputQueue.at(index)) * m_window.at(index),
            0.0,
        };
    }
    if (!Radix2Fft::transform(&spectrum, false)) {
        return;
    }

    QVector<double> power(binCount);
    for (int bin = 0; bin < binCount; ++bin) {
        power[bin] = std::max(std::norm(spectrum.at(bin)), PowerFloor);
    }
    if (!m_noiseInitialized) {
        m_noisePower = power;
        m_noiseInitialized = true;
    }

    const double minimumGain =
        std::pow(10.0, m_settings.minimumGainDb / 20.0);
    for (int bin = 0; bin < binCount; ++bin) {
        const double noisePower =
            std::max(m_noisePower.at(bin), PowerFloor);
        const double posteriorSnr = std::clamp(
            power.at(bin) / noisePower, 1.0e-6, 1000.0);
        const double instantaneousPrior =
            std::max(posteriorSnr - 1.0, 0.0);
        const double decisionDirected =
            m_previousGain.at(bin) * m_previousGain.at(bin)
            * m_previousPosteriorSnr.at(bin);
        const double priorSnr = std::max(
            m_settings.priorSnrSmoothing * decisionDirected
                + (1.0 - m_settings.priorSnrSmoothing)
                    * instantaneousPrior,
            1.0e-8);
        const double v = std::max(
            priorSnr * posteriorSnr / (1.0 + priorSnr), 1.0e-12);
        double gain = priorSnr / (1.0 + priorSnr)
            * std::exp(0.5 * exponentialIntegralE1(v));
        gain = std::clamp(
            std::isfinite(gain) ? gain : minimumGain,
            minimumGain, 1.0);
        spectrum[bin] *= gain;
        if (bin > 0 && bin < fftSize / 2) {
            spectrum[fftSize - bin] = std::conj(spectrum.at(bin));
        }

        const double smoothing =
            posteriorSnr < 3.0
                ? m_settings.noisePowerSmoothing
                : std::min(0.999,
                           m_settings.noisePowerSmoothing + 0.04);
        m_noisePower[bin] =
            smoothing * noisePower + (1.0 - smoothing) * power.at(bin);
        m_previousPosteriorSnr[bin] = posteriorSnr;
        m_previousGain[bin] = gain;
    }

    if (!Radix2Fft::transform(&spectrum, true)) {
        return;
    }
    for (int index = 0; index < fftSize; ++index) {
        const double window = m_window.at(index);
        m_overlapOutput[index] += spectrum.at(index).real() * window;
        m_overlapNormalization[index] += window * window;
    }
    for (int index = 0; index < m_hopSize; ++index) {
        const double normalization = m_overlapNormalization.at(index);
        const double sample = normalization > 1.0e-12
            ? m_overlapOutput.at(index) / normalization : 0.0;
        m_outputQueue.append(static_cast<float>(
            std::clamp(std::isfinite(sample) ? sample : 0.0, -1.0, 1.0)));
    }
    m_overlapOutput.remove(0, m_hopSize);
    m_overlapNormalization.remove(0, m_hopSize);
    m_overlapOutput.resize(fftSize);
    m_overlapNormalization.resize(fftSize);
}

} // namespace MicArray

