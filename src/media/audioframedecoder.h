#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace MicArray {

struct DecodedAudioFrame {
    bool valid = false;
    QString error;
    quint32 timestampSeconds = 0;
    quint32 timestampMicros = 0;
    QList<qint32> samples;
    QList<double> amplitudes;
    QList<double> averageAmplitudes;
    QList<double> normalizedAmplitudes;
    QByteArray outputPcm16;
    int outputChannel = 0;
    int samplePoints = 0;
    int maxAverageChannel = 0;
    int minAverageChannel = 0;
    double maxAverageAmplitude = 0.0;
    double minAverageAmplitude = 0.0;
};

class AudioFrameDecoder
{
public:
    static constexpr int ChannelCount = 48;
    static constexpr int Rows = 6;
    static constexpr int Columns = 8;
    static constexpr int BytesPerChannel = 4;
    static constexpr int ValidBits = 24;
    static constexpr int SamplesPerFrame = 200;
    static constexpr int BytesPerSamplePoint = ChannelCount * BytesPerChannel;
    static constexpr int FramePayloadBytes = SamplesPerFrame * BytesPerSamplePoint;
    static constexpr int DefaultSampleRate = 48000;

    void setOutputChannel(int channel);
    int outputChannel() const { return m_outputChannel; }

    void setAverageWindowFrames(int frames);
    int averageWindowFrames() const { return m_averageWindowFrames; }

    DecodedAudioFrame decode(const QByteArray &payload, quint32 timestampSeconds, quint32 timestampMicros);
    void reset();

private:
    QList<double> calculateAverages() const;
    static qint32 decodeSigned32InLittleEndian(const QByteArray &payload, int offset);
    static qint16 toPcm16(qint32 sample);

    int m_outputChannel = 0;
    int m_averageWindowFrames = 100;
    QList<QList<double>> m_amplitudeHistory;
};

} // namespace MicArray
