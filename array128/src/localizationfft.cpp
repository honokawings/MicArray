#include "localizationfft.h"

#include <array>
#include <cmath>
#include <numbers>

namespace MicArray::Array128 {
namespace {
struct Plan
{
    std::array<int, LocalizationFft::Size> reversed {};
    std::array<std::complex<double>, LocalizationFft::Size / 2> roots {};

    Plan()
    {
        for (int i = 0; i < LocalizationFft::Size; ++i) {
            int value = i;
            for (int bit = 0; bit < 11; ++bit) { reversed[i] = (reversed[i] << 1) | (value & 1); value >>= 1; }
        }
        for (int i = 0; i < int(roots.size()); ++i) {
            const double angle = -2 * std::numbers::pi * i / LocalizationFft::Size;
            roots[i] = {std::cos(angle), std::sin(angle)};
        }
    }
};
}

bool LocalizationFft::transform(QVector<std::complex<double>> *values, bool inverse)
{
    if (!values || values->size() != Size) return false;
    static const Plan plan;
    // Detach exactly once. Repeated non-const QVector indexing inside millions
    // of butterflies otherwise repeats implicit-sharing checks on every access.
    auto *data = values->data();
    for (int i = 0; i < Size; ++i) if (i < plan.reversed[i]) std::swap(data[i], data[plan.reversed[i]]);
    for (int length = 2; length <= Size; length <<= 1) {
        const int half = length / 2, stride = Size / length;
        for (int start = 0; start < Size; start += length) for (int offset = 0; offset < half; ++offset) {
            const auto factor = inverse ? std::conj(plan.roots[offset * stride]) : plan.roots[offset * stride];
            const auto even = data[start + offset], odd = data[start + offset + half] * factor;
            data[start + offset] = even + odd; data[start + offset + half] = even - odd;
        }
    }
    if (inverse) for (int i = 0; i < Size; ++i) data[i] /= Size;
    return true;
}

} // namespace MicArray::Array128
