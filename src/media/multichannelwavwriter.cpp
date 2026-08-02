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

QJsonObject gainConfigurationToJson(const GainConfiguration &configuration)
{
    QJsonArray shifts;
    QJsonArray automatic;
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        shifts.append(configuration.shifts.at(channel));
        automatic.append(configuration.automatic.at(channel));
    }
    return QJsonObject {
        {QStringLiteral("shifts"), shifts},
        {QStringLiteral("automatic"), automatic},
        {QStringLiteral("upperPeakRatio"), configuration.upperPeakRatio},
        {QStringLiteral("lowerPeakRatio"), configuration.lowerPeakRatio},
        {QStringLiteral("releaseHoldMs"), configuration.releaseHoldMs},
        {QStringLiteral("releaseStepMs"), configuration.releaseStepMs},
    };
}

QString recordingInputSourceKey(RecordingInputSource source)
{
    switch (source) {
    case RecordingInputSource::Array:
        return QStringLiteral("array");
    case RecordingInputSource::NoiseReductionStages:
        return QStringLiteral("noiseReductionStages");
    }
    return QStringLiteral("unknown");
}

QString recordingFileLayoutKey(RecordingFileLayout layout)
{
    switch (layout) {
    case RecordingFileLayout::SingleMultichannel:
        return QStringLiteral("singleMultichannel");
    case RecordingFileLayout::SeparateTracks:
        return QStringLiteral("separateTracks");
    }
    return QStringLiteral("unknown");
}

bool isKnownNoiseReductionStage(NoiseReductionDataFlowStage stage)
{
    switch (stage) {
    case NoiseReductionDataFlowStage::Suppression:
    case NoiseReductionDataFlowStage::BandPass:
    case NoiseReductionDataFlowStage::LogMmse:
    case NoiseReductionDataFlowStage::AutomaticGainControl:
        return true;
    }
    return false;
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
    value.audioFileCount = static_cast<quint64>(value.segmentPaths.size());
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
    m_finalGainConfiguration = settings.initialGainConfiguration;
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

bool MultiChannelWavWriter::updateFinalGainConfiguration(const GainConfiguration &configuration,
                                                         QString *errorMessage)
{
    if (!m_open || m_failed) {
        setError(errorMessage, QStringLiteral("No recording file is open"));
        return false;
    }
    if (!m_settings.gainApplied) {
        setError(errorMessage, QStringLiteral("Gain metadata is not enabled for this recording"));
        return false;
    }
    QString validationError;
    if (!GainConfiguration::validate(configuration, &validationError)) {
        setError(errorMessage, QStringLiteral("Invalid final gain configuration: %1").arg(validationError));
        return false;
    }
    m_finalGainConfiguration = configuration;
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
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

    const bool explicitTrackKeys = !settings->trackKeys.isEmpty();
    const bool explicitTrackNames = !settings->trackNames.isEmpty();
    if (explicitTrackKeys && settings->trackKeys.size() != settings->channels.size()) {
        setError(errorMessage, QStringLiteral("Recording track key count must match channel count"));
        return false;
    }
    if (explicitTrackNames && settings->trackNames.size() != settings->channels.size()) {
        setError(errorMessage, QStringLiteral("Recording track name count must match channel count"));
        return false;
    }

    struct TrackBinding {
        int channel = 0;
        NoiseReductionDataFlowStage stage = NoiseReductionDataFlowStage::Suppression;
        QString key;
        QString name;
    };
    QVector<TrackBinding> bindings;
    bindings.reserve(settings->channels.size());
    if (settings->inputSource == RecordingInputSource::Array) {
        if (!settings->noiseReductionStages.isEmpty()) {
            setError(errorMessage, QStringLiteral("Array recording cannot select noise-reduction stages"));
            return false;
        }
        for (int index = 0; index < settings->channels.size(); ++index) {
            const int channel = settings->channels.at(index);
            bindings.append({
                channel,
                NoiseReductionDataFlowStage::Suppression,
                explicitTrackKeys
                    ? settings->trackKeys.at(index).trimmed()
                    : QStringLiteral("ch%1").arg(channel, 2, 10, QLatin1Char('0')),
                explicitTrackNames
                    ? settings->trackNames.at(index).trimmed()
                    : QStringLiteral("CH%1").arg(channel),
            });
        }
        std::sort(bindings.begin(), bindings.end(), [](const TrackBinding &left,
                                                       const TrackBinding &right) {
            return left.channel < right.channel;
        });
    } else {
        if (settings->noiseReductionStages.size() != settings->channels.size()) {
            setError(errorMessage,
                     QStringLiteral("Noise-reduction stage count must match internal channel count"));
            return false;
        }
        QVector<NoiseReductionDataFlowStage> seenStages;
        for (int index = 0; index < settings->channels.size(); ++index) {
            const NoiseReductionDataFlowStage stage = settings->noiseReductionStages.at(index);
            if (!isKnownNoiseReductionStage(stage) || seenStages.contains(stage)) {
                setError(errorMessage, QStringLiteral("Noise-reduction recording stages must be known and unique"));
                return false;
            }
            if (index > 0 && settings->channels.at(index) <= settings->channels.at(index - 1)) {
                setError(errorMessage,
                         QStringLiteral("Noise-reduction internal channels must be strictly increasing"));
                return false;
            }
            seenStages.append(stage);
            bindings.append({
                settings->channels.at(index),
                stage,
                explicitTrackKeys
                    ? settings->trackKeys.at(index).trimmed()
                    : noiseReductionDataFlowStageKey(stage),
                explicitTrackNames
                    ? settings->trackNames.at(index).trimmed()
                    : noiseReductionDataFlowStageName(stage),
            });
        }
    }

    settings->channels.clear();
    settings->trackKeys.clear();
    settings->trackNames.clear();
    if (settings->inputSource == RecordingInputSource::NoiseReductionStages) {
        settings->noiseReductionStages.clear();
    }
    QSet<QString> seenTrackKeys;
    for (const TrackBinding &binding : bindings) {
        if (binding.key.isEmpty() || QFileInfo(binding.key).fileName() != binding.key
            || binding.name.isEmpty() || seenTrackKeys.contains(binding.key)) {
            setError(errorMessage,
                     QStringLiteral("Recording track keys must be unique file-name components and names must not be empty"));
            return false;
        }
        seenTrackKeys.insert(binding.key);
        settings->channels.append(binding.channel);
        settings->trackKeys.append(binding.key);
        settings->trackNames.append(binding.name);
        if (settings->inputSource == RecordingInputSource::NoiseReductionStages) {
            settings->noiseReductionStages.append(binding.stage);
        }
    }

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
    if (settings->gainApplied) {
        QString validationError;
        if (!GainConfiguration::validate(settings->initialGainConfiguration, &validationError)) {
            setError(errorMessage,
                     QStringLiteral("Invalid initial gain configuration: %1").arg(validationError));
            return false;
        }
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

    QJsonArray tracks;
    for (int index = 0; index < m_settings.channels.size(); ++index) {
        QJsonObject track {
            {QStringLiteral("channel"), m_settings.channels.at(index)},
            {QStringLiteral("trackKey"), m_settings.trackKeys.at(index)},
            {QStringLiteral("trackName"), m_settings.trackNames.at(index)},
        };
        if (m_settings.inputSource == RecordingInputSource::NoiseReductionStages) {
            track.insert(QStringLiteral("stage"),
                         noiseReductionDataFlowStageKey(m_settings.noiseReductionStages.at(index)));
        }
        tracks.append(track);
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

    QJsonObject root {
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("baseName"), m_settings.baseName},
        {QStringLiteral("inputSource"), recordingInputSourceKey(m_settings.inputSource)},
        {QStringLiteral("fileLayout"), recordingFileLayoutKey(m_settings.fileLayout)},
        {QStringLiteral("channels"), channels},
        {QStringLiteral("tracks"), tracks},
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
        {QStringLiteral("gainApplied"), m_settings.gainApplied},
    };
    if (m_settings.trackKeys.size() == 1) {
        root.insert(QStringLiteral("trackKey"), m_settings.trackKeys.first());
        root.insert(QStringLiteral("trackName"), m_settings.trackNames.first());
    }
    if (m_settings.gainApplied) {
        root.insert(QStringLiteral("gain"), QJsonObject {
            {QStringLiteral("initial"), gainConfigurationToJson(m_settings.initialGainConfiguration)},
            {QStringLiteral("final"), gainConfigurationToJson(m_finalGainConfiguration)},
        });
    }

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
