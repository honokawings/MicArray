#pragma once

#include "app/appsettings.h"
#include "control/controlvalues.h"
#include "media/noisereductionworker.h"
#include "media/noisereductionmailbox.h"
#include "control/pldenoisesession.h"

#include <QImage>
#include <QThread>
#include <QWidget>

#include <deque>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;

namespace MicArray {

class AudioWaveformWidget;
class NoiseReductionVideoWidget;
class PowerSpectrumWidget;
class ProcessingNodeListWidget;

class NoiseReductionPage : public QWidget
{
    Q_OBJECT

public:
    explicit NoiseReductionPage(AppSettings *settings,
                                QWidget *parent = nullptr);
    ~NoiseReductionPage() override;

    void setActive(bool active);
    void setRecordingRequired(
        bool required,
        const QVector<NoiseReductionDataFlowStage> &stages);
    void setQuickRecordingState(bool active,
                                bool canStart,
                                const QString &message);
    void setConnected(bool connected);
    void setControlLocked(bool locked);
    void setSampleRate(int sampleRate);
    void setVideoFrame(const QImage &image,
                       quint32 timestampSeconds,
                       quint32 timestampMicros);
    void submitRawAudioFrame(const QByteArray &payload,
                             quint32 timestampSeconds,
                             quint32 timestampMicros);
    void submitPlSidebandFrame(const QByteArray &payload,
                               quint32 timestampSeconds, quint32 timestampMicros);
    void setControlSendResult(bool success, const QString &message);
    void showPresentation();
    ControlValues currentControlValues() const;
    NoiseReductionSettings currentSettings() const { return m_settings; }
    int pendingAudioFrames() const {
        const auto state = m_mailbox->snapshot();
        return state.queuedAudioFrames + state.inFlight - state.controlInFlight;
    }
    quint64 droppedAudioFrames() const { return m_droppedAudioFrames; }
    quint64 submittedAudioFrames() const { return m_mailbox->snapshot().submittedAudioFrames; }

signals:
    void configureWorkerRequested(const NoiseReductionSettings &settings,
                                  int sampleRate);
    void workerActiveRequested(bool active);
    void rawAudioFrameRequested(const QByteArray &payload,
                                quint32 timestampSeconds,
                                quint32 timestampMicros);
    void plBeamFrameRequested(const PlBeamFrame &frame);
    void plWienerFrameRequested(const PlWienerFrame &frame);
    void plFinalFrameRequested(const PlFinalFrame &frame);
    void controlSendRequested(const ControlValues &values);
    void noiseRecordingFrameReady(
        const NoiseReductionRecordingFrame &frame);
    void quickRecordingToggleRequested();
    void settingsChanged();
    void persistentPlDiagnostic(const QString &message);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    static constexpr int MaximumPendingFrames = 32;
    static constexpr int MaximumBufferedPlSamples = 96000;
    static constexpr int MaximumBufferedReferenceSamples = 96000;
    static constexpr int MaximumQueuedAuthorizations = 64;

    enum class WorkKind { Raw, Beam, Wiener, Final, Authorize };
    struct PendingWork {
        WorkKind kind = WorkKind::Raw;
        quint64 generation = 0;
        int samples = 0;
        bool referenceOnly = false;
        QByteArray raw;
        quint32 seconds = 0;
        quint32 micros = 0;
        PlBeamFrame beam;
        PlWienerFrame wiener;
        PlFinalFrame final;
        quint16 stream = 0;
        bool restart = false;
    };
    void enqueuePlWork(PendingWork work);
    void enqueueReferenceWork(PendingWork work);
    bool enqueueAuthorization(quint16 stream, bool restart);
    void submitWorkerWork(PendingWork work);
    bool enqueueWorkerControl(std::function<void()> operation, bool authorization = false);
    void clearQueuedAudioWork(const QString &reason);
    void updateMailboxProperties();
    void logPlDiagnostic(const QString &event, const QString &reason = {});
    void scheduleQueuePeakDiagnostic();

    void buildUi();
    void buildPresentationWindow();
    void connectUi();
    void restoreSettings();
    NoiseReductionSettings settingsFromUi() const;
    void applySettingsFromUi();
    bool workerShouldBeActive() const;
    void updateWorkerActive(bool configurePl = true);
    void refreshStatusLabel();
    void refreshProcessingAvailability();
    void refreshNodeSummaries();
    void showNodeParameters(int nodeId);
    bool usesLocalizationCalibration() const;
    CameraViewSettings effectiveDirectionCamera() const;
    void refreshDirectionProjection();
    void refreshDirectionViews();
    void refreshControlAvailability();
    void handleVideoPoint(NoiseReductionVideoWidget *view,
                          const QPointF &normalizedPoint);
    void handleDirectionDragPoint(
        NoiseReductionVideoWidget *view,
        NoiseDirectionSelection selection,
        const QPointF &normalizedPoint,
        bool finished);
    void requestControlSend();
    void configurePlMode(bool force = false);
    void showPlotFrame(const NoiseReductionPlotFrame &frame);
    void setPresentationMode(bool enabled);

    AppSettings *m_settingsStore = nullptr;
    NoiseReductionSettings m_settings;
    QThread m_workerThread;
    NoiseReductionWorker *m_worker = nullptr;
    NoiseReductionMailbox *m_mailbox = nullptr;
    PlDenoiseTransport *m_plTransport = nullptr;
    PlDenoiseSession *m_plSession = nullptr;
    QTimer *m_plWatchdog = nullptr;
    QElapsedTimer m_plStarted;
    QElapsedTimer m_plLastStatus;
    QString m_plStatusMessage;
    quint64 m_workGeneration = 0;
    qint64 m_plBufferedSamples = 0;
    qint64 m_referenceBufferedSamples = 0;
    qint64 m_plQueuedSamples = 0;
    qint64 m_referenceQueuedSamples = 0;
    qint64 m_plQueuePeakSamples = 0;
    qint64 m_referenceQueuePeakSamples = 0;
    int m_queuedAuthorizations = 0;
    int m_inFlightAuthorizations = 0;
    bool m_queueDiagnosticScheduled = false;
    bool m_faultDiagnosticLatched = false;
    quint64 m_plFaultCount = 0;
    quint16 m_diagnosticResetTransaction = 0;
    QString m_lastWorkerWarning;
    quint64 m_workerWarningRepeats = 0;
    quint64 m_acceptedPlFrames = 0;
    quint64 m_completedPlFrames = 0;
    quint64 m_referenceDroppedFrames = 0;
    PlDenoiseStatus m_lastPlStatus;
    bool m_active = false;
    bool m_recordingRequired = false;
    bool m_workerActive = false;
    QVector<NoiseReductionDataFlowStage> m_recordingStages;
    QString m_processingStatusMessage;
    QString m_recordingStatusMessage;
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
    QComboBox *m_inputModeCombo = nullptr;
    QCheckBox *m_reuseLocalizationCalibrationCheck = nullptr;
    QComboBox *m_algorithmCombo = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QDoubleSpinBox *m_singleCoefficientSpin = nullptr;
    QDoubleSpinBox *m_dualB1CoefficientSpin = nullptr;
    QDoubleSpinBox *m_dualB2CoefficientSpin = nullptr;
    QCheckBox *m_monitorEnabledCheck = nullptr;
    QComboBox *m_monitorSourceCombo = nullptr;
    QDoubleSpinBox *m_monitorGainSpin = nullptr;
    QSpinBox *m_monitorBufferSpin = nullptr;
    QPushButton *m_quickRecordingButton = nullptr;
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
    QTabWidget *m_controlTabs = nullptr;
    ProcessingNodeListWidget *m_processingNodeList = nullptr;
    QStackedWidget *m_nodeParameterStack = nullptr;
    QCheckBox *m_logMmseEnabledCheck = nullptr;
    QComboBox *m_logMmseFftSizeCombo = nullptr;
    QDoubleSpinBox *m_logMmseOverlapSpin = nullptr;
    QDoubleSpinBox *m_logMmsePriorSnrSpin = nullptr;
    QDoubleSpinBox *m_logMmseNoiseSmoothingSpin = nullptr;
    QDoubleSpinBox *m_logMmseMinimumGainSpin = nullptr;
    QCheckBox *m_agcEnabledCheck = nullptr;
    QDoubleSpinBox *m_agcTargetSpin = nullptr;
    QDoubleSpinBox *m_agcMaximumGainSpin = nullptr;
    QDoubleSpinBox *m_agcMaximumAttenuationSpin = nullptr;
    QDoubleSpinBox *m_agcAttackSpin = nullptr;
    QDoubleSpinBox *m_agcReleaseSpin = nullptr;
    QDoubleSpinBox *m_agcLimiterSpin = nullptr;
    QVector<QWidget *> m_wheelBlockedControls;
};

} // namespace MicArray
