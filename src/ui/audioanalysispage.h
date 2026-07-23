#pragma once

#include "analysis/audioanalysisworker.h"
#include "app/appsettings.h"

#include <QThread>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;

namespace MicArray {

class AudioWaveformWidget;
class DirectionPhotoWidget;

class AudioAnalysisPage : public QWidget
{
    Q_OBJECT

public:
    explicit AudioAnalysisPage(AppSettings *settings, QWidget *parent = nullptr);
    ~AudioAnalysisPage() override;

    void setActive(bool active);

signals:
    void loadFileRequested(const QString &path, quint64 requestId);
    void playRequested();
    void pauseRequested();
    void stopRequested();
    void seekRequested(qint64 samplePosition);
    void outputChannelRequested(int channel);
    void volumeRequested(double volume);
    void locatorRequested(const QString &locatorId);
    void configurationRequested(const AudioAnalysisSettings &settings);
    void analysisRequested(qint64 centerSample, quint64 requestId);

private:
    void buildUi();
    void connectUi();
    void restoreSettings();
    void persistSettings();
    void chooseWaveFile();
    void choosePhoto();
    void updateCamera();
    void updateAnalysisConfiguration();
    qint64 sliderSamplePosition() const;
    void setSliderSamplePosition(qint64 samplePosition);

    AppSettings *m_settingsStore = nullptr;
    AudioAnalysisSettings m_settings;
    QThread m_workerThread;
    AudioAnalysisWorker *m_worker = nullptr;
    WaveFileInfo m_fileInfo;
    quint64 m_requestId = 0;
    bool m_active = false;
    bool m_updatingPosition = false;

    QPushButton *m_openWaveButton = nullptr;
    QPushButton *m_openPhotoButton = nullptr;
    QPushButton *m_playButton = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QComboBox *m_outputChannelCombo = nullptr;
    QComboBox *m_locatorCombo = nullptr;
    QDoubleSpinBox *m_volumeSpin = nullptr;
    QSpinBox *m_windowSamplesSpin = nullptr;
    QSpinBox *m_updateIntervalSpin = nullptr;
    QDoubleSpinBox *m_soundSpeedSpin = nullptr;
    QDoubleSpinBox *m_smoothingSpin = nullptr;
    QDoubleSpinBox *m_confidenceSpin = nullptr;
    QDoubleSpinBox *m_centerAzimuthSpin = nullptr;
    QDoubleSpinBox *m_centerElevationSpin = nullptr;
    QDoubleSpinBox *m_horizontalFovSpin = nullptr;
    QDoubleSpinBox *m_verticalFovSpin = nullptr;
    QDoubleSpinBox *m_rollSpin = nullptr;
    QSlider *m_positionSlider = nullptr;
    QLabel *m_fileLabel = nullptr;
    QLabel *m_positionLabel = nullptr;
    QLabel *m_directionLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    AudioWaveformWidget *m_waveform = nullptr;
    DirectionPhotoWidget *m_photoWidget = nullptr;
};

} // namespace MicArray
