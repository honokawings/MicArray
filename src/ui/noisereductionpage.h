#pragma once

#include "app/appsettings.h"
#include "control/controlvalues.h"
#include "media/noisereductionworker.h"

#include <QImage>
#include <QThread>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

namespace MicArray {

class AudioWaveformWidget;
class NoiseReductionVideoWidget;
class PowerSpectrumWidget;

class NoiseReductionPage : public QWidget
{
    Q_OBJECT

public:
    explicit NoiseReductionPage(AppSettings *settings,
                                QWidget *parent = nullptr);
    ~NoiseReductionPage() override;

    void setActive(bool active);
    void setConnected(bool connected);
    void setControlLocked(bool locked);
    void setSampleRate(int sampleRate);
    void setVideoFrame(const QImage &image,
                       quint32 timestampSeconds,
                       quint32 timestampMicros);
    void submitRawAudioFrame(const QByteArray &payload,
                             quint32 timestampSeconds,
                             quint32 timestampMicros);
    void setControlSendResult(bool success, const QString &message);
    ControlValues currentControlValues() const;
    NoiseReductionSettings currentSettings() const { return m_settings; }
    int pendingAudioFrames() const { return m_pendingAudioFrames; }
    quint64 droppedAudioFrames() const { return m_droppedAudioFrames; }
    quint64 submittedAudioFrames() const { return m_submittedAudioFrames; }

signals:
    void configureWorkerRequested(const NoiseReductionSettings &settings,
                                  int sampleRate);
    void workerActiveRequested(bool active);
    void rawAudioFrameRequested(const QByteArray &payload,
                                quint32 timestampSeconds,
                                quint32 timestampMicros);
    void controlSendRequested(const ControlValues &values);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    static constexpr int MaximumPendingFrames = 32;

    void buildUi();
    void buildPresentationWindow();
    void connectUi();
    void restoreSettings();
    NoiseReductionSettings settingsFromUi() const;
    void applySettingsFromUi();
    void refreshProcessingAvailability();
    void refreshDirectionViews();
    void refreshControlAvailability();
    void handleVideoPoint(const QPointF &normalizedPoint);
    void requestControlSend();
    void showPlotFrame(const NoiseReductionPlotFrame &frame);
    void setPresentationMode(bool enabled);

    AppSettings *m_settingsStore = nullptr;
    NoiseReductionSettings m_settings;
    QThread m_workerThread;
    NoiseReductionWorker *m_worker = nullptr;
    bool m_active = false;
    bool m_connected = false;
    bool m_controlLocked = false;
    int m_sampleRate = 48000;
    int m_pendingAudioFrames = 0;
    quint64 m_droppedAudioFrames = 0;
    quint64 m_submittedAudioFrames = 0;
    QImage m_videoFrame;
    NoiseReductionPlotFrame m_plotFrame;

    NoiseReductionVideoWidget *m_videoView = nullptr;
    NoiseReductionVideoWidget *m_presentationVideo = nullptr;
    AudioWaveformWidget *m_firstWaveform = nullptr;
    AudioWaveformWidget *m_secondWaveform = nullptr;
    PowerSpectrumWidget *m_firstSpectrum = nullptr;
    PowerSpectrumWidget *m_secondSpectrum = nullptr;
    PowerSpectrumWidget *m_presentationFirstSpectrum = nullptr;
    PowerSpectrumWidget *m_presentationSecondSpectrum = nullptr;
    QWidget *m_presentationWindow = nullptr;
    QCheckBox *m_enabledCheck = nullptr;
    QComboBox *m_algorithmCombo = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QDoubleSpinBox *m_singleCoefficientSpin = nullptr;
    QDoubleSpinBox *m_dualB1CoefficientSpin = nullptr;
    QDoubleSpinBox *m_dualB2CoefficientSpin = nullptr;
    QCheckBox *m_monitorEnabledCheck = nullptr;
    QComboBox *m_monitorSourceCombo = nullptr;
    QDoubleSpinBox *m_monitorGainSpin = nullptr;
    QCheckBox *m_bandPassEnabledCheck = nullptr;
    QComboBox *m_bandPassPresetCombo = nullptr;
    QDoubleSpinBox *m_bandPassLowSpin = nullptr;
    QDoubleSpinBox *m_bandPassHighSpin = nullptr;
    QComboBox *m_firstPlotSourceCombo = nullptr;
    QComboBox *m_secondPlotSourceCombo = nullptr;
    QComboBox *m_directionSelectionCombo = nullptr;
    QDoubleSpinBox *m_directionSpins[6] {};
    QLabel *m_directionCodeLabels[6] {};
    QCheckBox *m_showMarkersCheck = nullptr;
    QDoubleSpinBox *m_mappingSpins[4] {};
    QDoubleSpinBox *m_cameraSpins[5] {};
    QPushButton *m_sendDirectionButton = nullptr;
    QPushButton *m_fullscreenButton = nullptr;
    QLabel *m_statusLabel = nullptr;
};

} // namespace MicArray
