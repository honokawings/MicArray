#include <QtTest/QtTest>

#include "app/appsettings.h"
#include "media/pldenoisetransport.h"
#include "media/wienernoisereducer.h"
#include "ui/noisereductionpage.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QLabel>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QtEndian>

#include <bit>

using namespace MicArray;

namespace {
QByteArray rawFrame(const QVector<QByteArray> &packets)
{
    QByteArray raw(38400, 0);
    for (int index = 0; index < packets.size(); ++index)
        for (int byte = 0; byte < 41; ++byte) raw[index * 192 + (byte + 7) * 4 + 3] = packets[index][byte];
    return raw;
}
QByteArray statusPacket(quint16 active, quint16 transaction, quint16 field)
{
    QByteArray packet(41, 0);
    packet.replace(0, 4, "DNS1");
    packet[4] = 1; packet[5] = 1;
    qToLittleEndian(active, packet.data() + 6);
    qToLittleEndian(transaction, packet.data() + 8);
    qToLittleEndian(field, packet.data() + 10);
    packet[12] = active ? 1 : 0;
    qToLittleEndian(quint32(0x20261020), packet.data() + 25);
    packet[40] = char(PlDenoiseTransport::crc8(packet.first(40)));
    return packet;
}
QByteArray audioPacket(quint32 sample, quint16 config)
{
    QByteArray packet(41, 0);
    packet.replace(0, 4, "AUD3"); packet[4] = 1; packet[5] = 0x11;
    qToLittleEndian(config, packet.data() + 6);
    qToLittleEndian(sample, packet.data() + 8); packet[12] = 2;
    for (int tuple = 0; tuple < 2; ++tuple)
        for (int beam = 0; beam < 3; ++beam)
            qToLittleEndian(std::bit_cast<quint32>(beam == 0 ? 0.1f : 0.0f), packet.data() + 13 + tuple * 12 + beam * 4);
    packet[40] = char(PlDenoiseTransport::crc8(packet.first(40)));
    return packet;
}
QByteArray wienerStatus(quint16 active, quint16 stream, quint16 transaction, quint16 field)
{
    QByteArray packet = statusPacket(active, transaction, field);
    packet[5] = active ? 2 : 1; // RESET transition intentionally still stage1.
    qToLittleEndian(quint32(0x20261021), packet.data() + 25);
    qToLittleEndian(stream, packet.data() + 31);
    packet[40] = char(PlDenoiseTransport::crc8(packet.first(40)));
    return packet;
}
QByteArray wienerPacket(quint32 first, quint16 stream, int count)
{
    QByteArray packet(41, 0);
    packet.replace(0, 4, "AUW1"); packet[4] = 1; packet[5] = 0x21;
    qToLittleEndian(stream, packet.data() + 6);
    qToLittleEndian(first, packet.data() + 8); packet[12] = char(count);
    for (int index = 0; index < count; ++index)
        qToLittleEndian(std::bit_cast<quint32>(0.125f), packet.data() + 13 + index * 4);
    packet[40] = char(PlDenoiseTransport::crc8(packet.first(40)));
    return packet;
}
QVector<QByteArray> wienerPackets(quint32 first, quint16 stream, int count)
{
    QVector<QByteArray> packets;
    for (int offset = 0; offset < count; offset += 6)
        packets.append(wienerPacket(first + offset, stream, std::min(6, count - offset)));
    return packets;
}
} // namespace

class PlDenoiseUiTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        const int font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
        QVERIFY(font >= 0);
        QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font).first(), 10));
    }
    void explicitModeWaitsWithoutFakeRawBeams();
    void guiHandshakeAndDirectionUpdateKeepDspHistory();
    void fixedWienerRetryAndParameterResetUseMonoPostOnly();
    void fixedWienerRejectsAdaptiveAndLabelsUnavailableBeams();
    void fullModeDisplaysFinalAndRejectsChangedPostProfile();
    void fullModeRejectsWrongCapabilityFingerprint();
};

void PlDenoiseUiTest::explicitModeWaitsWithoutFakeRawBeams()
{
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    auto settings = store.noiseReduction();
    settings.inputMode = NoiseReductionInputMode::PlThreeBeamOriginalDenoising;
    settings.enabled = true;
    settings.monitorEnabled = false;
    store.saveNoiseReduction(settings);
    NoiseReductionPage page(&store);
    page.setActive(true);
    page.setRecordingRequired(true, {NoiseReductionDataFlowStage::Suppression});
    auto *mode = page.findChild<QComboBox *>("noiseInputModeCombo");
    auto *status = page.findChild<QLabel *>("noiseReductionStatusLabel");
    QVERIFY(mode); QVERIFY(status);
    QCOMPARE(mode->currentData().toInt(), int(NoiseReductionInputMode::PlThreeBeamOriginalDenoising));
    QVERIFY(mode->currentText().contains(QStringLiteral("上位机原降噪")));
    QSignalSpy recordings(&page, &NoiseReductionPage::noiseRecordingFrameReady);
    for (int frame = 0; frame < 10; ++frame) page.submitRawAudioFrame(QByteArray(38400, 0), 1, frame * 4167);
    QTest::qWait(100);
    QCOMPARE(recordings.size(), 0);
    QVERIFY(status->text().contains(QStringLiteral("等待设备连接")));
}

void PlDenoiseUiTest::guiHandshakeAndDirectionUpdateKeepDspHistory()
{
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    auto settings = store.noiseReduction();
    settings.inputMode = NoiseReductionInputMode::PlThreeBeamOriginalDenoising;
    settings.enabled = true;
    settings.monitorEnabled = false;
    settings.singleSuppressionCoefficient = 0;
    store.saveNoiseReduction(settings);
    NoiseReductionPage page(&store);
    page.resize(1350, 900);
    page.setActive(true);
    page.setRecordingRequired(true, {NoiseReductionDataFlowStage::Suppression});
    quint16 active = 0;
    QVector<quint16> commands;
    connect(&page, &NoiseReductionPage::controlSendRequested, &page, [&](const ControlValues &values) {
        const quint16 transaction = quint16(values.channelValue(1));
        const quint16 field = quint16(values.channelValue(2));
        commands.append(field);
        if (field == 0x102) active = 0;
        if (field == 0x101) active = transaction;
        page.submitPlSidebandFrame(rawFrame({statusPacket(active, transaction, field)}), 1, 0);
    });
    QSignalSpy recordings(&page, &NoiseReductionPage::noiseRecordingFrameReady);
    page.setControlLocked(false);
    page.setConnected(true);
    QCOMPARE(commands.size(), 9);
    QCOMPARE(commands[0], quint16(0x102));
    QVERIFY(active != 0);
    auto feed = [&](int first, int count) {
        for (int input = first; input < first + count; input += 200) {
            QVector<QByteArray> packets;
            for (int pair = 0; pair < 100; ++pair) packets.append(audioPacket(input + pair * 2, active));
            page.submitPlSidebandFrame(rawFrame(packets), 1, input * 1000000ULL / 48000);
            page.submitRawAudioFrame(QByteArray(38400, 0), 1, input * 1000000ULL / 48000);
            QCoreApplication::processEvents();
        }
    };
    feed(0, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(recordings.size() >= 5, 3000);
    commands.clear();
    auto *azimuth = page.findChild<QDoubleSpinBox *>("noiseDirectionAAzimuthDegreesSpin");
    QVERIFY(azimuth);
    azimuth->setValue(12.345);
    QCOMPARE(commands.size(), 8);
    QVERIFY(!commands.contains(0x102));
    feed(2000, 200);
    QTRY_VERIFY_WITH_TIMEOUT(recordings.size() >= 7, 3000);
    QVector<float> actual;
    for (const auto &emission : recordings)
        actual += qvariant_cast<NoiseReductionRecordingFrame>(emission[0]).stages[0].samples;
    WienerNoiseReducer expected;
    expected.setSuppressionCoefficients(0, 5, 5);
    const auto reference = expected.process(QVector<float>(2200, 0.1f), QVector<float>(2200, 0.0f), QVector<float>(2200, 0.0f));
    QCOMPARE(actual.size(), reference.size() / 200 * 200);
    QCOMPARE(actual, reference.first(actual.size()));
    auto *status = page.findChild<QLabel *>("noiseReductionStatusLabel");
    QVERIFY(status);
    QVERIFY(status->text().contains(QStringLiteral("上位机=原维纳及后处理")));
    QVERIFY(status->text().contains(QStringLiteral("已就绪")));
    page.show();
    QTest::qWait(30);
    QVERIFY(page.grab().save(QString::fromUtf8(MICARRAY_PL_UI_PREVIEW)));
}

void PlDenoiseUiTest::fixedWienerRetryAndParameterResetUseMonoPostOnly()
{
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    auto settings = store.noiseReduction();
    settings.enabled = true; settings.monitorEnabled = false;
    settings.inputMode = NoiseReductionInputMode::PlFixedWienerOriginalPostProcessing;
    settings.mode = NoiseReductionMode::TwoPoint;
    settings.dualB1SuppressionCoefficient = 5;
    settings.bandPass.enabled = false; settings.logMmse.enabled = false; settings.automaticGainControl.enabled = false;
    store.saveNoiseReduction(settings);
    NoiseReductionPage page(&store);
    page.setActive(true);
    page.setRecordingRequired(true, {NoiseReductionDataFlowStage::Suppression});
    QVector<quint16> commands;
    quint16 active = 0, stream = 0;
    bool dropFirstCommitAck = true;
    connect(&page, &NoiseReductionPage::controlSendRequested, &page, [&](const ControlValues &values) {
        const quint16 transaction = quint16(values.channelValue(1));
        const quint16 field = quint16(values.channelValue(2));
        commands.append(field);
        if (field == 0x102) { active = 0; stream = 0; }
        if (field == 0x101) {
            active = transaction;
            if (stream == 0) stream = transaction;
            if (dropFirstCommitAck) {
                dropFirstCommitAck = false;
                // PL audio may precede the COMMIT ACK. Retain the actual first samples.
                page.submitPlSidebandFrame(rawFrame(wienerPackets(0, stream, 200)), 1, 0);
                return;
            }
        }
        page.submitPlSidebandFrame(rawFrame({wienerStatus(active, stream, transaction, field)}), 1, 0);
    });
    QSignalSpy recordings(&page, &NoiseReductionPage::noiseRecordingFrameReady);
    page.setControlLocked(false);
    page.setConnected(true);
    QTRY_COMPARE_WITH_TIMEOUT(commands.size(), 13, 2000); // 12 commands plus identical lost-ACK retry.
    QTRY_COMPARE_WITH_TIMEOUT(recordings.size(), 1, 2000);
    QCOMPARE(commands.front(), quint16(0x102));
    QCOMPARE(commands[11], quint16(0x101)); QCOMPARE(commands[12], quint16(0x101));
    const quint16 firstStream = stream;
    auto feed = [&](quint32 first, int count) {
        for (int offset = 0; offset < count; offset += 200) {
            page.submitPlSidebandFrame(rawFrame(wienerPackets(first + offset, stream, 200)), 1, 0);
            QCoreApplication::processEvents();
        }
    };
    feed(200, 800);
    QTRY_COMPARE_WITH_TIMEOUT(recordings.size(), 5, 2000);
    commands.clear();
    auto *azimuth = page.findChild<QDoubleSpinBox *>("noiseDirectionAAzimuthDegreesSpin");
    QVERIFY(azimuth); azimuth->setValue(12.3);
    QCOMPARE(commands.size(), 11); QVERIFY(!commands.contains(0x102)); QCOMPARE(stream, firstStream);
    feed(1000, 200);
    QTRY_COMPARE_WITH_TIMEOUT(recordings.size(), 6, 2000);
    commands.clear();
    auto *coefficient = page.findChild<QDoubleSpinBox *>("noiseReductionDualB1CoefficientSpin");
    QVERIFY(coefficient); coefficient->setValue(6);
    QCOMPARE(commands.size(), 12); QCOMPARE(commands.front(), quint16(0x102)); QVERIFY(stream != firstStream);
    feed(0, 400);
    QTRY_COMPARE_WITH_TIMEOUT(recordings.size(), 8, 2000);
    for (const auto &entry : recordings)
        QCOMPARE(qvariant_cast<NoiseReductionRecordingFrame>(entry[0]).stages.front().samples, QVector<float>(200, 0.125f));
    auto *status = page.findChild<QLabel *>("noiseReductionStatusLabel");
    QVERIFY(status); QVERIFY(status->text().contains(QStringLiteral("不再次维纳")));
    QVERIFY(status->text().contains(QStringLiteral("已就绪")));
}

void PlDenoiseUiTest::fixedWienerRejectsAdaptiveAndLabelsUnavailableBeams()
{
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    auto settings = store.noiseReduction();
    settings.enabled = true; settings.monitorEnabled = false;
    settings.inputMode = NoiseReductionInputMode::PlFixedWienerOriginalPostProcessing;
    settings.algorithm = NoiseReductionAlgorithm::AdaptiveWiener;
    store.saveNoiseReduction(settings);
    QCOMPARE(store.noiseReduction().inputMode, settings.inputMode);
    NoiseReductionPage page(&store);
    page.setActive(true);
    QSignalSpy commands(&page, &NoiseReductionPage::controlSendRequested);
    page.setControlLocked(false); page.setConnected(true);
    QCOMPARE(commands.size(), 0);
    auto *mode = page.findChild<QComboBox *>("noiseInputModeCombo");
    auto *status = page.findChild<QLabel *>("noiseReductionStatusLabel");
    QVERIFY(mode); QVERIFY(status);
    QVERIFY(mode->currentText().contains(QStringLiteral("PL 固定维纳")));
    QVERIFY(status->text().contains(QStringLiteral("只支持固定参数维纳")));
    for (const QString name : {QStringLiteral("noisePlotSource1Combo"), QStringLiteral("noisePlotSource2Combo"), QStringLiteral("noiseReductionMonitorSourceCombo")}) {
        auto *combo = page.findChild<QComboBox *>(name);
        QVERIFY2(combo, qPrintable(name));
        auto *model = qobject_cast<QStandardItemModel *>(combo->model());
        QVERIFY(model);
        for (int index = 0; index < combo->count(); ++index) {
            const auto source = static_cast<NoiseSignalSource>(combo->itemData(index).toInt());
            const bool available = source == NoiseSignalSource::FirstElement || source == NoiseSignalSource::Denoised;
            QCOMPARE(model->item(index)->isEnabled(), available);
            if (!available) QVERIFY(combo->itemText(index).contains(QStringLiteral("不上传")));
        }
    }
}

namespace
{
NoiseReductionSettings fullProfile()
{
    NoiseReductionSettings value;
    value.enabled = true; value.monitorEnabled = false;
    value.inputMode = NoiseReductionInputMode::PlFullDenoisingFixedProfile;
    value.bandPass.enabled = true; value.bandPass.lowCutoffHz = 100; value.bandPass.highCutoffHz = 8000;
    value.logMmse.enabled = true; value.logMmse.fftSize = 2048; value.logMmse.overlapRatio = .5;
    value.logMmse.priorSnrSmoothing = .98; value.logMmse.noisePowerSmoothing = .95; value.logMmse.minimumGainDb = -20;
    value.automaticGainControl.enabled = true; value.automaticGainControl.targetLevelDbfs = -24;
    value.automaticGainControl.maximumGainDb = 20; value.automaticGainControl.maximumAttenuationDb = -20;
    value.automaticGainControl.attackMilliseconds = 20; value.automaticGainControl.releaseMilliseconds = 300;
    value.automaticGainControl.limiterLevelDbfs = -1;
    return value;
}
QByteArray fullStatus(quint16 active, quint16 stream, quint16 txn, quint16 field, bool wrongFingerprint = false)
{
    auto packet = wienerStatus(active, stream, txn, field);
    packet[5] = active ? 3 : 1;
    qToLittleEndian(quint32(0x20261022), packet.data() + 25);
    qToLittleEndian(active ? PlDenoiseTransport::FullProfileFingerprint ^ quint32(wrongFingerprint) : 0U, packet.data() + 33);
    packet[40] = char(PlDenoiseTransport::crc8(packet.first(40)));
    return packet;
}
QVector<QByteArray> fullAudio(quint32 first, quint16 stream)
{
    auto packets = wienerPackets(first, stream, 200);
    for (auto &packet : packets)
    {
        packet.replace(0, 4, "AUF1"); packet[5] = 0x31;
        packet[40] = char(PlDenoiseTransport::crc8(packet.first(40)));
    }
    return packets;
}
}
void PlDenoiseUiTest::fullModeDisplaysFinalAndRejectsChangedPostProfile()
{
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    store.saveNoiseReduction(fullProfile());
    NoiseReductionPage page(&store);
    page.setActive(true); page.setRecordingRequired(true, {NoiseReductionDataFlowStage::AutomaticGainControl});
    quint16 active = 0, stream = 0;
    QVector<quint16> commands;
    connect(&page, &NoiseReductionPage::controlSendRequested, &page, [&](const ControlValues &value)
    {
        const quint16 txn = quint16(value.channelValue(1)), field = quint16(value.channelValue(2));
        commands.append(field);
        if (field == 0x102) { active = 0; stream = 0; }
        if (field == 0x101) { active = txn; if (!stream) stream = txn; }
        page.submitPlSidebandFrame(rawFrame({fullStatus(active, stream, txn, field)}), 1, 0);
    });
    QSignalSpy recordings(&page, &NoiseReductionPage::noiseRecordingFrameReady);
    page.setControlLocked(false); page.setConnected(true);
    QCOMPARE(commands.size(), 12);
    page.submitPlSidebandFrame(rawFrame(fullAudio(0, stream)), 1, 0);
    QTRY_COMPARE_WITH_TIMEOUT(recordings.size(), 1, 2000);
    const auto record = qvariant_cast<NoiseReductionRecordingFrame>(recordings[0][0]);
    QCOMPARE(record.stages[0].stage, NoiseReductionDataFlowStage::AutomaticGainControl);
    QCOMPARE(record.stages[0].samples, QVector<float>(200, .125f)); // No repeated BP/Log/AGC.
    auto *status = page.findChild<QLabel *>("noiseReductionStatusLabel");
    auto *mode = page.findChild<QComboBox *>("noiseInputModeCombo");
    QVERIFY(status); QVERIFY(mode);
    QCOMPARE(mode->currentText(), QStringLiteral("PL 全链降噪（固定演示参数）"));
    QVERIFY(status->text().contains(QStringLiteral("上位机不再降噪")));
    QCOMPARE(page.property("plDenoiseProfileFingerprint").toUInt(), PlDenoiseTransport::FullProfileFingerprint);
    commands.clear();
    auto *azimuth = page.findChild<QDoubleSpinBox *>("noiseDirectionAAzimuthDegreesSpin");
    QVERIFY(azimuth); azimuth->setValue(13.5);
    QCOMPARE(commands.size(), 11); QVERIFY(!commands.contains(0x102));
    commands.clear();
    auto *fft = page.findChild<QComboBox *>("noiseReductionLogMmseFftSizeCombo");
    QVERIFY(fft); fft->setCurrentIndex(fft->findData(512));
    QCOMPARE(commands.size(), 0);
    QCOMPARE(page.currentSettings().logMmse.fftSize, 512); QCOMPARE(store.noiseReduction().logMmse.fftSize, 512);
    QVERIFY(status->text().contains(QStringLiteral("不兼容")));
    QCOMPARE(page.currentSettings().inputMode, NoiseReductionInputMode::PlFullDenoisingFixedProfile);
}
void PlDenoiseUiTest::fullModeRejectsWrongCapabilityFingerprint()
{
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini")); store.saveNoiseReduction(fullProfile());
    NoiseReductionPage page(&store); page.setActive(true);
    connect(&page, &NoiseReductionPage::controlSendRequested, &page, [&](const ControlValues &value)
    {
        const quint16 txn = quint16(value.channelValue(1)), field = quint16(value.channelValue(2));
        const bool committed = field == 0x101;
        page.submitPlSidebandFrame(rawFrame({fullStatus(committed ? txn : 0, committed ? txn : 0, txn, field, committed)}), 1, 0);
    });
    page.setControlLocked(false); page.setConnected(true);
    auto *status = page.findChild<QLabel *>("noiseReductionStatusLabel"); QVERIFY(status);
    QVERIFY(status->text().contains(QStringLiteral("指纹不匹配")));
    QCOMPARE(page.currentSettings().inputMode, NoiseReductionInputMode::PlFullDenoisingFixedProfile);
}

QTEST_MAIN(PlDenoiseUiTest)
#include "test_pldenoise_ui.moc"
