#include "diagnostics.h"

#include <QCryptographicHash>
#include <QFile>

namespace MicArray::Array128 {

SequenceTracker::Event SequenceTracker::observe(quint64 value)
{
    if (!m_started) {
        m_started = true;
        m_first = value;
        m_highest = 0;
        m_unique = 1;
        m_ranges.emplace(0, 0);
        return Event::First;
    }
    const quint64 position = value - m_first;
    if (position >= (quint64(1) << 63)) { ++beforeStart; return Event::BeforeStart; }
    auto right = m_ranges.upper_bound(position);
    auto left = right == m_ranges.begin() ? m_ranges.end() : std::prev(right);
    if (left != m_ranges.end() && left->second >= position) {
        ++duplicates;
        return Event::Duplicate;
    }
    Event event = Event::Reordered;
    if (position > m_highest) {
        const quint64 gap = position - m_highest - 1;
        forwardGaps += gap;
        event = gap ? Event::Gap : Event::Continuous;
        m_highest = position;
    } else ++reordered;
    if (left != m_ranges.end() && left->second + 1 == position) {
        left->second = position;
        if (right != m_ranges.end() && right->first == position + 1) {
            left->second = right->second;
            m_ranges.erase(right);
        }
    } else if (right != m_ranges.end() && right->first == position + 1) {
        const auto end = right->second;
        m_ranges.erase(right);
        m_ranges.emplace(position, end);
    } else {
        if (m_ranges.size() >= 65536) { exact = false; return Event::CapacityExceeded; }
        m_ranges.emplace(position, position);
    }
    ++m_unique;
    return event;
}

quint64 SequenceTracker::missing() const
{
    return m_started ? m_highest + 1 - m_unique : 0;
}

QJsonObject SequenceTracker::json() const
{
    return {{"first", QString::number(m_first)}, {"highest", QString::number(m_first + m_highest)},
        {"unique", QString::number(m_unique)}, {"missing_observed_range", QString::number(missing())},
        {"forward_gap_observations", QString::number(forwardGaps)},
        {"duplicates", QString::number(duplicates)}, {"out_of_order", QString::number(reordered)},
        {"before_capture", QString::number(beforeStart)}, {"exact", exact}};
}

QByteArray selftestReference()
{
    QFile file(QStringLiteral(":/m128/resources/selftest_pcm.bin"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

SelftestAudit::SelftestAudit(const QByteArray &reference) : m_reference(reference)
{
    referenceValid = reference.size() == 16 * PcmFrameBytes
        && QCryptographicHash::hash(reference, QCryptographicHash::Sha256).toHex()
            == "fb653a4b25043495c2f54b36fbe9df7a9d9d6df06cbb95ce1d888bec444dc75e";
}

void SelftestAudit::observe(const Packet &packet, bool continuous, bool hostDropped)
{
    if (!packet.selftest()) return;
    hostLoss |= hostDropped;
    if (packet.flags & ErrorFlags) ++flaggedPackets;
    if (frames && !continuous) ++continuityErrors;
    for (int r = 0; r < 3; ++r) {
        if (r && packet.frameIds[r] != packet.frameIds[r - 1] + 1) ++continuityErrors;
        ++frames;
        if (!referenceValid) continue;
        const char *expected = m_reference.constData() + (packet.frameIds[r] % 16) * PcmFrameBytes;
        if (memcmp(expected, packet.pcm(r), PcmFrameBytes) == 0) continue;
        ++mismatchingFrames;
        for (int ch = 0; ch < Channels; ++ch) {
            const qint32 actual = packet.sample(r, ch), wanted = readS24(expected + 3 * ch);
            if (actual == wanted) continue;
            ++mismatchingSamples;
            ++channelMismatches[ch];
            if (firstMismatch.isEmpty()) firstMismatch = QStringLiteral("frame_id=%1 CH%2：实际 %3，预期 %4")
                .arg(packet.frameIds[r]).arg(ch).arg(actual).arg(wanted);
        }
    }
}

QString SelftestAudit::status() const
{
    if (!referenceValid) return QStringLiteral("不可验证：参考表缺失或 SHA-256 错误");
    if (mismatchingFrames || continuityErrors || flaggedPackets || hostLoss) return QStringLiteral("FAIL");
    if (frames >= 48000) return QStringLiteral("PASS（PC 数字链路，不代表声学验收）");
    return frames ? QStringLiteral("校验中：%1 / 48000 帧").arg(frames) : QStringLiteral("等待自测数据（bit4=1）");
}

QJsonObject SelftestAudit::json() const
{
    return {{"status", status()}, {"reference_valid", referenceValid}, {"frames", QString::number(frames)},
        {"mismatching_frames", QString::number(mismatchingFrames)},
        {"mismatching_samples", QString::number(mismatchingSamples)},
        {"continuity_errors", QString::number(continuityErrors)},
        {"error_flag_packets", QString::number(flaggedPackets)}, {"host_loss", hostLoss},
        {"first_mismatch", firstMismatch}};
}

} // namespace MicArray::Array128
