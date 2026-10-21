#include "media/noisereductionmailbox.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QTimer>
#include <QSemaphore>
#include <QScopeGuard>
#include <QtTest/QtTest>
#include <QApplication>
#include <QLabel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

#include "app/appsettings.h"
#include "control/pldenoisesession.h"
#include "media/noisereductionworker.h"
#include <QImage>
#include <QThread>
#include <QWidget>

// Test-only visibility to observe the actual worker without changing any host file.
// All dependencies are already included, so this applies only to the page class.
#define private public
#include "ui/noisereductionpage.h"
#undef private

#include <atomic>
#include <bit>

using namespace MicArray;

namespace
{
QByteArray checked(QByteArray packet)
{
    packet[40] = char(PlDenoiseTransport::crc8(packet.first(40)));
    return packet;
}

QByteArray rawFrame(const QVector<QByteArray> &packets)
{
    Q_ASSERT(packets.size() <= 200);
    QByteArray raw(38400, 0);
    for (int sample = 0; sample < packets.size(); ++sample)
    {
        for (int byte = 0; byte < 41; ++byte)
        {
            raw[sample * 192 + (byte + 7) * 4 + 3] = packets[sample][byte];
        }
    }
    return raw;
}

QByteArray statusPacket(int stage, quint16 active, quint16 stream,
                      quint16 transaction, quint16 field)
{
    QByteArray packet(41, 0);
    packet.replace(0, 4, "DNS1");
    packet[4] = 1;
    packet[5] = char(active ? stage : 1);
    qToLittleEndian(active, packet.data() + 6);
    qToLittleEndian(transaction, packet.data() + 8);
    qToLittleEndian(field, packet.data() + 10);
    packet[12] = active ? 1 : 0;
    qToLittleEndian(quint32(stage == 1 ? 0x20261020 : stage == 2 ? 0x20261021 : 0x20261022),
                    packet.data() + 25);
    if (stage > 1)
    {
        qToLittleEndian(stream, packet.data() + 31);
    }
    if (stage == 3)
    {
        qToLittleEndian(active ? PlDenoiseTransport::FullProfileFingerprint : 0U,
                        packet.data() + 33);
    }
    return checked(packet);
}

QByteArray audio200(int stage, quint32 first, quint16 stream)
{
    QVector<QByteArray> packets;
    const int capacity = stage == 1 ? 2 : 6;
    for (int offset = 0; offset < 200; offset += capacity)
    {
        const int count = std::min(capacity, 200 - offset);
        QByteArray packet(41, 0);
        packet.replace(0, 4, stage == 1 ? "AUD3" : stage == 2 ? "AUW1" : "AUF1");
        packet[4] = 1;
        packet[5] = char(stage == 1 ? 0x11 : stage == 2 ? 0x21 : 0x31);
        qToLittleEndian(stream, packet.data() + 6);
        qToLittleEndian(first + quint32(offset), packet.data() + 8);
        packet[12] = char(count);
        for (int sample = 0; sample < count; ++sample)
        {
            const int beams = stage == 1 ? 3 : 1;
            for (int beam = 0; beam < beams; ++beam)
            {
                const float value = beam == 0 ? float(1 + (first + quint32(offset + sample)) % 4093) / 65536.0f : 0.0f;
                qToLittleEndian(std::bit_cast<quint32>(value),
                                packet.data() + 13 + (sample * beams + beam) * 4);
            }
        }
        packets.append(checked(packet));
    }
    return rawFrame(packets);
}

NoiseReductionSettings settingsFor(int stage)
{
    NoiseReductionSettings value;
    value.enabled = true;
    value.monitorEnabled = false;
    value.inputMode = stage == 1 ? NoiseReductionInputMode::PlThreeBeamOriginalDenoising
        : stage == 2 ? NoiseReductionInputMode::PlFixedWienerOriginalPostProcessing
                     : NoiseReductionInputMode::PlFullDenoisingFixedProfile;
    value.mode = NoiseReductionMode::TwoPoint;
    value.singleSuppressionCoefficient = 8.5;
    value.dualB1SuppressionCoefficient = 5;
    value.dualB2SuppressionCoefficient = 5;
    if (stage == 3)
    {
        value.bandPass.enabled = true;
        value.bandPass.lowCutoffHz = 100;
        value.bandPass.highCutoffHz = 8000;
        value.logMmse.enabled = true;
        value.logMmse.fftSize = 2048;
        value.logMmse.overlapRatio = .5;
        value.logMmse.priorSnrSmoothing = .98;
        value.logMmse.noisePowerSmoothing = .95;
        value.logMmse.minimumGainDb = -20;
        value.automaticGainControl.enabled = true;
        value.automaticGainControl.targetLevelDbfs = -24;
        value.automaticGainControl.maximumGainDb = 20;
        value.automaticGainControl.maximumAttenuationDb = -20;
        value.automaticGainControl.attackMilliseconds = 20;
        value.automaticGainControl.releaseMilliseconds = 300;
        value.automaticGainControl.limiterLevelDbfs = -1;
    }
    return value;
}

bool drainWorker(NoiseReductionPage &page)
{
    return QMetaObject::invokeMethod(page.m_worker, [] {}, Qt::BlockingQueuedConnection);
}

void deliverUiReceipts(NoiseReductionPage &page)
{
    QCoreApplication::sendPostedEvents(&page, QEvent::MetaCall);
}

struct Handshake
{
    quint16 active = 0;
    quint16 stream = 0;
    int commands = 0;
};

void connectHandshake(NoiseReductionPage &page, int stage, Handshake &state,
                      int beforeCommitFrames = 0)
{
    QObject::connect(&page, &NoiseReductionPage::controlSendRequested, &page,
                     [&page, stage, &state, beforeCommitFrames](const ControlValues &values)
    {
        ++state.commands;
        const quint16 transaction = quint16(values.channelValue(1));
        const quint16 field = quint16(values.channelValue(2));
        if (field == 0x102)
        {
            state.active = 0;
            state.stream = 0;
        }
        if (field == 0x101)
        {
            state.active = transaction;
            if (!state.stream)
            {
                state.stream = transaction;
            }
            for (int frame = 0; frame < beforeCommitFrames; ++frame)
            {
                page.submitPlSidebandFrame(audio200(stage, frame * 200, state.stream),
                                          1, frame * 4167);
            }
        }
        page.submitPlSidebandFrame(rawFrame({statusPacket(stage, state.active,
            state.stream, transaction, field)}), 1, 0);
    });
}
}

namespace
{
struct WorkerGate
{
    QSemaphore entered;
    QSemaphore resume;
};

std::shared_ptr<WorkerGate> queueWorkerGate(NoiseReductionPage &page)
{
    auto gate = std::make_shared<WorkerGate>();
    page.m_mailbox->enqueue(NoiseReductionMailbox::Kind::Control, 0, [gate]
    {
        gate->entered.release();
        gate->resume.acquire();
    });
    return gate;
}

QVector<float> ramp(int count)
{
    QVector<float> result;
    result.reserve(count);
    for (int index = 0; index < count; ++index)
    {
        result.append(float(1 + quint32(index) % 4093) / 65536.0f);
    }
    return result;
}

QByteArray partialAudio(int stage, quint32 first, quint16 stream, int total)
{
    QVector<QByteArray> packets;
    const int capacity = stage == 1 ? 2 : 6;
    for (int offset = 0; offset < total; offset += capacity)
    {
        const int count = std::min(capacity, total - offset);
        QByteArray packet(41, 0);
        packet.replace(0, 4, stage == 1 ? "AUD3" : stage == 2 ? "AUW1" : "AUF1");
        packet[4] = 1;
        packet[5] = char(stage == 1 ? 0x11 : stage == 2 ? 0x21 : 0x31);
        qToLittleEndian(stream, packet.data() + 6);
        qToLittleEndian(first + quint32(offset), packet.data() + 8);
        packet[12] = char(count);
        for (int sample = 0; sample < count; ++sample)
        {
            const int beams = stage == 1 ? 3 : 1;
            for (int beam = 0; beam < beams; ++beam)
            {
                const float value = beam == 0
                    ? float(1 + (first + quint32(offset + sample)) % 4093) / 65536.0f : 0;
                qToLittleEndian(std::bit_cast<quint32>(value),
                                packet.data() + 13 + (sample * beams + beam) * 4);
            }
        }
        packets.append(checked(packet));
    }
    return rawFrame(packets);
}

QVector<float> referenceSamples(int stage, int count)
{
    QVector<float> result = ramp(count);
    if (stage == 1)
    {
        const auto settings = settingsFor(stage);
        WienerNoiseReducer reference;
        reference.setAlgorithm(settings.algorithm);
        reference.setMode(settings.mode);
        reference.setSuppressionCoefficients(settings.singleSuppressionCoefficient,
            settings.dualB1SuppressionCoefficient, settings.dualB2SuppressionCoefficient);
        result = reference.process(result, QVector<float>(count, 0), QVector<float>(count, 0));
    }
    return result.first(result.size() / 200 * 200);
}

QVector<float> recordedSamples(const QSignalSpy &recordings, quint32 minimumSecond = 0)
{
    QVector<float> result;
    for (const auto &emission : recordings)
    {
        const auto frame = qvariant_cast<NoiseReductionRecordingFrame>(emission[0]);
        if (frame.timestampSeconds >= minimumSecond && frame.stages.size() == 1)
        {
            result += frame.stages[0].samples;
        }
    }
    return result;
}

bool drainFixedQueues(NoiseReductionPage &page)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 5000)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        const auto state = page.m_mailbox->snapshot();
        if (state.plBufferedSamples == 0 && state.referenceBufferedSamples == 0
            && state.controls == 0 && state.inFlight == 0)
        {
            QCoreApplication::sendPostedEvents(&page, QEvent::MetaCall);
            return true;
        }
        QThread::msleep(1);
    }
    return false;
}

void configureRecording(NoiseReductionPage &page, int stage)
{
    page.setActive(true);
    page.setRecordingRequired(true, {stage == 3
        ? NoiseReductionDataFlowStage::AutomaticGainControl
        : NoiseReductionDataFlowStage::Suppression});
    // This diagnostic isolates queue/epoch behavior; it does not simulate elapsed
    // realtime packet delivery and therefore disables only its own watchdog.
    page.m_plWatchdog->stop();
}
}

class StreamStallFixedTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }
    void mixedBurstPreservesEveryPlSample_data();
    void mixedBurstPreservesEveryPlSample();
    void commitAckBurstPreservesEverySample_data();
    void commitAckBurstPreservesEverySample();
    void twoSecondCapacityHasExplicitBoundary_data();
    void twoSecondCapacityHasExplicitBoundary();
    void resetRetainsOldCreditsAndStartsCleanEpoch_data();
    void resetRetainsOldCreditsAndStartsCleanEpoch();
    void modeSwitchRetainsOldCredits_data();
    void modeSwitchRetainsOldCredits();
    void partialEpochTailsPrecedeTheirAuthorizations_data();
    void partialEpochTailsPrecedeTheirAuthorizations();
    void authorizationCapacityCountsQueuedAndInFlight_data();
    void authorizationCapacityCountsQueuedAndInFlight();
};

void StreamStallFixedTest::mixedBurstPreservesEveryPlSample_data()
{
    QTest::addColumn<int>("stage");
    QTest::newRow("AUD3") << 1;
    QTest::newRow("AUW1") << 2;
    QTest::newRow("AUF1") << 3;
}

void StreamStallFixedTest::mixedBurstPreservesEveryPlSample()
{
    QFETCH(int, stage);
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    store.saveNoiseReduction(settingsFor(stage));
    NoiseReductionPage page(&store);
    configureRecording(page, stage);
    Handshake state;
    connectHandshake(page, stage, state);
    page.setControlLocked(false);
    page.setConnected(true);
    QVERIFY(drainFixedQueues(page));
    QSignalSpy recordings(&page, &NoiseReductionPage::noiseRecordingFrameReady);
    QSignalSpy faults(page.m_plTransport, &PlDenoiseTransport::faultRaised);
    for (int frame = 0; frame < 128; ++frame)
    {
        page.submitPlSidebandFrame(audio200(stage, frame * 200, state.stream),
                                  1, frame * 4167);
        page.submitRawAudioFrame(QByteArray(38400, 0), 1, frame * 4167);
        QVERIFY(drainWorker(page));
        QVERIFY(page.m_mailbox->snapshot().inFlight <= 1);
        QVERIFY(page.m_mailbox->snapshot().inFlight <= 1);
        QVERIFY(!page.m_plTransport->faulted());
    }
    QVERIFY(drainFixedQueues(page));
    QCOMPARE(faults.size(), 0);
    const auto expected = referenceSamples(stage, 25600);
    QCOMPARE(recordedSamples(recordings), expected);
    qInfo().noquote() << "[FIXED_CONTINUITY_PASS] stage=" << stage
        << " input_frames=128 raw_frames=128 compared_samples=" << expected.size()
        << " fault=0 pending=0";
}

void StreamStallFixedTest::commitAckBurstPreservesEverySample_data()
{
    mixedBurstPreservesEveryPlSample_data();
}

void StreamStallFixedTest::commitAckBurstPreservesEverySample()
{
    QFETCH(int, stage);
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    store.saveNoiseReduction(settingsFor(stage));
    NoiseReductionPage page(&store);
    configureRecording(page, stage);
    Handshake state;
    connectHandshake(page, stage, state, 33);
    QSignalSpy recordings(&page, &NoiseReductionPage::noiseRecordingFrameReady);
    QSignalSpy faults(page.m_plTransport, &PlDenoiseTransport::faultRaised);
    page.setControlLocked(false);
    page.setConnected(true);
    QVERIFY(!page.m_plTransport->faulted());
    for (int frame = 33; frame < 97; ++frame)
    {
        page.submitPlSidebandFrame(audio200(stage, frame * 200, state.stream),
                                  1, frame * 4167);
    }
    QVERIFY(drainFixedQueues(page));
    QCOMPARE(faults.size(), 0);
    const auto expected = referenceSamples(stage, 19400);
    QCOMPARE(recordedSamples(recordings), expected);
    qInfo().noquote() << "[FIXED_ACK_CONTINUITY_PASS] stage=" << stage
        << " pre_ack_samples=6600 continuation_samples=12800 compared_samples="
        << expected.size() << " fault=0 pending=0";
}

void StreamStallFixedTest::twoSecondCapacityHasExplicitBoundary_data()
{
    mixedBurstPreservesEveryPlSample_data();
}

void StreamStallFixedTest::twoSecondCapacityHasExplicitBoundary()
{
    QFETCH(int, stage);
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    store.saveNoiseReduction(settingsFor(stage));
    NoiseReductionPage page(&store);
    configureRecording(page, stage);
    Handshake state;
    connectHandshake(page, stage, state);
    page.setControlLocked(false);
    page.setConnected(true);
    QVERIFY(drainFixedQueues(page));
    QSignalSpy faults(page.m_plTransport, &PlDenoiseTransport::faultRaised);
    auto gate = queueWorkerGate(page);
    auto resumeOnExit = qScopeGuard([gate] { gate->resume.release(); });
    QVERIFY(gate->entered.tryAcquire(1, 1000));
    for (int frame = 0; frame < 480; ++frame)
    {
        page.submitPlSidebandFrame(audio200(stage, frame * 200, state.stream),
                                  1, frame * 4167);
        QVERIFY2(!page.m_plTransport->faulted(), qPrintable(QString::number(frame)));
        QVERIFY(page.m_mailbox->snapshot().inFlight <= 1);
        QVERIFY(page.m_mailbox->snapshot().inFlight <= 1);
    }
    QCOMPARE(faults.size(), 0);
    page.submitPlSidebandFrame(audio200(stage, 96000, state.stream), 3, 0);
    QCOMPARE(faults.size(), 1);
    QVERIFY(page.m_plTransport->faulted());
    gate->resume.release();
    resumeOnExit.dismiss();
    QVERIFY(drainFixedQueues(page));
    qInfo().noquote() << "[FIXED_CAPACITY_PASS] stage=" << stage
        << " accepted_samples=96000 first_rejected_sample=96000 reason="
        << faults[0][0].toString();
}

void StreamStallFixedTest::resetRetainsOldCreditsAndStartsCleanEpoch_data()
{
    mixedBurstPreservesEveryPlSample_data();
}

void StreamStallFixedTest::resetRetainsOldCreditsAndStartsCleanEpoch()
{
    QFETCH(int, stage);
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    store.saveNoiseReduction(settingsFor(stage));
    NoiseReductionPage page(&store);
    configureRecording(page, stage);
    Handshake state;
    connectHandshake(page, stage, state);
    page.setControlLocked(false);
    page.setConnected(true);
    QVERIFY(drainFixedQueues(page));
    QSignalSpy recordings(&page, &NoiseReductionPage::noiseRecordingFrameReady);
    QSignalSpy faults(page.m_plTransport, &PlDenoiseTransport::faultRaised);
    auto gate = queueWorkerGate(page);
    auto resumeOnExit = qScopeGuard([gate] { gate->resume.release(); });
    QVERIFY(gate->entered.tryAcquire(1, 1000));
    for (int frame = 0; frame < 12; ++frame)
    {
        page.submitPlSidebandFrame(audio200(stage, frame * 200, state.stream),
                                  1, frame * 4167);
    }
    const int oldCredits = page.m_mailbox->snapshot().inFlight;
    const auto generation = page.property("plWorkGeneration").toULongLong();
    QVERIFY(oldCredits > 0);
    QVERIFY(oldCredits <= 1);
    page.setConnected(false);
    QCOMPARE(page.m_mailbox->snapshot().inFlight, oldCredits);
    page.setConnected(true);
    QCOMPARE(page.m_mailbox->snapshot().inFlight, oldCredits);
    QVERIFY(page.property("plWorkGeneration").toULongLong() > generation);
    for (int frame = 0; frame < 33; ++frame)
    {
        page.submitPlSidebandFrame(audio200(stage, frame * 200, state.stream),
                                  10, frame * 4167);
    }
    gate->resume.release();
    resumeOnExit.dismiss();
    QVERIFY(drainFixedQueues(page));
    QCOMPARE(faults.size(), 0);
    const auto expected = referenceSamples(stage, 6600);
    QCOMPARE(recordedSamples(recordings, 10), expected);
    qInfo().noquote() << "[FIXED_RESET_CREDIT_PASS] stage=" << stage
        << " old_credits=" << oldCredits << " new_epoch_samples=" << expected.size()
        << " pending=0 fault=0";
}

void StreamStallFixedTest::modeSwitchRetainsOldCredits_data()
{
    QTest::addColumn<int>("stage");
    QTest::addColumn<int>("nextStage");
    QTest::newRow("AUD3_to_AUW1") << 1 << 2;
    QTest::newRow("AUW1_to_AUD3") << 2 << 1;
}

void StreamStallFixedTest::modeSwitchRetainsOldCredits()
{
    QFETCH(int, stage);
    QFETCH(int, nextStage);
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    store.saveNoiseReduction(settingsFor(stage));
    NoiseReductionPage page(&store);
    configureRecording(page, stage);
    Handshake state;
    connectHandshake(page, stage, state);
    page.setControlLocked(false);
    page.setConnected(true);
    QVERIFY(drainFixedQueues(page));
    QSignalSpy recordings(&page, &NoiseReductionPage::noiseRecordingFrameReady);
    QSignalSpy faults(page.m_plTransport, &PlDenoiseTransport::faultRaised);
    auto gate = queueWorkerGate(page);
    auto resumeOnExit = qScopeGuard([gate] { gate->resume.release(); });
    QVERIFY(gate->entered.tryAcquire(1, 1000));
    for (int frame = 0; frame < 12; ++frame)
    {
        page.submitPlSidebandFrame(audio200(stage, frame * 200, state.stream),
                                  1, frame * 4167);
    }
    const int oldCredits = page.m_mailbox->snapshot().inFlight;
    const auto generation = page.property("plWorkGeneration").toULongLong();
    QVERIFY(oldCredits > 0);
    QVERIFY(oldCredits <= 1);
    QObject::disconnect(&page, &NoiseReductionPage::controlSendRequested, nullptr, nullptr);
    connectHandshake(page, nextStage, state);
    auto *mode = page.findChild<QComboBox *>("noiseInputModeCombo");
    QVERIFY(mode);
    const auto nextInputMode = settingsFor(nextStage).inputMode;
    mode->setCurrentIndex(mode->findData(int(nextInputMode)));
    QCOMPARE(page.currentSettings().inputMode, nextInputMode);
    QCOMPARE(page.m_mailbox->snapshot().inFlight, oldCredits);
    QVERIFY(page.property("plWorkGeneration").toULongLong() > generation);
    for (int frame = 0; frame < 33; ++frame)
    {
        page.submitPlSidebandFrame(audio200(nextStage, frame * 200, state.stream),
                                  10, frame * 4167);
    }
    gate->resume.release();
    resumeOnExit.dismiss();
    QVERIFY(drainFixedQueues(page));
    QCOMPARE(faults.size(), 0);
    const auto expected = referenceSamples(nextStage, 6600);
    QCOMPARE(recordedSamples(recordings, 10), expected);
    qInfo().noquote() << "[FIXED_MODE_CREDIT_PASS] previous_stage=" << stage
        << " next_stage=" << nextStage << " old_credits=" << oldCredits
        << " compared_samples=" << expected.size() << " pending=0 fault=0";
}

void StreamStallFixedTest::partialEpochTailsPrecedeTheirAuthorizations_data()
{
    mixedBurstPreservesEveryPlSample_data();
}

void StreamStallFixedTest::partialEpochTailsPrecedeTheirAuthorizations()
{
    QFETCH(int, stage);
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    store.saveNoiseReduction(settingsFor(stage));
    NoiseReductionPage page(&store);
    configureRecording(page, stage);
    Handshake state;
    connectHandshake(page, stage, state);
    page.setControlLocked(false);
    page.setConnected(true);
    QVERIFY(drainFixedQueues(page));
    QSignalSpy recordings(&page, &NoiseReductionPage::noiseRecordingFrameReady);
    QSignalSpy faults(page.m_plTransport, &PlDenoiseTransport::faultRaised);
    QStringList warnings;
    connect(page.m_worker, &NoiseReductionWorker::warningRaised, &page,
            [&warnings](const QString &warning) { warnings.append(warning); });
    struct SubmittedFrame
    {
        quint32 first;
        quint16 stream;
        QVector<float> samples;
    };
    QVector<SubmittedFrame> submitted;
    connect(&page, &NoiseReductionPage::plBeamFrameRequested, &page,
        [&submitted](const PlBeamFrame &frame)
        { submitted.append({frame.firstSampleIndex, frame.configId, frame.beams[0]}); });
    connect(&page, &NoiseReductionPage::plWienerFrameRequested, &page,
        [&submitted](const PlWienerFrame &frame)
        { submitted.append({frame.firstSampleIndex, frame.streamId, frame.samples}); });
    connect(&page, &NoiseReductionPage::plFinalFrameRequested, &page,
        [&submitted](const PlFinalFrame &frame)
        { submitted.append({frame.firstSampleIndex, frame.streamId, frame.samples}); });
    auto gate = queueWorkerGate(page);
    auto resumeOnExit = qScopeGuard([gate] { gate->resume.release(); });
    QVERIFY(gate->entered.tryAcquire(1, 1000));
    auto *azimuth = page.findChild<QDoubleSpinBox *>("noiseDirectionAAzimuthDegreesSpin");
    QVERIFY(azimuth);
    QVector<quint16> streams;
    const std::array<int, 3> lengths {50, 70, 80};
    quint32 first = 0;
    for (int epoch = 0; epoch < 3; ++epoch)
    {
        const quint16 stream = stage == 1 ? state.active : state.stream;
        streams.append(stream);
        page.submitPlSidebandFrame(partialAudio(stage, first, stream, lengths[epoch]),
                                  1, first * 1000000ULL / 48000);
        first += lengths[epoch];
        if (epoch < 2)
        {
            azimuth->setValue(epoch + 1.0);
        }
    }
    page.m_plTransport->flushBufferedBeforeConfiguration();
    QCOMPARE(submitted.size(), 3);
    QVector<float> actualInput;
    first = 0;
    for (int epoch = 0; epoch < 3; ++epoch)
    {
        QCOMPARE(submitted[epoch].first, first);
        QCOMPARE(submitted[epoch].stream, streams[epoch]);
        QCOMPARE(submitted[epoch].samples.size(), lengths[epoch]);
        first += lengths[epoch];
        actualInput += submitted[epoch].samples;
    }
    QCOMPARE(actualInput, ramp(200));
    gate->resume.release();
    resumeOnExit.dismiss();
    QVERIFY(drainFixedQueues(page));
    for (int input = 200; input < 2200; input += 200)
    {
        page.submitPlSidebandFrame(audio200(stage, input,
            stage == 1 ? state.active : state.stream), 1, input * 1000000ULL / 48000);
    }
    QVERIFY(drainFixedQueues(page));
    QCOMPARE(faults.size(), 0);
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    const auto expected = referenceSamples(stage, 2200);
    QCOMPARE(recordedSamples(recordings), expected);
    qInfo().noquote() << "[FIXED_PARTIAL_EPOCH_PASS] stage=" << stage
        << " epochs=3 partial_lengths=50,70,80 worker_paused_across_acks=2 compared_samples="
        << expected.size() << " warnings=0 fault=0";
}

void StreamStallFixedTest::authorizationCapacityCountsQueuedAndInFlight_data()
{
    mixedBurstPreservesEveryPlSample_data();
}

void StreamStallFixedTest::authorizationCapacityCountsQueuedAndInFlight()
{
    QFETCH(int, stage);
    QTemporaryDir directory;
    AppSettings store(directory.filePath("settings.ini"));
    store.saveNoiseReduction(settingsFor(stage));
    NoiseReductionPage page(&store);
    configureRecording(page, stage);
    Handshake state;
    connectHandshake(page, stage, state);
    page.setControlLocked(false);
    page.setConnected(true);
    QVERIFY(drainFixedQueues(page));
    QSignalSpy faults(page.m_plTransport, &PlDenoiseTransport::faultRaised);
    auto gate = queueWorkerGate(page); // One real executing generic control.
    auto resumeOnExit = qScopeGuard([gate] { gate->resume.release(); });
    QVERIFY(gate->entered.tryAcquire(1, 1000));
    for (int control = 1; control < 64; ++control)
    {
        QVERIFY(page.enqueueWorkerControl([] {}, control % 2 == 0));
        QCOMPARE(page.m_mailbox->snapshot().controls, control + 1);
    }
    QCOMPARE(page.m_mailbox->snapshot().controls, 64);
    QCOMPARE(page.m_mailbox->snapshot().inFlight, 1);
    QVERIFY(!page.enqueueWorkerControl([] {}, true));
    QCOMPARE(faults.size(), 1);
    QVERIFY(page.m_plTransport->faulted());
    QCOMPARE(page.m_mailbox->snapshot().inFlight, 1);
    gate->resume.release();
    resumeOnExit.dismiss();
    QVERIFY(drainFixedQueues(page));
    QCOMPARE(page.m_mailbox->snapshot().controls, 0);
    qInfo().noquote() << "[FIXED_AUTH_BOUNDARY_PASS] stage=" << stage
        << " combined_control_capacity=64 actual_executing=1 first_rejected=65 final_controls=0";
}

QTEST_MAIN(StreamStallFixedTest)
#include "test_legacy_contract.moc"
