#pragma once

#include <QHash>
#include <QString>

namespace MicArray {

struct AlertDecision {
    bool shouldEmit = false;
    int suppressedCount = 0;
};

class AlertCoalescer
{
public:
    static constexpr qint64 WindowMs = 1000;

    AlertDecision record(const QString &key, qint64 monotonicMs);
    void clear() { m_entries.clear(); }

private:
    struct Entry {
        qint64 lastEmissionMs = 0;
        int suppressedCount = 0;
    };
    QHash<QString, Entry> m_entries;
};

} // namespace MicArray
