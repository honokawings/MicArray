#include "analysis/calibrationprofile.h"
#include "ui/arrayheatmapwidget.h"
#include "ui/audioanalysispage.h"
#include "ui/directionphotowidget.h"
#include "ui/livelocalizationview.h"
#include "ui/livelocalizationpage.h"
#include "ui/displayformat.h"
#include "ui/trendchartwidget.h"
#include "ui/videopreviewwidget.h"
#include "ui/mainwindow.h"

#include <QSignalSpy>
#include <QCheckBox>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QtTest>

#include <limits>
#include <algorithm>

using namespace MicArray;

class DashboardTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void audioAnalysisPageProvidesOfflineControls();
    void directionPhotoWidgetMapsDirectionsIntoImageRect();
    void enteringAudioAnalysisDisablesOnlyRealtimePlayback();
    void liveLocalizationPageKeepsRealtimePlaybackAndSharesVideo();
    void clickingHeatmapCellSelectsChannel();
    void heatmapTracksPeakAndSelectedChannels();
    void heatmapProvidesFortyEightGainControls();
    void movingAutomaticGainSliderSwitchesChannelToManual();
    void heatmapBulkGainCommandsUpdateVisibleState();
    void videoDoubleClickRequestsFullScreen();
    void videoPreviewReportsActualVideoRectangle();
    void liveLocalizationViewAcceptsOnlyVideoClicks();
    void liveLocalizationPageProvidesCalibrationControls();
    void liveLocalizationPageLoadsCalibrationProfile();
    void trendChartRetainsSixtySamples();
    void mainWindowProvidesDashboardAndPresentationToggle();
    void heatmapSelectionUpdatesOutputChannel();
    void applyingPresetMarksValuesPendingWithoutSending();
    void formatsAdaptiveByteUnits();
    void mainWindowUsesAudioVideoFusionTitle();
    void debugPageProvidesPostTailPreview();
    void recordingPageProvidesChannelSelectionAndCommands();
    void recordingDefaultsToPersistedOutputChannel();
    void recordingPageFitsAtMinimumSupportedSize();
    void gainGridFitsAtMinimumSupportedSize();
    void audioVisualizationWaitsForRenderTick();
    void audioPipelineAllowsMultipleFramesInFlight();
    void heatmapAmplitudeRefreshKeepsStyleSheetStable();
};

void DashboardTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("MicArrayTests"));
    QCoreApplication::setApplicationName(QStringLiteral("MicArrayDashboardTests"));
}

void DashboardTest::audioAnalysisPageProvidesOfflineControls()
{
    AppSettings settings;
    AudioAnalysisPage page(&settings);
    page.resize(1200, 700);
    page.show();

    auto *open = page.findChild<QPushButton *>(QStringLiteral("analysisOpenWaveButton"));
    auto *play = page.findChild<QPushButton *>(QStringLiteral("analysisPlayButton"));
    auto *channel = page.findChild<QComboBox *>(QStringLiteral("analysisOutputChannelCombo"));
    auto *locator = page.findChild<QComboBox *>(QStringLiteral("analysisLocatorCombo"));
    auto *photo = page.findChild<DirectionPhotoWidget *>(QStringLiteral("directionPhotoWidget"));
    auto *horizontalFov = page.findChild<QDoubleSpinBox *>(QStringLiteral("cameraHorizontalFovSpin"));

    QVERIFY(open != nullptr);
    QVERIFY(play != nullptr);
    QVERIFY(channel != nullptr);
    QVERIFY(locator != nullptr);
    QVERIFY(photo != nullptr);
    QVERIFY(horizontalFov != nullptr);
    QCOMPARE(channel->count(), 48);
    QVERIFY(locator->findData(QStringLiteral("gcc-phat-tdoa")) >= 0);
    QVERIFY(!play->isEnabled());
}

void DashboardTest::directionPhotoWidgetMapsDirectionsIntoImageRect()
{
    DirectionPhotoWidget widget;
    widget.resize(640, 480);
    widget.setImage(QImage(1600, 900, QImage::Format_RGB32));
    CameraViewSettings camera;
    camera.horizontalFovDegrees = 90.0;
    camera.verticalFovDegrees = 60.0;
    widget.setCameraView(camera);

    SoundSourceDirection direction;
    direction.confidence = 0.9;
    widget.setDirections({direction});

    const QRectF imageRect = widget.imageRect();
    QCOMPARE(qRound(imageRect.width()), 640);
    QCOMPARE(qRound(imageRect.height()), 360);
    QCOMPARE(qRound(imageRect.top()), 60);
    const QVector<QPointF> points = widget.markerPoints();
    QCOMPARE(points.size(), 1);
    QVERIFY(QLineF(points.first(), imageRect.center()).length() < 1.0);
}

void DashboardTest::enteringAudioAnalysisDisablesOnlyRealtimePlayback()
{
    MainWindow window;
    window.show();
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabs"));
    auto *analysisPage = window.findChild<AudioAnalysisPage *>(QStringLiteral("audioAnalysisPage"));
    auto *playback = window.findChild<QCheckBox *>(QStringLiteral("audioPlaybackCheck"));
    auto *outputChannel = window.findChild<QSpinBox *>(QStringLiteral("outputChannelSpin"));
    QVERIFY(tabs != nullptr);
    QVERIFY(analysisPage != nullptr);
    QVERIFY(playback != nullptr);
    QVERIFY(outputChannel != nullptr);

    playback->setChecked(true);
    QVERIFY(playback->isChecked());
    tabs->setCurrentWidget(analysisPage);
    QCoreApplication::processEvents();

    QVERIFY(!playback->isChecked());
    QVERIFY(outputChannel->isEnabled());
    tabs->setCurrentIndex(0);
    QVERIFY(!playback->isChecked());
}

void DashboardTest::clickingHeatmapCellSelectsChannel()
{
    ArrayHeatmapWidget widget;
    widget.show();
    QSignalSpy spy(&widget, &ArrayHeatmapWidget::channelSelected);
    auto *cell = widget.findChild<QToolButton *>(QStringLiteral("channelCell_17"));
    QVERIFY(cell != nullptr);
    QTest::mouseClick(cell, Qt::LeftButton);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), 17);
    QCOMPARE(widget.selectedChannel(), 17);
}

void DashboardTest::heatmapTracksPeakAndSelectedChannels()
{
    ArrayHeatmapWidget widget;
    QVector<double> amplitudes(48, 0.1);
    amplitudes[23] = 1.0;
    widget.setAmplitudes(amplitudes, 5);
    QCOMPARE(widget.selectedChannel(), 5);
    QCOMPARE(widget.peakChannel(), 23);
    QVERIFY(widget.findChild<QToolButton *>(QStringLiteral("channelCell_5"))->property("selected").toBool());
    QVERIFY(widget.findChild<QToolButton *>(QStringLiteral("channelCell_23"))->property("peak").toBool());
}

void DashboardTest::heatmapProvidesFortyEightGainControls()
{
    ArrayHeatmapWidget widget;
    widget.resize(760, 320);
    widget.show();
    GainConfiguration configuration;
    configuration.shifts[7] = -3;
    configuration.automatic[7] = true;
    widget.setGainConfiguration(configuration);

    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        auto *slider = widget.findChild<QSlider *>(QStringLiteral("channelGainSlider_%1").arg(channel));
        auto *automatic = widget.findChild<QCheckBox *>(QStringLiteral("channelAutoGain_%1").arg(channel));
        QVERIFY(slider != nullptr);
        QVERIFY(automatic != nullptr);
        QCOMPARE(slider->orientation(), Qt::Vertical);
        QCOMPARE(slider->minimum(), GainConfiguration::MinimumShift);
        QCOMPARE(slider->maximum(), GainConfiguration::MaximumShift);
    }
    QCOMPARE(widget.findChild<QSlider *>(QStringLiteral("channelGainSlider_7"))->value(), -3);
    QVERIFY(widget.findChild<QCheckBox *>(QStringLiteral("channelAutoGain_7"))->isChecked());
    QVERIFY(widget.findChild<QToolButton *>(QStringLiteral("channelCell_7"))
                ->text()
                .contains(QStringLiteral("-3 bit A")));
}

void DashboardTest::movingAutomaticGainSliderSwitchesChannelToManual()
{
    ArrayHeatmapWidget widget;
    GainConfiguration configuration;
    configuration.automatic[5] = true;
    widget.setGainConfiguration(configuration);
    QSignalSpy shiftSpy(&widget, &ArrayHeatmapWidget::channelShiftChanged);
    QSignalSpy automaticSpy(&widget, &ArrayHeatmapWidget::channelAutomaticChanged);
    auto *slider = widget.findChild<QSlider *>(QStringLiteral("channelGainSlider_5"));
    auto *automatic = widget.findChild<QCheckBox *>(QStringLiteral("channelAutoGain_5"));
    QVERIFY(slider != nullptr);
    QVERIFY(automatic != nullptr);

    slider->setValue(2);

    QCOMPARE(automaticSpy.count(), 1);
    QCOMPARE(automaticSpy.first().at(0).toInt(), 5);
    QCOMPARE(automaticSpy.first().at(1).toBool(), false);
    QCOMPARE(shiftSpy.count(), 1);
    QCOMPARE(shiftSpy.first().at(0).toInt(), 5);
    QCOMPARE(shiftSpy.first().at(1).toInt(), 2);
    QVERIFY(!automatic->isChecked());
}

void DashboardTest::heatmapBulkGainCommandsUpdateVisibleState()
{
    ArrayHeatmapWidget widget;
    auto *allAutomatic = widget.findChild<QToolButton *>(QStringLiteral("gainAllAutomaticButton"));
    auto *allManual = widget.findChild<QToolButton *>(QStringLiteral("gainAllManualButton"));
    auto *reset = widget.findChild<QToolButton *>(QStringLiteral("gainResetButton"));
    QVERIFY(allAutomatic != nullptr);
    QVERIFY(allManual != nullptr);
    QVERIFY(reset != nullptr);
    QSignalSpy allSpy(&widget, &ArrayHeatmapWidget::setAllAutomaticRequested);
    QSignalSpy resetSpy(&widget, &ArrayHeatmapWidget::resetRequested);

    QTest::mouseClick(allAutomatic, Qt::LeftButton);
    QCOMPARE(allSpy.takeFirst().at(0).toBool(), true);
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        QVERIFY(widget.findChild<QCheckBox *>(QStringLiteral("channelAutoGain_%1").arg(channel))->isChecked());
    }
    QTest::mouseClick(allManual, Qt::LeftButton);
    QCOMPARE(allSpy.takeFirst().at(0).toBool(), false);
    widget.findChild<QSlider *>(QStringLiteral("channelGainSlider_11"))->setValue(4);
    QTest::mouseClick(reset, Qt::LeftButton);
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(widget.findChild<QSlider *>(QStringLiteral("channelGainSlider_11"))->value(), 0);
    QVERIFY(!widget.findChild<QCheckBox *>(QStringLiteral("channelAutoGain_11"))->isChecked());
    QVERIFY(widget.findChild<QScrollArea *>(QStringLiteral("gainArrayScrollArea")) != nullptr);
}

void DashboardTest::videoDoubleClickRequestsFullScreen()
{
    VideoPreviewWidget widget;
    widget.resize(320, 180);
    widget.show();
    QSignalSpy spy(&widget, &VideoPreviewWidget::fullScreenRequested);
    QTest::mouseDClick(&widget, Qt::LeftButton, Qt::NoModifier, widget.rect().center());
    QCOMPARE(spy.count(), 1);
}

void DashboardTest::liveLocalizationPageKeepsRealtimePlaybackAndSharesVideo()
{
    MainWindow window;
    window.show();
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabs"));
    auto *page = window.findChild<LiveLocalizationPage *>(
        QStringLiteral("liveLocalizationPage"));
    auto *playback = window.findChild<QCheckBox *>(
        QStringLiteral("audioPlaybackCheck"));
    auto *view = window.findChild<LiveLocalizationView *>(
        QStringLiteral("liveLocalizationView"));
    QVERIFY(tabs != nullptr);
    QVERIFY(page != nullptr);
    QVERIFY(playback != nullptr);
    QVERIFY(view != nullptr);

    playback->setChecked(true);
    tabs->setCurrentWidget(page);
    QCoreApplication::processEvents();
    QVERIFY(playback->isChecked());

    const QImage frame(800, 450, QImage::Format_RGB32);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "showVideoFrame", Qt::DirectConnection,
        Q_ARG(QImage, frame), Q_ARG(double, 30.0),
        Q_ARG(quint32, quint32(1)), Q_ARG(quint32, quint32(2))));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "renderPendingVideoFrame", Qt::DirectConnection));
    QCOMPARE(view->frame().size(), frame.size());
    tabs->setCurrentIndex(0);
    QVERIFY(playback->isChecked());
    playback->setChecked(false);
}

void DashboardTest::videoPreviewReportsActualVideoRectangle()
{
    VideoPreviewWidget widget;
    widget.resize(640, 480);
    widget.setFrame(QImage(1600, 900, QImage::Format_RGB32));
    const QRectF videoRect = widget.videoRect();
    QCOMPARE(qRound(videoRect.left()), 0);
    QCOMPARE(qRound(videoRect.top()), 60);
    QCOMPARE(qRound(videoRect.width()), 640);
    QCOMPARE(qRound(videoRect.height()), 360);
    const std::optional<QPointF> center =
        widget.normalizedVideoPoint(videoRect.center());
    QVERIFY(center.has_value());
    QVERIFY(QLineF(*center, QPointF(0.5, 0.5)).length() < 1.0e-6);
    QVERIFY(!widget.normalizedVideoPoint(QPointF(320, 20)).has_value());
}

void DashboardTest::liveLocalizationViewAcceptsOnlyVideoClicks()
{
    LiveLocalizationView widget;
    widget.resize(640, 480);
    widget.setFrame(QImage(1600, 900, QImage::Format_RGB32));
    widget.setCalibrationMode(true);
    widget.show();
    QSignalSpy clicked(&widget, &LiveLocalizationView::calibrationPointClicked);
    QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, QPoint(320, 240));
    QCOMPARE(clicked.count(), 1);
    const QPointF point = clicked.first().at(0).toPointF();
    QVERIFY(QLineF(point, QPointF(0.5, 0.5)).length() < 0.01);
    QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, QPoint(320, 20));
    QCOMPARE(clicked.count(), 1);
}

void DashboardTest::liveLocalizationPageProvidesCalibrationControls()
{
    AppSettings settings;
    LiveLocalizationSettings stored;
    stored.displayMode = QStringLiteral("heatmap");
    stored.heatmapGridColumns = 320;
    stored.heatmapGridRows = 180;
    stored.camera.centerAzimuthDegrees = 16.0;
    stored.camera.horizontalFovDegrees = 78.0;
    settings.saveLiveLocalization(stored);
    LiveLocalizationPage page(&settings);
    page.resize(1200, 700);
    page.show();

    auto *enabled = page.findChild<QCheckBox *>(
        QStringLiteral("liveLocalizationEnabledCheck"));
    auto *mode = page.findChild<QComboBox *>(
        QStringLiteral("liveLocalizationDisplayModeCombo"));
    auto *view = page.findChild<LiveLocalizationView *>(
        QStringLiteral("liveLocalizationView"));
    auto *gridColumns = page.findChild<QSpinBox *>(
        QStringLiteral("liveHeatmapGridColumnsSpin"));
    auto *gridRows = page.findChild<QSpinBox *>(
        QStringLiteral("liveHeatmapGridRowsSpin"));
    auto *centerAzimuth = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("liveCameraCenterAzimuthSpin"));
    auto *horizontalFov = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("liveCameraHorizontalFovSpin"));
    auto *start = page.findChild<QPushButton *>(
        QStringLiteral("startFourPointCalibrationButton"));
    auto *cancel = page.findChild<QPushButton *>(
        QStringLiteral("cancelFourPointCalibrationButton"));
    auto *progress = page.findChild<QLabel *>(
        QStringLiteral("liveCalibrationProgressLabel"));
    auto *undo = page.findChild<QPushButton *>(
        QStringLiteral("undoCalibrationPointButton"));
    auto *showPoints = page.findChild<QCheckBox *>(
        QStringLiteral("showCalibrationOverlayCheck"));
    auto *samples = page.findChild<QTableWidget *>(
        QStringLiteral("calibrationSamplesTable"));
    auto *exportButton = page.findChild<QPushButton *>(
        QStringLiteral("exportVideoCalibrationButton"));
    auto *importButton = page.findChild<QPushButton *>(
        QStringLiteral("importVideoCalibrationButton"));
    QVERIFY(enabled != nullptr);
    QVERIFY(mode != nullptr);
    QVERIFY(view != nullptr);
    QVERIFY(gridColumns != nullptr);
    QVERIFY(gridRows != nullptr);
    QVERIFY(centerAzimuth != nullptr);
    QVERIFY(horizontalFov != nullptr);
    QVERIFY(start != nullptr);
    QVERIFY(cancel != nullptr);
    QVERIFY(progress != nullptr);
    QVERIFY(undo != nullptr);
    QVERIFY(showPoints != nullptr);
    QVERIFY(samples != nullptr);
    QVERIFY(exportButton != nullptr);
    QVERIFY(importButton != nullptr);
    QVERIFY(!undo->isEnabled());
    QCOMPARE(samples->rowCount(), 4);
    QCOMPARE(samples->columnCount(), 7);
    QVERIFY(mode->findData(QStringLiteral("marker")) >= 0);
    QVERIFY(mode->findData(QStringLiteral("heatmap")) >= 0);
    QCOMPARE(mode->currentData().toString(), QStringLiteral("heatmap"));
    QCOMPARE(gridColumns->value(), 320);
    QCOMPARE(gridColumns->maximum(), 640);
    QCOMPARE(gridRows->value(), 180);
    QCOMPARE(gridRows->maximum(), 360);
    QCOMPARE(centerAzimuth->value(), 16.0);
    QCOMPARE(horizontalFov->value(), 78.0);
    QVERIFY(!cancel->isEnabled());

    QSignalSpy gridChanged(&page, &LiveLocalizationPage::spatialGridRequested);
    page.setVideoFrame(QImage(1280, 720, QImage::Format_RGB32), 0, 0);
    gridColumns->setValue(80);
    gridRows->setValue(45);
    QVERIFY(gridChanged.count() >= 3);
    const SpatialResponseGrid grid =
        qvariant_cast<SpatialResponseGrid>(gridChanged.last().at(0));
    QCOMPARE(grid.columns, 80);
    QCOMPARE(grid.rows, 45);
    QCOMPARE(grid.directions.size(), 80 * 45);
    const LiveLocalizationSettings updated = settings.liveLocalization();
    QCOMPARE(updated.heatmapGridColumns, 80);
    QCOMPARE(updated.heatmapGridRows, 45);
}

void DashboardTest::liveLocalizationPageLoadsCalibrationProfile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
    LiveLocalizationPage page(&settings);

    VideoCalibrationProfile profile;
    profile.camera.centerAzimuthDegrees = 12.0;
    profile.camera.centerElevationDegrees = -4.0;
    profile.camera.horizontalFovDegrees = 82.0;
    profile.camera.verticalFovDegrees = 56.0;
    profile.camera.rollDegrees = 3.0;
    profile.referenceVideoSize = {1920, 1080};
    profile.calibrationRmsPixels = 2.5;
    profile.savedAtUtc = QDateTime::currentDateTimeUtc();
    profile.samples = {
        {{-20.0, 10.0}, {0.1, 0.2}, 0.8, 1.2},
        {{20.0, 10.0}, {0.9, 0.2}, 0.82, 1.1},
        {{20.0, -10.0}, {0.9, 0.8}, 0.9, 1.0},
        {{-20.0, -10.0}, {0.1, 0.8}, 0.88, 1.3},
    };
    const QString path = directory.filePath(QStringLiteral("profile.json"));
    QString error;
    QVERIFY2(VideoCalibrationProfileStore::save(path, profile, &error),
             qPrintable(error));
    QVERIFY2(page.loadCalibrationProfile(path, &error), qPrintable(error));

    const LiveLocalizationSettings loaded = settings.liveLocalization();
    QCOMPARE(loaded.camera, profile.camera);
    QCOMPARE(loaded.calibrationSamples, profile.samples);
    QCOMPARE(loaded.calibrationReferenceVideoSize, profile.referenceVideoSize);
    auto *samples = page.findChild<QTableWidget *>(
        QStringLiteral("calibrationSamplesTable"));
    QVERIFY(samples != nullptr);
    QVERIFY(samples->item(0, 1) != nullptr);
    QVERIFY(samples->item(0, 1)->text() != QStringLiteral("--"));
}

void DashboardTest::trendChartRetainsSixtySamples()
{
    TrendChartWidget widget;
    for (int index = 0; index < 100; ++index) {
        widget.appendSample(index, index * 0.5);
    }
    QCOMPARE(widget.sampleCount(), 60);
}

void DashboardTest::mainWindowProvidesDashboardAndPresentationToggle()
{
    MainWindow window;
    window.show();
    auto *health = window.findChild<QLabel *>(QStringLiteral("healthStatusLabel"));
    auto *presentation = window.findChild<QPushButton *>(QStringLiteral("presentationModeButton"));
    QVERIFY(health != nullptr);
    QVERIFY(presentation != nullptr);
    QVERIFY(window.findChild<QToolButton *>(QStringLiteral("channelCell_0")) != nullptr);

    QTest::mouseClick(presentation, Qt::LeftButton);
    QTRY_VERIFY(window.isFullScreen());
    QTest::mouseClick(presentation, Qt::LeftButton);
    QTRY_VERIFY(!window.isFullScreen());
}

void DashboardTest::heatmapSelectionUpdatesOutputChannel()
{
    MainWindow window;
    window.show();
    auto *cell = window.findChild<QToolButton *>(QStringLiteral("channelCell_17"));
    auto *output = window.findChild<QSpinBox *>(QStringLiteral("outputChannelSpin"));
    QVERIFY(cell != nullptr);
    QVERIFY(output != nullptr);
    QTest::mouseClick(cell, Qt::LeftButton);
    QCOMPARE(output->value(), 17);
}

void DashboardTest::applyingPresetMarksValuesPendingWithoutSending()
{
    AppSettings settings;
    settings.resetAll();
    ControlPreset preset;
    preset.name = QStringLiteral("UI Test Preset");
    preset.values = {11, 12, 13, 14, 15, 16};
    QVERIFY(settings.saveControlPreset(preset));
    QCOMPARE(settings.controlPresets().size(), 1);

    MainWindow window;
    window.show();
    auto *combo = window.findChild<QComboBox *>(QStringLiteral("controlPresetCombo"));
    auto *apply = window.findChild<QPushButton *>(QStringLiteral("applyControlPresetButton"));
    auto *control0 = window.findChild<QSpinBox *>(QStringLiteral("controlSpin_0"));
    auto *control6 = window.findChild<QSpinBox *>(QStringLiteral("controlSpin_6"));
    auto *control7 = window.findChild<QSpinBox *>(QStringLiteral("controlSpin_7"));
    auto *send = window.findChild<QPushButton *>(QStringLiteral("sendControlButton"));
    QVERIFY(combo != nullptr);
    QVERIFY(apply != nullptr);
    QVERIFY(control0 != nullptr);
    QVERIFY(control6 == nullptr);
    QVERIFY(control7 == nullptr);
    QVERIFY(send != nullptr);
    const int presetIndex = combo->findText(preset.name);
    QVERIFY(presetIndex >= 0);
    combo->setCurrentIndex(presetIndex);
    QTest::mouseClick(apply, Qt::LeftButton);
    QCOMPARE(control0->value(), 11);
    QVERIFY(send->text().contains(QLatin1Char('*')));
    QVERIFY(!send->text().contains(QStringLiteral("已发送")));
    settings.resetAll();
}

void DashboardTest::formatsAdaptiveByteUnits()
{
    QCOMPARE(formatByteSize(0), QStringLiteral("0 B"));
    QCOMPARE(formatByteSize(1023), QStringLiteral("1023 B"));
    QCOMPARE(formatByteSize(1024), QStringLiteral("1 KiB"));
    QCOMPARE(formatByteSize(1536), QStringLiteral("1.5 KiB"));
    QCOMPARE(formatByteSize(1024 * 1024 - 1), QStringLiteral("1 MiB"));
    QCOMPARE(formatByteSize(5ULL * 1024 * 1024), QStringLiteral("5 MiB"));
    QCOMPARE(formatByteSize(3ULL * 1024 * 1024 * 1024), QStringLiteral("3 GiB"));
    QCOMPARE(formatByteRate(1536.0), QStringLiteral("1.5 KiB/s"));
    QCOMPARE(formatByteRate(-1.0), QStringLiteral("0 B/s"));
    QCOMPARE(formatByteRate(std::numeric_limits<double>::quiet_NaN()), QStringLiteral("0 B/s"));
    QCOMPARE(formatByteRate(std::numeric_limits<double>::infinity()), QStringLiteral("0 B/s"));
}

void DashboardTest::mainWindowUsesAudioVideoFusionTitle()
{
    MainWindow window;
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabs"));
    auto *monitorPage = window.findChild<QWidget *>(QStringLiteral("monitorPage"));
    QVERIFY(tabs != nullptr);
    QVERIFY(monitorPage != nullptr);
    const int index = tabs->indexOf(monitorPage);
    QVERIFY(index >= 0);
    QCOMPARE(tabs->tabText(index), QStringLiteral("音视频融合"));
}

void DashboardTest::debugPageProvidesPostTailPreview()
{
    MainWindow window;
    auto *postTail = window.findChild<QLabel *>(QStringLiteral("debugPostTailLabel"));
    QVERIFY(postTail != nullptr);
    QVERIFY(postTail->styleSheet().contains(QStringLiteral("monospace")));
}

void DashboardTest::recordingPageProvidesChannelSelectionAndCommands()
{
    MainWindow window;
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabs"));
    auto *recordingPage = window.findChild<QWidget *>(QStringLiteral("recordingPage"));
    auto *selectAll = window.findChild<QPushButton *>(QStringLiteral("recordSelectAllButton"));
    auto *clear = window.findChild<QPushButton *>(QStringLiteral("recordClearButton"));
    auto *selectCurrent = window.findChild<QPushButton *>(QStringLiteral("recordCurrentChannelButton"));
    auto *start = window.findChild<QPushButton *>(QStringLiteral("startRecordingButton"));
    auto *statistics = window.findChild<QLabel *>(QStringLiteral("recordingStatisticsLabel"));
    auto *resetSettings = window.findChild<QPushButton *>(QStringLiteral("resetSettingsButton"));
    QVERIFY(tabs != nullptr);
    QVERIFY(recordingPage != nullptr);
    QVERIFY(tabs->indexOf(recordingPage) >= 0);
    QCOMPARE(tabs->tabText(tabs->indexOf(recordingPage)), QStringLiteral("音频录制"));
    QVERIFY(selectAll != nullptr);
    QVERIFY(clear != nullptr);
    QVERIFY(selectCurrent != nullptr);
    QVERIFY(start != nullptr);
    QVERIFY(statistics != nullptr);
    QVERIFY(resetSettings != nullptr);
    QVERIFY(statistics->text().contains(QStringLiteral("0 B")));
    QVERIFY(!start->isEnabled());

    QVector<QToolButton *> channels;
    for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
        auto *button = window.findChild<QToolButton *>(QStringLiteral("recordChannel_%1").arg(channel));
        QVERIFY(button != nullptr);
        QVERIFY(button->isCheckable());
        channels.append(button);
    }

    QTest::mouseClick(selectAll, Qt::LeftButton);
    QCOMPARE(std::count_if(channels.cbegin(), channels.cend(), [](QToolButton *button) {
                 return button->isChecked();
             }),
             AudioFrameDecoder::ChannelCount);
    QTest::mouseClick(clear, Qt::LeftButton);
    QCOMPARE(std::count_if(channels.cbegin(), channels.cend(), [](QToolButton *button) {
                 return button->isChecked();
             }),
             0);

    auto *output = window.findChild<QSpinBox *>(QStringLiteral("outputChannelSpin"));
    QVERIFY(output != nullptr);
    output->setValue(17);
    QTest::mouseClick(selectCurrent, Qt::LeftButton);
    QCOMPARE(std::count_if(channels.cbegin(), channels.cend(), [](QToolButton *button) {
                 return button->isChecked();
             }),
             1);
    QVERIFY(channels.at(17)->isChecked());

    auto *sampleRate = window.findChild<QDoubleSpinBox *>(QStringLiteral("sampleRateSpin"));
    QVERIFY(sampleRate != nullptr);
    QVERIFY(QMetaObject::invokeMethod(&window,
                                      "updateRecordingState",
                                      Q_ARG(MicArray::RecordingState, MicArray::RecordingState::Recording)));
    QVERIFY(!selectAll->isEnabled());
    QVERIFY(!channels.at(17)->isEnabled());
    QVERIFY(!sampleRate->isEnabled());
    QVERIFY(!resetSettings->isEnabled());
}

void DashboardTest::recordingDefaultsToPersistedOutputChannel()
{
    AppSettings settings;
    settings.resetAll();
    AudioSettings audio;
    audio.outputChannel = 23;
    settings.saveAudio(audio);

    MainWindow window;
    for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
        auto *button = window.findChild<QToolButton *>(QStringLiteral("recordChannel_%1").arg(channel));
        QVERIFY(button != nullptr);
        QCOMPARE(button->isChecked(), channel == 23);
    }
    settings.resetAll();
}

void DashboardTest::recordingPageFitsAtMinimumSupportedSize()
{
    MainWindow window;
    window.resize(1366, 768);
    window.show();
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabs"));
    auto *page = window.findChild<QWidget *>(QStringLiteral("recordingPage"));
    QVERIFY(tabs != nullptr);
    QVERIFY(page != nullptr);
    tabs->setCurrentWidget(page);
    QCoreApplication::processEvents();

    QList<QRect> channelRects;
    for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
        auto *button = window.findChild<QToolButton *>(QStringLiteral("recordChannel_%1").arg(channel));
        QVERIFY(button != nullptr);
        QVERIFY(button->isVisible());
        QVERIFY(button->width() >= 58);
        QVERIFY(button->height() >= 44);
        const QRect pageRect(button->mapTo(page, QPoint(0, 0)), button->size());
        QVERIFY(page->rect().contains(pageRect));
        for (const QRect &existing : channelRects) {
            QVERIFY(!existing.intersects(pageRect));
        }
        channelRects.append(pageRect);
    }
}

void DashboardTest::gainGridFitsAtMinimumSupportedSize()
{
    MainWindow window;
    window.resize(1366, 768);
    window.show();
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabs"));
    auto *monitorPage = window.findChild<QWidget *>(QStringLiteral("monitorPage"));
    auto *scroll = window.findChild<QScrollArea *>(QStringLiteral("gainArrayScrollArea"));
    auto *content = window.findChild<QWidget *>(QStringLiteral("gainArrayContent"));
    QVERIFY(tabs != nullptr);
    QVERIFY(monitorPage != nullptr);
    QVERIFY(scroll != nullptr);
    QVERIFY(content != nullptr);
    tabs->setCurrentWidget(monitorPage);
    QCoreApplication::processEvents();
    QVERIFY(scroll->isVisible());
    QVERIFY(scroll->width() > 100);
    QVERIFY(scroll->height() > 80);

    QList<QRect> panelRects;
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        auto *panel = window.findChild<QWidget *>(QStringLiteral("channelGainPanel_%1").arg(channel));
        auto *slider = window.findChild<QSlider *>(QStringLiteral("channelGainSlider_%1").arg(channel));
        auto *automatic = window.findChild<QCheckBox *>(QStringLiteral("channelAutoGain_%1").arg(channel));
        QVERIFY(panel != nullptr);
        QVERIFY(slider != nullptr);
        QVERIFY(automatic != nullptr);
        QVERIFY(panel->size().width() > 0);
        QVERIFY(panel->size().height() > 0);
        QVERIFY(panel->rect().contains(QRect(slider->mapTo(panel, QPoint(0, 0)), slider->size())));
        QVERIFY(panel->rect().contains(QRect(automatic->mapTo(panel, QPoint(0, 0)), automatic->size())));
        const QRect panelRect(panel->mapTo(content, QPoint(0, 0)), panel->size());
        for (const QRect &existing : panelRects) {
            QVERIFY(!existing.intersects(panelRect));
        }
        panelRects.append(panelRect);
    }
}

void DashboardTest::audioVisualizationWaitsForRenderTick()
{
    MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    auto *heatmap = window.findChild<ArrayHeatmapWidget *>();
    QVERIFY(heatmap != nullptr);

    DecodedAudioFrame frame;
    frame.valid = true;
    frame.outputChannel = 5;
    frame.normalizedAmplitudes.fill(0.1, AudioFrameDecoder::ChannelCount);
    frame.normalizedAmplitudes[23] = 1.0;
    frame.outputPcm16 = QByteArray(AudioFrameDecoder::SamplesPerFrame * 2, char(0));

    QCOMPARE(heatmap->peakChannel(), -1);
    QVERIFY(QMetaObject::invokeMethod(&window,
                                      "showArrayFrame",
                                      Qt::DirectConnection,
                                      Q_ARG(MicArray::DecodedAudioFrame, frame)));
    QCOMPARE(heatmap->peakChannel(), -1);

    QVERIFY(QMetaObject::invokeMethod(&window,
                                      "renderPendingAudioFrame",
                                      Qt::DirectConnection));
    QCOMPARE(heatmap->peakChannel(), 23);
}

void DashboardTest::audioPipelineAllowsMultipleFramesInFlight()
{
    MainWindow window;
    HostFrame frame;
    frame.type = FrameType::Audio;

    for (int index = 0; index < 4; ++index) {
        QVERIFY(QMetaObject::invokeMethod(&window,
                                          "enqueueAudioFrame",
                                          Qt::DirectConnection,
                                          Q_ARG(MicArray::HostFrame, frame)));
    }

    QCOMPARE(window.property("audioFramesInFlight").toInt(), 4);
}

void DashboardTest::heatmapAmplitudeRefreshKeepsStyleSheetStable()
{
    ArrayHeatmapWidget widget;
    QVector<double> amplitudes(AudioFrameDecoder::ChannelCount, 0.1);
    amplitudes[10] = 1.0;
    widget.setAmplitudes(amplitudes, 5);

    QStringList styles;
    for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
        styles.append(widget.findChild<QToolButton *>(QStringLiteral("channelCell_%1").arg(channel))->styleSheet());
    }

    amplitudes.fill(0.2);
    amplitudes[10] = 0.9;
    widget.setAmplitudes(amplitudes, 5);
    for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
        QCOMPARE(widget.findChild<QToolButton *>(QStringLiteral("channelCell_%1").arg(channel))->styleSheet(),
                 styles.at(channel));
    }
}

QTEST_MAIN(DashboardTest)
#include "test_dashboard.moc"
