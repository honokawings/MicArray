#include "ui/livelocalizationpage.h"

#include "analysis/calibrationprofile.h"
#include "analysis/cameraviewcalibrator.h"
#include "analysis/directionprojection.h"
#include "analysis/soundsourcelocatorregistry.h"
#include "ui/livelocalizationview.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QSaveFile>
#include <QLineF>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
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

bool matchesLensAspect(const QSize &size, const CameraLensCalibration &lens)
{
    if (size.isEmpty() || !lens.isValid())
    {
        return true;
    }
    const double expected = double(lens.referenceWidth) / lens.referenceHeight;
    return std::abs(double(size.width()) / size.height() / expected - 1.0) < 0.005;
}

} // namespace

LiveLocalizationPage::LiveLocalizationPage(AppSettings *settings, QWidget *parent)
    : QWidget(parent)
    , m_settingsStore(settings)
    , m_worker(new LiveLocalizationWorker)
{
    setObjectName(QStringLiteral("liveLocalizationPage"));
    buildUi();
    m_heatmapExpiryTimer.setSingleShot(true);
    connect(&m_heatmapExpiryTimer, &QTimer::timeout, this, [this] {
        clearReferenceHeatmap();
        m_referenceHeatmapLabel->setText(plHeatmapSelected() ? QStringLiteral("PL 热力图：结果过期，已清除") : QStringLiteral("上位机热力图：结果过期，已清除"));
    });
    connect(&m_plController, &PlLocalizationController::directionsChanged, this,
        [this](const QVector<SoundSourceDirection> &directions) {
            if (m_active && m_settings.enabled && m_settings.localizationSource == QStringLiteral("pl") && !m_calibrating) {
                m_view->setDirections(directions);
                m_resultLabel->setText(directions.isEmpty()
                    ? QStringLiteral("PL：本帧无可信声源")
                    : QStringLiteral("显示方位 %1° / 俯仰 %2°，置信度 %3%")
                        .arg(directions.first().azimuthDegrees, 0, 'f', 2)
                        .arg(directions.first().elevationDegrees, 0, 'f', 2)
                        .arg(directions.first().confidence * 100.0, 0, 'f', 0));
            }
            emit plDirectionsChanged(m_settings.enabled ? directions : QVector<SoundSourceDirection> {});
        });
    connect(&m_plController, &PlLocalizationController::statusChanged, this, [this](const QString &status) {
        m_plResultLabel->setText(status);
        if (m_settings.localizationSource == QStringLiteral("pl")) {
            m_statusLabel->setText(status);
            m_view->setStatusText(status.contains(QStringLiteral("原始方位"))
                ? QStringLiteral("PL 实时定位：按视频标定投影") : status);
        }
        emit plStatusChanged(status);
    });
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
    connect(m_worker, &LiveLocalizationWorker::localizationReady,
            this, &LiveLocalizationPage::acceptHostLocalizationResult);
    connect(m_worker, &LiveLocalizationWorker::spatialResponseReady,
            this, &LiveLocalizationPage::acceptSpatialResponse);
    connect(m_worker, &LiveLocalizationWorker::statusChanged, this,
            [this](const QString &status) {
                if (m_settings.localizationSource == QStringLiteral("host")) {
                    m_statusLabel->setText(status);
                    m_view->setStatusText(status);
                }
            });
    connect(m_worker, &LiveLocalizationWorker::spatialResponseSupported,
            this, [this](bool supported) {
                if (!softwareResponseSelected()) { return; }
                const int heatmap =
                    m_displayModeCombo->findData(plFusionSelected()
                        ? QStringLiteral("fusion") : QStringLiteral("heatmap"));
                if (heatmap >= 0) {
                    m_displayModeCombo->setItemData(
                        heatmap, supported ? QVariant() : QVariant(0), Qt::UserRole - 1);
                }
                if (!supported
                    && m_displayModeCombo->currentData().toString()
                        != QStringLiteral("marker")) {
                    m_displayModeCombo->setCurrentIndex(
                        m_displayModeCombo->findData(QStringLiteral("marker")));
                }
            });
    connect(m_worker, &LiveLocalizationWorker::calibrationSampleReady,
            this, &LiveLocalizationPage::acceptCalibrationDirection);
    connectUi();
    updateLocalizationSource();
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
    updateLocalizationSource();
    const bool processingActive =
        hostProcessingActive();
    m_view->setSpatialResponseActive(processingActive || (m_active && m_settings.enabled && plHeatmapSelected()));
    emit workerActiveRequested(processingActive);
}

void LiveLocalizationPage::setVideoFrame(const QImage &image,
                                         quint32,
                                         quint32)
{
    const bool sizeChanged = image.size() != m_videoFrame.size();
    m_videoFrame = image;
    m_view->setFrame(image);
    m_saveRawFrameButton->setEnabled(!image.isNull());
    if (sizeChanged) {
        m_videoGeometryLabel->setText(image.isNull()
            ? QStringLiteral("原始视频尺寸：等待视频")
            : QStringLiteral("原始视频尺寸：%1 × %2（不含显示黑边）")
                .arg(image.width()).arg(image.height()));
        if (m_settings.camera.lens.enabled
            && !matchesLensAspect(image.size(), m_settings.camera.lens))
        {
            m_settings.camera.lens.enabled = false;
            m_settings.calibrationSamples.clear();
            m_settings.calibrationRmsPixels = 0.0;
            m_rmsLabel->setText(QStringLiteral("RMS 误差：--"));
            applyCameraToUi(m_settings.camera);
            m_view->setCameraView(m_settings.camera);
            m_lensStatusLabel->setText(QStringLiteral("视频宽高比改变，镜头投影已关闭；请核对裁剪方式。"));
            persistSettings();
            startNewProcessingEpoch();
            refreshCalibrationDetails();
            refreshCalibrationOverlay();
        }
        rebuildSpatialGrid();
    }
}

void LiveLocalizationPage::submitAdjustedAudioFrame(const QByteArray &payload,
                                                    quint32 timestampSeconds,
                                                    quint32 timestampMicros)
{
    if (!hostProcessingActive()
        || m_pendingAudioFrames >= MaxPendingAudioFrames) {
        return;
    }
    ++m_pendingAudioFrames;
    emit audioFrameRequested(payload, timestampSeconds, timestampMicros);
}

void LiveLocalizationPage::setPlLocation(const PlLocationResult &location)
{
    m_plController.accept(location);
}

bool LiveLocalizationPage::plHeatmapSelected() const
{
    return m_settings.localizationSource == QStringLiteral("pl")
        && m_settings.plDisplayMode == QStringLiteral("plheatmap");
}

void LiveLocalizationPage::setPlSpatialMap(const PlSpatialMap &map)
{
    const quint16 delta = map.locatorFrameCounter - m_plMapFrameBarrier;
    if (!m_active || !m_settings.enabled || !plHeatmapSelected() || m_calibrating
        || (m_hasPlMapBarrier && (delta == 0 || delta >= 0x8000u))
        || !m_plController.isCurrentFrame(map.locatorFrameCounter, map.sourceMask))
    {
        return;
    }
    const auto response = m_plProjector.project(map, m_settings.camera, m_gridGeneration);
    m_view->setSpatialResponse(response);
    if (response.normalizedValues.isEmpty())
    {
        m_heatmapExpiryTimer.stop();
        m_referenceHeatmapLabel->setText(QStringLiteral("PL 热力图：本帧无可信声源，已清除"));
    }
    else
    {
        m_heatmapExpiryTimer.start(1000);
        m_referenceHeatmapLabel->setText(
            QStringLiteral("PL 热力图：37×37 扫描，8 位响应；定位点来自同帧多峰精化；帧 %1")
                .arg(map.locatorFrameCounter));
    }
}

void LiveLocalizationPage::clearPlLocation()
{
    m_plController.reset();
    // Device disconnect/reset ends both layers' processing epoch. Queued old
    // software responses cannot repopulate the new session's heatmap.
    startNewProcessingEpoch();
}

bool LiveLocalizationPage::hostProcessingActive() const
{
    return m_active && m_settings.enabled && softwareResponseSelected();
}

bool LiveLocalizationPage::plFusionSelected() const
{
    return m_settings.localizationSource == QStringLiteral("pl")
        && m_settings.plDisplayMode == QStringLiteral("fusion");
}

bool LiveLocalizationPage::softwareResponseSelected() const
{
    return m_settings.localizationSource == QStringLiteral("host") || plFusionSelected();
}

void LiveLocalizationPage::clearReferenceHeatmap()
{
    m_heatmapExpiryTimer.stop();
    LiveSpatialResponse empty;
    empty.generation = m_gridGeneration;
    m_view->setSpatialResponse(empty);
    m_referenceHeatmapLabel->setText(plHeatmapSelected() ? QStringLiteral("PL 热力图：等待完整的 37×37 空间响应，需要配套新版 bit") : QStringLiteral("上位机热力图：等待原始阵列音频"));
}

void LiveLocalizationPage::acceptHostLocalizationResult(const LiveLocalizationResult &result)
{
    // The software worker also finds peaks to assess its heatmap. Those peaks
    // must never become markers or calibration truth when PL is selected.
    if (!hostProcessingActive() || m_calibrating
        || m_settings.localizationSource != QStringLiteral("host")
        || result.generation != m_gridGeneration) {
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
}

void LiveLocalizationPage::acceptSpatialResponse(const LiveSpatialResponse &response)
{
    if (!hostProcessingActive() || m_calibrating || response.generation != m_gridGeneration) {
        return;
    }
    m_view->setSpatialResponse(response);
    const bool usable = response.columns > 0 && response.rows > 0
        && response.normalizedValues.size() == response.columns * response.rows
        && response.reliability > 0.0;
    if (usable) {
        m_heatmapExpiryTimer.start(std::max(1000, m_settings.updateIntervalMs * 3));
        m_referenceHeatmapLabel->setText(QStringLiteral("上位机热力图：已更新；定位点仍来自 PL"));
    } else {
        m_heatmapExpiryTimer.stop();
        m_referenceHeatmapLabel->setText(QStringLiteral("上位机热力图：空间响应不足"));
    }
}

void LiveLocalizationPage::updateLocalizationSource()
{
    const bool host = m_settings.localizationSource == QStringLiteral("host");
    const bool software = softwareResponseSelected();
    const QList<QWidget *> widgets = {m_locatorCombo,
        m_spatialResolutionModeCombo, m_neuralPeakEnhancementCheck, m_heatmapOpacitySpin,
        m_heatmapFocusSpin, m_heatmapGridColumnsSpin, m_heatmapGridRowsSpin, m_updateIntervalSpin};
    for (QWidget *widget : widgets)
    {
        const bool visible = software || (plHeatmapSelected()
            && (widget == m_heatmapOpacitySpin || widget == m_heatmapFocusSpin));
        widget->setEnabled(visible);
        qobject_cast<QFormLayout *>(widget->parentWidget()->layout())->setRowVisible(widget, visible);
    }
    m_displayModeCombo->setEnabled(true);
    m_neuralPeakEnhancementStrengthSpin->setEnabled(software && m_settings.neuralPeakEnhancementEnabled);
    qobject_cast<QFormLayout *>(m_neuralPeakEnhancementStrengthSpin->parentWidget()->layout())
        ->setRowVisible(m_neuralPeakEnhancementStrengthSpin, software);
    m_view->setSpatialResponseActive(hostProcessingActive() || (m_active && m_settings.enabled && plHeatmapSelected()));
    {
        const QSignalBlocker blocker(m_displayModeCombo);
        m_displayModeCombo->clear();
        m_displayModeCombo->addItem(QStringLiteral("纯标记"), QStringLiteral("marker"));
        m_displayModeCombo->addItem(host ? QStringLiteral("真实空间响应热力图")
            : QStringLiteral("上位机热力图 + PL 定位"),
            host ? QStringLiteral("heatmap") : QStringLiteral("fusion"));
        if (!host)
        {
            m_displayModeCombo->addItem(QStringLiteral("PL 热力图 + PL 多声源"), QStringLiteral("plheatmap"));
        }
        const QString mode = host ? m_settings.displayMode : m_settings.plDisplayMode;
        m_displayModeCombo->setCurrentIndex(m_displayModeCombo->findData(mode));
        m_view->setDisplayMode(mode);
    }
    m_displayModeCombo->setToolTip(plHeatmapSelected()
        ? QStringLiteral("热力图和多个定位点均由 PL 计算；需要配套新版 bit，PS 继续转发")
        : host
        ? QStringLiteral("热力图由上位机依据原始 PCM 计算")
        : QStringLiteral("融合模式由上位机计算真实空间响应热力图，定位点仍使用 PL LOC3；无需更换 bit"));
    m_locatorCombo->setToolTip(QStringLiteral("软件热力图算法，不改变 PL 上传的定位角度"));
    m_view->setDirections(!host && m_active && m_settings.enabled && !m_calibrating
        ? m_plController.directions() : QVector<SoundSourceDirection> {});
    if (!host)
    {
        const auto directions = m_plController.directions();
        const QString status = !m_settings.enabled
            ? QStringLiteral("实时定位已关闭")
            : directions.isEmpty() ? QStringLiteral("等待新的可信 PL 定位")
                : QStringLiteral("PL 实时定位：按视频标定投影");
        m_view->setStatusText(status);
        m_statusLabel->setText(status);
        m_resultLabel->setText(!m_settings.enabled || directions.isEmpty()
            ? QStringLiteral("PL：暂无可信声源")
            : QStringLiteral("显示方位 %1° / 俯仰 %2°，置信度 %3%")
                .arg(directions.first().azimuthDegrees, 0, 'f', 2)
                .arg(directions.first().elevationDegrees, 0, 'f', 2)
                .arg(directions.first().confidence * 100.0, 0, 'f', 0));
    }
    m_plResultLabel->setVisible(!host);
    m_referenceHeatmapLabel->setVisible(plFusionSelected() || plHeatmapSelected());
    if (!hostProcessingActive()) { clearReferenceHeatmap(); }
    emit plDirectionsChanged(m_settings.enabled ? m_plController.directions() : QVector<SoundSourceDirection> {});
    emit workerActiveRequested(hostProcessingActive());
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
    if (profile.camera.lens.enabled
        && !matchesLensAspect(m_videoFrame.size(), profile.camera.lens))
    {
        if (errorMessage) { *errorMessage = QStringLiteral("视频宽高比与镜头标定不一致，请核对裁剪方式。"); }
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

bool LiveLocalizationPage::loadLensCalibration(const QString &filePath, QString *errorMessage)
{
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) { *errorMessage = message; }
        return false;
    };
    if (m_calibrating) { return fail(QStringLiteral("请先结束四点校准。")); }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) { return fail(file.errorString()); }
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto object = document.object();
    CameraLensCalibration lens;
    if (!document.isObject()
        || object.value(QStringLiteral("format")).toString() != QStringLiteral("MicArrayCameraLens")
        || object.value(QStringLiteral("version")).toInt() != 1
        || !object.value(QStringLiteral("lens")).isObject()
        || !cameraLensFromJson(object.value(QStringLiteral("lens")).toObject(), &lens, errorMessage))
    {
        return fail(QStringLiteral("镜头文件格式或参数无效。"));
    }
    if (!matchesLensAspect(m_videoFrame.size(), lens))
    {
        return fail(QStringLiteral("视频宽高比与镜头标定不一致，请核对裁剪方式。"));
    }
    m_settings.camera.lens = lens;
    // Existing sample RMS belongs to the previous projection, so invalidate it.
    m_settings.calibrationSamples.clear();
    m_settings.calibrationRmsPixels = 0.0;
    m_settings.calibrationReferenceVideoSize = {lens.referenceWidth, lens.referenceHeight};
    m_savedCamera = m_settings.camera;
    applyCameraToUi(m_settings.camera);
    m_view->setCameraView(m_settings.camera);
    m_rmsLabel->setText(QStringLiteral("RMS 误差：--"));
    m_progressLabel->setText(QStringLiteral("镜头参数已载入；安装朝向需实测核对"));
    persistSettings();
    startNewProcessingEpoch();
    refreshCalibrationDetails();
    refreshCalibrationOverlay();
    if (errorMessage) { errorMessage->clear(); }
    return true;
}

bool LiveLocalizationPage::saveRawVideoFrame(const QString &filePath, QString *errorMessage) const
{
    QSaveFile file(filePath);
    if (m_videoFrame.isNull() || !file.open(QIODevice::WriteOnly)
        || !m_videoFrame.save(&file, "PNG") || !file.commit())
    {
        if (errorMessage) { *errorMessage = QStringLiteral("没有视频或原始帧保存失败。"); }
        return false;
    }
    if (errorMessage) { errorMessage->clear(); }
    return true;
}

void LiveLocalizationPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    m_view = new LiveLocalizationView(splitter);
    auto *controlTabs = new QTabWidget(splitter);
    controlTabs->setObjectName(
        QStringLiteral("liveLocalizationControlTabs"));
    controlTabs->setMinimumWidth(520);
    controlTabs->setMaximumWidth(680);

    auto *realtimeTab = new QScrollArea(controlTabs);
    realtimeTab->setObjectName(
        QStringLiteral("liveLocalizationRealtimeTab"));
    realtimeTab->setWidgetResizable(true);
    realtimeTab->setFrameShape(QFrame::NoFrame);
    auto *realtimePanel = new QWidget(realtimeTab);
    auto *realtimePanelLayout = new QVBoxLayout(realtimePanel);

    auto *calibrationTab = new QWidget(controlTabs);
    calibrationTab->setObjectName(
        QStringLiteral("liveLocalizationCalibrationTab"));
    auto *calibrationTabLayout =
        new QVBoxLayout(calibrationTab);

    auto *localizationGroup = new QGroupBox(
        QStringLiteral("实时定位"), realtimePanel);
    auto *localizationLayout = new QFormLayout(localizationGroup);
    m_enabledCheck = new QCheckBox(QStringLiteral("启用实时定位"), localizationGroup);
    m_enabledCheck->setObjectName(QStringLiteral("liveLocalizationEnabledCheck"));
    m_sourceCombo = new QComboBox(localizationGroup);
    m_sourceCombo->setObjectName(QStringLiteral("liveLocalizationSourceCombo"));
    m_sourceCombo->addItem(QStringLiteral("FPGA 定位（PL LOC3）"), QStringLiteral("pl"));
    m_sourceCombo->addItem(QStringLiteral("上位机 SRP-PHAT（参考分析）"), QStringLiteral("host"));
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
    m_neuralPeakEnhancementCheck =
        new QCheckBox(
            QStringLiteral("启用神经网络主峰锐化"),
            localizationGroup);
    m_neuralPeakEnhancementCheck->setObjectName(
        QStringLiteral(
            "liveLocalizationNeuralPeakEnhancementCheck"));
    m_neuralPeakEnhancementStrengthSpin =
        new QDoubleSpinBox(localizationGroup);
    m_neuralPeakEnhancementStrengthSpin->setObjectName(
        QStringLiteral(
            "liveLocalizationNeuralPeakEnhancementStrengthSpin"));
    m_neuralPeakEnhancementStrengthSpin->setRange(
        0.0, 1.0);
    m_neuralPeakEnhancementStrengthSpin->setDecimals(2);
    m_neuralPeakEnhancementStrengthSpin->setSingleStep(
        0.05);
    m_heatmapOpacitySpin = new QDoubleSpinBox(localizationGroup);
    m_heatmapOpacitySpin->setObjectName(QStringLiteral("liveHeatmapOpacitySpin"));
    m_heatmapOpacitySpin->setRange(0.0, 1.0);
    m_heatmapOpacitySpin->setSingleStep(0.05);
    m_heatmapFocusSpin = new QDoubleSpinBox(localizationGroup);
    m_heatmapFocusSpin->setObjectName(
        QStringLiteral("liveHeatmapFocusSpin"));
    m_heatmapFocusSpin->setRange(0.5, 4.0);
    m_heatmapFocusSpin->setDecimals(1);
    m_heatmapFocusSpin->setSingleStep(0.1);
    m_heatmapFocusSpin->setToolTip(
        QStringLiteral(
            "1.0 保持原始响应；数值越大，热力主峰显示越聚焦"));
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
    m_confidenceSpin->setDecimals(2);
    m_confidenceSpin->setSingleStep(0.02);
    m_confidenceSpin->setToolTip(QStringLiteral(
        "综合判断声源是否可信的阈值。数值越低越容易检出弱声源，"
        "但误报风险越高；数值越高则判定越严格。建议从 0.40 开始调节。"));
    m_confidenceSpin->installEventFilter(this);
    localizationLayout->addRow(m_enabledCheck);
    localizationLayout->addRow(QStringLiteral("定位来源"), m_sourceCombo);
    localizationLayout->addRow(QStringLiteral("算法"), m_locatorCombo);
    localizationLayout->addRow(QStringLiteral("显示方式"), m_displayModeCombo);
    localizationLayout->addRow(
        QStringLiteral("空间精度"),
        m_spatialResolutionModeCombo);
    localizationLayout->addRow(QStringLiteral("热力图透明度"), m_heatmapOpacitySpin);
    localizationLayout->addRow(
        QStringLiteral("热力聚焦强度"),
        m_heatmapFocusSpin);
    localizationLayout->addRow(QStringLiteral("方向网格宽度"), m_heatmapGridColumnsSpin);
    localizationLayout->addRow(QStringLiteral("方向网格高度"), m_heatmapGridRowsSpin);
    localizationLayout->addRow(QStringLiteral("更新周期"), m_updateIntervalSpin);
    auto *confidenceLabel = new QLabel(
        QStringLiteral("可信判定阈值"), localizationGroup);
    confidenceLabel->setObjectName(
        QStringLiteral("liveLocalizationConfidenceLabel"));
    confidenceLabel->setBuddy(m_confidenceSpin);
    localizationLayout->addRow(confidenceLabel, m_confidenceSpin);
    localizationLayout->addRow(
        m_neuralPeakEnhancementCheck);
    localizationLayout->addRow(
        QStringLiteral("神经锐化强度"),
        m_neuralPeakEnhancementStrengthSpin);
    m_plResultLabel = new QLabel(QStringLiteral("PL 定位：等待 LOC3 新帧"), localizationGroup);
    m_plResultLabel->setObjectName(QStringLiteral("plLocationResultLabel"));
    m_plResultLabel->setWordWrap(true);
    localizationLayout->addRow(m_plResultLabel);
    m_referenceHeatmapLabel = new QLabel(QStringLiteral("上位机热力图：等待原始阵列音频"), localizationGroup);
    m_referenceHeatmapLabel->setObjectName(QStringLiteral("referenceHeatmapStatusLabel"));
    m_referenceHeatmapLabel->setWordWrap(true);
    localizationLayout->addRow(m_referenceHeatmapLabel);
    realtimePanelLayout->addWidget(localizationGroup);
    realtimePanelLayout->addStretch();
    realtimeTab->setWidget(realtimePanel);

    auto *cameraGroup = new QGroupBox(
        QStringLiteral("视频视角参数"), calibrationTab);
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
    m_lensEnabledCheck = new QCheckBox(QStringLiteral("使用镜头畸变投影"), cameraGroup);
    m_lensEnabledCheck->setObjectName(QStringLiteral("cameraLensEnabledCheck"));
    m_lensEnabledCheck->setToolTip(QStringLiteral("按镜头模型将方向投到原始视频；视频画面保持原样。"));
    m_lensStatusLabel = new QLabel(QStringLiteral("镜头参数：未载入"), cameraGroup);
    m_lensStatusLabel->setObjectName(QStringLiteral("cameraLensStatusLabel"));
    m_lensStatusLabel->setWordWrap(true);
    m_videoGeometryLabel = new QLabel(QStringLiteral("原始视频尺寸：等待视频"), cameraGroup);
    m_videoGeometryLabel->setObjectName(QStringLiteral("rawVideoGeometryLabel"));
    m_importLensButton = new QPushButton(QStringLiteral("载入镜头参数"), cameraGroup);
    m_importLensButton->setObjectName(QStringLiteral("importCameraLensButton"));
    m_saveRawFrameButton = new QPushButton(QStringLiteral("保存原始视频帧"), cameraGroup);
    m_saveRawFrameButton->setObjectName(QStringLiteral("saveRawVideoFrameButton"));
    m_saveRawFrameButton->setEnabled(false);
    auto *lensButtons = new QHBoxLayout;
    lensButtons->addWidget(m_importLensButton);
    lensButtons->addWidget(m_saveRawFrameButton);
    cameraLayout->addRow(m_lensEnabledCheck);
    cameraLayout->addRow(m_lensStatusLabel);
    cameraLayout->addRow(m_videoGeometryLabel);
    cameraLayout->addRow(lensButtons);
    auto *restoreRow = new QHBoxLayout;
    m_restoreSavedButton = new QPushButton(QStringLiteral("恢复已保存"), cameraGroup);
    m_restoreDefaultsButton = new QPushButton(QStringLiteral("恢复默认"), cameraGroup);
    restoreRow->addWidget(m_restoreSavedButton);
    restoreRow->addWidget(m_restoreDefaultsButton);
    cameraLayout->addRow(restoreRow);
    calibrationTabLayout->addWidget(cameraGroup);

    auto *calibrationGroup = new QGroupBox(
        QStringLiteral("四点视频校准"), calibrationTab);
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
        QHeaderView::Stretch);
    m_calibrationSamplesTable->setVerticalScrollBarPolicy(
        Qt::ScrollBarAlwaysOff);
    m_calibrationSamplesTable->setHorizontalScrollBarPolicy(
        Qt::ScrollBarAlwaysOff);
    updateCalibrationTableHeight();
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
    calibrationTabLayout->addWidget(calibrationGroup, 1);
    controlTabs->addTab(
        realtimeTab, QStringLiteral("实时定位"));
    controlTabs->addTab(
        calibrationTab, QStringLiteral("视角校准"));
    splitter->addWidget(m_view);
    splitter->addWidget(controlTabs);
    splitter->setStretchFactor(0, 1);
    root->addWidget(splitter, 1);
    m_resultLabel = new QLabel(QStringLiteral("方位角 --　俯仰角 --"), this);
    m_resultLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel = new QLabel(QStringLiteral("等待实时音视频"), this);
    root->addWidget(m_resultLabel);
    root->addWidget(m_statusLabel);
}

void LiveLocalizationPage::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (m_calibrationSamplesTable == nullptr) {
        return;
    }
    if (event->type() == QEvent::StyleChange
        || event->type() == QEvent::FontChange
        || event->type() == QEvent::ApplicationFontChange
        || event->type() == QEvent::Polish) {
        QTimer::singleShot(
            0, this,
            &LiveLocalizationPage::updateCalibrationTableHeight);
    }
}

bool LiveLocalizationPage::eventFilter(
    QObject *watched, QEvent *event)
{
    if (watched == m_confidenceSpin
        && event->type() == QEvent::Wheel) {
        event->accept();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void LiveLocalizationPage::updateCalibrationTableHeight()
{
    if (m_calibrationSamplesTable == nullptr) {
        return;
    }
    m_calibrationSamplesTable->ensurePolished();
    m_calibrationSamplesTable->resizeRowsToContents();
    int rowsHeight = 0;
    for (int row = 0;
         row < m_calibrationSamplesTable->rowCount();
         ++row) {
        rowsHeight += m_calibrationSamplesTable->rowHeight(row);
    }
    const int headerHeight = std::max(
        m_calibrationSamplesTable->horizontalHeader()->height(),
        m_calibrationSamplesTable->horizontalHeader()
            ->sizeHint().height());
    const int requiredHeight = headerHeight + rowsHeight
        + 2 * m_calibrationSamplesTable->frameWidth() + 2;
    if (m_calibrationSamplesTable->minimumHeight()
        != requiredHeight) {
        m_calibrationSamplesTable->setMinimumHeight(
            requiredHeight);
    }
}

void LiveLocalizationPage::connectUi()
{
    connect(m_lensEnabledCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (m_calibrating)
        {
            const QSignalBlocker blocker(m_lensEnabledCheck);
            m_lensEnabledCheck->setChecked(m_settings.camera.lens.enabled);
            return;
        }
        if (enabled && !matchesLensAspect(m_videoFrame.size(), m_settings.camera.lens))
        {
            const QSignalBlocker blocker(m_lensEnabledCheck);
            m_lensEnabledCheck->setChecked(false);
            m_lensStatusLabel->setText(QStringLiteral("视频宽高比不匹配，无法启用镜头投影。"));
            return;
        }
        m_settings.camera.lens.enabled = enabled;
        m_settings.calibrationSamples.clear();
        m_settings.calibrationRmsPixels = 0.0;
        m_rmsLabel->setText(QStringLiteral("RMS 误差：--"));
        applyCameraToUi(m_settings.camera);
        updateCameraFromUi(true);
        startNewProcessingEpoch();
        refreshCalibrationDetails();
    });
    connect(m_importLensButton, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("载入镜头参数"), {},
                                                      QStringLiteral("JSON 镜头参数 (*.json)"));
        if (path.isEmpty()) { return; }
        QString error;
        m_statusLabel->setText(loadLensCalibration(path, &error)
            ? QStringLiteral("镜头参数已载入，请进行实际声源验证") : error);
    });
    connect(m_saveRawFrameButton, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("保存原始视频帧"),
            QStringLiteral("camera-raw.png"), QStringLiteral("PNG 图像 (*.png)"));
        if (path.isEmpty()) { return; }
        QString error;
        m_statusLabel->setText(saveRawVideoFrame(path, &error)
            ? QStringLiteral("原始帧已保存：%1 × %2").arg(m_videoFrame.width()).arg(m_videoFrame.height())
            : error);
    });
    connect(m_sourceCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_calibrating) { cancelCalibration(); }
        m_settings.localizationSource = m_sourceCombo->itemData(index).toString();
        startNewProcessingEpoch();
        updateLocalizationSource();
        persistSettings();
    });
    connect(m_enabledCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        m_settings.enabled = enabled;
        startNewProcessingEpoch();
        m_view->setSpatialResponseActive(
            hostProcessingActive());
        persistSettings();
        emit workerActiveRequested(hostProcessingActive());
        updateLocalizationSource();
    });
    connect(m_locatorCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_settings.locatorId = m_locatorCombo->itemData(index).toString();
        startNewProcessingEpoch();
        persistSettings();
        emit configureWorkerRequested(m_settings);
    });
    connect(m_displayModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString mode = m_displayModeCombo->itemData(index).toString();
        if (m_settings.localizationSource == QStringLiteral("pl")) {
            m_settings.plDisplayMode = mode;
        } else {
            m_settings.displayMode = mode;
        }
        startNewProcessingEpoch();
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
    connect(
        m_neuralPeakEnhancementCheck,
        &QCheckBox::toggled,
        this,
        [this](bool enabled) {
        m_settings.neuralPeakEnhancementEnabled =
            enabled;
        m_neuralPeakEnhancementStrengthSpin
            ->setEnabled(enabled);
        startNewProcessingEpoch();
        persistSettings();
        emit configureWorkerRequested(m_settings);
    });
    connect(
        m_neuralPeakEnhancementStrengthSpin,
        &QDoubleSpinBox::valueChanged,
        this,
        [this](double value) {
        m_settings.neuralPeakEnhancementStrength =
            value;
        startNewProcessingEpoch();
        persistSettings();
        emit configureWorkerRequested(m_settings);
    });
    connect(m_heatmapOpacitySpin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        m_settings.heatmapOpacity = value;
        m_view->setHeatmapOpacity(value);
        persistSettings();
    });
    connect(
        m_heatmapFocusSpin,
        &QDoubleSpinBox::valueChanged,
        this,
        [this](double value) {
        m_settings.heatmapFocusExponent = value;
        m_view->setHeatmapFocusExponent(value);
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
                const quint64 request = ++m_calibrationRequestId;
                if (m_settings.localizationSource == QStringLiteral("pl")) {
                    VideoCalibrationSample sample;
                    QString error;
                    const bool valid = m_plController.stableCalibrationSample(&sample, &error);
                    QTimer::singleShot(0, this, [this, request, valid, sample, error] {
                        acceptCalibrationDirection(request, valid, sample, error);
                    });
                } else {
                    emit calibrationSampleRequested(request);
                }
                m_progressLabel->setText(QStringLiteral("正在评估第 %1/4 点的声源稳定性…")
                                             .arg(m_workingSamples.size() + 1));
            });
}

void LiveLocalizationPage::restoreSettings()
{
    m_settings = m_settingsStore != nullptr
        ? m_settingsStore->liveLocalization() : LiveLocalizationSettings {};
    m_savedCamera = m_settings.camera;
    m_sourceCombo->setCurrentIndex(std::max(0, m_sourceCombo->findData(m_settings.localizationSource)));
    m_plController.setConfidenceThreshold(m_settings.confidenceThreshold);
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
    m_heatmapFocusSpin->setValue(
        m_settings.heatmapFocusExponent);
    m_neuralPeakEnhancementCheck->setChecked(
        m_settings.neuralPeakEnhancementEnabled);
    m_neuralPeakEnhancementStrengthSpin->setValue(
        m_settings.neuralPeakEnhancementStrength);
    m_neuralPeakEnhancementStrengthSpin->setEnabled(
        m_settings.neuralPeakEnhancementEnabled);
    m_heatmapGridColumnsSpin->setValue(m_settings.heatmapGridColumns);
    m_heatmapGridRowsSpin->setValue(m_settings.heatmapGridRows);
    m_updateIntervalSpin->setValue(m_settings.updateIntervalMs);
    m_confidenceSpin->setValue(m_settings.confidenceThreshold);
    m_showCalibrationOverlayCheck->setChecked(m_settings.showCalibrationOverlay);
    applyCameraToUi(m_settings.camera);
    m_view->setCameraView(m_settings.camera);
    m_view->setDisplayMode(m_settings.displayMode);
    m_view->setHeatmapOpacity(m_settings.heatmapOpacity);
    m_view->setHeatmapFocusExponent(
        m_settings.heatmapFocusExponent);
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
    const QSignalBlocker lensBlocker(m_lensEnabledCheck);
    m_settings.camera.lens = camera.lens;
    m_lensEnabledCheck->setEnabled(camera.lens.isValid());
    m_lensEnabledCheck->setChecked(camera.lens.enabled);
    m_horizontalFovSpin->setEnabled(!camera.lens.enabled);
    m_verticalFovSpin->setEnabled(!camera.lens.enabled);
    m_lensStatusLabel->setText(camera.lens.isValid()
        ? QStringLiteral("镜头模型：鱼眼；参考尺寸 %1 × %2；安装朝向需实测核对")
            .arg(camera.lens.referenceWidth).arg(camera.lens.referenceHeight)
        : QStringLiteral("镜头参数：未载入"));
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
    emit plDirectionsChanged(m_settings.enabled ? m_plController.directions() : QVector<SoundSourceDirection> {});
    if (persist) {
        m_savedCamera = m_settings.camera;
        persistSettings();
    }
}

void LiveLocalizationPage::startNewProcessingEpoch()
{
    m_hasPlMapBarrier = m_plController.hasObservation();
    m_plMapFrameBarrier = m_plController.currentFrame();
    emit workerActiveRequested(false);
    m_heatmapExpiryTimer.stop();
    m_plController.setConfidenceThreshold(m_settings.confidenceThreshold);
    m_view->setDirections({});
    clearReferenceHeatmap();
    m_resultLabel->setText(
        QStringLiteral("方位角 --　俯仰角 --"));
    rebuildSpatialGrid();
    updateLocalizationSource();
}

void LiveLocalizationPage::rebuildSpatialGrid()
{
    const quint64 generation = ++m_gridGeneration;
    m_view->setExpectedSpatialResponseGeneration(generation);
    SpatialResponseGrid grid;
    if (m_videoFrame.isNull() || !softwareResponseSelected()) {
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
    clearReferenceHeatmap();
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
