#include "analysis/arraygeometry.h"
#include "analysis/audioanalysisworker.h"
#include "analysis/calibrationprofile.h"
#include "analysis/cameraviewcalibrator.h"
#include "analysis/directionprojection.h"
#include "analysis/gccphatlocator.h"
#include "analysis/ispatialresponseprovider.h"
#include "analysis/livelocalizationworker.h"
#include "analysis/radix2fft.h"
#include "analysis/soundsourcelocatorregistry.h"
#include "analysis/wavefilereader.h"
#include "media/audioframedecoder.h"
#include "media/multichannelwavwriter.h"

#include <QFile>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <QtEndian>

#include <cmath>
#include <complex>

using namespace MicArray;

namespace {

class FixedLocator final : public ISoundSourceLocator
{
public:
    LocatorDescriptor descriptor() const override
    {
        return {QStringLiteral("fixed"),
                QStringLiteral("固定结果"),
                QStringLiteral("测试定位器")};
    }

    bool configure(const LocatorConfiguration &, QString *) override
    {
        return true;
    }

    QVector<SoundSourceDirection> locate(const MultiChannelAudioWindow &) override
    {
        return {{10.0, 5.0, 1.0, 0.0, 1, 0}};
    }
};

bool positionMatches(const MicrophonePosition &actual,
                     double xMeters,
                     double yMeters,
                     double zMeters)
{
    return std::abs(actual.xMeters - xMeters) < 1.0e-12
        && std::abs(actual.yMeters - yMeters) < 1.0e-12
        && std::abs(actual.zMeters - zMeters) < 1.0e-12;
}

void appendLe16(QByteArray *bytes, quint16 value)
{
    char encoded[2];
    qToLittleEndian(value, encoded);
    bytes->append(encoded, 2);
}

void appendLe32(QByteArray *bytes, quint32 value)
{
    char encoded[4];
    qToLittleEndian(value, encoded);
    bytes->append(encoded, 4);
}

QByteArray makePcmWave(int channelCount,
                       int sampleRate,
                       int containerBits,
                       const QByteArray &data,
                       bool includeOddJunk = false,
                       quint16 blockAlignOverride = 0)
{
    const quint16 blockAlign = blockAlignOverride == 0
        ? static_cast<quint16>(channelCount * containerBits / 8)
        : blockAlignOverride;
    QByteArray body;
    body.append("WAVE", 4);
    if (includeOddJunk) {
        body.append("JUNK", 4);
        appendLe32(&body, 3);
        body.append("abc", 3);
        body.append(char(0));
    }
    body.append("fmt ", 4);
    appendLe32(&body, 16);
    appendLe16(&body, 1);
    appendLe16(&body, static_cast<quint16>(channelCount));
    appendLe32(&body, static_cast<quint32>(sampleRate));
    appendLe32(&body, static_cast<quint32>(sampleRate * blockAlign));
    appendLe16(&body, blockAlign);
    appendLe16(&body, static_cast<quint16>(containerBits));
    body.append("data", 4);
    appendLe32(&body, static_cast<quint32>(data.size()));
    body.append(data);
    if (data.size() % 2 != 0) {
        body.append(char(0));
    }

    QByteArray result("RIFF", 4);
    appendLe32(&result, static_cast<quint32>(body.size()));
    result.append(body);
    return result;
}

QByteArray makeNonPcmExtensibleWave()
{
    QByteArray body("WAVEfmt ", 8);
    appendLe32(&body, 40);
    appendLe16(&body, 0xfffe);
    appendLe16(&body, 1);
    appendLe32(&body, 48000);
    appendLe32(&body, 192000);
    appendLe16(&body, 4);
    appendLe16(&body, 32);
    appendLe16(&body, 22);
    appendLe16(&body, 32);
    appendLe32(&body, 0);
    body.append(QByteArray::fromHex("0300000000001000800000aa00389b71"));
    body.append("data", 4);
    appendLe32(&body, 4);
    body.append(QByteArray(4, char(0)));
    QByteArray result("RIFF", 4);
    appendLe32(&result, static_cast<quint32>(body.size()));
    result.append(body);
    return result;
}

QString writeWaveFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) {
        return {};
    }
    file.close();
    return path;
}

MultiChannelAudioWindow syntheticPlaneWave(double azimuthDegrees,
                                           double elevationDegrees,
                                           int corruptedChannel = -1)
{
    constexpr int Margin = 128;
    constexpr int Samples = 2048;
    QRandomGenerator random(0x5848);
    QVector<float> source(Samples + Margin * 2 + 2);
    for (float &value : source) {
        value = static_cast<float>(random.generateDouble() * 2.0 - 1.0);
    }
    const QVector3D direction = directionFromAngles(azimuthDegrees, elevationDegrees);
    MultiChannelAudioWindow window;
    window.sampleRate = 48000;
    window.centerSample = 10000;
    const QVector<MicrophonePosition> microphones = createDefaultArrayGeometry();
    for (int channelIndex = 0; channelIndex < microphones.size(); ++channelIndex) {
        const MicrophonePosition &microphone = microphones.at(channelIndex);
        const double delaySamples = (microphone.xMeters * direction.x()
                                     + microphone.yMeters * direction.y())
            * window.sampleRate / 343.0;
        QVector<float> channel(Samples);
        for (int sample = 0; sample < Samples; ++sample) {
            if (channelIndex == corruptedChannel) {
                channel[sample] = static_cast<float>(random.generateDouble() * 2.0 - 1.0);
                continue;
            }
            const double sourceIndex = Margin + sample - delaySamples;
            const int left = static_cast<int>(std::floor(sourceIndex));
            const double fraction = sourceIndex - left;
            channel[sample] = static_cast<float>(source.at(left) * (1.0 - fraction)
                                                  + source.at(left + 1) * fraction);
        }
        window.channels.append(channel);
    }
    return window;
}

QString createSyntheticPlaneWaveFile(const QString &directory,
                                     double azimuthDegrees,
                                     double elevationDegrees,
                                     int outputChannelCount = 48)
{
    const MultiChannelAudioWindow window = syntheticPlaneWave(azimuthDegrees,
                                                               elevationDegrees);
    RecordingSettings settings;
    settings.outputDirectory = directory;
    settings.baseName = QStringLiteral("synthetic_%1")
                            .arg(outputChannelCount);
    settings.sampleRate = window.sampleRate;
    for (int channel = 0; channel < outputChannelCount; ++channel) {
        settings.channels.append(channel);
    }
    MultiChannelWavWriter writer;
    QString error;
    if (!writer.open(settings, &error)) {
        return {};
    }
    constexpr int WrittenFrames = 10;
    for (int frameIndex = 0; frameIndex < WrittenFrames; ++frameIndex) {
        QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
        for (int sample = 0; sample < AudioFrameDecoder::SamplesPerFrame; ++sample) {
            const int sourceSample = frameIndex * AudioFrameDecoder::SamplesPerFrame + sample;
            for (int channel = 0; channel < 48; ++channel) {
                const qint32 value = static_cast<qint32>(std::lround(
                    window.channels.at(channel).at(sourceSample) * 4'000'000.0f));
                qToLittleEndian(value,
                                reinterpret_cast<uchar *>(payload.data()
                                    + (sample * 48 + channel) * 4));
            }
        }
        if (!writer.appendFrame(payload, &error)) {
            return {};
        }
    }
    if (!writer.close(QStringLiteral("test"), &error)) {
        return {};
    }
    return QDir(directory).filePath(settings.baseName + QStringLiteral(".wav"));
}

QByteArray syntheticPayload(const MultiChannelAudioWindow &window, int frameIndex)
{
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    for (int sample = 0; sample < AudioFrameDecoder::SamplesPerFrame; ++sample) {
        const int sourceSample =
            (frameIndex * AudioFrameDecoder::SamplesPerFrame + sample)
            % window.channels.first().size();
        for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
            const qint32 value = static_cast<qint32>(std::lround(
                window.channels.at(channel).at(sourceSample) * 4'000'000.0f));
            qToLittleEndian(value,
                            reinterpret_cast<uchar *>(payload.data()
                                + (sample * AudioFrameDecoder::ChannelCount + channel) * 4));
        }
    }
    return payload;
}

SpatialResponseGrid fieldOfViewGrid(int columns, int rows)
{
    SpatialResponseGrid grid;
    grid.columns = columns;
    grid.rows = rows;
    grid.directions.reserve(columns * rows);
    for (int row = 0; row < rows; ++row) {
        const double elevation = 30.0
            - 60.0 * (row + 0.5) / static_cast<double>(rows);
        for (int column = 0; column < columns; ++column) {
            const double azimuth = -60.0
                + 120.0 * (column + 0.5) / static_cast<double>(columns);
            grid.directions.append({azimuth, elevation});
        }
    }
    return grid;
}

} // namespace

class AudioAnalysisTest : public QObject
{
    Q_OBJECT

private slots:
    void createsCenteredSixByEightGeometry();
    void convertsAnglesToDirectionRoundTrip();
    void registersAndCreatesLocatorByStableId();
    void readsRecordedExtensibleWave();
    void rejectsTruncatedWaveFrame();
    void skipsUnknownPaddedChunks();
    void readsStandardIntegerPcm();
    void rejectsInvalidWaveMetadata();
    void projectsDirectionIntoConfiguredCameraView();
    void appliesCameraOrientationAndRoll();
    void rejectsInvalidCameraAndDirectionBehindView();
    void unprojectsVideoPointBackToDirection();
    void fitsCameraViewFromFourDirectionPoints();
    void rejectsDegenerateCameraCalibration();
    void roundTripsCompleteCalibrationProfile();
    void roundTripsCameraOnlyCalibrationProfile();
    void rejectsInvalidCalibrationProfiles();
    void transformsRadixTwoFftRoundTrip();
    void locatesSyntheticPlaneWaves();
    void rejectsSilentLocalizationWindow();
    void toleratesOneCorruptedMicrophone();
    void producesSpatialResponsePeakAtSyntheticDirection();
    void rejectsInvalidOrSilentSpatialResponse();
    void spatialResponseMatchesAfterLocalizationWarmup();
    void preparedSpatialGridInvalidatesForChangedDirections();
    void evaluatesMaximumSpatialGridWithinCpuBudget();
    void registersBuiltInLocator();
    void workerLoadsAndAnalyzesPausedFile();
    void workerRejectsStaleAnalysisRequest();
    void workerDisablesLocalizationForPartialRecording();
    void workerShutsDownCleanlyInDedicatedThread();
    void liveWorkerAccumulatesRealtimeFrames();
    void liveWorkerClearsAudioWhenInactive();
    void liveWorkerRejectsStaleCalibrationHistory();
    void liveWorkerShutsDownInDedicatedThread();
};

void AudioAnalysisTest::createsCenteredSixByEightGeometry()
{
    const QVector<MicrophonePosition> microphones = createDefaultArrayGeometry();
    QCOMPARE(microphones.size(), 48);
    QVERIFY(positionMatches(microphones.at(0), -0.175, 0.125, 0.0));
    QVERIFY(positionMatches(microphones.at(7), 0.175, 0.125, 0.0));
    QVERIFY(positionMatches(microphones.at(40), -0.175, -0.125, 0.0));
    QVERIFY(positionMatches(microphones.at(47), 0.175, -0.125, 0.0));
}

void AudioAnalysisTest::convertsAnglesToDirectionRoundTrip()
{
    const QVector3D direction = directionFromAngles(30.0, 20.0);
    const SoundSourceDirection result = anglesFromDirection(direction);
    QVERIFY(std::abs(result.azimuthDegrees - 30.0) < 1.0e-5);
    QVERIFY(std::abs(result.elevationDegrees - 20.0) < 1.0e-5);
    QVERIFY(direction.z() > 0.0f);
}

void AudioAnalysisTest::registersAndCreatesLocatorByStableId()
{
    SoundSourceLocatorRegistry registry;
    QString error;
    QVERIFY2(registry.registerFactory([] {
        return std::make_unique<FixedLocator>();
    }, &error), qPrintable(error));
    QVERIFY(!registry.registerFactory([] {
        return std::make_unique<FixedLocator>();
    }, &error));
    QVERIFY(error.contains(QStringLiteral("fixed")));
    QCOMPARE(registry.descriptors().size(), 1);
    QVERIFY(registry.create(QStringLiteral("fixed")) != nullptr);
    QVERIFY(registry.create(QStringLiteral("missing")) == nullptr);
}

void AudioAnalysisTest::readsRecordedExtensibleWave()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RecordingSettings settings;
    settings.outputDirectory = directory.path();
    settings.baseName = QStringLiteral("analysis");
    settings.sampleRate = 48000;
    for (int channel = 0; channel < 48; ++channel) {
        settings.channels.append(channel);
    }

    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    for (int sample = 0; sample < AudioFrameDecoder::SamplesPerFrame; ++sample) {
        for (int channel = 0; channel < 48; ++channel) {
            qToLittleEndian<qint32>((channel + 1) << 8,
                                    reinterpret_cast<uchar *>(payload.data()
                                        + (sample * 48 + channel) * 4));
        }
    }
    MultiChannelWavWriter writer;
    QString error;
    QVERIFY2(writer.open(settings, &error), qPrintable(error));
    QVERIFY2(writer.appendFrame(payload, &error), qPrintable(error));
    QVERIFY2(writer.close(QStringLiteral("test"), &error), qPrintable(error));

    WaveFileReader reader;
    QVERIFY2(reader.open(directory.filePath(QStringLiteral("analysis.wav")), &error),
             qPrintable(error));
    QCOMPARE(reader.info().channelCount, 48);
    QCOMPARE(reader.info().sampleRate, 48000);
    QCOMPARE(reader.info().containerBits, 32);
    QCOMPARE(reader.info().validBits, 24);
    QCOMPARE(reader.info().totalSamples, qint64(AudioFrameDecoder::SamplesPerFrame));
    const QByteArray pcm = reader.readMonoPcm16(17, 0, 4, &error);
    QCOMPARE(pcm.size(), 8);
    QCOMPARE(qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(pcm.constData())),
             qint16(18));
    const QVector<QVector<float>> channels = reader.readChannels(0, 2, &error);
    QCOMPARE(channels.size(), 48);
    QCOMPARE(channels.at(47).size(), 2);
    QVERIFY(std::abs(channels.at(47).at(0) - 48.0f / 32768.0f) < 1.0e-6f);
}

void AudioAnalysisTest::rejectsTruncatedWaveFrame()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray data;
    appendLe16(&data, 100);
    const QString path = writeWaveFile(directory.filePath(QStringLiteral("truncated.wav")),
                                       makePcmWave(1, 48000, 16, data));
    QVERIFY(!path.isEmpty());
    QFile file(path);
    QVERIFY(file.resize(file.size() - 1));

    WaveFileReader reader;
    QString error;
    QVERIFY(!reader.open(path, &error));
    QVERIFY(error.contains(QStringLiteral("截断"))
            || error.contains(QStringLiteral("完整采样帧")));
}

void AudioAnalysisTest::skipsUnknownPaddedChunks()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray data;
    appendLe16(&data, static_cast<quint16>(qint16(-1234)));
    const QString path = writeWaveFile(directory.filePath(QStringLiteral("junk.wav")),
                                       makePcmWave(1, 16000, 16, data, true));
    WaveFileReader reader;
    QString error;
    QVERIFY2(reader.open(path, &error), qPrintable(error));
    const QByteArray pcm = reader.readMonoPcm16(0, 0, 1, &error);
    QCOMPARE(qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(pcm.constData())),
             qint16(-1234));
}

void AudioAnalysisTest::readsStandardIntegerPcm()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QList<QPair<int, QByteArray>> cases {
        {16, QByteArray::fromHex("0040")},
        {24, QByteArray::fromHex("000040")},
        {32, QByteArray::fromHex("00000040")},
    };
    for (const auto &[bits, data] : cases) {
        const QString path = writeWaveFile(
            directory.filePath(QStringLiteral("pcm_%1.wav").arg(bits)),
            makePcmWave(1, 48000, bits, data));
        WaveFileReader reader;
        QString error;
        QVERIFY2(reader.open(path, &error), qPrintable(error));
        const QVector<QVector<float>> channels = reader.readChannels(0, 1, &error);
        QVERIFY(std::abs(channels.at(0).at(0) - 0.5f) < 1.0e-5f);
        const QByteArray pcm = reader.readMonoPcm16(0, 0, 1, &error);
        const qint16 sample = qFromLittleEndian<qint16>(
            reinterpret_cast<const uchar *>(pcm.constData()));
        QVERIFY(std::abs(static_cast<int>(sample) - 16384) <= 1);
    }
}

void AudioAnalysisTest::rejectsInvalidWaveMetadata()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray data(4, char(0));
    const QString badAlign = writeWaveFile(directory.filePath(QStringLiteral("align.wav")),
                                           makePcmWave(2, 48000, 16, data, false, 2));
    const QString nonPcm = writeWaveFile(directory.filePath(QStringLiteral("float.wav")),
                                         makeNonPcmExtensibleWave());
    WaveFileReader reader;
    QString error;
    QVERIFY(!reader.open(badAlign, &error));
    QVERIFY(error.contains(QStringLiteral("blockAlign")));
    QVERIFY(!reader.open(nonPcm, &error));
    QVERIFY(error.contains(QStringLiteral("PCM")));

    const QString valid = writeWaveFile(directory.filePath(QStringLiteral("valid.wav")),
                                        makePcmWave(1, 48000, 16,
                                                    QByteArray::fromHex("0100")));
    QVERIFY(reader.open(valid, &error));
    QVERIFY(reader.readMonoPcm16(1, 0, 1, &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("通道")));
}

void AudioAnalysisTest::projectsDirectionIntoConfiguredCameraView()
{
    const CameraViewSettings camera;
    const ProjectionResult center = projectDirection({0.0, 0.0, 1.0}, camera);
    QVERIFY(center.valid);
    QVERIFY(center.inFront);
    QVERIFY(center.inView);
    QVERIFY(QLineF(center.normalizedPoint, QPointF(0.5, 0.5)).length() < 1.0e-6);

    const ProjectionResult rightEdge = projectDirection({45.0, 0.0, 1.0}, camera);
    QVERIFY(rightEdge.valid);
    QVERIFY(rightEdge.inView);
    QVERIFY(std::abs(rightEdge.normalizedPoint.x() - 1.0) < 1.0e-6);
    QVERIFY(std::abs(rightEdge.normalizedPoint.y() - 0.5) < 1.0e-6);

    const ProjectionResult outside = projectDirection({70.0, 0.0, 1.0}, camera);
    QVERIFY(outside.valid);
    QVERIFY(!outside.inView);
    QVERIFY(outside.edgePoint.x() <= 1.0 && outside.edgePoint.x() >= 0.0);
    QVERIFY(outside.edgePoint.y() <= 1.0 && outside.edgePoint.y() >= 0.0);
    QVERIFY(std::abs(outside.edgePoint.x() - 1.0) < 1.0e-6);
}

void AudioAnalysisTest::appliesCameraOrientationAndRoll()
{
    CameraViewSettings camera;
    camera.centerAzimuthDegrees = 30.0;
    camera.centerElevationDegrees = 15.0;
    const ProjectionResult centered = projectDirection({30.0, 15.0, 1.0}, camera);
    QVERIFY(centered.inView);
    QVERIFY(QLineF(centered.normalizedPoint, QPointF(0.5, 0.5)).length() < 1.0e-6);

    CameraViewSettings levelCamera;
    const ProjectionResult above = projectDirection({0.0, 30.0, 1.0}, levelCamera);
    levelCamera.rollDegrees = 90.0;
    const ProjectionResult rolled = projectDirection({0.0, 30.0, 1.0}, levelCamera);
    QVERIFY(above.normalizedPoint.y() < 0.5);
    QVERIFY(std::abs(rolled.normalizedPoint.y() - 0.5) < 1.0e-6);
    QVERIFY(rolled.normalizedPoint.x() > 0.5);
}

void AudioAnalysisTest::rejectsInvalidCameraAndDirectionBehindView()
{
    CameraViewSettings invalid;
    invalid.horizontalFovDegrees = 180.0;
    QVERIFY(!projectDirection({0.0, 0.0, 1.0}, invalid).valid);

    const ProjectionResult behind = projectDirection({180.0, 0.0, 1.0}, {});
    QVERIFY(behind.valid);
    QVERIFY(!behind.inFront);
    QVERIFY(!behind.inView);
}

void AudioAnalysisTest::unprojectsVideoPointBackToDirection()
{
    CameraViewSettings camera;
    camera.centerAzimuthDegrees = 18.0;
    camera.centerElevationDegrees = -7.0;
    camera.horizontalFovDegrees = 82.0;
    camera.verticalFovDegrees = 56.0;
    camera.rollDegrees = 5.0;
    SoundSourceDirection source;
    source.azimuthDegrees = 32.0;
    source.elevationDegrees = 11.0;
    const ProjectionResult projected = projectDirection(source, camera);
    QVERIFY(projected.inView);

    const ScanDirection restored = unprojectVideoPoint(projected.normalizedPoint, camera);
    QVERIFY(std::abs(restored.azimuthDegrees - source.azimuthDegrees) < 1.0e-5);
    QVERIFY(std::abs(restored.elevationDegrees - source.elevationDegrees) < 1.0e-5);
}

void AudioAnalysisTest::fitsCameraViewFromFourDirectionPoints()
{
    CameraViewSettings expected;
    expected.centerAzimuthDegrees = 14.0;
    expected.centerElevationDegrees = -6.0;
    expected.horizontalFovDegrees = 84.0;
    expected.verticalFovDegrees = 58.0;
    expected.rollDegrees = 4.0;
    QVector<VideoCalibrationSample> samples;
    for (const ScanDirection direction : {
             ScanDirection {-25.0, 12.0},
             ScanDirection {35.0, 10.0},
             ScanDirection {30.0, -20.0},
             ScanDirection {-22.0, -18.0}}) {
        SoundSourceDirection source;
        source.azimuthDegrees = direction.azimuthDegrees;
        source.elevationDegrees = direction.elevationDegrees;
        const ProjectionResult projected = projectDirection(source, expected);
        QVERIFY(projected.inView);
        samples.append({direction, projected.normalizedPoint, 0.9, 1.0});
    }

    CameraViewSettings initial;
    initial.centerAzimuthDegrees = 5.0;
    initial.centerElevationDegrees = 2.0;
    initial.horizontalFovDegrees = 95.0;
    initial.verticalFovDegrees = 65.0;
    const CameraViewFitResult fitted = fitCameraView(samples, initial, {1280, 720});
    QVERIFY2(fitted.valid, qPrintable(fitted.error));
    QVERIFY(fitted.rmsPixels < 1.0);
    for (int index = 0; index < samples.size(); ++index) {
        SoundSourceDirection source;
        source.azimuthDegrees = samples.at(index).direction.azimuthDegrees;
        source.elevationDegrees = samples.at(index).direction.elevationDegrees;
        const ProjectionResult projected = projectDirection(source, fitted.camera);
        QVERIFY(projected.inFront);
        QVERIFY(QLineF(projected.normalizedPoint,
                       samples.at(index).normalizedVideoPoint).length() < 1.0e-3);
    }
}

void AudioAnalysisTest::rejectsDegenerateCameraCalibration()
{
    QVector<VideoCalibrationSample> samples(4);
    for (int index = 0; index < samples.size(); ++index) {
        samples[index].direction = {0.0, 0.0};
        samples[index].normalizedVideoPoint = {0.5, 0.5};
        samples[index].confidence = 0.9;
    }
    const CameraViewFitResult fitted = fitCameraView(samples, {}, {1280, 720});
    QVERIFY(!fitted.valid);
    QVERIFY(!fitted.error.isEmpty());
}

void AudioAnalysisTest::roundTripsCompleteCalibrationProfile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    VideoCalibrationProfile expected;
    expected.camera.centerAzimuthDegrees = 12.0;
    expected.camera.centerElevationDegrees = -4.0;
    expected.camera.horizontalFovDegrees = 82.0;
    expected.camera.verticalFovDegrees = 56.0;
    expected.camera.rollDegrees = 3.0;
    expected.referenceVideoSize = {1920, 1080};
    expected.calibrationRmsPixels = 2.5;
    expected.savedAtUtc = QDateTime::fromString(
        QStringLiteral("2026-07-23T03:04:05.000Z"), Qt::ISODateWithMs);
    expected.samples = {
        {{-20.0, 10.0}, {0.1, 0.2}, 0.8, 1.2},
        {{20.0, 10.0}, {0.9, 0.2}, 0.82, 1.1},
        {{20.0, -10.0}, {0.9, 0.8}, 0.9, 1.0},
        {{-20.0, -10.0}, {0.1, 0.8}, 0.88, 1.3},
    };
    const QString path = directory.filePath(QStringLiteral("calibration.json"));
    QString error;
    QVERIFY2(VideoCalibrationProfileStore::save(path, expected, &error),
             qPrintable(error));

    VideoCalibrationProfile actual;
    QVERIFY2(VideoCalibrationProfileStore::load(path, &actual, &error),
             qPrintable(error));
    QCOMPARE(actual, expected);
}

void AudioAnalysisTest::roundTripsCameraOnlyCalibrationProfile()
{
    QTemporaryDir directory;
    VideoCalibrationProfile expected;
    expected.camera.centerAzimuthDegrees = -15.0;
    expected.camera.horizontalFovDegrees = 70.0;
    expected.referenceVideoSize = {0, 0};
    expected.savedAtUtc = QDateTime::currentDateTimeUtc();
    const QString path = directory.filePath(QStringLiteral("camera-only.json"));
    QString error;
    QVERIFY(VideoCalibrationProfileStore::save(path, expected, &error));
    VideoCalibrationProfile actual;
    QVERIFY(VideoCalibrationProfileStore::load(path, &actual, &error));
    QCOMPARE(actual, expected);
    QVERIFY(actual.samples.isEmpty());
}

void AudioAnalysisTest::rejectsInvalidCalibrationProfiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("invalid.json"));
    QString error;
    VideoCalibrationProfile actual;
    QVERIFY(!VideoCalibrationProfileStore::load(path, &actual, &error));
    QVERIFY(!error.isEmpty());

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"format":"Wrong","version":1})");
    file.close();
    QVERIFY(!VideoCalibrationProfileStore::load(path, &actual, &error));

    VideoCalibrationProfile invalid;
    invalid.camera.horizontalFovDegrees = 180.0;
    QVERIFY(!VideoCalibrationProfileStore::save(path, invalid, &error));
    invalid = {};
    invalid.samples.resize(3);
    QVERIFY(!VideoCalibrationProfileStore::save(path, invalid, &error));
}

void AudioAnalysisTest::transformsRadixTwoFftRoundTrip()
{
    QVector<std::complex<double>> values {
        {1.0, 0.0}, {2.0, -1.0}, {-3.0, 0.5}, {0.25, 4.0},
        {-0.5, 2.0}, {3.0, 1.0}, {0.0, -2.0}, {1.5, 0.25},
    };
    const auto original = values;
    QVERIFY(Radix2Fft::transform(&values, false));
    QVERIFY(Radix2Fft::transform(&values, true));
    for (int index = 0; index < values.size(); ++index) {
        QVERIFY(std::abs(values.at(index) - original.at(index)) < 1.0e-9);
    }
    QVector<std::complex<double>> invalid(6);
    QVERIFY(!Radix2Fft::transform(&invalid, false));
}

void AudioAnalysisTest::locatesSyntheticPlaneWaves()
{
    const QList<QPair<double, double>> directions {
        {0.0, 0.0},
        {30.0, 15.0},
        {-35.0, -20.0},
    };
    for (const auto &[azimuth, elevation] : directions) {
        GccPhatLocator locator;
        LocatorConfiguration configuration;
        configuration.microphones = createDefaultArrayGeometry();
        QString error;
        QVERIFY2(locator.configure(configuration, &error), qPrintable(error));
        const QVector<SoundSourceDirection> results = locator.locate(
            syntheticPlaneWave(azimuth, elevation));
        QCOMPARE(results.size(), 1);
        QVERIFY2(std::abs(results.first().azimuthDegrees - azimuth) < 5.0,
                 qPrintable(QStringLiteral("azimuth expected %1 actual %2")
                                .arg(azimuth)
                                .arg(results.first().azimuthDegrees)));
        QVERIFY2(std::abs(results.first().elevationDegrees - elevation) < 5.0,
                 qPrintable(QStringLiteral("elevation expected %1 actual %2")
                                .arg(elevation)
                                .arg(results.first().elevationDegrees)));
        QVERIFY(results.first().confidence > 0.45);
        QVERIFY(results.first().validPairCount >= 40);
    }
}

void AudioAnalysisTest::rejectsSilentLocalizationWindow()
{
    GccPhatLocator locator;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(locator.configure(configuration));
    MultiChannelAudioWindow silence;
    silence.sampleRate = 48000;
    silence.channels.resize(48);
    for (QVector<float> &channel : silence.channels) {
        channel.fill(0.0f, 2048);
    }
    QVERIFY(locator.locate(silence).isEmpty());
}

void AudioAnalysisTest::toleratesOneCorruptedMicrophone()
{
    GccPhatLocator locator;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(locator.configure(configuration));
    const QVector<SoundSourceDirection> results = locator.locate(
        syntheticPlaneWave(20.0, -10.0, 0));
    QCOMPARE(results.size(), 1);
    QVERIFY(std::abs(results.first().azimuthDegrees - 20.0) < 6.0);
    QVERIFY(std::abs(results.first().elevationDegrees + 10.0) < 6.0);
}

void AudioAnalysisTest::producesSpatialResponsePeakAtSyntheticDirection()
{
    GccPhatLocator locator;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(locator.configure(configuration));
    auto *provider = dynamic_cast<ISpatialResponseProvider *>(&locator);
    QVERIFY(provider != nullptr);
    SpatialResponseGrid grid;
    grid.columns = 5;
    grid.rows = 3;
    for (double elevation : {-20.0, 10.0, 30.0}) {
        for (double azimuth : {-50.0, -10.0, 25.0, 50.0, 70.0}) {
            grid.directions.append({azimuth, elevation});
        }
    }
    const SpatialResponseResult response = provider->evaluateSpatialResponse(
        syntheticPlaneWave(25.0, 10.0), grid);
    QCOMPARE(response.values.size(), 15);
    const auto peak = std::max_element(response.values.cbegin(), response.values.cend());
    QCOMPARE(std::distance(response.values.cbegin(), peak), qsizetype(7));
    QVERIFY(response.reliability > 0.2);
    for (float value : response.values) {
        QVERIFY(std::isfinite(value));
        QVERIFY(value >= 0.0f);
    }
}

void AudioAnalysisTest::rejectsInvalidOrSilentSpatialResponse()
{
    GccPhatLocator locator;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(locator.configure(configuration));
    auto *provider = dynamic_cast<ISpatialResponseProvider *>(&locator);
    QVERIFY(provider != nullptr);
    SpatialResponseGrid invalid;
    invalid.columns = 2;
    invalid.rows = 2;
    invalid.directions = {{0.0, 0.0}};
    QVERIFY(provider->evaluateSpatialResponse(syntheticPlaneWave(0.0, 0.0),
                                              invalid).values.isEmpty());

    MultiChannelAudioWindow silence;
    silence.sampleRate = 48000;
    silence.channels.resize(48);
    for (QVector<float> &channel : silence.channels) {
        channel.fill(0.0f, 2048);
    }
    SpatialResponseGrid grid;
    grid.columns = 1;
    grid.rows = 1;
    grid.directions = {{0.0, 0.0}};
    const SpatialResponseResult response = provider->evaluateSpatialResponse(silence, grid);
    QVERIFY(response.values.isEmpty());
    QCOMPARE(response.reliability, 0.0);
}

void AudioAnalysisTest::spatialResponseMatchesAfterLocalizationWarmup()
{
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    const MultiChannelAudioWindow window = syntheticPlaneWave(18.0, -7.0);
    const SpatialResponseGrid grid = fieldOfViewGrid(32, 18);

    GccPhatLocator direct;
    GccPhatLocator warmed;
    QVERIFY(direct.configure(configuration));
    QVERIFY(warmed.configure(configuration));
    const SpatialResponseResult directResult =
        direct.evaluateSpatialResponse(window, grid);
    QVERIFY(!warmed.locate(window).isEmpty());
    const SpatialResponseResult warmedResult =
        warmed.evaluateSpatialResponse(window, grid);

    QCOMPARE(warmedResult.values.size(), directResult.values.size());
    for (int index = 0; index < directResult.values.size(); ++index) {
        QVERIFY(std::abs(warmedResult.values.at(index)
                         - directResult.values.at(index)) < 1.0e-6f);
    }
    QVERIFY(std::abs(warmedResult.reliability - directResult.reliability) < 1.0e-9);
}

void AudioAnalysisTest::preparedSpatialGridInvalidatesForChangedDirections()
{
    GccPhatLocator locator;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(locator.configure(configuration));
    const MultiChannelAudioWindow window = syntheticPlaneWave(25.0, 10.0);

    SpatialResponseGrid first;
    first.columns = 2;
    first.rows = 1;
    first.directions = {{-40.0, 10.0}, {25.0, 10.0}};
    locator.prepareSpatialResponseGrid(first);
    const SpatialResponseResult firstResponse =
        locator.evaluateSpatialResponse(window, first);
    QCOMPARE(std::distance(firstResponse.values.cbegin(),
                           std::max_element(firstResponse.values.cbegin(),
                                            firstResponse.values.cend())),
             qsizetype(1));

    SpatialResponseGrid changed = first;
    std::reverse(changed.directions.begin(), changed.directions.end());
    locator.prepareSpatialResponseGrid(changed);
    const SpatialResponseResult changedResponse =
        locator.evaluateSpatialResponse(window, changed);
    QCOMPARE(std::distance(changedResponse.values.cbegin(),
                           std::max_element(changedResponse.values.cbegin(),
                                            changedResponse.values.cend())),
             qsizetype(0));
}

void AudioAnalysisTest::evaluatesMaximumSpatialGridWithinCpuBudget()
{
    GccPhatLocator locator;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(locator.configure(configuration));
    MultiChannelAudioWindow window = syntheticPlaneWave(20.0, 5.0);
    const SpatialResponseGrid grid = fieldOfViewGrid(640, 360);
    locator.prepareSpatialResponseGrid(grid);
    locator.evaluateSpatialResponse(window, grid);
    QVector<qint64> measurements;
    QVector<qint64> localizationMeasurements;
    QVector<qint64> heatmapMeasurements;
    SpatialResponseResult response;
    for (int iteration = 0; iteration < 5; ++iteration) {
        ++window.centerSample;
        QElapsedTimer elapsed;
        elapsed.start();
        const QVector<SoundSourceDirection> directions = locator.locate(window);
        localizationMeasurements.append(elapsed.restart());
        response = locator.evaluateSpatialResponse(window, grid);
        heatmapMeasurements.append(elapsed.elapsed());
        measurements.append(localizationMeasurements.last()
                            + heatmapMeasurements.last());
        QVERIFY(!directions.isEmpty());
    }
    std::sort(measurements.begin(), measurements.end());
    std::sort(localizationMeasurements.begin(), localizationMeasurements.end());
    std::sort(heatmapMeasurements.begin(), heatmapMeasurements.end());
    const qint64 milliseconds = measurements.at(measurements.size() / 2);
    qInfo().noquote() << "640x360 SRP-PHAT median:" << milliseconds
                      << "ms; localization:"
                      << localizationMeasurements.at(
                             localizationMeasurements.size() / 2)
                      << "ms; heatmap:"
                      << heatmapMeasurements.at(heatmapMeasurements.size() / 2)
                      << "ms; samples:" << measurements;

    QCOMPARE(response.values.size(), 640 * 360);
    QVERIFY(std::all_of(response.values.cbegin(), response.values.cend(),
                        [](float value) { return std::isfinite(value); }));
#ifdef NDEBUG
    constexpr qint64 BudgetMilliseconds = 100;
#else
    constexpr qint64 BudgetMilliseconds = 1000;
#endif
    QVERIFY2(milliseconds <= BudgetMilliseconds,
             qPrintable(QStringLiteral("640x360 took %1 ms, budget is %2 ms")
                            .arg(milliseconds)
                            .arg(BudgetMilliseconds)));
}

void AudioAnalysisTest::registersBuiltInLocator()
{
    SoundSourceLocatorRegistry registry;
    registerBuiltInSoundSourceLocators(&registry);
    const auto locator = registry.create(QStringLiteral("gcc-phat-tdoa"));
    QVERIFY(locator != nullptr);
    QCOMPARE(locator->descriptor().displayName, QStringLiteral("GCC-PHAT 时差定位"));
}

void AudioAnalysisTest::workerLoadsAndAnalyzesPausedFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = createSyntheticPlaneWaveFile(directory.path(), 25.0, 10.0);
    QVERIFY(!path.isEmpty());
    AudioAnalysisWorker worker;
    QSignalSpy loaded(&worker, &AudioAnalysisWorker::fileLoaded);
    QSignalSpy located(&worker, &AudioAnalysisWorker::directionsReady);
    worker.loadFile(path, 1);
    QCOMPARE(loaded.count(), 1);
    QCOMPARE(qvariant_cast<WaveFileInfo>(loaded.first().at(0)).channelCount, 48);
    worker.analyzeAt(1000, 2);
    QCOMPARE(located.count(), 1);
    QCOMPARE(located.first().at(0).toULongLong(), quint64(2));
    const auto directions = qvariant_cast<QVector<SoundSourceDirection>>(
        located.first().at(1));
    QCOMPARE(directions.size(), 1);
    QVERIFY(std::abs(directions.first().azimuthDegrees - 25.0) < 5.0);
    QVERIFY(std::abs(directions.first().elevationDegrees - 10.0) < 5.0);
}

void AudioAnalysisTest::workerRejectsStaleAnalysisRequest()
{
    QTemporaryDir directory;
    const QString path = createSyntheticPlaneWaveFile(directory.path(), 0.0, 0.0);
    AudioAnalysisWorker worker;
    QSignalSpy located(&worker, &AudioAnalysisWorker::directionsReady);
    worker.loadFile(path, 5);
    worker.analyzeAt(1000, 4);
    QCOMPARE(located.count(), 0);
    worker.analyzeAt(1000, 6);
    QCOMPARE(located.count(), 1);
}

void AudioAnalysisTest::workerDisablesLocalizationForPartialRecording()
{
    QTemporaryDir directory;
    const QString path = createSyntheticPlaneWaveFile(directory.path(), 0.0, 0.0, 2);
    AudioAnalysisWorker worker;
    QSignalSpy status(&worker, &AudioAnalysisWorker::analysisStatusChanged);
    QSignalSpy located(&worker, &AudioAnalysisWorker::directionsReady);
    worker.loadFile(path, 1);
    QVERIFY(!status.isEmpty());
    QVERIFY(status.last().at(0).toString().contains(QStringLiteral("48 通道")));
    worker.analyzeAt(500, 2);
    QCOMPARE(located.count(), 1);
    QVERIFY(qvariant_cast<QVector<SoundSourceDirection>>(located.first().at(1)).isEmpty());
}

void AudioAnalysisTest::workerShutsDownCleanlyInDedicatedThread()
{
    QTemporaryDir directory;
    const QString path = createSyntheticPlaneWaveFile(directory.path(), -15.0, 5.0);
    auto *worker = new AudioAnalysisWorker;
    QThread thread;
    worker->moveToThread(&thread);
    connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    QSignalSpy loaded(worker, &AudioAnalysisWorker::fileLoaded);
    QSignalSpy stopped(worker, &AudioAnalysisWorker::shutdownFinished);
    thread.start();
    QVERIFY(QMetaObject::invokeMethod(worker,
                                      "loadFile",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, path),
                                      Q_ARG(quint64, quint64(1))));
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 3000);
    QVERIFY(QMetaObject::invokeMethod(worker, "shutdown", Qt::QueuedConnection));
    QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
    thread.quit();
    QVERIFY(thread.wait(3000));
}

void AudioAnalysisTest::liveWorkerAccumulatesRealtimeFrames()
{
    LiveLocalizationWorker worker;
    LiveLocalizationSettings settings;
    settings.updateIntervalMs = 20;
    worker.configure(settings);
    worker.setActive(true);
    QSignalSpy located(&worker, &LiveLocalizationWorker::localizationReady);
    const MultiChannelAudioWindow window = syntheticPlaneWave(22.0, -8.0);
    for (int frame = 0; frame < 10; ++frame) {
        worker.pushAdjustedFrame(syntheticPayload(window, frame), 0,
                                 static_cast<quint32>(frame * 4167));
    }
    QCOMPARE(located.count(), 0);
    worker.pushAdjustedFrame(syntheticPayload(window, 10), 0, 41670);
    QCOMPARE(located.count(), 1);
    const LiveLocalizationResult result =
        qvariant_cast<LiveLocalizationResult>(located.first().at(0));
    QCOMPARE(result.directions.size(), 1);
    QVERIFY(std::abs(result.directions.first().azimuthDegrees - 22.0) < 6.0);
    QVERIFY(std::abs(result.directions.first().elevationDegrees + 8.0) < 6.0);
}

void AudioAnalysisTest::liveWorkerClearsAudioWhenInactive()
{
    LiveLocalizationWorker worker;
    LiveLocalizationSettings settings;
    settings.updateIntervalMs = 20;
    worker.configure(settings);
    worker.setActive(true);
    QSignalSpy located(&worker, &LiveLocalizationWorker::localizationReady);
    QSignalSpy consumed(&worker, &LiveLocalizationWorker::audioFrameConsumed);
    const MultiChannelAudioWindow window = syntheticPlaneWave(0.0, 0.0);
    for (int frame = 0; frame < 10; ++frame) {
        worker.pushAdjustedFrame(syntheticPayload(window, frame), 0,
                                 static_cast<quint32>(frame * 4167));
    }
    worker.setActive(false);
    worker.pushAdjustedFrame(syntheticPayload(window, 10), 0, 41670);
    QCOMPARE(located.count(), 0);
    QCOMPARE(consumed.count(), 11);
    worker.setActive(true);
    worker.pushAdjustedFrame(syntheticPayload(window, 11), 0, 45837);
    QCOMPARE(located.count(), 0);
    QCOMPARE(consumed.count(), 12);
}

void AudioAnalysisTest::liveWorkerRejectsStaleCalibrationHistory()
{
    LiveLocalizationWorker worker;
    LiveLocalizationSettings settings;
    settings.updateIntervalMs = 100;
    worker.configure(settings);
    worker.setActive(true);
    QSignalSpy samples(&worker, &LiveLocalizationWorker::calibrationSampleReady);
    const MultiChannelAudioWindow window = syntheticPlaneWave(15.0, 5.0);
    for (int frame = 0; frame < 140; ++frame) {
        const quint32 timestamp = static_cast<quint32>(frame * 4167);
        worker.pushAdjustedFrame(syntheticPayload(window, frame), 0, timestamp);
    }
    const QByteArray silence(AudioFrameDecoder::FramePayloadBytes, char(0));
    for (int frame = 140; frame < 240; ++frame) {
        const quint32 timestamp = static_cast<quint32>(frame * 4167);
        worker.pushAdjustedFrame(silence, 0, timestamp);
    }

    worker.captureStableDirection(7);

    QCOMPARE(samples.count(), 1);
    QCOMPARE(samples.first().at(0).toULongLong(), quint64(7));
    QVERIFY(!samples.first().at(1).toBool());
}

void AudioAnalysisTest::liveWorkerShutsDownInDedicatedThread()
{
    auto *worker = new LiveLocalizationWorker;
    QThread thread;
    worker->moveToThread(&thread);
    connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    QSignalSpy stopped(worker, &LiveLocalizationWorker::shutdownFinished);
    thread.start();
    QVERIFY(QMetaObject::invokeMethod(worker, "shutdown", Qt::QueuedConnection));
    QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
    thread.quit();
    QVERIFY(thread.wait(3000));
}

QTEST_MAIN(AudioAnalysisTest)
#include "test_audioanalysis.moc"
