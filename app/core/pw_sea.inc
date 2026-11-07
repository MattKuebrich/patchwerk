/* SPDX-License-Identifier: GPL-3.0-only
 * Compressed sounds in the sample bank: SEA (Simple Embedded Audio codec, Dani Biro, MIT; QOA-derived), mono, 3-bit
 * residuals, 4-bit scale factors, a scale factor every 20 frames, chunks of 500 frames. About 3.4 bits a sample.
 * A stored sound is the chunk stream alone (no file header): each chunk carries the predictor's state at its start
 * (4 history words, 4 weight words, 16-bit little-endian), so any chunk can be decoded without the ones before it.
 *   chunk: type 1, (4 << 4 | 3), 20, 0x5A | LMS 16 bytes | scale factors n/20 x 4 bits | residuals n x 3 bits (MSB first)
 * The decoder is integer only; the dequantiser table is the codec's for 3-bit residuals and 4-bit scale factors,
 * worked out once with the reference code (sea-codec 0.8.0 dqt.rs) and checked against its decoder in the tests.
 * Included by pw_engine.c. */
#define SEA_FPC 500u                                  /* frames in a chunk (the last one holds what is left) */
#define SEA_SLICE 20u
#define SEA_FULL_BYTES (20u + (SEA_FPC / SEA_SLICE * 4u + 7u) / 8u + (SEA_FPC * 3u + 7u) / 8u)   /* 221 */
static const int16_t sea_dqt[16][8] = {
    {1,-1,3,-3,5,-5,7,-7}, {5,-5,15,-15,27,-27,42,-42}, {16,-16,53,-53,95,-95,147,-147}, {36,-36,120,-120,216,-216,336,-336},
    {68,-68,225,-225,405,-405,630,-630}, {113,-113,375,-375,675,-675,1050,-1050}, {174,-174,580,-580,1044,-1044,1624,-1624},
    {253,-253,843,-843,1517,-1517,2359,-2359}, {352,-352,1173,-1173,2111,-2111,3283,-3283}, {473,-473,1575,-1575,2835,-2835,4410,-4410},
    {617,-617,2058,-2058,3704,-3704,5761,-5761}, {788,-788,2628,-2628,4730,-4730,7357,-7357}, {986,-986,3288,-3288,5918,-5918,9205,-9205},
    {1214,-1214,4045,-4045,7281,-7281,11326,-11326}, {1472,-1472,4908,-4908,8834,-8834,13741,-13741}, {1764,-1764,5880,-5880,10584,-10584,16464,-16464}};

/* the bytes of a stored sound of `frames` frames (a multiple of 20), the stream and its padding */
uint32_t pw_sea_bytes(uint32_t frames)
{
    uint32_t full = frames / SEA_FPC, rest = frames % SEA_FPC;
    uint32_t b = full * SEA_FULL_BYTES + (rest ? 20u + (rest / SEA_SLICE * 4u + 7u) / 8u + (rest * 3u + 7u) / 8u : 0u);
    return (b + 3u) & ~3u;   /* (stored padded to whole words: the flash is written and pulled in them) */
}
/* chunk c of a stream of `frames` frames into out[] (SEA_FPC frames at most); returns the frames in it */
static uint32_t sea_chunk(const uint8_t *stream, uint32_t frames, uint32_t c, int16_t *out)
{
    uint32_t n = frames - c * SEA_FPC;
    if (c * SEA_FPC >= frames) return 0;
    if (n > SEA_FPC) n = SEA_FPC;
    const uint8_t *p = stream + c * SEA_FULL_BYTES;
    int32_t h[4], w[4];
    for (int i = 0; i < 4; ++i) {
        h[i] = (int16_t)(uint16_t)(p[4 + 2 * i] | (p[5 + 2 * i] << 8));
        w[i] = (int16_t)(uint16_t)(p[12 + 2 * i] | (p[13 + 2 * i] << 8));
    }
    const uint8_t *sf = p + 20, *rs = sf + (n / SEA_SLICE * 4u + 7u) / 8u;
    uint32_t bit = 0;
    for (uint32_t k = 0; k < n / SEA_SLICE; ++k) {
        const int16_t *dq = sea_dqt[(sf[k >> 1] >> ((k & 1u) ? 0 : 4)) & 15u];
        for (uint32_t j = 0; j < SEA_SLICE; ++j, bit += 3u) {
            uint32_t by = bit >> 3, sh = bit & 7u;
            uint32_t q = (((uint32_t)rs[by] << 8 | rs[by + 1u]) >> (13u - sh)) & 7u;
            int32_t pr = (int32_t)((uint32_t)w[0] * (uint32_t)h[0] + (uint32_t)w[1] * (uint32_t)h[1] + (uint32_t)w[2] * (uint32_t)h[2] + (uint32_t)w[3] * (uint32_t)h[3]) >> 13;
            int32_t d = dq[q], v = pr + d;
            v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
            int32_t dl = d >> 4;
            for (int i = 0; i < 4; ++i) w[i] += h[i] < 0 ? -dl : dl;
            h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = v;
            out[k * SEA_SLICE + j] = (int16_t)v;
        }
    }
    return n;
}
/* Reading a stored sound. The audio side reads through a reader (pw_rdr) a voice holds: it decodes forward a slice of
 * 20 frames at a time into a ring of 64 (the last 44 stay readable: a cubic interpolation looks 1 back, 2 ahead),
 * and a jump (a new note, a reverse run, a grain) restarts at the chunk's start, which carries the predictor's state.
 * Readers are few (PW_RDR_N, about 170 bytes each) and a voice finds its own by a key; one not asked for in the last
 * two blocks is free for another voice, and a voice that finds none gets silence (never a decode for each sample).
 * Anything else (the display, the slice finder) reads a run of frames into its own buffer (pw_smp_read). */
#ifndef PW_RDR_N
#define PW_RDR_N 16
#endif
struct pw_rdr { const uint8_t *src; uint32_t frames, dec, base, blk, rev;   /* (base: where the ring's run of decoded frames starts) */ int32_t h[4], w[4]; uint16_t key; int16_t ring[64]; };
static struct pw_rdr rdrs[PW_RDR_N];
static uint32_t sea_blk;
void pw_sea_block(void) { ++sea_blk; }
static void rdr_slice(struct pw_rdr *r)
{
    uint32_t c = r->dec / SEA_FPC, in = r->dec - c * SEA_FPC, n = r->frames - c * SEA_FPC;
    if (n > SEA_FPC) n = SEA_FPC;
    const uint8_t *p = r->src + c * SEA_FULL_BYTES;
    if (!in)
        for (int i = 0; i < 4; ++i) {
            r->h[i] = (int16_t)(uint16_t)(p[4 + 2 * i] | (p[5 + 2 * i] << 8));
            r->w[i] = (int16_t)(uint16_t)(p[12 + 2 * i] | (p[13 + 2 * i] << 8));
        }
    uint32_t k = in / SEA_SLICE;
    const uint8_t *rs = p + 20 + (n / SEA_SLICE * 4u + 7u) / 8u;
    const int16_t *dq = sea_dqt[(p[20 + (k >> 1)] >> ((k & 1u) ? 0 : 4)) & 15u];
    int32_t *h = r->h, *w = r->w;
    uint32_t bit = k * (SEA_SLICE * 3u);
    for (uint32_t j = 0; j < SEA_SLICE; ++j, bit += 3u) {
        uint32_t by = bit >> 3, sh = bit & 7u;
        uint32_t q = (((uint32_t)rs[by] << 8 | rs[by + 1u]) >> (13u - sh)) & 7u;
        int32_t pr = (int32_t)((uint32_t)w[0] * (uint32_t)h[0] + (uint32_t)w[1] * (uint32_t)h[1] + (uint32_t)w[2] * (uint32_t)h[2] + (uint32_t)w[3] * (uint32_t)h[3]) >> 13;
        int32_t d = dq[q], v = pr + d;
        v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        int32_t dl = d >> 4;
        for (int i = 0; i < 4; ++i) w[i] += h[i] < 0 ? -dl : dl;
        h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = v;
        r->ring[(r->dec + j) & 63u] = (int16_t)v;
    }
    r->dec += SEA_SLICE;
}
#include "pw_internal.inc"
/* A sound id: 0-15 INTERNAL (built in), 16-31 A (the bank's slots 0-15), 32-47 B (slots PW_A2_SLOT..) */
static int snd_slot(unsigned id) { return id >= 16u && id < 32u ? (int)(id - 16u) : id >= 32u && id < PW_SOUNDS ? (int)(PW_A2_SLOT + id - 32u) : -1; }
int pw_smp_open(unsigned id, pw_smp *s)
{
    uint32_t f = 0, r = 0;
    s->raw = 0; s->sea = 0; s->frames = 0; s->rate = 0;
#if PW_INTERNAL_N > 0
    if (id < PW_INTERNAL_N) { s->sea = pw_internal_sea + pw_internal[id].off; s->frames = pw_internal[id].frames; s->rate = pw_internal[id].rate; return 1; }
#endif
    int slot = snd_slot(id);
    if (slot < 0) return 0;
    const int16_t *raw = pw_plat_sample((unsigned)slot, &f, &r);
    s->raw = raw; s->frames = f; s->rate = r;
    if (raw) return 1;
    const uint8_t *z = pw_plat_sample_sea((unsigned)slot, &f, &r);
    if (!z) return 0;
    s->sea = z; s->frames = f; s->rate = r;
    return 1;
}
const char *pw_sample_name(unsigned id)
{
#if PW_INTERNAL_N > 0
    if (id < PW_INTERNAL_N) return pw_internal[id].name;
#endif
    int slot = snd_slot(id);
    return slot < 0 ? 0 : pw_plat_sample_name((unsigned)slot);
}
void pw_smp_ram(pw_smp *s, const int16_t *ram, uint32_t frames, uint32_t rate) { s->raw = ram; s->sea = 0; s->frames = frames; s->rate = rate; }
/* the reader of voice `key` (any nonzero number a voice keeps the same) for sound s, or 0 (a raw sound needs none; none free) */
pw_rdr *pw_rdr_get(unsigned key, const pw_smp *s)
{
    if (!s->sea) return 0;
    struct pw_rdr *free_ = 0;
    for (unsigned k = 0; k < PW_RDR_N; ++k) {
        struct pw_rdr *r = &rdrs[k];
        if (r->key == key) {
            if (r->src != s->sea || r->frames != s->frames || r->rev != pws.bank_rev) { r->src = s->sea; r->frames = s->frames; r->rev = pws.bank_rev; r->dec = r->base = 0; }
            r->blk = sea_blk;
            return r;
        }
        if (!free_ && (!r->key || r->blk + 2u < sea_blk)) free_ = r;
    }
    if (!free_) return 0;
    free_->key = (uint16_t)key; free_->src = s->sea; free_->frames = s->frames; free_->rev = pws.bank_rev; free_->dec = free_->base = 0; free_->blk = sea_blk;
    return free_;
}
int16_t pw_smp_get_z(const pw_smp *s, pw_rdr *r, uint32_t i)
{
    if (!r || i >= s->frames) return 0;
    if (i >= r->dec ? i - r->dec >= SEA_FPC : (r->dec - i > 44u || i < r->base)) r->dec = r->base = i / SEA_FPC * SEA_FPC;   /* (a jump: from its chunk's start) */
    while (r->dec <= i) rdr_slice(r);
    return r->ring[i & 63u];
}
void pw_smp_read(const pw_smp *s, uint32_t from, uint32_t n, int16_t *dst)
{
    if (s->raw) { for (uint32_t i = 0; i < n; ++i) dst[i] = from + i < s->frames ? s->raw[from + i] : 0; return; }
    int16_t tmp[SEA_FPC];
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t at = from + i;
        if (at >= s->frames) { dst[i] = 0; continue; }
        if (at / SEA_FPC != c) { c = at / SEA_FPC; sea_chunk(s->sea, s->frames, c, tmp); }
        dst[i] = tmp[at % SEA_FPC];
    }
}
/* The sound as n columns of peaks (0-255, a full-scale frame is 255): every frame of a compressed sound is looked at
 * (decoded once, the chunks in turn, and the answer kept until the bank changes), at most 64 a column of a raw one. */
void pw_smp_peaks(const pw_smp *s, uint8_t *pk, unsigned n, int unused)
{
    static struct { const uint8_t *src; uint32_t frames, rev; unsigned n; uint8_t pk[224]; } memo;
    (void)unused;
    if (!s->frames || !n) return;
    if (!s->sea) {
        for (unsigned c = 0; c < n; ++c) {
            uint32_t a = (uint32_t)((uint64_t)s->frames * c / n), b = (uint32_t)((uint64_t)s->frames * (c + 1u) / n), st = (b - a) / 64u + 1u;
            int p = 0;
            for (uint32_t i = a; i < b; i += st) { int v = s->raw[i] < 0 ? -s->raw[i] : s->raw[i]; if (v > p) p = v; }
            pk[c] = (uint8_t)(p >> 7 > 255 ? 255 : p >> 7);
        }
        return;
    }
    if (n <= sizeof memo.pk && memo.src == s->sea && memo.frames == s->frames && memo.n == n && memo.rev == pws.bank_rev) { memcpy(pk, memo.pk, n); return; }
    int16_t tmp[SEA_FPC];
    int p = 0;
    unsigned col = 0;
    uint32_t edge = (uint32_t)((uint64_t)s->frames * 1u / n);
    for (unsigned c = 0; c < n; ++c) pk[c] = 0;
    for (uint32_t ck = 0, base = 0; base < s->frames; ++ck, base += SEA_FPC) {
        uint32_t m = sea_chunk(s->sea, s->frames, ck, tmp);
        for (uint32_t i = 0; i < m; ++i) {
            if (base + i >= edge && col + 1u < n) {
                pk[col] = (uint8_t)(p >> 7 > 255 ? 255 : p >> 7); p = 0; ++col;
                edge = (uint32_t)((uint64_t)s->frames * (col + 1u) / n);
            }
            int v = tmp[i] < 0 ? -tmp[i] : tmp[i];
            if (v > p) p = v;
        }
    }
    pk[col] = (uint8_t)(p >> 7 > 255 ? 255 : p >> 7);
    if (n <= sizeof memo.pk) { memo.src = s->sea; memo.frames = s->frames; memo.n = n; memo.rev = pws.bank_rev; memcpy(memo.pk, pk, n); }
}
