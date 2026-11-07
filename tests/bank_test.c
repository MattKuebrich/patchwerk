/* SPDX-License-Identifier: GPL-3.0-only
 * The sample bank (core/pw_bank.c) on a simulated 1 MiB flash: sounds go to the main window first, then the two extra
 * windows (plat.h PLAT_XDATA1, PLAT_XDATA0) when it is full; a sound that would put "AT" at 0xF06 of a sector in an extra
 * window is refused; the room and what is free add up. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pw_bank.h"
#include "pw_engine.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

static uint8_t nor[0x100000];
static uint8_t host[400000];
static uint32_t host_n;
void pw_plat_lock(void) {}
void pw_plat_unlock(void) {}
uint32_t pw_plat_ticks(void) { return 0; }
uint32_t pw_plat_cpu1_post(pw_cpu1_fn fn, void *m, const float *const *in, float *const *out, unsigned connected) { (void)fn; (void)m; (void)in; (void)out; (void)connected; return 0; }
int pw_plat_cpu1_wait(uint32_t ticket, uint32_t *us) { (void)ticket; (void)us; return 0; }
void pw_plat_cpu1_stop(void) {}
pw_cpu1_fn pw_plat_cpu1_dummy(void) { return 0; }
uint32_t pw_plat_ticks_per_us(void) { return 1000; }
int pw_plat_audio_running(void) { return 0; }
uint32_t pw_plat_millis(void) { return 0; }
void pw_plat_midi_out(uint8_t st, uint8_t d1, uint8_t d2) { (void)st; (void)d1; (void)d2; }
int pw_plat_audio_in(float *l, float *r) { (void)l; (void)r; return 0; }
static int in_data(uint32_t off, uint32_t n)
{
    return (off >= 0xA7000u && off + n <= 0xDC000u) || (off >= 0x93000u && off + n <= 0x97000u) || (off >= 0xE5000u && off + n <= 0xFC000u);
}
const uint8_t *pw_plat_flash(uint32_t off) { return in_data(off, 1) ? nor + off : 0; }
int pw_plat_flash_erase(uint32_t off) { if (!in_data(off, 4096) || (off & 4095u)) return -1; memset(nor + off, 0xFF, 4096); return 0; }
int pw_plat_flash_write(uint32_t off, const void *src, uint32_t n)
{
    if (!in_data(off, n)) return -1;
    for (uint32_t i = 0; i < n; i++) { if (nor[off + i] != 0xFF) return -2; nor[off + i] = ((const uint8_t *)src)[i]; }   /* (into erased flash only) */
    return 0;
}
int pw_plat_host_read(uint32_t addr, uint8_t *dst, uint32_t n) { if (addr + n > host_n) return -1; memcpy(dst, host + addr, n); return 0; }
void pw_plat_progress(const char *what, unsigned percent) { (void)what; (void)percent; }

/* a 16-bit sound of `frames` frames that cannot contain "AT" at the places the loader looks (all bytes below 0x40) */
static int send(unsigned slot, uint32_t frames, int poison)
{
    host_n = frames * 2u;
    for (uint32_t i = 0; i < host_n; i++) host[i] = (uint8_t)(i * 7u % 61u);
    if (poison) for (uint32_t o = 0xF06u; o + 1 < host_n; o += 4096u) { host[o] = 'A'; host[o + 1] = 'T'; }
    uint32_t c = ~pw_crc32(~0u, host, host_n);
    return pw_bank_receive(slot, frames, 22050, "T", c, 0);
}
int main(void)
{
    memset(nor, 0xFF, sizeof nor);
    pw_init();
    pw_bank_init();
    CHECK(PW_BANK_ROOM == 0x4F000u, "the room is 316 KB (%u)", (unsigned)PW_BANK_ROOM);
    CHECK(pw_bank_free_total() == PW_BANK_ROOM && pw_bank_free() == PW_BANK_DATA_HI - PW_BANK_DATA_LO, "an empty bank: all the room free (%u, largest %u)", (unsigned)pw_bank_free_total(), (unsigned)pw_bank_free());
    /* 100,000 frames: 200,000 bytes of the 212,992 in the main window */
    CHECK(send(0, 100000, 0) == PW_BANK_OK, "a big sound goes in");
    const uint8_t *p = pw_plat_flash(PW_BANK_DATA_LO);
    CHECK(p && !memcmp(p, host, 64), "...in the main window");
    /* the next does not fit there: the 92 KB window takes it */
    CHECK(send(1, 40000, 0) == PW_BANK_OK, "a second sound goes to the extra window");
    uint32_t fr = 0, rt = 0; (void)fr; (void)rt;
    const uint8_t *q = pw_plat_flash(PW_BANK_X1_LO);
    CHECK(q && !memcmp(q, host, 64), "...at 0xE5000");
    /* one more of 14,000 frames (28,000 bytes: seven sectors) fits in neither the rest of the main window (12 KB) nor the 16 KB window */
    CHECK(send(2, 14000, 0) == PW_BANK_E_FULL, "28 KB does not fit anywhere (largest gap %u)", (unsigned)pw_bank_free());
    CHECK(send(2, 7000, 0) == PW_BANK_OK, "14 KB fits in the 16 KB window");
    CHECK(pw_plat_flash(PW_BANK_X0_LO) && !memcmp(pw_plat_flash(PW_BANK_X0_LO), host, 64), "...at 0x93000");
    /* a sound listed in the extra windows survives a restart */
    pw_bank_init();
    uint32_t frames = 0; unsigned codec = 9; const uint8_t *at = 0;
    CHECK(pw_bank_stored(1, &frames, &codec, &at) == 80000u && frames == 40000u && at == nor + PW_BANK_X1_LO, "the directory lists it after a restart (%u bytes)", (unsigned)pw_bank_stored(1, &frames, &codec, &at));
    /* the loader's record: "AT" at 0xF06 of a sector in an extra window is refused, but fine in the main window */
    CHECK(pw_bank_delete(2) == PW_BANK_OK && pw_bank_delete(1) == PW_BANK_OK && pw_bank_delete(0) == PW_BANK_OK, "all removed");
    CHECK(send(3, 100000, 1) == PW_BANK_OK, "AT at 0xF06 is fine in the main window");
    CHECK(send(4, 20000, 1) == PW_BANK_E_REC, "AT at 0xF06 is refused in an extra window");
    CHECK(send(4, 20000, 0) == PW_BANK_OK, "...and the same sound without it goes in");
    CHECK(pw_bank_free_total() + 100000u * 2u / 4096u * 4096u + 4096u + 20000u * 2u / 4096u * 4096u + 4096u <= PW_BANK_ROOM + 8192u, "what is free adds up");
    /* a SAMPLER's recorded sound is kept with a user patch: stored as ADPCM in the bank's extra slots, back after a restart */
    {
        pw_clear();
        int sm = pw_add(PW_T_SAMPLER, 0, 0);
        unsigned cap = 0; int16_t *b = pw_sampler_buffer((unsigned)sm, &cap);
        CHECK(b && cap >= 44100u, "a SAMPLER has its second of RAM (%u)", cap);
        CHECK(!pw_sampler_marked((unsigned)sm), "a new SAMPLER's RAM is not marked (it is the starting break)");
        for (unsigned i = 0; i < 44100u; ++i) b[i] = (int16_t)(12000.0 * sin(i * 0.05) + 6000.0 * sin(i * 0.31));
        pw_sampler_commit((unsigned)sm, 44100u);
        CHECK(pw_sampler_marked((unsigned)sm), "a committed sound is marked");
        int16_t ref[44100]; memcpy(ref, b, sizeof ref);
        pw_bank_delete(0);
        pw_ram_save(2);
        CHECK(pw_ram_err == 0, "the sound is stored (error %u)", pw_ram_err);
        uint32_t frames = 0; unsigned codec = 0; const uint8_t *at = 0;
        uint32_t bytes = pw_bank_stored(PW_BANK_RAM0 + 4u, &frames, &codec, &at);
        CHECK(bytes > 20000u && bytes < 24000u, "about 22 KB (%u)", (unsigned)bytes);
        memset(b, 0, 44100u * 2u); pw_sampler_commit((unsigned)sm, 44100u);
        pw_bank_init();                                  /* a restart */
        pw_ram_load(2);
        double err = 0, sig = 0;
        for (unsigned i = 0; i < 44100u; ++i) { double d = (double)b[i] - ref[i]; err += d * d; sig += (double)ref[i] * ref[i]; }
        CHECK(sig > 0 && err / sig < 0.002, "the sound comes back (error %.5f of the signal)", err / sig);
        pw_ram_erase(2);
        CHECK(pw_bank_stored(PW_BANK_RAM0 + 4u, &frames, &codec, &at) == 0, "erasing the patch frees its sound");
        pw_ram_save(2);                                  /* (a sampler that was never recorded keeps nothing) */
    }
    printf(fails ? "\n%d BANK FAILURES\n" : "BANK TESTS PASSED\n", fails);
    return fails != 0;
}
