#include "media/noisereductionmailbox.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>

#include <algorithm>

namespace MicArray {

NoiseReductionMailbox::NoiseReductionMailbox(QObject *parent) : QObject(parent) {}

bool NoiseReductionMailbox::enqueue(Kind kind, int samples,
                                    std::function<void()> operation,
                                    quint32 firstSampleIndex)
{
    bool wake = false;
    {
        const QMutexLocker locker(&m_mutex);
        const bool control = kind == Kind::Authorization || kind == Kind::Control;
        if (m_state.closing || !operation || (control ? samples != 0 : samples <= 0)
            || (!control && samples > 200)
            || (kind == Kind::Pl && m_state.plBufferedSamples + samples > MaximumPlSamples)
            || (kind == Kind::Reference && m_state.referenceBufferedSamples + samples > MaximumReferenceSamples)
            || (kind == Kind::Software && m_state.softwareFrames >= MaximumSoftwareFrames)
            || (control && m_state.controls >= MaximumControls)) return false;
        m_queue.push_back({kind, samples, firstSampleIndex, m_state.generation,
                           Clock::now(), std::move(operation)});
        if (kind == Kind::Pl) {
            m_state.plBufferedSamples += samples;
            m_state.plQueuedSamples += samples;
            m_state.plPeakSamples = std::max(m_state.plPeakSamples, m_state.plBufferedSamples);
            ++m_state.acceptedPlFrames;
        } else if (kind == Kind::Reference) {
            m_state.referenceBufferedSamples += samples;
            m_state.referenceQueuedSamples += samples;
            m_state.referencePeakSamples = std::max(m_state.referencePeakSamples, m_state.referenceBufferedSamples);
        } else if (kind == Kind::Software) ++m_state.softwareFrames;
        if (control) {
            ++m_state.controls;
            if (kind == Kind::Authorization) ++m_state.queuedAuthorizations;
        } else {
            ++m_state.queuedAudioFrames;
            ++m_state.submittedAudioFrames;
        }
        if (!m_state.scheduled) {
            m_state.scheduled = true;
            wake = true;
        }
    }
    return !wake || postWake();
}

bool NoiseReductionMailbox::postWake()
{
    if (QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection)) return true;
    const QMutexLocker locker(&m_mutex);
    m_state.scheduled = false;
    ++m_state.wakeFailures;
    return false;
}

NoiseReductionMailbox::Snapshot NoiseReductionMailbox::snapshot() const
{
    const QMutexLocker locker(&m_mutex);
    return m_state;
}

void NoiseReductionMailbox::release(const Work &work, bool processed)
{
    if (work.kind == Kind::Pl) {
        m_state.plBufferedSamples -= work.samples;
        if (processed) {
            ++m_state.completedPlFrames;
            m_state.processedPlSamples += work.samples;
            m_state.lastProcessedSampleIndex = work.first + quint32(work.samples);
        }
    } else if (work.kind == Kind::Reference) m_state.referenceBufferedSamples -= work.samples;
    else if (work.kind == Kind::Software) --m_state.softwareFrames;
    else --m_state.controls;
    if (processed) ++m_state.processedTasks;
    else ++m_state.discardedTasks;
}

void NoiseReductionMailbox::clearPending(bool discardControls)
{
    const QMutexLocker locker(&m_mutex);
    // Configuration/active/recording/invalidation controls survive cancellation:
    // dropping them could leave the worker in the preceding mode. They retain
    // FIFO order before newly submitted work. The executing item is untouched.
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        if (it->kind == Kind::Control && !discardControls) { ++it; continue; }
        if (it->kind == Kind::Pl) m_state.plQueuedSamples -= it->samples;
        else if (it->kind == Kind::Reference) m_state.referenceQueuedSamples -= it->samples;
        if (it->kind == Kind::Authorization) --m_state.queuedAuthorizations;
        else if (it->kind != Kind::Control) --m_state.queuedAudioFrames;
        release(*it, false);
        it = m_queue.erase(it);
    }
    ++m_state.generation;
}

void NoiseReductionMailbox::close()
{
    const QMutexLocker locker(&m_mutex);
    m_state.closing = true;
    for (const auto &work : m_queue) release(work, false);
    m_queue.clear();
    m_state.plQueuedSamples = 0;
    m_state.referenceQueuedSamples = 0;
    m_state.queuedAudioFrames = 0;
    m_state.queuedAuthorizations = 0;
    ++m_state.generation;
}

void NoiseReductionMailbox::drain()
{
    Q_ASSERT(QThread::currentThread() == thread());
    QElapsedTimer batch;
    batch.start();
    for (int count = 0; count < 16; ++count) {
        Work work;
        {
            const QMutexLocker locker(&m_mutex);
            if (m_state.closing || m_queue.empty()) {
                // Clearing wake ownership and observing emptiness use the same
                // mutex as ingress, so enqueue cannot lose an idle transition.
                m_state.scheduled = false;
                return;
            }
            work = std::move(m_queue.front());
            m_queue.pop_front();
            const bool control = work.kind == Kind::Control || work.kind == Kind::Authorization;
            if (work.kind == Kind::Pl) m_state.plQueuedSamples -= work.samples;
            else if (work.kind == Kind::Reference) m_state.referenceQueuedSamples -= work.samples;
            if (work.kind == Kind::Authorization) --m_state.queuedAuthorizations;
            if (!control) {
                --m_state.queuedAudioFrames;
            }
            m_state.inFlight = 1;
            m_state.controlInFlight = control ? 1 : 0;
            m_state.plInFlight = work.kind == Kind::Pl ? 1 : 0;
            const auto residence = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - work.queued).count();
            m_state.queueResidenceMaxUsec = std::max(m_state.queueResidenceMaxUsec, qint64(residence));
        }
        const auto started = Clock::now();
        work.operation(); // No mutex around DSP, monitor writes or signal emission.
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
        {
            const QMutexLocker locker(&m_mutex);
            release(work, true); // Actual completion returns capacity on this thread.
            m_state.inFlight = 0;
            m_state.controlInFlight = 0;
            m_state.plInFlight = 0;
            m_state.processMaxUsec = std::max(m_state.processMaxUsec, qint64(elapsed));
        }
        if (batch.elapsed() >= 2) break;
    }
    {
        const QMutexLocker locker(&m_mutex);
        if (m_state.closing || m_queue.empty()) {
            m_state.scheduled = false;
            return;
        }
    }
    // Keep wake ownership while posting our own continuation. GUI delivery and
    // frameConsumed signals are irrelevant to the next batch.
    postWake();
}

} // namespace MicArray
