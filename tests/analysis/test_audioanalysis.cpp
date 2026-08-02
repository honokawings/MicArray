#include "analysis/arraygeometry.h"
#include "analysis/audioanalysisworker.h"
#include "analysis/calibrationprofile.h"
#include "analysis/cameraviewcalibrator.h"
#include "analysis/directionprojection.h"
#include "analysis/gccphatlocator.h"
#include "analysis/ispatialresponseprovider.h"
#include "analysis/livelocalizationworker.h"
#include "analysis/neuralmodeldata.h"
#include "analysis/neuralsrppeakrefiner.h"
#include "analysis/radix2fft.h"
#include "analysis/robustsrpphatscorer.h"
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

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

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

MultiChannelAudioWindow syntheticPlaneWaveWithNoiseAndReflection(
    double azimuthDegrees,
    double elevationDegrees)
{
    MultiChannelAudioWindow direct =
        syntheticPlaneWave(azimuthDegrees, elevationDegrees);
    const MultiChannelAudioWindow reflection =
        syntheticPlaneWave(azimuthDegrees + 25.0,
                           elevationDegrees + 10.0);
    constexpr int ReflectionDelaySamples = 144;
    double signalPower = 0.0;
    qsizetype sampleCount = 0;
    for (int channel = 0; channel < direct.channels.size();
         ++channel) {
        for (int sample = 0;
             sample < direct.channels.at(channel).size();
             ++sample) {
            const double reflected =
                sample >= ReflectionDelaySamples
                ? reflection.channels.at(channel).at(
                    sample - ReflectionDelaySamples)
                : 0.0;
            direct.channels[channel][sample] +=
                static_cast<float>(0.35 * reflected);
            signalPower += std::pow(
                direct.channels.at(channel).at(sample), 2.0);
            ++sampleCount;
        }
    }
    signalPower /= sampleCount;
    const double noiseStandardDeviation = std::sqrt(
        signalPower / std::pow(10.0, 5.0 / 10.0));
    QRandomGenerator random(0x52564d42);
    bool hasSpareGaussian = false;
    double spareGaussian = 0.0;
    auto gaussian = [&] {
        if (hasSpareGaussian) {
            hasSpareGaussian = false;
            return spareGaussian;
        }
        const double radius = std::sqrt(
            -2.0 * std::log(std::max(
                random.generateDouble(), 1.0e-12)));
        const double angle =
            2.0 * std::numbers::pi
            * random.generateDouble();
        spareGaussian = radius * std::sin(angle);
        hasSpareGaussian = true;
        return radius * std::cos(angle);
    };
    for (QVector<float> &channel : direct.channels) {
        for (float &sample : channel) {
            sample += static_cast<float>(
                noiseStandardDeviation * gaussian());
        }
    }
    return direct;
}

MultiChannelAudioWindow syntheticLowCoherencePlaneWave(
    double azimuthDegrees,
    double elevationDegrees)
{
    constexpr double SnrDecibels = -15.0;
    MultiChannelAudioWindow window =
        syntheticPlaneWave(azimuthDegrees, elevationDegrees);
    double signalPower = 0.0;
    qsizetype sampleCount = 0;
    for (const QVector<float> &channel : std::as_const(
             window.channels)) {
        for (float sample : channel) {
            signalPower += sample * sample;
            ++sampleCount;
        }
    }
    signalPower /= sampleCount;
    const double noiseStandardDeviation = std::sqrt(
        signalPower / std::pow(
            10.0, SnrDecibels / 10.0));
    QRandomGenerator random(0x54525553u);
    bool hasSpareGaussian = false;
    double spareGaussian = 0.0;
    auto gaussian = [&] {
        if (hasSpareGaussian) {
            hasSpareGaussian = false;
            return spareGaussian;
        }
        const double radius = std::sqrt(
            -2.0 * std::log(std::max(
                random.generateDouble(), 1.0e-12)));
        const double angle = 2.0 * std::numbers::pi
            * random.generateDouble();
        spareGaussian = radius * std::sin(angle);
        hasSpareGaussian = true;
        return radius * std::cos(angle);
    };
    for (QVector<float> &channel : window.channels) {
        for (float &sample : channel) {
            sample += static_cast<float>(
                noiseStandardDeviation * gaussian());
        }
    }
    return window;
}

MultiChannelAudioWindow syntheticNarrowbandPlaneWave(
    double azimuthDegrees,
    double elevationDegrees,
    double frequencyHz = 1000.0)
{
    constexpr int Samples = 2048;
    MultiChannelAudioWindow window;
    window.sampleRate = 48000;
    window.centerSample = 20000;
    const QVector3D direction = directionFromAngles(
        azimuthDegrees, elevationDegrees);
    const QVector<MicrophonePosition> microphones =
        createDefaultArrayGeometry();
    window.channels.resize(microphones.size());
    for (int channel = 0; channel < microphones.size();
         ++channel) {
        const MicrophonePosition &microphone =
            microphones.at(channel);
        const double delaySamples = (
            microphone.xMeters * direction.x()
            + microphone.yMeters * direction.y())
            * window.sampleRate / 343.0;
        QVector<float> &samples = window.channels[channel];
        samples.resize(Samples);
        for (int sample = 0; sample < Samples; ++sample) {
            samples[sample] = static_cast<float>(std::sin(
                2.0 * std::numbers::pi * frequencyHz
                * (sample - delaySamples)
                / window.sampleRate));
        }
    }
    return window;
}

MultiChannelAudioWindow syntheticBandLimitedPlaneWave(
    double azimuthDegrees,
    double elevationDegrees,
    double minimumFrequencyHz,
    double maximumFrequencyHz)
{
    constexpr int FftSize = 4096;
    constexpr int Samples = 2048;
    constexpr int Margin = 512;
    constexpr int SampleRate = 48000;
    QVector<std::complex<double>> spectrum(FftSize);
    QRandomGenerator random(0x42414e44);
    const int firstBin = static_cast<int>(std::ceil(
        minimumFrequencyHz * FftSize / SampleRate));
    const int lastBin = static_cast<int>(std::floor(
        maximumFrequencyHz * FftSize / SampleRate));
    for (int bin = firstBin; bin <= lastBin; ++bin) {
        const double phase =
            2.0 * std::numbers::pi * random.generateDouble();
        spectrum[bin] = std::polar(1.0, phase);
        spectrum[FftSize - bin] = std::conj(spectrum.at(bin));
    }
    if (!Radix2Fft::transform(&spectrum, true)) {
        return {};
    }
    double maximum = 0.0;
    for (const std::complex<double> &value : std::as_const(spectrum)) {
        maximum = std::max(maximum, std::abs(value.real()));
    }
    if (maximum <= 0.0) {
        return {};
    }

    const QVector3D direction = directionFromAngles(
        azimuthDegrees, elevationDegrees);
    const QVector<MicrophonePosition> microphones =
        createDefaultArrayGeometry();
    MultiChannelAudioWindow window;
    window.sampleRate = SampleRate;
    window.centerSample = 30000;
    window.channels.resize(microphones.size());
    for (int channel = 0; channel < microphones.size(); ++channel) {
        const MicrophonePosition &microphone =
            microphones.at(channel);
        const double delaySamples = (
            microphone.xMeters * direction.x()
            + microphone.yMeters * direction.y())
            * SampleRate / 343.0;
        QVector<float> &samples = window.channels[channel];
        samples.resize(Samples);
        for (int sample = 0; sample < Samples; ++sample) {
            const double sourceIndex =
                Margin + sample - delaySamples;
            const int left =
                static_cast<int>(std::floor(sourceIndex));
            const double fraction = sourceIndex - left;
            samples[sample] = static_cast<float>((
                spectrum.at(left).real() * (1.0 - fraction)
                + spectrum.at(left + 1).real() * fraction)
                / maximum);
        }
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

double angularDistanceDegrees(const ScanDirection &first,
                              const ScanDirection &second)
{
    const double cosine = std::clamp(
        static_cast<double>(QVector3D::dotProduct(
            directionFromAngles(first.azimuthDegrees,
                                first.elevationDegrees),
            directionFromAngles(second.azimuthDegrees,
                                second.elevationDegrees))),
        -1.0, 1.0);
    return qRadiansToDegrees(std::acos(cosine));
}

NeuralSrpPeakInput syntheticNeuralPeakInput()
{
    NeuralSrpPeakInput input;
    input.normalizedPatch.reserve(
        NeuralSrpPeakInput::PatchValueCount);
    for (int row = 0;
         row < NeuralSrpPeakInput::PatchSize;
         ++row) {
        for (int column = 0;
             column < NeuralSrpPeakInput::PatchSize;
             ++column) {
            const double x = column - 4.25;
            const double y = row - 3.75;
            input.normalizedPatch.append(
                static_cast<float>(
                    std::exp(-0.5
                             * (x * x / 2.25
                                + y * y / 1.44))));
        }
    }
    input.diagnostics = {
        0.92f, 1.0f, 0.87f, 0.5f,
        0.5f, 0.3f, 0.25f, 0.1f,
        0.86f, 0.7f, 0.81f, 0.96f,
    };
    return input;
}

NeuralSrpPeakInput exportedGoldenNeuralPeakInput()
{
    NeuralSrpPeakInput input;
    input.normalizedPatch.reserve(
        NeuralSrpPeakInput::PatchValueCount);
    for (int index = 0;
         index
            < NeuralSrpPeakInput::PatchValueCount;
         ++index) {
        input.normalizedPatch.append(
            NeuralModelData::GoldenInput.at(index));
    }
    for (int index = 0;
         index < input.diagnostics.size();
         ++index) {
        input.diagnostics[index] =
            NeuralModelData::GoldenInput.at(
                NeuralSrpPeakInput
                    ::PatchValueCount
                + index);
    }
    return input;
}

} // namespace

class AudioAnalysisTest : public QObject
{
    Q_OBJECT

private slots:
    void createsCenteredSixByEightGeometry();
    void groupsDefaultArrayAcrossTwoDimensions();
    void rejectsInsufficientValidChannels();
    void producesMultiBaselineResponseNearestOffGridSource();
    void refinesSingleSourceBelowHalfDegree();
    void refinesSingleSourceAtFieldEdgesBelowHalfDegree();
    void toleratesOneTwoAndFourCorruptedMicrophones();
    void localizesThroughNoiseAndSingleReflection();
    void lowCoherenceAccuratePeakObeysConfiguredTrustThreshold();
    void rejectsDiffuseAndInsufficientSpatialEvidence();
    void realtimeDefaultThresholdRejectsAmbiguousEvidence();
    void remainsFiniteForNarrowbandInput();
    void downweightsMarginalBandwidthSpatialEvidence();
    void neuralPeakModelRejectsInvalidInputs();
    void neuralPeakModelProducesBoundedFiniteOutput();
    void neuralPeakModelMatchesGoldenForwardResult();
    void samplesNormalizedNineByNineSincPatch();
    void neuralPeakRefinementPreservesPhysicalSafetyEnvelope();
    void neuralPeakRefinementBypassesBelowPhysicalThreshold();
    void neuralPeakHeatmapSharpeningContractsMainLobe();
    void neuralPeakHeatmapBlendsContinuouslyAtThreeSigma();
    void neuralPeakHeatmapBlendsCorrelatedEllipseAtThreeSigma();
    void neuralPeakPostprocessingStaysWithinCpuBudget();
    void liveWorkerRebuildsLocatorForNeuralConfiguration();
    void ignoresBackHemisphereSpatialDirections();
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
    void projectsDirectionsLinearlyAcrossWideAngleView();
    void roundTripsAngularProjectionWithOrientationAndRoll();
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
    void locatorAndHeatmapShareRefinedPeak();
    void smoothedLocalizationKeepsSpatialPeakAligned();
    void invalidWindowBreaksTemporalSmoothing();
    void preparedSpatialGridInvalidatesForChangedDirections();
    void evaluatesMaximumSpatialGridWithinCpuBudget();
    void evaluatesHighPrecisionSpatialGridWithinCpuBudget();
    void registersBuiltInLocator();
    void workerLoadsAndAnalyzesPausedFile();
    void workerRejectsStaleAnalysisRequest();
    void workerDisablesLocalizationForPartialRecording();
    void workerShutsDownCleanlyInDedicatedThread();
    void liveWorkerAccumulatesRealtimeFrames();
    void liveWorkerSpatialGridMatchesRefinedPeakAfterDirectionChange();
    void liveWorkerMarksMarginalSpatialPeakUntrusted();
    void liveWorkerClearsSpatialResponseWhenSourceDisappears();
    void liveWorkerRecoversFromTimestampRollback();
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

void AudioAnalysisTest::groupsDefaultArrayAcrossTwoDimensions()
{
    RobustSrpPhatScorer scorer;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(scorer.configure(configuration));
    QVERIFY(scorer.baselineGroupCount() >= 70);
    QVERIFY(scorer.realtimeBaselineGroupCount() >= 24);
    QVERIFY(scorer.realtimeBaselineGroupCount() <= 32);
}

void AudioAnalysisTest::rejectsInsufficientValidChannels()
{
    RobustSrpPhatScorer scorer;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(scorer.configure(configuration));
    MultiChannelAudioWindow window = syntheticPlaneWave(13.4, -7.2);
    for (int channel = 0; channel < 17; ++channel) {
        window.channels[channel].fill(0.0f);
    }
    QVERIFY(!scorer.prepareWindow(window));
    QVERIFY(!scorer.estimate().hasDirection);

    GccPhatLocator locator;
    configuration.confidenceThreshold = 0.0;
    QVERIFY(locator.configure(configuration));
    QVERIFY(locator.locate(window).isEmpty());
}

void AudioAnalysisTest::producesMultiBaselineResponseNearestOffGridSource()
{
    RobustSrpPhatScorer scorer;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    configuration.neuralPeakEnhancementEnabled = false;
    QVERIFY(scorer.configure(configuration));
    const SpatialResponseGrid grid = fieldOfViewGrid(64, 36);
    scorer.prepareGrid(grid);
    const MultiChannelAudioWindow window =
        syntheticPlaneWave(17.35, -8.65);
    QVERIFY(scorer.prepareWindow(window));
    const RobustSpatialEstimate result = scorer.estimate(&grid);

    QCOMPARE(result.gridValues.size(), grid.directions.size());
    QVERIFY(result.validBaselineGroupCount >= 24);
    QVERIFY(std::all_of(result.gridValues.cbegin(),
                        result.gridValues.cend(),
                        [](float value) {
                            return std::isfinite(value)
                                && value >= 0.0f && value <= 1.0f;
                        }));
    const int peak = static_cast<int>(std::distance(
        result.gridValues.cbegin(),
        std::max_element(result.gridValues.cbegin(),
                         result.gridValues.cend())));
    constexpr int NearestColumn = 41;
    constexpr int NearestRow = 23;
    QCOMPARE(peak, NearestRow * grid.columns + NearestColumn);
    const ScanDirection peakDirection = grid.directions.at(peak);
    const double peakError = angularDistanceDegrees(
        peakDirection, {17.35, -8.65});
    QVERIFY2(peakError < 2.5,
             qPrintable(QStringLiteral(
                 "peak=(%1,%2), angular error=%3")
                 .arg(peakDirection.azimuthDegrees)
                 .arg(peakDirection.elevationDegrees)
                 .arg(peakError)));
}

void AudioAnalysisTest::refinesSingleSourceBelowHalfDegree()
{
    const QVector<ScanDirection> expected {
        {3.37, -2.41},
        {27.65, 14.35},
        {-36.25, -18.75},
        {58.45, 22.15},
    };
    RobustSrpPhatScorer scorer;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(scorer.configure(configuration));

    for (const ScanDirection &direction : expected) {
        QVERIFY(scorer.prepareWindow(
            syntheticPlaneWave(direction.azimuthDegrees,
                               direction.elevationDegrees)));
        const RobustSpatialEstimate result = scorer.estimate();
        QVERIFY2(result.hasDirection,
                 qPrintable(QStringLiteral(
                     "missing direction for (%1,%2)")
                     .arg(direction.azimuthDegrees)
                     .arg(direction.elevationDegrees)));
        const ScanDirection actual {
            result.direction.azimuthDegrees,
            result.direction.elevationDegrees,
        };
        const double error =
            angularDistanceDegrees(actual, direction);
        QVERIFY2(error < 0.5,
                 qPrintable(QStringLiteral(
                     "expected=(%1,%2), actual=(%3,%4), error=%5")
                     .arg(direction.azimuthDegrees)
                     .arg(direction.elevationDegrees)
                     .arg(actual.azimuthDegrees)
                     .arg(actual.elevationDegrees)
                     .arg(error)));
        QVERIFY(std::isfinite(
            result.azimuthStandardDeviationDegrees));
        QVERIFY(std::isfinite(
            result.elevationStandardDeviationDegrees));
        QVERIFY(result.azimuthStandardDeviationDegrees > 0.0);
        QVERIFY(result.elevationStandardDeviationDegrees > 0.0);
        QVERIFY(result.reliability > 0.45);
    }
}

void AudioAnalysisTest::refinesSingleSourceAtFieldEdgesBelowHalfDegree()
{
    const QVector<ScanDirection> expected {
        {-72.35, 42.15},
        {-68.45, -43.25},
        {71.65, -41.75},
        {74.25, 39.35},
    };
    RobustSrpPhatScorer scorer;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    configuration.spatialResolutionMode =
        SpatialResolutionMode::HighPrecision;
    QVERIFY(scorer.configure(configuration));

    for (const ScanDirection &direction : expected) {
        QVERIFY(scorer.prepareWindow(
            syntheticPlaneWave(direction.azimuthDegrees,
                               direction.elevationDegrees)));
        const RobustSpatialEstimate result = scorer.estimate();
        QVERIFY2(result.hasDirection,
                 qPrintable(QStringLiteral(
                     "missing edge direction for (%1,%2), reliability=%3")
                     .arg(direction.azimuthDegrees)
                     .arg(direction.elevationDegrees)
                     .arg(result.reliability)));
        const ScanDirection actual {
            result.direction.azimuthDegrees,
            result.direction.elevationDegrees,
        };
        const double error =
            angularDistanceDegrees(actual, direction);
        QVERIFY2(error < 0.5,
                 qPrintable(QStringLiteral(
                     "edge expected=(%1,%2), actual=(%3,%4), error=%5")
                     .arg(direction.azimuthDegrees)
                     .arg(direction.elevationDegrees)
                     .arg(actual.azimuthDegrees)
                     .arg(actual.elevationDegrees)
                     .arg(error)));
    }
}

void AudioAnalysisTest::toleratesOneTwoAndFourCorruptedMicrophones()
{
    constexpr std::array<int, 4> CorruptedChannels {
        0, 11, 26, 47,
    };
    for (int corruptedCount : {1, 2, 4}) {
        MultiChannelAudioWindow window =
            syntheticPlaneWave(31.45, -12.65);
        QRandomGenerator random(
            0x4c4f4341u + static_cast<quint32>(corruptedCount));
        for (int index = 0; index < corruptedCount; ++index) {
            for (float &sample
                 : window.channels[CorruptedChannels.at(index)]) {
                sample = static_cast<float>(
                    random.generateDouble() * 2.0 - 1.0);
            }
        }

        RobustSrpPhatScorer scorer;
        LocatorConfiguration configuration;
        configuration.microphones = createDefaultArrayGeometry();
        QVERIFY(scorer.configure(configuration));
        QVERIFY(scorer.prepareWindow(window));
        const RobustSpatialEstimate result = scorer.estimate();
        QVERIFY2(result.hasDirection,
                 qPrintable(QStringLiteral(
                     "%1 corrupted channels produced no direction")
                     .arg(corruptedCount)));
        const ScanDirection actual {
            result.direction.azimuthDegrees,
            result.direction.elevationDegrees,
        };
        const double error = angularDistanceDegrees(
            actual, {31.45, -12.65});
        QVERIFY2(error < 1.5,
                 qPrintable(QStringLiteral(
                     "%1 corrupted channels: actual=(%2,%3), error=%4, reliability=%5")
                     .arg(corruptedCount)
                     .arg(actual.azimuthDegrees)
                     .arg(actual.elevationDegrees)
                     .arg(error)
                     .arg(result.reliability)));
    }
}

void AudioAnalysisTest::localizesThroughNoiseAndSingleReflection()
{
    RobustSrpPhatScorer scorer;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(scorer.configure(configuration));
    QVERIFY(scorer.prepareWindow(
        syntheticPlaneWaveWithNoiseAndReflection(18.35, -6.75)));
    const RobustSpatialEstimate result = scorer.estimate();
    QVERIFY(result.hasDirection);
    const ScanDirection actual {
        result.direction.azimuthDegrees,
        result.direction.elevationDegrees,
    };
    const double error = angularDistanceDegrees(
        actual, {18.35, -6.75});
    QVERIFY2(error < 2.0,
             qPrintable(QStringLiteral(
                 "actual=(%1,%2), error=%3, reliability=%4")
                 .arg(actual.azimuthDegrees)
                 .arg(actual.elevationDegrees)
                 .arg(error)
                 .arg(result.reliability)));
    QVERIFY(std::isfinite(result.reliability));
}

void AudioAnalysisTest::
lowCoherenceAccuratePeakObeysConfiguredTrustThreshold()
{
    constexpr double ExpectedAzimuth = 18.35;
    constexpr double ExpectedElevation = -6.75;
    const MultiChannelAudioWindow window =
        syntheticLowCoherencePlaneWave(
            ExpectedAzimuth, ExpectedElevation);

    LocatorConfiguration diagnosticConfiguration;
    diagnosticConfiguration.microphones =
        createDefaultArrayGeometry();
    diagnosticConfiguration.neuralPeakEnhancementEnabled = false;
    QString diagnostics;
    for (int run = 0; run < 2; ++run) {
        RobustSrpPhatScorer scorer;
        QVERIFY(scorer.configure(diagnosticConfiguration));
        QVERIFY(scorer.prepareWindow(window));
        const RobustSpatialEstimate estimate = scorer.estimate();
        const double estimateError = angularDistanceDegrees(
            {estimate.direction.azimuthDegrees,
             estimate.direction.elevationDegrees},
            {ExpectedAzimuth, ExpectedElevation});
        diagnostics = QStringLiteral(
            "run=%1 error=%2 reliability=%3 steered=%4 "
            "coverage=%5 contrast=%6 curvature=%7 "
            "std=(%8,%9) groups=%10")
            .arg(run)
            .arg(estimateError)
            .arg(estimate.reliability)
            .arg(estimate.steeredChannelCoherence)
            .arg(estimate.coherentCoverage)
            .arg(estimate.peakContrast)
            .arg(estimate.curvatureScore)
            .arg(estimate.azimuthStandardDeviationDegrees)
            .arg(estimate.elevationStandardDeviationDegrees)
            .arg(estimate.validBaselineGroupCount);
        QVERIFY2(estimateError < 2.0,
                 qPrintable(diagnostics));
        QVERIFY2(estimate.steeredChannelCoherence < 0.15,
                 qPrintable(diagnostics));
        QVERIFY2(estimate.reliability > 0.30,
                 qPrintable(diagnostics));
        QVERIFY2(estimate.reliability < 0.60,
                 qPrintable(diagnostics));
        QVERIFY2(
            estimate.azimuthStandardDeviationDegrees > 0.0,
            qPrintable(diagnostics));
        QVERIFY2(
            estimate.elevationStandardDeviationDegrees > 0.0,
            qPrintable(diagnostics));
    }

    LocatorConfiguration permissiveConfiguration =
        diagnosticConfiguration;
    permissiveConfiguration.confidenceThreshold = 0.30;
    GccPhatLocator permissive;
    QVERIFY(permissive.configure(permissiveConfiguration));
    const QVector<SoundSourceDirection> accepted =
        permissive.locate(window);
    QVERIFY2(!accepted.isEmpty(), qPrintable(diagnostics));
    const double acceptedError = angularDistanceDegrees(
        {accepted.first().azimuthDegrees,
         accepted.first().elevationDegrees},
        {ExpectedAzimuth, ExpectedElevation});
    QVERIFY2(acceptedError < 2.0, qPrintable(diagnostics));

    LocatorConfiguration strictConfiguration =
        diagnosticConfiguration;
    strictConfiguration.confidenceThreshold = 0.60;
    GccPhatLocator strict;
    QVERIFY(strict.configure(strictConfiguration));
    QVERIFY2(strict.locate(window).isEmpty(),
             qPrintable(diagnostics));
}

void AudioAnalysisTest::rejectsDiffuseAndInsufficientSpatialEvidence()
{
    RobustSrpPhatScorer scorer;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(scorer.configure(configuration));
    for (quint32 seed = 0; seed < 8; ++seed) {
        MultiChannelAudioWindow diffuse;
        diffuse.sampleRate = 48000;
        diffuse.centerSample = 48000 + seed * 2048;
        diffuse.channels.resize(48);
        QRandomGenerator random(0x44494646u + seed);
        for (QVector<float> &channel : diffuse.channels) {
            channel.resize(2048);
            for (float &sample : channel) {
                sample = static_cast<float>(
                    random.generateDouble() * 2.0 - 1.0);
            }
        }
        const bool prepared = scorer.prepareWindow(diffuse);
        const RobustSpatialEstimate result = scorer.estimate();
        QVERIFY2(!prepared || !result.hasDirection
                     || result.reliability < 0.2,
                 qPrintable(QStringLiteral(
                     "diffuse seed=%1 produced direction=(%2,%3), reliability=%4, groups=%5, psr=%6, std=(%7,%8), coverage=%9, steered=%10, contrast=%11, curvature=%12")
                     .arg(seed)
                     .arg(result.direction.azimuthDegrees)
                     .arg(result.direction.elevationDegrees)
                     .arg(result.reliability)
                     .arg(result.validBaselineGroupCount)
                     .arg(result.peakToSidelobeRatio)
                     .arg(result.azimuthStandardDeviationDegrees)
                     .arg(result.elevationStandardDeviationDegrees)
                     .arg(result.coherentCoverage)
                     .arg(result.steeredChannelCoherence)
                     .arg(result.peakContrast)
                     .arg(result.curvatureScore)));
    }
}

void AudioAnalysisTest::
realtimeDefaultThresholdRejectsAmbiguousEvidence()
{
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    configuration.confidenceThreshold = 0.40;
    configuration.neuralPeakEnhancementEnabled = false;

    MultiChannelAudioWindow silence;
    silence.sampleRate = 48000;
    silence.centerSample = 48000;
    silence.channels.resize(48);
    for (QVector<float> &channel : silence.channels) {
        channel.fill(0.0f, 2048);
    }
    GccPhatLocator silenceLocator;
    QVERIFY(silenceLocator.configure(configuration));
    QVERIFY(silenceLocator.locate(silence).isEmpty());

    for (quint32 seed = 0; seed < 8; ++seed) {
        MultiChannelAudioWindow diffuse;
        diffuse.sampleRate = 48000;
        diffuse.centerSample = 48000 + seed * 2048;
        diffuse.channels.resize(48);
        QRandomGenerator random(0x44494646u + seed);
        for (QVector<float> &channel : diffuse.channels) {
            channel.resize(2048);
            for (float &sample : channel) {
                sample = static_cast<float>(
                    random.generateDouble() * 2.0 - 1.0);
            }
        }
        GccPhatLocator locator;
        QVERIFY(locator.configure(configuration));
        const QVector<SoundSourceDirection> directions =
            locator.locate(diffuse);
        if (!directions.isEmpty()) {
            RobustSrpPhatScorer diagnosticsScorer;
            QVERIFY(diagnosticsScorer.configure(configuration));
            QVERIFY(diagnosticsScorer.prepareWindow(diffuse));
            const RobustSpatialEstimate estimate =
                diagnosticsScorer.estimate();
            QFAIL(qPrintable(QStringLiteral(
                "diffuse seed=%1 passed threshold 0.40; "
                "reliability=%2 steered=%3 "
                "coverage=%4 contrast=%5 curvature=%6 "
                "std=(%7,%8) groups=%9")
                                 .arg(seed)
                                 .arg(estimate.reliability)
                                 .arg(estimate.steeredChannelCoherence)
                                 .arg(estimate.coherentCoverage)
                                 .arg(estimate.peakContrast)
                                 .arg(estimate.curvatureScore)
                                 .arg(estimate.azimuthStandardDeviationDegrees)
                                 .arg(estimate.elevationStandardDeviationDegrees)
                                 .arg(estimate.validBaselineGroupCount)));
        }
    }

    for (double frequencyHz
         : {350.0, 1000.0, 3500.0, 7000.0}) {
        GccPhatLocator locator;
        QVERIFY(locator.configure(configuration));
        const MultiChannelAudioWindow narrowband =
            syntheticNarrowbandPlaneWave(
                14.2, -4.7, frequencyHz);
        QVERIFY2(locator.locate(narrowband).isEmpty(),
                 qPrintable(QStringLiteral(
                     "%1 Hz narrowband passed threshold 0.40")
                                .arg(frequencyHz)));
    }
}

void AudioAnalysisTest::remainsFiniteForNarrowbandInput()
{
    RobustSrpPhatScorer scorer;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(scorer.configure(configuration));
    const SpatialResponseGrid grid =
        fieldOfViewGrid(64, 36);
    scorer.prepareGrid(grid);
    for (double frequencyHz : {350.0, 1000.0, 3500.0, 7000.0}) {
        const bool prepared = scorer.prepareWindow(
            syntheticNarrowbandPlaneWave(
                14.2, -4.7, frequencyHz));
        const RobustSpatialEstimate result =
            scorer.estimate(&grid);

        QVERIFY(std::isfinite(result.reliability));
        QVERIFY(std::isfinite(result.direction.azimuthDegrees));
        QVERIFY(std::isfinite(result.direction.elevationDegrees));
        QVERIFY(std::all_of(
            result.gridValues.cbegin(),
            result.gridValues.cend(),
            [](float value) {
                return std::isfinite(value)
                    && value >= 0.0f && value <= 1.0f;
            }));
        QVERIFY2(!prepared || !result.hasDirection
                     || result.reliability
                        < configuration.confidenceThreshold,
                 qPrintable(QStringLiteral(
                     "%1 Hz narrowband input produced reliability=%2")
                     .arg(frequencyHz)
                     .arg(result.reliability)));
    }
}

void AudioAnalysisTest::downweightsMarginalBandwidthSpatialEvidence()
{
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    RobustSrpPhatScorer broadScorer;
    RobustSrpPhatScorer limitedScorer;
    QVERIFY(broadScorer.configure(configuration));
    QVERIFY(limitedScorer.configure(configuration));
    QVERIFY(broadScorer.prepareWindow(
        syntheticPlaneWave(14.2, -4.7)));
    QVERIFY(limitedScorer.prepareWindow(
        syntheticBandLimitedPlaneWave(
            14.2, -4.7, 1000.0, 2400.0)));
    const RobustSpatialEstimate broad =
        broadScorer.estimate();
    const RobustSpatialEstimate limited =
        limitedScorer.estimate();

    QVERIFY(broad.hasDirection);
    QVERIFY(std::isfinite(limited.reliability));
    QVERIFY2(limited.reliability
                 < broad.reliability * 0.85,
             qPrintable(QStringLiteral(
                 "broad reliability=%1, marginal-band reliability=%2")
                 .arg(broad.reliability)
                 .arg(limited.reliability)));
}

void AudioAnalysisTest::neuralPeakModelRejectsInvalidInputs()
{
    const NeuralSrpPeakRefiner refiner;
    QVERIFY(refiner.modelValid());

    NeuralSrpPeakInput tooShort =
        syntheticNeuralPeakInput();
    tooShort.normalizedPatch.removeLast();
    QVERIFY(!refiner.infer(tooShort).valid);

    NeuralSrpPeakInput tooLong =
        syntheticNeuralPeakInput();
    tooLong.normalizedPatch.append(0.0f);
    QVERIFY(!refiner.infer(tooLong).valid);

    NeuralSrpPeakInput constant =
        syntheticNeuralPeakInput();
    constant.normalizedPatch.fill(
        0.5f,
        NeuralSrpPeakInput::PatchValueCount);
    QVERIFY(!refiner.infer(constant).valid);

    NeuralSrpPeakInput notANumber =
        syntheticNeuralPeakInput();
    notANumber.normalizedPatch[10] =
        std::numeric_limits<float>::quiet_NaN();
    QVERIFY(!refiner.infer(notANumber).valid);

    NeuralSrpPeakInput infinite =
        syntheticNeuralPeakInput();
    infinite.normalizedPatch[20] =
        std::numeric_limits<float>::infinity();
    QVERIFY(!refiner.infer(infinite).valid);

    NeuralSrpPeakInput invalidDiagnostic =
        syntheticNeuralPeakInput();
    invalidDiagnostic.diagnostics[3] =
        std::numeric_limits<float>::infinity();
    QVERIFY(!refiner.infer(invalidDiagnostic).valid);
}

void AudioAnalysisTest::
neuralPeakModelProducesBoundedFiniteOutput()
{
    const NeuralSrpPeakRefiner refiner;
    const NeuralSrpPeakOutput output =
        refiner.infer(syntheticNeuralPeakInput());

    QVERIFY(output.valid);
    QVERIFY(std::isfinite(output.deltaAzimuthCells));
    QVERIFY(std::isfinite(output.deltaElevationCells));
    QVERIFY(std::abs(output.deltaAzimuthCells) <= 1.0);
    QVERIFY(std::abs(output.deltaElevationCells) <= 1.0);
    QVERIFY(output.azimuthSigmaScale >= 0.35
            && output.azimuthSigmaScale <= 1.0);
    QVERIFY(output.elevationSigmaScale >= 0.35
            && output.elevationSigmaScale <= 1.0);
    QVERIFY(std::abs(output.correlationResidual) <= 0.35);
    QVERIFY(output.confidence >= 0.0
            && output.confidence <= 1.0);
}

void AudioAnalysisTest::
neuralPeakModelMatchesGoldenForwardResult()
{
    const NeuralSrpPeakRefiner refiner;
    const NeuralSrpPeakOutput output =
        refiner.infer(
            exportedGoldenNeuralPeakInput());

    QVERIFY(output.valid);
    QVERIFY(std::abs(output.deltaAzimuthCells
                     - NeuralModelData
                           ::GoldenOutput.at(0))
            < 1.0e-6);
    QVERIFY(std::abs(output.deltaElevationCells
                     - NeuralModelData
                           ::GoldenOutput.at(1))
            < 1.0e-6);
    QVERIFY(std::abs(output.azimuthSigmaScale
                     - NeuralModelData
                           ::GoldenOutput.at(2))
            < 1.0e-6);
    QVERIFY(std::abs(output.elevationSigmaScale
                     - NeuralModelData
                           ::GoldenOutput.at(3))
            < 1.0e-6);
    QVERIFY(std::abs(output.correlationResidual
                     - NeuralModelData
                           ::GoldenOutput.at(4))
            < 1.0e-6);
    QVERIFY(std::abs(output.confidence
                     - NeuralModelData
                           ::GoldenOutput.at(5))
            < 1.0e-6);
}

void AudioAnalysisTest::
samplesNormalizedNineByNineSincPatch()
{
    LocatorConfiguration configuration;
    configuration.microphones =
        createDefaultArrayGeometry();
    configuration.spatialResolutionMode =
        SpatialResolutionMode::HighPrecision;
    configuration.neuralPeakEnhancementEnabled = false;
    RobustSrpPhatScorer scorer;
    QVERIFY(scorer.configure(configuration));
    QVERIFY(scorer.prepareWindow(
        syntheticPlaneWave(18.4, -7.3)));
    const RobustSpatialEstimate physical =
        scorer.estimate();
    QVERIFY(physical.hasDirection);

    const std::optional<SrpLocalPeakPatch> patch =
        scorer.sampleLocalPeakPatch(physical);

    QVERIFY(patch.has_value());
    QCOMPARE(patch->normalizedValues.size(), 81);
    QVERIFY(std::all_of(
        patch->normalizedValues.cbegin(),
        patch->normalizedValues.cend(),
        [](float value) {
            return std::isfinite(value)
                && value >= 0.0f && value <= 1.0f;
        }));
    const auto [minimum, maximum] =
        std::minmax_element(
            patch->normalizedValues.cbegin(),
            patch->normalizedValues.cend());
    QVERIFY(*maximum - *minimum > 1.0e-5f);
    QCOMPARE(
        patch->azimuthStepDegrees,
        std::clamp(
            physical.azimuthStandardDeviationDegrees
                / 2.5,
            0.25, 1.5));
    QCOMPARE(
        patch->elevationStepDegrees,
        std::clamp(
            physical.elevationStandardDeviationDegrees
                / 2.5,
            0.25, 1.5));
    QVERIFY(std::isfinite(
        patch->centerPhysicalScore));
    QVERIFY(patch->dynamicRange > 1.0e-9);
    QVERIFY(std::abs(
                patch->diagnostics.at(0)
                - physical.direction.azimuthDegrees
                    / 90.0)
            < 1.0e-6);
    QVERIFY(std::abs(
                patch->diagnostics.at(1)
                - physical.direction.elevationDegrees
                    / 90.0)
            < 1.0e-6);
}

void AudioAnalysisTest::
neuralPeakRefinementPreservesPhysicalSafetyEnvelope()
{
    LocatorConfiguration physicalConfiguration;
    physicalConfiguration.microphones =
        createDefaultArrayGeometry();
    physicalConfiguration.spatialResolutionMode =
        SpatialResolutionMode::HighPrecision;
    physicalConfiguration.neuralPeakEnhancementEnabled =
        false;
    LocatorConfiguration neuralConfiguration =
        physicalConfiguration;
    neuralConfiguration.neuralPeakEnhancementEnabled =
        true;
    neuralConfiguration.neuralPeakEnhancementStrength =
        0.65;

    const MultiChannelAudioWindow window =
        syntheticPlaneWave(23.37, -11.42);
    const SpatialResponseGrid grid =
        fieldOfViewGrid(32, 18);
    GccPhatLocator physicalLocator;
    GccPhatLocator neuralLocator;
    QVERIFY(physicalLocator.configure(
        physicalConfiguration));
    QVERIFY(neuralLocator.configure(
        neuralConfiguration));

    const SpatialResponseResult physical =
        physicalLocator.evaluateSpatialResponse(
            window, grid);
    const SpatialResponseResult neural =
        neuralLocator.evaluateSpatialResponse(
            window, grid);

    QVERIFY(physical.hasRefinedPeak);
    QVERIFY(neural.hasRefinedPeak);
    QVERIFY(!physical.neuralPeakAttempted);
    QVERIFY(!physical.neuralPeakApplied);
    QVERIFY(neural.neuralPeakAttempted);
    QVERIFY(std::isfinite(neural.neuralPeakConfidence));
    QVERIFY(neural.neuralPeakConfidence >= 0.0);
    QVERIFY(neural.neuralPeakConfidence <= 1.0);
    QVERIFY(angularDistanceDegrees(
                neural.physicalPeak,
                physical.refinedPeak)
            < 1.0e-6);
    QVERIFY(angularDistanceDegrees(
                neural.refinedPeak,
                neural.physicalPeak)
            <= 1.5 + 1.0e-9);
    QVERIFY2(
        neural.neuralPeakApplied,
        qPrintable(QStringLiteral(
            "confidence=%1 correction=(%2,%3)")
                       .arg(neural.neuralPeakConfidence)
                       .arg(
                           neural
                               .neuralAzimuthCorrectionDegrees)
                       .arg(
                           neural
                               .neuralElevationCorrectionDegrees)));
    QVERIFY(neural.neuralPeakConfidence >= 0.55);
    QVERIFY(neural.azimuthStandardDeviationDegrees
            <= physical
                .azimuthStandardDeviationDegrees);
    QVERIFY(neural.elevationStandardDeviationDegrees
            <= physical
                .elevationStandardDeviationDegrees);
}

void AudioAnalysisTest::
neuralPeakRefinementBypassesBelowPhysicalThreshold()
{
    LocatorConfiguration configuration;
    configuration.microphones =
        createDefaultArrayGeometry();
    configuration.spatialResolutionMode =
        SpatialResolutionMode::HighPrecision;
    configuration.confidenceThreshold = 1.0;
    configuration.neuralPeakEnhancementEnabled = true;
    RobustSrpPhatScorer scorer;
    QVERIFY(scorer.configure(configuration));
    QVERIFY(scorer.prepareWindow(
        syntheticPlaneWaveWithNoiseAndReflection(
            23.37, -11.42)));

    const RobustSpatialEstimate estimate =
        scorer.estimate();

    QVERIFY(estimate.hasDirection);
    QVERIFY(estimate.reliability < 1.0);
    QVERIFY(!estimate.neuralPeakAttempted);
    QVERIFY(!estimate.neuralPeakApplied);
    QCOMPARE(
        estimate.direction.azimuthDegrees,
        estimate.physicalPeak.azimuthDegrees);
    QCOMPARE(
        estimate.direction.elevationDegrees,
        estimate.physicalPeak.elevationDegrees);
}

void AudioAnalysisTest::
neuralPeakHeatmapSharpeningContractsMainLobe()
{
    SpatialResponseGrid grid;
    grid.columns = 11;
    grid.rows = 11;
    QVector<float> values;
    for (int row = 0; row < grid.rows; ++row) {
        const double elevation = 5.0 - row;
        for (int column = 0;
             column < grid.columns;
             ++column) {
            const double azimuth = column - 5.0;
            grid.directions.append({
                azimuth, elevation,
            });
            values.append(static_cast<float>(
                std::exp(-0.5 * (
                    (azimuth + 1.0) * (azimuth + 1.0)
                    + elevation * elevation)
                    / 4.0)));
        }
    }

    RobustSpatialEstimate estimate;
    estimate.hasDirection = true;
    estimate.direction.azimuthDegrees = 0.0;
    estimate.direction.elevationDegrees = 0.0;
    estimate.azimuthStandardDeviationDegrees = 1.5;
    estimate.elevationStandardDeviationDegrees = 1.5;
    estimate.azimuthElevationCorrelation = 0.0;
    estimate.neuralPeakApplied = true;
    estimate.neuralPeakConfidence = 1.0;
    const QVector<float> original = values;

    sharpenNeuralSrpHeatmap(
        grid, estimate, 0.65, &values);

    int originalHalfPowerCount = 0;
    int sharpenedHalfPowerCount = 0;
    for (int index = 0; index < values.size(); ++index) {
        originalHalfPowerCount +=
            original.at(index) >= 0.5f ? 1 : 0;
        sharpenedHalfPowerCount +=
            values.at(index) >= 0.5f ? 1 : 0;
        QVERIFY(std::isfinite(values.at(index)));
        QVERIFY(values.at(index) >= 0.0f);
        QVERIFY(values.at(index) <= 1.0f);
    }
    const int sharpenedPeak = static_cast<int>(
        std::distance(
            values.cbegin(),
            std::max_element(
                values.cbegin(), values.cend())));
    QCOMPARE(sharpenedPeak, 5 * 11 + 5);
    QVERIFY(values.at(5 * 11 + 5)
            > values.at(5 * 11 + 4));
    QCOMPARE(values.first(), original.first());
    QVERIFY(sharpenedHalfPowerCount > 0);
    QVERIFY(sharpenedHalfPowerCount
            <= 0.75 * originalHalfPowerCount);
}

void AudioAnalysisTest::
neuralPeakHeatmapBlendsContinuouslyAtThreeSigma()
{
    SpatialResponseGrid grid;
    grid.columns = 7;
    grid.rows = 1;
    grid.directions = {
        {0.0, 0.0},
        {1.0, 0.0},
        {2.0, 0.0},
        {2.9, 0.0},
        {3.0, 0.0},
        {3.1, 0.0},
        {4.0, 0.0},
    };
    QVector<float> values {
        1.0f, 0.8f, 0.8f, 0.8f,
        0.8f, 0.8f, 0.8f,
    };
    const QVector<float> original = values;

    RobustSpatialEstimate estimate;
    estimate.hasDirection = true;
    estimate.direction = {0.0, 0.0};
    estimate.azimuthStandardDeviationDegrees = 1.0;
    estimate.elevationStandardDeviationDegrees = 1.0;
    estimate.neuralPeakApplied = true;
    estimate.neuralPeakConfidence = 1.0;

    sharpenNeuralSrpHeatmap(
        grid, estimate, 0.65, &values);

    QCOMPARE(values.at(4), original.at(4));
    QCOMPARE(values.at(5), original.at(5));
    QCOMPARE(values.at(6), original.at(6));
    QVERIFY2(std::abs(values.at(3) - values.at(4)) < 0.1f,
             qPrintable(QStringLiteral(
                 "3sigma boundary jump=%1")
                            .arg(std::abs(
                                values.at(3) - values.at(4)))));
    QVERIFY(values.at(0) > values.at(2));
    QVERIFY(values.at(2) < original.at(2));
}

void AudioAnalysisTest::
neuralPeakHeatmapBlendsCorrelatedEllipseAtThreeSigma()
{
    constexpr double Correlation = 0.6;
    const auto diagonalCoordinate = [](double radius) {
        return radius
            / std::sqrt(2.0 / (1.0 + Correlation));
    };
    SpatialResponseGrid grid;
    grid.columns = 5;
    grid.rows = 1;
    for (double radius : {0.0, 2.0, 2.9, 3.0, 3.1}) {
        const double coordinate = diagonalCoordinate(radius);
        grid.directions.append({coordinate, coordinate});
    }
    QVector<float> values {
        1.0f, 0.8f, 0.8f, 0.8f, 0.8f,
    };
    const QVector<float> original = values;

    RobustSpatialEstimate estimate;
    estimate.hasDirection = true;
    estimate.direction = {0.0, 0.0};
    estimate.azimuthStandardDeviationDegrees = 1.0;
    estimate.elevationStandardDeviationDegrees = 1.0;
    estimate.azimuthElevationCorrelation = Correlation;
    estimate.neuralPeakApplied = true;
    estimate.neuralPeakConfidence = 1.0;

    sharpenNeuralSrpHeatmap(
        grid, estimate, 0.65, &values);

    QCOMPARE(values.at(3), original.at(3));
    QCOMPARE(values.at(4), original.at(4));
    QVERIFY(std::abs(values.at(2) - values.at(3)) < 0.1f);
    QVERIFY(values.at(1) < original.at(1));
}

void AudioAnalysisTest::
liveWorkerRebuildsLocatorForNeuralConfiguration()
{
    LiveLocalizationWorker worker;
    LiveLocalizationSettings settings;
    settings.updateIntervalMs = 20;
    settings.confidenceThreshold = 0.0;
    settings.neuralPeakEnhancementEnabled = false;
    worker.configure(settings);
    worker.setSpatialGrid(fieldOfViewGrid(32, 18), 21);
    worker.setActive(true);
    QSignalSpy spatial(
        &worker, &LiveLocalizationWorker::spatialResponseReady);
    const MultiChannelAudioWindow window =
        syntheticPlaneWave(23.37, -11.42);
    for (int frame = 0; frame <= 10; ++frame) {
        worker.pushAdjustedFrame(
            syntheticPayload(window, frame),
            0,
            static_cast<quint32>(frame * 4167));
    }
    QVERIFY(!spatial.isEmpty());
    const LiveSpatialResponse disabled =
        qvariant_cast<LiveSpatialResponse>(
            spatial.last().at(0));
    QVERIFY(!disabled.neuralPeakAttempted);

    spatial.clear();
    settings.neuralPeakEnhancementEnabled = true;
    settings.neuralPeakEnhancementStrength = 1.0;
    worker.configure(settings);
    for (int frame = 0; frame <= 10; ++frame) {
        const int timelineFrame = frame + 11;
        worker.pushAdjustedFrame(
            syntheticPayload(window, frame),
            0,
            static_cast<quint32>(
                timelineFrame * 4167));
    }
    QVERIFY(!spatial.isEmpty());
    const LiveSpatialResponse enabled =
        qvariant_cast<LiveSpatialResponse>(
            spatial.last().at(0));
    QVERIFY(enabled.neuralPeakAttempted);
    QVERIFY(enabled.neuralPeakApplied);
}

void AudioAnalysisTest::
neuralPeakPostprocessingStaysWithinCpuBudget()
{
    LocatorConfiguration configuration;
    configuration.microphones =
        createDefaultArrayGeometry();
    configuration.spatialResolutionMode =
        SpatialResolutionMode::HighPrecision;
    configuration.neuralPeakEnhancementEnabled =
        false;
    RobustSrpPhatScorer scorer;
    QVERIFY(scorer.configure(configuration));
    QVERIFY(scorer.prepareWindow(
        syntheticPlaneWave(23.37, -11.42)));
    RobustSpatialEstimate estimate =
        scorer.estimate();
    QVERIFY(estimate.hasDirection);
    estimate.neuralPeakApplied = true;
    estimate.neuralPeakConfidence = 0.9;

    const SpatialResponseGrid grid =
        fieldOfViewGrid(640, 360);
    QVector<float> originalValues(
        grid.directions.size(), 0.2f);
    QVector<qint64> measurements;
    const NeuralSrpPeakRefiner refiner;
    for (int iteration = 0;
         iteration < 7;
         ++iteration) {
        QVector<float> values = originalValues;
        QElapsedTimer elapsed;
        elapsed.start();
        const std::optional<SrpLocalPeakPatch> patch =
            scorer.sampleLocalPeakPatch(estimate);
        QVERIFY(patch.has_value());
        NeuralSrpPeakInput input;
        input.normalizedPatch =
            patch->normalizedValues;
        input.diagnostics = patch->diagnostics;
        QVERIFY(refiner.infer(input).valid);
        sharpenNeuralSrpHeatmap(
            grid, estimate, 0.65, &values);
        measurements.append(elapsed.nsecsElapsed());
    }
    std::sort(
        measurements.begin(),
        measurements.end());
    const double medianMilliseconds =
        measurements.at(measurements.size() / 2)
        / 1.0e6;
    qInfo().noquote()
        << "neural SRP postprocessing median:"
        << medianMilliseconds << "ms";
#ifdef NDEBUG
    constexpr double BudgetMilliseconds = 2.0;
#else
    constexpr double BudgetMilliseconds = 30.0;
#endif
    QVERIFY2(
        medianMilliseconds <= BudgetMilliseconds,
        qPrintable(QStringLiteral(
            "neural postprocessing took %1 ms, "
            "budget is %2 ms")
                       .arg(medianMilliseconds)
                       .arg(BudgetMilliseconds)));
}

void AudioAnalysisTest::ignoresBackHemisphereSpatialDirections()
{
    RobustSrpPhatScorer scorer;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(scorer.configure(configuration));
    SpatialResponseGrid grid;
    grid.columns = 2;
    grid.rows = 1;
    grid.directions = {
        {0.0, 0.0},
        {180.0, 0.0},
    };
    scorer.prepareGrid(grid);
    QVERIFY(scorer.prepareWindow(
        syntheticPlaneWave(0.0, 0.0)));
    const RobustSpatialEstimate result =
        scorer.estimate(&grid);

    QCOMPARE(result.gridValues.size(), 2);
    QVERIFY(result.gridValues.at(0)
            > result.gridValues.at(1));
    QCOMPARE(result.gridValues.at(1), 0.0f);
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

void AudioAnalysisTest::projectsDirectionsLinearlyAcrossWideAngleView()
{
    CameraViewSettings camera;
    camera.horizontalFovDegrees = 180.0;
    camera.verticalFovDegrees = 180.0;

    const ProjectionResult left =
        projectDirectionAngular({-90.0, 0.0}, camera);
    const ProjectionResult center =
        projectDirectionAngular({0.0, 0.0}, camera);
    const ProjectionResult middleRight =
        projectDirectionAngular({45.0, 0.0}, camera);
    const ProjectionResult right =
        projectDirectionAngular({90.0, 0.0}, camera);
    for (const ProjectionResult &result :
         {left, center, middleRight, right}) {
        QVERIFY(result.valid);
        QVERIFY(result.inFront);
        QVERIFY(result.inView);
    }
    QVERIFY(QLineF(left.normalizedPoint,
                   QPointF(0.0, 0.5)).length() < 1.0e-6);
    QVERIFY(QLineF(center.normalizedPoint,
                   QPointF(0.5, 0.5)).length() < 1.0e-6);
    QVERIFY(QLineF(middleRight.normalizedPoint,
                   QPointF(0.75, 0.5)).length() < 1.0e-6);
    QVERIFY(QLineF(right.normalizedPoint,
                   QPointF(1.0, 0.5)).length() < 1.0e-6);
}

void AudioAnalysisTest::
    roundTripsAngularProjectionWithOrientationAndRoll()
{
    CameraViewSettings camera;
    camera.centerAzimuthDegrees = 20.0;
    camera.centerElevationDegrees = -10.0;
    camera.horizontalFovDegrees = 180.0;
    camera.verticalFovDegrees = 120.0;
    camera.rollDegrees = 15.0;
    const QVector<SoundSourceDirection> sources {
        {20.0, -10.0},
        {35.0, 5.0},
        {-20.0, -25.0},
        {60.0, -5.0},
    };
    for (const SoundSourceDirection &source : sources) {
        const ProjectionResult projected =
            projectDirectionAngular(source, camera);
        QVERIFY(projected.valid);
        QVERIFY(projected.inFront);
        QVERIFY(projected.inView);
        const ScanDirection restored =
            unprojectVideoPointAngular(
                projected.normalizedPoint, camera);
        QVERIFY(std::abs(restored.azimuthDegrees
                         - source.azimuthDegrees)
                < 1.0e-5);
        QVERIFY(std::abs(restored.elevationDegrees
                         - source.elevationDegrees)
                < 1.0e-5);
    }
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

void AudioAnalysisTest::locatorAndHeatmapShareRefinedPeak()
{
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    const MultiChannelAudioWindow window =
        syntheticPlaneWave(23.37, -11.42);
    const SpatialResponseGrid grid = fieldOfViewGrid(32, 18);

    GccPhatLocator locator;
    QVERIFY(locator.configure(configuration));
    const SpatialResponseResult response =
        locator.evaluateSpatialResponse(window, grid);
    const QVector<SoundSourceDirection> directions =
        locator.locate(window);

    QCOMPARE(directions.size(), 1);
    QVERIFY(response.hasRefinedPeak);
    const ScanDirection located {
        directions.first().azimuthDegrees,
        directions.first().elevationDegrees,
    };
    QVERIFY(angularDistanceDegrees(
                located, response.refinedPeak) < 0.1);
    QVERIFY(angularDistanceDegrees(
                response.refinedPeak, {23.37, -11.42}) < 0.5);
    QVERIFY(response.azimuthStandardDeviationDegrees > 0.0);
    QVERIFY(response.elevationStandardDeviationDegrees > 0.0);
    QVERIFY(std::isfinite(
        response.azimuthElevationCorrelation));
    QVERIFY(std::abs(
        response.azimuthElevationCorrelation) < 1.0);
    QVERIFY(response.peakToSidelobeRatio > 0.0);
}

void AudioAnalysisTest::smoothedLocalizationKeepsSpatialPeakAligned()
{
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    configuration.smoothingFactor = 0.25;
    GccPhatLocator locator;
    QVERIFY(locator.configure(configuration));
    MultiChannelAudioWindow first =
        syntheticPlaneWave(-20.0, 0.0);
    MultiChannelAudioWindow second =
        syntheticPlaneWave(20.0, 0.0);
    second.centerSample = first.centerSample + 4800;
    QVERIFY(!locator.locate(first).isEmpty());
    const QVector<SoundSourceDirection> located =
        locator.locate(second);
    const SpatialResponseGrid grid =
        fieldOfViewGrid(64, 36);
    const SpatialResponseResult response =
        locator.evaluateSpatialResponse(second, grid);

    QCOMPARE(located.size(), 1);
    QVERIFY(response.hasRefinedPeak);
    const int peak = static_cast<int>(std::distance(
        response.values.cbegin(),
        std::max_element(response.values.cbegin(),
                         response.values.cend())));
    const ScanDirection gridPeak = grid.directions.at(peak);
    QVERIFY2(angularDistanceDegrees(
                 gridPeak, response.refinedPeak) < 3.0,
             qPrintable(QStringLiteral(
                 "grid peak=(%1,%2), refined=(%3,%4)")
                 .arg(gridPeak.azimuthDegrees)
                 .arg(gridPeak.elevationDegrees)
                 .arg(response.refinedPeak.azimuthDegrees)
                 .arg(response.refinedPeak.elevationDegrees)));
    QVERIFY(angularDistanceDegrees(
                response.refinedPeak,
                {located.first().azimuthDegrees,
                 located.first().elevationDegrees})
            < 0.1);
}

void AudioAnalysisTest::invalidWindowBreaksTemporalSmoothing()
{
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    configuration.smoothingFactor = 0.1;
    GccPhatLocator locator;
    QVERIFY(locator.configure(configuration));

    MultiChannelAudioWindow first =
        syntheticPlaneWave(-25.0, 0.0);
    MultiChannelAudioWindow invalid = first;
    invalid.sampleRate /= 2;
    MultiChannelAudioWindow second =
        syntheticPlaneWave(25.0, 0.0);
    second.centerSample =
        first.centerSample + configuration.sampleRate / 10;

    QVERIFY(!locator.locate(first).isEmpty());
    QVERIFY(locator.locate(invalid).isEmpty());
    const QVector<SoundSourceDirection> located =
        locator.locate(second);

    QCOMPARE(located.size(), 1);
    QVERIFY2(std::abs(
                 located.first().azimuthDegrees - 25.0) < 0.5,
             qPrintable(QStringLiteral(
                 "invalid window retained temporal history: %1 deg")
                 .arg(located.first().azimuthDegrees)));
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

void AudioAnalysisTest::
evaluatesHighPrecisionSpatialGridWithinCpuBudget()
{
    GccPhatLocator locator;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    configuration.spatialResolutionMode =
        SpatialResolutionMode::HighPrecision;
    QVERIFY(locator.configure(configuration));
    MultiChannelAudioWindow window =
        syntheticPlaneWave(20.0, 5.0);
    const SpatialResponseGrid grid =
        fieldOfViewGrid(640, 360);
    locator.prepareSpatialResponseGrid(grid);
    locator.evaluateSpatialResponse(window, grid);

    QVector<qint64> measurements;
    SpatialResponseResult response;
    for (int iteration = 0; iteration < 5; ++iteration) {
        ++window.centerSample;
        QElapsedTimer elapsed;
        elapsed.start();
        const QVector<SoundSourceDirection> directions =
            locator.locate(window);
        response =
            locator.evaluateSpatialResponse(window, grid);
        measurements.append(elapsed.elapsed());
        QVERIFY(!directions.isEmpty());
    }
    std::sort(measurements.begin(), measurements.end());
    const qint64 milliseconds =
        measurements.at(measurements.size() / 2);
    qInfo().noquote()
        << "640x360 high-precision SRP-PHAT median:"
        << milliseconds << "ms; samples:" << measurements;

    QCOMPARE(response.values.size(), 640 * 360);
    QVERIFY(std::all_of(
        response.values.cbegin(), response.values.cend(),
        [](float value) { return std::isfinite(value); }));
#ifdef NDEBUG
    constexpr qint64 BudgetMilliseconds = 200;
#else
    constexpr qint64 BudgetMilliseconds = 1500;
#endif
    QVERIFY2(milliseconds <= BudgetMilliseconds,
             qPrintable(QStringLiteral(
                 "640x360 high precision took %1 ms, budget is %2 ms")
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
    worker.setSpatialGrid(fieldOfViewGrid(16, 9), 7);
    worker.setActive(true);
    QSignalSpy located(&worker, &LiveLocalizationWorker::localizationReady);
    QSignalSpy spatial(
        &worker, &LiveLocalizationWorker::spatialResponseReady);
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
    QCOMPARE(result.generation, quint64(7));
    QCOMPARE(result.directions.size(), 1);
    QVERIFY(std::abs(result.directions.first().azimuthDegrees - 22.0) < 6.0);
    QVERIFY(std::abs(result.directions.first().elevationDegrees + 8.0) < 6.0);
    QCOMPARE(spatial.count(), 1);
    const LiveSpatialResponse response =
        qvariant_cast<LiveSpatialResponse>(
            spatial.first().at(0));
    QCOMPARE(response.generation, quint64(7));
    QVERIFY(response.hasRefinedPeak);
    QVERIFY(angularDistanceDegrees(
                response.refinedPeak,
                {result.directions.first().azimuthDegrees,
                 result.directions.first().elevationDegrees})
            < 0.1);
    QVERIFY(response.azimuthStandardDeviationDegrees > 0.0);
    QVERIFY(response.elevationStandardDeviationDegrees > 0.0);
    QVERIFY(std::isfinite(
        response.azimuthElevationCorrelation));
    QVERIFY(std::abs(
        response.azimuthElevationCorrelation) < 1.0);
    QVERIFY(std::all_of(
        response.normalizedValues.cbegin(),
        response.normalizedValues.cend(),
        [](float value) {
            return std::isfinite(value)
                && value >= 0.0f && value <= 1.0f;
        }));
}

void AudioAnalysisTest::
liveWorkerSpatialGridMatchesRefinedPeakAfterDirectionChange()
{
    LiveLocalizationWorker worker;
    LiveLocalizationSettings settings;
    settings.updateIntervalMs = 20;
    settings.smoothingFactor = 0.35;
    worker.configure(settings);
    const SpatialResponseGrid grid =
        fieldOfViewGrid(64, 36);
    worker.setSpatialGrid(grid, 11);
    worker.setActive(true);
    QSignalSpy spatial(
        &worker, &LiveLocalizationWorker::spatialResponseReady);
    const MultiChannelAudioWindow first =
        syntheticPlaneWave(-45.0, 0.0);
    const MultiChannelAudioWindow second =
        syntheticPlaneWave(45.0, 0.0);

    for (int frame = 0; frame <= 10; ++frame) {
        worker.pushAdjustedFrame(
            syntheticPayload(first, frame),
            0,
            static_cast<quint32>(frame * 4167));
    }
    spatial.clear();
    for (int frame = 0; frame <= 10; ++frame) {
        const int timelineFrame = frame + 11;
        worker.pushAdjustedFrame(
            syntheticPayload(second, frame),
            0,
            static_cast<quint32>(
                timelineFrame * 4167));
    }

    QVERIFY(!spatial.isEmpty());
    for (const QList<QVariant> &arguments :
         std::as_const(spatial)) {
        const LiveSpatialResponse response =
            qvariant_cast<LiveSpatialResponse>(
                arguments.at(0));
        if (!response.hasRefinedPeak) {
            continue;
        }
        QCOMPARE(response.normalizedValues.size(),
                 grid.directions.size());
        const int peak = static_cast<int>(std::distance(
            response.normalizedValues.cbegin(),
            std::max_element(
                response.normalizedValues.cbegin(),
                response.normalizedValues.cend())));
        const ScanDirection gridPeak =
            grid.directions.at(peak);
        QVERIFY2(angularDistanceDegrees(
                     gridPeak,
                     response.refinedPeak) < 3.0,
                 qPrintable(QStringLiteral(
                     "worker grid peak=(%1,%2), refined=(%3,%4)")
                     .arg(gridPeak.azimuthDegrees)
                     .arg(gridPeak.elevationDegrees)
                     .arg(response.refinedPeak.azimuthDegrees)
                     .arg(response.refinedPeak.elevationDegrees)));
    }
}

void AudioAnalysisTest::
liveWorkerClearsSpatialResponseWhenSourceDisappears()
{
    LiveLocalizationWorker worker;
    LiveLocalizationSettings settings;
    settings.updateIntervalMs = 20;
    worker.configure(settings);
    worker.setSpatialGrid(fieldOfViewGrid(16, 9), 7);
    worker.setActive(true);
    QSignalSpy spatial(
        &worker, &LiveLocalizationWorker::spatialResponseReady);
    const MultiChannelAudioWindow window =
        syntheticPlaneWave(22.0, -8.0);
    for (int frame = 0; frame <= 10; ++frame) {
        worker.pushAdjustedFrame(
            syntheticPayload(window, frame),
            0,
            static_cast<quint32>(frame * 4167));
    }
    QVERIFY(!spatial.isEmpty());
    QVERIFY(!qvariant_cast<LiveSpatialResponse>(
                 spatial.last().at(0)).normalizedValues.isEmpty());

    const QByteArray silence(
        AudioFrameDecoder::FramePayloadBytes, char(0));
    for (int frame = 11; frame <= 30; ++frame) {
        worker.pushAdjustedFrame(
            silence,
            0,
            static_cast<quint32>(frame * 4167));
    }

    QVERIFY(spatial.size() >= 2);
    const LiveSpatialResponse cleared =
        qvariant_cast<LiveSpatialResponse>(
            spatial.last().at(0));
    QCOMPARE(cleared.generation, quint64(7));
    QVERIFY(cleared.normalizedValues.isEmpty());
    QVERIFY(!cleared.hasRefinedPeak);
}

void AudioAnalysisTest::
liveWorkerMarksMarginalSpatialPeakUntrusted()
{
    LiveLocalizationWorker worker;
    LiveLocalizationSettings settings;
    settings.updateIntervalMs = 20;
    settings.confidenceThreshold = 0.30;
    worker.configure(settings);
    worker.setSpatialGrid(fieldOfViewGrid(16, 9), 31);
    worker.setActive(true);

    QSignalSpy located(
        &worker, &LiveLocalizationWorker::localizationReady);
    QSignalSpy spatial(
        &worker, &LiveLocalizationWorker::spatialResponseReady);
    QSignalSpy status(
        &worker, &LiveLocalizationWorker::statusChanged);
    const MultiChannelAudioWindow window =
        syntheticPlaneWaveWithNoiseAndReflection(
            23.37, -11.42);
    for (int frame = 0; frame <= 10; ++frame) {
        worker.pushAdjustedFrame(
            syntheticPayload(window, frame),
            0,
            static_cast<quint32>(frame * 4167));
    }

    QVERIFY(!located.isEmpty());
    QVERIFY(!qvariant_cast<LiveLocalizationResult>(
                 located.last().at(0)).directions.isEmpty());
    QVERIFY(!spatial.isEmpty());
    QVERIFY(qvariant_cast<LiveSpatialResponse>(
                spatial.last().at(0)).trustedDirection);

    settings.confidenceThreshold = 1.0;
    worker.configure(settings);
    located.clear();
    spatial.clear();
    status.clear();
    for (int frame = 11; frame <= 21; ++frame) {
        worker.pushAdjustedFrame(
            syntheticPayload(window, frame),
            0,
            static_cast<quint32>(frame * 4167));
    }

    QVERIFY(!located.isEmpty());
    QVERIFY(qvariant_cast<LiveLocalizationResult>(
                located.last().at(0)).directions.isEmpty());
    QVERIFY(!spatial.isEmpty());
    const LiveSpatialResponse response =
        qvariant_cast<LiveSpatialResponse>(
            spatial.last().at(0));
    QVERIFY(!response.normalizedValues.isEmpty());
    QVERIFY(!response.trustedDirection);
    QVERIFY(response.reliability > 0.0);
    QVERIFY(response.reliability < settings.confidenceThreshold);
    QVERIFY(!status.isEmpty());
    const QString finalStatus = status.last().at(0).toString();
    QVERIFY2(finalStatus.contains(QStringLiteral("候选可靠度")),
             qPrintable(finalStatus));
    QVERIFY2(finalStatus.contains(QStringLiteral("门限")),
             qPrintable(finalStatus));
}

void AudioAnalysisTest::liveWorkerRecoversFromTimestampRollback()
{
    LiveLocalizationWorker worker;
    LiveLocalizationSettings settings;
    settings.updateIntervalMs = 20;
    worker.configure(settings);
    worker.setActive(true);
    QSignalSpy located(
        &worker, &LiveLocalizationWorker::localizationReady);
    const MultiChannelAudioWindow window =
        syntheticPlaneWave(12.0, 4.0);

    for (int frame = 0; frame <= 10; ++frame) {
        const qint64 timestamp =
            10'000'000 + frame * 4167;
        worker.pushAdjustedFrame(
            syntheticPayload(window, frame),
            static_cast<quint32>(timestamp / 1'000'000),
            static_cast<quint32>(timestamp % 1'000'000));
    }
    QVERIFY(!located.isEmpty());
    const int countBeforeRollback = located.size();

    for (int frame = 0; frame <= 20; ++frame) {
        worker.pushAdjustedFrame(
            syntheticPayload(window, frame),
            0,
            static_cast<quint32>(frame * 4167));
    }

    QVERIFY(located.size() > countBeforeRollback);
    const LiveLocalizationResult recovered =
        qvariant_cast<LiveLocalizationResult>(
            located.last().at(0));
    QVERIFY(!recovered.directions.isEmpty());
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
