#include "analysis/neuralsrppeakrefiner.h"

#include "analysis/neuralmodeldata.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace MicArray {

namespace {

constexpr std::uint64_t FnvOffset =
    UINT64_C(14695981039346656037);
constexpr std::uint64_t FnvPrime =
    UINT64_C(1099511628211);

void addChecksumFloat(std::uint64_t *checksum, float value)
{
    const std::uint32_t bits =
        std::bit_cast<std::uint32_t>(value);
    for (int shift = 0; shift < 32; shift += 8) {
        const std::uint8_t byte =
            static_cast<std::uint8_t>(
                (bits >> shift) & 0xffU);
        *checksum ^= byte;
        *checksum *= FnvPrime;
    }
}

template<std::size_t Size>
void addChecksumArray(
    std::uint64_t *checksum,
    const std::array<float, Size> &values)
{
    for (float value : values) {
        addChecksumFloat(checksum, value);
    }
}

std::uint64_t modelChecksum()
{
    std::uint64_t checksum = FnvOffset;
    addChecksumArray(
        &checksum, NeuralModelData::InputWeights);
    addChecksumArray(
        &checksum, NeuralModelData::Hidden1Bias);
    addChecksumArray(
        &checksum, NeuralModelData::Hidden1Weights);
    addChecksumArray(
        &checksum, NeuralModelData::Hidden2Bias);
    addChecksumArray(
        &checksum, NeuralModelData::Hidden2Weights);
    addChecksumArray(
        &checksum, NeuralModelData::OutputBias);
    addChecksumArray(
        &checksum, NeuralModelData::InputMeans);
    addChecksumArray(
        &checksum, NeuralModelData::InputScales);
    return checksum;
}

double sigmoid(double value)
{
    if (value >= 0.0) {
        const double exponential = std::exp(-value);
        return 1.0 / (1.0 + exponential);
    }
    const double exponential = std::exp(value);
    return exponential / (1.0 + exponential);
}

bool allFinite(const NeuralSrpPeakInput &input)
{
    return std::all_of(
               input.normalizedPatch.cbegin(),
               input.normalizedPatch.cend(),
               [](float value) {
        return std::isfinite(value);
    })
        && std::all_of(
            input.diagnostics.cbegin(),
            input.diagnostics.cend(),
            [](float value) {
        return std::isfinite(value);
    });
}

} // namespace

bool NeuralSrpPeakRefiner::modelValid() const
{
    static const bool valid =
        NeuralModelData::ModelVersion == 1
        && NeuralModelData::InputCount
            == NeuralSrpPeakInput::PatchValueCount
                + static_cast<int>(
                    NeuralSrpPeakInput {}.diagnostics.size())
        && NeuralModelData::Hidden1Count == 48
        && NeuralModelData::Hidden2Count == 24
        && NeuralModelData::OutputCount == 6
        && NeuralModelData::ParameterCount
            == static_cast<int>(
                NeuralModelData::InputWeights.size()
                + NeuralModelData::Hidden1Bias.size()
                + NeuralModelData::Hidden1Weights.size()
                + NeuralModelData::Hidden2Bias.size()
                + NeuralModelData::Hidden2Weights.size()
                + NeuralModelData::OutputBias.size())
        && NeuralModelData::ParameterCount <= 8192
        && modelChecksum() == NeuralModelData::Checksum;
    return valid;
}

NeuralSrpPeakOutput NeuralSrpPeakRefiner::infer(
    const NeuralSrpPeakInput &input) const
{
    NeuralSrpPeakOutput output;
    if (!modelValid()
        || input.normalizedPatch.size()
            != NeuralSrpPeakInput::PatchValueCount
        || !allFinite(input)) {
        return output;
    }
    const auto [minimum, maximum] =
        std::minmax_element(
            input.normalizedPatch.cbegin(),
            input.normalizedPatch.cend());
    if (maximum == input.normalizedPatch.cend()
        || static_cast<double>(*maximum) - *minimum
            <= 1.0e-5) {
        return output;
    }

    std::array<double, NeuralModelData::InputCount>
        normalized {};
    for (int index = 0;
         index < input.normalizedPatch.size();
         ++index) {
        normalized[index] =
            input.normalizedPatch.at(index);
    }
    for (int index = 0;
         index < static_cast<int>(
             input.diagnostics.size());
         ++index) {
        normalized[
            NeuralSrpPeakInput::PatchValueCount
            + index] = input.diagnostics.at(index);
    }
    for (int index = 0;
         index < normalized.size();
         ++index) {
        const double scale =
            NeuralModelData::InputScales.at(index);
        if (!std::isfinite(scale)
            || std::abs(scale) <= 1.0e-12) {
            return output;
        }
        normalized[index] = std::clamp(
            (normalized.at(index)
             - NeuralModelData::InputMeans.at(index))
                / scale,
            -8.0, 8.0);
    }

    std::array<double, NeuralModelData::Hidden1Count>
        hidden1 {};
    for (int target = 0;
         target < hidden1.size();
         ++target) {
        double sum =
            NeuralModelData::Hidden1Bias.at(target);
        for (int source = 0;
             source < normalized.size();
             ++source) {
            sum += normalized.at(source)
                * NeuralModelData::InputWeights.at(
                    target
                        * NeuralModelData::InputCount
                    + source);
        }
        hidden1[target] = std::tanh(sum);
    }

    std::array<double, NeuralModelData::Hidden2Count>
        hidden2 {};
    for (int target = 0;
         target < hidden2.size();
         ++target) {
        double sum =
            NeuralModelData::Hidden2Bias.at(target);
        for (int source = 0;
             source < hidden1.size();
             ++source) {
            sum += hidden1.at(source)
                * NeuralModelData::Hidden1Weights.at(
                    target
                        * NeuralModelData::Hidden1Count
                    + source);
        }
        hidden2[target] = std::tanh(sum);
    }

    std::array<double, NeuralModelData::OutputCount>
        raw {};
    for (int target = 0;
         target < raw.size();
         ++target) {
        double sum =
            NeuralModelData::OutputBias.at(target);
        for (int source = 0;
             source < hidden2.size();
             ++source) {
            sum += hidden2.at(source)
                * NeuralModelData::Hidden2Weights.at(
                    target
                        * NeuralModelData::Hidden2Count
                    + source);
        }
        if (!std::isfinite(sum)) {
            return {};
        }
        raw[target] = sum;
    }

    output.valid = true;
    output.deltaAzimuthCells =
        std::tanh(raw.at(0));
    output.deltaElevationCells =
        std::tanh(raw.at(1));
    output.azimuthSigmaScale =
        0.35 + 0.65 * sigmoid(raw.at(2));
    output.elevationSigmaScale =
        0.35 + 0.65 * sigmoid(raw.at(3));
    output.correlationResidual =
        0.35 * std::tanh(raw.at(4));
    output.confidence = sigmoid(raw.at(5));
    return output;
}

} // namespace MicArray
