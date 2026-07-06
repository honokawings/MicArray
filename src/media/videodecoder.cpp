#include "media/videodecoder.h"

namespace MicArray {

VideoDecoder::VideoDecoder(QObject *parent)
    : QObject(parent)
{
    m_timer.start();
}

bool VideoDecoder::decodeJpeg(const QByteArray &payload, quint32 timestampSeconds, quint32 timestampMicros)
{
    QImage image;
    if (!image.loadFromData(payload, "JPG")) {
        ++m_failedFrames;
        emit decodeError(QStringLiteral("Video payload is not a valid JPEG frame"));
        return false;
    }

    m_lastFrame = image;
    ++m_decodedFrames;
    const qint64 elapsed = m_timer.elapsed();
    if (elapsed > 0) {
        m_fps = 1000.0 * static_cast<double>(m_decodedFrames) / static_cast<double>(elapsed);
    }
    emit frameReady(m_lastFrame, m_fps, timestampSeconds, timestampMicros);
    return true;
}

} // namespace MicArray
