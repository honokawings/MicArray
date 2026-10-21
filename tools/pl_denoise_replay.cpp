#include "media/noisereductionworker.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>
#include <QtEndian>

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace MicArray;

namespace {

QVector<float> readFloats(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(("Cannot open " + path).toStdString());
    const QByteArray bytes = file.readAll();
    if (bytes.size() % 4 != 0) throw std::runtime_error("Incomplete float32 data");
    QVector<float> result(bytes.size() / 4);
    for (int index = 0; index < result.size(); ++index)
    {
        result[index] = std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.constData() + index * 4));
        if (!std::isfinite(result[index])) throw std::runtime_error("Nonfinite input beam");
    }
    return result;
}

NoiseReductionSettings configure(const QJsonObject &config)
{
    if (config["schema_version"].toInt() != 1 || config["sample_rate_hz"].toInt() != 48000)
        throw std::runtime_error("Expected schema1 and 48kHz");
    NoiseReductionSettings settings;
    settings.inputMode = NoiseReductionInputMode::PlThreeBeamOriginalDenoising;
    settings.enabled = true;
    settings.monitorEnabled = false;
    const auto directions = config["directions"].toObject();
    auto direction = [&](const QString &name) {
        const auto value = directions[name].toObject();
        return ContinuousDirectionTarget{value["azimuth_deg"].toDouble(), value["elevation_deg"].toDouble()};
    };
    settings.enhancedA = direction("A"); settings.suppressionB1 = direction("B1"); settings.suppressionB2 = direction("B2");
    const auto wiener = config["wiener"].toObject();
    const auto algorithm = wiener["algorithm"].toString();
    const auto mode = wiener["mode"].toString();
    if ((algorithm != "fixed" && algorithm != "adaptive") || (mode != "singlePoint" && mode != "twoPoint"))
        throw std::runtime_error("Invalid Wiener algorithm or mode");
    settings.algorithm = algorithm == "adaptive" ? NoiseReductionAlgorithm::AdaptiveWiener : NoiseReductionAlgorithm::FixedParameterWiener;
    settings.mode = mode == "singlePoint" ? NoiseReductionMode::SinglePoint : NoiseReductionMode::TwoPoint;
    settings.singleSuppressionCoefficient = wiener["single_coefficient"].toDouble();
    settings.dualB1SuppressionCoefficient = wiener["b1_coefficient"].toDouble();
    settings.dualB2SuppressionCoefficient = wiener["b2_coefficient"].toDouble();
    const auto band = config["bandpass"].toObject();
    settings.bandPass.enabled = band["enabled"].toBool();
    settings.bandPass.lowCutoffHz = band["low_hz"].toDouble();
    settings.bandPass.highCutoffHz = band["high_hz"].toDouble();
    const auto log = config["logmmse"].toObject();
    settings.logMmse.enabled = log["enabled"].toBool();
    settings.logMmse.fftSize = log["fft_size"].toInt();
    settings.logMmse.overlapRatio = log["overlap_ratio"].toDouble();
    settings.logMmse.priorSnrSmoothing = log["prior_snr_smoothing"].toDouble();
    settings.logMmse.noisePowerSmoothing = log["noise_power_smoothing"].toDouble();
    settings.logMmse.minimumGainDb = log["minimum_gain_db"].toDouble();
    const auto agc = config["agc"].toObject();
    settings.automaticGainControl.enabled = agc["enabled"].toBool();
    settings.automaticGainControl.targetLevelDbfs = agc["target_level_dbfs"].toDouble();
    settings.automaticGainControl.maximumGainDb = agc["maximum_gain_db"].toDouble();
    settings.automaticGainControl.maximumAttenuationDb = agc["maximum_attenuation_db"].toDouble();
    settings.automaticGainControl.attackMilliseconds = agc["attack_ms"].toDouble();
    settings.automaticGainControl.releaseMilliseconds = agc["release_ms"].toDouble();
    settings.automaticGainControl.limiterLevelDbfs = agc["limiter_level_dbfs"].toDouble();
    settings.postProcessingOrder.clear();
    QSet<QString> nodes;
    for (const auto &node : config["post_processing_order"].toArray())
    {
        const auto key = node.toString();
        if ((key != "bandpass" && key != "logmmse" && key != "agc") || nodes.contains(key))
            throw std::runtime_error("Invalid post-processing order");
        nodes.insert(key);
        settings.postProcessingOrder.append(key == "bandpass" ? NoisePostProcessingNode::BandPass
            : (key == "logmmse" ? NoisePostProcessingNode::LogMmse : NoisePostProcessingNode::AutomaticGainControl));
    }
    if (nodes.size() != 3) throw std::runtime_error("All post-processing nodes must be explicit");
    return settings;
}

QJsonObject compareAndSave(const QVector<float> &actual, const QString &goldenPath, const QString &outputPath)
{
    const auto expected = readFloats(goldenPath);
    if (actual.size() != expected.size()) throw std::runtime_error("Output/golden sample count mismatch");
    QByteArray bytes(actual.size() * 4, Qt::Uninitialized);
    long double errorPower = 0, referencePower = 0, actualPower = 0;
    double maximumError = 0, actualPeak = 0;
    int differentBits = 0, nonfinite = 0;
    for (int index = 0; index < actual.size(); ++index)
    {
        const double value = actual[index], reference = expected[index];
        qToLittleEndian(std::bit_cast<quint32>(actual[index]), bytes.data() + index * 4);
        if (!std::isfinite(value)) { ++nonfinite; continue; }
        const double error = value - reference;
        errorPower += error * error;
        referencePower += reference * reference;
        actualPower += value * value;
        maximumError = std::max(maximumError, std::abs(error));
        actualPeak = std::max(actualPeak, std::abs(value));
        differentBits += std::bit_cast<quint32>(actual[index]) != std::bit_cast<quint32>(expected[index]);
    }
    QSaveFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot save float32 output");
    const double snr = errorPower > 0 && referencePower > 0 ? 10 * std::log10(double(referencePower / errorPower)) : 0;
    const double levelDifference = referencePower > 0 && actualPower > 0 ? 10 * std::log10(double(actualPower / referencePower)) : 0;
    // Internal numerical gate; not an industry audio quality standard or a listening-test claim.
    const bool passed = nonfinite == 0 && maximumError <= 1.0e-6
        && (errorPower == 0 || (referencePower > 0 && snr >= 80.0)) && std::abs(levelDifference) <= 0.001;
    return {{"sample_count", actual.size()}, {"golden_sample_count", expected.size()},
        {"max_absolute_error", maximumError}, {"rmse", actual.isEmpty() ? 0 : std::sqrt(double(errorPower / actual.size()))},
        {"error_snr_db_without_gain_fit", errorPower == 0 ? QJsonValue("infinite_exact") : QJsonValue(snr)},
        {"rms_level_difference_db", levelDifference}, {"actual_peak", actualPeak},
        {"different_float32_samples", differentBits}, {"non_finite_count", nonfinite}, {"passed_internal_numerical_gate", passed},
        {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
        {"golden_file", goldenPath}, {"output_file", outputPath}};
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    for (const QString &name : {QString("beam-dir"), QString("wiener-input"), QString("config"), QString("golden-dir"), QString("output")})
        parser.addOption(QCommandLineOption(name, name, "path"));
    parser.process(application);
    try
    {
        for (const QString &name : {QString("config"), QString("golden-dir"), QString("output")})
            if (!parser.isSet(name)) throw std::runtime_error(("Missing --" + name).toStdString());
        const bool postOnly = parser.isSet("wiener-input");
        if (postOnly == parser.isSet("beam-dir"))
            throw std::runtime_error("Supply exactly one of --beam-dir or --wiener-input");
        QFile configFile(parser.value("config"));
        if (!configFile.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open config");
        const auto document = QJsonDocument::fromJson(configFile.readAll());
        if (!document.isObject()) throw std::runtime_error("Invalid config JSON");
        auto settings = configure(document.object());
        const QDir beamDirectory(parser.value("beam-dir")), goldenDirectory(parser.value("golden-dir"));
        QDir output(parser.value("output"));
        if (output.exists() && !output.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty())
            throw std::runtime_error("Output directory is not empty");
        if (!QDir().mkpath(output.absolutePath())) throw std::runtime_error("Cannot create output directory");
        std::array<QVector<float>, 3> beams;
        std::array<QVector<float>, 4> stages;
        QVector<float> wienerInput;
        if (postOnly) {
            wienerInput = readFloats(parser.value("wiener-input"));
            if (wienerInput.isEmpty() || wienerInput.size() % 512 != 0)
                throw std::runtime_error("Wiener model must contain complete 512-sample hops");
            settings.inputMode = NoiseReductionInputMode::PlFixedWienerOriginalPostProcessing;
        } else {
        beams = {readFloats(beamDirectory.filePath("beam_A.f32le")),
            readFloats(beamDirectory.filePath("beam_B1.f32le")), readFloats(beamDirectory.filePath("beam_B2.f32le"))};
        if (beams[0].size() != beams[1].size() || beams[0].size() != beams[2].size() || beams[0].size() % 200 != 0)
            throw std::runtime_error("Three beam inputs must have equal complete 200-sample packets");
        }
        NoiseReductionWorker worker;
        QString error;
        QObject::connect(&worker, &NoiseReductionWorker::warningRaised, &worker, [&](const QString &message) { error = message; });
        worker.configure(settings, 48000);
        if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
        worker.setActive(true);
        worker.setRecordingStages({NoiseReductionDataFlowStage::Suppression, NoiseReductionDataFlowStage::BandPass,
                                  NoiseReductionDataFlowStage::LogMmse, NoiseReductionDataFlowStage::AutomaticGainControl});
        worker.setDiagnosticStageCapture(true);
        worker.authorizePlConfiguration(1, true);
        QObject::connect(&worker, &NoiseReductionWorker::diagnosticStageFrameReady, &worker,
            [&](const NoiseReductionRecordingFrame &frame) {
                for (int index = 0; index < 4; ++index) stages[index] += frame.stages[index].samples;
            });
        const int inputSamples = int(postOnly ? wienerInput.size() : beams[0].size());
        for (int offset = 0; offset < inputSamples; offset += 200)
        {
            const int count = std::min(200, inputSamples - offset);
            if (postOnly) {
                PlWienerFrame frame;
                frame.streamId = 1;
                frame.firstSampleIndex = offset;
                frame.timestampSeconds = 1 + offset / 48000;
                frame.timestampMicros = quint32(offset % 48000) * 1000000ULL / 48000;
                frame.samples = wienerInput.mid(offset, count);
                worker.processPlWienerFrame(frame);
            } else {
            PlBeamFrame frame;
            frame.configId = 1;
            frame.firstSampleIndex = offset;
            frame.timestampSeconds = 1 + offset / 48000;
            frame.timestampMicros = quint32(offset % 48000) * 1000000ULL / 48000;
            for (int beam = 0; beam < 3; ++beam) frame.beams[beam] = beams[beam].mid(offset, 200);
            worker.processPlBeamFrame(frame);
            }
            if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
        }
        const std::array<QString, 4> names {"wiener", "post_bandpass", "post_logmmse", "post_agc"};
        QJsonObject comparisons;
        bool passed = true;
        for (int stage = 0; stage < 4; ++stage)
        {
            const auto result = compareAndSave(stages[stage], goldenDirectory.filePath(names[stage] + ".f32le"),
                                               output.filePath(names[stage] + ".f32le"));
            comparisons.insert(names[stage], result);
            passed = passed && result["passed_internal_numerical_gate"].toBool();
        }
        const int finalStage = int(settings.postProcessingOrder.last()) + 1;
        const auto final = compareAndSave(stages[finalStage], goldenDirectory.filePath("final.f32le"), output.filePath("final.f32le"));
        comparisons.insert("final", final);
        passed = passed && final["passed_internal_numerical_gate"].toBool();
        const QJsonObject report {{"passed", passed}, {"config", document.object()},
            {"input_beam_samples", beams[0].size()}, {"output_samples", stages[finalStage].size()},
            {"input_wiener_samples", wienerInput.size()},
            {"input_stage", postOnly ? "candidate_model_wiener" : "PL_three_beams"},
            {"wiener_executed_in_replay", !postOnly},
            {"end_flush_performed", false}, {"time_alignment_samples", 0}, {"gain_fit_performed", false},
            {"implementation", postOnly
                ? "Actual candidate NoiseReductionWorker PL-Wiener entry through original post-processing only, 200-sample chunks plus real tail. No second Wiener, gain fit, padding, or board claim."
                : "Actual new NoiseReductionWorker PL-beam entry followed by unchanged original Wiener/ordered post-processing classes"},
            {"internal_gate", "max abs <=1e-6, error SNR >=80 dB unless exact, absolute RMS level delta <=0.001 dB, all finite; project numerical criterion, not industry standard or physical acoustic acceptance"},
            {"stages", comparisons}};
        QSaveFile resultFile(output.filePath("comparison.json"));
        const auto json = QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (!resultFile.open(QIODevice::WriteOnly) || resultFile.write(json) != json.size() || !resultFile.commit())
            throw std::runtime_error("Cannot save report");
        QTextStream(stdout) << (passed ? "PASS " : "FAIL ") << output.filePath("comparison.json") << Qt::endl;
        return passed ? 0 : 2;
    }
    catch (const std::exception &exception)
    {
        QTextStream(stderr) << "ERROR: " << exception.what() << Qt::endl;
        return 1;
    }
}
