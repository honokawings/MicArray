#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <array>
#include <optional>

namespace MicArray::Array128 {

enum class AudioUartAction { QueryI2s, QueryGain, SelectI2s, DisableI2s, SetGain };

struct AudioUartCommand
{
    AudioUartAction action = AudioUartAction::QueryI2s;
    int channel = 0;
    quint32 evenGain = 0x400000;
    quint32 oddGain = 0x400000;
    bool query() const { return action == AudioUartAction::QueryI2s || action == AudioUartAction::QueryGain; }
    bool gain() const { return action == AudioUartAction::QueryGain || action == AudioUartAction::SetGain; }
    QByteArray encode() const;
    QByteArray queryBytes() const;
    QString description() const;
};

struct AudioUartFrame
{
    bool gainResponse = false;
    std::array<quint32, 14> words {};
    QByteArray raw;
    bool fresh() const { return words[1] & 1; }
    bool pending() const { return words[1] & 2; }
    bool resetReleased() const { return words[1] & 4; }
    bool actualValid() const { return fresh() && resetReleased(); }
    quint8 errorCode() const { return quint8(words[1] >> 8); }
    quint16 queueOverflows() const { return quint16(words[1] >> 16); }
    qint32 lastPcm() const;
    QString errorDescription() const;
    QJsonObject json(bool current = true) const;
};

std::optional<AudioUartFrame> decodeAudioUartLine(const QByteArray &line);
QString audioGainText(quint32 value);

} // namespace MicArray::Array128
