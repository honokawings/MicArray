#include "ui/displayformat.h"

#include <QStringList>

#include <algorithm>
#include <cmath>

namespace MicArray {
namespace {

QString formatValue(double value, int unitIndex, const QString &suffix)
{
    static const QStringList units = {
        QStringLiteral("B"),
        QStringLiteral("KiB"),
        QStringLiteral("MiB"),
        QStringLiteral("GiB"),
        QStringLiteral("TiB"),
    };

    const int decimals = unitIndex == 0 ? 0 : (value < 10.0 ? 2 : (value < 100.0 ? 1 : 0));
    QString number = QString::number(value, 'f', decimals);
    if (number.contains(QLatin1Char('.'))) {
        while (number.endsWith(QLatin1Char('0'))) {
            number.chop(1);
        }
        if (number.endsWith(QLatin1Char('.'))) {
            number.chop(1);
        }
    }
    return QStringLiteral("%1 %2%3").arg(number, units.at(unitIndex), suffix);
}

QString formatBytes(double bytes, const QString &suffix)
{
    double value = std::isfinite(bytes) ? std::max(0.0, bytes) : 0.0;
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 4) {
        value /= 1024.0;
        ++unitIndex;
    }

    const int decimals = unitIndex == 0 ? 0 : (value < 10.0 ? 2 : (value < 100.0 ? 1 : 0));
    const double scale = std::pow(10.0, decimals);
    if (std::round(value * scale) / scale >= 1024.0 && unitIndex < 4) {
        value /= 1024.0;
        ++unitIndex;
    }
    return formatValue(value, unitIndex, suffix);
}

} // namespace

QString formatByteSize(quint64 bytes)
{
    return formatBytes(static_cast<double>(bytes), QString());
}

QString formatByteRate(double bytesPerSecond)
{
    return formatBytes(bytesPerSecond, QStringLiteral("/s"));
}

} // namespace MicArray
