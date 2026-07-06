#pragma once

#include <QDateTime>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>

#include "control/controlvalues.h"
#include "protocol/protocolcodec.h"

namespace MicArray {

enum class DeviceState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Error,
};

QString deviceStateName(DeviceState state);

class DeviceClient : public QObject
{
    Q_OBJECT

public:
    explicit DeviceClient(QObject *parent = nullptr);

    void connectToDevice(const QString &host, quint16 port);
    void disconnectFromDevice();
    bool sendControl(const ControlValues &values);

    void setAutoReconnect(bool enabled) { m_autoReconnect = enabled; }
    bool autoReconnect() const { return m_autoReconnect; }
    DeviceState state() const { return m_state; }
    QString host() const { return m_host; }
    quint16 port() const { return m_port; }
    QDateTime lastReceiveTime() const { return m_lastReceiveTime; }
    quint64 bytesReceived() const { return m_bytesReceived; }
    quint64 framesReceived() const { return m_framesReceived; }
    quint64 parseErrorCount() const { return m_parseErrorCount; }

signals:
    void stateChanged(DeviceState state);
    void logMessage(const QString &module, const QString &message);
    void warningMessage(const QString &module, const QString &message);
    void errorMessage(const QString &module, const QString &message);
    void parseErrorReceived(const ParseError &error);
    void frameReceived(const HostFrame &frame);
    void audioFrameReceived(const HostFrame &frame);
    void videoFrameReceived(const HostFrame &frame);
    void statisticsChanged();

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);
    void sendHeartbeat();
    void reconnect();

private:
    void setState(DeviceState state);
    void scheduleReconnect();

    QTcpSocket m_socket;
    QTimer m_heartbeatTimer;
    QTimer m_reconnectTimer;
    ProtocolCodec m_codec;
    DeviceState m_state = DeviceState::Disconnected;
    QString m_host = QStringLiteral("192.168.2.10");
    quint16 m_port = 9968;
    bool m_autoReconnect = true;
    bool m_manualDisconnect = false;
    QDateTime m_lastReceiveTime;
    quint64 m_bytesReceived = 0;
    quint64 m_framesReceived = 0;
    quint64 m_parseErrorCount = 0;
};

} // namespace MicArray
