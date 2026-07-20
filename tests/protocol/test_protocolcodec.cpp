#include <QtTest/QtTest>

#include "control/controlvalues.h"
#include "protocol/protocolcodec.h"

using namespace MicArray;

class ProtocolCodecTest : public QObject
{
    Q_OBJECT

private slots:
    void encodesHeartbeat();
    void encodesControlFrame();
    void parsesAudioFrame();
    void parsesVideoFrame();
    void buffersPartialFrame();
    void parsesStickyFrames();
    void keepsStructurallyValidUnknownTypeForDebug();
    void resynchronizesAfterGarbage();
    void reportsInvalidTailAndRecovers();
    void reportsParseMetrics();
    void capturesSixteenBytesAfterTailInSameChunk();
    void completesPostTailPreviewAcrossChunksWithoutDelayingMediaFrame();
    void flushesIncompletePostTailPreviewOnClear();
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
    values.setChannelValue(6, 0);
    values.setChannelValue(7, 32767);

    const QByteArray frame = ProtocolCodec::encodeControl(values);

    QCOMPARE(frame.size(), 26);
    QCOMPARE(frame.mid(0, 8).toHex(' '), QByteArray("7e 7e 11 cc 16 00 00 00"));
    QCOMPARE(frame.mid(8, 16).toHex(' '), QByteArray("01 00 fe ff 2c 01 70 fe f4 01 a8 fd 00 00 ff 7f"));
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
    QByteArray frame = QByteArray::fromHex("7e7e22ee03000000ddccbbaa07000000ffeedd0a");

    const ParseResult result = codec.parseIncoming(frame);

    QCOMPARE(result.frames.size(), 1);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.frames.first().type, FrameType::Video);
    QCOMPARE(result.frames.first().payload, QByteArray::fromHex("ffeedd"));
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
    const QByteArray video = QByteArray::fromHex("7e7e22ee020000000200000002000000aabb0a");

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
    const QByteArray good = QByteArray::fromHex("7e7e22ee010000000200000002000000020a");

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
    const QByteArray video = QByteArray::fromHex("7e7e22ee020000000200000002000000aabb0a");
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

QTEST_MAIN(ProtocolCodecTest)
#include "test_protocolcodec.moc"
