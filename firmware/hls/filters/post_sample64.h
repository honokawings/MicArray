#ifndef DENOISE_POST_SAMPLE64_H
#define DENOISE_POST_SAMPLE64_H

void denoise_bandpass64(bool reset_state, bool sample_valid, float sample,
    double hp_b0, double hp_b1, double hp_b2, double hp_a1, double hp_a2,
    double lp_b0, double lp_b1, double lp_b2, double lp_a1, double lp_a2,
    float &result);

void denoise_agc64(bool reset_state, bool sample_valid, float sample,
    double attack, double release, double target_amplitude,
    double minimum_gain, double maximum_gain, double limiter_amplitude,
    float &result);

#endif
