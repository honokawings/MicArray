#include "post_sample64.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static std::vector<float> read_floats(const std::string &name)
{
    std::ifstream file(name.c_str(), std::ios::binary);
    if (!file) return std::vector<float>();
    file.seekg(0, std::ios::end);
    const size_t count = static_cast<size_t>(file.tellg()) / sizeof(float);
    file.seekg(0);
    std::vector<float> values(count);
    file.read(reinterpret_cast<char *>(&values[0]), count * sizeof(float));
    return values;
}

int main(int argc, char **argv)
{
    const std::string root = argc > 1 ? argv[1] : "../../../../../../validation/golden_reference_current_3s_v2";
    const char *cases[] = {"background", "A_only", "B_only", "A_and_B"};
    const double pi = 3.141592653589793238462643383279502884;
    const double q = 0.70710678118654752440;
    const double wh = 2.0 * pi * 100.0 / 48000.0;
    const double wl = 2.0 * pi * 8000.0 / 48000.0;
    const double ch = std::cos(wh), cl = std::cos(wl);
    const double ah = std::sin(wh) / (2.0 * q), al = std::sin(wl) / (2.0 * q);
    const double hp[] = {(1.0 + ch)*0.5/(1.0+ah), -(1.0+ch)/(1.0+ah), (1.0+ch)*0.5/(1.0+ah), -2.0*ch/(1.0+ah), (1.0-ah)/(1.0+ah)};
    const double lp[] = {(1.0 - cl)*0.5/(1.0+al), (1.0-cl)/(1.0+al), (1.0-cl)*0.5/(1.0+al), -2.0*cl/(1.0+al), (1.0-al)/(1.0+al)};
    const double attack = std::exp(-1.0/(20.0*0.001*48000.0));
    const double release = std::exp(-1.0/(300.0*0.001*48000.0));
    const double d = 0.11512925464970228420;
    const double target = std::exp(-24.0*d), lo = std::exp(-20.0*d), hi = std::exp(20.0*d), limiter = std::exp(-1.0*d);
    int errors = 0;
    for (int c = 0; c < 4; ++c)
    {
        const std::string folder = root + "/" + cases[c] + "/reference/";
#ifdef AGC_TOP
        const std::vector<float> input = read_floats(folder + "post_logmmse.f32le");
        const std::vector<float> expected = read_floats(folder + "post_agc.f32le");
#else
        const std::vector<float> input = read_floats(folder + "wiener.f32le");
        const std::vector<float> expected = read_floats(folder + "post_bandpass.f32le");
#endif
        if (input.empty() || input.size() != expected.size())
        {
            std::printf("[TB_ERROR] missing/mismatched golden: %s\n", folder.c_str());
            return 1;
        }
        float output = 0.0f;
#ifdef AGC_TOP
        denoise_agc64(true, false, 0.0f, attack, release, target, lo, hi, limiter, output);
#else
        denoise_bandpass64(true, false, 0.0f, hp[0], hp[1], hp[2], hp[3], hp[4], lp[0], lp[1], lp[2], lp[3], lp[4], output);
#endif
        double peak = 0.0, squared = 0.0, signal = 0.0;
        int exact = 0;
        const size_t limit = argc > 2 ? std::min(input.size(), static_cast<size_t>(std::atoi(argv[2]))) : input.size();
        for (size_t i = 0; i < limit; ++i)
        {
#ifdef AGC_TOP
            denoise_agc64(false, true, input[i], attack, release, target, lo, hi, limiter, output);
#else
            denoise_bandpass64(false, true, input[i], hp[0], hp[1], hp[2], hp[3], hp[4], lp[0], lp[1], lp[2], lp[3], lp[4], output);
#endif
            const double delta = static_cast<double>(output) - expected[i];
            peak = std::max(peak, std::abs(delta));
            squared += delta*delta;
            signal += static_cast<double>(expected[i])*expected[i];
            if (std::memcmp(&output, &expected[i], sizeof(float)) == 0) ++exact;
            if (!std::isfinite(output)) ++errors;
        }
        const double ratio = squared == 0.0 ? 999.0 : 10.0*std::log10(signal/squared);
        std::printf("[TB_DATA] case=%s count=%llu exact=%d maxabs=%.17g error_snr_db=%.9f\n", cases[c], static_cast<unsigned long long>(limit), exact, peak, ratio);
        if (peak > 1.0e-7 || (squared != 0.0 && ratio < 100.0)) ++errors;
    }
    if (errors)
    {
        std::printf("[TB_ERROR] numerical gate errors=%d\n", errors);
        return 1;
    }
    std::printf("[TB_PASS] original scalar post recurrence preserved; no time/gain fitting\n");
    return 0;
}
