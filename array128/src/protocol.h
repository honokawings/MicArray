#pragma once

#include <QByteArray>
#include <QString>
#include <array>
#include <optional>

namespace MicArray::Array128 {

inline constexpr int Channels = 128;
inline constexpr int SampleRate = 48000;
inline constexpr int FramesPerPacket = 3;
inline constexpr int PcmFrameBytes = 384;
inline constexpr int DatagramBytes = 1208;
inline constexpr quint32 ErrorFlags = 0x2f;

struct Packet
{
    QByteArray bytes;
    quint64 sequence = 0;
    quint32 epoch = 0;
    quint32 flags = 0;
    std::array<quint64, 3> frameIds {};
    bool selftest() const { return flags & 0x10; }
    const char *pcm(int frame) const { return bytes.constData() + 40 + frame * 392; }
    qint32 sample(int frame, int channel) const;
};

qint32 readS24(const char *bytes);
void appendS24(QByteArray &bytes, qint32 value);
std::optional<Packet> decodeDatagram(const QByteArray &bytes, QString *error = nullptr);
QString flagsDescription(quint32 flags);
double effectiveTimeSeconds(quint64 frameId);

} // namespace MicArray::Array128
