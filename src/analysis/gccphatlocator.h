#pragma once

#include "analysis/isoundsourcelocator.h"

#include <QVector3D>

namespace MicArray {

class GccPhatLocator final : public ISoundSourceLocator
{
public:
    LocatorDescriptor descriptor() const override;
    bool configure(const LocatorConfiguration &configuration,
                   QString *errorMessage = nullptr) override;
    QVector<SoundSourceDirection> locate(const MultiChannelAudioWindow &window) override;

private:
    struct Pair {
        int first = 0;
        int second = 0;
        double dxMeters = 0.0;
        double dyMeters = 0.0;
        double distanceMeters = 0.0;
    };

    struct Measurement {
        double ax = 0.0;
        double ay = 0.0;
        double targetMeters = 0.0;
        double baseWeight = 0.0;
        double weight = 0.0;
        double residualMeters = 0.0;
    };

    void rebuildPairs();
    bool solveDirection(QVector<Measurement> *measurements,
                        double *ux,
                        double *uy) const;

    LocatorConfiguration m_configuration;
    QVector<Pair> m_pairs;
    QVector3D m_smoothedDirection;
    bool m_configured = false;
    bool m_hasSmoothedDirection = false;
};

} // namespace MicArray
