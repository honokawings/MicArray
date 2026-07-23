#pragma once

#include <QByteArray>
#include <QFile>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace MicArray {

struct WaveFileInfo {
    QString path;
    int channelCount = 0;
    int sampleRate = 0;
    int containerBits = 0;
    int validBits = 0;
    int blockAlign = 0;
    qint64 totalSamples = 0;
    double durationSeconds = 0.0;

    bool operator==(const WaveFileInfo &) const = default;
};

class WaveFileReader
{
public:
    bool open(const QString &path, QString *errorMessage = nullptr);
    void close();

    bool isOpen() const { return m_file.isOpen(); }
    WaveFileInfo info() const { return m_info; }

    QVector<QVector<float>> readChannels(qint64 startSample,
                                         int sampleCount,
                                         QString *errorMessage = nullptr);
    QByteArray readMonoPcm16(int channel,
                             qint64 startSample,
                             int sampleCount,
                             QString *errorMessage = nullptr);

private:
    bool readFrameBytes(qint64 startSample,
                        int sampleCount,
                        QByteArray *bytes,
                        int *destinationStart,
                        int *framesRead,
                        QString *errorMessage);
    qint64 decodeIntegerSample(const char *bytes) const;
    float normalizeSample(qint64 sample) const;
    static void setError(QString *errorMessage, const QString &message);

    QFile m_file;
    WaveFileInfo m_info;
    qint64 m_dataOffset = 0;
    qint64 m_dataLength = 0;
};

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::WaveFileInfo)
