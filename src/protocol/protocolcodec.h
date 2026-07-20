#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QMetaType>
#include <QString>

#include <optional>

namespace MicArray {

enum class FrameType : quint16 {
    Audio = 0xee11,
    Video = 0xee22,
    Heartbeat = 0xcc00,
    Control = 0xcc11,
    PlPs = 0xeeff,
};

struct HostFrame {
    quint16 header = 0;
    quint16 rawType = 0;
    FrameType type = FrameType::Audio;
    quint32 payloadLength = 0;
    quint32 timestampSeconds = 0;
    quint32 timestampMicros = 0;
    QByteArray payload;
    QByteArray postTailPreview;
    bool postTailPreviewComplete = false;
};

enum class ParseErrorCode {
    GarbageDiscarded,
    InvalidTail,
    UnknownFrameType,
    InvalidLength,
};

struct ParseError {
    ParseErrorCode code = ParseErrorCode::GarbageDiscarded;
    QString message;
    quint16 rawType = 0;
    quint32 payloadLength = 0;
    qsizetype tailOffset = -1;
    qsizetype bufferSize = 0;
    quint16 actualTail = 0;
    quint16 expectedTail = 0;
    QByteArray headerPreview;
    QByteArray tailContext;
};

struct ParseResult {
    QList<HostFrame> frames;
    QList<HostFrame> debugFrames;
    QList<ParseError> errors;
    qsizetype bytesAppended = 0;
    qsizetype bufferBefore = 0;
    qsizetype bufferAfter = 0;
    qsizetype bytesParsed = 0;
    qsizetype bytesDiscarded = 0;
    QList<qsizetype> frameSizes;
};

class ControlValues;

class ProtocolCodec
{
public:
    static constexpr quint16 HostHeader = 0x7e7e;
    static constexpr quint16 HostTail = 0x0d0a;
    static constexpr int MediaHeaderSize = 16;
    static constexpr int HeartbeatFrameSize = 10;
    static constexpr int ControlFrameSize = 26;
    static constexpr quint32 MaxPayloadSize = 16 * 1024 * 1024;

    static QByteArray encodeHeartbeat();
    static QByteArray encodeControl(const ControlValues &values);

    ParseResult parseIncoming(QByteArrayView streamChunk);
    QList<HostFrame> clear();

private:
    QByteArray m_buffer;
    std::optional<HostFrame> m_pendingDebugFrame;
};

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::HostFrame)
Q_DECLARE_METATYPE(MicArray::ParseError)
