#include "sys3protocol.h"

#include <QJsonArray>

namespace MicArray::Array128 {
namespace {
QString hex(quint32 value, int width = 8) { return QStringLiteral("0x%1").arg(value, width, 16, QChar('0')).toUpper(); }
QString bit(quint32 value, int index) { return value & (quint32(1) << index) ? QStringLiteral("是") : QStringLiteral("否"); }
}

bool Sys3Frame::clocksHealthy() const
{
    const quint32 c = words[2];
    return (c & 7) == 7 && ((c >> 3) & 7) == 7 && ((c >> 11) & 15) == 15
        && ((c >> 19) & 15) == 0 && !(c & 0x08000000);
}

std::optional<Sys3Frame> decodeSys3Line(const QByteArray &line)
{
    if (line.size() != 312 || !line.startsWith("SYS3 ") || !line.endsWith("\r\n")) return {};
    Sys3Frame result; result.raw = line;
    for (int i = 0; i < 34; ++i) {
        const int offset = 5 + i * 9;
        quint32 value = 0;
        for (int j = 0; j < 8; ++j) {
            const char c = line[offset + j];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return {};
            value = (value << 4) | quint32(c <= '9' ? c - '0' : c - 'A' + 10);
        }
        if (i < 33 && line[offset + 8] != ' ') return {};
        result.words[i] = value;
    }
    return result;
}

int sys3WordDomain(int word)
{
    return word < 8 ? 0 : word < 12 ? 1 : word < 30 ? 2 : 3;
}

QStringList sys3WordNames()
{
    return {"UART sequence", "Observer uptime (ms)", "Clock/reset flags", "Raw125/96 frequency counts",
        "Audio/Eth frequency counts", "Command count/last ASCII", "UART receive/unknown errors", "Snapshot freshness/waiting",
        "Acquisition flags", "Dropped frames", "PCM handshakes", "Last PCM frame (low32)", "PHY flags/state/step/address",
        "PHY ID", "Last MDIO transaction", "MDIO write/read data", "MDIO transactions", "MDIO read no ACK", "PHY retries",
        "Last PHY failure", "PHYSR", "MAC/TX flags", "ARP request TX", "ARP reply TX", "ARP accepted", "UDP TX",
        "Ethernet TX", "Stream epoch", "Peer MAC low32", "Peer MAC high16", "RX frames", "RX_ER frames", "RX_DV bytes", "RX cycles"};
}

QJsonObject Sys3Frame::json() const
{
    QJsonArray raw;
    for (quint32 w : words) raw.append(hex(w));
    return {{"sequence", double(words[0])}, {"uptime_ms", double(words[1])}, {"raw_words_hex", raw},
        {"clocks_healthy", clocksHealthy()}, {"fresh_mask", int(words[7] & 7)},
        {"snapshot_waiting_mask", int((words[7] >> 4) & 7)},
        {"observer_controller_state", int((words[2] >> 6) & 31)},
        {"audio_controller_state", int((words[8] >> 19) & 31)},
        {"audio_fresh", audioFresh()}, {"eth_fresh", ethFresh()}, {"rx_fresh", rxFresh()},
        {"dc_correction_enabled", bool(words[8] & (1 << 12))}, {"dc_estimation_finished", bool(words[8] & (1 << 13))},
        {"dc_any_channel_rejected", bool(words[8] & (1 << 14))},
        {"selftest_mode", selftest()}, {"commands", commands()}, {"last_command_hex", hex((words[5] >> 16) & 255, 2)},
        {"phy_id", hex(words[13])}, {"phy_ready", bool(words[12] & 0x4000)}, {"link_up", bool(words[12] & 0x8000)},
        {"physr", hex(words[20], 4)}, {"gigabit_full_duplex", (words[20] & 0x3c) == 0x2c},
        {"peer_valid", bool(words[21] & 4)}, {"udp_sent", double(words[25])}, {"stream_epoch", double(words[27])},
        {"tx_enabled", bool(words[21] & 1)}, {"packet_full", bool(words[21] & 8)},
        {"arp_reply_pending", bool(words[21] & 32)}, {"tx_draining", bool(words[21] & 64)},
        {"rx_frames", double(words[30])}, {"scope", "FPGA snapshot; domain freshness and host age must both be checked; not PC reception proof"}};
}

QVector<QStringList> sys3Details(const Sys3Frame &f)
{
    const auto &w = f.words;
    QVector<QStringList> rows;
    auto add = [&](const QString &name, const QString &value, int domain = 0) { rows.append({name, value, QString::number(domain)}); };
    auto n = [&](const QString &name, quint32 value, int domain) { add(name, QString::number(value), domain); };
    add(QStringLiteral("时钟与复位正常"), f.clocksHealthy() ? QStringLiteral("是") : QStringLiteral("否"));
    add(QStringLiteral("PLL LOCK / 复位释放掩码"), hex(w[2] & 7, 1) + " / " + hex((w[2] >> 3) & 7, 1));
    add(QStringLiteral("频率合格 / 检查启用掩码"), hex((w[2] >> 11) & 15, 1) + " / " + hex((w[2] >> 23) & 15, 1));
    add(QStringLiteral("时钟当前 / 历史故障"), hex((w[2] >> 19) & 15, 1) + " / " + hex((w[2] >> 15) & 15, 1));
    add(QStringLiteral("系统复位请求"), bit(w[2], 27));
    n(QStringLiteral("控制器状态（观察域）"), (w[2] >> 6) & 31, 0);
    QStringList mhz;
    for (int i = 0; i < 4; ++i) mhz << QString::number(((w[3 + i / 2] >> (16 * (i % 2))) & 65535) * 0.1024, 'f', 3);
    add(QStringLiteral("相对频率 MHz（raw125/96/audio/eth）"), mhz.join(" / "));
    add(QStringLiteral("域新鲜 / 等待应答掩码"), hex(w[7] & 7, 1) + " / " + hex((w[7] >> 4) & 7, 1));
    add(QStringLiteral("命令计数 / 最近字符"), QString::number(f.commands()) + " / " + hex((w[5] >> 16) & 255, 2));
    add(QStringLiteral("UART 停止位错误 / 未知命令"), QString::number(w[6] & 65535) + " / " + QString::number(w[6] >> 16));
    add(QStringLiteral("采集模式"), f.selftest() ? QStringLiteral("内部 PDM 自测") : QStringLiteral("真实麦克风"), 1);
    add(QStringLiteral("启动 / 采集中 / 运行"), bit(w[8], 0) + " / " + bit(w[8], 1) + " / " + bit(w[8], 2), 1);
    add(QStringLiteral("校准完成 / 已加载自测模式"), bit(w[8], 10) + " / " + bit(w[8], 11), 1);
    add(QStringLiteral("音频复位释放"), bit(w[8], 9), 1);
    add(QStringLiteral("直流补偿配置使能 / 估计审核结束 / 存在拒绝通道"), bit(w[8], 12) + " / " + bit(w[8], 13) + " / " + bit(w[8], 14), 1);
    n(QStringLiteral("控制器状态（音频域）"), (w[8] >> 19) & 31, 1);
    add(QStringLiteral("溢出 / 处理错误 / 饱和"), bit(w[8], 4) + " / " + bit(w[8], 5) + " / " + bit(w[8], 6), 1);
    add(QStringLiteral("本地 BIST 通过 / 失败"), bit(w[8], 7) + " / " + bit(w[8], 8), 1);
    n(QStringLiteral("丢弃采样帧"), w[9], 1); n(QStringLiteral("PCM 握手计数"), w[10], 1); n(QStringLiteral("最近 PCM 帧低 32 位"), w[11], 1);
    add(QStringLiteral("PHY state / step / address"), QStringLiteral("%1 / %2 / %3").arg(w[12] & 15).arg((w[12] >> 4) & 31).arg((w[12] >> 9) & 31), 2);
    add(QStringLiteral("PHY ID（期望 001CC916）"), hex(w[13]), 2);
    add(QStringLiteral("PHY 就绪 / 链路 / 历史故障 / 复位释放"), bit(w[12], 14) + " / " + bit(w[12], 15) + " / " + bit(w[12], 16) + " / " + bit(w[12], 17), 2);
    add(QStringLiteral("PHYSR / 1000M 全双工"), hex(w[20], 4) + ((w[20] & 0x3c) == 0x2c ? QStringLiteral(" / 是") : QStringLiteral(" / 否")), 2);
    add(QStringLiteral("MDIO state / step / register / address"), QStringLiteral("%1 / %2 / %3 / %4").arg(w[14] & 15).arg((w[14] >> 4) & 31).arg((w[14] >> 9) & 31).arg((w[14] >> 14) & 31), 2);
    add(QStringLiteral("MDIO 类型 / ACK / 数据"), (w[14] & (1 << 20)) ? QStringLiteral("读 / %1 / %2").arg(bit(w[14], 19), hex(w[15] & 65535, 4))
        : QStringLiteral("写 / 不适用 / %1").arg(hex(w[15] >> 16, 4)), 2);
    n(QStringLiteral("MDIO 事务 / 完成数"), w[16], 2); n(QStringLiteral("MDIO 读无 ACK（扫描可出现）"), w[17], 2); n(QStringLiteral("PHY 重试"), w[18], 2);
    add(QStringLiteral("最近失败 state / step / register"), QStringLiteral("%1 / %2 / %3").arg(w[19] >> 28).arg((w[19] >> 23) & 31).arg((w[19] >> 16) & 31), 2);
    add(QStringLiteral("最近失败 read / ACK / data"), bit(w[19], 22) + " / " + ((w[19] & (1 << 22)) ? bit(w[19], 21) : QStringLiteral("不适用")) + " / " + hex(w[19] & 65535, 4), 2);
    add(QStringLiteral("MAC 链路 / ARP 对端有效"), bit(w[21], 1) + " / " + bit(w[21], 2), 2);
    add(QStringLiteral("TX 使能 / 数据包就绪"), bit(w[21], 0) + " / " + bit(w[21], 3), 2);
    add(QStringLiteral("ARP 应答待发 / TX 排空"), bit(w[21], 5) + " / " + bit(w[21], 6), 2);
    add(QStringLiteral("对端 MAC"), QStringLiteral("%1%2").arg(w[29] & 65535, 4, 16, QChar('0')).arg(w[28], 8, 16, QChar('0')).toUpper(), 2);
    add(QStringLiteral("TX 错误 / 帧错误"), bit(w[21], 4) + " / " + bit(w[21], 12), 2);
    add(QStringLiteral("TX BIST 通过 / 失败"), bit(w[21], 13) + " / " + bit(w[21], 14), 2);
    add(QStringLiteral("MAC state / 帧类型 / 流复位释放"), QStringLiteral("%1 / %2 / %3").arg((w[21] >> 7) & 7).arg((w[21] >> 10) & 3).arg(bit(w[21], 15)), 2);
    n(QStringLiteral("ARP 请求发送"), w[22], 2); n(QStringLiteral("ARP 回复发送"), w[23], 2); n(QStringLiteral("ARP 接受"), w[24], 2);
    n(QStringLiteral("UDP 发送（不代表 PC 接收）"), w[25], 2); n(QStringLiteral("Ethernet 发送"), w[26], 2); n(QStringLiteral("流 epoch"), w[27], 2);
    n(QStringLiteral("RX 帧（不保证 FCS）"), w[30], 3); n(QStringLiteral("RX_ER 帧"), w[31], 3);
    n(QStringLiteral("RX_DV 字节（含前导码/FCS）"), w[32], 3); n(QStringLiteral("RX 域周期"), w[33], 3);
    return rows;
}

bool Sys3Session::current(qint64 now) const { return connected && identified && !receiveBacklog && lastFrameMs >= 0 && now >= lastFrameMs && now - lastFrameMs <= 1500; }
QString Sys3Session::commandBlock(char command, qint64 now) const
{
    if (!QByteArray("shrtc").contains(command) || !command) return QStringLiteral("不支持的命令");
    if (captureBusy && (command == 'r' || command == 't')) return QStringLiteral("请先停止录制并等待写盘完成");
    if (receiveBacklog) return QStringLiteral("串口接收积压，等待排空并重新同步状态");
    if (!current(now)) return QStringLiteral("需要当前连接中的新鲜 SYS3 状态");
    if (pending || audioPending) return QStringLiteral("上一命令尚未完成，不能排队或自动重试");
    if (command == 't' && now - lastToggleMs < 200) return QStringLiteral("自测切换间隔至少 200 ms");
    if (command == 't' && (!latest.audioFresh() || !latest.ethFresh() || !latest.clocksHealthy()))
        return QStringLiteral("切换需要新鲜音频/以太网域及已恢复的时钟");
    return {};
}
bool Sys3Session::request(char command, qint64 now)
{
    const auto block = commandBlock(command, now);
    if (!block.isEmpty()) { commandStatus = block; return false; }
    pending = command; queued = true; acknowledged = false; commandMs = now;
    baseCommands = latest.commands(); baseMode = latest.selftest(); baseEpoch = latest.words[27];
    commandStatus = QStringLiteral("命令 %1：待发送").arg(QChar(command));
    appendLog(QStringLiteral("[%1 ms] TX 请求 %2").arg(now).arg(QChar(command))); return true;
}
void Sys3Session::appendLog(const QString &line)
{
    log.append(line); while (log.size() > 400) log.removeFirst();
}
char Sys3Session::takeCommand(qint64 now)
{
    if (!queued) return 0;
    queued = false;
    if (!current(now) || now - commandMs > 200 || (captureBusy && (pending == 'r' || pending == 't'))
        || (pending == 't' && (now - lastToggleMs < 200 || !latest.audioFresh() || !latest.ethFresh() || !latest.clocksHealthy()))) {
        failCommand(QStringLiteral("发送前状态已过期或切换条件不满足")); return 0;
    }
    // A physical key or another controller may change the device after the UI
    // request. Never transmit a toggle against a different mode/epoch baseline.
    if (latest.commands() != baseCommands
        || (pending == 't' && (latest.selftest() != baseMode || latest.words[27] != baseEpoch))) {
        failCommand(QStringLiteral("发送前命令计数、模式或 epoch 已改变，请核对设备状态")); return 0;
    }
    // Reserve at dispatch, then move the cooldown to successful write completion.
    // UI request time may precede the physical write by up to 200 ms.
    if (pending == 't') lastToggleMs = now;
    return pending;
}
void Sys3Session::commandWritten(qint64 now)
{
    if (!pending || queued) return;
    if (pending == 't') lastToggleMs = now;
    commandStatus = QStringLiteral("命令 %1 已发送 1 字节，等待识别回执").arg(QChar(pending));
    appendLog(commandStatus);
}

bool Sys3Session::audioCurrent(qint64 now) const
{
    return connected && audioIdentified && !receiveBacklog && lastAudioFrameMs >= 0
        && now >= lastAudioFrameMs && now - lastAudioFrameMs <= 1500;
}

QString Sys3Session::audioCommandBlock(const AudioUartCommand &command, qint64 now) const
{
    if (command.encode().isEmpty()) return QStringLiteral("参数越界：通道 0–127，增益 000000–7FFFFF");
    if (!current(now)) return QStringLiteral("需要当前连接中的新鲜 SYS3 状态");
    if (pending || audioPending) return QStringLiteral("上一命令尚未完成，请等待设备确认");
    if (!command.query() && !audioIdentified) return QStringLiteral("请先查询音频状态，识别新版音频控制协议");
    if (captureBusy && command.action == AudioUartAction::SetGain) return QStringLiteral("增益会改变采集音频，请先停止录制并等待写盘完成");
    return {};
}

bool Sys3Session::requestAudio(const AudioUartCommand &command, qint64 now)
{
    const auto reason = audioCommandBlock(command, now);
    if (!reason.isEmpty()) { audioCommandStatus = reason; return false; }
    audioCommand = command; audioPending = true; audioQueued = true; audioAwaitingReply = false;
    audioConfirmationQuery = false; audioCommandMs = now; audioNextQueryMs = -1;
    audioBaseCommands = latest.commands();
    audioPreservedChannel = audioCurrent(now) ? int(latestAudio.words[2] & 127) : -1;
    audioCommandStatus = command.description() + QStringLiteral("：待发送");
    appendLog(QStringLiteral("[%1 ms] TX 请求 %2").arg(now).arg(QString::fromLatin1(command.encode()).trimmed()));
    return true;
}

QByteArray Sys3Session::takeAudioCommand(qint64 now)
{
    tick(now);
    if (!audioPending || audioAwaitingReply) return {};
    if (!audioQueued && (audioNextQueryMs < 0 || now < audioNextQueryMs)) return {};
    if (!current(now) || latest.commands() != audioBaseCommands
        || (captureBusy && audioCommand.action == AudioUartAction::SetGain)
        || (audioQueued && now - audioCommandMs > 200)) {
        failAudioCommand(QStringLiteral("发送前状态过期、命令计数改变或录制互锁")); return {};
    }
    const bool first = audioQueued;
    audioQueued = false; audioAwaitingReply = true; audioNextQueryMs = -1;
    if (!first) audioConfirmationQuery = true;
    return first ? audioCommand.encode() : audioCommand.queryBytes();
}

void Sys3Session::audioCommandWritten(qint64 now)
{
    if (!audioPending || !audioAwaitingReply) return;
    audioCommandStatus = audioCommand.description() + (audioConfirmationQuery
        ? QStringLiteral("：已发送确认查询，等待当前状态") : QStringLiteral("：已发送，等待独立音频回执"));
    appendLog(QStringLiteral("[%1 ms] %2").arg(now).arg(audioCommandStatus));
}

void Sys3Session::failAudioCommand(const QString &reason)
{
    if (!audioPending) return;
    audioCommandStatus = audioCommand.description() + QStringLiteral("：") + reason + QStringLiteral("；设置不自动重试");
    appendLog(audioCommandStatus);
    audioPending = false; audioQueued = false; audioAwaitingReply = false; audioNextQueryMs = -1;
}

void Sys3Session::acceptAudio(const AudioUartFrame &frame, qint64 now)
{
    if (lastAudioFrameMs >= 0) {
        const quint32 delta = frame.words[0] - latestAudio.words[0];
        if (!delta) { ++audioDuplicates; return; }
        if (delta >= 0x80000000u) { ++invalid; return; }
        if (delta > 1) audioMissing += delta - 1;
    }
    latestAudio = frame; ++audioValid; audioIdentified = true; lastAudioFrameMs = now;
    if (!audioPending || !audioAwaitingReply || frame.gainResponse != audioCommand.gain()) return;
    audioAwaitingReply = false;
    if (audioCommand.query()) {
        audioCommandStatus = frame.errorCode() || frame.queueOverflows()
            ? QStringLiteral("查询已返回：%1；回应队列溢出 %2").arg(frame.errorDescription()).arg(frame.queueOverflows())
            : !frame.actualValid() ? QStringLiteral("查询已返回：音频域未就绪，实际值未知")
            : frame.pending() ? QStringLiteral("查询已返回：配置待生效，请稍后重新查询") : QStringLiteral("查询完成：已取得当前音频状态");
        audioPending = false; appendLog(audioCommandStatus); return;
    }
    if (frame.errorCode() || frame.queueOverflows()) {
        failAudioCommand(QStringLiteral("设备拒绝或队列告警：%1，溢出 %2").arg(frame.errorDescription()).arg(frame.queueOverflows())); return;
    }
    const auto &w = frame.words;
    bool requestedMatches = false, actualMatches = false;
    if (audioCommand.action == AudioUartAction::SetGain) {
        requestedMatches = (w[4] & 0xffffff) == audioCommand.evenGain && (w[5] & 0xffffff) == audioCommand.oddGain;
        actualMatches = (w[6] & 0xffffff) == audioCommand.evenGain && (w[7] & 0xffffff) == audioCommand.oddGain;
    } else {
        const quint32 requestedI2s = w[2] & 255;
        requestedMatches = audioCommand.action == AudioUartAction::SelectI2s ? requestedI2s == quint32(128 + audioCommand.channel)
            : requestedI2s <= 127 && (audioPreservedChannel < 0 || requestedI2s == quint32(audioPreservedChannel));
        actualMatches = (w[3] & 255) == requestedI2s;
    }
    if (!requestedMatches) { failAudioCommand(QStringLiteral("设备保存的请求值与本次设定不一致")); return; }
    if (audioConfirmationQuery && frame.actualValid() && !frame.pending()) {
        if (!actualMatches) { failAudioCommand(QStringLiteral("音频域实际值与请求值不一致")); return; }
        audioCommandStatus = audioCommand.description() + QStringLiteral("：已确认生效（新鲜、无等待、实际值一致）");
        audioPending = false; appendLog(audioCommandStatus); return;
    }
    // Only read-only queries follow a setting. Never resend a mutating command,
    // and never send another query until the preceding one has a response.
    audioNextQueryMs = now + 200;
    audioCommandStatus = frame.actualValid() && !frame.pending() ? QStringLiteral("请求值已保存，等待后续查询确认实际生效")
        : QStringLiteral("请求值已保存，音频域待生效；正在等待确认");
}
void Sys3Session::receiveDiscontinuity(const QString &reason)
{
    ++receiveDiscontinuities;
    identified = false;
    audioIdentified = false;
    m_buffer.clear();
    m_discarding = true;
    failCommand(reason + QStringLiteral("，执行结果未知"));
    appendLog(QStringLiteral("RX 失去同步：%1；丢弃至下一换行后重新识别 SYS3").arg(reason));
}
void Sys3Session::failCommand(const QString &reason)
{
    failAudioCommand(reason);
    if (!pending) return;
    commandStatus = QStringLiteral("命令 %1：%2；不自动重试").arg(QChar(pending), reason);
    appendLog(commandStatus); pending = 0; queued = false;
}
void Sys3Session::tick(qint64 now)
{
    if (audioPending && now - audioCommandMs > 5000) failAudioCommand(QStringLiteral("确认超时，实际生效状态未知"));
    if (pending && now - commandMs > 5000) failCommand(acknowledged ? QStringLiteral("已识别，但未确认后续状态") : QStringLiteral("回执超时，执行结果未知"));
}
void Sys3Session::feed(const QByteArray &data, qint64 now)
{
    // Expire first: a delayed reply must not turn an already timed-out command
    // into a success merely because it arrived in the same read as this tick.
    tick(now);
    bytes += quint64(data.size());
    for (char c : data) {
        if (!m_discarding) m_buffer.append(c);
        if (m_buffer.size() > 1024) { m_buffer.clear(); m_discarding = true; ++invalid; appendLog(QStringLiteral("RX 超长行已丢弃")); }
        if (c != '\n') continue;
        if (!m_discarding) {
            const auto frame = decodeSys3Line(m_buffer);
            appendLog(QStringLiteral("[%1 ms] RX %2").arg(now).arg(QString::fromLatin1(m_buffer).trimmed()));
            if (frame) accept(*frame, now);
            else if (const auto audio = decodeAudioUartLine(m_buffer)) acceptAudio(*audio, now);
            else {
                ++invalid;
                if (m_buffer.startsWith("CLK2")) { identified = false; failCommand(QStringLiteral("检测到不兼容 CLK2 协议")); }
            }
        }
        m_buffer.clear(); m_discarding = false;
    }
    tick(now);
}
void Sys3Session::accept(const Sys3Frame &frame, qint64 now)
{
    if (valid) {
        const quint32 delta = frame.words[0] - latest.words[0];
        const quint32 timeDelta = frame.words[1] - latest.words[1];
        // Uptime can restart even if the new sequence equals/exceeds the last
        // observed sequence. Modular subtraction still permits natural wraps.
        if (timeDelta >= 0x80000000u) {
            ++restarts; failCommand(QStringLiteral("观察域重启，命令结果未知"));
            audioIdentified = false; lastAudioFrameMs = -1;
        } else {
            if (!delta) { ++duplicates; return; } // duplicates never refresh liveness
            if (delta >= 0x80000000u) { ++invalid; return; }
            if (delta > 1) missing += delta - 1;
        }
    }
    latest = frame; identified = true; ++valid; lastFrameMs = now;
    if (audioPending && latest.commands() != audioBaseCommands) failAudioCommand(QStringLiteral("系统命令计数变化，可能存在外部干预"));
    if (!pending || queued) return;
    if (!acknowledged) {
        const quint16 count = quint16(frame.commands() - baseCommands);
        if (!count) return;
        if (count != 1 || frame.lastCommand() != pending) { failCommand(QStringLiteral("命令计数或字符不匹配，可能存在外部干预")); return; }
        acknowledged = true; ackSequence = frame.words[0];
        commandStatus = QStringLiteral("命令 %1 已识别；等待后续新鲜状态").arg(QChar(pending));
        if (pending == 's' || pending == 'h') {
            commandStatus = QStringLiteral("命令 %1 已识别，已收到 SYS3 快照；各域时效分别检查").arg(QChar(pending)); pending = 0;
        }
        return; // command recognition is NOT action completion
    }
    if (frame.words[0] == ackSequence) return;
    if (frame.commands() != quint16(baseCommands + 1) || frame.lastCommand() != pending) { failCommand(QStringLiteral("后续命令计数变化，无法确认本次动作")); return; }
    bool confirmed = false;
    if (pending == 'c') confirmed = ((frame.words[2] >> 15) & 15) == 0;
    const bool calibrationReady = (frame.words[8] & 0x600) == 0x600
        && bool(frame.words[8] & 0x800) == frame.selftest();
    if (pending == 't') confirmed = frame.audioFresh() && frame.ethFresh() && frame.clocksHealthy()
        && frame.selftest() != baseMode && frame.words[27] != baseEpoch && calibrationReady;
    if (pending == 'r') confirmed = frame.audioFresh() && frame.clocksHealthy() && !frame.selftest() && calibrationReady;
    if (confirmed) {
        commandStatus = pending == 'r' ? QStringLiteral("复位命令已识别；后续时钟/校准已恢复实采，非复位脉宽测量")
            : pending == 't' ? QStringLiteral("已确认模式切换、新 epoch 与校准完成") : QStringLiteral("已确认时钟历史掩码清零；其他故障与计数未清除");
        appendLog(commandStatus); pending = 0;
    }
}
QJsonObject Sys3Session::json(qint64 now) const
{
    const bool fresh = current(now);
    return {{"format", "micarray128-sys3-v1"}, {"port", port}, {"baud", 115200}, {"connected", connected},
        {"identified", identified}, {"current", fresh}, {"last_frame_age_ms", lastFrameMs < 0 ? -1 : now - lastFrameMs},
        {"valid_frames", QString::number(valid)}, {"invalid_lines", QString::number(invalid)}, {"received_bytes", QString::number(bytes)},
        {"missing_frames", QString::number(missing)}, {"duplicate_frames", QString::number(duplicates)}, {"observer_restarts", QString::number(restarts)},
        {"receive_backlog", receiveBacklog}, {"receive_discontinuities", QString::number(receiveDiscontinuities)},
        {"audio_current", fresh && latest.audioFresh()}, {"eth_current", fresh && latest.ethFresh()}, {"rx_current", fresh && latest.rxFresh()},
        {"latest", valid ? QJsonValue(latest.json()) : QJsonValue(QJsonValue::Null)}, {"error", error},
        {"command_pending", bool(pending) || audioPending}, {"command_status", audioPending ? audioCommandStatus : commandStatus}, {"recent_log", QJsonArray::fromStringList(log)},
        {"capture_busy", captureBusy},
        {"audio_uart", QJsonObject{{"identified", audioIdentified}, {"current", audioCurrent(now)},
            {"valid_frames", QString::number(audioValid)}, {"missing_frames", QString::number(audioMissing)},
            {"duplicate_frames", QString::number(audioDuplicates)}, {"last_frame_age_ms", lastAudioFrameMs < 0 ? -1 : now - lastAudioFrameMs},
            {"latest", audioValid ? QJsonValue(latestAudio.json(audioCurrent(now))) : QJsonValue(QJsonValue::Null)},
            {"command_pending", audioPending}, {"command_status", audioCommandStatus}}},
        {"log_limit_lines", 400}, {"scope", "FPGA UART telemetry; separate from UDP PC capture, firmware identity and electrical verification"}};
}

} // namespace MicArray::Array128
