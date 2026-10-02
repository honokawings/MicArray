#include "cameraprofile.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace MicArray::Array128 {

QByteArray CameraProfile::json() const
{
    QJsonArray values {camera.centerAzimuthDegrees, camera.centerElevationDegrees,
        camera.horizontalFovDegrees, camera.verticalFovDegrees, camera.rollDegrees};
    QJsonArray points;
    for (const auto &p : samples) points.append(QJsonArray {p.direction.azimuthDegrees, p.direction.elevationDegrees,
        p.normalizedVideoPoint.x(), p.normalizedVideoPoint.y(), p.confidence, p.angularDeviationDegrees});
    return QJsonDocument(QJsonObject {{"format", "m128-camera-v2"}, {"projection", "pinhole"},
        {"values", values}, {"samples", points}, {"reference_size", QJsonArray {referenceSize.width(), referenceSize.height()}}}).toJson();
}

bool CameraProfile::load(const QByteArray &bytes, CameraProfile *destination, QString *error)
{
    auto fail = [&](const QString &message) { if (error) *error = message; return false; };
    const auto object = QJsonDocument::fromJson(bytes).object();
    const bool legacy = object["format"] == "m128-camera-v1";
    if (!legacy && (object["format"] != "m128-camera-v2" || object["projection"] != "pinhole"))
        return fail(QStringLiteral("校准文件格式或投影模型不匹配"));
    auto validNumber = [](const QJsonValue &value, double low, double high) {
        return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= low && value.toDouble() <= high;
    };
    const auto values = object["values"].toArray();
    if (values.size() != 5) return fail(QStringLiteral("校准文件需要五项视角参数"));
    for (int i = 0; i < 5; ++i) if (!validNumber(values[i], i == 2 || i == 3 ? 5 : -89, i == 2 || i == 3 ? 170 : 89))
        return fail(QStringLiteral("视角参数非数值或超出允许范围"));
    CameraProfile candidate;
    candidate.camera = {values[0].toDouble(), values[1].toDouble(), values[2].toDouble(), values[3].toDouble(), values[4].toDouble()};
    if (!legacy) {
        const auto size = object["reference_size"].toArray();
        if (size.size() != 2 || !validNumber(size[0], 1, 32768) || !validNumber(size[1], 1, 32768)
            || size[0].toDouble() != size[0].toInt() || size[1].toDouble() != size[1].toInt())
            return fail(QStringLiteral("校准参考图像尺寸无效"));
        candidate.referenceSize = {size[0].toInt(), size[1].toInt()};
        if (!object["samples"].isArray() || object["samples"].toArray().size() > 4)
            return fail(QStringLiteral("校准样本必须是至多四点的数组"));
        for (const auto &sample : object["samples"].toArray()) {
            const auto p = sample.toArray();
            if (p.size() != 6 || !validNumber(p[0], -90, 90) || !validNumber(p[1], -90, 90)
                || !validNumber(p[2], 0, 1) || !validNumber(p[3], 0, 1) || !validNumber(p[4], 0, 1) || !validNumber(p[5], 0, 180))
                return fail(QStringLiteral("校准点的方向、像素位置或质量参数无效"));
            candidate.samples.append({{p[0].toDouble(), p[1].toDouble()}, {p[2].toDouble(), p[3].toDouble()}, p[4].toDouble(), p[5].toDouble()});
        }
    }
    if (destination) *destination = candidate;
    if (error) error->clear();
    return true;
}

} // namespace MicArray::Array128
