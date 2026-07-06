#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVector>

#include "app/appsettings.h"
#include "app/logger.h"
#include "control/controlvalues.h"
#include "device/deviceclient.h"
#include "media/audioprocessor.h"
#include "media/audioframedecoder.h"
#include "media/videodecoder.h"
#include "ui/audiowaveformwidget.h"

namespace MicArray {

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

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
    void updateAudioSettingsFromUi();

private:
    void buildUi();
    QWidget *buildMonitorPage();
    QWidget *buildDebugPage();
    QWidget *buildCalibrationPage();
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

    AppSettings m_settings;
    Logger m_logger;
    DeviceClient m_client;
    VideoDecoder m_videoDecoder;
    AudioProcessor m_audioProcessor;
    QElapsedTimer m_connectionTimer;
    ControlValues m_controlValues;

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
    QLabel *m_videoLabel = nullptr;
    AudioWaveformWidget *m_waveformWidget = nullptr;
    QLabel *m_videoStatsLabel = nullptr;
    QLabel *m_audioStatsLabel = nullptr;
    QProgressBar *m_audioLevel = nullptr;
    QLabel *m_protocolStatsLabel = nullptr;
    QLabel *m_debugMetaLabel = nullptr;
    QLabel *m_debugHeadLabel = nullptr;
    QLabel *m_debugTailLabel = nullptr;
    QSpinBox *m_debugHistorySpin = nullptr;
    QPlainTextEdit *m_debugHistoryView = nullptr;
    QPlainTextEdit *m_debugErrorView = nullptr;
    QLabel *m_calibrationStatsLabel = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QPushButton *m_sendControlButton = nullptr;
    QPushButton *m_resetControlButton = nullptr;
    QCheckBox *m_lockControlCheck = nullptr;
    QSpinBox *m_controlSpins[ControlValues::ChannelCount] {};
    QDoubleSpinBox *m_sampleRateSpin = nullptr;
    QSpinBox *m_channelCountSpin = nullptr;
    QSpinBox *m_outputChannelSpin = nullptr;
    QSpinBox *m_averageWindowSpin = nullptr;
    QCheckBox *m_audioPlaybackCheck = nullptr;
    QComboBox *m_videoFormatCombo = nullptr;
    QVector<QLabel *> m_arrayCells;
    QVector<QLabel *> m_debugSegmentLabels;
    bool m_controlDirty = false;
};

} // namespace MicArray
