#include "ui/audioanalysispage.h"

#include "analysis/soundsourcelocatorregistry.h"
#include "ui/audiowaveformwidget.h"
#include "ui/directionphotowidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>

namespace MicArray {
namespace {

QDoubleSpinBox *angleSpin(QWidget *parent, double minimum, double maximum)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setRange(minimum, maximum);
    spin->setDecimals(1);
    spin->setSingleStep(1.0);
    spin->setSuffix(QStringLiteral("°"));
    return spin;
}

QString formatPosition(qint64 samples, int sampleRate)
{
    const double seconds = sampleRate > 0 ? static_cast<double>(samples) / sampleRate : 0.0;
    return QStringLiteral("%1:%2.%3")
        .arg(static_cast<int>(seconds) / 60, 2, 10, QLatin1Char('0'))
        .arg(static_cast<int>(seconds) % 60, 2, 10, QLatin1Char('0'))
        .arg(static_cast<int>(seconds * 10.0) % 10);
}

} // namespace

AudioAnalysisPage::AudioAnalysisPage(AppSettings *settings, QWidget *parent)
    : QWidget(parent)
    , m_settingsStore(settings)
    , m_worker(new AudioAnalysisWorker)
{
    setObjectName(QStringLiteral("audioAnalysisPage"));
    buildUi();
    restoreSettings();

    m_workerThread.setObjectName(QStringLiteral("OfflineAudioAnalysisThread"));
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &AudioAnalysisPage::loadFileRequested, m_worker,
            &AudioAnalysisWorker::loadFile, Qt::QueuedConnection);
    connect(this, &AudioAnalysisPage::playRequested, m_worker,
            &AudioAnalysisWorker::play, Qt::QueuedConnection);
    connect(this, &AudioAnalysisPage::pauseRequested, m_worker,
            &AudioAnalysisWorker::pause, Qt::QueuedConnection);
    connect(this, &AudioAnalysisPage::stopRequested, m_worker,
            &AudioAnalysisWorker::stop, Qt::QueuedConnection);
    connect(this, &AudioAnalysisPage::seekRequested, m_worker,
            &AudioAnalysisWorker::seek, Qt::QueuedConnection);
    connect(this, &AudioAnalysisPage::outputChannelRequested, m_worker,
            &AudioAnalysisWorker::setOutputChannel, Qt::QueuedConnection);
    connect(this, &AudioAnalysisPage::volumeRequested, m_worker,
            &AudioAnalysisWorker::setVolume, Qt::QueuedConnection);
    connect(this, &AudioAnalysisPage::locatorRequested, m_worker,
            &AudioAnalysisWorker::setLocator, Qt::QueuedConnection);
    connect(this, &AudioAnalysisPage::configurationRequested, m_worker,
            &AudioAnalysisWorker::configureAnalysis, Qt::QueuedConnection);
    connect(this, &AudioAnalysisPage::analysisRequested, m_worker,
            &AudioAnalysisWorker::analyzeAt, Qt::QueuedConnection);

    connect(m_worker, &AudioAnalysisWorker::fileLoaded, this,
            [this](const WaveFileInfo &info, quint64 requestId) {
                if (requestId != m_requestId) {
                    return;
                }
                m_fileInfo = info;
                m_positionSlider->setEnabled(true);
                m_playButton->setEnabled(true);
                m_pauseButton->setEnabled(true);
                m_stopButton->setEnabled(true);
                m_fileLabel->setText(QStringLiteral("%1 · %2 通道 · %3 Hz · %4 秒")
                                         .arg(QFileInfo(info.path).fileName())
                                         .arg(info.channelCount)
                                         .arg(info.sampleRate)
                                         .arg(info.durationSeconds, 0, 'f', 2));
                m_settings.lastWavePath = info.path;
                persistSettings();
            });
    connect(m_worker, &AudioAnalysisWorker::positionChanged, this,
            [this](qint64 samplePosition) {
                setSliderSamplePosition(samplePosition);
                m_positionLabel->setText(QStringLiteral("%1 / %2")
                                             .arg(formatPosition(samplePosition, m_fileInfo.sampleRate),
                                                  formatPosition(m_fileInfo.totalSamples, m_fileInfo.sampleRate)));
            });
    connect(m_worker, &AudioAnalysisWorker::waveformReady,
            m_waveform, &AudioWaveformWidget::appendPcm16);
    connect(m_worker, &AudioAnalysisWorker::directionsReady, this,
            [this](quint64 requestId, const QVector<SoundSourceDirection> &directions,
                   qint64 elapsedMilliseconds) {
                if (requestId < m_requestId) {
                    return;
                }
                m_photoWidget->setDirections(directions);
                if (directions.isEmpty()) {
                    m_directionLabel->setText(QStringLiteral("未检测到可信声源 · %1 ms")
                                                  .arg(elapsedMilliseconds));
                    return;
                }
                const SoundSourceDirection &direction = directions.first();
                m_directionLabel->setText(
                    QStringLiteral("方位角 %1°　俯仰角 %2°　置信度 %3%　耗时 %4 ms")
                        .arg(direction.azimuthDegrees, 0, 'f', 1)
                        .arg(direction.elevationDegrees, 0, 'f', 1)
                        .arg(direction.confidence * 100.0, 0, 'f', 0)
                        .arg(elapsedMilliseconds));
            });
    connect(m_worker, &AudioAnalysisWorker::analysisStatusChanged, this,
            [this](const QString &status) {
                m_statusLabel->setText(status);
                m_photoWidget->setStatusText(status);
            });
    connect(m_worker, &AudioAnalysisWorker::errorOccurred, this,
            [this](const QString &error) {
                const QString text = QStringLiteral("错误：%1").arg(error);
                m_statusLabel->setText(text);
                m_photoWidget->setStatusText(text);
            });

    connectUi();
    m_workerThread.start();
    emit configurationRequested(m_settings);
    emit outputChannelRequested(m_settings.outputChannel);
    emit volumeRequested(m_settings.volume);
    emit locatorRequested(m_settings.locatorId);
}

AudioAnalysisPage::~AudioAnalysisPage()
{
    persistSettings();
    if (m_workerThread.isRunning()) {
        QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
        m_workerThread.quit();
        m_workerThread.wait(3000);
    }
}

void AudioAnalysisPage::setActive(bool active)
{
    m_active = active;
    if (!active) {
        emit stopRequested();
    }
}

void AudioAnalysisPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    auto *fileRow = new QHBoxLayout;
    m_openWaveButton = new QPushButton(QStringLiteral("打开 WAV"), this);
    m_openWaveButton->setObjectName(QStringLiteral("analysisOpenWaveButton"));
    m_openPhotoButton = new QPushButton(QStringLiteral("选择背景照片"), this);
    m_openPhotoButton->setObjectName(QStringLiteral("analysisOpenPhotoButton"));
    m_fileLabel = new QLabel(QStringLiteral("尚未打开音频文件"), this);
    m_fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fileRow->addWidget(m_openWaveButton);
    fileRow->addWidget(m_openPhotoButton);
    fileRow->addWidget(m_fileLabel, 1);
    root->addLayout(fileRow);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    auto *leftScroll = new QScrollArea(splitter);
    leftScroll->setWidgetResizable(true);
    leftScroll->setMinimumWidth(285);
    leftScroll->setMaximumWidth(380);
    auto *settingsPanel = new QWidget(leftScroll);
    auto *settingsLayout = new QVBoxLayout(settingsPanel);

    auto *playbackGroup = new QGroupBox(QStringLiteral("单通道播放"), settingsPanel);
    auto *playbackLayout = new QFormLayout(playbackGroup);
    m_outputChannelCombo = new QComboBox(playbackGroup);
    m_outputChannelCombo->setObjectName(QStringLiteral("analysisOutputChannelCombo"));
    for (int channel = 0; channel < 48; ++channel) {
        m_outputChannelCombo->addItem(QStringLiteral("通道 %1").arg(channel + 1), channel);
    }
    m_volumeSpin = new QDoubleSpinBox(playbackGroup);
    m_volumeSpin->setRange(0.0, 1.0);
    m_volumeSpin->setSingleStep(0.05);
    playbackLayout->addRow(QStringLiteral("输出通道"), m_outputChannelCombo);
    playbackLayout->addRow(QStringLiteral("音量"), m_volumeSpin);
    settingsLayout->addWidget(playbackGroup);

    auto *algorithmGroup = new QGroupBox(QStringLiteral("声源定位"), settingsPanel);
    auto *algorithmLayout = new QFormLayout(algorithmGroup);
    m_locatorCombo = new QComboBox(algorithmGroup);
    m_locatorCombo->setObjectName(QStringLiteral("analysisLocatorCombo"));
    SoundSourceLocatorRegistry registry;
    registerBuiltInSoundSourceLocators(&registry);
    for (const LocatorDescriptor &descriptor : registry.descriptors()) {
        m_locatorCombo->addItem(descriptor.displayName, descriptor.id);
        m_locatorCombo->setItemData(m_locatorCombo->count() - 1,
                                    descriptor.description, Qt::ToolTipRole);
    }
    m_windowSamplesSpin = new QSpinBox(algorithmGroup);
    m_windowSamplesSpin->setRange(256, 16384);
    m_windowSamplesSpin->setSingleStep(256);
    m_updateIntervalSpin = new QSpinBox(algorithmGroup);
    m_updateIntervalSpin->setRange(20, 1000);
    m_updateIntervalSpin->setSuffix(QStringLiteral(" ms"));
    m_soundSpeedSpin = new QDoubleSpinBox(algorithmGroup);
    m_soundSpeedSpin->setRange(300.0, 380.0);
    m_soundSpeedSpin->setSuffix(QStringLiteral(" m/s"));
    m_smoothingSpin = new QDoubleSpinBox(algorithmGroup);
    m_smoothingSpin->setRange(0.0, 1.0);
    m_smoothingSpin->setSingleStep(0.05);
    m_confidenceSpin = new QDoubleSpinBox(algorithmGroup);
    m_confidenceSpin->setRange(0.0, 1.0);
    m_confidenceSpin->setSingleStep(0.05);
    m_neuralPeakEnhancementCheck =
        new QCheckBox(
            QStringLiteral("启用神经网络主峰锐化"),
            algorithmGroup);
    m_neuralPeakEnhancementCheck->setObjectName(
        QStringLiteral(
            "audioAnalysisNeuralPeakEnhancementCheck"));
    m_neuralPeakEnhancementStrengthSpin =
        new QDoubleSpinBox(algorithmGroup);
    m_neuralPeakEnhancementStrengthSpin->setObjectName(
        QStringLiteral(
            "audioAnalysisNeuralPeakEnhancementStrengthSpin"));
    m_neuralPeakEnhancementStrengthSpin->setRange(
        0.0, 1.0);
    m_neuralPeakEnhancementStrengthSpin->setDecimals(2);
    m_neuralPeakEnhancementStrengthSpin->setSingleStep(
        0.05);
    algorithmLayout->addRow(QStringLiteral("算法"), m_locatorCombo);
    algorithmLayout->addRow(QStringLiteral("窗口采样数"), m_windowSamplesSpin);
    algorithmLayout->addRow(QStringLiteral("更新周期"), m_updateIntervalSpin);
    algorithmLayout->addRow(QStringLiteral("声速"), m_soundSpeedSpin);
    algorithmLayout->addRow(QStringLiteral("平滑系数"), m_smoothingSpin);
    algorithmLayout->addRow(QStringLiteral("置信阈值"), m_confidenceSpin);
    algorithmLayout->addRow(
        m_neuralPeakEnhancementCheck);
    algorithmLayout->addRow(
        QStringLiteral("神经锐化强度"),
        m_neuralPeakEnhancementStrengthSpin);
    settingsLayout->addWidget(algorithmGroup);

    auto *cameraGroup = new QGroupBox(QStringLiteral("照片视角"), settingsPanel);
    auto *cameraLayout = new QFormLayout(cameraGroup);
    m_centerAzimuthSpin = angleSpin(cameraGroup, -180.0, 180.0);
    m_centerElevationSpin = angleSpin(cameraGroup, -89.0, 89.0);
    m_horizontalFovSpin = angleSpin(cameraGroup, 1.0, 179.0);
    m_horizontalFovSpin->setObjectName(QStringLiteral("cameraHorizontalFovSpin"));
    m_verticalFovSpin = angleSpin(cameraGroup, 1.0, 179.0);
    m_rollSpin = angleSpin(cameraGroup, -180.0, 180.0);
    cameraLayout->addRow(QStringLiteral("中心方位角"), m_centerAzimuthSpin);
    cameraLayout->addRow(QStringLiteral("中心俯仰角"), m_centerElevationSpin);
    cameraLayout->addRow(QStringLiteral("水平视场角"), m_horizontalFovSpin);
    cameraLayout->addRow(QStringLiteral("垂直视场角"), m_verticalFovSpin);
    cameraLayout->addRow(QStringLiteral("画面滚转角"), m_rollSpin);
    settingsLayout->addWidget(cameraGroup);
    settingsLayout->addStretch();
    leftScroll->setWidget(settingsPanel);

    auto *center = new QWidget(splitter);
    auto *centerLayout = new QVBoxLayout(center);
    m_photoWidget = new DirectionPhotoWidget(center);
    m_photoWidget->setObjectName(QStringLiteral("directionPhotoWidget"));
    m_directionLabel = new QLabel(QStringLiteral("方位角 --　俯仰角 --"), center);
    m_directionLabel->setAlignment(Qt::AlignCenter);
    m_waveform = new AudioWaveformWidget(center);
    m_waveform->setMinimumHeight(100);
    m_waveform->setMaximumHeight(150);
    centerLayout->addWidget(m_photoWidget, 1);
    centerLayout->addWidget(m_directionLabel);
    centerLayout->addWidget(m_waveform);
    splitter->addWidget(leftScroll);
    splitter->addWidget(center);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 1);

    auto *transport = new QHBoxLayout;
    m_playButton = new QPushButton(QStringLiteral("播放"), this);
    m_playButton->setObjectName(QStringLiteral("analysisPlayButton"));
    m_pauseButton = new QPushButton(QStringLiteral("暂停"), this);
    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_playButton->setEnabled(false);
    m_pauseButton->setEnabled(false);
    m_stopButton->setEnabled(false);
    m_positionSlider = new QSlider(Qt::Horizontal, this);
    m_positionSlider->setRange(0, 1'000'000);
    m_positionSlider->setEnabled(false);
    m_positionLabel = new QLabel(QStringLiteral("00:00.0 / 00:00.0"), this);
    transport->addWidget(m_playButton);
    transport->addWidget(m_pauseButton);
    transport->addWidget(m_stopButton);
    transport->addWidget(m_positionSlider, 1);
    transport->addWidget(m_positionLabel);
    root->addLayout(transport);
    m_statusLabel = new QLabel(QStringLiteral("等待打开 48 通道 WAV 文件"), this);
    root->addWidget(m_statusLabel);
}

void AudioAnalysisPage::connectUi()
{
    connect(m_openWaveButton, &QPushButton::clicked, this, &AudioAnalysisPage::chooseWaveFile);
    connect(m_openPhotoButton, &QPushButton::clicked, this, &AudioAnalysisPage::choosePhoto);
    connect(m_playButton, &QPushButton::clicked, this, &AudioAnalysisPage::playRequested);
    connect(m_pauseButton, &QPushButton::clicked, this, &AudioAnalysisPage::pauseRequested);
    connect(m_stopButton, &QPushButton::clicked, this, &AudioAnalysisPage::stopRequested);
    connect(m_outputChannelCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_settings.outputChannel = m_outputChannelCombo->itemData(index).toInt();
        emit outputChannelRequested(m_settings.outputChannel);
        persistSettings();
    });
    connect(m_volumeSpin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        m_settings.volume = value;
        emit volumeRequested(value);
        persistSettings();
    });
    connect(m_locatorCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_settings.locatorId = m_locatorCombo->itemData(index).toString();
        emit locatorRequested(m_settings.locatorId);
        persistSettings();
    });
    const QList<QAbstractSpinBox *> analysisSpins {
        m_windowSamplesSpin,
        m_updateIntervalSpin,
        m_soundSpeedSpin,
        m_smoothingSpin,
        m_confidenceSpin,
    };
    for (QAbstractSpinBox *spin : analysisSpins) {
        connect(spin, &QAbstractSpinBox::editingFinished,
                this, &AudioAnalysisPage::updateAnalysisConfiguration);
    }
    connect(
        m_neuralPeakEnhancementCheck,
        &QCheckBox::toggled,
        this,
        [this](bool enabled) {
        m_neuralPeakEnhancementStrengthSpin
            ->setEnabled(enabled);
        updateAnalysisConfiguration();
    });
    connect(
        m_neuralPeakEnhancementStrengthSpin,
        &QDoubleSpinBox::valueChanged,
        this,
        [this](double) {
        updateAnalysisConfiguration();
    });
    for (QDoubleSpinBox *spin : {m_centerAzimuthSpin, m_centerElevationSpin,
                                 m_horizontalFovSpin, m_verticalFovSpin, m_rollSpin}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { updateCamera(); });
    }
    connect(m_positionSlider, &QSlider::sliderMoved, this, [this] {
        if (!m_updatingPosition) {
            const qint64 sample = sliderSamplePosition();
            emit seekRequested(sample);
            emit analysisRequested(sample, ++m_requestId);
        }
    });
}

void AudioAnalysisPage::restoreSettings()
{
    m_settings = m_settingsStore != nullptr ? m_settingsStore->audioAnalysis()
                                            : AudioAnalysisSettings {};
    m_outputChannelCombo->setCurrentIndex(std::max(0, m_outputChannelCombo->findData(m_settings.outputChannel)));
    m_volumeSpin->setValue(m_settings.volume);
    m_locatorCombo->setCurrentIndex(std::max(0, m_locatorCombo->findData(m_settings.locatorId)));
    m_windowSamplesSpin->setValue(m_settings.windowSamples);
    m_updateIntervalSpin->setValue(m_settings.updateIntervalMs);
    m_soundSpeedSpin->setValue(m_settings.soundSpeedMetersPerSecond);
    m_smoothingSpin->setValue(m_settings.smoothingFactor);
    m_confidenceSpin->setValue(m_settings.confidenceThreshold);
    m_neuralPeakEnhancementCheck->setChecked(
        m_settings.neuralPeakEnhancementEnabled);
    m_neuralPeakEnhancementStrengthSpin->setValue(
        m_settings.neuralPeakEnhancementStrength);
    m_neuralPeakEnhancementStrengthSpin->setEnabled(
        m_settings.neuralPeakEnhancementEnabled);
    m_centerAzimuthSpin->setValue(m_settings.camera.centerAzimuthDegrees);
    m_centerElevationSpin->setValue(m_settings.camera.centerElevationDegrees);
    m_horizontalFovSpin->setValue(m_settings.camera.horizontalFovDegrees);
    m_verticalFovSpin->setValue(m_settings.camera.verticalFovDegrees);
    m_rollSpin->setValue(m_settings.camera.rollDegrees);
    m_photoWidget->setCameraView(m_settings.camera);
    m_photoWidget->setConfidenceThreshold(m_settings.confidenceThreshold);
    if (!m_settings.lastPhotoPath.isEmpty()) {
        QImageReader reader(m_settings.lastPhotoPath);
        m_photoWidget->setImage(reader.read());
    }
}

void AudioAnalysisPage::persistSettings()
{
    if (m_settingsStore != nullptr) {
        m_settingsStore->saveAudioAnalysis(m_settings);
    }
}

void AudioAnalysisPage::chooseWaveFile()
{
    const QString initial = m_settings.lastWavePath.isEmpty()
                                ? QString() : QFileInfo(m_settings.lastWavePath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开多通道 WAV"),
                                                      initial, QStringLiteral("WAV 音频 (*.wav)"));
    if (path.isEmpty()) {
        return;
    }
    m_waveform->clear();
    m_photoWidget->setDirections({});
    m_statusLabel->setText(QStringLiteral("正在读取 %1…").arg(QFileInfo(path).fileName()));
    emit loadFileRequested(path, ++m_requestId);
}

void AudioAnalysisPage::choosePhoto()
{
    const QString initial = m_settings.lastPhotoPath.isEmpty()
                                ? QString() : QFileInfo(m_settings.lastPhotoPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择定位背景照片"), initial,
        QStringLiteral("图像 (*.png *.jpg *.jpeg *.bmp *.webp)"));
    if (path.isEmpty()) {
        return;
    }
    QImageReader reader(path);
    const QImage image = reader.read();
    if (image.isNull()) {
        m_statusLabel->setText(QStringLiteral("无法读取照片：%1").arg(reader.errorString()));
        return;
    }
    m_settings.lastPhotoPath = path;
    m_photoWidget->setImage(image);
    persistSettings();
}

void AudioAnalysisPage::updateCamera()
{
    m_settings.camera.centerAzimuthDegrees = m_centerAzimuthSpin->value();
    m_settings.camera.centerElevationDegrees = m_centerElevationSpin->value();
    m_settings.camera.horizontalFovDegrees = m_horizontalFovSpin->value();
    m_settings.camera.verticalFovDegrees = m_verticalFovSpin->value();
    m_settings.camera.rollDegrees = m_rollSpin->value();
    m_photoWidget->setCameraView(m_settings.camera);
    persistSettings();
}

void AudioAnalysisPage::updateAnalysisConfiguration()
{
    m_settings.windowSamples = m_windowSamplesSpin->value();
    m_settings.updateIntervalMs = m_updateIntervalSpin->value();
    m_settings.soundSpeedMetersPerSecond = m_soundSpeedSpin->value();
    m_settings.smoothingFactor = m_smoothingSpin->value();
    m_settings.confidenceThreshold = m_confidenceSpin->value();
    m_settings.neuralPeakEnhancementEnabled =
        m_neuralPeakEnhancementCheck->isChecked();
    m_settings.neuralPeakEnhancementStrength =
        m_neuralPeakEnhancementStrengthSpin->value();
    m_photoWidget->setConfidenceThreshold(m_settings.confidenceThreshold);
    emit configurationRequested(m_settings);
    persistSettings();
    if (m_fileInfo.totalSamples > 0) {
        emit analysisRequested(sliderSamplePosition(), ++m_requestId);
    }
}

qint64 AudioAnalysisPage::sliderSamplePosition() const
{
    if (m_fileInfo.totalSamples <= 0) {
        return 0;
    }
    return qRound64(static_cast<double>(m_positionSlider->value())
                    / m_positionSlider->maximum() * m_fileInfo.totalSamples);
}

void AudioAnalysisPage::setSliderSamplePosition(qint64 samplePosition)
{
    if (m_fileInfo.totalSamples <= 0) {
        return;
    }
    m_updatingPosition = true;
    m_positionSlider->setValue(static_cast<int>(std::clamp(
        static_cast<double>(samplePosition) / m_fileInfo.totalSamples
            * m_positionSlider->maximum(),
        0.0, static_cast<double>(m_positionSlider->maximum()))));
    m_updatingPosition = false;
}

} // namespace MicArray
