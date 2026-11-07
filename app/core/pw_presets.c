/* SPDX-License-Identifier: GPL-3.0-only
 * Factory patches, built through the public engine API. */
#include "pw_engine.h"
#include "pw_presets.h"
#include <string.h>

#ifdef PW_FACTORY_GEN
#include "../../tools/old_names.inc"   /* (the factory source still names seven old modules: built here as the ones that replaced them) */
static uint8_t old_of[PW_MAX_MODULES];   /* the type a slot was asked for */
__attribute__((noinline)) static const old_merge_t *old_merge(int slot) { return slot >= 0 && slot < PW_MAX_MODULES && old_of[slot] ? old_merge_of(old_of[slot]) : 0; }
__attribute__((noinline)) static int add(unsigned type, int col, int row)
{
    const old_merge_t *mg = old_merge_of(type);
    int s = pw_add(mg ? mg->to : type, 24 + col * 204, 24 + row * 268);
    if (s >= 0 && type == PW_T_SAMPLER) pw_set_param((unsigned)s, 0, 0);   /* (a SAMPLER a patch builds plays its own loop, as it always did: the default is INTERNAL 1 for one a person adds) */
    if (s >= 0 && s < PW_MAX_MODULES) {
        old_of[s] = (uint8_t)(mg ? type : 0);
        if (mg) {
            for (unsigned k = 0; k < mg->np; ++k) if (mg->pmap[k] >= 0) pw_set_param((unsigned)s, (unsigned)mg->pmap[k], mg->def[k]);
            if (mg->mode_param != 255) pw_set_param((unsigned)s, mg->mode_param, (float)mg->mode);
        }
    }
    return s;
}
__attribute__((noinline)) static void cable(int sm, unsigned so, int dm, unsigned di)
{
    const old_merge_t *a = old_merge(sm), *b = old_merge(dm);
    if (a) { if (so >= a->no || a->omap[so] < 0) return; so = (unsigned)a->omap[so]; }
    if (b) { if (di >= b->ni || b->imap[di] < 0) return; di = (unsigned)b->imap[di]; }
    if (sm >= 0 && dm >= 0) pw_connect((unsigned)sm, so, (unsigned)dm, di);
}
__attribute__((noinline)) static void set(int slot, unsigned p, float v)
{
    const old_merge_t *mg = old_merge(slot);
    if (mg) { if (p >= mg->np || mg->pmap[p] < 0) return; p = (unsigned)mg->pmap[p]; }
    if (slot >= 0) pw_set_param((unsigned)slot, p, v);
}
#else
__attribute__((noinline)) static int add(unsigned type, int col, int row)
{
    int s = pw_add(type, 24 + col * 204, 24 + row * 268);
    if (s >= 0 && type == PW_T_SAMPLER) pw_set_param((unsigned)s, 0, 0);   /* (a SAMPLER a patch builds plays its own loop, as it always did) */
    return s;
}
__attribute__((noinline)) static void cable(int sm, unsigned so, int dm, unsigned di) { if (sm >= 0 && dm >= 0) pw_connect((unsigned)sm, so, (unsigned)dm, di); }
__attribute__((noinline)) static void set(int slot, unsigned p, float v) { if (slot >= 0) pw_set_param((unsigned)slot, p, v); }
#endif
static void steps(int slot, const int8_t notes[16], unsigned gates)
{
    if (slot < 0) return;
    for (int i = 0; i < 16; ++i) pws.mod[slot].data[i] = (uint8_t)notes[i];
    pws.mod[slot].data[16] = (uint8_t)gates;
    pws.mod[slot].data[17] = (uint8_t)(gates >> 8);
}
/* Port names, for readability. */
enum { VCO_VOCT = 0, VCO_FM, VCO_PWM, VCO_SYNC, VCO_SIN = 0, VCO_TRI, VCO_SAW, VCO_SQR };
enum { K_PITCH = 0, K_GATE, K_VEL, K_TRIG, K_MOD };

static const char *const names[PW_FACTORY_COUNT] = {
    "BASIC VOICE", "BERLIN SEQ", "WEST COAST", "DRONE", "RANDOM TUNES", "FM BELLS", "SHIFTER",
    "FM E.PIANO", "GLITCH LOOP", "TURING PLUCK", "ACID 303", "VOX CHOIR", "POLY STRINGS", "RACK BEAT", "WARP BREAK", "SLICE JAM", "MIDI DUO",
    "FM BASS", "DUB CHORDS", "NOISE SURF", "ARP DREAMS", "FOLD BASS", "TALK BOX", "COIN DRUMS", "BACKWARDS", "CHAOS PAD",
    "SWITCH SEQ", "INPUT FX", "GRAIN CLOUD", "TAPE DUB", "BELL GARDEN", "MOTION BASS",
    "DRUM RACK", "GLASS PAD", "RAIN MACHINE", "IDM SHUFFLE", "GATE ECHOES", "MICRO GLITCH",
    "SONG BERLIN", "SONG ACID", "SONG BREAK",
    "SONG DUB", "SPRING BASS", "SNAP DICE", "CZ BASS", "DRIFT TUNE", "RISING RUNS", "CZ ENSEMBLE", "DX ENSEMBLE", "ROLL CHORDS"};
const char *pw_factory_name(unsigned i) { return i < PW_FACTORY_COUNT ? names[i] : ""; }

/* A new patch's name: two words that go together (an adjective and a noun, at most 12 letters with the space) */
static const char *const adj[] = {
    "AMBER", "BRASS", "CALM", "COSMIC", "DUSTY", "ELECTRIC", "FADED", "FERAL", "FROZEN", "GENTLE", "GHOST", "GOLDEN",
    "HAZY", "HOLLOW", "HUMBLE", "IRON", "JADE", "LUNAR", "MAGIC", "MELLOW", "MIDNIGHT", "MISTY", "NEON", "NIGHT",
    "OCEAN", "PALE", "PAPER", "PLASTIC", "PURPLE", "QUIET", "RADIO", "RAPID", "RUSTY", "SILVER", "SLOW", "SOLAR",
    "SONIC", "SPARE", "STATIC", "STORMY", "SUNNY", "SWIFT", "TINY", "VELVET", "WARM", "WILD", "WOODEN", "ZEN"};
static const char *const noun[] = {
    "ANT", "ATLAS", "BEACON", "BIRD", "BLOOM", "CANYON", "CASTLE", "CLOUD", "COMET", "CORAL", "DELTA", "DREAM",
    "DRIFT", "DUNE", "ECHO", "EMBER", "FERN", "FIELD", "GARDEN", "GLOW", "HARBOR", "HILL", "ISLAND", "LAKE",
    "LANTERN", "MACHINE", "MEADOW", "MIRROR", "MOTH", "ORBIT", "PEBBLE", "PILOT", "PULSE", "RIVER", "ROBOT", "SIGNAL",
    "SPARK", "STAR", "STONE", "TIDE", "TOWER", "TRAIN", "VALLEY", "WAVE", "WHALE", "WIND", "WIRE", "WOLF"};
void pw_random_name(char out[13])
{
    static uint32_t r;
    r = r * 1664525u + 1013904223u + pw_plat_millis() * 2654435761u + pws.blocks;
    for (int tries = 0; tries < 16; ++tries) {
        r = r * 1664525u + 1013904223u;
        const char *a = adj[(r >> 8) % (sizeof adj / sizeof adj[0])];
        r = r * 1664525u + 1013904223u;
        const char *b = noun[(r >> 8) % (sizeof noun / sizeof noun[0])];
        unsigned la = (unsigned)strlen(a), lb = (unsigned)strlen(b);
        if (la + 1 + lb > 12) continue;
        memset(out, 0, 13);
        memcpy(out, a, la); out[la] = ' '; memcpy(out + la + 1, b, lb);
        return;
    }
    memset(out, 0, 13); memcpy(out, "NEW PATCH", 9);
}

__attribute__((noinline)) static void perf(int slot, unsigned p)
{
#ifdef PW_FACTORY_GEN
    const old_merge_t *mg = old_merge(slot);
    if (mg) { if (p >= mg->np || mg->pmap[p] < 0) return; p = (unsigned)mg->pmap[p]; }
#endif
    if (slot >= 0) pw_perf_assign((unsigned)slot, p);
}

/* the CV RECORDER's demo motion (also MOTION BASS's): one bar, a long rise from shut to wide open, a drop, then a quick flutter */
static void cvr_shape(uint8_t *d)
{
    for (int k = 0; k < 48; ++k) {
        float x = (float)k / 48.0f, v = x < 0.72f ? x / 0.72f : (k & 1) ? 0.55f : 0.05f;
        d[k] = (uint8_t)(30 + (int)(v * 220.0f));
    }
}

/* ---- the factory patches: each is saved as the engine saves any patch (pw_save), packed with a small LZ (tools/lz.py) and
 * kept in pw_factory_data.inc. Patch 0 is stored as it is and is also the dictionary the others are packed against.
 * tools/factory_src.inc builds them in code, tools/gen_factory.sh turns that into the data. */
#ifdef PW_FACTORY_GEN
#include "../../tools/factory_src.inc"
#else
#include "pw_factory_data.inc"
/* the LZ block s (n bytes) into o (cap bytes): a token with literal and match counts, the literals, a 2-byte offset back into the
 * dictionary + output so far; returns the length, 0 on a mistake */
static unsigned unlz(const uint8_t *s, unsigned n, uint8_t *o, unsigned cap)
{
    const uint8_t *e = s + n;
    unsigned w = 0;
    while (s < e) {
        unsigned t = *s++, ll = t >> 4, ml = (t & 15u) + 4u, b;
        if (ll == 15u) do { b = *s++; ll += b; } while (b == 255u);
        if (w + ll > cap) return 0;
        while (ll--) o[w++] = *s++;
        if (s >= e) break;
        unsigned off = s[0] | (unsigned)s[1] << 8; s += 2;
        if ((t & 15u) == 15u) do { b = *s++; ml += b; } while (b == 255u);
        if (w + ml > cap || off > w + FAC_DICT || !off) return 0;
        while (ml--) { unsigned v = FAC_DICT + w - off; o[w] = v < FAC_DICT ? fac_data[v] : o[w - off]; ++w; }
    }
    return w;
}
int pw_factory_build(unsigned index)
{
    static uint8_t buf[FAC_MAX] PW_POOL;   /* (the pool: it is only a scratch for unpacking a factory patch) */
    if (index >= PW_FACTORY_COUNT) return -1;
    const uint8_t *p = fac_data + fac_off[index];
    unsigned n = fac_off[index + 1] - fac_off[index];
    if (index == 0) { memcpy(buf, p, n); }
    else if (!(n = unlz(p, n, buf, sizeof buf))) return -1;
    pw_clear();
    pw_set_playing(0);
    if (pw_load(buf, n) != 0) return -1;
    if (FAC_PLAYS >> index & 1u) pw_set_playing(1);   /* (the SONG patches play at once) */
    pw_undo_reset();
    pws.revision++;
    return 0;
}
#endif

/* ---- ROLL: a random patch that always plays. A driver (the keys, a sequencer, or a TURING through a quantizer),
 * a sound source, something to shape it, sometimes something to move it, one or two effects, the output. The
 * performance page gets the sound's knobs; variations 2-4 are the same patch, nudged. */
static uint32_t rr;
static unsigned rnd(unsigned n) { rr = rr * 1664525u + 1013904223u; return n ? (rr >> 8) % n : 0; }
static float rndf(float lo, float hi) { rr = rr * 1664525u + 1013904223u; return lo + (hi - lo) * (float)(rr >> 8) * (1.0f / 16777216.0f); }
static int rcol;
static int radd(unsigned type, int row)
{
    int s = add(type, rcol, row);
    if (s >= 0 && pws.mod[s].status != PW_STATUS_OK) { pw_remove((unsigned)s); s = -1; }
    if (s >= 0 && row == 0) ++rcol;
    return s;
}
int pw_random_patch(void)
{
    rr ^= pw_plat_millis() * 2654435761u + pws.blocks * 40503u + 0x9E3779B9u;
    pw_clear();
    pw_set_playing(0);
    pw_set_bpm((float)(84 + rnd(14) * 4));
    rcol = 0;
    const int adv = rnd(3) == 0;   /* now and then an adventurous one: stranger sources, a second kind of motion, odder effects */
    /* the driver */
    int drv = (int)rnd(3), pit = -1, gat = -1; unsigned pp = 0, gp = 0, tp = 0;
    if (drv == 0) { pit = gat = radd(PW_T_KEYS, 0); pp = K_PITCH; gp = K_GATE; tp = K_TRIG; }
    else if (drv == 1) {
        static const int8_t pent[] = {-12, -9, -7, -5, -2, 0, 3, 5, 7, 10, 12};
        int seq = radd(PW_T_SEQ, 0);
        if (seq >= 0) {
            int8_t n[16]; unsigned g = 0, dens = 5 + rnd(5);
            for (int i = 0; i < 16; ++i) { n[i] = pent[rnd(sizeof pent)]; if (rnd(10) < dens || !(i & 3)) g |= 1u << i; }
            steps(seq, n, g);
            set(seq, 0, rnd(2) ? 16 : 8); set(seq, 1, rndf(0.2f, 0.7f)); if (!rnd(4)) set(seq, 3, 0.05f);
        }
        pit = gat = seq; pp = 0; gp = 1; tp = 2;
    } else {
        int tu = add(PW_T_TURING, rcol, 1), q = radd(PW_T_QUANT, 0);
        static const uint8_t sc[] = {2, 3, 7, 8, 9, 5};
        set(tu, 1, rndf(0.03f, 0.25f)); set(tu, 2, rndf(1.0f, 2.5f)); set(tu, 0, (float)(rnd(2) ? 8 : 16));
        set(q, 0, sc[rnd(sizeof sc)]); set(q, 1, (float)rnd(12)); set(q, 2, -1);
        cable(tu, 0, q, 0); cable(tu, 2, q, 1);
        pit = q; pp = 0; gat = tu; gp = 1; tp = 2;
    }
    /* the source: some shape their own loudness */
    static const uint8_t srcs[] = {PW_T_VCO, PW_T_VCO, PW_T_WAVETABLE, PW_T_COMPLEX, PW_T_FM4, PW_T_STRING, PW_T_ADDITIVE, PW_T_MACRO};
    unsigned st = srcs[rnd(sizeof srcs)];
    if (adv && rnd(2)) { static const uint8_t odd[] = {PW_T_FORMANT, PW_T_GENDY, PW_T_CZ}; st = odd[rnd(sizeof odd)]; }
    int src = radd(st, 0), self_amp = st == PW_T_FM4 || st == PW_T_STRING || st == PW_T_CZ, mod_dst = -1; unsigned mod_port = 0, so = 0;
    switch (st) {
    case PW_T_VCO: so = (unsigned)(1 + rnd(3)); set(src, 0, (float)(rnd(3) ? 0 : -12)); set(src, 2, rndf(0.2f, 0.6f)); break;
    case PW_T_WAVETABLE: set(src, 2, (float)rnd(4)); set(src, 3, rndf(0, 1)); mod_dst = src; mod_port = 1; break;
    case PW_T_COMPLEX: so = 1; set(src, 1, rnd(2) ? (float)(1 + rnd(4)) : rndf(0.5f, 4.0f)); set(src, 2, rndf(0.5f, 4.0f)); set(src, 3, rndf(0.05f, 0.5f)); mod_dst = src; mod_port = 1; break;
    case PW_T_FM4: set(src, 0, (float)(1 + rnd(12))); set(src, 1, rndf(0.2f, 0.8f)); set(src, 2, rndf(0, 0.4f)); set(src, 4, (float)rnd(PW_FM_SHAPES)); set(src, 5, (float)rnd(PW_FM_SHAPES));
                   cable(gat, gp, src, 1); break;
    case PW_T_ADDITIVE: set(src, 1, rndf(0.2f, 0.9f)); set(src, 2, rndf(0.3f, 0.8f)); set(src, 4, rndf(0.2f, 0.7f)); set(src, 5, rndf(0, 0.6f));
                        set(src, 6, rnd(2) ? rndf(0, 0.5f) : 0); set(src, 7, rnd(3) ? 0 : rndf(-0.3f, 0.5f)); mod_dst = src; mod_port = 4; break;
    case PW_T_MACRO: set(src, 0, (float)rnd(8)); set(src, 2, rndf(0.1f, 0.8f)); set(src, 3, rndf(0.1f, 0.8f)); set(src, 4, rndf(0.1f, 0.7f));
                     mod_dst = src; mod_port = 3; break;
    case PW_T_STRING: set(src, 4, (float)rnd(6)); set(src, 1, rndf(0.5f, 0.9f)); set(src, 2, rndf(0.3f, 0.8f)); cable(gat, tp, src, 0); break;
    case PW_T_CZ: set(src, 8, (float)rnd(8)); set(src, 9, rnd(3) ? 0.0f : (float)(1 + rnd(8))); set(src, 10, rndf(0.2f, 0.9f)); set(src, 13, rndf(0.1f, 1.0f)); set(src, 17, rndf(0.2f, 2.0f));
                  set(src, 20, rnd(4) ? 0.0f : rndf(2, 12)); cable(gat, gp, src, 1); mod_dst = src; mod_port = 2; break;
    case PW_T_FORMANT: set(src, 1, rndf(0, 4)); set(src, 2, rndf(0.2f, 0.6f)); mod_dst = src; mod_port = 1; break;
    case PW_T_GENDY: set(src, 2, rndf(0.1f, 0.6f)); set(src, 3, rndf(0.1f, 0.6f)); set(src, 4, rndf(0.2f, 0.7f)); set(src, 1, (float)(4 + rnd(10))); break;
    default: break;
    }
    cable(pit, pp, src, st == PW_T_STRING ? 1 : 0);
    int cur = src; unsigned cp = so;
    /* shaping it */
    float gain = st == PW_T_STRING ? 1.0f : st == PW_T_FM4 ? 0.4f : st == PW_T_CZ ? 0.5f : st == PW_T_GENDY ? 0.3f : 0.42f;
    unsigned shape = self_amp ? (rnd(3) ? 9 : (unsigned)rnd(2)) : (unsigned)rnd(4);
    if (shape == 0 || shape == 2) {                          /* a filter (2: a wavefolder first) */
        if (shape == 2) { int fo = radd(PW_T_FOLDER, 0); set(fo, 0, rndf(1.5f, 4.0f)); set(fo, 2, (float)rnd(2)); cable(cur, cp, fo, 0); if (fo >= 0) { cur = fo; cp = 0; gain *= 1.3f; } }
        int f = radd(PW_T_VCF, 0);
        { static const uint8_t ft[] = {0, 1, 3, 4, 0, 1}; set(f, 4, (float)ft[rnd(sizeof ft)]); } set(f, 0, rndf(400, 2000)); set(f, 1, rndf(0.1f, 0.55f)); set(f, 2, rndf(0.3f, 0.8f));
        cable(cur, cp, f, 0); if (f >= 0) { cur = f; cp = 0; if (mod_dst < 0) { mod_dst = f; mod_port = 1; } }
        if (!self_amp) {
            int e = add(PW_T_ENV, rcol - 1, 1), a = radd(PW_T_VCA, 0);
            set(e, 0, rnd(4) || drv ? rndf(0.001f, 0.02f) : rndf(0.1f, 0.6f)); set(e, 1, rndf(0.1f, 0.6f)); set(e, 2, rndf(0, 0.7f)); set(e, 3, rndf(0.1f, 0.8f));
            cable(gat, gp, e, 0); cable(e, 0, f, 1); cable(e, 0, a, 1); cable(cur, cp, a, 0); if (a >= 0) { cur = a; cp = 0; }
        }
    } else if (shape == 1 || shape == 3) {                   /* a low pass gate the steps strike (3: a vowel filter first) */
        if (shape == 3) { int vw = radd(PW_T_VOWEL, 0); set(vw, 0, rndf(0, 4)); cable(cur, cp, vw, 0); if (vw >= 0) { cur = vw; cp = 0; mod_dst = vw; mod_port = 1; gain *= 0.6f; } }
        int g = radd(PW_T_LPG, 0);
        set(g, 2, 2); set(g, 1, rndf(0.15f, 0.8f)); set(g, 3, rndf(0.1f, 0.5f));
        if (self_amp) set(g, 0, 1);
        else if (drv == 0) { int e = add(PW_T_ENV, rcol - 1, 1); set(e, 0, 0.003f); set(e, 1, 0.3f); set(e, 2, 0.6f); set(e, 3, 0.4f); cable(gat, gp, e, 0); cable(e, 0, g, 1); }
        else cable(gat, tp, g, 2);
        cable(cur, cp, g, 0); if (g >= 0) { cur = g; cp = 0; }
    }
    /* something to move it (an adventurous one: sometimes random steps or chaos instead of an LFO, and an odd extra stage) */
    if (mod_dst >= 0 && rnd(3)) {
        unsigned mk = adv ? rnd(3) : 0;
        if (mk == 1) { int h = add(PW_T_SH, rcol > 0 ? rcol - 1 : 0, 2); set(h, 0, rndf(0.05f, 0.5f)); set(h, 1, rndf(1.0f, 3.0f)); set(h, 3, rndf(0.5f, 6.0f)); cable(h, 1, mod_dst, mod_port); }
        else if (mk == 2) { int ch = add(PW_T_CHAOS, rcol > 0 ? rcol - 1 : 0, 2); set(ch, 0, rndf(0.2f, 3.0f)); set(ch, 2, rndf(0.2f, 0.5f)); cable(ch, 0, mod_dst, mod_port); }
        else { int l = add(PW_T_LFO, rcol > 0 ? rcol - 1 : 0, 2); set(l, 0, rndf(0.05f, adv ? 8.0f : 1.5f)); set(l, 1, rndf(0.2f, 0.6f)); cable(l, 1, mod_dst, mod_port); }
    }
    if (adv && rnd(2) && cur >= 0) {   /* one odd stage in the voice: a fold or a comb tuned to the notes */
        if (rnd(2)) { int fo = radd(PW_T_FOLDER, 0); set(fo, 0, rndf(1.5f, 6.0f)); set(fo, 1, rndf(-0.3f, 0.3f)); set(fo, 2, (float)rnd(4)); set(fo, 3, rndf(0.4f, 1.0f)); cable(cur, cp, fo, 0); if (fo >= 0) { cur = fo; cp = 0; gain *= 0.8f; } }
        else { int cb = radd(PW_T_COMB, 0); set(cb, 1, rndf(0.5f, 0.92f)); set(cb, 2, rndf(0.1f, 0.6f)); set(cb, 3, rndf(0.4f, 0.8f)); cable(cur, cp, cb, 0); cable(pit, pp, cb, 1); if (cb >= 0) { cur = cb; cp = 0; } }
    }
    /* effects: one or two */
    static const uint8_t fxs[] = {PW_T_DELAY, PW_T_REVERB, PW_T_SWIRL, PW_T_SWIRL, PW_T_TAPE, PW_T_DIST, PW_T_DELAY, PW_T_REVERB};
    static const uint8_t fxa[] = {PW_T_DELAY, PW_T_REVERB, PW_T_SWIRL, PW_T_TAPE, PW_T_PSHIFT, PW_T_PSHIFT, PW_T_DIST, PW_T_FOLDER, PW_T_DELAY, PW_T_REVERB};
    unsigned nfx = 1 + rnd(adv ? 3 : 2); uint64_t used = 0; int curR = -1; unsigned cpR = 0;
    for (unsigned k = 0; k < nfx; ++k) {
        unsigned ft = adv ? fxa[rnd(sizeof fxa)] : fxs[rnd(sizeof fxs)];
        if (used & (1ull << ft)) continue;
        if (curR >= 0 && ft != PW_T_REVERB) continue;           /* once stereo, only a reverb follows */
        int fx = radd(ft, 0); if (fx < 0) continue;
        used |= 1ull << ft;
        switch (ft) {
        case PW_T_DELAY: set(fx, 0, rnd(2) ? 0.375f : 0.25f); set(fx, 1, rndf(0.3f, 0.6f)); set(fx, 3, rndf(0.2f, 0.4f)); break;
        case PW_T_REVERB: set(fx, 1, rndf(1.5f, 6.0f)); set(fx, 3, rndf(0.2f, 0.4f)); break;
        case PW_T_SWIRL: set(fx, 0, (float)(1 + rnd(2))); set(fx, 1, rndf(0.1f, 0.8f)); set(fx, 3, rndf(0.3f, 0.6f)); break;   /* a phaser or a chorus */
        case PW_T_TAPE: set(fx, 1, rndf(0.1f, 0.5f)); set(fx, 3, rndf(0.2f, 0.6f)); break;
        case PW_T_PSHIFT: { static const int8_t sh[] = {-12, -7, 5, 7, 12, -5, 3}; set(fx, 0, (float)sh[rnd(sizeof sh)]); set(fx, 1, rndf(-20, 20)); set(fx, 3, rndf(0.3f, 0.6f)); break; }
        case PW_T_FOLDER: set(fx, 0, rndf(1.5f, 4.0f)); set(fx, 3, rndf(0.3f, 0.7f)); gain *= 0.8f; break;
        case PW_T_DIST: gain *= 0.6f; set(fx, 0, rndf(0.2f, 0.5f)); set(fx, 1, (float)rnd(4)); set(fx, 3, 0.7f); break;
        default: break;
        }
        cable(cur, cp, fx, 0);
        if (ft == PW_T_REVERB) cable(curR >= 0 ? curR : cur, curR >= 0 ? cpR : cp, fx, 1);
        cur = fx; cp = 0;
        if (ft == PW_T_REVERB || ft == PW_T_SWIRL) { curR = fx; cpR = 1; } else curR = -1;
    }
    int out = radd(PW_T_OUTPUT, 0);
    cable(cur, cp, out, 0); cable(curR >= 0 ? curR : cur, curR >= 0 ? cpR : cp, out, 1);
    set(out, 0, gain > 1.0f ? 1.0f : gain);
    if (drv == 0 && !rnd(2)) pw_set_voices(st == PW_T_FM4 || st == PW_T_STRING || adv ? 3 : 2);
    /* the performance page: the first two knobs of each sound module, in patch order */
    unsigned np = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES && np < PW_PERF; ++i) {
        if (pws.mod[i].used != 1) continue;
        unsigned t = pws.mod[i].type;
        if (t == PW_T_KEYS || t == PW_T_OUTPUT || t == PW_T_QUANT || t == PW_T_SEQ || t == PW_T_TURING || t == PW_T_ENV) continue;
        for (unsigned k = 0; k < 2 && np < PW_PERF; ++k, ++np) perf((int)i, k);
    }
    /* variations 2-4: the performance knobs nudged */
    for (unsigned v = 1; v < PW_VARS; ++v) {
        pw_var_copy(0, v);
        if (v > 3) continue;
        pw_var_select(v);
        for (unsigned k = 0; k < PW_PERF; ++k) {
            unsigned sl = pws.perf[k][0], pi = pws.perf[k][1];
            if (sl >= PW_MAX_MODULES || !pws.mod[sl].used) continue;
            const pw_param_desc *d = &pw_types[pws.mod[sl].type].param[pi];
            if (d->kind == PW_ENUM || d->kind == PW_INT) continue;
            float span = d->max - d->min, x = pws.mod[sl].p[pi] + rndf(-0.25f, 0.25f) * span;
            pw_set_param(sl, pi, x < d->min ? d->min : x > d->max ? d->max : x);
        }
    }
    pw_var_select(0);
    pw_random_name(pws.name);
    pw_undo_reset();
    pws.revision++;
    return 0;
}

/* ------------------------------------------------------------------------------------------ examples
 * One small patch per module (pw_examples.inc), written as a recipe and built through the public API. */
#include "pw_examples_z.inc"   /* (packed from pw_examples.inc by tools/gen_examples.py) */
#define PW_EXAMPLE_COUNT ((unsigned)(sizeof pw_examples / sizeof pw_examples[0]))

/* a packed string, expanded into d (cap bytes): a byte of 0x80 and up names a dictionary entry, which may use others */
static unsigned ex_unz(const char *s, char *d, unsigned n, unsigned cap)
{
    for (; *s && n + 1 < cap; ++s) {
        if ((uint8_t)*s >= 128) n = ex_unz(ex_dict[(uint8_t)*s - 128], d, n, cap);
        else d[n++] = *s;
    }
    d[n] = 0;
    return n;
}
static char ex_tbuf[420], ex_rbuf[420];   /* the text and the recipe being used */
static const char *ex_find(unsigned type, int recipe)
{
    for (unsigned i = 0; i < PW_EXAMPLE_COUNT; ++i) {
        if (pw_examples[i].type != type) continue;
        if (recipe) { ex_unz(pw_examples[i].recipe, ex_rbuf, 0, sizeof ex_rbuf); return ex_rbuf; }
        ex_unz(pw_examples[i].text, ex_tbuf, 0, sizeof ex_tbuf); return ex_tbuf;
    }
    return 0;
}
const char *pw_example_text(unsigned type) { return ex_find(type, 0); }
static char ex_hbuf[200];
const char *pw_type_help(unsigned type)
{
    for (unsigned i = 0; i < sizeof pw_helps / sizeof pw_helps[0]; ++i)
        if (pw_helps[i].type == type) { ex_unz(pw_helps[i].text, ex_hbuf, 0, sizeof ex_hbuf); return ex_hbuf; }
    return "";
}
/* the modules it uses, as "KEYS VCO ENV VCA OUT" (a second of one kind without its #2) */
void pw_example_uses(unsigned type, char *out, unsigned cap)
{
    unsigned n = 0;
    const char *rc = ex_find(type, 1);
    out[0] = 0;
    for (const char *r = rc; r && *r && *r != '|' && n + 1 < cap; ++r) {
        if (*r == '#') { ++r; continue; }
        out[n++] = *r;
    }
    out[n] = 0;
}

typedef struct { uint8_t type, inst; int slot; } ex_mod;
static int ex_nmod;
static ex_mod ex_mods[24];

static int ex_tok_end(char c, char stop) { return !c || c == ' ' || c == stop; }
/* a module reference at r: "TAG" or "TAG#2", up to `stop` (or a space/end); the slot, or -1 */
static int ex_module(const char **rp, char stop)
{
    const char *r = *rp, *b = r;
    while (!ex_tok_end(*r, stop) && *r != '#') ++r;
    unsigned len = (unsigned)(r - b), inst = 1;
    if (*r == '#') { ++r; inst = 0; while (*r >= '0' && *r <= '9') inst = inst * 10 + (unsigned)(*r++ - '0'); }
    *rp = r;
    for (int i = 0; i < ex_nmod; ++i) {
        const char *tag = pw_types[ex_mods[i].type].tag;
        unsigned k = 0;
        while (k < len && tag[k] == b[k]) ++k;
        if (k == len && !tag[k] && ex_mods[i].inst == inst) return ex_mods[i].slot;
    }
    return -1;
}
/* a name at r up to `end`, its '_' read as a space, against `name`: equal? */
static int ex_name_is(const char *r, const char *end, const char *name)
{
    while (r < end && *name) { char c = *r == '_' ? ' ' : *r; if (c != *name) return 0; ++r; ++name; }
    return r == end && !*name;
}
static const char *ex_scan(const char *r, const char *stops)
{
    while (*r && *r != ' ') { for (const char *q = stops; *q; ++q) if (*r == *q) return r; ++r; }
    return r;
}
static float ex_number(const char *r)
{
    int neg = 0; float v = 0, f = 0.1f;
    if (*r == '-') { neg = 1; ++r; }
    while (*r >= '0' && *r <= '9') v = v * 10.0f + (float)(*r++ - '0');
    if (*r == '.') { ++r; while (*r >= '0' && *r <= '9') { v += (float)(*r++ - '0') * f; f *= 0.1f; } }
    return neg ? -v : v;
}
/* Builds the example for a module type as the patch (replacing it). 0 done; -1 none for it; -2 something in its
 * recipe did not resolve (a mistake in the table: the rest is built anyway). */
static int build_from_recipe(const char *recipe)   /* 0 done, -2 a mistake in the recipe (the rest is built anyway) */
{
    int bad = 0;
    pw_clear();
    pw_set_playing(0);
    pw_set_bpm(120);
    ex_nmod = 0;
    const char *r = recipe;
    /* 1: the modules */
    for (int col = 0; *r && *r != '|'; ) {
        if (*r == ' ') { ++r; continue; }
        const char *b = r;
        while (*r && *r != ' ' && *r != '|' && *r != '#') ++r;
        unsigned inst = 1, t = PW_T_COUNT;
        for (unsigned k = 0; k < PW_T_COUNT; ++k) {
            const char *tag = pw_types[k].tag; unsigned n = 0;
            while (tag[n] && b + n < r && tag[n] == b[n]) ++n;
            if (!tag[n] && b + n == r) t = k;
        }
        if (*r == '#') { ++r; inst = 0; while (*r >= '0' && *r <= '9') inst = inst * 10 + (unsigned)(*r++ - '0'); }
        if (t == PW_T_COUNT || ex_nmod >= 24) { bad = 1; continue; }
        int slot = add(t, col % 5, col / 5); ++col;
        if (slot < 0) { bad = 1; continue; }
        ex_mods[ex_nmod].type = (uint8_t)t; ex_mods[ex_nmod].inst = (uint8_t)inst; ex_mods[ex_nmod].slot = slot; ++ex_nmod;
    }
    if (*r == '|') ++r;
    /* 2: the cables */
    for (; *r && *r != '|'; ) {
        if (*r == ' ') { ++r; continue; }
        const char *a = r;
        int sm = ex_module(&a, '.');
        if (*a != '.') { bad = 1; while (*r && *r != ' ' && *r != '|') ++r; continue; }
        const char *pb = ++a, *pe = ex_scan(pb, ">");
        int so = -1, di = -1, dm = -1;
        if (sm >= 0) { const pw_type *t = &pw_types[pws.mod[sm].type]; for (unsigned k = 0; k < t->n_out; ++k) if (ex_name_is(pb, pe, t->out[k])) so = (int)k; }
        const char *d = pe;
        if (*d == '>') { ++d; dm = ex_module(&d, '.'); }
        if (*d == '.') {
            const char *qb = ++d, *qe = ex_scan(qb, "|");
            if (dm >= 0) { const pw_type *t = &pw_types[pws.mod[dm].type]; for (unsigned k = 0; k < t->n_in; ++k) if (ex_name_is(qb, qe, t->in[k])) di = (int)k; }
            d = qe;
        }
        if (sm < 0 || so < 0 || dm < 0 || di < 0 || pw_connect((unsigned)sm, (unsigned)so, (unsigned)dm, (unsigned)di) < 0) bad = 1;
        r = d > r ? d : r + 1;
    }
    if (*r == '|') ++r;
    /* 3: settings and options */
    while (*r) {
        if (*r == ' ') { ++r; continue; }
        const char *a = r;
        if (*a == '@') {
            ++a;
            if (a[0] == 'P' && a[1] == 'L') pw_set_playing(1);
            else if (a[0] == 'S' && a[1] == 'E' && a[2] == 'Q' && a[3] == '=') {   /* the first SEQUENCER: notes 0,3,7... all gates on */
                int8_t notes[16] = {0}; unsigned n = 0, sq = 0;
                for (int i = 0; i < ex_nmod; ++i) if (ex_mods[i].type == PW_T_SEQ) { sq = (unsigned)ex_mods[i].slot; break; }
                const char *q = a + 4;
                while (*q && *q != ' ' && n < 16) { notes[n++] = (int8_t)ex_number(q); while (*q && *q != ',' && *q != ' ') ++q; if (*q == ',') ++q; }
                steps((int)sq, notes, n >= 16 ? 0xffffu : (1u << n) - 1u);
                set((int)sq, 0, (float)n);
            } else if (a[0] == 'T' && a[1] == 'R' && a[2] == 'I' && a[3] == 'G' && a[4] == '=') {   /* the TRIG SEQ: A:1000100010001000,B:... */
                int tg = -1;
                for (int i = 0; i < ex_nmod; ++i) if (ex_mods[i].type == PW_T_TRIGSEQ) { tg = ex_mods[i].slot; break; }
                const char *q = a + 5;
                while (*q && *q != ' ') {
                    unsigned track = (unsigned)(*q - 'A');
                    q += 2;
                    for (unsigned st = 0; *q == '0' || *q == '1' || *q == '2' || *q == '3'; ++q, ++st) if (tg >= 0 && *q != '0') pw_trig_set((unsigned)tg, track, st, *q - '0');
                    if (*q == ',') ++q;
                }
            } else bad = 1;
        } else {
            int m = ex_module(&a, '.'), pi = -1;
            if (*a == '.') {
                const char *pb = ++a, *pe = ex_scan(pb, "=");
                if (m >= 0) { const pw_type *t = &pw_types[pws.mod[m].type]; for (unsigned k = 0; k < t->n_param; ++k) if (ex_name_is(pb, pe, t->param[k].name) && (pi < 0 || t != &pw_types[PW_T_PARAMMOD])) pi = (int)k;   /* (PARAM MOD's four channels share their names: the first) */   /* (the first of a name: PARAM MOD's channels share theirs) */ }
                if (m >= 0 && pi >= 0 && *pe == '=') set(m, (unsigned)pi, ex_number(pe + 1));
                else bad = 1;
            } else bad = 1;
        }
        while (*r && *r != ' ') ++r;
    }
    pw_undo_reset();
    return bad ? -2 : 0;
}
static void roll_demo(void)   /* PIANO ROLL's example: a chord, a second one, and a short tune over them (notes are data, not part of a recipe) */
{
    static const uint8_t n[][3] = {{0, 8, 57}, {0, 8, 60}, {0, 8, 64}, {8, 8, 53}, {8, 8, 57}, {8, 8, 60}, {0, 2, 72}, {2, 2, 76}, {4, 4, 74}, {8, 2, 72}, {10, 2, 69}, {12, 4, 65}};
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i)
        if (pws.mod[i].used == 1 && pws.mod[i].type == PW_T_PROLL) { for (unsigned k = 0; k < sizeof n / sizeof n[0]; ++k) pw_roll_add(&pws.mod[i], n[k][0], n[k][1], n[k][2], 24); break; }
    pw_set_voices(4);
}
int pw_example_build(unsigned type)
{
    const char *recipe = ex_find(type, 1);
    if (!recipe) return -1;
    int r = build_from_recipe(recipe);
    if (type == PW_T_PROLL) roll_demo();
    if (type == PW_T_CVREC) for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1 && pws.mod[i].type == PW_T_CVREC) { cvr_shape(pws.mod[i].data); break; }
    memset(pws.name, 0, sizeof pws.name);
    for (const char *t = pw_types[type].tag; *t && t - pw_types[type].tag < 6; ++t) pws.name[t - pw_types[type].tag] = *t;
    { unsigned n = (unsigned)strlen(pws.name); memcpy(pws.name + n, " DEMO", 6); }
    return r;
}
