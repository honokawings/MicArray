#include "protocol.h"

#include <QStringList>
#include <QtEndian>

namespace MicArray::Array128 {

qint32 readS24(const char *bytes)
{
    const auto *p = reinterpret_cast<const uchar *>(bytes);
    const quint32 value = quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16);
    return value & 0x800000 ? qint32(value) - 0x1000000 : qint32(value);
}

void appendS24(QByteArray &bytes, qint32 value)
{
    const quint32 v = quint32(value);
    bytes.append(char(v));
    bytes.append(char(v >> 8));
    bytes.append(char(v >> 16));
}

qint32 Packet::sample(int frame, int channel) const
{
    return readS24(pcm(frame) + 3 * channel);
}

std::optional<Packet> decodeDatagram(const QByteArray &bytes, QString *error)
{
    auto fail = [&](const QString &reason) -> std::optional<Packet> {
        if (error) *error = reason;
        return std::nullopt;
    };
    if (bytes.size() != DatagramBytes)
        return fail(QStringLiteral("UDP 长度 %1，必须是 1208 字节").arg(bytes.size()));
    const auto *p = reinterpret_cast<const uchar *>(bytes.constData());
    if (bytes.first(4) != "M128") return fail(QStringLiteral("magic 不是 M128"));
    if (p[4] != 1 || p[5] != 32 || p[6] != 1 || p[7] != 128)
        return fail(QStringLiteral("版本 / 头长 / 编码 / 通道数不匹配 M128 v1"));
    if (qFromBigEndian<quint32>(p + 8) != SampleRate
        || qFromBigEndian<quint16>(p + 12) != 3
        || qFromBigEndian<quint16>(p + 14) != 392)
        return fail(QStringLiteral("采样率 / 帧数 / 记录长度错误"));
    Packet result;
    result.bytes = bytes;
    result.sequence = qFromBigEndian<quint64>(p + 16);
    result.epoch = qFromBigEndian<quint32>(p + 24);
    result.flags = qFromBigEndian<quint32>(p + 28);
    if (result.flags & ~0x3fu) return fail(QStringLiteral("保留 flags 非零，拒绝未知协议"));
    if ((result.flags & 0x20) && !(result.flags & 0x10))
        return fail(QStringLiteral("selftest_mismatch 未同时设置 selftest_data"));
    for (int i = 0; i < 3; ++i) result.frameIds[i] = qFromBigEndian<quint64>(p + 32 + i * 392);
    if (error) error->clear();
    return result;
}

QString flagsDescription(quint32 flags)
{
    const QStringList names {QStringLiteral("前处理饱和"), QStringLiteral("前处理队列溢出"),
        QStringLiteral("处理调度异常"), QStringLiteral("打包结构异常"),
        QStringLiteral("数字自测模式"), QStringLiteral("FPGA 自测不匹配")};
    QStringList active;
    for (int i = 0; i < names.size(); ++i) if (flags & (1u << i)) active << names[i];
    return active.isEmpty() ? QStringLiteral("正常实采标志") : active.join(QStringLiteral(" / "));
}

double effectiveTimeSeconds(quint64 frameId)
{
    return (double(frameId) * 64.0 + 63.0 - 2285.5) / 3072000.0;
}

} // namespace MicArray::Array128
