#pragma once

#include <array>

#include <QtGlobal>

namespace MicArray {

class ControlValues
{
public:
    static constexpr int ChannelCount = 8;
    static constexpr int ActiveChannelCount = 6;

    qint16 channelValue(int channel) const;
    void setChannelValue(int channel, qint16 value);
    void reset();
    bool isReserved(int channel) const;
    const std::array<qint16, ChannelCount> &values() const { return m_values; }

private:
    std::array<qint16, ChannelCount> m_values {};
};

} // namespace MicArray
