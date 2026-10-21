#include <QDir>
#include <QtTest/QtTest>
#include "app/appsettings.h"
#include "control/pldenoisesession.h"
#include "media/noisereductionworker.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <bit>
#include <limits>

using namespace MicArray;
namespace
{
NoiseReductionSettings profile()
{
    NoiseReductionSettings value;
    value.enabled = true; value.monitorEnabled = false;
    value.inputMode = NoiseReductionInputMode::PlFullDenoisingFixedProfile;
    value.mode = NoiseReductionMode::TwoPoint; value.singleSuppressionCoefficient = 8.5;
    value.dualB1SuppressionCoefficient = 5; value.dualB2SuppressionCoefficient = 5;
    value.bandPass.enabled = true; value.bandPass.lowCutoffHz = 100; value.bandPass.highCutoffHz = 8000;
    value.logMmse.enabled = true; value.logMmse.fftSize = 2048; value.logMmse.overlapRatio = .5;
    value.logMmse.priorSnrSmoothing = .98; value.logMmse.noisePowerSmoothing = .95; value.logMmse.minimumGainDb = -20;
    value.automaticGainControl.enabled = true; value.automaticGainControl.targetLevelDbfs = -24;
    value.automaticGainControl.maximumGainDb = 20; value.automaticGainControl.maximumAttenuationDb = -20;
    value.automaticGainControl.attackMilliseconds = 20; value.automaticGainControl.releaseMilliseconds = 300;
    value.automaticGainControl.limiterLevelDbfs = -1;
    return value;
}
QByteArray checked(QByteArray packet)
{
    packet[40] = char(PlDenoiseTransport::crc8(packet.first(40)));
    return packet;
}
QByteArray dns(quint16 config = 10, quint16 stream = 10, int stage = 3,
               quint32 fingerprint = PlDenoiseTransport::FullProfileFingerprint)
{
    QByteArray packet(41, 0);
    packet.replace(0, 4, "DNS1"); packet[4] = 1; packet[5] = char(stage);
    qToLittleEndian(config, packet.data() + 6); qToLittleEndian(config, packet.data() + 8);
    qToLittleEndian(quint16(0x101), packet.data() + 10); packet[12] = config ? 1 : 0;
    qToLittleEndian(quint32(0x20261022), packet.data() + 25);
    qToLittleEndian(stream, packet.data() + 31);
    qToLittleEndian(stage == 3 ? fingerprint : quint32(3), packet.data() + 33);
    return checked(packet);
}
QByteArray finalPacket(quint32 index, quint16 stream = 10, int count = 6, float sample = .125f)
{
    QByteArray packet(41, 0);
    packet.replace(0, 4, "AUF1"); packet[4] = 1; packet[5] = 0x31;
    qToLittleEndian(stream, packet.data() + 6); qToLittleEndian(index, packet.data() + 8); packet[12] = char(count);
    for (int point = 0; point < std::min(6, count); ++point)
        qToLittleEndian(std::bit_cast<quint32>(sample), packet.data() + 13 + point * 4);
    return checked(packet);
}
QByteArray raw(const QVector<QByteArray> &packets)
{
    QByteArray value(38400, 0);
    for (int point = 0; point < packets.size(); ++point)
        for (int byte = 0; byte < 41; ++byte) value[point * 192 + (byte + 7) * 4 + 3] = packets[point][byte];
    return value;
}
QByteArray final200(quint32 first, quint16 stream = 10)
{
    QVector<QByteArray> packets;
    for (int point = 0; point < 200; point += 6) packets.append(finalPacket(first + point, stream, std::min(6, 200 - point)));
    return raw(packets);
}
void authorize(PlDenoiseTransport &transport)
{
    transport.setExpectedStage(3);
    transport.consume(raw({dns()}), 1, 0);
    transport.authorizeConfiguration(10, true);
}
QVector<float> readFloats(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.readAll();
    QVector<float> value(bytes.size() / 4);
    for (int index = 0; index < value.size(); ++index)
        value[index] = std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.constData() + index * 4));
    return value;
}
}

class PlFullTest : public QObject
{
    Q_OBJECT
private slots:
    void capabilityMatchesFrozenManifest();
    void incompatibleSettingIsRejected_data();
    void incompatibleSettingIsRejected();
    void compatibleSettingsAndModeAreSavedWithoutReplacement();
    void fingerprintIsRequiredAndNewBuildSupportsAllStages();
    void sessionUsesModeTwoAndPreservesStreamForDirections();
    void buffersEarlyFinalWithoutRunningPostTwice();
    void malformedFinalRejected_data();
    void malformedFinalRejected();
    void finalWorkerGoldenIsBitExact_data();
    void finalWorkerGoldenIsBitExact();
    void workerDoesNotInventIntermediateRecordings();
};

void PlFullTest::capabilityMatchesFrozenManifest()
{
    const QString path = QString::fromUtf8(MICARRAY_FULL_PROFILE_MANIFEST);
    QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
    QVERIFY(manifest["passed"].toBool());
    QCOMPARE(manifest["fingerprint_crc32_hex"].toString(), QString::number(PlDenoiseTransport::FullProfileFingerprint, 16));
    QCOMPARE(manifest["build_id_hex"].toString(), QStringLiteral("20261022"));
    QCOMPARE(manifest["audio_magic"].toString(), QStringLiteral("AUF1"));
    QCOMPARE(manifest["audio_flags_hex"].toString(), QStringLiteral("31"));
    QFile canonical(QFileInfo(path).dir().filePath("canonical_profile.json"));
    QVERIFY(canonical.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(canonical.readAll(), QCryptographicHash::Sha256).toHex()),
        manifest["canonical_sha256"].toString());
}

void PlFullTest::incompatibleSettingIsRejected_data()
{
    QTest::addColumn<int>("setting");
    const QStringList labels {"adaptive", "BP-disabled", "BP-low", "BP-high", "Log-disabled", "Log-size", "Log-overlap",
        "Log-prior", "Log-noise", "Log-minGain", "AGC-disabled", "AGC-target", "AGC-max", "AGC-min", "AGC-attack",
        "AGC-release", "AGC-limit", "post-order"};
    for (int index = 0; index < labels.size(); ++index) QTest::newRow(qPrintable(labels[index])) << index;
}
void PlFullTest::incompatibleSettingIsRejected()
{
    QFETCH(int, setting);
    auto value = profile();
    switch (setting)
    {
    case 0: value.algorithm = NoiseReductionAlgorithm::AdaptiveWiener; break;
    case 1: value.bandPass.enabled = false; break;
    case 2: value.bandPass.lowCutoffHz = 101; break;
    case 3: value.bandPass.highCutoffHz = 7999; break;
    case 4: value.logMmse.enabled = false; break;
    case 5: value.logMmse.fftSize = 512; break;
    case 6: value.logMmse.overlapRatio = .25; break;
    case 7: value.logMmse.priorSnrSmoothing = .97; break;
    case 8: value.logMmse.noisePowerSmoothing = .94; break;
    case 9: value.logMmse.minimumGainDb = -19; break;
    case 10: value.automaticGainControl.enabled = false; break;
    case 11: value.automaticGainControl.targetLevelDbfs = -23; break;
    case 12: value.automaticGainControl.maximumGainDb = 19; break;
    case 13: value.automaticGainControl.maximumAttenuationDb = -19; break;
    case 14: value.automaticGainControl.attackMilliseconds = 21; break;
    case 15: value.automaticGainControl.releaseMilliseconds = 301; break;
    case 16: value.automaticGainControl.limiterLevelDbfs = -2; break;
    case 17: std::swap(value.postProcessingOrder[0], value.postProcessingOrder[1]); break;
    }
    const auto before = value;
    QVERIFY(!plFullDenoiseProfileError(value).isEmpty());
    PlDenoiseSession session;
    QSignalSpy commands(&session, &PlDenoiseSession::commandReady), faults(&session, &PlDenoiseSession::failed);
    session.configure(value, 48000); session.setAvailable(true, true);
    QCOMPARE(commands.size(), 0); QCOMPARE(faults.size(), 1); QVERIFY(!session.ready());
    QCOMPARE(value, before);
}
void PlFullTest::compatibleSettingsAndModeAreSavedWithoutReplacement()
{
    auto value = profile();
    QVERIFY(plFullDenoiseProfileError(value).isEmpty());
    value.mode = NoiseReductionMode::SinglePoint; value.singleSuppressionCoefficient = 3.125;
    value.enhancedA.azimuthDegrees = 45; value.monitorGainDb = 39;
    QVERIFY(plFullDenoiseProfileError(value).isEmpty());
    QCOMPARE(PlDenoiseSession::configurationTuple(value)[6], 2);
    QCOMPARE(PlDenoiseSession::configurationTuple(value)[7], qint32(3.125 * 65536));
    QCOMPARE(PlDenoiseSession::configurationTuple(value)[8], 0);
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    value.logMmse.fftSize = 512; // User-incompatible value must remain visible, never auto-replaced.
    store.saveNoiseReduction(value);
    const auto restored = store.noiseReduction();
    QCOMPARE(restored.inputMode, NoiseReductionInputMode::PlFullDenoisingFixedProfile);
    QCOMPARE(restored.logMmse.fftSize, 512); QCOMPARE(restored.monitorGainDb, 39.0);
    QCOMPARE(restored.bandPass, value.bandPass); QCOMPARE(restored.automaticGainControl, value.automaticGainControl);
}
void PlFullTest::fingerprintIsRequiredAndNewBuildSupportsAllStages()
{
    for (int stage : {1, 2, 3})
    {
        PlDenoiseTransport transport;
        transport.setExpectedStage(stage);
        QSignalSpy status(&transport, &PlDenoiseTransport::statusReceived), faults(&transport, &PlDenoiseTransport::faultRaised);
        transport.consume(raw({dns(10, 10, stage)}), 1, 0);
        QCOMPARE(status.size(), 1); QCOMPARE(faults.size(), 0);
        QCOMPARE(qvariant_cast<PlDenoiseStatus>(status[0][0]).stage, quint8(stage));
    }
    PlDenoiseTransport transport;
    transport.setExpectedStage(3);
    QSignalSpy status(&transport, &PlDenoiseTransport::statusReceived), faults(&transport, &PlDenoiseTransport::faultRaised);
    transport.consume(raw({dns(10, 10, 3, 0xb2c8f064)}), 1, 0);
    QCOMPARE(status.size(), 0); QCOMPARE(faults.size(), 1); QVERIFY(transport.faulted());
}
void PlFullTest::sessionUsesModeTwoAndPreservesStreamForDirections()
{
    auto settings = profile();
    PlDenoiseSession session;
    QVector<quint16> commands;
    quint16 active = 0, stream = 0;
    connect(&session, &PlDenoiseSession::commandReady, &session, [&](const ControlValues &command)
    {
        const auto field = quint16(command.channelValue(2)), transaction = quint16(command.channelValue(1));
        commands.append(field);
        if (field == 0x102) { active = 0; stream = 0; }
        if (field == 6) QCOMPARE(quint16(command.channelValue(3)), quint16(2));
        PlDenoiseStatus status;
        status.buildId = 0x20261022; status.ackTransaction = transaction; status.ackField = field;
        status.stage = 1;
        if (field == 0x101)
        {
            active = transaction; if (!stream) stream = transaction;
            status.flags = 1; status.activeConfigId = active; status.streamId = stream; status.stage = 3;
            session.acknowledge(status); QVERIFY(!session.ready() || session.activeConfigId() != transaction);
            status.profileFingerprint = PlDenoiseTransport::FullProfileFingerprint;
        }
        session.acknowledge(status);
    });
    session.configure(settings, 48000); session.setAvailable(true, true);
    QCOMPARE(commands.size(), 12); QCOMPARE(commands.front(), quint16(0x102)); QVERIFY(session.ready());
    const auto firstStream = stream;
    commands.clear(); settings.enhancedA.azimuthDegrees = 12.3; session.configure(settings, 48000);
    QCOMPARE(commands.size(), 11); QVERIFY(!commands.contains(0x102)); QCOMPARE(stream, firstStream);
    commands.clear(); settings.dualB1SuppressionCoefficient = 6; session.configure(settings, 48000);
    QCOMPARE(commands.size(), 12); QCOMPARE(commands.front(), quint16(0x102)); QVERIFY(stream != firstStream);
    commands.clear(); settings.monitorGainDb = 39; session.configure(settings, 48000); QCOMPARE(commands.size(), 0);
}
void PlFullTest::buffersEarlyFinalWithoutRunningPostTwice()
{
    PlDenoiseTransport transport;
    transport.setExpectedStage(3); transport.setPendingConfiguration(10);
    QSignalSpy final(&transport, &PlDenoiseTransport::finalFrameReady), wiener(&transport, &PlDenoiseTransport::wienerFrameReady);
    transport.consume(raw({dns(0, 0, 1)}), 1, 0);
    transport.consume(final200(0), 1, 0);
    QCOMPARE(final.size(), 0);
    transport.consume(raw({dns()}), 1, 0); transport.authorizeConfiguration(10, true);
    QCOMPARE(final.size(), 1); QCOMPARE(wiener.size(), 0);
    const auto frame = qvariant_cast<PlFinalFrame>(final[0][0]);
    QCOMPARE(frame.samples, QVector<float>(200, .125f)); QCOMPARE(frame.firstSampleIndex, quint32(0));
    transport.consume(raw({dns(11, 10)}), 1, 100); transport.authorizeConfiguration(11, false);
    transport.consume(final200(200), 1, 200);
    QCOMPARE(final.size(), 2); QCOMPARE(qvariant_cast<PlFinalFrame>(final[1][0]).firstSampleIndex, quint32(200));
}
void PlFullTest::malformedFinalRejected_data()
{
    QTest::addColumn<int>("kind");
    for (int kind = 0; kind < 8; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
}
void PlFullTest::malformedFinalRejected()
{
    QFETCH(int, kind);
    PlDenoiseTransport transport; authorize(transport);
    auto packet = finalPacket(0);
    switch (kind)
    {
    case 0: packet[40] ^= 1; break;
    case 1: packet[4] = 2; packet = checked(packet); break;
    case 2: packet[5] = 0x21; packet = checked(packet); break;
    case 3: packet[37] = 1; packet = checked(packet); break;
    case 4: packet = finalPacket(0, 10, 6, std::numeric_limits<float>::infinity()); break;
    case 5: packet = finalPacket(1); break;
    case 6: packet.replace(0, 4, "AUW1"); packet[5] = 0x21; packet = checked(packet); break;
    case 7: packet[12] = 0; packet = checked(packet); break;
    }
    transport.consume(raw({packet}), 1, 0);
    QVERIFY(transport.faulted());
}
void PlFullTest::finalWorkerGoldenIsBitExact_data()
{
    QTest::addColumn<QString>("name");
    for (const QString name : {"background", "A_only", "B_only", "A_and_B"}) QTest::newRow(qPrintable(name)) << name;
}
void PlFullTest::finalWorkerGoldenIsBitExact()
{
    if (!QDir(QString::fromUtf8(MICARRAY_DENOISE_GOLDEN_ROOT)).exists())
        QSKIP("Private field-audio golden fixtures are not distributed; set MICARRAY_DENOISE_GOLDEN_ROOT to run this check.");
    QFETCH(QString, name);
    const auto expected = readFloats(QString::fromUtf8(MICARRAY_DENOISE_GOLDEN_ROOT) + "/" + name + "/reference/final.f32le");
    QCOMPARE(expected.size(), 143360);
    auto settings = profile(); settings.monitorGainDb = 39;
    NoiseReductionWorker worker;
    worker.configure(settings, 48000); worker.setActive(true);
    worker.setRecordingStages({NoiseReductionDataFlowStage::AutomaticGainControl}); worker.setDiagnosticStageCapture(true);
    worker.authorizePlConfiguration(10, true);
    QVector<float> actual;
    connect(&worker, &NoiseReductionWorker::diagnosticStageFrameReady, &worker, [&](const NoiseReductionRecordingFrame &frame)
    {
        QCOMPARE(frame.stages.size(), 1); QCOMPARE(frame.stages[0].stage, NoiseReductionDataFlowStage::AutomaticGainControl);
        actual += frame.stages[0].samples;
    });
    for (int first = 0; first < expected.size(); first += 200)
    {
        PlFinalFrame frame;
        frame.samples = expected.mid(first, std::min(200, int(expected.size()) - first)); frame.streamId = 10;
        frame.firstSampleIndex = first; frame.timestampSeconds = 1 + first / 48000; frame.timestampMicros = first % 48000 * 1000000ULL / 48000;
        worker.processPlFinalFrame(frame);
    }
    QCOMPARE(actual.size(), expected.size());
    for (int index = 0; index < actual.size(); ++index)
        QCOMPARE(std::bit_cast<quint32>(actual[index]), std::bit_cast<quint32>(expected[index]));
}
void PlFullTest::workerDoesNotInventIntermediateRecordings()
{
    NoiseReductionWorker worker;
    worker.configure(profile(), 48000); worker.setActive(true); worker.authorizePlConfiguration(10, true);
    QSignalSpy records(&worker, &NoiseReductionWorker::recordingFrameReady), warnings(&worker, &NoiseReductionWorker::warningRaised);
    worker.setRecordingStages({NoiseReductionDataFlowStage::Suppression, NoiseReductionDataFlowStage::AutomaticGainControl});
    QCOMPARE(warnings.size(), 1);
    worker.processPlFinalFrame({QVector<float>(200, .125f), 0, 10, 1, 0});
    QCOMPARE(records.size(), 0);
    auto incompatible = profile(); incompatible.automaticGainControl.targetLevelDbfs = -23;
    worker.configure(incompatible, 48000); worker.authorizePlConfiguration(11, true);
    worker.processPlFinalFrame({QVector<float>(200, .125f), 0, 11, 1, 0});
    QVERIFY(warnings.size() >= 2); QCOMPARE(records.size(), 0);
}
QTEST_GUILESS_MAIN(PlFullTest)
#include "test_plfull.moc"
