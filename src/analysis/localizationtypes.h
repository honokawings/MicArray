#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

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

struct LocatorConfiguration {
    QVector<MicrophonePosition> microphones;
    int sampleRate = 48000;
    int windowSamples = 2048;
    double soundSpeedMetersPerSecond = 343.0;
    double confidenceThreshold = 0.45;
    double smoothingFactor = 0.35;
};

struct MultiChannelAudioWindow {
    QVector<QVector<float>> channels;
    qint64 centerSample = 0;
    int sampleRate = 48000;
};

struct SoundSourceDirection {
    double azimuthDegrees = 0.0;
    double elevationDegrees = 0.0;
    double confidence = 0.0;
    double residualSamples = 0.0;
    int validPairCount = 0;
    qint64 centerSample = 0;
};

struct CameraViewSettings {
    double centerAzimuthDegrees = 0.0;
    double centerElevationDegrees = 0.0;
    double horizontalFovDegrees = 90.0;
    double verticalFovDegrees = 60.0;
    double rollDegrees = 0.0;

    bool isValid() const;
    bool operator==(const CameraViewSettings &) const = default;
};

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::LocatorConfiguration)
Q_DECLARE_METATYPE(MicArray::MultiChannelAudioWindow)
Q_DECLARE_METATYPE(MicArray::SoundSourceDirection)
Q_DECLARE_METATYPE(QVector<MicArray::SoundSourceDirection>)
