#include "media/pldenoisetransport.h"

#include "media/audioframedecoder.h"

#include <QtEndian>

#include <bit>
#include <cmath>

namespace MicArray {
namespace {
quint16 u16(const QByteArray &packet, int offset)
{
    return qFromLittleEndian<quint16>(packet.constData() + offset);
}
quint32 u32(const QByteArray &packet, int offset)
{
    return qFromLittleEndian<quint32>(packet.constData() + offset);
}
bool zeroRange(const QByteArray &packet, int begin, int end)
{
    for (int index = begin; index < end; ++index)
    {
        if (packet.at(index) != 0) return false;
    }
    return true;
}
} // namespace

PlDenoiseTransport::PlDenoiseTransport(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<PlDenoiseStatus>();
    qRegisterMetaType<PlBeamFrame>();
    qRegisterMetaType<PlWienerFrame>();
    qRegisterMetaType<PlFinalFrame>();
}

quint8 PlDenoiseTransport::crc8(const QByteArray &bytes)
{
    quint8 crc = 0;
    for (char byte : bytes)
    {
        crc ^= static_cast<quint8>(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<quint8>((crc & 0x80U) ? (crc << 1) ^ 0x07U : crc << 1);
    }
    return crc;
}

void PlDenoiseTransport::clearStream()
{
    m_haveSequence = false;
    for (auto &samples : m_buffer) samples.clear();
    m_lastAudio.invalidate();
    m_pendingPackets.clear();
    m_pendingSamples = 0;
    m_havePendingConfig = false;
}

void PlDenoiseTransport::setPendingConfiguration(quint16 configId)
{
    if (m_havePendingConfig && m_pendingConfig != configId && !m_pendingPackets.isEmpty()) {
        invalidate(QStringLiteral("含未确认音频的配置事务被替换"));
        return;
    }
    m_pendingConfig = configId;
    m_havePendingConfig = true;
}

void PlDenoiseTransport::reset()
{
    clearStream();
    m_authorized = false;
    m_deviceReady = false;
    m_faulted = false;
    m_controlTransactionActive = false;
    m_receivedSamples = 0;
    m_rejectedPackets = 0;
}

void PlDenoiseTransport::setExpectedStage(int stage)
{
    if (stage < 1 || stage > 3) return;
    if (stage != m_expectedStage) reset();
    m_expectedStage = stage;
}

void PlDenoiseTransport::flushBufferedBeforeConfiguration()
{
    // Publish an old epoch's partial tail before the page queues the next
    // worker authorization. In particular, multiple short direction epochs
    // must never be delivered behind a later configuration barrier.
    if (m_authorized && !m_faulted) publishBuffered();
}

void PlDenoiseTransport::authorizeConfiguration(quint16 configId, bool restartStream)
{
    const quint16 activeStream = m_expectedStage > 1 ? m_latestStatus.streamId : configId;
    if (m_expectedStage > 1 && (m_latestStatus.stage != m_expectedStage || activeStream == 0
        || m_latestStatus.activeConfigId != configId)) {
        invalidate(QStringLiteral("PL 单声道配置确认缺少有效 stream_id 或阶段不匹配"));
        return;
    }
    QVector<PendingPacket> pending;
    if (m_havePendingConfig && m_pendingConfig == configId)
        pending = std::move(m_pendingPackets);
    if (restartStream || m_faulted || !m_authorized)
    {
        clearStream();
        m_streamConfig = activeStream;
    }
    m_authorized = true;
    m_authorizedConfig = activeStream;
    m_faulted = false;
    m_pendingPackets.clear();
    m_pendingSamples = 0;
    m_havePendingConfig = false;
    for (const auto &packet : pending) {
        consumePacket(packet.bytes, packet.timestamp);
        if (m_faulted) break;
    }
}

void PlDenoiseTransport::invalidate(const QString &reason)
{
    ++m_rejectedPackets;
    if (m_faulted) return;
    m_faulted = true;
    for (auto &samples : m_buffer) samples.clear();
    m_pendingPackets.clear();
    m_pendingSamples = 0;
    emit faultRaised(reason + QStringLiteral("；PL 音频已暂停，请重新同步方向配置"));
}

void PlDenoiseTransport::consume(const QByteArray &payload, quint32 seconds, quint32 micros)
{
    if (payload.size() != AudioFrameDecoder::FramePayloadBytes)
    {
        invalidate(QStringLiteral("PL 音频原始帧长度错误"));
        return;
    }
    const qint64 timestamp = qint64(seconds) * 1000000 + micros;
    for (int sample = 0; sample < AudioFrameDecoder::SamplesPerFrame; ++sample)
    {
        QByteArray packet(41, Qt::Uninitialized);
        for (int channel = 7; channel < 48; ++channel)
            packet[channel - 7] = payload.at(sample * 192 + channel * 4 + 3);
        consumePacket(packet, timestamp + qint64(sample) * 1000000 / 48000);
    }
}

void PlDenoiseTransport::consumePacket(const QByteArray &packet, qint64 timestamp)
{
    const bool final = packet.startsWith("AUF1");
    const bool mono = packet.startsWith("AUW1") || final;
    const bool audio = packet.startsWith("AUD3") || mono;
    const bool statusPacket = packet.startsWith("DNS1");
    if (!audio && !statusPacket) return; // LOC3/HMP1 keep their existing owners.
    if (crc8(packet.first(40)) != static_cast<quint8>(packet.at(40)))
    {
        invalidate(QStringLiteral("%1 CRC8 校验失败").arg(audio ? (final ? "AUF1" : (mono ? "AUW1" : "AUD3")) : "DNS1"));
        return;
    }
    if (static_cast<quint8>(packet.at(4)) != 1)
    {
        invalidate(QStringLiteral("不支持的 PL 降噪传输版本"));
        return;
    }
    if (statusPacket)
    {
        const quint8 stage = static_cast<quint8>(packet.at(5));
        const quint32 build = u32(packet, 25);
        const bool legacy = build == 0x20261020U && stage == 1 && m_expectedStage == 1 && zeroRange(packet, 31, 40);
        const bool extended = ((build == 0x20261021U && (stage == 1 || stage == 2))
            || (build == 0x20261022U && stage >= 1 && stage <= 3)) && zeroRange(packet, 37, 40);
        if ((!legacy && !extended) || (static_cast<quint8>(packet.at(12)) & ~7U) != 0)
        {
            invalidate(QStringLiteral("DNS1 阶段、build id 或保留字段错误"));
            return;
        }
        PlDenoiseStatus status;
        status.activeConfigId = u16(packet, 6);
        status.ackTransaction = u16(packet, 8);
        status.ackField = u16(packet, 10);
        status.flags = static_cast<quint8>(packet.at(12));
        status.latestSampleIndex = u32(packet, 13);
        status.overflowCount = u32(packet, 17);
        status.invalidControlCount = u32(packet, 21);
        status.buildId = u32(packet, 25);
        status.errorCode = u16(packet, 29);
        status.stage = stage;
        status.streamId = extended ? u16(packet, 31) : status.activeConfigId;
        status.wienerFrames = extended && stage != 3 ? u32(packet, 33) : 0;
        status.profileFingerprint = stage == 3 ? u32(packet, 33) : 0;
        if (stage == 3 && status.profileFingerprint != FullProfileFingerprint) {
            invalidate(QStringLiteral("PL 全链演示参数指纹不匹配，拒绝能力未经确认的最终音频"));
            return;
        }
        m_latestStatus = status;
        m_deviceReady = status.ready() && status.stage == m_expectedStage;
        emit statusReceived(status); // ACK may authorize the next packet in this same raw frame.
        const bool transientControlCrc = m_controlTransactionActive && status.errorCode == 1 && status.overflowCount == 0;
        if (m_authorized && !transientControlCrc
            && (status.overflowCount != 0 || status.errorCode != 0 || (status.flags & 4U) != 0))
            invalidate(QStringLiteral("PL 报告音频溢出 %1 / 错误 %2")
                .arg(status.overflowCount).arg(status.errorCode));
        return;
    }
    if (mono) {
        if (m_expectedStage != (final ? 3 : 2)) {
            if (m_authorized) invalidate(QStringLiteral("PL 音频阶段与选择模式不符，拒绝替代或重复处理"));
            return;
        }
        consumeWienerPacket(packet, timestamp);
        return;
    }
    if (m_expectedStage > 1) {
        if (m_authorized && m_deviceReady)
            invalidate(QStringLiteral("PL 单声道模式收到三波束，禁止重复或替代处理"));
        return;
    }
    if (m_faulted) return;
    const int count = static_cast<quint8>(packet.at(12));
    if (static_cast<quint8>(packet.at(5)) != 0x11 || (count != 1 && count != 2)
        || !zeroRange(packet, 37, 40) || (count == 1 && !zeroRange(packet, 25, 37)))
    {
        invalidate(QStringLiteral("AUD3 格式、tuple 数量或保留字段错误"));
        return;
    }
    const quint16 config = u16(packet, 6);
    const quint32 firstSample = u32(packet, 8);
    std::array<std::array<float, 3>, 2> values {};
    for (int tuple = 0; tuple < count; ++tuple)
        for (int beam = 0; beam < 3; ++beam)
        {
            values[tuple][beam] = std::bit_cast<float>(u32(packet, 13 + tuple * 12 + beam * 4));
            if (!std::isfinite(values[tuple][beam]))
            {
                invalidate(QStringLiteral("AUD3 含非有限浮点样本"));
                return;
            }
        }
    if (m_havePendingConfig && config == m_pendingConfig && (!m_authorized || config != m_authorizedConfig)) {
        const quint32 expected = m_pendingPackets.isEmpty() ? m_nextSample : m_pendingNextSample;
        if ((!m_pendingPackets.isEmpty() || m_haveSequence) && firstSample != expected) {
            invalidate(QStringLiteral("待确认 AUD3 配置存在样本缺口或重复"));
            return;
        }
        if (m_pendingSamples + count > 96000) {
            invalidate(QStringLiteral("待确认 AUD3 超过两秒缓存上限，拒绝无界等待"));
            return;
        }
        m_pendingPackets.append({packet, timestamp});
        m_pendingNextSample = firstSample + count;
        m_pendingSamples += count;
        m_lastAudio.restart();
        return;
    }
    if (!m_authorized || !m_deviceReady) return;
    if (!m_pendingPackets.isEmpty()) {
        invalidate(QStringLiteral("新配置音频后又收到旧配置音频，拒绝交错 epoch"));
        return;
    }
    if (config != m_authorizedConfig && config != m_streamConfig) {
        invalidate(QStringLiteral("AUD3 未确认或过期配置 %1（已确认 %2）")
            .arg(config).arg(m_authorizedConfig));
        return;
    }
    if (m_haveSequence && firstSample != m_nextSample) {
        invalidate(QStringLiteral("AUD3 样本缺口/重复：期望 %1，收到 %2")
            .arg(m_nextSample).arg(firstSample));
        return;
    }
    if (!m_haveSequence)
    {
        m_originSample = firstSample;
        m_originTimestamp = timestamp;
        m_streamConfig = config;
        m_nextSample = firstSample;
        m_haveSequence = true;
    }
    if (config != m_streamConfig)
    {
        publishBuffered(); // Preserve every old-epoch sample; never pad or mix configurations.
        m_streamConfig = config;
    }
    for (int tuple = 0; tuple < count; ++tuple)
    {
        if (m_buffer[0].isEmpty()) m_bufferFirst = m_nextSample;
        for (int beam = 0; beam < 3; ++beam) m_buffer[beam].append(values[tuple][beam]);
        ++m_nextSample;
        ++m_receivedSamples;
        if (m_buffer[0].size() == AudioFrameDecoder::SamplesPerFrame) publishBuffered();
    }
    m_lastAudio.restart();
}

void PlDenoiseTransport::publishBuffered()
{
    if (m_buffer[0].isEmpty()) return;
    const qint64 timestamp = m_originTimestamp
        + qint64(quint32(m_bufferFirst - m_originSample)) * 1000000 / 48000;
    if (m_expectedStage > 1) {
        PlWienerFrame frame;
        frame.samples = std::move(m_buffer[0]);
        m_buffer[0].clear();
        frame.firstSampleIndex = m_bufferFirst;
        frame.streamId = m_streamConfig;
        frame.timestampSeconds = static_cast<quint32>(timestamp / 1000000);
        frame.timestampMicros = static_cast<quint32>(timestamp % 1000000);
        if (m_expectedStage == 3) {
            PlFinalFrame final;
            final.samples = std::move(frame.samples);
            final.firstSampleIndex = frame.firstSampleIndex;
            final.streamId = frame.streamId;
            final.timestampSeconds = frame.timestampSeconds;
            final.timestampMicros = frame.timestampMicros;
            emit finalFrameReady(final);
        } else emit wienerFrameReady(frame);
        return;
    }
    PlBeamFrame frame;
    frame.beams = std::move(m_buffer);
    for (auto &samples : m_buffer) samples.clear();
    frame.firstSampleIndex = m_bufferFirst;
    frame.configId = m_streamConfig;
    frame.timestampSeconds = static_cast<quint32>(timestamp / 1000000);
    frame.timestampMicros = static_cast<quint32>(timestamp % 1000000);
    emit frameReady(frame);
}

void PlDenoiseTransport::consumeWienerPacket(const QByteArray &packet, qint64 timestamp)
{
    if (m_faulted) return;
    const int count = static_cast<quint8>(packet.at(12));
    if (static_cast<quint8>(packet.at(5)) != (m_expectedStage == 3 ? 0x31 : 0x21) || count < 1 || count > 6
        || !zeroRange(packet, 13 + count * 4, 40)) {
        invalidate(QStringLiteral("PL 单声道格式、样本数量或保留字段错误"));
        return;
    }
    QVector<float> samples;
    for (int index = 0; index < count; ++index) {
        const float value = std::bit_cast<float>(u32(packet, 13 + index * 4));
        if (!std::isfinite(value)) { invalidate(QStringLiteral("PL 单声道含非有限浮点样本")); return; }
        samples.append(value);
    }
    const quint16 stream = u16(packet, 6);
    const quint32 first = u32(packet, 8);
    if (m_havePendingConfig && stream == m_pendingConfig && (!m_authorized || stream != m_authorizedConfig)) {
        const quint32 expected = m_pendingPackets.isEmpty() ? 0 : m_pendingNextSample;
        if (first != expected || m_pendingSamples + count > 96000) {
            invalidate(QStringLiteral("待确认 PL 单声道样本不连续或超过两秒缓存上限"));
            return;
        }
        m_pendingPackets.append({packet, timestamp});
        m_pendingNextSample = first + count;
        m_pendingSamples += count;
        m_lastAudio.restart();
        return;
    }
    if (!m_authorized || !m_deviceReady) return;
    if (stream != m_authorizedConfig) {
        ++m_rejectedPackets;
        emit staleStreamDiscarded(stream);
        return;
    }
    if ((!m_haveSequence && first != 0) || (m_haveSequence && first != m_nextSample)) {
        invalidate(QStringLiteral("PL 单声道样本缺口/重复或新流未从索引 0 开始"));
        return;
    }
    if (!m_haveSequence) {
        m_originSample = first;
        m_originTimestamp = timestamp;
        m_nextSample = first;
        m_streamConfig = stream;
        m_haveSequence = true;
    }
    for (float value : samples) {
        if (m_buffer[0].isEmpty()) m_bufferFirst = m_nextSample;
        m_buffer[0].append(value);
        ++m_nextSample;
        ++m_receivedSamples;
        if (m_buffer[0].size() == 200) publishBuffered();
    }
    m_lastAudio.restart();
}

} // namespace MicArray
