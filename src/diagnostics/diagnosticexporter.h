#pragma once

#include <QDateTime>
#include <QStringList>
#include <QVariantMap>

namespace MicArray {

struct SessionMetrics {
    QDateTime startedAt;
    QDateTime endedAt;
    quint64 totalBytes = 0;
    quint64 totalFrames = 0;
    quint64 parseErrors = 0;
    quint64 reconnectCount = 0;
    quint64 audioDroppedFrames = 0;
    quint64 videoDroppedFrames = 0;
    double averageReceiveMbps = 0.0;
    double peakReceiveMbps = 0.0;
};

struct DiagnosticBundle {
    SessionMetrics session;
    QVariantMap settings;
    QStringList events;
};

class DiagnosticExporter
{
public:
    static QString exportBundle(const QString &baseDirectory,
                                const DiagnosticBundle &bundle,
                                const QStringList &logFiles,
                                QString *errorMessage = nullptr);
};

} // namespace MicArray
