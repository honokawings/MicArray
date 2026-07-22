#pragma once

#include "media/channelgainprocessor.h"

#include <QMetaType>
#include <QObject>
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
    GainConfiguration gainConfiguration() const;

    void saveConnection(const ConnectionSettings &settings);
    void saveAudio(const AudioSettings &settings);
    void saveVideo(const VideoSettings &settings);
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
