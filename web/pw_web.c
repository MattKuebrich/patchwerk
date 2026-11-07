/* SPDX-License-Identifier: GPL-3.0-only
 * PATCHWERK browser emulator glue: implements the FM-1 SDK calls the firmware
 * uses (panel, LCD, LEDs, Flash) on top of plain memory and exports the
 * engine to JavaScript. The firmware sources are compiled unchanged. */
#include "pw_engine.h"
#include "pw_ui.h"
#include "pw_presets.h"
#include "pw_hw.h"
#include "pw_link.h"
#include <string.h>

#define EXPORT(name) __attribute__((export_name(name)))

/* ---------------------------------------------------------- tiny libc */
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
int memcmp(const void *x, const void *y, size_t n)
{
    const unsigned char *a = x, *b = y;
    for (; n; --n, ++a, ++b) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }

/* --------------------------------------------------------- FM-1 shims */
static struct fm1_controls controls;
static uint16_t framebuffer[240 * 240];
static uint32_t fb_revision, now_ms, flash_revision, blit_px;
static uint8_t leds[41], leds_dim[41], leds_out[41];
#define FLASH_BYTES (64u * 1024u)
static uint8_t flash[FLASH_BYTES];
static unsigned master = 80, master_adc = 0xffff;
static float out_lr[PW_N * 2];

void fm1_controls_read(struct fm1_controls *s)
{
    *s = controls;
    for (int i = 0; i < 7; ++i) controls.encoders[i] = 0;
}
void fm1_led_set(unsigned key, int on) { if (key < sizeof leds) leds[key] = on != 0; }
void patchwerk_led_dim(unsigned key, int on) { if (key < sizeof leds) leds_dim[key] = on != 0; }
uint32_t fm1_millis(void) { return now_ms; }
unsigned fm1_battery_raw(void) { return 0; }   /* (the emulator has none) */
unsigned fm1_battery_level(void) { return 0; }
int fm1_screen_blit(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *px, unsigned stride)
{
    if (!px || x + w > 240 || y + h > 240 || stride < w) return -1;
    for (unsigned r = 0; r < h; ++r) memcpy(framebuffer + (y + r) * 240 + x, px + r * stride, w * 2u);
    ++fb_revision;
    blit_px += w * h;
    return 0;
}
uint32_t fm1_flash_size(void) { return FLASH_BYTES; }
int fm1_flash_read(uint32_t at, void *data, size_t n)
{
    if (at + n > FLASH_BYTES) return FM1_FLASH_INVALID;
    memcpy(data, flash + at, n);
    return FM1_FLASH_OK;
}
int fm1_flash_write(uint32_t at, const void *data, size_t n)
{
    if (at + n > FLASH_BYTES) return FM1_FLASH_INVALID;
    const uint8_t *s = data;
    for (size_t i = 0; i < n; ++i) flash[at + i] &= s[i]; /* NOR semantics */
    ++flash_revision;
    return FM1_FLASH_OK;
}
int fm1_flash_erase(uint32_t at)
{
    if (at % FM1_FLASH_SECTOR_BYTES || at >= FLASH_BYTES) return FM1_FLASH_INVALID;
    memset(flash + at, 0xff, FM1_FLASH_SECTOR_BYTES);
    ++flash_revision;
    return FM1_FLASH_OK;
}
void pw_plat_lock(void) {}
void pw_plat_unlock(void) {}
uint32_t pw_plat_ticks(void) { return 0; }
uint32_t pw_plat_ticks_per_us(void) { return 1000; }
int pw_plat_cpu1(uint32_t *count, int start) { (void)count; (void)start; return -1; }
uint32_t pw_plat_cpu1_post(pw_cpu1_fn fn, void *m, const float *const *in, float *const *out, unsigned connected) { (void)fn; (void)m; (void)in; (void)out; (void)connected; return 0; }
int pw_plat_cpu1_wait(uint32_t ticket, uint32_t *us) { (void)ticket; (void)us; return 0; }
void pw_plat_cpu1_stop(void) {}
pw_cpu1_fn pw_plat_cpu1_dummy(void) { return 0; }
void pw_plat_cpu1_info(uint32_t *w) { for (int i = 0; i < 6; i++) w[i] = 0; }
void pw_plat_cpu1_jobs(uint32_t *w) { for (int i = 0; i < 7; i++) w[i] = 0; }
int pw_plat_audio_running(void) { return 0; } /* single thread: edits are atomic */
uint32_t pw_plat_millis(void) { return now_ms; }
unsigned pw_plat_cpu(void) { return 255; }
unsigned pw_plat_master(unsigned adc, int *changed)
{
    *changed = 0;
    if (adc > 1023) return master;
    unsigned level = (adc * 100u + 511u) / 1023u;
    unsigned d = level > master ? level - master : master - level;
    if (master_adc == 0xffff) { master = level; master_adc = adc; return master; }
    if (d >= 3u || (d && (level == 0u || level >= 100u))) { *changed = 1; master = level; }
    master_adc = adc;
    return master;
}

/* ------------------------------------------------------------- exports */
EXPORT("pw_web_flash") uint8_t *pw_web_flash(void) { return flash; }
EXPORT("pw_web_flash_size") uint32_t pw_web_flash_size(void) { return FLASH_BYTES; }
EXPORT("pw_web_flash_rev") uint32_t pw_web_flash_rev(void) { return flash_revision; }
EXPORT("pw_web_flash_format") void pw_web_flash_format(void) { memset(flash, 0xff, sizeof flash); }
EXPORT("pw_web_init") void pw_web_init(int boot_patch)
{
    pw_init();
    controls.knob = 0xffff;
    pw_ui_init();
    if (boot_patch >= 0) pw_factory_build((unsigned)boot_patch);
}
EXPORT("pw_web_render") float *pw_web_render(void)
{
    float l[PW_N], r[PW_N];
    pw_render(l, r);
    float g = (float)master / 100.0f;
    g *= g;
    for (int i = 0; i < PW_N; ++i) { out_lr[i] = l[i] * g; out_lr[PW_N + i] = r[i] * g; }
    return out_lr;
}
EXPORT("pw_web_control") void pw_web_control(uint32_t ms) { now_ms = ms; pw_ui_control(); }
EXPORT("pw_web_set_keys") void pw_web_set_keys(uint32_t keys) { controls.keys = keys; }
EXPORT("pw_web_set_buttons") void pw_web_set_buttons(uint32_t b) { controls.buttons = b; }
EXPORT("pw_web_encoder") void pw_web_encoder(int i, int d) { if (i >= 0 && i < 7) controls.encoders[i] = (int16_t)(controls.encoders[i] + d); }
EXPORT("pw_web_set_knob") void pw_web_set_knob(int adc) { controls.knob = (uint16_t)adc; }
EXPORT("pw_web_master") unsigned pw_web_master(void) { return master; }
EXPORT("pw_web_fb") uint16_t *pw_web_fb(void) { return framebuffer; }
EXPORT("pw_web_blit_px") uint32_t pw_web_blit_px(void) { return blit_px; }
EXPORT("pw_web_redraw_all") void pw_web_redraw_all(void) { pw_ui_redraw_all(); }
static uint16_t pal_buf[64];
EXPORT("pw_web_palette_buf") uint16_t *pw_web_palette_buf(void) { return pal_buf; }
EXPORT("pw_web_palette") int pw_web_palette(int pal, int tone, int bright) { return pw_ui_palette(pal, tone, bright, pal_buf, 64); }
EXPORT("pw_web_user_palette") int pw_web_user_palette(int on) { return pw_ui_user_palette(on ? pal_buf : 0); }   /* (tests: the USER palette from pal_buf, or removed) */
EXPORT("pw_web_user_get") int pw_web_user_get(void) { return pw_ui_user_palette_get(pal_buf); }
EXPORT("pw_web_set_palette") void pw_web_set_palette(int pal) { pw_ui_set_palette(pal); }
static uint8_t set_buf[128];
EXPORT("pw_web_settings_buf") uint8_t *pw_web_settings_buf(void) { return set_buf; }
EXPORT("pw_web_settings_get") int pw_web_settings_get(void) { return pw_ui_settings_get(set_buf, (int)sizeof set_buf); }   /* (a backup of the emulator's settings) */
EXPORT("pw_web_settings_put") int pw_web_settings_put(int n) { return pw_ui_settings_put(set_buf, n); }
EXPORT("pw_web_custom") void pw_web_custom(int idx, int v) { pw_ui_custom(idx, v); }
EXPORT("pw_web_custom_use") void pw_web_custom_use(int on) { pw_ui_custom_use(on); }
EXPORT("pw_web_has_vis") int pw_web_has_vis(int type) { return pw_ui_has_vis((unsigned)type); }
EXPORT("pw_web_ui_page") int pw_web_ui_page(void) { return pw_ui_page(); }
EXPORT("pw_web_ui_select") void pw_web_ui_select(int slot) { pw_ui_select(slot); }
EXPORT("pw_web_fb_rev") uint32_t pw_web_fb_rev(void) { return fb_revision; }
EXPORT("pw_web_leds") uint8_t *pw_web_leds(void)   /* 0 dark, 1 lit, 2 glowing softly */
{
    for (unsigned i = 0; i < sizeof leds; ++i) leds_out[i] = leds[i] ? 1 : leds_dim[i] ? 2 : 0;
    return leds_out;
}

EXPORT("pw_web_add") int pw_web_add(int type, int x, int y) { return pw_add((unsigned)type, x, y); }
/* the library dropped on a jack (slot, is_out, port) or a cable: the module wired in there (pw_add_wired) */
EXPORT("pw_web_add_wired") int pw_web_add_wired(int type, int x, int y, int slot, int is_out, int port, int cable)
{
    return pw_add_wired((unsigned)type, x, y, slot, is_out, (unsigned)port, cable);
}
EXPORT("pw_web_remove") int pw_web_remove(int slot) { return pw_remove((unsigned)slot); }
EXPORT("pw_web_connect") int pw_web_connect(int sm, int so, int dm, int di) { return pw_connect((unsigned)sm, (unsigned)so, (unsigned)dm, (unsigned)di); }
EXPORT("pw_web_disconnect") int pw_web_disconnect(int c) { return pw_disconnect((unsigned)c); }
EXPORT("pw_web_set_param") void pw_web_set_param(int slot, int p, float v) { pw_set_param((unsigned)slot, (unsigned)p, v); }
EXPORT("pw_web_move") void pw_web_move(int slot, int x, int y) { pw_move((unsigned)slot, x, y); }
EXPORT("pw_web_set_bpm") void pw_web_set_bpm(float bpm) { pw_set_bpm(bpm); }
EXPORT("pw_web_play") void pw_web_play(int on) { pw_set_playing(on); }
EXPORT("pw_web_button") void pw_web_button(int slot, int down) { pw_button_virtual((unsigned)slot, down); }
EXPORT("pw_web_key") void pw_web_key(int note, int vel) { pw_key(note, vel); }
EXPORT("pw_web_midi") void pw_web_midi(int s, int a, int b) { pw_midi((uint8_t)s, (uint8_t)a, (uint8_t)b); }
EXPORT("pw_web_panic") void pw_web_panic(void) { pw_all_notes_off(); }
EXPORT("pw_web_factory") int pw_web_factory(int i) { return pw_factory_build((unsigned)i); }
EXPORT("pw_web_random") int pw_web_random(void) { return pw_random_patch(); }
EXPORT("pw_web_ungroup") void pw_web_ungroup(int slot) { pw_ui_ungroup(slot); }
EXPORT("pw_web_add_in_group") int pw_web_add_in_group(int type, int x, int y, int g) { int s = pw_add((unsigned)type, x, y); if (s >= 0 && g > 0) pw_set_group((unsigned)s, (unsigned)g); return s; }
EXPORT("pw_web_group") int pw_web_group(int i) { return pw_ui_sub_insert((unsigned)i); }
EXPORT("pw_web_factory_count") int pw_web_factory_count(void) { return PW_FACTORY_COUNT; }
EXPORT("pw_web_clear") void pw_web_clear(void) { pw_clear(); pw_set_playing(0); pw_random_name(pws.name); }
EXPORT("pw_web_revision") uint32_t pw_web_revision(void) { return pws.revision; }
EXPORT("pw_web_set_seq_step") void pw_web_set_seq_step(int slot, int step, int note, int gate)
{
    if (slot < 0 || slot >= PW_MAX_MODULES || pws.mod[slot].type != PW_T_SEQ || step < 0 || step > 15) return;
    pw_module *m = &pws.mod[slot];
    pw_undo_data((unsigned)slot);
    if (note < -36) note = -36;
    if (note > 36) note = 36;
    m->data[step] = (uint8_t)(int8_t)note;
    uint16_t g = (uint16_t)(m->data[16] | (m->data[17] << 8));
    if (gate) g |= (uint16_t)(1u << step); else g &= (uint16_t)~(1u << step);
    m->data[16] = (uint8_t)g; m->data[17] = (uint8_t)(g >> 8);
    pws.revision++;
}
EXPORT("pw_web_trig_set") void pw_web_trig_set(int slot, int track, int step, int v) { if (slot >= 0 && track >= 0 && step >= 0) pw_trig_set((unsigned)slot, (unsigned)track, (unsigned)step, v); }
EXPORT("pw_web_set_voices") void pw_web_set_voices(int n) { pw_set_voices((unsigned)n); }
EXPORT("pw_web_cable_amount") void pw_web_cable_amount(int c, int amt) { pw_cable_amount((unsigned)c, amt); }
EXPORT("pw_web_set_flags") void pw_web_set_flags(int slot, int f) { pw_set_flags((unsigned)slot, (unsigned)f); }
/* a variation: linked, the FM-1 switches (its values come back); else here */
EXPORT("pw_web_var_select") void pw_web_var_select(int v) { if (pw_link_active()) { if (v >= 0 && v < PW_VARS) { pws.var_cur = (uint8_t)v; pws.revision++; } } else pw_var_select((unsigned)v); }
EXPORT("pw_web_var_copy") void pw_web_var_copy(int from, int to) { pw_var_copy((unsigned)from, (unsigned)to); }
EXPORT("pw_web_perf_assign") int pw_web_perf_assign(int slot, int p) { return pw_perf_assign((unsigned)slot, (unsigned)p); }
EXPORT("pw_web_perf_clear") void pw_web_perf_clear(int k) { pw_perf_clear((unsigned)k); }
EXPORT("pw_web_undo") int pw_web_undo(void) { return pw_undo(); }
EXPORT("pw_web_redo") int pw_web_redo(void) { return pw_redo(); }
EXPORT("pw_web_copy") int pw_web_copy(int slot) { return pw_copy((unsigned)slot); }
EXPORT("pw_web_paste") int pw_web_paste(int x, int y) { return pw_paste(x, y); }
EXPORT("pw_web_roll_demo") void pw_web_roll_demo(int slot)   /* the PIANO ROLL picture's notes: a chord and a short tune over it */
{
    static const uint8_t n[][3] = {{0, 4, 57}, {0, 4, 60}, {0, 4, 64}, {4, 4, 53}, {4, 4, 57}, {4, 4, 60}, {8, 4, 55}, {8, 4, 59}, {8, 4, 62}, {0, 2, 72}, {2, 2, 76}, {4, 2, 74}, {6, 2, 72}, {8, 4, 71}, {12, 4, 67}};
    if (slot < 0 || slot >= PW_MAX_MODULES || pws.mod[slot].type != PW_T_PROLL) return;
    for (unsigned k = 0; k < sizeof n / sizeof n[0]; ++k) pw_roll_add(&pws.mod[slot], n[k][0], n[k][1], n[k][2], 24);
}
EXPORT("pw_web_seq_fill") void pw_web_seq_fill(int slot, int clear) { if (slot >= 0 && slot < PW_MAX_MODULES) pw_seq_fill((unsigned)slot, clear); }
EXPORT("pw_web_mseq_set") void pw_web_mseq_set(int slot, int lane, int step, int v)   /* MOD SEQ: lane 0-3 */
{
    if (slot < 0 || slot >= PW_MAX_MODULES || pws.mod[slot].type != PW_T_MODSEQ || step < 0 || step > 15 || lane < 0 || lane > 3) return;
    pw_undo_data((unsigned)slot);
    pw_mseq_set(&pws.mod[slot], (unsigned)lane, (unsigned)step, v);
    pws.revision++;
}
EXPORT("pw_web_set_seq_feat") void pw_web_set_seq_feat(int slot, int step, int what, int v)   /* SEQUENCER step: 0 ratchet 1-4, 1 chance 0-15, 2 velocity 0-255 */
{
    if (slot < 0 || slot >= PW_MAX_MODULES || pws.mod[slot].type != PW_T_SEQ || step < 0 || step > 15) return;
    pw_module *m = &pws.mod[slot];
    pw_undo_data((unsigned)slot);
    uint8_t *fb = &m->data[PW_SEQ_STEPFEAT + step];
    if (what == 0) { v = v < 1 ? 1 : v > 4 ? 4 : v; *fb = (uint8_t)((*fb & 0xfc) | (v - 1)); }
    else if (what == 1) { v = v < 0 ? 0 : v > 15 ? 15 : v; *fb = (uint8_t)((*fb & 0x0f) | (v << 4)); }
    else m->data[PW_SEQ_STEPVEL + step] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    pws.revision++;
}
EXPORT("pw_web_set_data") void pw_web_set_data(int slot, int i, int v)   /* CHORD STEPS and GRAVITY: a step's byte */
{
    if (slot < 0 || slot >= PW_MAX_MODULES || i < 0 || i > 15) return;
    unsigned t = pws.mod[slot].type;
    if (t != PW_T_CHORDSTEPS && t != PW_T_GRAVITY) return;
    pw_undo_data((unsigned)slot);
    pws.mod[slot].data[i] = (uint8_t)v;
    pws.revision++;
}
EXPORT("pw_web_kick") void pw_web_kick(int slot) { if (slot >= 0 && slot < PW_MAX_MODULES) pw_kick((unsigned)slot); }
EXPORT("pw_web_set_name") void pw_web_set_name(int c0, int c1, int c2, int c3)
{
    /* twelve characters packed little-endian in four words of three bytes */
    int w[4] = {c0, c1, c2, c3};
    for (int i = 0; i < 12; ++i) {
        int ch = (w[i / 3] >> (8 * (i % 3))) & 255;
        pws.name[i] = (char)((ch >= 32 && ch < 127) ? (ch >= 'a' && ch <= 'z' ? ch - 32 : ch) : 0);
    }
    pws.name[12] = 0;
    pws.revision++;
}

/* Shared transfer buffer for patches, JSON and audio. */
static uint8_t xfer[96 * 1024];
static unsigned xfer_len;
/* ------------------------------------------------------------- sample bank
   The emulator's bank: PW_BANK_SLOTS sounds in memory (the page keeps them in the browser's storage and sends
   them to an FM-1's flash). */
#define BANK_FRAMES (4u * 1024u * 1024u)      /* 95 s at 44.1 kHz in all */
static int16_t bank_mem[BANK_FRAMES];
static struct { uint32_t off, frames, rate, words; uint8_t codec; char name[13]; } bank[PW_BANK_SLOTS];   /* (words: 16-bit words held; codec 1: SEA stream, pw_sea.inc) */
const int16_t *pw_plat_sample(unsigned slot, uint32_t *frames, uint32_t *rate)
{
    if (slot >= PW_BANK_SLOTS || !bank[slot].frames || bank[slot].codec) return 0;
    *frames = bank[slot].frames; *rate = bank[slot].rate;
    return bank_mem + bank[slot].off;
}
const uint8_t *pw_plat_sample_sea(unsigned slot, uint32_t *frames, uint32_t *rate)
{
    if (slot >= PW_BANK_SLOTS || !bank[slot].frames || !bank[slot].codec) return 0;
    *frames = bank[slot].frames; *rate = bank[slot].rate;
    return (const uint8_t *)(const void *)(bank_mem + bank[slot].off);
}
const char *pw_plat_sample_name(unsigned slot) { return slot < PW_BANK_SLOTS && bank[slot].frames ? bank[slot].name : 0; }
/* room for `frames` 16-bit words in slot (its old sound let go): where to write them, or 0. The bank is compacted first. */
EXPORT("pw_web_bank_alloc") int16_t *pw_web_bank_alloc(int slot, int frames, int rate)
{
    if (slot < 0 || slot >= PW_BANK_SLOTS || frames <= 0) return 0;
    bank[slot].frames = 0;
    uint32_t at = 0;
    for (int pass = 0; pass < PW_BANK_SLOTS; ++pass) {      /* compact: each in order of its offset */
        int lowest = -1;
        for (int k = 0; k < PW_BANK_SLOTS; ++k)
            if (bank[k].frames && bank[k].off >= at && (lowest < 0 || bank[k].off < bank[lowest].off)) lowest = k;
        if (lowest < 0) break;
        if (bank[lowest].off != at) memmove(bank_mem + at, bank_mem + bank[lowest].off, bank[lowest].words * 2u);
        bank[lowest].off = at;
        at += bank[lowest].words;
    }
    if (at + (uint32_t)frames + 1u > BANK_FRAMES) return 0;   /* (+1: the decoder may look a byte past a stream) */
    bank[slot].off = at; bank[slot].rate = (uint32_t)rate; bank[slot].words = (uint32_t)frames;
    return bank_mem + at;
}
/* frames: the sound's length; codec 1: the words written are a SEA stream (frames a multiple of 20, only for B1-B16) */
EXPORT("pw_web_bank_commit") void pw_web_bank_commit(int slot, int frames, int codec, int c0, int c1, int c2, int c3)
{
    if (slot < 0 || slot >= PW_BANK_SLOTS) return;
    bank[slot].codec = (uint8_t)(codec == 1 && (slot < PW_SAMPLE_SLOTS || (slot >= PW_A2_SLOT && slot < PW_A2_SLOT + PW_SAMPLE_SLOTS)) && frames % 20 == 0);
    int w[4] = {c0, c1, c2, c3};
    for (int i = 0; i < 12; ++i) {
        int ch = (w[i / 3] >> (8 * (i % 3))) & 255;
        bank[slot].name[i] = (char)((ch >= 32 && ch < 127) ? (ch >= 'a' && ch <= 'z' ? ch - 32 : ch) : 0);
    }
    bank[slot].name[12] = 0;
    bank[slot].frames = (uint32_t)frames;
    if (bank[slot].codec && bank[slot].words * 2u < pw_sea_bytes((uint32_t)frames)) bank[slot].frames = 0;   /* (a stream cut short: not listed) */
    pws.revision++; pws.bank_rev++;
}
EXPORT("pw_web_bank_clear") void pw_web_bank_clear(int slot) { if (slot >= 0 && slot < PW_BANK_SLOTS) { bank[slot].frames = 0; pws.revision++; pws.bank_rev++; } }
EXPORT("pw_web_bank_frames") int pw_web_bank_frames(int slot) { return slot >= 0 && slot < PW_BANK_SLOTS ? (int)bank[slot].frames : 0; }
EXPORT("pw_web_bank_codec") int pw_web_bank_codec(int slot) { return slot >= 0 && slot < PW_BANK_SLOTS && bank[slot].frames ? bank[slot].codec : 0; }
EXPORT("pw_web_sea_bytes") int pw_web_sea_bytes(int frames) { return frames > 0 ? (int)pw_sea_bytes((uint32_t)frames) : 0; }
EXPORT("pw_web_bank_ptr") int16_t *pw_web_bank_ptr(int slot) { return slot >= 0 && slot < PW_BANK_SLOTS && bank[slot].frames ? bank_mem + bank[slot].off : 0; }

/* -------------------------------------------------------------- MIDI out
   MIDI OUT modules' messages, queued for the page to send to the computer's MIDI outputs */
static uint8_t mo_q[3 * 256]; static unsigned mo_n;
void pw_plat_midi_out(uint8_t st, uint8_t d1, uint8_t d2)
{
    if (mo_n >= 256) return;
    mo_q[mo_n * 3] = st; mo_q[mo_n * 3 + 1] = d1; mo_q[mo_n * 3 + 2] = d2; ++mo_n;
}
static uint8_t mo_out[3 * 256];
EXPORT("pw_web_midi_out") uint8_t *pw_web_midi_out(void) { return mo_out; }
EXPORT("pw_web_midi_out_take") int pw_web_midi_out_take(void) { unsigned n = mo_n; memcpy(mo_out, mo_q, n * 3); mo_n = 0; return (int)n; }

/* -------------------------------------------------------------- audio in
   The page's microphone (or any input it is given), pushed in ahead of the render. */
#define AIN_N 8192u
static float ain_l[AIN_N], ain_r[AIN_N];
static uint32_t ain_w, ain_r_at, ain_on;
static float ain_push_l[PW_N * 8], ain_push_r[PW_N * 8];
EXPORT("pw_web_ain_buf") float *pw_web_ain_buf(void) { return ain_push_l; }        /* left, then right at +PW_N * 8 */
EXPORT("pw_web_ain_bufr") float *pw_web_ain_bufr(void) { return ain_push_r; }
EXPORT("pw_web_ain_push") void pw_web_ain_push(int n)
{
    ain_on = 1;
    for (int i = 0; i < n && i < PW_N * 8; ++i) {
        ain_l[ain_w % AIN_N] = ain_push_l[i]; ain_r[ain_w % AIN_N] = ain_push_r[i]; ++ain_w;
    }
    if (ain_w - ain_r_at > AIN_N / 2) ain_r_at = ain_w - PW_N * 4;   /* far behind: catch up */
}
EXPORT("pw_web_ain_stop") void pw_web_ain_stop(void) { ain_on = 0; }
int pw_plat_audio_in(float *l, float *r)
{
    if (!ain_on || ain_w - ain_r_at < PW_N) return 0;
    for (int i = 0; i < PW_N; ++i) { l[i] = ain_l[ain_r_at % AIN_N]; r[i] = ain_r[ain_r_at % AIN_N]; ++ain_r_at; }
    return 1;
}

/* ------------------------------------------------------------- live link
   The page as the link's host (an FM-1 at the other end), or, in the tests, as its device. Frames out are
   queued here and taken by the page (pw_web_link_tx); frames in arrive in the transfer buffer (pw_web_link_rx). */
static uint8_t link_q[16384];
static uint32_t link_qn;
static uint8_t link_out[PW_LINK_WIRE_MAX + 8];
void pw_plat_link_send(const uint8_t *frame, unsigned n)
{
    if (link_qn + n + 2 > sizeof link_q || n > PW_LINK_WIRE_MAX + 4) return;
    link_q[link_qn] = (uint8_t)n; link_q[link_qn + 1] = (uint8_t)(n >> 8);
    memcpy(link_q + link_qn + 2, frame, n);
    link_qn += n + 2;
}
EXPORT("pw_web_link_init") void pw_web_link_init(int role) { pw_link_init(role); link_qn = 0; }
EXPORT("pw_web_link_hello") void pw_web_link_hello(uint32_t ms) { pw_link_hello(ms); }
EXPORT("pw_web_link_rx") int pw_web_link_rx(int len, uint32_t ms) { return len > 2 ? pw_link_rx(xfer + 1, (unsigned)len - 2, ms) : 0; }
EXPORT("pw_web_link_poll") void pw_web_link_poll(uint32_t ms) { pw_link_poll(ms); }
EXPORT("pw_web_link_active") int pw_web_link_active(void) { return pw_link_active(); }
EXPORT("pw_web_link_cap") int pw_web_link_cap(void) { unsigned m, c; return pw_link_capacity(&m, &c) ? (int)(m | c << 8) : 0; }
EXPORT("pw_web_link_stat") uint32_t pw_web_link_stat(int what) { return pw_link_stats((unsigned)what); }
EXPORT("pw_web_link_out") uint8_t *pw_web_link_out(void) { return link_out; }
/* the next queued frame into pw_web_link_out: its length, 0 when none */
EXPORT("pw_web_link_tx") int pw_web_link_tx(void)
{
    if (!link_qn) return 0;
    unsigned n = link_q[0] | (link_q[1] << 8);
    memcpy(link_out, link_q + 2, n);
    memmove(link_q, link_q + 2 + n, link_qn - 2 - n);
    link_qn -= 2 + n;
    return (int)n;
}

EXPORT("pw_web_buf") uint8_t *pw_web_buf(void) { return xfer; }
EXPORT("pw_web_buf_len") unsigned pw_web_buf_len(void) { return xfer_len; }
EXPORT("pw_web_save") int pw_web_save(void) { int n = pw_save(xfer, sizeof xfer); xfer_len = n > 0 ? (unsigned)n : 0; return n; }
EXPORT("pw_web_load") int pw_web_load(int len) { return pw_load(xfer, (unsigned)len); }
int pw_store_raw_get(unsigned slot, uint8_t *dst, unsigned cap);
int pw_store_raw_put(unsigned slot, const uint8_t *src, unsigned n);
EXPORT("pw_web_example_lines") int pw_web_example_lines(int type) { return pw_ui_example_lines((unsigned)type); }
EXPORT("pw_web_help_lines_xs") int pw_web_help_lines_xs(int type) { return pw_ui_help_lines_xs((unsigned)type); }
EXPORT("pw_web_example_lines_xs") int pw_web_example_lines_xs(int type) { return pw_ui_example_lines_xs((unsigned)type); }
EXPORT("pw_web_help_lines") int pw_web_help_lines(int type) { return pw_ui_help_lines((unsigned)type); }
EXPORT("pw_web_slot_get") int pw_web_slot_get(int slot) { int n = pw_store_raw_get((unsigned)slot, xfer, sizeof xfer); xfer_len = (unsigned)n; return n; }
EXPORT("pw_web_slot_put") int pw_web_slot_put(int slot, int len) { return pw_store_raw_put((unsigned)slot, xfer, (unsigned)len); }
EXPORT("pw_web_sampler_buffer") int16_t *pw_web_sampler_buffer(int slot) { return pw_sampler_buffer((unsigned)slot, &xfer_len); }
EXPORT("pw_web_sampler_commit") void pw_web_sampler_commit(int slot, int frames) { pw_sampler_commit((unsigned)slot, (unsigned)frames); }

/* Live meters: per slot 16 bytes: used, n_out, out levels[6], in levels[6], seq step+1, status. */
static uint8_t live[PW_MAX_MODULES * 16 + 8];
EXPORT("pw_web_live") uint8_t *pw_web_live(void)
{
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        uint8_t *o = live + i * 16;
        const pw_module *m = &pws.mod[i];
        o[0] = m->used == 1;
        o[1] = m->n_out;
        for (int k = 0; k < 6; ++k) { o[2 + k] = m->level[k]; o[8 + k] = m->in_level[k]; }
        int st = pw_link_active() ? pw_link_remote_step(i) : pw_seq_step(i);
        o[14] = (uint8_t)(st + 1);
        o[15] = m->status;
    }
    uint8_t *t = live + PW_MAX_MODULES * 16;
    float l = pws.out_peak_l * 255.0f, r = pws.out_peak_r * 255.0f;
    t[0] = (uint8_t)(l > 255 ? 255 : l); t[1] = (uint8_t)(r > 255 ? 255 : r);
    t[2] = pws.playing; t[3] = pws.kb_gate;
    return live;
}

/* ----------------------------------------------------------- JSON out */
static char *jp;
static char *jend;
static void js(const char *s) { while (*s && jp < jend) *jp++ = *s++; }
static void jc(char c) { if (jp < jend) *jp++ = c; }
static void jstr(const char *s)
{
    jc('"');
    for (; *s; ++s) { if (*s == '"' || *s == '\\') jc('\\'); jc(*s); }
    jc('"');
}
static void jint(int v)
{
    char t[12]; int n = 0;
    if (v < 0) { jc('-'); v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) jc(t[--n]);
}
static void jnum(float v)
{
    if (!(v == v)) { js("0"); return; }
    if (v < 0) { jc('-'); v = -v; }
    if (v > 2e9f) v = 2e9f;
    unsigned ip = (unsigned)v;
    float f = v - (float)ip;
    unsigned frac = (unsigned)(f * 1000000.0f + 0.5f);
    if (frac >= 1000000u) { ++ip; frac -= 1000000u; }
    jint((int)ip);
    if (frac) {
        char d[7]; int n = 6;
        for (int i = 5; i >= 0; --i) { d[i] = (char)('0' + frac % 10); frac /= 10; }
        while (n > 0 && d[n - 1] == '0') --n;
        jc('.');
        for (int i = 0; i < n; ++i) jc(d[i]);
    }
}
static const char *const kind_name[] = {"lin", "exp", "enum", "int"};
static const char *const unit_name[] = {"", "Hz", "s", "%", "V", "st", "c", "bpm", "x", "Hz/V", "dB", "div"};
EXPORT("pw_web_types_json") unsigned pw_web_types_json(void)
{
    jp = (char *)xfer; jend = (char *)xfer + sizeof xfer - 1;
    js("{\"cats\":[");
    for (int c = 0; c < PW_C_COUNT; ++c) { if (c) jc(','); jstr(pw_cat_names[c]); }
    js("],\"max\":{\"modules\":"); jint(PW_MAX_MODULES); js(",\"cables\":"); jint(PW_MAX_CABLES);
    js("},\"factory\":[");
    for (int i = 0; i < PW_FACTORY_COUNT; ++i) { if (i) jc(','); jstr(pw_factory_name((unsigned)i)); }
    js("],\"groups\":[");
    for (int i = 0; i < pw_ui_sub_count(); ++i) { if (i) jc(','); js("{\"name\":"); jstr(pw_ui_sub_name((unsigned)i)); js(",\"what\":"); jstr(pw_ui_sub_what((unsigned)i)); jc('}'); }
    js("],\"types\":[");
    for (int t = 0; t < PW_T_COUNT; ++t) {
        const pw_type *d = &pw_types[t];
        if (t) jc(',');
        js("{\"name\":"); jstr(d->name); js(",\"tag\":"); jstr(d->tag); js(",\"help\":"); jstr(pw_type_help((unsigned)t));
        js(",\"cat\":"); jint(d->cat);
        if (pw_type_hidden((unsigned)t)) js(",\"hidden\":1");
        js(",\"ins\":[");
        for (int i = 0; i < d->n_in; ++i) { if (i) jc(','); jstr(d->in[i]); }
        js("],\"outs\":[");
        for (int i = 0; i < d->n_out; ++i) { if (i) jc(','); jstr(d->out[i]); }
        js("],\"mem\":"); jint((int)d->mem_ideal);
        js(",\"order\":[");
        for (int p = 0; p < d->n_param; ++p) { if (p) jc(','); jint((int)pw_param_at((unsigned)t, (unsigned)p)); }
        jc(']');
        if (d->pages) { js(",\"pages\":["); for (int p = 0; p < (d->n_param + 3) / 4; ++p) { if (p) jc(','); jstr(d->pages[p]); } jc(']'); }
        js(",\"params\":[");
        for (int p = 0; p < d->n_param; ++p) {
            const pw_param_desc *q = &d->param[p];
            if (p) jc(',');
            js("{\"name\":"); jstr(q->name); js(",\"kind\":"); jstr(kind_name[q->kind]);
            js(",\"unit\":"); jstr(unit_name[q->unit]);
            js(",\"min\":"); jnum(q->min); js(",\"max\":"); jnum(q->max); js(",\"def\":"); jnum(q->def);
            if (q->kind == PW_ENUM) {
                js(",\"labels\":[");
                for (int l = 0; l <= (int)q->max; ++l) { if (l) jc(','); jstr(q->labels[l]); }
                jc(']');
            }
            jc('}');
        }
        js("]}");
    }
    js("]}");
    xfer_len = (unsigned)(jp - (char *)xfer);
    return xfer_len;
}
EXPORT("pw_web_patch_json") unsigned pw_web_patch_json(void)
{
    jp = (char *)xfer; jend = (char *)xfer + sizeof xfer - 1;
    js("{\"name\":"); jstr(pws.name); js(",\"bpm\":"); jnum(pws.bpm);
    js(",\"voices\":"); jint(pws.voices); js(",\"var\":"); jint(pws.var_cur); js(",\"clip\":"); jint(pw_clip_type());
    js(",\"perf\":[");
    for (int k = 0; k < PW_PERF; ++k) { if (k) jc(','); jc('['); jint(pws.perf[k][0] == 0xff ? -1 : pws.perf[k][0]); jc(','); jint(pws.perf[k][1] == 0xff ? -1 : pws.perf[k][1]); jc(']'); }
    jc(']');
    js(",\"playing\":"); jint(pws.playing); js(",\"rev\":"); jint((int)pws.revision);
    js(",\"modules\":[");
    int first = 1;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        const pw_module *m = &pws.mod[i];
        if (m->used != 1) continue;
        const pw_type *t = &pw_types[m->type];
        if (!first) jc(',');
        first = 0;
        char label[12], text[12];
        pw_module_label(i, label);
        js("{\"slot\":"); jint((int)i); js(",\"type\":"); jint(m->type);
        js(",\"x\":"); jint(m->x); js(",\"y\":"); jint(m->y);
        js(",\"group\":"); jint(m->group); js(",\"label\":"); jstr(label); js(",\"status\":"); jint(m->status); js(",\"flags\":"); jint(m->flags);
        js(",\"p\":[");
        for (int p = 0; p < t->n_param; ++p) { if (p) jc(','); jnum(m->p[p]); }
        js("],\"t\":[");
        for (int p = 0; p < t->n_param; ++p) { if (p) jc(','); pw_format_param(i, (unsigned)p, text); jstr(text); }
        jc(']');
        if (m->type == PW_T_SEQ) {
            js(",\"steps\":[");
            for (int s = 0; s < 16; ++s) { if (s) jc(','); jint((int8_t)m->data[s]); }
            js("],\"gates\":"); jint(m->data[16] | (m->data[17] << 8));
            js(",\"feat\":[");
            for (int st = 0; st < 16; ++st) { if (st) jc(','); jint(m->data[PW_SEQ_STEPFEAT + st]); }
            js("],\"vel\":[");
            for (int st = 0; st < 16; ++st) { if (st) jc(','); jint(m->data[PW_SEQ_STEPVEL + st]); }
            jc(']');
        }
        if (m->type == PW_T_MODSEQ || m->type == PW_T_CHORDSTEPS || m->type == PW_T_GRAVITY) {
            js(",\"data\":[");
            for (int st = 0; st < PW_DATA_BYTES; ++st) { if (st) jc(','); jint(m->data[st]); }
            jc(']');
        }
        if (m->type == PW_T_TRIGSEQ) {
            js(",\"trig\":[");
            for (int t = 0; t < 4; ++t) for (int st = 0; st < 16; ++st) { if (t || st) jc(','); jint(pw_trig_get(i, (unsigned)t, (unsigned)st)); }
            jc(']');
        }
        jc('}');
    }
    js("],\"cables\":[");
    first = 1;
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (!k->used) continue;
        if (!first) jc(',');
        first = 0;
        jc('['); jint((int)c); jc(','); jint(k->sm); jc(','); jint(k->so); jc(','); jint(k->dm); jc(','); jint(k->di); jc(','); jint(k->amt); jc(']');
    }
    js("]}");
    xfer_len = (unsigned)(jp - (char *)xfer);
    return xfer_len;
}
