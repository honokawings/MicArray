#pragma once

#include "pipeline.h"
#include "views.h"
#include "media/audiomonitorbuffer.h"

#include <QMainWindow>
#include <QMediaCaptureSession>
#include <QSettings>
#include <memory>

class QAudioSink;
class QCamera;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QTimer;
class QVideoSink;

namespace MicArray::Array128 {

class Sys3Panel;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    Pipeline &pipeline() { return m_pipeline; }
    void startReceiving();
    void saveDiagnosticReport(const QString &path);
    void openSerial(const QString &port);
private:
    QWidget *buildMonitorPage();
    QWidget *buildLocalizationPage();
    QWidget *buildDenoisePage();
    QWidget *buildRecordingPage();
    QWidget *buildGeometryPage();
    QWidget *buildDebugPage();
    QWidget *buildOfflinePage();
    void refresh();
    void applySettings();
    void loadSettings();
    void saveSettings();
    void toggleRecording();
    void toggleCamera();
    void applyTheme(bool dark);
    void resetAudio();
    void pumpAudio();
    void calibratePoint(const QPointF &point);
    void loadGeometry();
    void showError(const QString &text);
    void updateCalibrationTable();
    void updateGeometryTable();

    QSettings m_store;
    Pipeline m_pipeline;
    ProcessingSettings m_processing;
    QTabWidget *m_pages = nullptr;
    QTabWidget *m_configTabs = nullptr;
    QTabWidget *m_debugTabs = nullptr;
    Sys3Panel *m_serial = nullptr;
    QTableWidget *m_geometryTable = nullptr;
    QLineEdit *m_bind = nullptr, *m_source = nullptr;
    QSpinBox *m_port = nullptr, *m_sourcePort = nullptr;
    QPushButton *m_connect = nullptr;
    QLabel *m_health = nullptr, *m_rates = nullptr, *m_geometryLabel = nullptr;
    QLabel *m_endpoint = nullptr;
    QComboBox *m_cameras = nullptr;
    QPushButton *m_cameraButton = nullptr;
    std::unique_ptr<QCamera> m_camera;
    QMediaCaptureSession m_captureSession;
    QVideoSink *m_videoSink = nullptr;
    FieldView *m_monitorView = nullptr, *m_localView = nullptr, *m_noiseView = nullptr;
    ChannelGrid *m_channelGrid = nullptr;
    WaveView *m_wave = nullptr, *m_spectrum = nullptr;
    QSpinBox *m_monitorChannel = nullptr, *m_bufferMs = nullptr;
    QCheckBox *m_monitorEnabled = nullptr;
    QComboBox *m_monitorSource = nullptr;
    QDoubleSpinBox *m_monitorGain = nullptr;
    QCheckBox *m_localEnabled = nullptr;
    QDoubleSpinBox *m_threshold = nullptr, *m_focus = nullptr, *m_soundSpeed = nullptr;
    QSpinBox *m_gridColumns = nullptr, *m_gridRows = nullptr;
    QLabel *m_location = nullptr;
    std::array<QDoubleSpinBox *, 5> m_cameraValues {};
    QTableWidget *m_calibrationTable = nullptr;
    QCheckBox *m_collectCalibration = nullptr, *m_showCalibration = nullptr;
    QVector<VideoCalibrationSample> m_calibration;
    QVector<LocalizationResult> m_recentDirections;
    QCheckBox *m_denoiseEnabled = nullptr, *m_dual = nullptr, *m_adaptive = nullptr;
    QCheckBox *m_bandpass = nullptr, *m_logmmse = nullptr, *m_agc = nullptr;
    QDoubleSpinBox *m_b1 = nullptr, *m_b2 = nullptr, *m_lowHz = nullptr, *m_highHz = nullptr;
    QListWidget *m_order = nullptr;
    std::array<QDoubleSpinBox *, 6> m_directionValues {};
    QComboBox *m_activeTarget = nullptr;
    QLineEdit *m_recordDirectory = nullptr;
    QComboBox *m_recordSource = nullptr, *m_recordLayout = nullptr;
    QListWidget *m_recordTracks = nullptr;
    QPushButton *m_recordButton = nullptr, *m_quickRecord = nullptr;
    QLabel *m_recordStatus = nullptr;
    QLabel *m_commDetails = nullptr, *m_qualityDetails = nullptr, *m_selftestDetails = nullptr, *m_hardwareDetails = nullptr;
    QProgressBar *m_selftestProgress = nullptr;
    QTableWidget *m_qualityTable = nullptr;
    QPlainTextEdit *m_packetDetails = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QString m_lastError;
    std::unique_ptr<QAudioSink> m_audio;
    QIODevice *m_audioDevice = nullptr;
    AudioMonitorBuffer m_audioBuffer;
    quint64 m_seenResult = 0;
    quint64 m_seenRun = 0;
    bool m_receiving = false, m_loading = true;
    bool m_skipSettingsSave = false;
    QTimer *m_refresh = nullptr;
    QTimer *m_audioTimer = nullptr;
    std::unique_ptr<QThread> m_offlineThread;
    quint64 m_offlineGeneration = 0;
};

} // namespace MicArray::Array128
