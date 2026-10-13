#include "fusionworker.h"

#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>

namespace MicArray::Array128 {
namespace {
using Clock = std::chrono::steady_clock;

qint64 elapsedMs(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

qint64 aged(qint64 originalAge, Clock::time_point submitted)
{
    const qint64 elapsed = elapsedMs(submitted);
    if (originalAge < 0 || elapsed < 0 || originalAge > std::numeric_limits<qint64>::max() - elapsed)
        return std::numeric_limits<qint64>::max();
    return originalAge + elapsed;
}

QString invalidConfiguration(const FusionWorkerSettings &settings)
{
    const auto &options = settings.options;
    if (settings.maxSkewMs < 0 || settings.maxSkewMs > 10000 || settings.maxAgeMs < 1 || settings.maxAgeMs > 60000)
        return QStringLiteral("融合时序限制无效：偏差范围 0..10000 ms，时效范围 1..60000 ms");
    if (!std::isfinite(options.confidenceThreshold) || options.confidenceThreshold < 0 || options.confidenceThreshold > 1
        || !std::isfinite(options.activeThreshold) || options.activeThreshold < 0 || options.activeThreshold > 1
        || !std::isfinite(options.nmsIou) || options.nmsIou < 0 || options.nmsIou > 1
        || options.maxDetections < 1 || options.maxDetections > 16)
        return QStringLiteral("融合概率阈值、NMS 或目标数量无效");
    return {};
}

QString timingRejection(const FusionPacket &packet, const FusionWorkerSettings &settings, qint64 currentAge)
{
    if (packet.ageMs < 0) return QStringLiteral("缺少可比的音视频观测时刻，不执行融合");
    // Compare without abs(), which overflows on the minimum signed integer.
    if (packet.audioVideoSkewMs > settings.maxSkewMs || packet.audioVideoSkewMs < -settings.maxSkewMs)
        return QStringLiteral("音视频时间偏差超过 %1 ms，不执行融合").arg(settings.maxSkewMs);
    if (currentAge > settings.maxAgeMs)
        return QStringLiteral("融合观测已超过 %1 ms 时效，不使用旧数据").arg(settings.maxAgeMs);
    return {};
}
}

struct FusionWorker::State
{
    struct Pending
    {
        FusionPacket packet;
        quint64 revision;
        Clock::time_point submitted;
    };

    mutable QMutex mutex;
    QWaitCondition changed;
    std::unique_ptr<QThread> thread;
    FusionWorkerSettings settings;
    FusionWorkerSnapshot status;
    quint64 revision = 0;
    quint64 loadRevision = 0;
    bool stopping = false;
    bool hasSubmittedFrame = false;
    quint64 lastSubmittedFrame = 0;
    std::optional<Pending> pending;
    Clock::time_point resultSubmitted;
    qint64 resultOriginalAge = -1;

    void clearResult()
    {
        status.result = {};
        status.frame = 0;
        status.ageMs = -1;
        status.audioVideoSkewMs = 0;
        resultOriginalAge = -1;
    }

    void invalidateLocked()
    {
        ++revision;
        status.revision = revision;
        status.generation = settings.generation;
        status.run = settings.run;
        if (pending) ++status.discarded;
        pending.reset();
        status.queued = 0;
        hasSubmittedFrame = false;
        status.rejectionReason.clear();
        clearResult();
    }

    void work()
    {
        // Every ORT object is created, used and released only on this thread.
        FusionModel model;
        QString loadedModel, loadedMetadata, loadedRuntime;
        quint64 applied = std::numeric_limits<quint64>::max();
        quint64 loadedRevision = std::numeric_limits<quint64>::max();
        quint64 requestedLoadRevision = 0;
        FusionWorkerSettings current;
        for (;;) {
            std::optional<Pending> packet;
            quint64 token = 0;
            bool configurationChanged = false;
            {
                QMutexLocker lock(&mutex);
                while (!stopping && revision == applied && !pending) changed.wait(&mutex);
                if (stopping) break;
                token = revision;
                configurationChanged = token != applied;
                if (configurationChanged) {
                    current = settings;
                    requestedLoadRevision = loadRevision;
                    applied = token;
                    status.loading = current.enabled;
                    status.ready = false;
                    status.status = current.enabled ? QStringLiteral("加载融合模型") : QStringLiteral("未启用融合推理");
                }
            }
            if (configurationChanged) {
                QString error = current.enabled ? invalidConfiguration(current) : QString {};
                if (!current.enabled) {
                    model.unload();
                    loadedModel.clear(); loadedMetadata.clear(); loadedRuntime.clear();
                    loadedRevision = std::numeric_limits<quint64>::max();
                } else if (error.isEmpty() && (!model.isLoaded() || requestedLoadRevision != loadedRevision || loadedModel != current.modelPath
                           || loadedMetadata != current.metadataPath || loadedRuntime != current.runtimePath)) {
                    if (model.load(current.modelPath, current.metadataPath, current.runtimePath, &error)) {
                        loadedModel = current.modelPath; loadedMetadata = current.metadataPath; loadedRuntime = current.runtimePath;
                        loadedRevision = requestedLoadRevision;
                    }
                }
                QMutexLocker lock(&mutex);
                if (stopping || revision != token) continue;
                status.loading = false;
                status.ready = current.enabled && error.isEmpty() && model.isLoaded();
                status.error = error;
                status.status = !error.isEmpty() ? error : status.ready ? QStringLiteral("模型就绪，等待同步音视频") : QStringLiteral("未启用融合推理");
                status.metadata = status.ready ? model.metadata() : QJsonObject {};
                status.runtimeVersion = status.ready ? model.runtimeVersion() : QString {};
            }
            {
                QMutexLocker lock(&mutex);
                if (stopping || revision != token) continue;
                if (!pending) continue;
                packet = std::move(pending);
                pending.reset(); status.queued = 0;
                if (!current.enabled || !status.ready) {
                    ++status.rejected;
                    status.rejectionReason = QStringLiteral("融合模型未就绪");
                    continue;
                }
                const QString rejection = timingRejection(packet->packet, current, aged(packet->packet.ageMs, packet->submitted));
                if (!rejection.isEmpty()) {
                    ++status.rejected; status.rejectionReason = rejection; status.status = rejection; clearResult(); continue;
                }
                status.busy = true;
                status.status = QStringLiteral("正在执行融合推理");
            }
            FusionResult result = model.infer(packet->packet.video, packet->packet.heat, packet->packet.columns,
                packet->packet.rows, packet->packet.camera, current.options);
            {
                QMutexLocker lock(&mutex);
                status.busy = false;
                ++status.processed;
                if (stopping || revision != packet->revision) { ++status.discarded; continue; }
                const QString rejection = timingRejection(packet->packet, current, aged(packet->packet.ageMs, packet->submitted));
                if (!rejection.isEmpty()) {
                    ++status.rejected; status.rejectionReason = rejection; status.status = rejection; clearResult(); continue;
                }
                status.error = result.error;
                status.result = std::move(result);
                status.status = status.result.valid ? QStringLiteral("融合完成：%1 个候选（仿真域）").arg(status.result.detections.size()) : status.error;
                status.generation = packet->packet.generation; status.run = packet->packet.run; status.frame = packet->packet.frame;
                status.audioVideoSkewMs = packet->packet.audioVideoSkewMs;
                resultSubmitted = packet->submitted; resultOriginalAge = packet->packet.ageMs;
                status.ageMs = aged(resultOriginalAge, resultSubmitted);
                status.rejectionReason.clear();
            }
        }
        model.unload();
        QMutexLocker lock(&mutex);
        status.stopped = true; status.loading = false; status.busy = false; status.ready = false;
        status.status = QStringLiteral("融合线程已关闭");
        status.metadata = {}; status.runtimeVersion.clear(); clearResult();
    }
};

FusionWorker::FusionWorker(QObject *parent) : QObject(parent), m_state(std::make_unique<State>())
{
    State *state = m_state.get();
    state->thread.reset(QThread::create([state] { state->work(); }));
    state->thread->setObjectName(QStringLiteral("Array128FusionInference"));
    state->thread->start();
}

FusionWorker::~FusionWorker()
{
    shutdown();
    m_state->thread->wait(); // Final destruction only; disable()/shutdown() never wait on inference.
}

quint64 FusionWorker::configure(const FusionWorkerSettings &settings)
{
    QMutexLocker lock(&m_state->mutex);
    if (m_state->stopping) return m_state->revision;
    m_state->settings = settings;
    ++m_state->loadRevision; // Explicit configure reloads even a file updated at the same path.
    m_state->invalidateLocked();
    m_state->status.enabled = settings.enabled;
    m_state->status.ready = false;
    m_state->status.loading = settings.enabled;
    m_state->status.error.clear();
    m_state->status.status = settings.enabled ? QStringLiteral("加载融合模型") : QStringLiteral("未启用融合推理");
    m_state->status.metadata = {}; m_state->status.runtimeVersion.clear();
    m_state->changed.wakeOne();
    return m_state->revision;
}

quint64 FusionWorker::invalidate(quint64 generation, quint64 run)
{
    QMutexLocker lock(&m_state->mutex);
    if (m_state->stopping) return m_state->revision;
    m_state->settings.generation = generation; m_state->settings.run = run;
    m_state->invalidateLocked();
    m_state->status.status = m_state->settings.enabled ? QStringLiteral("数据已失效，等待新观测") : QStringLiteral("未启用融合推理");
    m_state->changed.wakeOne();
    return m_state->revision;
}

void FusionWorker::disable()
{
    QMutexLocker lock(&m_state->mutex);
    if (m_state->stopping) return;
    m_state->settings.enabled = false;
    m_state->invalidateLocked();
    m_state->status.enabled = false; m_state->status.ready = false; m_state->status.loading = false;
    m_state->status.status = QStringLiteral("未启用融合推理");
    m_state->status.error.clear(); m_state->status.metadata = {}; m_state->status.runtimeVersion.clear();
    m_state->changed.wakeOne();
}

bool FusionWorker::submit(FusionPacket packet)
{
    QMutexLocker lock(&m_state->mutex);
    auto &status = m_state->status;
    if (m_state->stopping || !m_state->settings.enabled) return false;
    const QString configurationError = invalidConfiguration(m_state->settings);
    if (!configurationError.isEmpty()) {
        ++status.rejected; status.rejectionReason = configurationError; return false;
    }
    if (packet.generation != m_state->settings.generation || packet.run != m_state->settings.run) {
        ++status.rejected; status.rejectionReason = QStringLiteral("丢弃旧采集批次或旧配置的融合数据"); return false;
    }
    if (m_state->hasSubmittedFrame && packet.frame <= m_state->lastSubmittedFrame) {
        ++status.rejected; status.rejectionReason = QStringLiteral("忽略重复或倒序的声学帧"); return false;
    }
    const QString rejection = timingRejection(packet, m_state->settings, packet.ageMs);
    if (!rejection.isEmpty()) {
        ++status.rejected; status.rejectionReason = rejection; status.status = rejection; return false;
    }
    m_state->hasSubmittedFrame = true;
    m_state->lastSubmittedFrame = packet.frame;
    ++status.submitted;
    if (m_state->pending) ++status.replaced;
    m_state->pending = State::Pending {std::move(packet), m_state->revision, Clock::now()};
    status.queued = 1;
    if (!status.loading && !status.busy) status.status = QStringLiteral("等待融合推理");
    m_state->changed.wakeOne();
    return true;
}

FusionWorkerSnapshot FusionWorker::snapshot() const
{
    QMutexLocker lock(&m_state->mutex);
    auto snapshot = m_state->status;
    if (snapshot.result.valid) {
        snapshot.ageMs = aged(m_state->resultOriginalAge, m_state->resultSubmitted);
        if (snapshot.ageMs > m_state->settings.maxAgeMs) {
            snapshot.result = {};
            snapshot.rejectionReason = QStringLiteral("融合结果已过期，等待新观测");
            snapshot.status = snapshot.rejectionReason;
        }
    }
    return snapshot;
}

void FusionWorker::shutdown()
{
    QMutexLocker lock(&m_state->mutex);
    if (m_state->stopping) return;
    m_state->stopping = true;
    m_state->settings.enabled = false;
    m_state->invalidateLocked();
    m_state->status.enabled = false; m_state->status.ready = false; m_state->status.loading = false;
    m_state->status.status = QStringLiteral("正在关闭融合线程");
    m_state->changed.wakeOne();
}

bool FusionWorker::waitForStopped(unsigned long timeoutMs)
{
    return m_state->thread->wait(timeoutMs);
}

} // namespace MicArray::Array128
