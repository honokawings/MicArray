#include "ui/noisereductionpage.h"

#include "analysis/directionprojection.h"
#include "control/directioncontrolmapper.h"
#include "ui/audiowaveformwidget.h"
#include "ui/noisereductionvideowidget.h"
#include "ui/powerspectrumwidget.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>

namespace MicArray {
namespace {

void addNoiseSources(QComboBox *combo)
{
    for (NoiseSignalSource source : {
             NoiseSignalSource::FirstElement,
             NoiseSignalSource::EnhancedA,
             NoiseSignalSource::SuppressionB1,
             NoiseSignalSource::SuppressionB2,
             NoiseSignalSource::Denoised,
         }) {
        combo->addItem(noiseSignalSourceName(source),
                       static_cast<int>(source));
    }
}

QDoubleSpinBox *angleSpin(
    QWidget *parent,
    const QString &objectName,
    double minimum,
    double maximum)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setObjectName(objectName);
    spin->setRange(minimum, maximum);
    spin->setDecimals(1);
    spin->setSingleStep(1.0);
    spin->setSuffix(QStringLiteral("°"));
    return spin;
}

QDoubleSpinBox *coefficientSpin(
    QWidget *parent, const QString &objectName)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setObjectName(objectName);
    spin->setRange(0.0, 100.0);
    spin->setDecimals(2);
    spin->setSingleStep(0.25);
    return spin;
}

CameraViewSettings projectionSafeCamera(CameraViewSettings camera)
{
    camera.horizontalFovDegrees =
        std::min(camera.horizontalFovDegrees, 179.0);
    camera.verticalFovDegrees =
        std::min(camera.verticalFovDegrees, 179.0);
    return camera;
}

void selectData(QComboBox *combo, int value)
{
    const int index = combo->findData(value);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    }
}

} // namespace

NoiseReductionPage::NoiseReductionPage(
    AppSettings *settings, QWidget *parent)
    : QWidget(parent)
    , m_settingsStore(settings)
    , m_worker(new NoiseReductionWorker)
{
    setObjectName(QStringLiteral("noiseReductionPage"));
    qRegisterMetaType<ControlValues>("MicArray::ControlValues");
    buildUi();
    buildPresentationWindow();
    restoreSettings();
    connectUi();

    m_workerThread.setObjectName(QStringLiteral("NoiseReductionThread"));
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished,
            m_worker, &QObject::deleteLater);
    connect(this, &NoiseReductionPage::configureWorkerRequested,
            m_worker, &NoiseReductionWorker::configure, Qt::QueuedConnection);
    connect(this, &NoiseReductionPage::workerActiveRequested,
            m_worker, &NoiseReductionWorker::setActive, Qt::QueuedConnection);
    connect(this, &NoiseReductionPage::rawAudioFrameRequested,
            m_worker, &NoiseReductionWorker::processRawFrame,
            Qt::QueuedConnection);
    connect(m_worker, &NoiseReductionWorker::frameConsumed, this, [this] {
        m_pendingAudioFrames = std::max(0, m_pendingAudioFrames - 1);
        setProperty("pendingAudioFrames", m_pendingAudioFrames);
    });
    connect(m_worker, &NoiseReductionWorker::plotFrameReady,
            this, &NoiseReductionPage::showPlotFrame);
    connect(m_worker, &NoiseReductionWorker::warningRaised, this,
            [this](const QString &message) {
                m_statusLabel->setText(message);
            });
    connect(m_worker, &NoiseReductionWorker::statusChanged, this,
            [this](const QString &message) {
                m_statusLabel->setText(message);
            });
    m_workerThread.start();
    emit configureWorkerRequested(m_settings, m_sampleRate);
    emit workerActiveRequested(false);
    setProperty("pendingAudioFrames", 0);
    setProperty("droppedAudioFrames", qulonglong(0));
    setProperty("submittedAudioFrames", qulonglong(0));
}

NoiseReductionPage::~NoiseReductionPage()
{
    if (m_presentationWindow != nullptr) {
        m_presentationWindow->removeEventFilter(this);
        m_presentationWindow->hide();
        delete m_presentationWindow;
        m_presentationWindow = nullptr;
        m_presentationVideo = nullptr;
        m_presentationFirstSpectrum = nullptr;
        m_presentationSecondSpectrum = nullptr;
    }
    if (m_settingsStore != nullptr) {
        m_settingsStore->saveNoiseReduction(m_settings);
    }
    if (m_workerThread.isRunning()) {
        QMetaObject::invokeMethod(
            m_worker, "shutdown", Qt::BlockingQueuedConnection);
        m_workerThread.quit();
        m_workerThread.wait(3000);
    }
}

void NoiseReductionPage::setActive(bool active)
{
    m_active = active;
    emit workerActiveRequested(active && m_settings.enabled);
    if (!active) {
        setPresentationMode(false);
        m_firstWaveform->clear();
        m_secondWaveform->clear();
        m_firstSpectrum->clear();
        m_secondSpectrum->clear();
    }
}

void NoiseReductionPage::setConnected(bool connected)
{
    m_connected = connected;
    refreshControlAvailability();
}

void NoiseReductionPage::setControlLocked(bool locked)
{
    m_controlLocked = locked;
    refreshControlAvailability();
}

void NoiseReductionPage::setSampleRate(int sampleRate)
{
    m_sampleRate = std::clamp(sampleRate, 8000, 192000);
    emit configureWorkerRequested(m_settings, m_sampleRate);
}

void NoiseReductionPage::setVideoFrame(
    const QImage &image, quint32, quint32)
{
    m_videoFrame = image;
    m_videoView->setFrame(image);
    m_presentationVideo->setFrame(image);
}

void NoiseReductionPage::submitRawAudioFrame(
    const QByteArray &payload,
    quint32 timestampSeconds,
    quint32 timestampMicros)
{
    if (!m_active || !m_settings.enabled) {
        return;
    }
    if (m_pendingAudioFrames >= MaximumPendingFrames) {
        ++m_droppedAudioFrames;
        setProperty("droppedAudioFrames",
                    QVariant::fromValue(m_droppedAudioFrames));
        return;
    }
    ++m_pendingAudioFrames;
    ++m_submittedAudioFrames;
    setProperty("pendingAudioFrames", m_pendingAudioFrames);
    setProperty("submittedAudioFrames",
                QVariant::fromValue(m_submittedAudioFrames));
    emit rawAudioFrameRequested(payload,
                                timestampSeconds,
                                timestampMicros);
}

void NoiseReductionPage::setControlSendResult(
    bool success, const QString &message)
{
    m_statusLabel->setText(
        success ? QStringLiteral("方向控制已发送：%1").arg(message) : message);
}

void NoiseReductionPage::showPresentation()
{
    setPresentationMode(true);
}

ControlValues NoiseReductionPage::currentControlValues() const
{
    return DirectionControlMapper::controlValues(
        m_settings.enhancedA,
        m_settings.suppressionB1,
        m_settings.suppressionB2,
        m_settings.mapping);
}

bool NoiseReductionPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Wheel) {
        for (QDoubleSpinBox *spin : m_directionSpins) {
            if (watched == spin) {
                event->accept();
                return true;
            }
        }
        for (QDoubleSpinBox *spin : m_mappingSpins) {
            if (watched == spin) {
                event->accept();
                return true;
            }
        }
        for (QDoubleSpinBox *spin : m_cameraSpins) {
            if (watched == spin) {
                event->accept();
                return true;
            }
        }
    }
    if (watched == m_presentationWindow) {
        if (event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            setPresentationMode(false);
            return true;
        }
        if (event->type() == QEvent::Close) {
            setPresentationMode(false);
            event->ignore();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void NoiseReductionPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    auto *verticalSplitter = new QSplitter(Qt::Vertical, this);
    verticalSplitter->setChildrenCollapsible(false);
    root->addWidget(verticalSplitter);

    auto *topSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);
    topSplitter->setChildrenCollapsible(false);
    m_videoView = new NoiseReductionVideoWidget(topSplitter);
    topSplitter->addWidget(m_videoView);

    auto *scroll = new QScrollArea(topSplitter);
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(320);
    scroll->setMaximumWidth(430);
    auto *panel = new QWidget(scroll);
    auto *panelLayout = new QVBoxLayout(panel);

    auto *processingGroup =
        new QGroupBox(QStringLiteral("降噪处理"), panel);
    auto *processingLayout = new QFormLayout(processingGroup);
    m_enabledCheck =
        new QCheckBox(QStringLiteral("启用降噪处理"), processingGroup);
    m_enabledCheck->setObjectName(
        QStringLiteral("noiseReductionEnabledCheck"));
    m_algorithmCombo = new QComboBox(processingGroup);
    m_algorithmCombo->setObjectName(
        QStringLiteral("noiseReductionAlgorithmCombo"));
    m_algorithmCombo->addItem(
        QStringLiteral("固定参数维纳"),
        static_cast<int>(NoiseReductionAlgorithm::FixedParameterWiener));
    m_algorithmCombo->addItem(
        QStringLiteral("自适应维纳"),
        static_cast<int>(NoiseReductionAlgorithm::AdaptiveWiener));
    m_modeCombo = new QComboBox(processingGroup);
    m_modeCombo->setObjectName(QStringLiteral("noiseReductionModeCombo"));
    m_modeCombo->addItem(QStringLiteral("单点抑制"),
                         static_cast<int>(NoiseReductionMode::SinglePoint));
    m_modeCombo->addItem(QStringLiteral("两点抑制"),
                         static_cast<int>(NoiseReductionMode::TwoPoint));
    m_singleCoefficientSpin = coefficientSpin(
        processingGroup,
        QStringLiteral("noiseReductionSingleCoefficientSpin"));
    m_dualB1CoefficientSpin = coefficientSpin(
        processingGroup,
        QStringLiteral("noiseReductionDualB1CoefficientSpin"));
    m_dualB2CoefficientSpin = coefficientSpin(
        processingGroup,
        QStringLiteral("noiseReductionDualB2CoefficientSpin"));
    m_statusLabel = new QLabel(QStringLiteral("降噪处理已关闭"), processingGroup);
    m_statusLabel->setObjectName(QStringLiteral("noiseReductionStatusLabel"));
    m_statusLabel->setWordWrap(true);
    processingLayout->addRow(m_enabledCheck);
    processingLayout->addRow(QStringLiteral("降噪算法"), m_algorithmCombo);
    processingLayout->addRow(QStringLiteral("处理模式"), m_modeCombo);
    processingLayout->addRow(QStringLiteral("单点抑制系数"),
                             m_singleCoefficientSpin);
    processingLayout->addRow(QStringLiteral("双点 B1 系数"),
                             m_dualB1CoefficientSpin);
    processingLayout->addRow(QStringLiteral("双点 B2 系数"),
                             m_dualB2CoefficientSpin);
    processingLayout->addRow(QStringLiteral("状态"), m_statusLabel);
    panelLayout->addWidget(processingGroup);

    auto *monitorGroup = new QGroupBox(QStringLiteral("监听"), panel);
    auto *monitorLayout = new QFormLayout(monitorGroup);
    m_monitorEnabledCheck =
        new QCheckBox(QStringLiteral("启用监听输出"), monitorGroup);
    m_monitorEnabledCheck->setObjectName(
        QStringLiteral("noiseReductionMonitorEnabledCheck"));
    m_monitorSourceCombo = new QComboBox(monitorGroup);
    m_monitorSourceCombo->setObjectName(
        QStringLiteral("noiseReductionMonitorSourceCombo"));
    addNoiseSources(m_monitorSourceCombo);
    m_monitorGainSpin = new QDoubleSpinBox(monitorGroup);
    m_monitorGainSpin->setObjectName(
        QStringLiteral("noiseReductionMonitorGainSpin"));
    m_monitorGainSpin->setRange(-60.0, 24.0);
    m_monitorGainSpin->setDecimals(1);
    m_monitorGainSpin->setSuffix(QStringLiteral(" dB"));
    monitorLayout->addRow(m_monitorEnabledCheck);
    monitorLayout->addRow(QStringLiteral("监听源"), m_monitorSourceCombo);
    monitorLayout->addRow(QStringLiteral("监听增益"), m_monitorGainSpin);
    panelLayout->addWidget(monitorGroup);

    auto *bandPassGroup =
        new QGroupBox(QStringLiteral("输出带通滤波"), panel);
    auto *bandPassLayout = new QFormLayout(bandPassGroup);
    m_bandPassEnabledCheck =
        new QCheckBox(QStringLiteral("启用带通滤波"), bandPassGroup);
    m_bandPassEnabledCheck->setObjectName(
        QStringLiteral("noiseReductionBandPassEnabledCheck"));
    m_bandPassPresetCombo = new QComboBox(bandPassGroup);
    m_bandPassPresetCombo->setObjectName(
        QStringLiteral("noiseReductionBandPassPresetCombo"));
    m_bandPassPresetCombo->addItem(
        QStringLiteral("人声（100–8000 Hz）"),
        static_cast<int>(BandPassPreset::Voice));
    m_bandPassPresetCombo->addItem(
        QStringLiteral("音乐（40–16000 Hz）"),
        static_cast<int>(BandPassPreset::Music));
    m_bandPassPresetCombo->addItem(
        QStringLiteral("自定义"),
        static_cast<int>(BandPassPreset::Custom));
    m_bandPassLowSpin = new QDoubleSpinBox(bandPassGroup);
    m_bandPassLowSpin->setObjectName(
        QStringLiteral("noiseReductionBandPassLowSpin"));
    m_bandPassLowSpin->setRange(1.0, 95999.0);
    m_bandPassLowSpin->setDecimals(0);
    m_bandPassLowSpin->setSingleStep(10.0);
    m_bandPassLowSpin->setSuffix(QStringLiteral(" Hz"));
    m_bandPassHighSpin = new QDoubleSpinBox(bandPassGroup);
    m_bandPassHighSpin->setObjectName(
        QStringLiteral("noiseReductionBandPassHighSpin"));
    m_bandPassHighSpin->setRange(2.0, 96000.0);
    m_bandPassHighSpin->setDecimals(0);
    m_bandPassHighSpin->setSingleStep(100.0);
    m_bandPassHighSpin->setSuffix(QStringLiteral(" Hz"));
    bandPassLayout->addRow(m_bandPassEnabledCheck);
    bandPassLayout->addRow(QStringLiteral("参数预设"),
                           m_bandPassPresetCombo);
    bandPassLayout->addRow(QStringLiteral("下限频率"),
                           m_bandPassLowSpin);
    bandPassLayout->addRow(QStringLiteral("上限频率"),
                           m_bandPassHighSpin);
    panelLayout->addWidget(bandPassGroup);

    auto *directionGroup =
        new QGroupBox(QStringLiteral("方向控制"), panel);
    auto *directionLayout = new QFormLayout(directionGroup);
    m_directionSelectionCombo = new QComboBox(directionGroup);
    m_directionSelectionCombo->setObjectName(
        QStringLiteral("noiseDirectionTargetCombo"));
    m_directionSelectionCombo->addItem(
        QStringLiteral("增强 A"),
        static_cast<int>(NoiseDirectionSelection::EnhancedA));
    m_directionSelectionCombo->addItem(
        QStringLiteral("抑制 B1"),
        static_cast<int>(NoiseDirectionSelection::SuppressionB1));
    m_directionSelectionCombo->addItem(
        QStringLiteral("抑制 B2"),
        static_cast<int>(NoiseDirectionSelection::SuppressionB2));
    directionLayout->addRow(QStringLiteral("当前目标"),
                            m_directionSelectionCombo);
    const QStringList directionNames {
        QStringLiteral("A 方位"), QStringLiteral("A 俯仰"),
        QStringLiteral("B1 方位"), QStringLiteral("B1 俯仰"),
        QStringLiteral("B2 方位"), QStringLiteral("B2 俯仰"),
    };
    const QStringList objectNames {
        QStringLiteral("noiseDirectionAAzimuthDegreesSpin"),
        QStringLiteral("noiseDirectionAElevationDegreesSpin"),
        QStringLiteral("noiseDirectionB1AzimuthDegreesSpin"),
        QStringLiteral("noiseDirectionB1ElevationDegreesSpin"),
        QStringLiteral("noiseDirectionB2AzimuthDegreesSpin"),
        QStringLiteral("noiseDirectionB2ElevationDegreesSpin"),
    };
    const QStringList codeObjectNames {
        QStringLiteral("noiseDirectionAAzimuthCodeLabel"),
        QStringLiteral("noiseDirectionAElevationCodeLabel"),
        QStringLiteral("noiseDirectionB1AzimuthCodeLabel"),
        QStringLiteral("noiseDirectionB1ElevationCodeLabel"),
        QStringLiteral("noiseDirectionB2AzimuthCodeLabel"),
        QStringLiteral("noiseDirectionB2ElevationCodeLabel"),
    };
    for (int index = 0; index < 6; ++index) {
        m_directionSpins[index] = new QDoubleSpinBox(directionGroup);
        m_directionSpins[index]->setObjectName(objectNames.at(index));
        m_directionSpins[index]->setRange(
            index % 2 == 0 ? -180.0 : -90.0,
            index % 2 == 0 ? 180.0 : 90.0);
        m_directionSpins[index]->setDecimals(3);
        m_directionSpins[index]->setSingleStep(0.1);
        m_directionSpins[index]->setSuffix(QStringLiteral("°"));
        m_directionSpins[index]->installEventFilter(this);
        m_directionCodeLabels[index] = new QLabel(
            QStringLiteral("0"), directionGroup);
        m_directionCodeLabels[index]->setObjectName(
            codeObjectNames.at(index));
        m_directionCodeLabels[index]->setAlignment(
            Qt::AlignRight | Qt::AlignVCenter);
        m_directionCodeLabels[index]->setMinimumWidth(24);
        auto *row = new QWidget(directionGroup);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->addWidget(m_directionSpins[index], 1);
        rowLayout->addWidget(new QLabel(
            QStringLiteral("接口码"), row));
        rowLayout->addWidget(m_directionCodeLabels[index]);
        directionLayout->addRow(directionNames.at(index), row);
    }
    m_sendDirectionButton =
        new QPushButton(QStringLiteral("发送当前方向"), directionGroup);
    m_sendDirectionButton->setObjectName(
        QStringLiteral("noiseReductionSendDirectionButton"));
    m_showMarkersCheck =
        new QCheckBox(QStringLiteral("显示方向标记"), directionGroup);
    m_showMarkersCheck->setObjectName(
        QStringLiteral("noiseReductionShowMarkersCheck"));
    directionLayout->addRow(m_sendDirectionButton);
    directionLayout->addRow(m_showMarkersCheck);
    panelLayout->addWidget(directionGroup);

    auto *mappingGroup =
        new QGroupBox(QStringLiteral("实际角度映射"), panel);
    auto *mappingLayout = new QFormLayout(mappingGroup);
    m_mappingSpins[0] = angleSpin(
        mappingGroup, QStringLiteral("noiseAzimuthMinimumSpin"),
        -180.0, 179.9);
    m_mappingSpins[1] = angleSpin(
        mappingGroup, QStringLiteral("noiseAzimuthMaximumSpin"),
        -179.9, 180.0);
    m_mappingSpins[2] = angleSpin(
        mappingGroup, QStringLiteral("noiseElevationMinimumSpin"),
        -90.0, 89.9);
    m_mappingSpins[3] = angleSpin(
        mappingGroup, QStringLiteral("noiseElevationMaximumSpin"),
        -89.9, 90.0);
    for (QDoubleSpinBox *spin : m_mappingSpins) {
        spin->installEventFilter(this);
    }
    mappingLayout->addRow(QStringLiteral("方位最小"), m_mappingSpins[0]);
    mappingLayout->addRow(QStringLiteral("方位最大"), m_mappingSpins[1]);
    mappingLayout->addRow(QStringLiteral("俯仰最小"), m_mappingSpins[2]);
    mappingLayout->addRow(QStringLiteral("俯仰最大"), m_mappingSpins[3]);
    panelLayout->addWidget(mappingGroup);

    auto *cameraGroup =
        new QGroupBox(QStringLiteral("视频视角参数"), panel);
    auto *cameraLayout = new QFormLayout(cameraGroup);
    m_cameraSpins[0] = angleSpin(
        cameraGroup, QStringLiteral("noiseCameraCenterAzimuthSpin"),
        -180.0, 180.0);
    m_cameraSpins[1] = angleSpin(
        cameraGroup, QStringLiteral("noiseCameraCenterElevationSpin"),
        -89.0, 89.0);
    m_cameraSpins[2] = angleSpin(
        cameraGroup, QStringLiteral("noiseCameraHorizontalFovSpin"),
        1.1, 180.0);
    m_cameraSpins[3] = angleSpin(
        cameraGroup, QStringLiteral("noiseCameraVerticalFovSpin"),
        1.1, 180.0);
    m_cameraSpins[4] = angleSpin(
        cameraGroup, QStringLiteral("noiseCameraRollSpin"),
        -180.0, 180.0);
    for (QDoubleSpinBox *spin : m_cameraSpins) {
        spin->installEventFilter(this);
    }
    cameraLayout->addRow(QStringLiteral("中心方位"), m_cameraSpins[0]);
    cameraLayout->addRow(QStringLiteral("中心俯仰"), m_cameraSpins[1]);
    cameraLayout->addRow(QStringLiteral("水平视场角"), m_cameraSpins[2]);
    cameraLayout->addRow(QStringLiteral("垂直视场角"), m_cameraSpins[3]);
    cameraLayout->addRow(QStringLiteral("滚转角"), m_cameraSpins[4]);
    panelLayout->addWidget(cameraGroup);

    m_fullscreenButton =
        new QPushButton(QStringLiteral("全屏显示"), panel);
    m_fullscreenButton->setObjectName(
        QStringLiteral("noiseReductionFullscreenButton"));
    panelLayout->addWidget(m_fullscreenButton);
    panelLayout->addStretch();
    scroll->setWidget(panel);
    topSplitter->addWidget(scroll);
    topSplitter->setSizes({800, 360});
    verticalSplitter->addWidget(topSplitter);

    auto *comparisons = new QWidget(verticalSplitter);
    auto *comparisonLayout = new QHBoxLayout(comparisons);
    comparisonLayout->setContentsMargins(0, 0, 0, 0);
    auto buildComparison = [&](const QString &title,
                               const QString &comboName,
                               QComboBox **combo,
                               AudioWaveformWidget **waveform,
                               PowerSpectrumWidget **spectrum) {
        auto *group = new QGroupBox(title, comparisons);
        auto *layout = new QVBoxLayout(group);
        *combo = new QComboBox(group);
        (*combo)->setObjectName(comboName);
        addNoiseSources(*combo);
        *waveform = new AudioWaveformWidget(group);
        (*waveform)->setMaximumHeight(120);
        *spectrum = new PowerSpectrumWidget(group);
        layout->addWidget(*combo);
        layout->addWidget(*waveform);
        layout->addWidget(*spectrum, 1);
        comparisonLayout->addWidget(group);
    };
    buildComparison(QStringLiteral("对比源 1"),
                    QStringLiteral("noisePlotSource1Combo"),
                    &m_firstPlotSourceCombo,
                    &m_firstWaveform,
                    &m_firstSpectrum);
    buildComparison(QStringLiteral("对比源 2"),
                    QStringLiteral("noisePlotSource2Combo"),
                    &m_secondPlotSourceCombo,
                    &m_secondWaveform,
                    &m_secondSpectrum);
    verticalSplitter->addWidget(comparisons);
    verticalSplitter->setSizes({460, 280});
}

void NoiseReductionPage::buildPresentationWindow()
{
    m_presentationWindow =
        new QWidget(this, Qt::Window | Qt::FramelessWindowHint);
    m_presentationWindow->setObjectName(
        QStringLiteral("noiseReductionPresentationWindow"));
    m_presentationWindow->setWindowTitle(
        QStringLiteral("MicArray 降噪演示"));
    auto *layout = new QHBoxLayout(m_presentationWindow);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    m_presentationVideo =
        new NoiseReductionVideoWidget(m_presentationWindow);
    m_presentationVideo->setObjectName(
        QStringLiteral("noiseReductionPresentationVideo"));
    layout->addWidget(m_presentationVideo, 2);
    auto *spectra = new QWidget(m_presentationWindow);
    auto *spectraLayout = new QVBoxLayout(spectra);
    spectraLayout->setContentsMargins(0, 0, 0, 0);
    m_presentationFirstSpectrum = new PowerSpectrumWidget(spectra);
    m_presentationFirstSpectrum->setObjectName(
        QStringLiteral("noiseReductionPresentationSpectrum1"));
    m_presentationSecondSpectrum = new PowerSpectrumWidget(spectra);
    m_presentationSecondSpectrum->setObjectName(
        QStringLiteral("noiseReductionPresentationSpectrum2"));
    spectraLayout->addWidget(m_presentationFirstSpectrum);
    spectraLayout->addWidget(m_presentationSecondSpectrum);
    layout->addWidget(spectra, 1);
    m_presentationWindow->installEventFilter(this);
    m_presentationWindow->hide();
}

void NoiseReductionPage::connectUi()
{
    auto settingsChanged = [this] {
        applySettingsFromUi();
    };
    connect(m_enabledCheck, &QCheckBox::toggled,
            this, settingsChanged);
    connect(m_algorithmCombo, &QComboBox::currentIndexChanged,
            this, settingsChanged);
    connect(m_modeCombo, &QComboBox::currentIndexChanged,
            this, settingsChanged);
    connect(m_singleCoefficientSpin, &QDoubleSpinBox::valueChanged,
            this, settingsChanged);
    connect(m_dualB1CoefficientSpin, &QDoubleSpinBox::valueChanged,
            this, settingsChanged);
    connect(m_dualB2CoefficientSpin, &QDoubleSpinBox::valueChanged,
            this, settingsChanged);
    connect(m_monitorEnabledCheck, &QCheckBox::toggled,
            this, settingsChanged);
    connect(m_monitorSourceCombo, &QComboBox::currentIndexChanged,
            this, settingsChanged);
    connect(m_monitorGainSpin, &QDoubleSpinBox::valueChanged,
            this, settingsChanged);
    connect(m_bandPassEnabledCheck, &QCheckBox::toggled,
            this, settingsChanged);
    connect(m_bandPassPresetCombo, &QComboBox::currentIndexChanged,
            this, [this] {
                const BandPassPreset preset =
                    static_cast<BandPassPreset>(
                        m_bandPassPresetCombo->currentData().toInt());
                if (preset == BandPassPreset::Voice
                    || preset == BandPassPreset::Music) {
                    const QSignalBlocker lowBlocker(m_bandPassLowSpin);
                    const QSignalBlocker highBlocker(m_bandPassHighSpin);
                    m_bandPassLowSpin->setValue(
                        preset == BandPassPreset::Voice ? 100.0 : 40.0);
                    m_bandPassHighSpin->setValue(
                        preset == BandPassPreset::Voice
                            ? 8000.0 : 16000.0);
                }
                applySettingsFromUi();
            });
    auto customBandPassChanged = [this] {
        if (static_cast<BandPassPreset>(
                m_bandPassPresetCombo->currentData().toInt())
            != BandPassPreset::Custom) {
            const QSignalBlocker blocker(m_bandPassPresetCombo);
            selectData(m_bandPassPresetCombo,
                       static_cast<int>(BandPassPreset::Custom));
        }
        applySettingsFromUi();
    };
    connect(m_bandPassLowSpin, &QDoubleSpinBox::valueChanged,
            this, customBandPassChanged);
    connect(m_bandPassHighSpin, &QDoubleSpinBox::valueChanged,
            this, customBandPassChanged);
    connect(m_firstPlotSourceCombo, &QComboBox::currentIndexChanged,
            this, settingsChanged);
    connect(m_secondPlotSourceCombo, &QComboBox::currentIndexChanged,
            this, settingsChanged);
    connect(m_showMarkersCheck, &QCheckBox::toggled,
            this, settingsChanged);
    for (QDoubleSpinBox *spin : m_directionSpins) {
        connect(spin, &QDoubleSpinBox::valueChanged,
                this, settingsChanged);
    }
    for (QDoubleSpinBox *spin : m_mappingSpins) {
        connect(spin, &QDoubleSpinBox::editingFinished,
                this, settingsChanged);
    }
    for (QDoubleSpinBox *spin : m_cameraSpins) {
        connect(spin, &QDoubleSpinBox::editingFinished,
                this, settingsChanged);
    }
    connect(m_directionSelectionCombo, &QComboBox::currentIndexChanged,
            this, [this] {
                refreshDirectionViews();
            });
    connect(m_videoView, &NoiseReductionVideoWidget::videoPointClicked,
            this, &NoiseReductionPage::handleVideoPoint);
    connect(m_sendDirectionButton, &QPushButton::clicked,
            this, &NoiseReductionPage::requestControlSend);
    connect(m_fullscreenButton, &QPushButton::clicked,
            this, [this] {
                setPresentationMode(true);
            });
    connect(m_videoView, &NoiseReductionVideoWidget::fullScreenRequested,
            this, [this] {
                setPresentationMode(true);
            });
    connect(m_presentationVideo,
            &NoiseReductionVideoWidget::fullScreenRequested,
            this, [this] {
                setPresentationMode(false);
            });
}

void NoiseReductionPage::restoreSettings()
{
    m_settings = m_settingsStore != nullptr
        ? m_settingsStore->noiseReduction() : NoiseReductionSettings {};
    m_enabledCheck->setChecked(m_settings.enabled);
    selectData(m_algorithmCombo, static_cast<int>(m_settings.algorithm));
    selectData(m_modeCombo, static_cast<int>(m_settings.mode));
    m_singleCoefficientSpin->setValue(
        m_settings.singleSuppressionCoefficient);
    m_dualB1CoefficientSpin->setValue(
        m_settings.dualB1SuppressionCoefficient);
    m_dualB2CoefficientSpin->setValue(
        m_settings.dualB2SuppressionCoefficient);
    m_monitorEnabledCheck->setChecked(m_settings.monitorEnabled);
    selectData(m_monitorSourceCombo,
               static_cast<int>(m_settings.monitorSource));
    m_monitorGainSpin->setValue(m_settings.monitorGainDb);
    m_bandPassEnabledCheck->setChecked(m_settings.bandPass.enabled);
    selectData(m_bandPassPresetCombo,
               static_cast<int>(m_settings.bandPass.preset));
    m_bandPassLowSpin->setValue(m_settings.bandPass.lowCutoffHz);
    m_bandPassHighSpin->setValue(m_settings.bandPass.highCutoffHz);
    selectData(m_firstPlotSourceCombo,
               static_cast<int>(m_settings.firstPlotSource));
    selectData(m_secondPlotSourceCombo,
               static_cast<int>(m_settings.secondPlotSource));
    m_showMarkersCheck->setChecked(m_settings.showMarkers);
    const double directions[6] {
        m_settings.enhancedA.azimuthDegrees,
        m_settings.enhancedA.elevationDegrees,
        m_settings.suppressionB1.azimuthDegrees,
        m_settings.suppressionB1.elevationDegrees,
        m_settings.suppressionB2.azimuthDegrees,
        m_settings.suppressionB2.elevationDegrees,
    };
    for (int index = 0; index < 6; ++index) {
        m_directionSpins[index]->setValue(directions[index]);
    }
    m_mappingSpins[0]->setValue(m_settings.mapping.azimuthMinimumDegrees);
    m_mappingSpins[1]->setValue(m_settings.mapping.azimuthMaximumDegrees);
    m_mappingSpins[2]->setValue(m_settings.mapping.elevationMinimumDegrees);
    m_mappingSpins[3]->setValue(m_settings.mapping.elevationMaximumDegrees);
    m_cameraSpins[0]->setValue(m_settings.camera.centerAzimuthDegrees);
    m_cameraSpins[1]->setValue(m_settings.camera.centerElevationDegrees);
    m_cameraSpins[2]->setValue(m_settings.camera.horizontalFovDegrees);
    m_cameraSpins[3]->setValue(m_settings.camera.verticalFovDegrees);
    m_cameraSpins[4]->setValue(m_settings.camera.rollDegrees);
    refreshProcessingAvailability();
    refreshDirectionViews();
    refreshControlAvailability();
}

NoiseReductionSettings NoiseReductionPage::settingsFromUi() const
{
    NoiseReductionSettings value;
    value.enabled = m_enabledCheck->isChecked();
    value.algorithm = static_cast<NoiseReductionAlgorithm>(
        m_algorithmCombo->currentData().toInt());
    value.mode = static_cast<NoiseReductionMode>(
        m_modeCombo->currentData().toInt());
    value.singleSuppressionCoefficient =
        m_singleCoefficientSpin->value();
    value.dualB1SuppressionCoefficient =
        m_dualB1CoefficientSpin->value();
    value.dualB2SuppressionCoefficient =
        m_dualB2CoefficientSpin->value();
    value.monitorEnabled = m_monitorEnabledCheck->isChecked();
    value.monitorSource = static_cast<NoiseSignalSource>(
        m_monitorSourceCombo->currentData().toInt());
    value.monitorGainDb = m_monitorGainSpin->value();
    value.bandPass.enabled = m_bandPassEnabledCheck->isChecked();
    value.bandPass.preset = static_cast<BandPassPreset>(
        m_bandPassPresetCombo->currentData().toInt());
    value.bandPass.lowCutoffHz = m_bandPassLowSpin->value();
    value.bandPass.highCutoffHz = m_bandPassHighSpin->value();
    value.firstPlotSource = static_cast<NoiseSignalSource>(
        m_firstPlotSourceCombo->currentData().toInt());
    value.secondPlotSource = static_cast<NoiseSignalSource>(
        m_secondPlotSourceCombo->currentData().toInt());
    value.showMarkers = m_showMarkersCheck->isChecked();
    value.enhancedA = {
        m_directionSpins[0]->value(),
        m_directionSpins[1]->value(),
    };
    value.suppressionB1 = {
        m_directionSpins[2]->value(),
        m_directionSpins[3]->value(),
    };
    value.suppressionB2 = {
        m_directionSpins[4]->value(),
        m_directionSpins[5]->value(),
    };
    value.mapping = {
        m_mappingSpins[0]->value(),
        m_mappingSpins[1]->value(),
        m_mappingSpins[2]->value(),
        m_mappingSpins[3]->value(),
    };
    if (!value.mapping.isValid()) {
        value.mapping = m_settings.mapping;
    }
    value.enhancedA = DirectionControlMapper::clampDirection(
        value.enhancedA, value.mapping);
    value.suppressionB1 = DirectionControlMapper::clampDirection(
        value.suppressionB1, value.mapping);
    value.suppressionB2 = DirectionControlMapper::clampDirection(
        value.suppressionB2, value.mapping);
    value.camera.centerAzimuthDegrees = m_cameraSpins[0]->value();
    value.camera.centerElevationDegrees = m_cameraSpins[1]->value();
    value.camera.horizontalFovDegrees = m_cameraSpins[2]->value();
    value.camera.verticalFovDegrees = m_cameraSpins[3]->value();
    value.camera.rollDegrees = m_cameraSpins[4]->value();
    return value;
}

void NoiseReductionPage::applySettingsFromUi()
{
    m_settings = settingsFromUi();
    if (m_settingsStore != nullptr) {
        m_settingsStore->saveNoiseReduction(m_settings);
    }
    refreshProcessingAvailability();
    refreshDirectionViews();
    emit configureWorkerRequested(m_settings, m_sampleRate);
    emit workerActiveRequested(m_active && m_settings.enabled);
    if (!m_settings.enabled) {
        m_firstWaveform->clear();
        m_secondWaveform->clear();
        m_firstSpectrum->clear();
        m_secondSpectrum->clear();
    }
}

void NoiseReductionPage::refreshProcessingAvailability()
{
    const bool fixedAlgorithm =
        static_cast<NoiseReductionAlgorithm>(
            m_algorithmCombo->currentData().toInt())
        == NoiseReductionAlgorithm::FixedParameterWiener;
    const bool twoPoint =
        static_cast<NoiseReductionMode>(
            m_modeCombo->currentData().toInt())
        == NoiseReductionMode::TwoPoint;
    m_singleCoefficientSpin->setEnabled(fixedAlgorithm && !twoPoint);
    m_dualB1CoefficientSpin->setEnabled(fixedAlgorithm && twoPoint);
    m_dualB2CoefficientSpin->setEnabled(fixedAlgorithm && twoPoint);

    const bool monitorEnabled = m_monitorEnabledCheck->isChecked();
    m_monitorSourceCombo->setEnabled(monitorEnabled);
    m_monitorGainSpin->setEnabled(monitorEnabled);

    const bool bandPassEnabled = m_bandPassEnabledCheck->isChecked();
    m_bandPassPresetCombo->setEnabled(bandPassEnabled);
    m_bandPassLowSpin->setEnabled(bandPassEnabled);
    m_bandPassHighSpin->setEnabled(bandPassEnabled);
}

void NoiseReductionPage::refreshDirectionViews()
{
    const double directions[6] {
        m_settings.enhancedA.azimuthDegrees,
        m_settings.enhancedA.elevationDegrees,
        m_settings.suppressionB1.azimuthDegrees,
        m_settings.suppressionB1.elevationDegrees,
        m_settings.suppressionB2.azimuthDegrees,
        m_settings.suppressionB2.elevationDegrees,
    };
    for (int index = 0; index < 6; ++index) {
        const QSignalBlocker blocker(m_directionSpins[index]);
        if (index % 2 == 0) {
            m_directionSpins[index]->setRange(
                m_settings.mapping.azimuthMinimumDegrees,
                m_settings.mapping.azimuthMaximumDegrees);
            m_directionCodeLabels[index]->setText(
                QString::number(DirectionControlMapper::azimuthCode(
                    directions[index], m_settings.mapping)));
        } else {
            m_directionSpins[index]->setRange(
                m_settings.mapping.elevationMinimumDegrees,
                m_settings.mapping.elevationMaximumDegrees);
            m_directionCodeLabels[index]->setText(
                QString::number(DirectionControlMapper::elevationCode(
                    directions[index], m_settings.mapping)));
        }
        m_directionSpins[index]->setValue(directions[index]);
    }
    const NoiseDirectionSelection selection =
        static_cast<NoiseDirectionSelection>(
            m_directionSelectionCombo->currentData().toInt());
    for (NoiseReductionVideoWidget *view :
         {m_videoView, m_presentationVideo}) {
        if (view == nullptr) {
            continue;
        }
        view->setCameraView(m_settings.camera);
        view->setControlMapping(m_settings.mapping);
        view->setDirectionTargets(m_settings.enhancedA,
                                  m_settings.suppressionB1,
                                  m_settings.suppressionB2);
        view->setCurrentSelection(selection);
        view->setMarkersVisible(m_settings.showMarkers);
    }
}

void NoiseReductionPage::refreshControlAvailability()
{
    if (m_sendDirectionButton == nullptr) {
        return;
    }
    m_sendDirectionButton->setEnabled(m_connected && !m_controlLocked);
}

void NoiseReductionPage::handleVideoPoint(
    const QPointF &normalizedPoint)
{
    const ScanDirection direction =
        unprojectVideoPoint(
            normalizedPoint, projectionSafeCamera(m_settings.camera));
    const ContinuousDirectionTarget target =
        DirectionControlMapper::clampDirection(
            {direction.azimuthDegrees, direction.elevationDegrees},
            m_settings.mapping);
    const NoiseDirectionSelection selection =
        static_cast<NoiseDirectionSelection>(
            m_directionSelectionCombo->currentData().toInt());
    const int base = selection == NoiseDirectionSelection::EnhancedA
        ? 0 : (selection == NoiseDirectionSelection::SuppressionB1 ? 2 : 4);
    {
        const QSignalBlocker first(m_directionSpins[base]);
        const QSignalBlocker second(m_directionSpins[base + 1]);
        m_directionSpins[base]->setValue(target.azimuthDegrees);
        m_directionSpins[base + 1]->setValue(target.elevationDegrees);
    }
    applySettingsFromUi();
    requestControlSend();
}

void NoiseReductionPage::requestControlSend()
{
    if (!m_connected) {
        setControlSendResult(
            false, QStringLiteral("设备未连接，方向值尚未发送"));
        return;
    }
    if (m_controlLocked) {
        setControlSendResult(
            false, QStringLiteral("控制已锁定，方向值尚未发送"));
        return;
    }
    emit controlSendRequested(currentControlValues());
}

void NoiseReductionPage::showPlotFrame(
    const NoiseReductionPlotFrame &frame)
{
    m_plotFrame = frame;
    const QString firstName = noiseSignalSourceName(frame.first.source);
    const QString secondName = noiseSignalSourceName(frame.second.source);
    m_firstWaveform->setTitle(
        QStringLiteral("%1 · 时域波形").arg(firstName));
    m_secondWaveform->setTitle(
        QStringLiteral("%1 · 时域波形").arg(secondName));
    m_firstWaveform->setSamples(frame.first.waveform);
    m_secondWaveform->setSamples(frame.second.waveform);
    m_firstSpectrum->setSpectrum(
        firstName, frame.first.spectrumDbfs, frame.sampleRate);
    m_secondSpectrum->setSpectrum(
        secondName, frame.second.spectrumDbfs, frame.sampleRate);
    m_presentationFirstSpectrum->setSpectrum(
        firstName, frame.first.spectrumDbfs, frame.sampleRate);
    m_presentationSecondSpectrum->setSpectrum(
        secondName, frame.second.spectrumDbfs, frame.sampleRate);
}

void NoiseReductionPage::setPresentationMode(bool enabled)
{
    if (m_presentationWindow == nullptr) {
        return;
    }
    if (enabled) {
        if (!m_videoFrame.isNull()) {
            m_presentationVideo->setFrame(m_videoFrame);
        }
        refreshDirectionViews();
        if (!m_plotFrame.first.spectrumDbfs.isEmpty()) {
            showPlotFrame(m_plotFrame);
        }
        m_presentationWindow->showFullScreen();
        m_presentationWindow->raise();
        m_presentationWindow->activateWindow();
    } else {
        m_presentationWindow->hide();
    }
}

} // namespace MicArray
