#pragma once

#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QObject>
#include <memory>

#include "app/appsettings.h"
#include "media/audioframedecoder.h"
#include "media/channelgainprocessor.h"

namespace MicArray {

class AudioProcessor : public QObject
{
    Q_OBJECT

public:
    explicit AudioProcessor(QObject *parent = nullptr);

public slots:
    void configure(const AudioSettings &settings);
    void processPcmFrame(const QByteArray &payload, quint32 timestampSeconds, quint32 timestampMicros);
    void processArrayFrame(const QByteArray &payload, quint32 timestampSeconds, quint32 timestampMicros);
    void setGainConfiguration(const GainConfiguration &configuration);
    void setChannelShift(int channel, int shift);
    void setChannelAutomatic(int channel, bool automatic);
    void setAllAutomatic(bool automatic);
    void resetGain();

    QAudioFormat format() const { return m_format; }
    double peakLevel() const { return m_peakLevel; }
    qint64 bytesReceived() const { return m_bytesReceived; }
    int droppedFrames() const { return m_droppedFrames; }
    int outputChannel() const { return m_decoder.outputChannel(); }
    int averageWindowFrames() const { return m_decoder.averageWindowFrames(); }
    GainConfiguration gainConfiguration() const { return m_gainProcessor.configuration(); }

signals:
    void levelChanged(double peakLevel, qint64 bytesReceived, quint32 timestampSeconds, quint32 timestampMicros);
    void arrayFrameDecoded(const DecodedAudioFrame &frame);
    void adjustedFrameReady(const QByteArray &payload,
                            quint32 timestampSeconds,
                            quint32 timestampMicros,
                            const GainSnapshot &snapshot);
    void audioWarning(const QString &message);
    void frameProcessed(bool valid);

private:
    QAudioFormat m_format;
    std::unique_ptr<QAudioSink> m_sink;
    QIODevice *m_output = nullptr;
    bool m_playbackEnabled = false;
    double m_peakLevel = 0.0;
    qint64 m_bytesReceived = 0;
    int m_droppedFrames = 0;
    AudioFrameDecoder m_decoder;
    ChannelGainProcessor m_gainProcessor;
};

} // namespace MicArray
