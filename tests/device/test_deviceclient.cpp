#include <QtTest/QtTest>

#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>

#include "device/deviceclient.h"
#include "protocol/protocolcodec.h"

using namespace MicArray;

class DeviceClientTest : public QObject
{
    Q_OBJECT

private slots:
    void sendsHeartbeatAfterConnect();
    void emitsFramesFromServerData();
    void emitsUnknownStructuredFramesOnlyToGenericSignal();
    void rejectsControlWhenDisconnected();
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
    socket->write(unknown);
    QVERIFY(socket->waitForBytesWritten(1000));

    QVERIFY(frameSpy.wait(1000));
    QCOMPARE(frameSpy.count(), 1);
    QCOMPARE(audioSpy.count(), 0);
    QCOMPARE(videoSpy.count(), 0);
    QCOMPARE(client.framesReceived(), 1u);
    QCOMPARE(client.parseErrorCount(), 1u);
}

void DeviceClientTest::rejectsControlWhenDisconnected()
{
    DeviceClient client;
    QSignalSpy warningSpy(&client, &DeviceClient::warningMessage);
    ControlValues values;

    QVERIFY(!client.sendControl(values));
    QCOMPARE(warningSpy.count(), 1);
}

QTEST_MAIN(DeviceClientTest)
#include "test_deviceclient.moc"
