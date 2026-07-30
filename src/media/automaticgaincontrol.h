#pragma once

#include "media/noisereductiontypes.h"

#include <QVector>

namespace MicArray {

class AutomaticGainControl
{
public:
    bool configure(
        int sampleRate,
        const AutomaticGainControlSettings &settings);
    void reset();
    QVector<float> process(const QVector<float> &samples);

private:
    AutomaticGainControlSettings m_settings;
    int m_sampleRate = 0;
    bool m_configured = false;
    double m_attackCoefficient = 0.0;
    double m_releaseCoefficient = 0.0;
    double m_envelopePower = 0.0;
    double m_targetAmplitude = 0.0;
    double m_minimumGain = 0.0;
    double m_maximumGain = 1.0;
    double m_limiterAmplitude = 1.0;
    double m_gain = 1.0;
};

} // namespace MicArray
