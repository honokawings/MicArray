#pragma once

#include <QString>

namespace MicArray {

enum class HealthLevel {
    Disconnected,
    Healthy,
    Warning,
    Critical,
};

struct HealthInputs {
    bool connected = false;
    qint64 lastReceiveAgeMs = 0;
    quint64 totalBytes = 0;
    quint64 totalFrames = 0;
    quint64 parseErrors = 0;
    bool backpressureActive = false;
    qint64 pendingProtocolBytes = 0;
    int audioQueueDepth = 0;
    quint64 audioDroppedFrames = 0;
    quint64 videoDroppedFrames = 0;
};

struct HealthSnapshot {
    HealthLevel level = HealthLevel::Disconnected;
    QString reason;
    qint64 lastReceiveAgeMs = 0;
    double receiveBytesPerSecond = 0.0;
    double framesPerSecond = 0.0;
    quint64 parseErrorDelta = 0;
    bool backpressureActive = false;
    qint64 pendingProtocolBytes = 0;
    int audioQueueDepth = 0;
    quint64 audioDroppedFrames = 0;
    quint64 videoDroppedFrames = 0;
};

class HealthMonitor
{
public:
    static constexpr qint64 WarningStaleMs = 2000;
    static constexpr qint64 CriticalStaleMs = 5000;

    HealthSnapshot update(const HealthInputs &inputs, qint64 monotonicMs);
    void reset();

private:
    bool m_initialized = false;
    qint64 m_lastUpdateMs = 0;
    quint64 m_lastBytes = 0;
    quint64 m_lastFrames = 0;
    quint64 m_lastParseErrors = 0;
};

QString healthLevelName(HealthLevel level);

} // namespace MicArray
