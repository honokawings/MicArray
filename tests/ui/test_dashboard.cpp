#include "ui/arrayheatmapwidget.h"
#include "ui/displayformat.h"
#include "ui/trendchartwidget.h"
#include "ui/videopreviewwidget.h"
#include "ui/mainwindow.h"

#include <QSignalSpy>
#include <QToolButton>
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
    void clickingHeatmapCellSelectsChannel();
    void heatmapTracksPeakAndSelectedChannels();
    void videoDoubleClickRequestsFullScreen();
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
};

void DashboardTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("MicArrayTests"));
    QCoreApplication::setApplicationName(QStringLiteral("MicArrayDashboardTests"));
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

void DashboardTest::videoDoubleClickRequestsFullScreen()
{
    VideoPreviewWidget widget;
    widget.resize(320, 180);
    widget.show();
    QSignalSpy spy(&widget, &VideoPreviewWidget::fullScreenRequested);
    QTest::mouseDClick(&widget, Qt::LeftButton, Qt::NoModifier, widget.rect().center());
    QCOMPARE(spy.count(), 1);
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
    preset.values = {11, 12, 13, 14, 15, 16, 0, 0};
    QVERIFY(settings.saveControlPreset(preset));
    QCOMPARE(settings.controlPresets().size(), 1);

    MainWindow window;
    window.show();
    auto *combo = window.findChild<QComboBox *>(QStringLiteral("controlPresetCombo"));
    auto *apply = window.findChild<QPushButton *>(QStringLiteral("applyControlPresetButton"));
    auto *control0 = window.findChild<QSpinBox *>(QStringLiteral("controlSpin_0"));
    auto *send = window.findChild<QPushButton *>(QStringLiteral("sendControlButton"));
    QVERIFY(combo != nullptr);
    QVERIFY(apply != nullptr);
    QVERIFY(control0 != nullptr);
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

    auto *sampleRate = window.findChild<QDoubleSpinBox *>();
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

QTEST_MAIN(DashboardTest)
#include "test_dashboard.moc"
