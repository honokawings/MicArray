#pragma once

#include "media/noisereductiontypes.h"

#include <QVector>

#include <complex>

namespace MicArray {

class LogMmseProcessor
{
public:
    bool configure(int sampleRate, const LogMmseSettings &settings);
    void reset();
    QVector<float> process(const QVector<float> &samples);

private:
    static double exponentialIntegralE1(double value);
    void processAvailableFrames();
    void processFrame();

    LogMmseSettings m_settings;
    int m_sampleRate = 0;
    int m_hopSize = 0;
    bool m_configured = false;
    bool m_noiseInitialized = false;
    QVector<double> m_window;
    QVector<float> m_inputQueue;
    QVector<float> m_outputQueue;
    QVector<double> m_overlapOutput;
    QVector<double> m_overlapNormalization;
    QVector<double> m_noisePower;
    QVector<double> m_previousPosteriorSnr;
    QVector<double> m_previousGain;
};

} // namespace MicArray

