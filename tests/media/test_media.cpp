#include <QtTest/QtTest>

#include <QBuffer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>

#include <limits>
#include <memory>
#include <numbers>
#include <random>

#include "control/directioncontrolmapper.h"
#include "media/audioframedecoder.h"
#include "media/audioprocessor.h"
#include "media/audiorecorder.h"
#include "media/bandpassfilter.h"
#include "media/channelgainprocessor.h"
#include "media/gainprofilestore.h"
#include "media/videodecoder.h"
#include "media/mediaframequeue.h"
#include "media/multichannelwavwriter.h"
#include "media/noisereductionframedecoder.h"
#include "media/noisereductionworker.h"
#include "media/wienernoisereducer.h"

using namespace MicArray;

namespace {

void putSample(QByteArray &payload, int samplePoint, int channel, qint32 sample)
{
    const int offset = samplePoint * AudioFrameDecoder::BytesPerSamplePoint
        + channel * AudioFrameDecoder::BytesPerChannel;
    qToLittleEndian(sample, reinterpret_cast<uchar *>(payload.data() + offset));
}

qint32 readSample(const QByteArray &payload, int samplePoint, int channel)
{
    const int offset = samplePoint * AudioFrameDecoder::BytesPerSamplePoint
        + channel * AudioFrameDecoder::BytesPerChannel;
    return qFromLittleEndian<qint32>(
        reinterpret_cast<const uchar *>(payload.constData() + offset));
}

double rootMeanSquareError(const QVector<float> &actual,
                           const QVector<float> &expected,
                           int skipSamples)
{
    const int count = std::min(actual.size(), expected.size());
    double sum = 0.0;
    int used = 0;
    for (int index = skipSamples; index < count; ++index) {
        const double difference =
            static_cast<double>(actual.at(index) - expected.at(index));
        sum += difference * difference;
        ++used;
    }
    return used > 0 ? std::sqrt(sum / static_cast<double>(used)) : 0.0;
}

double toneAmplitude(const QVector<float> &samples,
                     double frequencyHz,
                     int sampleRate,
                     int skipSamples)
{
    double sine = 0.0;
    double cosine = 0.0;
    int used = 0;
    for (int index = skipSamples; index < samples.size(); ++index) {
        const double phase =
            2.0 * std::numbers::pi * frequencyHz * index / sampleRate;
        sine += samples.at(index) * std::sin(phase);
        cosine += samples.at(index) * std::cos(phase);
        ++used;
    }
    return used > 0
        ? 2.0 * std::hypot(sine, cosine) / static_cast<double>(used)
        : 0.0;
}

void fillChannelSamples(QByteArray &payload, int channel, qint32 sample)
{
    for (int samplePoint = 0; samplePoint < AudioFrameDecoder::SamplesPerFrame; ++samplePoint) {
        putSample(payload, samplePoint, channel, sample);
    }
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
    void appliesChannelShiftsWithSaturation();
    void accumulatesClipCountsOnlyForClippedSamples();
    void preservesUntargetedGainChannels();
    void rejectsInvalidGainPayload();
    void rejectsSampleRateWithZeroFrameDuration();
    void rejectsInvalidGainConfigurationAtomically();
    void automaticGainAttacksStrongChannelIndependently();
    void automaticGainHonorsReleaseHoldAndStep();
    void automaticGainAccumulatesTruncatedFrameDurationAt48kHz();
    void automaticGainLongFrameDoesNotOvershootThresholds();
    void automaticGainRaisesSilenceToMaximum();
    void changingOneGainChannelPreservesOtherAgcState();
    void savesAndLoadsGainProfileWithAllChannels();
    void rejectsInvalidGainProfileDocuments();
    void rejectsGainProfileShapeAndTypeViolations();
    void keepsExistingGainProfileWhenSaveFails();
    void keepsExistingGainProfileWhenAtomicCommitFails();
    void reportsUnwritableGainProfileTarget();
    void boundedFrameQueueDropsOldestAudioFrame();
    void singleFrameQueueKeepsLatestVideoFrame();
    void mediaProcessorsSignalCompletionForInvalidFrames();
    void audioProcessorDistributesGainAdjustedFrame();
    void recordsOnlyGainAdjustedAudioFrames();
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
    void recorderRejectsInvalidPayloadBeforeQueueing();
    void recorderBoundsPendingFrameCount();
    void recordsGainMetadataInManifest();
    void rejectsInvalidAndRepeatedRecorderStarts();
    void stopsRecordingOnQueueOverflow();
    void preservesFormatFailureAfterQueuedFramesAndStop();
    void preservesAsynchronousStartFailureAfterImmediateStop();
    void mapsNoiseReductionDirectionsToSixControlInterfaces();
    void quantizesContinuousNoiseDirectionsOnlyAtControlBoundary();
    void clampsAndRejectsInvalidNoiseReductionDirectionMappings();
    void decodesOnlyFirstFourNoiseReductionChannels();
    void rejectsInvalidNoiseReductionPayload();
    void suppressesOneAndTwoReferenceComponentsWithWienerFilter();
    void appliesFixedWienerCoefficientsIndependently();
    void keepsWienerOutputFiniteForSilentAndDegenerateReferences();
    void appliesAndResetsFourthOrderBandPassFilter();
    void noiseReductionWorkerConsumesRawFramesAndPublishesSelectedSources();
    void filtersDenoisedWorkerOutputWhileMonitoringIsDisabled();
    void computesNoiseReductionSpectrumAndSaturatingMonitorGain();
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

void MediaTest::appliesChannelShiftsWithSaturation()
{
    ChannelGainProcessor processor;
    GainConfiguration configuration;
    configuration.shifts[0] = 1;
    configuration.shifts[1] = -1;
    configuration.shifts[2] = -1;
    configuration.shifts[3] = 1;
    configuration.shifts[4] = 1;
    QString error;
    QVERIFY2(processor.setConfiguration(configuration, &error), qPrintable(error));

    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    putSample(payload, 0, 0, 123);
    putSample(payload, 0, 1, 9);
    putSample(payload, 0, 2, -3);
    putSample(payload, 0, 3, std::numeric_limits<qint32>::max());
    putSample(payload, 0, 4, std::numeric_limits<qint32>::min());

    const GainProcessResult result = processor.process(payload, 48000);

    QVERIFY2(result.valid, qPrintable(result.error));
    QCOMPARE(result.payload.size(), AudioFrameDecoder::FramePayloadBytes);
    QCOMPARE(readSample(result.payload, 0, 0), qint32(246));
    QCOMPARE(readSample(result.payload, 0, 1), qint32(4));
    QCOMPARE(readSample(result.payload, 0, 2), qint32(-2));
    QCOMPARE(readSample(result.payload, 0, 3), std::numeric_limits<qint32>::max());
    QCOMPARE(readSample(result.payload, 0, 4), std::numeric_limits<qint32>::min());
    QCOMPARE(result.snapshot.clipCounts.at(3), quint64(1));
    QCOMPARE(result.snapshot.clipCounts.at(4), quint64(1));
}

void MediaTest::accumulatesClipCountsOnlyForClippedSamples()
{
    ChannelGainProcessor processor;
    GainConfiguration configuration;
    configuration.shifts[0] = 1;
    QVERIFY(processor.setConfiguration(configuration));

    QByteArray first(AudioFrameDecoder::FramePayloadBytes, char(0));
    putSample(first, 0, 0, std::numeric_limits<qint32>::max());
    putSample(first, 1, 0, std::numeric_limits<qint32>::min());
    putSample(first, 2, 0, 1'073'741'823);
    putSample(first, 0, 1, std::numeric_limits<qint32>::max());
    GainProcessResult result = processor.process(first, 48000);
    QCOMPARE(result.snapshot.clipCounts.at(0), quint64(2));
    QCOMPARE(result.snapshot.clipCounts.at(1), quint64(0));

    QByteArray second(AudioFrameDecoder::FramePayloadBytes, char(0));
    putSample(second, 17, 0, std::numeric_limits<qint32>::max());
    putSample(second, 18, 0, 1000);
    result = processor.process(second, 48000);
    QCOMPARE(result.snapshot.clipCounts.at(0), quint64(3));

    const QByteArray noClipping(AudioFrameDecoder::FramePayloadBytes, char(1));
    result = processor.process(noClipping, 48000);
    QCOMPARE(result.snapshot.clipCounts.at(0), quint64(3));
    QCOMPARE(result.snapshot.clipCounts.at(1), quint64(0));
}

void MediaTest::preservesUntargetedGainChannels()
{
    ChannelGainProcessor processor;
    GainConfiguration configuration;
    configuration.shifts[17] = 1;
    QVERIFY(processor.setConfiguration(configuration));

    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    for (int samplePoint = 0; samplePoint < AudioFrameDecoder::SamplesPerFrame; ++samplePoint) {
        for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
            putSample(payload, samplePoint, channel, samplePoint * 1000 + channel);
        }
    }

    const GainProcessResult result = processor.process(payload, 48000);

    QVERIFY2(result.valid, qPrintable(result.error));
    for (int samplePoint = 0; samplePoint < AudioFrameDecoder::SamplesPerFrame; ++samplePoint) {
        for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
            const qint32 expected = channel == 17
                ? (samplePoint * 1000 + channel) * 2
                : samplePoint * 1000 + channel;
            QCOMPARE(readSample(result.payload, samplePoint, channel), expected);
        }
    }
}

void MediaTest::rejectsInvalidGainPayload()
{
    ChannelGainProcessor processor;

    const GainProcessResult result = processor.process(QByteArray(38399, char(0)), 48000);

    QVERIFY(!result.valid);
    QVERIFY(result.payload.isEmpty());
    QVERIFY(result.error.contains(QStringLiteral("38400")));
}

void MediaTest::rejectsSampleRateWithZeroFrameDuration()
{
    ChannelGainProcessor processor;
    const QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));

    const GainProcessResult result = processor.process(payload, 200'000'001);

    QVERIFY(!result.valid);
    QVERIFY(result.payload.isEmpty());
    QVERIFY(result.error.contains(QStringLiteral("duration"), Qt::CaseInsensitive));
    QVERIFY(result.error.contains(QStringLiteral("microsecond"), Qt::CaseInsensitive));
}

void MediaTest::rejectsInvalidGainConfigurationAtomically()
{
    ChannelGainProcessor processor;
    GainConfiguration initial;
    initial.shifts[0] = 2;
    initial.automatic[1] = true;
    QVERIFY(processor.setConfiguration(initial));

    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    fillChannelSamples(payload, 0, std::numeric_limits<qint32>::max());
    fillChannelSamples(payload, 1, 107'374'182);
    for (int frame = 0; frame < 4; ++frame) {
        QVERIFY(processor.process(payload, 2000).valid);
    }

    const GainConfiguration baselineConfiguration = processor.configuration();
    const GainSnapshot baselineSnapshot = processor.snapshot();
    QCOMPARE(baselineSnapshot.clipCounts.at(0), quint64(800));

    QList<QPair<GainConfiguration, QString>> invalidConfigurations;
    GainConfiguration invalid = baselineConfiguration;
    invalid.shifts.resize(GainConfiguration::ChannelCount - 1);
    invalidConfigurations.append({invalid, QStringLiteral("shifts")});
    invalid = baselineConfiguration;
    invalid.automatic.append(false);
    invalidConfigurations.append({invalid, QStringLiteral("Automatic")});
    invalid = baselineConfiguration;
    invalid.shifts[47] = 9;
    invalidConfigurations.append({invalid, QStringLiteral("shift")});
    invalid = baselineConfiguration;
    invalid.upperPeakRatio = std::numeric_limits<double>::quiet_NaN();
    invalidConfigurations.append({invalid, QStringLiteral("upper")});
    invalid = baselineConfiguration;
    invalid.lowerPeakRatio = std::numeric_limits<double>::infinity();
    invalidConfigurations.append({invalid, QStringLiteral("lower")});
    invalid = baselineConfiguration;
    invalid.upperPeakRatio = 1.01;
    invalidConfigurations.append({invalid, QStringLiteral("upper")});
    invalid = baselineConfiguration;
    invalid.lowerPeakRatio = -0.01;
    invalidConfigurations.append({invalid, QStringLiteral("lower")});
    invalid = baselineConfiguration;
    invalid.lowerPeakRatio = invalid.upperPeakRatio;
    invalidConfigurations.append({invalid, QStringLiteral("lower")});
    invalid = baselineConfiguration;
    invalid.releaseHoldMs = 0;
    invalidConfigurations.append({invalid, QStringLiteral("hold")});
    invalid = baselineConfiguration;
    invalid.releaseStepMs = -1;
    invalidConfigurations.append({invalid, QStringLiteral("step")});

    for (const auto &[candidate, errorFragment] : invalidConfigurations) {
        QString error;
        QVERIFY(!processor.setConfiguration(candidate, &error));
        QVERIFY2(error.contains(errorFragment, Qt::CaseInsensitive), qPrintable(error));
        QVERIFY(processor.configuration() == baselineConfiguration);
        QCOMPARE(processor.snapshot().shifts, baselineSnapshot.shifts);
        QCOMPARE(processor.snapshot().automatic, baselineSnapshot.automatic);
        QCOMPARE(processor.snapshot().inputPeakRatios, baselineSnapshot.inputPeakRatios);
        QCOMPARE(processor.snapshot().outputPeakRatios, baselineSnapshot.outputPeakRatios);
        QCOMPARE(processor.snapshot().clipCounts, baselineSnapshot.clipCounts);
    }

    GainProcessResult result;
    for (int frame = 0; frame < 4; ++frame) {
        result = processor.process(payload, 2000);
    }
    QCOMPARE(result.snapshot.shifts.at(1), 1);
    QCOMPARE(result.snapshot.clipCounts.at(0), quint64(1600));
}

void MediaTest::automaticGainAttacksStrongChannelIndependently()
{
    ChannelGainProcessor processor;
    GainConfiguration configuration;
    configuration.automatic[0] = true;
    configuration.automatic[1] = true;
    QVERIFY(processor.setConfiguration(configuration));

    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    fillChannelSamples(payload, 0, 1'932'735'283);
    fillChannelSamples(payload, 1, 1'073'741'824);

    const GainProcessResult result = processor.process(payload, 48000);

    QVERIFY2(result.valid, qPrintable(result.error));
    QCOMPARE(result.snapshot.shifts.at(0), -1);
    QCOMPARE(result.snapshot.shifts.at(1), 0);
    QCOMPARE(readSample(result.payload, 0, 0), qint32(966'367'641));
    QCOMPARE(readSample(result.payload, 0, 1), qint32(1'073'741'824));
    QVERIFY(result.snapshot.inputPeakRatios.at(0) > 0.89);
    QVERIFY(result.snapshot.outputPeakRatios.at(0) < 0.80);
}

void MediaTest::automaticGainHonorsReleaseHoldAndStep()
{
    ChannelGainProcessor processor;
    GainConfiguration configuration;
    configuration.automatic[0] = true;
    QVERIFY(processor.setConfiguration(configuration));
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    fillChannelSamples(payload, 0, 107'374'182);

    GainProcessResult result;
    for (int frame = 0; frame < 5; ++frame) {
        result = processor.process(payload, 2000);
    }
    QCOMPARE(result.snapshot.shifts.at(0), 0);

    for (int frame = 0; frame < 2; ++frame) {
        result = processor.process(payload, 2000);
    }
    QCOMPARE(result.snapshot.shifts.at(0), 0);

    result = processor.process(payload, 2000);
    QCOMPARE(result.snapshot.shifts.at(0), 1);

    result = processor.process(payload, 2000);
    QCOMPARE(result.snapshot.shifts.at(0), 1);
    result = processor.process(payload, 2000);
    QCOMPARE(result.snapshot.shifts.at(0), 2);
}

void MediaTest::automaticGainAccumulatesTruncatedFrameDurationAt48kHz()
{
    ChannelGainProcessor processor;
    GainConfiguration configuration;
    configuration.automatic[0] = true;
    QVERIFY(processor.setConfiguration(configuration));
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    fillChannelSamples(payload, 0, 53'687'091);

    GainProcessResult result;
    for (int frame = 0; frame < 120; ++frame) {
        result = processor.process(payload, 48000);
    }
    QCOMPARE(result.snapshot.shifts.at(0), 0);

    result = processor.process(payload, 48000);
    QCOMPARE(result.snapshot.shifts.at(0), 0);
    for (int frame = 0; frame < 59; ++frame) {
        result = processor.process(payload, 48000);
    }
    QCOMPARE(result.snapshot.shifts.at(0), 0);

    result = processor.process(payload, 48000);
    QCOMPARE(result.snapshot.shifts.at(0), 1);
    for (int frame = 0; frame < 59; ++frame) {
        result = processor.process(payload, 48000);
    }
    QCOMPARE(result.snapshot.shifts.at(0), 1);

    result = processor.process(payload, 48000);
    QCOMPARE(result.snapshot.shifts.at(0), 2);
}

void MediaTest::automaticGainLongFrameDoesNotOvershootThresholds()
{
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    fillChannelSamples(payload, 0, 429'496'729);

    ChannelGainProcessor hysteresisProcessor;
    GainConfiguration hysteresisConfiguration;
    hysteresisConfiguration.automatic[0] = true;
    QVERIFY(hysteresisProcessor.setConfiguration(hysteresisConfiguration));

    const GainProcessResult hysteresisResult = hysteresisProcessor.process(payload, 100);

    QVERIFY2(hysteresisResult.valid, qPrintable(hysteresisResult.error));
    QCOMPARE(hysteresisResult.snapshot.shifts.at(0), 1);
    QVERIFY(hysteresisResult.snapshot.outputPeakRatios.at(0) >= 0.35);
    QVERIFY(hysteresisResult.snapshot.outputPeakRatios.at(0) <= 0.80);

    ChannelGainProcessor upperLimitProcessor;
    GainConfiguration upperLimitConfiguration;
    upperLimitConfiguration.automatic[0] = true;
    upperLimitConfiguration.upperPeakRatio = 0.40;
    upperLimitConfiguration.lowerPeakRatio = 0.35;
    QVERIFY(upperLimitProcessor.setConfiguration(upperLimitConfiguration));
    fillChannelSamples(payload, 0, 644'245'094);

    const GainProcessResult upperLimitResult = upperLimitProcessor.process(payload, 100);

    QVERIFY2(upperLimitResult.valid, qPrintable(upperLimitResult.error));
    QCOMPARE(upperLimitResult.snapshot.shifts.at(0), 0);
    QVERIFY(upperLimitResult.snapshot.outputPeakRatios.at(0) <= 0.40);
}

void MediaTest::automaticGainRaisesSilenceToMaximum()
{
    ChannelGainProcessor processor;
    GainConfiguration configuration;
    configuration.automatic[0] = true;
    QVERIFY(processor.setConfiguration(configuration));
    const QByteArray silence(AudioFrameDecoder::FramePayloadBytes, char(0));

    GainProcessResult result;
    for (int frame = 0; frame < 24; ++frame) {
        result = processor.process(silence, 2000);
    }
    QCOMPARE(result.snapshot.shifts.at(0), 7);

    result = processor.process(silence, 2000);
    QCOMPARE(result.snapshot.shifts.at(0), 8);
    for (int frame = 0; frame < 10; ++frame) {
        result = processor.process(silence, 2000);
    }
    QCOMPARE(result.snapshot.shifts.at(0), 8);
}

void MediaTest::changingOneGainChannelPreservesOtherAgcState()
{
    ChannelGainProcessor processor;
    GainConfiguration configuration;
    configuration.automatic[1] = true;
    QVERIFY(processor.setConfiguration(configuration));
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    fillChannelSamples(payload, 1, 107'374'182);

    for (int frame = 0; frame < 4; ++frame) {
        QVERIFY(processor.process(payload, 2000).valid);
    }

    GainConfiguration updated = processor.configuration();
    updated.shifts[0] = 1;
    QVERIFY(processor.setConfiguration(updated));

    GainProcessResult result;
    for (int frame = 0; frame < 4; ++frame) {
        result = processor.process(payload, 2000);
    }
    QCOMPARE(result.snapshot.shifts.at(0), 1);
    QCOMPARE(result.snapshot.shifts.at(1), 1);
}

void MediaTest::savesAndLoadsGainProfileWithAllChannels()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("gain-profile.json"));
    GainConfiguration configuration;
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        configuration.shifts[channel] = channel % 17 - 8;
        configuration.automatic[channel] = channel % 3 == 0;
    }
    configuration.upperPeakRatio = 0.72;
    configuration.lowerPeakRatio = 0.24;
    configuration.releaseHoldMs = 600;
    configuration.releaseStepMs = 175;

    GainProfileStore store;
    QString error;
    QVERIFY2(store.save(path, configuration, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(document.value(QStringLiteral("schemaVersion")).toInt(), 1);
    QCOMPARE(document.value(QStringLiteral("channelCount")).toInt(), 48);
    QCOMPARE(document.value(QStringLiteral("shifts")).toArray().size(), 48);
    QCOMPARE(document.value(QStringLiteral("automatic")).toArray().size(), 48);
    QVERIFY(document.value(QStringLiteral("agc")).isObject());

    const std::optional<GainConfiguration> loaded = store.load(path, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(*loaded, configuration);
}

void MediaTest::rejectsInvalidGainProfileDocuments()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("gain-profile.json"));
    GainConfiguration valid;
    QJsonObject document;
    document.insert(QStringLiteral("schemaVersion"), 1);
    document.insert(QStringLiteral("channelCount"), GainConfiguration::ChannelCount);
    QJsonArray shifts;
    QJsonArray automatic;
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        shifts.append(valid.shifts.at(channel));
        automatic.append(valid.automatic.at(channel));
    }
    document.insert(QStringLiteral("shifts"), shifts);
    document.insert(QStringLiteral("automatic"), automatic);
    document.insert(QStringLiteral("agc"), QJsonObject {
        {QStringLiteral("upperPeakRatio"), valid.upperPeakRatio},
        {QStringLiteral("lowerPeakRatio"), valid.lowerPeakRatio},
        {QStringLiteral("releaseHoldMs"), valid.releaseHoldMs},
        {QStringLiteral("releaseStepMs"), valid.releaseStepMs},
    });

    struct InvalidDocument {
        QJsonObject document;
        QString errorFragment;
    };
    QList<InvalidDocument> invalidDocuments;
    QJsonObject invalid = document;
    invalid.insert(QStringLiteral("schemaVersion"), 2);
    invalidDocuments.append({invalid, QStringLiteral("schema")});
    invalid = document;
    QJsonArray shortShifts = invalid.value(QStringLiteral("shifts")).toArray();
    shortShifts.removeLast();
    invalid.insert(QStringLiteral("shifts"), shortShifts);
    invalidDocuments.append({invalid, QStringLiteral("shifts")});
    invalid = document;
    QJsonArray fractionalShifts = invalid.value(QStringLiteral("shifts")).toArray();
    fractionalShifts[7] = 1.5;
    invalid.insert(QStringLiteral("shifts"), fractionalShifts);
    invalidDocuments.append({invalid, QStringLiteral("integer")});
    invalid = document;
    QJsonArray outOfRangeShifts = invalid.value(QStringLiteral("shifts")).toArray();
    outOfRangeShifts[12] = 9;
    invalid.insert(QStringLiteral("shifts"), outOfRangeShifts);
    invalidDocuments.append({invalid, QStringLiteral("shift")});
    invalid = document;
    QJsonObject invalidAgc = invalid.value(QStringLiteral("agc")).toObject();
    invalidAgc.insert(QStringLiteral("lowerPeakRatio"), 0.8);
    invalidAgc.insert(QStringLiteral("upperPeakRatio"), 0.8);
    invalid.insert(QStringLiteral("agc"), invalidAgc);
    invalidDocuments.append({invalid, QStringLiteral("lower")});
    invalid = document;
    invalidAgc = invalid.value(QStringLiteral("agc")).toObject();
    invalidAgc.insert(QStringLiteral("releaseHoldMs"), 0);
    invalid.insert(QStringLiteral("agc"), invalidAgc);
    invalidDocuments.append({invalid, QStringLiteral("hold")});

    GainProfileStore store;
    for (const InvalidDocument &invalidDocument : invalidDocuments) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(invalidDocument.document).toJson());
        file.close();

        QString error;
        QVERIFY(!store.load(path, &error).has_value());
        QVERIFY2(error.contains(invalidDocument.errorFragment, Qt::CaseInsensitive), qPrintable(error));
    }
}

void MediaTest::rejectsGainProfileShapeAndTypeViolations()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("gain-profile.json"));
    GainConfiguration configuration;
    QJsonObject document {
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("channelCount"), GainConfiguration::ChannelCount},
    };
    QJsonArray shifts;
    QJsonArray automatic;
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        shifts.append(configuration.shifts.at(channel));
        automatic.append(configuration.automatic.at(channel));
    }
    document.insert(QStringLiteral("shifts"), shifts);
    document.insert(QStringLiteral("automatic"), automatic);
    document.insert(QStringLiteral("agc"), QJsonObject {
        {QStringLiteral("upperPeakRatio"), configuration.upperPeakRatio},
        {QStringLiteral("lowerPeakRatio"), configuration.lowerPeakRatio},
        {QStringLiteral("releaseHoldMs"), configuration.releaseHoldMs},
        {QStringLiteral("releaseStepMs"), configuration.releaseStepMs},
    });

    struct InvalidDocument {
        QByteArray contents;
        QString errorFragment;
    };
    QList<InvalidDocument> invalidDocuments;
    const auto appendObject = [&invalidDocuments](const QJsonObject &value, const QString &errorFragment) {
        invalidDocuments.append({QJsonDocument(value).toJson(), errorFragment});
    };
    invalidDocuments.append({QByteArrayLiteral("{ invalid"), QStringLiteral("parse")});
    invalidDocuments.append({QJsonDocument(QJsonArray {1}).toJson(), QStringLiteral("root")});
    QJsonObject invalid = document;
    invalid.insert(QStringLiteral("unexpected"), true);
    appendObject(invalid, QStringLiteral("unexpected"));
    invalid = document;
    invalid.remove(QStringLiteral("schemaVersion"));
    appendObject(invalid, QStringLiteral("schemaVersion"));
    invalid = document;
    invalid.insert(QStringLiteral("schemaVersion"), QStringLiteral("1"));
    appendObject(invalid, QStringLiteral("schemaVersion"));
    invalid = document;
    invalid.insert(QStringLiteral("channelCount"), true);
    appendObject(invalid, QStringLiteral("channelCount"));
    invalid = document;
    invalid.remove(QStringLiteral("shifts"));
    appendObject(invalid, QStringLiteral("shifts"));
    invalid = document;
    invalid.insert(QStringLiteral("shifts"), QStringLiteral("not-an-array"));
    appendObject(invalid, QStringLiteral("shifts"));
    invalid = document;
    QJsonArray invalidShifts = invalid.value(QStringLiteral("shifts")).toArray();
    invalidShifts[0] = true;
    invalid.insert(QStringLiteral("shifts"), invalidShifts);
    appendObject(invalid, QStringLiteral("shift"));
    invalid = document;
    invalidShifts = invalid.value(QStringLiteral("shifts")).toArray();
    invalidShifts[0] = QStringLiteral("0");
    invalid.insert(QStringLiteral("shifts"), invalidShifts);
    appendObject(invalid, QStringLiteral("shift"));
    invalid = document;
    invalid.remove(QStringLiteral("automatic"));
    appendObject(invalid, QStringLiteral("automatic"));
    invalid = document;
    invalid.insert(QStringLiteral("automatic"), QJsonObject {});
    appendObject(invalid, QStringLiteral("automatic"));
    invalid = document;
    QJsonArray invalidAutomatic = invalid.value(QStringLiteral("automatic")).toArray();
    invalidAutomatic.removeLast();
    invalid.insert(QStringLiteral("automatic"), invalidAutomatic);
    appendObject(invalid, QStringLiteral("automatic"));
    invalid = document;
    invalidAutomatic = invalid.value(QStringLiteral("automatic")).toArray();
    invalidAutomatic[0] = 0;
    invalid.insert(QStringLiteral("automatic"), invalidAutomatic);
    appendObject(invalid, QStringLiteral("automatic"));
    invalid = document;
    invalid.remove(QStringLiteral("agc"));
    appendObject(invalid, QStringLiteral("agc"));
    invalid = document;
    QJsonObject invalidAgc = invalid.value(QStringLiteral("agc")).toObject();
    invalidAgc.insert(QStringLiteral("extra"), 1);
    invalid.insert(QStringLiteral("agc"), invalidAgc);
    appendObject(invalid, QStringLiteral("extra"));
    invalid = document;
    invalidAgc = invalid.value(QStringLiteral("agc")).toObject();
    invalidAgc.remove(QStringLiteral("lowerPeakRatio"));
    invalid.insert(QStringLiteral("agc"), invalidAgc);
    appendObject(invalid, QStringLiteral("lowerPeakRatio"));
    invalid = document;
    invalidAgc = invalid.value(QStringLiteral("agc")).toObject();
    invalidAgc.insert(QStringLiteral("upperPeakRatio"), true);
    invalid.insert(QStringLiteral("agc"), invalidAgc);
    appendObject(invalid, QStringLiteral("upperPeakRatio"));
    invalid = document;
    invalidAgc = invalid.value(QStringLiteral("agc")).toObject();
    invalidAgc.insert(QStringLiteral("releaseHoldMs"), 1.5);
    invalid.insert(QStringLiteral("agc"), invalidAgc);
    appendObject(invalid, QStringLiteral("releaseHoldMs"));
    invalid = document;
    invalidAgc = invalid.value(QStringLiteral("agc")).toObject();
    invalidAgc.insert(QStringLiteral("releaseStepMs"), QStringLiteral("250"));
    invalid.insert(QStringLiteral("agc"), invalidAgc);
    appendObject(invalid, QStringLiteral("releaseStepMs"));

    GainProfileStore store;
    for (const InvalidDocument &invalidDocument : invalidDocuments) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(invalidDocument.contents), invalidDocument.contents.size());
        file.close();

        QString error;
        QVERIFY(!store.load(path, &error).has_value());
        QVERIFY2(error.contains(invalidDocument.errorFragment, Qt::CaseInsensitive), qPrintable(error));
    }
}

void MediaTest::keepsExistingGainProfileWhenSaveFails()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("gain-profile.json"));
    GainProfileStore store;
    GainConfiguration baseline;
    baseline.shifts[17] = 4;
    QVERIFY(store.save(path, baseline));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray originalContents = file.readAll();
    file.close();

    GainConfiguration invalid = baseline;
    invalid.releaseStepMs = 0;
    QString error;
    QVERIFY(!store.save(path, invalid, &error));
    QVERIFY(error.contains(QStringLiteral("step"), Qt::CaseInsensitive));

    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), originalContents);
    const std::optional<GainConfiguration> loaded = store.load(path, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(*loaded, baseline);
}

void MediaTest::keepsExistingGainProfileWhenAtomicCommitFails()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("gain-profile.json"));
    GainConfiguration baseline;
    baseline.shifts[5] = 3;
    GainProfileStore normalStore;
    QVERIFY(normalStore.save(path, baseline));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray originalContents = file.readAll();
    file.close();

    GainConfiguration replacement = baseline;
    replacement.shifts[5] = -3;
    bool writeObserved = false;
    GainProfileStore failingStore([&writeObserved](QSaveFile &file) {
        writeObserved = file.size() > 0;
        return false;
    });
    QString error;
    QVERIFY(!failingStore.save(path, replacement, &error));
    QVERIFY(error.contains(QStringLiteral("commit"), Qt::CaseInsensitive));
    QVERIFY(writeObserved);

    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), originalContents);
    file.close();
    const std::optional<GainConfiguration> loaded = normalStore.load(path, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(*loaded, baseline);
}

void MediaTest::reportsUnwritableGainProfileTarget()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    GainProfileStore store;
    QString error;

    QVERIFY(!store.save(directory.filePath(QStringLiteral("missing/profile.json")),
                        GainConfiguration(),
                        &error));
    QVERIFY(!error.isEmpty());
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

void MediaTest::audioProcessorDistributesGainAdjustedFrame()
{
    AudioProcessor processor;
    processor.configure({48000, 48, 0, 1, false});
    GainConfiguration configuration;
    configuration.shifts[0] = 1;
    configuration.automatic[0] = true;
    processor.setGainConfiguration(configuration);
    processor.setChannelShift(0, 1);

    QSignalSpy adjustedSpy(&processor, &AudioProcessor::adjustedFrameReady);
    QSignalSpy decodedSpy(&processor, &AudioProcessor::arrayFrameDecoded);
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    fillChannelSamples(payload, 0, 100'000);

    processor.processArrayFrame(payload, 12, 34);

    QCOMPARE(adjustedSpy.count(), 1);
    QCOMPARE(decodedSpy.count(), 1);
    const QByteArray adjusted = adjustedSpy.first().at(0).toByteArray();
    QCOMPARE(readSample(adjusted, 0, 0), qint32(200'000));
    QCOMPARE(adjustedSpy.first().at(1).toUInt(), quint32(12));
    QCOMPARE(adjustedSpy.first().at(2).toUInt(), quint32(34));
    const GainSnapshot snapshot = qvariant_cast<GainSnapshot>(adjustedSpy.first().at(3));
    QCOMPARE(snapshot.shifts.at(0), 1);
    QCOMPARE(snapshot.automatic.at(0), false);

    const DecodedAudioFrame decoded = qvariant_cast<DecodedAudioFrame>(decodedSpy.first().at(0));
    QCOMPARE(decoded.samples.at(0), qint32(200'000));
    QCOMPARE(decoded.amplitudes.at(0), 200'000.0);
}

void MediaTest::recordsOnlyGainAdjustedAudioFrames()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AudioRecorder recorder;
    GainConfiguration configuration;
    configuration.shifts[0] = 1;
    RecordingSettings settings {directory.path(), QStringLiteral("adjusted_only"), {0}, 48000};
    settings.gainApplied = true;
    settings.initialGainConfiguration = GainConfiguration {};
    QVERIFY(recorder.startRecording(settings));
    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Recording, 3000);

    AudioProcessor processor;
    processor.configure({48000, 48, 0, 1, false});
    processor.setGainConfiguration(configuration);
    connect(&processor,
            &AudioProcessor::adjustedFrameReady,
            &recorder,
            [&recorder, configuration](const QByteArray &payload,
                                       quint32,
                                       quint32,
                                       const GainSnapshot &) {
                recorder.enqueueFrame(payload, configuration);
            });
    QByteArray raw(AudioFrameDecoder::FramePayloadBytes, char(0));
    fillChannelSamples(raw, 0, 123'456);
    processor.processArrayFrame(raw, 1, 2);
    QTRY_COMPARE_WITH_TIMEOUT(recorder.statistics().inputFrames, quint64(1), 3000);
    recorder.stopRecording(QStringLiteral("user"));
    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Idle, 3000);

    const RecordingStatistics statistics = recorder.statistics();
    QFile manifest(statistics.outputPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(manifest.readAll()).object();
    const QJsonObject gain = root.value(QStringLiteral("gain")).toObject();
    QCOMPARE(gain.value(QStringLiteral("initial"))
                 .toObject()
                 .value(QStringLiteral("shifts"))
                 .toArray()
                 .at(0)
                 .toInt(),
             0);
    QCOMPARE(gain.value(QStringLiteral("final"))
                 .toObject()
                 .value(QStringLiteral("shifts"))
                 .toArray()
                 .at(0)
                 .toInt(),
             1);
    const QString wavName = root.value(QStringLiteral("segments"))
                                .toArray()
                                .first()
                                .toObject()
                                .value(QStringLiteral("file"))
                                .toString();
    QFile wav(directory.filePath(wavName));
    QVERIFY(wav.open(QIODevice::ReadOnly));
    const QByteArray bytes = wav.readAll();
    QCOMPARE(qFromLittleEndian<qint32>(
                 reinterpret_cast<const uchar *>(bytes.constData() + 68)),
             qint32(246'912));
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

void MediaTest::recorderRejectsInvalidPayloadBeforeQueueing()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AudioRecorder recorder;
    RecordingSettings settings {directory.path(), QStringLiteral("invalid_queue_payload"), {0}, 48000};
    QVERIFY(recorder.startRecording(settings));
    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Recording, 3000);

    QVERIFY(!recorder.enqueueFrame(QByteArray()));
    QVERIFY(!recorder.enqueueFrame(QByteArray(10, char(0))));
    QCOMPARE(recorder.statistics().pendingBytes, quint64(0));
    QCOMPARE(recorder.statistics().pendingFrames, quint64(0));
    QCOMPARE(recorder.statistics().discardedFrames, quint64(2));
    QCOMPARE(recorder.state(), RecordingState::Recording);

    QVERIFY(recorder.stopAndWait());
}

void MediaTest::recorderBoundsPendingFrameCount()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AudioRecorder recorder(AudioRecorder::MaxPendingBytes, 1);
    RecordingSettings settings {directory.path(), QStringLiteral("frame_queue_limit"), {0}, 48000};
    QVERIFY(recorder.startRecording(settings));
    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Recording, 3000);

    const QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    QVERIFY(recorder.enqueueFrame(payload));
    QCOMPARE(recorder.statistics().pendingFrames, quint64(1));
    QVERIFY(!recorder.enqueueFrame(payload));
    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Error, 3000);
    QCOMPARE(recorder.statistics().pendingFrames, quint64(0));
    QCOMPARE(recorder.statistics().pendingBytes, quint64(0));
}

void MediaTest::recordsGainMetadataInManifest()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    GainConfiguration initialGain;
    initialGain.shifts[0] = 1;
    initialGain.automatic[1] = true;
    RecordingSettings settings {directory.path(), QStringLiteral("gain_metadata"), {0, 1}, 48000};
    settings.gainApplied = true;
    settings.initialGainConfiguration = initialGain;

    MultiChannelWavWriter writer;
    QString error;
    QVERIFY2(writer.open(settings, &error), qPrintable(error));
    QVERIFY2(writer.appendFrame(QByteArray(AudioFrameDecoder::FramePayloadBytes, char(0)), &error), qPrintable(error));
    GainConfiguration finalGain = initialGain;
    finalGain.shifts[0] = 3;
    finalGain.automatic[1] = false;
    QVERIFY2(writer.updateFinalGainConfiguration(finalGain, &error), qPrintable(error));
    QVERIFY2(writer.close(QStringLiteral("user"), &error), qPrintable(error));

    QFile manifest(writer.summary().manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(manifest.readAll()).object();
    QCOMPARE(root.value(QStringLiteral("gainApplied")).toBool(), true);
    const QJsonObject gain = root.value(QStringLiteral("gain")).toObject();
    const QJsonObject initial = gain.value(QStringLiteral("initial")).toObject();
    const QJsonObject final = gain.value(QStringLiteral("final")).toObject();
    QCOMPARE(initial.value(QStringLiteral("shifts")).toArray().size(), GainConfiguration::ChannelCount);
    QCOMPARE(initial.value(QStringLiteral("shifts")).toArray().at(0).toInt(), 1);
    QCOMPARE(initial.value(QStringLiteral("automatic")).toArray().at(1).toBool(), true);
    QCOMPARE(final.value(QStringLiteral("shifts")).toArray().at(0).toInt(), 3);
    QCOMPARE(final.value(QStringLiteral("automatic")).toArray().at(1).toBool(), false);
    QCOMPARE(final.value(QStringLiteral("upperPeakRatio")).toDouble(), finalGain.upperPeakRatio);
    QCOMPARE(final.value(QStringLiteral("releaseHoldMs")).toInt(), finalGain.releaseHoldMs);
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

    QVERIFY(!recorder.enqueueFrame(QByteArray(10, char(0))));
    QVERIFY(recorder.enqueueFrame(QByteArray(AudioFrameDecoder::FramePayloadBytes, char(0))));
    recorder.stopRecording(QStringLiteral("user"));

    QTRY_COMPARE_WITH_TIMEOUT(recorder.state(), RecordingState::Idle, 3000);
    const RecordingStatistics statistics = recorder.statistics();
    QCOMPARE(statistics.inputFrames, quint64(1));
    QCOMPARE(statistics.discardedFrames, quint64(1));
    QVERIFY(statistics.error.contains(QStringLiteral("38400")));
    QVERIFY(QFileInfo::exists(statistics.outputPath));

    QFile manifest(statistics.outputPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(manifest.readAll()).object();
    QCOMPARE(root.value(QStringLiteral("stopReason")).toString(), QStringLiteral("user"));
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

void MediaTest::mapsNoiseReductionDirectionsToSixControlInterfaces()
{
    const DirectionControlMapping mapping;
    QCOMPARE(DirectionControlMapper::azimuthCode(-90.0, mapping), qint16(-4));
    QCOMPARE(DirectionControlMapper::azimuthCode(0.0, mapping), qint16(0));
    QCOMPARE(DirectionControlMapper::azimuthCode(90.0, mapping), qint16(4));
    QCOMPARE(DirectionControlMapper::elevationCode(-90.0, mapping), qint16(-3));
    QCOMPARE(DirectionControlMapper::elevationCode(0.0, mapping), qint16(0));
    QCOMPARE(DirectionControlMapper::elevationCode(90.0, mapping), qint16(3));
    QCOMPARE(DirectionControlMapper::azimuthDegrees(1, mapping), 22.5);
    QCOMPARE(DirectionControlMapper::elevationDegrees(-1, mapping), -30.0);

    const ContinuousDirectionTarget enhancedA {22.5, -30.0};
    const ContinuousDirectionTarget suppressionB1 {-45.0, 60.0};
    const ContinuousDirectionTarget suppressionB2 {90.0, -90.0};
    const ControlValues values = DirectionControlMapper::controlValues(
        enhancedA, suppressionB1, suppressionB2, mapping);
    QCOMPARE(values.channelValue(0), qint16(1));
    QCOMPARE(values.channelValue(1), qint16(-1));
    QCOMPARE(values.channelValue(2), qint16(-2));
    QCOMPARE(values.channelValue(3), qint16(2));
    QCOMPARE(values.channelValue(4), qint16(4));
    QCOMPARE(values.channelValue(5), qint16(-3));
}

void MediaTest::quantizesContinuousNoiseDirectionsOnlyAtControlBoundary()
{
    const DirectionControlMapping mapping;
    const ContinuousDirectionTarget enhancedA {11.2, -7.4};
    const ContinuousDirectionTarget suppressionB1 {-38.8, 12.6};
    const ContinuousDirectionTarget suppressionB2 {74.9, -33.3};

    const ControlValues values = DirectionControlMapper::controlValues(
        enhancedA, suppressionB1, suppressionB2, mapping);
    QCOMPARE(values.channelValue(0), qint16(0));
    QCOMPARE(values.channelValue(1), qint16(0));
    QCOMPARE(values.channelValue(2), qint16(-2));
    QCOMPARE(values.channelValue(3), qint16(0));
    QCOMPARE(values.channelValue(4), qint16(3));
    QCOMPARE(values.channelValue(5), qint16(-1));
    QCOMPARE(enhancedA.azimuthDegrees, 11.2);
    QCOMPARE(enhancedA.elevationDegrees, -7.4);

    const ContinuousDirectionTarget outside {120.5, -120.25};
    const ContinuousDirectionTarget clamped =
        DirectionControlMapper::clampDirection(outside, mapping);
    QCOMPARE(clamped.azimuthDegrees, 90.0);
    QCOMPARE(clamped.elevationDegrees, -90.0);
}

void MediaTest::clampsAndRejectsInvalidNoiseReductionDirectionMappings()
{
    DirectionControlMapping mapping;
    QCOMPARE(DirectionControlMapper::azimuthCode(-1000.0, mapping), qint16(-4));
    QCOMPARE(DirectionControlMapper::azimuthCode(1000.0, mapping), qint16(4));
    QCOMPARE(DirectionControlMapper::elevationCode(-1000.0, mapping), qint16(-3));
    QCOMPARE(DirectionControlMapper::elevationCode(1000.0, mapping), qint16(3));

    mapping.azimuthMinimumDegrees = -80.0;
    mapping.azimuthMaximumDegrees = 80.0;
    QCOMPARE(DirectionControlMapper::azimuthCode(10.1, mapping), qint16(1));
    QCOMPARE(DirectionControlMapper::azimuthCode(-10.1, mapping), qint16(-1));

    mapping.azimuthMaximumDegrees = mapping.azimuthMinimumDegrees;
    QVERIFY(!mapping.isValid());
    QCOMPARE(DirectionControlMapper::azimuthCode(30.0, mapping), qint16(0));
    QCOMPARE(DirectionControlMapper::azimuthDegrees(3, mapping), 0.0);
}

void MediaTest::decodesOnlyFirstFourNoiseReductionChannels()
{
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
        fillChannelSamples(payload, channel, (channel + 1) * 100 * 256);
    }

    const NoiseReductionInputFrame decoded =
        NoiseReductionFrameDecoder().decode(payload);
    QVERIFY(decoded.valid);
    for (int channel = 0; channel < 4; ++channel) {
        QCOMPARE(decoded.channels.at(channel).size(),
                 AudioFrameDecoder::SamplesPerFrame);
        QVERIFY(qAbs(decoded.channels.at(channel).first()
                     - static_cast<float>((channel + 1) * 100) / 32768.0f)
                < 1.0e-6f);
    }

    QByteArray changed = payload;
    for (int channel = 4; channel < AudioFrameDecoder::ChannelCount; ++channel) {
        fillChannelSamples(changed, channel, -(channel + 1) * 1000 * 256);
    }
    const NoiseReductionInputFrame changedDecoded =
        NoiseReductionFrameDecoder().decode(changed);
    QVERIFY(changedDecoded.valid);
    QCOMPARE(changedDecoded.channels, decoded.channels);
}

void MediaTest::rejectsInvalidNoiseReductionPayload()
{
    const NoiseReductionInputFrame decoded =
        NoiseReductionFrameDecoder().decode(QByteArray(128, char(0)));
    QVERIFY(!decoded.valid);
    QVERIFY(decoded.error.contains(QStringLiteral("38400")));
}

void MediaTest::suppressesOneAndTwoReferenceComponentsWithWienerFilter()
{
    constexpr int SampleRate = 48000;
    constexpr int SampleCount = SampleRate * 2;
    QVector<float> target(SampleCount);
    QVector<float> b1(SampleCount);
    QVector<float> b2(SampleCount);
    QVector<float> enhanced(SampleCount);
    std::mt19937 generator(7319);
    std::normal_distribution<float> noise(0.0f, 0.16f);
    for (int index = 0; index < SampleCount; ++index) {
        target[index] = 0.20f * std::sin(
            2.0 * std::numbers::pi * 1000.0 * index / SampleRate);
        b1[index] = noise(generator);
        b2[index] = noise(generator);
        enhanced[index] =
            target.at(index) + 0.85f * b1.at(index) + 0.65f * b2.at(index);
    }

    WienerNoiseReducer single;
    single.setAlgorithm(NoiseReductionAlgorithm::AdaptiveWiener);
    single.setMode(NoiseReductionMode::SinglePoint);
    const QVector<float> singleOutput = single.process(enhanced, b1, b2);

    WienerNoiseReducer dual;
    dual.setAlgorithm(NoiseReductionAlgorithm::AdaptiveWiener);
    dual.setMode(NoiseReductionMode::TwoPoint);
    const QVector<float> dualOutput = dual.process(enhanced, b1, b2);

    constexpr int SettlingSamples = 12000;
    const double inputError =
        rootMeanSquareError(enhanced, target, SettlingSamples);
    const double singleError =
        rootMeanSquareError(singleOutput, target, SettlingSamples);
    const double dualError =
        rootMeanSquareError(dualOutput, target, SettlingSamples);
    QVERIFY2(singleError < inputError * 0.82,
             qPrintable(QStringLiteral("single=%1 input=%2")
                            .arg(singleError)
                            .arg(inputError)));
    QVERIFY2(dualError < singleError * 0.78,
             qPrintable(QStringLiteral("dual=%1 single=%2")
                            .arg(dualError)
                            .arg(singleError)));
}

void MediaTest::appliesFixedWienerCoefficientsIndependently()
{
    constexpr int SampleRate = 48000;
    constexpr int SampleCount = SampleRate;
    constexpr int SettlingSamples = 4096;
    QVector<float> b1(SampleCount);
    QVector<float> b2(SampleCount);
    QVector<float> enhancedSingle(SampleCount);
    QVector<float> enhancedDual(SampleCount);
    for (int index = 0; index < SampleCount; ++index) {
        b1[index] = 0.3f * std::sin(
            2.0 * std::numbers::pi * 750.0 * index / SampleRate);
        b2[index] = 0.3f * std::sin(
            2.0 * std::numbers::pi * 3000.0 * index / SampleRate);
        enhancedSingle[index] = b1.at(index);
        enhancedDual[index] = b1.at(index) + b2.at(index);
    }

    WienerNoiseReducer bypass;
    bypass.setAlgorithm(NoiseReductionAlgorithm::FixedParameterWiener);
    bypass.setMode(NoiseReductionMode::SinglePoint);
    bypass.setSuppressionCoefficients(0.0, 5.0, 5.0);
    const QVector<float> bypassOutput =
        bypass.process(enhancedSingle, b1, b2);

    WienerNoiseReducer single;
    single.setAlgorithm(NoiseReductionAlgorithm::FixedParameterWiener);
    single.setMode(NoiseReductionMode::SinglePoint);
    single.setSuppressionCoefficients(3.0, 5.0, 5.0);
    const QVector<float> singleOutput =
        single.process(enhancedSingle, b1, b2);
    const double bypassAmplitude =
        toneAmplitude(bypassOutput, 750.0, SampleRate, SettlingSamples);
    const double singleAmplitude =
        toneAmplitude(singleOutput, 750.0, SampleRate, SettlingSamples);
    QVERIFY(bypassAmplitude > 0.28);
    QVERIFY(singleAmplitude > bypassAmplitude * 0.22);
    QVERIFY(singleAmplitude < bypassAmplitude * 0.28);

    WienerNoiseReducer suppressB1;
    suppressB1.setAlgorithm(NoiseReductionAlgorithm::FixedParameterWiener);
    suppressB1.setMode(NoiseReductionMode::TwoPoint);
    suppressB1.setSuppressionCoefficients(3.0, 9.0, 0.0);
    const QVector<float> b1Output =
        suppressB1.process(enhancedDual, b1, b2);
    QVERIFY(toneAmplitude(b1Output, 750.0, SampleRate, SettlingSamples)
            < toneAmplitude(b1Output, 3000.0, SampleRate, SettlingSamples)
                * 0.2);

    WienerNoiseReducer suppressB2;
    suppressB2.setAlgorithm(NoiseReductionAlgorithm::FixedParameterWiener);
    suppressB2.setMode(NoiseReductionMode::TwoPoint);
    suppressB2.setSuppressionCoefficients(3.0, 0.0, 9.0);
    const QVector<float> b2Output =
        suppressB2.process(enhancedDual, b1, b2);
    QVERIFY(toneAmplitude(b2Output, 3000.0, SampleRate, SettlingSamples)
            < toneAmplitude(b2Output, 750.0, SampleRate, SettlingSamples)
                * 0.2);
}

void MediaTest::keepsWienerOutputFiniteForSilentAndDegenerateReferences()
{
    QVector<float> silence(8192, 0.0f);
    WienerNoiseReducer reducer;
    reducer.setAlgorithm(NoiseReductionAlgorithm::AdaptiveWiener);
    reducer.setMode(NoiseReductionMode::TwoPoint);
    const QVector<float> silentOutput =
        reducer.process(silence, silence, silence);
    QVERIFY(!silentOutput.isEmpty());
    for (float sample : silentOutput) {
        QVERIFY(std::isfinite(sample));
        QCOMPARE(sample, 0.0f);
    }

    reducer.reset();
    QVector<float> enhanced(8192);
    QVector<float> reference(8192);
    for (int index = 0; index < enhanced.size(); ++index) {
        reference[index] = 0.2f * std::sin(
            2.0 * std::numbers::pi * 700.0 * index / 48000.0);
        enhanced[index] = 0.5f * reference.at(index);
    }
    const QVector<float> output =
        reducer.process(enhanced, reference, reference);
    QVERIFY(!output.isEmpty());
    for (float sample : output) {
        QVERIFY(std::isfinite(sample));
        QVERIFY(sample >= -1.0f && sample <= 1.0f);
    }
}

void MediaTest::appliesAndResetsFourthOrderBandPassFilter()
{
    constexpr int SampleRate = 48000;
    constexpr int SampleCount = SampleRate * 2;
    constexpr int SettlingSamples = SampleRate / 2;
    QVector<float> input(SampleCount);
    for (int index = 0; index < input.size(); ++index) {
        input[index] =
            0.15f * std::sin(
                2.0 * std::numbers::pi * 40.0 * index / SampleRate)
            + 0.15f * std::sin(
                2.0 * std::numbers::pi * 1000.0 * index / SampleRate)
            + 0.15f * std::sin(
                2.0 * std::numbers::pi * 12000.0 * index / SampleRate);
    }

    BandPassFilter filter;
    QVERIFY(filter.configure(SampleRate, 100.0, 8000.0));
    const QVector<float> output = filter.process(input);
    QCOMPARE(output.size(), input.size());
    const double inputMid =
        toneAmplitude(input, 1000.0, SampleRate, SettlingSamples);
    const double outputLow =
        toneAmplitude(output, 40.0, SampleRate, SettlingSamples);
    const double outputMid =
        toneAmplitude(output, 1000.0, SampleRate, SettlingSamples);
    const double outputHigh =
        toneAmplitude(output, 12000.0, SampleRate, SettlingSamples);
    QVERIFY(outputMid > inputMid * 0.9);
    QVERIFY(outputLow < outputMid * 0.2);
    QVERIFY(outputHigh < outputMid * 0.5);

    filter.reset();
    const QVector<float> repeated = filter.process(input);
    QCOMPARE(repeated, output);
    QVERIFY(filter.process({}).isEmpty());
    QVERIFY(!filter.configure(SampleRate, 0.0, 8000.0));
    QVERIFY(!filter.configure(SampleRate, 8000.0, 100.0));
    QVERIFY(!filter.configure(SampleRate, 100.0, SampleRate / 2.0));
}

void MediaTest::noiseReductionWorkerConsumesRawFramesAndPublishesSelectedSources()
{
    NoiseReductionWorker worker;
    NoiseReductionSettings settings;
    settings.enabled = true;
    settings.firstPlotSource = NoiseSignalSource::FirstElement;
    settings.secondPlotSource = NoiseSignalSource::Denoised;
    worker.configure(settings, 48000);
    worker.setActive(true);

    QSignalSpy consumed(&worker, &NoiseReductionWorker::frameConsumed);
    QSignalSpy plots(&worker, &NoiseReductionWorker::plotFrameReady);
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    qint64 globalSample = 0;
    for (int frame = 0; frame < 20; ++frame) {
        for (int sample = 0; sample < AudioFrameDecoder::SamplesPerFrame;
             ++sample, ++globalSample) {
            const qint32 desired = static_cast<qint32>(
                std::sin(2.0 * std::numbers::pi * 1000.0
                         * globalSample / 48000.0)
                * 5000.0) << 8;
            const qint32 reference = static_cast<qint32>(
                std::sin(2.0 * std::numbers::pi * 3000.0
                         * globalSample / 48000.0)
                * 2500.0) << 8;
            putSample(payload, sample, 0, desired);
            putSample(payload, sample, 1, desired + reference);
            putSample(payload, sample, 2, reference);
            putSample(payload, sample, 3, 0);
        }
        worker.processRawFrame(payload, 1, static_cast<quint32>(frame * 4167));
    }

    QCOMPARE(consumed.count(), 20);
    QVERIFY(!plots.isEmpty());
    const NoiseReductionPlotFrame plot =
        qvariant_cast<NoiseReductionPlotFrame>(plots.last().at(0));
    QCOMPARE(plot.first.source, NoiseSignalSource::FirstElement);
    QCOMPARE(plot.second.source, NoiseSignalSource::Denoised);
    QVERIFY(!plot.first.waveform.isEmpty());
    QVERIFY(!plot.second.waveform.isEmpty());
    QCOMPARE(plot.first.spectrumDbfs.size(), 1025);
    QCOMPARE(plot.second.spectrumDbfs.size(), 1025);
    for (float value : plot.second.spectrumDbfs) {
        QVERIFY(std::isfinite(value));
        QVERIFY(value >= -120.0f && value <= 0.0f);
    }

    worker.setActive(false);
    const int previousPlots = plots.count();
    worker.processRawFrame(payload, 2, 0);
    QCOMPARE(consumed.count(), 21);
    QCOMPARE(plots.count(), previousPlots);
}

void MediaTest::filtersDenoisedWorkerOutputWhileMonitoringIsDisabled()
{
    NoiseReductionWorker worker;
    NoiseReductionSettings settings;
    settings.enabled = true;
    settings.algorithm = NoiseReductionAlgorithm::FixedParameterWiener;
    settings.singleSuppressionCoefficient = 0.0;
    settings.monitorEnabled = false;
    settings.firstPlotSource = NoiseSignalSource::EnhancedA;
    settings.secondPlotSource = NoiseSignalSource::Denoised;
    settings.bandPass.enabled = true;
    settings.bandPass.preset = BandPassPreset::Voice;
    settings.bandPass.lowCutoffHz = 100.0;
    settings.bandPass.highCutoffHz = 8000.0;
    worker.configure(settings, 48000);
    worker.setActive(true);

    QSignalSpy plots(&worker, &NoiseReductionWorker::plotFrameReady);
    QSignalSpy warnings(&worker, &NoiseReductionWorker::warningRaised);
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    qint64 globalSample = 0;
    for (int frame = 0; frame < 40; ++frame) {
        for (int sample = 0; sample < AudioFrameDecoder::SamplesPerFrame;
             ++sample, ++globalSample) {
            const double mixed =
                0.25 * std::sin(2.0 * std::numbers::pi * 1000.0
                                * globalSample / 48000.0)
                + 0.25 * std::sin(2.0 * std::numbers::pi * 12000.0
                                  * globalSample / 48000.0);
            const qint32 value =
                static_cast<qint32>(mixed * 8388607.0) << 8;
            putSample(payload, sample, 1, value);
            putSample(payload, sample, 2, 0);
            putSample(payload, sample, 3, 0);
        }
        worker.processRawFrame(
            payload, 1, static_cast<quint32>(frame * 4167));
    }

    QVERIFY(!plots.isEmpty());
    QCOMPARE(warnings.count(), 0);
    const NoiseReductionPlotFrame plot =
        qvariant_cast<NoiseReductionPlotFrame>(plots.last().at(0));
    constexpr int MidBin = 43;
    constexpr int HighBin = 512;
    QVERIFY(plot.second.spectrumDbfs.at(MidBin)
            > plot.first.spectrumDbfs.at(MidBin) - 2.0f);
    QVERIFY(plot.second.spectrumDbfs.at(HighBin)
            < plot.first.spectrumDbfs.at(HighBin) - 5.0f);
}

void MediaTest::computesNoiseReductionSpectrumAndSaturatingMonitorGain()
{
    QVector<float> tone(2048);
    for (int index = 0; index < tone.size(); ++index) {
        tone[index] = 0.5f * std::sin(
            2.0 * std::numbers::pi * 1000.0 * index / 48000.0);
    }
    const QVector<float> spectrum =
        NoiseReductionWorker::powerSpectrumDbfs(tone, 48000);
    QCOMPARE(spectrum.size(), 1025);
    const auto peak = std::max_element(spectrum.cbegin(), spectrum.cend());
    QVERIFY(peak != spectrum.cend());
    const int peakBin = static_cast<int>(peak - spectrum.cbegin());
    QVERIFY(qAbs(peakBin - 43) <= 1);

    const QByteArray pcm = NoiseReductionWorker::pcm16WithGain(
        QVector<float> {0.5f, -0.5f, 1.0f, -1.0f}, 12.0);
    QCOMPARE(pcm.size(), 8);
    QCOMPARE(qFromLittleEndian<qint16>(
                 reinterpret_cast<const uchar *>(pcm.constData())),
             qint16(32767));
    QCOMPARE(qFromLittleEndian<qint16>(
                 reinterpret_cast<const uchar *>(pcm.constData() + 2)),
             qint16(-32768));
    QCOMPARE(qFromLittleEndian<qint16>(
                 reinterpret_cast<const uchar *>(pcm.constData() + 4)),
             qint16(32767));
    QCOMPARE(qFromLittleEndian<qint16>(
                 reinterpret_cast<const uchar *>(pcm.constData() + 6)),
             qint16(-32768));
}

QTEST_MAIN(MediaTest)
#include "test_media.moc"
