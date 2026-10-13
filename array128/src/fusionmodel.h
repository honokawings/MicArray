#pragma once

#include "analysis/localizationtypes.h"

#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <memory>

namespace MicArray::Array128 {

struct FusionOptions
{
    double confidenceThreshold = .15;
    double activeThreshold = .5;
    double nmsIou = .45;
    int maxDetections = 16;
};

struct FusionDetection
{
    QRectF box; // Normalized video xywh; may extend outside the image, as in the model output.
    QPointF sourcePoint; // Independent neural regression, never substituted with box center or SRP peak.
    ScanDirection direction;
    double confidence = 0;
    double activeProbability = 0;
    int classId = 0;
    QString className;
};

struct FusionResult
{
    bool valid = false;
    QString error;
    QVector<FusionDetection> detections;
    QVector<float> rawOutput; // Exact [1260,9] output for reproducible numerical verification; not an acoustic heatmap.
    double elapsedMs = 0;
};

// Synchronous CPU inference. Own/use an instance on one worker thread; calls
// on the same instance are not concurrent. No model or runtime is downloaded.
class FusionModel
{
public:
    static constexpr int InputWidth = 320;
    static constexpr int InputHeight = 192;
    static constexpr int OutputAnchors = 1260;
    static constexpr int OutputFields = 9;

    FusionModel();
    ~FusionModel();
    FusionModel(const FusionModel &) = delete;
    FusionModel &operator=(const FusionModel &) = delete;

    bool load(const QString &modelPath, const QString &metadataPath,
              const QString &runtimePath = {}, QString *error = nullptr);
    void unload();
    bool isLoaded() const;
    QJsonObject metadata() const;
    QString runtimeVersion() const;
    FusionResult infer(const QImage &video, const QVector<float> &heat, int columns, int rows,
                       const CameraViewSettings &camera, const FusionOptions &options = {});

    static QVector<float> prepareInput(const QImage &video, const QVector<float> &heat,
                                       int columns, int rows, QString *error = nullptr);
    static bool validateMetadata(const QJsonObject &metadata, QString *error = nullptr);
    static QVector<FusionDetection> decodeOutput(const QVector<float> &output,
                                                const CameraViewSettings &camera,
                                                const FusionOptions &options = {},
                                                QString *error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace MicArray::Array128
