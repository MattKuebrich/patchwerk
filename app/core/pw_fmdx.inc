/* SPDX-License-Identifier: GPL-3.0-only
 * FM-DX: a four-operator FM voice with the TX81Z's chip (the Yamaha YM2414, "OPZ") and its voices. A voice is 128 bytes, the TX81Z's own bulk dump
 * (VMEM) format: kept raw in a bank slot (BANK, B1-B4: the web page puts a .syx file's voices there) or in the built-in INTERNAL bank; PRESET picks one.
 * The knobs show the voice, and a knob moved off its value takes that part over (on a RAM copy; the bank is never touched), as the CZ does.
 * The operators, envelopes, the eight waves and the algorithms are the chip's (pw_opz.inc, a port of ymfm); the chip's registers are set from the voice
 * the way the TX81Z does (the coarse frequency's sorted table, DT1/DT2, the output level curve, rate scaling, EG shift); level scaling and the LFO
 * are close, not checked against a real TX81Z. Velocity, envelope bias and the pitch EG are not played. */
#include "pw_opz.inc"
#define DX_VOICE 128u
enum { DX_BANK, DX_PRESET, DX_LEVEL, DX_TUNE, DX_ALGO, DX_FBK, DX_VIB, DX_TREM, DX_LRATE, DX_LWAVE, DX_OP0, DX_N = DX_OP0 + 32 };
enum { DXS_RATIO, DXS_LEVEL, DXS_AR, DXS_D1R, DXS_D1L, DXS_D2R, DXS_RR, DXS_WAVE };   /* an operator's eight knobs, from DX_OP0 + 8 * (OP number - 1) */
static const uint8_t dx_nth[4] PW_DSP_DATA = {3, 1, 2, 0};     /* OP 1-4 -> its place in a voice (stored OP4, OP2, OP3, OP1) */
static const uint8_t dx_fnth[4] PW_DSP_DATA = {0, 2, 1, 3};    /* a voice's place -> the engine's operator (0 is the first of the chain: OP4) */
static const uint8_t dx_car[8] PW_DSP_DATA = {8, 8, 8, 8, 10, 14, 14, 15};   /* the carriers of each algorithm, by engine operator */
/* F, the coarse frequency 0-63, goes up the sorted list of the chip's multiples and DT2 steps: (multiple << 2 | DT2) */
static const uint8_t dx_fmap[64] PW_DSP_DATA = {0,1,2,3,4,5,6,7,8,9,12,10,11,16,13,14,20,15,17,24,18,19,28,21,22,32,25,23,36,26,29,40,27,30,44,33,48,31,34,37,52,35,56,38,41,60,45,39,42,49,46,43,53,50,47,57,54,51,61,58,55,62,59,63};
static const char *const L_dxratio[64] = {"0.50","0.71","0.79","0.87","1.00","1.41","1.57","1.73","2.00","2.83","3.00","3.14","3.46","4.00","4.24","4.71","5.00","5.20","5.66","6.00","6.28","6.93","7.00","7.07","7.85","8.00","8.49","8.66","9.00","9.42","9.90","10.00","10.39","10.99","11.00","11.31","12.00","12.12","12.57","12.73","13.00","13.86","14.00","14.14","14.14","15.00","15.56","15.59","15.71","16.97","17.32","17.32","18.38","18.85","19.05","19.80","20.43","20.78","21.21","21.99","22.63","23.56","24.25","25.98"};
static const char *const L_dxalg[] = {"4>3>2>1", "4+3>2>1", "4+(3>2)>1", "(4>3)+2>1", "4>3 2>1", "4>3,2,1", "4>3 +2+1", "1+2+3+4"};
static const char *const L_dxwave[] = {"W1", "W2", "W3", "W4", "W5", "W6", "W7", "W8"};
static const char *const L_dxlfw[] = {"SAW", "SQUARE", "TRIANGLE", "S/H"};
static const char *const L_dx_pages[] = {"PRESET", "VOICE", "OP 1", "OP 1 ENV", "OP 2", "OP 2 ENV", "OP 3", "OP 3 ENV", "OP 4", "OP 4 ENV", "LFO"};
#define DXO(op, sub) (DX_OP0 + 8 * (op) + (sub))
/* the knob pages: PRESET (BANK PRESET LEVEL TUNE), VOICE (ALGO FBK VIB TREM), each operator's two (RATIO LEVEL WAVE ATTACK; DECAY 1, SUS LVL, DECAY 2, RELEASE), LFO */
static const uint8_t dx_order[42] = {0, 1, 2, 3, 4, 5, 6, 7,
    DXO(0, 0), DXO(0, 1), DXO(0, 7), DXO(0, 2), DXO(0, 3), DXO(0, 4), DXO(0, 5), DXO(0, 6),
    DXO(1, 0), DXO(1, 1), DXO(1, 7), DXO(1, 2), DXO(1, 3), DXO(1, 4), DXO(1, 5), DXO(1, 6),
    DXO(2, 0), DXO(2, 1), DXO(2, 7), DXO(2, 2), DXO(2, 3), DXO(2, 4), DXO(2, 5), DXO(2, 6),
    DXO(3, 0), DXO(3, 1), DXO(3, 7), DXO(3, 2), DXO(3, 3), DXO(3, 4), DXO(3, 5), DXO(3, 6), 8, 9};
typedef struct {
    dx_core c; uint32_t egc, trev, rng;
    float eacc, lph, ltime;
    uint8_t vc[DX_VOICE];
    uint8_t gate_hi, kh, tk, tb, tv, emb, lidx, lnoise;
} dx_st;
static void dx_init(pw_module *m)
{
    ST(dx_st);
    for (unsigned k = 0; k < 4u; ++k) { s->c.o[k].att = 0x3ff; s->c.o[k].st = DXE_REL; }
    s->rng = 0x2545F491u;
}
/* ---- the voice's bytes: where a knob's value lives (VMEM: four operators of 10 bytes in the order OP4, OP2, OP3, OP1; voice bytes at 40; operator extras at 73) ---- */
static int dx_loc(unsigned p, unsigned *off, unsigned *sh, unsigned *mask)
{
    *sh = 0;
    switch (p) {
    case DX_ALGO: *off = 40; *mask = 7; return 1;
    case DX_FBK: *off = 40; *sh = 3; *mask = 7; return 1;
    case DX_VIB: *off = 43; *mask = 127; return 1;
    case DX_TREM: *off = 44; *mask = 127; return 1;
    case DX_LRATE: *off = 41; *mask = 127; return 1;
    case DX_LWAVE: *off = 45; *mask = 3; return 1;
    default: break;
    }
    if (p < DX_OP0 || p >= DX_N) return 0;
    unsigned n = dx_nth[(p - DX_OP0) >> 3];
    switch ((p - DX_OP0) & 7u) {
    case DXS_RATIO: *off = n * 10u + 8u; *mask = 63; break;
    case DXS_LEVEL: *off = n * 10u + 7u; *mask = 127; break;
    case DXS_AR: *off = n * 10u; *mask = 31; break;
    case DXS_D1R: *off = n * 10u + 1u; *mask = 31; break;
    case DXS_D1L: *off = n * 10u + 4u; *mask = 15; break;
    case DXS_D2R: *off = n * 10u + 2u; *mask = 31; break;
    case DXS_RR: *off = n * 10u + 3u; *mask = 15; break;
    default: *off = 73u + n * 2u + 1u; *sh = 4; *mask = 7; break;
    }
    return 1;
}
/* the knobs' values that say what a voice says (BANK, PRESET, LEVEL and TUNE are not the voice's) */
static void dx_view(const uint8_t *b, float *v)
{
    for (unsigned p = DX_ALGO; p < DX_N; ++p) {
        unsigned off, sh, mask;
        if (!dx_loc(p, &off, &sh, &mask)) continue;
        float x = (float)((b[off] >> sh) & mask);
        unsigned sub = p >= DX_OP0 ? (p - DX_OP0) & 7u : 99u;
        float hi = p == DX_LRATE || p == DX_VIB || p == DX_TREM || sub == DXS_LEVEL ? 99.0f : (float)mask;
        v[p] = pw_clamp(x, sub == DXS_RR ? 1.0f : 0.0f, hi);
    }
}
static uint64_t dx_edits(const pw_module *m, const float *v)   /* the knobs the player has moved off the voice (by more than 1.5 % of their travel) */
{
    unsigned slot = (unsigned)(m - pws.mod);
    uint64_t e = 0;
    for (unsigned p = DX_ALGO; p < DX_N; ++p)
        if (pw_absf(pw_lane_norm(slot, p, m->p[p]) - pw_lane_norm(slot, p, v[p])) > 0.015f) e |= (uint64_t)1 << p;
    return e;
}
static const uint8_t *dx_apply(const uint8_t *b, const pw_module *m, uint8_t *w)   /* the voice as the knobs say it */
{
    float v[DX_N];
    dx_view(b, v);
    uint64_t e = dx_edits(m, v);
    if (!e) return b;
    memcpy(w, b, DX_VOICE);
    for (unsigned p = DX_ALGO; p < DX_N; ++p) {
        unsigned off, sh, mask;
        if (!((e >> p) & 1u) || !dx_loc(p, &off, &sh, &mask)) continue;
        unsigned x = (unsigned)pw_clamp(m->p[p] + 0.5f, 0.0f, (float)mask);
        w[off] = (uint8_t)((w[off] & ~(mask << sh)) | (x << sh));
    }
    if ((e >> DX_VIB) & 1u) { if (((w[45] >> 4) & 7u) < 5u) w[45] = (uint8_t)((w[45] & ~0x70u) | 0x50u); }   /* (a vibrato asked for with none set: some sensitivity) */
    if ((e >> DX_TREM) & 1u) {
        if (((w[45] >> 2) & 3u) < 2u) w[45] = (uint8_t)((w[45] & ~0x0Cu) | 0x08u);
        for (unsigned n = 0; n < 4u; ++n) if ((dx_car[w[40] & 7u] >> dx_fnth[n]) & 1u) w[n * 10u + 6u] |= 0x40u;   /* (the carriers follow the LFO) */
    }
    return w;
}
/* ---- the banks: 0 INTERNAL (built in, pw_dxbank.inc), then the .syx banks the page sent (the bank's slots PW_DX_SLOT up) ---- */
#include "pw_dxbank.inc"
static const uint8_t *dx_bank_of(unsigned bank, unsigned *n)
{
    *n = 0;
    if (bank == 0u) { *n = DX_INTERNAL_N; return dx_internal; }
    if (bank > PW_DX_BANKS) return 0;
    uint32_t frames = 0, rate = 0;
    const int16_t *d = pw_plat_sample(PW_DX_SLOT + bank - 1u, &frames, &rate);
    *n = d ? frames * 2u / DX_VOICE : 0u;
    if (*n > 128u) *n = 128u;
    return *n ? (const uint8_t *)d : 0;
}
static unsigned dx_bank_knob(const pw_module *m) { return (unsigned)pw_clamp(m->p[DX_BANK] + 0.5f, 0.0f, (float)PW_DX_BANKS); }
static int dx_valid(const uint8_t *b)
{
    for (unsigned i = 0; i < DX_VOICE; ++i) if (b[i] & 0x80u) return 0;
    return 1;
}
static int dx_emb_ok(const pw_module *m)   /* the module plays the voice its patch carried (BANK and PRESET are as saved) */
{
    const dx_st *s = (const dx_st *)(const void *)m->stp;
    if (!s) return 0;   /* (the add list's preview module has no state) */
    return s->emb && s->tk == (unsigned)pw_clamp(m->p[DX_PRESET] + 0.5f, 1.0f, 128.0f) && s->tb == dx_bank_knob(m);
}
static const uint8_t *dx_voice(const pw_module *m)   /* the bank's (or the patch's) voice BANK and PRESET name, or 0 */
{
    if (dx_emb_ok(m)) return ((const dx_st *)(const void *)m->stp)->vc;
    unsigned n = 0;
    const uint8_t *d = dx_bank_of(dx_bank_knob(m), &n);
    if (!d) return 0;
    uint32_t k = (uint32_t)pw_clamp(m->p[DX_PRESET] + 0.5f, 1.0f, 128.0f) - 1u;
    const uint8_t *b = d + (k < n ? k : n - 1u) * DX_VOICE;
    return dx_valid(b) ? b : 0;
}
static uint8_t dx_name16[16];
static const uint8_t *dx_name_of(const uint8_t *v)   /* a voice's name (10 bytes at 57) as 16 bytes, padded with spaces */
{
    memset(dx_name16, ' ', 16);
    for (unsigned i = 0; i < 10u; ++i) dx_name16[i] = v[57u + i] >= 32u && v[57u + i] < 127u ? v[57u + i] : (uint8_t)' ';
    return dx_name16;
}
const uint8_t *pw_dx_voice_name(unsigned bank, unsigned voice)   /* the 16 name bytes of a bank's voice (1-128), or 0 */
{
    unsigned n = 0;
    const uint8_t *d = voice ? dx_bank_of(bank, &n) : 0;
    if (!d || voice > n) return 0;
    return dx_name_of(d + (voice - 1u) * DX_VOICE);
}
unsigned pw_dx_voices(unsigned bank) { unsigned n = 0; dx_bank_of(bank, &n); return n; }
static float dx_clamp(unsigned slot, unsigned p, float v)
{
    const pw_module *m = &pws.mod[slot];
    if (p == DX_BANK) {
        int b = (int)(v + 0.5f), cur = (int)dx_bank_knob(m), dir = b >= cur ? 1 : -1;
        if (b < 0) b = 0;
        if (b > (int)PW_DX_BANKS) b = (int)PW_DX_BANKS;
        for (int k = b; k >= 0 && k <= (int)PW_DX_BANKS; k += dir) if (pw_dx_voices((unsigned)k)) return (float)k;
        return (float)cur;
    }
    if (p == DX_PRESET) { unsigned n = pw_dx_voices(dx_bank_knob(m)); return n ? pw_clamp(v, 1.0f, (float)n) : 1.0f; }
    return v;
}
static float dx_norm(unsigned slot, unsigned p)   /* BANK's or PRESET's bar */
{
    const pw_module *m = &pws.mod[slot];
    if (p == DX_BANK) {
        unsigned cur = dx_bank_knob(m), rank = 0, cnt = 0;
        for (unsigned k = 0; k <= PW_DX_BANKS; ++k) if (pw_dx_voices(k)) { if (k < cur) ++rank; ++cnt; }
        return cnt > 1u ? (float)rank / (float)(cnt - 1u) : 1.0f;
    }
    unsigned n = pw_dx_voices(dx_bank_knob(m));
    float k = pw_clamp(m->p[DX_PRESET], 1.0f, 128.0f);
    return n > 1u ? pw_clamp((k - 1.0f) / (float)(n - 1u), 0.0f, 1.0f) : 1.0f;
}
static uint8_t dx_work[DX_VOICE];
static const uint8_t *dx_playing(const pw_module *m)   /* the voice the module plays (the bank's, as the knobs change it), or 0; scratch is dx_work */
{
    const uint8_t *b = dx_voice(m);
    return b ? dx_apply(b, m, dx_work) : 0;
}
static int dx_edited(unsigned slot)
{
    const pw_module *m = &pws.mod[slot];
    const uint8_t *b = dx_voice(m);
    if (!b) return 0;
    float v[DX_N];
    dx_view(b, v);
    return dx_edits(m, v) != 0;
}
static int dx_embed(unsigned slot, uint8_t *out)   /* the voice a saved patch carries (128 bytes, then 16 of padding): when it comes from a .syx bank or has been edited */
{
    const pw_module *m = &pws.mod[slot];
    const uint8_t *b = dx_voice(m);
    if (!b || !(dx_bank_knob(m) >= 1u || dx_edited(slot))) return 0;
    memcpy(out, b, DX_VOICE);
    memset(out + DX_VOICE, 0, 16);
    return 1;
}
static void dx_embed_set(unsigned slot, const uint8_t *voice)
{
    pw_module *m = &pws.mod[slot];
    dx_st *s = (dx_st *)(void *)m->stp;
    if (!dx_valid(voice)) return;
    memcpy(s->vc, voice, DX_VOICE);
    s->tk = (uint8_t)pw_clamp(m->p[DX_PRESET] + 0.5f, 1.0f, 128.0f); s->tb = (uint8_t)dx_bank_knob(m); s->tv = 1; s->emb = 1; s->trev = pws.bank_rev;
}
/* ---- the voice as the chip is set from it ---- */
typedef struct { uint8_t ar, d1r, d2r, rr, d1l, ls, rs, out, f, det, egs, fix, fixrg, osw, fine, ame; } dx_op;
typedef struct { dx_op o[4]; uint8_t alg, fbl, lfs, lfd, pmd, amd, pms, ams, lfw, sync, trps; } dx_vc;   /* o[] by engine operator (0 is OP4) */
static void dx_decode(const uint8_t *b, dx_vc *v)
{
    for (unsigned n = 0; n < 4u; ++n) {
        const uint8_t *q = b + n * 10u, *x = b + 73u + n * 2u;
        dx_op *o = &v->o[dx_fnth[n]];
        o->ar = q[0] & 31u; o->d1r = q[1] & 31u; o->d2r = q[2] & 31u; o->rr = q[3] & 15u; if (o->rr < 1u) o->rr = 1u;
        o->d1l = 15u - (q[4] & 15u); o->ls = q[5] > 99u ? 99u : q[5]; o->ame = (q[6] >> 6) & 1u;
        o->out = q[7] > 99u ? 99u : q[7]; o->f = q[8] & 63u; o->rs = (q[9] >> 3) & 3u; o->det = q[9] & 7u; if (o->det > 6u) o->det = 6u;
        o->egs = (x[0] >> 4) & 3u; o->fix = (x[0] >> 3) & 1u; o->fixrg = x[0] & 7u; o->osw = (x[1] >> 4) & 7u; o->fine = x[1] & 15u;
    }
    v->alg = b[40] & 7u; v->fbl = (b[40] >> 3) & 7u; v->sync = (b[40] >> 6) & 1u;
    v->lfs = b[41] > 99u ? 99u : b[41]; v->lfd = b[42] > 99u ? 99u : b[42]; v->pmd = b[43] > 99u ? 99u : b[43]; v->amd = b[44] > 99u ? 99u : b[44];
    v->pms = (b[45] >> 4) & 7u; v->ams = (b[45] >> 2) & 3u; v->lfw = b[45] & 3u; v->trps = b[46] > 48u ? 48u : b[46];
}
static unsigned dx_tl(unsigned out)   /* the OUT level 0-99 as the chip's total level (0.75 dB steps): the top 80 steps one to one, the bottom ones steeper */
{
    return out >= 20u ? 99u - out : 79u + ((20u - out) * 12u + 2u) / 5u;
}
/* the chip's registers for an operator at note position t (64 per semitone), plus the envelope rates for key code kc */
static void dx_regs(const dx_op *o, dx_rt *r, unsigned kc, unsigned note, int tl_add)
{
    int tl = (int)dx_tl(o->out) + (int)(o->ls * (note > 24u ? note - 24u : 0u) / 200u) + tl_add;
    dx_setup(r, o->ar, o->d1r, o->d2r, o->rr, o->d1l, o->rs, (unsigned)(tl < 0 ? 0 : tl > 127 ? 127 : tl), kc, 0);
    r->osw = o->osw; r->ame = o->ame;
}
static unsigned dx_dt1(unsigned det) { return det >= 3u ? det - 3u : 4u + (3u - det); }   /* the TX81Z's DET 0-6 (3 is none) as the chip's DT1 (0-3 up, 4-7 down) */
static float dx_lfo_hz(unsigned lfs) { return lfs ? 0.06f + 47.9f * pw_exp2(2.2f * pw_log2((float)lfs * (1.0f / 99.0f))) : 0.06f; }
/* the LFO's two outputs at a step 0-255: AM 0-255 and PM -128..127 (the chip's waves; noise is a held random value) */
static void dx_lfo(unsigned w, unsigned i, unsigned noise, unsigned *am, int *pm)
{
    unsigned a, p;
    switch (w & 3u) {
    case 0: a = i ^ 0xffu; p = i; break;
    case 1: a = (i & 0x80u) ? 0u : 0xffu; p = a ^ 0x80u; break;
    case 2: a = ((i & 0x80u) ? i << 1 : (i ^ 0xffu) << 1) & 0xffu; p = (i & 0x40u) ? a : ~a & 0xffu; break;
    default: a = p = noise; break;
    }
    *am = a; *pm = (int)(int8_t)(uint8_t)p;
}
/* the fixed frequency of an operator in Hz: 8 to 255, doubled for each step of the range */
static float dx_fixhz(const dx_op *o) { return (8.0f + (float)o->f * 3.85f + (float)o->fine * 0.25f) * (float)(1u << o->fixrg); }
PROC(dx_proc)
{
    ST(dx_st);
    const uint8_t *b;
    {   /* the bank's voice is read from the flash once and kept in this module's RAM (the audio must not read the flash while it is written); the knobs' changes go on a copy */
        unsigned k0 = (unsigned)pw_clamp(m->p[DX_PRESET] + 0.5f, 1.0f, 128.0f), bk = dx_bank_knob(m);
        if (s->emb && !dx_emb_ok(m)) s->emb = 0;   /* (BANK or PRESET moved: the bank's voice again) */
        if (!s->emb && (!s->tv || s->tk != k0 || s->tb != bk || s->trev != pws.bank_rev)) {
            const uint8_t *f = dx_voice(m);
            s->tv = 0;
            if (f) { memcpy(s->vc, f, DX_VOICE); s->tk = (uint8_t)k0; s->tb = (uint8_t)bk; s->trev = pws.bank_rev; s->tv = 1; }
        }
        b = s->tv ? dx_apply(s->vc, m, dx_work) : 0;
    }
    const int gate_conn = IN_CONNECTED(1);
    if (!b) {
        for (int i = 0; i < PW_N; ++i) out[0][i] = 0;
        if (gate_conn) for (int i = 0; i < PW_N; ++i) pw_rise(&s->gate_hi, in[1][i]);
        return;
    }
    dx_vc v;
    dx_decode(b, &v);
    const unsigned car = dx_car[v.alg];
    const float level = m->p[DX_LEVEL] * (5.0f / 16384.0f), kph = 4096.0f * DX_FS * PW_INV_SR, egk = DX_FS / (3.0f * PW_SRF), lhz = dx_lfo_hz(v.lfs);
    const float ldelay = (float)v.lfd * 0.05f, lramp = 0.5f * ldelay + 0.02f;
    const int pm_on = v.pmd && v.pms, am_on = v.amd && v.ams;
    const uint32_t pdep = (uint32_t)v.pmd * 127u / 99u, adep = (uint32_t)v.amd * 127u / 99u;
    for (int c0 = 0; c0 < PW_N; c0 += 32) {
        const int cn = PW_N - c0 < 32 ? PW_N - c0 : 32;
        const float dt = (float)cn * PW_INV_SR;
        int rose = 0;
        if (gate_conn) { for (int i = 0; i < cn; ++i) if (pw_rise(&s->gate_hi, in[1][c0 + i])) rose = 1; }
        else if (!s->gate_hi) { s->gate_hi = 1; rose = 1; }
        /* the note: 0 V is C4 (MIDI 60); the chip's key position counts 64 to a semitone from its lowest key (MIDI 13) */
        const float volts = IN_CONNECTED(0) ? in[0][c0] : 0.0f;
        float nt = pw_clamp(60.0f + 12.0f * volts + m->p[DX_TUNE] + (float)v.trps - 24.0f, 13.0f, 120.0f);
        const int32_t t = (int32_t)((nt - 13.0f) * 64.0f + 0.5f);
        const unsigned kc = dx_keycode(t), note = (unsigned)(nt + 0.5f);
        const int adj = IN_CONNECTED(2) ? (int)(in[2][c0] * 8.0f) : 0;   /* INDEX: 0-5 V brightens the modulators by up to 40 steps of level */
        dx_rt rt[4];
        for (unsigned k = 0; k < 4u; ++k) {
            dx_regs(&v.o[k], &rt[k], kc, note, (car >> k) & 1u ? 0 : -adj);
            rt[k].shift = k ? v.o[k].egs : 0;   /* (the first of the chain has no EG shift, as on the TX81Z) */
        }
        if (rose) {
            for (unsigned k = 0; k < 4u; ++k) dx_on(&s->c.o[k], &rt[k]);
            s->kh = 1; s->ltime = 0;
            if (v.sync) s->lph = 0;
        }
        if (!s->gate_hi && s->kh) { for (unsigned k = 0; k < 4u; ++k) dx_off(&s->c.o[k]); s->kh = 0; }
        s->ltime += dt;
        const uint32_t fade = s->ltime <= ldelay ? 0u : s->ltime >= ldelay + lramp ? 256u : (uint32_t)((s->ltime - ldelay) / lramp * 256.0f);
        uint32_t step32[4];
        for (unsigned k = 0; k < 4u; ++k) {
            const dx_op *o = &v.o[k];
            const unsigned mulf = dx_fmap[o->f], mul = mulf >> 2, dt2 = mulf & 3u;
            if (o->fix) step32[k] = (uint32_t)(dx_fixhz(o) * 4294967296.0f * PW_INV_SR);
            else step32[k] = (uint32_t)((float)dx_opstep(t, kc, mul, dt2, dx_dt1(o->det), o->fine) * kph);
        }
        for (int i = 0; i < cn; ++i) {
            s->lph += lhz * PW_INV_SR;
            if (s->lph >= 1.0f) s->lph -= 1.0f;
            unsigned li = (unsigned)(s->lph * 256.0f) & 255u, am8;
            int pm8;
            if (li != s->lidx) { s->lidx = (uint8_t)li; s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5; s->lnoise = (uint8_t)(s->rng >> 11); }
            dx_lfo(v.lfw, li, s->lnoise, &am8, &pm8);
            uint32_t aoff = 0;
            if (am_on) aoff = (((am8 * (adep * fade >> 8)) >> 7) << (v.ams - 1u));
            if (pm_on) {
                int32_t pm0 = (pm8 * (int32_t)(pdep * fade >> 8)) >> 7, delta = v.pms < 6u ? (int32_t)(int8_t)pm0 >> (6u - v.pms) : (int32_t)(int8_t)pm0 << (v.pms - 5u);
                for (unsigned k = 0; k < 4u; ++k) {
                    const dx_op *o = &v.o[k];
                    if (o->fix) continue;
                    const unsigned mulf = dx_fmap[o->f];
                    step32[k] = (uint32_t)((float)dx_opstep(t + delta, kc, mulf >> 2, mulf & 3u, dx_dt1(o->det), o->fine) * kph);
                }
            }
            s->eacc += egk;
            if (s->eacc >= 1.0f) { s->eacc -= 1.0f; ++s->egc; for (unsigned k = 0; k < 4u; ++k) dx_eg(&s->c.o[k], &rt[k], s->egc); }
            dx_clock(&s->c, step32);
            int32_t ext = IN_CONNECTED(3) ? (int32_t)(in[3][c0 + i] * 400.0f) : 0;
            out[0][c0 + i] = (float)dx_mix(&s->c, rt, v.alg, v.fbl, aoff, ext) * level;
        }
    }
}
static int dx_embedded(unsigned slot) { return dx_emb_ok(&pws.mod[slot]); }
static const uint8_t *dx_embedded_name(unsigned slot) { return dx_emb_ok(&pws.mod[slot]) ? dx_name_of(((const dx_st *)(const void *)pws.mod[slot].stp)->vc) : 0; }
/* ---- the pictures ---- */
/* an operator's ratio (to the pitch), level (0-1, linear) and wave: op 0-3 is OP 1-4 */
int pw_dx_op(unsigned slot, unsigned op, float *ratio, float *level, unsigned *wave)
{
    const pw_module *m = slot < PW_MAX_MODULES && op < 4u && pws.mod[slot].type == PW_T_FMDX ? &pws.mod[slot] : 0;
    const uint8_t *b = m ? dx_playing(m) : 0;
    if (!b) return 0;
    dx_vc v; dx_decode(b, &v);
    const dx_op *o = &v.o[3u - op];
    const unsigned mulf = dx_fmap[o->f];
    *ratio = o->fix ? dx_fixhz(o) / 261.6f : (float)dx_opstep(3008, 15, mulf >> 2, mulf & 3u, dx_dt1(o->det), o->fine) / (float)dx_pstep(3008);
    *level = (float)dx_a2v((dx_tl(o->out) << 3) << 2) * (1.0f / 8168.0f);
    *wave = o->osw;
    return 1;
}
/* an operator's loudness as the chip's envelope makes it at middle C: the key down for the first `hold` seconds. out gets n points over `secs` (or NULL); *tend is when it was last
 * above 2 % (its release over). The steps with nothing changing (a rate of 0, or silence) are skipped. */
static void dx_sim(const dx_vc *v, unsigned e, float secs, float hold, float *out, unsigned n, float *tend)
{
    dx_rt r;
    dx_regs(&v->o[e], &r, 15, 60, 0);
    r.shift = e ? v->o[e].egs : 0;
    dx_opst o; o.att = 0x3ff; o.st = DXE_REL; o.ph = 0;
    dx_on(&o, &r);
    const float tick = 3.0f / DX_FS, top = (float)dx_a2v(0);
    float t = 0, last = 0, lvl = 1.0f;
    uint32_t ctr = 0;
    uint16_t seen = 0xffffu;
    int off = 0;
    unsigned pts = out ? n : 1u;
    for (unsigned i = 0; i < pts; ++i) {
        float until = out ? (float)(i + 1u) * secs / (float)n : secs;
        for (;;) {
            if (!off && t >= hold) { dx_off(&o); off = 1; }
            if (t >= until) break;
            uint16_t att = o.att; uint8_t st = o.st;
            ++ctr; dx_eg(&o, &r, ctr); t += tick;
            if (o.att == att && o.st == st && (r.rate[o.st] == 0u || (o.att >= 0x3ffu && o.st != DXE_ATK))) {   /* nothing is moving: on to the next event */
                float to = !off && hold < until ? hold : until;
                if (to > t) { ctr += (uint32_t)((to - t) / tick); t = to; }
            }
            if (o.att != seen) { uint32_t a = (uint32_t)(o.att >> r.shift) + r.tl; lvl = (float)dx_a2v((a > 0x3ffu ? 0x3ffu : a) << 2) / top; seen = o.att; }   /* (only when it moved) */
            if (lvl > 0.02f) last = t;
        }
        if (out) out[i] = lvl;
    }
    *tend = last;
}
/* the four operators' envelopes (OP 1-4), n points each in out[4 * n], over a window that fits the voice (*secs; the key goes up at *hold); kept until the voice changes. 0: no voice */
int pw_dx_shapes(unsigned slot, unsigned n, float *out, float *secs, float *hold)
{
    static float cache[4 * 64], csecs, chold;
    static uint32_t csig; static unsigned cslot = 99, cn;
    const pw_module *m = slot < PW_MAX_MODULES && pws.mod[slot].type == PW_T_FMDX ? &pws.mod[slot] : 0;
    const uint8_t *b = m && n <= 64u ? dx_playing(m) : 0;
    if (!b) return 0;
    uint32_t sig = 2166136261u;
    for (unsigned i = 0; i < DX_VOICE; ++i) sig = (sig ^ b[i]) * 16777619u;
    if (cslot != slot || csig != sig || cn != n) {
        dx_vc v; dx_decode(b, &v);
        float tend = 0.2f, te;
        for (unsigned e = 0; e < 4u; ++e) { dx_sim(&v, e, 6.0f, 1.0f, 0, 1, &te); if (te > tend) tend = te; }
        csecs = pw_clamp(tend * 1.1f, 0.2f, 6.0f); chold = pw_minf(1.0f, csecs * 0.55f);
        for (unsigned k = 0; k < 4u; ++k) dx_sim(&v, 3u - k, csecs, chold, cache + k * n, n, &te);
        cslot = slot; csig = sig; cn = n;
    }
    memcpy(out, cache, 4u * n * sizeof(float));
    *secs = csecs; *hold = chold;
    return 1;
}
/* one cycle of the voice (its operators at their ratios, the envelopes at the top) as nw points, -1..1 */
void pw_dx_wave(unsigned slot, float *wave, unsigned nw)
{
    const pw_module *m = slot < PW_MAX_MODULES && pws.mod[slot].type == PW_T_FMDX ? &pws.mod[slot] : 0;
    const uint8_t *b = m ? dx_playing(m) : 0;
    for (unsigned i = 0; i < nw; ++i) wave[i] = 0.0f;
    if (!b) return;
    dx_vc v; dx_decode(b, &v);
    dx_core c; dx_rt rt[4];
    memset(&c, 0, sizeof c);
    uint32_t step[4];
    unsigned car = dx_car[v.alg], ncar = 0;
    for (unsigned k = 0; k < 4u; ++k) {
        dx_regs(&v.o[k], &rt[k], 15, 60, 0);
        rt[k].shift = 0;
        c.o[k].att = 0; c.o[k].st = DXE_SUS;
        const unsigned mulf = dx_fmap[v.o[k].f];
        float ratio = v.o[k].fix ? 1.0f : (float)dx_opstep(3008, 15, mulf >> 2, mulf & 3u, dx_dt1(v.o[k].det), v.o[k].fine) / (float)dx_pstep(3008);
        step[k] = (uint32_t)(ratio * (4294967296.0f / (float)nw));
        ncar += (car >> k) & 1u;
    }
    for (unsigned pass = 0; pass < 2u; ++pass)   /* (the first cycle settles the feedback) */
        for (unsigned i = 0; i < nw; ++i) {
            dx_clock(&c, step);
            float y = (float)dx_mix(&c, rt, v.alg, v.fbl, 0, 0) * (1.0f / 8168.0f) / (float)(ncar ? ncar : 1u);
            if (pass) wave[i] = y;
        }
}
