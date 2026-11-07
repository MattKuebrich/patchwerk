/* SPDX-License-Identifier: GPL-3.0-only
 * The OPZ (Yamaha YM2414, the TX81Z's chip) operator, envelope and algorithm core for FM-DX. Plain integer arithmetic on the chip's own tables.
 * It is a port of ymfm (https://github.com/aaronsgiles/ymfm, BSD-3-Clause, Copyright (c) 2021 Aaron Giles): the waveforms, the envelope
 * generator (rates, increments, sustain level, reverb stage, shift), the phase steps with detune and multiple, and the eight algorithms with
 * feedback. See LICENSING.md. Needs <stdint.h>; no other part of the firmware. */
#include "pw_opz_tab.inc"

#define DX_FS 55930.4f   /* the chip's sample rate on the TX81Z: its 3.579545 MHz clock / 64 */
enum { DXE_ATK, DXE_DEC, DXE_SUS, DXE_REL, DXE_REV };
typedef struct { uint32_t ph; uint16_t att; uint8_t st; } dx_opst;                      /* an operator: phase (the top 10 bits are the chip's), attenuation, envelope stage */
typedef struct { uint16_t tl, sus; uint8_t rate[5], shift, ame, osw; } dx_rt;           /* an operator's settings: total level (x8), sustain attenuation, rates by stage, EG shift, AM on, wave */
typedef struct { dx_opst o[4]; int16_t fb[2], fbin; } dx_core;                          /* the four operators of a voice (o[0] is the first of the chain: the TX81Z's OP4) */

static uint32_t dx_w0(uint32_t i) { return (uint32_t)dx_sin[(i & 0x100u) ? (~i & 0xffu) : (i & 0xffu)] | (((i >> 9) & 1u) << 15); }
static uint32_t dx_w1(uint32_t i)
{
    uint32_t a = dx_w0(i), m = (a & 0x7fffu) * 2u;
    return (m > dx_sin[0] ? (uint32_t)dx_sin[0] : m) | (a & 0x8000u);
}
/* the eight operator waves of the OPZ as attenuation (4.8 log) with a sign in bit 15; i is the 10-bit phase */
static uint32_t dx_wave(unsigned osw, uint32_t i)
{
    uint32_t zero = dx_sin[0];
    switch (osw & 7u) {
    case 0: return dx_w0(i);
    case 1: return dx_w1(i);
    case 2: return (i & 0x200u) ? zero : dx_w0(i);
    case 3: return (i & 0x200u) ? zero : dx_w1(i);
    case 4: return (i & 0x200u) ? zero : dx_w0(i * 2u);
    case 5: return (i & 0x200u) ? zero : dx_w1(i * 2u);
    case 6: return (i & 0x200u) ? zero : dx_w0((i * 2u) & 0x1ffu);
    default: return (i & 0x200u) ? zero : dx_w1((i * 2u) & 0x1ffu);
    }
}
static int32_t dx_a2v(uint32_t x)   /* a 5.8 log attenuation -> a 13-bit linear volume */
{
    uint32_t sh = x >> 8;
    return sh > 31u ? 0 : (int32_t)((((uint32_t)dx_pow[x & 0xffu] | 0x400u) << 2) >> sh);
}
static int32_t dx_vol(const dx_opst *o, const dx_rt *r, int32_t phase, uint32_t am)
{
    if (o->att > 0x380u) return 0;
    uint32_t sa = dx_wave(r->osw, (uint32_t)phase & 0x3ffu);
    uint32_t e = (uint32_t)(o->att >> r->shift);
    if (r->ame) e += am;
    e += r->tl;
    if (e > 0x3ffu) e = 0x3ffu;
    int32_t v = dx_a2v((sa & 0x7fffu) + (e << 2));
    return (sa & 0x8000u) ? -v : v;
}
/* the envelope, one chip envelope tick (every third sample of the chip's clock); ctr counts the ticks from 1 */
static void dx_eg(dx_opst *o, const dx_rt *r, uint32_t ctr)
{
    if (o->st == DXE_ATK && o->att == 0) o->st = DXE_DEC;
    if (o->st == DXE_DEC && o->att >= r->sus) o->st = DXE_SUS;
    uint32_t rate = r->rate[o->st], rs = rate >> 2, c = ctr << rs;
    if (c & 0x7ffu) return;
    uint32_t bits = (c >> (rs <= 11u ? 11u : rs)) & 7u, inc = (dx_inc[rate] >> (4u * bits)) & 15u;
    if (o->st == DXE_ATK) {
        if (rate < 62u) o->att = (uint16_t)(o->att + (((uint32_t)~(uint32_t)o->att * inc) >> 4));
    } else {
        uint32_t a = (uint32_t)o->att + inc;
        o->att = (uint16_t)(a >= 0x400u ? 0x3ffu : a);
        if (o->st == DXE_REL && o->att >= 0xc0u) o->st = DXE_REV;
    }
}
static void dx_off(dx_opst *o) { if (o->st < DXE_REL) o->st = DXE_REL; }
static void dx_on(dx_opst *o, const dx_rt *r)   /* a key down (after a key up, if it was held) */
{
    dx_off(o);
    o->st = DXE_ATK; o->ph = 0;
    if (r->rate[DXE_ATK] >= 62u) o->att = 0;
}
/* ALGORITHM(op2 in, op3 in, op4 in, op1 out, op2 out, op3 out): the operator outputs 0 none, 1 O1, 2 O2, 3 O3, 5 O1+O2, 6 O1+O3, 7 O2+O3 */
#define DX_ALG(a, b, c, d, e, f) ((a) | ((b) << 1) | ((c) << 4) | ((d) << 7) | ((e) << 8) | ((f) << 9))
static const uint16_t dx_algo[8] PW_DSP_DATA = {DX_ALG(1, 2, 3, 0, 0, 0), DX_ALG(0, 5, 3, 0, 0, 0), DX_ALG(0, 2, 6, 0, 0, 0), DX_ALG(1, 0, 7, 0, 0, 0),
                                                DX_ALG(1, 0, 3, 0, 1, 0), DX_ALG(1, 1, 1, 0, 1, 1), DX_ALG(1, 0, 0, 0, 1, 1), DX_ALG(0, 0, 0, 1, 1, 1)};
/* One output sample of the four operators (after dx_clock): fbl feedback 0-7, am the AM offset, ext an extra phase modulation of the first operator. */
static int32_t dx_mix(dx_core *c, const dx_rt *r, unsigned alg, unsigned fbl, uint32_t am, int32_t ext)
{
    int32_t fbm = fbl ? (c->fb[0] + c->fb[1]) >> (10u - fbl) : 0, op[8];
    uint32_t a = dx_algo[alg & 7u];
    op[0] = 0;
    op[1] = dx_vol(&c->o[0], &r[0], (int32_t)(c->o[0].ph >> 22) + fbm + ext, am);
    c->fbin = (int16_t)op[1];
    op[2] = dx_vol(&c->o[1], &r[1], (int32_t)(c->o[1].ph >> 22) + (op[a & 1u] >> 1), am);
    op[5] = op[1] + op[2];
    op[3] = dx_vol(&c->o[2], &r[2], (int32_t)(c->o[2].ph >> 22) + (op[(a >> 1) & 7u] >> 1), am);
    op[6] = op[1] + op[3]; op[7] = op[2] + op[3];
    int32_t y = dx_vol(&c->o[3], &r[3], (int32_t)(c->o[3].ph >> 22) + (op[(a >> 4) & 7u] >> 1), am);
    if (a & 0x80u) { y += op[1]; y = y > 32767 ? 32767 : y < -32768 ? -32768 : y; }
    if (a & 0x100u) { y += op[2]; y = y > 32767 ? 32767 : y < -32768 ? -32768 : y; }
    if (a & 0x200u) { y += op[3]; y = y > 32767 ? 32767 : y < -32768 ? -32768 : y; }
    return y;
}
/* the per-sample bookkeeping that comes before the output: the feedback history, then (steps given) the phases */
static void dx_clock(dx_core *c, const uint32_t *step32)
{
    c->fb[0] = c->fb[1]; c->fb[1] = c->fbin;
    for (unsigned k = 0; k < 4u; ++k) c->o[k].ph += step32[k];
}
/* ---- pitch ---- */
/* A note position: 64 per semitone, 768 to an octave, 0 the chip's lowest key (block 0, code 0: a C sharp); A 440 is block 4 note 8: 56 * 64 + 512 */
static uint32_t dx_pstep(int32_t t)
{
    if (t < 0) return dx_step[0] >> 7;
    int32_t blk = t / 768, eff = t % 768;
    return blk > 7 ? dx_step[767] : dx_step[eff] >> (blk ^ 7);
}
static unsigned dx_keycode(int32_t t)   /* the 5-bit key code the rate scaling and detune use: octave * 4 + note / 3 */
{
    if (t < 0) return 0;
    int32_t blk = t / 768, eff = t % 768;
    return blk > 7 ? 31u : (unsigned)(blk * 4 + (eff >> 6) / 3);
}
static int32_t dx_detune(unsigned det3, unsigned kc)   /* DT1 (0-7: 0-3 up, 4-7 down) at a key code */
{
    int32_t v = dx_det[kc * 4u + (det3 & 3u)];
    return (det3 & 4u) ? -v : v;
}
/* an operator's phase step (the chip's 10.10 units per sample): t the note position with the vibrato in it, kc its key code, mul 0-15 (0 is a half), dt2 0-3
 * (0, 600, 781, 950 cents), det 0-7, fine 0-15 */
static uint32_t dx_opstep(int32_t t, unsigned kc, unsigned mul, unsigned dt2, unsigned det, unsigned fine)
{
    static const int16_t d2[4] = {0, (600 * 64 + 50) / 100, (781 * 64 + 50) / 100, (950 * 64 + 50) / 100};
    uint32_t s = dx_pstep(t + d2[dt2 & 3u]) + (uint32_t)dx_detune(det, kc);
    uint32_t m = mul ? (mul << 4) : 0x08u;
    return (s * (m | fine)) >> 4;
}
static unsigned dx_er(unsigned raw, unsigned k) { return raw ? (raw + k > 63u ? 63u : raw + k) : 0u; }   /* the effective rate: raw plus the rate scaling, at most 63 */
/* an operator's envelope settings from the voice's: ar 0-31, d1r, d2r 0-31, rr 0-15, d1l 0-15, ksr 0-3 (rate scaling), tl 0-127, rev 0-7 */
static void dx_setup(dx_rt *r, unsigned ar, unsigned d1r, unsigned d2r, unsigned rr, unsigned d1l, unsigned ksr, unsigned tl, unsigned kc, unsigned rev)
{
    unsigned k = kc >> (ksr ^ 3u);
    r->rate[DXE_ATK] = (uint8_t)dx_er(ar * 2u, k);
    r->rate[DXE_DEC] = (uint8_t)dx_er(d1r * 2u, k);
    r->rate[DXE_SUS] = (uint8_t)dx_er(d2r * 2u, k);
    r->rate[DXE_REL] = (uint8_t)dx_er(rr * 4u + 2u, k);
    r->rate[DXE_REV] = r->rate[DXE_REL];
    if (rev) { unsigned v = dx_er(rev * 4u + 2u, k); if (v < r->rate[DXE_REV]) r->rate[DXE_REV] = (uint8_t)v; }
    unsigned sl = d1l & 15u;
    sl |= (sl + 1u) & 0x10u;
    r->sus = (uint16_t)(sl << 5);
    r->tl = (uint16_t)(tl << 3);
}
