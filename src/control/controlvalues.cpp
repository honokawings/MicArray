#include "control/controlvalues.h"

#include <QtGlobal>

namespace MicArray {

qint16 ControlValues::channelValue(int channel) const
{
    Q_ASSERT(channel >= 0 && channel < ChannelCount);
    return m_values[static_cast<size_t>(channel)];
}

void ControlValues::setChannelValue(int channel, qint16 value)
{
    Q_ASSERT(channel >= 0 && channel < ChannelCount);
    m_values[static_cast<size_t>(channel)] = value;
}

void ControlValues::reset()
{
    m_values.fill(0);
}

} // namespace MicArray
