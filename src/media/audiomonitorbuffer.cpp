#include "media/audiomonitorbuffer.h"

#include <algorithm>
#include <limits>

namespace MicArray {

void AudioMonitorBuffer::configure(int sampleRate, int targetMilliseconds)
{
    const qint64 targetBytes = sampleRate > 0 && targetMilliseconds > 0
        ? static_cast<qint64>(sampleRate) * targetMilliseconds
            * BytesPerSample / 1000
        : 0;
    const qint64 maximumTarget =
        static_cast<qint64>(std::numeric_limits<qsizetype>::max() / 2);
    m_targetBytes = alignedBytes(static_cast<qsizetype>(
        std::min(targetBytes, maximumTarget)));
    m_maxBytes = m_targetBytes * 2;
    clear();
}

qsizetype AudioMonitorBuffer::append(const QByteArray &samples)
{
    const qsizetype appendBytes = alignedBytes(samples.size());
    if (appendBytes == 0) {
        return 0;
    }

    const qsizetype available = availableBytes();
    if (m_maxBytes == 0) {
        clear();
        return available + appendBytes;
    }
    if (appendBytes >= m_maxBytes) {
        const qsizetype dropped = available + appendBytes - m_maxBytes;
        m_bytes = samples.mid(appendBytes - m_maxBytes, m_maxBytes);
        m_readOffset = 0;
        return dropped;
    }

    const qsizetype dropped = available + appendBytes > m_maxBytes
        ? available + appendBytes - m_maxBytes
        : 0;
    discard(dropped);
    compactIfNeeded(appendBytes);
    m_bytes.append(samples.constData(), appendBytes);
    return dropped;
}

bool AudioMonitorBuffer::readyToStart() const
{
    return m_targetBytes > 0 && availableBytes() >= m_targetBytes;
}

qsizetype AudioMonitorBuffer::availableBytes() const
{
    return m_bytes.size() - m_readOffset;
}

QByteArray AudioMonitorBuffer::peek(qsizetype bytes) const
{
    const qsizetype count = std::min(alignedBytes(bytes), availableBytes());
    return m_bytes.mid(m_readOffset, count);
}

void AudioMonitorBuffer::discard(qsizetype bytes)
{
    const qsizetype count = std::min(alignedBytes(bytes), availableBytes());
    if (count == availableBytes()) {
        clear();
        return;
    }
    m_readOffset += count;
}

void AudioMonitorBuffer::clear()
{
    m_bytes.clear();
    m_readOffset = 0;
}

qsizetype AudioMonitorBuffer::alignedBytes(qsizetype bytes)
{
    return bytes > 0 ? bytes - bytes % BytesPerSample : 0;
}

void AudioMonitorBuffer::compactIfNeeded(qsizetype appendBytes)
{
    if (m_readOffset > 0 && m_bytes.size() + appendBytes > m_maxBytes) {
        m_bytes.remove(0, m_readOffset);
        m_readOffset = 0;
    }
}

} // namespace MicArray
