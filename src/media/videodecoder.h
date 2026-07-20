#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>

namespace MicArray {

class VideoDecoder : public QObject
{
    Q_OBJECT

public:
    explicit VideoDecoder(QObject *parent = nullptr);

public slots:
    bool decodeJpeg(const QByteArray &payload, quint32 timestampSeconds, quint32 timestampMicros);
    double framesPerSecond() const { return m_fps; }
    int decodedFrames() const { return m_decodedFrames; }
    int failedFrames() const { return m_failedFrames; }
    QImage lastFrame() const { return m_lastFrame; }

signals:
    void frameReady(const QImage &image, double fps, quint32 timestampSeconds, quint32 timestampMicros);
    void decodeError(const QString &message);
    void frameProcessed(bool valid);

private:
    QElapsedTimer m_timer;
    QImage m_lastFrame;
    int m_decodedFrames = 0;
    int m_failedFrames = 0;
    double m_fps = 0.0;
};

} // namespace MicArray
