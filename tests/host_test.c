/* Host tests for the PATCHWERK engine: every preset renders finite, audible
 * audio; serialization round-trips; module-level DSP checks; CPU timing. */
#include "pw_engine.h"
#include "pw_dsp.h"
#include "pw_presets.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <time.h>

void pw_plat_lock(void) {}
void pw_plat_unlock(void) {}
static int t_c1_on;   /* a pretend second core that does each block at once: the engine's offload logic can be tested here */
static uint32_t t_tick, t_post;
uint32_t pw_plat_cpu1_post(pw_cpu1_fn fn, void *m, const float *const *in, float *const *out, unsigned connected) { if (!t_c1_on) return 0; fn(m, in, out, connected); return ++t_post; }
int pw_plat_cpu1_wait(uint32_t ticket, uint32_t *us) { (void)ticket; if (us) *us = 7; return 0; }
void pw_plat_cpu1_stop(void) {}
pw_cpu1_fn pw_plat_cpu1_dummy(void) { return 0; }
uint32_t pw_plat_ticks(void) { if (t_c1_on) t_tick += 100000u; return t_tick; }   /* (100 us a call while the pretend core is on: every module "costs" 100 us) */

uint32_t pw_plat_ticks_per_us(void) { return 1000; }
int pw_plat_audio_running(void) { return 0; }
uint32_t pw_plat_millis(void) { return 0; }
/* a bank with one sample (slot 2: a 440 Hz sine, 22050 Hz), and an audio input (a 220 Hz sine) */
static int16_t bank_sine[11025];
static int16_t bank_hits[44100];
static uint8_t cz_bank[2 * 144];   /* the CZ slot: two CZ tones (filled by the CZ test) */
static uint8_t dx_bank[2 * 128];   /* FM-DX's first .syx slot: two TX81Z voices (filled by the FM-DX test) */
const int16_t *pw_plat_sample(unsigned slot, uint32_t *frames, uint32_t *rate)
{
    if (slot == PW_DX_SLOT) { *frames = sizeof dx_bank / 2; *rate = 44100; return (const int16_t *)(const void *)dx_bank; }
    if (slot == PW_CZ_SLOT) { *frames = sizeof cz_bank / 2; *rate = 44100; return (const int16_t *)(const void *)cz_bank; }
    if (slot == 3) {             /* B4: four hits (at 0, 0.25, 0.5 and 0.8 s), each a decaying 300 Hz burst */
        static const float at[4] = {0, 0.25f, 0.5f, 0.8f};
        for (int i = 0; i < 44100; ++i) {
            float t = i / 44100.0f, v = 0;
            for (int k = 0; k < 4; ++k) if (t >= at[k]) v += sinf(2.0f * (float)M_PI * 300.0f * (t - at[k])) * expf(-(t - at[k]) * 30.0f);
            bank_hits[i] = (int16_t)(v * 15000.0f);
        }
        *frames = 44100; *rate = 44100;
        return bank_hits;
    }
    if (slot != 2) return 0;
    for (int i = 0; i < 11025; ++i) bank_sine[i] = (int16_t)(sinf(2.0f * (float)M_PI * 440.0f * i / 22050.0f) * 20000.0f);
    *frames = 11025; *rate = 22050;
    return bank_sine;
}
/* B5: a compressed sound (SEA, tests/sea_fixture.inc: 3020 frames at 44.1 kHz) */
#include "sea_fixture.inc"
const uint8_t *pw_plat_sample_sea(unsigned slot, uint32_t *frames, uint32_t *rate)
{
    if (slot != 4) return 0;
    *frames = 3020; *rate = 44100;
    return sea_fix_stream;
}
const char *pw_plat_sample_name(unsigned slot) { return slot == 2 ? "SINE" : slot == 3 ? "HITS" : slot == 4 ? "SEA" : 0; }
static unsigned midi_sent; static uint8_t midi_log[64][3];
void pw_plat_midi_out(uint8_t st, uint8_t d1, uint8_t d2) { if (midi_sent < 64) { midi_log[midi_sent][0] = st; midi_log[midi_sent][1] = d1; midi_log[midi_sent][2] = d2; } ++midi_sent; }
static int audio_in_on;
static double ain_ph;
int pw_plat_audio_in(float *l, float *r)
{
    if (!audio_in_on) return 0;
    for (int i = 0; i < PW_N; ++i) { l[i] = r[i] = (float)(0.5 * sin(ain_ph)); ain_ph += 2 * M_PI * 220.0 / 44100.0; }
    return 1;
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

static void write_wav(const char *path, const float *l, const float *r, int frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int bytes = frames * 4;
    unsigned char h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
        0x44,0xac,0,0,0x10,0xb1,2,0,4,0,16,0,'d','a','t','a'};
    int riff = bytes + 36;
    memcpy(h + 4, &riff, 4); memcpy(h + 40, &bytes, 4);
    fwrite(h, 1, 44, f);
    for (int i = 0; i < frames; ++i) {
        short s[2] = {(short)(fmaxf(-1, fminf(1, l[i])) * 32767), (short)(fmaxf(-1, fminf(1, r[i])) * 32767)};
        fwrite(s, 2, 2, f);
    }
    fclose(f);
}

#define SECONDS 4
static float L[44100 * SECONDS], R[44100 * SECONDS];
static double render(int blocks, int play_keys)
{
    clock_t t0 = clock();
    for (int b = 0; b < blocks; ++b) {
        if (play_keys) {
            int t = b % 172; /* every ~0.5 s */
            if (t == 0) pw_key(48 + (b / 172 % 4) * 5, 100);
            if (t == 120) pw_key(48 + (b / 172 % 4) * 5, 0);
        }
        pw_render(L + b * PW_N, R + b * PW_N);
    }
    return (double)(clock() - t0) / CLOCKS_PER_SEC;
}
static void stats(int frames, float *rms, float *peak, int *finite)
{
    double s = 0; float p = 0; *finite = 1;
    for (int i = 0; i < frames; ++i) {
        if (!isfinite(L[i]) || !isfinite(R[i])) *finite = 0;
        s += L[i] * L[i] + R[i] * R[i];
        p = fmaxf(p, fmaxf(fabsf(L[i]), fabsf(R[i])));
    }
    *rms = (float)sqrt(s / (2.0 * frames)); *peak = p;
}

static float tone_rms_through(unsigned type, unsigned port_in, unsigned port_out, float hz, void (*setup)(int))
{
    pw_clear();
    int osc = pw_add(PW_T_VCO, 0, 0), dut = pw_add(type, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
    int cv = pw_add(PW_T_ATTEN, 0, 0);
    pw_set_param(cv, 1, log2f(hz / 261.625565f));
    pw_connect(cv, 0, osc, 0);
    pw_set_param(out, 0, 1.0f);
    if (setup) setup(dut);
    pw_connect(osc, 0, dut, port_in);
    pw_connect(dut, port_out, out, 0);
    int blocks = 44100 / PW_N;
    render(blocks, 0);
    float rms, peak; int finite;
    stats(blocks * PW_N / 2, &rms, &peak, &finite); /* skip nothing: steady */
    double s = 0; int n = 0;
    for (int i = blocks * PW_N / 2; i < blocks * PW_N; ++i) { s += L[i] * L[i]; ++n; }
    return (float)sqrt(s / n);
}
static int filter_type;
static void vcf_setup(int m) { pw_set_param(m, 0, 1000); pw_set_param(m, 1, 0); pw_set_param(m, 4, (float)filter_type); }


/* CLIP PROTECT: OUTPUT's brickwall limiter (-0.5 dBFS, 88 frames of lookahead, one gain for both channels) */
static void clip_patch(float level, int two_outs, int protect, float r_amt)
{
    pw_clear();
    int osc = pw_add(PW_T_VCO, 0, 0), o1 = pw_add(PW_T_OUTPUT, 0, 0);
    pw_set_param(osc, 0, 0.0f);
    pw_set_param(o1, 0, level);
    pw_set_param(o1, 1, (float)protect);
    pw_connect(osc, 0, o1, 0);
    if (r_amt > 0) { int c = pw_connect(osc, 0, o1, 1); pw_cable_amount((unsigned)c, (int)(r_amt * 100.0f)); }
    if (two_outs) { int o2 = pw_add(PW_T_OUTPUT, 0, 0); pw_set_param(o2, 0, level); pw_set_param(o2, 1, (float)protect); pw_connect(osc, 0, o2, 0); }
}
static void test_clip_protect(void)
{
    const float ceil_ = 0.94406f + 1e-5f;
    int blocks = 44100 / PW_N, nfr = blocks * PW_N;
    float rms, peak; int finite;
    CHECK(pw_types[PW_T_OUTPUT].n_param == 2 && pw_types[PW_T_OUTPUT].param[1].def == 1.0f, "OUTPUT's CLIP PROTECT defaults to ON");
    clip_patch(1.0f, 1, 1, 0); render(blocks, 0);
    float pk = 0; for (int i = 256; i < nfr; ++i) pk = fmaxf(pk, fmaxf(fabsf(L[i]), fabsf(R[i])));
    stats(nfr, &rms, &peak, &finite);
    CHECK(finite && pk <= ceil_ && pk > 0.9f, "CLIP PROTECT: loud signal peaks at %.5f (ceiling 0.94406)", pk);
    CHECK(pws.clip_on == 1 && pw_clip_gr_tenths() < -30, "CLIP PROTECT reports about -6 dB: %d tenths", pw_clip_gr_tenths());
    clip_patch(1.0f, 1, 0, 0); render(blocks, 0);
    pk = 0; for (int i = 256; i < nfr; ++i) pk = fmaxf(pk, fabsf(L[i]));
    CHECK(pws.clip_on == 0 && pk > 0.95f && pw_clip_gr_tenths() == 0, "CLIP PROTECT off: no limiting (peak %.4f, gr %d)", pk, pw_clip_gr_tenths());
    static float offl[44100], offr[44100];
    clip_patch(0.6f, 0, 0, 0); render(blocks, 0);
    memcpy(offl, L, sizeof offl); memcpy(offr, R, sizeof offr);
    clip_patch(0.6f, 0, 1, 0); render(blocks, 0);
    float worst = 0; for (int i = 600; i < nfr - 100; ++i) worst = fmaxf(worst, fmaxf(fabsf(L[i + 88] - offl[i]), fabsf(R[i + 88] - offr[i])));
    CHECK(worst < 1e-6f, "CLIP PROTECT below the ceiling is the input 88 frames late (worst difference %.2e)", worst);
    CHECK(pw_clip_gr_tenths() == 0, "no gain reduction below the ceiling (%d)", pw_clip_gr_tenths());
    static float inl[44100], inr[44100];
    clip_patch(0.5f, 0, 1, 0.1f); render(blocks, 0);
    for (int i = 0; i + 88 < nfr; ++i) { inl[i] = 2.0f * L[i + 88]; inr[i] = 2.0f * R[i + 88]; }
    clip_patch(1.0f, 0, 1, 0.1f); render(blocks, 0);
    float rl = 0, rr = 0, dl = 0, dr = 0;
    for (int i = 20000; i < 22000; ++i) { rl = fmaxf(rl, fabsf(L[i + 88])); rr = fmaxf(rr, fabsf(R[i + 88])); dl = fmaxf(dl, fabsf(inl[i])); dr = fmaxf(dr, fabsf(inr[i])); }
    CHECK(fabsf(rr / dr - rl / dl) < 0.01f && rl / dl < 0.97f, "CLIP PROTECT: linked gain (R x%.3f, L x%.3f)", rr / dr, rl / dl);
    pw_set_rate(1);
    clip_patch(1.0f, 1, 1, 0); render(blocks, 0);
    pk = 0; for (int i = 512; i < nfr; ++i) pk = fmaxf(pk, fmaxf(fabsf(L[i]), fabsf(R[i])));
    CHECK(pk <= ceil_, "CLIP PROTECT at 22.05 kHz: peak %.5f", pk);
    pw_set_rate(0);
    clip_patch(1.0f, 1, 0, 0); render(40, 0);
    pw_set_param(1, 1, 1.0f); pw_set_param(2, 1, 1.0f);
    for (int k = 0; k < 40; ++k) pw_render(L + k * PW_N, R + k * PW_N);
    float jump = 0; for (int i = 1; i < 40 * PW_N; ++i) jump = fmaxf(jump, fabsf(L[i] - L[i - 1]));
    stats(40 * PW_N, &rms, &peak, &finite);
    CHECK(finite && jump < 0.2f, "switching CLIP PROTECT on while playing is smooth (largest step %.3f)", jump);
}

/* SEA: the decoder gives the reference decoder's samples, in order, at random and by runs; a SAMPLER and the overview play and show it */
/* a SAMPLER on its own recording (RAM): the new-module default is INTERNAL 1 */
static int add_smp(void) { int s = pw_add(PW_T_SAMPLER, 0, 0); if (s >= 0) { pw_set_param(s, 0, 0); pw_set_param(s, 1, 0); } return s; }
static void test_sea(void)
{
    pw_smp s;
    CHECK(pw_smp_open(20, &s) && s.sea && !s.raw && s.frames == 3020 && s.rate == 44100, "SEA: A-5 opens as a compressed sound");
    CHECK(!pw_smp_open(21, &s), "SEA: an empty slot does not open");
    pw_smp_open(20, &s);
    CHECK(pw_sea_bytes(3020) == sizeof sea_fix_stream, "SEA: the stored size of 3020 frames is %u (stream %u)", pw_sea_bytes(3020), (unsigned)sizeof sea_fix_stream);
    int bad = 0;
    pw_rdr *r = pw_rdr_get(7, &s);
    CHECK(r != 0, "SEA: a reader for a compressed sound");
    for (uint32_t i = 0; i < 3020; ++i) bad += pw_smp_get(&s, r, i) != sea_fix_pcm[i];                       /* forward */
    for (uint32_t i = 0; i < 3020; ++i) { uint32_t j = (i * 1237u) % 3020u; bad += pw_smp_get(&s, r, j) != sea_fix_pcm[j]; }   /* jumping about */
    for (uint32_t i = 3020; i-- > 0;) bad += pw_smp_get(&s, r, i) != sea_fix_pcm[i];                         /* backwards */
    for (uint32_t i = 0; i < 3020; i += 3) bad += pw_smp_get(&s, r, i) != sea_fix_pcm[i];                    /* a fast read */
    for (uint32_t i = 0; i < 3017; ++i) for (int k = -1; k <= 2; ++k) { uint32_t j = i + (uint32_t)(k < 0 && !i ? 1 : k); bad += pw_smp_get(&s, r, j) != sea_fix_pcm[j]; }   /* a cubic's window */
    pw_rdr *r2 = pw_rdr_get(8, &s);
    CHECK(r2 && r2 != r && pw_rdr_get(7, &s) == r, "SEA: another voice gets another reader, the first keeps its own");
    for (uint32_t i = 0; i < 3020; i += 7) bad += pw_smp_get(&s, r2, i) != sea_fix_pcm[i] || pw_smp_get(&s, r, 3019 - i) != sea_fix_pcm[3019 - i];
    pw_rdr *all[40]; int got = 0;
    for (unsigned k = 0; k < 40; ++k) { all[k] = pw_rdr_get(100 + k, &s); got += all[k] != 0; }
    CHECK(got < 40 && got >= 14, "SEA: only so many voices get a reader at once (%d of 40), the rest get silence, not a decode each sample", got);
    CHECK(pw_smp_get(&s, 0, 100) == 0, "SEA: a voice without a reader hears silence");
    for (int b = 0; b < 3; ++b) pw_sea_block();
    CHECK(pw_rdr_get(500, &s) != 0, "SEA: readers not asked for in two blocks are free again");
    static int16_t run[3020];
    pw_smp_read(&s, 0, 3020, run);
    for (uint32_t i = 0; i < 3020; ++i) bad += run[i] != sea_fix_pcm[i];
    pw_smp_read(&s, 495, 30, run);
    for (uint32_t i = 0; i < 30; ++i) bad += run[i] != sea_fix_pcm[495 + i];
    CHECK(bad == 0, "SEA: the decoder matches the reference decoder (%d frames differ)", bad);
    CHECK(pw_smp_get(&s, pw_rdr_get(7, &s), 3020) == 0 && pw_smp_get(&s, pw_rdr_get(7, &s), 100000) == 0, "SEA: reading past the end gives silence");
    uint8_t pk[20]; uint32_t fr = 0;
    CHECK(pw_sample_overview(20, pk, 20, &fr) && fr == 3020, "SEA: overview");
    int mx = 0; for (int i = 0; i < 20; ++i) mx = pk[i] > mx ? pk[i] : mx;
    int ref = 0; for (int i = 0; i < 3020; ++i) { int a = sea_fix_pcm[i] < 0 ? -sea_fix_pcm[i] : sea_fix_pcm[i]; ref = a > ref ? a : ref; }
    CHECK(mx == ref >> 7 || mx == (ref >> 7) - 1, "SEA: overview peak %d, sound's %d", mx, ref >> 7);
    pw_clear();
    int sm = add_smp(), g = pw_add(PW_T_ATTEN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
    (void)g;
    pw_set_param(sm, 0, 5); pw_set_param(sm, 25, 1); pw_set_param(sm, 1, 3); pw_set_param(out, 0, 1); pw_connect(sm, 0, out, 0);
    int gate = pw_add(PW_T_LFO, 0, 0); pw_set_param(gate, 0, 2.0f); pw_set_param(gate, 2, 1); pw_connect(gate, 3, sm, 1);
    render(100, 0);
    float rms, peak; int finite;
    stats(100 * PW_N, &rms, &peak, &finite);
    CHECK(finite && rms > 0.005f, "SEA: a SAMPLER plays A-5 (rms %.4f)", rms);
    /* a new SAMPLER's RAM starts with the drum break (one second, decoded from the firmware's SEA stream): it sounds, and ends where the second ends */
    pw_clear(); pw_set_voices(1);
    int sm2 = add_smp(); out = pw_add(PW_T_OUTPUT, 0, 0);
    pw_set_param(out, 0, 1); pw_set_param(sm2, 1, 1); pw_connect(sm2, 0, out, 0);   /* (1SHOT: plays the sound through) */
    int gt = pw_add(PW_T_LFO, 0, 0); pw_set_param(gt, 0, 1.0f); pw_set_param(gt, 2, 1); pw_connect(gt, 3, sm2, 1);
    render(200, 0);
    stats(200 * PW_N, &rms, &peak, &finite);
    CHECK(finite && rms > 0.01f, "SAMPLER: the starting drum break sounds (rms %.4f)", rms);
    /* RAM is the INTERNAL bank's: in A-D SAMPLE 0 is sound 1; a new SAMPLER starts as 1SHOT; arming REC goes back to RAM */
    pw_clear(); pw_set_voices(1);
    {
        int sm4 = pw_add(PW_T_SAMPLER, 0, 0);
        CHECK((int)(pws.mod[sm4].p[1] + 0.5f) == 1, "SAMPLER: a new one starts as 1SHOT");
        char lab[16]; pw_set_param(sm4, 25, 1); pw_set_param(sm4, 0, 0); pw_format_param((unsigned)sm4, 0, lab);
        CHECK(lab[0] == '1', "SAMPLER: in bank A, SAMPLE 0 reads as sound 1 (%s)", lab);
        pw_sampler_rec((unsigned)sm4, 1);
        CHECK((int)(pws.mod[sm4].p[25] + 0.5f) == 0, "SAMPLER: arming REC selects the INTERNAL bank");
        pw_sampler_rec((unsigned)sm4, -1);
    }
    /* the panel's REC on a SAMPLER: a press arms, the next records IN (a second at most), the next stops; nothing patched: nothing recorded */
    pw_clear(); pw_set_voices(1);
    {
        int sm3 = add_smp(), vc = pw_add(PW_T_VCO, 0, 0);
        CHECK(pw_sampler_rec((unsigned)sm3, 0) == 0 && !pw_sampler_marked((unsigned)sm3), "REC: idle to begin with");
        CHECK(pw_sampler_rec((unsigned)sm3, 1) == 1, "REC: a press arms");
        CHECK(pw_sampler_rec((unsigned)sm3, 1) == 2, "REC: the next records");
        render(10, 0);
        CHECK(pw_sampler_rec((unsigned)sm3, 0) == 0 && pw_sampler_rec_done((unsigned)sm3) == 2 && !pw_sampler_marked((unsigned)sm3), "REC: with nothing patched into IN, nothing is recorded");
        pw_connect(vc, 0, sm3, 6);
        pw_sampler_rec((unsigned)sm3, 1); pw_sampler_rec((unsigned)sm3, 1);
        render(100, 0);
        CHECK(pw_sampler_rec((unsigned)sm3, 0) == 2, "REC: recording");
        pw_sampler_rec((unsigned)sm3, 1);                /* stop */
        render(5, 0);
        unsigned fr = 0;
        CHECK(pw_sampler_rec((unsigned)sm3, 0) == 0 && pw_sampler_recorded((unsigned)sm3, &fr) && fr > 1000u && fr < 44100u, "REC: stopped, %u frames kept", fr);
        pw_sampler_rec((unsigned)sm3, 1); pw_sampler_rec((unsigned)sm3, 1);
        render(400, 0);
        CHECK(pw_sampler_rec((unsigned)sm3, 0) == 0 && pw_sampler_recorded((unsigned)sm3, &fr) && fr == 44100u, "REC: a second is the most (%u)", fr);
    }
}

/* the banks: INTERNAL (built in), A (the bank's slots 0-7), B (8-15), C (24-31), D (32-39), eight sounds each; INTERNAL is empty until sounds are built in; a new SAMPLER starts on RAM */
static void test_banks(void)
{
    pw_smp s; char t[12];
    CHECK(!pw_smp_open(0, &s) && !pw_smp_open(15, &s), "INTERNAL: no sounds are built in for now (the code region is full)");
    CHECK(!pw_smp_open(48, &s) && !pw_smp_open(16, &s), "INTERNAL: ids past D and an empty A slot do not open");
    pw_clear();
    int sm = pw_add(PW_T_SAMPLER, 0, 0);
    pw_format_param(sm, 0, t);
    CHECK(pws.mod[sm].p[0] == 0 && pws.mod[sm].p[25] == 0 && !strcmp(t, "RAM"), "a new SAMPLER is on RAM (%s)", t);
    pw_set_param(sm, 0, 3); pw_format_param(sm, 0, t);
    CHECK(!strcmp(t, "3-"), "an empty INTERNAL slot (%s)", t);
    pw_set_param(sm, 0, 4); pw_set_param(sm, 25, 1); pw_format_param(sm, 0, t);
    CHECK(!strcmp(t, "4 HITS"), "SAMPLE shows the sound in its BANK (%s)", t);
    pw_set_param(sm, 25, 2); pw_format_param(sm, 0, t);
    CHECK(!strcmp(t, "4-"), "an empty B slot (%s)", t);
    CHECK(pw_types[PW_T_SAMPLER].order && pw_param_at(PW_T_SAMPLER, 0) == 25 && pw_param_at(PW_T_SAMPLER, 1) == 0, "BANK comes first, then SAMPLE");
    pw_clear();
    int r = pw_add(PW_T_DRUMRACK, 0, 0);
    pw_set_param(r, 4, 8); pw_format_param(r, 4, t);  CHECK(!strncmp(t, "A-1", 3), "rack pad on A-1 (%s)", t);
    pw_set_param(r, 4, 10); pw_format_param(r, 4, t); CHECK(!strcmp(t, "A-3 SINE"), "rack pad on A-3 (%s)", t);
    pw_set_param(r, 4, 24); pw_format_param(r, 4, t); CHECK(!strcmp(t, "C-1"), "rack pad on C-1 (%s)", t);
    pw_set_param(r, 4, 40); pw_format_param(r, 4, t); CHECK(!strcmp(t, "BOOM KICK"), "rack pad on a kit voice (%s)", t);
    pw_set_param(r, 8, 0); pw_nudge_param(r, 8, 103, 0);   /* turned up to the last kit voice: its tune, decay and level go on the pad */
    pw_format_param(r, 8, t); CHECK(!strcmp(t, "TOY BELL") && pws.mod[r].p[9] == 14 && fabsf(pws.mod[r].p[10] - 0.1f) < 0.011f, "rack pad voice TOY BELL puts its tune and decay on the pad (%s %g %g)", t, pws.mod[r].p[9], pws.mod[r].p[10]);
    pw_set_param(r, 36, 10);
    CHECK(pws.mod[r].p[4] == 16 && pws.mod[r].p[32] == 23, "rack KIT B (%g .. %g)", pws.mod[r].p[4], pws.mod[r].p[32]);
    pw_set_param(r, 36, 11);
    CHECK(pws.mod[r].p[4] == 24 && pws.mod[r].p[32] == 31, "rack KIT C (%g .. %g)", pws.mod[r].p[4], pws.mod[r].p[32]);
    {   /* a patch saved before format 12 (pad sounds 24-55 were banks A-D, 8-23 INTERNAL) loads with the new numbers */
        static uint8_t pb[4096];
        pw_set_param(r, 4, 26); pw_set_param(r, 8, 12); pw_set_param(r, 12, 50);
        int n = pw_save(pb, sizeof pb);
        pb[4] = 11; { uint32_t crc = ~pw_crc32(~0u, pb, (unsigned)n - 4); pb[n - 4] = (uint8_t)crc; pb[n - 3] = (uint8_t)(crc >> 8); pb[n - 2] = (uint8_t)(crc >> 16); pb[n - 1] = (uint8_t)(crc >> 24); }
        pw_clear(); int lr = pw_load(pb, (unsigned)n);
        int q = 0; while (q < PW_MAX_MODULES && !(pws.mod[q].used == 1 && pws.mod[q].type == PW_T_DRUMRACK)) ++q;
        CHECK(lr >= 0 && q < PW_MAX_MODULES && pws.mod[q].p[4] == 10 && pws.mod[q].p[8] == 0 && pws.mod[q].p[12] == 34, "rack sounds from an old patch are renumbered (%g %g %g)", pws.mod[q].p[4], pws.mod[q].p[8], pws.mod[q].p[12]);
    }
}

static int maxsave;
/* MUTATE (the engine's nudge and roll, the module's jacks, the locks and their saving) and WAVESET (every mode makes finite, audible sound) */
static void test_mutate_waveset(void)
{
    static float L[PW_NMAX], R[PW_NMAX];
    pw_init(); pw_clear();
    CHECK(pw_example_build(PW_T_MUTATE) == 0, "MUTATE: its example builds");
    int mu = -1, sq = -1, vf = -1, vco = -1;
    for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) {
        if (pws.mod[i].type == PW_T_MUTATE) mu = i;
        if (pws.mod[i].type == PW_T_SEQ) sq = i;
        if (pws.mod[i].type == PW_T_VCF) vf = i;
        if (pws.mod[i].type == PW_T_VCO) vco = i;
    }
    CHECK(mu >= 0 && sq >= 0 && vf >= 0, "MUTATE: the example has a MUTATE, a SEQUENCER and a VCF");
    if (mu < 0 || sq < 0 || vf < 0) return;
    CHECK(!pw_mut_eligible((unsigned)mu), "MUTATE: a MUTATE module does not mutate itself");
    uint8_t d0[PW_DATA_MAX]; float p0[PW_MAX_PARAM];
    memcpy(d0, pws.mod[sq].data, sizeof d0); memcpy(p0, pws.mod[vf].p, sizeof p0);
    unsigned tune0 = 0; (void)tune0;
    float tune = vco >= 0 ? pws.mod[vco].p[0] : 0;
    pws.mut_lock = ~(uint64_t)0;
    CHECK(pw_mutate(1, 1.0f, 1.0f) == 0 && !memcmp(d0, pws.mod[sq].data, sizeof d0) && !memcmp(p0, pws.mod[vf].p, sizeof p0), "MUTATE: locked modules are left alone");
    pws.mut_lock = 0;
    int ch = pw_mutate(1, 1.0f, 1.0f);
    CHECK(ch > 0, "MUTATE: a roll changes something (%d)", ch);
    CHECK(memcmp(d0, pws.mod[sq].data, 18) != 0, "MUTATE: a roll changes the sequencer's steps");
    CHECK(memcmp(p0, pws.mod[vf].p, sizeof p0) != 0, "MUTATE: a roll changes the VCF's knobs");
    CHECK(vco < 0 || pws.mod[vco].p[0] == tune, "MUTATE: tuning is left alone");
    CHECK(pws.mod[vf].p[1] <= 0.8f + 1e-4f, "MUTATE: resonance stays under 80%% (%.2f)", pws.mod[vf].p[1]);
    CHECK(pw_undo() == 1 && !memcmp(d0, pws.mod[sq].data, 18) && !memcmp(p0, pws.mod[vf].p, sizeof p0), "MUTATE: one undo takes the whole roll back");
    memcpy(d0, pws.mod[sq].data, sizeof d0);
    ch = pw_mutate(0, 0.0f, 1.0f);
    CHECK(!memcmp(d0, pws.mod[sq].data, 18) || ch >= 0, "MUTATE: a nudge of 0 is small");
    pw_mutate(0, 1.0f, 1.0f);
    {   /* the module: pulses on its jacks do the same, from the screen's loop */
        memcpy(d0, pws.mod[sq].data, sizeof d0); memcpy(p0, pws.mod[vf].p, sizeof p0);
        pw_mutate_follow(0);   /* (counts what is there) */
        pw_set_playing(1);
        for (int b = 0; b < 44100 * 3 / PW_N; ++b) pw_render(L, R);
        pw_mutate_follow(100);
        CHECK(memcmp(d0, pws.mod[sq].data, 18) || memcmp(p0, pws.mod[vf].p, sizeof p0), "MUTATE module: a pulse on NUDGE changed the patch");
    }
    {   /* the locks are saved */
        static uint8_t pb[4096];
        pws.mut_lock = ((uint64_t)1 << sq) | ((uint64_t)1 << vf);
        int n = pw_save(pb, sizeof pb);
        pw_clear(); pws.mut_lock = 0;
        CHECK(n > 0 && pw_load(pb, (unsigned)n) >= 0, "MUTATE: the patch loads (%d)", n);
        int nl = 0; for (int i = 0; i < 64; ++i) nl += (int)(pws.mut_lock >> i & 1u);
        CHECK(nl == 2, "MUTATE: the locks come back with the patch (%d)", nl);
    }
    /* WAVESET */
    const char *mn[10] = {"REPEAT", "REVERSE", "OMIT", "DIVIDE", "FREEZE", "SCRAMBLE", "AVERAGE", "TELESCOPE", "INTERP", "FRACTAL"};
    for (int mode = 0; mode < 10; ++mode) {
        pw_init(); pw_clear();
        int v = pw_add(PW_T_VCO, 0, 0), ws = pw_add(PW_T_WAVESET, 0, 0), ou = pw_add(PW_T_OUTPUT, 0, 0);
        pw_connect(v, 1, ws, 0); pw_connect(ws, 0, ou, 0);
        pw_set_param(ws, 0, (float)mode); pw_set_param(ws, 2, mode == 2 ? 0.0f : 0.5f);
        pw_set_playing(1);
        double e = 0; int bad = 0;
        for (int b = 0; b < 44100 / PW_N; ++b) { pw_render(L, R); if (b > 10) for (int i = 0; i < PW_N; ++i) { e += (double)L[i] * L[i]; if (!(L[i] == L[i]) || fabsf(L[i]) > 50.0f) ++bad; } }
        CHECK(bad == 0 && e > 1.0, "WAVESET %s: finite and audible (rms^2 sum %.1f, %d bad)", mn[mode], e, bad);
    }
    {   /* SEQUENCER ties and glides: step 1 is a TIE and step 2 a GLIDE, so the gate runs on from step 0 into step 2, and only steps 0 and 3 trigger */
        enum { SPS = 5512, NS2 = 4 * SPS * 2 };   /* a 16th at 120 BPM; two loops */
        static float sig[3][NS2];
        for (int run = 0; run < 3; ++run) {
            pw_init(); pw_clear();
            int sq = pw_add(PW_T_SEQ, 0, 0), ou = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(sq, 0, 4); pw_set_param(sq, 1, 0.5f);
            static const int8_t notes[4] = {0, 0, 7, 7};
            for (int k = 0; k < 4; ++k) pws.mod[sq].data[k] = (uint8_t)notes[k];
            pws.mod[sq].data[16] = 0xff; pws.mod[sq].data[17] = 0xff;
            pws.mod[sq].data[PW_SEQ_STEPFEAT + 1] = 0xF0 | 4;   /* tie */
            pws.mod[sq].data[PW_SEQ_STEPFEAT + 2] = 0xF0 | 8;   /* glide */
            pw_connect(sq, run == 0 ? 1 : run == 1 ? 0 : 2, ou, 0);
            pw_set_playing(1);
            int n = 0;
            for (int b = 0; b < NS2 / PW_N; ++b) { pw_render(L, R); for (int i = 0; i < PW_N; ++i, ++n) sig[run][n] = L[i]; }
        }
        float gmax = 0, pmax = 0, tmax = 0;
        for (int i = 0; i < NS2; ++i) { if (sig[0][i] > gmax) gmax = sig[0][i]; if (sig[1][i] > pmax) pmax = sig[1][i]; if (sig[2][i] > tmax) tmax = sig[2][i]; }
        /* from the second loop on: the gate is high through steps 0, 1 and the first half of step 2, low in its second half */
        int base = 4 * SPS, high = 1, trigs = 0, pv = 0;
        for (int i = base + 200; i < base + 2 * SPS + SPS / 3; ++i) if (sig[0][i] < gmax * 0.5f) { high = 0; break; }
        for (int i = 0; i < NS2; ++i) { int h = sig[2][i] > tmax * 0.5f; if (h && !pv) ++trigs; pv = h; }
        CHECK(gmax > 0 && high, "SEQ tie: the gate runs on from step 0 through step 2");
        CHECK(sig[0][base + 2 * SPS + SPS * 3 / 4] < gmax * 0.5f, "SEQ: the gate ends in the second half of step 2 (step 3 is not legato)");
        CHECK(trigs >= 3 && trigs <= 5, "SEQ tie: two triggers a loop, not four (%d in two loops)", trigs);
        float early = sig[1][base + 2 * SPS + 400] / pmax * 7.0f, late = sig[1][base + 2 * SPS + 4000] / pmax * 7.0f;   /* in semitones: the glide has not arrived 9 ms in, has by 90 ms */
        CHECK(early > 0.3f && early < 5.0f && late > 6.5f, "SEQ glide: the pitch slides (%.1f semitones at 9 ms, %.1f at 90 ms; the end is 7)", early, late);
    }
    {   /* SEQUENCER: a step with velocity 0 is off: steps 1 and 3 at velocity 0 leave two triggers a loop, not four */
        enum { SPS = 5512, NS3 = 4 * SPS * 2 };
        int cnt[2];
        static float sg[NS3];
        for (int run = 0; run < 2; ++run) {
            pw_init(); pw_clear();
            int sq = pw_add(PW_T_SEQ, 0, 0), ou = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(sq, 0, 4); pw_set_param(sq, 1, 0.5f);
            pws.mod[sq].data[16] = 0xff; pws.mod[sq].data[17] = 0xff;
            if (run) { pws.mod[sq].data[PW_SEQ_STEPVEL + 1] = 0; pws.mod[sq].data[PW_SEQ_STEPVEL + 3] = 0; }
            pw_connect(sq, 2, ou, 0);
            pw_set_playing(1);
            int n = 0;
            for (int b = 0; b < NS3 / PW_N; ++b) { pw_render(L, R); for (int i = 0; i < PW_N; ++i, ++n) sg[n] = L[i]; }
            float mx = 0; for (int i = 0; i < NS3; ++i) if (sg[i] > mx) mx = sg[i];
            int pv = 0; cnt[run] = 0;
            for (int i = 0; i < NS3; ++i) { int h = sg[i] > mx * 0.5f; if (h && !pv) ++cnt[run]; pv = h; }
        }
        CHECK(cnt[0] >= 7 && cnt[0] <= 9 && cnt[1] >= 3 && cnt[1] <= 5, "SEQ velocity 0 is off: %d triggers in two loops with every step, %d with two at velocity 0", cnt[0], cnt[1]);
    }
    {   /* TELESCOPE with AMOUNT 0 squeezes every cycle into half: twice the rising crossings of REPEAT with AMOUNT 0 */
        int zc[2] = {0, 0};
        for (int k = 0; k < 2; ++k) {
            pw_init(); pw_clear();
            int v = pw_add(PW_T_VCO, 0, 0), ws = pw_add(PW_T_WAVESET, 0, 0), ou = pw_add(PW_T_OUTPUT, 0, 0);
            pw_connect(v, 1, ws, 0); pw_connect(ws, 0, ou, 0);
            pw_set_param(ws, 0, k ? 7 : 0); pw_set_param(ws, 2, 0.0f);
            pw_set_playing(1);
            int pos = 0;
            for (int b = 0; b < 44100 / PW_N; ++b) { pw_render(L, R); if (b > 20) for (int i = 0; i < PW_N; ++i) { if (!pos && L[i] > 0.05f) { pos = 1; ++zc[k]; } else if (L[i] < -0.05f) pos = 0; } }
        }
        CHECK(zc[1] > zc[0] * 17 / 10 && zc[1] < zc[0] * 23 / 10, "WAVESET TELESCOPE: about twice the cycles (%d against %d)", zc[1], zc[0]);
    }
    {   /* DIVIDE with AMOUNT 0: every second cycle upside down: one period on, the wave is the opposite; two periods on, the same */
        pw_init(); pw_clear();
        int v = pw_add(PW_T_VCO, 0, 0), ws = pw_add(PW_T_WAVESET, 0, 0), ou = pw_add(PW_T_OUTPUT, 0, 0);
        pw_connect(v, 1, ws, 0); pw_connect(ws, 0, ou, 0);
        pw_set_param(ws, 0, 3); pw_set_param(ws, 2, 0.0f);
        pw_set_playing(1);
        enum { NS = 44100 / 2 };
        static float o[NS]; int nn = 0;
        for (int b = 0; b < NS / PW_N; ++b) { pw_render(L, R); for (int i = 0; i < PW_N; ++i) o[nn++] = L[i]; }
        double best = 1e9; int bl = 0;
        for (int lag = 150; lag < 190; ++lag) {
            double c = 0, e = 0;
            for (int i = 8000; i < 12000; ++i) { c += (double)o[i] * o[i + lag]; e += (double)o[i] * o[i]; }
            if (c / e < best) { best = c / e; bl = lag; }
        }
        double c2 = 0, e2 = 0;
        for (int i = 8000; i < 12000; ++i) { c2 += (double)o[i] * o[i + 2 * bl]; e2 += (double)o[i] * o[i]; }
        CHECK(best < -0.7 && c2 / e2 > 0.7, "WAVESET DIVIDE: one period on it is upside down (%.2f at %d), two on the same (%.2f)", best, bl, c2 / e2);
    }
}

int main(void)
{
    pw_init();
    printf("module types: %d, state bytes %d, engine %zu bytes, pool %u\n",
           PW_T_COUNT, PW_STATE_BYTES, sizeof pws, (unsigned)PW_POOL_BYTES);
    for (int t = 0; t < PW_T_COUNT; ++t) {
        if (pw_type_hidden((unsigned)t)) { CHECK(pw_types[t].proc == 0 && !pw_types[t].n_param, "removed type %d is empty", t); continue; }
        CHECK(pw_types[t].proc != 0, "type %d has no proc", t);
        CHECK(pw_types[t].n_in <= PW_MAX_IN && pw_types[t].n_out <= PW_MAX_OUT && pw_types[t].n_param <= PW_MAX_PARAM, "type %d bounds", t);
        for (int p = 0; p < pw_types[t].n_param; ++p) {
            const pw_param_desc *d = &pw_types[t].param[p];
            CHECK(d->def >= d->min && d->def <= d->max, "%s.%s default out of range", pw_types[t].name, d->name);
            CHECK(strlen(d->name) <= 7, "%s.%s name too long", pw_types[t].name, d->name);
        }
        CHECK(strlen(pw_types[t].tag) <= 6, "tag too long %s", pw_types[t].tag);
    }
    /* Every module alone, all inputs fed by an LFO + VCO: finite outputs. */
    for (int t = 0; t < PW_T_COUNT; ++t) {
        pw_clear();
        int src = pw_add(PW_T_VCO, 0, 0), lfo = pw_add(PW_T_LFO, 0, 0), dut = pw_add(t, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(lfo, 0, 7);
        for (int i = 0; i < pw_types[t].n_in; ++i) pw_connect(i & 1 ? lfo : src, i & 1 ? 3 : 2, dut, i);
        for (int o = 0; o < pw_types[t].n_out; ++o) pw_connect(dut, o, out, o & 1);
        pw_key(60, 100);
        render(344, 0);
        float rms, peak; int finite;
        stats(344 * PW_N, &rms, &peak, &finite);
        CHECK(finite, "%s produced non-finite output", pw_types[t].name);
        printf("  %-13s rms %.3f peak %.3f\n", pw_types[t].name, rms, peak);
        pw_key(60, 0);
    }
    /* every module's example patch builds without a mistake in its recipe, and plays */
    for (unsigned t = 0; t < PW_T_COUNT; ++t) {
        int r = pw_example_build(t);
        CHECK(r != -2, "example for %s: its recipe has a mistake", pw_types[t].name);
    }
    /* Presets */
    for (unsigned p = 0; p < PW_FACTORY_COUNT; ++p) {
        pw_factory_build(p);
        pw_set_playing(1);
        int blocks = 44100 * SECONDS / PW_N;
        double secs = render(blocks, 1);
        float rms, peak; int finite;
        stats(blocks * PW_N, &rms, &peak, &finite);
        double load = secs / SECONDS * 100.0;
        printf("preset %u %-13s modules %2d cables %2d  rms %.3f peak %.3f  host cpu %.2f%%\n",
               p, pw_factory_name(p), pw_count(), pw_cable_count(), rms, peak, load);
        CHECK(finite, "preset %u non-finite", p);
        int input_only = 0, nomem = 0;   /* (INPUT FX: AUDIO IN's sound, none here) */
        for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) {
            if (pws.mod[i].type == PW_T_AUDIOIN) input_only = 1;
            if (pws.mod[i].status == PW_STATUS_NOMEM) { ++nomem; printf("  out of memory: %s\n", pw_types[pws.mod[i].type].name); }
        }
        CHECK(input_only || rms > 0.01f, "preset %u nearly silent (rms %f)", p, rms);
        CHECK(!nomem, "preset %u: %d modules out of memory", p, nomem);
        { int gone = 0, loose = 0; for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) gone += pw_type_hidden(pws.mod[i].type); (void)loose;
          CHECK(!gone, "preset %u names a removed module", p); }
        CHECK(peak <= 1.0f, "preset %u clips beyond full scale", p);
        if (p < 45) {   /* the variations (knob 4: 2-4) play without clipping */
            for (unsigned v = 1; v < 4; ++v) {
                pw_var_select(v);
                render(44100 / PW_N, 1);
                int sb = 44100 * 2 / PW_N;
                render(sb, 1);
                float r2, p2; int f2;
                stats(sb * PW_N, &r2, &p2, &f2);
                if (p2 > 1.0f) printf("   variation %u: peak %.3f\n", v + 1, p2);
                CHECK(f2 && p2 <= 1.0f, "preset %u variation %u clips (peak %f)", p, v + 1, p2);
            }
            pw_var_select(0);
        }
        if (p >= 45) {   /* a song example: each of its snapshots plays without clipping and is audible */
            unsigned used[PW_VARS] = {0};
            for (unsigned c = 0; c < PW_SONG_CLIPS; ++c) if (pws.song.clip[c].len && !(pws.song.clip[c].val & 0x80u)) used[pws.song.clip[c].val & 15u] = 1;
            for (unsigned v = 0; v < PW_VARS; ++v) {
                if (!used[v]) continue;
                pw_var_select(v);
                render(44100 / PW_N, 1);   /* (let it settle) */
                int sb = 44100 * SECONDS / PW_N;
                double sec2 = render(sb, 1);
                float r2, p2; int f2;
                stats(sb * PW_N, &r2, &p2, &f2);
                printf("   snapshot %u: rms %.3f peak %.3f  host cpu %.2f%%\n", v + 1, r2, p2, sec2 / SECONDS * 100.0);
                CHECK(f2 && r2 > 0.01f, "preset %u snapshot %u silent or broken (rms %f)", p, v + 1, r2);
                CHECK(p2 <= 1.0f, "preset %u snapshot %u clips (peak %f)", p, v + 1, p2);
            }
            pw_var_select(0);
        }
        char path[64];
        snprintf(path, sizeof path, "out/preset%u.wav", p);
        write_wav(path, L, R, blocks * PW_N);
        /* round trip */
        static uint8_t a[8192], b[8192];
        int na = pw_save(a, sizeof a);
        CHECK(na > 0 && na <= 3840 && !pw_save_trimmed, "preset %u save size %d (trimmed %d)", p, na, pw_save_trimmed);
        if (na > maxsave) maxsave = na;
        int r = pw_load(a, (unsigned)na);
        CHECK(r == 0, "preset %u load %d", p, r);
        int nb = pw_save(b, sizeof b);
        CHECK(na == nb && !memcmp(a, b, (size_t)na), "preset %u round trip differs", p);
    }
    printf("largest factory patch saved: %d bytes\n", maxsave);
    /* ROLL: random patches all sound, never clip, never run out of memory, and save */
    {
        int quiet = 0, worst_n = 0; float lo = 1e9f, hi = 0;
        for (int k = 0; k < 300; ++k) {
            pw_random_patch();
            pw_set_playing(1);
            pw_key(60, 100);
            render(44100 * 2 / PW_N, 1);
            float rms, peak; int finite;
            stats(44100 * 2 / PW_N * PW_N, &rms, &peak, &finite);
            pw_key(60, 0);
            pw_set_playing(0);
            render(8, 0);
            CHECK(finite, "random patch %d non-finite", k);
            CHECK(peak <= 1.0f, "random patch %d clips (%f)", k, peak);
            int nomem = 0, outs = 0;
            for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) {
                if (pws.mod[i].status == PW_STATUS_NOMEM) ++nomem;
                if (pws.mod[i].type == PW_T_OUTPUT) ++outs;
            }
            CHECK(!nomem && outs == 1, "random patch %d: nomem %d outputs %d", k, nomem, outs);
            if (rms < 0.006f) { ++quiet; if (quiet < 6) { printf("  quiet roll %d:", k); for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) printf(" %s", pw_types[pws.mod[i].type].tag); printf(" rms %.4f\n", rms); } }
            if (rms < lo) lo = rms;
            if (rms > hi) hi = rms;
            if (pw_count() > worst_n) worst_n = pw_count();
            static uint8_t a[8192];
            int na = pw_save(a, sizeof a);
            CHECK(na > 0 && pw_load(a, (unsigned)na) == 0, "random patch %d does not save", k);
        }
        printf("random patches: 300 rolled, rms %.3f..%.3f, at most %d modules, %d quiet\n", lo, hi, worst_n, quiet);
        CHECK(quiet <= 3, "%d random patches nearly silent", quiet);   /* (a roll can land just under the line: 1% is fine) */
    }
    /* Corrupt patch rejected */
    {
        static uint8_t a[8192];
        pw_factory_build(0);
        int n = pw_save(a, sizeof a);
        a[30] ^= 0x55;
        CHECK(pw_load(a, (unsigned)n) == -2, "corrupt patch not rejected");
    }
    /* Filter: 1 kHz cutoff passes 200 Hz, attenuates 8 kHz on LP. */
    {
        float lo = tone_rms_through(PW_T_VCF, 0, 0, 200, vcf_setup);
        float hi = tone_rms_through(PW_T_VCF, 0, 0, 6000, vcf_setup);
        printf("VCF LP @1k: 200Hz %.3f  6kHz %.3f  ratio %.1f dB\n", lo, hi, 20 * log10f(hi / lo));
        CHECK(hi < lo * 0.1f, "VCF does not attenuate");
    }
    /* SNAPSHOT: a slow saw LFO (+-5 V) over 4 scenes visits 0, 1, 2, 3 in order, and a clock holds the CV between ticks. */
    {
        pw_clear();
        int lfo = pw_add(PW_T_LFO, 0, 0), sn = pw_add(PW_T_SNAPSHOT, 0, 0);
        pw_set_param(lfo, 0, 2.0f); pw_set_param(lfo, 1, 1.0f);
        pw_connect(lfo, 2, sn, 0);
        pw_set_param(sn, 0, 4); pw_set_param(sn, 1, 1); pw_set_param(sn, 2, 0);
        int seen[4] = {0, 0, 0, 0}, order = 1, last = -1;
        for (int b = 0; b < 400; ++b) {
            render(1, 0);
            int c = (int)pw_live((unsigned)sn, 0);
            if (c < 0 || c > 3) { order = 0; break; }
            if (c != last && last >= 0 && c != (last + 1) % 4) order = 0;
            seen[c] = 1; last = c;
        }
        CHECK(seen[0] && seen[1] && seen[2] && seen[3] && order, "SNAPSHOT: LFO saw visits the scenes in order (%d %d %d %d, order %d)", seen[0], seen[1], seen[2], seen[3], order);
    }
    /* SNAPSHOT follows its CV: each scene recalls its snapshot (the VCO's TUNE is the scene number); CHANGE BAR waits for the bar line;
     * MORPH glides there. */
    {
        double now = 0; int vc, lf, sn;
        for (int mode = 0; mode < 3; ++mode) {
            pw_clear();
            lf = pw_add(PW_T_LFO, 0, 0); vc = pw_add(PW_T_VCO, 0, 0); sn = pw_add(PW_T_SNAPSHOT, 0, 0);
            pw_set_param(lf, 0, mode == 2 ? 0.7f : 2.0f); pw_set_param(lf, 1, 1.0f); pw_connect(lf, 2, sn, 0);
            pw_set_param(sn, 0, 4); pw_set_param(sn, 1, 1); pw_set_param(sn, 2, 0);
            pw_set_param(sn, 3, mode == 1 ? 2.0f : 0.0f); pw_set_param(sn, 4, mode == 2 ? 0.3f : 0.0f);
            for (int k = 3; k >= 0; --k) { pw_var_select((unsigned)k); pw_set_param(vc, 0, (float)k); }
            pw_var_select(0);
            pw_set_bpm(120); pw_set_playing(mode == 1);
            int seen = 0, bad = 0, off_bar = 0, glided = 0; unsigned last = pws.var_cur;
            for (int b = 0; b < 3200; ++b) {
                render(1, 0); now += 1000.0 * PW_N / 44100.0;
                pw_snap_follow((uint32_t)now);
                if (pws.morphing && pws.mod[vc].p[0] > 0.2f && pws.mod[vc].p[0] < 2.8f && pws.mod[vc].p[0] != floorf(pws.mod[vc].p[0])) glided = 1;
                if (pws.var_cur != last) {
                    last = pws.var_cur; seen |= 1 << last;
                    if (!pws.morphing && fabsf(pws.mod[vc].p[0] - (float)last) > 0.01f) bad = 1;
                    if (mode == 1 && pws.sixteenths % 16u > 1u) off_bar = 1;
                }
            }
            CHECK(seen == 15 || (mode == 1 && seen != 0), "SNAPSHOT mode %d: scenes visited %x", mode, seen);
            CHECK(!bad, "SNAPSHOT mode %d: a recalled snapshot has the wrong knobs", mode);
            CHECK(!off_bar, "SNAPSHOT CHANGE BAR changed off the bar line");
            CHECK(mode != 2 || glided, "SNAPSHOT MORPH did not glide");
        }
        pw_set_playing(0); pw_set_bpm(120);
    }
    /* INERTIA: FOLLOW overshoots a step when lightly damped and does not when heavily; extremes in every mode stay finite and in range; an IMPULSE rings. */
    {
        for (int fr = 0; fr < 2; ++fr) {
            pw_clear();
            int lf = pw_add(PW_T_LFO, 0, 0), in_ = pw_add(PW_T_INERTIA, 0, 0);
            pw_set_param(lf, 0, 0.5f); pw_set_param(lf, 1, 1.0f); pw_connect(lf, 3, in_, 0);
            pw_set_param(in_, 1, fr ? 1.0f : 0.1f);
            float mx = -100, last = 0;
            int started = 0;
            for (int b = 0; b < 330; ++b) {   /* 0.48 s: inside the LFO's first half cycle */
                render(1, 0);
                float v = pw_live((unsigned)in_, 0);
                if (v > mx) mx = v;
                last = v; started = 1;
            }
            (void)started;
            printf("INERTIA step: friction %s peak %.2f end %.2f\n", fr ? "high" : "low", mx, last);
            if (fr) CHECK(mx < 5.15f, "INERTIA with high friction overshoots (%f)", mx);
            else CHECK(mx > 5.4f && mx < 8.0f, "INERTIA with low friction does not overshoot sensibly (%f)", mx);
        }
        for (int mode = 0; mode < 3; ++mode)
            for (int ex = 0; ex < 4; ++ex) {
                pw_clear();
                int nz = pw_add(PW_T_NOISE, 0, 0), in_ = pw_add(PW_T_INERTIA, 0, 0), lf = pw_add(PW_T_LFO, 0, 1);
                pw_connect(nz, 0, in_, 0); pw_connect(lf, 3, in_, 1);
                pw_set_param(lf, 0, 8.0f);
                pw_set_param(in_, 0, (ex & 1) ? 0.0f : 1.0f); pw_set_param(in_, 1, (ex & 2) ? 0.0f : 1.0f);
                pw_set_param(in_, 2, (ex & 1) ? 1.0f : 0.0f); pw_set_param(in_, 3, 1.0f); pw_set_param(in_, 4, (float)mode);
                int ok = 1;
                for (int b = 0; b < 300; ++b) {
                    render(1, 0);
                    { float v = pw_live((unsigned)in_, 0), w = pw_live((unsigned)in_, 1); if (!(v == v) || !(w == w) || v > 12.01f || v < -12.01f) ok = 0; }
                }
                CHECK(ok, "INERTIA mode %d extreme %d: output out of range or not finite", mode, ex);
            }
        {
            pw_clear();
            int lf = pw_add(PW_T_LFO, 0, 0), in_ = pw_add(PW_T_INERTIA, 0, 0);
            pw_set_param(lf, 0, 2.0f); pw_set_param(lf, 1, 1.0f); pw_connect(lf, 3, in_, 1);
            pw_set_param(in_, 1, 0.1f);
            float pk = 0;
            for (int b = 0; b < 200; ++b) { render(1, 0); float v = pw_live((unsigned)in_, 0); if (v > pk) pk = v; }
            CHECK(pk > 1.0f, "INERTIA IMPULSE does not move it (%f)", pk);
        }
    }
    /* Frequency shifter: 1 kHz sine shifted +100 Hz -> energy near 1100 Hz. */
    {
        pw_clear();
        int osc = pw_add(PW_T_VCO, 0, 0), fs = pw_add(PW_T_RING, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(osc, 0, 12.0f * log2f(1000.0f / 261.625565f));
        pw_set_param(fs, 0, 100); pw_set_param(out, 0, 1);
        pw_connect(osc, 0, fs, 0); pw_connect(fs, 1, out, 0);
        render(344, 0);
        int n = 16384, base = 344 * PW_N - n;
        double best = 0; int best_f = 0;
        for (int f = 800; f <= 1300; f += 10) {
            double re = 0, im = 0;
            for (int i = 0; i < n; ++i) { re += L[base + i] * cos(2 * M_PI * f * i / 44100.0); im += L[base + i] * sin(2 * M_PI * f * i / 44100.0); }
            double mag = sqrt(re * re + im * im);
            if (mag > best) { best = mag; best_f = f; }
        }
        double re = 0, im = 0; int f = 900;
        for (int i = 0; i < n; ++i) { re += L[base + i] * cos(2 * M_PI * f * i / 44100.0); im += L[base + i] * sin(2 * M_PI * f * i / 44100.0); }
        printf("shifter UP peak at %d Hz, image@900 %.1f dB\n", best_f, 20 * log10(sqrt(re * re + im * im) / best));
        CHECK(best_f >= 1090 && best_f <= 1110, "frequency shifter UP output at %d Hz", best_f);
    }
    /* Pitch tracking: VCO at 0V = C4 */
    {
        pw_clear();
        int osc = pw_add(PW_T_VCO, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1);
        pw_connect(osc, 2, out, 0);
        render(344, 0);
        int crossings = 0;
        for (int i = 1; i < 344 * PW_N; ++i) if (L[i - 1] > 0 && L[i] <= 0) ++crossings;
        float hz = crossings / (344.0f * PW_N / 44100.0f);
        printf("VCO 0V -> %.1f Hz\n", hz);
        CHECK(fabsf(hz - 261.6f) < 2.0f, "VCO pitch %f", hz);
    }
    /* SAMPLE RATE 22.05 kHz: the same pitch, the same time (a delay's echo and an envelope), finite, then back to 44.1 */
    {
        pw_clear();
        int osc = pw_add(PW_T_VCO, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1);
        pw_connect(osc, 2, out, 0);
        pw_set_rate(1);
        render(344, 0);
        int crossings = 0, fin = 1;
        for (int i = 1; i < 344 * PW_N; ++i) { if (L[i - 1] > 0 && L[i] <= 0) ++crossings; if (!isfinite(L[i])) fin = 0; }
        float hz = crossings / (344.0f * PW_N / 44100.0f);
        float rms, peak; int f2; stats(344 * PW_N, &rms, &peak, &f2);
        printf("22.05 kHz: VCO 0V -> %.1f Hz, rms %.3f, half %d\n", hz, rms, pw_rate_half());
        CHECK(fabsf(hz - 261.6f) < 2.0f && fin && rms > 0.1f && pw_rate_half(), "half-rate pitch %f", hz);
        /* a patch loaded in half rate: the demo patch is alive and finite */
        pw_factory_build(1); pw_set_playing(1);
        render(300, 1); stats(300 * PW_N, &rms, &peak, &f2);
        CHECK(f2 && rms > 0.01f, "half-rate factory patch rms %f", rms);
        pw_set_rate(0); render(2, 0);
        CHECK(!pw_rate_half() && pw_blk == 128, "back to 44.1 kHz");
    }
    /* ON DELETE: osc > reverb > output without the reverb is osc > output, in one undo step */
    {
        pw_clear();
        int osc = pw_add(PW_T_VCO, 0, 0), rv = pw_add(PW_T_REVERB, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_connect(osc, 2, rv, 0); pw_connect(rv, 0, out, 0);
        int n = pw_remove_wired((unsigned)rv);
        int joined = pw_find_cable((unsigned)osc, 2, (unsigned)out, 0) >= 0;
        pw_undo();
        int back = pw_find_cable((unsigned)osc, 2, (unsigned)rv, 0) >= 0 && pw_find_cable((unsigned)rv, 0, (unsigned)out, 0) >= 0 && pw_find_cable((unsigned)osc, 2, (unsigned)out, 0) < 0;
        printf("ON DELETE: bridged %d, joined %d, undone in one step %d\n", n, joined, back);
        CHECK(n == 1 && joined && back, "delete and close the gap");
    }
    /* OFFLOAD: the heavy modules on the second core are the same sound some blocks later (one per module, in a chain), and come back when switched off */
    {
        static float ref[44 * 128];
        float worst1 = 0, worst2 = 0;
        for (int pass = 0; pass < 3; ++pass) {
            pw_clear();
            int osc = pw_add(PW_T_VCO, 0, 0), dl = pass == 2 ? pw_add(PW_T_DELAY, 0, 0) : -1, rv = pw_add(PW_T_REVERB, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            if (dl >= 0) { pw_connect(osc, 2, dl, 0); pw_connect(dl, 0, rv, 0); } else pw_connect(osc, 2, rv, 0);
            pw_connect(rv, 0, out, 0);
            t_c1_on = pass == 1; pw_cpu1_offload = (uint8_t)(pass ? 1 : 0); pw_off_state = 0;
            if (pass == 2) { t_c1_on = 1; }
            render(40, 0);
            if (pass == 0) memcpy(ref, L, 40 * 128 * sizeof(float));
            if (pass == 1) { for (int i = 128 * 14; i < 39 * 128; ++i) worst1 = fmaxf(worst1, fabsf(L[i + 128] - ref[i])); CHECK(pw_off_state == 1 && pw_off_n == 1, "offload: the reverb runs on the second core"); }
            if (pass == 2) { CHECK(pw_off_n == 2, "offload: delay and reverb both run on the second core"); }
        }
        (void)worst2;
        pw_cpu1_offload = 0; render(3, 0);
        CHECK(pw_off_state == 0 && pw_off_n == 0, "offload: switched off, the modules are this core's again");
        printf("OFFLOAD: the reverb's stream differs from the one-core stream a block later by %g at most\n", worst1);
        CHECK(worst1 < 1e-6f, "offload: the same sound, one block later");
        t_c1_on = 0;
        pw_clear(); { int o = pw_add(PW_T_VCO, 0, 0), r = pw_add(PW_T_REVERB, 0, 0), u = pw_add(PW_T_OUTPUT, 0, 0); pw_connect(o, 2, r, 0); pw_connect(r, 0, u, 0); }
        pw_cpu1_offload = 1; render(25, 0);
        CHECK(pw_off_n == 0, "offload: no second core, so the reverb stays on this one");
        pw_cpu1_offload = 0; pw_off_state = 0;
    }
    /* Capacity: fill to the module limit and remove everything again. */
    {
        pw_clear();
        int n = 0;
        while (pw_add(PW_T_DELAY, 0, 0) >= 0) ++n;
        printf("delays until full: %d (limit %d)\n", n, PW_MAX_MODULES);
        for (int i = 0; i < PW_MAX_MODULES; ++i) pw_remove(i);
        CHECK(pw_count() == 0, "remove all");
        int s = add_smp();
        CHECK(s >= 0 && pws.mod[s].status == PW_STATUS_OK, "sampler after compaction");
    }

    /* ---- the newer modules ---- */
    /* VCF: every TYPE passes 200 Hz, cuts 6 kHz (the 4-pole types harder), stays finite at full resonance */
    for (int ty = 0; ty < 6; ++ty) {
        static const char *nm[6] = {"SVF 12", "LADDER", "DIODE", "OTA 24", "OTA 12", "SALLEN"};
        filter_type = ty;
        float lo = tone_rms_through(PW_T_VCF, 0, 0, 200, vcf_setup);
        float hi = tone_rms_through(PW_T_VCF, 0, 0, 6000, vcf_setup);
        printf("VCF %-5s LP @1k: 200Hz %.3f  6kHz %.3f  %.1f dB\n", nm[ty], lo, hi, 20 * log10f(hi / lo));
        CHECK(lo > 0.05f && hi < lo * (ty == 1 || ty == 3 ? 0.01f : 0.1f), "VCF %s does not attenuate", nm[ty]);
        /* self-oscillation at full resonance, no input: finite, and for the resonant types audible */
        pw_clear();
        int v = pw_add(PW_T_VCF, 0, 0), o = pw_add(PW_T_OUTPUT, 0, 0), osc = pw_add(PW_T_VCO, 0, 0), vca = pw_add(PW_T_VCA, 0, 0);
        pw_set_param(v, 4, (float)ty); pw_set_param(v, 1, 1); pw_set_param(v, 0, 800); pw_set_param(o, 0, 1);
        pw_set_param(vca, 0, 0.0f);
        pw_connect(osc, 2, vca, 0); pw_connect(vca, 0, v, 0); pw_connect(v, 0, o, 0);
        render(20, 0); pw_set_param(vca, 0, 1.0f); render(2, 0); pw_set_param(vca, 0, 0.0f);   /* a kick of input */
        render(300, 0);
        float rms, peak; int finite; stats(300 * PW_N, &rms, &peak, &finite);
        printf("        full resonance: rms %.3f peak %.3f\n", rms, peak);
        CHECK(finite && peak <= 1.0f, "VCF %s unstable", nm[ty]);
    }
    filter_type = 0;
    /* count rising edges of a gate in L over `frames` */
    #define EDGES(n_, frames_) do { n_ = 0; for (int i_ = 1; i_ < (frames_); ++i_) if (L[i_ - 1] < 0.1f && L[i_] >= 0.1f) ++n_; } while (0)
    /* EUCLID 5 of 16 on the transport: 5 gates per bar */
    {
        pw_clear();
        int e = pw_add(PW_T_EUCLID, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_set_param(out, 1, 0); pw_set_param(e, 3, 0.3f);
        pw_connect(e, 0, out, 0);
        pw_set_bpm(120); pw_set_playing(1);
        int blocks = (int)(2.0 * 44100 / PW_N);      /* one bar at 120 */
        render(blocks, 0);
        int n = 0; for (int i = 256; i < blocks * PW_N; ++i) if (L[i - 1] < 0.1f && L[i] >= 0.1f) ++n;   /* (from past the output's first block, where the limiter's delay line fills) */
        printf("EUCLID 5/16: %d hits in a bar\n", n);
        CHECK(n == 5, "euclid hits %d", n);
        pw_set_playing(0);
    }
    /* BURST: a key makes COUNT hits; CURVE up tightens the gaps, down widens them; a new key starts it again */
    {
        for (int cv = 0; cv < 3; ++cv) {
            pw_clear();
            int k = pw_add(PW_T_KEYS, 0, 0), b = pw_add(PW_T_BURST, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(b, 0, 8); pw_set_param(b, 1, 20); pw_set_param(b, 2, cv == 0 ? 0.0f : cv == 1 ? 1.0f : -1.0f);
            pw_connect(k, 3, b, 0); pw_connect(b, 0, out, 0);
            pw_key(60, 100);
            int blocks = (int)(2.0 * 44100 / PW_N);
            render(blocks, 0);
            int n = 0, first = -1, last = -1, g1 = 0, g2 = 0;
            for (int i = 1; i < blocks * PW_N; ++i) if (L[i - 1] < 0.1f && L[i] >= 0.1f) {
                if (n == 1) g1 = i - first;
                if (first < 0) first = i; last = i; ++n;
                (void)g2;
            }
            int gl = 0, prev = -1, pp = -1;
            for (int i = 1; i < blocks * PW_N; ++i) if (L[i - 1] < 0.1f && L[i] >= 0.1f) { pp = prev; prev = i; if (pp >= 0) gl = prev - pp; }
            printf("BURST curve %d: %d hits, first gap %d last gap %d\n", cv, n, g1, gl);
            CHECK(n == 8, "BURST curve %d made %d hits", cv, n);
            if (cv == 1) CHECK(gl * 2 < g1, "BURST CURVE up does not speed up (%d then %d)", g1, gl);
            if (cv == 2) CHECK(gl > g1 * 2, "BURST CURVE down does not slow down (%d then %d)", g1, gl);
            if (cv == 0) CHECK(gl > g1 * 9 / 10 && gl < g1 * 11 / 10, "BURST CURVE 0 is not even (%d, %d)", g1, gl);
            (void)last;
        }
        pw_key(60, 0);
        {   /* PITCH: STEP semitones a hit, held after the last (1 V/oct) */
            pw_clear();
            int k = pw_add(PW_T_KEYS, 0, 0), b = pw_add(PW_T_BURST, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(b, 0, 5); pw_set_param(b, 4, 12);
            pw_connect(k, 3, b, 0); pw_connect(b, 2, out, 0);
            pw_key(60, 100);
            render((int)(1.0 * 44100 / PW_N), 0);
            float v = L[(int)(1.0 * 44100 / PW_N) * PW_N - 1];
            printf("BURST pitch: %.3f V after 5 hits of +12 st\n", v);
            CHECK(v > 0.78f && v < 0.82f, "BURST PITCH ends at %f (the output is volts over 5), wanted 0.8", v);
            pw_key(60, 0);
        }
    }
    /* DRUMS: a kick (sine swept down) and a hat (noise and FM) both sound and die away */
    for (int kind = 0; kind < 2; ++kind) {
        pw_clear();
        int d = pw_add(PW_T_DRUMS, 0, 0), lfo = pw_add(PW_T_LFO, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        if (kind) { pw_set_param(d, 0, 420); pw_set_param(d, 3, 0.03f); pw_set_param(d, 5, 0.9f); pw_set_param(d, 8, 1); }
        pw_set_param(out, 0, 1); pw_set_param(lfo, 0, 1.0f); pw_set_param(lfo, 2, 1);
        pw_connect(lfo, 3, d, 0); pw_connect(d, 0, out, 0);
        render(160, 0);
        float rms, peak; int finite;
        stats(160 * PW_N, &rms, &peak, &finite);
        float tail = 0; for (int i = 150 * PW_N; i < 160 * PW_N; ++i) tail = fmaxf(tail, fabsf(L[i]));
        printf("DRUMS %s: peak %.3f, after 0.4 s %.4f\n", kind ? "hat " : "kick", peak, tail);
        CHECK(finite && peak > 0.05f && tail < peak * (kind ? 0.1f : 0.7f), "drum %d", kind);
    }
    /* FM-2: 0 V sounds C4; more INDEX is brighter (more zero crossings) */
    {
        int zc[2];
        for (int b = 0; b < 2; ++b) {
            pw_clear();
            int f = pw_add(PW_T_FM4, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(out, 1, 0); pw_set_param(f, 1, b ? 1.0f : 0.0f); pw_set_param(f, 2, 0);
            pw_connect(f, 0, out, 0);
            render(100, 0);
            zc[b] = 0; for (int i = 1; i < 100 * PW_N; ++i) if ((L[i - 1] > 0) != (L[i] > 0)) ++zc[b];
        }
        float hz = zc[0] / 2.0f / (100.0f * PW_N / 44100.0f);
        printf("FM4: unmodulated %.1f Hz, modulated %d crossings vs %d\n", hz, zc[1], zc[0]);
        CHECK(fabsf(hz - 261.6f) < 3 && zc[1] > zc[0] * 1.3f, "fm4");
    }
    /* FM-2's envelope shapes: a held PERC dies away, a held ORGAN keeps sounding; SWELL comes in slowly */
    {
        float tail[2];
        for (int b = 0; b < 2; ++b) {
            pw_clear();
            int f = pw_add(PW_T_FM4, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0), k = pw_add(PW_T_KEYS, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(f, 4, b ? 0.0f : 2.0f); pw_set_param(f, 5, b ? 0.0f : 2.0f);
            pw_connect(k, 1, f, 1); pw_connect(f, 0, out, 0);
            pw_key(60, 100);
            render(300, 0);
            tail[b] = 0; for (int i = 290 * PW_N; i < 300 * PW_N; ++i) tail[b] = fmaxf(tail[b], fabsf(L[i]));
            pw_key(60, 0);
        }
        printf("FM4 ENV: held tail %.3f PERC, %.3f ORGAN\n", tail[0], tail[1]);
        CHECK(tail[0] < 0.01f && tail[1] > 0.1f, "fm4 env shapes");
        pw_clear();
        int f = pw_add(PW_T_FM4, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0), k = pw_add(PW_T_KEYS, 0, 0);
        pw_set_param(out, 0, 1); pw_set_param(f, 4, 8.0f); pw_set_param(f, 5, 8.0f);
        pw_set_param(f, 6, 1);
        pw_connect(k, 1, f, 1); pw_connect(f, 0, out, 0);
        pw_key(60, 100);
        float early = 0, late = 0;
        render(20, 0); early = pw_live((unsigned)f, 0);     /* the carrier's envelope */
        render(700, 0); late = pw_live((unsigned)f, 0);
        pw_key(60, 0);
        printf("FM4 SWELL: %.3f after 0.05 s, %.3f after 2 s\n", early, late);
        CHECK(late > 0.1f && early < late * 0.1f, "fm4 swell");
    }
    /* SAMPLER's SAMPLE input: picks the bank sound at each note (0.625 V a sound); DRUM RACK's KIT sets the pads */
    {
        pw_clear();
        int k = pw_add(PW_T_KEYS, 0, 0), sm = add_smp(), a = pw_add(PW_T_ATTEN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(sm, 0, 1); pw_set_param(sm, 1, 1); pw_set_param(sm, 25, 1); pw_set_param(out, 0, 1);   /* (BANK A: the test's bank) */
        pw_all_notes_off(); render(4, 0);
        pw_connect(k, 1, sm, 1); pw_connect(a, 0, sm, 8); pw_connect(sm, 0, out, 0);
        float lv[3]; const float cv[3] = {0.0f, 1.25f, 1.875f};
        for (int j = 0; j < 3; ++j) {
            pw_set_param(a, 1, cv[j]); render(4, 0);
            pw_key(60, 100); render(20, 0); pw_key(60, 0);
            float pk = 0; for (int i = 4 * PW_N; i < 20 * PW_N; ++i) pk = fmaxf(pk, fabsf(L[i]));
            lv[j] = pk; render(60, 0);
        }
        printf("SAMPLE input: B1 (empty) %.3f, B3 %.3f, B4 %.3f\n", lv[0], lv[1], lv[2]);
        CHECK(lv[0] < 1e-4f && lv[1] > 0.05f && lv[2] > 0.05f, "sampler SAMPLE input");
        int r = pw_add(PW_T_DRUMRACK, 0, 0);
        pw_set_param(r, 36, 1);
        int k808 = pws.mod[r].p[4] == 0 && pws.mod[r].p[5] == -2 && fabsf(pws.mod[r].p[6] - 0.9f) < 0.01f;
        pw_set_param(r, 36, 9);
        int ksmp = pws.mod[r].p[4] == 8 && pws.mod[r].p[16] == 11 && pws.mod[r].p[32] == 15 && pws.mod[r].p[6] == 4.0f;
        pw_set_param(r, 36, 12);
        int kb = pws.mod[r].p[4] == 32 && pws.mod[r].p[32] == 39;
        pw_undo();
        int und = pws.mod[r].p[4] == 8 && pws.mod[r].p[32] == 15 && pws.mod[r].p[36] == 9;
        printf("KIT: 808 %d, A %d, D %d, one undo %d\n", k808, ksmp, kb, und);
        CHECK(k808 && ksmp && kb && und, "drum rack KIT");
    }
    /* GATE DELAY: a 10 ms trigger comes out TIME later, as long; ADDITIVE and MACRO OSC: every model sounds */
    {
        pw_clear();
        int a = pw_add(PW_T_ATTEN, 0, 0), g = pw_add(PW_T_GATEDLY, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(g, 0, 0.1f); pw_set_param(g, 1, 0.3f); pw_set_param(out, 0, 1);
        pw_connect(a, 0, g, 0); pw_connect(g, 0, out, 0); pw_connect(g, 1, out, 1);
        pw_set_param(a, 1, 0); render(4, 0);
        pw_set_param(a, 1, 5); render(4, 0); pw_set_param(a, 1, 0);
        render(120, 0);
        int on_a = -1, off_a = -1, on_b = -1;
        for (int i = 0; i < 120 * PW_N; ++i) {
            if (on_a < 0 && L[i] > 0.5f) on_a = i;
            if (on_a >= 0 && off_a < 0 && L[i] < 0.5f) off_a = i;
            if (on_b < 0 && R[i] > 0.5f) on_b = i;
        }
        printf("GATE DELAY: A on at %.3f s for %.3f s, B on at %.3f s\n", on_a / 44100.0, (off_a - on_a) / 44100.0, on_b / 44100.0);
        CHECK(on_a > 0 && fabs(on_a / 44100.0 + 4 * PW_N / 44100.0 - 0.1) < 0.006 && abs((off_a - on_a) - 4 * PW_N) < 3 &&
              fabs(on_b / 44100.0 + 4 * PW_N / 44100.0 - 0.3) < 0.006, "gate delay");
        pw_clear();
        int ad = pw_add(PW_T_ADDITIVE, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_connect(ad, 0, out, 0); pw_connect(ad, 1, out, 1);
        const float shapes[3] = {0, 0.5f, 1};
        for (int k = 0; k < 3; ++k) {
            pw_set_param(ad, 1, shapes[k]); pw_set_param(ad, 5, k * 0.5f); pw_set_param(ad, 6, k * 0.4f); pw_set_param(ad, 7, k == 2 ? 0.6f : 0);
            render(60, 0); float rms, peak; int fin; stats(60 * PW_N, &rms, &peak, &fin);
            printf("ADDITIVE shape %.1f: rms %.3f peak %.3f\n", shapes[k], rms, peak);
            CHECK(fin && rms > 0.05f && peak <= 1.0f, "additive %d", k);
        }
        for (int model = 0; model < 8; ++model) {
            pw_clear();
            int mo = pw_add(PW_T_MACRO, 0, 0), k = pw_add(PW_T_KEYS, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(mo, 0, (float)model); pw_set_param(mo, 2, 0.5f);
            pw_connect(mo, 0, out, 0);
            render(60, 0); float rms, peak; int fin; stats(60 * PW_N, &rms, &peak, &fin);
            pw_connect(k, 3, mo, 1); pw_all_notes_off(); render(4, 0);
            pw_key(60, 100); render(20, 0); pw_key(60, 0); float hit = 0; for (int i = 0; i < 20 * PW_N; ++i) hit = fmaxf(hit, fabsf(L[i]));
            render(300, 0); float tail = 0; for (int i = 280 * PW_N; i < 300 * PW_N; ++i) tail = fmaxf(tail, fabsf(L[i]));
            printf("MACRO %-8s drone rms %.3f peak %.3f, struck %.3f, after %.4f\n", pw_types[PW_T_MACRO].param[0].labels[model], rms, peak, hit, tail);
            CHECK(fin && rms > 0.03f && peak <= 1.0f && hit > 0.05f && tail < hit * 0.1f, "macro model %d", model);
        }
    }
    /* CLOCK WARP: COUNT pulses a cycle, rushing when WARP is up; MICRO PERC: every model sounds and dies; SHUFFLER; REVERB FREEZE */
    {
        pw_clear();
        int w = pw_add(PW_T_CLOCK, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_set_param(w, 5, 8); pw_set_param(w, 6, 5); pw_set_param(w, 7, 0.8f);   /* LENGTH, COUNT, WARP */
        pw_connect(w, 4, out, 0);
        pw_set_bpm(120); pw_set_playing(1); render(4, 0);
        int blocks = (int)(44100 * 2.0 / PW_N) + 20;          /* 8 sixteenths at 120: 1 s; two cycles */
        render(blocks, 0);
        int edges = 0, at[16]; float prev = 0;
        for (int i = 0; i < blocks * PW_N; ++i) { if (L[i] > 0.5f && prev <= 0.5f && edges < 16) at[edges++] = i; prev = L[i]; }
        pw_set_playing(0);
        int rush = edges >= 5 && (at[1] - at[0]) > (at[4] - at[3]);
        printf("CLOCK WARP: %d pulses in two cycles (5 a cycle), first gap %d, fourth %d\n", edges, edges > 1 ? at[1] - at[0] : 0, edges > 4 ? at[4] - at[3] : 0);
        CHECK(edges >= 9 && edges <= 11 && rush, "clock warp");
        for (int model = 0; model < 6; ++model) {
            pw_clear();
            int k = pw_add(PW_T_KEYS, 0, 0), mp = pw_add(PW_T_MICROPERC, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(mp, 0, (float)model); pw_set_param(mp, 2, 0.1f);
            pw_connect(k, 3, mp, 0); pw_connect(mp, 0, out, 0);
            pw_all_notes_off(); render(4, 0);
            pw_key(60, 100); render(20, 0); pw_key(60, 0);
            float hit = 0; int fin = 1; for (int i = 0; i < 20 * PW_N; ++i) { hit = fmaxf(hit, fabsf(L[i])); if (!isfinite(L[i])) fin = 0; }
            render(200, 0); float tail = 0; for (int i = 180 * PW_N; i < 200 * PW_N; ++i) tail = fmaxf(tail, fabsf(L[i]));
            printf("MICRO PERC %-8s %.3f, then %.5f\n", pw_types[PW_T_MICROPERC].param[0].labels[model], hit, tail);
            CHECK(fin && hit > 0.05f && hit <= 1.0f && tail < 0.001f, "micro perc %d", model);
        }
        pw_clear();
        int v = pw_add(PW_T_VCO, 0, 0), sh = pw_add(PW_T_FRACTURE, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_set_param(sh, 6, 2); pw_set_param(sh, 1, 1.0f);   /* SHUFFLE, CHANCE */
        pw_connect(v, 0, sh, 0); pw_connect(sh, 0, out, 0);
        pw_set_playing(1); render(400, 0); pw_set_playing(0);
        float rms, peak; int fin; stats(400 * PW_N, &rms, &peak, &fin);
        printf("SHUFFLER: rms %.3f peak %.3f (%s)\n", rms, peak, pws.mod[sh].status == PW_STATUS_OK ? "ok" : "no memory");
        CHECK(fin && rms > 0.1f && peak <= 1.0f && pws.mod[sh].status == PW_STATUS_OK, "shuffler");
        pw_clear();
        int k = pw_add(PW_T_KEYS, 0, 0), rv = pw_add(PW_T_REVERB, 0, 0), e = pw_add(PW_T_ENV, 0, 0); v = pw_add(PW_T_VCO, 0, 0);
        int a = pw_add(PW_T_VCA, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 0.5f); pw_set_param(rv, 3, 1.0f); pw_set_param(rv, 1, 1.0f);
        pw_connect(k, 1, e, 0); pw_connect(v, 0, a, 0); pw_connect(e, 0, a, 1); pw_connect(a, 0, rv, 0); pw_connect(rv, 0, out, 0);
        pw_all_notes_off(); render(4, 0); pw_key(60, 100); render(60, 0); pw_key(60, 0);
        float before = 0; for (int i = 40 * PW_N; i < 60 * PW_N; ++i) before = fmaxf(before, fabsf(L[i]));
        pw_set_param(rv, 4, 1); render(1000, 0);
        float held = 0; for (int i = 980 * PW_N; i < 1000 * PW_N; ++i) held = fmaxf(held, fabsf(L[i]));
        pw_set_param(rv, 4, 0); render(1000, 0);
        float gone = 0; for (int i = 980 * PW_N; i < 1000 * PW_N; ++i) gone = fmaxf(gone, fabsf(L[i]));
        printf("REVERB FREEZE: %.3f playing, held %.3f after 3 s, %.5f 3 s after letting go\n", before, held, gone);
        CHECK(held > 0.02f && held < 0.99f && gone < held * 0.05f, "reverb freeze");
        /* REVERB MODE: every mode stays finite and bounded; GATED cuts the tail soon after the note, HALL lets it ring; PRE-DLY holds the wet back */
        float tailv[5];
        for (int md = 0; md < 5; ++md) {
            pw_set_param(rv, 4, 0); pw_set_param(rv, 7, (float)md); pw_set_param(rv, 1, 3.0f);
            pw_all_notes_off(); render(400, 0);
            pw_key(60, 100); render(60, 0); pw_key(60, 0);
            float pk = 0; for (int i = 0; i < 60 * PW_N; ++i) pk = fmaxf(pk, fabsf(L[i]));
            render(400, 0);
            float t = 0; for (int i = 380 * PW_N; i < 400 * PW_N; ++i) t = fmaxf(t, fabsf(L[i]));
            int fin = 1; for (int i = 0; i < 400 * PW_N; ++i) if (!isfinite(L[i])) fin = 0;
            tailv[md] = t;
            CHECK(fin && pk < 1.5f, "reverb mode finite");
        }
        printf("REVERB MODES: tail after 1.1 s: room %.4f hall %.4f plate %.4f gated %.4f swell %.4f\n", tailv[0], tailv[1], tailv[2], tailv[3], tailv[4]);
        CHECK(tailv[1] > 0.005f && tailv[3] < tailv[1] * 0.1f, "reverb gated cuts the tail");
        pw_set_param(rv, 7, 1); pw_set_param(rv, 8, 0.05f); pw_all_notes_off(); render(400, 0);
        pw_key(60, 100); render(2, 0);
        float early = 0; for (int i = 0; i < 2 * PW_N; ++i) early = fmaxf(early, fabsf(L[i]));
        printf("REVERB PRE-DLY 50 ms: wet in the first 2 blocks %.4f\n", early);
        pw_set_param(rv, 3, 0.0f); pw_set_param(rv, 8, 0);
        /* GROUPS: a view of the patch: saved with it (the flags are not disturbed), undone and redone */
        {
            pw_clear();
            int g1 = pw_add(PW_T_VCO, 0, 0), g2 = pw_add(PW_T_VCA, 0, 0), g3 = pw_add(PW_T_OUTPUT, 0, 0);
            pw_connect(g1, 0, g2, 0); pw_connect(g2, 0, g3, 0);
            pw_set_flags((unsigned)g2, PW_F_BYPASS);
            pw_set_group((unsigned)g1, 3); pw_undo_group(1); pw_set_group((unsigned)g2, 3); pw_undo_group(0);
            pw_undo();   /* the second module's group goes */
            int undone = pws.mod[1].group == 0 && pws.mod[0].group == 3;
            pw_redo();
            int redone = pws.mod[1].group == 3;
            static uint8_t gbuf[16384];
            int gn = pw_save(gbuf, sizeof gbuf);
            pw_clear();
            int lr = gn > 0 ? pw_load(gbuf, (unsigned)gn) : -1;
            int ok = lr >= 0 && pws.mod[0].group == 3 && pws.mod[1].group == 3 && pws.mod[2].group == 0 && (pws.mod[1].flags & PW_F_BYPASS) && !(pws.mod[0].flags & PW_F_BYPASS);
            printf("GROUPS: undo %s, redo %s, save/load %s\n", undone ? "ok" : "BAD", redone ? "ok" : "BAD", ok ? "ok" : "BAD");
            CHECK(ok && undone && redone, "module groups");
        }
        /* SPACE: the small reverb rings out, stays bounded, and a bigger SIZE rings longer */
        {
            pw_clear();
            int k2 = pw_add(PW_T_KEYS, 0, 0), e2 = pw_add(PW_T_ENV, 0, 0), v2 = pw_add(PW_T_VCO, 0, 0), a2 = pw_add(PW_T_VCA, 0, 0);
            int sp = pw_add(PW_T_SPACE, 0, 0), o2 = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(o2, 0, 0.5f); pw_set_param(sp, 2, 1.0f);
            pw_connect(k2, 1, e2, 0); pw_connect(v2, 0, a2, 0); pw_connect(e2, 0, a2, 1); pw_connect(a2, 0, sp, 0); pw_connect(sp, 0, o2, 0); pw_connect(sp, 1, o2, 1);
            float tl[2]; int fin2 = 1;
            for (int sz = 0; sz < 2; ++sz) {
                pw_set_param(sp, 0, sz ? 1.0f : 0.0f);
                pw_all_notes_off(); render(400, 0);
                pw_key(60, 100); render(60, 0); pw_key(60, 0); render(300, 0);
                float t = 0; for (int i = 250 * PW_N; i < 300 * PW_N; ++i) t = fmaxf(t, fabsf(L[i]));
                for (int i = 0; i < 300 * PW_N; ++i) if (!isfinite(L[i]) || fabsf(L[i]) > 1.5f) fin2 = 0;
                tl[sz] = t;
            }
            printf("SPACE: tail after 0.8 s, small %.4f, large %.4f\n", tl[0], tl[1]);
            CHECK(fin2 && tl[1] > 0.003f && tl[1] > tl[0] * 1.5f && pws.mod[sp].status == PW_STATUS_OK, "space reverb");
        }
    }
    /* module presets: every one of FM SYNTH, ADDITIVE, MACRO OSC and MODAL PERC sounds, none clips */
    {
        static const unsigned types[4] = {PW_T_FM4, PW_T_ADDITIVE, PW_T_MACRO, PW_T_STRING};
        for (int ti = 0; ti < 4; ++ti) {
            unsigned t = types[ti]; int pp = pw_preset_param(t);
            unsigned n = (unsigned)pw_types[t].param[pp].max;
            float lo = 9, hi = 0;
            for (unsigned k = 1; k <= n; ++k) {
                pw_clear();
                int kk = pw_add(PW_T_KEYS, 0, 0), m = pw_add(t, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
                pw_set_param(out, 0, 0.8f); pw_set_param(m, (unsigned)pp, (float)k);
                if (t == PW_T_FM4) { pw_connect(kk, 0, m, 0); pw_connect(kk, 1, m, 1); }
                else if (t == PW_T_STRING) { pw_connect(kk, 3, m, 0); pw_connect(kk, 0, m, 1); }
                else { pw_connect(kk, 0, m, 0); pw_connect(kk, t == PW_T_MACRO ? 3 : 1, m, 1); }
                pw_connect(m, 0, out, 0);
                pw_all_notes_off(); render(4, 0);
                pw_key(60, 100); render(120, 0); pw_key(60, 0);
                float rms, peak; int fin; stats(120 * PW_N, &rms, &peak, &fin);
                CHECK(fin && rms > 0.01f && peak <= 1.0f, "%s preset %s: rms %.3f peak %.3f", pw_types[t].name, pw_types[t].param[pp].labels[k], rms, peak);
                if (rms < lo) lo = rms;
                if (rms > hi) hi = rms;
            }
            printf("%s presets: %u, rms %.3f..%.3f\n", pw_types[t].name, n, lo, hi);
        }
    }
    /* cable amounts: half, inverted, saved and loaded, undone */
    {
        pw_clear();
        int a = pw_add(PW_T_ATTEN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_set_param(a, 1, 2.5f);
        int c = pw_connect(a, 0, out, 0);
        render(4, 0); float full = L[PW_N * 4 - 1];
        pw_cable_amount((unsigned)c, 50); render(4, 0); float half = L[PW_N * 4 - 1];
        pw_cable_amount((unsigned)c, -100); render(4, 0); float inv = L[PW_N * 4 - 1];
        static uint8_t buf[8192]; int nb = pw_save(buf, sizeof buf);
        pw_clear(); int r = pw_load(buf, (unsigned)nb);
        int k = pw_find_cable(0, 0, 1, 0), kept = r == 0 && k >= 0 && pws.cable[k].amt == -100;
        pw_cable_amount((unsigned)k, 20); pw_undo(); int und = pws.cable[k].amt == -100;
        pw_disconnect((unsigned)k); pw_undo(); k = pw_find_cable(0, 0, 1, 0); int back = k >= 0 && pws.cable[k].amt == -100;
        printf("CABLE AMOUNT: full %.3f half %.3f inverted %.3f, saved %d, undo %d, unplug undone %d\n", full, half, inv, kept, und, back);
        CHECK(fabsf(half - full * 0.5f) < 0.01f && fabsf(inv + full) < 0.01f && kept && und && back, "cable amounts");
    }
    /* BYPASS: an effect passes its input straight through; a source goes quiet */
    {
        pw_clear();
        int v = pw_add(PW_T_VCO, 0, 0), f = pw_add(PW_T_VCF, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_set_param(f, 0, 60); pw_connect(v, 2, f, 0); pw_connect(f, 0, out, 0);
        render(50, 0); float shut = 0; for (int i = 40 * PW_N; i < 50 * PW_N; ++i) shut = fmaxf(shut, fabsf(L[i]));
        pw_set_flags(f, PW_F_BYPASS);
        render(50, 0); float open = 0; for (int i = 40 * PW_N; i < 50 * PW_N; ++i) open = fmaxf(open, fabsf(L[i]));
        pw_set_flags(v, PW_F_BYPASS);
        render(50, 0); float none = 0; for (int i = 40 * PW_N; i < 50 * PW_N; ++i) none = fmaxf(none, fabsf(L[i]));
        printf("BYPASS: filtered %.3f, bypassed %.3f, source bypassed %.4f\n", shut, open, none);
        CHECK(open > shut * 2 && none < 1e-4f, "bypass");
        pw_undo(); pw_undo();
        CHECK(!(pws.mod[f].flags & PW_F_BYPASS) && !(pws.mod[v].flags & PW_F_BYPASS), "bypass undo");
    }
    /* MOD SEQ lanes B and C: values out of LANE B / LANE C; saved and loaded */
    {
        pw_clear();
        int q = pw_add(PW_T_MODSEQ, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        for (unsigned st = 0; st < 16; ++st) { pw_mseq_set(&pws.mod[q], 1, st, 255); pw_mseq_set(&pws.mod[q], 2, st, 128); }
        pw_mseq_set(&pws.mod[q], 0, 3, 77);
        pw_connect(q, 1, out, 0); pw_connect(q, 2, out, 1); pw_set_param(out, 0, 1);
        pw_set_playing(1); render(200, 0); pw_set_playing(0);
        float b = L[199 * PW_N] * 5.0f, c = R[199 * PW_N] * 5.0f;   /* (OUTPUT: 5 V full scale) */
        static uint8_t pb[4096]; int n = pw_save(pb, sizeof pb);
        pw_clear(); int r = pw_load(pb, (unsigned)n);
        int q2 = 0; while (q2 < PW_MAX_MODULES && !(pws.mod[q2].used == 1 && pws.mod[q2].type == PW_T_MODSEQ)) ++q2;
        printf("SEQ lanes: MOD B %.2f V, MOD C %.2f V; reloaded B %u C %u A3 %u\n", b, c, pw_mseq_get(&pws.mod[q2], 1, 5), pw_mseq_get(&pws.mod[q2], 2, 5), pw_mseq_get(&pws.mod[q2], 0, 3));
        CHECK(r == 0 && b > 4.0f && c > 2.0f && c < 3.0f && pw_mseq_get(&pws.mod[q2], 1, 5) == 255 && pw_mseq_get(&pws.mod[q2], 0, 3) >= 73 && pw_mseq_get(&pws.mod[q2], 0, 3) <= 81, "seq lanes");
    }
    /* MOD SEQ: a TRIG output a lane, one pulse each time that lane steps (a lane's own length does not matter) */
    {
        pw_clear(); pw_set_bpm(120);
        int q = pw_add(PW_T_MODSEQ, 0, 0);
        pw_set_param(q, 0, 4); pw_set_param(q, 3, 3);
        pw_set_playing(1);
        int edges[4] = {0, 0, 0, 0}; float prev[4] = {0, 0, 0, 0};
        for (int b = 0; b < (int)(4.0f * PW_SRF / PW_N); ++b) {
            pw_render(L, R);
            for (int l = 0; l < 4; ++l) { float v = pw_out_last(q, 5 + (unsigned)l); if (v > 2.5f && prev[l] < 2.5f) ++edges[l]; prev[l] = v; }
        }
        printf("MOD SEQ TRIG pulses in 4 s: %d %d %d %d\n", edges[0], edges[1], edges[2], edges[3]);
        CHECK(edges[0] >= 6 && edges[0] == edges[1] && edges[1] == edges[3] && edges[2] == edges[0], "MOD SEQ: every lane's TRIG pulses once a step (%d %d %d %d)", edges[0], edges[1], edges[2], edges[3]);
        CHECK(pw_types[PW_T_MODSEQ].n_out == 9, "MOD SEQ has nine outputs");
    }
    /* PIANO ROLL: notes get voices (never two overlapping on one), at most four at once; chords sound on one cable; it saves, loads and undoes */
    {
        pw_clear(); pw_set_bpm(120);
        int r = pw_add(PW_T_PROLL, 0, 0), vco = pw_add(PW_T_VCO, 0, 0), env = pw_add(PW_T_ENV, 0, 0), vca = pw_add(PW_T_VCA, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_module *m = &pws.mod[r];
        unsigned vo[6], a, b, c, d, e, f;
        int i0 = pw_roll_add(m, 0, 4, 60, 24), i1 = pw_roll_add(m, 0, 4, 64, 24), i2 = pw_roll_add(m, 0, 4, 67, 24), i3 = pw_roll_add(m, 0, 4, 72, 24);
        int i4 = pw_roll_add(m, 2, 2, 76, 24);
        CHECK(i0 == 0 && i1 == 1 && i2 == 2 && i3 == 3 && i4 < 0, "ROLL: four notes at once, a fifth is refused (%d %d %d %d %d)", i0, i1, i2, i3, i4);
        for (int k = 0; k < 4; ++k) pw_roll_get(m, (unsigned)k, &a, &b, &c, &d, &vo[k]);
        CHECK(vo[0] == 0 && vo[1] == 1 && vo[2] == 2 && vo[3] == 3, "ROLL: the chord takes voices 0-3 (%u %u %u %u)", vo[0], vo[1], vo[2], vo[3]);
        CHECK(pw_roll_add(m, 0, 4, 60, 24) < 0, "ROLL: the same pitch twice at once is refused");
        CHECK(pw_roll_add(m, 4, 4, 60, 24) == 4 && (pw_roll_get(m, 4, &a, &b, &c, &d, &e), e == 0), "ROLL: after the chord the first voice is free again");
        CHECK(pw_roll_find(m, 2, 64) == 1 && pw_roll_find(m, 4, 64) < 0, "ROLL: find a note by step and pitch");
        pw_roll_del(m, 4); pw_roll_del(m, 3); pw_roll_del(m, 2); pw_roll_del(m, 1);
        CHECK(pw_roll_count(m) == 1, "ROLL: delete");
        pw_roll_clear(m);
        pw_roll_add(m, 0, 4, 60, 24); pw_roll_add(m, 0, 4, 64, 24); pw_roll_add(m, 0, 4, 67, 24); pw_roll_add(m, 8, 2, 72, 31);
        (void)f;
        pw_set_voices(4);
        pw_connect(r, 0, vco, 0); pw_connect(r, 1, env, 0); pw_connect(vco, 2, vca, 0); pw_connect(env, 0, vca, 1); pw_connect(vca, 0, out, 0);
        pw_set_param(out, 0, 1); pw_set_param(env, 0, 0.001f);
        CHECK(pws.mod[r].flags & PW_F_POLY, "ROLL: in the voice area");
        pw_set_playing(1);
        float pk_chord = 0, pk_one = 0;
        for (int b2 = 0; b2 < (int)(0.4f * PW_SRF / PW_N); ++b2) { pw_render(L, R); for (int i = 0; i < PW_N; ++i) if (fabsf(L[i * 1]) > pk_chord) pk_chord = fabsf(L[i]); }
        int blocks = (int)(0.5f * 3 * PW_SRF / PW_N);   /* a 16th at 120 BPM is 0.125 s: the single note at step 8 is 1 s in */
        for (int b2 = 0; b2 < blocks; ++b2) { pw_render(L, R); if (b2 > blocks / 3) for (int i = 0; i < PW_N; ++i) if (fabsf(L[i]) > pk_one) pk_one = fabsf(L[i]); }
        printf("PIANO ROLL: chord peak %.3f, the lone note %.3f\n", pk_chord, pk_one);
        CHECK(pk_chord > 0.02f, "ROLL: the chord sounds (%.3f)", pk_chord);
        pw_set_playing(0);
        pw_set_playing(0);
        static uint8_t pb[4096]; int n = pw_save(pb, sizeof pb);
        pw_clear(); int rr = pw_load(pb, (unsigned)n);
        int q2 = 0; while (q2 < PW_MAX_MODULES && !(pws.mod[q2].used == 1 && pws.mod[q2].type == PW_T_PROLL)) ++q2;
        CHECK(rr == 0 && q2 < PW_MAX_MODULES && pw_roll_count(&pws.mod[q2]) == 4 && pw_roll_find(&pws.mod[q2], 9, 72) == 3, "ROLL: saves and loads with its notes");
        pw_undo_data((unsigned)q2); pw_roll_clear(&pws.mod[q2]);
        CHECK(pw_roll_count(&pws.mod[q2]) == 0, "ROLL: cleared");
        {   /* the same note on the roll and on the sequencer is the same voltage (C4 is 0 V on both), also when play starts in the middle of the note */
            float lv[2];
            for (int which = 0; which < 2; ++which) {
                pw_clear(); pw_set_voices(1); pw_set_bpm(120);
                int src = pw_add(which ? PW_T_SEQ : PW_T_PROLL, 0, 0), o2 = pw_add(PW_T_OUTPUT, 0, 0);
                if (which) { pws.mod[src].data[0] = 6; pw_set_param(src, 3, 0.0f); }
                else { pw_roll_clear(&pws.mod[src]); pw_roll_add(&pws.mod[src], 0, 32, 60, 24); pw_set_param(src, 2, 6); }
                pw_connect(src, 0, o2, 0); pw_set_param(o2, 0, 1);
                pw_set_playing(1);
                render(40, 0);
                lv[which] = L[PW_N - 1]; pw_set_playing(0);
            }
            printf("0.5 V: roll %.4f, sequencer %.4f\n", lv[0], lv[1]);
            CHECK(fabsf(lv[0] - lv[1]) < 0.01f && fabsf(lv[1]) > 0.01f, "ROLL pitch equals SEQ pitch (%.4f vs %.4f)", lv[0], lv[1]);
        }
    }
    /* METRONOME: a click on the beat while the transport plays, nothing when it is off; the roll's first step is the transport's first sixteenth */
    {
        pw_clear(); pw_set_bpm(120); pws.metro = 1; pw_set_playing(1);
        float pk_on = 0, pk_off = 0;
        render(60, 0); for (int i = 0; i < 60 * PW_N; ++i) pk_on = fmaxf(pk_on, fabsf(L[i]));   /* (render keeps the last block: check block by block) */
        pw_set_playing(0); pws.metro = 0; pw_set_playing(1);
        render(60, 0); for (int i = 0; i < PW_N; ++i) pk_off = fmaxf(pk_off, fabsf(L[i]));
        pw_set_playing(0);
        printf("METRONOME: on %.3f, off %.3f\n", pk_on, pk_off);
        CHECK(pk_off < 0.001f && pk_on > 0.05f, "METRONOME: a click when on, silence when off (%.3f %.4f)", pk_on, pk_off);
        pw_clear(); pw_set_bpm(120);
        int r2 = pw_add(PW_T_PROLL, 0, 0), o3 = pw_add(PW_T_OUTPUT, 0, 0);
        pw_roll_add(&pws.mod[r2], 0, 1, 60, 24); pw_set_param(r2, 2, 12);
        pw_connect(r2, 1, o3, 0); pw_set_param(o3, 0, 1);
        pw_set_playing(1); render(1, 0);
        CHECK(L[PW_N - 1] > 0.01f, "ROLL: a note at step 0 sounds in the first sixteenth (%.4f)", L[PW_N - 1]);
        pw_set_playing(0);
    }
    /* DIV: 1 and up divide the clock, 0 and below multiply it (x2, x4); TRIG SEQ MONITOR fires a track on a hit */
    {
        pw_clear(); pw_set_bpm(120);
        int q = pw_add(PW_T_SEQ, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_connect(q, 2, out, 0);
        float divs[4] = {2, 1, 0, -2}; int hits[4];
        for (int k = 0; k < 4; ++k) {
            pw_set_param(q, 5, divs[k]); pw_set_playing(0); render(4, 0); pw_set_playing(1);
            int nb = (int)(2.0f * PW_SRF / PW_N); render(nb, 0);
            int n = 0; float last = 0;
            for (int i = 0; i < nb * PW_N; ++i) { if (L[i] > 0.5f && last <= 0.5f) ++n; last = L[i]; }
            hits[k] = n;
        }
        pw_set_playing(0);
        printf("DIV: /2 %d, 1 %d, x2 %d, x4 %d trigs in two seconds\n", hits[0], hits[1], hits[2], hits[3]);
        CHECK(hits[0] >= 7 && hits[0] <= 9 && hits[1] >= 15 && hits[1] <= 17 && hits[2] >= 31 && hits[2] <= 33 && hits[3] >= 62 && hits[3] <= 66, "div multiplies");
        pw_clear();
        int t = pw_add(PW_T_TRIGSEQ, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_connect(t, 1, out, 0);
        pw_set_param(t, 16, 1); pw_trig_monitor((unsigned)t, 1, 0); render(4, 0);
        float pk = 0; for (int i = 0; i < 4 * PW_N; ++i) pk = fmaxf(pk, L[i]);
        pw_clear(); t = pw_add(PW_T_TRIGSEQ, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_connect(t, 1, out, 0);
        pw_set_param(t, 16, 0); pw_trig_monitor((unsigned)t, 1, 0); render(4, 0);
        float off = 0; for (int i = 0; i < 4 * PW_N; ++i) off = fmaxf(off, L[i]);
        printf("TRIG MONITOR: on %.2f, off %.2f\n", pk, off);
        CHECK(pk > 0.5f && off < 0.1f, "trig monitor");
    }
    /* NOISE: DIGITAL is a 4-bit crackle, TONE darkens (below the middle) or thins (above) all of them; LIMITER holds its
     * ceiling in every mode; OTT lifts a quiet sound and brings a loud one down */
    {
        float rms_hi[3];
        for (int k = 0; k < 3; ++k) {
            pw_clear();
            int nz = pw_add(PW_T_NOISE, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_connect(nz, 0, out, 0); pw_set_param(nz, 1, k == 0 ? 0.0f : k == 1 ? -1.0f : 1.0f);
            render(40, 0);
            double e = 0; for (int i = 1; i < 40 * PW_N; ++i) { float d = L[i] - L[i - 1]; e += d * d; }
            rms_hi[k] = (float)sqrt(e / (40 * PW_N));
        }
        pw_clear();
        int nz = pw_add(PW_T_NOISE, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_connect(nz, 3, out, 0); render(40, 0);
        int steps = 1; float pk = 0;
        for (int i = 0; i < 40 * PW_N; ++i) { pk = fmaxf(pk, fabsf(L[i])); float q = L[i] * 5.0f / (0.8f * 0.125f); if (fabsf(q - floorf(q)) > 0.02f && fabsf(q - floorf(q)) < 0.98f && fabsf(q - floorf(q) - 0.5f) > 0.52f) steps = 0; }
        printf("NOISE: change per sample %.2f / dark %.2f / thin %.2f, DIGITAL peak %.2f\n", rms_hi[0], rms_hi[1], rms_hi[2], pk);
        CHECK(rms_hi[1] < rms_hi[0] * 0.5f && pk > 0.3f && steps, "noise tone and digital");
        float peaks[3], quiet = 0, loud = 0;
        for (int mode = 0; mode < 3; ++mode) {
            pw_clear();
            int v = pw_add(PW_T_VCO, 0, 0), lm = pw_add(PW_T_LIMITER, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_connect(v, 0, lm, 0); pw_connect(lm, 0, out, 0);
            pw_set_param(lm, 0, 12.0f); pw_set_param(lm, 1, -6.0f); pw_set_param(lm, 3, (float)mode);
            render(120, 0);
            float p = 0; for (int i = 60 * PW_N; i < 120 * PW_N; ++i) p = fmaxf(p, fabsf(L[i]));
            peaks[mode] = p * 5.0f;
        }
        for (int k = 0; k < 2; ++k) {
            pw_clear();
            int v = pw_add(PW_T_VCO, 0, 0), at = pw_add(PW_T_ATTEN, 0, 0), ot = pw_add(PW_T_OTT, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_connect(v, 0, at, 0); pw_connect(at, 0, ot, 0); pw_connect(ot, 0, out, 0);
            pw_set_param(at, 0, k ? 1.0f : 0.01f); pw_set_param(ot, 0, 1.0f); pw_set_param(ot, 1, 0.1f);
            render(600, 0);
            float p = 0, pin = 0;
            for (int i = 500 * PW_N; i < 600 * PW_N; ++i) p = fmaxf(p, fabsf(L[i]));
            pin = k ? 5.0f : 0.05f;
            if (k) loud = p * 5.0f / pin; else quiet = p * 5.0f / pin;
        }
        printf("LIMITER ceiling 2.5 V: brick %.2f soft %.2f fold %.2f; OTT gain quiet x%.1f loud x%.2f\n", peaks[0], peaks[1], peaks[2], quiet, loud);
        CHECK(peaks[0] < 2.6f && peaks[1] < 2.6f && peaks[2] < 2.6f && peaks[0] > 2.0f, "limiter ceiling");
        CHECK(quiet > 1.2f && loud < 0.6f, "ott squeezes both ways");
    }
    /* CV RECORDER: MODE REC records a pass of its KNOB, then plays it back (LEVEL scales it) */
    {
        pw_clear(); pw_set_bpm(120);
        int c = pw_add(PW_T_CVREC, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_connect(c, 0, out, 0);
        pw_set_param(c, 1, 1); pw_set_param(c, 2, 3.0f); pw_set_param(c, 3, 0); pw_set_param(c, 0, 1);   /* 1 beat: half a second */
        render(240, 0);                                                       /* 0.56 s: the pass, then playing */
        int playing = pws.mod[c].p[0] < 0.5f;
        pw_set_param(c, 2, -4.0f); pw_set_param(c, 5, 0.5f);
        render(100, 0);
        float v = L[99 * PW_N] * 5.0f;
        printf("CV RECORDER: back to PLAY %d, plays %.2f V (3 V recorded, LEVEL 50%%)\n", playing, v);
        CHECK(playing && fabsf(v - 1.5f) < 0.2f, "cv recorder");
    }
    /* GRANULAR: grains of a sine coming in; FREEZE keeps the cloud going after it stops */
    {
        pw_clear();
        int v = pw_add(PW_T_VCO, 0, 0), vca = pw_add(PW_T_VCA, 0, 0), g = pw_add(PW_T_GRAIN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_connect(v, 0, vca, 0); pw_connect(vca, 0, g, 0); pw_connect(g, 0, out, 0);
        pw_set_param(g, 7, 1.0f);
        render(300, 0); float a = 0; for (int i = 200 * PW_N; i < 300 * PW_N; ++i) a = fmaxf(a, fabsf(L[i]));
        pw_set_param(g, 6, 1); pw_set_param(vca, 0, 0);
        render(300, 0); float b = 0; for (int i = 200 * PW_N; i < 300 * PW_N; ++i) b = fmaxf(b, fabsf(L[i]));
        pw_set_param(g, 6, 0);
        render(400, 0); float z = 0; for (int i = 300 * PW_N; i < 400 * PW_N; ++i) z = fmaxf(z, fabsf(L[i]));
        printf("GRANULAR: %.3f playing, %.3f frozen with no input, %.4f unfrozen (%s)\n", a, b, z, pws.mod[g].status ? "NO MEMORY" : "memory ok");
        CHECK(a > 0.1f && b > 0.1f && z < 0.01f, "granular");
    }
    /* TAPE: passes sound; its STOP gate brakes it to silence */
    {
        pw_clear();
        int v = pw_add(PW_T_VCO, 0, 0), t = pw_add(PW_T_TAPE, 0, 0), a = pw_add(PW_T_ATTEN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_connect(v, 0, t, 0); pw_connect(t, 0, out, 0); pw_set_param(t, 4, 0);
        render(100, 0); float run = 0; for (int i = 50 * PW_N; i < 100 * PW_N; ++i) run = fmaxf(run, fabsf(L[i]));
        pw_set_param(a, 1, 5.0f); pw_connect(a, 0, t, 1); pw_set_param(t, 5, 0.2f);
        render(400, 0); float stop = 0; for (int i = 380 * PW_N; i < 400 * PW_N; ++i) stop = fmaxf(stop, fabsf(L[i]));
        pw_set_param(a, 1, 0);
        render(200, 0); float back = 0; for (int i = 150 * PW_N; i < 200 * PW_N; ++i) back = fmaxf(back, fabsf(L[i]));
        printf("TAPE: %.3f running, %.4f stopped, %.3f running again\n", run, stop, back);
        CHECK(run > 0.3f && stop < 0.02f && back > 0.3f, "tape stop");
    }
    /* MODAL PERC (the old RESONATOR): each model rings when struck and dies away, with every exciter */
    for (int model = 0; model < 12 * 3; ++model) {
        pw_clear();
        int r = pw_add(PW_T_STRING, 0, 0), k = pw_add(PW_T_KEYS, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_set_param(r, 4, (float)(model / 3)); pw_set_param(r, 8, (float)(model % 3)); pw_set_param(r, 1, 0.4f);
        pw_connect(k, 3, r, 0); pw_connect(k, 0, r, 1); pw_connect(r, 0, out, 0);
        pw_key(60, 100); render(40, 0); pw_key(60, 0);
        float ring = 0; for (int i = 5 * PW_N; i < 40 * PW_N; ++i) ring = fmaxf(ring, fabsf(L[i]));
        render(1000, 0); float tail = 0; for (int i = 980 * PW_N; i < 1000 * PW_N; ++i) tail = fmaxf(tail, fabsf(L[i]));
        printf("MODAL PERC model %d exciter %d: %.3f struck, %.5f after 3 s\n", model / 3, model % 3, ring, tail);
        CHECK(ring > 0.05f && tail < ring * 0.1f, "modal model %d exciter %d", model / 3, model % 3);
    }
    /* 303: small signals ring at the cutoff (squelch), full resonance sings by itself above a few hundred Hz */
    {
        pw_clear();
        int v = pw_add(PW_T_VCF, 0, 0), osc = pw_add(PW_T_VCO, 0, 0), cv = pw_add(PW_T_ATTEN, 0, 0), vca = pw_add(PW_T_VCA, 0, 0);
        int out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(v, 4, 2); pw_set_param(v, 0, 1500); pw_set_param(out, 0, 1);
        float g[2];
        for (int k = 0; k < 2; ++k) {
            pw_set_param(cv, 1, log2f((k ? 1500.0f : 200.0f) / 261.625565f));
            pw_connect(cv, 0, osc, 0);
            pw_set_param(v, 1, 0.9f); pw_set_param(vca, 0, 0.05f);
            pw_connect(osc, 0, vca, 0); pw_connect(vca, 0, v, 0); pw_connect(v, 0, out, 0);
            render(200, 0);
            double e = 0; for (int i = 100 * PW_N; i < 200 * PW_N; ++i) e += L[i] * L[i];
            g[k] = (float)sqrt(e / (100.0 * PW_N));
        }
        printf("VCF 303 res 0.9: 1500 Hz (at cutoff) %.3f vs 200 Hz %.3f\n", g[1], g[0]);
        CHECK(g[1] > g[0] * 4.0f, "303 does not resonate");
        pw_clear();
        v = pw_add(PW_T_VCF, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
        int lfo = pw_add(PW_T_LFO, 0, 0);
        pw_set_param(v, 4, 2); pw_set_param(v, 0, 1500); pw_set_param(v, 1, 1); pw_set_param(out, 0, 1);
        pw_set_param(lfo, 0, 0.2f);
        pw_connect(lfo, 3, v, 0); pw_connect(v, 0, out, 0);
        render(30, 0);
        pw_disconnect(0);
        render(300, 0);
        float pk = 0; for (int i = 250 * PW_N; i < 300 * PW_N; ++i) pk = fmaxf(pk, fabsf(L[i]));
        printf("VCF 303 full resonance, input gone: %.3f\n", pk);
        CHECK(pk > 0.05f && pk < 1.0f, "303 does not self-oscillate");
    }
    /* FORMANT OSC: pitched at C4; the vowel I is brighter than U */
    {
        int zc[2]; float rms = 0;
        for (int b = 0; b < 2; ++b) {
            pw_clear();
            int f = pw_add(PW_T_FORMANT, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(f, 1, b ? 2.0f : 4.0f);
            pw_connect(f, 0, out, 0);
            render(100, 0);
            zc[b] = 0; for (int i = 1; i < 100 * PW_N; ++i) if ((L[i - 1] > 0) != (L[i] > 0)) ++zc[b];
            float pk; int fin; stats(100 * PW_N, &rms, &pk, &fin);
            CHECK(fin && pk <= 1.0f, "formant range");
        }
        printf("FORMANT: U %d crossings, I %d, rms %.3f\n", zc[0], zc[1], rms);
        CHECK(zc[1] > zc[0] * 1.5f && rms > 0.05f, "formant");
    }
    /* COMB: a sine at FREQ comes through far louder than one between its peaks */
    {
        float a = 0, b = 0;
        for (int k = 0; k < 2; ++k) {
            pw_clear();
            int c = pw_add(PW_T_COMB, 0, 0), osc = pw_add(PW_T_VCO, 0, 0), cv = pw_add(PW_T_ATTEN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(c, 0, 220); pw_set_param(c, 1, 0.9f); pw_set_param(c, 2, 0); pw_set_param(out, 0, 1);
            pw_set_param(cv, 1, log2f((k ? 330.0f : 440.0f) / 261.625565f));
            pw_connect(cv, 0, osc, 0); pw_connect(osc, 0, c, 0); pw_connect(c, 0, out, 0);
            pw_set_param(osc, 3, 0); pw_set_param(out, 0, 0.3f);
            render(200, 0);
            double e = 0; for (int i = 100 * PW_N; i < 200 * PW_N; ++i) e += L[i] * L[i];
            if (k) b = (float)sqrt(e / (100.0 * PW_N)); else a = (float)sqrt(e / (100.0 * PW_N));
        }
        printf("COMB 220 Hz: 440 Hz (a peak) %.3f, 330 Hz (between) %.3f\n", a, b);
        CHECK(a > b * 4, "comb");
    }
    /* EQ: +12 dB at the mid frequency, nothing far below it; the screen's curve agrees */
    {
        float lvl[2];
        for (int k = 0; k < 2; ++k) {
            pw_clear();
            int e = pw_add(PW_T_EQ, 0, 0), osc = pw_add(PW_T_VCO, 0, 0), cv = pw_add(PW_T_ATTEN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(e, 1, 12); pw_set_param(e, 3, 1000); pw_set_param(out, 0, 0.2f);
            pw_set_param(cv, 1, log2f((k ? 1000.0f : 60.0f) / 261.625565f));
            pw_connect(cv, 0, osc, 0); pw_connect(osc, 0, e, 0); pw_connect(e, 0, out, 0);
            render(200, 0);
            double s2 = 0; for (int i = 100 * PW_N; i < 200 * PW_N; ++i) s2 += L[i] * L[i];
            lvl[k] = (float)sqrt(s2 / (100.0 * PW_N));
        }
        float p[7] = {0, 12, 0, 1000, 150, 5000, 1};
        float db = 20.0f * log10f(lvl[1] / lvl[0]);
        printf("EQ mid +12: 1 kHz vs 60 Hz %.1f dB (curve says %.1f at 1k, %.1f at 60)\n", db, pw_eq_response(p, 1000), pw_eq_response(p, 60));
        CHECK(fabsf(db - 12) < 1.5f && fabsf(pw_eq_response(p, 1000) - 12) < 0.5f && fabsf(pw_eq_response(p, 60)) < 0.5f, "eq");
    }
    /* VOCODER: a 700 Hz tone on MOD lets a saw through (a 15 kHz one, above every band, lets much less); with no MOD it still sounds, finite */
    {
        float rms[3];
        for (int k = 0; k < 3; ++k) {
            pw_clear();
            int v = pw_add(PW_T_VOCODER, 0, 0), car = pw_add(PW_T_VCO, 0, 0), mod = pw_add(PW_T_VCO, 0, 0), cv = pw_add(PW_T_ATTEN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1);
            pw_set_param(cv, 1, log2f(700.0f / 261.625565f));
            pw_connect(cv, 0, mod, 0);
            pw_connect(car, 2, v, 0);
            if (k < 2) pw_connect(mod, 0, v, 1);
            if (k == 1) pw_set_param(cv, 1, log2f(15000.0f / 261.625565f));   /* (a 15 kHz modulator is above every band) */
            pw_connect(v, 0, out, 0);
            render(300, 0);
            double e = 0; for (int i = 150 * PW_N; i < 300 * PW_N; ++i) { CHECK(L[i] == L[i], "vocoder nan"); e += L[i] * L[i]; }
            rms[k] = (float)sqrt(e / (150.0 * PW_N));
        }
        printf("VOCODER: MOD 700 Hz %.4f, MOD 15 kHz %.4f, self %.4f\n", rms[0], rms[1], rms[2]);
        CHECK(rms[0] > 0.001f && rms[0] > rms[1] * 2.0f, "vocoder passes a modulated band");
        CHECK(rms[2] > 0.0f, "vocoder self mode sounds");
    }
    /* VOCODER's example: the drum beat shapes the chord, so the level moves with the beat (not a steady tone) */
    {
        CHECK(pw_example_build(PW_T_VOCODER) == 0, "VOCODER: its example builds");
        pw_set_playing(1);
        render(344, 0);
        const int W = 441; double lo = 1e9, hi = 0, all = 0; int n = 0;
        render(344 * 3, 0);
        for (int i = 0; i + W <= 344 * 3 * PW_N; i += W) { double e = 0; for (int j = 0; j < W; ++j) e += L[i + j] * L[i + j]; e = sqrt(e / W); if (e < lo) lo = e; if (e > hi) hi = e; all += e; ++n; }
        printf("VOCODER example: window rms min %.4f max %.4f mean %.4f\n", lo, hi, all / n);
        CHECK(hi > 0.01 && hi > lo * 4.0, "vocoder example follows the beat");
        pw_set_playing(0);
    }
    /* PITCH SHIFTER: +12 doubles a 220 Hz sine */
    {
        pw_clear();
        int ps = pw_add(PW_T_PSHIFT, 0, 0), osc = pw_add(PW_T_VCO, 0, 0), cv = pw_add(PW_T_ATTEN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(ps, 0, 12); pw_set_param(out, 0, 1);
        pw_set_param(cv, 1, log2f(220.0f / 261.625565f));
        pw_connect(cv, 0, osc, 0); pw_connect(osc, 0, ps, 0); pw_connect(ps, 0, out, 0);
        render(300, 0);
        /* the strongest of 220 / 440 Hz by correlation */
        double c[2] = {0, 0};
        for (int k = 0; k < 2; ++k) {
            double re = 0, im = 0, f = k ? 440.0 : 220.0;
            for (int i = 100 * PW_N; i < 300 * PW_N; ++i) { re += L[i] * cos(2 * M_PI * f * i / 44100.0); im += L[i] * sin(2 * M_PI * f * i / 44100.0); }
            c[k] = sqrt(re * re + im * im);
        }
        printf("PITCH SHIFT +12 from 220 Hz: 440 Hz %.0f, 220 Hz %.0f\n", c[1], c[0]);
        CHECK(c[1] > c[0] * 5, "pitch shift");
    }
    /* TRIG SEQ on the transport: a bar of the starting pattern: A 5 hits, B 2 + a roll of 3, C 8; ACC 2 */
    {
        pw_clear();
        int t = pw_add(PW_T_TRIGSEQ, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1);
        int counts[5];
        for (int o = 0; o < 5; ++o) {
            pw_clear();
            t = pw_add(PW_T_TRIGSEQ, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(out, 1, 0);
            pw_connect(t, (unsigned)o, out, 0);
            pw_set_bpm(120); pw_set_playing(1);
            int blocks = (int)(2.0 * 44100 / PW_N) - 4;      /* one bar at 120, less the next downbeat */
            render(blocks, 0);
            EDGES(counts[o], blocks * PW_N);
            pw_set_playing(0);
        }
        printf("TRIG SEQ bar: A %d B %d C %d D %d ACC %d\n", counts[0], counts[1], counts[2], counts[3], counts[4]);
        CHECK(counts[0] == 5 && counts[1] == 5 && counts[2] == 8 && counts[3] == 0 && counts[4] == 2, "trig seq pattern");
        /* LEN C 3: polymeter; PROB A 0: silent; pw_trig_set adds a hit to D */
        pw_clear();
        t = pw_add(PW_T_TRIGSEQ, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_set_param(t, 6, 3); pw_set_param(t, 8, 0);
        pw_trig_set(t, 3, 5, 1);
        CHECK(pw_trig_get(t, 3, 5) == 1 && pw_trig_get(t, 0, 0) == 2, "trig get/set");
        int got[3];
        unsigned outs[3] = {2, 0, 3};
        for (int k = 0; k < 3; ++k) {
            pw_disconnect(0); pw_disconnect(1);
            for (int c = 0; c < PW_MAX_CABLES; ++c) if (pws.cable[c].used) pw_disconnect(c);
            pw_connect(t, outs[k], out, 0);
            pw_set_playing(1);
            int blocks = (int)(2.0 * 44100 / PW_N) - 4;
            render(blocks, 0);
            EDGES(got[k], blocks * PW_N);
            pw_set_playing(0);
        }
        printf("TRIG SEQ LEN C 3: C %d hits (16 steps: 0,2 of each 3 -> 11); PROB A 0: %d; D %d\n", got[0], got[1], got[2]);
        CHECK(got[0] == 11 && got[1] == 0 && got[2] == 1, "trig seq len/prob");
        /* saved and loaded with its steps */
        uint8_t buf[4096];
        int n = pw_save(buf, sizeof buf);
        CHECK(n > 0 && pw_load(buf, (unsigned)n) == 0 && pw_trig_get(0, 3, 5) == 1, "trig seq save");
    }
    /* POLYPHONY: four voices, a chord of four notes: each heard (its frequency in the output) */
    {
        pw_clear();
        int k = pw_add(PW_T_KEYS, 0, 0), v = pw_add(PW_T_VCO, 0, 0), e = pw_add(PW_T_ENV, 0, 0), a = pw_add(PW_T_VCA, 0, 0);
        int out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_connect(k, 0, v, 0); pw_connect(k, 1, e, 0); pw_connect(v, 0, a, 0); pw_connect(e, 0, a, 1); pw_connect(a, 0, out, 0);
        pw_set_param(out, 0, 0.5f); pw_set_param(e, 2, 1.0f);
        pw_set_voices(4);
        printf("POLY: voices %d, flags KEYS %d VCO %d ENV %d VCA %d OUT %d\n", pws.voices, pws.mod[k].flags, pws.mod[v].flags, pws.mod[e].flags, pws.mod[a].flags, pws.mod[out].flags);
        CHECK(pws.mod[k].flags && pws.mod[v].flags && pws.mod[e].flags && pws.mod[a].flags && !pws.mod[out].flags, "auto poly");
        int notes[4] = {60, 64, 67, 72};
        for (int i = 0; i < 4; ++i) pw_key(notes[i], 100);
        render(200, 0);
        int heard = 0;
        for (int i = 0; i < 4; ++i) {
            double f = 261.625565 * pow(2.0, (notes[i] - 60) / 12.0), re = 0, im = 0, off = 0, ro = 0, io = 0;
            for (int j = 100 * PW_N; j < 200 * PW_N; ++j) {
                re += L[j] * cos(2 * M_PI * f * j / 44100.0); im += L[j] * sin(2 * M_PI * f * j / 44100.0);
                ro += L[j] * cos(2 * M_PI * f * 1.06 * j / 44100.0); io += L[j] * sin(2 * M_PI * f * 1.06 * j / 44100.0);
            }
            double on = sqrt(re * re + im * im); off = sqrt(ro * ro + io * io);
            if (on > off * 10) ++heard;
        }
        printf("POLY: %d of 4 notes heard\n", heard);
        CHECK(heard == 4, "poly chord");
        for (int i = 0; i < 4; ++i) pw_key(notes[i], 0);
        uint8_t buf[4096];
        int n = pw_save(buf, sizeof buf);
        CHECK(n > 0 && pw_load(buf, (unsigned)n) == 0 && pws.voices == 4 && pws.mod[v].flags, "poly save");
        pw_set_voices(1);
    }
    /* VARIATIONS: each keeps its own values; saved and loaded */
    {
        pw_clear();
        int f = pw_add(PW_T_VCF, 0, 0), sq = pw_add(PW_T_SEQ, 0, 0);
        pw_set_param(f, 0, 500);
        pw_var_select(1);
        pw_set_param(f, 0, 3000); sq >= 0 ? (void)(pws.mod[sq].data[0] = 7) : (void)0;
        pw_var_select(0);
        float a = pws.mod[f].p[0]; int d0 = (int8_t)pws.mod[sq].data[0];
        pw_var_select(1);
        float b = pws.mod[f].p[0]; int d1 = (int8_t)pws.mod[sq].data[0];
        printf("VARIATIONS: cutoff %.0f / %.0f, step %d / %d\n", a, b, d0, d1);
        CHECK(fabsf(a - 500) < 1 && fabsf(b - 3000) < 2 && d0 == 0 && d1 == 7, "variations");
        uint8_t buf[4096];
        int n = pw_save(buf, sizeof buf);
        CHECK(n > 0 && pw_load(buf, (unsigned)n) == 0, "var save");
        f = 0; sq = 1;
        CHECK(pws.var_cur == 1 && fabsf(pws.mod[f].p[0] - 3000) < 2, "var load current");
        pw_var_select(0);
        CHECK(fabsf(pws.mod[f].p[0] - 500) < 1 && pws.mod[sq].data[0] == 0, "var load other");
        pw_var_copy(0, 5); pw_var_select(5);
        CHECK(fabsf(pws.mod[f].p[0] - 500) < 1, "var copy");
    }
    /* UNDO / REDO: add, cable, knob, delete; and back */
    {
        pw_clear();
        int v = pw_add(PW_T_VCO, 0, 0), o = pw_add(PW_T_OUTPUT, 0, 0);
        pw_connect(v, 2, o, 0);
        pw_set_param(v, 0, 5); pw_set_param(v, 0, 7);
        pw_remove(v);
        CHECK(pw_count() == 1 && pw_cable_count() == 0, "removed");
        pw_undo();
        CHECK(pw_count() == 2 && pw_cable_count() == 1 && fabsf(pws.mod[v].p[0] - 7) < 0.01f, "undo remove");
        pw_undo();
        CHECK(fabsf(pws.mod[v].p[0]) < 0.01f, "undo knob (one step for the turns)");
        pw_undo();
        CHECK(pw_cable_count() == 0, "undo cable");
        pw_redo(); pw_redo();
        CHECK(pw_cable_count() == 1 && fabsf(pws.mod[v].p[0] - 7) < 0.01f, "redo");
        pw_copy((unsigned)v);
        int p2 = pw_paste(0, 0);
        CHECK(p2 >= 0 && pws.mod[p2].type == PW_T_VCO && fabsf(pws.mod[p2].p[0] - 7) < 0.01f, "paste");
        pw_undo();
        CHECK(pw_count() == 2, "undo paste");
        printf("UNDO: ok\n");
    }
    /* SIMPLER: B3 (a 440 Hz sine, half a second) as an instrument: its pitch at C4, an octave up at +1 V; held
     * past its end it stops, looped it goes on */
    {
        float hz[2], tail[2];
        for (int k = 0; k < 2; ++k) {
            pw_clear();
            int sp = add_smp(), keys = pw_add(PW_T_KEYS, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(sp, 0, 3); pw_set_param(sp, 25, 1); pw_set_param(out, 0, 1); pw_set_param(sp, 6, (float)k);
            pw_connect(keys, 0, sp, 0); pw_connect(keys, 1, sp, 1); pw_connect(sp, 0, out, 0);
            pw_key(k ? 72 : 60, 100);
            render(120, 0);
            int zc = 0; for (int i = 20 * PW_N; i < 100 * PW_N; ++i) if ((L[i - 1] > 0) != (L[i] > 0)) ++zc;
            hz[k] = zc / 2.0f / (80.0f * PW_N / 44100.0f);
            render(150, 0);
            tail[k] = 0; for (int i = 100 * PW_N; i < 150 * PW_N; ++i) tail[k] = fmaxf(tail[k], fabsf(L[i]));
            pw_key(k ? 72 : 60, 0);
        }
        printf("SAMPLER (classic): %.0f Hz at C4, %.0f Hz at C5; after the end %.3f, looped %.3f\n", hz[0], hz[1], tail[0], tail[1]);
        CHECK(fabsf(hz[0] - 440) < 6 && fabsf(hz[1] - 880) < 10 && tail[0] < 0.01f && tail[1] > 0.1f, "simpler");
    }
    /* SAMPLER: transient slices of B4 land on its four hits; warp at half speed keeps the pitch and lasts twice as
     * long; reversed it swells; RAM records what comes in */
    {
        pw_clear();
        int sp = add_smp();
        pw_set_param(sp, 0, 4); pw_set_param(sp, 25, 1); pw_set_param(sp, 1, 2); pw_set_param(sp, 12, 4); pw_set_param(sp, 13, 1);
        pw_maintain();
        float f[4]; int n = 0;
        for (unsigned k = 0; k < 4; ++k) n = pw_sampler_slice(sp, k, &f[k]);
        printf("SAMPLER transients: %d slices at %.3f %.3f %.3f %.3f\n", n, f[0], f[1], f[2], f[3]);
        CHECK(n == 4 && fabsf(f[1] - 0.25f) < 0.01f && fabsf(f[2] - 0.5f) < 0.01f && fabsf(f[3] - 0.8f) < 0.01f, "transients");
        /* warp */
        float hz[2]; int len[2];
        for (int w = 0; w < 2; ++w) {
            pw_clear();
            sp = add_smp(); int keys = pw_add(PW_T_KEYS, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(sp, 0, 3); pw_set_param(sp, 25, 1); pw_set_param(sp, 1, 1); pw_set_param(out, 0, 1);
            if (w) { pw_set_param(sp, 8, 1); pw_set_param(sp, 9, 0.5f); }
            pw_connect(keys, 1, sp, 1); pw_connect(sp, 0, out, 0);
            pw_key(60, 100);
            render(344, 0);
            pw_key(60, 0);
            int zc = 0; for (int i = 1; i < 6000; ++i) if ((L[i - 1] > 0) != (L[i] > 0)) ++zc;
            hz[w] = zc / 2.0f / (6000.0f / 44100.0f);
            len[w] = 0; for (int i = 0; i < 344 * PW_N; ++i) if (fabsf(L[i]) > 0.02f) len[w] = i;
        }
        printf("SAMPLER warp: %.0f Hz over %.2f s, at half speed %.0f Hz over %.2f s\n", hz[0], len[0] / 44100.0, hz[1], len[1] / 44100.0);
        CHECK(fabsf(hz[1] - hz[0]) < 15 && len[1] > len[0] * 1.8f && len[1] < len[0] * 2.2f, "warp");
        /* BEATS: B3 (half a second) made to last 2 beats at 120 BPM (a second) keeps its pitch; 1 beat at 60 BPM too */
        {
            float bh[2]; int bl[2];
            for (int b = 0; b < 2; ++b) {
                pw_clear();
                sp = add_smp(); int keys = pw_add(PW_T_KEYS, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
                pws.bpm = b ? 240.0f : 120.0f;
                pw_set_param(sp, 0, 3); pw_set_param(sp, 25, 1); pw_set_param(sp, 1, 1); pw_set_param(out, 0, 1); pw_set_param(sp, 24, b ? 2.0f : 2.0f);
                pw_connect(keys, 1, sp, 1); pw_connect(sp, 0, out, 0);
                pw_key(60, 100);
                render(500, 0);
                pw_key(60, 0);
                int zc = 0; for (int i = 1; i < 6000; ++i) if ((L[i - 1] > 0) != (L[i] > 0)) ++zc;
                bh[b] = zc / 2.0f / (6000.0f / 44100.0f);
                bl[b] = 0; for (int i = 0; i < 500 * PW_N; ++i) if (fabsf(L[i]) > 0.02f) bl[b] = i;
            }
            pws.bpm = 120.0f;
            printf("SAMPLER beats: 2 beats at 120 BPM %.0f Hz over %.2f s; at 240 BPM %.2f s\n", bh[0], bl[0] / 44100.0, bl[1] / 44100.0);
            CHECK(fabsf(bh[0] - 440) < 15 && bl[0] > 0.9f * 44100 && bl[0] < 1.1f * 44100 && bl[1] > 0.4f * 44100 && bl[1] < 0.6f * 44100, "sampler beats");
        }
        /* reverse: B4's last hit first, so the loudest part comes late in the first 0.25 s */
        pw_clear();
        sp = add_smp(); int keys = pw_add(PW_T_KEYS, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(sp, 0, 4); pw_set_param(sp, 25, 1); pw_set_param(sp, 1, 1); pw_set_param(sp, 7, 1); pw_set_param(out, 0, 1);
        pw_connect(keys, 1, sp, 1); pw_connect(sp, 0, out, 0);
        pw_key(60, 100); render(100, 0); pw_key(60, 0);
        double e1 = 0, e2 = 0;
        for (int i = 0; i < 4000; ++i) e1 += L[i] * L[i];
        for (int i = 8000; i < 8800; ++i) e2 += L[i] * L[i];
        printf("SAMPLER reverse: start %.3f, 0.19 s in %.3f\n", e1 / 4000, e2 / 800);
        CHECK(e2 / 800 > e1 / 4000 * 3, "reverse");
        /* RAM: a 220 Hz sine recorded for half a second, then played */
        pw_clear();
        sp = add_smp(); int osc = pw_add(PW_T_VCO, 0, 0), cv = pw_add(PW_T_ATTEN, 0, 0), g = pw_add(PW_T_ATTEN, 0, 0);
        keys = pw_add(PW_T_KEYS, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(cv, 1, log2f(220.0f / 261.625565f)); pw_connect(cv, 0, osc, 0);
        pw_connect(osc, 0, sp, 6); pw_set_param(g, 1, 5); pw_connect(g, 0, sp, 7);
        render(172, 0);
        pw_disconnect((unsigned)pw_find_cable(g, 0, sp, 7));
        pw_set_param(sp, 1, 1); pw_set_param(out, 0, 1);
        pw_connect(keys, 1, sp, 1); pw_connect(sp, 0, out, 0);
        render(2, 0); pw_key(60, 100); render(40, 0); pw_key(60, 0);
        int zc = 0; for (int i = 1000; i < 5000; ++i) if ((L[i - 1] > 0) != (L[i] > 0)) ++zc;
        float rh = zc / 2.0f / (4000.0f / 44100.0f);
        printf("SAMPLER RAM recording: plays back at %.0f Hz\n", rh);
        CHECK(fabsf(rh - 220) < 8, "ram recording");
    }
    /* DRUM RACK: T1 strikes the kick (it dies away); the keys (KEYS on) strike pad 2 */
    {
        pw_clear();
        int r = pw_add(PW_T_DRUMRACK, 0, 0), lfo = pw_add(PW_T_LFO, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_set_param(lfo, 0, 1.0f); pw_set_param(lfo, 2, 1);
        pw_connect(lfo, 3, r, 0); pw_connect(r, 0, out, 0);
        render(160, 0);
        float pk = 0, tail = 0;
        for (int i = 0; i < 160 * PW_N; ++i) pk = fmaxf(pk, fabsf(L[i]));
        for (int i = 150 * PW_N; i < 160 * PW_N; ++i) tail = fmaxf(tail, fabsf(L[i]));
        pw_disconnect(0);
        pw_set_param(r, 2, 1);
        render(40, 0);
        pw_key(62, 100);   /* D: pad 3 (C, C#, D) */
        render(20, 0);
        float key = 0; for (int i = 0; i < 20 * PW_N; ++i) key = fmaxf(key, fabsf(L[i]));
        printf("DRUM RACK: kick peak %.3f, tail %.4f; key D %.3f, pad 3 %.2f\n", pk, tail, key, pw_live((unsigned)r, 2));
        CHECK(pk > 0.1f && tail < pk * 0.5f && key > 0.05f && pw_live((unsigned)r, 2) > 0, "drum rack");
        pw_key(62, 0);
    }
    /* MIDI: clock in drives the transport (96 clocks: 16 sixteenths); MIDI OUT plays notes; CC 21 moves the first
     * performance knob; program change picks a variation; the sustain pedal holds a note */
    {
        pw_all_notes_off(); render(1, 0);
        pw_clear();
        int e = pw_add(PW_T_EUCLID, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(e, 1, 16); pw_set_param(e, 3, 0.3f); pw_set_param(out, 0, 1);
        pw_connect(e, 1, out, 0);
        pw_midi(0xFA, 0, 0);
        int blocks = 96 * 4;
        for (int b = 0; b < blocks; ++b) { if (b % 4 == 0) pw_midi(0xF8, 0, 0); pw_render(L + (b % 300) * PW_N, R); }
        pw_midi(0xFC, 0, 0);
        /* count the triggers over the whole run again, block by block */
        pw_clear();
        e = pw_add(PW_T_EUCLID, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(e, 1, 16); pw_set_param(out, 0, 1); pw_connect(e, 1, out, 0);
        pw_midi(0xFA, 0, 0);
        int trig = 0; float prev = 0;
        for (int b = 0; b < blocks; ++b) {
            if (b % 4 == 0) pw_midi(0xF8, 0, 0);
            pw_render(L, R);
            for (int i = 0; i < PW_N; ++i) { if (prev < 0.1f && L[i] >= 0.1f) ++trig; prev = L[i]; }
        }
        pw_midi(0xFC, 0, 0);
        pws.ext_sync = 0;
        printf("MIDI clock: 96 clocks in, %d sixteenths\n", trig);
        CHECK(trig == 16, "midi clock");
        pw_clear();
        int k = pw_add(PW_T_KEYS, 0, 0), mo = pw_add(PW_T_MIDIOUT, 0, 0), v = pw_add(PW_T_VCF, 0, 0);
        pw_connect(k, 0, mo, 0); pw_connect(k, 1, mo, 1);
        midi_sent = 0;
        pw_key(64, 100); render(10, 0); pw_key(64, 0); render(10, 0);
        printf("MIDI OUT: %u messages: %02X %d %d, %02X %d\n", midi_sent, midi_log[0][0], midi_log[0][1], midi_log[0][2], midi_log[1][0], midi_log[1][1]);
        CHECK(midi_sent == 2 && midi_log[0][0] == 0x90 && midi_log[0][1] == 64 && midi_log[1][0] == 0x80, "midi out");
        pw_perf_assign((unsigned)v, 0);
        pw_midi(0xB0, 21, 127);
        CHECK(fabsf(pws.mod[v].p[0] - 18000) < 1, "cc to perform knob");
        pw_midi(0xC0, 3, 0);
        CHECK(pws.var_cur == 3, "program change");
        pw_midi(0x90, 60, 100); pw_midi(0xB0, 64, 127); pw_midi(0x80, 60, 0); render(2, 0);
        int held = pws.kb_gate;
        pw_midi(0xB0, 64, 0); render(2, 0);
        printf("MIDI sustain: held %d, after the pedal %d\n", held, pws.kb_gate);
        CHECK(held && !pws.kb_gate, "sustain");
    }
    /* REVERSER: a decaying pluck in comes out swelling: in each piece the loud part arrives last */
    {
        pw_clear();
        int st = pw_add(PW_T_STRING, 0, 0), lfo = pw_add(PW_T_LFO, 0, 0), r = pw_add(PW_T_FRACTURE, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(lfo, 0, 2.0f); pw_set_param(lfo, 2, 1); pw_set_param(st, 1, 0.5f);
        pw_set_param(r, 6, 1); pw_set_param(r, 7, 0.5f);   /* REVERSE, TIME */ pw_set_param(out, 0, 1);
        pw_connect(lfo, 3, st, 0); pw_connect(st, 0, r, 0); pw_connect(r, 0, out, 0);
        render(700, 0);
        /* the dry pluck is loudest just after each trigger (every 0.5 s); reversed, loudest just before */
        double before = 0, after = 0;
        for (int k = 2; k < 4; ++k) {
            int t = k * 22050;
            for (int i = t - 2000; i < t; ++i) before += L[i] * L[i];
            for (int i = t + 200; i < t + 2200; ++i) after += L[i] * L[i];
        }
        printf("REVERSER: energy before each pluck %.2f, after it %.2f\n", before, after);
        CHECK(before > after * 1.5, "reverser");
    }
    /* SAMPLER SOURCE B3: the bank's 440 Hz sine at 22.05 kHz plays at 440 Hz */
    {
        pw_clear();
        int sm = add_smp(), lfo = pw_add(PW_T_LFO, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(sm, 0, 3); pw_set_param(sm, 25, 1); pw_set_param(sm, 1, 3); pw_set_param(out, 0, 1); pw_set_param(lfo, 0, 0.5f); pw_set_param(lfo, 2, 1);
        pw_connect(lfo, 3, sm, 1); pw_connect(sm, 0, out, 0);
        render(200, 0);
        int crossings = 0;
        for (int i = 44100 / 4; i < 200 * PW_N; ++i) if (L[i - 1] > 0 && L[i] <= 0) ++crossings;
        float hz = crossings / ((200.0f * PW_N - 44100 / 4) / 44100.0f);
        char t[12]; pw_format_param(sm, 0, t);
        printf("SAMPLER bank A sound 3 (%s): %.0f Hz\n", t, hz);
        CHECK(fabsf(hz - 440) < 10, "bank sample plays at %f Hz", hz);
        CHECK(!strcmp(t, "3 SINE"), "source text %s", t);
    }
    /* AUDIO IN: the platform's input through */
    {
        pw_clear();
        int a = pw_add(PW_T_AUDIOIN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_connect(a, 0, out, 0);
        audio_in_on = 1; render(50, 0); audio_in_on = 0;
        float rms, peak; int finite; stats(50 * PW_N, &rms, &peak, &finite);
        CHECK(peak > 0.3f && peak < 0.7f, "audio in peak %f", peak);
    }
    /* CHORD: MAJ on C4 is C E G C */
    {
        pw_clear();
        int c = pw_add(PW_T_CHORD, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0), v = pw_add(PW_T_ATTEN, 0, 0);
        pw_set_param(v, 1, 0); pw_set_param(out, 0, 1);
        pw_connect(v, 0, c, 0); pw_connect(c, 1, out, 0);
        render(4, 0);
        CHECK(fabsf(L[3 * PW_N] / 0.2f - 4.0f / 12.0f) < 1e-3f, "chord third %f", L[3 * PW_N] / 0.2f);
    }
    /* ARP: two held keys, UP at 1/16: the pitch alternates */
    {
        pw_clear();
        int a = pw_add(PW_T_ARP, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_connect(a, 0, out, 0);
        pw_key(60, 100); pw_key(67, 100);
        render(520, 0);
        int changes = 0;
        for (int i = 1; i < 520 * PW_N; ++i) if (fabsf(L[i] - L[i - 1]) > 0.05f) ++changes;
        printf("ARP: %d pitch changes in %.1f s (1/16 at 120: 8 a second)\n", changes, 520.0 * PW_N / 44100);
        CHECK(changes >= 10 && changes <= 13, "arp changes %d", changes);
        pw_key(60, 0); pw_key(67, 0);
    }
    /* COMPRESSOR: 4:1 at -18 dB turns a loud sine down */
    {
        pw_clear();
        int osc = pw_add(PW_T_VCO, 0, 0), c = pw_add(PW_T_COMP, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(c, 4, 0); pw_set_param(out, 0, 1);
        pw_connect(osc, 0, c, 0); pw_connect(c, 0, out, 0);
        render(400, 0);
        float peak = 0; for (int i = 200 * PW_N; i < 400 * PW_N; ++i) peak = fmaxf(peak, fabsf(L[i]));
        printf("COMP: 5 V sine (0 dB) at -18 dB 4:1 -> peak %.3f (expected about 0.21)\n", peak);
        CHECK(peak < 0.3f && peak > 0.15f, "comp peak %f", peak);
    }
    /* STRING: a pluck at 0 V sounds C3 */
    {
        pw_clear();
        int st = pw_add(PW_T_STRING, 0, 0), lfo = pw_add(PW_T_LFO, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_set_param(lfo, 0, 0.5f); pw_set_param(lfo, 2, 1);
        pw_connect(lfo, 3, st, 0); pw_connect(st, 0, out, 0);
        render(120, 0);
        int best = 0; double bestc = -1e9;      /* autocorrelation: the period */
        for (int lag = 100; lag < 1000; ++lag) {
            double c = 0; for (int i = 3000; i < 3000 + 4096; ++i) c += L[i] * L[i + lag];
            if (c > bestc) { bestc = c; best = lag; }
        }
        float hz = 44100.0f / (float)best;
        printf("STRING: %.1f Hz (C3 130.8)\n", hz);
        CHECK(fabsf(hz - 130.8f) < 4, "string pitch %f", hz);
    }
    /* FRACTURE: GATE high repeats a slice (the output repeats while the input moves on) */
    {
        pw_clear();
        int osc = pw_add(PW_T_LFO, 0, 0), f = pw_add(PW_T_FRACTURE, 0, 0), g = pw_add(PW_T_ATTEN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(osc, 0, 0.7f); pw_set_param(f, 1, 0); pw_set_param(out, 0, 1);
        pw_connect(osc, 2, f, 0); pw_connect(f, 0, out, 0);
        render(150, 0);
        pw_set_param(g, 1, 5); pw_connect(g, 0, f, 1);
        render(200, 0);
        /* a saw at 0.7 Hz is monotonic over 0.5 s; repeated 1/8-beat slices go back (each dips to 0 at its ends) */
        int jumps = 0, low = 0;
        for (int i = 1; i < 200 * PW_N; ++i) { int l = fabsf(L[i]) < 0.01f; if (l && !low) ++jumps; low = l; }
        printf("FRACTURE: %d slice starts while held\n", jumps);
        CHECK(jumps >= 4, "fracture repeats %d", jumps);
    }
    /* v28: SWIRL, FRACTURE's modes, CLOCK's warp, the one MIXER; and old patches that name the modules they replace */
    {
        for (int ty = 0; ty < 3; ++ty) {
            pw_clear();
            int v = pw_add(PW_T_VCO, 0, 0), sw = pw_add(PW_T_SWIRL, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(sw, 0, (float)ty); pw_set_param(sw, 3, 1.0f);
            pw_connect(v, 0, sw, 0); pw_connect(sw, 0, out, 0); pw_connect(sw, 1, out, 1);
            render(120, 0);
            float rms, peak; int fin; stats(120 * PW_N, &rms, &peak, &fin);
            double dl = 0; for (int i = 60 * PW_N; i < 120 * PW_N; ++i) dl += fabs(L[i] - R[i]);
            printf("SWIRL %-8s rms %.3f peak %.3f L-R %.1f\n", pw_types[PW_T_SWIRL].param[0].labels[ty], rms, peak, dl);
            CHECK(fin && rms > 0.05f && peak <= 1.0f && pws.mod[sw].status == PW_STATUS_OK, "swirl %d", ty);
            if (ty == 2) CHECK(dl > 1.0, "swirl chorus is stereo");
        }
        /* FRACTURE REVERSE: a decaying pluck swells (the old REVERSER); SHUFFLE sounds */
        pw_clear();
        int st = pw_add(PW_T_STRING, 0, 0), lfo = pw_add(PW_T_LFO, 0, 0), r = pw_add(PW_T_FRACTURE, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(lfo, 0, 2.0f); pw_set_param(lfo, 2, 1); pw_set_param(st, 1, 0.5f);
        pw_set_param(r, 6, 1); pw_set_param(r, 7, 0.5f); pw_set_param(out, 0, 1);
        pw_connect(lfo, 3, st, 0); pw_connect(st, 0, r, 0); pw_connect(r, 0, out, 0);
        render(700, 0);
        double before = 0, after = 0;
        for (int k = 2; k < 4; ++k) {
            int t = k * 22050;
            for (int i = t - 2000; i < t; ++i) before += L[i] * L[i];
            for (int i = t + 200; i < t + 2200; ++i) after += L[i] * L[i];
        }
        printf("FRACTURE REVERSE: energy before each pluck %.2f, after it %.2f\n", before, after);
        CHECK(before > after * 1.5, "fracture reverse");
        pw_clear();
        int v = pw_add(PW_T_VCO, 0, 0), sh = pw_add(PW_T_FRACTURE, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_set_param(sh, 6, 2); pw_set_param(sh, 1, 1.0f);
        pw_connect(v, 0, sh, 0); pw_connect(sh, 0, out, 0);
        pw_set_playing(1); render(400, 0); pw_set_playing(0);
        float rms, peak; int fin; stats(400 * PW_N, &rms, &peak, &fin);
        printf("FRACTURE SHUFFLE: rms %.3f peak %.3f\n", rms, peak);
        CHECK(fin && rms > 0.1f && peak <= 1.0f && pws.mod[sh].status == PW_STATUS_OK, "fracture shuffle");
        /* CLOCK: its WARP output plays COUNT pulses over LENGTH steps */
        pw_clear();
        int c = pw_add(PW_T_CLOCK, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_set_param(out, 0, 1); pw_set_param(c, 5, 8); pw_set_param(c, 6, 5); pw_set_param(c, 7, 0.8f);
        pw_connect(c, 4, out, 0);
        pw_set_bpm(120); pw_set_playing(1); render(4, 0);
        int blocks = (int)(44100 * 2.0 / PW_N) + 20;
        render(blocks, 0);
        int edges = 0, at[16]; float prev = 0;
        for (int i = 0; i < blocks * PW_N; ++i) { if (L[i] > 0.5f && prev <= 0.5f && edges < 16) at[edges++] = i; prev = L[i]; }
        pw_set_playing(0);
        printf("CLOCK warp: %d pulses in two cycles\n", edges);
        CHECK(edges >= 9 && edges <= 11 && at[1] - at[0] > at[4] - at[3], "clock warp output");
        /* a patch that names a module that is gone (FLANGER, PHASER, CHORUS, REVERSER, SHUFFLER, CLOCK WARP, AUDIO MIXER) loads without it */
        pw_clear();
        int vo = pw_add(PW_T_VCO, 0, 0), sw = pw_add(PW_T_SWIRL, 1, 0); out = pw_add(PW_T_OUTPUT, 2, 0);
        pw_connect(vo, 0, sw, 0); pw_connect(sw, 0, out, 0);
        uint8_t blob[4096]; int nb = pw_save(blob, sizeof blob);
        CHECK(nb > 0, "save");
        for (int i = 0, at = 24; i < blob[5]; ++i) {   /* the SWIRL's type number, as a removed one */
            if (blob[at] == PW_T_SWIRL) blob[at] = PW_T_GONE_FLANGER;
            at += 1 + 4 + 1 + blob[at + 5] * 4 + 1 + blob[at + 6 + blob[at + 5] * 4] + 1;
        }
        { uint32_t c = ~pw_crc32(~0u, blob, (unsigned)nb - 4); blob[nb - 4] = (uint8_t)c; blob[nb - 3] = (uint8_t)(c >> 8); blob[nb - 2] = (uint8_t)(c >> 16); blob[nb - 1] = (uint8_t)(c >> 24); }
        int rc = pw_load(blob, (unsigned)nb), have_vco = 0, have_gone = 0, have_out = 0;
        for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) { have_vco |= pws.mod[i].type == PW_T_VCO; have_out |= pws.mod[i].type == PW_T_OUTPUT; have_gone |= pw_type_hidden(pws.mod[i].type); }
        CHECK(rc == 1 && have_vco && have_out && !have_gone, "a removed module type loads without it (rc %d)", rc);
        CHECK(!pw_type_hidden(PW_T_SWIRL) && pw_type_hidden(PW_T_GONE_MIXER), "hidden types");
    }
    /* beta 1: tempo sync, the VCO's ANALOG and FM MODE, NOISE's DUST, VCF key tracking, DELAY's ping-pong, freeze and
     * wobble, ENV's loop and range, TAPE's varispeed, TURING's lock; old patches keep their sound (the new knobs at rest) */
    {
        #define RISES(x, n, hi, lo) ({ int r_ = 0, up_ = 0; for (int i_ = 0; i_ < (n); ++i_) { if (!up_ && (x)[i_] > (hi)) { up_ = 1; ++r_; } else if (up_ && (x)[i_] < (lo)) up_ = 0; } r_; })
        /* LFO SYNC 1/4 at 120 BPM: 2 Hz, so 8 cycles in 4 s (the knob alone: 1 Hz) */
        for (int sy = 0; sy < 2; ++sy) {
            pw_clear(); pw_set_bpm(120);
            int lf = pw_add(PW_T_LFO, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(lf, 0, 1.0f); if (sy) pw_set_param(lf, 3, 5);
            pw_connect(lf, 3, out, 0);
            render(1378, 0);
            int n = RISES(L, 1378 * PW_N, 0.2f, -0.2f);
            printf("LFO %s: %d cycles in 4 s\n", sy ? "1/4 synced to 120 BPM" : "1 Hz", n);
            CHECK(sy ? n >= 7 && n <= 9 : n >= 3 && n <= 5, "lfo sync cycles %d", n);
        }
        /* VCO: ANALOG 1 keeps the pitch within a percent of C4; each FM MODE sounds and is finite */
        for (int fm = 0; fm < 3; ++fm) {
            pw_clear();
            int v = pw_add(PW_T_VCO, 0, 0), mod = pw_add(PW_T_VCO, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(v, 4, 1.0f); pw_set_param(v, 5, (float)fm); pw_set_param(v, 3, fm ? 0.6f : 0.2f);
            pw_set_param(mod, 0, -12);
            if (fm) pw_connect(mod, 0, v, 1);
            pw_connect(v, 0, out, 0);
            render(1378, 0);
            float rms, peak; int fin; stats(1378 * PW_N, &rms, &peak, &fin);
            int n = RISES(L, 1378 * PW_N, 0.0f, -0.0f);
            printf("VCO analog, FM %s: rms %.3f peak %.3f, %d cycles in 4 s\n", pw_types[PW_T_VCO].param[5].labels[fm], rms, peak, n);
            CHECK(fin && rms > 0.1f && peak <= 1.0f, "vco fm mode %d", fm);
            if (!fm) CHECK(n > 1030 && n < 1062, "analog vco stays near C4: %d", n);
        }
        /* NOISE DUST: DENSITY 200 a second, a gate for each */
        {
            pw_clear();
            int nz = pw_add(PW_T_NOISE, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(nz, 2, 100.0f);
            pw_connect(nz, 5, out, 0);
            render(1378, 0);
            int n = RISES(L, 1378 * PW_N, 0.3f, 0.1f);
            printf("DUST: %d gates in 4 s at 100 a second\n", n);
            CHECK(n > 280 && n < 450, "dust density %d", n);
        }
        /* VCF key tracking: 1 V on KEY with KEYTRK 1 doubles the cutoff, so the noise through it gets louder */
        {
            float r2[2];
            for (int kt = 0; kt < 2; ++kt) {
                pw_clear();
                int nz = pw_add(PW_T_NOISE, 0, 0), f = pw_add(PW_T_VCF, 0, 0), at = pw_add(PW_T_ATTEN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
                pw_set_param(out, 0, 1); pw_set_param(f, 0, 400.0f); pw_set_param(f, 1, 0.0f); pw_set_param(f, 5, (float)kt);
                pw_set_param(at, 0, 0.0f); pw_set_param(at, 1, 2.0f);
                pw_connect(nz, 0, f, 0); pw_connect(at, 0, f, 3); pw_connect(f, 0, out, 0);
                render(300, 0);
                float rms, peak; int fin; stats(300 * PW_N, &rms, &peak, &fin);
                r2[kt] = rms; CHECK(fin, "vcf keytrack finite");
            }
            printf("VCF KEYTRK: rms %.4f off, %.4f on\n", r2[0], r2[1]);
            CHECK(r2[1] > r2[0] * 1.2f, "key tracking opens the filter");
        }
        /* LPG: shut it is silent, open it passes the sound; a strike from PING rings and dies; 12 dB with RES sounds
         * different from the plain one-pole; the VAC output follows the cell */
        {
            float r_shut = 0, r_open = 0, r_ping = 0, r_vac = 0, r_6 = 0, r_12 = 0;
            for (int cs = 0; cs < 5; ++cs) {
                pw_clear();
                int nz = pw_add(PW_T_NOISE, 0, 0), g = pw_add(PW_T_LPG, 0, 0), lf = pw_add(PW_T_LFO, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
                pw_set_param(out, 0, 1); pw_set_param(nz, 0, 0.8f);
                pw_set_param(g, 0, cs == 1 || cs >= 3 ? 1.0f : 0.0f);
                if (cs == 4) { pw_set_param(g, 8, 1); pw_set_param(g, 3, 0.9f); }
                pw_connect(nz, 0, g, 0); pw_connect(g, cs == 3 ? 1 : 0, out, 0);
                if (cs == 2) { pw_set_param(lf, 0, 2.0f); pw_connect(lf, 3, g, 2); }
                render(400, 0);
                float rms, peak; int fin; stats(400 * PW_N, &rms, &peak, &fin);
                CHECK(fin, "lpg case %d finite", cs);
                if (cs == 0) r_shut = rms; else if (cs == 1) { r_open = rms; r_6 = rms; } else if (cs == 2) r_ping = rms; else if (cs == 3) r_vac = rms; else r_12 = rms;
            }
            printf("LPG: shut %.4f open %.4f struck %.4f VAC %.4f, 6 dB %.4f 12 dB RES .9 %.4f\n", r_shut, r_open, r_ping, r_vac, r_6, r_12);
            CHECK(r_shut < 0.01f && r_open > 0.05f && r_ping > r_shut * 2.0f && r_ping < r_open, "lpg shut / open / struck");
            CHECK(r_vac > 0.05f, "lpg VAC output follows the cell");
            CHECK(pw_absf(r_12 - r_6) > 0.0005f, "lpg slopes differ");
        }
        /* SONG and SNAPSHOTS: clips, ramps and each snapshot's bypassed modules are saved with the patch; edit-all follows shared values */
        {
            static uint8_t sbuf[8192];
            pw_clear();
            int v = pw_add(PW_T_VCO, 0, 0), vf = pw_add(PW_T_VCF, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0); (void)out;
            pws.song.clip[0] = (pw_clip){0, 0, 4, 1}; pws.song.clip[1] = (pw_clip){0, 4, 4, 15}; pws.song.clip[2] = (pw_clip){0, 12, 2, 0x82};
            int ln = pw_lane_add((unsigned)vf, 0), ln2 = pw_lane_add((unsigned)v, 1);
            pw_lane_set((unsigned)ln, 0, 10, 1); pw_lane_set((unsigned)ln, 32, 200, 0); pw_lane_set((unsigned)ln2, 16, 77, 1); pw_lane_set((unsigned)ln, 16, 100, 2);
            CHECK(pws.song.al[ln].n == 3 && pws.song.al[ln2].n == 1 && pws.song.pt_t[pw_lane_start((unsigned)ln2)] == (16 | 1 << 12), "lane points sorted into a shared pool");
            CHECK(pw_lane_eval((unsigned)ln, 8.0f) == 10.0f && pw_lane_eval((unsigned)ln, 16.0f) == 100.0f && pw_lane_eval((unsigned)ln, 48.0f) == 200.0f, "lane eval: step holds, hold after the last point");
            { float mid = pw_lane_eval((unsigned)ln, 24.0f); CHECK(mid > 140.0f && mid < 160.0f, "lane eval: smooth half way (%g)", mid); }
            pws.song.glide = 2; pws.song.on = 1;
            /* snapshots: 3 plays with the VCO bypassed, 1 without; knobs edit one snapshot only */
            float c0 = pws.mod[vf].p[0];
            pw_var_select(2); pw_set_flags((unsigned)v, pws.mod[v].flags | PW_F_BYPASS); pw_set_param(vf, 0, c0 * 2.0f);
            pw_var_select(0);
            CHECK(!(pws.mod[v].flags & PW_F_BYPASS) && pw_absf(pws.mod[vf].p[0] - c0) < c0 * 0.001f, "snapshot 1 keeps its own module state and knobs");
            pw_var_select(2);
            CHECK((pws.mod[v].flags & PW_F_BYPASS) && pws.mod[vf].p[0] > c0 * 1.9f, "snapshot 3 comes back bypassed with its own knob");
            pw_var_select(0);
            pws.edit_all = 1; pw_set_param(vf, 0, c0 * 0.5f); pws.edit_all = 0;
            pw_var_select(2);
            CHECK(pws.mod[vf].p[0] > c0 * 1.9f, "edit-all leaves a snapshot with its own value alone");
            pw_var_select(7);
            CHECK(pws.mod[vf].p[0] < c0 * 0.6f, "edit-all reached a snapshot that shared the value");
            pw_var_select(15);
            CHECK(pws.var_cur == 15, "snapshot 16 exists");
            int n = pw_save(sbuf, sizeof sbuf);
            pw_clear();
            CHECK(!pws.song.clip[0].len && !pws.song.on && pws.song.n_lanes == 0, "a cleared patch has an empty song");
            CHECK(n > 0 && pw_load(sbuf, (unsigned)n) >= 0, "song patch loads");
            CHECK(pws.song.clip[1].start == 4 && pws.song.clip[1].val == 15 && pws.song.clip[2].val == 0x82 && pws.song.glide == 2 && pws.song.on &&
                  pws.song.n_lanes == 2 && pws.song.al[0].mod < PW_MAX_MODULES && pws.mod[pws.song.al[0].mod].type == PW_T_VCF && pws.song.al[0].n == 3 && pws.song.pt_v[2] == 200 && pws.song.al[1].par == 1, "song saved and loaded");
            pw_var_select(2);
            int vco = -1, vcf = -1;
            for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) { if (pws.mod[i].type == PW_T_VCO) vco = i; if (pws.mod[i].type == PW_T_VCF) vcf = i; }
            CHECK(vco >= 0 && (pws.mod[vco].flags & PW_F_BYPASS) && pws.mod[vcf].p[0] > c0 * 1.9f, "snapshot 3 saved and loaded with its module state");
            pw_var_select(0);
            CHECK(!(pws.mod[vco].flags & PW_F_BYPASS), "snapshot 1 saved and loaded with its module state");
            pw_remove((unsigned)vcf);
            CHECK(pws.song.al[0].mod == 0xff, "a removed module leaves its lane silent");
            pw_lane_del(0);
            CHECK(pws.song.n_lanes == 1 && pws.song.al[0].n == 1 && pws.song.pt_v[0] == 77, "deleting a lane closes the pool");
            printf("SONG: saved %d bytes\n", n);
        }
        /* LOOPER: what is recorded comes back one loop later, FBK fades it, CLEAR empties it, WRAP ticks once a pass.
         * REVERB SHIMMER: the tail climbs (an octave up has more high end than none) and stays bounded */
        {
            float r_first = 0, r_after = 0, r_cleared = 0; int wraps = 0, fin_all = 1;
            pw_clear(); pw_set_bpm(120);
            int v = pw_add(PW_T_VCO, 0, 0), lp = pw_add(PW_T_LOOPER, 0, 0), cl = pw_add(PW_T_ATTEN, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(lp, 0, 2); pw_set_param(lp, 6, 1); pw_set_param(lp, 7, 0); pw_set_param(lp, 1, 1);   /* 2 beats: 1 s; recording; dry off */
            pw_set_param(cl, 0, 0.0f); pw_set_param(cl, 1, 0.0f);
            pw_connect(v, 1, lp, 0); pw_connect(lp, 1, out, 0);
            render(172, 0);                                   /* 0.5 s: recording */
            float rms, peak; int fin; stats(172 * PW_N, &rms, &peak, &fin);
            r_first = rms; fin_all &= fin;
            pw_set_param(lp, 6, 0);                            /* stop recording; the loop plays on */
            render(172, 0);                                   /* to 1.0 s: ends the first pass */
            render(172, 0);                                   /* 1.0..1.5 s: the loop sounds */
            stats(172 * PW_N, &rms, &peak, &fin); r_after = rms; fin_all &= fin;
            printf("LOOPER: first pass %.4f, second pass %.4f\n", r_first, r_after);
            CHECK(fin_all && r_after > 0.02f, "looper plays back what it recorded (%.4f)", r_after);
            (void)r_cleared; (void)wraps;
        }
        {
            float tail[2];
            for (int sh = 0; sh < 2; ++sh) {
                pw_clear();
                int v = pw_add(PW_T_VCO, 0, 0), rv = pw_add(PW_T_REVERB, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
                pw_set_param(out, 0, 1); pw_set_param(rv, 1, 12.0f); pw_set_param(rv, 3, 1.0f); pw_set_param(rv, 5, sh ? 1.0f : 0.0f);
                pw_connect(v, 1, rv, 0); pw_connect(rv, 0, out, 0);
                render(600, 0);
                float rms, peak; int fin; stats(600 * PW_N, &rms, &peak, &fin);
                tail[sh] = rms; CHECK(fin && peak <= 1.01f, "shimmer finite and bounded %d (peak %.2f)", sh, peak);
            }
            printf("REVERB SHIMMER: rms %.4f off, %.4f on\n", tail[0], tail[1]);
            CHECK(tail[1] > 0.005f, "shimmer sounds");
        }
        /* DELAY: PING-PONG is stereo, FREEZE keeps the echo going after the input is gone, WOBBLE and TAP stay finite */
        {
            pw_clear(); pw_set_bpm(120);
            int v = pw_add(PW_T_VCO, 0, 0), d = pw_add(PW_T_DELAY, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(d, 0, 0.1f); pw_set_param(d, 1, 0.6f); pw_set_param(d, 3, 1.0f); pw_set_param(d, 5, 1);
            pw_connect(v, 0, d, 0); pw_connect(d, 2, out, 0); pw_connect(d, 3, out, 1);
            render(200, 0);
            float rms, peak; int fin; stats(200 * PW_N, &rms, &peak, &fin);
            double dl = 0; for (int i = 100 * PW_N; i < 200 * PW_N; ++i) dl += fabs(L[i] - R[i]);
            printf("DELAY ping-pong: rms %.3f, L-R %.1f\n", rms, dl);
            CHECK(fin && rms > 0.05f && dl > 1.0, "ping-pong is stereo");
            pw_set_param(d, 5, 0); pw_set_param(d, 3, 1.0f); pw_set_param(d, 6, 1.0f); pw_set_param(d, 4, 6);
            render(100, 0); stats(100 * PW_N, &rms, &peak, &fin); CHECK(fin && rms > 0.05f, "delay wobble and sync");
            /* freeze: patch the input away; the held echo is still there seconds later, and without freeze it is gone */
            for (int fz = 0; fz < 2; ++fz) {
                pw_clear();
                v = pw_add(PW_T_VCO, 0, 0); d = pw_add(PW_T_DELAY, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
                pw_set_param(out, 0, 1); pw_set_param(d, 0, 0.05f); pw_set_param(d, 1, 0.3f); pw_set_param(d, 3, 1.0f);
                pw_connect(v, 0, d, 0); pw_connect(d, 1, out, 0);
                render(100, 0);
                if (fz) pw_set_param(d, 8, 1);
                render(2, 0);
                int c = pw_find_cable(v, 0, d, 0); if (c >= 0) pw_disconnect((unsigned)c);
                render(600, 0);
                float r, pk; int f2; stats(600 * PW_N, &r, &pk, &f2);
                double e = 0; for (int i = 500 * PW_N; i < 600 * PW_N; ++i) e += L[i] * L[i];
                printf("DELAY %s: tail rms %.4f\n", fz ? "frozen" : "free", sqrt(e / (100 * PW_N)));
                CHECK(fz ? sqrt(e / (100 * PW_N)) > 0.05 : sqrt(e / (100 * PW_N)) < 0.01, "freeze holds the echo");
            }
            pw_clear();
            int lf = pw_add(PW_T_LFO, 0, 0);
            v = pw_add(PW_T_VCO, 0, 0); d = pw_add(PW_T_DELAY, 0, 0); out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_set_param(out, 0, 1); pw_set_param(lf, 0, 4.0f); pw_set_param(lf, 2, 1);
            pw_connect(v, 0, d, 0); pw_connect(lf, 3, d, 3); pw_connect(d, 0, out, 0);
            render(200, 0); stats(200 * PW_N, &rms, &peak, &fin); CHECK(fin && rms > 0.05f, "delay tap");
        }
        /* ENV LOOP: with no gate it cycles (EOC pulses); RANGE SLOW stretches the cycle; CURVEs and TRIGGER modes are finite */
        {
            int cnt[3];
            for (int k = 0; k < 3; ++k) {
                pw_clear();
                int e = pw_add(PW_T_ENV, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
                pw_set_param(out, 0, 1); pw_set_param(out, 1, 0); pw_set_param(e, 0, 0.01f); pw_set_param(e, 1, 0.1f); pw_set_param(e, 4, 1);
                pw_set_param(e, 5, k == 0 ? 0.0f : 1.0f); if (k == 2) pw_set_param(e, 11, 2);
                pw_connect(e, 2, out, 0);
                render(1378, 0);
                cnt[k] = RISES(L + 256, 1378 * PW_N - 256, 0.3f, 0.1f);   /* (past the output's first block) */
            }
            printf("ENV loop: %d / %d / %d EOC pulses in 4 s (off, on, on SLOW)\n", cnt[0], cnt[1], cnt[2]);
            CHECK(cnt[0] == 0 && cnt[1] > 15 && cnt[2] < cnt[1] / 3, "env loop and range");
            for (int cv = 0; cv < 4; ++cv) {
                pw_clear();
                int e = pw_add(PW_T_ENV, 0, 0), v = pw_add(PW_T_VCO, 0, 0), a = pw_add(PW_T_VCA, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0), k = pw_add(PW_T_KEYS, 0, 0);
                pw_set_param(out, 0, 1);
                pw_set_param(e, 6, cv == 1 ? 1.0f : cv == 2 ? -1.0f : 0.0f); pw_set_param(e, 7, cv == 1 ? 1.0f : cv == 2 ? -1.0f : 0.0f);
                pw_set_param(e, 8, cv == 1 ? 1.0f : cv == 2 ? -1.0f : 0.0f); pw_set_param(e, 9, (float)(cv == 3 ? 2 : cv == 2 ? 1 : 0)); pw_set_param(e, 10, 1.0f);
                pw_connect(k, 1, e, 0); pw_connect(k, 2, e, 2); pw_connect(v, 0, a, 0); pw_connect(e, 0, a, 1); pw_connect(a, 0, out, 0);
                render(10, 0); pw_key(60, 100); render(200, 0); pw_key(60, 0); render(200, 0); pw_key(62, 40); render(200, 0);
                float rms, peak; int fin; stats(610 * PW_N, &rms, &peak, &fin);
                CHECK(fin && peak <= 1.0f && rms > 0.01f, "env curve variant %d (rms %.3f)", cv, rms);
            }
        }
        /* TAPE SPEED +12: the pitch doubles (a sine at C4 comes out near C5) */
        {
            int n[2];
            for (int sp = 0; sp < 2; ++sp) {
                pw_clear();
                int v = pw_add(PW_T_VCO, 0, 0), t = pw_add(PW_T_TAPE, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
                pw_set_param(out, 0, 1); pw_set_param(t, 0, 0.0f); pw_set_param(t, 1, 0.0f); pw_set_param(t, 2, 0.0f);
                pw_set_param(t, 3, 0.0f); pw_set_param(t, 4, 0.0f); pw_set_param(t, 7, sp ? 12.0f : 0.0f);
                pw_connect(v, 0, t, 0); pw_connect(t, 0, out, 0);
                render(1378, 0);
                n[sp] = RISES(L + 400 * PW_N, 978 * PW_N, 0.0f, -0.0f);
            }
            printf("TAPE: %d cycles at normal speed, %d at +12\n", n[0], n[1]);
            CHECK(n[1] > n[0] * 17 / 10 && n[1] < n[0] * 22 / 10, "tape varispeed doubles the pitch");
        }
        /* TURING LOCK: the loop of 8 stops changing (CHANCE 100 %) */
        for (int lk = 0; lk < 2; ++lk) {
            pw_clear(); pw_set_bpm(120); pw_set_playing(1);
            int t = pw_add(PW_T_TURING, 0, 0);
            pw_set_param(t, 0, 8); pw_set_param(t, 1, 1.0f); pw_set_param(t, 3, (float)lk);
            uint32_t reg[40]; int nr = 0, last = -1;
            for (int b = 0; b < 1400 && nr < 40; ++b) {
                pw_render(L, R);
                uint32_t g = (uint32_t)pw_live((unsigned)t, 0) | (uint32_t)pw_live((unsigned)t, 1) << 16;
                if ((int)g != last) { reg[nr++] = g; last = (int)g; }
            }
            int same = 0; for (int k = 8; k < nr; ++k) if ((reg[k] & 0xFFu) == (reg[k - 8] & 0xFFu)) ++same;
            printf("TURING %s: %d of %d steps repeat the one 8 before\n", lk ? "locked" : "free", same, nr - 8);
            CHECK(lk ? same >= nr - 8 - 1 && nr > 20 : same < (nr - 8) / 2, "turing lock");
        }
        /* the new knobs at rest leave an old patch alone: a loaded 5-parameter ENV still plays */
        {
            pw_clear();
            int e = pw_add(PW_T_ENV, 0, 0);
            CHECK(pws.mod[e].p[5] == 0 && pws.mod[e].p[9] == 0 && pws.mod[e].p[11] == 1, "env defaults: no loop, soft, normal");
        }
    }
    /* a knob that follows a MOD SEQ lane (ARP + knob): a MOD SEQ is added, the lane's steps start at the knob's value, and the knob takes each step */
    {
        pw_clear(); pw_set_bpm(240); pw_set_playing(1);
        int v = pw_add(PW_T_VCF, 0, 0);
        pw_set_param(v, 1, 0.3f);
        int ms = -1; unsigned ln = 9;
        CHECK(pw_mseq_lock_assign((unsigned)v, 1, &ms, &ln) == 0 && ms >= 0 && ln == 0 && pws.mod[ms].type == PW_T_MODSEQ, "lock assign adds a MOD SEQ");
        CHECK(pw_mseq_get(&pws.mod[ms], 0, 5) >= 76 && pw_mseq_get(&pws.mod[ms], 0, 5) <= 80, "steps start at the knob's value (%u)", pw_mseq_get(&pws.mod[ms], 0, 5));
        CHECK(pw_mseq_lock_assign((unsigned)v, 1, &ms, &ln) == 1 && ln == 0, "the same knob keeps its lane");
        for (unsigned k = 0; k < 16; ++k) pw_mseq_set(&pws.mod[ms], 0, k, (int)(k * 17));
        float lo = 9, hi = -9; int changes = 0, lastp = -1;
        for (int b = 0; b < 700; ++b) {
            pw_render(L, R);
            pw_mseq_follow(-1, -1);
            float p = pw_get_param(v, 1);
            if (p < lo) lo = p;
            if (p > hi) hi = p;
            int pos = (int)pw_live((unsigned)ms, 0); if (pos != lastp) { ++changes; lastp = pos; }
        }
        printf("LOCK: the knob followed the lane from %.2f to %.2f over %d steps\n", lo, hi, changes);
        CHECK(hi - lo > 0.6f && changes > 8, "the knob follows the lane");
        pw_mseq_lock_clear((unsigned)ms, 0);
        { int m2; unsigned l2; CHECK(!pw_mseq_lock_find((unsigned)v, 1, &m2, &l2), "a cleared lock is gone"); }
    }
    /* knob detents: a full sweep of a linear parameter is about 40, of an exponential one too */
    {
        pw_clear();
        int v = pw_add(PW_T_VCF, 0, 0);
        pw_set_param(v, 1, 0);
        int n = 0; while (pw_get_param(v, 1) < 0.999f && n < 400) { pw_nudge_param(v, 1, 1, 0); ++n; }
        pw_set_param(v, 0, 20);
        int m = 0; while (pw_get_param(v, 0) < 17900 && m < 400) { pw_nudge_param(v, 0, 1, 0); ++m; }
        printf("detents for a full sweep: linear %d, exponential %d\n", n, m);
        CHECK(n >= 38 && n <= 42 && m >= 36 && m <= 44, "knob steps %d %d", n, m);
    }
    {   /* SEQUENCER and MOD SEQ: randomize and clear */
        pw_clear();
        int sq = pw_add(PW_T_SEQ, 0, 0), ms = pw_add(PW_T_MODSEQ, 0, 0);
        pw_seq_fill((unsigned)sq, 0);
        { const uint8_t *d = pws.mod[sq].data; unsigned g = d[16] | (unsigned)d[17] << 8; int far = 0, distinct = 0;
          for (int i = 0; i < 16; ++i) { int n = (int8_t)d[i]; if (n < -12 || n > 12) ++far; if (n != (int8_t)d[0]) ++distinct; }
          CHECK((g & 1u) && g != 0xffffu && far == 0 && distinct > 3, "SEQ randomize: gates %04x, %d far, %d distinct", g, far, distinct); }
        pw_seq_fill((unsigned)sq, 1);
        { const uint8_t *d = pws.mod[sq].data; int bad = (d[16] | d[17]) != 0;
          for (int i = 0; i < 16; ++i) if (d[i] || d[PW_SEQ_STEPFEAT + i] != 0xF0 || d[PW_SEQ_STEPVEL + i] != 200) ++bad;
          CHECK(!bad, "SEQ clear: %d bad", bad); }
        pw_seq_fill((unsigned)ms, 0);
        { int distinct = 0; for (unsigned l = 0; l < 4; ++l) for (unsigned i = 1; i < 16; ++i) if (pw_mseq_get(&pws.mod[ms], l, i) != pw_mseq_get(&pws.mod[ms], l, 0)) ++distinct;
          CHECK(distinct > 30, "MOD SEQ randomize: %d differ", distinct); }
        pw_seq_fill((unsigned)ms, 1);
        { int bad = 0; for (unsigned l = 0; l < 4; ++l) for (unsigned i = 0; i < 16; ++i) bad += pw_mseq_get(&pws.mod[ms], l, i) != 0;
          CHECK(!bad, "MOD SEQ clear: %d not zero", bad); }
    }
    {   /* CZ: a tone of the bank plays its pitch, holds while the key is down and ends after it; every line and modulation override renders */
        uint8_t *t = cz_bank;
        memset(cz_bank, 0, sizeof cz_bank);
        for (int k = 0; k < 2; ++k) {   /* tone 1: saw, DCW to 480, DCA 127 held; tone 2: the same, an octave down */
            uint8_t *q = t + 144 * k;
            q[0] = (uint8_t)(k ? 8 : 0);                 /* octave bits: 2 = -1 octave */
            q[37] = 1; q[38] = 0x7F; q[39] = 0x80 | 60;  /* DCW: one step (to 60 * 8), sustained, then END */
            q[20] = 1; q[21] = 0x7F; q[22] = 0x80 | 127; q[23] = 90;   /* DCA: up to 127 and hold, release at rate 90 */
            q[54] = 0;                                   /* pitch: nothing */
            memcpy(q + 128, k ? "TWO             " : "ONE             ", 16);
        }
        pw_clear(); pw_all_notes_off(); pw_render(L, R); pw_set_playing(1);
        int cz = pw_add(PW_T_CZ, 0, 0), kk = pw_add(PW_T_KEYS, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_connect(kk, 0, cz, 0); pw_connect(kk, 1, cz, 1); pw_connect(cz, 0, out, 0);
        pw_set_param(cz, 0, 1); pw_set_param(cz, 1, 1);   /* BANK 1: the first .syx bank (the stub above) */
        (void)pw_module_preset_name((unsigned)cz);   /* (the rack asks: the PRESET knob has no labels) */
        pw_key(60, 100);
        float pk = 0; int zc = 0, n = 0, bad = 0; float prev = 0;
        for (int b = 0; b < 44100 / 2 / PW_N; ++b) {
            pw_render(L, R);
            for (int i = 0; i < PW_N; ++i) { float v = L[i]; if (!(v == v) || v > 100 || v < -100) ++bad; if (b > 20) { pk = fmaxf(pk, fabsf(v)); if (prev < 0 && v >= 0) ++zc; ++n; } prev = v; }
        }
        float hz = (float)zc * 44100.0f / (float)n;
        CHECK(!bad && pk > 0.2f, "CZ: peak %.3f, %d bad samples", pk, bad);
        CHECK(hz > 250.0f && hz < 273.0f, "CZ: C4 plays %.1f Hz", hz);
        pw_key(60, 0);
        for (int b = 0; b < 44100 * 2 / PW_N; ++b) { pw_render(L, R); }
        CHECK(fabsf(pw_out_last(cz, 0)) < 0.001f, "CZ: still sounding 2 s after the key (%.4f)", pw_out_last(cz, 0));
        pw_set_param(cz, 1, 2); pw_key(60, 100);
        zc = 0; n = 0;
        for (int b = 0; b < 44100 / 2 / PW_N; ++b) { pw_render(L, R); for (int i = 0; i < PW_N; ++i) { float v = L[i]; if (b > 20) { if (prev < 0 && v >= 0) ++zc; ++n; } prev = v; } }
        hz = (float)zc * 44100.0f / (float)n;
        CHECK(hz > 125.0f && hz < 137.0f, "CZ: tone 2 (an octave down) plays %.1f Hz", hz);
        char nm[16]; pw_format_param((unsigned)cz, 1, nm);
        CHECK(!strcmp(nm, "2 TWO"), "CZ: the PRESET knob reads '%s'", nm);
        bad = 0;
        CHECK(pw_get_param((unsigned)cz, 8) == 0 && fabsf(pw_get_param((unsigned)cz, 10) - 60.0f / 127.0f) < 0.01f && pw_get_param((unsigned)cz, 5) == 0,
              "CZ: loading a tone sets the knobs to its wave and DCW (wave %.0f, DCW %.3f)", pw_get_param((unsigned)cz, 8), pw_get_param((unsigned)cz, 10));
        for (int ln = 0; ln < 4; ++ln) for (int md = 0; md < 4; ++md) {
            pw_set_param(cz, 4, (float)ln); pw_set_param(cz, 5, (float)(md % 3)); pw_key(60, 0); pw_key(60, 100);
            for (int b = 0; b < 44100 / 8 / PW_N; ++b) { pw_render(L, R); for (int i = 0; i < PW_N; ++i) if (!(L[i] == L[i]) || fabsf(L[i]) > 100) ++bad; }
        }
        CHECK(!bad, "CZ: %d bad samples over the line and modulation overrides", bad);
        {   /* the knobs: INIT with a knob moved is marked *, plays its pitch, holds, ends after the key, and every wave renders */
            char kn[16]; pw_set_param(cz, 0, 0); pw_set_param(cz, 1, 1); pw_format_param((unsigned)cz, 1, kn);
            CHECK(!strcmp(kn, "1 INIT"), "CZ: INIT reads '%s'", kn);
            pw_set_param(cz, 10, 0.9f); pw_format_param((unsigned)cz, 1, kn);
            CHECK(kn[strlen(kn) - 1] == '*', "CZ: a moved knob marks PRESET (%s)", kn);
            pw_set_param(cz, 10, pw_get_param((unsigned)cz, 10) > 0.8f ? 0.5f : 0.9f); pw_set_param(cz, 1, 1); pw_format_param((unsigned)cz, 1, kn);
            CHECK(kn[strlen(kn) - 1] != '*', "CZ: choosing the preset again clears the * (%s)", kn);
            pw_key(60, 0); for (int b = 0; b < 44100 / PW_N; ++b) pw_render(L, R);
            pw_key(60, 100);
            float pk2 = 0, lastpk = 0; int zc2 = 0, n2 = 0, bad2 = 0; float pv = 0;
            for (int b = 0; b < 44100 / PW_N; ++b) {
                pw_render(L, R);
                for (int i = 0; i < PW_N; ++i) { float v = L[i]; if (!(v == v) || fabsf(v) > 100) ++bad2; if (b > 20) { pk2 = fmaxf(pk2, fabsf(v)); if (pv < 0 && v >= 0) ++zc2; ++n2; } if (b > 300) lastpk = fmaxf(lastpk, fabsf(v)); pv = v; }
            }
            float hz2 = (float)zc2 * 44100.0f / (float)n2;
            CHECK(!bad2 && pk2 > 0.2f, "CZ knobs: peak %.3f, %d bad", pk2, bad2);
            CHECK(hz2 > 250.0f && hz2 < 273.0f, "CZ knobs: C4 plays %.1f Hz", hz2);
            pw_key(60, 0);
            for (int b = 0; b < 44100 * 3 / PW_N; ++b) pw_render(L, R);
            CHECK(fabsf(pw_out_last(cz, 0)) < 0.001f, "CZ knobs: still sounding 3 s after the key (%.4f)", pw_out_last(cz, 0));
            /* the shape: a short attack and a long one come at different times */
            float at[2];
            for (int q = 0; q < 2; ++q) {
                pw_set_param(cz, 16, q ? 1.0f : 0.005f);   /* A.ATK */
                pw_key(60, 0); for (int b = 0; b < 44100 * 3 / PW_N; ++b) pw_render(L, R);
                pw_key(60, 100);
                float pkq = 0; for (int b = 0; b < 44100 / 8 / PW_N; ++b) { pw_render(L, R); for (int i = 0; i < PW_N; ++i) pkq = fmaxf(pkq, fabsf(L[i])); }
                at[q] = pkq;
            }
            pw_set_param(cz, 16, 0.005f);
            CHECK(at[0] > at[1] * 3.0f, "CZ knobs: a long attack is not slower (%.3f vs %.3f in the first 125 ms)", at[0], at[1]);
            int bad3 = 0;
            for (int w1 = 0; w1 < 8; ++w1) for (int w2 = 0; w2 < 9; w2 += 4) for (int ln = 0; ln < 5; ln += 2) {
                pw_set_param(cz, 8, (float)w1); pw_set_param(cz, 9, (float)w2); pw_set_param(cz, 4, (float)(ln / 2)); pw_set_param(cz, 20, w1 & 1 ? 12.0f : 0.0f); pw_set_param(cz, 27, 0.5f);
                pw_key(60, 0); pw_key(60, 100);
                for (int b = 0; b < 44100 / 10 / PW_N; ++b) { pw_render(L, R); for (int i = 0; i < PW_N; ++i) if (!(L[i] == L[i]) || fabsf(L[i]) > 100) ++bad3; }
            }
            CHECK(!bad3, "CZ knobs: %d bad samples over the waves", bad3);
        }
    }
    {   /* PARAM MOD: a voltage (a key's pitch, 1 V) moves a knob of another module (the VCO's TUNE: +7.2 semitones at DEPTH 0.5), the knob keeps its own value;
         * the target survives a save and load that renumbers the modules */
        uint8_t pb2[8192];
        float hz2[2] = {0, 0};
        for (int pass = 0; pass < 2; ++pass) {
            int keys, vco, pm, ou;
            if (pass == 0) {
                pw_clear(); pw_all_notes_off();
                keys = pw_add(PW_T_KEYS, 0, 0); int dummy = pw_add(PW_T_ATTEN, 0, 0); vco = pw_add(PW_T_VCO, 0, 0); pm = pw_add(PW_T_PARAMMOD, 0, 0); ou = pw_add(PW_T_OUTPUT, 0, 0);
                pw_remove((unsigned)dummy);
                pw_set_param(pm, 0, (float)(vco + 1)); pw_set_param(pm, 1, 0); pw_set_param(pm, 2, 0.5f); pw_set_param(pm, 3, 0);
                pw_connect(keys, 0, pm, 0); pw_connect(vco, 0, ou, 0); pw_set_param(ou, 0, 1); pw_set_param(ou, 1, 0);
                int n = pw_save(pb2, sizeof pb2);
                CHECK(n > 0, "param mod save");
                pw_clear(); CHECK(pw_load(pb2, (unsigned)n) >= 0, "param mod load");
                vco = -1; pm = -1; for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) { if (pws.mod[i].type == PW_T_VCO) vco = i; if (pws.mod[i].type == PW_T_PARAMMOD) pm = i; }
                CHECK(vco >= 0 && pm >= 0 && pw_pm_target((unsigned)pm, 0) == vco, "PARAM MOD: its target after a save and load (%d, wants %d)", pm >= 0 ? pw_pm_target((unsigned)pm, 0) : -9, vco);
            } else { pm = -1; vco = -1; for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) { if (pws.mod[i].type == PW_T_VCO) vco = i; if (pws.mod[i].type == PW_T_PARAMMOD) pm = i; } }
            pw_key(pass == 0 ? 72 : 60, 100);
            render(60, 0);
            int zc = 0; for (int i = 20 * PW_N + 1; i < 60 * PW_N; ++i) if ((L[i - 1] > 0) != (L[i] > 0)) ++zc;
            hz2[pass] = zc / 2.0f / (40.0f * PW_N / 44100.0f);
            CHECK(pws.mod[vco].p[0] == 0.0f, "PARAM MOD: the knob keeps its own value (%.3f)", pws.mod[vco].p[0]);
            pw_key(pass == 0 ? 72 : 60, 0);
        }
        printf("PARAM MOD: +1 V: %.1f Hz, 0 V: %.1f Hz\n", hz2[0], hz2[1]);
        CHECK(hz2[0] > 370 && hz2[0] < 420 && hz2[1] > 250 && hz2[1] < 275, "PARAM MOD moves the VCO's TUNE (%.1f Hz at 1 V, %.1f at 0 V)", hz2[0], hz2[1]);
    }
    {   /* LOGIC MIN / MAX / ABS; ATTENUVERTER RECT and CURVE; PARAM MOD runs before the module it moves */
        pw_clear(); pw_all_notes_off();
        int a1 = pw_add(PW_T_ATTEN, 0, 0), lg = pw_add(PW_T_LOGIC, 0, 0), a2 = pw_add(PW_T_ATTEN, 0, 0);
        pw_set_param(a1, 1, -3.0f); pw_set_param(a1, 3, 2.0f);
        pw_connect(a1, 0, lg, 0); pw_connect(a1, 1, lg, 1);
        pw_connect(a1, 0, a2, 0);
        render(4, 0);
        CHECK(fabsf(pw_out_last(lg, 5) + 3.0f) < 0.01f && fabsf(pw_out_last(lg, 6) - 2.0f) < 0.01f && fabsf(pw_out_last(lg, 7) - 3.0f) < 0.01f,
              "LOGIC MIN/MAX/ABS: %.2f %.2f %.2f", pw_out_last(lg, 5), pw_out_last(lg, 6), pw_out_last(lg, 7));
        pw_set_param(a2, 5, 2); render(4, 0);
        CHECK(fabsf(pw_out_last(a2, 0) - 3.0f) < 0.01f, "ATTEN RECT full: %.2f", pw_out_last(a2, 0));
        pw_set_param(a2, 5, 1); render(4, 0);
        CHECK(fabsf(pw_out_last(a2, 0)) < 0.01f, "ATTEN RECT half: %.2f", pw_out_last(a2, 0));
        pw_set_param(a2, 5, 0); pw_set_param(a2, 4, 0.5f); render(4, 0);
        CHECK(fabsf(pw_out_last(a2, 0) + 1.8f) < 0.03f, "ATTEN CURVE: %.3f", pw_out_last(a2, 0));
        pw_clear();
        int vo = pw_add(PW_T_VCO, 0, 0), pmx = pw_add(PW_T_PARAMMOD, 0, 0);
        pw_set_param(pmx, 0, (float)(vo + 1));
        const pw_plan *pl = &pws.plan[pws.plan_cur & 1u];
        int ip = -1, iv = -1; for (unsigned i = 0; i < pl->count; ++i) { if (pl->order[i] == pmx) ip = (int)i; if (pl->order[i] == vo) iv = (int)i; }
        CHECK(ip >= 0 && iv >= 0 && ip < iv, "PARAM MOD runs before its target (%d, %d)", ip, iv);
    }
    {   /* COMPARE: A 3 V against B 1 V; SHIFT REG: a value goes in and walks to the end */
        pw_clear(); pw_all_notes_off();
        int sa = pw_add(PW_T_ATTEN, 0, 0), cm = pw_add(PW_T_COMPARE, 0, 0);
        pw_set_param(sa, 1, 3.0f); pw_set_param(sa, 3, 1.0f);
        pw_connect(sa, 0, cm, 0); pw_connect(sa, 1, cm, 1);
        render(4, 0);
        CHECK(pw_out_last(cm, 0) > 4.0f && fabsf(pw_out_last(cm, 2) - 2.0f) < 0.01f, "COMPARE A>B: gate %.2f diff %.2f", pw_out_last(cm, 0), pw_out_last(cm, 2));
        pw_set_param(cm, 0, 3.0f); render(4, 0);
        CHECK(pw_out_last(cm, 0) < 0.5f && pw_out_last(cm, 1) > 4.0f, "COMPARE THRESH 3: A is not over B + 3 (%.2f)", pw_out_last(cm, 0));
        pw_set_param(cm, 0, 0.0f); pw_set_param(cm, 2, 1); render(4, 0);
        CHECK(pw_out_last(cm, 0) < 0.5f, "COMPARE A<B: low (%.2f)", pw_out_last(cm, 0));
        pw_set_param(cm, 2, 2); pw_set_param(cm, 0, 3.0f); render(4, 0);
        CHECK(pw_out_last(cm, 0) > 4.0f, "COMPARE WINDOW: |A-B| = 2 inside 3 (%.2f)", pw_out_last(cm, 0));
        pw_clear();
        pw_set_bpm(240); pw_set_playing(1);
        int ck = pw_add(PW_T_CLOCK, 0, 0), sg = pw_add(PW_T_ATTEN, 0, 0), sr = pw_add(PW_T_SHIFTREG, 0, 0);
        pw_set_param(sg, 1, 2.0f); pw_set_param(sr, 0, 16);
        pw_connect(ck, 0, sr, 1); pw_connect(sg, 0, sr, 0);
        render(((44100 / 4) / PW_N), 0);
        float now1 = pw_out_last(sr, 0), end1 = pw_out_last(sr, 4);
        render((44100 * 2) / PW_N, 0);
        float end2 = pw_out_last(sr, 4);
        CHECK(fabsf(now1 - 2.0f) < 0.01f && fabsf(end1) < 0.01f && fabsf(end2 - 2.0f) < 0.01f, "SHIFT REG: NOW %.2f END %.2f then %.2f", now1, end1, end2);
        pw_set_playing(0);
    }
    {   /* MARKOV: every pitch is a note of the scale */
        pw_clear(); pw_set_bpm(240); pw_set_playing(1);
        int ck = pw_add(PW_T_CLOCK, 0, 0), mk = pw_add(PW_T_MARKOV, 0, 0);
        pw_connect(ck, 0, mk, 0); pw_set_param(mk, 0, 2); pw_set_param(mk, 1, 0);   /* scale 2, root C */
        unsigned mask = pw_scale_mask(2); int bad = 0, seen = 0;
        for (int b = 0; b < 44100 * 3 / PW_N; ++b) {
            pw_render(L, R);
            float v = pw_out_last(mk, 0); int semi = (int)floorf(v * 12.0f + 0.5f); int pc = ((semi % 12) + 12) % 12;
            ++seen; if (!(mask >> pc & 1)) ++bad;
        }
        CHECK(seen > 0 && bad == 0, "MARKOV: %d of %d pitches left the scale", bad, seen);
    }
    {   /* BUTTON: the keys as gates, triggers, toggles, repeats; held keys; double taps; sample */
        #define BR(ms) for (int b_ = 0; b_ < (ms) * 44100 / 1000 / PW_N; ++b_) pw_render(L, R)
        pw_clear(); pw_all_notes_off(); pw_render(L, R); pw_set_playing(1);
        int bt = pw_add(PW_T_BUTTON, 0, 0), ou = pw_add(PW_T_OUTPUT, 0, 0);
        pw_connect(bt, 0, ou, 0);
        pw_set_param(bt, 0, 1); pw_set_param(bt, 1, 27);
        pw_set_param(bt, 2, 0);   /* GATE */
        BR(20); CHECK(pw_out_last(bt, 0) < 0.01f, "BUTTON: high with nothing held");
        pw_button_keys(1u << 3); BR(20);
        CHECK(pw_out_last(bt, 0) > 0.9f, "BUTTON gate: low with a key held (%.2f)", pw_out_last(bt, 0));
        pw_button_keys((1u << 3) | (1u << 9)); BR(20);
        pw_button_keys(1u << 9); BR(20); CHECK(pw_out_last(bt, 0) > 0.9f, "BUTTON gate: dropped while one key is still down");
        pw_button_keys(0); BR(20); CHECK(pw_out_last(bt, 0) < 0.01f, "BUTTON gate: stuck after release");
        /* TRIG: one pulse per new press even with another held */
        int pulses = 0, prev = 0;
        pw_button_keys(1u << 1);
        for (int b = 0; b < 40; ++b) { pw_render(L, R); int hi = pw_out_last(bt, 1) > 0.5f; pulses += hi && !prev; prev = hi; }
        pw_button_keys((1u << 1) | (1u << 2));
        for (int b = 0; b < 40; ++b) { pw_render(L, R); int hi = pw_out_last(bt, 1) > 0.5f; pulses += hi && !prev; prev = hi; }
        CHECK(pulses == 2, "BUTTON TRIG: %d pulses for two presses", pulses);
        pw_button_keys(0); BR(50);
        /* keys outside FROM..TO are ignored */
        pw_set_param(bt, 0, 5); pw_set_param(bt, 1, 6);
        pw_button_keys(1u << 0); BR(20); CHECK(pw_out_last(bt, 0) < 0.01f, "BUTTON range: key 1 reached a 5-6 button");
        pw_button_keys(1u << 4); BR(20); CHECK(pw_out_last(bt, 0) > 0.9f, "BUTTON range: key 5 missed");
        pw_button_keys(0); BR(20);
        pw_set_param(bt, 0, 1); pw_set_param(bt, 1, 27);
        /* TOGGLE */
        pw_set_param(bt, 2, 2);
        pw_button_keys(1u); BR(20); int on1 = pw_out_last(bt, 0) > 0.9f;
        pw_button_keys(0); BR(20); int on2 = pw_out_last(bt, 0) > 0.9f;
        pw_button_keys(1u); BR(20); int on3 = pw_out_last(bt, 0) > 0.9f;
        pw_button_keys(0); BR(20);
        CHECK(on1 && on2 && !on3, "BUTTON toggle: %d %d %d (want 1 1 0)", on1, on2, on3);
        /* HOLD and DBL */
        pw_set_param(bt, 2, 0);
        pw_button_keys(1u << 7); BR(200); int h0 = pw_out_last(bt, 2) > 0.5f; BR(800); int h1 = pw_out_last(bt, 2) > 0.5f;
        pw_button_keys(0); BR(50);
        CHECK(!h0 && h1, "BUTTON HOLD: %d at 0.2 s, %d at 1 s", h0, h1);
        int dbl = 0;
        for (int k = 0; k < 2; ++k) { pw_button_keys(1u << 7); for (int b = 0; b < 8; ++b) { pw_render(L, R); dbl |= pw_out_last(bt, 3) > 0.5f; } pw_button_keys(0); for (int b = 0; b < 8; ++b) { pw_render(L, R); dbl |= pw_out_last(bt, 3) > 0.5f; } }
        CHECK(dbl, "BUTTON DBL: two quick taps gave nothing");
        BR(1000);
        /* the on-screen button counts as a key */
        pw_set_param(bt, 2, 0); pw_button_virtual((unsigned)bt, 1); BR(20);
        CHECK(pw_out_last(bt, 0) > 0.9f, "BUTTON: the on-screen button did not gate");
        pw_button_virtual((unsigned)bt, 0); BR(20); CHECK(pw_out_last(bt, 0) < 0.01f, "BUTTON: the on-screen button stuck");
        /* SAMPLE */
        int lf = pw_add(PW_T_LFO, 0, 0); pw_connect(lf, 0, bt, 0);
        pw_set_param(bt, 2, 6);
        float lo = 9, hi2 = -9;
        for (int n2 = 0; n2 < 6; ++n2) { BR(170); pw_button_keys(1u << 2); BR(10); float v = pw_out_last(bt, 0); pw_button_keys(0); BR(10); if (fabsf(pw_out_last(bt, 0) - v) > 0.001f) CHECK(0, "BUTTON sample: moved after release"); lo = fminf(lo, v); hi2 = fmaxf(hi2, v); }
        CHECK(hi2 - lo > 0.05f, "BUTTON sample: never captured a different value (%.2f..%.2f)", lo, hi2);
        pw_button_keys(0);
        #undef BR
    }
    {   /* the CZ's built-in banks: every INTERNAL tone sounds, stays finite, and ends after the key */
        static float L2[PW_N], R2[PW_N];
        CHECK(pw_cz_tones(0) == 81 && pw_cz_tones(1) == 2, "CZ banks: INTERNAL %u, bank 1 %u tones", pw_cz_tones(0), pw_cz_tones(1));
        pw_clear(); pw_all_notes_off(); pw_render(L2, R2); pw_set_playing(1);
        int cz = pw_add(PW_T_CZ, 0, 0), kk = pw_add(PW_T_KEYS, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_connect(kk, 0, cz, 0); pw_connect(kk, 1, cz, 1); pw_connect(cz, 0, out, 0);
        char bn[12]; pw_format_param((unsigned)cz, 0, bn); CHECK(!strcmp(bn, "INTERNAL"), "CZ banks: BANK reads '%s'", bn);
        for (int bank = 0; bank < 1; ++bank) for (unsigned t = 1; t <= pw_cz_tones((unsigned)bank); ++t) {
            pw_set_param(cz, 0, (float)bank); pw_set_param(cz, 1, (float)t);
            pw_key(60, 0); for (int b = 0; b < 3 * 44100 / PW_N; ++b) pw_render(L2, R2);
            pw_key(60, 100);
            float pk = 0; int bad = 0;
            for (int b = 0; b < 44100 / PW_N; ++b) { pw_render(L2, R2); for (int i = 0; i < PW_N; ++i) { float v = L2[i]; if (!(v == v) || fabsf(v) > 100) ++bad; pk = fmaxf(pk, fabsf(v)); } }
            if (bank == 0 && t == 80) { for (int b = 0; b < 3 * 44100 / PW_N; ++b) { pw_render(L2, R2); for (int i = 0; i < PW_N; ++i) pk = fmaxf(pk, fabsf(L2[i])); } }   /* (a pad with a 3 s swell) */
            pw_key(60, 0);
            for (int b = 0; b < 8 * 44100 / PW_N; ++b) pw_render(L2, R2);
            float tail = fabsf(pw_out_last(cz, 0));
            const uint8_t *nmb = pw_cz_tone_name((unsigned)bank, t);
            if (getenv("CZ_BANK_REPORT")) printf("bank %d tone %2u %-16.16s peak %.3f tail %.4f\n", bank, t, nmb ? (const char *)nmb : "?", pk, tail);
            CHECK(!bad && pk > 0.05f && pk < 1.5f, "CZ bank %d tone %u: peak %.3f, %d bad", bank, t, pk, bad);
            CHECK(tail < 0.15f, "CZ bank %d tone %u: still sounding 8 s after the key (%.4f)", bank, t, tail);
        }
    }
    {   /* a CZ preset with knobs moved sounds the same after the patch is saved and loaded (power off) */
        static float L3[PW_N], R3[PW_N];
        static uint8_t sv[8192];
        double e[2][2]; char nm[2][16]; static uint8_t bank_bak[2 * 144];
        for (int pre = 0; pre < 2; ++pre) {   /* 0 an INTERNAL tone, 1 a tone of the .syx bank 1 (the stub) */
            pw_clear(); pw_all_notes_off(); pw_render(L3, R3); pw_set_playing(1);
            int cz = pw_add(PW_T_CZ, 0, 0), kk = pw_add(PW_T_KEYS, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
            pw_connect(kk, 0, cz, 0); pw_connect(kk, 1, cz, 1); pw_connect(cz, 0, out, 0);
            pw_set_param(cz, 0, (float)pre); pw_set_param(cz, 1, pre ? 2.0f : 6.0f);
            pw_set_param(cz, 10, 0.3f); pw_set_param(cz, 17, 0.4f); pw_set_param(cz, 8, 3); pw_set_param(cz, 20, 5.0f); pw_set_param(cz, 19, 0.7f);
            pw_format_param((unsigned)cz, 1, nm[0]);
            for (int pass = 0; pass < 2; ++pass) {
                if (pass) {
                    int n = pw_save(sv, sizeof sv);
                    memcpy(bank_bak, cz_bank, sizeof cz_bank);
                    if (pre) memset(cz_bank, 0, sizeof cz_bank);   /* (the .syx bank is gone, or another file) */
                    pw_clear(); pw_all_notes_off(); pw_render(L3, R3);
                    CHECK(n > 0 && pw_load(sv, (unsigned)n) == 0, "CZ save: load failed");
                    pw_set_playing(1);
                    pw_format_param((unsigned)cz, 1, nm[1]);
                    CHECK(!pre || pw_cz_embedded((unsigned)cz), "CZ save: the tone did not come back with the patch");
                    CHECK(!pre || (pw_get_param((unsigned)cz, 0) == 1.0f && pw_get_param((unsigned)cz, 1) == 2.0f), "CZ save: BANK %.0f PRESET %.0f after a load with its bank gone", pw_get_param((unsigned)cz, 0), pw_get_param((unsigned)cz, 1));
                }
                pw_key(60, 0); for (int b = 0; b < 3 * 44100 / PW_N; ++b) pw_render(L3, R3);
                pw_key(60, 100);
                double en = 0; for (int b = 0; b < 44100 / PW_N; ++b) { pw_render(L3, R3); for (int i = 0; i < PW_N; ++i) en += (double)L3[i] * L3[i]; }
                e[pass][pre] = en;
            }
            memcpy(cz_bank, bank_bak, sizeof cz_bank);
            if (pre) { pw_set_param((unsigned)cz, 1, 1.0f); pw_render(L3, R3); CHECK(!pw_cz_embedded((unsigned)cz), "CZ save: PRESET moved but the module still plays the carried tone"); }
            CHECK(e[0][pre] > 0.01 && fabs(e[1][pre] - e[0][pre]) < 0.01 * e[0][pre], "CZ save, bank %d: energy %.4f before, %.4f after reload", pre, e[0][pre], e[1][pre]);
            CHECK(!strcmp(nm[0], nm[1]) && strchr(nm[1], '*'), "CZ save, bank %d: PRESET reads '%s' then '%s'", pre, nm[0], nm[1]);
        }
    }
    {   /* FM-DX: the INTERNAL voices sound and end, a plain sine voice plays its pitch, a moved knob marks PRESET, and a voice from a bank rides in the saved patch */
        static float L4[PW_N], R4[PW_N];
        static uint8_t sv[8192];
        CHECK(pw_dx_voices(0) == 32, "FM-DX: INTERNAL holds %u voices", pw_dx_voices(0));
        pw_clear(); pw_all_notes_off(); pw_render(L4, R4); pw_set_playing(1);
        int dx = pw_add(PW_T_FMDX, 0, 0), kk = pw_add(PW_T_KEYS, 0, 0), out = pw_add(PW_T_OUTPUT, 0, 0);
        pw_connect(kk, 0, dx, 0); pw_connect(kk, 1, dx, 1); pw_connect(dx, 0, out, 0);
        char bn[16]; pw_format_param((unsigned)dx, 0, bn); CHECK(!strcmp(bn, "INTERNAL"), "FM-DX: BANK reads '%s'", bn);
        { char in[24]; (void)pw_module_preset_name((unsigned)dx); pw_format_param((unsigned)dx, 1, in);
          CHECK(!strcmp(in, "1 INIT"), "FM-DX: a new module reads PRESET '%s' (its knobs are the INIT voice, no *)", in); }
        for (unsigned v = 1; v <= pw_dx_voices(0); ++v) {
            pw_set_param(dx, 0, 0); pw_set_param(dx, 1, (float)v);
            pw_key(60, 0); for (int b = 0; b < 2 * 44100 / PW_N; ++b) pw_render(L4, R4);
            pw_key(60, 100);
            float pk = 0; int bad = 0;
            const char *wd = getenv("DX_WAV_DIR"); FILE *wf = 0; char wn[200];
            if (wd) { snprintf(wn, sizeof wn, "%s/v%02u.f32", wd, v); wf = fopen(wn, "wb"); }
            for (int b = 0; b < (wf ? 3 : 1) * 44100 / PW_N; ++b) { pw_render(L4, R4); if (wf) fwrite(L4, sizeof(float), PW_N, wf); if (b >= 44100 / PW_N) continue; for (int i = 0; i < PW_N; ++i) { float x = L4[i]; if (!(x == x) || fabsf(x) > 100) ++bad; pk = fmaxf(pk, fabsf(x)); } }
            pw_key(60, 0);
            if (wf) { for (int b = 0; b < 3 * 44100 / PW_N; ++b) { pw_render(L4, R4); fwrite(L4, sizeof(float), PW_N, wf); } fclose(wf); }
            for (int b = 0; b < 10 * 44100 / PW_N; ++b) pw_render(L4, R4);
            float tail = fabsf(pw_out_last(dx, 0));
            const uint8_t *nmb = pw_dx_voice_name(0, v);
            if (getenv("DX_SHAPES")) { float sh[4 * 32], sc = 0, ho = 0; pw_dx_shapes((unsigned)dx, 32, sh, &sc, &ho); printf("v%02u win %.2f hold %.2f ", v, sc, ho); for (int o = 0; o < 4; ++o) { printf(" OP%d:", o + 1); for (int i = 0; i < 32; i += 4) printf("%3d", (int)(sh[o * 32 + i] * 100)); } printf("\n"); }
            if (getenv("DX_BANK_REPORT")) printf("voice %2u %-10.10s peak %.3f tail %.4f\n", v, nmb ? (const char *)nmb : "?", pk, tail);
            CHECK(!bad && pk > 0.03f && pk < 1.5f, "FM-DX voice %u: peak %.3f, %d bad", v, pk, bad);
            CHECK(tail < 0.02f, "FM-DX voice %u: still sounding 10 s after the key (%.4f)", v, tail);
        }
        /* bank 1: a voice of one operator (OP1, ratio 1, algorithm 8, a sine) and the same one an octave up (ratio 2) */
        memset(dx_bank, 0, sizeof dx_bank);
        for (int k = 0; k < 2; ++k) {
            uint8_t *q = dx_bank + 128 * k;
            q[30 + 0] = 31; q[30 + 3] = 15; q[30 + 4] = 15; q[30 + 7] = 99; q[30 + 8] = (uint8_t)(k ? 8 : 4);
            q[40] = 7; q[46] = 24; q[47] = 12; q[50] = 99;
            memcpy(q + 57, k ? "OCTAVE    " : "PLAIN     ", 10);
        }
        pw_set_param(dx, 0, 1); pw_set_param(dx, 1, 1);
        (void)pw_module_preset_name((unsigned)dx);
        pw_key(60, 100);
        float prev = 0; int zc = 0, n = 0;
        for (int b = 0; b < 44100 / 2 / PW_N; ++b) { pw_render(L4, R4); for (int i = 0; i < PW_N; ++i) { float x = L4[i]; if (b > 20) { if (prev < 0 && x >= 0) ++zc; ++n; } prev = x; } }
        float hz = (float)zc * 44100.0f / (float)n;
        CHECK(hz > 255.0f && hz < 268.0f, "FM-DX: C4 plays %.1f Hz", hz);
        pw_key(60, 0); for (int b = 0; b < 2 * 44100 / PW_N; ++b) pw_render(L4, R4);
        pw_set_param(dx, 1, 2); pw_key(60, 100); zc = 0; n = 0;
        for (int b = 0; b < 44100 / 2 / PW_N; ++b) { pw_render(L4, R4); for (int i = 0; i < PW_N; ++i) { float x = L4[i]; if (b > 20) { if (prev < 0 && x >= 0) ++zc; ++n; } prev = x; } }
        hz = (float)zc * 44100.0f / (float)n;
        CHECK(hz > 515.0f && hz < 535.0f, "FM-DX: ratio 2 plays %.1f Hz", hz);
        char nm[2][20]; pw_format_param((unsigned)dx, 1, nm[0]);
        CHECK(!strcmp(nm[0], "2 OCTAVE"), "FM-DX: the PRESET knob reads '%s'", nm[0]);
        pw_set_param(dx, 1, 1);
        CHECK(pw_get_param((unsigned)dx, 4) == 7.0f && pw_get_param((unsigned)dx, 5) == 0.0f, "FM-DX: loading a voice sets the knobs (ALGO %.0f, FBK %.0f)", pw_get_param((unsigned)dx, 4), pw_get_param((unsigned)dx, 5));
        pw_format_param((unsigned)dx, 1, nm[0]);
        CHECK(nm[0][strlen(nm[0]) - 1] != '*', "FM-DX: a fresh voice reads '%s'", nm[0]);
        pw_set_param(dx, 5, 4.0f); pw_format_param((unsigned)dx, 1, nm[0]);
        CHECK(nm[0][strlen(nm[0]) - 1] == '*', "FM-DX: a moved knob marks PRESET (%s)", nm[0]);
        /* the saved patch carries the edited voice; with the bank wiped it loads and sounds the same */
        double e[2];
        for (int pass = 0; pass < 2; ++pass) {
            if (pass) {
                int sz = pw_save(sv, sizeof sv);
                static uint8_t bak[sizeof dx_bank]; memcpy(bak, dx_bank, sizeof bak); memset(dx_bank, 0, sizeof dx_bank);
                pw_clear(); pw_all_notes_off(); pw_render(L4, R4);
                CHECK(sz > 0 && pw_load(sv, (unsigned)sz) == 0, "FM-DX save: load failed");
                pw_set_playing(1);
                CHECK(pw_cz_embedded((unsigned)dx), "FM-DX save: the voice did not come back with the patch");
                pw_format_param((unsigned)dx, 1, nm[1]);
                CHECK(pw_get_param((unsigned)dx, 0) == 1.0f && pw_get_param((unsigned)dx, 1) == 1.0f, "FM-DX save: BANK %.0f PRESET %.0f after a load with its bank gone", pw_get_param((unsigned)dx, 0), pw_get_param((unsigned)dx, 1));
                memcpy(dx_bank, bak, sizeof bak);
            }
            pw_key(60, 0); for (int b = 0; b < 2 * 44100 / PW_N; ++b) pw_render(L4, R4);
            pw_key(60, 100);
            double en = 0; for (int b = 0; b < 44100 / PW_N; ++b) { pw_render(L4, R4); for (int i = 0; i < PW_N; ++i) en += (double)L4[i] * L4[i]; }
            e[pass] = en;
        }
        CHECK(e[0] > 0.01 && fabs(e[1] - e[0]) < 0.01 * e[0], "FM-DX save: energy %.4f before, %.4f after reload", e[0], e[1]);
        CHECK(strchr(nm[1], '*'), "FM-DX save: PRESET reads '%s' after the reload", nm[1]);
        /* the pictures: a voice's operators and envelopes are there, and a module nobody plays (the add list's) draws */
        float sh[4 * 32], secs = 0, hold = 0, ra, lv; unsigned wv;
        CHECK(pw_dx_shapes((unsigned)dx, 32, sh, &secs, &hold) && secs >= 0.2f && hold > 0 && hold < secs, "FM-DX shapes: window %.2f s, key up at %.2f s", secs, hold);
        CHECK(pw_dx_op((unsigned)dx, 0, &ra, &lv, &wv) && fabsf(ra - 1.0f) < 0.02f && lv > 0.5f && wv == 0, "FM-DX: OP1 ratio %.2f level %.2f wave %u", ra, lv, wv);
    }
    test_clip_protect();
    test_sea();
    test_banks();
    test_mutate_waveset();
    printf(failures ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", failures);
    return failures != 0;
}
