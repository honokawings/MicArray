#include "recorder.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace MicArray::Array128 {
namespace {
void le16(QByteArray &b, quint16 v) { char p[2]; qToLittleEndian(v, p); b.append(p, 2); }
void le32(QByteArray &b, quint32 v) { char p[4]; qToLittleEndian(v, p); b.append(p, 4); }
void be64(QByteArray &b, quint64 v) { char p[8]; qToBigEndian(v, p); b.append(p, 8); }

QByteArray wavHeader(int channels, quint32 dataBytes)
{
    const bool extended = channels > 2;
    QByteArray b("RIFF", 4);
    le32(b, dataBytes + (extended ? 60 : 36) + (dataBytes & 1));
    b += "WAVEfmt ";
    le32(b, extended ? 40 : 16);
    le16(b, extended ? 0xfffe : 1);
    le16(b, quint16(channels));
    le32(b, SampleRate);
    le32(b, SampleRate * channels * 3);
    le16(b, quint16(channels * 3));
    le16(b, 24);
    if (extended) {
        le16(b, 22); le16(b, 24); le32(b, 0);
        b += QByteArray::fromHex("0100000000001000800000aa00389b71");
    }
    b += "data";
    le32(b, dataBytes);
    return b;
}

class SessionWriter
{
public:
    RecordingSettings settings;
    QString directory;
    QString error;
    QFile evidence;
    QJsonArray segments;
    quint64 packetCount = 0, frameCount = 0, totalBytes = 0;
    QJsonObject diagnostics;

    bool start(const RecordingSettings &s)
    {
        settings = s;
        const QString id = QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz")
            + "_" + QUuid::createUuid().toString(QUuid::Id128).left(8);
        directory = QDir(settings.directory).absoluteFilePath("m128_" + id);
        if (!QDir().mkpath(directory)) { error = QStringLiteral("无法创建录制目录"); return false; }
        evidence.setFileName(QDir(directory).filePath("packets.m128"));
        if (!evidence.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { error = evidence.errorString(); return false; }
        return write(evidence, QByteArray("M128CAP1", 8)) && saveSummary(false, 0);
    }

    bool packets(const QVector<CaptureRecord> &records)
    {
        QByteArray binary;
        binary.reserve(records.size() * 1224);
        for (const auto &record : records) {
            be64(binary, record.arrivalNs);
            be64(binary, record.run);
            binary += record.packet.bytes;
            ++packetCount;
        }
        if (!write(evidence, binary)) return false;
        if (!settings.arraySource) return true;
        for (const auto &record : records) {
            for (int r = 0; r < 3; ++r) {
                const quint64 fid = record.packet.frameIds[r];
                const bool contiguous = r ? fid == record.packet.frameIds[r - 1] + 1 : record.continuous;
                if (m_files.empty() || !contiguous || fid != m_lastFrame + 1 || record.run != m_key
                    || record.packet.epoch != m_epoch
                    || record.packet.selftest() != m_selftest || m_dataBytes >= 256 * 1024 * 1024) {
                    if (!closeSegment() || !openSegment(record.run, fid, record.packet.selftest(), record.packet.epoch)) return false;
                }
                m_lastFrame = fid;
                m_flags |= record.packet.flags;
                if (settings.separate) {
                    for (int i = 0; i < settings.tracks.size(); ++i) {
                        m_pending[i].append(record.packet.pcm(r) + 3 * settings.tracks[i], 3);
                    }
                } else {
                    QByteArray pcm;
                    for (int channel : settings.tracks) pcm.append(record.packet.pcm(r) + channel * 3, 3);
                    m_pending[0] += pcm;
                }
                m_dataBytes += settings.separate ? 3 : settings.tracks.size() * 3;
                ++m_segmentFrames;
                ++frameCount;
            }
        }
        return flushPcm();
    }

    bool stages(const QVector<QVector<float>> &stages, quint64 key, quint64 inputEnd)
    {
        if (settings.arraySource || stages.size() != 4 || stages[0].isEmpty()) return true;
        const int count = int(stages[0].size());
        for (const auto &stage : stages) if (stage.size() != count) { error = QStringLiteral("DSP 录制轨道长度不一致"); return false; }
        if (m_files.empty() || key != m_key || m_dataBytes >= 256 * 1024 * 1024) {
            if (!closeSegment() || !openSegment(key, inputEnd, false)) return false;
        }
        m_lastFrame = inputEnd;
        QVector<QByteArray> bytes(settings.separate ? settings.tracks.size() : 1);
        for (int n = 0; n < count; ++n) for (int i = 0; i < settings.tracks.size(); ++i) {
            const double value = std::clamp(double(stages[settings.tracks[i]][n]), -1.0, 8388607.0 / 8388608.0);
            appendS24(bytes[settings.separate ? i : 0], qint32(std::llround(value * 8388608.0)));
        }
        for (int i = 0; i < bytes.size(); ++i) if (!write(*m_files[size_t(i)], bytes[i])) return false;
        m_dataBytes += quint64(bytes[0].size());
        m_segmentFrames += count;
        frameCount += count;
        return true;
    }

    bool finish(quint64 dropped)
    {
        const bool closed = closeSegment();
        if (evidence.isOpen() && !evidence.flush() && error.isEmpty()) error = evidence.errorString();
        evidence.close();
        return saveSummary(closed && error.isEmpty() && dropped == 0, dropped);
    }
private:
    std::vector<std::unique_ptr<QFile>> m_files;
    QVector<QByteArray> m_pending;
    QStringList m_names;
    quint64 m_key = 0, m_firstFrame = 0, m_lastFrame = 0, m_segmentFrames = 0, m_dataBytes = 0;
    quint32 m_flags = 0;
    quint32 m_epoch = 0;
    bool m_selftest = false;
    int m_part = 0;
    bool write(QFile &file, const QByteArray &bytes)
    {
        if (file.write(bytes) != bytes.size()) { error = file.errorString(); return false; }
        totalBytes += bytes.size();
        return true;
    }
    bool openSegment(quint64 key, quint64 first, bool selftest, quint32 epoch = 0)
    {
        m_key = key; m_firstFrame = first; m_lastFrame = first; m_selftest = selftest;
        m_epoch = epoch;
        m_segmentFrames = 0; m_dataBytes = 0; m_flags = 0; ++m_part; m_names.clear();
        const int count = settings.separate ? int(settings.tracks.size()) : 1;
        m_pending = QVector<QByteArray>(count);
        for (int i = 0; i < count; ++i) {
            const QString suffix = settings.separate
                ? (settings.arraySource ? QStringLiteral("CH%1").arg(settings.tracks[i], 3, 10, QChar('0'))
                                        : QStringLiteral("stage%1").arg(settings.tracks[i]))
                : QStringLiteral("multichannel");
            const QString name = QStringLiteral("part%1_%2.wav").arg(m_part, 4, 10, QChar('0')).arg(suffix);
            auto file = std::make_unique<QFile>(QDir(directory).filePath(name));
            if (!file->open(QIODevice::WriteOnly | QIODevice::NewOnly)) { error = file->errorString(); return false; }
            if (!write(*file, wavHeader(settings.separate ? 1 : int(settings.tracks.size()), 0))) return false;
            m_names.append(name);
            m_files.push_back(std::move(file));
        }
        return true;
    }
    bool closeSegment()
    {
        if (m_files.empty()) return true;
        bool valid = flushPcm();
        for (auto &file : m_files) {
            if (m_dataBytes & 1) valid &= write(*file, QByteArray(1, '\0'));
            if (!file->seek(0)) { error = file->errorString(); valid = false; }
            else valid &= write(*file, wavHeader(settings.separate ? 1 : int(settings.tracks.size()), quint32(m_dataBytes)));
            if (!file->flush()) { error = file->errorString(); valid = false; }
            file->close();
        }
        segments.append(QJsonObject {{"files", QJsonArray::fromStringList(m_names)}, {"run_or_continuity_id", QString::number(m_key)},
            {"first_input_frame", QString::number(m_firstFrame)}, {"last_input_frame", QString::number(m_lastFrame)},
            {"sample_frames", QString::number(m_segmentFrames)}, {"flags_or", int(m_flags)},
            {"selftest", m_selftest}, {"valid_file", valid},
            {"epoch", settings.arraySource ? QJsonValue(QString::number(m_epoch)) : QJsonValue(QJsonValue::Null)},
            {"timeline", settings.arraySource ? "contiguous frame_id" : "DSP emitted samples; input frame bounds, algorithmic latency applies"}});
        m_files.clear();
        return valid;
    }
    bool flushPcm()
    {
        for (int i = 0; i < m_pending.size(); ++i) {
            if (!m_pending[i].isEmpty() && !write(*m_files[size_t(i)], m_pending[i])) return false;
            m_pending[i].clear();
        }
        return true;
    }
    bool saveSummary(bool complete, quint64 dropped)
    {
        QJsonArray tracks;
        for (int track : settings.tracks) tracks.append(track);
        QJsonObject summary {{"format", "micarray128-capture-v1"}, {"complete", complete},
            {"source", settings.arraySource ? "raw-array-s24le" : "dsp-stages-s24le"},
            {"separate_files", settings.separate}, {"tracks", tracks}, {"sample_rate", SampleRate},
            {"packets", QString::number(packetCount)}, {"wav_frames", QString::number(frameCount)},
            {"host_recording_queue_drops", QString::number(dropped)}, {"error", error},
            {"segments", segments}, {"configuration", settings.metadata}, {"final_diagnostics", diagnostics},
            {"evidence", "packets.m128: M128CAP1 + repeated u64be arrival_ns, u64be run_id, 1208-byte UDP payload"}};
        QSaveFile file(QDir(directory).filePath("manifest.json"));
        if (!file.open(QIODevice::WriteOnly)) { error = file.errorString(); return false; }
        const auto bytes = QJsonDocument(summary).toJson();
        if (file.write(bytes) != bytes.size() || !file.commit()) { error = file.errorString(); return false; }
        return true;
    }
};
}

Recorder::Recorder() { setObjectName("M128Recorder"); start(); }
Recorder::~Recorder()
{
    { QMutexLocker lock(&m_mutex); m_quit = true; m_stop = true; m_status.active = false; m_wake.wakeAll(); }
    wait();
}

bool Recorder::begin(const RecordingSettings &settings)
{
    QMutexLocker lock(&m_mutex);
    if (m_status.active || m_status.finishing || m_start || settings.tracks.isEmpty()) return false;
    QSet<int> unique;
    for (int track : settings.tracks) {
        if (track < 0 || track >= (settings.arraySource ? 128 : 4) || unique.contains(track)) return false;
        unique.insert(track);
    }
    m_settings = settings;
    m_diagnostics = {};
    m_status = {};
    m_status.active = true;
    m_start = true;
    m_stop = false;
    m_wake.wakeAll();
    return true;
}

void Recorder::finish()
{
    QMutexLocker lock(&m_mutex);
    if (!m_status.active) return;
    m_status.active = false;
    m_status.finishing = true;
    m_stop = true;
    m_wake.wakeAll();
}

void Recorder::push(Chunk chunk)
{
    QMutexLocker lock(&m_mutex);
    if (!m_status.active) return;
    if (m_status.queueBytes + chunk.bytes > 64 * 1024 * 1024 || m_queue.size() >= 4096) {
        ++m_status.dropped;
        m_status.error = QStringLiteral("录制队列满：已停止接收，正在保存此前数据");
        m_status.active = false;
        m_status.finishing = true;
        m_stop = true;
    } else {
        m_status.queueBytes += chunk.bytes;
        m_queue.push_back(std::move(chunk));
    }
    m_wake.wakeAll();
}

void Recorder::enqueue(const QVector<CaptureRecord> &packets)
{
    if (packets.isEmpty()) return;
    Chunk chunk;
    chunk.packets = packets;
    chunk.bytes = quint64(packets.size()) * 1280;
    push(std::move(chunk));
}

void Recorder::enqueueStages(const QVector<QVector<float>> &stages, quint64 continuityId, quint64 inputEndFrame)
{
    Chunk chunk;
    chunk.stages = stages;
    chunk.continuityId = continuityId;
    chunk.inputEndFrame = inputEndFrame;
    for (const auto &s : stages) chunk.bytes += s.size() * sizeof(float);
    push(std::move(chunk));
}

RecordingStatus Recorder::snapshot() const { QMutexLocker lock(&m_mutex); return m_status; }

void Recorder::setDiagnostics(const QJsonObject &diagnostics)
{
    QMutexLocker lock(&m_mutex); m_diagnostics = diagnostics;
}

void Recorder::run()
{
    std::unique_ptr<SessionWriter> writer;
    for (;;) {
        Chunk chunk;
        bool stop = false, quit = false;
        RecordingSettings settings;
        bool start = false;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_start && m_queue.empty() && !m_stop && !m_quit) m_wake.wait(&m_mutex);
            start = m_start; m_start = false; settings = m_settings;
            if (!m_queue.empty()) {
                chunk = std::move(m_queue.front()); m_queue.pop_front();
                m_status.queueBytes -= chunk.bytes;
            }
            stop = m_stop && m_queue.empty();
            if (stop) m_stop = false;
            quit = m_quit && m_queue.empty();
        }
        if (start) {
            writer = std::make_unique<SessionWriter>();
            writer->start(settings);
        }
        if (writer && writer->error.isEmpty()) {
            if (!chunk.packets.isEmpty()) writer->packets(chunk.packets);
            if (!chunk.stages.isEmpty()) writer->stages(chunk.stages, chunk.continuityId, chunk.inputEndFrame);
        }
        if (writer) {
            QMutexLocker lock(&m_mutex);
            m_status.directory = writer->directory;
            m_status.packets = writer->packetCount; m_status.frames = writer->frameCount;
            m_status.bytes = writer->totalBytes;
            if (!writer->error.isEmpty()) {
                m_status.error = writer->error;
                m_status.active = false;
                m_status.finishing = true;
                m_stop = true;
                m_queue.clear(); m_status.queueBytes = 0;
                stop = true;
            }
        }
        if (stop && writer) {
            const auto status = snapshot();
            { QMutexLocker lock(&m_mutex); writer->diagnostics = m_diagnostics; }
            writer->finish(status.dropped);
            QMutexLocker lock(&m_mutex);
            if (!writer->error.isEmpty()) m_status.error = writer->error;
            m_status.active = false; m_status.finishing = false;
            writer.reset();
        }
        if (quit) break;
    }
}

} // namespace MicArray::Array128
