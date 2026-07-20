#include <QtTest/QtTest>

#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>

#include "device/deviceclient.h"
#include "device/networkworker.h"
#include "protocol/protocolcodec.h"

using namespace MicArray;

class DeviceClientTest : public QObject
{
    Q_OBJECT

private slots:
    void sendsHeartbeatAfterConnect();
    void emitsFramesFromServerData();
    void emitsUnknownStructuredFramesOnlyToGenericSignal();
    void exposesProtocolPipelineCounters();
    void networkWorkerConfiguresBoundedReadBuffer();
    void reportsNetworkReadBufferConfiguration();
    void networkWorkerReportsReceiveBackpressure();
    void rejectsControlWhenDisconnected();
    void reconnectBackoffUsesBoundedExponentialDelays();
    void networkWorkerReconnectsAfterDataTimeout();
    void genericDebugSignalIsSampledWhileMediaSignalsRemainComplete();
};

void DeviceClientTest::sendsHeartbeatAfterConnect()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    DeviceClient client;
    client.setAutoReconnect(false);
    client.connectToDevice(QStringLiteral("127.0.0.1"), server.serverPort());
    QVERIFY(server.waitForNewConnection(1000));
    QTcpSocket *socket = server.nextPendingConnection();
    QVERIFY(socket != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(client.state(), DeviceState::Connected, 1000);

    QTRY_VERIFY_WITH_TIMEOUT(socket->bytesAvailable() > 0 || socket->waitForReadyRead(20), 1500);
    QCOMPARE(socket->readAll().toHex(' '), QByteArray("7e 7e 00 cc 00 00 00 00 0a 0d"));
}

void DeviceClientTest::emitsFramesFromServerData()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    DeviceClient client;
    client.setAutoReconnect(false);
    QSignalSpy audioSpy(&client, &DeviceClient::audioFrameReceived);
    QSignalSpy videoSpy(&client, &DeviceClient::videoFrameReceived);

    client.connectToDevice(QStringLiteral("127.0.0.1"), server.serverPort());
    QVERIFY(server.waitForNewConnection(1000));
    QTcpSocket *socket = server.nextPendingConnection();
    QVERIFY(socket != nullptr);

    const QByteArray audio = QByteArray::fromHex("7e7e11ee010000000100000001000000010a0d");
    const QByteArray video = QByteArray::fromHex("7e7e22ee020000000200000002000000aabb0a");
    socket->write(audio + video);
    QVERIFY(socket->waitForBytesWritten(1000));

    QVERIFY(audioSpy.wait(1000));
    QVERIFY(videoSpy.count() == 1 || videoSpy.wait(1000));
    QCOMPARE(audioSpy.count(), 1);
    QCOMPARE(videoSpy.count(), 1);
    QCOMPARE(client.framesReceived(), 2u);
}

void DeviceClientTest::emitsUnknownStructuredFramesOnlyToGenericSignal()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    DeviceClient client;
    client.setAutoReconnect(false);
    QSignalSpy frameSpy(&client, &DeviceClient::frameReceived);
    QSignalSpy audioSpy(&client, &DeviceClient::audioFrameReceived);
    QSignalSpy videoSpy(&client, &DeviceClient::videoFrameReceived);

    client.connectToDevice(QStringLiteral("127.0.0.1"), server.serverPort());
    QVERIFY(server.waitForNewConnection(1000));
    QTcpSocket *socket = server.nextPendingConnection();
    QVERIFY(socket != nullptr);

    const QByteArray unknown = QByteArray::fromHex("7e7e3412030000000900000008000000aabbcc0a0d");
    const QByteArray followingHeader = QByteArray::fromHex("7e7e11ee010000000100000001000000");
    socket->write(unknown + followingHeader);
    QVERIFY(socket->waitForBytesWritten(1000));

    QVERIFY(frameSpy.wait(1000));
    QCOMPARE(frameSpy.count(), 1);
    QCOMPARE(audioSpy.count(), 0);
    QCOMPARE(videoSpy.count(), 0);
    QCOMPARE(client.framesReceived(), 1u);
    QCOMPARE(client.parseErrorCount(), 1u);
}

void DeviceClientTest::exposesProtocolPipelineCounters()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    DeviceClient client;
    client.setAutoReconnect(false);
    QSignalSpy audioSpy(&client, &DeviceClient::audioFrameReceived);

    client.connectToDevice(QStringLiteral("127.0.0.1"), server.serverPort());
    QVERIFY(server.waitForNewConnection(1000));
    QTcpSocket *socket = server.nextPendingConnection();
    QVERIFY(socket != nullptr);

    const QByteArray garbage = QByteArray::fromHex("0001");
    const QByteArray audio = QByteArray::fromHex("7e7e11ee010000000100000001000000010a0d");
    const QByteArray partial = QByteArray::fromHex("7e7e11ee010000000300000004000000");
    socket->write(garbage + audio + partial);
    QVERIFY(socket->waitForBytesWritten(1000));

    QVERIFY(audioSpy.wait(1000));
    QTRY_COMPARE_WITH_TIMEOUT(client.protocolBytesParsed(), quint64(audio.size()), 1000);
    QCOMPARE(client.protocolBytesDiscarded(), quint64(garbage.size()));
    QCOMPARE(client.protocolBufferedBytes(), qsizetype(partial.size()));
}

void DeviceClientTest::networkWorkerConfiguresBoundedReadBuffer()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    NetworkWorker worker;
    QSignalSpy configuredSpy(&worker, &NetworkWorker::transportConfigured);

    worker.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort(), false);

    QCOMPARE(configuredSpy.count(), 1);
    QCOMPARE(configuredSpy.takeFirst().at(0).toLongLong(), NetworkWorker::ReadBufferLimitBytes);
}

void DeviceClientTest::reportsNetworkReadBufferConfiguration()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    DeviceClient client;
    client.setAutoReconnect(false);
    QSignalSpy logSpy(&client, &DeviceClient::logMessage);

    client.connectToDevice(QStringLiteral("127.0.0.1"), server.serverPort());

    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(logSpy.cbegin(), logSpy.cend(), [](const QList<QVariant> &entry) {
        return entry.size() >= 2
            && entry.at(0).toString() == QStringLiteral("network")
            && entry.at(1).toString().contains(QStringLiteral("read buffer"));
    }), 1000);
}

void DeviceClientTest::networkWorkerReportsReceiveBackpressure()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    NetworkWorker worker;
    QSignalSpy backpressureSpy(&worker, &NetworkWorker::receiveBackpressureChanged);

    worker.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort(), false);
    QVERIFY(server.waitForNewConnection(1000));
    QTcpSocket *socket = server.nextPendingConnection();
    QVERIFY(socket != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(socket->state(), QAbstractSocket::ConnectedState, 1000);

    socket->write(QByteArray(NetworkWorker::PendingProtocolPauseBytes + 1024, char(0x55)));
    QVERIFY(socket->waitForBytesWritten(1000));

    QTRY_VERIFY_WITH_TIMEOUT(!backpressureSpy.isEmpty(), 1000);
    QCOMPARE(backpressureSpy.takeFirst().at(0).toBool(), true);

    worker.onProtocolBytesProcessed(NetworkWorker::PendingProtocolPauseBytes + 1024);
    QTRY_VERIFY_WITH_TIMEOUT(!backpressureSpy.isEmpty(), 1000);
    QCOMPARE(backpressureSpy.takeLast().at(0).toBool(), false);
}

void DeviceClientTest::rejectsControlWhenDisconnected()
{
    DeviceClient client;
    QSignalSpy warningSpy(&client, &DeviceClient::warningMessage);
    ControlValues values;

    QVERIFY(!client.sendControl(values));
    QCOMPARE(warningSpy.count(), 1);
}

void DeviceClientTest::reconnectBackoffUsesBoundedExponentialDelays()
{
    ReconnectBackoff backoff;
    QCOMPARE(backoff.nextDelayMs(0.0), 1000);
    QCOMPARE(backoff.nextDelayMs(0.0), 2000);
    QCOMPARE(backoff.nextDelayMs(0.0), 4000);
    QCOMPARE(backoff.nextDelayMs(0.0), 8000);
    QCOMPARE(backoff.nextDelayMs(0.0), 15000);
    QCOMPARE(backoff.nextDelayMs(0.0), 15000);
    backoff.reset();
    QCOMPARE(backoff.nextDelayMs(-1.0), 800);
    backoff.reset();
    QCOMPARE(backoff.nextDelayMs(1.0), 1200);
}

void DeviceClientTest::networkWorkerReconnectsAfterDataTimeout()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    NetworkWorker worker;
    QSignalSpy warningSpy(&worker, &NetworkWorker::warningMessage);
    worker.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort(), true);
    QVERIFY(server.waitForNewConnection(1000));

    QTRY_VERIFY_WITH_TIMEOUT([&warningSpy]() {
        for (const QList<QVariant> &arguments : warningSpy) {
            if (arguments.at(1).toString().contains(QStringLiteral("timeout"), Qt::CaseInsensitive)) {
                return true;
            }
        }
        return false;
    }(), 6500);
}

void DeviceClientTest::genericDebugSignalIsSampledWhileMediaSignalsRemainComplete()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    DeviceClient client;
    client.setAutoReconnect(false);
    QSignalSpy frameSpy(&client, &DeviceClient::frameReceived);
    QSignalSpy audioSpy(&client, &DeviceClient::audioFrameReceived);
    client.connectToDevice(QStringLiteral("127.0.0.1"), server.serverPort());
    QVERIFY(server.waitForNewConnection(1000));
    QTcpSocket *socket = server.nextPendingConnection();
    QVERIFY(socket != nullptr);

    const QByteArray audio = QByteArray::fromHex("7e7e11ee010000000100000001000000010a0d");
    socket->write(audio.repeated(20));
    QVERIFY(socket->waitForBytesWritten(1000));

    QTRY_COMPARE_WITH_TIMEOUT(audioSpy.count(), 20, 1000);
    QCOMPARE(frameSpy.count(), 1);
}

QTEST_MAIN(DeviceClientTest)
#include "test_deviceclient.moc"
