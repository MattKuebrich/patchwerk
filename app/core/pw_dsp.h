/* SPDX-License-Identifier: GPL-3.0-only
 * Freestanding single-precision helpers. No libm: the same code runs in the
 * FM-1 audio ISR (SRAM, FPU) and in a WebAssembly module without a libc. */
#ifndef PW_DSP_H
#define PW_DSP_H
#include <stdint.h>
#include "pw_engine.h"

#define PW_INLINE static inline __attribute__((always_inline))
#define PW_PI 3.14159265358979f
#define PW_TWO_PI 6.28318530717959f
#define PW_C4 261.625565f
#define PW_INV_SR (pw_isr)

#define PW_SINE_BITS 10
#define PW_SINE_SIZE (1 << PW_SINE_BITS)
extern float pw_sine_table[PW_SINE_SIZE + 1];

PW_INLINE float pw_clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
PW_INLINE float pw_absf(float x) { return x < 0 ? -x : x; }
PW_INLINE float pw_maxf(float a, float b) { return a > b ? a : b; }
PW_INLINE float pw_minf(float a, float b) { return a < b ? a : b; }
PW_INLINE int pw_floori(float x) { int i = (int)x; return (x < (float)i) ? i - 1 : i; }
PW_INLINE float pw_frac(float x) { return x - (float)pw_floori(x); }
PW_INLINE float pw_lerp(float a, float b, float t) { return a + (b - a) * t; }

/* 2^x, |error| < 1e-7 relative over [0,1) of the mantissa polynomial. */
PW_INLINE float pw_exp2(float x)
{
    if (x < -60.0f) x = -60.0f;
    if (x > 60.0f) x = 60.0f;
    int i = pw_floori(x);
    float f = x - (float)i;
    float p = 1.0f + f * (0.69315308f + f * (0.24015361f + f * (0.05582631f +
              f * (0.00898934f + f * 0.00187757f))));
    union { float f; int32_t i; } u;
    u.i = (i + 127) << 23;
    return p * u.f;
}
/* log2 for control-rate use (decibel display, period estimates). */
PW_INLINE float pw_log2(float x)
{
    if (x <= 0) return -60.0f;
    union { float f; int32_t i; } u = { x };
    float e = (float)(((u.i >> 23) & 255) - 128);
    u.i = (u.i & 0x007fffff) | 0x3f800000;   /* m in [1,2) */
    float m = u.f;
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}
/* sin(2*pi*phase), phase any real; table + linear interpolation. */
PW_INLINE float pw_sin1(float phase)
{
    float x = (phase - (float)pw_floori(phase)) * (float)PW_SINE_SIZE;
    int i = (int)x;
    float f = x - (float)i;
    return pw_sine_table[i] + (pw_sine_table[i + 1] - pw_sine_table[i]) * f;
}
PW_INLINE float pw_cos1(float phase) { return pw_sin1(phase + 0.25f); }
/* tan(pi * x) for 0 <= x < 0.5 */
PW_INLINE float pw_tan_pi(float x)
{
    float s = pw_sin1(x * 0.5f), c = pw_cos1(x * 0.5f);
    return s / (c < 1e-4f ? 1e-4f : c);
}
/* Smooth saturator, exact tanh within 2% and hard limited at |x| = 3. */
PW_INLINE float pw_tanh(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
PW_INLINE float pw_pitch_hz(float volts) { return PW_C4 * pw_exp2(volts); }

/* PolyBLEP residual for a discontinuity at phase 0 with increment dt. */
PW_INLINE float pw_blep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
/* xorshift32 white noise in [-1, 1). */
PW_INLINE uint32_t pw_rand_u(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
PW_INLINE float pw_rand_f(uint32_t *s)
{
    return (float)(int32_t)pw_rand_u(s) * (1.0f / 2147483648.0f);
}
/* Schmitt trigger: returns 1 on a rising edge. */
PW_INLINE int pw_rise(uint8_t *hi, float v)
{
    if (*hi) { if (v < 0.5f) *hi = 0; return 0; }
    if (v > 1.0f) { *hi = 1; return 1; }
    return 0;
}
/* One-pole coefficient for a time constant in seconds. */
PW_INLINE float pw_coef(float seconds)
{
    if (seconds < 1e-5f) return 0.0f;
    return pw_exp2(-1.442695f / (seconds * PW_SRF));
}
PW_INLINE float pw_lp_coef_hz(float hz)
{
    float w = PW_TWO_PI * hz * PW_INV_SR;
    return w / (1.0f + w);
}
#define PW_POOL16(m) ((int16_t *)(pw_pool + (m)->mem_off))
extern uint8_t pw_pool[];
#endif
