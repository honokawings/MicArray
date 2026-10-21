#include "control/pldenoisesession.h"

#include "analysis/arraygeometry.h"

#include <QRandomGenerator>
#include <QtEndian>

#include <cmath>
#include <algorithm>

namespace MicArray {

PlDenoiseSession::PlDenoiseSession(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<ControlValues>();
    m_transaction = static_cast<quint16>(QRandomGenerator::global()->bounded(1, 65535));
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(150);
    connect(&m_timeout, &QTimer::timeout, this, &PlDenoiseSession::retryNow);
}

quint16 PlDenoiseSession::crc16(const QByteArray &bytes)
{
    quint16 crc = 0xffff;
    for (char byte : bytes)
    {
        crc ^= quint16(static_cast<quint8>(byte)) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<quint16>((crc & 0x8000U) ? (crc << 1) ^ 0x1021U : crc << 1);
    }
    return crc;
}

std::array<qint32, 6> PlDenoiseSession::directionSlopes(const NoiseReductionSettings &settings)
{
    std::array<qint32, 6> result {};
    const std::array<ContinuousDirectionTarget, 3> directions {
        settings.enhancedA, settings.suppressionB1, settings.suppressionB2};
    constexpr double factor = 0.025 * 48000.0 / 343.0 * 67108864.0;
    for (int index = 0; index < 3; ++index)
    {
        const QVector3D direction = directionFromAngles(directions[index].azimuthDegrees,
                                                       directions[index].elevationDegrees);
        result[index * 2] = static_cast<qint32>(std::llround(factor * double(direction.x())));
        result[index * 2 + 1] = static_cast<qint32>(std::llround(factor * double(direction.y())));
    }
    return result;
}

std::array<qint32, 9> PlDenoiseSession::configurationTuple(const NoiseReductionSettings &settings)
{
    std::array<qint32, 9> tuple {};
    const auto slopes = directionSlopes(settings);
    std::copy(slopes.begin(), slopes.end(), tuple.begin());
    if (isPlMonoNoiseInputMode(settings.inputMode)) {
        tuple[6] = settings.inputMode == NoiseReductionInputMode::PlFullDenoisingFixedProfile ? 2 : 1;
        const bool two = settings.mode == NoiseReductionMode::TwoPoint;
        tuple[7] = qint32(std::llround((two ? settings.dualB1SuppressionCoefficient
            : settings.singleSuppressionCoefficient) * 65536.0));
        tuple[8] = two ? qint32(std::llround(settings.dualB2SuppressionCoefficient * 65536.0)) : 0;
    }
    return tuple;
}

ControlValues PlDenoiseSession::command(quint16 transaction, quint16 field, qint32 value)
{
    ControlValues result;
    result.setChannelValue(0, 0x4e44);
    result.setChannelValue(1, static_cast<qint16>(transaction));
    result.setChannelValue(2, static_cast<qint16>(field));
    result.setChannelValue(3, static_cast<qint16>(quint32(value) & 0xffffU));
    result.setChannelValue(4, static_cast<qint16>(quint32(value) >> 16));
    QByteArray bytes(10, Qt::Uninitialized);
    for (int index = 0; index < 5; ++index)
        qToLittleEndian(result.channelValue(index), bytes.data() + index * 2);
    result.setChannelValue(5, static_cast<qint16>(crc16(bytes)));
    return result;
}

void PlDenoiseSession::setAvailable(bool connected, bool enabled)
{
    const bool wasAvailable = m_connected && m_enabled;
    m_connected = connected;
    m_enabled = enabled;
    if (!connected || !enabled)
    {
        m_timeout.stop();
        m_commandIndex = -1;
        m_ready = false;
        m_needReset = true;
        return;
    }
    if (!wasAvailable && m_haveDirections) begin();
}

void PlDenoiseSession::configure(const NoiseReductionSettings &settings, int sampleRate, bool force)
{
    const int stage = settings.inputMode == NoiseReductionInputMode::PlFullDenoisingFixedProfile ? 3
        : (settings.inputMode == NoiseReductionInputMode::PlFixedWienerOriginalPostProcessing ? 2 : 1);
    if (stage == 3 && !plFullDenoiseProfileError(settings).isEmpty()) {
        m_haveDirections = false;
        fail(plFullDenoiseProfileError(settings));
        return;
    }
    if (stage > 1 && settings.algorithm != NoiseReductionAlgorithm::FixedParameterWiener) {
        m_haveDirections = false;
        fail(QStringLiteral("此 PL 模式只支持固定参数维纳；自适应维纳请明确选择三波束模式或完整软件模式"));
        return;
    }
    if (sampleRate != 48000)
    {
        m_haveDirections = false;
        fail(QStringLiteral("PL 三路波束仅支持 48 kHz；未启动处理"));
        return;
    }
    for (const auto &target : {settings.enhancedA, settings.suppressionB1, settings.suppressionB2})
        if (!std::isfinite(target.azimuthDegrees) || !std::isfinite(target.elevationDegrees)
            || std::abs(target.azimuthDegrees) > 90 || std::abs(target.elevationDegrees) > 90)
        {
            m_haveDirections = false;
            fail(QStringLiteral("PL 波束方向参数无效"));
            return;
        }
    for (double coefficient : {settings.singleSuppressionCoefficient, settings.dualB1SuppressionCoefficient,
                               settings.dualB2SuppressionCoefficient}) {
        if (!std::isfinite(coefficient) || coefficient < 0 || coefficient > 100) {
            m_haveDirections = false;
            fail(QStringLiteral("PL 维纳抑制系数必须在 0..100 范围内"));
            return;
        }
    }
    const auto tuple = configurationTuple(settings);
    const bool changed = !m_haveDirections || tuple != m_desired;
    const auto &previous = m_previousSettings;
    const bool originalDspReset = m_havePreviousSettings && (stage > 1 || m_desiredStage > 1)
        && (settings.inputMode != previous.inputMode || settings.enabled != previous.enabled
            || settings.algorithm != previous.algorithm || settings.mode != previous.mode
            || settings.singleSuppressionCoefficient != previous.singleSuppressionCoefficient
            || settings.dualB1SuppressionCoefficient != previous.dualB1SuppressionCoefficient
            || settings.dualB2SuppressionCoefficient != previous.dualB2SuppressionCoefficient
            || settings.bandPass != previous.bandPass || settings.logMmse != previous.logMmse
            || settings.automaticGainControl != previous.automaticGainControl
            || settings.postProcessingOrder != previous.postProcessingOrder);
    force = force || originalDspReset;
    m_previousSettings = settings;
    m_havePreviousSettings = true;
    m_desiredStage = stage;
    m_desired = tuple;
    m_haveDirections = true;
    if (force)
    {
        m_timeout.stop();
        m_commandIndex = -1;
        m_ready = false;
        m_needReset = true;
    }
    if (m_connected && m_enabled && !busy() && (changed || force || !m_ready)) begin();
}

void PlDenoiseSession::begin()
{
    if (!m_connected || !m_enabled || !m_haveDirections) return;
    ++m_transaction;
    if (m_transaction == 0) ++m_transaction;
    m_sending = m_desired;
    m_sendingStage = m_desiredStage;
    m_sendingFieldCount = m_sendingStage > 1 ? 9 : 6;
    m_resetFirst = m_needReset;
    m_commandIndex = 0;
    m_retries = 0;
    emit statusChanged(QStringLiteral("PL 方向配置 %1：等待 %2 确认")
        .arg(m_transaction).arg(m_resetFirst ? QStringLiteral("RESET") : QStringLiteral("BEGIN")));
    sendCurrent();
}

quint16 PlDenoiseSession::currentField() const
{
    const int index = m_commandIndex - (m_resetFirst ? 1 : 0);
    return index < 0 ? 0x102 : (index == 0 ? 0x100
        : (index == m_sendingFieldCount + 1 ? 0x101 : static_cast<quint16>(index - 1)));
}

void PlDenoiseSession::sendCurrent()
{
    if (!busy()) return;
    const quint16 field = currentField();
    const qint32 value = field < m_sendingFieldCount ? m_sending[field] : 0;
    m_timeout.start();
    emit commandReady(command(m_transaction, field, value));
}

void PlDenoiseSession::acknowledge(const PlDenoiseStatus &status)
{
    if (!m_connected || !m_enabled) return;
    if (busy() && status.errorCode == 1 && status.overflowCount == 0) {
        emit statusChanged(QStringLiteral("配置写入暂态 CRC 错误，等待稳定 ACK / 定时重发（事务 %1）").arg(m_transaction));
        return;
    }
    if (status.errorCode != 0 || status.overflowCount != 0 || (status.flags & 4U) != 0)
    {
        if (!m_ready && busy() && status.ackTransaction != m_transaction) return;
        fail(QStringLiteral("DNS1 报告 PL 错误 %1 / 溢出 %2")
            .arg(status.errorCode).arg(status.overflowCount));
        return;
    }
    if (!busy())
    {
        if (m_ready && (!status.ready() || status.activeConfigId != m_activeConfig
            || status.stage != m_appliedStage
            || (m_appliedStage > 1 && status.streamId != m_activeStream)
            || (m_appliedStage == 3 && status.profileFingerprint != PlDenoiseTransport::FullProfileFingerprint)))
            fail(QStringLiteral("PL 就绪状态或活动配置改变，请重新同步"));
        return;
    }
    const quint16 field = currentField();
    if (status.ackTransaction != m_transaction || status.ackField != field) return;
    if (field == 0x101 && (!status.ready() || status.activeConfigId != m_transaction
        || status.stage != m_sendingStage
        || (m_sendingStage > 1 && (status.streamId == 0 || (!m_resetFirst && status.streamId != m_activeStream)))
        || (m_sendingStage == 3 && status.profileFingerprint != PlDenoiseTransport::FullProfileFingerprint))) return;
    m_timeout.stop();
    m_retries = 0;
    if (++m_commandIndex < m_sendingFieldCount + 2 + (m_resetFirst ? 1 : 0))
    {
        sendCurrent();
        return;
    }
    m_commandIndex = -1;
    const bool restart = !m_ready;
    m_ready = true;
    m_needReset = false;
    m_activeConfig = m_transaction;
    m_activeStream = status.streamId;
    m_appliedStage = m_sendingStage;
    m_applied = m_sending;
    emit configurationApplied(m_activeConfig, restart);
    emit statusChanged(m_appliedStage == 3
        ? QStringLiteral("PL 全链通路已就绪，配置 %1 / 流 %2 / 指纹 b2c8f065；上位机仅显示、录制、监听").arg(m_activeConfig).arg(m_activeStream)
        : (m_appliedStage == 2
            ? QStringLiteral("PL 固定维纳已就绪，配置 %1 / 流 %2；上位机仅执行原后处理").arg(m_activeConfig).arg(m_activeStream)
            : QStringLiteral("PL 三路波束已就绪，配置 %1；维纳/后处理由上位机执行").arg(m_activeConfig)));
    if (m_desired != m_applied) begin();
}

void PlDenoiseSession::retryNow()
{
    if (!busy()) return;
    if (++m_retries > 8)
    {
        fail(QStringLiteral("PL 配置 ACK 超时，未回退软件波束形成"));
        return;
    }
    emit statusChanged(QStringLiteral("PL 配置 %1 等待 ACK，重发 %2/8")
        .arg(m_transaction).arg(m_retries));
    sendCurrent();
}

void PlDenoiseSession::fail(const QString &message)
{
    m_timeout.stop();
    m_commandIndex = -1;
    m_ready = false;
    m_needReset = true;
    emit failed(message);
}

} // namespace MicArray
