#include "media/audiolocationsideband.h"

#include "media/audioframedecoder.h"
#include "analysis/directionprojection.h"

#include <QtEndian>

#include <algorithm>

namespace MicArray {
namespace {

constexpr quint32 Pcm24Mask = 0x00ffffffu;
constexpr quint32 Pcm24SignBit = 0x00800000u;

quint8 byteAt(const QByteArray &bytes, int offset)
{
    return static_cast<quint8>(bytes.at(offset));
}

quint16 readU16(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

qint16 readI16(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<qint16>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

quint32 readU32(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

qint32 readI32(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<qint32>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

qint32 signExtendPcm24(quint32 word)
{
    const quint32 raw24 = word & Pcm24Mask;
    return static_cast<qint32>((raw24 & Pcm24SignBit) != 0
                                  ? raw24 | 0xff000000u
                                  : raw24);
}

bool hasLocationMagic(const QByteArray &record)
{
    return record.size() == AudioLocationSidebandDecoder::RecordBytes
        && record.left(4) == QByteArrayLiteral("LOC1");
}

bool hasProcessedAudioMagic(const QByteArray &sideband)
{
    return sideband.size() == AudioLocationSidebandDecoder::RecordBytes
        && sideband.mid(3, 4) == QByteArrayLiteral("AUD1");
}

bool hasLoc3Magic(const QByteArray &sideband)
{
    return sideband.size() == AudioLocationSidebandDecoder::RecordBytes
        && sideband.mid(7, 4) == QByteArrayLiteral("LOC3");
}

QString validateLoc3Packet(const QByteArray &packet)
{
    if (packet.size() != AudioLocationSidebandDecoder::Loc3PacketBytes) {
        return QStringLiteral("invalid LOC3 packet length %1").arg(packet.size());
    }
    if (packet.left(4) != QByteArrayLiteral("LOC3")) {
        return QStringLiteral("LOC3 magic mismatch");
    }
    const quint8 version = byteAt(packet, 4);
    if (version < 1 || version > 4) {
        return QStringLiteral("unsupported LOC3 version %1").arg(byteAt(packet, 4));
    }
    if ((byteAt(packet, 5) & 0xf8u) != 0) {
        return QStringLiteral("LOC3 valid mask contains reserved bits");
    }
    for (int source = 0; source < 3; ++source) {
        const quint8 descriptor = byteAt(packet, 10 + source * 10);
        if ((descriptor & 0x03u) != static_cast<quint8>(source)) {
            return QStringLiteral("LOC3 source descriptor %1 has id %2")
                .arg(source)
                .arg(descriptor & 0x03u);
        }
        if ((descriptor & 0x7cu) != 0) {
            return QStringLiteral("LOC3 source descriptor %1 contains reserved bits")
                .arg(source);
        }
    }
    quint8 checksum = 0;
    for (int index = 0; index < 40; ++index) {
        checksum ^= byteAt(packet, index);
    }
    if (byteAt(packet, 40) != checksum) {
        return QStringLiteral("LOC3 checksum mismatch: received 0x%1, calculated 0x%2")
            .arg(byteAt(packet, 40), 2, 16, QLatin1Char('0'))
            .arg(checksum, 2, 16, QLatin1Char('0'));
    }
    return {};
}

QString validateRecord(const QByteArray &record)
{
    if (byteAt(record, 4) != 1) {
        return QStringLiteral("unsupported LOC1 version %1").arg(byteAt(record, 4));
    }
    if (byteAt(record, 5) != AudioLocationSidebandDecoder::RecordBytes) {
        return QStringLiteral("invalid LOC1 record length %1").arg(byteAt(record, 5));
    }
    if (byteAt(record, 7) != 1) {
        return QStringLiteral("unsupported LOC1 record type %1").arg(byteAt(record, 7));
    }

    quint8 checksum = 0;
    for (int index = 0; index < 44; ++index) {
        checksum ^= byteAt(record, index);
    }
    if (byteAt(record, 44) != checksum) {
        return QStringLiteral("LOC1 checksum mismatch: received 0x%1, calculated 0x%2")
            .arg(byteAt(record, 44), 2, 16, QLatin1Char('0'))
            .arg(checksum, 2, 16, QLatin1Char('0'));
    }
    if (byteAt(record, 45) != static_cast<quint8>(~checksum)) {
        return QStringLiteral("LOC1 checksum complement mismatch");
    }
    if (byteAt(record, 46) != 0x0a || byteAt(record, 47) != 0x0d) {
        return QStringLiteral("LOC1 record tail mismatch");
    }
    return {};
}

} // namespace

double PlLocationResult::azimuthDegrees() const
{
    return azimuthCentiDegrees / 100.0;
}

double PlLocationResult::elevationDegrees() const
{
    return elevationCentiDegrees / 100.0;
}

double PlLocationResult::confidence() const
{
    return std::max(0.0, std::min(confidenceQ15 / 32767.0, 1.0));
}

SoundSourceDirection PlLocationResult::physicalDirection() const
{
    SoundSourceDirection direction;
    if (recordType == 3 && version >= 4) {
        direction.azimuthDegrees = azimuthDegrees();
        direction.elevationDegrees = elevationDegrees();
    } else {
        const ScanDirection scan = unprojectVideoPoint(
            {(imageX + 0.5) / AudioLocationSidebandDecoder::ImageWidth,
             (imageY + 0.5) / AudioLocationSidebandDecoder::ImageHeight},
            CameraViewSettings {});
        direction.azimuthDegrees = scan.azimuthDegrees;
        direction.elevationDegrees = scan.elevationDegrees;
    }
    direction.confidence = confidence();
    direction.sourceId = sourceId;
    return direction;
}

AudioLocationSidebandResult AudioLocationSidebandDecoder::decode(
    const QByteArray &payload, quint32 timestampSeconds, quint32 timestampMicros)
{
    AudioLocationSidebandResult result;
    if (payload.size() != AudioFrameDecoder::FramePayloadBytes) {
        result.error = QStringLiteral(
                           "Expected 38400-byte 48-channel x 200-sample audio payload, got %1 bytes")
                           .arg(payload.size());
        return result;
    }

    result.pcmPayload.resize(payload.size());
    result.processedPcm16.reserve(AudioFrameDecoder::SamplesPerFrame * 2);
    QByteArray sideband(RecordBytes, char(0));

    for (int samplePoint = 0; samplePoint < AudioFrameDecoder::SamplesPerFrame;
         ++samplePoint) {
        const int sampleBase = samplePoint * AudioFrameDecoder::BytesPerSamplePoint;
        for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
            const int offset = sampleBase + channel * AudioFrameDecoder::BytesPerChannel;
            const quint32 word = qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar *>(payload.constData() + offset));
            const qint32 pcm24 = signExtendPcm24(word);
            qToLittleEndian(pcm24,
                            reinterpret_cast<uchar *>(result.pcmPayload.data() + offset));
            sideband[channel] = static_cast<char>(word >> 24);
        }

        if (hasProcessedAudioMagic(sideband)) {
            const quint32 raw24 = static_cast<quint32>(byteAt(sideband, 0))
                | (static_cast<quint32>(byteAt(sideband, 1)) << 8)
                | (static_cast<quint32>(byteAt(sideband, 2)) << 16);
            const qint32 processed24 = signExtendPcm24(raw24);
            const qint16 processed16 = static_cast<qint16>(
                std::clamp(processed24 / 256, -32768, 32767));
            char processedBytes[2];
            qToLittleEndian(processed16, processedBytes);
            result.processedPcm16.append(processedBytes, 2);
            ++result.processedSampleCount;
        }

        if (hasLoc3Magic(sideband)) {
            const QByteArray packet = sideband.mid(7, Loc3PacketBytes);
            const QString validationError = validateLoc3Packet(packet);
            if (!validationError.isEmpty()) {
                result.warnings.append(
                    QStringLiteral("Rejected LOC3 packet at audio sample point %1: %2")
                        .arg(samplePoint)
                        .arg(validationError));
            } else {
                const quint8 validMask = byteAt(packet, 5);
                for (int source = 0; source < 3; ++source) {
                    const int entry = 10 + source * 10;
                    const quint8 descriptor = byteAt(packet, entry);
                    PlLocationResult location;
                    location.sourceId = descriptor & 0x03u;
                    location.version = byteAt(packet, 4);
                    location.flags = validMask;
                    location.recordType = 3;
                    location.sequence = readU16(packet, 6);
                    location.locatorFrameCounter = readU16(packet, 8);
                    location.imageX = readU16(packet, entry + 1);
                    location.imageY = readU16(packet, entry + 3);
                    location.azimuthCentiDegrees = readI16(packet, entry + 5);
                    location.elevationCentiDegrees = readI16(packet, entry + 7);
                    const quint16 confidenceByte = byteAt(packet, entry + 9);
                    location.confidenceQ15 = static_cast<quint16>(
                        (confidenceByte << 7) | (confidenceByte >> 1));
                    location.timestampSeconds = timestampSeconds;
                    location.timestampMicros = timestampMicros;
                    location.samplePoint = samplePoint;
                    const bool descriptorValid = (descriptor & 0x80u) != 0;
                    const bool maskValid = (validMask & (1u << source)) != 0;
                    const bool coordinateValid = location.version >= 4
                        ? location.azimuthCentiDegrees >= -18000
                            && location.azimuthCentiDegrees <= 18000
                            && location.elevationCentiDegrees >= -9000
                            && location.elevationCentiDegrees <= 9000
                        : location.imageX < ImageWidth && location.imageY < ImageHeight;
                    location.locationValid = descriptorValid && maskValid
                        && coordinateValid && confidenceByte != 0;
                    if (descriptorValid != maskValid) {
                        result.warnings.append(
                            QStringLiteral("LOC3 sequence %1 source %2 valid bits disagree")
                                .arg(location.sequence)
                                .arg(source));
                    }
                    if (descriptorValid && maskValid && !coordinateValid) {
                        result.warnings.append(
                            QStringLiteral("LOC3 sequence %1 source %2 has out-of-range coordinate (%3, %4)")
                                .arg(location.sequence)
                                .arg(source)
                                .arg(location.imageX)
                                .arg(location.imageY));
                    }
                    result.locations.append(location);
                }
            }
        }

        if (!hasLocationMagic(sideband)) {
            continue;
        }
        const QString validationError = validateRecord(sideband);
        if (!validationError.isEmpty()) {
            result.warnings.append(
                QStringLiteral("Rejected LOC1 record at audio sample point %1: %2")
                    .arg(samplePoint)
                    .arg(validationError));
            continue;
        }

        PlLocationResult location;
        location.sourceId = 0;
        location.version = byteAt(sideband, 4);
        location.flags = byteAt(sideband, 6);
        location.recordType = byteAt(sideband, 7);
        location.sequence = readU32(sideband, 8);
        location.locatorFrameCounter = readU32(sideband, 12);
        location.imageX = readU16(sideband, 16);
        location.imageY = readU16(sideband, 18);
        location.azimuthCentiDegrees = readI16(sideband, 20);
        location.elevationCentiDegrees = readI16(sideband, 22);
        location.confidenceQ15 = readU16(sideband, 24);
        location.score = readI32(sideband, 26);
        location.timestampSeconds = timestampSeconds;
        location.timestampMicros = timestampMicros;
        location.samplePoint = samplePoint;
        location.locationValid = (location.flags & 0x01u) != 0
            && location.imageX < ImageWidth && location.imageY < ImageHeight
            && location.confidenceQ15 != 0;
        if ((location.flags & 0x01u) != 0
            && (location.imageX >= ImageWidth || location.imageY >= ImageHeight)) {
            result.warnings.append(
                QStringLiteral("LOC1 sequence %1 has out-of-range image coordinate (%2, %3)")
                    .arg(location.sequence)
                    .arg(location.imageX)
                    .arg(location.imageY));
        }
        result.locations.append(location);
    }

    result.valid = true;
    return result;
}

} // namespace MicArray
