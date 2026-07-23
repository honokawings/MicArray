#include "analysis/wavefilereader.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>

namespace MicArray {
namespace {

constexpr quint16 WaveFormatPcm = 0x0001;
constexpr quint16 WaveFormatExtensible = 0xfffe;
const QByteArray PcmSubFormat = QByteArray::fromHex("0100000000001000800000aa00389b71");

quint16 readLe16(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

quint32 readLe32(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

} // namespace

bool WaveFileReader::open(const QString &path, QString *errorMessage)
{
    QFile candidate(path);
    if (!candidate.open(QIODevice::ReadOnly)) {
        setError(errorMessage,
                 QStringLiteral("无法打开 WAV 文件 %1：%2").arg(path, candidate.errorString()));
        return false;
    }

    const QByteArray header = candidate.read(12);
    if (header.size() != 12
        || header.first(4) != QByteArrayLiteral("RIFF")
        || header.mid(8, 4) != QByteArrayLiteral("WAVE")) {
        setError(errorMessage, QStringLiteral("文件不是有效的 RIFF/WAVE"));
        return false;
    }

    const qint64 riffEnd = static_cast<qint64>(readLe32(header, 4)) + 8;
    if (riffEnd > candidate.size()) {
        setError(errorMessage, QStringLiteral("WAV 文件已截断：RIFF 长度超过实际文件"));
        return false;
    }

    bool foundFormat = false;
    bool foundData = false;
    quint16 formatTag = 0;
    int channelCount = 0;
    int sampleRate = 0;
    int byteRate = 0;
    int blockAlign = 0;
    int containerBits = 0;
    int validBits = 0;
    qint64 dataOffset = 0;
    qint64 dataLength = 0;

    qint64 position = 12;
    while (position + 8 <= riffEnd) {
        if (!candidate.seek(position)) {
            setError(errorMessage, QStringLiteral("无法定位 WAV chunk"));
            return false;
        }
        const QByteArray chunkHeader = candidate.read(8);
        if (chunkHeader.size() != 8) {
            setError(errorMessage, QStringLiteral("WAV chunk 头已截断"));
            return false;
        }
        const QByteArray chunkId = chunkHeader.first(4);
        const qint64 chunkLength = readLe32(chunkHeader, 4);
        const qint64 chunkDataOffset = position + 8;
        const qint64 chunkEnd = chunkDataOffset + chunkLength;
        const qint64 paddedEnd = chunkEnd + (chunkLength & 1);
        if (chunkEnd < chunkDataOffset || chunkEnd > riffEnd || paddedEnd > candidate.size()) {
            setError(errorMessage, QStringLiteral("WAV chunk 数据已截断"));
            return false;
        }

        if (chunkId == QByteArrayLiteral("fmt ")) {
            if (chunkLength < 16 || chunkLength > 1024) {
                setError(errorMessage, QStringLiteral("WAV fmt chunk 长度无效"));
                return false;
            }
            if (!candidate.seek(chunkDataOffset)) {
                setError(errorMessage, QStringLiteral("无法读取 WAV fmt chunk"));
                return false;
            }
            const QByteArray format = candidate.read(chunkLength);
            if (format.size() != chunkLength) {
                setError(errorMessage, QStringLiteral("WAV fmt chunk 已截断"));
                return false;
            }
            formatTag = readLe16(format, 0);
            channelCount = readLe16(format, 2);
            sampleRate = static_cast<int>(readLe32(format, 4));
            byteRate = static_cast<int>(readLe32(format, 8));
            blockAlign = readLe16(format, 12);
            containerBits = readLe16(format, 14);
            validBits = containerBits;
            if (formatTag == WaveFormatExtensible) {
                if (format.size() < 40 || readLe16(format, 16) < 22) {
                    setError(errorMessage, QStringLiteral("WAVE_FORMAT_EXTENSIBLE fmt chunk 不完整"));
                    return false;
                }
                validBits = readLe16(format, 18);
                if (format.mid(24, 16) != PcmSubFormat) {
                    setError(errorMessage, QStringLiteral("仅支持 PCM WAVE_FORMAT_EXTENSIBLE"));
                    return false;
                }
            } else if (formatTag != WaveFormatPcm) {
                setError(errorMessage, QStringLiteral("仅支持整数 PCM WAV"));
                return false;
            }
            foundFormat = true;
        } else if (chunkId == QByteArrayLiteral("data") && !foundData) {
            dataOffset = chunkDataOffset;
            dataLength = chunkLength;
            foundData = true;
        }

        position = paddedEnd;
    }

    if (!foundFormat || !foundData) {
        setError(errorMessage, QStringLiteral("WAV 缺少 fmt 或 data chunk"));
        return false;
    }
    if (channelCount < 1 || channelCount > 48) {
        setError(errorMessage, QStringLiteral("WAV 通道数必须为 1 到 48"));
        return false;
    }
    if (sampleRate < 8000 || sampleRate > 192000) {
        setError(errorMessage, QStringLiteral("WAV 采样率超出 8000 到 192000 Hz"));
        return false;
    }
    if ((containerBits != 16 && containerBits != 24 && containerBits != 32)
        || validBits < 1 || validBits > containerBits) {
        setError(errorMessage, QStringLiteral("WAV PCM 位深必须为 16、24 或 32 bit"));
        return false;
    }
    const int expectedBlockAlign = channelCount * containerBits / 8;
    if (blockAlign != expectedBlockAlign) {
        setError(errorMessage,
                 QStringLiteral("WAV blockAlign 无效：期望 %1，实际 %2")
                     .arg(expectedBlockAlign)
                     .arg(blockAlign));
        return false;
    }
    if (byteRate != sampleRate * blockAlign) {
        setError(errorMessage, QStringLiteral("WAV byteRate 与采样率和 blockAlign 不一致"));
        return false;
    }
    if (dataLength % blockAlign != 0) {
        setError(errorMessage, QStringLiteral("WAV data 末尾不足一个完整采样帧"));
        return false;
    }

    close();
    candidate.close();
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadOnly)) {
        setError(errorMessage,
                 QStringLiteral("无法重新打开 WAV 文件 %1：%2").arg(path, m_file.errorString()));
        return false;
    }
    m_info.path = m_file.fileName();
    m_info.channelCount = channelCount;
    m_info.sampleRate = sampleRate;
    m_info.containerBits = containerBits;
    m_info.validBits = validBits;
    m_info.blockAlign = blockAlign;
    m_info.totalSamples = dataLength / blockAlign;
    m_info.durationSeconds = static_cast<double>(m_info.totalSamples)
        / static_cast<double>(sampleRate);
    m_dataOffset = dataOffset;
    m_dataLength = dataLength;
    setError(errorMessage, {});
    return true;
}

void WaveFileReader::close()
{
    m_file.close();
    m_file.setFileName({});
    m_info = {};
    m_dataOffset = 0;
    m_dataLength = 0;
}

QVector<QVector<float>> WaveFileReader::readChannels(qint64 startSample,
                                                     int sampleCount,
                                                     QString *errorMessage)
{
    if (!isOpen()) {
        setError(errorMessage, QStringLiteral("尚未打开 WAV 文件"));
        return {};
    }
    if (sampleCount < 0) {
        setError(errorMessage, QStringLiteral("读取样本数不能为负数"));
        return {};
    }

    QVector<QVector<float>> channels(m_info.channelCount);
    for (QVector<float> &channel : channels) {
        channel.fill(0.0f, sampleCount);
    }
    QByteArray bytes;
    int destinationStart = 0;
    int framesRead = 0;
    if (!readFrameBytes(startSample,
                        sampleCount,
                        &bytes,
                        &destinationStart,
                        &framesRead,
                        errorMessage)) {
        return {};
    }

    const int containerBytes = m_info.containerBits / 8;
    for (int frame = 0; frame < framesRead; ++frame) {
        const char *frameBytes = bytes.constData() + frame * m_info.blockAlign;
        for (int channel = 0; channel < m_info.channelCount; ++channel) {
            const qint64 sample = decodeIntegerSample(frameBytes + channel * containerBytes);
            channels[channel][destinationStart + frame] = normalizeSample(sample);
        }
    }
    setError(errorMessage, {});
    return channels;
}

QByteArray WaveFileReader::readMonoPcm16(int channel,
                                         qint64 startSample,
                                         int sampleCount,
                                         QString *errorMessage)
{
    if (!isOpen()) {
        setError(errorMessage, QStringLiteral("尚未打开 WAV 文件"));
        return {};
    }
    if (channel < 0 || channel >= m_info.channelCount) {
        setError(errorMessage, QStringLiteral("WAV 播放通道超出有效范围"));
        return {};
    }
    if (sampleCount < 0) {
        setError(errorMessage, QStringLiteral("读取样本数不能为负数"));
        return {};
    }

    QByteArray result(sampleCount * 2, char(0));
    QByteArray bytes;
    int destinationStart = 0;
    int framesRead = 0;
    if (!readFrameBytes(startSample,
                        sampleCount,
                        &bytes,
                        &destinationStart,
                        &framesRead,
                        errorMessage)) {
        return {};
    }

    const int containerBytes = m_info.containerBits / 8;
    for (int frame = 0; frame < framesRead; ++frame) {
        const char *sampleBytes = bytes.constData()
            + frame * m_info.blockAlign + channel * containerBytes;
        const double normalized = normalizeSample(decodeIntegerSample(sampleBytes));
        const int scaled = static_cast<int>(std::lround(normalized * 32767.0));
        const qint16 pcm = static_cast<qint16>(std::clamp(scaled, -32768, 32767));
        qToLittleEndian(pcm, result.data() + (destinationStart + frame) * 2);
    }
    setError(errorMessage, {});
    return result;
}

bool WaveFileReader::readFrameBytes(qint64 startSample,
                                    int sampleCount,
                                    QByteArray *bytes,
                                    int *destinationStart,
                                    int *framesRead,
                                    QString *errorMessage)
{
    *destinationStart = 0;
    *framesRead = 0;
    bytes->clear();
    if (sampleCount == 0) {
        return true;
    }

    const qint64 requestedEnd = startSample > std::numeric_limits<qint64>::max() - sampleCount
        ? std::numeric_limits<qint64>::max()
        : startSample + sampleCount;
    const qint64 validStart = std::max<qint64>(0, startSample);
    const qint64 validEnd = std::min(m_info.totalSamples, requestedEnd);
    if (validStart >= validEnd) {
        return true;
    }

    const qint64 destination = validStart - startSample;
    const qint64 frameCount = validEnd - validStart;
    if (destination > std::numeric_limits<int>::max()
        || frameCount > std::numeric_limits<int>::max()) {
        setError(errorMessage, QStringLiteral("WAV 读取范围过大"));
        return false;
    }
    const qint64 byteCount = frameCount * m_info.blockAlign;
    if (!m_file.seek(m_dataOffset + validStart * m_info.blockAlign)) {
        setError(errorMessage, QStringLiteral("无法定位 WAV 样本：%1").arg(m_file.errorString()));
        return false;
    }
    *bytes = m_file.read(byteCount);
    if (bytes->size() != byteCount) {
        setError(errorMessage,
                 QStringLiteral("WAV 样本读取已截断：期望 %1 字节，实际 %2 字节")
                     .arg(byteCount)
                     .arg(bytes->size()));
        return false;
    }
    *destinationStart = static_cast<int>(destination);
    *framesRead = static_cast<int>(frameCount);
    return true;
}

qint64 WaveFileReader::decodeIntegerSample(const char *bytes) const
{
    switch (m_info.containerBits) {
    case 16:
        return qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(bytes));
    case 24: {
        qint32 value = static_cast<quint8>(bytes[0])
            | (static_cast<qint32>(static_cast<quint8>(bytes[1])) << 8)
            | (static_cast<qint32>(static_cast<quint8>(bytes[2])) << 16);
        if ((value & 0x00800000) != 0) {
            value |= static_cast<qint32>(0xff000000);
        }
        return value;
    }
    case 32:
        return qFromLittleEndian<qint32>(reinterpret_cast<const uchar *>(bytes));
    default:
        return 0;
    }
}

float WaveFileReader::normalizeSample(qint64 sample) const
{
    const double scale = std::ldexp(1.0, m_info.validBits - 1);
    return static_cast<float>(std::clamp(static_cast<double>(sample) / scale, -1.0, 1.0));
}

void WaveFileReader::setError(QString *errorMessage, const QString &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

} // namespace MicArray
