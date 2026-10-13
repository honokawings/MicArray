#include "fusioncontext.h"

#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <limits>

namespace MicArray::Array128 {

void FusionVideoBuffer::append(const QImage &image, qint64 arrivalNs)
{
    if (image.isNull() || arrivalNs < 0) return;
    if (!m_frames.empty() && arrivalNs <= m_frames.back().arrivalNs) return;
    // Preserve pixels: the model owns the exact half-pixel preprocessing. A Qt
    // thumbnail here would apply a different resampler before inference.
    const auto rgb = image.convertToFormat(QImage::Format_RGB888);
    constexpr qsizetype byteLimit = 64 * 1024 * 1024;
    if (rgb.sizeInBytes() > byteLimit) return;
    m_frames.push_back({rgb, arrivalNs}); m_bytes += rgb.sizeInBytes();
    while (m_frames.size() > 32 || m_bytes > byteLimit || arrivalNs - m_frames.front().arrivalNs > 600000000ll) {
        m_bytes -= m_frames.front().image.sizeInBytes(); m_frames.pop_front();
    }
}

void FusionVideoBuffer::clear() { m_frames.clear(); m_bytes = 0; }

PairedVideo FusionVideoBuffer::nearest(qint64 audioObservationNs, qint64 nowNs, double maxSkewMs, double maxAgeMs) const
{
    PairedVideo result;
    if (audioObservationNs < 0 || nowNs < audioObservationNs || !std::isfinite(maxSkewMs) || maxSkewMs < 0
        || !std::isfinite(maxAgeMs) || maxAgeMs <= 0) {
        result.error = QStringLiteral("音视频观测时间无效"); return result;
    }
    const Frame *best = nullptr;
    qint64 distance = std::numeric_limits<qint64>::max();
    for (const auto &frame : m_frames) {
        if (frame.arrivalNs > nowNs) continue;
        const qint64 delta = std::abs(frame.arrivalNs - audioObservationNs);
        if (delta < distance) { distance = delta; best = &frame; }
    }
    if (!best) { result.error = QStringLiteral("等待实时视频；静态图像不参与同步融合"); return result; }
    result.skewMs = double(best->arrivalNs - audioObservationNs) / 1e6;
    result.ageMs = double(nowNs - std::min(best->arrivalNs, audioObservationNs)) / 1e6;
    if (std::abs(result.skewMs) > maxSkewMs) { result.error = QStringLiteral("音视频时间差超限"); return result; }
    if (result.ageMs > maxAgeMs) { result.error = QStringLiteral("音视频输入已过期"); return result; }
    result.image = best->image;
    return result;
}

QString fusionDomainError(const QJsonObject &metadata, const ProcessingSettings &settings)
{
    const auto coordinates = metadata["geometry_coordinates_m"].toArray();
    if (!settings.geometry.valid() || coordinates.size() != 128)
        return QStringLiteral("模型缺少 128 路训练坐标契约，或尚未配置阵列");
    for (int ch = 0; ch < 128; ++ch) {
        const auto xyz = coordinates[ch].toArray();
        const auto &p = settings.geometry.microphones[ch];
        const double actual[] {p.xMeters, p.yMeters, p.zMeters};
        if (xyz.size() != 3) return QStringLiteral("模型训练坐标格式错误");
        for (int axis = 0; axis < 3; ++axis) {
            const double expected = xyz[axis].toDouble(std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(expected) || !std::isfinite(actual[axis]) || std::abs(expected - actual[axis]) > 1e-8)
                return QStringLiteral("CH%1 坐标与模型训练阵列不符；请使用匹配模型").arg(ch);
        }
    }
    if (metadata["training_srp_grid"].toArray() != QJsonArray {72, 45} || settings.gridColumns != 72 || settings.gridRows != 45)
        return QStringLiteral("SRP 网格与模型训练规格不符（本模型使用 72 × 45）");
    const auto srp = metadata["training_srp_settings"].toObject();
    if (srp["max_sources"].toInt(-1) != 4 || srp["minimum_source_separation_deg"].toDouble(-1) != 4.0
        || srp["sound_speed_m_s"].toDouble(-1) != 343.0 || srp["window_samples"].toInt(-1) != 2048)
        return QStringLiteral("模型缺少匹配的 SRP 搜索参数契约");
    if (settings.maxSources != 4 || !std::isfinite(settings.minimumSourceSeparationDegrees)
        || std::abs(settings.minimumSourceSeparationDegrees - 4.0) > 1e-6)
        return QStringLiteral("融合模型要求 SRP 最多 4 个目标、间隔 4°；可点击“模型建议”");
    if (!std::isfinite(settings.soundSpeed) || std::abs(settings.soundSpeed - 343.0) > .001)
        return QStringLiteral("仿真模型只验证了 343 m/s 声速，请使用匹配设置");
    if (std::count(settings.disabledChannels.cbegin(), settings.disabledChannels.cend(), true) > 16)
        return QStringLiteral("屏蔽通道超过仿真模型验证的 16 路上限；SRP 可继续独立工作");
    const auto &c = settings.camera;
    if (!std::isfinite(c.centerAzimuthDegrees) || !std::isfinite(c.centerElevationDegrees) || !std::isfinite(c.horizontalFovDegrees)
        || !std::isfinite(c.verticalFovDegrees) || !std::isfinite(c.rollDegrees)
        || c.centerAzimuthDegrees < -8 || c.centerAzimuthDegrees > 8 || c.centerElevationDegrees < -5 || c.centerElevationDegrees > 5
        || c.horizontalFovDegrees < 75 || c.horizontalFovDegrees > 100 || c.verticalFovDegrees < 48 || c.verticalFovDegrees > 67
        || c.rollDegrees < -5 || c.rollDegrees > 5)
        return QStringLiteral("当前视角超出仿真训练范围；SRP 可继续独立工作");
    return {};
}

void FusionDisplayBuffer::store(const QImage &video, const LocalizationResult &localization)
{
    if (video.isNull()) return;
    const QImage preview = video.width() > 1280 || video.height() > 720
        ? video.scaled(1280, 720, Qt::KeepAspectRatio, Qt::SmoothTransformation) : video;
    m_frames.push_back({preview, localization});
    while (m_frames.size() > 8) m_frames.pop_front();
}

void FusionDisplayBuffer::clear() { m_frames.clear(); }

std::optional<FusionDisplayContext> FusionDisplayBuffer::find(quint64 generation, quint64 run, quint64 frame) const
{
    for (const auto &context : m_frames) {
        const auto &result = context.localization;
        if (result.generation == generation && result.run == run && result.lastFrame == frame) return context;
    }
    return std::nullopt;
}

} // namespace MicArray::Array128
