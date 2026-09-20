#pragma once

#include "analysis/localizationtypes.h"

#include <QJsonObject>

namespace MicArray::Array128 {

struct Geometry
{
    QString name;
    QVector<MicrophonePosition> microphones;
    bool valid() const { return microphones.size() == 128; }
    bool operator==(const Geometry &) const = default;
    QJsonObject json() const;
    static bool load(const QByteArray &json, Geometry *destination, QString *error);
};

} // namespace MicArray::Array128
