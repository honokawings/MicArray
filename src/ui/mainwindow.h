#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVector>

#include "app/appsettings.h"
#include "app/logger.h"
#include "control/controlvalues.h"
#include "device/deviceclient.h"
#include "diagnostics/alertcoalescer.h"
#include "diagnostics/diagnosticexporter.h"
#include "media/audioprocessor.h"
#include "media/audiorecorder.h"
#include "media/audioframedecoder.h"
#include "media/mediaframequeue.h"
#include "media/videodecoder.h"
#include "monitoring/healthmonitor.h"
#include "ui/arrayheatmapwidget.h"
#include "ui/audioanalysispage.h"
#include "ui/audiowaveformwidget.h"
#include "ui/trendchartwidget.h"
#include "ui/videopreviewwidget.h"

namespace MicArray {

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void applyPreset(int index);
    void connectOrDisconnect();
    void sendControl();
    void resetControl();
    void markControlDirty();
    void updateConnectionState(DeviceState state);
    void updateStatistics();
    void appendLog(LogLevel level, const QString &module, const QString &message, const QString &formatted);
    void showVideoFrame(const QImage &image, double fps, quint32 timestampSeconds, quint32 timestampMicros);
    void updateAudioLevel(double peakLevel, qint64 bytesReceived, quint32 timestampSeconds, quint32 timestampMicros);
    void showArrayFrame(const DecodedAudioFrame &frame);
    void renderPendingVideoFrame();
    void renderPendingAudioFrame();
    void updateAudioSettingsFromUi();
    void updateHealthSnapshot();
    void enqueueAudioFrame(const HostFrame &frame);
    void dispatchNextAudioFrame();
    void dispatchNextVideoFrame();
    void togglePresentationMode();
    void applyControlPreset();
    void saveControlPreset();
    void deleteControlPreset();
    void restoreLastSentControl();
    void exportDiagnostics();
    void resetSettingsToDefaults();
    void startRecording();
    void stopRecording();
    void chooseRecordingDirectory();
    void updateRecordingState(RecordingState state);
    void updateRecordingStatistics(const RecordingStatistics &statistics);
    void saveGainProfile();
    void loadGainProfile();

private:
    void buildUi();
    QWidget *buildMonitorPage();
    QWidget *buildDebugPage();
    QWidget *buildCalibrationPage();
    QWidget *buildRecordingPage();
    void connectSignals();
    void loadSettings();
    void saveSettings();
    void updateDebugFrame(const HostFrame &frame);
    void updateDebugError(const ParseError &error);
    void updateCalibrationGrid(const DecodedAudioFrame &frame);
    ControlValues collectControlValues() const;
    void setControlsEnabled(bool enabled);
    static QString bytesToHexPreview(const QByteArray &bytes);
    static QString calibrationColor(double normalized);
    static QString frameTypeDisplayName(quint16 rawType);
    void updateDebugSegments(const HostFrame &frame);
    void flashDebugSegments();
    void refreshControlPresets();
    void appendAlert(LogLevel level, const QString &message);
    QVector<qint16> currentControlVector() const;
    void setControlVector(const QVector<qint16> &values);
    QVector<int> selectedRecordingChannels() const;
    void setRecordingConfigurationEnabled(bool enabled);
    void updateRecordingStartEnabled();
    void persistGainConfiguration();

    AppSettings m_settings;
    Logger m_logger;
    DeviceClient m_client;
    AudioRecorder m_audioRecorder;
    QThread m_audioThread;
    QThread m_videoThread;
    VideoDecoder *m_videoDecoder = nullptr;
    AudioProcessor *m_audioProcessor = nullptr;
    QElapsedTimer m_connectionTimer;
    int m_videoDecodedFrames = 0;
    qint64 m_audioBytesReceived = 0;
    quint64 m_audioDroppedFrames = 0;
    quint64 m_videoDroppedFrames = 0;
    int m_audioOutputChannel = 0;
    int m_audioAverageWindowFrames = 100;
    GainConfiguration m_gainConfiguration;
    QTimer m_videoRenderTimer;
    QTimer m_audioRenderTimer;
    QImage m_pendingVideoImage;
    double m_pendingVideoFps = 0.0;
    quint32 m_pendingVideoTimestampSeconds = 0;
    quint32 m_pendingVideoTimestampMicros = 0;
    bool m_hasPendingVideoFrame = false;
    DecodedAudioFrame m_pendingAudioFrame;
    bool m_hasPendingAudioFrame = false;
    HealthMonitor m_healthMonitor;
    HealthSnapshot m_healthSnapshot;
    HealthLevel m_lastHealthLevel = HealthLevel::Disconnected;
    QTimer m_healthTimer;
    QElapsedTimer m_sessionTimer;
    static constexpr int MaxAudioFramesInFlight = 16;
    MediaFrameQueue m_audioFrameQueue {32};
    MediaFrameQueue m_videoFrameQueue {1};
    int m_audioFramesInFlight = 0;
    bool m_videoProcessing = false;
    QVector<qint16> m_lastSentControlValues;
    bool m_hasLastSentControl = false;
    bool m_presentationMode = false;
    AlertCoalescer m_alertCoalescer;
    QStringList m_alertEvents;
    QDateTime m_sessionStartedAt;
    double m_peakReceiveMbps = 0.0;

    QComboBox *m_presetCombo = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QCheckBox *m_autoReconnectCheck = nullptr;
    QPushButton *m_connectButton = nullptr;
    QLabel *m_stateLabel = nullptr;
    QLabel *m_endpointLabel = nullptr;
    QLabel *m_heartbeatLabel = nullptr;
    QLabel *m_rateLabel = nullptr;
    QLabel *m_errorLabel = nullptr;
    QLabel *m_healthLabel = nullptr;
    VideoPreviewWidget *m_videoLabel = nullptr;
    AudioWaveformWidget *m_waveformWidget = nullptr;
    ArrayHeatmapWidget *m_dashboardHeatmap = nullptr;
    TrendChartWidget *m_trendChart = nullptr;
    QLabel *m_videoStatsLabel = nullptr;
    QLabel *m_audioStatsLabel = nullptr;
    QProgressBar *m_audioLevel = nullptr;
    QLabel *m_protocolStatsLabel = nullptr;
    QLabel *m_debugMetaLabel = nullptr;
    QLabel *m_debugHeadLabel = nullptr;
    QLabel *m_debugTailLabel = nullptr;
    QLabel *m_debugPostTailLabel = nullptr;
    QSpinBox *m_debugHistorySpin = nullptr;
    QPlainTextEdit *m_debugHistoryView = nullptr;
    QPlainTextEdit *m_debugErrorView = nullptr;
    QLabel *m_calibrationStatsLabel = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QPlainTextEdit *m_alertTimeline = nullptr;
    QPushButton *m_sendControlButton = nullptr;
    QPushButton *m_resetControlButton = nullptr;
    QPushButton *m_undoControlButton = nullptr;
    QComboBox *m_controlPresetCombo = nullptr;
    QPushButton *m_applyPresetButton = nullptr;
    QPushButton *m_savePresetButton = nullptr;
    QPushButton *m_deletePresetButton = nullptr;
    QCheckBox *m_lockControlCheck = nullptr;
    QSpinBox *m_controlSpins[ControlValues::ChannelCount] {};
    QDoubleSpinBox *m_sampleRateSpin = nullptr;
    QSpinBox *m_channelCountSpin = nullptr;
    QSpinBox *m_outputChannelSpin = nullptr;
    QSpinBox *m_averageWindowSpin = nullptr;
    QCheckBox *m_audioPlaybackCheck = nullptr;
    QComboBox *m_videoFormatCombo = nullptr;
    QPushButton *m_resetSettingsButton = nullptr;
    QVector<QLabel *> m_arrayCells;
    QVector<QLabel *> m_debugSegmentLabels;
    QTabWidget *m_mainTabs = nullptr;
    AudioAnalysisPage *m_audioAnalysisPage = nullptr;
    QPushButton *m_presentationButton = nullptr;
    QPushButton *m_exportDiagnosticsButton = nullptr;
    QLineEdit *m_recordingDirectoryEdit = nullptr;
    QPushButton *m_recordingBrowseButton = nullptr;
    QPushButton *m_recordingStartButton = nullptr;
    QPushButton *m_recordingStopButton = nullptr;
    QLabel *m_recordingStateLabel = nullptr;
    QLabel *m_recordingStatisticsLabel = nullptr;
    QLabel *m_recordingErrorLabel = nullptr;
    QWidget *m_recordingSelectionCommands = nullptr;
    QVector<QToolButton *> m_recordingChannelButtons;
    bool m_controlDirty = false;
};

} // namespace MicArray
