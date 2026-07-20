#pragma once

#include <QDateTime>
#include <QObject>
#include <QThread>

#include "control/controlvalues.h"
#include "protocol/protocolcodec.h"

namespace MicArray {

class NetworkWorker;
class ProtocolWorker;

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
    ~DeviceClient() override;

    void connectToDevice(const QString &host, quint16 port);
    void disconnectFromDevice();
    bool sendControl(const ControlValues &values);

    void setAutoReconnect(bool enabled);
    bool autoReconnect() const { return m_autoReconnect; }
    DeviceState state() const { return m_state; }
    QString host() const { return m_host; }
    quint16 port() const { return m_port; }
    QDateTime lastReceiveTime() const { return m_lastReceiveTime; }
    quint64 bytesReceived() const { return m_bytesReceived; }
    quint64 framesReceived() const { return m_framesReceived; }
    quint64 parseErrorCount() const { return m_parseErrorCount; }
    quint64 protocolBytesParsed() const { return m_protocolBytesParsed; }
    quint64 protocolBytesDiscarded() const { return m_protocolBytesDiscarded; }
    qsizetype protocolBufferedBytes() const { return m_protocolBufferedBytes; }
    bool receiveBackpressureActive() const { return m_receiveBackpressureActive; }
    qint64 pendingProtocolBytes() const { return m_pendingProtocolBytes; }
    quint64 reconnectCount() const { return m_reconnectCount; }
    qint64 lastReceiveAgeMs() const;

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
    void reconnectCountChanged(quint64 reconnectCount);

private slots:
    void onBytesReceivedChanged(quint64 totalBytes, QDateTime lastReceiveTime);
    void onProtocolStatisticsChanged(quint64 frames, quint64 errors);
    void onProtocolPipelineMetricsChanged(quint64 bytesAppended, quint64 bytesParsed, quint64 bytesDiscarded, qint64 bufferedBytes);
    void onReceiveBackpressureChanged(bool active, qint64 pendingProtocolBytes);

private:
    void setState(DeviceState state);

    QThread m_networkThread;
    QThread m_protocolThread;
    NetworkWorker *m_networkWorker = nullptr;
    ProtocolWorker *m_protocolWorker = nullptr;
    DeviceState m_state = DeviceState::Disconnected;
    QString m_host = QStringLiteral("192.168.2.10");
    quint16 m_port = 9968;
    bool m_autoReconnect = true;
    QDateTime m_lastReceiveTime;
    quint64 m_bytesReceived = 0;
    quint64 m_framesReceived = 0;
    quint64 m_parseErrorCount = 0;
    quint64 m_protocolBytesParsed = 0;
    quint64 m_protocolBytesDiscarded = 0;
    qsizetype m_protocolBufferedBytes = 0;
    bool m_receiveBackpressureActive = false;
    qint64 m_pendingProtocolBytes = 0;
    quint64 m_reconnectCount = 0;
    QDateTime m_connectedSince;
};

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::DeviceState)
