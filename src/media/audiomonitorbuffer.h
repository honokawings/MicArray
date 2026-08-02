#pragma once

#include <QByteArray>

namespace MicArray {

class AudioMonitorBuffer
{
public:
    void configure(int sampleRate, int targetMilliseconds);

    qsizetype append(const QByteArray &samples);
    bool readyToStart() const;
    qsizetype availableBytes() const;
    QByteArray peek(qsizetype bytes) const;
    void discard(qsizetype bytes);
    void clear();

private:
    static constexpr qsizetype BytesPerSample = 2;

    static qsizetype alignedBytes(qsizetype bytes);
    void compactIfNeeded(qsizetype appendBytes);

    QByteArray m_bytes;
    qsizetype m_readOffset = 0;
    qsizetype m_targetBytes = 0;
    qsizetype m_maxBytes = 0;
};

} // namespace MicArray
