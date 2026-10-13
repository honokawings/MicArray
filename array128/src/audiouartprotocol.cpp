#include "audiouartprotocol.h"

#include <QJsonArray>

namespace MicArray::Array128 {

QByteArray AudioUartCommand::encode() const
{
    switch (action) {
    case AudioUartAction::QueryI2s: return "@I2S?\n";
    case AudioUartAction::QueryGain: return "@GAIN?\n";
    case AudioUartAction::SelectI2s:
        return channel >= 0 && channel < 128 ? "@I2S=" + QByteArray::number(channel) + '\n' : QByteArray();
    case AudioUartAction::DisableI2s: return "@I2S=OFF\n";
    case AudioUartAction::SetGain:
        if (evenGain > 0x7fffff || oddGain > 0x7fffff) return {};
        return "@GAIN=" + QByteArray::number(evenGain, 16).rightJustified(6, '0').toUpper() + ','
            + QByteArray::number(oddGain, 16).rightJustified(6, '0').toUpper() + '\n';
    }
    return {};
}

QByteArray AudioUartCommand::queryBytes() const { return gain() ? QByteArray("@GAIN?\n") : QByteArray("@I2S?\n"); }
QString AudioUartCommand::description() const
{
    switch (action) {
    case AudioUartAction::QueryI2s: return QStringLiteral("查询板端监听");
    case AudioUartAction::QueryGain: return QStringLiteral("查询奇偶增益");
    case AudioUartAction::SelectI2s: return QStringLiteral("启用板端监听 CH%1").arg(channel);
    case AudioUartAction::DisableI2s: return QStringLiteral("关闭板端监听");
    case AudioUartAction::SetGain: return QStringLiteral("设置奇偶增益");
    }
    return {};
}

std::optional<AudioUartFrame> decodeAudioUartLine(const QByteArray &line)
{
    const bool gain = line.startsWith("GAIN1 ");
    if ((!gain && !line.startsWith("I2S1 ")) || line.size() != (gain ? 133 : 132) || !line.endsWith("\r\n")) return {};
    AudioUartFrame result; result.gainResponse = gain; result.raw = line;
    const int prefix = gain ? 6 : 5;
    for (int i = 0; i < 14; ++i) {
        const int offset = prefix + i * 9;
        quint32 value = 0;
        for (int j = 0; j < 8; ++j) {
            const char c = line[offset + j];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return {};
            value = (value << 4) | quint32(c <= '9' ? c - '0' : c - 'A' + 10);
        }
        if (i < 13 && line[offset + 8] != ' ') return {};
        result.words[i] = value;
    }
    return result;
}

qint32 AudioUartFrame::lastPcm() const
{
    const quint32 sample = words[11] & 0xffffff;
    return sample & 0x800000 ? qint32(sample) - 0x1000000 : qint32(sample);
}

QString AudioUartFrame::errorDescription() const
{
    switch (errorCode()) {
    case 0: return QStringLiteral("无命令错误");
    case 1: return QStringLiteral("命令语法错误");
    case 2: return QStringLiteral("参数越界");
    default: return QStringLiteral("未知错误码 %1").arg(errorCode());
    }
}

QString audioGainText(quint32 value)
{
    return QStringLiteral("%1 × / %2").arg(double(value & 0xffffff) / 4194304.0, 0, 'f', 7)
        .arg(value & 0xffffff, 6, 16, QChar('0')).toUpper();
}

QJsonObject AudioUartFrame::json(bool current) const
{
    QJsonArray rawWords;
    for (quint32 word : words) rawWords.append(QStringLiteral("%1").arg(word, 8, 16, QChar('0')).toUpper());
    const bool usable = current && actualValid();
    auto actual = [&](quint32 value) { return usable ? QJsonValue(double(value)) : QJsonValue(QJsonValue::Null); };
    return {{"type", gainResponse ? "GAIN1" : "I2S1"}, {"sequence", double(words[0])}, {"raw_words_hex", rawWords},
        {"fresh", fresh()}, {"pending", pending()}, {"audio_reset_released", resetReleased()}, {"current", current},
        {"error_code", errorCode()}, {"queue_overflows", queueOverflows()},
        {"requested_i2s", double(words[2] & 255)}, {"actual_i2s", actual(words[3] & 255)},
        {"requested_even_gain_q2_22", double(words[4] & 0xffffff)}, {"requested_odd_gain_q2_22", double(words[5] & 0xffffff)},
        {"actual_even_gain_q2_22", actual(words[6] & 0xffffff)}, {"actual_odd_gain_q2_22", actual(words[7] & 0xffffff)},
        {"received_samples", actual(words[8])}, {"output_frames", actual(words[9])}, {"underflow_frames", actual(words[10])},
        {"last_pcm_s24", usable ? QJsonValue(lastPcm()) : QJsonValue(QJsonValue::Null)},
        {"verified_frames", actual(words[12])}, {"serial_errors", actual(words[13])}};
}

} // namespace MicArray::Array128
