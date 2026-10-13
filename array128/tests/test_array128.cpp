#include "diagnostics.h"
#include "dsp.h"
#include "hardwareprofile.h"
#include "mainwindow.h"
#include "recorder.h"
#include "sys3protocol.h"
#include "sys3client.h"
#include "sys3panel.h"
#include "cameraprofile.h"
#include "channelmaskwidget.h"
#include "analysis/wavefilereader.h"
#include "analysis/directionprojection.h"

#include <QFile>
#include <QDir>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QSignalSpy>
#include <QTimer>
#include <QWheelEvent>
#include <QUdpSocket>
#include <QtEndian>

#include <cmath>

using namespace MicArray::Array128;

class Array128Test : public QObject
{
    Q_OBJECT
private:
    QByteArray uartWords(const std::array<quint32, 34> &words)
    {
        QByteArray line("SYS3");
        for (quint32 w : words) line += ' ' + QByteArray::number(w, 16).rightJustified(8, '0').toUpper();
        return line + "\r\n";
    }
    QByteArray uartLine(quint32 sequence = 1, quint32 uptime = 250)
    {
        std::array<quint32, 34> words {};
        words[0] = sequence; words[1] = uptime; words[2] = 7 | (7 << 3) | (15 << 11);
        words[7] = 7; words[8] = (1 << 9) | (1 << 10); words[12] = 0x2c00a;
        words[13] = 0x001cc916; words[20] = 0xa42c; words[21] = 0x8006; words[27] = 2;
        return uartWords(words);
    }
    QByteArray audioUartWords(const std::array<quint32, 14> &words, bool gain = false)
    {
        QByteArray line(gain ? "GAIN1" : "I2S1");
        for (quint32 word : words) line += ' ' + QByteArray::number(word, 16).rightJustified(8, '0').toUpper();
        return line + "\r\n";
    }
    std::array<quint32, 14> audioUartState(quint32 sequence = 1)
    {
        std::array<quint32, 14> words {};
        words[0] = sequence; words[1] = 5; words[2] = words[3] = 128;
        for (int i = 4; i <= 7; ++i) words[i] = 0x400000;
        words[8] = 48000; words[9] = words[12] = 47999; words[11] = 0x800000;
        return words;
    }
    QByteArray fixture()
    {
        QFile f(QStringLiteral(M128_TEST_ROOT "/tests/fixtures/selftest_packet_v1.bin"));
        if (!f.open(QIODevice::ReadOnly)) return {};
        return f.readAll();
    }
    Packet packet(quint64 sequence)
    {
        auto bytes = fixture();
        qToBigEndian(sequence, bytes.data() + 16);
        const auto ref = selftestReference();
        for (int r = 0; r < 3; ++r) {
            quint64 id = 96 + sequence * 3 + r;
            qToBigEndian(id, bytes.data() + 32 + r * 392);
            memcpy(bytes.data() + 40 + r * 392, ref.constData() + (id % 16) * 384, 384);
        }
        return *decodeDatagram(bytes);
    }
private slots:
    void init()
    {
        // The test main redirects these settings to its own temporary directory.
        // Each case starts clean; production operator settings are never touched.
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, "MicArray", "MicArray128Tests"); settings.clear();
    }
    void channelMaskParsingIsAtomic()
    {
        ChannelMask mask {}; QString error;
        QVERIFY(parseChannelMask(QStringLiteral("0, 7；16-23 127，7"), &mask, &error));
        QCOMPARE(channelMaskText(mask), QStringLiteral("0,7,16-23,127"));
        const auto original = mask;
        for (const auto &invalid : {"0,128", "0,-1", "3-1", "4.5", "1-", "1-2-3", "CH7", "1e2", "1 99999999999"}) {
            QVERIFY(!parseChannelMask(invalid, &mask, &error));
            QVERIFY(mask == original); QVERIFY(!error.isEmpty());
        }
        QVERIFY(parseChannelMask("", &mask, &error)); QCOMPARE(channelMaskText(mask), QString());
        QVERIFY(parseChannelMask("0-127", &mask, &error)); QVERIFY(std::all_of(mask.begin(), mask.end(), [](bool v) { return v; }));
    }
    void channelMaskWidgetAppliesAndPreservesSettings()
    {
        {
            MainWindow window;
            auto *mask = window.findChild<ChannelMaskWidget *>(); QVERIFY(mask);
            auto *list = mask->findChild<QLineEdit *>("disabledChannelList");
            auto *apply = mask->findChild<QPushButton *>("applyChannelMask");
            auto *table = mask->findChild<QTableWidget *>("channelMaskTable"); QVERIFY(list); QVERIFY(apply); QVERIFY(table);
            list->setText("1,4-6,127"); apply->click();
            QVERIFY(mask->mask()[1]); QVERIFY(mask->mask()[127]); QVERIFY(!mask->mask()[0]);
            QCOMPARE(table->item(7, 15)->checkState(), Qt::Checked);
            const auto before = mask->mask();
            QSignalSpy rejected(mask, &ChannelMaskWidget::inputRejected);
            list->setText("2,128"); apply->click(); QCOMPARE(rejected.size(), 1); QVERIFY(mask->mask() == before);
            table->item(0, 2)->setSelected(true); table->item(4, 0)->setSelected(true);
            mask->findChild<QPushButton *>("disableSelectedChannels")->click();
            QVERIFY(mask->mask()[2]); QVERIFY(mask->mask()[64]);
            auto *maxSources = window.findChild<QSpinBox *>("maximumSources"); QVERIFY(maxSources); maxSources->setValue(6);
            auto *separation = window.findChild<QDoubleSpinBox *>("sourceSeparation"); QVERIFY(separation); separation->setValue(12.5);
            const auto report = window.pipeline().snapshot().json();
            QCOMPARE(report["disabled_processing_channels"].toArray().size(), 7);
            QVERIFY(report["channels"].toArray()[64].toObject()["excluded_from_processing"].toBool());
            // No raw channels disappear from diagnostics when excluded from DSP.
            QCOMPARE(report["channels"].toArray().size(), 128);
        }
        MainWindow restored;
        const auto mask = restored.findChild<ChannelMaskWidget *>()->mask();
        QCOMPARE(channelMaskText(mask), QStringLiteral("1-2,4-6,64,127"));
        QCOMPARE(restored.findChild<QSpinBox *>("maximumSources")->value(), 6);
        QCOMPARE(restored.findChild<QDoubleSpinBox *>("sourceSeparation")->value(), 12.5);
        auto *shortcut = restored.findChild<QPushButton *>("channelMaskShortcut"); QVERIFY(shortcut); shortcut->click();
        QCOMPARE(restored.findChild<QTabWidget *>("configurationTabs")->currentWidget()->objectName(), QStringLiteral("channelMaskPanel"));
    }
    void qualityGateSeparatesChannelAndGlobalFaults()
    {
        PipelineSnapshot snapshot;
        snapshot.latchedFlags = 1; // Aggregate hardware bit cannot identify a channel.
        QCOMPARE(snapshot.acousticFaultFlags(), 0u);
        QVERIFY(snapshot.acousticFaultReason().isEmpty());
        snapshot.clippedChannels[7] = true; snapshot.clippedChannels[127] = true;
        snapshot.quality[7].clipped = 3; snapshot.quality[127].clipped = 8;
        QCOMPARE(snapshot.acousticFaultFlags(), 1u);
        QVERIFY(snapshot.acousticFaultReason().contains("CH7"));
        snapshot.disabledProcessingChannels[7] = true;
        QCOMPARE(snapshot.acousticFaultFlags(), 1u);
        QVERIFY(!snapshot.acousticFaultReason().contains("CH7"));
        QVERIFY(snapshot.acousticFaultReason().contains("CH127"));
        snapshot.disabledProcessingChannels[127] = true;
        QCOMPARE(snapshot.acousticFaultFlags(), 0u);
        auto report = snapshot.json();
        QCOMPARE(report["latched_flags"].toInt(), 1); QCOMPARE(report["acoustic_fault_flags"].toInt(), 0);
        QVERIFY(report["acoustic_fault_channels"].toArray().isEmpty());
        QCOMPARE(report["channels"].toArray().size(), 128);
        const auto channel = report["channels"].toArray()[7].toObject();
        QVERIFY(channel["excluded_from_quality_gate"].toBool()); QVERIFY(channel["pcm_saturation_latched"].toBool());
        QCOMPARE(channel["clipped"].toString(), QStringLiteral("3")); // Mask does not destroy raw evidence.
        snapshot.disabledProcessingChannels.fill(true);
        for (quint32 global : {2u, 4u, 8u, 0x20u}) {
            snapshot.latchedFlags = global | 1;
            QCOMPARE(snapshot.acousticFaultFlags(), global);
            QVERIFY(snapshot.acousticFaultReason().contains(QStringLiteral("全局故障")));
        }
        snapshot.latchedFlags = 0;
        snapshot.disabledProcessingChannels[7] = false;
        QCOMPARE(snapshot.acousticFaultFlags(), 1u); // Detect PCM clipping even when hardware bit0 is absent.
        QCOMPARE(snapshot.json()["acoustic_fault_channels"].toArray(), QJsonArray{7});
    }
    void maskedFaultUiAndRecordingReadiness()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Geometry geometry;
        for (int ch = 0; ch < Channels; ++ch) geometry.microphones.append({(ch % 16) * .01, (ch / 16) * .01, 0});
        QSettings store(QSettings::IniFormat, QSettings::UserScope, "MicArray", "MicArray128Tests");
        store.setValue("geometry", QJsonDocument(geometry.json()).toJson());
        MainWindow window; window.resize(1120, 700); window.show();
        auto *denoise = window.findChild<QCheckBox *>("denoiseEnabled"); QVERIFY(denoise); denoise->setChecked(true);
        auto *source = window.findChild<QComboBox *>("recordSource"); QVERIFY(source); source->setCurrentIndex(1);
        window.findChild<QLineEdit *>("recordDirectory")->setText(directory.path());
        window.findChild<QListWidget *>("recordTracks")->item(0)->setCheckState(Qt::Checked);
        auto *record = window.findChild<QPushButton *>("recordButton"); QVERIFY(record);
        auto *quick = window.findChild<QPushButton *>("quickRecordButton"); QVERIFY(quick);
        auto *status = window.findChild<QLabel *>("denoiseStatus"); QVERIFY(status);
        QUdpSocket sender, reservation; QVERIFY(sender.bind(QHostAddress::LocalHost, 0)); QVERIFY(reservation.bind(QHostAddress::LocalHost, 0));
        const auto port = reservation.localPort(); reservation.close();
        window.pipeline().start({"127.0.0.1", port, "127.0.0.1", sender.localPort()});
        QTRY_VERIFY_WITH_TIMEOUT(window.pipeline().snapshot().listening, 1000);
        quint64 sequence = 0;
        quint32 flags = 1;
        auto send = [&] {
            auto bytes = packet(sequence++).bytes;
            qToBigEndian(flags, bytes.data() + 28);
            for (int r = 0; r < 3; ++r) {
                // Real-mode PCM with exactly one clipped channel; no acoustic
                // accuracy claim is made for the other constant samples.
                memset(bytes.data() + 40 + r * 392, 0x01, 384);
                char *sample = bytes.data() + 40 + r * 392 + 127 * 3;
                sample[0] = char(0xff); sample[1] = char(0xff); sample[2] = 0x7f;
            }
            sender.writeDatagram(bytes, QHostAddress::LocalHost, port);
        };
        QTimer stream; connect(&stream, &QTimer::timeout, &window, send); stream.start(20);
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("CH127"), 1000);
        QVERIFY(!record->isEnabled()); QVERIFY(!quick->isEnabled());
        auto *mask = window.findChild<ChannelMaskWidget *>(); QVERIFY(mask);
        auto *list = mask->findChild<QLineEdit *>("disabledChannelList");
        auto *apply = mask->findChild<QPushButton *>("applyChannelMask");
        list->setText("127"); apply->click();
        QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("处理中")), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(record->isEnabled() && quick->isEnabled(), 1000);
        QCOMPARE(window.pipeline().snapshot().runs, 1u);
        QCOMPARE(window.pipeline().snapshot().latchedFlags, 1u);
        // The raw evidence is visible, but explicitly excluded from quality gating.
        window.findChild<QTabWidget *>("mainPages")->setCurrentIndex(int(MainWindow::Page::Diagnostics));
        auto *tabs = window.findChild<QTabWidget *>("debugTabs"); tabs->setCurrentIndex(1);
        auto *table = window.findChild<QTableWidget *>("channelQualityTable");
        QTRY_VERIFY_WITH_TIMEOUT(table->item(127, 0)->text().contains(QStringLiteral("×")), 1000);
        QVERIFY(table->item(127, 6)->toolTip().contains(QStringLiteral("不参与")));
        QVERIFY(table->item(127, 6)->text().toULongLong() > 0);
        const auto screenshotDirectory = qEnvironmentVariable("M128_QUALITY_SCREENSHOT_DIR");
        if (!screenshotDirectory.isEmpty()) {
            table->scrollToItem(table->item(127, 0)); QTest::qWait(100);
            QVERIFY(window.grab().save(screenshotDirectory + "/masked-channel-quality.png"));
            tabs->setCurrentIndex(0); QTest::qWait(150);
            QVERIFY(window.grab().save(screenshotDirectory + "/masked-quality-overview.png"));
        }
        list->clear(); apply->click();
        QTRY_VERIFY_WITH_TIMEOUT(!record->isEnabled() && !quick->isEnabled(), 1000);
        QVERIFY(status->text().contains("CH127"));
        list->setText("127"); apply->click(); flags = 4;
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("全局故障")), 1000);
        QVERIFY(!record->isEnabled()); QVERIFY(!quick->isEnabled());
        source->setCurrentIndex(0); // Original recordings remain available as evidence.
        QTRY_VERIFY_WITH_TIMEOUT(record->isEnabled(), 1000);
        stream.stop(); window.pipeline().stop();
    }
    void malformedSavedMaskFailsClosed()
    {
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, "MicArray", "MicArray128Tests");
        settings.setValue("processing/disabledChannels", "0,128");
        MainWindow window;
        const auto mask = window.findChild<ChannelMaskWidget *>()->mask();
        QVERIFY(std::all_of(mask.begin(), mask.end(), [](bool value) { return value; }));
        auto *notice = window.findChild<QLabel *>("operationNoticeText"); QVERIFY(notice);
        QVERIFY(notice->text().contains(QStringLiteral("屏蔽列表无效")));
    }
    void audioUartFramingAndMixedStreams()
    {
        for (bool gain : {false, true}) {
            const auto line = audioUartWords(audioUartState(), gain);
            QCOMPARE(line.size(), gain ? 133 : 132);
            const auto frame = decodeAudioUartLine(line); QVERIFY(frame); QCOMPARE(frame->gainResponse, gain);
            QCOMPARE(frame->lastPcm(), -8388608); QVERIFY(frame->actualValid());
            for (auto bad : {line.left(line.size() - 1), line + 'x', line.toLower()}) QVERIFY(!decodeAudioUartLine(bad));
            auto bad = line; bad[gain ? 14 : 13] = '\t'; QVERIFY(!decodeAudioUartLine(bad));
            for (int split = 0; split <= line.size(); ++split) {
                Sys3Session session; session.connected = true; session.feed(uartLine(), 0);
                session.feed(line.left(split), 1); session.feed(line.mid(split) + audioUartWords(audioUartState(2), !gain) + uartLine(2, 500), 2);
                QCOMPARE(session.valid, 2u); QCOMPARE(session.audioValid, 2u); QCOMPARE(session.invalid, 0u);
                QCOMPARE(session.missing, 0u); QCOMPARE(session.audioMissing, 0u);
            }
        }
    }
    void audioUartEncodingAndUnknownActualValues()
    {
        AudioUartCommand command; QCOMPARE(command.encode(), QByteArray("@I2S?\n"));
        command.action = AudioUartAction::SelectI2s; command.channel = 127; QCOMPARE(command.encode(), QByteArray("@I2S=127\n"));
        command.channel = 128; QVERIFY(command.encode().isEmpty()); command.channel = -1; QVERIFY(command.encode().isEmpty());
        command.action = AudioUartAction::DisableI2s; QCOMPARE(command.encode(), QByteArray("@I2S=OFF\n"));
        command.action = AudioUartAction::SetGain; command.evenGain = 0; command.oddGain = 0x7fffff;
        QCOMPARE(command.encode(), QByteArray("@GAIN=000000,7FFFFF\n")); command.oddGain = 0x800000; QVERIFY(command.encode().isEmpty());
        auto words = audioUartState(); words[1] = 2;
        const auto frame = decodeAudioUartLine(audioUartWords(words)); QVERIFY(frame);
        const auto report = frame->json(); QVERIFY(report["actual_i2s"].isNull()); QVERIFY(report["actual_even_gain_q2_22"].isNull());
        QVERIFY(report["received_samples"].isNull()); QCOMPARE(report["requested_i2s"].toInt(), 128);
        words[1] = 5; auto currentFrame = decodeAudioUartLine(audioUartWords(words)); QVERIFY(currentFrame);
        QVERIFY(currentFrame->json(false)["last_pcm_s24"].isNull());
    }
    void audioUartSettingsNeedReadbackAndSerializeCommands()
    {
        Sys3Session session; session.connected = true; session.feed(uartLine(), 0); session.feed(audioUartWords(audioUartState()), 0);
        AudioUartCommand command; command.action = AudioUartAction::SelectI2s; command.channel = 127;
        QVERIFY(session.requestAudio(command, 1)); QCOMPARE(session.takeAudioCommand(2), QByteArray("@I2S=127\n")); session.audioCommandWritten(3);
        QVERIFY(!session.request('s', 4)); QVERIFY(!session.requestAudio(command, 4));
        auto reply = audioUartState(2); reply[2] = 255; reply[1] = 6; // pending, no fresh snapshot
        session.feed(audioUartWords(reply), 10); QVERIFY(session.audioPending);
        QVERIFY(session.takeAudioCommand(209).isEmpty()); QCOMPARE(session.takeAudioCommand(210), QByteArray("@I2S?\n"));
        session.audioCommandWritten(211); QVERIFY(session.takeAudioCommand(212).isEmpty());
        reply[0]++; reply[1] = 5; reply[3] = 255; session.feed(audioUartWords(reply), 220);
        QVERIFY(!session.audioPending); QVERIFY(session.audioCommandStatus.contains(QStringLiteral("已确认生效")));
        QVERIFY(session.request('s', 221));
    }
    void audioUartReadyInitialReplyStillRequiresLaterQuery()
    {
        Sys3Session session; session.connected = true; session.feed(uartLine(), 0); session.feed(audioUartWords(audioUartState()), 0);
        AudioUartCommand command; command.action = AudioUartAction::SetGain; command.evenGain = 0x200000; command.oddGain = 0x300000;
        QVERIFY(session.requestAudio(command, 1)); QCOMPARE(session.takeAudioCommand(2), QByteArray("@GAIN=200000,300000\n")); session.audioCommandWritten(3);
        auto reply = audioUartState(2); reply[4] = reply[6] = command.evenGain; reply[5] = reply[7] = command.oddGain;
        session.feed(audioUartWords(reply, true), 10); QVERIFY(session.audioPending); QCOMPARE(session.takeAudioCommand(210), QByteArray("@GAIN?\n"));
        session.audioCommandWritten(211); reply[0]++; session.feed(audioUartWords(reply, true), 220); QVERIFY(!session.audioPending);
        QCOMPARE(session.latest.commands(), 0); // Audio control does not use SYS3 command count.
    }
    void audioUartFailuresDoNotRetrySettings()
    {
        for (int failure = 0; failure < 6; ++failure) {
            Sys3Session session; session.connected = true; session.feed(uartLine(), 0); session.feed(audioUartWords(audioUartState()), 0);
            AudioUartCommand command; command.action = AudioUartAction::SelectI2s; command.channel = 127;
            QVERIFY(session.requestAudio(command, 1)); QVERIFY(!session.takeAudioCommand(2).isEmpty()); session.audioCommandWritten(3);
            auto reply = audioUartState(2); reply[2] = reply[3] = 255;
            if (failure == 0) reply[1] |= 1 << 8; // syntax error
            if (failure == 1) reply[1] |= 2 << 8; // range error
            if (failure == 2) reply[1] |= 1 << 16; // full response queue
            if (failure == 3) reply[2] = 128; // saved channel differs (historical first-bit FPGA defect)
            if (failure == 4) reply[3] = 128; // fresh actual differs on the follow-up query
            if (failure == 5) {
                session.feed(audioUartWords(reply), 5002); // late reply is evidence, never a successful command
            } else {
                session.feed(audioUartWords(reply), 10);
                if (failure == 4) {
                    QCOMPARE(session.takeAudioCommand(210), QByteArray("@I2S?\n")); session.audioCommandWritten(211);
                    reply[0]++; session.feed(audioUartWords(reply), 220);
                }
            }
            QVERIFY(!session.audioPending); QVERIFY(session.audioCommandStatus.contains(QStringLiteral("不自动重试")));
            QVERIFY(session.takeAudioCommand(230).isEmpty());
        }
    }
    void audioUartFreshnessInterlocksAndRestart()
    {
        Sys3Session session; session.connected = true; session.feed(uartLine(), 0);
        AudioUartCommand command; command.action = AudioUartAction::SetGain;
        QVERIFY(!session.requestAudio(command, 1)); // Must identify the new response format before changing hardware.
        auto words = audioUartState(0xffffffff); session.feed(audioUartWords(words), 2);
        words[0] = 0; session.feed(audioUartWords(words, true), 3); QCOMPARE(session.audioMissing, 0u);
        session.feed(audioUartWords(words), 1400); QCOMPARE(session.audioDuplicates, 1u); QVERIFY(!session.audioCurrent(1504));
        session.captureBusy = true; QVERIFY(!session.requestAudio(command, 1401)); session.captureBusy = false;
        QVERIFY(session.requestAudio(command, 1402)); session.captureBusy = true; QVERIFY(session.takeAudioCommand(1403).isEmpty());
        QVERIFY(!session.audioPending); session.captureBusy = false;
        command.action = AudioUartAction::QueryI2s; QVERIFY(session.requestAudio(command, 1404)); QVERIFY(!session.takeAudioCommand(1405).isEmpty());
        auto sys = decodeSys3Line(uartLine(2, 100))->words; session.feed(uartWords(sys), 1406);
        QVERIFY(!session.audioPending); QVERIFY(!session.audioIdentified); QCOMPARE(session.restarts, 1u);
        QVERIFY(session.json(1407)["audio_uart"].toObject()["latest"].toObject()["actual_i2s"].isNull());
    }
    void audioUartWrongRepliesAndDiscontinuitiesStayUnconfirmed()
    {
        for (int interruption = 0; interruption < 3; ++interruption) {
            Sys3Session session; session.connected = true; session.feed(uartLine(), 0); session.feed(audioUartWords(audioUartState()), 0);
            AudioUartCommand command; command.action = AudioUartAction::SelectI2s; command.channel = 127;
            QVERIFY(session.requestAudio(command, 1)); QCOMPARE(session.takeAudioCommand(2), QByteArray("@I2S=127\n"));
            session.audioCommandWritten(3);
            // A GAIN1 reply cannot acknowledge an I2S request, even if its data matches.
            auto reply = audioUartState(2); reply[2] = reply[3] = 255;
            session.feed(audioUartWords(reply, true), 10);
            QVERIFY(session.audioPending); QVERIFY(session.audioAwaitingReply); QVERIFY(session.takeAudioCommand(220).isEmpty());
            if (interruption == 0) {
                auto sys = decodeSys3Line(uartLine(2, 500))->words; sys[5] = 1 | (quint32('s') << 16);
                session.feed(uartWords(sys), 230);
                QVERIFY(session.audioCommandStatus.contains(QStringLiteral("外部干预")));
            } else if (interruption == 1) {
                session.receiveDiscontinuity(QStringLiteral("测试驱动中断"));
                QVERIFY(!session.audioCurrent(230)); QVERIFY(!session.audioIdentified);
                QVERIFY(session.json(230)["audio_uart"].toObject()["latest"].toObject()["actual_i2s"].isNull());
            } else {
                session.connected = false; session.failCommand(QStringLiteral("串口已断开"));
                QVERIFY(!session.audioCurrent(230));
                QVERIFY(session.json(230)["audio_uart"].toObject()["latest"].toObject()["actual_i2s"].isNull());
            }
            const auto failedStatus = session.audioCommandStatus;
            QVERIFY(!session.audioPending); QVERIFY(session.takeAudioCommand(240).isEmpty());
            reply[0]++; session.feed("\n" + audioUartWords(reply), 250);
            QCOMPARE(session.audioCommandStatus, failedStatus); // Late evidence never completes an invalidated request.
        }
    }
    void audioUartConfirmationUsesDefinedFieldBits()
    {
        for (bool gain : {false, true}) {
            Sys3Session session; session.connected = true; session.feed(uartLine(), 0); session.feed(audioUartWords(audioUartState()), 0);
            AudioUartCommand command; command.action = gain ? AudioUartAction::SetGain : AudioUartAction::SelectI2s;
            command.channel = 127; command.evenGain = 0x200000; command.oddGain = 0x300000;
            QVERIFY(session.requestAudio(command, 1)); QVERIFY(!session.takeAudioCommand(2).isEmpty()); session.audioCommandWritten(3);
            auto reply = audioUartState(2); reply[2] = 0x120000ff; reply[3] = 0x340000ff;
            reply[4] = 0x12200000; reply[5] = 0x34300000; reply[6] = 0x56200000; reply[7] = 0x78300000;
            session.feed(audioUartWords(reply, gain), 10); QVERIFY(session.audioPending);
            QCOMPARE(session.takeAudioCommand(210), command.queryBytes()); session.audioCommandWritten(211);
            reply[0]++; session.feed(audioUartWords(reply, gain), 220);
            QVERIFY(!session.audioPending); QVERIFY(session.audioCommandStatus.contains(QStringLiteral("已确认生效")));
            const auto json = session.latestAudio.json(); QCOMPARE(json["requested_i2s"].toInt(), 255); QCOMPARE(json["actual_i2s"].toInt(), 255);
            QCOMPARE(json["actual_even_gain_q2_22"].toInt(), 0x200000); QCOMPARE(json["raw_words_hex"].toArray()[6].toString(), QStringLiteral("56200000"));
        }
    }
    void uartAudioCompactLayoutKeepsControlsAndRowsVisible()
    {
        for (bool dark : {false, true}) {
            QSettings settings(QSettings::IniFormat, QSettings::UserScope, "MicArray", "MicArray128Tests"); settings.setValue("dark", dark);
            MainWindow window; window.resize(1120, 700); window.show();
            auto *panel = window.findChild<Sys3Panel *>(); QVERIFY(panel);
            window.findChild<QTabWidget *>("mainPages")->setCurrentIndex(int(MainWindow::Page::Diagnostics));
            window.findChild<QTabWidget *>("debugTabs")->setCurrentWidget(panel);
            panel->findChild<QTabWidget *>("sys3DetailsTabs")->setCurrentIndex(1); QTest::qWait(100);
            QCOMPARE(window.size(), QSize(1120, 700));
            auto *page = panel->findChild<QWidget *>("uartAudioPanel"); QVERIFY(page);
            for (const auto &name : {"uartQueryI2s", "uartApplyI2s", "uartDisableI2s", "uartQueryGain", "uartApplyGain",
                "uartI2sChannel", "uartEvenGain", "uartOddGain", "uartLoadRequested", "uartUnityPreset"}) {
                auto *control = panel->findChild<QWidget *>(name); QVERIFY(control); QVERIFY(control->isVisible());
                QVERIFY2(control->height() >= 24, name);
                QVERIFY2(page->rect().contains(QRect(control->mapTo(page, QPoint()), control->size())), name);
            }
            auto *values = panel->findChild<QTableWidget *>("uartAudioValues"); QVERIFY(values);
            QVERIFY(values->viewport()->rect().contains(values->visualItemRect(values->item(2, 2))));
            const auto directory = qEnvironmentVariable("M128_UART_SCREENSHOT_DIR");
            if (!directory.isEmpty()) QVERIFY(window.grab().save(directory + (dark ? "/uart-audio-compact-dark.png" : "/uart-audio-compact-light.png")));
        }
    }
    void uartAudioControlsAndDcFields()
    {
        Sys3Panel panel("COM3"); panel.resize(1060, 480); panel.show();
        for (const auto &name : {"uartQueryI2s", "uartApplyI2s", "uartDisableI2s", "uartQueryGain", "uartApplyGain"}) {
            auto *button = panel.findChild<QPushButton *>(name); QVERIFY(button); QVERIFY(!button->isEnabled());
        }
        auto *channel = panel.findChild<QSpinBox *>("uartI2sChannel"); QVERIFY(channel); QCOMPARE(channel->minimum(), 0); QCOMPARE(channel->maximum(), 127);
        auto *even = panel.findChild<QDoubleSpinBox *>("uartEvenGain"); QVERIFY(even); even->setValue(0);
        panel.findChild<QPushButton *>("uartUnityPreset")->click(); QCOMPARE(even->value(), 1.0);
        QCOMPARE(panel.findChild<QTableWidget *>("uartAudioValues")->rowCount(), 3);
        auto *source = panel.findChild<QComboBox *>("uartRawSource"); source->setCurrentIndex(1);
        QCOMPARE(panel.findChild<QTableWidget *>("sys3RawTable")->rowCount(), 14);
        for (int bit : {12, 13, 14}) {
            Sys3Frame frame; frame.words[8] = 1 << bit;
            const QString key = bit == 12 ? "dc_correction_enabled" : bit == 13 ? "dc_estimation_finished" : "dc_any_channel_rejected";
            QVERIFY(frame.json()[key].toBool());
        }
        panel.findChild<QTabWidget *>("sys3DetailsTabs")->setCurrentIndex(1); QTest::qWait(100);
        const auto directory = qEnvironmentVariable("M128_UART_SCREENSHOT_DIR");
        if (!directory.isEmpty()) QVERIFY(panel.grab().save(directory + "/uart-audio-controls.png"));
    }
    void audioUartVirtualSerialRoundTrip()
    {
        if (!qEnvironmentVariableIsSet("M128_UART_AUDIO_TEST_PORT")) QSKIP("Optional isolated com0com pair required");
        const auto port = qEnvironmentVariable("M128_UART_AUDIO_TEST_PORT");
        Sys3Client client; client.open(port);
        QTRY_VERIFY_WITH_TIMEOUT(client.snapshot().connected, 2000);
        QFile ready(qEnvironmentVariable("M128_UART_AUDIO_READY")); QVERIFY(ready.open(QIODevice::WriteOnly | QIODevice::NewOnly)); ready.write("ready"); ready.close();
        QTRY_VERIFY_WITH_TIMEOUT(client.snapshot().current(client.now()), 2000);
        AudioUartCommand command;
        auto send = [&] {
            QVERIFY(client.sendAudio(command));
            QTRY_VERIFY_WITH_TIMEOUT(!client.snapshot().audioPending, 2500);
            QVERIFY2(client.snapshot().audioCommandStatus.contains(QStringLiteral("查询完成"))
                || client.snapshot().audioCommandStatus.contains(QStringLiteral("已确认生效")), qPrintable(client.snapshot().audioCommandStatus));
        };
        send(); QCOMPARE(client.snapshot().latestAudio.words[2], 128u);
        command.action = AudioUartAction::SelectI2s; command.channel = 127; send(); QCOMPARE(client.snapshot().latestAudio.words[3], 255u);
        command.action = AudioUartAction::DisableI2s; send(); QCOMPARE(client.snapshot().latestAudio.words[3], 127u);
        command.action = AudioUartAction::QueryGain; send();
        command.action = AudioUartAction::SetGain; command.evenGain = 0x200000; command.oddGain = 0x300000; send();
        QCOMPARE(client.snapshot().latestAudio.words[6], 0x200000u); QCOMPARE(client.snapshot().latestAudio.words[7], 0x300000u);
        QCOMPARE(client.snapshot().invalid, 0u); QCOMPARE(client.snapshot().latest.commands(), 0);
        client.close();
        MainWindow window; window.resize(1120, 700); window.show(); window.openSerial(port);
        auto *panel = window.findChild<Sys3Panel *>(); QVERIFY(panel);
        window.findChild<QTabWidget *>("mainPages")->setCurrentIndex(int(MainWindow::Page::Diagnostics));
        window.findChild<QTabWidget *>("debugTabs")->setCurrentWidget(panel);
        panel->findChild<QTabWidget *>("sys3DetailsTabs")->setCurrentIndex(1);
        auto *query = panel->findChild<QPushButton *>("uartQueryGain"); QTRY_VERIFY_WITH_TIMEOUT(query->isEnabled(), 2000); query->click();
        auto *load = panel->findChild<QPushButton *>("uartLoadRequested"); QTRY_VERIFY_WITH_TIMEOUT(load->isEnabled(), 2000); load->click();
        QCOMPARE(panel->findChild<QSpinBox *>("uartI2sChannel")->value(), 127);
        QCOMPARE(panel->findChild<QDoubleSpinBox *>("uartEvenGain")->value(), .5);
        QCOMPARE(panel->findChild<QDoubleSpinBox *>("uartOddGain")->value(), .75);
        auto *actual = panel->findChild<QTableWidget *>("uartAudioValues"); QVERIFY(actual->item(0, 2)->text().contains(QStringLiteral("静音")));
        QVERIFY(actual->viewport()->rect().contains(actual->visualItemRect(actual->item(2, 2))));
        QVERIFY(panel->findChild<QPushButton *>("uartApplyGain")->height() >= 24);
        auto *dc = panel->findChild<QLabel *>("uartDcStatus"); QVERIFY(dc->text().contains(QStringLiteral("拒绝通道")));
        const auto directory = qEnvironmentVariable("M128_UART_AUDIO_EVIDENCE");
        QVERIFY(window.grab().save(directory + "/uart-audio-live-1120.png"));
        window.resize(1440, 900); QTest::qWait(100); QVERIFY(window.grab().save(directory + "/uart-audio-live-1440.png"));
        window.saveDiagnosticReport(directory + "/report.json");
    }
    void sys3VirtualSerialRoundTrip()
    {
        if (!qEnvironmentVariableIsSet("M128_SYS3_TEST_PORT")) QSKIP("Optional: isolated com0com pair required");
        Sys3Client client; client.open(qEnvironmentVariable("M128_SYS3_TEST_PORT"));
        QTRY_VERIFY_WITH_TIMEOUT(client.snapshot().connected, 2000);
        if (qEnvironmentVariableIsSet("M128_SYS3_TEST_READY")) {
            QFile ready(qEnvironmentVariable("M128_SYS3_TEST_READY")); QVERIFY(ready.open(QIODevice::WriteOnly | QIODevice::NewOnly)); ready.write("ready"); ready.close();
        }
        QTRY_VERIFY_WITH_TIMEOUT(client.snapshot().valid >= 2, 2500);
        QVERIFY(client.snapshot().current(client.now()));
        for (char command : QByteArray("shrtc")) {
            QVERIFY2(client.send(command), qPrintable(client.snapshot().commandStatus));
            QTRY_VERIFY_WITH_TIMEOUT(!client.snapshot().pending, 4000);
            const auto result = client.snapshot();
            QVERIFY2(result.acknowledged, qPrintable(result.commandStatus));
            QVERIFY2(!result.commandStatus.contains(QStringLiteral("未知")), qPrintable(result.commandStatus));
            QVERIFY2(!result.commandStatus.contains(QStringLiteral("超时")), qPrintable(result.commandStatus));
            QCOMPARE(result.latest.lastCommand(), command);
        }
        QVERIFY(client.snapshot().latest.selftest());
        QElapsedTimer closing; closing.start(); client.close(); QVERIFY(closing.elapsed() < 1000);
        QVERIFY(!client.snapshot().current(client.now()));
    }
    void sys3HistoricalHardwareFrame()
    {
        QFile file(QStringLiteral(M128_TEST_ROOT "/tests/fixtures/sys3_historical.txt")); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto f = decodeSys3Line(file.readAll().trimmed() + "\r\n"); QVERIFY(f);
        QCOMPARE(f->words[0], 11211u); QCOMPARE(f->commands(), quint16(8)); QCOMPARE(f->lastCommand(), 't');
        QVERIFY(f->clocksHealthy()); QVERIFY(f->selftest()); QVERIFY(f->audioFresh()); QVERIFY(f->ethFresh()); QVERIFY(f->rxFresh());
        QCOMPARE(f->words[13], 0x001cc916u); QVERIFY((f->words[20] & 0x3c) == 0x2c);
        QCOMPARE(f->words[33], 0x946ea561u); QVERIFY(f->words[21] & (1 << 13));
        QVERIFY(f->json()["gigabit_full_duplex"].toBool());
    }
    void sys3VirtualSerialBacklogRecovery()
    {
        if (!qEnvironmentVariableIsSet("M128_SYS3_BACKLOG_TEST")) QSKIP("Optional: isolated com0com backlog fixture required");
        Sys3Client client; client.open(qEnvironmentVariable("M128_SYS3_TEST_PORT"));
        QTRY_VERIFY_WITH_TIMEOUT(client.snapshot().connected, 2000);
        QFile ready(qEnvironmentVariable("M128_SYS3_TEST_READY"));
        QVERIFY(ready.open(QIODevice::WriteOnly | QIODevice::NewOnly)); ready.write("ready"); ready.close();
        QTRY_VERIFY_WITH_TIMEOUT(client.snapshot().receiveDiscontinuities >= 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(client.snapshot().valid >= 2 && client.snapshot().current(client.now()), 5000);
        QVERIFY(!client.snapshot().receiveBacklog);
        QCOMPARE(client.snapshot().latest.commands(), quint16(0)); // Recovery never transmits automatically.
        QVERIFY(client.send('s'));
        QTRY_VERIFY_WITH_TIMEOUT(!client.snapshot().pending, 4000);
        QVERIFY(client.snapshot().acknowledged);
        QCOMPARE(client.snapshot().latest.lastCommand(), 's');
        client.close(); QVERIFY(!client.snapshot().connected);
    }
    void sys3StrictFraming()
    {
        const auto line = uartLine(); QCOMPARE(line.size(), 312);
        const auto f = decodeSys3Line(line); QVERIFY(f); QVERIFY(f->clocksHealthy()); QVERIFY(f->audioFresh());
        QCOMPARE(f->words[13], 0x001cc916u); QCOMPARE(sys3WordNames().size(), 34);
        for (auto bad : {line.left(311), line + "x", QByteArray("CLK2") + line.mid(4), line.toLower()}) QVERIFY(!decodeSys3Line(bad));
        auto bad = line; bad[13] = '\t'; QVERIFY(!decodeSys3Line(bad)); bad = line; bad[5] = 'G'; QVERIFY(!decodeSys3Line(bad));
        for (int split = 0; split <= line.size(); ++split) {
            Sys3Session session; session.connected = true;
            session.feed(line.left(split), 0); session.feed(line.mid(split) + uartLine(2, 500), 10);
            QCOMPARE(session.valid, 2u); QCOMPARE(session.invalid, 0u); QVERIFY(session.current(20));
        }
    }
    void sys3ResynchronizesAndBoundsLogs()
    {
        Sys3Session session; session.connected = true;
        session.feed(QByteArray(100000, 'x'), 0); session.feed("\n" + uartLine(), 1);
        QCOMPARE(session.valid, 1u); QCOMPARE(session.invalid, 1u);
        for (int i = 2; i < 700; ++i) session.feed(uartLine(i, i * 250), i);
        QCOMPARE(session.log.size(), 400); QVERIFY(session.current(700));
        session.feed("CLK2 incompatible\r\n", 701); QVERIFY(!session.current(701));
        QVERIFY(!session.request('r', 701)); session.feed(uartLine(701, 701 * 250), 702); QVERIFY(session.current(702));
    }
    void sys3FreshnessWrapAndRestart()
    {
        Sys3Session session; session.connected = true;
        session.feed(uartLine(0xffffffffu, 0xffffff00u), 0);
        session.feed(uartLine(0, 0), 250); QCOMPARE(session.restarts, 0u); QCOMPARE(session.missing, 0u);
        session.feed(uartLine(0, 0), 1700); QCOMPARE(session.duplicates, 1u);
        QVERIFY(session.current(1750)); QVERIFY(!session.current(1751));
        session.feed(uartLine(10, 2500), 1800); QCOMPARE(session.missing, 9u);
        auto words = decodeSys3Line(uartLine(11, 2750))->words; words[7] = 2;
        session.feed(uartWords(words), 1900); const auto report = session.json(2000);
        QVERIFY(!report["audio_current"].toBool()); QVERIFY(report["eth_current"].toBool()); QVERIFY(!report["rx_current"].toBool());
        QVERIFY(!session.request('t', 2000)); QVERIFY(session.request('r', 2000)); session.queued = false;
        session.feed(uartLine(1, 100), 2100); QCOMPARE(session.restarts, 1u); QVERIFY(!session.pending);
        session.connected = false; QVERIFY(!session.json(2101)["current"].toBool());
    }
    void sys3CommandAcknowledgementAndTimeout()
    {
        Sys3Session s; QVERIFY(!s.request('s', 0)); s.connected = true; s.feed(uartLine(), 0);
        QVERIFY(!s.request('x', 0)); QVERIFY(s.request('s', 0)); QVERIFY(!s.request('r', 10)); s.queued = false;
        auto w = decodeSys3Line(uartLine(2, 500))->words; w[5] = 1 | (quint32('s') << 16);
        s.feed(uartWords(w), 250); QVERIFY(!s.pending); QVERIFY(s.acknowledged);
        QVERIFY(s.request('h', 251)); s.queued = false; s.tick(5252); QVERIFY(!s.pending);
        QVERIFY(s.commandStatus.contains(QStringLiteral("超时"))); QVERIFY(!s.request('r', 5253));
        s.feed(uartLine(3, 750), 6000); QVERIFY(s.request('c', 6001)); s.queued = false;
        w = decodeSys3Line(uartLine(4, 1000))->words; w[5] = 2 | (quint32('t') << 16);
        s.feed(uartWords(w), 6250); QVERIFY(!s.pending); QVERIFY(s.commandStatus.contains(QStringLiteral("不匹配")));
    }
    void sys3RechecksBeforeSending()
    {
        Sys3Session s; s.connected = true; s.feed(uartLine(), 0);
        QVERIFY(s.request('t', 1)); auto w = decodeSys3Line(uartLine(2, 500))->words; w[7] = 2;
        s.feed(uartWords(w), 10); QCOMPARE(s.takeCommand(11), char(0)); QVERIFY(!s.pending);
        QVERIFY(s.request('s', 12)); QCOMPARE(s.takeCommand(220), char(0)); QVERIFY(!s.pending);
        QVERIFY(s.request('h', 221)); QCOMPARE(s.takeCommand(222), 'h'); QCOMPARE(s.takeCommand(223), char(0));
    }
    void sys3ModeActionNeedsLaterFreshSnapshot()
    {
        Sys3Session s; s.connected = true;
        auto w = decodeSys3Line(uartLine())->words; w[5] = 65535; s.feed(uartWords(w), 0);
        QVERIFY(s.request('t', 1)); s.queued = false;
        w[0]++; w[1] += 250; w[5] = quint32('t') << 16; w[8] |= 8; w[27] = 3;
        s.feed(uartWords(w), 250); QVERIFY(s.pending); QVERIFY(s.acknowledged);
        w[0]++; w[7] = 0; s.feed(uartWords(w), 500); QVERIFY(s.pending);
        w[0]++; w[7] = 7; w[27] = 2; s.feed(uartWords(w), 750); QVERIFY(s.pending);
        w[0]++; w[27] = 3; s.feed(uartWords(w), 1000); QVERIFY(s.pending); // Old loaded mode is not completion.
        w[0]++; w[8] |= 0x800; w[8] &= ~0x200u; s.feed(uartWords(w), 1100); QVERIFY(s.pending);
        w[0]++; w[8] |= 0x200; w[2] |= 1 << 19; s.feed(uartWords(w), 1200); QVERIFY(s.pending);
        w[0]++; w[2] &= ~(1 << 19); s.feed(uartWords(w), 1300); QVERIFY(!s.pending);
        QVERIFY(s.commandStatus.contains(QStringLiteral("已确认模式切换")));
        s.lastToggleMs = 1250; QVERIFY(!s.request('t', 1301));
    }
    void sys3ResetAndClearDoNotOverclaim()
    {
        Sys3Session s; s.connected = true; s.feed(uartLine(), 0); QVERIFY(s.request('r', 1)); s.queued = false;
        auto w = decodeSys3Line(uartLine(2, 500))->words; w[5] = 1 | (quint32('r') << 16);
        s.feed(uartWords(w), 250); QVERIFY(s.pending);
        w[0]++; w[2] |= 1 << 19; s.feed(uartWords(w), 500); QVERIFY(s.pending);
        w[0]++; w[2] &= ~(1 << 19); s.feed(uartWords(w), 750); QVERIFY(!s.pending);
        QVERIFY(s.commandStatus.contains(QStringLiteral("非复位脉宽测量")));
        QVERIFY(s.request('c', 751)); s.queued = false;
        w[0]++; w[5] = 2 | (quint32('c') << 16); w[2] |= 1 << 15; s.feed(uartWords(w), 1000); QVERIFY(s.pending);
        w[0]++; w[2] &= ~(1 << 15); w[2] |= 1 << 19; w[8] |= 16; w[18] = 123;
        s.feed(uartWords(w), 1250); QVERIFY(!s.pending); QVERIFY(!s.latest.clocksHealthy());
        QCOMPARE(s.latest.words[18], 123u); QVERIFY(s.latest.words[8] & 16);
    }
    void sys3ClosedTransportCannotTransmit()
    {
        Sys3Client client; QVERIFY(!client.running()); QVERIFY(!client.send('r'));
        client.open("../../arbitrary-file"); QVERIFY(!client.running()); QVERIFY(!client.snapshot().error.isEmpty());
        QVERIFY(!client.send('s')); client.close(); QVERIFY(!client.snapshot().connected);
    }
    void sys3LateReplyCannotCompleteCommand()
    {
        for (char command : QByteArray("shrtc")) {
            Sys3Session s; s.connected = true; s.feed(uartLine(), 0);
            QVERIFY(s.request(command, 1)); QCOMPARE(s.takeCommand(2), command);
            auto w = decodeSys3Line(uartLine(2, 500))->words;
            w[5] = 1 | (quint32(command) << 16);
            s.feed(uartWords(w), 5002); // Deadline must be checked before accepting this reply.
            QVERIFY(!s.pending); QVERIFY(!s.acknowledged);
            QVERIFY2(s.commandStatus.contains(QStringLiteral("超时")), qPrintable(s.commandStatus));
        }
        Sys3Session s; s.connected = true; s.feed(uartLine(), 0);
        QVERIFY(s.request('c', 1)); QCOMPARE(s.takeCommand(2), 'c');
        auto w = decodeSys3Line(uartLine(2, 500))->words; w[5] = 1 | (quint32('c') << 16);
        s.feed(uartWords(w), 250); QVERIFY(s.acknowledged); QVERIFY(s.pending);
        w[0]++; w[1] += 250; s.feed(uartWords(w), 5002);
        QVERIFY(!s.pending); QVERIFY(s.commandStatus.contains(QStringLiteral("未确认后续状态")));
    }
    void sys3PreSendContextMustNotChange()
    {
        for (int change = 0; change < 4; ++change) {
            Sys3Session s; s.connected = true; s.feed(uartLine(), 0);
            QVERIFY(s.request('t', 1));
            auto w = decodeSys3Line(uartLine(2, 500))->words;
            if (change == 0) w[5] = 1 | (quint32('h') << 16);
            if (change == 1) w[8] |= 8;
            if (change == 2) w[27]++;
            if (change == 3) w[7] &= ~2u;
            s.feed(uartWords(w), 10);
            QCOMPARE(s.takeCommand(11), char(0)); QVERIFY(!s.pending);
        }
        Sys3Session s; s.connected = true;
        auto w = decodeSys3Line(uartLine())->words; w[7] = 1;
        s.feed(uartWords(w), 0); QVERIFY(!s.request('t', 1));
        QVERIFY(s.request('s', 1)); QCOMPARE(s.takeCommand(2), 's');
    }
    void sys3ObserverRestartWithReusedSequence()
    {
        for (quint32 newSequence : {10u, 11u}) {
            Sys3Session s; s.connected = true; s.feed(uartLine(10, 10000), 0);
            QVERIFY(s.request('s', 1)); QCOMPARE(s.takeCommand(2), 's');
            auto w = decodeSys3Line(uartLine(newSequence, 250))->words;
            w[5] = 1 | (quint32('s') << 16);
            s.feed(uartWords(w), 250);
            QCOMPARE(s.restarts, 1u); QVERIFY(!s.pending); QVERIFY(!s.acknowledged);
            QVERIFY(s.commandStatus.contains(QStringLiteral("观察域重启")));
        }
    }
    void sys3ToggleCooldownUsesWireTime()
    {
        Sys3Session s; s.connected = true; s.feed(uartLine(), 0);
        QVERIFY(s.request('t', 1));
        QCOMPARE(s.takeCommand(190), 't');
        s.commandWritten(230); // Slow dispatch/write must not consume the cooldown.
        auto w = decodeSys3Line(uartLine(2, 500))->words;
        w[5] = 1 | (quint32('t') << 16); w[8] |= 0x808; w[27]++;
        s.feed(uartWords(w), 240); QVERIFY(s.acknowledged); QVERIFY(s.pending);
        w[0]++; w[1] += 250; s.feed(uartWords(w), 250); QVERIFY(!s.pending);
        QVERIFY(!s.request('t', 251));
        QVERIFY(!s.request('t', 429));
        QVERIFY(s.request('t', 430)); QCOMPARE(s.takeCommand(430), 't');

        Sys3Session cancelled; cancelled.connected = true; cancelled.feed(uartLine(), 0);
        QVERIFY(cancelled.request('t', 1)); QCOMPARE(cancelled.takeCommand(202), char(0));
        QCOMPARE(cancelled.lastToggleMs, qint64(-1000)); // Unsent requests do not start cooldown.
        QVERIFY(cancelled.request('t', 203)); QCOMPARE(cancelled.takeCommand(204), 't');
    }
    void sys3ByteLossCannotJoinDifferentFrames()
    {
        for (int split : {5, 40, 100, 300, 311}) {
            Sys3Session s; s.connected = true; s.feed(uartLine(), 0);
            QVERIFY(s.request('s', 1)); QCOMPARE(s.takeCommand(2), 's');
            auto w = decodeSys3Line(uartLine(3, 750))->words;
            w[5] = 1 | (quint32('s') << 16);
            s.feed(uartLine(2, 500).left(split), 10);
            s.receiveDiscontinuity(QStringLiteral("模拟驱动丢字节"));
            QVERIFY(!s.pending); QVERIFY(!s.current(11));
            s.feed(uartWords(w).mid(split), 12);
            QCOMPARE(s.valid, quint64(1)); QVERIFY(!s.current(12));
            QVERIFY(!s.acknowledged);
            s.feed(uartLine(4, 1000), 13);
            QCOMPARE(s.valid, quint64(2)); QVERIFY(s.current(13));
            QCOMPARE(s.receiveDiscontinuities, quint64(1));
            QVERIFY(s.commandStatus.contains(QStringLiteral("未知")));
        }
    }
    void sys3ReceiveBacklogBlocksCommands()
    {
        Sys3Session s; s.connected = true; s.feed(uartLine(), 0);
        QVERIFY(s.request('t', 1));
        s.receiveBacklog = true;
        QVERIFY(!s.current(2)); QCOMPARE(s.takeCommand(2), char(0));
        for (char command : QByteArray("shrtc")) QVERIFY(!s.request(command, 3));
        QVERIFY(s.json(3)["receive_backlog"].toBool());
        QVERIFY(!s.json(3)["audio_current"].toBool());
        s.receiveDiscontinuity(QStringLiteral("接收积压"));
        s.feed("discarded tail\r\n" + uartLine(2, 500), 4);
        QVERIFY(!s.current(4)); // A syntactically valid buffered frame is still not current.
        s.receiveBacklog = false;
        QVERIFY(s.current(5)); QVERIFY(s.request('s', 5));
        QCOMPARE(s.json(5)["receive_discontinuities"].toString(), QStringLiteral("1"));
    }
    void sys3AdditionalStatusBitfields()
    {
        for (int value = 0; value < 32; ++value) {
            Sys3Frame f; f.words[2] = quint32(value) << 6; f.words[8] = quint32(31 - value) << 19;
            const auto report = f.json();
            QCOMPARE(report["observer_controller_state"].toInt(), value);
            QCOMPARE(report["audio_controller_state"].toInt(), 31 - value);
            const auto rows = sys3Details(f);
            QVERIFY(rows.contains(QStringList{QStringLiteral("控制器状态（观察域）"), QString::number(value), "0"}));
            QVERIFY(rows.contains(QStringList{QStringLiteral("控制器状态（音频域）"), QString::number(31 - value), "1"}));
        }
        const QStringList keys {"tx_enabled", "packet_full", "arp_reply_pending", "tx_draining"};
        const int bits[] {0, 3, 5, 6};
        for (int bit = 0; bit < 32; ++bit) {
            Sys3Frame f; f.words[21] = quint32(1) << bit;
            for (int i = 0; i < keys.size(); ++i) QCOMPARE(f.json()[keys[i]].toBool(), bit == bits[i]);
        }
        Sys3Frame f; f.words[7] = 0x57;
        QCOMPARE(f.json()["snapshot_waiting_mask"].toInt(), 5);
        QCOMPARE(f.json()["fresh_mask"].toInt(), 7);
    }
    void sys3CaptureInterlockAlsoAppliesAtDispatch()
    {
        for (char command : QByteArray("rt")) {
            Sys3Session s; s.connected = true; s.feed(uartLine(), 0);
            s.captureBusy = true; QVERIFY(!s.request(command, 1));
            s.captureBusy = false; QVERIFY(s.request(command, 2));
            s.captureBusy = true; QCOMPARE(s.takeCommand(3), char(0)); QVERIFY(!s.pending);
            QVERIFY(s.json(3)["capture_busy"].toBool());
            QVERIFY(s.request('s', 4)); QCOMPARE(s.takeCommand(5), 's'); // Read-only is allowed.
        }
        Sys3Client client; client.setCaptureBusy(true);
        client.open("invalid"); // Reopening a port must not drop the recording interlock.
        QVERIFY(client.snapshot().captureBusy);
    }
    void referencePacket()
    {
        const auto p = decodeDatagram(fixture()); QVERIFY(p);
        QCOMPARE(p->sequence, 0u); QCOMPARE(p->epoch, 1u); QCOMPARE(p->flags, 16u);
        QCOMPARE(p->frameIds, (std::array<quint64, 3>{96, 97, 98}));
        QCOMPARE(p->sample(0, 0), -3248570); QCOMPARE(p->sample(0, 1), -5381647);
        QCOMPARE(p->sample(0, 2), -5010411); QCOMPARE(p->sample(0, 3), -2633031);
        QByteArray extremes = QByteArray::fromHex("000080ffffff000000ffff7f");
        QCOMPARE(readS24(extremes.data()), -8388608); QCOMPARE(readS24(extremes.data() + 3), -1);
        QCOMPARE(readS24(extremes.data() + 6), 0); QCOMPARE(readS24(extremes.data() + 9), 8388607);
    }
    void rejectInvalidDatagrams()
    {
        auto good = fixture();
        QVERIFY(!decodeDatagram(good.first(1207))); QVERIFY(!decodeDatagram(good + "x"));
        for (int offset : {0, 4, 5, 6, 7, 8, 12, 14}) {
            auto bad = good; bad[offset] = char(uchar(bad[offset]) ^ 1); QVERIFY(!decodeDatagram(bad));
        }
        auto bad = good; qToBigEndian<quint32>(0x40, bad.data() + 28); QVERIFY(!decodeDatagram(bad));
        qToBigEndian<quint32>(0x20, bad.data() + 28); QVERIFY(!decodeDatagram(bad));
        qToBigEndian<quint32>(0x30, bad.data() + 28); QVERIFY(decodeDatagram(bad));
    }
    void sequenceAudit()
    {
        SequenceTracker tracker;
        QCOMPARE(tracker.observe(100), SequenceTracker::Event::First);
        tracker.observe(103); QCOMPARE(tracker.missing(), 2u);
        QCOMPARE(tracker.observe(101), SequenceTracker::Event::Reordered); QCOMPARE(tracker.missing(), 1u);
        tracker.observe(102); QCOMPARE(tracker.missing(), 0u);
        QCOMPARE(tracker.observe(101), SequenceTracker::Event::Duplicate);
        QCOMPARE(tracker.observe(99), SequenceTracker::Event::BeforeStart);
        QCOMPARE(tracker.forwardGaps, 2u);
        SequenceTracker wrap;
        wrap.observe(~quint64(0) - 1); wrap.observe(~quint64(0));
        QCOMPARE(wrap.observe(0), SequenceTracker::Event::Continuous); QCOMPARE(wrap.missing(), 0u);
    }
    void digitalSelftest()
    {
        SelftestAudit audit(selftestReference()); QVERIFY(audit.referenceValid);
        for (int i = 0; i < 16000; ++i) audit.observe(packet(i), true, false);
        QCOMPARE(audit.frames, 48000u); QVERIFY(audit.status().startsWith("PASS"));
        auto bad = packet(16000); bad.bytes[40] ^= 1; audit.observe(bad, true, false);
        QCOMPARE(audit.mismatchingSamples, 1u); QCOMPARE(audit.status(), QStringLiteral("FAIL"));
        SelftestAudit lost(selftestReference()); lost.observe(packet(0), true, true); QCOMPARE(lost.status(), QStringLiteral("FAIL"));
        SelftestAudit normal(selftestReference()); auto p = packet(0); p.flags = 0; normal.observe(p, true, false); QCOMPARE(normal.frames, 0u);
    }
    void geometryValidation()
    {
        Geometry original; QString error;
        QVERIFY(!Geometry::load("{}", &original, &error));
        QJsonArray points;
        for (int i = 0; i < 128; ++i) points.append(QJsonObject{{"channel", i}, {"x", (i % 16) * .01}, {"y", (i / 16) * .01}, {"z", 0}});
        QJsonObject object {{"format", "micarray128-geometry-v1"}, {"units", "m"}, {"name", "test-only"}, {"microphones", points}};
        QVERIFY(Geometry::load(QJsonDocument(object).toJson(), &original, &error)); QVERIFY(original.valid());
        auto duplicate = points; duplicate[127] = duplicate[0]; object["microphones"] = duplicate;
        Geometry unchanged = original; QVERIFY(!Geometry::load(QJsonDocument(object).toJson(), &original, &error));
        QCOMPARE(original.microphones, unchanged.microphones);
    }
    void beamformerContinuity()
    {
        Geometry geometry;
        for (int i = 0; i < 128; ++i) geometry.microphones.append({(i % 16 - 7.5) * .01, (i / 16 - 3.5) * .01, 0});
        QVector<QVector<float>> input(128, QVector<float>(2048));
        for (auto &channel : input) for (int n = 0; n < 2048; ++n) channel[n] = float(.2 * std::sin(n * .1));
        Beamformer all, chunked; all.configure(geometry, {{{0, 0}, {15.2, 0}, {-30.1, 4.2}}}, 343);
        chunked.configure(geometry, {{{0, 0}, {15.2, 0}, {-30.1, 4.2}}}, 343);
        const auto full = all.process(input); std::array<QVector<float>, 3> joined;
        for (int start = 0; start < 2048; start += 512) {
            QVector<QVector<float>> block; for (const auto &channel : input) block.append(channel.mid(start, 512));
            auto part = chunked.process(block); for (int i = 0; i < 3; ++i) joined[i] += part[i];
        }
        QCOMPARE(full, joined);
    }
    void syntheticSpiralLocalization()
    {
        ProcessingSettings settings;
        const double az = 13.25 * M_PI / 180, el = -8.5 * M_PI / 180;
        for (int ch = 0; ch < 128; ++ch) {
            const double radius = .015 * std::sqrt(ch + 1.0), theta = ch * 2.399963229728653;
            settings.geometry.microphones.append({radius * std::cos(theta), radius * std::sin(theta), 0});
        }
        QVector<QVector<float>> channels(128, QVector<float>(2048));
        for (int ch = 0; ch < 128; ++ch) {
            const auto &p = settings.geometry.microphones[ch];
            const double tau = (p.xMeters * std::sin(az) * std::cos(el) + p.yMeters * std::sin(el)) * 48000 / 343;
            for (int n = 0; n < 2048; ++n) for (int k = 0; k < 24; ++k)
                channels[ch][n] += float(.008 * std::sin(2 * M_PI * (361 + k * 173) * (n + tau) / 48000 + k * 1.713));
        }
        const auto result = localize(channels, settings);
        QVERIFY2(result.trusted, qPrintable(result.reason + " " + QString::number(result.confidence)));
        QVERIFY2(std::hypot(result.direction.azimuthDegrees - 13.25, result.direction.elevationDegrees + 8.5) < 2,
            qPrintable(QString("%1/%2").arg(result.direction.azimuthDegrees).arg(result.direction.elevationDegrees)));
        for (auto &channel : channels) channel.fill(0);
        QVERIFY(!localize(channels, settings).trusted);
    }
    void directionUpdatesPreserveDspHistory()
    {
        ProcessingSettings settings;
        settings.denoise = true;
        for (int i = 0; i < 128; ++i) settings.geometry.microphones.append({(i % 16 - 7.5) * .01, (i / 16 - 3.5) * .01, 0});
        QVector<QVector<float>> block(128, QVector<float>(512));
        for (auto &channel : block) for (int n = 0; n < 512; ++n) channel[n] = float(.2 * std::sin(n * .071));
        Denoiser denoiser; denoiser.configure(settings);
        QVERIFY(denoiser.process(block)[3].isEmpty());
        QCOMPARE(denoiser.process(block)[3].size(), 512);
        const auto previous = settings;
        settings.directions[0].azimuthDegrees += .012;
        QVERIFY(onlyDirectionChange(previous, settings));
        denoiser.configure(settings);
        QCOMPARE(denoiser.process(block)[3].size(), 512);
        settings.coefficientB1 += 1;
        QVERIFY(!onlyDirectionChange(previous, settings));
        denoiser.configure(settings);
        QVERIFY(denoiser.process(block)[3].isEmpty());
    }
    void asynchronousRecording()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Recorder recorder; RecordingSettings settings; settings.directory = directory.path(); settings.tracks = {0, 127};
        QVERIFY(recorder.begin(settings));
        QVector<CaptureRecord> records;
        for (int i = 0; i < 400; ++i) records.append({packet(i), quint64(i) * 62500, 1, i != 0});
        recorder.enqueue(records); recorder.finish();
        QTRY_VERIFY_WITH_TIMEOUT(!recorder.snapshot().finishing, 10000);
        auto state = recorder.snapshot(); QVERIFY2(state.error.isEmpty(), qPrintable(state.error)); QCOMPARE(state.frames, 1200u);
        QFile manifest(state.directory + "/manifest.json"); QVERIFY(manifest.open(QIODevice::ReadOnly));
        auto json = QJsonDocument::fromJson(manifest.readAll()).object(); QVERIFY(json["complete"].toBool());
        MicArray::WaveFileReader reader; QString error;
        QVERIFY2(reader.open(state.directory + "/part0001_CH000.wav", &error), qPrintable(error));
        QCOMPARE(reader.info().channelCount, 1); QCOMPARE(reader.info().containerBits, 24); QCOMPARE(reader.info().totalSamples, 1200);
        auto samples = reader.readChannels(0, 3); QVERIFY(std::abs(samples[0][0] - (-3248570 / 8388608.0)) < 1e-6);
        QFile evidence(state.directory + "/packets.m128"); QCOMPARE(evidence.size(), 8 + 400 * 1224);
    }
    void recordingSeparatesLateEpochPackets()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Recorder recorder; RecordingSettings settings; settings.directory = directory.path();
        QVERIFY(recorder.begin(settings));
        auto first = packet(0), late = packet(1), resumed = packet(2);
        first.epoch = resumed.epoch = 2;
        qToBigEndian<quint32>(2, first.bytes.data() + 24);
        qToBigEndian<quint32>(2, resumed.bytes.data() + 24);
        // Late epoch-1 evidence uses the current host run. Even sequential frame IDs
        // must not make the next current-epoch packet join that stale WAV segment.
        recorder.enqueue({{first, 0, 2, false}, {late, 62500, 2, false}, {resumed, 125000, 2, true}});
        recorder.finish(); QTRY_VERIFY_WITH_TIMEOUT(!recorder.snapshot().finishing, 10000);
        const auto status = recorder.snapshot(); QVERIFY2(status.error.isEmpty(), qPrintable(status.error));
        QFile manifest(status.directory + "/manifest.json"); QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto segments = QJsonDocument::fromJson(manifest.readAll()).object()["segments"].toArray();
        QCOMPARE(segments.size(), 3);
        for (int i = 0; i < segments.size(); ++i) {
            QCOMPARE(segments[i].toObject()["sample_frames"].toString(), QStringLiteral("3"));
            QCOMPARE(segments[i].toObject()["epoch"].toString(), i == 1 ? QStringLiteral("1") : QStringLiteral("2"));
        }
    }
    void udpPipelineAndSelftestIsolation()
    {
        QUdpSocket reserve; QVERIFY(reserve.bind(QHostAddress::LocalHost, 0));
        const auto port = reserve.localPort(); reserve.close();
        QUdpSocket sender; QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        Pipeline pipeline; ProcessingSettings config; config.localization = true; config.denoise = true;
        // Deliberately enable acoustic paths: selftest must still never enter them.
        for (int i = 0; i < 128; ++i) config.geometry.microphones.append({(i % 16) * .01, (i / 16) * .01, 0});
        pipeline.configure(config); pipeline.start({"127.0.0.1", port, "127.0.0.1", sender.localPort()});
        QTRY_VERIFY(pipeline.snapshot().listening);
        for (int i = 0; i < 600; ++i) {
            QCOMPARE(sender.writeDatagram(packet(i).bytes, QHostAddress::LocalHost, port), qint64(1208));
            if (i % 30 == 0) QTest::qWait(2);
        }
        QTRY_COMPARE_WITH_TIMEOUT(pipeline.snapshot().valid, 600u, 5000);
        QTRY_COMPARE(pipeline.snapshot().selftest.frames, 1800u);
        auto state = pipeline.snapshot(); QCOMPARE(state.queueDrops, 0u); QCOMPARE(state.selftest.mismatchingFrames, 0u);
        QVERIFY(state.lastSourceDatagramAgeMs >= 0); QVERIFY(state.processingDelayMs >= 0);
        QVERIFY(state.json().contains("queued_packets")); QVERIFY(state.json().contains("last_source_datagram_age_ms"));
        QVERIFY(state.localization.heat.isEmpty()); QVERIFY(!state.localization.trusted);
        QVERIFY(state.acousticStatus.contains(QStringLiteral("禁止")));
        auto bad = packet(600).bytes; bad[4] = 2; sender.writeDatagram(bad, QHostAddress::LocalHost, port);
        QTRY_VERIFY(pipeline.snapshot().invalid > 0);
        QTRY_VERIFY(pipeline.snapshot().selftest.hostLoss);
        pipeline.stop(); QVERIFY(!pipeline.snapshot().listening);
    }
    void stopPublishesFinalAudit()
    {
        QUdpSocket reserve; QVERIFY(reserve.bind(QHostAddress::LocalHost, 0));
        const auto port = reserve.localPort(); reserve.close();
        QUdpSocket sender; QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        Pipeline pipeline;
        pipeline.start({"127.0.0.1", port, "127.0.0.1", sender.localPort()});
        QTRY_VERIFY(pipeline.snapshot().listening);
        auto tail = packet(0).bytes; tail[40] ^= 1;
        QCOMPARE(sender.writeDatagram(tail, QHostAddress::LocalHost, port), qint64(1208));
        QTRY_COMPARE(pipeline.snapshot().valid, 1u);
        pipeline.stop();
        const auto state = pipeline.snapshot();
        QCOMPARE(state.selftest.frames, 3u);
        QCOMPARE(state.selftest.mismatchingSamples, 1u);
        QCOMPARE(state.selftest.status(), QStringLiteral("FAIL"));
        QCOMPARE(state.packetSequence["unique"].toString(), QStringLiteral("1"));
        QCOMPARE(state.frameSequence["unique"].toString(), QStringLiteral("3"));
    }
    void all128ChannelsRecordingThroughput()
    {
        QTemporaryDir directory;
        Recorder recorder; RecordingSettings settings; settings.directory = directory.path(); settings.tracks.clear();
        for (int ch = 0; ch < 128; ++ch) settings.tracks.append(ch);
        QVERIFY(recorder.begin(settings));
        for (int b = 0; b < 32; ++b) {
            QVector<CaptureRecord> records;
            for (int i = 0; i < 128; ++i) records.append({packet(b * 128 + i), quint64(b * 128 + i) * 62500, 1, b || i});
            recorder.enqueue(records);
        }
        recorder.finish(); QTRY_VERIFY_WITH_TIMEOUT(!recorder.snapshot().finishing, 10000);
        const auto status = recorder.snapshot(); QVERIFY2(status.error.isEmpty(), qPrintable(status.error));
        QCOMPARE(status.frames, 32u * 128 * 3); QCOMPARE(status.dropped, 0u);
    }
    void uiReadinessAndInvalidInput()
    {
        MainWindow window; window.show();
        auto *record = window.findChild<QPushButton *>("recordButton");
        auto *quick = window.findChild<QPushButton *>("quickRecordButton");
        QVERIFY(record); QVERIFY(quick); QVERIFY(!record->isEnabled()); QVERIFY(!quick->isEnabled());
        auto *tracks = window.findChild<QListWidget *>("recordTracks"); QVERIFY(tracks);
        auto *source = window.findChild<QComboBox *>("recordSource"); QVERIFY(source);
        tracks->item(0)->setCheckState(Qt::Unchecked); tracks->item(127)->setCheckState(Qt::Checked);
        source->setCurrentIndex(1); QCOMPARE(tracks->count(), 4);
        tracks->item(2)->setCheckState(Qt::Checked); source->setCurrentIndex(0);
        QCOMPARE(tracks->item(0)->checkState(), Qt::Unchecked); QCOMPARE(tracks->item(127)->checkState(), Qt::Checked);
        source->setCurrentIndex(1); QCOMPARE(tracks->item(2)->checkState(), Qt::Checked);
        auto *bandpass = window.findChild<QCheckBox *>("bandpassEnabled"); QVERIFY(bandpass); bandpass->setChecked(true);
        auto *low = window.findChild<QDoubleSpinBox *>("bandpassLow"); auto *high = window.findChild<QDoubleSpinBox *>("bandpassHigh");
        QVERIFY(low); QVERIFY(high); high->setValue(8000); low->setValue(9000); QVERIFY(high->value() > low->value());
        high->setValue(200); QVERIFY(low->value() < high->value()); QVERIFY(bandpass->isChecked());
        low->setValue(100); high->setValue(8000); bandpass->setChecked(false);
        auto *bind = window.findChild<QLineEdit *>("bindAddress"); QVERIFY(bind); bind->setText("not-an-ip");
        window.startReceiving(); QVERIFY(!window.pipeline().snapshot().listening);
        auto *notice = window.findChild<QLabel *>("operationNoticeText"); QVERIFY(notice); QVERIFY(notice->text().contains("IPv4"));
        auto *device = window.findChild<QLineEdit *>("deviceAddress"); QVERIFY(device); device->setText("127.0.0.1");
        bind->setText("127.0.0.1");
        QUdpSocket occupied; QVERIFY(occupied.bind(QHostAddress::LocalHost, 0, QUdpSocket::DontShareAddress));
        auto *port = window.findChild<QSpinBox *>("localPort"); QVERIFY(port); port->setValue(occupied.localPort());
        window.startReceiving();
        QTRY_VERIFY_WITH_TIMEOUT(!window.pipeline().snapshot().error.isEmpty(), 2000);
        auto *receive = window.findChild<QPushButton *>("receiveButton"); QVERIFY(receive);
        QTRY_COMPARE_WITH_TIMEOUT(receive->text(), QStringLiteral("开始接收"), 2000);
        QVERIFY(bind->isEnabled()); QVERIFY(port->isEnabled()); QVERIFY(notice->isVisible());
        QVERIFY(notice->text().contains(QStringLiteral("绑定失败")));
    }
    void recordingSelectionsPersistAcrossWindows()
    {
        {
            MainWindow window;
            auto *source = window.findChild<QComboBox *>("recordSource");
            auto *tracks = window.findChild<QListWidget *>("recordTracks"); QVERIFY(source); QVERIFY(tracks);
            tracks->item(0)->setCheckState(Qt::Unchecked); tracks->item(4)->setCheckState(Qt::Checked); tracks->item(127)->setCheckState(Qt::Checked);
            source->setCurrentIndex(1); tracks->item(0)->setCheckState(Qt::Unchecked); tracks->item(3)->setCheckState(Qt::Checked);
            QSettings saved(QSettings::IniFormat, QSettings::UserScope, "MicArray", "MicArray128Tests"); saved.sync();
            QCOMPARE(saved.status(), QSettings::NoError);
            QCOMPARE(saved.value("record/source").toInt(), 1);
        }
        MainWindow restored;
        auto *source = restored.findChild<QComboBox *>("recordSource"); auto *tracks = restored.findChild<QListWidget *>("recordTracks");
        QCOMPARE(source->currentIndex(), 1); QCOMPARE(tracks->item(0)->checkState(), Qt::Unchecked); QCOMPARE(tracks->item(3)->checkState(), Qt::Checked);
        source->setCurrentIndex(0); QCOMPARE(tracks->item(0)->checkState(), Qt::Unchecked);
        QCOMPARE(tracks->item(4)->checkState(), Qt::Checked); QCOMPARE(tracks->item(127)->checkState(), Qt::Checked);
        QVERIFY(!restored.pipeline().snapshot().listening); QVERIFY(!restored.pipeline().recorder().snapshot().active);
    }
    void closeWaitsForRecordingAndCanCancel()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        MainWindow window; window.show();
        RecordingSettings settings; settings.directory = directory.path(); settings.tracks = {0, 127};
        auto &recorder = window.pipeline().recorder(); QVERIFY(recorder.begin(settings));
        QVector<CaptureRecord> capture;
        for (int i = 0; i < 1200; ++i) capture.append({packet(i), quint64(i) * 62500, 1, i != 0});
        recorder.enqueue(capture);
        QTimer::singleShot(0, &window, [&] { auto *box = window.findChild<QMessageBox *>(); QVERIFY(box); box->button(QMessageBox::No)->click(); });
        QVERIFY(!window.close()); QVERIFY(window.isVisible()); QVERIFY(recorder.snapshot().active);
        QTimer::singleShot(0, &window, [&] { auto *box = window.findChild<QMessageBox *>(); QVERIFY(box); box->button(QMessageBox::Yes)->click(); });
        QVERIFY(!window.close());
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        const auto state = recorder.snapshot(); QVERIFY(!state.active); QVERIFY(!state.finishing); QVERIFY(state.error.isEmpty());
        QCOMPARE(state.frames, 3600u); QCOMPARE(state.dropped, 0u);
        QFile manifest(state.directory + "/manifest.json"); QVERIFY(manifest.open(QIODevice::ReadOnly));
        QVERIFY(QJsonDocument::fromJson(manifest.readAll()).object()["complete"].toBool());
    }
    void recordingFailureIsVisibleOutsideRecordingPage()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QFile blocker(directory.filePath("not-a-directory")); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        MainWindow window; window.show();
        RecordingSettings settings; settings.directory = blocker.fileName();
        QVERIFY(window.pipeline().recorder().begin(settings));
        auto *notice = window.findChild<QLabel *>("operationNoticeText"); QVERIFY(notice);
        QTRY_VERIFY_WITH_TIMEOUT(notice->text().contains(QStringLiteral("录制异常")), 3000);
        QVERIFY(notice->isVisible()); QVERIFY(!window.pipeline().recorder().snapshot().active);
    }
    void compactMonitorDoesNotClipChannelGrid()
    {
        MainWindow window; window.resize(1120, 700); window.show();
        auto *pages = window.findChild<QTabWidget *>("mainPages"); pages->setCurrentIndex(int(MainWindow::Page::Monitor));
        QTest::qWait(50);
        auto *grid = window.findChild<ChannelGrid *>(); QVERIFY(grid);
        qInfo() << "Channel grid geometry" << grid->geometry() << "parent bounds" << grid->parentWidget()->rect();
        QVERIFY(grid->parentWidget()->rect().contains(grid->geometry()));
        auto *notice = window.findChild<QLabel *>("operationNoticeText"); QVERIFY(notice);
        notice->setText(QStringLiteral("测试：音频输出设备已改变，监听已停止。请检查系统默认输出后重新开启监听；录制不受影响。")); notice->parentWidget()->show(); QTest::qWait(30);
        qInfo() << "Grid with notice" << grid->geometry() << "parent" << grid->parentWidget()->rect();
        QCOMPARE(window.size(), QSize(1120, 700)); QVERIFY(grid->parentWidget()->rect().contains(grid->geometry()));
    }
    void channelGridKeyboardAndHistory()
    {
        ChannelGrid grid; grid.show(); grid.setFocus();
        QSignalSpy selected(&grid, &ChannelGrid::channelSelected);
        QTest::keyClick(&grid, Qt::Key_End); QCOMPARE(selected.last()[0].toInt(), 127);
        QTest::keyClick(&grid, Qt::Key_Down); QCOMPARE(selected.last()[0].toInt(), 127);
        QTest::keyClick(&grid, Qt::Key_Up); QCOMPARE(selected.last()[0].toInt(), 111);
        QTest::keyClick(&grid, Qt::Key_Home); QCOMPARE(selected.last()[0].toInt(), 0);
        PipelineSnapshot snapshot; snapshot.latestHeader = "test"; snapshot.listening = true; snapshot.lastPacketAgeMs = 0;
        grid.setData(snapshot, 5); QVERIFY(grid.accessibleDescription().contains(QStringLiteral("实时")));
        snapshot.listening = false; grid.setData(snapshot, 5); QVERIFY(grid.accessibleDescription().contains(QStringLiteral("历史")));
    }
    void calibrationProfileRoundTripAndAtomicRejection()
    {
        CameraProfile original; original.camera = {12.3, -4.5, 93.2, 62.4, 1.5}; original.referenceSize = {1920, 1080};
        for (int i = 0; i < 4; ++i) original.samples.append({{-20.0 + i * 12, 15.0 - i * 9}, {.1 + i * .2, .8 - i * .2}, .8, 1.2});
        CameraProfile restored; QString error;
        QVERIFY(CameraProfile::load(original.json(), &restored, &error)); QVERIFY(error.isEmpty());
        QCOMPARE(restored.camera, original.camera); QCOMPARE(restored.samples, original.samples); QCOMPARE(restored.referenceSize, original.referenceSize);
        auto invalid = QJsonDocument::fromJson(original.json()).object(); invalid["projection"] = "linear-wide";
        QVERIFY(!CameraProfile::load(QJsonDocument(invalid).toJson(), &restored, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(restored.samples, original.samples);
        invalid = QJsonDocument::fromJson(original.json()).object(); invalid["samples"] = QJsonArray {QJsonArray {0, 0, 2, .5, .8, 1}};
        QVERIFY(!CameraProfile::load(QJsonDocument(invalid).toJson(), &restored, &error)); QCOMPARE(restored.camera, original.camera);
        QVERIFY(!CameraProfile::load("{invalid}", &restored, &error)); QCOMPARE(restored.camera, original.camera);
        const QJsonObject legacy {{"format", "m128-camera-v1"}, {"values", QJsonArray {0, 0, 90, 60, 0}}};
        QVERIFY(CameraProfile::load(QJsonDocument(legacy).toJson(), &restored, &error)); QVERIFY(restored.samples.isEmpty());
        MainWindow window;
        auto *fit = window.findChild<QPushButton *>("calibrationFit"); auto *undo = window.findChild<QPushButton *>("calibrationUndo"); QVERIFY(fit); QVERIFY(undo);
        QVERIFY(!fit->isEnabled()); QVERIFY(!undo->isEnabled());
    }
    void postprocessingOrderKeyboardAndBypass()
    {
        MainWindow window; window.show();
        auto *pages = window.findChild<QTabWidget *>("mainPages"); pages->setCurrentIndex(int(MainWindow::Page::Denoise));
        auto *order = window.findChild<QListWidget *>("postprocessingOrder"); QVERIFY(order); order->setCurrentRow(1); order->setFocus();
        QTest::qWait(30); QTest::keyClick(order, Qt::Key_Up, Qt::AltModifier);
        QCOMPARE(order->item(0)->data(Qt::UserRole).toInt(), 1); QVERIFY(order->item(0)->text().contains(QStringLiteral("旁路")));
        auto *bandpass = window.findChild<QCheckBox *>("bandpassEnabled"); QVERIFY(bandpass); bandpass->setChecked(true);
        QCOMPARE(order->item(1)->data(Qt::UserRole).toInt(), 0); QVERIFY(order->item(1)->text().contains(QStringLiteral("启用")));
        QTest::keyClick(order, Qt::Key_Down, Qt::AltModifier); QCOMPARE(order->item(1)->data(Qt::UserRole).toInt(), 1);
    }
    void resumedReceiverDistinguishesHistoricalError()
    {
        MainWindow window; window.show(); QUdpSocket sender, reserved;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0)); QVERIFY(reserved.bind(QHostAddress::LocalHost, 0));
        const auto port = reserved.localPort(); reserved.close();
        window.findChild<QLineEdit *>("bindAddress")->setText("127.0.0.1");
        window.findChild<QLineEdit *>("deviceAddress")->setText("127.0.0.1");
        window.findChild<QSpinBox *>("localPort")->setValue(port); window.findChild<QSpinBox *>("devicePort")->setValue(sender.localPort());
        window.startReceiving(); QTRY_VERIFY_WITH_TIMEOUT(window.pipeline().snapshot().listening, 2000);
        QCOMPARE(sender.writeDatagram("bad", QHostAddress::LocalHost, port), qint64(3));
        QTRY_VERIFY_WITH_TIMEOUT(!window.pipeline().snapshot().error.isEmpty(), 2000);
        auto bytes = packet(0).bytes; qToBigEndian<quint32>(0, bytes.data() + 28);
        QCOMPARE(sender.writeDatagram(bytes, QHostAddress::LocalHost, port), qint64(1208));
        auto *health = window.findChild<QLabel *>("acquisitionStatus"); QVERIFY(health);
        QTRY_VERIFY_WITH_TIMEOUT(health->text().contains(QStringLiteral("实采接收中 · 有历史告警")), 2000);
        QVERIFY(window.pipeline().snapshot().listening); QCOMPARE(window.pipeline().snapshot().invalid, 1u);
        window.findChild<QPushButton *>("receiveButton")->click();
        QTRY_COMPARE_WITH_TIMEOUT(health->text(), QStringLiteral("接收已停止 · 有历史告警"), 2000);
        QVERIFY(!window.pipeline().snapshot().receiverFailed);
    }
    void draggingSelectsCorrectTarget()
    {
        FieldView view; view.resize(640, 360); view.show();
        MicArray::CameraViewSettings camera; camera.horizontalFovDegrees = 90; camera.verticalFovDegrees = 60;
        view.setCameraView(camera); view.setTargets({{{0, 0}, {-30, 0}, {30, 0}}}, true, 0);
        QSignalSpy moved(&view, &FieldView::targetMoved);
        MicArray::SoundSourceDirection direction; direction.azimuthDegrees = 30;
        const auto point = MicArray::projectDirection(direction, camera).normalizedPoint;
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, QPoint(qRound(point.x() * 640), qRound(point.y() * 360)));
        QVERIFY(!moved.isEmpty()); QCOMPARE(moved.last()[0].toInt(), 2); QVERIFY(!moved.last()[3].toBool());
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, QPoint(500, 170));
        QCOMPARE(moved.last()[0].toInt(), 2); QVERIFY(moved.last()[3].toBool());
        const double az = moved.last()[1].toDouble();
        QTest::keyClick(&view, Qt::Key_Right); QVERIFY(std::abs(moved.last()[1].toDouble() - az - .1) < 1e-8);
        QTest::keyClick(&view, Qt::Key_Left, Qt::ShiftModifier); QVERIFY(std::abs(moved.last()[1].toDouble() - az + .9) < 1e-8);
        MainWindow window;
        auto *field = window.findChild<FieldView *>("directionView"); auto *target = window.findChild<QComboBox *>("activeDirection"); QVERIFY(field); QVERIFY(target);
        emit field->targetMoved(1, -21.2, 4.5, true); QCOMPARE(target->currentIndex(), 1);
    }
    void fusionUiIsExplicitAndDoesNotAutoResume()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        {
            MainWindow window;
            auto *enabled = window.findChild<QCheckBox *>("fusionEnabled");
            auto *status = window.findChild<QLabel *>("fusionStatus");
            auto *threshold = window.findChild<QDoubleSpinBox *>("fusionConfidence");
            auto *path = window.findChild<QLineEdit *>("fusionMetadataPath");
            QVERIFY(enabled); QVERIFY(status); QVERIFY(threshold); QVERIFY(path); QVERIFY(!enabled->isChecked());
            path->setText(directory.filePath("not-a-model.json"));
            threshold->setValue(.32);
            QWheelEvent wheel(QPointF(10, 10), QPointF(threshold->mapToGlobal(QPoint(10, 10))), {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(threshold, &wheel); QCOMPARE(threshold->value(), .32);
            enabled->setChecked(true);
            // Report the missing model even without a connected camera/UDP stream.
            QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("无法读取融合模型元数据")), 2000);
            QCOMPARE(window.findChild<QTableWidget *>("fusionSourceTable")->rowCount(), 0);
            window.saveDiagnosticReport(directory.filePath("diagnostics.json"));
            QFile report(directory.filePath("diagnostics.json")); QVERIFY(report.open(QIODevice::ReadOnly));
            const auto fusion = QJsonDocument::fromJson(report.readAll()).object()["audiovisual_fusion"].toObject();
            QVERIFY(fusion["enabled"].toBool()); QVERIFY(!fusion["ready"].toBool()); QVERIFY(!fusion["current_result"].toBool());
            QVERIFY(!fusion["hardware_synchronized"].toBool()); QCOMPARE(fusion["domain"].toString(), QString("simulation_only"));
        }
        MainWindow restored;
        QVERIFY(!restored.findChild<QCheckBox *>("fusionEnabled")->isChecked());
        QCOMPARE(restored.findChild<QDoubleSpinBox *>("fusionConfidence")->value(), .32);
        QCOMPARE(restored.findChild<QLineEdit *>("fusionMetadataPath")->text(), directory.filePath("not-a-model.json"));
    }
    void uiSmoke_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("dark") << true; QTest::newRow("light") << false;
    }
    void uiSmoke()
    {
        QFETCH(bool, dark);
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, "MicArray", "MicArray128Tests"); settings.setValue("dark", dark);
        MainWindow window; window.show();
        auto *tabs = window.findChild<QTabWidget *>("mainPages"); QVERIFY(tabs); QCOMPARE(tabs->count(), 6);
        auto *health = window.findChild<QLabel *>("acquisitionStatus"); QVERIFY(health);
        const QStringList names {QStringLiteral("实时监测"), QStringLiteral("声源定位"), QStringLiteral("定向降噪"), QStringLiteral("音频录制"), QStringLiteral("系统配置"), QStringLiteral("设备诊断")};
        for (int i = 0; i < names.size(); ++i) QCOMPARE(tabs->tabText(i), names[i]);
        auto *configuration = window.findChild<QTabWidget *>("configurationTabs"); QVERIFY(configuration);
        auto *serialButton = window.findChild<QPushButton *>("serialSettingsButton"); QVERIFY(serialButton); serialButton->click();
        auto *debugTabs = window.findChild<QTabWidget *>("debugTabs"); QVERIFY(debugTabs);
        QCOMPARE(tabs->currentIndex(), static_cast<int>(MainWindow::Page::Diagnostics)); QCOMPARE(debugTabs->currentWidget()->objectName(), QStringLiteral("sys3Panel"));
        for (char command : QByteArray("shrtc")) {
            auto *button = window.findChild<QPushButton *>(QString("sys3Command_%1").arg(QChar(command)));
            QVERIFY(button); QVERIFY(!button->isEnabled());
        }
        auto *network = window.findChild<QPushButton *>("interfaceSettingsButton"); QVERIFY(network); network->click();
        QCOMPARE(tabs->currentIndex(), static_cast<int>(MainWindow::Page::Configuration)); QCOMPARE(configuration->currentIndex(), 0);
        auto *port = window.findChild<QSpinBox *>("localPort"); QVERIFY(port);
        const int before = port->value();
        QWheelEvent wheel(QPointF(10, 10), QPointF(port->mapToGlobal(QPoint(10, 10))), {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(port, &wheel); QCOMPARE(port->value(), before);
        auto *geometry = window.findChild<QPushButton *>("geometrySettingsButton"); QVERIFY(geometry); geometry->click();
        QCOMPARE(configuration->currentIndex(), 1);
        auto *mapping = window.findChild<QTableWidget *>("geometryTable"); QVERIFY(mapping); QCOMPARE(mapping->rowCount(), 128);
        QCOMPARE(mapping->item(0, 2)->text(), QStringLiteral("J26 / L7"));
        QCOMPARE(mapping->item(127, 3)->text(), QStringLiteral("V26"));
        auto *specs = window.findChild<QTableWidget *>("hardwareSpecifications"); QVERIFY(specs); QCOMPARE(specs->rowCount(), 18);
        auto *localTabs = window.findChild<QTabWidget *>("localizationTabs"); QVERIFY(localTabs);
        QCOMPARE(localTabs->count(), 4); QCOMPARE(localTabs->tabText(3), QStringLiteral("融合识别"));
        auto *calibration = window.findChild<QTableWidget *>("calibrationTable"); QVERIFY(calibration);
        auto *calibrationActions = window.findChild<QWidget *>("calibrationActions"); QVERIFY(calibrationActions);
        for (const QSize size : {QSize(1440, 900), QSize(1120, 700)}) {
            window.resize(size); tabs->setCurrentIndex(1); localTabs->setCurrentIndex(1); QTest::qWait(50);
            QCOMPARE(window.size(), size);
            const auto last = calibration->visualItemRect(calibration->item(3, 3));
            QVERIFY(calibration->viewport()->rect().contains(last));
            QVERIFY(window.rect().contains(QRect(calibration->mapTo(&window, QPoint(0, 0)), calibration->size())));
            QVERIFY(calibration->geometry().bottom() < calibrationActions->geometry().top());
            auto *notice = window.findChild<QLabel *>("operationNoticeText");
            notice->setText(QStringLiteral("测试：校准参考宽高比与当前画面不同，请核对视频配置。")); notice->parentWidget()->show(); QTest::qWait(30);
            QCOMPARE(window.size(), size);
            QVERIFY(calibration->viewport()->rect().contains(calibration->visualItemRect(calibration->item(3, 3))));
            QVERIFY(window.rect().contains(QRect(calibrationActions->mapTo(&window, QPoint()), calibrationActions->size())));
            notice->parentWidget()->hide(); QTest::qWait(20);
            localTabs->setCurrentIndex(3); QTest::qWait(20);
            auto *fusionTable = window.findChild<QTableWidget *>("fusionSourceTable"); QVERIFY(fusionTable);
            QVERIFY(window.rect().contains(QRect(fusionTable->mapTo(&window, QPoint()), fusionTable->size())));
            QVERIFY(fusionTable->height() >= 100);
            localTabs->setCurrentIndex(1);
            if (qEnvironmentVariableIsSet("M128_SCREENSHOT_DIR")) {
                const QDir directory(qEnvironmentVariable("M128_SCREENSHOT_DIR")); QVERIFY(directory.exists());
                for (int p = 0; p < tabs->count(); ++p) {
                    tabs->setCurrentIndex(p);
                    auto *subtabs = tabs->widget(p)->findChild<QTabWidget *>();
                    for (int sub = 0; sub < (subtabs ? subtabs->count() : 1); ++sub) {
                        if (subtabs) subtabs->setCurrentIndex(sub); QTest::qWait(30);
                        QCOMPARE(window.size(), size);
                        QVERIFY(window.grab().save(directory.filePath(QString("%1-%2x%3-page%4-tab%5.png").arg(dark ? "dark" : "light").arg(size.width()).arg(size.height()).arg(p).arg(sub))));
                    }
                }
            }
        }
        QTest::qWait(150);
        const auto statusImage = health->grab().toImage().convertToFormat(QImage::Format_RGB32);
        int brightPixels = 0;
        for (int y = 0; y < statusImage.height(); ++y) for (int x = 0; x < statusImage.width(); ++x)
            brightPixels += qGray(statusImage.pixel(x, y)) > 180;
        QVERIFY2(brightPixels > 20, "The status badge must retain visible light text in both themes");
        const auto screenshot = window.grab(); QVERIFY(!screenshot.isNull());
        if (qEnvironmentVariableIsSet("M128_SCREENSHOT")) QVERIFY(screenshot.save(qEnvironmentVariable("M128_SCREENSHOT")));
    }
    void hardwareReferenceAndMapping()
    {
        const auto reference = hardwareReference();
        QVERIFY(reference["reference_only"].toBool()); QVERIFY(reference["detected_firmware"].isNull());
        QCOMPARE(reference["clock_outputs"].toInt(), 8); QCOMPARE(reference["mems_io_voltage_v"].toDouble(), 1.8);
        QCOMPARE(reference["passband_high_hz"].toInt(), 16000); QVERIFY(reference["uart_supported_by_host"].toBool());
        QVERIFY(dataPin(-1).isEmpty()); QVERIFY(dataPin(128).isEmpty());
        QSet<QString> pads;
        for (int ch = 0; ch < 128; ++ch) {
            QCOMPARE(dataConnector(ch), ch < 64 ? QStringLiteral("J26") : QStringLiteral("J25"));
            QCOMPARE(dataBank(ch), ch < 64 ? QStringLiteral("L7") : QStringLiteral("L6"));
            QCOMPARE(dataPin(ch), dataPin(ch ^ 1)); pads.insert(dataPin(ch));
        }
        QCOMPARE(pads.size(), 64); QCOMPARE(dataPin(0), QStringLiteral("AD25")); QCOMPARE(dataPin(64), QStringLiteral("U16"));
        const PipelineSnapshot snapshot;
        const auto report = snapshot.json();
        QCOMPARE(report["hardware_reference"].toObject(), reference);
        const auto channels = report["channels"].toArray(); QCOMPARE(channels.size(), 128);
        for (int ch = 0; ch < 128; ++ch) {
            const auto channel = channels[ch].toObject(); QVERIFY(channel.contains("clock_group")); QVERIFY(channel["clock_group"].isNull());
            QCOMPARE(channel["connector"].toString(), dataConnector(ch)); QCOMPARE(channel["io_bank"].toString(), dataBank(ch));
            QCOMPARE(channel["fpga_pad"].toString(), dataPin(ch));
        }
        QVERIFY(commissioningGuide().contains("SYS3"));
    }
};

int main(int argc, char **argv)
{
    Q_INIT_RESOURCE(reference);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    if (qEnvironmentVariableIsSet("M128_FONT")) QFontDatabase::addApplicationFont(qEnvironmentVariable("M128_FONT"));
    QCoreApplication::setApplicationName("MicArray128Tests");
    Array128Test test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_array128.moc"
