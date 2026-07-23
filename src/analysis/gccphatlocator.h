#pragma once

#include "analysis/isoundsourcelocator.h"
#include "analysis/ispatialresponseprovider.h"

#include <QThreadPool>
#include <QVector3D>

#include <complex>
#include <limits>
#include <memory>

namespace MicArray {

class GccPhatLocator final : public ISoundSourceLocator,
                             public ISpatialResponseProvider
{
public:
    ~GccPhatLocator() override;

    LocatorDescriptor descriptor() const override;
    bool configure(const LocatorConfiguration &configuration,
                   QString *errorMessage = nullptr) override;
    QVector<SoundSourceDirection> locate(const MultiChannelAudioWindow &window) override;
    void prepareSpatialResponseGrid(const SpatialResponseGrid &grid) override;
    SpatialResponseResult evaluateSpatialResponse(
        const MultiChannelAudioWindow &window,
        const SpatialResponseGrid &grid) override;

private:
    struct Pair {
        int first = 0;
        int second = 0;
        double dxMeters = 0.0;
        double dyMeters = 0.0;
        double distanceMeters = 0.0;
        int groupIndex = -1;
    };

    struct BaselineGroup {
        double dxMeters = 0.0;
        double dyMeters = 0.0;
        int pairCount = 0;
    };

    struct SteeringGroup {
        QVector<quint16> firstIndices;
        QVector<quint16> secondIndices;
        QVector<float> fractions;
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
    void invalidateSpatialCaches();
    bool buildGroupedCorrelations(
        const QVector<QVector<std::complex<double>>> &spectra,
        const MultiChannelAudioWindow &window);
    bool finalizeGroupedCorrelations(
        QVector<QVector<std::complex<double>>> *groupSpectra,
        const MultiChannelAudioWindow &window);
    bool groupedCorrelationCacheMatches(
        const MultiChannelAudioWindow &window) const;
    bool preparedGridMatches(const SpatialResponseGrid &grid) const;
    QThreadPool *spatialThreadPool();
    bool solveDirection(QVector<Measurement> *measurements,
                        double *ux,
                        double *uy) const;

    LocatorConfiguration m_configuration;
    QVector<Pair> m_pairs;
    QVector<BaselineGroup> m_baselineGroups;
    QVector<QVector<std::complex<double>>> m_groupedCorrelations;
    QVector<SteeringGroup> m_steeringGroups;
    QVector<quint8> m_validSteeringDirections;
    SpatialResponseGrid m_preparedGrid;
    qint64 m_cachedCenterSample = std::numeric_limits<qint64>::min();
    int m_cachedSampleRate = 0;
    int m_cachedSampleCount = 0;
    int m_cachedChannelCount = 0;
    const float *m_cachedFirstChannelData = nullptr;
    std::unique_ptr<QThreadPool> m_spatialThreadPool;
    QVector3D m_smoothedDirection;
    bool m_configured = false;
    bool m_hasSmoothedDirection = false;
};

} // namespace MicArray
