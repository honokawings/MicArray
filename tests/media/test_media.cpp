#include <QtTest/QtTest>

#include <QBuffer>
#include <QSignalSpy>
#include <QtEndian>

#include "media/audioframedecoder.h"
#include "media/audioprocessor.h"
#include "media/videodecoder.h"

using namespace MicArray;

namespace {

void putSample(QByteArray &payload, int samplePoint, int channel, qint32 sample)
{
    const int offset = samplePoint * AudioFrameDecoder::BytesPerSamplePoint
        + channel * AudioFrameDecoder::BytesPerChannel;
    qToLittleEndian(sample, reinterpret_cast<uchar *>(payload.data() + offset));
}

}

class MediaTest : public QObject
{
    Q_OBJECT

private slots:
    void decodesJpegPayload();
    void reportsInvalidJpegPayload();
    void configuresAudioFormat();
    void calculatesAudioPeakLevel();
    void decodesFortyEightChannelFrame();
    void extractsSelectedChannelForPlayback();
    void normalizesArrayAmplitudeWindow();
};

void MediaTest::decodesJpegPayload()
{
    QImage image(8, 6, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray payload;
    QBuffer buffer(&payload);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "JPG"));

    VideoDecoder decoder;
    QSignalSpy frameSpy(&decoder, &VideoDecoder::frameReady);

    QVERIFY(decoder.decodeJpeg(payload, 1, 2));
    QCOMPARE(frameSpy.count(), 1);
    QCOMPARE(decoder.lastFrame().size(), QSize(8, 6));
}

void MediaTest::reportsInvalidJpegPayload()
{
    VideoDecoder decoder;
    QSignalSpy errorSpy(&decoder, &VideoDecoder::decodeError);

    QVERIFY(!decoder.decodeJpeg(QByteArray("not-jpeg"), 1, 2));
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(decoder.failedFrames(), 1);
}

void MediaTest::configuresAudioFormat()
{
    AudioProcessor processor;
    processor.configure({48000, 48, 12, 100, false});

    QCOMPARE(processor.format().sampleRate(), 48000);
    QCOMPARE(processor.format().channelCount(), 1);
    QCOMPARE(processor.format().sampleFormat(), QAudioFormat::Int16);
}

void MediaTest::calculatesAudioPeakLevel()
{
    AudioProcessor processor;
    processor.configure({16000, 1, false});
    QSignalSpy levelSpy(&processor, &AudioProcessor::levelChanged);
    QByteArray payload;
    const qint16 samples[] = {0, 16384, -32768};
    payload.append(reinterpret_cast<const char *>(samples), sizeof(samples));

    processor.processPcmFrame(payload, 3, 4);

    QCOMPARE(levelSpy.count(), 1);
    QCOMPARE(processor.bytesReceived(), static_cast<qint64>(sizeof(samples)));
    QVERIFY(processor.peakLevel() > 0.99);
}

void MediaTest::decodesFortyEightChannelFrame()
{
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    for (int samplePoint = 0; samplePoint < AudioFrameDecoder::SamplesPerFrame; ++samplePoint) {
        for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
            putSample(payload, samplePoint, channel, channel + 1 + samplePoint);
        }
    }

    AudioFrameDecoder decoder;
    const DecodedAudioFrame frame = decoder.decode(payload, 7, 8);

    QVERIFY(frame.valid);
    QCOMPARE(frame.samples.size(), AudioFrameDecoder::ChannelCount);
    QCOMPARE(frame.samplePoints, AudioFrameDecoder::SamplesPerFrame);
    QCOMPARE(frame.samples.at(0), 200);
    QCOMPARE(frame.samples.at(47), 247);
    QCOMPARE(frame.amplitudes.at(0), 100.5);
    QCOMPARE(frame.timestampSeconds, 7u);
    QCOMPARE(frame.timestampMicros, 8u);
}

void MediaTest::extractsSelectedChannelForPlayback()
{
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    const int selectedChannel = 12;
    const qint32 sample = 0x00400000;
    for (int samplePoint = 0; samplePoint < AudioFrameDecoder::SamplesPerFrame; ++samplePoint) {
        putSample(payload, samplePoint, selectedChannel, sample);
    }

    AudioFrameDecoder decoder;
    decoder.setOutputChannel(selectedChannel);
    const DecodedAudioFrame frame = decoder.decode(payload, 1, 2);

    QVERIFY(frame.valid);
    QCOMPARE(frame.outputPcm16.size(), AudioFrameDecoder::SamplesPerFrame * 2);
    const qint16 pcm = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(frame.outputPcm16.constData()));
    QVERIFY(pcm > 16000);
}

void MediaTest::normalizesArrayAmplitudeWindow()
{
    AudioFrameDecoder decoder;
    decoder.setAverageWindowFrames(2);

    QByteArray first(AudioFrameDecoder::FramePayloadBytes, char(0));
    QByteArray second(AudioFrameDecoder::FramePayloadBytes, char(0));
    for (int samplePoint = 0; samplePoint < AudioFrameDecoder::SamplesPerFrame; ++samplePoint) {
        for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
            const qint32 firstSample = static_cast<qint32>((channel + 1) * 10);
            const qint32 secondSample = static_cast<qint32>((channel + 1) * 30);
            putSample(first, samplePoint, channel, firstSample);
            putSample(second, samplePoint, channel, secondSample);
        }
    }

    decoder.decode(first, 1, 0);
    const DecodedAudioFrame frame = decoder.decode(second, 2, 0);

    QVERIFY(frame.valid);
    QCOMPARE(frame.averageAmplitudes.size(), AudioFrameDecoder::ChannelCount);
    QCOMPARE(frame.normalizedAmplitudes.size(), AudioFrameDecoder::ChannelCount);
    QCOMPARE(frame.averageAmplitudes.at(0), 20.0);
    QCOMPARE(frame.averageAmplitudes.at(47), 960.0);
    QCOMPARE(frame.normalizedAmplitudes.at(47), 1.0);
    QVERIFY(frame.normalizedAmplitudes.at(0) > 0.02);
    QVERIFY(frame.normalizedAmplitudes.at(0) < 0.03);
}

QTEST_MAIN(MediaTest)
#include "test_media.moc"
