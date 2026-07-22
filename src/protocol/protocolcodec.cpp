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
    Q_UNUSED(rawType);
    return 2;
}

void appendI16(QByteArray &buffer, qint16 value)
{
    appendU16(buffer, static_cast<quint16>(value));
}

bool hasValidIncomingTail(const QByteArray &buffer, qsizetype offset, quint16 rawType)
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
    for (qint16 value : values.values()) {
        appendI16(frame, value);
    }
    appendI16(frame, 0);
    appendI16(frame, 0);
    appendU16(frame, HostTail);
    return frame;
}

ParseResult ProtocolCodec::parseIncoming(QByteArrayView streamChunk)
{
    static constexpr qsizetype DebugPostTailPreviewSize = 16;

    ParseResult result;
    result.bufferBefore = m_buffer.size();
    result.bytesAppended = streamChunk.size();
    m_buffer.append(streamChunk.data(), streamChunk.size());

    const auto finish = [&result, this]() {
        result.bufferAfter = m_buffer.size();
        return result;
    };

    if (m_pendingDebugFrame.has_value()) {
        const qsizetype captured = m_pendingDebugFrame->postTailPreview.size();
        const qsizetype available = std::max<qsizetype>(0, m_buffer.size() - captured);
        const qsizetype required = DebugPostTailPreviewSize - captured;
        m_pendingDebugFrame->postTailPreview.append(m_buffer.mid(captured, std::min(available, required)));
        if (m_pendingDebugFrame->postTailPreview.size() < DebugPostTailPreviewSize) {
            return finish();
        }
        m_pendingDebugFrame->postTailPreviewComplete = true;
        result.debugFrames.append(*m_pendingDebugFrame);
        m_pendingDebugFrame.reset();
    }

    while (true) {
        const qsizetype headerIndex = m_buffer.indexOf(QByteArray::fromHex("7e7e"));
        if (headerIndex < 0) {
            if (!m_buffer.isEmpty()) {
                result.bytesDiscarded += m_buffer.size();
                result.errors.append({ParseErrorCode::GarbageDiscarded,
                                      QStringLiteral("Discarded %1 bytes before frame header").arg(m_buffer.size())});
                m_buffer.clear();
            }
            return finish();
        }

        if (headerIndex > 0) {
            result.bytesDiscarded += headerIndex;
            result.errors.append({ParseErrorCode::GarbageDiscarded,
                                  QStringLiteral("Discarded %1 bytes before frame header").arg(headerIndex)});
            m_buffer.remove(0, headerIndex);
        }

        if (m_buffer.size() < MediaHeaderSize) {
            return finish();
        }

        const quint16 rawType = readU16(m_buffer, 2);
        const quint32 payloadLength = readU32(m_buffer, 4);

        if (payloadLength > MaxPayloadSize) {
            result.errors.append({ParseErrorCode::InvalidLength,
                                  QStringLiteral("Payload length %1 exceeds maximum").arg(payloadLength)});
            result.bytesDiscarded += 1;
            m_buffer.remove(0, 1);
            continue;
        }

        const int tailSize = expectedIncomingTailSize(rawType);
        const qsizetype frameSize = MediaHeaderSize + payloadLength + tailSize;
        if (m_buffer.size() < frameSize) {
            return finish();
        }

        const qsizetype tailOffset = MediaHeaderSize + payloadLength;
        if (!hasValidIncomingTail(m_buffer, tailOffset, rawType)) {
            const quint16 tail = readTailForDiagnostics(m_buffer, tailOffset);
            const quint16 expectedTail = expectedTailForDiagnostics(rawType);
            const QString expectedTailText = rawType == static_cast<quint16>(FrameType::Video)
                ? QStringLiteral("0A ??")
                : QStringLiteral("0x%1").arg(expectedTail, 4, 16, QLatin1Char('0'));
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
            error.message = QStringLiteral("Invalid frame tail 0x%1, expected %2; type=0x%3, payloadLength=%4, tailOffset=%5, bufferSize=%6, head16=%7, tailContext=%8")
                                .arg(tail, 4, 16, QLatin1Char('0'))
                                .arg(expectedTailText)
                                .arg(rawType, 4, 16, QLatin1Char('0'))
                                .arg(payloadLength)
                                .arg(tailOffset)
                                .arg(m_buffer.size())
                                .arg(hexPreview(error.headerPreview))
                                .arg(hexPreview(error.tailContext));
            result.errors.append(error);
            result.bytesDiscarded += 1;
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
        frame.rawFrame = m_buffer.left(frameSize);
        frame.postTailPreview = m_buffer.mid(frameSize, DebugPostTailPreviewSize);
        frame.postTailPreviewComplete = frame.postTailPreview.size() == DebugPostTailPreviewSize;
        result.frames.append(frame);

        if (!isMediaType(rawType)) {
            result.errors.append({ParseErrorCode::UnknownFrameType,
                                  QStringLiteral("Unsupported incoming frame type 0x%1; frame kept for debug")
                                      .arg(rawType, 4, 16, QLatin1Char('0'))});
        }

        result.bytesParsed += frameSize;
        result.frameSizes.append(frameSize);
        m_buffer.remove(0, frameSize);
        if (frame.postTailPreviewComplete) {
            result.debugFrames.append(frame);
        } else {
            m_pendingDebugFrame = frame;
            return finish();
        }
    }
}

QList<HostFrame> ProtocolCodec::clear()
{
    QList<HostFrame> pendingFrames;
    if (m_pendingDebugFrame.has_value()) {
        pendingFrames.append(*m_pendingDebugFrame);
    }
    m_buffer.clear();
    m_pendingDebugFrame.reset();
    return pendingFrames;
}

} // namespace MicArray
