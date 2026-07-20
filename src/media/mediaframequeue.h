#pragma once

#include "protocol/protocolcodec.h"

#include <QQueue>

namespace MicArray {

class MediaFrameQueue
{
public:
    explicit MediaFrameQueue(int capacity);

    bool push(const HostFrame &frame);
    HostFrame takeNext();
    void clear();

    int size() const { return m_frames.size(); }
    bool isEmpty() const { return m_frames.isEmpty(); }
    int capacity() const { return m_capacity; }
    quint64 droppedFrames() const { return m_droppedFrames; }

private:
    int m_capacity;
    QQueue<HostFrame> m_frames;
    quint64 m_droppedFrames = 0;
};

} // namespace MicArray
