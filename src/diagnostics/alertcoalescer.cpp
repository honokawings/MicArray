#include "diagnostics/alertcoalescer.h"

namespace MicArray {

AlertDecision AlertCoalescer::record(const QString &key, qint64 monotonicMs)
{
    const auto existing = m_entries.find(key);
    if (existing == m_entries.end()) {
        m_entries.insert(key, {monotonicMs, 0});
        return {true, 0};
    }

    Entry &entry = existing.value();
    if (monotonicMs - entry.lastEmissionMs < WindowMs) {
        ++entry.suppressedCount;
        return {false, entry.suppressedCount};
    }

    const int suppressed = entry.suppressedCount;
    entry.lastEmissionMs = monotonicMs;
    entry.suppressedCount = 0;
    return {true, suppressed};
}

} // namespace MicArray
