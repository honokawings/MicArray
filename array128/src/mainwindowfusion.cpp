#include "mainwindow.h"

#include <QCamera>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QTableWidget>

#include <algorithm>
#include <cmath>

namespace MicArray::Array128 {

void MainWindow::configureFusion()
{
    if (m_loading) return;
    resetFusion();
    FusionWorkerSettings settings;
    settings.enabled = m_fusionEnabled->isChecked();
    settings.modelPath = m_fusionModelPath->text().trimmed();
    settings.metadataPath = m_fusionMetadataPath->text().trimmed();
    settings.runtimePath = m_fusionRuntimePath->text().trimmed();
    settings.options.confidenceThreshold = m_fusionConfidence->value();
    settings.options.activeThreshold = m_fusionActive->value();
    settings.maxSkewMs = m_fusionSkew->value();
    settings.generation = m_processing.generation; settings.run = m_pipeline.snapshot().runs;
    m_fusion.configure(settings);
    if (!m_skipSettingsSave) saveSettings();
}

void MainWindow::resetFusion()
{
    m_fusionVideo.clear(); m_fusionDisplayFrames.clear();
    m_fusionRun = m_pipeline.snapshot().runs; m_fusionSubmittedFrame = 0; m_fusionPaintedFrame = 0;
    m_fusion.invalidate(m_processing.generation, m_fusionRun);
    if (m_localView) m_localView->setFusionDetections({});
    if (m_fusionTable) m_fusionTable->setRowCount(0);
}

void MainWindow::refreshFusion(const PipelineSnapshot &state)
{
    auto snapshot = m_fusion.snapshot();
    if (m_fusionRun != state.runs || snapshot.generation != m_processing.generation) {
        resetFusion(); snapshot = m_fusion.snapshot();
    }
    m_fusionGate.clear();
    if (!m_fusionEnabled->isChecked()) m_fusionGate = QStringLiteral("未启用融合推理");
    else if (!snapshot.ready) m_fusionGate = snapshot.error.isEmpty() ? snapshot.status : snapshot.error;
    else m_fusionGate = fusionDomainError(snapshot.metadata, m_processing);
    if (m_fusionGate.isEmpty()) {
        if (!m_processing.localization) m_fusionGate = QStringLiteral("请先启用 SRP 声源定位");
        else if (!state.listening || state.lastPacketAgeMs < 0 || state.lastPacketAgeMs >= 500)
            m_fusionGate = QStringLiteral("等待及时、有效的实采音频");
        else if (state.flags & 0x10) m_fusionGate = QStringLiteral("自测数据禁止融合定位");
        else if (state.acousticFaultFlags()) m_fusionGate = state.acousticFaultReason();
        else if (!m_camera || !m_camera->isActive()) m_fusionGate = QStringLiteral("需要实时视频；静态图像不参与同步融合");
    }

    if (m_fusionGate.isEmpty() && (state.localization.generation != m_processing.generation
        || state.localization.run != state.runs || state.localization.heat.isEmpty()))
        m_fusionGate = QStringLiteral("等待当前设置对应的 SRP 响应");
    if (m_fusionGate.isEmpty() && state.localization.validChannels < 112)
        m_fusionGate = QStringLiteral("实际有效通道少于训练域的 112 路；SRP 可继续独立工作");

    if (m_fusionGate.isEmpty() && state.localization.lastFrame != m_fusionSubmittedFrame) {
        const auto paired = m_fusionVideo.nearest(state.localization.observationTimeNs, m_pipeline.monotonicNowNs(), m_fusionSkew->value());
        m_fusionGate = paired.error;
        if (paired.error.isEmpty()) {
            FusionPacket packet;
            packet.video = paired.image; packet.heat = state.localization.heat;
            packet.columns = state.localization.columns; packet.rows = state.localization.rows; packet.camera = m_processing.camera;
            packet.generation = m_processing.generation; packet.run = state.runs; packet.frame = state.localization.lastFrame;
            // Round conservatively; do not let fractional milliseconds weaken rejection.
            packet.ageMs = qint64(std::ceil(paired.ageMs));
            packet.audioVideoSkewMs = qint64(std::copysign(std::ceil(std::abs(paired.skewMs)), paired.skewMs));
            if (m_fusion.submit(std::move(packet))) {
                m_fusionSubmittedFrame = state.localization.lastFrame;
                m_fusionDisplayFrames.store(paired.image, state.localization);
            }
        }
    }
    if (!m_fusionGate.isEmpty() && (snapshot.result.valid || snapshot.busy || snapshot.queued)) {
        m_fusion.invalidate(m_processing.generation, state.runs);
        m_fusionSubmittedFrame = 0;
    }
    snapshot = m_fusion.snapshot();
    const auto matched = m_fusionDisplayFrames.find(snapshot.generation, snapshot.run, snapshot.frame);
    const bool current = m_fusionGate.isEmpty() && snapshot.result.valid && matched.has_value()
        && snapshot.generation == m_processing.generation && snapshot.run == state.runs;
    if (current) {
        if (m_fusionPaintedFrame != snapshot.frame) {
            m_localView->setImage(matched->video);
            m_localView->setResponse(matched->localization, m_focus->value());
            m_localView->setFusionDetections(snapshot.result.detections);
            m_fusionPaintedFrame = snapshot.frame;
            m_fusionTable->setRowCount(snapshot.result.detections.size());
            for (int row = 0; row < snapshot.result.detections.size(); ++row) {
                const auto &d = snapshot.result.detections[row];
                const QStringList values {QStringLiteral("Y%1").arg(row + 1), QString::number(d.direction.azimuthDegrees, 'f', 2),
                    QString::number(d.direction.elevationDegrees, 'f', 2), QString::number(d.confidence, 'f', 3), QString::number(d.activeProbability, 'f', 3)};
                for (int col = 0; col < values.size(); ++col) {
                    auto *item = m_fusionTable->item(row, col);
                    if (!item) { item = new QTableWidgetItem; m_fusionTable->setItem(row, col, item); }
                    item->setText(values[col]); item->setToolTip(QStringLiteral("仿真候选目标；Y 编号为本帧排序，不是跟踪 ID。源点由神经网络独立回归。"));
                }
            }
        }
    } else if (m_fusionPaintedFrame) {
        m_fusionPaintedFrame = 0; m_fusionTable->setRowCount(0); m_localView->setFusionDetections({});
        m_localView->setResponse(state.localization, m_focus->value());
        const qint64 now = m_pipeline.monotonicNowNs(); const auto latest = m_fusionVideo.nearest(now, now);
        if (!latest.image.isNull()) m_localView->setImage(latest.image);
    }
    if (!m_fusionGate.isEmpty()) m_fusionStatus->setText(m_fusionGate);
    else if (current) m_fusionStatus->setText(QStringLiteral("仿真模型 · %1 个发声候选 · %2 ms\n配对时差 %3 ms · 帧龄 %4 ms · 替换 %5")
        .arg(snapshot.result.detections.size()).arg(snapshot.result.elapsedMs, 0, 'f', 1).arg(snapshot.audioVideoSkewMs).arg(snapshot.ageMs).arg(snapshot.replaced));
    else if (snapshot.result.valid && !matched)
        m_fusionStatus->setText(QStringLiteral("配对显示帧已失效，等待新结果"));
    else m_fusionStatus->setText(snapshot.error.isEmpty() ? snapshot.status : snapshot.error);
    m_fusionStatus->setToolTip(m_fusionStatus->text());
}

QJsonObject MainWindow::fusionDiagnostics() const
{
    const auto state = m_fusion.snapshot(); QJsonArray detections;
    const bool displayed = state.result.valid && state.frame == m_fusionPaintedFrame && m_fusionGate.isEmpty();
    if (displayed) for (const auto &d : state.result.detections) detections.append(QJsonObject {
        {"azimuth_deg", d.direction.azimuthDegrees}, {"elevation_deg", d.direction.elevationDegrees},
        {"source_u", d.sourcePoint.x()}, {"source_v", d.sourcePoint.y()}, {"confidence", d.confidence}, {"active_probability", d.activeProbability}});
    return {{"enabled", state.enabled}, {"ready", state.ready}, {"current_result", displayed},
        {"domain", "simulation_only"}, {"hardware_synchronized", false}, {"gate", m_fusionGate}, {"error", state.error},
        {"model_sha256", state.metadata["model_sha256"]}, {"runtime", state.runtimeVersion},
        {"generation", QString::number(state.generation)}, {"run", QString::number(state.run)}, {"frame", QString::number(state.frame)},
        {"frame_age_ms", state.ageMs}, {"audio_video_skew_ms", state.audioVideoSkewMs},
        {"submitted", QString::number(state.submitted)}, {"processed", QString::number(state.processed)},
        {"replaced", QString::number(state.replaced)}, {"rejected", QString::number(state.rejected)},
        {"discarded", QString::number(state.discarded)}, {"detections", detections}};
}

} // namespace MicArray::Array128
