/* SPDX-License-Identifier: GPL-3.0-only
 * PATCHWERK's sample bank in the FM-1's flash (pw_bank.h). Main loop only, except pw_plat_sample (the audio side
 * reads the cached directory and the flash in place). */
#include "pw_bank.h"
#include "pw_engine.h"
#include <string.h>

#define SECTOR 4096u
typedef struct { uint32_t lo, hi; } win;
static const win wins[3] = {{PW_BANK_DATA_LO, PW_BANK_DATA_HI}, {PW_BANK_X1_LO, PW_BANK_X1_HI}, {PW_BANK_X0_LO, PW_BANK_X0_HI}};   /* (in the order sounds are placed) */
static int in_win(uint32_t off, uint32_t bytes) { for (unsigned w = 0; w < 3; ++w) if (off >= wins[w].lo && off <= wins[w].hi && bytes <= wins[w].hi - off) return 1; return 0; }
static int extra_win(uint32_t off) { return off < PW_BANK_DATA_LO || off >= PW_BANK_DATA_HI; }   /* (the loader's record rule applies there) */
typedef struct { char name[12]; uint32_t off, frames, rate, crc; uint8_t codec; } entry;   /* codec 0: 16-bit samples, 1: SEA (pw_sea.inc) */
static entry dir[PW_BANK_TOTAL] PW_POOL;             /* the directory as in the flash; frames 0 = empty */
static uint8_t io[512] PW_POOL;
static void (*src_fill)(uint32_t off, uint8_t *dst, uint32_t n, void *ctx); static void *src_ctx;   /* a sound made here rather than sent: its bytes come from this */
static volatile uint8_t busy;                        /* the flash is being erased or written: the audio side finds nothing in the bank meanwhile */

static uint32_t rd32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
static void wr32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
/* the bytes a sound takes in the flash */
static uint32_t stored(const entry *e) { return e->codec ? pw_sea_bytes(e->frames) : e->frames * 2u; }
/* a slot a sound can be in: A (0-15) and B (PW_A2_SLOT..); the CZ and FM-DX banks between them are data */
static int is_sound(unsigned s) { return s < PW_SAMPLE_SLOTS || (s >= PW_A2_SLOT && s < PW_A2_SLOT + PW_SAMPLE_SLOTS); }
static int valid(unsigned s)
{
    return dir[s].frames && dir[s].frames != 0xFFFFFFFFu && (dir[s].codec || dir[s].frames < PW_BANK_ROOM / 2u) && dir[s].frames < 0x1000000u &&
           in_win(dir[s].off, stored(&dir[s])) && (!dir[s].codec || (is_sound(s) && dir[s].frames % 20u == 0));
}

void pw_bank_init(void)
{
    const uint8_t *d = pw_plat_flash(PW_BANK_DIR);
    memset(dir, 0, sizeof dir);
    unsigned n = d ? rd32(d + 4) : 0;                /* (a directory from before the CZ's slots lists 16, before FM-DX's 20, before B 24) */
    if (!d || memcmp(d, "NBK1", 4) || (n != PW_BANK_TOTAL && n != 40u && n != 24u && n != PW_SAMPLE_SLOTS && n != 17u && n != 20u)) return;
    if (~pw_crc32(~0u, d + 16, n * 32u) != rd32(d + 8)) return;
    for (unsigned s = 0; s < n; ++s) {
        const uint8_t *e = d + 16 + s * 32u;
        memcpy(dir[s].name, e, 12);
        dir[s].off = rd32(e + 12); dir[s].frames = rd32(e + 16); dir[s].rate = rd32(e + 20); dir[s].crc = rd32(e + 24);
        dir[s].codec = e[28] == 1 ? 1 : 0;           /* (0xFF in a directory from before the codec) */
        if (!valid(s)) dir[s].frames = 0;
    }
}
static int write_dir(void)
{
    static uint8_t d[16 + PW_BANK_TOTAL * 32];
    memset(d, 0xFF, sizeof d);
    memcpy(d, "NBK1", 4);
    wr32(d + 4, PW_BANK_TOTAL);
    for (unsigned s = 0; s < PW_BANK_TOTAL; ++s) {
        uint8_t *e = d + 16 + s * 32u;
        memcpy(e, dir[s].name, 12);
        wr32(e + 12, dir[s].off); wr32(e + 16, dir[s].frames); wr32(e + 20, dir[s].rate); wr32(e + 24, dir[s].crc);
        e[28] = dir[s].codec;
    }
    wr32(d + 8, ~pw_crc32(~0u, d + 16, PW_BANK_TOTAL * 32u));
    PW_TRACE(0x108);
    if (pw_plat_flash_erase(PW_BANK_DIR)) return PW_BANK_E_FLASH;
    PW_TRACE(0x109);
    return pw_plat_flash_write(PW_BANK_DIR, d, sizeof d) ? PW_BANK_E_FLASH : PW_BANK_OK;
}

/* the start of the first gap of `bytes` (sector-rounded) between the other slots' sounds, in the windows in turn, or 0; *largest the
 * biggest gap there is, *total all the free room */
static uint32_t find_space(uint32_t bytes, unsigned except, uint32_t *largest, uint32_t *total)
{
    uint32_t best = 0, found = 0, sum = 0;
    for (unsigned w = 0; w < 3; ++w) {
        uint32_t at = wins[w].lo;
        for (;;) {
            /* the lowest sound at or above `at`, in this window */
            int low = -1;
            for (unsigned s = 0; s < PW_BANK_TOTAL; ++s)
                if (s != except && dir[s].frames && dir[s].off >= wins[w].lo && dir[s].off < wins[w].hi && dir[s].off + stored(&dir[s]) > at && (low < 0 || dir[s].off < dir[low].off)) low = (int)s;
            uint32_t end = low < 0 ? wins[w].hi : dir[low].off;
            if (end > at) {
                uint32_t gap = end - at;
                sum += gap;
                if (gap > best) best = gap;
                if (!found && gap >= bytes) found = at;
            }
            if (low < 0) break;
            at = (dir[low].off + stored(&dir[low]) + SECTOR - 1u) & ~(SECTOR - 1u);
        }
    }
    if (largest) *largest = best;
    if (total) *total = sum;
    return found;
}
uint32_t pw_bank_free(void) { uint32_t b = 0; find_space(0xFFFFFFFFu, PW_BANK_TOTAL, &b, 0); return b; }
uint32_t pw_bank_free_total(void) { uint32_t t = 0; find_space(0xFFFFFFFFu, PW_BANK_TOTAL, 0, &t); return t; }

static int receive_inner(unsigned slot, uint32_t frames, uint32_t rate, const char *name, uint32_t crc, unsigned codec)
{
    if (slot >= PW_BANK_TOTAL || !frames || rate < 4000u || rate > 96000u || codec > 1u) return PW_BANK_E_ARG;
    if (codec && (!is_sound(slot) || frames % 20u)) return PW_BANK_E_ARG;   /* (only sounds are compressed: the CZ and FM-DX banks are data) */
    if (frames >= 0x1000000u) return PW_BANK_E_FULL;
    uint32_t bytes = codec ? pw_sea_bytes(frames) : frames * 2u, need = (bytes + SECTOR - 1u) & ~(SECTOR - 1u);
    if (need > PW_BANK_DATA_HI - PW_BANK_DATA_LO) return PW_BANK_E_FULL;   /* (the main window is the biggest) */   /* (the room, in bytes: a SEA sound holds ~4x the frames of a raw one) */
    PW_TRACE(0x101);
    uint32_t at = find_space(need, slot, 0, 0);
    PW_TRACE(0x102);
    if (!at) return PW_BANK_E_FULL;
    /* the slot empty first: a sound half written is never listed */
    if (dir[slot].frames) {
        dir[slot].frames = 0;
        if (write_dir()) return PW_BANK_E_FLASH;
    }
    for (uint32_t o = 0; o < need; o += SECTOR) {
        PW_TRACE(0x1000u | (o >> 12));
        if (pw_plat_flash_erase(at + o)) return PW_BANK_E_FLASH;
        PW_TRACE(0x1800u | (o >> 12));
        pw_plat_progress("ERASING", o * 30u / need);
    }
    for (uint32_t o = 0; o < bytes; o += sizeof io) {
        uint32_t n = bytes - o < sizeof io ? bytes - o : sizeof io;
        PW_TRACE(0x2000u | (o >> 9));
        if (src_fill) src_fill(o, io, n, src_ctx);
        else if (pw_plat_host_read(o, io, n)) return PW_BANK_E_HOST;
        if (extra_win(at))      /* (the update loader's record: "AT" at 0xF06 of a sector: never stored there) */
            for (uint32_t i = 0; i + 1u < n; ++i)
                if (((at + o + i) & (SECTOR - 1u)) == 0xF06u && io[i] == 'A' && io[i + 1] == 'T') return PW_BANK_E_REC;
        PW_TRACE(0x2800u | (o >> 9));
        if (pw_plat_flash_write(at + o, io, n)) return PW_BANK_E_FLASH;
        if ((o & 0x3FFFu) == 0) pw_plat_progress("RECEIVING", 30u + (o >> 10) * 70u / ((bytes >> 10) + 1u));
    }
    PW_TRACE(0x105);
    const uint8_t *f = pw_plat_flash(at);
    if (!f) return PW_BANK_E_CRC;
    if (src_fill) crc = ~pw_crc32(~0u, f, bytes);
    else if (~pw_crc32(~0u, f, bytes) != crc) return PW_BANK_E_CRC;
    memset(dir[slot].name, 0, 12);
    for (unsigned i = 0; i < 12 && name[i]; ++i) dir[slot].name[i] = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32) : name[i];
    dir[slot].off = at; dir[slot].rate = rate; dir[slot].crc = crc; dir[slot].codec = (uint8_t)codec;
    dir[slot].frames = frames;
    PW_TRACE(0x106);
    pw_plat_progress("DONE", 100);
    PW_TRACE(0x107);
    return write_dir();
}
int pw_bank_receive(unsigned slot, uint32_t frames, uint32_t rate, const char *name, uint32_t crc, unsigned codec)
{
    busy = 1;   /* (the audio side reads nothing from the bank while its flash is erased and written: a CZ reads its tone every block) */
    int r = receive_inner(slot, frames, rate, name, crc, codec);
    busy = 0;
    pws.bank_rev++;
    return r;
}
int pw_bank_delete(unsigned slot)
{
    if (slot >= PW_BANK_TOTAL) return PW_BANK_E_ARG;
    if (!dir[slot].frames) return PW_BANK_OK;
    busy = 1;
    dir[slot].frames = 0;
    int r = write_dir();
    busy = 0;
    pws.bank_rev++;
    return r;
}
int pw_bank_erase_kind(unsigned kind)   /* 1: the sounds (A: slots 0-15, B: 24-39), 2: the CZ banks (16-19), 3: the FM-DX banks (20-23); the saved RAM sounds stay */
{
    unsigned lo[3][2] = {{0, 16}, {16, 20}, {20, 24}};
    if (kind < 1 || kind > 3) return PW_BANK_E_ARG;
    busy = 1;
    for (unsigned s = lo[kind - 1][0]; s < lo[kind - 1][1]; ++s) memset(&dir[s], 0, sizeof dir[s]);
    if (kind == 1) for (unsigned s = 24; s < 40; ++s) memset(&dir[s], 0, sizeof dir[s]);
    int r = write_dir();
    busy = 0;
    pws.bank_rev++;
    return r;
}
int pw_bank_erase_all(void)
{
    busy = 1;
    memset(dir, 0, sizeof dir);
    int r = write_dir();
    busy = 0;
    pws.bank_rev++;
    return r;
}
void pw_bank_entry(unsigned slot, uint8_t out[20])
{
    memset(out, 0, 20);
    if (slot >= PW_BANK_TOTAL || !dir[slot].frames) return;
    wr32(out, dir[slot].frames); wr32(out + 4, dir[slot].rate | (dir[slot].codec ? 0x80000000u : 0u));   /* (bit 31 of the rate: compressed) */
    memcpy(out + 8, dir[slot].name, 12);
}

/* the engine's view (pw_engine.h): the audio side */
const int16_t *pw_plat_sample(unsigned slot, uint32_t *frames, uint32_t *rate)
{
    if (busy || slot >= PW_BANK_TOTAL || !dir[slot].frames || dir[slot].codec) return 0;
    const uint8_t *p = pw_plat_flash(dir[slot].off);
    if (!p) return 0;
    *frames = dir[slot].frames; *rate = dir[slot].rate;
    return (const int16_t *)(const void *)p;
}
const uint8_t *pw_plat_sample_sea(unsigned slot, uint32_t *frames, uint32_t *rate)
{
    if (busy || slot >= PW_BANK_TOTAL || !dir[slot].frames || !dir[slot].codec) return 0;
    const uint8_t *p = pw_plat_flash(dir[slot].off);
    if (!p) return 0;
    *frames = dir[slot].frames; *rate = dir[slot].rate;
    return p;
}
/* what is stored in a slot, for the link: frames, rate, codec, bytes (0 frames: empty) */
uint32_t pw_bank_stored(unsigned slot, uint32_t *frames, unsigned *codec, const uint8_t **at)
{
    if (slot >= PW_BANK_TOTAL || !dir[slot].frames) return 0;
    *frames = dir[slot].frames; *codec = dir[slot].codec; *at = pw_plat_flash(dir[slot].off);
    return *at ? stored(&dir[slot]) : 0;
}
const char *pw_plat_sample_name(unsigned slot)
{
    static char name[13];
    if (slot >= PW_BANK_TOTAL || !dir[slot].frames) return 0;
    memcpy(name, dir[slot].name, 12);
    name[12] = 0;
    for (int i = 11; i >= 0 && (name[i] == ' ' || !name[i]); --i) name[i] = 0;
    return name[0] ? name : "SAMPLE";
}

/* ---- a SAMPLER's RAM sound kept with a user patch ------------------------------------------------------------------------
 * The RAM holds a second at most. It is stored as IMA ADPCM (4 bits a frame: about 22 KB, a quarter of the raw 88 KB), in the bank slot
 * PW_BANK_RAM0 + 2 * patch + k (k: which of the patch's samplers, the first two with a recorded sound). The stream: header (frames u16,
 * first sample i16, step index u8, spare u8), then two codes a byte. Written when the patch is stored, read back when it is loaded; the
 * bytes are made and consumed in 512-byte pieces, so nothing but the flash holds the compressed copy. */
static const int16_t ima_step[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t ima_adj[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static int ima_clip(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static int ima_idx(int i) { return i < 0 ? 0 : i > 88 ? 88 : i; }
typedef struct { const int16_t *src; uint32_t frames; int pred, idx; } ima_enc_ctx;
static void ram_fill(uint32_t off, uint8_t *dst, uint32_t n, void *ctx)
{
    ima_enc_ctx *e = (ima_enc_ctx *)ctx;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t at = off + k, v = 0;
        if (at < 6u) {
            const uint32_t h = e->frames, p = (uint32_t)(uint16_t)e->src[0];
            v = at == 0 ? (h & 255u) : at == 1 ? (h >> 8) : at == 2 ? (p & 255u) : at == 3 ? (p >> 8) : at == 4 ? 0u : 0u;
            if (at == 0 && off == 0) { e->pred = e->src[0]; e->idx = 0; }
        } else {
            for (unsigned half = 0; half < 2; ++half) {
                uint32_t i = 1u + (at - 6u) * 2u + half;   /* (frame 0 is in the header) */
                int s = i < e->frames ? e->src[i] : e->pred, step = ima_step[e->idx], diff = s - e->pred, code = 0;
                if (diff < 0) { code = 8; diff = -diff; }
                int vp = step >> 3;
                if (diff >= step) { code |= 4; diff -= step; vp += step; }
                step >>= 1; if (diff >= step) { code |= 2; diff -= step; vp += step; }
                step >>= 1; if (diff >= step) { code |= 1; vp += step; }
                e->pred = ima_clip(e->pred + (code & 8 ? -vp : vp)); e->idx = ima_idx(e->idx + ima_adj[code & 7]);
                v |= (uint32_t)code << (half * 4u);
            }
        }
        dst[k] = (uint8_t)v;
    }
}
static unsigned ram_slot(unsigned patch, unsigned k) { return PW_BANK_RAM0 + 2u * patch + k; }
static int ram_put(unsigned slot, const int16_t *src, uint32_t frames)
{
    ima_enc_ctx e = {src, frames, src[0], 0};
    uint32_t bytes = 6u + (frames + 1u) / 2u; bytes = (bytes + 1u) & ~1u;
    src_fill = ram_fill; src_ctx = &e;
    busy = 1;
    int r = receive_inner(slot, bytes / 2u, 44100u, "RAM", 0, 0);
    busy = 0; src_fill = 0;
    return r;
}
void pw_ram_save(unsigned patch)
{
    if (patch >= 8u) return;
    unsigned k = 0;
    pw_ram_err = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES && k < 2u; ++i) {
        unsigned frames = 0, cap = 0;
        if (!pw_sampler_marked(i)) continue;
        const int16_t *b = pw_sampler_buffer(i, &cap);
        if (b && pw_sampler_recorded(i, &frames) && frames > 64u) { int r = ram_put(ram_slot(patch, k), b, frames); if (r && !pw_ram_err) pw_ram_err = (uint8_t)r; }
        else pw_bank_delete(ram_slot(patch, k));
        ++k;
    }
    for (; k < 2u; ++k) pw_bank_delete(ram_slot(patch, k));
}
void pw_ram_load(unsigned patch)
{
    if (patch >= 8u) return;
    unsigned k = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES && k < 2u; ++i) {
        unsigned slot = ram_slot(patch, k);
        if (!pw_sampler_marked(i)) continue;
        ++k;
        unsigned cap = 0; int16_t *b = pw_sampler_buffer(i, &cap);
        const uint8_t *p = dir[slot].frames ? pw_plat_flash(dir[slot].off) : 0;
        if (!b || !p) { pw_sampler_unmark(i); continue; }
        uint32_t n = p[0] | ((uint32_t)p[1] << 8);
        if (n < 64u || n > cap || 6u + (n + 1u) / 2u > dir[slot].frames * 2u) { pw_sampler_unmark(i); continue; }
        int pred = (int16_t)(p[2] | (p[3] << 8)), idx = 0;
        b[0] = (int16_t)pred;
        for (uint32_t f = 1; f < n; ++f) {
            unsigned code = (p[6u + (f - 1u) / 2u] >> (((f - 1u) & 1u) * 4u)) & 15u;
            int step = ima_step[idx], vp = step >> 3;
            if (code & 4) vp += step;
            if (code & 2) vp += step >> 1;
            if (code & 1) vp += step >> 2;
            pred = ima_clip(pred + (code & 8 ? -vp : vp)); idx = ima_idx(idx + ima_adj[code & 7]);
            b[f] = (int16_t)pred;
        }
        pw_sampler_commit(i, n);
    }
}
void pw_ram_erase(unsigned patch) { if (patch < 8u) { pw_bank_delete(ram_slot(patch, 0)); pw_bank_delete(ram_slot(patch, 1)); } }
