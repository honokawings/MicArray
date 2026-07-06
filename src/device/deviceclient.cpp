#include "device/deviceclient.h"

namespace MicArray {

QString deviceStateName(DeviceState state)
{
    switch (state) {
    case DeviceState::Disconnected:
        return QStringLiteral("Disconnected");
    case DeviceState::Connecting:
        return QStringLiteral("Connecting");
    case DeviceState::Connected:
        return QStringLiteral("Connected");
    case DeviceState::Reconnecting:
        return QStringLiteral("Reconnecting");
    case DeviceState::Error:
        return QStringLiteral("Error");
    }
    return QStringLiteral("Disconnected");
}

DeviceClient::DeviceClient(QObject *parent)
    : QObject(parent)
{
    m_heartbeatTimer.setInterval(1000);
    m_reconnectTimer.setInterval(2000);
    m_reconnectTimer.setSingleShot(true);

    connect(&m_socket, &QTcpSocket::connected, this, &DeviceClient::onConnected);
    connect(&m_socket, &QTcpSocket::disconnected, this, &DeviceClient::onDisconnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &DeviceClient::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &DeviceClient::onSocketError);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &DeviceClient::sendHeartbeat);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &DeviceClient::reconnect);
}

void DeviceClient::connectToDevice(const QString &host, quint16 port)
{
    m_manualDisconnect = false;
    m_host = host;
    m_port = port;
    m_codec.clear();
    m_socket.abort();
    setState(DeviceState::Connecting);
    emit logMessage(QStringLiteral("device"), QStringLiteral("Connecting to %1:%2").arg(m_host).arg(m_port));
    m_socket.connectToHost(m_host, m_port);
}

void DeviceClient::disconnectFromDevice()
{
    m_manualDisconnect = true;
    m_reconnectTimer.stop();
    m_heartbeatTimer.stop();
    m_socket.disconnectFromHost();
    if (m_socket.state() == QAbstractSocket::UnconnectedState) {
        setState(DeviceState::Disconnected);
    }
}

bool DeviceClient::sendControl(const ControlValues &values)
{
    if (m_state != DeviceState::Connected || m_socket.state() != QAbstractSocket::ConnectedState) {
        emit warningMessage(QStringLiteral("device"), QStringLiteral("Cannot send control frame while device is disconnected"));
        return false;
    }

    const QByteArray frame = ProtocolCodec::encodeControl(values);
    const qint64 written = m_socket.write(frame);
    m_socket.flush();
    if (written != frame.size()) {
        emit errorMessage(QStringLiteral("device"), QStringLiteral("Control frame write failed"));
        return false;
    }

    emit logMessage(QStringLiteral("device"), QStringLiteral("Control frame sent"));
    return true;
}

void DeviceClient::onConnected()
{
    setState(DeviceState::Connected);
    emit logMessage(QStringLiteral("device"), QStringLiteral("Connected to %1:%2").arg(m_host).arg(m_port));
    sendHeartbeat();
    m_heartbeatTimer.start();
}

void DeviceClient::onDisconnected()
{
    m_heartbeatTimer.stop();
    emit warningMessage(QStringLiteral("device"), QStringLiteral("Disconnected from device"));
    if (m_manualDisconnect || !m_autoReconnect) {
        setState(DeviceState::Disconnected);
    } else {
        scheduleReconnect();
    }
}

void DeviceClient::onReadyRead()
{
    const QByteArray data = m_socket.readAll();
    m_bytesReceived += data.size();
    m_lastReceiveTime = QDateTime::currentDateTime();

    const ParseResult result = m_codec.parseIncoming(data);
    for (const ParseError &error : result.errors) {
        ++m_parseErrorCount;
        emit parseErrorReceived(error);
        emit warningMessage(QStringLiteral("protocol"), error.message);
    }

    for (const HostFrame &frame : result.frames) {
        ++m_framesReceived;
        emit frameReceived(frame);
        if (frame.rawType == static_cast<quint16>(FrameType::Audio)) {
            emit audioFrameReceived(frame);
        } else if (frame.rawType == static_cast<quint16>(FrameType::Video)) {
            emit videoFrameReceived(frame);
        }
    }

    emit statisticsChanged();
}

void DeviceClient::onSocketError(QAbstractSocket::SocketError)
{
    emit errorMessage(QStringLiteral("device"), m_socket.errorString());
    if (!m_manualDisconnect && m_autoReconnect) {
        scheduleReconnect();
    } else {
        setState(DeviceState::Error);
    }
}

void DeviceClient::sendHeartbeat()
{
    if (m_socket.state() != QAbstractSocket::ConnectedState) {
        return;
    }
    m_socket.write(ProtocolCodec::encodeHeartbeat());
    m_socket.flush();
}

void DeviceClient::reconnect()
{
    connectToDevice(m_host, m_port);
}

void DeviceClient::setState(DeviceState state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(m_state);
    emit statisticsChanged();
}

void DeviceClient::scheduleReconnect()
{
    setState(DeviceState::Reconnecting);
    if (!m_reconnectTimer.isActive()) {
        emit warningMessage(QStringLiteral("device"), QStringLiteral("Reconnecting in 2 seconds"));
        m_reconnectTimer.start();
    }
}

} // namespace MicArray
