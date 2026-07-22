#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace MicArray {

struct GainConfiguration
{
    static constexpr int ChannelCount = 48;
    static constexpr int MinimumShift = -8;
    static constexpr int MaximumShift = 8;

    QVector<int> shifts = QVector<int>(ChannelCount, 0);
    QVector<bool> automatic = QVector<bool>(ChannelCount, false);
    double upperPeakRatio = 0.80;
    double lowerPeakRatio = 0.35;
    int releaseHoldMs = 500;
    int releaseStepMs = 250;

    bool operator==(const GainConfiguration &) const = default;

    static bool validate(const GainConfiguration &configuration, QString *error = nullptr);
};

struct GainSnapshot
{
    QVector<int> shifts = QVector<int>(GainConfiguration::ChannelCount, 0);
    QVector<bool> automatic = QVector<bool>(GainConfiguration::ChannelCount, false);
    QVector<double> inputPeakRatios = QVector<double>(GainConfiguration::ChannelCount, 0.0);
    QVector<double> outputPeakRatios = QVector<double>(GainConfiguration::ChannelCount, 0.0);
    QVector<quint64> clipCounts = QVector<quint64>(GainConfiguration::ChannelCount, 0);
};

struct GainProcessResult
{
    bool valid = false;
    QString error;
    QByteArray payload;
    GainSnapshot snapshot;
};

class ChannelGainProcessor
{
public:
    bool setConfiguration(const GainConfiguration &configuration, QString *error = nullptr);
    const GainConfiguration &configuration() const { return m_configuration; }
    const GainSnapshot &snapshot() const { return m_snapshot; }

    GainProcessResult process(const QByteArray &payload, int sampleRate);

private:
    void updateAutomaticGain(const QVector<quint64> &inputPeaks, qint64 frameDurationMicros);

    GainConfiguration m_configuration;
    GainSnapshot m_snapshot;
    QVector<qint64> m_lowDurationMicros = QVector<qint64>(GainConfiguration::ChannelCount, 0);
    QVector<qint64> m_releaseDurationMicros = QVector<qint64>(GainConfiguration::ChannelCount, 0);
    QVector<quint64> m_clipCounts = QVector<quint64>(GainConfiguration::ChannelCount, 0);
};

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::GainConfiguration)
Q_DECLARE_METATYPE(MicArray::GainSnapshot)
