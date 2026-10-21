#include <QDir>
#include <QtTest/QtTest>
#include "control/pldenoisesession.h"
#include "media/noisereductionworker.h"
#include "media/pldenoisetransport.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtEndian>
#include <bit>
#include <limits>

using namespace MicArray;
namespace {
QByteArray checked(QByteArray packet)
{
    packet[40] = char(PlDenoiseTransport::crc8(packet.first(40)));
    return packet;
}
QByteArray dns(quint16 config, quint16 stream, quint16 transaction = 0, quint16 field = 0, quint8 stage = 2)
{
    QByteArray packet(41, 0);
    packet.replace(0, 4, "DNS1"); packet[4] = 1; packet[5] = char(stage);
    qToLittleEndian(config, packet.data() + 6);
    qToLittleEndian(transaction, packet.data() + 8);
    qToLittleEndian(field, packet.data() + 10);
    packet[12] = config ? 1 : 0;
    qToLittleEndian(quint32(0x20261021), packet.data() + 25);
    qToLittleEndian(stream, packet.data() + 31);
    qToLittleEndian(quint32(23), packet.data() + 33);
    return checked(packet);
}
QByteArray auw(quint32 index, quint16 stream, const QVector<float> &samples)
{
    QByteArray packet(41, 0);
    packet.replace(0, 4, "AUW1"); packet[4] = 1; packet[5] = 0x21;
    qToLittleEndian(stream, packet.data() + 6);
    qToLittleEndian(index, packet.data() + 8);
    packet[12] = char(samples.size());
    for (int sample = 0; sample < std::min(6, int(samples.size())); ++sample)
        qToLittleEndian(std::bit_cast<quint32>(samples[sample]), packet.data() + 13 + sample * 4);
    return checked(packet);
}
QByteArray raw(const QVector<QByteArray> &packets)
{
    QByteArray payload(38400, 0);
    for (int point = 0; point < packets.size(); ++point)
        for (int byte = 0; byte < 41; ++byte) payload[point * 192 + (byte + 7) * 4 + 3] = packets[point][byte];
    return payload;
}
void initialize(PlDenoiseTransport &transport, quint16 config = 10, quint16 stream = 10)
{
    transport.setExpectedStage(2);
    transport.consume(raw({dns(config, stream)}), 1, 0);
    transport.authorizeConfiguration(config, true);
}
QVector<float> readFloats(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.readAll();
    QVector<float> samples(bytes.size() / 4);
    for (int index = 0; index < samples.size(); ++index)
        samples[index] = std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.constData() + index * 4));
    return samples;
}
NoiseReductionSettings frozenSettings()
{
    NoiseReductionSettings settings;
    settings.enabled = true; settings.monitorEnabled = false;
    settings.inputMode = NoiseReductionInputMode::PlFixedWienerOriginalPostProcessing;
    settings.algorithm = NoiseReductionAlgorithm::FixedParameterWiener;
    settings.mode = NoiseReductionMode::TwoPoint;
    settings.singleSuppressionCoefficient = 8.5;
    settings.dualB1SuppressionCoefficient = 5; settings.dualB2SuppressionCoefficient = 5;
    settings.bandPass.enabled = true; settings.bandPass.lowCutoffHz = 100; settings.bandPass.highCutoffHz = 8000;
    settings.logMmse.enabled = true; settings.logMmse.fftSize = 2048;
    settings.logMmse.overlapRatio = 0.5; settings.logMmse.priorSnrSmoothing = 0.98;
    settings.logMmse.noisePowerSmoothing = 0.95; settings.logMmse.minimumGainDb = -20;
    settings.automaticGainControl.enabled = true;
    settings.automaticGainControl.targetLevelDbfs = -24;
    settings.automaticGainControl.maximumGainDb = 20;
    settings.automaticGainControl.maximumAttenuationDb = -20;
    settings.automaticGainControl.attackMilliseconds = 20;
    settings.automaticGainControl.releaseMilliseconds = 300;
    settings.automaticGainControl.limiterLevelDbfs = -1;
    return settings;
}
} // namespace

class PlWienerTest : public QObject
{
    Q_OBJECT
private slots:
    void tupleMapsSingleAndDualCoefficients();
    void requiresNineFieldsAndStreamCommit();
    void directionRetainsStreamButDspSettingsReset();
    void adaptiveSelectionIsExplicitlyRejected();
    void acceptsResetStageTransitionAndBuffersEarlyAudio();
    void directionConfigChangesDoNotChangeStreamTimeline();
    void malformedAudioIsRejected_data();
    void malformedAudioIsRejected();
    void oldStreamIsDiscardedWithoutInventingSamples();
    void originalWienerThroughWireMatchesOriginalPost_data();
    void originalWienerThroughWireMatchesOriginalPost();
};

void PlWienerTest::tupleMapsSingleAndDualCoefficients()
{
    auto settings = frozenSettings();
    auto tuple = PlDenoiseSession::configurationTuple(settings);
    QCOMPARE(tuple[6], 1); QCOMPARE(tuple[7], 5 * 65536); QCOMPARE(tuple[8], 5 * 65536);
    settings.mode = NoiseReductionMode::SinglePoint; settings.singleSuppressionCoefficient = 3.125;
    tuple = PlDenoiseSession::configurationTuple(settings);
    QCOMPARE(tuple[7], qint32(3.125 * 65536)); QCOMPARE(tuple[8], 0);
}

void PlWienerTest::requiresNineFieldsAndStreamCommit()
{
    PlDenoiseSession session;
    QSignalSpy commands(&session, &PlDenoiseSession::commandReady);
    QSignalSpy applied(&session, &PlDenoiseSession::configurationApplied);
    session.configure(frozenSettings(), 48000); session.setAvailable(true, true);
    const QVector<quint16> fields {0x102, 0x100, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0x101};
    for (int index = 0; index < fields.size(); ++index) {
        QCOMPARE(commands.size(), index + 1);
        const auto command = qvariant_cast<ControlValues>(commands.last()[0]);
        QCOMPARE(quint16(command.channelValue(2)), fields[index]);
        PlDenoiseStatus status;
        status.ackTransaction = quint16(command.channelValue(1)); status.ackField = fields[index];
        status.stage = 1; // RESET / partially staged mode may remain stage1.
        if (fields[index] == 0x101) {
            status.flags = 1; status.activeConfigId = status.ackTransaction;
            session.acknowledge(status); QVERIFY(!session.ready());
            status.stage = 2; status.streamId = status.ackTransaction;
        }
        session.acknowledge(status);
    }
    QVERIFY(session.ready()); QCOMPARE(applied.size(), 1);
    QCOMPARE(applied[0][1].toBool(), true);
}

void PlWienerTest::directionRetainsStreamButDspSettingsReset()
{
    for (int mutation = 0; mutation < 9; ++mutation) {
        PlDenoiseSession session;
        QVector<quint16> fields;
        quint16 active = 0, stream = 0;
        connect(&session, &PlDenoiseSession::commandReady, &session, [&](const ControlValues &values) {
            const auto field = quint16(values.channelValue(2)); fields.append(field);
            if (field == 0x102) { stream = 0; active = 0; }
            PlDenoiseStatus status;
            status.ackField = field; status.ackTransaction = quint16(values.channelValue(1));
            if (field == 0x101) { active = status.ackTransaction; if (!stream) stream = active; }
            status.stage = active ? 2 : 1; status.activeConfigId = active;
            status.streamId = stream; status.flags = active ? 1 : 0;
            session.acknowledge(status);
        });
        auto settings = frozenSettings();
        session.configure(settings, 48000); session.setAvailable(true, true);
        const auto oldStream = stream; fields.clear();
        if (mutation == 0) settings.enhancedA.azimuthDegrees = 12.345;
        else if (mutation == 1) settings.dualB1SuppressionCoefficient = 6;
        else if (mutation == 2) settings.mode = NoiseReductionMode::SinglePoint;
        else if (mutation == 3) settings.bandPass.lowCutoffHz = 120;
        else if (mutation == 4) settings.logMmse.fftSize = 1024;
        else if (mutation == 5) settings.automaticGainControl.maximumGainDb = 10;
        else if (mutation == 6) std::swap(settings.postProcessingOrder[0], settings.postProcessingOrder[2]);
        else if (mutation == 7) settings.monitorGainDb = 20;
        else settings.firstPlotSource = NoiseSignalSource::FirstElement;
        session.configure(settings, 48000);
        if (mutation == 0) { QCOMPARE(fields.size(), 11); QVERIFY(!fields.contains(0x102)); QCOMPARE(stream, oldStream); }
        else if (mutation == 7 || mutation == 8) { QCOMPARE(fields.size(), 0); QCOMPARE(stream, oldStream); }
        else { QCOMPARE(fields.size(), 12); QCOMPARE(fields[0], quint16(0x102)); QVERIFY(stream != oldStream); }
        QVERIFY(session.ready());
    }
}

void PlWienerTest::adaptiveSelectionIsExplicitlyRejected()
{
    PlDenoiseSession session;
    QSignalSpy errors(&session, &PlDenoiseSession::failed);
    QSignalSpy commands(&session, &PlDenoiseSession::commandReady);
    auto settings = frozenSettings(); settings.algorithm = NoiseReductionAlgorithm::AdaptiveWiener;
    session.configure(settings, 48000); session.setAvailable(true, true);
    QCOMPARE(errors.size(), 1); QCOMPARE(commands.size(), 0); QVERIFY(!session.ready());
}

void PlWienerTest::acceptsResetStageTransitionAndBuffersEarlyAudio()
{
    PlDenoiseTransport transport;
    transport.setExpectedStage(2);
    transport.setControlTransactionActive(true); transport.setPendingConfiguration(22);
    QSignalSpy output(&transport, &PlDenoiseTransport::wienerFrameReady);
    transport.consume(raw({dns(0, 0, 22, 0x102, 1)}), 1, 0);
    QVERIFY(!transport.faulted());
    QVector<QByteArray> packets;
    for (int index = 0; index < 100; ++index) packets.append(auw(index * 6, 22, QVector<float>(6, 0.1f)));
    transport.consume(raw(packets), 1, 10000);
    QCOMPARE(output.size(), 0);
    transport.consume(raw({dns(22, 22, 22, 0x101, 2)}), 1, 20000);
    transport.authorizeConfiguration(22, true);
    QVERIFY(!transport.faulted()); QCOMPARE(output.size(), 3);
    QCOMPARE(qvariant_cast<PlWienerFrame>(output[0][0]).firstSampleIndex, quint32(0));
    QCOMPARE(qvariant_cast<PlWienerFrame>(output[2][0]).firstSampleIndex, quint32(400));
}

void PlWienerTest::directionConfigChangesDoNotChangeStreamTimeline()
{
    PlDenoiseTransport transport; initialize(transport);
    QSignalSpy output(&transport, &PlDenoiseTransport::wienerFrameReady);
    QVector<QByteArray> packets;
    for (int index = 0; index < 100; ++index) packets.append(auw(index * 2, 10, {0.1f, 0.2f}));
    transport.consume(raw(packets), 1, 0);
    transport.consume(raw({dns(11, 10, 11, 0x101)}), 1, 10000);
    transport.authorizeConfiguration(11, false);
    packets.clear();
    for (int index = 0; index < 100; ++index) packets.append(auw(200 + index * 2, 10, {0.3f, 0.4f}));
    transport.consume(raw(packets), 1, 20000);
    QVERIFY(!transport.faulted()); QCOMPARE(output.size(), 2);
    QCOMPARE(qvariant_cast<PlWienerFrame>(output[1][0]).streamId, quint16(10));
    QCOMPARE(qvariant_cast<PlWienerFrame>(output[1][0]).firstSampleIndex, quint32(200));
}

void PlWienerTest::malformedAudioIsRejected_data()
{
    QTest::addColumn<QByteArray>("packet");
    auto packet = auw(0, 10, {0.1f}); packet[40] ^= 1; QTest::newRow("crc") << packet;
    packet = auw(0, 10, {0.1f}); packet[4] = 2; QTest::newRow("version") << checked(packet);
    packet = auw(0, 10, {0.1f}); packet[5] = 0x11; QTest::newRow("stage") << checked(packet);
    packet = auw(0, 10, {0.1f}); packet[12] = 7; QTest::newRow("count") << checked(packet);
    packet = auw(0, 10, {0.1f}); packet[39] = 1; QTest::newRow("reserved") << checked(packet);
    packet = auw(0, 10, {std::numeric_limits<float>::infinity()}); QTest::newRow("nonfinite") << packet;
    packet = auw(1, 10, {0.1f}); QTest::newRow("missing-index-zero") << packet;
}

void PlWienerTest::malformedAudioIsRejected()
{
    QFETCH(QByteArray, packet);
    PlDenoiseTransport transport; initialize(transport);
    transport.consume(raw({packet}), 1, 0);
    QVERIFY(transport.faulted());
}

void PlWienerTest::oldStreamIsDiscardedWithoutInventingSamples()
{
    PlDenoiseTransport transport; initialize(transport, 11, 11);
    QSignalSpy stale(&transport, &PlDenoiseTransport::staleStreamDiscarded);
    transport.consume(raw({auw(300, 10, {0.1f}), auw(0, 11, {0.2f})}), 1, 0);
    QVERIFY(!transport.faulted()); QCOMPARE(stale.size(), 1); QCOMPARE(transport.receivedSamples(), quint64(1));
}

void PlWienerTest::originalWienerThroughWireMatchesOriginalPost_data()
{
    QTest::addColumn<QString>("caseName");
    for (const auto &name : {QString("background"), QString("A_only"), QString("B_only"), QString("A_and_B")})
        QTest::newRow(name.toLatin1().constData()) << name;
}

void PlWienerTest::originalWienerThroughWireMatchesOriginalPost()
{
    if (!QDir(QString::fromUtf8(MICARRAY_DENOISE_GOLDEN_ROOT)).exists())
        QSKIP("Private field-audio golden fixtures are not distributed; set MICARRAY_DENOISE_GOLDEN_ROOT to run this check.");
    QFETCH(QString, caseName);
    const QString directory = QString::fromUtf8(MICARRAY_DENOISE_GOLDEN_ROOT) + "/" + caseName + "/reference/";
    const auto wiener = readFloats(directory + "wiener.f32le"); QCOMPARE(wiener.size(), 143360);
    auto settings = frozenSettings();
    NoiseReductionWorker worker; worker.configure(settings, 48000); worker.setActive(true);
    worker.setRecordingStages({NoiseReductionDataFlowStage::Suppression, NoiseReductionDataFlowStage::BandPass,
        NoiseReductionDataFlowStage::LogMmse, NoiseReductionDataFlowStage::AutomaticGainControl});
    worker.setDiagnosticStageCapture(true); worker.authorizePlConfiguration(10, true);
    PlDenoiseTransport transport; initialize(transport);
    connect(&transport, &PlDenoiseTransport::wienerFrameReady, &worker, &NoiseReductionWorker::processPlWienerFrame);
    std::array<QVector<float>, 4> output;
    connect(&worker, &NoiseReductionWorker::diagnosticStageFrameReady, &worker,
        [&](const NoiseReductionRecordingFrame &frame) {
            for (int stage = 0; stage < 4; ++stage) output[stage] += frame.stages[stage].samples;
        });
    QSignalSpy errors(&worker, &NoiseReductionWorker::warningRaised);
    QVector<QByteArray> packets;
    for (int index = 0; index < wiener.size();) {
        const int count = std::min(6, int(wiener.size()) - index);
        packets.append(auw(index, 10, wiener.mid(index, count))); index += count;
        if (packets.size() == 200) { transport.consume(raw(packets), 1, index * 1000000ULL / 48000); packets.clear(); }
    }
    if (!packets.isEmpty()) transport.consume(raw(packets), 1, 0);
    // Transport deliberately keeps the final incomplete 200-sample packet; feed its real tail directly, no padding.
    const int delivered = int(output[0].size());
    if (delivered < wiener.size()) {
        PlWienerFrame tail; tail.samples = wiener.mid(delivered); tail.firstSampleIndex = delivered;
        tail.streamId = 10; tail.timestampSeconds = 4; worker.processPlWienerFrame(tail);
    }
    QVERIFY(!transport.faulted()); QCOMPARE(errors.size(), 0);
    const std::array<QString, 4> stages {"wiener", "post_bandpass", "post_logmmse", "post_agc"};
    for (int stage = 0; stage < 4; ++stage) QCOMPARE(output[stage], readFloats(directory + stages[stage] + ".f32le"));
}

QTEST_GUILESS_MAIN(PlWienerTest)
#include "test_plwiener.moc"
