#pragma once

#include "analysis/localizationtypes.h"

#include <QVector3D>

namespace MicArray {

QVector<MicrophonePosition> createDefaultArrayGeometry();
QVector3D directionFromAngles(double azimuthDegrees, double elevationDegrees);
SoundSourceDirection anglesFromDirection(const QVector3D &direction);

} // namespace MicArray
