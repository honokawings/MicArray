#pragma once

#include "control/controlvalues.h"
#include "media/noisereductiontypes.h"
#include "media/pldenoisetransport.h"

#include <QObject>
#include <QTimer>

#include <array>

namespace MicArray {

class PlDenoiseSession : public QObject
{
    Q_OBJECT
public:
    explicit PlDenoiseSession(QObject *parent = nullptr);
    static quint16 crc16(const QByteArray &bytes);
    static std::array<qint32, 6> directionSlopes(const NoiseReductionSettings &settings);
    static std::array<qint32, 9> configurationTuple(const NoiseReductionSettings &settings);
    static ControlValues command(quint16 transaction, quint16 field, qint32 value);
    void setAvailable(bool connected, bool enabled);
    void configure(const NoiseReductionSettings &settings, int sampleRate, bool force = false);
    void acknowledge(const PlDenoiseStatus &status);
    void retryNow();
    bool ready() const { return m_ready; }
    bool busy() const { return m_commandIndex >= 0; }
    quint16 activeConfigId() const { return m_activeConfig; }

signals:
    void commandReady(const ControlValues &command);
    void configurationApplied(quint16 configId, bool restartStream);
    void statusChanged(const QString &message);
    void failed(const QString &message);

private:
    void begin();
    void sendCurrent();
    quint16 currentField() const;
    void fail(const QString &message);
    std::array<qint32, 9> m_desired {};
    std::array<qint32, 9> m_sending {};
    std::array<qint32, 9> m_applied {};
    NoiseReductionSettings m_previousSettings;
    bool m_havePreviousSettings = false;
    int m_desiredStage = 1;
    int m_sendingStage = 1;
    int m_appliedStage = 1;
    int m_sendingFieldCount = 6;
    quint16 m_activeStream = 0;
    bool m_haveDirections = false;
    bool m_connected = false;
    bool m_enabled = false;
    bool m_ready = false;
    bool m_needReset = true;
    bool m_resetFirst = true;
    quint16 m_transaction = 0;
    quint16 m_activeConfig = 0;
    int m_commandIndex = -1;
    int m_retries = 0;
    QTimer m_timeout;
};

} // namespace MicArray
