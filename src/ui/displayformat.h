#pragma once

#include <QString>
#include <QtTypes>

namespace MicArray {

QString formatByteSize(quint64 bytes);
QString formatByteRate(double bytesPerSecond);

} // namespace MicArray
