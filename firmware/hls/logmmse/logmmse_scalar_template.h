#include <cmath>
#include <cfloat>

// Direct scalar transcription of GitHub aa281b34 LogMmseProcessor::processFrame.
// Arithmetic order is intentional. State and parameters are external double64.
template<int LANE> static double e1_original(double value,bool &cap_hit) {
    cap_hit=false;
    const double x = value > 1.0e-12 ? value : 1.0e-12;
    if (x <= 1.0) {
        double sum = 0.0, term = 1.0;bool converged=false;
        for (int index = 1; index <= 20; ++index) {
            term *= -x / static_cast<double>(index);
            const double addition = term / static_cast<double>(index);
            sum += addition;
            if (std::fabs(addition) < std::fabs(sum) * DBL_EPSILON) {converged=true;break;}
        }
        cap_hit=!converged;
        return -0.57721566490153286061 - std::log(x) - sum;
    }
    const double Tiny = 1.0e-300;
    double b = x + 1.0, c = 1.0 / Tiny, d = 1.0 / b, result = d;bool converged=false;
    for (int index = 1; index <= 55; ++index) {
        const double a = -static_cast<double>(index * index);
        b += 2.0;
        d = a * d + b;
        if (std::fabs(d) < Tiny) d = Tiny;
        c = b + a / c;
        if (std::fabs(c) < Tiny) c = Tiny;
        d = 1.0 / d;
        const double delta = c * d;
        result *= delta;
        if (std::fabs(delta - 1.0) < 1.0e-12) {converged=true;break;}
    }
    cap_hit=!converged;
    return result * std::exp(-x);
}

template<int LANE> static void scalar_bin(double power, double old_noise, double old_gamma,
                 double old_gain, double alpha, double beta, double min_gain,
                 double &new_gain, double &new_noise, double &new_gamma,bool &numeric_fault) {
#pragma HLS INLINE off
    const double noise = old_noise > 1.0e-12 ? old_noise : 1.0e-12;
    power = power > 1.0e-12 ? power : 1.0e-12;
    double gamma = power / noise;
    if (gamma < 1.0e-6) gamma = 1.0e-6;
    if (gamma > 1000.0) gamma = 1000.0;
    const double instantaneous = gamma > 1.0 ? gamma - 1.0 : 0.0;
    const double decision_directed = old_gain * old_gain * old_gamma;
    double xi = alpha * decision_directed + (1.0 - alpha) * instantaneous;
    if (xi < 1.0e-8) xi = 1.0e-8;
    double v = xi * gamma / (1.0 + xi);
    if (v < 1.0e-12) v = 1.0e-12;
    double gain = xi / (1.0 + xi) * std::exp(0.5 * e1_original<LANE>(v,numeric_fault));
    // All inputs are finite bounded algorithm states; guard also matches source.
    if (!std::isfinite(gain)) gain = min_gain;
    if (gain < min_gain) gain = min_gain;
    if (gain > 1.0) gain = 1.0;
    double smoothing = beta;
    if (gamma >= 3.0) smoothing = beta + 0.04 < 0.999 ? beta + 0.04 : 0.999;
    new_gain = gain;
    new_noise = smoothing * noise + (1.0 - smoothing) * power;
    new_gamma = gamma;
}
