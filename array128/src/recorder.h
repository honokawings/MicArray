#pragma once

#include "protocol.h"

#include <QJsonObject>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <deque>

namespace MicArray::Array128 {

struct CaptureRecord
{
    Packet packet;
    quint64 arrivalNs = 0;
    quint64 run = 0;
    bool continuous = false;
};

struct RecordingSettings
{
    QString directory;
    bool arraySource = true;
    QVector<int> tracks {0}; // Array CH0..127, or DSP stages 0..3: suppression/BP/LogMMSE/AGC
    bool separate = true;
    QJsonObject metadata;
};

struct RecordingStatus
{
    bool active = false;
    bool finishing = false;
    quint64 packets = 0;
    quint64 frames = 0;
    quint64 bytes = 0;
    quint64 queueBytes = 0;
    quint64 dropped = 0;
    QString directory;
    QString error;
};

class Recorder final : public QThread
{
public:
    Recorder();
    ~Recorder() override;
    bool begin(const RecordingSettings &settings);
    void finish();
    void enqueue(const QVector<CaptureRecord> &packets);
    void enqueueStages(const QVector<QVector<float>> &stages, quint64 continuityId, quint64 inputEndFrame);
    void setDiagnostics(const QJsonObject &diagnostics);
    RecordingStatus snapshot() const;
protected:
    void run() override;
private:
    struct Chunk {
        QVector<CaptureRecord> packets;
        QVector<QVector<float>> stages;
        quint64 continuityId = 0;
        quint64 inputEndFrame = 0;
        quint64 bytes = 0;
    };
    void push(Chunk chunk);
    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    std::deque<Chunk> m_queue;
    RecordingSettings m_settings;
    RecordingStatus m_status;
    QJsonObject m_diagnostics;
    bool m_start = false;
    bool m_stop = false;
    bool m_quit = false;
};

} // namespace MicArray::Array128
