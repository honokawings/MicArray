#pragma once

#include "analysis/localizationtypes.h"

#include <QByteArray>
#include <QSize>

namespace MicArray::Array128 {

struct CameraProfile
{
    CameraViewSettings camera;
    QVector<VideoCalibrationSample> samples;
    QSize referenceSize;
    QByteArray json() const;
    static bool load(const QByteArray &bytes, CameraProfile *destination, QString *error = nullptr);
};

} // namespace MicArray::Array128
