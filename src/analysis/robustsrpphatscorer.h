#pragma once

#include "analysis/localizationtypes.h"

#include <QVector3D>

#include <complex>

namespace MicArray {

struct RobustSpatialEstimate {
    bool hasDirection = false;
    SoundSourceDirection direction;
    QVector<float> gridValues;
    double reliability = 0.0;
    double peakToSidelobeRatio = 0.0;
    double coherentCoverage = 0.0;
    double steeredChannelCoherence = 0.0;
    double peakContrast = 0.0;
    double curvatureScore = 0.0;
    double azimuthStandardDeviationDegrees = 0.0;
    double elevationStandardDeviationDegrees = 0.0;
    double azimuthElevationCorrelation = 0.0;
    int validChannelCount = 0;
    int validBaselineGroupCount = 0;
};

class RobustSrpPhatScorer
{
public:
    bool configure(const LocatorConfiguration &configuration,
                   QString *errorMessage = nullptr);
    void prepareGrid(const SpatialResponseGrid &grid);
    bool prepareWindow(const MultiChannelAudioWindow &window);
    RobustSpatialEstimate estimate(
        const SpatialResponseGrid *grid = nullptr);
    void reset();

    int baselineGroupCount() const;
    int realtimeBaselineGroupCount() const;

private:
    struct MicrophonePair {
        int firstChannel = 0;
        int secondChannel = 0;
    };

    struct BaselineGroup {
        QVector3D displacement;
        QVector<MicrophonePair> pairs;
        QVector<std::complex<double>> correlation;
        double reliableBinFraction = 0.0;
        bool canBlendPreviousCorrelation = false;
        bool valid = false;
    };

    struct GridSteering {
        QVector<quint16> lowerIndices;
        QVector<float> fractions;
    };

    bool preparedGridMatches(const SpatialResponseGrid &grid) const;
    bool buildChannelSpectra(const MultiChannelAudioWindow &window);
    bool buildGroupedCorrelations();
    void clearTemporalCorrelations();
    double scoreDirectionCubic(
        const ScanDirection &direction,
        const QVector<int> &groupIndices) const;
    double scoreDirectionSinc(
        const ScanDirection &direction,
        const QVector<int> &groupIndices) const;
    double steeredChannelCoherence(
        const ScanDirection &direction) const;
    bool refineContinuousPeak(
        const QVector<int> &seedGroupIndices,
        RobustSpatialEstimate *result) const;

    LocatorConfiguration m_configuration;
    QVector<BaselineGroup> m_baselineGroups;
    QVector<int> m_realtimeGroupIndices;
    QVector<int> m_validGroupIndices;
    QVector<QVector<std::complex<double>>> m_channelUnitSpectra;
    QVector<QVector<std::complex<double>>>
        m_smoothedChannelUnitSpectra;
    QVector<double> m_channelQualities;
    SpatialResponseGrid m_preparedGrid;
    QVector<GridSteering> m_gridSteering;
    QVector<quint8> m_validGridDirections;
    bool m_configured = false;
    bool m_windowPrepared = false;
    bool m_hasContinuousEstimate = false;
    bool m_hasPreviousCorrelations = false;
    int m_validChannelCount = 0;
    qint64 m_centerSample = 0;
    RobustSpatialEstimate m_continuousEstimate;
};

} // namespace MicArray
