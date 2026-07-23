#pragma once

#include "analysis/localizationtypes.h"

#include <QSize>

namespace MicArray {

struct CameraViewFitResult {
    bool valid = false;
    CameraViewSettings camera;
    QVector<double> residualPixels;
    double rmsPixels = 0.0;
    QString error;
};

CameraViewFitResult fitCameraView(const QVector<VideoCalibrationSample> &samples,
                                  const CameraViewSettings &initial,
                                  const QSize &videoSize);

} // namespace MicArray
