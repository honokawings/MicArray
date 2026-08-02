#include "analysis/calibrationprofile.h"
#include "analysis/directionprojection.h"
#include "control/directioncontrolmapper.h"
#include "ui/arrayheatmapwidget.h"
#include "ui/audioanalysispage.h"
#include "ui/audiowaveformwidget.h"
#include "ui/directionphotowidget.h"
#include "ui/livelocalizationview.h"
#include "ui/livelocalizationpage.h"
#include "ui/displayformat.h"
#include "ui/trendchartwidget.h"
#include "ui/uitheme.h"
#include "ui/videopreviewwidget.h"
#include "ui/mainwindow.h"
#include "ui/noisereductionpage.h"
#include "ui/processingnodelistwidget.h"
#include "ui/noisereductionvideowidget.h"
#include "ui/powerspectrumwidget.h"

#include <QSignalSpy>
#include <QAbstractButton>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QStandardPaths>
#include <QWheelEvent>
#include <QWindow>
#include <QtTest>

#include <limits>
#include <algorithm>

using namespace MicArray;

namespace {

class ParentChangeOverlayProbe : public QObject
{
public:
    explicit ParentChangeOverlayProbe(QWidget *overlay)
        : m_overlay(overlay)
    {
    }

    bool sawParentChange = false;
    bool overlayVisibleDuringParentChange = false;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::ParentChange) {
            sawParentChange = true;
            overlayVisibleDuringParentChange = m_overlay->isVisible();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget *m_overlay;
};

int contrastingPixelCount(const QImage &image)
{
    const QColor background = image.pixelColor(8, image.height() / 2);
    const QRect textArea(image.width() / 6,
                         image.height() / 5,
                         image.width() * 2 / 3,
                         image.height() * 3 / 5);
    int contrastingPixels = 0;
    for (int y = textArea.top(); y <= textArea.bottom(); ++y) {
        for (int x = textArea.left(); x <= textArea.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            const int redDifference = pixel.red() - background.red();
            const int greenDifference = pixel.green() - background.green();
            const int blueDifference = pixel.blue() - background.blue();
            if (redDifference * redDifference
                    + greenDifference * greenDifference
                    + blueDifference * blueDifference
                > 45 * 45) {
                ++contrastingPixels;
            }
        }
    }
    return contrastingPixels;
}

} // namespace

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
    void themeStylePreservesHeatmapAmplitudeColors();
    void heatmapProvidesFortyEightGainControls();
    void movingAutomaticGainSliderSwitchesChannelToManual();
    void heatmapBulkGainCommandsUpdateVisibleState();
    void videoDoubleClickRequestsFullScreen();
    void videoPreviewReportsActualVideoRectangle();
    void liveLocalizationViewAcceptsOnlyVideoClicks();
    void liveHeatmapRendersRefinedPeakAtSubgridCenter();
    void liveHeatmapMutesUntrustedRelativePeak();
    void liveHeatmapFocusExponentSuppressesShoulders();
    void liveHeatmapFocusSuppressesRefinedPeakShoulders();
    void liveHeatmapRejectsStaleGeneration();
    void liveHeatmapRejectsResponseWhileInactive();
    void liveHeatmapProjectsAngularAxesWithCameraRoll();
    void liveHeatmapRendersCorrelatedHessianShape();
    void liveHeatmapCachesUnchangedResponse();
    void liveLocalizationPageProvidesCalibrationControls();
    void liveCalibrationTableAdaptsToStyleChanges();
    void liveLocalizationTrustThresholdIsAdjustable();
    void liveLocalizationConfigurationStartsNewGeneration();
    void liveLocalizationPageLoadsCalibrationProfile();
    void trendChartRetainsSixtySamples();
    void uiThemeProvidesDistinctPalettesAndBulbIcons();
    void arrayHeatmapTitleFollowsApplicationTheme();
    void mainWindowProvidesDashboardAndPresentationToggle();
    void presentationButtonRoutesToCurrentPageFullscreen();
    void presentationRestoresMaximizedWindow();
    void presentationVideoSplitterRemainsStableAfterDiagnosticsUpdates();
    void mainWindowTogglesAndPersistsTheme();
    void heatmapSelectionUpdatesOutputChannel();
    void applyingPresetMarksValuesPendingWithoutSending();
    void formatsAdaptiveByteUnits();
    void mainWindowUsesAudioVideoFusionTitle();
    void monitorStatisticsDoNotContainUnlabelledProgressBar();
    void debugPageProvidesPostTailPreview();
    void recordingPageProvidesChannelSelectionAndCommands();
    void recordingSourceSelectionControlsStartAvailability();
    void recordingRoutesOnlyFramesFromTheActiveSource();
    void quickNoiseRecordingReusesRecordingPageConfiguration();
    void quickRecordingKeepsDetailedErrorAcrossRefreshes();
    void noiseRecordingContinuesAcrossPageSwitchAndStopsOnTerminalState();
    void lightThemeRecordingChannelsUseDedicatedReadableStyle();
    void recordingDefaultsToPersistedOutputChannel();
    void recordingPageFitsAtMinimumSupportedSize();
    void gainGridFitsAtMinimumSupportedSize();
    void audioVisualizationWaitsForRenderTick();
    void audioPipelineAllowsMultipleFramesInFlight();
    void heatmapAmplitudeRefreshKeepsStyleSheetStable();
    void powerSpectrumWidgetStoresRealtimeSeries();
    void noiseReductionVideoAcceptsOnlyVisibleVideoClicks();
    void noiseReductionVideoDragsMarkerAndFinishesOnce();
    void waveformWidgetAcceptsNamedFloatSeries();
    void noiseReductionPageProvidesRealtimeControlsAndDefaults();
    void noiseReductionListeningTabProvidesBufferAndQuickRecording();
    void noiseReductionRecordingRequirementKeepsWorkerActiveOffPage();
    void noiseReductionPageUsesTabbedReorderableDataFlow();
    void noiseReductionCriticalAnglesIgnoreMouseWheel();
    void noiseReductionVideoClickUpdatesSelectedTargetAndRequestsAllControls();
    void noiseReductionPageSendsDraggedMarkerOnlyOnRelease();
    void noiseReductionManualAnglesRemainContinuousAndShowQuantizedCodes();
    void enteringNoiseReductionDisablesRealtimePlaybackWithoutRestoringIt();
    void noiseReductionFullscreenUsesIndependentMirrorWidgets();
};

void DashboardTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("MicArrayTests"));
    QCoreApplication::setApplicationName(QStringLiteral("MicArrayDashboardTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings testSettings;
    testSettings.clear();
    testSettings.sync();
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
    auto *neuralEnabled = page.findChild<QCheckBox *>(
        QStringLiteral(
            "audioAnalysisNeuralPeakEnhancementCheck"));
    auto *neuralStrength =
        page.findChild<QDoubleSpinBox *>(
            QStringLiteral(
                "audioAnalysisNeuralPeakEnhancementStrengthSpin"));

    QVERIFY(open != nullptr);
    QVERIFY(play != nullptr);
    QVERIFY(channel != nullptr);
    QVERIFY(locator != nullptr);
    QVERIFY(photo != nullptr);
    QVERIFY(horizontalFov != nullptr);
    QVERIFY(neuralEnabled != nullptr);
    QVERIFY(neuralStrength != nullptr);
    QVERIFY(neuralEnabled->isChecked());
    QCOMPARE(neuralStrength->value(), 0.30);
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

void DashboardTest::themeStylePreservesHeatmapAmplitudeColors()
{
    QApplication::setPalette(paletteForUiTheme(UiTheme::Dark));
    qApp->setStyleSheet(styleSheetForUiTheme(UiTheme::Dark));

    ArrayHeatmapWidget widget;
    widget.resize(780, 760);
    QVector<double> amplitudes(48, 0.0);
    amplitudes[0] = 1.0;
    widget.setAmplitudes(amplitudes, 1);
    widget.show();
    QCoreApplication::processEvents();

    auto *cell = widget.findChild<QToolButton *>(QStringLiteral("channelCell_0"));
    QVERIFY(cell != nullptr);
    const QColor expected = cell->palette().color(QPalette::Button);
    const QColor rendered = cell->grab().toImage().pixelColor(10, 10);
    QCOMPARE(rendered, expected);

    QApplication::setPalette(paletteForUiTheme(UiTheme::Light));
    qApp->setStyleSheet(styleSheetForUiTheme(UiTheme::Light));
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

void DashboardTest::liveHeatmapRendersRefinedPeakAtSubgridCenter()
{
    LiveLocalizationView widget;
    widget.resize(640, 360);
    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(Qt::black);
    widget.setFrame(frame);
    widget.setDisplayMode(QStringLiteral("heatmap"));
    widget.setHeatmapOpacity(1.0);

    LiveSpatialResponse response;
    response.trustedDirection = true;
    response.columns = 4;
    response.rows = 3;
    response.normalizedValues.fill(0.0f, 12);
    response.normalizedValues[7] = 1.0f;
    response.reliability = 0.9;
    response.hasRefinedPeak = true;
    response.refinedPeak = {0.0, 0.0};
    response.azimuthStandardDeviationDegrees = 0.6;
    response.elevationStandardDeviationDegrees = 0.6;
    widget.setSpatialResponse(response);
    widget.show();
    QCoreApplication::processEvents();

    const QImage rendered = widget.grab().toImage();
    const QPoint imageCenter(
        rendered.width() / 2,
        rendered.height() / 2);
    const QColor center =
        rendered.pixelColor(imageCenter);
    QVERIFY2(center.red() > 200
                 && center.red() > center.blue() + 80,
             qPrintable(QStringLiteral(
                 "center color=(%1,%2,%3)")
                 .arg(center.red())
                 .arg(center.green())
                 .arg(center.blue())));

    response.hasRefinedPeak = false;
    widget.setSpatialResponse(response);
    QCoreApplication::processEvents();
    const QImage withoutRefinedPeakImage =
        widget.grab().toImage();
    const QColor withoutRefinedPeak =
        withoutRefinedPeakImage.pixelColor(
            withoutRefinedPeakImage.width() / 2,
            withoutRefinedPeakImage.height() / 2);
    QVERIFY(withoutRefinedPeak.blue()
                > withoutRefinedPeak.red() + 80);
}

void DashboardTest::liveHeatmapMutesUntrustedRelativePeak()
{
    LiveLocalizationView widget;
    widget.resize(640, 360);
    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(Qt::black);
    widget.setFrame(frame);
    widget.setDisplayMode(QStringLiteral("heatmap"));
    widget.setHeatmapOpacity(1.0);
    widget.setHeatmapFocusExponent(0.5);

    LiveSpatialResponse response;
    response.columns = 1;
    response.rows = 1;
    response.normalizedValues = {1.0f};
    response.trustedDirection = false;
    widget.setSpatialResponse(response);
    widget.show();
    QCoreApplication::processEvents();

    const QColor untrusted =
        widget.grab().toImage().pixelColor(320, 180);
    QVERIFY2(untrusted.hsvHue() >= 140,
             qPrintable(QStringLiteral(
                 "untrusted color=(%1,%2,%3), hue=%4")
                            .arg(untrusted.red())
                            .arg(untrusted.green())
                            .arg(untrusted.blue())
                            .arg(untrusted.hsvHue())));

    response.trustedDirection = true;
    widget.setSpatialResponse(response);
    QCoreApplication::processEvents();
    const QColor trusted =
        widget.grab().toImage().pixelColor(320, 180);
    QVERIFY2(trusted.hsvHue() <= 10 || trusted.hsvHue() >= 350,
             qPrintable(QStringLiteral(
                 "trusted color=(%1,%2,%3), hue=%4")
                            .arg(trusted.red())
                            .arg(trusted.green())
                            .arg(trusted.blue())
                            .arg(trusted.hsvHue())));
}

void DashboardTest::liveHeatmapFocusExponentSuppressesShoulders()
{
    LiveLocalizationView widget;
    widget.resize(640, 360);
    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(Qt::black);
    widget.setFrame(frame);
    widget.setDisplayMode(QStringLiteral("heatmap"));
    widget.setHeatmapOpacity(1.0);

    LiveSpatialResponse response;
    response.trustedDirection = true;
    response.columns = 2;
    response.rows = 1;
    response.normalizedValues = {1.0f, 0.5f};
    widget.setSpatialResponse(response);
    widget.show();

    widget.setHeatmapFocusExponent(1.0);
    QCoreApplication::processEvents();
    const QImage neutral = widget.grab().toImage();
    const QColor neutralPeak = neutral.pixelColor(160, 180);
    const QColor neutralShoulder = neutral.pixelColor(480, 180);

    widget.setHeatmapFocusExponent(3.0);
    QCoreApplication::processEvents();
    const QImage focused = widget.grab().toImage();
    const QColor focusedPeak = focused.pixelColor(160, 180);
    const QColor focusedShoulder = focused.pixelColor(480, 180);

    QCOMPARE(focusedPeak, neutralPeak);
    QVERIFY2(
        focusedShoulder.hsvHue()
            > neutralShoulder.hsvHue() + 60,
        qPrintable(QStringLiteral(
            "neutral hue=%1, focused hue=%2")
                       .arg(neutralShoulder.hsvHue())
                       .arg(focusedShoulder.hsvHue())));
}

void DashboardTest::liveHeatmapFocusSuppressesRefinedPeakShoulders()
{
    LiveLocalizationView widget;
    widget.resize(640, 360);
    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(Qt::black);
    widget.setFrame(frame);
    widget.setDisplayMode(QStringLiteral("heatmap"));
    widget.setHeatmapOpacity(1.0);

    LiveSpatialResponse response;
    response.trustedDirection = true;
    response.columns = 4;
    response.rows = 1;
    response.normalizedValues = {
        1.0f, 0.0f, 0.0f, 0.0f,
    };
    response.reliability = 0.9;
    response.refinedPeak = {0.0, 0.0};
    response.azimuthStandardDeviationDegrees = 5.0;
    response.elevationStandardDeviationDegrees = 5.0;
    response.hasRefinedPeak = false;
    widget.setSpatialResponse(response);
    widget.show();

    widget.setHeatmapFocusExponent(1.0);
    QCoreApplication::processEvents();
    const QImage withoutRefinedPeakImage =
        widget.grab().toImage();

    response.hasRefinedPeak = true;
    widget.setSpatialResponse(response);
    QCoreApplication::processEvents();
    const QImage neutral = widget.grab().toImage();
    int shoulderX = -1;
    int closestShoulderHue = 241;
    for (int x = 240; x < 560; ++x) {
        const int baseHue =
            withoutRefinedPeakImage.pixelColor(x, 180)
                .hsvHue();
        const int refinedHue =
            neutral.pixelColor(x, 180).hsvHue();
        if (baseHue >= 230
            && refinedHue >= 40
            && refinedHue <= 180
            && std::abs(refinedHue - 110)
                < std::abs(closestShoulderHue - 110)) {
            shoulderX = x;
            closestShoulderHue = refinedHue;
        }
    }
    QVERIFY2(
        shoulderX >= 0,
        "refined peak did not create an isolated shoulder");
    const QColor neutralShoulder =
        neutral.pixelColor(shoulderX, 180);

    widget.setHeatmapFocusExponent(3.0);
    QCoreApplication::processEvents();
    const QImage focused = widget.grab().toImage();
    const QColor focusedShoulder =
        focused.pixelColor(shoulderX, 180);

    QVERIFY2(
        focusedShoulder.hsvHue()
            > neutralShoulder.hsvHue() + 40,
        qPrintable(QStringLiteral(
            "neutral refined hue=%1, focused refined hue=%2")
                       .arg(neutralShoulder.hsvHue())
                       .arg(focusedShoulder.hsvHue())));
}

void DashboardTest::liveHeatmapRejectsStaleGeneration()
{
    LiveLocalizationView widget;
    widget.resize(640, 360);
    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(Qt::black);
    widget.setFrame(frame);
    widget.setDisplayMode(QStringLiteral("heatmap"));
    widget.setHeatmapOpacity(1.0);
    widget.setExpectedSpatialResponseGeneration(2);

    LiveSpatialResponse response;
    response.trustedDirection = true;
    response.columns = 4;
    response.rows = 3;
    response.normalizedValues.fill(0.0f, 12);
    response.normalizedValues[5] = 1.0f;
    response.normalizedValues[6] = 1.0f;
    response.generation = 1;
    widget.setSpatialResponse(response);
    widget.show();
    QCoreApplication::processEvents();
    const QImage staleImage =
        widget.grab().toImage();
    const QColor staleCenter =
        staleImage.pixelColor(
            staleImage.width() / 2,
            staleImage.height() / 2);
    QVERIFY(staleCenter.red() < 40
            && staleCenter.green() < 40
            && staleCenter.blue() < 40);

    response.generation = 2;
    widget.setSpatialResponse(response);
    QCoreApplication::processEvents();
    const QImage currentImage =
        widget.grab().toImage();
    const QColor currentCenter =
        currentImage.pixelColor(
            currentImage.width() / 2,
            currentImage.height() / 2);
    QVERIFY(currentCenter.red() > 200);
}

void DashboardTest::liveHeatmapRejectsResponseWhileInactive()
{
    LiveLocalizationView widget;
    widget.resize(640, 360);
    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(Qt::black);
    widget.setFrame(frame);
    widget.setDisplayMode(QStringLiteral("heatmap"));
    widget.setHeatmapOpacity(1.0);
    widget.setSpatialResponseActive(false);

    LiveSpatialResponse response;
    response.trustedDirection = true;
    response.columns = 2;
    response.rows = 1;
    response.normalizedValues = {1.0f, 1.0f};
    widget.setSpatialResponse(response);
    widget.show();
    QCoreApplication::processEvents();
    const QColor inactiveCenter =
        widget.grab().toImage().pixelColor(320, 180);
    QVERIFY(inactiveCenter.red() < 40
            && inactiveCenter.green() < 40
            && inactiveCenter.blue() < 40);

    widget.setSpatialResponseActive(true);
    widget.setSpatialResponse(response);
    QCoreApplication::processEvents();
    const QColor activeCenter =
        widget.grab().toImage().pixelColor(320, 180);
    QVERIFY(activeCenter.red() > 200);
}

void DashboardTest::liveHeatmapProjectsAngularAxesWithCameraRoll()
{
    LiveLocalizationView widget;
    widget.resize(640, 360);
    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(Qt::black);
    widget.setFrame(frame);
    widget.setDisplayMode(QStringLiteral("heatmap"));
    widget.setHeatmapOpacity(1.0);
    CameraViewSettings camera;
    camera.horizontalFovDegrees = 90.0;
    camera.verticalFovDegrees = 60.0;
    camera.rollDegrees = 45.0;
    widget.setCameraView(camera);

    LiveSpatialResponse response;
    response.trustedDirection = true;
    response.columns = 4;
    response.rows = 3;
    response.normalizedValues.fill(0.0f, 12);
    response.normalizedValues[0] = 1.0f;
    response.reliability = 0.9;
    response.hasRefinedPeak = true;
    response.refinedPeak = {0.0, 0.0};
    response.azimuthStandardDeviationDegrees = 6.0;
    response.elevationStandardDeviationDegrees = 1.5;
    widget.setSpatialResponse(response);
    widget.show();
    QCoreApplication::processEvents();

    SoundSourceDirection centerDirection;
    SoundSourceDirection azimuthOffsetDirection;
    azimuthOffsetDirection.azimuthDegrees = 6.0;
    const ProjectionResult center =
        projectDirection(centerDirection, camera);
    const ProjectionResult azimuthOffset =
        projectDirection(azimuthOffsetDirection, camera);
    QVERIFY(center.valid && center.inView);
    QVERIFY(azimuthOffset.valid && azimuthOffset.inView);
    const QPointF centerPixel(
        center.normalizedPoint.x() * widget.width(),
        center.normalizedPoint.y() * widget.height());
    const QPointF azimuthBasis(
        (azimuthOffset.normalizedPoint.x()
         - center.normalizedPoint.x()) * widget.width(),
        (azimuthOffset.normalizedPoint.y()
         - center.normalizedPoint.y()) * widget.height());
    const QPointF alongAzimuth =
        centerPixel + 0.8 * azimuthBasis;
    const QPointF perpendicular =
        centerPixel + 0.8 * QPointF(
            -azimuthBasis.y(), azimuthBasis.x());

    const QImage rendered = widget.grab().toImage();
    const double imageScaleX =
        rendered.width()
        / static_cast<double>(widget.width());
    const double imageScaleY =
        rendered.height()
        / static_cast<double>(widget.height());
    const QColor alongColor = rendered.pixelColor(
        qRound(alongAzimuth.x() * imageScaleX),
        qRound(alongAzimuth.y() * imageScaleY));
    const QColor perpendicularColor = rendered.pixelColor(
        qRound(perpendicular.x() * imageScaleX),
        qRound(perpendicular.y() * imageScaleY));
    QVERIFY2(alongColor.red() > perpendicularColor.red() + 80,
             qPrintable(QStringLiteral(
                 "along=(%1,%2,%3), perpendicular=(%4,%5,%6)")
                 .arg(alongColor.red())
                 .arg(alongColor.green())
                 .arg(alongColor.blue())
                 .arg(perpendicularColor.red())
                 .arg(perpendicularColor.green())
                 .arg(perpendicularColor.blue())));
}

void DashboardTest::liveHeatmapRendersCorrelatedHessianShape()
{
    LiveLocalizationView widget;
    widget.resize(640, 360);
    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(Qt::black);
    widget.setFrame(frame);
    widget.setDisplayMode(QStringLiteral("heatmap"));
    widget.setHeatmapOpacity(1.0);
    CameraViewSettings camera;
    camera.horizontalFovDegrees = 90.0;
    camera.verticalFovDegrees = 60.0;
    widget.setCameraView(camera);

    LiveSpatialResponse response;
    response.trustedDirection = true;
    response.columns = 4;
    response.rows = 3;
    response.normalizedValues.fill(0.0f, 12);
    response.normalizedValues[0] = 1.0f;
    response.reliability = 0.9;
    response.hasRefinedPeak = true;
    response.refinedPeak = {0.0, 0.0};
    response.azimuthStandardDeviationDegrees = 4.0;
    response.elevationStandardDeviationDegrees = 4.0;
    response.azimuthElevationCorrelation = 0.8;
    widget.setSpatialResponse(response);
    widget.show();
    QCoreApplication::processEvents();

    SoundSourceDirection centerDirection;
    SoundSourceDirection azimuthOffsetDirection;
    SoundSourceDirection elevationOffsetDirection;
    azimuthOffsetDirection.azimuthDegrees = 4.0;
    elevationOffsetDirection.elevationDegrees = 4.0;
    const ProjectionResult center =
        projectDirection(centerDirection, camera);
    const ProjectionResult azimuthOffset =
        projectDirection(azimuthOffsetDirection, camera);
    const ProjectionResult elevationOffset =
        projectDirection(elevationOffsetDirection, camera);
    QVERIFY(center.valid && azimuthOffset.valid
            && elevationOffset.valid);
    const QPointF centerPixel(
        center.normalizedPoint.x() * widget.width(),
        center.normalizedPoint.y() * widget.height());
    const QPointF azimuthBasis(
        (azimuthOffset.normalizedPoint.x()
         - center.normalizedPoint.x()) * widget.width(),
        (azimuthOffset.normalizedPoint.y()
         - center.normalizedPoint.y()) * widget.height());
    const QPointF elevationBasis(
        (elevationOffset.normalizedPoint.x()
         - center.normalizedPoint.x()) * widget.width(),
        (elevationOffset.normalizedPoint.y()
         - center.normalizedPoint.y()) * widget.height());
    const QPointF correlated =
        centerPixel + 0.7 * (
            azimuthBasis + elevationBasis);
    const QPointF antiCorrelated =
        centerPixel + 0.7 * (
            azimuthBasis - elevationBasis);

    const QImage rendered = widget.grab().toImage();
    const double imageScaleX =
        rendered.width()
        / static_cast<double>(widget.width());
    const double imageScaleY =
        rendered.height()
        / static_cast<double>(widget.height());
    const QColor correlatedColor = rendered.pixelColor(
        qRound(correlated.x() * imageScaleX),
        qRound(correlated.y() * imageScaleY));
    const QColor antiCorrelatedColor = rendered.pixelColor(
        qRound(antiCorrelated.x() * imageScaleX),
        qRound(antiCorrelated.y() * imageScaleY));
    QVERIFY2(correlatedColor.red()
                 > antiCorrelatedColor.red() + 80,
             qPrintable(QStringLiteral(
                 "correlated=(%1,%2,%3), anti=(%4,%5,%6)")
                 .arg(correlatedColor.red())
                 .arg(correlatedColor.green())
                 .arg(correlatedColor.blue())
                 .arg(antiCorrelatedColor.red())
                 .arg(antiCorrelatedColor.green())
                 .arg(antiCorrelatedColor.blue())));
}

void DashboardTest::liveHeatmapCachesUnchangedResponse()
{
    LiveLocalizationView widget;
    widget.resize(640, 360);
    QImage frame(640, 360, QImage::Format_RGB32);
    frame.fill(Qt::black);
    widget.setFrame(frame);
    widget.setDisplayMode(QStringLiteral("heatmap"));
    widget.setHeatmapOpacity(1.0);
    LiveSpatialResponse response;
    response.trustedDirection = true;
    response.columns = 64;
    response.rows = 36;
    response.normalizedValues.fill(0.2f, 64 * 36);
    response.normalizedValues[18 * 64 + 32] = 1.0f;
    response.reliability = 0.9;
    response.hasRefinedPeak = true;
    response.refinedPeak = {0.0, 0.0};
    response.azimuthStandardDeviationDegrees = 1.0;
    response.elevationStandardDeviationDegrees = 1.0;
    widget.setSpatialResponse(response);
    widget.show();
    QCoreApplication::processEvents();
    widget.grab();

    QElapsedTimer elapsed;
    elapsed.start();
    for (int iteration = 0; iteration < 30; ++iteration) {
        widget.grab();
    }
    const qint64 milliseconds = elapsed.elapsed();
    qInfo() << "30 cached 640x360 heatmap paints:"
            << milliseconds << "ms";
    QVERIFY2(milliseconds <= 250,
             qPrintable(QStringLiteral(
                 "cached heatmap paints took %1 ms")
                 .arg(milliseconds)));
}

void DashboardTest::powerSpectrumWidgetStoresRealtimeSeries()
{
    PowerSpectrumWidget widget;
    const QVector<float> spectrum {-120.0f, -30.0f, -6.0f};
    widget.setSpectrum(QStringLiteral("增强 A"), spectrum, 48000);
    QCOMPARE(widget.sourceName(), QStringLiteral("增强 A"));
    QCOMPARE(widget.spectrum(), spectrum);
    QCOMPARE(widget.sampleRate(), 48000);
}

void DashboardTest::noiseReductionVideoAcceptsOnlyVisibleVideoClicks()
{
    NoiseReductionVideoWidget widget;
    widget.resize(640, 480);
    widget.setFrame(QImage(1600, 900, QImage::Format_RGB32));
    widget.setMarkersVisible(false);
    QVERIFY(!widget.markersVisible());
    widget.show();

    QSignalSpy clicked(&widget,
                       &NoiseReductionVideoWidget::videoPointClicked);
    QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier,
                      QPoint(320, 240));
    QCOMPARE(clicked.count(), 1);
    const QPointF point = clicked.first().at(0).toPointF();
    QVERIFY(QLineF(point, QPointF(0.5, 0.5)).length() < 0.01);
    QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier,
                      QPoint(320, 20));
    QCOMPARE(clicked.count(), 1);
}

void DashboardTest::noiseReductionVideoDragsMarkerAndFinishesOnce()
{
    NoiseReductionVideoWidget widget;
    widget.resize(640, 480);
    widget.setFrame(QImage(1600, 900, QImage::Format_RGB32));
    widget.setCameraView({0.0, 0.0, 120.0, 90.0, 0.0});
    widget.setDirectionTargets({0.0, 0.0},
                               {30.0, 0.0},
                               {-30.0, 0.0});
    widget.setCurrentSelection(NoiseDirectionSelection::EnhancedA);
    widget.show();
    QCoreApplication::processEvents();

    QSignalSpy started(
        &widget, &NoiseReductionVideoWidget::directionDragStarted);
    QSignalSpy dragged(
        &widget, &NoiseReductionVideoWidget::directionDragged);
    QSignalSpy finished(
        &widget, &NoiseReductionVideoWidget::directionDragFinished);
    QSignalSpy clicked(
        &widget, &NoiseReductionVideoWidget::videoPointClicked);
    const QPoint start = widget.videoRect().center().toPoint();
    const QPoint end = start + QPoint(60, -20);
    QTest::mousePress(
        &widget, Qt::LeftButton, Qt::NoModifier, start);
    QCOMPARE(started.count(), 1);
    QCOMPARE(clicked.count(), 0);
    QTest::mouseMove(&widget, end);
    QVERIFY(dragged.count() >= 1);
    QCOMPARE(finished.count(), 0);
    QTest::mouseRelease(
        &widget, Qt::LeftButton, Qt::NoModifier, end);
    QCOMPARE(finished.count(), 1);
}

void DashboardTest::waveformWidgetAcceptsNamedFloatSeries()
{
    AudioWaveformWidget widget;
    widget.setTitle(QStringLiteral("降噪输出"));
    widget.setSamples(QVector<float> {-1.0f, -0.5f, 0.5f, 1.0f});
    QCOMPARE(widget.title(), QStringLiteral("降噪输出"));
    QCOMPARE(widget.sampleCount(), 4);
}

void DashboardTest::noiseReductionPageProvidesRealtimeControlsAndDefaults()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
    NoiseReductionPage page(&settings);
    page.resize(1200, 760);
    page.show();

    QVERIFY(page.findChild<QCheckBox *>(
        QStringLiteral("noiseReductionEnabledCheck")));
    auto *mode = page.findChild<QComboBox *>(
        QStringLiteral("noiseReductionModeCombo"));
    auto *algorithm = page.findChild<QComboBox *>(
        QStringLiteral("noiseReductionAlgorithmCombo"));
    auto *singleCoefficient = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("noiseReductionSingleCoefficientSpin"));
    auto *dualB1Coefficient = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("noiseReductionDualB1CoefficientSpin"));
    auto *dualB2Coefficient = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("noiseReductionDualB2CoefficientSpin"));
    auto *monitorEnabled = page.findChild<QCheckBox *>(
        QStringLiteral("noiseReductionMonitorEnabledCheck"));
    auto *bandPassEnabled = page.findChild<QCheckBox *>(
        QStringLiteral("noiseReductionBandPassEnabledCheck"));
    auto *bandPassPreset = page.findChild<QComboBox *>(
        QStringLiteral("noiseReductionBandPassPresetCombo"));
    auto *bandPassLow = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("noiseReductionBandPassLowSpin"));
    auto *bandPassHigh = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("noiseReductionBandPassHighSpin"));
    auto *monitor = page.findChild<QComboBox *>(
        QStringLiteral("noiseReductionMonitorSourceCombo"));
    auto *first = page.findChild<QComboBox *>(
        QStringLiteral("noisePlotSource1Combo"));
    auto *second = page.findChild<QComboBox *>(
        QStringLiteral("noisePlotSource2Combo"));
    QVERIFY(mode);
    QVERIFY(algorithm);
    QVERIFY(singleCoefficient);
    QVERIFY(dualB1Coefficient);
    QVERIFY(dualB2Coefficient);
    QVERIFY(monitorEnabled);
    QVERIFY(bandPassEnabled);
    QVERIFY(bandPassPreset);
    QVERIFY(bandPassLow);
    QVERIFY(bandPassHigh);
    QVERIFY(monitor);
    QVERIFY(first);
    QVERIFY(second);
    QCOMPARE(mode->currentData().toInt(),
             static_cast<int>(NoiseReductionMode::SinglePoint));
    QCOMPARE(algorithm->currentData().toInt(),
             static_cast<int>(
                 NoiseReductionAlgorithm::FixedParameterWiener));
    QCOMPARE(singleCoefficient->value(), 3.0);
    QCOMPARE(dualB1Coefficient->value(), 5.0);
    QCOMPARE(dualB2Coefficient->value(), 5.0);
    QVERIFY(monitorEnabled->isChecked());
    QVERIFY(!bandPassEnabled->isChecked());
    QCOMPARE(bandPassPreset->currentData().toInt(),
             static_cast<int>(BandPassPreset::Voice));
    QCOMPARE(bandPassLow->value(), 100.0);
    QCOMPARE(bandPassHigh->value(), 8000.0);
    QCOMPARE(monitor->currentData().toInt(),
             static_cast<int>(NoiseSignalSource::Denoised));
    QCOMPARE(first->currentData().toInt(),
             static_cast<int>(NoiseSignalSource::EnhancedA));
    QCOMPARE(second->currentData().toInt(),
             static_cast<int>(NoiseSignalSource::Denoised));
    QCOMPARE(page.findChild<QDoubleSpinBox *>(
                 QStringLiteral("noiseElevationMinimumSpin"))->value(),
             -90.0);
    QCOMPARE(page.findChild<QDoubleSpinBox *>(
                 QStringLiteral("noiseElevationMaximumSpin"))->value(),
             90.0);
    QCOMPARE(page.findChild<QDoubleSpinBox *>(
                 QStringLiteral("noiseCameraHorizontalFovSpin"))->value(),
             180.0);
    QCOMPARE(page.findChild<QDoubleSpinBox *>(
                 QStringLiteral("noiseCameraVerticalFovSpin"))->value(),
             180.0);
    QCOMPARE(page.findChildren<AudioWaveformWidget *>().size(), 2);
    QCOMPARE(page.findChildren<PowerSpectrumWidget *>().size(), 4);
    QVERIFY(page.findChild<QPushButton *>(
        QStringLiteral("noiseReductionFullscreenButton")));
    QCOMPARE(page.findChild<QDoubleSpinBox *>(
                 QStringLiteral("noiseReductionMonitorGainSpin"))
                 ->maximum(),
             48.0);
}

void DashboardTest::noiseReductionListeningTabProvidesBufferAndQuickRecording()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString settingsPath =
        directory.filePath(QStringLiteral("settings.ini"));
    AppSettings settings(settingsPath);
    NoiseReductionPage page(&settings);
    page.resize(1200, 760);
    page.show();

    auto *buffer = page.findChild<QSpinBox *>(
        QStringLiteral("noiseMonitorBufferMilliseconds"));
    auto *quickRecording = page.findChild<QPushButton *>(
        QStringLiteral("noiseQuickRecordingButton"));
    auto *status = page.findChild<QLabel *>(
        QStringLiteral("noiseReductionStatusLabel"));
    QVERIFY(buffer != nullptr);
    QVERIFY(quickRecording != nullptr);
    QVERIFY(status != nullptr);
    QCOMPARE(buffer->minimum(), 50);
    QCOMPARE(buffer->maximum(), 1000);
    QCOMPARE(buffer->singleStep(), 50);
    QCOMPARE(buffer->suffix(), QStringLiteral(" ms"));
    QCOMPARE(buffer->value(), 250);
    QCOMPARE(quickRecording->text(), QStringLiteral("开始录制"));
    QVERIFY(!quickRecording->isEnabled());

    buffer->setValue(450);
    QCOMPARE(page.currentSettings().monitorBufferMilliseconds, 450);
    QCOMPARE(settings.noiseReduction().monitorBufferMilliseconds, 450);
    QSettings raw(settingsPath, QSettings::IniFormat);
    QCOMPARE(raw.value(QStringLiteral(
                 "noiseReduction/monitorBufferMilliseconds"))
                 .toInt(),
             450);

    buffer->setFocus();
    const QPointF local = buffer->rect().center();
    const QPointF global = buffer->mapToGlobal(
        buffer->rect().center());
    QWheelEvent wheel(local,
                      global,
                      QPoint(),
                      QPoint(0, 120),
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::ScrollUpdate,
                      false);
    QCoreApplication::sendEvent(buffer, &wheel);
    QCOMPARE(buffer->value(), 450);

    QSignalSpy toggleSpy(
        &page, &NoiseReductionPage::quickRecordingToggleRequested);
    page.setQuickRecordingState(
        false, true, QStringLiteral("录制状态：空闲"));
    QCOMPARE(quickRecording->text(), QStringLiteral("开始录制"));
    QVERIFY(quickRecording->isEnabled());
    QVERIFY(status->text().contains(QStringLiteral("降噪处理")));
    QVERIFY(status->text().contains(QStringLiteral("录制状态：空闲")));
    QTest::mouseClick(quickRecording, Qt::LeftButton);
    QCOMPARE(toggleSpy.count(), 1);

    page.setQuickRecordingState(
        true, true, QStringLiteral("录制状态：录制中"));
    QCOMPARE(quickRecording->text(), QStringLiteral("停止录制"));
    QVERIFY(quickRecording->isEnabled());
    QTest::mouseClick(quickRecording, Qt::LeftButton);
    QCOMPARE(toggleSpy.count(), 2);

    page.setQuickRecordingState(
        true, false, QStringLiteral("录制状态：正在停止"));
    QCOMPARE(quickRecording->text(), QStringLiteral("正在停止…"));
    QVERIFY(!quickRecording->isEnabled());
    QVERIFY(status->text().contains(
        QStringLiteral("录制状态：正在停止")));
}

void DashboardTest::
noiseReductionRecordingRequirementKeepsWorkerActiveOffPage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings(
        directory.filePath(QStringLiteral("settings.ini")));
    NoiseReductionSettings noiseSettings;
    noiseSettings.enabled = true;
    noiseSettings.monitorEnabled = false;
    settings.saveNoiseReduction(noiseSettings);
    NoiseReductionPage page(&settings);

    QSignalSpy activeSpy(
        &page, &NoiseReductionPage::workerActiveRequested);
    QSignalSpy rawFrameSpy(
        &page, &NoiseReductionPage::rawAudioFrameRequested);
    QSignalSpy recordingFrameSpy(
        &page, &NoiseReductionPage::noiseRecordingFrameReady);
    page.setActive(false);
    QCOMPARE(activeSpy.count(), 0);

    const QVector<NoiseReductionDataFlowStage> requestedStages {
        NoiseReductionDataFlowStage::Suppression,
        NoiseReductionDataFlowStage::Suppression,
        static_cast<NoiseReductionDataFlowStage>(99),
    };
    page.setRecordingRequired(true, requestedStages);
    QCOMPARE(activeSpy.count(), 1);
    QCOMPARE(activeSpy.takeFirst().at(0).toBool(), true);
    page.setRecordingRequired(true, requestedStages);
    QCOMPARE(activeSpy.count(), 0);

    const QByteArray payload(
        AudioFrameDecoder::FramePayloadBytes, char(0));
    for (int frameIndex = 0; frameIndex < 6; ++frameIndex) {
        page.submitRawAudioFrame(
            payload, 7, 500 + frameIndex * 4167);
    }
    QCOMPARE(page.submittedAudioFrames(), quint64(6));
    QCOMPARE(rawFrameSpy.count(), 6);
    QTRY_VERIFY_WITH_TIMEOUT(!recordingFrameSpy.isEmpty(), 2000);
    const NoiseReductionRecordingFrame frame =
        qvariant_cast<NoiseReductionRecordingFrame>(
            recordingFrameSpy.takeFirst().at(0));
    QCOMPARE(frame.stages.size(), 1);
    QCOMPARE(frame.stages.first().stage,
             NoiseReductionDataFlowStage::Suppression);
    QCOMPARE(frame.timestampSeconds, quint32(7));
    QCOMPARE(frame.timestampMicros, quint32(500));

    page.setActive(true);
    page.setActive(false);
    QCOMPARE(activeSpy.count(), 0);
    page.setRecordingRequired(false, requestedStages);
    QCOMPARE(activeSpy.count(), 1);
    QCOMPARE(activeSpy.takeFirst().at(0).toBool(), false);
    page.submitRawAudioFrame(payload, 8, 500);
    QCOMPARE(page.submittedAudioFrames(), quint64(6));
    QCOMPARE(rawFrameSpy.count(), 6);
}

void DashboardTest::noiseReductionPageUsesTabbedReorderableDataFlow()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
    NoiseReductionPage page(&settings);
    page.resize(1400, 800);
    page.show();
    QCoreApplication::processEvents();

    auto *panel = page.findChild<QWidget *>(
        QStringLiteral("noiseReductionControlPanel"));
    auto *tabs = page.findChild<QTabWidget *>(
        QStringLiteral("noiseReductionControlTabs"));
    auto *nodes = page.findChild<ProcessingNodeListWidget *>(
        QStringLiteral("noiseReductionProcessingNodeList"));
    auto *stack = page.findChild<QStackedWidget *>(
        QStringLiteral("noiseReductionNodeParameterStack"));
    QVERIFY(panel);
    QVERIFY(tabs);
    QVERIFY(nodes);
    QVERIFY(stack);
    QVERIFY(panel->minimumWidth() >= 440);
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(tabs->tabText(0), QStringLiteral("数据流"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("监听"));
    QCOMPARE(tabs->tabText(2), QStringLiteral("方向控制"));
    QCOMPARE(nodes->count(), 4);
    QCOMPARE(nodes->flow(), QListView::LeftToRight);
    QVERIFY(!nodes->isWrapping());
    QCOMPARE(nodes->horizontalScrollBarPolicy(),
             Qt::ScrollBarAlwaysOff);
    QCOMPARE(nodes->verticalScrollBarPolicy(),
             Qt::ScrollBarAlwaysOff);
    QVERIFY(!(nodes->item(0)->flags() & Qt::ItemIsDragEnabled));
    for (int row = 1; row < nodes->count(); ++row) {
        QVERIFY(nodes->item(row)->flags() & Qt::ItemIsDragEnabled);
    }
    QCoreApplication::processEvents();
    const QRect viewportRectangle = nodes->viewport()->rect();
    for (int row = 0; row < nodes->count(); ++row) {
        const QRect itemRectangle =
            nodes->visualItemRect(nodes->item(row));
        QVERIFY2(!itemRectangle.isEmpty(),
                 "processing node card has empty geometry");
        QVERIFY2(viewportRectangle.contains(itemRectangle),
                 "processing node card requires scrolling");
    }
    QCOMPARE(nodes->postProcessingOrder(),
             QVector<NoisePostProcessingNode>({
                 NoisePostProcessingNode::BandPass,
                 NoisePostProcessingNode::LogMmse,
                 NoisePostProcessingNode::AutomaticGainControl,
             }));

    nodes->setPostProcessingOrder({
        NoisePostProcessingNode::AutomaticGainControl,
        NoisePostProcessingNode::LogMmse,
        NoisePostProcessingNode::BandPass,
    });
    QCOMPARE(nodes->postProcessingOrder(),
             QVector<NoisePostProcessingNode>({
                 NoisePostProcessingNode::AutomaticGainControl,
                 NoisePostProcessingNode::LogMmse,
                 NoisePostProcessingNode::BandPass,
             }));
    nodes->setCurrentRow(1);
    QCOMPARE(stack->currentWidget()->objectName(),
             QStringLiteral("noiseReductionAgcParameters"));

    auto *logMmseEnabled = page.findChild<QCheckBox *>(
        QStringLiteral("noiseReductionLogMmseEnabledCheck"));
    auto *agcEnabled = page.findChild<QCheckBox *>(
        QStringLiteral("noiseReductionAgcEnabledCheck"));
    auto *fftSize = page.findChild<QComboBox *>(
        QStringLiteral("noiseReductionLogMmseFftSizeCombo"));
    auto *agcTarget = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("noiseReductionAgcTargetSpin"));
    QVERIFY(logMmseEnabled);
    QVERIFY(agcEnabled);
    QVERIFY(fftSize);
    QVERIFY(agcTarget);
    QVERIFY(!logMmseEnabled->isChecked());
    QVERIFY(!agcEnabled->isChecked());
    QCOMPARE(fftSize->currentData().toInt(), 512);
    QCOMPARE(agcTarget->value(), -18.0);
}

void DashboardTest::noiseReductionCriticalAnglesIgnoreMouseWheel()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
    NoiseReductionPage page(&settings);
    page.resize(1200, 760);
    page.show();
    QCoreApplication::processEvents();

    auto sendWheel = [](QDoubleSpinBox *spin) {
        spin->setFocus();
        const QPointF local = spin->rect().center();
        const QPointF global = spin->mapToGlobal(
            spin->rect().center());
        QWheelEvent event(local,
                          global,
                          QPoint(),
                          QPoint(0, 120),
                          Qt::NoButton,
                          Qt::NoModifier,
                          Qt::ScrollUpdate,
                          false);
        QCoreApplication::sendEvent(spin, &event);
    };

    const QStringList protectedNames {
        QStringLiteral("noiseDirectionAAzimuthDegreesSpin"),
        QStringLiteral("noiseAzimuthMinimumSpin"),
        QStringLiteral("noiseCameraHorizontalFovSpin"),
    };
    for (const QString &name : protectedNames) {
        auto *spin = page.findChild<QDoubleSpinBox *>(name);
        QVERIFY(spin);
        const double before = spin->value();
        sendWheel(spin);
        QCOMPARE(spin->value(), before);
    }

    auto *coefficient = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("noiseReductionSingleCoefficientSpin"));
    QVERIFY(coefficient);
    const double before = coefficient->value();
    sendWheel(coefficient);
    QVERIFY(coefficient->value() > before);
}

void DashboardTest::noiseReductionVideoClickUpdatesSelectedTargetAndRequestsAllControls()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
    NoiseReductionPage page(&settings);
    page.setConnected(true);
    page.setControlLocked(false);
    page.resize(1200, 760);
    page.show();

    auto *target = page.findChild<QComboBox *>(
        QStringLiteral("noiseDirectionTargetCombo"));
    auto *view = page.findChild<NoiseReductionVideoWidget *>(
        QStringLiteral("noiseReductionVideoView"));
    QVERIFY(target);
    QVERIFY(view);
    target->setCurrentIndex(target->findData(
        static_cast<int>(NoiseDirectionSelection::SuppressionB1)));
    page.setVideoFrame(QImage(1600, 900, QImage::Format_RGB32), 1, 2);
    QCoreApplication::processEvents();

    QSignalSpy requested(&page, &NoiseReductionPage::controlSendRequested);
    const QRectF video = view->videoRect();
    const QPoint click(qRound(video.left() + video.width() * 0.75),
                       qRound(video.center().y()));
    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, click);
    QCOMPARE(requested.count(), 1);
    const NoiseReductionSettings settingsAfterClick =
        page.currentSettings();
    QVERIFY(qAbs(settingsAfterClick.suppressionB1.azimuthDegrees - 45.0)
            < 0.5);
    QVERIFY(qAbs(settingsAfterClick.suppressionB1.elevationDegrees) < 0.5);
    const ControlValues values = page.currentControlValues();
    QCOMPARE(values.channelValue(0), qint16(0));
    QCOMPARE(values.channelValue(1), qint16(0));
    QCOMPARE(values.channelValue(2), qint16(2));
    QCOMPARE(values.channelValue(3), qint16(0));
    QCOMPARE(values.channelValue(4), qint16(0));
    QCOMPARE(values.channelValue(5), qint16(0));
}

void DashboardTest::noiseReductionPageSendsDraggedMarkerOnlyOnRelease()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
    NoiseReductionPage page(&settings);
    page.setConnected(true);
    page.setControlLocked(false);
    page.resize(1200, 760);
    page.show();
    page.setVideoFrame(
        QImage(1600, 900, QImage::Format_RGB32), 1, 2);
    QCoreApplication::processEvents();

    auto dragCenterMarker = [](NoiseReductionVideoWidget *view,
                               QSignalSpy *requested,
                               QPointF *releasePoint) {
        const QRectF video = view->videoRect();
        const QPoint start = view->videoRect().center().toPoint();
        const QPoint end = start + QPoint(45, 0);
        QTest::mousePress(
            view, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(view, end);
        QCOMPARE(requested->count(), 0);
        QTest::mouseRelease(
            view, Qt::LeftButton, Qt::NoModifier, end);
        QCOMPARE(requested->count(), 1);
        *releasePoint = QPointF(
            (end.x() - video.left()) / video.width(),
            (end.y() - video.top()) / video.height());
    };

    auto *normalVideo = page.findChild<NoiseReductionVideoWidget *>(
        QStringLiteral("noiseReductionVideoView"));
    QVERIFY(normalVideo);
    QSignalSpy normalRequested(
        &page, &NoiseReductionPage::controlSendRequested);
    QPointF normalRelease;
    dragCenterMarker(
        normalVideo, &normalRequested, &normalRelease);
    const double normalExpectedAzimuth =
        (normalRelease.x() - 0.5) * 180.0;
    QVERIFY(qAbs(page.currentSettings().enhancedA.azimuthDegrees
                 - normalExpectedAzimuth)
            < 0.5);

    QTemporaryDir presentationDirectory;
    QVERIFY(presentationDirectory.isValid());
    AppSettings presentationSettings(
        presentationDirectory.filePath(QStringLiteral("settings.ini")));
    NoiseReductionPage presentationPage(&presentationSettings);
    presentationPage.setConnected(true);
    presentationPage.setControlLocked(false);
    presentationPage.resize(1200, 760);
    presentationPage.show();
    presentationPage.setVideoFrame(
        QImage(1600, 900, QImage::Format_RGB32), 1, 2);
    QSignalSpy presentationRequested(
        &presentationPage,
        &NoiseReductionPage::controlSendRequested);
    presentationPage.showPresentation();
    auto *presentationVideo =
        presentationPage.findChild<NoiseReductionVideoWidget *>(
            QStringLiteral("noiseReductionPresentationVideo"));
    QVERIFY(presentationVideo);
    QTRY_VERIFY(presentationVideo->isVisible());
    QPointF presentationRelease;
    dragCenterMarker(
        presentationVideo,
        &presentationRequested,
        &presentationRelease);
    const double presentationExpectedAzimuth =
        (presentationRelease.x() - 0.5) * 180.0;
    QVERIFY(qAbs(
                presentationPage.currentSettings()
                    .enhancedA.azimuthDegrees
                - presentationExpectedAzimuth)
            < 0.5);
}

void DashboardTest::
    noiseReductionManualAnglesRemainContinuousAndShowQuantizedCodes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
    NoiseReductionPage page(&settings);
    page.resize(1200, 760);
    page.show();

    auto *azimuth = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("noiseDirectionB2AzimuthDegreesSpin"));
    auto *elevation = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("noiseDirectionB2ElevationDegreesSpin"));
    auto *azimuthCode = page.findChild<QLabel *>(
        QStringLiteral("noiseDirectionB2AzimuthCodeLabel"));
    auto *elevationCode = page.findChild<QLabel *>(
        QStringLiteral("noiseDirectionB2ElevationCodeLabel"));
    QVERIFY(azimuth);
    QVERIFY(elevation);
    QVERIFY(azimuthCode);
    QVERIFY(elevationCode);

    azimuth->setValue(17.3);
    elevation->setValue(-8.6);
    QCoreApplication::processEvents();

    const NoiseReductionSettings actual = page.currentSettings();
    QCOMPARE(actual.suppressionB2.azimuthDegrees, 17.3);
    QCOMPARE(actual.suppressionB2.elevationDegrees, -8.6);
    QCOMPARE(azimuth->value(), 17.3);
    QCOMPARE(elevation->value(), -8.6);
    QCOMPARE(azimuthCode->text(), QStringLiteral("1"));
    QCOMPARE(elevationCode->text(), QStringLiteral("0"));
}

void DashboardTest::enteringNoiseReductionDisablesRealtimePlaybackWithoutRestoringIt()
{
    MainWindow window;
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabs"));
    auto *playback = window.findChild<QCheckBox *>(
        QStringLiteral("audioPlaybackCheck"));
    auto *page = window.findChild<NoiseReductionPage *>(
        QStringLiteral("noiseReductionPage"));
    QVERIFY(tabs);
    QVERIFY(playback);
    QVERIFY(page);
    QCOMPARE(tabs->indexOf(page), 1);
    QCOMPARE(tabs->tabText(1), QStringLiteral("降噪处理"));

    playback->setChecked(true);
    tabs->setCurrentWidget(page);
    QCoreApplication::processEvents();
    QVERIFY(!playback->isChecked());
    tabs->setCurrentIndex(0);
    QVERIFY(!playback->isChecked());
}

void DashboardTest::noiseReductionFullscreenUsesIndependentMirrorWidgets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
    NoiseReductionPage page(&settings);
    page.resize(1200, 760);
    page.show();
    auto *normalVideo = page.findChild<NoiseReductionVideoWidget *>(
        QStringLiteral("noiseReductionVideoView"));
    auto *button = page.findChild<QPushButton *>(
        QStringLiteral("noiseReductionFullscreenButton"));
    auto *presentation = page.findChild<QWidget *>(
        QStringLiteral("noiseReductionPresentationWindow"));
    QVERIFY(normalVideo);
    QVERIFY(button);
    QVERIFY(presentation);
    QWidget *normalParent = normalVideo->parentWidget();

    QTest::mouseClick(button, Qt::LeftButton);
    QTRY_VERIFY(presentation->isFullScreen());
    QCOMPARE(normalVideo->parentWidget(), normalParent);
    QCOMPARE(presentation->findChildren<NoiseReductionVideoWidget *>().size(),
             1);
    QCOMPARE(presentation->findChildren<PowerSpectrumWidget *>().size(), 2);
    QVERIFY(presentation->findChildren<AudioWaveformWidget *>().isEmpty());
    QVERIFY(presentation->findChildren<QPushButton *>().isEmpty());

    QTest::keyClick(presentation, Qt::Key_Escape);
    QTRY_VERIFY(!presentation->isVisible());
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
    auto *controlTabs = page.findChild<QTabWidget *>(
        QStringLiteral("liveLocalizationControlTabs"));
    auto *realtimeTab = page.findChild<QWidget *>(
        QStringLiteral("liveLocalizationRealtimeTab"));
    auto *calibrationTab = page.findChild<QWidget *>(
        QStringLiteral("liveLocalizationCalibrationTab"));
    auto *mode = page.findChild<QComboBox *>(
        QStringLiteral("liveLocalizationDisplayModeCombo"));
    auto *resolution = page.findChild<QComboBox *>(
        QStringLiteral("liveSpatialResolutionModeCombo"));
    auto *neuralEnabled = page.findChild<QCheckBox *>(
        QStringLiteral(
            "liveLocalizationNeuralPeakEnhancementCheck"));
    auto *neuralStrength =
        page.findChild<QDoubleSpinBox *>(
            QStringLiteral(
                "liveLocalizationNeuralPeakEnhancementStrengthSpin"));
    auto *view = page.findChild<LiveLocalizationView *>(
        QStringLiteral("liveLocalizationView"));
    auto *gridColumns = page.findChild<QSpinBox *>(
        QStringLiteral("liveHeatmapGridColumnsSpin"));
    auto *gridRows = page.findChild<QSpinBox *>(
        QStringLiteral("liveHeatmapGridRowsSpin"));
    auto *focus = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("liveHeatmapFocusSpin"));
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
    QVERIFY(controlTabs != nullptr);
    QVERIFY(realtimeTab != nullptr);
    QVERIFY(calibrationTab != nullptr);
    QVERIFY(mode != nullptr);
    QVERIFY(resolution != nullptr);
    QVERIFY(neuralEnabled != nullptr);
    QVERIFY(neuralStrength != nullptr);
    QVERIFY(view != nullptr);
    QVERIFY(gridColumns != nullptr);
    QVERIFY(gridRows != nullptr);
    QVERIFY(focus != nullptr);
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
    QCOMPARE(controlTabs->count(), 2);
    QCOMPARE(controlTabs->tabText(0), QStringLiteral("实时定位"));
    QCOMPARE(controlTabs->tabText(1), QStringLiteral("视角校准"));
    QVERIFY(realtimeTab->isAncestorOf(focus));
    QVERIFY(calibrationTab->isAncestorOf(centerAzimuth));
    QVERIFY(calibrationTab->isAncestorOf(samples));
    QVERIFY(!undo->isEnabled());
    QCOMPARE(samples->rowCount(), 4);
    QCOMPARE(samples->columnCount(), 7);
    QVERIFY(mode->findData(QStringLiteral("marker")) >= 0);
    QVERIFY(mode->findData(QStringLiteral("heatmap")) >= 0);
    QCOMPARE(mode->currentData().toString(), QStringLiteral("heatmap"));
    QCOMPARE(resolution->currentData().toInt(),
             static_cast<int>(
                 SpatialResolutionMode::RealtimeEnhanced));
    QVERIFY(neuralEnabled->isChecked());
    QCOMPARE(neuralStrength->value(), 0.30);
    QCOMPARE(focus->value(), 1.5);
    QCOMPARE(gridColumns->value(), 320);
    QCOMPARE(gridColumns->maximum(), 640);
    QCOMPARE(gridRows->value(), 180);
    QCOMPARE(gridRows->maximum(), 360);
    QCOMPARE(centerAzimuth->value(), 16.0);
    QCOMPARE(horizontalFov->value(), 78.0);
    QVERIFY(!cancel->isEnabled());

    controlTabs->setCurrentWidget(calibrationTab);
    QCoreApplication::processEvents();
    int calibrationRowsHeight = 0;
    for (int row = 0; row < samples->rowCount(); ++row) {
        calibrationRowsHeight += samples->rowHeight(row);
    }
    QVERIFY(samples->viewport()->height()
            >= calibrationRowsHeight);
    QCOMPARE(samples->verticalScrollBarPolicy(),
             Qt::ScrollBarAlwaysOff);
    QCOMPARE(samples->verticalScrollBar()->maximum(), 0);
    controlTabs->setCurrentWidget(realtimeTab);

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
    QSignalSpy reconfigured(
        &page,
        &LiveLocalizationPage::configureWorkerRequested);
    const int gridCountBeforeFocus = gridChanged.count();
    focus->setValue(2.5);
    QCOMPARE(reconfigured.count(), 0);
    QCOMPARE(gridChanged.count(), gridCountBeforeFocus);
    QCOMPARE(settings.liveLocalization().heatmapFocusExponent, 2.5);
    resolution->setCurrentIndex(resolution->findData(
        static_cast<int>(
            SpatialResolutionMode::HighPrecision)));
    QCOMPARE(reconfigured.count(), 1);
    QCOMPARE(settings.liveLocalization().spatialResolutionMode,
             SpatialResolutionMode::HighPrecision);
    neuralEnabled->setChecked(false);
    neuralStrength->setValue(0.4);
    QCOMPARE(settings.liveLocalization()
                 .neuralPeakEnhancementEnabled,
             false);
    QCOMPARE(settings.liveLocalization()
                 .neuralPeakEnhancementStrength,
             0.4);
}

void DashboardTest::liveCalibrationTableAdaptsToStyleChanges()
{
    AppSettings settings;
    LiveLocalizationPage page(&settings);
    page.resize(1200, 560);
    page.show();

    auto *tabs = page.findChild<QTabWidget *>(
        QStringLiteral("liveLocalizationControlTabs"));
    auto *calibrationTab = page.findChild<QWidget *>(
        QStringLiteral("liveLocalizationCalibrationTab"));
    auto *samples = page.findChild<QTableWidget *>(
        QStringLiteral("calibrationSamplesTable"));
    QVERIFY(tabs != nullptr);
    QVERIFY(calibrationTab != nullptr);
    QVERIFY(samples != nullptr);
    tabs->setCurrentWidget(calibrationTab);

    page.setStyleSheet(QStringLiteral(
        "QTableWidget { font-size: 24px; }"
        "QHeaderView::section { padding: 12px; }"));
    samples->resizeRowsToContents();
    QCoreApplication::processEvents();

    int requiredRowsHeight = 0;
    for (int row = 0; row < samples->rowCount(); ++row) {
        requiredRowsHeight += samples->rowHeight(row);
    }
    QVERIFY2(
        samples->viewport()->height()
            >= requiredRowsHeight,
        qPrintable(QStringLiteral(
            "viewport=%1, rows=%2, table=%3")
                       .arg(samples->viewport()->height())
                       .arg(requiredRowsHeight)
                       .arg(samples->height())));
    QCOMPARE(samples->verticalScrollBar()->maximum(), 0);
}

void DashboardTest::liveLocalizationTrustThresholdIsAdjustable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(
        QStringLiteral("settings.ini"));
    AppSettings settings(path);
    LiveLocalizationPage page(&settings);
    page.resize(1200, 700);
    page.show();
    QCoreApplication::processEvents();

    auto *threshold = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("liveLocalizationConfidenceSpin"));
    auto *label = page.findChild<QLabel *>(
        QStringLiteral("liveLocalizationConfidenceLabel"));
    QVERIFY(threshold != nullptr);
    QVERIFY(label != nullptr);
    QCOMPARE(label->text(), QStringLiteral("可信判定阈值"));
    QCOMPARE(threshold->value(), 0.40);
    QCOMPARE(threshold->minimum(), 0.0);
    QCOMPARE(threshold->maximum(), 1.0);
    QCOMPARE(threshold->decimals(), 2);
    QCOMPARE(threshold->singleStep(), 0.02);
    QVERIFY(threshold->toolTip().contains(
        QStringLiteral("越低")));
    QVERIFY(threshold->toolTip().contains(
        QStringLiteral("误报")));

    threshold->setFocus();
    const QPointF local = threshold->rect().center();
    const QPointF global = threshold->mapToGlobal(
        threshold->rect().center());
    QWheelEvent wheel(local,
                      global,
                      QPoint(),
                      QPoint(0, 120),
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::ScrollUpdate,
                      false);
    QCoreApplication::sendEvent(threshold, &wheel);
    QCOMPARE(threshold->value(), 0.40);

    QSignalSpy gridChanged(
        &page,
        &LiveLocalizationPage::spatialGridRequested);
    page.setVideoFrame(
        QImage(640, 360, QImage::Format_RGB32), 0, 0);
    QVERIFY(!gridChanged.isEmpty());
    const quint64 previousGeneration =
        gridChanged.last().at(1).toULongLong();
    gridChanged.clear();
    QSignalSpy reconfigured(
        &page,
        &LiveLocalizationPage::configureWorkerRequested);
    threshold->setValue(0.36);
    QCOMPARE(reconfigured.count(), 1);
    QCOMPARE(gridChanged.count(), 1);
    QVERIFY(gridChanged.first().at(1).toULongLong()
            > previousGeneration);
    QCOMPARE(settings.liveLocalization().confidenceThreshold,
             0.36);
    QSettings raw(path, QSettings::IniFormat);
    QCOMPARE(raw.value(QStringLiteral(
                 "liveLocalization/trustedSourceThreshold"))
                 .toDouble(),
             0.36);
}

void DashboardTest::
liveLocalizationConfigurationStartsNewGeneration()
{
    AppSettings settings;
    LiveLocalizationPage page(&settings);
    QSignalSpy gridChanged(
        &page,
        &LiveLocalizationPage::spatialGridRequested);
    page.setVideoFrame(
        QImage(640, 360, QImage::Format_RGB32), 0, 0);
    QVERIFY(!gridChanged.isEmpty());
    const quint64 initialGeneration =
        gridChanged.last().at(1).toULongLong();

    auto *updateInterval = page.findChild<QSpinBox *>(
        QStringLiteral("liveLocalizationUpdateIntervalSpin"));
    auto *confidence = page.findChild<QDoubleSpinBox *>(
        QStringLiteral("liveLocalizationConfidenceSpin"));
    QVERIFY(updateInterval != nullptr);
    QVERIFY(confidence != nullptr);

    updateInterval->setValue(
        updateInterval->value() + 20);
    QVERIFY(gridChanged.last().at(1).toULongLong()
            > initialGeneration);
    const quint64 updateGeneration =
        gridChanged.last().at(1).toULongLong();

    confidence->setValue(
        std::min(1.0, confidence->value() + 0.1));
    QVERIFY(gridChanged.last().at(1).toULongLong()
            > updateGeneration);
    const quint64 confidenceGeneration =
        gridChanged.last().at(1).toULongLong();

    page.setActive(true);
    QVERIFY(gridChanged.last().at(1).toULongLong()
            > confidenceGeneration);
    const quint64 activeGeneration =
        gridChanged.last().at(1).toULongLong();
    page.setActive(false);
    QVERIFY(gridChanged.last().at(1).toULongLong()
            > activeGeneration);
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

void DashboardTest::uiThemeProvidesDistinctPalettesAndBulbIcons()
{
    QCOMPARE(uiThemeFromName(QStringLiteral("dark")), UiTheme::Dark);
    QCOMPARE(uiThemeFromName(QStringLiteral("invalid")), UiTheme::Light);
    QVERIFY(paletteForUiTheme(UiTheme::Light).color(QPalette::Window)
            != paletteForUiTheme(UiTheme::Dark).color(QPalette::Window));
    QVERIFY(!styleSheetForUiTheme(UiTheme::Dark).isEmpty());
    QVERIFY(!styleSheetForUiTheme(UiTheme::Light).contains(QStringLiteral("palette(disabled")));
    QVERIFY(!styleSheetForUiTheme(UiTheme::Dark).contains(QStringLiteral("palette(disabled")));
    QVERIFY(!bulbIconForUiTheme(UiTheme::Light).isNull());
    QVERIFY(!bulbIconForUiTheme(UiTheme::Dark).isNull());
    QVERIFY(bulbIconForUiTheme(UiTheme::Light).availableSizes().contains(QSize(48, 48)));
    QVERIFY(bulbIconForUiTheme(UiTheme::Light).availableSizes().contains(QSize(96, 96)));
}

void DashboardTest::arrayHeatmapTitleFollowsApplicationTheme()
{
    ArrayHeatmapWidget widget;
    auto *title = widget.findChild<QLabel *>(QStringLiteral("arrayHeatmapTitle"));
    QVERIFY(title != nullptr);
    QVERIFY(!title->styleSheet().contains(QStringLiteral("color:"), Qt::CaseInsensitive));
}

void DashboardTest::mainWindowProvidesDashboardAndPresentationToggle()
{
    MainWindow window;
    window.resize(1200, 700);
    window.show();
    auto *health = window.findChild<QLabel *>(QStringLiteral("healthStatusLabel"));
    auto *presentation = window.findChild<QPushButton *>(QStringLiteral("presentationModeButton"));
    auto *statusBand = window.findChild<QWidget *>(QStringLiteral("statusBand"));
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabs"));
    auto *splitter = window.findChild<QSplitter *>(QStringLiteral("monitorSplitter"));
    auto *left = window.findChild<QWidget *>(QStringLiteral("monitorLeftPanel"));
    auto *center = window.findChild<QWidget *>(QStringLiteral("monitorCenterPanel"));
    auto *right = window.findChild<QWidget *>(QStringLiteral("monitorDiagnosticsPanel"));
    auto *presentationDiagnostics = window.findChild<QWidget *>(
        QStringLiteral("presentationDiagnosticsPanel"));
    auto *presentationWindow = window.findChild<QWidget *>(
        QStringLiteral("presentationWindow"));
    auto *transitionOverlay = window.findChild<QLabel *>(
        QStringLiteral("presentationTransitionOverlay"));
    auto *presentationProtocolStats = window.findChild<QLabel *>(
        QStringLiteral("presentationProtocolStatsLabel"));
    auto *protocolStats = window.findChild<QLabel *>(QStringLiteral("protocolStatsLabel"));
    auto *errorLabel = window.findChild<QLabel *>(QStringLiteral("errorCountLabel"));
    auto *exportButton = window.findChild<QPushButton *>(QStringLiteral("exportDiagnosticsButton"));
    auto *video = window.findChild<VideoPreviewWidget *>(
        QStringLiteral("dashboardVideoPreview"));
    auto *windowStatusBar = window.statusBar();
    QVERIFY(health != nullptr);
    QVERIFY(presentation != nullptr);
    QVERIFY(statusBand != nullptr);
    QVERIFY(tabs != nullptr);
    QVERIFY(splitter != nullptr);
    QVERIFY(left != nullptr);
    QVERIFY(center != nullptr);
    QVERIFY(right != nullptr);
    QVERIFY(presentationDiagnostics != nullptr);
    QVERIFY(presentationWindow != nullptr);
    QVERIFY(transitionOverlay != nullptr);
    QVERIFY(presentationProtocolStats != nullptr);
    QVERIFY(protocolStats != nullptr);
    QVERIFY(errorLabel != nullptr);
    QVERIFY(exportButton != nullptr);
    QVERIFY(video != nullptr);
    QVERIFY(windowStatusBar->isVisible());
    QVERIFY(!presentationDiagnostics->isVisible());
    QVERIFY(window.findChild<QToolButton *>(QStringLiteral("channelCell_0")) != nullptr);
    splitter->setSizes({240, 700, 260});
    QCoreApplication::processEvents();
    const QList<int> normalSizes = splitter->sizes();
    const QRect normalGeometry = window.geometry();

    QTest::mouseClick(presentation, Qt::LeftButton);
    QTRY_VERIFY(presentationWindow->isFullScreen());
    QVERIFY(!window.isFullScreen());
    QCOMPARE(window.geometry(), normalGeometry);
    QVERIFY(statusBand->isVisible());
    QVERIFY(center->isVisible());
    QCOMPARE(statusBand->window(), presentationWindow);
    QCOMPARE(center->window(), presentationWindow);
    QVERIFY(presentationDiagnostics->isVisible());
    QVERIFY(presentationDiagnostics->geometry().left() >= video->geometry().right());
    QVERIFY(presentationDiagnostics->width() >= 240);
    QCOMPARE(presentationProtocolStats->text(), protocolStats->text());
    QVERIFY(!errorLabel->isVisible());
    QVERIFY(!exportButton->isVisible());
    QVERIFY(windowStatusBar->isVisible());
    QVERIFY(tabs->tabBar()->isVisible());

    ParentChangeOverlayProbe overlayProbe(transitionOverlay);
    center->installEventFilter(&overlayProbe);
    QTest::keyClick(presentationWindow, Qt::Key_Escape);
    QTRY_VERIFY(!presentationWindow->isVisible());
    QVERIFY(!window.isFullScreen());
    QVERIFY(left->isVisible());
    QVERIFY(right->isVisible());
    QCOMPARE(statusBand->window(), static_cast<QWidget *>(&window));
    QCOMPARE(center->window(), static_cast<QWidget *>(&window));
    QVERIFY(!presentationDiagnostics->isVisible());
    QVERIFY(errorLabel->isVisible());
    QVERIFY(exportButton->isVisible());
    QVERIFY(windowStatusBar->isVisible());
    QVERIFY(tabs->tabBar()->isVisible());
    QCOMPARE(window.geometry(), normalGeometry);
    QVERIFY(overlayProbe.sawParentChange);
    QVERIFY(overlayProbe.overlayVisibleDuringParentChange);
    QCOMPARE(splitter->sizes(), normalSizes);

    QTest::mouseClick(presentation, Qt::LeftButton);
    QTRY_VERIFY(presentationWindow->isFullScreen());
    QTest::mouseDClick(video, Qt::LeftButton, Qt::NoModifier, video->rect().center());
    QTRY_VERIFY(!presentationWindow->isVisible());
    QCOMPARE(splitter->sizes(), normalSizes);

    QTest::mouseClick(presentation, Qt::LeftButton);
    QTRY_VERIFY(presentationWindow->isFullScreen());
    QTest::mouseClick(presentation, Qt::LeftButton);
    QTRY_VERIFY(!presentationWindow->isVisible());
    QCOMPARE(splitter->sizes(), normalSizes);

    QTest::mouseClick(presentation, Qt::LeftButton);
    QTRY_VERIFY(presentationWindow->isFullScreen());
    presentationWindow->close();
    QTRY_VERIFY(!presentationWindow->isVisible());
    QCOMPARE(statusBand->window(), static_cast<QWidget *>(&window));
    QCOMPARE(center->window(), static_cast<QWidget *>(&window));
}

void DashboardTest::presentationButtonRoutesToCurrentPageFullscreen()
{
    MainWindow window;
    window.resize(1200, 700);
    window.show();

    auto *button = window.findChild<QPushButton *>(
        QStringLiteral("presentationModeButton"));
    auto *tabs = window.findChild<QTabWidget *>(
        QStringLiteral("mainTabs"));
    auto *noisePage = window.findChild<NoiseReductionPage *>(
        QStringLiteral("noiseReductionPage"));
    auto *fusionPresentation = window.findChild<QWidget *>(
        QStringLiteral("presentationWindow"));
    auto *noisePresentation = window.findChild<QWidget *>(
        QStringLiteral("noiseReductionPresentationWindow"));
    QVERIFY(button);
    QVERIFY(tabs);
    QVERIFY(noisePage);
    QVERIFY(fusionPresentation);
    QVERIFY(noisePresentation);

    tabs->setCurrentIndex(0);
    QTest::mouseClick(button, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(fusionPresentation->isFullScreen(), 1000);
    QVERIFY(!noisePresentation->isVisible());
    QTest::keyClick(fusionPresentation, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(!fusionPresentation->isVisible(), 1000);

    tabs->setCurrentWidget(noisePage);
    QCoreApplication::processEvents();
    QTest::mouseClick(button, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(noisePresentation->isFullScreen(), 1000);
    QVERIFY(!fusionPresentation->isVisible());
    QTest::keyClick(noisePresentation, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(!noisePresentation->isVisible(), 1000);
    QVERIFY(!fusionPresentation->isVisible());
}

void DashboardTest::presentationRestoresMaximizedWindow()
{
    MainWindow window;
    window.showMaximized();
    QTRY_VERIFY(window.isMaximized());
    auto *presentation = window.findChild<QPushButton *>(
        QStringLiteral("presentationModeButton"));
    auto *presentationWindow = window.findChild<QWidget *>(
        QStringLiteral("presentationWindow"));
    QVERIFY(presentation != nullptr);
    QVERIFY(presentationWindow != nullptr);
    QVERIFY(window.windowHandle() != nullptr);
    const Qt::WindowStates originalState = window.windowState();
    QSignalSpy windowStateSpy(window.windowHandle(), &QWindow::windowStateChanged);

    QTest::mouseClick(presentation, Qt::LeftButton);
    QTRY_VERIFY(presentationWindow->isFullScreen());
    QCOMPARE(window.windowState(), originalState);
    QCOMPARE(windowStateSpy.count(), 0);
    QTest::mouseClick(presentation, Qt::LeftButton);
    QTRY_VERIFY(!presentationWindow->isVisible());
    QCOMPARE(window.windowState(), originalState);
    QCOMPARE(windowStateSpy.count(), 0);
}

void DashboardTest::presentationVideoSplitterRemainsStableAfterDiagnosticsUpdates()
{
    MainWindow window;
    window.resize(1200, 700);
    window.show();
    auto *presentation = window.findChild<QPushButton *>(
        QStringLiteral("presentationModeButton"));
    auto *presentationWindow = window.findChild<QWidget *>(
        QStringLiteral("presentationWindow"));
    auto *videoSplitter = window.findChild<QSplitter *>(
        QStringLiteral("presentationVideoSplitter"));
    auto *video = window.findChild<VideoPreviewWidget *>(
        QStringLiteral("dashboardVideoPreview"));
    auto *diagnostics = window.findChild<QWidget *>(
        QStringLiteral("presentationDiagnosticsPanel"));
    auto *diagnosticsText = window.findChild<QLabel *>(
        QStringLiteral("presentationProtocolStatsLabel"));
    QVERIFY(presentation != nullptr);
    QVERIFY(presentationWindow != nullptr);
    QVERIFY(videoSplitter != nullptr);
    QVERIFY(video != nullptr);
    QVERIFY(diagnostics != nullptr);
    QVERIFY(diagnosticsText != nullptr);
    QCOMPARE(videoSplitter->orientation(), Qt::Horizontal);
    QVERIFY(videoSplitter->handleWidth() >= 6);
    QVERIFY(videoSplitter->styleSheet().contains(
        QStringLiteral("QSplitter::handle")));
    QCOMPARE(videoSplitter->count(), 1);
    QCOMPARE(diagnostics->parentWidget(), presentationWindow);

    QTest::mouseClick(presentation, Qt::LeftButton);
    QTRY_VERIFY(presentationWindow->isFullScreen());
    QCOMPARE(video->parentWidget(), static_cast<QWidget *>(videoSplitter));
    QCOMPARE(diagnostics->parentWidget(), static_cast<QWidget *>(videoSplitter));
    QVERIFY(videoSplitter->handle(1) != nullptr);
    videoSplitter->setSizes({900, 300});
    QCoreApplication::processEvents();
    const QList<int> manuallyAdjustedSizes = videoSplitter->sizes();
    QVERIFY(manuallyAdjustedSizes.at(0) > manuallyAdjustedSizes.at(1));

    for (int update = 0; update < 12; ++update) {
        diagnosticsText->setText(QString(update + 1, QLatin1Char('X')));
        QCoreApplication::processEvents();
        QCOMPARE(videoSplitter->sizes(), manuallyAdjustedSizes);
    }

    QTest::mouseClick(presentation, Qt::LeftButton);
    QTRY_VERIFY(!presentationWindow->isVisible());
    QCOMPARE(videoSplitter->count(), 1);
    QCOMPARE(diagnostics->parentWidget(), presentationWindow);
    QTest::mouseClick(presentation, Qt::LeftButton);
    QTRY_VERIFY(presentationWindow->isFullScreen());
    QTRY_COMPARE(videoSplitter->sizes(), manuallyAdjustedSizes);
}

void DashboardTest::mainWindowTogglesAndPersistsTheme()
{
    QSettings settings;
    settings.remove(QStringLiteral("appearance"));
    settings.sync();

    {
        MainWindow window;
        window.show();
        auto *themeButton = window.findChild<QToolButton *>(
            QStringLiteral("themeToggleButton"));
        QVERIFY(themeButton != nullptr);
        QVERIFY(themeButton->text().isEmpty());
        QVERIFY(!themeButton->icon().isNull());
        QCOMPARE(window.property("uiTheme").toString(), QStringLiteral("light"));
        QVERIFY(themeButton->toolTip().contains(QStringLiteral("深色")));

        QTest::mouseClick(themeButton, Qt::LeftButton);
        QCOMPARE(window.property("uiTheme").toString(), QStringLiteral("dark"));
        QVERIFY(themeButton->toolTip().contains(QStringLiteral("浅色")));
    }

    {
        MainWindow reloaded;
        reloaded.show();
        auto *themeButton = reloaded.findChild<QToolButton *>(
            QStringLiteral("themeToggleButton"));
        QVERIFY(themeButton != nullptr);
        QCOMPARE(reloaded.property("uiTheme").toString(), QStringLiteral("dark"));
        QTest::mouseClick(themeButton, Qt::LeftButton);
        QCOMPARE(reloaded.property("uiTheme").toString(), QStringLiteral("light"));
    }
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

void DashboardTest::monitorStatisticsDoNotContainUnlabelledProgressBar()
{
    MainWindow window;
    auto *center = window.findChild<QWidget *>(QStringLiteral("monitorCenterPanel"));
    QVERIFY(center != nullptr);
    QCOMPARE(center->findChildren<QProgressBar *>(QString(),
                                                 Qt::FindDirectChildrenOnly)
                 .size(),
             0);
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
    auto *sourceMode = window.findChild<QComboBox *>(QStringLiteral("recordingSourceModeCombo"));
    auto *sourceStack = window.findChild<QStackedWidget *>(QStringLiteral("recordingSourceStack"));
    auto *fileLayout = window.findChild<QComboBox *>(QStringLiteral("recordingFileLayoutCombo"));
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
    QVERIFY(sourceMode != nullptr);
    QVERIFY(sourceStack != nullptr);
    QVERIFY(fileLayout != nullptr);
    QCOMPARE(sourceMode->count(), 2);
    QCOMPARE(sourceMode->itemText(0), QStringLiteral("阵列通道"));
    QCOMPARE(sourceMode->itemData(0).toInt(), static_cast<int>(RecordingInputSource::Array));
    QCOMPARE(sourceMode->itemText(1), QStringLiteral("降噪数据流"));
    QCOMPARE(sourceMode->itemData(1).toInt(), static_cast<int>(RecordingInputSource::NoiseReductionStages));
    QCOMPARE(sourceMode->currentIndex(), 0);
    QCOMPARE(sourceStack->count(), 2);
    QCOMPARE(sourceStack->currentIndex(), 0);
    QCOMPARE(fileLayout->count(), 2);
    QCOMPARE(fileLayout->itemText(0), QStringLiteral("单个多通道文件"));
    QCOMPARE(fileLayout->itemData(0).toInt(), static_cast<int>(RecordingFileLayout::SingleMultichannel));
    QCOMPARE(fileLayout->itemText(1), QStringLiteral("分离为单声道文件"));
    QCOMPARE(fileLayout->itemData(1).toInt(), static_cast<int>(RecordingFileLayout::SeparateTracks));
    QCOMPARE(fileLayout->currentIndex(), 0);
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
    QVERIFY(!sourceMode->isEnabled());
    QVERIFY(!sourceStack->isEnabled());
    QVERIFY(!fileLayout->isEnabled());
}

void DashboardTest::recordingSourceSelectionControlsStartAvailability()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    MainWindow window;
    window.show();
    auto *recordingPage = window.findChild<QWidget *>(QStringLiteral("recordingPage"));
    auto *sourceMode = window.findChild<QComboBox *>(QStringLiteral("recordingSourceModeCombo"));
    auto *sourceStack = window.findChild<QStackedWidget *>(QStringLiteral("recordingSourceStack"));
    auto *start = window.findChild<QPushButton *>(QStringLiteral("startRecordingButton"));
    auto *quick = window.findChild<QPushButton *>(
        QStringLiteral("noiseQuickRecordingButton"));
    auto *noiseEnabled = window.findChild<QCheckBox *>(
        QStringLiteral("noiseReductionEnabledCheck"));
    auto *noiseStatus = window.findChild<QLabel *>(
        QStringLiteral("noiseReductionStatusLabel"));
    QVERIFY(recordingPage != nullptr);
    QVERIFY(sourceMode != nullptr);
    QVERIFY(sourceStack != nullptr);
    QVERIFY(start != nullptr);
    QVERIFY(quick != nullptr);
    QVERIFY(noiseEnabled != nullptr);
    QVERIFY(noiseStatus != nullptr);

    QLabel *stateLabel = nullptr;
    QLabel *errorLabel = nullptr;
    for (QLabel *label : recordingPage->findChildren<QLabel *>()) {
        if (label->text().startsWith(QStringLiteral("状态:"))) {
            stateLabel = label;
        }
        if (label->styleSheet().contains(QStringLiteral("#c74343"))) {
            errorLabel = label;
        }
    }
    QVERIFY(stateLabel != nullptr);
    QVERIFY(errorLabel != nullptr);
    noiseEnabled->setChecked(false);

    const QStringList stageObjectNames {
        QStringLiteral("recordingNoiseStage_suppression"),
        QStringLiteral("recordingNoiseStage_bandpass"),
        QStringLiteral("recordingNoiseStage_logmmse"),
        QStringLiteral("recordingNoiseStage_agc"),
    };
    const QStringList stageNames {
        QStringLiteral("降噪抑制"),
        QStringLiteral("带通滤波"),
        QStringLiteral("LogMMSE"),
        QStringLiteral("自动增益控制"),
    };
    QVector<QAbstractButton *> stageButtons;
    for (qsizetype index = 0; index < stageObjectNames.size(); ++index) {
        auto *button = window.findChild<QAbstractButton *>(stageObjectNames.at(index));
        QVERIFY(button != nullptr);
        QVERIFY(button->isCheckable());
        QVERIFY(button->isChecked());
        QCOMPARE(button->text(), stageNames.at(index));
        stageButtons.append(button);
    }

    sourceMode->setCurrentIndex(1);
    QCOMPARE(sourceStack->currentIndex(), 1);

    const auto recordingEdits = recordingPage->findChildren<QLineEdit *>();
    QCOMPARE(recordingEdits.size(), 1);
    QTemporaryDir outputDirectory;
    QVERIFY(outputDirectory.isValid());
    recordingEdits.first()->setText(outputDirectory.path());

    QGroupBox *connectionGroup = nullptr;
    for (QGroupBox *group : window.findChildren<QGroupBox *>()) {
        if (group->title() == QStringLiteral("设备连接")) {
            connectionGroup = group;
            break;
        }
    }
    QVERIFY(connectionGroup != nullptr);
    auto *connectionForm = qobject_cast<QFormLayout *>(connectionGroup->layout());
    QVERIFY(connectionForm != nullptr);
    auto *host = qobject_cast<QLineEdit *>(connectionForm->itemAt(1, QFormLayout::FieldRole)->widget());
    auto *port = qobject_cast<QSpinBox *>(connectionForm->itemAt(2, QFormLayout::FieldRole)->widget());
    auto *connectButton = qobject_cast<QPushButton *>(connectionForm->itemAt(4, QFormLayout::FieldRole)->widget());
    QVERIFY(host != nullptr);
    QVERIFY(port != nullptr);
    QVERIFY(connectButton != nullptr);
    host->setText(QStringLiteral("127.0.0.1"));
    port->setValue(server.serverPort());
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 1000);
    QCoreApplication::processEvents();
    QVERIFY(!start->isEnabled());
    QVERIFY(!quick->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(&window, "startRecording"));
    QCOMPARE(stateLabel->text(), QStringLiteral("状态: 空闲"));
    QVERIFY(errorLabel->text().contains(QStringLiteral("启用降噪处理")));
    QVERIFY(noiseStatus->text().contains(QStringLiteral("启用降噪处理")));

    noiseEnabled->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 1000);
    QVERIFY(quick->isEnabled());

    for (QAbstractButton *button : stageButtons) {
        button->setChecked(false);
    }
    QVERIFY(!start->isEnabled());
    QVERIFY(!quick->isEnabled());
    stageButtons.first()->setChecked(true);
    QVERIFY(start->isEnabled());
    QVERIFY(quick->isEnabled());
}

void DashboardTest::recordingRoutesOnlyFramesFromTheActiveSource()
{
    const auto stateLabelFor = [](QWidget *page) {
        for (QLabel *label : page->findChildren<QLabel *>()) {
            if (label->text().startsWith(QStringLiteral("状态:"))) {
                return label;
            }
        }
        return static_cast<QLabel *>(nullptr);
    };
    const auto errorLabelFor = [](QWidget *page) {
        for (QLabel *label : page->findChildren<QLabel *>()) {
            if (label->styleSheet().contains(QStringLiteral("#c74343"))) {
                return label;
            }
        }
        return static_cast<QLabel *>(nullptr);
    };
    const auto readOnlyDirectoryEditFor = [](QWidget *page) {
        for (QLineEdit *edit : page->findChildren<QLineEdit *>()) {
            if (edit->isReadOnly()) {
                return edit;
            }
        }
        return static_cast<QLineEdit *>(nullptr);
    };
    const auto readManifest = [](const QTemporaryDir &directory) {
        const QStringList manifests = QDir(directory.path()).entryList(
            {QStringLiteral("*.json")}, QDir::Files);
        if (manifests.size() != 1) {
            return QJsonObject {};
        }
        QFile file(QDir(directory.path()).filePath(manifests.first()));
        if (!file.open(QIODevice::ReadOnly)) {
            return QJsonObject {};
        }
        return QJsonDocument::fromJson(file.readAll()).object();
    };

    HostFrame rawFrame;
    rawFrame.type = FrameType::Audio;
    rawFrame.timestampSeconds = 7;
    rawFrame.timestampMicros = 500;
    rawFrame.payload = QByteArray(AudioFrameDecoder::FramePayloadBytes, char(0));
    NoiseReductionRecordingFrame noiseFrame;
    noiseFrame.sampleRate = 48000;
    noiseFrame.timestampSeconds = 7;
    noiseFrame.timestampMicros = 500;
    noiseFrame.stages.append({NoiseReductionDataFlowStage::Suppression,
                              QVector<float>(AudioFrameDecoder::SamplesPerFrame, 0.25F)});

    QTemporaryDir noiseDirectory;
    QVERIFY(noiseDirectory.isValid());
    {
        MainWindow window;
        auto *page = window.findChild<QWidget *>(QStringLiteral("recordingPage"));
        auto *sourceMode = window.findChild<QComboBox *>(QStringLiteral("recordingSourceModeCombo"));
        auto *fileLayout = window.findChild<QComboBox *>(QStringLiteral("recordingFileLayoutCombo"));
        auto *stop = window.findChild<QPushButton *>(QStringLiteral("stopRecordingButton"));
        auto *noiseEnabled = window.findChild<QCheckBox *>(
            QStringLiteral("noiseReductionEnabledCheck"));
        QVERIFY(page != nullptr);
        QVERIFY(sourceMode != nullptr);
        QVERIFY(fileLayout != nullptr);
        QVERIFY(stop != nullptr);
        QVERIFY(noiseEnabled != nullptr);
        QLabel *stateLabel = stateLabelFor(page);
        QLabel *errorLabel = errorLabelFor(page);
        QLineEdit *directoryEdit = readOnlyDirectoryEditFor(page);
        QVERIFY(stateLabel != nullptr);
        QVERIFY(errorLabel != nullptr);
        QVERIFY(directoryEdit != nullptr);
        directoryEdit->setText(noiseDirectory.path());
        sourceMode->setCurrentIndex(1);
        fileLayout->setCurrentIndex(1);
        noiseEnabled->setChecked(true);
        const QStringList stageObjectNames {
            QStringLiteral("recordingNoiseStage_suppression"),
            QStringLiteral("recordingNoiseStage_bandpass"),
            QStringLiteral("recordingNoiseStage_logmmse"),
            QStringLiteral("recordingNoiseStage_agc"),
        };
        for (qsizetype index = 0; index < stageObjectNames.size(); ++index) {
            auto *button = window.findChild<QAbstractButton *>(stageObjectNames.at(index));
            QVERIFY(button != nullptr);
            button->setChecked(index == 0);
        }

        QVERIFY(QMetaObject::invokeMethod(&window, "startRecording"));
        QTRY_VERIFY_WITH_TIMEOUT(stateLabel->text().contains(QStringLiteral("录制中")), 1000);
        QVERIFY(!sourceMode->isEnabled());
        QVERIFY(!fileLayout->isEnabled());
        sourceMode->setCurrentIndex(0);
        fileLayout->setCurrentIndex(0);
        QVERIFY(QMetaObject::invokeMethod(&window,
                                          "enqueueAudioFrame",
                                          Qt::DirectConnection,
                                          Q_ARG(MicArray::HostFrame, rawFrame)));
        QCOMPARE(window.property("audioFramesInFlight").toInt(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(window.property("audioFramesInFlight").toInt(), 0, 1000);
        QVERIFY(errorLabel->text().isEmpty());
        QVERIFY(stateLabel->text().contains(QStringLiteral("录制中")));
        QVERIFY(QMetaObject::invokeMethod(
            &window,
            "enqueueNoiseReductionRecordingFrame",
            Qt::DirectConnection,
            Q_ARG(MicArray::NoiseReductionRecordingFrame, noiseFrame)));
        QVERIFY(QMetaObject::invokeMethod(&window, "stopRecording"));
        QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(), 1000);
        QVERIFY(errorLabel->text().isEmpty());
    }
    const QJsonObject noiseManifest = readManifest(noiseDirectory);
    QVERIFY(!noiseManifest.isEmpty());
    QCOMPARE(noiseManifest.value(QStringLiteral("inputSource")).toString(),
             QStringLiteral("noiseReductionStages"));
    QCOMPARE(noiseManifest.value(QStringLiteral("fileLayout")).toString(),
             QStringLiteral("separateTracks"));
    QCOMPARE(noiseManifest.value(QStringLiteral("inputFrames")).toInt(), 1);
    QVERIFY(noiseManifest.value(QStringLiteral("dataBytes")).toInt() > 0);

    QTemporaryDir arrayDirectory;
    QVERIFY(arrayDirectory.isValid());
    {
        MainWindow window;
        auto *page = window.findChild<QWidget *>(QStringLiteral("recordingPage"));
        auto *sourceMode = window.findChild<QComboBox *>(QStringLiteral("recordingSourceModeCombo"));
        auto *fileLayout = window.findChild<QComboBox *>(QStringLiteral("recordingFileLayoutCombo"));
        auto *stop = window.findChild<QPushButton *>(QStringLiteral("stopRecordingButton"));
        QVERIFY(page != nullptr);
        QVERIFY(sourceMode != nullptr);
        QVERIFY(fileLayout != nullptr);
        QVERIFY(stop != nullptr);
        QLabel *stateLabel = stateLabelFor(page);
        QLabel *errorLabel = errorLabelFor(page);
        QLineEdit *directoryEdit = readOnlyDirectoryEditFor(page);
        QVERIFY(stateLabel != nullptr);
        QVERIFY(errorLabel != nullptr);
        QVERIFY(directoryEdit != nullptr);
        directoryEdit->setText(arrayDirectory.path());
        sourceMode->setCurrentIndex(0);

        QVERIFY(QMetaObject::invokeMethod(&window, "startRecording"));
        QTRY_VERIFY_WITH_TIMEOUT(stateLabel->text().contains(QStringLiteral("录制中")), 1000);
        QVERIFY(!sourceMode->isEnabled());
        QVERIFY(!fileLayout->isEnabled());
        sourceMode->setCurrentIndex(1);
        fileLayout->setCurrentIndex(1);
        QVERIFY(QMetaObject::invokeMethod(
            &window,
            "enqueueNoiseReductionRecordingFrame",
            Qt::DirectConnection,
            Q_ARG(MicArray::NoiseReductionRecordingFrame, noiseFrame)));
        QVERIFY(errorLabel->text().isEmpty());
        QVERIFY(stateLabel->text().contains(QStringLiteral("录制中")));
        QVERIFY(QMetaObject::invokeMethod(&window,
                                          "enqueueAudioFrame",
                                          Qt::DirectConnection,
                                          Q_ARG(MicArray::HostFrame, rawFrame)));
        QCOMPARE(window.property("audioFramesInFlight").toInt(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(window.property("audioFramesInFlight").toInt(), 0, 1000);
        QVERIFY(QMetaObject::invokeMethod(&window, "stopRecording"));
        QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(), 1000);
        QVERIFY(errorLabel->text().isEmpty());
    }
    const QJsonObject arrayManifest = readManifest(arrayDirectory);
    QVERIFY(!arrayManifest.isEmpty());
    QCOMPARE(arrayManifest.value(QStringLiteral("inputSource")).toString(),
             QStringLiteral("array"));
    QCOMPARE(arrayManifest.value(QStringLiteral("fileLayout")).toString(),
             QStringLiteral("singleMultichannel"));
    QCOMPARE(arrayManifest.value(QStringLiteral("inputFrames")).toInt(), 1);
    QVERIFY(arrayManifest.value(QStringLiteral("dataBytes")).toInt() > 0);
}

void DashboardTest::quickNoiseRecordingReusesRecordingPageConfiguration()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QTemporaryDir outputDirectory;
    QVERIFY(outputDirectory.isValid());

    MainWindow window;
    window.show();
    auto *recordingPage = window.findChild<QWidget *>(
        QStringLiteral("recordingPage"));
    auto *sourceMode = window.findChild<QComboBox *>(
        QStringLiteral("recordingSourceModeCombo"));
    auto *fileLayout = window.findChild<QComboBox *>(
        QStringLiteral("recordingFileLayoutCombo"));
    auto *quick = window.findChild<QPushButton *>(
        QStringLiteral("noiseQuickRecordingButton"));
    auto *start = window.findChild<QPushButton *>(
        QStringLiteral("startRecordingButton"));
    auto *stop = window.findChild<QPushButton *>(
        QStringLiteral("stopRecordingButton"));
    auto *statistics = window.findChild<QLabel *>(
        QStringLiteral("recordingStatisticsLabel"));
    auto *noisePage = window.findChild<NoiseReductionPage *>(
        QStringLiteral("noiseReductionPage"));
    auto *noiseEnabled = window.findChild<QCheckBox *>(
        QStringLiteral("noiseReductionEnabledCheck"));
    auto *monitorEnabled = window.findChild<QCheckBox *>(
        QStringLiteral("noiseReductionMonitorEnabledCheck"));
    QVERIFY(recordingPage != nullptr);
    QVERIFY(sourceMode != nullptr);
    QVERIFY(fileLayout != nullptr);
    QVERIFY(quick != nullptr);
    QVERIFY(start != nullptr);
    QVERIFY(stop != nullptr);
    QVERIFY(statistics != nullptr);
    QVERIFY(noisePage != nullptr);
    QVERIFY(noiseEnabled != nullptr);
    QVERIFY(monitorEnabled != nullptr);

    QLineEdit *directoryEdit = nullptr;
    QLabel *stateLabel = nullptr;
    for (QLineEdit *edit : recordingPage->findChildren<QLineEdit *>()) {
        if (edit->isReadOnly()) {
            directoryEdit = edit;
            break;
        }
    }
    for (QLabel *label : recordingPage->findChildren<QLabel *>()) {
        if (label->text().startsWith(QStringLiteral("状态:"))) {
            stateLabel = label;
            break;
        }
    }
    QVERIFY(directoryEdit != nullptr);
    QVERIFY(stateLabel != nullptr);
    directoryEdit->setText(outputDirectory.path());
    sourceMode->setCurrentIndex(1);
    fileLayout->setCurrentIndex(1);
    monitorEnabled->setChecked(false);
    noiseEnabled->setChecked(true);
    const QStringList stageObjectNames {
        QStringLiteral("recordingNoiseStage_suppression"),
        QStringLiteral("recordingNoiseStage_bandpass"),
        QStringLiteral("recordingNoiseStage_logmmse"),
        QStringLiteral("recordingNoiseStage_agc"),
    };
    QVector<QAbstractButton *> stageButtons;
    for (qsizetype index = 0; index < stageObjectNames.size(); ++index) {
        auto *button = window.findChild<QAbstractButton *>(
            stageObjectNames.at(index));
        QVERIFY(button != nullptr);
        button->setChecked(index == 0);
        stageButtons.append(button);
    }

    QGroupBox *connectionGroup = nullptr;
    for (QGroupBox *group : window.findChildren<QGroupBox *>()) {
        if (group->title() == QStringLiteral("设备连接")) {
            connectionGroup = group;
            break;
        }
    }
    QVERIFY(connectionGroup != nullptr);
    auto *connectionForm =
        qobject_cast<QFormLayout *>(connectionGroup->layout());
    QVERIFY(connectionForm != nullptr);
    auto *host = qobject_cast<QLineEdit *>(
        connectionForm->itemAt(1, QFormLayout::FieldRole)->widget());
    auto *port = qobject_cast<QSpinBox *>(
        connectionForm->itemAt(2, QFormLayout::FieldRole)->widget());
    auto *connectButton = qobject_cast<QPushButton *>(
        connectionForm->itemAt(4, QFormLayout::FieldRole)->widget());
    QVERIFY(host != nullptr);
    QVERIFY(port != nullptr);
    QVERIFY(connectButton != nullptr);
    host->setText(QStringLiteral("127.0.0.1"));
    port->setValue(server.serverPort());
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(quick->isEnabled(), 1000);

    const int sourceBefore = sourceMode->currentIndex();
    const int layoutBefore = fileLayout->currentIndex();
    const QString directoryBefore = directoryEdit->text();
    QVector<bool> stagesBefore;
    for (QAbstractButton *button : stageButtons) {
        stagesBefore.append(button->isChecked());
    }

    QTest::mouseClick(quick, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        stateLabel->text().contains(QStringLiteral("录制中")), 1000);
    QCOMPARE(quick->text(), QStringLiteral("停止录制"));
    QVERIFY(quick->isEnabled());
    QVERIFY(!start->isEnabled());
    QVERIFY(stop->isEnabled());
    QCOMPARE(sourceMode->currentIndex(), sourceBefore);
    QCOMPARE(fileLayout->currentIndex(), layoutBefore);
    QCOMPARE(directoryEdit->text(), directoryBefore);
    for (qsizetype index = 0; index < stageButtons.size(); ++index) {
        QCOMPARE(stageButtons.at(index)->isChecked(),
                 stagesBefore.at(index));
    }

    QSignalSpy typedFrames(
        noisePage, &NoiseReductionPage::noiseRecordingFrameReady);
    const QByteArray payload(
        AudioFrameDecoder::FramePayloadBytes, char(0));
    for (int frameIndex = 0; frameIndex < 6; ++frameIndex) {
        noisePage->submitRawAudioFrame(
            payload, 17, 250 + frameIndex * 4167);
    }
    QTRY_VERIFY_WITH_TIMEOUT(!typedFrames.isEmpty(), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(noisePage->pendingAudioFrames(), 0, 2000);

    QTest::qWait(250);
    for (int frameIndex = 6; frameIndex < 9; ++frameIndex) {
        noisePage->submitRawAudioFrame(
            payload, 17, 250 + frameIndex * 4167);
    }
    QTRY_COMPARE_WITH_TIMEOUT(noisePage->pendingAudioFrames(), 0, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !statistics->text().contains(QStringLiteral("已写帧: 0")),
        2000);
    QTest::mouseClick(quick, Qt::LeftButton);
    QCOMPARE(quick->text(), QStringLiteral("正在停止…"));
    QVERIFY(!quick->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(
        stateLabel->text().contains(QStringLiteral("空闲")), 1000);
    QVERIFY(!stop->isEnabled());
    QCOMPARE(quick->text(), QStringLiteral("开始录制"));
    QVERIFY(quick->isEnabled());
    QVERIFY(!statistics->text().contains(QStringLiteral("已写帧: 0")));
    QCOMPARE(sourceMode->currentIndex(), sourceBefore);
    QCOMPARE(fileLayout->currentIndex(), layoutBefore);
    QCOMPARE(directoryEdit->text(), directoryBefore);

    const QStringList manifests = QDir(outputDirectory.path()).entryList(
        {QStringLiteral("*.json")}, QDir::Files);
    QCOMPARE(manifests.size(), 1);
    QFile manifestFile(
        QDir(outputDirectory.path()).filePath(manifests.first()));
    QVERIFY(manifestFile.open(QIODevice::ReadOnly));
    const QJsonObject manifest =
        QJsonDocument::fromJson(manifestFile.readAll()).object();
    const QString baseName =
        manifest.value(QStringLiteral("baseName")).toString();
    QVERIFY(baseName.startsWith(QStringLiteral("MicArray_")));
    QCOMPARE(QFileInfo(manifestFile.fileName()).completeBaseName(),
             baseName);
    QCOMPARE(manifest.value(QStringLiteral("inputSource")).toString(),
             QStringLiteral("noiseReductionStages"));
    QCOMPARE(manifest.value(QStringLiteral("fileLayout")).toString(),
             QStringLiteral("separateTracks"));
    QVERIFY(manifest.value(QStringLiteral("inputFrames")).toInt() > 0);
    const QJsonArray tracks =
        manifest.value(QStringLiteral("tracks")).toArray();
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks.first().toObject()
                 .value(QStringLiteral("stage"))
                 .toString(),
             QStringLiteral("suppression"));
}

void DashboardTest::quickRecordingKeepsDetailedErrorAcrossRefreshes()
{
    QTemporaryDir outputDirectory;
    QVERIFY(outputDirectory.isValid());

    MainWindow window;
    window.show();
    auto *recordingPage = window.findChild<QWidget *>(
        QStringLiteral("recordingPage"));
    auto *sourceMode = window.findChild<QComboBox *>(
        QStringLiteral("recordingSourceModeCombo"));
    auto *fileLayout = window.findChild<QComboBox *>(
        QStringLiteral("recordingFileLayoutCombo"));
    auto *monitorBuffer = window.findChild<QSpinBox *>(
        QStringLiteral("noiseMonitorBufferMilliseconds"));
    auto *noiseStatus = window.findChild<QLabel *>(
        QStringLiteral("noiseReductionStatusLabel"));
    QVERIFY(recordingPage != nullptr);
    QVERIFY(sourceMode != nullptr);
    QVERIFY(fileLayout != nullptr);
    QVERIFY(monitorBuffer != nullptr);
    QVERIFY(noiseStatus != nullptr);

    QLineEdit *directoryEdit = nullptr;
    QLabel *stateLabel = nullptr;
    for (QLineEdit *edit : recordingPage->findChildren<QLineEdit *>()) {
        if (edit->isReadOnly()) {
            directoryEdit = edit;
            break;
        }
    }
    for (QLabel *label : recordingPage->findChildren<QLabel *>()) {
        if (label->text().startsWith(QStringLiteral("状态:"))) {
            stateLabel = label;
            break;
        }
    }
    QVERIFY(directoryEdit != nullptr);
    QVERIFY(stateLabel != nullptr);
    directoryEdit->setText(outputDirectory.path());

    const QString errorDetail = QStringLiteral("第二轨写入失败：磁盘已满");
    QVERIFY(QMetaObject::invokeMethod(
        &window,
        "updateRecordingState",
        Qt::DirectConnection,
        Q_ARG(MicArray::RecordingState, MicArray::RecordingState::Error)));
    QVERIFY(QMetaObject::invokeMethod(
        &window,
        "handleRecordingError",
        Qt::DirectConnection,
        Q_ARG(QString, errorDetail)));
    QVERIFY(noiseStatus->text().contains(errorDetail));

    sourceMode->setCurrentIndex(1);
    QCoreApplication::processEvents();
    QVERIFY(noiseStatus->text().contains(errorDetail));
    fileLayout->setCurrentIndex(1);
    QCoreApplication::processEvents();
    QVERIFY(noiseStatus->text().contains(errorDetail));
    monitorBuffer->setValue(monitorBuffer->value() == 250 ? 300 : 250);
    QCoreApplication::processEvents();
    QVERIFY(noiseStatus->text().contains(errorDetail));

    sourceMode->setCurrentIndex(0);
    fileLayout->setCurrentIndex(0);
    QVERIFY(QMetaObject::invokeMethod(&window, "startRecording"));
    QTRY_VERIFY_WITH_TIMEOUT(
        stateLabel->text().contains(QStringLiteral("录制中")), 3000);
    QVERIFY(!noiseStatus->text().contains(errorDetail));
    QVERIFY(QMetaObject::invokeMethod(&window, "stopRecording"));
    QTRY_VERIFY_WITH_TIMEOUT(
        stateLabel->text().contains(QStringLiteral("空闲")), 3000);
    QVERIFY(!noiseStatus->text().contains(errorDetail));
}

void DashboardTest::
noiseRecordingContinuesAcrossPageSwitchAndStopsOnTerminalState()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QTemporaryDir firstOutputDirectory;
    QTemporaryDir secondOutputDirectory;
    QVERIFY(firstOutputDirectory.isValid());
    QVERIFY(secondOutputDirectory.isValid());

    MainWindow window;
    window.show();
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabs"));
    auto *noisePage = window.findChild<NoiseReductionPage *>(
        QStringLiteral("noiseReductionPage"));
    auto *recordingPage = window.findChild<QWidget *>(
        QStringLiteral("recordingPage"));
    auto *noiseEnabled = window.findChild<QCheckBox *>(
        QStringLiteral("noiseReductionEnabledCheck"));
    auto *sourceMode = window.findChild<QComboBox *>(
        QStringLiteral("recordingSourceModeCombo"));
    auto *quick = window.findChild<QPushButton *>(
        QStringLiteral("noiseQuickRecordingButton"));
    auto *start = window.findChild<QPushButton *>(
        QStringLiteral("startRecordingButton"));
    auto *stop = window.findChild<QPushButton *>(
        QStringLiteral("stopRecordingButton"));
    QVERIFY(tabs != nullptr);
    QVERIFY(noisePage != nullptr);
    QVERIFY(recordingPage != nullptr);
    QVERIFY(noiseEnabled != nullptr);
    QVERIFY(sourceMode != nullptr);
    QVERIFY(quick != nullptr);
    QVERIFY(start != nullptr);
    QVERIFY(stop != nullptr);

    QLineEdit *directoryEdit = nullptr;
    QLabel *stateLabel = nullptr;
    for (QLineEdit *edit : recordingPage->findChildren<QLineEdit *>()) {
        if (edit->isReadOnly()) {
            directoryEdit = edit;
            break;
        }
    }
    for (QLabel *label : recordingPage->findChildren<QLabel *>()) {
        if (label->text().startsWith(QStringLiteral("状态:"))) {
            stateLabel = label;
            break;
        }
    }
    QVERIFY(directoryEdit != nullptr);
    QVERIFY(stateLabel != nullptr);
    directoryEdit->setText(firstOutputDirectory.path());
    sourceMode->setCurrentIndex(1);
    noiseEnabled->setChecked(true);
    const QStringList stageObjectNames {
        QStringLiteral("recordingNoiseStage_suppression"),
        QStringLiteral("recordingNoiseStage_bandpass"),
        QStringLiteral("recordingNoiseStage_logmmse"),
        QStringLiteral("recordingNoiseStage_agc"),
    };
    for (qsizetype index = 0; index < stageObjectNames.size(); ++index) {
        auto *button = window.findChild<QAbstractButton *>(
            stageObjectNames.at(index));
        QVERIFY(button != nullptr);
        button->setChecked(index == 0);
    }

    QGroupBox *connectionGroup = nullptr;
    for (QGroupBox *group : window.findChildren<QGroupBox *>()) {
        if (group->title() == QStringLiteral("设备连接")) {
            connectionGroup = group;
            break;
        }
    }
    QVERIFY(connectionGroup != nullptr);
    auto *connectionForm =
        qobject_cast<QFormLayout *>(connectionGroup->layout());
    QVERIFY(connectionForm != nullptr);
    auto *host = qobject_cast<QLineEdit *>(
        connectionForm->itemAt(1, QFormLayout::FieldRole)->widget());
    auto *port = qobject_cast<QSpinBox *>(
        connectionForm->itemAt(2, QFormLayout::FieldRole)->widget());
    auto *connectButton = qobject_cast<QPushButton *>(
        connectionForm->itemAt(4, QFormLayout::FieldRole)->widget());
    QVERIFY(host != nullptr);
    QVERIFY(port != nullptr);
    QVERIFY(connectButton != nullptr);
    host->setText(QStringLiteral("127.0.0.1"));
    port->setValue(server.serverPort());
    QTest::mouseClick(connectButton, Qt::LeftButton);

    tabs->setCurrentWidget(noisePage);
    QTRY_VERIFY_WITH_TIMEOUT(quick->isEnabled(), 1000);
    QTest::mouseClick(quick, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        stateLabel->text().contains(QStringLiteral("录制中")), 1000);
    tabs->setCurrentWidget(recordingPage);
    QCoreApplication::processEvents();

    HostFrame rawFrame;
    rawFrame.type = FrameType::Audio;
    rawFrame.payload = QByteArray(
        AudioFrameDecoder::FramePayloadBytes, char(0));
    rawFrame.timestampSeconds = 23;
    const quint64 submittedBefore = noisePage->submittedAudioFrames();
    for (int frameIndex = 0; frameIndex < 6; ++frameIndex) {
        rawFrame.timestampMicros = frameIndex * 4167;
        QVERIFY(QMetaObject::invokeMethod(
            &window,
            "enqueueAudioFrame",
            Qt::DirectConnection,
            Q_ARG(MicArray::HostFrame, rawFrame)));
    }
    QCOMPARE(noisePage->submittedAudioFrames(), submittedBefore + 6);
    QTRY_COMPARE_WITH_TIMEOUT(
        window.property("audioFramesInFlight").toInt(), 0, 1000);

    QTest::mouseClick(stop, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        stateLabel->text().contains(QStringLiteral("空闲")), 1000);
    const quint64 submittedAfterIdle =
        noisePage->submittedAudioFrames();
    QVERIFY(QMetaObject::invokeMethod(
        &window,
        "enqueueAudioFrame",
        Qt::DirectConnection,
        Q_ARG(MicArray::HostFrame, rawFrame)));
    QCOMPARE(noisePage->submittedAudioFrames(), submittedAfterIdle);
    QTRY_COMPARE_WITH_TIMEOUT(
        window.property("audioFramesInFlight").toInt(), 0, 1000);

    directoryEdit->setText(secondOutputDirectory.path());
    QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 1000);
    QTest::mouseClick(start, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        stateLabel->text().contains(QStringLiteral("录制中")), 1000);
    const quint64 submittedBeforeError =
        noisePage->submittedAudioFrames();
    QVERIFY(QMetaObject::invokeMethod(
        &window,
        "enqueueAudioFrame",
        Qt::DirectConnection,
        Q_ARG(MicArray::HostFrame, rawFrame)));
    QCOMPARE(noisePage->submittedAudioFrames(), submittedBeforeError + 1);
    QVERIFY(QMetaObject::invokeMethod(
        &window,
        "updateRecordingState",
        Qt::DirectConnection,
        Q_ARG(MicArray::RecordingState, MicArray::RecordingState::Error)));
    const quint64 submittedAfterError =
        noisePage->submittedAudioFrames();
    QVERIFY(QMetaObject::invokeMethod(
        &window,
        "enqueueAudioFrame",
        Qt::DirectConnection,
        Q_ARG(MicArray::HostFrame, rawFrame)));
    QCOMPARE(noisePage->submittedAudioFrames(), submittedAfterError);
    QVERIFY(QMetaObject::invokeMethod(&window, "stopRecording"));
    QTRY_VERIFY_WITH_TIMEOUT(
        stateLabel->text().contains(QStringLiteral("空闲")), 1000);
}

void DashboardTest::lightThemeRecordingChannelsUseDedicatedReadableStyle()
{
    QApplication::setPalette(paletteForUiTheme(UiTheme::Light));
    qApp->setStyleSheet(styleSheetForUiTheme(UiTheme::Light));

    MainWindow window;
    window.show();
    auto *unchecked = window.findChild<QToolButton *>(QStringLiteral("recordChannel_1"));
    auto *checked = window.findChild<QToolButton *>(QStringLiteral("recordChannel_0"));
    QVERIFY(unchecked != nullptr);
    QVERIFY(checked != nullptr);
    QVERIFY(unchecked->property("recordingChannelButton").toBool());
    QVERIFY(checked->property("recordingChannelButton").toBool());
    unchecked->setChecked(false);
    checked->setChecked(true);
    QVERIFY(!unchecked->isChecked());
    QVERIFY(checked->isChecked());
    QCoreApplication::processEvents();

    const QImage uncheckedImage = unchecked->grab().toImage();
    const QImage checkedImage = checked->grab().toImage();
    QVERIFY(contrastingPixelCount(uncheckedImage) > 8);
    QVERIFY(contrastingPixelCount(checkedImage) > 8);

    const QString themeStyle = styleSheetForUiTheme(UiTheme::Light);
    QVERIFY(themeStyle.contains(
        QStringLiteral("QToolButton[recordingChannelButton=\"true\"]")));
    QVERIFY(themeStyle.contains(
        QStringLiteral("QToolButton[recordingChannelButton=\"true\"]:checked")));
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
    QCoreApplication::processEvents();
    const QString actualClientSize = QStringLiteral("actual client size: %1x%2")
                                         .arg(window.width())
                                         .arg(window.height());
    QVERIFY2(window.width() <= 1366, qPrintable(actualClientSize));
    QVERIFY2(window.height() <= 768, qPrintable(actualClientSize));
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabs"));
    auto *page = window.findChild<QWidget *>(QStringLiteral("recordingPage"));
    QVERIFY(tabs != nullptr);
    QVERIFY(page != nullptr);
    auto *sourceMode = window.findChild<QComboBox *>(QStringLiteral("recordingSourceModeCombo"));
    auto *sourceStack = window.findChild<QStackedWidget *>(QStringLiteral("recordingSourceStack"));
    QVERIFY(sourceMode != nullptr);
    QVERIFY(sourceStack != nullptr);
    tabs->setCurrentWidget(page);
    auto *fileLayout = window.findChild<QComboBox *>(QStringLiteral("recordingFileLayoutCombo"));
    auto *start = window.findChild<QPushButton *>(QStringLiteral("startRecordingButton"));
    auto *stop = window.findChild<QPushButton *>(QStringLiteral("stopRecordingButton"));
    QLineEdit *directoryEdit = nullptr;
    QPushButton *browseButton = nullptr;
    for (QLineEdit *edit : page->findChildren<QLineEdit *>()) {
        if (edit->isReadOnly()) {
            directoryEdit = edit;
        }
    }
    for (QPushButton *button : page->findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("选择目录")) {
            browseButton = button;
        }
    }
    QVERIFY(fileLayout != nullptr);
    QVERIFY(start != nullptr);
    QVERIFY(stop != nullptr);
    QVERIFY(directoryEdit != nullptr);
    QVERIFY(browseButton != nullptr);

    const auto verifyVisibleLeaves = [page](const QList<QWidget *> &controls) {
        QList<QRect> rects;
        for (QWidget *control : controls) {
            QVERIFY(control != nullptr);
            QVERIFY(control->isVisible());
            const QRect rect(control->mapTo(page, QPoint(0, 0)), control->size());
            QVERIFY(page->rect().contains(rect));
            for (const QRect &existing : rects) {
                QVERIFY(!existing.intersects(rect));
            }
            rects.append(rect);
        }
    };

    for (int mode = 0; mode < 2; ++mode) {
        sourceMode->setCurrentIndex(mode);
        QCoreApplication::processEvents();
        QCOMPARE(sourceStack->currentIndex(), mode);
        QList<QWidget *> leaves {
            sourceMode,
            fileLayout,
            directoryEdit,
            browseButton,
            start,
            stop,
        };
        if (mode == 0) {
            for (int channel = 0; channel < AudioFrameDecoder::ChannelCount; ++channel) {
                auto *button = window.findChild<QToolButton *>(
                    QStringLiteral("recordChannel_%1").arg(channel));
                QVERIFY(button != nullptr);
                QVERIFY(button->width() >= 58);
                QVERIFY(button->height() >= 44);
                leaves.append(button);
            }
            for (const QString &objectName : {
                     QStringLiteral("recordSelectAllButton"),
                     QStringLiteral("recordClearButton"),
                     QStringLiteral("recordInvertButton"),
                     QStringLiteral("recordCurrentChannelButton")}) {
                leaves.append(window.findChild<QPushButton *>(objectName));
            }
        } else {
            for (const QString &objectName : {
                     QStringLiteral("recordingNoiseStage_suppression"),
                     QStringLiteral("recordingNoiseStage_bandpass"),
                     QStringLiteral("recordingNoiseStage_logmmse"),
                     QStringLiteral("recordingNoiseStage_agc")}) {
                leaves.append(window.findChild<QAbstractButton *>(objectName));
            }
        }
        verifyVisibleLeaves(leaves);
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
