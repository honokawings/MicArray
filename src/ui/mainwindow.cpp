#include "ui/mainwindow.h"

#include "ui/displayformat.h"

#include <QAbstractAnimation>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QEasingCurve>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPropertyAnimation>
#include <QSplitter>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTabBar>
#include <QTime>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtEndian>

#include <algorithm>

namespace MicArray {
namespace {

void appendU16(QByteArray &buffer, quint16 value)
{
    char bytes[2];
    qToLittleEndian(value, bytes);
    buffer.append(bytes, 2);
}

void appendU32(QByteArray &buffer, quint32 value)
{
    char bytes[4];
    qToLittleEndian(value, bytes);
    buffer.append(bytes, 4);
}

QString frameTypeName(quint16 rawType)
{
    switch (rawType) {
    case static_cast<quint16>(FrameType::Audio):
        return QStringLiteral("Audio 0xee11");
    case static_cast<quint16>(FrameType::Video):
        return QStringLiteral("Video 0xee22");
    case static_cast<quint16>(FrameType::Heartbeat):
        return QStringLiteral("Heartbeat 0xcc00");
    case static_cast<quint16>(FrameType::Control):
        return QStringLiteral("Control 0xcc11");
    case static_cast<quint16>(FrameType::PlPs):
        return QStringLiteral("PL/PS 0xeeff");
    }
    return QStringLiteral("Unknown 0x%1").arg(rawType, 4, 16, QLatin1Char('0'));
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<AudioSettings>("AudioSettings");
    qRegisterMetaType<DecodedAudioFrame>("MicArray::DecodedAudioFrame");

    m_audioProcessor = new AudioProcessor;
    m_videoDecoder = new VideoDecoder;
    m_audioProcessor->moveToThread(&m_audioThread);
    m_videoDecoder->moveToThread(&m_videoThread);
    connect(&m_audioThread, &QThread::finished, m_audioProcessor, &QObject::deleteLater);
    connect(&m_videoThread, &QThread::finished, m_videoDecoder, &QObject::deleteLater);
    m_audioThread.setObjectName(QStringLiteral("MicArrayAudioThread"));
    m_videoThread.setObjectName(QStringLiteral("MicArrayVideoThread"));
    m_audioThread.start();
    m_videoThread.start();

    buildUi();
    loadSettings();
    connectSignals();
    m_videoRenderTimer.setInterval(33);
    m_audioRenderTimer.setInterval(33);
    connect(&m_videoRenderTimer, &QTimer::timeout, this, &MainWindow::renderPendingVideoFrame);
    connect(&m_audioRenderTimer, &QTimer::timeout, this, &MainWindow::renderPendingAudioFrame);
    m_videoRenderTimer.start();
    m_audioRenderTimer.start();
    m_healthTimer.setInterval(1000);
    connect(&m_healthTimer, &QTimer::timeout, this, &MainWindow::updateHealthSnapshot);
    m_sessionTimer.start();
    m_sessionStartedAt = QDateTime::currentDateTime();
    m_healthTimer.start();
    refreshControlPresets();
    updateConnectionState(DeviceState::Disconnected);
    updateHealthSnapshot();
    updateStatistics();
    m_logger.info(QStringLiteral("app"), QStringLiteral("MicArray 上位机已启动"));
}

MainWindow::~MainWindow()
{
    m_audioRecorder.stopAndWait();
    m_healthTimer.stop();
    m_videoRenderTimer.stop();
    m_audioRenderTimer.stop();
    m_audioFrameQueue.clear();
    m_videoFrameQueue.clear();
    m_client.disconnectFromDevice();
    m_audioThread.quit();
    m_videoThread.quit();
    if (!m_audioThread.wait(3000)) {
        m_logger.error(QStringLiteral("lifecycle"), QStringLiteral("Audio thread did not stop within 3 seconds"));
    }
    if (!m_videoThread.wait(3000)) {
        m_logger.error(QStringLiteral("lifecycle"), QStringLiteral("Video thread did not stop within 3 seconds"));
    }
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("MicArray Operator Station"));
    resize(1366, 768);

    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    auto *statusBand = new QWidget(central);
    auto *statusLayout = new QHBoxLayout(statusBand);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    m_stateLabel = new QLabel(statusBand);
    m_endpointLabel = new QLabel(statusBand);
    m_heartbeatLabel = new QLabel(statusBand);
    m_rateLabel = new QLabel(statusBand);
    m_errorLabel = new QLabel(statusBand);
    m_healthLabel = new QLabel(QStringLiteral("健康: 未连接"), statusBand);
    m_healthLabel->setObjectName(QStringLiteral("healthStatusLabel"));
    m_healthLabel->setAlignment(Qt::AlignCenter);
    for (QLabel *label : {m_healthLabel, m_stateLabel, m_endpointLabel, m_heartbeatLabel, m_rateLabel, m_errorLabel}) {
        label->setMinimumWidth(150);
        statusLayout->addWidget(label);
    }
    statusLayout->addStretch();
    m_presentationButton = new QPushButton(QStringLiteral("演示全屏"), statusBand);
    m_presentationButton->setObjectName(QStringLiteral("presentationModeButton"));
    m_exportDiagnosticsButton = new QPushButton(QStringLiteral("导出诊断"), statusBand);
    statusLayout->addWidget(m_exportDiagnosticsButton);
    statusLayout->addWidget(m_presentationButton);
    root->addWidget(statusBand);

    m_mainTabs = new QTabWidget(central);
    m_mainTabs->setObjectName(QStringLiteral("mainTabs"));
    m_mainTabs->addTab(buildMonitorPage(), QStringLiteral("音视频融合"));
    m_mainTabs->addTab(buildRecordingPage(), QStringLiteral("音频录制"));
    m_mainTabs->addTab(buildDebugPage(), QStringLiteral("高级帧调试"));
    m_mainTabs->addTab(buildCalibrationPage(), QStringLiteral("阵列校准"));
    root->addWidget(m_mainTabs, 1);

    setCentralWidget(central);
}

QWidget *MainWindow::buildMonitorPage()
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("monitorPage"));
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, page);
    mainSplitter->setChildrenCollapsible(false);

    auto *left = new QWidget(mainSplitter);
    auto *leftLayout = new QVBoxLayout(left);

    auto *connectionGroup = new QGroupBox(QStringLiteral("设备连接"), left);
    auto *connectionLayout = new QFormLayout(connectionGroup);
    m_presetCombo = new QComboBox(connectionGroup);
    m_presetCombo->addItem(QStringLiteral("有线 192.168.2.10"), QStringLiteral("192.168.2.10"));
    m_presetCombo->addItem(QStringLiteral("无线 192.168.5.1"), QStringLiteral("192.168.5.1"));
    m_presetCombo->addItem(QStringLiteral("自定义"), QString());
    m_hostEdit = new QLineEdit(connectionGroup);
    m_portSpin = new QSpinBox(connectionGroup);
    m_portSpin->setRange(1, 65535);
    m_autoReconnectCheck = new QCheckBox(QStringLiteral("自动重连"), connectionGroup);
    m_connectButton = new QPushButton(QStringLiteral("连接"), connectionGroup);
    connectionLayout->addRow(QStringLiteral("预设"), m_presetCombo);
    connectionLayout->addRow(QStringLiteral("IP"), m_hostEdit);
    connectionLayout->addRow(QStringLiteral("端口"), m_portSpin);
    connectionLayout->addRow(QString(), m_autoReconnectCheck);
    connectionLayout->addRow(QString(), m_connectButton);
    leftLayout->addWidget(connectionGroup);

    auto *controlGroup = new QGroupBox(QStringLiteral("控制接口"), left);
    auto *controlLayout = new QFormLayout(controlGroup);
    for (int i = 0; i < ControlValues::ChannelCount; ++i) {
        m_controlSpins[i] = new QSpinBox(controlGroup);
        m_controlSpins[i]->setObjectName(QStringLiteral("controlSpin_%1").arg(i));
        m_controlSpins[i]->setRange(-32768, 32767);
        m_controlSpins[i]->setSuffix(QStringLiteral(" int16"));
        if (m_controlValues.isReserved(i)) {
            m_controlSpins[i]->setEnabled(false);
        }
        controlLayout->addRow(QStringLiteral("接口 %1%2").arg(i).arg(m_controlValues.isReserved(i) ? QStringLiteral(" 预留") : QString()), m_controlSpins[i]);
    }
    m_lockControlCheck = new QCheckBox(QStringLiteral("锁定控制"), controlGroup);
    m_sendControlButton = new QPushButton(QStringLiteral("发送控制"), controlGroup);
    m_sendControlButton->setObjectName(QStringLiteral("sendControlButton"));
    m_resetControlButton = new QPushButton(QStringLiteral("恢复默认"), controlGroup);
    m_undoControlButton = new QPushButton(QStringLiteral("撤销到上次发送"), controlGroup);
    m_undoControlButton->setEnabled(false);
    m_controlPresetCombo = new QComboBox(controlGroup);
    m_controlPresetCombo->setObjectName(QStringLiteral("controlPresetCombo"));
    m_applyPresetButton = new QPushButton(QStringLiteral("应用预设"), controlGroup);
    m_applyPresetButton->setObjectName(QStringLiteral("applyControlPresetButton"));
    m_savePresetButton = new QPushButton(QStringLiteral("另存预设"), controlGroup);
    m_deletePresetButton = new QPushButton(QStringLiteral("删除预设"), controlGroup);
    auto *presetButtons = new QWidget(controlGroup);
    auto *presetButtonLayout = new QHBoxLayout(presetButtons);
    presetButtonLayout->setContentsMargins(0, 0, 0, 0);
    presetButtonLayout->addWidget(m_applyPresetButton);
    presetButtonLayout->addWidget(m_savePresetButton);
    presetButtonLayout->addWidget(m_deletePresetButton);
    controlLayout->addRow(QString(), m_lockControlCheck);
    controlLayout->addRow(QStringLiteral("本地预设"), m_controlPresetCombo);
    controlLayout->addRow(QString(), presetButtons);
    controlLayout->addRow(m_sendControlButton, m_resetControlButton);
    controlLayout->addRow(QString(), m_undoControlButton);
    leftLayout->addWidget(controlGroup);

    auto *settingsGroup = new QGroupBox(QStringLiteral("音视频设置"), left);
    auto *settingsLayout = new QFormLayout(settingsGroup);
    m_sampleRateSpin = new QDoubleSpinBox(settingsGroup);
    m_sampleRateSpin->setRange(8.0, 192.0);
    m_sampleRateSpin->setDecimals(0);
    m_sampleRateSpin->setSingleStep(1.0);
    m_sampleRateSpin->setSuffix(QStringLiteral(" kHz"));
    m_channelCountSpin = new QSpinBox(settingsGroup);
    m_channelCountSpin->setRange(1, AudioFrameDecoder::ChannelCount);
    m_outputChannelSpin = new QSpinBox(settingsGroup);
    m_outputChannelSpin->setObjectName(QStringLiteral("outputChannelSpin"));
    m_outputChannelSpin->setRange(0, AudioFrameDecoder::ChannelCount - 1);
    m_averageWindowSpin = new QSpinBox(settingsGroup);
    m_averageWindowSpin->setRange(1, 10000);
    m_averageWindowSpin->setSuffix(QStringLiteral(" 帧"));
    m_audioPlaybackCheck = new QCheckBox(QStringLiteral("启用选中通道播放"), settingsGroup);
    m_videoFormatCombo = new QComboBox(settingsGroup);
    m_videoFormatCombo->addItem(QStringLiteral("JPEG"), QStringLiteral("JPG"));
    m_resetSettingsButton = new QPushButton(QStringLiteral("恢复全部默认设置"), settingsGroup);
    m_resetSettingsButton->setObjectName(QStringLiteral("resetSettingsButton"));
    settingsLayout->addRow(QStringLiteral("采样率"), m_sampleRateSpin);
    settingsLayout->addRow(QStringLiteral("阵列通道数"), m_channelCountSpin);
    settingsLayout->addRow(QStringLiteral("播放通道"), m_outputChannelSpin);
    settingsLayout->addRow(QStringLiteral("平均窗口"), m_averageWindowSpin);
    settingsLayout->addRow(QString(), m_audioPlaybackCheck);
    settingsLayout->addRow(QStringLiteral("视频格式"), m_videoFormatCombo);
    settingsLayout->addRow(QString(), m_resetSettingsButton);
    leftLayout->addWidget(settingsGroup);
    leftLayout->addStretch();
    mainSplitter->addWidget(left);

    auto *center = new QWidget(mainSplitter);
    auto *centerLayout = new QVBoxLayout(center);
    m_videoLabel = new VideoPreviewWidget(center);
    m_waveformWidget = new AudioWaveformWidget(center);
    m_videoStatsLabel = new QLabel(center);
    m_audioLevel = new QProgressBar(center);
    m_audioLevel->setRange(0, 100);
    m_audioStatsLabel = new QLabel(center);
    centerLayout->addWidget(m_videoLabel, 1);
    centerLayout->addWidget(m_waveformWidget);
    centerLayout->addWidget(m_videoStatsLabel);
    centerLayout->addWidget(m_audioLevel);
    centerLayout->addWidget(m_audioStatsLabel);
    auto *dashboardDetails = new QTabWidget(center);
    m_dashboardHeatmap = new ArrayHeatmapWidget(dashboardDetails);
    m_trendChart = new TrendChartWidget(dashboardDetails);
    dashboardDetails->addTab(m_dashboardHeatmap, QStringLiteral("阵列幅值"));
    dashboardDetails->addTab(m_trendChart, QStringLiteral("60 秒趋势"));
    centerLayout->addWidget(dashboardDetails);
    mainSplitter->addWidget(center);

    auto *right = new QWidget(mainSplitter);
    auto *rightLayout = new QVBoxLayout(right);
    auto *diagnosticGroup = new QGroupBox(QStringLiteral("诊断"), right);
    auto *diagnosticLayout = new QVBoxLayout(diagnosticGroup);
    m_protocolStatsLabel = new QLabel(diagnosticGroup);
    m_protocolStatsLabel->setWordWrap(true);
    diagnosticLayout->addWidget(m_protocolStatsLabel);
    rightLayout->addWidget(diagnosticGroup);

    auto *alertGroup = new QGroupBox(QStringLiteral("告警时间线"), right);
    auto *alertLayout = new QVBoxLayout(alertGroup);
    m_alertTimeline = new QPlainTextEdit(alertGroup);
    m_alertTimeline->setReadOnly(true);
    m_alertTimeline->setMaximumBlockCount(200);
    alertLayout->addWidget(m_alertTimeline);
    rightLayout->addWidget(alertGroup, 1);

    auto *logGroup = new QGroupBox(QStringLiteral("日志"), right);
    auto *logLayout = new QVBoxLayout(logGroup);
    auto *logButtonLayout = new QHBoxLayout();
    auto *clearLogButton = new QPushButton(QStringLiteral("清空"), logGroup);
    auto *copyLogButton = new QPushButton(QStringLiteral("复制"), logGroup);
    logButtonLayout->addWidget(clearLogButton);
    logButtonLayout->addWidget(copyLogButton);
    logButtonLayout->addStretch();
    m_logView = new QPlainTextEdit(logGroup);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(1000);
    logLayout->addLayout(logButtonLayout);
    logLayout->addWidget(m_logView);
    rightLayout->addWidget(logGroup, 1);
    mainSplitter->addWidget(right);

    mainSplitter->setSizes({320, 730, 316});
    pageLayout->addWidget(mainSplitter, 1);

    connect(clearLogButton, &QPushButton::clicked, m_logView, &QPlainTextEdit::clear);
    connect(copyLogButton, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_logView->toPlainText());
    });

    return page;
}

QWidget *MainWindow::buildRecordingPage()
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("recordingPage"));
    auto *layout = new QHBoxLayout(page);

    auto *channelGroup = new QGroupBox(QStringLiteral("录制通道"), page);
    auto *channelLayout = new QVBoxLayout(channelGroup);
    auto *channelGrid = new QGridLayout();
    channelGrid->setSpacing(6);
    m_recordingChannelButtons.clear();
    for (int row = 0; row < AudioFrameDecoder::Rows; ++row) {
        for (int column = 0; column < AudioFrameDecoder::Columns; ++column) {
            const int channel = row * AudioFrameDecoder::Columns + column;
            auto *button = new QToolButton(channelGroup);
            button->setObjectName(QStringLiteral("recordChannel_%1").arg(channel));
            button->setText(QStringLiteral("CH%1").arg(channel));
            button->setCheckable(true);
            button->setChecked(channel == 0);
            button->setMinimumSize(58, 44);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            channelGrid->addWidget(button, row, column);
            m_recordingChannelButtons.append(button);
            connect(button, &QToolButton::toggled, this, [this]() {
                updateRecordingStartEnabled();
            });
        }
    }
    channelLayout->addLayout(channelGrid);

    m_recordingSelectionCommands = new QWidget(channelGroup);
    auto *selectionCommands = new QHBoxLayout(m_recordingSelectionCommands);
    selectionCommands->setContentsMargins(0, 0, 0, 0);
    auto *selectAll = new QPushButton(QStringLiteral("全选"), channelGroup);
    auto *clear = new QPushButton(QStringLiteral("清空"), channelGroup);
    auto *invert = new QPushButton(QStringLiteral("反选"), channelGroup);
    auto *selectCurrent = new QPushButton(QStringLiteral("当前播放通道"), channelGroup);
    selectAll->setObjectName(QStringLiteral("recordSelectAllButton"));
    clear->setObjectName(QStringLiteral("recordClearButton"));
    invert->setObjectName(QStringLiteral("recordInvertButton"));
    selectCurrent->setObjectName(QStringLiteral("recordCurrentChannelButton"));
    selectionCommands->addWidget(selectAll);
    selectionCommands->addWidget(clear);
    selectionCommands->addWidget(invert);
    selectionCommands->addWidget(selectCurrent);
    channelLayout->addWidget(m_recordingSelectionCommands);
    channelLayout->addStretch();
    layout->addWidget(channelGroup, 3);

    auto *details = new QWidget(page);
    auto *detailsLayout = new QVBoxLayout(details);
    auto *storageGroup = new QGroupBox(QStringLiteral("文件与控制"), details);
    auto *storageLayout = new QFormLayout(storageGroup);
    m_recordingDirectoryEdit = new QLineEdit(storageGroup);
    m_recordingDirectoryEdit->setReadOnly(true);
    QString defaultDirectory = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (defaultDirectory.isEmpty() || !QDir(defaultDirectory).exists()) {
        defaultDirectory = QDir::homePath();
    }
    m_recordingDirectoryEdit->setText(defaultDirectory);
    m_recordingBrowseButton = new QPushButton(QStringLiteral("选择目录"), storageGroup);
    auto *directoryRow = new QWidget(storageGroup);
    auto *directoryLayout = new QHBoxLayout(directoryRow);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    directoryLayout->addWidget(m_recordingDirectoryEdit, 1);
    directoryLayout->addWidget(m_recordingBrowseButton);
    m_recordingStartButton = new QPushButton(QStringLiteral("开始录制"), storageGroup);
    m_recordingStartButton->setObjectName(QStringLiteral("startRecordingButton"));
    m_recordingStopButton = new QPushButton(QStringLiteral("停止录制"), storageGroup);
    m_recordingStopButton->setObjectName(QStringLiteral("stopRecordingButton"));
    m_recordingStopButton->setEnabled(false);
    auto *recordingCommands = new QWidget(storageGroup);
    auto *recordingCommandLayout = new QHBoxLayout(recordingCommands);
    recordingCommandLayout->setContentsMargins(0, 0, 0, 0);
    recordingCommandLayout->addWidget(m_recordingStartButton);
    recordingCommandLayout->addWidget(m_recordingStopButton);
    storageLayout->addRow(QStringLiteral("保存目录"), directoryRow);
    storageLayout->addRow(QStringLiteral("录制格式"), new QLabel(QStringLiteral("多通道 WAV · 32-bit 容器 / 24-bit 有效位"), storageGroup));
    storageLayout->addRow(QString(), recordingCommands);
    detailsLayout->addWidget(storageGroup);

    auto *statusGroup = new QGroupBox(QStringLiteral("录制状态"), details);
    auto *statusLayout = new QVBoxLayout(statusGroup);
    m_recordingStateLabel = new QLabel(QStringLiteral("状态: 空闲"), statusGroup);
    m_recordingStatisticsLabel = new QLabel(QStringLiteral("时长: 00:00:00\n已写: 0 B\n写入速率: 0 B/s\n待写队列: 0 B\n已写帧: 0\n未写帧: 0\n清单: -"), statusGroup);
    m_recordingStatisticsLabel->setObjectName(QStringLiteral("recordingStatisticsLabel"));
    m_recordingStatisticsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_recordingErrorLabel = new QLabel(statusGroup);
    m_recordingErrorLabel->setWordWrap(true);
    m_recordingErrorLabel->setStyleSheet(QStringLiteral("QLabel { color:#c74343; }"));
    statusLayout->addWidget(m_recordingStateLabel);
    statusLayout->addWidget(m_recordingStatisticsLabel);
    statusLayout->addWidget(m_recordingErrorLabel);
    detailsLayout->addWidget(statusGroup);
    detailsLayout->addStretch();
    layout->addWidget(details, 2);

    connect(selectAll, &QPushButton::clicked, this, [this]() {
        for (QToolButton *button : m_recordingChannelButtons) {
            button->setChecked(true);
        }
    });
    connect(clear, &QPushButton::clicked, this, [this]() {
        for (QToolButton *button : m_recordingChannelButtons) {
            button->setChecked(false);
        }
    });
    connect(invert, &QPushButton::clicked, this, [this]() {
        for (QToolButton *button : m_recordingChannelButtons) {
            button->setChecked(!button->isChecked());
        }
    });
    connect(selectCurrent, &QPushButton::clicked, this, [this]() {
        const int currentChannel = m_outputChannelSpin->value();
        for (int channel = 0; channel < m_recordingChannelButtons.size(); ++channel) {
            m_recordingChannelButtons.at(channel)->setChecked(channel == currentChannel);
        }
    });

    return page;
}

QWidget *MainWindow::buildDebugPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *settingsGroup = new QGroupBox(QStringLiteral("调试设置"), page);
    auto *settingsLayout = new QFormLayout(settingsGroup);
    m_debugHistorySpin = new QSpinBox(settingsGroup);
    m_debugHistorySpin->setRange(1, 200);
    m_debugHistorySpin->setValue(20);
    m_debugHistorySpin->setSuffix(QStringLiteral(" 帧"));
    settingsLayout->addRow(QStringLiteral("历史保留"), m_debugHistorySpin);
    layout->addWidget(settingsGroup);

    auto *debugGroup = new QGroupBox(QStringLiteral("最近解析帧"), page);
    auto *form = new QFormLayout(debugGroup);
    m_debugMetaLabel = new QLabel(QStringLiteral("-"), debugGroup);
    m_debugHeadLabel = new QLabel(QStringLiteral("-"), debugGroup);
    m_debugTailLabel = new QLabel(QStringLiteral("-"), debugGroup);
    m_debugPostTailLabel = new QLabel(QStringLiteral("-"), debugGroup);
    m_debugPostTailLabel->setObjectName(QStringLiteral("debugPostTailLabel"));
    m_debugMetaLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_debugHeadLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_debugTailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_debugPostTailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    const QString hexPreviewStyle = QStringLiteral("QLabel { font-family: Consolas, 'Courier New', monospace; }");
    m_debugHeadLabel->setStyleSheet(hexPreviewStyle);
    m_debugTailLabel->setStyleSheet(hexPreviewStyle);
    m_debugPostTailLabel->setStyleSheet(hexPreviewStyle);
    form->addRow(QStringLiteral("元信息"), m_debugMetaLabel);
    form->addRow(QStringLiteral("前 16 字节"), m_debugHeadLabel);
    form->addRow(QStringLiteral("尾 16 字节"), m_debugTailLabel);
    form->addRow(QStringLiteral("帧后 16 字节"), m_debugPostTailLabel);
    layout->addWidget(debugGroup);

    auto *segmentGroup = new QGroupBox(QStringLiteral("帧结构"), page);
    auto *segmentLayout = new QHBoxLayout(segmentGroup);
    const QStringList segmentNames = {
        QStringLiteral("帧头\n2B"),
        QStringLiteral("类型\n2B"),
        QStringLiteral("长度\n4B"),
        QStringLiteral("秒时间戳\n4B"),
        QStringLiteral("微秒时间戳\n4B"),
        QStringLiteral("Payload\n-"),
        QStringLiteral("帧尾\n2B"),
    };
    m_debugSegmentLabels.clear();
    for (int i = 0; i < segmentNames.size(); ++i) {
        auto *segment = new QLabel(segmentNames.at(i), segmentGroup);
        segment->setAlignment(Qt::AlignCenter);
        segment->setMinimumHeight(58);
        segment->setFrameShape(QFrame::Box);
        segment->setStyleSheet(QStringLiteral("QLabel { background:#20242a; color:#e6edf3; border:1px solid #4d5966; font-weight:600; }"));
        segmentLayout->addWidget(segment, i == 5 ? 3 : 1);
        m_debugSegmentLabels.append(segment);
    }
    layout->addWidget(segmentGroup);

    auto *historyGroup = new QGroupBox(QStringLiteral("解析帧历史"), page);
    auto *historyLayout = new QVBoxLayout(historyGroup);
    m_debugHistoryView = new QPlainTextEdit(historyGroup);
    m_debugHistoryView->setReadOnly(true);
    m_debugHistoryView->setMaximumBlockCount(m_debugHistorySpin->value());
    m_debugHistoryView->setStyleSheet(QStringLiteral("QPlainTextEdit { font-family: Consolas, 'Courier New', monospace; }"));
    historyLayout->addWidget(m_debugHistoryView);
    layout->addWidget(historyGroup, 1);

    auto *errorGroup = new QGroupBox(QStringLiteral("解析异常诊断"), page);
    auto *errorLayout = new QVBoxLayout(errorGroup);
    m_debugErrorView = new QPlainTextEdit(errorGroup);
    m_debugErrorView->setReadOnly(true);
    m_debugErrorView->setMaximumBlockCount(80);
    m_debugErrorView->setStyleSheet(QStringLiteral("QPlainTextEdit { font-family: Consolas, 'Courier New', monospace; }"));
    errorLayout->addWidget(m_debugErrorView);
    layout->addWidget(errorGroup, 1);

    connect(m_debugHistorySpin, &QSpinBox::valueChanged, m_debugHistoryView, &QPlainTextEdit::setMaximumBlockCount);

    return page;
}

QWidget *MainWindow::buildCalibrationPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    m_calibrationStatsLabel = new QLabel(QStringLiteral("等待 48 通道音频帧"), page);
    layout->addWidget(m_calibrationStatsLabel);

    auto *grid = new QGridLayout();
    grid->setSpacing(6);
    m_arrayCells.clear();
    for (int row = 0; row < AudioFrameDecoder::Rows; ++row) {
        for (int column = 0; column < AudioFrameDecoder::Columns; ++column) {
            const int channel = row * AudioFrameDecoder::Columns + column;
            auto *cell = new QLabel(QStringLiteral("%1\n-").arg(channel), page);
            cell->setAlignment(Qt::AlignCenter);
            cell->setMinimumSize(80, 56);
            cell->setFrameShape(QFrame::Box);
            cell->setStyleSheet(QStringLiteral("QLabel { background:#202020; color:#eeeeee; border:1px solid #555; }"));
            grid->addWidget(cell, row, column);
            m_arrayCells.append(cell);
        }
    }
    layout->addLayout(grid);
    layout->addStretch();
    return page;
}

void MainWindow::connectSignals()
{
    connect(m_presetCombo, &QComboBox::currentIndexChanged, this, &MainWindow::applyPreset);
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::connectOrDisconnect);
    connect(m_sendControlButton, &QPushButton::clicked, this, &MainWindow::sendControl);
    connect(m_resetControlButton, &QPushButton::clicked, this, &MainWindow::resetControl);
    connect(m_undoControlButton, &QPushButton::clicked, this, &MainWindow::restoreLastSentControl);
    connect(m_applyPresetButton, &QPushButton::clicked, this, &MainWindow::applyControlPreset);
    connect(m_savePresetButton, &QPushButton::clicked, this, &MainWindow::saveControlPreset);
    connect(m_deletePresetButton, &QPushButton::clicked, this, &MainWindow::deleteControlPreset);
    connect(m_resetSettingsButton, &QPushButton::clicked, this, &MainWindow::resetSettingsToDefaults);
    connect(m_presentationButton, &QPushButton::clicked, this, &MainWindow::togglePresentationMode);
    connect(m_exportDiagnosticsButton, &QPushButton::clicked, this, &MainWindow::exportDiagnostics);
    connect(m_recordingBrowseButton, &QPushButton::clicked, this, &MainWindow::chooseRecordingDirectory);
    connect(m_recordingStartButton, &QPushButton::clicked, this, &MainWindow::startRecording);
    connect(m_recordingStopButton, &QPushButton::clicked, this, &MainWindow::stopRecording);
    connect(m_videoLabel, &VideoPreviewWidget::fullScreenRequested, this, &MainWindow::togglePresentationMode);
    connect(m_dashboardHeatmap, &ArrayHeatmapWidget::channelSelected, this, [this](int channel) {
        m_outputChannelSpin->setValue(channel);
        appendAlert(LogLevel::Info, QStringLiteral("监听通道已切换为 CH%1").arg(channel));
    });
    connect(m_lockControlCheck, &QCheckBox::toggled, this, [this](bool locked) {
        setControlsEnabled(!locked && m_client.state() == DeviceState::Connected);
    });

    for (QSpinBox *spin : m_controlSpins) {
        connect(spin, &QSpinBox::valueChanged, this, &MainWindow::markControlDirty);
    }

    connect(m_sampleRateSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateAudioSettingsFromUi);
    connect(m_channelCountSpin, &QSpinBox::valueChanged, this, &MainWindow::updateAudioSettingsFromUi);
    connect(m_outputChannelSpin, &QSpinBox::valueChanged, this, &MainWindow::updateAudioSettingsFromUi);
    connect(m_averageWindowSpin, &QSpinBox::valueChanged, this, &MainWindow::updateAudioSettingsFromUi);
    connect(m_audioPlaybackCheck, &QCheckBox::toggled, this, &MainWindow::updateAudioSettingsFromUi);

    connect(&m_client, &DeviceClient::stateChanged, this, &MainWindow::updateConnectionState);
    connect(&m_client, &DeviceClient::statisticsChanged, this, &MainWindow::updateStatistics);
    connect(&m_client, &DeviceClient::logMessage, &m_logger, &Logger::info);
    connect(&m_client, &DeviceClient::warningMessage, &m_logger, &Logger::warning);
    connect(&m_client, &DeviceClient::errorMessage, &m_logger, &Logger::error);
    connect(&m_client, &DeviceClient::parseErrorReceived, this, &MainWindow::updateDebugError);
    connect(&m_client, &DeviceClient::frameReceived, this, &MainWindow::updateDebugFrame);
    connect(&m_client, &DeviceClient::audioFrameReceived, this, [this](const HostFrame &frame) {
        if (m_audioRecorder.state() == RecordingState::Recording) {
            m_audioRecorder.enqueueFrame(frame.payload);
        }
        if (m_audioFrameQueue.push(frame)) {
            ++m_audioDroppedFrames;
        }
        dispatchNextAudioFrame();
    });
    connect(&m_client, &DeviceClient::videoFrameReceived, this, [this](const HostFrame &frame) {
        if (m_videoFrameQueue.push(frame)) {
            ++m_videoDroppedFrames;
        }
        dispatchNextVideoFrame();
    });

    connect(m_videoDecoder, &VideoDecoder::frameReady, this, &MainWindow::showVideoFrame, Qt::QueuedConnection);
    connect(m_videoDecoder, &VideoDecoder::decodeError, &m_logger, [this](const QString &message) {
        m_logger.warning(QStringLiteral("video"), message);
    }, Qt::QueuedConnection);
    connect(m_audioProcessor, &AudioProcessor::levelChanged, this, &MainWindow::updateAudioLevel, Qt::QueuedConnection);
    connect(m_audioProcessor, &AudioProcessor::arrayFrameDecoded, this, [this](const DecodedAudioFrame &frame) {
        showArrayFrame(frame);
    }, Qt::QueuedConnection);
    connect(m_audioProcessor, &AudioProcessor::audioWarning, &m_logger, [this](const QString &message) {
        m_logger.warning(QStringLiteral("audio"), message);
    }, Qt::QueuedConnection);
    connect(m_audioProcessor, &AudioProcessor::frameProcessed, this, [this](bool valid) {
        if (!valid) {
            ++m_audioDroppedFrames;
        }
        m_audioProcessing = false;
        dispatchNextAudioFrame();
    }, Qt::QueuedConnection);
    connect(m_videoDecoder, &VideoDecoder::frameProcessed, this, [this](bool valid) {
        if (!valid) {
            ++m_videoDroppedFrames;
        }
        m_videoProcessing = false;
        dispatchNextVideoFrame();
    }, Qt::QueuedConnection);
    connect(&m_logger, &Logger::messageLogged, this, &MainWindow::appendLog);
    connect(&m_logger, &Logger::messageLogged, this, [this](LogLevel level, const QString &, const QString &message, const QString &) {
        if (level != LogLevel::Info) {
            appendAlert(level, message);
        }
    });
    connect(&m_audioRecorder, &AudioRecorder::stateChanged, this, &MainWindow::updateRecordingState);
    connect(&m_audioRecorder, &AudioRecorder::statisticsChanged, this, &MainWindow::updateRecordingStatistics);
    connect(&m_audioRecorder, &AudioRecorder::recordingError, this, [this](const QString &message) {
        m_recordingErrorLabel->setText(QStringLiteral("错误: %1").arg(message));
        m_logger.error(QStringLiteral("recording"), message);
    });
}

void MainWindow::loadSettings()
{
    const ConnectionSettings connection = m_settings.connection();
    m_hostEdit->setText(connection.host);
    m_portSpin->setValue(connection.port);
    m_autoReconnectCheck->setChecked(connection.autoReconnect);

    const AudioSettings audio = m_settings.audio();
    m_sampleRateSpin->setValue(static_cast<double>(audio.sampleRate) / 1000.0);
    m_channelCountSpin->setValue(audio.channelCount);
    m_outputChannelSpin->setValue(audio.outputChannel);
    m_averageWindowSpin->setValue(audio.averageWindowFrames);
    m_audioPlaybackCheck->setChecked(audio.playbackEnabled);
    m_audioOutputChannel = audio.outputChannel;
    m_audioAverageWindowFrames = audio.averageWindowFrames;
    for (int channel = 0; channel < m_recordingChannelButtons.size(); ++channel) {
        m_recordingChannelButtons.at(channel)->setChecked(channel == audio.outputChannel);
    }
    QMetaObject::invokeMethod(m_audioProcessor,
                              "configure",
                              Qt::QueuedConnection,
                              Q_ARG(AudioSettings, audio));

    const VideoSettings video = m_settings.video();
    const int index = m_videoFormatCombo->findData(video.format);
    if (index >= 0) {
        m_videoFormatCombo->setCurrentIndex(index);
    }
}

void MainWindow::saveSettings()
{
    m_settings.saveConnection({m_hostEdit->text(), static_cast<quint16>(m_portSpin->value()), m_autoReconnectCheck->isChecked()});
    m_settings.saveAudio({static_cast<int>(m_sampleRateSpin->value() * 1000.0),
                          m_channelCountSpin->value(),
                          m_outputChannelSpin->value(),
                          m_averageWindowSpin->value(),
                          m_audioPlaybackCheck->isChecked()});
    m_settings.saveVideo({m_videoFormatCombo->currentData().toString()});
}

void MainWindow::applyPreset(int index)
{
    const QString host = m_presetCombo->itemData(index).toString();
    if (!host.isEmpty()) {
        m_hostEdit->setText(host);
        m_portSpin->setValue(9968);
    }
}

void MainWindow::connectOrDisconnect()
{
    if (m_client.state() == DeviceState::Connected || m_client.state() == DeviceState::Connecting || m_client.state() == DeviceState::Reconnecting) {
        m_client.disconnectFromDevice();
        return;
    }

    saveSettings();
    updateAudioSettingsFromUi();
    m_client.setAutoReconnect(m_autoReconnectCheck->isChecked());
    m_connectionTimer.restart();
    m_client.connectToDevice(m_hostEdit->text(), static_cast<quint16>(m_portSpin->value()));
}

void MainWindow::sendControl()
{
    const QVector<qint16> values = currentControlVector();
    const bool highRisk = std::any_of(values.cbegin(), values.cend(), [](qint16 value) {
        return std::abs(static_cast<int>(value)) >= 30000;
    });
    if (highRisk && QMessageBox::warning(this,
                                         QStringLiteral("确认高风险控制"),
                                         QStringLiteral("至少一个控制值接近 int16 极限。确认发送？"),
                                         QMessageBox::Yes | QMessageBox::No,
                                         QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    if (m_client.sendControl(collectControlValues())) {
        m_lastSentControlValues = values;
        m_hasLastSentControl = true;
        m_undoControlButton->setEnabled(true);
        m_controlDirty = false;
        m_sendControlButton->setText(QStringLiteral("已发送（设备未确认）"));
        appendAlert(LogLevel::Info, QStringLiteral("控制帧已发送，设备协议不提供执行确认"));
    }
}

void MainWindow::resetControl()
{
    for (int i = 0; i < ControlValues::ChannelCount; ++i) {
        m_controlSpins[i]->setValue(0);
    }
    markControlDirty();
}

void MainWindow::markControlDirty()
{
    m_controlDirty = true;
    m_sendControlButton->setText(QStringLiteral("发送待确认变更 *"));
}

void MainWindow::updateConnectionState(DeviceState state)
{
    m_stateLabel->setText(QStringLiteral("状态: %1").arg(deviceStateName(state)));
    m_connectButton->setText(state == DeviceState::Connected || state == DeviceState::Connecting || state == DeviceState::Reconnecting
                                 ? QStringLiteral("断开")
                                 : QStringLiteral("连接"));
    if (state != DeviceState::Connected
        && (m_audioRecorder.state() == RecordingState::Recording
            || m_audioRecorder.state() == RecordingState::Starting)) {
        m_audioRecorder.stopRecording(QStringLiteral("device-disconnected"));
    }
    if (state == DeviceState::Disconnected && m_waveformWidget != nullptr) {
        m_waveformWidget->clear();
        m_audioDroppedFrames += static_cast<quint64>(m_audioFrameQueue.size());
        m_videoDroppedFrames += static_cast<quint64>(m_videoFrameQueue.size());
        m_audioFrameQueue.clear();
        m_videoFrameQueue.clear();
        m_audioProcessing = false;
        m_videoProcessing = false;
    }
    setControlsEnabled(state == DeviceState::Connected && !m_lockControlCheck->isChecked());
    updateRecordingStartEnabled();
    updateStatistics();
}

void MainWindow::startRecording()
{
    const QVector<int> channels = selectedRecordingChannels();
    if (channels.isEmpty()) {
        m_recordingErrorLabel->setText(QStringLiteral("错误: 请至少选择一个录制通道"));
        return;
    }

    RecordingSettings settings;
    settings.outputDirectory = m_recordingDirectoryEdit->text();
    settings.baseName = QStringLiteral("MicArray_%1")
                            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    settings.channels = channels;
    settings.sampleRate = static_cast<int>(m_sampleRateSpin->value() * 1000.0);
    m_recordingErrorLabel->clear();
    m_audioRecorder.startRecording(settings);
}

void MainWindow::stopRecording()
{
    m_audioRecorder.stopRecording(QStringLiteral("user"));
}

void MainWindow::chooseRecordingDirectory()
{
    const QString directory = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("选择录音保存目录"),
        m_recordingDirectoryEdit->text());
    if (!directory.isEmpty()) {
        m_recordingDirectoryEdit->setText(directory);
        updateRecordingStartEnabled();
    }
}

void MainWindow::updateRecordingState(RecordingState state)
{
    m_recordingStateLabel->setText(QStringLiteral("状态: %1").arg(recordingStateName(state)));
    const bool active = state == RecordingState::Starting
        || state == RecordingState::Recording
        || state == RecordingState::Stopping;
    setRecordingConfigurationEnabled(!active);
    m_recordingStopButton->setEnabled(active);
    updateRecordingStartEnabled();
}

void MainWindow::updateRecordingStatistics(const RecordingStatistics &statistics)
{
    const qint64 elapsedSeconds = std::max<qint64>(0, statistics.elapsedMs / 1000);
    const qint64 hours = elapsedSeconds / 3600;
    const qint64 minutes = (elapsedSeconds % 3600) / 60;
    const qint64 seconds = elapsedSeconds % 60;
    const QString duration = QStringLiteral("%1:%2:%3")
                                 .arg(hours, 2, 10, QLatin1Char('0'))
                                 .arg(minutes, 2, 10, QLatin1Char('0'))
                                 .arg(seconds, 2, 10, QLatin1Char('0'));
    const double bytesPerSecond = statistics.elapsedMs > 0
        ? static_cast<double>(statistics.dataBytes) * 1000.0 / static_cast<double>(statistics.elapsedMs)
        : 0.0;
    m_recordingStatisticsLabel->setText(
        QStringLiteral("时长: %1\n已写: %2\n写入速率: %3\n待写队列: %4\n已写帧: %5\n未写帧: %6\n清单: %7")
            .arg(duration)
            .arg(formatByteSize(statistics.dataBytes))
            .arg(formatByteRate(bytesPerSecond))
            .arg(formatByteSize(statistics.pendingBytes))
            .arg(statistics.inputFrames)
            .arg(statistics.discardedFrames)
            .arg(statistics.outputPath.isEmpty() ? QStringLiteral("-") : QDir::toNativeSeparators(statistics.outputPath)));
}

void MainWindow::updateStatistics()
{
    m_endpointLabel->setText(QStringLiteral("端点: %1:%2").arg(m_hostEdit->text()).arg(m_portSpin->value()));
    m_heartbeatLabel->setText(QStringLiteral("心跳: 1s"));
    m_rateLabel->setText(QStringLiteral("接收: %1 / %2 帧")
                             .arg(formatByteSize(m_client.bytesReceived()))
                             .arg(m_client.framesReceived()));
    m_errorLabel->setText(QStringLiteral("错误: %1").arg(m_client.parseErrorCount()));
    const QString lastRx = m_client.lastReceiveTime().isValid()
        ? m_client.lastReceiveTime().toString(QStringLiteral("HH:mm:ss.zzz"))
        : QStringLiteral("-");
    m_protocolStatsLabel->setText(QStringLiteral("最后接收: %1\n健康状态: %2\n解析错误: %3\n接收字节: %4\n已解析字节: %5\n丢弃字节: %6\n协议缓冲: %7\n接收背压: %8\n待解析队列: %9\n重连次数: %10\n视频帧: %11\n音频字节: %12\n音频丢帧: %13\n视频丢帧: %14\n播放通道: %15\n平均窗口: %16 帧")
                                      .arg(lastRx)
                                      .arg(healthLevelName(m_healthSnapshot.level))
                                      .arg(m_client.parseErrorCount())
                                      .arg(formatByteSize(m_client.bytesReceived()))
                                      .arg(formatByteSize(m_client.protocolBytesParsed()))
                                      .arg(formatByteSize(m_client.protocolBytesDiscarded()))
                                      .arg(formatByteSize(static_cast<quint64>(std::max<qsizetype>(0, m_client.protocolBufferedBytes()))))
                                      .arg(m_client.receiveBackpressureActive() ? QStringLiteral("启用") : QStringLiteral("正常"))
                                      .arg(formatByteSize(static_cast<quint64>(std::max<qint64>(0, m_client.pendingProtocolBytes()))))
                                      .arg(m_client.reconnectCount())
                                      .arg(m_videoDecodedFrames)
                                      .arg(formatByteSize(static_cast<quint64>(std::max<qint64>(0, m_audioBytesReceived))))
                                      .arg(m_audioDroppedFrames)
                                      .arg(m_videoDroppedFrames)
                                      .arg(m_audioOutputChannel)
                                      .arg(m_audioAverageWindowFrames));
}

void MainWindow::appendLog(LogLevel, const QString &, const QString &, const QString &formatted)
{
    m_logView->appendPlainText(formatted);
    statusBar()->showMessage(formatted, 5000);
}

void MainWindow::showVideoFrame(const QImage &image, double fps, quint32 timestampSeconds, quint32 timestampMicros)
{
    ++m_videoDecodedFrames;
    m_pendingVideoImage = image;
    m_pendingVideoFps = fps;
    m_pendingVideoTimestampSeconds = timestampSeconds;
    m_pendingVideoTimestampMicros = timestampMicros;
    m_hasPendingVideoFrame = true;
}

void MainWindow::updateAudioLevel(double peakLevel, qint64 bytesReceived, quint32 timestampSeconds, quint32 timestampMicros)
{
    m_audioBytesReceived = bytesReceived;
    m_audioLevel->setValue(static_cast<int>(std::clamp(peakLevel, 0.0, 1.0) * 100.0));
    m_audioStatsLabel->setText(QStringLiteral("音频: %1  选中通道峰值 %2%  时间戳 %3.%4")
                                   .arg(formatByteSize(static_cast<quint64>(std::max<qint64>(0, bytesReceived))))
                                   .arg(static_cast<int>(std::clamp(peakLevel, 0.0, 1.0) * 100.0))
                                   .arg(timestampSeconds)
                                   .arg(timestampMicros, 6, 10, QLatin1Char('0')));
    updateStatistics();
}

void MainWindow::showArrayFrame(const DecodedAudioFrame &frame)
{
    m_audioOutputChannel = frame.outputChannel;
    if (m_dashboardHeatmap != nullptr && frame.valid) {
        m_dashboardHeatmap->setAmplitudes(frame.normalizedAmplitudes, frame.outputChannel);
    }
    m_pendingAudioFrame = frame;
    m_hasPendingAudioFrame = true;
}

void MainWindow::renderPendingVideoFrame()
{
    if (!m_hasPendingVideoFrame || m_pendingVideoImage.isNull()) {
        return;
    }

    m_videoLabel->setPixmap(QPixmap::fromImage(m_pendingVideoImage).scaled(m_videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_videoStatsLabel->setText(QStringLiteral("视频: %1x%2  %3 FPS  时间戳 %4.%5")
                                   .arg(m_pendingVideoImage.width())
                                   .arg(m_pendingVideoImage.height())
                                   .arg(m_pendingVideoFps, 0, 'f', 1)
                                   .arg(m_pendingVideoTimestampSeconds)
                                   .arg(m_pendingVideoTimestampMicros, 6, 10, QLatin1Char('0')));
    m_hasPendingVideoFrame = false;
    updateStatistics();
}

void MainWindow::renderPendingAudioFrame()
{
    if (!m_hasPendingAudioFrame) {
        return;
    }

    if (m_waveformWidget != nullptr && m_pendingAudioFrame.valid) {
        m_waveformWidget->appendPcm16(m_pendingAudioFrame.outputPcm16);
    }
    updateCalibrationGrid(m_pendingAudioFrame);
    m_hasPendingAudioFrame = false;
}

void MainWindow::updateAudioSettingsFromUi()
{
    AudioSettings audio;
    audio.sampleRate = static_cast<int>(m_sampleRateSpin->value() * 1000.0);
    audio.channelCount = m_channelCountSpin->value();
    audio.outputChannel = m_outputChannelSpin->value();
    audio.averageWindowFrames = m_averageWindowSpin->value();
    audio.playbackEnabled = m_audioPlaybackCheck->isChecked();
    m_audioOutputChannel = audio.outputChannel;
    m_audioAverageWindowFrames = audio.averageWindowFrames;
    QMetaObject::invokeMethod(m_audioProcessor,
                              "configure",
                              Qt::QueuedConnection,
                              Q_ARG(AudioSettings, audio));
    m_settings.saveAudio(audio);
    updateStatistics();
}

void MainWindow::updateDebugFrame(const HostFrame &frame)
{
    const quint16 rawType = frame.rawType != 0 ? frame.rawType : static_cast<quint16>(frame.type);
    QByteArray raw;
    appendU16(raw, ProtocolCodec::HostHeader);
    appendU16(raw, rawType);
    appendU32(raw, frame.payloadLength);
    appendU32(raw, frame.timestampSeconds);
    appendU32(raw, frame.timestampMicros);
    raw.append(frame.payload);
    if (rawType == static_cast<quint16>(FrameType::Video)) {
        raw.append(char(0x0a));
    } else {
        appendU16(raw, ProtocolCodec::HostTail);
    }

    const QString typeName = frameTypeDisplayName(rawType);
    const QString head = bytesToHexPreview(raw.left(16));
    const QString tail = bytesToHexPreview(raw.right(16));
    const QString postTailBytes = frame.postTailPreview.isEmpty()
        ? QStringLiteral("-")
        : bytesToHexPreview(frame.postTailPreview);
    const QString postTail = QStringLiteral("%1 (%2/16 B%3)")
                                 .arg(postTailBytes)
                                 .arg(frame.postTailPreview.size())
                                 .arg(frame.postTailPreviewComplete ? QString() : QStringLiteral("，不完整"));

    m_debugMetaLabel->setText(QStringLiteral("%1，载荷 %2 B，总长 %3 B，时间戳 %4.%5")
                                  .arg(typeName)
                                  .arg(frame.payloadLength)
                                  .arg(raw.size())
                                  .arg(frame.timestampSeconds)
                                  .arg(frame.timestampMicros, 6, 10, QLatin1Char('0')));
    m_debugHeadLabel->setText(head);
    m_debugTailLabel->setText(tail);
    m_debugPostTailLabel->setText(postTail);

    if (m_debugHistoryView) {
        m_debugHistoryView->appendPlainText(QStringLiteral("[%1] %2 | 长度 %3 B | 前16: %4 | 后16: %5 | 帧后16: %6")
                                                .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                                                .arg(typeName)
                                                .arg(raw.size())
                                                .arg(head)
                                                .arg(tail)
                                                .arg(postTail));
    }

    updateDebugSegments(frame);
    flashDebugSegments();
}

void MainWindow::updateDebugError(const ParseError &error)
{
    if (m_debugErrorView == nullptr) {
        return;
    }

    m_debugErrorView->appendPlainText(QStringLiteral("[%1] %2\n  type=0x%3 payloadLength=%4 tailOffset=%5 bufferSize=%6\n  actualTail=0x%7 expectedTail=0x%8\n  head16=%9\n  tailContext=%10")
                                          .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                                          .arg(error.message)
                                          .arg(error.rawType, 4, 16, QLatin1Char('0'))
                                          .arg(error.payloadLength)
                                          .arg(error.tailOffset)
                                          .arg(error.bufferSize)
                                          .arg(error.actualTail, 4, 16, QLatin1Char('0'))
                                          .arg(error.expectedTail, 4, 16, QLatin1Char('0'))
                                          .arg(bytesToHexPreview(error.headerPreview))
                                          .arg(bytesToHexPreview(error.tailContext)));
}

void MainWindow::updateCalibrationGrid(const DecodedAudioFrame &frame)
{
    if (!frame.valid || frame.normalizedAmplitudes.size() != AudioFrameDecoder::ChannelCount) {
        return;
    }

    for (int channel = 0; channel < AudioFrameDecoder::ChannelCount && channel < m_arrayCells.size(); ++channel) {
        const double normalized = frame.normalizedAmplitudes.at(channel);
        QLabel *cell = m_arrayCells.at(channel);
        cell->setText(QStringLiteral("%1\n%2%").arg(channel).arg(normalized * 100.0, 0, 'f', 1));
        cell->setStyleSheet(QStringLiteral("QLabel { background:%1; color:#ffffff; border:1px solid #333; font-weight:600; }")
                                .arg(calibrationColor(normalized)));
    }

    m_calibrationStatsLabel->setText(QStringLiteral("窗口: %1 帧，最大通道 %2=%3，最小通道 %4=%5，播放通道 %6")
                                         .arg(m_audioAverageWindowFrames)
                                         .arg(frame.maxAverageChannel)
                                         .arg(frame.maxAverageAmplitude, 0, 'f', 0)
                                         .arg(frame.minAverageChannel)
                                         .arg(frame.minAverageAmplitude, 0, 'f', 0)
                                         .arg(frame.outputChannel));
}

ControlValues MainWindow::collectControlValues() const
{
    ControlValues values;
    for (int i = 0; i < ControlValues::ChannelCount; ++i) {
        values.setChannelValue(i, static_cast<qint16>(m_controlSpins[i]->value()));
    }
    return values;
}

void MainWindow::setControlsEnabled(bool enabled)
{
    for (int i = 0; i < ControlValues::ChannelCount; ++i) {
        m_controlSpins[i]->setEnabled(enabled && !m_controlValues.isReserved(i));
    }
    m_sendControlButton->setEnabled(enabled);
}

void MainWindow::dispatchNextAudioFrame()
{
    if (m_audioProcessing || m_audioFrameQueue.isEmpty()) {
        return;
    }
    const HostFrame frame = m_audioFrameQueue.takeNext();
    m_audioProcessing = true;
    QMetaObject::invokeMethod(m_audioProcessor,
                              "processArrayFrame",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, frame.payload),
                              Q_ARG(quint32, frame.timestampSeconds),
                              Q_ARG(quint32, frame.timestampMicros));
}

void MainWindow::dispatchNextVideoFrame()
{
    if (m_videoProcessing || m_videoFrameQueue.isEmpty()) {
        return;
    }
    const HostFrame frame = m_videoFrameQueue.takeNext();
    m_videoProcessing = true;
    QMetaObject::invokeMethod(m_videoDecoder,
                              "decodeJpeg",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, frame.payload),
                              Q_ARG(quint32, frame.timestampSeconds),
                              Q_ARG(quint32, frame.timestampMicros));
}

void MainWindow::updateHealthSnapshot()
{
    HealthInputs inputs;
    inputs.connected = m_client.state() == DeviceState::Connected;
    inputs.lastReceiveAgeMs = m_client.lastReceiveAgeMs();
    inputs.totalBytes = m_client.bytesReceived();
    inputs.totalFrames = m_client.framesReceived();
    inputs.parseErrors = m_client.parseErrorCount();
    inputs.backpressureActive = m_client.receiveBackpressureActive();
    inputs.pendingProtocolBytes = m_client.pendingProtocolBytes();
    inputs.audioQueueDepth = m_audioFrameQueue.size() + (m_audioProcessing ? 1 : 0);
    inputs.audioDroppedFrames = m_audioDroppedFrames;
    inputs.videoDroppedFrames = m_videoDroppedFrames;
    m_healthSnapshot = m_healthMonitor.update(inputs, m_sessionTimer.elapsed());
    const double receiveMbps = m_healthSnapshot.receiveBytesPerSecond * 8.0 / 1000000.0;
    m_peakReceiveMbps = std::max(m_peakReceiveMbps, receiveMbps);

    const QString color = m_healthSnapshot.level == HealthLevel::Healthy
        ? QStringLiteral("#1f9d62")
        : m_healthSnapshot.level == HealthLevel::Warning
            ? QStringLiteral("#c58b19")
            : m_healthSnapshot.level == HealthLevel::Critical
                ? QStringLiteral("#c53b3b")
                : QStringLiteral("#52606d");
    m_healthLabel->setText(QStringLiteral("健康: %1\n%2").arg(healthLevelName(m_healthSnapshot.level), m_healthSnapshot.reason));
    m_healthLabel->setStyleSheet(QStringLiteral("QLabel { background:%1; color:white; border-radius:5px; padding:5px; font-weight:700; }").arg(color));
    if (m_trendChart != nullptr) {
        m_trendChart->appendSample(receiveMbps, m_healthSnapshot.framesPerSecond);
    }
    if (m_healthSnapshot.level != m_lastHealthLevel) {
        const LogLevel level = m_healthSnapshot.level == HealthLevel::Critical
            ? LogLevel::Error
            : m_healthSnapshot.level == HealthLevel::Warning ? LogLevel::Warning : LogLevel::Info;
        appendAlert(level, QStringLiteral("健康状态切换为 %1：%2")
                               .arg(healthLevelName(m_healthSnapshot.level), m_healthSnapshot.reason));
        m_lastHealthLevel = m_healthSnapshot.level;
    }
    updateStatistics();
}

void MainWindow::togglePresentationMode()
{
    m_presentationMode = !m_presentationMode;
    if (m_mainTabs != nullptr) {
        m_mainTabs->setCurrentIndex(0);
        m_mainTabs->tabBar()->setVisible(!m_presentationMode);
    }
    m_presentationButton->setText(m_presentationMode ? QStringLiteral("退出全屏") : QStringLiteral("演示全屏"));
    if (m_presentationMode) {
        showFullScreen();
    } else {
        showNormal();
    }
}

QVector<qint16> MainWindow::currentControlVector() const
{
    QVector<qint16> values;
    values.reserve(ControlValues::ChannelCount);
    for (QSpinBox *spin : m_controlSpins) {
        values.append(static_cast<qint16>(spin->value()));
    }
    return values;
}

void MainWindow::setControlVector(const QVector<qint16> &values)
{
    if (values.size() != ControlValues::ChannelCount) {
        return;
    }
    for (int index = 0; index < ControlValues::ChannelCount; ++index) {
        m_controlSpins[index]->setValue(values.at(index));
    }
    markControlDirty();
}

void MainWindow::refreshControlPresets()
{
    if (m_controlPresetCombo == nullptr) {
        return;
    }
    const QString selected = m_controlPresetCombo->currentData().toString();
    m_controlPresetCombo->clear();
    for (const ControlPreset &preset : m_settings.controlPresets()) {
        m_controlPresetCombo->addItem(preset.name, preset.name);
    }
    const int index = m_controlPresetCombo->findData(selected);
    if (index >= 0) {
        m_controlPresetCombo->setCurrentIndex(index);
    }
    const bool available = m_controlPresetCombo->count() > 0;
    m_applyPresetButton->setEnabled(available);
    m_deletePresetButton->setEnabled(available);
}

void MainWindow::applyControlPreset()
{
    const QString name = m_controlPresetCombo->currentData().toString();
    for (const ControlPreset &preset : m_settings.controlPresets()) {
        if (preset.name == name) {
            setControlVector(preset.values);
            appendAlert(LogLevel::Info, QStringLiteral("已应用本地预设“%1”，尚未发送").arg(name));
            return;
        }
    }
}

void MainWindow::saveControlPreset()
{
    bool accepted = false;
    const QString suggestedName = m_controlPresetCombo->currentText();
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("保存控制预设"),
                                               QStringLiteral("预设名称（最多 32 字符）"),
                                               QLineEdit::Normal,
                                               suggestedName,
                                               &accepted).trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    ControlPreset preset {name, currentControlVector()};
    if (!m_settings.saveControlPreset(preset)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("预设名称或控制值无效。"));
        return;
    }
    refreshControlPresets();
    const int index = m_controlPresetCombo->findData(name);
    if (index >= 0) {
        m_controlPresetCombo->setCurrentIndex(index);
    }
    appendAlert(LogLevel::Info, QStringLiteral("本地控制预设“%1”已保存").arg(name));
}

void MainWindow::deleteControlPreset()
{
    const QString name = m_controlPresetCombo->currentData().toString();
    if (name.isEmpty()) {
        return;
    }
    if (QMessageBox::question(this,
                              QStringLiteral("删除预设"),
                              QStringLiteral("确认删除本地预设“%1”？").arg(name)) != QMessageBox::Yes) {
        return;
    }
    if (m_settings.removeControlPreset(name)) {
        refreshControlPresets();
        appendAlert(LogLevel::Info, QStringLiteral("本地控制预设“%1”已删除").arg(name));
    }
}

void MainWindow::restoreLastSentControl()
{
    if (!m_hasLastSentControl) {
        return;
    }
    setControlVector(m_lastSentControlValues);
    appendAlert(LogLevel::Info, QStringLiteral("已恢复上次发送值，尚未重新发送"));
}

void MainWindow::appendAlert(LogLevel level, const QString &message)
{
    if (m_alertTimeline == nullptr) {
        return;
    }
    const QString key = QStringLiteral("%1:%2").arg(static_cast<int>(level)).arg(message);
    const AlertDecision decision = m_alertCoalescer.record(key, m_sessionTimer.elapsed());
    if (!decision.shouldEmit) {
        return;
    }
    const QString levelText = level == LogLevel::Error ? QStringLiteral("严重")
                            : level == LogLevel::Warning ? QStringLiteral("警告")
                                                         : QStringLiteral("信息");
    const QString repeated = decision.suppressedCount > 0
        ? QStringLiteral("（已合并 %1 条重复）").arg(decision.suppressedCount)
        : QString();
    const QString formatted = QStringLiteral("[%1] [%2] %3%4")
                                  .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), levelText, message, repeated);
    m_alertTimeline->appendPlainText(formatted);
    m_alertEvents.append(formatted);
    while (m_alertEvents.size() > 1000) {
        m_alertEvents.removeFirst();
    }
}

void MainWindow::exportDiagnostics()
{
    const QString baseDirectory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择诊断包导出目录"));
    if (baseDirectory.isEmpty()) {
        return;
    }

    const qint64 elapsedMs = std::max<qint64>(1, m_sessionTimer.elapsed());
    DiagnosticBundle bundle;
    bundle.session.startedAt = m_sessionStartedAt;
    bundle.session.endedAt = QDateTime::currentDateTime();
    bundle.session.totalBytes = m_client.bytesReceived();
    bundle.session.totalFrames = m_client.framesReceived();
    bundle.session.parseErrors = m_client.parseErrorCount();
    bundle.session.reconnectCount = m_client.reconnectCount();
    bundle.session.audioDroppedFrames = m_audioDroppedFrames;
    bundle.session.videoDroppedFrames = m_videoDroppedFrames;
    bundle.session.averageReceiveMbps = static_cast<double>(m_client.bytesReceived()) * 8.0 / (static_cast<double>(elapsedMs) * 1000.0);
    bundle.session.peakReceiveMbps = m_peakReceiveMbps;

    const ConnectionSettings connection = m_settings.connection();
    const AudioSettings audio = m_settings.audio();
    const VideoSettings video = m_settings.video();
    bundle.settings.insert(QStringLiteral("schemaVersion"), AppSettings::SchemaVersion);
    bundle.settings.insert(QStringLiteral("connection"), QVariantMap {
        {QStringLiteral("host"), connection.host},
        {QStringLiteral("port"), connection.port},
        {QStringLiteral("autoReconnect"), connection.autoReconnect},
    });
    bundle.settings.insert(QStringLiteral("audio"), QVariantMap {
        {QStringLiteral("sampleRate"), audio.sampleRate},
        {QStringLiteral("channelCount"), audio.channelCount},
        {QStringLiteral("outputChannel"), audio.outputChannel},
        {QStringLiteral("averageWindowFrames"), audio.averageWindowFrames},
        {QStringLiteral("playbackEnabled"), audio.playbackEnabled},
    });
    bundle.settings.insert(QStringLiteral("video"), QVariantMap {{QStringLiteral("format"), video.format}});
    QStringList presetNames;
    for (const ControlPreset &preset : m_settings.controlPresets()) {
        presetNames.append(preset.name);
    }
    bundle.settings.insert(QStringLiteral("controlPresetNames"), presetNames);
    bundle.events = m_alertEvents;

    QString error;
    const QString path = DiagnosticExporter::exportBundle(baseDirectory, bundle, m_logger.logFiles(), &error);
    if (path.isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("导出失败"), error);
        return;
    }
    appendAlert(LogLevel::Info, QStringLiteral("诊断包已导出到 %1").arg(path));
    QMessageBox::information(this, QStringLiteral("导出完成"), QStringLiteral("诊断包已保存到：\n%1").arg(path));
}

void MainWindow::resetSettingsToDefaults()
{
    if (QMessageBox::question(this,
                              QStringLiteral("恢复默认设置"),
                              QStringLiteral("连接、音视频设置和本地控制预设都将被清除。是否继续？"),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    m_settings.resetAll();
    loadSettings();
    refreshControlPresets();
    appendAlert(LogLevel::Info, QStringLiteral("全部本地设置已恢复默认值"));
}

QVector<int> MainWindow::selectedRecordingChannels() const
{
    QVector<int> channels;
    for (int channel = 0; channel < m_recordingChannelButtons.size(); ++channel) {
        if (m_recordingChannelButtons.at(channel)->isChecked()) {
            channels.append(channel);
        }
    }
    return channels;
}

void MainWindow::setRecordingConfigurationEnabled(bool enabled)
{
    for (QToolButton *button : m_recordingChannelButtons) {
        button->setEnabled(enabled);
    }
    m_recordingBrowseButton->setEnabled(enabled);
    m_recordingDirectoryEdit->setEnabled(enabled);
    m_recordingSelectionCommands->setEnabled(enabled);
    m_sampleRateSpin->setEnabled(enabled);
    m_resetSettingsButton->setEnabled(enabled);
}

void MainWindow::updateRecordingStartEnabled()
{
    const RecordingState recordingState = m_audioRecorder.state();
    const bool idle = recordingState == RecordingState::Idle
        || recordingState == RecordingState::Error;
    const QFileInfo directoryInfo(m_recordingDirectoryEdit->text());
    const bool canStart = idle
        && m_client.state() == DeviceState::Connected
        && !selectedRecordingChannels().isEmpty()
        && directoryInfo.isDir()
        && directoryInfo.isWritable();
    m_recordingStartButton->setEnabled(canStart);
}

QString MainWindow::bytesToHexPreview(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex(' ').toUpper());
}

QString MainWindow::frameTypeDisplayName(quint16 rawType)
{
    return frameTypeName(rawType);
}

void MainWindow::updateDebugSegments(const HostFrame &frame)
{
    if (m_debugSegmentLabels.size() < 7) {
        return;
    }

    const quint16 rawType = frame.rawType != 0 ? frame.rawType : static_cast<quint16>(frame.type);
    const bool knownMediaType = rawType == static_cast<quint16>(FrameType::Audio)
        || rawType == static_cast<quint16>(FrameType::Video);
    const QString typeColor = knownMediaType ? QStringLiteral("#1f6f4a") : QStringLiteral("#8a5a10");
    const QString payloadColor = frame.payloadLength > 0 ? QStringLiteral("#1f4f78") : QStringLiteral("#39424e");

    const QString tailText = rawType == static_cast<quint16>(FrameType::Video)
        ? QStringLiteral("帧尾\n0x0A")
        : QStringLiteral("帧尾\n0x%1").arg(ProtocolCodec::HostTail, 4, 16, QLatin1Char('0'));

    const QStringList texts = {
        QStringLiteral("帧头\n0x%1").arg(ProtocolCodec::HostHeader, 4, 16, QLatin1Char('0')),
        QStringLiteral("类型\n0x%1").arg(rawType, 4, 16, QLatin1Char('0')),
        QStringLiteral("长度\n%1 B").arg(frame.payloadLength),
        QStringLiteral("秒时间戳\n%1").arg(frame.timestampSeconds),
        QStringLiteral("微秒时间戳\n%1").arg(frame.timestampMicros),
        QStringLiteral("Payload\n%1 B").arg(frame.payloadLength),
        tailText,
    };
    const QStringList colors = {
        QStringLiteral("#5b3f8c"),
        typeColor,
        QStringLiteral("#325f6d"),
        QStringLiteral("#5b4f2a"),
        QStringLiteral("#5b4f2a"),
        payloadColor,
        QStringLiteral("#7a3030"),
    };

    for (int i = 0; i < m_debugSegmentLabels.size() && i < texts.size(); ++i) {
        QLabel *segment = m_debugSegmentLabels.at(i);
        segment->setText(texts.at(i));
        segment->setStyleSheet(QStringLiteral("QLabel { background:%1; color:#ffffff; border:2px solid #d7e3f4; font-weight:700; }")
                                   .arg(colors.at(i)));
    }
}

void MainWindow::flashDebugSegments()
{
    for (QLabel *segment : m_debugSegmentLabels) {
        auto *effect = new QGraphicsOpacityEffect(segment);
        segment->setGraphicsEffect(effect);
        auto *animation = new QPropertyAnimation(effect, "opacity", effect);
        animation->setDuration(260);
        animation->setStartValue(0.45);
        animation->setEndValue(1.0);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(animation, &QPropertyAnimation::finished, segment, [segment, effect]() {
            if (segment->graphicsEffect() == effect) {
                segment->setGraphicsEffect(nullptr);
            }
        });
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

QString MainWindow::calibrationColor(double normalized)
{
    const double bounded = std::clamp(normalized, 0.0, 1.0);
    const int hue = static_cast<int>((1.0 - bounded) * 220.0);
    const QColor color = QColor::fromHsv(hue, 210, 210);
    return color.name(QColor::HexRgb);
}

} // namespace MicArray
