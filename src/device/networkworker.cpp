#include "device/networkworker.h"

#include "device/deviceclient.h"
#include "protocol/protocolcodec.h"

#include <QMetaObject>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>

namespace MicArray {

int ReconnectBackoff::nextDelayMs(double jitterUnit)
{
    static constexpr int delays[] = {1000, 2000, 4000, 8000, 15000};
    const int base = delays[std::min(m_attempt, 4)];
    ++m_attempt;
    const double clampedJitter = std::clamp(jitterUnit, -1.0, 1.0);
    return static_cast<int>(std::lround(static_cast<double>(base) * (1.0 + 0.2 * clampedJitter)));
}

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
    if (m_healthTimer != nullptr) {
        m_healthTimer->stop();
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
    m_reconnectCount = 0;
    m_backoff.reset();
    emit reconnectCountChanged(m_reconnectCount);
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
    m_healthTimer->stop();
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
    m_lastReceiveTimer.restart();
    m_connectedTimer.restart();
    m_dataTimeoutReported = false;
    m_healthTimer->start();
}

void NetworkWorker::onDisconnected()
{
    if (m_heartbeatTimer != nullptr) {
        m_heartbeatTimer->stop();
    }
    if (m_healthTimer != nullptr) {
        m_healthTimer->stop();
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
    m_lastReceiveTimer.restart();
    m_dataTimeoutReported = false;
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
    if (m_manualDisconnect) {
        return;
    }
    ++m_reconnectCount;
    emit reconnectCountChanged(m_reconnectCount);
    m_socket->abort();
    setState(DeviceState::Reconnecting);
    emit logMessage(QStringLiteral("device"), QStringLiteral("Reconnect attempt %1 to %2:%3").arg(m_reconnectCount).arg(m_host).arg(m_port));
    m_socket->connectToHost(m_host, m_port);
}

void NetworkWorker::checkConnectionHealth()
{
    if (m_state != DeviceState::Connected) {
        return;
    }
    if (m_connectedTimer.isValid() && m_connectedTimer.elapsed() >= HealthyResetMs) {
        m_backoff.reset();
    }
    if (!m_dataTimeoutReported && m_lastReceiveTimer.isValid() && m_lastReceiveTimer.elapsed() >= DataTimeoutMs) {
        m_dataTimeoutReported = true;
        emit warningMessage(QStringLiteral("device"), QStringLiteral("Data receive timeout; rebuilding connection"));
        if (m_autoReconnect && !m_manualDisconnect) {
            m_socket->abort();
            scheduleReconnect();
        }
    }
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
    m_healthTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(1000);
    m_reconnectTimer->setSingleShot(true);
    m_healthTimer->setInterval(250);

    connect(m_socket, &QTcpSocket::connected, this, &NetworkWorker::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &NetworkWorker::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &NetworkWorker::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &NetworkWorker::onSocketError);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &NetworkWorker::sendHeartbeat);
    connect(m_reconnectTimer, &QTimer::timeout, this, &NetworkWorker::reconnect);
    connect(m_healthTimer, &QTimer::timeout, this, &NetworkWorker::checkConnectionHealth);
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
        const double jitter = QRandomGenerator::global()->generateDouble() * 2.0 - 1.0;
        const int delayMs = m_backoff.nextDelayMs(jitter);
        emit warningMessage(QStringLiteral("device"), QStringLiteral("Reconnecting in %1 ms").arg(delayMs));
        m_reconnectTimer->start(delayMs);
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
