#pragma once

#include <QBitArray>
#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QStringList>
#include <QVector>

namespace MicArray {

struct PlSpatialMap
{
    quint16 sequence = 0;
    quint16 locatorFrameCounter = 0;
    quint8 sourceMask = 0;
    int columns = 37;
    int rows = 37;
    double reliability = 0.0;
    QVector<float> values;
    qint64 timestampMicroseconds = 0;
    quint64 streamEpoch = 0;
};

class PlSpatialMapAssembler
{
public:
    QList<PlSpatialMap> consume(const QByteArray &payload,
                               quint32 seconds,
                               quint32 micros,
                               QStringList *warnings = nullptr);
    void reset();

private:
    PlSpatialMap m_map;
    QByteArray m_bytes;
    QBitArray m_seen;
    int m_received = 0;
    bool m_hasMap = false;
    bool m_complete = false;
    bool m_corrupt = false;
    qint64 m_lastTimestamp = -1;
};

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::PlSpatialMap)
