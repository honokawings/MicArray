#include "monitoring/healthmonitor.h"

namespace MicArray {

HealthSnapshot HealthMonitor::update(const HealthInputs &inputs, qint64 monotonicMs)
{
    HealthSnapshot snapshot;
    snapshot.lastReceiveAgeMs = inputs.lastReceiveAgeMs;
    snapshot.backpressureActive = inputs.backpressureActive;
    snapshot.pendingProtocolBytes = inputs.pendingProtocolBytes;
    snapshot.audioQueueDepth = inputs.audioQueueDepth;
    snapshot.audioDroppedFrames = inputs.audioDroppedFrames;
    snapshot.videoDroppedFrames = inputs.videoDroppedFrames;

    if (m_initialized && monotonicMs > m_lastUpdateMs) {
        const double seconds = static_cast<double>(monotonicMs - m_lastUpdateMs) / 1000.0;
        if (inputs.totalBytes >= m_lastBytes) {
            snapshot.receiveBytesPerSecond = static_cast<double>(inputs.totalBytes - m_lastBytes) / seconds;
        }
        if (inputs.totalFrames >= m_lastFrames) {
            snapshot.framesPerSecond = static_cast<double>(inputs.totalFrames - m_lastFrames) / seconds;
        }
        if (inputs.parseErrors >= m_lastParseErrors) {
            snapshot.parseErrorDelta = inputs.parseErrors - m_lastParseErrors;
        }
    }

    m_initialized = true;
    m_lastUpdateMs = monotonicMs;
    m_lastBytes = inputs.totalBytes;
    m_lastFrames = inputs.totalFrames;
    m_lastParseErrors = inputs.parseErrors;

    if (!inputs.connected) {
        snapshot.level = HealthLevel::Disconnected;
        snapshot.reason = QStringLiteral("设备未连接");
    } else if (inputs.lastReceiveAgeMs >= CriticalStaleMs) {
        snapshot.level = HealthLevel::Critical;
        snapshot.reason = QStringLiteral("数据接收超时");
    } else if (inputs.lastReceiveAgeMs >= WarningStaleMs) {
        snapshot.level = HealthLevel::Warning;
        snapshot.reason = QStringLiteral("数据接收延迟");
    } else if (inputs.backpressureActive) {
        snapshot.level = HealthLevel::Warning;
        snapshot.reason = QStringLiteral("接收背压已启用");
    } else if (snapshot.parseErrorDelta > 0) {
        snapshot.level = HealthLevel::Warning;
        snapshot.reason = QStringLiteral("检测到新的协议错误");
    } else {
        snapshot.level = HealthLevel::Healthy;
        snapshot.reason = QStringLiteral("运行正常");
    }
    return snapshot;
}

void HealthMonitor::reset()
{
    m_initialized = false;
    m_lastUpdateMs = 0;
    m_lastBytes = 0;
    m_lastFrames = 0;
    m_lastParseErrors = 0;
}

QString healthLevelName(HealthLevel level)
{
    switch (level) {
    case HealthLevel::Disconnected:
        return QStringLiteral("未连接");
    case HealthLevel::Healthy:
        return QStringLiteral("健康");
    case HealthLevel::Warning:
        return QStringLiteral("警告");
    case HealthLevel::Critical:
        return QStringLiteral("严重");
    }
    return QStringLiteral("未知");
}

} // namespace MicArray
