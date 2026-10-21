#pragma once

#include <QObject>
#include <QMutex>
#include <QtGlobal>

#include <chrono>
#include <deque>
#include <functional>

namespace MicArray {

// Thread-safe ingress, worker-thread execution. GUI notifications never return
// capacity or wake the consumer. Every control and audio operation shares FIFO
// order; only explicit epoch changes discard pending audio/authorizations.
class NoiseReductionMailbox : public QObject
{
public:
    enum class Kind { Pl, Reference, Software, Authorization, Control };
    struct Snapshot {
        qint64 plBufferedSamples = 0;
        qint64 plQueuedSamples = 0;
        qint64 referenceBufferedSamples = 0;
        qint64 referenceQueuedSamples = 0;
        qint64 plPeakSamples = 0;
        qint64 referencePeakSamples = 0;
        int softwareFrames = 0;
        int queuedAudioFrames = 0;
        int queuedAuthorizations = 0;
        int controls = 0;
        int inFlight = 0;
        int controlInFlight = 0;
        int plInFlight = 0;
        quint64 generation = 0;
        quint64 acceptedPlFrames = 0;
        quint64 completedPlFrames = 0;
        quint64 submittedAudioFrames = 0;
        // Completed worker slot invocations, process-lifetime cumulative. This
        // is scheduling telemetry, not proof that DSP accepted a valid stream.
        quint64 processedPlSamples = 0;
        quint64 processedTasks = 0;
        quint64 discardedTasks = 0;
        quint64 wakeFailures = 0;
        qint64 queueResidenceMaxUsec = 0;
        qint64 processMaxUsec = 0;
        quint32 lastProcessedSampleIndex = 0;
        bool scheduled = false;
        bool closing = false;
    };

    static constexpr int MaximumPlSamples = 96000;
    static constexpr int MaximumReferenceSamples = 96000;
    static constexpr int MaximumSoftwareFrames = 32;
    static constexpr int MaximumControls = 64;

    explicit NoiseReductionMailbox(QObject *parent = nullptr);
    bool enqueue(Kind kind, int samples, std::function<void()> operation,
                 quint32 firstSampleIndex = 0);
    Snapshot snapshot() const;
    void clearPending(bool discardControls = false);
    void close();

private:
    using Clock = std::chrono::steady_clock;
    struct Work {
        Kind kind;
        int samples;
        quint32 first;
        quint64 generation;
        Clock::time_point queued;
        std::function<void()> operation;
    };
    void drain();
    void release(const Work &work, bool processed);
    bool postWake();

    mutable QMutex m_mutex;
    std::deque<Work> m_queue;
    Snapshot m_state;
};

} // namespace MicArray
