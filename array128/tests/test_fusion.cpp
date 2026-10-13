#include "fusionmodel.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace MicArray;
using namespace MicArray::Array128;

namespace {
QJsonObject contract()
{
    return {
        {"format", "micarray128-yolox-fusion-v1"}, {"architecture", "YOLOX-Nano-derived-RGB-SRP-point-active"},
        {"domain", "simulation_only"}, {"input_name", "rgb_srp"}, {"input_dtype", "float32"},
        {"rgb_normalization", "uint8_div_255"}, {"srp_normalization", "cpp_response_0_1"},
        {"srp_resize", "bilinear_half_pixel_align_corners_false"}, {"output_name", "detections"},
        {"box_format", "normalized_cxcywh"}, {"source_point_format", "normalized_video_uv"},
        {"source_point_origin", "independent_learned_regression_head_not_box_center_or_srp_peak"},
        {"confidence_rule", "objectness_times_class_0"}, {"projection", "pinhole"},
        {"input_shape", QJsonArray {1, 4, 192, 320}}, {"output_shape", QJsonArray {1, 1260, 9}},
        {"channel_order", QJsonArray {"R", "G", "B", "SRP"}},
        {"output_layout", QJsonArray {"cx", "cy", "width", "height", "objectness", "class_0", "source_x", "source_y", "active"}},
        {"class_names", QJsonArray {"visible_candidate_simulation_domain"}},
        {"srp_color_map_or_focus_transform", false}, {"model_sha256", QString(64, '0')}
    };
}

QVector<float> readFloats(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = file.readAll();
    if (bytes.size() % sizeof(float)) return {};
    QVector<float> values(bytes.size() / sizeof(float));
    std::memcpy(values.data(), bytes.constData(), size_t(bytes.size()));
    return values;
}

#ifdef Q_OS_WIN
bool canOpenExclusiveForWrite(const QString &path)
{
    // OPEN_EXISTING does not create or truncate: only test the file lock.
    const HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}
#endif
}

class FusionTest : public QObject
{
    Q_OBJECT
private slots:
    void exactChannelOrderAndHalfPixelResize()
    {
        QImage video(2, 2, QImage::Format_RGB888);
        video.setPixelColor(0, 0, Qt::red); video.setPixelColor(1, 0, Qt::green);
        video.setPixelColor(0, 1, Qt::blue); video.setPixelColor(1, 1, Qt::white);
        QString error;
        const auto tensor = FusionModel::prepareInput(video, {0.f, .25f, .5f, 1.f}, 2, 2, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        constexpr int plane = 320 * 192;
        QCOMPARE(tensor.size(), 4 * plane);
        QCOMPARE(tensor[0], 1.f); QCOMPARE(tensor[plane], 0.f); QCOMPARE(tensor[2 * plane], 0.f); QCOMPARE(tensor[3 * plane], 0.f);
        QCOMPARE(tensor[319], 0.f); QCOMPARE(tensor[plane + 319], 1.f); QCOMPARE(tensor[3 * plane + 319], .25f);
        QCOMPARE(tensor[2 * plane - 1], 1.f); QCOMPARE(tensor[4 * plane - 1], 1.f);
        const double x = (160.5 * 2 / 320) - .5, y = (96.5 * 2 / 192) - .5;
        const int index = 96 * 320 + 160;
        QVERIFY(std::abs(tensor[index] - ((1 - x) * (1 - y) + x * y)) < 1e-6);
        QVERIFY(std::abs(tensor[plane + index] - x) < 1e-6);
        QVERIFY(std::abs(tensor[2 * plane + index] - y) < 1e-6);
        const double heat = .25 * x * (1 - y) + .5 * (1 - x) * y + x * y;
        QVERIFY(std::abs(tensor[3 * plane + index] - heat) < 1e-6);
        // A native-sized image must not be channel-swapped or re-normalized.
        QImage native(320, 192, QImage::Format_ARGB32); native.fill(QColor(17, 103, 249));
        const auto nativeTensor = FusionModel::prepareInput(native, QVector<float>(4, .37f), 2, 2, &error);
        QVERIFY(std::abs(nativeTensor[0] - 17 / 255.f) < 1e-7);
        QVERIFY(std::abs(nativeTensor[plane] - 103 / 255.f) < 1e-7);
        QVERIFY(std::abs(nativeTensor[2 * plane] - 249 / 255.f) < 1e-7);
        QVERIFY(std::abs(nativeTensor[3 * plane] - .37f) < 1e-7);
    }

    void malformedInputAndMetadataAreRejected()
    {
        QString error;
        QImage video(2, 2, QImage::Format_RGB888); video.fill(Qt::black);
        QVERIFY(FusionModel::prepareInput({}, {0, 0, 0, 0}, 2, 2, &error).isEmpty()); QVERIFY(!error.isEmpty());
        QVERIFY(FusionModel::prepareInput(video, {0, 0, 0}, 2, 2, &error).isEmpty());
        QVERIFY(FusionModel::prepareInput(video, {0, 0, 0, std::numeric_limits<float>::quiet_NaN()}, 2, 2, &error).isEmpty());
        QVERIFY(FusionModel::prepareInput(video, {0, 0, 0, 1.1f}, 2, 2, &error).isEmpty());
        QVERIFY(FusionModel::prepareInput(video, {0, 0, 0, 0}, 1, 4, &error).isEmpty());
        const auto valid = contract(); QVERIFY2(FusionModel::validateMetadata(valid, &error), qPrintable(error));
        for (auto it = valid.constBegin(); it != valid.constEnd(); ++it) {
            auto missing = valid; missing.remove(it.key());
            QVERIFY2(!FusionModel::validateMetadata(missing, &error), qPrintable("Missing field accepted: " + it.key()));
        }
        auto changed = valid; changed["input_shape"] = QJsonArray {1, 3, 192, 320};
        QVERIFY(!FusionModel::validateMetadata(changed, &error));
        changed = valid; changed["srp_color_map_or_focus_transform"] = true;
        QVERIFY(!FusionModel::validateMetadata(changed, &error));
        changed = valid; changed["source_point_origin"] = "box_center";
        QVERIFY(!FusionModel::validateMetadata(changed, &error));
        changed = valid; changed["class_names"] = QJsonArray {"person"};
        QVERIFY(!FusionModel::validateMetadata(changed, &error));
    }

    void neuralPointThresholdsAndNms()
    {
        QVector<float> output(1260 * 9, 0);
        auto row = [&](int index, std::initializer_list<float> values) {
            std::copy(values.begin(), values.end(), output.begin() + index * 9);
        };
        row(0, {.5f, .5f, .4f, .4f, .8f, .9f, .25f, .75f, .9f});
        row(1, {.51f, .51f, .4f, .4f, .7f, .9f, .4f, .4f, .9f}); // Same object, suppressed.
        row(2, {.9f, .1f, .1f, .1f, .8f, .9f, .95f, .05f, .95f});
        row(3, {.1f, .1f, .1f, .1f, .99f, .99f, .1f, .1f, .49f}); // Silent visual candidate.
        row(4, {.1f, .9f, .1f, .1f, .1f, .9f, .1f, .9f, .99f}); // Low object/class confidence.
        row(5, {.1f, .9f, .1f, .1f, .9f, .9f, 1.001f, .9f, .99f}); // Out-of-image neural point: reject, do not clamp.
        QString error;
        const auto result = FusionModel::decodeOutput(output, {}, {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(result.size(), 2);
        QCOMPARE(result[1].sourcePoint, QPointF(.25, .75));
        QVERIFY(result[1].sourcePoint != result[1].box.center());
        constexpr double pi = 3.14159265358979323846;
        QVERIFY(std::abs(result[1].direction.azimuthDegrees - std::atan2(-.5, 1.0) * 180 / pi) < 1e-4);
        QVERIFY(std::abs(result[1].direction.elevationDegrees - std::atan2(-.5 * std::tan(pi / 6), std::sqrt(1.25)) * 180 / pi) < 1e-4);
        QVERIFY(std::abs(result[1].confidence - .72) < 1e-6);
        FusionOptions strict; strict.confidenceThreshold = .99;
        QVERIFY(FusionModel::decodeOutput(output, {}, strict, &error).isEmpty()); QVERIFY(error.isEmpty());
        strict = {}; strict.maxDetections = 1;
        QCOMPARE(FusionModel::decodeOutput(output, {}, strict, &error).size(), 1);
        output[4] = 2;
        QVERIFY(FusionModel::decodeOutput(output, {}, {}, &error).isEmpty()); QVERIFY(!error.isEmpty());
        output[4] = std::numeric_limits<float>::infinity();
        QVERIFY(FusionModel::decodeOutput(output, {}, {}, &error).isEmpty());
        output.resize(100); QVERIFY(FusionModel::decodeOutput(output, {}, {}, &error).isEmpty());
    }

    void missingRuntimeAndHashMismatchAreExplicit()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.path() + "/fixture.onnx", metadataPath = directory.path() + "/fixture.json";
        QFile model(path); QVERIFY(model.open(QIODevice::WriteOnly)); model.write("not an onnx file"); model.close();
        auto description = contract();
        QFile metadata(metadataPath); QVERIFY(metadata.open(QIODevice::WriteOnly)); metadata.write(QJsonDocument(description).toJson()); metadata.close();
        FusionModel fusion; QString error;
        QVERIFY(!fusion.load(path, metadataPath, directory.path() + "/missing.dll", &error));
        QVERIFY(error.contains("SHA-256")); QVERIFY(!fusion.isLoaded());
        description["model_sha256"] = QString::fromLatin1(QCryptographicHash::hash("not an onnx file", QCryptographicHash::Sha256).toHex());
        QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate)); metadata.write(QJsonDocument(description).toJson()); metadata.close();
        QVERIFY(!fusion.load(path, metadataPath, directory.path() + "/missing.dll", &error));
        QVERIFY(error.contains("Runtime")); QVERIFY(error.contains(QStringLiteral("未找到")));
        const auto result = fusion.infer({}, {}, 0, 0, {});
        QVERIFY(!result.valid); QVERIFY(!result.error.isEmpty());
    }

    void actualOnnxRuntimeMatchesPythonGolden()
    {
        const QString directory = qEnvironmentVariable("M128_FUSION_MODEL_DIR");
        const QString runtime = qEnvironmentVariable("M128_ONNXRUNTIME");
        if (directory.isEmpty() || runtime.isEmpty()) {
            if (qEnvironmentVariableIntValue("M128_REQUIRE_FUSION_RUNTIME")) QFAIL("Real ONNX runtime/model fixture paths are required");
            QSKIP("Set M128_FUSION_MODEL_DIR and M128_ONNXRUNTIME for actual ONNX numerical equivalence; no neural performance is inferred from a skip");
        }
        const auto goldenInput = readFloats(directory + "/golden_input.f32");
        const auto goldenOutput = readFloats(directory + "/golden_output.f32");
        constexpr int plane = 320 * 192;
        QCOMPARE(goldenInput.size(), 4 * plane); QCOMPARE(goldenOutput.size(), 1260 * 9);
        // Compare the production 72x45 -> 320x192 heat resize to the actual
        // PyTorch tensor, not merely to another use of the C++ implementation.
        const QImage video(directory + "/golden_rgb.png"); QVERIFY(!video.isNull());
        const auto heat = readFloats(directory + "/golden_heat.f32");
        QFile heatDescription(directory + "/golden_heat.json"); QVERIFY(heatDescription.open(QIODevice::ReadOnly));
        const auto heatShape = QJsonDocument::fromJson(heatDescription.readAll()).object();
        const int columns = heatShape["columns"].toInt(), rows = heatShape["rows"].toInt();
        QCOMPARE(heat.size(), columns * rows);
        QString error;
        const auto prepared = FusionModel::prepareInput(video, heat, columns, rows, &error);
        QVERIFY(error.isEmpty()); QCOMPARE(prepared.size(), goldenInput.size());
        for (int i = 0; i < prepared.size(); ++i) QVERIFY2(std::abs(prepared[i] - goldenInput[i]) < 2e-6, qPrintable(QString("Input mismatch at %1").arg(i)));
        FusionModel model;
        QVERIFY2(model.load(directory + "/fusion.onnx", directory + "/fusion.json", runtime, &error), qPrintable(error));
        QVERIFY(model.isLoaded()); QVERIFY(!model.runtimeVersion().isEmpty());
        const auto actual = model.infer(video, heat, columns, rows, {});
        QVERIFY2(actual.valid, qPrintable(actual.error)); QCOMPARE(actual.rawOutput.size(), goldenOutput.size());
        double maximum = 0;
        for (int i = 0; i < goldenOutput.size(); ++i) {
            maximum = std::max(maximum, double(std::abs(actual.rawOutput[i] - goldenOutput[i])));
            QVERIFY2(std::abs(actual.rawOutput[i] - goldenOutput[i]) <= 1e-4 + 1e-4 * std::abs(goldenOutput[i]),
                qPrintable(QString("ONNX mismatch at %1: actual %2 Python %3").arg(i).arg(actual.rawOutput[i]).arg(goldenOutput[i])));
        }
        const auto expected = FusionModel::decodeOutput(goldenOutput, {}, {}, &error);
        QCOMPARE(actual.detections.size(), expected.size());
        qInfo() << "Real C ABI/ONNX numerical equivalence max error" << maximum << "inference ms" << actual.elapsedMs
                << "; numerical interface test only, independent of the model's accuracy evaluation";
        const QString malformedPath = directory + "/shape-invalid.onnx";
        QFile malformed(malformedPath); QVERIFY(malformed.open(QIODevice::ReadOnly));
        auto description = contract();
        description["model_sha256"] = QString::fromLatin1(QCryptographicHash::hash(malformed.readAll(), QCryptographicHash::Sha256).toHex());
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QFile descriptionFile(temporary.path() + "/shape.json"); QVERIFY(descriptionFile.open(QIODevice::WriteOnly));
        descriptionFile.write(QJsonDocument(description).toJson()); descriptionFile.close();
        QVERIFY(!model.load(malformedPath, descriptionFile.fileName(), runtime, &error));
        QVERIFY2(error.contains(QStringLiteral("形状")), qPrintable(error));
        QVERIFY(!model.isLoaded());
    }

    void windowsRuntimeCopyIsReleasedAfterSuccessAndShapeFailure()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows-only DLL exclusive-access regression");
#else
        const QString directory = qEnvironmentVariable("M128_FUSION_MODEL_DIR");
        const QString runtime = qEnvironmentVariable("M128_ONNXRUNTIME");
        if (directory.isEmpty() || runtime.isEmpty()) {
            if (qEnvironmentVariableIntValue("M128_REQUIRE_FUSION_RUNTIME")) QFAIL("Real ONNX runtime/model fixture paths are required");
            QSKIP("Set actual ONNX fixture paths for DLL release validation");
        }
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const QString copyPath = temporary.filePath("onnxruntime.dll");
        QVERIFY2(QFile::copy(runtime, copyPath), "Could not create an isolated runtime copy; the original DLL is never modified");
        const QString provider = QFileInfo(runtime).dir().filePath("onnxruntime_providers_shared.dll");
        if (QFileInfo::exists(provider))
            QVERIFY(QFile::copy(provider, temporary.filePath("onnxruntime_providers_shared.dll")));
        QVERIFY2(canOpenExclusiveForWrite(copyPath), "The unloaded temporary DLL must initially allow exclusive access");
        QString error;
        FusionModel model;
        QVERIFY2(model.load(directory + "/fusion.onnx", directory + "/fusion.json", copyPath, &error), qPrintable(error));
        QVERIFY2(!canOpenExclusiveForWrite(copyPath), "A successfully mapped DLL must deny exclusive write access");
        model.unload();
        QVERIFY(!model.isLoaded());
        QVERIFY2(canOpenExclusiveForWrite(copyPath), "Explicit unload left the runtime DLL mapped or locked");
        {
            FusionModel scoped;
            QVERIFY2(scoped.load(directory + "/fusion.onnx", directory + "/fusion.json", copyPath, &error), qPrintable(error));
            QVERIFY(!canOpenExclusiveForWrite(copyPath));
        }
        QVERIFY2(canOpenExclusiveForWrite(copyPath), "FusionModel destruction left the runtime DLL mapped or locked");

        const QString malformedPath = directory + "/shape-invalid.onnx";
        QFile malformed(malformedPath); QVERIFY(malformed.open(QIODevice::ReadOnly));
        auto description = contract();
        description["model_sha256"] = QString::fromLatin1(QCryptographicHash::hash(malformed.readAll(), QCryptographicHash::Sha256).toHex());
        QFile metadata(temporary.filePath("shape.json")); QVERIFY(metadata.open(QIODevice::WriteOnly));
        metadata.write(QJsonDocument(description).toJson()); metadata.close();
        // This valid ONNX graph gets as far as a real ORT session, then fails
        // the output-shape contract. The failed candidate must also unload.
        QVERIFY(!model.load(malformedPath, metadata.fileName(), copyPath, &error));
        QVERIFY2(error.contains(QStringLiteral("形状")), qPrintable(error));
        QVERIFY(!model.isLoaded());
        QVERIFY2(canOpenExclusiveForWrite(copyPath), "Shape-validation failure left the runtime DLL mapped or locked");
        qInfo() << "Temporary runtime DLL exclusive access verified after explicit unload, destruction and failed shape validation";
#endif
    }
};

QTEST_GUILESS_MAIN(FusionTest)
#include "test_fusion.moc"
