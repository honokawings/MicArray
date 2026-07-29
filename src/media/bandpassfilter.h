#pragma once

#include <QVector>

namespace MicArray {

class BandPassFilter
{
public:
    bool configure(int sampleRate,
                   double lowCutoffHz,
                   double highCutoffHz);
    void reset();
    QVector<float> process(const QVector<float> &samples);

private:
    struct Biquad
    {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double delay1 = 0.0;
        double delay2 = 0.0;

        double process(double sample);
        void reset();
    };

    static Biquad highPass(int sampleRate, double cutoffHz);
    static Biquad lowPass(int sampleRate, double cutoffHz);
    static Biquad normalized(double b0,
                             double b1,
                             double b2,
                             double a0,
                             double a1,
                             double a2);

    Biquad m_highPass;
    Biquad m_lowPass;
    bool m_configured = false;
};

} // namespace MicArray
