#include <QtTest/QtTest>

#include "control/controlvalues.h"
#include "protocol/protocolcodec.h"
#include <QRandomGenerator>
#include <QtEndian>

namespace
{
QByteArray mediaFrame(const QByteArray &payload)
{
    QByteArray wire(16, 0);
    qToLittleEndian(MicArray::ProtocolCodec::HostHeader, wire.data());
    qToLittleEndian(quint16(MicArray::FrameType::Audio), wire.data() + 2);
    qToLittleEndian(quint32(payload.size()), wire.data() + 4);
    qToLittleEndian(quint32(17), wire.data() + 8);
    qToLittleEndian(quint32(123), wire.data() + 12);
    wire += payload;
    wire += QByteArray::fromHex("0a0d");
    return wire;
}
}

using namespace MicArray;

class ProtocolCodecTest : public QObject
{
    Q_OBJECT

private slots:
    void encodesHeartbeat();
    void encodesControlFrame();
    void parsesAudioFrame();
    void parsesVideoFrame();
    void acceptsAnySecondVideoTailByte();
    void waitsForSecondVideoTailByte();
    void rejectsInvalidFirstVideoTailByte();
    void rejectsAudioTailWithVideoWildcardSecondByte();
    void videoPostTailStartsAfterWildcardByte();
    void buffersPartialFrame();
    void parsesStickyFrames();
    void keepsStructurallyValidUnknownTypeForDebug();
    void resynchronizesAfterGarbage();
    void reportsInvalidTailAndRecovers();
    void reportsParseMetrics();
    void capturesSixteenBytesAfterTailInSameChunk();
    void completesPostTailPreviewAcrossChunksWithoutDelayingMediaFrame();
    void flushesIncompletePostTailPreviewOnClear();
    void retainsSplitSynchronizationPrefix();
    void byteByByteAndRandomFragmentation();
    void coalescedFramesAndFrameBoundarySplit();
    void garbageResynchronizationKeepsPrefix();
    void maximumGarbageRetentionIsOneByte();
    void maximumPayloadBoundaryAndInvalidLengthRecovery();
    void clearDropsRetainedPrefix();
};

void ProtocolCodecTest::encodesHeartbeat()
{
    const QByteArray frame = ProtocolCodec::encodeHeartbeat();
    QCOMPARE(frame.toHex(' '), QByteArray("7e 7e 00 cc 00 00 00 00 0a 0d"));
}

void ProtocolCodecTest::encodesControlFrame()
{
    ControlValues values;
    values.setChannelValue(0, 1);
    values.setChannelValue(1, -2);
    values.setChannelValue(2, 300);
    values.setChannelValue(3, -400);
    values.setChannelValue(4, 500);
    values.setChannelValue(5, -600);

    const QByteArray frame = ProtocolCodec::encodeControl(values);

    QCOMPARE(ControlValues::ChannelCount, 6);
    QCOMPARE(frame.size(), ProtocolCodec::ControlFrameSize);
    QCOMPARE(frame.mid(0, 8).toHex(' '), QByteArray("7e 7e 11 cc 16 00 00 00"));
    QCOMPARE(frame.mid(8, 12).toHex(' '), QByteArray("01 00 fe ff 2c 01 70 fe f4 01 a8 fd"));
    QCOMPARE(frame.mid(20, 4), QByteArray(4, '\0'));
    QCOMPARE(frame.mid(24, 2).toHex(' '), QByteArray("0a 0d"));
}

void ProtocolCodecTest::parsesAudioFrame()
{
    ProtocolCodec codec;
    QByteArray frame = QByteArray::fromHex("7e7e11ee04000000040302012a000000112233440a0d");

    const ParseResult result = codec.parseIncoming(frame);

    QCOMPARE(result.frames.size(), 1);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.frames.first().type, FrameType::Audio);
    QCOMPARE(result.frames.first().payloadLength, 4u);
    QCOMPARE(result.frames.first().timestampSeconds, 0x01020304u);
    QCOMPARE(result.frames.first().timestampMicros, 42u);
    QCOMPARE(result.frames.first().payload.toHex(), QByteArray("11223344"));
}

void ProtocolCodecTest::parsesVideoFrame()
{
    ProtocolCodec codec;
    QByteArray frame = QByteArray::fromHex("7e7e22ee03000000ddccbbaa07000000ffeedd0a00");

    const ParseResult result = codec.parseIncoming(frame);

    QCOMPARE(result.frames.size(), 1);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.frames.first().type, FrameType::Video);
    QCOMPARE(result.frames.first().payload, QByteArray::fromHex("ffeedd"));
}

void ProtocolCodecTest::acceptsAnySecondVideoTailByte()
{
    const QList<QByteArray> frames = {
        QByteArray::fromHex("7e7e22ee020000000200000002000000aabb0a00"),
        QByteArray::fromHex("7e7e22ee020000000200000002000000aabb0aff"),
    };

    for (const QByteArray &frame : frames) {
        ProtocolCodec codec;
        const ParseResult result = codec.parseIncoming(frame);

        QCOMPARE(result.frames.size(), 1);
        QCOMPARE(result.errors.size(), 0);
        QCOMPARE(result.bytesParsed, qsizetype(20));
        QCOMPARE(result.frameSizes, QList<qsizetype>({20}));
        QCOMPARE(result.frames.first().rawFrame, frame);
    }
}

void ProtocolCodecTest::waitsForSecondVideoTailByte()
{
    ProtocolCodec codec;
    const QByteArray incomplete = QByteArray::fromHex("7e7e22ee020000000200000002000000aabb0a");

    const ParseResult result = codec.parseIncoming(incomplete);

    QCOMPARE(result.frames.size(), 0);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.bytesParsed, qsizetype(0));
}

void ProtocolCodecTest::rejectsInvalidFirstVideoTailByte()
{
    ProtocolCodec codec;
    const QByteArray frame = QByteArray::fromHex("7e7e22ee020000000200000002000000aabb7f00");

    const ParseResult result = codec.parseIncoming(frame);

    QCOMPARE(result.frames.size(), 0);
    QVERIFY(!result.errors.isEmpty());
    QCOMPARE(result.errors.first().code, ParseErrorCode::InvalidTail);
    QCOMPARE(result.errors.first().actualTail, 0x007f);
    QVERIFY(result.errors.first().message.contains(QStringLiteral("expected 0A ??")));
}

void ProtocolCodecTest::rejectsAudioTailWithVideoWildcardSecondByte()
{
    ProtocolCodec codec;
    const QByteArray frame = QByteArray::fromHex("7e7e11ee020000000200000002000000aabb0a00");

    const ParseResult result = codec.parseIncoming(frame);

    QCOMPARE(result.frames.size(), 0);
    QVERIFY(!result.errors.isEmpty());
    QCOMPARE(result.errors.first().code, ParseErrorCode::InvalidTail);
    QCOMPARE(result.errors.first().actualTail, 0x000a);
    QCOMPARE(result.errors.first().expectedTail, ProtocolCodec::HostTail);
}

void ProtocolCodecTest::videoPostTailStartsAfterWildcardByte()
{
    ProtocolCodec codec;
    const QByteArray video = QByteArray::fromHex("7e7e22ee020000000200000002000000aabb0aff");
    const QByteArray afterTail = QByteArray::fromHex("00112233445566778899aabbccddeeff");

    const ParseResult result = codec.parseIncoming(video + afterTail);

    QCOMPARE(result.frames.size(), 1);
    QCOMPARE(result.debugFrames.size(), 1);
    QCOMPARE(result.debugFrames.first().postTailPreview, afterTail);
    QVERIFY(result.debugFrames.first().postTailPreviewComplete);
}

void ProtocolCodecTest::buffersPartialFrame()
{
    ProtocolCodec codec;
    const QByteArray frame = QByteArray::fromHex("7e7e11ee04000000040302012a000000112233440a0d");

    ParseResult first = codec.parseIncoming(QByteArrayView(frame.constData(), 9));
    QCOMPARE(first.frames.size(), 0);
    QCOMPARE(first.errors.size(), 0);

    ParseResult second = codec.parseIncoming(QByteArrayView(frame.constData() + 9, frame.size() - 9));
    QCOMPARE(second.frames.size(), 1);
    QCOMPARE(second.frames.first().payload.toHex(), QByteArray("11223344"));
}

void ProtocolCodecTest::parsesStickyFrames()
{
    ProtocolCodec codec;
    const QByteArray audio = QByteArray::fromHex("7e7e11ee010000000100000001000000010a0d");
    const QByteArray video = QByteArray::fromHex("7e7e22ee020000000200000002000000aabb0a00");

    const ParseResult result = codec.parseIncoming(audio + video);

    QCOMPARE(result.frames.size(), 2);
    QCOMPARE(result.frames.at(0).type, FrameType::Audio);
    QCOMPARE(result.frames.at(1).type, FrameType::Video);
}

void ProtocolCodecTest::keepsStructurallyValidUnknownTypeForDebug()
{
    ProtocolCodec codec;
    const QByteArray frame = QByteArray::fromHex("7e7e3412030000000900000008000000aabbcc0a0d");

    const ParseResult result = codec.parseIncoming(frame);

    QCOMPARE(result.frames.size(), 1);
    QCOMPARE(result.frames.first().rawType, 0x1234);
    QCOMPARE(result.frames.first().payloadLength, 3u);
    QCOMPARE(result.frames.first().timestampSeconds, 9u);
    QCOMPARE(result.frames.first().timestampMicros, 8u);
    QCOMPARE(result.frames.first().payload, QByteArray::fromHex("aabbcc"));
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.errors.first().code, ParseErrorCode::UnknownFrameType);
}

void ProtocolCodecTest::resynchronizesAfterGarbage()
{
    ProtocolCodec codec;
    const QByteArray garbage = QByteArray::fromHex("00112233ff");
    const QByteArray frame = QByteArray::fromHex("7e7e11ee010000000100000001000000010a0d");

    const ParseResult result = codec.parseIncoming(garbage + frame);

    QCOMPARE(result.frames.size(), 1);
    QVERIFY(result.errors.size() >= 1);
    QCOMPARE(result.frames.first().type, FrameType::Audio);
}

void ProtocolCodecTest::reportsInvalidTailAndRecovers()
{
    ProtocolCodec codec;
    const QByteArray bad = QByteArray::fromHex("7e7e11ee01000000010000000100000001ffff");
    const QByteArray good = QByteArray::fromHex("7e7e22ee010000000200000002000000020a00");

    const ParseResult result = codec.parseIncoming(bad + good);

    QCOMPARE(result.frames.size(), 1);
    QVERIFY(result.errors.size() >= 1);
    QVERIFY(result.errors.first().message.contains(QStringLiteral("0xffff")));
    QCOMPARE(result.errors.first().rawType, 0xee11);
    QCOMPARE(result.errors.first().payloadLength, 1u);
    QCOMPARE(result.errors.first().tailOffset, qsizetype(17));
    QCOMPARE(result.errors.first().actualTail, 0xffff);
    QCOMPARE(result.errors.first().expectedTail, ProtocolCodec::HostTail);
    QCOMPARE(result.errors.first().headerPreview.toHex(), QByteArray("7e7e11ee010000000100000001000000"));
    QVERIFY(!result.errors.first().tailContext.isEmpty());
    QCOMPARE(result.frames.first().type, FrameType::Video);
}

void ProtocolCodecTest::reportsParseMetrics()
{
    ProtocolCodec codec;
    const QByteArray garbage = QByteArray::fromHex("0001");
    const QByteArray audio = QByteArray::fromHex("7e7e11ee010000000100000001000000010a0d");
    const QByteArray video = QByteArray::fromHex("7e7e22ee020000000200000002000000aabb0a00");
    const QByteArray partial = QByteArray::fromHex("7e7e11ee010000000300000004000000");

    const ParseResult result = codec.parseIncoming(garbage + audio + video + partial);

    QCOMPARE(result.frames.size(), 2);
    QCOMPARE(result.bytesAppended, qsizetype(garbage.size() + audio.size() + video.size() + partial.size()));
    QCOMPARE(result.bufferBefore, qsizetype(0));
    QCOMPARE(result.bytesDiscarded, qsizetype(garbage.size()));
    QCOMPARE(result.bytesParsed, qsizetype(audio.size() + video.size()));
    QCOMPARE(result.bufferAfter, qsizetype(partial.size()));
    QCOMPARE(result.frameSizes, QList<qsizetype>({audio.size(), video.size()}));
}

void ProtocolCodecTest::capturesSixteenBytesAfterTailInSameChunk()
{
    ProtocolCodec codec;
    const QByteArray audio = QByteArray::fromHex("7e7e11ee010000000100000001000000010a0d");
    const QByteArray afterTail = QByteArray::fromHex("00112233445566778899aabbccddeeff");

    const ParseResult result = codec.parseIncoming(audio + afterTail);

    QCOMPARE(result.frames.size(), 1);
    QCOMPARE(result.debugFrames.size(), 1);
    QCOMPARE(result.debugFrames.first().postTailPreview, afterTail);
    QVERIFY(result.debugFrames.first().postTailPreviewComplete);
}

void ProtocolCodecTest::completesPostTailPreviewAcrossChunksWithoutDelayingMediaFrame()
{
    ProtocolCodec codec;
    const QByteArray audio = QByteArray::fromHex("7e7e11ee010000000100000001000000010a0d");
    const QByteArray firstPreviewPart = QByteArray::fromHex("0011223344");
    const QByteArray secondPreviewPart = QByteArray::fromHex("5566778899aabbccddeeff");

    const ParseResult first = codec.parseIncoming(audio + firstPreviewPart);
    QCOMPARE(first.frames.size(), 1);
    QCOMPARE(first.debugFrames.size(), 0);

    const ParseResult second = codec.parseIncoming(secondPreviewPart);
    QCOMPARE(second.debugFrames.size(), 1);
    QCOMPARE(second.debugFrames.first().postTailPreview, firstPreviewPart + secondPreviewPart);
    QVERIFY(second.debugFrames.first().postTailPreviewComplete);
}

void ProtocolCodecTest::flushesIncompletePostTailPreviewOnClear()
{
    ProtocolCodec codec;
    const QByteArray audio = QByteArray::fromHex("7e7e11ee010000000100000001000000010a0d");
    const QByteArray afterTail = QByteArray::fromHex("aabbcc");

    const ParseResult result = codec.parseIncoming(audio + afterTail);
    QCOMPARE(result.frames.size(), 1);
    QCOMPARE(result.debugFrames.size(), 0);

    const QList<HostFrame> flushed = codec.clear();
    QCOMPARE(flushed.size(), 1);
    QCOMPARE(flushed.first().postTailPreview, afterTail);
    QVERIFY(!flushed.first().postTailPreviewComplete);
}

void ProtocolCodecTest::retainsSplitSynchronizationPrefix()
{
    ProtocolCodec codec;
    const auto wire = mediaFrame("original payload");
    const auto first = codec.parseIncoming(wire.first(1));
    QCOMPARE(first.bytesDiscarded, qsizetype(0));
    QCOMPARE(first.bufferAfter, qsizetype(1));
    QVERIFY(first.errors.isEmpty());
    const auto rest = codec.parseIncoming(wire.sliced(1));
    QCOMPARE(rest.frames.size(), 1);
    QCOMPARE(rest.frames.front().payload, QByteArray("original payload"));
    QCOMPARE(rest.bytesDiscarded, qsizetype(0));
    QVERIFY(rest.errors.isEmpty());
}

void ProtocolCodecTest::byteByByteAndRandomFragmentation()
{
    const QList<QByteArray> payloads {QByteArray::fromHex("7e7e0a0d"), QByteArray(38400, char(0x7e)), "third"};
    QByteArray wire;
    for (const auto &payload : payloads) wire += mediaFrame(payload);
    for (bool bytewise : {true, false})
    {
        ProtocolCodec codec;
        QRandomGenerator random(0x20261020);
        QList<QByteArray> actual;
        for (int offset = 0; offset < wire.size();)
        {
            const int count = std::min(bytewise ? 1 : int(random.bounded(1U, 8193U)), int(wire.size()) - offset);
            const auto result = codec.parseIncoming(QByteArrayView(wire).sliced(offset, count));
            QVERIFY(result.errors.isEmpty());
            QCOMPARE(result.bytesDiscarded, qsizetype(0));
            for (const auto &frame : result.frames) actual.append(frame.payload);
            offset += count;
        }
        QCOMPARE(actual, payloads);
    }
}

void ProtocolCodecTest::coalescedFramesAndFrameBoundarySplit()
{
    ProtocolCodec codec;
    const auto firstWire = mediaFrame("first");
    const auto secondWire = mediaFrame("second");
    const auto thirdWire = mediaFrame("third");
    const auto first = codec.parseIncoming(firstWire + secondWire + thirdWire.first(1));
    QCOMPARE(first.frames.size(), 2);
    QCOMPARE(first.frames[0].payload, QByteArray("first"));
    QCOMPARE(first.frames[1].payload, QByteArray("second"));
    QVERIFY(first.errors.isEmpty());
    const auto second = codec.parseIncoming(thirdWire.sliced(1));
    QCOMPARE(second.frames.size(), 1);
    QCOMPARE(second.frames[0].payload, QByteArray("third"));
    QVERIFY(second.errors.isEmpty());
}

void ProtocolCodecTest::garbageResynchronizationKeepsPrefix()
{
    ProtocolCodec codec;
    const auto first = codec.parseIncoming(QByteArray::fromHex("deadbeef007e"));
    QCOMPARE(first.bytesDiscarded, qsizetype(5));
    QCOMPARE(first.bufferAfter, qsizetype(1));
    QCOMPARE(first.errors.size(), 1);
    QCOMPARE(first.errors[0].code, ParseErrorCode::GarbageDiscarded);
    const auto wire = mediaFrame("after garbage");
    const auto second = codec.parseIncoming(wire.sliced(1));
    QCOMPARE(second.frames.size(), 1);
    QCOMPARE(second.frames[0].payload, QByteArray("after garbage"));
    QVERIFY(second.errors.isEmpty());
}

void ProtocolCodecTest::maximumGarbageRetentionIsOneByte()
{
    ProtocolCodec codec;
    QByteArray garbage(ProtocolCodec::MaxPayloadSize + 4096, char(0x55));
    garbage.append(char(0x7e));
    const auto result = codec.parseIncoming(garbage);
    QCOMPARE(result.bytesDiscarded, garbage.size() - 1);
    QCOMPARE(result.bufferAfter, qsizetype(1));
    const auto next = codec.parseIncoming(QByteArray::fromHex("01"));
    QCOMPARE(next.bytesDiscarded, qsizetype(2));
    QCOMPARE(next.bufferAfter, qsizetype(0));
}

void ProtocolCodecTest::maximumPayloadBoundaryAndInvalidLengthRecovery()
{
    ProtocolCodec codec;
    const QByteArray payload(ProtocolCodec::MaxPayloadSize, char(0x52));
    const auto maximum = mediaFrame(payload);
    const auto partial = codec.parseIncoming(maximum.first(maximum.size() - 3));
    QVERIFY(partial.errors.isEmpty());
    QCOMPARE(partial.bufferAfter, qsizetype(ProtocolCodec::MediaHeaderSize + ProtocolCodec::MaxPayloadSize - 1));
    const auto complete = codec.parseIncoming(maximum.last(3));
    QCOMPARE(complete.frames.size(), 1);
    QCOMPARE(complete.frames.front().payload, payload);
    QVERIFY(complete.errors.isEmpty());
    codec.clear();
    QByteArray oversized(16, char(0));
    qToLittleEndian(ProtocolCodec::HostHeader, oversized.data());
    qToLittleEndian(quint16(FrameType::Audio), oversized.data() + 2);
    qToLittleEndian(quint32(ProtocolCodec::MaxPayloadSize + 1), oversized.data() + 4);
    oversized.append(char(0x7e));
    const auto rejected = codec.parseIncoming(oversized);
    QVERIFY(!rejected.errors.isEmpty());
    QCOMPARE(rejected.errors.front().code, ParseErrorCode::InvalidLength);
    QCOMPARE(rejected.bufferAfter, qsizetype(1));
    const auto valid = mediaFrame("recovered");
    const auto recovered = codec.parseIncoming(valid.sliced(1));
    QCOMPARE(recovered.frames.size(), 1);
    QCOMPARE(recovered.frames.front().payload, QByteArray("recovered"));
}

void ProtocolCodecTest::clearDropsRetainedPrefix()
{
    ProtocolCodec codec;
    const auto first = codec.parseIncoming(QByteArray::fromHex("7e"));
    QCOMPARE(first.bufferAfter, qsizetype(1));
    codec.clear();
    const auto valid = codec.parseIncoming(mediaFrame("new connection"));
    QCOMPARE(valid.frames.size(), 1);
    QVERIFY(valid.errors.isEmpty());
    QCOMPARE(valid.frames.front().payload, QByteArray("new connection"));
}

QTEST_MAIN(ProtocolCodecTest)
#include "test_protocolcodec.moc"
