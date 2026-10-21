#pragma once

#include <QMetaType>
#include <QPointF>
#include <QString>
#include <QVector>

#include <array>

namespace MicArray {

struct MicrophonePosition {
    double xMeters = 0.0;
    double yMeters = 0.0;
    double zMeters = 0.0;

    bool operator==(const MicrophonePosition &) const = default;
};

struct LocatorDescriptor {
    QString id;
    QString displayName;
    QString description;
};

enum class SpatialResolutionMode {
    RealtimeEnhanced,
    HighPrecision,
};

struct LocatorConfiguration {
    QVector<MicrophonePosition> microphones;
    int sampleRate = 48000;
    int windowSamples = 2048;
    double soundSpeedMetersPerSecond = 343.0;
    double confidenceThreshold = 0.45;
    double smoothingFactor = 0.35;
    SpatialResolutionMode spatialResolutionMode =
        SpatialResolutionMode::RealtimeEnhanced;
    bool neuralPeakEnhancementEnabled = true;
    double neuralPeakEnhancementStrength = 0.30;
};

struct MultiChannelAudioWindow {
    QVector<QVector<float>> channels;
    qint64 centerSample = 0;
    int sampleRate = 48000;

    bool operator==(const MultiChannelAudioWindow &) const = default;
};

struct SoundSourceDirection {
    double azimuthDegrees = 0.0;
    double elevationDegrees = 0.0;
    double confidence = 0.0;
    double residualSamples = 0.0;
    int validPairCount = 0;
    qint64 centerSample = 0;
    int sourceId = -1;
};

struct CameraLensCalibration {
    bool enabled = false;
    // Intrinsics in full-video normalized coordinates, excluding letterboxing.
    double fx = 0.0;
    double fy = 0.0;
    double cx = 0.5;
    double cy = 0.5;
    std::array<double, 4> coefficients {};
    int referenceWidth = 0;
    int referenceHeight = 0;

    bool isValid() const;
    bool operator==(const CameraLensCalibration &) const = default;
};

struct CameraViewSettings {
    double centerAzimuthDegrees = 0.0;
    double centerElevationDegrees = 0.0;
    double horizontalFovDegrees = 90.0;
    double verticalFovDegrees = 60.0;
    double rollDegrees = 0.0;
    CameraLensCalibration lens;

    bool isValid() const;
    bool operator==(const CameraViewSettings &) const = default;
};

struct ScanDirection {
    double azimuthDegrees = 0.0;
    double elevationDegrees = 0.0;

    bool operator==(const ScanDirection &) const = default;
};

struct SpatialResponseGrid {
    int columns = 0;
    int rows = 0;
    QVector<ScanDirection> directions;
};

struct SpatialResponseResult {
    QVector<float> values;
    double reliability = 0.0;
    bool hasRefinedPeak = false;
    ScanDirection refinedPeak;
    ScanDirection physicalPeak;
    double azimuthStandardDeviationDegrees = 0.0;
    double elevationStandardDeviationDegrees = 0.0;
    double azimuthElevationCorrelation = 0.0;
    double peakToSidelobeRatio = 0.0;
    bool neuralPeakAttempted = false;
    bool neuralPeakApplied = false;
    double neuralPeakConfidence = 0.0;
    double neuralAzimuthCorrectionDegrees = 0.0;
    double neuralElevationCorrectionDegrees = 0.0;
};

struct LiveLocalizationResult {
    QVector<SoundSourceDirection> directions;
    qint64 timestampMicroseconds = 0;
    qint64 elapsedMilliseconds = 0;
    quint64 generation = 0;
};

struct LiveSpatialResponse {
    int columns = 0;
    int rows = 0;
    QVector<float> normalizedValues;
    qint64 timestampMicroseconds = 0;
    quint64 generation = 0;
    double reliability = 0.0;
    bool trustedDirection = false;
    bool hasRefinedPeak = false;
    ScanDirection refinedPeak;
    ScanDirection physicalPeak;
    double azimuthStandardDeviationDegrees = 0.0;
    double elevationStandardDeviationDegrees = 0.0;
    double azimuthElevationCorrelation = 0.0;
    double peakToSidelobeRatio = 0.0;
    bool neuralPeakAttempted = false;
    bool neuralPeakApplied = false;
    double neuralPeakConfidence = 0.0;
    double neuralAzimuthCorrectionDegrees = 0.0;
    double neuralElevationCorrectionDegrees = 0.0;
};

struct VideoCalibrationSample {
    ScanDirection direction;
    QPointF normalizedVideoPoint;
    double confidence = 0.0;
    double angularDeviationDegrees = 0.0;

    bool operator==(const VideoCalibrationSample &) const = default;
};

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::LocatorConfiguration)
Q_DECLARE_METATYPE(MicArray::SpatialResolutionMode)
Q_DECLARE_METATYPE(MicArray::MultiChannelAudioWindow)
Q_DECLARE_METATYPE(MicArray::SoundSourceDirection)
Q_DECLARE_METATYPE(QVector<MicArray::SoundSourceDirection>)
Q_DECLARE_METATYPE(MicArray::SpatialResponseGrid)
Q_DECLARE_METATYPE(MicArray::LiveLocalizationResult)
Q_DECLARE_METATYPE(MicArray::LiveSpatialResponse)
Q_DECLARE_METATYPE(MicArray::VideoCalibrationSample)
