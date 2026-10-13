#include "fusioncontext.h"
#include "fusionworker.h"
#include "pipeline.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QTest>
#include <QUdpSocket>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <stdexcept>

using namespace MicArray;
using namespace MicArray::Array128;

namespace {
void require(bool condition, const QString &message)
{
    if (!condition) throw std::runtime_error(message.toStdString());
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), path + ": " + file.errorString());
    return file.readAll();
}

// This is a component wiring regression, NOT an accuracy evaluation. The
// stationary simulated RGB frame is timestamped repeatedly on the same host
// clock as the UDP stream. Only input.json/audio.f32/rgb.png are opened; no
// truth, source directions, saved heat or desired target counts enter inference.
class LocalFusionStream
{
public:
    explicit LocalFusionStream(QElapsedTimer &deadline) : m_deadline(deadline) {}

    ProcessingSettings settings;
    Pipeline pipeline;
    FusionWorker worker;
    FusionVideoBuffer videoBuffer;
    quint32 epoch = 1;
    quint32 flags = 0;
    int clippedChannel = -1;
    QString error;

    void open(const QString &fixture, const QString &modelDirectory, const QString &runtime)
    {
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(readFile(fixture + "/input.json"), &parse);
        require(parse.error == QJsonParseError::NoError && document.isObject(), "Invalid fixture input JSON");
        const auto input = document.object();
        require(input["format"].toString() == "micarray128-srp-input-v1", "Wrong fixture format");
        QString geometryError;
        require(Geometry::load(QJsonDocument(input["geometry"].toObject()).toJson(), &settings.geometry, &geometryError), geometryError);
        settings.localization = true;
        settings.generation = 1;
        const auto camera = input["camera"].toObject();
        settings.camera = {camera["center_azimuth_deg"].toDouble(), camera["center_elevation_deg"].toDouble(),
            camera["horizontal_fov_deg"].toDouble(), camera["vertical_fov_deg"].toDouble(), camera["roll_deg"].toDouble()};
        require(settings.camera.isValid(), "Invalid fixture camera");
        settings.gridColumns = input["grid"].toObject()["columns"].toInt();
        settings.gridRows = input["grid"].toObject()["rows"].toInt();
        settings.soundSpeed = input["sound_speed_m_s"].toDouble(343);
        settings.confidenceThreshold = input["confidence_threshold"].toDouble(.35);
        settings.maxSources = input["max_sources"].toInt(4);
        settings.minimumSourceSeparationDegrees = input["minimum_source_separation_deg"].toDouble(8);
        for (const auto &value : input["disabled_channels"].toArray()) {
            const int channel = value.toInt(-1);
            require(channel >= 0 && channel < Channels, "Invalid fixture channel mask");
            settings.disabledChannels[channel] = true;
        }
        const auto pcm = input["pcm"].toObject();
        require(pcm["path"].toString() == "audio.f32" && pcm["format"].toString() == "f32le-interleaved"
            && pcm["frames"].toInt() == 2048 && pcm["channels"].toInt() == Channels && pcm["sample_rate"].toInt() == SampleRate,
            "Fixture must contain 2048 x 128 f32le audio at 48 kHz");
        const auto bytes = readFile(fixture + "/audio.f32");
        require(bytes.size() == 2048 * Channels * 4, "Wrong PCM length");
        QVector<qint32> samples(2048 * Channels);
        for (qsizetype i = 0; i < samples.size(); ++i) {
            const float sample = std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.constData() + i * 4));
            require(std::isfinite(sample), "Nonfinite PCM fixture");
            samples[i] = qint32(std::lround(std::clamp(double(sample), -1.0, 8388607.0 / 8388608) * 8388608));
        }
        m_image = QImage(fixture + "/rgb.png");
        require(m_image.size() == QSize(320, 192), "Fixture RGB must be 320 x 192");
        QByteArray prototype(DatagramBytes, 0);
        prototype.replace(0, 8, QByteArray::fromHex("4d31323801200180"));
        qToBigEndian<quint32>(SampleRate, prototype.data() + 8);
        qToBigEndian<quint16>(3, prototype.data() + 12);
        qToBigEndian<quint16>(392, prototype.data() + 14);
        // 3 and 2048 are coprime, so 2048 packets cover the payload repetition.
        m_payloads.reserve(2048);
        for (int sequence = 0; sequence < 2048; ++sequence) {
            auto packet = prototype;
            for (int r = 0; r < 3; ++r) for (int ch = 0; ch < Channels; ++ch) {
                const quint32 value = quint32(samples[((sequence * 3 + r) % 2048) * Channels + ch]);
                char *target = packet.data() + 40 + r * 392 + ch * 3;
                target[0] = char(value); target[1] = char(value >> 8); target[2] = char(value >> 16);
            }
            m_payloads.append(packet);
        }
        require(m_sender.bind(QHostAddress::LocalHost, 0), m_sender.errorString());
        QUdpSocket reservation;
        require(reservation.bind(QHostAddress::LocalHost, 0), reservation.errorString());
        m_port = reservation.localPort(); reservation.close();
        FusionWorkerSettings fusion;
        fusion.enabled = true; fusion.generation = settings.generation;
        fusion.modelPath = modelDirectory + "/fusion.onnx";
        fusion.metadataPath = modelDirectory + "/fusion.json";
        fusion.runtimePath = runtime;
        worker.configure(fusion);
        pipeline.configure(settings);
        pipeline.start({"127.0.0.1", m_port, "127.0.0.1", m_sender.localPort()});
        require(waitUntil([&] { return pipeline.snapshot().listening && (worker.snapshot().ready || !worker.snapshot().error.isEmpty()); }, 4000, false),
            "Timed out opening actual pipeline/runtime");
        require(worker.snapshot().ready, worker.snapshot().error);
        require(!worker.snapshot().runtimeVersion.isEmpty(), "Actual runtime version is missing");
        m_pacing.start();
    }

    bool waitUntil(const std::function<bool()> &condition, qint64 timeoutMs, bool stream = true)
    {
        QElapsedTimer phase; phase.start();
        while (phase.elapsed() < timeoutMs && m_deadline.elapsed() < 15000) {
            if (stream && !sendDuePackets()) return false;
            refresh();
            if (condition()) return true;
            QTest::qWait(1);
        }
        return condition();
    }

    bool hasResult() const
    {
        const auto state = worker.snapshot();
        return state.result.valid && state.generation == settings.generation && state.run == pipeline.snapshot().runs;
    }

    void verifyResult()
    {
        const auto result = worker.snapshot();
        require(result.result.valid, result.error);
        require(m_submissions.contains(result.frame), "Result frame did not originate in this UDP/SRP session");
        const auto submitted = m_submissions[result.frame];
        require(result.generation == submitted.generation && result.run == submitted.run, "Inference identity changed across the seam");
        require(result.audioVideoSkewMs == submitted.audioVideoSkewMs, "Pairing skew was not preserved");
        require(result.ageMs >= submitted.ageMs && result.ageMs <= 500 && std::abs(result.audioVideoSkewMs) <= 100,
            "Result age/skew contract violated");
        require(result.result.rawOutput.size() == FusionModel::OutputAnchors * FusionModel::OutputFields,
            "Actual ONNX inference did not return the numeric output tensor");
        require(std::all_of(result.result.rawOutput.cbegin(), result.result.rawOutput.cend(), [](float value) { return std::isfinite(value); }),
            "Nonfinite neural output");
        require(submitted.columns == 72 && submitted.rows == 45 && submitted.heat.size() == 72 * 45,
            "Production SRP heat shape changed");
        require(*std::max_element(submitted.heat.cbegin(), submitted.heat.cend()) > 0,
            "Fixture produced no numerical SRP response");
        require(submitted.video == m_image.convertToFormat(QImage::Format_RGB888), "Buffered pixels changed before inference");
        require(pipeline.snapshot().invalid == 0 && pipeline.snapshot().queueDrops == 0 && pipeline.snapshot().discontinuities == 0,
            "Local test lost or malformed UDP packets");
    }

    void changeConfiguration()
    {
        ++settings.generation;
        settings.confidenceThreshold = settings.confidenceThreshold == .35 ? .36 : .35;
        pipeline.configure(settings);
        // Same explicit integration seam as MainWindow::resetFusion. The worker
        // does not magically observe Pipeline state without its owning caller.
        invalidate();
    }

    void invalidate()
    {
        worker.invalidate(settings.generation, pipeline.snapshot().runs);
        videoBuffer.clear(); m_submissions.clear(); m_lastSubmitted = 0;
    }

private:
    bool sendDuePackets()
    {
        if (!m_pacing.isValid()) return true;
        const quint64 due = quint64(m_pacing.nsecsElapsed() / 62500); // 16000 packets/s, 48000 frames/s.
        while (m_sent < due) {
            auto bytes = m_payloads[m_sent % 2048];
            qToBigEndian(m_sent, bytes.data() + 16);
            qToBigEndian(epoch, bytes.data() + 24); qToBigEndian(flags, bytes.data() + 28);
            for (int r = 0; r < 3; ++r) qToBigEndian(m_sent * 3 + quint64(r), bytes.data() + 32 + r * 392);
            if (clippedChannel >= 0) for (int r = 0; r < 3; ++r) {
                char *sample = bytes.data() + 40 + r * 392 + clippedChannel * 3;
                sample[0] = char(0xff); sample[1] = char(0xff); sample[2] = 0x7f;
            }
            if (m_sender.writeDatagram(bytes, QHostAddress::LocalHost, m_port) != bytes.size()) {
                error = m_sender.errorString(); return false;
            }
            ++m_sent;
        }
        return true;
    }

    void refresh()
    {
        const auto audio = pipeline.snapshot();
        auto neural = worker.snapshot();
        if (neural.generation != settings.generation || neural.run != audio.runs) {
            invalidate(); neural = worker.snapshot();
        }
        const auto now = pipeline.monotonicNowNs();
        if (now - m_lastVideoNs >= 16000000) {
            videoBuffer.append(m_image, now); m_lastVideoNs = now;
        }
        if (!audio.listening || audio.flags & 0x10 || audio.acousticFaultFlags() || audio.lastPacketAgeMs < 0
            || audio.lastPacketAgeMs >= 500 || audio.localization.heat.isEmpty()) {
            if (neural.result.valid || neural.busy || neural.queued) invalidate();
            return;
        }
        if (!neural.ready || audio.localization.generation != settings.generation || audio.localization.run != audio.runs
            || audio.localization.lastFrame == m_lastSubmitted) return;
        const auto paired = videoBuffer.nearest(audio.localization.observationTimeNs, now);
        if (!paired.error.isEmpty()) return;
        FusionPacket packet;
        packet.video = paired.image; packet.heat = audio.localization.heat;
        packet.columns = audio.localization.columns; packet.rows = audio.localization.rows;
        packet.camera = settings.camera; packet.generation = settings.generation;
        packet.run = audio.runs; packet.frame = audio.localization.lastFrame;
        packet.ageMs = qint64(std::ceil(paired.ageMs));
        packet.audioVideoSkewMs = qint64(std::copysign(std::ceil(std::abs(paired.skewMs)), paired.skewMs));
        if (worker.submit(packet)) {
            m_lastSubmitted = packet.frame;
            m_submissions.insert(packet.frame, packet);
            while (m_submissions.size() > 32) m_submissions.erase(m_submissions.begin());
        }
    }

    QElapsedTimer &m_deadline;
    QElapsedTimer m_pacing;
    QUdpSocket m_sender;
    quint16 m_port = 0;
    QVector<QByteArray> m_payloads;
    QImage m_image;
    quint64 m_sent = 0;
    quint64 m_lastSubmitted = 0;
    qint64 m_lastVideoNs = -16000000;
    QMap<quint64, FusionPacket> m_submissions;
};
}

class FusionPipelineTest : public QObject
{
    Q_OBJECT
private slots:
    void realUdpSrpRgbAndOrtKeepIdentityAndInvalidate()
    {
        const auto fixtures = qEnvironmentVariable("M128_FUSION_PIPELINE_FIXTURES");
        const auto model = qEnvironmentVariable("M128_FUSION_MODEL_DIR");
        const auto runtime = qEnvironmentVariable("M128_ONNXRUNTIME");
        if (fixtures.isEmpty() || model.isEmpty() || runtime.isEmpty()) {
            if (qEnvironmentVariableIntValue("M128_REQUIRE_FUSION_RUNTIME"))
                QFAIL("Actual model/runtime and M128_FUSION_PIPELINE_FIXTURES are mandatory for this run");
            QSKIP("Set model/runtime and generated validation fixture paths for local UDP-to-ORT structural testing");
        }
        QElapsedTimer deadline; deadline.start();
        try {
            for (const QString &name : {QString("val-000001"), QString("val-000002")}) {
                LocalFusionStream stream(deadline);
                stream.open(QDir(fixtures).filePath(name), model, runtime);
                require(stream.waitUntil([&] { return stream.hasResult(); }, 1500), "No first real UDP/SRP/ORT result: " + stream.error);
                stream.verifyResult();
                stream.changeConfiguration();
                require(stream.pipeline.snapshot().localization.heat.isEmpty() && !stream.worker.snapshot().result.valid,
                    "Configuration change retained old acoustic/neural output");
                require(stream.waitUntil([&] { return stream.hasResult(); }, 1500), "No result for the updated configuration");
                stream.verifyResult();
                // A masked faulty input must reach real SRP + ONNX despite the
                // device's sticky saturation flag. Reuse an existing exclusion
                // where possible, so the test stays within the mask-count domain.
                const auto excluded = std::find(stream.settings.disabledChannels.cbegin(), stream.settings.disabledChannels.cend(), true);
                const int channel = excluded == stream.settings.disabledChannels.cend() ? 0
                    : int(excluded - stream.settings.disabledChannels.cbegin());
                stream.clippedChannel = channel; stream.flags = 1;
                stream.settings.disabledChannels[channel] = true; stream.changeConfiguration();
                require(stream.waitUntil([&] { return stream.hasResult() && stream.pipeline.snapshot().clippedChannels[channel]; }, 1500),
                    "Masked channel saturation blocked real fusion output");
                stream.verifyResult();
                const auto run = stream.pipeline.snapshot().runs;
                stream.settings.disabledChannels[channel] = false; stream.changeConfiguration();
                require(stream.waitUntil([&] { return stream.pipeline.snapshot().acousticFaultFlags() == 1
                    && stream.pipeline.snapshot().localization.heat.isEmpty() && !stream.worker.snapshot().result.valid; }, 600),
                    "Unmasking a saturated channel retained acoustic/neural output");
                stream.settings.disabledChannels[channel] = true; stream.changeConfiguration();
                require(stream.waitUntil([&] { return stream.hasResult(); }, 1500), "Masking did not recover fusion in the same epoch");
                require(stream.pipeline.snapshot().runs == run && stream.pipeline.snapshot().latchedFlags == 1,
                    "Mask recovery lost session fault evidence");
                stream.verifyResult();
                stream.flags = 4;
                require(stream.waitUntil([&] { return stream.pipeline.snapshot().latchedFlags != 0
                    && stream.pipeline.snapshot().localization.heat.isEmpty() && !stream.worker.snapshot().result.valid; }, 600),
                    "Quality fault did not clear the pipeline/fusion result");
                stream.flags = 0; stream.clippedChannel = -1;
                // A new epoch is the explicit clean-session boundary; no stale
                // pre-fault result is allowed to revive under the new run ID.
                ++stream.epoch;
                require(stream.waitUntil([&] { return stream.hasResult(); }, 1500), "No clean-epoch result after the fault");
                stream.verifyResult();
                require(stream.waitUntil([&] { return stream.pipeline.snapshot().localization.heat.isEmpty()
                    && !stream.worker.snapshot().result.valid; }, 2200, false),
                    "Stopping UDP while simulated video continues retained old results");
                stream.pipeline.stop(); stream.invalidate();
                require(!stream.pipeline.snapshot().listening && stream.pipeline.snapshot().localization.heat.isEmpty()
                    && !stream.worker.snapshot().result.valid, "Stopped acquisition retained output");
                stream.worker.shutdown();
                require(stream.worker.waitForStopped(1000), "Fusion worker did not stop");
                qInfo() << name << "local UDP -> production SRP -> paired simulated RGB -> real ORT passed; not an accuracy metric";
            }
            require(deadline.elapsed() < 15000, "Two-scene structural test exceeded the 15 s deadline");
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};

int main(int argc, char **argv)
{
    Q_INIT_RESOURCE(reference);
    QCoreApplication app(argc, argv);
    FusionPipelineTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_fusionpipeline.moc"
