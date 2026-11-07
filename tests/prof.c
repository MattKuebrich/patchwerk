/* SPDX-License-Identifier: GPL-3.0-only
 * Host profile: which modules of a factory patch cost what (nanoseconds a block, summed over their voices), with the transport playing.
 *   cc ... tests/prof.c ... && tests/out/prof 43     (43: SONG DUB) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pw_engine.h"
#include "pw_presets.h"
#include <time.h>
void pw_plat_lock(void) {}
void pw_plat_unlock(void) {}
uint32_t pw_plat_ticks(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint32_t)((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec); }
uint32_t pw_plat_cpu1_post(pw_cpu1_fn fn, void *m, const float *const *in, float *const *out, unsigned connected) { (void)fn; (void)m; (void)in; (void)out; (void)connected; return 0; }
int pw_plat_cpu1_wait(uint32_t ticket, uint32_t *us) { (void)ticket; (void)us; return 0; }
void pw_plat_cpu1_stop(void) {}
pw_cpu1_fn pw_plat_cpu1_dummy(void) { return 0; }
uint32_t pw_plat_ticks_per_us(void) { return 1000; }
int pw_plat_audio_running(void) { return 0; }
uint32_t pw_plat_millis(void) { return 0; }
void pw_plat_midi_out(uint8_t st, uint8_t d1, uint8_t d2) { (void)st; (void)d1; (void)d2; }
int pw_plat_audio_in(float *l, float *r) { (void)l; (void)r; return 0; }
const int16_t *pw_plat_sample(unsigned slot, uint32_t *frames, uint32_t *rate) { (void)slot; (void)frames; (void)rate; return 0; }
const uint8_t *pw_plat_sample_sea(unsigned slot, uint32_t *frames, uint32_t *rate) { (void)slot; (void)frames; (void)rate; return 0; }
const char *pw_plat_sample_name(unsigned slot) { (void)slot; return 0; }
int main(int argc, char **argv)
{
    unsigned which = argc > 1 ? (unsigned)atoi(argv[1]) : 43, blocks = argc > 2 ? (unsigned)atoi(argv[2]) : 3000;
    pw_init();
    if (pw_factory_build(which)) { printf("no such patch\n"); return 1; }
    pw_set_playing(1);
    static float l[PW_NMAX], r[PW_NMAX];
    for (unsigned i = 0; i < 300; ++i) pw_render(l, r);      /* (settle) */
    pw_prof_on = 1; memset(pw_prof_tk, 0, sizeof pw_prof_tk);
    for (unsigned i = 0; i < blocks; ++i) pw_render(l, r);
    double total = 0;
    for (unsigned k = 0; k < PW_MAX_MODULES; ++k) total += (double)pw_prof_tk[k];
    printf("%s: %u modules, %u voices, modules' procs %.2f us a block\n", pw_factory_name(which), (unsigned)pw_count(), (unsigned)pws.voices, total / blocks / 1000.0);
    for (int pass = 0; pass < 12; ++pass) {   /* the twelve costliest */
        int best = -1; double bv = 0;
        for (unsigned k = 0; k < PW_MAX_MODULES; ++k) if ((double)pw_prof_tk[k] > bv) { bv = (double)pw_prof_tk[k]; best = (int)k; }
        if (best < 0) break;
        printf("  %5.1f%%  %-14s slot %2d  nv %u  %.2f us a block\n", 100.0 * bv / total, pw_types[pws.mod[best].type].name, best, (unsigned)pws.mod[best].nv, bv / blocks / 1000.0);
        pw_prof_tk[best] = 0;
    }
    return 0;
}
