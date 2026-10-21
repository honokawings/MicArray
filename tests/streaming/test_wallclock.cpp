#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#ifdef R2_MAILBOX
#include "media/noisereductionmailbox.h"
#endif
#include "test_helpers.h"
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <chrono>
#include <mutex>
#include <thread>

using Clock = std::chrono::steady_clock;
namespace
{
quint64 workerCpuTicks()
{
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return 0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
}

QVector<float> reference(int stage, int samples)
{
    QVector<float> input;
    input.reserve(samples);
    for (int sample = 0; sample < samples; ++sample)
        input.append(float(1 + quint32(sample) % 4093) / 65536.0f);
    if (stage == 1)
    {
        const auto config = settingsFor(stage);
        WienerNoiseReducer processor;
        processor.setAlgorithm(config.algorithm);
        processor.setMode(config.mode);
        processor.setSuppressionCoefficients(config.singleSuppressionCoefficient,
            config.dualB1SuppressionCoefficient, config.dualB2SuppressionCoefficient);
        input = processor.process(input, QVector<float>(samples, 0), QVector<float>(samples, 0));
    }
    return input.first(input.size() / 200 * 200);
}

struct Metrics
{
    std::atomic<int> produced {0}, ingressed {0}, completedJobs {0}, waitingReceipts {0};
    std::atomic<quint64> maxProducerLateUsec {0};
    std::atomic<bool> stop {false};
    std::mutex outputMutex;
    QVector<float> output;
    quint64 cpuStart = 0, cpuEnd = 0;
};

void maximum(std::atomic<quint64> &destination, quint64 value)
{
    quint64 current = destination.load();
    while (value > current && !destination.compare_exchange_weak(current, value)) {}
}

qint64 buffered(NoiseReductionPage &page)
{
#ifdef R2_MAILBOX
    return page.m_mailbox->snapshot().plBufferedSamples;
#else
    return page.property("plBufferedSamples").toLongLong();
#endif
}

qint64 referenceBuffered(NoiseReductionPage &page)
{
#ifdef R2_MAILBOX
    return page.m_mailbox->snapshot().referenceBufferedSamples;
#else
    return page.property("rawReferenceBufferedSamples").toLongLong();
#endif
}

double queueSlope(const QVector<QPair<double, qint64>> &points)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int count = 0;
    for (const auto &point : points)
    {
        if (point.first < 2.0 || point.first > 9.5) continue;
        ++count;
        sx += point.first; sy += point.second;
        sxx += point.first * point.first; sxy += point.first * point.second;
    }
    const double denominator = count * sxx - sx * sx;
    return denominator > 0 ? (count * sxy - sx * sy) / denominator : 0;
}
}

class WallclockTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void page48kTenSeconds_data();
    void page48kTenSeconds();
    void delayedCompletion50ms_data();
    void delayedCompletion50ms();
    void withheldCompletion_data();
    void withheldCompletion();
#ifdef R2_MAILBOX
    void schedulerConsumesWhileGuiFrozen_data();
    void schedulerConsumesWhileGuiFrozen();
#endif
private:
    void runPage(int stage, int receiptDelayMs, int durationMs);
};

void WallclockTest::page48kTenSeconds_data()
{
    QTest::addColumn<int>("stage");
    QTest::newRow("AUD3") << 1;
    QTest::newRow("AUW1") << 2;
    QTest::newRow("AUF1") << 3;
}
void WallclockTest::page48kTenSeconds()
{
    QFETCH(int, stage); runPage(stage, -1, 10000);
}
void WallclockTest::delayedCompletion50ms_data() { page48kTenSeconds_data(); }
void WallclockTest::delayedCompletion50ms()
{
    QFETCH(int, stage); runPage(stage, 50, 10000);
}
void WallclockTest::withheldCompletion_data() { page48kTenSeconds_data(); }
void WallclockTest::withheldCompletion()
{
    QFETCH(int, stage); runPage(stage, 0, 10000);
}

void WallclockTest::runPage(int stage, int receiptDelayMs, int durationMs)
{
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    store.saveNoiseReduction(settingsFor(stage));
    NoiseReductionPage page(&store);
    page.resize(1350, 900);
    page.setActive(true);
    page.setRecordingRequired(true, {stage == 3
        ? NoiseReductionDataFlowStage::AutomaticGainControl : NoiseReductionDataFlowStage::Suppression});
    Handshake state;
    connectHandshake(page, stage, state);
    page.setControlLocked(false);
    page.setConnected(true);
    page.show();
    QTest::qWait(50); // Setup only; never drain/barrier after individual input frames.
    Metrics metrics;
    QMetaObject::invokeMethod(page.m_worker, [&] { metrics.cpuStart = workerCpuTicks(); },
                              Qt::BlockingQueuedConnection);
    connect(page.m_worker, &NoiseReductionWorker::recordingFrameReady, page.m_worker,
        [&metrics](const NoiseReductionRecordingFrame &frame)
        {
            std::lock_guard<std::mutex> lock(metrics.outputMutex);
            if (frame.stages.size() == 1) metrics.output += frame.stages[0].samples;
        }, Qt::DirectConnection);
    if (receiptDelayMs >= 0)
        QObject::disconnect(page.m_worker, &NoiseReductionWorker::frameConsumed, &page, nullptr);
    connect(page.m_worker, &NoiseReductionWorker::frameConsumed, page.m_worker,
        [&metrics] { ++metrics.completedJobs; ++metrics.waitingReceipts; }, Qt::DirectConnection);

    QEventLoop loop;
    QTimer receiptTimer;
    if (receiptDelayMs > 0)
    {
        receiptTimer.setTimerType(Qt::PreciseTimer);
        receiptTimer.setInterval(receiptDelayMs);
        connect(&receiptTimer, &QTimer::timeout, &page, [&]
        {
            const int count = metrics.waitingReceipts.exchange(0);
#ifndef R2_MAILBOX
            for (int index = 0; index < count; ++index) page.consumeWorkerCredit();
#else
            Q_UNUSED(count); // Completion notifications are not scheduler credit in R2.
#endif
        });
        receiptTimer.start();
    }
    QSignalSpy faults(page.m_plTransport, &PlDenoiseTransport::faultRaised);
    connect(page.m_plTransport, &PlDenoiseTransport::faultRaised, &loop,
            [&] { metrics.stop = true; loop.quit(); });
    const auto start = Clock::now();
    const int targetFrames = durationMs * 48 / 200;
    std::thread producer([&, config = state.active, stream = state.stream]
    {
        for (int frame = 0; frame < targetFrames && !metrics.stop; ++frame)
        {
            const auto due = start + std::chrono::nanoseconds(quint64(frame) * 200000000000ULL / 48000);
            std::this_thread::sleep_until(due);
            const auto now = Clock::now();
            maximum(metrics.maxProducerLateUsec, quint64(std::max<qint64>(0,
                std::chrono::duration_cast<std::chrono::microseconds>(now - due).count())));
            const quint32 first = frame * 200;
            auto payload = audio200(stage, first, stage == 1 ? config : stream);
            const auto heartbeat = rawFrame({statusPacket(stage, config, stream, config, 0x101)});
            // Put a valid DNS1 in an unused sideband group of the same real raw frame.
            for (int channel = 7; channel < 48; ++channel)
                payload[199 * 192 + channel * 4 + 3] = heartbeat[channel * 4 + 3];
            ++metrics.produced;
            QMetaObject::invokeMethod(&page, [&, payload, first]
            {
                if (metrics.stop) return;
                ++metrics.ingressed;
                page.submitPlSidebandFrame(payload, 1 + first / 48000,
                    quint64(first % 48000) * 1000000 / 48000);
                page.submitRawAudioFrame(QByteArray(38400, 0), 1 + first / 48000,
                    quint64(first % 48000) * 1000000 / 48000);
            }, Qt::QueuedConnection);
        }
    });
    QVector<QPair<double, qint64>> queueHistory;
    qint64 peak = 0;
    QTimer sampleTimer;
    sampleTimer.setInterval(100);
    connect(&sampleTimer, &QTimer::timeout, &page, [&]
    {
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        const qint64 samples = buffered(page);
        peak = std::max(peak, samples);
        queueHistory.append({seconds, samples});
    });
    sampleTimer.start();
    QTimer::singleShot(durationMs, Qt::PreciseTimer, &loop, &QEventLoop::quit);
    loop.exec();
    const double wallSeconds = std::chrono::duration<double>(Clock::now() - start).count();
    qsizetype outputAtWallEnd = 0;
    { std::lock_guard<std::mutex> lock(metrics.outputMutex); outputAtWallEnd = metrics.output.size(); }
    const int jobsAtWallEnd = metrics.completedJobs.load();
    if (!faults.isEmpty()) metrics.stop = true;
    producer.join();
    receiptTimer.stop();
    sampleTimer.stop();
    if (!faults.isEmpty())
    {
        peak = 96000;
    }
    else
    {
        QEventLoop drainLoop;
        QTimer drained;
        drained.setInterval(5);
        connect(&drained, &QTimer::timeout, &drainLoop, [&]
        {
            if (buffered(page) == 0 && referenceBuffered(page) == 0)
                drainLoop.quit();
        });
        drained.start();
        QTimer::singleShot(2000, &drainLoop, &QEventLoop::quit);
        drainLoop.exec();
    }
    QMetaObject::invokeMethod(page.m_worker, [&] { metrics.cpuEnd = workerCpuTicks(); },
                              Qt::BlockingQueuedConnection);
    metrics.stop = true;
#ifdef R2_MAILBOX
    peak = std::max(peak, page.m_mailbox->snapshot().plPeakSamples);
    const qint64 rawPeak = page.m_mailbox->snapshot().referencePeakSamples;
#else
    peak = std::max(peak, page.property("plQueuePeakSamples").toLongLong());
    const qint64 rawPeak = page.property("rawReferenceQueuePeakSamples").toLongLong();
#endif
    QVector<float> actual;
    { std::lock_guard<std::mutex> lock(metrics.outputMutex); actual = metrics.output; }
    const auto expected = reference(stage, metrics.ingressed * 200);
    const QJsonObject data {
        {"stage", stage}, {"receipt_delay_ms", receiptDelayMs}, {"wall_seconds", wallSeconds},
        {"produced_frames", metrics.produced.load()}, {"ingressed_frames", metrics.ingressed.load()},
        {"completed_jobs", metrics.completedJobs.load()}, {"output_samples", actual.size()},
        {"completed_jobs_at_wall_end", jobsAtWallEnd}, {"output_samples_at_wall_end", outputAtWallEnd},
        {"worker_cpu_ms", double(metrics.cpuEnd - metrics.cpuStart) / 10000.0},
        {"producer_max_late_us", qint64(metrics.maxProducerLateUsec.load())},
        {"queue_peak_samples", peak}, {"queue_slope_samples_per_second", queueSlope(queueHistory)},
        {"raw_queue_peak_samples", rawPeak},
        {"faults", faults.size()}, {"pl_final_buffered", buffered(page)},
        {"fault_reason", faults.isEmpty() ? QString() : faults[0][0].toString()}
    };
    qInfo().noquote() << "[WALLCLOCK_METRICS]" << QJsonDocument(data).toJson(QJsonDocument::Compact);
    disconnect(page.m_worker, nullptr, page.m_worker, nullptr);
    QCOMPARE(faults.size(), 0);
    QCOMPARE(metrics.produced.load(), targetFrames);
    QCOMPARE(metrics.ingressed.load(), targetFrames);
    QCOMPARE(actual, expected);
    QVERIFY2(outputAtWallEnd >= expected.size() - 2000, "Worker did not consume near 48 kHz by the actual deadline");
    QVERIFY2(peak <= 4800, "More than 100 ms of audio backlog under real wall-clock input");
    QVERIFY(rawPeak <= 4800);
    QVERIFY2(queueSlope(queueHistory) <= 200, "Sustained positive queue growth");
    QVERIFY2(metrics.maxProducerLateUsec < 50000, "Producer pacing was not a valid near-realtime load");
}

#ifdef R2_MAILBOX
void WallclockTest::schedulerConsumesWhileGuiFrozen_data() { page48kTenSeconds_data(); }

void WallclockTest::schedulerConsumesWhileGuiFrozen()
{
    QFETCH(int, stage);
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    store.saveNoiseReduction(settingsFor(stage));
    NoiseReductionPage page(&store);
    page.setActive(true);
    page.setRecordingRequired(true, {stage == 3
        ? NoiseReductionDataFlowStage::AutomaticGainControl : NoiseReductionDataFlowStage::Suppression});
    Handshake state;
    connectHandshake(page, stage, state);
    page.setControlLocked(false);
    page.setConnected(true);
    page.m_plWatchdog->stop(); // Direct scheduler test does not simulate DNS1 transport.
    QTest::qWait(50);
    Metrics metrics;
    QMetaObject::invokeMethod(page.m_worker, [&] { metrics.cpuStart = workerCpuTicks(); },
                              Qt::BlockingQueuedConnection);
    connect(page.m_worker, &NoiseReductionWorker::recordingFrameReady, page.m_worker,
        [&metrics](const NoiseReductionRecordingFrame &frame)
        {
            std::lock_guard<std::mutex> lock(metrics.outputMutex);
            if (frame.stages.size() == 1) metrics.output += frame.stages[0].samples;
        }, Qt::DirectConnection);
    connect(page.m_worker, &NoiseReductionWorker::frameConsumed, page.m_worker,
        [&metrics] { ++metrics.completedJobs; }, Qt::DirectConnection);
    const auto before = page.m_mailbox->snapshot();
    auto *mailbox = page.m_mailbox;
    auto *worker = page.m_worker;
    const auto start = Clock::now();
    std::atomic<int> rejected {0};
    QVector<QPair<double, qint64>> queueHistory;
    std::thread producer([&, config = state.active, stream = state.stream]
    {
        for (int frame = 0; frame < 2400; ++frame)
        {
            const auto due = start + std::chrono::nanoseconds(quint64(frame) * 200000000000ULL / 48000);
            std::this_thread::sleep_until(due);
            maximum(metrics.maxProducerLateUsec, quint64(std::max<qint64>(0,
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - due).count())));
            const quint32 first = frame * 200;
            const quint32 seconds = 1 + first / 48000;
            const quint32 micros = quint64(first % 48000) * 1000000 / 48000;
            QVector<float> samples;
            samples.reserve(200);
            for (int point = 0; point < 200; ++point)
                samples.append(float(1 + (first + quint32(point)) % 4093) / 65536.0f);
            std::function<void()> operation;
            if (stage == 1)
            {
                PlBeamFrame input;
                input.beams = {samples, QVector<float>(200, 0), QVector<float>(200, 0)};
                input.firstSampleIndex = first; input.configId = config;
                input.timestampSeconds = seconds; input.timestampMicros = micros;
                operation = [worker, input] { worker->processPlBeamFrame(input); };
            }
            else if (stage == 2)
            {
                PlWienerFrame input;
                input.samples = samples; input.firstSampleIndex = first; input.streamId = stream;
                input.timestampSeconds = seconds; input.timestampMicros = micros;
                operation = [worker, input] { worker->processPlWienerFrame(input); };
            }
            else
            {
                PlFinalFrame input;
                input.samples = samples; input.firstSampleIndex = first; input.streamId = stream;
                input.timestampSeconds = seconds; input.timestampMicros = micros;
                operation = [worker, input] { worker->processPlFinalFrame(input); };
            }
            if (!mailbox->enqueue(NoiseReductionMailbox::Kind::Pl, 200, std::move(operation), first)) ++rejected;
            const QByteArray raw(38400, 0);
            if (!mailbox->enqueue(NoiseReductionMailbox::Kind::Reference, 200,
                [worker, raw, seconds, micros] { worker->processRawFrame(raw, seconds, micros); }, first)) ++rejected;
            ++metrics.produced;
            if (frame % 24 == 0)
                queueHistory.append({std::chrono::duration<double>(Clock::now() - start).count(),
                                     mailbox->snapshot().plBufferedSamples});
        }
    });
    // This blocks only this offscreen test process's GUI. Producer and worker
    // continue independently; neither touches QWidget from another thread.
    std::this_thread::sleep_until(start + std::chrono::seconds(10));
    const auto atDeadline = mailbox->snapshot();
    qsizetype outputAtDeadline;
    { std::lock_guard<std::mutex> lock(metrics.outputMutex); outputAtDeadline = metrics.output.size(); }
    producer.join();
    const auto drainLimit = Clock::now() + std::chrono::seconds(1);
    while ((mailbox->snapshot().plBufferedSamples || mailbox->snapshot().referenceBufferedSamples)
           && Clock::now() < drainLimit)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    QMetaObject::invokeMethod(worker, [&] { metrics.cpuEnd = workerCpuTicks(); },
                              Qt::BlockingQueuedConnection);
    const auto after = mailbox->snapshot();
    QVector<float> actual;
    { std::lock_guard<std::mutex> lock(metrics.outputMutex); actual = metrics.output; }
    const auto expected = reference(stage, 480000);
    const QJsonObject data {
        {"stage", stage}, {"gui_frozen_ms", 10000}, {"gui_pump_calls_during_input", 0},
        {"produced_frames", metrics.produced.load()}, {"rejected_jobs", rejected.load()},
        {"processed_pl_samples_at_deadline", qint64(atDeadline.processedPlSamples - before.processedPlSamples)},
        {"processed_pl_samples", qint64(after.processedPlSamples - before.processedPlSamples)},
        {"processed_tasks", qint64(after.processedTasks - before.processedTasks)},
        {"completed_jobs", metrics.completedJobs.load()}, {"output_samples", actual.size()},
        {"output_samples_at_deadline", outputAtDeadline},
        {"pl_peak_samples", after.plPeakSamples}, {"reference_peak_samples", after.referencePeakSamples},
        {"queue_slope_samples_per_second", queueSlope(queueHistory)},
        {"worker_cpu_ms", double(metrics.cpuEnd - metrics.cpuStart) / 10000.0},
        {"producer_max_late_us", qint64(metrics.maxProducerLateUsec.load())},
        {"queue_residence_max_us", after.queueResidenceMaxUsec}, {"process_max_us", after.processMaxUsec}
    };
    qInfo().noquote() << "[SCHEDULER_GUI_FROZEN_METRICS]" << QJsonDocument(data).toJson(QJsonDocument::Compact);
    disconnect(worker, nullptr, worker, nullptr);
    QCOMPARE(metrics.produced.load(), 2400);
    QCOMPARE(rejected.load(), 0);
    QCOMPARE(after.processedPlSamples - before.processedPlSamples, quint64(480000));
    QCOMPARE(metrics.completedJobs.load(), 4800);
    QCOMPARE(actual, expected);
    QVERIFY(atDeadline.processedPlSamples - before.processedPlSamples >= 478000);
    QVERIFY(after.plPeakSamples <= 4800 && after.referencePeakSamples <= 4800);
    QVERIFY(queueSlope(queueHistory) <= 200);
    QVERIFY(metrics.maxProducerLateUsec < 50000);
}
#endif

QTEST_MAIN(WallclockTest)
#include "test_wallclock.moc"
