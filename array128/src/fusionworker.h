#pragma once

#include "fusionmodel.h"

#include <QObject>

#include <memory>

namespace MicArray::Array128 {

struct FusionWorkerSettings
{
    bool enabled = false;
    QString modelPath;
    QString metadataPath;
    QString runtimePath;
    FusionOptions options;
    qint64 maxSkewMs = 100;
    qint64 maxAgeMs = 500;
    quint64 generation = 0;
    quint64 run = 0;
};

struct FusionPacket
{
    QImage video;
    QVector<float> heat;
    int columns = 0;
    int rows = 0;
    CameraViewSettings camera;
    quint64 generation = 0;
    quint64 run = 0;
    quint64 frame = 0;
    qint64 audioVideoSkewMs = 0; // Signed host-arrival difference; not device synchronization.
    qint64 ageMs = -1; // Age of the older observation at submit(), computed by the caller on ONE clock.
};

struct FusionWorkerSnapshot
{
    bool enabled = false;
    bool ready = false;
    bool loading = false;
    bool busy = false;
    bool stopped = false;
    QString status = QStringLiteral("未启用融合推理");
    QString error;
    QString rejectionReason;
    QString runtimeVersion;
    QJsonObject metadata;
    quint64 revision = 0;
    quint64 submitted = 0;
    quint64 processed = 0;
    quint64 replaced = 0;
    quint64 rejected = 0;
    quint64 discarded = 0;
    int queued = 0;
    FusionResult result;
    quint64 generation = 0;
    quint64 run = 0;
    quint64 frame = 0;
    qint64 audioVideoSkewMs = 0;
    qint64 ageMs = -1;
};

// Bounded latest-packet CPU worker. All public operations except waitForStopped
// and final destruction are non-blocking with respect to model load/inference.
// snapshot() also invalidates expired results without needing an incoming frame.
class FusionWorker final : public QObject
{
    Q_OBJECT
public:
    explicit FusionWorker(QObject *parent = nullptr);
    ~FusionWorker() override;
    quint64 configure(const FusionWorkerSettings &settings);
    quint64 invalidate(quint64 generation, quint64 run);
    void disable();
    bool submit(FusionPacket packet);
    FusionWorkerSnapshot snapshot() const;
    void shutdown();
    bool waitForStopped(unsigned long timeoutMs = 2000);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace MicArray::Array128
