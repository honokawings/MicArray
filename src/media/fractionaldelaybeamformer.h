#pragma once

#include "media/noisereductiontypes.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <array>

namespace MicArray {

struct BeamformedDirectionFrame {
    QVector<float> firstElement;
    QVector<float> enhancedA;
    QVector<float> suppressionB1;
    QVector<float> suppressionB2;
    bool valid = false;
    QString error;
};

class FractionalDelayBeamformer
{
public:
    void configure(int sampleRate,
                   const ContinuousDirectionTarget &enhancedA,
                   const ContinuousDirectionTarget &suppressionB1,
                   const ContinuousDirectionTarget &suppressionB2);
    BeamformedDirectionFrame process(const QByteArray &payload);
    void reset();

private:
    struct DelayTap {
        int integerDelay = 0;
        std::array<double, 4> coefficients {};
    };

    using DirectionDelayTaps = std::array<DelayTap, 48>;
    using HistorySample = std::array<float, 48>;

    void initializeForSampleRate(int sampleRate);
    DirectionDelayTaps tapsForDirection(
        const ContinuousDirectionTarget &target) const;
    float delayedSample(int channel, const DelayTap &tap) const;
    float beamformedSample(const DirectionDelayTaps &taps) const;

    int m_sampleRate = 48000;
    double m_referenceDelaySamples = 0.0;
    int m_startupSamples = 0;
    int m_writeIndex = 0;
    qint64 m_totalSamples = 0;
    bool m_configured = false;
    QVector<HistorySample> m_history;
    std::array<DirectionDelayTaps, 3> m_directionTaps;
};

} // namespace MicArray
