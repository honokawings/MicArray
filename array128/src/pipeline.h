#pragma once

#include "diagnostics.h"
#include "dsp.h"
#include "recorder.h"

#include <QObject>
#include <memory>

namespace MicArray::Array128 {

struct NetworkSettings
{
    QString bindAddress = "0.0.0.0";
    quint16 port = 5000;
    QString sourceAddress = "192.168.10.2";
    quint16 sourcePort = 5000;
};

struct ChannelQuality
{
    double rmsDb = -120;
    double peakDb = -120;
    double dc = 0;
    double acRmsDb = -120;
    quint64 clipped = 0;
    quint64 zeros = 0;
    quint64 unchanged = 0;
};

struct PipelineSnapshot
{
    bool listening = false;
    QString error;
    quint64 received = 0;
    quint64 receivedBytes = 0;
    quint64 valid = 0;
    quint64 invalid = 0;
    quint64 rejectedSource = 0;
    quint64 queueDrops = 0;
    int queued = 0;
    int socketBuffer = 0;
    quint64 runs = 0;
    quint64 sequence = 0;
    quint64 frame = 0;
    quint32 epoch = 0;
    quint32 flags = 0;
    quint32 latchedFlags = 0;
    quint64 discontinuities = 0;
    quint64 staleEpochPackets = 0;
    quint64 analysisReplaced = 0;
    quint64 monitorDrops = 0;
    double packetRate = 0;
    double byteRate = 0;
    qint64 lastPacketAgeMs = -1;
    qint64 lastSourceDatagramAgeMs = -1;
    qint64 processingDelayMs = -1;
    QJsonObject packetSequence;
    QJsonObject frameSequence;
    SelftestAudit selftest;
    std::array<ChannelQuality, Channels> quality;
    QVector<float> waveform;
    QByteArray latestHeader;
    QString acousticStatus = QStringLiteral("尚未收到数据");
    LocalizationResult localization;
    QJsonObject json() const;
};

class Pipeline final : public QObject
{
    Q_OBJECT
public:
    explicit Pipeline(QObject *parent = nullptr);
    ~Pipeline() override;
    void start(const NetworkSettings &settings);
    void stop();
    void configure(const ProcessingSettings &settings);
    PipelineSnapshot snapshot() const;
    QByteArray takeMonitorPcm();
    Recorder &recorder();
private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace MicArray::Array128
