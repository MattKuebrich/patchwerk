/* SPDX-License-Identifier: GPL-3.0-only */
/* PATCHWERK on fm1-base: binds the core (core/: engine, modules, UI, presets) to plat.h.
 * Part of the platform's unity build (base/device/unity.c, base/host/plat_host.c), after gfx.c and
 * announce.c; the core itself is compiled apart at -O2 (fm1.toml: o2) and reaches the panel, the
 * screen and the store through the functions defined here (core/pw_hw.h, core/pw_store.h). */
#if defined(PLAT_HOST)
#include <stdlib.h>
#endif
#include "pw_hw.h"
#include "pw_engine.h"
#include "pw_ui.h"
#include "pw_store.h"
#include "pw_link.h"
#include "pw_bank.h"
#include "touch.h"                       /* (base/lib: velocity and pressure from the keys) */

/* ------------------------------------------------------------------ audio --- */
static volatile uint32_t hold;           /* pw_plat_lock depth: the render outputs silence meanwhile */
static volatile uint8_t ready, panicked, overloaded;
static float buf_l[PW_N], buf_r[PW_N];
static float gain_now;
static uint32_t render_at;               /* the engine block's first frame within app_render's (AUDIO IN) */

/* AUDIO IN: the computer's audio over USB (fm1-base's uac_play, in felucca-bsp's usb.c earlier in the same unity
 * build, takes it from its ring after app_render, at the gain APP_UAC_GAIN gives it: 0, so it is not heard there).
 * Read here without taking it from the ring, at the frames uac_play takes next. */
int pw_plat_audio_in(float *l, float *r)
{
#if !defined(PLAT_HOST) && FELUCCA_UAC_OUT
    uint32_t rd = uo_r, i;
    if (!uo.alt || !uo.go || uo_w - rd < render_at + PW_N)
        return 0;
    for (i = 0; i < PW_N; i++) {
        uint32_t v = uo_ring[(rd + render_at + i) & (UO_N - 1u)];
        l[i] = (float)(int16_t)v * (1.0f / 32768.0f);
        r[i] = (float)(int16_t)(v >> 16) * (1.0f / 32768.0f);
    }
    return 1;
#else
    (void)l, (void)r;
    return 0;
#endif
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t done = 0, i;
    float gain = (float)master_q12 * (8388607.0f / 4096.0f);
    if (!ready || hold || panicked) {
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        return;
    }
    while (done + PW_N <= n) {
        render_at = done;
        pw_render(buf_l, buf_r);
        for (i = 0; i < PW_N; i++) {
            float l, r;
            gain_now += (gain - gain_now) * 0.004f;   /* MASTER glides: no step at a block's edge */
            l = buf_l[i] * gain_now;
            r = buf_r[i] * gain_now;
            lr[2u * (done + i)] = (int32_t)(l > 8388607.0f ? 8388607.0f : l < -8388607.0f ? -8388607.0f : l);
            lr[2u * (done + i) + 1u] = (int32_t)(r > 8388607.0f ? 8388607.0f : r < -8388607.0f ? -8388607.0f : r);
        }
        done += PW_N;
    }
    for (i = 2u * done; i < 2u * n; i++)     /* (APP_BLOCK is a multiple of PW_N: never) */
        lr[i] = 0;
}

void app_panic(void)
{
    panicked = 1;                            /* an update is about to rewrite the flash: silence for good */
    pw_all_notes_off();
}

void app_overload(void) { overloaded = 1; }
void app_splash(void) { pw_ui_splash(); }
void app_splash_frame(void) { pw_ui_splash_frame(); }   /* PATCHWERK's boot screen, the emulator's too */
void app_redraw(void) { pw_ui_redraw_all(); }   /* the platform drew over the screen: send it all again */

/* pw_engine.h's platform hooks. On one core the main loop never runs inside the render, so holding it
 * is a flag the next audio interrupt sees. */
static void cpu1_quiesce(void);
void pw_plat_lock(void) { hold++; cpu1_quiesce(); }
void pw_plat_unlock(void) { if (hold) hold--; }
/* every flash erase and write (base/bsp/chip/fm1_flash.h): the render stops and the second core finishes its last job first (CPU1 runs module code from the flash) */
void app_flash_begin(void) { pw_plat_lock(); }
void app_flash_end(void) { pw_plat_unlock(); }
#ifdef PLAT_HOST
int pw_plat_audio_running(void) { return 0; }   /* the simulator renders between frames, not during them */
#else
int pw_plat_audio_running(void) { return ready && !hold; }
#endif
uint32_t pw_plat_millis(void) { return plat_ms(); }
unsigned pw_plat_cpu(void) { return plat_cpu_pct(); }
#if defined(APP_SYS_TEXT) && !defined(PLAT_HOST) && !defined(PLAT_WEB)
/* the platform's own screens (base/device/sys_text.c) in PATCHWERK's faces (pw_ui.c), not fm1-base's: 29 KB of flash. To go back: remove APP_SYS_TEXT from app.h. */
int pw_sys_h(int face);
int pw_sys_w(int face, const char *s);
void pw_sys_text(int x, int y, int face, const char *s, void (*plot)(int, int, unsigned));
static uint16_t sys_col;
static void sys_plot(int x, int y, unsigned a)
{
    y += cv_oy;
    if ((uint32_t)x >= cv_w || y < cv_y0 || y >= cv_y1) return;
    uint16_t *p = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
    if (a >= 15u) { *p = swap16(sys_col); return; }
    uint32_t bg = swap16(*p), al = (a * 32u + 7u) / 15u;
    uint32_t b = (bg | (bg << 16)) & 0x07E0F81Fu, f = (sys_col | ((uint32_t)sys_col << 16)) & 0x07E0F81Fu;
    uint32_t r = ((f * al + b * (32u - al)) >> 5) & 0x07E0F81Fu;
    *p = swap16(r | (r >> 16));
}
static int32_t app_sys_h(int face) { return pw_sys_h(face); }
static int32_t app_sys_w(int face, const char *s) { return pw_sys_w(face, s); }
static void app_sys_text(int32_t x, int32_t y, int face, const char *s, uint16_t c) { sys_col = c; pw_sys_text(x, y, face, s, sys_plot); }
#endif
#if defined(PLAT_HOST) || defined(PLAT_WEB)
#include <time.h>
uint32_t pw_plat_ticks(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint32_t)((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec); }
uint32_t pw_plat_ticks_per_us(void) { return 1000u; }
unsigned pw_plat_bootprobe(char *buf, unsigned max) { (void)buf; (void)max; return 0; }
int pw_plat_cpu1(uint32_t *count, int start) { (void)count; (void)start; return -1; }
uint32_t pw_plat_cpu1_post(pw_cpu1_fn fn, void *m, const float *const *in, float *const *out, unsigned connected) { (void)fn; (void)m; (void)in; (void)out; (void)connected; return 0; }
int pw_plat_cpu1_wait(uint32_t ticket, uint32_t *us) { (void)ticket; (void)us; return 0; }
void pw_plat_cpu1_stop(void) {}
pw_cpu1_fn pw_plat_cpu1_dummy(void) { return 0; }
static void cpu1_quiesce(void) {}
void pw_plat_cpu1_info(uint32_t *w) { for (int i = 0; i < 6; i++) w[i] = 0; }
void pw_plat_cpu1_jobs(uint32_t *w) { for (int i = 0; i < 7; i++) w[i] = 0; }
#else
uint32_t pw_plat_ticks(void) { return fm1_ticks(); }
/* READ-ONLY boot probe for the interrupted-update work: the first 32 bytes of the app image in flash (app.bin offset 0, read through XIP)
   and a CRC32 for each 4 KB of the first 40 sectors. Nothing is written or erased; comparing two builds' lines shows whether the
   start of the image (the part a boot stub would have to keep fixed) changes from build to build. */
unsigned pw_plat_bootprobe(char *buf, unsigned max)
{
    char *o = buf, *end = buf + max - 24;
    extern char _xip_start[];
    const uint8_t *x = (const uint8_t *)_xip_start;   /* app.bin offset 0 */
    static const char hx[] = "0123456789ABCDEF";
    unsigned k, j;
    if (max < 200u) return 0;
    { const char *t = "BOOTPROBE HEAD "; while (*t) *o++ = *t++; }
    for (k = 0; k < 32u; k++) { *o++ = hx[x[k] >> 4]; *o++ = hx[x[k] & 15u]; }
    *o++ = '\n';
    { const char *t = "BOOTPROBE PRE "; const uint8_t *h = x - 0x120; while (*t) *o++ = *t++;   /* the 0x120 bytes before app.bin offset 0 (the package header) */
      for (k = 0; k < 0x120u; k++) { *o++ = hx[h[k] >> 4]; *o++ = hx[h[k] & 15u]; }
      *o++ = '\n'; }
    for (k = 0; k < 40u && o < end; k++) {
        uint32_t c = ~pw_crc32(~0u, x + k * 4096u, 4096u);
        *o++ = 'S'; if (k >= 10u) *o++ = (char)('0' + k / 10u); *o++ = (char)('0' + k % 10u); *o++ = ' ';
        for (j = 8; j--;) *o++ = hx[(c >> (j * 4)) & 15u];
        *o++ = (k & 3u) == 3u ? '\n' : ' ';
    }
    *o++ = '\n';
    { extern int fm1_ota_imagecheck(uint32_t *early);   /* base/device/main_fm1.c: the boot check's verdict, live */
      uint32_t e = 0, j2; int v = fm1_ota_imagecheck(&e);
      const char *t = "IMAGECHECK "; while (*t) *o++ = *t++;
      *o++ = v < 0 ? 'N' : (char)('0' + v);   /* 1 whole, 0 not whole, N not judged */
      t = " EARLY "; while (*t) *o++ = *t++;
      for (j2 = 8; j2--;) *o++ = hx[(e >> (j2 * 4)) & 15u];
      *o++ = '\n'; }
    *o = 0;
    return (unsigned)(o - buf);
}
uint32_t pw_plat_ticks_per_us(void) { return FM1_TICKS_PER_US; }
#ifdef APP_CPU1
/* The second core, on request from DIAGNOSTICS: started once, then it only counts (polled, no interrupts). Its loop is in RAM (.c1_text) and calls nothing. */
#include "cpu1/cpu1.h"
#else
int pw_plat_cpu1(uint32_t *count, int start) { (void)count; (void)start; return -1; }
void pw_plat_cpu1_info(uint32_t *w) { for (int i = 0; i < 6; i++) w[i] = 0; }
void pw_plat_cpu1_jobs(uint32_t *w) { for (int i = 0; i < 7; i++) w[i] = 0; }
#endif
#endif

/* -------------------------------------------------------------- the panel --- */
/* core/pw_hw.h's button order (the FM-1's own) to plat.h's */
static const uint8_t BTN_MAP[14] = {B_OCTDN, B_OCTUP, B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE,
                                    B_ARP, B_SEQ, B_PLAY, B_REC};
/* encoders: SELECT, ALGORITHM, KNOB 1-4, PRESETS */
static const uint8_t ENC_MAP[7] = {EN_SELECT, EN_ALGO, EN_K1, EN_K2, EN_K3, EN_K4, EN_PRESET};
static uint32_t led_btn, led_key, led_key_dim;

void fm1_controls_read(struct fm1_controls *c)
{
    uint32_t b = plat_buttons(), i;
    c->buttons = 0;
    for (i = 0; i < 14u; i++)
        if (b >> BTN_MAP[i] & 1u)
            c->buttons |= 1u << i;
    touch_poll();                            /* the keys: presses with their velocity, and pressure (base/lib/touch.h) */
    c->keys = touch_keys();
    for (i = 0; i < 27u; i++) { c->vel[i] = touch_vel(i); c->press[i] = (c->keys >> i & 1u) ? touch_pressure(i) : 0; }
    for (i = 0; i < 7u; i++) {
        int32_t e = plat_enc(ENC_MAP[i]);
        c->encoders[i] = (int16_t)(e > 32767 ? 32767 : e < -32767 ? -32767 : e);
    }
    c->knob = (uint16_t)(plat_master() >> 2);
    c->battery = 0;
}

/* the battery, the way fm1-base's plat_battery() does: channel 3, averaged over many readings, three thresholds
 * (raw 531, 561, 591 = about 3.5, 3.7, 3.9 V) with a little hysteresis. The converter is the key scan's while it
 * reads the keys, so the scan lets go of it for a reading, no key held: one reading every 250 ms, 16 of them averaged. */
static unsigned batt_avg;
unsigned fm1_battery_raw(void)
{
#if defined(PLAT_HOST)
    const char *e = getenv("PW_BATT");   /* (the simulator: a reading to look at, PW_BATT=560) */
    return e ? (unsigned)atoi(e) : 0;
#else
    static uint32_t last_ms;
    static uint32_t sum, cnt;
    uint32_t now = plat_ms();
    if (now - last_ms < 250u || touch_keys())
        return batt_avg;
    int was = touch_is_on();
    if (was)
        touch_on(0);
    for (uint32_t t0 = plat_ms(); fm1_an.pend && plat_ms() - t0 < 5u;)
        ;
    int32_t a = fm1_adc_read(FM1_ADC_BATT);
    if (was)
        touch_on(1);
    last_ms = now;
    if (a > 0) {
        if (!batt_avg) { sum = (uint32_t)a * 16u; cnt = 16u; }
        sum += (uint32_t)a; cnt++;
        if (cnt > 16u) { sum -= sum / cnt; cnt--; }      /* a running average over about 16 readings (4 s) */
        batt_avg = (sum + cnt / 2u) / cnt;
    }
    return batt_avg;
#endif
}
unsigned fm1_battery_level(void)
{
#if defined(PLAT_HOST)
    unsigned r = fm1_battery_raw();
    if (getenv("PW_USB")) return 4;
    return r >= 596 ? 3 : r >= 565 ? 2 : r >= 535 ? 1 : 0;
#else
    static const uint16_t at[3] = {531, 561, 591};
    static unsigned lvl;
    unsigned raw = fm1_battery_raw();
    if (plat_usb_powered())
        return 4;
    if (!raw)
        return 0;
    if (lvl > 3) lvl = 0;                                 /* (back from the cable) */
    for (;;) {                                            /* a level moves once the reading is 4 past its threshold */
        if (lvl < 3 && raw >= at[lvl] + 4u) lvl++;
        else if (lvl > 0 && raw + 4u <= at[lvl - 1]) lvl--;
        else break;
    }
    return lvl;
#endif
}

void fm1_led_set(unsigned key, int on)
{
    if (key < 14u) {
        uint32_t bit = 1u << BTN_MAP[key];
        led_btn = on ? led_btn | bit : led_btn & ~bit;
    } else if (key < 41u) {
        uint32_t bit = 1u << (key - 14u);
        led_key = on ? led_key | bit : led_key & ~bit;
    }
}

void patchwerk_led_dim(unsigned key, int on)
{
    if (key >= 14u && key < 41u) {
        uint32_t bit = 1u << (key - 14u);
        led_key_dim = on ? led_key_dim | bit : led_key_dim & ~bit;
    }
}

uint32_t fm1_millis(void) { return plat_ms(); }

/* The panel takes RGB565 high byte first (gfx.c); the core draws native 16-row bands. lcd_blit leaves its
 * DMA running and returns (device/lcd.c): its source must not change until the next LCD access has waited for
 * it. So two buffers in turn: one is filled while the other may still be going out. lcd_blit's own window
 * setup waits for the earlier transfer, so a buffer is always free again before it is refilled. */
static uint16_t band_swapped[2][240u * 16u] PLAT_POOL;   /* (the pool: 15 KB less static RAM) */
static uint32_t band_next;
int fm1_screen_blit(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *px, unsigned stride)
{
    uint32_t r, i;
    uint16_t *dst;
    if (!px || x + w > 240u || y + h > 240u || stride < w || (uint32_t)w * h > 240u * 16u)
        return -1;
    dst = band_swapped[band_next];
    band_next ^= 1u;
    for (r = 0; r < h; r++)
        for (i = 0; i < w; i++)
            dst[r * w + i] = (uint16_t)((px[r * stride + i] >> 8) | (px[r * stride + i] << 8));
    lcd_blit(x, y, w, h, dst);
    return 0;
}

static uint32_t master_last = 0xFFFFFFFFu;
unsigned pw_plat_master(unsigned adc, int *changed)
{
    /* the knob's reading flickers by a count or two when only brushed: the level and its popup move only once the
     * knob has really travelled (3 %), and an end stop is reached exactly */
    uint32_t raw = (plat_master() * 100u + 2048u) / 4096u, level = master_last;
    (void)adc;
    *changed = 0;
    if (master_last == 0xFFFFFFFFu) { master_last = raw; return raw; }
    uint32_t d = raw > master_last ? raw - master_last : master_last - raw;
    if (d >= 3u || (d && (raw == 0u || raw >= 100u))) { *changed = 1; master_last = raw; level = raw; }
    return level;
}

/* ------------------------------------------------------------------- MIDI --- */
/* MIDI OUT modules (from the render): USB-MIDI packets, code index from the status */
void pw_plat_midi_out(uint8_t st, uint8_t d1, uint8_t d2)
{
    uint32_t cin = st >= 0xF8u ? 0xFu : (uint32_t)(st >> 4);
    plat_midi_send(cin | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24, TO_USB);
}
/* USB, TRS and (if built) Bluetooth in: notes, pitch bend, the mod wheel, all notes off, to the KEYS
 * module. Main loop only, so the engine's key queue keeps one writer. */
static void midi_poll(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint8_t st = (uint8_t)(pkt >> 8), d1 = (uint8_t)(pkt >> 16 & 0x7Fu), d2 = (uint8_t)(pkt >> 24 & 0x7Fu);
        if ((st >= 0x80u && st < 0xF0u) || st >= 0xF8u)   /* (and the clock, start, stop, continue) */
            pw_midi(st, d1, d2);
    }
}

/* ------------------------------------------------------------- the store --- */
/* DUAL CORE and the flash: with modules on CPU1 (which runs their code from the flash) a save restarted the FM-1 (rc107: reported on a real unit, fine with DUAL CORE
 * off). Before any flash write the offload is switched off and given a few blocks to bring every module home (the engine's off_check does it at each block's start);
 * it is switched back on a moment after the last write. */
static uint8_t quiet_was;
static uint32_t quiet_until;
static void flash_quiet(void)
{
    if (pw_cpu1_offload) {
        quiet_was = pw_cpu1_offload; pw_cpu1_offload = 0;
        for (uint32_t t0 = plat_ms(); plat_ms() - t0 < 30u;) ;   /* (the sound goes on: the audio interrupt renders those blocks) */
    }
    quiet_until = plat_ms() + 1500u;
}
static void flash_quiet_tick(void)
{
    if (quiet_was && (int32_t)(plat_ms() - quiet_until) > 0) {
        if (!pw_cpu1_offload && pw_off_state != 3) pw_cpu1_offload = quiet_was;
        quiet_was = 0;
    }
}
/* eight user patches, each a store object (A/B sectors, CRC, torn-write safe) */
static char names[PW_USER_SLOTS][13];
static uint8_t valid[PW_USER_SLOTS], kind[PW_USER_SLOTS];
static uint8_t patch_buf[PLAT_STORE_MAX] PW_POOL;   /* (in the pool: the RAM is short) */

static void store_refresh(unsigned slot)
{
    uint8_t head[24];
    unsigned n = 12, i;
    valid[slot] = 0;
    names[slot][0] = 0;
    if (plat_store_head(OBJ_PATCH0 + slot, head, sizeof head) != (int)sizeof head || memcmp(head, "NDL1", 4))
        return;
    valid[slot] = 1;
    kind[slot] = head[9] >> 7;   /* (the bpm's top bit: saved as a GROUP) */
    while (n && head[10 + n - 1] == ' ')
        n--;
    for (i = 0; i < n; i++)
        names[slot][i] = (head[10 + i] >= 32 && head[10 + i] < 127) ? (char)head[10 + i] : '?';
    names[slot][n] = 0;
    if (!n)
        memcpy(names[slot], "UNTITLED", 9);
}
void pw_store_init(void)
{
    unsigned i;
    for (i = 0; i < PW_USER_SLOTS; i++)
        store_refresh(i);
}
const char *pw_store_name(unsigned slot) { return slot < PW_USER_SLOTS && valid[slot] ? names[slot] : 0; }
int pw_store_save(unsigned slot)
{
    int n, r;
    if (slot >= PW_USER_SLOTS)
        return -1;
    n = pw_save(patch_buf, sizeof patch_buf);
    if (n <= 0)
        return -2;
    flash_quiet();
    r = plat_store_save(OBJ_PATCH0 + slot, patch_buf, (uint32_t)n);   /* stops the sound for a moment */
    store_refresh(slot);
    if (r == 0) pw_ram_save(slot);   /* (the SAMPLERs' recorded sounds, in the bank's own slots) */
    return r == 0 ? 0 : -3;
}
int pw_store_erase(unsigned slot)
{
    int r;
    if (slot >= PW_USER_SLOTS)
        return -1;
    flash_quiet();
    r = plat_store_save(OBJ_PATCH0 + slot, "", 0);
    store_refresh(slot);
    pw_ram_erase(slot);
    return r;
}
int pw_settings_load(void *dst, unsigned n) { return plat_store_load(OBJ_SETTINGS, dst, n); }
int pw_settings_save(const void *src, unsigned n) { flash_quiet(); return plat_store_save(OBJ_SETTINGS, src, n); }
unsigned pw_flash_free_kb(void) { return pw_bank_free() / 1024u; }
/* the settings page's budget: the image in the flash (code, constants and the start values of the variables), static RAM and the pool,
 * each with its room (base/device/app.ld). On the host and the web page: the numbers of a build, for the screenshots. */
#if defined(PLAT_HOST) || defined(PLAT_WEB)
void pw_plat_budget(uint32_t v[6]) { v[0] = 562068u; v[1] = 0x8DFBCu; v[2] = 97264u; v[3] = 98304u; v[4] = 288288u; v[5] = 344064u; }
#else
extern uint32_t _data_start[], _data_end[], _bss_end[], _pool_start[], _pool_end[], _rt_start[], _rt_end[];
extern char _etext[], _xip_start[];
void pw_plat_budget(uint32_t v[6])
{
    v[0] = (uint32_t)((uintptr_t)_etext - (uintptr_t)_xip_start) + (uint32_t)((uintptr_t)_rt_end - (uintptr_t)_rt_start) + (uint32_t)((uintptr_t)_data_end - (uintptr_t)_data_start);
    v[1] = 0x8DFBCu;
    v[2] = (uint32_t)((uintptr_t)_bss_end - (uintptr_t)_data_start); v[3] = 96u * 1024u;
    v[4] = (uint32_t)((uintptr_t)_pool_end - (uintptr_t)_pool_start); v[5] = 0x54000u;
}
#endif
int pw_store_kind(unsigned slot) { return slot < PW_USER_SLOTS && valid[slot] ? kind[slot] : 0; }
int pw_store_insert(unsigned slot, uint64_t *set)
{
    int n;
    *set = 0;
    if (slot >= PW_USER_SLOTS || !valid[slot])
        return -1;
    n = plat_store_load(OBJ_PATCH0 + slot, patch_buf, sizeof patch_buf);
    if (n <= 0)
        return -2;
    return pw_insert(patch_buf, (unsigned)n, set);
}
int pw_store_load(unsigned slot)
{
    int n;
    if (slot >= PW_USER_SLOTS || !valid[slot])
        return -1;
    n = plat_store_load(OBJ_PATCH0 + slot, patch_buf, sizeof patch_buf);
    if (n <= 0)
        return -2;
    n = pw_load(patch_buf, (unsigned)n);
    if (n >= 0) pw_ram_load(slot);
    return n;
}

/* -------------------------------------------------------- the live link --- */
/* the web page's patch bay and this patch kept the same (core/pw_link.c), over USB-MIDI SysEx */
void pw_plat_link_send(const uint8_t *frame, unsigned n) { plat_sysex_send(frame, n); }
int app_sysex(const uint8_t *p, uint32_t n) { return pw_link_rx(p, n, plat_ms()); }

/* -------------------------------------------------------- the sample bank --- */
/* core/pw_bank.c: sounds in the data area, sent from the web page (APP_USB_CMD, cmd 0x60) */
const uint8_t *pw_plat_flash(uint32_t off) { return plat_data(off); }
int pw_plat_flash_erase(uint32_t off) { flash_quiet(); return plat_data_erase(off); }
int pw_plat_flash_write(uint32_t off, const void *src, uint32_t n) { flash_quiet(); return plat_data_write(off, src, n); }
int pw_plat_host_read(uint32_t addr, uint8_t *dst, uint32_t n) { return plat_host_read(addr, dst, n); }
static char bank_what[24];
void pw_plat_progress(const char *what, unsigned percent) { pw_ui_progress(what, bank_what, percent); }

#define BANK_CMD 0x60u
enum { BANK_PUT = 1, BANK_DELETE = 2, BANK_ERASE_ALL = 3, BANK_LIST = 4,
       SLOT_GET = 5, SLOT_PUT = 6, BANK_GET = 8, DIAG_GET = 9, BANK_ERASE_KIND = 10,
       PAL_PUT = 11, PAL_GET = 12, PAL_CLEAR = 13, SET_GET = 14, SET_PUT = 15 };   /* (the USER palette: 18 colors as u16, little-endian, after the command (an older page's 33 are read too); PAL_GET answers: set, then the 18) */   /* (the page's backup: the user patches and the bank's sounds) */
#define CHUNK 248u                                   /* the most a backup answer carries (its length: one byte) */
/* an answer in the update's framing: F0 pack7(00 59 cmd len24 body ~sum) F7 */
static void usb_answer(const uint8_t *body, uint32_t n)
{
    static uint8_t raw[8 + 16 + 24u * 20u], wire[2 + (sizeof raw * 8u + 6u) / 7u];
    uint32_t i, s = 0, acc = 0, nb = 0, o = 1;
    raw[0] = 0; raw[1] = 0x59; raw[2] = BANK_CMD; raw[3] = (uint8_t)n; raw[4] = (uint8_t)(n >> 8); raw[5] = 0;
    for (i = 0; i < n; i++) { raw[6 + i] = body[i]; s += body[i]; }
    raw[6 + n] = (uint8_t)~s;
    wire[0] = 0xF0;
    for (i = 0; i < 7u + n; i++) {
        acc |= (uint32_t)raw[i] << nb;
        nb += 8u;
        while (nb >= 7u) { wire[o++] = (uint8_t)(acc & 0x7Fu); acc >>= 7; nb -= 7u; }
    }
    if (nb) wire[o++] = (uint8_t)(acc & 0x7Fu);
    wire[o++] = 0xF7;
    plat_sysex_send(wire, o);
}
static uint32_t rd32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
/* d: the decoded frame (cmd d[2], body from d + 6) */
void app_usb_cmd(const uint8_t *d, uint32_t n)
{
    uint8_t ans[4 + 4 + 8 + 24u * 20u];
    _Static_assert(8u + CHUNK <= 4u + 4u + 24u * 20u, "a backup answer fits");
    uint32_t len = n >= 7u ? (uint32_t)(d[3] | d[4] << 8) : 0, i;
    const uint8_t *b = d + 6;
    if (n < 8u || d[2] != BANK_CMD || len + 7u > n || !len)
        return;
    ans[0] = b[0]; ans[1] = PW_BANK_E_ARG;
    if (b[0] == BANK_PUT && len >= 26u) {         /* slot, frames u32, rate u32, crc u32, name[12], codec (1: SEA; not sent: samples) */
        char name[13];
        uint32_t frames = rd32(b + 2), rate = rd32(b + 6), crc = rd32(b + 10);
        for (i = 0; i < 12u; i++)
            name[i] = (char)(b[14 + i] >= 32 && b[14 + i] < 127 ? b[14 + i] : 0);
        name[12] = 0;
        bank_what[0] = 'B';
        i = 1;
        if (b[1] >= 9u) bank_what[i++] = (char)('0' + (b[1] + 1) / 10);
        bank_what[i++] = (char)('0' + (b[1] + 1) % 10);
        bank_what[i++] = ' ';
        memcpy(bank_what + i, name, 13);
        PW_TRACE(0x0F0);
        pw_ui_progress("SAMPLE FROM USB", bank_what, 0);
        PW_TRACE(0x0F1);
        plat_host_patience(2000u, 120000u);
        ans[1] = (uint8_t)pw_bank_receive(b[1], frames, rate, name, crc, len >= 27u ? b[26] : 0u);
        PW_TRACE(0x0F2);
        pw_ui_redraw_all();
        PW_TRACE(0x0F3);
        pw_ui_notice(ans[1] ? "SAMPLE NOT SAVED" : "SAMPLE SAVED IN THE BANK");
        PW_TRACE(0x0F4);
        usb_answer(ans, 2);
        PW_TRACE(0x0F5);
        return;
    }
    if (b[0] == SLOT_GET && len >= 4u) {           /* slot, offset u16: total u16, offset u16, n, n bytes of the patch */
        unsigned slot = b[1], off = b[2] | (unsigned)b[3] << 8, k;
        int n = slot < PW_USER_SLOTS && valid[slot] ? plat_store_load(OBJ_PATCH0 + slot, patch_buf, sizeof patch_buf) : -1;
        ans[2] = (uint8_t)slot;
        if (n <= 0) { ans[1] = PW_BANK_E_ARG; usb_answer(ans, 3); return; }   /* empty */
        k = off < (unsigned)n ? (unsigned)n - off : 0u;
        if (k > CHUNK) k = CHUNK;
        ans[1] = 0; ans[3] = (uint8_t)n; ans[4] = (uint8_t)(n >> 8); ans[5] = (uint8_t)off; ans[6] = (uint8_t)(off >> 8); ans[7] = (uint8_t)k;
        memcpy(ans + 8, patch_buf + off, k);
        usb_answer(ans, 8u + k);
        return;
    }
    if (b[0] == DIAG_GET && len >= 3u) {           /* offset u16: total u16, offset u16, n, n bytes of the diagnostics text (made when the offset is 0) */
        static char diag_txt[2560] PW_POOL;   /* (in the pool: the RAM is short) */
        static unsigned diag_n;
        unsigned off = b[1] | (unsigned)b[2] << 8, k;
        if (!off) { diag_n = pw_diag_report(diag_txt, sizeof diag_txt); diag_n += pw_plat_bootprobe(diag_txt + diag_n, sizeof diag_txt - diag_n); }
        k = off < diag_n ? diag_n - off : 0u;
        if (k > CHUNK) k = CHUNK;
        ans[1] = 0; ans[2] = 0; ans[3] = (uint8_t)diag_n; ans[4] = (uint8_t)(diag_n >> 8); ans[5] = (uint8_t)off; ans[6] = (uint8_t)(off >> 8); ans[7] = (uint8_t)k;
        memcpy(ans + 8, diag_txt + off, k);
        usb_answer(ans, 8u + k);
        return;
    }
    if (b[0] == SLOT_PUT && len >= 8u) {           /* slot, length u16, crc u32: the patch pulled from the host, stored */
        unsigned slot = b[1], total = b[2] | (unsigned)b[3] << 8, off;
        uint32_t crc = rd32(b + 4);
        ans[1] = PW_BANK_E_ARG;
        if (slot < PW_USER_SLOTS && total >= 28u && total <= sizeof patch_buf) {
            plat_host_patience(2000u, 20000u);
            ans[1] = 0;
            for (off = 0; off < total && !ans[1]; off += 512u)
                if (plat_host_read(off, patch_buf + off, total - off > 512u ? 512u : total - off)) ans[1] = PW_BANK_E_HOST;
            if (!ans[1] && (~pw_crc32(~0u, patch_buf, total) != crc || memcmp(patch_buf, "NDL1", 4))) ans[1] = PW_BANK_E_CRC;
            if (!ans[1]) { flash_quiet(); if (plat_store_save(OBJ_PATCH0 + slot, patch_buf, total)) ans[1] = PW_BANK_E_FLASH; }
            store_refresh(slot);
            if (!ans[1]) pw_ui_notice("PATCH RESTORED FROM USB");
        }
        usb_answer(ans, 2);
        return;
    }
    if (b[0] == PAL_PUT && len >= 37u) {           /* the USER palette: 18 colors (an older page sends 33: the old order, read as such) */
        uint16_t v[33], w[18];
        unsigned n = len >= 67u ? 33u : 18u;
        for (i = 0; i < n; i++) v[i] = (uint16_t)(b[1 + 2 * i] | b[2 + 2 * i] << 8);
        if (n == 33u) pw_ui_pal_from_legacy(v, w); else memcpy(w, v, sizeof w);
        ans[1] = (uint8_t)pw_ui_user_palette(w);
        pw_ui_notice("USER PALETTE RECEIVED");
        usb_answer(ans, 2);
        return;
    }
    if (b[0] == SET_PUT && len >= 3u) {            /* the settings of a backup: n, then n bytes */
        ans[1] = (uint8_t)(b[1] + 2u <= len ? pw_ui_settings_put(b + 2, b[1]) : -1);
        if (!ans[1]) pw_ui_notice("SETTINGS RESTORED FROM USB");
        usb_answer(ans, 2);
        return;
    }
    if (b[0] == SET_GET) {                         /* answers: 0, n, the n bytes */
        uint8_t blob[128];
        int n = pw_ui_settings_get(blob, (int)sizeof blob);
        ans[1] = 0; ans[2] = (uint8_t)n;
        memcpy(ans + 3, blob, (size_t)n);
        usb_answer(ans, 3u + (unsigned)n);
        return;
    }
    if (b[0] == PAL_CLEAR) { ans[1] = (uint8_t)pw_ui_user_palette(0); usb_answer(ans, 2); return; }
    if (b[0] == PAL_GET) {
        uint16_t v[18];
        ans[1] = 0; ans[2] = (uint8_t)pw_ui_user_palette_get(v);
        for (i = 0; i < 18u; i++) { ans[3 + 2 * i] = (uint8_t)v[i]; ans[4 + 2 * i] = (uint8_t)(v[i] >> 8); }
        if (!ans[2]) memset(ans + 3, 0, 36);
        usb_answer(ans, 39);
        return;
    }
    if (b[0] == BANK_GET && len >= 6u) {           /* slot, offset u32: offset u32, n, n bytes of the sound (16-bit) */
        uint32_t frames = 0, off = rd32(b + 2), bytes, k;
        unsigned codec = 0;
        const uint8_t *d = 0;
        bytes = pw_bank_stored(b[1], &frames, &codec, &d);      /* (as stored: a compressed sound goes out compressed, the listing says which) */
        (void)codec;
        if (!bytes) d = 0;
        ans[2] = b[1];
        if (!d || off > bytes) { ans[1] = PW_BANK_E_ARG; usb_answer(ans, 3); return; }
        k = bytes - off > CHUNK ? CHUNK : bytes - off;
        ans[1] = 0; ans[3] = (uint8_t)off; ans[4] = (uint8_t)(off >> 8); ans[5] = (uint8_t)(off >> 16); ans[6] = (uint8_t)(off >> 24);
        ans[7] = (uint8_t)k;
        memcpy(ans + 8, d + off, k);
        usb_answer(ans, 8u + k);
        return;
    }
    if (b[0] == BANK_DELETE && len >= 2u) ans[1] = (uint8_t)pw_bank_delete(b[1]);
    else if (b[0] == BANK_ERASE_ALL) ans[1] = (uint8_t)pw_bank_erase_all();
    else if (b[0] == BANK_ERASE_KIND && len >= 2u) ans[1] = (uint8_t)pw_bank_erase_kind(b[1]);   /* (1 the sounds, 2 the CZ banks, 3 the FM-DX banks: the page's three erase buttons) */
    if (b[0] == BANK_LIST) {                       /* the free room, then each slot: frames, rate, name */
        uint32_t f = pw_bank_free(), first = len >= 2u ? b[1] : 0u, cnt = first < PW_BANK_SLOTS ? PW_BANK_SLOTS - first : 0u;
        if (cnt > 24u) cnt = 24u;      /* (24 entries an answer: [4] lists slots 0-23, [4, 24] the rest) */
        ans[1] = 0; ans[2] = (uint8_t)cnt; ans[3] = (uint8_t)first;
        ans[4] = (uint8_t)f; ans[5] = (uint8_t)(f >> 8); ans[6] = (uint8_t)(f >> 16); ans[7] = (uint8_t)(f >> 24);
        for (i = 0; i < cnt; i++)
            pw_bank_entry(first + i, ans + 8 + i * 20u);
        {   /* after the entries: the room in all the windows (u32) and what is free of it (u32); the page asks for older firmware's 208 KB */
            uint32_t room = PW_BANK_ROOM, ft = pw_bank_free_total(), at = 8u + cnt * 20u;
            ans[at] = (uint8_t)room; ans[at + 1] = (uint8_t)(room >> 8); ans[at + 2] = (uint8_t)(room >> 16); ans[at + 3] = (uint8_t)(room >> 24);
            ans[at + 4] = (uint8_t)ft; ans[at + 5] = (uint8_t)(ft >> 8); ans[at + 6] = (uint8_t)(ft >> 16); ans[at + 7] = (uint8_t)(ft >> 24);
        }
        usb_answer(ans, 16u + cnt * 20u);
        return;
    }
    usb_answer(ans, 2);
}

/* ------------------------------------------------------------ the libc gap --- */
/* felucca-bsp's libc.c has memset, memcpy and memcmp; the core also needs these two */
#ifndef PLAT_HOST
void *memmove(void *d, const void *s, unsigned n)
{
    unsigned char *a = (unsigned char *)d;
    const unsigned char *b = (const unsigned char *)s;
    if (a < b)
        while (n--)
            *a++ = *b++;
    else {
        a += n;
        b += n;
        while (n--)
            *--a = *--b;
    }
    return d;
}
unsigned strlen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}
#endif

/* ------------------------------------------------------------ the contract --- */
void app_init(void)
{
    pw_init();                               /* the engine and its tables */
    pw_bank_init();                          /* the sample bank's directory */
    pw_link_init(PW_LINK_DEVICE);
    ready = 1;
}

void app_ui_init(void)
{
    static uint8_t first = 1;                /* user patch 1 if saved, else the first factory patch */
    if (first) { first = 0;
#if defined(PLAT_HOST)
        pw_ui_init_noboot();
#else
        pw_ui_init();
#endif
    }  /* power-on: the boot page animates the splash's cables in */
    else pw_ui_init_noboot();                /* (back from an update session: straight to the rack) */
    touch_on(1);                             /* velocity and pressure: the keys' readings on */
}

void app_input_only(void) { midi_poll(); }

void app_frame(void)
{
    midi_poll();
    flash_quiet_tick();
    if (overloaded) {
        static uint32_t last_cut;
        overloaded = 0;
        if (pw_ui_rate_drop()) { last_cut = plat_ms(); pw_ui_notice("CPU: DROPPED TO 22.05 kHz"); }   /* (the music goes on, duller) */
        else if (pws.voices > 1 && plat_ms() - last_cut > 500u) {   /* polyphony first: a voice fewer */
            char msg[32] = "CPU: VOICES DOWN TO ";
            last_cut = plat_ms();
            pw_set_voices(pws.voices - 1u);
            msg[20] = (char)('0' + pws.voices); msg[21] = 0;
            pw_ui_notice(msg);
        } else if (pws.voices <= 1) {   /* (said once in a while, so the shortcut strip stays readable) */
            static uint32_t last_say;
            if (plat_ms() - last_say > 6000u) { last_say = plat_ms(); pw_ui_notice("CPU OVERLOAD - FEWER MODULES"); }
        }
    }
    pw_ui_control();
    plat_present();                          /* (the mirror: this frame is whole) */
    pw_link_poll(plat_ms());                 /* a web page linked: what changed here goes to it */
    plat_glow(1);                            /* the unlit buttons glow, readable in the dark */
    plat_key_glow(led_key_dim);
    plat_leds(led_btn, led_key);
}

void app_say(const char *a, const char *b)
{
    char t[40];
    unsigned i = 0, j;
    for (j = 0; a && a[j] && i < 38u; j++)
        t[i++] = a[j];
    if (b && b[0] && i < 38u)
        t[i++] = ' ';
    for (j = 0; b && b[j] && i < 39u; j++)
        t[i++] = b[j];
    t[i] = 0;
    pw_ui_notice(t);
    if (ann_on())
        ann_say(t, 0, ANN_NOW);
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "modules"))
        *got = pw_count();
    else if (!strcmp(what, "cables"))
        *got = pw_cable_count();
    else if (!strcmp(what, "playing"))
        *got = pws.playing;
    else if (!strcmp(what, "peak"))          /* the output's recent peak, 0..1000 */
        *got = (int)(pws.out_peak_l * 1000.0f);
    else if (!strcmp(what, "page"))          /* 1 patch, 2 module, 3 add, 4 cables, 5 scope, 6 seq, 7 patches, 8 help, 9 perform */
        *got = pw_ui_page();
    else if (!strcmp(what, "param0"))        /* the cursor module's first parameter, x 1000 */
        *got = pw_ui_slot() >= 0 ? (int)(pws.mod[pw_ui_slot()].p[0] * 1000.0f) : -1;
    else if (!strcmp(what, "gview"))         /* the group opened on the patch screen (0: the top) */
        *got = pw_ui_gview();
    else if (!strcmp(what, "carrying"))      /* a cable in hand on the patch screen */
        *got = pw_ui_carrying();
    else if (!strcmp(what, "addcur"))        /* the long add list's cursor */
        *got = pw_ui_addcur();
    else if (!strcmp(what, "type"))          /* the cursor module's type */
        *got = pw_ui_slot() >= 0 ? pws.mod[pw_ui_slot()].type : -1;
    else if (!strcmp(what, "var"))           /* the variation playing, 0..7 */
        *got = pws.var_cur;
    else if (!strcmp(what, "kbvel"))         /* the last key's velocity, 0..127 */
        *got = (int)(pws.kb_vel * 127.0f + 0.5f);
    else if (!strcmp(what, "kbpress"))       /* the keys' pressure, 0..127 */
        *got = (int)(pws.kb_press * 127.0f + 0.5f);
    else if (!strcmp(what, "clip"))          /* CLIP PROTECT is working (OUTPUT's setting), 0 or 1 */
        *got = pws.clip_on;
    else if (!strcmp(what, "clipgr"))        /* the limiter's gain reduction, in tenths of a dB (0 or below) */
        *got = pw_clip_gr_tenths();
    else if (!strcmp(what, "voices"))
        *got = pws.voices;
    else if (!strcmp(what, "perf")) {        /* knobs on the performance page */
        int n = 0;
        for (unsigned k = 0; k < PW_PERF; k++)
            n += pws.perf[k][0] != 0xff;
        *got = n;
    }
    else if (!strcmp(what, "locks")) {       /* knobs that follow a MOD SEQ lane (ARP + knob) */
        int n = 0;
        for (unsigned i = 0; i < PW_MAX_MODULES; i++)
            for (unsigned l = 0; l < 4; l++) { unsigned a, b; n += pws.mod[i].used == 1 && pw_mseq_lock_of(&pws.mod[i], l, &a, &b); }
        *got = n;
    }
    else if (!strcmp(what, "bankfree"))      /* the largest free gap in the bank's flash, in bytes */
        *got = (int)pw_bank_free();
    else if (!strcmp(what, "smp0")) {        /* A-1's loudest frame among 256 from frame 1000 (a compressed sound is decoded to read it) */
        pw_smp s; int16_t b[256]; int mx = 0;
        if (pw_smp_open(16, &s)) { pw_smp_read(&s, 1000, 256, b); for (int i = 0; i < 256; i++) { int v = b[i] < 0 ? -b[i] : b[i]; if (v > mx) mx = v; } }
        *got = mx;
    }
    else if (!strcmp(what, "bank")) {        /* sounds in the sample bank */
        int n = 0;
        pw_smp sm;
        for (unsigned i = 16; i < PW_SOUNDS; i++)     /* (A and B: INTERNAL is always there) */
            n += pw_smp_open(i, &sm) != 0;
        *got = n;
    }
    else
        return 0;
    return 1;
}
#endif
