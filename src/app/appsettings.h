#pragma once

#include <QObject>
#include <QString>

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

class AppSettings : public QObject
{
    Q_OBJECT

public:
    explicit AppSettings(QObject *parent = nullptr);

    ConnectionSettings connection() const;
    AudioSettings audio() const;
    VideoSettings video() const;

    void saveConnection(const ConnectionSettings &settings);
    void saveAudio(const AudioSettings &settings);
    void saveVideo(const VideoSettings &settings);
};

} // namespace MicArray
