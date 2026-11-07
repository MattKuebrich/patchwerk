/* SPDX-License-Identifier: GPL-3.0-only
 * PATCHWERK module library: DSP and descriptors for every module type. */
#define PW_NVAR 1   /* in the core PW_N is the engine block (64 or 128 frames, see pw_set_rate) */
#include "pw_dsp.h"
#include <string.h>
#include <stddef.h>

#define ST(T) T *s = (T *)(void *)m->stp; \
    _Static_assert(sizeof(T) <= PW_STATE_BYTES, "state too large: " #T)
#define IN_CONNECTED(k) ((connected >> (k)) & 1U)
#define PROC(name) static PW_DSP void name(pw_module *m, const float *const *in, \
                                           float *const *out, unsigned connected)
#define PROC_COLD(name) PW_COLD PROC(name)

float pw_sine_table[PW_SINE_SIZE + 1] PW_POOL;   /* (in the pool: the RAM is full; filled at start) */
uint8_t pw_ram_err;
float pw_mix_l[PW_NMAX], pw_mix_r[PW_NMAX];

/* Device and browser memory budgets for the modules that need audio memory. */
#ifdef PW_WEB
#define PW_DELAY_SECONDS   2.0f
#define PW_SAMPLER_SECONDS 10.0f
#define PW_FRACTURE_SECONDS 1.5f
#define PW_REVERSE_SECONDS 2.0f
#define PW_GRAIN_SECONDS   4.0f
#define PW_TAPE_SECONDS    1.5f
#define PW_SHUF_SECONDS    4.0f
#define PW_LOOP_SECONDS    16.0f
#else
#define PW_DELAY_SECONDS   0.6f
#define PW_SAMPLER_SECONDS 1.0f
#define PW_FRACTURE_SECONDS 0.5f
#define PW_REVERSE_SECONDS 0.5f
#define PW_GRAIN_SECONDS   0.6f
#define PW_TAPE_SECONDS    0.3f
#define PW_SHUF_SECONDS    1.2f   /* at half rate: 53 KB */
#define PW_LOOP_SECONDS    3.0f   /* at a quarter rate: 66 KB */
#endif
#define LOOP_BYTES(s) ((uint32_t)((s) * 11025.0f) * 2u + 16u)
#define SECONDS_TO_BYTES(s) ((uint32_t)((s) * 44100.0f) * 2u + 16u)
#define MAXU(a, b) ((a) > (b) ? (a) : (b))
/* FRACTURE's memory: what the largest of the three it does (glitch, reverse, shuffle) wants */
#define FRACT_MEM_IDEAL MAXU(MAXU(SECONDS_TO_BYTES(PW_FRACTURE_SECONDS), SECONDS_TO_BYTES(PW_REVERSE_SECONDS * 2.0f)), (uint32_t)(PW_SHUF_SECONDS * 22050.0f) * 2u + 16u)
#define FRACT_MEM_MIN   MAXU(MAXU(SECONDS_TO_BYTES(0.12f), SECONDS_TO_BYTES(0.3f)), (uint32_t)(0.3f * 22050.0f) * 2u + 16u)

/* ------------------------------------------------------------------ tables */
#define WT_BANKS 4
#define WT_FRAMES 8
#define WT_SIZE 256
#define WT_BYTES (WT_BANKS * WT_FRAMES * WT_SIZE * 2)   /* (in the WAVETABLE module's pool region, built when one is added) */
typedef int16_t wt_tab[WT_FRAMES][WT_SIZE];
static wt_tab *wt_dst;

static float sin_slow(float x) /* init only: sin(x) for any x */
{
    while (x > PW_PI) x -= PW_TWO_PI;
    while (x < -PW_PI) x += PW_TWO_PI;
    float x2 = x * x, term = x, sum = x;
    for (int k = 1; k < 10; ++k) {
        term *= -x2 / (float)((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}
static float gauss(float x) { return pw_exp2(-1.442695f * x * x); }

static void wt_store(int bank, int frame, float *buf)
{
    float peak = 1e-6f, mean = 0;
    for (int n = 0; n < WT_SIZE; ++n) mean += buf[n];
    mean /= WT_SIZE;
    for (int n = 0; n < WT_SIZE; ++n) { buf[n] -= mean; if (pw_absf(buf[n]) > peak) peak = pw_absf(buf[n]); }
    for (int n = 0; n < WT_SIZE; ++n) wt_dst[bank][frame][n] = (int16_t)(buf[n] / peak * 30000.0f);
}
static void wt_additive(float *buf, const float *amp, int harmonics, int cosine)
{
    for (int n = 0; n < WT_SIZE; ++n) {
        float sum = 0, ph = (float)n / WT_SIZE;
        for (int h = 1; h <= harmonics; ++h)
            if (amp[h]) sum += amp[h] * pw_sin1(ph * (float)h + (cosine ? 0.25f : 0.0f));
        buf[n] = sum;
    }
}
PW_COLD static void build_wavetables(void)
{
    float buf[WT_SIZE], amp[65];
    /* Bank 0: BASIC, sine -> triangle -> saws -> square -> pulses */
    for (int f = 0; f < WT_FRAMES; ++f) {
        memset(amp, 0, sizeof amp);
        int cos_phase = 0;
        for (int h = 1; h <= 48; ++h) {
            float a = 0;
            switch (f) {
            case 0: a = h == 1; break;
            case 1: a = (h & 1) ? ((h / 2) & 1 ? -1.0f : 1.0f) / (float)(h * h) : 0; break;
            case 2: a = h <= 6 ? 1.0f / h : 0; break;
            case 3: a = 1.0f / h; break;
            case 4: a = (h & 1) ? 1.0f / h : 0; break;
            case 5: a = sin_slow(PW_PI * h * 0.25f) / h; cos_phase = 1; break;
            case 6: a = sin_slow(PW_PI * h * 0.10f) / h; cos_phase = 1; break;
            default: a = 1.0f / h + ((h & 1) ? 0 : 0.8f / h); break;
            }
            amp[h] = a;
        }
        wt_additive(buf, amp, 48, cos_phase);
        wt_store(0, f, buf);
    }
    /* Bank 1: VOCAL formant frames (A E I O U, then higher "choir" voices) */
    static const float formant[8][4] = {
        {800, 1150, 2900, 130}, {400, 1600, 2700, 130}, {270, 2300, 3000, 130},
        {450, 800, 2830, 130},  {325, 700, 2530, 130},  {800, 1150, 2900, 220},
        {450, 800, 2830, 220},  {270, 2300, 3000, 220}};
    for (int f = 0; f < WT_FRAMES; ++f) {
        memset(amp, 0, sizeof amp);
        float f0 = formant[f][3];
        for (int h = 1; h <= 60 && h * f0 < 8000; ++h) {
            float hz = h * f0;
            amp[h] = gauss((hz - formant[f][0]) / 110.0f) +
                     0.6f * gauss((hz - formant[f][1]) / 140.0f) +
                     0.3f * gauss((hz - formant[f][2]) / 200.0f) + 0.02f / h;
        }
        wt_additive(buf, amp, 60, 0);
        wt_store(1, f, buf);
    }
    /* Bank 2: DIGITAL — hashed harmonic patterns, then hard-sync sweeps */
    uint32_t seed = 0x5eed1234u;
    for (int f = 0; f < 4; ++f) {
        memset(amp, 0, sizeof amp);
        for (int h = 1; h <= 40; ++h) {
            uint32_t r = pw_rand_u(&seed);
            amp[h] = ((r >> 8) & 3) ? (float)((r >> 4) & 15) / 15.0f / (1.0f + h * 0.15f) : 0;
        }
        amp[1] = 1;
        wt_additive(buf, amp, 40, 0);
        wt_store(2, f, buf);
    }
    for (int f = 4; f < 8; ++f) {
        float ratio = 1.5f + (f - 4) * 1.1f;
        for (int n = 0; n < WT_SIZE; ++n) {
            float ph = (float)n / WT_SIZE;
            buf[n] = (pw_frac(ph * ratio) * 2.0f - 1.0f) * (1.0f - ph * 0.3f);
        }
        wt_store(2, f, buf);
    }
    /* Bank 3: FM — two-operator spectra with rising index */
    static const float fm[8][2] = {{1, 0.5f}, {1, 1.5f}, {1, 3}, {1, 6}, {2, 1}, {2, 3}, {3, 2}, {4, 4}};
    for (int f = 0; f < WT_FRAMES; ++f) {
        for (int n = 0; n < WT_SIZE; ++n) {
            float ph = (float)n / WT_SIZE;
            buf[n] = pw_sin1(ph + fm[f][1] / PW_TWO_PI * pw_sin1(ph * fm[f][0]));
        }
        wt_store(3, f, buf);
    }
}
PW_COLD void pw_modules_init(void)
{
    for (int i = 0; i <= PW_SINE_SIZE; ++i)
        pw_sine_table[i] = sin_slow(PW_TWO_PI * (float)i / PW_SINE_SIZE);
}
PW_COLD void pw_wt_build(pw_module *m)
{
    if (m->audio_len < WT_BYTES) return;
    wt_dst = (wt_tab *)(void *)(pw_pool + m->reg_off);
    build_wavetables();
}
PW_COLD static void wavetable_init(pw_module *m) { pw_wt_build(m); }

/* ------------------------------------------------------------- oscillators */
/* XFADE: A and B blended by MIX (a knob and a CV, 0-5 V over the whole range). EQUAL POWER keeps the loudness level through the middle;
 * LINEAR is for CV. */
typedef struct { float gm; } xf_st;
PROC_COLD(xfade_proc)
{
    ST(xf_st);
    (void)s;
    float mix = m->p[0], amt = m->p[2];
    int lin = m->p[1] > 0.5f;
    float ga = 0, gb = 0;
    for (int i = 0; i < PW_N; ++i) {
        if (i == 0 || IN_CONNECTED(2)) {
            float x = pw_clamp(mix + (IN_CONNECTED(2) ? in[2][i] * 0.2f * amt : 0.0f), 0.0f, 1.0f);
            if (lin) { ga = 1.0f - x; gb = x; } else { ga = pw_cos1(x * 0.25f); gb = pw_sin1(x * 0.25f); }
        }
        out[0][i] = in[0][i] * ga + in[1][i] * gb;
    }
}
/* SNAPSHOT: a CV picks the patch's snapshot. 0..5 V (or -5..5 V) is spread over SCENES snapshots; with a clock patched the CV is read only on its ticks (a sample & hold of its own), so noise through a clock is a
 * random snapshot on every beat and a slow LFO drifts through them. HYST keeps a CV sitting on a boundary from flicking between two. The module only decides which snapshot: the screen's loop
 * (snap_follow) recalls it, waiting for the beat or bar if CHANGE asks, and glides there over MORPH. */
typedef struct { int8_t cur; uint8_t clk_hi; uint16_t pulse; } snp_st;
static const char *const L_snrange[] = {"0-5V", "+-5V"};
static const char *const L_snchg[] = {"NOW", "BEAT", "BAR"};
PROC_COLD(snap_proc)
{
    ST(snp_st);
    int n = (int)(m->p[0] + 0.5f), bip = m->p[1] > 0.5f;
    if (n < 2) n = 2; else if (n > PW_VARS) n = PW_VARS;
    float h = m->p[2] * 0.5f;
    for (int i = 0; i < PW_N; ++i) {
        int tick = IN_CONNECTED(1) ? pw_rise(&s->clk_hi, in[1][i]) : 1;
        if (tick && IN_CONNECTED(0)) {
            float x = in[0][i] * 0.2f;
            if (bip) x = x * 0.5f + 0.5f;
            float u = pw_clamp(x, 0.0f, 0.9999f) * (float)n;
            if (s->cur < 0 || s->cur >= n) { s->cur = (int8_t)u; }
            else if (u >= (float)(s->cur + 1) + h || u < (float)s->cur - h) { s->cur = (int8_t)u; s->pulse = (uint16_t)(PW_SRF * 0.003f); }
        }
        if (s->pulse) --s->pulse;
        out[0][i] = s->cur < 0 ? 0.0f : 5.0f * (float)s->cur / (float)(n - 1);
        out[1][i] = s->pulse ? 5.0f : 0.0f;
    }
}
/* INERTIA: CV with mass. A weight on a spring: FOLLOW pulls it toward the CV (a spring to the target), PUSH takes the CV as a force
 * (a spring to the middle holds it), FLICK kicks it by how fast the CV moves. MASS, SPRING and FRICTION set the natural frequency
 * w0 = sqrt(k/m) (0.3-450 rad/s) and the damping ratio (0.02-2: under 1 it rings); FORCE sizes the IMPULSE kick (and PUSH and FLICK).
 * POS is the position, VEL and ACC are the speed and the acceleration in units of the swing (v / w0, a / w0^2), so they stay in range
 * whatever the settings. Semi-implicit Euler at the sample rate (stable for any setting here); position and speed are limited. */
typedef struct { float x, v, a, prev; uint8_t imp_hi, rst_hi; } ine_st;
PW_INLINE float ine_sqrt(float x) { return x > 1e-9f ? pw_exp2(0.5f * pw_log2(x)) : 0.0f; }
static const char *const L_inemode[] = {"FOLLOW", "PUSH", "FLICK"};
PW_INLINE void ine_coef(float mass, float fric, float spring, float *mm, float *k, float *c, float *w0)
{
    float lm = 4.3219f * mass, ls = 6.1439f * spring;   /* (log2 of the square roots of 400 ^ mass and of 5000 ^ spring) */
    *mm = 0.05f * pw_exp2(2.0f * lm);                   /* 0.05 .. 20 */
    *k = 2.0f * pw_exp2(2.0f * ls);                     /* 2 .. 10000 */
    *w0 = 6.3246f * pw_exp2(ls - lm);                   /* sqrt(k / m): 0.3 .. 450 rad/s */
    *c = 2.0f * (0.02f + 1.98f * fric) * 0.31623f * pw_exp2(ls + lm);   /* 2 zeta sqrt(k m) */
}
PROC_COLD(inertia_proc)
{
    ST(ine_st);
    int mode = (int)(m->p[4] + 0.5f), cvc = (int)((connected >> 3) & 15U);
    float mass = m->p[0], fric = m->p[1], spring = m->p[2], force = m->p[3], mix = m->p[5], mm, k, c, w0;
    ine_coef(mass, fric, spring, &mm, &k, &c, &w0);
    for (int i = 0; i < PW_N; ++i) {
        if (cvc) {   /* the four CV jacks move their knobs (5 V is the whole way) */
            float ms = pw_clamp(mass + (IN_CONNECTED(3) ? in[3][i] * 0.2f : 0.0f), 0.0f, 1.0f);
            float fr = pw_clamp(fric + (IN_CONNECTED(4) ? in[4][i] * 0.2f : 0.0f), 0.0f, 1.0f);
            float sp = pw_clamp(spring + (IN_CONNECTED(5) ? in[5][i] * 0.2f : 0.0f), 0.0f, 1.0f);
            force = pw_clamp(m->p[3] + (IN_CONNECTED(6) ? in[6][i] * 0.2f : 0.0f), 0.0f, 1.0f);
            ine_coef(ms, fr, sp, &mm, &k, &c, &w0);
        }
        float cv = IN_CONNECTED(0) ? in[0][i] : 0.0f;
        if (IN_CONNECTED(2) && pw_rise(&s->rst_hi, in[2][i])) { s->x = 0.0f; s->v = 0.0f; s->a = 0.0f; s->prev = cv; }
        if (IN_CONNECTED(1) && pw_rise(&s->imp_hi, in[1][i])) s->v += force * 10.0f * w0;
        float f;
        if (mode == 1) f = cv * force * 150.0f - k * s->x - c * s->v;
        else if (mode == 2) { s->v += (cv - s->prev) * force * 2.0f * w0; f = -k * s->x - c * s->v; }
        else f = k * (cv - s->x) - c * s->v;
        s->prev = cv;
        s->a = f / mm;
        s->v = pw_clamp(s->v + s->a * PW_INV_SR, -40.0f * w0, 40.0f * w0);
        s->x += s->v * PW_INV_SR;
        if (s->x > 12.0f) { s->x = 12.0f; if (s->v > 0) s->v = 0; } else if (s->x < -12.0f) { s->x = -12.0f; if (s->v < 0) s->v = 0; }
        out[0][i] = pw_clamp(cv + (s->x - cv) * mix, -10.0f, 10.0f);   /* MIX: how much of the CV is the weight's (0: the CV as it came) */
        out[1][i] = pw_clamp(s->v / w0, -10.0f, 10.0f);
        out[2][i] = pw_clamp(s->a / (w0 * w0), -10.0f, 10.0f);
    }
}
/* BURST: one trigger starts a rapid series of triggers. COUNT of them; SPEED is the rate in the middle of the burst; CURVE bends it, so the
 * gaps shrink (positive: a bouncing ball, a roll that tightens) or grow (negative: it slows to a stop), the whole burst changing by 8 times
 * at the ends. JITTER shakes each gap. A new trigger starts it again. The COUNT and SPEED jacks are read when it starts (COUNT: 2 hits a volt, SPEED: 1 V/oct).
 * TRIG is the series; LEVEL is a CV for the hits that falls from 5 V to nothing over the burst (feed a VCA for a ball's fading bounces);
 * PITCH is a 1 V/oct CV that moves STEP semitones on every hit (0 at the first), held after the last until the next burst. */
float pw_sync_sec(float param);
typedef struct { uint8_t hi, run; int idx, total; float wait, gm, lvl, pit; int pulse; } bst_st;
PW_INLINE float bst_gap(float gm, float curve, int j, int total)   /* the gap after hit j, in samples (no jitter) */
{
    float u = total > 1 ? ((float)j + 0.5f) / (float)(total - 1) : 0.5f;
    float g = gm * pw_exp2(-curve * 3.0f * (u - 0.5f));
    return g < 88.0f ? 88.0f : g;
}
PROC_COLD(burst_proc)
{
    ST(bst_st);
    float curve = m->p[2], jit = m->p[3], step = (float)((int)(m->p[4] + (m->p[4] < 0 ? -0.5f : 0.5f))) * (1.0f / 12.0f), sy = pw_sync_sec(m->p[5]);
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(0) && pw_rise(&s->hi, in[0][i])) {
            float cn = m->p[0] + (IN_CONNECTED(1) ? in[1][i] * 2.0f : 0.0f), sp = m->p[1] * (IN_CONNECTED(2) ? pw_exp2(pw_clamp(in[2][i], -6.0f, 6.0f)) : 1.0f);
            int cnt = (int)(cn + 0.5f);
            s->total = cnt < 1 ? 1 : cnt > 32 ? 32 : cnt;
            s->gm = PW_SRF / pw_clamp(sp, 0.5f, 400.0f);
            if (sy > 0.0f) s->gm = sy * PW_SRF / (IN_CONNECTED(2) ? pw_exp2(pw_clamp(in[2][i], -6.0f, 6.0f)) : 1.0f);   /* SYNC: the middle gap is the note value */
            s->idx = 0; s->wait = 0.0f; s->run = 1;
        }
        if (s->run) {
            s->wait -= 1.0f;
            if (s->wait <= 0.0f) {   /* a hit */
                float gap = 0.0f;
                s->lvl = 5.0f * (float)(s->total - s->idx) / (float)s->total;
                s->pit = step * (float)s->idx;
                if (s->idx + 1 < s->total) {
                    gap = bst_gap(s->gm, curve, s->idx, s->total) * (1.0f + jit * 0.8f * pw_rand_f(&pws.rng));
                    if (gap < 88.0f) gap = 88.0f;
                }
                s->pulse = (int)(gap > 0.0f && gap * 0.5f < PW_SRF * 0.003f ? gap * 0.5f : PW_SRF * 0.003f);
                s->wait += gap;
                if (++s->idx >= s->total) s->run = 0;
            }
        }
        out[0][i] = s->pulse > 0 ? 5.0f : 0.0f;
        if (s->pulse > 0) --s->pulse;
        else if (!s->run) s->lvl = 0.0f;
        out[1][i] = s->lvl;
        out[2][i] = s->pit;
    }
}
/* the screen's picture: where the hits fall (0..1 of the burst's length), returns how many; *len is the length in seconds */
int pw_burst_hits(const float *p, float *t, float *len)
{
    int total = (int)(p[0] + 0.5f); if (total < 1) total = 1; else if (total > 32) total = 32;
    float gm = PW_SRF / pw_clamp(p[1], 0.5f, 400.0f), at = 0.0f, sy = pw_sync_sec(p[5]);
    if (sy > 0.0f) gm = sy * PW_SRF;
    for (int j = 0; j < total; ++j) { t[j] = at; if (j + 1 < total) at += bst_gap(gm, p[2], j, total); }
    for (int j = 0; j < total; ++j) t[j] = at > 0.0f ? t[j] / at : 0.0f;
    *len = at / PW_SRF;
    return total;
}
/* the screen's picture: how the weight answers a step of the CV (FOLLOW, PUSH) or a kick (FLICK), n points, 0..1 of the swing; *t is the time shown */
PW_COLD void pw_inertia_curve(const float *p, float *y, int n, float *t)
{
    float mm, k, c, w0;
    ine_coef(p[0], p[1], p[2], &mm, &k, &c, &w0);
    float z = 0.02f + 1.98f * p[1], a = z * w0, T;
    (void)c; (void)k; (void)mm;
    int kick = (int)(p[4] + 0.5f) == 2;
    if (z < 1.0f) T = 4.5f / a; else T = 4.5f / (w0 * (z - ine_sqrt(z * z - 1.0f)));
    if (z > 0.98f && z < 1.02f) z = 1.0f;
    T = pw_clamp(T, 0.05f, 20.0f);
    *t = T;
    float wd = z < 1.0f ? w0 * ine_sqrt(1.0f - z * z) : 0.0f, r = z > 1.0f ? w0 * ine_sqrt(z * z - 1.0f) : 0.0f;
    for (int i = 0; i < n; ++i) {
        float x = T * (float)i / (float)(n - 1), e;
        if (z < 1.0f) {
            e = pw_exp2(-a * x * 1.442695f);
            float ph = wd * x * (1.0f / 6.2831853f);
            y[i] = kick ? w0 / wd * e * pw_sin1(ph) : 1.0f - e * (pw_cos1(ph) + z / ine_sqrt(1.0f - z * z) * pw_sin1(ph));
        } else if (z < 1.001f && z > 0.999f) {
            e = pw_exp2(-w0 * x * 1.442695f);
            y[i] = kick ? w0 * x * e : 1.0f - e * (1.0f + w0 * x);
        } else {
            float e1 = pw_exp2((-a + r) * x * 1.442695f), e2 = pw_exp2((-a - r) * x * 1.442695f);
            y[i] = kick ? w0 * (e1 - e2) / (2.0f * r) : 1.0f - ((-a - r) * e1 - (-a + r) * e2) / (-2.0f * r);
        }
    }
}
/* The oscillator's shapes. DRIFT (0..1) is what drifts: the pitch (two slow random walks, a few cents), a 1 V/oct input that tracks a
 * little wide and the pulse width, which breathes. ANALOG (0..1) is what the four waves look like on a scope: the saw's ramp is bowed (a capacitor charging) and its
 * reset rounded, the triangle and sine rounded off at their peaks (a diode shaper), the square's top and bottom tilt (an AC-coupled
 * output) and its corners round, overshoot and ring. The pitch-relative filter keeps the picture the same on every note. 0 is the
 * exact maths. FM MODE: EXP is 1 V/oct pitch FM; LIN is linear FM (the oscillator's frequency times 1 + FM x AMT, never below 0);
 * TZ is through-zero FM: that factor may go negative and the phase runs backwards, so the pitch holds as the modulator swings. */
static const char *const L_fmmode[] = {"EXP", "LIN", "TZ"};
typedef struct { float ph; float dr, dr2; uint8_t sync_hi; float hp, sw[2], sq[2]; } vco_st;
/* one sample of the four shapes (sin, tri, saw, sq) at phase ph, step adt (>= 0), pulse width pw; mask: which of them
 * are wanted (bit 0 sin .. bit 3 sq: the rest are not worked out, the module's unpatched outputs) */
static void vco_shape(vco_st *s, float ph, float adt, float pw, float an, float *o, unsigned mask)
{
    float saw = 0, sq = 0, tri = 0, sn = 0;
    float b0 = (mask & 12u) ? pw_blep(ph, adt) : 0.0f;
    if (mask & 4u) saw = 2.0f * ph - 1.0f - b0;
    if (mask & 8u) { float t2 = ph - pw; if (t2 < 0) t2 += 1.0f; sq = (ph < pw ? 1.0f : -1.0f) + b0 - pw_blep(t2, adt); }
    if (mask & 2u) tri = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
    if (mask & 1u) sn = pw_sin1(ph);
    if (an > 0.0f) {
        float rnd = 0.3f * an;
        if (mask & 4u) saw += 0.68f * an * (ph * (1.0f - ph) - 0.16667f);
        if (mask & 2u) tri = (tri - rnd * tri * tri * tri) / (1.0f - rnd);
        if (mask & 1u) sn -= 0.22f * an * sn * sn * sn;
        float g = pw_clamp(adt * (80.0f - 40.0f * an), 0.0f, 0.35f), d = 2.0f - 1.0f * an;
        float wet = pw_minf(an * 4.0f, 1.0f);
        if (mask & 8u) { s->hp += (sq - s->hp) * an * 0.35f * adt; sq -= s->hp; }   /* the tilt: the top sags, the bottom lifts */
        float *v[2] = {&saw, &sq}, *st[2] = {s->sw, s->sq};
        for (int k = 0; k < 2; ++k) {                             /* the corners: a damped low-pass, a little under-damped */
            if (!(mask & (k ? 8u : 4u))) continue;
            float x = *v[k], lp = st[k][0], bp = st[k][1];
            lp += g * bp;
            float hi = x - lp - d * bp;
            bp += g * hi;
            st[k][0] = lp; st[k][1] = bp;
            *v[k] = x + wet * (lp - x);
        }
    }
    o[0] = sn; o[1] = tri; o[2] = saw; o[3] = sq;
}
/* one cycle of the shapes for the screen (n points of output k, -1..1), the same code the oscillator runs */
void pw_vco_wave(const float *p, int k, float *y, int n)
{
    vco_st s; float o[4], adt = 1.0f / 128.0f;
    memset(&s, 0, sizeof s);
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 128; ++i) {
            vco_shape(&s, (float)i * adt, adt, p[2], p[6], o, 15u);
            if (c == 3) { int j = i * (n - 1) / 127; if (i == 0 || j != (i - 1) * (n - 1) / 127) y[j] = o[k]; }
        }
    vco_shape(&s, 0.0f, adt, p[2], p[6], o, 15u);
    y[n - 1] = o[k];
}
PROC(vco_proc)
{
    ST(vco_st);
    float base = m->p[0] / 12.0f + m->p[1] / 1200.0f, pw0 = m->p[2], fmamt = m->p[3], dr = m->p[4], an = m->p[6];
    int fmode = (int)(m->p[5] + 0.5f);
    int per_sample = (connected & 3U) != 0;
    unsigned mask = m->omask & 15u;
    float drift = 0.0f, track = 1.0f + 0.012f * dr, pwd = 0.0f;
    if (dr > 0.0f) {   /* DRIFT: the pitch wanders (about 20 cents), the octaves are a little off, the pulse width breathes */
        s->dr += (pw_rand_f(&pws.rng) - s->dr) * 0.004f;
        s->dr2 += (pw_rand_f(&pws.rng) - s->dr2) * 0.0004f;
        drift = dr * (0.35f * s->dr + 1.4f * s->dr2);
        pwd = dr * 1.5f * s->dr;
    }
    float tb = base + drift;
    float dt = pw_clamp(pw_pitch_hz(tb) * PW_INV_SR, 0.0f, 0.45f), ph = s->ph;
    float pw_c = pw_clamp(pw0 + pwd, 0.03f, 0.97f);
    float last_a = 1e30f, last_b = 1e30f;   /* (the pitch is worked out again only when an input moved: a sequencer's note holds for many blocks) */
    for (int i = 0; i < PW_N; ++i) {
        if (per_sample && (in[0][i] != last_a || in[1][i] != last_b)) {
            last_a = in[0][i]; last_b = in[1][i];
            if (fmode == 0) dt = pw_clamp(pw_pitch_hz(tb + last_a * track + last_b * fmamt) * PW_INV_SR, 0.0f, 0.45f);
            else {
                float k = 1.0f + last_b * fmamt;
                if (fmode == 1 && k < 0.0f) k = 0.0f;
                dt = pw_clamp(pw_pitch_hz(tb + last_a * track) * PW_INV_SR * k, -0.45f, 0.45f);
            }
        }
        if (IN_CONNECTED(3) && pw_rise(&s->sync_hi, in[3][i])) ph = 0;
        float pw = IN_CONNECTED(2) ? pw_clamp(pw0 + pwd + in[2][i] * 0.09f, 0.03f, 0.97f) : pw_c, o[4];
        vco_shape(s, ph, dt < 0.0f ? -dt : dt, pw, an, o, mask);   /* (the band-limiting depends on where the phase is, not which way it runs) */
        out[0][i] = 5.0f * o[0];
        out[1][i] = 5.0f * o[1];
        out[2][i] = 5.0f * o[2];
        out[3][i] = 5.0f * o[3];
        ph += dt; if (ph >= 1.0f) ph -= 1.0f; else if (ph < 0.0f) ph += 1.0f;
    }
    s->ph = ph;
}

/* Tempo sync: SYNC picks a note value of the transport's tempo (OFF: the module's own rate or time). */
static const char *const L_sync[] = {"OFF", "4 BAR", "2 BAR", "1 BAR", "1/2", "1/4", "1/8", "1/16", "1/32", "1/2T", "1/4T", "1/8T", "1/16T", "1/2.", "1/4.", "1/8.", "1/16."};
#define SYNC_N 17
static const float sync_beat[SYNC_N] PW_DSP_DATA = {0, 16, 8, 4, 2, 1, 0.5f, 0.25f, 0.125f, 4.0f / 3, 2.0f / 3, 1.0f / 3, 1.0f / 6, 3, 1.5f, 0.75f, 0.375f};
PW_INLINE float sync_sec(float param)   /* seconds of the note value (0: no sync) */
{
    int k = (int)(param + 0.5f);
    return k <= 0 || k >= SYNC_N ? 0.0f : sync_beat[k] * 60.0f / pw_maxf(pws.bpm, 20.0f);
}
PW_INLINE float sync_hz(float param) { float t = sync_sec(param); return t > 0 ? 1.0f / t : 0.0f; }
float pw_sync_sec(float param) { return sync_sec(param); }   /* for the screens */

typedef struct { float ph; uint8_t rst_hi; } lfo_st;
PROC_COLD(lfo_proc)
{
    ST(lfo_st);
    float rate = sync_hz(m->p[3]), level = m->p[1] * 5.0f, ph = s->ph;
    if (rate <= 0) rate = m->p[0];
    int uni = m->p[2] > 0.5f;
    float dt = rate * PW_INV_SR;
    /* CONTROL RATE (a setting): a slow LFO with no rate input is worked out every fourth sample and held between (11 kHz steps of a
     * signal that moves a few times a second); a reset pulse shorter than four samples can then be missed */
    int k = pws.ctl_rate && !IN_CONNECTED(0) && rate < 60.0f ? 4 : 1;
    for (int i = 0; i < PW_N; i += k) {
        if (IN_CONNECTED(0)) dt = rate * pw_exp2(in[0][i]) * PW_INV_SR;
        if (IN_CONNECTED(1) && pw_rise(&s->rst_hi, in[1][i])) ph = 0;
        float v[4] = { pw_sin1(ph), ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph,
                       2.0f * ph - 1.0f, ph < 0.5f ? 1.0f : -1.0f };
        float lv = IN_CONNECTED(2) ? level * pw_clamp(in[2][i] * 0.2f, 0.0f, 1.0f) : level;
        for (int j = 0; j < 4; ++j) {
            float y = (uni ? (v[j] + 1.0f) * 0.5f : v[j]) * lv;
            out[j][i] = y;
            for (int q = 1; q < k; ++q) out[j][i + q] = y;
        }
        ph += dt * (float)k; if (ph >= 1.0f) ph -= (float)pw_floori(ph);
    }
    s->ph = ph;
}

typedef struct { uint32_t seed; float b[7], brown; float lp[4]; float held; uint32_t hold; uint32_t dseed; uint16_t dgate;
                uint32_t clk, et[12]; int8_t ea[12]; uint8_t eh, ec; } noise_st;   /* (the last dust events, for the visualization) */
static void noise_init(pw_module *m)
{
    ST(noise_st);
    s->seed = 0x1234567u + (uint32_t)(m - pws.mod) * 2654435761u * 2654435761u;
    s->dseed = 0x5eed5eedu ^ s->seed;
}
PROC(noise_proc)
{
    ST(noise_st);
    float level = m->p[0] * 5.0f, tone = m->p[1];
    /* TONE: below the middle a low pass darkens all four noises, above it a high pass thins them */
    float a = tone < 0 ? pw_lp_coef_hz(18000.0f * pw_exp2(tone * 6.64f)) : pw_lp_coef_hz(25.0f * pw_exp2(tone * 8.97f));
    int filt = tone < -0.01f || tone > 0.01f;
    float dens = pw_clamp(m->p[2] * PW_INV_SR, 0.0f, 0.5f);   /* DUST: random impulses, this many a second */
    uint32_t thr = (uint32_t)(dens * 4294967040.0f);
    (void)in; (void)connected;
    for (int i = 0; i < PW_N; ++i) {
        float w = pw_rand_f(&s->seed);
        uint32_t r = pw_rand_u(&s->dseed);
        ++s->clk;
        if (r < thr) {
            float am = pw_rand_f(&s->dseed);
            out[4][i] = am * level; s->dgate = 44;
            s->et[s->eh] = s->clk; s->ea[s->eh] = (int8_t)(am * 127.0f); s->eh = (uint8_t)((s->eh + 1u) % 12u); if (s->ec < 12) ++s->ec;
        }
        else out[4][i] = 0.0f;
        out[5][i] = s->dgate ? 5.0f : 0.0f;
        if (s->dgate) --s->dgate;
        float *b = s->b;
        b[0] = 0.99886f * b[0] + w * 0.0555179f; b[1] = 0.99332f * b[1] + w * 0.0750759f;
        b[2] = 0.96900f * b[2] + w * 0.1538520f; b[3] = 0.86650f * b[3] + w * 0.3104856f;
        b[4] = 0.55000f * b[4] + w * 0.5329522f; b[5] = -0.7616f * b[5] - w * 0.0168980f;
        float pink = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f) * 0.2f;
        b[6] = w * 0.115926f;
        s->brown = (s->brown + 0.02f * w) * (1.0f / 1.02f);
        /* DIGITAL: a new 4-bit value every third sample, so it crackles and aliases like a cheap converter */
        if (++s->hold >= 3u) { s->hold = 0; s->held = (float)pw_floori(w * 8.0f) * 0.125f + 0.0625f; }
        float v[4] = {w, pink, s->brown * 3.5f, s->held};
        for (int k = 0; k < 4; ++k) {
            float x = v[k];
            if (filt) { s->lp[k] += a * (x - s->lp[k]); x = tone < 0 ? s->lp[k] : x - s->lp[k]; }
            out[k][i] = x * level;
        }
    }
    for (int k = 0; k < 4; ++k) if (s->lp[k] < 1e-20f && s->lp[k] > -1e-20f) s->lp[k] = 0;
}

typedef struct { float ph; } wt_st;
PROC(wavetable_proc)
{
    ST(wt_st);
    const wt_tab *tab = (const wt_tab *)(const void *)(pw_pool + m->reg_off);   /* (voice 0's region: the tables are shared by the voices) */
    if (m->status != PW_STATUS_OK || m->audio_len < WT_BYTES) { for (int i = 0; i < PW_N; ++i) out[0][i] = 0; return; }   /* (no room for the tables: silent) */
    float base = m->p[0] / 12.0f + m->p[1] / 1200.0f;
    int bank = (int)(m->p[2] + 0.5f) & 3;
    float pos0 = m->p[3], ph = s->ph;
    int per_sample = (connected & 5U) != 0;
    float dt = pw_pitch_hz(base) * PW_INV_SR;
    for (int i = 0; i < PW_N; ++i) {
        if (per_sample) dt = pw_pitch_hz(base + in[0][i] + in[2][i]) * PW_INV_SR;
        float pos = pw_clamp(pos0 + in[1][i] * 0.2f, 0.0f, 1.0f) * (WT_FRAMES - 1);
        int f0 = (int)pos; if (f0 > WT_FRAMES - 2) f0 = WT_FRAMES - 2;
        float ff = pos - (float)f0;
        float x = ph * WT_SIZE;
        int i0 = (int)x, i1 = (i0 + 1) & (WT_SIZE - 1);
        float fx = x - (float)i0;
        const int16_t *a = tab[bank][f0], *b = tab[bank][f0 + 1];
        float va = a[i0] + (a[i1] - a[i0]) * fx, vb = b[i0] + (b[i1] - b[i0]) * fx;
        out[0][i] = (va + (vb - va) * ff) * (5.0f / 30000.0f);
        ph += pw_clamp(dt, 0.0f, 0.45f); if (ph >= 1.0f) ph -= 1.0f;
    }
    s->ph = ph;
}

typedef struct { float pm, pc; } cx_st;
PROC(complex_proc)
{
    ST(cx_st);
    float base = m->p[0] / 12.0f, ratio = m->p[1], index0 = m->p[2], timbre0 = m->p[3];
    float f = pw_pitch_hz(base) * PW_INV_SR;
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(0)) f = pw_pitch_hz(base + in[0][i]) * PW_INV_SR;
        float timbre = pw_clamp(timbre0 + in[1][i] * 0.2f, 0.0f, 1.0f);
        float index = pw_clamp(index0 + in[2][i] * 2.0f, 0.0f, 20.0f);
        float mod = pw_sin1(s->pm);
        float car = pw_sin1(s->pc + index * mod * (1.0f / PW_TWO_PI));
        float main = pw_sin1(car * (1.0f + timbre * 5.0f) * 0.25f);
        out[0][i] = 5.0f * mod;
        out[1][i] = 5.0f * main;
        out[2][i] = 2.5f * (mod + main);
        s->pm += pw_clamp(f * ratio, 0.0f, 0.45f); if (s->pm >= 1.0f) s->pm -= 1.0f;
        s->pc += pw_clamp(f, 0.0f, 0.45f); if (s->pc >= 1.0f) s->pc -= 1.0f;
    }
}

/* -------------------------------------------------------- filters and VCA */
/* VCF: six filter circuits, one TYPE knob. Outputs LP, BP, HP and NOTCH from each (the four-pole ones by
 * mixing their stages, as the Oberheim Xpander did).
 *   SEM    12 dB state-variable (Oberheim SEM): smooth, never quite self-oscillates
 *   MOOG   24 dB transistor ladder (zero-delay feedback, saturating input): fat, bass thins with resonance
 *   303    the TB-303's diode ladder: coupled stages, ~18 dB, a high-pass in its feedback: squelch
 *   JUNO   24 dB cascade of four OTA stages (Jupiter-8, Juno-60, IR3109): clean, singing resonance
 *   JP-6   12 dB multimode state-variable (Jupiter-6): resonance up to self-oscillation, soft-limited
 *   MS-20  12 dB Sallen-Key (Korg35): the resonance through a diode clipper, screams and growls */
enum { F_SEM, F_MOOG, F_303, F_JUNO, F_JP6, F_MS20 };
typedef struct { float s[4], hp, fb, sat, w303, a303, k303; } vcf_st;
PW_INLINE float diode_clip(float x) { return x > 0 ? pw_tanh(x * 1.2f) : pw_tanh(x * 0.9f) * 1.1f; }
static float vcf_g(float fc) { return pw_tan_pi(pw_clamp(fc, 20.0f, 18500.0f) * PW_INV_SR); }
PROC(vcf_proc)
{
    ST(vcf_st);
    int type = (int)pw_clamp(m->p[4] + 0.5f, 0.0f, 5.0f);
    float cutoff = m->p[0], res0 = m->p[1], amt = m->p[2];
    float drive = 1.0f + m->p[3] * 9.0f, makeup = 5.0f / (1.0f + m->p[3] * 2.0f);
    int per = (connected & 14U) != 0;   /* FREQ, RES or KEY patched */
    float keytrk = m->p[5];
    float res = res0, g = vcf_g(cutoff);
    float *st = s->s;
    for (int i = 0; i < PW_N; ++i) {
        if (per && (i & 3) == 0) {
            g = vcf_g(cutoff * pw_exp2(in[1][i] * amt + in[3][i] * keytrk));
            res = pw_clamp(res0 + in[2][i] * 0.2f, 0.0f, 1.0f);
        }
        float x = in[0][i] * 0.2f * drive, lp, bp, hp, no;
        switch (type) {
        default: case F_SEM: case F_JP6: case F_MS20: {
            float k = type == F_SEM ? 2.0f - 1.9f * res : type == F_JP6 ? 2.0f - 2.04f * res : 2.0f - 2.08f * res;
            if (type == F_MS20) k += 0.35f * pw_absf(diode_clip(st[0] * 0.7f)) * res;   /* the clipper limits it */
            if (type == F_JP6) k += 0.4f * s->sat * s->sat;    /* damping that grows with the swing: a steady sine */
            float xin = type == F_SEM ? pw_tanh(x) : type == F_MS20 ? diode_clip(x * 1.3f) : pw_tanh(x * 0.8f);
            float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            float v3 = xin - st[1], v1 = a1 * st[0] + a2 * v3, v2 = st[1] + a2 * st[0] + a3 * v3;
            st[0] = 2.0f * v1 - st[0]; st[1] = 2.0f * v2 - st[1];
            if (type == F_MS20) st[0] = 2.5f * pw_tanh(st[0] * 0.4f);                 /* self-oscillation: bounded */
            else if (type == F_JP6) st[0] = pw_clamp(st[0], -6.0f, 6.0f);
            lp = v2; bp = v1; hp = xin - k * v1 - v2; no = lp + hp;
            s->sat = v1;
            if (type == F_MS20) { lp = diode_clip(lp * 1.4f) * 0.45f; bp = diode_clip(bp * 1.4f) * 0.45f; hp *= 0.64f; no *= 0.64f; }
        } break;
        case F_MOOG: {
            float G = g / (1.0f + g), b = 1.0f / (1.0f + g), k = 4.3f * res;
            float S = G * G * G * b * st[0] + G * G * b * st[1] + G * b * st[2] + b * st[3];
            float G4 = G * G * G * G, y4 = (G4 * x + S) / (1.0f + k * G4);
            float u = pw_tanh(x - k * y4), y[4], u0 = u;
            for (int n = 0; n < 4; ++n) { float v = (u - st[n]) * G; y[n] = v + st[n]; st[n] = y[n] + v; u = y[n]; }
            float c = 1.0f + res * 1.3f;
            lp = y[3] * c; bp = 4.0f * (y[1] - 2.0f * y[2] + y[3]) * c;
            hp = u0 - 4.0f * y[0] + 6.0f * y[1] - 4.0f * y[2] + y[3]; no = lp + hp;
        } break;
        case F_JUNO: {
            /* four OTA stages (IR3109): the feedback resolved without a delay (as the ladder's), each stage's
             * input saturating; a clean, singing resonance that self-oscillates at the top */
            float G = g / (1.0f + g), b = 1.0f / (1.0f + g), k = 4.06f * res;
            float S = G * G * G * b * st[0] + G * G * b * st[1] + G * b * st[2] + b * st[3];
            float G4 = G * G * G * G, xin = x * (1.0f + 0.6f * res);
            float y4 = (G4 * xin + S) / (1.0f + k * G4);
            float u = xin - k * y4, u0 = u, y[4];
            for (int n = 0; n < 4; ++n) {
                float v = (1.6f * pw_tanh(u * 0.625f) - st[n]) * G;
                y[n] = v + st[n]; st[n] = y[n] + v; u = y[n];
            }
            lp = u * (1.0f + res); bp = 4.0f * (y[1] - 2.0f * y[2] + y[3]);
            hp = u0 - 4.0f * y[0] + 6.0f * y[1] - 4.0f * y[2] + y[3]; no = lp + hp;
        } break;
        case F_303: {
            /* The TB-303's diode ladder: four capacitors joined by diode pairs, so each stage is loaded by its
             * neighbours (dy1 = 2w(u - y1 + y2), dy2 = w(y1 - 2y2 + y3), dy3 = w(y2 - 2y3 + y4), dy4 = w(y3 - 2y4));
             * it peaks at sqrt(2) w and needs a loop gain of 17 to ring (a transistor ladder: 4). Twice
             * oversampled, each stage solved implicitly in itself; d303 holds w and the k that just rings at it.
             * The feedback goes through a ~60 Hz high-pass, as the 303's: the bass stays while the resonance
             * squelches. */
            if (per ? (i & 3) == 0 : i == 0) {
                float fc = pw_clamp(per ? cutoff * pw_exp2(in[1][i] * amt) : cutoff, 20.0f, 18000.0f);
                float w = fc * (6.2831853f / 1.4142136f / (2.0f * PW_SRF));
                s->w303 = w; s->a303 = 1.0f / (1.0f + 2.0f * w);
                float r = res, skew = (1.0f - pw_exp2(-4.33f * r)) * 1.0524f;   /* (1 - e^-3r) / (1 - e^-3) */
                s->k303 = skew * 17.0f * (1.0f + 2.0f * w + w * w) * 1.08f;
            }
            float w = s->w303, a = s->a303, k = s->k303, w2 = 2.0f * w;
            for (int os = 0; os < 2; ++os) {
                float fbh = st[3] - s->hp; s->hp += 0.0043f * fbh;
                float u = pw_tanh(x * 2.0f - k * fbh);
                st[0] = (st[0] + w2 * (u + st[1])) * a;
                st[1] = (st[1] + w * (st[0] + st[2])) * a;
                st[2] = (st[2] + w * (st[1] + st[3])) * a;
                st[3] = (st[3] + w * st[2]) * a;
            }
            float c = 1.0f + k * 0.35f;
            lp = st[3] * c * 1.6f; bp = (st[1] - st[3]) * c * 1.4f; hp = x - st[0] * 1.2f; no = lp + hp;
        } break;
        }
        out[0][i] = lp * makeup;
        out[1][i] = bp * makeup;
        out[2][i] = hp * makeup;
        out[3][i] = no * makeup;
    }
    /* denormals, and a recovery from any blow-up */
    for (int n = 0; n < 4; ++n) if (!(st[n] == st[n]) || pw_absf(st[n]) > 1e4f || pw_absf(st[n]) < 1e-20f) st[n] = 0;
    if (!(s->fb == s->fb) || pw_absf(s->fb) < 1e-20f) s->fb = 0;
    if (!(s->hp == s->hp) || pw_absf(s->hp) < 1e-20f) s->hp = 0;
}

typedef struct { float g; } vca_st;
PROC(vca_proc)
{
    ST(vca_st);
    float gain = m->p[0];
    int expo = m->p[1] > 0.5f;
    if (IN_CONNECTED(1)) {
        for (int i = 0; i < PW_N; ++i) {
            float c = pw_clamp(in[1][i] * 0.2f, 0.0f, 1.5f);
            if (expo) c = c * c * c;
            out[0][i] = in[0][i] * c * gain;
        }
        s->g = gain;
    } else {
        float g = s->g, step = (gain - g) * (1.0f / PW_N);
        for (int i = 0; i < PW_N; ++i) { g += step; out[0][i] = in[0][i] * (expo ? g * g * g : g); }
        s->g = gain;
    }
}

/* LPG: a vactrol low pass gate. The control (OFFSET + CV) and the strikes (PING) light a lamp; a photocell follows it,
 * and the cell sets both how bright (the gain) and how open (the cutoff) the sound is.
 *  The cell is two things at once: a quick part (the first bounce of a strike, ATTACK in, RESP out) and a slow part that
 *  rises more slowly and lets go more slowly the more it has been lit (MEMORY): a short strike rings out quickly, a long
 *  one lingers. PING is a pulse of the lamp (STRIKE, scaled by how hard the pulse is), held for as long as the input
 *  stays high. SLOPE 6 dB is the plain one-pole gate; 12 dB adds RES. VAC puts the cell's brightness out as a CV. */
typedef struct { float vf, vs, ic1, ic2, lp, hold, hit; uint8_t ping_hi; } lpg_st;
PROC(lpg_proc)
{
    ST(lpg_st);
    float offset = m->p[0], resp = m->p[1];
    int mode = (int)(m->p[2] + 0.5f), two = m->p[8] > 0.5f;
    float k = 2.0f - 1.6f * m->p[3];
    float strike = m->p[6], cvamt = m->p[9] * 0.2f, drive = 1.0f + 3.0f * m->p[7], mem = m->p[5];
    float kaf = 1.0f - pw_coef(m->p[4]), kas = 1.0f - pw_coef(m->p[4] * 6.0f + 0.012f);
    float krf = 1.0f - pw_coef(resp * 0.2f);
    float krs = 1.0f - pw_coef(resp * (0.6f + 4.0f * mem * s->vs));
    float minp = pw_maxf(0.002f, 5.0f * m->p[4]) * PW_SR;   /* (a strike lasts long enough for the cell to take it) */
    for (int i = 0; i < PW_N; ++i) {
        float c = pw_clamp(offset + in[1][i] * cvamt, 0.0f, 1.0f);
        if (IN_CONNECTED(2)) {
            if (pw_rise(&s->ping_hi, in[2][i])) { s->hit = strike * pw_clamp(in[2][i] * 0.2f, 0.3f, 1.0f); s->hold = minp; if (s->vs < s->hit * 0.9f) s->vs = s->hit * 0.9f; }
            else if (s->hold > 0) s->hold -= 1.0f;
        } else s->hit = s->hold = 0;
        float d = (s->ping_hi || s->hold > 0) && IN_CONNECTED(2) ? pw_maxf(c, s->hit) : c;
        s->vf += (d - s->vf) * (d > s->vf ? kaf : krf);
        s->vs += (d - s->vs) * (d > s->vs ? kas : krs);
        float v = pw_clamp(0.6f * s->vf + 0.4f * s->vs, 0.0f, 1.0f), x = pw_tanh(in[0][i] * 0.2f * drive) / pw_tanh(drive), y = x;
        if (mode != 0) {
            float g = pw_tan_pi(pw_clamp(40.0f * pw_exp2(v * 8.8f), 20.0f, 19000.0f) * PW_INV_SR);
            if (two) {
                float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
                float v3 = x - s->ic2, v1 = a1 * s->ic1 + a2 * v3, v2 = s->ic2 + a2 * s->ic1 + a3 * v3;
                s->ic1 = 2.0f * v1 - s->ic1; s->ic2 = 2.0f * v2 - s->ic2;
                y = v2;
            } else {   /* the plain one-pole RC */
                float vv = (x - s->lp) * g / (1.0f + g);
                y = vv + s->lp; s->lp = y + vv;
            }
        }
        if (mode != 1) y *= v * (0.4f + 0.6f * v);
        out[0][i] = 5.0f * y;
        out[1][i] = 5.0f * v;
    }
    if (pw_absf(s->ic1) < 1e-20f) s->ic1 = 0;
    if (pw_absf(s->ic2) < 1e-20f) s->ic2 = 0;
    if (pw_absf(s->lp) < 1e-20f) s->lp = 0;
}

/* ---------------------------------------------------------------- effects */
/* WAVEFOLDER: four shapes.
 *  FOLD   a triangle fold (the signal bounces between +-1)
 *  SINE   a sine of the driven signal
 *  CHEBY  a Chebyshev waveshaper: FOLD is the gain into it, ORDER which polynomial T(n) (between two whole orders it
 *         blends them): order 1 is the input, 2 doubles its frequency, higher orders add upper harmonics
 *  259    after the Buchla 259 (Esqueda, Pontynen, Valimaki, Parker: "Virtual Analog Buchla 259 Wavefolder", DAFx-2017):
 *         five folding stages and the direct path, summed, then the output's one-pole low pass (TONE, 1.33 kHz as built).
 *         The input is in volts there (FOLD drives it). Not oversampled: the 1.33 kHz low pass hides most of what would alias. */
typedef struct { float x1, y1, px, py; } fold_st;
static const float f259_th[5] = {0.6000f, 2.9940f, 5.4600f, 1.8000f, 4.0800f};
static const float f259_a[5]  = {0.8333f, 0.3768f, 0.2829f, 0.5743f, 0.2673f};
static const float f259_b[5]  = {0.5000f, 1.1281f, 1.5446f, 1.0338f, 1.0907f};
static const float f259_w[5]  = {-12.000f, -27.777f, -21.428f, 17.647f, 36.363f};   /* the output summers' weights */
#define F259_DIRECT 5.0f
#define F259_NORM   0.26f   /* the model swings about +-3.4 V over its useful input: that is about 0.9 of full scale here */
PROC(folder_proc)
{
    ST(fold_st);
    (void)connected;
    float fold = m->p[0], bias = m->p[1], mix = m->p[3];
    int shape = (int)(m->p[2] + 0.5f);
    float order = pw_clamp(m->p[4], 1.0f, 16.0f);
    int on = (int)order; float of = order - (float)on;
    float wc = PW_TWO_PI * pw_clamp(m->p[5], 200.0f, 16000.0f) * PW_INV_SR;
    float lb = wc / (2.0f + wc), la = (wc - 2.0f) / (wc + 2.0f);
    for (int i = 0; i < PW_N; ++i) {
        float x = in[0][i] * 0.2f;
        float drive = pw_clamp(fold + in[1][i] * 2.0f, 1.0f, 24.0f), y;
        if (shape == 3) {   /* 259 */
            float v = in[0][i] * 0.4f * drive + bias * 3.0f, a = pw_absf(v), sg = v < 0 ? -1.0f : 1.0f;
            float o = F259_DIRECT * v;
            for (int k = 0; k < 5; ++k) if (a > f259_th[k]) o += f259_w[k] * (f259_a[k] * v - f259_b[k] * sg);
            o *= F259_NORM;
            y = lb * (o + s->px) - la * s->py;     /* the one-pole low pass (bilinear) */
            s->px = o; s->py = y;
        } else if (shape == 2) {   /* CHEBY */
            float t = pw_clamp(x * drive + bias, -1.0f, 1.0f), t0 = 1.0f, t1 = t, ya = t, yb = t;   /* T(0), T(1) */
            for (int k = 2; k <= on + 1; ++k) {   /* T(k) = 2 t T(k-1) - T(k-2) */
                float tk = 2.0f * t * t1 - t0;
                t0 = t1; t1 = tk;
                if (k == on) ya = tk;
                if (k == on + 1) yb = tk;
            }
            y = ya + (yb - ya) * of;
        } else {
            float t = x * drive + bias;
            if (shape == 1) y = pw_sin1(t * 0.25f);
            else { float u = pw_frac((t + 1.0f) * 0.25f); y = 1.0f - 4.0f * pw_absf(u - 0.5f); }
        }
        y = mix * y + (1.0f - mix) * (shape == 3 ? in[0][i] * 0.2f : x);
        float hp = y - s->x1 + 0.9985f * s->y1;
        s->x1 = y; s->y1 = hp;
        out[0][i] = 5.0f * hp;
    }
    if (pw_absf(s->py) < 1e-20f) s->py = 0;
}

/* HARMONIC TREMOLO: the sound splits at SPLIT into lows and highs and each is turned up and down by the same LFO but
 * in opposite directions, so the tone seems to swirl rather than just pulse (the old amps' "harmonic" tremolo). */
typedef struct { float ph, lp; } htrem_st;
PROC(htrem_proc)
{
    ST(htrem_st);
    float rate = sync_hz(m->p[4]), depth = m->p[1], a = pw_lp_coef_hz(pw_clamp(m->p[2], 100.0f, 8000.0f)), mix = m->p[5];
    if (rate <= 0) rate = m->p[0];
    int tri = m->p[3] > 0.5f;
    float ph = s->ph, dt = rate * PW_INV_SR;
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(1)) dt = rate * pw_exp2(in[1][i]) * PW_INV_SR;
        float l = tri ? (ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph) : pw_sin1(ph);   /* -1..1 */
        float up = 0.5f + 0.5f * l, down = 1.0f - up;
        float x = in[0][i];
        s->lp += a * (x - s->lp);
        float lo = s->lp, hi = x - s->lp;
        out[0][i] = x + (lo * (1.0f - depth * up) + hi * (1.0f - depth * down) - x) * mix;
        ph += dt; if (ph >= 1.0f) ph -= (float)pw_floori(ph);
    }
    s->ph = ph;
    if (pw_absf(s->lp) < 1e-20f) s->lp = 0;
}

typedef struct { float lp, hold, x1, y1; int cnt; } dist_st;
PROC(dist_proc)
{
    ST(dist_st);
    int mode = (int)(m->p[1] + 0.5f);
    float d0 = m->p[0], mix = m->p[3];
    float a = pw_lp_coef_hz(400.0f * pw_exp2(m->p[2] * 5.5f));
    float d = d0, g = pw_exp2(d * 6.64f);
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(1)) { d = pw_clamp(d0 + in[1][i] * 0.2f, 0.0f, 1.0f); g = pw_exp2(d * 6.64f); }
        float x = in[0][i] * 0.2f, y;
        switch (mode) {
        case 0: y = pw_tanh(x * g); break;
        case 1: y = pw_clamp(x * g, -1.0f, 1.0f) * 0.85f; break;
        case 2: { y = pw_tanh(x * g * 1.5f + 0.35f) - 0.3364f;
                  float hp = y - s->x1 + 0.995f * s->y1; s->x1 = y; s->y1 = hp; y = hp * 1.1f; } break;
        default: {
            int hold = 1 + (int)(d * d * 40.0f);
            if (--s->cnt <= 0) {
                float step = pw_exp2(-(15.0f - d * 13.0f));
                s->hold = (float)pw_floori(x / step + 0.5f) * step;
                s->cnt = hold;
            }
            y = s->hold;
        } break;
        }
        s->lp += a * (y - s->lp);
        out[0][i] = 5.0f * (mix * s->lp + (1.0f - mix) * x);
    }
    if (pw_absf(s->lp) < 1e-20f) s->lp = 0;
}

typedef struct { uint32_t w, since, tap; float t, lp, lpb, wp, fl; uint8_t tap_hi; } delay_st;
#define DELAY_SCALE 3000.0f
PW_INLINE float dly_read(const int16_t *buf, uint32_t len, uint32_t w, float t)
{
    float rp = (float)w - t;
    if (rp < 0) rp += (float)len;
    uint32_t i0 = (uint32_t)rp, i1 = i0 + 1u >= len ? 0 : i0 + 1u;
    return (buf[i0] + (buf[i1] - buf[i0]) * (rp - (float)i0)) * (1.0f / DELAY_SCALE);
}
PROC_COLD(delay_proc)
{
    ST(delay_st);
    int16_t *buf = PW_POOL16(m);
    uint32_t len = m->mem_len / 2u;
    if (m->status != PW_STATUS_OK || len < 64) {
        for (int i = 0; i < PW_N; ++i) { out[0][i] = in[0][i]; out[1][i] = 0; out[2][i] = out[3][i] = in[0][i]; }
        return;
    }
    int ping = m->p[5] > 0.5f, frz = m->p[8] > 0.5f || (IN_CONNECTED(4) && in[4][0] > 1.0f);
    uint32_t half = len / 2u, n = ping ? half : len;   /* PING-PONG: two lines, left and right, half the memory each */
    int16_t *bufb = buf + half;
    if (s->w >= n) s->w = 0;
    float maxt = (float)(n - 4u);
    float fb0 = m->p[1], mix = m->p[3];
    float a = frz ? 1.0f : pw_lp_coef_hz(300.0f * pw_exp2(m->p[2] * 6.0f));
    float base = m->p[0], sy = sync_sec(m->p[4]);
    if (sy > 0) base = sy;                                    /* SYNC: a note value of the tempo */
    float depth = m->p[6] * 0.012f, wrate = m->p[7] * PW_INV_SR;   /* WOBBLE: the time swims, as a tape's does */
    float target = pw_clamp(base * PW_SRF, 2.0f, maxt);
    if (s->t < 1.0f) s->t = target;
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(3)) {                                 /* TAP: the time between two taps is the delay time */
            if (s->since < 4u * PW_SR) ++s->since;
            if (pw_rise(&s->tap_hi, in[3][i])) { if (s->since > 64u && s->since < 4u * PW_SR) s->tap = s->since; s->since = 0; }
            if (s->tap) base = (float)s->tap * PW_INV_SR;
        } else s->tap = 0;
        if (IN_CONNECTED(1)) target = pw_clamp(base * pw_exp2(-in[1][i]) * PW_SRF, 2.0f, maxt);
        else target = pw_clamp(base * PW_SRF, 2.0f, maxt);
        s->t += (target - s->t) * 0.0006f;
        float tt = s->t;
        if (depth > 0) {
            s->wp += wrate; s->wp -= (float)pw_floori(s->wp);
            s->fl += 0.0008f * (pw_rand_f(&pws.rng) - s->fl);
            tt = pw_clamp(tt * (1.0f + depth * (pw_sin1(s->wp) + 6.0f * s->fl)), 2.0f, maxt);
        }
        float fb = frz ? 1.0f : pw_clamp(fb0 + in[2][i] * 0.2f, 0.0f, 1.2f), x = frz ? 0.0f : in[0][i];
        float dry = in[0][i] * (1.0f - mix), wl, wr, wet;
        if (!ping) {
            wl = wr = dly_read(buf, n, s->w, tt);
            s->lp += a * (wl - s->lp);
            float v = 8.0f * pw_tanh((x + fb * s->lp) * 0.125f);
            buf[s->w] = (int16_t)(v * DELAY_SCALE);
            wet = wl;
        } else {
            wl = dly_read(buf, n, s->w, tt); wr = dly_read(bufb, n, s->w, tt);
            s->lp += a * (wl - s->lp); s->lpb += a * (wr - s->lpb);
            buf[s->w] = (int16_t)(8.0f * pw_tanh((x + fb * s->lpb) * 0.125f) * DELAY_SCALE);   /* the left line: the input and what comes back from the right */
            bufb[s->w] = (int16_t)(8.0f * pw_tanh(fb * s->lp * 0.125f) * DELAY_SCALE);          /* the right line: the left, once more */
            wet = 0.5f * (wl + wr);
        }
        if (++s->w >= n) s->w = 0;
        out[0][i] = dry + wet * mix;
        out[1][i] = wet;
        out[2][i] = dry + wl * mix;
        out[3][i] = dry + wr * mix;
    }
    if (pw_absf(s->lp) < 1e-20f) s->lp = 0;
    if (pw_absf(s->lpb) < 1e-20f) s->lpb = 0;
}

/* Reverb: 4-line feedback delay network, int16 lines. Four allpasses a side diffuse the input (DIFFUSE), the lines' reads wander
 * a little (MOD: the tail stops ringing), a pre-delay and a low cut sit in front of and inside the tail, and MODE picks a room, a
 * hall, a plate, or a gated or swelling tail (a gate on the wet signal). */
static const uint16_t rv_len[4] PW_DSP_DATA = {1693, 1997, 2251, 2591};
static const uint16_t rv_ap[8] PW_DSP_DATA = {142, 379, 151, 389, 89, 211, 101, 233};
#define RV_LINE 2620u
#define RV_AP_SUM (142u + 379u + 151u + 389u + 89u + 211u + 101u + 233u)
#define RV_SH 1056u   /* the shimmer's pitch shifter: a 1024-sample window */
#define RV_PRE 2205u  /* the pre-delay: 50 ms a side */
#define RV_BYTES ((4u * RV_LINE + RV_AP_SUM + RV_SH + 2u * RV_PRE + 16u) * 2u)
#define RV_SCALE 12000.0f
typedef struct {
    uint16_t w[4], apw[8], sw, pw; float lp[4], lo[4], ph[4], size, sd, env, g, t; uint32_t hold, idle; uint8_t hi;
} rv_st;   /* (idle: the samples since the sound last came in) */
PW_INLINE float rv_allpass(int16_t *b, uint16_t n, uint16_t *w, float x, float k)
{
    float d = b[*w] * (1.0f / RV_SCALE);
    float y = d - k * x;
    float v = pw_clamp(x + k * y, -2.7f, 2.7f);
    b[*w] = (int16_t)(v * RV_SCALE);
    if (++*w >= n) *w = 0;
    return y;
}
PROC(reverb_proc)
{
    ST(rv_st);
    if (m->status != PW_STATUS_OK) {
        for (int i = 0; i < PW_N; ++i) { out[0][i] = in[0][i]; out[1][i] = IN_CONNECTED(1) ? in[1][i] : in[0][i]; }
        return;
    }
    int16_t *line = PW_POOL16(m), *ap[8];
    ap[0] = line + 4 * RV_LINE;
    for (int k = 1; k < 8; ++k) ap[k] = ap[k - 1] + rv_ap[k - 1];
    int16_t *sb = ap[7] + rv_ap[7], *pre0 = sb + RV_SH, *pre1 = pre0 + RV_PRE;
    static const float shim_r[5] PW_DSP_DATA = {2.0f, 1.5f, 1.3348f, 0.5f, 4.0f};   /* OCTAVE, FIFTH, FOURTH up; OCTAVE down; TWO OCTAVES */
    /* MODE: ROOM, HALL, PLATE, GATED, SWELL */
    static const float md_size[5] PW_DSP_DATA = {0.7f, 1.0f, 0.85f, 0.8f, 0.9f}, md_ap[5] PW_DSP_DATA = {-0.1f, 0.0f, 0.12f, 0.05f, 0.0f};
    static const float md_mod[5] PW_DSP_DATA = {0.5f, 1.0f, 0.35f, 0.5f, 1.0f}, md_damp[5] PW_DSP_DATA = {0.85f, 1.0f, 1.15f, 1.0f, 1.0f};
    int mode = (int)(m->p[7] + 0.5f); if (mode < 0 || mode > 4) mode = 1;
    float shim = m->p[5], sr = shim_r[(int)(m->p[6] + 0.5f) % 5];
    float size = (0.35f + 0.65f * m->p[0]) * md_size[mode], t60 = m->p[1];
    float damp = pw_minf(1.0f, (0.12f + 0.88f * (1.0f - m->p[2])) * md_damp[mode]), mix = m->p[3];
    int pd = (int)(pw_clamp(m->p[8], 0.0f, 0.05f) * PW_SRF); if (pd > (int)RV_PRE - 2) pd = (int)RV_PRE - 2;
    float apk = pw_clamp(0.25f + 0.5f * m->p[9] + md_ap[mode], 0.1f, 0.78f);
    float depth = 4.0f * m->p[10] * md_mod[mode];
    float lca = 0.0003f + m->p[11] * m->p[11] * 0.045f;
    if (s->size <= 0) s->size = size;
    float gain[4];
    for (int k = 0; k < 4; ++k) gain[k] = pw_exp2(-9.966f * rv_len[k] * size / (t60 * PW_SRF));
    int frz = m->p[4] > 0.5f || (IN_CONNECTED(2) && in[2][0] > 1.0f);
    if (frz) { damp = 1.0f; for (int k = 0; k < 4; ++k) gain[k] = pw_exp2(-9.966f * rv_len[k] * size / (40.0f * PW_SRF)); }   /* (frozen: 40 s to fade) */
    float tg = pw_clamp(t60 * 0.3f, mode == 4 ? 0.2f : 0.12f, mode == 4 ? 1.2f : 0.6f) * PW_SRF;   /* the gate's hold, the swell's rise */
    static const float lrate[4] PW_DSP_DATA = {0.31f, 0.43f, 0.57f, 0.71f};
    {   /* nothing has come in for twice the tail's time (-120 dB): the tail is gone, so the block is only the dry sound */
        float pk = 0;
        for (int i = 0; i < PW_N; i += 2) { float a = pw_absf(in[0][i]); if (a > pk) pk = a; if (IN_CONNECTED(1)) { a = pw_absf(in[1][i]); if (a > pk) pk = a; } }
        if (pk > 1e-5f || frz || shim > 0.001f || mode >= 3) s->idle = 0;
        else if (s->idle < 0x7FFFFFFFu) s->idle += PW_N;
        if (s->idle > (uint32_t)((2.0f * t60 + 0.2f) * PW_SRF)) {
            for (int i = 0; i < PW_N; ++i) { out[0][i] = in[0][i] * (1.0f - mix); out[1][i] = (IN_CONNECTED(1) ? in[1][i] : in[0][i]) * (1.0f - mix); }
            return;
        }
    }
    float moff[4] = {0, 0, 0, 0};   /* the lines' wander: a slow sine, worked out every 16 samples */
    for (int i = 0; i < PW_N; ++i) {
        if ((i & 15) == 0)
            for (int k = 0; k < 4; ++k) {
                s->ph[k] += lrate[k] * PW_INV_SR * 16.0f; if (s->ph[k] >= 1.0f) s->ph[k] -= 1.0f;
                moff[k] = depth * (1.0f + pw_sin1(s->ph[k]));
            }
        s->size += (size - s->size) * 0.0002f;
        float il = in[0][i], ir = IN_CONNECTED(1) ? in[1][i] : in[0][i];
        float xl = il * 0.2f, xr = ir * 0.2f;
        if (pd > 0) {   /* the pre-delay */
            int rp = (int)s->pw - pd; if (rp < 0) rp += (int)RV_PRE;
            float dl = pre0[rp] * (1.0f / RV_SCALE), dr = pre1[rp] * (1.0f / RV_SCALE);
            pre0[s->pw] = (int16_t)(pw_clamp(xl, -2.7f, 2.7f) * RV_SCALE); pre1[s->pw] = (int16_t)(pw_clamp(xr, -2.7f, 2.7f) * RV_SCALE);
            if (++s->pw >= RV_PRE) s->pw = 0;
            xl = dl; xr = dr;
        }
        xl = rv_allpass(ap[0], rv_ap[0], &s->apw[0], rv_allpass(ap[1], rv_ap[1], &s->apw[1], rv_allpass(ap[4], rv_ap[4], &s->apw[4], rv_allpass(ap[5], rv_ap[5], &s->apw[5], xl, apk), apk), apk), apk);
        xr = rv_allpass(ap[2], rv_ap[2], &s->apw[2], rv_allpass(ap[3], rv_ap[3], &s->apw[3], rv_allpass(ap[6], rv_ap[6], &s->apw[6], rv_allpass(ap[7], rv_ap[7], &s->apw[7], xr, apk), apk), apk), apk);
        float f[4];
        for (int k = 0; k < 4; ++k) {
            float dd = rv_len[k] * s->size + moff[k];
            int di = (int)dd; float df = dd - (float)di;
            int r = (int)s->w[k] - di; if (r < 0) r += RV_LINE;
            int r2 = r - 1; if (r2 < 0) r2 += RV_LINE;
            float v = (line[k * RV_LINE + r] + (line[k * RV_LINE + r2] - line[k * RV_LINE + r]) * df) * (1.0f / RV_SCALE);
            s->lp[k] += damp * (v - s->lp[k]);
            s->lo[k] += lca * (s->lp[k] - s->lo[k]);   /* the low cut */
            f[k] = s->lp[k] - s->lo[k];
        }
        float h0 = 0.5f * (f[0] + f[1] + f[2] + f[3]), h1 = 0.5f * (f[0] - f[1] + f[2] - f[3]);
        float h2 = 0.5f * (f[0] + f[1] - f[2] - f[3]), h3 = 0.5f * (f[0] - f[1] - f[2] + f[3]);
        float h[4] = {h0, h1, h2, h3}, inj[4] = {xl, xr, xl, xr};
        if (frz) inj[0] = inj[1] = inj[2] = inj[3] = 0;
        if (shim > 0.001f) {   /* SHIMMER: the tail, shifted, goes back in: each pass climbs (two crossfaded taps, like PITCH SHIFTER) */
            sb[s->sw] = (int16_t)(pw_clamp(h0, -2.7f, 2.7f) * RV_SCALE);
            s->sd += 1.0f - sr;
            if (s->sd >= 1024.0f) s->sd -= 1024.0f;
            if (s->sd < 0) s->sd += 1024.0f;
            float ps = 0;
            for (int q = 0; q < 2; ++q) {
                float d = s->sd + (q ? 512.0f : 0.0f);
                if (d >= 1024.0f) d -= 1024.0f;
                float g = 1.0f - pw_absf(d * (2.0f / 1024.0f) - 1.0f);
                float rp = (float)s->sw - (d + 2.0f);
                if (rp < 0) rp += (float)(RV_SH - 16u);
                int ri = (int)rp; float rf = rp - (float)ri;
                int rj = ri + 1; if (rj >= (int)(RV_SH - 16u)) rj = 0;
                ps += g * (sb[ri] + (sb[rj] - sb[ri]) * rf);
            }
            if (++s->sw >= RV_SH - 16u) s->sw = 0;
            ps *= (1.0f / RV_SCALE) * shim * 0.12f;
            for (int k = 0; k < 4; ++k) inj[k] += ps;
        }
        for (int k = 0; k < 4; ++k) {
            float v = pw_clamp(inj[k] + gain[k] * h[k], -2.7f, 2.7f);
            line[k * RV_LINE + s->w[k]] = (int16_t)(v * RV_SCALE);
            if (++s->w[k] >= RV_LINE) s->w[k] = 0;
        }
        float gt = 1.0f;
        if (mode >= 3) {   /* GATED: the tail is cut a moment after the sound stops; SWELL: it rises after each onset, then is cut */
            float e = pw_maxf(pw_absf(il), pw_absf(ir));
            s->env = e > s->env ? e : s->env * 0.9995f;
            int hi = s->env > 0.25f;
            float target;
            if (mode == 3) {
                if (hi) s->hold = (uint32_t)tg;
                else if (s->hold) --s->hold;
                target = s->hold ? 1.0f : 0.0f;
                s->g += (target - s->g) * (target > s->g ? 0.02f : 0.004f);
            } else {
                if (hi && !s->hi) s->t = 0;
                s->t += 1.0f;
                if (s->t < tg) { float q = s->t / tg; s->g = q * q; }
                else s->g += (0.0f - s->g) * 0.004f;
            }
            s->hi = (uint8_t)hi;
            gt = s->g;
        }
        float wl = f[0] + 0.6f * f[2] - 0.3f * f[1], wr = f[1] + 0.6f * f[3] - 0.3f * f[0];
        out[0][i] = il * (1.0f - mix) + wl * 5.0f * mix * gt;
        out[1][i] = ir * (1.0f - mix) + wr * 5.0f * mix * gt;
    }
    for (int k = 0; k < 4; ++k) { if (pw_absf(s->lp[k]) < 1e-20f) s->lp[k] = 0; if (pw_absf(s->lo[k]) < 1e-20f) s->lo[k] = 0; }
}

/* SPACE: a small, cheap stereo reverb for using everywhere: two damped combs a side and an allpass, one knob for the room and its tail. */
static const uint16_t rm_len[4] PW_DSP_DATA = {1237, 1559, 1277, 1613};
static const uint16_t rm_ap[2] PW_DSP_DATA = {225, 245};
#define RM_BYTES ((1237u + 1559u + 1277u + 1613u + 225u + 245u + 16u) * 2u)
typedef struct { uint16_t w[4], apw[2]; float lp[4], size; } rm_st;
PROC(space_proc)
{
    ST(rm_st);
    if (m->status != PW_STATUS_OK) {
        for (int i = 0; i < PW_N; ++i) { out[0][i] = in[0][i]; out[1][i] = IN_CONNECTED(1) ? in[1][i] : in[0][i]; }
        return;
    }
    int16_t *line[4], *ap[2];
    line[0] = PW_POOL16(m); line[1] = line[0] + 1237; line[2] = line[1] + 1559; line[3] = line[2] + 1277; ap[0] = line[3] + 1613; ap[1] = ap[0] + 225;
    float size = 0.45f + 0.55f * m->p[0], t60 = 0.25f + 2.75f * m->p[0] * m->p[0];
    float damp = 0.08f + 0.92f * m->p[1], mix = m->p[2];
    if (s->size <= 0) s->size = size;
    float g[4];
    for (int k = 0; k < 4; ++k) g[k] = pw_exp2(-9.966f * rm_len[k] * size / (t60 * PW_SRF));
    for (int i = 0; i < PW_N; ++i) {
        s->size += (size - s->size) * 0.0002f;
        float il = in[0][i], ir = IN_CONNECTED(1) ? in[1][i] : il;
        float x = (il + ir) * 0.1f, f[4];
        for (int a = 0; a < 2; ++a) {   /* a little diffusion before the combs */
            float d = ap[a][s->apw[a]] * (1.0f / RV_SCALE), y = d - 0.6f * x;
            ap[a][s->apw[a]] = (int16_t)(pw_clamp(x + 0.6f * y, -2.7f, 2.7f) * RV_SCALE);
            if (++s->apw[a] >= rm_ap[a]) s->apw[a] = 0;
            x = y;
        }
        for (int k = 0; k < 4; ++k) {
            int d = (int)(rm_len[k] * s->size), r = (int)s->w[k] - d; if (r < 0) r += rm_len[k];
            float v = line[k][r] * (1.0f / RV_SCALE);
            s->lp[k] += damp * (v - s->lp[k]);
            f[k] = s->lp[k];
            line[k][s->w[k]] = (int16_t)(pw_clamp(x * (1.0f - g[k]) * 2.0f + g[k] * f[k], -2.7f, 2.7f) * RV_SCALE);
            if (++s->w[k] >= rm_len[k]) s->w[k] = 0;
        }
        out[0][i] = il * (1.0f - mix) + (f[0] + f[1]) * 2.5f * mix;
        out[1][i] = ir * (1.0f - mix) + (f[2] + f[3]) * 2.5f * mix;
    }
    for (int k = 0; k < 4; ++k) if (pw_absf(s->lp[k]) < 1e-20f) s->lp[k] = 0;
}


/* ROOM MODES: the standing waves of a room. A box room rings at the frequencies where its walls fit whole half waves; those are the
 * ROOT (the longest wall) and its relatives, not a harmonic series: along each wall, and across the faces. SHAPE moves the other walls
 * (0: nearly the same length, so the modes crowd and beat; 1: well apart). Whatever comes in, the modes that match it ring on (DECAY,
 * the high ones sooner), drifting a little like air moving; the modes go left and right by turns. */
#define RO_N 8
static const float ro_mode[RO_N][3] PW_DSP_DATA = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {2, 0, 0}, {1, 0, 1}, {0, 2, 0}, {0, 1, 1}};
typedef struct { float y1[RO_N], y2[RO_N], ph[RO_N], f[RO_N]; uint16_t quiet; } ro_st;   /* f: each mode's pitch now, for the picture */
/* GENDY: Xenakis' dynamic stochastic synthesis (after SuperCollider's Gendy1). One cycle is a ring of POINTS corners joined by straight lines; every cycle each
 * corner's height and the time it takes random-walk (A CHAOS / T CHAOS set the step, DIST its odds; bounded: they bounce off the walls). 0 holds one shape.
 * RANGE: AUDIO tracks pitch; LOW is 3 octaves down; CV is a slow wandering voltage (about 2 Hz at C4, no DC block). */
typedef struct { float a[16], d[16], c[17]; float ph, y, dc_x, dc_y; uint32_t seed; uint8_t seg, rst_hi; uint16_t trig; } gendy_st;
static void gendy_shape(gendy_st *s, unsigned n)   /* where each corner's segment starts in the cycle (0..1) */
{
    float sum = 0.0f;
    for (unsigned i = 0; i < n; ++i) sum += s->d[i];
    s->c[0] = 0.0f;
    for (unsigned i = 0; i < n; ++i) s->c[i + 1] = s->c[i] + s->d[i] / sum;
    s->c[n] = 1.0f;
}
static void gendy_reset(pw_module *m)
{
    ST(gendy_st);
    s->seed = 0x6E3D1u + (uint32_t)(m - pws.mod) * 2654435761u;
    for (unsigned i = 0; i < 16; ++i) { s->a[i] = pw_rand_f(&s->seed); s->d[i] = 1.0f; }
    s->ph = 0.0f; s->seg = 0; s->y = 0.0f;
    gendy_shape(s, 16);
}
static void gendy_init(pw_module *m) { gendy_reset(m); }
/* one random step, -1..1, from the chosen odds (v: uniform -1..1) */
static float gendy_step(unsigned kind, float v)
{
    switch (kind) {
    case 1: return pw_clamp(pw_tan_pi(0.4f * v) * 0.325f, -1.0f, 1.0f);                       /* CAUCHY: mostly small, now and then a leap */
    case 2: { float u = 0.5f + 0.49f * v; return pw_clamp(pw_log2(u / (1.0f - u)) * 0.1733f, -1.0f, 1.0f); }   /* LOGIST: bell-like */
    case 3: return pw_sin1(v * 0.25f);                                                           /* ARCSINE: nearly always a big step either way */
    case 4: { float q = 1.0f - 0.97f * pw_absf(v); float e = -pw_log2(q) * 0.2f; e = pw_minf(e, 1.0f); return v < 0 ? -e : e; }   /* EXPON: tiny steps, rare big ones */
    default: return v;                                                                           /* LINEAR: even odds */
    }
}
PROC(gendy_proc)
{
    ST(gendy_st);
    unsigned n = (unsigned)pw_clamp(m->p[1] + 0.5f, 3.0f, 16.0f), kind = (unsigned)(m->p[6] + 0.5f), rng = (unsigned)(m->p[7] + 0.5f);
    float tune = m->p[0] * (1.0f / 12.0f) - (rng == 1 ? 3.0f : rng == 2 ? 7.0f : 0.0f), ach = m->p[2] * m->p[2] * 0.5f, tch = m->p[3] * m->p[3] * 0.5f;
    float sm = 1.0f - m->p[4], k = 0.02f + 0.98f * sm * sm * sm, lvl = m->p[5] * 5.0f;
    (void)connected;
    if (s->seg >= n) { s->seg = 0; s->ph = 0.0f; }
    gendy_shape(s, n);
    for (int i = 0; i < PW_N; ++i) {
        out[1][i] = s->trig > 0 ? 5.0f : 0.0f;
        if (s->trig > 0) --s->trig;
        if (IN_CONNECTED(1) && pw_rise(&s->rst_hi, in[1][i])) { gendy_reset(m); gendy_shape(s, n); }
        float hz = pw_pitch_hz((IN_CONNECTED(0) ? in[0][i] : 0.0f) + tune);
        s->ph += pw_minf(hz * PW_INV_SR, 0.45f);
        if (s->ph >= 1.0f) {   /* a new cycle: every corner takes a step */
            s->ph -= 1.0f; if (s->ph >= 1.0f) s->ph = 0.0f;
            for (unsigned j = 0; j < n; ++j) {
                float a = s->a[j] + gendy_step(kind, pw_rand_f(&s->seed)) * ach, d = s->d[j] + gendy_step(kind, pw_rand_f(&s->seed)) * tch;
                if (a > 1.0f) a = 2.0f - a; else if (a < -1.0f) a = -2.0f - a;
                if (d > 1.75f) d = 3.5f - d; else if (d < 0.25f) d = 0.5f - d;
                s->a[j] = pw_clamp(a, -1.0f, 1.0f); s->d[j] = pw_clamp(d, 0.25f, 1.75f);
            }
            gendy_shape(s, n); s->seg = 0;
            s->trig = (uint16_t)(0.002f * PW_SRF + 1.0f);
        }
        while (s->seg + 1u < n && s->ph >= s->c[s->seg + 1u]) ++s->seg;
        float a0 = s->a[s->seg], a1 = s->a[s->seg + 1u < n ? s->seg + 1u : 0u];
        float x = a0 + (a1 - a0) * (s->ph - s->c[s->seg]) / (s->c[s->seg + 1u] - s->c[s->seg]);
        s->y += (x - s->y) * k;
        if (rng == 2) { out[0][i] = lvl * s->y; continue; }   /* CV: the voltage as it is */
        float o = s->y - s->dc_x + 0.9995f * s->dc_y; s->dc_x = s->y; s->dc_y = o;
        out[0][i] = lvl * o;
    }
    if (pw_absf(s->dc_y) < 1e-20f) s->dc_y = 0;
}
PROC(room_proc)
{
    ST(ro_st);
    int nm = (int)(m->p[2] + 0.5f); nm = nm < 1 ? 1 : nm > RO_N ? RO_N : nm;
    float root = m->p[0] * pw_exp2(IN_CONNECTED(1) ? in[1][0] : 0.0f), shape = m->p[1];
    float fy = 1.2f + 0.7f * shape, fz = 1.5f + 1.5f * shape;
    float t60 = m->p[3], mix = m->p[4];
    const float damp = 0.5f, drift = 0.3f, width = 0.7f;   /* fixed: the high modes die sooner, the air moves, the modes alternate sides */
    float a1[RO_N], a2[RO_N], g[RO_N], wl[RO_N], wr[RO_N];
    for (int k = nm; k < RO_N; ++k) s->f[k] = 0.0f;
    for (int k = 0; k < nm; ++k) {
        float fx = ro_mode[k][0], fyy = ro_mode[k][1] * fy, fzz = ro_mode[k][2] * fz;
        float r2 = fx * fx + fyy * fyy + fzz * fzz;
        float f = root * (r2 > 0.0f ? pw_exp2(0.5f * pw_log2(r2)) : 1.0f);
        s->ph[k] += (0.03f + 0.017f * (float)k) * (float)PW_N * PW_INV_SR; if (s->ph[k] >= 1.0f) s->ph[k] -= 1.0f;
        f *= 1.0f + drift * 0.012f * pw_sin1(s->ph[k]);
        s->f[k] = f;
        if (f > PW_SRF * 0.45f) { a1[k] = a2[k] = g[k] = 0; wl[k] = wr[k] = 0; continue; }
        float tk = t60 / (1.0f + damp * 2.5f * (float)k / (float)RO_N * (float)k);   /* the higher modes die sooner */
        float r = pw_exp2(-1.442695f * 6.9f / (tk * PW_SRF));
        a1[k] = 2.0f * r * pw_cos1(f * PW_INV_SR); a2[k] = -r * r;
        g[k] = 2.0f * (1.0f - r) * pw_sin1(f * PW_INV_SR) * 30.0f;
        float pan = (k & 1) ? 0.5f + 0.5f * width : 0.5f - 0.5f * width;   /* the modes alternate sides */
        wl[k] = 1.0f - pan; wr[k] = pan;
    }
    int idle = 1;
    for (int i = 0; i < PW_N; ++i) if (in[0][i] > 1e-5f || in[0][i] < -1e-5f) { idle = 0; break; }
    if (idle && s->quiet > 8) {   /* nothing in and the modes gone: nothing to work out */
        for (int i = 0; i < PW_N; ++i) { out[0][i] = 0; out[1][i] = 0; }
        return;
    }
    for (int i = 0; i < PW_N; ++i) {
        float x = in[0][i], l = 0, r = 0;
        for (int k = 0; k < nm; ++k) {
            float v = a1[k] * s->y1[k] + a2[k] * s->y2[k] + g[k] * x;
            s->y2[k] = s->y1[k]; s->y1[k] = v;
            l += v * wl[k]; r += v * wr[k];
        }
        l = 5.0f * pw_tanh(l * 0.2f); r = 5.0f * pw_tanh(r * 0.2f);
        out[0][i] = x * (1.0f - mix) + l * mix;
        out[1][i] = x * (1.0f - mix) + r * mix;
    }
    float big = 0;
    for (int k = 0; k < nm; ++k) { if (pw_absf(s->y1[k]) < 1e-20f) s->y1[k] = 0; if (pw_absf(s->y2[k]) < 1e-20f) s->y2[k] = 0; big = pw_maxf(big, pw_absf(s->y1[k])); }
    if (big < 1e-4f && idle) { if (s->quiet < 60000u) ++s->quiet; } else s->quiet = 0;
}

/* Ring modulator and Hilbert frequency shifter (Niemitalo coefficients). */
static const float hb_a[4] PW_DSP_DATA = {0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f};
static const float hb_b[4] PW_DSP_DATA = {0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f};
typedef struct { float a[4][4], b[4][4], delay, ph; } ring_st;
PW_INLINE float hb_chain(float st[4][4], const float *c, float x)
{
    for (int k = 0; k < 4; ++k) {
        float c2 = c[k] * c[k];
        float y = c2 * (x + st[k][3]) - st[k][1];
        st[k][1] = st[k][0]; st[k][0] = x;   /* x[n-1] -> x[n-2] */
        st[k][3] = st[k][2]; st[k][2] = y;   /* y[n-1] -> y[n-2] */
        x = y;
    }
    return x;
}
PROC(ring_proc)
{
    ST(ring_st);
    float shift0 = m->p[0], amt = m->p[1], mix = m->p[2];
    for (int i = 0; i < PW_N; ++i) {
        float shift = shift0 + in[2][i] * amt;
        float x = in[0][i];
        float re = hb_chain(s->a, hb_a, x), im = hb_chain(s->b, hb_b, x);
        float I = s->delay; s->delay = re;
        float c = pw_cos1(s->ph), sn = pw_sin1(s->ph);
        out[0][i] = x + ((IN_CONNECTED(1) ? x * in[1][i] * 0.2f : x * sn) - x) * mix;
        out[1][i] = I + (I * c + im * sn - I) * mix;
        out[2][i] = I + (I * c - im * sn - I) * mix;
        s->ph += shift * PW_INV_SR;
        s->ph -= (float)pw_floori(s->ph);
    }
    for (int k = 0; k < 4; ++k) for (int j = 0; j < 4; ++j) {
        if (pw_absf(s->a[k][j]) < 1e-20f) s->a[k][j] = 0;
        if (pw_absf(s->b[k][j]) < 1e-20f) s->b[k][j] = 0;
    }
}

/* ------------------------------------------------------------- modulation */
typedef struct { float v, y, vr, vel; uint8_t stage, gate_hi, trig_hi; uint16_t eoc; } env_st;
enum { ENV_IDLE, ENV_ATT, ENV_DEC, ENV_SUS, ENV_REL };
/* A stage's CURVE, -1..1: above 0 the stage runs towards linear (its one-pole aims past its end), below 0 it is
 * bent the other way (slow start, snap): 0 is the classic exponential, as ever. */
PW_INLINE float env_over(float c) { return c > 0 ? 3.0f * c * c : 0.0f; }
PW_INLINE float env_ln(float x) { return pw_log2(x) * 0.6931472f; }
PW_INLINE float env_pow(float x, float p) { return x <= 0 ? 0.0f : pw_exp2(p * pw_log2(x)); }
PROC(env_proc)
{
    ST(env_st);
    int mode = (int)(m->p[4] + 0.5f); /* 0 ADSR, 1 AD, 2 AR */
    int loop = m->p[5] > 0.5f, trg = (int)(m->p[9] + 0.5f);   /* TRIGGER: 0 SOFT (from where it is), 1 HARD (from 0), 2 LEGATO (no new attack while it sounds) */
    float vamt = m->p[10], rs = m->p[11] < 0.5f ? 0.1f : m->p[11] < 1.5f ? 1.0f : 10.0f;   /* RANGE: FAST x0.1, NORMAL, SLOW x10 */
    float ca = m->p[6], cd = m->p[7], cr = m->p[8];
    float aK = 0.3f + env_over(ca), dK = env_over(cd), rK = env_over(cr);
    float ka = 1.0f - pw_coef(m->p[0] * rs / (ca > 0 ? env_ln((1.0f + aK) / aK) : 1.466f));
    float sus = m->p[2], dd = 1.0f - sus;
    float kd = 1.0f - pw_coef(m->p[1] * rs / (dK > 0 ? env_ln((1.0f + dK + 0.0183f) / (dK + 0.0183f)) : 4.0f));
    float kr = 1.0f - pw_coef(m->p[3] * rs / (rK > 0 ? env_ln((1.0f + rK + 0.0183f) / (rK + 0.0183f)) : 4.0f));
    float pa = ca < 0 ? 1.0f - 2.0f * ca : 1.0f, pd = cd < 0 ? 1.0f - 2.0f * cd : 1.0f, pr = cr < 0 ? 1.0f - 2.0f * cr : 1.0f;
    int bent = ca < 0 || cd < 0 || cr < 0;
    if (s->stage == ENV_IDLE && !loop && !s->eoc && s->v == 0.0f && pw_absf(s->y) < 1e-6f) {   /* at rest, and nothing comes in: the block is a constant */
        int quiet = 1;
        for (int i = 0; i < PW_N && quiet; ++i) if (in[0][i] > 0.4f || (IN_CONNECTED(1) && in[1][i] > 0.4f)) quiet = 0;
        if (quiet) {
            float sc = 1.0f - vamt * (1.0f - s->vel);
            s->gate_hi = 0; s->trig_hi = 0; s->y = 0;
            for (int i = 0; i < PW_N; ++i) { out[0][i] = 0; out[1][i] = 5.0f - 5.0f * 0.0f * sc; out[2][i] = 0; }
            return;
        }
    }
    for (int i = 0; i < PW_N; ++i) {
        uint8_t was = s->gate_hi;
        int rise = pw_rise(&s->gate_hi, in[0][i]);
        int trig = rise || (IN_CONNECTED(1) && pw_rise(&s->trig_hi, in[1][i]));
        if (trig) {
            s->vel = IN_CONNECTED(2) ? pw_clamp(in[2][i] * 0.2f, 0.0f, 1.0f) : 1.0f;
            if (trg == 2 && s->stage != ENV_IDLE && s->v > 0.0005f) s->stage = s->v > sus ? ENV_DEC : ENV_SUS;   /* LEGATO: carries on */
            else { s->stage = ENV_ATT; if (trg == 1) { s->v = 0; s->y = 0; } }
        } else if (was && !s->gate_hi && mode != 1 && s->stage != ENV_IDLE) { s->stage = ENV_REL; s->vr = s->v > 0.0005f ? s->v : 0.0005f; }
        float shaped = s->v;
        switch (s->stage) {
        case ENV_ATT:
            s->v += (1.0f + aK - s->v) * ka;
            if (s->v >= 1.0f) { s->v = 1.0f; s->stage = mode == 2 ? ENV_SUS : ENV_DEC; }
            shaped = pa > 1.0f ? env_pow(s->v, pa) : s->v;
            break;
        case ENV_DEC: {
            float target = mode == 1 ? 0.0f : sus - dK * dd;
            if (mode == 1) target = -dK;
            s->v += (target - s->v) * kd;
            int done = mode == 1 ? s->v < (dK > 0 ? 0.0f : 0.0005f) : dK > 0 ? s->v <= sus : pw_absf(s->v - sus) < 0.0005f;
            if (done && mode == 1) { s->v = 0; s->stage = loop && (s->gate_hi || !IN_CONNECTED(0)) ? ENV_ATT : ENV_IDLE; s->eoc = 441; }
            else if (done) { s->v = sus; s->stage = loop && (s->gate_hi || !IN_CONNECTED(0)) ? ENV_REL : ENV_SUS; if (s->stage == ENV_REL) s->vr = s->v > 0.0005f ? s->v : 0.0005f; }
            shaped = mode == 0 && pd > 1.0f && dd > 0.001f ? sus + dd * env_pow((s->v - sus) / dd, pd) : mode == 1 && pd > 1.0f ? env_pow(s->v, pd) : s->v;
        } break;
        case ENV_SUS:
            s->v += ((mode == 2 ? 1.0f : sus) - s->v) * 0.002f;
            if (loop && mode == 2 && (s->gate_hi || !IN_CONNECTED(0))) { s->stage = ENV_REL; s->vr = s->v; }
            shaped = s->v;
            break;
        case ENV_REL:
            s->v -= (s->v + rK * s->vr) * kr;
            if (s->v < (rK > 0 ? 0.0f : 0.0005f)) {
                s->v = 0; s->eoc = 441;
                s->stage = loop && (s->gate_hi || !IN_CONNECTED(0)) ? ENV_ATT : ENV_IDLE;
            }
            shaped = pr > 1.0f && s->vr > 0 ? s->vr * env_pow(s->v / s->vr, pr) : s->v;
            break;
        default:
            if (loop && !IN_CONNECTED(0)) { s->stage = ENV_ATT; if (!s->vel) s->vel = 1.0f; }
            break;
        }
        if (bent) s->y += (shaped - s->y) * 0.1f; else s->y = shaped;
        float sc = 1.0f - vamt * (1.0f - s->vel);
        out[0][i] = 5.0f * s->y * sc;
        out[1][i] = 5.0f - 5.0f * s->y * sc;
        out[2][i] = s->eoc ? 5.0f : 0.0f;
        if (s->eoc) --s->eoc;
    }
}

typedef struct { float r, outv; uint8_t stage, trig_hi; uint16_t eoc; } func_st;
PW_INLINE float func_shape(float r, float c)
{
    if (c >= 0) return pw_lerp(r, r * r * r, c);
    float q = 1.0f - r;
    return pw_lerp(r, 1.0f - q * q * q, -c);
}
PROC_COLD(func_proc)
{
    ST(func_st);
    float up = 1.0f / (pw_maxf(m->p[0], 0.0005f) * PW_SRF), down = 1.0f / (pw_maxf(m->p[1], 0.0005f) * PW_SRF);
    float curve = m->p[2];
    int cycle = m->p[3] > 0.5f;
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(1) && pw_rise(&s->trig_hi, in[1][i])) {
            if (s->stage == 0) s->r = pw_clamp(s->outv * 0.2f, 0.0f, 1.0f);
            s->stage = 1;
        }
        if (s->stage == 0 && cycle && !IN_CONNECTED(0)) s->stage = 1;
        if (s->stage == 1) {
            s->r += up;
            if (s->r >= 1.0f) { s->r = 1.0f; s->stage = 2; }
            s->outv = 5.0f * func_shape(s->r, curve);
        } else if (s->stage == 2) {
            s->r -= down;
            if (s->r <= 0) { s->r = 0; s->stage = cycle ? 1 : 0; s->eoc = 441; }
            s->outv = 5.0f * func_shape(s->r, curve);
        } else if (IN_CONNECTED(0)) {
            float target = in[0][i], d = target - s->outv;
            float lim = d > 0 ? up * 5.0f : down * 5.0f;
            s->outv += pw_clamp(d, -lim, lim);
        }
        out[0][i] = s->outv;
        out[1][i] = s->stage == 1 ? 0.0f : 5.0f;
        out[2][i] = s->eoc ? 5.0f : 0.0f;
        if (s->eoc) --s->eoc;
    }
}

typedef struct { float held, smooth, ph; uint32_t seed; uint8_t trig_hi, gate; } sh_st;
static void sh_init(pw_module *m) { ST(sh_st); s->seed = 0xabcdef1u ^ (uint32_t)(m - pws.mod) * 2654435761u; }
PROC_COLD(sh_proc)
{
    ST(sh_st);
    float k = 1.0f - pw_coef(m->p[0] * 0.25f), range = m->p[1], prob = m->p[2], rate = m->p[3];
    { float sh = sync_hz(m->p[4]); if (sh > 0) rate = sh; }
    for (int i = 0; i < PW_N; ++i) {
        int tick, level;
        if (IN_CONNECTED(1)) {
            tick = pw_rise(&s->trig_hi, in[1][i]);
            level = s->trig_hi;
        } else {
            s->ph += rate * PW_INV_SR;
            tick = s->ph >= 1.0f;
            if (tick) s->ph -= 1.0f;
            level = s->ph < 0.5f;
        }
        if (tick) {
            s->held = IN_CONNECTED(0) ? in[0][i] : (pw_rand_f(&s->seed) * 0.5f + 0.5f) * range;
            s->gate = (pw_rand_f(&s->seed) * 0.5f + 0.5f) < prob;
        }
        s->smooth = k >= 1.0f ? s->held : s->smooth + (s->held - s->smooth) * k;
        out[0][i] = s->held;
        out[1][i] = s->smooth;
        out[2][i] = (s->gate && level) ? 5.0f : 0.0f;
    }
}

/* -------------------------------------------------------------- utilities */
/* ATTENUVERTER: each side is gain and offset; before them, RECT (off / half: the negative half cut / full: flipped up) and CURVE (bends a
 * 5 V swing: above 0 it sags at first then climbs, below 0 it climbs at first then eases; 0 is straight), both sides alike */
PROC_COLD(atten_proc)
{
    (void)connected;
    int rect = (int)(m->p[5] + 0.5f);
    float cv = m->p[4], e = cv == 0 ? 1.0f : pw_exp2(cv * 2.0f);
    for (unsigned c = 0; c < 2; ++c) {
        float g = m->p[2 * c], o = m->p[2 * c + 1];
        for (int i = 0; i < PW_N; ++i) {
            float x = in[c][i];
            if (rect) x = rect == 1 ? pw_maxf(x, 0.0f) : pw_absf(x);
            if (cv != 0) {
                float u = pw_absf(x) * 0.2f;
                if (u > 0.0001f) { u = pw_exp2(e * pw_log2(u)) * 5.0f; x = x < 0 ? -u : u; }
            }
            out[c][i] = x * g + o;
        }
    }
}

typedef struct { float g[4]; } mix_st;
PROC_COLD(cvmix_proc)
{
    (void)connected;
    for (int i = 0; i < PW_N; ++i) {
        float sum = in[0][i] * m->p[0] + in[1][i] * m->p[1] + in[2][i] * m->p[2] + in[3][i] * m->p[3] + m->p[4];
        out[0][i] = sum;
        out[1][i] = -sum;
    }
}

typedef struct { uint8_t a, b; } logic_st;
PROC_COLD(logic_proc)
{
    ST(logic_st);
    float th = m->p[0];
    for (int i = 0; i < PW_N; ++i) {
        float a = in[0][i], b = in[1][i];
        if (s->a) { if (a < th - 0.1f) s->a = 0; } else if (a > th + 0.1f) s->a = 1;
        if (s->b) { if (b < th - 0.1f) s->b = 0; } else if (b > th + 0.1f) s->b = 1;
        out[0][i] = (s->a && s->b) ? 5.0f : 0.0f;
        out[1][i] = (s->a || s->b) ? 5.0f : 0.0f;
        out[2][i] = (s->a != s->b) ? 5.0f : 0.0f;
        out[3][i] = s->a ? 0.0f : 5.0f;
        out[4][i] = (IN_CONNECTED(1) ? a > b : a > th) ? 5.0f : 0.0f;
        out[5][i] = a < b ? a : b;
        out[6][i] = a > b ? a : b;
        out[7][i] = pw_absf(a);
    }
}

/* COMPARE: two signals (CV or audio) in, a gate out. A is compared with B + THRESH (B unpatched: with THRESH alone); HYST keeps a noisy
 * signal from chattering; MODE: A>B, A<B, WINDOW (on while A is within THRESH of B) or CROSS (a pulse each time A crosses B, either way).
 * PULSE 0 makes the gate follow the comparison; above 0 it is a trigger of that length on each turn-on (CROSS: at least 5 ms). */
typedef struct { uint8_t on, side; uint32_t left; } cmp_st;
PROC_COLD(compare_proc)
{
    ST(cmp_st);
    float th = m->p[0], hy = m->p[1] * 0.5f;
    int mode = (int)(m->p[2] + 0.5f);
    uint32_t plen = (uint32_t)(m->p[3] * (float)PW_SR);
    if (mode == 3 && plen < (uint32_t)(0.005f * (float)PW_SR)) plen = (uint32_t)(0.005f * (float)PW_SR);
    int b_on = IN_CONNECTED(1);
    for (int i = 0; i < PW_N; ++i) {
        float d = in[0][i] - (b_on ? in[1][i] : 0.0f), x;
        int was = s->on, gate;
        if (mode == 3) {
            int side = d > th + hy ? 1 : d < th - hy ? 0 : s->side;
            if (side != s->side) { s->side = (uint8_t)side; s->left = plen; }
            gate = s->left > 0;
            if (s->left) --s->left;
            s->on = (uint8_t)gate;
        } else {
            x = mode == 0 ? d - th : mode == 1 ? -d - th : pw_absf(th) - pw_absf(d);
            s->on = (uint8_t)(was ? x > -hy : x > hy);
            if (plen) {
                if (s->on && !was) s->left = plen;
                gate = s->left > 0;
                if (s->left) --s->left;
            } else gate = s->on;
        }
        out[0][i] = gate ? 5.0f : 0.0f;
        out[1][i] = gate ? 0.0f : 5.0f;
        out[2][i] = d;
    }
}

/* SHIFT REG: each CLOCK pulse takes IN (or, mixed in by FEEDBACK, the value that fell off the end, so the pattern goes round) and shifts every
 * stored value one place along a register LENGTH long. MUTATE adds a random nudge to what goes in; RESET empties it. NOW is the newest value,
 * -1 -2 -3 the ones before it, END the oldest; MIX is NOW (MIX 0) to END (1): a canon against yourself. */
typedef struct { float r[16]; uint8_t clk_hi, rst_hi; uint32_t seed; } shr_st;
static void shr_init(pw_module *m) { ST(shr_st); s->seed = 0x1234567u ^ (uint32_t)(m - pws.mod) * 2654435761u; }
PROC_COLD(shr_proc)
{
    ST(shr_st);
    unsigned len = (unsigned)(m->p[0] + 0.5f);
    if (len < 2) len = 2;
    if (len > 16) len = 16;
    float fb = m->p[1], mut = m->p[2] * 5.0f, mix = m->p[3];
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(2) && pw_rise(&s->rst_hi, in[2][i])) for (unsigned k = 0; k < 16; ++k) s->r[k] = 0;
        if (IN_CONNECTED(1) && pw_rise(&s->clk_hi, in[1][i])) {
            float end = s->r[len - 1], v = IN_CONNECTED(0) ? in[0][i] * (1.0f - fb) + end * fb : end * fb;
            if (mut > 0) v += pw_rand_f(&s->seed) * mut;
            for (unsigned k = 15; k > 0; --k) s->r[k] = s->r[k - 1];
            s->r[0] = pw_clamp(v, -10.0f, 10.0f);
            for (unsigned k = len; k < 16; ++k) s->r[k] = 0;
        }
        float now = s->r[0], endv = s->r[len - 1];
        out[0][i] = now;
        out[1][i] = s->r[1];
        out[2][i] = s->r[2 < len ? 2 : len - 1];
        out[3][i] = s->r[3 < len ? 3 : len - 1];
        out[4][i] = endv;
        out[5][i] = now + (endv - now) * mix;
    }
}

typedef struct { int pos, dir; float g[4]; uint8_t clk_hi, rst_hi; uint32_t seed; } sw_st;
PROC_COLD(switch_proc)
{
    ST(sw_st);
    int steps = (int)(m->p[0] + 0.5f), mode = (int)(m->p[1] + 0.5f), offset = (int)(m->p[2] + 0.5f) - 1;
    if (steps < 1) steps = 1;
    if (steps > 4) steps = 4;
    float k = 1.0f - pw_coef(pw_maxf(m->p[3], 0.0005f));
    if (!s->dir) s->dir = 1;
    if (s->pos >= steps) s->pos = 0;
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(5) && pw_rise(&s->rst_hi, in[5][i])) s->pos = 0;
        if (IN_CONNECTED(4) && pw_rise(&s->clk_hi, in[4][i])) {
            switch (mode) {
            case 0: s->pos = (s->pos + 1) % steps; break;
            case 1: s->pos = (s->pos + steps - 1) % steps; break;
            case 2:
                if (steps == 1) s->pos = 0;
                else {
                    if (s->pos + s->dir >= steps || s->pos + s->dir < 0) s->dir = -s->dir;
                    s->pos += s->dir;
                }
                break;
            default: s->pos = (int)(pw_rand_u(&s->seed) % (uint32_t)steps); break;
            }
        }
        int sel = ((IN_CONNECTED(4) ? s->pos : 0) + offset) % steps;
        if (sel < 0) sel = 0;
        float sum = 0;
        for (int c = 0; c < 4; ++c) {
            s->g[c] += ((c == sel ? 1.0f : 0.0f) - s->g[c]) * k;
            sum += in[c][i] * s->g[c];
            out[1 + c][i] = in[0][i] * s->g[c];
        }
        out[0][i] = sum;
    }
}

static const uint16_t scale_mask[] PW_DSP_DATA = {
    0xfff, 0xab5, 0x5ad, 0x6ad, 0x5ab, 0xad5, 0x6b5, 0x295, 0x4a9, 0x4e9, 0x9ad, 0x555};
static const char *const scale_names[] = {
    "CHROM", "MAJOR", "MINOR", "DORIAN", "PHRYG", "LYDIAN", "MIXO", "PENT MA", "PENT MI",
    "BLUES", "HARM MI", "WHOLE"};
static const char *const note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
typedef struct { float out, last_in; int note; uint16_t pulse; uint8_t trig_hi, valid; } q_st;
static PW_DSP float quantize(float v, unsigned mask, int root, int *note)
{
    float semis = v * 12.0f - (float)root;
    int base = pw_floori(semis + 0.5f), best = base;
    float bestd = 1e9f;
    for (int d = -6; d <= 6; ++d) {
        int c = base + d, deg = ((c % 12) + 12) % 12;
        if (!((mask >> deg) & 1U)) continue;
        float dist = pw_absf(semis - (float)c);
        if (dist < bestd) { bestd = dist; best = c; }
    }
    *note = best + root;
    return (float)(best + root) / 12.0f;
}
PROC_COLD(quant_proc)
{
    ST(q_st);
    int sc = (int)(m->p[0] + 0.5f); if (sc < 0 || sc > 11) sc = 0;
    unsigned mask = scale_mask[sc];
    int root = (int)(m->p[1] + 0.5f);
    float oct = (float)(int)(m->p[2] + (m->p[2] < 0 ? -0.5f : 0.5f));
    for (int i = 0; i < PW_N; ++i) {
        int sample = IN_CONNECTED(1) ? pw_rise(&s->trig_hi, in[1][i]) : 1;
        if (sample && (!s->valid || in[0][i] != s->last_in)) {
            int note;
            s->out = quantize(in[0][i], mask, root, &note);
            s->last_in = in[0][i];
            if (!s->valid || note != s->note) { s->note = note; s->pulse = 88; }
            s->valid = 1;
        }
        out[0][i] = s->out + oct;
        out[1][i] = s->pulse ? 5.0f : 0.0f;
        if (s->pulse) --s->pulse;
    }
}

/* -------------------------------------------------------- sequencing/clock */
/* MOD SEQ's lanes: lane L step S is six bits at bit (L*16+S)*6 of data[0..47] */
unsigned pw_mseq_get(const pw_module *m, unsigned lane, unsigned step)
{
    unsigned bit = ((lane & 3u) * 16u + (step & 15u)) * 6u, at = bit / 8u, sh = bit % 8u;
    unsigned w = m->data[at] | (unsigned)m->data[at + 1u] << 8;
    return ((w >> sh) & 63u) * 255u / 63u;
}
void pw_mseq_set(pw_module *m, unsigned lane, unsigned step, int v)
{
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    unsigned q = ((unsigned)v * 63u + 127u) / 255u;
    unsigned bit = ((lane & 3u) * 16u + (step & 15u)) * 6u, at = bit / 8u, sh = bit % 8u;
    unsigned w = m->data[at] | (unsigned)m->data[at + 1u] << 8;
    w = (w & ~(63u << sh)) | (q << sh);
    m->data[at] = (uint8_t)w; m->data[at + 1u] = (uint8_t)(w >> 8);
}
/* a SEQUENCER's step features and velocities, to their defaults (a new one) */
void pw_seq_upgrade(uint8_t *data)
{
    for (unsigned k = 0; k < 16; ++k) { data[PW_SEQ_STEPFEAT + k] = 0xF0; data[PW_SEQ_STEPVEL + k] = 200; }
    for (unsigned k = 50; k < PW_DATA_BYTES; ++k) data[k] = 0;
}
/* DIV: 1 and up divide the clock (1, /2, /3...); 0 and below multiply it (x2 at 0, x3 at -1 ... x8 at -6): the
 * extra steps of a multiplied clock follow the tick at equal gaps */
typedef struct { float gap, t; uint8_t rem; } subclk;
PW_INLINE void div_split(float p, int *div, int *mult)
{
    int v = (int)(p + (p < 0 ? -0.5f : 0.5f));
    if (v >= 1) { *div = v; *mult = 1; } else { *div = 1; *mult = 2 - v; if (*mult > 8) *mult = 8; }
}
PW_INLINE void subclk_arm(subclk *c, float period, int mult) { c->rem = (uint8_t)(mult - 1); c->gap = period / (float)mult; c->t = c->gap; }
PW_INLINE int subclk_step(subclk *c)
{
    if (c->rem && (c->t -= 1.0f) <= 0.0f) { c->t += c->gap; --c->rem; return 1; }
    return 0;
}
/* SEQUENCER: 16 steps of note, gate, ratchet (1-4 hits), chance and velocity; swing and clock division; and a
 * MONITOR that sounds the key you are recording. A step can also be a TIE (the gate of the step before runs on into it with no new trigger: a held
 * note, or a legato change of pitch) or a GLIDE (the same, and the pitch slides into the note: a 303's slide; 60 ms, or SLIDE if that is longer).
 * They are bits 2 and 3 of the step's feature byte (1 tie, 2 glide); the step before holds its gate on when it sees one coming. */
typedef struct {
    int step, dir; float period, since, gate_left, pitch, target, vel, swing_wait, sub_t, sub_len, gate_len;
    uint16_t trig, eos; uint8_t clk_hi, rst_hi, reset, divc, parity, swing_pend, ratch_left, mon_ser, held, gl; int8_t nxt; uint32_t seed; subclk sc;
} seq_st;
static void seq_init(pw_module *m)
{
    static const int8_t melody[16] = {0, 0, 12, 0, 3, 0, 7, 10, 0, 0, 12, 0, 15, 12, 7, 3};
    for (int k = 0; k < 16; ++k) m->data[k] = (uint8_t)melody[k];
    m->data[16] = 0xff; m->data[17] = 0xff;
    pw_seq_upgrade(m->data);
    ST(seq_st);
    s->step = -1; s->dir = 1; s->seed = 0x77u; s->nxt = -1;
}
PROC_COLD(seq_proc)
{
    ST(seq_st);
    int steps = (int)(m->p[0] + 0.5f), mode = (int)(m->p[2] + 0.5f), div, mult;
    div_split(m->p[5], &div, &mult);
    float gate0 = m->p[1], ks = 1.0f - pw_coef(m->p[3] * 0.3f), swing = m->p[4];
    float kg = 1.0f - pw_coef((m->p[3] > 0.06f ? m->p[3] : 0.06f) * 0.3f);   /* (a GLIDE step) */
    int mon_ok = m->p[6] > 0.5f && pws.mon_on && pws.mon_slot == (unsigned)(m - pws.mod);
    uint16_t gates = (uint16_t)(m->data[16] | (m->data[17] << 8));
    for (unsigned k = 0; k < 16; ++k) if (!m->data[PW_SEQ_STEPVEL + k]) gates = (uint16_t)(gates & ~(1u << k));   /* (velocity 0: the step is off) */
    if (div < 1) div = 1;
    if (pws.reset_now && !IN_CONNECTED(0)) s->reset = 1;
    if (s->period <= 0) s->period = PW_SRF * 15.0f / pws.bpm;
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(1) && pw_rise(&s->rst_hi, in[1][i])) s->reset = 1;
        int tick = IN_CONNECTED(0) ? pw_rise(&s->clk_hi, in[0][i]) : pws.tick_at[i];
        int fire = 0, due = 0;
        s->since += 1.0f;
        if (tick) {
            if (s->since > 64.0f && s->since < PW_SRF * 4.0f) s->period = s->since;
            if (!IN_CONNECTED(0)) s->period = PW_SRF * 15.0f / pws.bpm;
            s->since = 0;
            due = s->divc == 0;
            s->divc = (uint8_t)((s->divc + 1) % div);
            if (s->reset) { due = 1; s->divc = (uint8_t)(1 % div); s->parity = 0; }
            if (due && mult > 1) subclk_arm(&s->sc, s->period, mult);
        }
        if (subclk_step(&s->sc)) due = 1;
        if (due) {
            int odd = s->parity & 1;
            ++s->parity;
            if (swing > 0.004f && odd) { s->swing_pend = 1; s->swing_wait = swing * (mult > 1 ? s->period / (float)mult : s->period * (float)div); }
            else fire = 1;
        }
        if (s->swing_pend && (s->swing_wait -= 1.0f) <= 0.0f) { s->swing_pend = 0; fire = 1; }
        if (fire) {
            float step_len = mult > 1 ? s->period / (float)mult : s->period * (float)div;
            /* LENGTH patched: 0..5 V picks 1..16 steps; STEP patched: the voltage picks the step (addressed) */
            int n = IN_CONNECTED(3) ? 1 + (int)pw_clamp(in[3][i] * 3.2f, 0.0f, 15.0f) : steps;
            if (n < 1) n = 1;
            if (n > 16) n = 16;
            int prev = s->step;
            if (IN_CONNECTED(5)) s->step = (int)pw_clamp(in[5][i] * (float)n / 5.0f, 0.0f, (float)(n - 1));
            else if (s->reset || s->step < 0 || s->step >= n) { s->step = mode == 1 ? n - 1 : 0; s->dir = 1; s->reset = 0; }
            else switch (mode) {
                case 0: s->step = (s->step + 1) % n; break;
                case 1: s->step = (s->step + n - 1) % n; break;
                case 2:
                    if (n > 1) {
                        if (s->step + s->dir >= n || s->step + s->dir < 0) s->dir = -s->dir;
                        s->step += s->dir;
                    }
                    break;
                default: s->step = s->nxt >= 0 && s->nxt < n ? s->nxt : (int)(pw_rand_u(&s->seed) % (uint32_t)n); break;
            }
            int nx = -1;   /* the step that follows (a tie or glide there makes this gate run on): known ahead unless STEP is patched */
            if (!IN_CONNECTED(5)) switch (mode) {
                case 0: nx = (s->step + 1) % n; break;
                case 1: nx = (s->step + n - 1) % n; break;
                case 2: { int d = s->dir; if (n > 1) { if (s->step + d >= n || s->step + d < 0) d = -d; nx = s->step + d; } else nx = s->step; break; }
                default: s->nxt = (int8_t)(pw_rand_u(&s->seed) % (uint32_t)n); nx = s->nxt; break;
            }
            if (s->step == 0 && prev != 0) s->eos = 88;
            uint8_t fb = m->data[PW_SEQ_STEPFEAT + s->step];
            int leg = (fb >> 2) & 3, rat = leg ? 1 : (fb & 3) + 1, chance = fb >> 4, play = (gates >> s->step) & 1;
            if (play && chance < 15 && (int)(pw_rand_u(&s->seed) & 15u) > chance) play = 0;
            s->ratch_left = 0;
            int held = s->held && leg;   /* (the step before held its gate on for this one) */
            s->held = 0;
            if (play) {
                float gate = pw_clamp(gate0 + (IN_CONNECTED(4) ? in[4][i] * 0.2f : 0.0f), 0.02f, 1.0f);
                if (rat > 1) gate = pw_minf(gate, 0.6f);
                s->sub_len = step_len / (float)rat; s->gate_len = gate; s->sub_t = s->sub_len;
                s->gate_left = gate >= 0.98f ? step_len + 256.0f : pw_maxf(gate * s->sub_len, 64.0f);
                if (nx >= 0 && ((m->data[PW_SEQ_STEPFEAT + nx] >> 2) & 3) && ((gates >> nx) & 1)) { s->gate_left = step_len * (1.0f + swing) + 256.0f; s->held = 1; }
                if (!held) s->trig = 88;
                s->ratch_left = (uint8_t)(rat - 1);
                s->target = (float)(int8_t)m->data[s->step] / 12.0f;
                if (!(held && leg == 1)) s->vel = (float)m->data[PW_SEQ_STEPVEL + s->step] * (5.0f / 255.0f);
                s->gl = leg == 2;
            }
        }
        if (s->ratch_left && (s->sub_t -= 1.0f) <= 0.0f) {
            s->sub_t += s->sub_len; --s->ratch_left;
            s->gate_left = pw_maxf(s->gate_len * s->sub_len, 64.0f); s->trig = 88;
        }
        if (mon_ok) {   /* MONITOR: the key being recorded plays through the sequencer's own outputs */
            if (pws.mon_ser != s->mon_ser) { s->mon_ser = pws.mon_ser; s->trig = 88; }
            s->target = s->pitch = (float)pws.mon_note / 12.0f;
        } else { float kc = s->gl ? kg : ks; if (kc >= 1.0f) s->pitch = s->target; else s->pitch += (s->target - s->pitch) * kc; }
        out[0][i] = s->pitch + in[2][i];
        out[1][i] = (s->gate_left > 0 || mon_ok) ? 5.0f : 0.0f;
        out[2][i] = s->trig ? 5.0f : 0.0f;
        out[3][i] = s->eos ? 5.0f : 0.0f;
        out[4][i] = s->step < 0 ? 0.0f : (float)s->step * (5.0f / 16.0f);
        out[5][i] = s->vel;
        if (s->gate_left > 0) s->gate_left -= 1.0f;
        if (s->trig) --s->trig;
        if (s->eos) --s->eos;
    }
}

void pw_seq_monitor(unsigned slot, int note_rel, int on)
{
    if (on) { pws.mon_note = (int8_t)(note_rel < -36 ? -36 : note_rel > 36 ? 36 : note_rel); pws.mon_slot = (uint8_t)slot; pws.mon_ser++; pws.mon_on = 1; }
    else if (pws.mon_slot == slot) pws.mon_on = 0;
}

/* MOD SEQ: four lanes of 16 steps that move together on one clock but each with its own length (polymeter),
 * direction mode shared; a lane's value goes out as a voltage (RANGE), slewed, and by CHANCE may hold instead */
typedef struct {
    int pos[4], dir[4]; float v[4], tgt[4], period, since; uint16_t eos, trig[4];
    uint8_t clk_hi, rst_hi, reset, divc; uint32_t seed; subclk sc;
} mseq_st;
static void mseq_init(pw_module *m)
{
    for (unsigned k = 0; k < 16; ++k) {
        pw_mseq_set(m, 0, k, (int)(k * 17));                     /* A: a ramp up */
        pw_mseq_set(m, 1, k, (int)((15 - k) * 17));              /* B: a ramp down */
        pw_mseq_set(m, 2, k, (int)((k % 4) * 85));               /* C: a short saw, four steps */
        pw_mseq_set(m, 3, k, (k & 1) ? 255 : 0);                 /* D: every other step */
    }
    ST(mseq_st);
    for (int l = 0; l < 4; ++l) { s->pos[l] = -1; s->dir[l] = 1; }
    s->seed = 0xC0DEu + (uint32_t)(m - pws.mod) * 2654435761u;
}
PROC_COLD(mseq_proc)
{
    ST(mseq_st);
    int mode = (int)(m->p[4] + 0.5f), div, mult, range = (int)(m->p[7] + 0.5f);
    div_split(m->p[5], &div, &mult);
    float ks = 1.0f - pw_coef(m->p[6] * 0.5f), chance = m->p[8];
    float lo = range == 1 ? -5.0f : 0.0f, span = range == 1 ? 10.0f : range == 2 ? 2.5f : 5.0f;
    if (div < 1) div = 1;
    if (pws.reset_now && !IN_CONNECTED(0)) s->reset = 1;
    if (s->period <= 0) s->period = PW_SRF * 15.0f / pws.bpm;
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(1) && pw_rise(&s->rst_hi, in[1][i])) s->reset = 1;
        int tick = IN_CONNECTED(0) ? pw_rise(&s->clk_hi, in[0][i]) : pws.tick_at[i], due = 0;
        s->since += 1.0f;
        if (tick) {
            if (s->since > 64.0f && s->since < PW_SRF * 4.0f) s->period = s->since;
            if (!IN_CONNECTED(0)) s->period = PW_SRF * 15.0f / pws.bpm;
            s->since = 0;
            due = s->divc == 0;
            s->divc = (uint8_t)((s->divc + 1) % div);
            if (s->reset) { due = 1; s->divc = (uint8_t)(1 % div); }
            if (due && mult > 1) subclk_arm(&s->sc, s->period, mult);
        }
        if (subclk_step(&s->sc)) due = 1;
        {
            if (due) {
                for (int l = 0; l < 4; ++l) {
                    int n = (int)(m->p[l] + 0.5f), prev = s->pos[l];
                    if (n < 1) n = 1;
                    if (n > 16) n = 16;
                    if (s->reset || s->pos[l] < 0 || s->pos[l] >= n) { s->pos[l] = mode == 1 ? n - 1 : 0; s->dir[l] = 1; }
                    else switch (mode) {
                        case 0: s->pos[l] = (s->pos[l] + 1) % n; break;
                        case 1: s->pos[l] = (s->pos[l] + n - 1) % n; break;
                        case 2:
                            if (n > 1) {
                                if (s->pos[l] + s->dir[l] >= n || s->pos[l] + s->dir[l] < 0) s->dir[l] = -s->dir[l];
                                s->pos[l] += s->dir[l];
                            }
                            break;
                        default: s->pos[l] = (int)(pw_rand_u(&s->seed) % (uint32_t)n); break;
                    }
                    if (l == 0 && s->pos[0] == 0 && prev != 0) s->eos = 88;
                    s->trig[l] = 88;
                    if (chance >= 0.995f || (float)(pw_rand_u(&s->seed) & 1023u) < chance * 1024.0f)
                        s->tgt[l] = lo + span * (float)pw_mseq_get(m, (unsigned)l, (unsigned)s->pos[l]) * (1.0f / 255.0f);
                }
                s->reset = 0;
            }
        }
        for (int l = 0; l < 4; ++l) {
            if (ks >= 1.0f) s->v[l] = s->tgt[l]; else s->v[l] += (s->tgt[l] - s->v[l]) * ks;
            out[l][i] = s->v[l];
        }
        out[4][i] = s->eos ? 5.0f : 0.0f;
        if (s->eos) --s->eos;
        for (int l = 0; l < 4; ++l) { out[5 + l][i] = s->trig[l] ? 5.0f : 0.0f; if (s->trig[l]) --s->trig[l]; }
    }
}

/* scale degrees and chords in the key */
static const char *const L_scale7[] = {"MAJOR", "MINOR", "DORIAN", "PHRYG", "LYDIAN", "MIXO", "HARM MI"};
static const uint16_t scale7_mask[7] PW_DSP_DATA = {0xab5, 0x5ad, 0x6ad, 0x5ab, 0xad5, 0x6b5, 0x9ad};
static PW_DSP int scale_semis(unsigned mask, int idx)   /* the idx-th note up a seven-note scale (any integer: octaves wrap) */
{
    int oct = idx >= 0 ? idx / 7 : -((-idx + 6) / 7), deg = idx - oct * 7, n = 0;
    for (int b = 0; b < 12; ++b) if ((mask >> b) & 1u) { if (n++ == deg) return b + 12 * oct; }
    return 12 * oct;
}
/* CHORD STEPS: a progression, one chord a step, always in the key. A step is a degree (I-VII), REST or TIE (the
 * chord holds), a type (triad, seventh, sus4, add9) and an inversion. Four pitches out for four oscillators, a
 * gate, and the root an octave down for a bass. */
typedef struct {
    int step, dir; float period, since, gate_left, pitch[4], root; uint16_t trig, eos;
    uint8_t clk_hi, rst_hi, reset, divc; uint32_t seed; subclk sc;
} cst_st;
static void cst_init(pw_module *m)
{
    static const uint8_t prog[16] = {1, 8, 6, 8, 4, 8, 5, 8, 1, 8, 6, 8, 4, 8, 5, 0};
    for (int k = 0; k < 16; ++k) m->data[k] = prog[k];
    ST(cst_st);
    s->step = -1; s->dir = 1; s->seed = 0x5EEDu;
}
PW_COLD static PW_DSP void cst_chord(const pw_module *m, uint8_t b, float pitch[4], float *root)
{
    unsigned mask = scale7_mask[(int)pw_clamp(m->p[2] + 0.5f, 0.0f, 6.0f)];
    int key = (int)(m->p[1] + 0.5f), oct = (int)(m->p[3] + (m->p[3] < 0 ? -0.5f : 0.5f));
    int d = (b & 15) - 1, type = (b >> 4) & 3, inv = b >> 6, iv[4];
    if (d < 0 || d > 6) d = 0;
    int r = scale_semis(mask, d);
    switch (type) {
        case 1: iv[0] = r; iv[1] = scale_semis(mask, d + 2); iv[2] = scale_semis(mask, d + 4); iv[3] = scale_semis(mask, d + 6); break;
        case 2: iv[0] = r; iv[1] = scale_semis(mask, d + 3); iv[2] = scale_semis(mask, d + 4); iv[3] = r + 12; break;
        case 3: iv[0] = r; iv[1] = scale_semis(mask, d + 2); iv[2] = scale_semis(mask, d + 4); iv[3] = scale_semis(mask, d + 8); break;
        default: iv[0] = r; iv[1] = scale_semis(mask, d + 2); iv[2] = scale_semis(mask, d + 4); iv[3] = r + 12; break;
    }
    for (int k = 0; k < inv; ++k) { int t = iv[0] + 12; iv[0] = iv[1]; iv[1] = iv[2]; iv[2] = iv[3]; iv[3] = t; }
    if (m->p[6] > 0.5f) { iv[1] += 12; iv[3] += 12; }
    for (int k = 0; k < 4; ++k) pitch[k] = (float)(iv[k] + key + 12 * oct) * (1.0f / 12.0f);
    *root = (float)(r + key + 12 * oct - 12) * (1.0f / 12.0f);
}
static const char *const chord_note[12] PW_DSP_DATA = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
PW_COLD void pw_chord_name(const pw_module *m, uint8_t b, char out[10])   /* "Am", "G7", "Bdim"... for a step */
{
    out[0] = 0;
    int d = b & 15;
    if (d < 1 || d > 7) { const char *t = d == 8 ? "TIE" : "REST"; int i = 0; while (t[i]) { out[i] = t[i]; ++i; } out[i] = 0; return; }
    float p[4], root; cst_chord(m, (uint8_t)(b & 0x3f), p, &root);
    unsigned mask = scale7_mask[(int)pw_clamp(m->p[2] + 0.5f, 0.0f, 6.0f)];
    int r = scale_semis(mask, d - 1), third = scale_semis(mask, d + 1) - r, fifth = scale_semis(mask, d + 3) - r, ty = (b >> 4) & 3;
    int pc = ((r + (int)(m->p[1] + 0.5f)) % 12 + 12) % 12, n = 0;
    const char *nm = chord_note[pc];
    while (*nm) out[n++] = *nm++;
    if (ty == 2) { out[n++] = 's'; out[n++] = 'u'; out[n++] = 's'; }
    else if (fifth == 6) { out[n++] = 'd'; out[n++] = 'i'; out[n++] = 'm'; }
    else if (third == 3) out[n++] = 'm';
    if (ty == 1) out[n++] = '7';
    else if (ty == 3) { out[n++] = '9'; }
    out[n] = 0;
}
int pw_chord_degree(const pw_module *m, int semis_from_c)   /* the degree (1-7) of the key's scale that a played note is nearest to */
{
    unsigned mask = scale7_mask[(int)pw_clamp(m->p[2] + 0.5f, 0.0f, 6.0f)];
    int pc = ((semis_from_c - (int)(m->p[1] + 0.5f)) % 12 + 12) % 12, best = 1, bd = 99;
    for (int d = 1; d <= 7; ++d) {
        int e = scale_semis(mask, d - 1), dist = e > pc ? e - pc : pc - e;
        if (12 - dist < dist) dist = 12 - dist;
        if (dist < bd) { bd = dist; best = d; }
    }
    return best;
}
int pw_seq_step(unsigned slot)   /* the step a module with steps is on, or -1 (the SEQUENCER, MOD SEQ's lane A, CHORD STEPS) */
{
    if (slot >= PW_MAX_MODULES || !pws.mod[slot].used) return -1;
    const void *st = pws.mod[slot].st.raw;
    switch (pws.mod[slot].type) {
    case PW_T_SEQ: return ((const seq_st *)st)->step;
    case PW_T_MODSEQ: return ((const mseq_st *)st)->pos[0];
    case PW_T_CHORDSTEPS: return ((const cst_st *)st)->step;
    default: return -1;
    }
}
void pw_kick(unsigned slot) { pws.kick_slot = (uint8_t)slot; pws.kick_ser++; }
PROC_COLD(cst_proc)
{
    ST(cst_st);
    int steps = (int)(m->p[0] + 0.5f), mode = (int)(m->p[5] + 0.5f), div, mult;
    float gate0 = m->p[4];
    div_split(m->p[7], &div, &mult);
    if (pws.reset_now && !IN_CONNECTED(0)) s->reset = 1;
    if (s->period <= 0) s->period = PW_SRF * 15.0f / pws.bpm;
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(1) && pw_rise(&s->rst_hi, in[1][i])) s->reset = 1;
        int tick = IN_CONNECTED(0) ? pw_rise(&s->clk_hi, in[0][i]) : pws.tick_at[i], due = 0;
        s->since += 1.0f;
        if (tick) {
            if (s->since > 64.0f && s->since < PW_SRF * 4.0f) s->period = s->since;
            if (!IN_CONNECTED(0)) s->period = PW_SRF * 15.0f / pws.bpm;
            s->since = 0;
            due = s->divc == 0;
            s->divc = (uint8_t)((s->divc + 1) % div);
            if (s->reset) { due = 1; s->divc = (uint8_t)(1 % div); }
            if (due && mult > 1) subclk_arm(&s->sc, s->period, mult);
        }
        if (subclk_step(&s->sc)) due = 1;
        {
            if (due) {
                int n = steps < 1 ? 1 : steps > 16 ? 16 : steps, prev = s->step;
                if (s->reset || s->step < 0 || s->step >= n) { s->step = mode == 1 ? n - 1 : 0; s->dir = 1; s->reset = 0; }
                else switch (mode) {
                    case 0: s->step = (s->step + 1) % n; break;
                    case 1: s->step = (s->step + n - 1) % n; break;
                    case 2:
                        if (n > 1) {
                            if (s->step + s->dir >= n || s->step + s->dir < 0) s->dir = -s->dir;
                            s->step += s->dir;
                        }
                        break;
                    default: s->step = (int)(pw_rand_u(&s->seed) % (uint32_t)n); break;
                }
                if (s->step == 0 && prev != 0) s->eos = 88;
                uint8_t b = m->data[s->step];
                float full = mult > 1 ? s->period / (float)mult : s->period * (float)div;
                if ((b & 15) == 8) { if (s->gate_left > 0) s->gate_left += full; }    /* TIE: the chord holds */
                else if ((b & 15) >= 1 && (b & 15) <= 7) {
                    cst_chord(m, b, s->pitch, &s->root);
                    s->gate_left = gate0 >= 0.98f ? full + 256.0f : pw_maxf(gate0 * full, 64.0f);
                    s->trig = 88;
                } else s->gate_left = 0;                                                  /* REST */
            }
        }
        for (int k = 0; k < 4; ++k) out[k][i] = s->pitch[k] + in[2][i];
        out[4][i] = s->gate_left > 0 ? 5.0f : 0.0f;
        out[5][i] = s->trig ? 5.0f : 0.0f;
        out[6][i] = s->root + in[2][i];
        out[7][i] = s->eos ? 5.0f : 0.0f;
        if (s->gate_left > 0) s->gate_left -= 1.0f;
        if (s->trig) --s->trig;
        if (s->eos) --s->eos;
    }
}

/* GRAVITY: a ball dropped onto a short list of notes. It falls (FALL, in sixteenths), hits the first note, bounces back
 * up lower by ELASTIC, plays the next, and so on: an accelerating rattle of notes that dies away, then is dropped
 * again every EVERY beats (or by a trigger in DROP). VEL is how hard it hit, HEIGHT the ball's height as a voltage. */
typedef struct {
    float t, interval, first, vel; int k, hit; uint8_t active, drop_hi; int ticks; uint32_t seed;
    float pitch, gate_left, el, rest; uint16_t trig, eos; uint8_t kick_ser;
} grav_st;
static void grav_init(pw_module *m)
{
    static const int8_t tune[16] = {0, 4, 7, 12, 9, 4, 2, 7, 12, 16, 14, 9, 7, 4, 2, 0};
    for (int k = 0; k < 16; ++k) m->data[k] = (uint8_t)tune[k];
    ST(grav_st);
    s->seed = 0x6A17u; s->hit = -1; s->rest = 1e7f;
}
PROC_COLD(grav_proc)
{
    ST(grav_st);
    int every = (int)(m->p[0] + 0.5f), n = (int)(m->p[3] + 0.5f), order = (int)(m->p[4] + 0.5f);
    int sc = (int)pw_clamp(m->p[5] + 0.5f, 0.0f, 11.0f), root = (int)(m->p[6] + 0.5f);
    float fall = m->p[1], e = m->p[2], gate = m->p[7], spread = m->p[8];
    float tp = PW_SRF * 15.0f / pws.bpm, t1 = fall * tp;
    int kicked = pws.kick_ser != s->kick_ser && pws.kick_slot == (unsigned)(m - pws.mod);
    if (pws.kick_ser != s->kick_ser) s->kick_ser = pws.kick_ser;
    if (every < 1) every = 1;
    if (n < 2) n = 2;
    if (n > 16) n = 16;
    for (int i = 0; i < PW_N; ++i) {
        int drop = 0;
        if (IN_CONNECTED(0)) drop = pw_rise(&s->drop_hi, in[0][i]);
        else {
            if (pws.reset_now && i == 0) { s->ticks = 0; drop = 1; }
            else if (pws.tick_at[i]) { if (++s->ticks >= every * 4) { s->ticks = 0; drop = 1; } }
        }
        if (kicked && i == 0) drop = 1;
        if (drop) { s->active = 1; s->el = 0; s->k = 0; s->t = 0; s->interval = t1; s->first = t1; s->vel = 1.0f; s->hit = -1; }
        float h = 0.0f;
        if (s->active) {
            s->t += 1.0f; s->el += 1.0f; s->rest = 0;
            if (s->t >= s->interval) {                          /* the ball lands: the next note */
                int k = s->k, p = 0;
                switch (order) {
                    case 0: p = k % n; break;
                    case 1: p = n - 1 - (k % n); break;
                    case 2: { int c = 2 * (n - 1), q = k % c; p = q < n ? q : c - q; } break;
                    default: p = (int)(pw_rand_u(&s->seed) % (uint32_t)n); break;
                }
                if (spread > 0.01f && (float)(pw_rand_u(&s->seed) & 1023u) < spread * 1024.0f) {
                    p += (pw_rand_u(&s->seed) & 1u) ? 1 : -1;
                    p = p < 0 ? 0 : p >= n ? n - 1 : p;
                }
                int note;
                s->pitch = quantize((float)(int8_t)m->data[p] / 12.0f, scale_mask[sc], root, &note);
                s->hit = p;
                float v = 1.0f; for (int j = 0; j < k; ++j) v *= e;
                s->vel = v;
                ++s->k;
                s->interval = 2.0f * t1 * v * e;                  /* up and down again, lower each time */
                s->t = 0;
                s->trig = 88;
                s->gate_left = pw_maxf(gate * s->interval, 64.0f);
                if (s->interval < 0.02f * PW_SRF) { s->active = 0; s->eos = 88; s->gate_left = pw_minf(s->gate_left, 400.0f); }
            } else {
                float x = s->t / s->interval;
                if (s->k == 0) h = 1.0f - x * x;
                else { float v = 1.0f; for (int j = 1; j < s->k; ++j) v *= e; h = 4.0f * x * (1.0f - x) * v * v; }
            }
        }
        if (!s->active && s->rest < 1e7f) s->rest += 1.0f;
        out[0][i] = s->pitch + in[1][i];
        out[1][i] = s->gate_left > 0 ? 5.0f : 0.0f;
        out[2][i] = s->trig ? 5.0f : 0.0f;
        out[3][i] = 5.0f * s->vel;
        out[4][i] = 5.0f * pw_clamp(h, 0.0f, 1.0f);
        out[5][i] = s->eos ? 5.0f : 0.0f;
        if (s->gate_left > 0) s->gate_left -= 1.0f;
        if (s->trig) --s->trig;
        if (s->eos) --s->eos;
    }
}

/* LISTENER: plays a melody out of whatever it hears. Each onset (a rise in loudness) picks a note from the sound at
 * that moment, a few milliseconds in: how BRIGHT it is, how LOUD, or its PITCH, quantized to a scale. */
static const char *const L_listen[] = {"BRIGHT", "LOUD", "PITCH"};
typedef struct {
    float lp, zl, ef, es, elp, ehp, per, peak, pitch, level; int since_zc, cool, cap; uint8_t zpos, above, gate; uint16_t trig;
} lis_st;
PROC_COLD(listen_proc)
{
    ST(lis_st);
    (void)connected;
    float th = m->p[0], ratio = 1.0f + m->p[1];
    int mode = (int)(m->p[2] + 0.5f), sc = (int)pw_clamp(m->p[3] + 0.5f, 0.0f, 11.0f), root = (int)(m->p[4] + 0.5f);
    float range = m->p[5], oct = (float)(int)(m->p[6] + (m->p[6] < 0 ? -0.5f : 0.5f));
    int gap = (int)(m->p[7] * PW_SRF);
    for (int i = 0; i < PW_N; ++i) {
        float x = in[0][i] * 0.2f, a = pw_absf(x);
        s->ef += (a - s->ef) * (a > s->ef ? 0.1f : 0.002f);
        s->es += (a - s->es) * (a > s->es ? 0.0006f : 0.0002f);
        s->lp += (x - s->lp) * 0.08f;
        float hp = x - s->lp;
        s->elp += (pw_absf(s->lp) - s->elp) * 0.01f;
        s->ehp += (pw_absf(hp) - s->ehp) * 0.01f;
        s->zl += (x - s->zl) * 0.1f;
        ++s->since_zc;
        if (!s->zpos && s->zl > 0.15f * s->ef + 1e-4f) {
            s->zpos = 1;
            if (s->since_zc > (int)(PW_SRF / 1500.0f) && s->since_zc < (int)(PW_SRF / 40.0f)) s->per += ((float)s->since_zc - s->per) * (s->per > 0 ? 0.4f : 1.0f);
            s->since_zc = 0;
        } else if (s->zpos && s->zl < -0.15f * s->ef - 1e-4f) s->zpos = 0;
        if (s->cool > 0) --s->cool;
        if (s->ef > th && !s->above && s->cool == 0 && s->ef > s->es * ratio) {
            s->above = 1; s->gate = 1; s->trig = 88; s->cap = (int)((mode == 2 ? 0.03f : 0.008f) * PW_SRF); s->cool = gap; s->peak = 0; s->per = 0;
        }
        if (s->above) {
            if (s->ef > s->peak) s->peak = s->ef;
            if (s->ef < th * 0.5f || s->ef < s->es * 1.05f) { s->above = 0; s->gate = 0; }
            if (s->cap > 0 && --s->cap == 0) {          /* a few milliseconds in: what the sound is */
                float volts, semis = 0.0f;
                if (mode == 2 && s->per > 0) volts = pw_log2(PW_SRF / s->per / PW_C4);
                else {
                    float t = mode == 1 ? pw_clamp(s->peak * 2.0f, 0.0f, 1.0f)
                                        : pw_clamp((s->ehp / (s->ehp + s->elp + 1e-6f) - 0.05f) * 1.6f, 0.0f, 1.0f);
                    semis = t * range * 12.0f; volts = semis / 12.0f;
                }
                int note;
                s->pitch = quantize(volts, scale_mask[sc], root, &note);
                s->level = pw_clamp(s->peak * 2.0f, 0.0f, 1.0f);
            }
        }
        out[0][i] = s->pitch + oct;
        out[1][i] = s->gate ? 5.0f : 0.0f;
        out[2][i] = s->trig ? 5.0f : 0.0f;
        out[3][i] = 5.0f * s->level;
        if (s->trig) --s->trig;
    }
    if (s->ef != s->ef) s->ef = 0;
}

/* ------------------------------------------------------------------ I/O */
typedef struct { float g; } out_st;
PROC(output_proc)
{
    ST(out_st);
    float g = s->g, step = (m->p[0] - g) * (1.0f / PW_N);
    const float *r = IN_CONNECTED(1) ? in[1] : in[0];
    (void)out;
    for (int i = 0; i < PW_N; ++i) {
        g += step;
        pw_mix_l[i] += in[0][i] * g;
        pw_mix_r[i] += r[i] * g;
    }
    s->g = m->p[0];
}

typedef struct { float pitch, press; uint32_t serial; uint16_t trig; } keys_st;
PROC_COLD(keys_proc)
{
    ST(keys_st);
    (void)in; (void)connected;
    float target = (pws.kb_pitch_note - 60.0f) / 12.0f + (float)(int)(m->p[1] + (m->p[1] < 0 ? -0.5f : 0.5f)) +
                   pws.kb_bend * m->p[2] / 12.0f;
    if (s->serial != pws.kb_serial) { s->serial = pws.kb_serial; s->trig = 88; }
    float k = 1.0f - pw_coef(m->p[0] * 0.3f);
    /* VEL CURVE: - softer playing comes out louder, + it takes a harder hit; PRESS: how far a lean goes */
    float kv = pws.kb_vel;
    switch (pws.key_sens) {   /* the KEY SENSITIVITY setting: how hard a key must be hit */
    case 0: kv = 0.787f; break;
    case 1: kv = kv * kv; break;
    case 3: kv = 1.0f - (1.0f - kv) * (1.0f - kv); break;
    default: break;
    }
    float vc = m->p[3], vel = kv > 0 ? 5.0f * pw_exp2(pw_exp2(vc * 1.5f) * pw_log2(kv)) : 0.0f;
    float ptarget = pw_minf(1.0f, pws.kb_press * pw_exp2(m->p[4] * 4.0f - 2.0f)) * 5.0f;
    for (int i = 0; i < PW_N; ++i) {
        s->pitch = k >= 1.0f ? target : s->pitch + (target - s->pitch) * k;
        out[0][i] = s->pitch;
        out[1][i] = pws.kb_gate ? 5.0f : 0.0f;
        out[2][i] = vel;
        out[3][i] = s->trig ? 5.0f : 0.0f;
        out[4][i] = pws.kb_mod * 5.0f;
        s->press += (ptarget - s->press) * 0.003f;
        out[5][i] = s->press;
        if (s->trig) --s->trig;
    }
}


/* PIANO ROLL: notes on a grid of 16ths, up to four sounding at once. Every note has a voice (0-3, given when it is made, so two never
 * share one while they overlap); the module is in the voice area, each voice reads its own notes and the cables carry one PITCH,
 * GATE and VEL for all four (as KEYS does). The place in the loop is the transport's 16ths, so every voice agrees without talking. */
static const char *const L_snap[] = {"1/16", "1/8", "1/4", "BAR"};
#define RN(m) ((m)->data[0])
#define RB(m, i) (&(m)->data[2 + 4 * (i)])
unsigned pw_roll_count(const pw_module *m) { return RN(m) > PW_ROLL_MAX ? PW_ROLL_MAX : RN(m); }
unsigned pw_roll_loop(const pw_module *m) { int b = (int)(m->p[0] + 0.5f); return (unsigned)(b < 1 ? 1 : b > 4 ? 4 : b) * 16u; }
int pw_roll_get(const pw_module *m, unsigned i, unsigned *start, unsigned *len, unsigned *pitch, unsigned *vel, unsigned *voice)
{
    if (i >= pw_roll_count(m)) return 0;
    const uint8_t *b = RB(m, i);
    if (start) *start = b[0]; if (len) *len = b[1]; if (pitch) *pitch = b[2]; if (vel) *vel = b[3] >> 2; if (voice) *voice = b[3] & 3u;
    return 1;
}
int pw_roll_find(const pw_module *m, unsigned t, unsigned pitch)
{
    for (unsigned i = 0; i < pw_roll_count(m); ++i) { const uint8_t *b = RB(m, i); if (b[2] == pitch && t >= b[0] && t < (unsigned)b[0] + b[1]) return (int)i; }
    return -1;
}
static int roll_voice(const pw_module *m, int skip, unsigned start, unsigned len, unsigned pitch)   /* the lowest voice free over that span (-1: none, or the same pitch is already sounding there) */
{
    unsigned used = 0;
    for (unsigned i = 0; i < pw_roll_count(m); ++i) {
        if ((int)i == skip) continue;
        const uint8_t *b = RB(m, i);
        if (start < (unsigned)b[0] + b[1] && b[0] < start + len) { if (b[2] == pitch) return -1; used |= 1u << (b[3] & 3u); }
    }
    for (int v = 0; v < 4; ++v) if (!(used >> v & 1u)) return v;
    return -1;
}
int pw_roll_add(pw_module *m, unsigned start, unsigned len, unsigned pitch, unsigned vel)
{
    unsigned n = pw_roll_count(m);
    if (n >= PW_ROLL_MAX || start > 63 || pitch > 127) return -1;
    if (len < 1) len = 1;
    if (start + len > 64) len = 64 - start;
    int v = roll_voice(m, -1, start, len, pitch);
    if (v < 0) return -1;
    uint8_t *b = RB(m, n);
    b[0] = (uint8_t)start; b[1] = (uint8_t)len; b[2] = (uint8_t)pitch; b[3] = (uint8_t)((vel > 31 ? 31 : vel) << 2 | (unsigned)v);
    RN(m) = (uint8_t)(n + 1);
    return (int)n;
}
void pw_roll_del(pw_module *m, unsigned i)
{
    unsigned n = pw_roll_count(m);
    if (i >= n) return;
    for (unsigned k = i; k + 1 < n; ++k) memcpy(RB(m, k), RB(m, k + 1), 4);
    memset(RB(m, n - 1), 0, 4);
    RN(m) = (uint8_t)(n - 1);
}
int pw_roll_edit(pw_module *m, unsigned i, int start, int len, int vel)
{
    if (i >= pw_roll_count(m)) return -1;
    uint8_t *b = RB(m, i);
    int s = start < 0 ? b[0] : start, l = len < 0 ? b[1] : len, q = vel < 0 ? (b[3] >> 2) : vel;
    if (s < 0) s = 0; if (s > 63) s = 63; if (l < 1) l = 1; if (s + l > 64) l = 64 - s; if (q > 31) q = 31;
    int v = roll_voice(m, (int)i, (unsigned)s, (unsigned)l, b[2]);
    if (v < 0) return -1;
    b[0] = (uint8_t)s; b[1] = (uint8_t)l; b[3] = (uint8_t)(q << 2 | v);
    return 0;
}
int pw_roll_pitch(pw_module *m, unsigned i, unsigned pitch)
{
    if (i >= pw_roll_count(m) || pitch > 127) return -1;
    uint8_t *b = RB(m, i);
    if (roll_voice(m, (int)i, b[0], b[1], pitch) < 0) return -1;
    b[2] = (uint8_t)pitch;
    return 0;
}
void pw_roll_clear(pw_module *m) { memset(m->data, 0, PW_ROLL_BYTES); }
typedef struct { uint32_t serial; uint16_t trig, gap; int16_t nstart; uint8_t valid, gate, was; float pitch, vel; } roll_st;
PROC_COLD(roll_proc)
{
    ST(roll_st);
    (void)in; (void)connected;
    unsigned v = pw_cur_voice(), loop = pw_roll_loop(m), ticks = 0;
    float tr = (float)(int)(m->p[2] + (m->p[2] < 0 ? -0.5f : 0.5f)) / 12.0f;
    for (int i = 0; i < PW_N; ++i) ticks += pws.tick_at[i] ? 1u : 0u;
    unsigned cur = pws.sixteenths - ticks;
    if (!pws.playing) { s->valid = 0; s->gate = 0; s->nstart = 0; }
    for (int i = 0; i < PW_N; ++i) {
        int tick = pws.tick_at[i];
        if (tick) ++cur;
        if (pws.playing && (tick || !s->valid)) {
            s->valid = 1;
            unsigned step = (cur + loop - 1u) % loop;   /* (the count includes the sixteenth that just began: the first one is step 0) */
            int hit = -1;
            for (unsigned k = 0; k < pw_roll_count(m); ++k) {
                const uint8_t *b = RB(m, k);
                if ((b[3] & 3u) == v && step >= b[0] && step < (unsigned)b[0] + b[1]) { hit = (int)k; break; }
            }
            if (hit < 0) { s->gate = 0; s->nstart = 0; }
            else {
                const uint8_t *b = RB(m, (unsigned)hit);
                if (b[0] + 1 != s->nstart || b[0] == step) {   /* a note begins (nstart is its start + 1; 0 none, so a start in mid-note sounds too) */
                    if (b[0] == step || s->nstart == 0) {
                        if (s->gate) s->gap = 88;
                        s->trig = 88; ++s->serial;
                    }
                    s->nstart = (int16_t)(b[0] + 1);
                }
                s->pitch = (float)b[2] - 60.0f; s->vel = (float)((b[3] >> 2) + 1) * (5.0f / 32.0f);   /* (every step: a note edited under the playhead follows) */
                s->gate = 1;
            }
        }
        out[0][i] = s->pitch * (1.0f / 12.0f) + tr;   /* (pitch is semitones from C4, so a voice that has played nothing sits at 0 V) */
        out[1][i] = s->gate && !s->gap ? 5.0f : 0.0f;
        out[2][i] = s->vel;
        out[3][i] = s->trig ? 5.0f : 0.0f;
        if (s->trig) --s->trig;
        if (s->gap) --s->gap;
    }
}

/* =============================================================== more modules */
/* a triangle LFO, 0..1 phase in, -1..1 out */
PW_INLINE float tri1(float ph) { return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; }
/* a delay line of int16 in the pool, read at a fractional distance behind the write head */
#define DL_SCALE 3000.0f
PW_INLINE float dl_read(const int16_t *b, uint32_t len, uint32_t w, float dist)
{
    float rp = (float)w - dist;
    while (rp < 0) rp += (float)len;
    uint32_t i0 = (uint32_t)rp, i1 = i0 + 1u >= len ? 0 : i0 + 1u;
    float fr = rp - (float)i0;
    if (i0 >= len) i0 = 0;
    return (b[i0] + (b[i1] - b[i0]) * fr) * (1.0f / DL_SCALE);
}
PW_INLINE float dl_read_f(const float *b, uint32_t len, uint32_t w, float dist)
{
    float rp = (float)w - dist;
    while (rp < 0) rp += (float)len;
    uint32_t i0 = (uint32_t)rp, i1 = i0 + 1u >= len ? 0 : i0 + 1u;
    if (i0 >= len) i0 = 0;
    return b[i0] + (b[i1] - b[i0]) * (rp - (float)(uint32_t)rp);
}
PW_INLINE int16_t dl_word(float v) { return (int16_t)(pw_clamp(v, -10.5f, 10.5f) * DL_SCALE); }
static void pass_through(const float *const *in, float *const *out, unsigned n_out)
{
    for (unsigned o = 0; o < n_out; ++o) for (int i = 0; i < PW_N; ++i) out[o][i] = in[0][i];
}

/* FLANGER: a short delay swept by a triangle, fed back */
typedef struct { uint32_t w; float ph, lp; } flang_st;
PROC(flanger_proc)
{
    ST(flang_st);
    int16_t *buf = PW_POOL16(m);
    uint32_t len = m->mem_len / 2u;
    if (m->status != PW_STATUS_OK || len < 512) { pass_through(in, out, 1); return; }
    float rate = m->p[0], depth = m->p[1], center = m->p[2] * PW_SRF, fb = m->p[3], mix = m->p[4];
    float maxd = (float)(len - 4u);
    for (int i = 0; i < PW_N; ++i) {
        float r = IN_CONNECTED(1) ? rate * pw_exp2(in[1][i]) : rate;
        s->ph += r * PW_INV_SR; if (s->ph >= 1.0f) s->ph -= 1.0f;
        float d = pw_clamp(center * (1.0f + 0.95f * depth * tri1(s->ph)), 2.0f, maxd);
        float wet = dl_read(buf, len, s->w, d);
        buf[s->w] = dl_word(in[0][i] + fb * wet);
        if (++s->w >= len) s->w = 0;
        out[0][i] = in[0][i] * (1.0f - mix * 0.5f) + wet * mix;
    }
}

/* PHASER: six first-order allpasses swept around a centre, fed back */
typedef struct { float z[6], ph, fbv, a; } phase_st;
PROC(phaser_proc)
{
    ST(phase_st);
    float rate = m->p[0], depth = m->p[1], center = m->p[2], fb = m->p[3], mix = m->p[4];
    for (int i = 0; i < PW_N; ++i) {
        if ((i & 15) == 0) {
            float r = IN_CONNECTED(1) ? rate * pw_exp2(in[1][i]) : rate;
            s->ph += r * PW_INV_SR * 16.0f; if (s->ph >= 1.0f) s->ph -= 1.0f;
            float f = pw_clamp(center * pw_exp2(depth * 2.5f * tri1(s->ph)), 30.0f, 16000.0f);
            float t = pw_tan_pi(f * PW_INV_SR);
            s->a = (1.0f - t) / (1.0f + t);
        }
        float x = in[0][i] * 0.2f + fb * s->fbv, a = s->a;
        for (int k = 0; k < 6; ++k) {        /* y = -a x + z; z = x + a y */
            float y = -a * x + s->z[k];
            s->z[k] = x + a * y;
            x = y;
        }
        s->fbv = pw_tanh(x);
        out[0][i] = 5.0f * (in[0][i] * 0.2f * (1.0f - mix * 0.5f) + x * mix * 0.7f);
    }
    for (int k = 0; k < 6; ++k) if (pw_absf(s->z[k]) < 1e-20f) s->z[k] = 0;
}

/* CHORUS: three voices on slow, offset LFOs; two to each side */
typedef struct { uint32_t w; float ph; } chor_st;
PROC(chorus_proc)
{
    ST(chor_st);
    int16_t *buf = PW_POOL16(m);
    uint32_t len = m->mem_len / 2u;
    if (m->status != PW_STATUS_OK || len < 1024) { pass_through(in, out, 2); return; }
    float rate = m->p[0], depth = m->p[1], mix = m->p[2], width = m->p[3];
    float base = 0.012f * PW_SRF, span = 0.007f * PW_SRF * depth;
    for (int i = 0; i < PW_N; ++i) {
        float r = IN_CONNECTED(1) ? rate * pw_exp2(in[1][i]) : rate;
        s->ph += r * PW_INV_SR; if (s->ph >= 1.0f) s->ph -= 1.0f;
        buf[s->w] = dl_word(in[0][i]);
        float v[3];
        for (int k = 0; k < 3; ++k) v[k] = dl_read(buf, len, s->w, base + span * (1.0f + pw_sin1(s->ph + (float)k / 3.0f)));
        if (++s->w >= len) s->w = 0;
        float l = v[0] + v[1] * (1.0f - width * 0.5f) + v[2] * (1.0f - width), r2 = v[2] + v[1] * (1.0f - width * 0.5f) + v[0] * (1.0f - width);
        float g = 0.45f;
        out[0][i] = in[0][i] * (1.0f - mix * 0.5f) + l * g * mix;
        out[1][i] = in[0][i] * (1.0f - mix * 0.5f) + r2 * g * mix;
    }
}

/* FM-2: two sine operators, a modulator into a carrier, in eight knobs. RATIO is the modulator's frequency against the
 * carrier's, INDEX how hard it modulates, FBK its feedback (sine to saw to noise). Each operator has its own envelope,
 * picked from shapes rather than set stage by stage: ORGAN to TICK below, each an attack (linear, from where it is), a
 * decay to the sustain level, a release, its times scaled by TIME. The carrier's shape (AMP) is the loudness, the
 * modulator's (MOD) is the brightness: a bell is two long ones, a pluck a short MOD under a longer AMP, a pad two slow
 * ones. GATE's rising edge starts the attack, its fall the release; GATE unpatched: they sound for ever. The INDEX jack adds
 * to INDEX; FM puts any audio into the carrier's phase. */
static const char *const L_ratio[] = {"0.25", "0.5", "0.75", "1", "1.5", "2", "2.5", "3", "3.5", "4",
                                      "5", "6", "7", "8", "9", "10", "11", "12", "14", "16"};
static const float fm_ratio[20] PW_DSP_DATA = {0.25f, 0.5f, 0.75f, 1, 1.5f, 2, 2.5f, 3, 3.5f, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16};
/* per algorithm: for each operator, the operators modulating it (bit n: op n + 1); and the carriers */
static const char *const L_fmenv[] = {"ORGAN", "PLUCK", "PERC", "PIANO", "BELL", "BRASS", "BOW", "PAD", "SWELL", "GATE", "BLOOM", "TICK"};
/* attack, decay, sustain, release (seconds, at TIME 1; decay and release: time constants) */
const float pw_fm_shape[PW_FM_SHAPES][4] = {
    {0.005f, 0.1f, 1.0f, 0.08f}, {0.002f, 0.25f, 0.0f, 0.1f}, {0.001f, 0.08f, 0.0f, 0.05f}, {0.002f, 2.0f, 0.0f, 0.35f},
    {0.001f, 4.0f, 0.0f, 1.5f},  {0.06f, 0.3f, 0.7f, 0.15f},  {0.25f, 0.5f, 0.85f, 0.4f},  {0.8f, 1.0f, 0.8f, 1.5f},
    {2.0f, 0.1f, 1.0f, 2.0f},    {0.001f, 0.0f, 1.0f, 0.01f}, {0.4f, 2.0f, 0.3f, 1.0f},  {0.0003f, 0.025f, 0.0f, 0.02f}};
typedef struct { float ph[2], env[2], fb1; uint8_t gate_hi, stage[2]; } fm4_st;   /* stage: attack, decay, release */
static void fm4_init(pw_module *m) { ST(fm4_st); for (int k = 0; k < 2; ++k) { s->env[k] = 0; s->stage[k] = 2; } }
/* A PRESET jack: 0-5 V steps through the module's presets (a DRUM RACK's KITs), added to the PRESET knob. The audio
 * side only says which one; pw_maintain (the control side) makes the knobs follow. */
static void preset_cv(const pw_module *m, const float *cv, unsigned connected, unsigned jack)
{
    unsigned slot = (unsigned)(m - pws.mod);
    if (slot >= PW_MAX_MODULES) return;
    if (!((connected >> jack) & 1u)) { pws.pre_eff[slot] = 0xff; return; }
    int pp = pw_preset_param(m->type), n = pw_preset_count(m->type);
    if (pp < 0 || n <= 0) return;
    int eff = (int)(m->p[pp] + 0.5f) + pw_floori(cv[0] * 0.2f * (float)n + 0.5f);
    pws.pre_eff[slot] = (uint8_t)(eff < 0 ? 0 : eff > n ? n : eff);
}
enum { FM_RATIO, FM_INDEX, FM_FBK, FM_TUNE, FM_AMP, FM_MOD, FM_TIME, FM_PRESET };
PROC_COLD(fm4_proc)
{
    ST(fm4_st);
    preset_cv(m, in[4], connected, 4);
    float fbk = m->p[FM_FBK] * 0.9f, base = m->p[FM_TUNE] / 12.0f, ratio = fm_ratio[(int)pw_clamp(m->p[FM_RATIO] + 0.5f, 0.0f, 19.0f)];
    float inc[2], dec[2], sus[2], rel[2];
    for (int k = 0; k < 2; ++k) {
        const float *sh = pw_fm_shape[(int)pw_clamp(m->p[FM_AMP + k] + 0.5f, 0.0f, (float)(PW_FM_SHAPES - 1))];
        float t = m->p[FM_TIME];
        inc[k] = 1.0f / (pw_maxf(sh[0] * t, 0.0003f) * PW_SRF);
        dec[k] = pw_coef(sh[1] * t);
        sus[k] = sh[2];
        rel[k] = pw_coef(pw_maxf(sh[3] * t, 0.002f));
    }
    int gated = IN_CONNECTED(1);
    float f0 = pw_pitch_hz(base) * PW_INV_SR;
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(0)) f0 = pw_pitch_hz(base + in[0][i]) * PW_INV_SR;
        float idx = m->p[FM_INDEX] * 1.5f + (IN_CONNECTED(2) ? in[2][i] * 0.2f : 0);
        if (gated) {
            uint8_t was = s->gate_hi;
            if (pw_rise(&s->gate_hi, in[1][i])) { s->stage[0] = 0; s->stage[1] = 0; }
            if (was && !s->gate_hi) { s->stage[0] = 2; s->stage[1] = 2; }
            for (int k = 0; k < 2; ++k) {
                if (s->stage[k] == 0) {          /* attack: a straight line up from where it is */
                    s->env[k] += inc[k];
                    if (s->env[k] >= 1.0f) { s->env[k] = 1.0f; s->stage[k] = 1; }
                } else if (s->stage[k] == 1) s->env[k] = sus[k] + (s->env[k] - sus[k]) * dec[k];
                else s->env[k] *= rel[k];
            }
        } else { s->env[0] = 1.0f; s->env[1] = 1.0f; }
        float om = pw_sin1(s->ph[1] + fbk * s->fb1);                 /* the modulator, feeding back on itself */
        s->fb1 = 0.5f * (s->fb1 + om);
        float pm = om * pw_clamp(idx, 0.0f, 1.5f) * s->env[1] * 1.3f;
        if (IN_CONNECTED(3)) pm += in[3][i] * 0.1f;                   /* FM: any audio into the carrier */
        out[0][i] = pw_sin1(s->ph[0] + pm) * s->env[0] * 5.0f;
        s->ph[0] += f0; s->ph[1] += f0 * ratio;
        if (s->ph[0] >= 1.0f) s->ph[0] -= (float)pw_floori(s->ph[0]);
        if (s->ph[1] >= 1.0f) s->ph[1] -= (float)pw_floori(s->ph[1]);
    }
    for (int k = 0; k < 2; ++k) if (s->env[k] < 1e-12f) s->env[k] = 0;
}

/* CZ: Casio CZ / CZ01 tones, played as the CZ does. A tone is 144 bytes (128 of sound, 16 of name) kept raw in a sample bank
 * slot (BANK, B1-B16: the web page puts a .syx file's tones there, 144 bytes each); TONE picks one. The knobs change it on top:
 * the tone's own envelopes, waves, windows, key follow and vibrato play as stored.
 * The playback math (phase distortion windows and waves, the chip's envelope rates and targets, the logarithmic amplitude table) follows
 * Devin Acker's uPD933 model in MAME (BSD-3-Clause) as adapted for the FM-1 in Melodee by Kerem Kilic (GPL-3.0); see LICENSING.md.
 * Tone bytes (Casio MIDI specification): [0] line select, octave; [1] detune sign; [2] fine; [3] detune note; [4] vibrato wave; [5-13] vibrato
 * delay/rate/depth; [14-70] line 1: wave word, key follows, velocity bytes and the pitch, DCW and DCA envelopes (8 steps of rate, level);
 * [71-127] line 2 the same; [128-143] name. */
static const uint16_t cz_amp[128] = {
    0,4,5,5,5,6,6,7,7,8,8,9,9,10,11,12,
    12,13,14,15,16,18,19,20,22,23,25,27,29,31,33,36,
    38,41,44,47,51,54,58,63,67,72,77,83,89,96,103,110,
    118,127,136,146,157,168,180,194,208,223,239,257,275,296,317,340,
    365,392,421,451,484,520,558,598,642,689,739,793,851,914,980,1052,
    1129,1212,1300,1395,1497,1606,1724,1850,1985,2130,2286,2453,2632,2824,3031,3252,
    3490,3745,4019,4313,4628,4966,5329,5718,6136,6585,7066,7582,8136,8731,9369,10054,
    10789,11577,12423,13331,14305,15351,16473,17676,18968,20355,21842,23438,25151,26990,28962,31079};
static const char *const L_czwave[] = {"SAW", "SQUARE", "PULSE", "DBL SIN", "SAW PLS", "RESO 1", "RESO 2", "RESO 3"};
static const char *const L_czwave2[] = {"SAME", "SAW", "SQUARE", "PULSE", "DBL SIN", "SAW PLS", "RESO 1", "RESO 2", "RESO 3"};
static const char *const L_czline[] = {"LINE 1", "LINE 2", "1+1'", "1+2'"};
static const char *const L_czmod[] = {"OFF", "RING", "NOISE"};
static const char *const L_czoct[] = {"-1 OCT", "NORMAL", "+1 OCT"};
static const char *const L_cz_pages[] = {"PRESET", "LINE", "WAVE", "DCW ENV", "LOUD ENV", "PITCH", "SCALE"};
typedef struct { float level; uint8_t stage, gate; } cz_env;
typedef struct { cz_env e[2][3]; float ph[2], dc[2], a[2], vph, vt, nph; uint32_t noise, trev; uint8_t tog[2], gate_hi, nhi, tk, tb, tv, emb, tone[144]; } cz_st;   /* (tone: a bank tone copied out of the flash, tk its number, tv: held, trev: the bank it came from; emb: the tone came with the patch, not from the bank) */
static void cz_init(pw_module *m) { ST(cz_st); s->noise = 0x2545F491u; }
/* the rate byte (0-127) whose chip rate comes nearest `per_s` units a second, for envelope kind e (0 pitch, 1 DCW, 2 DCA) */
static float cz_rate(unsigned raw, unsigned e);
static unsigned cz_raw(float per_s, unsigned e)
{
    float c = per_s / cz_rate(0u, e) * 8.0f;                 /* (the chip rate wanted: rate 0 is 8) */
    if (!(c > 8.0f)) return 0u;
    int ex = (int)pw_floori(pw_log2(c)) - 3;
    ex = ex < 0 ? 0 : ex > 15 ? 15 : ex;
    int mt = (int)(c / (float)(1u << ex) + 0.5f) - 8;
    if (mt > 7) { if (ex < 15) { ++ex; mt = 0; } else mt = 7; }
    return (unsigned)(ex * 8 + (mt < 0 ? 0 : mt));
}
static uint8_t cz_level(float x, unsigned e)               /* amplitude 0-1 -> the level byte of a DCA step (log: 127 is full) */
{
    if (!(x > 0.003f)) return 0u;
    float l = 127.0f + 9.81f * pw_log2(x > 1.0f ? 1.0f : x);
    (void)e;
    return (uint8_t)(l < 1.0f ? 1.0f : l);
}
/* The CZ module. BANK and PRESET pick a tone: 144 bytes, two lines, each a wave bent by DCW, with eight-step pitch, DCW and loudness
 * envelopes. The other knobs show what the tone says (the waves, the DCW and loudness envelopes as attack, decay, sustain and release, the pitch
 * drop, the vibrato, the line, ring or noise, detune, octave). A knob moved off its tone's value takes that part over: the tone's bytes are changed
 * on their way to the voice (in RAM; the bank is never touched), and PRESET shows a * until the knobs are back. */
enum { CZ_BANK, CZ_PRESET, CZ_LEVEL, CZ_TUNE, CZ_LINE, CZ_MOD, CZ_DETUNE, CZ_OCT, CZ_W1, CZ_W2, CZ_DCW, CZ_FOLLOW, CZ_DATK, CZ_DDEC, CZ_DSUS, CZ_DREL,
       CZ_AATK, CZ_ADEC, CZ_ASUS, CZ_AREL, CZ_PAMT, CZ_PTIM, CZ_VIB, CZ_VRATE, CZ_BRIGHT, CZ_SPEED, CZ_ASPD, CZ_REL, CZ_N };
static const uint8_t cz_wmap[8][2] = {{0, 0}, {1, 0}, {2, 0}, {4, 0}, {5, 0}, {6, 1}, {6, 2}, {6, 3}};
static void cz_env_bytes(uint8_t *b, unsigned end_at, unsigned first, unsigned e, const float *lv, const float *per_s, int n)
{
    (void)end_at;
    for (int i = 0; i < n; ++i) { b[first + 2u * (unsigned)i] = (uint8_t)cz_raw(per_s[i], e); b[first + 2u * (unsigned)i + 1u] = (uint8_t)lv[i]; }
}
/* the CZ's banks: 0 INTERNAL (built in, pw_czbank.inc), then the .syx banks the page sent (the bank's slots PW_CZ_SLOT up); BANK picks one */
#include "pw_czbank.inc"
static const uint8_t *cz_bank_of(unsigned bank, unsigned *n)
{
    *n = 0;
    if (bank == 0u) { *n = CZ_INTERNAL_N; return cz_internal; }
    if (bank > PW_CZ_BANKS) return 0;
    uint32_t frames = 0, rate = 0;
    const int16_t *d = pw_plat_sample(PW_CZ_SLOT + bank - 1u, &frames, &rate);
    *n = d ? frames * 2u / 144u : 0u;
    if (*n > 128u) *n = 128u;
    return *n ? (const uint8_t *)d : 0;
}
static unsigned cz_bank_knob(const pw_module *m) { return (unsigned)pw_clamp(m->p[CZ_BANK] + 0.5f, 0.0f, (float)PW_CZ_BANKS); }
static int cz_valid(const uint8_t *b)
{
    if ((b[0] & 0xF0u) || (b[0] >> 2) > 2u || b[1] > 1u || b[3] > 47u) return 0;
    for (unsigned l = 0; l < 2u; ++l) {
        unsigned off = l ? 57u : 0u;
        if ((b[16u + off] & 15u) > 9u || (b[16u + off] >> 4) > 14u || b[18u + off] > 9u) return 0;
    }
    return 1;
}
/* the tone (144 bytes) the module's BANK and PRESET name, or 0 (an empty bank, a damaged tone). A tone that came with the patch (a saved patch whose bank is
 * gone, or whose tone was edited) is the module's own while BANK and PRESET are what they were saved as. */
static int cz_emb_ok(const pw_module *m)
{
    const cz_st *s = (const cz_st *)(const void *)m->stp;
    if (!s) return 0;   /* (the add list's preview module has no state) */
    return s->emb && s->tk == (unsigned)pw_clamp(m->p[CZ_PRESET] + 0.5f, 1.0f, 128.0f) && s->tb == cz_bank_knob(m);
}
static const uint8_t *cz_tone(const pw_module *m)
{
    if (cz_emb_ok(m)) return ((const cz_st *)(const void *)m->stp)->tone;
    unsigned n = 0;
    const uint8_t *d = cz_bank_of(cz_bank_knob(m), &n);
    if (!d) return 0;
    uint32_t k = (uint32_t)pw_clamp(m->p[CZ_PRESET] + 0.5f, 1.0f, 128.0f) - 1u;
    const uint8_t *b = d + (k < n ? k : n - 1u) * 144u;
    return cz_valid(b) ? b : 0;
}
const uint8_t *pw_cz_tone_name(unsigned bank, unsigned tone)   /* the 16 name bytes of a bank's tone (1-128), or 0 */
{
    unsigned n = 0;
    const uint8_t *d = tone ? cz_bank_of(bank, &n) : 0;
    if (!d || tone > n) return 0;
    return d + (tone - 1u) * 144u + 128u;
}
unsigned pw_cz_tones(unsigned bank)   /* how many tones a bank holds (0: empty) */
{
    unsigned n = 0;
    cz_bank_of(bank, &n);
    return n;
}
static float dx_clamp(unsigned slot, unsigned p, float v);   /* (FM-DX shares these calls: below) */
static float dx_norm(unsigned slot, unsigned p);
static int dx_edited(unsigned slot);
static int dx_embed(unsigned slot, uint8_t *out);
static void dx_embed_set(unsigned slot, const uint8_t *voice);
static int dx_embedded(unsigned slot);
static const uint8_t *dx_embedded_name(unsigned slot);
#define IS_DX(slot) ((slot) < PW_MAX_MODULES && pws.mod[slot].type == PW_T_FMDX)
/* a BANK or PRESET value the module can take: a bank that holds tones (the next one in the direction of the turn), a preset that is there */
float pw_cz_clamp(unsigned slot, unsigned p, float v)
{
    if (IS_DX(slot)) return dx_clamp(slot, p, v);
    const pw_module *m = &pws.mod[slot];
    if (p == CZ_BANK) {
        int b = (int)(v + 0.5f), cur = (int)cz_bank_knob(m), dir = b >= cur ? 1 : -1;
        if (b < 0) b = 0;
        if (b > (int)PW_CZ_BANKS) b = (int)PW_CZ_BANKS;
        for (int k = b; k >= 0 && k <= (int)PW_CZ_BANKS; k += dir) if (pw_cz_tones((unsigned)k)) return (float)k;
        return (float)cur;
    }
    if (p == CZ_PRESET) { unsigned n = pw_cz_tones(cz_bank_knob(m)); return n ? pw_clamp(v, 1.0f, (float)n) : 1.0f; }
    return v;
}
/* where BANK or PRESET sits among what is there (the knob's bar): a bank among the banks that hold tones, a preset among its bank's tones */
PW_COLD float pw_cz_norm(unsigned slot, unsigned p)
{
    if (IS_DX(slot)) return dx_norm(slot, p);
    const pw_module *m = &pws.mod[slot];
    if (p == CZ_BANK) {
        unsigned cur = cz_bank_knob(m), rank = 0, cnt = 0;
        for (unsigned k = 0; k <= PW_CZ_BANKS; ++k) if (pw_cz_tones(k)) { if (k < cur) ++rank; ++cnt; }
        return cnt > 1u ? (float)rank / (float)(cnt - 1u) : 1.0f;
    }
    unsigned n = pw_cz_tones(cz_bank_knob(m));
    float k = pw_clamp(m->p[CZ_PRESET], 1.0f, 128.0f);
    return n > 1u ? pw_clamp((k - 1.0f) / (float)(n - 1u), 0.0f, 1.0f) : 1.0f;
}
static float cz_rate(unsigned raw, unsigned e)   /* units a second: the chip's rate law */
{
    float chip = (float)((8u | (raw & 7u)) << ((raw & 127u) >> 3));
    return chip * 40000.0f * (e == 0u ? 1.0f / (8.0f * 131072.0f) : e == 1u ? 1.0f / (4.0f * 16384.0f) : 1.0f / (2.0f * 131072.0f));
}
static float cz_target(unsigned raw, unsigned e)
{
    raw &= 127u;
    if (e == 0u) return (float)(raw & 63u) * ((raw & 64u) ? 2.0f : 0.0625f);   /* semitones */
    return (float)raw * (e == 1u ? 8.0f : 1.0f);                                  /* DCW phase units, DCA 0-127 */
}
/* one envelope (line l, kind k: 0 pitch, 1 DCW, 2 DCA) for a block of dt seconds; the rates times `scale` */
static float cz_env_run(cz_env *e, const uint8_t *tone, unsigned l, unsigned k, int gate, float scale, float dt)
{
    static const uint8_t base[2][3] = {{55, 38, 21}, {112, 95, 78}}, endb[2][3] = {{54, 37, 20}, {111, 94, 77}};
    const uint8_t *st = tone + base[l][k];
    unsigned end = tone[endb[l][k]] & 7u, sus = 255u;
    for (unsigned j = 0; j < end; ++j) if ((st[2u * j + 1u] & 128u) && sus == 255u) sus = j;
    if (e->gate && !gate) {   /* key up: past the sustain step, or with none, to the END step (the release) */
        if (sus <= end) { if (e->stage <= sus) e->stage = (uint8_t)(sus + 1u); }
        else if (e->stage < end) e->stage = (uint8_t)end;
    }
    e->gate = (uint8_t)gate;
    float left = 1.0f;
    for (int n = 0; n < 8 && e->stage <= end; ++n) {
        unsigned s = e->stage;
        float rate = cz_rate(st[2u * s], k) * scale * dt, target = s == end ? 0.0f : cz_target(st[2u * s + 1u], k);
        float delta = target - e->level, dist = delta < 0 ? -delta : delta, step = rate * left;
        if (dist > step) { e->level += delta < 0 ? -step : step; break; }
        e->level = target;
        if (gate && s == sus) break;
        e->stage++;
        if (dist > 0 && rate > 0) left = pw_maxf(0.0f, left - dist / rate);
        if (left <= 0) break;
    }
    return e->level;
}
static float cz_amp_of(float level, unsigned atten)
{
    float x = pw_clamp(level, 0.0f, 127.0f);
    unsigned i = (unsigned)x;
    float a = (float)cz_amp[i];
    if (i < 127u) a += (float)((int)cz_amp[i + 1u] - (int)cz_amp[i]) * (x - (float)i);
    return a * (1.0f / 32768.0f) * (float)(15u - atten) * (1.0f / 15.0f);
}
typedef struct { uint8_t w[2], win; float pivot, k0, k1, dcw; } cz_pd;
static float cz_wave(const cz_pd *p, float pos, unsigned tog)
{
    float half = pos >= 1024.0f ? pos - 1024.0f : pos, phase = 0.0f, window = 0.0f, pivot = p->pivot;
    switch (p->w[tog & 1u]) {
    case 0: phase = pos < pivot ? pos * p->k0 : 1024.0f + (pos - pivot) * p->k1; break;
    case 1: phase = (half < pivot ? half * p->k0 : 1024.0f) + (pos >= 1024.0f ? 1024.0f : 0.0f); break;
    case 2: phase = pos < pivot * 2.0f ? pos * p->k0 : 2048.0f; break;
    case 3: return 0.0f;
    case 4: phase = pos < pivot ? pos * p->k0 * 2.0f : (pos - pivot) * p->k1 * 2.0f; break;
    case 5: phase = pos < 1024.0f ? pos : pos < pivot + 1024.0f ? 1024.0f + half * p->k0 : 2048.0f; break;
    case 6: phase = pos * (1.0f + p->dcw * (1.0f / 64.0f)); break;
    default: phase = half < pivot ? half * p->k0 : 2048.0f; break;
    }
    if (phase >= 2048.0f) phase -= (float)(int)(phase * (1.0f / 2048.0f)) * 2048.0f;
    switch (p->win) {
    case 0: break;
    case 1: window = pos; break;
    case 2: window = pos < 1024.0f ? 2048.0f - half * 2.0f : half * 2.0f; break;
    case 3: if (pos >= 1024.0f) window = half * 2.0f; break;
    case 4: window = pos < 1024.0f ? pos * 2.0f : 2048.0f; break;
    default: window = (1024.0f - half) * 2.0f; break;
    }
    return (1.0f - pw_cos1(phase * (1.0f / 2048.0f))) * (1.0f - window * (1.0f / 2048.0f)) - 1.0f;
}
/* a tone as the knobs would say it: WAVE 1, WAVE 2, DCW, D.ATK, D.DEC, D.SUS, D.REL, A.ATK, A.DEC, A.SUS, A.REL, PIT AMT, PIT TIM, VIB (what loading a tone puts on
 * the knobs; the tone itself still plays exactly as it is, TONE 0 plays the knobs) */
static float cz_time(const uint8_t *st, unsigned j, unsigned k, float from)
{
    float to = cz_target(st[2u * j + 1u], k), r = cz_rate(st[2u * j], k);
    return pw_absf(to - from) / pw_maxf(r, 0.001f);
}
static void cz_summary(const uint8_t *b, float *o)
{
    unsigned off = (b[0] & 3u) == 1u ? 57u : 0u, word = (unsigned)b[14u + off] << 8 | b[15u + off], w1 = (word >> 13) & 7u, win = (word >> 6) & 7u, wi = 0, found = 0, wi2 = 0;
    for (unsigned i = 0; i < 8u && !found; ++i) if (cz_wmap[i][0] == w1 && cz_wmap[i][1] == win) { wi = i; found = 1; }
    for (unsigned i = 0; i < 8u && !found; ++i) if (cz_wmap[i][0] == w1) { wi = i; found = 1; }
    if (word & 512u) for (unsigned i = 0; i < 8u; ++i) if (cz_wmap[i][0] == ((word >> 10) & 7u)) { wi2 = i + 1u; break; }
    o[0] = (float)wi; o[1] = (float)wi2;
    for (unsigned k = 1; k <= 2; ++k) {   /* DCW (k 1), then the loudness (k 2): attack, decay, sustain (a fraction of the top), release */
        const uint8_t *st = b + (k == 1 ? 38u : 21u) + off;
        unsigned end = b[(k == 1 ? 37u : 20u) + off] & 7u, sus = end ? end - 1u : 0u, top = 0;
        for (unsigned j = 0; j < end; ++j) { if ((st[2u * j + 1u] & 128u) && sus == end - 1u) sus = j; if ((st[2u * j + 1u] & 127u) > top) top = st[2u * j + 1u] & 127u; }
        float lv0 = end ? cz_target(st[1], k) : 0.0f, lvs = end ? cz_target(st[2u * sus + 1u], k) : 0.0f;
        float atk = end ? cz_time(st, 0, k, 0.0f) : 0.005f, dec = end > 1u ? cz_time(st, 1, k, lv0) : 1.0f, rel = cz_time(st, end, k, lvs);
        float frac = k == 1 ? (lv0 > 0.0f ? lvs / lv0 : 1.0f) : (end > 1u ? cz_amp_of(lvs, 0) / pw_maxf(cz_amp_of(lv0, 0), 0.001f) : 1.0f);
        unsigned q = k == 1 ? 3u : 7u;
        o[q] = pw_clamp(atk, 0.001f, 4.0f); o[q + 1u] = pw_clamp(dec, 0.01f, 8.0f); o[q + 2u] = pw_clamp(frac, 0.0f, 1.0f); o[q + 3u] = pw_clamp(rel, 0.01f, 8.0f);
        if (k == 1) o[2] = pw_clamp((float)top * (1.0f / 127.0f), 0.0f, 1.0f);
    }
    {
        unsigned pend = b[54u + off] & 7u;
        float amt = pend ? pw_absf(cz_target(b[56u + off], 0)) : 0.0f;
        o[11] = pw_clamp(amt, 0.0f, 24.0f);
        o[12] = pend ? pw_clamp(cz_time(b + 55u + off, 0, 0, 0.0f), 0.005f, 2.0f) : 0.1f;
    }
    { float d = (float)((unsigned)b[12] | (unsigned)b[13] << 8); o[13] = d <= 1.0f ? 0.0f : pw_minf(d * 0.01f, 1.0f); }
}
/* the knobs' values that say what a tone says (the ones cz_summary does not give); the rest of v[] is left alone */
PW_COLD static void cz_view(const uint8_t *b, float *v)
{
    static const uint8_t ks[14] = {CZ_W1, CZ_W2, CZ_DCW, CZ_DATK, CZ_DDEC, CZ_DSUS, CZ_DREL, CZ_AATK, CZ_ADEC, CZ_ASUS, CZ_AREL, CZ_PAMT, CZ_PTIM, CZ_VIB};
    float sm[14];
    cz_summary(b, sm);
    for (unsigned q = 0; q < 14u; ++q) v[ks[q]] = sm[q];
    v[CZ_LINE] = (float)(b[0] & 3u);
    { unsigned md = (b[15] >> 3) & 7u; v[CZ_MOD] = md == 4u ? 1.0f : md == 3u ? 2.0f : 0.0f; }
    v[CZ_DETUNE] = pw_clamp(((float)b[3] + (float)(b[2] >> 2) * (1.0f / 64.0f)) * (b[1] ? -1.0f : 1.0f), -12.0f, 12.0f);
    v[CZ_OCT] = (b[0] >> 2) == 2u ? 0.0f : (b[0] >> 2) == 1u ? 2.0f : 1.0f;
}
enum { CZE_WAVE = 1, CZE_DCW = 2, CZE_DCA = 4, CZE_PIT = 8, CZE_VIB = 16, CZE_LINE = 32, CZE_MOD = 64, CZE_DET = 128, CZE_OCT = 256 };
/* which parts of the tone the knobs have taken over: those whose knobs are off the tone's values by more than 1.5 % of their travel (a snapshot's
 * 16-bit knobs and the tone's own rounding stay under that) */
static unsigned cz_edits(const pw_module *m, const float *v)
{
    static const struct { uint8_t k; uint16_t part; } map[] = {
        {CZ_W1, CZE_WAVE}, {CZ_W2, CZE_WAVE}, {CZ_DCW, CZE_DCW}, {CZ_DATK, CZE_DCW}, {CZ_DDEC, CZE_DCW}, {CZ_DSUS, CZE_DCW}, {CZ_DREL, CZE_DCW},
        {CZ_AATK, CZE_DCA}, {CZ_ADEC, CZE_DCA}, {CZ_ASUS, CZE_DCA}, {CZ_AREL, CZE_DCA}, {CZ_PAMT, CZE_PIT}, {CZ_PTIM, CZE_PIT}, {CZ_VIB, CZE_VIB},
        {CZ_LINE, CZE_LINE}, {CZ_MOD, CZE_MOD}, {CZ_DETUNE, CZE_DET}, {CZ_OCT, CZE_OCT}};
    unsigned slot = (unsigned)(m - pws.mod), e = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; ++i)
        if (pw_absf(pw_lane_norm(slot, map[i].k, m->p[map[i].k]) - pw_lane_norm(slot, map[i].k, v[map[i].k])) > 0.015f) e |= map[i].part;
    return e;
}
/* the tone as the knobs say it: the tone's own bytes where the knobs agree with them, new bytes (as the knobs describe) for the parts they have taken over */
static const uint8_t *cz_apply(const uint8_t *b, const pw_module *m, uint8_t *w)
{
    float v[CZ_N];
    cz_view(b, v);
    unsigned e = cz_edits(m, v);
    if (!e) return b;
    const float *p = m->p;
    memcpy(w, b, 144);
    for (unsigned l = 0; l < 2u; ++l) {
        uint8_t *q = w + l * 57u;
        if (e & CZE_WAVE) {
            unsigned w1 = (unsigned)pw_clamp(p[CZ_W1] + 0.5f, 0.0f, 7.0f), w2 = (unsigned)pw_clamp(p[CZ_W2] + 0.5f, 0.0f, 8.0f);
            unsigned win = cz_wmap[w1][1] ? cz_wmap[w1][1] : (w2 ? cz_wmap[w2 - 1u][1] : 0u);
            unsigned word = (unsigned)cz_wmap[w1][0] << 13 | (w2 ? 512u | (unsigned)cz_wmap[w2 - 1u][0] << 10 : 0u) | win << 6 | (((unsigned)q[15] & 0x3Fu));
            q[14] = (uint8_t)(word >> 8); q[15] = (uint8_t)word;
        }
        if (e & CZE_DCA) {   /* loudness: attack to full, decay to the sustain, release to nothing */
            float sl = (float)cz_level(p[CZ_ASUS], 2), pk = 127.0f, ls[3] = {pk, sl, 0.0f}, rs[3];
            rs[0] = pk / pw_maxf(p[CZ_AATK], 0.001f); rs[1] = (pk - sl) / pw_maxf(p[CZ_ADEC], 0.01f); rs[2] = pw_maxf(sl, 8.0f) / pw_maxf(p[CZ_AREL], 0.01f);
            q[20] = 2; cz_env_bytes(q, 20, 21, 2, ls, rs, 3); q[24] |= 128;
        }
        if (e & CZE_DCW) {   /* the wave's: the same shape, to DCW times 127 */
            float pk = (float)(int)(pw_clamp(p[CZ_DCW], 0.0f, 1.0f) * 127.0f + 0.5f), ps = (float)(int)(pk * pw_clamp(p[CZ_DSUS], 0.0f, 1.0f) + 0.5f), ls[3] = {pk, ps, 0.0f}, rs[3];
            rs[0] = pk * 8.0f / pw_maxf(p[CZ_DATK], 0.001f); rs[1] = (pk - ps) * 8.0f / pw_maxf(p[CZ_DDEC], 0.01f); rs[2] = pw_maxf(ps, 1.0f) * 8.0f / pw_maxf(p[CZ_DREL], 0.01f);
            q[37] = 2; cz_env_bytes(q, 37, 38, 1, ls, rs, 3); q[41] |= 128;
        }
        if (e & CZE_PIT) {   /* a pitch drop: up at once, then back to the note over PIT TIM */
            float amt = pw_clamp(p[CZ_PAMT], 0.0f, 24.0f);
            if (amt > 0.05f) {
                q[54] = 1; q[55] = 127; q[56] = amt <= 3.9375f ? (uint8_t)(amt * 16.0f + 0.5f) : (uint8_t)(64.0f + (float)(int)(amt * 0.5f + 0.5f));
                q[57] = (uint8_t)cz_raw(amt / pw_maxf(p[CZ_PTIM], 0.005f), 0);
            } else q[54] = 0;
        }
    }
    if (e & CZE_VIB) {
        float vb = pw_clamp(p[CZ_VIB], 0.0f, 1.0f);
        unsigned d = (unsigned)b[12] | (unsigned)b[13] << 8;
        if (d <= 1u && vb > 0.01f) { w[4] = 8; w[6] = 20; w[7] = 0; w[9] = 0x60; w[10] = 0x0A; }   /* (a tone with no vibrato gets a triangle one, after a tenth of a second, about 8 Hz) */
        d = vb > 0.01f ? (unsigned)(2.0f + 98.0f * vb) : 1u;
        w[12] = (uint8_t)d; w[13] = (uint8_t)(d >> 8);
    }
    if (e & CZE_LINE) w[0] = (uint8_t)((w[0] & ~3u) | (unsigned)pw_clamp(p[CZ_LINE] + 0.5f, 0.0f, 3.0f));
    if (e & CZE_OCT) { unsigned o = (unsigned)pw_clamp(p[CZ_OCT] + 0.5f, 0.0f, 2.0f); w[0] = (uint8_t)((w[0] & 3u) | ((o == 0u ? 2u : o == 1u ? 0u : 1u) << 2)); }
    if (e & CZE_MOD) { unsigned k = (unsigned)pw_clamp(p[CZ_MOD] + 0.5f, 0.0f, 2.0f); w[15] = (uint8_t)((w[15] & ~0x38u) | ((k == 1u ? 4u : k == 2u ? 3u : 0u) << 3)); }
    if (e & CZE_DET) {
        float d = pw_clamp(p[CZ_DETUNE], -12.0f, 12.0f), a = pw_absf(d);
        unsigned note = (unsigned)a, fine = (unsigned)((a - (float)note) * 64.0f);
        w[1] = d < 0.0f ? 1u : 0u; w[3] = (uint8_t)note; w[2] = (uint8_t)((fine & 63u) << 2);
    }
    return w;
}
static uint8_t cz_work[144];
/* the tone the module plays (the bank's, as the knobs change it), or 0; scratch is cz_work (the main loop's and the audio's use never overlap: one thread runs the engine) */
static const uint8_t *cz_playing(const pw_module *m)
{
    const uint8_t *b = cz_tone(m);
    return b ? cz_apply(b, m, cz_work) : 0;
}
PW_COLD int pw_cz_edited(unsigned slot)   /* 1: a knob has changed the tone, or a modifier is off neutral (PRESET shows a *) */
{
    if (IS_DX(slot)) return dx_edited(slot);
    if (slot >= PW_MAX_MODULES || pws.mod[slot].type != PW_T_CZ) return 0;
    const pw_module *m = &pws.mod[slot];
    const uint8_t *b = cz_tone(m);
    if (!b) return 0;
    float v[CZ_N];
    cz_view(b, v);
    if (cz_edits(m, v)) return 1;
    static const uint8_t sc[] = {CZ_FOLLOW, CZ_VRATE, CZ_BRIGHT, CZ_SPEED, CZ_ASPD, CZ_REL};
    for (unsigned i = 0; i < sizeof sc; ++i) if (pw_absf(pw_lane_norm(slot, sc[i], m->p[sc[i]]) - pw_lane_norm(slot, sc[i], 1.0f)) > 0.015f) return 1;
    return 0;
}
/* The tone that travels with a saved patch: 1 (and the 144 bytes) when the patch must carry it: the knobs have edited it, or it comes from a .syx bank (that bank
 * may not be on the unit that loads the patch). An unedited INTERNAL tone is only named. */
int pw_cz_embed(unsigned slot, uint8_t *out)
{
    if (IS_DX(slot)) return dx_embed(slot, out);
    if (slot >= PW_MAX_MODULES || pws.mod[slot].type != PW_T_CZ) return 0;
    const pw_module *m = &pws.mod[slot];
    const uint8_t *b = cz_tone(m);
    if (!b || !(cz_bank_knob(m) >= 1u || pw_cz_edited(slot))) return 0;
    memcpy(out, b, 144);
    return 1;
}
void pw_cz_embed_set(unsigned slot, const uint8_t *tone)   /* a patch loads: the tone it carried (BANK and PRESET already set) */
{
    if (IS_DX(slot)) { dx_embed_set(slot, tone); return; }
    if (slot >= PW_MAX_MODULES || pws.mod[slot].type != PW_T_CZ || !cz_valid(tone)) return;
    pw_module *m = &pws.mod[slot];
    cz_st *s = (cz_st *)(void *)m->stp;
    memcpy(s->tone, tone, 144);
    s->tk = (uint8_t)pw_clamp(m->p[CZ_PRESET] + 0.5f, 1.0f, 128.0f); s->tb = (uint8_t)cz_bank_knob(m); s->tv = 1; s->emb = 1; s->trev = pws.bank_rev;
}
int pw_cz_embedded(unsigned slot)   /* 1: the module plays the tone its patch carried (its bank is not needed) */
{
    if (IS_DX(slot)) return dx_embedded(slot);
    return slot < PW_MAX_MODULES && pws.mod[slot].type == PW_T_CZ && cz_emb_ok(&pws.mod[slot]);
}
const uint8_t *pw_cz_embedded_name(unsigned slot)   /* its 16 name bytes, or 0 */
{
    if (IS_DX(slot)) return dx_embedded_name(slot);
    return pw_cz_embedded(slot) ? ((const cz_st *)(const void *)pws.mod[slot].stp)->tone + 128 : 0;
}
/* the CZ's pictures. A line's envelope over `secs` (the key down for the first `hold` of them), n points, 0-1: kind 1 the DCW's (of its top), 2 the loudness (the
 * chip's amplitude). line 0 or 1. Returns 0 when there is no tone; 2 when the other line is just the same (draw one). */
PW_COLD int pw_cz_env(unsigned slot, unsigned kind, unsigned line, float secs, float hold, float *out, unsigned n)
{
    const pw_module *m = slot < PW_MAX_MODULES ? &pws.mod[slot] : 0;
    const uint8_t *b = m && m->type == PW_T_CZ ? cz_playing(m) : 0;
    for (unsigned i = 0; i < n; ++i) out[i] = 0.0f;
    if (!b) return 0;
    static const uint8_t kf_dca[10] = {0, 8, 17, 26, 36, 47, 58, 69, 82, 95}, kf_dcw[10] = {0, 31, 44, 57, 70, 83, 96, 110, 146, 255};
    unsigned ls = b[0] & 3u, mod = (b[15] >> 3) & 7u;
    float dt = secs / (float)n, speed = m->p[CZ_SPEED], kfn = 24.0f * m->p[CZ_FOLLOW];   /* (a C4: as the voice computes it) */
    int ring = ls == 3u && mod == 4u && kind == 2u;
    float prod[64];
    for (unsigned pass = 0; pass < (ring ? 2u : 1u); ++pass) {
        unsigned src = ring ? pass : ls == 1u ? 1u : ls == 2u ? 0u : line, off = src * 57u;   /* (a single line is the only one heard; line 2 of a LINE 1+1' tone is line 1's) */
        cz_env e; memset(&e, 0, sizeof e);
        unsigned atten = (unsigned)(b[16u + off] >> 4);
        float top = 1.0f;
        if (kind == 1u) {
            top = 0.0f;
            for (unsigned j = 0; j < (unsigned)(b[37u + off] & 7u); ++j) if ((float)(b[39u + off + 2u * j] & 127u) * 8.0f > top) top = (float)(b[39u + off + 2u * j] & 127u) * 8.0f;
            if (top < 8.0f) top = 8.0f;
        }
        unsigned kf = b[18u + off], kd = b[16u + off] & 15u;
        float dscale = (!kf || kfn <= 0.0f) ? 1.0f : 96.0f / (96.0f + (float)kf_dcw[kf > 9u ? 9u : kf] * (9.0f / 255.0f) * kfn);
        float ak = 1.0f + (float)kf_dca[kd > 9u ? 9u : kd] * (9.0f / 95.0f) * kfn * (1.0f / 96.0f);
        float *o = ring && pass ? prod : out;
        for (unsigned i = 0; i < n; ++i) {
            int gate = (float)i * dt < hold;
            float lev = cz_env_run(&e, b, src, kind, gate, kind == 2u ? speed * m->p[CZ_ASPD] * ak * (gate ? 1.0f : m->p[CZ_REL]) : speed, dt);
            o[i] = kind == 2u ? cz_amp_of(lev, atten) : pw_clamp(lev * dscale / top, 0.0f, 1.0f);
        }
    }
    if (ring) { for (unsigned i = 0; i < n; ++i) out[i] *= prod[i]; return 2; }   /* (ring: the product of the two, as the voice makes it) */
    if (ls != 3u) return 2;
    return memcmp(b + 14, b + 71, 57) == 0 ? 2 : 1;
}
/* one cycle of the wave at its DCW (its peak, times BRIGHT) */
void pw_cz_wave(unsigned slot, float *wave, unsigned nw)
{
    const pw_module *m = slot < PW_MAX_MODULES ? &pws.mod[slot] : 0;
    const uint8_t *b = m && m->type == PW_T_CZ ? cz_playing(m) : 0;
    for (unsigned i = 0; i < nw; ++i) wave[i] = 0.0f;
    if (!b) return;
    unsigned off = (b[0] & 3u) == 1u ? 57u : 0u, word = (unsigned)b[14u + off] << 8 | b[15u + off], top = 0;
    for (unsigned j = 0; j < (unsigned)(b[37u + off] & 7u); ++j) if ((b[39u + off + 2u * j] & 127u) > top) top = b[39u + off + 2u * j] & 127u;
    cz_pd pd;
    pd.w[0] = (uint8_t)((word >> 13) & 7u); pd.w[1] = (word & 512u) ? (uint8_t)((word >> 10) & 7u) : pd.w[0]; pd.win = (uint8_t)((word >> 6) & 7u);
    pd.dcw = pw_clamp((float)top * 8.0f * m->p[CZ_BRIGHT], 0.0f, 1023.0f);
    pd.pivot = 1024.0f - pd.dcw; pd.k0 = 1024.0f / pd.pivot; pd.k1 = 1024.0f / (2048.0f - pd.pivot);
    for (unsigned i = 0; i < nw; ++i) wave[i] = cz_wave(&pd, (float)i * 2048.0f / (float)nw, 0u) * 0.5f;
}
PROC(cz_proc)
{
    ST(cz_st);
    const uint8_t *b;
    {   /* the bank's tone is read from the flash once and kept in this module's RAM (the audio must not read the flash while it is being written); the knobs' changes go on a copy */
        unsigned k0 = (unsigned)pw_clamp(m->p[CZ_PRESET] + 0.5f, 1.0f, 128.0f), bk = cz_bank_knob(m);
        if (s->emb && !cz_emb_ok(m)) s->emb = 0;   /* (BANK or PRESET moved: the bank's tone again) */
        if (!s->emb && (!s->tv || s->tk != k0 || s->tb != bk || s->trev != pws.bank_rev)) {
            const uint8_t *f = cz_tone(m);
            s->tv = 0;
            if (f) { memcpy(s->tone, f, 144); s->tk = (uint8_t)k0; s->tb = (uint8_t)bk; s->trev = pws.bank_rev; s->tv = 1; }
        }
        b = s->tv ? cz_apply(s->tone, m, cz_work) : 0;
    }
    if (!IN_CONNECTED(1)) s->gate_hi = 1;
    if (!b) { for (int i = 0; i < PW_N; ++i) out[0][i] = 0; if (IN_CONNECTED(1)) for (int i = 0; i < PW_N; ++i) pw_rise(&s->gate_hi, in[1][i]); return; }
    const float level = m->p[CZ_LEVEL], bright = m->p[CZ_BRIGHT], speed = m->p[CZ_SPEED], aspd = m->p[CZ_ASPD], rel = m->p[CZ_REL], vrate = m->p[CZ_VRATE], follow = m->p[CZ_FOLLOW], tune = m->p[CZ_TUNE];
    const float vdelay = (float)(b[6] | (b[7] << 8)) * (1.0f / 200.0f), vinc = (float)(b[9] | (b[10] << 8)) * (200.0f / 65536.0f), vdepth = (float)(b[12] | (b[13] << 8));
    const unsigned vwav = b[4];
    const unsigned ls = b[0] & 3u, mod = (b[15] >> 3) & 7u, first = ls == 1u ? 1u : 0u, last = ls >= 2u ? 1u : first;
    const float oct = (b[0] >> 2) == 1u ? 12.0f : (b[0] >> 2) == 2u ? -12.0f : 0.0f;
    const float dck = 1.0f / (0.0256f * PW_SRF);
    /* the control side (envelopes, vibrato, pitch, DCW) runs every 32 samples, as the CZ's does; the waves every sample */
    for (int c0 = 0; c0 < PW_N; c0 += 32) {
        const int cn = PW_N - c0 < 32 ? PW_N - c0 : 32;
        const float dt = (float)cn * PW_INV_SR, n_inv = 1.0f / (float)cn;
        int rose = 0;
        if (IN_CONNECTED(1)) for (int i = 0; i < cn; ++i) if (pw_rise(&s->gate_hi, in[1][c0 + i])) rose = 1;
        if (rose) {
            memset(s->e, 0, sizeof s->e);
            s->ph[0] = s->ph[1] = 0; s->tog[0] = s->tog[1] = 0; s->vt = s->vph = 0;
            s->a[0] = s->a[1] = 0;
        }
        const int gate = s->gate_hi;
        const float volts = IN_CONNECTED(0) ? in[0][c0] : 0.0f, kfn = pw_maxf(0.0f, 12.0f * volts + 24.0f) * follow;   /* (key follow counts from MIDI note 36) */
        float vib = 0.0f;
        {
            float delay = vdelay, inc = vinc, depth = vdepth;
            s->vt += dt;
            if (s->vt >= delay) {
                s->vph = pw_frac(s->vph + inc * vrate * dt);
                float x = s->vph * 65536.0f, wave;
                if (vwav & 32u) wave = 32767.0f - x;
                else if (vwav & 8u) wave = x < 32768.0f ? x * 2.0f - 32768.0f : 98303.0f - x * 2.0f;
                else if (vwav & 4u) wave = x - 32768.0f;
                else wave = x < 32768.0f ? -32768.0f : 32767.0f;
                if (depth > 1.0f) vib = wave * depth * (1.0f / 8388608.0f);
            }
        }
        cz_pd pd[2];
        float a0[2], a1[2], inc[2] = {0.0f, 0.0f};
        for (unsigned l = 0; l < 2u; ++l) {
            unsigned src = ls == 2u ? 0u : l, off = src * 57u;
            const int heard = l >= first && l <= last;
            unsigned atten = (unsigned)(b[16u + off] >> 4);
            a0[l] = s->a[l];
            float pitch = cz_env_run(&s->e[l][0], b, src, 0, gate, speed, dt);
            float depth = cz_env_run(&s->e[l][1], b, src, 1, gate, speed, dt);
            static const uint8_t kf_dca[10] = {0, 8, 17, 26, 36, 47, 58, 69, 82, 95}, kf_dcw[10] = {0, 31, 44, 57, 70, 83, 96, 110, 146, 255};   /* the tone's key follow 0-9 as the chip's own amounts */
            float ascale = speed * aspd * (1.0f + (float)kf_dca[(b[16u + off] & 15u) > 9u ? 9u : (b[16u + off] & 15u)] * (9.0f / 95.0f) * kfn * (1.0f / 96.0f)) * (gate ? 1.0f : rel);
            float lev = cz_env_run(&s->e[l][2], b, src, 2, gate, ascale, dt);
            a1[l] = heard ? cz_amp_of(lev, atten) : 0.0f;
            s->a[l] = a1[l];
            if (!heard) continue;
            unsigned kf = b[18u + off];
            depth *= (!kf || kfn <= 0.0f) ? 1.0f : 96.0f / (96.0f + (float)kf_dcw[kf > 9u ? 9u : kf] * (9.0f / 255.0f) * kfn);
            float det = l ? ((float)b[3] + (float)(b[2] >> 2) * (1.0f / 64.0f)) * (b[1] ? -1.0f : 1.0f) : 0.0f;
            float nt = pw_clamp(12.0f * volts + tune + oct + vib + pitch + det, -60.0f, 67.9375f);   /* (MIDI note 0 to 127.9, as the CZ) */
            inc[l] = pw_pitch_hz(nt * (1.0f / 12.0f)) * PW_INV_SR;
            if (inc[l] > 0.45f) inc[l] = 0.45f;
            unsigned word = (unsigned)b[14u + off] << 8 | b[15u + off];
            float dcw = depth * bright + (IN_CONNECTED(2) ? in[2][c0] * 100.0f : 0.0f);
            pd[l].dcw = pw_clamp(dcw, 0.0f, pw_maxf(0.0f, pw_minf(1023.0f, 1024.0f - 8192.0f * inc[l] * (PW_SRF * (1.0f / 40000.0f)))));   /* (the chip caps DCW as the pitch climbs: narrow pulses stay audible and nothing aliases) */
            pd[l].w[0] = (uint8_t)((word >> 13) & 7u);
            pd[l].w[1] = (word & 512u) ? (uint8_t)((word >> 10) & 7u) : pd[l].w[0];
            pd[l].win = (uint8_t)((word >> 6) & 7u);
            pd[l].pivot = 1024.0f - pd[l].dcw; pd[l].k0 = 1024.0f / pd[l].pivot; pd[l].k1 = 1024.0f / (2048.0f - pd[l].pivot);
        }
        for (int i = 0; i < cn; ++i) {
            float line[2] = {0.0f, 0.0f}, wv[2] = {0.0f, 0.0f}, am[2] = {0.0f, 0.0f};
            for (unsigned l = first; l <= last; ++l) {
                float adv = inc[l];
                if (l && mod == 3u) {   /* (the chip draws a new pitch, base or 32 semitones up, every 8 of its 40 kHz samples) */
                    if ((s->nph += 5000.0f * PW_INV_SR) >= 1.0f) {
                        s->nph -= 1.0f;
                        s->noise ^= s->noise << 13; s->noise ^= s->noise >> 17; s->noise ^= s->noise << 5;
                        s->nhi = (uint8_t)(s->noise & 1u);
                    }
                    if (s->nhi) adv *= 1625.0f / 256.0f;
                }
                float raw = cz_wave(&pd[l], s->ph[l] * 2048.0f, s->tog[l]);
                s->ph[l] += adv;
                if (s->ph[l] >= 1.0f) { s->ph[l] -= (float)(int)s->ph[l]; s->tog[l] ^= 1u; }
                s->dc[l] += (raw - s->dc[l]) * dck;
                am[l] = a0[l] + (a1[l] - a0[l]) * (float)i * n_inv;
                wv[l] = (raw - s->dc[l]) * 0.5f;
                line[l] = wv[l] * am[l];
            }
            float y = first == last ? line[first] : mod == 4u ? wv[0] * wv[1] * am[0] * am[1]   /* (ring: the two lines multiplied after their loudness, as the chip does it (MAME's uPD933): it rings as long as both) */
                : (line[0] + line[1]) * 0.5f;
            out[0][c0 + i] = y * level * 10.0f;
        }
    }
}

#include "pw_fmdx.inc"

/* LOOPER: a phrase looper, a tape loop that stays in the patch. It keeps its loop at a quarter of the sample rate (11 kHz: warm and
 * dull, on purpose) so a few seconds fit. BEATS is the length at the tempo. While REC is on (the knob or a gate), what comes in is
 * added to the loop, and every pass keeps FBK of what was there and TONE darkens it, so layers fade like old tape. SPEED is varispeed
 * (a half speed is an octave down, twice as long), REV plays it backwards. CLEAR (a trigger) empties it. WRAP gives a trigger at the
 * start of each pass. */
typedef struct { float pos, acc, lp, olp, ph; uint32_t cnt, wrap; uint8_t rec_hi, clr_hi, recing; } loop_st;
#define LOOP_SCALE 12000.0f
PROC(looper_proc)
{
    ST(loop_st);
    int16_t *buf = PW_POOL16(m);
    uint32_t cap = m->mem_len / 2u;
    if (m->status != PW_STATUS_OK || cap < 256u) { for (int i = 0; i < PW_N; ++i) { out[0][i] = in[0][i]; out[1][i] = 0; out[2][i] = 0; } return; }
    float len = pw_clamp(m->p[0] * 60.0f / pws.bpm * 11025.0f, 64.0f, (float)(cap - 4u));
    float fb = m->p[1], semis = m->p[2];
    int rev = m->p[3] > 0.5f, rec_knob = m->p[6] > 0.5f;
    float ta = pw_clamp(pw_lp_coef_hz(4.0f * 300.0f * pw_exp2(m->p[4] * 5.0f)), 0.0f, 1.0f);   /* TONE: 300 Hz to open (at the loop's quarter rate) */
    float level = m->p[5], dry = m->p[7];
    float rate = pw_exp2(semis / 12.0f), oa = pw_lp_coef_hz(4500.0f);
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(2) && pw_rise(&s->clr_hi, in[2][i])) memset(buf, 0, (size_t)cap * 2u);
        else if (!IN_CONNECTED(2)) s->clr_hi = 0;
        int rec = rec_knob || (IN_CONNECTED(1) && in[1][i] > 1.0f);
        s->recing = (uint8_t)rec;
        if (IN_CONNECTED(3) && (i & 7) == 0) rate = pw_exp2(semis / 12.0f + in[3][i]);
        float step = rate * 0.25f * (rev ? -1.0f : 1.0f);
        float x = in[0][i] * 0.2f;
        s->acc += x; ++s->cnt;
        float p0 = s->pos, p1 = p0 + step;
        int wrapped = 0;
        if (p1 >= len) { p1 -= len; wrapped = 1; } else if (p1 < 0) { p1 += len; wrapped = 1; }
        if (wrapped) s->wrap = (uint32_t)(0.004f * PW_SRF);
        if (rec && (int)p1 != (int)p0) {   /* crossed into another stored sample: write the one we leave (its old content kept FBK, plus the input); with REC off the loop is left alone, so it holds */
            int w = (int)p0;
            if (w >= (int)len) w = 0;
            float old = buf[w] * (1.0f / LOOP_SCALE);
            s->lp += ta * (old - s->lp);
            float v = s->lp * fb + (rec ? s->acc / (float)(s->cnt ? s->cnt : 1u) : 0.0f);
            buf[w] = (int16_t)(pw_clamp(v, -2.7f, 2.7f) * LOOP_SCALE);
            s->acc = 0; s->cnt = 0;
        }
        if (!rec) { s->acc = 0; s->cnt = 0; }
        s->pos = p1;
        int ia = (int)p1; float fr = p1 - (float)ia;
        int ib = ia + 1; if (ib >= (int)len) ib = 0;
        float lv = (buf[ia] + (buf[ib] - buf[ia]) * fr) * (1.0f / LOOP_SCALE);
        s->olp += oa * (lv - s->olp);
        float loop = s->olp * level * 5.0f;
        out[0][i] = in[0][i] * dry + loop;
        out[1][i] = loop;
        out[2][i] = s->wrap ? 5.0f : 0.0f;
        if (s->wrap) --s->wrap;
    }
    s->ph = s->pos / len;
    if (pw_absf(s->lp) < 1e-20f) s->lp = 0;
    if (pw_absf(s->olp) < 1e-20f) s->olp = 0;
}

/* REVERSER: what came in plays back backwards, a TIME-long piece at a time. Two read heads, half a piece apart,
 * each faded in and out, so the joins never click; FBK feeds the reversed sound back in. */
typedef struct { uint32_t w; float ph; } rev_st;
PROC(reverser_proc)
{
    ST(rev_st);
    int16_t *buf = PW_POOL16(m);
    uint32_t len = m->mem_len / 2u;
    if (m->status != PW_STATUS_OK || len < 4096) { pass_through(in, out, 1); return; }
    float fb = m->p[1], mix = m->p[2];
    for (int i = 0; i < PW_N; ++i) {
        float t = m->p[0] * (IN_CONNECTED(1) ? pw_exp2(in[1][i]) : 1.0f);
        float L = pw_clamp(t * PW_SRF, 256.0f, (float)(len / 2u) - 4.0f);
        if (s->ph >= L) s->ph -= L * (float)pw_floori(s->ph / L);
        float wet = 0;
        for (int h = 0; h < 2; ++h) {
            float ph = s->ph + (h ? L * 0.5f : 0.0f);
            if (ph >= L) ph -= L;
            float win = 1.0f - pw_absf(2.0f * ph / L - 1.0f);
            wet += win * dl_read(buf, len, s->w, 2.0f * ph + 1.0f);
        }
        buf[s->w] = dl_word(in[0][i] + fb * wet);
        if (++s->w >= len) s->w = 0;
        s->ph += 1.0f;
        out[0][i] = in[0][i] * (1.0f - mix) + wet * mix;
    }
}

/* DRUMS: one synthesized drum voice in the manner of the Syncussion and its descendants: an oscillator whose
 * pitch sweeps (up or down, SWEEP T long), its shape from sine through triangle to square, FM from a second
 * oscillator for metal, band-passed noise with its own decay, a drive. Kick, snare, toms, zaps, hats, clicks:
 * all from the knobs. */
typedef struct { float ph, phm, ph2, ph3, a, p, n, lp, bp, acc, mph[6], mlp, mlp2; uint32_t seed, bt; uint8_t trig_hi; } drum_st;
static void drums_init(pw_module *m) { ST(drum_st); s->seed = 0xD12Au + (uint32_t)(m - pws.mod) * 2654435761u; }
PROC(drums_proc)
{
    ST(drum_st);
    preset_cv(m, in[4], connected, 4);
    float tune = m->p[0], sweep = m->p[1] / 12.0f;
    float pk = pw_coef(m->p[2]), dk0 = m->p[3], wave = m->p[4], noise = m->p[5];
    float color = m->p[6], nk = pw_coef(m->p[7]), mod = m->p[8] * 0.8f, ratio = m->p[9];
    float level = m->p[10], drive = 1.0f + m->p[11] * 7.0f, dnorm = 1.0f / pw_tanh(drive) ;
    float dk = pw_coef(dk0), tune_f = tune * PW_INV_SR;
    float metal = m->p[13], modes = m->p[15];
    unsigned bursts = (unsigned)(m->p[14] + 0.5f), gap = (unsigned)(0.0105f * PW_SRF);
    float nfast = pw_coef(0.007f);
    static const float mr[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
    float mf[6];
    for (int k = 0; k < 6; ++k) mf[k] = mr[k] * (tune / 330.0f) * PW_INV_SR;
    float g = 2.0f * pw_sin1(pw_clamp(color, 50.0f, 15000.0f) * PW_INV_SR * 0.5f);
    for (int i = 0; i < PW_N; ++i) {
        if (pw_rise(&s->trig_hi, in[0][i])) {
            s->a = 1.0f; s->p = 1.0f; s->n = 1.0f; s->ph = 0; s->phm = 0; s->ph2 = 0; s->ph3 = 0;
            s->acc = IN_CONNECTED(1) ? pw_clamp(in[1][i] * 0.2f, 0.0f, 1.5f) : 1.0f;
            s->bt = 0;
        }
        float nkk = nk;
        if (bursts > 1u) {   /* a clap: the noise struck again every ~10 ms (each dying fast), the last one carries the tail */
            if (s->bt < (bursts - 1u) * gap) nkk = nfast;
            if (s->bt && s->bt <= (bursts - 1u) * gap && s->bt % gap == 0) s->n = 1.0f;
        }
        if (s->bt < 0xFFFFFFu) ++s->bt;
        if (s->a < 3e-4f && s->n < 3e-4f) { out[0][i] = 0; continue; }   /* (-70 dB: the tail is gone, the oscillators rest) */
        if (IN_CONNECTED(3) && (i & 15) == 0) dk = pw_coef(dk0 * pw_exp2(in[3][i]));
        float sw = sweep * s->p;   /* (the sweep has died away after a few time constants: the pitch is then TUNE, no exp2 needed) */
        float f = (IN_CONNECTED(2) || (sw > 1e-4f || sw < -1e-4f)) ? tune * pw_exp2(sw + (IN_CONNECTED(2) ? in[2][i] : 0)) * PW_INV_SR : tune_f;
        float ph = mod > 0.0f ? s->ph + mod * pw_sin1(s->phm) * s->a : s->ph;
        float sn = pw_sin1(ph), tr = 1.0f - 4.0f * pw_absf(pw_frac(ph + 0.25f) - 0.5f);
        float osc = wave < 0.5f ? pw_lerp(sn, tr, wave * 2.0f) : pw_lerp(tr, pw_tanh(sn * 8.0f), wave * 2.0f - 1.0f);
        float ow = pw_maxf(1.0f - noise * 0.6f - pw_maxf(noise - 0.9f, 0.0f) * 4.0f, 0.0f);   /* how much of the oscillator is in the mix */
        if (modes > 0.001f) {   /* the shell's other modes (a drum head rings at inharmonic partials, a snare's at ~1.5x and ~3.2x): two more sines that follow the pitch, the higher one dying twice as fast */
            float m2 = pw_sin1(s->ph2), m3 = pw_sin1(s->ph3);
            osc = (osc + modes * (0.6f * m2 * s->a + 0.4f * m3 * s->a * s->a)) / (1.0f + 0.5f * modes);
        }
        if (metal > 0.001f) {   /* six square waves at the 808's cymbal ratios (band-limited, or their aliasing is a hiss), through two high-passes near 6 kHz: the ring of hats, ride and crash */
            float sum = 0;
            for (int k = 0; k < 6; ++k) {
                float dt = pw_clamp(mf[k], 0.0f, 0.45f);
                s->mph[k] += dt; if (s->mph[k] >= 1.0f) s->mph[k] -= 1.0f;
                sum += (s->mph[k] < 0.5f ? 1.0f : -1.0f) + pw_blep(s->mph[k], dt) - pw_blep(pw_frac(s->mph[k] + 0.5f), dt);
            }
            s->mlp += 0.55f * (sum - s->mlp);
            float h1 = sum - s->mlp;
            s->mlp2 += 0.55f * (h1 - s->mlp2);
            osc = pw_lerp(osc, (h1 - s->mlp2) * 1.2f, metal);
        }
        float y = osc * s->a * ow;                     /* (NOISE all the way up: nothing but noise) */
        if (noise > 0.0f) {                            /* noise, band-passed at COLOR */
            float w = pw_rand_f(&s->seed);
            float hp = w - s->lp - 1.2f * s->bp;
            s->bp += g * hp; s->lp += g * s->bp;
            s->bp = pw_clamp(s->bp, -4.0f, 4.0f); s->lp = pw_clamp(s->lp, -4.0f, 4.0f);
            y += s->bp * s->n * noise * 1.4f;
        }
        y = pw_tanh(y * drive) * dnorm;
        out[0][i] = 5.0f * y * level * s->acc;
        s->ph += pw_clamp(f, 0.0f, 0.45f); if (s->ph >= 1.0f) s->ph -= (float)pw_floori(s->ph);
        if (modes > 0.001f) {
            s->ph2 += pw_clamp(f * 1.47f, 0.0f, 0.45f); if (s->ph2 >= 1.0f) s->ph2 -= (float)pw_floori(s->ph2);
            s->ph3 += pw_clamp(f * 3.18f, 0.0f, 0.45f); if (s->ph3 >= 1.0f) s->ph3 -= (float)pw_floori(s->ph3);
        }
        if (mod > 0.0f) { s->phm += pw_clamp(f * ratio, 0.0f, 0.45f); if (s->phm >= 1.0f) s->phm -= (float)pw_floori(s->phm); }
        s->a *= dk; s->p *= pk; s->n *= nkk;
    }
    if (s->lp != s->lp || pw_absf(s->lp) < 1e-20f) s->lp = 0;
    if (s->bp != s->bp || pw_absf(s->bp) < 1e-20f) s->bp = 0;
}

/* ENV FOLLOWER: the loudness of what comes in as a voltage. A one-pole smoother on the rectified input that rises
 * at ATTACK and falls at RELEASE; ENV 0..5 V, INV its opposite (5 V when quiet: the CV of a VCA that ducks), GATE high
 * above THRESH (with a little hysteresis). */
typedef struct { float e; uint8_t gate; } follow_st;
PROC_COLD(follow_proc)
{
    ST(follow_st);
    (void)connected;
    float att = 1.0f - pw_coef(m->p[0]), rel = 1.0f - pw_coef(m->p[1]), gain = m->p[2] * 0.2f, th = m->p[3];
    for (int i = 0; i < PW_N; ++i) {
        float a = pw_absf(in[0][i]) * gain;
        s->e += (a - s->e) * (a > s->e ? att : rel);
        float v = pw_clamp(s->e, 0.0f, 1.0f);
        if (!s->gate && v > th) s->gate = 1;
        else if (s->gate && v < th * 0.7f) s->gate = 0;
        out[0][i] = 5.0f * v; out[1][i] = 5.0f - 5.0f * v; out[2][i] = s->gate ? 5.0f : 0.0f;
    }
    if (s->e != s->e || s->e < 1e-20f) s->e = 0;
}

/* VOCODER: twelve bands from 120 Hz to 6 kHz. Each band of the CARRIER is turned up and down by how loud the same band of the MOD (the modulator: a voice,
 * drums, anything with a moving spectrum) is, so the carrier takes on its shape. With nothing on MOD it is the carrier's own spectrum, but each band is driven by
 * its neighbour (odd bands by even ones, as on Spectra's spectrum transfer): a single sound talks to itself. SHIFT moves the modulator's bands up or down (a
 * formant shift), FOCUS is how narrow the bands are, BRIGHT tilts the carrier toward the highs (a vocoder's consonants live there), RELEASE how long a band
 * holds after its modulator falls. Each band is two state-variable filters (carrier, modulator) and an envelope follower: 60 floats of state. */
#define VOC_N 12
typedef struct { float cs1[VOC_N], cs2[VOC_N], ms1[VOC_N], ms2[VOC_N], env[VOC_N]; } voc_st;
static const float voc_hz[VOC_N] PW_DSP_DATA = {120, 172, 245, 351, 502, 718, 1026, 1467, 2098, 3000, 4290, 6135};
PROC(voc_proc)
{
    ST(voc_st);
    if (!IN_CONNECTED(0)) { for (int i = 0; i < PW_N; ++i) out[0][i] = 0.0f; return; }
    const int self = !IN_CONNECTED(1);
    const float att = 1.0f - pw_coef(0.002f), rel = 1.0f - pw_coef(m->p[0]);
    const float kd = 1.0f / m->p[2], gain = m->p[4] * 0.314f, mix = m->p[5];   /* (0.314: an envelope of the average |x| back to a peak, per 5 V) */
    const int shift = (int)(m->p[1] + (m->p[1] < 0 ? -0.5f : 0.5f));
    float a1[VOC_N], a2[VOC_N], a3[VOC_N], tilt[VOC_N];
    int src[VOC_N];
    for (int k = 0; k < VOC_N; ++k) {   /* the TPT state-variable filter's coefficients for band k */
        float f = pw_minf(voc_hz[k], PW_SRF * 0.45f), w = 0.5f * f * PW_INV_SR, g = pw_sin1(w) / pw_maxf(pw_cos1(w), 0.05f);
        a1[k] = 1.0f / (1.0f + g * (g + kd)); a2[k] = g * a1[k]; a3[k] = g * a2[k];
        tilt[k] = gain * pw_exp2(m->p[3] * ((float)k - 5.5f) * 0.4f);
        int j = (self ? (k ^ 1) : k) + shift; src[k] = j < 0 ? 0 : j >= VOC_N ? VOC_N - 1 : j;
    }
    for (int i = 0; i < PW_N; ++i) {
        const float x = in[0][i], mi = self ? x : in[1][i];
        float bc[VOC_N], bm[VOC_N], y = 0.0f;
        for (int k = 0; k < VOC_N; ++k) {
            float v3 = x - s->cs2[k], v1 = a1[k] * s->cs1[k] + a2[k] * v3, v2 = s->cs2[k] + a2[k] * s->cs1[k] + a3[k] * v3;
            s->cs1[k] = 2.0f * v1 - s->cs1[k]; s->cs2[k] = 2.0f * v2 - s->cs2[k];
            bc[k] = kd * v1;
            if (self) { bm[k] = bc[k]; continue; }
            v3 = mi - s->ms2[k]; v1 = a1[k] * s->ms1[k] + a2[k] * v3; v2 = s->ms2[k] + a2[k] * s->ms1[k] + a3[k] * v3;
            s->ms1[k] = 2.0f * v1 - s->ms1[k]; s->ms2[k] = 2.0f * v2 - s->ms2[k];
            bm[k] = kd * v1;
        }
        for (int k = 0; k < VOC_N; ++k) {
            float a = pw_absf(bm[k]);
            s->env[k] += (a - s->env[k]) * (a > s->env[k] ? att : rel);
        }
        for (int k = 0; k < VOC_N; ++k) y += bc[k] * s->env[src[k]] * tilt[k];
        out[0][i] = x + (y - x) * mix;
    }
    for (int k = 0; k < VOC_N; ++k) {
        if (!(s->env[k] == s->env[k]) || s->env[k] < 1e-20f) s->env[k] = 0;
        if (!(s->cs1[k] == s->cs1[k]) || pw_absf(s->cs1[k]) < 1e-20f) s->cs1[k] = 0;
        if (!(s->cs2[k] == s->cs2[k]) || pw_absf(s->cs2[k]) < 1e-20f) s->cs2[k] = 0;
        if (!(s->ms1[k] == s->ms1[k]) || pw_absf(s->ms1[k]) < 1e-20f) s->ms1[k] = 0;
        if (!(s->ms2[k] == s->ms2[k]) || pw_absf(s->ms2[k]) < 1e-20f) s->ms2[k] = 0;
    }
}

/* MUTATE: a pulse on NUDGE or ROLL nudges or rolls the patch's knobs and sequencer steps (the MUTATE page does the same by hand); AMOUNT (and its CV) says how far,
 * DENSITY how many knobs and steps are touched. The pulses are only counted here: the screen's loop does the changing (pw_mutate_follow), and TRIG pulses
 * when one came. Modules locked on the MUTATE page are left alone. */
typedef struct { float nc, rc, amt; uint32_t pulse; uint8_t hn, hr; } mut_st;
PROC_COLD(mutate_proc)
{
    ST(mut_st);
    float a0 = m->p[0];
    uint32_t hold = (uint32_t)(0.02f * PW_SRF);
    if (!s->amt) s->amt = a0;
    for (int i = 0; i < PW_N; ++i) {
        float amt = a0 + (IN_CONNECTED(2) ? in[2][i] * 0.1f : 0.0f);
        amt = pw_clamp(amt, 0.0f, 1.0f);
        float n = IN_CONNECTED(0) ? in[0][i] : 0.0f, r = IN_CONNECTED(1) ? in[1][i] : 0.0f;
        if (!s->hn && n > 1.0f) { s->hn = 1; s->nc += 1.0f; s->amt = amt; s->pulse = hold; }
        else if (s->hn && n < 0.5f) s->hn = 0;
        if (!s->hr && r > 1.0f) { s->hr = 1; s->rc += 1.0f; s->amt = amt; s->pulse = hold; }
        else if (s->hr && r < 0.5f) s->hr = 0;
        out[0][i] = s->pulse ? 5.0f : 0.0f;
        if (s->pulse) --s->pulse;
    }
}

/* FRACTURE: stutters, repeats, reverses. It keeps the last half second; a glitch freezes a slice of it (a
 * fraction of a beat at the tempo) and repeats it, maybe backwards, maybe sliding in pitch, fading. A glitch
 * happens while GATE is high, or by chance on each CLOCK (unpatched: each sixteenth of the tempo). */
typedef struct {
    uint32_t w, start; float pos, rate, gain, fade, tick;
    int32_t len, left; uint8_t active, rev, gate_hi, clk_hi, held; uint32_t seed;
} frac_st;
static const float frac_beats[6] PW_DSP_DATA = {0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f, 0.015625f};
static void frac_start(pw_module *m, frac_st *s, uint32_t len_buf, int repeats)
{
    float beats = frac_beats[(int)pw_clamp(m->p[0] + 0.5f, 0.0f, 5.0f)];
    int32_t len = (int32_t)(beats * 60.0f / pws.bpm * PW_SRF);
    if (len < 64) len = 64;
    if (len > (int32_t)len_buf - 256) len = (int32_t)len_buf - 256;
    s->len = len;
    s->start = (s->w + len_buf - (uint32_t)len) % len_buf;
    s->active = 1; s->left = repeats; s->gain = 1.0f; s->rate = 1.0f;
    s->rev = (pw_rand_u(&s->seed) >> 8 & 0xFFFFu) < (unsigned)(m->p[2] * 65536.0f);
    s->pos = s->rev ? (float)(len - 1) : 0;
}
PROC(fracture_proc)
{
    ST(frac_st);
    int16_t *buf = PW_POOL16(m);
    uint32_t len = m->mem_len / 2u;
    if (m->status != PW_STATUS_OK || len < 2048) { pass_through(in, out, 1); return; }
    float chance = m->p[1], semis = m->p[3], fade = m->p[4], mix = m->p[5];
    float tick_inc = pws.bpm * (4.0f / 60.0f) * PW_INV_SR;
    for (int i = 0; i < PW_N; ++i) {
        int decide = 0;
        if (IN_CONNECTED(1)) {
            uint8_t was = s->gate_hi;
            if (pw_rise(&s->gate_hi, in[1][i]) && !s->active) { frac_start(m, s, len, 1 << 20); s->held = 1; }
            if (was && !s->gate_hi && s->held) { s->left = 0; s->held = 0; }
        }
        if (IN_CONNECTED(2)) decide = pw_rise(&s->clk_hi, in[2][i]);
        else if (pws.playing) decide = pws.tick_at[i];
        else { s->tick += tick_inc; if (s->tick >= 1.0f) { s->tick -= 1.0f; decide = 1; } }
        if (decide && !s->held && !s->active && chance > 0 &&
            (pw_rand_u(&s->seed) >> 8 & 0xFFFFu) < (unsigned)(chance * 65536.0f))
            frac_start(m, s, len, 1 + (int)(pw_rand_u(&s->seed) >> 29));   /* 2 to 8 times */
        if (!s->active) buf[s->w] = dl_word(in[0][i]);
        if (++s->w >= len) s->w = 0;
        float wet = 0;
        if (s->active) {
            int32_t p = (int32_t)s->pos;
            if (p < 0) p = 0;
            if (p >= s->len) p = s->len - 1;
            wet = buf[(s->start + (uint32_t)p) % len] * (1.0f / DL_SCALE);
            /* a short fade at the slice's ends: no clicks */
            int32_t edge = s->rev ? p : s->len - 1 - p, head = s->rev ? s->len - 1 - p : p;
            float e = pw_minf(1.0f, (float)(edge < head ? edge : head) * (1.0f / 48.0f));
            s->fade += (1.0f - s->fade) * 0.02f;
            wet *= e * s->gain;
            s->pos += s->rev ? -s->rate : s->rate;
            if (s->pos >= (float)s->len || s->pos < 0) {
                if (--s->left <= 0 && !s->held) s->active = 0;
                s->gain *= 1.0f - fade * 0.45f;
                s->rate *= pw_exp2(semis / 12.0f);
                s->rate = pw_clamp(s->rate, 0.125f, 4.0f);
                s->pos = s->rev ? (float)(s->len - 1) : 0;
            }
        } else s->fade += (0.0f - s->fade) * 0.02f;
        out[0][i] = in[0][i] * (1.0f - s->fade * mix) + wet * mix;
    }
}

/* AUDIO IN: the platform's input (the FM-1: the computer's audio over USB) */
PROC(audioin_proc)
{
    (void)in; (void)connected;
    float g = m->p[0] * 5.0f;
    if (!pw_plat_audio_in(out[0], out[1])) { for (int i = 0; i < PW_N; ++i) { out[0][i] = 0; out[1][i] = 0; } return; }
    if (PW_N < PW_OUT) for (int i = 0; i < PW_N; ++i) { out[0][i] = 0.5f * (out[0][2 * i] + out[0][2 * i + 1]); out[1][i] = 0.5f * (out[1][2 * i] + out[1][2 * i + 1]); }   /* (22.05 kHz: pairs averaged) */
    for (int i = 0; i < PW_N; ++i) { out[0][i] *= g; out[1][i] *= g; }
}

/* EUCLID: HITS spread as evenly as they go over STEPS, turned by ROTATE */
typedef struct { int32_t step, trig, since, period; uint8_t clk_hi, rst_hi, on; } eucl_st;
PROC_COLD(euclid_proc)
{
    ST(eucl_st);
    int steps = (int)(m->p[0] + 0.5f), hits = (int)(m->p[1] + 0.5f), rot = (int)(m->p[2] + 0.5f);
    float width = m->p[3];
    if (steps < 1) steps = 1;
    if (hits > steps) hits = steps;
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(1) && pw_rise(&s->rst_hi, in[1][i])) s->step = -1;
        if (pws.reset_now && i == 0 && !IN_CONNECTED(0)) s->step = -1;
        int clock = IN_CONNECTED(0) ? pw_rise(&s->clk_hi, in[0][i]) : pws.tick_at[i];
        s->since++;
        if (clock) {
            if (s->since > 8) s->period = s->since;
            s->since = 0;
            s->step = (s->step + 1) % steps;
            int k = ((s->step + steps - rot % steps) % steps);
            s->on = hits > 0 && (k * hits) % steps < hits;
            if (s->on) s->trig = 88;
        }
        int gate = s->on && (float)s->since < (float)(s->period > 0 ? s->period : 5512) * width;
        out[0][i] = gate ? 5.0f : 0.0f;
        out[1][i] = s->trig ? 5.0f : 0.0f;
        out[2][i] = !s->on && (float)s->since < (float)(s->period > 0 ? s->period : 5512) * width && s->step >= 0 ? 5.0f : 0.0f;
        if (s->trig) --s->trig;
    }
}

/* STRING: a plucked string (Karplus-Strong): a burst of noise circulating in a tuned delay through a
 * low-pass; EXCITE feeds the string continuously (bow it with noise, strike it with anything) */
/* MODAL PERCUSSION (it was RESONATOR: those patches load as it is): something struck that rings. MODEL STRING: a plucked
 * string (Karplus-Strong: a delay line one period long, filtered and fed back); the others are modal synthesis, eight
 * tuned resonances whose frequencies are the object's (STRUCT stretches or squeezes them) and whose loudness comes from
 * where it is struck (POS): BELL, BAR, PLATE, DRUM (a membrane), TUBE (closed pipe), WOOD (a block), SHEET (metal sheet),
 * SPRING (a dense, falling twang), PIPE (open pipe), GONG and TIMPANI. The strike is a burst of NOISE, a CLICK or a
 * soft MALLET (STRIKE: soft to hard); BEND drops (or lifts) the pitch as it is hit, the way a tom does. TRIG strikes it,
 * EXCITE rings it with any sound. DECAY is how long it rings, BRIGHT how long its high partials last. */
#define RES_MODES 8
#define RES_MODELS 12
static const float res_ratio[RES_MODELS - 1][RES_MODES] PW_DSP_DATA = {
    {0.5f, 1.0f, 1.19f, 1.5f, 2.0f, 2.5f, 2.66f, 3.0f},                 /* BELL: hum, prime, tierce, quint, nominal... */
    {1.0f, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f, 24.81f, 31.87f},     /* BAR: a free bar */
    {1.0f, 2.32f, 2.66f, 4.18f, 4.64f, 5.53f, 6.22f, 7.0f},              /* PLATE */
    {1.0f, 1.594f, 2.136f, 2.296f, 2.653f, 2.918f, 3.156f, 3.501f},     /* DRUM: a membrane */
    {1.0f, 3.0f, 5.0f, 7.0f, 9.0f, 11.0f, 13.0f, 15.0f},                /* TUBE: closed at one end, odd harmonics */
    {1.0f, 2.52f, 4.07f, 5.64f, 7.51f, 9.4f, 11.8f, 14.2f},             /* WOOD: a hollow block, partials close and short */
    {1.0f, 1.43f, 1.86f, 2.37f, 2.91f, 3.52f, 4.27f, 5.18f},            /* SHEET: a metal sheet, dense and inharmonic */
    {1.0f, 1.12f, 1.27f, 1.46f, 1.7f, 2.0f, 2.37f, 2.83f},              /* SPRING: crowded, and the high ones fall (see res_dscale) */
    {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f},                   /* PIPE: open at both ends, every harmonic */
    {0.5f, 1.0f, 1.58f, 1.96f, 2.58f, 3.31f, 3.87f, 4.75f},             /* GONG: a hum, then a cloud */
    {1.0f, 1.5f, 1.99f, 2.44f, 2.89f, 3.34f, 3.78f, 4.2f}};             /* TIMPANI: a kettle's modes, nearly a harmonic series */
/* each model's level, measured: struck at C4 they all come out about as loud (the string was far louder) */
static const float res_gain[RES_MODELS] PW_DSP_DATA = {0.55f, 2.6f, 5.4f, 4.2f, 3.5f, 5.4f, 2.8f, 8.0f, 6.0f, 3.6f, 7.5f, 4.0f};
/* each model's own ring time (x DECAY) */
static const float res_dscale[RES_MODELS] PW_DSP_DATA = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.12f, 1.6f, 1.3f, 0.6f, 2.5f, 0.8f};
const float *pw_modal_ratios(unsigned model) { return model >= 1 && model < RES_MODELS ? res_ratio[model - 1] : res_ratio[0]; }
typedef struct {
    uint32_t w; float lp, ap, apx, dc_x, dc_y; uint32_t seed; uint8_t trig_hi; int32_t burst, bn; float nlp, benv, xk;
    float y1[RES_MODES], y2[RES_MODES], a1[RES_MODES], a2[RES_MODES], g[RES_MODES]; float hz_was; int model_was;
} str_st;
static void string_init(pw_module *m) { ST(str_st); s->seed = 0x57A1u + (uint32_t)(m - pws.mod) * 2654435761u; s->model_was = -1; }
/* the strike: NOISE a burst of low-passed noise, CLICK one sharp impulse, MALLET a soft half-sine pulse (harder = shorter) */
static PW_DSP float res_strike(str_st *s, const float *const *in, unsigned connected, int i, float d, float noisy, float burst_len, int exc, float bend)
{
    if (pw_rise(&s->trig_hi, in[0][i])) {
        s->burst = (int32_t)burst_len; s->bn = 0;
        if (exc == 1) s->burst = 1;
        else if (exc == 2) s->burst = (int32_t)(8.0f + 70.0f * (1.0f - noisy));
        s->benv = bend != 0 ? 1.0f : 0.0f;
    }
    float x = IN_CONNECTED(2) ? in[2][i] * 0.05f : 0;
    (void)d;
    if (s->burst > 0) {
        if (exc == 1) x += 11.0f * (0.35f + 0.65f * noisy);
        else if (exc == 2) { s->bn++; x += 1.2f * pw_sin1(0.5f * (float)s->bn / (float)(s->burst + 1)) * (0.4f + 0.6f * noisy); }
        else {
            float n = pw_rand_f(&s->seed);
            s->nlp += (0.15f + 0.85f * noisy) * (n - s->nlp);
            x += s->nlp * 0.9f;
        }
        --s->burst;
    }
    return x;
}
PROC(string_proc)
{
    ST(str_st);
    preset_cv(m, in[3], connected, 3);
    float base = m->p[0] / 12.0f, decay = m->p[1], bright = m->p[2], noisy = m->p[3];
    int model = (int)pw_clamp(m->p[4] + 0.5f, 0.0f, (float)(RES_MODELS - 1));
    int exc = (int)pw_clamp(m->p[8] + 0.5f, 0.0f, 2.0f);
    float bend = m->p[9];
    if (model != s->model_was) { for (int k = 0; k < RES_MODES; ++k) s->y1[k] = s->y2[k] = 0; s->model_was = model; s->hz_was = -1; }
    if (model == 0) {                                   /* STRING */
        float *buf = (float *)(void *)(pw_pool + m->mem_off);
        uint32_t len = m->mem_len / 4u;
        if (m->status != PW_STATUS_OK || len < 256) { for (int i = 0; i < PW_N; ++i) out[0][i] = 0; return; }
        float fb = 0.9f + 0.0995f * decay, k = 0.15f + 0.85f * bright;
        for (int i = 0; i < PW_N; ++i) {
            float hz = pw_pitch_hz(base + (IN_CONNECTED(1) ? in[1][i] : 0) - 1.0f);
            float d = pw_clamp(PW_SRF / hz - 0.5f, 2.0f, (float)(len - 2u));
            float x = res_strike(s, in, connected, i, d, noisy, d, exc, 0);
            float v = dl_read_f(buf, len, s->w, d);
            s->lp += k * (v - s->lp);
            float y = s->lp * fb + x;
            buf[s->w] = pw_clamp(y, -4.0f, 4.0f);
            if (++s->w >= len) s->w = 0;
            float o = y - s->dc_x + 0.995f * s->dc_y; s->dc_x = y; s->dc_y = o;
            out[0][i] = 5.0f * res_gain[0] * o;
        }
        if (pw_absf(s->lp) < 1e-20f) s->lp = 0;
        if (pw_absf(s->dc_y) < 1e-20f) s->dc_y = 0;
        return;
    }
    /* modal: the eight resonances' coefficients once a block */
    const float *ratio = res_ratio[model - 1];
    float stretch = (m->p[5] - 0.5f) * 0.5f, pos = pw_clamp(m->p[6], 0.02f, 0.98f);
    float bv = s->benv;
    float hz = pw_pitch_hz(base + (IN_CONNECTED(1) ? in[1][0] : 0) - 1.0f + bend * bv * (1.0f / 12.0f));
    float t60 = 0.05f + 6.0f * decay * decay * res_dscale[model];
    float lvl = 0.6f * res_gain[model] * pw_exp2(0.5f * pw_log2(pw_maxf(hz, 20.0f) / 261.6f));   /* (higher: a little more, as the string does not) */
    if (bv > 0.0005f) s->benv = bv * pw_exp2(-1.442695f * (float)PW_N / (0.07f * PW_SRF)); else s->benv = 0;   /* (the bend dies away in about a tenth of a second) */
    for (int k = 0; k < RES_MODES; ++k) {
        float r0 = ratio[k];
        float f = hz * r0 * pw_exp2(stretch * pw_log2(r0 > 0.0f ? r0 : 1.0f));
        if (f > PW_SRF * 0.45f) { s->a1[k] = s->a2[k] = s->g[k] = 0; continue; }
        float tk = t60 / (1.0f + (float)k * (1.0f - bright) * (model == 8 ? 3.0f : 1.5f));   /* the high partials die sooner, the darker it is (a spring's far sooner) */
        float r = pw_exp2(-1.442695f * 6.9f / (tk * PW_SRF));
        s->a1[k] = 2.0f * r * pw_cos1(f * PW_INV_SR);
        s->a2[k] = -r * r;
        float amp = pw_sin1(0.5f * (float)(k + 1) * pos);              /* struck at POS along it */
        s->g[k] = pw_absf(amp) * (1.0f - r) * 4.0f / (1.0f + 0.15f * (float)k);
    }
    for (int i = 0; i < PW_N; ++i) {
        float x = res_strike(s, in, connected, i, 0, noisy, 64.0f + 400.0f * (1.0f - noisy), exc, bend);
        float y = 0;
        for (int k = 0; k < RES_MODES; ++k) {
            float v = s->a1[k] * s->y1[k] + s->a2[k] * s->y2[k] + s->g[k] * x;
            s->y2[k] = s->y1[k]; s->y1[k] = v;
            y += v;
        }
        out[0][i] = 5.0f * pw_tanh(y * lvl);
    }
    for (int k = 0; k < RES_MODES; ++k) { if (pw_absf(s->y1[k]) < 1e-20f) s->y1[k] = 0; if (pw_absf(s->y2[k]) < 1e-20f) s->y2[k] = 0; }
}

/* GRANULAR: a cloud of short grains read from what comes in (the last PW_GRAIN_SECONDS of it, kept in a loop) or
 * from a bank sound (SOURCE: 1-16 INTERNAL, 17-24 A, 25-32 B, 33-40 C, 41-48 D). DENSITY grains a second (or one each TRIG), each SIZE long, from POS back in
 * time (into the sound), scattered by SPREAD, pitched by PITCH (+ V/OCT) and a random DETUNE, panned across WIDTH.
 * FREEZE (or its gate) stops recording: the cloud keeps playing what was there. */
#define GR_N 8
typedef struct { float pos, inc, age, len, gl, gr; uint8_t on; } grain_t;
typedef struct { grain_t g[GR_N]; uint32_t w, filled, seed; float ph; uint8_t trig_hi; } gran_st;
static void gran_init(pw_module *m) { ST(gran_st); s->seed = 0x6EA1u + (uint32_t)(m - pws.mod) * 2654435761u; }
PROC(gran_proc)
{
    ST(gran_st);
    int16_t *buf = PW_POOL16(m);
    uint32_t len = m->status == PW_STATUS_OK ? m->mem_len / 2u : 0u;
    int src = (int)(m->p[8] + 0.5f);
    uint32_t frames = 0, rate = 44100;
    pw_smp smp;
    int have = src > 0 && pw_smp_open((unsigned)src - 1u, &smp);
    if (have) { frames = smp.frames; rate = smp.rate; }
    if (src > 0 && !have) frames = 0;
    pw_rdr *rd[GR_N] = {0};   /* (each grain's place in a compressed sound) */
    float dens = m->p[2], size = m->p[1] * PW_SRF, mix = m->p[7], width = m->p[5], spread = m->p[4];
    { float h = sync_hz(m->p[10]); if (h > 0) dens = h; }   /* SYNC: the grains' rate as a note value */
    float pitch = m->p[3] / 12.0f + (IN_CONNECTED(3) ? in[3][0] : 0.0f), detune = m->p[9];
    int freeze = m->p[6] > 0.5f || (IN_CONNECTED(1) && in[1][0] > 1.0f);
    float pos = pw_clamp(m->p[0] + (IN_CONNECTED(2) ? in[2][0] * 0.2f : 0.0f), 0.0f, 1.0f);
    float overlap = pw_maxf(1.0f, dens * m->p[1]), gain = 1.0f / (0.6f + 0.4f * overlap);
    for (int i = 0; i < PW_N; ++i) {
        float x = in[0][i];
        if (len && !freeze && src <= 0) { buf[s->w] = dl_word(x); if (++s->w >= len) s->w = 0; if (s->filled < len) ++s->filled; }
        int fire = 0;
        if (IN_CONNECTED(4)) fire = pw_rise(&s->trig_hi, in[4][i]);
        else { s->ph += dens * PW_INV_SR; if (s->ph >= 1.0f) { s->ph -= 1.0f; fire = 1; } }
        uint32_t span = src > 0 ? frames : s->filled;
        if (fire && span > 2048u) {
            int k = 0; while (k < GR_N && s->g[k].on) ++k;
            if (k < GR_N) {
                grain_t *g = &s->g[k];
                float r1 = pw_rand_f(&s->seed), r2 = pw_rand_f(&s->seed), r3 = pw_rand_f(&s->seed);
                g->inc = pw_exp2(pitch + detune * r2 * 0.5f) * (src > 0 ? (float)rate * PW_INV_SR : 1.0f);
                g->len = size; g->age = 0;
                float p = pw_clamp(pos + spread * r1 * 0.5f, 0.0f, 1.0f);
                if (src > 0) g->pos = p * (float)(frames - 1u);
                else {   /* POS back from now: far enough that a fast grain never reaches the recording head */
                    float back = 64.0f + p * (float)(span - 64u) + (g->inc > 1.0f ? (g->inc - 1.0f) * size : 0.0f);
                    if (back > (float)span - 2.0f) back = (float)span - 2.0f;
                    g->pos = (float)s->w - back; if (g->pos < 0) g->pos += (float)len;
                }
                float pan = 0.5f + 0.5f * width * r3;
                g->gl = 1.0f - pan; g->gr = pan; g->on = 1;
            }
        }
        float l = 0, r = 0;
        for (int k = 0; k < GR_N; ++k) {
            grain_t *g = &s->g[k];
            if (!g->on) continue;
            float u = g->age / g->len, wv = pw_sin1(0.5f * u); wv *= wv;
            float v;
            if (src > 0) {
                uint32_t i0 = (uint32_t)g->pos;
                if (i0 + 1u >= frames) { g->on = 0; continue; }
                { if (!rd[k]) rd[k] = pw_rdr_get(((unsigned)(m - pws.mod) << 4 | (unsigned)k) + 1u, &smp);
                  float a0 = (float)pw_smp_get(&smp, rd[k], i0); v = (a0 + ((float)pw_smp_get(&smp, rd[k], i0 + 1u) - a0) * (g->pos - (float)i0)) * (1.0f / 32768.0f) * 5.0f; }
            } else {
                uint32_t i0 = (uint32_t)g->pos % len, i1 = i0 + 1u >= len ? 0u : i0 + 1u;
                v = ((float)buf[i0] + ((float)buf[i1] - (float)buf[i0]) * (g->pos - (float)(uint32_t)g->pos)) / DL_SCALE;
            }
            v *= wv;
            l += v * g->gl; r += v * g->gr;
            g->pos += g->inc;
            if (src <= 0 && g->pos >= (float)len) g->pos -= (float)len;
            g->age += 1.0f;
            if (g->age >= g->len) g->on = 0;
        }
        float dry = src > 0 ? 0.0f : x * (1.0f - mix);
        out[0][i] = dry + l * gain * mix * 1.4f;
        out[1][i] = dry + r * gain * mix * 1.4f;
    }
}

/* the sound the grains come from, as n peaks (0-255) left to right: a bank sound start to end, or the live recording past to now (the display) */
int pw_grain_peaks(unsigned slot, uint8_t *pk, unsigned n)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || pws.mod[slot].type != PW_T_GRAIN || !n) return 0;
    pw_module *m = &pws.mod[slot];
    gran_st *s = (gran_st *)m->stp;
    int src = (int)(m->p[8] + 0.5f);
    if (src > 0) {
        pw_smp smp;
        if (!pw_smp_open((unsigned)src - 1u, &smp) || smp.frames < n) return 0;
        pw_smp_peaks(&smp, pk, n, 0);
        return 1;
    }
    if (m->status != PW_STATUS_OK) return 0;
    const int16_t *buf = PW_POOL16(m);
    uint32_t len = m->mem_len / 2u;
    if (len < n * 16u || !s->filled) return 0;
    uint32_t per = len / n, st = per / 16u + 1u;
    for (unsigned c = 0; c < n; ++c) {
        int pkv = 0;
        for (uint32_t k = 0; k < per; k += st) {
            uint32_t at = c * per + k, back = len - at;   /* (how far back from the write point) */
            if (back > s->filled) continue;
            int a = buf[(s->w + at) % len]; if (a < 0) a = -a;
            if (a > pkv) pkv = a;
        }
        pkv = (int)((float)pkv * (255.0f / (5.0f * DL_SCALE)));
        pk[c] = (uint8_t)(pkv > 255 ? 255 : pkv);
    }
    return 1;
}

/* TAPE: a tape machine's character. DRIVE saturates, WOW (slow) and FLUTTER (fast) wobble the speed, AGE dulls the
 * top end, HISS adds tape noise; STOP (its gate) brakes the tape to a halt in STOP's time, the pitch falling with
 * it, and lets go with a quick splice back. MIX against the dry sound. */
typedef struct { uint32_t w, seed; float d, spd, wph, fph, wdrift, lp, hp, hpx, fade, fd, eff; uint8_t stopped; } tape_st;
static void tape_init(pw_module *m) { ST(tape_st); s->seed = 0x7A9Eu + (uint32_t)(m - pws.mod) * 2654435761u; s->spd = 1.0f; s->d = 220.0f; }
PROC(tape_proc)
{
    ST(tape_st);
    int16_t *buf = PW_POOL16(m);
    uint32_t len = m->status == PW_STATUS_OK ? m->mem_len / 2u : 0u;
    float drive = m->p[0], wow = m->p[1], flut = m->p[2], age = m->p[3], hiss = m->p[4], stop_t = m->p[5], mix = m->p[6];
    float pre = 1.0f + drive * 5.0f, post = (1.0f + drive * 0.6f) / pre;
    float lpk = pw_lp_coef_hz(16000.0f * pw_exp2(-3.0f * age));
    float ramp = 1.0f / (pw_maxf(stop_t, 0.05f) * PW_SRF);
    const float base = 220.0f, vs0 = m->p[7] / 12.0f;   /* SPEED: varispeed, in semitones (and a 1 V/oct input) */
    if (!len || len < 1024u) { for (int i = 0; i < PW_N; ++i) out[0][i] = in[0][i]; return; }
    for (int i = 0; i < PW_N; ++i) {
        float x = in[0][i];
        float sat = pw_tanh(x * 0.2f * pre) * 5.0f * post;
        buf[s->w] = dl_word(sat);
        int stop = IN_CONNECTED(1) && in[1][i] > 1.0f;
        if (stop) { s->spd -= ramp; if (s->spd < 0) s->spd = 0; s->stopped = 1; }
        else if (s->spd < 1.0f) { s->spd += ramp * 4.0f; if (s->spd > 1.0f) s->spd = 1.0f; }
        if (s->stopped && !stop && s->spd >= 1.0f) { s->stopped = 0; s->fd = s->d; s->d = base; s->fade = 1.0f; }   /* splice */
        /* wow and flutter: the read point's distance swings */
        float wa = (wow + (IN_CONNECTED(2) ? in[2][i] * 0.2f : 0.0f));
        s->wph += (0.55f + s->wdrift) * PW_INV_SR; if (s->wph >= 1.0f) { s->wph -= 1.0f; s->wdrift = 0.3f * pw_rand_f(&s->seed); }
        s->fph += 9.0f * PW_INV_SR; if (s->fph >= 1.0f) s->fph -= 1.0f;
        float mod = wa * 130.0f * (1.0f + pw_sin1(s->wph)) + flut * 12.0f * (1.0f + pw_sin1(s->fph));
        float vs = (vs0 != 0.0f || IN_CONNECTED(3)) ? pw_exp2(pw_clamp(vs0 + (IN_CONNECTED(3) ? in[3][i] : 0.0f), -2.0f, 1.0f)) : 1.0f;
        s->eff = s->spd * vs;
        s->d += 1.0f - s->spd * vs;
        float maxd = (float)len - 4.0f - 300.0f;
        /* varispeed runs the read point into the write point (or out of the tape): it jumps over a stretch, spliced */
        if (vs != 1.0f && s->spd > 0.5f && maxd > 12000.0f) {
            if (s->d < 60.0f) { s->fd = s->d; s->d += 8000.0f; s->fade = 1.0f; }
            else if (s->d > maxd - 100.0f) { s->fd = s->d; s->d -= 8000.0f; s->fade = 1.0f; }
        }
        if (s->d > maxd) s->d = maxd;
        if (s->d < 4.0f) s->d = 4.0f;
        float y = s->spd <= 0.0f && s->d >= maxd ? 0.0f : dl_read(buf, len, s->w, s->d + mod);
        if (s->fade > 0) { y = y * (1.0f - s->fade) + dl_read(buf, len, s->w, s->fd + mod) * s->fade; s->fade -= 1.0f / 512.0f; }
        if (++s->w >= len) s->w = 0;
        s->lp += lpk * (y - s->lp);
        float h = pw_rand_f(&s->seed);
        s->hp = 0.9f * (s->hp + h - s->hpx); s->hpx = h;      /* hiss: high-passed noise */
        float wet = s->lp + s->hp * hiss * hiss * 0.25f;
        out[0][i] = x * (1.0f - mix) + wet * mix;
    }
    if (pw_absf(s->lp) < 1e-20f) s->lp = 0;
}

/* SCOPE: A and B drawn on its page (the last PW_SCOPE_RING samples of each kept); nothing patched, the page shows the
 * output under the patch screen's cursor. ZOOM the time across the screen, V/DIV the height, TRIG what it waits for. */
typedef struct { uint32_t w; } scope_st;
PROC_COLD(scope_proc)
{
    ST(scope_st);
    int16_t *b = PW_POOL16(m);
    (void)out; (void)connected;
    if (m->status != PW_STATUS_OK || m->mem_len < PW_SCOPE_RING * 4u) return;
    for (int i = 0; i < PW_N; ++i) {
        b[s->w] = dl_word(in[0][i]); b[PW_SCOPE_RING + s->w] = dl_word(in[1][i]);
        s->w = (s->w + 1u) & (PW_SCOPE_RING - 1u);
    }
}
const int16_t *pw_scope_ring(unsigned slot, unsigned ch, uint32_t *w)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || pws.mod[slot].type != PW_T_SCOPE || ch > 1) return 0;
    const pw_module *m = &pws.mod[slot];
    if (m->status != PW_STATUS_OK || m->mem_len < PW_SCOPE_RING * 4u) return 0;
    *w = ((const scope_st *)(const void *)m->st.raw)->w;
    return (const int16_t *)(const void *)(pw_pool + m->mem_off) + ch * PW_SCOPE_RING;
}

/* CV RECORDER: records a voltage (IN; unpatched, its own KNOB) over LENGTH beats and plays it back in a loop, in time
 * with the tempo or a clock (each CLOCK pulse a sixteenth). MODE REC records one pass (then it plays), DUB blends a
 * new pass with what was there; REC's gate records while high. SPEED half, double or backwards; SMOOTH rounds the
 * steps; LEVEL and OFFSET scale it. 48 points a pass, kept in the patch (each variation its own). */
#define CVR_N 48
typedef struct { float ph, period, since, sm, out, eoc; uint32_t rec_left; uint8_t clk_hi, rst_hi, rec_was, dub; int32_t last_j; } cvr_st;
static void cvr_init(pw_module *m) { ST(cvr_st); s->last_j = -1; for (int k = 0; k < CVR_N; ++k) m->data[k] = 128; }
static uint8_t cvr_q(float v) { return (uint8_t)pw_clamp(v * (127.5f / 5.0f) + 127.5f, 0.0f, 255.0f); }
PROC_COLD(cvr_proc)
{
    ST(cvr_st);
    int mode = (int)(m->p[0] + 0.5f), beats = (int)(m->p[1] + 0.5f), spm = (int)(m->p[4] + 0.5f);
    float speed = spm == 0 ? 0.5f : spm == 2 ? 2.0f : spm == 3 ? -1.0f : 1.0f;
    float loop = (float)(beats < 1 ? 1 : beats) * 60.0f / pw_maxf(pws.bpm, 20.0f) * PW_SRF;
    float smc = pw_coef(m->p[3] * m->p[3] * 0.25f);
    if (pws.reset_now && !IN_CONNECTED(2)) s->ph = 0;
    int gate_rec = IN_CONNECTED(1);
    if (gate_rec) s->dub = (uint8_t)(mode == 2);
    if (mode >= 1 && !s->rec_was) { s->rec_left = (uint32_t)(loop / pw_absf(speed)); s->dub = (uint8_t)(mode == 2); }
    s->rec_was = (uint8_t)(mode >= 1);
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(3) && pw_rise(&s->rst_hi, in[3][i])) s->ph = speed < 0 ? 0.9999f : 0.0f;
        float inc = speed / loop;
        if (IN_CONNECTED(2)) {    /* a clock: each pulse a sixteenth, the phase locked to them */
            s->since += 1.0f;
            if (pw_rise(&s->clk_hi, in[2][i])) {
                if (s->since > 32.0f) s->period = s->since;
                s->since = 0;
                float stepn = 1.0f / (float)(beats * 4);
                float snapped = (float)pw_floori(s->ph / stepn + 0.5f) * stepn;
                s->ph = snapped;
            }
            if (s->period > 0) inc = speed / (s->period * (float)(beats * 4));
        }
        float prev = s->ph;
        s->ph += inc;
        if (s->ph >= 1.0f || s->ph < 0.0f) { s->ph -= (float)pw_floori(s->ph); s->eoc = 88; }
        (void)prev;
        float pj = s->ph * (float)CVR_N;
        int j = (int)pj; if (j >= CVR_N) j = CVR_N - 1;
        float src = IN_CONNECTED(0) ? in[0][i] : m->p[2];
        int recording = gate_rec ? in[1][i] > 1.0f : (s->rec_left > 0 && mode >= 1);
        if (recording) {
            if (j != s->last_j) {
                uint8_t q = cvr_q(src);
                m->data[j] = s->dub ? (uint8_t)(((unsigned)m->data[j] + q + 1u) / 2u) : q;
            }
            if (!gate_rec && --s->rec_left == 0) { m->p[0] = 0.0f; pws.revision++; }   /* one pass: then it plays */
        }
        s->last_j = j;
        int j1 = j + 1 >= CVR_N ? 0 : j + 1;
        float a = ((float)m->data[j] - 127.5f) * (5.0f / 127.5f), b = ((float)m->data[j1] - 127.5f) * (5.0f / 127.5f);
        float v = recording ? src : a + (b - a) * (pj - (float)j);
        s->sm = v + (s->sm - v) * smc;
        out[0][i] = s->sm * m->p[5] + m->p[6];
        out[1][i] = s->eoc > 0 ? 5.0f : 0.0f;
        if (s->eoc > 0) s->eoc -= 1.0f;
    }
}

/* PARAM MOD: four channels, each a voltage (CV 1-4) that moves one knob of one module: MODULE picks the module (0 none; the knob
 * turns through the modules there are), PARAM which of its knobs, DEPTH how far a 5 V swing moves it (of the knob's whole range,
 * either way; a negative depth turns it over), SMOOTH how slowly it follows. The knob keeps its own value: the offset is added just
 * before the module runs and taken off after. The voltage is worked out for each sixteen samples (2.7 kHz); a module that can be run in
 * pieces (pw_pm_steppable) runs sixteen samples at a time with the offset of its own piece, so the knob follows the voltage at that rate;
 * the others get the block's mean. A module with a knob moved this way runs on this core, not the second one. */
uint64_t pw_pm_mask;
#define PM_Q (PW_NMAX / 16)
typedef struct { float sm[4]; float ch[4][PM_Q]; uint8_t on; } pmod_st;
PROC_COLD(pmod_proc)
{
    ST(pmod_st);
    (void)out;
    float dt = 16.0f * PW_INV_SR;
    unsigned own = (unsigned)(m - pws.mod), nq = ((unsigned)PW_N + 15u) >> 4;
    s->on = 0;
    for (unsigned c = 0; c < 4; ++c) {
        int md = (int)(m->p[4 * c] + 0.5f), par = (int)(m->p[4 * c + 1] + 0.5f);
        if (!IN_CONNECTED(c) || md < 1 || md > PW_MAX_MODULES || (unsigned)(md - 1) == own || pws.mod[md - 1].used != 1 || par < 0 || par >= (int)pw_types[pws.mod[md - 1].type].n_param) {
            s->sm[c] = 0;
            for (unsigned q = 0; q < PM_Q; ++q) s->ch[c][q] = 0;
            continue;
        }
        float tau = m->p[4 * c + 3] * m->p[4 * c + 3] * 2.0f, al = dt / (tau + dt), sm = s->sm[c], depth = m->p[4 * c + 2];
        for (unsigned q = 0; q < nq; ++q) {
            unsigned n = (unsigned)PW_N - 16u * q < 16u ? (unsigned)PW_N - 16u * q : 16u;
            float a = 0;
            for (unsigned i = 0; i < n; ++i) a += in[c][16u * q + i];
            sm += (pw_clamp(a * (0.2f / (float)n), -1.0f, 1.0f) * depth - sm) * al;
            s->ch[c][q] = sm;
        }
        s->sm[c] = sm;
        s->on |= (uint8_t)(1u << c);
        pw_pm_mask |= (uint64_t)1 << (md - 1);
    }
}
/* the modules that can run sixteen samples at a time: they hold nothing that depends on where in the block they are (no ticks, no
 * transport, no keyboard), and what they set up from their knobs is cheap */
int pw_pm_steppable(unsigned type)
{
    switch (type) {
    case PW_T_VCO: case PW_T_LFO: case PW_T_NOISE: case PW_T_WAVETABLE: case PW_T_COMPLEX: case PW_T_FORMANT: case PW_T_ADDITIVE: case PW_T_MACRO: case PW_T_FM4:
    case PW_T_VCF: case PW_T_VCA: case PW_T_FOLDER: case PW_T_HARMTREM: case PW_T_DIST: case PW_T_RING: case PW_T_COMB: case PW_T_VOWEL: case PW_T_EQ: case PW_T_LPG:
    case PW_T_DELAY: case PW_T_COMP: case PW_T_ENV: case PW_T_FUNC: case PW_T_ATTEN: case PW_T_SWIRL: return 1;
    default: return 0;
    }
}
int pw_pm_target(unsigned slot, unsigned ch)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || pws.mod[slot].type != PW_T_PARAMMOD || ch > 3) return -1;
    int md = (int)(pws.mod[slot].p[4 * ch] + 0.5f);
    return md >= 1 && md <= PW_MAX_MODULES && pws.mod[md - 1].used == 1 && (unsigned)(md - 1) != slot ? md - 1 : -1;
}
PW_DSP unsigned pw_pm_apply(unsigned slot, pw_pm_undo *u, unsigned c0, unsigned cn)
{
    unsigned n = 0, q0 = c0 >> 4, q1 = (c0 + cn + 15u) >> 4;
    if (q1 <= q0) q1 = q0 + 1;
    if (q1 > PM_Q) q1 = PM_Q;
    pw_module *t = &pws.mod[slot];
    const pw_type *ty = &pw_types[t->type];
    for (unsigned j = 0; j < PW_MAX_MODULES && n < PW_PM_MAX; ++j) {
        const pw_module *pm = &pws.mod[j];
        if (pm->used != 1 || pm->type != PW_T_PARAMMOD || pm->status != PW_STATUS_OK || (pm->flags & PW_F_BYPASS)) continue;
        const pmod_st *st = (const pmod_st *)(const void *)pm->st.raw;
        for (unsigned c = 0; c < 4 && n < PW_PM_MAX; ++c) {
            if (!((st->on >> c) & 1u) || (int)(pm->p[4 * c] + 0.5f) != (int)slot + 1) continue;
            unsigned par = (unsigned)(pm->p[4 * c + 1] + 0.5f);
            if (par >= ty->n_param) continue;
            const pw_param_desc *d = &ty->param[par];
            float off = 0;
            for (unsigned q = q0; q < q1; ++q) off += st->ch[c][q];
            off *= 1.0f / (float)(q1 - q0);
            float v = t->p[par], nv;
            if (d->kind == PW_EXP) {
                float lo = d->min > 0 ? d->min : 0.001f, r = pw_log2(d->max / lo);
                if (r < 0.01f) r = 1.0f;
                nv = lo * pw_exp2(pw_clamp(pw_log2(pw_maxf(v, lo) / lo) / r + off, 0.0f, 1.0f) * r);
            } else {
                nv = pw_clamp(v + off * (d->max - d->min), d->min, d->max);
                if (d->kind == PW_INT || d->kind == PW_ENUM) nv = (float)pw_floori(nv + 0.5f);
            }
            u[n].par = (uint8_t)par; u[n].orig = v; u[n].wr = nv; ++n;
            t->p[par] = nv;
        }
    }
    return n;
}
PW_DSP void pw_pm_restore(unsigned slot, const pw_pm_undo *u, unsigned n)
{
    pw_module *t = &pws.mod[slot];
    while (n--) if (t->p[u[n].par] == u[n].wr) t->p[u[n].par] = u[n].orig;   /* (a knob turned meanwhile keeps the new value) */
}

/* VOWEL: three band-passes at a voice's formants, morphing A E I O U */
static const float vowel_f[5][3] PW_DSP_DATA = {{800, 1150, 2900}, {400, 1700, 2600}, {270, 2140, 2950},
                                                {450, 800, 2830}, {325, 700, 2530}};
static const float vowel_g[3] PW_DSP_DATA = {1.0f, 0.55f, 0.3f};
typedef struct { float lp[3], bp[3], g[3], q; } vowel_st;
PROC(vowel_proc)
{
    ST(vowel_st);
    float shift = pw_exp2(m->p[1] / 12.0f), mix = m->p[3];
    for (int i = 0; i < PW_N; ++i) {
        if ((i & 15) == 0) {
            float v = pw_clamp(m->p[0] + (IN_CONNECTED(1) ? in[1][i] * 0.8f : 0), 0.0f, 4.0f);
            int a = (int)v; if (a > 3) a = 3;
            float t = v - (float)a;
            for (int k = 0; k < 3; ++k) {
                float f = pw_lerp(vowel_f[a][k], vowel_f[a + 1][k], t) * shift;
                s->g[k] = 2.0f * pw_sin1(pw_clamp(f, 50.0f, 12000.0f) * PW_INV_SR * 0.5f);
            }
            s->q = 1.0f / (1.0f + m->p[2] * 14.0f);
        }
        float x = in[0][i] * 0.2f, y = 0;
        for (int k = 0; k < 3; ++k) {          /* Chamberlin state-variable band-pass */
            float hp = x - s->lp[k] - s->q * 2.0f * s->bp[k];
            s->bp[k] += s->g[k] * hp;
            s->lp[k] += s->g[k] * s->bp[k];
            s->bp[k] = pw_clamp(s->bp[k], -8.0f, 8.0f); s->lp[k] = pw_clamp(s->lp[k], -8.0f, 8.0f);
            y += s->bp[k] * vowel_g[k];
        }
        out[0][i] = 5.0f * (x * (1.0f - mix) + y * mix * 1.5f);
    }
    for (int k = 0; k < 3; ++k) { if (pw_absf(s->bp[k]) < 1e-20f) s->bp[k] = 0; if (pw_absf(s->lp[k]) < 1e-20f) s->lp[k] = 0; }
}

/* COMPRESSOR: a peak detector, a gain computer in decibels; SIDE patched: it ducks to that instead */
typedef struct { float env, gr; } comp_st;
PROC(comp_proc)
{
    ST(comp_st);
    float thr = m->p[0], ratio = m->p[1], makeup = m->p[4], mix = m->p[5];
    float att = 1.0f - pw_coef(m->p[2]), rel = 1.0f - pw_coef(m->p[3]);
    float slope = 1.0f - 1.0f / ratio;
    for (int i = 0; i < PW_N; ++i) {
        float d = pw_absf(IN_CONNECTED(1) ? in[1][i] : in[0][i]) * 0.2f;
        s->env += (d - s->env) * (d > s->env ? att : rel);
        if ((i & 7) == 0) {
            float db = 6.0206f * pw_log2(s->env + 1e-6f), over = db - thr;
            s->gr = over > 0 ? over * slope : 0;
        }
        float g = pw_exp2((makeup - s->gr) * 0.16609640f);
        out[0][i] = in[0][i] * (1.0f + (g - 1.0f) * mix);
        out[1][i] = pw_clamp(s->gr * (5.0f / 24.0f), 0.0f, 5.0f);
    }
    if (s->env < 1e-20f) s->env = 0;
}

/* LIMITER: nothing gets past the CEILING. DRIVE pushes the sound into it (a loudness maximizer); MODE picks how it
 * holds: BRICK turns the gain down, SOFT also rounds the peaks off, FOLD folds the excess back instead. PUMP ducks the
 * sound on every beat (or on a gate into DUCK) and lets it swell back over RECOVER: the classic pumping. */
static const char *const L_limit[] = {"BRICK", "SOFT", "FOLD"};
typedef struct { float g, pp, grdb; uint8_t duck_hi; uint32_t beat; } lim_st;
PROC(limiter_proc)
{
    ST(lim_st);
    float drive = pw_exp2(m->p[0] * 0.16609640f), ceil_v = 5.0f * pw_exp2(m->p[1] * 0.16609640f);
    int mode = (int)(m->p[3] + 0.5f);
    float rel = 1.0f - pw_coef(m->p[2]), pump = m->p[4], mix = m->p[6];
    float pstep = 1.0f / (pw_maxf(m->p[5], 0.02f) * PW_SRF);
    if (s->g <= 0.0f) s->g = 1.0f;
    for (int i = 0; i < PW_N; ++i) {
        int trg = 0;
        if (IN_CONNECTED(1)) trg = pw_rise(&s->duck_hi, in[1][i]);
        else if (pws.playing) {
            if (pws.reset_now && i == 0) s->beat = 0;
            if (pws.tick_at[i]) { trg = (s->beat & 3u) == 0; ++s->beat; }
        }
        if (trg) s->pp = 0;
        else if (s->pp < 1.0f) s->pp += pstep;
        float duck = 1.0f - pump * (1.0f - pw_minf(s->pp, 1.0f)) * (1.0f - pw_minf(s->pp, 1.0f));
        float x = in[0][i] * drive, y;
        if (mode == 2) {
            float u = x / ceil_v, f = pw_frac((u + 1.0f) * 0.25f);
            y = ceil_v * (1.0f - 4.0f * pw_absf(f - 0.5f));
            s->g = 1.0f;
        } else {
            float lim = mode == 1 ? ceil_v * 1.5f : ceil_v, ax = pw_absf(x), t = ax > lim ? lim / ax : 1.0f;
            if (t < s->g) s->g = t; else { s->g += (1.0f - s->g) * rel; if (s->g > t) s->g = t; }
            y = x * s->g;
            if (mode == 1) y = ceil_v * pw_tanh(y / ceil_v);
        }
        y *= duck;
        out[0][i] = in[0][i] * (1.0f - mix) + y * mix;
        if ((i & 7) == 0) { s->grdb = -6.0206f * pw_log2(pw_maxf(s->g * duck, 1e-4f)); }
        out[1][i] = pw_clamp(s->grdb * (5.0f / 24.0f), 0.0f, 5.0f);
    }
}

/* OTT: three bands, each squeezed from both sides: loud parts turned down and quiet parts turned up. DEPTH is how much;
 * SKEW leans it toward the loud (down) or the quiet (up) side; TIME the speed; LOW/MID/HIGH tilt the bands. */
typedef struct { float l1, l2, env[3], g[3], gdb[3], lvl[3]; } ott_st;
PROC(ott_proc)
{
    ST(ott_st);
    (void)connected;
    float depth = m->p[0], tm = m->p[1], skew = m->p[2], mix = m->p[3], outg = pw_exp2(m->p[7] * 0.16609640f);
    float bg[3] = {m->p[4], m->p[5], m->p[6]};
    float a1 = pw_lp_coef_hz(140.0f), a2 = pw_lp_coef_hz(2800.0f);
    float att = 1.0f - pw_coef(0.001f * pw_exp2(tm * 5.0f)), rel = 1.0f - pw_coef(0.02f * pw_exp2(tm * 6.0f));
    float wd = skew > 0 ? 1.0f - skew : 1.0f, wu = skew < 0 ? 1.0f + skew : 1.0f, sg = 1.0f - pw_coef(0.002f);
    for (int i = 0; i < PW_N; ++i) {
        float x = in[0][i];
        s->l1 += a1 * (x - s->l1); s->l2 += a2 * (x - s->l2);
        float band[3] = {s->l1, s->l2 - s->l1, x - s->l2}, y = 0;
        for (int b = 0; b < 3; ++b) {
            float d = pw_absf(band[b]) * 0.2f;
            s->env[b] += (d - s->env[b]) * (d > s->env[b] ? att : rel);
            if ((i & 7) == 0) {
                float db = 6.0206f * pw_log2(s->env[b] + 1e-6f), g = 0;
                if (db > -18.0f) g -= (db + 18.0f) * 0.75f * wd;           /* 4:1 above -18 dB */
                else if (db < -36.0f) g += pw_minf((-36.0f - db) * 0.66f, 30.0f) * wu;   /* 3:1 up below -36 dB, no more than +30 */
                s->lvl[b] = db; s->gdb[b] = g * depth + bg[b];
            }
            float target = pw_exp2(s->gdb[b] * 0.16609640f);
            s->g[b] += (target - s->g[b]) * sg;
            y += band[b] * s->g[b];
        }
        out[0][i] = x * (1.0f - mix) + y * outg * mix;
    }
    for (int b = 0; b < 3; ++b) if (s->env[b] < 1e-20f) s->env[b] = 0;
}

/* ARP: the held keys (KEYS' notes) one at a time; clock patched: each pulse, else RATE of the tempo */
static const char *const L_arpmode[] = {"UP", "DOWN", "UP/DN", "RANDOM", "ORDER"};
static const char *const L_rate[] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const float rate_per_beat[6] PW_DSP_DATA = {1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
typedef struct {
    uint8_t notes[16], n, idx, dir, clk_hi, latched; uint8_t prev_held;
    float ph, since, period, pitch; uint16_t trig; uint32_t seed, serial; int8_t oct; uint8_t gate;
} arp_st;
static void arp_init(pw_module *m) { ST(arp_st); s->seed = 0xA2Bu + (uint32_t)(m - pws.mod) * 2654435761u; s->dir = 1; }
PROC_COLD(arp_proc)
{
    ST(arp_st);
    int mode = (int)(m->p[0] + 0.5f), octs = (int)(m->p[2] + 0.5f), latch = m->p[4] > 0.5f;
    float width = m->p[3];
    /* the chord: the held keys, in the order pressed (latch: kept after they are let go) */
    if (pws.n_held) {
        if (!s->prev_held && latch) s->n = 0;
        if (pws.n_held != s->n || !latch || !s->prev_held) {
            s->n = 0;
            for (unsigned k = 0; k < pws.n_held && k < 16; ++k) s->notes[s->n++] = pws.held[k];
        }
    } else if (!latch) s->n = 0;
    int fresh = pws.n_held && !s->prev_held;
    s->prev_held = pws.n_held;
    int n = s->n;
    uint8_t sorted[16];
    for (int k = 0; k < n; ++k) sorted[k] = s->notes[k];
    if (mode != 4)
        for (int a = 1; a < n; ++a) for (int b = a; b > 0 && sorted[b - 1] > sorted[b]; --b) { uint8_t t = sorted[b]; sorted[b] = sorted[b - 1]; sorted[b - 1] = t; }
    int total = n * (octs < 1 ? 1 : octs);
    float inc = pws.bpm / 60.0f * rate_per_beat[(int)pw_clamp(m->p[1] + 0.5f, 0.0f, 5.0f)] * PW_INV_SR;
    for (int i = 0; i < PW_N; ++i) {
        int clock;
        if (IN_CONNECTED(0)) clock = pw_rise(&s->clk_hi, in[0][i]);
        else { s->ph += inc; clock = s->ph >= 1.0f; if (clock) s->ph -= 1.0f; }
        if (fresh && i == 0) { clock = 1; s->ph = 0; s->idx = 0; s->dir = 1; s->oct = 0; }
        s->since += 1.0f;
        if (clock) {
            if (s->since > 8.0f) s->period = s->since;
            s->since = 0;
            if (total > 0) {
                int at;
                if (mode == 3) at = (int)(pw_rand_u(&s->seed) % (uint32_t)total);
                else {
                    if (!fresh || i) {
                        if (mode == 2 && total > 1) {
                            int nx = (int)s->idx + (s->dir ? 1 : -1);
                            if (nx >= total) { s->dir = 0; nx = total - 2; }
                            if (nx < 0) { s->dir = 1; nx = 1; }
                            s->idx = (uint8_t)nx;
                        } else s->idx = (uint8_t)((s->idx + 1) % total);
                    }
                    if (s->idx >= total) s->idx = 0;
                    at = s->idx;
                }
                int note = sorted[at % n] + 12 * (at / n);
                if (mode == 1) note = sorted[n - 1 - at % n] + 12 * ((total - 1 - at) / n);
                s->pitch = (float)(note - 60) / 12.0f;
                s->gate = 1; s->trig = 88;
            } else s->gate = 0;
        }
        float per = s->period > 0 ? s->period : 1.0f / (inc > 0 ? inc : 1e-4f);
        int g = s->gate && n > 0 && s->since < per * width;
        out[0][i] = s->pitch;
        out[1][i] = g ? 5.0f : 0.0f;
        out[2][i] = s->trig ? 5.0f : 0.0f;
        if (s->trig) --s->trig;
    }
}

/* TURING: a 16-bit loop of random bits, LENGTH long; each clock the bit coming round is flipped by CHANCE
 * (0: the loop repeats for ever, 1: always new). CV: the top 8 bits as a voltage; GATE: the newest bit. */
typedef struct { uint32_t reg; uint8_t clk_hi; uint16_t trig; uint32_t seed; float cv; } turing_st;
static void turing_init(pw_module *m) { ST(turing_st); s->seed = 0x7E51u + (uint32_t)(m - pws.mod) * 2654435761u; s->reg = pw_rand_u(&s->seed); }
PROC_COLD(turing_proc)
{
    ST(turing_st);
    int len = (int)(m->p[0] + 0.5f);
    float chance = m->p[1], range = m->p[2];
    int lock = m->p[3] > 0.5f;   /* LOCK (the knob, or a gate into LOCK): the loop stops changing */
    if (len < 2) len = 2;
    if (len > 32) len = 32;
    for (int i = 0; i < PW_N; ++i) {
        int clock = IN_CONNECTED(0) ? pw_rise(&s->clk_hi, in[0][i]) : pws.tick_at[i];
        if (clock) {
            unsigned bit = (s->reg >> (len - 1)) & 1u;
            float c = pw_clamp(chance + (IN_CONNECTED(1) ? in[1][i] * 0.2f : 0), 0.0f, 1.0f);
            if (lock || (IN_CONNECTED(2) && in[2][i] > 1.0f)) c = 0.0f;
            if ((pw_rand_u(&s->seed) >> 8 & 0xFFFFu) < (unsigned)(c * 65536.0f)) bit ^= 1u;
            s->reg = s->reg << 1 | bit;
            s->cv = (float)(s->reg & 0xFFu) * (1.0f / 255.0f) * range;
            if (bit) s->trig = 88;
        }
        out[0][i] = s->cv;
        out[1][i] = (s->reg & 1u) ? 5.0f : 0.0f;
        out[2][i] = s->trig ? 5.0f : 0.0f;
        if (s->trig) --s->trig;
    }
}

/* MARKOV: a melody from a chain of chances. Each step picks the next note of the SCALE (RANGE octaves of it) from where it is: STEP
 * makes near notes likelier (0: any leap), HOME pulls toward the root and the fifth, REPEAT keeps the same note, REST leaves the
 * step silent. A tune played into LEARN (a pitch) teaches it which note follows which: LEARN is how much that overrides the chances. */
#define MK_MAX 36
typedef struct { uint8_t learn[12][12]; uint8_t clk_hi, rst_hi, have, ldeg, reset, divc, gate; int8_t cur; uint16_t trig; uint32_t seed; float pitch, period, since; subclk sc; } mk_st;
static void mk_init(pw_module *m) { ST(mk_st); s->seed = 0x3A4Bu + (uint32_t)(m - pws.mod) * 2654435761u; s->cur = -1; }
static int mk_scale(const pw_module *m, int *semis)   /* the scale's notes (semitones above the root); how many */
{
    int sc = (int)pw_clamp(m->p[0] + 0.5f, 0.0f, 11.0f), n = 0;
    unsigned mask = scale_mask[sc];
    for (int d = 0; d < 12; ++d) if ((mask >> d) & 1u) semis[n++] = d;
    return n;
}
/* the chances of each note after note `cur` (0 .. N-1, N = notes in the scale x RANGE octaves); returns N */
int pw_markov_row(const pw_module *m, int cur, float *w)
{
    int semis[12], nd = mk_scale(m, semis), range = (int)pw_clamp(m->p[2] + 0.5f, 1.0f, 3.0f), n = nd * range;
    const mk_st *s = (const mk_st *)(const void *)m->st.raw;
    float a = 0.15f + 1.6f * m->p[3], home = m->p[4], rep = m->p[5], learn = m->p[8], sum = 0.0f, rsum = 0.0f;
    if (n > MK_MAX) n = MK_MAX;
    if (cur < 0 || cur >= n) cur = 0;
    const uint8_t *row = s->learn[cur % nd];
    for (int j = 0; j < nd; ++j) rsum += (float)row[j];
    for (int j = 0; j < n; ++j) {
        int dist = j > cur ? j - cur : cur - j, dj = j % nd;
        float v = dist == 0 ? 0.03f + 5.0f * rep * rep : pw_exp2(-a * (float)dist);
        float hw = dj == 0 ? 1.0f : semis[dj] == 7 ? 0.6f : (semis[dj] == 4 || semis[dj] == 3) ? 0.25f : 0.0f;
        v *= 1.0f + 3.0f * home * hw;
        if (learn > 0.0f && rsum > 0.0f) {
            float mean = rsum / (float)nd, f = ((float)row[dj] + 0.04f * mean) / (1.2f * mean);
            v *= 1.0f + learn * (f * 4.0f - 1.0f);
            if (v < 0.0f) v = 0.0f;
        }
        w[j] = v; sum += v;
    }
    if (sum > 0.0f) for (int j = 0; j < n; ++j) w[j] /= sum;
    return n;
}
PROC_COLD(markov_proc)
{
    ST(mk_st);
    int semis[12], nd = mk_scale(m, semis), range = (int)pw_clamp(m->p[2] + 0.5f, 1.0f, 3.0f), n = nd * range, root = (int)(m->p[1] + 0.5f);
    int div, mult;
    float rest = m->p[6], width = m->p[7], oct = (float)(int)(m->p[9] + (m->p[9] < 0 ? -0.5f : 0.5f));
    div_split(m->p[10], &div, &mult);
    if (n > MK_MAX) n = MK_MAX;
    if (s->period <= 0) s->period = PW_SRF * 15.0f / pws.bpm;
    if (pws.reset_now && !IN_CONNECTED(0)) s->reset = 1;
    if (IN_CONNECTED(2) && m->p[8] >= 0.0f) {   /* LEARN: a note played in (its pitch) teaches which scale note follows which */
        float x = in[2][0] * 12.0f - (float)root;
        int pc = ((pw_floori(x + 0.5f) % 12) + 12) % 12, best = 0, bd = 99;
        for (int d = 0; d < nd; ++d) { int dd = pc > semis[d] ? pc - semis[d] : semis[d] - pc; if (dd > 6) dd = 12 - dd; if (dd < bd) { bd = dd; best = d; } }
        if (!s->have) { s->have = 1; s->ldeg = (uint8_t)best; }
        else if (best != s->ldeg && in[2][0] > -4.0f) {
            uint8_t *r = s->learn[s->ldeg];
            if (r[best] > 250) for (int j = 0; j < 12; ++j) r[j] = (uint8_t)(r[j] >> 1);
            r[best]++;
            s->ldeg = (uint8_t)best;
        }
    }
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(1) && pw_rise(&s->rst_hi, in[1][i])) s->reset = 1;
        int tick = IN_CONNECTED(0) ? pw_rise(&s->clk_hi, in[0][i]) : pws.tick_at[i], due = 0;
        s->since += 1.0f;
        if (tick) {
            if (s->since > 64.0f && s->since < PW_SRF * 4.0f) s->period = s->since;
            if (!IN_CONNECTED(0)) s->period = PW_SRF * 15.0f / pws.bpm;
            s->since = 0;
            due = s->divc == 0;
            s->divc = (uint8_t)((s->divc + 1) % div);
            if (s->reset) { due = 1; s->divc = (uint8_t)(1 % div); }
            if (due && mult > 1) subclk_arm(&s->sc, s->period, mult);
        }
        if (subclk_step(&s->sc)) due = 1;
        if (due) {
            float w[MK_MAX];
            int first = s->reset || s->cur < 0 || s->cur >= n;
            s->reset = 0;
            if (first) s->cur = 0;
            else if (!((pw_rand_u(&s->seed) >> 8 & 0xFFFFu) < (unsigned)(rest * 65536.0f))) {
                pw_markov_row(m, s->cur, w);
                float r = (float)(pw_rand_u(&s->seed) >> 8 & 0xFFFFu) * (1.0f / 65536.0f), acc = 0.0f;
                int pick = n - 1;
                for (int j = 0; j < n; ++j) { acc += w[j]; if (r < acc) { pick = j; break; } }
                s->cur = (int8_t)pick;
            } else { s->gate = 0; goto held; }   /* a rest: the note rings on silent */
            s->pitch = (float)(root + semis[s->cur % nd] + 12 * (s->cur / nd)) * (1.0f / 12.0f);
            s->gate = 1; s->trig = 88;
        }
held:;
        float per = s->period > 0 ? s->period : 5512.0f;
        int g = s->gate && s->since < per * width;
        out[0][i] = s->pitch + oct + (IN_CONNECTED(3) ? in[3][i] : 0.0f);
        out[1][i] = g ? 5.0f : 0.0f;
        out[2][i] = s->trig ? 5.0f : 0.0f;
        if (s->trig) --s->trig;
    }
}


/* STEREO MIXER: four channels, each with a level and an equal-power pan */
typedef struct { float gl[4], gr[4]; } stereo_st;
PROC(stereo_proc)
{
    ST(stereo_st);
    float tl[4], tr[4];
    for (int k = 0; k < 4; ++k) {
        float pan = (m->p[4 + k] + 1.0f) * 0.25f;   /* 0..0.5: a quarter turn of the sine */
        tl[k] = m->p[k] * pw_cos1(pan * 0.5f) * 1.4142f;
        tr[k] = m->p[k] * pw_sin1(pan * 0.5f) * 1.4142f;
    }
    int ramp[4];   /* (a gain that has not moved needs no ramp) */
    for (int k = 0; k < 4; ++k) ramp[k] = s->gl[k] != tl[k] || s->gr[k] != tr[k];
    for (int i = 0; i < PW_N; ++i) {
        float l = 0, r = 0, t = (float)(i + 1) * (1.0f / PW_N);
        for (int k = 0; k < 4; ++k) {
            if (!IN_CONNECTED(k)) continue;
            float a = tl[k], b = tr[k];
            if (ramp[k]) { a = pw_lerp(s->gl[k], tl[k], t); b = pw_lerp(s->gr[k], tr[k], t); }
            l += in[k][i] * a; r += in[k][i] * b;
        }
        out[0][i] = 5.0f * pw_tanh(l * 0.16f) * 1.25f;
        out[1][i] = 5.0f * pw_tanh(r * 0.16f) * 1.25f;
    }
    for (int k = 0; k < 4; ++k) { s->gl[k] = tl[k]; s->gr[k] = tr[k]; }
}

/* MIXER 8: eight channels, the same level and equal-power pan (levels are params 0-7, pans 8-15) */
typedef struct { float gl[8], gr[8]; } mix8_st;
PROC(mix8_proc)
{
    ST(mix8_st);
    float tl[8], tr[8];
    for (int k = 0; k < 8; ++k) {
        float pan = (m->p[8 + k] + 1.0f) * 0.25f;
        tl[k] = m->p[k] * pw_cos1(pan * 0.5f) * 1.4142f;
        tr[k] = m->p[k] * pw_sin1(pan * 0.5f) * 1.4142f;
    }
    int ramp[8];
    for (int k = 0; k < 8; ++k) ramp[k] = s->gl[k] != tl[k] || s->gr[k] != tr[k];
    for (int i = 0; i < PW_N; ++i) {
        float l = 0, r = 0, t = (float)(i + 1) * (1.0f / PW_N);
        for (int k = 0; k < 8; ++k) {
            if (!IN_CONNECTED(k)) continue;
            float a = tl[k], b = tr[k];
            if (ramp[k]) { a = pw_lerp(s->gl[k], tl[k], t); b = pw_lerp(s->gr[k], tr[k], t); }
            l += in[k][i] * a; r += in[k][i] * b;
        }
        out[0][i] = 5.0f * pw_tanh(l * 0.16f) * 1.25f;
        out[1][i] = 5.0f * pw_tanh(r * 0.16f) * 1.25f;
    }
    for (int k = 0; k < 8; ++k) { s->gl[k] = tl[k]; s->gr[k] = tr[k]; }
}

/* CHAOS: the Lorenz attractor, three wandering voltages */
typedef struct { float x, y, z; } chaos_st;
static void chaos_init(pw_module *m) { ST(chaos_st); s->x = 0.1f; s->y = 0; s->z = 0; }
PROC(chaos_proc)
{
    ST(chaos_st);
    float rate = m->p[0], rho = m->p[1], level = m->p[2];
    for (int i = 0; i < PW_N; ++i) {
        float dt = pw_clamp(rate * (IN_CONNECTED(0) ? pw_exp2(in[0][i]) : 1.0f) * PW_INV_SR, 0.0f, 0.004f);
        float dx = 10.0f * (s->y - s->x), dy = s->x * (rho - s->z) - s->y, dz = s->x * s->y - 2.6667f * s->z;
        s->x += dx * dt; s->y += dy * dt; s->z += dz * dt;
        if (!(s->x == s->x) || pw_absf(s->x) > 1e3f) { s->x = 0.1f; s->y = 0; s->z = 0; }
        out[0][i] = pw_clamp(s->x * (5.0f / 20.0f) * level, -5.0f, 5.0f);
        out[1][i] = pw_clamp(s->y * (5.0f / 27.0f) * level, -5.0f, 5.0f);
        out[2][i] = pw_clamp((s->z - 25.0f) * (5.0f / 25.0f) * level, -5.0f, 5.0f);
    }
}

/* COIN: each trigger goes to A or B, CHANCE of B; LATCH: the outputs stay until the next toss */
typedef struct { uint8_t trig_hi, side, held; uint32_t seed; } coin_st;
static void coin_init(pw_module *m) { ST(coin_st); s->seed = 0xC014u + (uint32_t)(m - pws.mod) * 2654435761u; }
PROC_COLD(coin_proc)
{
    ST(coin_st);
    int latch = m->p[1] > 0.5f;
    for (int i = 0; i < PW_N; ++i) {
        if (pw_rise(&s->trig_hi, in[0][i])) {
            float c = pw_clamp(m->p[0] + (IN_CONNECTED(1) ? in[1][i] * 0.2f : 0), 0.0f, 1.0f);
            s->side = (pw_rand_u(&s->seed) >> 8 & 0xFFFFu) < (unsigned)(c * 65536.0f);
        }
        int on = latch ? 1 : s->trig_hi;
        out[0][i] = on && !s->side ? 5.0f : 0.0f;
        out[1][i] = on && s->side ? 5.0f : 0.0f;
    }
}

/* BUTTON: the panel's keys as patchable controls. Which keys (FROM..TO of the 27: all by default) are held comes from the keyboard
 * dispatcher (pw_button_keys), or from the page's on-screen button (pw_button_virtual). OUT follows MODE: GATE (high while any is held),
 * TRIGGER (a pulse of TIME per press), TOGGLE (flips on each press), REPEAT (pulses every TIME while held), BURST (COUNT pulses TIME
 * apart per press; a press in a burst restarts it), TAP (the presses set a tempo; a held key clears it), SAMPLE (CV IN is taken at each
 * press and held). TRIG: a pulse for every newly pressed key, whatever the mode. HOLD: high once a key has been down for HOLD seconds.
 * DBL: a pulse when two presses come within DBL seconds. VALUE sets the level of OUT (1 is 5 V). */
static const char *const L_btnmode[] = {"GATE", "TRIGGER", "TOGGLE", "REPEAT", "BURST", "TAP", "SAMPLE"};
typedef struct { uint32_t prev, clk, last_press, tap_last, tap_per, rep; uint32_t hold_t; float held; int32_t pulse, trig, dbl; uint8_t tog, fired, dbl_valid, burst, taps; } btn_st;
PROC_COLD(button_proc)
{
    ST(btn_st);
    unsigned a = (unsigned)pw_clamp(m->p[0] + 0.5f, 1.0f, 27.0f), b = (unsigned)pw_clamp(m->p[1] + 0.5f, 1.0f, 27.0f);
    if (a > b) { unsigned t = a; a = b; b = t; }
    const uint32_t mask = (((b >= 27u) ? 0x7FFFFFFu : ((1u << b) - 1u)) & ~((1u << (a - 1u)) - 1u)) | (1u << 27);
    unsigned slot = (unsigned)(m - pws.mod);
    uint32_t keys = pws.btn_keys & mask & 0x7FFFFFFu;
    if (slot < PW_MAX_MODULES && pws.btn_virt[slot]) keys |= 1u << 27;
    const unsigned mode = (unsigned)pw_clamp(m->p[2] + 0.5f, 0.0f, 6.0f);
    const float v = m->p[3] * 5.0f, sr = PW_SRF;
    const int32_t tlen = (int32_t)(m->p[4] * sr) + 1, pw5 = (int32_t)(0.005f * sr) + 1;
    const unsigned count = (unsigned)pw_clamp(m->p[5] + 0.5f, 1.0f, 16.0f);
    const uint32_t hold_n = (uint32_t)(m->p[6] * sr), dbl_n = (uint32_t)(m->p[7] * sr);
    const uint32_t fresh = keys & ~s->prev;
    const int any = keys != 0, was = s->prev != 0;
    s->prev = keys;
    if (fresh) {   /* a press (or several at once) */
        s->trig = pw5;
        if (s->dbl_valid && s->clk - s->last_press <= dbl_n) { s->dbl = pw5; s->dbl_valid = 0; }
        else { s->dbl_valid = 1; s->last_press = s->clk; }
        switch (mode) {
        case 1: s->pulse = tlen; break;
        case 2: s->tog ^= 1u; break;
        case 3: s->rep = 0; s->pulse = 0; break;
        case 4: s->burst = (uint8_t)count; s->rep = 0; s->pulse = 0; break;
        case 5: {
            uint32_t d = s->clk - s->tap_last;
            if (s->taps && d < (uint32_t)(2.0f * sr) && d > (uint32_t)(0.1f * sr)) { s->tap_per = s->tap_per ? (s->tap_per + d) / 2u : d; s->rep = 0; s->pulse = 0; }
            else if (!s->taps || d >= (uint32_t)(2.0f * sr)) s->tap_per = 0;
            s->tap_last = s->clk; if (s->taps < 255) ++s->taps;
            s->pulse = pw5;
        } break;
        case 6: s->held = IN_CONNECTED(0) ? in[0][0] : 0.0f; break;
        default: break;
        }
    }
    if (any && !was) { s->hold_t = 0; s->fired = 0; }
    if (!any) s->fired = 0;
    for (int i = 0; i < PW_N; ++i) {
        ++s->clk;
        if (any) {
            ++s->hold_t;
            if (!s->fired && s->hold_t >= hold_n) { s->fired = 1; if (mode == 5) { s->tap_per = 0; s->taps = 0; } }
        }
        float o = 0.0f;
        switch (mode) {
        case 0: o = any ? v : 0.0f; break;
        case 1: o = s->pulse > 0 ? v : 0.0f; break;
        case 2: o = s->tog ? v : 0.0f; break;
        case 3:
            if (any) { if (s->rep == 0) s->pulse = tlen / 2 + 1; if (++s->rep >= (uint32_t)tlen) s->rep = 0; }
            o = s->pulse > 0 ? v : 0.0f; break;
        case 4:
            if (s->burst) { if (s->rep == 0) s->pulse = tlen / 2 + 1; if (++s->rep >= (uint32_t)tlen) { s->rep = 0; --s->burst; } }
            o = s->pulse > 0 ? v : 0.0f; break;
        case 5:
            if (s->tap_per) { if (++s->rep >= s->tap_per) { s->rep = 0; s->pulse = pw5; } }
            o = s->pulse > 0 ? v : 0.0f; break;
        default: o = s->held * m->p[3]; break;
        }
        if (s->pulse > 0) --s->pulse;
        out[0][i] = o;
        out[1][i] = s->trig > 0 ? 5.0f : 0.0f;
        out[2][i] = s->fired ? 5.0f : 0.0f;
        out[3][i] = s->dbl > 0 ? 5.0f : 0.0f;
        if (s->trig > 0) --s->trig;
        if (s->dbl > 0) --s->dbl;
    }
}
void pw_button_keys(uint32_t mask) { pws.btn_keys = mask & 0x7FFFFFFu; }
void pw_button_virtual(unsigned slot, int down) { if (slot < PW_MAX_MODULES) pws.btn_virt[slot] = down != 0; }

/* CHORD: one pitch in, four out: a chord on it, inverted, spread over octaves */
static const char *const L_chord[] = {"MAJ", "MIN", "SUS2", "SUS4", "MAJ7", "MIN7", "DOM7", "DIM", "AUG", "5TH"};
static const int8_t chord_iv[10][4] PW_DSP_DATA = {{0, 4, 7, 12}, {0, 3, 7, 12}, {0, 2, 7, 12}, {0, 5, 7, 12},
    {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 4, 7, 10}, {0, 3, 6, 9}, {0, 4, 8, 12}, {0, 7, 12, 19}};
PROC_COLD(chord_proc)
{
    (void)connected;
    int type = (int)pw_clamp(m->p[0] + 0.5f, 0.0f, 9.0f), inv = (int)(m->p[1] + 0.5f), spread = (int)(m->p[2] + 0.5f);
    float iv[4];
    for (int k = 0; k < 4; ++k) iv[k] = (float)chord_iv[type][k];
    for (int r = 0; r < inv; ++r) {             /* each inversion: the lowest note an octave up */
        float t = iv[0] + 12.0f;
        iv[0] = iv[1]; iv[1] = iv[2]; iv[2] = iv[3]; iv[3] = t;
    }
    if (spread) { iv[1] += 12.0f; iv[3] += 12.0f; }
    for (int i = 0; i < PW_N; ++i)
        for (int k = 0; k < 4; ++k) out[k][i] = in[0][i] + iv[k] * (1.0f / 12.0f);
}

/* ------------------------------------------------------------ descriptors */
/* ========================================================== from the Nord Modular G2 */
/* FORMANT OSC: a train of grains, one per period of the pitch: in each, two sines at the vowel's first two
 * formants start from zero and die away (WIDTH: how fast, so how wide the formant), so the spectrum is the
 * pitch's harmonics shaped by two resonant humps: a voice. VOWEL sweeps A E I O U, FORMANT moves the humps. */
static const float formant_f[5][2] PW_DSP_DATA = {{800, 1150}, {400, 1700}, {270, 2140}, {450, 800}, {325, 700}};
typedef struct { float ph, r1, r2, dc_x, dc_y; } form_st;
PROC(formant_proc)
{
    ST(form_st);
    float base = m->p[0] / 12.0f, width = m->p[2], f0 = pw_pitch_hz(base);
    float bw = 60.0f * pw_exp2(width * 4.5f);                 /* the formants' width, 60 Hz..1.4 kHz */
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(0)) f0 = pw_pitch_hz(base + in[0][i]);
        f0 = pw_clamp(f0, 16.0f, 2000.0f);
        if ((i & 15) == 0) {
            float v = pw_clamp(m->p[1] + (IN_CONNECTED(1) ? in[1][i] * 0.8f : 0), 0.0f, 4.0f);
            int a = (int)v; if (a > 3) a = 3;
            float t = v - (float)a, sh = pw_exp2(m->p[3] / 12.0f + (IN_CONNECTED(2) ? in[2][i] : 0));
            s->r1 = pw_lerp(formant_f[a][0], formant_f[a + 1][0], t) * sh / f0;
            s->r2 = pw_lerp(formant_f[a][1], formant_f[a + 1][1], t) * sh / f0;
        }
        s->ph += f0 * PW_INV_SR;
        if (s->ph >= 1.0f) s->ph -= (float)pw_floori(s->ph);
        float ph = s->ph;
        float env = pw_exp2(-ph * (bw / f0) * 4.53f) * (1.0f - ph) * (1.0f - ph);
        float y = (pw_sin1(ph * s->r1) + 0.5f * pw_sin1(ph * s->r2)) * env;
        float o = y - s->dc_x + 0.995f * s->dc_y; s->dc_x = y; s->dc_y = o;
        out[0][i] = 4.0f * o;
    }
    if (pw_absf(s->dc_y) < 1e-20f) s->dc_y = 0;
}

/* COMB FILTER: the input plus itself FREQ's period ago, fed back: a row of resonant peaks at FREQ's harmonics
 * (FBK below zero: the odd ones only, an octave down, hollow). DAMP darkens the repeats. Patched V/OCT tunes it. */
typedef struct { uint32_t w; float lp, d; } comb_st;
PROC(comb_proc)
{
    ST(comb_st);
    int16_t *buf = PW_POOL16(m);
    uint32_t len = m->mem_len / 2u;
    if (m->status != PW_STATUS_OK || len < 512) { pass_through(in, out, 1); return; }
    float fb0 = m->p[1], a = 1.0f - m->p[2] * 0.9f, mix = m->p[3];
    float target = 0;
    for (int i = 0; i < PW_N; ++i) {
        if ((i & 7) == 0) {
            float hz = m->p[0] * (IN_CONNECTED(1) ? pw_exp2(in[1][i]) : 1.0f);
            target = pw_clamp(PW_SRF / pw_maxf(hz, 1.0f), 2.0f, (float)(len - 3u));
            if (s->d < 1.0f) s->d = target;
        }
        s->d += (target - s->d) * 0.01f;
        float fb = pw_clamp(fb0 + (IN_CONNECTED(2) ? in[2][i] * 0.2f : 0), -0.995f, 0.995f);
        float v = dl_read(buf, len, s->w, s->d);
        s->lp += a * (v - s->lp);
        float x = in[0][i], y = x + fb * s->lp;
        buf[s->w] = dl_word(8.0f * pw_tanh(y * 0.125f));
        if (++s->w >= len) s->w = 0;
        out[0][i] = x * (1.0f - mix) + y * mix * (1.0f - 0.6f * pw_absf(fb));
    }
    if (pw_absf(s->lp) < 1e-20f) s->lp = 0;
}

/* EQ: three bands, the classic console layout: a low shelf, a sweepable bell, a high shelf (RBJ biquads).
 * MID F patched: the bell follows it, 1 V an octave. */
typedef struct { float c[3][5], z[3][2], key[8]; } eq_st;
static void eq_coef(float *c, int kind, float hz, float db, float q)
{
    float w = pw_clamp(hz, 20.0f, 20000.0f) * PW_INV_SR, sn = pw_sin1(w), cs = pw_cos1(w);
    float A = pw_exp2(db * (3.3219281f / 40.0f)), sA = pw_exp2(db * (3.3219281f / 80.0f));
    float b0, b1, b2, a0, a1, a2;
    if (kind == 1) {                                   /* bell */
        float al = sn / (2.0f * q);
        b0 = 1.0f + al * A; b1 = -2.0f * cs; b2 = 1.0f - al * A; a0 = 1.0f + al / A; a1 = -2.0f * cs; a2 = 1.0f - al / A;
    } else {                                           /* shelves, slope 1 */
        float al = sn * 0.70710678f, t = 2.0f * sA * al;
        if (kind == 0) {
            b0 = A * ((A + 1) - (A - 1) * cs + t); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - t);
            a0 = (A + 1) + (A - 1) * cs + t; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - t;
        } else {
            b0 = A * ((A + 1) + (A - 1) * cs + t); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - t);
            a0 = (A + 1) - (A - 1) * cs + t; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - t;
        }
    }
    float r = 1.0f / a0;
    c[0] = b0 * r; c[1] = b1 * r; c[2] = b2 * r; c[3] = a1 * r; c[4] = a2 * r;
}
/* the response of an EQ (dB at hz): for the screen */
float pw_eq_response(const float *p, float hz)
{
    float c[3][5];
    eq_coef(c[0], 0, p[4], p[0], 0.7f); eq_coef(c[1], 1, p[3], p[1], p[6]); eq_coef(c[2], 2, p[5], p[2], 0.7f);
    float w = hz * PW_INV_SR, c1 = pw_cos1(w), s1 = pw_sin1(w), c2 = pw_cos1(2.0f * w), s2 = pw_sin1(2.0f * w), db = 0;
    for (int k = 0; k < 3; ++k) {   /* |H(e^jw)|^2 of each biquad */
        float nr = c[k][0] + c[k][1] * c1 + c[k][2] * c2, ni = -(c[k][1] * s1 + c[k][2] * s2);
        float dr = 1.0f + c[k][3] * c1 + c[k][4] * c2, di = -(c[k][3] * s1 + c[k][4] * s2);
        db += 3.0103f * pw_log2((nr * nr + ni * ni) / pw_maxf(dr * dr + di * di, 1e-12f));
    }
    return db;
}
PROC(eq_proc)
{
    ST(eq_st);
    float midf = m->p[3];
    for (int i = 0; i < PW_N; ++i) {
        if ((i & 31) == 0) {
            float mf = IN_CONNECTED(1) ? midf * pw_exp2(in[1][i]) : midf;
            float key[8] = {m->p[0], m->p[1], m->p[2], mf, m->p[4], m->p[5], m->p[6], 1};
            if (memcmp(key, s->key, sizeof key)) {
                memcpy(s->key, key, sizeof key);
                eq_coef(s->c[0], 0, m->p[4], m->p[0], 0.7f);
                eq_coef(s->c[1], 1, mf, m->p[1], m->p[6]);
                eq_coef(s->c[2], 2, m->p[5], m->p[2], 0.7f);
            }
        }
        float x = in[0][i];
        for (int k = 0; k < 3; ++k) {   /* transposed direct form II */
            const float *c = s->c[k];
            float y = c[0] * x + s->z[k][0];
            s->z[k][0] = c[1] * x - c[3] * y + s->z[k][1];
            s->z[k][1] = c[2] * x - c[4] * y;
            x = y;
        }
        out[0][i] = x;
    }
    for (int k = 0; k < 3; ++k) for (int j = 0; j < 2; ++j) if (!(s->z[k][j] == s->z[k][j]) || pw_absf(s->z[k][j]) < 1e-20f) s->z[k][j] = 0;
}

/* WAVESET: the input is cut into wavesets, the stretches between one rising zero crossing and the next (a hysteresis of 0.03 keeps noise from
 * making false ones), GROUP of them to a group. The newest finished group is copied out of a short ring of the input and played back, one group
 * behind: REPEAT plays it 1 to 8 times (AMOUNT) and so skips the groups that arrive meanwhile; REVERSE plays a group backwards, OMIT leaves it
 * out, each with the chance AMOUNT; DIVIDE needs no groups: every Nth cycle comes out upside down, so a sub-octave appears (N: 2 to 5 by AMOUNT);
 * FREEZE keeps playing the same group (AMOUNT: the chance of taking a new one at each repeat), and a gate on FREEZE does that in any mode.
 * A group longer than the play buffer (a low note, a big GROUP) is closed early; one cycle that is too long, or no crossing for a while, plays the
 * input as it is. The ring holds two thirds of the memory, the play buffer the rest. */
static const char *const L_wsmode[] = {"REPEAT", "REVERSE", "OMIT", "DIVIDE", "FREEZE", "SCRAMBLE", "AVERAGE", "TELESCOPE", "INTERP", "FRACTAL"};
typedef struct {
    uint32_t w, last_x, g0, pg_start, pg_len, pg_seq, taken_seq, pl_pos, pl_len, seed, idle;
    uint32_t tot, hs[4], hl[4], ht[4];   /* the last four groups: where in the ring, how long, and the write count at their start */
    uint16_t cycles, reps;
    uint8_t pos, have_g0, has_pg, has_play, rev, mute, div_n, div_k, hn;
    int8_t sign;
} ws_st;
/* a group's sample at a fractional place (the ring wraps; one past its end is the next cycle's first sample, near zero) */
static PW_DSP float ws_get(const int16_t *buf, uint32_t rlen, uint32_t start, float ph)
{
    uint32_t i0 = (uint32_t)ph; float f = ph - (float)i0;
    uint32_t a = (start + i0) % rlen, b = a + 1u >= rlen ? 0u : a + 1u;
    return ((float)buf[a] + ((float)buf[b] - (float)buf[a]) * f) * (1.0f / DL_SCALE);
}
/* the modes that make a new group from the last few: SCRAMBLE plays an older one, AVERAGE the mean of the last 1 to 4 (as long as the newest),
 * TELESCOPE the newest squeezed into a half to a quarter (an octave or two up), INTERP slides from the one before to the newest, FRACTAL adds
 * copies of it at double and quadruple speed. Returns the length written to the play buffer. */
static PW_DSP uint32_t ws_build(ws_st *s, const int16_t *buf, uint32_t rlen, int16_t *play, uint32_t lmax, int mode, float amt)
{
    unsigned ok = 0;   /* how many of the last four groups are still whole in the ring */
    for (unsigned h = 0; h < s->hn; ++h) { if (s->tot - s->ht[h] + 8u < rlen) ++ok; else break; }
    if (!ok) return 0;
    uint32_t n = s->hl[0];
    if (n > lmax) n = lmax;
    if (mode == 5) {   /* SCRAMBLE: with the chance AMOUNT an older group in place of the newest */
        unsigned h = 0;
        if ((float)(pw_rand_u(&s->seed) & 1023u) < amt * 1024.0f) h = pw_rand_u(&s->seed) % ok;
        n = s->hl[h]; if (n > lmax) n = lmax;
        for (uint32_t k = 0; k < n; ++k) play[k] = dl_word(ws_get(buf, rlen, s->hs[h], (float)k));
    } else if (mode == 6) {   /* AVERAGE */
        unsigned cnt = 1u + (unsigned)(amt * 3.0f + 0.5f); if (cnt > ok) cnt = ok;
        float inv = 1.0f / (float)cnt;
        for (uint32_t k = 0; k < n; ++k) {
            float a = 0;
            for (unsigned h = 0; h < cnt; ++h) a += ws_get(buf, rlen, s->hs[h], (float)k * (float)s->hl[h] / (float)n);
            play[k] = dl_word(a * inv);
        }
    } else if (mode == 7) {   /* TELESCOPE */
        float f = 2.0f + (float)(int)(amt * 2.0f + 0.5f);
        uint32_t m2 = (uint32_t)((float)n / f); if (m2 < 16u) m2 = 16u; if (m2 > n) m2 = n;
        for (uint32_t k = 0; k < m2; ++k) play[k] = dl_word(ws_get(buf, rlen, s->hs[0], (float)k * (float)s->hl[0] / (float)m2));
        n = m2;
    } else if (mode == 8) {   /* INTERP: the one before fades into the newest over the group */
        unsigned h1 = ok > 1 ? 1u : 0u;
        for (uint32_t k = 0; k < n; ++k) {
            float r = (float)k / (float)n, a = 1.0f - amt * (1.0f - r);
            float cur = ws_get(buf, rlen, s->hs[0], (float)k * (float)s->hl[0] / (float)n);
            float prv = ws_get(buf, rlen, s->hs[h1], (float)k * (float)s->hl[h1] / (float)n);
            play[k] = dl_word(prv + (cur - prv) * a);
        }
    } else {   /* FRACTAL */
        float a1 = amt * 0.7f, a2 = amt * amt * 0.5f, g = 1.0f / (1.0f + a1 + a2);
        for (uint32_t k = 0; k < n; ++k) {
            float ph = (float)k * (float)s->hl[0] / (float)n, L = (float)s->hl[0];
            float p2 = ph * 2.0f, p4 = ph * 4.0f;
            while (p2 >= L) p2 -= L;
            while (p4 >= L) p4 -= L;
            play[k] = dl_word((ws_get(buf, rlen, s->hs[0], ph) + a1 * ws_get(buf, rlen, s->hs[0], p2) + a2 * ws_get(buf, rlen, s->hs[0], p4)) * g);
        }
    }
    return n;
}
PROC(waveset_proc)
{
    ST(ws_st);
    int16_t *buf = PW_POOL16(m);
    uint32_t len = m->mem_len / 2u;
    if (m->status != PW_STATUS_OK || len < 1024u) { pass_through(in, out, 1); return; }
    const uint32_t rlen = len / 3u * 2u, plen = len - rlen, lmax = plen;
    int16_t *play = buf + rlen;
    int mode = (int)(m->p[0] + 0.5f), group = (int)(m->p[1] + 0.5f);
    float amt0 = m->p[2], mix = m->p[3];
    if (group < 1) group = 1;
    if (group > 8) group = 8;
    int gate = 0;
    if (!s->seed) s->seed = 0x9E3779B1u ^ (uint32_t)(uintptr_t)m;
    if (!s->sign) s->sign = 1;
    for (int i = 0; i < PW_N; ++i) {
        float x = in[0][i], amt = amt0;
        if (IN_CONNECTED(1)) amt += in[1][i] * 0.1f;
        amt = pw_clamp(amt, 0.0f, 1.0f);
        if (IN_CONNECTED(2)) gate = in[2][i] > 1.0f;
        int frozen = gate || mode == 4;
        float wet = x;
        /* the ring, and the rising zero crossings (the waveset boundaries) */
        buf[s->w] = dl_word(x);
        ++s->tot;
        int cross = 0;
        if (!s->pos) { if (x > 0.03f) { s->pos = 1; cross = 1; } }
        else if (x < -0.03f) s->pos = 0;
        ++s->idle;
        if (cross) {
            uint32_t c = s->w;
            if (s->have_g0) {
                uint32_t cyc = (c + rlen - s->last_x) % rlen, glen = (c + rlen - s->g0) % rlen;
                if (s->idle > lmax) { s->g0 = c; s->cycles = 0; }   /* (a gap: start over) */
                else {
                    ++s->cycles;
                    if (s->cycles >= (uint16_t)group || glen + cyc > lmax) {
                        if (glen > 16u && glen <= lmax) {
                            s->pg_start = s->g0; s->pg_len = glen; ++s->pg_seq; s->has_pg = 1;
                            for (int h = 3; h > 0; --h) { s->hs[h] = s->hs[h - 1]; s->hl[h] = s->hl[h - 1]; s->ht[h] = s->ht[h - 1]; }
                            s->hs[0] = s->g0; s->hl[0] = glen; s->ht[0] = s->tot - glen; if (s->hn < 4) ++s->hn;
                        }
                        else s->has_pg = 0;
                        s->g0 = c; s->cycles = 0;
                    }
                }
            } else { s->g0 = c; s->have_g0 = 1; s->cycles = 0; }
            s->last_x = c; s->idle = 0;
            if (mode == 3) {   /* DIVIDE: every Nth cycle turns over */
                int n = 2 + (int)(amt * 3.0f + 0.5f);
                if (++s->div_k >= (uint8_t)n) s->div_k = 0;
                s->sign = (int8_t)((int)s->div_k * 2 < n ? 1 : -1);
            }
        }
        if (s->idle > lmax * 2u) { s->have_g0 = 0; s->has_pg = 0; s->cycles = 0; s->hn = 0; if (!frozen) s->has_play = 0; }
        if (++s->w >= rlen) s->w = 0;
        if (mode == 3 && !gate) wet = x * (float)s->sign;
        else {
            /* the playing group: at its end, pick what plays next */
            if (!s->has_play || s->pl_pos >= s->pl_len) {
                int take = 0, again = 0;
                if (!s->has_play) take = s->has_pg;
                else if (frozen) { again = 1; if (mode == 4 && !gate && s->has_pg && s->pg_seq != s->taken_seq && (float)(pw_rand_u(&s->seed) & 1023u) < amt * 1024.0f) take = 1; }
                else if (mode == 0 && s->reps > 0) { --s->reps; again = 1; }
                else take = s->has_pg;
                uint32_t n = s->pg_len;
                if (take && s->has_pg) {
                    if (mode >= 5) { n = ws_build(s, buf, rlen, play, lmax, mode, amt); if (!n) take = 0; }
                    else for (uint32_t k = 0, ix = s->pg_start; k < n; ++k) { play[k] = buf[ix]; if (++ix >= rlen) ix = 0; }
                }
                if (take && s->has_pg) {
                    s->pl_len = n; s->taken_seq = s->pg_seq; s->pl_pos = 0; s->has_play = 1;
                    s->rev = (mode == 1) && (float)(pw_rand_u(&s->seed) & 1023u) < amt * 1024.0f;
                    s->mute = (mode == 2) && (float)(pw_rand_u(&s->seed) & 1023u) < amt * 1024.0f;
                    s->reps = mode == 0 ? (uint16_t)(amt * 7.0f + 0.5f) : 0;
                } else if (again && s->has_play) s->pl_pos = 0;
                else if (!frozen && !s->has_pg) s->has_play = 0;
                else if (s->has_play) s->pl_pos = 0;
            }
            if (s->has_play) {
                uint32_t p = s->pl_pos++;
                wet = s->mute ? 0.0f : (float)play[s->rev ? s->pl_len - 1u - p : p] * (1.0f / DL_SCALE);
            }
        }
        out[0][i] = x * (1.0f - mix) + wet * mix;
    }
}

/* PITCH SHIFTER: two read heads slide through a short delay at the new speed, each faded in and out over
 * WINDOW and half a window apart, so one is always mid-fade as the other jumps back: the sound up or down
 * SHIFT semitones, as long as it was. Short windows suit drums, long ones chords. */
typedef struct { uint32_t w; float d; } pshift_st;
PROC(pshift_proc)
{
    ST(pshift_st);
    int16_t *buf = PW_POOL16(m);
    uint32_t len = m->mem_len / 2u;
    if (m->status != PW_STATUS_OK || len < 1024) { pass_through(in, out, 1); return; }
    float win = pw_clamp(m->p[2] * PW_SRF, 256.0f, (float)(len - 8u)), mix = m->p[3];
    float semis = m->p[0] + m->p[1] * 0.01f, r = pw_exp2(semis / 12.0f);
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(1) && (i & 7) == 0) r = pw_exp2(semis / 12.0f + in[1][i]);
        buf[s->w] = dl_word(in[0][i]);
        s->d += 1.0f - r;                               /* the distance behind the write head grows or shrinks */
        if (s->d >= win) s->d -= win;
        if (s->d < 0) s->d += win;
        float wet = 0;
        for (int h = 0; h < 2; ++h) {
            float d = s->d + (h ? win * 0.5f : 0.0f);
            if (d >= win) d -= win;
            float g = 1.0f - pw_absf(2.0f * d / win - 1.0f);
            wet += g * dl_read(buf, len, s->w, d + 2.0f);
        }
        if (++s->w >= len) s->w = 0;
        out[0][i] = in[0][i] * (1.0f - mix) + wet * mix;
    }
}

/* TRIG SEQ: a drum machine's grid: four tracks of 16 steps, each step off, on, accented or a roll (ROLL quick
 * hits inside the step). Each track its own length (polymeter), chance and shift; SWING delays the off-beat
 * steps; FILL held high plays a fill (every hit a roll, double time, or extra hits by chance). Unpatched clock
 * follows PLAY in 16ths. ACC is high with every accented hit, for a drum's ACCENT input. */
static const char *const L_trate[] = {"1/8", "1/16", "1/32"};
static const char *const L_fill[] = {"ROLL", "DOUBLE", "RANDOM"};
typedef struct {
    int8_t pos[4], idx[4]; uint8_t clk_hi, rst_hi, fill_hi, roll_k[4];
    uint32_t seed, ticks, steps;
    int32_t since, period, pend[2], trig[4], acc, roll_next[4], roll_gap[4];
    float chance, rnd; int8_t shift; uint8_t mon_ser;
} tseq_st;
#define TS_GET(m, t, i) (((uint32_t)(m)->data[(t) * 4 + ((i) >> 2)] >> (((i) & 3) * 2)) & 3u)
static void tseq_init(pw_module *m)
{
    static const uint8_t init[4][16] = {{2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0},
                                        {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 3},
                                        {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2, 0},
                                        {0}};
    memset(m->data, 0, 16);
    for (int t = 0; t < 4; ++t)
        for (int i = 0; i < 16; ++i) m->data[t * 4 + (i >> 2)] |= (uint8_t)(init[t][i] << ((i & 3) * 2));
    ST(tseq_st);
    for (int t = 0; t < 4; ++t) { s->pos[t] = -1; s->idx[t] = -1; }
    s->pend[0] = s->pend[1] = -1; s->mon_ser = pws.tmon_ser;
    s->seed = 0x7215u + (uint32_t)(m - pws.mod) * 2654435761u;
}
static PW_DSP void tseq_step(pw_module *m, tseq_st *s, int fill_mode, float step_len)
{
    for (int t = 0; t < 4; ++t) {
        int len = (int)(m->p[4 + t] + 0.5f); if (len < 1) len = 1; if (len > 16) len = 16;
        int rot = (int)(m->p[12 + t] + 0.5f) + s->shift;
        s->pos[t] = (int8_t)(s->pos[t] < 0 || s->pos[t] + 1 >= len ? 0 : s->pos[t] + 1);
        int idx = ((s->pos[t] - rot) % len + len) % len;
        s->idx[t] = (int8_t)idx;
        unsigned v = TS_GET(m, t, idx);
        if (fill_mode == 0 && v) v = 3;
        if (fill_mode == 2 && !v && pw_rand_f(&s->seed) > 0.4f) v = 1;
        if (v && (pw_rand_f(&s->seed) * 0.5f + 0.5f) > m->p[8 + t] * s->chance) v = 0;
        if (!v) continue;
        s->trig[t] = 220;
        if (v == 2) s->acc = 220;
        if (v == 3) {
            int n = (int)(m->p[2] + 0.5f);
            s->roll_k[t] = (uint8_t)(n - 1);
            s->roll_gap[t] = (int32_t)(step_len / (float)n);
            s->roll_next[t] = s->roll_gap[t];
            if (s->trig[t] > s->roll_gap[t] / 2) s->trig[t] = s->roll_gap[t] / 2;
        }
    }
    s->rnd = (pw_rand_f(&s->seed) * 0.5f + 0.5f) * 5.0f;
    ++s->steps;
}
PROC_COLD(tseq_proc)
{
    ST(tseq_st);
    int rate = (int)(m->p[0] + 0.5f), fill_mode = (int)(m->p[3] + 0.5f);
    float swing = m->p[1];
    if (s->period <= 0) s->period = (int32_t)(PW_SRF * 15.0f / pws.bpm);
    if (pws.reset_now && !IN_CONNECTED(0)) {
        for (int t = 0; t < 4; ++t) s->pos[t] = -1;
        s->ticks = s->steps = 0; s->pend[0] = s->pend[1] = -1;
    }
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(1) && pw_rise(&s->rst_hi, in[1][i])) {
            for (int t = 0; t < 4; ++t) s->pos[t] = -1;
            s->ticks = s->steps = 0; s->pend[0] = s->pend[1] = -1;
        }
        int filling = IN_CONNECTED(2) && in[2][i] > 1.0f;
        s->chance = IN_CONNECTED(3) ? pw_clamp(in[3][i] * 0.2f, 0.0f, 1.0f) : 1.0f;
        s->shift = IN_CONNECTED(4) ? (int8_t)pw_clamp(in[4][i] * 3.2f, 0.0f, 15.0f) : 0;
        if (IN_CONNECTED(5)) swing = pw_clamp(m->p[1] + in[5][i] * 0.1f, 0.0f, 0.5f);
        int r = rate + (filling && fill_mode == 1 ? 1 : 0);
        int tick = IN_CONNECTED(0) ? pw_rise(&s->clk_hi, in[0][i]) : pws.tick_at[i];
        ++s->since;
        if (tick) {
            if (IN_CONNECTED(0)) { if (s->since > 64 && s->since < (int32_t)(PW_SRF * 4.0f)) s->period = s->since; }
            else s->period = (int32_t)(PW_SRF * 15.0f / pws.bpm);
            s->since = 0;
            /* the steps this clock: 1/8 every other clock, 1/16 one, 1/32 two (the second half a clock on) */
            if (r == 0) { if (!(s->ticks & 1u)) s->pend[0] = 0; }
            else if (r == 1) s->pend[0] = 0;
            else { s->pend[0] = 0; s->pend[1] = s->period / 2; }
            ++s->ticks;
            /* swing: an off-beat step waits (its place among the steps: steps counts the ones already played) */
            float step_len = (float)s->period * (r == 0 ? 2.0f : r == 1 ? 1.0f : 0.5f);
            for (int k = 0; k < 2; ++k)
                if (s->pend[k] >= 0 && ((s->steps + (uint32_t)k) & 1u)) s->pend[k] += (int32_t)(swing * step_len);
        }
        for (int k = 0; k < 2; ++k)
            if (s->pend[k] >= 0 && s->pend[k]-- == 0) {
                float step_len = (float)s->period * (r == 0 ? 2.0f : r == 1 ? 1.0f : 0.5f);
                tseq_step(m, s, filling ? fill_mode : -1, step_len);
            }
        if (m->p[16] > 0.5f && pws.tmon_slot == (uint8_t)(m - pws.mod) && s->mon_ser != pws.tmon_ser) {   /* MONITOR: the hit being recorded */
            s->mon_ser = pws.tmon_ser;
            s->trig[pws.tmon_track & 3] = 88;
            if (pws.tmon_acc) s->acc = 88;
        }
        for (int t = 0; t < 4; ++t) {
            if (s->roll_k[t] && --s->roll_next[t] <= 0) {
                s->trig[t] = s->roll_gap[t] / 2 < 220 ? s->roll_gap[t] / 2 : 220;
                s->roll_next[t] = s->roll_gap[t];
                --s->roll_k[t];
            }
            out[t][i] = s->trig[t] > 0 ? 5.0f : 0.0f;
            if (s->trig[t] > 0) --s->trig[t];
        }
        out[4][i] = s->acc > 0 ? 5.0f : 0.0f;
        out[5][i] = s->rnd;
        if (s->acc > 0) --s->acc;
    }
}
/* control side: a step's state (0 off, 1 on, 2 accent, 3 roll), and where a track plays (-1: stopped) */
int pw_trig_get(unsigned slot, unsigned track, unsigned step)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || pws.mod[slot].type != PW_T_TRIGSEQ || track > 3 || step > 15) return 0;
    return (int)TS_GET(&pws.mod[slot], track, step);
}
void pw_trig_monitor(unsigned slot, unsigned track, int accent)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || pws.mod[slot].type != PW_T_TRIGSEQ) return;
    pws.tmon_slot = (uint8_t)slot; pws.tmon_track = (uint8_t)(track & 3u); pws.tmon_acc = (uint8_t)(accent != 0); pws.tmon_ser++;
}
void pw_trig_set(unsigned slot, unsigned track, unsigned step, int v)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || pws.mod[slot].type != PW_T_TRIGSEQ || track > 3 || step > 15) return;
    pw_undo_data(slot);
    uint8_t *b = &pws.mod[slot].data[track * 4 + (step >> 2)];
    unsigned sh = (step & 3) * 2;
    *b = (uint8_t)((*b & ~(3u << sh)) | ((unsigned)(v & 3) << sh));
    pws.revision++;
}

/* ======================================================= Ableton-style samplers */
/* the next noise sample, -1..1 */
PW_INLINE float rnd1(uint32_t *s) { return pw_rand_f(s); }
/* a 4-point cubic between b and c (a before, d after), t 0..1 */
PW_INLINE float hermite(float a, float b, float c, float d, float t)
{
    float c1 = 0.5f * (c - a), c2 = a - 2.5f * b + 2.0f * c - 0.5f * d, c3 = 0.5f * (d - a) + 1.5f * (b - c);
    return ((c3 * t + c2) * t + c1) * t + b;
}
PW_INLINE float smp_at(const pw_smp *d, pw_rdr *rd, int32_t frames, float pos)
{
    int32_t i = (int32_t)pos;
    float t = pos - (float)i;
    float a = pw_smp_get(d, rd, (uint32_t)(i > 0 ? i - 1 : 0)), b = pw_smp_get(d, rd, (uint32_t)(i < frames ? i : frames - 1));
    float c = pw_smp_get(d, rd, (uint32_t)(i + 1 < frames ? i + 1 : frames - 1)), e = pw_smp_get(d, rd, (uint32_t)(i + 2 < frames ? i + 2 : frames - 1));
    return hermite(a, b, c, e, t) * (5.0f / PW_SMP_SCALE);
}

/* DRUM RACK: eight pads, each a sound (eight drawn here, KICK..COWBL, or a bank sample: INTERNAL, A or B) with its own
 * tune, decay and level; T1-T8 or the keys (C to G of any octave, KEYS on) strike them; ACC high when a pad is
 * struck plays it louder; CHOKE: an open hat cut by the closed one (7+8), or every pad cutting the rest. Out in
 * stereo (the pads spread a little), pads 1-4 also on their own. */
/* a pad's SOUND: 0-7 the drawn drums, 8-39 the sounds of banks A-D (the bank's id + 8 less 16), 40-103 the kits' voices (BOOM KICK ..: a drawn drum with that kit's tune, decay and level, which picking it puts on the pad's own knobs), so a kit of any mix can be built */
enum { RK_BANK = 8, RK_VOICE = 40 };
static const char *const L_rack[] = {
"KICK", "SNARE", "CLAP", "HAT", "OHAT", "TOM", "RIM", "COWBL",
"A-1", "A-2", "A-3", "A-4", "A-5", "A-6", "A-7", "A-8",
"B-1", "B-2", "B-3", "B-4", "B-5", "B-6", "B-7", "B-8",
"C-1", "C-2", "C-3", "C-4", "C-5", "C-6", "C-7", "C-8",
"D-1", "D-2", "D-3", "D-4", "D-5", "D-6", "D-7", "D-8",
"BOOM KICK", "BOOM SNR", "BOOM CLAP", "BOOM HAT", "BOOM OHAT", "BOOM TOM", "BOOM RIM", "BOOM BELL",
"PUNCH KICK", "PUNCH SNR", "PUNCH CLAP", "PUNCH HAT", "PUNCH OHAT", "PUNCH TOM", "PUNCH RIM", "PUNCH BELL",
"LO-FI KICK", "LO-FI SNR", "LO-FI CLAP", "LO-FI HAT", "LO-FI OHAT", "LO-FI TOM", "LO-FI RIM", "LO-FI BELL",
"PERC KICK", "PERC SNR", "PERC CLAP", "PERC HAT", "PERC OHAT", "PERC TOM", "PERC RIM", "PERC BELL",
"GLITCH KICK", "GLITCH SNR", "GLITCH CLAP", "GLITCH HAT", "GLITCH OHAT", "GLITCH TOM", "GLITCH RIM", "GLITCH BELL",
"METAL KICK", "METAL SNR", "METAL CLAP", "METAL HAT", "METAL OHAT", "METAL TOM", "METAL RIM", "METAL BELL",
"DEEP KICK", "DEEP SNR", "DEEP CLAP", "DEEP HAT", "DEEP OHAT", "DEEP TOM", "DEEP RIM", "DEEP BELL",
"TOY KICK", "TOY SNR", "TOY CLAP", "TOY HAT", "TOY OHAT", "TOY TOM", "TOY RIM", "TOY BELL"};
static const char *const L_choke[] = {"OFF", "7+8", "ALL"};
static const char *const L_kit[] = {"CUSTOM", "BOOM", "PUNCH", "LO-FI", "PERC", "GLITCH", "METAL", "DEEP", "TOY", "A", "B", "C", "D"};
/* the kits: per pad SOUND (0-7 drawn, 8+ bank sounds: 8-23 INTERNAL, 24-39 A, 40-55 B), TUNE, DECAY, LEVEL; pads keep their parts (1 kick, 2 snare,
 * 3 clap, 4 closed hat, 5 open hat, 6 tom, 7 rim, 8 bell) so a beat made for one kit plays on the others */
typedef struct { int8_t sound, tune; uint8_t decay, level; } kit_pad;   /* decay in 10 ms, level in % */
static const kit_pad kits[8][8] = {
    {{0, -2, 90, 90}, {1, 0, 20, 75}, {2, 0, 30, 70}, {3, 0, 5, 55}, {4, 0, 40, 50}, {5, -3, 50, 75}, {6, 0, 5, 60}, {7, 0, 25, 50}},     /* BOOM */
    {{0, 3, 35, 95}, {1, 3, 16, 80}, {2, 1, 20, 70}, {3, 4, 4, 60}, {4, 4, 30, 50}, {5, 2, 28, 70}, {6, 3, 4, 65}, {7, 2, 15, 50}},       /* PUNCH */
    {{0, -5, 45, 90}, {1, -5, 12, 80}, {2, -4, 18, 70}, {3, -7, 3, 50}, {4, -7, 18, 45}, {5, -8, 30, 70}, {6, -6, 3, 60}, {7, -9, 12, 50}}, /* LO-FI */
    {{5, -7, 35, 80}, {5, 0, 30, 80}, {5, 5, 25, 75}, {3, 2, 3, 45}, {7, 0, 20, 55}, {7, 5, 15, 50}, {6, 0, 5, 60}, {2, 3, 15, 60}},     /* PERC */
    {{0, 12, 8, 90}, {1, 10, 5, 80}, {2, 14, 6, 70}, {3, 12, 2, 60}, {4, 9, 8, 50}, {5, 16, 6, 70}, {6, 7, 2, 65}, {7, 19, 4, 55}},          /* GLITCH */
    {{0, -4, 40, 90}, {7, -12, 25, 80}, {6, -10, 30, 70}, {3, -12, 15, 60}, {4, -14, 80, 50}, {7, 7, 60, 60}, {6, 12, 8, 65}, {7, -5, 90, 55}}, /* METAL */
    {{0, -9, 180, 95}, {1, -10, 40, 80}, {2, -8, 60, 70}, {3, -10, 8, 50}, {4, -12, 90, 45}, {5, -14, 120, 75}, {6, -12, 10, 60}, {7, -16, 60, 50}}, /* DEEP */
    {{0, 10, 12, 80}, {1, 12, 8, 70}, {2, 12, 10, 65}, {3, 14, 3, 55}, {4, 14, 15, 45}, {5, 12, 10, 70}, {6, 17, 3, 60}, {7, 14, 10, 55}},    /* TOY */
};
static void rack_kit(unsigned slot, int kit)
{
    if (kit <= 0 || kit > 12) return;
    for (unsigned k = 0; k < 8; ++k) {
        float snd, tune = 0, dec = 4.0f, lvl = 0.8f;
        if (kit <= 8) { const kit_pad *q = &kits[kit - 1][k]; snd = q->sound; tune = q->tune; dec = pw_maxf(0.02f, q->decay * 0.01f); lvl = q->level * 0.01f; }
        else snd = (float)RK_BANK + 8.0f * (float)(kit - 9) + (float)k;   /* A, B, C, D: the bank's eight sounds on the pads */
        pw_set_param(slot, 4 + 4 * k, snd); pw_set_param(slot, 5 + 4 * k, tune);
        pw_set_param(slot, 6 + 4 * k, dec); pw_set_param(slot, 7 + 4 * k, lvl);
    }
}
void pw_rack_voice(unsigned slot, unsigned p)   /* a pad's SOUND was turned to a kit voice: that voice's tune, decay and level go on the pad's knobs */
{
    int v = (int)(pws.mod[slot].p[p] + 0.5f) - RK_VOICE;
    if (v < 0 || v >= 64) return;
    const kit_pad *q = &kits[v / 8][v % 8];
    pw_set_param(slot, p + 1, (float)q->tune); pw_set_param(slot, p + 2, pw_maxf(0.02f, q->decay * 0.01f)); pw_set_param(slot, p + 3, q->level * 0.01f);
}
static const float rack_pan[8] PW_DSP_DATA = {0, 0.1f, -0.25f, 0.3f, 0.3f, -0.35f, 0.2f, -0.15f};
typedef struct { float pos, env, ph, pe, n1, n2, amp; uint8_t on, hi, fade; } pad_st;
typedef struct { pad_st pad[8]; uint32_t seed, serial; } rack_st;
/* (DECAY: the time to fall 40 dB) */
static void rack_init(pw_module *m) { ST(rack_st); s->seed = 0xD4ACu + (uint32_t)(m - pws.mod) * 2654435761u; s->serial = pws.kb_serial; }
static PW_DSP void rack_strike(pw_module *m, rack_st *s, int k, float amp)
{
    int choke = (int)(m->p[1] + 0.5f);
    for (int j = 0; j < 8; ++j)
        if (j != k && s->pad[j].on && (choke == 2 || (choke == 1 && k >= 6 && j >= 6))) s->pad[j].fade = 1;
    pad_st *d = &s->pad[k];
    d->on = 1; d->fade = 0; d->pos = 0; d->env = 1.0f; d->ph = 0; d->pe = 1.0f; d->amp = amp;
}
PROC_COLD(rack_proc)
{
    ST(rack_st);
    preset_cv(m, in[9], connected, 9);
    float vol = m->p[0] * 0.9f, acc = m->p[3];
    int keys = m->p[2] > 0.5f;
    if (keys && s->serial != pws.kb_serial) {           /* a new note: C..G strike pads 1..8 */
        s->serial = pws.kb_serial;
        int pc = ((int)pws.kb_pitch_note % 12 + 12) % 12;
        if (pc < 8 && pws.kb_gate) rack_strike(m, s, pc, 0.4f + pws.kb_vel * 0.9f);
    } else s->serial = pws.kb_serial;
    for (int i = 0; i < PW_N; ++i) { out[0][i] = out[1][i] = 0; for (int k = 0; k < 4; ++k) out[2 + k][i] = 0; }
    for (int k = 0; k < 8; ++k) {
        pad_st *d = &s->pad[k];
        uint8_t hi = d->hi;
        const float *trig = in[k];
        int sound = (int)(m->p[4 + 4 * k] + 0.5f);
        if (sound >= RK_VOICE) sound = kits[((sound - RK_VOICE) >> 3) & 7][(sound - RK_VOICE) & 7].sound;   /* (a kit's voice: its drawn drum) */
        float tune = pw_exp2(m->p[5 + 4 * k] / 12.0f), dec = pw_coef(m->p[6 + 4 * k] * 0.22f), lvl = m->p[7 + 4 * k] * vol;
        uint32_t frames = 0, rate = 44100;
        pw_smp smp;
        int have = sound >= RK_BANK && pw_smp_open((unsigned)(sound + 8), &smp);   /* (bank A-D: ids 16-47) */
        if (have) { frames = smp.frames; rate = smp.rate; }
        pw_rdr *rd = 0;   /* (this pad's place in a compressed sound, found when it first plays in the block) */
        float step = (float)rate * PW_INV_SR * tune;
        float pl = 1.0f - rack_pan[k], pr = 1.0f + rack_pan[k];
        for (int i = 0; i < PW_N; ++i) {
            if (IN_CONNECTED(k) && pw_rise(&hi, trig[i])) rack_strike(m, s, k, IN_CONNECTED(8) && in[8][i] > 1.0f ? 1.0f + acc : 1.0f);
            if (!d->on) continue;
            float y = 0, t = d->pos * PW_INV_SR;
            switch (sound) {
            case 0: { float f = 50.0f * tune * (1.0f + 3.0f * d->pe); d->pe *= 0.9985f; y = pw_sin1(d->ph); d->ph += f * PW_INV_SR; } break;
            case 1: { float f = 185.0f * tune * (1.0f + 0.6f * d->pe); d->pe *= 0.997f; float n = rnd1(&s->seed);
                      d->n1 += 0.35f * (n - d->n1); y = 0.55f * pw_sin1(d->ph) + 0.8f * (n - d->n1); d->ph += f * PW_INV_SR; } break;
            case 2: { float n = rnd1(&s->seed), g = 0.17f * tune;     /* band-passed noise in three quick claps */
                      float hp = n - d->n1 - 0.6f * d->n2; d->n2 += g * hp; d->n1 += g * d->n2;
                      float burst = t < 0.03f ? (pw_frac(t * 100.0f) < 0.35f ? 1.0f : 0.15f) : 1.0f;
                      y = 1.3f * d->n2 * burst; } break;
            case 3: case 4: { float n = rnd1(&s->seed); d->n1 += 0.55f * (n - d->n1); float hp = n - d->n1;
                      d->n2 += 0.5f * tune * (hp - d->n2); y = 2.0f * (hp - 0.3f * d->n2); } break;
            case 5: { float f = 110.0f * tune * (1.0f + 1.2f * d->pe); d->pe *= 0.9993f; y = 0.8f * pw_sin1(d->ph); d->ph += f * PW_INV_SR; } break;
            case 6: { y = 1.1f * pw_sin1(d->ph * 1700.0f * tune) + 0.8f * pw_sin1(d->ph * 480.0f * tune); d->ph += PW_INV_SR;
                      if (t > 0.002f) y *= 0.6f; } break;
            case 7: { float a = pw_sin1(d->ph * 540.0f * tune) > 0 ? 1.0f : -1.0f, b = pw_sin1(d->ph * 800.0f * tune) > 0 ? 1.0f : -1.0f;
                      d->ph += PW_INV_SR; float x = 0.5f * (a + b); float hp = x - d->n1 - 0.4f * d->n2; d->n2 += 0.12f * hp; d->n1 += 0.12f * d->n2;
                      y = d->n2 * 0.6f; } break;
            default:
                if (!have || d->pos >= (float)frames - 1.0f) { d->on = 0; continue; }
                if (!rd) rd = pw_rdr_get(((unsigned)(m - pws.mod) << 4 | (unsigned)k) + 1u, &smp);
                y = smp_at(&smp, rd, (int32_t)frames, d->pos) * 0.2f;
                d->pos += step - 1.0f;          /* (the +1 below keeps time for the drawn sounds) */
                break;
            }
            if (d->ph >= 1.0f && sound != 6 && sound != 7) d->ph -= 1.0f;
            d->pos += 1.0f;
            d->env *= d->fade ? 0.995f : dec;
            if (d->env < 0.0005f) { d->on = 0; continue; }
            y *= d->env * d->amp * lvl * 4.0f;
            out[0][i] += y * pl; out[1][i] += y * pr;
            if (k < 4) out[2 + k][i] = y;
        }
        d->hi = hi;
        if (pw_absf(d->n1) < 1e-20f) d->n1 = 0;
        if (pw_absf(d->n2) < 1e-20f) d->n2 = 0;
    }
}

/* SAMPLER: a sound played as an instrument, as Ableton's Simpler. SAMPLE: RAM (its own memory: a drum loop to begin
 * with, what REC records from IN, a file the emulator loads) or BANK (INTERNAL, A-D) and its 8 sounds. MODE: CLASSIC (follows V/OCT,
 * loops while held), 1SHOT (plays its region through), SLICE (the region cut in SLICES, EVEN or at its TRANSIENTS
 * (SENS); the note above C4 or SLICE picks one) or LOOP (each trigger restarts it, it loops until the next).
 * START, LENGTH, LOOP (forward, alternating) and REVERSE shape what plays; WARP stretches time apart from pitch
 * (grains of GRAIN length, the sound at SPEED); an ADSR, a resonant low-pass the envelope opens, velocity, glide.
 * Hermite interpolation. */
static const char *const L_scmode[] = {"CLASSIC", "1SHOT", "SLICE", "LOOP"};
static const char *const L_loop[] = {"OFF", "FWD", "ALT"};
static const char *const L_sliceby[] = {"EVEN", "TRANS"};
enum { SM_CLASSIC, SM_1SHOT, SM_SLICE, SM_LOOP };
/* parameters */
enum { SP_SAMPLE, SP_MODE, SP_TUNE, SP_LEVEL, SP_START, SP_LENGTH, SP_LOOP, SP_REV, SP_WARP, SP_SPEED, SP_GRAIN, SP_FINE,
       SP_SLICES, SP_SLICEBY, SP_SENS, SP_GLIDE, SP_ATT, SP_DEC, SP_SUS, SP_REL, SP_CUT, SP_RES, SP_ENVAMT, SP_VEL, SP_BEATS, SP_BANK };
typedef struct {
    float pos, env, pitch, ic1, ic2, amp, s0, s1;
    float gph[2], gst[2], goff[2];
    int32_t rec_pos, ram_frames;
    float bspd;                       /* BEATS: the stretch that makes the region last that many beats at the tempo (taken at each note) */
    int8_t dir; uint8_t stage, gate_hi, playing, rec_hi, rec_req, ram_rev; uint16_t end_pulse;   /* rec_req: the panel's REC: 0 off, 1 armed, 2 recording, 3 stop asked, 4 IN lost, 5 nothing was patched (read once) */
    uint8_t msrc;                     /* the sound SAMPLE's input chose at the last note: 1 + (0 RAM, 1-16 the BANK's sounds); 0 unpatched */
} samp_st;
/* the transient slices live in data[]: 16 starts (fractions of the sound, 16 bits), their count, and what they
 * were found for */
#define SD_COUNT 32
#define SD_SIG 36
#define SD_RAM 40                 /* data[40] bit 0: the RAM holds a sound that was recorded or loaded (it is stored with the patch) */
#define SMP_REC_MAX 44100         /* the longest the panel's REC records, and what a patch keeps: one second */
static int samp_source(const pw_module *m, const samp_st *s, pw_smp *o)   /* 0: nothing to play */
{
    int src = s->msrc ? s->msrc - 1 : (int)(m->p[SP_SAMPLE] + 0.5f);
    if (src <= 0 && m->p[SP_BANK] > 0.5f) src = 1;   /* (RAM is the INTERNAL bank's first entry: A-D start at their sound 1) */
    if (src <= 0) {
        uint32_t frames = m->status == PW_STATUS_OK && m->mem_len ? (uint32_t)s->ram_frames : 0;
        pw_smp_ram(o, frames ? (const int16_t *)PW_POOL16(m) : 0, frames, 44100);
        return frames != 0;
    }
    if (src > 8) return 0;
    return pw_smp_open(pw_snd_id((int)(m->p[SP_BANK] + 0.5f), src), o);   /* (INTERNAL, A, B, C, D: eight sounds each) */
}
#include "pw_break.inc"
static void sampler_default(pw_module *m)
{
    /* The sound a new SAMPLER starts with in its RAM: a one-second drum break, stored as a 22.05 kHz SEA stream (pw_break.inc) and decoded here,
     * the rate doubled by linear interpolation, in place: the decoded half sits at the end of the buffer and is read one frame ahead of what is written. */
    ST(samp_st);
    int16_t *buf = PW_POOL16(m);
    int32_t cap = (int32_t)(m->mem_len / 2u);
    int32_t n = cap < 2 * (int32_t)PW_BREAK_FRAMES ? cap & ~1 : 2 * (int32_t)PW_BREAK_FRAMES, h = n / 2;
    pw_smp b = {0, pw_break_sea, PW_BREAK_CODED, PW_BREAK_RATE};
    pw_smp_read(&b, 0, (uint32_t)h, buf + n - h);
    for (int32_t i = 0; i < h; ++i) {
        int32_t a0 = buf[n - h + i], a1 = i + 1 < h ? buf[n - h + i + 1] : a0;
        buf[2 * i] = (int16_t)a0; buf[2 * i + 1] = (int16_t)((a0 + a1) / 2);
    }
    s->ram_frames = n;
}
static void sampler_init(pw_module *m) { if (m->status == PW_STATUS_OK && m->mem_len) sampler_default(m); }
/* the slice idx of [a, b): even, or at the transients found */
static void samp_slice(const pw_module *m, float s0, float s1, float F, int idx, float *a, float *b)
{
    int n = (int)(m->p[SP_SLICES] + 0.5f);
    if (m->p[SP_SLICEBY] > 0.5f && m->data[SD_COUNT] > 1) {
        n = m->data[SD_COUNT];
        idx %= n;
        float x0 = (float)(m->data[idx * 2] | (m->data[idx * 2 + 1] << 8)) * (1.0f / 65535.0f) * F;
        float x1 = idx + 1 < n ? (float)(m->data[idx * 2 + 2] | (m->data[idx * 2 + 3] << 8)) * (1.0f / 65535.0f) * F : s1;
        *a = pw_clamp(x0, s0, s1); *b = pw_clamp(x1, *a + 32.0f, s1);
        return;
    }
    if (n < 1) n = 1;
    idx %= n;
    float len = (s1 - s0) / (float)n;
    *a = s0 + len * (float)idx; *b = *a + len;
}
static int samp_slices(const pw_module *m)
{
    return m->p[SP_SLICEBY] > 0.5f && m->data[SD_COUNT] > 1 ? m->data[SD_COUNT] : (int)(m->p[SP_SLICES] + 0.5f);
}
PROC(sampler_proc)
{
    ST(samp_st);
    uint32_t frames = 0, rate = 44100;
    int16_t *ram = m->status == PW_STATUS_OK && m->mem_len ? PW_POOL16(m) : 0;
    int32_t cap = ram ? (int32_t)(m->mem_len / 2u) : 0;
    pw_smp smp;
    int have = samp_source(m, s, &smp);
    if (have) { frames = smp.frames; rate = smp.rate; }
    pw_rdr *rd[3] = {0, 0, 0};   /* (the voice's places in a compressed sound: the play head, the two warp grains; found when first read in the block) */
    const unsigned rkey = ((unsigned)(m - pws.mod) << 4) + 1u;
    int mode = (int)(m->p[SP_MODE] + 0.5f), loop = (int)(m->p[SP_LOOP] + 0.5f), rev = m->p[SP_REV] > 0.5f, warp = m->p[SP_WARP] > 0.5f || m->p[SP_BEATS] > 0.5f;   /* (BEATS stretches: it is a warp) */
    float ka = 1.0f - pw_coef(m->p[SP_ATT] / 1.466f), kd = 1.0f - pw_coef(m->p[SP_DEC] / 4.0f), sus = m->p[SP_SUS];
    float kr = 1.0f - pw_coef(m->p[SP_REL] / 4.0f), res = m->p[SP_RES], envamt = m->p[SP_ENVAMT] * 6.0f, velamt = m->p[SP_VEL];
    float kg = 1.0f - pw_coef(m->p[SP_GLIDE] * 0.3f), base = (m->p[SP_TUNE] + m->p[SP_FINE] * 0.01f) / 12.0f, lvl = m->p[SP_LEVEL];
    float speed = m->p[SP_SPEED], ginc = 1.0f / (pw_maxf(m->p[SP_GRAIN], 0.005f) * PW_SRF);
    float k = 2.0f - 1.96f * res, g = 0;
    float F = (float)frames, rk = (float)rate * PW_INV_SR;
    int filt = !(m->p[SP_CUT] >= 17900.0f && envamt == 0.0f && !IN_CONNECTED(4));
    for (int i = 0; i < PW_N; ++i) {
        /* recording: REC high writes IN into the module's own memory */
        if (ram && s->rec_req >= 2 && s->rec_req <= 4) {   /* the panel's REC: IN into the RAM, a second at most (REC again or PLAY stops it) */
            if (s->rec_req == 2 && !IN_CONNECTED(6)) s->rec_req = 4;
            else if (s->rec_req == 2 && s->rec_pos < cap && s->rec_pos < SMP_REC_MAX) ram[s->rec_pos++] = (int16_t)(pw_clamp(in[6][i] * 0.2f, -1.25f, 1.25f) * PW_SMP_SCALE);
            if (s->rec_req != 2 || s->rec_pos >= cap || s->rec_pos >= SMP_REC_MAX) {
                int ok = s->rec_pos > 64;
                if (ok) { s->ram_frames = s->rec_pos; ++s->ram_rev; m->data[SD_RAM] |= 1u; if ((int)(m->p[SP_SAMPLE] + 0.5f) == 0) { frames = (uint32_t)s->ram_frames; F = (float)frames; } }
                s->rec_req = (uint8_t)(s->rec_req == 4 && !ok ? 5 : 0);
            }
        }
        if (ram && IN_CONNECTED(7)) {
            uint8_t was = s->rec_hi;
            if (pw_rise(&s->rec_hi, in[7][i])) s->rec_pos = 0;
            if (s->rec_hi && s->rec_pos < cap) ram[s->rec_pos++] = (int16_t)(pw_clamp(in[6][i] * 0.2f, -1.25f, 1.25f) * PW_SMP_SCALE);
            if (was && !s->rec_hi && s->rec_pos > 64) { s->ram_frames = s->rec_pos; ++s->ram_rev; m->data[SD_RAM] |= 1u; if ((int)(m->p[SP_SAMPLE] + 0.5f) == 0) { frames = (uint32_t)s->ram_frames; F = (float)frames; } }
        }
        uint8_t was = s->gate_hi;
        int rise = pw_rise(&s->gate_hi, in[1][i]), gate = s->gate_hi;
        if (rise && IN_CONNECTED(8)) {          /* SAMPLE's input: 0.625 V a sound (5 V: 8 on), taken at each note */
            int src = ((int)(m->p[SP_SAMPLE] + 0.5f) + pw_floori(in[8][i] * 1.6f + 0.5f)) % 9;
            s->msrc = (uint8_t)(1 + (src < 0 ? src + 9 : src));
            have = samp_source(m, s, &smp); frames = have ? smp.frames : 0; rate = have ? smp.rate : 44100; F = (float)frames; rk = (float)rate * PW_INV_SR;
        } else if (!IN_CONNECTED(8) && s->msrc) { s->msrc = 0; have = samp_source(m, s, &smp); frames = have ? smp.frames : 0; rate = have ? smp.rate : 44100; F = (float)frames; rk = (float)rate * PW_INV_SR; }
        if (!have || frames < 64) { out[0][i] = out[1][i] = out[2][i] = 0; s->playing = 0; continue; }
        if (rise) {
            float r0 = pw_clamp(m->p[SP_START] + (IN_CONNECTED(3) ? in[3][i] * 0.2f : 0.0f), 0.0f, 0.99f) * F;
            float r1 = pw_minf(r0 + pw_maxf(m->p[SP_LENGTH] * (F - r0), 64.0f), F - 2.0f);
            s->bspd = 1.0f;
            if (m->p[SP_BEATS] > 0.5f) {   /* BEATS: the region (the whole of it, not one slice) lasts that many beats at the tempo */
                float beats = (float)(int)(m->p[SP_BEATS] + 0.5f), secs = (r1 - r0) / (float)rate;
                s->bspd = pw_clamp(secs * pw_maxf(pws.bpm, 20.0f) / (60.0f * beats), 0.1f, 10.0f);
            }
            if (mode == SM_SLICE) {
                int n = samp_slices(m);
                int idx = IN_CONNECTED(5) ? (int)(pw_clamp(in[5][i] * 0.2f, 0.0f, 0.999f) * (float)n)
                                          : ((pw_floori(in[0][i] * 12.0f + 0.5f)) % n + n) % n;
                samp_slice(m, r0, r1, F, idx, &r0, &r1);
            }
            s->s0 = r0; s->s1 = r1;
            s->pos = rev ? s->s1 - 1.0f : s->s0; s->dir = (int8_t)(rev ? -1 : 1);
            s->gph[0] = 0; s->gph[1] = 0.5f; s->gst[0] = s->gst[1] = s->pos; s->goff[0] = s->goff[1] = 0;
            s->stage = 1; s->playing = 1;
            float v = IN_CONNECTED(2) ? pw_clamp(in[2][i] * 0.2f, 0.0f, 1.0f) : 1.0f;
            s->amp = 1.0f - velamt + velamt * v;
            if (kg >= 1.0f || !was) s->pitch = mode == SM_SLICE ? 0.0f : in[0][i];
        }
        if (was && !gate && s->stage && (mode == SM_CLASSIC || mode == SM_SLICE)) s->stage = 4;
        switch (s->stage) {
        case 1: s->env += (1.3f - s->env) * ka; if (s->env >= 1.0f) { s->env = 1.0f; s->stage = 2; } break;
        case 2: if (mode == SM_1SHOT || mode == SM_LOOP) s->env = 1.0f; else s->env += (sus - s->env) * kd; break;
        case 4: s->env -= s->env * kr; if (s->env < 0.0003f) { s->env = 0; s->stage = 0; s->playing = 0; } break;
        default: break;
        }
        float y = 0;
        if (s->playing) {
            float target = mode == SM_SLICE ? 0.0f : in[0][i];
            s->pitch = kg >= 1.0f ? target : s->pitch + (target - s->pitch) * kg;
            float pr = rk * pw_exp2(base + s->pitch);                    /* the pitch's read speed */
            float d = (float)s->dir;
            if (!warp) { if (!rd[0]) rd[0] = pw_rdr_get(rkey, &smp);
                y = smp_at(&smp, rd[0], (int32_t)frames, s->pos); s->pos += pr * d; }
            else {
                /* two grains half a grain apart, each from where the sound has got to, read at the pitch */
                for (int gi = 0; gi < 2; ++gi) {
                    s->gph[gi] += ginc;
                    if (s->gph[gi] >= 1.0f) { s->gph[gi] -= 1.0f; s->gst[gi] = s->pos; s->goff[gi] = 0; }
                    float rp = s->gst[gi] + s->goff[gi] * d;
                    if (rp < 0) rp = 0;
                    if (rp > F - 2.0f) rp = F - 2.0f;
                    float w = pw_sin1(s->gph[gi] * 0.5f);
                    if (!rd[1 + gi]) rd[1 + gi] = pw_rdr_get(rkey + 1u + (unsigned)gi, &smp);
                    y += w * w * smp_at(&smp, rd[1 + gi], (int32_t)frames, rp);
                    s->goff[gi] += pr;
                }
                s->pos += rk * speed * s->bspd * d;
            }
            int looping = (mode == SM_CLASSIC && loop && (gate || s->stage == 4)) || mode == SM_LOOP;
            if (s->pos >= s->s1 || s->pos < s->s0) {
                if (looping && (loop != 2 || mode == SM_LOOP)) s->pos = s->dir > 0 ? s->s0 + (s->pos - s->s1) : s->s1 - (s->s0 - s->pos);
                else if (looping) { s->dir = (int8_t)-s->dir; s->pos = pw_clamp(s->pos, s->s0, s->s1 - 1.0f); }
                else { s->playing = 0; s->stage = 0; s->env = 0; s->end_pulse = 88; }
                if (mode == SM_LOOP) s->end_pulse = 88;
            }
        }
        if ((i & 7) == 0 && filt) {
            float fc = m->p[SP_CUT] * pw_exp2(envamt * s->env + (IN_CONNECTED(4) ? in[4][i] : 0.0f));
            g = pw_tan_pi(pw_clamp(fc, 20.0f, 18000.0f) * PW_INV_SR);
        }
        float x = y * s->env * s->amp, o = x;
        if (filt) {
            float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            float v3 = x - s->ic2, v1 = a1 * s->ic1 + a2 * v3, v2 = s->ic2 + a2 * s->ic1 + a3 * v3;
            s->ic1 = 2.0f * v1 - s->ic1; s->ic2 = 2.0f * v2 - s->ic2;
            o = v2;
        }
        out[0][i] = o * lvl;
        out[1][i] = s->env * 5.0f;
        out[2][i] = s->end_pulse ? 5.0f : 0.0f;
        if (s->end_pulse) --s->end_pulse;
    }
    if (pw_absf(s->ic1) < 1e-20f) s->ic1 = 0;
    if (pw_absf(s->ic2) < 1e-20f) s->ic2 = 0;
}
/* the emulator's file into SAMPLE RAM */
int16_t *pw_sampler_buffer(unsigned slot, unsigned *frames)
{
    if (slot >= PW_MAX_MODULES || !pws.mod[slot].used || pws.mod[slot].type != PW_T_SAMPLER ||
        pws.mod[slot].status != PW_STATUS_OK || !pws.mod[slot].mem_len) return 0;
    if (frames) *frames = pws.mod[slot].mem_len / 2u;
    return PW_POOL16(&pws.mod[slot]);
}
/* the panel's REC on a SAMPLER (EDIT page): a press arms it, the next records IN into the RAM (a second at most), the next stops. Returns the state: 0 off, 1 armed, 2 recording; -1 no memory */
int pw_sampler_rec(unsigned slot, int press)
{
    if (!pw_sampler_buffer(slot, 0)) return -1;
    samp_st *s = (samp_st *)(void *)pws.mod[slot].st.raw;
    if (!press) return s->rec_req == 1 || s->rec_req == 2 ? s->rec_req : s->rec_req == 3 ? 2 : 0;
    if (press < 0) { if (s->rec_req == 1) s->rec_req = 0; else if (s->rec_req == 2) s->rec_req = 3; return s->rec_req == 1 ? 1 : 0; }
    if (s->rec_req == 1) { s->rec_pos = 0; s->rec_req = 2; }
    else if (s->rec_req == 2) s->rec_req = 3;
    else if (s->rec_req != 3) { s->rec_req = 1; if (pws.mod[slot].p[SP_BANK] > 0.5f || pws.mod[slot].p[SP_SAMPLE] > 0.5f) { pws.mod[slot].p[SP_BANK] = 0; pws.mod[slot].p[SP_SAMPLE] = 0; pws.revision++; } }   /* (what is recorded is the RAM: it is what plays) */
    return s->rec_req == 3 ? 0 : s->rec_req;
}
unsigned pw_sampler_rev(unsigned slot)   /* changes whenever the RAM's sound does (a recording ends, a file or a stored sound comes in) or grows (recording) */
{
    if (!pw_sampler_buffer(slot, 0)) return 0;
    const samp_st *s = (const samp_st *)(const void *)pws.mod[slot].st.raw;
    return (unsigned)s->ram_rev + (s->rec_req == 2 ? 256u + ((unsigned)s->rec_pos >> 12) * 512u : 0u);
}
int pw_sampler_rec_done(unsigned slot)   /* 1 after a recording ended on its own or was stopped, 2 when it found IN unpatched (read once) */
{
    if (!pw_sampler_buffer(slot, 0)) return 0;
    samp_st *s = (samp_st *)(void *)pws.mod[slot].st.raw;
    if (s->rec_req == 5) { s->rec_req = 0; return 2; }
    return 0;
}
int pw_sampler_recorded(unsigned slot, unsigned *frames)   /* the RAM holds a sound worth keeping with the patch: its frames (at most a second), or 0 */
{
    if (!pw_sampler_buffer(slot, 0) || !(pws.mod[slot].data[SD_RAM] & 1u)) return 0;
    unsigned f = (unsigned)((samp_st *)(void *)pws.mod[slot].st.raw)->ram_frames;
    if (frames) *frames = f > SMP_REC_MAX ? SMP_REC_MAX : f;
    return f > 64;
}
int pw_sampler_marked(unsigned slot) { return slot < PW_MAX_MODULES && pws.mod[slot].used == 1 && pws.mod[slot].type == PW_T_SAMPLER && (pws.mod[slot].data[SD_RAM] & 1u); }
void pw_sampler_unmark(unsigned slot) { if (slot < PW_MAX_MODULES) pws.mod[slot].data[SD_RAM] &= (uint8_t)~1u; }
void pw_sampler_commit(unsigned slot, unsigned frames)
{
    if (!pw_sampler_buffer(slot, 0)) return;
    samp_st *s = (samp_st *)(void *)pws.mod[slot].st.raw;
    unsigned cap = pws.mod[slot].mem_len / 2u;
    s->ram_frames = (int32_t)(frames > cap ? cap : frames);
    pws.mod[slot].data[SD_RAM] |= 1u; ++s->ram_rev;
    s->playing = 0;
    pws.revision++;
}
/* Control side: a sampler sliced at its transients finds them again when its sound, region, SLICES or SENS
 * change. The energy every 256 samples; a slice starts where it jumps well above the moment before (SENS: how
 * little a jump counts), at least 50 ms after the last; the strongest SLICES of them, in order. */
static uint32_t samp_sig(const pw_module *m, uint32_t frames)
{
    uint32_t h = 2166136261u;
    float v[6] = {m->p[SP_SAMPLE] + 100.0f * m->p[SP_BANK], m->p[SP_SLICES], m->p[SP_SENS], m->p[SP_START], m->p[SP_LENGTH], (float)frames};
    for (int i = 0; i < 6; ++i) { union { float f; uint32_t u; } x = { v[i] }; h = (h ^ x.u) * 16777619u; }
    return h | 1u;
}
PW_COLD static void samp_analyze(pw_module *m)
{
    const samp_st *s = (const samp_st *)(const void *)m->st.raw;
    pw_smp d;
    int have = samp_source(m, s, &d);
    uint32_t frames = have ? d.frames : 0, rate = have ? d.rate : 0;
    uint32_t sig = have ? samp_sig(m, frames) : 0, old = m->data[SD_SIG] | (m->data[SD_SIG + 1] << 8) | (m->data[SD_SIG + 2] << 16) | ((uint32_t)m->data[SD_SIG + 3] << 24);
    if (sig == old) return;
    m->data[SD_SIG] = (uint8_t)sig; m->data[SD_SIG + 1] = (uint8_t)(sig >> 8); m->data[SD_SIG + 2] = (uint8_t)(sig >> 16); m->data[SD_SIG + 3] = (uint8_t)(sig >> 24);
    m->data[SD_COUNT] = 0;
    pws.revision++;
    if (!have || frames < 1024) return;
    uint32_t r0 = (uint32_t)(pw_clamp(m->p[SP_START], 0.0f, 0.99f) * (float)frames);
    uint32_t r1 = r0 + (uint32_t)(m->p[SP_LENGTH] * (float)(frames - r0));
    if (r1 > frames) r1 = frames;
    enum { HOP = 256, MAXC = 64 };
    uint32_t cand[MAXC]; float str[MAXC]; unsigned nc = 0;
    float hist[8] = {0}, thr = 1.4f + (1.0f - m->p[SP_SENS]) * 4.0f, total = 0;
    unsigned hops = 0;
    int16_t hb[HOP];
    for (uint32_t a = r0; a + HOP <= r1; a += HOP) { float e = 0; pw_smp_read(&d, a, HOP, hb); for (uint32_t i = 0; i < HOP; i += 4) e += (float)(hb[i] < 0 ? -hb[i] : hb[i]); total += e; ++hops; }
    float floor_e = hops ? total / (float)hops * 0.15f : 0;
    uint32_t last = 0; int any = 0;
    for (uint32_t a = r0, j = 0; a + HOP <= r1; a += HOP, ++j) {
        float e = 0;
        pw_smp_read(&d, a, HOP, hb);
        for (uint32_t i = 0; i < HOP; i += 4) e += (float)(hb[i] < 0 ? -hb[i] : hb[i]);
        float before = 0; for (int q = 0; q < 8; ++q) before += hist[q];
        before = before / 8.0f + 1.0f;
        if (e > floor_e && e > before * thr && (!any || a - last > (uint32_t)(rate / 20u))) {
            if (nc < MAXC) { cand[nc] = a; str[nc] = e / before; ++nc; }
            last = a; any = 1;
        }
        hist[j & 7] = e;
    }
    /* the slice at the region's start always, then the strongest */
    unsigned want = (unsigned)(m->p[SP_SLICES] + 0.5f), n = 0;
    uint32_t pick[16];
    pick[n++] = r0;
    while (n < want && n < 16) {
        int best = -1;
        for (unsigned c = 0; c < nc; ++c) if (str[c] > 0 && cand[c] > r0 + rate / 40u && (best < 0 || str[c] > str[best])) best = (int)c;
        if (best < 0) break;
        pick[n++] = cand[best]; str[best] = 0;
    }
    for (unsigned a = 1; a < n; ++a) for (unsigned b = a; b > 0 && pick[b] < pick[b - 1]; --b) { uint32_t t = pick[b]; pick[b] = pick[b - 1]; pick[b - 1] = t; }
    for (unsigned q = 0; q < n; ++q) {
        uint32_t f = (uint32_t)((uint64_t)pick[q] * 65535u / frames);
        m->data[q * 2] = (uint8_t)f; m->data[q * 2 + 1] = (uint8_t)(f >> 8);
    }
    m->data[SD_COUNT] = (uint8_t)n;
}
int pw_preset_count(unsigned type)
{
    int pp = pw_preset_param(type);
    return pp < 0 || type >= PW_T_COUNT ? 0 : (int)(pw_types[type].param[pp].max + 0.5f);
}
PW_COLD void pw_maintain(void)
{
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        pw_module *m = &pws.mod[i];
        if (m->used == 1 && pw_preset_param(m->type) >= 0) {   /* a PRESET jack's cable: its preset's knobs */
            uint8_t e = pws.pre_eff[i];
            if (e == 0xff) pws.pre_seen[i] = 0xff;
            else if (pws.pre_seen[i] == 0xff) pws.pre_seen[i] = e;     /* (first look: the patch's own knobs stand) */
            else if (e != pws.pre_seen[i]) { pws.pre_seen[i] = e; if (e > 0) pw_preset_apply(i, e); }
        }
        if (m->used == 1 && m->type == PW_T_SAMPLER && m->p[SP_SLICEBY] > 0.5f && (int)(m->p[SP_MODE] + 0.5f) == SM_SLICE) samp_analyze(m);
    }
}
int pw_sampler_slice(unsigned slot, unsigned k, float *frac)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || pws.mod[slot].type != PW_T_SAMPLER) return 0;
    const pw_module *m = &pws.mod[slot];
    int n = samp_slices(m);
    if ((int)k >= n) return n;
    if (m->p[SP_SLICEBY] > 0.5f && m->data[SD_COUNT] > 1) *frac = (float)(m->data[k * 2] | (m->data[k * 2 + 1] << 8)) / 65535.0f;
    else { float st = m->p[SP_START], len = m->p[SP_LENGTH] * (1.0f - st); *frac = st + len * (float)k / (float)n; }
    return n;
}
static int overview(const pw_smp *d, uint8_t *peaks, unsigned n, uint32_t *frames_out);
int pw_sampler_overview(unsigned slot, uint8_t *peaks, unsigned n, uint32_t *frames_out)
{
    pw_smp d;
    int have = 0;
    if (slot < PW_MAX_MODULES && pws.mod[slot].used == 1 && pws.mod[slot].type == PW_T_SAMPLER) {
        const samp_st *ss = (const samp_st *)(const void *)pws.mod[slot].st.raw;
        have = samp_source(&pws.mod[slot], ss, &d);
        if (ss->rec_req == 2 && ss->rec_pos > 64 && (pws.mod[slot].p[SP_SAMPLE] < 0.5f && pws.mod[slot].p[SP_BANK] < 0.5f)) {   /* (recording: the picture grows with it) */
            pw_smp_ram(&d, (const int16_t *)PW_POOL16(&pws.mod[slot]), (uint32_t)ss->rec_pos, 44100); have = 1;
        }
    }
    return overview(have ? &d : 0, peaks, n, frames_out);
}
int pw_sample_overview(unsigned bank, uint8_t *peaks, unsigned n, uint32_t *frames_out)
{
    pw_smp d;
    return overview(pw_smp_open(bank, &d) ? &d : 0, peaks, n, frames_out);
}
static int overview(const pw_smp *d, uint8_t *peaks, unsigned n, uint32_t *frames_out)
{
    if (frames_out) *frames_out = d ? d->frames : 0;
    if (!d || !d->frames || !n) { for (unsigned i = 0; i < n; ++i) peaks[i] = 0; return 0; }
    pw_smp_peaks(d, peaks, n, 0);
    return 1;
}

/* ================================================================== MIDI (the G2's MIDI modules, in two) */
/* MIDI IN: four CCs (any controller's knobs) as voltages, 0..5 V, from one channel or all; CLOCK: the transport's
 * sixteenths (a MIDI clock coming in drives them), RUN high while it plays. Notes come in through KEYS. */
static const char *const L_mch[] = {"ALL", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"};
typedef struct { float v[4]; uint16_t clk; } midiin_st;
PROC_COLD(midiin_proc)
{
    ST(midiin_st);
    (void)in; (void)connected;
    int ch = (int)(m->p[0] + 0.5f);
    float target[4];
    for (int k = 0; k < 4; ++k) {
        int cc = (int)(m->p[1 + k] + 0.5f) & 127;
        if (!ch || pws.cc_ch[cc] == ch - 1) target[k] = (float)pws.cc[cc] * (5.0f / 127.0f);
        else target[k] = s->v[k];
    }
    for (int i = 0; i < PW_N; ++i) {
        for (int k = 0; k < 4; ++k) { s->v[k] += (target[k] - s->v[k]) * 0.004f; out[k][i] = s->v[k]; }
        if (pws.tick_at[i]) s->clk = 220;
        out[4][i] = s->clk ? 5.0f : 0.0f;
        out[5][i] = pws.playing ? 5.0f : 0.0f;
        if (s->clk) --s->clk;
    }
}
/* MIDI OUT: plays other gear. GATE's rising edge sends a note (PITCH, 1 V an octave from C4 = 60; VEL 0..5 V, 100
 * when unpatched), its fall the note off; CC A and CC B patched send those controllers (0..5 V: 0..127) as they
 * change; CLOCK on sends MIDI clock, start and stop with the transport. Over USB. */
typedef struct { uint8_t gate_hi, note, playing, clk_count, last_cc[2]; int32_t cc_wait, clk_next; float clk_step; } midiout_st;
static void midiout_init(pw_module *m) { ST(midiout_st); s->last_cc[0] = s->last_cc[1] = 255; s->note = 255; }
PROC_COLD(midiout_proc)
{
    ST(midiout_st);
    (void)out;
    uint8_t ch = (uint8_t)(((int)(m->p[0] + 0.5f) - 1) & 15);
    int clock = m->p[3] > 0.5f && !pws.clk_out;   /* (the MIDI CLOCK OUT setting sends it already) */
    if (clock) {
        if (pws.reset_now) pw_plat_midi_out(0xFA, 0, 0);
        if (s->playing && !pws.playing) pw_plat_midi_out(0xFC, 0, 0);
        s->clk_step = PW_SRF * 60.0f / (pws.bpm * 24.0f);
    }
    s->playing = pws.playing;
    for (int i = 0; i < PW_N; ++i) {
        uint8_t was = s->gate_hi;
        if (pw_rise(&s->gate_hi, in[1][i])) {
            if (s->note != 255) pw_plat_midi_out((uint8_t)(0x80 | ch), s->note, 0);
            int n = 60 + pw_floori(in[0][i] * 12.0f + 0.5f);
            n = n < 0 ? 0 : n > 127 ? 127 : n;
            int v = IN_CONNECTED(2) ? (int)(pw_clamp(in[2][i] * 0.2f, 0.0f, 1.0f) * 126.0f + 1.0f) : 100;
            pw_plat_midi_out((uint8_t)(0x90 | ch), (uint8_t)n, (uint8_t)v);
            s->note = (uint8_t)n;
        } else if (was && !s->gate_hi && s->note != 255) { pw_plat_midi_out((uint8_t)(0x80 | ch), s->note, 0); s->note = 255; }
        if (clock && pws.playing) {
            if (pws.tick_at[i]) s->clk_next = 0;          /* the sixteenth: six clocks from here */
            if (s->clk_next >= 0 && s->clk_next-- == 0) {
                pw_plat_midi_out(0xF8, 0, 0);
                s->clk_count = (uint8_t)(s->clk_count + 1u);
                s->clk_next = s->clk_count % 6u ? (int32_t)s->clk_step : -1;
            }
        }
    }
    if (--s->cc_wait <= 0) {                            /* the controllers: at most every 3 ms, when they change */
        s->cc_wait = 1;
        for (int k = 0; k < 2; ++k) {
            if (!IN_CONNECTED(3 + k)) continue;
            uint8_t v = (uint8_t)(pw_clamp(in[3 + k][PW_N - 1] * 0.2f, 0.0f, 1.0f) * 127.0f + 0.5f);
            if (v != s->last_cc[k]) { s->last_cc[k] = v; pw_plat_midi_out((uint8_t)(0xB0 | ch), (uint8_t)((int)(m->p[1 + k] + 0.5f) & 127), v); }
        }
    }
}

/* knob order (display only; the stored parameter numbers stay): an effect's MIX is knob 4 of its first page */
static const uint8_t ht_order[6] = {0, 1, 2, 5, 3, 4};         /* RATE DEPTH SPLIT MIX | SHAPE SYNC */
static const uint8_t tape_order[8] = {0, 1, 3, 6, 2, 4, 5, 7}; /* DRIVE WOW AGE MIX | FLUTTER HISS STOP SPEED */
static const uint8_t fm4_order[8] = {7, 0, 1, 2, 4, 5, 6, 3};   /* PRESET RATIO INDEX FBK | AMP MOD TIME TUNE */
static const uint8_t addtv_order[9] = {8, 1, 2, 3, 4, 5, 6, 7, 0};
static const char *const L_add_pages[] = {"MAIN", "SPECTRUM", "TUNE"};
static const uint8_t mac_order[9] = {8, 0, 2, 3, 4, 5, 6, 1, 7};
static const char *const L_mac_pages[] = {"MAIN", "SHAPE", "FINE"};
static const uint8_t res_order[10] = {7, 4, 1, 2, 3, 5, 6, 0, 8, 9};
static const char *const L_xfcurve[] = {"EQ POWER", "LINEAR"};
static const char *const L_fm4_pages[] = {"TONE", "ENVELOPE"};
static const char *const L_resmodel[] = {"STRING", "BELL", "BAR", "PLATE", "DRUM", "TUBE", "WOOD", "SHEET", "SPRING", "PIPE", "GONG", "TIMPANI"};
static const char *const L_resexc[] = {"NOISE", "CLICK", "MALLET"};
static const char *const L_cvrmode[] = {"PLAY", "REC", "DUB"};
static const char *const L_cvrspeed[] = {"1/2", "1", "2", "REV"};
static const char *const L_cvr_pages[] = {"RECORD", "PLAY"};
static const char *const L_pmod_pages[] = {"CV 1", "CV 2", "CV 3", "CV 4"};
static const char *const L_zoom[] = {"5MS", "11MS", "22MS", "44MS"};
static const char *const L_vdiv[] = {"0.5V", "1V", "2.5V"};
static const char *const L_strig[] = {"A", "B", "FREE"};
static const char *const L_scopemode[] = {"WAVE", "SPECTRUM"};
static const char *const L_gran_pages[] = {"GRAINS", "SPACE", "SOURCE"};
static const char *const L_rack_pages[] = {"KIT", "PAD 1", "PAD 2", "PAD 3", "PAD 4", "PAD 5", "PAD 6", "PAD 7", "PAD 8", "KEYS"};
/* the knob pages: KIT, VOLUME, CHOKE, ACCENT; a page a pad; KEYS last */
static const uint8_t rack_order[37] = {36, 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
                                       24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 2};
static const char *const L_sampler_pages[] = {"SAMPLE", "REGION", "WARP", "SLICE", "AMP", "FILTER", "SYNC"};
static const char *const L_sbank[] = {"INTERNAL", "A", "B", "C", "D"};
/* the knob pages: BANK SAMPLE TUNE LEVEL first; REGION to FILTER as they were; MODE joins BEATS on the last */
static const uint8_t sampler_order[26] = {25, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 1};
static const char *const L_bipolar[] = {"BI", "UNI"};
static const char *const L_vca[] = {"LIN", "EXP"};
static const char *const L_gdist[] = {"LINEAR", "CAUCHY", "LOGIST", "ARCSIN", "EXPON"};
static const char *const L_grange[] = {"AUDIO", "LOW", "CV"};
static const char *const L_tables[] = {"BASIC", "VOCAL", "DIGITAL", "FM"};
static const char *const L_fold[] = {"FOLD", "SINE", "CHEBY", "259"};
static const char *const L_trem[] = {"SINE", "TRI"};
static const char *const L_dist[] = {"SOFT", "HARD", "FUZZ", "CRUSH"};
static const char *const L_env[] = {"ADSR", "AD", "AR"};
static const char *const L_onoff[] = {"OFF", "ON"};
static const char *const L_rect[] = {"OFF", "HALF", "FULL"};
static const char *const L_cmpmode[] = {"A>B", "A<B", "WINDOW", "CROSS"};
static const char *const L_envtrig[] = {"SOFT", "HARD", "LEGATO"};
static const char *const L_envrange[] = {"FAST", "NORMAL", "SLOW"};
static const char *const L_mrange[] = {"0-5V", "+-5V", "0-2.5V"};
static const char *const L_dir[] = {"FWD", "BWD", "PEND", "RND"};
static const char *const L_clock[] = {"GLOBAL", "INT"};
static const char *const L_lpg[] = {"VCA", "LP", "BOTH"};
static const char *const L_shim[] = {"OCT UP", "5TH UP", "4TH UP", "OCT DOWN", "2 OCT UP"};
static const char *const L_rvmode[] = {"ROOM", "HALL", "PLATE", "GATED", "SWELL"};
static const char *const L_lpgslope[] = {"6 DB", "12 DB"};
static const char *const L_filt[] = {"SVF 12", "LADDER", "DIODE", "OTA 24", "OTA 12", "SALLEN"};
static const char *const L_slice[] = {"1/2", "1/4", "1/8", "1/16", "1/32", "1/64"};
static const char *const L_coin[] = {"GATE", "LATCH"};

#define P(n, k, u, lo, hi, d) {n, k, u, lo, hi, d, 0}
#define PE(n, labels, count, d) {n, PW_ENUM, PW_U_NONE, 0, (float)((count) - 1), d, labels}

const char *const pw_cat_names[PW_C_COUNT] = {"SOURCE", "FILTER/AMP", "EFFECT", "MODULATOR", "UTILITY", "SEQUENCER", "IN/OUT"};

/* ------------------------------------------------------- GATE DELAY, ADDITIVE, MACRO OSC */
/* GATE DELAY (after o_C Hemisphere's, itself after the Ladik S-189): two gates or triggers, each later by its TIME
 * (0-2 s), lengths kept; CV A/B add +-1 s at 5 V (200 ms a volt), taken as each edge arrives. B unpatched: a second
 * tap of A. The edges wait in a queue of when each must come out. */
#define GD_Q 24
typedef struct { uint32_t now, due[2][GD_Q]; uint8_t head[2], n[2], in_hi[2], out_hi[2]; } gd_st;
PROC_COLD(gatedly_proc)
{
    ST(gd_st);
    for (int i = 0; i < PW_N; ++i) {
        uint32_t now = ++s->now;
        for (int c = 0; c < 2; ++c) {
            const float *x = (c == 1 && !IN_CONNECTED(1)) ? in[0] : in[c];
            uint8_t hi = s->in_hi[c];
            int edge = hi ? x[i] < 0.5f : x[i] > 1.0f;
            if (edge) {
                s->in_hi[c] = !hi;
                float base = sync_sec(m->p[2 + c]); if (base <= 0) base = m->p[c];
                float t = pw_clamp(base + (IN_CONNECTED(2 + c) ? in[2 + c][i] * 0.2f : 0.0f), 0.0f, 2.0f);
                uint32_t due = now + (uint32_t)(t * PW_SRF);
                if (s->n[c]) {                       /* never before the edge ahead of it */
                    uint32_t last = s->due[c][(s->head[c] + s->n[c] - 1u) % GD_Q];
                    if ((int32_t)(due - last) <= 0) due = last + 1u;
                }
                if (s->n[c] < GD_Q) { s->due[c][(s->head[c] + s->n[c]) % GD_Q] = due; ++s->n[c]; }
            }
            while (s->n[c] && (int32_t)(now - s->due[c][s->head[c]]) >= 0) {
                s->out_hi[c] = !s->out_hi[c]; s->head[c] = (uint8_t)((s->head[c] + 1u) % GD_Q); --s->n[c];
            }
            out[c][i] = s->out_hi[c] ? 5.0f : 0.0f;
        }
    }
}

/* ADDITIVE (after NI's Razor): 32 sine partials whose loudness comes from a few spectral controls instead of a
 * filter. SHAPE morphs the spectrum (sine, triangle, saw, square, buzz); BRIGHT a steep low-pass that tracks
 * nothing (Hz); PEAK and RES a resonant bump; COMB notches swept through the partials (a phaser made of
 * partials); STRETCH pulls them apart (bells) or together; PLUCK: each GATE opens BRIGHT four octaves, falling
 * back in PLUCK's time. Odd partials lean left, even right. Amplitudes are worked out once a block and glided. */
#define ADD_N 32
PW_INLINE float pw_sqrtf_(float x) { return x > 1e-12f ? pw_exp2(0.5f * pw_log2(x)) : 0.0f; }
typedef struct { float ph[ADD_N], amp[ADD_N]; float env; uint8_t hi; } add_st;
static float add_shape(float sh, int n)
{
    float fn = (float)n, odd = (n & 1) ? 1.0f : 0.0f, a, b, t;
    float x = pw_clamp(sh, 0.0f, 1.0f) * 4.0f; int seg = (int)x; if (seg > 3) seg = 3; t = x - (float)seg;
    float tab[5];
    tab[0] = n == 1 ? 1.0f : 0.0f;                                   /* sine */
    tab[1] = odd / (fn * fn);                                        /* triangle */
    tab[2] = 1.0f / fn;                                              /* saw */
    tab[3] = odd / fn;                                               /* square */
    tab[4] = 0.35f / pw_maxf(1.0f, fn * 0.35f + 0.65f);              /* buzz: nearly flat */
    a = tab[seg]; b = tab[seg + 1];
    return a + (b - a) * t;
}
PROC(additive_proc)
{
    ST(add_st);
    preset_cv(m, in[5], connected, 5);
    float f0 = pw_pitch_hz(m->p[0] / 12.0f + in[0][0]);
    if (IN_CONNECTED(1)) for (int i = 0; i < PW_N; ++i) if (pw_rise(&s->hi, in[1][i])) s->env = 1.0f;
    float pluck = m->p[3], kp = pluck > 0.001f ? pw_coef(0.03f * pw_exp2(pluck * 7.0f) / 4.6f) : 0.0f;
    float bright = pw_exp2(pw_clamp(m->p[2] * 8.0f + in[2][0] * 0.8f + s->env * 4.0f * (pluck > 0.001f), 0.0f, 9.0f)) * 100.0f;
    s->env *= kp > 0 ? pw_exp2(pw_log2(kp) * (float)PW_N) : 0.0f;
    float peak = 100.0f * pw_exp2(pw_clamp(m->p[4] + in[3][0] * 0.16f, 0.0f, 1.0f) * 6.3f), res = m->p[5];
    float comb = pw_clamp(m->p[6] + in[4][0] * 0.2f, 0.0f, 1.0f), cd = 0.04f + comb * 0.46f, cdepth = pw_minf(1.0f, comb * 5.0f);
    float stretch = m->p[7], width = 0.75f - 0.6f * res;
    float amp[ADD_N], inc[ADD_N], sum2 = 0;
    for (int k = 0; k < ADD_N; ++k) {
        int n = k + 1;
        float f = f0 * (float)n * pw_maxf(0.1f, 1.0f + stretch * 0.035f * (float)(n - 1));
        inc[k] = f * PW_INV_SR;
        float a = 0;
        if (f < 19000.0f && inc[k] < 0.45f) {
            a = add_shape(m->p[1], n);
            float r = f / bright, r2 = r * r; a /= 1.0f + r2 * r2;                       /* BRIGHT */
            float d = pw_log2(f / peak) / width; a *= 1.0f + res * 10.0f * pw_exp2(-1.4427f * d * d);   /* PEAK, RES */
            float c = pw_cos1((float)n * cd * 0.5f); a *= 1.0f - cdepth * (1.0f - c * c);  /* COMB */
            if (inc[k] > 0.35f) a *= (0.45f - inc[k]) * 10.0f;                       /* fade near the top */
        }
        amp[k] = a; sum2 += a * a;
    }
    float norm = 2.6f / pw_maxf(0.35f, pw_sqrtf_(sum2));
    for (int i = 0; i < PW_N; ++i) out[0][i] = out[1][i] = 0;
    for (int k = 0; k < ADD_N; ++k) {
        float a0 = s->amp[k], a1 = amp[k] * norm, da = (a1 - a0) * (1.0f / PW_N), ph = s->ph[k], di = inc[k];
        s->amp[k] = a1;
        if (a0 == 0.0f && a1 == 0.0f) { s->ph[k] = pw_frac(ph + di * PW_N); continue; }
        /* a rotating phasor (exact enough over one block), from the kept phase */
        float pl = (k & 1) ? 0.7f : 1.3f, pr = 2.0f - pl, a = a0;
        float c = pw_cos1(ph), sn = pw_sin1(ph), rc = pw_cos1(di), rs = pw_sin1(di);
        for (int i = 0; i < PW_N; ++i) {
            float y = sn * a;
            out[0][i] += y * pl; out[1][i] += y * pr;
            float c2 = c * rc - sn * rs; sn = sn * rc + c * rs; c = c2; a += da;
        }
        s->ph[k] = pw_frac(ph + di * (float)PW_N);
    }
}

/* MACRO OSC: one oscillator, eight ways of making sound (after Mutable Instruments' Plaits); HARMONICS, TIMBRE
 * and MORPH mean something in each, AUX a variation. TRIG patched: an internal low pass gate struck each time
 * (DECAY, COLOR: VCA only to fully filtered); else it drones.
 *   VA        two oscillators: HARM detunes the second (0-7 semitones), TIMBRE saw to square, MORPH hard sync
 *   FOLD      sine to triangle (MORPH) through a folder (TIMBRE), HARM its bias
 *   FM        two operators: HARM the ratio (1/2 .. 7), TIMBRE the index, MORPH feedback
 *   FORMANT   pulses of two formants (HARM, TIMBRE: their pitch above the note), MORPH their width
 *   CHORD     four saws: HARM the chord (octave, fifth, sus4, minor, major, m7, maj7, dim), TIMBRE the voicing, MORPH saw to square
 *   SWARM     seven detuned saws: HARM the spread, TIMBRE the side voices' level, MORPH drift
 *   NOISE     noise through a resonant filter at the note: HARM low to band to high pass, TIMBRE resonance, MORPH clocked
 *   PARTICLE  random impulses (HARM: how many) ringing two resonators (TIMBRE: their interval), MORPH the ring */
static const float macro_gain[8] PW_DSP_DATA = {1.2f, 0.55f, 0.55f, 0.85f, 2.0f, 0.9f, 1.8f, 0.8f};
static const char *const L_macro[] = {"VA", "FOLD", "FM", "FORMANT", "CHORD", "SWARM", "NOISE", "PARTICLE"};
static const float macro_ratio[9] PW_DSP_DATA = {0.5f, 1, 1.5f, 2, 3, 4, 5, 7, 7};
static const int8_t macro_chord[9][4] PW_DSP_DATA = {{0, 12, 24, 12}, {0, 7, 12, 19}, {0, 5, 7, 12}, {0, 3, 7, 12}, {0, 4, 7, 12},
                                                      {0, 3, 7, 10}, {0, 4, 7, 11}, {0, 3, 6, 9}, {0, 3, 6, 9}};
typedef struct {
    float ph[8], fb, sub, f1, f2, z1, z2, r1[2], r2[2], env, lp, hold, hcount, drift[7];
    uint32_t seed; uint8_t hi;
} macro_st;
PW_INLINE float mo_saw(float ph, float dt) { return 2.0f * ph - 1.0f - pw_blep(ph, dt); }
PW_INLINE float mo_sq(float ph, float dt, float pw) { return (ph < pw ? 1.0f : -1.0f) + pw_blep(ph, dt) - pw_blep(pw_frac(ph - pw + 1.0f), dt); }
PW_INLINE float mo_adv(float *ph, float dt) { float p = *ph + dt; if (p >= 1.0f) p -= 1.0f; *ph = p; return p; }
PROC_COLD(macro_proc)
{
    ST(macro_st);
    preset_cv(m, in[6], connected, 6);
    int model = (int)(m->p[0] + 0.5f) + (IN_CONNECTED(5) ? pw_floori(in[5][0] * 1.6f + 0.5f) : 0);
    model = model < 0 ? 0 : model > 7 ? 7 : model;
    float tune = m->p[1] / 12.0f + m->p[7] / 1200.0f;
    float kd = pw_coef(m->p[5] / 4.6f), color = m->p[6];
    int lpg = IN_CONNECTED(1);
    if (!s->seed) s->seed = 0x51ED27u + (uint32_t)(m - pws.mod) * 2654435761u;
    for (int i = 0; i < PW_N; ++i) {
        float f = pw_clamp(pw_pitch_hz(tune + in[0][i]), 1.0f, 18000.0f), dt = f * PW_INV_SR;
        float h = pw_clamp(m->p[2] + in[2][i] * 0.2f, 0.0f, 1.0f), t = pw_clamp(m->p[3] + in[3][i] * 0.2f, 0.0f, 1.0f);
        float mo = pw_clamp(m->p[4] + in[4][i] * 0.2f, 0.0f, 1.0f);
        float y = 0, aux = 0;
        switch (model) {
        case 0: {                                     /* VA */
            float dt2 = dt * pw_exp2(h * 7.0f / 12.0f), ds = dt * (1.0f + mo * 3.0f);
            float p0 = mo_adv(&s->ph[0], dt);
            if (mo > 0.01f) { float pp = s->ph[1] + ds; if (p0 < dt) pp = p0 * ds / dt; else if (pp >= 1.0f) pp -= 1.0f; s->ph[1] = pp; }
            else mo_adv(&s->ph[1], ds);
            float a = mo_saw(p0, dt), b = mo > 0.01f ? 2.0f * s->ph[1] - 1.0f : mo_saw(s->ph[1], ds);
            float p2 = mo_adv(&s->ph[2], dt2), sq = mo_sq(p2, dt2, 0.5f - 0.4f * t), sw = mo_saw(p2, dt2);
            y = 0.5f * (mo > 0.01f ? b : a) + 0.5f * (sw + (sq - sw) * t);
            float ps = mo_adv(&s->ph[3], dt * 0.5f); aux = mo_sq(ps, dt * 0.5f, 0.5f);
        } break;
        case 1: {                                     /* FOLD */
            float p = mo_adv(&s->ph[0], dt), sn = pw_sin1(p);
            float tr = p < 0.25f ? 4.0f * p : p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f;
            float x = (sn + (tr - sn) * mo) * (1.0f + t * 7.0f) + (h - 0.5f) * 1.5f;
            y = pw_sin1(x * 0.25f); aux = pw_sin1(mo_adv(&s->ph[1], dt * 0.5f));
        } break;
        case 2: {                                     /* FM */
            float rx = h * 8.0f; int ri = (int)rx; float r = macro_ratio[ri] + (macro_ratio[ri + 1] - macro_ratio[ri]) * (rx - (float)ri);
            float pm = mo_adv(&s->ph[1], dt * r), md = pw_sin1(pm + s->fb * mo * 0.25f);
            s->fb = md;
            float pc = mo_adv(&s->ph[0], dt);
            y = pw_sin1(pc + md * t * t * 1.3f); aux = md;
        } break;
        case 3: {                                     /* FORMANT: windowed sines restarted each period */
            float p = mo_adv(&s->ph[0], dt);
            if (p < dt) { s->ph[1] = 0; s->ph[2] = 0; }
            float f1 = pw_minf(f * pw_exp2(h * 4.0f + 0.5f), 9000.0f), f2 = pw_minf(f * pw_exp2(t * 5.0f + 1.0f), 12000.0f);
            float w = pw_exp2(-p * (2.0f + (1.0f - mo) * 14.0f));
            float a = pw_sin1(mo_adv(&s->ph[1], f1 * PW_INV_SR)), b = pw_sin1(mo_adv(&s->ph[2], f2 * PW_INV_SR));
            y = (a * 0.7f + b * 0.5f) * w * 1.4f; aux = a * w;
        } break;
        case 4: {                                     /* CHORD */
            float cx = h * 8.0f; int ci = (int)cx; if (ci > 7) ci = 7;
            int inv = (int)(t * 3.99f);
            for (int v = 0; v < 4; ++v) {
                float semis = (float)macro_chord[ci][v] + (v < inv ? 12.0f : 0.0f);
                float d = dt * pw_exp2(semis / 12.0f), p = mo_adv(&s->ph[v], d);
                float sw = mo_saw(p, d), sq = mo_sq(p, d, 0.5f), o = sw + (sq - sw) * mo;
                y += o * 0.32f; if (v == 0) aux = o;
            }
        } break;
        case 5: {                                     /* SWARM */
            static const float det[7] PW_DSP_DATA = {0, -0.11f, 0.11f, -0.06f, 0.06f, -0.02f, 0.02f};
            if ((i & 31) == 0) for (int v = 0; v < 7; ++v) s->drift[v] += (pw_rand_f(&s->seed) * 0.02f - s->drift[v] * 0.01f) * mo;
            for (int v = 0; v < 7; ++v) {
                float d = dt * (1.0f + det[v] * h * 0.5f + s->drift[v] * 0.02f), o = mo_saw(mo_adv(&s->ph[v], d), d);
                y += v ? o * t * 0.3f : o * 0.6f; if (!v) aux = o;
            }
        } break;
        case 6: {                                     /* NOISE: a state-variable filter at the note */
            float n = pw_rand_f(&s->seed);
            if (mo > 0.01f) { s->hcount += f * PW_INV_SR * (1.0f + (1.0f - mo) * 30.0f); if (s->hcount >= 1.0f) { s->hcount -= 1.0f; s->hold = n; } n = s->hold; }
            float g = pw_tan_pi(pw_minf(f, 16000.0f) * PW_INV_SR), k = 2.0f - 1.95f * t;
            float a1 = 1.0f / (1.0f + g * (g + k)), v3 = n - s->z2, v1 = a1 * s->z1 + g * a1 * v3, v2 = s->z2 + g * v1;
            s->z1 = 2.0f * v1 - s->z1; s->z2 = 2.0f * v2 - s->z2;
            float lp = v2, bp = v1, hp = n - k * v1 - v2;
            y = h < 0.5f ? lp + (bp - lp) * h * 2.0f : bp + (hp - bp) * (h - 0.5f) * 2.0f;
            y *= 0.6f + t; aux = n;
        } break;
        default: {                                    /* PARTICLE: dust ringing two resonators */
            float dens = pw_exp2(h * 9.0f) * 2.0f;      /* 2 .. 1000 a second */
            float u = pw_rand_f(&s->seed) * 0.5f + 0.5f, imp = u < dens * PW_INV_SR ? pw_rand_f(&s->seed) * 4.0f : 0.0f;
            float r = 0.9990f + 0.0009f * mo, f2 = f * pw_exp2(t * 2.0f + 0.2f);
            for (int q = 0; q < 2; ++q) {
                float fr = q ? f2 : f, c = 2.0f * r * pw_cos1(pw_minf(fr, 15000.0f) * PW_INV_SR);
                float *z = q ? s->r2 : s->r1, o = imp * (1.0f - r) * 30.0f + c * z[0] - r * r * z[1];
                z[1] = z[0]; z[0] = o; y += o;
            }
            y = pw_tanh(y * 0.8f); aux = imp * 0.25f;
        } break;
        }
        if (lpg) {                                    /* the low pass gate */
            if (pw_rise(&s->hi, in[1][i])) s->env = 1.0f;
            s->env *= kd;
            float fc = 60.0f * pw_exp2(9.0f * (s->env * color + (1.0f - color))), g = pw_minf(1.0f, fc * PW_TWO_PI * PW_INV_SR);
            s->lp += (y - s->lp) * g;
            y = s->lp * s->env; aux *= s->env;
        }
        out[0][i] = 4.0f * macro_gain[model] * y; out[1][i] = 4.0f * aux;
    }
    if (pw_absf(s->z1) < 1e-20f) s->z1 = 0;
    if (pw_absf(s->z2) < 1e-20f) s->z2 = 0;
}

/* -------------------------------------------------------- CLOCK WARP, MICRO PERC, SHUFFLER */
/* CLOCK WARP: bends time. Over every LENGTH steps of the clock in (unpatched: the transport's sixteenths) it plays
 * COUNT pulses, spread by a curve: WARP 0 even, + rushing (each gap shorter than the last), - dragging; SWING delays
 * every other pulse; DRIFT lets each pulse wander early or late, slowly (a drunk drummer). COUNT against LENGTH makes
 * polyrhythms (5 over 7: COUNT 5, LENGTH 7). OUT: the pulses (GATE long); RAMP: where the cycle is, 0-5 V; EOC: the
 * cycle's start. The WARP and COUNT inputs move them (5 V: WARP full, COUNT +16), taken at each cycle's start. */
typedef struct { uint32_t since, period, t, clen; float drift, dtarget; uint32_t seed; int16_t step, k, count, len;
                 float warp; uint8_t clk_hi, rst_hi, gate, nat_ok; uint16_t eoc; uint32_t off_at; float nat; } cw_st;   /* (nat: the time of the next pulse, in samples, worked out once per pulse) */
static float cw_at(const cw_st *s, const float *p, int k)    /* pulse k's time in the cycle, 0..1 */
{
    float x = (float)k / (float)s->count, e = pw_exp2(-s->warp * 1.6f);
    float y = x <= 0 ? 0 : pw_exp2(e * pw_log2(x));                     /* x^e */
    if (k & 1) y += p[3] / (float)s->count;                              /* swing */
    y += s->drift * p[4] * 0.5f / (float)s->count;
    return pw_clamp(y, 0.0f, 0.999f);
}
/* one sample of it: p = LENGTH, COUNT, WARP, SWING, DRIFT, GATE; clock: a step of it now; reset: start over;
 * cvw, cvc: the WARP and COUNT inputs (taken at each cycle's start) */
static PW_DSP void cw_sample(cw_st *s, const float *p, unsigned seed_from, int clock, int reset, int has_w, float cvw, int has_c, float cvc,
                             float *o0, float *o1, float *o2)
{
    if (!s->seed) s->seed = 0xC10C4u + seed_from * 2654435761u;
    if (reset) { s->step = -1; s->t = 0; s->k = 0; s->clen = 0; s->nat_ok = 0; }
    s->since++; s->t++;
    if (clock) {
        if (s->since > 8) s->period = s->since;
        s->since = 0;
        int len = (int)(p[0] + 0.5f); if (len < 1) len = 1;
        if (++s->step >= s->len || s->step < 0) {                         /* a new cycle */
            s->step = 0; s->len = (int16_t)len;
            int c = (int)(p[1] + 0.5f) + (has_c ? pw_floori(cvc * 3.2f + 0.5f) : 0);
            s->count = (int16_t)(c < 1 ? 1 : c > 64 ? 64 : c);
            s->warp = pw_clamp(p[2] + (has_w ? cvw * 0.2f : 0.0f), -1.0f, 1.0f);
            s->clen = (s->period ? s->period : 5512u) * (uint32_t)len;
            s->t = 0; s->k = 0; s->eoc = 88; s->nat_ok = 0;
        }
    }
    if (s->clen && s->k < s->count && !s->nat_ok) { s->nat = cw_at(s, p, s->k) * (float)s->clen; s->nat_ok = 1; }
    if (s->clen && s->k < s->count && (float)s->t >= s->nat) {
        if ((s->k & 3) == 0) s->dtarget = pw_rand_f(&s->seed);           /* drift wanders */
        s->drift += (s->dtarget - s->drift) * 0.5f;
        s->gate = 1; s->off_at = s->t + (uint32_t)(p[5] * (float)s->clen / (float)s->count) + 30u;
        s->k++; s->nat_ok = 0;
    }
    if (s->gate && s->t >= s->off_at) s->gate = 0;
    *o0 = s->gate ? 5.0f : 0.0f;
    *o1 = s->clen ? 5.0f * pw_minf(1.0f, (float)s->t / (float)s->clen) : 0.0f;
    *o2 = s->eoc ? (s->eoc--, 5.0f) : 0.0f;
}
/* CLOCK: the tempo, divided, multiplied, swung (CLK DIV MULT RST); and the same ticks bent in time (WARP RAMP EOC):
 * over every LENGTH ticks it plays COUNT pulses, spread by a curve: WARP 0 even, + rushing (each gap shorter than
 * the last), - dragging; its own SWING delays every other pulse; DRIFT lets each wander early or late (a drunk
 * drummer); COUNT against LENGTH makes polyrhythms (5 over 7). WARP: the pulses (GATE long); RAMP: where the cycle
 * is, 0-5 V; EOC: the cycle's start. The WARP and COUNT inputs move them (5 V: WARP full, COUNT +16). */
typedef struct {
    float ph, period, since, clk_left, div_left, mult_left, mult_next, swing_wait, rst_left;
    uint32_t count; int mult_k; uint8_t ext_hi, rst_hi;
} clk_st;
typedef struct { clk_st c; cw_st w; } clk2_st;   /* (the clock's first, so what reads clk_st still works) */
static PW_DSP void clock_fire(clk_st *s, int div, int mult)
{
    s->clk_left = s->period * 0.5f;
    if (div < 1) div = 1;
    if (s->count % (uint32_t)div == 0) s->div_left = s->period * (float)div * 0.5f;
    if (mult < 1) mult = 1;
    s->mult_k = mult - 1;
    s->mult_next = s->period / (float)mult;
    s->mult_left = s->period / (float)(2 * mult);
    ++s->count;
}
PROC_COLD(clock_proc)
{
    ST(clk2_st);
    clk_st *c = &s->c;
    int global = m->p[0] < 0.5f;
    if (global) m->p[1] = pws.bpm;
    float tempo = pw_clamp(m->p[1], 20.0f, 400.0f), swing = m->p[4];
    int div = (int)(m->p[2] + 0.5f), mult = (int)(m->p[3] + 0.5f);
    if (!IN_CONNECTED(0)) c->period = PW_SRF * 15.0f / tempo;
    int reset0 = 0;
    if (global && pws.reset_now && !IN_CONNECTED(0)) { c->count = 0; c->rst_left = 88; c->swing_wait = 0; reset0 = 1; }
    for (int i = 0; i < PW_N; ++i) {
        int reset = reset0 && i == 0;
        if (IN_CONNECTED(1) && pw_rise(&c->rst_hi, in[1][i])) { c->count = 0; c->rst_left = 88; reset = 1; }
        int tick = 0;
        c->since += 1.0f;
        if (IN_CONNECTED(0)) {
            tick = pw_rise(&c->ext_hi, in[0][i]);
            if (tick) { if (c->since > 32.0f && c->since < PW_SRF * 4.0f) c->period = c->since; c->since = 0; }
        } else if (global) {
            tick = pws.tick_at[i];
        } else {
            c->ph += tempo * (4.0f / 60.0f) * PW_INV_SR;
            if (c->ph >= 1.0f) { c->ph -= 1.0f; tick = 1; }
        }
        float w0, w1, w2;   /* the bent clock runs on the same ticks, before the swing */
        cw_sample(&s->w, m->p + 5, (unsigned)(m - pws.mod), tick, reset, IN_CONNECTED(2), in[2][i], IN_CONNECTED(3), in[3][i], &w0, &w1, &w2);
        if (tick) {
            if (!IN_CONNECTED(0) && swing > 0.001f && (c->count & 1U)) c->swing_wait = swing * c->period;
            else clock_fire(c, div, mult);
        }
        if (c->swing_wait > 0) { c->swing_wait -= 1.0f; if (c->swing_wait <= 0) clock_fire(c, div, mult); }
        if (c->mult_k > 0) {
            c->mult_next -= 1.0f;
            if (c->mult_next <= 0) { c->mult_left = c->period / (float)(2 * (mult < 1 ? 1 : mult)); c->mult_next = c->period / (float)(mult < 1 ? 1 : mult); --c->mult_k; }
        }
        out[0][i] = c->clk_left > 0 ? 5.0f : 0.0f;
        out[1][i] = c->div_left > 0 ? 5.0f : 0.0f;
        out[2][i] = c->mult_left > 0 ? 5.0f : 0.0f;
        out[3][i] = c->rst_left > 0 ? 5.0f : 0.0f;
        out[4][i] = w0; out[5][i] = w1; out[6][i] = w2;
        if (c->clk_left > 0) c->clk_left -= 1.0f;
        if (c->div_left > 0) c->div_left -= 1.0f;
        if (c->mult_left > 0) c->mult_left -= 1.0f;
        if (c->rst_left > 0) c->rst_left -= 1.0f;
    }
}

/* MICRO PERC: the small sounds between the drums: CLICK (a struck resonance), ZAP (a falling sine), BLIP (a short
 * FM beep), TICK (a filtered noise flick), METAL (two squares ring-modulated) and CRACKLE (dust ringing at the
 * pitch). TONE is each model's brightness or depth; SCATTER makes every hit a little different (pitch, length,
 * tone), the amount it moved out at RND. */
static const char *const L_micro[] = {"CLICK", "ZAP", "BLIP", "TICK", "METAL", "CRACKLE"};
typedef struct { float env, kd, f, tone, ph, ph2, pe, z1, z2, rnd, hp; uint32_t seed; uint8_t hi, on; } mp_st;
PROC(microperc_proc)
{
    ST(mp_st);
    if (!s->seed) s->seed = 0x7E1C5u + (uint32_t)(m - pws.mod) * 2654435761u;
    int model = (int)(m->p[0] + 0.5f);
    for (int i = 0; i < PW_N; ++i) {
        if (IN_CONNECTED(0) && pw_rise(&s->hi, in[0][i])) {
            float sc = pw_clamp(m->p[4] + (IN_CONNECTED(2) ? in[2][i] * 0.2f : 0.0f), 0.0f, 1.0f);
            float r1 = pw_rand_f(&s->seed), r2 = pw_rand_f(&s->seed), r3 = pw_rand_f(&s->seed);
            s->f = pw_minf(1000.0f * pw_exp2(m->p[1] / 12.0f + (IN_CONNECTED(1) ? in[1][i] : 0.0f) + r1 * sc * 1.5f), 16000.0f);
            float dec = m->p[2] * pw_exp2(r2 * sc * 1.5f);
            s->kd = pw_coef(dec / 6.9f);
            s->tone = pw_clamp(m->p[3] + r3 * sc * 0.4f, 0.0f, 1.0f);
            s->rnd = (r1 + 1.0f) * 2.5f;
            s->env = 1.0f; s->pe = 1.0f; s->on = 1; s->ph = s->ph2 = 0;
            if (model == 0) s->z1 += 1.0f;                                    /* strike the resonance */
        }
        float y = 0;
        if (s->on) {
            float f = s->f, t = s->tone;
            switch (model) {
            case 0: {                                                          /* CLICK: a resonance struck */
                float rr = pw_exp2(-PW_PI * f * (0.02f + 0.3f * t) * PW_INV_SR * 1.4427f), c = 2.0f * rr * pw_cos1(f * PW_INV_SR);
                float o = c * s->z1 - rr * rr * s->z2; s->z2 = s->z1; s->z1 = o; y = o * pw_sin1(f * PW_INV_SR) * 2.0f;
            } break;
            case 1: {                                                          /* ZAP */
                float fz = f * (1.0f + (2.0f + t * 40.0f) * s->pe * s->pe); s->pe *= 0.9985f - t * 0.001f;
                y = pw_sin1(s->ph); s->ph = pw_frac(s->ph + pw_minf(fz, 18000.0f) * PW_INV_SR);
            } break;
            case 2: {                                                          /* BLIP */
                float md = pw_sin1(s->ph2) * (1.0f + t * 6.0f) * s->env;
                y = pw_sin1(s->ph + md * 0.25f);
                s->ph = pw_frac(s->ph + f * PW_INV_SR); s->ph2 = pw_frac(s->ph2 + f * (1.5f + t * 2.5f) * PW_INV_SR);
            } break;
            case 3: {                                                          /* TICK: noise through a band */
                float n = pw_rand_f(&s->seed), g = pw_tan_pi(pw_minf(f, 16000.0f) * PW_INV_SR), k = 2.0f - 1.9f * t;
                float a1 = 1.0f / (1.0f + g * (g + k)), v3 = n - s->z2, v1 = a1 * s->z1 + g * a1 * v3, v2 = s->z2 + g * v1;
                s->z1 = 2.0f * v1 - s->z1; s->z2 = 2.0f * v2 - s->z2; y = v1 * (1.5f + t * 2.0f);
            } break;
            case 4: {                                                          /* METAL */
                float a = s->ph < 0.5f ? 1.0f : -1.0f, b = s->ph2 < 0.5f ? 1.0f : -1.0f;
                s->ph = pw_frac(s->ph + f * PW_INV_SR); s->ph2 = pw_frac(s->ph2 + f * (1.17f + t * 1.3f) * PW_INV_SR);
                float x = a * b; s->hp += 0.3f * (x - s->hp); y = (x - s->hp) * 0.8f;
            } break;
            default: {                                                         /* CRACKLE */
                float dens = 200.0f + t * 4000.0f, u = pw_rand_f(&s->seed) * 0.5f + 0.5f;
                float imp = u < dens * PW_INV_SR ? pw_rand_f(&s->seed) : 0.0f;
                float rr = 0.96f, c = 2.0f * rr * pw_cos1(f * PW_INV_SR), o = imp + c * s->z1 - rr * rr * s->z2;
                s->z2 = s->z1; s->z1 = o; y = o * 0.6f;
            } break;
            }
            y *= s->env * m->p[5];
            s->env *= s->kd;
            if (s->env < 0.0003f && model != 0) s->on = 0;
            if (s->env < 0.0003f && model == 0) { s->on = 0; s->z1 = s->z2 = 0; }
        }
        out[0][i] = pw_tanh(y * 1.3f) * 4.0f;
        out[1][i] = s->rnd;
    }
}

/* SHUFFLER: what came in, played back out of order. It keeps the last few seconds (half rate: a little lo-fi);
 * at each step of the clock (unpatched: the transport's sixteenths), by CHANCE, the step plays a slice from up to
 * DEPTH steps back instead of what is coming in, sometimes REVERSEd, sometimes an octave up (PITCH +) or down (-),
 * sometimes stuttered into four (STUTTER). MIX: the dry sound under it. */
typedef struct { uint32_t w, since, period; float rpos, rinc, fade, seg0, seglen, segpos; uint32_t seed;
                 uint8_t clk_hi, wet, stut, half; float acc; } shuf_st;
PROC_COLD(shuffler_proc)
{
    ST(shuf_st);
    if (!s->seed) s->seed = 0x5F1u + (uint32_t)(m - pws.mod) * 2654435761u;
    if (m->status != PW_STATUS_OK || !m->mem_len) { for (int i = 0; i < PW_N; ++i) out[0][i] = in[0][i]; return; }
    int16_t *buf = PW_POOL16(m);
    uint32_t cap = (m->mem_len - 16u) / 2u;
    float mix = m->p[5];
    for (int i = 0; i < PW_N; ++i) {
        float x = in[0][i];
        s->acc += x * 0.5f;                                                   /* half rate: the average of two */
        if (s->half ^= 1) { } else { buf[s->w] = (int16_t)(pw_clamp(s->acc * 0.2f, -1.2f, 1.2f) * 26000.0f); s->acc = 0; if (++s->w >= cap) s->w = 0; }
        int clock = IN_CONNECTED(1) ? pw_rise(&s->clk_hi, in[1][i]) : pws.tick_at[i];
        s->since++;
        if (clock) {
            if (s->since > 8) s->period = s->since;
            s->since = 0;
            float chance = pw_clamp(m->p[0] + (IN_CONNECTED(2) ? in[2][i] * 0.2f : 0.0f), 0.0f, 1.0f);
            float step = (float)(s->period ? s->period : 5512u) * 0.5f;          /* a step in the buffer's samples */
            int depth = (int)(m->p[1] + 0.5f), back = 1 + (int)((pw_rand_f(&s->seed) * 0.5f + 0.5f) * (float)depth);
            if (back > depth) back = depth;
            if ((float)back * step + step > (float)cap - 8.0f) back = (int)(((float)cap - 8.0f - step) / step);
            s->wet = back >= 1 && (pw_rand_f(&s->seed) * 0.5f + 0.5f) < chance;
            if (s->wet) {
                int rev = (pw_rand_f(&s->seed) * 0.5f + 0.5f) < m->p[2];
                float pr = pw_rand_f(&s->seed) * 0.5f + 0.5f, pc = m->p[3], speed = pr < (pc < 0 ? -pc : pc) ? (pc < 0 ? 0.5f : 2.0f) : 1.0f;   /* PITCH: + an octave up, - down, by chance */
                s->stut = (pw_rand_f(&s->seed) * 0.5f + 0.5f) < m->p[4];
                float start = (float)s->w - (float)back * step; if (start < 0) start += (float)cap;
                s->seglen = s->stut ? step * 0.25f : step;
                s->seg0 = rev ? start + s->seglen * speed - 1.0f : start;
                s->rinc = (rev ? -0.5f : 0.5f) * speed;
                s->segpos = 0; s->rpos = s->seg0;
            }
            s->fade = 0;
        }
        float y = x;
        if (s->wet) {
            if (s->stut && pw_absf(s->rinc) * s->segpos >= s->seglen) { s->segpos = 0; s->rpos = s->seg0; }
            float p = s->rpos; while (p < 0) p += (float)cap; while (p >= (float)cap) p -= (float)cap;
            int a = (int)p; float fr = p - (float)a; int b = a + 1 >= (int)cap ? 0 : a + 1;
            float v = ((float)buf[a] + ((float)buf[b] - (float)buf[a]) * fr) * (5.0f / 26000.0f);
            s->rpos += s->rinc; s->segpos += 1.0f;
            float g = s->fade < 1.0f ? (s->fade += 0.02f) : 1.0f;
            y = x * mix + v * g + x * (1.0f - g) * (1.0f - mix);
        } else y = x;
        out[0][i] = y;
    }
}


/* ---- modules made of others (v28): run an older module's code on a view of this one: its knobs, jacks and
 * state picked out of the new module's. (A copy of the head of the module, without its 320-byte state.) */
static PW_DSP void sub_run(pw_proc_fn fn, pw_module *m, void *stp, const float *const *in, float *const *out, unsigned connected,
                           const int8_t *pmap, unsigned np, const int8_t *imap, unsigned ni, const int8_t *omap, unsigned no)
{
    pw_module t;
    memcpy(&t, m, offsetof(pw_module, st));
    t.stp = (uint8_t *)stp;
    for (unsigned k = 0; k < np; ++k) t.p[k] = m->p[pmap[k]];
    if (m->type == PW_T_FRACTURE && fn == reverser_proc) { float h = sync_sec(m->p[13]); if (h > 0) t.p[0] = h; }   /* FRACTURE's SYNC: the REVERSE piece as a note value */
    if (m->type == PW_T_SWIRL) { float h = sync_hz(m->p[8]); if (h > 0) t.p[0] = h; }   /* MOD FX's SYNC: the sweep's rate as a note value */
    const float *in2[PW_MAX_IN];
    float *out2[PW_MAX_OUT];
    unsigned c2 = 0;
    for (unsigned j = 0; j < ni; ++j) { in2[j] = in[imap[j]]; c2 |= ((connected >> imap[j]) & 1u) << j; }
    for (unsigned j = ni; j < PW_MAX_IN; ++j) in2[j] = in[imap[0]];
    for (unsigned o = 0; o < no; ++o) out2[o] = out[omap[o]];
    fn(&t, in2, out2, c2);
}

/* SWIRL: FLANGER, PHASER or CHORUS (TYPE); the knobs that belong to one are named for it on the help page */
static const uint8_t fract_order[14] = {6, 5, 1, 2, 0, 3, 4, 7, 8, 9, 10, 11, 12, 13};
static const char *const L_fract_pages[] = {"MAIN", "GLITCH", "MORE", "SHUF"};
static const char *const L_swirl[] = {"FLANGER", "PHASER", "CHORUS"};
typedef struct { union { flang_st f; phase_st p; chor_st c; } u; uint8_t type; } swirl_st;
static void swirl_init(pw_module *m) { ST(swirl_st); s->type = 255; }
PROC(swirl_proc)
{
    ST(swirl_st);
    static const int8_t pm_fl[5] = {1, 2, 5, 4, 3}, pm_ph[5] = {1, 2, 6, 4, 3}, pm_ch[4] = {1, 2, 3, 7};
    static const int8_t im[2] = {0, 1}, om[2] = {0, 1};
    unsigned type = (unsigned)pw_clamp(m->p[0] + 0.5f, 0.0f, 2.0f);
    if (s->type != type) {                                   /* another effect: start it from silence */
        memset(&s->u, 0, sizeof s->u);
        if (m->status == PW_STATUS_OK && m->mem_len) memset(PW_POOL16(m), 0, m->mem_len);
        s->type = (uint8_t)type;
    }
    if (type == 0) { sub_run(flanger_proc, m, &s->u, in, out, connected, pm_fl, 5, im, 2, om, 1); for (int i = 0; i < PW_N; ++i) out[1][i] = out[0][i]; }
    else if (type == 1) { sub_run(phaser_proc, m, &s->u, in, out, connected, pm_ph, 5, im, 2, om, 1); for (int i = 0; i < PW_N; ++i) out[1][i] = out[0][i]; }
    else sub_run(chorus_proc, m, &s->u, in, out, connected, pm_ch, 4, im, 2, om, 2);
}

/* FRACTURE: GLITCH (stutters, repeats, reverses slices of the beat), REVERSE (everything backwards, TIME at a
 * time) or SHUFFLE (slices from the last few beats, out of order) */
static const char *const L_fracmode[] = {"GLITCH", "REVERSE", "SHUFFLE"};
typedef struct { union { frac_st f; rev_st r; shuf_st h; } u; uint8_t mode; } fract_st;
static void fract_init(pw_module *m) { ST(fract_st); s->mode = 255; }
PROC(fract_proc)
{
    ST(fract_st);
    static const int8_t pm_g[6] = {0, 1, 2, 3, 4, 5}, pm_r[3] = {7, 8, 5}, pm_s[6] = {1, 9, 2, 10, 11, 12};
    static const int8_t im_g[3] = {0, 1, 2}, im_r[2] = {0, 3}, im_s[3] = {0, 2, 4}, om[1] = {0};
    unsigned mode = (unsigned)pw_clamp(m->p[6] + 0.5f, 0.0f, 2.0f);
    if (s->mode != mode) {
        memset(&s->u, 0, sizeof s->u);
        if (m->status == PW_STATUS_OK && m->mem_len) memset(PW_POOL16(m), 0, m->mem_len);
        s->mode = (uint8_t)mode;
        unsigned seed = (unsigned)(m - pws.mod);
        if (mode == 0) s->u.f.seed = 0xF4AC7u + seed * 2654435761u;
        if (mode == 2) s->u.h.seed = 0x5F1u + seed * 2654435761u;
    }
    if (mode == 0) sub_run(fracture_proc, m, &s->u, in, out, connected, pm_g, 6, im_g, 3, om, 1);
    else if (mode == 1) sub_run(reverser_proc, m, &s->u, in, out, connected, pm_r, 3, im_r, 2, om, 1);
    else sub_run(shuffler_proc, m, &s->u, in, out, connected, pm_s, 6, im_s, 3, om, 1);
}

/* the modules folded into others in v28 and removed in rc15: their numbers are kept (PW_T_GONE_*) with an empty descriptor, so that no other type moves */
int pw_type_hidden(unsigned t)
{
    return t == PW_T_GONE_FLANGER || t == PW_T_GONE_PHASER || t == PW_T_GONE_CHORUS || t == PW_T_GONE_REVERSER || t == PW_T_GONE_SHUFFLER || t == PW_T_GONE_CLOCKWARP || t == PW_T_GONE_MIXER || t == PW_T_GONE_PDOSC || t == PW_T_GONE_FMTX;
}

/* ---- module presets: a PRESET knob (DRUM RACK: KIT) that sets the module's other knobs at once, one undo. -1 in a
 * row: that knob as it is. */
#define KEEP -1.0f
static const char *const L_fmpre[] = {"-", "E.PIANO", "BELL", "BRASS", "BASS", "MARIMBA", "ORGAN", "PAD", "PLUCK", "GLASS", "GROWL", "LOG DRUM"};
static const int8_t fm_pre[11][6] = {   /* RATIO idx, INDEX%, FBK%, AMP shape, MOD shape, TIME x10 */
    {3, 45, 10, 3, 1, 10},    /* E.PIANO */
    {8, 60, 0, 4, 4, 10},     /* BELL */
    {3, 55, 30, 5, 5, 10},    /* BRASS */
    {3, 55, 25, 1, 2, 10},    /* BASS */
    {9, 30, 0, 2, 11, 12},    /* MARIMBA */
    {5, 30, 10, 0, 0, 10},    /* ORGAN */
    {3, 35, 0, 7, 7, 15},     /* PAD */
    {7, 50, 0, 1, 1, 10},     /* PLUCK */
    {10, 40, 0, 10, 4, 10},   /* GLASS */
    {1, 80, 60, 9, 9, 10},    /* GROWL */
    {1, 60, 0, 2, 11, 10},    /* LOG DRUM */
};
static const char *const L_addpre[] = {"-", "GLASS PAD", "ORGAN", "VOX", "BELLS", "PLUCK", "HOLLOW", "BUZZ", "SWEEP", "GONG", "STRINGS"};
static const float add_pre[10][8] = {   /* TUNE, SHAPE, BRIGHT, PLUCK, PEAK, RES, COMB, STRETCH */
    {KEEP, 0.1f, 0.75f, 0, 0.7f, 0.4f, 0.2f, 0.05f}, {KEEP, 0.5f, 0.55f, 0, 0.3f, 0, 0.55f, 0},
    {KEEP, 0.5f, 0.6f, 0, 0.45f, 0.8f, 0, 0}, {KEEP, 0.2f, 0.8f, 0.55f, 0.6f, 0.2f, 0, 0.55f},
    {KEEP, 0.5f, 0.4f, 0.45f, 0.5f, 0.2f, 0, 0}, {KEEP, 0.75f, 0.5f, 0, 0.35f, 0.3f, 0.35f, 0},
    {KEEP, 1.0f, 0.7f, 0, 0.55f, 0.5f, 0, 0}, {KEEP, 0.5f, 0.5f, 0, 0.3f, 0.9f, 0.15f, 0},
    {KEEP, 0.3f, 0.7f, 0.8f, 0.4f, 0.3f, 0, 0.9f}, {KEEP, 0.55f, 0.55f, 0, 0.5f, 0.25f, 0.1f, 0.02f}};
static const char *const L_macpre[] = {"-", "SUPERSAW", "SYNC LEAD", "WOODBLOCK", "VOX", "CHORD STAB", "NOISE SWEEP", "RAIN", "FM BELL", "WOBBLE", "GRIT"};
static const float mac_pre[10][8] = {   /* MODEL, TUNE, HARM, TIMBRE, MORPH, DECAY, COLOR, FINE */
    {5, KEEP, 0.6f, 0.8f, 0.3f, 0.6f, 0.7f, KEEP}, {0, KEEP, 0.5f, 0.4f, 0.6f, 0.4f, 0.7f, KEEP},
    {1, KEEP, 0.5f, 0.35f, 0.2f, 0.08f, 0.9f, KEEP}, {3, KEEP, 0.3f, 0.55f, 0.5f, 0.5f, 0.6f, KEEP},
    {4, KEEP, 0.45f, 0.3f, 0.2f, 0.3f, 0.8f, KEEP}, {6, KEEP, 0.5f, 0.7f, 0, 0.6f, 0.5f, KEEP},
    {7, KEEP, 0.45f, 0.5f, 0.6f, 1.0f, 0.5f, KEEP}, {2, KEEP, 0.7f, 0.55f, 0.1f, 1.5f, 0.3f, KEEP},
    {2, KEEP, 0.25f, 0.6f, 0.5f, 0.3f, 0.8f, KEEP}, {1, KEEP, 0.8f, 0.8f, 0.9f, 0.2f, 1.0f, KEEP}};
static const char *const L_respre[] = {"-", "GLOCK", "KALIMBA", "STEEL DRUM", "BOWL", "HARP", "NYLON", "CHIME", "TABLA", "GAMELAN", "WOOD",
    "WOODBLOCK", "CLAVES", "TEMPLE BLOCK", "TOM", "TIMPANI", "CONGA", "SPRING BOING", "THUNDER SHEET", "GONG", "TUBE BELLS", "ORGAN PIPE", "STEEL PLATE", "ANVIL", "WINE GLASS"};
static const float res_pre[24][10] = {   /* TUNE, DECAY, BRIGHT, STRIKE, MODEL, STRUCT, POS, (PRESET), EXCITER, BEND */
    {KEEP, 0.7f, 0.85f, 0.8f, 2, 0.5f, 0.2f, KEEP, 0, 0}, {KEEP, 0.55f, 0.5f, 0.6f, 2, 0.35f, 0.4f, KEEP, 0, 0}, {KEEP, 0.6f, 0.6f, 0.7f, 3, 0.6f, 0.3f, KEEP, 0, 0},
    {KEEP, 0.95f, 0.4f, 0.4f, 1, 0.5f, 0.5f, KEEP, 0, 0}, {KEEP, 0.8f, 0.6f, 0.7f, 0, 0.5f, 0.25f, KEEP, 0, 0}, {KEEP, 0.65f, 0.35f, 0.5f, 0, 0.5f, 0.35f, KEEP, 0, 0},
    {KEEP, 0.9f, 0.7f, 0.8f, 5, 0.5f, 0.2f, KEEP, 0, 0}, {KEEP, 0.4f, 0.5f, 0.8f, 4, 0.3f, 0.6f, KEEP, 0, 0}, {KEEP, 0.85f, 0.7f, 0.9f, 1, 0.75f, 0.3f, KEEP, 0, 0},
    {KEEP, 0.25f, 0.3f, 0.9f, 2, 0.2f, 0.5f, KEEP, 0, 0},
    {KEEP, 0.55f, 0.55f, 0.75f, 6, 0.5f, 0.3f, KEEP, 2, 0},    /* WOODBLOCK: a hollow block, a short knock */
    {KEEP, 0.35f, 0.4f, 0.55f, 2, 0.3f, 0.5f, KEEP, 1, 0},     /* CLAVES: a stick on a stick */
    {KEEP, 0.75f, 0.7f, 0.8f, 6, 0.3f, 0.4f, KEEP, 2, 0},      /* TEMPLE BLOCK: bigger, a little longer */
    {KEEP, 0.55f, 0.45f, 0.7f, 4, 0.4f, 0.35f, KEEP, 2, 8},  /* TOM: a head that drops */
    {KEEP, 0.75f, 0.4f, 0.6f, 11, 0.5f, 0.3f, KEEP, 2, 4},   /* TIMPANI: a kettle */
    {KEEP, 0.5f, 0.55f, 0.7f, 4, 0.55f, 0.2f, KEEP, 2, 2},    /* CONGA: a skin hit near the edge */
    {KEEP, 0.65f, 0.7f, 0.55f, 8, 0.5f, 0.3f, KEEP, 1, 7},    /* SPRING BOING: a twang that falls */
    {KEEP, 0.95f, 0.6f, 0.5f, 7, 0.5f, 0.4f, KEEP, 0, 0},    /* THUNDER SHEET: a shaken sheet of metal */
    {KEEP, 0.97f, 0.55f, 0.55f, 10, 0.5f, 0.3f, KEEP, 0, 0}, /* GONG: a hum that blooms */
    {KEEP, 0.95f, 0.5f, 0.7f, 1, 0.35f, 0.35f, KEEP, 2, 0},    /* TUBE BELLS: long, with a strong prime */
    {KEEP, 0.55f, 0.3f, 0.35f, 9, 0.5f, 0.3f, KEEP, 0, 0},    /* ORGAN PIPE: a breath on a pipe */
    {KEEP, 0.85f, 0.5f, 0.8f, 3, 0.4f, 0.3f, KEEP, 1, 0},     /* STEEL PLATE */
    {KEEP, 0.4f, 0.8f, 0.5f, 7, 0.4f, 0.5f, KEEP, 1, 0},       /* ANVIL: struck hard, short and bright */
    {KEEP, 0.9f, 0.2f, 0.45f, 1, 0.7f, 0.3f, KEEP, 2, 0}};     /* WINE GLASS: a pure, quiet ring */
static const char *const L_drumpre[] = {"-", "808 KICK","DEEP KICK","PUNCH KICK","HOUSE KICK","TECHNO KICK","CLICK KICK","SUB BOOM","SOFT KICK","GABBER KICK","808 SNARE","TIGHT SNARE","FAT SNARE","RIMSHOT","CLAP","SNAP","LOW TOM","MID TOM","HIGH TOM","SYN TOM","CLOSED HAT","PEDAL HAT","OPEN HAT","SIZZLE HAT","RIDE","CRASH","COWBELL","CLAVES","WOOD BLOCK","CONGA","BONGO","ZAP","LASER","BLIP","RISER","BELL","GLASS TAP","FM BONG","NOISE HIT","909 SNARE","SHELL SNARE","CRACK SNARE","LOOSE CLAP","TIGHT CLAP","THUMP KICK","TICK HAT","DARK HAT","WASH HAT","TIMPANI"};
static const float drum_pre[48][11] = {   /* TUNE, SWEEP, SWEEP T, DECAY, WAVE, NOISE, COLOR, N DECAY, FM, RATIO, DRIVE (LEVEL stays); drum_x: METAL, BURST */
    {50, 24, 0.06f, 1, 0, 0.02f, 800, 0.01f, 0, 1.41f, 0.15f}, /* 808 KICK */
    {40, 30, 0.08f, 1.4f, 0, 0, 500, 0.01f, 0, 1, 0.1f}, /* DEEP KICK */
    {60, 36, 0.025f, 0.35f, 0.2f, 0.1f, 2500, 0.015f, 0, 1.41f, 0.35f}, /* PUNCH KICK */
    {55, 30, 0.035f, 0.45f, 0.1f, 0.05f, 2000, 0.02f, 0, 1.41f, 0.4f}, /* HOUSE KICK */
    {48, 40, 0.03f, 0.5f, 0.3f, 0.05f, 1500, 0.02f, 0.05f, 1.41f, 0.7f}, /* TECHNO KICK */
    {58, 36, 0.02f, 0.25f, 0.6f, 0.35f, 5000, 0.01f, 0, 1.41f, 0.3f}, /* CLICK KICK */
    {32, 20, 0.12f, 2.5f, 0, 0, 300, 0.01f, 0, 1, 0.05f}, /* SUB BOOM */
    {65, 18, 0.03f, 0.3f, 0, 0.05f, 1200, 0.02f, 0, 1, 0}, /* SOFT KICK */
    {62, 36, 0.04f, 0.5f, 0.7f, 0.1f, 2500, 0.02f, 0, 1.41f, 1}, /* GABBER KICK */
    {190, 8, 0.02f, 0.18f, 0.1f, 0.65f, 3500, 0.2f, 0, 1.41f, 0.2f}, /* 808 SNARE */
    {220, 10, 0.015f, 0.1f, 0.2f, 0.7f, 4500, 0.12f, 0, 1.41f, 0.3f}, /* TIGHT SNARE */
    {170, 12, 0.025f, 0.28f, 0.1f, 0.7f, 2800, 0.3f, 0, 1.41f, 0.4f}, /* FAT SNARE */
    {1650, -5, 0.004f, 0.02f, 0.4f, 0.3f, 3200, 0.012f, 0.3f, 1.5f, 0.35f}, /* RIMSHOT */
    {1100, 0, 0.01f, 0.01f, 0, 1, 1300, 0.05f, 0, 1, 0.35f}, /* CLAP */
    {800, -12, 0.01f, 0.05f, 0.5f, 0.6f, 5500, 0.06f, 0.3f, 2.1f, 0.2f}, /* SNAP */
    {85, 14, 0.06f, 0.55f, 0, 0.05f, 1500, 0.03f, 0, 1.41f, 0.1f}, /* LOW TOM */
    {125, 14, 0.05f, 0.45f, 0, 0.05f, 1800, 0.03f, 0, 1.41f, 0.1f}, /* MID TOM */
    {180, 14, 0.04f, 0.35f, 0, 0.05f, 2200, 0.03f, 0, 1.41f, 0.1f}, /* HIGH TOM */
    {110, 30, 0.08f, 0.5f, 0.2f, 0, 2000, 0.03f, 0, 1.41f, 0.3f}, /* SYN TOM */
    {330, 0, 0.01f, 0.012f, 0, 0.3f, 9000, 0.012f, 0, 1, 0.15f}, /* CLOSED HAT */
    {330, 0, 0.01f, 0.02f, 0, 0.3f, 8500, 0.02f, 0, 1, 0.15f}, /* PEDAL HAT */
    {330, 0, 0.01f, 0.075f, 0, 0.35f, 9000, 0.09f, 0, 1, 0.2f}, /* OPEN HAT */
    {360, 0, 0.01f, 0.18f, 0, 0.5f, 11000, 0.25f, 0, 1, 0.25f}, /* SIZZLE HAT */
    {420, 0, 0.01f, 0.45f, 0, 0.15f, 7000, 0.3f, 0.3f, 3.5f, 0.15f}, /* RIDE */
    {380, 0, 0.01f, 0.5f, 0, 0.6f, 6500, 0.55f, 0, 1, 0.3f}, /* CRASH */
    {540, 0, 0.005f, 0.06f, 1, 0.1f, 2640, 0.006f, 0.25f, 1.48f, 0.5f}, /* COWBELL */
    {2000, -4, 0.005f, 0.06f, 0, 0, 3000, 0.01f, 0, 1, 0.1f}, /* CLAVES */
    {850, -2, 0.01f, 0.016f, 0.25f, 0.1f, 3000, 0.004f, 0.25f, 1.77f, 0.1f}, /* WOOD BLOCK */
    {220, 10, 0.03f, 0.3f, 0, 0.05f, 2000, 0.03f, 0, 1.41f, 0.1f}, /* CONGA */
    {330, 8, 0.02f, 0.18f, 0, 0.05f, 2500, 0.02f, 0, 1.41f, 0.1f}, /* BONGO */
    {1500, 48, 0.15f, 0.3f, 0.5f, 0, 3000, 0.02f, 0, 1.41f, 0.2f}, /* ZAP */
    {400, 48, 0.4f, 0.5f, 0.3f, 0, 3000, 0.02f, 0.2f, 1.41f, 0.2f}, /* LASER */
    {900, 12, 0.02f, 0.1f, 0, 0, 3000, 0.02f, 0, 1, 0.1f}, /* BLIP */
    {300, -36, 0.5f, 0.6f, 0.3f, 0.1f, 3000, 0.3f, 0, 1.41f, 0.2f}, /* RISER */
    {700, 0, 0.01f, 1.5f, 0, 0, 3000, 0.02f, 0.5f, 3.5f, 0}, /* BELL */
    {1400, 0, 0.01f, 0.4f, 0, 0, 3000, 0.02f, 0.7f, 5.4f, 0}, /* GLASS TAP */
    {160, 6, 0.03f, 0.8f, 0, 0, 3000, 0.02f, 0.7f, 1.41f, 0.1f}, /* FM BONG */
    {400, 0, 0.01f, 0.01f, 0, 1, 2500, 0.4f, 0, 1, 0.2f}, /* NOISE HIT */
    {200, 14, 0.02f, 0.16f, 0.15f, 0.55f, 5000, 0.16f, 0.25f, 1.47f, 0.35f}, /* 909 SNARE: two tones a 1.47 apart, FM, noise */
    {170, 9, 0.02f, 0.22f, 0, 0.45f, 3000, 0.22f, 0, 1.41f, 0.25f}, /* SHELL SNARE: the shell's modes up, noise under */
    {260, 20, 0.012f, 0.09f, 0.4f, 0.75f, 6000, 0.1f, 0.35f, 2.1f, 0.5f}, /* CRACK SNARE: short, hard, bright */
    {1000, 0, 0.01f, 0.01f, 0, 1, 1100, 0.12f, 0, 1, 0.3f}, /* LOOSE CLAP: few bursts, a long tail */
    {1250, 0, 0.01f, 0.01f, 0, 1, 1500, 0.03f, 0, 1, 0.3f}, /* TIGHT CLAP: many bursts, almost no tail */
    {45, 18, 0.09f, 0.9f, 0, 0.02f, 600, 0.01f, 0, 1, 0.2f}, /* THUMP KICK: soft, long, low */
    {340, 0, 0.01f, 0.007f, 0, 0.25f, 10000, 0.006f, 0, 1, 0.15f}, /* TICK HAT: a flick */
    {300, 0, 0.01f, 0.03f, 0, 0.3f, 6500, 0.04f, 0, 1, 0.2f}, /* DARK HAT */
    {330, 0, 0.01f, 0.35f, 0, 0.4f, 8000, 0.4f, 0, 1, 0.2f}, /* WASH HAT: open, loose */
    {90, 8, 0.08f, 1.2f, 0, 0, 1500, 0.02f, 0, 1, 0.05f}, /* TIMPANI: a long head, many modes */
};
static const uint8_t drums_order[16] = {12, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15};   /* PRESET first */
int pw_preset_param(unsigned type)
{
    switch (type) {
    case PW_T_DRUMRACK: return 36;
    case PW_T_DRUMS: return 12;
    case PW_T_FM4: return FM_PRESET;
    case PW_T_ADDITIVE: case PW_T_MACRO: return 8;
    case PW_T_STRING: return 7;
    case PW_T_CZ: return CZ_PRESET;
    case PW_T_FMDX: return DX_PRESET;
    default: return -1;
    }
}
static void set_row(unsigned slot, const float *row, unsigned n) { for (unsigned p = 0; p < n; ++p) if (row[p] > -0.5f) pw_set_param(slot, p, row[p]); }
PW_COLD void pw_module_preset(unsigned slot, int k)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || k <= 0) return;
    switch (pws.mod[slot].type) {
    case PW_T_DRUMRACK: rack_kit(slot, k); break;
    case PW_T_FM4: {
        if (k > 11) break;
        const int8_t *r = fm_pre[k - 1];
        pw_set_param(slot, FM_RATIO, r[0]); pw_set_param(slot, FM_INDEX, r[1] * 0.01f); pw_set_param(slot, FM_FBK, r[2] * 0.01f);
        pw_set_param(slot, FM_AMP, r[3]); pw_set_param(slot, FM_MOD, r[4]); pw_set_param(slot, FM_TIME, r[5] * 0.1f);
    } break;
    case PW_T_DRUMS: if (k <= 38) {
        const float *r = drum_pre[k - 1];
        for (unsigned q = 0; q < 10; ++q) pw_set_param(slot, q, r[q]);
        pw_set_param(slot, 11, r[10]);
        {   /* the cymbals' metal (six squares) and the clap's bursts */
            float mt = 0, bu = 0, mo = 0;
            switch (k) {
            case 20: case 21: case 22: mt = 0.95f; break; case 23: mt = 0.85f; break; case 24: mt = 0.8f; break; case 25: mt = 0.6f; break;
            case 14: bu = 4; break;
            case 11: mo = 0.5f; break; case 12: mo = 0.35f; break; case 13: mo = 0.7f; break;   /* the snares' shells */
            case 17: case 18: case 19: mo = 0.25f; break; case 30: case 31: mo = 0.3f; break;   /* toms and congas: a head's other modes */
            case 39: mo = 0.55f; break; case 40: mo = 0.9f; break; case 41: mo = 0.3f; break;
            case 42: bu = 3; break; case 43: bu = 5; break;
            case 45: case 46: case 47: mt = 0.9f; break;
            case 48: mo = 0.8f; break;
            default: break;
            }
            pw_set_param(slot, 13, mt); pw_set_param(slot, 14, bu); pw_set_param(slot, 15, mo);
        }
    } break;
    case PW_T_ADDITIVE: if (k <= 10) set_row(slot, add_pre[k - 1], 8); break;
    case PW_T_MACRO: if (k <= 10) set_row(slot, mac_pre[k - 1], 8); break;
    case PW_T_STRING: if (k <= 24) set_row(slot, res_pre[k - 1], 10); break;
    case PW_T_CZ: {   /* a bank's tone: the knobs show it, the modifiers go back to neutral */
        pws.mod[slot].p[CZ_PRESET] = pw_cz_clamp(slot, CZ_PRESET, pws.mod[slot].p[CZ_PRESET]);
        const uint8_t *b = cz_tone(&pws.mod[slot]);
        if (!b) break;
        float v[CZ_N];
        cz_view(b, v);
        static const uint8_t tk[] = {CZ_W1, CZ_W2, CZ_DCW, CZ_DATK, CZ_DDEC, CZ_DSUS, CZ_DREL, CZ_AATK, CZ_ADEC, CZ_ASUS, CZ_AREL, CZ_PAMT, CZ_PTIM, CZ_VIB, CZ_LINE, CZ_MOD, CZ_DETUNE, CZ_OCT};
        for (unsigned q = 0; q < sizeof tk; ++q) pw_set_param(slot, tk[q], v[tk[q]]);
        static const uint8_t sc[] = {CZ_FOLLOW, CZ_VRATE, CZ_BRIGHT, CZ_SPEED, CZ_ASPD, CZ_REL};
        for (unsigned q = 0; q < sizeof sc; ++q) pw_set_param(slot, sc[q], 1.0f);
    } break;
    case PW_T_FMDX: {   /* a bank's voice: the knobs show it */
        pws.mod[slot].p[DX_PRESET] = dx_clamp(slot, DX_PRESET, pws.mod[slot].p[DX_PRESET]);
        const uint8_t *b = dx_voice(&pws.mod[slot]);
        if (!b) break;
        float v[DX_N];
        dx_view(b, v);
        for (unsigned q = DX_ALGO; q < DX_N; ++q) pw_set_param(slot, q, v[q]);
    } break;
    default: break;
    }
}
const pw_type pw_types[PW_T_COUNT] = {
    [PW_T_VCO] = {"VCO", "VCO", PW_C_SOURCE, 4, 4, 7,
        {"V/OCT", "FM", "PWM", "SYNC"}, {"SIN", "TRI", "SAW", "SQR"},
        (const pw_param_desc[]){P("TUNE", PW_LIN, PW_U_SEMI, -36, 36, 0), P("FINE", PW_LIN, PW_U_CENT, -100, 100, 0),
         P("PW", PW_LIN, PW_U_PCT, 0.05f, 0.95f, 0.5f), P("FM AMT", PW_LIN, PW_U_PCT, 0, 1, 0.2f),
         P("DRIFT", PW_LIN, PW_U_PCT, 0, 1, 0), PE("FM MODE", L_fmmode, 3, 0), P("ANALOG", PW_LIN, PW_U_PCT, 0, 1, 0)},
        0, 0, 0, vco_proc, 0, 0},
    [PW_T_LFO] = {"LFO", "LFO", PW_C_MOD, 3, 4, 4,
        {"RATE", "RESET", "DEPTH"}, {"SIN", "TRI", "SAW", "SQR"},
        (const pw_param_desc[]){P("RATE", PW_EXP, PW_U_HZ, 0.01f, 40, 1), P("LEVEL", PW_LIN, PW_U_PCT, 0, 1, 1),
         PE("RANGE", L_bipolar, 2, 0), PE("SYNC", L_sync, SYNC_N, 0)},
        0, 0, 0, lfo_proc, 0, 0},
    [PW_T_NOISE] = {"NOISE", "NOISE", PW_C_SOURCE, 0, 6, 3,
        {0}, {"WHITE", "PINK", "BROWN", "DIGITAL", "DUST", "DTRIG"},
        (const pw_param_desc[]){P("LEVEL", PW_LIN, PW_U_PCT, 0, 1, 0.8f), P("TONE", PW_LIN, PW_U_PCT, -1, 1, 0), P("DENSITY", PW_EXP, PW_U_HZ, 0.5f, 5000, 30)},
        0, 0, noise_init, noise_proc, 0, 0},
    [PW_T_WAVETABLE] = {"WAVETABLE", "WAVE", PW_C_SOURCE, 3, 1, 4,
        {"V/OCT", "POS", "FM"}, {"OUT"},
        (const pw_param_desc[]){P("TUNE", PW_LIN, PW_U_SEMI, -36, 36, 0), P("FINE", PW_LIN, PW_U_CENT, -100, 100, 0),
         PE("TABLE", L_tables, 4, 0), P("POS", PW_LIN, PW_U_PCT, 0, 1, 0)},
        WT_BYTES, WT_BYTES, wavetable_init, wavetable_proc, 0, 0},
    [PW_T_COMPLEX] = {"COMPLEX OSC", "CMPLX", PW_C_SOURCE, 3, 3, 4,
        {"V/OCT", "TIMBRE", "INDEX"}, {"MOD", "MAIN", "MIX"},
        (const pw_param_desc[]){P("TUNE", PW_LIN, PW_U_SEMI, -36, 36, 0), P("RATIO", PW_EXP, PW_U_X, 0.25f, 8, 2),
         P("INDEX", PW_LIN, PW_U_NONE, 0, 10, 1.5f), P("TIMBRE", PW_LIN, PW_U_PCT, 0, 1, 0.2f)},
        0, 0, 0, complex_proc, 0, 0},
    [PW_T_VCF] = {"VCF", "VCF", PW_C_FILTER, 4, 4, 6,
        {"IN", "FREQ", "RES", "KEY"}, {"LP", "BP", "HP", "NOTCH"},
        (const pw_param_desc[]){P("CUTOFF", PW_EXP, PW_U_HZ, 20, 18000, 1200), P("RES", PW_LIN, PW_U_PCT, 0, 1, 0.25f),
         P("CV AMT", PW_LIN, PW_U_PCT, -1, 1, 1), P("DRIVE", PW_LIN, PW_U_PCT, 0, 1, 0), PE("TYPE", L_filt, 6, 0),
         P("KEYTRK", PW_LIN, PW_U_PCT, 0, 1, 0)},
        0, 0, 0, vcf_proc, 0, 0},
    [PW_T_VCA] = {"VCA", "VCA", PW_C_FILTER, 2, 1, 2,
        {"IN", "CV"}, {"OUT"},
        (const pw_param_desc[]){P("GAIN", PW_LIN, PW_U_PCT, 0, 1, 1), PE("CURVE", L_vca, 2, 0)},
        0, 0, 0, vca_proc, 0, 0},
    [PW_T_FOLDER] = {"WAVEFOLDER", "FOLD", PW_C_FX, 2, 1, 6,
        {"IN", "FOLD"}, {"OUT"},
        (const pw_param_desc[]){P("FOLD", PW_LIN, PW_U_X, 1, 12, 2), P("BIAS", PW_LIN, PW_U_NONE, -1, 1, 0),
         PE("SHAPE", L_fold, 4, 0), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1),
         P("ORDER", PW_LIN, PW_U_NONE, 1, 16, 3), P("TONE", PW_EXP, PW_U_HZ, 200, 16000, 1330)},
        0, 0, 0, folder_proc, 0, 0},
    [PW_T_HARMTREM] = {"HARM TREMOLO", "HTREM", PW_C_FX, 2, 1, 6,
        {"IN", "RATE"}, {"OUT"},
        (const pw_param_desc[]){P("RATE", PW_EXP, PW_U_HZ, 0.1f, 12, 4.5f), P("DEPTH", PW_LIN, PW_U_PCT, 0, 1, 0.7f),
         P("SPLIT", PW_EXP, PW_U_HZ, 100, 8000, 800), PE("SHAPE", L_trem, 2, 0), PE("SYNC", L_sync, SYNC_N, 0),
         P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1)},
        0, 0, 0, htrem_proc, ht_order, 0},
    [PW_T_DIST] = {"DISTORTION", "DIST", PW_C_FX, 2, 1, 4,
        {"IN", "DRIVE"}, {"OUT"},
        (const pw_param_desc[]){P("DRIVE", PW_LIN, PW_U_PCT, 0, 1, 0.35f), PE("MODE", L_dist, 4, 0),
         P("TONE", PW_LIN, PW_U_PCT, 0, 1, 0.7f), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1)},
        0, 0, 0, dist_proc, 0, 0},
    [PW_T_DELAY] = {"DELAY", "DELAY", PW_C_FX, 5, 4, 9,
        {"IN", "TIME", "FBK", "TAP", "FREEZE"}, {"MIX", "WET", "L", "R"},
        (const pw_param_desc[]){P("TIME", PW_EXP, PW_U_SEC, 0.005f, PW_DELAY_SECONDS, 0.3f), P("FBK", PW_LIN, PW_U_PCT, 0, 1.1f, 0.45f),
         P("TONE", PW_LIN, PW_U_PCT, 0, 1, 0.6f), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 0.35f),
         PE("SYNC", L_sync, SYNC_N, 0), PE("PING", L_onoff, 2, 0), P("WOBBLE", PW_LIN, PW_U_PCT, 0, 1, 0),
         P("WOB HZ", PW_EXP, PW_U_HZ, 0.1f, 8, 0.7f), PE("FREEZE", L_onoff, 2, 0)},
        SECONDS_TO_BYTES(PW_DELAY_SECONDS), SECONDS_TO_BYTES(0.1f), 0, delay_proc, 0, 0},
    [PW_T_REVERB] = {"REVERB", "VERB", PW_C_FX, 3, 2, 12,
        {"IN L", "IN R", "FREEZE"}, {"L", "R"},
        (const pw_param_desc[]){P("SIZE", PW_LIN, PW_U_PCT, 0, 1, 0.7f), P("DECAY", PW_EXP, PW_U_SEC, 0.3f, 20, 2.5f),
         P("DAMP", PW_LIN, PW_U_PCT, 0, 1, 0.4f), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 0.3f), PE("FREEZE", L_onoff, 2, 0),
         P("SHIMMER", PW_LIN, PW_U_PCT, 0, 1, 0), PE("SHIM", L_shim, 5, 0),
         PE("MODE", L_rvmode, 5, 1), P("PRE-DLY", PW_LIN, PW_U_SEC, 0, 0.05f, 0), P("DIFFUSE", PW_LIN, PW_U_PCT, 0, 1, 0.7f),
         P("MOD", PW_LIN, PW_U_PCT, 0, 1, 0.3f), P("LOW CUT", PW_LIN, PW_U_PCT, 0, 1, 0)},
        RV_BYTES, RV_BYTES, 0, reverb_proc, 0, 0},
    [PW_T_RING] = {"RING/SHIFT", "RING", PW_C_FX, 3, 3, 3,
        {"X", "Y", "SHIFT"}, {"RING", "UP", "DOWN"},
        (const pw_param_desc[]){P("SHIFT", PW_LIN, PW_U_HZ, -1000, 1000, 110), P("CV AMT", PW_LIN, PW_U_HZV, 0, 400, 50),
         P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1)},
        0, 0, 0, ring_proc, 0, 0},
    [PW_T_ENV] = {"ENVELOPE", "ENV", PW_C_MOD, 3, 3, 12,
        {"GATE", "RETRIG", "VEL"}, {"ENV", "INV", "EOC"},
        (const pw_param_desc[]){P("ATTACK", PW_EXP, PW_U_SEC, 0.001f, 10, 0.005f), P("DECAY", PW_EXP, PW_U_SEC, 0.001f, 10, 0.25f),
         P("SUSTAIN", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("RELEASE", PW_EXP, PW_U_SEC, 0.001f, 10, 0.4f),
         PE("MODE", L_env, 3, 0), PE("LOOP", L_onoff, 2, 0), P("A CURVE", PW_LIN, PW_U_NONE, -1, 1, 0),
         P("D CURVE", PW_LIN, PW_U_NONE, -1, 1, 0), P("R CURVE", PW_LIN, PW_U_NONE, -1, 1, 0),
         PE("TRIGGER", L_envtrig, 3, 0), P("VEL AMT", PW_LIN, PW_U_PCT, 0, 1, 0), PE("RANGE", L_envrange, 3, 1)},
        0, 0, 0, env_proc, 0, 0},
    [PW_T_FUNC] = {"FUNCTION", "FUNC", PW_C_MOD, 2, 3, 4,
        {"IN", "TRIG"}, {"OUT", "EOR", "EOC"},
        (const pw_param_desc[]){P("RISE", PW_EXP, PW_U_SEC, 0.001f, 10, 0.1f), P("FALL", PW_EXP, PW_U_SEC, 0.001f, 10, 0.3f),
         P("CURVE", PW_LIN, PW_U_NONE, -1, 1, 0), PE("CYCLE", L_onoff, 2, 0)},
        0, 0, 0, func_proc, 0, 0},
    [PW_T_SH] = {"S&H/RANDOM", "S&H", PW_C_MOD, 2, 3, 5,
        {"IN", "TRIG"}, {"S&H", "SMOOTH", "GATE"},
        (const pw_param_desc[]){P("SLEW", PW_LIN, PW_U_SEC, 0, 1, 0.1f), P("RANGE", PW_LIN, PW_U_VOLT, 0, 10, 2),
         P("PROB", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("RATE", PW_EXP, PW_U_HZ, 0.05f, 30, 4), PE("SYNC", L_sync, SYNC_N, 0)},
        0, 0, sh_init, sh_proc, 0, 0},
    [PW_T_ATTEN] = {"ATTENUVERTER", "ATTEN", PW_C_UTIL, 2, 2, 6,
        {"A", "B"}, {"A", "B"},
        (const pw_param_desc[]){P("A GAIN", PW_LIN, PW_U_PCT, -1, 1, 1), P("A OFFS", PW_LIN, PW_U_VOLT, -5, 5, 0),
         P("B GAIN", PW_LIN, PW_U_PCT, -1, 1, 1), P("B OFFS", PW_LIN, PW_U_VOLT, -5, 5, 0),
         P("CURVE", PW_LIN, PW_U_PCT, -1, 1, 0), PE("RECT", L_rect, 3, 0)},
        0, 0, 0, atten_proc, 0, 0},
    [PW_T_GONE_MIXER] = {"", "", PW_C_FX, 0, 0, 0, {0}, {0}, 0, 0, 0, 0, 0, 0, 0},
    [PW_T_CVMIX] = {"CV MIXER", "CVMIX", PW_C_UTIL, 4, 2, 5,
        {"IN 1", "IN 2", "IN 3", "IN 4"}, {"SUM", "INV"},
        (const pw_param_desc[]){P("ATT 1", PW_LIN, PW_U_PCT, -1, 1, 1), P("ATT 2", PW_LIN, PW_U_PCT, -1, 1, 1),
         P("ATT 3", PW_LIN, PW_U_PCT, -1, 1, 1), P("ATT 4", PW_LIN, PW_U_PCT, -1, 1, 1),
         P("OFFSET", PW_LIN, PW_U_VOLT, -5, 5, 0)},
        0, 0, 0, cvmix_proc, 0, 0},
    [PW_T_LOGIC] = {"LOGIC", "LOGIC", PW_C_UTIL, 2, 8, 1,
        {"A", "B"}, {"AND", "OR", "XOR", "NOT A", "A>B", "MIN", "MAX", "ABS A"},
        (const pw_param_desc[]){P("THRESH", PW_LIN, PW_U_VOLT, -5, 5, 1)},
        0, 0, 0, logic_proc, 0, 0},
    [PW_T_COMPARE] = {"COMPARE", "CMP", PW_C_UTIL, 2, 3, 4,
        {"A", "B"}, {"GATE", "INV", "A-B"},
        (const pw_param_desc[]){P("THRESH", PW_LIN, PW_U_VOLT, -5, 5, 0), P("HYST", PW_LIN, PW_U_VOLT, 0, 2, 0.1f),
         PE("MODE", L_cmpmode, 4, 0), P("PULSE", PW_LIN, PW_U_SEC, 0, 0.5f, 0)},
        0, 0, 0, compare_proc, 0, 0},
    [PW_T_SHIFTREG] = {"SHIFT REG", "SHREG", PW_C_MOD, 3, 6, 4,
        {"IN", "CLOCK", "RESET"}, {"NOW", "-1", "-2", "-3", "END", "MIX"},
        (const pw_param_desc[]){P("LENGTH", PW_INT, PW_U_NONE, 2, 16, 8), P("FEEDBK", PW_LIN, PW_U_PCT, 0, 1, 0),
         P("MUTATE", PW_LIN, PW_U_PCT, 0, 1, 0), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 0)},
        0, 0, shr_init, shr_proc, 0, 0},
    [PW_T_SWITCH] = {"SEQ SWITCH", "SWTCH", PW_C_UTIL, 6, 5, 4,
        {"IN 1", "IN 2", "IN 3", "IN 4", "CLOCK", "RESET"}, {"OUT", "OUT 1", "OUT 2", "OUT 3", "OUT 4"},
        (const pw_param_desc[]){P("STEPS", PW_INT, PW_U_NONE, 1, 4, 4), PE("MODE", L_dir, 4, 0),
         P("POS", PW_INT, PW_U_NONE, 1, 4, 1), P("FADE", PW_LIN, PW_U_SEC, 0, 0.1f, 0.004f)},
        0, 0, 0, switch_proc, 0, 0},
    [PW_T_SEQ] = {"SEQUENCER", "SEQ", PW_C_SEQ, 6, 6, 7,
        {"CLOCK", "RESET", "TRANSP", "LENGTH", "GATE", "STEP"}, {"PITCH", "GATE", "TRIG", "EOS", "POS", "VEL"},
        (const pw_param_desc[]){P("STEPS", PW_INT, PW_U_NONE, 1, 16, 8), P("GATE", PW_LIN, PW_U_PCT, 0.05f, 1, 0.5f),
         PE("MODE", L_dir, 4, 0), P("SLIDE", PW_LIN, PW_U_SEC, 0, 0.5f, 0),
         P("SWING", PW_LIN, PW_U_PCT, 0, 0.5f, 0), P("DIV", PW_INT, PW_U_DIV, -6, 8, 1), PE("MONITOR", L_onoff, 2, 1)},
        0, 0, seq_init, seq_proc, 0, 0},
    [PW_T_MODSEQ] = {"MOD SEQ", "MSEQ", PW_C_SEQ, 2, 9, 9,
        {"CLOCK", "RESET"}, {"LANE A", "LANE B", "LANE C", "LANE D", "EOS", "TRIG A", "TRIG B", "TRIG C", "TRIG D"},
        (const pw_param_desc[]){P("LEN A", PW_INT, PW_U_NONE, 1, 16, 16), P("LEN B", PW_INT, PW_U_NONE, 1, 16, 16),
         P("LEN C", PW_INT, PW_U_NONE, 1, 16, 16), P("LEN D", PW_INT, PW_U_NONE, 1, 16, 16),
         PE("MODE", L_dir, 4, 0), P("DIV", PW_INT, PW_U_DIV, -6, 8, 1), P("SLEW", PW_LIN, PW_U_PCT, 0, 1, 0),
         PE("RANGE", L_mrange, 3, 0), P("CHANCE", PW_LIN, PW_U_PCT, 0, 1, 1)},
        0, 0, mseq_init, mseq_proc, 0, 0},
    [PW_T_CHORDSTEPS] = {"CHORD STEPS", "CSTEP", PW_C_SEQ, 3, 8, 8,
        {"CLOCK", "RESET", "TRANSP"}, {"N1", "N2", "N3", "N4", "GATE", "TRIG", "ROOT", "EOS"},
        (const pw_param_desc[]){P("STEPS", PW_INT, PW_U_NONE, 1, 16, 8), PE("KEY", note_names, 12, 0), PE("SCALE", L_scale7, 7, 0),
         P("OCTAVE", PW_INT, PW_U_NONE, -3, 2, -1), P("GATE", PW_LIN, PW_U_PCT, 0.05f, 1, 0.8f),
         PE("MODE", L_dir, 4, 0), PE("SPREAD", L_onoff, 2, 0), P("DIV", PW_INT, PW_U_DIV, -6, 8, 1)},
        0, 0, cst_init, cst_proc, 0, 0},
    [PW_T_GRAVITY] = {"GRAVITY", "GRAV", PW_C_SEQ, 2, 6, 9,
        {"DROP", "TRANSP"}, {"PITCH", "GATE", "TRIG", "VEL", "HEIGHT", "EOS"},
        (const pw_param_desc[]){P("EVERY", PW_INT, PW_U_NONE, 1, 16, 4), P("FALL", PW_LIN, PW_U_NONE, 0.5f, 8, 2),
         P("ELASTIC", PW_LIN, PW_U_PCT, 0.4f, 0.95f, 0.75f), P("NOTES", PW_INT, PW_U_NONE, 2, 16, 8),
         PE("ORDER", L_dir, 4, 0), PE("SCALE", scale_names, 12, 7), PE("ROOT", note_names, 12, 0),
         P("GATE", PW_LIN, PW_U_PCT, 0.05f, 1, 0.4f), P("SPREAD", PW_LIN, PW_U_PCT, 0, 1, 0)},
        0, 0, grav_init, grav_proc, 0, 0},
    [PW_T_LISTENER] = {"LISTENER", "LISTEN", PW_C_MOD, 1, 4, 8,
        {"IN"}, {"PITCH", "GATE", "TRIG", "LEVEL"},
        (const pw_param_desc[]){P("THRESH", PW_LIN, PW_U_PCT, 0.01f, 0.6f, 0.08f), P("ONSET", PW_LIN, PW_U_NONE, 0.1f, 3, 0.8f),
         PE("MODE", L_listen, 3, 0), PE("SCALE", scale_names, 12, 7), PE("ROOT", note_names, 12, 0),
         P("RANGE", PW_LIN, PW_U_NONE, 1, 4, 2), P("OCTAVE", PW_INT, PW_U_NONE, -2, 2, 0),
         P("GAP", PW_LIN, PW_U_SEC, 0.02f, 0.3f, 0.06f)},
        0, 0, 0, listen_proc, 0, 0},
    [PW_T_CLOCK] = {"CLOCK", "CLOCK", PW_C_SEQ, 4, 7, 11,
        {"EXT", "RESET", "WARP", "COUNT"}, {"CLK", "DIV", "MULT", "RST", "WARP", "RAMP", "EOC"},
        (const pw_param_desc[]){PE("SOURCE", L_clock, 2, 0), P("TEMPO", PW_LIN, PW_U_BPM, 30, 300, 120),
         P("DIV", PW_INT, PW_U_NONE, 1, 16, 4), P("MULT", PW_INT, PW_U_NONE, 1, 8, 2),
         P("SWING", PW_LIN, PW_U_PCT, 0, 0.5f, 0),
         P("LENGTH", PW_INT, PW_U_NONE, 1, 32, 16), P("COUNT", PW_INT, PW_U_NONE, 1, 32, 16), P("WARP", PW_LIN, PW_U_PCT, -1, 1, 0),
         P("W SWING", PW_LIN, PW_U_PCT, 0, 0.75f, 0), P("DRIFT", PW_LIN, PW_U_PCT, 0, 1, 0), P("GATE", PW_LIN, PW_U_PCT, 0.05f, 0.95f, 0.3f)},
        0, 0, 0, clock_proc, 0, 0},
    [PW_T_QUANT] = {"QUANTIZER", "QUANT", PW_C_SEQ, 2, 2, 3,
        {"IN", "TRIG"}, {"OUT", "TRIG"},
        (const pw_param_desc[]){PE("SCALE", scale_names, 12, 8), PE("ROOT", note_names, 12, 0), P("OCT", PW_INT, PW_U_NONE, -3, 3, 0)},
        0, 0, 0, quant_proc, 0, 0},
    [PW_T_LPG] = {"LOW PASS GATE", "LPG", PW_C_FILTER, 3, 2, 10,
        {"IN", "CV", "PING"}, {"OUT", "VAC"},
        (const pw_param_desc[]){P("OFFSET", PW_LIN, PW_U_PCT, 0, 1, 0), P("RESP", PW_EXP, PW_U_SEC, 0.02f, 2, 0.3f),
         PE("MODE", L_lpg, 3, 2), P("RES", PW_LIN, PW_U_PCT, 0, 1, 0.2f),
         P("ATTACK", PW_EXP, PW_U_SEC, 0.0005f, 0.1f, 0.003f), P("MEMORY", PW_LIN, PW_U_PCT, 0, 1, 0.5f),
         P("STRIKE", PW_LIN, PW_U_PCT, 0, 1, 1), P("DRIVE", PW_LIN, PW_U_PCT, 0, 1, 0),
         PE("SLOPE", L_lpgslope, 2, 0), P("CV AMT", PW_LIN, PW_U_PCT, 0, 2, 1)},
        0, 0, 0, lpg_proc, 0, 0},
    [PW_T_SAMPLER] = {"SAMPLER", "SMPL", PW_C_SOURCE, 9, 3, 26,
        {"V/OCT", "GATE", "VEL", "START", "CUTOFF", "SLICE", "IN", "REC", "SAMPLE"}, {"OUT", "ENV", "END"},
        (const pw_param_desc[]){P("SAMPLE", PW_INT, PW_U_NONE, 0, 8, 0), PE("MODE", L_scmode, 4, 1), P("TUNE", PW_LIN, PW_U_SEMI, -24, 24, 0), P("LEVEL", PW_LIN, PW_U_PCT, 0, 1, 0.8f),
         P("START", PW_LIN, PW_U_PCT, 0, 1, 0), P("LENGTH", PW_LIN, PW_U_PCT, 0.01f, 1, 1), PE("LOOP", L_loop, 3, 0), PE("REVERSE", L_onoff, 2, 0),
         PE("WARP", L_onoff, 2, 0), P("SPEED", PW_EXP, PW_U_X, 0.25f, 4, 1), P("GRAIN", PW_EXP, PW_U_SEC, 0.01f, 0.2f, 0.06f), P("FINE", PW_LIN, PW_U_CENT, -50, 50, 0),
         P("SLICES", PW_INT, PW_U_NONE, 2, 16, 8), PE("SPLIT", L_sliceby, 2, 0), P("SENS", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("GLIDE", PW_LIN, PW_U_SEC, 0, 1, 0),
         P("ATTACK", PW_EXP, PW_U_SEC, 0.001f, 5, 0.002f), P("DECAY", PW_EXP, PW_U_SEC, 0.01f, 10, 1), P("SUSTAIN", PW_LIN, PW_U_PCT, 0, 1, 1), P("RELEASE", PW_EXP, PW_U_SEC, 0.005f, 10, 0.2f),
         P("CUTOFF", PW_EXP, PW_U_HZ, 20, 18000, 18000), P("RES", PW_LIN, PW_U_PCT, 0, 1, 0.1f), P("ENV AMT", PW_LIN, PW_U_PCT, -1, 1, 0), P("VEL", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("BEATS", PW_INT, PW_U_NONE, 0, 32, 0), PE("BANK", L_sbank, 5, 0)},
        SECONDS_TO_BYTES(PW_SAMPLER_SECONDS), 0, sampler_init, sampler_proc, sampler_order, L_sampler_pages},
    [PW_T_OUTPUT] = {"OUTPUT", "OUT", PW_C_IO, 2, 0, 2,
        {"L", "R"}, {0},
        (const pw_param_desc[]){P("LEVEL", PW_LIN, PW_U_PCT, 0, 1, 0.8f), PE("PROTECT", L_onoff, 2, 1)},
        0, 0, 0, output_proc, 0, 0},
    [PW_T_KEYS] = {"KEYS", "KEYS", PW_C_IO, 0, 6, 5,
        {0}, {"PITCH", "GATE", "VEL", "TRIG", "MOD", "PRESS"},
        (const pw_param_desc[]){P("GLIDE", PW_LIN, PW_U_SEC, 0, 1, 0), P("OCT", PW_INT, PW_U_NONE, -3, 3, 0),
         P("BEND", PW_INT, PW_U_SEMI, 0, 12, 2), P("VEL CRV", PW_LIN, PW_U_NONE, -1, 1, 0), P("PRESS", PW_LIN, PW_U_PCT, 0, 1, 0.5f)},
        0, 0, 0, keys_proc, 0, 0},
    [PW_T_GONE_FLANGER] = {"", "", PW_C_FX, 0, 0, 0, {0}, {0}, 0, 0, 0, 0, 0, 0, 0},
    [PW_T_GONE_PHASER] = {"", "", PW_C_FX, 0, 0, 0, {0}, {0}, 0, 0, 0, 0, 0, 0, 0},
    [PW_T_GONE_CHORUS] = {"", "", PW_C_FX, 0, 0, 0, {0}, {0}, 0, 0, 0, 0, 0, 0, 0},
    [PW_T_SWIRL] = {"MOD FX", "MODFX", PW_C_FX, 2, 2, 9,
        {"IN", "RATE"}, {"L", "R"},
        (const pw_param_desc[]){PE("TYPE", L_swirl, 3, 0), P("RATE", PW_EXP, PW_U_HZ, 0.02f, 10, 0.25f), P("DEPTH", PW_LIN, PW_U_PCT, 0, 1, 0.7f),
         P("MIX", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("FBK", PW_LIN, PW_U_PCT, -0.95f, 0.95f, 0.6f),
         P("DELAY", PW_EXP, PW_U_SEC, 0.0003f, 0.012f, 0.003f), P("FREQ", PW_EXP, PW_U_HZ, 100, 4000, 700), P("WIDTH", PW_LIN, PW_U_PCT, 0, 1, 0.8f), PE("SYNC", L_sync, SYNC_N, 0)},
        4112, 4112, swirl_init, swirl_proc, 0, 0},
    [PW_T_FM4] = {"FM-2", "FM2", PW_C_SOURCE, 5, 1, 8,
        {"V/OCT", "GATE", "INDEX", "FM", "PRESET"}, {"OUT"},
        (const pw_param_desc[]){PE("RATIO", L_ratio, 20, 5), P("INDEX", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("FBK", PW_LIN, PW_U_PCT, 0, 1, 0),
         P("TUNE", PW_LIN, PW_U_SEMI, -24, 24, 0),
         PE("AMP", L_fmenv, PW_FM_SHAPES, 3), PE("MOD", L_fmenv, PW_FM_SHAPES, 3),
         P("TIME", PW_EXP, PW_U_X, 0.25f, 4, 1),
         PE("PRESET", L_fmpre, 12, 0)},
        0, 0, fm4_init, fm4_proc, fm4_order, L_fm4_pages},
    [PW_T_LOOPER] = {"LOOPER", "LOOP", PW_C_FX, 4, 3, 8,
        {"IN", "REC", "CLEAR", "SPEED"}, {"OUT", "LOOP", "WRAP"},
        (const pw_param_desc[]){P("BEATS", PW_EXP, PW_U_NONE, 0.125f, 32, 8), P("FBK", PW_LIN, PW_U_PCT, 0, 1, 0.9f),
         P("SPEED", PW_LIN, PW_U_SEMI, -12, 12, 0), PE("REV", L_onoff, 2, 0), P("TONE", PW_LIN, PW_U_PCT, 0, 1, 1),
         P("LEVEL", PW_LIN, PW_U_PCT, 0, 1, 0.8f), PE("REC", L_onoff, 2, 0), P("DRY", PW_LIN, PW_U_PCT, 0, 1, 1)},
        LOOP_BYTES(PW_LOOP_SECONDS), LOOP_BYTES(0.5f), 0, looper_proc, 0, 0},
    [PW_T_GONE_REVERSER] = {"", "", PW_C_FX, 0, 0, 0, {0}, {0}, 0, 0, 0, 0, 0, 0, 0},
    [PW_T_DRUMS] = {"DRUM SYNTH", "DRUM", PW_C_SOURCE, 5, 1, 16,
        {"TRIG", "ACCENT", "PITCH", "DECAY", "PRESET"}, {"OUT"},
        (const pw_param_desc[]){P("TUNE", PW_EXP, PW_U_HZ, 20, 2000, 55), P("SWEEP", PW_LIN, PW_U_SEMI, -48, 48, 30),
         P("SWEEP T", PW_EXP, PW_U_SEC, 0.002f, 1, 0.035f), P("DECAY", PW_EXP, PW_U_SEC, 0.01f, 4, 0.4f),
         P("WAVE", PW_LIN, PW_U_PCT, 0, 1, 0), P("NOISE", PW_LIN, PW_U_PCT, 0, 1, 0),
         P("COLOR", PW_EXP, PW_U_HZ, 100, 12000, 3000), P("N DECAY", PW_EXP, PW_U_SEC, 0.005f, 2, 0.1f),
         P("FM", PW_LIN, PW_U_PCT, 0, 1, 0), P("RATIO", PW_EXP, PW_U_X, 0.5f, 8, 1.41f),
         P("LEVEL", PW_LIN, PW_U_PCT, 0, 1, 0.8f), P("DRIVE", PW_LIN, PW_U_PCT, 0, 1, 0.1f),
         PE("PRESET", L_drumpre, 49, 0), P("METAL", PW_LIN, PW_U_PCT, 0, 1, 0), P("BURST", PW_INT, PW_U_NONE, 0, 5, 0),
         P("MODES", PW_LIN, PW_U_PCT, 0, 1, 0)},
        0, 0, drums_init, drums_proc, drums_order, 0},
    [PW_T_ENVFOLLOW] = {"ENV FOLLOWER", "FOLLOW", PW_C_MOD, 1, 3, 4,
        {"IN"}, {"ENV", "INV", "GATE"},
        (const pw_param_desc[]){P("ATTACK", PW_EXP, PW_U_SEC, 0.001f, 0.5f, 0.005f), P("RELEASE", PW_EXP, PW_U_SEC, 0.005f, 2, 0.15f),
         P("GAIN", PW_EXP, PW_U_X, 0.25f, 16, 1), P("THRESH", PW_LIN, PW_U_PCT, 0, 1, 0.3f)},
        0, 0, 0, follow_proc, 0, 0},
    [PW_T_FRACTURE] = {"FRACTURE", "FRACT", PW_C_FX, 5, 1, 14,
        {"IN", "GATE", "CLOCK", "TIME", "CHANCE"}, {"OUT"},
        (const pw_param_desc[]){PE("SIZE", L_slice, 6, 2), P("CHANCE", PW_LIN, PW_U_PCT, 0, 1, 0.25f),
         P("REVERSE", PW_LIN, PW_U_PCT, 0, 1, 0.2f), P("PITCH", PW_LIN, PW_U_SEMI, -12, 12, 0),
         P("FADE", PW_LIN, PW_U_PCT, 0, 1, 0.3f), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1),
         PE("MODE", L_fracmode, 3, 0),
         P("TIME", PW_EXP, PW_U_SEC, 0.05f, PW_REVERSE_SECONDS, 0.4f), P("FBK", PW_LIN, PW_U_PCT, 0, 0.9f, 0),
         P("DEPTH", PW_INT, PW_U_NONE, 1, 16, 8), P("OCT", PW_LIN, PW_U_PCT, -1, 1, 0), P("STUTTER", PW_LIN, PW_U_PCT, 0, 1, 0.2f),
         P("DRY", PW_LIN, PW_U_PCT, 0, 1, 0), PE("SYNC", L_sync, SYNC_N, 0)},
        FRACT_MEM_IDEAL, FRACT_MEM_MIN, fract_init, fract_proc, fract_order, L_fract_pages},
    [PW_T_AUDIOIN] = {"AUDIO IN", "AUDIO", PW_C_IO, 0, 2, 1,
        {0}, {"L", "R"},
        (const pw_param_desc[]){P("GAIN", PW_LIN, PW_U_X, 0, 4, 1)},
        0, 0, 0, audioin_proc, 0, 0},
    [PW_T_EUCLID] = {"EUCLID", "EUCL", PW_C_SEQ, 2, 3, 4,
        {"CLOCK", "RESET"}, {"GATE", "TRIG", "INV"},
        (const pw_param_desc[]){P("STEPS", PW_INT, PW_U_NONE, 1, 32, 16), P("HITS", PW_INT, PW_U_NONE, 0, 32, 5),
         P("ROTATE", PW_INT, PW_U_NONE, 0, 31, 0), P("WIDTH", PW_LIN, PW_U_PCT, 0.05f, 1, 0.5f)},
        0, 0, 0, euclid_proc, 0, 0},
    [PW_T_STRING] = {"MODAL PERC", "MODAL", PW_C_SOURCE, 4, 1, 10,
        {"TRIG", "V/OCT", "EXCITE", "PRESET"}, {"OUT"},
        (const pw_param_desc[]){P("TUNE", PW_LIN, PW_U_SEMI, -24, 24, 0), P("DECAY", PW_LIN, PW_U_PCT, 0, 1, 0.85f),
         P("BRIGHT", PW_LIN, PW_U_PCT, 0, 1, 0.6f), P("STRIKE", PW_LIN, PW_U_PCT, 0, 1, 0.7f),
         PE("MODEL", L_resmodel, 12, 0), P("STRUCT", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("POS", PW_LIN, PW_U_PCT, 0, 1, 0.3f),
         PE("PRESET", L_respre, 25, 0), PE("EXCITER", L_resexc, 3, 0), P("BEND", PW_LIN, PW_U_SEMI, -24, 24, 0)},
        8208, 4112, string_init, string_proc, res_order, 0},
    [PW_T_VOWEL] = {"VOWEL FILTER", "VOWEL", PW_C_FILTER, 2, 1, 4,
        {"IN", "VOWEL"}, {"OUT"},
        (const pw_param_desc[]){P("VOWEL", PW_LIN, PW_U_NONE, 0, 4, 0), P("SHIFT", PW_LIN, PW_U_SEMI, -12, 12, 0),
         P("Q", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1)},
        0, 0, 0, vowel_proc, 0, 0},
    [PW_T_COMP] = {"COMPRESSOR", "COMP", PW_C_FX, 2, 2, 6,
        {"IN", "SIDE"}, {"OUT", "GR"},
        (const pw_param_desc[]){P("THRESH", PW_LIN, PW_U_DB, -40, 0, -18), P("RATIO", PW_EXP, PW_U_X, 1, 20, 4),
         P("ATTACK", PW_EXP, PW_U_SEC, 0.0001f, 0.1f, 0.005f), P("RELEASE", PW_EXP, PW_U_SEC, 0.01f, 2, 0.15f),
         P("MAKEUP", PW_LIN, PW_U_DB, 0, 24, 6), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1)},
        0, 0, 0, comp_proc, 0, 0},
    [PW_T_ARP] = {"ARPEGGIATOR", "ARP", PW_C_SEQ, 1, 3, 5,
        {"CLOCK"}, {"PITCH", "GATE", "TRIG"},
        (const pw_param_desc[]){PE("MODE", L_arpmode, 5, 0), PE("RATE", L_rate, 6, 3), P("OCT", PW_INT, PW_U_NONE, 1, 4, 1),
         P("GATE", PW_LIN, PW_U_PCT, 0.05f, 1, 0.5f), PE("LATCH", L_onoff, 2, 0)},
        0, 0, arp_init, arp_proc, 0, 0},
    [PW_T_TURING] = {"TURING", "TURNG", PW_C_SEQ, 3, 3, 4,
        {"CLOCK", "CHANCE", "LOCK"}, {"CV", "GATE", "TRIG"},
        (const pw_param_desc[]){P("LENGTH", PW_INT, PW_U_NONE, 2, 32, 8), P("CHANCE", PW_LIN, PW_U_PCT, 0, 1, 0.1f),
         P("RANGE", PW_LIN, PW_U_VOLT, 0, 5, 2), PE("LOCK", L_onoff, 2, 0)},
        0, 0, turing_init, turing_proc, 0, 0},
    [PW_T_STEREO] = {"MIXER", "MIX", PW_C_UTIL, 4, 2, 8,
        {"IN 1", "IN 2", "IN 3", "IN 4"}, {"L", "R"},
        (const pw_param_desc[]){P("LEVEL 1", PW_LIN, PW_U_PCT, 0, 1, 0.8f), P("LEVEL 2", PW_LIN, PW_U_PCT, 0, 1, 0.8f),
         P("LEVEL 3", PW_LIN, PW_U_PCT, 0, 1, 0.8f), P("LEVEL 4", PW_LIN, PW_U_PCT, 0, 1, 0.8f),
         P("PAN 1", PW_LIN, PW_U_NONE, -1, 1, 0), P("PAN 2", PW_LIN, PW_U_NONE, -1, 1, 0),
         P("PAN 3", PW_LIN, PW_U_NONE, -1, 1, 0), P("PAN 4", PW_LIN, PW_U_NONE, -1, 1, 0)},
        0, 0, 0, stereo_proc, 0, 0},
    [PW_T_PROLL] = {"PIANO ROLL", "ROLL", PW_C_SEQ, 0, 4, 3,
        {0}, {"PITCH", "GATE", "VEL", "TRIG"},
        (const pw_param_desc[]){P("BARS", PW_INT, PW_U_NONE, 1, 4, 2), PE("SNAP", L_snap, 4, 0), P("TRANSP", PW_INT, PW_U_SEMI, -24, 24, 0)},
        0, 0, 0, roll_proc, 0, 0},
    [PW_T_MIX8] = {"MIXER 8", "MIX8", PW_C_UTIL, 8, 2, 16,
        {"IN 1", "IN 2", "IN 3", "IN 4", "IN 5", "IN 6", "IN 7", "IN 8"}, {"L", "R"},
        (const pw_param_desc[]){P("LEVEL 1", PW_LIN, PW_U_PCT, 0, 1, 0.6f), P("LEVEL 2", PW_LIN, PW_U_PCT, 0, 1, 0.6f),
         P("LEVEL 3", PW_LIN, PW_U_PCT, 0, 1, 0.6f), P("LEVEL 4", PW_LIN, PW_U_PCT, 0, 1, 0.6f),
         P("LEVEL 5", PW_LIN, PW_U_PCT, 0, 1, 0.6f), P("LEVEL 6", PW_LIN, PW_U_PCT, 0, 1, 0.6f),
         P("LEVEL 7", PW_LIN, PW_U_PCT, 0, 1, 0.6f), P("LEVEL 8", PW_LIN, PW_U_PCT, 0, 1, 0.6f),
         P("PAN 1", PW_LIN, PW_U_NONE, -1, 1, 0), P("PAN 2", PW_LIN, PW_U_NONE, -1, 1, 0),
         P("PAN 3", PW_LIN, PW_U_NONE, -1, 1, 0), P("PAN 4", PW_LIN, PW_U_NONE, -1, 1, 0),
         P("PAN 5", PW_LIN, PW_U_NONE, -1, 1, 0), P("PAN 6", PW_LIN, PW_U_NONE, -1, 1, 0),
         P("PAN 7", PW_LIN, PW_U_NONE, -1, 1, 0), P("PAN 8", PW_LIN, PW_U_NONE, -1, 1, 0)},
        0, 0, 0, mix8_proc, 0, 0},
    [PW_T_CHAOS] = {"CHAOS", "CHAOS", PW_C_MOD, 1, 3, 3,
        {"RATE"}, {"X", "Y", "Z"},
        (const pw_param_desc[]){P("RATE", PW_EXP, PW_U_X, 0.05f, 400, 1), P("RHO", PW_LIN, PW_U_NONE, 10, 40, 28),
         P("LEVEL", PW_LIN, PW_U_PCT, 0, 1, 0.8f)},
        0, 0, chaos_init, chaos_proc, 0, 0},
    [PW_T_COIN] = {"PROBABILITY", "PROB", PW_C_UTIL, 2, 2, 2,
        {"TRIG", "CHANCE"}, {"A", "B"},
        (const pw_param_desc[]){P("CHANCE", PW_LIN, PW_U_PCT, 0, 1, 0.5f), PE("MODE", L_coin, 2, 0)},
        0, 0, coin_init, coin_proc, 0, 0},
    [PW_T_CHORD] = {"CHORD", "CHORD", PW_C_SEQ, 1, 4, 3,
        {"V/OCT"}, {"N1", "N2", "N3", "N4"},
        (const pw_param_desc[]){PE("TYPE", L_chord, 10, 0), P("INVERT", PW_INT, PW_U_NONE, 0, 3, 0), PE("SPREAD", L_onoff, 2, 0)},
        0, 0, 0, chord_proc, 0, 0},
    [PW_T_FORMANT] = {"FORMANT OSC", "FORM", PW_C_SOURCE, 3, 1, 4,
        {"V/OCT", "VOWEL", "FORM"}, {"OUT"},
        (const pw_param_desc[]){P("TUNE", PW_LIN, PW_U_SEMI, -36, 36, 0), P("VOWEL", PW_LIN, PW_U_NONE, 0, 4, 0),
         P("WIDTH", PW_LIN, PW_U_PCT, 0, 1, 0.35f), P("FORMANT", PW_LIN, PW_U_SEMI, -24, 24, 0)},
        0, 0, 0, formant_proc, 0, 0},
    [PW_T_COMB] = {"COMB FILTER", "COMB", PW_C_FILTER, 3, 1, 4,
        {"IN", "V/OCT", "FBK"}, {"OUT"},
        (const pw_param_desc[]){P("FREQ", PW_EXP, PW_U_HZ, 20, 5000, 220), P("FBK", PW_LIN, PW_U_PCT, -0.995f, 0.995f, 0.85f),
         P("DAMP", PW_LIN, PW_U_PCT, 0, 1, 0.2f), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1)},
        2400u * 2u + 16u, 600u * 2u + 16u, 0, comb_proc, 0, 0},
    [PW_T_EQ] = {"EQUALIZER", "EQ", PW_C_FILTER, 2, 1, 7,
        {"IN", "MID F"}, {"OUT"},
        (const pw_param_desc[]){P("LOW", PW_LIN, PW_U_DB, -15, 15, 0), P("MID", PW_LIN, PW_U_DB, -15, 15, 0), P("HIGH", PW_LIN, PW_U_DB, -15, 15, 0),
         P("MID F", PW_EXP, PW_U_HZ, 100, 10000, 1000), P("LOW F", PW_EXP, PW_U_HZ, 30, 600, 150),
         P("HIGH F", PW_EXP, PW_U_HZ, 1500, 16000, 5000), P("Q", PW_EXP, PW_U_NONE, 0.3f, 10, 1)},
        0, 0, 0, eq_proc, 0, 0},
    [PW_T_PSHIFT] = {"PITCH SHIFTER", "SHIFT", PW_C_FX, 2, 1, 4,
        {"IN", "SHIFT"}, {"OUT"},
        (const pw_param_desc[]){P("SHIFT", PW_LIN, PW_U_SEMI, -24, 24, 12), P("FINE", PW_LIN, PW_U_CENT, -50, 50, 0),
         P("WINDOW", PW_EXP, PW_U_SEC, 0.02f, 0.1f, 0.05f), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1)},
        SECONDS_TO_BYTES(0.11f), SECONDS_TO_BYTES(0.03f), 0, pshift_proc, 0, 0},
    [PW_T_WAVESET] = {"WAVESET", "WVSET", PW_C_FX, 3, 1, 4,
        {"IN", "AMOUNT", "FREEZE"}, {"OUT"},
        (const pw_param_desc[]){PE("MODE", L_wsmode, 10, 0), P("GROUP", PW_INT, PW_U_NONE, 1, 8, 2), P("AMOUNT", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1)},
        SECONDS_TO_BYTES(0.15f), SECONDS_TO_BYTES(0.06f), 0, waveset_proc, 0, 0},
    [PW_T_MUTATE] = {"MUTATE", "MUTATE", PW_C_MOD, 3, 1, 2,
        {"NUDGE", "ROLL", "AMOUNT"}, {"TRIG"},
        (const pw_param_desc[]){P("AMOUNT", PW_LIN, PW_U_PCT, 0, 1, 0.25f), P("DENSITY", PW_LIN, PW_U_PCT, 0, 1, 0.3f)},
        0, 0, 0, mutate_proc, 0, 0},
    [PW_T_TRIGSEQ] = {"TRIG SEQ", "TRIG", PW_C_SEQ, 6, 6, 17,
        {"CLOCK", "RESET", "FILL", "CHANCE", "SHIFT", "SWING"}, {"A", "B", "C", "D", "ACC", "RND"},
        (const pw_param_desc[]){PE("RATE", L_trate, 3, 1), P("SWING", PW_LIN, PW_U_PCT, 0, 0.5f, 0), P("ROLL", PW_INT, PW_U_NONE, 2, 4, 3),
         PE("FILL", L_fill, 3, 0),
         P("LEN A", PW_INT, PW_U_NONE, 1, 16, 16), P("LEN B", PW_INT, PW_U_NONE, 1, 16, 16),
         P("LEN C", PW_INT, PW_U_NONE, 1, 16, 16), P("LEN D", PW_INT, PW_U_NONE, 1, 16, 16),
         P("PROB A", PW_LIN, PW_U_PCT, 0, 1, 1), P("PROB B", PW_LIN, PW_U_PCT, 0, 1, 1),
         P("PROB C", PW_LIN, PW_U_PCT, 0, 1, 1), P("PROB D", PW_LIN, PW_U_PCT, 0, 1, 1),
         P("SHIFT A", PW_INT, PW_U_NONE, 0, 15, 0), P("SHIFT B", PW_INT, PW_U_NONE, 0, 15, 0),
         P("SHIFT C", PW_INT, PW_U_NONE, 0, 15, 0), P("SHIFT D", PW_INT, PW_U_NONE, 0, 15, 0),
         PE("MONITOR", L_onoff, 2, 1)},
        0, 0, tseq_init, tseq_proc, 0, 0},
    [PW_T_DRUMRACK] = {"DRUM RACK", "RACK", PW_C_SOURCE, 10, 6, 37,
        {"T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8", "ACC", "PRESET"}, {"L", "R", "1", "2", "3", "4"},
        (const pw_param_desc[]){P("VOLUME", PW_LIN, PW_U_PCT, 0, 1, 0.8f), PE("CHOKE", L_choke, 3, 1), PE("KEYS", L_onoff, 2, 0), P("ACCENT", PW_LIN, PW_U_PCT, 0, 1, 0.5f),
         PE("SOUND 1", L_rack, 104, 0), P("TUNE 1", PW_LIN, PW_U_SEMI, -24, 24, 0), P("DECAY 1", PW_EXP, PW_U_SEC, 0.02f, 4, 0.45f), P("LEVEL 1", PW_LIN, PW_U_PCT, 0, 1, 0.8f),
         PE("SOUND 2", L_rack, 104, 1), P("TUNE 2", PW_LIN, PW_U_SEMI, -24, 24, 0), P("DECAY 2", PW_EXP, PW_U_SEC, 0.02f, 4, 0.18f), P("LEVEL 2", PW_LIN, PW_U_PCT, 0, 1, 0.8f),
         PE("SOUND 3", L_rack, 104, 2), P("TUNE 3", PW_LIN, PW_U_SEMI, -24, 24, 0), P("DECAY 3", PW_EXP, PW_U_SEC, 0.02f, 4, 0.25f), P("LEVEL 3", PW_LIN, PW_U_PCT, 0, 1, 0.8f),
         PE("SOUND 4", L_rack, 104, 3), P("TUNE 4", PW_LIN, PW_U_SEMI, -24, 24, 0), P("DECAY 4", PW_EXP, PW_U_SEC, 0.02f, 4, 0.06f), P("LEVEL 4", PW_LIN, PW_U_PCT, 0, 1, 0.8f),
         PE("SOUND 5", L_rack, 104, 4), P("TUNE 5", PW_LIN, PW_U_SEMI, -24, 24, 0), P("DECAY 5", PW_EXP, PW_U_SEC, 0.02f, 4, 0.45f), P("LEVEL 5", PW_LIN, PW_U_PCT, 0, 1, 0.8f),
         PE("SOUND 6", L_rack, 104, 5), P("TUNE 6", PW_LIN, PW_U_SEMI, -24, 24, 0), P("DECAY 6", PW_EXP, PW_U_SEC, 0.02f, 4, 0.35f), P("LEVEL 6", PW_LIN, PW_U_PCT, 0, 1, 0.8f),
         PE("SOUND 7", L_rack, 104, 6), P("TUNE 7", PW_LIN, PW_U_SEMI, -24, 24, 0), P("DECAY 7", PW_EXP, PW_U_SEC, 0.02f, 4, 0.05f), P("LEVEL 7", PW_LIN, PW_U_PCT, 0, 1, 0.8f),
         PE("SOUND 8", L_rack, 104, 7), P("TUNE 8", PW_LIN, PW_U_SEMI, -24, 24, 0), P("DECAY 8", PW_EXP, PW_U_SEC, 0.02f, 4, 0.2f), P("LEVEL 8", PW_LIN, PW_U_PCT, 0, 1, 0.8f),
         PE("KIT", L_kit, 13, 0)},
        0, 0, rack_init, rack_proc, rack_order, L_rack_pages},
    [PW_T_MIDIIN] = {"MIDI IN", "MIDIN", PW_C_IO, 0, 6, 5,
        {0}, {"CC A", "CC B", "CC C", "CC D", "CLOCK", "RUN"},
        (const pw_param_desc[]){PE("CHANNEL", L_mch, 17, 0), P("CC A", PW_INT, PW_U_NONE, 0, 127, 1), P("CC B", PW_INT, PW_U_NONE, 0, 127, 74),
         P("CC C", PW_INT, PW_U_NONE, 0, 127, 71), P("CC D", PW_INT, PW_U_NONE, 0, 127, 7)},
        0, 0, 0, midiin_proc, 0, 0},
    [PW_T_MIDIOUT] = {"MIDI OUT", "MIDOUT", PW_C_IO, 5, 0, 4,
        {"PITCH", "GATE", "VEL", "CC A", "CC B"}, {0},
        (const pw_param_desc[]){P("CHANNEL", PW_INT, PW_U_NONE, 1, 16, 1), P("CC A", PW_INT, PW_U_NONE, 0, 127, 74), P("CC B", PW_INT, PW_U_NONE, 0, 127, 71),
         PE("CLOCK", L_onoff, 2, 0)},
        0, 0, midiout_init, midiout_proc, 0, 0},
    [PW_T_CVREC] = {"CV RECORDER", "CVREC", PW_C_MOD, 4, 2, 7,
        {"IN", "REC", "CLOCK", "RESET"}, {"OUT", "EOC"},
        (const pw_param_desc[]){PE("MODE", L_cvrmode, 3, 0), P("LENGTH", PW_INT, PW_U_NONE, 1, 16, 4), P("KNOB", PW_LIN, PW_U_VOLT, -5, 5, 0),
         P("SMOOTH", PW_LIN, PW_U_PCT, 0, 1, 0.3f), PE("SPEED", L_cvrspeed, 4, 1), P("LEVEL", PW_LIN, PW_U_PCT, -1, 1, 1),
         P("OFFSET", PW_LIN, PW_U_VOLT, -5, 5, 0)},
        0, 0, cvr_init, cvr_proc, 0, L_cvr_pages},
    [PW_T_GRAIN] = {"GRANULAR", "GRAIN", PW_C_FX, 5, 2, 11,
        {"IN", "FREEZE", "POS", "V/OCT", "TRIG"}, {"L", "R"},
        (const pw_param_desc[]){P("POS", PW_LIN, PW_U_PCT, 0, 1, 0.3f), P("SIZE", PW_EXP, PW_U_SEC, 0.01f, 0.5f, 0.12f), P("DENSITY", PW_EXP, PW_U_HZ, 1, 80, 14),
         P("PITCH", PW_LIN, PW_U_SEMI, -24, 24, 0), P("SPREAD", PW_LIN, PW_U_PCT, 0, 1, 0.25f), P("WIDTH", PW_LIN, PW_U_PCT, 0, 1, 0.6f),
         PE("FREEZE", L_onoff, 2, 0), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 0.7f), P("SOURCE", PW_INT, PW_U_NONE, 0, 48, 0),
         P("DETUNE", PW_LIN, PW_U_PCT, 0, 1, 0.1f), PE("SYNC", L_sync, SYNC_N, 0)},
        SECONDS_TO_BYTES(PW_GRAIN_SECONDS), SECONDS_TO_BYTES(0.2f), gran_init, gran_proc, 0, L_gran_pages},
    [PW_T_TAPE] = {"TAPE", "TAPE", PW_C_FX, 4, 1, 8,
        {"IN", "STOP", "WOW", "SPEED"}, {"OUT"},
        (const pw_param_desc[]){P("DRIVE", PW_LIN, PW_U_PCT, 0, 1, 0.3f), P("WOW", PW_LIN, PW_U_PCT, 0, 1, 0.2f), P("FLUTTER", PW_LIN, PW_U_PCT, 0, 1, 0.15f),
         P("AGE", PW_LIN, PW_U_PCT, 0, 1, 0.3f), P("HISS", PW_LIN, PW_U_PCT, 0, 1, 0.1f), P("STOP", PW_EXP, PW_U_SEC, 0.1f, 3, 0.8f),
         P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1), P("SPEED", PW_LIN, PW_U_SEMI, -12, 12, 0)},
        SECONDS_TO_BYTES(PW_TAPE_SECONDS), SECONDS_TO_BYTES(0.03f), tape_init, tape_proc, tape_order, 0},
    [PW_T_SCOPE] = {"SCOPE", "SCOPE", PW_C_UTIL, 2, 0, 4,
        {"A", "B"}, {0},
        (const pw_param_desc[]){PE("ZOOM", L_zoom, 4, 1), PE("V/DIV", L_vdiv, 3, 1), PE("TRIG", L_strig, 3, 0), PE("MODE", L_scopemode, 2, 0)},
        PW_SCOPE_RING * 4u + 16u, PW_SCOPE_RING * 4u + 16u, 0, scope_proc, 0, 0},
    [PW_T_GATEDLY] = {"GATE DELAY", "GDLY", PW_C_SEQ, 4, 2, 4,
        {"A", "B", "CV A", "CV B"}, {"A", "B"},
        (const pw_param_desc[]){P("TIME A", PW_LIN, PW_U_SEC, 0, 2, 0.25f), P("TIME B", PW_LIN, PW_U_SEC, 0, 2, 0.5f),
         PE("SYNC A", L_sync, SYNC_N, 0), PE("SYNC B", L_sync, SYNC_N, 0)},
        0, 0, 0, gatedly_proc, 0, 0},
    [PW_T_ADDITIVE] = {"ADDITIVE", "ADDTV", PW_C_SOURCE, 6, 2, 9,
        {"V/OCT", "GATE", "BRIGHT", "PEAK", "COMB", "PRESET"}, {"L", "R"},
        (const pw_param_desc[]){P("TUNE", PW_LIN, PW_U_SEMI, -36, 36, 0), P("SHAPE", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("BRIGHT", PW_LIN, PW_U_PCT, 0, 1, 0.6f), P("PLUCK", PW_LIN, PW_U_PCT, 0, 1, 0),
         P("PEAK", PW_LIN, PW_U_PCT, 0, 1, 0.45f), P("RES", PW_LIN, PW_U_PCT, 0, 1, 0), P("COMB", PW_LIN, PW_U_PCT, 0, 1, 0), P("STRETCH", PW_LIN, PW_U_PCT, -1, 1, 0),
         PE("PRESET", L_addpre, 11, 0)},
        0, 0, 0, additive_proc, addtv_order, L_add_pages},
    [PW_T_MACRO] = {"MACRO OSC", "MACRO", PW_C_SOURCE, 7, 2, 9,
        {"V/OCT", "TRIG", "HARM", "TIMBRE", "MORPH", "MODEL", "PRESET"}, {"OUT", "AUX"},
        (const pw_param_desc[]){PE("MODEL", L_macro, 8, 0), P("TUNE", PW_LIN, PW_U_SEMI, -36, 36, 0), P("HARM", PW_LIN, PW_U_PCT, 0, 1, 0.3f), P("TIMBRE", PW_LIN, PW_U_PCT, 0, 1, 0.4f),
         P("MORPH", PW_LIN, PW_U_PCT, 0, 1, 0.3f), P("DECAY", PW_EXP, PW_U_SEC, 0.02f, 4, 0.4f), P("COLOR", PW_LIN, PW_U_PCT, 0, 1, 0.7f), P("FINE", PW_LIN, PW_U_CENT, -100, 100, 0),
         PE("PRESET", L_macpre, 11, 0)},
        0, 0, 0, macro_proc, mac_order, L_mac_pages},
    [PW_T_GONE_CLOCKWARP] = {"", "", PW_C_FX, 0, 0, 0, {0}, {0}, 0, 0, 0, 0, 0, 0, 0},
    [PW_T_MICROPERC] = {"MICRO PERC", "MICRO", PW_C_SOURCE, 3, 2, 6,
        {"TRIG", "PITCH", "SCATTER"}, {"OUT", "RND"},
        (const pw_param_desc[]){PE("MODEL", L_micro, 6, 0), P("PITCH", PW_LIN, PW_U_SEMI, -36, 36, 0), P("DECAY", PW_EXP, PW_U_SEC, 0.002f, 1, 0.04f),
         P("TONE", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("SCATTER", PW_LIN, PW_U_PCT, 0, 1, 0.3f), P("LEVEL", PW_LIN, PW_U_PCT, 0, 1, 0.8f)},
        0, 0, 0, microperc_proc, 0, 0},
    [PW_T_GONE_SHUFFLER] = {"", "", PW_C_FX, 0, 0, 0, {0}, {0}, 0, 0, 0, 0, 0, 0, 0},
    [PW_T_GONE_PDOSC] = {"", "", PW_C_FX, 0, 0, 0, {0}, {0}, 0, 0, 0, 0, 0, 0, 0},
    [PW_T_GONE_FMTX] = {"", "", PW_C_FX, 0, 0, 0, {0}, {0}, 0, 0, 0, 0, 0, 0, 0},
    [PW_T_LIMITER] = {"LIMITER", "LIMIT", PW_C_FX, 2, 2, 7,
        {"IN", "DUCK"}, {"OUT", "GR"},
        (const pw_param_desc[]){P("DRIVE", PW_LIN, PW_U_DB, 0, 24, 6), P("CEILING", PW_LIN, PW_U_DB, -24, 0, -1),
         P("RELEASE", PW_EXP, PW_U_SEC, 0.005f, 1, 0.08f), PE("MODE", L_limit, 3, 0),
         P("PUMP", PW_LIN, PW_U_PCT, 0, 1, 0), P("RECOVER", PW_EXP, PW_U_SEC, 0.05f, 1, 0.25f), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1)},
        0, 0, 0, limiter_proc, 0, 0},
    [PW_T_OTT] = {"OTT", "OTT", PW_C_FX, 1, 1, 8,
        {"IN"}, {"OUT"},
        (const pw_param_desc[]){P("DEPTH", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("TIME", PW_LIN, PW_U_PCT, 0, 1, 0.4f), P("SKEW", PW_LIN, PW_U_PCT, -1, 1, 0),
         P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1), P("LOW", PW_LIN, PW_U_DB, -12, 12, 0), P("MID", PW_LIN, PW_U_DB, -12, 12, 0),
         P("HIGH", PW_LIN, PW_U_DB, -12, 12, 0), P("OUT", PW_LIN, PW_U_DB, -12, 12, 0)},
        0, 0, 0, ott_proc, 0, 0},
    [PW_T_MARKOV] = {"MARKOV", "MARKV", PW_C_SEQ, 4, 3, 11,
        {"CLOCK", "RESET", "LEARN", "TRANSP"}, {"PITCH", "GATE", "TRIG"},
        (const pw_param_desc[]){PE("SCALE", scale_names, 12, 8), PE("ROOT", note_names, 12, 0), P("RANGE", PW_INT, PW_U_NONE, 1, 3, 2),
         P("STEP", PW_LIN, PW_U_PCT, 0, 1, 0.6f), P("HOME", PW_LIN, PW_U_PCT, 0, 1, 0.4f), P("REPEAT", PW_LIN, PW_U_PCT, 0, 1, 0.15f),
         P("REST", PW_LIN, PW_U_PCT, 0, 1, 0.1f), P("GATE", PW_LIN, PW_U_PCT, 0.05f, 1, 0.6f), P("LEARN", PW_LIN, PW_U_PCT, 0, 1, 0),
         P("OCTAVE", PW_INT, PW_U_NONE, -3, 3, 0), P("DIV", PW_INT, PW_U_DIV, -6, 8, 1)},
        0, 0, mk_init, markov_proc, 0, 0},
    [PW_T_XFADE] = {"XFADE", "XFADE", PW_C_UTIL, 3, 1, 3,
        {"A", "B", "MIX"}, {"OUT"},
        (const pw_param_desc[]){P("MIX", PW_LIN, PW_U_PCT, 0, 1, 0.5f), PE("CURVE", L_xfcurve, 2, 0), P("CV AMT", PW_LIN, PW_U_PCT, -1, 1, 1)},
        0, 0, 0, xfade_proc, 0, 0},
    [PW_T_SNAPSHOT] = {"SNAPSHOT", "SNAPSH", PW_C_UTIL, 2, 2, 5,
        {"CV", "CLK"}, {"SCENE", "CHANGE"},
        (const pw_param_desc[]){P("SCENES", PW_INT, PW_U_NONE, 2, 16, 4), PE("RANGE", L_snrange, 2, 0), P("HYST", PW_LIN, PW_U_PCT, 0, 1, 0.2f),
         PE("CHANGE", L_snchg, 3, 0), P("MORPH", PW_LIN, PW_U_SEC, 0, 4, 0)},
        0, 0, 0, snap_proc, 0, 0},
    [PW_T_INERTIA] = {"INERTIA", "INERT", PW_C_MOD, 7, 3, 6,
        {"CV", "IMPULS", "RESET", "MASS", "FRICTN", "SPRING", "FORCE"}, {"POS", "VEL", "ACC"},
        (const pw_param_desc[]){P("MASS", PW_LIN, PW_U_PCT, 0, 1, 0.4f), P("FRICTN", PW_LIN, PW_U_PCT, 0, 1, 0.25f), P("SPRING", PW_LIN, PW_U_PCT, 0, 1, 0.5f),
         P("FORCE", PW_LIN, PW_U_PCT, 0, 1, 0.5f), PE("MODE", L_inemode, 3, 0), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1)},
        0, 0, 0, inertia_proc, 0, 0},
    [PW_T_BURST] = {"BURST", "BURST", PW_C_SEQ, 3, 3, 6,
        {"TRIG", "COUNT", "SPEED"}, {"TRIG", "LEVEL", "PITCH"},
        (const pw_param_desc[]){P("COUNT", PW_INT, PW_U_NONE, 1, 32, 8), P("SPEED", PW_EXP, PW_U_HZ, 1, 200, 20), P("CURVE", PW_LIN, PW_U_PCT, -1, 1, 0.5f),
         P("JITTER", PW_LIN, PW_U_PCT, 0, 1, 0), P("PITCH", PW_INT, PW_U_SEMI, -12, 12, 0), PE("SYNC", L_sync, SYNC_N, 0)},
        0, 0, 0, burst_proc, 0, 0},
    [PW_T_SPACE] = {"SPACE", "SPACE", PW_C_FX, 2, 2, 3,
        {"IN L", "IN R"}, {"L", "R"},
        (const pw_param_desc[]){P("SIZE", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("TONE", PW_LIN, PW_U_PCT, 0, 1, 0.6f), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 0.25f)},
        RM_BYTES, RM_BYTES, 0, space_proc, 0, 0},
    [PW_T_ROOM] = {"ROOM MODES", "ROOM", PW_C_FX, 2, 2, 5,
        {"IN", "V/OCT"}, {"L", "R"},
        (const pw_param_desc[]){P("ROOT", PW_EXP, PW_U_HZ, 30, 400, 80), P("SHAPE", PW_LIN, PW_U_PCT, 0, 1, 0.5f), P("MODES", PW_INT, PW_U_NONE, 1, 8, 6), P("DECAY", PW_EXP, PW_U_SEC, 0.2f, 12, 3),
         P("MIX", PW_LIN, PW_U_PCT, 0, 1, 0.5f)},
        0, 0, 0, room_proc, 0, 0},
    [PW_T_CZ] = {"CZ", "CZ", PW_C_SOURCE, 3, 1, 28,
        {"V/OCT", "GATE", "DCW"}, {"OUT"},
        (const pw_param_desc[]){P("BANK", PW_INT, PW_U_NONE, 0, 4, 0), P("PRESET", PW_INT, PW_U_NONE, 1, 128, 1), P("LEVEL", PW_LIN, PW_U_PCT, 0, 1, 0.8f), P("TUNE", PW_LIN, PW_U_SEMI, -24, 24, 0),
         PE("LINE", L_czline, 4, 0), PE("MOD", L_czmod, 3, 0), P("DETUNE", PW_LIN, PW_U_SEMI, -12, 12, 0), PE("OCTAVE", L_czoct, 3, 1),
         PE("WAVE 1", L_czwave, 8, 0), PE("WAVE 2", L_czwave2, 9, 0), P("DCW", PW_LIN, PW_U_PCT, 0, 1, 0.6f), P("FOLLOW", PW_LIN, PW_U_X, 0, 2, 1),
         P("D.ATK", PW_EXP, PW_U_SEC, 0.001f, 4, 0.005f), P("D.DEC", PW_EXP, PW_U_SEC, 0.01f, 8, 0.6f), P("D.SUS", PW_LIN, PW_U_PCT, 0, 1, 0.3f), P("D.REL", PW_EXP, PW_U_SEC, 0.01f, 8, 0.3f),
         P("A.ATK", PW_EXP, PW_U_SEC, 0.001f, 4, 0.005f), P("A.DEC", PW_EXP, PW_U_SEC, 0.01f, 8, 1.0f), P("A.SUS", PW_LIN, PW_U_PCT, 0, 1, 0.7f), P("A.REL", PW_EXP, PW_U_SEC, 0.01f, 8, 0.3f),
         P("PIT AMT", PW_LIN, PW_U_SEMI, 0, 24, 0), P("PIT TIM", PW_EXP, PW_U_SEC, 0.005f, 2, 0.1f), P("VIB", PW_LIN, PW_U_PCT, 0, 1, 0), P("V RATE", PW_EXP, PW_U_X, 0.25f, 4, 1),
         P("BRIGHT", PW_LIN, PW_U_X, 0, 2, 1), P("SPEED", PW_EXP, PW_U_X, 0.25f, 4, 1), P("AMP SPD", PW_EXP, PW_U_X, 0.25f, 4, 1), P("REL", PW_EXP, PW_U_X, 0.25f, 4, 1)},
        0, 0, cz_init, cz_proc, 0, L_cz_pages},
    [PW_T_FMDX] = {"FM-DX", "FMDX", PW_C_SOURCE, 4, 1, 42,
        {"V/OCT", "GATE", "INDEX", "FM"}, {"OUT"},
        (const pw_param_desc[]){P("BANK", PW_INT, PW_U_NONE, 0, 4, 0), P("PRESET", PW_INT, PW_U_NONE, 1, 128, 1), P("LEVEL", PW_LIN, PW_U_PCT, 0, 1, 0.8f), P("TUNE", PW_LIN, PW_U_SEMI, -24, 24, 0),
         PE("ALGO", L_dxalg, 8, 0), P("FBK", PW_INT, PW_U_NONE, 0, 7, 0), P("VIB", PW_INT, PW_U_NONE, 0, 99, 0), P("TREM", PW_INT, PW_U_NONE, 0, 99, 0),
         P("LFO", PW_INT, PW_U_NONE, 0, 99, 35), PE("LFO WAV", L_dxlfw, 4, 2),
#define DXOPK(r, lv) PE("RATIO", L_dxratio, 64, r), P("LEVEL", PW_INT, PW_U_NONE, 0, 99, lv), P("ATTACK", PW_INT, PW_U_NONE, 0, 31, 31), P("DECAY 1", PW_INT, PW_U_NONE, 0, 31, 12), \
         P("SUS LVL", PW_INT, PW_U_NONE, 0, 15, 15), P("DECAY 2", PW_INT, PW_U_NONE, 0, 31, 0), P("RELEASE", PW_INT, PW_U_NONE, 1, 15, 7), PE("WAVE", L_dxwave, 8, 0)
         DXOPK(4, 99), DXOPK(4, 0), DXOPK(4, 0), DXOPK(4, 0)},
        0, 0, dx_init, dx_proc, dx_order, L_dx_pages},
    [PW_T_PARAMMOD] = {"PARAM MOD", "PMOD", PW_C_MOD, 4, 0, 16,
        {"CV 1", "CV 2", "CV 3", "CV 4"}, {0},
        (const pw_param_desc[]){
         P("MODULE", PW_INT, PW_U_NONE, 0, PW_MAX_MODULES, 0), P("PARAM", PW_INT, PW_U_NONE, 0, PW_MAX_PARAM - 1, 0), P("DEPTH", PW_LIN, PW_U_PCT, -1, 1, 0.5f), P("SMOOTH", PW_LIN, PW_U_PCT, 0, 1, 0.2f),
         P("MODULE", PW_INT, PW_U_NONE, 0, PW_MAX_MODULES, 0), P("PARAM", PW_INT, PW_U_NONE, 0, PW_MAX_PARAM - 1, 0), P("DEPTH", PW_LIN, PW_U_PCT, -1, 1, 0.5f), P("SMOOTH", PW_LIN, PW_U_PCT, 0, 1, 0.2f),
         P("MODULE", PW_INT, PW_U_NONE, 0, PW_MAX_MODULES, 0), P("PARAM", PW_INT, PW_U_NONE, 0, PW_MAX_PARAM - 1, 0), P("DEPTH", PW_LIN, PW_U_PCT, -1, 1, 0.5f), P("SMOOTH", PW_LIN, PW_U_PCT, 0, 1, 0.2f),
         P("MODULE", PW_INT, PW_U_NONE, 0, PW_MAX_MODULES, 0), P("PARAM", PW_INT, PW_U_NONE, 0, PW_MAX_PARAM - 1, 0), P("DEPTH", PW_LIN, PW_U_PCT, -1, 1, 0.5f), P("SMOOTH", PW_LIN, PW_U_PCT, 0, 1, 0.2f)},
        0, 0, 0, pmod_proc, 0, L_pmod_pages},
    [PW_T_BUTTON] = {"BUTTON", "BUTTON", PW_C_UTIL, 1, 4, 8,
        {"CV IN"}, {"OUT", "TRIG", "HOLD", "DBL"},
        (const pw_param_desc[]){P("FROM", PW_INT, PW_U_NONE, 1, 27, 1), P("TO", PW_INT, PW_U_NONE, 1, 27, 27), PE("MODE", L_btnmode, 7, 0),
         P("VALUE", PW_LIN, PW_U_PCT, -1, 1, 1), P("TIME", PW_EXP, PW_U_SEC, 0.005f, 2, 0.05f), P("COUNT", PW_INT, PW_U_NONE, 1, 16, 4),
         P("HOLD", PW_EXP, PW_U_SEC, 0.1f, 3, 0.5f), P("DBL", PW_EXP, PW_U_SEC, 0.05f, 1, 0.3f)},
        0, 0, 0, button_proc, 0, 0},
    [PW_T_GENDY] = {"GENDY", "GENDY", PW_C_SOURCE, 2, 2, 8,
        {"V/OCT", "RESET"}, {"OUT", "CYCLE"},
        (const pw_param_desc[]){P("PITCH", PW_INT, PW_U_SEMI, -36, 36, 0), P("POINTS", PW_INT, PW_U_NONE, 3, 16, 12), P("A CHAOS", PW_LIN, PW_U_PCT, 0, 1, 0.3f), P("T CHAOS", PW_LIN, PW_U_PCT, 0, 1, 0.3f),
         P("SMOOTH", PW_LIN, PW_U_PCT, 0, 1, 0.3f), P("LEVEL", PW_LIN, PW_U_PCT, 0, 1, 0.6f), PE("DIST", L_gdist, 5, 0), PE("RANGE", L_grange, 3, 0)},
        0, 0, gendy_init, gendy_proc, 0, 0},
    [PW_T_VOCODER] = {"VOCODER", "VOCOD", PW_C_FX, 2, 1, 6,
        {"CARRIER", "MOD"}, {"OUT"},
        (const pw_param_desc[]){P("RELEASE", PW_EXP, PW_U_SEC, 0.01f, 0.5f, 0.06f), P("SHIFT", PW_INT, PW_U_NONE, -6, 6, 0), P("FOCUS", PW_EXP, PW_U_X, 1.5f, 8, 4),
         P("BRIGHT", PW_LIN, PW_U_PCT, -1, 1, 0.3f), P("GAIN", PW_EXP, PW_U_X, 0.25f, 16, 4), P("MIX", PW_LIN, PW_U_PCT, 0, 1, 1)},
        0, 0, 0, voc_proc, 0, 0},
};

unsigned pw_scale_mask(unsigned scale) { return scale < 12 ? scale_mask[scale] : scale_mask[0]; }
int pw_has_data(unsigned type) { return type == PW_T_SEQ || type == PW_T_MODSEQ || type == PW_T_CHORDSTEPS || type == PW_T_GRAVITY || type == PW_T_TRIGSEQ || type == PW_T_SAMPLER || type == PW_T_CVREC || type == PW_T_PROLL; }
PW_COLD float pw_live(unsigned slot, unsigned what)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1) return 0;
    const pw_module *m = &pws.mod[slot];
    const void *st = m->st.raw;
    switch (m->type) {
    case PW_T_VOCODER: return what < (unsigned)VOC_N ? pw_clamp(((const voc_st *)st)->env[what] * 0.314f, 0.0f, 1.0f) : 0.0f;   /* 0..11: how loud band k's modulator is (0..1) */
    case PW_T_MUTATE: { const mut_st *q = (const mut_st *)st; return what == 0 ? q->nc : what == 1 ? q->rc : q->amt; }
    case PW_T_ENV: { const env_st *e = (const env_st *)st; return what == 0 ? e->v : (float)e->stage; }
    case PW_T_FM4: return what < 2 ? ((const fm4_st *)st)->env[what] : what < 4 ? (float)((const fm4_st *)st)->stage[what - 2] : 0;
    case PW_T_FMDX: {   /* 0-3: how loudly OP 1-4's envelope stands (0-1); 4-7: its stage (0 attack .. 4 reverb) */
        const dx_st *q = (const dx_st *)st;
        if (what >= 8u) return 0;
        const dx_opst *o = &q->c.o[3u - (what & 3u)];
        return what < 4u ? (float)dx_a2v((uint32_t)o->att << 2) * (1.0f / 8168.0f) : (float)o->st;
    }
    case PW_T_TRIGSEQ: return what < 4 ? (float)((const tseq_st *)st)->idx[what] : -1;
    case PW_T_DRUMRACK: return what < 8 && ((const rack_st *)st)->pad[what].on ? ((const rack_st *)st)->pad[what].env : 0;
    case PW_T_SAMPLER: {
        const samp_st *q = (const samp_st *)st;
        pw_smp sm;
        if (!q->playing || !samp_source(m, q, &sm) || !sm.frames) return -1;
        return what == 0 ? q->pos / (float)sm.frames : q->env;
    }
    case PW_T_EUCLID: return (float)((const eucl_st *)st)->step;
    case PW_T_SEQ: return (float)((const seq_st *)st)->step;
    case PW_T_SWITCH: return what < 4 ? ((const sw_st *)st)->g[what] : 0;
    case PW_T_GENDY: {   /* 0..15: corner heights (-1..1); 16..32: where each segment starts (0..1); 33: the phase in the cycle */
        const gendy_st *q = (const gendy_st *)st;
        return what < 16u ? q->a[what] : what < 33u ? q->c[what - 16u] : q->ph;
    }
    case PW_T_ROOM: {   /* 0..7: how loudly mode k rings now (0..1); 8..15: its pitch in Hz, 0 if off */
        const ro_st *q = (const ro_st *)st;
        if (what < 8u) return pw_clamp(1.0f + pw_log2(ine_sqrt(0.5f * (q->y1[what] * q->y1[what] + q->y2[what] * q->y2[what])) + 1e-6f) * (1.0f / 9.0f), 0.0f, 1.0f);   /* (a dB-like scale: the tail is seen to the end) */
        return what < 16u ? q->f[what - 8u] : 0.0f;
    }
    case PW_T_FRACTURE: {
        const fract_st *fs = (const fract_st *)st;
        if (fs->mode == 1) {         /* REVERSE: always going backwards; where in the piece (1 at its start, 0 at its end) */
            float L = pw_clamp(m->p[7] * PW_SRF, 256.0f, (float)(m->mem_len / 4u) - 4.0f);
            return what == 0 ? 2.0f : 1.0f - pw_clamp(fs->u.r.ph / L, 0.0f, 1.0f);
        }
        if (fs->mode == 2) {         /* SHUFFLE: a slice playing (forwards 1, backwards 2) and how far through it is */
            const shuf_st *h = &fs->u.h;
            return what == 0 ? (h->wet ? (h->rinc < 0 ? 2.0f : 1.0f) : 0.0f) : (h->seglen > 0 ? pw_clamp(pw_absf(h->rinc) * h->segpos / h->seglen, 0.0f, 1.0f) : 0.0f);
        }
        const frac_st *f = &fs->u.f; return what == 0 ? (f->active ? (f->rev ? 2.0f : 1.0f) : 0.0f) : (f->len > 0 ? f->pos / (float)f->len : 0.0f); }
    case PW_T_NOISE: {   /* the nth last DUST event: how long ago (seconds; 0..11), its size (-1..1; 16..27); 99 / 0: none */
        const noise_st *q = (const noise_st *)st;
        unsigned n = what & 15u;
        if (n >= q->ec) return what >= 16u ? 0.0f : 99.0f;
        unsigned at = (q->eh + 11u - n) % 12u;
        return what >= 16u ? (float)q->ea[at] * (1.0f / 127.0f) : (float)(q->clk - q->et[at]) * PW_INV_SR;
    }
    case PW_T_LOOPER: return what == 0 ? ((const loop_st *)st)->ph : (float)((const loop_st *)st)->recing;
    case PW_T_TAPE: return what == 0 ? ((const tape_st *)st)->spd : what == 2 ? ((const tape_st *)st)->eff : ((const tape_st *)st)->stopped ? 1.0f : 0.0f;
    case PW_T_LIMITER: return what == 0 ? ((const lim_st *)st)->grdb : ((const lim_st *)st)->pp;
    case PW_T_OTT: return what < 3 ? ((const ott_st *)st)->gdb[what] : what < 6 ? ((const ott_st *)st)->lvl[what - 3] : 0;
    case PW_T_MODSEQ: return what < 4 ? (float)((const mseq_st *)st)->pos[what] : -1;
    case PW_T_CHORDSTEPS: return (float)((const cst_st *)st)->step;
    case PW_T_MARKOV: return what == 0 ? (float)((const mk_st *)st)->cur : what == 1 ? ((const mk_st *)st)->pitch : (float)((const mk_st *)st)->gate;
    case PW_T_ARP: { const arp_st *a = (const arp_st *)st; return what == 0 ? (float)a->n : what == 1 ? (float)a->idx : what == 2 ? (a->gate ? 1.0f : 0.0f) : what >= 8 && what < 24 ? (float)a->notes[what - 8] : 0; }
    case PW_T_GRAVITY: { const grav_st *g = (const grav_st *)st; return what == 0 ? (float)g->hit : what == 1 ? (g->active ? 1.0f : 0.0f) : what == 2 ? g->el * PW_INV_SR : g->rest * PW_INV_SR; }
    case PW_T_PARAMMOD: return what < 4 ? ((const pmod_st *)st)->sm[what] : 0;
    case PW_T_SHIFTREG: return what < 16 ? ((const shr_st *)st)->r[what] : 0;
    case PW_T_CVREC: return what == 0 ? ((const cvr_st *)st)->ph : what == 1 ? ((const cvr_st *)st)->sm : (float)((const cvr_st *)st)->rec_left;
    case PW_T_GRAIN: {   /* 0..7: grain k's place (0..1 of what it reads), -1 off; 8: the recording head's */
        const gran_st *g = (const gran_st *)st;
        uint32_t len = m->mem_len / 2u;
        if (what == 8) return len ? (float)g->w / (float)len : 0;
        if (what >= GR_N || !g->g[what].on) return -1;
        int src = (int)(m->p[8] + 0.5f);
        if (src > 0) { pw_smp sm; if (!pw_smp_open((unsigned)src - 1u, &sm) || !sm.frames) return -1; return g->g[what].pos / (float)sm.frames; }
        return len ? g->g[what].pos / (float)len : -1;
    }
    case PW_T_SNAPSHOT: return (float)((const snp_st *)st)->cur;
    case PW_T_INERTIA: return what == 0 ? ((const ine_st *)st)->x : ((const ine_st *)st)->v;
    case PW_T_LFO: return ((const lfo_st *)st)->ph;
    case PW_T_TURING: return (float)(what == 1 ? ((const turing_st *)st)->reg >> 16 : ((const turing_st *)st)->reg & 0xFFFFu);
    default: return 0;
    }
}

/* Which CV jack moves which knob, and how: a display aid (the modulation marker on the knob), not the sound. LIN: the
 * knob's value plus the CV x K; EXP: its value x 2^(CV x K) (a V/oct jack; K negative: the other way). AMT: another knob
 * that scales the CV (VCF's CV AMT), or 255. */
typedef struct { uint8_t type, param, in, mode; float k; uint8_t amt; } cvmap_t;
static const cvmap_t cv_map[] = {
    {PW_T_LFO, 0, 0, 1, 1.0f, 255}, {PW_T_DELAY, 0, 1, 1, -1.0f, 255}, {PW_T_DELAY, 1, 2, 0, 0.2f, 255},
    {PW_T_SWIRL, 1, 1, 1, 1.0f, 255},
    {PW_T_CHAOS, 0, 0, 1, 1.0f, 255}, {PW_T_FRACTURE, 7, 3, 1, 1.0f, 255}, {PW_T_VCF, 0, 1, 1, 1.0f, 2}, {PW_T_VCF, 1, 2, 0, 0.2f, 255},
    {PW_T_PSHIFT, 0, 1, 0, 12.0f, 255}, {PW_T_COMPLEX, 3, 1, 0, 0.2f, 255}, {PW_T_COMPLEX, 2, 2, 0, 2.0f, 255},
    {PW_T_COMB, 1, 2, 0, 0.2f, 255}, {PW_T_TAPE, 1, 2, 0, 0.2f, 255}, {PW_T_COIN, 0, 1, 0, 0.2f, 255},
    {PW_T_TURING, 1, 1, 0, 0.2f, 255}, {PW_T_FRACTURE, 1, 4, 0, 0.2f, 255}, {PW_T_FOLDER, 0, 1, 0, 2.0f, 255},
    {PW_T_DIST, 0, 1, 0, 0.2f, 255}, {PW_T_VOWEL, 0, 1, 0, 0.8f, 255}, {PW_T_FORMANT, 1, 1, 0, 0.8f, 255},
};
int pw_param_cv(unsigned type, unsigned param, float *k, int *mode, int *amt)
{
    for (unsigned i = 0; i < sizeof cv_map / sizeof cv_map[0]; ++i)
        if (cv_map[i].type == type && cv_map[i].param == param) {
            *k = cv_map[i].k; *mode = cv_map[i].mode; *amt = cv_map[i].amt;
            return cv_map[i].in;
        }
    return -1;
}

unsigned pw_param_at(unsigned type, unsigned pos)
{
    if (type >= PW_T_COUNT) return pos;
    return pw_types[type].order && pos < pw_types[type].n_param ? pw_types[type].order[pos] : pos;
}
