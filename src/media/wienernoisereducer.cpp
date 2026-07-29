#include "media/wienernoisereducer.h"

#include "analysis/radix2fft.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace MicArray {
namespace {

constexpr double SpectralSmoothing = 0.92;
constexpr double AbsoluteRegularization = 1.0e-6;
constexpr double RelativeRegularization = 1.0e-3;
constexpr double MaximumFilterMagnitude = 4.0;

bool finiteComplex(const std::complex<double> &value)
{
    return std::isfinite(value.real()) && std::isfinite(value.imag());
}

std::complex<double> limitMagnitude(std::complex<double> value,
                                    double maximumMagnitude)
{
    const double magnitude = std::abs(value);
    if (magnitude > maximumMagnitude && magnitude > 0.0) {
        value *= maximumMagnitude / magnitude;
    }
    return value;
}

} // namespace

WienerNoiseReducer::WienerNoiseReducer()
{
    initializeWindow();
    reset();
}

void WienerNoiseReducer::setAlgorithm(NoiseReductionAlgorithm algorithm)
{
    if (m_algorithm == algorithm) {
        return;
    }
    m_algorithm = algorithm;
    reset();
}

void WienerNoiseReducer::setMode(NoiseReductionMode mode)
{
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    reset();
}

void WienerNoiseReducer::setSuppressionCoefficients(
    double single, double dualB1, double dualB2)
{
    const double sanitizedSingle = std::clamp(single, 0.0, 100.0);
    const double sanitizedDualB1 = std::clamp(dualB1, 0.0, 100.0);
    const double sanitizedDualB2 = std::clamp(dualB2, 0.0, 100.0);
    if (m_singleSuppressionCoefficient == sanitizedSingle
        && m_dualB1SuppressionCoefficient == sanitizedDualB1
        && m_dualB2SuppressionCoefficient == sanitizedDualB2) {
        return;
    }
    m_singleSuppressionCoefficient = sanitizedSingle;
    m_dualB1SuppressionCoefficient = sanitizedDualB1;
    m_dualB2SuppressionCoefficient = sanitizedDualB2;
    reset();
}

void WienerNoiseReducer::reset()
{
    m_enhancedInput.clear();
    m_b1Input.clear();
    m_b2Input.clear();
    m_overlap.fill(0.0, HopSize);
    m_b1Power.fill(0.0, FftSize);
    m_b2Power.fill(0.0, FftSize);
    m_b1B2Cross.fill({}, FftSize);
    m_enhancedB1Cross.fill({}, FftSize);
    m_enhancedB2Cross.fill({}, FftSize);
    m_statisticsInitialized = false;
}

QVector<float> WienerNoiseReducer::process(
    const QVector<float> &enhancedA,
    const QVector<float> &suppressionB1,
    const QVector<float> &suppressionB2)
{
    const int sampleCount = std::min(
        {enhancedA.size(), suppressionB1.size(), suppressionB2.size()});
    if (sampleCount <= 0) {
        return {};
    }
    m_enhancedInput.reserve(m_enhancedInput.size() + sampleCount);
    m_b1Input.reserve(m_b1Input.size() + sampleCount);
    m_b2Input.reserve(m_b2Input.size() + sampleCount);
    for (int index = 0; index < sampleCount; ++index) {
        m_enhancedInput.append(std::clamp(enhancedA.at(index), -1.0f, 1.0f));
        m_b1Input.append(std::clamp(suppressionB1.at(index), -1.0f, 1.0f));
        m_b2Input.append(std::clamp(suppressionB2.at(index), -1.0f, 1.0f));
    }

    QVector<float> output;
    while (m_enhancedInput.size() >= FftSize
           && m_b1Input.size() >= FftSize
           && m_b2Input.size() >= FftSize) {
        output += processBlock();
        m_enhancedInput.erase(m_enhancedInput.begin(),
                              m_enhancedInput.begin() + HopSize);
        m_b1Input.erase(m_b1Input.begin(), m_b1Input.begin() + HopSize);
        m_b2Input.erase(m_b2Input.begin(), m_b2Input.begin() + HopSize);
    }
    return output;
}

void WienerNoiseReducer::initializeWindow()
{
    m_window.resize(FftSize);
    for (int index = 0; index < FftSize; ++index) {
        const double hann =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * index / FftSize);
        m_window[index] = std::sqrt(std::max(0.0, hann));
    }
}

QVector<float> WienerNoiseReducer::processBlock()
{
    QVector<Complex> enhanced(FftSize);
    QVector<Complex> b1(FftSize);
    QVector<Complex> b2(FftSize);
    for (int index = 0; index < FftSize; ++index) {
        const double window = m_window.at(index);
        enhanced[index] = Complex(m_enhancedInput.at(index) * window, 0.0);
        b1[index] = Complex(m_b1Input.at(index) * window, 0.0);
        b2[index] = Complex(m_b2Input.at(index) * window, 0.0);
    }
    if (!Radix2Fft::transform(&enhanced, false)
        || !Radix2Fft::transform(&b1, false)
        || !Radix2Fft::transform(&b2, false)) {
        return {};
    }

    if (m_algorithm == NoiseReductionAlgorithm::AdaptiveWiener) {
        updateStatistics(enhanced, b1, b2);
    }
    QVector<Complex> reduced(FftSize);
    for (int bin = 0; bin < FftSize; ++bin) {
        if (m_algorithm == NoiseReductionAlgorithm::FixedParameterWiener) {
            reduced[bin] =
                fixedOutput(enhanced.at(bin), b1.at(bin), b2.at(bin));
        } else {
            reduced[bin] = m_mode == NoiseReductionMode::TwoPoint
                ? twoPointOutput(
                      bin, enhanced.at(bin), b1.at(bin), b2.at(bin))
                : singlePointOutput(bin, enhanced.at(bin), b1.at(bin));
        }
    }
    if (!Radix2Fft::transform(&reduced, true)) {
        return {};
    }

    QVector<float> output;
    output.reserve(HopSize);
    QVector<double> nextOverlap(HopSize);
    for (int index = 0; index < HopSize; ++index) {
        const double first =
            finiteComplex(reduced.at(index))
            ? reduced.at(index).real() * m_window.at(index)
            : 0.0;
        const double second =
            finiteComplex(reduced.at(index + HopSize))
            ? reduced.at(index + HopSize).real()
                * m_window.at(index + HopSize)
            : 0.0;
        output.append(static_cast<float>(
            std::clamp(first + m_overlap.at(index), -1.0, 1.0)));
        nextOverlap[index] = second;
    }
    m_overlap = std::move(nextOverlap);
    return output;
}

WienerNoiseReducer::Complex WienerNoiseReducer::fixedOutput(
    const Complex &enhanced,
    const Complex &suppressionB1,
    const Complex &suppressionB2) const
{
    const double enhancedPower = std::norm(enhanced);
    const double b1Coefficient =
        m_mode == NoiseReductionMode::TwoPoint
        ? m_dualB1SuppressionCoefficient
        : m_singleSuppressionCoefficient;
    const double b2Coefficient =
        m_mode == NoiseReductionMode::TwoPoint
        ? m_dualB2SuppressionCoefficient : 0.0;
    const double denominator =
        enhancedPower
        + b1Coefficient * std::norm(suppressionB1)
        + b2Coefficient * std::norm(suppressionB2);
    if (!std::isfinite(denominator)
        || denominator <= std::numeric_limits<double>::epsilon()) {
        return {};
    }
    const double gain = enhancedPower / denominator;
    return finiteOrZero(enhanced * gain);
}

void WienerNoiseReducer::updateStatistics(
    const QVector<Complex> &enhanced,
    const QVector<Complex> &suppressionB1,
    const QVector<Complex> &suppressionB2)
{
    const double previousWeight =
        m_statisticsInitialized ? SpectralSmoothing : 0.0;
    const double currentWeight = 1.0 - previousWeight;
    for (int bin = 0; bin < FftSize; ++bin) {
        const Complex a = enhanced.at(bin);
        const Complex b1 = suppressionB1.at(bin);
        const Complex b2 = suppressionB2.at(bin);
        m_b1Power[bin] =
            previousWeight * m_b1Power.at(bin)
            + currentWeight * std::norm(b1);
        m_b2Power[bin] =
            previousWeight * m_b2Power.at(bin)
            + currentWeight * std::norm(b2);
        m_b1B2Cross[bin] =
            previousWeight * m_b1B2Cross.at(bin)
            + currentWeight * b1 * std::conj(b2);
        m_enhancedB1Cross[bin] =
            previousWeight * m_enhancedB1Cross.at(bin)
            + currentWeight * a * std::conj(b1);
        m_enhancedB2Cross[bin] =
            previousWeight * m_enhancedB2Cross.at(bin)
            + currentWeight * a * std::conj(b2);
    }
    m_statisticsInitialized = true;
}

WienerNoiseReducer::Complex WienerNoiseReducer::singlePointOutput(
    int bin, const Complex &enhanced, const Complex &suppressionB1) const
{
    const double power = std::max(0.0, m_b1Power.at(bin));
    const double regularization =
        AbsoluteRegularization + RelativeRegularization * power;
    Complex filter =
        m_enhancedB1Cross.at(bin) / (power + regularization);
    filter = limitMagnitude(finiteOrZero(filter), MaximumFilterMagnitude);
    const Complex estimate = filter * suppressionB1;
    return finiteOrZero(enhanced - estimate);
}

WienerNoiseReducer::Complex WienerNoiseReducer::twoPointOutput(
    int bin,
    const Complex &enhanced,
    const Complex &suppressionB1,
    const Complex &suppressionB2) const
{
    const double totalPower =
        std::max(0.0, m_b1Power.at(bin))
        + std::max(0.0, m_b2Power.at(bin));
    const double regularization =
        AbsoluteRegularization + RelativeRegularization * totalPower;
    const double r11 = m_b1Power.at(bin) + regularization;
    const double r22 = m_b2Power.at(bin) + regularization;
    const Complex r12 = m_b1B2Cross.at(bin);
    const double determinant =
        r11 * r22 - std::norm(r12);
    if (!std::isfinite(determinant)
        || determinant <= AbsoluteRegularization * AbsoluteRegularization) {
        return singlePointOutput(bin, enhanced, suppressionB1);
    }

    const Complex p1 = m_enhancedB1Cross.at(bin);
    const Complex p2 = m_enhancedB2Cross.at(bin);
    Complex h1 = (p1 * r22 - p2 * std::conj(r12)) / determinant;
    Complex h2 = (p2 * r11 - p1 * r12) / determinant;
    h1 = limitMagnitude(finiteOrZero(h1), MaximumFilterMagnitude);
    h2 = limitMagnitude(finiteOrZero(h2), MaximumFilterMagnitude);
    const Complex estimate = h1 * suppressionB1 + h2 * suppressionB2;
    return finiteOrZero(enhanced - estimate);
}

WienerNoiseReducer::Complex WienerNoiseReducer::finiteOrZero(
    const Complex &value)
{
    return finiteComplex(value) ? value : Complex {};
}

} // namespace MicArray
