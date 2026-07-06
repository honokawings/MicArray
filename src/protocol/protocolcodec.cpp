#include "protocol/protocolcodec.h"

#include "control/controlvalues.h"

#include <QtEndian>

#include <algorithm>

namespace MicArray {
namespace {

void appendU16(QByteArray &buffer, quint16 value)
{
    char bytes[2];
    qToLittleEndian(value, bytes);
    buffer.append(bytes, 2);
}

void appendU32(QByteArray &buffer, quint32 value)
{
    char bytes[4];
    qToLittleEndian(value, bytes);
    buffer.append(bytes, 4);
}

quint16 readU16(const QByteArray &buffer, qsizetype offset)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(buffer.constData() + offset));
}

quint32 readU32(const QByteArray &buffer, qsizetype offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(buffer.constData() + offset));
}

bool isMediaType(quint16 rawType)
{
    return rawType == static_cast<quint16>(FrameType::Audio)
        || rawType == static_cast<quint16>(FrameType::Video);
}

QString hexPreview(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex(' ').toUpper());
}

int expectedIncomingTailSize(quint16 rawType)
{
    return rawType == static_cast<quint16>(FrameType::Video) ? 1 : 2;
}

bool hasExpectedIncomingTail(const QByteArray &buffer, qsizetype offset, quint16 rawType)
{
    if (rawType == static_cast<quint16>(FrameType::Video)) {
        return static_cast<uchar>(buffer.at(offset)) == 0x0a;
    }
    return readU16(buffer, offset) == ProtocolCodec::HostTail;
}

quint16 readTailForDiagnostics(const QByteArray &buffer, qsizetype offset)
{
    if (offset + 1 >= buffer.size()) {
        return static_cast<uchar>(buffer.at(offset));
    }
    return readU16(buffer, offset);
}

quint16 expectedTailForDiagnostics(quint16 rawType)
{
    return rawType == static_cast<quint16>(FrameType::Video) ? 0x000a : ProtocolCodec::HostTail;
}

} // namespace

QByteArray ProtocolCodec::encodeHeartbeat()
{
    QByteArray frame;
    frame.reserve(HeartbeatFrameSize);
    appendU16(frame, HostHeader);
    appendU16(frame, static_cast<quint16>(FrameType::Heartbeat));
    appendU32(frame, 0);
    appendU16(frame, HostTail);
    return frame;
}

QByteArray ProtocolCodec::encodeControl(const ControlValues &values)
{
    QByteArray frame;
    frame.reserve(ControlFrameSize);
    appendU16(frame, HostHeader);
    appendU16(frame, static_cast<quint16>(FrameType::Control));
    appendU32(frame, 0x16);
    for (int i = 0; i < ControlValues::ChannelCount; ++i) {
        appendU16(frame, static_cast<quint16>(values.channelValue(i)));
    }
    appendU16(frame, HostTail);
    return frame;
}

ParseResult ProtocolCodec::parseIncoming(QByteArrayView streamChunk)
{
    ParseResult result;
    m_buffer.append(streamChunk.data(), streamChunk.size());

    while (true) {
        const qsizetype headerIndex = m_buffer.indexOf(QByteArray::fromHex("7e7e"));
        if (headerIndex < 0) {
            if (!m_buffer.isEmpty()) {
                result.errors.append({ParseErrorCode::GarbageDiscarded,
                                      QStringLiteral("Discarded %1 bytes before frame header").arg(m_buffer.size())});
                m_buffer.clear();
            }
            return result;
        }

        if (headerIndex > 0) {
            result.errors.append({ParseErrorCode::GarbageDiscarded,
                                  QStringLiteral("Discarded %1 bytes before frame header").arg(headerIndex)});
            m_buffer.remove(0, headerIndex);
        }

        if (m_buffer.size() < MediaHeaderSize) {
            return result;
        }

        const quint16 rawType = readU16(m_buffer, 2);
        const quint32 payloadLength = readU32(m_buffer, 4);

        if (payloadLength > MaxPayloadSize) {
            result.errors.append({ParseErrorCode::InvalidLength,
                                  QStringLiteral("Payload length %1 exceeds maximum").arg(payloadLength)});
            m_buffer.remove(0, 1);
            continue;
        }

        const int tailSize = expectedIncomingTailSize(rawType);
        const qsizetype frameSize = MediaHeaderSize + payloadLength + tailSize;
        if (m_buffer.size() < frameSize) {
            return result;
        }

        const qsizetype tailOffset = MediaHeaderSize + payloadLength;
        if (!hasExpectedIncomingTail(m_buffer, tailOffset, rawType)) {
            const quint16 tail = readTailForDiagnostics(m_buffer, tailOffset);
            const quint16 expectedTail = expectedTailForDiagnostics(rawType);
            const qsizetype contextStart = std::max<qsizetype>(0, tailOffset - 8);
            const qsizetype contextLength = std::min<qsizetype>(m_buffer.size() - contextStart, 18);
            ParseError error;
            error.code = ParseErrorCode::InvalidTail;
            error.rawType = rawType;
            error.payloadLength = payloadLength;
            error.tailOffset = tailOffset;
            error.bufferSize = m_buffer.size();
            error.actualTail = tail;
            error.expectedTail = expectedTail;
            error.headerPreview = m_buffer.left(MediaHeaderSize);
            error.tailContext = m_buffer.mid(contextStart, contextLength);
            error.message = QStringLiteral("Invalid frame tail 0x%1, expected 0x%2; type=0x%3, payloadLength=%4, tailOffset=%5, bufferSize=%6, head16=%7, tailContext=%8")
                                .arg(tail, 4, 16, QLatin1Char('0'))
                                .arg(expectedTail, 4, 16, QLatin1Char('0'))
                                .arg(rawType, 4, 16, QLatin1Char('0'))
                                .arg(payloadLength)
                                .arg(tailOffset)
                                .arg(m_buffer.size())
                                .arg(hexPreview(error.headerPreview))
                                .arg(hexPreview(error.tailContext));
            result.errors.append(error);
            m_buffer.remove(0, 1);
            continue;
        }

        HostFrame frame;
        frame.header = HostHeader;
        frame.rawType = rawType;
        frame.type = static_cast<FrameType>(rawType);
        frame.payloadLength = payloadLength;
        frame.timestampSeconds = readU32(m_buffer, 8);
        frame.timestampMicros = readU32(m_buffer, 12);
        frame.payload = m_buffer.mid(MediaHeaderSize, payloadLength);
        result.frames.append(frame);

        if (!isMediaType(rawType)) {
            result.errors.append({ParseErrorCode::UnknownFrameType,
                                  QStringLiteral("Unsupported incoming frame type 0x%1; frame kept for debug")
                                      .arg(rawType, 4, 16, QLatin1Char('0'))});
        }

        m_buffer.remove(0, frameSize);
    }
}

void ProtocolCodec::clear()
{
    m_buffer.clear();
}

} // namespace MicArray
