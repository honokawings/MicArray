#include "analysis/radix2fft.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace MicArray {

bool Radix2Fft::transform(QVector<std::complex<double>> *values, bool inverse)
{
    if (values == nullptr || values->isEmpty()) {
        return false;
    }
    const int size = values->size();
    if ((size & (size - 1)) != 0) {
        return false;
    }

    for (int index = 1, reversed = 0; index < size; ++index) {
        int bit = size >> 1;
        while ((reversed & bit) != 0) {
            reversed ^= bit;
            bit >>= 1;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap((*values)[index], (*values)[reversed]);
        }
    }

    for (int length = 2; length <= size; length <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * std::numbers::pi
            / static_cast<double>(length);
        const std::complex<double> root(std::cos(angle), std::sin(angle));
        for (int start = 0; start < size; start += length) {
            std::complex<double> factor(1.0, 0.0);
            const int half = length / 2;
            for (int offset = 0; offset < half; ++offset) {
                const std::complex<double> even = (*values)[start + offset];
                const std::complex<double> odd = (*values)[start + offset + half] * factor;
                (*values)[start + offset] = even + odd;
                (*values)[start + offset + half] = even - odd;
                factor *= root;
            }
        }
    }

    if (inverse) {
        for (std::complex<double> &value : *values) {
            value /= static_cast<double>(size);
        }
    }
    return true;
}

} // namespace MicArray
