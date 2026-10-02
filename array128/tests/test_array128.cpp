#include "diagnostics.h"
#include "dsp.h"
#include "hardwareprofile.h"
#include "mainwindow.h"
#include "recorder.h"
#include "sys3protocol.h"
#include "sys3client.h"
#include "cameraprofile.h"
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
