#pragma once

#include "analysis/localizationtypes.h"
#include "media/channelgainprocessor.h"
#include "media/noisereductiontypes.h"

#include <QMetaType>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVector>

#include <memory>

class QSettings;

namespace MicArray {

struct ConnectionSettings {
    QString host = QStringLiteral("192.168.2.10");
    quint16 port = 9968;
    bool autoReconnect = true;
};

struct AudioSettings {
    int sampleRate = 48000;
    int channelCount = 48;
    int outputChannel = 0;
    int averageWindowFrames = 100;
    bool playbackEnabled = false;
};

struct VideoSettings {
    QString format = QStringLiteral("JPG");
};

struct AppearanceSettings {
    QString theme = QStringLiteral("light");

    bool operator==(const AppearanceSettings &) const = default;
};

struct AudioAnalysisSettings {
    QString lastWavePath;
    QString lastPhotoPath;
    int outputChannel = 0;
    double volume = 1.0;
    QString locatorId = QStringLiteral("gcc-phat-tdoa");
    int windowSamples = 2048;
    int updateIntervalMs = 50;
    double soundSpeedMetersPerSecond = 343.0;
    double smoothingFactor = 0.35;
    double confidenceThreshold = 0.45;
    bool neuralPeakEnhancementEnabled = true;
    double neuralPeakEnhancementStrength = 0.30;
    CameraViewSettings camera;

    bool operator==(const AudioAnalysisSettings &) const = default;
};

struct LiveLocalizationSettings {
    bool enabled = true;
    QString localizationSource = QStringLiteral("pl");
    QString locatorId = QStringLiteral("gcc-phat-tdoa");
    QString displayMode = QStringLiteral("marker");
    QString plDisplayMode = QStringLiteral("plheatmap");
    SpatialResolutionMode spatialResolutionMode =
        SpatialResolutionMode::RealtimeEnhanced;
    double heatmapOpacity = 0.45;
    double heatmapFocusExponent = 1.5;
    int heatmapGridColumns = 64;
    int heatmapGridRows = 36;
    int windowSamples = 2048;
    int updateIntervalMs = 100;
    double soundSpeedMetersPerSecond = 343.0;
    double smoothingFactor = 0.35;
    double confidenceThreshold = 0.40;
    bool neuralPeakEnhancementEnabled = true;
    double neuralPeakEnhancementStrength = 0.30;
    CameraViewSettings camera;
    bool showCalibrationOverlay = false;
    QSize calibrationReferenceVideoSize = {0, 0};
    QVector<VideoCalibrationSample> calibrationSamples;
    double calibrationRmsPixels = 0.0;
    int calibrationVersion = 1;

    bool operator==(const LiveLocalizationSettings &) const = default;
};

struct ControlPreset {
    QString name;
    QVector<qint16> values;

    bool operator==(const ControlPreset &) const = default;
};

class AppSettings : public QObject
{
    Q_OBJECT

public:
    explicit AppSettings(QObject *parent = nullptr);
    explicit AppSettings(const QString &fileName, QObject *parent = nullptr);
    ~AppSettings() override;

    ConnectionSettings connection() const;
    AudioSettings audio() const;
    VideoSettings video() const;
    AppearanceSettings appearance() const;
    AudioAnalysisSettings audioAnalysis() const;
    LiveLocalizationSettings liveLocalization() const;
    NoiseReductionSettings noiseReduction() const;
    GainConfiguration gainConfiguration() const;

    void saveConnection(const ConnectionSettings &settings);
    void saveAudio(const AudioSettings &settings);
    void saveVideo(const VideoSettings &settings);
    void saveAppearance(const AppearanceSettings &settings);
    void saveAudioAnalysis(const AudioAnalysisSettings &settings);
    void saveLiveLocalization(const LiveLocalizationSettings &settings);
    void saveNoiseReduction(const NoiseReductionSettings &settings);
    bool saveGainConfiguration(const GainConfiguration &configuration, QString *error = nullptr);

    QList<ControlPreset> controlPresets() const;
    bool saveControlPreset(const ControlPreset &preset);
    bool removeControlPreset(const QString &name);
    void resetAll();

    static constexpr int SchemaVersion = 1;

private:
    enum class SettingsBackend {
        Native,
        IniFile,
    };

    std::unique_ptr<QSettings> createSettingsBackend() const;
    void captureSettingsBackendIdentity();
    void resetSettingsBackend();
    void initializeSchema();
    void writeControlPresets(const QList<ControlPreset> &presets);

    SettingsBackend m_settingsBackend = SettingsBackend::Native;
    QString m_settingsFileName;
    int m_settingsFormat = 0;
    int m_settingsScope = 0;
    QString m_settingsOrganization;
    QString m_settingsApplication;
    bool m_fallbacksEnabled = true;
    bool m_atomicSyncRequired = true;
    std::unique_ptr<QSettings> m_settings;
};

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::AudioSettings)
Q_DECLARE_METATYPE(MicArray::AudioAnalysisSettings)
Q_DECLARE_METATYPE(MicArray::LiveLocalizationSettings)
