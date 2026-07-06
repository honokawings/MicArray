#include "app/appsettings.h"

#include <QSettings>

namespace MicArray {

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
{
}

ConnectionSettings AppSettings::connection() const
{
    QSettings settings;
    ConnectionSettings value;
    value.host = settings.value(QStringLiteral("connection/host"), value.host).toString();
    value.port = settings.value(QStringLiteral("connection/port"), value.port).toUInt();
    value.autoReconnect = settings.value(QStringLiteral("connection/autoReconnect"), value.autoReconnect).toBool();
    return value;
}

AudioSettings AppSettings::audio() const
{
    QSettings settings;
    AudioSettings value;
    value.sampleRate = settings.value(QStringLiteral("audio/sampleRate"), value.sampleRate).toInt();
    value.channelCount = settings.value(QStringLiteral("audio/channelCount"), value.channelCount).toInt();
    value.outputChannel = settings.value(QStringLiteral("audio/outputChannel"), value.outputChannel).toInt();
    value.averageWindowFrames = settings.value(QStringLiteral("audio/averageWindowFrames"), value.averageWindowFrames).toInt();
    value.playbackEnabled = settings.value(QStringLiteral("audio/playbackEnabled"), value.playbackEnabled).toBool();
    return value;
}

VideoSettings AppSettings::video() const
{
    QSettings settings;
    VideoSettings value;
    value.format = settings.value(QStringLiteral("video/format"), value.format).toString();
    return value;
}

void AppSettings::saveConnection(const ConnectionSettings &settingsValue)
{
    QSettings settings;
    settings.setValue(QStringLiteral("connection/host"), settingsValue.host);
    settings.setValue(QStringLiteral("connection/port"), settingsValue.port);
    settings.setValue(QStringLiteral("connection/autoReconnect"), settingsValue.autoReconnect);
}

void AppSettings::saveAudio(const AudioSettings &settingsValue)
{
    QSettings settings;
    settings.setValue(QStringLiteral("audio/sampleRate"), settingsValue.sampleRate);
    settings.setValue(QStringLiteral("audio/channelCount"), settingsValue.channelCount);
    settings.setValue(QStringLiteral("audio/outputChannel"), settingsValue.outputChannel);
    settings.setValue(QStringLiteral("audio/averageWindowFrames"), settingsValue.averageWindowFrames);
    settings.setValue(QStringLiteral("audio/playbackEnabled"), settingsValue.playbackEnabled);
}

void AppSettings::saveVideo(const VideoSettings &settingsValue)
{
    QSettings settings;
    settings.setValue(QStringLiteral("video/format"), settingsValue.format);
}

} // namespace MicArray
