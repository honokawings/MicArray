#pragma once
#include <QTimer>
#include <QtTest/QtTest>
#include <QApplication>
#include <QLabel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

#include "app/appsettings.h"
#include "control/pldenoisesession.h"
#include "media/noisereductionworker.h"
#include <QImage>
#include <QThread>
#include <QWidget>

// Test-only visibility to observe the actual worker without changing any host file.
// All dependencies are already included, so this applies only to the page class.
#define private public
#include "ui/noisereductionpage.h"
#undef private

#include <atomic>
#include <bit>

using namespace MicArray;

namespace
{
QByteArray checked(QByteArray packet)
{
    packet[40] = char(PlDenoiseTransport::crc8(packet.first(40)));
    return packet;
}

QByteArray rawFrame(const QVector<QByteArray> &packets)
{
    Q_ASSERT(packets.size() <= 200);
    QByteArray raw(38400, 0);
    for (int sample = 0; sample < packets.size(); ++sample)
    {
        for (int byte = 0; byte < 41; ++byte)
        {
            raw[sample * 192 + (byte + 7) * 4 + 3] = packets[sample][byte];
        }
    }
    return raw;
}

QByteArray statusPacket(int stage, quint16 active, quint16 stream,
                      quint16 transaction, quint16 field)
{
    QByteArray packet(41, 0);
    packet.replace(0, 4, "DNS1");
    packet[4] = 1;
    packet[5] = char(active ? stage : 1);
    qToLittleEndian(active, packet.data() + 6);
    qToLittleEndian(transaction, packet.data() + 8);
    qToLittleEndian(field, packet.data() + 10);
    packet[12] = active ? 1 : 0;
    qToLittleEndian(quint32(stage == 1 ? 0x20261020 : stage == 2 ? 0x20261021 : 0x20261022),
                    packet.data() + 25);
    if (stage > 1)
    {
        qToLittleEndian(stream, packet.data() + 31);
    }
    if (stage == 3)
    {
        qToLittleEndian(active ? PlDenoiseTransport::FullProfileFingerprint : 0U,
                        packet.data() + 33);
    }
    return checked(packet);
}

QByteArray audio200(int stage, quint32 first, quint16 stream)
{
    QVector<QByteArray> packets;
    const int capacity = stage == 1 ? 2 : 6;
    for (int offset = 0; offset < 200; offset += capacity)
    {
        const int count = std::min(capacity, 200 - offset);
        QByteArray packet(41, 0);
        packet.replace(0, 4, stage == 1 ? "AUD3" : stage == 2 ? "AUW1" : "AUF1");
        packet[4] = 1;
        packet[5] = char(stage == 1 ? 0x11 : stage == 2 ? 0x21 : 0x31);
        qToLittleEndian(stream, packet.data() + 6);
        qToLittleEndian(first + quint32(offset), packet.data() + 8);
        packet[12] = char(count);
        for (int sample = 0; sample < count; ++sample)
        {
            const int beams = stage == 1 ? 3 : 1;
            for (int beam = 0; beam < beams; ++beam)
            {
                const float value = beam == 0 ? float(1 + (first + quint32(offset + sample)) % 4093) / 65536.0f : 0.0f;
                qToLittleEndian(std::bit_cast<quint32>(value),
                                packet.data() + 13 + (sample * beams + beam) * 4);
            }
        }
        packets.append(checked(packet));
    }
    return rawFrame(packets);
}

NoiseReductionSettings settingsFor(int stage)
{
    NoiseReductionSettings value;
    value.enabled = true;
    value.monitorEnabled = false;
    value.inputMode = stage == 1 ? NoiseReductionInputMode::PlThreeBeamOriginalDenoising
        : stage == 2 ? NoiseReductionInputMode::PlFixedWienerOriginalPostProcessing
                     : NoiseReductionInputMode::PlFullDenoisingFixedProfile;
    value.mode = NoiseReductionMode::TwoPoint;
    value.singleSuppressionCoefficient = 8.5;
    value.dualB1SuppressionCoefficient = 5;
    value.dualB2SuppressionCoefficient = 5;
    if (stage == 3)
    {
        value.bandPass.enabled = true;
        value.bandPass.lowCutoffHz = 100;
        value.bandPass.highCutoffHz = 8000;
        value.logMmse.enabled = true;
        value.logMmse.fftSize = 2048;
        value.logMmse.overlapRatio = .5;
        value.logMmse.priorSnrSmoothing = .98;
        value.logMmse.noisePowerSmoothing = .95;
        value.logMmse.minimumGainDb = -20;
        value.automaticGainControl.enabled = true;
        value.automaticGainControl.targetLevelDbfs = -24;
        value.automaticGainControl.maximumGainDb = 20;
        value.automaticGainControl.maximumAttenuationDb = -20;
        value.automaticGainControl.attackMilliseconds = 20;
        value.automaticGainControl.releaseMilliseconds = 300;
        value.automaticGainControl.limiterLevelDbfs = -1;
    }
    return value;
}

bool drainWorker(NoiseReductionPage &page)
{
    return QMetaObject::invokeMethod(page.m_worker, [] {}, Qt::BlockingQueuedConnection);
}

void deliverUiReceipts(NoiseReductionPage &page)
{
    QCoreApplication::sendPostedEvents(&page, QEvent::MetaCall);
}

struct Handshake
{
    quint16 active = 0;
    quint16 stream = 0;
    int commands = 0;
};

void connectHandshake(NoiseReductionPage &page, int stage, Handshake &state,
                      int beforeCommitFrames = 0)
{
    QObject::connect(&page, &NoiseReductionPage::controlSendRequested, &page,
                     [&page, stage, &state, beforeCommitFrames](const ControlValues &values)
    {
        ++state.commands;
        const quint16 transaction = quint16(values.channelValue(1));
        const quint16 field = quint16(values.channelValue(2));
        if (field == 0x102)
        {
            state.active = 0;
            state.stream = 0;
        }
        if (field == 0x101)
        {
            state.active = transaction;
            if (!state.stream)
            {
                state.stream = transaction;
            }
            for (int frame = 0; frame < beforeCommitFrames; ++frame)
            {
                page.submitPlSidebandFrame(audio200(stage, frame * 200, state.stream),
                                          1, frame * 4167);
            }
        }
        page.submitPlSidebandFrame(rawFrame({statusPacket(stage, state.active,
            state.stream, transaction, field)}), 1, 0);
    });
}
}
