#pragma once

#include "analysis/localizationtypes.h"

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace MicArray {

struct PlLocationResult
{
    bool locationValid = false;
    quint8 sourceId = 0;
    quint8 version = 0;
    quint8 flags = 0;
    quint8 recordType = 0;
    quint32 sequence = 0;
    quint32 locatorFrameCounter = 0;
    quint16 imageX = 0;
    quint16 imageY = 0;
    qint16 azimuthCentiDegrees = 0;
    qint16 elevationCentiDegrees = 0;
    quint16 confidenceQ15 = 0;
    qint32 score = 0;
    quint32 timestampSeconds = 0;
    quint32 timestampMicros = 0;
    int samplePoint = 0;
    quint64 streamEpoch = 0;

    double azimuthDegrees() const;
    double elevationDegrees() const;
    double confidence() const;
    // v4 carries physical angles. Legacy pixels use the fixed PL scan camera.
    SoundSourceDirection physicalDirection() const;
};

struct AudioLocationSidebandResult
{
    bool valid = false;
    QString error;
    QByteArray pcmPayload;
    QByteArray processedPcm16;
    int processedSampleCount = 0;
    QList<PlLocationResult> locations;
    QStringList warnings;
};

class AudioLocationSidebandDecoder
{
public:
    static constexpr int RecordBytes = 48;
    static constexpr int Loc3PacketBytes = 41;
    static constexpr int ImageWidth = 1920;
    static constexpr int ImageHeight = 1080;

    static AudioLocationSidebandResult decode(const QByteArray &payload,
                                              quint32 timestampSeconds,
                                              quint32 timestampMicros);
};

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::PlLocationResult)
