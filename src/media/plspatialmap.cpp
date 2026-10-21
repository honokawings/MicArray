#include "media/plspatialmap.h"

#include <QtEndian>

namespace MicArray {
namespace {

quint8 crc8(const QByteArray &bytes)
{
    quint8 crc = 0;
    for (int index = 0; index < 40; ++index)
    {
        crc ^= static_cast<quint8>(bytes.at(index));
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 0x80u) != 0 ? (crc << 1) ^ 0x07u : crc << 1;
        }
    }
    return crc;
}

quint16 read16(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

} // namespace

void PlSpatialMapAssembler::reset()
{
    m_map = {};
    m_bytes.clear();
    m_seen.clear();
    m_received = 0;
    m_hasMap = false;
    m_complete = false;
    m_corrupt = false;
    m_lastTimestamp = -1;
}

QList<PlSpatialMap> PlSpatialMapAssembler::consume(
    const QByteArray &payload, quint32 seconds, quint32 micros, QStringList *warnings)
{
    QList<PlSpatialMap> results;
    constexpr int GroupBytes = 48 * 4;
    if (payload.isEmpty() || payload.size() % GroupBytes != 0 || micros >= 1'000'000)
    {
        return results;
    }
    const qint64 timestamp = qint64(seconds) * 1'000'000 + micros;
    if (m_lastTimestamp >= 0 && timestamp < m_lastTimestamp)
    {
        reset();
    }
    m_lastTimestamp = timestamp;
    const auto warn = [warnings](const QString &message)
    {
        if (warnings != nullptr)
        {
            warnings->append(message);
        }
    };
    for (int group = 0; group < payload.size(); group += GroupBytes)
    {
        QByteArray packet(41, '\0');
        for (int byte = 0; byte < 41; ++byte)
        {
            packet[byte] = payload.at(group + (byte + 7) * 4 + 3);
        }
        if (packet.left(4) != QByteArrayLiteral("HMP1"))
        {
            continue;
        }
        if (crc8(packet) != static_cast<quint8>(packet.at(40)))
        {
            warn(QStringLiteral("PL heatmap CRC failure; incomplete map withheld"));
            continue;
        }
        const quint8 flags = static_cast<quint8>(packet.at(5));
        const quint8 mask = flags & 7u;
        const quint16 sequence = read16(packet, 6);
        const quint16 frame = read16(packet, 8);
        const int offset = read16(packet, 10);
        const int length = static_cast<quint8>(packet.at(12));
        if (packet.at(4) != 1 || packet.at(13) != 37 || packet.at(14) != 37
            || length > 25 || offset + length > 1369
            || (mask == 0 && (offset != 0 || length != 0))
            || (mask != 0 && length == 0))
        {
            warn(QStringLiteral("PL heatmap header invalid"));
            continue;
        }
        if (m_hasMap && sequence != m_map.sequence)
        {
            const quint16 delta = sequence - m_map.sequence;
            const quint16 frameDelta = frame - m_map.locatorFrameCounter;
            if (delta >= 0x8000u || frameDelta == 0 || frameDelta >= 0x8000u)
            {
                continue;
            }
        }
        if (!m_hasMap || sequence != m_map.sequence)
        {
            m_map = {};
            m_map.sequence = sequence;
            m_map.locatorFrameCounter = frame;
            m_map.sourceMask = mask;
            m_map.reliability = (flags >> 3) / 31.0;
            m_bytes = QByteArray(1369, '\0');
            m_seen = QBitArray(1369);
            m_received = 0;
            m_hasMap = true;
            m_complete = false;
            m_corrupt = false;
        }
        if (m_complete || m_corrupt)
        {
            continue;
        }
        if (frame != m_map.locatorFrameCounter || mask != m_map.sourceMask
            || m_map.reliability != (flags >> 3) / 31.0)
        {
            m_corrupt = true;
            warn(QStringLiteral("PL heatmap mixed frame metadata; map withheld"));
            continue;
        }
        if (mask == 0)
        {
            m_map.values.clear();
            m_map.timestampMicroseconds = timestamp;
            results.append(m_map);
            m_complete = true;
            continue;
        }
        for (int cell = 0; cell < length; ++cell)
        {
            const int index = offset + cell;
            const char value = packet.at(15 + cell);
            if (m_seen.testBit(index))
            {
                if (m_bytes.at(index) != value)
                {
                    m_corrupt = true;
                }
            }
            else
            {
                m_seen.setBit(index);
                m_bytes[index] = value;
                ++m_received;
            }
        }
        if (m_received == 1369 && !m_corrupt)
        {
            m_map.values.resize(1369);
            for (int cell = 0; cell < 1369; ++cell)
            {
                m_map.values[cell] = static_cast<quint8>(m_bytes.at(cell)) / 255.0f;
            }
            m_map.timestampMicroseconds = timestamp;
            results.append(m_map);
            m_complete = true;
        }
    }
    return results;
}

} // namespace MicArray
