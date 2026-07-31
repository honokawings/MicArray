#include "ui/livelocalizationpage.h"

#include "analysis/calibrationprofile.h"
#include "analysis/cameraviewcalibrator.h"
#include "analysis/directionprojection.h"
#include "analysis/soundsourcelocatorregistry.h"
#include "ui/livelocalizationview.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineF>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace MicArray {
namespace {

QDoubleSpinBox *angleSpin(QWidget *parent, const QString &objectName,
                          double minimum, double maximum)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setObjectName(objectName);
    spin->setRange(minimum, maximum);
    spin->setDecimals(1);
    spin->setSingleStep(1.0);
    spin->setSuffix(QStringLiteral("°"));
    return spin;
}

} // namespace

LiveLocalizationPage::LiveLocalizationPage(AppSettings *settings, QWidget *parent)
    : QWidget(parent)
    , m_settingsStore(settings)
    , m_worker(new LiveLocalizationWorker)
{
    setObjectName(QStringLiteral("liveLocalizationPage"));
    buildUi();
    m_view->setSpatialResponseActive(false);
    restoreSettings();
    m_workerThread.setObjectName(QStringLiteral("LiveSourceLocalizationThread"));
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &LiveLocalizationPage::configureWorkerRequested,
            m_worker, &LiveLocalizationWorker::configure, Qt::QueuedConnection);
    connect(this, &LiveLocalizationPage::workerActiveRequested,
            m_worker, &LiveLocalizationWorker::setActive, Qt::QueuedConnection);
    connect(this, &LiveLocalizationPage::spatialGridRequested,
            m_worker, &LiveLocalizationWorker::setSpatialGrid, Qt::QueuedConnection);
    connect(this, &LiveLocalizationPage::audioFrameRequested,
            m_worker, &LiveLocalizationWorker::pushAdjustedFrame, Qt::QueuedConnection);
    connect(m_worker, &LiveLocalizationWorker::audioFrameConsumed, this, [this] {
        m_pendingAudioFrames = std::max(0, m_pendingAudioFrames - 1);
    });
    connect(this, &LiveLocalizationPage::calibrationSampleRequested,
            m_worker, &LiveLocalizationWorker::captureStableDirection, Qt::QueuedConnection);
    connect(m_worker, &LiveLocalizationWorker::localizationReady, this,
             [this](const LiveLocalizationResult &result) {
                 if (!m_active || !m_settings.enabled
                     || result.generation
                         != m_gridGeneration) {
                    return;
                }
                m_view->setDirections(result.directions);
                if (result.directions.isEmpty()) {
                    m_resultLabel->setText(QStringLiteral("方位角 --　俯仰角 --"));
                    return;
                }
                const auto &direction = result.directions.first();
                m_resultLabel->setText(
                    QStringLiteral("方位角 %1°　俯仰角 %2°　置信度 %3%　耗时 %4 ms")
                        .arg(direction.azimuthDegrees, 0, 'f', 1)
                        .arg(direction.elevationDegrees, 0, 'f', 1)
                        .arg(direction.confidence * 100.0, 0, 'f', 0)
                        .arg(result.elapsedMilliseconds));
            });
    connect(m_worker, &LiveLocalizationWorker::spatialResponseReady,
            m_view, &LiveLocalizationView::setSpatialResponse);
    connect(m_worker, &LiveLocalizationWorker::statusChanged, this,
            [this](const QString &status) {
                m_statusLabel->setText(status);
                m_view->setStatusText(status);
            });
    connect(m_worker, &LiveLocalizationWorker::spatialResponseSupported,
            this, [this](bool supported) {
                const int heatmap =
                    m_displayModeCombo->findData(QStringLiteral("heatmap"));
                if (heatmap >= 0) {
                    m_displayModeCombo->setItemData(
                        heatmap, supported ? QVariant() : QVariant(0), Qt::UserRole - 1);
                }
                if (!supported
                    && m_displayModeCombo->currentData().toString()
                        == QStringLiteral("heatmap")) {
                    m_displayModeCombo->setCurrentIndex(
                        m_displayModeCombo->findData(QStringLiteral("marker")));
                }
            });
    connect(m_worker, &LiveLocalizationWorker::calibrationSampleReady,
            this, &LiveLocalizationPage::acceptCalibrationDirection);
    connectUi();
    m_workerThread.start();
    emit configureWorkerRequested(m_settings);
}

LiveLocalizationPage::~LiveLocalizationPage()
{
    persistSettings();
    if (m_workerThread.isRunning()) {
        QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
        m_workerThread.quit();
        m_workerThread.wait(3000);
    }
}

void LiveLocalizationPage::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    startNewProcessingEpoch();
    const bool processingActive =
        active && m_settings.enabled;
    m_view->setSpatialResponseActive(processingActive);
    emit workerActiveRequested(processingActive);
}

void LiveLocalizationPage::setVideoFrame(const QImage &image,
                                         quint32,
                                         quint32)
{
    const bool sizeChanged = image.size() != m_videoFrame.size();
    m_videoFrame = image;
    m_view->setFrame(image);
    if (sizeChanged) {
        rebuildSpatialGrid();
    }
}

void LiveLocalizationPage::submitAdjustedAudioFrame(const QByteArray &payload,
                                                    quint32 timestampSeconds,
                                                    quint32 timestampMicros)
{
    if (!m_active || !m_settings.enabled
        || m_pendingAudioFrames >= MaxPendingAudioFrames) {
        return;
    }
    ++m_pendingAudioFrames;
    emit audioFrameRequested(payload, timestampSeconds, timestampMicros);
}

bool LiveLocalizationPage::saveCalibrationProfile(
    const QString &filePath, QString *errorMessage) const
{
    VideoCalibrationProfile profile;
    profile.camera = m_settings.camera;
    profile.referenceVideoSize = m_settings.calibrationReferenceVideoSize;
    profile.calibrationRmsPixels = m_settings.calibrationRmsPixels;
    profile.samples = m_settings.calibrationSamples;
    profile.savedAtUtc = QDateTime::currentDateTimeUtc();
    return VideoCalibrationProfileStore::save(filePath, profile, errorMessage);
}

bool LiveLocalizationPage::loadCalibrationProfile(
    const QString &filePath, QString *errorMessage)
{
    VideoCalibrationProfile profile;
    if (!VideoCalibrationProfileStore::load(filePath, &profile, errorMessage)) {
        return false;
    }

    m_calibrating = false;
    m_workingSamples.clear();
    m_settings.camera = profile.camera;
    m_settings.calibrationReferenceVideoSize = profile.referenceVideoSize;
    m_settings.calibrationRmsPixels = profile.calibrationRmsPixels;
    m_settings.calibrationSamples = profile.samples;
    m_savedCamera = profile.camera;
    m_view->setCalibrationMode(false);
    m_startCalibrationButton->setEnabled(true);
    m_cancelCalibrationButton->setEnabled(false);
    m_undoCalibrationButton->setEnabled(false);
    applyCameraToUi(profile.camera);
    m_view->setCameraView(profile.camera);
    m_rmsLabel->setText(profile.samples.size() == 4
        ? QStringLiteral("RMS 误差：%1 px").arg(profile.calibrationRmsPixels, 0, 'f', 1)
        : QStringLiteral("RMS 误差：--"));
    m_progressLabel->setText(profile.samples.size() == 4
        ? QStringLiteral("已载入四点校准")
        : QStringLiteral("已载入视角参数"));
    persistSettings();
    rebuildSpatialGrid();
    refreshCalibrationDetails();
    refreshCalibrationOverlay();
    return true;
}

void LiveLocalizationPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    m_view = new LiveLocalizationView(splitter);
    auto *scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(300);
    scroll->setMaximumWidth(410);
    auto *panel = new QWidget(scroll);
    auto *panelLayout = new QVBoxLayout(panel);

    auto *localizationGroup = new QGroupBox(QStringLiteral("实时定位"), panel);
    auto *localizationLayout = new QFormLayout(localizationGroup);
    m_enabledCheck = new QCheckBox(QStringLiteral("启用实时定位"), localizationGroup);
    m_enabledCheck->setObjectName(QStringLiteral("liveLocalizationEnabledCheck"));
    m_locatorCombo = new QComboBox(localizationGroup);
    m_locatorCombo->setObjectName(QStringLiteral("liveLocalizationLocatorCombo"));
    SoundSourceLocatorRegistry registry;
    registerBuiltInSoundSourceLocators(&registry);
    for (const LocatorDescriptor &descriptor : registry.descriptors()) {
        m_locatorCombo->addItem(descriptor.displayName, descriptor.id);
    }
    m_displayModeCombo = new QComboBox(localizationGroup);
    m_displayModeCombo->setObjectName(QStringLiteral("liveLocalizationDisplayModeCombo"));
    m_displayModeCombo->addItem(QStringLiteral("纯标记"), QStringLiteral("marker"));
    m_displayModeCombo->addItem(QStringLiteral("真实空间响应热力图"),
                                QStringLiteral("heatmap"));
    m_spatialResolutionModeCombo =
        new QComboBox(localizationGroup);
    m_spatialResolutionModeCombo->setObjectName(
        QStringLiteral("liveSpatialResolutionModeCombo"));
    m_spatialResolutionModeCombo->addItem(
        QStringLiteral("实时增强（约 10 FPS）"),
        static_cast<int>(
            SpatialResolutionMode::RealtimeEnhanced));
    m_spatialResolutionModeCombo->addItem(
        QStringLiteral("高精度（约 5 FPS）"),
        static_cast<int>(
            SpatialResolutionMode::HighPrecision));
    m_heatmapOpacitySpin = new QDoubleSpinBox(localizationGroup);
    m_heatmapOpacitySpin->setObjectName(QStringLiteral("liveHeatmapOpacitySpin"));
    m_heatmapOpacitySpin->setRange(0.0, 1.0);
    m_heatmapOpacitySpin->setSingleStep(0.05);
    m_heatmapGridColumnsSpin = new QSpinBox(localizationGroup);
    m_heatmapGridColumnsSpin->setObjectName(
        QStringLiteral("liveHeatmapGridColumnsSpin"));
    m_heatmapGridColumnsSpin->setRange(16, 640);
    m_heatmapGridRowsSpin = new QSpinBox(localizationGroup);
    m_heatmapGridRowsSpin->setObjectName(QStringLiteral("liveHeatmapGridRowsSpin"));
    m_heatmapGridRowsSpin->setRange(9, 360);
    m_updateIntervalSpin = new QSpinBox(localizationGroup);
    m_updateIntervalSpin->setObjectName(
        QStringLiteral("liveLocalizationUpdateIntervalSpin"));
    m_updateIntervalSpin->setRange(20, 1000);
    m_updateIntervalSpin->setSuffix(QStringLiteral(" ms"));
    m_confidenceSpin = new QDoubleSpinBox(localizationGroup);
    m_confidenceSpin->setObjectName(
        QStringLiteral("liveLocalizationConfidenceSpin"));
    m_confidenceSpin->setRange(0.0, 1.0);
    m_confidenceSpin->setSingleStep(0.05);
    localizationLayout->addRow(m_enabledCheck);
    localizationLayout->addRow(QStringLiteral("算法"), m_locatorCombo);
    localizationLayout->addRow(QStringLiteral("显示方式"), m_displayModeCombo);
    localizationLayout->addRow(
        QStringLiteral("空间精度"),
        m_spatialResolutionModeCombo);
    localizationLayout->addRow(QStringLiteral("热力图透明度"), m_heatmapOpacitySpin);
    localizationLayout->addRow(QStringLiteral("方向网格宽度"), m_heatmapGridColumnsSpin);
    localizationLayout->addRow(QStringLiteral("方向网格高度"), m_heatmapGridRowsSpin);
    localizationLayout->addRow(QStringLiteral("更新周期"), m_updateIntervalSpin);
    localizationLayout->addRow(QStringLiteral("置信度门限"), m_confidenceSpin);
    panelLayout->addWidget(localizationGroup);

    auto *cameraGroup = new QGroupBox(QStringLiteral("视频视角参数"), panel);
    auto *cameraLayout = new QFormLayout(cameraGroup);
    m_centerAzimuthSpin = angleSpin(
        cameraGroup, QStringLiteral("liveCameraCenterAzimuthSpin"), -180.0, 180.0);
    m_centerElevationSpin = angleSpin(
        cameraGroup, QStringLiteral("liveCameraCenterElevationSpin"), -89.0, 89.0);
    m_horizontalFovSpin = angleSpin(
        cameraGroup, QStringLiteral("liveCameraHorizontalFovSpin"), 1.1, 178.9);
    m_verticalFovSpin = angleSpin(
        cameraGroup, QStringLiteral("liveCameraVerticalFovSpin"), 1.1, 178.9);
    m_rollSpin = angleSpin(
        cameraGroup, QStringLiteral("liveCameraRollSpin"), -180.0, 180.0);
    cameraLayout->addRow(QStringLiteral("中心方位角"), m_centerAzimuthSpin);
    cameraLayout->addRow(QStringLiteral("中心俯仰角"), m_centerElevationSpin);
    cameraLayout->addRow(QStringLiteral("水平视场角"), m_horizontalFovSpin);
    cameraLayout->addRow(QStringLiteral("垂直视场角"), m_verticalFovSpin);
    cameraLayout->addRow(QStringLiteral("滚转角"), m_rollSpin);
    auto *restoreRow = new QHBoxLayout;
    m_restoreSavedButton = new QPushButton(QStringLiteral("恢复已保存"), cameraGroup);
    m_restoreDefaultsButton = new QPushButton(QStringLiteral("恢复默认"), cameraGroup);
    restoreRow->addWidget(m_restoreSavedButton);
    restoreRow->addWidget(m_restoreDefaultsButton);
    cameraLayout->addRow(restoreRow);
    panelLayout->addWidget(cameraGroup);

    auto *calibrationGroup = new QGroupBox(QStringLiteral("四点视频校准"), panel);
    auto *calibrationLayout = new QVBoxLayout(calibrationGroup);
    auto *calibrationButtons = new QHBoxLayout;
    m_startCalibrationButton = new QPushButton(QStringLiteral("开始四点校准"),
                                               calibrationGroup);
    m_startCalibrationButton->setObjectName(
        QStringLiteral("startFourPointCalibrationButton"));
    m_cancelCalibrationButton = new QPushButton(QStringLiteral("取消"),
                                                calibrationGroup);
    m_cancelCalibrationButton->setObjectName(
        QStringLiteral("cancelFourPointCalibrationButton"));
    m_clearCalibrationButton = new QPushButton(QStringLiteral("清除"),
                                               calibrationGroup);
    m_clearCalibrationButton->setObjectName(
        QStringLiteral("clearVideoCalibrationButton"));
    m_cancelCalibrationButton->setEnabled(false);
    calibrationButtons->addWidget(m_startCalibrationButton);
    calibrationButtons->addWidget(m_cancelCalibrationButton);
    calibrationButtons->addWidget(m_clearCalibrationButton);
    m_undoCalibrationButton = new QPushButton(QStringLiteral("撤销上一点"),
                                              calibrationGroup);
    m_undoCalibrationButton->setObjectName(
        QStringLiteral("undoCalibrationPointButton"));
    m_undoCalibrationButton->setEnabled(false);
    m_showCalibrationOverlayCheck = new QCheckBox(QStringLiteral("显示已完成校准点"),
                                                  calibrationGroup);
    m_showCalibrationOverlayCheck->setObjectName(
        QStringLiteral("showCalibrationOverlayCheck"));
    m_calibrationSamplesTable = new QTableWidget(4, 7, calibrationGroup);
    m_calibrationSamplesTable->setObjectName(
        QStringLiteral("calibrationSamplesTable"));
    m_calibrationSamplesTable->setHorizontalHeaderLabels({
        QStringLiteral("点位"), QStringLiteral("方位角"), QStringLiteral("俯仰角"),
        QStringLiteral("置信度"), QStringLiteral("角波动"),
        QStringLiteral("视频 X"), QStringLiteral("视频 Y"),
    });
    m_calibrationSamplesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_calibrationSamplesTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_calibrationSamplesTable->verticalHeader()->setVisible(false);
    m_calibrationSamplesTable->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    m_calibrationSamplesTable->horizontalHeader()->setStretchLastSection(true);
    m_exportCalibrationButton = new QPushButton(QStringLiteral("保存校准文件"),
                                                calibrationGroup);
    m_exportCalibrationButton->setObjectName(
        QStringLiteral("exportVideoCalibrationButton"));
    m_importCalibrationButton = new QPushButton(QStringLiteral("载入校准文件"),
                                                calibrationGroup);
    m_importCalibrationButton->setObjectName(
        QStringLiteral("importVideoCalibrationButton"));
    auto *profileButtons = new QHBoxLayout;
    profileButtons->addWidget(m_exportCalibrationButton);
    profileButtons->addWidget(m_importCalibrationButton);
    m_progressLabel = new QLabel(QStringLiteral("尚未开始校准"), calibrationGroup);
    m_progressLabel->setObjectName(QStringLiteral("liveCalibrationProgressLabel"));
    m_rmsLabel = new QLabel(QStringLiteral("RMS 误差：--"), calibrationGroup);
    m_rmsLabel->setObjectName(QStringLiteral("liveCalibrationRmsLabel"));
    calibrationLayout->addLayout(calibrationButtons);
    calibrationLayout->addWidget(m_undoCalibrationButton);
    calibrationLayout->addWidget(m_showCalibrationOverlayCheck);
    calibrationLayout->addWidget(m_calibrationSamplesTable);
    calibrationLayout->addLayout(profileButtons);
    calibrationLayout->addWidget(m_progressLabel);
    calibrationLayout->addWidget(m_rmsLabel);
    panelLayout->addWidget(calibrationGroup);
    panelLayout->addStretch();
    scroll->setWidget(panel);
    splitter->addWidget(m_view);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 1);
    root->addWidget(splitter, 1);
    m_resultLabel = new QLabel(QStringLiteral("方位角 --　俯仰角 --"), this);
    m_resultLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel = new QLabel(QStringLiteral("等待实时音视频"), this);
    root->addWidget(m_resultLabel);
    root->addWidget(m_statusLabel);
}

void LiveLocalizationPage::connectUi()
{
    connect(m_enabledCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        m_settings.enabled = enabled;
        startNewProcessingEpoch();
        m_view->setSpatialResponseActive(
            m_active && enabled);
        persistSettings();
        emit workerActiveRequested(m_active && enabled);
    });
    connect(m_locatorCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_settings.locatorId = m_locatorCombo->itemData(index).toString();
        startNewProcessingEpoch();
        persistSettings();
        emit configureWorkerRequested(m_settings);
    });
    connect(m_displayModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_settings.displayMode = m_displayModeCombo->itemData(index).toString();
        m_view->setDisplayMode(m_settings.displayMode);
        persistSettings();
    });
    connect(m_spatialResolutionModeCombo,
            &QComboBox::currentIndexChanged,
            this, [this](int index) {
        m_settings.spatialResolutionMode =
            static_cast<SpatialResolutionMode>(
                m_spatialResolutionModeCombo->itemData(
                    index).toInt());
        startNewProcessingEpoch();
        persistSettings();
        emit configureWorkerRequested(m_settings);
    });
    connect(m_heatmapOpacitySpin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        m_settings.heatmapOpacity = value;
        m_view->setHeatmapOpacity(value);
        persistSettings();
    });
    connect(m_heatmapGridColumnsSpin, &QSpinBox::valueChanged, this, [this](int value) {
        m_settings.heatmapGridColumns = value;
        rebuildSpatialGrid();
        persistSettings();
    });
    connect(m_heatmapGridRowsSpin, &QSpinBox::valueChanged, this, [this](int value) {
        m_settings.heatmapGridRows = value;
        rebuildSpatialGrid();
        persistSettings();
    });
    connect(m_updateIntervalSpin, &QSpinBox::valueChanged, this, [this](int value) {
        m_settings.updateIntervalMs = value;
        startNewProcessingEpoch();
        emit configureWorkerRequested(m_settings);
        persistSettings();
    });
    connect(m_confidenceSpin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        m_settings.confidenceThreshold = value;
        startNewProcessingEpoch();
        emit configureWorkerRequested(m_settings);
        persistSettings();
    });
    for (QDoubleSpinBox *spin : {m_centerAzimuthSpin, m_centerElevationSpin,
                                 m_horizontalFovSpin, m_verticalFovSpin, m_rollSpin}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this,
                [this] { updateCameraFromUi(false); });
        connect(spin, &QDoubleSpinBox::editingFinished, this,
                [this] { updateCameraFromUi(true); });
    }
    connect(m_startCalibrationButton, &QPushButton::clicked,
            this, &LiveLocalizationPage::startCalibration);
    connect(m_cancelCalibrationButton, &QPushButton::clicked,
            this, &LiveLocalizationPage::cancelCalibration);
    connect(m_undoCalibrationButton, &QPushButton::clicked,
            this, &LiveLocalizationPage::undoLastCalibrationPoint);
    connect(m_clearCalibrationButton, &QPushButton::clicked,
            this, &LiveLocalizationPage::clearCalibration);
    connect(m_showCalibrationOverlayCheck, &QCheckBox::toggled,
            this, [this](bool visible) {
                m_settings.showCalibrationOverlay = visible;
                persistSettings();
                refreshCalibrationOverlay();
            });
    connect(m_exportCalibrationButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("保存视频校准"),
            QStringLiteral("video-calibration.json"),
            QStringLiteral("JSON 校准文件 (*.json)"));
        if (path.isEmpty()) {
            return;
        }
        QString error;
        m_statusLabel->setText(saveCalibrationProfile(path, &error)
            ? QStringLiteral("校准文件已保存：%1").arg(path)
            : QStringLiteral("保存校准文件失败：%1").arg(error));
    });
    connect(m_importCalibrationButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("载入视频校准"), {},
            QStringLiteral("JSON 校准文件 (*.json)"));
        if (path.isEmpty()) {
            return;
        }
        QString error;
        m_statusLabel->setText(loadCalibrationProfile(path, &error)
            ? QStringLiteral("校准文件已载入：%1").arg(path)
            : QStringLiteral("载入校准文件失败：%1").arg(error));
    });
    connect(m_restoreSavedButton, &QPushButton::clicked, this, [this] {
        applyCameraToUi(m_savedCamera);
        updateCameraFromUi(false);
    });
    connect(m_restoreDefaultsButton, &QPushButton::clicked, this, [this] {
        applyCameraToUi(CameraViewSettings {});
        updateCameraFromUi(true);
    });
    connect(m_view, &LiveLocalizationView::calibrationPointClicked,
            this, [this](const QPointF &point) {
                if (!m_calibrating) {
                    return;
                }
                if (m_workingSamples.size() >= 4) {
                    m_progressLabel->setText(
                        QStringLiteral("拟合未通过，请先撤销上一点再重新采样"));
                    return;
                }
                m_pendingCalibrationPoint = point;
                emit calibrationSampleRequested(++m_calibrationRequestId);
                m_progressLabel->setText(QStringLiteral("正在评估第 %1/4 点的声源稳定性…")
                                             .arg(m_workingSamples.size() + 1));
            });
}

void LiveLocalizationPage::restoreSettings()
{
    m_settings = m_settingsStore != nullptr
        ? m_settingsStore->liveLocalization() : LiveLocalizationSettings {};
    m_savedCamera = m_settings.camera;
    m_enabledCheck->setChecked(m_settings.enabled);
    m_locatorCombo->setCurrentIndex(
        std::max(0, m_locatorCombo->findData(m_settings.locatorId)));
    m_displayModeCombo->setCurrentIndex(
        std::max(0, m_displayModeCombo->findData(m_settings.displayMode)));
    m_spatialResolutionModeCombo->setCurrentIndex(
        std::max(0, m_spatialResolutionModeCombo->findData(
            static_cast<int>(
                m_settings.spatialResolutionMode))));
    m_heatmapOpacitySpin->setValue(m_settings.heatmapOpacity);
    m_heatmapGridColumnsSpin->setValue(m_settings.heatmapGridColumns);
    m_heatmapGridRowsSpin->setValue(m_settings.heatmapGridRows);
    m_updateIntervalSpin->setValue(m_settings.updateIntervalMs);
    m_confidenceSpin->setValue(m_settings.confidenceThreshold);
    m_showCalibrationOverlayCheck->setChecked(m_settings.showCalibrationOverlay);
    applyCameraToUi(m_settings.camera);
    m_view->setCameraView(m_settings.camera);
    m_view->setDisplayMode(m_settings.displayMode);
    m_view->setHeatmapOpacity(m_settings.heatmapOpacity);
    m_rmsLabel->setText(m_settings.calibrationSamples.size() == 4
        ? QStringLiteral("RMS 误差：%1 px").arg(m_settings.calibrationRmsPixels, 0, 'f', 1)
        : QStringLiteral("RMS 误差：--"));
    refreshCalibrationDetails();
    refreshCalibrationOverlay();
}

void LiveLocalizationPage::persistSettings()
{
    if (m_settingsStore != nullptr) {
        m_settingsStore->saveLiveLocalization(m_settings);
    }
}

void LiveLocalizationPage::applyCameraToUi(const CameraViewSettings &camera)
{
    const QSignalBlocker a(m_centerAzimuthSpin);
    const QSignalBlocker b(m_centerElevationSpin);
    const QSignalBlocker c(m_horizontalFovSpin);
    const QSignalBlocker d(m_verticalFovSpin);
    const QSignalBlocker e(m_rollSpin);
    m_centerAzimuthSpin->setValue(camera.centerAzimuthDegrees);
    m_centerElevationSpin->setValue(camera.centerElevationDegrees);
    m_horizontalFovSpin->setValue(camera.horizontalFovDegrees);
    m_verticalFovSpin->setValue(camera.verticalFovDegrees);
    m_rollSpin->setValue(camera.rollDegrees);
}

void LiveLocalizationPage::updateCameraFromUi(bool persist)
{
    m_settings.camera.centerAzimuthDegrees = m_centerAzimuthSpin->value();
    m_settings.camera.centerElevationDegrees = m_centerElevationSpin->value();
    m_settings.camera.horizontalFovDegrees = m_horizontalFovSpin->value();
    m_settings.camera.verticalFovDegrees = m_verticalFovSpin->value();
    m_settings.camera.rollDegrees = m_rollSpin->value();
    m_view->setCameraView(m_settings.camera);
    rebuildSpatialGrid();
    refreshCalibrationOverlay();
    if (persist) {
        m_savedCamera = m_settings.camera;
        persistSettings();
    }
}

void LiveLocalizationPage::startNewProcessingEpoch()
{
    m_view->setDirections({});
    m_view->setSpatialResponse({});
    m_resultLabel->setText(
        QStringLiteral("方位角 --　俯仰角 --"));
    rebuildSpatialGrid();
}

void LiveLocalizationPage::rebuildSpatialGrid()
{
    const quint64 generation = ++m_gridGeneration;
    m_view->setExpectedSpatialResponseGeneration(generation);
    SpatialResponseGrid grid;
    if (m_videoFrame.isNull()) {
        emit spatialGridRequested(grid, generation);
        return;
    }
    grid.columns = m_settings.heatmapGridColumns;
    grid.rows = m_settings.heatmapGridRows;
    grid.directions.reserve(grid.columns * grid.rows);
    for (int row = 0; row < grid.rows; ++row) {
        for (int column = 0; column < grid.columns; ++column) {
            grid.directions.append(unprojectVideoPoint(
                {static_cast<double>(column + 0.5) / grid.columns,
                 static_cast<double>(row + 0.5) / grid.rows},
                m_settings.camera));
        }
    }
    emit spatialGridRequested(grid, generation);
}

void LiveLocalizationPage::startCalibration()
{
    if (m_videoFrame.isNull()) {
        m_statusLabel->setText(QStringLiteral("没有实时视频，无法开始校准"));
        return;
    }
    m_calibrating = true;
    m_workingSamples.clear();
    m_view->setCalibrationMode(true);
    m_view->setDirections({});
    m_view->setSpatialResponse({});
    m_view->setCalibrationSamples({});
    m_startCalibrationButton->setEnabled(false);
    m_cancelCalibrationButton->setEnabled(true);
    m_undoCalibrationButton->setEnabled(false);
    refreshCalibrationDetails();
    m_progressLabel->setText(
        QStringLiteral("第 1/4 点：保持声源稳定并点击视频中的声源位置"));
}

void LiveLocalizationPage::cancelCalibration()
{
    m_calibrating = false;
    m_workingSamples.clear();
    m_view->setCalibrationMode(false);
    m_startCalibrationButton->setEnabled(true);
    m_cancelCalibrationButton->setEnabled(false);
    m_undoCalibrationButton->setEnabled(false);
    m_progressLabel->setText(QStringLiteral("校准已取消"));
    refreshCalibrationDetails();
    refreshCalibrationOverlay();
}

void LiveLocalizationPage::undoLastCalibrationPoint()
{
    if (!m_calibrating || m_workingSamples.isEmpty()) {
        return;
    }
    m_workingSamples.removeLast();
    m_undoCalibrationButton->setEnabled(!m_workingSamples.isEmpty());
    m_progressLabel->setText(
        QStringLiteral("第 %1/4 点：保持声源稳定并点击视频中的声源位置")
            .arg(m_workingSamples.size() + 1));
    refreshCalibrationDetails();
    refreshCalibrationOverlay();
}

void LiveLocalizationPage::clearCalibration()
{
    m_settings.calibrationSamples.clear();
    m_settings.calibrationRmsPixels = 0.0;
    m_settings.calibrationReferenceVideoSize = {0, 0};
    m_settings.showCalibrationOverlay = false;
    {
        const QSignalBlocker blocker(m_showCalibrationOverlayCheck);
        m_showCalibrationOverlayCheck->setChecked(false);
    }
    m_settings.camera = CameraViewSettings {};
    m_savedCamera = m_settings.camera;
    applyCameraToUi(m_settings.camera);
    updateCameraFromUi(false);
    persistSettings();
    m_rmsLabel->setText(QStringLiteral("RMS 误差：--"));
    m_progressLabel->setText(QStringLiteral("校准数据已清除"));
    refreshCalibrationDetails();
    refreshCalibrationOverlay();
}

void LiveLocalizationPage::acceptCalibrationDirection(
    quint64 requestId, bool valid, VideoCalibrationSample sample,
    const QString &message)
{
    if (!m_calibrating || requestId != m_calibrationRequestId) {
        return;
    }
    if (m_workingSamples.size() >= 4) {
        return;
    }
    if (!valid) {
        m_progressLabel->setText(QStringLiteral("点击被拒绝：%1，请重试第 %2/4 点")
                                     .arg(message)
                                     .arg(m_workingSamples.size() + 1));
        return;
    }
    sample.normalizedVideoPoint = m_pendingCalibrationPoint;
    for (const VideoCalibrationSample &existing : std::as_const(m_workingSamples)) {
        if (std::hypot(existing.direction.azimuthDegrees
                           - sample.direction.azimuthDegrees,
                       existing.direction.elevationDegrees
                           - sample.direction.elevationDegrees) < 2.0
            || QLineF(existing.normalizedVideoPoint,
                      sample.normalizedVideoPoint).length() < 0.02) {
            m_progressLabel->setText(
                QStringLiteral("点击被拒绝：与已有校准点过近，请移动声源"));
            return;
        }
    }
    m_workingSamples.append(sample);
    m_undoCalibrationButton->setEnabled(true);
    refreshCalibrationDetails();
    refreshCalibrationOverlay();
    if (m_workingSamples.size() == 4) {
        finishCalibration();
    } else {
        m_progressLabel->setText(
            QStringLiteral("第 %1/4 点：保持声源稳定并点击视频中的声源位置")
                .arg(m_workingSamples.size() + 1));
    }
}

void LiveLocalizationPage::finishCalibration()
{
    const CameraViewFitResult fitted =
        fitCameraView(m_workingSamples, m_settings.camera, m_videoFrame.size());
    if (!fitted.valid) {
        m_progressLabel->setText(QStringLiteral("校准失败：%1").arg(fitted.error));
        return;
    }
    m_calibrating = false;
    m_settings.camera = fitted.camera;
    m_settings.calibrationSamples = m_workingSamples;
    m_settings.calibrationRmsPixels = fitted.rmsPixels;
    m_settings.calibrationReferenceVideoSize = m_videoFrame.size();
    m_settings.showCalibrationOverlay = false;
    {
        const QSignalBlocker blocker(m_showCalibrationOverlayCheck);
        m_showCalibrationOverlayCheck->setChecked(false);
    }
    m_savedCamera = fitted.camera;
    applyCameraToUi(fitted.camera);
    m_view->setCameraView(fitted.camera);
    m_view->setCalibrationMode(false);
    m_startCalibrationButton->setEnabled(true);
    m_cancelCalibrationButton->setEnabled(false);
    m_undoCalibrationButton->setEnabled(false);
    m_progressLabel->setText(QStringLiteral("四点校准完成"));
    m_rmsLabel->setText(
        QStringLiteral("RMS 误差：%1 px").arg(fitted.rmsPixels, 0, 'f', 1));
    persistSettings();
    rebuildSpatialGrid();
    refreshCalibrationDetails();
    refreshCalibrationOverlay();
}

void LiveLocalizationPage::refreshCalibrationOverlay()
{
    QVector<QPointF> predicted;
    QVector<VideoCalibrationSample> samples;
    if (m_calibrating) {
        samples = m_workingSamples;
    } else if (m_settings.showCalibrationOverlay) {
        samples = m_settings.calibrationSamples;
    }
    predicted.reserve(samples.size());
    for (const VideoCalibrationSample &sample : samples) {
        SoundSourceDirection direction;
        direction.azimuthDegrees = sample.direction.azimuthDegrees;
        direction.elevationDegrees = sample.direction.elevationDegrees;
        predicted.append(projectDirection(direction, m_settings.camera).normalizedPoint);
    }
    m_view->setCalibrationSamples(samples, predicted);
}

void LiveLocalizationPage::refreshCalibrationDetails()
{
    const QVector<VideoCalibrationSample> &samples = m_calibrating
        ? m_workingSamples : m_settings.calibrationSamples;
    for (int row = 0; row < 4; ++row) {
        const bool available = row < samples.size();
        const VideoCalibrationSample sample =
            available ? samples.at(row) : VideoCalibrationSample {};
        const QStringList values {
            QString::number(row + 1),
            available ? QString::number(sample.direction.azimuthDegrees, 'f', 1)
                      : QStringLiteral("--"),
            available ? QString::number(sample.direction.elevationDegrees, 'f', 1)
                      : QStringLiteral("--"),
            available ? QString::number(sample.confidence, 'f', 2)
                      : QStringLiteral("--"),
            available ? QString::number(sample.angularDeviationDegrees, 'f', 1)
                      : QStringLiteral("--"),
            available ? QString::number(sample.normalizedVideoPoint.x(), 'f', 4)
                      : QStringLiteral("--"),
            available ? QString::number(sample.normalizedVideoPoint.y(), 'f', 4)
                      : QStringLiteral("--"),
        };
        for (int column = 0; column < values.size(); ++column) {
            QTableWidgetItem *item = m_calibrationSamplesTable->item(row, column);
            if (!item) {
                item = new QTableWidgetItem;
                item->setTextAlignment(Qt::AlignCenter);
                m_calibrationSamplesTable->setItem(row, column, item);
            }
            item->setText(values.at(column));
        }
    }
}

} // namespace MicArray
