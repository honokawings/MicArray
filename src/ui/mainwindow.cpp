#include "ui/mainwindow.h"

#include <QAbstractAnimation>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QEasingCurve>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMetaObject>
#include <QPropertyAnimation>
#include <QSplitter>
#include <QStatusBar>
#include <QTime>
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
    updateConnectionState(DeviceState::Disconnected);
    updateStatistics();
    m_logger.info(QStringLiteral("app"), QStringLiteral("MicArray 上位机已启动"));
}

MainWindow::~MainWindow()
{
    m_audioThread.quit();
    m_videoThread.quit();
    m_audioThread.wait(2000);
    m_videoThread.wait(2000);
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
    for (QLabel *label : {m_stateLabel, m_endpointLabel, m_heartbeatLabel, m_rateLabel, m_errorLabel}) {
        label->setMinimumWidth(150);
        statusLayout->addWidget(label);
    }
    statusLayout->addStretch();
    root->addWidget(statusBand);

    auto *tabs = new QTabWidget(central);
    tabs->addTab(buildMonitorPage(), QStringLiteral("实时监控"));
    tabs->addTab(buildDebugPage(), QStringLiteral("帧调试"));
    tabs->addTab(buildCalibrationPage(), QStringLiteral("阵列校准"));
    root->addWidget(tabs, 1);

    setCentralWidget(central);
}

QWidget *MainWindow::buildMonitorPage()
{
    auto *page = new QWidget(this);
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
        m_controlSpins[i]->setRange(-32768, 32767);
        m_controlSpins[i]->setSuffix(QStringLiteral(" int16"));
        if (m_controlValues.isReserved(i)) {
            m_controlSpins[i]->setEnabled(false);
        }
        controlLayout->addRow(QStringLiteral("接口 %1%2").arg(i).arg(m_controlValues.isReserved(i) ? QStringLiteral(" 预留") : QString()), m_controlSpins[i]);
    }
    m_lockControlCheck = new QCheckBox(QStringLiteral("锁定控制"), controlGroup);
    m_sendControlButton = new QPushButton(QStringLiteral("发送控制"), controlGroup);
    m_resetControlButton = new QPushButton(QStringLiteral("恢复默认"), controlGroup);
    controlLayout->addRow(QString(), m_lockControlCheck);
    controlLayout->addRow(m_sendControlButton, m_resetControlButton);
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
    m_outputChannelSpin->setRange(0, AudioFrameDecoder::ChannelCount - 1);
    m_averageWindowSpin = new QSpinBox(settingsGroup);
    m_averageWindowSpin->setRange(1, 10000);
    m_averageWindowSpin->setSuffix(QStringLiteral(" 帧"));
    m_audioPlaybackCheck = new QCheckBox(QStringLiteral("启用选中通道播放"), settingsGroup);
    m_videoFormatCombo = new QComboBox(settingsGroup);
    m_videoFormatCombo->addItem(QStringLiteral("JPEG"), QStringLiteral("JPG"));
    settingsLayout->addRow(QStringLiteral("采样率"), m_sampleRateSpin);
    settingsLayout->addRow(QStringLiteral("阵列通道数"), m_channelCountSpin);
    settingsLayout->addRow(QStringLiteral("播放通道"), m_outputChannelSpin);
    settingsLayout->addRow(QStringLiteral("平均窗口"), m_averageWindowSpin);
    settingsLayout->addRow(QString(), m_audioPlaybackCheck);
    settingsLayout->addRow(QStringLiteral("视频格式"), m_videoFormatCombo);
    leftLayout->addWidget(settingsGroup);
    leftLayout->addStretch();
    mainSplitter->addWidget(left);

    auto *center = new QWidget(mainSplitter);
    auto *centerLayout = new QVBoxLayout(center);
    m_videoLabel = new QLabel(QStringLiteral("等待视频帧"), center);
    m_videoLabel->setAlignment(Qt::AlignCenter);
    m_videoLabel->setMinimumSize(640, 360);
    m_videoLabel->setStyleSheet(QStringLiteral("QLabel { background:#111; color:#ddd; border:1px solid #555; }"));
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
    mainSplitter->addWidget(center);

    auto *right = new QWidget(mainSplitter);
    auto *rightLayout = new QVBoxLayout(right);
    auto *diagnosticGroup = new QGroupBox(QStringLiteral("诊断"), right);
    auto *diagnosticLayout = new QVBoxLayout(diagnosticGroup);
    m_protocolStatsLabel = new QLabel(diagnosticGroup);
    m_protocolStatsLabel->setWordWrap(true);
    diagnosticLayout->addWidget(m_protocolStatsLabel);
    rightLayout->addWidget(diagnosticGroup);

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
    m_debugMetaLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_debugHeadLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_debugTailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    const QString hexPreviewStyle = QStringLiteral("QLabel { font-family: Consolas, 'Courier New', monospace; }");
    m_debugHeadLabel->setStyleSheet(hexPreviewStyle);
    m_debugTailLabel->setStyleSheet(hexPreviewStyle);
    form->addRow(QStringLiteral("元信息"), m_debugMetaLabel);
    form->addRow(QStringLiteral("前 16 字节"), m_debugHeadLabel);
    form->addRow(QStringLiteral("尾 16 字节"), m_debugTailLabel);
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
        QMetaObject::invokeMethod(m_audioProcessor,
                                  "processArrayFrame",
                                  Qt::QueuedConnection,
                                  Q_ARG(QByteArray, frame.payload),
                                  Q_ARG(quint32, frame.timestampSeconds),
                                  Q_ARG(quint32, frame.timestampMicros));
    });
    connect(&m_client, &DeviceClient::videoFrameReceived, this, [this](const HostFrame &frame) {
        QMetaObject::invokeMethod(m_videoDecoder,
                                  "decodeJpeg",
                                  Qt::QueuedConnection,
                                  Q_ARG(QByteArray, frame.payload),
                                  Q_ARG(quint32, frame.timestampSeconds),
                                  Q_ARG(quint32, frame.timestampMicros));
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
    connect(&m_logger, &Logger::messageLogged, this, &MainWindow::appendLog);
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
    if (m_client.sendControl(collectControlValues())) {
        m_controlDirty = false;
        m_sendControlButton->setText(QStringLiteral("发送控制"));
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
    m_sendControlButton->setText(QStringLiteral("发送控制 *"));
}

void MainWindow::updateConnectionState(DeviceState state)
{
    m_stateLabel->setText(QStringLiteral("状态: %1").arg(deviceStateName(state)));
    m_connectButton->setText(state == DeviceState::Connected || state == DeviceState::Connecting || state == DeviceState::Reconnecting
                                 ? QStringLiteral("断开")
                                 : QStringLiteral("连接"));
    if (state == DeviceState::Disconnected && m_waveformWidget != nullptr) {
        m_waveformWidget->clear();
    }
    setControlsEnabled(state == DeviceState::Connected && !m_lockControlCheck->isChecked());
    updateStatistics();
}

void MainWindow::updateStatistics()
{
    m_endpointLabel->setText(QStringLiteral("端点: %1:%2").arg(m_hostEdit->text()).arg(m_portSpin->value()));
    m_heartbeatLabel->setText(QStringLiteral("心跳: 1s"));
    m_rateLabel->setText(QStringLiteral("接收: %1 B / %2 帧").arg(m_client.bytesReceived()).arg(m_client.framesReceived()));
    m_errorLabel->setText(QStringLiteral("错误: %1").arg(m_client.parseErrorCount()));
    const QString lastRx = m_client.lastReceiveTime().isValid()
        ? m_client.lastReceiveTime().toString(QStringLiteral("HH:mm:ss.zzz"))
        : QStringLiteral("-");
    m_protocolStatsLabel->setText(QStringLiteral("最后接收: %1\n解析错误: %2\n接收字节: %3\n已解析字节: %4\n丢弃字节: %5\n协议缓冲: %6 B\n接收背压: %7\n待解析队列: %8 B\n视频帧: %9\n音频字节: %10\n音频丢帧: %11\n播放通道: %12\n平均窗口: %13 帧")
                                      .arg(lastRx)
                                      .arg(m_client.parseErrorCount())
                                      .arg(m_client.bytesReceived())
                                      .arg(m_client.protocolBytesParsed())
                                      .arg(m_client.protocolBytesDiscarded())
                                      .arg(m_client.protocolBufferedBytes())
                                      .arg(m_client.receiveBackpressureActive() ? QStringLiteral("启用") : QStringLiteral("正常"))
                                      .arg(m_client.pendingProtocolBytes())
                                      .arg(m_videoDecodedFrames)
                                      .arg(m_audioBytesReceived)
                                      .arg(m_audioDroppedFrames)
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
    m_audioStatsLabel->setText(QStringLiteral("音频: %1 B  选中通道峰值 %2%  时间戳 %3.%4")
                                   .arg(bytesReceived)
                                   .arg(static_cast<int>(std::clamp(peakLevel, 0.0, 1.0) * 100.0))
                                   .arg(timestampSeconds)
                                   .arg(timestampMicros, 6, 10, QLatin1Char('0')));
    updateStatistics();
}

void MainWindow::showArrayFrame(const DecodedAudioFrame &frame)
{
    m_audioOutputChannel = frame.outputChannel;
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

    m_debugMetaLabel->setText(QStringLiteral("%1，载荷 %2 B，总长 %3 B，时间戳 %4.%5")
                                  .arg(typeName)
                                  .arg(frame.payloadLength)
                                  .arg(raw.size())
                                  .arg(frame.timestampSeconds)
                                  .arg(frame.timestampMicros, 6, 10, QLatin1Char('0')));
    m_debugHeadLabel->setText(head);
    m_debugTailLabel->setText(tail);

    if (m_debugHistoryView) {
        m_debugHistoryView->appendPlainText(QStringLiteral("[%1] %2 | 长度 %3 B | 前16: %4 | 后16: %5")
                                                .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                                                .arg(typeName)
                                                .arg(raw.size())
                                                .arg(head)
                                                .arg(tail));
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
