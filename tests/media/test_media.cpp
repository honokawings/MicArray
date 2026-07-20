#include <QtTest/QtTest>

#include <QBuffer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>

#include <memory>

#include "media/audioframedecoder.h"
#include "media/audioprocessor.h"
#include "media/audiorecorder.h"
#include "media/videodecoder.h"
#include "media/mediaframequeue.h"
#include "media/multichannelwavwriter.h"

using namespace MicArray;

namespace {

void putSample(QByteArray &payload, int samplePoint, int channel, qint32 sample)
{
    const int offset = samplePoint * AudioFrameDecoder::BytesPerSamplePoint
        + channel * AudioFrameDecoder::BytesPerChannel;
    qToLittleEndian(sample, reinterpret_cast<uchar *>(payload.data() + offset));
}

quint16 readLe16(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

quint32 readLe32(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

class FailingWriteFile : public QFile
{
public:
    FailingWriteFile(const QString &path, int failingWriteCall, bool shortWrite)
        : QFile(path)
        , m_failingWriteCall(failingWriteCall)
        , m_shortWrite(shortWrite)
    {
    }

protected:
    qint64 writeData(const char *data, qint64 length) override
    {
        ++m_writeCalls;
        if (m_writeCalls == m_failingWriteCall) {
            if (!m_shortWrite) {
                setErrorString(QStringLiteral("Injected write failure"));
                return -1;
            }
            const qint64 partial = length / 2;
            return QFile::writeData(data, partial);
        }
        return QFile::writeData(data, length);
    }

private:
    int m_failingWriteCall = 0;
    int m_writeCalls = 0;
    bool m_shortWrite = false;
};

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
    void boundedFrameQueueDropsOldestAudioFrame();
    void singleFrameQueueKeepsLatestVideoFrame();
    void mediaProcessorsSignalCompletionForInvalidFrames();
    void writesSelectedChannelsToExtensibleWav();
    void rollsWavSegmentsOnWholeFrameBoundaries();
    void rejectsInvalidRecordingPayload();
    void rejectsInvalidRecordingSettingsAndRepeatedLifecycle();
    void keepsManifestConsistentAfterShortWrite();
    void keepsManifestConsistentAfterHeaderFinalizeFailure();
    void keepsUndeletableDamagedSegmentVisibleInManifest();
    void rejectsExistingRecordingArtifacts();
    void reservesManifestBeforeCreatingWav();
    void recordsUndeletableInitialHeaderFailure();
    void recordsAudioFramesAsynchronously();
    void rejectsInvalidAndRepeatedRecorderStarts();
    void stopsRecordingOnQueueOverflow();
    void preservesFormatFailureAfterQueuedFramesAndStop();
    void preservesAsynchronousStartFailureAfterImmediateStop();
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

void MediaTest::boundedFrameQueueDropsOldestAudioFrame()
{
    MediaFrameQueue queue(2);
    HostFrame first;
    first.timestampMicros = 1;
    HostFrame second;
    second.timestampMicros = 2;
    HostFrame third;
    third.timestampMicros = 3;

    QVERIFY(!queue.push(first));
    QVERIFY(!queue.push(second));
    QVERIFY(queue.push(third));
    QCOMPARE(queue.size(), 2);
    QCOMPARE(queue.droppedFrames(), quint64(1));
    QCOMPARE(queue.takeNext().timestampMicros, quint32(2));
    QCOMPARE(queue.takeNext().timestampMicros, quint32(3));
}

void MediaTest::singleFrameQueueKeepsLatestVideoFrame()
{
    MediaFrameQueue queue(1);
    HostFrame first;
    first.timestampSeconds = 1;
    HostFrame latest;
    latest.timestampSeconds = 2;
    queue.push(first);
    QVERIFY(queue.push(latest));
    QCOMPARE(queue.takeNext().timestampSeconds, quint32(2));
}

void MediaTest::mediaProcessorsSignalCompletionForInvalidFrames()
{
    AudioProcessor audio;
    VideoDecoder video;
    QSignalSpy audioSpy(&audio, &AudioProcessor::frameProcessed);
    QSignalSpy videoSpy(&video, &VideoDecoder::frameProcessed);

    audio.processArrayFrame(QByteArray("bad"), 1, 2);
    video.decodeJpeg(QByteArray("bad"), 1, 2);

    QCOMPARE(audioSpy.count(), 1);
    QCOMPARE(audioSpy.first().at(0).toBool(), false);
    QCOMPARE(videoSpy.count(), 1);
    QCOMPARE(videoSpy.first().at(0).toBool(), false);
}

void MediaTest::writesSelectedChannelsToExtensibleWav()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    RecordingSettings settings;
    settings.outputDirectory = directory.path();
    settings.baseName = QStringLiteral("selected_channels");
    settings.channels = {1, 4};
    settings.sampleRate = 48000;

    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    putSample(payload, 0, 1, 0x01020304);
    putSample(payload, 0, 4, 0x11121314);
    putSample(payload, 1, 1, 0x21222324);
    putSample(payload, 1, 4, 0x31323334);

    MultiChannelWavWriter writer;
    QString error;
    QVERIFY2(writer.open(settings, &error), qPrintable(error));
    QVERIFY2(writer.appendFrame(payload, &error), qPrintable(error));
    QVERIFY2(writer.close(QStringLiteral("user"), &error), qPrintable(error));

    const RecordingSummary summary = writer.summary();
    QCOMPARE(summary.segmentPaths.size(), 1);
    QCOMPARE(summary.inputFrames, quint64(1));
    QCOMPARE(summary.sampleFrames, quint64(AudioFrameDecoder::SamplesPerFrame));
    QCOMPARE(summary.dataBytes, quint64(AudioFrameDecoder::SamplesPerFrame * 2 * 4));

    QFile wav(summary.segmentPaths.first());
    QVERIFY(wav.open(QIODevice::ReadOnly));
    const QByteArray bytes = wav.readAll();
    QCOMPARE(bytes.mid(0, 4), QByteArray("RIFF"));
    QCOMPARE(bytes.mid(8, 4), QByteArray("WAVE"));
    QCOMPARE(bytes.mid(12, 4), QByteArray("fmt "));
    QCOMPARE(readLe32(bytes, 16), quint32(40));
    QCOMPARE(readLe16(bytes, 20), quint16(0xfffe));
    QCOMPARE(readLe16(bytes, 22), quint16(2));
    QCOMPARE(readLe32(bytes, 24), quint32(48000));
    QCOMPARE(readLe16(bytes, 34), quint16(32));
    QCOMPARE(readLe16(bytes, 38), quint16(24));
    QCOMPARE(readLe32(bytes, 40), quint32(0));
    QCOMPARE(bytes.mid(44, 16).toHex(), QByteArray("0100000000001000800000aa00389b71"));
    QCOMPARE(bytes.mid(60, 4), QByteArray("data"));
    QCOMPARE(readLe32(bytes, 64), quint32(AudioFrameDecoder::SamplesPerFrame * 2 * 4));
    QCOMPARE(readLe32(bytes, 4), quint32(bytes.size() - 8));
    QCOMPARE(bytes.mid(68, 16).toHex(), QByteArray("04030201141312112423222134333231"));

    QFile manifest(summary.manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const QJsonObject json = QJsonDocument::fromJson(manifest.readAll()).object();
    QCOMPARE(json.value(QStringLiteral("channels")).toArray().at(0).toInt(), 1);
    QCOMPARE(json.value(QStringLiteral("channels")).toArray().at(1).toInt(), 4);
    QCOMPARE(json.value(QStringLiteral("inputFrames")).toDouble(), 1.0);
    QCOMPARE(json.value(QStringLiteral("stopReason")).toString(), QStringLiteral("user"));
}

void MediaTest::rollsWavSegmentsOnWholeFrameBoundaries()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    RecordingSettings settings;
    settings.outputDirectory = directory.path();
    settings.baseName = QStringLiteral("segmented");
    settings.channels = {0, 47};
    settings.sampleRate = 48000;
    const quint64 outputFrameBytes = AudioFrameDecoder::SamplesPerFrame * 2 * 4;

    MultiChannelWavWriter writer(outputFrameBytes * 2);
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    QString error;
    QVERIFY2(writer.open(settings, &error), qPrintable(error));
    QVERIFY2(writer.appendFrame(payload, &error), qPrintable(error));
    QVERIFY2(writer.appendFrame(payload, &error), qPrintable(error));
    QVERIFY2(writer.appendFrame(payload, &error), qPrintable(error));
    QVERIFY2(writer.close(QStringLiteral("user"), &error), qPrintable(error));

    const RecordingSummary summary = writer.summary();
    QCOMPARE(summary.segmentPaths.size(), 2);
    QCOMPARE(summary.inputFrames, quint64(3));

    QFile first(summary.segmentPaths.at(0));
    QFile second(summary.segmentPaths.at(1));
    QVERIFY(first.open(QIODevice::ReadOnly));
    QVERIFY(second.open(QIODevice::ReadOnly));
    const QByteArray firstBytes = first.readAll();
    const QByteArray secondBytes = second.readAll();
    QCOMPARE(readLe32(firstBytes, 64), quint32(outputFrameBytes * 2));
    QCOMPARE(readLe32(secondBytes, 64), quint32(outputFrameBytes));
    QVERIFY(summary.segmentPaths.at(1).endsWith(QStringLiteral("_part002.wav")));

    QFile manifest(summary.manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const QJsonArray segments = QJsonDocument::fromJson(manifest.readAll())
                                    .object()
                                    .value(QStringLiteral("segments"))
                                    .toArray();
    QCOMPARE(segments.size(), 2);
    QCOMPARE(segments.at(0).toObject().value(QStringLiteral("dataBytes")).toDouble(),
             static_cast<double>(outputFrameBytes * 2));
    QCOMPARE(segments.at(1).toObject().value(QStringLiteral("dataBytes")).toDouble(),
             static_cast<double>(outputFrameBytes));
}

void MediaTest::rejectsInvalidRecordingPayload()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    RecordingSettings settings;
    settings.outputDirectory = directory.path();
    settings.baseName = QStringLiteral("invalid_payload");
    settings.channels = {0};

    MultiChannelWavWriter writer;
    QString error;
    QVERIFY(writer.open(settings, &error));
    QVERIFY(!writer.appendFrame(QByteArray(10, char(0)), &error));
    QVERIFY(error.contains(QStringLiteral("38400")));
    QVERIFY(writer.close(QStringLiteral("format-error"), &error));
}

void MediaTest::rejectsInvalidRecordingSettingsAndRepeatedLifecycle()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    MultiChannelWavWriter writer;
    RecordingSettings settings;
    settings.outputDirectory = QString();
    settings.baseName = QStringLiteral("invalid");
    settings.channels = {0};
    QString error;
    QVERIFY(!writer.open(settings, &error));

    settings.outputDirectory = directory.path();
    settings.channels = {0, 0};
    QVERIFY(!writer.open(settings, &error));
    settings.channels = {48};
    QVERIFY(!writer.open(settings, &error));

    settings.channels = {0};
    QVERIFY(writer.open(settings, &error));
    QVERIFY(!writer.open(settings, &error));
    QVERIFY(writer.close(QStringLiteral("user"), &error));
    QVERIFY(!writer.close(QStringLiteral("user"), &error));
}

void MediaTest::keepsManifestConsistentAfterShortWrite()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    RecordingSettings settings {directory.path(), QStringLiteral("short_write"), {0}, 48000};
    MultiChannelWavWriter writer(
        MultiChannelWavWriter::DefaultSegmentDataLimit,
        [](const QString &path) {
            return std::make_unique<FailingWriteFile>(path, 2, true);
        });

    QString error;
    QVERIFY(writer.open(settings, &error));
    QVERIFY(!writer.appendFrame(QByteArray(AudioFrameDecoder::FramePayloadBytes, char(0)), &error));
    QVERIFY(error.contains(QStringLiteral("Short WAV write")));
    QVERIFY(!writer.close(QStringLiteral("io-error"), &error));

    const RecordingSummary summary = writer.summary();
    QCOMPARE(summary.inputFrames, quint64(0));
    QCOMPARE(summary.dataBytes, quint64(0));
    QCOMPARE(summary.segmentPaths.size(), 0);
    QVERIFY(QFileInfo::exists(summary.manifestPath));
    QFile manifest(summary.manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(manifest.readAll()).object();
    QCOMPARE(root.value(QStringLiteral("segments")).toArray().size(), 0);
    QCOMPARE(root.value(QStringLiteral("dataBytes")).toDouble(), 0.0);
    QCOMPARE(root.value(QStringLiteral("stopReason")).toString(), QStringLiteral("io-error"));
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("short_write.wav"))));
}

void MediaTest::keepsManifestConsistentAfterHeaderFinalizeFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    RecordingSettings settings {directory.path(), QStringLiteral("finalize_failure"), {0}, 48000};
    MultiChannelWavWriter writer(
        MultiChannelWavWriter::DefaultSegmentDataLimit,
        [](const QString &path) {
            return std::make_unique<FailingWriteFile>(path, 3, false);
        });

    QString error;
    QVERIFY(writer.open(settings, &error));
    QVERIFY(writer.appendFrame(QByteArray(AudioFrameDecoder::FramePayloadBytes, char(0)), &error));
    QVERIFY(!writer.close(QStringLiteral("io-error"), &error));

    const RecordingSummary summary = writer.summary();
    QCOMPARE(summary.inputFrames, quint64(0));
    QCOMPARE(summary.dataBytes, quint64(0));
    QCOMPARE(summary.segmentPaths.size(), 0);
    QVERIFY(QFileInfo::exists(summary.manifestPath));
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("finalize_failure.wav"))));
}

void MediaTest::keepsUndeletableDamagedSegmentVisibleInManifest()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    RecordingSettings settings {directory.path(), QStringLiteral("undeletable"), {0}, 48000};
    MultiChannelWavWriter writer(
        MultiChannelWavWriter::DefaultSegmentDataLimit,
        [](const QString &path) {
            return std::make_unique<FailingWriteFile>(path, 2, true);
        },
        [](const QString &) {
            return false;
        });

    QString error;
    QVERIFY(writer.open(settings, &error));
    QVERIFY(!writer.appendFrame(QByteArray(AudioFrameDecoder::FramePayloadBytes, char(0)), &error));
    QVERIFY(!writer.close(QStringLiteral("io-error"), &error));
    QVERIFY(error.contains(QStringLiteral("could not be removed")));

    const RecordingSummary summary = writer.summary();
    QCOMPARE(summary.segmentPaths.size(), 1);
    QFile manifest(summary.manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const QJsonArray segments = QJsonDocument::fromJson(manifest.readAll())
                                    .object()
                                    .value(QStringLiteral("segments"))
                                    .toArray();
    QCOMPARE(segments.size(), 1);
    QVERIFY(!segments.at(0).toObject().value(QStringLiteral("valid")).toBool());
}

void MediaTest::rejectsExistingRecordingArtifacts()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString manifestPath = directory.filePath(QStringLiteral("collision.json"));
    QFile existingManifest(manifestPath);
    QVERIFY(existingManifest.open(QIODevice::WriteOnly));
    QCOMPARE(existingManifest.write("keep"), qint64(4));
    existingManifest.close();

    RecordingSettings settings {directory.path(), QStringLiteral("collision"), {0}, 48000};
    MultiChannelWavWriter writer;
    QString error;
    QVERIFY(!writer.open(settings, &error));
    QVERIFY(error.contains(QStringLiteral("already exists")));

    QVERIFY(existingManifest.open(QIODevice::ReadOnly));
    QCOMPARE(existingManifest.readAll(), QByteArray("keep"));
}

void MediaTest::reservesManifestBeforeCreatingWav()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString manifestPath = directory.filePath(QStringLiteral("reserved.json"));
    bool manifestObserved = false;

    MultiChannelWavWriter writer(
        MultiChannelWavWriter::DefaultSegmentDataLimit,
        [&manifestObserved, &manifestPath](const QString &path) {
            QFile manifest(manifestPath);
            manifestObserved = manifest.open(QIODevice::ReadOnly)
                && manifest.readAll().contains("recording");
            return std::make_unique<QFile>(path);
        });
    RecordingSettings settings {directory.path(), QStringLiteral("reserved"), {0}, 48000};
    QString error;
    QVERIFY2(writer.open(settings, &error), qPrintable(error));
    QVERIFY(manifestObserved);
    QVERIFY(writer.close(QStringLiteral("user"), &error));
}

void MediaTest::recordsUndeletableInitialHeaderFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    MultiChannelWavWriter writer(
        MultiChannelWavWriter::DefaultSegmentDataLimit,
        [](const QString &path) {
            return std::make_unique<FailingWriteFile>(path, 1, false);
        },
        [](const QString &) {
            return false;
        });
    RecordingSettings settings {directory.path(), QStringLiteral("header_failure"), {0}, 48000};
    QString error;
    QVERIFY(!writer.open(settings, &error));
    QVERIFY(error.contains(QStringLiteral("WAV header")));

    const RecordingSummary summary = writer.summary();
    QCOMPARE(summary.segmentPaths.size(), 1);
    QVERIFY(QFileInfo::exists(summary.manifestPath));
    QFile manifest(summary.manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(manifest.readAll()).object();
    QCOMPARE(root.value(QStringLiteral("stopReason")).toString(), QStringLiteral("start-error"));
    const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    QCOMPARE(segments.size(), 1);
    QVERIFY(!segments.first().toObject().value(QStringLiteral("valid")).toBool());
}

void MediaTest::recordsAudioFramesAsynchronously()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    AudioRecorder recorder;
    RecordingSettings settings {directory.path(), QStringLiteral("async_recording"), {0, 7}, 48000};
    QVERIFY(recorder.startRecording(settings));
    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Recording, 3000);

    const QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    QVERIFY(recorder.enqueueFrame(payload));
    QTRY_COMPARE_WITH_TIMEOUT(recorder.statistics().inputFrames, quint64(1), 3000);

    recorder.stopRecording(QStringLiteral("user"));
    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Idle, 3000);
    const RecordingStatistics statistics = recorder.statistics();
    QCOMPARE(statistics.dataBytes,
             quint64(AudioFrameDecoder::SamplesPerFrame * 2 * AudioFrameDecoder::BytesPerChannel));
    QCOMPARE(statistics.fileBytes, statistics.dataBytes + quint64(68));
    QVERIFY(QFileInfo::exists(statistics.outputPath));
}

void MediaTest::rejectsInvalidAndRepeatedRecorderStarts()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    AudioRecorder recorder;
    RecordingSettings invalid {directory.path(), QStringLiteral("invalid_recorder"), {}, 48000};
    QVERIFY(!recorder.startRecording(invalid));
    QCOMPARE(recorder.state(), RecordingState::Error);

    RecordingSettings valid {directory.path(), QStringLiteral("recovered_recorder"), {3}, 48000};
    QVERIFY(recorder.startRecording(valid));
    QVERIFY(!recorder.startRecording(valid));
    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Recording, 3000);
    QVERIFY(recorder.stopAndWait());
    QCOMPARE(recorder.state(), RecordingState::Idle);
}

void MediaTest::stopsRecordingOnQueueOverflow()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    AudioRecorder recorder(AudioFrameDecoder::FramePayloadBytes - 1);
    RecordingSettings settings {directory.path(), QStringLiteral("queue_overflow"), {0}, 48000};
    QVERIFY(recorder.startRecording(settings));
    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Recording, 3000);

    QVERIFY(!recorder.enqueueFrame(QByteArray(AudioFrameDecoder::FramePayloadBytes, char(0))));
    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Error, 3000);
    QVERIFY(recorder.statistics().error.contains(QStringLiteral("queue"), Qt::CaseInsensitive));
}

void MediaTest::preservesFormatFailureAfterQueuedFramesAndStop()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    AudioRecorder recorder;
    RecordingSettings settings {directory.path(), QStringLiteral("format_failure"), {0}, 48000};
    QVERIFY(recorder.startRecording(settings));
    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Recording, 3000);

    QVERIFY(recorder.enqueueFrame(QByteArray(10, char(0))));
    QVERIFY(recorder.enqueueFrame(QByteArray(AudioFrameDecoder::FramePayloadBytes, char(0))));
    recorder.stopRecording(QStringLiteral("user"));

    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Error, 3000);
    const RecordingStatistics statistics = recorder.statistics();
    QCOMPARE(statistics.inputFrames, quint64(0));
    QCOMPARE(statistics.discardedFrames, quint64(1));
    QVERIFY(statistics.error.contains(QStringLiteral("38400")));
    QVERIFY(QFileInfo::exists(statistics.outputPath));

    QFile manifest(statistics.outputPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(manifest.readAll()).object();
    QCOMPARE(root.value(QStringLiteral("stopReason")).toString(), QStringLiteral("format-error"));
}

void MediaTest::preservesAsynchronousStartFailureAfterImmediateStop()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    AudioRecorder recorder;
    RecordingSettings settings {
        directory.filePath(QStringLiteral("missing")),
        QStringLiteral("start_failure"),
        {0},
        48000,
    };
    QVERIFY(recorder.startRecording(settings));
    recorder.stopRecording(QStringLiteral("user"));

    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Error, 3000);
    QVERIFY(recorder.statistics().error.contains(QStringLiteral("does not exist")));
}

QTEST_MAIN(MediaTest)
#include "test_media.moc"
