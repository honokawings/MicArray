#pragma once

#include "sys3client.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

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
    void setCaptureBusy(bool busy) { m_captureBusy = busy; }
signals:
    void aboutToReset();
private:
    void refresh();
    void refreshPorts();
    void sendCommand(char command);
    Sys3Client m_client;
    QComboBox *m_ports;
    QPushButton *m_connect, *m_scan;
    std::array<QPushButton *, 5> m_commands {};
    QLabel *m_status, *m_freshness, *m_commandStatus;
    QTableWidget *m_details, *m_words;
    QPlainTextEdit *m_log;
    bool m_captureBusy = false;
    QStringList m_seenLog;
};

} // namespace MicArray::Array128
