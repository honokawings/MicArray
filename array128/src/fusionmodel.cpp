#include "fusionmodel.h"
#include "analysis/directionprojection.h"
#include "../third_party/onnxruntime/onnxruntime_c_api.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibrary>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace MicArray::Array128 {
namespace {

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

bool validOptions(const FusionOptions &options)
{
    return std::isfinite(options.confidenceThreshold) && options.confidenceThreshold >= 0 && options.confidenceThreshold <= 1
        && std::isfinite(options.activeThreshold) && options.activeThreshold >= 0 && options.activeThreshold <= 1
        && std::isfinite(options.nmsIou) && options.nmsIou >= 0 && options.nmsIou <= 1
        && options.maxDetections >= 1 && options.maxDetections <= 16;
}

struct ResampleAxis
{
    int lower;
    int upper;
    float fraction;
};

ResampleAxis resampleAxis(int outputIndex, int inputSize, int outputSize)
{
    // torch interpolate(..., bilinear, align_corners=False), edge replication.
    const double position = (outputIndex + .5) * inputSize / outputSize - .5;
    const int lower = int(std::floor(position));
    return {std::clamp(lower, 0, inputSize - 1), std::clamp(lower + 1, 0, inputSize - 1), float(position - lower)};
}

float bilinear(float a, float b, float c, float d, float x, float y)
{
    return (a * (1 - x) + b * x) * (1 - y) + (c * (1 - x) + d * x) * y;
}

} // namespace

struct FusionModel::Impl
{
    QLibrary library;
    const OrtApi *api = nullptr;
    OrtEnv *environment = nullptr;
    OrtSession *session = nullptr;
    OrtMemoryInfo *memory = nullptr;
    QJsonObject description;
    QString version;

    ~Impl()
    {
        if (api) {
            if (memory) api->ReleaseMemoryInfo(memory);
            if (session) api->ReleaseSession(session);
            if (environment) api->ReleaseEnv(environment);
        }
        // QLibrary destruction alone keeps the DLL resident. Pair every load
        // with unload, after all objects whose deleters use its C API are gone.
        library.unload();
    }

    bool accept(OrtStatus *status, QString *error) const
    {
        if (!status) return true;
        const QString message = QString::fromUtf8(api->GetErrorMessage(status));
        api->ReleaseStatus(status);
        return fail(error, QStringLiteral("ONNX Runtime：%1").arg(message));
    }

    bool checkTensorShape(const OrtTensorTypeAndShapeInfo *tensor, const QVector<int64_t> &expected, QString *error) const
    {
        if (!tensor) return fail(error, QStringLiteral("模型输入或输出不是张量"));
        ONNXTensorElementDataType type;
        size_t rank = 0;
        if (!accept(api->GetTensorElementType(tensor, &type), error)
            || !accept(api->GetDimensionsCount(tensor, &rank), error)) return false;
        if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || rank != size_t(expected.size()))
            return fail(error, QStringLiteral("模型张量必须是规定形状的 float32"));
        QVector<int64_t> actual;
        actual.resize(qsizetype(rank));
        if (!accept(api->GetDimensions(tensor, actual.data(), rank), error)) return false;
        if (actual != expected) return fail(error, QStringLiteral("模型张量形状不符合 RGB+SRP YOLOX 契约（不接受动态维度）"));
        return true;
    }

    bool checkSession(QString *error) const
    {
        size_t inputs = 0, outputs = 0;
        if (!accept(api->SessionGetInputCount(session, &inputs), error)
            || !accept(api->SessionGetOutputCount(session, &outputs), error)) return false;
        if (inputs != 1 || outputs != 1) return fail(error, QStringLiteral("模型必须恰有一个输入和一个输出"));
        OrtAllocator *allocator = nullptr;
        if (!accept(api->GetAllocatorWithDefaultOptions(&allocator), error)) return false;
        for (bool input : {true, false}) {
            char *name = nullptr;
            if (!accept(input ? api->SessionGetInputName(session, 0, allocator, &name)
                              : api->SessionGetOutputName(session, 0, allocator, &name), error)) return false;
            const QByteArray actualName(name);
            allocator->Free(allocator, name);
            if (actualName != (input ? "rgb_srp" : "detections"))
                return fail(error, QStringLiteral("ONNX 输入输出名称与元数据不一致"));
            OrtTypeInfo *information = nullptr;
            if (!accept(input ? api->SessionGetInputTypeInfo(session, 0, &information)
                              : api->SessionGetOutputTypeInfo(session, 0, &information), error)) return false;
            const auto release = [this](OrtTypeInfo *value) { api->ReleaseTypeInfo(value); };
            std::unique_ptr<OrtTypeInfo, decltype(release)> owned(information, release);
            const OrtTensorTypeAndShapeInfo *tensor = nullptr;
            if (!accept(api->CastTypeInfoToTensorInfo(information, &tensor), error)) return false;
            const QVector<int64_t> expected = input ? QVector<int64_t> {1, 4, InputHeight, InputWidth}
                                                    : QVector<int64_t> {1, OutputAnchors, OutputFields};
            if (!checkTensorShape(tensor, expected, error)) return false;
        }
        return true;
    }
};

FusionModel::FusionModel() = default;
FusionModel::~FusionModel() = default;

bool FusionModel::validateMetadata(const QJsonObject &metadata, QString *error)
{
    if (error) error->clear();
    const std::array<std::pair<const char *, const char *>, 14> fields {{
        {"format", "micarray128-yolox-fusion-v1"}, {"architecture", "YOLOX-Nano-derived-RGB-SRP-point-active"},
        {"domain", "simulation_only"}, {"input_name", "rgb_srp"}, {"input_dtype", "float32"},
        {"rgb_normalization", "uint8_div_255"}, {"srp_normalization", "cpp_response_0_1"},
        {"srp_resize", "bilinear_half_pixel_align_corners_false"}, {"output_name", "detections"},
        {"box_format", "normalized_cxcywh"}, {"source_point_format", "normalized_video_uv"},
        {"source_point_origin", "independent_learned_regression_head_not_box_center_or_srp_peak"},
        {"confidence_rule", "objectness_times_class_0"}, {"projection", "pinhole"}
    }};
    for (const auto &[key, expected] : fields) {
        if (!metadata[key].isString() || metadata[key].toString() != QLatin1String(expected))
            return fail(error, QStringLiteral("模型元数据字段 %1 不符合已支持的融合契约").arg(QLatin1String(key)));
    }
    if (metadata["input_shape"].toArray() != QJsonArray {1, 4, InputHeight, InputWidth}
        || metadata["output_shape"].toArray() != QJsonArray {1, OutputAnchors, OutputFields}
        || metadata["channel_order"].toArray() != QJsonArray {"R", "G", "B", "SRP"}
        || metadata["output_layout"].toArray() != QJsonArray {"cx", "cy", "width", "height", "objectness", "class_0", "source_x", "source_y", "active"})
        return fail(error, QStringLiteral("模型维度、通道顺序或输出字段布局不匹配"));
    if (!metadata["srp_color_map_or_focus_transform"].isBool() || metadata["srp_color_map_or_focus_transform"].toBool())
        return fail(error, QStringLiteral("网络输入必须是原始归一化 SRP 响应，不能使用伪彩色或聚焦变换"));
    if (metadata["class_names"].toArray() != QJsonArray {"visible_candidate_simulation_domain"})
        return fail(error, QStringLiteral("模型类别定义不受支持；不能将仿真候选类冒充真实语义类别"));
    const auto hash = metadata["model_sha256"].toString().toLatin1();
    if (hash.size() != 64 || !std::all_of(hash.cbegin(), hash.cend(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    })) return fail(error, QStringLiteral("模型元数据缺少有效的 SHA-256"));
    return true;
}

bool FusionModel::load(const QString &modelPath, const QString &metadataPath, const QString &runtimePath, QString *error)
{
    unload();
    if (error) error->clear();
    QFile descriptionFile(metadataPath);
    if (!descriptionFile.open(QIODevice::ReadOnly) || descriptionFile.size() > 1024 * 1024)
        return fail(error, QStringLiteral("无法读取融合模型元数据，或文件超过 1 MiB"));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(descriptionFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("融合模型元数据不是有效的 JSON 对象"));
    if (!validateMetadata(document.object(), error)) return false;
    QFile model(modelPath);
    if (!model.open(QIODevice::ReadOnly) || model.size() <= 0 || model.size() > 512 * 1024 * 1024)
        return fail(error, QStringLiteral("无法读取 ONNX 模型，或模型大小不在 1 B..512 MiB 内"));
    const QByteArray modelBytes = model.readAll();
    if (modelBytes.size() != model.size()) return fail(error, QStringLiteral("ONNX 模型读取不完整"));
    if (QCryptographicHash::hash(modelBytes, QCryptographicHash::Sha256).toHex()
        != document.object()["model_sha256"].toString().toLatin1().toLower())
        return fail(error, QStringLiteral("ONNX 模型 SHA-256 与元数据不一致"));
    QString runtime = runtimePath;
    if (runtime.isEmpty()) {
#ifdef Q_OS_WIN
        runtime = QCoreApplication::applicationDirPath() + "/onnxruntime.dll";
#elif defined(Q_OS_MACOS)
        runtime = QCoreApplication::applicationDirPath() + "/libonnxruntime.dylib";
#else
        runtime = QCoreApplication::applicationDirPath() + "/libonnxruntime.so";
#endif
    }
    if (!QFileInfo(runtime).isFile()) return fail(error, QStringLiteral("未找到 ONNX Runtime 动态库：%1；SRP 定位仍可独立使用").arg(runtime));
    auto candidate = std::make_unique<Impl>();
    candidate->library.setFileName(QFileInfo(runtime).absoluteFilePath());
    if (!candidate->library.load()) return fail(error, QStringLiteral("无法加载 ONNX Runtime：%1").arg(candidate->library.errorString()));
    const auto getApiBase = reinterpret_cast<decltype(&OrtGetApiBase)>(candidate->library.resolve("OrtGetApiBase"));
    if (!getApiBase) return fail(error, QStringLiteral("动态库未导出 ONNX Runtime C API"));
    const OrtApiBase *base = getApiBase();
    if (!base || !(candidate->api = base->GetApi(ORT_API_VERSION)))
        return fail(error, QStringLiteral("ONNX Runtime 不支持 C API 30，请部署兼容运行库"));
    candidate->version = QString::fromUtf8(base->GetVersionString());
    const auto api = candidate->api;
    if (!candidate->accept(api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "MicArray128Fusion", &candidate->environment), error)
        || !candidate->accept(api->DisableTelemetryEvents(candidate->environment), error)) return false;
    OrtSessionOptions *rawOptions = nullptr;
    if (!candidate->accept(api->CreateSessionOptions(&rawOptions), error)) return false;
    const auto releaseOptions = [api](OrtSessionOptions *value) { api->ReleaseSessionOptions(value); };
    std::unique_ptr<OrtSessionOptions, decltype(releaseOptions)> options(rawOptions, releaseOptions);
    if (!candidate->accept(api->SetIntraOpNumThreads(rawOptions, 2), error)
        || !candidate->accept(api->SetInterOpNumThreads(rawOptions, 1), error)
        || !candidate->accept(api->SetSessionExecutionMode(rawOptions, ORT_SEQUENTIAL), error)
        || !candidate->accept(api->SetSessionGraphOptimizationLevel(rawOptions, ORT_ENABLE_ALL), error)
        || !candidate->accept(api->CreateSessionFromArray(candidate->environment, modelBytes.constData(), size_t(modelBytes.size()), rawOptions, &candidate->session), error)
        || !candidate->checkSession(error)
        || !candidate->accept(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &candidate->memory), error)) return false;
    candidate->description = document.object();
    m_impl = std::move(candidate);
    return true;
}

void FusionModel::unload() { m_impl.reset(); }
bool FusionModel::isLoaded() const { return bool(m_impl); }
QJsonObject FusionModel::metadata() const { return m_impl ? m_impl->description : QJsonObject {}; }
QString FusionModel::runtimeVersion() const { return m_impl ? m_impl->version : QString {}; }

QVector<float> FusionModel::prepareInput(const QImage &video, const QVector<float> &heat, int columns, int rows, QString *error)
{
    if (error) error->clear();
    if (video.isNull() || video.width() < 1 || video.height() < 1) {
        fail(error, QStringLiteral("融合推理需要有效的视频帧")); return {};
    }
    if (columns < 2 || rows < 2 || columns > 4096 || rows > 4096 || heat.size() != qsizetype(columns) * rows
        || !std::all_of(heat.cbegin(), heat.cend(), [](float value) { return std::isfinite(value) && value >= -1e-5f && value <= 1.00001f; })) {
        fail(error, QStringLiteral("SRP 输入必须是有限的二维 0..1 数值响应，尺寸与数据长度一致")); return {};
    }
    const QImage rgb = video.convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull()) { fail(error, QStringLiteral("无法转换视频帧为 RGB")); return {}; }
    constexpr int plane = InputWidth * InputHeight;
    QVector<float> input(4 * plane);
    for (int y = 0; y < InputHeight; ++y) {
        const auto imageY = resampleAxis(y, rgb.height(), InputHeight), heatY = resampleAxis(y, rows, InputHeight);
        const auto *line0 = rgb.constScanLine(imageY.lower), *line1 = rgb.constScanLine(imageY.upper);
        for (int x = 0; x < InputWidth; ++x) {
            const auto imageX = resampleAxis(x, rgb.width(), InputWidth), heatX = resampleAxis(x, columns, InputWidth);
            const int index = y * InputWidth + x;
            for (int color = 0; color < 3; ++color) {
                input[color * plane + index] = bilinear(line0[3 * imageX.lower + color] / 255.f, line0[3 * imageX.upper + color] / 255.f,
                    line1[3 * imageX.lower + color] / 255.f, line1[3 * imageX.upper + color] / 255.f, imageX.fraction, imageY.fraction);
            }
            input[3 * plane + index] = bilinear(heat[heatY.lower * columns + heatX.lower], heat[heatY.lower * columns + heatX.upper],
                heat[heatY.upper * columns + heatX.lower], heat[heatY.upper * columns + heatX.upper], heatX.fraction, heatY.fraction);
        }
    }
    return input;
}

QVector<FusionDetection> FusionModel::decodeOutput(const QVector<float> &output, const CameraViewSettings &camera,
                                                  const FusionOptions &options, QString *error)
{
    if (error) error->clear();
    if (!camera.isValid() || !validOptions(options)) { fail(error, QStringLiteral("融合阈值或相机投影参数无效")); return {}; }
    if (output.size() != OutputAnchors * OutputFields
        || !std::all_of(output.cbegin(), output.cend(), [](float value) { return std::isfinite(value); })) {
        fail(error, QStringLiteral("融合输出应为有限的 [1,1260,9] float32 张量")); return {};
    }
    QVector<FusionDetection> candidates;
    for (int anchor = 0; anchor < OutputAnchors; ++anchor) {
        const float *row = output.constData() + anchor * OutputFields;
        if (row[4] < 0 || row[4] > 1 || row[5] < 0 || row[5] > 1 || row[8] < 0 || row[8] > 1) {
            fail(error, QStringLiteral("融合概率字段不在 0..1 内；可能加载了未解码的 logits 模型")); return {};
        }
        const double confidence = double(row[4]) * row[5];
        if (confidence < options.confidenceThreshold || row[8] < options.activeThreshold
            || row[2] <= 0 || row[3] <= 0 || row[6] < 0 || row[6] > 1 || row[7] < 0 || row[7] > 1) continue;
        FusionDetection detection;
        detection.box = QRectF(row[0] - row[2] / 2.0, row[1] - row[3] / 2.0, row[2], row[3]);
        detection.sourcePoint = {row[6], row[7]};
        detection.direction = unprojectVideoPoint(detection.sourcePoint, camera);
        detection.confidence = confidence; detection.activeProbability = row[8];
        detection.className = QStringLiteral("visible_candidate_simulation_domain");
        candidates.append(detection);
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const FusionDetection &a, const FusionDetection &b) {
        return a.confidence * a.activeProbability > b.confidence * b.activeProbability;
    });
    QVector<FusionDetection> kept;
    for (const auto &candidate : candidates) {
        bool suppressed = false;
        for (const auto &previous : kept) {
            const QRectF intersection = candidate.box.intersected(previous.box);
            const double overlap = intersection.isEmpty() ? 0 : intersection.width() * intersection.height();
            const double area = candidate.box.width() * candidate.box.height() + previous.box.width() * previous.box.height() - overlap;
            if (overlap / std::max(area, 1e-8) > options.nmsIou) { suppressed = true; break; }
        }
        if (!suppressed) kept.append(candidate);
        if (kept.size() >= options.maxDetections) break;
    }
    return kept;
}

FusionResult FusionModel::infer(const QImage &video, const QVector<float> &heat, int columns, int rows,
                                const CameraViewSettings &camera, const FusionOptions &options)
{
    FusionResult result;
    QElapsedTimer timer; timer.start();
    if (!m_impl) { result.error = QStringLiteral("尚未加载融合模型及运行库；可继续使用 SRP 定位"); return result; }
    if (!camera.isValid() || !validOptions(options)) { result.error = QStringLiteral("融合阈值或相机投影参数无效"); return result; }
    auto input = prepareInput(video, heat, columns, rows, &result.error);
    if (!result.error.isEmpty()) return result;
    const auto api = m_impl->api;
    const auto releaseValue = [api](OrtValue *value) { if (value) api->ReleaseValue(value); };
    OrtValue *inputValue = nullptr;
    const int64_t dimensions[] {1, 4, InputHeight, InputWidth};
    if (!m_impl->accept(api->CreateTensorWithDataAsOrtValue(m_impl->memory, input.data(), size_t(input.size()) * sizeof(float),
        dimensions, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputValue), &result.error)) return result;
    std::unique_ptr<OrtValue, decltype(releaseValue)> ownedInput(inputValue, releaseValue);
    const char *inputNames[] {"rgb_srp"}, *outputNames[] {"detections"};
    const OrtValue *inputValues[] {inputValue};
    OrtValue *outputValue = nullptr;
    if (!m_impl->accept(api->Run(m_impl->session, nullptr, inputNames, inputValues, 1, outputNames, 1, &outputValue), &result.error)) {
        if (outputValue) api->ReleaseValue(outputValue);
        return result;
    }
    std::unique_ptr<OrtValue, decltype(releaseValue)> ownedOutput(outputValue, releaseValue);
    OrtTensorTypeAndShapeInfo *shape = nullptr;
    if (!m_impl->accept(api->GetTensorTypeAndShape(outputValue, &shape), &result.error)) return result;
    const auto releaseShape = [api](OrtTensorTypeAndShapeInfo *value) { api->ReleaseTensorTypeAndShapeInfo(value); };
    std::unique_ptr<OrtTensorTypeAndShapeInfo, decltype(releaseShape)> ownedShape(shape, releaseShape);
    if (!m_impl->checkTensorShape(shape, {1, OutputAnchors, OutputFields}, &result.error)) return result;
    void *raw = nullptr;
    if (!m_impl->accept(api->GetTensorMutableData(outputValue, &raw), &result.error)) return result;
    const float *values = static_cast<const float *>(raw);
    result.rawOutput = QVector<float>(values, values + OutputAnchors * OutputFields);
    result.detections = decodeOutput(result.rawOutput, camera, options, &result.error);
    result.valid = result.error.isEmpty();
    result.elapsedMs = timer.nsecsElapsed() / 1e6;
    return result;
}

} // namespace MicArray::Array128
