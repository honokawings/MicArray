#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QStringList>
#include <QVector>
#include <array>
#include <optional>

namespace MicArray::Array128 {

struct Sys3Frame
{
    std::array<quint32, 34> words {};
    QByteArray raw;
    bool clocksHealthy() const;
    bool audioFresh() const { return words[7] & 1; }
    bool ethFresh() const { return words[7] & 2; }
    bool rxFresh() const { return words[7] & 4; }
    bool selftest() const { return words[8] & 8; }
    quint16 commands() const { return quint16(words[5]); }
    char lastCommand() const { return char((words[5] >> 16) & 255); }
    QJsonObject json() const;
};

std::optional<Sys3Frame> decodeSys3Line(const QByteArray &line);
QStringList sys3WordNames();
// Domain: 0 observer, 1 audio, 2 Ethernet (including PHY/MAC), 3 RX.
int sys3WordDomain(int word);
QVector<QStringList> sys3Details(const Sys3Frame &frame);

// Pure protocol/session state. All timestamps are caller-supplied monotonic ms,
// allowing fragmentation, timeouts and command acknowledgements to be tested.
struct Sys3Session
{
    bool connected = false, identified = false;
    QString port, error;
    Sys3Frame latest;
    quint64 valid = 0, invalid = 0, bytes = 0, missing = 0, duplicates = 0, restarts = 0;
    qint64 lastFrameMs = -1;
    QStringList log;
    char pending = 0;
    bool queued = false, acknowledged = false;
    qint64 commandMs = -1, lastToggleMs = -1000;
    quint16 baseCommands = 0;
    quint32 ackSequence = 0, baseEpoch = 0;
    bool baseMode = false;
    QString commandStatus = QStringLiteral("尚未发送命令");

    bool current(qint64 now) const;
    QString commandBlock(char command, qint64 now) const;
    bool request(char command, qint64 now);
    char takeCommand(qint64 now);
    void feed(const QByteArray &data, qint64 now);
    void tick(qint64 now);
    void failCommand(const QString &reason);
    void appendLog(const QString &line);
    QJsonObject json(qint64 now) const;
private:
    void accept(const Sys3Frame &frame, qint64 now);
    QByteArray m_buffer;
    bool m_discarding = false;
};

} // namespace MicArray::Array128
