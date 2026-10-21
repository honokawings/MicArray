#include "analysis/pllocalizationcontroller.h"
#include "analysis/directionprojection.h"
#include "app/appsettings.h"
#include "media/audioprocessor.h"
#include "media/noisereductionframedecoder.h"
#include "ui/livelocalizationpage.h"
#include "ui/livelocalizationview.h"
#include "ui/mainwindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

using namespace MicArray;

namespace {

QByteArray payloadFor(quint16 sequence, quint16 frame, quint8 mask = 1,
                      qint16 azimuth = -3000, qint16 elevation = -2000)
{
    QByteArray packet(41, 0);
    packet.replace(0, 4, "LOC3");
    packet[4] = 4;
    packet[5] = static_cast<char>(mask);
    qToLittleEndian(sequence, packet.data() + 6);
    qToLittleEndian(frame, packet.data() + 8);
    for (int source = 0; source < 3; ++source)
    {
        const int entry = 10 + source * 10;
        packet[entry] = static_cast<char>(source | ((mask & (1 << source)) ? 0x80 : 0));
        qToLittleEndian<quint16>(1910, packet.data() + entry + 1);
        qToLittleEndian<quint16>(1072, packet.data() + entry + 3);
        qToLittleEndian<qint16>(azimuth + source * 3000, packet.data() + entry + 5);
        qToLittleEndian<qint16>(elevation, packet.data() + entry + 7);
        packet[entry + 9] = (mask & (1 << source)) ? static_cast<char>(220) : 0;
    }
    quint8 checksum = 0;
    for (int index = 0; index < 40; ++index) { checksum ^= static_cast<quint8>(packet[index]); }
    packet[40] = static_cast<char>(checksum);
    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, 0);
    for (int sample = 0; sample < 200; ++sample)
    {
        for (int channel = 0; channel < 48; ++channel)
        {
            const qint32 value = channel == 0 ? -8388608
                : channel == 47 ? 8388607 : ((channel % 2) ? -1 : 1) * (channel * 256 + sample);
            const quint8 sideband = sample == 3
                ? (channel >= 7 ? static_cast<quint8>(packet[channel - 7]) : quint8(0x6b + channel)) : 0;
            const quint32 word = (static_cast<quint32>(value) & 0xffffffu)
                | (static_cast<quint32>(sideband) << 24);
            qToLittleEndian(word, payload.data() + (sample * 48 + channel) * 4);
        }
    }
    return payload;
}

void deliver(PlLocalizationController &controller, quint16 seq, quint16 frame, quint8 mask = 1)
{
    const auto decoded = AudioLocationSidebandDecoder::decode(payloadFor(seq, frame, mask), 0, 0);
    for (const auto &location : decoded.locations) { controller.accept(location); }
}

QByteArray coherentPcmFor(int frame)
{
    // Identical broadband channels model a center source, with clean signed PCM24.
    QByteArray pcm(AudioFrameDecoder::FramePayloadBytes, 0);
    for (int sample = 0; sample < 200; ++sample)
    {
        quint32 noise = quint32(frame * 200 + sample + 1) * 0x9e3779b9u;
        noise ^= noise >> 16;
        noise *= 0x85ebca6bu;
        noise ^= noise >> 13;
        const qint32 value = qint32(noise & 0xfffffu) - 0x80000;
        for (int channel = 0; channel < 48; ++channel)
        {
            qToLittleEndian(value, pcm.data() + (sample * 48 + channel) * 4);
        }
    }
    return pcm;
}

void deliverToPage(LiveLocalizationPage &page, int sequence, qint16 azimuth = -3000,
                   qint16 elevation = -2000)
{
    const auto packet = AudioLocationSidebandDecoder::decode(
        payloadFor(sequence, sequence, 1, azimuth, elevation), 0, 0);
    for (const auto &location : packet.locations) { page.setPlLocation(location); }
}

LiveSpatialResponse blueResponse(quint64 generation)
{
    LiveSpatialResponse response;
    response.generation = generation;
    response.columns = 16;
    response.rows = 9;
    response.normalizedValues.fill(0.1f, 16 * 9);
    response.reliability = 0.9;
    response.trustedDirection = true;
    return response;
}

bool publishResponse(LiveLocalizationPage &page, const LiveSpatialResponse &response)
{
    return QMetaObject::invokeMethod(&page, "acceptSpatialResponse", Qt::DirectConnection,
                                    Q_ARG(MicArray::LiveSpatialResponse, response));
}

} // namespace

class PlIntegrationTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_settings.isValid());
        QCoreApplication::setOrganizationName("MicArrayMergeTests");
        QCoreApplication::setApplicationName("Integration");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        const int font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
        QVERIFY(font >= 0);
        QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font).first(), 10));
    }

    void keepsAll48SignedSamplesAndUsesV4Angles()
    {
        const auto decoded = AudioLocationSidebandDecoder::decode(payloadFor(12, 34), 100, 500);
        QVERIFY(decoded.valid);
        QVERIFY(decoded.warnings.isEmpty());
        QCOMPARE(decoded.locations.size(), 3);
        const auto direction = decoded.locations.first().physicalDirection();
        QCOMPARE(direction.azimuthDegrees, -30.0);
        QCOMPARE(direction.elevationDegrees, -20.0);
        for (int sample = 0; sample < 200; ++sample)
        {
            for (int channel = 0; channel < 48; ++channel)
            {
                const qint32 expected = channel == 0 ? -8388608
                    : channel == 47 ? 8388607 : ((channel % 2) ? -1 : 1) * (channel * 256 + sample);
                QCOMPARE(qFromLittleEndian<qint32>(decoded.pcmPayload.constData()
                    + (sample * 48 + channel) * 4), expected);
            }
        }
    }

    void rejectsChecksumWithoutLosingPcm()
    {
        QByteArray payload = payloadFor(12, 34);
        payload[(3 * 48 + 47) * 4 + 3] ^= 1;
        const auto decoded = AudioLocationSidebandDecoder::decode(payload, 0, 0);
        QVERIFY(decoded.valid);
        QCOMPARE(decoded.locations.size(), 0);
        QCOMPARE(decoded.warnings.size(), 1);
        QCOMPARE(decoded.pcmPayload, AudioLocationSidebandDecoder::decode(payloadFor(12, 34), 0, 0).pcmPayload);
    }

    void extractsLocationsBeforeGainAndPublishesCleanRawAudio()
    {
        AudioProcessor processor;
        GainConfiguration gain;
        gain.shifts.fill(1);
        processor.setGainConfiguration(gain);
        QSignalSpy raw(&processor, &AudioProcessor::rawFrameReady);
        QSignalSpy adjusted(&processor, &AudioProcessor::adjustedFrameReady);
        QSignalSpy locations(&processor, &AudioProcessor::plLocationDecoded);
        QSignalSpy done(&processor, &AudioProcessor::frameProcessed);
        processor.processArrayFrame(payloadFor(5, 5), 10, 20);
        QCOMPARE(locations.size(), 3);
        QCOMPARE(raw.size(), 1);
        QCOMPARE(adjusted.size(), 1);
        QCOMPARE(done.first().first().toBool(), true);
        const auto rawPcm = raw.first().first().toByteArray();
        const auto adjustedPcm = adjusted.first().first().toByteArray();
        QCOMPARE(qFromLittleEndian<qint32>(rawPcm.constData()), -8388608);
        QCOMPARE(qFromLittleEndian<qint32>(adjustedPcm.constData()), -16777216);
        QCOMPARE(qFromLittleEndian<qint32>(rawPcm.constData() + 4), -256);
        QCOMPARE(qFromLittleEndian<qint32>(adjustedPcm.constData() + 4), -512);
    }

    void commitsCompletePacketsAndClearsNoSource()
    {
        PlLocalizationController controller;
        const auto decoded = AudioLocationSidebandDecoder::decode(payloadFor(1, 1), 0, 0);
        QSignalSpy status(&controller, &PlLocalizationController::statusChanged);
        controller.accept(decoded.locations[0]);
        controller.accept(decoded.locations[1]);
        QVERIFY(status.isEmpty());
        controller.accept(decoded.locations[2]);
        QCOMPARE(status.size(), 1);
        deliver(controller, 2, 2);
        deliver(controller, 3, 3);
        QCOMPARE(controller.directions().size(), 1);
        QVERIFY(std::abs(controller.directions().first().azimuthDegrees + 30.0) < 0.0001);
        deliver(controller, 4, 4, 0);
        QVERIFY(controller.directions().isEmpty());
    }

    void heartbeatsCannotKeepOldMarkersAliveAndWrapIsAccepted()
    {
        PlLocalizationController controller;
        deliver(controller, 65534, 65534);
        deliver(controller, 65535, 65535);
        deliver(controller, 0, 0);
        QCOMPARE(controller.directions().size(), 1);
        QTest::qWait(600);
        deliver(controller, 1, 0); // Heartbeat: sequence advances, acoustic frame does not.
        QTest::qWait(550);
        QVERIFY(controller.directions().isEmpty());
        deliver(controller, 1, 1);
        deliver(controller, 2, 2);
        deliver(controller, 3, 3);
        QCOMPARE(controller.directions().size(), 1);
        deliver(controller, 2, 2, 0); // Delayed old no-source packet cannot erase a fresh observation.
        QCOMPARE(controller.directions().size(), 1);
    }

    void defaultsToPlHeatmapAndKeepsFusionPreferencesSeparate()
    {
        AppSettings settings(m_settings.filePath("source-switch.ini"));
        LiveLocalizationPage page(&settings);
        auto *source = page.findChild<QComboBox *>("liveLocalizationSourceCombo");
        auto *mode = page.findChild<QComboBox *>("liveLocalizationDisplayModeCombo");
        QVERIFY(source && mode);
        QCOMPARE(source->currentData().toString(), QString("pl"));
        QVERIFY(mode->isEnabled());
        QCOMPARE(mode->currentData().toString(), QString("plheatmap"));
        page.setActive(true);
        QSignalSpy audioRequests(&page, &LiveLocalizationPage::audioFrameRequested);
        QSignalSpy grids(&page, &LiveLocalizationPage::spatialGridRequested);
        page.setVideoFrame(QImage(1280, 720, QImage::Format_RGB32), 0, 0);
        QVERIFY(qvariant_cast<SpatialResponseGrid>(grids.last().first()).directions.isEmpty());
        page.submitAdjustedAudioFrame(payloadFor(1, 1), 0, 0);
        QCOMPARE(audioRequests.size(), 0);
        mode->setCurrentIndex(mode->findData("fusion"));
        QVERIFY(mode->currentText().contains(QStringLiteral("上位机热力图")));
        QVERIFY(!qvariant_cast<SpatialResponseGrid>(grids.last().first()).directions.isEmpty());
        page.submitAdjustedAudioFrame(payloadFor(1, 1), 0, 0);
        QCOMPARE(audioRequests.size(), 1);
        QCOMPARE(settings.liveLocalization().plDisplayMode, QString("fusion"));
        source->setCurrentIndex(source->findData("host"));
        QVERIFY(mode->isEnabled());
        QVERIFY(!qvariant_cast<SpatialResponseGrid>(grids.last().first()).directions.isEmpty());
        page.submitAdjustedAudioFrame(payloadFor(1, 1), 0, 0);
        QCOMPARE(audioRequests.size(), 2);
        mode->setCurrentIndex(mode->findData("heatmap"));
        source->setCurrentIndex(source->findData("pl"));
        QVERIFY(mode->isEnabled());
        QCOMPARE(mode->currentData().toString(), QString("fusion"));
        mode->setCurrentIndex(mode->findData("marker"));
        page.submitAdjustedAudioFrame(payloadFor(1, 1), 0, 0);
        QCOMPARE(audioRequests.size(), 2);
        source->setCurrentIndex(source->findData("host"));
        QCOMPARE(mode->currentData().toString(), QString("heatmap"));
        QCOMPARE(settings.liveLocalization().localizationSource, QString("host"));
    }

    void fusionSettingsPersistAndRejectInvalidModes()
    {
        const QString path = m_settings.filePath("fusion-settings.ini");
        {
            AppSettings settings(path);
            auto value = settings.liveLocalization();
            value.plDisplayMode = " FUSION ";
            value.displayMode = "heatmap";
            settings.saveLiveLocalization(value);
        }
        AppSettings settings(path);
        QCOMPARE(settings.liveLocalization().plDisplayMode, QString("fusion"));
        QCOMPARE(settings.liveLocalization().displayMode, QString("heatmap"));
        LiveLocalizationPage page(&settings);
        QCOMPARE(page.findChild<QComboBox *>("liveLocalizationDisplayModeCombo")
                     ->currentData().toString(), QString("fusion"));
        auto invalid = settings.liveLocalization();
        invalid.plDisplayMode = "heatmap"; // PL needs an explicit fusion source label.
        settings.saveLiveLocalization(invalid);
        QCOMPARE(settings.liveLocalization().plDisplayMode, QString("plheatmap"));
    }

    void fusionCannotReplacePlMarkerWithSoftwarePeak()
    {
        AppSettings settings(m_settings.filePath("fusion-isolation.ini"));
        auto value = settings.liveLocalization();
        value.plDisplayMode = "fusion";
        settings.saveLiveLocalization(value);
        LiveLocalizationPage page(&settings);
        QSignalSpy grids(&page, &LiveLocalizationPage::spatialGridRequested);
        page.setActive(true);
        QImage frame(640, 360, QImage::Format_RGB32);
        frame.fill(QColor(23, 31, 43));
        page.setVideoFrame(frame, 0, 0);
        page.resize(1000, 650);
        page.show();
        for (int sequence = 1; sequence <= 3; ++sequence) { deliverToPage(page, sequence); }
        QCoreApplication::processEvents();
        const quint64 generation = grids.last()[1].toULongLong();
        auto *view = page.findChild<LiveLocalizationView *>("liveLocalizationView");
        QVERIFY(view);
        const QImage plOnly = view->grab().toImage();
        LiveLocalizationResult cpu;
        cpu.generation = generation;
        cpu.directions = {{40.0, 20.0, 0.95}};
        QVERIFY(QMetaObject::invokeMethod(&page, "acceptHostLocalizationResult", Qt::DirectConnection,
                                          Q_ARG(MicArray::LiveLocalizationResult, cpu)));
        QCOMPARE(view->grab().toImage(), plOnly);
        QVERIFY(publishResponse(page, blueResponse(generation)));
        const QImage fused = view->grab().toImage();
        QVERIFY(fused != plOnly);
        auto stale = blueResponse(generation - 1);
        stale.normalizedValues.fill(1.0f, 16 * 9);
        QVERIFY(publishResponse(page, stale));
        QCOMPARE(view->grab().toImage(), fused);
        QVERIFY(page.findChild<QLabel *>("referenceHeatmapStatusLabel")
                    ->text().contains(QStringLiteral("定位点仍来自 PL")));
        page.clearPlLocation();
        const QImage reset = view->grab().toImage();
        QVERIFY(reset != fused);
        QVERIFY(publishResponse(page, blueResponse(generation)));
        QCOMPARE(view->grab().toImage(), reset);
    }

    void fusionHeatmapExpiresWithoutRefreshingPlAndCalibrationUsesPl()
    {
        AppSettings settings(m_settings.filePath("fusion-expiry.ini"));
        auto value = settings.liveLocalization();
        value.plDisplayMode = "fusion";
        settings.saveLiveLocalization(value);
        LiveLocalizationPage page(&settings);
        QSignalSpy grids(&page, &LiveLocalizationPage::spatialGridRequested);
        QSignalSpy pl(&page, &LiveLocalizationPage::plDirectionsChanged);
        QSignalSpy calibration(&page, &LiveLocalizationPage::calibrationSampleRequested);
        page.setActive(true);
        QImage frame(640, 360, QImage::Format_RGB32);
        frame.fill(Qt::black);
        page.setVideoFrame(frame, 0, 0);
        page.resize(1000, 650);
        page.show();
        for (int sequence = 1; sequence <= 3; ++sequence) { deliverToPage(page, sequence); }
        const quint64 generation = grids.last()[1].toULongLong();
        QVERIFY(publishResponse(page, blueResponse(generation)));
        QTest::qWait(600);
        deliverToPage(page, 4);
        QTRY_VERIFY_WITH_TIMEOUT(page.findChild<QLabel *>("referenceHeatmapStatusLabel")
                                    ->text().contains(QStringLiteral("过期")), 800);
        QVERIFY(!qvariant_cast<QVector<SoundSourceDirection>>(pl.last()[0]).isEmpty());
        // Repeated software maps cannot prevent a genuinely stale PL marker clearing.
        for (int i = 0; i < 3; ++i) {
            QVERIFY(publishResponse(page, blueResponse(generation)));
            QTest::qWait(400);
        }
        QVERIFY(qvariant_cast<QVector<SoundSourceDirection>>(pl.last()[0]).isEmpty());
        for (int sequence = 5; sequence <= 7; ++sequence) { deliverToPage(page, sequence); }
        auto *start = page.findChild<QPushButton *>("startFourPointCalibrationButton");
        QVERIFY(start && start->isEnabled());
        start->click();
        auto *view = page.findChild<LiveLocalizationView *>("liveLocalizationView");
        QVERIFY(view);
        QCoreApplication::processEvents();
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, view->videoRect().center().toPoint());
        QCOMPARE(calibration.size(), 0);
    }

    void fusionDrawsHeatmapAndMarkerAtCalibratedDirection()
    {
        LiveLocalizationView view;
        view.resize(640, 360);
        QImage frame(640, 360, QImage::Format_RGB32);
        frame.fill(Qt::black);
        view.setFrame(frame);
        const CameraViewSettings camera {18.0, -10.0, 100.0, 70.0, 22.0};
        view.setCameraView(camera);
        view.setHeatmapOpacity(1.0);
        view.setSpatialResponse(blueResponse(0));
        SoundSourceDirection pl {37.0, -2.0, 0.9};
        pl.sourceId = 0;
        view.setDirections({pl});
        view.setDisplayMode("heatmap");
        view.show();
        QCoreApplication::processEvents();
        const QImage heatmap = view.grab().toImage();
        const auto projected = projectDirection(pl, camera);
        QVERIFY(projected.valid && projected.inView);
        const QPoint point(qRound(projected.normalizedPoint.x() * 640),
                           qRound(projected.normalizedPoint.y() * 360));
        view.setDisplayMode("fusion");
        const QImage fusion = view.grab().toImage();
        // grab() returns physical pixels; all projection/video coordinates are logical pixels.
        const double dpr = fusion.devicePixelRatio();
        const auto physical = [dpr](const QPoint &logical) {
            return QPoint(qRound(logical.x() * dpr), qRound(logical.y() * dpr));
        };
        const QPoint markerPixel = physical(point + QPoint(4, 4));
        QVERIFY2(fusion.pixelColor(markerPixel).red() > 150,
            qPrintable(QStringLiteral("DPR=%1 size=%2x%3 marker=%4,%5 color=%6")
                .arg(dpr).arg(fusion.width()).arg(fusion.height()).arg(markerPixel.x()).arg(markerPixel.y())
                .arg(fusion.pixelColor(markerPixel).name())));
        QVERIFY(fusion.pixelColor(markerPixel).green() < 130);
        // Distant map pixels keep their real software response; no PL-centered blob is added.
        QCOMPARE(fusion.pixelColor(physical(QPoint(600, 300))), heatmap.pixelColor(physical(QPoint(600, 300))));
        view.setDirections({});
        QCOMPARE(view.grab().toImage().pixelColor(markerPixel), heatmap.pixelColor(markerPixel));
    }

    void fusionProcessesRealRawPcmWhileKeepingPlDirections()
    {
        AppSettings settings(m_settings.filePath("fusion-pcm.ini"));
        auto value = settings.liveLocalization();
        value.plDisplayMode = "fusion";
        value.heatmapGridColumns = 16;
        value.heatmapGridRows = 9;
        settings.saveLiveLocalization(value);
        LiveLocalizationPage page(&settings);
        page.setActive(true);
        QImage frame(640, 360, QImage::Format_RGB32);
        frame.fill(QColor(23, 31, 43));
        page.setVideoFrame(frame, 0, 0);
        page.resize(1200, 750);
        page.show();
        QSignalSpy pl(&page, &LiveLocalizationPage::plDirectionsChanged);
        for (int sequence = 1; sequence <= 3; ++sequence) { deliverToPage(page, sequence); }
        for (int index = 0; index < 16; ++index) {
            page.submitAdjustedAudioFrame(coherentPcmFor(index), 10, index * 4167);
        }
        auto *label = page.findChild<QLabel *>("referenceHeatmapStatusLabel");
        QTRY_VERIFY_WITH_TIMEOUT(label->text().contains(QStringLiteral("已更新")), 8000);
        const auto directions = qvariant_cast<QVector<SoundSourceDirection>>(pl.last()[0]);
        QCOMPARE(directions.size(), 1);
        QVERIFY(std::abs(directions.first().azimuthDegrees + 30.0) < 0.0001);
        QVERIFY(std::abs(directions.first().elevationDegrees + 20.0) < 0.0001);
        const QString output = qEnvironmentVariable("MICARRAY_QA_DIR");
        if (!output.isEmpty()) {
            QDir().mkpath(output);
            QVERIFY(page.grab().save(output + "/pl-fusion-synthetic.png"));
        }
    }

    void noiseInputUsesCleanPcmPayload()
    {
        const auto raw = payloadFor(1, 1);
        const auto clean = AudioLocationSidebandDecoder::decode(raw, 0, 0).pcmPayload;
        NoiseReductionFrameDecoder decoder;
        const auto channels = decoder.decode(clean).channels;
        QCOMPARE(channels[0][3], -1.0f);
        QVERIFY(std::abs(channels[1][3] + 2.0f / 32768.0f) < 0.000001f);
        QVERIFY(decoder.decode(raw).channels != channels);
    }

    void tcpLoc3UpdatesUiAndRecordingKeepsRaw48Pcm()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir recordings;
        QVERIFY(recordings.isValid());
        MainWindow window;
        auto *page = window.findChild<LiveLocalizationPage *>("liveLocalizationPage");
        QVERIFY(page);
        window.findChild<QTabWidget *>("mainTabs")->setCurrentWidget(page);
        auto *mode = page->findChild<QComboBox *>("liveLocalizationDisplayModeCombo");
        mode->setCurrentIndex(mode->findData("fusion"));
        page->findChild<QSpinBox *>("liveHeatmapGridColumnsSpin")->setValue(16);
        page->findChild<QSpinBox *>("liveHeatmapGridRowsSpin")->setValue(9);
        QImage video(640, 360, QImage::Format_RGB32);
        video.fill(QColor(23, 31, 43));
        page->setVideoFrame(video, 0, 0);
        QSignalSpy directions(page, &LiveLocalizationPage::plDirectionsChanged);
        QSignalSpy fusionAudio(page, &LiveLocalizationPage::audioFrameRequested);
        QFormLayout *form = nullptr;
        for (auto *group : window.findChildren<QGroupBox *>())
        {
            if (group->title() == QStringLiteral("设备连接"))
            {
                form = qobject_cast<QFormLayout *>(group->layout());
            }
        }
        QVERIFY(form);
        qobject_cast<QLineEdit *>(form->itemAt(1, QFormLayout::FieldRole)->widget())->setText("127.0.0.1");
        qobject_cast<QSpinBox *>(form->itemAt(2, QFormLayout::FieldRole)->widget())->setValue(server.serverPort());
        qobject_cast<QCheckBox *>(form->itemAt(3, QFormLayout::FieldRole)->widget())->setChecked(false);
        QVERIFY(QMetaObject::invokeMethod(&window, "connectOrDisconnect", Qt::DirectConnection));
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 2000);
        QTcpSocket *socket = server.nextPendingConnection();
        auto *recordingPage = window.findChild<QWidget *>("recordingPage");
        for (auto *edit : recordingPage->findChildren<QLineEdit *>())
        {
            if (edit->isReadOnly()) { edit->setText(recordings.path()); }
        }
        window.findChild<QPushButton *>("recordSelectAllButton")->click();
        window.findChild<QComboBox *>("recordingFileLayoutCombo")->setCurrentIndex(0);
        auto *start = window.findChild<QPushButton *>("startRecordingButton");
        auto *stop = window.findChild<QPushButton *>("stopRecordingButton");
        QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 2000);
        start->click();
        QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled(), 2000);
        QTest::qWait(50);
        QByteArray expected;
        for (quint16 seq = 1; seq <= 16; ++seq)
        {
            auto payload = coherentPcmFor(seq);
            const auto metadata = payloadFor(seq, seq);
            for (int channel = 7; channel < 48; ++channel) {
                payload[(3 * 48 + channel) * 4 + 3] = metadata[(3 * 48 + channel) * 4 + 3];
            }
            expected.append(coherentPcmFor(seq));
            QByteArray wire(16, 0);
            qToLittleEndian<quint16>(ProtocolCodec::HostHeader, wire.data());
            qToLittleEndian<quint16>(static_cast<quint16>(FrameType::Audio), wire.data() + 2);
            qToLittleEndian<quint32>(payload.size(), wire.data() + 4);
            qToLittleEndian<quint32>(1, wire.data() + 8);
            qToLittleEndian<quint32>(seq * 5000, wire.data() + 12);
            wire.append(payload);
            wire.append(QByteArray::fromHex("0a0d"));
            socket->write(wire.left(11)); // Exercise partial TCP header input.
            socket->flush();
            QTest::qWait(10);
            socket->write(wire.mid(11));
            socket->flush();
            QTest::qWait(80);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!directions.isEmpty()
            && qvariant_cast<QVector<SoundSourceDirection>>(directions.last().first()).size() == 1, 2000);
        QVERIFY(window.findChild<QLabel *>("plLocationStatsLabel")->text().contains("-30.00"));
        QTRY_COMPARE_WITH_TIMEOUT(fusionAudio.size(), 16, 2000);
        for (int index = 0; index < 16; ++index) {
            QCOMPARE(fusionAudio[index][0].toByteArray(), coherentPcmFor(index + 1));
        }
        QTRY_VERIFY_WITH_TIMEOUT(page->findChild<QLabel *>("referenceHeatmapStatusLabel")
                                    ->text().contains(QStringLiteral("已更新")), 8000);
        stop->click();
        QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(), 2000);
        const auto files = QDir(recordings.path()).entryList({"*.wav"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QFile wav(recordings.filePath(files.first()));
        QVERIFY(wav.open(QIODevice::ReadOnly));
        const auto bytes = wav.readAll();
        QCOMPARE(bytes.mid(68), expected);
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 22), quint16(48));
        const auto manifests = QDir(recordings.path()).entryList({"*.json"}, QDir::Files);
        QCOMPARE(manifests.size(), 1);
        QFile manifest(recordings.filePath(manifests.first()));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto metadata = QJsonDocument::fromJson(manifest.readAll()).object();
        QVERIFY(metadata.contains("gainApplied"));
        QVERIFY(!metadata.value("gainApplied").toBool());
        QVERIFY(QMetaObject::invokeMethod(&window, "connectOrDisconnect", Qt::DirectConnection));
        QTRY_VERIFY_WITH_TIMEOUT(qvariant_cast<QVector<SoundSourceDirection>>(directions.last().first()).isEmpty(), 2000);
    }

    void uiHasNoVoiceprintAndProducesReviewScreenshots()
    {
        MainWindow window;
        window.resize(1440, 900);
        window.show();
        QTest::qWait(100);
        auto *tabs = window.findChild<QTabWidget *>("mainTabs");
        QVERIFY(tabs);
        QVERIFY(window.findChild<QWidget *>("voiceprintPage") == nullptr);
        for (int i = 0; i < tabs->count(); ++i)
        {
            QVERIFY(!tabs->tabText(i).contains(QStringLiteral("声纹")));
            QVERIFY(!tabs->tabText(i).contains(QStringLiteral("人声自动")));
        }
        auto *raw = window.findChild<QCheckBox *>("recordRawPcmCheck");
        QVERIFY(raw && raw->isChecked());
        const QString output = qEnvironmentVariable("MICARRAY_QA_DIR");
        if (!output.isEmpty())
        {
            QDir().mkpath(output);
            QVERIFY(window.grab().save(output + "/monitor.png"));
            auto *page = window.findChild<LiveLocalizationPage *>("liveLocalizationPage");
            tabs->setCurrentWidget(page);
            QImage frame(1280, 720, QImage::Format_RGB32);
            frame.fill(QColor(45, 65, 80));
            page->setVideoFrame(frame, 0, 0);
            for (int seq = 1; seq <= 3; ++seq)
            {
                const auto packet = AudioLocationSidebandDecoder::decode(payloadFor(seq, seq), 0, 0);
                for (const auto &location : packet.locations) { page->setPlLocation(location); }
            }
            QTest::qWait(100);
            QVERIFY(window.grab().save(output + "/pl-localization.png"));
            auto *source = page->findChild<QComboBox *>("liveLocalizationSourceCombo");
            source->setCurrentIndex(source->findData("host"));
            QTest::qWait(100);
            QVERIFY(window.grab().save(output + "/host-localization.png"));
            tabs->setCurrentWidget(window.findChild<QWidget *>("recordingPage"));
            window.resize(1280, 800);
            QTest::qWait(100);
            QVERIFY(window.grab().save(output + "/recording.png"));
            window.findChild<QToolButton *>("themeToggleButton")->click();
            tabs->setCurrentWidget(page);
            source->setCurrentIndex(source->findData("pl"));
            QTest::qWait(100);
            QVERIFY(window.grab().save(output + "/pl-dark.png"));
            auto *mode = page->findChild<QComboBox *>("liveLocalizationDisplayModeCombo");
            mode->setCurrentIndex(mode->findData("fusion"));
            page->clearPlLocation();
            for (int sequence = 1; sequence <= 3; ++sequence) { deliverToPage(*page, sequence, 0, 0); }
            for (int index = 0; index < 16; ++index) {
                page->submitAdjustedAudioFrame(coherentPcmFor(index), 20, index * 4167);
            }
            QTRY_VERIFY_WITH_TIMEOUT(page->findChild<QLabel *>("referenceHeatmapStatusLabel")
                                        ->text().contains(QStringLiteral("已更新")), 8000);
            QVERIFY(window.grab().save(output + "/pl-fusion.png"));
        }
    }


    void tcpPipelineDisplaysPlHeatmapAndMultipleLocations()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        MainWindow window;
        auto *page = window.findChild<LiveLocalizationPage *>("liveLocalizationPage");
        auto *tabs = window.findChild<QTabWidget *>("mainTabs");
        if (tabs == nullptr) { tabs = window.findChild<QTabWidget *>(); }
        QVERIFY(page && tabs);
        tabs->setCurrentWidget(page);
        page->setActive(true);
        auto *source = page->findChild<QComboBox *>("liveLocalizationSourceCombo");
        source->setCurrentIndex(source->findData("pl"));
        auto *mode = page->findChild<QComboBox *>("liveLocalizationDisplayModeCombo");
        mode->setCurrentIndex(mode->findData("plheatmap"));
        QImage image(640, 360, QImage::Format_RGB32);
        image.fill(Qt::black);
        page->setVideoFrame(image, 1, 0);
        QSignalSpy directions(page, &LiveLocalizationPage::plDirectionsChanged);
        QSignalSpy softwareRequests(page, &LiveLocalizationPage::audioFrameRequested);
        QFormLayout *form = nullptr;
        for (auto *group : window.findChildren<QGroupBox *>())
        {
            if (group->title() == QStringLiteral("设备连接"))
            {
                form = qobject_cast<QFormLayout *>(group->layout());
            }
        }
        QVERIFY(form);
        qobject_cast<QLineEdit *>(form->itemAt(1, QFormLayout::FieldRole)->widget())->setText("127.0.0.1");
        qobject_cast<QSpinBox *>(form->itemAt(2, QFormLayout::FieldRole)->widget())->setValue(server.serverPort());
        qobject_cast<QCheckBox *>(form->itemAt(3, QFormLayout::FieldRole)->widget())->setChecked(false);
        QVERIFY(QMetaObject::invokeMethod(&window, "connectOrDisconnect", Qt::DirectConnection));
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 2000);
        QTcpSocket *socket = server.nextPendingConnection();
        QFile fixture(QStringLiteral(MICARRAY_PL_FIXTURE));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        QByteArray original;
        for (const auto &line : fixture.readAll().split('\n'))
        {
            if (line.trimmed().isEmpty()) { continue; }
            char bytes[4];
            qToLittleEndian(line.trimmed().toUInt(nullptr, 16), bytes);
            original.append(bytes, 4);
        }
        QCOMPARE(original.size(), AudioFrameDecoder::FramePayloadBytes);
        for (quint16 seq = 1; seq <= 3; ++seq)
        {
            QByteArray payload = original;
            for (int sample = 0; sample < 200; ++sample)
            {
                QByteArray packet(41, '\0');
                for (int byte = 0; byte < 41; ++byte)
                {
                    packet[byte] = payload[(sample * 48 + byte + 7) * 4 + 3];
                }
                if (packet.left(4) != QByteArrayLiteral("HMP1")) { continue; }
                if ((packet[5] & 7) == 0) { packet.fill('\0'); }
                else
                {
                    qToLittleEndian(seq, packet.data() + 6);
                    qToLittleEndian(seq, packet.data() + 8);
                    quint8 crc = 0;
                    for (int i = 0; i < 40; ++i)
                    {
                        crc ^= static_cast<quint8>(packet.at(i));
                        for (int b = 0; b < 8; ++b) { crc = crc & 128u ? (crc << 1) ^ 7u : crc << 1; }
                    }
                    packet[40] = char(crc);
                }
                for (int byte = 0; byte < 41; ++byte)
                {
                    payload[(sample * 48 + byte + 7) * 4 + 3] = packet.at(byte);
                }
            }
            const auto locations = payloadFor(seq, seq, 3);
            for (int channel = 7; channel < 48; ++channel)
            {
                payload[channel * 4 + 3] = locations[(3 * 48 + channel) * 4 + 3];
            }
            QByteArray wire(16, '\0');
            qToLittleEndian<quint16>(ProtocolCodec::HostHeader, wire.data());
            qToLittleEndian<quint16>(static_cast<quint16>(FrameType::Audio), wire.data() + 2);
            qToLittleEndian<quint32>(payload.size(), wire.data() + 4);
            qToLittleEndian<quint32>(1, wire.data() + 8);
            qToLittleEndian<quint32>(seq * 5000, wire.data() + 12);
            wire += payload;
            wire += QByteArray::fromHex("0a0d");
            socket->write(wire.left(11));
            socket->flush();
            QTest::qWait(10);
            socket->write(wire.mid(11));
            socket->flush();
            QTest::qWait(80);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!directions.isEmpty()
            && qvariant_cast<QVector<SoundSourceDirection>>(directions.last().first()).size() == 2, 2000);
        auto *status = page->findChild<QLabel *>("referenceHeatmapStatusLabel");
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("帧 3")), 2000);
        QCOMPARE(softwareRequests.size(), 0);
        QVERIFY(QMetaObject::invokeMethod(&window, "connectOrDisconnect", Qt::DirectConnection));
        QTRY_VERIFY_WITH_TIMEOUT(qvariant_cast<QVector<SoundSourceDirection>>(directions.last().first()).isEmpty(), 2000);
        QVERIFY(!status->text().contains(QStringLiteral("帧 3")));
    }

private:
    QTemporaryDir m_settings;
};

QTEST_MAIN(PlIntegrationTest)
#include "test_plintegration.moc"
