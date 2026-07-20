#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QString>

class QTcpSocket;
class QTimer;

namespace MicArray {

class ReconnectBackoff
{
public:
    int nextDelayMs(double jitterUnit);
    void reset() { m_attempt = 0; }
    int attempt() const { return m_attempt; }

private:
    int m_attempt = 0;
};

enum class DeviceState;

class NetworkWorker : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 ReadBufferLimitBytes = 8 * 1024 * 1024;
    static constexpr qint64 PendingProtocolPauseBytes = 4 * 1024 * 1024;
    static constexpr qint64 PendingProtocolResumeBytes = 2 * 1024 * 1024;
    static constexpr qint64 DataTimeoutMs = 5000;
    static constexpr qint64 HealthyResetMs = 30000;

    explicit NetworkWorker(QObject *parent = nullptr);
    ~NetworkWorker() override;

public slots:
    void connectToHost(const QString &host, quint16 port, bool autoReconnect);
    void disconnectFromHost();
    void sendFrame(const QByteArray &frame, const QString &description);
    void sendHeartbeat();
    void onProtocolBytesProcessed(qint64 bytes);

signals:
    void stateChanged(DeviceState state);
    void bytesRead(const QByteArray &data);
    void logMessage(const QString &module, const QString &message);
    void warningMessage(const QString &module, const QString &message);
    void errorMessage(const QString &module, const QString &message);
    void bytesReceivedChanged(quint64 totalBytes, QDateTime lastReceiveTime);
    void transportConfigured(qint64 readBufferLimitBytes);
    void receiveBackpressureChanged(bool active, qint64 pendingProtocolBytes);
    void reconnectCountChanged(quint64 reconnectCount);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);
    void reconnect();
    void checkConnectionHealth();

private:
    void ensureTransport();
    void setReceiveBackpressure(bool active);
    void scheduleReconnect();
    void setState(DeviceState state);

    QTcpSocket *m_socket = nullptr;
    QTimer *m_heartbeatTimer = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    QTimer *m_healthTimer = nullptr;
    QString m_host = QStringLiteral("192.168.2.10");
    quint16 m_port = 9968;
    bool m_autoReconnect = true;
    bool m_manualDisconnect = false;
    bool m_receiveBackpressureActive = false;
    quint64 m_bytesReceived = 0;
    qint64 m_pendingProtocolBytes = 0;
    quint64 m_reconnectCount = 0;
    ReconnectBackoff m_backoff;
    QElapsedTimer m_lastReceiveTimer;
    QElapsedTimer m_connectedTimer;
    bool m_dataTimeoutReported = false;
    DeviceState m_state;
};

} // namespace MicArray
