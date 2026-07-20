#include "media/multichannelwavwriter.h"

#include "media/audioframedecoder.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace MicArray {
namespace {

void appendU16(QByteArray &buffer, quint16 value)
{
    char bytes[2];
    qToLittleEndian(value, bytes);
    buffer.append(bytes, 2);
}

void appendU32(QByteArray &buffer, quint32 value)
{
    char bytes[4];
    qToLittleEndian(value, bytes);
    buffer.append(bytes, 4);
}

} // namespace

MultiChannelWavWriter::MultiChannelWavWriter(quint64 segmentDataLimit,
                                             FileFactory fileFactory,
                                             FileRemover fileRemover)
    : m_segmentDataLimit(std::min(segmentDataLimit, DefaultSegmentDataLimit))
    , m_fileFactory(std::move(fileFactory))
    , m_fileRemover(std::move(fileRemover))
{
    if (!m_fileFactory) {
        m_fileFactory = [](const QString &path) {
            return std::make_unique<QFile>(path);
        };
    }
    if (!m_fileRemover) {
        m_fileRemover = [](const QString &path) {
            return QFile::remove(path);
        };
    }
}

MultiChannelWavWriter::~MultiChannelWavWriter()
{
    if (m_open) {
        close(QStringLiteral("writer-destroyed"));
    }
}

RecordingSummary MultiChannelWavWriter::summary() const
{
    RecordingSummary value = m_summary;
    if (m_file != nullptr && m_file->isOpen()) {
        value.fileBytes += static_cast<quint64>(std::max<qint64>(0, m_file->size()));
    }
    return value;
}

bool MultiChannelWavWriter::open(const RecordingSettings &settingsValue, QString *errorMessage)
{
    if (m_open) {
        setError(errorMessage, QStringLiteral("A recording file is already open"));
        return false;
    }

    RecordingSettings settings = settingsValue;
    if (!validateSettings(&settings, errorMessage)) {
        return false;
    }

    m_settings = settings;
    m_summary = {};
    m_segmentDataSizes.clear();
    m_invalidSegmentPaths.clear();
    m_segmentIndex = 1;
    m_segmentDataBytes = 0;
    m_segmentInputFrames = 0;
    m_failed = false;
    m_manifestReserved = false;
    m_failureMessage.clear();
    m_startedAt = QDateTime::currentDateTimeUtc();
    m_summary.manifestPath = QDir(m_settings.outputDirectory).filePath(m_settings.baseName + QStringLiteral(".json"));
    if (QFileInfo::exists(m_summary.manifestPath)) {
        setError(errorMessage, QStringLiteral("Recording manifest already exists: %1").arg(m_summary.manifestPath));
        return false;
    }
    if (!reserveManifest(errorMessage)) {
        return false;
    }
    m_open = true;
    if (!openSegment(errorMessage)) {
        m_summary.stopReason = QStringLiteral("start-error");
        m_open = false;
        if (m_failed && !m_summary.segmentPaths.isEmpty()) {
            QString manifestError;
            if (!writeManifest(&manifestError) && errorMessage != nullptr) {
                *errorMessage += QStringLiteral("; %1").arg(manifestError);
            }
        } else if (m_manifestReserved) {
            if (!m_fileRemover(m_summary.manifestPath) && errorMessage != nullptr) {
                *errorMessage += QStringLiteral("; manifest reservation could not be removed: %1")
                                     .arg(m_summary.manifestPath);
            }
            m_manifestReserved = false;
        }
        return false;
    }
    return true;
}

bool MultiChannelWavWriter::appendFrame(const QByteArray &payload, QString *errorMessage)
{
    if (!m_open || m_failed || m_file == nullptr || !m_file->isOpen()) {
        setError(errorMessage, QStringLiteral("No recording file is open"));
        return false;
    }
    if (payload.size() != AudioFrameDecoder::FramePayloadBytes) {
        setError(errorMessage,
                 QStringLiteral("Expected 38400-byte audio payload, got %1 bytes").arg(payload.size()));
        return false;
    }

    const quint64 outputFrameBytes = static_cast<quint64>(AudioFrameDecoder::SamplesPerFrame)
        * static_cast<quint64>(m_settings.channels.size()) * AudioFrameDecoder::BytesPerChannel;
    if (m_segmentDataBytes > 0 && m_segmentDataBytes + outputFrameBytes > m_segmentDataLimit) {
        if (!finalizeSegment(errorMessage)) {
            latchFailure(errorMessage != nullptr ? *errorMessage : QStringLiteral("Cannot finalize WAV segment"), true);
            return false;
        }
        ++m_segmentIndex;
        if (!openSegment(errorMessage)) {
            latchFailure(errorMessage != nullptr ? *errorMessage : QStringLiteral("Cannot open WAV segment"), false);
            return false;
        }
    }

    const QByteArray selected = selectChannels(payload);
    const qint64 written = m_file->write(selected);
    if (written != selected.size()) {
        const QString message = QStringLiteral("Short WAV write to %1: expected %2 bytes, wrote %3")
                                    .arg(m_file->fileName())
                                    .arg(selected.size())
                                    .arg(written);
        latchFailure(message, true);
        setError(errorMessage, message);
        return false;
    }

    m_segmentDataBytes += static_cast<quint64>(selected.size());
    ++m_summary.inputFrames;
    ++m_segmentInputFrames;
    m_summary.sampleFrames += AudioFrameDecoder::SamplesPerFrame;
    m_summary.dataBytes += static_cast<quint64>(selected.size());
    return true;
}

bool MultiChannelWavWriter::close(const QString &stopReason, QString *errorMessage)
{
    if (!m_open) {
        setError(errorMessage, QStringLiteral("No recording file is open"));
        return false;
    }

    QString finalizeError;
    if (!m_failed && !finalizeSegment(&finalizeError)) {
        latchFailure(finalizeError, true);
    }
    m_summary.stopReason = stopReason;
    m_open = false;

    QString manifestError;
    const bool manifestWritten = writeManifest(&manifestError);
    if (!manifestWritten) {
        setError(errorMessage, manifestError);
        return false;
    }
    if (m_failed) {
        setError(errorMessage, m_failureMessage);
        return false;
    }
    return true;
}

bool MultiChannelWavWriter::validateSettings(RecordingSettings *settings, QString *errorMessage) const
{
    settings->outputDirectory = settings->outputDirectory.trimmed();
    settings->baseName = settings->baseName.trimmed();
    std::sort(settings->channels.begin(), settings->channels.end());

    if (settings->outputDirectory.isEmpty()) {
        setError(errorMessage, QStringLiteral("Recording output directory is empty"));
        return false;
    }
    settings->outputDirectory = QDir::cleanPath(settings->outputDirectory);
    if (!QDir(settings->outputDirectory).exists()) {
        setError(errorMessage, QStringLiteral("Recording output directory does not exist: %1").arg(settings->outputDirectory));
        return false;
    }
    const QFileInfo directoryInfo(settings->outputDirectory);
    if (!directoryInfo.isDir() || !directoryInfo.isWritable()) {
        setError(errorMessage, QStringLiteral("Recording output directory is not writable: %1").arg(settings->outputDirectory));
        return false;
    }
    if (settings->baseName.isEmpty() || QFileInfo(settings->baseName).fileName() != settings->baseName) {
        setError(errorMessage, QStringLiteral("Recording base name is empty or contains a path"));
        return false;
    }
    if (settings->sampleRate < 8000 || settings->sampleRate > 192000) {
        setError(errorMessage, QStringLiteral("Recording sample rate is outside 8000-192000 Hz"));
        return false;
    }
    if (settings->channels.isEmpty()) {
        setError(errorMessage, QStringLiteral("At least one recording channel must be selected"));
        return false;
    }
    for (int index = 0; index < settings->channels.size(); ++index) {
        const int channel = settings->channels.at(index);
        if (channel < 0 || channel >= AudioFrameDecoder::ChannelCount) {
            setError(errorMessage, QStringLiteral("Recording channel %1 is outside 0-47").arg(channel));
            return false;
        }
        if (index > 0 && channel == settings->channels.at(index - 1)) {
            setError(errorMessage, QStringLiteral("Recording channel %1 is selected more than once").arg(channel));
            return false;
        }
    }

    const quint64 outputFrameBytes = static_cast<quint64>(AudioFrameDecoder::SamplesPerFrame)
        * static_cast<quint64>(settings->channels.size()) * AudioFrameDecoder::BytesPerChannel;
    if (m_segmentDataLimit < outputFrameBytes) {
        setError(errorMessage,
                 QStringLiteral("WAV segment limit %1 is smaller than one output frame (%2 bytes)")
                     .arg(m_segmentDataLimit)
                     .arg(outputFrameBytes));
        return false;
    }
    return true;
}

bool MultiChannelWavWriter::openSegment(QString *errorMessage)
{
    const QString path = segmentPath(m_segmentIndex);
    m_file = m_fileFactory(path);
    if (m_file == nullptr) {
        setError(errorMessage, QStringLiteral("Cannot create a WAV file device for %1").arg(path));
        return false;
    }
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        setError(errorMessage, QStringLiteral("Cannot create WAV file %1: %2").arg(path, m_file->errorString()));
        return false;
    }

    m_segmentDataBytes = 0;
    m_segmentInputFrames = 0;
    m_summary.segmentPaths.append(path);

    const QByteArray header = createHeader(0);
    if (m_file->write(header) != header.size()) {
        const QString message = QStringLiteral("Cannot write WAV header to %1: %2").arg(path, m_file->errorString());
        latchFailure(message, true);
        setError(errorMessage, m_failureMessage);
        return false;
    }
    return true;
}

bool MultiChannelWavWriter::finalizeSegment(QString *errorMessage)
{
    if (m_file == nullptr || !m_file->isOpen()) {
        return true;
    }
    if (m_segmentDataBytes > DefaultSegmentDataLimit) {
        setError(errorMessage, QStringLiteral("WAV data exceeds the RIFF 32-bit size limit"));
        m_file->close();
        return false;
    }
    if (!m_file->seek(0)) {
        setError(errorMessage, QStringLiteral("Cannot seek WAV header in %1").arg(m_file->fileName()));
        m_file->close();
        return false;
    }

    const QByteArray header = createHeader(static_cast<quint32>(m_segmentDataBytes));
    if (m_file->write(header) != header.size()) {
        setError(errorMessage, QStringLiteral("Cannot finalize WAV header in %1: %2").arg(m_file->fileName(), m_file->errorString()));
        m_file->close();
        return false;
    }
    if (!m_file->flush()) {
        setError(errorMessage, QStringLiteral("Cannot flush WAV file %1: %2").arg(m_file->fileName(), m_file->errorString()));
        m_file->close();
        return false;
    }

    const QString path = m_file->fileName();
    m_file->close();
    m_segmentDataSizes.append(m_segmentDataBytes);
    m_summary.fileBytes += static_cast<quint64>(QFileInfo(path).size());
    m_segmentDataBytes = 0;
    m_segmentInputFrames = 0;
    return true;
}

bool MultiChannelWavWriter::reserveManifest(QString *errorMessage)
{
    QFile reservation(m_summary.manifestPath);
    if (!reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        setError(errorMessage,
                 QStringLiteral("Recording manifest already exists or cannot be reserved: %1")
                     .arg(m_summary.manifestPath));
        return false;
    }
    const QByteArray marker("{\"state\":\"recording\"}\n");
    if (reservation.write(marker) != marker.size() || !reservation.flush()) {
        QString message = QStringLiteral("Cannot write recording manifest reservation %1: %2")
                              .arg(m_summary.manifestPath, reservation.errorString());
        reservation.close();
        if (!m_fileRemover(m_summary.manifestPath)) {
            message += QStringLiteral("; failed manifest reservation could not be removed");
        }
        setError(errorMessage, message);
        return false;
    }
    reservation.close();
    m_manifestReserved = true;
    return true;
}

bool MultiChannelWavWriter::writeManifest(QString *errorMessage)
{
    QJsonArray channels;
    for (int channel : m_settings.channels) {
        channels.append(channel);
    }

    QJsonArray segments;
    for (int index = 0; index < m_summary.segmentPaths.size(); ++index) {
        const quint64 dataBytes = index < m_segmentDataSizes.size() ? m_segmentDataSizes.at(index) : 0;
        segments.append(QJsonObject {
            {QStringLiteral("file"), QFileInfo(m_summary.segmentPaths.at(index)).fileName()},
            {QStringLiteral("dataBytes"), static_cast<double>(dataBytes)},
            {QStringLiteral("fileBytes"), static_cast<double>(QFileInfo(m_summary.segmentPaths.at(index)).size())},
            {QStringLiteral("valid"), !m_invalidSegmentPaths.contains(m_summary.segmentPaths.at(index))},
        });
    }

    const QJsonObject root {
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("baseName"), m_settings.baseName},
        {QStringLiteral("channels"), channels},
        {QStringLiteral("sampleRate"), m_settings.sampleRate},
        {QStringLiteral("containerBits"), 32},
        {QStringLiteral("validBits"), 24},
        {QStringLiteral("startedAt"), m_startedAt.toString(Qt::ISODateWithMs)},
        {QStringLiteral("endedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("inputFrames"), static_cast<double>(m_summary.inputFrames)},
        {QStringLiteral("sampleFrames"), static_cast<double>(m_summary.sampleFrames)},
        {QStringLiteral("dataBytes"), static_cast<double>(m_summary.dataBytes)},
        {QStringLiteral("stopReason"), m_summary.stopReason},
        {QStringLiteral("segments"), segments},
    };

    if (!m_manifestReserved) {
        setError(errorMessage, QStringLiteral("Recording manifest is not reserved: %1").arg(m_summary.manifestPath));
        return false;
    }
    QSaveFile manifest(m_summary.manifestPath);
    if (!manifest.open(QIODevice::WriteOnly)) {
        setError(errorMessage,
                 QStringLiteral("Cannot create recording manifest %1: %2")
                     .arg(m_summary.manifestPath, manifest.errorString()));
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (manifest.write(json) != json.size() || !manifest.commit()) {
        setError(errorMessage,
                 QStringLiteral("Cannot write recording manifest %1: %2")
                     .arg(m_summary.manifestPath, manifest.errorString()));
        return false;
    }
    m_manifestReserved = false;
    return true;
}

QByteArray MultiChannelWavWriter::createHeader(quint32 dataSize) const
{
    const quint16 channelCount = static_cast<quint16>(m_settings.channels.size());
    const quint16 blockAlign = static_cast<quint16>(channelCount * AudioFrameDecoder::BytesPerChannel);
    const quint32 byteRate = static_cast<quint32>(m_settings.sampleRate) * blockAlign;

    QByteArray header;
    header.reserve(WavHeaderSize);
    header.append("RIFF", 4);
    appendU32(header, 60U + dataSize);
    header.append("WAVE", 4);
    header.append("fmt ", 4);
    appendU32(header, 40);
    appendU16(header, 0xfffe);
    appendU16(header, channelCount);
    appendU32(header, static_cast<quint32>(m_settings.sampleRate));
    appendU32(header, byteRate);
    appendU16(header, blockAlign);
    appendU16(header, 32);
    appendU16(header, 22);
    appendU16(header, 24);
    appendU32(header, 0);
    header.append(QByteArray::fromHex("0100000000001000800000aa00389b71"));
    header.append("data", 4);
    appendU32(header, dataSize);
    return header;
}

QByteArray MultiChannelWavWriter::selectChannels(const QByteArray &payload) const
{
    QByteArray selected;
    selected.resize(AudioFrameDecoder::SamplesPerFrame * m_settings.channels.size() * AudioFrameDecoder::BytesPerChannel);
    char *destination = selected.data();
    for (int samplePoint = 0; samplePoint < AudioFrameDecoder::SamplesPerFrame; ++samplePoint) {
        const int sampleBase = samplePoint * AudioFrameDecoder::BytesPerSamplePoint;
        for (int channel : m_settings.channels) {
            const int sourceOffset = sampleBase + channel * AudioFrameDecoder::BytesPerChannel;
            std::memcpy(destination,
                        payload.constData() + sourceOffset,
                        AudioFrameDecoder::BytesPerChannel);
            destination += AudioFrameDecoder::BytesPerChannel;
        }
    }
    return selected;
}

QString MultiChannelWavWriter::segmentPath(int segmentIndex) const
{
    const QString suffix = segmentIndex == 1
        ? QStringLiteral(".wav")
        : QStringLiteral("_part%1.wav").arg(segmentIndex, 3, 10, QLatin1Char('0'));
    return QDir(m_settings.outputDirectory).filePath(m_settings.baseName + suffix);
}

void MultiChannelWavWriter::latchFailure(const QString &message, bool discardSegment)
{
    if (!m_failed) {
        m_failed = true;
        m_failureMessage = message;
    }
    if (discardSegment) {
        discardCurrentSegment();
    }
}

void MultiChannelWavWriter::discardCurrentSegment()
{
    QString path;
    if (m_file != nullptr) {
        path = m_file->fileName();
        if (m_file->isOpen()) {
            m_file->close();
        }
    }

    if (m_summary.inputFrames >= m_segmentInputFrames) {
        m_summary.inputFrames -= m_segmentInputFrames;
    }
    const quint64 segmentSampleFrames = m_segmentInputFrames * AudioFrameDecoder::SamplesPerFrame;
    if (m_summary.sampleFrames >= segmentSampleFrames) {
        m_summary.sampleFrames -= segmentSampleFrames;
    }
    if (m_summary.dataBytes >= m_segmentDataBytes) {
        m_summary.dataBytes -= m_segmentDataBytes;
    }

    if (!path.isEmpty()) {
        const bool removed = !QFileInfo::exists(path) || m_fileRemover(path);
        if (removed) {
            if (!m_summary.segmentPaths.isEmpty() && m_summary.segmentPaths.last() == path) {
                m_summary.segmentPaths.removeLast();
            }
        } else {
            m_invalidSegmentPaths.insert(path);
            m_segmentDataSizes.append(m_segmentDataBytes);
            m_summary.fileBytes += static_cast<quint64>(std::max<qint64>(0, QFileInfo(path).size()));
            m_failureMessage += QStringLiteral("; damaged segment could not be removed: %1").arg(path);
        }
    }
    m_segmentDataBytes = 0;
    m_segmentInputFrames = 0;
}

void MultiChannelWavWriter::setError(QString *errorMessage, const QString &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

} // namespace MicArray
