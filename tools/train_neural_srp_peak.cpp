#include "analysis/arraygeometry.h"
#include "analysis/neuralmodeldata.h"
#include "analysis/neuralsrppeakrefiner.h"
#include "analysis/robustsrpphatscorer.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMap>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>
#include <random>

using namespace MicArray;

namespace {

constexpr int InputCount = 93;
constexpr int Hidden1Count = 48;
constexpr int Hidden2Count = 24;
constexpr int OutputCount = 6;
constexpr int W1Offset = 0;
constexpr int B1Offset =
    W1Offset + InputCount * Hidden1Count;
constexpr int W2Offset = B1Offset + Hidden1Count;
constexpr int B2Offset =
    W2Offset + Hidden1Count * Hidden2Count;
constexpr int W3Offset = B2Offset + Hidden2Count;
constexpr int B3Offset =
    W3Offset + Hidden2Count * OutputCount;
constexpr int ParameterCount = B3Offset + OutputCount;
constexpr int SampleRate = 48000;
constexpr int WindowSamples = 2048;
constexpr double SoundSpeed = 343.0;
constexpr std::uint64_t FnvOffset =
    UINT64_C(14695981039346656037);
constexpr std::uint64_t FnvPrime =
    UINT64_C(1099511628211);

static_assert(ParameterCount == 5838);

struct Example
{
    std::array<float, InputCount> input {};
    std::array<float, OutputCount> target {};
    double trueAzimuthDegrees = 0.0;
    double trueElevationDegrees = 0.0;
    double physicalAzimuthDegrees = 0.0;
    double physicalElevationDegrees = 0.0;
    double azimuthStepDegrees = 0.0;
    double elevationStepDegrees = 0.0;
    double azimuthSigmaDegrees = 0.0;
    double elevationSigmaDegrees = 0.0;
    double correlation = 0.0;
    double reliability = 0.0;
    double gridArgmaxErrorDegrees = 0.0;
    double physicalErrorDegrees = 0.0;
    int corruptChannelCount = 0;
    int signalKind = 0;
    double reflectionGain = 0.0;
    bool fieldEdge = false;
    std::optional<MultiChannelAudioWindow>
        evaluationPreviousWindow;
    std::optional<MultiChannelAudioWindow>
        evaluationWindow;

    bool operator==(const Example &) const = default;
};

struct Model
{
    std::array<double, ParameterCount> parameters {};
    std::array<float, InputCount> inputMeans {};
    std::array<float, InputCount> inputScales {};
};

struct ForwardCache
{
    std::array<double, InputCount> input {};
    std::array<double, Hidden1Count> hidden1 {};
    std::array<double, Hidden2Count> hidden2 {};
    std::array<double, OutputCount> raw {};
    std::array<double, OutputCount> decoded {};
};

Model embeddedModel()
{
    Model model;
    int offset = 0;
    const auto appendParameters =
        [&](const auto &values) {
        for (float value : values) {
            model.parameters[offset++] = value;
        }
    };
    appendParameters(
        NeuralModelData::InputWeights);
    appendParameters(
        NeuralModelData::Hidden1Bias);
    appendParameters(
        NeuralModelData::Hidden1Weights);
    appendParameters(
        NeuralModelData::Hidden2Bias);
    appendParameters(
        NeuralModelData::Hidden2Weights);
    appendParameters(
        NeuralModelData::OutputBias);
    model.inputMeans =
        NeuralModelData::InputMeans;
    model.inputScales =
        NeuralModelData::InputScales;
    return model;
}

Model quantizedModel(Model model)
{
    for (double &parameter : model.parameters) {
        parameter = static_cast<float>(parameter);
    }
    return model;
}

struct Metrics
{
    double gridMedianDegrees = 0.0;
    double physicalMedianDegrees = 0.0;
    double neuralMedianDegrees = 0.0;
    double physicalP95Degrees = 0.0;
    double neuralP95Degrees = 0.0;
    int neuralAppliedSamples = 0;
};

struct TrainingOptions
{
    std::uint64_t seed = 20260731;
    int trainSamples = 4096;
    int validationSamples = 512;
    int testSamples = 512;
    int maximumEpochs = 100;
    QString outputHeader =
        QStringLiteral("src/analysis/neuralmodeldata.h");
    QString outputReport =
        QStringLiteral(
            "doc/neural-srp-peak-v1-training-report.json");
};

double uniform(std::mt19937_64 *random,
               double minimum,
               double maximum)
{
    return std::uniform_real_distribution<double>(
        minimum, maximum)(*random);
}

int uniformInt(std::mt19937_64 *random,
               int minimum,
               int maximum)
{
    return std::uniform_int_distribution<int>(
        minimum, maximum)(*random);
}

double angularDistanceDegrees(double firstAzimuth,
                              double firstElevation,
                              double secondAzimuth,
                              double secondElevation)
{
    const QVector3D first = directionFromAngles(
        firstAzimuth, firstElevation);
    const QVector3D second = directionFromAngles(
        secondAzimuth, secondElevation);
    return qRadiansToDegrees(std::acos(std::clamp(
        static_cast<double>(
            QVector3D::dotProduct(first, second)),
        -1.0, 1.0)));
}

double percentile(QVector<double> values,
                  double fraction)
{
    if (values.isEmpty()) {
        return std::numeric_limits<double>::infinity();
    }
    const qsizetype index = std::clamp<qsizetype>(
        static_cast<qsizetype>(std::floor(
            fraction * (values.size() - 1))),
        0, values.size() - 1);
    std::nth_element(
        values.begin(), values.begin() + index,
        values.end());
    return values.at(index);
}

double sigmoid(double value)
{
    if (value >= 0.0) {
        const double exponential = std::exp(-value);
        return 1.0 / (1.0 + exponential);
    }
    const double exponential = std::exp(value);
    return exponential / (1.0 + exponential);
}

ForwardCache forward(const Model &model,
                     const Example &example)
{
    ForwardCache cache;
    for (int index = 0; index < InputCount; ++index) {
        cache.input[index] = std::clamp(
            (example.input.at(index)
             - model.inputMeans.at(index))
                / model.inputScales.at(index),
            -8.0F, 8.0F);
    }
    for (int target = 0;
         target < Hidden1Count;
         ++target) {
        double sum =
            model.parameters.at(B1Offset + target);
        for (int source = 0;
             source < InputCount;
             ++source) {
            sum += cache.input.at(source)
                * model.parameters.at(
                    W1Offset
                    + target * InputCount
                    + source);
        }
        cache.hidden1[target] = std::tanh(sum);
    }
    for (int target = 0;
         target < Hidden2Count;
         ++target) {
        double sum =
            model.parameters.at(B2Offset + target);
        for (int source = 0;
             source < Hidden1Count;
             ++source) {
            sum += cache.hidden1.at(source)
                * model.parameters.at(
                    W2Offset
                    + target * Hidden1Count
                    + source);
        }
        cache.hidden2[target] = std::tanh(sum);
    }
    for (int target = 0;
         target < OutputCount;
         ++target) {
        double sum =
            model.parameters.at(B3Offset + target);
        for (int source = 0;
             source < Hidden2Count;
             ++source) {
            sum += cache.hidden2.at(source)
                * model.parameters.at(
                    W3Offset
                    + target * Hidden2Count
                    + source);
        }
        cache.raw[target] = sum;
    }
    cache.decoded[0] = std::tanh(cache.raw.at(0));
    cache.decoded[1] = std::tanh(cache.raw.at(1));
    cache.decoded[2] =
        0.35 + 0.65 * sigmoid(cache.raw.at(2));
    cache.decoded[3] =
        0.35 + 0.65 * sigmoid(cache.raw.at(3));
    cache.decoded[4] =
        0.35 * std::tanh(cache.raw.at(4));
    cache.decoded[5] = sigmoid(cache.raw.at(5));
    return cache;
}

double huberGradient(double residual,
                     double delta)
{
    return std::abs(residual) <= delta
        ? residual
        : std::copysign(delta, residual);
}

void accumulateGradient(
    const Model &model,
    const Example &example,
    std::array<double, ParameterCount> *gradient)
{
    const ForwardCache cache =
        forward(model, example);
    std::array<double, OutputCount> outputGradient {};
    outputGradient[0] =
        huberGradient(
            cache.decoded.at(0)
                - example.target.at(0),
            0.15)
        * (1.0
           - cache.decoded.at(0)
               * cache.decoded.at(0));
    outputGradient[1] =
        huberGradient(
            cache.decoded.at(1)
                - example.target.at(1),
            0.15)
        * (1.0
           - cache.decoded.at(1)
               * cache.decoded.at(1));
    for (int index = 2; index <= 3; ++index) {
        const double probability =
            sigmoid(cache.raw.at(index));
        outputGradient[index] =
            0.1
            * huberGradient(
                cache.decoded.at(index)
                    - example.target.at(index),
                0.1)
            * 0.65 * probability
            * (1.0 - probability);
    }
    const double correlationTanh =
        std::tanh(cache.raw.at(4));
    outputGradient[4] =
        0.05
        * huberGradient(
            cache.decoded.at(4)
                - example.target.at(4),
            0.1)
        * 0.35
        * (1.0
           - correlationTanh
               * correlationTanh);
    outputGradient[5] =
        0.2
        * (cache.decoded.at(5)
           - example.target.at(5));

    std::array<double, Hidden2Count>
        hidden2Gradient {};
    for (int target = 0;
         target < OutputCount;
         ++target) {
        (*gradient)[B3Offset + target] +=
            outputGradient.at(target);
        for (int source = 0;
             source < Hidden2Count;
             ++source) {
            (*gradient)[
                W3Offset
                + target * Hidden2Count
                + source] +=
                outputGradient.at(target)
                * cache.hidden2.at(source);
            hidden2Gradient[source] +=
                outputGradient.at(target)
                * model.parameters.at(
                    W3Offset
                    + target * Hidden2Count
                    + source);
        }
    }

    std::array<double, Hidden1Count>
        hidden1Gradient {};
    for (int target = 0;
         target < Hidden2Count;
         ++target) {
        const double activationGradient =
            hidden2Gradient.at(target)
            * (1.0
               - cache.hidden2.at(target)
                   * cache.hidden2.at(target));
        (*gradient)[B2Offset + target] +=
            activationGradient;
        for (int source = 0;
             source < Hidden1Count;
             ++source) {
            (*gradient)[
                W2Offset
                + target * Hidden1Count
                + source] +=
                activationGradient
                * cache.hidden1.at(source);
            hidden1Gradient[source] +=
                activationGradient
                * model.parameters.at(
                    W2Offset
                    + target * Hidden1Count
                    + source);
        }
    }

    for (int target = 0;
         target < Hidden1Count;
         ++target) {
        const double activationGradient =
            hidden1Gradient.at(target)
            * (1.0
               - cache.hidden1.at(target)
                   * cache.hidden1.at(target));
        (*gradient)[B1Offset + target] +=
            activationGradient;
        for (int source = 0;
             source < InputCount;
             ++source) {
            (*gradient)[
                W1Offset
                + target * InputCount
                + source] +=
                activationGradient
                * cache.input.at(source);
        }
    }
}

MultiChannelAudioWindow syntheticWindow(
    std::mt19937_64 *random,
    double azimuthDegrees,
    double elevationDegrees,
    int corruptChannelCount,
    int signalKind,
    qint64 centerSample,
    double *reflectionGainOutput = nullptr)
{
    const QVector<MicrophonePosition> microphones =
        createDefaultArrayGeometry();
    const QVector3D direction = directionFromAngles(
        azimuthDegrees, elevationDegrees);
    const double reflectionAzimuth =
        std::clamp(
            azimuthDegrees
                + uniform(random, -55.0, 55.0),
            -80.0, 80.0);
    const double reflectionElevation =
        std::clamp(
            elevationDegrees
                + uniform(random, -40.0, 40.0),
            -80.0, 80.0);
    const QVector3D reflectionDirection =
        directionFromAngles(
            reflectionAzimuth, reflectionElevation);
    const double maximumReflectionGain =
        corruptChannelCount == 0 ? 0.25
        : corruptChannelCount == 1 ? 0.45
        : corruptChannelCount == 2 ? 0.65
                                   : 0.75;
    const double reflectionGain =
        uniform(
            random, 0.0,
            maximumReflectionGain);
    if (reflectionGainOutput != nullptr) {
        *reflectionGainOutput = reflectionGain;
    }
    const double reflectionDelay =
        uniform(random, 0.002, 0.025)
        * SampleRate;
    const double minimumSnrDb =
        corruptChannelCount == 0 ? 15.0
        : corruptChannelCount == 1 ? 5.0
        : corruptChannelCount == 2 ? 0.0
                                   : -5.0;
    const double maximumSnrDb =
        corruptChannelCount == 0 ? 35.0
                                 : 30.0;
    const double snrDb =
        uniform(
            random,
            minimumSnrDb,
            maximumSnrDb);
    const double noiseScale =
        std::pow(10.0, -snrDb / 20.0);
    const int toneCount = signalKind == 1
        ? 24
        : signalKind == 2
            ? uniformInt(random, 8, 16)
            : uniformInt(random, 6, 14);
    const double minimumFrequency =
        signalKind == 1
        ? 180.0
        : signalKind == 2
            ? uniform(random, 300.0, 1800.0)
            : uniform(random, 250.0, 1200.0);
    const double maximumFrequency =
        signalKind == 1
        ? 10'000.0
        : signalKind == 2
            ? std::min(
                  minimumFrequency
                      + uniform(
                          random, 700.0, 2200.0),
                  7'500.0)
            : uniform(random, 2600.0, 8000.0);
    QVector<double> frequencies;
    QVector<double> phases;
    QVector<double> amplitudes;
    frequencies.reserve(toneCount);
    phases.reserve(toneCount);
    amplitudes.reserve(toneCount);
    for (int tone = 0; tone < toneCount; ++tone) {
        frequencies.append(uniform(
            random, minimumFrequency,
            maximumFrequency));
        phases.append(uniform(
            random, 0.0,
            2.0 * std::numbers::pi));
        amplitudes.append(uniform(
            random, 0.5, 1.0));
    }

    QVector<int> corruptChannels(
        microphones.size());
    std::iota(
        corruptChannels.begin(),
        corruptChannels.end(), 0);
    std::shuffle(
        corruptChannels.begin(),
        corruptChannels.end(), *random);
    corruptChannels.resize(corruptChannelCount);
    std::sort(
        corruptChannels.begin(),
        corruptChannels.end());

    std::normal_distribution<double>
        normal(0.0, 1.0);
    MultiChannelAudioWindow window;
    window.sampleRate = SampleRate;
    window.centerSample = centerSample;
    window.channels.resize(microphones.size());
    const double inverseToneScale =
        0.55 / std::sqrt(
            static_cast<double>(toneCount));
    for (int channel = 0;
         channel < microphones.size();
         ++channel) {
        const MicrophonePosition &microphone =
            microphones.at(channel);
        const double directDelay = (
            microphone.xMeters * direction.x()
            + microphone.yMeters * direction.y())
            * SampleRate / SoundSpeed;
        const double reflectedSpatialDelay = (
            microphone.xMeters
                * reflectionDirection.x()
            + microphone.yMeters
                * reflectionDirection.y())
            * SampleRate / SoundSpeed;
        const double gain = std::pow(
            10.0,
            uniform(random, -3.0, 3.0) / 20.0);
        const bool corrupt =
            std::binary_search(
                corruptChannels.cbegin(),
                corruptChannels.cend(),
                channel);
        QVector<float> &samples =
            window.channels[channel];
        samples.resize(WindowSamples);
        for (int sample = 0;
             sample < WindowSamples;
             ++sample) {
            double direct = 0.0;
            double reflected = 0.0;
            for (int tone = 0;
                 tone < toneCount;
                 ++tone) {
                const double radiansPerSample =
                    2.0 * std::numbers::pi
                    * frequencies.at(tone)
                    / SampleRate;
                direct += amplitudes.at(tone)
                    * std::sin(
                        radiansPerSample
                            * (sample - directDelay)
                        + phases.at(tone));
                reflected += amplitudes.at(tone)
                    * std::sin(
                        radiansPerSample
                            * (sample
                               - reflectionDelay
                               - reflectedSpatialDelay)
                        + phases.at(tone));
            }
            const double signal =
                inverseToneScale
                * (direct
                   + reflectionGain * reflected);
            const double value = corrupt
                ? 0.35 * normal(*random)
                : gain * signal
                    + 0.2 * noiseScale
                        * normal(*random);
            samples[sample] =
                static_cast<float>(
                    std::clamp(
                        value, -1.0, 1.0));
        }
    }
    return window;
}

QVector<Example> makeDatasetShard(
    int sampleCount,
    int sampleOffset,
    std::uint64_t seed,
    bool retainEvaluationWindows,
    QString *errorMessage)
{
    std::mt19937_64 random(seed);
    LocatorConfiguration configuration;
    configuration.microphones =
        createDefaultArrayGeometry();
    configuration.sampleRate = SampleRate;
    configuration.windowSamples = WindowSamples;
    configuration.soundSpeedMetersPerSecond =
        SoundSpeed;
    configuration.confidenceThreshold = 0.0;
    configuration.smoothingFactor = 0.35;
    configuration.spatialResolutionMode =
        SpatialResolutionMode::HighPrecision;
    configuration.neuralPeakEnhancementEnabled =
        false;
    RobustSrpPhatScorer scorer;
    if (!scorer.configure(
            configuration, errorMessage)) {
        return {};
    }

    QVector<Example> examples;
    examples.reserve(sampleCount);
    const std::array<int, 10> corruptCounts {
        0, 0, 0, 0, 0,
        1, 1, 2, 2, 4,
    };
    const int maximumAttempts =
        std::max(sampleCount * 8, 64);
    for (int attempt = 0;
         attempt < maximumAttempts
         && examples.size() < sampleCount;
         ++attempt) {
        const double azimuth =
            uniform(&random, -75.0, 75.0);
        const double elevation =
            uniform(&random, -70.0, 70.0);
        const int globalSampleIndex =
            sampleOffset + examples.size();
        const int stratumIndex =
            globalSampleIndex
            % corruptCounts.size();
        const int corruptCount =
            corruptCounts.at(
                stratumIndex);
        const std::array<int, 10> signalKinds {
            0, 0, 0, 0, 0,
            0, 0, 0, 1, 2,
        };
        const int signalKind =
            signalKinds.at(
                (stratumIndex * 7
                 + (globalSampleIndex / 10) * 3
                 + 3)
                % signalKinds.size());
        double reflectionGain = 0.0;
        MultiChannelAudioWindow window =
            syntheticWindow(
                &random, azimuth, elevation,
                corruptCount, signalKind,
                static_cast<qint64>(attempt + 1)
                    * SampleRate * 2,
                &reflectionGain);
        std::mt19937_64 previousRandom(
            seed
            ^ (UINT64_C(0xd1b54a32d192ed03)
               * static_cast<std::uint64_t>(
                   globalSampleIndex + 1)));
        std::normal_distribution<double>
            previousNoise(0.0, 0.005);
        MultiChannelAudioWindow previousWindow =
            window;
        previousWindow.centerSample -=
            WindowSamples;
        for (QVector<float> &channel :
             previousWindow.channels) {
            for (float &sample : channel) {
                sample = static_cast<float>(
                    std::clamp(
                        sample
                            + previousNoise(
                                previousRandom),
                        -1.0, 1.0));
            }
        }
        if (!scorer.prepareWindow(
                previousWindow)) {
            continue;
        }
        if (!scorer.prepareWindow(window)) {
            continue;
        }
        const RobustSpatialEstimate physical =
            scorer.estimate();
        const std::optional<SrpLocalPeakPatch> patch =
            scorer.sampleLocalPeakPatch(physical);
        if (!physical.hasDirection
            || !patch.has_value()
            || patch->normalizedValues.size()
                != NeuralSrpPeakInput
                    ::PatchValueCount) {
            continue;
        }

        Example example;
        for (int index = 0;
             index < NeuralSrpPeakInput
                         ::PatchValueCount;
             ++index) {
            example.input[index] =
                patch->normalizedValues.at(index);
        }
        for (int index = 0;
             index < patch->diagnostics.size();
             ++index) {
            example.input[
                NeuralSrpPeakInput
                    ::PatchValueCount
                + index] =
                patch->diagnostics.at(index);
        }
        example.trueAzimuthDegrees = azimuth;
        example.trueElevationDegrees = elevation;
        example.physicalAzimuthDegrees =
            physical.direction.azimuthDegrees;
        example.physicalElevationDegrees =
            physical.direction.elevationDegrees;
        example.azimuthStepDegrees =
            patch->azimuthStepDegrees;
        example.elevationStepDegrees =
            patch->elevationStepDegrees;
        example.azimuthSigmaDegrees =
            physical
                .azimuthStandardDeviationDegrees;
        example.elevationSigmaDegrees =
            physical
                .elevationStandardDeviationDegrees;
        example.correlation =
            physical.azimuthElevationCorrelation;
        example.reliability =
            physical.reliability;
        example.corruptChannelCount =
            corruptCount;
        example.signalKind = signalKind;
        example.reflectionGain =
            reflectionGain;
        example.fieldEdge =
            std::abs(azimuth) >= 60.0
            || std::abs(elevation) >= 55.0;
        if (retainEvaluationWindows) {
            example.evaluationPreviousWindow =
                std::move(previousWindow);
            example.evaluationWindow = window;
        }
        const double azimuthCells =
            (azimuth
             - example.physicalAzimuthDegrees)
            / example.azimuthStepDegrees;
        const double elevationCells =
            (elevation
             - example.physicalElevationDegrees)
            / example.elevationStepDegrees;
        const double correctionDegrees =
            angularDistanceDegrees(
                azimuth, elevation,
                example.physicalAzimuthDegrees,
                example.physicalElevationDegrees);
        const bool learnable =
            correctionDegrees <= 1.5;
        example.target = {
            static_cast<float>(std::clamp(
                azimuthCells,
                -1.0, 1.0)),
            static_cast<float>(std::clamp(
                elevationCells,
                -1.0, 1.0)),
            0.5f,
            0.5f,
            static_cast<float>(std::clamp(
                -physical
                    .azimuthElevationCorrelation,
                -0.35, 0.35)),
            learnable ? 1.0f : 0.0f,
        };
        example.physicalErrorDegrees =
            correctionDegrees;
        const auto maximum = std::max_element(
            patch->normalizedValues.cbegin(),
            patch->normalizedValues.cend());
        const int peakIndex =
            static_cast<int>(std::distance(
                patch->normalizedValues.cbegin(),
                maximum));
        const int peakRow =
            peakIndex
            / NeuralSrpPeakInput::PatchSize;
        const int peakColumn =
            peakIndex
            % NeuralSrpPeakInput::PatchSize;
        const double gridAzimuth =
            example.physicalAzimuthDegrees
            + (peakColumn - 4)
                * example.azimuthStepDegrees;
        const double gridElevation =
            example.physicalElevationDegrees
            + (4 - peakRow)
                * example.elevationStepDegrees;
        example.gridArgmaxErrorDegrees =
            angularDistanceDegrees(
                azimuth, elevation,
                gridAzimuth, gridElevation);
        examples.append(example);
    }
    if (examples.size() != sampleCount
        && errorMessage != nullptr) {
        *errorMessage = QStringLiteral(
            "generated only %1/%2 valid examples")
            .arg(examples.size())
            .arg(sampleCount);
    }
    return examples;
}

struct DatasetShardRequest
{
    int sampleCount = 0;
    int sampleOffset = 0;
    std::uint64_t seed = 0;
    bool retainEvaluationWindows = false;
};

struct DatasetShardResult
{
    QVector<Example> examples;
    QString errorMessage;
};

QVector<Example> makeDataset(
    int sampleCount,
    std::uint64_t seed,
    QString *errorMessage,
    bool retainEvaluationWindows = false)
{
    constexpr int DeterministicShardCount = 8;
    const int shardCount = std::clamp(
        std::min(
            sampleCount,
            DeterministicShardCount),
        1, DeterministicShardCount);
    QVector<DatasetShardRequest> requests;
    requests.reserve(shardCount);
    const int baseCount = sampleCount / shardCount;
    const int remainder = sampleCount % shardCount;
    int sampleOffset = 0;
    for (int shard = 0;
         shard < shardCount;
         ++shard) {
        requests.append({
            baseCount + (shard < remainder ? 1 : 0),
            sampleOffset,
            seed
                ^ (UINT64_C(0x9e3779b97f4a7c15)
                   * static_cast<std::uint64_t>(
                       shard + 1)),
            retainEvaluationWindows,
        });
        sampleOffset += requests.last().sampleCount;
    }

    const QVector<DatasetShardResult> results =
        QtConcurrent::blockingMapped(
            requests,
            [](const DatasetShardRequest &request) {
        DatasetShardResult result;
        result.examples = makeDatasetShard(
            request.sampleCount,
            request.sampleOffset,
            request.seed,
            request.retainEvaluationWindows,
            &result.errorMessage);
        return result;
    });
    QVector<Example> examples;
    examples.reserve(sampleCount);
    for (const DatasetShardResult &result : results) {
        if (result.examples.isEmpty()
            && sampleCount > 0) {
            if (errorMessage != nullptr) {
                *errorMessage =
                    result.errorMessage;
            }
            return {};
        }
        examples.append(result.examples);
    }
    if (examples.size() != sampleCount) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "generated only %1/%2 valid examples")
                .arg(examples.size())
                .arg(sampleCount);
        }
        return {};
    }
    return examples;
}

void initializeModel(Model *model,
                     const QVector<Example> &training,
                     std::uint64_t seed)
{
    for (int input = 0;
         input < InputCount;
         ++input) {
        double sum = 0.0;
        for (const Example &example :
             training) {
            sum += example.input.at(input);
        }
        const double mean =
            sum / training.size();
        double squared = 0.0;
        for (const Example &example :
             training) {
            const double difference =
                example.input.at(input) - mean;
            squared += difference * difference;
        }
        const double scale = std::sqrt(
            squared / training.size());
        model->inputMeans[input] =
            static_cast<float>(mean);
        model->inputScales[input] =
            static_cast<float>(
                std::max(scale, 1.0e-4));
    }

    std::mt19937_64 random(seed);
    const auto initializeWeights =
        [&random, model](int offset,
                         int inputCount,
                         int outputCount) {
        const double limit = std::sqrt(
            6.0 / (inputCount + outputCount));
        for (int index = 0;
             index < inputCount * outputCount;
             ++index) {
            model->parameters[offset + index] =
                uniform(&random, -limit, limit);
        }
    };
    initializeWeights(
        W1Offset, InputCount, Hidden1Count);
    initializeWeights(
        W2Offset, Hidden1Count, Hidden2Count);
    initializeWeights(
        W3Offset, Hidden2Count, OutputCount);
    model->parameters[B3Offset + 2] = -1.2;
    model->parameters[B3Offset + 3] = -1.2;
    model->parameters[B3Offset + 5] = 1.0;
}

Metrics evaluate(const Model &model,
                 const QVector<Example> &examples)
{
    QVector<double> gridErrors;
    QVector<double> physicalErrors;
    QVector<double> neuralErrors;
    gridErrors.reserve(examples.size());
    physicalErrors.reserve(examples.size());
    neuralErrors.reserve(examples.size());
    int applied = 0;
    for (const Example &example : examples) {
        const ForwardCache prediction =
            forward(model, example);
        const double deltaAzimuth =
            prediction.decoded.at(0)
            * example.azimuthStepDegrees;
        const double deltaElevation =
            prediction.decoded.at(1)
            * example.elevationStepDegrees;
        const double correctionDegrees =
            std::hypot(
                deltaAzimuth, deltaElevation);
        const double normalizedAzimuth =
            deltaAzimuth
            / std::max(
                example.azimuthSigmaDegrees,
                1.0e-6);
        const double normalizedElevation =
            deltaElevation
            / std::max(
                example.elevationSigmaDegrees,
                1.0e-6);
        const double denominator =
            std::max(
                1.0
                    - example.correlation
                        * example.correlation,
                1.0e-3);
        const double mahalanobisSquared = (
            normalizedAzimuth
                * normalizedAzimuth
            - 2.0 * example.correlation
                * normalizedAzimuth
                * normalizedElevation
            + normalizedElevation
                * normalizedElevation)
            / denominator;
        const bool accept =
            prediction.decoded.at(5) >= 0.55
            && correctionDegrees <= 1.5
            && mahalanobisSquared <= 6.25;
        double neuralAzimuth =
            example.physicalAzimuthDegrees;
        double neuralElevation =
            example.physicalElevationDegrees;
        if (accept) {
            const double blend = std::clamp(
                0.30 * prediction.decoded.at(5)
                    * example.reliability,
                0.0, 1.0);
            neuralAzimuth +=
                blend * deltaAzimuth;
            neuralElevation +=
                blend * deltaElevation;
            ++applied;
        }
        gridErrors.append(
            example.gridArgmaxErrorDegrees);
        physicalErrors.append(
            example.physicalErrorDegrees);
        neuralErrors.append(
            angularDistanceDegrees(
                example.trueAzimuthDegrees,
                example.trueElevationDegrees,
                neuralAzimuth,
                neuralElevation));
    }
    Metrics metrics;
    metrics.gridMedianDegrees =
        percentile(gridErrors, 0.5);
    metrics.physicalMedianDegrees =
        percentile(physicalErrors, 0.5);
    metrics.neuralMedianDegrees =
        percentile(neuralErrors, 0.5);
    metrics.physicalP95Degrees =
        percentile(physicalErrors, 0.95);
    metrics.neuralP95Degrees =
        percentile(neuralErrors, 0.95);
    metrics.neuralAppliedSamples = applied;
    return metrics;
}

Metrics evaluateProductionPath(
    const Model &model,
    const QVector<Example> &examples,
    QJsonObject *bucketReport,
    bool *bucketsAccepted,
    QString *errorMessage)
{
    struct BucketErrors {
        QVector<double> grid;
        QVector<double> physical;
        QVector<double> neural;
        QVector<double> reliability;
        QVector<double> coherence;
        QVector<double> validChannelFraction;
        int applied = 0;
    };
    QVector<double> gridErrors;
    QVector<double> physicalErrors;
    QVector<double> neuralErrors;
    gridErrors.reserve(examples.size());
    physicalErrors.reserve(examples.size());
    neuralErrors.reserve(examples.size());
    int applied = 0;
    QMap<QString, BucketErrors> buckets;

    LocatorConfiguration configuration;
    configuration.microphones =
        createDefaultArrayGeometry();
    configuration.sampleRate = SampleRate;
    configuration.windowSamples = WindowSamples;
    configuration.soundSpeedMetersPerSecond =
        SoundSpeed;
    configuration.confidenceThreshold = 0.45;
    configuration.smoothingFactor = 0.35;
    configuration.spatialResolutionMode =
        SpatialResolutionMode::HighPrecision;
    configuration.neuralPeakEnhancementEnabled =
        false;
    RobustSrpPhatScorer scorer;
    if (!scorer.configure(
            configuration, errorMessage)) {
        return {};
    }

    for (const Example &example : examples) {
        if (!example
                 .evaluationPreviousWindow
                 .has_value()
            || !example.evaluationWindow.has_value()
            || !scorer.prepareWindow(
                *example
                     .evaluationPreviousWindow)
            || !scorer.prepareWindow(
                *example.evaluationWindow)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral(
                    "production evaluation window unavailable");
            }
            return {};
        }
        const RobustSpatialEstimate physical =
            scorer.estimate();
        const std::optional<SrpLocalPeakPatch> patch =
            scorer.sampleLocalPeakPatch(physical);
        if (!physical.hasDirection
            || !patch.has_value()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral(
                    "production evaluation physical path failed");
            }
            return {};
        }

        double neuralAzimuth =
            physical.direction.azimuthDegrees;
        double neuralElevation =
            physical.direction.elevationDegrees;
        const ForwardCache prediction =
            forward(model, example);
        if (physical.reliability >=
                configuration.confidenceThreshold
            && prediction.decoded.at(5) >= 0.55) {
            const double blend = std::clamp(
                0.30 * prediction.decoded.at(5)
                    * physical.reliability,
                0.0, 1.0);
            const double deltaAzimuth =
                prediction.decoded.at(0)
                * patch->azimuthStepDegrees
                * blend;
            const double deltaElevation =
                prediction.decoded.at(1)
                * patch->elevationStepDegrees
                * blend;
            const double correctionDegrees =
                std::hypot(
                    deltaAzimuth, deltaElevation);
            const double azimuthSigma = std::max(
                physical
                    .azimuthStandardDeviationDegrees,
                1.0e-6);
            const double elevationSigma = std::max(
                physical
                    .elevationStandardDeviationDegrees,
                1.0e-6);
            const double correlation = std::clamp(
                physical
                    .azimuthElevationCorrelation,
                -0.95, 0.95);
            const double normalizedAzimuth =
                deltaAzimuth / azimuthSigma;
            const double normalizedElevation =
                deltaElevation / elevationSigma;
            const double mahalanobisSquared = (
                normalizedAzimuth
                    * normalizedAzimuth
                - 2.0 * correlation
                    * normalizedAzimuth
                    * normalizedElevation
                + normalizedElevation
                    * normalizedElevation)
                / (1.0
                   - correlation * correlation);
            const ScanDirection corrected {
                neuralAzimuth + deltaAzimuth,
                neuralElevation + deltaElevation,
            };
            const bool inBounds =
                corrected.azimuthDegrees >= -90.0
                && corrected.azimuthDegrees <= 90.0
                && corrected.elevationDegrees >= -90.0
                && corrected.elevationDegrees <= 90.0;
            const double correctedScore = inBounds
                ? scorer.scorePreparedDirectionSinc(
                      corrected)
                : std::numeric_limits<double>
                      ::quiet_NaN();
            if (correctionDegrees <= 1.5
                && std::isfinite(
                    mahalanobisSquared)
                && mahalanobisSquared <= 6.25
                && std::isfinite(correctedScore)
                && correctedScore
                    >= 0.97
                        * patch
                            ->centerPhysicalScore) {
                neuralAzimuth =
                    corrected.azimuthDegrees;
                neuralElevation =
                    corrected.elevationDegrees;
                ++applied;
            }
        }
        const double gridError =
            example.gridArgmaxErrorDegrees;
        const double physicalError =
            angularDistanceDegrees(
                example.trueAzimuthDegrees,
                example.trueElevationDegrees,
                physical.direction.azimuthDegrees,
                physical.direction.elevationDegrees);
        const double neuralError =
            angularDistanceDegrees(
                example.trueAzimuthDegrees,
                example.trueElevationDegrees,
                neuralAzimuth, neuralElevation);
        gridErrors.append(gridError);
        physicalErrors.append(physicalError);
        neuralErrors.append(neuralError);
        const auto recordBucket =
            [&](const QString &name) {
            BucketErrors &bucket = buckets[name];
            bucket.grid.append(gridError);
            bucket.physical.append(physicalError);
            bucket.neural.append(neuralError);
            bucket.reliability.append(
                physical.reliability);
            bucket.coherence.append(
                physical
                    .steeredChannelCoherence);
            bucket.validChannelFraction.append(
                physical.validChannelCount
                / static_cast<double>(
                    configuration
                        .microphones.size()));
            if (neuralAzimuth
                    != physical
                        .direction.azimuthDegrees
                || neuralElevation
                    != physical
                        .direction.elevationDegrees) {
                ++bucket.applied;
            }
        };
        recordBucket(
            example.fieldEdge
                ? QStringLiteral("fieldEdge")
                : QStringLiteral("interior"));
        recordBucket(QStringLiteral("corrupt%1")
                         .arg(example
                                  .corruptChannelCount));
        static const std::array<QString, 3>
            SignalKindNames {
                QStringLiteral("multiTone"),
                QStringLiteral("denseBroadband"),
                QStringLiteral("bandLimited"),
            };
        recordBucket(SignalKindNames.at(
            std::clamp(
                example.signalKind, 0, 2)));
        recordBucket(
            example.reflectionGain >= 0.4
                ? QStringLiteral("strongReflection")
                : QStringLiteral("weakReflection"));
    }

    Metrics metrics;
    metrics.gridMedianDegrees =
        percentile(gridErrors, 0.5);
    metrics.physicalMedianDegrees =
        percentile(physicalErrors, 0.5);
    metrics.neuralMedianDegrees =
        percentile(neuralErrors, 0.5);
    metrics.physicalP95Degrees =
        percentile(physicalErrors, 0.95);
    metrics.neuralP95Degrees =
        percentile(neuralErrors, 0.95);
    metrics.neuralAppliedSamples = applied;
    if (bucketReport != nullptr) {
        bool allBucketsAccepted = true;
        for (auto iterator = buckets.cbegin();
             iterator != buckets.cend();
             ++iterator) {
            const BucketErrors &bucket =
                iterator.value();
            const double physicalMedian =
                percentile(bucket.physical, 0.5);
            const double neuralMedian =
                percentile(bucket.neural, 0.5);
            const double physicalP95 =
                percentile(bucket.physical, 0.95);
            const double neuralP95 =
                percentile(bucket.neural, 0.95);
            constexpr int MinimumBucketSamples = 24;
            const bool bucketAccepted =
                bucket.grid.size()
                    >= MinimumBucketSamples
                && neuralMedian
                    <= physicalMedian + 0.05
                && neuralP95
                    <= physicalP95 + 0.05;
            allBucketsAccepted =
                allBucketsAccepted
                && bucketAccepted;
            bucketReport->insert(
                iterator.key(),
                QJsonObject {
                    {QStringLiteral("samples"),
                     bucket.grid.size()},
                    {QStringLiteral(
                         "gridArgmaxMedianDegrees"),
                     percentile(bucket.grid, 0.5)},
                    {QStringLiteral(
                         "physicalMedianDegrees"),
                     physicalMedian},
                    {QStringLiteral(
                         "neuralMedianDegrees"),
                     neuralMedian},
                    {QStringLiteral(
                         "physicalP95Degrees"),
                     physicalP95},
                    {QStringLiteral(
                         "neuralP95Degrees"),
                     neuralP95},
                    {QStringLiteral(
                         "neuralAppliedSamples"),
                     bucket.applied},
                    {QStringLiteral(
                         "medianPhysicalReliability"),
                     percentile(
                         bucket.reliability, 0.5)},
                    {QStringLiteral(
                         "medianSteeredChannelCoherence"),
                     percentile(
                         bucket.coherence, 0.5)},
                    {QStringLiteral(
                         "medianValidChannelFraction"),
                     percentile(
                         bucket
                             .validChannelFraction,
                         0.5)},
                    {QStringLiteral("accepted"),
                     bucketAccepted},
                    {QStringLiteral(
                         "minimumRequiredSamples"),
                     MinimumBucketSamples},
                });
        }
        if (bucketsAccepted != nullptr) {
            *bucketsAccepted =
                allBucketsAccepted;
        }
    } else if (bucketsAccepted != nullptr) {
        *bucketsAccepted = true;
    }
    return metrics;
}

bool predictedCorrectionImproves(
    const Model &model,
    const Example &example)
{
    const ForwardCache prediction =
        forward(model, example);
    const double deltaAzimuth =
        prediction.decoded.at(0)
        * example.azimuthStepDegrees;
    const double deltaElevation =
        prediction.decoded.at(1)
        * example.elevationStepDegrees;
    const double correctionDegrees = std::hypot(
        deltaAzimuth, deltaElevation);
    const double normalizedAzimuth =
        deltaAzimuth
        / std::max(
            example.azimuthSigmaDegrees,
            1.0e-6);
    const double normalizedElevation =
        deltaElevation
        / std::max(
            example.elevationSigmaDegrees,
            1.0e-6);
    const double denominator = std::max(
        1.0
            - example.correlation
                * example.correlation,
        1.0e-3);
    const double mahalanobisSquared = (
        normalizedAzimuth * normalizedAzimuth
        - 2.0 * example.correlation
            * normalizedAzimuth
            * normalizedElevation
        + normalizedElevation
            * normalizedElevation)
        / denominator;
    if (correctionDegrees > 1.5
        || mahalanobisSquared > 6.25) {
        return false;
    }
    const double blend = std::clamp(
        0.30 * example.reliability,
        0.0, 1.0);
    const double correctedError =
        angularDistanceDegrees(
            example.trueAzimuthDegrees,
            example.trueElevationDegrees,
            example.physicalAzimuthDegrees
                + blend * deltaAzimuth,
            example.physicalElevationDegrees
                + blend * deltaElevation);
    return correctedError + 0.005
        < example.physicalErrorDegrees;
}

Model calibrateConfidence(
    Model model,
    const QVector<Example> &training,
    const QVector<Example> &validation,
    std::uint64_t seed)
{
    QVector<quint8> labels;
    labels.reserve(training.size());
    int positiveCount = 0;
    for (const Example &example : training) {
        const bool positive =
            predictedCorrectionImproves(
                model, example);
        labels.append(positive ? 1 : 0);
        positiveCount += positive ? 1 : 0;
    }
    if (positiveCount == 0
        || positiveCount == training.size()) {
        return model;
    }

    const int negativeCount =
        training.size() - positiveCount;
    constexpr double positiveWeight = 1.0;
    constexpr double negativeWeight = 1.0;
    constexpr int ConfidenceParameterCount =
        Hidden2Count + 1;
    std::array<double, ConfidenceParameterCount>
        firstMoment {};
    std::array<double, ConfidenceParameterCount>
        secondMoment {};
    QVector<int> order(training.size());
    std::iota(order.begin(), order.end(), 0);
    std::mt19937_64 random(
        seed ^ UINT64_C(0xc0f1d3ce5eed1234));
    Model best = model;
    Metrics bestMetrics =
        evaluate(best, validation);
    std::uint64_t update = 0;
    int staleEpochs = 0;
    constexpr int BatchSize = 64;
    constexpr double LearningRate = 0.01;
    for (int epoch = 0; epoch < 80; ++epoch) {
        std::shuffle(
            order.begin(), order.end(), random);
        for (int first = 0;
             first < order.size();
             first += BatchSize) {
            const int last = std::min(
                first + BatchSize,
                static_cast<int>(order.size()));
            std::array<double,
                       ConfidenceParameterCount>
                gradient {};
            for (int item = first;
                 item < last;
                 ++item) {
                const int exampleIndex =
                    order.at(item);
                const ForwardCache prediction =
                    forward(
                        model,
                        training.at(exampleIndex));
                const double label =
                    labels.at(exampleIndex);
                const double weight = label > 0.5
                    ? positiveWeight
                    : negativeWeight;
                const double rawGradient =
                    weight
                    * (prediction.decoded.at(5)
                       - label);
                for (int source = 0;
                     source < Hidden2Count;
                     ++source) {
                    gradient[source] +=
                        rawGradient
                        * prediction.hidden2.at(
                            source);
                }
                gradient[Hidden2Count] +=
                    rawGradient;
            }
            ++update;
            const double inverseBatch =
                1.0 / (last - first);
            const double beta1Power =
                1.0 - std::pow(
                    0.9,
                    static_cast<double>(update));
            const double beta2Power =
                1.0 - std::pow(
                    0.999,
                    static_cast<double>(update));
            for (int parameter = 0;
                 parameter
                    < ConfidenceParameterCount;
                 ++parameter) {
                const double value =
                    gradient.at(parameter)
                    * inverseBatch;
                firstMoment[parameter] =
                    0.9
                        * firstMoment.at(parameter)
                    + 0.1 * value;
                secondMoment[parameter] =
                    0.999
                        * secondMoment.at(parameter)
                    + 0.001 * value * value;
                const double correctedFirst =
                    firstMoment.at(parameter)
                    / beta1Power;
                const double correctedSecond =
                    secondMoment.at(parameter)
                    / beta2Power;
                const double change =
                    LearningRate
                    * correctedFirst
                    / (std::sqrt(
                           correctedSecond)
                       + 1.0e-8);
                if (parameter < Hidden2Count) {
                    model.parameters[
                        W3Offset
                        + 5 * Hidden2Count
                        + parameter] -= change;
                } else {
                    model.parameters[B3Offset + 5]
                        -= change;
                }
            }
        }

        const Metrics metrics =
            evaluate(model, validation);
        const bool p95Safe =
            metrics.neuralP95Degrees
            <= metrics.physicalP95Degrees
                + 0.05;
        if (p95Safe
            && metrics.neuralMedianDegrees
                < bestMetrics.neuralMedianDegrees
                    - 1.0e-6) {
            best = model;
            bestMetrics = metrics;
            staleEpochs = 0;
        } else if (++staleEpochs >= 20) {
            break;
        }
    }
    return best;
}

Model trainModel(const QVector<Example> &training,
                 const QVector<Example> &validation,
                 std::uint64_t seed,
                 int maximumEpochs,
                 const Model *initialModel = nullptr)
{
    Model model;
    if (initialModel != nullptr) {
        model = *initialModel;
    } else {
        initializeModel(&model, training, seed);
    }
    Model best = model;
    std::array<double, ParameterCount> firstMoment {};
    std::array<double, ParameterCount> secondMoment {};
    std::mt19937_64 random(seed ^ UINT64_C(
        0xa5a5a5a55a5a5a5a));
    QVector<int> order(training.size());
    std::iota(order.begin(), order.end(), 0);
    double bestValidation =
        evaluate(model, validation)
            .neuralMedianDegrees;
    int staleEpochs = 0;
    std::uint64_t update = 0;
    constexpr int BatchSize = 32;
    constexpr double LearningRate = 0.002;
    for (int epoch = 0;
         epoch < maximumEpochs;
         ++epoch) {
        std::shuffle(
            order.begin(), order.end(), random);
        for (int first = 0;
             first < order.size();
             first += BatchSize) {
            const int last = std::min(
                first + BatchSize,
                static_cast<int>(
                    order.size()));
            std::array<double, ParameterCount>
                gradient {};
            for (int item = first;
                 item < last;
                 ++item) {
                accumulateGradient(
                    model,
                    training.at(order.at(item)),
                    &gradient);
            }
            ++update;
            const double inverseBatch =
                1.0 / (last - first);
            const double beta1Power =
                1.0 - std::pow(0.9,
                               static_cast<double>(
                                   update));
            const double beta2Power =
                1.0 - std::pow(0.999,
                               static_cast<double>(
                                   update));
            for (int parameter = 0;
                 parameter < ParameterCount;
                 ++parameter) {
                const double value =
                    gradient.at(parameter)
                    * inverseBatch;
                firstMoment[parameter] =
                    0.9
                        * firstMoment.at(parameter)
                    + 0.1 * value;
                secondMoment[parameter] =
                    0.999
                        * secondMoment.at(parameter)
                    + 0.001 * value * value;
                const double correctedFirst =
                    firstMoment.at(parameter)
                    / beta1Power;
                const double correctedSecond =
                    secondMoment.at(parameter)
                    / beta2Power;
                model.parameters[parameter] -=
                    LearningRate
                    * correctedFirst
                    / (std::sqrt(
                           correctedSecond)
                       + 1.0e-8);
            }
        }
        const double validationMedian =
            evaluate(model, validation)
                .neuralMedianDegrees;
        if (validationMedian
            < bestValidation - 1.0e-6) {
            bestValidation =
                validationMedian;
            best = model;
            staleEpochs = 0;
        } else if (++staleEpochs >= 20) {
            break;
        }
    }
    return calibrateConfidence(
        best, training, validation, seed);
}

std::uint64_t checksumFloatSequence(
    const QVector<float> &values)
{
    std::uint64_t checksum = FnvOffset;
    for (float value : values) {
        const std::uint32_t bits =
            std::bit_cast<std::uint32_t>(value);
        for (int shift = 0;
             shift < 32;
             shift += 8) {
            checksum ^=
                static_cast<std::uint8_t>(
                    (bits >> shift) & 0xffU);
            checksum *= FnvPrime;
        }
    }
    return checksum;
}

QVector<float> serializedModel(const Model &model)
{
    QVector<float> values;
    values.reserve(
        ParameterCount + 2 * InputCount);
    for (double parameter : model.parameters) {
        values.append(
            static_cast<float>(parameter));
    }
    for (const float value : model.inputMeans) {
        values.append(value);
    }
    for (const float value : model.inputScales) {
        values.append(value);
    }
    return values;
}

QString floatLiteral(float value)
{
    QString literal = QLocale::c().toString(
        static_cast<double>(value),
        'g', 9);
    if (!literal.contains(QLatin1Char('.'))
        && !literal.contains(QLatin1Char('e'),
                             Qt::CaseInsensitive)) {
        literal += QStringLiteral(".0");
    }
    return literal + QLatin1Char('f');
}

void writeArray(QTextStream *stream,
                const QString &name,
                const QVector<float> &values,
                int offset,
                int count)
{
    *stream << "inline constexpr std::array<float, "
            << count << "> " << name << " {\n";
    for (int index = 0; index < count; ++index) {
        if (index % 8 == 0) {
            *stream << "    ";
        }
        *stream << floatLiteral(
            values.at(offset + index));
        if (index + 1 != count) {
            *stream << ", ";
        }
        if (index % 8 == 7
            || index + 1 == count) {
            *stream << "\n";
        }
    }
    *stream << "};\n";
}

bool exportHeader(const QString &filePath,
                  const Model &model,
                  const Example &goldenExample,
                  const TrainingOptions &options,
                  std::uint64_t checksum,
                  QString *errorMessage)
{
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly
                   | QIODevice::Text)) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    const QVector<float> values =
        serializedModel(model);
    QTextStream stream(&file);
    stream << "#pragma once\n\n"
              "#include <array>\n"
              "#include <cstdint>\n\n"
              "namespace MicArray::NeuralModelData {\n\n"
              "inline constexpr std::uint32_t ModelVersion = 1;\n"
              "inline constexpr std::uint32_t TrainingSeed = "
           << options.seed << ";\n"
              "inline constexpr std::uint32_t TrainingSamples = "
           << options.trainSamples << ";\n"
              "inline constexpr std::uint32_t ValidationSamples = "
           << options.validationSamples << ";\n"
              "inline constexpr std::uint32_t TestSamples = "
           << options.testSamples << ";\n"
              "inline constexpr int InputCount = 93;\n"
              "inline constexpr int Hidden1Count = "
           << Hidden1Count << ";\n"
              "inline constexpr int Hidden2Count = "
           << Hidden2Count << ";\n"
              "inline constexpr int OutputCount = 6;\n"
              "inline constexpr int ParameterCount = "
           << ParameterCount << ";\n\n";
    writeArray(
        &stream, QStringLiteral("InputWeights"),
        values, W1Offset,
        InputCount * Hidden1Count);
    writeArray(
        &stream, QStringLiteral("Hidden1Bias"),
        values, B1Offset, Hidden1Count);
    writeArray(
        &stream, QStringLiteral("Hidden1Weights"),
        values, W2Offset,
        Hidden1Count * Hidden2Count);
    writeArray(
        &stream, QStringLiteral("Hidden2Bias"),
        values, B2Offset, Hidden2Count);
    writeArray(
        &stream, QStringLiteral("Hidden2Weights"),
        values, W3Offset,
        Hidden2Count * OutputCount);
    writeArray(
        &stream, QStringLiteral("OutputBias"),
        values, B3Offset, OutputCount);
    writeArray(
        &stream, QStringLiteral("InputMeans"),
        values, ParameterCount, InputCount);
    writeArray(
        &stream, QStringLiteral("InputScales"),
        values, ParameterCount + InputCount,
        InputCount);
    QVector<float> goldenInput;
    goldenInput.reserve(InputCount);
    for (float value : goldenExample.input) {
        goldenInput.append(value);
    }
    const Model exportedModel =
        quantizedModel(model);
    const ForwardCache goldenForward =
        forward(exportedModel, goldenExample);
    QVector<float> goldenOutput;
    goldenOutput.reserve(OutputCount);
    for (double value : goldenForward.decoded) {
        goldenOutput.append(
            static_cast<float>(value));
    }
    writeArray(
        &stream, QStringLiteral("GoldenInput"),
        goldenInput, 0, InputCount);
    writeArray(
        &stream, QStringLiteral("GoldenOutput"),
        goldenOutput, 0, OutputCount);
    stream << "\ninline constexpr std::uint64_t Checksum =\n"
              "    UINT64_C(0x"
           << QString::number(checksum, 16)
           << ");\n\n"
              "} // namespace MicArray::NeuralModelData\n";
    if (stream.status() != QTextStream::Ok
        || !file.commit()) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    return true;
}

bool exportReport(const QString &filePath,
                  const TrainingOptions &options,
                  const Metrics &metrics,
                  const QJsonObject &bucketMetrics,
                  std::uint64_t checksum,
                  bool accepted,
                  QString *errorMessage)
{
    QJsonObject report {
        {QStringLiteral("modelVersion"), 1},
        {QStringLiteral("seed"),
         static_cast<qint64>(options.seed)},
        {QStringLiteral("trainSamples"),
         options.trainSamples},
        {QStringLiteral("validationSamples"),
         options.validationSamples},
        {QStringLiteral("testSamples"),
         options.testSamples},
        {QStringLiteral("maximumEpochs"),
         options.maximumEpochs},
        {QStringLiteral("inputCount"),
         InputCount},
        {QStringLiteral("hidden1Count"),
         Hidden1Count},
        {QStringLiteral("hidden2Count"),
         Hidden2Count},
        {QStringLiteral("parameterCount"),
         ParameterCount},
        {QStringLiteral("gridArgmaxMedianDegrees"),
         metrics.gridMedianDegrees},
        {QStringLiteral("physicalMedianDegrees"),
         metrics.physicalMedianDegrees},
        {QStringLiteral("neuralMedianDegrees"),
         metrics.neuralMedianDegrees},
        {QStringLiteral("physicalP95Degrees"),
         metrics.physicalP95Degrees},
        {QStringLiteral("neuralP95Degrees"),
         metrics.neuralP95Degrees},
        {QStringLiteral("neuralAppliedSamples"),
         metrics.neuralAppliedSamples},
        {QStringLiteral("gridMedianImprovementPercent"),
         100.0 * (
             1.0
             - metrics.neuralMedianDegrees
                 / metrics.gridMedianDegrees)},
        {QStringLiteral(
             "physicalMedianImprovementPercent"),
         100.0 * (
             1.0
             - metrics.neuralMedianDegrees
                 / metrics.physicalMedianDegrees)},
        {QStringLiteral("p95DeltaDegrees"),
         metrics.neuralP95Degrees
             - metrics.physicalP95Degrees},
        {QStringLiteral(
             "trainingDistributionTargets"),
         QStringLiteral(
             "50% nominal, 20% one corrupt, "
             "20% two corrupt, 10% four corrupt; "
             "80% multi-tone, 10% dense broadband, "
             "10% band-limited; retained counts are "
             "reported in heldOutBuckets")},
        {QStringLiteral("trainingInitialization"),
         QStringLiteral(
             "validated embedded checkpoint warm start; "
             "fine-tuned replacement requires at least "
             "5% production-validation improvement")},
        {QStringLiteral("evaluationPath"),
         QStringLiteral(
             "production-equivalent physical SRP, "
             "default 0.35 temporal smoothing, "
             "confidence, 1.5 degree, 2.5 sigma, "
             "97% sinc-score and field-of-view gates")},
        {QStringLiteral(
             "minimumPhysicalMedianImprovementPercent"),
         0.3},
        {QStringLiteral(
             "minimumUpgradeValidationImprovementPercent"),
         5.0},
        {QStringLiteral("evaluationStrength"),
         0.30},
        {QStringLiteral("heldOutBuckets"),
         bucketMetrics},
        {QStringLiteral("accepted"), accepted},
        {QStringLiteral("checksum"),
         QStringLiteral("%1").arg(
             checksum, 16, 16,
             QLatin1Char('0'))},
    };
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    const QByteArray json =
        QJsonDocument(report)
            .toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()
        || !file.commit()) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    return true;
}

bool writeFileAtomically(
    const QString &filePath,
    const QByteArray &contents,
    QString *errorMessage)
{
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(contents) != contents.size()
        || !file.commit()) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    return true;
}

bool restoreFile(
    const QString &filePath,
    bool existed,
    const QByteArray &contents,
    QString *errorMessage)
{
    if (existed) {
        return writeFileAtomically(
            filePath, contents, errorMessage);
    }
    if (!QFile::exists(filePath)
        || QFile::remove(filePath)) {
        return true;
    }
    if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral(
            "failed to remove newly created %1")
            .arg(filePath);
    }
    return false;
}

bool exportArtifactPair(
    const TrainingOptions &options,
    const Model &model,
    const Example &goldenExample,
    const Metrics &metrics,
    const QJsonObject &bucketMetrics,
    std::uint64_t checksum,
    QString *errorMessage)
{
    if (QFileInfo(options.outputHeader)
            .absoluteFilePath()
        == QFileInfo(options.outputReport)
            .absoluteFilePath()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "header and report paths must differ");
        }
        return false;
    }

    QTemporaryDir staging;
    if (!staging.isValid()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "failed to create artifact staging directory");
        }
        return false;
    }
    const QString stagedHeader =
        staging.filePath(QStringLiteral("model.h"));
    const QString stagedReport =
        staging.filePath(QStringLiteral("report.json"));
    if (!exportHeader(
            stagedHeader, model, goldenExample,
            options,
            checksum, errorMessage)
        || !exportReport(
            stagedReport, options, metrics,
            bucketMetrics, checksum, true,
            errorMessage)) {
        return false;
    }

    QFile headerInput(stagedHeader);
    QFile reportInput(stagedReport);
    if (!headerInput.open(QIODevice::ReadOnly)
        || !reportInput.open(QIODevice::ReadOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "failed to read staged artifacts");
        }
        return false;
    }
    const QByteArray newHeader =
        headerInput.readAll();
    const QByteArray newReport =
        reportInput.readAll();

    const bool headerExisted =
        QFile::exists(options.outputHeader);
    const bool reportExisted =
        QFile::exists(options.outputReport);
    QByteArray oldHeader;
    QByteArray oldReport;
    if (headerExisted) {
        QFile file(options.outputHeader);
        if (!file.open(QIODevice::ReadOnly)) {
            if (errorMessage != nullptr) {
                *errorMessage = file.errorString();
            }
            return false;
        }
        oldHeader = file.readAll();
    }
    if (reportExisted) {
        QFile file(options.outputReport);
        if (!file.open(QIODevice::ReadOnly)) {
            if (errorMessage != nullptr) {
                *errorMessage = file.errorString();
            }
            return false;
        }
        oldReport = file.readAll();
    }

    if (!writeFileAtomically(
            options.outputHeader,
            newHeader, errorMessage)) {
        return false;
    }
    QString reportError;
    if (writeFileAtomically(
            options.outputReport,
            newReport, &reportError)) {
        return true;
    }

    QString rollbackError;
    const bool headerRestored = restoreFile(
        options.outputHeader,
        headerExisted, oldHeader,
        &rollbackError);
    const bool reportRestored = restoreFile(
        options.outputReport,
        reportExisted, oldReport,
        &rollbackError);
    if (errorMessage != nullptr) {
        *errorMessage = headerRestored
                && reportRestored
            ? QStringLiteral(
                  "report export failed; prior artifact pair restored: %1")
                  .arg(reportError)
            : QStringLiteral(
                  "report export failed and artifact rollback failed: %1; %2")
                  .arg(reportError, rollbackError);
    }
    return false;
}

bool parsePositiveInt(
    const QCommandLineParser &parser,
    const QCommandLineOption &option,
    int *value,
    QString *errorMessage)
{
    bool okay = false;
    const int parsed =
        parser.value(option).toInt(&okay);
    if (!okay || parsed <= 0) {
        *errorMessage = QStringLiteral(
            "%1 must be a positive integer")
            .arg(option.names().first());
        return false;
    }
    *value = parsed;
    return true;
}

bool runTraining(const TrainingOptions &options,
                 QString *errorMessage)
{
    QTextStream(stdout)
        << "generating training data...\n";
    const QVector<Example> training =
        makeDataset(
            options.trainSamples,
            options.seed, errorMessage);
    if (training.size() != options.trainSamples) {
        return false;
    }
    const QVector<Example> validation =
        makeDataset(
            options.validationSamples,
            options.seed
                ^ UINT64_C(0x3141592653589793),
            errorMessage, true);
    if (validation.size()
        != options.validationSamples) {
        return false;
    }
    const QVector<Example> test =
        makeDataset(
            options.testSamples,
            options.seed
                ^ UINT64_C(0x2718281828459045),
            errorMessage, true);
    if (test.size() != options.testSamples) {
        return false;
    }

    QTextStream(stdout)
        << "training " << ParameterCount
        << " parameters...\n";
    const Model warmStart =
        quantizedModel(embeddedModel());
    const Model trainedCandidate = quantizedModel(
        trainModel(
            training, validation, options.seed,
            options.maximumEpochs,
            &warmStart));
    QJsonObject candidateValidationBuckets;
    bool candidateValidationAccepted = false;
    const Metrics candidateValidation =
        evaluateProductionPath(
            trainedCandidate, validation,
            &candidateValidationBuckets,
            &candidateValidationAccepted,
            errorMessage);
    if (errorMessage != nullptr
        && !errorMessage->isEmpty()) {
        return false;
    }
    QJsonObject warmStartValidationBuckets;
    bool warmStartValidationAccepted = false;
    const Metrics warmStartValidation =
        evaluateProductionPath(
            warmStart, validation,
            &warmStartValidationBuckets,
            &warmStartValidationAccepted,
            errorMessage);
    if (errorMessage != nullptr
        && !errorMessage->isEmpty()) {
        return false;
    }
    const bool useTrainedCandidate =
        candidateValidationAccepted
        && candidateValidation.neuralMedianDegrees
            <= 0.95
                * warmStartValidation
                    .neuralMedianDegrees
        && candidateValidation.neuralP95Degrees
            <= warmStartValidation
                    .neuralP95Degrees
                + 0.05;
    const Model model = useTrainedCandidate
        ? trainedCandidate : warmStart;
    QTextStream(stdout)
        << "production validation selected "
        << (useTrainedCandidate
                ? "fine-tuned candidate"
                : "quantized warm start")
        << "; candidate median "
        << candidateValidation.neuralMedianDegrees
        << " deg; warm-start median "
        << warmStartValidation.neuralMedianDegrees
        << " deg\n";
    QJsonObject bucketMetrics;
    bool bucketsAccepted = false;
    const Metrics metrics =
        evaluateProductionPath(
            model, test, &bucketMetrics,
            &bucketsAccepted,
            errorMessage);
    if (errorMessage != nullptr
        && !errorMessage->isEmpty()) {
        return false;
    }
    const bool accepted =
        metrics.neuralMedianDegrees
            <= 0.997
                * metrics.physicalMedianDegrees
        && metrics.neuralMedianDegrees
            <= 0.70
                * metrics.gridMedianDegrees
        && metrics.neuralP95Degrees
            <= metrics.physicalP95Degrees
                + 0.05
        && bucketsAccepted;
    QTextStream(stdout)
        << "grid median: "
        << metrics.gridMedianDegrees
        << " deg; physical median: "
        << metrics.physicalMedianDegrees
        << " deg; neural median: "
        << metrics.neuralMedianDegrees
        << " deg; physical P95: "
        << metrics.physicalP95Degrees
        << " deg; neural P95: "
        << metrics.neuralP95Degrees
        << " deg; applied: "
        << metrics.neuralAppliedSamples
        << "/" << test.size() << "\n";
    const QVector<float> serialized =
        serializedModel(model);
    const std::uint64_t checksum =
        checksumFloatSequence(serialized);
    if (!accepted) {
        *errorMessage = QStringLiteral(
            "independent test acceptance failed");
        return false;
    }
    return exportArtifactPair(
        options, model, test.first(), metrics,
        bucketMetrics,
        checksum, errorMessage);
}

bool modelsEqual(const Model &first,
                 const Model &second)
{
    return first.parameters == second.parameters
        && first.inputMeans == second.inputMeans
        && first.inputScales == second.inputScales;
}

int runEmbeddedEvaluation(
    const TrainingOptions &options,
    bool writeReport)
{
    QString error;
    const QVector<Example> test = makeDataset(
        options.testSamples,
        options.seed
            ^ UINT64_C(0x2718281828459045),
        &error, true);
    if (test.size() != options.testSamples) {
        QTextStream(stderr)
            << "embedded evaluation data failed: "
            << error << "\n";
        return 1;
    }
    QJsonObject buckets;
    bool bucketsAccepted = false;
    const Metrics metrics =
        evaluateProductionPath(
            embeddedModel(), test,
            &buckets, &bucketsAccepted,
            &error);
    if (!error.isEmpty()) {
        QTextStream(stderr)
            << "embedded evaluation failed: "
            << error << "\n";
        return 1;
    }
    QTextStream(stdout)
        << "embedded grid median: "
        << metrics.gridMedianDegrees
        << " deg; physical median: "
        << metrics.physicalMedianDegrees
        << " deg; neural median: "
        << metrics.neuralMedianDegrees
        << " deg; physical P95: "
        << metrics.physicalP95Degrees
        << " deg; neural P95: "
        << metrics.neuralP95Degrees
        << " deg; applied: "
        << metrics.neuralAppliedSamples
        << "/" << test.size() << "\n";
    const bool accepted =
        metrics.neuralMedianDegrees
            <= 0.997
                * metrics.physicalMedianDegrees
        && metrics.neuralMedianDegrees
            <= 0.70
                * metrics.gridMedianDegrees
        && metrics.neuralP95Degrees
            <= metrics.physicalP95Degrees
                + 0.05
        && bucketsAccepted;
    if (!accepted) {
        if (writeReport) {
            TrainingOptions diagnosticOptions =
                options;
            diagnosticOptions.trainSamples =
                NeuralModelData::TrainingSamples;
            diagnosticOptions.validationSamples =
                NeuralModelData
                    ::ValidationSamples;
            diagnosticOptions.maximumEpochs = 150;
            QString diagnosticError;
            if (!exportReport(
                    diagnosticOptions.outputReport,
                    diagnosticOptions, metrics,
                    buckets,
                    NeuralModelData::Checksum,
                    false, &diagnosticError)) {
                QTextStream(stderr)
                    << "failed evaluation report export failed: "
                    << diagnosticError << "\n";
            }
        }
        QTextStream(stderr)
            << "embedded production-path acceptance failed\n";
        return 1;
    }
    if (writeReport) {
        TrainingOptions reportOptions = options;
        reportOptions.trainSamples =
            NeuralModelData::TrainingSamples;
        reportOptions.validationSamples =
            NeuralModelData::ValidationSamples;
        reportOptions.testSamples =
            options.testSamples;
        reportOptions.maximumEpochs = 150;
        if (!exportArtifactPair(
                reportOptions,
                embeddedModel(), test.first(),
                metrics, buckets,
                NeuralModelData::Checksum,
                &error)) {
            QTextStream(stderr)
                << "embedded artifact refresh failed: "
                << error << "\n";
            return 1;
        }
    }
    return 0;
}

int runSelfTest()
{
    QString error;
    Example embeddedGolden;
    NeuralSrpPeakInput runtimeGolden;
    runtimeGolden.normalizedPatch.reserve(
        NeuralSrpPeakInput::PatchValueCount);
    for (int index = 0;
         index < InputCount;
         ++index) {
        embeddedGolden.input[index] =
            NeuralModelData::GoldenInput.at(index);
        if (index
            < NeuralSrpPeakInput
                ::PatchValueCount) {
            runtimeGolden
                .normalizedPatch.append(
                    NeuralModelData
                        ::GoldenInput.at(index));
        } else {
            runtimeGolden.diagnostics[
                index
                - NeuralSrpPeakInput
                    ::PatchValueCount] =
                NeuralModelData
                    ::GoldenInput.at(index);
        }
    }
    const ForwardCache trainerGolden =
        forward(embeddedModel(), embeddedGolden);
    const NeuralSrpPeakOutput runtimeOutput =
        NeuralSrpPeakRefiner {}.infer(
            runtimeGolden);
    const std::array<double, OutputCount>
        runtimeDecoded {
            runtimeOutput.deltaAzimuthCells,
            runtimeOutput.deltaElevationCells,
            runtimeOutput.azimuthSigmaScale,
            runtimeOutput.elevationSigmaScale,
            runtimeOutput.correlationResidual,
            runtimeOutput.confidence,
        };
    if (!runtimeOutput.valid) {
        QTextStream(stderr)
            << "embedded runtime parity inference failed\n";
        return 1;
    }
    for (int index = 0;
         index < OutputCount;
         ++index) {
        if (std::abs(
                trainerGolden.decoded.at(index)
                - runtimeDecoded.at(index))
                > 1.0e-6
            || std::abs(
                runtimeDecoded.at(index)
                - NeuralModelData
                    ::GoldenOutput.at(index))
                > 1.0e-6) {
            QTextStream(stderr)
                << "embedded runtime parity failed at output "
                << index << "\n";
            return 1;
        }
    }
    const QVector<Example> trainingFirst =
        makeDataset(32, 0x53454c46, &error);
    const QVector<Example> validationFirst =
        makeDataset(8, 0x56414c31, &error);
    const QVector<Example> trainingSecond =
        makeDataset(32, 0x53454c46, &error);
    const QVector<Example> validationSecond =
        makeDataset(8, 0x56414c31, &error);
    if (!error.isEmpty()
        || trainingFirst != trainingSecond
        || validationFirst != validationSecond) {
        QTextStream(stderr)
            << "dataset determinism failed: "
            << error << "\n";
        return 1;
    }
    const Model first = trainModel(
        trainingFirst, validationFirst,
        0x54524149, 4);
    const Model second = trainModel(
        trainingSecond, validationSecond,
        0x54524149, 4);
    if (!modelsEqual(first, second)) {
        QTextStream(stderr)
            << "training determinism failed\n";
        return 1;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        QTextStream(stderr)
            << "temporary directory failed\n";
        return 1;
    }
    TrainingOptions options;
    options.seed = 0x54524149;
    options.trainSamples = 32;
    options.validationSamples = 8;
    options.testSamples = 8;
    options.outputHeader =
        directory.filePath(
            QStringLiteral("model.h"));
    options.outputReport =
        directory.filePath(
            QStringLiteral("report.json"));
    const Metrics metrics =
        evaluate(first, validationFirst);
    const std::uint64_t checksum =
        checksumFloatSequence(
            serializedModel(first));
    if (!exportArtifactPair(
            options, first,
            validationFirst.first(),
            metrics, {}, checksum, &error)
        || QFileInfo(options.outputHeader).size()
            <= 0
        || QFileInfo(options.outputReport).size()
            <= 0) {
        QTextStream(stderr)
            << "atomic export failed: "
            << error << "\n";
        return 1;
    }
    QFile headerBeforeFailure(
        options.outputHeader);
    if (!headerBeforeFailure.open(
            QIODevice::ReadOnly)) {
        QTextStream(stderr)
            << "artifact rollback setup failed\n";
        return 1;
    }
    const QByteArray expectedHeader =
        headerBeforeFailure.readAll();
    TrainingOptions failingOptions = options;
    failingOptions.outputReport =
        directory.filePath(
            QStringLiteral(
                "missing/report.json"));
    QString expectedFailure;
    if (exportArtifactPair(
            failingOptions, first,
            validationFirst.first(),
            metrics, {}, checksum,
            &expectedFailure)) {
        QTextStream(stderr)
            << "artifact rollback failure was not detected\n";
        return 1;
    }
    QFile headerAfterFailure(
        options.outputHeader);
    if (!headerAfterFailure.open(
            QIODevice::ReadOnly)
        || headerAfterFailure.readAll()
            != expectedHeader) {
        QTextStream(stderr)
            << "artifact rollback changed prior header\n";
        return 1;
    }
    QTextStream(stdout)
        << "neural SRP trainer self-test passed\n";
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("train_neural_srp_peak"));
    QCommandLineParser parser;
    const QCommandLineOption help =
        parser.addHelpOption();
    const QCommandLineOption selfTest(
        QStringLiteral("self-test"),
        QStringLiteral(
            "Run deterministic generation, training, and export checks."));
    const QCommandLineOption evaluateEmbedded(
        QStringLiteral("evaluate-embedded"),
        QStringLiteral(
            "Evaluate the currently embedded model through production gates."));
    const QCommandLineOption seed(
        QStringLiteral("seed"),
        QStringLiteral("Deterministic training seed."),
        QStringLiteral("integer"),
        QStringLiteral("20260731"));
    const QCommandLineOption trainSamples(
        QStringLiteral("train-samples"),
        QStringLiteral("Number of training examples."),
        QStringLiteral("count"),
        QStringLiteral("4096"));
    const QCommandLineOption validationSamples(
        QStringLiteral("validation-samples"),
        QStringLiteral("Number of validation examples."),
        QStringLiteral("count"),
        QStringLiteral("512"));
    const QCommandLineOption testSamples(
        QStringLiteral("test-samples"),
        QStringLiteral("Number of independent test examples."),
        QStringLiteral("count"),
        QStringLiteral("512"));
    const QCommandLineOption maximumEpochs(
        QStringLiteral("maximum-epochs"),
        QStringLiteral("Maximum Adam epochs."),
        QStringLiteral("count"),
        QStringLiteral("100"));
    const QCommandLineOption outputHeader(
        QStringLiteral("output-header"),
        QStringLiteral("Generated C++ model header."),
        QStringLiteral("path"),
        QStringLiteral(
            "src/analysis/neuralmodeldata.h"));
    const QCommandLineOption outputReport(
        QStringLiteral("output-report"),
        QStringLiteral("Generated JSON training report."),
        QStringLiteral("path"),
        QStringLiteral(
            "doc/neural-srp-peak-v1-training-report.json"));
    parser.addOptions({
        selfTest, evaluateEmbedded,
        seed, trainSamples,
        validationSamples, testSamples,
        maximumEpochs, outputHeader,
        outputReport,
    });
    if (!parser.parse(
            QCoreApplication::arguments())) {
        QTextStream(stderr)
            << parser.errorText() << "\n";
        return 2;
    }
    if (parser.isSet(help)) {
        QTextStream(stdout)
            << parser.helpText();
        return 0;
    }
    if (parser.isSet(selfTest)) {
        return runSelfTest();
    }

    TrainingOptions options;
    bool seedOkay = false;
    options.seed =
        parser.value(seed).toULongLong(&seedOkay);
    QString error;
    if (!seedOkay
        || !parsePositiveInt(
            parser, trainSamples,
            &options.trainSamples, &error)
        || !parsePositiveInt(
            parser, validationSamples,
            &options.validationSamples, &error)
        || !parsePositiveInt(
            parser, testSamples,
            &options.testSamples, &error)
        || !parsePositiveInt(
            parser, maximumEpochs,
            &options.maximumEpochs, &error)) {
        if (!seedOkay) {
            error = QStringLiteral(
                "seed must be an unsigned integer");
        }
        QTextStream(stderr) << error << "\n";
        return 2;
    }
    options.outputHeader =
        parser.value(outputHeader);
    options.outputReport =
        parser.value(outputReport);
    if (options.testSamples < 256) {
        QTextStream(stderr)
            << "test-samples must be at least 256 "
               "for independent acceptance\n";
        return 2;
    }
    if (parser.isSet(evaluateEmbedded)) {
        return runEmbeddedEvaluation(
            options,
            parser.isSet(outputReport));
    }
    if (!runTraining(options, &error)) {
        QTextStream(stderr)
            << "training failed: " << error
            << "\n";
        return 1;
    }
    QTextStream(stdout)
        << "training and export passed\n";
    return 0;
}
