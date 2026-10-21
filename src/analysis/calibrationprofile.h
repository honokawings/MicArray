#pragma once

#include "analysis/localizationtypes.h"

#include <QDateTime>
#include <QJsonObject>
#include <QSize>
#include <QString>

namespace MicArray {

QJsonObject cameraLensToJson(const CameraLensCalibration &lens);
bool cameraLensFromJson(const QJsonObject &object,
                        CameraLensCalibration *lens,
                        QString *errorMessage = nullptr);

struct VideoCalibrationProfile {
    CameraViewSettings camera;
    QSize referenceVideoSize = {0, 0};
    double calibrationRmsPixels = 0.0;
    QVector<VideoCalibrationSample> samples;
    QDateTime savedAtUtc;

    bool operator==(const VideoCalibrationProfile &) const = default;
};

class VideoCalibrationProfileStore
{
public:
    static bool save(const QString &filePath,
                     const VideoCalibrationProfile &profile,
                     QString *errorMessage = nullptr);
    static bool load(const QString &filePath,
                     VideoCalibrationProfile *profile,
                     QString *errorMessage = nullptr);
    static bool validate(const VideoCalibrationProfile &profile,
                         QString *errorMessage = nullptr);
};

} // namespace MicArray
