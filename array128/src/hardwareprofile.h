#pragma once

#include <QJsonObject>
#include <QStringList>
#include <QVector>

namespace MicArray::Array128 {

// Documented board configuration, NOT live telemetry or a detected firmware ID.
QString dataConnector(int channel);
QString dataBank(int channel);
QString dataPin(int channel);
QJsonObject hardwareReference();
QVector<QStringList> hardwareSpecificationRows();
QString commissioningGuide();

} // namespace MicArray::Array128
