#pragma once

#include "protocol.h"

#include <QJsonObject>
#include <map>

namespace MicArray::Array128 {

// Exact intervals within a bounded audit. If an adversarial stream exhausts
// the bound, accuracy is explicitly invalidated rather than claiming no loss.
class SequenceTracker
{
public:
    enum class Event { First, Continuous, Gap, Duplicate, Reordered, BeforeStart, CapacityExceeded };
    Event observe(quint64 value);
    quint64 missing() const;
    QJsonObject json() const;
    quint64 duplicates = 0;
    quint64 reordered = 0;
    quint64 beforeStart = 0;
    quint64 forwardGaps = 0;
    bool exact = true;
private:
    bool m_started = false;
    quint64 m_first = 0;
    quint64 m_highest = 0;
    quint64 m_unique = 0;
    std::map<quint64, quint64> m_ranges;
};

struct SelftestAudit
{
    explicit SelftestAudit(const QByteArray &reference = {});
    void observe(const Packet &packet, bool continuous, bool hostDropped);
    QString status() const;
    QJsonObject json() const;
    bool referenceValid = false;
    quint64 frames = 0;
    quint64 mismatchingFrames = 0;
    quint64 mismatchingSamples = 0;
    quint64 continuityErrors = 0;
    quint64 flaggedPackets = 0;
    bool hostLoss = false;
    QString firstMismatch;
    std::array<quint64, Channels> channelMismatches {};
private:
    QByteArray m_reference;
};

QByteArray selftestReference();

} // namespace MicArray::Array128
