#pragma once

#include <QVector>
#include <complex>

namespace MicArray::Array128 {

// Fixed-size plan for the independent 128-channel SRP path. The immutable plan
// is shared safely; sample storage belongs to each caller. Legacy FFT unchanged.
class LocalizationFft final
{
public:
    static constexpr int Size = 2048;
    static bool transform(QVector<std::complex<double>> *values, bool inverse);
};

} // namespace MicArray::Array128
