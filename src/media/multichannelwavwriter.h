#pragma once

#include <QDateTime>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QVector>

#include <limits>
#include <functional>
#include <memory>

#include "media/channelgainprocessor.h"
#include "media/noisereductiontypes.h"

namespace MicArray {

enum class RecordingInputSource {
    Array,
    NoiseReductionStages,
};

enum class RecordingFileLayout {
    SingleMultichannel,
    SeparateTracks,
};

struct RecordingSettings {
    QString outputDirectory;
    QString baseName;
    QVector<int> channels;
    int sampleRate = 48000;
    bool gainApplied = false;
    GainConfiguration initialGainConfiguration;
    RecordingInputSource inputSource = RecordingInputSource::Array;
    RecordingFileLayout fileLayout = RecordingFileLayout::SingleMultichannel;
    QVector<NoiseReductionDataFlowStage> noiseReductionStages;
    QStringList trackKeys;
    QStringList trackNames;
};

struct RecordingSummary {
    QString manifestPath;
    QStringList segmentPaths;
    quint64 audioFileCount = 0;
    quint64 inputFrames = 0;
    quint64 sampleFrames = 0;
    quint64 dataBytes = 0;
    quint64 fileBytes = 0;
    QString stopReason;
};

class MultiChannelWavWriter
{
public:
    using FileFactory = std::function<std::unique_ptr<QFile>(const QString &path)>;
    using FileRemover = std::function<bool(const QString &path)>;

    static constexpr quint64 DefaultSegmentDataLimit = static_cast<quint64>(std::numeric_limits<quint32>::max()) - 60ULL;

    explicit MultiChannelWavWriter(quint64 segmentDataLimit = DefaultSegmentDataLimit,
                                   FileFactory fileFactory = {},
                                   FileRemover fileRemover = {});
    ~MultiChannelWavWriter();

    bool open(const RecordingSettings &settings, QString *errorMessage = nullptr);
    bool appendFrame(const QByteArray &payload, QString *errorMessage = nullptr);
    bool updateFinalGainConfiguration(const GainConfiguration &configuration,
                                      QString *errorMessage = nullptr);
    bool close(const QString &stopReason, QString *errorMessage = nullptr);

    bool isOpen() const { return m_open; }
    RecordingSummary summary() const;

private:
    static constexpr int WavHeaderSize = 68;

    bool validateSettings(RecordingSettings *settings, QString *errorMessage) const;
    bool openSegment(QString *errorMessage);
    bool finalizeSegment(QString *errorMessage);
    bool reserveManifest(QString *errorMessage);
    bool writeManifest(QString *errorMessage);
    QByteArray createHeader(quint32 dataSize) const;
    QByteArray selectChannels(const QByteArray &payload) const;
    QString segmentPath(int segmentIndex) const;
    void latchFailure(const QString &message, bool discardCurrentSegment);
    void discardCurrentSegment();
    static void setError(QString *errorMessage, const QString &message);

    quint64 m_segmentDataLimit = DefaultSegmentDataLimit;
    RecordingSettings m_settings;
    RecordingSummary m_summary;
    GainConfiguration m_finalGainConfiguration;
    FileFactory m_fileFactory;
    FileRemover m_fileRemover;
    std::unique_ptr<QFile> m_file;
    quint64 m_segmentDataBytes = 0;
    quint64 m_segmentInputFrames = 0;
    int m_segmentIndex = 0;
    bool m_open = false;
    bool m_failed = false;
    bool m_manifestReserved = false;
    QString m_failureMessage;
    QDateTime m_startedAt;
    QVector<quint64> m_segmentDataSizes;
    QSet<QString> m_invalidSegmentPaths;
};

} // namespace MicArray
