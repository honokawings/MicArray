#include "mainwindow.h"
#include "hardwareprofile.h"
#include "sys3panel.h"
#include "cameraprofile.h"
#include "channelmaskwidget.h"
#include "analysis/cameraviewcalibrator.h"

#include <QAudioSink>
#include <QCamera>
#include <QCameraDevice>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaDevices>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace MicArray::Array128 {
namespace {
class NoWheelSpin final : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;
protected:
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};
class NoWheelInteger final : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;
protected:
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};
class NoWheelCombo final : public QComboBox
{
public:
    using QComboBox::QComboBox;
protected:
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};
QLabel *label(const QString &text = {})
{
    auto *result = new QLabel(text); result->setWordWrap(true); result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return result;
}
QDoubleSpinBox *number(double low, double high, double value, int decimals = 2)
{
    auto *spin = new NoWheelSpin;
    spin->setRange(low, high); spin->setDecimals(decimals); spin->setValue(value); spin->setKeyboardTracking(false);
    return spin;
}
QSpinBox *integer(int low, int high, int value)
{
    auto *spin = new NoWheelInteger; spin->setRange(low, high); spin->setValue(value); spin->setKeyboardTracking(false); return spin;
}
QGroupBox *group(const QString &title, QWidget *content)
{
    auto *box = new QGroupBox(title); auto *layout = new QVBoxLayout(box); layout->addWidget(content); return box;
}
QWidget *formPage(QFormLayout **form)
{
    auto *page = new QWidget; *form = new QFormLayout(page);
    (*form)->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); return page;
}
QString counter(const QJsonObject &json, const QString &name) { return json[name].toString("0"); }
QString mib(quint64 bytes) { return QString::number(bytes / 1048576.0, 'f', 2) + " MiB"; }
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent),
    m_store(QSettings::defaultFormat(), QSettings::UserScope, "MicArray", QCoreApplication::applicationName().endsWith("Tests") ? "MicArray128Tests" : "MicArray128")
{
    setWindowTitle(QStringLiteral("M128 阵列采集系统"));
    resize(1440, 900);
    setMinimumSize(1120, 700);
    auto *central = new QWidget; auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(12, 8, 12, 8); layout->setSpacing(8);
    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("M128  阵列采集系统")); title->setObjectName("appTitle");
    heading->addWidget(title);
    auto *formatLabel = new QLabel(QStringLiteral("128 CH  /  48 kHz  /  PCM24")); formatLabel->setObjectName("secondaryText");
    heading->addWidget(formatLabel); heading->addStretch();
    m_health = label(QStringLiteral("接收未启动")); m_health->setObjectName("acquisitionStatus"); heading->addWidget(m_health);
    auto *theme = new QPushButton(QStringLiteral("主题")); theme->setToolTip(QStringLiteral("切换浅色 / 深色显示")); heading->addWidget(theme);
    auto *fullscreen = new QPushButton(QStringLiteral("全屏")); heading->addWidget(fullscreen);
    layout->addLayout(heading);
    m_bind = new QLineEdit("0.0.0.0"); m_bind->setObjectName("bindAddress");
    m_port = integer(1, 65535, 5000); m_port->setObjectName("localPort");
    m_source = new QLineEdit("192.168.10.2"); m_source->setObjectName("deviceAddress");
    m_sourcePort = integer(1, 65535, 5000); m_sourcePort->setObjectName("devicePort");
    m_connect = new QPushButton(QStringLiteral("开始接收")); m_connect->setObjectName("receiveButton");
    auto *operations = new QHBoxLayout;
    m_endpoint = label(QStringLiteral("UDP 192.168.10.2:5000")); m_endpoint->setWordWrap(false);
    operations->addWidget(m_endpoint); operations->addWidget(m_connect);
    auto *interfaceSettings = new QPushButton(QStringLiteral("接口设置")); interfaceSettings->setObjectName("interfaceSettingsButton");
    operations->addWidget(interfaceSettings); operations->addSpacing(12);
    operations->addWidget(new QLabel(QStringLiteral("视频")));
    m_cameras = new NoWheelCombo; m_cameras->setMinimumWidth(135); m_cameras->setMaximumWidth(220);
    m_cameras->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_cameras->setMinimumContentsLength(12);
    for (const auto &camera : QMediaDevices::videoInputs()) m_cameras->addItem(camera.description(), camera.id());
    if (!m_cameras->count()) m_cameras->addItem(QStringLiteral("未检测到摄像头"));
    m_cameraButton = new QPushButton(QStringLiteral("打开视频"));
    auto *photo = new QPushButton(QStringLiteral("导入图像")); photo->setToolTip(QStringLiteral("导入静态参考图像，同时关闭视频采集"));
    operations->addWidget(m_cameras, 1); operations->addWidget(m_cameraButton); operations->addWidget(photo); operations->addStretch();
    layout->addLayout(operations);
    auto *statusRow = new QHBoxLayout;
    m_rates = label(QStringLiteral("接收速率 —  /  UDP 负载 —  /  数据时效 —")); m_rates->setObjectName("secondaryText");
    m_geometryLabel = label(QStringLiteral("阵列：未配置")); m_geometryLabel->setObjectName("geometryStatus");
    for (auto *singleLine : {m_health, m_rates, m_geometryLabel}) singleLine->setWordWrap(false);
    statusRow->addWidget(m_rates); statusRow->addStretch(); statusRow->addWidget(m_geometryLabel);
    auto *geometrySettings = new QPushButton(QStringLiteral("阵列配置")); geometrySettings->setObjectName("geometrySettingsButton"); statusRow->addWidget(geometrySettings);
    layout->addLayout(statusRow);
    m_notice = new QWidget; m_notice->setObjectName("operationNotice");
    auto *noticeLayout = new QHBoxLayout(m_notice); noticeLayout->setContentsMargins(8, 4, 8, 4);
    m_noticeText = label(); m_noticeText->setObjectName("operationNoticeText"); noticeLayout->addWidget(m_noticeText, 1);
    auto *dismiss = new QPushButton(QStringLiteral("关闭提示")); noticeLayout->addWidget(dismiss);
    connect(dismiss, &QPushButton::clicked, m_notice, &QWidget::hide);
    layout->addWidget(m_notice); m_notice->hide();
    m_pages = new QTabWidget; m_pages->setObjectName("mainPages");
    m_pages->addTab(buildMonitorPage(), QStringLiteral("实时监测"));
    m_pages->addTab(buildLocalizationPage(), QStringLiteral("声源定位"));
    m_pages->addTab(buildDenoisePage(), QStringLiteral("定向降噪"));
    m_pages->addTab(buildRecordingPage(), QStringLiteral("音频录制"));
    m_pages->addTab(buildGeometryPage(), QStringLiteral("系统配置"));
    m_pages->addTab(buildDebugPage(), QStringLiteral("设备诊断"));
    layout->addWidget(m_pages, 1); setCentralWidget(central);
    connect(interfaceSettings, &QPushButton::clicked, this, [this] { navigate(Page::Configuration); m_configTabs->setCurrentIndex(0); });
    connect(geometrySettings, &QPushButton::clicked, this, [this] { navigate(Page::Configuration); m_configTabs->setCurrentIndex(1); });
    m_videoSink = new QVideoSink(this); m_captureSession.setVideoSink(m_videoSink);
    connect(m_videoSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        const qint64 arrived = m_pipeline.monotonicNowNs();
        const auto image = frame.toImage(); if (image.isNull()) return;
        m_monitorView->setImage(image); m_noiseView->setImage(image);
        if (!m_fusionPaintedFrame) m_localView->setImage(image);
        if (m_camera && m_camera->isActive() && m_fusionEnabled->isChecked()) m_fusionVideo.append(image, arrived);
    });
    connect(m_cameraButton, &QPushButton::clicked, this, &MainWindow::toggleCamera);
    auto *mediaDevices = new QMediaDevices(this);
    connect(mediaDevices, &QMediaDevices::audioOutputsChanged, this, [this] {
        if (!m_monitorEnabled->isChecked()) return;
        m_monitorEnabled->setChecked(false);
        showError(QStringLiteral("音频输出设备已改变，监听已停止。请检查系统默认输出后重新开启监听；录制不受影响。"));
    });
    connect(mediaDevices, &QMediaDevices::videoInputsChanged, this, [this] {
        const auto selected = m_cameras->currentData().toByteArray();
        const auto devices = QMediaDevices::videoInputs();
        m_cameras->clear(); int index = -1;
        for (const auto &device : devices) {
            m_cameras->addItem(device.description(), device.id());
            if (device.id() == selected) index = m_cameras->count() - 1;
        }
        if (!m_cameras->count()) m_cameras->addItem(QStringLiteral("未检测到摄像头"));
        if (index >= 0) m_cameras->setCurrentIndex(index);
        else if (m_camera) toggleCamera();
    });
    connect(photo, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("静态参考照片"), {}, "Images (*.jpg *.png *.bmp)");
        if (path.isEmpty()) return; QImage image(path);
        if (image.isNull()) { showError(QStringLiteral("无法读取照片")); return; }
        if (m_camera) toggleCamera();
        m_monitorView->setImage(image); m_localView->setImage(image); m_noiseView->setImage(image);
    });
    connect(m_connect, &QPushButton::clicked, this, [this] {
        if (m_receiving) { m_pipeline.stop(); m_receiving = false; resetAudio(); resetFusion(); }
        else startReceiving();
        m_connect->setText(m_receiving ? QStringLiteral("停止接收") : QStringLiteral("开始接收"));
    });
    connect(theme, &QPushButton::clicked, this, [this] { const bool dark = !m_store.value("dark", true).toBool(); m_store.setValue("dark", dark); applyTheme(dark); });
    connect(fullscreen, &QPushButton::clicked, this, [this] { if (isFullScreen()) showNormal(); else showFullScreen(); });
    connect(new QShortcut(QKeySequence(Qt::Key_Escape), this), &QShortcut::activated, this, [this] { if (isFullScreen()) showNormal(); });
    loadSettings();
    updateGeometryTable();
    updateCalibrationTable();
    m_loading = false; applySettings();
    applyTheme(m_store.value("dark", true).toBool());
    navigate(Page::Diagnostics);
    m_refresh = new QTimer(this); m_refresh->setInterval(100);
    connect(m_refresh, &QTimer::timeout, this, &MainWindow::refresh); m_refresh->start();
    m_audioTimer = new QTimer(this); m_audioTimer->setInterval(15);
    connect(m_audioTimer, &QTimer::timeout, this, &MainWindow::pumpAudio); m_audioTimer->start();
    statusBar()->showMessage(QStringLiteral("M128 UDP 数据接收  |  本机 USB 视频  |  UART 串口按需连接"));
    refresh();
}

MainWindow::~MainWindow()
{
    if (m_camera) m_camera->stop();
    m_captureSession.setCamera(nullptr);
    m_pipeline.stop(); resetAudio(); saveSettings();
    m_fusion.shutdown();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    const auto recording = m_pipeline.recorder().snapshot();
    if (!recording.active && !recording.finishing) { event->accept(); return; }
    event->ignore();
    if (m_closePending) return;
    if (recording.active && QMessageBox::question(this, QStringLiteral("录制尚未结束"),
        QStringLiteral("停止录制，等待已接收的数据写入完成后退出？不会丢弃当前写盘队列。"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    m_closePending = true;
    m_pipeline.stop(); m_receiving = false; resetAudio();
    centralWidget()->setEnabled(false);
    statusBar()->showMessage(QStringLiteral("正在完成录制文件和清单，完成后自动退出。请勿移除保存磁盘。"));
}

void MainWindow::navigate(Page page)
{
    m_pages->setCurrentIndex(static_cast<int>(page));
}

QWidget *MainWindow::buildMonitorPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    auto *controls = new QHBoxLayout;
    m_monitorEnabled = new QCheckBox(QStringLiteral("开启监听"));
    m_monitorEnabled->setObjectName("monitorEnabled");
    m_monitorChannel = integer(0, 127, 0); m_monitorGain = number(-60, 48, 0, 1); m_monitorGain->setSuffix(" dB");
    m_bufferMs = integer(50, 1000, 250); m_bufferMs->setSuffix(" ms");
    m_monitorSource = new NoWheelCombo;
    m_monitorSource->setObjectName("monitorSource"); m_monitorChannel->setObjectName("monitorChannel");
    m_monitorSource->addItems({QStringLiteral("原始通道"), QStringLiteral("增强 A"), QStringLiteral("抑制 B1"), QStringLiteral("抑制 B2"), QStringLiteral("维纳输出"), QStringLiteral("带通输出"), "LogMMSE", "AGC"});
    controls->addWidget(m_monitorEnabled); controls->addWidget(new QLabel("CH")); controls->addWidget(m_monitorChannel);
    controls->addWidget(m_monitorSource); controls->addWidget(new QLabel(QStringLiteral("监听增益"))); controls->addWidget(m_monitorGain);
    controls->addWidget(new QLabel(QStringLiteral("预缓冲"))); controls->addWidget(m_bufferMs); controls->addStretch();
    layout->addLayout(controls);
    m_monitorStatus = label(); m_monitorStatus->setObjectName("monitorStatus"); layout->addWidget(m_monitorStatus);
    auto *split = new QSplitter; split->setMinimumHeight(280);
    m_monitorView = new FieldView; split->addWidget(m_monitorView);
    auto *levels = new QWidget; auto *levelLayout = new QVBoxLayout(levels);
    levelLayout->setContentsMargins(0, 0, 0, 0); levelLayout->setSpacing(4);
    levelLayout->addWidget(label(QStringLiteral("通道电平 · RMS (dBFS) · 索引排列")));
    m_channelGrid = new ChannelGrid; levelLayout->addWidget(m_channelGrid, 1); split->addWidget(levels);
    split->setSizes({600, 640}); layout->addWidget(split, 3);
    auto *plots = new QHBoxLayout; m_wave = new WaveView; m_spectrum = new WaveView; m_spectrum->setSpectrum(true);
    plots->addWidget(m_wave); plots->addWidget(m_spectrum); layout->addLayout(plots, 1);
    connect(m_channelGrid, &ChannelGrid::channelSelected, m_monitorChannel, &QSpinBox::setValue);
    for (auto *spin : {m_monitorChannel, m_bufferMs}) connect(spin, &QSpinBox::valueChanged, this, &MainWindow::applySettings);
    connect(m_monitorGain, &QDoubleSpinBox::valueChanged, this, &MainWindow::applySettings);
    connect(m_monitorSource, &QComboBox::currentIndexChanged, this, &MainWindow::applySettings);
    connect(m_monitorEnabled, &QCheckBox::toggled, this, &MainWindow::applySettings);
    return page;
}

QWidget *MainWindow::buildLocalizationPage()
{
    auto *page = new QWidget; auto *layout = new QHBoxLayout(page);
    m_localView = new FieldView; layout->addWidget(m_localView, 3);
    auto *tabs = new QTabWidget; tabs->setObjectName("localizationTabs"); tabs->setMinimumWidth(375); tabs->setMaximumWidth(480); layout->addWidget(tabs, 1);
    QFormLayout *form; auto *live = formPage(&form);
    m_localEnabled = new QCheckBox(QStringLiteral("启用声源定位")); m_localEnabled->setObjectName("localizationEnabled"); form->addRow(m_localEnabled);
    m_threshold = number(0, 1, 0.35); m_threshold->setSingleStep(0.02);
    m_focus = number(0.5, 4, 1.5); m_focus->setSingleStep(0.1);
    m_soundSpeed = number(300, 380, 343, 1);
    m_gridColumns = integer(24, 160, 72); m_gridRows = integer(15, 100, 45);
    m_threshold->setToolTip(QStringLiteral("软件可信度门限；0–1，越低越宽松。不能绕过静音、自测和硬件故障保护。"));
    m_focus->setToolTip(QStringLiteral("仅调整热力图显示，数值越大峰值越聚焦；不改变定位结果或可信度。"));
    form->addRow(QStringLiteral("可信度阈值"), m_threshold); form->addRow(QStringLiteral("热力聚焦"), m_focus);
    form->addRow(QStringLiteral("声速 (m/s)"), m_soundSpeed); form->addRow(QStringLiteral("方位网格点数"), m_gridColumns); form->addRow(QStringLiteral("俯仰网格点数"), m_gridRows);
    m_maxSources = integer(1, 16, 4); m_maxSources->setObjectName("maximumSources");
    m_sourceSeparation = number(2, 90, 8, 1); m_sourceSeparation->setObjectName("sourceSeparation");
    m_sourceSeparation->setToolTip(QStringLiteral("SRP 多峰的球面角距离去重门限，不代表阵列可分辨的物理极限，也不是 YOLO 超分辨率参数。"));
    auto *sourceRow = new QWidget; auto *sourceFields = new QHBoxLayout(sourceRow); sourceFields->setContentsMargins(0, 0, 0, 0);
    m_maxSources->setPrefix(QStringLiteral("最多 ")); m_sourceSeparation->setSuffix(QStringLiteral(" °"));
    sourceFields->addWidget(m_maxSources); sourceFields->addWidget(m_sourceSeparation);
    form->addRow(QStringLiteral("目标数 / 间隔"), sourceRow);
    auto *maskRow = new QWidget; auto *maskFields = new QHBoxLayout(maskRow); maskFields->setContentsMargins(0, 0, 0, 0);
    m_maskStatus = label(); m_maskStatus->setObjectName("localizationMaskStatus"); maskFields->addWidget(m_maskStatus, 1);
    auto *maskButton = new QPushButton(QStringLiteral("通道屏蔽")); maskButton->setObjectName("channelMaskShortcut"); maskFields->addWidget(maskButton); form->addRow(maskRow);
    connect(maskButton, &QPushButton::clicked, this, [this] { navigate(Page::Configuration); m_configTabs->setCurrentWidget(m_channelMask); });
    m_location = label(QStringLiteral("等待有效实采数据和阵元坐标")); m_location->setObjectName("locationStatus"); form->addRow(m_location);
    form->addRow(label(QStringLiteral("SRP-PHAT：2048 样本 / 256 基线 / 最高 10 次/s。\n软件分析频带约 250–8000 Hz；不等同于硬件有效通带 20 Hz–16 kHz。\n需真实坐标、有效实采流；窄带、静音及质量故障不执行可信定位。")));
    tabs->addTab(live, QStringLiteral("实时定位"));
    QFormLayout *cal; auto *calibration = formPage(&cal);
    const QStringList labels {QStringLiteral("中心方位 (°)"), QStringLiteral("中心俯仰 (°)"), QStringLiteral("水平视场角 (°)"), QStringLiteral("垂直视场角 (°)"), QStringLiteral("滚转角 (°)")};
    const double defaults[] {0, 0, 90, 60, 0};
    for (int i = 0; i < 5; ++i) {
        m_cameraValues[i] = number(i == 2 || i == 3 ? 5 : -89, i == 2 || i == 3 ? 170 : 89, defaults[i]);
        m_cameraValues[i]->setToolTip(labels[i]);
        connect(m_cameraValues[i], &QDoubleSpinBox::valueChanged, this, &MainWindow::applySettings);
    }
    const QStringList prefixes {QStringLiteral("方位 "), QStringLiteral("俯仰 "), QStringLiteral("水平 "), QStringLiteral("垂直 ")};
    for (int pair = 0; pair < 2; ++pair) {
        auto *row = new QWidget; auto *fields = new QHBoxLayout(row); fields->setContentsMargins(0, 0, 0, 0);
        for (int i = pair * 2; i < pair * 2 + 2; ++i) { m_cameraValues[i]->setPrefix(prefixes[i]); fields->addWidget(m_cameraValues[i]); }
        cal->addRow(pair ? QStringLiteral("视场角 (°)") : QStringLiteral("中心方向 (°)"), row);
    }
    cal->addRow(labels[4], m_cameraValues[4]);
    auto *checkRow = new QWidget; auto *checks = new QHBoxLayout(checkRow); checks->setContentsMargins(0, 0, 0, 0);
    m_collectCalibration = new QCheckBox(QStringLiteral("采集四点")); m_collectCalibration->setToolTip(QStringLiteral("依次点击画面中的稳定声源，声源应处于四个分散位置。")); checks->addWidget(m_collectCalibration);
    m_showCalibration = new QCheckBox(QStringLiteral("显示校准点")); m_showCalibration->setChecked(true); checks->addWidget(m_showCalibration); cal->addRow(checkRow);
    m_calibrationTable = new QTableWidget(4, 4); m_calibrationTable->setObjectName("calibrationTable");
    m_calibrationTable->setHorizontalHeaderLabels({QStringLiteral("方位 (°)"), QStringLiteral("俯仰 (°)"), "X (0–1)", "Y (0–1)"});
    m_calibrationTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_calibrationTable->verticalHeader()->setDefaultSectionSize(32);
    m_calibrationTable->setMinimumHeight(175); m_calibrationTable->setMaximumHeight(190);
    m_calibrationTable->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_calibrationTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_calibrationTable->setEditTriggers(QAbstractItemView::NoEditTriggers); cal->addRow(m_calibrationTable);
    auto *actions = new QWidget; actions->setObjectName("calibrationActions"); auto *buttons = new QHBoxLayout(actions); buttons->setContentsMargins(0, 0, 0, 0);
    auto *undo = new QPushButton(QStringLiteral("撤销")), *fit = new QPushButton(QStringLiteral("拟合视角"));
    m_calibrationUndo = undo; m_calibrationFit = fit;
    undo->setObjectName("calibrationUndo"); fit->setObjectName("calibrationFit");
    fit->setToolTip(QStringLiteral("采集四个分散、稳定的声源点后可拟合视角"));
    buttons->addWidget(undo); buttons->addWidget(fit); cal->addRow(actions);
    auto *save = new QPushButton(QStringLiteral("导出校准")), *load = new QPushButton(QStringLiteral("载入校准"));
    auto *files = new QWidget; auto *fileButtons = new QHBoxLayout(files); fileButtons->setContentsMargins(0, 0, 0, 0);
    fileButtons->addWidget(save); fileButtons->addWidget(load); cal->addRow(files);
    tabs->addTab(calibration, QStringLiteral("视角校准"));
    m_sourceTable = new QTableWidget(0, 5); m_sourceTable->setObjectName("srpSourceTable");
    m_sourceTable->setHorizontalHeaderLabels({QStringLiteral("目标"), QStringLiteral("方位°"), QStringLiteral("俯仰°"), QStringLiteral("可信度"), QStringLiteral("判定")});
    m_sourceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); m_sourceTable->verticalHeader()->hide();
    m_sourceTable->setEditTriggers(QAbstractItemView::NoEditTriggers); m_sourceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_sourceTable->setToolTip(QStringLiteral("SRP 多峰候选按当前响应强弱排序，编号不是跨帧跟踪 ID；此处没有神经网络识别结果。"));
    tabs->addTab(m_sourceTable, QStringLiteral("SRP 目标"));
    tabs->addTab(buildFusionPanel(), QStringLiteral("融合识别"));
    connect(m_localView, &FieldView::pointClicked, this, &MainWindow::calibratePoint);
    connect(m_showCalibration, &QCheckBox::toggled, this, &MainWindow::updateCalibrationTable);
    connect(undo, &QPushButton::clicked, this, [this] { if (!m_calibration.isEmpty()) m_calibration.removeLast(); updateCalibrationTable(); });
    connect(fit, &QPushButton::clicked, this, [this] {
        const auto result = fitCameraView(m_calibration, m_processing.camera, m_localView->imageSize());
        if (!result.valid) { showError(result.error); return; }
        const double values[] {result.camera.centerAzimuthDegrees, result.camera.centerElevationDegrees,
            result.camera.horizontalFovDegrees, result.camera.verticalFovDegrees, result.camera.rollDegrees};
        m_loading = true; for (int i = 0; i < 5; ++i) m_cameraValues[i]->setValue(values[i]); m_loading = false;
        m_collectCalibration->setChecked(false); applySettings();
        statusBar()->showMessage(QStringLiteral("四点拟合 RMS = %1 像素").arg(result.rmsPixels, 0, 'f', 2));
    });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出校准"), "m128-camera.json", "JSON (*.json)");
        if (path.isEmpty()) return;
        const CameraProfile profile {m_processing.camera, m_calibration, m_localView->imageSize()};
        QSaveFile file(path); if (!file.open(QIODevice::WriteOnly)) { showError(file.errorString()); return; }
        const auto bytes = profile.json();
        if (file.write(bytes) != bytes.size() || !file.commit()) showError(file.errorString());
        else statusBar()->showMessage(QStringLiteral("已导出视角与 %1 个校准点：%2").arg(m_calibration.size()).arg(path));
    });
    connect(load, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("载入校准"), {}, "JSON (*.json)"); if (path.isEmpty()) return;
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) { showError(file.errorString()); return; }
        CameraProfile profile; QString error;
        if (!CameraProfile::load(file.readAll(), &profile, &error)) { showError(error + QStringLiteral("；原设置保持不变")); return; }
        const double values[] {profile.camera.centerAzimuthDegrees, profile.camera.centerElevationDegrees,
            profile.camera.horizontalFovDegrees, profile.camera.verticalFovDegrees, profile.camera.rollDegrees};
        m_loading = true; for (int i = 0; i < 5; ++i) m_cameraValues[i]->setValue(values[i]); m_loading = false;
        m_calibration = profile.samples; m_collectCalibration->setChecked(false); updateCalibrationTable(); applySettings();
        statusBar()->showMessage(QStringLiteral("已载入视角与 %1 个校准点").arg(m_calibration.size()));
        const auto currentSize = m_localView->imageSize();
        if (profile.referenceSize.isValid() && std::abs(double(profile.referenceSize.width()) / profile.referenceSize.height() - double(currentSize.width()) / currentSize.height()) > .01)
            showError(QStringLiteral("校准已载入，但参考图像宽高比与当前画面不同。请核对摄像头裁切或视频分辨率，避免叠加错位。"));
    });
    for (auto *spin : {m_threshold, m_soundSpeed, m_sourceSeparation}) connect(spin, &QDoubleSpinBox::valueChanged, this, &MainWindow::applySettings);
    connect(m_focus, &QDoubleSpinBox::valueChanged, this, [this] { m_seenResult = 0; saveSettings(); });
    for (auto *spin : {m_gridColumns, m_gridRows, m_maxSources}) connect(spin, &QSpinBox::valueChanged, this, &MainWindow::applySettings);
    connect(m_localEnabled, &QCheckBox::toggled, this, &MainWindow::applySettings);
    return page;
}

QWidget *MainWindow::buildFusionPanel()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    m_fusionEnabled = new QCheckBox(QStringLiteral("启用 YOLO 融合（实验）")); m_fusionEnabled->setObjectName("fusionEnabled");
    layout->addWidget(m_fusionEnabled);
    auto *form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); layout->addLayout(form);
    const auto fileRow = [this, form](const QString &title, const QString &filter, const char *name) {
        auto *row = new QWidget; auto *fields = new QHBoxLayout(row); fields->setContentsMargins(0, 0, 0, 0);
        auto *path = new QLineEdit; path->setClearButtonEnabled(true); path->setObjectName(name); path->setMinimumWidth(0);
        connect(path, &QLineEdit::editingFinished, this, &MainWindow::configureFusion);
        auto *browse = new QPushButton(QStringLiteral("选择")); fields->addWidget(path, 1); fields->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this, path, title, filter] {
            const QString selected = QFileDialog::getOpenFileName(this, title, path->text(), filter);
            if (selected.isEmpty()) return;
            path->setText(selected); path->setToolTip(selected);
            if (path == m_fusionModelPath && m_fusionMetadataPath->text().isEmpty()) {
                const QString paired = QFileInfo(selected).absolutePath() + "/fusion.json";
                if (QFileInfo::exists(paired)) { m_fusionMetadataPath->setText(paired); m_fusionMetadataPath->setToolTip(paired); }
            }
            configureFusion();
        });
        form->addRow(title, row); return path;
    };
    m_fusionModelPath = fileRow(QStringLiteral("模型"), "ONNX (*.onnx)", "fusionModelPath");
    m_fusionMetadataPath = fileRow(QStringLiteral("模型描述"), "JSON (*.json)", "fusionMetadataPath");
    m_fusionRuntimePath = fileRow(QStringLiteral("运行库"), "Runtime (*.dll *.so *.dylib)", "fusionRuntimePath");
    m_fusionRuntimePath->setPlaceholderText(QStringLiteral("留空使用程序目录运行库"));
    m_fusionConfidence = number(.01, .99, .15); m_fusionActive = number(.01, .99, .5);
    m_fusionConfidence->setObjectName("fusionConfidence"); m_fusionActive->setObjectName("fusionActiveThreshold");
    m_fusionConfidence->setToolTip(QStringLiteral("目标置信度 = objectness × class；非 SRP 可信度。"));
    m_fusionActive->setToolTip(QStringLiteral("网络发声判别概率阈值，用于排除可见但静音的候选。"));
    auto *thresholds = new QWidget; auto *thresholdFields = new QHBoxLayout(thresholds); thresholdFields->setContentsMargins(0, 0, 0, 0);
    m_fusionConfidence->setPrefix(QStringLiteral("目标 ")); m_fusionActive->setPrefix(QStringLiteral("发声 "));
    thresholdFields->addWidget(m_fusionConfidence); thresholdFields->addWidget(m_fusionActive); form->addRow(QStringLiteral("概率阈值"), thresholds);
    auto *syncRow = new QWidget; auto *syncFields = new QHBoxLayout(syncRow); syncFields->setContentsMargins(0, 0, 0, 0);
    m_fusionSkew = integer(10, 200, 100); m_fusionSkew->setObjectName("fusionMaximumSkew"); m_fusionSkew->setSuffix(" ms");
    auto *defaults = new QPushButton(QStringLiteral("模型建议")); syncFields->addWidget(m_fusionSkew); syncFields->addWidget(defaults);
    defaults->setToolTip(QStringLiteral("应用模型概率阈值及训练用 SRP 网格/声速/目标数/间隔；不改变阵列坐标或相机校准。"));
    form->addRow(QStringLiteral("最大时差"), syncRow);
    connect(defaults, &QPushButton::clicked, this, [this] {
        QFile file(m_fusionMetadataPath->text());
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) { showError(QStringLiteral("无法读取模型描述")); return; }
        const auto metadata = QJsonDocument::fromJson(file.readAll()).object();
        QString error; if (!FusionModel::validateMetadata(metadata, &error)) { showError(error); return; }
        const double confidence = metadata["confidence_threshold"].toDouble(-1), active = metadata["active_threshold"].toDouble(-1);
        if (confidence < .01 || confidence > .99 || active < .01 || active > .99) { showError(QStringLiteral("模型建议阈值无效")); return; }
        const auto srp = metadata["training_srp_settings"].toObject();
        if (metadata["training_srp_grid"].toArray() != QJsonArray {72, 45} || srp["max_sources"].toInt(-1) != 4
            || srp["minimum_source_separation_deg"].toDouble(-1) != 4 || srp["sound_speed_m_s"].toDouble(-1) != 343
            || srp["window_samples"].toInt(-1) != 2048) { showError(QStringLiteral("模型 SRP 建议规格不受支持")); return; }
        const QSignalBlocker first(m_fusionConfidence), second(m_fusionActive);
        m_fusionConfidence->setValue(confidence); m_fusionActive->setValue(active);
        m_loading = true;
        m_gridColumns->setValue(72); m_gridRows->setValue(45); m_soundSpeed->setValue(343);
        m_maxSources->setValue(4); m_sourceSeparation->setValue(4);
        m_loading = false; applySettings(); configureFusion();
    });
    auto *note = label(QStringLiteral("仅仿真域验证，不等同于真实人物/设备识别。青色 Y 标记为神经源点；S 标记为 SRP。推理显示配对帧，主机时间近似同步，最大保留 500 ms。"));
    note->setObjectName("fusionDomainNotice"); layout->addWidget(note);
    m_fusionStatus = label(QStringLiteral("未启用；缺少模型时 SRP 仍可独立使用。")); m_fusionStatus->setObjectName("fusionStatus"); layout->addWidget(m_fusionStatus);
    m_fusionStatus->setMaximumHeight(72); // Preserve the table when a runtime error contains a long path.
    m_fusionTable = new QTableWidget(0, 5); m_fusionTable->setObjectName("fusionSourceTable");
    m_fusionTable->setHorizontalHeaderLabels({QStringLiteral("目标"), QStringLiteral("方位°"), QStringLiteral("俯仰°"), QStringLiteral("置信度"), QStringLiteral("发声概率")});
    m_fusionTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); m_fusionTable->verticalHeader()->hide();
    m_fusionTable->setEditTriggers(QAbstractItemView::NoEditTriggers); m_fusionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fusionTable->setMinimumHeight(100); layout->addWidget(m_fusionTable, 1);
    connect(m_fusionEnabled, &QCheckBox::toggled, this, &MainWindow::configureFusion);
    for (auto *spin : {m_fusionConfidence, m_fusionActive}) connect(spin, &QDoubleSpinBox::valueChanged, this, &MainWindow::configureFusion);
    connect(m_fusionSkew, &QSpinBox::valueChanged, this, &MainWindow::configureFusion);
    return page;
}

QWidget *MainWindow::buildDenoisePage()
{
    auto *page = new QWidget; auto *layout = new QHBoxLayout(page);
    m_noiseView = new FieldView; m_noiseView->setObjectName("directionView"); layout->addWidget(m_noiseView, 3);
    m_noiseView->setAccessibleName(QStringLiteral("降噪方向控制"));
    m_noiseView->setToolTip(QStringLiteral("拖动标记选择并调整 A/B1/B2；点击空白移动当前目标。方向键微调 0.1°，Shift + 方向键调整 1°。"));
    auto *tabs = new QTabWidget; tabs->setMinimumWidth(360); tabs->setMaximumWidth(480); layout->addWidget(tabs, 1);
    QFormLayout *form; auto *flow = formPage(&form);
    m_denoiseEnabled = new QCheckBox(QStringLiteral("启用定向降噪")); m_denoiseEnabled->setObjectName("denoiseEnabled"); form->addRow(m_denoiseEnabled);
    m_denoiseStatus = label(); m_denoiseStatus->setObjectName("denoiseStatus"); form->addRow(m_denoiseStatus);
    m_dual = new QCheckBox(QStringLiteral("双抑制参考 B1 + B2")); m_dual->setChecked(true); form->addRow(m_dual);
    m_adaptive = new QCheckBox(QStringLiteral("自适应维纳（默认固定参数）")); form->addRow(m_adaptive);
    m_b1 = number(0, 100, 5); m_b2 = number(0, 100, 5);
    form->addRow(QStringLiteral("B1 抑制权重"), m_b1); form->addRow(QStringLiteral("B2 抑制权重"), m_b2);
    m_bandpass = new QCheckBox(QStringLiteral("带通")); m_logmmse = new QCheckBox("LogMMSE"); m_agc = new QCheckBox("AGC");
    form->addRow(m_bandpass); m_lowHz = number(20, 15000, 100, 0); m_highHz = number(40, 16000, 8000, 0);
    m_lowHz->setObjectName("bandpassLow"); m_highHz->setObjectName("bandpassHigh"); m_bandpass->setObjectName("bandpassEnabled");
    m_lowHz->setToolTip(QStringLiteral("下限必须小于上限；修改边界时会同步约束另一端，避免显示启用却实际旁路。"));
    m_highHz->setToolTip(m_lowHz->toolTip());
    form->addRow(QStringLiteral("下限频率 (Hz)"), m_lowHz); form->addRow(QStringLiteral("上限频率 (Hz)"), m_highHz);
    form->addRow(m_logmmse); form->addRow(m_agc);
    m_order = new QListWidget; m_order->setObjectName("postprocessingOrder"); m_order->setMaximumHeight(105); m_order->setDragDropMode(QAbstractItemView::InternalMove);
    m_order->setToolTip(QStringLiteral("维纳抑制固定在首位。拖动后处理节点，或选中后按 Alt + ↑/↓ 调整顺序。旁路节点仍保留位置。"));
    const QStringList names {QStringLiteral("带通"), "LogMMSE", "AGC"};
    for (int i = 0; i < 3; ++i) { auto *item = new QListWidgetItem(names[i], m_order); item->setData(Qt::UserRole, i); }
    form->addRow(QStringLiteral("后处理顺序"), m_order);
    for (int offset : {-1, 1}) {
        auto *shortcut = new QShortcut(QKeySequence(offset < 0 ? "Alt+Up" : "Alt+Down"), m_order);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, offset] {
            const int row = m_order->currentRow(), next = row + offset;
            if (row < 0 || next < 0 || next >= m_order->count()) return;
            auto *item = m_order->takeItem(row); m_order->insertItem(next, item); m_order->setCurrentRow(next); applySettings();
        });
    }
    m_logmmse->setToolTip(QStringLiteral("FFT512 / 重叠 50% / 最小增益 −20 dB"));
    m_agc->setToolTip(QStringLiteral("目标 −18 dBFS / 增益 −20～+30 dB / 启动 20 ms / 释放 300 ms / 限幅 −1 dBFS"));
    tabs->addTab(flow, QStringLiteral("处理参数"));
    QFormLayout *direction; auto *directions = formPage(&direction);
    m_activeTarget = new NoWheelCombo; m_activeTarget->setObjectName("activeDirection"); m_activeTarget->addItems({QStringLiteral("增强 A"), QStringLiteral("抑制 B1"), QStringLiteral("抑制 B2")}); direction->addRow(QStringLiteral("当前目标"), m_activeTarget);
    for (int i = 0; i < 6; ++i) {
        m_directionValues[i] = number(-89, 89, i == 2 ? -30 : i == 4 ? 30 : 0);
        direction->addRow(QStringLiteral("%1 %2 °").arg(QStringList{"A", "B1", "B2"}[i / 2], i % 2 ? QStringLiteral("俯仰") : QStringLiteral("方位")), m_directionValues[i]);
        connect(m_directionValues[i], &QDoubleSpinBox::valueChanged, this, &MainWindow::applySettings);
    }
    direction->addRow(label(QStringLiteral("与定位页共用 USB 摄像头和视角校准；使用真实阵元坐标、未量化 double 角度。完全在 PC 运算，不发送 FPGA 切换或方向命令。")));
    m_quickRecord = new QPushButton(QStringLiteral("开始录制")); m_quickRecord->setObjectName("quickRecordButton"); m_quickRecord->setToolTip(QStringLiteral("使用“音频录制”页设定的来源、音轨和保存目录。")); direction->addRow(m_quickRecord);
    auto *recordSettings = new QPushButton(QStringLiteral("录制设置")); direction->addRow(recordSettings);
    connect(recordSettings, &QPushButton::clicked, this, [this] { navigate(Page::Recording); });
    auto *monitor = new QPushButton(QStringLiteral("打开监听设置")); direction->addRow(monitor);
    connect(monitor, &QPushButton::clicked, this, [this] { navigate(Page::Monitor); });
    connect(m_quickRecord, &QPushButton::clicked, this, &MainWindow::toggleRecording);
    tabs->addTab(directions, QStringLiteral("方向控制"));
    connect(m_activeTarget, &QComboBox::currentIndexChanged, this, [this] { m_noiseView->setTargets(m_processing.directions, true, m_activeTarget->currentIndex()); });
    connect(m_noiseView, &FieldView::targetMoved, this, [this](int target, double az, double el, bool final) {
        const QSignalBlocker selectionBlocker(m_activeTarget); m_activeTarget->setCurrentIndex(target);
        m_loading = true; m_directionValues[target * 2]->setValue(az); m_directionValues[target * 2 + 1]->setValue(el); m_loading = false;
        m_skipSettingsSave = !final; applySettings(); m_skipSettingsSave = false;
    });
    for (auto *check : {m_denoiseEnabled, m_dual, m_adaptive, m_bandpass, m_logmmse, m_agc}) connect(check, &QCheckBox::toggled, this, &MainWindow::applySettings);
    for (auto *spin : {m_b1, m_b2}) connect(spin, &QDoubleSpinBox::valueChanged, this, &MainWindow::applySettings);
    connect(m_lowHz, &QDoubleSpinBox::valueChanged, this, [this] {
        if (m_loading) return;
        m_loading = true; if (m_highHz->value() <= m_lowHz->value()) m_highHz->setValue(m_lowHz->value() + 1); m_loading = false;
        applySettings();
    });
    connect(m_highHz, &QDoubleSpinBox::valueChanged, this, [this] {
        if (m_loading) return;
        m_loading = true; if (m_lowHz->value() >= m_highHz->value()) m_lowHz->setValue(m_highHz->value() - 1); m_loading = false;
        applySettings();
    });
    connect(m_order->model(), &QAbstractItemModel::rowsMoved, this, &MainWindow::applySettings);
    return page;
}

QWidget *MainWindow::buildRecordingPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    auto *pathRow = new QHBoxLayout; m_recordDirectory = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    auto *browse = new QPushButton(QStringLiteral("保存目录…")); m_recordBrowse = browse;
    m_recordDirectory->setObjectName("recordDirectory"); pathRow->addWidget(browse); pathRow->addWidget(m_recordDirectory); layout->addLayout(pathRow);
    auto *row = new QHBoxLayout; m_recordSource = new NoWheelCombo; m_recordSource->addItems({QStringLiteral("原始通道"), QStringLiteral("处理节点")});
    m_recordLayout = new NoWheelCombo; m_recordLayout->addItems({QStringLiteral("独立音轨 WAV"), QStringLiteral("多通道 WAV")});
    m_recordLayout->setToolTip(QStringLiteral("独立音轨：每路单声道文件，可直接播放。多通道：所选音轨交织为一个文件。"));
    auto *all = new QPushButton(QStringLiteral("全选")), *none = new QPushButton(QStringLiteral("清空"));
    m_recordAll = all; m_recordNone = none; m_recordSource->setObjectName("recordSource");
    row->addWidget(new QLabel(QStringLiteral("录制来源"))); row->addWidget(m_recordSource); row->addWidget(new QLabel(QStringLiteral("文件布局"))); row->addWidget(m_recordLayout); row->addWidget(all); row->addWidget(none); row->addStretch(); layout->addLayout(row);
    m_recordTracks = new QListWidget; m_recordTracks->setViewMode(QListView::IconMode); m_recordTracks->setResizeMode(QListView::Adjust);
    m_recordTracks->setObjectName("recordTracks"); m_recordTracks->setSelectionMode(QAbstractItemView::SingleSelection);
    m_recordTracks->setGridSize(QSize(105, 34)); m_recordTracks->setMovement(QListView::Static); layout->addWidget(m_recordTracks, 1);
    populateRecordingTracks();
    connect(m_recordSource, &QComboBox::currentIndexChanged, this, &MainWindow::populateRecordingTracks);
    m_recordSummary = label(); m_recordSummary->setObjectName("recordSummary"); layout->addWidget(m_recordSummary);
    auto selectTracks = [this](Qt::CheckState state) {
        if (!m_recordTracks->isEnabled()) return;
        const QSignalBlocker blocker(m_recordTracks);
        for (int i = 0; i < m_recordTracks->count(); ++i) m_recordTracks->item(i)->setCheckState(state);
        saveSettings();
    };
    connect(all, &QPushButton::clicked, this, [selectTracks] { selectTracks(Qt::Checked); });
    connect(none, &QPushButton::clicked, this, [selectTracks] { selectTracks(Qt::Unchecked); });
    connect(browse, &QPushButton::clicked, this, [this] { if (!m_recordDirectory->isEnabled()) return; const QString path = QFileDialog::getExistingDirectory(this); if (!path.isEmpty()) { m_recordDirectory->setText(path); saveSettings(); } });
    connect(m_recordTracks, &QListWidget::itemChanged, this, &MainWindow::saveSettings);
    connect(m_recordDirectory, &QLineEdit::editingFinished, this, &MainWindow::saveSettings);
    connect(m_recordLayout, &QComboBox::currentIndexChanged, this, &MainWindow::saveSettings);
    layout->addWidget(label(QStringLiteral("原始录制保留 24 位 PCM，不应用监听增益。每次录制独立目录，包含 WAV、原始 UDP 证据 packets.m128 和 manifest.json。\n丢帧、乱序、会话/模式变化会分段，绝不静默补零。默认每文件 256 MiB 分段。自测数据可录制，但必须按清单标记为数字自测，不能作为真实声学数据。")));
    m_recordButton = new QPushButton(QStringLiteral("开始录制")); m_recordButton->setObjectName("recordButton"); layout->addWidget(m_recordButton);
    m_recordStatus = label(QStringLiteral("未录制")); layout->addWidget(m_recordStatus);
    connect(m_recordButton, &QPushButton::clicked, this, &MainWindow::toggleRecording);
    return page;
}

void MainWindow::populateRecordingTracks()
{
    if (!m_loading && m_recordTracks->count()) {
        auto &selection = m_trackSelections[m_trackSource]; selection.clear();
        for (int i = 0; i < m_recordTracks->count(); ++i) if (m_recordTracks->item(i)->checkState() == Qt::Checked) selection.append(i);
    }
    m_trackSource = std::clamp(m_recordSource->currentIndex(), 0, 1);
    const QSignalBlocker blocker(m_recordTracks);
    m_recordTracks->clear(); const bool raw = m_trackSource == 0;
    const QStringList names {QStringLiteral("维纳抑制"), QStringLiteral("带通输出"), "LogMMSE", "AGC"};
    for (int i = 0; i < (raw ? 128 : 4); ++i) {
        auto *item = new QListWidgetItem(raw ? QStringLiteral("CH%1").arg(i, 3, 10, QChar('0')) : names[i], m_recordTracks);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(m_trackSelections[m_trackSource].contains(i) ? Qt::Checked : Qt::Unchecked);
    }
    if (!m_loading) saveSettings();
}

QWidget *MainWindow::buildGeometryPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    m_configTabs = new QTabWidget; m_configTabs->setObjectName("configurationTabs"); layout->addWidget(m_configTabs);
    auto *interfaces = new QWidget; auto *interfaceLayout = new QHBoxLayout(interfaces);
    QFormLayout *network; auto *networkForm = formPage(&network);
    m_bind->setToolTip(QStringLiteral("0.0.0.0 表示监听所有 IPv4 网卡；指定地址必须已配置到本机。不会修改网卡 IP。"));
    m_source->setToolTip(QStringLiteral("仅接收此设备来源的数据报；修改此项不改变 FPGA 的 IP。"));
    m_port->setToolTip(QStringLiteral("主机 UDP 监听端口，默认 5000"));
    m_sourcePort->setToolTip(QStringLiteral("允许的设备 UDP 源端口，默认 5000"));
    network->addRow(QStringLiteral("监听地址"), m_bind); network->addRow(QStringLiteral("监听端口"), m_port);
    network->addRow(QStringLiteral("设备地址"), m_source); network->addRow(QStringLiteral("设备端口"), m_sourcePort);
    network->addRow(label(QStringLiteral("接收中锁定网络参数。上述参数仅配置主机监听与来源过滤，不向 FPGA 写入网络设置。")));
    network->addRow(label(QStringLiteral("推荐网卡：192.168.10.1/24\n设备默认：192.168.10.2:5000\n链路要求：1000M 全双工，ARP 就绪后自动采集。")));
    interfaceLayout->addWidget(group(QStringLiteral("UDP 接收"), networkForm), 1);
    QFormLayout *auxiliary; auto *auxiliaryForm = formPage(&auxiliary);
    auxiliary->addRow(QStringLiteral("视频接口"), label(QStringLiteral("本机 USB 摄像头，在顶部选择并打开")));
    auxiliary->addRow(QStringLiteral("串口协议"), label(QStringLiteral("SYS3 / I2S1 / GAIN1 · 独立收发")));
    auxiliary->addRow(QStringLiteral("默认串口"), label(QStringLiteral("COM3 / 115200 8N1 / 无流控")));
    auxiliary->addRow(QStringLiteral("UART 电平"), label(QStringLiteral("3.3 V，与 1.8 V MEMS 接口不同")));
    auxiliary->addRow(label(QStringLiteral("连接后识别 SYS3；在 UART 面板手动查询板端监听与奇偶增益。\ns/h 读取状态，r 复位，t 切换自测，c 清时钟历史；录制中禁止复位、切换和增益配置。")));
    auto *serialSettings = new QPushButton(QStringLiteral("打开串口面板")); serialSettings->setObjectName("serialSettingsButton"); auxiliary->addRow(serialSettings);
    connect(serialSettings, &QPushButton::clicked, this, [this] { navigate(Page::Diagnostics); m_debugTabs->setCurrentWidget(m_serial); });
    interfaceLayout->addWidget(group(QStringLiteral("辅助接口"), auxiliaryForm), 1);
    m_configTabs->addTab(interfaces, QStringLiteral("接口设置"));

    auto *geometry = new QWidget; auto *geometryLayout = new QVBoxLayout(geometry);
    auto *actions = new QHBoxLayout;
    auto *load = new QPushButton(QStringLiteral("导入坐标")); actions->addWidget(load);
    connect(load, &QPushButton::clicked, this, &MainWindow::loadGeometry);
    auto *formatButton = new QPushButton(QStringLiteral("格式说明")); formatButton->setCheckable(true); actions->addWidget(formatButton); actions->addStretch();
    actions->addWidget(label(QStringLiteral("坐标单位 m  |  +X 右 / +Y 上 / +Z 前"))); geometryLayout->addLayout(actions);
    geometryLayout->addWidget(label(QStringLiteral("坐标来自实际机械/布线表；下面的接线映射为 README 参考值，不自动推测阵元位置或 CLK 负载。")));
    auto *format = new QPlainTextEdit; format->setReadOnly(true); format->setMaximumHeight(140); format->hide();
    format->setPlainText(QStringLiteral("{\"format\":\"micarray128-geometry-v1\",\"name\":\"机械表版本\",\"units\":\"m\",\"microphones\":[{\"channel\":0,\"x\":0.012,\"y\":0.034,\"z\":0.0}, …]}\n需完整 128 项，CH0–127 各一次。上例仅示意，不能用作实际阵列。\n检查单位、数量、唯一通道、有限范围、重复坐标与共线退化；导入失败保留旧设置。\nFPGA 焊盘列不是连接器针号。平面阵列不能物理区分前后镜像。"));
    connect(formatButton, &QPushButton::toggled, format, &QWidget::setVisible); geometryLayout->addWidget(format);
    m_geometryTable = new QTableWidget(128, 7); m_geometryTable->setObjectName("geometryTable");
    m_geometryTable->setHorizontalHeaderLabels({"CH", "DATA / LR", QStringLiteral("接口 / Bank"), QStringLiteral("FPGA 焊盘"), "X (m)", "Y (m)", "Z (m)"});
    m_geometryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_geometryTable->verticalHeader()->hide(); m_geometryTable->verticalHeader()->setDefaultSectionSize(28);
    m_geometryTable->setEditTriggers(QAbstractItemView::NoEditTriggers); m_geometryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_geometryTable->setAlternatingRowColors(true);
    for (int r = 0; r < 128; ++r) for (int c = 0; c < 7; ++c) m_geometryTable->setItem(r, c, new QTableWidgetItem);
    geometryLayout->addWidget(m_geometryTable, 1);
    m_configTabs->addTab(geometry, QStringLiteral("阵列几何"));

    m_channelMask = new ChannelMaskWidget;
    m_configTabs->addTab(m_channelMask, QStringLiteral("通道屏蔽"));
    connect(m_channelMask, &ChannelMaskWidget::maskChanged, this, &MainWindow::applySettings);
    connect(m_channelMask, &ChannelMaskWidget::inputRejected, this, &MainWindow::showError);

    auto *specifications = new QWidget; auto *specLayout = new QVBoxLayout(specifications);
    auto *referenceNotice = label(QStringLiteral("参考规格 · README 2026-09-20\n不代表已识别当前位流；电压、实际 CLK 分组与声学性能需要现场确认。"));
    referenceNotice->setObjectName("referenceNotice"); specLayout->addWidget(referenceNotice);
    const auto rows = hardwareSpecificationRows(); auto *specTable = new QTableWidget(rows.size(), 3);
    specTable->setObjectName("hardwareSpecifications"); specTable->setHorizontalHeaderLabels({QStringLiteral("项目"), QStringLiteral("参考规格"), QStringLiteral("说明")});
    specTable->setEditTriggers(QAbstractItemView::NoEditTriggers); specTable->verticalHeader()->hide(); specTable->setAlternatingRowColors(true);
    specTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    specTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch); specTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    for (int r = 0; r < rows.size(); ++r) for (int c = 0; c < 3; ++c) { auto *item = new QTableWidgetItem(rows[r][c]); item->setToolTip(rows[r][c]); specTable->setItem(r, c, item); }
    specTable->resizeRowsToContents(); specLayout->addWidget(specTable, 1);
    m_configTabs->addTab(specifications, QStringLiteral("硬件规格"));
    return page;
}

void MainWindow::updateGeometryTable()
{
    const bool valid = m_processing.geometry.valid();
    m_geometryLabel->setText(valid ? QStringLiteral("阵列：已配置 128 通道") : QStringLiteral("阵列：未配置"));
    m_geometryLabel->setToolTip(valid ? m_processing.geometry.name : QStringLiteral("导入真实阵元坐标后才能定位与软件波束处理"));
    for (int ch = 0; ch < 128; ++ch) {
        QStringList values {QString::number(ch), QStringLiteral("%1 / %2").arg(ch / 2).arg(ch % 2 ? "VDD" : "GND"),
            dataConnector(ch) + " / " + dataBank(ch), dataPin(ch)};
        if (valid) {
            const auto &position = m_processing.geometry.microphones[ch];
            values << QString::number(position.xMeters, 'f', 5) << QString::number(position.yMeters, 'f', 5) << QString::number(position.zMeters, 'f', 5);
        } else { values << QStringLiteral("—") << QStringLiteral("—") << QStringLiteral("—"); }
        for (int c = 0; c < values.size(); ++c) m_geometryTable->item(ch, c)->setText(values[c]);
    }
}

QWidget *MainWindow::buildDebugPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    auto *toolbar = new QHBoxLayout;
    auto *debugTitle = label(QStringLiteral("UDP / UART 设备诊断")); debugTitle->setWordWrap(false);
    toolbar->addWidget(debugTitle); toolbar->addStretch();
    auto *report = new QPushButton(QStringLiteral("导出诊断报告")); toolbar->addWidget(report);
    connect(report, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出诊断"), "m128-diagnostics.json", "JSON (*.json)");
        if (!path.isEmpty()) saveDiagnosticReport(path);
    });
    layout->addLayout(toolbar);
    auto *tabs = new QTabWidget; m_debugTabs = tabs; tabs->setObjectName("debugTabs"); layout->addWidget(tabs, 1);
    auto *overview = new QWidget; auto *grid = new QGridLayout(overview);
    m_commDetails = label(); m_qualityDetails = label(); m_selftestDetails = label(); m_hardwareDetails = label();
    m_qualityDetails->setObjectName("signalQualityDetails");
    for (auto *l : {m_commDetails, m_qualityDetails, m_selftestDetails, m_hardwareDetails}) l->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    grid->addWidget(group(QStringLiteral("通信状态"), m_commDetails), 0, 0);
    grid->addWidget(group(QStringLiteral("信号质量"), m_qualityDetails), 0, 1);
    auto *selftest = new QWidget; auto *selfLayout = new QVBoxLayout(selftest);
    selfLayout->addWidget(m_selftestDetails); m_selftestProgress = new QProgressBar;
    m_selftestProgress->setRange(0, 48000); selfLayout->addWidget(m_selftestProgress);
    grid->addWidget(group(QStringLiteral("数字自测"), selftest), 1, 0);
    grid->addWidget(group(QStringLiteral("硬件状态"), m_hardwareDetails), 1, 1);
    grid->setRowStretch(0, 1); grid->setRowStretch(1, 1);
    grid->setColumnStretch(0, 1); grid->setColumnStretch(1, 1);
    tabs->addTab(overview, QStringLiteral("运行状态"));
    m_qualityTable = new QTableWidget(128, 10);
    m_qualityTable->setHorizontalHeaderLabels({"CH", "DATA/LR", QStringLiteral("接口/Bank"), "RMS dBFS", "AC dBFS", "DC / FS", QStringLiteral("削顶数"), QStringLiteral("零值数"), QStringLiteral("相邻不变"), QStringLiteral("自测错误")});
    m_qualityTable->verticalHeader()->hide();
    m_qualityTable->setObjectName("channelQualityTable");
    m_qualityTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_qualityTable->setEditTriggers(QAbstractItemView::NoEditTriggers); m_qualityTable->setAlternatingRowColors(true);
    for (int r = 0; r < 128; ++r) for (int c = 0; c < 10; ++c) m_qualityTable->setItem(r, c, new QTableWidgetItem);
    tabs->addTab(m_qualityTable, QStringLiteral("通道质量"));
    m_packetDetails = new QPlainTextEdit; m_packetDetails->setReadOnly(true);
    tabs->addTab(m_packetDetails, QStringLiteral("数据报文"));
    m_serial = new Sys3Panel(m_store.value("serial/port", "COM3").toString()); tabs->addTab(m_serial, QStringLiteral("UART 串口"));
    connect(m_serial, &Sys3Panel::aboutToReset, this, [this] {
        m_pipeline.stop(); m_receiving = false; m_connect->setText(QStringLiteral("开始接收")); resetAudio();
        statusBar()->showMessage(QStringLiteral("设备复位前已暂停 UDP；恢复后请手动开始接收，建立独立主机会话。"));
    });
    auto *guide = new QPlainTextEdit; guide->setReadOnly(true);
    guide->setPlainText(commissioningGuide());
    tabs->addTab(guide, QStringLiteral("联调指引"));
    m_log = new QPlainTextEdit; m_log->setReadOnly(true); m_log->setMaximumBlockCount(200); m_log->setMaximumHeight(90); layout->addWidget(m_log); m_log->hide();
    auto *logButton = new QPushButton(QStringLiteral("事件记录")); logButton->setCheckable(true); toolbar->addWidget(logButton);
    connect(logButton, &QPushButton::toggled, m_log, &QWidget::setVisible);
    return page;
}

void MainWindow::applySettings()
{
    if (m_loading) return;
    const auto previous = m_processing;
    m_processing.localization = m_localEnabled->isChecked(); m_processing.confidenceThreshold = m_threshold->value();
    m_processing.soundSpeed = m_soundSpeed->value(); m_processing.gridColumns = m_gridColumns->value(); m_processing.gridRows = m_gridRows->value();
    m_processing.disabledChannels = m_channelMask->mask();
    m_processing.maxSources = m_maxSources->value(); m_processing.minimumSourceSeparationDegrees = m_sourceSeparation->value();
    const int disabledCount = int(std::count(m_processing.disabledChannels.cbegin(), m_processing.disabledChannels.cend(), true));
    m_maskStatus->setText(QStringLiteral("启用 %1 / 128 路").arg(128 - disabledCount));
    m_maskStatus->setToolTip(QStringLiteral("已屏蔽：%1").arg(channelMaskText(m_processing.disabledChannels)));
    m_processing.camera = {m_cameraValues[0]->value(), m_cameraValues[1]->value(), m_cameraValues[2]->value(), m_cameraValues[3]->value(), m_cameraValues[4]->value()};
    m_processing.denoise = m_denoiseEnabled->isChecked(); m_processing.dual = m_dual->isChecked(); m_processing.adaptive = m_adaptive->isChecked();
    m_processing.coefficientB1 = m_b1->value(); m_processing.coefficientB2 = m_b2->value();
    m_processing.bandpass = m_bandpass->isChecked(); m_processing.lowHz = m_lowHz->value(); m_processing.highHz = m_highHz->value();
    if (m_processing.lowHz >= m_processing.highHz) {
        m_loading = true; m_highHz->setValue(m_processing.lowHz + 1); m_loading = false;
        m_processing.highHz = m_highHz->value();
    }
    m_processing.logmmse = m_logmmse->isChecked(); m_processing.agc = m_agc->isChecked();
    m_processing.postOrder.clear(); for (int i = 0; i < m_order->count(); ++i) m_processing.postOrder.append(m_order->item(i)->data(Qt::UserRole).toInt());
    const QStringList nodeNames {QStringLiteral("带通"), "LogMMSE", "AGC"};
    const bool nodeEnabled[] {m_processing.bandpass, m_processing.logmmse, m_processing.agc};
    for (int i = 0; i < m_order->count(); ++i) {
        auto *item = m_order->item(i); const int key = item->data(Qt::UserRole).toInt();
        item->setText(nodeNames[key] + (nodeEnabled[key] ? QStringLiteral(" · 启用") : QStringLiteral(" · 旁路")));
    }
    for (int i = 0; i < 3; ++i) m_processing.directions[i] = {m_directionValues[i * 2]->value(), m_directionValues[i * 2 + 1]->value()};
    m_processing.monitor = m_monitorEnabled->isChecked(); m_processing.monitorChannel = m_monitorChannel->value();
    m_processing.monitorSource = m_monitorSource->currentIndex(); m_processing.monitorGainDb = m_monitorGain->value(); m_processing.bufferMs = m_bufferMs->value();
    const bool directionsOnly = onlyDirectionChange(previous, m_processing);
    if (!directionsOnly) { ++m_processing.generation; m_seenResult = 0; m_recentDirections.clear(); resetFusion(); }
    for (auto *view : {m_monitorView, m_localView, m_noiseView}) view->setCameraView(m_processing.camera);
    m_noiseView->setTargets(m_processing.directions, true, m_activeTarget->currentIndex());
    if (!directionsOnly) m_localView->setResponse({}, m_focus->value());
    m_pipeline.configure(m_processing);
    if (!directionsOnly) resetAudio();
    if (!m_skipSettingsSave) saveSettings();
}

void MainWindow::loadSettings()
{
    m_bind->setText(m_store.value("network/bind", "0.0.0.0").toString()); m_source->setText(m_store.value("network/source", "192.168.10.2").toString());
    m_port->setValue(m_store.value("network/port", 5000).toInt()); m_sourcePort->setValue(m_store.value("network/sourcePort", 5000).toInt());
    const auto geometry = m_store.value("geometry").toByteArray(); QString error;
    if (!geometry.isEmpty() && Geometry::load(geometry, &m_processing.geometry, &error)) m_geometryLabel->setText(QStringLiteral("阵元坐标：%1 · 128 路").arg(m_processing.geometry.name));
    m_threshold->setValue(m_store.value("threshold", 0.35).toDouble()); m_focus->setValue(m_store.value("focus", 1.5).toDouble());
    m_soundSpeed->setValue(m_store.value("soundSpeed", 343).toDouble());
    m_gridColumns->setValue(m_store.value("grid/columns", 72).toInt()); m_gridRows->setValue(m_store.value("grid/rows", 45).toInt());
    m_maxSources->setValue(m_store.value("processing/maxSources", 4).toInt());
    m_sourceSeparation->setValue(m_store.value("processing/sourceSeparation", 8.0).toDouble());
    m_fusionModelPath->setText(m_store.value("fusion/model").toString());
    m_fusionMetadataPath->setText(m_store.value("fusion/metadata").toString());
    m_fusionRuntimePath->setText(m_store.value("fusion/runtime").toString());
    m_fusionConfidence->setValue(m_store.value("fusion/confidence", .15).toDouble());
    m_fusionActive->setValue(m_store.value("fusion/active", .5).toDouble());
    m_fusionSkew->setValue(m_store.value("fusion/skewMs", 100).toInt());
    ChannelMask mask {};
    if (!parseChannelMask(m_store.value("processing/disabledChannels").toString(), &mask, &error)) {
        mask.fill(true); // Corrupt exclusions must not silently re-enable faulty inputs.
        showError(QStringLiteral("已保存的屏蔽列表无效；暂时屏蔽全部通道。请在“通道屏蔽”页核对后恢复。"));
    }
    m_channelMask->setMask(mask);
    m_b1->setValue(m_store.value("dsp/b1", 5).toDouble()); m_b2->setValue(m_store.value("dsp/b2", 5).toDouble());
    m_dual->setChecked(m_store.value("dsp/dual", true).toBool()); m_adaptive->setChecked(m_store.value("dsp/adaptive", false).toBool());
    m_bandpass->setChecked(m_store.value("dsp/bandpass", false).toBool()); m_logmmse->setChecked(m_store.value("dsp/logmmse", false).toBool()); m_agc->setChecked(m_store.value("dsp/agc", false).toBool());
    m_lowHz->setValue(m_store.value("dsp/lowHz", 100).toDouble()); m_highHz->setValue(m_store.value("dsp/highHz", 8000).toDouble());
    auto order = m_store.value("dsp/order").toList();
    if (order.size() == 3) {
        QVector<int> keys; for (const auto &value : order) keys.append(value.toInt());
        auto sorted = keys; std::sort(sorted.begin(), sorted.end());
        if (sorted == QVector<int>{0, 1, 2}) {
            m_order->clear(); const QStringList names {QStringLiteral("带通"), "LogMMSE", "AGC"};
            for (int key : keys) { auto *item = new QListWidgetItem(names[key], m_order); item->setData(Qt::UserRole, key); }
        }
    }
    m_monitorChannel->setValue(m_store.value("monitor/channel", 0).toInt()); m_bufferMs->setValue(m_store.value("monitor/buffer", 250).toInt());
    m_monitorGain->setValue(m_store.value("monitor/gain", 0).toDouble());
    for (int i = 0; i < 5; ++i) m_cameraValues[i]->setValue(m_store.value(QString("camera/%1").arg(i), m_cameraValues[i]->value()).toDouble());
    for (int i = 0; i < 6; ++i) m_directionValues[i]->setValue(m_store.value(QString("direction/%1").arg(i), m_directionValues[i]->value()).toDouble());
    m_recordDirectory->setText(m_store.value("record/directory", m_recordDirectory->text()).toString());
    m_recordLayout->setCurrentIndex(m_store.value("record/layout", 0).toInt());
    for (int source = 0; source < 2; ++source) {
        const QString key = QString("record/tracks%1").arg(source);
        if (!m_store.contains(key)) continue;
        QVector<int> tracks; bool valid = true;
        for (const auto &value : m_store.value(key).toList()) {
            bool ok = false; const int track = value.toInt(&ok);
            valid &= ok && track >= 0 && track < (source == 0 ? 128 : 4) && !tracks.contains(track);
            tracks.append(track);
        }
        if (valid) m_trackSelections[source] = tracks;
    }
    m_recordSource->setCurrentIndex(std::clamp(m_store.value("record/source", 0).toInt(), 0, 1));
    populateRecordingTracks();
}

void MainWindow::saveSettings()
{
    if (m_serial) m_store.setValue("serial/port", m_serial->port());
    if (m_loading) return;
    m_store.setValue("network/bind", m_bind->text()); m_store.setValue("network/source", m_source->text());
    m_store.setValue("network/port", m_port->value()); m_store.setValue("network/sourcePort", m_sourcePort->value());
    m_store.setValue("threshold", m_threshold->value()); m_store.setValue("focus", m_focus->value()); m_store.setValue("soundSpeed", m_soundSpeed->value());
    m_store.setValue("grid/columns", m_gridColumns->value()); m_store.setValue("grid/rows", m_gridRows->value());
    m_store.setValue("processing/disabledChannels", channelMaskText(m_channelMask->mask()));
    m_store.setValue("processing/maxSources", m_maxSources->value());
    m_store.setValue("processing/sourceSeparation", m_sourceSeparation->value());
    m_store.setValue("fusion/model", m_fusionModelPath->text()); m_store.setValue("fusion/metadata", m_fusionMetadataPath->text());
    m_store.setValue("fusion/runtime", m_fusionRuntimePath->text()); m_store.setValue("fusion/confidence", m_fusionConfidence->value());
    m_store.setValue("fusion/active", m_fusionActive->value()); m_store.setValue("fusion/skewMs", m_fusionSkew->value());
    m_store.setValue("dsp/b1", m_b1->value()); m_store.setValue("dsp/b2", m_b2->value());
    m_store.setValue("dsp/dual", m_dual->isChecked()); m_store.setValue("dsp/adaptive", m_adaptive->isChecked());
    m_store.setValue("dsp/bandpass", m_bandpass->isChecked()); m_store.setValue("dsp/logmmse", m_logmmse->isChecked()); m_store.setValue("dsp/agc", m_agc->isChecked());
    m_store.setValue("dsp/lowHz", m_lowHz->value()); m_store.setValue("dsp/highHz", m_highHz->value());
    QVariantList order; for (int i = 0; i < m_order->count(); ++i) order.append(m_order->item(i)->data(Qt::UserRole)); m_store.setValue("dsp/order", order);
    m_store.setValue("monitor/channel", m_monitorChannel->value()); m_store.setValue("monitor/buffer", m_bufferMs->value()); m_store.setValue("monitor/gain", m_monitorGain->value());
    for (int i = 0; i < 5; ++i) m_store.setValue(QString("camera/%1").arg(i), m_cameraValues[i]->value());
    for (int i = 0; i < 6; ++i) m_store.setValue(QString("direction/%1").arg(i), m_directionValues[i]->value());
    m_store.setValue("record/directory", m_recordDirectory->text()); m_store.setValue("record/layout", m_recordLayout->currentIndex());
    m_store.setValue("record/source", m_recordSource->currentIndex());
    m_trackSelections[m_trackSource].clear();
    for (int i = 0; i < m_recordTracks->count(); ++i) if (m_recordTracks->item(i)->checkState() == Qt::Checked) m_trackSelections[m_trackSource].append(i);
    for (int source = 0; source < 2; ++source) {
        QVariantList tracks; for (int track : m_trackSelections[source]) tracks.append(track);
        m_store.setValue(QString("record/tracks%1").arg(source), tracks);
    }
    if (m_processing.geometry.valid()) m_store.setValue("geometry", QJsonDocument(m_processing.geometry.json()).toJson(QJsonDocument::Compact));
}

void MainWindow::startReceiving()
{
    const QHostAddress bind(m_bind->text().trimmed()), source(m_source->text().trimmed());
    if (bind.protocol() != QAbstractSocket::IPv4Protocol || source.protocol() != QAbstractSocket::IPv4Protocol
        || source == QHostAddress(QHostAddress::AnyIPv4) || source == QHostAddress(QHostAddress::Broadcast)
        || bind.isMulticast() || source.isMulticast()) {
        showError(QStringLiteral("请输入有效 IPv4 地址：监听地址可为 0.0.0.0，设备地址必须为单播地址。"));
        navigate(Page::Configuration); m_configTabs->setCurrentIndex(0); return;
    }
    m_lastError.clear(); m_notice->hide();
    applySettings();
    m_pipeline.start({m_bind->text(), quint16(m_port->value()), m_source->text(), quint16(m_sourcePort->value())});
    m_receiving = true; m_connect->setText(QStringLiteral("停止接收"));
}

void MainWindow::openSerial(const QString &port)
{
    m_serial->openPort(port); navigate(Page::Diagnostics); m_debugTabs->setCurrentWidget(m_serial);
}

void MainWindow::toggleCamera()
{
    resetFusion();
    if (m_camera) { m_camera->stop(); m_captureSession.setCamera(nullptr); m_camera.reset(); m_cameraButton->setText(QStringLiteral("打开视频")); m_cameras->setEnabled(true); return; }
    const auto inputs = QMediaDevices::videoInputs();
    for (const auto &device : inputs) if (device.id() == m_cameras->currentData().toByteArray()) {
        m_camera = std::make_unique<QCamera>(device);
        connect(m_camera.get(), &QCamera::errorOccurred, this, [this](QCamera::Error, const QString &error) { resetFusion(); showError(QStringLiteral("USB 摄像头：") + error); });
        m_captureSession.setCamera(m_camera.get()); m_camera->start(); m_cameraButton->setText(QStringLiteral("关闭视频")); m_cameras->setEnabled(false); return;
    }
    showError(QStringLiteral("未发现所选 USB 摄像头，请检查连接和系统隐私权限"));
}

void MainWindow::toggleRecording()
{
    if (m_pipeline.recorder().snapshot().active) { m_pipeline.recorder().setDiagnostics(m_pipeline.snapshot().json()); m_pipeline.recorder().finish(); return; }
    if (m_pipeline.recorder().snapshot().finishing) return;
    const auto state = m_pipeline.snapshot();
    const QString block = recordingBlock(state);
    if (!block.isEmpty()) { showError(block); return; }
    RecordingSettings settings; settings.directory = m_recordDirectory->text(); settings.arraySource = m_recordSource->currentIndex() == 0;
    settings.separate = m_recordLayout->currentIndex() == 0;
    settings.tracks.clear();
    for (int i = 0; i < m_recordTracks->count(); ++i) if (m_recordTracks->item(i)->checkState() == Qt::Checked) settings.tracks.append(i);
    if (settings.tracks.isEmpty()) { showError(QStringLiteral("至少选择一个录制轨道")); return; }
    if (!settings.arraySource && (!m_processing.denoise || !m_processing.geometry.valid() || state.flags & 0x10 || state.acousticFaultFlags())) {
        showError(QStringLiteral("降噪录制需要真实坐标、启用 DSP 和无故障实采流；当前条件不满足")); return;
    }
    settings.metadata = {{"source_ip", m_source->text()}, {"source_port", m_sourcePort->value()},
        {"geometry", m_processing.geometry.json()}, {"dsp_generation", QString::number(m_processing.generation)},
        {"start_diagnostics", state.json()}, {"sound_speed", m_processing.soundSpeed}, {"adaptive_wiener", m_processing.adaptive},
        {"dual_reference", m_processing.dual}, {"b1_coefficient", m_processing.coefficientB1}, {"b2_coefficient", m_processing.coefficientB2}};
    QJsonArray disabled; for (int ch = 0; ch < 128; ++ch) if (m_processing.disabledChannels[ch]) disabled.append(ch);
    settings.metadata["disabled_processing_channels"] = disabled;
    QJsonArray directions; for (const auto &direction : m_processing.directions) directions.append(QJsonObject{{"azimuth", direction.azimuthDegrees}, {"elevation", direction.elevationDegrees}});
    settings.metadata["directions"] = directions;
    QJsonArray order; for (int node : m_processing.postOrder) order.append(node); settings.metadata["post_order"] = order;
    m_lastRecordError.clear();
    // Lock destructive UART commands synchronously, not at the next 100 ms UI tick.
    m_serial->setCaptureBusy(true);
    if (!m_pipeline.recorder().begin(settings)) showError(QStringLiteral("无法启动录制：检查轨道选择或等待上次写盘完成"));
    saveSettings();
}

QString MainWindow::recordingBlock(const PipelineSnapshot &state) const
{
    if (m_serial && m_serial->acquisitionChangePending()) return QStringLiteral("等待串口采集控制完成（复位、自测切换或增益配置）后再启动录制。");
    int selected = 0;
    for (int i = 0; i < m_recordTracks->count(); ++i) selected += m_recordTracks->item(i)->checkState() == Qt::Checked;
    if (!selected) return QStringLiteral("至少勾选一条音轨；空格键可切换当前音轨。");
    const QString directory = m_recordDirectory->text().trimmed();
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory)) return QStringLiteral("请选择录制保存目录（绝对路径）。");
    if (QFileInfo(directory).isFile()) return QStringLiteral("保存路径指向文件，请选择目录。");
    if (!state.listening || state.lastPacketAgeMs < 0 || state.lastPacketAgeMs >= 1500) return QStringLiteral("等待有效数据：先开始 UDP 接收，再启动录制。");
    if (m_recordSource->currentIndex() == 1) {
        if (!m_processing.geometry.valid()) return QStringLiteral("处理节点录制需要导入真实 128 阵元坐标。");
        if (!m_processing.denoise) return QStringLiteral("处理节点录制需要在“定向降噪”中启用处理。");
        if (state.flags & 0x10) return QStringLiteral("数字自测流：处理节点已隔离，可改录原始通道保存诊断证据。");
        if (state.acousticFaultFlags()) return state.acousticFaultReason();
    }
    return {};
}

void MainWindow::loadGeometry()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("真实阵元坐标"), {}, "JSON (*.json)"); if (path.isEmpty()) return;
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) { showError(file.errorString()); return; }
    QString error; Geometry geometry;
    if (!Geometry::load(file.readAll(), &geometry, &error)) { showError(error); return; }
    m_processing.geometry = geometry; updateGeometryTable(); applySettings();
}

void MainWindow::calibratePoint(const QPointF &point)
{
    if (!m_collectCalibration->isChecked()) return;
    if (m_calibration.size() >= 4) { showError(QStringLiteral("已有四点，请拟合或撤销")); return; }
    const auto state = m_pipeline.snapshot();
    const int trustedSources = int(std::count_if(state.localization.sources.cbegin(), state.localization.sources.cend(), [](const AcousticSource &source) { return source.trusted; }));
    if (trustedSources > 1) { showError(QStringLiteral("校准时请仅保留一个稳定发声目标，避免混用不同声源的方向。")); return; }
    if (m_recentDirections.size() < 4 || !state.listening || !state.localization.trusted || state.lastPacketAgeMs < 0 || state.lastPacketAgeMs > 300
        || state.localization.generation != m_processing.generation || state.localization.run != state.runs
        || state.frame < state.localization.lastFrame || state.frame - state.localization.lastFrame > SampleRate / 2) {
        showError(QStringLiteral("需要至少四次稳定、可信且新鲜的实时定位结果；请等待声源稳定后点击")); return;
    }
    double az = 0, el = 0, confidence = 0;
    for (const auto &d : m_recentDirections) { az += d.direction.azimuthDegrees; el += d.direction.elevationDegrees; confidence += d.confidence; }
    az /= m_recentDirections.size(); el /= m_recentDirections.size(); confidence /= m_recentDirections.size();
    double deviation = 0;
    for (const auto &d : m_recentDirections) deviation = std::max(deviation, std::hypot(d.direction.azimuthDegrees - az, d.direction.elevationDegrees - el));
    if (deviation > 3) { showError(QStringLiteral("声源方向波动超过 3°，请保持稳定")); return; }
    for (const auto &p : m_calibration) if (std::hypot(p.normalizedVideoPoint.x() - point.x(), p.normalizedVideoPoint.y() - point.y()) < 0.12) { showError(QStringLiteral("校准点过近，请分散布置")); return; }
    m_calibration.append({{az, el}, point, confidence, deviation}); updateCalibrationTable();
}

void MainWindow::updateCalibrationTable()
{
    m_calibrationUndo->setEnabled(!m_calibration.isEmpty()); m_calibrationFit->setEnabled(m_calibration.size() == 4);
    m_collectCalibration->setText(QStringLiteral("采集四点 (%1/4)").arg(m_calibration.size()));
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) {
        QString value = "—";
        if (row < m_calibration.size()) {
            const auto &p = m_calibration[row]; const double v[] {p.direction.azimuthDegrees, p.direction.elevationDegrees, p.normalizedVideoPoint.x(), p.normalizedVideoPoint.y()};
            value = QString::number(v[col], 'f', col < 2 ? 2 : 3);
        }
        m_calibrationTable->setItem(row, col, new QTableWidgetItem(value));
    }
    m_localView->setCalibrationPoints(m_showCalibration->isChecked() ? m_calibration : QVector<VideoCalibrationSample>{});
}

void MainWindow::resetAudio()
{
    if (m_audio) m_audio->stop(); m_audio.reset(); m_audioDevice = nullptr;
    m_audioBuffer.configure(SampleRate, m_bufferMs ? m_bufferMs->value() : 250);
    m_audioBuffer.clear();
}

void MainWindow::pumpAudio()
{
    if (m_audio && (m_audio->error() == QAudio::OpenError || m_audio->error() == QAudio::IOError || m_audio->error() == QAudio::FatalError)) {
        m_monitorEnabled->setChecked(false);
        showError(QStringLiteral("音频输出异常，监听已停止。请检查默认输出设备后重新开启；采集与录制仍继续。")); return;
    }
    const auto pcm = m_pipeline.takeMonitorPcm();
    if (m_processing.monitor) m_audioBuffer.append(pcm);
    if (m_processing.monitor && !m_audio && m_audioBuffer.readyToStart()) {
        QAudioFormat format; format.setSampleRate(SampleRate); format.setChannelCount(1); format.setSampleFormat(QAudioFormat::Int16);
        const auto device = QMediaDevices::defaultAudioOutput();
        if (device.isNull()) { m_monitorEnabled->setChecked(false); showError(QStringLiteral("未检测到音频输出设备，无法监听。请连接扬声器或耳机后重新开启。")); return; }
        if (!device.isFormatSupported(format)) { m_monitorEnabled->setChecked(false); showError(QStringLiteral("输出设备不支持 48 kHz 单声道 PCM16")); return; }
        m_audio = std::make_unique<QAudioSink>(device, format); m_audio->setBufferSize(48000); m_audioDevice = m_audio->start();
        if (!m_audioDevice) { m_monitorEnabled->setChecked(false); showError(QStringLiteral("无法启动音频输出，请检查设备占用与系统声音设置。")); return; }
    }
    if (m_audioDevice && m_audio) {
        const auto bytes = m_audioBuffer.peek(std::min<qsizetype>(m_audio->bytesFree(), m_audioBuffer.availableBytes()));
        if (!bytes.isEmpty()) {
            const qint64 written = m_audioDevice->write(bytes);
            if (written > 0) m_audioBuffer.discard(written);
            else if (written < 0) { m_monitorEnabled->setChecked(false); showError(QStringLiteral("音频输出写入失败，监听已停止；录制不受影响。")); }
        }
    }
}

void MainWindow::refresh()
{
    const auto s = m_pipeline.snapshot(); const auto recording = m_pipeline.recorder().snapshot();
    if (m_receiving && s.receiverFailed) {
        m_pipeline.stop(); m_receiving = false; resetAudio();
    }
    m_connect->setText(m_receiving ? QStringLiteral("停止接收") : QStringLiteral("开始接收"));
    m_connect->setEnabled(!recording.finishing);
    if (recording.active) m_pipeline.recorder().setDiagnostics(s.json());
    const QVector<QWidget *> networkWidgets {m_bind, m_port, m_source, m_sourcePort};
    for (auto *widget : networkWidgets) widget->setEnabled(!m_receiving);
    const bool hasData = !s.latestHeader.isEmpty();
    const bool active = s.listening && s.lastPacketAgeMs >= 0 && s.lastPacketAgeMs < 1500;
    const bool failedReceiver = s.receiverFailed;
    const QString stoppedStatus = !s.error.isEmpty() ? QStringLiteral("接收已停止 · 有历史告警") : s.received ? QStringLiteral("接收已停止") : QStringLiteral("接收未启动");
    m_health->setText(failedReceiver ? QStringLiteral("接收异常 / 已停止") : !s.listening ? stoppedStatus : !active ? QStringLiteral("等待数据 / 超时") : s.acousticFaultFlags() ? QStringLiteral("数据质量故障") : s.flags & 0x10 ? QStringLiteral("数字自测中")
        : s.latchedFlags & 1 ? QStringLiteral("实采接收中 · 设备曾饱和")
        : s.error.isEmpty() ? QStringLiteral("实采接收中") : QStringLiteral("实采接收中 · 有历史告警"));
    const QString level = failedReceiver || (active && s.acousticFaultFlags()) ? "fault" : !s.listening ? "neutral" : !active || (s.flags & 0x10) || (s.latchedFlags & 1) || !s.error.isEmpty() ? "warning" : "normal";
    if (m_health->property("level").toString() != level) {
        m_health->setProperty("level", level); m_health->style()->unpolish(m_health); m_health->style()->polish(m_health);
    }
    const auto age = [](qint64 ms) { return ms < 0 ? QStringLiteral("—") : QStringLiteral("%1 ms").arg(ms); };
    m_endpoint->setText(QStringLiteral("UDP %1:%2").arg(m_source->text()).arg(m_sourcePort->value()));
    m_endpoint->setToolTip(QStringLiteral("来源过滤 %1:%2 → 本机监听 %3:%4").arg(m_source->text()).arg(m_sourcePort->value()).arg(m_bind->text()).arg(m_port->value()));
    m_cameraButton->setEnabled(bool(m_camera) || !m_cameras->currentData().toByteArray().isEmpty());
    m_rates->setText(QStringLiteral("接收 %1 包/s  |  UDP 负载 %2 MB/s  |  数据时效 %3").arg(s.packetRate, 0, 'f', 0).arg(s.byteRate / 1e6, 0, 'f', 3).arg(age(s.lastPacketAgeMs)));
    m_channelGrid->setData(s, m_monitorChannel->value());
    m_monitorChannel->setEnabled(m_monitorSource->currentIndex() == 0);
    m_b2->setEnabled(m_dual->isChecked());
    m_lowHz->setEnabled(m_bandpass->isChecked()); m_highHz->setEnabled(m_bandpass->isChecked());
    QString acousticBlock = !m_processing.geometry.valid() ? QStringLiteral("先导入真实 128 阵元坐标")
        : std::count(m_processing.disabledChannels.cbegin(), m_processing.disabledChannels.cend(), false) < 16 ? QStringLiteral("屏蔽后启用通道不足 16 路")
        : !active ? QStringLiteral("等待有效 UDP 实采数据") : s.flags & 0x10 ? QStringLiteral("数字自测流已隔离")
        : s.acousticFaultReason();
    m_denoiseStatus->setText(!m_processing.denoise ? QStringLiteral("处理未启用 · 输入为原始 128 通道")
        : !acousticBlock.isEmpty() ? QStringLiteral("处理等待：") + acousticBlock : QStringLiteral("处理中 · A/B1/B2 波束 → 维纳 → 后处理"));
    QString monitorState = !m_processing.monitor ? QStringLiteral("监听关闭") : !active ? QStringLiteral("监听等待有效输入")
        : m_processing.monitorSource != 0 && (!m_processing.denoise || !acousticBlock.isEmpty()) ? QStringLiteral("处理监听等待：需启用降噪、真实坐标和无故障实采")
        : m_audio ? QStringLiteral("监听输出中") : QStringLiteral("监听预缓冲中");
    m_monitorStatus->setText(QStringLiteral("%1 · 来源 %2 · 波形/频谱为原始 CH%3；监听增益不改变录制数据。")
        .arg(monitorState, m_monitorSource->currentText()).arg(m_monitorChannel->value()));
    if (m_pages->currentIndex() == static_cast<int>(Page::Monitor)) { m_wave->setSamples(s.waveform, active); m_spectrum->setSamples(s.waveform, active); }
    m_location->setText(s.localization.heat.isEmpty() ? QStringLiteral("%1\n%2\n尚无候选方向").arg(s.acousticStatus, s.localization.reason) : QStringLiteral("%1\n%2\n方位 %3° / 俯仰 %4°\n可信度 %5 / 门限 %6\n有效通道 %7 · 基线 %8 · %9 ms")
        .arg(s.acousticStatus, s.localization.reason).arg(s.localization.direction.azimuthDegrees, 0, 'f', 2).arg(s.localization.direction.elevationDegrees, 0, 'f', 2)
        .arg(s.localization.confidence, 0, 'f', 3).arg(m_threshold->value(), 0, 'f', 2).arg(s.localization.validChannels).arg(s.localization.validPairs).arg(s.localization.elapsedMs, 0, 'f', 1));
    if (!s.localization.heat.isEmpty()) {
        const int count = int(std::count_if(s.localization.sources.cbegin(), s.localization.sources.cend(), [](const AcousticSource &source) { return source.trusted; }));
        m_location->setText(m_location->text() + QStringLiteral("\n可信目标 %1 · 详见“SRP 目标”").arg(count));
    }
    if (!m_processing.localization) m_location->setText(QStringLiteral("定位未启用\n导入阵列坐标并接收实采数据后，勾选上方开关。"));
    else if (!acousticBlock.isEmpty()) m_location->setText(QStringLiteral("定位等待：") + acousticBlock);
    if (s.runs != m_seenRun || s.localization.lastFrame != m_seenResult || !m_seenResult) {
        if (!m_fusionPaintedFrame) m_localView->setResponse(s.localization, m_focus->value());
        m_sourceTable->setRowCount(s.localization.sources.size());
        for (int i = 0; i < s.localization.sources.size(); ++i) {
            const auto &source = s.localization.sources[i];
            const QStringList values {QStringLiteral("S%1").arg(i + 1), QString::number(source.direction.azimuthDegrees, 'f', 2),
                QString::number(source.direction.elevationDegrees, 'f', 2), QString::number(source.confidence, 'f', 3),
                source.trusted ? QStringLiteral("可信") : QStringLiteral("候选")};
            for (int col = 0; col < values.size(); ++col) {
                auto *item = m_sourceTable->item(i, col);
                if (!item) { item = new QTableWidgetItem; m_sourceTable->setItem(i, col, item); }
                item->setText(values[col]);
                item->setToolTip(QStringLiteral("相对 SRP 响应 %1（非声功率），本帧候选编号 S%2").arg(source.relativePower, 0, 'f', 3).arg(i + 1));
            }
        }
        const int trustedSources = int(std::count_if(s.localization.sources.cbegin(), s.localization.sources.cend(), [](const AcousticSource &source) { return source.trusted; }));
        if (s.runs != m_seenRun || !s.localization.trusted || trustedSources > 1) m_recentDirections.clear();
        if (s.localization.trusted && trustedSources <= 1 && s.localization.lastFrame != m_seenResult) {
            m_recentDirections.append(s.localization); if (m_recentDirections.size() > 10) m_recentDirections.removeFirst();
        }
        m_seenRun = s.runs; m_seenResult = s.localization.lastFrame;
    }
    refreshFusion(s);
    const bool locked = recording.active || recording.finishing;
    m_serial->setCaptureBusy(locked);
    const QVector<QWidget *> recordingWidgets {m_recordSource, m_recordLayout, m_recordTracks, m_recordDirectory, m_recordBrowse, m_recordAll, m_recordNone};
    for (QWidget *widget : recordingWidgets) widget->setEnabled(!locked);
    m_recordButton->setText(recording.active ? QStringLiteral("停止录制") : recording.finishing ? QStringLiteral("正在完成写盘…") : QStringLiteral("开始录制"));
    const QString recordBlock = recordingBlock(s);
    const bool canRecord = !recording.finishing && (recording.active || recordBlock.isEmpty());
    m_recordButton->setEnabled(canRecord); m_quickRecord->setEnabled(canRecord);
    m_recordButton->setToolTip(recordBlock); m_quickRecord->setToolTip(recording.active ? QStringLiteral("停止并完成写盘") : recordBlock.isEmpty() ? QStringLiteral("按录制页当前设置启动") : recordBlock);
    m_quickRecord->setText(recording.active ? QStringLiteral("停止录制") : recording.finishing ? QStringLiteral("正在完成写盘…") : QStringLiteral("开始录制"));
    int trackCount = 0;
    for (int i = 0; i < m_recordTracks->count(); ++i) trackCount += m_recordTracks->item(i)->checkState() == Qt::Checked;
    m_recordSummary->setText(QStringLiteral("已选 %1 / %2 条音轨 · %3 · PCM24 / 48 kHz\n%4")
        .arg(trackCount).arg(m_recordTracks->count()).arg(m_recordLayout->currentText(), locked ? QStringLiteral("录制配置已锁定，停止并完成写盘后可修改。") : recordBlock.isEmpty() ? QStringLiteral("准备就绪，可开始录制。") : recordBlock));
    m_recordStatus->setText(QStringLiteral("%1\n写入 %2 / %3 WAV 帧 / %4 UDP 包 · 队列 %5 · 丢弃 %6\n%7")
        .arg(recording.error.isEmpty() ? (recording.active ? QStringLiteral("录制中") : recording.finishing ? QStringLiteral("正在完成写盘，请勿退出或移除磁盘") : recording.directory.isEmpty() ? QStringLiteral("未录制") : QStringLiteral("本次录制已完成；文件已保留")) : recording.error)
        .arg(mib(recording.bytes)).arg(recording.frames).arg(recording.packets).arg(mib(recording.queueBytes)).arg(recording.dropped).arg(recording.directory));
    if (m_pages->currentIndex() == static_cast<int>(Page::Diagnostics)) {
        m_commDetails->setText(QStringLiteral("接收 %1 · 有效 %2 · 格式拒绝 %3\n来源过滤 %4 · 主机队列丢包 %5\n队列 %6 / 8192 · socket %7 MiB\n包缺失 %8 · 重复 %9 · 乱序 %10\n帧缺失 %11 · 累计跳号 %12\n到达相邻不连续 %13 · 旧 epoch %14\n来源时效 %15 · 处理延迟 %16")
            .arg(s.received).arg(s.valid).arg(s.invalid).arg(s.rejectedSource).arg(s.queueDrops).arg(s.queued).arg(s.socketBuffer / 1048576)
            .arg(counter(s.packetSequence, "missing_observed_range"), counter(s.packetSequence, "duplicates"), counter(s.packetSequence, "out_of_order"),
                 counter(s.frameSequence, "missing_observed_range"), counter(s.frameSequence, "forward_gap_observations")).arg(s.discontinuities).arg(s.staleEpochPackets)
            .arg(age(s.lastSourceDatagramAgeMs), age(s.processingDelayMs)));
        const auto qualityFaults = s.acousticFaultFlags();
        const QString qualitySummary = qualityFaults & ~quint32(1) ? QStringLiteral("全局故障：需排查后重新接收")
            : qualityFaults ? QStringLiteral("未屏蔽通道削顶：屏蔽故障通道后自动恢复") : s.acousticStatus;
        m_qualityDetails->setText(!hasData ? QStringLiteral("尚未收到有效数据\n\n质量状态未知；未接收不等于静音或无故障。") : QStringLiteral("设备 flags 0x%1 · 会话锁存 0x%2\n声学阻断 0x%3 · 通道削顶仅判定未屏蔽路\nbit0 饱和：%4    bit1 队列溢出：%5\nbit2 调度异常：%6    bit3 打包异常：%7\n\n%8\n全局故障不受屏蔽影响；设备 bit0 仅保留告警。")
            .arg(s.flags, 8, 16, QChar('0')).arg(s.latchedFlags, 2, 16, QChar('0')).arg(qualityFaults, 2, 16, QChar('0'))
            .arg(bool(s.latchedFlags & 1)).arg(bool(s.latchedFlags & 2)).arg(bool(s.latchedFlags & 4)).arg(bool(s.latchedFlags & 8)).arg(qualitySummary));
        m_qualityDetails->setToolTip(s.acousticStatus + QStringLiteral("\n") + flagsDescription(s.flags)
            + QStringLiteral("\n设备 bit0 为保持型总标志，无通道明细，不能精确归因；通道削顶依据实收 PCM 的正/负满量程样本。\n原始统计（含已屏蔽路）完整保留；详细通道见“通道质量”。"));
        m_selftestDetails->setText(QStringLiteral("%1\n已验证 %2 帧 / %3 通道样本\n不匹配 %4 帧 / %5 样本\n连续性错误 %6 · 故障包 %7\n%8")
            .arg(s.selftest.status()).arg(s.selftest.frames).arg(s.selftest.frames * 128).arg(s.selftest.mismatchingFrames).arg(s.selftest.mismatchingSamples)
            .arg(s.selftest.continuityErrors).arg(s.selftest.flaggedPackets).arg(s.selftest.firstMismatch));
        m_selftestProgress->setValue(int(std::min<quint64>(48000, s.selftest.frames)));
        m_hardwareDetails->setText(QStringLiteral("UDP：%1\n%2\n\nUART 明细见“UART 串口”；其 TX 计数不等于 PC 接收。\n电压、温度、CLK 负载和实际位流须外部确认。\n数字自测 PASS 不代表声学验收。")
            .arg(!hasData ? QStringLiteral("尚无有效数据") : active ? QStringLiteral("数据有效到达") : QStringLiteral("已停止或超时，显示历史数据"), m_serial->summary()));
        m_packetDetails->setPlainText(!hasData ? QStringLiteral("尚未收到有效 M128 报文；序号、帧号和采样时间未知。") : QStringLiteral("M128 v1 · UDP 1208 字节\n头 32 字节（大端），记录偏移 32 / 424 / 816，每条 frame_id 8 字节 + PCM 384 字节（s24le）\n\n最近头与首帧号（非实时状态时为历史数据）：\n%1\n\npacket_sequence=%2\nepoch=%3\nlast_frame_id=%4\nflags=0x%5\n\n有效相对采样时间=%6 秒（仅 bit2=0；非 UTC）\n\n包号审计：\n%7\n帧号审计：\n%8")
            .arg(QString::fromLatin1(s.latestHeader.toHex(' '))).arg(s.sequence).arg(s.epoch).arg(s.frame).arg(s.flags, 8, 16, QChar('0'))
            .arg(s.latchedFlags & 4 ? QStringLiteral("不可用") : QString::number(effectiveTimeSeconds(s.frame), 'f', 9))
            .arg(QString::fromUtf8(QJsonDocument(s.packetSequence).toJson()), QString::fromUtf8(QJsonDocument(s.frameSequence).toJson())));
        if (m_qualityTable->isVisible()) for (int ch = 0; ch < 128; ++ch) {
            const bool excluded = s.disabledProcessingChannels[ch];
            const auto &q = s.quality[ch]; const QStringList values {QString::number(ch) + (excluded ? QStringLiteral(" ×") : QString()), QString("%1/%2").arg(ch / 2).arg(ch % 2 ? "VDD" : "GND"), dataConnector(ch) + "/" + dataBank(ch),
                QString::number(q.rmsDb, 'f', 1), QString::number(q.acRmsDb, 'f', 1), QString::number(q.dc, 'f', 5), QString::number(q.clipped),
                QString::number(q.zeros), QString::number(q.unchanged), QString::number(s.selftest.channelMismatches[ch])};
            for (int col = 0; col < 10; ++col) {
                auto *item = m_qualityTable->item(ch, col);
                item->setText(col >= 3 && !hasData ? QStringLiteral("—") : values[col]);
                item->setForeground(excluded ? QBrush(QColor("#8795a3")) : QBrush());
                item->setToolTip(excluded ? QStringLiteral("已屏蔽：原始统计仅供排查，不参与声学质量阻断；原始监听、录制和自测审计不变。")
                    : QStringLiteral("参与声学质量判定；削顶故障按当前会话锁存，屏蔽该通道可排除其影响。"));
            }
            m_qualityTable->item(ch, 2)->setToolTip(QStringLiteral("FPGA 焊盘 %1；CLK 负载分组由实际接线决定，不能按通道号推定。").arg(dataPin(ch)));
        }
    }
    if (!s.error.isEmpty() && s.error != m_lastError) { showError(s.error); m_lastError = s.error; }
    if (!recording.error.isEmpty() && recording.error != m_lastRecordError) {
        showError(QStringLiteral("录制异常：") + recording.error); m_lastRecordError = recording.error;
    }
    if (m_closePending && !recording.active && !recording.finishing) {
        m_closePending = false;
        if (!recording.error.isEmpty()) {
            centralWidget()->setEnabled(true);
            showError(QStringLiteral("录制未完整写入，已取消自动退出：") + recording.error + QStringLiteral("。请检查保存目录与清单。"));
        } else QTimer::singleShot(0, this, &QWidget::close);
    }
}

void MainWindow::saveDiagnosticReport(const QString &path)
{
    auto report = m_pipeline.snapshot().json();
    report["exported_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    report["source_ip"] = m_source->text(); report["source_port"] = m_sourcePort->value();
    report["geometry"] = m_processing.geometry.json();
    report["sys3_uart"] = m_serial->json();
    report["audiovisual_fusion"] = fusionDiagnostics();
    QSaveFile file(path); if (!file.open(QIODevice::WriteOnly)) { showError(file.errorString()); return; }
    const auto bytes = QJsonDocument(report).toJson();
    if (file.write(bytes) != bytes.size() || !file.commit()) showError(file.errorString());
    else statusBar()->showMessage(QStringLiteral("诊断报告已导出：") + path);
}

void MainWindow::showError(const QString &text)
{
    statusBar()->showMessage(text, 15000); if (m_log) m_log->appendPlainText(text);
    if (m_notice) { m_noticeText->setText(text); m_notice->show(); }
}

void MainWindow::applyTheme(bool dark)
{
    const QString bg = dark ? "#101a25" : "#edf2f5", card = dark ? "#182634" : "#ffffff", text = dark ? "#dce8f0" : "#213746", border = dark ? "#344b60" : "#c2cfd8";
    setStyleSheet(QStringLiteral("QMainWindow,QWidget {background:%1;color:%3;font-family:'Microsoft YaHei UI';font-size:12px;}"
        "QLabel#appTitle {font-size:20px;font-weight:600;padding:4px;}"
        "QLabel#secondaryText {color:%5;}"
        "QWidget#operationNotice {border:1px solid #b87936;border-radius:4px;}"
        "QLabel#acquisitionStatus {border-radius:4px;padding:5px 12px;font-weight:600;}"
        "QLabel#acquisitionStatus[level='neutral'] {background:#344655;color:#e1eaf2;}"
        "QLabel#acquisitionStatus[level='normal'] {background:#156754;color:#e4fff3;}"
        "QLabel#acquisitionStatus[level='warning'] {background:#78551a;color:#fff0c1;}"
        "QLabel#acquisitionStatus[level='fault'] {background:#963c3c;color:#fff0f0;}"
        "QGroupBox {background:%2;border:1px solid %4;border-radius:8px;margin-top:16px;padding:12px;}"
        "QGroupBox::title {subcontrol-origin:margin;left:14px;padding:0 5px;color:#43bea5;}"
        "QGroupBox#uartAudioGroup {margin-top:10px;padding:5px;}"
        "QGroupBox#uartAudioGroup QPushButton {padding:4px 8px;}"
        "QGroupBox#uartAudioGroup QSpinBox,QGroupBox#uartAudioGroup QDoubleSpinBox {padding-top:3px;padding-bottom:3px;}"
        "QTableWidget#uartAudioValues QHeaderView::section {padding:3px;}"
        "QPushButton {background:%2;border:1px solid %4;border-radius:5px;padding:7px 12px;}"
        "QPushButton:hover {border-color:#46bea5;} QPushButton:disabled {color:#718290;}"
        "QPushButton:focus,QLineEdit:focus,QSpinBox:focus,QDoubleSpinBox:focus,QComboBox:focus,QListWidget:focus {border:2px solid #248f7d;}"
        "QCheckBox {spacing:6px;} QCheckBox::indicator,QListView::indicator {width:16px;height:16px;border:1px solid #8196a6;border-radius:3px;background:%2;}"
        "QCheckBox::indicator:checked,QListView::indicator:checked {background:#187965;border-color:#187965;image:url(:/m128/resources/check.svg);}"
        "QCheckBox::indicator:disabled,QListView::indicator:disabled {border-color:#718290;background:%1;}"
        "QCheckBox:focus {outline:1px solid #248f7d;}"
        "QLineEdit,QSpinBox,QDoubleSpinBox,QComboBox {background:%2;border:1px solid %4;border-radius:4px;padding:5px;}"
        "QSpinBox,QDoubleSpinBox,QComboBox {padding-right:22px;}"
        "QSpinBox::up-button,QDoubleSpinBox::up-button {subcontrol-origin:border;subcontrol-position:top right;width:20px;border-left:1px solid %4;}"
        "QSpinBox::down-button,QDoubleSpinBox::down-button {subcontrol-origin:border;subcontrol-position:bottom right;width:20px;border-left:1px solid %4;}"
        "QSpinBox::up-arrow,QDoubleSpinBox::up-arrow {image:url(:/m128/resources/up.svg);width:10px;height:7px;}"
        "QSpinBox::down-arrow,QDoubleSpinBox::down-arrow,QComboBox::down-arrow {image:url(:/m128/resources/down.svg);width:10px;height:7px;}"
        "QComboBox::drop-down {subcontrol-origin:border;subcontrol-position:top right;width:20px;border-left:1px solid %4;}"
        "QLineEdit:disabled,QSpinBox:disabled,QDoubleSpinBox:disabled,QComboBox:disabled {color:#718290;background:%1;}"
        "QTabWidget::pane {border:1px solid %4;} QTabBar::tab {padding:10px 16px;background:%2;}"
        "QTabBar::tab:selected {border-bottom:3px solid #43bea5;}"
        "QTableWidget,QListWidget,QPlainTextEdit {background:%2;alternate-background-color:%1;border:1px solid %4;}"
        "QHeaderView::section {background:%1;color:%3;padding:7px;border:1px solid %4;}"
        "QProgressBar {border:1px solid %4;text-align:center;height:20px;} QProgressBar::chunk {background:#248f7d;}")
        .arg(bg, card, text, border, dark ? "#9ab0c2" : "#50697c"));
}

} // namespace MicArray::Array128
