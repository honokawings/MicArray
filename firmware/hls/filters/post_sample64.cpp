#include "post_sample64.h"
#include <hls_math.h>

// HLS candidates derived from the original GitHub double-precision recurrence.
// Configuration trigonometric/exp constants are computed by the original host
// formula once per reset. Each node keeps the original float32 output boundary.
// No unsafe-math or fused-multiply-add optimization is authorized.

void denoise_bandpass64(bool reset_state, bool sample_valid, float sample,
    double hp_b0, double hp_b1, double hp_b2, double hp_a1, double hp_a2,
    double lp_b0, double lp_b1, double lp_b2, double lp_a1, double lp_a2,
    float &result)
{
#pragma HLS INTERFACE ap_ctrl_hs port=return
#pragma HLS INTERFACE ap_none port=reset_state
#pragma HLS INTERFACE ap_none port=sample_valid
#pragma HLS INTERFACE ap_none port=sample
#pragma HLS INTERFACE ap_none port=hp_b0
#pragma HLS INTERFACE ap_none port=hp_b1
#pragma HLS INTERFACE ap_none port=hp_b2
#pragma HLS INTERFACE ap_none port=hp_a1
#pragma HLS INTERFACE ap_none port=hp_a2
#pragma HLS INTERFACE ap_none port=lp_b0
#pragma HLS INTERFACE ap_none port=lp_b1
#pragma HLS INTERFACE ap_none port=lp_b2
#pragma HLS INTERFACE ap_none port=lp_a1
#pragma HLS INTERFACE ap_none port=lp_a2
#pragma HLS INTERFACE ap_vld port=result
#pragma HLS ALLOCATION instances=dmul limit=2 operation
#pragma HLS ALLOCATION instances=dadd limit=2 operation
#pragma HLS ALLOCATION instances=dsub limit=2 operation
    static double hp_delay1 = 0.0;
    static double hp_delay2 = 0.0;
    static double lp_delay1 = 0.0;
    static double lp_delay2 = 0.0;
    if (reset_state)
    {
        hp_delay1 = 0.0;
        hp_delay2 = 0.0;
        lp_delay1 = 0.0;
        lp_delay2 = 0.0;
        result = 0.0f;
        return;
    }
    if (!sample_valid)
    {
        result = 0.0f;
        return;
    }
    const double hp_output = hp_b0 * static_cast<double>(sample) + hp_delay1;
    hp_delay1 = hp_b1 * static_cast<double>(sample) - hp_a1 * hp_output + hp_delay2;
    hp_delay2 = hp_b2 * static_cast<double>(sample) - hp_a2 * hp_output;
    const double lp_output = lp_b0 * hp_output + lp_delay1;
    lp_delay1 = lp_b1 * hp_output - lp_a1 * lp_output + lp_delay2;
    lp_delay2 = lp_b2 * hp_output - lp_a2 * lp_output;
    const double finite = hls::isfinite(lp_output) ? lp_output : 0.0;
    result = static_cast<float>(finite > 1.0 ? 1.0 : finite < -1.0 ? -1.0 : finite);
}

void denoise_agc64(bool reset_state, bool sample_valid, float sample,
    double attack, double release, double target_amplitude,
    double minimum_gain, double maximum_gain, double limiter_amplitude,
    float &result)
{
#pragma HLS INTERFACE ap_ctrl_hs port=return
#pragma HLS INTERFACE ap_none port=reset_state
#pragma HLS INTERFACE ap_none port=sample_valid
#pragma HLS INTERFACE ap_none port=sample
#pragma HLS INTERFACE ap_none port=attack
#pragma HLS INTERFACE ap_none port=release
#pragma HLS INTERFACE ap_none port=target_amplitude
#pragma HLS INTERFACE ap_none port=minimum_gain
#pragma HLS INTERFACE ap_none port=maximum_gain
#pragma HLS INTERFACE ap_none port=limiter_amplitude
#pragma HLS INTERFACE ap_vld port=result
#pragma HLS ALLOCATION instances=dmul limit=2 operation
#pragma HLS ALLOCATION instances=dadd limit=2 operation
#pragma HLS ALLOCATION instances=dsub limit=2 operation
#pragma HLS ALLOCATION instances=ddiv limit=1 operation
#pragma HLS ALLOCATION instances=dsqrt limit=1 operation
    static double envelope = 0.0;
    static double gain = 1.0;
    if (reset_state)
    {
        envelope = 0.0;
        gain = 1.0;
        result = 0.0f;
        return;
    }
    if (!sample_valid)
    {
        result = 0.0f;
        return;
    }
    const double finite_sample = hls::isfinite(sample) ? static_cast<double>(sample) : 0.0;
    const double power = finite_sample * finite_sample;
    envelope = attack * envelope + (1.0 - attack) * power;
    const double bounded_envelope = envelope > 1.0e-12 ? envelope : 1.0e-12;
    const double raw_desired = target_amplitude / hls::sqrt(bounded_envelope);
    const double desired = raw_desired > maximum_gain ? maximum_gain : raw_desired < minimum_gain ? minimum_gain : raw_desired;
    const double coefficient = desired < gain ? attack : release;
    gain = coefficient * gain + (1.0 - coefficient) * desired;
    const double controlled = hls::isfinite(gain) ? finite_sample * gain : 0.0;
    result = static_cast<float>(controlled > limiter_amplitude ? limiter_amplitude :
        controlled < -limiter_amplitude ? -limiter_amplitude : controlled);
}
