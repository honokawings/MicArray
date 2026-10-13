#include "pipeline.h"
#include "hardwareprofile.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QMutex>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QWaitCondition>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>

namespace MicArray::Array128 {

namespace {
quint32 effectiveAcousticFaults(quint32 latchedFlags, const std::array<bool, Channels> &clipped,
                               const std::array<bool, Channels> &disabled)
{
    // M128 bit0 is aggregate/sticky, with no channel attribution. Keep it in
    // diagnostics, but base the channel quality gate on observed PCM endpoints.
    // Queue/schedule/framing/selftest faults are global and cannot be masked.
    quint32 faults = latchedFlags & (ErrorFlags & ~quint32(1));
    for (int ch = 0; ch < Channels; ++ch) if (clipped[ch] && !disabled[ch]) faults |= 1;
    return faults;
}

QString acousticDataStatus(const PipelineSnapshot &status, bool geometryValid)
{
    if (status.flags & 0x10) return QStringLiteral("数字自测：禁止定位和波束降噪");
    if (status.acousticFaultFlags()) return status.acousticFaultReason();
    if (!geometryValid) return QStringLiteral("等待导入真实 128 阵元坐标");
    return status.latchedFlags & 1 ? QStringLiteral("实采流；设备曾饱和，未屏蔽通道未见满量程削顶")
        : QStringLiteral("实采流，可用于声学处理");
}
}

quint32 PipelineSnapshot::acousticFaultFlags() const
{
    return effectiveAcousticFaults(latchedFlags, clippedChannels, disabledProcessingChannels);
}

QString PipelineSnapshot::acousticFaultReason() const
{
    const auto faults = acousticFaultFlags();
    if (faults & ~quint32(1)) {
        QStringList reasons;
        if (faults & 2) reasons.append(QStringLiteral("队列溢出"));
        if (faults & 4) reasons.append(QStringLiteral("调度异常"));
        if (faults & 8) reasons.append(QStringLiteral("打包异常"));
        if (faults & 0x20) reasons.append(QStringLiteral("自测失败"));
        return QStringLiteral("会话全局故障（%1）：通道屏蔽无法排除，请排查并重新接收").arg(reasons.join(QStringLiteral("、")));
    }
    if (faults & 1) {
        QStringList channels;
        int count = 0;
        for (int ch = 0; ch < Channels; ++ch) if (clippedChannels[ch] && !disabledProcessingChannels[ch]) {
            ++count;
            if (channels.size() < 8) channels.append(QStringLiteral("CH%1").arg(ch));
        }
        return QStringLiteral("未屏蔽通道削顶（%1%2，共 %3 路）：请屏蔽故障通道或排查后重新接收")
            .arg(channels.join(QStringLiteral("、")), count > 8 ? QStringLiteral("…") : QString()).arg(count);
    }
    return {};
}

QJsonObject PipelineSnapshot::json() const
{
    QJsonArray channels, disabled, sources, faultChannels;
    for (int ch = 0; ch < Channels; ++ch) if (disabledProcessingChannels[ch]) disabled.append(ch);
    for (int ch = 0; ch < Channels; ++ch) if (clippedChannels[ch] && !disabledProcessingChannels[ch]) faultChannels.append(ch);
    for (const auto &source : localization.sources) sources.append(QJsonObject{
        {"azimuth_deg", source.direction.azimuthDegrees}, {"elevation_deg", source.direction.elevationDegrees},
        {"confidence", source.confidence}, {"relative_response", source.relativePower}, {"trusted", source.trusted}});
    for (int ch = 0; ch < Channels; ++ch) {
        const auto &q = quality[ch];
        channels.append(QJsonObject {{"channel", ch}, {"data_lane", ch / 2}, {"lr", ch % 2 ? "VDD" : "GND"},
            {"clock_group", QJsonValue(QJsonValue::Null)}, {"connector", dataConnector(ch)},
            {"io_bank", dataBank(ch)}, {"fpga_pad", dataPin(ch)}, {"excluded_from_processing", disabledProcessingChannels[ch]},
            {"excluded_from_quality_gate", disabledProcessingChannels[ch]}, {"pcm_saturation_latched", clippedChannels[ch]},
            {"rms_dbfs", q.rmsDb}, {"ac_rms_dbfs", q.acRmsDb},
            {"peak_dbfs", q.peakDb}, {"dc_full_scale", q.dc}, {"clipped", QString::number(q.clipped)},
            {"zeros", QString::number(q.zeros)}, {"unchanged_adjacent", QString::number(q.unchanged)}});
    }
    return {{"format", "micarray128-diagnostics-v1"}, {"received_packets", QString::number(received)},
        {"valid_packets", QString::number(valid)}, {"invalid_packets", QString::number(invalid)},
        {"rejected_source", QString::number(rejectedSource)}, {"host_queue_drops", QString::number(queueDrops)},
        {"listening", listening}, {"receiver_failed", receiverFailed}, {"queued_packets", queued}, {"socket_buffer_bytes", socketBuffer},
        {"last_processed_packet_age_ms", lastPacketAgeMs}, {"last_source_datagram_age_ms", lastSourceDatagramAgeMs},
        {"last_processing_delay_ms", processingDelayMs}, {"stale_epoch_packets", QString::number(staleEpochPackets)},
        {"analysis_windows_replaced", QString::number(analysisReplaced)}, {"monitor_samples_dropped", QString::number(monitorDrops)},
        {"runs", QString::number(runs)}, {"epoch", QString::number(epoch)}, {"flags", int(flags)},
        {"latched_flags", int(latchedFlags)}, {"packet_rate", packetRate}, {"udp_bytes_per_second", byteRate},
        {"acoustic_fault_flags", int(acousticFaultFlags())}, {"acoustic_fault_channels", faultChannels},
        {"packet_sequence", packetSequence}, {"frame_sequence", frameSequence},
        {"discontinuities", QString::number(discontinuities)}, {"selftest", selftest.json()},
        {"acoustic_status", acousticStatus}, {"channels", channels}, {"error", error},
        {"disabled_processing_channels", disabled},
        {"srp_localization", QJsonObject{{"generation", QString::number(localization.generation)},
            {"run", QString::number(localization.run)}, {"last_frame", QString::number(localization.lastFrame)},
            {"host_observation_time_ns", QString::number(localization.observationTimeNs)},
            {"sources", sources}, {"valid_channels", localization.validChannels}, {"valid_pairs", localization.validPairs},
            {"elapsed_ms", localization.elapsedMs}, {"reason", localization.reason}}},
        {"hardware_reference", hardwareReference()},
        {"unobservable", "UDP has no PHY/PLL/ARP telemetry; separately connected SYS3 UART reports these with per-domain freshness. Voltage, clock loads and acoustics require external validation."}};
}

struct Pipeline::State
{
    struct Datagram { QByteArray bytes; quint64 arrivalNs = 0; };
    mutable QMutex mutex;
    QWaitCondition packetReady;
    QWaitCondition analysisReady;
    std::deque<Datagram> queue;
    PipelineSnapshot status;
    ProcessingSettings settings;
    quint64 revision = 0;
    QByteArray monitor;
    std::atomic_bool stopping {true};
    std::atomic_bool quitAnalysis {false};
    std::unique_ptr<QThread> networkThread;
    std::unique_ptr<QThread> processingThread;
    std::unique_ptr<QThread> analysisThread;
    Recorder recorder;
    QElapsedTimer clock;
    qint64 lastArrivalNs = -1;
    qint64 lastSourceArrivalNs = -1;
    QVector<QVector<float>> pendingWindow;
    ProcessingSettings pendingSettings;
    quint64 pendingRun = 0, pendingFrame = 0;
    quint64 acousticToken = 0, pendingToken = 0;
    qint64 pendingObservationNs = -1;

    State()
    {
        status.selftest = SelftestAudit(selftestReference());
        clock.start();
        analysisThread.reset(QThread::create([this] {
            for (;;) {
                QVector<QVector<float>> window;
                ProcessingSettings config;
                quint64 run, frame, token;
                qint64 observationNs;
                {
                    QMutexLocker lock(&mutex);
                    while (pendingWindow.isEmpty() && !quitAnalysis) analysisReady.wait(&mutex);
                    if (quitAnalysis) break;
                    window = std::move(pendingWindow); pendingWindow.clear();
                    config = pendingSettings; run = pendingRun; frame = pendingFrame; token = pendingToken;
                    observationNs = pendingObservationNs;
                }
                auto result = localize(window, config);
                result.run = run; result.lastFrame = frame;
                result.observationTimeNs = observationNs;
                QMutexLocker lock(&mutex);
                if (!stopping && config.generation == settings.generation && run == status.runs && token == acousticToken
                    && config.disabledChannels == settings.disabledChannels
                    && !status.acousticFaultFlags() && !(status.flags & 0x10)) status.localization = result;
            }
        }));
        analysisThread->setObjectName("M128Localization");
        analysisThread->start();
    }

    void receive(const NetworkSettings &config)
    {
        QUdpSocket socket;
        const QHostAddress bind(config.bindAddress), source(config.sourceAddress);
        if (bind.isNull() || source.isNull()
            || !socket.bind(bind, config.port, QUdpSocket::DontShareAddress)) {
            QMutexLocker lock(&mutex);
            status.error = QStringLiteral("UDP 绑定失败：%1").arg(socket.errorString());
            status.receiverFailed = true;
            status.listening = false;
            return;
        }
        socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 8 * 1024 * 1024);
        {
            QMutexLocker lock(&mutex);
            status.socketBuffer = socket.socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toInt();
            status.listening = true;
        }
        quint64 intervalPackets = 0, intervalBytes = 0;
        QElapsedTimer interval; interval.start();
        while (!stopping) {
            if (!socket.hasPendingDatagrams()) socket.waitForReadyRead(10);
            QVector<Datagram> batch;
            quint64 received = 0, bytes = 0, rejected = 0, invalid = 0;
            while (!stopping && socket.hasPendingDatagrams() && received < 256) {
                const auto datagram = socket.receiveDatagram(65535);
                ++received; bytes += datagram.data().size();
                if (datagram.senderAddress() != source || datagram.senderPort() != config.sourcePort) { ++rejected; continue; }
                if (datagram.data().size() != DatagramBytes) { ++invalid; continue; }
                batch.append({datagram.data(), quint64(clock.nsecsElapsed())});
            }
            intervalPackets += received; intervalBytes += bytes;
            QMutexLocker lock(&mutex);
            status.received += received; status.receivedBytes += bytes;
            status.rejectedSource += rejected; status.invalid += invalid;
            if (invalid) status.error = QStringLiteral("发现非 1208 字节 UDP 数据报，已拒绝");
            if (!batch.isEmpty()) lastSourceArrivalNs = qint64(batch.back().arrivalNs);
            for (auto &datagram : batch) {
                if (queue.size() >= 8192) ++status.queueDrops;
                else queue.push_back(std::move(datagram));
            }
            status.queued = int(queue.size());
            if (!batch.isEmpty()) packetReady.wakeOne();
            if (interval.elapsed() >= 500) {
                const double seconds = interval.nsecsElapsed() / 1e9;
                status.packetRate = intervalPackets / seconds; status.byteRate = intervalBytes / seconds;
                intervalPackets = intervalBytes = 0; interval.restart();
            }
        }
        QMutexLocker lock(&mutex);
        status.listening = false;
    }

    void process()
    {
        ProcessingSettings config;
        quint64 applied = ~quint64(0), run = 0, acousticId = 0;
        SequenceTracker packets, frames;
        SelftestAudit audit(selftestReference());
        quint32 epoch = 0, latched = 0;
        quint64 lastFrame = 0, lastSequence = 0, lastNs = 0, lastAnalysisFrame = 0;
        quint64 baselineDrops = 0;
        bool hasPrevious = false, lastSelftest = false;
        Denoiser denoiser;
        QVector<QVector<float>> block(Channels), window(Channels);
        std::array<double, Channels> sum {}, sumSquare {}, peak {};
        std::array<qint32, Channels> previousSample {};
        std::array<ChannelQuality, Channels> quality;
        std::array<bool, Channels> clippedChannels {};
        quint64 qualityFrames = 0;
        quint64 observed = 0, discontinuities = 0, stale = 0;
        QElapsedTimer publish; publish.start();
        auto clearAcoustic = [&] {
            for (auto &c : block) c.clear();
            for (auto &c : window) c.clear();
            denoiser.reset(); ++acousticId;
            QMutexLocker lock(&mutex);
            status.localization = {}; pendingWindow.clear(); monitor.clear(); ++acousticToken;
        };
        auto appendMonitor = [&](const QVector<float> &samples) {
            if (!config.monitor) return;
            QByteArray pcm(samples.size() * 2, Qt::Uninitialized);
            const double gain = std::pow(10, config.monitorGainDb / 20);
            for (int n = 0; n < samples.size(); ++n) {
                const int value = int(std::lround(std::clamp(samples[n] * gain, -1.0, 32767.0 / 32768) * 32768));
                qToLittleEndian<qint16>(qint16(value), pcm.data() + n * 2);
            }
            QMutexLocker lock(&mutex);
            monitor += pcm;
            const int cap = config.bufferMs * SampleRate * 4 / 1000;
            if (monitor.size() > cap) {
                const auto dropped = (monitor.size() - cap + 1) & ~qsizetype(1);
                monitor.remove(0, dropped); status.monitorDrops += dropped / 2;
            }
        };
        while (!stopping) {
            QVector<Datagram> batch;
            quint64 drops = 0;
            bool changed = false;
            bool directionsOnly = false;
            {
                QMutexLocker lock(&mutex);
                if (queue.empty()) packetReady.wait(&mutex, 20);
                if (stopping) break;
                if (applied != revision) {
                    directionsOnly = applied != ~quint64(0) && onlyDirectionChange(config, settings);
                    applied = revision; config = settings; changed = true;
                }
                drops = status.queueDrops + status.invalid;
                while (!queue.empty() && batch.size() < 128) { batch.append(std::move(queue.front())); queue.pop_front(); }
                status.queued = int(queue.size());
            }
            if (changed) { denoiser.configure(config); if (!directionsOnly) clearAcoustic(); }
            if (audit.frames && drops != baselineDrops) audit.hostLoss = true;
            QVector<CaptureRecord> capture;
            QVector<float> rawMonitor;
            for (const auto &datagram : batch) {
                QString error;
                const auto decoded = decodeDatagram(datagram.bytes, &error);
                if (!decoded) {
                    { QMutexLocker lock(&mutex); ++status.invalid; status.error = error; }
                    ++drops;
                    audit.hostLoss = audit.hostLoss || audit.frames > 0;
                    clearAcoustic();
                    continue;
                }
                const auto &packet = *decoded;
                ++observed;
                const bool idleRestart = hasPrevious && datagram.arrivalNs - lastNs > 1500000000ull;
                const bool newEpoch = hasPrevious && packet.epoch != epoch;
                const bool olderEpoch = newEpoch && quint32(packet.epoch - epoch) >= 0x80000000u && !idleRestart;
                if (olderEpoch) {
                    ++stale; capture.append({packet, datagram.arrivalNs, run, false});
                    continue;
                }
                if (!hasPrevious || newEpoch || idleRestart || packet.selftest() != lastSelftest) {
                    ++run; epoch = packet.epoch; latched = 0; clippedChannels = {}; packets = {}; frames = {};
                    audit = SelftestAudit(selftestReference()); baselineDrops = drops;
                    hasPrevious = false; quality = {}; qualityFrames = 0; sum = {}; sumSquare = {}; peak = {};
                    clearAcoustic();
                }
                const auto event = packets.observe(packet.sequence);
                const bool packetContinuous = !hasPrevious || (packet.sequence == lastSequence + 1 && packet.frameIds[0] == lastFrame + 1);
                const bool oldPacket = event == SequenceTracker::Event::Duplicate || event == SequenceTracker::Event::Reordered
                    || event == SequenceTracker::Event::BeforeStart || event == SequenceTracker::Event::CapacityExceeded;
                if (hasPrevious && !packetContinuous) { ++discontinuities; clearAcoustic(); }
                latched |= packet.flags & ErrorFlags;
                // Inspect the entire packet before admitting any frame into DSP.
                // A clip in its final channel/frame must invalidate earlier work
                // immediately, not when throttled quality statistics are published.
                if (!packet.selftest()) for (int r = 0; r < FramesPerPacket; ++r) for (int ch = 0; ch < Channels; ++ch) {
                    const auto sample = packet.sample(r, ch);
                    clippedChannels[ch] = clippedChannels[ch] || sample == -8388608 || sample == 8388607;
                }
                {
                    QMutexLocker lock(&mutex);
                    status.flags = packet.flags; status.latchedFlags = latched; status.clippedChannels = clippedChannels;
                }
                audit.observe(packet, packetContinuous, drops != baselineDrops);
                capture.append({packet, datagram.arrivalNs, run, packetContinuous && hasPrevious});
                const bool usable = !packet.selftest() && !effectiveAcousticFaults(latched, clippedChannels, config.disabledChannels) && !oldPacket;
                // A completed 512-sample block can be empty while an analysis
                // window/result is still live. A fault must invalidate both.
                if (!usable && (!block[0].isEmpty() || !window[0].isEmpty())) clearAcoustic();
                for (int r = 0; r < 3; ++r) {
                    frames.observe(packet.frameIds[r]);
                    if (r && packet.frameIds[r] != packet.frameIds[r - 1] + 1) { ++discontinuities; clearAcoustic(); }
                    for (int ch = 0; ch < Channels; ++ch) {
                        const qint32 sample = packet.sample(r, ch);
                        const double value = sample / 8388608.0;
                        sum[ch] += value; sumSquare[ch] += value * value; peak[ch] = std::max(peak[ch], std::abs(value));
                        quality[ch].clipped += sample == -8388608 || sample == 8388607;
                        quality[ch].zeros += sample == 0;
                        quality[ch].unchanged += qualityFrames > 0 && previousSample[ch] == sample;
                        previousSample[ch] = sample;
                        if (usable && config.geometry.valid() && (config.denoise || config.localization)) block[ch].append(float(value));
                        if (ch == config.monitorChannel) rawMonitor.append(float(value));
                    }
                    ++qualityFrames;
                    if (block[0].size() >= 512) {
                        if (config.denoise) {
                            const auto stages = denoiser.process(block);
                            recorder.enqueueStages({stages[3], stages[4], stages[5], stages[6]}, acousticId, packet.frameIds[r]);
                            if (config.monitorSource >= 1 && config.monitorSource <= 7) appendMonitor(stages[config.monitorSource - 1]);
                        }
                        for (int ch = 0; ch < Channels; ++ch) {
                            window[ch] += block[ch]; block[ch].clear();
                            if (window[ch].size() > 2048) window[ch].remove(0, window[ch].size() - 2048);
                        }
                        if (config.localization && window[0].size() == 2048 && packet.frameIds[r] - lastAnalysisFrame >= 4800) {
                            QMutexLocker lock(&mutex);
                            if (applied == revision) {
                                if (!pendingWindow.isEmpty()) ++status.analysisReplaced;
                                pendingWindow = window; pendingSettings = config; pendingRun = run; pendingFrame = packet.frameIds[r]; pendingToken = acousticToken;
                                pendingObservationNs = qint64(datagram.arrivalNs) - qint64((1023.5 + 2 - r) * 1e9 / 48000.0);
                                lastAnalysisFrame = packet.frameIds[r]; analysisReady.wakeOne();
                            }
                        }
                    }
                }
                lastFrame = packet.frameIds[2]; lastSequence = packet.sequence; lastNs = datagram.arrivalNs;
                lastSelftest = packet.selftest(); hasPrevious = true;
                {
                    QMutexLocker lock(&mutex);
                    lastArrivalNs = qint64(datagram.arrivalNs);
                    status.processingDelayMs = (clock.nsecsElapsed() - lastArrivalNs) / 1000000;
                    status.valid = observed; status.runs = run; status.epoch = epoch;
                    status.flags = packet.flags; status.latchedFlags = latched;
                    status.sequence = packet.sequence; status.frame = lastFrame;
                    status.latestHeader = packet.bytes.first(40);
                    status.acousticStatus = acousticDataStatus(status, settings.geometry.valid());
                }
            }
            recorder.enqueue(capture);
            if (config.monitorSource == 0) appendMonitor(rawMonitor);
            if (!rawMonitor.isEmpty()) {
                QMutexLocker lock(&mutex);
                status.waveform += rawMonitor;
                if (status.waveform.size() > 2048) status.waveform.remove(0, status.waveform.size() - 2048);
            }
            if (publish.elapsed() >= 100) {
                if (qualityFrames) for (int ch = 0; ch < Channels; ++ch) {
                    quality[ch].dc = sum[ch] / qualityFrames;
                    double power = sumSquare[ch] / qualityFrames;
                    quality[ch].rmsDb = 10 * std::log10(std::max(power, 1e-12));
                    quality[ch].peakDb = 20 * std::log10(std::max(peak[ch], 1e-6));
                    quality[ch].acRmsDb = 10 * std::log10(std::max(power - quality[ch].dc * quality[ch].dc, 1e-12));
                }
                QMutexLocker lock(&mutex);
                status.quality = quality; status.selftest = audit;
                status.packetSequence = packets.json(); status.frameSequence = frames.json();
                status.valid = observed;
                status.discontinuities = discontinuities; status.staleEpochPackets = stale;
                sum = {}; sumSquare = {}; peak = {}; qualityFrames = 0; publish.restart();
                if (lastArrivalNs >= 0 && clock.nsecsElapsed() - lastArrivalNs > 1500000000ll) {
                    status.localization = {}; pendingWindow.clear(); monitor.clear(); ++acousticToken;
                    status.acousticStatus = QStringLiteral("接收超时：声学结果已清空");
                }
            }
        }
        // The UI is throttled to 100 ms, but a final capture manifest must include
        // every processed packet, including errors in the last partial interval.
        QMutexLocker lock(&mutex);
        status.valid = observed;
        status.selftest = audit;
        status.packetSequence = packets.json();
        status.frameSequence = frames.json();
        status.discontinuities = discontinuities;
        status.staleEpochPackets = stale;
    }
};

Pipeline::Pipeline(QObject *parent) : QObject(parent), m_state(std::make_unique<State>()) {}
Pipeline::~Pipeline()
{
    stop();
    { QMutexLocker lock(&m_state->mutex); m_state->quitAnalysis = true; m_state->analysisReady.wakeAll(); }
    m_state->analysisThread->wait();
}

void Pipeline::start(const NetworkSettings &settings)
{
    stop();
    auto &s = *m_state;
    { QMutexLocker lock(&s.mutex); s.status = PipelineSnapshot(); s.status.selftest = SelftestAudit(selftestReference());
        s.status.disabledProcessingChannels = s.settings.disabledChannels;
        s.queue.clear(); s.lastArrivalNs = -1; s.lastSourceArrivalNs = -1; s.stopping = false; }
    s.processingThread.reset(QThread::create([&s] { s.process(); }));
    s.networkThread.reset(QThread::create([&s, settings] { s.receive(settings); }));
    s.processingThread->setObjectName("M128Processing");
    s.networkThread->setObjectName("M128UdpReceiver");
    s.processingThread->start(); s.networkThread->start();
}

void Pipeline::stop()
{
    auto &s = *m_state;
    s.stopping = true;
    { QMutexLocker lock(&s.mutex); s.packetReady.wakeAll(); s.pendingWindow.clear(); s.monitor.clear(); }
    if (s.networkThread) { s.networkThread->wait(); s.networkThread.reset(); }
    if (s.processingThread) { s.processingThread->wait(); s.processingThread.reset(); }
    {
        QMutexLocker lock(&s.mutex);
        s.status.queueDrops += s.queue.size();
        if (!s.queue.empty() && s.status.selftest.frames) s.status.selftest.hostLoss = true;
        s.queue.clear(); s.status.queued = 0;
    }
    s.recorder.setDiagnostics(snapshot().json()); s.recorder.finish();
    QMutexLocker lock(&s.mutex); s.status.listening = false; s.status.localization = {};
}

void Pipeline::configure(const ProcessingSettings &settings)
{
    auto &s = *m_state;
    QMutexLocker lock(&s.mutex);
    const bool directionsOnly = onlyDirectionChange(s.settings, settings);
    s.settings = settings;
    s.status.disabledProcessingChannels = settings.disabledChannels;
    s.settings.monitorChannel = std::clamp(settings.monitorChannel, 0, 127);
    s.settings.bufferMs = std::clamp(settings.bufferMs, 50, 1000);
    auto order = settings.postOrder;
    std::sort(order.begin(), order.end());
    if (order != QVector<int>({0, 1, 2})) s.settings.postOrder = {0, 1, 2};
    ++s.revision;
    if (!directionsOnly) { s.status.localization = {}; s.pendingWindow.clear(); s.monitor.clear(); ++s.acousticToken; }
    if (s.status.listening && s.lastArrivalNs >= 0 && s.clock.nsecsElapsed() - s.lastArrivalNs < 1500000000ll)
        s.status.acousticStatus = acousticDataStatus(s.status, settings.geometry.valid());
}

PipelineSnapshot Pipeline::snapshot() const
{
    const auto &s = *m_state;
    QMutexLocker lock(&s.mutex);
    auto status = s.status;
    status.disabledProcessingChannels = s.settings.disabledChannels;
    status.lastPacketAgeMs = s.lastArrivalNs < 0 ? -1 : (s.clock.nsecsElapsed() - s.lastArrivalNs) / 1000000;
    status.lastSourceDatagramAgeMs = s.lastSourceArrivalNs < 0 ? -1 : (s.clock.nsecsElapsed() - s.lastSourceArrivalNs) / 1000000;
    return status;
}

qint64 Pipeline::monotonicNowNs() const
{
    return m_state->clock.nsecsElapsed(); // Clock is initialized once and never restarted.
}

QByteArray Pipeline::takeMonitorPcm()
{
    auto &s = *m_state;
    QMutexLocker lock(&s.mutex); QByteArray result; result.swap(s.monitor); return result;
}

Recorder &Pipeline::recorder() { return m_state->recorder; }

} // namespace MicArray::Array128
