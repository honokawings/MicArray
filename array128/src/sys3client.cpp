#include "sys3client.h"

#include <QMutexLocker>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <vector>

namespace MicArray::Array128 {

Sys3Client::Sys3Client() { m_clock.start(); }
Sys3Client::~Sys3Client() { close(); }
bool Sys3Client::running() const { return m_thread && m_thread->isRunning(); }
Sys3Session Sys3Client::snapshot() const { QMutexLocker lock(&m_mutex); return m_session; }
QJsonObject Sys3Client::json() const { QMutexLocker lock(&m_mutex); return m_session.json(now()); }
bool Sys3Client::send(char command) { QMutexLocker lock(&m_mutex); return m_session.request(command, now()); }

QStringList Sys3Client::availablePorts()
{
    QStringList ports;
#ifdef Q_OS_WIN
    std::vector<wchar_t> names(65536);
    const DWORD count = QueryDosDeviceW(nullptr, names.data(), DWORD(names.size()));
    if (count) for (const wchar_t *p = names.data(); *p; p += wcslen(p) + 1) {
        const auto name = QString::fromWCharArray(p);
        if (QRegularExpression("^COM[1-9][0-9]{0,3}$").match(name).hasMatch()) ports.append(name);
    }
#endif
    std::sort(ports.begin(), ports.end(), [](const QString &a, const QString &b) { return a.mid(3).toInt() < b.mid(3).toInt(); });
    return ports;
}

void Sys3Client::open(const QString &port)
{
    close();
    const QString name = port.trimmed().toUpper();
    {
        QMutexLocker lock(&m_mutex); m_session = Sys3Session{}; m_session.port = name;
        if (!QRegularExpression("^COM[1-9][0-9]{0,3}$").match(name).hasMatch()) {
            m_session.error = QStringLiteral("串口名称必须为 COM1–COM9999"); return;
        }
    }
    m_thread.reset(QThread::create([this, name] { run(name); }));
    m_thread->setObjectName("M128Sys3Serial"); m_thread->start();
}

void Sys3Client::close()
{
    if (m_thread) { m_thread->requestInterruption(); m_thread->wait(); m_thread.reset(); }
    QMutexLocker lock(&m_mutex); m_session.connected = false; m_session.identified = false;
    m_session.failCommand(QStringLiteral("串口已断开，执行结果未知"));
}

void Sys3Client::run(const QString &port)
{
    auto fail = [this](const QString &message) {
        QMutexLocker lock(&m_mutex); m_session.error = message; m_session.appendLog(message);
        m_session.connected = false; m_session.identified = false; m_session.failCommand(QStringLiteral("串口 I/O 错误"));
    };
#ifdef Q_OS_WIN
    const QString path = "\\\\.\\" + port;
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { fail(QStringLiteral("无法打开 %1（Windows %2）；检查端口、驱动或其他串口助手占用").arg(port).arg(GetLastError())); return; }
    DCB config {}; config.DCBlength = sizeof(config);
    bool configured = GetCommState(handle, &config);
    config.BaudRate = CBR_115200; config.ByteSize = 8; config.Parity = NOPARITY; config.StopBits = ONESTOPBIT;
    config.fBinary = TRUE; config.fParity = FALSE; config.fOutxCtsFlow = FALSE; config.fOutxDsrFlow = FALSE;
    config.fDtrControl = DTR_CONTROL_DISABLE; config.fRtsControl = RTS_CONTROL_DISABLE; config.fDsrSensitivity = FALSE;
    config.fOutX = FALSE; config.fInX = FALSE; config.fErrorChar = FALSE; config.fNull = FALSE; config.fAbortOnError = FALSE;
    COMMTIMEOUTS timeout {}; timeout.ReadIntervalTimeout = MAXDWORD; timeout.WriteTotalTimeoutConstant = 250;
    // MAXDWORD/0/0 reads return immediately, even with no data. Writes have a
    // 250 ms deadline; neither ReadFile nor WriteFile is called on the GUI.
    configured = configured && SetCommState(handle, &config) && SetCommTimeouts(handle, &timeout) && SetupComm(handle, 65536, 1024);
    if (!configured) { const DWORD error = GetLastError(); CloseHandle(handle); fail(QStringLiteral("串口 115200 8N1 配置失败（Windows %1）").arg(error)); return; }
    {
        QMutexLocker lock(&m_mutex); m_session.connected = true;
        m_session.appendLog(port + QStringLiteral(" 已打开；115200 8N1，无流控，DTR/RTS 关闭；只接收，等待 SYS3"));
    }
    while (!QThread::currentThread()->isInterruptionRequested()) {
        char bytes[4096]; DWORD received = 0;
        if (!ReadFile(handle, bytes, sizeof(bytes), &received, nullptr)) { fail(QStringLiteral("串口读取失败（Windows %1）").arg(GetLastError())); break; }
        DWORD errors = 0; COMSTAT status {};
        if (!ClearCommError(handle, &errors, &status)) { fail(QStringLiteral("串口状态读取失败（Windows %1）").arg(GetLastError())); break; }
        char command = 0;
        {
            QMutexLocker lock(&m_mutex);
            if (errors) {
                ++m_session.invalid; m_session.identified = false;
                m_session.failCommand(QStringLiteral("UART 驱动报告接收错误"));
                m_session.appendLog(QStringLiteral("UART 驱动错误掩码 0x%1").arg(errors, 0, 16));
            }
            if (received && !errors) m_session.feed(QByteArray(bytes, int(received)), now());
            m_session.tick(now());
            command = m_session.takeCommand(now());
        }
        if (command && !QThread::currentThread()->isInterruptionRequested()) {
            DWORD written = 0;
            if (!WriteFile(handle, &command, 1, &written, nullptr) || written != 1) {
                fail(QStringLiteral("串口单字节发送失败/超时（Windows %1），未重试").arg(GetLastError())); break;
            }
            QMutexLocker lock(&m_mutex);
            m_session.commandStatus = QStringLiteral("命令 %1 已发送 1 字节，等待识别回执").arg(QChar(command));
            m_session.appendLog(m_session.commandStatus);
        }
        QThread::msleep(10);
    }
    CloseHandle(handle);
    QMutexLocker lock(&m_mutex); m_session.connected = false; m_session.identified = false;
    m_session.failCommand(QStringLiteral("串口已关闭"));
#else
    Q_UNUSED(port);
    fail(QStringLiteral("本版本串口后端仅支持 Windows"));
#endif
}

} // namespace MicArray::Array128
