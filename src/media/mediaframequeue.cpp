#include "media/mediaframequeue.h"

#include <algorithm>

namespace MicArray {

MediaFrameQueue::MediaFrameQueue(int capacity)
    : m_capacity(std::max(1, capacity))
{
}

bool MediaFrameQueue::push(const HostFrame &frame)
{
    bool dropped = false;
    while (m_frames.size() >= m_capacity) {
        m_frames.dequeue();
        ++m_droppedFrames;
        dropped = true;
    }
    m_frames.enqueue(frame);
    return dropped;
}

HostFrame MediaFrameQueue::takeNext()
{
    return m_frames.isEmpty() ? HostFrame {} : m_frames.dequeue();
}

void MediaFrameQueue::clear()
{
    m_frames.clear();
}

} // namespace MicArray
