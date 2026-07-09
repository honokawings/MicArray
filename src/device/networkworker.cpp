#include "device/networkworker.h"

#include "device/deviceclient.h"
#include "protocol/protocolcodec.h"

#include <QMetaObject>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>

namespace MicArray {

NetworkWorker::NetworkWorker(QObject *parent)
    : QObject(parent)
    , m_state(DeviceState::Disconnected)
{
}

NetworkWorker::~NetworkWorker()
{
    if (m_heartbeatTimer != nullptr) {
        m_heartbeatTimer->stop();
    }
    if (m_reconnectTimer != nullptr) {
        m_reconnectTimer->stop();
    }
    if (m_socket != nullptr) {
        m_socket->abort();
    }
}

void NetworkWorker::connectToHost(const QString &host, quint16 port, bool autoReconnect)
{
    ensureTransport();
    m_manualDisconnect = false;
    m_host = host;
    m_port = port;
    m_autoReconnect = autoReconnect;
    m_bytesReceived = 0;
    m_pendingProtocolBytes = 0;
    setReceiveBackpressure(false);

    m_reconnectTimer->stop();
    m_socket->abort();
    setState(DeviceState::Connecting);
    emit logMessage(QStringLiteral("device"), QStringLiteral("Connecting to %1:%2").arg(m_host).arg(m_port));
    m_socket->connectToHost(m_host, m_port);
}

void NetworkWorker::disconnectFromHost()
{
    ensureTransport();
    m_manualDisconnect = true;
    m_pendingProtocolBytes = 0;
    setReceiveBackpressure(false);
    m_reconnectTimer->stop();
    m_heartbeatTimer->stop();
    m_socket->disconnectFromHost();
    if (m_socket->state() == QAbstractSocket::UnconnectedState) {
        setState(DeviceState::Disconnected);
    }
}

void NetworkWorker::sendFrame(const QByteArray &frame, const QString &description)
{
    ensureTransport();
    if (m_state != DeviceState::Connected || m_socket->state() != QAbstractSocket::ConnectedState) {
        emit warningMessage(QStringLiteral("device"), QStringLiteral("Cannot send %1 while device is disconnected").arg(description));
        return;
    }

    const qint64 written = m_socket->write(frame);
    m_socket->flush();
    if (written != frame.size()) {
        emit errorMessage(QStringLiteral("device"), QStringLiteral("%1 write failed").arg(description));
        return;
    }

    if (!description.isEmpty() && description != QStringLiteral("heartbeat")) {
        emit logMessage(QStringLiteral("device"), QStringLiteral("%1 sent").arg(description));
    }
}

void NetworkWorker::sendHeartbeat()
{
    ensureTransport();
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    m_socket->write(ProtocolCodec::encodeHeartbeat());
    m_socket->flush();
}

void NetworkWorker::onProtocolBytesProcessed(qint64 bytes)
{
    m_pendingProtocolBytes = std::max<qint64>(0, m_pendingProtocolBytes - std::max<qint64>(0, bytes));
    if (m_receiveBackpressureActive && m_pendingProtocolBytes <= PendingProtocolResumeBytes) {
        setReceiveBackpressure(false);
        if (m_socket != nullptr && m_socket->bytesAvailable() > 0) {
            QMetaObject::invokeMethod(this, "onReadyRead", Qt::QueuedConnection);
        }
    }
}

void NetworkWorker::onConnected()
{
    setState(DeviceState::Connected);
    emit logMessage(QStringLiteral("device"), QStringLiteral("Connected to %1:%2").arg(m_host).arg(m_port));
    sendHeartbeat();
    m_heartbeatTimer->start();
}

void NetworkWorker::onDisconnected()
{
    if (m_heartbeatTimer != nullptr) {
        m_heartbeatTimer->stop();
    }
    emit warningMessage(QStringLiteral("device"), QStringLiteral("Disconnected from device"));
    if (m_manualDisconnect || !m_autoReconnect) {
        setState(DeviceState::Disconnected);
    } else {
        scheduleReconnect();
    }
}

void NetworkWorker::onReadyRead()
{
    const QByteArray data = m_socket->readAll();
    if (data.isEmpty()) {
        return;
    }

    m_bytesReceived += static_cast<quint64>(data.size());
    m_pendingProtocolBytes += data.size();
    emit bytesReceivedChanged(m_bytesReceived, QDateTime::currentDateTime());
    emit bytesRead(data);
    if (m_pendingProtocolBytes >= PendingProtocolPauseBytes) {
        setReceiveBackpressure(true);
    }
}

void NetworkWorker::onSocketError(QAbstractSocket::SocketError)
{
    emit errorMessage(QStringLiteral("device"), m_socket->errorString());
    if (!m_manualDisconnect && m_autoReconnect) {
        scheduleReconnect();
    } else {
        setState(DeviceState::Error);
    }
}

void NetworkWorker::reconnect()
{
    connectToHost(m_host, m_port, m_autoReconnect);
}

void NetworkWorker::ensureTransport()
{
    if (m_socket != nullptr) {
        return;
    }

    m_socket = new QTcpSocket(this);
    m_socket->setReadBufferSize(ReadBufferLimitBytes);
    m_heartbeatTimer = new QTimer(this);
    m_reconnectTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(1000);
    m_reconnectTimer->setInterval(2000);
    m_reconnectTimer->setSingleShot(true);

    connect(m_socket, &QTcpSocket::connected, this, &NetworkWorker::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &NetworkWorker::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &NetworkWorker::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &NetworkWorker::onSocketError);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &NetworkWorker::sendHeartbeat);
    connect(m_reconnectTimer, &QTimer::timeout, this, &NetworkWorker::reconnect);
    emit transportConfigured(ReadBufferLimitBytes);
}

void NetworkWorker::setReceiveBackpressure(bool active)
{
    ensureTransport();
    if (m_receiveBackpressureActive == active) {
        return;
    }

    m_receiveBackpressureActive = active;
    if (active) {
        disconnect(m_socket, &QTcpSocket::readyRead, this, &NetworkWorker::onReadyRead);
        emit warningMessage(QStringLiteral("network"),
                            QStringLiteral("Receive backpressure enabled; pending protocol bytes=%1").arg(m_pendingProtocolBytes));
    } else {
        connect(m_socket, &QTcpSocket::readyRead, this, &NetworkWorker::onReadyRead, Qt::UniqueConnection);
        emit logMessage(QStringLiteral("network"),
                        QStringLiteral("Receive backpressure cleared; pending protocol bytes=%1").arg(m_pendingProtocolBytes));
    }
    emit receiveBackpressureChanged(active, m_pendingProtocolBytes);
}

void NetworkWorker::scheduleReconnect()
{
    setState(DeviceState::Reconnecting);
    if (!m_reconnectTimer->isActive()) {
        emit warningMessage(QStringLiteral("device"), QStringLiteral("Reconnecting in 2 seconds"));
        m_reconnectTimer->start();
    }
}

void NetworkWorker::setState(DeviceState state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(m_state);
}

} // namespace MicArray
