#pragma once

#include "sys3protocol.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QThread>
#include <memory>

namespace MicArray::Array128 {

// Low-bandwidth serial I/O stays entirely off the GUI and audio threads.
class Sys3Client final
{
public:
    Sys3Client();
    ~Sys3Client();
    static QStringList availablePorts();
    void open(const QString &port);
    void close();
    bool send(char command);
    bool running() const;
    Sys3Session snapshot() const;
    qint64 now() const { return m_clock.elapsed(); }
    QJsonObject json() const;
private:
    void run(const QString &port);
    mutable QMutex m_mutex;
    Sys3Session m_session;
    QElapsedTimer m_clock;
    std::unique_ptr<QThread> m_thread;
};

} // namespace MicArray::Array128
