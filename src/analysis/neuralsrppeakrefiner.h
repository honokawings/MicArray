#pragma once

#include <QVector>

#include <array>

namespace MicArray {

struct NeuralSrpPeakInput
{
    static constexpr int PatchSize = 9;
    static constexpr int PatchValueCount =
        PatchSize * PatchSize;

    QVector<float> normalizedPatch;
    std::array<float, 12> diagnostics {};
};

struct NeuralSrpPeakOutput
{
    bool valid = false;
    double deltaAzimuthCells = 0.0;
    double deltaElevationCells = 0.0;
    double azimuthSigmaScale = 1.0;
    double elevationSigmaScale = 1.0;
    double correlationResidual = 0.0;
    double confidence = 0.0;
};

class NeuralSrpPeakRefiner
{
public:
    bool modelValid() const;
    NeuralSrpPeakOutput infer(
        const NeuralSrpPeakInput &input) const;
};

} // namespace MicArray
