#include "dsp.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <QtEndian>

#include <bit>
#include <cmath>
#include <stdexcept>

using namespace MicArray::Array128;

namespace {
void require(bool condition, const QString &message)
{
    if (!condition) throw std::runtime_error(message.toStdString());
}
double number(const QJsonObject &object, const QString &key, double fallback, double low, double high)
{
    if (!object.contains(key)) return fallback;
    const auto value = object[key];
    require(value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= low && value.toDouble() <= high,
        QStringLiteral("Invalid finite numeric field: %1").arg(key));
    return value.toDouble();
}
int integer(const QJsonObject &object, const QString &key, int fallback, int low, int high)
{
    const double value = number(object, key, fallback, low, high);
    require(std::floor(value) == value, QStringLiteral("Non-integer field: %1").arg(key));
    return int(value);
}
QJsonObject readObject(const QString &path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), file.errorString());
    require(file.size() <= 1024 * 1024, QStringLiteral("Scene JSON exceeds 1 MiB"));
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    require(error.error == QJsonParseError::NoError && doc.isObject(), QStringLiteral("Invalid scene JSON: ") + error.errorString());
    return doc.object();
}
QJsonObject evaluate(const QString &path)
{
    const auto input = readObject(path);
    require(input["format"].toString() == "micarray128-srp-input-v1", QStringLiteral("Unsupported scene format"));
    const QString id = input["scene_id"].toString();
    require(QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(id).hasMatch(), QStringLiteral("Invalid scene_id"));
    ProcessingSettings settings; settings.localization = true;
    QString error;
    require(Geometry::load(QJsonDocument(input["geometry"].toObject()).toJson(), &settings.geometry, &error), error);
    const auto camera = input["camera"].toObject();
    settings.camera = {
        number(camera, "center_azimuth_deg", 0, -89, 89), number(camera, "center_elevation_deg", 0, -89, 89),
        number(camera, "horizontal_fov_deg", 90, 5, 170), number(camera, "vertical_fov_deg", 60, 5, 170),
        number(camera, "roll_deg", 0, -89, 89)};
    settings.soundSpeed = number(input, "sound_speed_m_s", 343, 300, 380);
    settings.confidenceThreshold = number(input, "confidence_threshold", .35, 0, 1);
    settings.maxSources = integer(input, "max_sources", 4, 1, 16);
    settings.minimumSourceSeparationDegrees = number(input, "minimum_source_separation_deg", 8, 2, 90);
    const auto grid = input["grid"].toObject();
    settings.gridColumns = integer(grid, "columns", 72, 24, 160);
    settings.gridRows = integer(grid, "rows", 45, 15, 100);
    require(!input.contains("disabled_channels") || input["disabled_channels"].isArray(), QStringLiteral("disabled_channels must be an array"));
    for (const auto &entry : input["disabled_channels"].toArray()) {
        const double value = entry.toDouble(-1);
        require(entry.isDouble() && value >= 0 && value <= 127 && std::floor(value) == value, QStringLiteral("Invalid disabled channel"));
        const int ch = int(value); require(!settings.disabledChannels[ch], QStringLiteral("Duplicate disabled channel"));
        settings.disabledChannels[ch] = true;
    }
    const auto pcm = input["pcm"].toObject();
    require(pcm["format"].toString() == "f32le-interleaved" && pcm["channels"].toInt() == 128
        && pcm["sample_rate"].toInt() == 48000 && pcm["frames"].toInt() == 2048, QStringLiteral("Require 2048 x 128 f32le at 48000 Hz"));
    const QString pcmPath = pcm["path"].toString(); require(!pcmPath.isEmpty(), QStringLiteral("Missing PCM path"));
    QFile file(QFileInfo(path).dir().absoluteFilePath(pcmPath));
    require(file.open(QIODevice::ReadOnly), file.errorString());
    require(file.size() == 2048 * 128 * 4, QStringLiteral("PCM byte length mismatch"));
    const auto bytes = file.readAll();
    QVector<QVector<float>> channels(128, QVector<float>(2048));
    for (int n = 0; n < 2048; ++n) for (int ch = 0; ch < 128; ++ch)
        channels[ch][n] = std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.constData() + (n * 128 + ch) * 4));
    const auto result = localize(channels, settings);
    QJsonArray heat, sources;
    for (float value : result.heat) { require(std::isfinite(value), QStringLiteral("Non-finite SRP output")); heat.append(double(value)); }
    for (const auto &source : result.sources) sources.append(QJsonObject{
        {"azimuth_deg", source.direction.azimuthDegrees}, {"elevation_deg", source.direction.elevationDegrees},
        {"confidence", source.confidence}, {"relative_power", source.relativePower}, {"trusted", source.trusted}});
    return {{"format", "micarray128-srp-result-v1"}, {"scene_id", id}, {"columns", result.columns}, {"rows", result.rows},
        {"heat", heat}, {"sources", sources}, {"valid_channels", result.validChannels}, {"valid_pairs", result.validPairs},
        {"disabled_channels", result.disabledChannels}, {"elapsed_ms", result.elapsedMs}, {"reason", result.reason},
        {"trusted", result.trusted}, {"confidence", result.confidence},
        {"azimuth_deg", result.direction.azimuthDegrees}, {"elevation_deg", result.direction.elevationDegrees},
        {"method", "production-cpp-srp-phat; no ground-truth input; no synthetic visual peaks"}};
}
void save(const QString &path, const QJsonObject &object)
{
    require(!QFileInfo::exists(path), QStringLiteral("Refusing to overwrite output: ") + path);
    QSaveFile file(path); require(file.open(QIODevice::WriteOnly), file.errorString());
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    require(file.write(bytes) == bytes.size() && file.commit(), file.errorString());
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv); QCoreApplication::setApplicationName("m128_srp_export");
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOptions({{"input", "Scene JSON with PCM reference (no source truth).", "path"},
        {"output", "New result JSON path.", "path"}, {"manifest", "JSONL of scene JSON path strings (relative to manifest).", "path"},
        {"output-dir", "Batch output directory: <scene_id>.json", "path"}});
    parser.process(app);
    try {
        if (parser.isSet("manifest")) {
            require(!parser.isSet("input") && !parser.isSet("output") && parser.isSet("output-dir"), QStringLiteral("Batch requires --manifest and --output-dir only"));
            QFile manifest(parser.value("manifest")); require(manifest.open(QIODevice::ReadOnly), manifest.errorString());
            const QDir output(parser.value("output-dir")); require(QDir().mkpath(output.absolutePath()), QStringLiteral("Cannot create output directory"));
            int completed = 0;
            while (!manifest.atEnd()) {
                const auto line = manifest.readLine(1024 * 1024).trimmed(); if (line.isEmpty()) continue;
                QJsonParseError error; const auto wrapper = QJsonDocument::fromJson("[" + line + "]", &error);
                require(error.error == QJsonParseError::NoError && wrapper.array().size() == 1 && wrapper.array()[0].isString(), QStringLiteral("Manifest lines must be JSON path strings"));
                const auto result = evaluate(QFileInfo(manifest.fileName()).dir().absoluteFilePath(wrapper.array()[0].toString()));
                save(output.filePath(result["scene_id"].toString() + ".json"), result);
                if (++completed % 100 == 0) QTextStream(stdout) << "SRP exported " << completed << Qt::endl;
            }
            QTextStream(stdout) << "SRP exported " << completed << " scenes" << Qt::endl;
        } else {
            require(parser.isSet("input") && parser.isSet("output"), QStringLiteral("Require --input and --output"));
            save(parser.value("output"), evaluate(parser.value("input")));
        }
        return 0;
    } catch (const std::exception &error) { QTextStream(stderr) << error.what() << Qt::endl; return 1; }
}
