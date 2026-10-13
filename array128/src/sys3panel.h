#pragma once

#include "sys3client.h"

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QSpinBox;

namespace MicArray::Array128 {

class Sys3Panel final : public QWidget
{
    Q_OBJECT
public:
    explicit Sys3Panel(const QString &preferredPort, QWidget *parent = nullptr);
    void openPort(const QString &port);
    QString port() const;
    QString summary() const;
    QJsonObject json() const { return m_client.json(); }
    void setCaptureBusy(bool busy) { m_captureBusy = busy; m_client.setCaptureBusy(busy); }
    bool acquisitionChangePending() const;
signals:
    void aboutToReset();
private:
    void refresh();
    void refreshPorts();
    void sendCommand(char command);
    void sendAudio(AudioUartAction action);
    QWidget *buildAudioPanel();
    Sys3Client m_client;
    QComboBox *m_ports;
    QPushButton *m_connect, *m_scan;
    std::array<QPushButton *, 5> m_commands {};
    QLabel *m_status, *m_commandStatus;
    QTableWidget *m_details, *m_words;
    QTableWidget *m_audioValues = nullptr;
    QComboBox *m_rawSource = nullptr;
    QSpinBox *m_i2sChannel = nullptr;
    QDoubleSpinBox *m_evenGain = nullptr, *m_oddGain = nullptr;
    std::array<QPushButton *, 5> m_audioCommands {};
    QPushButton *m_loadAudioRequest = nullptr, *m_unityGain = nullptr;
    QLabel *m_audioStatus = nullptr, *m_audioCounters = nullptr, *m_dcStatus = nullptr;
    QPlainTextEdit *m_log;
    bool m_captureBusy = false;
    QStringList m_seenLog;
};

} // namespace MicArray::Array128
