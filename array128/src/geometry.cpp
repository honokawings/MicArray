#include "geometry.h"
#include "protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <cmath>

namespace MicArray::Array128 {

QJsonObject Geometry::json() const
{
    QJsonArray points;
    for (int i = 0; i < microphones.size(); ++i) {
        const auto &p = microphones[i];
        points.append(QJsonObject {{"channel", i}, {"x", p.xMeters}, {"y", p.yMeters}, {"z", p.zMeters}});
    }
    return {{"format", "micarray128-geometry-v1"}, {"name", name}, {"units", "m"}, {"microphones", points}};
}

bool Geometry::load(const QByteArray &json, Geometry *destination, QString *error)
{
    auto fail = [&](const QString &message) { if (error) *error = message; return false; };
    const auto doc = QJsonDocument::fromJson(json);
    const auto object = doc.object();
    if (object["format"] != "micarray128-geometry-v1" || object["units"] != "m")
        return fail(QStringLiteral("必须使用 micarray128-geometry-v1，单位 m"));
    const auto points = object["microphones"].toArray();
    if (points.size() != Channels) return fail(QStringLiteral("必须提供完整 128 个真实阵元坐标"));
    Geometry candidate;
    candidate.name = object["name"].toString();
    candidate.microphones.resize(Channels);
    QSet<int> seen;
    for (const auto &value : points) {
        const auto p = value.toObject();
        const double index = p["channel"].toDouble(-1);
        if (!std::isfinite(index) || index != std::floor(index) || index < 0 || index >= Channels
            || seen.contains(int(index))) return fail(QStringLiteral("通道必须是 0..127 且不得重复"));
        double xyz[3];
        const char *keys[] {"x", "y", "z"};
        for (int axis = 0; axis < 3; ++axis) {
            if (!p[keys[axis]].isDouble()) return fail(QStringLiteral("坐标必须是数值"));
            xyz[axis] = p[keys[axis]].toDouble();
            if (!std::isfinite(xyz[axis]) || std::abs(xyz[axis]) > 2)
                return fail(QStringLiteral("坐标必须有限且在 ±2 米内；请核对单位"));
        }
        for (int previous : seen) {
            const auto &a = candidate.microphones[previous];
            const double dx = a.xMeters - xyz[0], dy = a.yMeters - xyz[1], dz = a.zMeters - xyz[2];
            if (dx * dx + dy * dy + dz * dz < 1e-10) return fail(QStringLiteral("检测到重复阵元位置"));
        }
        candidate.microphones[int(index)] = {xyz[0], xyz[1], xyz[2]};
        seen.insert(int(index));
    }
    // Reject a collinear geometry: two angular coordinates are not observable.
    double maxAreaSquared = 0;
    const auto &origin = candidate.microphones[0];
    for (int i = 1; i < Channels; ++i) {
        const auto &a = candidate.microphones[i];
        for (int j = i + 1; j < Channels; ++j) {
            const auto &b = candidate.microphones[j];
            double ax = a.xMeters - origin.xMeters, ay = a.yMeters - origin.yMeters, az = a.zMeters - origin.zMeters;
            double bx = b.xMeters - origin.xMeters, by = b.yMeters - origin.yMeters, bz = b.zMeters - origin.zMeters;
            double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
            maxAreaSquared = std::max(maxAreaSquared, cx * cx + cy * cy + cz * cz);
        }
    }
    if (maxAreaSquared < 1e-12) return fail(QStringLiteral("阵元共线，不能求解二维方向"));
    if (destination) *destination = candidate;
    if (error) error->clear();
    return true;
}

} // namespace MicArray::Array128
