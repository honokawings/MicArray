#include "analysis/calibrationprofile.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLineF>
#include <QSaveFile>

#include <cmath>

namespace MicArray {
namespace {

constexpr auto ProfileFormat = "MicArrayVideoCalibration";
constexpr int ProfileVersion = 1;
constexpr int ArrayRows = 6;
constexpr int ArrayColumns = 8;
constexpr double ArraySpacingMeters = 0.05;

bool fail(const QString &message, QString *errorMessage)
{
    if (errorMessage) {
        *errorMessage = message;
    }
    return false;
}

bool finiteInRange(double value, double minimum, double maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool readNumber(const QJsonObject &object,
                const QString &name,
                double *value,
                QString *errorMessage)
{
    const QJsonValue jsonValue = object.value(name);
    if (!jsonValue.isDouble() || !std::isfinite(jsonValue.toDouble())) {
        return fail(QStringLiteral("字段“%1”必须是有限数值。").arg(name), errorMessage);
    }
    *value = jsonValue.toDouble();
    return true;
}

bool readInteger(const QJsonObject &object,
                 const QString &name,
                 int *value,
                 QString *errorMessage)
{
    double number = 0.0;
    if (!readNumber(object, name, &number, errorMessage)
        || std::floor(number) != number
        || number < 0.0
        || number > 65535.0) {
        return fail(QStringLiteral("字段“%1”必须是 0 到 65535 的整数。").arg(name),
                    errorMessage);
    }
    *value = static_cast<int>(number);
    return true;
}

QJsonObject cameraToJson(const CameraViewSettings &camera)
{
    return {
        {QStringLiteral("centerAzimuthDegrees"), camera.centerAzimuthDegrees},
        {QStringLiteral("centerElevationDegrees"), camera.centerElevationDegrees},
        {QStringLiteral("horizontalFovDegrees"), camera.horizontalFovDegrees},
        {QStringLiteral("verticalFovDegrees"), camera.verticalFovDegrees},
        {QStringLiteral("rollDegrees"), camera.rollDegrees},
    };
}

bool cameraFromJson(const QJsonValue &value,
                    CameraViewSettings *camera,
                    QString *errorMessage)
{
    if (!value.isObject()) {
        return fail(QStringLiteral("camera 必须是对象。"), errorMessage);
    }
    const QJsonObject object = value.toObject();
    return readNumber(object, QStringLiteral("centerAzimuthDegrees"),
                      &camera->centerAzimuthDegrees, errorMessage)
        && readNumber(object, QStringLiteral("centerElevationDegrees"),
                      &camera->centerElevationDegrees, errorMessage)
        && readNumber(object, QStringLiteral("horizontalFovDegrees"),
                      &camera->horizontalFovDegrees, errorMessage)
        && readNumber(object, QStringLiteral("verticalFovDegrees"),
                      &camera->verticalFovDegrees, errorMessage)
        && readNumber(object, QStringLiteral("rollDegrees"),
                      &camera->rollDegrees, errorMessage);
}

QJsonObject sampleToJson(const VideoCalibrationSample &sample)
{
    return {
        {QStringLiteral("azimuthDegrees"), sample.direction.azimuthDegrees},
        {QStringLiteral("elevationDegrees"), sample.direction.elevationDegrees},
        {QStringLiteral("confidence"), sample.confidence},
        {QStringLiteral("angularDeviationDegrees"), sample.angularDeviationDegrees},
        {QStringLiteral("normalizedVideoX"), sample.normalizedVideoPoint.x()},
        {QStringLiteral("normalizedVideoY"), sample.normalizedVideoPoint.y()},
    };
}

bool sampleFromJson(const QJsonValue &value,
                    VideoCalibrationSample *sample,
                    QString *errorMessage)
{
    if (!value.isObject()) {
        return fail(QStringLiteral("samples 中的元素必须是对象。"), errorMessage);
    }
    const QJsonObject object = value.toObject();
    double x = 0.0;
    double y = 0.0;
    if (!readNumber(object, QStringLiteral("azimuthDegrees"),
                    &sample->direction.azimuthDegrees, errorMessage)
        || !readNumber(object, QStringLiteral("elevationDegrees"),
                       &sample->direction.elevationDegrees, errorMessage)
        || !readNumber(object, QStringLiteral("confidence"),
                       &sample->confidence, errorMessage)
        || !readNumber(object, QStringLiteral("angularDeviationDegrees"),
                       &sample->angularDeviationDegrees, errorMessage)
        || !readNumber(object, QStringLiteral("normalizedVideoX"), &x, errorMessage)
        || !readNumber(object, QStringLiteral("normalizedVideoY"), &y, errorMessage)) {
        return false;
    }
    sample->normalizedVideoPoint = {x, y};
    return true;
}

} // namespace

QJsonObject cameraLensToJson(const CameraLensCalibration &lens)
{
    QJsonArray coefficients;
    for (const double coefficient : lens.coefficients)
    {
        coefficients.append(coefficient);
    }
    return {
        {QStringLiteral("model"), QStringLiteral("opencv-fisheye")},
        {QStringLiteral("enabled"), lens.enabled},
        {QStringLiteral("fxNormalized"), lens.fx},
        {QStringLiteral("fyNormalized"), lens.fy},
        {QStringLiteral("cxNormalized"), lens.cx},
        {QStringLiteral("cyNormalized"), lens.cy},
        {QStringLiteral("coefficients"), coefficients},
        {QStringLiteral("referenceWidth"), lens.referenceWidth},
        {QStringLiteral("referenceHeight"), lens.referenceHeight},
    };
}

bool cameraLensFromJson(const QJsonObject &object,
                        CameraLensCalibration *lens,
                        QString *errorMessage)
{
    CameraLensCalibration candidate;
    if (!lens || object.value(QStringLiteral("model")).toString()
                     != QStringLiteral("opencv-fisheye")
        || !object.value(QStringLiteral("enabled")).isBool()
        || !readNumber(object, QStringLiteral("fxNormalized"), &candidate.fx, errorMessage)
        || !readNumber(object, QStringLiteral("fyNormalized"), &candidate.fy, errorMessage)
        || !readNumber(object, QStringLiteral("cxNormalized"), &candidate.cx, errorMessage)
        || !readNumber(object, QStringLiteral("cyNormalized"), &candidate.cy, errorMessage)
        || !readInteger(object, QStringLiteral("referenceWidth"), &candidate.referenceWidth, errorMessage)
        || !readInteger(object, QStringLiteral("referenceHeight"), &candidate.referenceHeight, errorMessage))
    {
        return fail(QStringLiteral("镜头模型或参数无效。"), errorMessage);
    }
    const auto values = object.value(QStringLiteral("coefficients")).toArray();
    if (values.size() != 4)
    {
        return fail(QStringLiteral("镜头模型需要四个畸变系数。"), errorMessage);
    }
    for (int index = 0; index < 4; ++index)
    {
        if (!values[index].isDouble())
        {
            return fail(QStringLiteral("镜头畸变系数必须是有限数值。"), errorMessage);
        }
        candidate.coefficients[index] = values[index].toDouble();
    }
    candidate.enabled = object.value(QStringLiteral("enabled")).toBool();
    if (!candidate.isValid())
    {
        return fail(QStringLiteral("镜头参数无效或投影在前半球发生折叠。"), errorMessage);
    }
    *lens = candidate;
    return true;
}

bool VideoCalibrationProfileStore::validate(const VideoCalibrationProfile &profile,
                                            QString *errorMessage)
{
    if (!profile.camera.isValid()) {
        return fail(QStringLiteral("视角参数无效。"), errorMessage);
    }
    const QSize size = profile.referenceVideoSize;
    if ((size.width() == 0) != (size.height() == 0)
        || size.width() < 0 || size.height() < 0
        || size.width() > 65535 || size.height() > 65535) {
        return fail(QStringLiteral("参考视频尺寸无效。"), errorMessage);
    }
    if (!std::isfinite(profile.calibrationRmsPixels)
        || profile.calibrationRmsPixels < 0.0) {
        return fail(QStringLiteral("校准 RMS 必须是非负有限数值。"), errorMessage);
    }
    if (profile.samples.size() != 0 && profile.samples.size() != 4) {
        return fail(QStringLiteral("校准点数量只能是 0 或 4。"), errorMessage);
    }
    if (!profile.savedAtUtc.isValid()) {
        return fail(QStringLiteral("保存时间无效。"), errorMessage);
    }
    for (qsizetype index = 0; index < profile.samples.size(); ++index) {
        const VideoCalibrationSample &sample = profile.samples.at(index);
        if (!finiteInRange(sample.direction.azimuthDegrees, -180.0, 180.0)
            || !finiteInRange(sample.direction.elevationDegrees, -90.0, 90.0)
            || !finiteInRange(sample.normalizedVideoPoint.x(), 0.0, 1.0)
            || !finiteInRange(sample.normalizedVideoPoint.y(), 0.0, 1.0)
            || !finiteInRange(sample.confidence, 0.0, 1.0)
            || !std::isfinite(sample.angularDeviationDegrees)
            || sample.angularDeviationDegrees < 0.0) {
            return fail(QStringLiteral("第 %1 个校准点包含无效数值。").arg(index + 1),
                        errorMessage);
        }
        for (qsizetype other = 0; other < index; ++other) {
            const VideoCalibrationSample &existing = profile.samples.at(other);
            if (std::hypot(existing.direction.azimuthDegrees
                               - sample.direction.azimuthDegrees,
                           existing.direction.elevationDegrees
                               - sample.direction.elevationDegrees) < 2.0
                || QLineF(existing.normalizedVideoPoint,
                          sample.normalizedVideoPoint).length() < 0.02) {
                return fail(QStringLiteral("第 %1 个校准点与已有点过近。")
                                .arg(index + 1),
                            errorMessage);
            }
        }
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool VideoCalibrationProfileStore::save(const QString &filePath,
                                        const VideoCalibrationProfile &profile,
                                        QString *errorMessage)
{
    if (!validate(profile, errorMessage)) {
        return false;
    }

    QJsonArray samples;
    for (const VideoCalibrationSample &sample : profile.samples) {
        samples.append(sampleToJson(sample));
    }
    QJsonObject root {
        {QStringLiteral("format"), QString::fromLatin1(ProfileFormat)},
        {QStringLiteral("version"), ProfileVersion},
        {QStringLiteral("savedAtUtc"),
         profile.savedAtUtc.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("array"), QJsonObject {
             {QStringLiteral("rows"), ArrayRows},
             {QStringLiteral("columns"), ArrayColumns},
             {QStringLiteral("spacingMeters"), ArraySpacingMeters},
         }},
        {QStringLiteral("camera"), cameraToJson(profile.camera)},
        {QStringLiteral("referenceVideo"), QJsonObject {
             {QStringLiteral("width"), profile.referenceVideoSize.width()},
             {QStringLiteral("height"), profile.referenceVideoSize.height()},
         }},
        {QStringLiteral("calibrationRmsPixels"), profile.calibrationRmsPixels},
        {QStringLiteral("samples"), samples},
    };

    if (profile.camera.lens.isValid())
    {
        root.insert(QStringLiteral("version"), 2);
        root.insert(QStringLiteral("lens"), cameraLensToJson(profile.camera.lens));
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("无法写入校准文件：%1").arg(file.errorString()),
                    errorMessage);
    }
    const QByteArray contents = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size() || !file.commit()) {
        return fail(QStringLiteral("保存校准文件失败：%1").arg(file.errorString()),
                    errorMessage);
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool VideoCalibrationProfileStore::load(const QString &filePath,
                                        VideoCalibrationProfile *profile,
                                        QString *errorMessage)
{
    if (!profile) {
        return fail(QStringLiteral("校准配置输出对象不能为空。"), errorMessage);
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("无法读取校准文件：%1").arg(file.errorString()),
                    errorMessage);
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(QStringLiteral("校准文件不是有效的 JSON 对象：%1")
                        .arg(parseError.errorString()),
                    errorMessage);
    }
    const QJsonObject root = document.object();
    int version = 0;
    if (!root.value(QStringLiteral("format")).isString()
        || root.value(QStringLiteral("format")).toString()
            != QString::fromLatin1(ProfileFormat)
        || !readInteger(root, QStringLiteral("version"), &version, errorMessage)
        || (version != ProfileVersion && version != 2)) {
        return fail(QStringLiteral("校准文件格式或版本不受支持。"), errorMessage);
    }

    const QJsonValue arrayValue = root.value(QStringLiteral("array"));
    if (!arrayValue.isObject()) {
        return fail(QStringLiteral("array 必须是对象。"), errorMessage);
    }
    const QJsonObject array = arrayValue.toObject();
    double spacing = 0.0;
    int rows = 0;
    int columns = 0;
    if (!readInteger(array, QStringLiteral("rows"), &rows, errorMessage)
        || !readInteger(array, QStringLiteral("columns"), &columns, errorMessage)
        || !readNumber(array, QStringLiteral("spacingMeters"), &spacing, errorMessage)
        || rows != ArrayRows || columns != ArrayColumns
        || std::abs(spacing - ArraySpacingMeters) > 1.0e-9) {
        return fail(QStringLiteral("校准文件的麦克风阵列规格不匹配。"), errorMessage);
    }

    VideoCalibrationProfile candidate;
    if (!cameraFromJson(root.value(QStringLiteral("camera")),
                        &candidate.camera, errorMessage)) {
        return false;
    }
    if (version == 2)
    {
        if (!root.value(QStringLiteral("lens")).isObject()
            || !cameraLensFromJson(root.value(QStringLiteral("lens")).toObject(),
                                    &candidate.camera.lens, errorMessage))
        {
            return fail(QStringLiteral("镜头标定数据缺失或无效。"), errorMessage);
        }
    }
    const QJsonValue referenceValue = root.value(QStringLiteral("referenceVideo"));
    if (!referenceValue.isObject()) {
        return fail(QStringLiteral("referenceVideo 必须是对象。"), errorMessage);
    }
    int width = 0;
    int height = 0;
    const QJsonObject reference = referenceValue.toObject();
    if (!readInteger(reference, QStringLiteral("width"), &width, errorMessage)
        || !readInteger(reference, QStringLiteral("height"), &height, errorMessage)
        || !readNumber(root, QStringLiteral("calibrationRmsPixels"),
                       &candidate.calibrationRmsPixels, errorMessage)) {
        return false;
    }
    candidate.referenceVideoSize = {width, height};
    const QJsonValue savedAtValue = root.value(QStringLiteral("savedAtUtc"));
    if (!savedAtValue.isString()) {
        return fail(QStringLiteral("savedAtUtc 必须是字符串。"), errorMessage);
    }
    candidate.savedAtUtc =
        QDateTime::fromString(savedAtValue.toString(), Qt::ISODateWithMs).toUTC();

    const QJsonValue samplesValue = root.value(QStringLiteral("samples"));
    if (!samplesValue.isArray()) {
        return fail(QStringLiteral("samples 必须是数组。"), errorMessage);
    }
    for (const QJsonValue &value : samplesValue.toArray()) {
        VideoCalibrationSample sample;
        if (!sampleFromJson(value, &sample, errorMessage)) {
            return false;
        }
        candidate.samples.append(sample);
    }
    if (!validate(candidate, errorMessage)) {
        return false;
    }
    *profile = candidate;
    return true;
}

} // namespace MicArray
