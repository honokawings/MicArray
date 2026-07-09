#include "device/deviceclient.h"

#include "device/networkworker.h"
#include "protocol/protocolworker.h"

#include <QMetaObject>

#include <algorithm>

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
    qRegisterMetaType<DeviceState>("MicArray::DeviceState");
    qRegisterMetaType<HostFrame>("MicArray::HostFrame");
    qRegisterMetaType<ParseError>("MicArray::ParseError");

    m_networkWorker = new NetworkWorker;
    m_protocolWorker = new ProtocolWorker;
    m_networkWorker->moveToThread(&m_networkThread);
    m_protocolWorker->moveToThread(&m_protocolThread);

    connect(&m_networkThread, &QThread::finished, m_networkWorker, &QObject::deleteLater);
    connect(&m_protocolThread, &QThread::finished, m_protocolWorker, &QObject::deleteLater);

    connect(m_networkWorker, &NetworkWorker::stateChanged, this, &DeviceClient::setState, Qt::QueuedConnection);
    connect(m_networkWorker, &NetworkWorker::bytesReceivedChanged, this, &DeviceClient::onBytesReceivedChanged, Qt::QueuedConnection);
    connect(m_networkWorker, &NetworkWorker::bytesRead, m_protocolWorker, &ProtocolWorker::ingestBytes, Qt::QueuedConnection);
    connect(m_networkWorker, &NetworkWorker::logMessage, this, &DeviceClient::logMessage, Qt::QueuedConnection);
    connect(m_networkWorker, &NetworkWorker::warningMessage, this, &DeviceClient::warningMessage, Qt::QueuedConnection);
    connect(m_networkWorker, &NetworkWorker::errorMessage, this, &DeviceClient::errorMessage, Qt::QueuedConnection);
    connect(m_networkWorker, &NetworkWorker::transportConfigured, this, [this](qint64 readBufferLimitBytes) {
        emit logMessage(QStringLiteral("network"),
                        QStringLiteral("TCP read buffer limit configured: %1 B").arg(readBufferLimitBytes));
    }, Qt::QueuedConnection);
    connect(m_networkWorker, &NetworkWorker::receiveBackpressureChanged, this, &DeviceClient::onReceiveBackpressureChanged, Qt::QueuedConnection);

    connect(m_protocolWorker, &ProtocolWorker::frameParsed, this, &DeviceClient::frameReceived, Qt::QueuedConnection);
    connect(m_protocolWorker, &ProtocolWorker::audioFrameParsed, this, &DeviceClient::audioFrameReceived, Qt::QueuedConnection);
    connect(m_protocolWorker, &ProtocolWorker::videoFrameParsed, this, &DeviceClient::videoFrameReceived, Qt::QueuedConnection);
    connect(m_protocolWorker, &ProtocolWorker::parseError, this, &DeviceClient::parseErrorReceived, Qt::QueuedConnection);
    connect(m_protocolWorker, &ProtocolWorker::warningMessage, this, &DeviceClient::warningMessage, Qt::QueuedConnection);
    connect(m_protocolWorker, &ProtocolWorker::statisticsChanged, this, &DeviceClient::onProtocolStatisticsChanged, Qt::QueuedConnection);
    connect(m_protocolWorker, &ProtocolWorker::pipelineMetricsChanged, this, &DeviceClient::onProtocolPipelineMetricsChanged, Qt::QueuedConnection);
    connect(m_protocolWorker, &ProtocolWorker::bytesProcessed, m_networkWorker, &NetworkWorker::onProtocolBytesProcessed, Qt::QueuedConnection);

    m_networkThread.setObjectName(QStringLiteral("MicArrayNetworkThread"));
    m_protocolThread.setObjectName(QStringLiteral("MicArrayProtocolThread"));
    m_protocolThread.start();
    m_networkThread.start();
}

DeviceClient::~DeviceClient()
{
    disconnectFromDevice();

    m_networkThread.quit();
    m_protocolThread.quit();
    m_networkThread.wait(2000);
    m_protocolThread.wait(2000);
}

void DeviceClient::connectToDevice(const QString &host, quint16 port)
{
    m_host = host;
    m_port = port;
    m_bytesReceived = 0;
    m_framesReceived = 0;
    m_parseErrorCount = 0;
    m_protocolBytesParsed = 0;
    m_protocolBytesDiscarded = 0;
    m_protocolBufferedBytes = 0;
    m_receiveBackpressureActive = false;
    m_pendingProtocolBytes = 0;
    if (m_protocolWorker != nullptr) {
        QMetaObject::invokeMethod(m_protocolWorker, "clear", Qt::QueuedConnection);
    }
    setState(DeviceState::Connecting);
    QMetaObject::invokeMethod(m_networkWorker,
                              "connectToHost",
                              Qt::QueuedConnection,
                              Q_ARG(QString, m_host),
                              Q_ARG(quint16, m_port),
                              Q_ARG(bool, m_autoReconnect));
}

void DeviceClient::disconnectFromDevice()
{
    if (m_networkWorker != nullptr) {
        const Qt::ConnectionType connectionType =
            m_networkThread.isRunning() && QThread::currentThread() != m_networkWorker->thread()
            ? Qt::BlockingQueuedConnection
            : Qt::DirectConnection;
        QMetaObject::invokeMethod(m_networkWorker, "disconnectFromHost", connectionType);
    }
}

bool DeviceClient::sendControl(const ControlValues &values)
{
    if (m_state != DeviceState::Connected) {
        emit warningMessage(QStringLiteral("device"), QStringLiteral("Cannot send control frame while device is disconnected"));
        return false;
    }

    const QByteArray frame = ProtocolCodec::encodeControl(values);
    QMetaObject::invokeMethod(m_networkWorker,
                              "sendFrame",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, frame),
                              Q_ARG(QString, QStringLiteral("Control frame")));
    return true;
}

void DeviceClient::setAutoReconnect(bool enabled)
{
    m_autoReconnect = enabled;
}

void DeviceClient::onBytesReceivedChanged(quint64 totalBytes, QDateTime lastReceiveTime)
{
    m_bytesReceived = totalBytes;
    m_lastReceiveTime = lastReceiveTime;
    emit statisticsChanged();
}

void DeviceClient::onProtocolStatisticsChanged(quint64 frames, quint64 errors)
{
    m_framesReceived = frames;
    m_parseErrorCount = errors;
    emit statisticsChanged();
}

void DeviceClient::onProtocolPipelineMetricsChanged(quint64, quint64 bytesParsed, quint64 bytesDiscarded, qint64 bufferedBytes)
{
    m_protocolBytesParsed = bytesParsed;
    m_protocolBytesDiscarded = bytesDiscarded;
    m_protocolBufferedBytes = static_cast<qsizetype>(std::max<qint64>(0, bufferedBytes));
    emit statisticsChanged();
}

void DeviceClient::onReceiveBackpressureChanged(bool active, qint64 pendingProtocolBytes)
{
    m_receiveBackpressureActive = active;
    m_pendingProtocolBytes = pendingProtocolBytes;
    emit statisticsChanged();
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

} // namespace MicArray
