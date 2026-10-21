#include <QDir>
#include <QtTest/QtTest>

#include "analysis/arraygeometry.h"
#include "app/appsettings.h"
#include "control/pldenoisesession.h"
#include "device/deviceclient.h"
#include "media/audiolocationsideband.h"
#include "media/noisereductionworker.h"
#include "media/pldenoisetransport.h"
#include "protocol/protocolcodec.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>

#include <bit>
#include <cmath>
#include <limits>

using namespace MicArray;

namespace {

void put16(QByteArray &bytes, int offset, quint16 value)
{
    qToLittleEndian(value, bytes.data() + offset);
}
void put32(QByteArray &bytes, int offset, quint32 value)
{
    qToLittleEndian(value, bytes.data() + offset);
}
QByteArray checked(QByteArray packet)
{
    packet[40] = char(PlDenoiseTransport::crc8(packet.first(40)));
    return packet;
}
QByteArray statusPacket(quint16 config = 10, quint16 txn = 0, quint16 field = 0,
                        quint8 flags = 1, quint16 error = 0, quint32 overflow = 0)
{
    QByteArray packet(41, 0);
    packet.replace(0, 4, "DNS1");
    packet[4] = 1;
    packet[5] = 1;
    put16(packet, 6, config);
    put16(packet, 8, txn);
    put16(packet, 10, field);
    packet[12] = char(flags);
    put32(packet, 13, 1024);
    put32(packet, 17, overflow);
    put32(packet, 25, 0x20261020);
    put16(packet, 29, error);
    return checked(packet);
}
QByteArray audioPacket(quint32 index, quint16 config, int count = 2,
                       std::array<std::array<float, 3>, 2> values = {{{0.1f, -0.2f, 0.3f}, {0.2f, -0.1f, 0.4f}}})
{
    QByteArray packet(41, 0);
    packet.replace(0, 4, "AUD3");
    packet[4] = 1;
    packet[5] = 0x11;
    put16(packet, 6, config);
    put32(packet, 8, index);
    packet[12] = char(count);
    for (int tuple = 0; tuple < std::min(count, 2); ++tuple)
        for (int beam = 0; beam < 3; ++beam)
            put32(packet, 13 + tuple * 12 + beam * 4, std::bit_cast<quint32>(values[tuple][beam]));
    return checked(packet);
}
QByteArray rawFrame(const QVector<QByteArray> &packets)
{
    QByteArray payload(38400, 0);
    for (int sample = 0; sample < 200; ++sample)
        for (int channel = 0; channel < 48; ++channel)
            put32(payload, sample * 192 + channel * 4, quint32(qint32(-12345 + channel * 31)));
    for (int sample = 0; sample < packets.size(); ++sample)
        for (int channel = 7; channel < 48; ++channel)
            payload[sample * 192 + channel * 4 + 3] = packets.at(sample).at(channel - 7);
    return payload;
}
void initialize(PlDenoiseTransport &transport, quint16 config = 10)
{
    transport.consume(rawFrame({statusPacket(config)}), 1, 0);
    transport.authorizeConfiguration(config, true);
}
QVector<float> readFloats(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = file.readAll();
    QVector<float> values;
    values.reserve(bytes.size() / 4);
    for (int index = 0; index + 4 <= bytes.size(); index += 4)
        values.append(std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.constData() + index)));
    return values;
}
NoiseReductionSettings goldenSettings(const QJsonObject &config)
{
    NoiseReductionSettings settings;
    settings.enabled = true;
    settings.monitorEnabled = false;
    settings.inputMode = NoiseReductionInputMode::PlThreeBeamOriginalDenoising;
    const auto directions = config["directions"].toObject();
    auto direction = [&directions](const QString &name) {
        const auto value = directions[name].toObject();
        return ContinuousDirectionTarget{value["azimuth_deg"].toDouble(), value["elevation_deg"].toDouble()};
    };
    settings.enhancedA = direction("A");
    settings.suppressionB1 = direction("B1");
    settings.suppressionB2 = direction("B2");
    const auto wiener = config["wiener"].toObject();
    settings.algorithm = wiener["algorithm"].toString() == "adaptive"
        ? NoiseReductionAlgorithm::AdaptiveWiener : NoiseReductionAlgorithm::FixedParameterWiener;
    settings.mode = wiener["mode"].toString() == "singlePoint" ? NoiseReductionMode::SinglePoint : NoiseReductionMode::TwoPoint;
    settings.singleSuppressionCoefficient = wiener["single_coefficient"].toDouble();
    settings.dualB1SuppressionCoefficient = wiener["b1_coefficient"].toDouble();
    settings.dualB2SuppressionCoefficient = wiener["b2_coefficient"].toDouble();
    const auto band = config["bandpass"].toObject();
    settings.bandPass.enabled = band["enabled"].toBool();
    settings.bandPass.lowCutoffHz = band["low_hz"].toDouble();
    settings.bandPass.highCutoffHz = band["high_hz"].toDouble();
    const auto log = config["logmmse"].toObject();
    settings.logMmse.enabled = log["enabled"].toBool();
    settings.logMmse.fftSize = log["fft_size"].toInt();
    settings.logMmse.overlapRatio = log["overlap_ratio"].toDouble();
    settings.logMmse.priorSnrSmoothing = log["prior_snr_smoothing"].toDouble();
    settings.logMmse.noisePowerSmoothing = log["noise_power_smoothing"].toDouble();
    settings.logMmse.minimumGainDb = log["minimum_gain_db"].toDouble();
    const auto agc = config["agc"].toObject();
    settings.automaticGainControl.enabled = agc["enabled"].toBool();
    settings.automaticGainControl.targetLevelDbfs = agc["target_level_dbfs"].toDouble();
    settings.automaticGainControl.maximumGainDb = agc["maximum_gain_db"].toDouble();
    settings.automaticGainControl.maximumAttenuationDb = agc["maximum_attenuation_db"].toDouble();
    settings.automaticGainControl.attackMilliseconds = agc["attack_ms"].toDouble();
    settings.automaticGainControl.releaseMilliseconds = agc["release_ms"].toDouble();
    settings.automaticGainControl.limiterLevelDbfs = agc["limiter_level_dbfs"].toDouble();
    settings.postProcessingOrder.clear();
    for (const QJsonValue &node : config["post_processing_order"].toArray())
        settings.postProcessingOrder.append(node.toString() == "bandpass" ? NoisePostProcessingNode::BandPass
            : (node.toString() == "logmmse" ? NoisePostProcessingNode::LogMmse : NoisePostProcessingNode::AutomaticGainControl));
    return settings;
}

} // namespace

class PlDenoiseTest : public QObject
{
    Q_OBJECT
private slots:
    void standardCrcAndWireCommand();
    void slopesUseContinuousOriginalFloatDirections();
    void handshakeRequiresEveryAckAndActiveCommit();
    void directionUpdateDoesNotReset();
    void retriesSameCommandAndStopsOnTimeout();
    void ignoresStaleAcknowledgement();
    void rejectsDeviceErrorsAndUnsupportedRate();
    void reconstructsSynchronizedFloatSamplesAndPreservesRaw();
    void rejectsMalformedPackets_data();
    void rejectsMalformedPackets();
    void detectsGapDuplicateAndStaleEpoch();
    void epochTransitionRetainsPartialFramesAndHistory();
    void buffersNewEpochUntilDelayedCommitAck();
    void transientControlCrcAllowsIdempotentRetry();
    void rawChannelsCannotBecomePlBeams();
    void workerRejectsUnauthorizedAndDiscontinuousBeams();
    void savesExplicitModeWithoutChangingCamera();
    void originalGoldenThroughTransportMatchesAllFourStages_data();
    void originalGoldenThroughTransportMatchesAllFourStages();
    void tcpMockHandshakeRetryAndGoldenFloatStream();
};

void PlDenoiseTest::standardCrcAndWireCommand()
{
    QCOMPARE(PlDenoiseSession::crc16("123456789"), quint16(0x29b1));
    QCOMPARE(PlDenoiseTransport::crc8("123456789"), quint8(0xf4));
    const auto values = PlDenoiseSession::command(0xbeef, 3, -123456789);
    const QByteArray wire = ProtocolCodec::encodeControl(values);
    QCOMPARE(wire.size(), 26);
    QCOMPARE(qFromLittleEndian<quint16>(wire.constData() + 8), quint16(0x4e44));
    QCOMPARE(qFromLittleEndian<quint16>(wire.constData() + 10), quint16(0xbeef));
    QCOMPARE(qFromLittleEndian<quint16>(wire.constData() + 12), quint16(3));
    QCOMPARE(qFromLittleEndian<qint32>(wire.constData() + 14), qint32(-123456789));
    QCOMPARE(qFromLittleEndian<quint16>(wire.constData() + 18), PlDenoiseSession::crc16(wire.mid(8, 10)));
    QCOMPARE(wire.mid(20, 4), QByteArray(4, 0));
}

void PlDenoiseTest::slopesUseContinuousOriginalFloatDirections()
{
    NoiseReductionSettings settings;
    settings.enhancedA = {-5.885, -16.134};
    settings.suppressionB1 = {90, 0};
    settings.suppressionB2 = {-90, 0};
    const auto slopes = PlDenoiseSession::directionSlopes(settings);
    const auto direction = directionFromAngles(-5.885, -16.134);
    QCOMPARE(slopes[0], qint32(std::llround((0.025 * 48000 / 343) * double(direction.x()) * (1 << 26))));
    QCOMPARE(slopes[1], qint32(std::llround((0.025 * 48000 / 343) * double(direction.y()) * (1 << 26))));
    QVERIFY(slopes[2] > 0);
    QCOMPARE(slopes[2], -slopes[4]);
    QCOMPARE(slopes[3], qint32(0));
    auto slightlyDifferent = settings;
    slightlyDifferent.enhancedA.azimuthDegrees += 0.001;
    QVERIFY(PlDenoiseSession::directionSlopes(slightlyDifferent)[0] != slopes[0]);
}

void PlDenoiseTest::handshakeRequiresEveryAckAndActiveCommit()
{
    PlDenoiseSession session;
    QSignalSpy commands(&session, &PlDenoiseSession::commandReady);
    QSignalSpy applied(&session, &PlDenoiseSession::configurationApplied);
    session.configure({}, 48000);
    session.setAvailable(true, true);
    const std::array<quint16, 9> fields {0x102, 0x100, 0, 1, 2, 3, 4, 5, 0x101};
    for (int index = 0; index < 9; ++index)
    {
        QCOMPARE(commands.size(), index + 1);
        const auto command = qvariant_cast<ControlValues>(commands.last()[0]);
        QCOMPARE(quint16(command.channelValue(2)), fields[index]);
        PlDenoiseStatus status;
        status.ackTransaction = quint16(command.channelValue(1));
        status.ackField = fields[index];
        if (index == 8)
        {
            session.acknowledge(status); // Mere receipt does not mean the bank was applied.
            QVERIFY(!session.ready());
            QCOMPARE(applied.size(), 0);
            status.flags = 1;
            status.activeConfigId = status.ackTransaction;
        }
        session.acknowledge(status);
    }
    QVERIFY(session.ready());
    QCOMPARE(applied.size(), 1);
    QCOMPARE(applied[0][1].toBool(), true);
}

void PlDenoiseTest::directionUpdateDoesNotReset()
{
    PlDenoiseSession session;
    QVector<quint16> fields;
    connect(&session, &PlDenoiseSession::commandReady, &session, [&](const ControlValues &command) {
        const auto field = quint16(command.channelValue(2));
        fields.append(field);
        PlDenoiseStatus status;
        status.ackTransaction = quint16(command.channelValue(1));
        status.ackField = field;
        status.flags = 1;
        status.activeConfigId = status.ackTransaction;
        session.acknowledge(status);
    });
    QSignalSpy applied(&session, &PlDenoiseSession::configurationApplied);
    NoiseReductionSettings settings;
    session.configure(settings, 48000);
    session.setAvailable(true, true);
    QCOMPARE(fields.size(), 9);
    fields.clear();
    settings.enhancedA = {23.456, -12.345};
    session.configure(settings, 48000);
    QCOMPARE(fields.size(), 8);
    QVERIFY(!fields.contains(0x102));
    QCOMPARE(applied.size(), 2);
    QCOMPARE(applied[1][1].toBool(), false);
}

void PlDenoiseTest::retriesSameCommandAndStopsOnTimeout()
{
    PlDenoiseSession session;
    QSignalSpy commands(&session, &PlDenoiseSession::commandReady);
    QSignalSpy failures(&session, &PlDenoiseSession::failed);
    session.configure({}, 48000);
    session.setAvailable(true, true);
    for (int retry = 0; retry < 8; ++retry) session.retryNow();
    QCOMPARE(commands.size(), 9);
    const QByteArray first = ProtocolCodec::encodeControl(qvariant_cast<ControlValues>(commands[0][0]));
    for (const auto &emission : commands)
        QCOMPARE(ProtocolCodec::encodeControl(qvariant_cast<ControlValues>(emission[0])), first);
    session.retryNow();
    QCOMPARE(failures.size(), 1);
    QVERIFY(!session.ready());
    QVERIFY(!session.busy());
}

void PlDenoiseTest::ignoresStaleAcknowledgement()
{
    PlDenoiseSession session;
    QSignalSpy commands(&session, &PlDenoiseSession::commandReady);
    session.configure({}, 48000);
    session.setAvailable(true, true);
    const auto command = qvariant_cast<ControlValues>(commands[0][0]);
    PlDenoiseStatus status;
    status.ackTransaction = quint16(command.channelValue(1)) + 1;
    status.ackField = 0x102;
    session.acknowledge(status);
    QCOMPARE(commands.size(), 1);
    status.ackTransaction = quint16(command.channelValue(1));
    status.ackField = 0x100;
    session.acknowledge(status);
    QCOMPARE(commands.size(), 1);
}

void PlDenoiseTest::rejectsDeviceErrorsAndUnsupportedRate()
{
    PlDenoiseSession session;
    QSignalSpy commands(&session, &PlDenoiseSession::commandReady);
    QSignalSpy errors(&session, &PlDenoiseSession::failed);
    session.configure({}, 16000);
    session.setAvailable(true, true);
    QCOMPARE(commands.size(), 0);
    QCOMPARE(errors.size(), 1);
    session.configure({}, 48000);
    QVERIFY(session.busy());
    const auto command = qvariant_cast<ControlValues>(commands.last()[0]);
    PlDenoiseStatus status;
    status.ackTransaction = quint16(command.channelValue(1));
    status.ackField = 0x102;
    status.errorCode = 3;
    session.acknowledge(status);
    QCOMPARE(errors.size(), 2);
    QVERIFY(!session.ready());
}

void PlDenoiseTest::reconstructsSynchronizedFloatSamplesAndPreservesRaw()
{
    PlDenoiseTransport transport;
    QSignalSpy frames(&transport, &PlDenoiseTransport::frameReady);
    initialize(transport);
    QVector<QByteArray> packets;
    for (int index = 0; index < 100; ++index) packets.append(audioPacket(index * 2, 10));
    const QByteArray raw = rawFrame(packets);
    const QByteArray unchanged = raw;
    transport.consume(raw, 3, 0);
    QCOMPARE(raw, unchanged);
    QVERIFY(!transport.faulted());
    QCOMPARE(frames.size(), 1);
    const auto frame = qvariant_cast<PlBeamFrame>(frames[0][0]);
    QCOMPARE(frame.beams[0].size(), 200);
    QCOMPARE(frame.beams[0][0], 0.1f);
    QCOMPARE(frame.beams[1][0], -0.2f);
    QCOMPARE(frame.beams[2][1], 0.4f);
    const auto restored = AudioLocationSidebandDecoder::decode(raw, 3, 0);
    QVERIFY(restored.valid);
    for (int sample = 0; sample < 200; ++sample)
        for (int channel = 0; channel < 48; ++channel)
            QCOMPARE(qFromLittleEndian<qint32>(restored.pcmPayload.constData() + sample * 192 + channel * 4),
                     qint32(-12345 + channel * 31));
}

void PlDenoiseTest::rejectsMalformedPackets_data()
{
    QTest::addColumn<QByteArray>("packet");
    auto bad = audioPacket(0, 10); bad[40] ^= 1;
    QTest::newRow("crc") << bad;
    bad = audioPacket(0, 10); bad[4] = 2;
    QTest::newRow("version") << checked(bad);
    bad = audioPacket(0, 10); bad[5] = 0x21;
    QTest::newRow("stage") << checked(bad);
    bad = audioPacket(0, 10); bad[12] = 3;
    QTest::newRow("count") << checked(bad);
    bad = audioPacket(0, 10); bad[38] = 1;
    QTest::newRow("reserved") << checked(bad);
    bad = audioPacket(0, 11);
    QTest::newRow("unknown-config") << bad;
    bad = audioPacket(0, 10); put32(bad, 13, 0x7fc00000);
    QTest::newRow("nan") << checked(bad);
    bad = audioPacket(0, 10); put32(bad, 29, 0x7f800000);
    QTest::newRow("infinity-second-tuple") << checked(bad);
    bad = statusPacket(); put32(bad, 25, 0x11111111);
    QTest::newRow("wrong-build") << checked(bad);
    QTest::newRow("pl-overflow") << statusPacket(10, 0, 0, 5, 0, 1);
    QTest::newRow("pl-error") << statusPacket(10, 0, 0, 5, 2, 0);
}

void PlDenoiseTest::rejectsMalformedPackets()
{
    QFETCH(QByteArray, packet);
    PlDenoiseTransport transport;
    QSignalSpy errors(&transport, &PlDenoiseTransport::faultRaised);
    QSignalSpy frames(&transport, &PlDenoiseTransport::frameReady);
    initialize(transport);
    transport.consume(rawFrame({packet}), 1, 0);
    QVERIFY(transport.faulted());
    QCOMPARE(errors.size(), 1);
    QVector<QByteArray> valid;
    for (int index = 0; index < 100; ++index) valid.append(audioPacket(index * 2, 10));
    transport.consume(rawFrame(valid), 1, 10000);
    QCOMPARE(frames.size(), 0); // No hidden fallback or implicit gap repair.
}

void PlDenoiseTest::detectsGapDuplicateAndStaleEpoch()
{
    for (quint32 badIndex : {quint32(0), quint32(3)})
    {
        PlDenoiseTransport transport;
        initialize(transport);
        transport.consume(rawFrame({audioPacket(0, 10), audioPacket(badIndex, 10)}), 1, 0);
        QVERIFY(transport.faulted());
    }
    PlDenoiseTransport transport;
    initialize(transport);
    transport.consume(rawFrame({audioPacket(0, 10)}), 1, 0);
    transport.authorizeConfiguration(11, false);
    transport.consume(rawFrame({audioPacket(2, 11), audioPacket(4, 10)}), 1, 10000);
    QVERIFY(transport.faulted());
}

void PlDenoiseTest::epochTransitionRetainsPartialFramesAndHistory()
{
    PlDenoiseTransport transport;
    QSignalSpy frames(&transport, &PlDenoiseTransport::frameReady);
    initialize(transport);
    transport.consume(rawFrame({audioPacket(0, 10), audioPacket(2, 10, 1)}), 1, 0);
    transport.authorizeConfiguration(11, false);
    QVector<QByteArray> packets;
    for (int index = 0; index < 100; ++index) packets.append(audioPacket(3 + index * 2, 11));
    transport.consume(rawFrame(packets), 1, 10000);
    QVERIFY(!transport.faulted());
    QCOMPARE(frames.size(), 2);
    QCOMPARE(qvariant_cast<PlBeamFrame>(frames[0][0]).beams[0].size(), 3);
    QCOMPARE(qvariant_cast<PlBeamFrame>(frames[0][0]).configId, quint16(10));
    QCOMPARE(qvariant_cast<PlBeamFrame>(frames[1][0]).firstSampleIndex, quint32(3));
    QCOMPARE(qvariant_cast<PlBeamFrame>(frames[1][0]).beams[0].size(), 200);
}

void PlDenoiseTest::buffersNewEpochUntilDelayedCommitAck()
{
    PlDenoiseTransport transport;
    QSignalSpy frames(&transport, &PlDenoiseTransport::frameReady);
    initialize(transport, 10);
    transport.consume(rawFrame({audioPacket(0, 10)}), 1, 0);
    transport.setControlTransactionActive(true);
    transport.setPendingConfiguration(11);
    QVector<QByteArray> pending;
    for (int index = 0; index < 100; ++index) pending.append(audioPacket(2 + index * 2, 11));
    transport.consume(rawFrame(pending), 1, 10000); // COMMIT ACK is delayed/lost; no audio may disappear.
    QCOMPARE(frames.size(), 0);
    QVERIFY(!transport.faulted());
    transport.setPendingConfiguration(11); // Same transaction is retried idempotently.
    transport.consume(rawFrame({statusPacket(11, 11, 0x100)}), 1, 20000);
    QCOMPARE(frames.size(), 0);
    transport.authorizeConfiguration(11, false);
    QVERIFY(!transport.faulted());
    QCOMPARE(frames.size(), 2);
    QCOMPARE(qvariant_cast<PlBeamFrame>(frames[0][0]).beams[0].size(), 2);
    QCOMPARE(qvariant_cast<PlBeamFrame>(frames[1][0]).firstSampleIndex, quint32(2));
    QCOMPARE(qvariant_cast<PlBeamFrame>(frames[1][0]).beams[0].size(), 200);
    QCOMPARE(transport.receivedSamples(), quint64(202));
}

void PlDenoiseTest::transientControlCrcAllowsIdempotentRetry()
{
    PlDenoiseTransport transport;
    initialize(transport);
    transport.setControlTransactionActive(true);
    transport.consume(rawFrame({statusPacket(10, 11, 0x100, 5, 1)}), 1, 0);
    QVERIFY(!transport.faulted());
    transport.setControlTransactionActive(false);
    transport.consume(rawFrame({statusPacket(10, 11, 0x100, 5, 1)}), 1, 0);
    QVERIFY(transport.faulted());
    PlDenoiseSession session;
    QSignalSpy commands(&session, &PlDenoiseSession::commandReady);
    QSignalSpy errors(&session, &PlDenoiseSession::failed);
    session.configure({}, 48000);
    session.setAvailable(true, true);
    const auto values = qvariant_cast<ControlValues>(commands[0][0]);
    PlDenoiseStatus status;
    status.ackTransaction = quint16(values.channelValue(1));
    status.errorCode = 1;
    status.flags = 4;
    session.acknowledge(status);
    QCOMPARE(errors.size(), 0);
    session.retryNow();
    QCOMPARE(commands.size(), 2);
    QCOMPARE(ProtocolCodec::encodeControl(qvariant_cast<ControlValues>(commands[0][0])),
             ProtocolCodec::encodeControl(qvariant_cast<ControlValues>(commands[1][0])));
}

void PlDenoiseTest::rawChannelsCannotBecomePlBeams()
{
    NoiseReductionWorker worker;
    NoiseReductionSettings settings;
    settings.enabled = true;
    settings.monitorEnabled = false;
    settings.inputMode = NoiseReductionInputMode::PlThreeBeamOriginalDenoising;
    worker.configure(settings, 48000);
    worker.setActive(true);
    worker.setRecordingStages({NoiseReductionDataFlowStage::Suppression});
    QSignalSpy output(&worker, &NoiseReductionWorker::recordingFrameReady);
    for (int index = 0; index < 20; ++index) worker.processRawFrame(rawFrame({}), 1, index * 4167);
    QCOMPARE(output.size(), 0);
}

void PlDenoiseTest::workerRejectsUnauthorizedAndDiscontinuousBeams()
{
    NoiseReductionWorker worker;
    NoiseReductionSettings settings;
    settings.enabled = true;
    settings.monitorEnabled = false;
    settings.inputMode = NoiseReductionInputMode::PlThreeBeamOriginalDenoising;
    worker.configure(settings, 48000);
    worker.setActive(true);
    QSignalSpy errors(&worker, &NoiseReductionWorker::warningRaised);
    PlBeamFrame frame;
    frame.configId = 7;
    for (auto &beam : frame.beams) beam.fill(0.1f, 200);
    worker.processPlBeamFrame(frame);
    QCOMPARE(errors.size(), 1);
    worker.authorizePlConfiguration(7, true);
    worker.processPlBeamFrame(frame);
    QCOMPARE(errors.size(), 1);
    frame.firstSampleIndex = 201;
    worker.processPlBeamFrame(frame);
    QCOMPARE(errors.size(), 2);
}

void PlDenoiseTest::savesExplicitModeWithoutChangingCamera()
{
    QTemporaryDir directory;
    AppSettings settings(directory.filePath("settings.ini"));
    auto values = settings.noiseReduction();
    values.inputMode = NoiseReductionInputMode::PlThreeBeamOriginalDenoising;
    values.camera.centerAzimuthDegrees = 12.3;
    settings.saveNoiseReduction(values);
    QCOMPARE(settings.noiseReduction().inputMode, NoiseReductionInputMode::PlThreeBeamOriginalDenoising);
    QCOMPARE(settings.noiseReduction().camera.centerAzimuthDegrees, 12.3);
}

void PlDenoiseTest::originalGoldenThroughTransportMatchesAllFourStages_data()
{
    QTest::addColumn<QString>("caseName");
    QTest::addColumn<bool>("changeEpoch");
    for (const QString &name : {QString("background"), QString("A_only"), QString("B_only"), QString("A_and_B")})
        QTest::newRow(name.toLatin1().constData()) << name << false;
    QTest::newRow("A_and_B_direction_epoch_no_history_reset") << QString("A_and_B") << true;
}

void PlDenoiseTest::originalGoldenThroughTransportMatchesAllFourStages()
{
    if (!QDir(QString::fromUtf8(MICARRAY_DENOISE_GOLDEN_ROOT)).exists())
        QSKIP("Private field-audio golden fixtures are not distributed; set MICARRAY_DENOISE_GOLDEN_ROOT to run this check.");
    QFETCH(QString, caseName);
    QFETCH(bool, changeEpoch);
    const QString directory = QString::fromUtf8(MICARRAY_DENOISE_GOLDEN_ROOT) + "/" + caseName + "/reference/";
    QFile metadataFile(directory + "metadata.json");
    QVERIFY2(metadataFile.open(QIODevice::ReadOnly), qPrintable(directory));
    const auto metadata = QJsonDocument::fromJson(metadataFile.readAll()).object();
    const NoiseReductionSettings settings = goldenSettings(metadata["config"].toObject());
    std::array<QVector<float>, 3> beams {readFloats(directory + "beam_A.f32le"),
        readFloats(directory + "beam_B1.f32le"), readFloats(directory + "beam_B2.f32le")};
    QCOMPARE(beams[0].size(), 144000);
    QCOMPARE(beams[1].size(), beams[0].size());
    QCOMPARE(beams[2].size(), beams[0].size());
    NoiseReductionWorker worker;
    worker.configure(settings, 48000);
    worker.setActive(true);
    worker.setRecordingStages({NoiseReductionDataFlowStage::Suppression, NoiseReductionDataFlowStage::BandPass,
                               NoiseReductionDataFlowStage::LogMmse, NoiseReductionDataFlowStage::AutomaticGainControl});
    PlDenoiseTransport transport;
    initialize(transport, 10);
    worker.authorizePlConfiguration(10, true);
    connect(&transport, &PlDenoiseTransport::frameReady, &worker, &NoiseReductionWorker::processPlBeamFrame);
    std::array<QVector<float>, 4> collected;
    connect(&worker, &NoiseReductionWorker::recordingFrameReady, &worker,
        [&](const NoiseReductionRecordingFrame &frame) {
            for (int stage = 0; stage < 4; ++stage) collected[stage] += frame.stages[stage].samples;
        });
    QSignalSpy errors(&worker, &NoiseReductionWorker::warningRaised);
    quint16 config = 10;
    quint32 index = 0;
    QVector<QByteArray> packets;
    while (index < quint32(beams[0].size()))
    {
        if (changeEpoch && index == 1500)
        {
            if (!packets.isEmpty()) { transport.consume(rawFrame(packets), 1, index * 1000000ULL / 48000); packets.clear(); }
            config = 11;
            transport.authorizeConfiguration(config, false);
            worker.authorizePlConfiguration(config, false);
        }
        const int count = std::min(2, int(beams[0].size()) - int(index));
        std::array<std::array<float, 3>, 2> values {};
        for (int tuple = 0; tuple < count; ++tuple)
            for (int beam = 0; beam < 3; ++beam) values[tuple][beam] = beams[beam][index + tuple];
        packets.append(audioPacket(index, config, count, values));
        index += count;
        if (packets.size() == 200)
        {
            transport.consume(rawFrame(packets), 1, index * 1000000ULL / 48000);
            packets.clear();
        }
    }
    if (!packets.isEmpty()) transport.consume(rawFrame(packets), 1, index * 1000000ULL / 48000);
    QVERIFY(!transport.faulted());
    QCOMPARE(errors.size(), 0);
    const std::array<QString, 4> names {"wiener", "post_bandpass", "post_logmmse", "post_agc"};
    for (int stage = 0; stage < 4; ++stage)
    {
        const auto expected = readFloats(directory + names[stage] + ".f32le");
        QCOMPARE(collected[stage].size(), expected.size() / 200 * 200);
        QCOMPARE(collected[stage], expected.first(collected[stage].size()));
    }
}

void PlDenoiseTest::tcpMockHandshakeRetryAndGoldenFloatStream()
{
    if (!QDir(QString::fromUtf8(MICARRAY_DENOISE_GOLDEN_ROOT)).exists())
        QSKIP("Private field-audio golden fixtures are not distributed; set MICARRAY_DENOISE_GOLDEN_ROOT to run this check.");
    const QString directory = QString::fromUtf8(MICARRAY_DENOISE_GOLDEN_ROOT) + "/A_and_B/reference/";
    QFile metadataFile(directory + "metadata.json");
    QVERIFY(metadataFile.open(QIODevice::ReadOnly));
    const auto metadata = QJsonDocument::fromJson(metadataFile.readAll()).object();
    const auto settings = goldenSettings(metadata["config"].toObject());
    const std::array<QVector<float>, 3> beams {readFloats(directory + "beam_A.f32le"),
        readFloats(directory + "beam_B1.f32le"), readFloats(directory + "beam_B2.f32le")};
    QCOMPARE(beams[0].size(), 144000);
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    DeviceClient client;
    client.setAutoReconnect(false);
    PlDenoiseSession session;
    PlDenoiseTransport transport;
    NoiseReductionWorker worker;
    worker.configure(settings, 48000);
    worker.setActive(true);
    worker.setRecordingStages({NoiseReductionDataFlowStage::Suppression, NoiseReductionDataFlowStage::BandPass,
        NoiseReductionDataFlowStage::LogMmse, NoiseReductionDataFlowStage::AutomaticGainControl});
    worker.setDiagnosticStageCapture(true);
    std::array<QVector<float>, 4> output;
    connect(&worker, &NoiseReductionWorker::diagnosticStageFrameReady, &worker,
        [&](const NoiseReductionRecordingFrame &frame) {
            for (int stage = 0; stage < 4; ++stage) output[stage] += frame.stages[stage].samples;
        });
    QSignalSpy workerErrors(&worker, &NoiseReductionWorker::warningRaised);
    QSignalSpy transportErrors(&transport, &PlDenoiseTransport::faultRaised);
    QSignalSpy sessionErrors(&session, &PlDenoiseSession::failed);
    QTcpSocket *socket = nullptr;
    QByteArray incoming;
    bool wireValid = true, droppedOneAck = false;
    quint16 transaction = 0, activeConfig = 0, lastField = 0;
    quint8 fieldMask = 0;
    int commands = 0;
    quint32 sampleIndex = 0;
    QTimer stream;
    stream.setInterval(1);
    auto sendAudio = [&](const QVector<QByteArray> &packets) {
        if (!socket) { wireValid = false; return; }
        QByteArray frame(16, 0);
        put16(frame, 0, 0x7e7e);
        put16(frame, 2, 0xee11);
        put32(frame, 4, 38400);
        put32(frame, 8, 1 + sampleIndex / 48000);
        put32(frame, 12, quint32(sampleIndex % 48000) * 1000000ULL / 48000);
        frame += rawFrame(packets);
        frame += QByteArray::fromHex("0a0d");
        if (socket->write(frame) != frame.size()) wireValid = false;
    };
    connect(&stream, &QTimer::timeout, &server, [&]() {
        if (sampleIndex >= quint32(beams[0].size())) { stream.stop(); return; }
        QVector<QByteArray> packets;
        for (int pair = 0; pair < 100 && sampleIndex < quint32(beams[0].size()); ++pair)
        {
            std::array<std::array<float, 3>, 2> values {};
            for (int tuple = 0; tuple < 2; ++tuple)
                for (int beam = 0; beam < 3; ++beam) values[tuple][beam] = beams[beam][sampleIndex + tuple];
            packets.append(audioPacket(sampleIndex, activeConfig, 2, values));
            sampleIndex += 2;
        }
        if (sampleIndex % 3200 == 0) packets.append(statusPacket(activeConfig, transaction, 0x101));
        sendAudio(packets);
    });
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, &server, [&]() {
            incoming += socket->readAll();
            while (incoming.size() >= 10)
            {
                const quint16 type = qFromLittleEndian<quint16>(incoming.constData() + 2);
                if (type == 0xcc00) { incoming.remove(0, 10); continue; }
                if (incoming.size() < 26) return;
                const QByteArray frame = incoming.first(26);
                incoming.remove(0, 26);
                ++commands;
                if (qFromLittleEndian<quint16>(frame.constData()) != 0x7e7e || type != 0xcc11
                    || frame.mid(20, 4) != QByteArray(4, 0)
                    || qFromLittleEndian<quint16>(frame.constData() + 8) != 0x4e44
                    || qFromLittleEndian<quint16>(frame.constData() + 18) != PlDenoiseSession::crc16(frame.mid(8, 10)))
                { wireValid = false; return; }
                const quint16 txn = qFromLittleEndian<quint16>(frame.constData() + 10);
                const quint16 field = qFromLittleEndian<quint16>(frame.constData() + 12);
                if (field == 0x102) { sampleIndex = 0; activeConfig = 0; fieldMask = 0; }
                else if (field == 0x100) { if (txn != transaction) fieldMask = 0; transaction = txn; }
                else if (field < 6)
                {
                    if (txn != transaction) wireValid = false;
                    fieldMask |= 1U << field;
                    if (field == 2 && !droppedOneAck) { droppedOneAck = true; continue; }
                }
                else if (field == 0x101)
                {
                    if (txn != transaction || fieldMask != 0x3f) wireValid = false;
                    activeConfig = transaction;
                }
                else wireValid = false;
                lastField = field;
                sendAudio({statusPacket(activeConfig, txn, field, activeConfig ? 1 : 0)});
            }
        });
    });
    connect(&client, &DeviceClient::stateChanged, &session, [&](DeviceState state) {
        session.setAvailable(state == DeviceState::Connected, true);
    });
    connect(&session, &PlDenoiseSession::commandReady, &client, [&](const ControlValues &values) {
        transport.setControlTransactionActive(true);
        transport.setPendingConfiguration(quint16(values.channelValue(1)));
        wireValid = client.sendControl(values) && wireValid;
    });
    connect(&client, &DeviceClient::audioFrameReceived, &transport, [&](const HostFrame &frame) {
        transport.consume(frame.payload, frame.timestampSeconds, frame.timestampMicros);
    });
    connect(&transport, &PlDenoiseTransport::statusReceived, &session, &PlDenoiseSession::acknowledge);
    connect(&session, &PlDenoiseSession::configurationApplied, &transport, [&](quint16 config, bool restart) {
        worker.authorizePlConfiguration(config, restart);
        transport.setControlTransactionActive(false);
        transport.authorizeConfiguration(config, restart);
        stream.start();
    });
    connect(&transport, &PlDenoiseTransport::frameReady, &worker, &NoiseReductionWorker::processPlBeamFrame);
    session.configure(settings, 48000);
    client.connectToDevice("127.0.0.1", server.serverPort());
    QTRY_COMPARE_WITH_TIMEOUT(output[3].size(), 143360, 15000);
    QVERIFY(wireValid);
    QVERIFY(droppedOneAck);
    QVERIFY(commands >= 10); // RESET+BEGIN+6 fields+COMMIT, plus the lost ACK retry.
    QCOMPARE(lastField, quint16(0x101));
    QCOMPARE(workerErrors.size(), 0);
    QCOMPARE(transportErrors.size(), 0);
    QCOMPARE(sessionErrors.size(), 0);
    const std::array<QString, 4> names {"wiener", "post_bandpass", "post_logmmse", "post_agc"};
    for (int stage = 0; stage < 4; ++stage)
        QCOMPARE(output[stage], readFloats(directory + names[stage] + ".f32le"));
    stream.stop();
    client.disconnectFromDevice();
}

QTEST_GUILESS_MAIN(PlDenoiseTest)
#include "test_pldenoise.moc"
