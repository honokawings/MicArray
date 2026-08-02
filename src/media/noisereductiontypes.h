#pragma once

#include "analysis/localizationtypes.h"

#include <QMetaType>
#include <QString>
#include <QVector>

namespace MicArray {

enum class NoiseReductionAlgorithm {
    FixedParameterWiener,
    AdaptiveWiener,
};

enum class NoiseReductionMode {
    SinglePoint,
    TwoPoint,
};

enum class NoiseSignalSource {
    FirstElement,
    EnhancedA,
    SuppressionB1,
    SuppressionB2,
    Denoised,
};

enum class NoiseDirectionSelection {
    EnhancedA,
    SuppressionB1,
    SuppressionB2,
};

enum class BandPassPreset {
    Voice,
    Music,
    Custom,
};

enum class NoisePostProcessingNode {
    BandPass,
    LogMmse,
    AutomaticGainControl,
};

enum class NoiseReductionDataFlowStage {
    Suppression,
    BandPass,
    LogMmse,
    AutomaticGainControl,
};

struct NoiseReductionStageSamples {
    NoiseReductionDataFlowStage stage;
    QVector<float> samples;
};

struct NoiseReductionRecordingFrame {
    QVector<NoiseReductionStageSamples> stages;
    int sampleRate;
    quint32 timestampSeconds;
    quint32 timestampMicros;
};

struct BandPassFilterSettings {
    bool enabled = false;
    BandPassPreset preset = BandPassPreset::Voice;
    double lowCutoffHz = 100.0;
    double highCutoffHz = 8000.0;

    bool operator==(const BandPassFilterSettings &) const = default;
};

struct LogMmseSettings {
    bool enabled = false;
    int fftSize = 512;
    double overlapRatio = 0.5;
    double priorSnrSmoothing = 0.98;
    double noisePowerSmoothing = 0.95;
    double minimumGainDb = -20.0;

    bool operator==(const LogMmseSettings &) const = default;
};

struct AutomaticGainControlSettings {
    bool enabled = false;
    double targetLevelDbfs = -18.0;
    double maximumGainDb = 30.0;
    double maximumAttenuationDb = -20.0;
    double attackMilliseconds = 20.0;
    double releaseMilliseconds = 300.0;
    double limiterLevelDbfs = -1.0;

    bool operator==(const AutomaticGainControlSettings &) const = default;
};

struct ContinuousDirectionTarget {
    double azimuthDegrees = 0.0;
    double elevationDegrees = 0.0;

    bool operator==(const ContinuousDirectionTarget &) const = default;
};

struct DirectionControlMapping {
    double azimuthMinimumDegrees = -90.0;
    double azimuthMaximumDegrees = 90.0;
    double elevationMinimumDegrees = -90.0;
    double elevationMaximumDegrees = 90.0;

    bool isValid() const;
    bool operator==(const DirectionControlMapping &) const = default;
};

struct NoiseReductionSettings {
    bool enabled = false;
    NoiseReductionAlgorithm algorithm =
        NoiseReductionAlgorithm::FixedParameterWiener;
    NoiseReductionMode mode = NoiseReductionMode::SinglePoint;
    double singleSuppressionCoefficient = 3.0;
    double dualB1SuppressionCoefficient = 5.0;
    double dualB2SuppressionCoefficient = 5.0;
    bool monitorEnabled = true;
    int monitorBufferMilliseconds = 250;
    NoiseSignalSource monitorSource = NoiseSignalSource::Denoised;
    double monitorGainDb = 0.0;
    NoiseSignalSource firstPlotSource = NoiseSignalSource::EnhancedA;
    NoiseSignalSource secondPlotSource = NoiseSignalSource::Denoised;
    BandPassFilterSettings bandPass;
    QVector<NoisePostProcessingNode> postProcessingOrder {
        NoisePostProcessingNode::BandPass,
        NoisePostProcessingNode::LogMmse,
        NoisePostProcessingNode::AutomaticGainControl,
    };
    LogMmseSettings logMmse;
    AutomaticGainControlSettings automaticGainControl;
    int controlTabIndex = 0;
    int selectedDataFlowNode = -1;
    bool showMarkers = true;
    ContinuousDirectionTarget enhancedA;
    ContinuousDirectionTarget suppressionB1;
    ContinuousDirectionTarget suppressionB2;
    DirectionControlMapping mapping;
    CameraViewSettings camera {0.0, 0.0, 180.0, 180.0, 0.0};

    bool operator==(const NoiseReductionSettings &) const = default;
};

struct NoiseReductionPlotSeries {
    NoiseSignalSource source = NoiseSignalSource::EnhancedA;
    QVector<float> waveform;
    QVector<float> spectrumDbfs;
};

struct NoiseReductionPlotFrame {
    NoiseReductionPlotSeries first;
    NoiseReductionPlotSeries second;
    int sampleRate = 48000;
    qint64 timestampMicroseconds = 0;
};

QString noiseSignalSourceName(NoiseSignalSource source);
QString noiseReductionDataFlowStageName(NoiseReductionDataFlowStage stage);
QString noiseReductionDataFlowStageKey(NoiseReductionDataFlowStage stage);
QVector<NoisePostProcessingNode> normalizedPostProcessingOrder(
    const QVector<NoisePostProcessingNode> &order);

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::NoiseReductionMode)
Q_DECLARE_METATYPE(MicArray::NoiseReductionAlgorithm)
Q_DECLARE_METATYPE(MicArray::NoiseSignalSource)
Q_DECLARE_METATYPE(MicArray::NoiseDirectionSelection)
Q_DECLARE_METATYPE(MicArray::NoisePostProcessingNode)
Q_DECLARE_METATYPE(MicArray::NoiseReductionDataFlowStage)
Q_DECLARE_METATYPE(MicArray::NoiseReductionStageSamples)
Q_DECLARE_METATYPE(MicArray::NoiseReductionRecordingFrame)
Q_DECLARE_METATYPE(MicArray::NoiseReductionSettings)
Q_DECLARE_METATYPE(MicArray::NoiseReductionPlotFrame)
