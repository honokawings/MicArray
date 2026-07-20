#include "app/appsettings.h"

#include <QSettings>
#include <QVariantList>

#include <algorithm>

namespace MicArray {

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
    , m_settings(std::make_unique<QSettings>())
{
    initializeSchema();
}

AppSettings::AppSettings(const QString &fileName, QObject *parent)
    : QObject(parent)
    , m_settings(std::make_unique<QSettings>(fileName, QSettings::IniFormat))
{
    initializeSchema();
}

AppSettings::~AppSettings() = default;

void AppSettings::initializeSchema()
{
    if (m_settings->value(QStringLiteral("schemaVersion"), 0).toInt() != SchemaVersion) {
        m_settings->setValue(QStringLiteral("schemaVersion"), SchemaVersion);
    }
}

ConnectionSettings AppSettings::connection() const
{
    ConnectionSettings value;
    const QString host = m_settings->value(QStringLiteral("connection/host"), value.host).toString().trimmed();
    const uint port = m_settings->value(QStringLiteral("connection/port"), value.port).toUInt();
    value.host = host.isEmpty() ? value.host : host;
    value.port = port >= 1 && port <= 65535 ? static_cast<quint16>(port) : value.port;
    value.autoReconnect = m_settings->value(QStringLiteral("connection/autoReconnect"), value.autoReconnect).toBool();
    return value;
}

AudioSettings AppSettings::audio() const
{
    AudioSettings value;
    const int sampleRate = m_settings->value(QStringLiteral("audio/sampleRate"), value.sampleRate).toInt();
    const int channelCount = m_settings->value(QStringLiteral("audio/channelCount"), value.channelCount).toInt();
    const int outputChannel = m_settings->value(QStringLiteral("audio/outputChannel"), value.outputChannel).toInt();
    const int averageWindowFrames = m_settings->value(QStringLiteral("audio/averageWindowFrames"), value.averageWindowFrames).toInt();
    value.sampleRate = sampleRate >= 8000 && sampleRate <= 192000 ? sampleRate : value.sampleRate;
    value.channelCount = channelCount >= 1 && channelCount <= 48 ? channelCount : value.channelCount;
    value.outputChannel = outputChannel >= 0 && outputChannel < value.channelCount ? outputChannel : value.outputChannel;
    value.averageWindowFrames = averageWindowFrames >= 1 && averageWindowFrames <= 10000 ? averageWindowFrames : value.averageWindowFrames;
    value.playbackEnabled = m_settings->value(QStringLiteral("audio/playbackEnabled"), value.playbackEnabled).toBool();
    return value;
}

VideoSettings AppSettings::video() const
{
    VideoSettings value;
    const QString format = m_settings->value(QStringLiteral("video/format"), value.format).toString().toUpper();
    value.format = format == QStringLiteral("JPG") || format == QStringLiteral("JPEG") ? QStringLiteral("JPG") : value.format;
    return value;
}

void AppSettings::saveConnection(const ConnectionSettings &settingsValue)
{
    ConnectionSettings value = settingsValue;
    value.host = value.host.trimmed();
    if (value.host.isEmpty()) {
        value.host = ConnectionSettings().host;
    }
    if (value.port == 0) {
        value.port = ConnectionSettings().port;
    }
    m_settings->setValue(QStringLiteral("connection/host"), value.host);
    m_settings->setValue(QStringLiteral("connection/port"), value.port);
    m_settings->setValue(QStringLiteral("connection/autoReconnect"), value.autoReconnect);
}

void AppSettings::saveAudio(const AudioSettings &settingsValue)
{
    AudioSettings value = settingsValue;
    value.sampleRate = std::clamp(value.sampleRate, 8000, 192000);
    value.channelCount = std::clamp(value.channelCount, 1, 48);
    value.outputChannel = std::clamp(value.outputChannel, 0, value.channelCount - 1);
    value.averageWindowFrames = std::clamp(value.averageWindowFrames, 1, 10000);
    m_settings->setValue(QStringLiteral("audio/sampleRate"), value.sampleRate);
    m_settings->setValue(QStringLiteral("audio/channelCount"), value.channelCount);
    m_settings->setValue(QStringLiteral("audio/outputChannel"), value.outputChannel);
    m_settings->setValue(QStringLiteral("audio/averageWindowFrames"), value.averageWindowFrames);
    m_settings->setValue(QStringLiteral("audio/playbackEnabled"), value.playbackEnabled);
}

void AppSettings::saveVideo(const VideoSettings &settingsValue)
{
    const QString format = settingsValue.format.toUpper();
    m_settings->setValue(QStringLiteral("video/format"), format == QStringLiteral("JPEG") ? QStringLiteral("JPG") : format);
}

QList<ControlPreset> AppSettings::controlPresets() const
{
    QList<ControlPreset> presets;
    const int size = m_settings->beginReadArray(QStringLiteral("controlPresets"));
    for (int index = 0; index < size; ++index) {
        m_settings->setArrayIndex(index);
        ControlPreset preset;
        preset.name = m_settings->value(QStringLiteral("name")).toString().trimmed();
        const QVariantList storedValues = m_settings->value(QStringLiteral("values")).toList();
        for (const QVariant &stored : storedValues) {
            preset.values.append(static_cast<qint16>(stored.toInt()));
        }
        if (!preset.name.isEmpty() && preset.values.size() == 8) {
            presets.append(preset);
        }
    }
    m_settings->endArray();
    return presets;
}

bool AppSettings::saveControlPreset(const ControlPreset &presetValue)
{
    ControlPreset preset = presetValue;
    preset.name = preset.name.trimmed();
    if (preset.name.isEmpty() || preset.name.size() > 32 || preset.values.size() != 8) {
        return false;
    }

    QList<ControlPreset> presets = controlPresets();
    const auto existing = std::find_if(presets.begin(), presets.end(), [&preset](const ControlPreset &item) {
        return item.name == preset.name;
    });
    if (existing == presets.end()) {
        presets.append(preset);
    } else {
        *existing = preset;
    }
    writeControlPresets(presets);
    return true;
}

bool AppSettings::removeControlPreset(const QString &nameValue)
{
    const QString name = nameValue.trimmed();
    QList<ControlPreset> presets = controlPresets();
    const qsizetype removed = presets.removeIf([&name](const ControlPreset &preset) {
        return preset.name == name;
    });
    if (removed == 0) {
        return false;
    }
    writeControlPresets(presets);
    return true;
}

void AppSettings::writeControlPresets(const QList<ControlPreset> &presets)
{
    m_settings->remove(QStringLiteral("controlPresets"));
    m_settings->beginWriteArray(QStringLiteral("controlPresets"));
    for (int index = 0; index < presets.size(); ++index) {
        m_settings->setArrayIndex(index);
        m_settings->setValue(QStringLiteral("name"), presets.at(index).name);
        QVariantList storedValues;
        for (qint16 value : presets.at(index).values) {
            storedValues.append(value);
        }
        m_settings->setValue(QStringLiteral("values"), storedValues);
    }
    m_settings->endArray();
    m_settings->sync();
}

void AppSettings::resetAll()
{
    m_settings->clear();
    m_settings->setValue(QStringLiteral("schemaVersion"), SchemaVersion);
    m_settings->sync();
}

} // namespace MicArray
