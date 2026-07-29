#pragma once

#include "media/noisereductiontypes.h"

#include <QVector>

#include <complex>

namespace MicArray {

class WienerNoiseReducer
{
public:
    static constexpr int FftSize = 1024;
    static constexpr int HopSize = FftSize / 2;

    WienerNoiseReducer();

    void setAlgorithm(NoiseReductionAlgorithm algorithm);
    NoiseReductionAlgorithm algorithm() const { return m_algorithm; }
    void setMode(NoiseReductionMode mode);
    NoiseReductionMode mode() const { return m_mode; }
    void setSuppressionCoefficients(double single,
                                    double dualB1,
                                    double dualB2);
    void reset();
    QVector<float> process(const QVector<float> &enhancedA,
                           const QVector<float> &suppressionB1,
                           const QVector<float> &suppressionB2);

private:
    using Complex = std::complex<double>;

    void initializeWindow();
    QVector<float> processBlock();
    void updateStatistics(const QVector<Complex> &enhanced,
                          const QVector<Complex> &suppressionB1,
                          const QVector<Complex> &suppressionB2);
    Complex singlePointOutput(int bin,
                              const Complex &enhanced,
                              const Complex &suppressionB1) const;
    Complex twoPointOutput(int bin,
                           const Complex &enhanced,
                           const Complex &suppressionB1,
                           const Complex &suppressionB2) const;
    Complex fixedOutput(const Complex &enhanced,
                        const Complex &suppressionB1,
                        const Complex &suppressionB2) const;
    static Complex finiteOrZero(const Complex &value);

    NoiseReductionAlgorithm m_algorithm =
        NoiseReductionAlgorithm::FixedParameterWiener;
    NoiseReductionMode m_mode = NoiseReductionMode::SinglePoint;
    double m_singleSuppressionCoefficient = 3.0;
    double m_dualB1SuppressionCoefficient = 5.0;
    double m_dualB2SuppressionCoefficient = 5.0;
    QVector<double> m_window;
    QVector<float> m_enhancedInput;
    QVector<float> m_b1Input;
    QVector<float> m_b2Input;
    QVector<double> m_overlap;
    QVector<double> m_b1Power;
    QVector<double> m_b2Power;
    QVector<Complex> m_b1B2Cross;
    QVector<Complex> m_enhancedB1Cross;
    QVector<Complex> m_enhancedB2Cross;
    bool m_statisticsInitialized = false;
};

} // namespace MicArray
