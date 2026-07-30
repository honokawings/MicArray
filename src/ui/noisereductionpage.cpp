#include "ui/noisereductionpage.h"

#include "analysis/directionprojection.h"
#include "control/directioncontrolmapper.h"
#include "ui/audiowaveformwidget.h"
#include "ui/noisereductionvideowidget.h"
#include "ui/powerspectrumwidget.h"
#include "ui/processingnodelistwidget.h"

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
#include <QStackedWidget>
#include <QTabWidget>
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
        if (m_wheelBlockedControls.contains(
                qobject_cast<QWidget *>(watched))) {
            event->accept();
            return true;
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

    auto *controlPanel = new QWidget(topSplitter);
    controlPanel->setObjectName(
        QStringLiteral("noiseReductionControlPanel"));
    controlPanel->setMinimumWidth(440);
    auto *controlPanelLayout = new QVBoxLayout(controlPanel);
    controlPanelLayout->setContentsMargins(0, 0, 0, 0);
    m_controlTabs = new QTabWidget(controlPanel);
    m_controlTabs->setObjectName(
        QStringLiteral("noiseReductionControlTabs"));
    controlPanelLayout->addWidget(m_controlTabs);

    auto createTab = [this](
                         const QString &title,
                         QWidget **content,
                         QVBoxLayout **contentLayout) {
        auto *scroll = new QScrollArea(m_controlTabs);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        *content = new QWidget(scroll);
        *contentLayout = new QVBoxLayout(*content);
        (*contentLayout)->setContentsMargins(8, 8, 8, 8);
        scroll->setWidget(*content);
        m_controlTabs->addTab(scroll, title);
    };
    QWidget *panel = nullptr;
    QWidget *monitorPanel = nullptr;
    QWidget *directionPanel = nullptr;
    QVBoxLayout *panelLayout = nullptr;
    QVBoxLayout *monitorPanelLayout = nullptr;
    QVBoxLayout *directionPanelLayout = nullptr;
    createTab(QStringLiteral("数据流"), &panel, &panelLayout);
    createTab(QStringLiteral("监听"),
              &monitorPanel, &monitorPanelLayout);
    createTab(QStringLiteral("方向控制"),
              &directionPanel, &directionPanelLayout);

    m_processingNodeList =
        new ProcessingNodeListWidget(panel);
    m_nodeParameterStack = new QStackedWidget(panel);
    m_nodeParameterStack->setObjectName(
        QStringLiteral("noiseReductionNodeParameterStack"));
    panelLayout->addWidget(m_processingNodeList);
    panelLayout->addWidget(m_nodeParameterStack, 1);

    auto *processingGroup =
        new QGroupBox(QStringLiteral("降噪处理"), panel);
    processingGroup->setObjectName(
        QStringLiteral("noiseReductionSuppressionParameters"));
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
    m_nodeParameterStack->addWidget(processingGroup);

    auto *monitorGroup =
        new QGroupBox(QStringLiteral("监听"), monitorPanel);
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
    m_monitorGainSpin->setRange(-60.0, 48.0);
    m_monitorGainSpin->setDecimals(1);
    m_monitorGainSpin->setSuffix(QStringLiteral(" dB"));
    monitorLayout->addRow(m_monitorEnabledCheck);
    monitorLayout->addRow(QStringLiteral("监听源"), m_monitorSourceCombo);
    monitorLayout->addRow(QStringLiteral("监听增益"), m_monitorGainSpin);
    monitorPanelLayout->addWidget(monitorGroup);
    monitorPanelLayout->addStretch();

    auto *bandPassGroup =
        new QGroupBox(QStringLiteral("输出带通滤波"), panel);
    bandPassGroup->setObjectName(
        QStringLiteral("noiseReductionBandPassParameters"));
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
    m_nodeParameterStack->addWidget(bandPassGroup);

    auto *logMmseGroup =
        new QGroupBox(QStringLiteral("LogMMSE 降噪"), panel);
    logMmseGroup->setObjectName(
        QStringLiteral("noiseReductionLogMmseParameters"));
    auto *logMmseLayout = new QFormLayout(logMmseGroup);
    m_logMmseEnabledCheck =
        new QCheckBox(QStringLiteral("启用 LogMMSE"), logMmseGroup);
    m_logMmseEnabledCheck->setObjectName(
        QStringLiteral("noiseReductionLogMmseEnabledCheck"));
    m_logMmseFftSizeCombo = new QComboBox(logMmseGroup);
    m_logMmseFftSizeCombo->setObjectName(
        QStringLiteral("noiseReductionLogMmseFftSizeCombo"));
    for (int fftSize : {256, 512, 1024, 2048}) {
        m_logMmseFftSizeCombo->addItem(
            QString::number(fftSize), fftSize);
    }
    m_logMmseOverlapSpin = new QDoubleSpinBox(logMmseGroup);
    m_logMmseOverlapSpin->setObjectName(
        QStringLiteral("noiseReductionLogMmseOverlapSpin"));
    m_logMmseOverlapSpin->setRange(0.0, 0.875);
    m_logMmseOverlapSpin->setDecimals(3);
    m_logMmseOverlapSpin->setSingleStep(0.125);
    m_logMmsePriorSnrSpin = new QDoubleSpinBox(logMmseGroup);
    m_logMmsePriorSnrSpin->setObjectName(
        QStringLiteral("noiseReductionLogMmsePriorSnrSpin"));
    m_logMmsePriorSnrSpin->setRange(0.0, 0.999);
    m_logMmsePriorSnrSpin->setDecimals(3);
    m_logMmsePriorSnrSpin->setSingleStep(0.01);
    m_logMmseNoiseSmoothingSpin =
        new QDoubleSpinBox(logMmseGroup);
    m_logMmseNoiseSmoothingSpin->setObjectName(
        QStringLiteral("noiseReductionLogMmseNoiseSmoothingSpin"));
    m_logMmseNoiseSmoothingSpin->setRange(0.0, 0.999);
    m_logMmseNoiseSmoothingSpin->setDecimals(3);
    m_logMmseNoiseSmoothingSpin->setSingleStep(0.01);
    m_logMmseMinimumGainSpin =
        new QDoubleSpinBox(logMmseGroup);
    m_logMmseMinimumGainSpin->setObjectName(
        QStringLiteral("noiseReductionLogMmseMinimumGainSpin"));
    m_logMmseMinimumGainSpin->setRange(-80.0, 0.0);
    m_logMmseMinimumGainSpin->setDecimals(1);
    m_logMmseMinimumGainSpin->setSuffix(QStringLiteral(" dB"));
    logMmseLayout->addRow(m_logMmseEnabledCheck);
    logMmseLayout->addRow(QStringLiteral("FFT 点数"),
                          m_logMmseFftSizeCombo);
    logMmseLayout->addRow(QStringLiteral("重叠比例"),
                          m_logMmseOverlapSpin);
    logMmseLayout->addRow(QStringLiteral("先验 SNR 平滑"),
                          m_logMmsePriorSnrSpin);
    logMmseLayout->addRow(QStringLiteral("噪声功率平滑"),
                          m_logMmseNoiseSmoothingSpin);
    logMmseLayout->addRow(QStringLiteral("最小增益"),
                          m_logMmseMinimumGainSpin);
    m_nodeParameterStack->addWidget(logMmseGroup);

    auto *agcGroup =
        new QGroupBox(QStringLiteral("自动增益控制"), panel);
    agcGroup->setObjectName(
        QStringLiteral("noiseReductionAgcParameters"));
    auto *agcLayout = new QFormLayout(agcGroup);
    m_agcEnabledCheck =
        new QCheckBox(QStringLiteral("启用 AGC"), agcGroup);
    m_agcEnabledCheck->setObjectName(
        QStringLiteral("noiseReductionAgcEnabledCheck"));
    auto createAgcSpin = [this, agcGroup](
                             const QString &objectName,
                             double minimum,
                             double maximum,
                             const QString &suffix) {
        auto *spin = new QDoubleSpinBox(agcGroup);
        spin->setObjectName(objectName);
        spin->setRange(minimum, maximum);
        spin->setDecimals(1);
        spin->setSingleStep(1.0);
        spin->setSuffix(suffix);
        spin->installEventFilter(this);
        m_wheelBlockedControls.append(spin);
        return spin;
    };
    m_agcTargetSpin = createAgcSpin(
        QStringLiteral("noiseReductionAgcTargetSpin"),
        -60.0, 0.0, QStringLiteral(" dBFS"));
    m_agcMaximumGainSpin = createAgcSpin(
        QStringLiteral("noiseReductionAgcMaximumGainSpin"),
        0.0, 60.0, QStringLiteral(" dB"));
    m_agcMaximumAttenuationSpin = createAgcSpin(
        QStringLiteral("noiseReductionAgcMaximumAttenuationSpin"),
        -60.0, 0.0, QStringLiteral(" dB"));
    m_agcAttackSpin = createAgcSpin(
        QStringLiteral("noiseReductionAgcAttackSpin"),
        1.0, 1000.0, QStringLiteral(" ms"));
    m_agcReleaseSpin = createAgcSpin(
        QStringLiteral("noiseReductionAgcReleaseSpin"),
        1.0, 5000.0, QStringLiteral(" ms"));
    m_agcLimiterSpin = createAgcSpin(
        QStringLiteral("noiseReductionAgcLimiterSpin"),
        -20.0, 0.0, QStringLiteral(" dBFS"));
    agcLayout->addRow(m_agcEnabledCheck);
    agcLayout->addRow(QStringLiteral("目标电平"), m_agcTargetSpin);
    agcLayout->addRow(QStringLiteral("最大增益"),
                      m_agcMaximumGainSpin);
    agcLayout->addRow(QStringLiteral("最大衰减"),
                      m_agcMaximumAttenuationSpin);
    agcLayout->addRow(QStringLiteral("启动时间"), m_agcAttackSpin);
    agcLayout->addRow(QStringLiteral("释放时间"), m_agcReleaseSpin);
    agcLayout->addRow(QStringLiteral("限幅电平"), m_agcLimiterSpin);
    m_nodeParameterStack->addWidget(agcGroup);

    for (QWidget *control : {
             static_cast<QWidget *>(m_logMmseFftSizeCombo),
             static_cast<QWidget *>(m_logMmseOverlapSpin),
             static_cast<QWidget *>(m_logMmsePriorSnrSpin),
             static_cast<QWidget *>(m_logMmseNoiseSmoothingSpin),
             static_cast<QWidget *>(m_logMmseMinimumGainSpin),
         }) {
        control->installEventFilter(this);
        m_wheelBlockedControls.append(control);
    }

    auto *directionGroup =
        new QGroupBox(QStringLiteral("方向控制"), directionPanel);
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
    directionPanelLayout->addWidget(directionGroup);

    auto *mappingGroup =
        new QGroupBox(QStringLiteral("实际角度映射"), directionPanel);
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
    directionPanelLayout->addWidget(mappingGroup);

    auto *cameraGroup =
        new QGroupBox(QStringLiteral("视频视角参数"), directionPanel);
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
    directionPanelLayout->addWidget(cameraGroup);

    m_fullscreenButton =
        new QPushButton(QStringLiteral("全屏显示"), directionPanel);
    m_fullscreenButton->setObjectName(
        QStringLiteral("noiseReductionFullscreenButton"));
    directionPanelLayout->addWidget(m_fullscreenButton);
    directionPanelLayout->addStretch();
    topSplitter->addWidget(controlPanel);
    topSplitter->setSizes({880, 520});
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
    connect(m_processingNodeList,
            &ProcessingNodeListWidget::postProcessingOrderChanged,
            this, [this](const QVector<NoisePostProcessingNode> &) {
                applySettingsFromUi();
            });
    connect(m_processingNodeList,
            &ProcessingNodeListWidget::currentNodeChanged,
            this, [this](int nodeId) {
                showNodeParameters(nodeId);
                m_settings.selectedDataFlowNode = nodeId;
                if (m_settingsStore != nullptr) {
                    m_settingsStore->saveNoiseReduction(m_settings);
                }
            });
    connect(m_controlTabs, &QTabWidget::currentChanged,
            this, [this](int index) {
                m_settings.controlTabIndex = index;
                if (m_settingsStore != nullptr) {
                    m_settingsStore->saveNoiseReduction(m_settings);
                }
            });
    connect(m_logMmseEnabledCheck, &QCheckBox::toggled,
            this, settingsChanged);
    connect(m_logMmseFftSizeCombo,
            &QComboBox::currentIndexChanged,
            this, settingsChanged);
    for (QDoubleSpinBox *spin : {
             m_logMmseOverlapSpin,
             m_logMmsePriorSnrSpin,
             m_logMmseNoiseSmoothingSpin,
             m_logMmseMinimumGainSpin,
             m_agcTargetSpin,
             m_agcMaximumGainSpin,
             m_agcMaximumAttenuationSpin,
             m_agcAttackSpin,
             m_agcReleaseSpin,
             m_agcLimiterSpin,
         }) {
        connect(spin, &QDoubleSpinBox::valueChanged,
                this, settingsChanged);
    }
    connect(m_agcEnabledCheck, &QCheckBox::toggled,
            this, settingsChanged);
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
    for (NoiseReductionVideoWidget *view :
         {m_videoView, m_presentationVideo}) {
        connect(view,
                &NoiseReductionVideoWidget::directionDragStarted,
                this,
                [this](NoiseDirectionSelection selection) {
                    const QSignalBlocker blocker(
                        m_directionSelectionCombo);
                    selectData(
                        m_directionSelectionCombo,
                        static_cast<int>(selection));
                    refreshDirectionViews();
                });
        connect(view,
                &NoiseReductionVideoWidget::directionDragged,
                this,
                [this](NoiseDirectionSelection selection,
                       const QPointF &point) {
                    handleDirectionDragPoint(
                        selection, point, false);
                });
        connect(view,
                &NoiseReductionVideoWidget::directionDragFinished,
                this,
                [this](NoiseDirectionSelection selection,
                       const QPointF &point) {
                    handleDirectionDragPoint(
                        selection, point, true);
                });
    }
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
    showNodeParameters(
        m_processingNodeList->nodeIdAtRow(
            m_processingNodeList->currentRow()));
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
    m_processingNodeList->setPostProcessingOrder(
        m_settings.postProcessingOrder);
    m_controlTabs->setCurrentIndex(m_settings.controlTabIndex);
    for (int row = 0;
         row < m_processingNodeList->count(); ++row) {
        if (m_processingNodeList->nodeIdAtRow(row)
            == m_settings.selectedDataFlowNode) {
            m_processingNodeList->setCurrentRow(row);
            break;
        }
    }
    m_logMmseEnabledCheck->setChecked(
        m_settings.logMmse.enabled);
    selectData(m_logMmseFftSizeCombo,
               m_settings.logMmse.fftSize);
    m_logMmseOverlapSpin->setValue(
        m_settings.logMmse.overlapRatio);
    m_logMmsePriorSnrSpin->setValue(
        m_settings.logMmse.priorSnrSmoothing);
    m_logMmseNoiseSmoothingSpin->setValue(
        m_settings.logMmse.noisePowerSmoothing);
    m_logMmseMinimumGainSpin->setValue(
        m_settings.logMmse.minimumGainDb);
    m_agcEnabledCheck->setChecked(
        m_settings.automaticGainControl.enabled);
    m_agcTargetSpin->setValue(
        m_settings.automaticGainControl.targetLevelDbfs);
    m_agcMaximumGainSpin->setValue(
        m_settings.automaticGainControl.maximumGainDb);
    m_agcMaximumAttenuationSpin->setValue(
        m_settings.automaticGainControl.maximumAttenuationDb);
    m_agcAttackSpin->setValue(
        m_settings.automaticGainControl.attackMilliseconds);
    m_agcReleaseSpin->setValue(
        m_settings.automaticGainControl.releaseMilliseconds);
    m_agcLimiterSpin->setValue(
        m_settings.automaticGainControl.limiterLevelDbfs);
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
    refreshNodeSummaries();
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
    value.postProcessingOrder =
        m_processingNodeList->postProcessingOrder();
    value.controlTabIndex = m_controlTabs->currentIndex();
    value.selectedDataFlowNode =
        m_processingNodeList->nodeIdAtRow(
            m_processingNodeList->currentRow());
    value.logMmse.enabled =
        m_logMmseEnabledCheck->isChecked();
    value.logMmse.fftSize =
        m_logMmseFftSizeCombo->currentData().toInt();
    value.logMmse.overlapRatio =
        m_logMmseOverlapSpin->value();
    value.logMmse.priorSnrSmoothing =
        m_logMmsePriorSnrSpin->value();
    value.logMmse.noisePowerSmoothing =
        m_logMmseNoiseSmoothingSpin->value();
    value.logMmse.minimumGainDb =
        m_logMmseMinimumGainSpin->value();
    value.automaticGainControl.enabled =
        m_agcEnabledCheck->isChecked();
    value.automaticGainControl.targetLevelDbfs =
        m_agcTargetSpin->value();
    value.automaticGainControl.maximumGainDb =
        m_agcMaximumGainSpin->value();
    value.automaticGainControl.maximumAttenuationDb =
        m_agcMaximumAttenuationSpin->value();
    value.automaticGainControl.attackMilliseconds =
        m_agcAttackSpin->value();
    value.automaticGainControl.releaseMilliseconds =
        m_agcReleaseSpin->value();
    value.automaticGainControl.limiterLevelDbfs =
        m_agcLimiterSpin->value();
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
    refreshNodeSummaries();
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

    const bool logMmseEnabled = m_logMmseEnabledCheck->isChecked();
    m_logMmseFftSizeCombo->setEnabled(logMmseEnabled);
    m_logMmseOverlapSpin->setEnabled(logMmseEnabled);
    m_logMmsePriorSnrSpin->setEnabled(logMmseEnabled);
    m_logMmseNoiseSmoothingSpin->setEnabled(logMmseEnabled);
    m_logMmseMinimumGainSpin->setEnabled(logMmseEnabled);

    const bool agcEnabled = m_agcEnabledCheck->isChecked();
    for (QDoubleSpinBox *spin : {
             m_agcTargetSpin,
             m_agcMaximumGainSpin,
             m_agcMaximumAttenuationSpin,
             m_agcAttackSpin,
             m_agcReleaseSpin,
             m_agcLimiterSpin,
         }) {
        spin->setEnabled(agcEnabled);
    }
}

void NoiseReductionPage::refreshNodeSummaries()
{
    m_processingNodeList->setNodeSummary(
        ProcessingNodeListWidget::SuppressionNodeId,
        m_enabledCheck->isChecked(),
        m_modeCombo->currentText());
    m_processingNodeList->setNodeSummary(
        static_cast<int>(NoisePostProcessingNode::BandPass),
        m_bandPassEnabledCheck->isChecked(),
        QStringLiteral("%1–%2 Hz")
            .arg(m_bandPassLowSpin->value(), 0, 'f', 0)
            .arg(m_bandPassHighSpin->value(), 0, 'f', 0));
    m_processingNodeList->setNodeSummary(
        static_cast<int>(NoisePostProcessingNode::LogMmse),
        m_logMmseEnabledCheck->isChecked(),
        QStringLiteral("%1 点 · %2% 重叠")
            .arg(m_logMmseFftSizeCombo->currentData().toInt())
            .arg(m_logMmseOverlapSpin->value() * 100.0,
                 0, 'f', 0));
    m_processingNodeList->setNodeSummary(
        static_cast<int>(
            NoisePostProcessingNode::AutomaticGainControl),
        m_agcEnabledCheck->isChecked(),
        QStringLiteral("目标 %1 dBFS")
            .arg(m_agcTargetSpin->value(), 0, 'f', 1));
}

void NoiseReductionPage::showNodeParameters(int nodeId)
{
    const int pageIndex =
        nodeId == ProcessingNodeListWidget::SuppressionNodeId
        ? 0 : nodeId + 1;
    if (pageIndex >= 0
        && pageIndex < m_nodeParameterStack->count()) {
        m_nodeParameterStack->setCurrentIndex(pageIndex);
    }
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
        unprojectVideoPointAngular(
            normalizedPoint, m_settings.camera);
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

void NoiseReductionPage::handleDirectionDragPoint(
    NoiseDirectionSelection selection,
    const QPointF &normalizedPoint,
    bool finished)
{
    const ScanDirection direction =
        unprojectVideoPointAngular(
            normalizedPoint, m_settings.camera);
    const ContinuousDirectionTarget target =
        DirectionControlMapper::clampDirection(
            {direction.azimuthDegrees, direction.elevationDegrees},
            m_settings.mapping);
    {
        const QSignalBlocker selectionBlocker(
            m_directionSelectionCombo);
        selectData(m_directionSelectionCombo,
                   static_cast<int>(selection));
    }
    const int base = selection == NoiseDirectionSelection::EnhancedA
        ? 0 : (selection == NoiseDirectionSelection::SuppressionB1
                   ? 2 : 4);
    {
        const QSignalBlocker first(m_directionSpins[base]);
        const QSignalBlocker second(m_directionSpins[base + 1]);
        m_directionSpins[base]->setValue(target.azimuthDegrees);
        m_directionSpins[base + 1]->setValue(target.elevationDegrees);
    }
    if (finished) {
        applySettingsFromUi();
        requestControlSend();
        return;
    }
    m_settings = settingsFromUi();
    refreshDirectionViews();
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
