#pragma once

#include <QVector>

#include <complex>

namespace MicArray {

class Radix2Fft
{
public:
    static bool transform(QVector<std::complex<double>> *values, bool inverse);
};

} // namespace MicArray
