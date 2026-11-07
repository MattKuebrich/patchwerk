/* SPDX-License-Identifier: GPL-3.0-only
 * PATCHWERK panel UI for the FM-1: 240x240 LCD, 7 encoders, 14 buttons and
 * 27 keys. Runs in the control task. Frames are composed as a display list
 * and rasterized in 16-row bands; only the 16x16 tiles that changed are sent.
 *
 * Panel roles (printed labels):
 *   HOME  the patch: every module and cable    EDIT  the module in detail
 *   FX    add a module                         SEL   grab / plug a cable, add, load: "do it"
 *   GLO+ALGORITHM  the knobs' pages           LFO   every cable in a list
 *   ARP   patches (load, store, tempo, name)   SEQ   step sequencer
 *   PLAY  transport                            REC   unplug / delete / step record
 *   SAVE  store the patch                      GLO   held: fine knobs; tapped: the shortcuts
 *   SELECT jack to jack (turned fast or GLO + SELECT: module to module), ALGORITHM the knobs' parameter page,
 *   KNOB 1-4 parameters, PRESETS the patch list */
#include "pw_ui.h"
#include "pw_engine.h"
#include "pw_presets.h"
#include "pw_store.h"
#include "pw_fonts.h"
#include "pw_hw.h"
#include "pw_dsp.h"
#include <string.h>

#define W 240
#define H 240
#define RC5(v) ((v) > 251 ? 255 : (v) + 4)   /* (rounded to the nearest of the panel's 32 / 64 levels, not cut down: a palette's hex colors come out as near as the screen can show them) */
#define RC6(v) ((v) > 253 ? 255 : (v) + 2)
#define RGB(r, g, b) (uint16_t)(((RC5(r) & 0xf8) << 8) | ((RC6(g) & 0xfc) << 3) | (RC5(b) >> 3))
#include "pw_brand.h"   /* the brand colors (brand/brand.json): the RISO row below is made of them */
#define RGBV(...) RGB(__VA_ARGS__)
/* The palette: every color the screens use comes from `pal`, one of `pals` (Settings: PALETTE) */
typedef struct { uint16_t black, panel, raised, line, dim, text, green, red, amber, mod, white; uint16_t cat[7]; } pw_pal;   /* (four greys: every ground, well, panel, strip, plate and line of the screens is one of them) */
#define PAL_COUNT 12
#define PAL_USER_ID 15u      /* the USER palette's number in saved settings (the built-in ones have theirs in pal_id) */
#define PW_PAL_DEFAULT 2   /* (RISO's row; the settings save a palette by its pal_id, so removing a row does not change what a saved choice means)  (the browser shows the same colors as the unit, and lifts the darks the way the unit's panel does: page.html drawLCD) */
#define PW_TONE_DEFAULT 1
static const char *const pal_name[PAL_COUNT] = {"HARVEST", "MONO", BR_NAME, "BONE", "GRAPHITE", "SOFT", "CIRCUIT", "MULBERRY", "NANO", "HAZE", "MID", "BALANCE"};
/* a palette's number in saved settings stays what it was when the list was longer (DEFAULT 0, CMYK 1, HARVEST 2, MONO 3, RISO 4, BONE 5, MIDCENTURY 6, GRAPHITE 7, SOFT 8, CIRCUIT 9, MULBERRY 10, NANO 11, HAZE 12, MID 13, BALANCE 14): a removed one comes back as RISO */
static const uint8_t pal_id[PAL_COUNT] = {2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14};
static const pw_pal pals[PAL_COUNT] = {
    {RGB(0, 0, 0), RGB(16, 16, 19), RGB(30, 31, 36), RGB(62, 63, 74), RGB(180, 172, 140), RGB(248, 237, 217), RGB(110, 190, 80), RGB(228, 84, 60), RGB(248, 237, 217), RGB(86, 160, 110), RGB(255, 255, 255), {RGB(240, 160, 60), RGB(70, 185, 160), RGB(225, 130, 150), RGB(110, 190, 80), RGB(240, 200, 80), RGB(170, 130, 190), RGB(228, 84, 60)}},   /* HARVEST */
    {RGB(0, 0, 0), RGB(17, 17, 17), RGB(32, 32, 32), RGB(67, 67, 67), RGB(170, 170, 170), RGB(248, 248, 248), RGB(225, 225, 225), RGB(240, 80, 80), RGB(255, 255, 255), RGB(190, 190, 190), RGB(255, 255, 255), {RGB(255, 255, 255), RGB(200, 200, 200), RGB(235, 235, 235), RGB(170, 170, 170), RGB(250, 250, 250), RGB(215, 215, 215), RGB(185, 185, 185)}},   /* MONO */
    {RGB(0, 0, 0), RGB(16, 16, 19), RGB(30, 31, 36), RGB(62, 63, 74), RGB(170, 176, 150), RGB(242, 244, 232), RGBV(BR_OK), RGBV(BR_BAD), RGBV(BR_ACCENT), RGBV(BR_FILTER), RGB(255, 255, 255), {RGBV(BR_SOURCE), RGBV(BR_FILTER), RGBV(BR_FX), RGBV(BR_MOD), RGBV(BR_UTIL), RGBV(BR_SEQ), RGBV(BR_IO)}},   /* RISO */
    {RGB(226, 220, 206), RGB(248, 245, 237), RGB(238, 233, 222), RGB(132, 125, 110), RGB(84, 79, 70), RGB(22, 20, 17), RGB(10, 96, 36), RGB(170, 16, 16), RGB(196, 52, 24), RGB(30, 60, 160), RGB(22, 20, 17), {RGB(170, 58, 0), RGB(0, 96, 160), RGB(150, 20, 115), RGB(24, 100, 24), RGB(112, 84, 0), RGB(76, 40, 180), RGB(172, 16, 16)}},   /* BONE */
    {RGB(0, 0, 0), RGB(16, 16, 16), RGB(30, 30, 30), RGB(62, 62, 62), RGB(150, 150, 150), RGB(236, 236, 236), RGB(214, 214, 214), RGB(222, 152, 112), RGB(222, 152, 112), RGB(236, 236, 236), RGB(255, 255, 255), {RGB(236, 236, 236), RGB(206, 206, 206), RGB(178, 178, 178), RGB(152, 152, 152), RGB(128, 128, 128), RGB(104, 104, 104), RGB(222, 152, 112)}},   /* GRAPHITE */
    {RGB(0, 0, 0), RGB(16, 16, 19), RGB(30, 31, 36), RGB(62, 63, 74), RGB(170, 176, 150), RGB(242, 244, 232), RGB(121, 191, 165), RGB(232, 93, 74), RGB(242, 184, 75), RGB(76, 144, 184), RGB(255, 255, 255), {RGB(232, 93, 74), RGB(76, 144, 184), RGB(231, 154, 193), RGB(121, 191, 165), RGB(216, 230, 91), RGB(120, 102, 182), RGB(166, 78, 101)}},   /* SOFT */
    {RGB(0, 0, 0), RGB(14, 21, 31), RGB(25, 38, 56), RGB(58, 77, 103), RGB(170, 176, 150), RGB(242, 244, 232), RGB(56, 184, 165), RGB(217, 54, 100), RGB(180, 227, 90), RGB(121, 197, 232), RGB(255, 255, 255), {RGB(217, 54, 100), RGB(121, 197, 232), RGB(240, 107, 171), RGB(56, 184, 165), RGB(250, 253, 252), RGB(196, 155, 232), RGB(77, 103, 213)}},   /* CIRCUIT */
    {RGB(0, 0, 0), RGB(21, 22, 26), RGB(40, 42, 49), RGB(79, 80, 85), RGB(186, 184, 180), RGB(235, 231, 223), RGB(152, 190, 167), RGB(217, 108, 136), RGB(212, 222, 140), RGB(120, 169, 184), RGB(255, 255, 255), {RGB(217, 108, 136), RGB(120, 169, 184), RGB(180, 160, 219), RGB(152, 190, 167), RGB(212, 222, 140), RGB(84, 91, 156), RGB(156, 53, 93)}},   /* MULBERRY */
    {RGB(0, 0, 0), RGB(34, 34, 34), RGB(63, 63, 63), RGB(117, 117, 117), RGB(255, 255, 255), RGB(230, 230, 230), RGB(8, 152, 70), RGB(227, 68, 11), RGB(238, 186, 18), RGB(9, 151, 182), RGB(230, 230, 230), {RGB(93, 41, 234), RGB(7, 150, 65), RGB(187, 31, 152), RGB(11, 128, 214), RGB(8, 156, 167), RGB(234, 99, 109), RGB(115, 153, 16)}},   /* NANO */
    {RGB(0, 0, 15), RGB(51, 20, 63), RGB(82, 37, 100), RGB(211, 109, 173), RGB(133, 72, 157), RGB(255, 255, 255), RGB(0, 225, 123), RGB(255, 255, 255), RGB(228, 228, 229), RGB(22, 134, 255), RGB(255, 255, 255), {RGB(242, 15, 129), RGB(242, 70, 32), RGB(253, 221, 1), RGB(101, 188, 60), RGB(97, 202, 226), RGB(158, 73, 156), RGB(255, 77, 88)}},   /* HAZE */
    {RGB(0, 0, 0), RGB(25, 18, 12), RGB(53, 46, 37), RGB(80, 71, 59), RGB(255, 240, 214), RGB(255, 240, 214), RGB(0, 225, 123), RGB(255, 255, 255), RGB(218, 182, 53), RGB(38, 121, 190), RGB(255, 255, 255), {RGB(246, 113, 43), RGB(247, 176, 141), RGB(255, 219, 34), RGB(84, 157, 218), RGB(224, 162, 79), RGB(180, 66, 118), RGB(20, 141, 80)}},   /* MID */
    {RGB(0, 0, 0), RGB(16, 16, 16), RGB(16, 16, 16), RGB(88, 88, 88), RGB(219, 219, 219), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(200, 215, 93), RGB(255, 255, 255), RGB(219, 219, 219), {RGB(235, 175, 199), RGB(251, 221, 135), RGB(251, 221, 135), RGB(216, 102, 70), RGB(107, 23, 53), RGB(51, 110, 200), RGB(164, 188, 225)}},   /* BALANCE */
};
static pw_pal upal_p;      /* the USER palette: sent from the web page (link command PAL_PUT), kept in the settings object; the row after the built-in ones, there only once one is sent */
static uint8_t upal_set;
static unsigned pal_n(void) { return PAL_COUNT + upal_set; }
static const pw_pal *pal_ptr(unsigned row) { return row < PAL_COUNT ? &pals[row] : &upal_p; }
static const char *pal_label(unsigned row) { return row < PAL_COUNT ? pal_name[row] : "USER"; }
static unsigned pal_row_of(unsigned id)
{
    if (id == PAL_USER_ID && upal_set) return PAL_COUNT;
    for (unsigned i = 0; i < PAL_COUNT; ++i) if (pal_id[i] == id) return i;
    return PW_PAL_DEFAULT;
}
static pw_pal npal;   /* (set by pal_set before the first frame) */
#define C_BG (npal.black)
#define C_PANEL (npal.panel)
#define C_PANEL2 (npal.raised)
#define C_LINE (npal.line)
#define C_STRIP (npal.black)
#define C_STRIPL (npal.raised)
#define C_PLATE_E (npal.line)
#define C_PLATE (npal.panel)
#define C_WELL (npal.black)
#define C_SG1 (npal.black)
#define C_SG2 (npal.panel)
#define C_SG3 (npal.raised)
#define C_SGW (npal.black)
#define C_SBG (npal.black)
#define C_SPANEL (npal.panel)
#define C_SPANEL2 (npal.raised)
#define C_DIM (npal.dim)
#define C_TEXT (npal.text)
#define C_GREEN (npal.green)
#define C_RED (npal.red)
#define C_AMBER (npal.amber)
#define C_MOD (npal.mod)
#define C_WHITE (npal.white)
#define cat_color (npal.cat)

/* the buttons in pw_hw.h's order, by their printed labels */
enum { B_OCT_DOWN, B_OCT_UP, B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO,
       B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC };
enum { E_SELECT, E_ALGO, E_K1, E_K2, E_K3, E_K4, E_PRESETS };
enum { PG_BOOT, PG_RACK, PG_EDIT, PG_ADD, PG_CABLES, PG_SCOPE, PG_SEQ, PG_PATCHES, PG_HELP, PG_PERFORM, PG_LIVE, PG_SONG, PG_SETTINGS, PG_MUTATE, PG_SAVE };
static int key_tag(int x, int y, const char *k, uint16_t c);
static int key_w(const char *k);
static void hint_draw(int x, int y, const char *spec);
static char strip_buf[160], strip_right[20];
static const char *strip_pre, *strip_post;
static int ky_off;   /* the patch screen's strip is two rows tall: everything above it (the knob row, the status lines) moves up this much */
static int strip_chg;   /* 0: the page has no Bar Sync setting on its strip, 1: it has, 2: the cursor is on it (framed) */
static void strip_set(const char *spec);
static int hint_w(const char *spec);
static const char *const page_title[] = {"", "PATCH", "MODULE", "ADD MODULE", "MATRIX", "SCOPE", "STEPS",
                                         "MENU", "HELP", "PERFORM", "MUTES", "SONG", "SETTINGS", "MUTATE", "SAVE"};
#define BIT(b) (1u << (b))

/* ---------------------------------------------------------- display list */
enum { F_XS, F_S, F_M, F_L };   /* (F_L: only the letters of PATCHWERK, for the splash) */
static const pw_fontdef *const fonts[4] = {&pw_font_XS, &pw_font_S, &pw_font_M, &pw_font_L};
enum { PR_FILL, PR_TEXT, PR_LINE };
typedef struct { uint8_t kind, font; uint16_t color; int16_t x, y, w, h; uint16_t text; } prim;
#define MAX_PRIMS 800    /* (the busiest screen of all 57 factory patches draws 482) */
#define TEXT_POOL 3000   /* (the most any screen has used: 360) */
static prim prims[MAX_PRIMS] PW_POOL;
static unsigned n_prims, n_text;
static char text_pool[TEXT_POOL] PW_POOL;
static uint16_t band[W * 16] PW_POOL;   /* (the pool, not static RAM) */
#define TILE 16
static uint32_t tile_hash[H / 16][W / TILE];   /* one per 16x16 tile: only changed tiles are sent */
static uint8_t band_valid;

static void fill(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0 || n_prims >= MAX_PRIMS) return;
    prim *p = &prims[n_prims++];
    p->kind = PR_FILL; p->color = c; p->x = (int16_t)x; p->y = (int16_t)y; p->w = (int16_t)w; p->h = (int16_t)h;
}
static void rfill(int x, int y, int w, int h, uint16_t c)   /* corners cut: a softer box */
{
    fill(x + 1, y, w - 2, h, c); fill(x, y + 1, 1, h - 2, c); fill(x + w - 1, y + 1, 1, h - 2, c);
}
static void frame_rect(int x, int y, int w, int h, uint16_t c)
{
    fill(x, y, w, 1, c); fill(x, y + h - 1, w, 1, c); fill(x, y, 1, h, c); fill(x + w - 1, y, 1, h, c);
}
static void line(int x0, int y0, int x1, int y1, int thick, uint16_t c)
{
    if (n_prims >= MAX_PRIMS) return;
    prim *p = &prims[n_prims++];
    p->kind = PR_LINE; p->font = (uint8_t)thick; p->color = c;
    p->x = (int16_t)x0; p->y = (int16_t)y0; p->w = (int16_t)x1; p->h = (int16_t)y1;
}
static unsigned glyph_index(const pw_fontdef *f, unsigned ch)
{
    if (ch > f->last && ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < f->first || ch > f->last) ch = '?';
    return ch - f->first;
}
static int text_w(const char *s, int font)
{
    const pw_fontdef *f = fonts[font];
    int w = 0;
    for (; *s; ++s) w += f->g[glyph_index(f, (unsigned char)*s)].adv;
    return w;
}
/* y: the top of the capitals. Returns the pen's x after the text. */
static int text(int x, int y, const char *s, int font, uint16_t c)
{
    unsigned len = (unsigned)strlen(s);
    int w = text_w(s, font);
    if (!len || n_prims >= MAX_PRIMS || n_text + len + 1 > TEXT_POOL) return x + w;
    prim *p = &prims[n_prims++];
    p->kind = PR_TEXT; p->font = (uint8_t)font; p->color = c; p->x = (int16_t)x; p->y = (int16_t)y;
    p->w = (int16_t)w; p->h = (int16_t)fonts[font]->lh; p->text = (uint16_t)n_text;
    memcpy(text_pool + n_text, s, len + 1);
    n_text += len + 1;
    return x + w;
}
static void text_center(int cx, int y, const char *s, int font, uint16_t c) { text(cx - text_w(s, font) / 2, y, s, font, c); }
static void text_right(int rx, int y, const char *s, int font, uint16_t c) { text(rx - text_w(s, font), y, s, font, c); }
/* the text cut to fit max_w pixels (".." at the end when cut) */
static void text_fit(int x, int y, const char *s, int font, uint16_t c, int max_w)
{
    char buf[64];
    unsigned n = 0;
    while (s[n] && n < sizeof buf - 3) { buf[n] = s[n]; ++n; }
    buf[n] = 0;
    if (text_w(buf, font) <= max_w) { text(x, y, buf, font, c); return; }
    while (n > 1) {
        buf[--n] = 0;
        buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = 0;
        if (text_w(buf, font) <= max_w) break;
        buf[n] = 0;
    }
    text(x, y, buf, font, c);
}
static int text_clip(int x, int y, const char *s, int font, uint16_t c, int lim)   /* text_fit up to x = lim; returns where it ends */
{
    int room = lim - x;
    if (room < 6) return x;
    text_fit(x, y, s, font, c, room);
    int w = text_w(s, font);
    return x + (w < room ? w : room);
}
static inline uint16_t blend(uint16_t bg, uint16_t fg, unsigned a15)
{
    uint32_t al = (a15 * 32u + 7u) / 15u;
    uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu, f = (fg | ((uint32_t)fg << 16)) & 0x07E0F81Fu;
    uint32_t r = ((f * al + b * (32u - al)) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(r | (r >> 16));
}
static void raster_text(const prim *p, int top)
{
    const pw_fontdef *f = fonts[p->font];
    const char *s = text_pool + p->text;
    int x = p->x;
    for (; *s; ++s) {
        const pw_glyph *g = &f->g[glyph_index(f, (unsigned char)*s)];
        int gy0 = p->y + g->y, gx0 = x + g->x, stride = (g->w + 1) / 2;
        x += g->adv;
        if (!g->w || gy0 >= top + 16 || gy0 + g->h <= top || gx0 >= W || gx0 + g->w <= 0) continue;
        int r0 = top - gy0 > 0 ? top - gy0 : 0, r1 = top + 16 - gy0 < g->h ? top + 16 - gy0 : g->h;
        for (int r = r0; r < r1; ++r) {
            const uint8_t *src = f->bits + g->off + r * stride;
            uint16_t *row = band + (gy0 + r - top) * W;
            for (int c = 0; c < g->w; ++c) {
                unsigned a = (c & 1) ? (src[c >> 1] & 15u) : (src[c >> 1] >> 4);
                int xx = gx0 + c;
                if (!a || xx < 0 || xx >= W) continue;
                row[xx] = a == 15u ? p->color : blend(row[xx], p->color, a);
            }
        }
    }
}
static void raster_band(int top)
{
    for (int i = 0; i < W * 16; ++i) band[i] = C_BG;
    for (unsigned n = 0; n < n_prims; ++n) {
        const prim *p = &prims[n];
        if (p->kind == PR_LINE) {
            int x0 = p->x, y0 = p->y, x1 = p->w, y1 = p->h, th = p->font;
            int lo = y0 < y1 ? y0 : y1, hi = (y0 > y1 ? y0 : y1) + th;
            if (lo >= top + 16 || hi < top) continue;
            int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
            int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1, err = dx + dy;
            for (;;) {
                for (int t = 0; t < th; ++t) {
                    int yy = y0 + t;
                    if (yy >= top && yy < top + 16 && x0 >= 0 && x0 < W) band[(yy - top) * W + x0] = p->color;
                }
                if (x0 == x1 && y0 == y1) break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
            }
            continue;
        }
        if (p->kind == PR_TEXT) {
            if (p->y - 8 >= top + 16 || p->y + p->h <= top) continue;
            raster_text(p, top);
            continue;
        }
        if (p->y >= top + 16 || p->y + p->h <= top) continue;
        int y0 = p->y < top ? top : p->y, y1 = p->y + p->h > top + 16 ? top + 16 : p->y + p->h;
        int x0 = p->x < 0 ? 0 : p->x, x1 = p->x + p->w > W ? W : p->x + p->w;
        for (int y = y0; y < y1; ++y) {
            uint16_t *row = band + (y - top) * W;
            for (int x = x0; x < x1; ++x) row[x] = p->color;
        }
    }
}
static void present(void)
{
    /* Each band is drawn whole, then compared tile by tile with what the panel already shows; only the
     * runs of changed 16x16 tiles are sent. The FM-1's panel has no tearing-effect line, so the less a
     * frame sends, the less a transfer can race the panel's refresh (and the faster a frame is). */
    for (int b = 0; b < H / 16; ++b) {
        uint8_t changed[W / TILE];
        raster_band(b * 16);
        for (int t = 0; t < W / TILE; ++t) {
            uint32_t h = 2166136261u;
            for (int r = 0; r < 16; ++r) {
                const uint16_t *p = band + r * W + t * TILE;
                for (int i = 0; i < TILE; ++i) h = (h ^ p[i]) * 16777619u;
            }
            changed[t] = !band_valid || tile_hash[b][t] != h;
            tile_hash[b][t] = h;
        }
        for (int t = 0; t < W / TILE;) {
            if (!changed[t]) { ++t; continue; }
            int t1 = t;
            while (t1 < W / TILE && changed[t1]) ++t1;
            fm1_screen_blit((unsigned)(t * TILE), (unsigned)(b * 16), (unsigned)((t1 - t) * TILE), 16, band + t * TILE, W);
            t = t1;
        }
    }
    band_valid = 1;
    n_prims = n_text = 0;
}

/* ------------------------------------------------------------ text utils */
static char *s_cpy(char *o, const char *s) { while (*s) *o++ = *s++; *o = 0; return o; }
static char *s_int(char *o, int v)
{
    char t[12]; int n = 0;
    if (v < 0) { *o++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *o++ = t[--n];
    *o = 0; return o;
}
static char *s_tenths(char *o, int q)   /* q tenths as 12.3 (sign first when below 0) */
{
    if (q < 0) { *o++ = '-'; q = -q; }
    o = s_int(o, q / 10); *o++ = '.'; *o++ = (char)('0' + q % 10); *o = 0;
    return o;
}
static void port_label(unsigned slot, int is_out, unsigned port, char *out)
{
    char *o = out;
    pw_module_tag_label(slot, o);   /* (the module's own name, as its box says: the preset is a line of its own in the box) */
    o += strlen(o);
    *o++ = ' ';
    const pw_type *t = &pw_types[pws.mod[slot].type];
    s_cpy(o, is_out ? t->out[port] : t->in[port]);
}
static const char *const note_name[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void note_text(int midi, char *o)
{
    o = s_cpy(o, note_name[((midi % 12) + 12) % 12]);
    s_int(o, midi / 12 - 1);
}

/* ------------------------------------------------------------- UI state */
static struct {
    unsigned page, prev_page;
    int sel;                     /* selected module slot, -1 none */
    unsigned param_page[PW_MAX_MODULES];
    uint8_t page_type[PW_MAX_MODULES];   /* the type the page was turned for, + 1 */
    int cable_cursor;
    int lst_cursor, lst_pick, lst_pcur, lst_sm, lst_so, lst_dm, lst_di, lst_cable;   /* MATRIX and its cable picker */
    uint32_t lst_sel_ms; uint8_t lst_sel_down, lst_sel_long, lst_coll[8];   /* lst_coll: bit g set = group g folded in the matrix */
    int live_row, morph_b, live_qbar, solo_slot; float morph; uint64_t live_queue, solo_prev;   /* LIVE */
    uint8_t live_sel_used, live_rec_used, rsel_down, rsel_used;   /* (rsel: SEL on the patch screen acts when it is let go, unless a snapshot key was pressed during it) */
    uint8_t mut_row, mut_flash; int mut_last; float mut_amt, mut_den; uint32_t mut_ms;   /* the MUTATE page: the row, the key that just fired (1 nudge, 2 roll) and when, how many things the last one changed, AMOUNT and DENSITY */
    uint8_t fav[8], rec[6];   /* the ADD list: the favorites (REC on the list: a star) and the last six added (an R), each a type + 1 (0: none) */
    uint8_t add_page;   /* the FX list's right panel: 0 what a module is, 1 EXAMPLE PATCH, 2 VISUALIZATION */
    int8_t song_lane, rec_lane, rec_touch_lane; uint32_t rec_touch_ms; int16_t song_bar, song_vs, song_pickc; uint8_t song_sub, song_gsel, song_carry, song_newlen, song_ostart, song_chain, song_loading, last_mod, last_par, song_zoom, song_pick, song_pcarry, song_pv, song_psh, song_rec, rec_q; uint16_t song_pold, rec_t; uint32_t song_s16, song_t0;   /* SONG: the cursor (bar -1: the lane's name), the clip in hand (index + 1) */
    uint8_t var_quant; int8_t var_pend; int var_pend_bar;   /* PERFORM: variation changes wait for the next bar */
    int roll_t, roll_p, roll_len, roll_vx, roll_vy; uint32_t roll_ms, roll_ser; uint8_t roll_have, roll_fold; int roll_wo;   /* PIANO ROLL: the cursor's step and pitch, the length a new note gets, the view, the last key (a chord is keys within a moment) */
    int seq_cursor, seq_rec;   /* seq_rec: 0 off, 1 STEP (a note at the cursor), 2 LIVE (the transport runs: a note lands on the nearest step) */
    uint8_t trig_rec;          /* TRIG SEQ: REC cycles 0 off (the keys draw the grid), 1 STEP (C D E F = tracks A-D at the cursor), 2 LIVE */
    int trig_cur; uint8_t mon_key, mon_slot;   /* TRIG SEQ step cursor; the SEQUENCER key being monitored (+ 1) */
    int list_cursor;             /* patches page */
    int last_list;               /* the list row last loaded: the list opens on it */
    int press_last;              /* the keys' pressure last sent */
    int8_t save_cur; uint8_t save_group;   /* the SAVE page: the slot under the cursor, and whether it stores a GROUP */
    int user_slot;               /* the user slot the patch came from or went to, -1 none */
    int help_scroll;
    /* patch view: cursor jack and the cable being carried (out: 1 output, 0 input) */
    int cur_slot, cur_out, cur_port, carrying, carry_slot, carry_out, carry_port, carry_old, view_x, view_y;
    int name_pos;
    int edit_jacks, trig_track;
    uint8_t gview, gsug, compact; int8_t gsug_i;   /* the patch view: the group drilled into (0: the top, groups collapsed); suggesting groups: which */
    int add_cursor, page_from_patch; uint8_t last_type[4];   /* the add list: where it is, where it starts */
    uint32_t rec_down_ms, home_down_ms; uint8_t rec_used, rec_warned, rec_wire, home_rack, home_cyc;   /* (HOME on the patch screen: tapped, the selected module's next knob page; held, CPU and battery) */        /* REC: tapped unplugs, held deletes */
    int probe_slot, probe_port;                                 /* the last output the patch cursor was on (SCOPE, unpatched) */
    int raw_select, sel_n, sel_dir, sel_acc; uint32_t sel_ms;      /* SELECT's speed: fast turns jump modules */
    int perf_cursor, perf_bank; uint8_t arp_used, env_used, learn_used; int8_t lr_mseq; uint8_t lr_lane, lr_mod, lr_par; int8_t lr_pos; uint32_t lr_ms; uint32_t env_down_ms, arp_down_ms, learn_serial;  /* the module page: the jacks instead of the visualization; TRIG SEQ's track */
    int octave;
    uint32_t buttons, keys;
    uint32_t glo_down_ms, glo_enc_ms, tempo_ms; uint8_t glo_used, glo_latch, glo_on, cmp_auto;   /* (tempo_ms: when the tempo was last turned: the header shows it brighter for a moment) */
    uint8_t core_i, set_row, tone_i, bright_i, pal_i, autosave, edit_def, vel_mode, ctl_i, rate_i, smart_i, mclk_i, pch_i, diag, reset_all, set_dirty, as_dirty; int8_t as_slot; uint32_t as_rev, as_ms;   /* SETTINGS */
      /* glo_latch: the setting (GLO is tapped to latch instead of held); glo_on: GLO is latched now */   /* GLO tapped (no other use while held): the shortcuts */
    uint8_t key_note[27];
    uint8_t kbd_btn, kbd_eff;           /* the keyboard mode: BUTTONS (GLO + HOME) when the patch has a BUTTON module (always, without a KEYS module) */
    uint32_t btn_block;                 /* the keys already held when the mode changed: they count once they are let go and pressed again */
    const char *notice; char notice_buf[44]; uint32_t notice_until; uint8_t notice_glo;   /* (notice_glo: shown even while GLO is held: the answer to a GLO + key) */
    uint32_t confirm_until; unsigned confirm_kind; int confirm_arg;
    uint32_t boot_ms, boot_end, frame_ms;
    uint32_t led_buttons, led_keys, led_dim;
    uint8_t leds_valid;
} ui;

#define NOTICE_MS 900u   /* how long the words in the strip stay */
static void notice(const char *msg)
{
    unsigned n = 0;
    while (msg[n] && n < sizeof ui.notice_buf - 1) { ui.notice_buf[n] = msg[n]; ++n; }
    ui.notice_buf[n] = 0;
    ui.notice = ui.notice_buf;
    ui.notice_glo = 0;
    ui.notice_until = fm1_millis() + NOTICE_MS;
}
static void notice_glo(const char *msg) { notice(msg); ui.notice_glo = 1; ui.notice_until = fm1_millis() + 1300u; }   /* (the answer to GLO + a key: stays over GLO's own hints) */
void pw_ui_notice(const char *msg) { notice(msg); }
static int first_module(void)
{
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) return (int)i;
    return -1;
}
/* Next used module from `from` in direction `dir`, optionally filtered (1: has outputs). */
static int step_module(int from, int dir, int need)
{
    if (dir == 0) return from;
    int n = PW_MAX_MODULES, s = from < 0 ? (dir > 0 ? -1 : n) : from;
    int steps = dir > 0 ? dir : -dir, last = from;
    while (steps--) {
        int found = -1;
        for (int k = 1; k <= n; ++k) {
            int c = s + (dir > 0 ? k : -k);
            if (c < 0 || c >= n) break;
            const pw_module *m = &pws.mod[c];
            if (m->used != 1) continue;
            if (need == 1 && !pw_types[m->type].n_out) continue;
            found = c; break;
        }
        if (found < 0) break;
        s = last = found;
    }
    return last;
}
static int valid_sel(int need)
{
    if (ui.sel >= 0 && ui.sel < PW_MAX_MODULES && pws.mod[ui.sel].used == 1) {
        const pw_type *t = &pw_types[pws.mod[ui.sel].type];
        if (!need || (need == 1 && t->n_out)) return ui.sel;
    }
    int f = step_module(-1, 1, need);
    return f >= 0 && pws.mod[f].used == 1 ? f : -1;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void song_record(unsigned mod, unsigned par);   /* SONG: a knob turned while REC is armed lays points */
/* the step editors (SEQ): SEQUENCER, MOD SEQ, CHORD STEPS and GRAVITY (the TRIG SEQ has its own page) */
static int is_stepper(unsigned t) { return t == PW_T_SEQ || t == PW_T_MODSEQ || t == PW_T_CHORDSTEPS || t == PW_T_GRAVITY || t == PW_T_PROLL; }
static int slot_is(int s, unsigned type) { return s >= 0 && s < PW_MAX_MODULES && pws.mod[s].used == 1 && pws.mod[s].type == type; }
static int find_type(unsigned type)
{
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1 && pws.mod[i].type == type) return (int)i;
    return -1;
}
static int find_seq(void)   /* the module the SEQ page edits: the selected one if it has steps, else the first (SEQUENCER first) */
{
    if (ui.sel >= 0 && ui.sel < PW_MAX_MODULES && pws.mod[ui.sel].used == 1 && is_stepper(pws.mod[ui.sel].type)) return ui.sel;
    static const uint8_t order[5] = {PW_T_SEQ, PW_T_CHORDSTEPS, PW_T_MODSEQ, PW_T_GRAVITY, PW_T_PROLL};
    for (int k = 0; k < 5; ++k) { int s = find_type(order[k]); if (s >= 0) return s; }
    return -1;
}
static int seq_len(int s)   /* the steps in use */
{
    const pw_module *m = &pws.mod[s];
    int n = m->type == PW_T_SEQ || m->type == PW_T_CHORDSTEPS ? (int)(m->p[0] + 0.5f) : m->type == PW_T_GRAVITY ? (int)(m->p[3] + 0.5f) : 16;
    return n < 1 ? 1 : n > 16 ? 16 : n;
}
static int find_trig(void)
{
    if (ui.sel >= 0 && ui.sel < PW_MAX_MODULES && pws.mod[ui.sel].used == 1 && pws.mod[ui.sel].type == PW_T_TRIGSEQ) return ui.sel;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1 && pws.mod[i].type == PW_T_TRIGSEQ) return (int)i;
    return -1;
}
static int next_stepper(int cur)   /* SEQ again: the next module in the patch that has steps (the TRIG SEQ too) */
{
    for (int d = 1; d <= PW_MAX_MODULES; ++d) {
        int i = ((cur + d) % PW_MAX_MODULES + PW_MAX_MODULES) % PW_MAX_MODULES;
        if (pws.mod[i].used == 1 && (is_stepper(pws.mod[i].type) || pws.mod[i].type == PW_T_TRIGSEQ)) return i;
    }
    return -1;
}
/* ---- the add list's preview draws a module nobody plays: while pv_demo is set, what the pictures read from the engine
 * (pw_live, pw_out_last, pw_in_last) is made up instead, a sample of the module at work (what a patch feeding it would show). */
static int pv_demo;
static float pv_off;   /* seconds back from now: a strip chart's history is made up as if it had been running */
static float pv_t(void) { return (float)(fm1_millis() % 600000u) * 0.001f - pv_off; }
static uint32_t pv_hash(uint32_t v) { v ^= v >> 16; v *= 0x7feb352dU; v ^= v >> 15; v *= 0x846ca68bU; v ^= v >> 16; return v; }
static float pv_rnd(uint32_t n) { return (float)(pv_hash(n * 2654435761u + 77u) & 1023u) * (1.0f / 511.5f) - 1.0f; }   /* -1..1, the same for the same n */
static float pv_sq(float t, float period, float duty) { float u = t / period - (float)(int)(t / period); return u < duty ? 1.0f : 0.0f; }
static float pv_fr(float x) { return x - (float)(int)x; }
/* ROOM MODES: the pitch of mode k from the knobs (the engine's own list of ratios), and the sound the add list's preview makes up */
static float room_hz(const pw_module *m, unsigned k)
{
    static const float rm[8][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {2, 0, 0}, {1, 0, 1}, {0, 2, 0}, {0, 1, 1}};
    int nm = (int)(m->p[2] + 0.5f);
    if (k >= 8u || (int)k >= nm) return 0.0f;
    float fy = 1.2f + 0.7f * m->p[1], fz = 1.5f + 1.5f * m->p[1], a = rm[k][0], b = rm[k][1] * fy, c = rm[k][2] * fz;
    return m->p[0] * pw_exp2(0.5f * pw_log2(a * a + b * b + c * c));
}
static float pv_live(unsigned s, unsigned k)
{
    if (!pv_demo || s >= PW_MAX_MODULES) return pw_live(s, k);
    const pw_module *m = &pws.mod[s];
    float T = pv_t();
    switch (m->type) {
    case PW_T_SHIFTREG: { int st = (int)(T * 3.0f); return k < 16u ? (pv_rnd((uint32_t)(st - (int)k) + 301u) * 3.5f) : 0.0f; }
    case PW_T_ENV: {
        float u = T - 2.6f * (float)(int)(T / 2.6f), sus = m->p[2], v = 0; int st = 0;
        if (u < 0.25f) { st = 1; v = u / 0.25f; }
        else if (u < 0.6f) { st = 2; v = 1.0f - (1.0f - sus) * (u - 0.25f) / 0.35f; }
        else if (u < 1.6f) { st = 3; v = sus; }
        else if (u < 2.3f) { st = 4; v = sus * (1.0f - (u - 1.6f) / 0.7f); }
        return k == 0 ? v : (float)st;
    }
    case PW_T_CVREC: {
        float ph = pv_fr(T * 0.3f);
        if (k == 0) return ph;
        if (k == 1) { int d = m->data[(int)(ph * 48.0f) % 48]; return ((float)d / 255.0f - 0.5f) * 10.0f; }
        return 0;
    }
    case PW_T_GRAIN:
        if (k == 8) return pv_fr(T * 0.17f);
        if (k < 8) {   /* grains scattered round the window POS and SPREAD make, in the middle of the picture */
            if (((int)(T * 3.0f) + (int)k) % 3 == 0) return -1.0f;
            float head = pv_fr(T * 0.17f), at = (1.0f - m->p[0]) + 0.5f * m->p[4] * pv_rnd((uint32_t)(T * 3.0f) * 8u + k);
            at = at < 0 ? 0 : at > 1 ? 1 : at;
            float g = head - (1.0f - at);
            return g < 0 ? g + 1.0f : g;
        }
        return 0;
    case PW_T_FMDX: {   /* a note every 2.4 s: the operators fade at their own paces, the key up after 1.2 s */
        float u = T - 2.4f * (float)(int)(T / 2.4f), e;
        unsigned n = k & 3u;
        e = pw_exp2(-pw_minf(u, 1.2f) * (0.6f + 0.9f * (float)n));
        if (u > 1.2f) e *= pw_exp2(-(u - 1.2f) * 5.0f);
        return k < 4u ? e : (u < 0.05f ? 0.0f : u < 1.2f ? 1.0f : 3.0f);
    }
    case PW_T_SEQ: { int n = (int)(m->p[0] + 0.5f); n = n < 1 ? 1 : n > 16 ? 16 : n; return k == 0 ? (float)((int)(T * 4.0f) % n) : 0; }
    case PW_T_TRIGSEQ: { int n = (int)(m->p[4 + (k & 3)] + 0.5f); n = n < 1 ? 1 : n > 16 ? 16 : n; return (float)((int)(T * 4.0f) % n); }
    case PW_T_MODSEQ: { int n = (int)(m->p[k & 3] + 0.5f); n = n < 1 ? 1 : n > 16 ? 16 : n; return (float)((int)(T * 4.0f) % n); }
    case PW_T_CHORDSTEPS: { int n = (int)(m->p[0] + 0.5f); n = n < 1 ? 1 : n > 16 ? 16 : n; return k == 0 ? (float)((int)(T * 1.5f) % n) : 0; }
    case PW_T_TURING: { uint32_t r = pv_hash((uint32_t)(T * 4.0f) + 5u); return k == 0 ? (float)(r & 0xFFFFu) : (float)(r >> 16); }
    case PW_T_FRACTURE: return k == 0 ? (float)((int)(T * 0.7f) % 3) : pv_fr(T * 1.1f);
    case PW_T_GENDY: {   /* corners wandering round their places with the chaos knobs, 12 s to and fro */
        int n = (int)(m->p[1] + 0.5f); n = n < 3 ? 3 : n > 16 ? 16 : n;
        if (k == 32u) return pv_fr(T * 0.4f);
        float wob = 0.15f + 0.85f * m->p[2], twob = 0.05f + 0.25f * m->p[3];
        if (k < 16u) return (int)k < n ? pw_clamp(0.9f * pw_sin1(T * 0.13f * (1.0f + 0.17f * (float)k) + 0.37f * (float)k) * (0.4f + 0.6f * wob), -1.0f, 1.0f) : 0.0f;
        unsigned j = k - 16u; if ((int)j > n) return 1.0f; if (j == 0u) return 0.0f;
        return pw_clamp(((float)j + twob * pw_sin1(T * 0.09f * (1.0f + 0.3f * (float)j) + (float)j)) / (float)n, 0.0f, 1.0f);
    }
    case PW_T_VOCODER: return k < 12u ? pw_clamp(pv_fr(T * (0.45f + 0.11f * (float)k) + 0.37f * (float)k) * (1.0f - 0.04f * (float)k) * 1.1f, 0.0f, 1.0f) : 0.0f;   /* the bands moving with a voice */
    case PW_T_ROOM: {   /* a click every 4 s: each mode rings out, the high ones sooner */
        if (k >= 8u) return room_hz(m, k - 8u);
        float t = T - 4.0f * (float)(int)(T / 4.0f), tau = m->p[3] / (1.0f + 0.6f * (float)k) * 0.5f;
        return room_hz(m, k) > 0.0f ? 0.9f * pw_exp2(-t / pw_maxf(tau, 0.05f) * 1.4427f) : 0.0f;
    }
    default: return pw_live(s, k);
    }
}
static float pv_port(unsigned s, unsigned k, int is_in)
{
    const pw_module *m = &pws.mod[s];
    float T = pv_t(), a = pv_sq(T, 1.3f, 0.5f), b = pv_sq(T, 0.8f, 0.5f);
    uint32_t n = (uint32_t)(T * 2.2f);
    switch (m->type) {
    case PW_T_FUNC: { float u = T - 2.0f * (float)(int)(T / 2.0f); float v = u < 0.3f ? u / 0.3f : u < 1.0f ? 1.0f : u < 1.5f ? 1.0f - (u - 1.0f) / 0.5f : 0; return k == 0 ? v * 5.0f : k == 1 ? (1.0f - v) * 5.0f : (u > 1.45f && u < 1.55f ? 5.0f : 0); }
    case PW_T_SH: { float cur = pv_rnd(n) * 3.0f, prev = pv_rnd(n - 1u) * 3.0f, f = pv_fr(T * 2.2f); return k == 0 ? cur : k == 1 ? prev + (cur - prev) * f * f * (3.0f - 2.0f * f) : (f < 0.15f ? 5.0f : 0); }
    case PW_T_LISTENER: { float f = pv_fr(T * 2.2f); return k == 0 ? pv_rnd(n) * 2.0f : k == 1 ? (f < 0.7f ? 5.0f : 0) : k == 2 ? 5.0f * (1.0f - f) : pv_rnd(n + 9u) * 3.0f; }
    case PW_T_LOGIC: return 5.0f * (k == 0 ? a * b : k == 1 ? (a + b > 0.5f ? 1.0f : 0) : k == 2 ? (a != b ? 1.0f : 0) : k == 5 ? (a < b ? a : b) : k == 6 ? (a > b ? a : b) : k == 7 ? a : (a * b > 0.5f ? 0 : 1.0f));
    case PW_T_COMPARE: {   /* A a slow wave, B a slower one; the gate from the knobs (HYST left out) */
        float av = 3.0f * pw_sin1(T * 0.9f) + pw_sin1(T * 2.3f), bv = 1.5f * pw_sin1(T * 0.37f + 1.0f), th = m->p[0], d = av - bv;
        int mode = (int)(m->p[2] + 0.5f);
        if (is_in) return k == 0 ? av : bv;
        float gd = (3.0f * pw_sin1((T - 0.12f) * 0.9f) + pw_sin1((T - 0.12f) * 2.3f)) - 1.5f * pw_sin1((T - 0.12f) * 0.37f + 1.0f);
        int g = mode == 0 ? d > th : mode == 1 ? -d > th : mode == 2 ? pw_absf(d) < pw_absf(th) : (d > th) != (gd > th);
        return k == 0 ? (g ? 5.0f : 0.0f) : k == 1 ? (g ? 0.0f : 5.0f) : d;
    }
    case PW_T_COIN: { float f = pv_fr(T * 3.0f); int hitB = (pv_rnd((uint32_t)(T * 3.0f)) * 0.5f + 0.5f) < m->p[0]; return f < 0.3f && ((k == 1) == hitB) ? 5.0f : 0; }
    case PW_T_CHAOS: return k == 0 ? 4.0f * pw_sin1(T * 0.35f + 2.0f * pw_sin1(T * 0.11f)) : k == 1 ? 4.0f * pw_sin1(T * 0.27f + 2.5f * pw_sin1(T * 0.13f)) : 3.0f + 3.0f * pw_cos1(T * 0.5f + 2.0f * pw_sin1(T * 0.15f));
    case PW_T_TURING: return k == 0 ? (float)(pv_hash((uint32_t)(T * 4.0f) + 5u) & 0xFFu) * (5.0f / 255.0f) : 0;
    case PW_T_ENVFOLLOW: return k == 0 ? 5.0f * pv_fr(T * 1.1f) * (1.0f - pv_fr(T * 1.1f)) * 4.0f : 0;
    case PW_T_FRACTURE: return is_in ? 2.5f + 2.5f * pw_sin1(T * 1.7f) : 0;
    case PW_T_CVMIX: { float v = 1.8f * pw_sin1(T * 0.21f) + 1.2f * pw_sin1(T * 0.47f + 0.3f) + 0.9f * pw_sin1(T * 0.11f + 0.7f); return k == 0 ? v : k == 1 ? -v : 0; }
    case PW_T_GATEDLY: { float d = k == 0 ? 0.25f : 0.5f; return is_in ? 0 : 5.0f * pv_sq(T - d, 1.1f, 0.3f); }
    case PW_T_MIDIOUT: {   /* a little tune on PITCH and GATE, two CCs breathing */
        static const int8_t tune[8] = {0, 3, 7, 10, 12, 10, 7, 3};
        int st = (int)(T * 3.0f); float f = pv_fr(T * 3.0f);
        return k == 0 ? (float)tune[st & 7] / 12.0f : k == 1 ? (f < 0.7f ? 5.0f : 0) : k == 2 ? 3.0f : k == 3 ? 2.5f + 2.2f * pw_sin1(T * 0.23f) : 2.5f + 2.2f * pw_sin1(T * 0.37f + 0.4f);
    }
    default: return is_in ? pw_in_last(s, k) : pw_out_last(s, k);
    }
}
/* a made-up wave for the add list's preview: u 0..1 across the picture, `which` 1 the module's output (0 its input, where one is drawn) */
static float pv_wave(unsigned type, float u, int which)
{
    float T = pv_t(), x = u * 3.0f + T * 0.35f;
    if (type == PW_T_COMPLEX) x = u * 1.7f + T * 0.2f;
    switch (type) {
    case PW_T_COMPLEX: {   /* FM, the index swelling, then folded */
        float idx = 0.9f + 0.7f * pw_sin1(T * 0.4f), v = pw_sin1(x + idx * pw_sin1(x * 1.47f));
        return pw_sin1(0.2f * v * (1.5f + 1.0f * pw_sin1(T * 0.3f)));
    }
    case PW_T_WAVETABLE: {   /* the position sweeping through the table: sine, triangle, saw, square */
        float pos = (0.5f + 0.5f * pw_sin1(T * 0.18f)) * 3.0f, f = pv_fr(x * 0.7f);
        float w[4] = {pw_sin1(f), 4.0f * (f < 0.5f ? f : 1.0f - f) - 1.0f, 2.0f * f - 1.0f, f < 0.5f ? 0.9f : -0.9f};
        int a = (int)pos; if (a > 2) a = 2;
        float t = pos - (float)a;
        return w[a] + (w[a + 1] - w[a]) * t;
    }
    case PW_T_VCA: {   /* a tone shaped by an envelope: up fast, a long fall */
        float e = u < 0.08f ? u / 0.08f : pw_exp2(-(u - 0.08f) * 4.2f);
        return e * pw_sin1(u * 11.0f - T * 0.4f);
    }
    case PW_T_RING: return 0.9f * pw_sin1(u * 9.0f + T * 0.3f) * pw_sin1(u * 2.3f - T * 0.2f);
    case PW_T_PSHIFT: return which ? 0.8f * pw_sin1(u * 6.0f + T * 0.4f) : 0.8f * pw_sin1(u * 4.0f + T * 0.4f);   /* in: a fourth below */
    case PW_T_AUDIOIN: {   /* something played into it: a few partials, loud and quiet */
        float amp = 0.35f + 0.65f * pw_absf(pw_sin1(u * 0.8f + T * 0.17f));
        float v = 0.5f * pw_sin1(u * 7.0f + T * 0.5f) + 0.28f * pw_sin1(u * 17.0f + 0.3f) + 0.18f * pw_sin1(u * 29.0f + T);
        return pw_clamp(amp * v + 0.08f * pv_rnd((uint32_t)(u * 116.0f) + (uint32_t)(T * 20.0f) * 131u), -1.0f, 1.0f);
    }
    default: return 0.55f * pw_sin1(x) + 0.25f * pw_sin1(x * 3.0f + 0.2f);
    }
}
static float pv_out(unsigned s, unsigned k) { return pv_demo && s < PW_MAX_MODULES ? pv_port(s, k, 0) : pw_out_last(s, k); }
static float pv_in(unsigned s, unsigned k) { return pv_demo && s < PW_MAX_MODULES ? pv_port(s, k, 1) : pw_in_last(s, k); }
#define pw_live(s, k) pv_live((s), (k))
#define pw_out_last(s, k) pv_out((s), (k))
#define pw_in_last(s, k) pv_in((s), (k))
/* Ordered list for the ADD browser: grouped by category. */
static uint8_t add_order[PW_T_COUNT];
/* The long list's order inside each kind: the things that belong together side by side (oscillators, then the
 * samplers and drums; filters; effects from tone shaping to space; envelopes and LFOs; the sequencers; and so on).
 * (A saved patch stores the type numbers, so this is only how the list is shown; a type not named here goes last.) */
static const uint8_t add_pref[] = {
    PW_T_VCO, PW_T_MACRO, PW_T_COMPLEX, PW_T_FORMANT, PW_T_WAVETABLE, PW_T_ADDITIVE, PW_T_FM4, PW_T_FMDX, PW_T_CZ, PW_T_STRING, PW_T_GENDY, PW_T_NOISE,
    PW_T_SAMPLER, PW_T_DRUMS, PW_T_DRUMRACK, PW_T_MICROPERC,
    PW_T_VCF, PW_T_VOWEL, PW_T_VOCODER, PW_T_COMB, PW_T_EQ, PW_T_VCA, PW_T_LPG,
    PW_T_FOLDER, PW_T_DIST, PW_T_HARMTREM, PW_T_RING, PW_T_COMP, PW_T_LIMITER, PW_T_OTT, PW_T_PSHIFT, PW_T_WAVESET, PW_T_FRACTURE, PW_T_GRAIN,
    PW_T_SWIRL, PW_T_TAPE, PW_T_LOOPER, PW_T_DELAY, PW_T_REVERB, PW_T_SPACE, PW_T_ROOM,
    PW_T_LFO, PW_T_ENV, PW_T_FUNC, PW_T_INERTIA, PW_T_ENVFOLLOW, PW_T_LISTENER, PW_T_SH, PW_T_CHAOS, PW_T_CVREC, PW_T_PARAMMOD, PW_T_MUTATE, PW_T_SHIFTREG,
    PW_T_ATTEN, PW_T_CVMIX, PW_T_XFADE, PW_T_SNAPSHOT, PW_T_STEREO, PW_T_MIX8, PW_T_SWITCH, PW_T_LOGIC, PW_T_COMPARE, PW_T_COIN, PW_T_SCOPE,
    PW_T_SEQ, PW_T_PROLL, PW_T_MODSEQ, PW_T_TRIGSEQ, PW_T_CHORDSTEPS, PW_T_GRAVITY, PW_T_EUCLID, PW_T_BURST, PW_T_TURING, PW_T_MARKOV, PW_T_ARP, PW_T_CHORD,
    PW_T_GATEDLY, PW_T_QUANT, PW_T_CLOCK,
    PW_T_KEYS, PW_T_BUTTON, PW_T_MIDIIN, PW_T_MIDIOUT, PW_T_AUDIOIN, PW_T_OUTPUT,
};
static void build_add_order(void)
{
    unsigned n = 0;
    uint8_t seen[PW_T_COUNT] = {0};
    for (unsigned c = 0; c < PW_C_COUNT; ++c) {
        for (unsigned i = 0; i < sizeof add_pref; ++i) {
            unsigned t = add_pref[i];
            if (t < PW_T_COUNT && !seen[t] && !pw_type_hidden(t) && pw_types[t].cat == c) { seen[t] = 1; add_order[n++] = (uint8_t)t; }
        }
        for (unsigned t = 0; t < PW_T_COUNT; ++t) if (!seen[t] && !pw_type_hidden(t) && pw_types[t].cat == c) { seen[t] = 1; add_order[n++] = (uint8_t)t; }
    }
}
static int wrap_idx(int v, int n) { return n > 0 ? ((v % n) + n) % n : 0; }   /* a list that goes round its ends (like the patch list) */
static int cat_jump(int cur, int dir)   /* the first module of the next / this-or-previous category (round the ends) */
{
    cur = clampi(cur, 0, PW_T_SHOWN - 1);
    int cat = pw_types[add_order[cur]].cat, i = cur;
    if (dir < 0 && cur == 0) { i = PW_T_SHOWN - 1; cat = pw_types[add_order[i]].cat; while (i > 0 && pw_types[add_order[i - 1]].cat == cat) --i; return i; }
    if (dir > 0) { int e = cur; while (e + 1 < PW_T_SHOWN && pw_types[add_order[e + 1]].cat == cat) ++e; if (e == PW_T_SHOWN - 1) return 0; }
    while (i + dir >= 0 && i + dir < PW_T_SHOWN && pw_types[add_order[i]].cat == cat) i += dir;
    if (dir < 0) { int cc = pw_types[add_order[i]].cat; while (i > 0 && pw_types[add_order[i - 1]].cat == cc) --i; }
    return i;
}
/* the favorites, then the most recent, as list positions, for KNOB 4 and the quick-add keys */
static int fav_range(int *at, unsigned k0, unsigned k1)   /* the ADD list's positions of fav[] (0..7) and rec[] (8..13) entries from k0 to k1, once each */
{
    int n = 0;
    for (unsigned k = k0; k < k1; ++k) {
        unsigned t = k < 8 ? ui.fav[k] : ui.rec[k - 8];
        if (!t) continue;
        --t;
        int dup = 0;
        for (int j = 0; j < n; ++j) if (add_order[at[j]] == t) dup = 1;
        if (dup) continue;
        for (int i = 0; i < PW_T_SHOWN; ++i) if (add_order[i] == t) { at[n++] = i; break; }
    }
    return n;
}
static int is_fav(unsigned t) { for (unsigned k = 0; k < 8; ++k) if (ui.fav[k] == t + 1u) return 1; return 0; }
static int is_recent(unsigned t) { for (unsigned k = 0; k < 6; ++k) if (ui.rec[k] == t + 1u) return 1; return 0; }
static void recent_push(unsigned t)   /* (a module just added goes to the front of the recents) */
{
    unsigned k = 0;
    while (k < 5 && ui.rec[k] != t + 1u) ++k;
    for (; k > 0; --k) ui.rec[k] = ui.rec[k - 1];
    ui.rec[0] = (uint8_t)(t + 1u);
}
static void free_spot(int *x, int *y)
{
    /* Place new modules on the browser grid in the first free cell. */
    for (int row = 0; row < 12; ++row)
        for (int col = 0; col < 8; ++col) {
            int cx = 24 + col * 204, cy = 24 + row * 268, clash = 0;
            for (unsigned i = 0; i < PW_MAX_MODULES && !clash; ++i)
                if (pws.mod[i].used == 1 && pws.mod[i].x > cx - 150 && pws.mod[i].x < cx + 150 &&
                    pws.mod[i].y > cy - 200 && pws.mod[i].y < cy + 200) clash = 1;
            if (!clash) { *x = cx; *y = cy; return; }
        }
    *x = 24; *y = 24;
}
static unsigned param_pages(int slot) { return slot < 0 ? 0 : (pw_types[pws.mod[slot].type].n_param + 3u) / 4u; }
/* a slot's page stays in range when another module takes the slot */
static void fix_page(int slot)
{
    if (slot < 0 || slot >= PW_MAX_MODULES) return;
    if (ui.page_type[slot] != pws.mod[slot].type + 1u || ui.param_page[slot] >= param_pages(slot)) ui.param_page[slot] = 0;
    ui.page_type[slot] = (uint8_t)(pws.mod[slot].type + 1u);
}

/* ---------------------------------------------------------- common draw */
static float norm_of(const pw_param_desc *d, float v)
{
    if (d->kind == PW_EXP && d->min > 0) {
        float a = 0, b = 0, x = v / d->min, r = d->max / d->min;
        /* log ratio via repeated halving: cheap and adequate for a bar */
        while (x >= 2.0f) { x *= 0.5f; a += 1.0f; }
        while (r >= 2.0f) { r *= 0.5f; b += 1.0f; }
        a += x - 1.0f; b += r - 1.0f;
        return b > 0 ? a / b : 0;
    }
    return d->max > d->min ? (v - d->min) / (d->max - d->min) : 0;
}
static float param_norm(unsigned slot, unsigned p)
{
    if ((pws.mod[slot].type == PW_T_CZ || pws.mod[slot].type == PW_T_FMDX) && p < 2u) return pw_cz_norm(slot, p);
    return norm_of(&pw_types[pws.mod[slot].type].param[p], pws.mod[slot].p[p]);
}
/* The knob pages, the same everywhere: ALGO's ring, then "2/6" (its baseline the ring text's). y is the text row, as key_tag's; bg what it sits on. */
static int page_count(unsigned pages, unsigned cur, char *o)
{
    char *q = s_int(o, (int)cur + 1); q = s_cpy(q, "/"); s_int(q, (int)pages);
    return text_w(o, F_XS);
}
static int page_tabs_wk(unsigned pages, const char *key)
{
    char b[12];
    return key_w(key) + 4 + (pages ? page_count(pages, pages - 1, b) : 0);
}
static int page_tabs_w(unsigned pages) { return page_tabs_wk(pages, "ALGO"); }
static void page_tabs_k(int x, int y, unsigned pages, unsigned cur, uint16_t bg, const char *key)
{
    char b[12];
    (void)bg;
    int nx = key_tag(x, y, key, C_AMBER);
    if (!pages) return;
    page_count(pages, cur, b);
    text(nx - 1, y, b, F_XS, C_TEXT);
}
static void page_tabs(int x, int y, unsigned pages, unsigned cur, uint16_t bg) { page_tabs_k(x, y, pages, cur, bg, "ALGO"); }
/* a group box's pages: the same ALGO plate, then a dark plate with the module (in its color) and its page ("1/8", light grey) */
static void page_plate(int xr, int y, const char *name, const char *nums, uint16_t col)
{
    int tw = text_w(name, F_XS) + 6 + text_w(nums, F_XS) + 8, x = xr - page_tabs_wk(0, "ALGO") - tw;
    int nx = key_tag(x, y, "ALGO", C_AMBER) - 2;
    rfill(nx, y - 1, tw, 12, C_PLATE_E); rfill(nx + 1, y, tw - 2, 10, C_PLATE);
    int ex = text(nx + 4, y, name, F_XS, col);
    text(ex + 6, y, nums, F_XS, RGB(205, 207, 218));
}
#define HDR 20          /* the header's height */
#define KY (188 - ky_off)   /* the knob row's top */
#define KH 33           /* its height; under it the hint strip (STRIP_Y) */
#define STRIP_Y (224 - ky_off)   /* the hint strip: every page's shortcuts, one row at the bottom (the patch screen: two), always from the left */
#define STATUS_Y (168 - ky_off)   /* the patch screen's two status lines */
static void rec_dot(int x, int y)   /* the REC light: a red dot that blinks */
{
    if ((fm1_millis() / 350u) & 1u) return;
    fill(x + 1, y, 5, 1, C_RED); fill(x, y + 1, 7, 5, C_RED); fill(x + 1, y + 6, 5, 1, C_RED);
}
/* the pages that ALGORITHM turns between, as tabs in the header: the one you are on lifted and bright, the others dark; returns where the next thing may start */
static int header_tabs(const char *const *nm, int n, int cur)
{
    int x = 4, pad = n > 3 ? 6 : 8, gap = n > 3 ? 1 : 2;   /* (four tabs: a little tighter, to end before the play light) */
    for (int i = 0; i < n; ++i) {
        int on = i == cur, w = text_w(nm[i], F_S) + pad;
        rfill(x, on ? 2 : 4, w, on ? HDR - 2 : HDR - 4, on ? C_PANEL2 : C_BG);
        if (on) fill(x + 2, 2, w - 4, 2, C_AMBER);
        text(x + pad / 2, 6, nm[i], F_S, on ? C_WHITE : C_DIM);
        x += w + gap;
    }
    return x;
}
/* GLO is held (or tapped, where it latches): the keys' shortcuts show, and the header shows the CPU and the battery */
static int home_stats_now(void)   /* HOME held on the patch screen as long as REC takes to delete: the header shows the CPU and the battery while it is held */
{
    return ui.page == PG_RACK && ui.home_rack && (ui.buttons & BIT(B_HOME)) && fm1_millis() - ui.home_down_ms >= 700u;
}
static int glo_held_now(void)
{
    return ui.glo_on || ((ui.buttons & BIT(B_GLO)) && (ui.glo_used || fm1_millis() - ui.glo_down_ms >= 250u));
}
static int bat_pct(void)   /* about 3.4 V empty, 4.15 V full (a count is about 6.6 mV): 0..100, 101 on USB power, -1 unknown */
{
    unsigned raw = fm1_battery_raw();
    if (fm1_battery_level() == 4) return 101;
    return raw ? clampi(((int)(raw * 66u / 10u) - 3400) * 100 / 750, 0, 100) : -1;
}
static void header_stat(int x, const char *label, unsigned pct, uint16_t col, const char *val)
{
    text(x, 5, label, F_XS, C_DIM);
    fill(x + 24, 7, 44, 6, C_PANEL2); fill(x + 24, 7, (int)(pct > 100u ? 100u : pct) * 44 / 100, 6, col);
    text(x + 72, 5, val, F_XS, C_TEXT);
}
static void header_draw(void)
{
    char buf[24];
    fill(0, 0, W, HDR, C_PANEL);
    static const char *const t2[3] = {"PERFORM", "MUTES", "MUTATE"}, *const t3[4] = {"PATCHES", "SAVE", "SETTINGS", "HELP"};
    int two = ui.page == PG_PERFORM || ui.page == PG_LIVE || ui.page == PG_MUTATE, three = ui.page == PG_PATCHES || ui.page == PG_SAVE || ui.page == PG_HELP || (ui.page == PG_SETTINGS && !ui.diag);
    int x = two ? header_tabs(t2, 3, ui.page == PG_LIVE ? 1 : ui.page == PG_MUTATE ? 2 : 0) : three ? header_tabs(t3, 4, ui.page == PG_PATCHES ? 0 : ui.page == PG_SAVE ? 1 : ui.page == PG_SETTINGS ? 2 : 3)
        : text(6, 3, ui.page == PG_SETTINGS && ui.diag ? "DIAGNOSTICS" : ui.page == PG_SEQ && find_seq() >= 0 && pws.mod[find_seq()].type == PW_T_PROLL ? "PIANO ROLL" : page_title[ui.page], F_M, C_TEXT);
    int name_edge = W - 96;   /* where the patch name must stop: the left end of what is on the right (S, ALL, REC, GLO) */
    if (home_stats_now() && !(ui.tempo_ms && fm1_millis() - ui.tempo_ms < 1500u)) {   /* CPU and battery, over everything but the play light and the meters (not while the tempo is being turned: the normal bar shows it) */
        unsigned cpu = pw_plat_cpu();
        int bp = bat_pct(); unsigned lvl = fm1_battery_level();
        fill(0, 0, W - 46, HDR, C_PANEL);
        if (cpu > 100u) header_stat(8, "CPU", 0, C_GREEN, "--");
        else { s_cpy(s_int(buf, (int)cpu), "%"); header_stat(8, "CPU", cpu, cpu > 85u ? C_RED : cpu > 70u ? C_AMBER : C_GREEN, buf); }
        if (bp > 100) header_stat(104, "BAT", 100, C_GREEN, "USB");
        else if (bp < 0) header_stat(104, "BAT", 0, C_GREEN, "--");
        else { s_cpy(s_int(buf, bp), "%"); header_stat(104, "BAT", (unsigned)bp, lvl == 0 ? C_RED : lvl == 1 ? C_AMBER : C_GREEN, buf); }
        x = W;
    } else {
    s_int(buf, (int)(pws.bpm + 0.5f));
    int bx = text_w(buf, F_S);
    if (!three) text_right(W - 46, 5, buf, F_S, ui.tempo_ms && fm1_millis() - ui.tempo_ms < 1500u ? C_AMBER : C_DIM);   /* (the four menu tabs fill the bar: the tempo is on their pages) */
    if (!three && !two) {   /* (the three tabs leave room for the tempo only) */
        char vb[8] = {'S', 0}; s_int(vb + 1, pws.var_cur + 1);
        int song = pws.song.on;   /* SONG is on (it plays the snapshots): their label is underlined, quietly */
        text_right(W - 52 - bx, 5, vb, F_S, pws.var_cur || song ? C_AMBER : C_LINE);
        if (song) fill(W - 52 - bx - text_w(vb, F_S), 16, text_w(vb, F_S), 1, C_AMBER);
        int lx = W - 52 - bx - text_w(vb, F_S) - 4;
        if (pws.edit_all) { text_right(lx, 5, "ALL", F_S, C_RED); lx -= text_w("ALL", F_S) + 4; }   /* knob edits reach every snapshot that shares the value */
        if (ui.song_rec) { rec_dot(lx - 7, 6); lx -= 12; }   /* SONG recording is armed */
        if (ui.glo_on) { text_right(lx, 5, "GLO", F_S, C_AMBER); lx -= text_w("GLO", F_S) + 4; }   /* GLO is latched: the next key is a GLO key */
        if (ui.kbd_eff) { text_right(lx, 5, "BTN", F_S, C_AMBER); lx -= text_w("BTN", F_S) + 4; }
        if (pws.metro) { text_right(lx, 5, "MET", F_S, pws.playing && ((pws.sixteenths - 1u) & 3u) == 0u ? C_WHITE : C_AMBER); lx -= text_w("MET", F_S) + 4; }   /* the metronome is on (it flashes on the beat) */   /* the keys are the BUTTON modules' */
        name_edge = lx - 2;
    }
    }
    if (!two && !three && ui.page != PG_SETTINGS && name_edge > x + 16)
        text_fit(x + 8, 5, pws.name, F_S, C_DIM, name_edge - (x + 8));
    if (pws.playing) {
        for (int i = 0; i < 11; ++i) fill(W - 40, 4 + i, 1 + (i < 6 ? i : 10 - i), 1, C_GREEN);
    } else fill(W - 40, 5, 9, 9, C_DIM);
    /* output meter */
    int l = (int)(pws.out_peak_l * 20.0f), r = (int)(pws.out_peak_r * 20.0f);
    fill(W - 26, 4, 20, 4, C_PANEL2); fill(W - 26, 11, 20, 4, C_PANEL2);
    fill(W - 26, 4, clampi(l, 0, 20), 4, l > 18 ? C_RED : C_GREEN);
    fill(W - 26, 11, clampi(r, 0, 20), 4, r > 18 ? C_RED : C_GREEN);
}
static void header(void)
{
    strip_buf[0] = 0; strip_right[0] = 0; strip_pre = 0; strip_post = 0; strip_chg = 0;
    header_draw();
}
/* one knob cell: name, value, bar */
/* a value too wide for its cell (a preset: "THUNDER SHEET") goes on two lines, split at the space nearest the middle: false when it has none */
static int value_split(const char *v, char *a, char *b)
{
    int n = 0, sp = -1, best = 99;
    while (v[n]) ++n;
    for (int i = 1; i < n - 1; ++i) if (v[i] == ' ') { int d = i * 2 > n ? i * 2 - n : n - i * 2; if (d < best) { best = d; sp = i; } }
    if (sp < 0) return 0;
    for (int i = 0; i < sp; ++i) a[i] = v[i];
    a[sp] = 0;
    for (int i = sp + 1; i <= n; ++i) b[i - sp - 1] = v[i];
    return 1;
}
static void knob_cell(int k, const char *name, const char *value, float norm, uint16_t color)
{
    int x = 1 + k * 60;
    rfill(x, KY, 58, KH, C_PANEL);
    if (!name) return;
    text_center(x + 29, KY + 1, name, F_S, C_DIM);
    char l1[16], l2[16];
    if (text_w(value, F_S) > 56 && value_split(value, l1, l2)) {   /* two lines */
        text_center(x + 29, KY + 11, l1, F_XS, C_TEXT); text_center(x + 29, KY + 20, l2, F_XS, C_TEXT);
    } else {
        int f = text_w(value, F_M) <= 56 ? F_M : F_S;
        text_center(x + 29, f == F_M ? KY + 13 : KY + 15, value, f, C_TEXT);
    }
    if (norm >= 0) {
        fill(x + 5, KY + 29, 48, 3, C_PANEL2);
        fill(x + 5, KY + 29, clampi((int)(norm * 48.0f + 0.5f), 0, 48), 3, color);
    }
}
static void knob_row(int slot, unsigned page)
{
    if (slot >= 0) { fix_page(slot); page = ui.param_page[slot]; }
    for (unsigned k = 0; k < 4; ++k) {
        if (slot < 0) { knob_cell((int)k, 0, 0, -1, 0); continue; }
        const pw_type *t = &pw_types[pws.mod[slot].type];
        unsigned p = page * 4 + k;
        if (p >= t->n_param) { knob_cell((int)k, 0, 0, -1, 0); continue; }
        p = pw_param_at(pws.mod[slot].type, p);
        char v[16];
        pw_format_param((unsigned)slot, p, v);
        knob_cell((int)k, t->param[p].name, v, param_norm((unsigned)slot, p), cat_color[t->cat]);
        { int lm; unsigned ll; if (pw_mseq_lock_find((unsigned)slot, p, &lm, &ll)) text(1 + (int)k * 60 + 49, KY + 2, "*", F_XS, C_AMBER); }   /* (it follows a MOD SEQ lane) */
    }
    if (slot >= 0) {   /* more knobs than these: a chevron at the edge on the side they are (the ALGO knob turns that way) */
        unsigned pages = param_pages(slot);
        uint16_t cc = cat_color[pw_types[pws.mod[slot].type].cat];
        int cy = KY + KH / 2 - 1;
        if (page + 1 < pages) { line(233, cy - 4, 236, cy, 2, cc); line(236, cy, 233, cy + 4, 2, cc); }
        if (page > 0) { line(6, cy - 4, 3, cy, 2, cc); line(3, cy, 6, cy + 4, 2, cc); }
    }
}
static int notice_live(void)   /* a notice, drawn in the strip (strip_draw), for a few seconds; false once it has run out */
{
    if (!ui.notice) return 0;
    if ((int32_t)(fm1_millis() - ui.notice_until) >= 0) { ui.notice = 0; return 0; }
    return 1;
}
static int key_tag(int x, int y, const char *k, uint16_t c);
static const char *pw_has(const char *h, const char *n)   /* strstr, which the firmware's libc lacks */
{
    for (; *h; ++h) { const char *a = h, *b = n; while (*a && *b && *a == *b) { ++a; ++b; } if (!*b) return h; }
    return 0;
}
/* the knob page and its shortcut, right-aligned at y: where it begins (W when one page, or when it would not fit right of minx) */
static int page_hint(int slot, int y, int minx, uint16_t bg)
{
    unsigned pages = param_pages(slot);
    if (pages < 2) return W;
    int px = W - 4 - page_tabs_w(pages);
    if (px < minx) return W;
    page_tabs(px, y, pages, ui.param_page[slot], bg);
    return px;
}
/* wrap s to max_w pixels, at most `lines` lines from y */
static int wrap_text(int x, int y, const char *s, int font, uint16_t c, int max_w, int lines, int lh)
{
    int n = 0;
    while (*s && n < lines) {
        char buf[64]; int len = 0, cut = 0;
        while (s[len] && len < 63) {
            buf[len] = s[len]; buf[len + 1] = 0;
            if (text_w(buf, font) > max_w) break;
            if (s[len] == ' ') cut = len;
            ++len;
        }
        if (s[len] && cut) len = cut;
        buf[len] = 0;
        if (n == lines - 1 && s[len]) text_fit(x, y, s, font, c, max_w);
        else text(x, y, buf, font, c);
        s += len; while (*s == ' ') ++s;
        y += lh; ++n;
    }
    return y;
}

/* how many lines wrap_text would take (the add list's descriptions must fit: tests ask through pw_ui_help_lines) */
static int wrap_lines(const char *s, int font, int max_w)
{
    int n = 0;
    while (*s) {
        char buf[64]; int len = 0, cut = 0;
        while (s[len] && len < 63) {
            buf[len] = s[len]; buf[len + 1] = 0;
            if (text_w(buf, font) > max_w) break;
            if (s[len] == ' ') cut = len;
            ++len;
        }
        if (s[len] && cut) len = cut;
        s += len; while (*s == ' ') ++s;
        ++n;
    }
    return n;
}
#define ADD_HELP_X 112           /* the add list's description: where, how wide, how many lines */
#define ADD_HELP_W (W - ADD_HELP_X - 4)
#define ADD_HELP_LINES 8
#define ADD_EX_LINES 7
#define ADD_HELPS_LINES 8       /* under the patch strip: smaller text (F_XS), more line spacing */
#define ADD_EXS_LINES 7
#define ADD_LH 11
int pw_ui_help_lines(unsigned type) { return type < PW_T_COUNT ? wrap_lines(pw_type_help(type), F_S, ADD_HELP_W) : 0; }
int pw_ui_help_lines_xs(unsigned type) { return type < PW_T_COUNT ? wrap_lines(pw_type_help(type), F_XS, ADD_HELP_W) : 0; }
int pw_ui_example_lines_xs(unsigned type) { const char *t = type < PW_T_COUNT ? pw_example_text(type) : 0; return t ? wrap_lines(t, F_XS, ADD_HELP_W) : 0; }
int pw_ui_example_lines(unsigned type) { const char *t = type < PW_T_COUNT ? pw_example_text(type) : 0; return t ? wrap_lines(t, F_S, ADD_HELP_W) : 0; }

/* ------------------------------------------------------------------ pages */
static void draw_boot(uint32_t el)   /* el: ms since power-on (the splash screen draws it finished) */
{
    const uint16_t c[5] = {cat_color[PW_C_SOURCE], cat_color[PW_C_FX], cat_color[PW_C_FILTER], cat_color[PW_C_MOD], C_AMBER};   /* (the chosen palette's: its first four groups and its accent) */
    static const uint8_t to[5] = {3, 4, 0, 1, 2};      /* which right-hand jack each left-hand jack is patched to */
    const int XA = 30, XB = 210, N = XB - XA, Y0 = 52, DY = 14;
    /* five patch cables from a column of jacks to another, each leaving level and settling into its place (a smooth S).
     * At power-on they are drawn in from left to right, one after another (each 110 ms after the one above). */
    int done[5];
    for (int i = 0; i < 5; ++i) {
        int ya = Y0 + i * DY, yb = Y0 + to[i] * DY;
        int64_t q = (int64_t)el - (int64_t)i * 110;
        int len = q <= 0 ? -1 : q >= 750 ? N : (int)(q * N / 750);
        done[i] = len >= N;
        for (int t = 0; t <= len; t += 2) {
            int s = 3 * t * t * N - 2 * t * t * t;                    /* smoothstep, times N cubed */
            fill(XA + t, ya + (yb - ya) * s / (N * N * N) - 1, 2, 3, c[i]);
        }
    }
    for (int i = 0; i < 5; ++i)                                         /* the plugs: the left one as its cable starts, the right one as it arrives */
        for (int side = 0; side < 2; ++side) {
            if ((int64_t)el <= (int64_t)i * 110 || (side && !done[i])) continue;
            int x = (side ? XB : XA) - 6, y = Y0 + (side ? to[i] : i) * DY - 6;
            fill(x + 2, y + 2, 8, 8, c[i]);
        }
    text_center(W / 2, 132, "PATCHWERK", F_L, C_WHITE);
    text_center(W / 2, 172, "a modular synth for the FM-1", F_S, C_DIM);
    text_center(W / 2, 192, PW_VERSION, F_S, C_AMBER);
}
/* a long job (a sample arriving over USB): its screen, sent at once */
void pw_ui_progress(const char *what, const char *detail, unsigned percent)
{
    n_prims = n_text = 0;
    fill(0, 0, W, H, C_BG);
    text_center(W / 2, 70, what, F_M, C_TEXT);
    if (detail) text_center(W / 2, 98, detail, F_S, C_DIM);
    rfill(20, 130, 200, 16, C_PANEL2);
    rfill(20, 130, clampi((int)(percent * 2u), 4, 200), 16, cat_color[PW_C_SOURCE]);
    char b[8]; char *o = s_int(b, (int)percent); s_cpy(o, "%");
    text_center(W / 2, 160, b, F_S, C_DIM);
    present();
    band_valid = 0;                /* the page is drawn whole again afterwards */
}
/* (the power-on splash is the mark at its first moment: pw_ui_init's boot page then draws the cables in) */
static uint32_t splash_ms, anim_ms;   /* anim_ms: how far the splash's animation has got (a frame is 20 ms: not read from a clock, which may not tick yet at power-on) */
static void splash_pal(void);   /* (the colors are the chosen palette's from the very first frame) */
void pw_ui_splash_frame(void)
{
    splash_pal();
    anim_ms += 20u;
    band_valid = 0;
    n_prims = n_text = 0;
    draw_boot(anim_ms);
    present();
}
void pw_ui_splash(void)
{
    splash_pal();
    splash_ms = fm1_millis();
    band_valid = 0;
    n_prims = n_text = 0;
    draw_boot(0);
    present();
}

/* ---------------------------------------------------------- patch view
 * The whole patch on one screen: modules as boxes arranged by signal flow
 * (sources left, outputs right), inputs on the left edge of each box,
 * outputs on the right, cables drawn between them. One cursor visits jacks:
 * SELECT steps jack to jack, GLO + SELECT module to module. SEL picks a cable
 * up at the cursor and SEL again plugs it in; REC unplugs. */
#define PV_TOP HDR
#define PV_BOTTOM STATUS_Y
/* COMPACT: for a big patch, boxes carry only their tag and the jacks are dots (the numbers below are the compact set) */
static int pv_cmp;                                /* the layout in use is the compact one */
#define PV_BOXW (pv_cmp ? 30 : 48)
#define PV_COLW (pv_cmp ? 38 : 62)
#define PV_JACK (pv_cmp ? 7 : 11)
#define PV_HEAD (pv_cmp ? 11 : 18)                /* a box's header, above its first jack row */
typedef struct { int16_t x, y, h; uint8_t e; } pv_box;   /* e: the lines under the name (a module on a preset names it: one line, or two for a long one) */
static pv_box pv[PW_MAX_MODULES];
static int pv_order[PW_MAX_MODULES], pv_n, pv_w, pv_h;
typedef struct { uint8_t slot, out, port; } pv_stop;
static pv_stop stops[PW_MAX_MODULES * 8];   /* (jacks beyond that on a crowded patch are left out of the cursor's round) */
static int n_stops;

static int pv_preset_lines(unsigned slot)   /* 0: no preset; 1: its name fits the box; 2: it takes two lines */
{
    const char *n = pw_module_preset_name(slot);
    char a[16], b[16];
    if (!n || pv_cmp) return 0;
    return text_w(n, F_XS) > PV_BOXW - 6 && value_split(n, a, b) ? 2 : 1;
}
/* Groups: a group is a view of the patch (the sound is the same). At the top a group is one box whose jacks are the ones wired to
 * something outside it; SEL-less, GLO + FX opens it: then its modules show, and the modules wired to them, dimmed. */
static int grp_size(int slot)   /* the modules in the group a module is in (1: alone) */
{
    unsigned g = pws.mod[slot].group; int n = 0;
    if (!g) return 1;
    for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1 && pws.mod[i].group == g) ++n;
    return n < 2 ? 1 : n;
}
static uint8_t pv_eg[PW_MAX_MODULES];             /* each module's group, 0 when it is alone in it */
static uint8_t pv_rep[PW_MAX_MODULES];            /* the module whose slot stands for the box a module is drawn in; 0xff: not shown */
static uint8_t pv_fl[PW_MAX_MODULES];             /* bit 0: drawn inside a collapsed group's box; bit 1: a neighbour of the group opened, drawn dim */
static uint16_t pv_bin[PW_MAX_MODULES], pv_bout[PW_MAX_MODULES];   /* in a collapsed box: the jacks that have a row (the ones wired outside), a bit each */
#define pv_col_of(i) ((pv_fl[i] & 1u) != 0)
#define pv_dim_of(i) ((pv_fl[i] & 2u) != 0)
/* a collapsed group's knobs: every page of every member, one after the other (ALGORITHM turns through them) */
static uint8_t sp_idx, sp_grp;
static int sp_member(int slot, unsigned *page, unsigned *idx, unsigned *total)
{
    unsigned n = 0, at = 0; int found = slot; unsigned fpage = 0;
    if (slot < 0 || !pv_col_of(slot)) { if (idx) *idx = 0; if (total) *total = 0; if (page) *page = slot >= 0 ? ui.param_page[slot] : 0; return slot; }
    unsigned g = pws.mod[slot].group;
    if (sp_grp != g) { sp_grp = (uint8_t)g; sp_idx = 0; }
    for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1 && pws.mod[i].group == g) n += param_pages(i);
    if (sp_idx >= n) sp_idx = n ? (uint8_t)(n - 1) : 0;
    for (int i = 0; i < PW_MAX_MODULES; ++i) {
        if (pws.mod[i].used != 1 || pws.mod[i].group != g) continue;
        unsigned pg = param_pages(i);
        if (sp_idx >= at && sp_idx < at + pg) { found = i; fpage = sp_idx - at; }
        at += pg;
    }
    fix_page(found); ui.param_page[found] = fpage;
    if (idx) *idx = sp_idx; if (total) *total = n; if (page) *page = fpage;
    return found;
}
static void sp_turn(int slot, int d)
{
    unsigned idx, total; sp_member(slot, 0, &idx, &total);
    if (!d || !total) return;
    sp_idx = (uint8_t)clampi((int)idx + d, 0, (int)total - 1);
}
static int pv_mode;                               /* 1: the whole patch, no groups (the matrix, MUTES) */
static void pv_layout_do(void)
{
    int depth[PW_MAX_MODULES], linked[PW_MAX_MODULES], vis[PW_MAX_MODULES];
    unsigned gcount[PW_MAX_GROUP + 1];
    int gv = pv_mode ? 0 : ui.gview, collapse = !pv_mode && !gv;
    memset(gcount, 0, sizeof gcount);
    for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1 && pws.mod[i].group <= PW_MAX_GROUP) ++gcount[pws.mod[i].group];
    for (int i = 0; i < PW_MAX_MODULES; ++i) {
        unsigned g = pws.mod[i].used == 1 ? pws.mod[i].group : 0;
        pv_eg[i] = (uint8_t)(!pv_mode && g && g <= PW_MAX_GROUP && gcount[g] >= 2 ? g : 0);
        depth[i] = 0; linked[i] = 0; vis[i] = 0; pv_rep[i] = 0xff; pv_fl[i] = 0; pv_bin[i] = pv_bout[i] = 0;
    }
    if (gv && !gcount[gv]) { gv = ui.gview = 0; collapse = !pv_mode; }
    if (gv && gcount[gv] < 2) { gv = ui.gview = 0; collapse = !pv_mode; }
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c)
        if (pws.cable[c].used) { linked[pws.cable[c].sm] = 1; linked[pws.cable[c].dm] = 1; }
    for (int iter = 0; iter < PW_MAX_MODULES; ++iter) {
        int changed = 0;
        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
            const pw_cable *k = &pws.cable[c];
            if (!k->used || k->sm == k->dm || pws.mod[k->dm].type == PW_T_OUTPUT) continue;
            int d = depth[k->sm] + 1;
            if (d <= 6 && depth[k->dm] < d) { depth[k->dm] = d; changed = 1; }
        }
        if (!changed) break;
    }
    int maxd = 0;
    for (int i = 0; i < PW_MAX_MODULES; ++i) {
        const pw_module *m = &pws.mod[i];
        if (m->used != 1 || m->type == PW_T_OUTPUT) continue;
        if (!linked[i]) { unsigned c = pw_types[m->type].cat; depth[i] = (c == PW_C_FILTER || c == PW_C_FX || c == PW_C_UTIL) ? 1 : 0; }
        if (depth[i] > maxd) maxd = depth[i];
    }
    for (int i = 0; i < PW_MAX_MODULES; ++i)
        if (pws.mod[i].used == 1 && pws.mod[i].type == PW_T_OUTPUT) depth[i] = maxd + 1;
    /* what shows: everything; or the group opened and what is wired to it (dim) */
    for (int i = 0; i < PW_MAX_MODULES; ++i) vis[i] = pws.mod[i].used == 1 && (!gv || pv_eg[i] == gv);
    if (gv) {
        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
            const pw_cable *k = &pws.cable[c];
            if (!k->used) continue;
            if (pv_eg[k->sm] == gv && !vis[k->dm]) { vis[k->dm] = 1; pv_fl[k->dm] |= 2u; }
            if (pv_eg[k->dm] == gv && !vis[k->sm]) { vis[k->sm] = 1; pv_fl[k->sm] |= 2u; }
        }
        if (ui.carrying && ui.carry_slot >= 0 && ui.carry_slot < PW_MAX_MODULES && pws.mod[ui.carry_slot].used == 1 && !vis[ui.carry_slot]) { vis[ui.carry_slot] = 1; pv_fl[ui.carry_slot] |= 2u; }   /* (the cable in hand starts there) */
    }
    if (collapse) {   /* a group stands in one column: that of its deepest module */
        int gd[PW_MAX_GROUP + 1];
        for (int g = 0; g <= PW_MAX_GROUP; ++g) gd[g] = -1;
        for (int i = 0; i < PW_MAX_MODULES; ++i) if (pv_eg[i] && depth[i] > gd[pv_eg[i]]) gd[pv_eg[i]] = depth[i];
        for (int i = 0; i < PW_MAX_MODULES; ++i) if (pv_eg[i]) depth[i] = gd[pv_eg[i]];
    }
    pv_n = 0; pv_w = 0; pv_h = 0; n_stops = 0;
    int cx = 0;
    for (int col = 0; col <= maxd + 1; ++col) {
        int y = 5, any = 0;
        for (int i = 0; i < PW_MAX_MODULES; ++i) {
            const pw_module *m = &pws.mod[i];
            if (!vis[i] || depth[i] != col) continue;
            if (collapse && pv_eg[i]) {   /* a collapsed group: one box, drawn for its first module here */
                int g = pv_eg[i], first = 1;
                for (int j = 0; j < i; ++j) if (vis[j] && pv_eg[j] == g) first = 0;
                if (!first) continue;
                int nin = 0, nout = 0;
                for (int j = 0; j < PW_MAX_MODULES; ++j) {
                    if (pv_eg[j] != g) continue;
                    pv_fl[j] |= 1u; pv_rep[j] = (uint8_t)i;
                    const pw_type *t = &pw_types[pws.mod[j].type];
                    for (unsigned q = 0; q < t->n_in; ++q)
                        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) { const pw_cable *k = &pws.cable[c]; if (k->used && k->dm == j && k->di == q && pv_eg[k->sm] != g) { pv_bin[j] |= (uint16_t)(1u << q); ++nin; break; } }
                    for (unsigned q = 0; q < t->n_out; ++q)
                        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) { const pw_cable *k = &pws.cable[c]; if (k->used && k->sm == j && k->so == q && pv_eg[k->dm] != g) { pv_bout[j] |= (uint16_t)(1u << q); ++nout; break; } }
                    if (ui.carrying && ui.carry_slot == j) {   /* a cable in hand that starts inside: its jack is on the box (a new input or output of the group once it is plugged in) */
                        if (ui.carry_out && (unsigned)ui.carry_port < t->n_out) pv_bout[j] |= (uint16_t)(1u << ui.carry_port);
                        else if (!ui.carry_out && (unsigned)ui.carry_port < t->n_in) pv_bin[j] |= (uint16_t)(1u << ui.carry_port);
                    }
                }
                {   /* a group needs its interface even before anything is wired: the free V/OCT, GATE, IN, TRIG, CLOCK inputs, and the outputs of the members that feed no other member */
                    static const char *const ifn[] = {"V/OCT", "GATE", "IN", "IN L", "TRIG", "CLOCK"};
                    for (int j = 0; j < PW_MAX_MODULES; ++j) {
                        if (pv_eg[j] != g) continue;
                        const pw_type *t = &pw_types[pws.mod[j].type];
                        for (unsigned q = 0; q < t->n_in; ++q) {
                            if (pw_input_cables((unsigned)j, q) > 0) continue;
                            for (unsigned w = 0; w < 6; ++w) { const char *a1 = t->in[q], *b1 = ifn[w]; while (*a1 && *a1 == *b1) { ++a1; ++b1; } if (!*a1 && !*b1) { pv_bin[j] |= (uint16_t)(1u << q); break; } }
                        }
                        int terminal = 1;
                        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) { const pw_cable *k = &pws.cable[c]; if (k->used && k->sm == j && pv_eg[k->dm] == g) terminal = 0; }
                        if (terminal) for (unsigned q = 0; q < t->n_out; ++q) pv_bout[j] |= (uint16_t)(1u << q);
                        nin = nout = 0;
                    }
                    for (int j = 0; j < PW_MAX_MODULES; ++j) if (pv_eg[j] == g) { nin += __builtin_popcount(pv_bin[j]); nout += __builtin_popcount(pv_bout[j]); }
                }
                if (!nin && !nout) {   /* nothing wired outside: one jack, so the box can be reached */
                    const pw_type *t = &pw_types[pws.mod[i].type];
                    if (t->n_in) { pv_bin[i] = 1; nin = 1; } else if (t->n_out) { pv_bout[i] = 1; nout = 1; }
                }
                int rows = nin > nout ? nin : nout, h = PV_HEAD + rows * PV_JACK;
                if (h < (pv_cmp ? 18 : 28)) h = pv_cmp ? 18 : 28;
                for (int j = 0; j < PW_MAX_MODULES; ++j) {
                    if (pv_eg[j] != g) continue;
                    pv[j].x = (int16_t)(cx * PV_COLW + 6); pv[j].y = (int16_t)y; pv[j].h = (int16_t)h; pv[j].e = 0;
                }
                y += h + 8; any = 1;
                pv_order[pv_n++] = i;
                for (int j = 0; j < PW_MAX_MODULES; ++j) {
                    if (pv_eg[j] != g) continue;
                    for (unsigned q = 0; q < PW_MAX_IN; ++q) if ((pv_bin[j] >> q & 1u) && n_stops < (int)(sizeof stops / sizeof stops[0])) { stops[n_stops].slot = (uint8_t)j; stops[n_stops].out = 0; stops[n_stops].port = (uint8_t)q; ++n_stops; }
                }
                for (int j = 0; j < PW_MAX_MODULES; ++j) {
                    if (pv_eg[j] != g) continue;
                    for (unsigned q = 0; q < PW_MAX_OUT; ++q) if ((pv_bout[j] >> q & 1u) && n_stops < (int)(sizeof stops / sizeof stops[0])) { stops[n_stops].slot = (uint8_t)j; stops[n_stops].out = 1; stops[n_stops].port = (uint8_t)q; ++n_stops; }
                }
                if (cx * PV_COLW + 6 + PV_BOXW + 8 > pv_w) pv_w = cx * PV_COLW + 6 + PV_BOXW + 8;
                if (y > pv_h) pv_h = y;
                continue;
            }
            const pw_type *t = &pw_types[m->type];
            int rows = t->n_in > t->n_out ? t->n_in : t->n_out;
            int e = pv_preset_lines((unsigned)i), g = e ? 3 + (e == 2) : 0, h = PV_HEAD + g + e * 9 + rows * PV_JACK;   /* (g: air between the module's name and its preset's) */
            if (h < (pv_cmp ? 18 : 28) + g + e * 9) h = (pv_cmp ? 18 : 28) + g + e * 9;
            pv[i].e = (uint8_t)e; pv_rep[i] = (uint8_t)i; any = 1;
            pv[i].x = (int16_t)(cx * PV_COLW + 6); pv[i].y = (int16_t)y; pv[i].h = (int16_t)h;
            y += h + 8;
            pv_order[pv_n++] = i;
            for (unsigned k = 0; k < t->n_in && n_stops < (int)(sizeof stops / sizeof stops[0]); ++k) { stops[n_stops].slot = (uint8_t)i; stops[n_stops].out = 0; stops[n_stops].port = (uint8_t)k; ++n_stops; }
            for (unsigned k = 0; k < t->n_out && n_stops < (int)(sizeof stops / sizeof stops[0]); ++k) { stops[n_stops].slot = (uint8_t)i; stops[n_stops].out = 1; stops[n_stops].port = (uint8_t)k; ++n_stops; }
            if (pv[i].x + PV_BOXW + 8 > pv_w) pv_w = pv[i].x + PV_BOXW + 8;
            if (y > pv_h) pv_h = y;
        }
        if (any) ++cx;
    }
}
static void pv_layout(void)   /* the patch view's layout: full size, or compact (chosen, or when the full one does not fit the screen) */
{
    pv_cmp = ui.compact == 1 && !pv_mode;
    pv_layout_do();
    if (!pv_cmp && ui.cmp_auto && ui.compact == 0 && !pv_mode && (pv_n > 10 || pv_h > (PV_BOTTOM - PV_TOP) * 2)) { pv_cmp = 1; pv_layout_do(); }
}
static void pv_layout_flat(void) { pv_mode = 1; pv_layout(); pv_mode = 0; }   /* every module, groups ignored (the matrix, MUTES) */
static int pv_box_of(int slot) { return pv_col_of(slot) ? pv_rep[slot] : slot; }
static int pv_row_of(int slot, int out, int port)   /* a collapsed box's jack: its row (the wired-outside jacks of its modules, in slot order) */
{
    const uint16_t *mk = out ? pv_bout : pv_bin;
    int g = pv_eg[slot], r = 0;
    for (int j = 0; j < slot; ++j) if (pv_eg[j] == g) r += __builtin_popcount(mk[j]);
    return r + __builtin_popcount(mk[slot] & ((1u << port) - 1u));
}
static int pv_has(int slot, int out, int port)   /* is that jack on the screen */
{
    if (slot < 0 || slot >= PW_MAX_MODULES || pv_rep[slot] == 0xff) return 0;
    if (pv_col_of(slot)) return port < (out ? PW_MAX_OUT : PW_MAX_IN) && ((out ? pv_bout[slot] : pv_bin[slot]) >> port & 1u);
    return 1;
}
static void pv_jack(int slot, int out, int port, int *x, int *y)
{
    int row = port;
    if (pv_col_of(slot)) row = pv_row_of(slot, out, port);
    *x = pv[slot].x + (out ? PV_BOXW : 0);
    *y = pv[slot].y + (pv_cmp ? 14 : 20) + (pv[slot].e ? 3 + (pv[slot].e == 2) : 0) + pv[slot].e * 9 + row * PV_JACK;
}
static int pv_compatible(const pv_stop *s) { return !ui.carrying || s->out != ui.carry_out || pv_col_of(s->slot); }   /* (a group's box is always a place to stop with a cable in hand: SEL or GLO + FX there opens it, to plug the cable in inside) */
static int pv_index(void)
{
    for (int i = 0; i < n_stops; ++i)
        if (stops[i].slot == ui.cur_slot && stops[i].out == ui.cur_out && stops[i].port == ui.cur_port) return i;
    return -1;
}
static void pv_set(int i)
{
    if (i < 0 || i >= n_stops) return;
    ui.cur_slot = stops[i].slot; ui.cur_out = stops[i].out; ui.cur_port = stops[i].port;
    ui.sel = ui.cur_slot;
    if (ui.cur_out) { ui.probe_slot = ui.cur_slot; ui.probe_port = ui.cur_port; }
}
static void pv_fix_cursor(void)
{
    if (pv_index() >= 0) return;
    for (int i = 0; i < n_stops; ++i) if (stops[i].slot == ui.cur_slot && pv_compatible(&stops[i])) { pv_set(i); return; }
    for (int i = 0; i < n_stops; ++i) if (pv_compatible(&stops[i])) { pv_set(i); return; }
    if (n_stops) pv_set(0);
}
static void pv_step_jack(int delta)
{
    int i = pv_index();
    if (i < 0) { pv_fix_cursor(); return; }
    int dir = delta > 0 ? 1 : -1, n = delta > 0 ? delta : -delta;
    while (n--) {
        int j = i + dir;
        while (j >= 0 && j < n_stops && !pv_compatible(&stops[j])) j += dir;
        if (j < 0 || j >= n_stops) break;
        i = j;
    }
    pv_set(i);
}
static void pv_step_module(int delta)
{
    int at = 0;
    for (int k = 0; k < pv_n; ++k) if (pv_order[k] == pv_box_of(ui.cur_slot)) at = k;
    int dir = delta > 0 ? 1 : -1, n = delta > 0 ? delta : -delta;
    while (n--) {
        int k = at + dir;
        for (; k >= 0 && k < pv_n; k += dir) {
            int ok = 0;
            for (int i = 0; i < n_stops && !ok; ++i) ok = pv_box_of(stops[i].slot) == pv_order[k] && pv_compatible(&stops[i]);
            if (ok) break;
        }
        if (k < 0 || k >= pv_n) break;
        at = k;
    }
    for (int i = 0; i < n_stops; ++i)
        if (pv_box_of(stops[i].slot) == pv_order[at] && pv_compatible(&stops[i])) { pv_set(i); return; }
}
/* the cursor onto a jack (after an edit from elsewhere: the cable list, the module page) */
static void pv_goto(int slot, int out, int port)
{
    pv_layout();
    ui.cur_slot = slot; ui.cur_out = out; ui.cur_port = port; ui.sel = slot;
    if (pv_index() < 0 && slot >= 0 && slot < PW_MAX_MODULES && pws.mod[slot].used == 1) {   /* hidden in this view: open its group, or the top */
        ui.gview = pv_eg[slot] ? pv_eg[slot] : 0;
        pv_layout();
    }
    if (pv_index() < 0) { ui.cur_out = !out; ui.cur_port = 0; }
    pv_fix_cursor();
}
static void pv_open_group(void)   /* the group the cursor is on, opened (a cable in hand goes in with you, to a jack it fits) */
{
    int sl = ui.cur_slot, o = ui.cur_out, pt = ui.cur_port;
    ui.gview = pv_eg[sl]; pv_goto(sl, o, pt);
    if (ui.carrying) {
        pv_step_jack(1);
        int i = pv_index();
        if (i >= 0 && !pv_compatible(&stops[i])) pv_step_jack(-1);
    }
}
static void pv_cable_curve(int x0, int y0, int x1, int y1, int vx, int vy, int thick, uint16_t c)
{
    int dx = x1 - x0, adx = dx < 0 ? -dx : dx, ady = y1 > y0 ? y1 - y0 : y0 - y1;
    int sag = 5 + adx / 5 + ady / 6;
    if (sag > 28) sag = 28;
    /* cubic bezier, 8 segments, control points dropped by `sag` */
    float ax = (float)x0, ay = (float)y0, bx = x0 + dx * 0.3f + (dx < 0 ? 18.0f : 0.0f), by = (float)(y0 + sag);
    float cx = x1 - dx * 0.3f - (dx < 0 ? 18.0f : 0.0f), cy = (float)(y1 + sag), ex = (float)x1, ey = (float)y1;
    int px = x0 - vx, py = y0 - vy + PV_TOP;
    for (int i = 1; i <= 8; ++i) {
        float t = i / 8.0f, u = 1.0f - t;
        float x = u * u * u * ax + 3 * u * u * t * bx + 3 * u * t * t * cx + t * t * t * ex;
        float y = u * u * u * ay + 3 * u * u * t * by + 3 * u * t * t * cy + t * t * t * ey;
        int qx = (int)(x + 0.5f) - vx, qy = (int)(y + 0.5f) - vy + PV_TOP;
        if (!((px < 0 && qx < 0) || (px >= W && qx >= W) || (py < PV_TOP && qy < PV_TOP) || (py >= PV_BOTTOM && qy >= PV_BOTTOM)))
            line(px, py, qx, qy, thick, c);
        px = qx; py = qy;
    }
}
/* --- grouping: chains that could be a group (the sources going into an output or a mixer, with what only they use), a dial through them */
#if PW_MAX_MODULES <= 32
typedef uint32_t grp_set;
#else
typedef unsigned long long grp_set;
#endif
static grp_set grp_cand[8];
static int grp_cands(int unused)
{
    (void)unused;
    int n = 0;
    for (int sl = 0; sl < PW_MAX_MODULES && n < 8; ++sl) {
        if (pws.mod[sl].used != 1 || pv_eg[sl]) continue;
        unsigned ty = pws.mod[sl].type;
        if (ty == PW_T_OUTPUT || ty == PW_T_STEREO || ty == PW_T_MIX8) continue;
        int feeds = 0;
        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
            const pw_cable *k = &pws.cable[c];
            if (!k->used || k->sm != sl) continue;
            unsigned dt = pws.mod[k->dm].type;
            if (dt == PW_T_OUTPUT || dt == PW_T_STEREO || dt == PW_T_MIX8) feeds = 1;
        }
        if (!feeds) continue;
        grp_set set = (grp_set)1 << sl;
        for (int again = 1; again;) {   /* whatever feeds the set and feeds nothing else joins it */
            again = 0;
            for (int u = 0; u < PW_MAX_MODULES; ++u) {
                if (pws.mod[u].used != 1 || (set >> u & 1u) || pv_eg[u]) continue;
                unsigned ut = pws.mod[u].type;
                if (ut == PW_T_OUTPUT || ut == PW_T_STEREO || ut == PW_T_MIX8) continue;
                int into = 0, other = 0;
                for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
                    const pw_cable *k = &pws.cable[c];
                    if (!k->used || k->sm != u) continue;
                    if (set >> k->dm & 1u) into = 1; else other = 1;
                }
                if (into && !other) { set |= (grp_set)1 << u; again = 1; }
            }
        }
        if (set & (set - 1)) {   /* two modules at least, and not the same set twice */
            int dup = 0; for (int q = 0; q < n; ++q) if (grp_cand[q] == set) dup = 1;
            if (!dup) grp_cand[n++] = set;
        }
    }
    return n;
}
static int grp_make(grp_set set)
{
    int g = 0;
    for (int id = 1; id <= PW_MAX_GROUP && !g; ++id) {
        int used = 0; for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1 && pws.mod[i].group == id) used = 1;
        if (!used) g = id;
    }
    if (!g) return 0;
    pw_undo_group(1);
    for (int i = 0; i < PW_MAX_MODULES; ++i) if ((set >> i & 1u) && pws.mod[i].used == 1) pw_set_group((unsigned)i, (unsigned)g);
    pw_undo_group(0);
    return g;
}
static void grp_ungroup(int slot)
{
    unsigned g = pws.mod[slot].group;
    if (!g) return;
    pw_undo_group(1);
    for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1 && pws.mod[i].group == g) pw_set_group((unsigned)i, 0);
    pw_undo_group(0);
}
static void pv_to_box(int slot)   /* the cursor onto a jack of the box the module is drawn in */
{
    pv_layout();
    for (int i = 0; i < n_stops; ++i) if (pv_box_of(stops[i].slot) == pv_box_of(slot) && (!ui.carrying || stops[i].out != ui.carry_out)) { pv_set(i); return; }   /* (a jack the cable in hand plugs into) */
    for (int i = 0; i < n_stops; ++i) if (pv_box_of(stops[i].slot) == pv_box_of(slot)) { pv_set(i); return; }
    pv_fix_cursor();
}
static void draw_patchview(void)
{
    char buf[48], b2[28];
    ky_off = pv_n ? 11 : 0;   /* (the strip has two rows here) */
    pv_layout();
    if (!pv_n) {
        text_center(W / 2, 56, "EMPTY PATCH", F_M, C_TEXT);
        strip_set("FX|Add a module|PRESETS|Load a patch");
        knob_row(-1, 0);
        return;
    }
    pv_fix_cursor();
    int cx, cy;
    pv_jack(ui.cur_slot, ui.cur_out, ui.cur_port, &cx, &cy);
    int vw = W, vh = PV_BOTTOM - PV_TOP;   /* (the add list open: the cursor kept above it) */
    if (cx - ui.view_x < 30) ui.view_x = cx - 30;
    if (cx - ui.view_x > vw - 30) ui.view_x = cx - (vw - 30);
    if (cy - ui.view_y < 24) ui.view_y = cy - 24;
    if (cy - ui.view_y > vh - 14) ui.view_y = cy - (vh - 14);
    {   /* the whole box the cursor is on in view when it fits (a module's input jack is at its left edge: its outputs were cut off) */
        int bx0 = pv[ui.cur_slot].x, bx1 = bx0 + PV_BOXW, by0 = pv[ui.cur_slot].y, by1 = by0 + pv[ui.cur_slot].h;
        if (bx1 + 6 - ui.view_x > vw) ui.view_x = bx1 + 6 - vw;
        if (bx0 - 6 - ui.view_x < 0) ui.view_x = bx0 - 6;
        if (by1 + 4 - ui.view_y > vh) ui.view_y = by1 + 4 - vh;
        if (by0 - 4 - ui.view_y < 0) ui.view_y = by0 - 4;
    }
    if (ui.view_x > pv_w - vw) ui.view_x = pv_w - vw;
    if (ui.view_y > pv_h - vh) ui.view_y = pv_h - vh;
    if (ui.view_x < 0) ui.view_x = 0;
    if (ui.view_y < 0) ui.view_y = 0;
    int vx = ui.view_x, vy = ui.view_y;
    if (ui.gview) fill(0, PV_TOP, W, PV_BOTTOM - PV_TOP, C_SBG);
    /* boxes */
    grp_set sugg = 0;
    if (ui.gsug) { int n = grp_cands(0), i = n ? clampi(ui.gsug_i, 0, n - 1) : 0; if (n) sugg = grp_cand[i]; }
    for (int k = 0; k < pv_n; ++k) {
        int i = pv_order[k];
        const pw_module *m = &pws.mod[i];
        const pw_type *t = &pw_types[m->type];
        int x = pv[i].x - vx, y = pv[i].y - vy + PV_TOP, h = pv[i].h;
        if (x > W || x + PV_BOXW < 0 || y > PV_BOTTOM || y + h < PV_TOP) continue;
        if (pv_col_of(i)) {   /* a collapsed group */
            int here = pv_col_of(ui.cur_slot) && pv_rep[ui.cur_slot] == i;
            rfill(x, y, PV_BOXW, h, here ? C_SPANEL2 : C_SPANEL);
            fill(x + 1, y, PV_BOXW - 2, pv_cmp ? 2 : 3, C_AMBER);
            pw_module_tag_label((unsigned)i, buf);
            { char *a = buf, *o = buf; while (*a) { if (*a != ' ') *o++ = *a; ++a; } *o = 0; s_cpy(o, "+"); s_int(o + 1, grp_size(i) - 1); }
            text_fit(x + (pv_cmp ? 3 : 4), y + (pv_cmp ? 4 : 6), buf, F_XS, here ? C_WHITE : C_TEXT, PV_BOXW - (pv_cmp ? 4 : 6));
            if (!pv_cmp) text_fit(x + 4, y + 18, "GROUP", F_XS, C_AMBER, PV_BOXW - 6);
            for (int j = 0; j < PW_MAX_MODULES; ++j) {
                if (!pv_col_of(j) || pv_rep[j] != i) continue;
                const pw_type *tj = &pw_types[pws.mod[j].type];
                for (unsigned p = 0; p < tj->n_in; ++p) {
                    if (!(pv_bin[j] >> p & 1u)) continue;
                    int jx, jy; pv_jack(j, 0, (int)p, &jx, &jy);
                    fill(jx - vx - (pv_cmp ? 1 : 2), jy - vy + PV_TOP - (pv_cmp ? 1 : 2), pv_cmp ? 3 : 6, pv_cmp ? 3 : 6, pw_input_cables((unsigned)j, p) > 0 ? C_TEXT : C_LINE);
                }
                for (unsigned p = 0; p < tj->n_out; ++p) {
                    if (!(pv_bout[j] >> p & 1u)) continue;
                    int jx, jy; pv_jack(j, 1, (int)p, &jx, &jy);
                    fill(jx - vx - (pv_cmp ? 1 : 3), jy - vy + PV_TOP - (pv_cmp ? 1 : 2), pv_cmp ? 3 : 6, pv_cmp ? 3 : 6, pws.mod[j].level[p] > 30 ? cat_color[tj->cat] : C_LINE);
                }
            }
            continue;
        }
        int here = i == ui.cur_slot, byp = (m->flags & PW_F_BYPASS) != 0, dim = pv_dim_of(i);
        rfill(x, y, PV_BOXW, h, here ? C_PANEL2 : C_PANEL);
        fill(x + 1, y, PV_BOXW - 2, pv_cmp ? 2 : 3, byp || dim ? C_LINE : cat_color[t->cat]);
        if (sugg >> i & 1u) frame_rect(x - 1, y - 1, PV_BOXW + 2, h + 2, C_AMBER);
        if (byp) { if (pv_cmp) fill(x + PV_BOXW - 5, y + 3, 3, 3, C_AMBER); else text_center(x + PV_BOXW / 2, y + h - 11, "BYP", F_XS, C_AMBER); }
        pw_module_tag_label((unsigned)i, buf);
        { char *a = buf, *o = buf; while (*a) { if (*a != ' ') *o++ = *a; ++a; } *o = 0; } /* "VCF1" */
        text_fit(x + (pv_cmp ? 3 : 4), y + (pv_cmp ? 4 : 6), buf, F_XS, here ? C_WHITE : dim ? C_DIM : C_TEXT, PV_BOXW - (pv_cmp ? 4 : 6));
        if (pv[i].e) {   /* the preset, on a line of its own (two when it is long) */
            const char *pn = pw_module_preset_name((unsigned)i);
            char a[16], b[16];
            if (pv[i].e == 2 && value_split(pn, a, b)) { text_fit(x + 4, y + 18, a, F_XS, C_DIM, PV_BOXW - 6); text_fit(x + 4, y + 28, b, F_XS, C_DIM, PV_BOXW - 6); }
            else text_fit(x + 4, y + 18, pn, F_XS, C_DIM, PV_BOXW - 6);
        }
        if (m->status == PW_STATUS_NOMEM) { if (pv_cmp) fill(x + PV_BOXW - 5, y + 7, 3, 3, C_RED); else fill(x + PV_BOXW - 6, y + 6, 3, 9, C_RED); }
        for (unsigned p = 0; p < t->n_in; ++p) {
            int jx, jy; pv_jack(i, 0, (int)p, &jx, &jy);
            int on = pw_input_cables((unsigned)i, p) > 0;
            fill(jx - vx - (pv_cmp ? 1 : 2), jy - vy + PV_TOP - (pv_cmp ? 1 : 2), pv_cmp ? 3 : 6, pv_cmp ? 3 : 6, on ? C_TEXT : C_LINE);
        }
        for (unsigned p = 0; p < t->n_out; ++p) {
            int jx, jy; pv_jack(i, 1, (int)p, &jx, &jy);
            uint16_t c = m->level[p] > 30 ? cat_color[t->cat] : C_LINE;
            fill(jx - vx - (pv_cmp ? 1 : 3), jy - vy + PV_TOP - (pv_cmp ? 1 : 2), pv_cmp ? 3 : 6, pv_cmp ? 3 : 6, c);
        }
    }
    /* cables */
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (!k->used) continue;
        int x0, y0, x1, y1;
        if (!pv_has(k->sm, 1, k->so) || !pv_has(k->dm, 0, k->di)) continue;
        if (pv_col_of(k->sm) && pv_col_of(k->dm) && pv_rep[k->sm] == pv_rep[k->dm]) continue;   /* (inside a collapsed group) */
        pv_jack(k->sm, 1, k->so, &x0, &y0);
        pv_jack(k->dm, 0, k->di, &x1, &y1);
        int touch = (k->sm == ui.cur_slot && ui.cur_out && k->so == ui.cur_port) ||
                    (k->dm == ui.cur_slot && !ui.cur_out && k->di == ui.cur_port);
        pv_cable_curve(x0, y0, x1, y1, vx, vy, touch ? 2 : 1, touch ? C_WHITE : cat_color[pw_types[pws.mod[k->sm].type].cat]);
    }
    /* carried cable */
    if (ui.carrying && pv_has(ui.carry_slot, ui.carry_out, ui.carry_port)) {
        int x0, y0;
        pv_jack(ui.carry_slot, ui.carry_out, ui.carry_port, &x0, &y0);
        if (ui.carry_out) pv_cable_curve(x0, y0, cx, cy, vx, vy, 2, C_AMBER);
        else pv_cable_curve(cx, cy, x0, y0, vx, vy, 2, C_AMBER);
    }
    /* cursor */
    {
        int x = cx - vx, y = cy - vy + PV_TOP;
        uint16_t c = ui.carrying ? C_AMBER : C_WHITE;
        if (pv_cmp) frame_rect(x - 4, y - 4, 9, 9, c);
        else { frame_rect(x - 6, y - 6, 13, 13, c); frame_rect(x - 5, y - 5, 11, 11, c); }
    }
    /* chrome over the scrolled area */
    header();
    fill(0, PV_BOTTOM, W, H - PV_BOTTOM, C_BG);
    fill(0, PV_BOTTOM + 1, W, KY - PV_BOTTOM - 3, C_PANEL);
    port_label((unsigned)ui.cur_slot, ui.cur_out, (unsigned)ui.cur_port, buf);
    int lim = W - 4 - (pv_col_of(ui.cur_slot) ? page_tabs_wk(0, "ALGO") + 70 : param_pages(ui.cur_slot) > 1 ? page_tabs_w(param_pages(ui.cur_slot)) : 0) - 6;   /* (the name gives way: the page switcher stays) */
    int x = text_clip(5, PV_BOTTOM + 4, buf, F_S, cat_color[pw_types[pws.mod[ui.cur_slot].type].cat], lim);
    int found = 0;
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (!k->used) continue;
        int mine = ui.cur_out ? (k->sm == ui.cur_slot && k->so == ui.cur_port) : (k->dm == ui.cur_slot && k->di == ui.cur_port);
        if (!mine) continue;
        if (!found) {
            if (ui.cur_out) port_label(k->dm, 0, k->di, b2); else port_label(k->sm, 1, k->so, b2);
            x = text_clip(x + 4, PV_BOTTOM + 4, ui.cur_out ? ">" : "<", F_S, C_DIM, lim);
            x = text_clip(x + 4, PV_BOTTOM + 4, b2, F_S, C_TEXT, lim);
        }
        ++found;
    }
    if (found > 1) { char *o = s_cpy(b2, " +"); s_int(o, found - 1); text_clip(x, PV_BOTTOM + 4, b2, F_S, C_AMBER, lim); }
    const char *hint;
    if (ui.carrying) {
        int sm = ui.carry_out ? ui.carry_slot : ui.cur_slot, so = ui.carry_out ? ui.carry_port : ui.cur_port;
        int dm = ui.carry_out ? ui.cur_slot : ui.carry_slot, di = ui.carry_out ? ui.cur_port : ui.carry_port;
        hint = ui.carry_out == ui.cur_out ? "SELECT|To a jack|REC|Cancel"
             : pw_find_cable((unsigned)sm, (unsigned)so, (unsigned)dm, (unsigned)di) >= 0 ? "SEL|Remove cable|REC|Cancel"
             : "SEL|Plug in|REC|Cancel";
    } else hint = found ? "SEL|Cable|REC|Unplug|GLO+PLAY|Bypass"
                        : "SEL|Start a cable|GLO+PLAY|Bypass";
    int ms = sp_member(ui.cur_slot, 0, 0, 0);
    if (pv_col_of(ui.cur_slot)) {   /* a group box: which member's page, out of how many in all */
        unsigned idx, total; char lb[16], nb[16]; sp_member(ui.cur_slot, 0, &idx, &total);
        pw_module_label((unsigned)ms, lb);
        char *q = s_int(nb, (int)idx + 1); q = s_cpy(q, "/"); s_int(q, (int)total);
        page_plate(W - 4, PV_BOTTOM + 6, lb, nb, cat_color[pw_types[pws.mod[ms].type].cat]);
    } else page_hint(ui.cur_slot, PV_BOTTOM + 6, 0, C_PANEL);
    {   /* two rows: what SEL and REC do here, then the keys that are always there */
        char sp[200], *o = s_cpy(sp, hint);
        o = s_cpy(o, "\n");
        if (ui.carrying && ui.gview) o = s_cpy(o, "HOME|Leave group|GLO+FX|Leave group");
        else if (ui.carrying && pv_col_of(ui.cur_slot)) o = s_cpy(o, "GLO+FX|Open group");
        else if (!ui.carrying && ui.gview) o = s_cpy(o, "HOME|Leave group|FX|Add module|GLO+REC|Ungroup");
        else if (!ui.carrying && pv_col_of(ui.cur_slot)) o = s_cpy(o, "GLO+FX|Open group|GLO+REC|Ungroup|FX|Add module");
        else {
            if (!ui.carrying) o = s_cpy(o, pv_cmp ? "FX|Add module|GLO+SEQ|Size" : "FX|Add module|GLO+K1|Tempo");
            if (!ui.carrying) { if (found) o = s_cpy(o, "|GLO+SEL|Grab cable"); o = s_cpy(o, "|GLO+ENV|Copy|GLO+LFO|Paste|GLO+FX|Group"); }
        }
        if (ui.gsug) {
            int n = grp_cands(0); char *q = s_cpy(sp, "SELECT|Next chain ");
            q = s_int(q, n ? clampi(ui.gsug_i, 0, n - 1) + 1 : 0); q = s_cpy(q, "/"); q = s_int(q, n);
            s_cpy(q, "|SEL|Group them\nHOME|Cancel");
        }
        (void)o;
        strip_set(sp);
    }
    if (ui.gview) text_right(W - 4, PV_BOTTOM - 9, "IN GROUP", F_XS, C_DIM);
    knob_row(ms, ui.param_page[ms]);
}
static void pv_pick(void)   /* lift the cable plugged in here (its other end stays put) to plug it somewhere else: nothing is deleted until it lands */
{
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (!k->used) continue;
        int mine = ui.cur_out ? (k->sm == ui.cur_slot && k->so == ui.cur_port) : (k->dm == ui.cur_slot && k->di == ui.cur_port);
        if (!mine) continue;
        ui.carrying = 1; ui.carry_old = (int)c;
        if (ui.cur_out) { ui.carry_slot = k->dm; ui.carry_out = 0; ui.carry_port = k->di; }
        else { ui.carry_slot = k->sm; ui.carry_out = 1; ui.carry_port = k->so; }
        pv_step_jack(1);
        return;
    }
    notice("NOTHING PLUGGED HERE");
}
static void pv_sel(void)
{
    char a[24], b[24], msg[48];
    if (!pv_n) return;
    if (ui.carrying && ui.carry_out == ui.cur_out && pv_col_of(ui.cur_slot)) {   /* a cable in hand and a jack of its own kind on a group's box: open the group, the cable goes in with you */
        pv_open_group();
        return;
    }
    if (!ui.carrying || ui.carry_out == ui.cur_out) {
        ui.carrying = 1; ui.carry_old = -1; ui.carry_slot = ui.cur_slot; ui.carry_out = ui.cur_out; ui.carry_port = ui.cur_port;
        pv_step_jack(1); /* hop to the nearest jack it can plug into */
        return;
    }
    int sm = ui.carry_out ? ui.carry_slot : ui.cur_slot, so = ui.carry_out ? ui.carry_port : ui.cur_port;
    int dm = ui.carry_out ? ui.cur_slot : ui.carry_slot, di = ui.carry_out ? ui.cur_port : ui.carry_port;
    port_label((unsigned)sm, 1, (unsigned)so, a); port_label((unsigned)dm, 0, (unsigned)di, b);
    a[16] = b[16] = 0;
    int existing = pw_find_cable((unsigned)sm, (unsigned)so, (unsigned)dm, (unsigned)di);
    if (ui.carry_old >= 0 && ui.carry_old < PW_MAX_CABLES && pws.cable[ui.carry_old].used) {   /* a lifted cable lands: it leaves its old jack */
        if (existing == ui.carry_old) { notice("PUT BACK"); ui.carrying = 0; ui.carry_old = -1; return; }
        pw_disconnect((unsigned)ui.carry_old);
        existing = pw_find_cable((unsigned)sm, (unsigned)so, (unsigned)dm, (unsigned)di);
    }
    ui.carry_old = -1;
    if (existing >= 0) { pw_disconnect((unsigned)existing); char *o = s_cpy(msg, "REMOVED "); o = s_cpy(o, a); o = s_cpy(o, " > "); s_cpy(o, b); }
    else if (pw_connect((unsigned)sm, (unsigned)so, (unsigned)dm, (unsigned)di) >= 0) { char *o = s_cpy(msg, a); o = s_cpy(o, " > "); s_cpy(o, b); }
    else s_cpy(msg, "NO FREE CABLES");
    notice(msg);
    ui.carrying = 0;
}
static void pv_rec(void)
{
    if (ui.carrying) { ui.carrying = 0; notice(ui.carry_old >= 0 ? "PUT BACK" : "CABLE DROPPED"); ui.carry_old = -1; return; }
    int n = 0, oldest = -1;   /* the cables at this jack: REC unplugs one at a time, the one plugged in first first */
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (!k->used) continue;
        int mine = ui.cur_out ? (k->sm == ui.cur_slot && k->so == ui.cur_port) : (k->dm == ui.cur_slot && k->di == ui.cur_port);
        if (!mine) continue;
        ++n;
        if (oldest < 0 || (int16_t)(pws.cable_born[c] - pws.cable_born[oldest]) < 0) oldest = (int)c;
    }
    if (oldest >= 0) pw_disconnect((unsigned)oldest);
    notice(n > 2 ? "UNPLUGGED 1 CABLE (REC AGAIN FOR THE NEXT)" : n == 2 ? "UNPLUGGED 1 CABLE, 1 LEFT" : n == 1 ? "UNPLUGGED 1 CABLE" : "NOTHING PLUGGED HERE");
}
/* Adding a module where the patch screen's cursor is (the add list: when it was opened from there):
 * into the cables at the jack, onto the jack, or onto the loose end of the cable in hand (pw_add_wired) */
static void wire_target(int from_patch, int *slot, int *is_out, unsigned *port)
{
    *slot = -1; *is_out = 0; *port = 0;
    if (!from_patch || !pv_n) return;
    if (ui.carrying && pws.mod[ui.carry_slot].used == 1) { *slot = ui.carry_slot; *is_out = ui.carry_out | 2; *port = (unsigned)ui.carry_port; return; }
    if (ui.cur_slot < 0 || ui.cur_slot >= PW_MAX_MODULES || pws.mod[ui.cur_slot].used != 1) return;
    *slot = ui.cur_slot; *is_out = ui.cur_out; *port = (unsigned)ui.cur_port;
}
static void short_port(unsigned slot, int is_out, unsigned port, char *out)   /* "VCO1 SAW" */
{
    char *o = out;
    pw_module_label(slot, o);
    { char *a = o, *b = o; while (*a) { if (*a != ' ') *b++ = *a; ++a; } *b = 0; o = b; }
    *o++ = ' ';
    const pw_type *t = &pw_types[pws.mod[slot].type];
    s_cpy(o, is_out ? t->out[port] : t->in[port]);
}
/* the plan for adding `type`: "VCO1 SAW > DELAY > VCF1 IN"; what SEL does, in words (0: it adds it unconnected) */
static const char *wire_plan_text(unsigned type, int from_patch, char *buf)
{
    int slot, is_out; unsigned port, in, out;
    wire_target(from_patch, &slot, &is_out, &port);
    int kind = pw_wire_plan(type, slot, is_out, port, -1, &in, &out), carry = is_out & 2;
    is_out &= 1;
    const pw_type *t = &pw_types[type];
    char a[28], b[28];
    buf[0] = 0;
    const char *join = carry ? "plug in" : "connect";
    if (kind == PW_WIRE_ALONE) { if (slot >= 0) s_cpy(buf, "This must be connected to an output, so module will be added unpatched"); return 0; }
    if (kind == PW_WIRE_FEED) { short_port((unsigned)slot, 1, port, a); char *o = s_cpy(buf, a); o = s_cpy(o, " > "); o = s_cpy(o, t->name); o = s_cpy(o, " "); s_cpy(o, t->in[in]); return join; }
    if (kind == PW_WIRE_DRIVE) { char *o = s_cpy(buf, t->name); o = s_cpy(o, " "); o = s_cpy(o, t->out[out]); o = s_cpy(o, " > "); short_port((unsigned)slot, 0, port, a); s_cpy(o, a); return join; }
    int n = 0;   /* insert: the cables at the jack */
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (!k->used || !(is_out ? (k->sm == slot && k->so == port) : (k->dm == slot && k->di == port))) continue;
        if (!n++) { if (is_out) short_port(k->dm, 0, k->di, b); else short_port(k->sm, 1, k->so, b); }
    }
    if (is_out) short_port((unsigned)slot, 1, port, a); else short_port((unsigned)slot, 0, port, a);
    char *o = s_cpy(buf, is_out ? a : b); o = s_cpy(o, " > "); o = s_cpy(o, t->name); o = s_cpy(o, " > "); o = s_cpy(o, is_out ? b : a);
    if (n > 1) { o = s_cpy(o, " +"); s_int(o, n - 1); }
    return "insert";
}
/* the two lines saying what SEL and ENV do */
static int replace_target(void);
static void live_key(int wi, int glo_k, int sel_held, int rec_held);
/* a knob's name (ALGO, K1, SELECT ...) sits in an open ring, a button's in a filled plate: GLO+K1 is a plate then a ring */
static int is_knob_key(const char *b)
{
    static const char *const kn[] = {"ALGO", "K1", "K2", "K3", "K4", "K1-4", "SELECT", "KNOB"};
    for (unsigned i = 0; i < sizeof kn / sizeof kn[0]; ++i) if (strlen(kn[i]) == strlen(b) && !memcmp(b, kn[i], strlen(b))) return 1;
    return 0;
}
/* a key combination is one shape with a "+" between its keys: a plate, or (when a knob is in it) a plate with round ends and a dial before each knob's name */
static int key_walk(int x, int y, const char *k, uint16_t c, int draw)   /* the width; draws it when draw */
{
    const char *ps[4], *pe[4]; int n = 0, kn[4], anyk = 0;
    for (const char *q = k; n < 4; ++n) {
        const char *e = q; while (*e && *e != '+') ++e;
        char b[12]; int len = (int)(e - q); if (len > 11) len = 11;
        memcpy(b, q, (unsigned)len); b[len] = 0;
        ps[n] = q; pe[n] = e; kn[n] = is_knob_key(b); anyk |= kn[n];
        if (!*e) { ++n; break; }
        q = e + 1;
    }
    int plus = text_w("+", F_XS) + 6, lp = anyk ? 4 : 3, rp = anyk ? 5 : 3, w = lp + rp + (n - 1) * plus;
    char b[4][12];
    for (int i = 0; i < n; ++i) {
        int len = (int)(pe[i] - ps[i]); if (len > 11) len = 11;
        memcpy(b[i], ps[i], (unsigned)len); b[i][len] = 0;
        w += text_w(b[i], F_XS) + (kn[i] ? 8 : 0);
    }
    if (!draw) return w;
    if (anyk) {
        fill(x + 3, y - 1, w - 6, 12, C_PLATE_E); fill(x + 1, y, w - 2, 10, C_PLATE_E); fill(x, y + 2, w, 6, C_PLATE_E);   /* (a plate with round ends: the buttons' colors) */
        fill(x + 3, y, w - 6, 10, C_PLATE); fill(x + 1, y + 1, w - 2, 8, C_PLATE); fill(x + 1, y + 2, w - 2, 6, C_PLATE);
    } else { rfill(x, y - 1, w, 12, C_PLATE_E); rfill(x + 1, y, w - 2, 10, C_PLATE); }
    int cx = x + lp;
    for (int i = 0; i < n; ++i) {
        if (kn[i]) {   /* a little dial before the name: a knob, to turn (a button is pressed) */
            static const uint8_t dial[7] = {0x1c, 0x2a, 0x49, 0x49, 0x41, 0x22, 0x1c};
            for (int r = 0; r < 7; ++r)
                for (int q = 0; q < 7;) {
                    if (!(dial[r] >> (6 - q) & 1)) { ++q; continue; }
                    int e = q; while (e < 7 && (dial[r] >> (6 - e) & 1)) ++e;
                    fill(cx + q, y + 1 + r, e - q, 1, c); q = e;
                }
            cx += 8;
        }
        cx = text(cx, y, b[i], F_XS, c);
        if (i + 1 < n) { text(cx + 3, y, "+", F_XS, C_DIM); cx += plus; }
    }
    return w;
}
static int key_w(const char *k) { return key_walk(0, 0, k, 0, 0); }
static int key_tag(int x, int y, const char *k, uint16_t c)   /* a key's name; returns where the text after it starts */
{
    return x + key_walk(x, y, k, c, 1) + 4;
}
/* the two rows under the add list: what SEL, ENV, EDIT and GLO do right now (sel: the verb for SEL, or 0 when it adds it alone) */
static void wire_hint(const char *sel, int fx)   /* the add list's strip: what SEL, ENV and EDIT do right now (fx: 0) */
{
    char sp[112], cap[48], *o; int ci = 0;
    if (sel) { for (; sel[ci] && ci < 47; ++ci) cap[ci] = sel[ci]; if (ci && cap[0] >= 'a' && cap[0] <= 'z') cap[0] = (char)(cap[0] - 32); }
    cap[ci] = 0;
    if (fx) {
        o = s_cpy(sp, "SEL|"); o = s_cpy(o, sel ? cap : "Add it"); o = s_cpy(o, "|GLO+SEL|Replace|FX|Full list");
        if (sel) o = s_cpy(o, "|ENV|Add unconnected");
        strip_set(sp); return;
    }
    /* three keys, the ones that matter first: SEL, ENV (unconnected), REC (star); what does not fit gives way a word at a time */
    int fav = is_fav(add_order[clampi(ui.add_cursor, 0, PW_T_SHOWN - 1)]);
    for (int tries = 0; tries < 3; ++tries) {
        o = s_cpy(sp, "SEL|"); o = s_cpy(o, sel && tries < 1 ? cap : tries ? "Add" : "Add it");
        if (sel) o = s_cpy(o, tries < 2 ? "|ENV|Add unconnected" : "|ENV|Unconnected");
        o = s_cpy(o, "|REC|"); o = s_cpy(o, tries < 1 ? (fav ? "Unfavorite" : "Favorite") : "Fav");   /* (GLO + SEL, Replace, shows while GLO is held) */
        if (hint_w(sp) <= W - 8) break;
    }
    strip_set(sp);
}
/* what the cursor sits on, for where the add list starts: 0 an audio output, 1 a control input, 2 an audio input, 3 nothing */
static int wire_context(void)
{
    int slot, is_out; unsigned port;
    wire_target(1, &slot, &is_out, &port);
    if (slot < 0) return 3;
    const pw_type *t = &pw_types[pws.mod[slot].type];
    if (is_out & 1) return t->cat == PW_C_SOURCE || t->cat == PW_C_FILTER || t->cat == PW_C_FX ? 0 : 3;
    return pw_audio_jack(t->in[port]) ? 2 : 1;
}

static void port_row(int x, int y, int w, const char *name, const char *src, int more, uint16_t c)
{
    int nx = text(x, y, name, F_S, C_TEXT);
    if (!src) { text(nx + 6, y, "-", F_S, C_LINE); return; }
    int room = x + w - (nx + 6) - (more ? 14 : 0);
    text_fit(nx + 6, y + 1, src, F_XS, c, room);
    if (more) { char b[6]; b[0] = '+'; s_int(b + 1, more); text_right(x + w, y + 1, b, F_XS, C_AMBER); }
}
/* ------------------------------------------------------- the module page
 * A strip with the module's name and its parameter pages, a visualization of what the module does (its envelope, its
 * filter's curve, the FM algorithm and operators, a drum grid...) and the knob row. The visualization lights up what the
 * current knob page changes. Modules without a visualization of their own: every parameter page at once, or (one page)
 * what is wired to them. GLO+ALGORITHM: the faces. */
static int pg_row;   /* the module page: 13 when a line of page boxes sits above the knobs (the picture is that much shorter), else 0 */
#define VY (HDR + 18)            /* the visualization's area */
#define VH (KY - 2 - pg_row - VY)
static uint16_t dim_of(uint16_t c) { return blend(C_BG, c, 6); }
static uint16_t mid_of(uint16_t c) { return blend(C_PANEL, c, 10); }
/* the first word of the page's parameter names when they share it ("RATIO 1".."RATIO 4": RATIO) */
static void page_name(const pw_type *t, unsigned page, char *out)
{
    out[0] = 0;
    if (t->pages) { const char *q = t->pages[page]; unsigned i = 0; while (q[i] && i < 11) { out[i] = q[i]; ++i; } out[i] = 0; return; }
    unsigned p0 = page * 4, n = 0;
    const char *a = t->param[p0].name;
    while (a[n] && a[n] != ' ') ++n;
    if (!a[n] || n > 7) return;
    for (unsigned p = p0 + 1; p < p0 + 4 && p < t->n_param; ++p)
        for (unsigned i = 0; i <= n; ++i) if (t->param[p].name[i] != a[i]) return;
    memcpy(out, a, n); out[n] = 0;
}
/* the module page's faces: 0 its picture (or the knob pages), 1 INFO (what it does, what is wired), 2 DEMO (an example patch) */
static int face_next(int s)
{
    unsigned type = pws.mod[s].type;
    int info_same = !pw_ui_has_vis(type) && param_pages((unsigned)s) <= 1, ex = pw_example_text(type) != 0, f = ui.edit_jacks;
    if (f == 0) return info_same ? (ex ? 2 : 0) : 1;
    if (f == 1) return ex ? 2 : 0;
    return 0;
}
/* how many faces this module has, which one is showing (as its box number), and its name */
static int face_info(int s, int *cur, const char **label)
{
    unsigned type = pws.mod[s].type;
    int info_same = !pw_ui_has_vis(type) && param_pages((unsigned)s) <= 1, ex = pw_example_text(type) != 0, f = ui.edit_jacks;
    if (f == 2 && !ex) f = 0;
    int n = (info_same ? 1 : 2) + ex;
    *cur = f == 0 ? 0 : f == 1 ? 1 : (info_same ? 1 : 2);
    *label = f == 2 ? "DEMO" : f == 1 || info_same ? "INFO" : pw_ui_has_vis(type) ? "VISUAL" : "PAGES";
    return n;
}
static void edit_strip(int s)
{
    const pw_type *t = &pw_types[pws.mod[s].type];
    char label[16];
    pw_module_tag_label((unsigned)s, label);   /* (MODAL 1, not the preset's name: that is PRESET's knob) */
    fill(0, HDR, W, 17, C_PANEL);
    fill(0, HDR, 4, 17, cat_color[t->cat]);
    int x = text(9, HDR + 1, label, F_M, C_WHITE);
    int right = W - 4;
    {   /* the faces: its name, then EDIT's plate and a box for each (the one showing filled) */
        int fc; const char *fl; int nf = face_info(s, &fc, &fl);
        if (nf > 1) {
            int tw = page_tabs_wk((unsigned)nf, "EDIT");
            right -= tw; page_tabs_k(right, HDR + 4, (unsigned)nf, (unsigned)fc, C_PANEL, "EDIT");
            int lw = text_w(fl, F_XS);
            right -= lw + 6; text(right + 2, HDR + 4, fl, F_XS, C_AMBER);
            right -= 4;
        }
    }
    if (pws.mod[s].flags & PW_F_BYPASS) { right -= 30; rfill(right + 2, HDR + 2, 26, 13, C_RED); text_center(right + 15, HDR + 4, "BYP", F_XS, C_WHITE); right -= 2; }
    if (pws.voices > 1 && (pws.mod[s].flags & PW_F_POLY)) { right -= 30; rfill(right + 2, HDR + 2, 26, 13, C_AMBER); text_center(right + 15, HDR + 4, "VOICE", F_XS, C_BG); right -= 2; }
    text_fit(x + 6, HDR + 4, t->name, F_XS, cat_color[t->cat], right - x - 10);
}
/* the line above the knobs, as on the patch screen: what the page holds (left), ALGO's plate and a box for each page (right) */
static void edit_pagerow(int s)
{
    const pw_type *t = &pw_types[pws.mod[s].type];
    unsigned pages = param_pages(s), cur = ui.param_page[s];
    int y = KY - 12;
    char name[16];
    fill(0, KY - 14, W, 14, C_BG);
    page_name(t, cur, name);
    if (!name[0]) {
        unsigned n = t->n_param - cur * 4u;   /* the hardware has knobs 1-4: number them as they sit under this page */
        if (n > 4) n = 4;
        char *o = s_cpy(name, n > 1 ? "KNOBS 1-" : "KNOB 1"); if (n > 1) s_int(o, (int)n);
    }
    text(6, y, name, F_XS, cat_color[t->cat]);
    page_tabs(W - 4 - page_tabs_w(pages), y, pages, cur, C_BG);
}
/* --- the jacks: what is wired in and out (EDIT on the module page, and modules with one page and no visualization) */
static void edit_jacks(int s, int y0)
{
    const pw_module *m = &pws.mod[s];
    const pw_type *t = &pw_types[m->type];
    char buf[40];
    int rows = t->n_in > t->n_out ? t->n_in : t->n_out, lh = rows > 5 ? 15 : 17;
    for (unsigned i = 0; i < t->n_in; ++i) {
        int y = y0 + (int)i * lh, found = 0, am = 100;
        const char *src = 0;
        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
            const pw_cable *k = &pws.cable[c];
            if (!k->used || k->dm != s || k->di != i) continue;
            if (!found++) { port_label(k->sm, 1, k->so, buf); src = buf; am = k->amt; }
        }
        if (found && am != 100) { char av[8], *ao = s_int(av, am); s_cpy(ao, "%"); text_right(116, y + 1, av, F_XS, am < 0 ? C_RED : C_AMBER); }
        port_row(4, y, found && am != 100 ? 84 : 112, t->in[i], src, found > 1 ? found - 1 : 0, C_DIM);
    }
    if (rows) fill(119, y0 - 2, 1, rows * lh, C_PANEL2);
    for (unsigned o = 0; o < t->n_out; ++o) {
        int y = y0 + (int)o * lh, found = 0;
        const char *dst = 0;
        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
            const pw_cable *k = &pws.cable[c];
            if (!k->used || k->sm != s || k->so != o) continue;
            if (!found++) { port_label(k->dm, 0, k->di, buf); dst = buf; }
        }
        port_row(124, y, 112, t->out[o], dst, found > 1 ? found - 1 : 0, cat_color[t->cat]);
    }
}
static void view_wired(int s)
{
    int y = wrap_text(6, VY + 3, pw_type_help(pws.mod[s].type), F_S, C_DIM, W - 12, 2, 15);
    edit_jacks(s, y + 4);
}
/* a hint in the one style everywhere: "KEY|what it does|KEY|what it does": each key on an amber plate, its action in white */
static int hint_walk(int x, int y, const char *spec, int draw)
{
    char key[24], act[48];
    for (const char *q = spec; *q;) {
        int n = 0; while (*q && *q != '|' && n < 23) key[n++] = *q++; key[n] = 0; if (*q == '|') ++q;
        n = 0; while (*q && *q != '|' && n < 47) act[n++] = *q++; act[n] = 0; if (*q == '|') ++q;
        int st = n > 0 && act[n - 1] == '\x01';   /* "Bar Sync\x01": the setting's state (ON / OFF) follows it, same size and line, in its own color */
        if (st) act[n - 1] = 0;
        const char *sv = ui.var_quant ? "ON" : "OFF";
        int x0 = x, nx = key[0] ? (draw ? key_tag(x, y, key, C_AMBER) : x + key_w(key) + 4) : x;
        if (draw) text(nx, y + 1, act, F_XS, C_WHITE);
        int ex = nx + text_w(act, F_XS);
        if (st) {
            if (draw) text(ex + 4, y + 1, sv, F_XS, ui.var_quant ? C_AMBER : C_DIM);
            ex += 4 + text_w(sv, F_XS);
            if (draw && strip_chg == 2) frame_rect(x0 - 3, y - 2, ex - x0 + 6, 13, C_WHITE);   /* (PERFORM: the cursor is on it) */
        }
        x = ex + 8;
    }
    return x;
}
static int hint_w(const char *spec) { return hint_walk(0, 0, spec, 0) - 8; }
static void hint_draw(int x, int y, const char *spec) { hint_walk(x, y, spec, 1); }
/* the strip: a page says what its keys do ("KEY|action|KEY|action", four at most, kept in the order given: the first is
 * the most useful) and the frame draws it at the bottom, on the left. A little text may sit at its right end. */
/* (strip_buf, strip_right and strip_pre, the view's own hints ahead of the page's: declared at the top) */
static void strip_set(const char *spec)   /* (a "\n" starts the second row, which only the tall strip has) */
{
    int bars = 0, n = 0, skip = 0;
    for (const char *q = spec; *q && n < 110; ++q) {
        if (*q == '\n') { if (!ky_off) break; bars = 0; skip = 0; }
        else if (skip) continue;
        else if (*q == '|' && ++bars == 8) { skip = 1; continue; }   /* the fifth key of a row: no more */
        strip_buf[n++] = *q;
        if (ui.glo_latch && n >= 4 && strip_buf[n - 1] == '+' && strip_buf[n - 2] == 'O' && strip_buf[n - 3] == 'L' && strip_buf[n - 4] == 'G') strip_buf[n - 1] = '>';   /* (GLO is tapped, not held) */
    }
    while (n && (strip_buf[n - 1] == '|' || strip_buf[n - 1] == '\n')) --n;   /* (a trailing separator from a joined spec) */
    strip_buf[n] = 0;
}
/* one row of the strip at y: what does not fit goes, last pair first */
static void strip_row(const char *src, int len, int y, int maxw)
{
    char t[112]; int n = 0; while (n < len && n < 110) { t[n] = src[n]; ++n; } t[n] = 0;
    while (n && hint_w(t) > maxw) {
        int bars = 0, cut = -1;
        for (int i = n - 1; i >= 0; --i) if (t[i] == '|' && ++bars == 2) { cut = i; break; }
        if (cut < 0) break;
        t[cut] = 0; n = cut;
    }
    hint_draw(4, y, t);
}
static void strip_draw(void)
{
    int perf = strip_chg != 0, pw = 0;
    char gl[160]; int glo_layer = 0;
    int glo_held = glo_held_now();
    if (notice_live() && (!glo_held || ui.notice_glo)) {   /* what just happened: in the strip, in place of its shortcuts, for a moment */
        fill(0, STRIP_Y, W, H - STRIP_Y, C_STRIP);
        fill(0, STRIP_Y, W, 1, C_STRIPL);
        int f = text_w(ui.notice, F_S) <= W - 16 ? F_S : F_XS;
        text_fit(6, STRIP_Y + (H - STRIP_Y - (f == F_S ? 9 : 7)) / 2 + 1, ui.notice, f, C_AMBER, W - 12);
        return;
    }
    if (glo_held) {   /* GLO held: what GLO + a key does here */
        const char *g = 0;
        if (ui.page == PG_RACK && ky_off) {
            static char gk[160];
            const char *fx = ui.gview ? "GLO+FX|Leave group" : pv_col_of(ui.cur_slot) ? "GLO+FX|Open group" : "GLO+REC|Delete+wire|GLO+FX|Group";   /* (on a group's jack it opens the group) */
            char *q = s_cpy(gk, fx);
            s_cpy(q, ui.gview || pv_col_of(ui.cur_slot) ? "|GLO+REC|Ungroup\nGLO+SEL|Grab|GLO+ENV/LFO|Copy/Paste" : "\nGLO+SEL|Grab|GLO+ENV/LFO|Copy/Paste");
            g = gk;
        }
        else if (ui.page == PG_RACK) g = "GLO+REC|Delete+wire|GLO+ENV/LFO|Copy/Paste|GLO+OCT|Undo|GLO+K1|Tempo";
        else if (ui.page == PG_EDIT) g = "GLO+REC|Delete+wire|GLO+ENV/LFO|Copy/Paste|GLO+OCT|Undo|GLO+PLAY|Bypass";
        else if (ui.page == PG_SCOPE || ui.page == PG_CABLES) g = "GLO+ENV/LFO|Copy/Paste|GLO+OCT|Undo|GLO+PLAY|Bypass|GLO+PRESET|Metro";
        else if (ui.page == PG_PERFORM) g = "GLO+KEY|Save snapshot|GLO+SAVE|Edits all|GLO+OCT|Undo|GLO+PRESET|Metronome";
        else if (ui.page == PG_LIVE) g = "GLO+KEY|Morph to snapshot|GLO+PRESET|Metronome";
        else if (ui.page == PG_MUTATE) g = "GLO+OCT|Undo|GLO+PRESET|Metronome";
        else if (ui.page == PG_ADD) { int rt = replace_target(); static char gb[96]; char *q = s_cpy(gb, "GLO+SEL|Replace "); if (rt >= 0) {
                q = s_cpy(q, pw_types[pws.mod[rt].type].name);
                char *e = s_cpy(q, " with "); s_cpy(e, pw_types[add_order[clampi(ui.add_cursor, 0, PW_T_SHOWN - 1)]].name);
                if (hint_w(gb) > W - 8) *q = 0;   /* (too long: just what goes) */
            } else s_cpy(q, "(no module here)"); g = gb; }
        else if (ui.page == PG_PATCHES) g = "ALGO|Jump section";
        else if (ui.page == PG_SONG) g = ui.song_lane > 0 ? "GLO+REC|Delete lane|GLO+SAVE|Edits all|GLO+OCT|Undo" : "GLO+SEL|Duplicate clip|GLO+SAVE|Edits all|GLO+OCT|Undo|GLO+PRESET|Metronome";
        if (g) { int n = 0; while (g[n] && n < (int)sizeof gl - 1) { gl[n] = g[n]; ++n; } gl[n] = 0; glo_layer = 1;
            if (ui.glo_on) { int a = 0, b = 0; while (gl[a]) { if (gl[a]=='G'&&gl[a+1]=='L'&&gl[a+2]=='O'&&gl[a+3]=='+') a += 4; else gl[b++] = gl[a++]; } gl[b] = 0; }
        }
    }
    if (!glo_layer && (ui.buttons & BIT(B_ARP)) && fm1_millis() - ui.arp_down_ms >= 250u && (ui.page == PG_RACK || ui.page == PG_EDIT)) {   /* ARP held: what ARP + a knob does */
        const char *g = ky_off ? "ARP+KNOB|Record it in a MOD SEQ lane\nGLO+ARP+KNOB|Let it go" : "ARP+KNOB|Record to a lane|GLO+ARP+KNOB|Clear";
        int n = 0; while (g[n]) { gl[n] = g[n]; ++n; } gl[n] = 0; glo_layer = 1;
    }
    if (!glo_layer && !strip_buf[0] && !strip_right[0] && !perf) return;
    fill(0, STRIP_Y, W, H - STRIP_Y, C_STRIP);
    fill(0, STRIP_Y, W, 1, C_STRIPL);
    {
        const char *src = glo_layer ? gl : strip_buf;
        int len = 0; while (src[len] && src[len] != '\n') ++len;
        strip_row(src, len, STRIP_Y + 3, W - 6 - pw);
        if (ky_off && src[len] == '\n') {   /* the tall strip's second row */
            const char *r2 = src + len + 1; int l2 = 0; while (r2[l2]) ++l2;
            strip_row(r2, l2, STRIP_Y + 15, W - 6);
        }
    }
    if (strip_right[0] && !glo_layer) text_right(W - 5, STRIP_Y + 4, strip_right, F_XS, C_DIM);
}
/* AUDITION strikes a source on its own unless the keys already play it: a KEYS module somewhere upstream of it (through any modules between) means the keys play the
 * patch as usual. A source that is cabled out is still auditioned: its sound goes on down its cables, through the effects, to the output. */
static int keys_upstream(int s)
{
    uint8_t seen[PW_MAX_MODULES] = {0};
    int stack[PW_MAX_MODULES], n = 0;
    stack[n++] = s; seen[s] = 1;
    while (n) {
        int cur = stack[--n];
        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
            const pw_cable *k = &pws.cable[c];
            if (!k->used || k->dm != cur || k->sm >= PW_MAX_MODULES || seen[k->sm] || pws.mod[k->sm].used != 1) continue;
            if (pws.mod[k->sm].type == PW_T_KEYS) return 1;
            seen[k->sm] = 1; stack[n++] = k->sm;
        }
    }
    return 0;
}
static int audition_solo(int s)
{
    if (s < 0 || s >= PW_MAX_MODULES || pws.mod[s].used != 1 || !pw_audition_ok(pws.mod[s].type)) return 0;
    return !keys_upstream(s);
}
/* the note at the corner of a module page that says where EDIT goes next */
static void edit_hint(const char *msg)   /* the module page's strip: the view's own keys, or Audition and ALGO, then EDIT */
{
    char b[112], *o = b; const char *post = 0;
    int s = ui.sel;
    if (strip_pre) o = s_cpy(o, strip_pre);
    else if (ui.page == PG_EDIT && s >= 0 && s < PW_MAX_MODULES && pws.mod[s].used == 1) {
        if (pw_audition_ok(pws.mod[s].type)) o = s_cpy(o, audition_solo(s) ? "KEYS|Audition|" : "KEYS|Play patch|");
    }
    o = s_cpy(o, msg);
    if (strip_post) post = strip_post;
    if (post) s_cpy(o, post);
    strip_set(b);
}
static void edit_hint_at(int y, const char *msg) { (void)y; edit_hint(msg); }
static int extra_ok(unsigned type);
static void view_extra(int s, int x, int y, int w, int h);
static void view_pages_full(int s)
{
    const pw_type *t = &pw_types[pws.mod[s].type];
    unsigned pages = param_pages(s), cur = ui.param_page[s], p0 = 0, np = pages;
    int top = VY + 2, avail = H - 1 - top, rh = avail / (int)np; if (rh > 56) rh = 56;
    int y0 = top + (avail - rh * (int)np);
    if (extra_ok(pws.mod[s].type) && y0 - top >= 18) {
        int band = y0 - top - 2;
        if (y0 - top >= 32) { view_extra(s, 2, top, W - 4, band - 14); edit_hint_at(y0 - 14, "EDIT|Connections"); if (pages > 1) page_hint(s, y0 - 14, 124, C_BG); }
        else view_extra(s, 2, top, W - 4, band);
    } else if (y0 - top >= 32) { wrap_text(6, top + 1, pw_type_help(pws.mod[s].type), F_S, C_DIM, W - 12, (y0 - top - 14) / 15, 15); edit_hint_at(y0 - 14, "EDIT|Connections"); }
    int big = rh >= 46, mid = rh >= 34;
    for (unsigned pg = p0; pg < p0 + np; ++pg) {
        int y = y0 + (int)(pg - p0) * rh, lit = pg == cur;
        for (unsigned k = 0; k < 4; ++k) {
            unsigned p = pg * 4 + k;
            int x = 1 + (int)k * 60;
            rfill(x, y, 58, rh - 2, lit ? C_PANEL2 : C_PANEL);
            if (p >= t->n_param) continue;
            p = pw_param_at(pws.mod[s].type, p);
            char v[16];
            pw_format_param((unsigned)s, p, v);
            uint16_t cn = lit ? C_DIM : C_LINE, cv = lit ? C_WHITE : C_DIM;
            if (big) {
                text_center(x + 29, y + 4, t->param[p].name, F_S, cn);
                char l1[16], l2[16];
                if (text_w(v, F_S) > 56 && value_split(v, l1, l2)) {
                    text_center(x + 29, y + 19, l1, F_XS, cv); text_center(x + 29, y + 28, l2, F_XS, cv);
                } else {
                    int f = text_w(v, F_M) <= 56 ? F_M : F_S;
                    text_center(x + 29, y + (f == F_M ? 20 : 22), v, f, cv);
                }
            } else {
                text_center(x + 29, y + 2, t->param[p].name, F_XS, cn);
                int f = mid && text_w(v, F_S) <= 56 ? F_S : F_XS;
                text_center(x + 29, y + (mid ? 14 : 12), v, f, cv);
            }
            int bw = clampi((int)(param_norm((unsigned)s, p) * 48.0f + 0.5f), 0, 48);
            fill(x + 5, y + rh - 8, 48, big ? 4 : 2, C_BG);
            fill(x + 5, y + rh - 8, bw, big ? 4 : 2, lit ? cat_color[t->cat] : mid_of(cat_color[t->cat]));
        }
    }
}
/* a curve through n points, with a dim fill under it down to `base` */
static void curve(const int *xs, const int *ys, int n, int base, uint16_t c, int thick, int filled)
{
    if (filled)
        for (int i = 1; i < n; ++i)
            for (int x = xs[i - 1]; x < xs[i]; x += 2) {
                int y = ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1] ? xs[i] - xs[i - 1] : 1);
                if (base > y) fill(x, y, 2, base - y, dim_of(c));
            }
    for (int i = 1; i < n; ++i) line(xs[i - 1], ys[i - 1], xs[i], ys[i], thick, c);
}
/* --- ENVELOPE: its shape, A D S R under their segments, a dot where it is now */
static int log_width(float sec)      /* 1 ms .. 10 s: 8..62 px */
{
    float u = (pw_log2(pw_clamp(sec, 0.001f, 10.0f)) + 9.966f) / 13.288f;
    return 8 + (int)(u * 54.0f);
}
static float env_bend(float e, float lin, float c)   /* a stage's CURVE drawn: towards a straight line above 0, bowed the other way below */
{
    if (c >= 0) return e + (lin - e) * c;
    return e <= 0 ? 0.0f : pw_exp2((1.0f - 2.0f * c) * pw_log2(e));
}
static void view_env(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int mode = (int)(m->p[4] + 0.5f), top = VY + 10, bot = VY + VH - 24, h = bot - top;
    float sus = mode == 1 ? 0.0f : mode == 2 ? 1.0f : m->p[2];
    int loop = m->p[5] > 0.5f;   /* LOOP: decay runs into release, which runs into the next attack: no sustain */
    int wa = log_width(m->p[0]), wd = mode == 2 ? 0 : log_width(m->p[1]), ws = mode == 1 || loop ? 0 : 40, wr = mode == 1 ? 0 : log_width(m->p[3]);
    float ca = m->p[6], cd = m->p[7], cr = m->p[8];
    int x0 = (W - (wa + wd + ws + wr)) / 2, xa = x0 + wa, xd = xa + wd, xs = xd + ws, xr = xs + wr;
    int sy = bot - (int)(sus * (float)h);
    int px[40], py[40], n = 0;
    for (int i = 0; i <= 8; ++i) { float u = (float)i / 8.0f; px[n] = x0 + (int)(u * (float)wa); py[n++] = bot - (int)(env_bend((1.0f - pw_exp2(-3.0f * u)) / 0.875f, u, ca) * (float)h); }
    for (int i = 1; i <= 8 && wd; ++i) { float u = (float)i / 8.0f, e = pw_exp2(-5.0f * u); px[n] = xa + (int)(u * (float)wd); py[n++] = sy - (int)(((float)(sy - top)) * env_bend((e - 0.03f) / 0.97f, 1.0f - u, cd)); }
    if (ws) { px[n] = xs; py[n++] = sy; }
    for (int i = 1; i <= 8 && wr; ++i) { float u = (float)i / 8.0f, e = pw_exp2(-5.0f * u); px[n] = xs + (int)(u * (float)wr); py[n++] = bot - (int)((float)(bot - sy) * env_bend((e - 0.03f) / 0.97f, 1.0f - u, cr)); }
    fill(x0 - 4, bot, xr - x0 + 8, 1, C_LINE);
    curve(px, py, n, bot, c, 2, 1);
    static const char *const seg[4] = {"A", "D", "S", "R"};
    int mids[4] = {x0 + wa / 2, xa + wd / 2, xd + ws / 2, xs + wr / 2}, show[4] = {1, wd > 0, ws > 0, wr > 0};
    for (int k = 0; k < 4; ++k) if (show[k]) text_center(mids[k], bot + 6, seg[k], F_S, ui.param_page[s] == 0 ? C_TEXT : C_DIM);
    if (ws) for (int x = xd; x < xs; x += 4) fill(x, sy, 2, 1, C_WHITE);
    if (loop) text_right(W - 8, VY + 2, "LOOP", F_XS, C_AMBER);
    if (m->p[11] < 0.5f) text(8, VY + 2, "FAST x0.1", F_XS, C_DIM); else if (m->p[11] > 1.5f) text(8, VY + 2, "SLOW x10", F_XS, C_DIM);
    /* where it is */
    float v = pw_live((unsigned)s, 0);
    int stage = (int)pw_live((unsigned)s, 1), dx;
    if (stage == 0) return;
    if (stage == 1) dx = x0 + (int)(v * (float)wa);
    else if (stage == 2) dx = xa + (int)(pw_clamp((1.0f - v) / pw_maxf(1.0f - sus, 0.01f), 0, 1) * (float)wd);
    else if (stage == 3) dx = xd + ws / 2;
    else dx = xs + (int)(pw_clamp(1.0f - v / pw_maxf(sus, 0.01f), 0, 1) * (float)wr);
    int dy = bot - (int)(v * (float)h);
    fill(dx - 3, dy - 3, 7, 7, C_WHITE);
}
/* --- VCF: the low-pass's curve for its circuit, cutoff and resonance */
static float f_to_x(float hz) { return (pw_log2(pw_clamp(hz, 20.0f, 20000.0f) / 20.0f) / 9.966f) * (float)(W - 16) + 8.0f; }
static void view_vcf(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    static const char *const nm[6] = {"STATE VARIABLE 12dB", "TRANSISTOR LADDER", "DIODE LADDER", "OTA CASCADE 24dB", "OTA 2-POLE", "SALLEN-KEY"};
    int type = clampi((int)(m->p[4] + 0.5f), 0, 5);
    float fc = m->p[0], r = m->p[1];
    int top = VY + 4, bot = VY + VH - 4, zero = top + (bot - top) * 18 / 54;   /* +18 .. -36 dB */
    for (float dec = 100.0f; dec < 20000.0f; dec *= 10.0f) fill((int)f_to_x(dec), top, 1, bot - top, C_PANEL);
    fill(8, zero, W - 16, 1, C_PANEL2);
    int px[64], py[64], n = 0;
    for (int i = 0; i < 56; ++i) {
        float hz = 20.0f * pw_exp2((float)i * 9.966f / 55.0f), x = hz / fc, re, im;
        if (type == 1 || type == 3) {            /* (1 + jx)^4 + k */
            float k = (type == 1 ? 4.3f : 4.1f) * r; if (k > 3.95f) k = 3.95f;
            float x2 = x * x;
            re = 1.0f - 6.0f * x2 + x2 * x2 + k; im = 4.0f * x - 4.0f * x2 * x;
        } else if (type == 2) {                  /* the diode ladder: s^4 + 8s^3 + 20s^2 + 16s + 2 + 2k, s = j x sqrt2 */
            float k = 17.0f * r; if (k > 16.6f) k = 16.6f;
            float w = x * 1.4142136f, w2 = w * w;
            re = (w2 * w2 - 20.0f * w2 + 2.0f + 2.0f * k) * 0.5f; im = (-8.0f * w2 * w + 16.0f * w) * 0.5f;
        } else {                                 /* 1 - x^2 + j k x */
            float k = type == 0 ? 2.0f - 1.9f * r : type == 4 ? 2.0f - 2.0f * r : 2.0f - 2.08f * r;
            if (k < 0.06f) k = 0.06f;
            re = 1.0f - x * x; im = k * x;
        }
        float db = -3.0103f * pw_log2(pw_maxf(re * re + im * im, 1e-9f));
        if (type == 1 || type == 3) db += 20.0f * 0.30103f * pw_log2(1.0f + ((type == 1 ? 4.3f : 4.1f) * r > 3.95f ? 3.95f : (type == 1 ? 4.3f : 4.1f) * r));
        if (type == 2) db += 20.0f * 0.30103f * pw_log2(1.0f + pw_minf(17.0f * r, 16.6f));
        px[n] = (int)f_to_x(hz); py[n++] = clampi(zero - (int)(db * (float)(bot - top) / 54.0f), top, bot);
    }
    curve(px, py, n, bot, c, 2, 1);
    int cx = (int)f_to_x(fc);
    for (int y = top; y < bot; y += 4) fill(cx, y, 1, 2, C_WHITE);
    text(10, top + 3, nm[type], F_S, ui.param_page[s] == 1 ? C_WHITE : C_TEXT);
    char v[16]; pw_format_param((unsigned)s, 0, v);
    text_right(W - 8, top + 3, v, F_S, C_DIM);
}
/* --- EQ: the real response, a dot per band */
static void view_eq(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int top = VY + 6, bot = VY + VH - 6, mid = (top + bot) / 2;
    float scale = (float)(bot - top) / 36.0f;                /* +-18 dB */
    fill(8, mid, W - 16, 1, C_PANEL2);
    for (int d = -12; d <= 12; d += 12) if (d) fill(8, mid - (int)((float)d * scale), W - 16, 1, C_PANEL);
    int px[64], py[64], n = 0;
    for (int i = 0; i < 56; ++i) {
        float hz = 20.0f * pw_exp2((float)i * 9.966f / 55.0f);
        px[n] = (int)f_to_x(hz); py[n++] = clampi(mid - (int)(pw_eq_response(m->p, hz) * scale), top, bot);
    }
    for (int i = 1; i < n; ++i) {
        int y0 = py[i - 1];
        for (int x = px[i - 1]; x < px[i]; x += 2) fill(x, y0 < mid ? y0 : mid, 2, (y0 < mid ? mid - y0 : y0 - mid), dim_of(c));
    }
    for (int i = 1; i < n; ++i) line(px[i - 1], py[i - 1], px[i], py[i], 2, c);
    static const uint8_t fq[3] = {4, 3, 5};
    static const char *const nm[3] = {"LOW", "MID", "HIGH"};
    for (int b = 0; b < 3; ++b) {
        int x = (int)f_to_x(m->p[fq[b]]), y = clampi(mid - (int)(m->p[b] * scale), top, bot);
        fill(x - 3, y - 3, 7, 7, C_WHITE);
        text_center(x, b == 1 ? top + 2 : bot - 12, nm[b], F_XS, C_DIM);
    }
}
/* --- COMB: its peaks, the harmonics of FREQ (odd ones when FBK is negative) */
static void view_comb(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    float fb = m->p[1] * (1.0f - m->p[2] * 0.3f);
    int top = VY + 20, bot = VY + VH - 6;
    int px[121], py[121];
    for (int i = 0; i <= 120; ++i) {
        float th = (float)i / 120.0f * 8.0f;                 /* 0 .. 8 x FREQ */
        float g = 1.0f / pw_maxf(1.0f - 2.0f * fb * pw_cos1(th) + fb * fb, 1e-4f);
        float db = 3.0103f * pw_log2(g * (1.0f - pw_absf(fb)) * (1.0f - pw_absf(fb)));
        px[i] = 8 + i * (W - 16) / 120; py[i] = clampi(bot - (int)((db + 30.0f) * (float)(bot - top) / 34.0f), top, bot);
    }
    curve(px, py, 121, bot, c, 1, 1);
    char v[16]; pw_format_param((unsigned)s, 0, v);
    text(10, VY + 4, v, F_S, C_TEXT);
    text_right(W - 8, VY + 4, fb < 0 ? "ODD HARMONICS" : "ALL HARMONICS", F_XS, C_DIM);
}
/* --- FORMANT OSC: the voice's harmonics under its two formants */
static void view_formant(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    static const float ff[5][2] = {{800, 1150}, {400, 1700}, {270, 2140}, {450, 800}, {325, 700}};
    float v = pw_clamp(m->p[1], 0, 4), t; int a = (int)v; if (a > 3) a = 3; t = v - (float)a;
    float sh = pw_exp2(m->p[3] / 12.0f), f1 = pw_lerp(ff[a][0], ff[a + 1][0], t) * sh, f2 = pw_lerp(ff[a][1], ff[a + 1][1], t) * sh;
    float f0 = 130.8f * pw_exp2(m->p[0] / 12.0f), bw = 60.0f * pw_exp2(m->p[2] * 4.5f);
    int top = VY + 26, bot = VY + VH - 14;
    for (float h = f0; h < 4000.0f; h += f0) {
        float d1 = (h - f1) / bw, d2 = (h - f2) / bw, g = 1.0f / (1.0f + d1 * d1) + 0.5f / (1.0f + d2 * d2);
        int x = 8 + (int)(h / 4000.0f * (float)(W - 16)), hh = (int)(pw_minf(g, 1.0f) * (float)(bot - top));
        fill(x, bot - hh, 2, hh, c);
    }
    fill(8, bot, W - 16, 1, C_LINE);
    text(8, bot + 3, "0", F_XS, C_LINE); text_right(W - 8, bot + 3, "4 KHZ", F_XS, C_LINE);
    char vv[16]; pw_format_param((unsigned)s, 1, vv);
    text(10, VY + 4, vv, F_M, C_WHITE);
}
/* an envelope's shape in a box: attack, decay to sustain, a hold, release; a dot at `live` (level, <0 none) */
static void adsr_shape(int x, int y, int w, int h, float at, float dc, float su, float rl, int hold_decay, uint16_t c, int lit, float live)
{
    int wa = log_width(at) * w / 230, wd = hold_decay ? 0 : log_width(dc) * w / 230, ws = w / 6, wr = log_width(rl) * w / 230;
    int tot = wa + wd + ws + wr, x0 = x + (w - tot) / 2, bot = y + h, sy = bot - (int)(su * (float)h);
    if (hold_decay) sy = y;
    int px[32], py[32], n = 0;
    for (int i = 0; i <= 6; ++i) { float u = (float)i / 6.0f; px[n] = x0 + (int)(u * (float)wa); py[n++] = bot - (int)((1.0f - pw_exp2(-3.0f * u)) / 0.875f * (float)h); }
    for (int i = 1; i <= 8 && wd; ++i) { float u = (float)i / 8.0f, e = pw_exp2(-5.0f * u); px[n] = x0 + wa + (int)(u * (float)wd); py[n++] = sy - (int)((float)(sy - y) * (e - 0.03f) / 0.97f); }
    px[n] = x0 + wa + wd + ws; py[n++] = sy;
    for (int i = 1; i <= 8; ++i) { float u = (float)i / 8.0f, e = pw_exp2(-5.0f * u); px[n] = x0 + wa + wd + ws + (int)(u * (float)wr); py[n++] = bot - (int)((float)(bot - sy) * (e - 0.03f) / 0.97f); }
    fill(x0 - 2, bot, tot + 4, 1, C_LINE);
    curve(px, py, n, bot, lit ? c : mid_of(c), lit ? 2 : 1, 1);
    if (live >= 0) { int ly = bot - (int)(pw_clamp(live, 0, 1) * (float)h); fill(x0 - 6, ly - 2, 5, 5, C_WHITE); }
}
/* --- FM-DX's algorithm tables (the TX81Z's eight; used by view_dx below) */
static const uint8_t fx_mods_ui[8][4] = {       /* FM TX's algorithms (pw_modules.c's fx_mods) */
    {0x2, 0x4, 0x8, 0}, {0x2, 0xC, 0, 0}, {0xA, 0x4, 0, 0}, {0x6, 0, 0x8, 0},
    {0x2, 0, 0x8, 0},   {0x8, 0x8, 0x8, 0}, {0, 0, 0x8, 0}, {0, 0, 0, 0}};
static const uint8_t fx_car_ui[8] = {0x1, 0x1, 0x1, 0x1, 0x5, 0x7, 0x7, 0xF};
static const int8_t fx_pos[8][4][2] = {
    {{48, 98}, {48, 70}, {48, 42}, {48, 14}}, {{48, 98}, {48, 62}, {26, 26}, {70, 26}},
    {{48, 98}, {26, 58}, {26, 18}, {70, 58}}, {{48, 98}, {26, 56}, {70, 56}, {70, 18}},
    {{26, 98}, {26, 56}, {70, 98}, {70, 56}}, {{18, 98}, {48, 98}, {78, 98}, {48, 50}},
    {{18, 98}, {48, 98}, {78, 98}, {78, 56}}, {{12, 98}, {36, 98}, {60, 98}, {84, 98}}};
static void view_fm(int s)   /* FM-2: the modulator above the carrier, each lit by its envelope, and the two envelopes drawn at their time */
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int page = (int)ui.param_page[s];
    float e0 = pw_clamp(pw_live((unsigned)s, 0), 0, 1), e1 = pw_clamp(pw_live((unsigned)s, 1), 0, 1);
    rfill(0, VY, 100, VH, C_PANEL);
    int bx = 28, my = VY + 18, cy = VY + 64;
    line(bx + 22, my + 22, bx + 22, cy - 1, 1, C_DIM);                       /* the modulator into the carrier */
    line(bx + 22, cy - 5, bx + 18, cy - 9, 1, C_DIM); line(bx + 22, cy - 5, bx + 26, cy - 9, 1, C_DIM);
    if (m->p[2] > 0.01f) {                                                    /* the modulator's feedback loop */
        uint16_t fc = C_AMBER;
        fill(bx + 44, my + 8, 8, 1, fc); fill(bx + 51, my - 6, 1, 15, fc); fill(bx + 8, my - 6, 44, 1, fc); fill(bx + 8, my - 6, 1, 5, fc);
    }
    rfill(bx, my, 44, 22, blend(C_PANEL2, C_MOD, (unsigned)(4.0f + 11.0f * e1)));
    rfill(bx, cy, 44, 22, blend(C_PANEL2, c, (unsigned)(4.0f + 11.0f * e0)));
    text_center(bx + 22, my + 5, "MOD", F_S, C_WHITE);
    text_center(bx + 22, cy + 5, "CAR", F_S, C_WHITE);
    char t[16], *o;
    o = s_cpy(t, "x"); pw_format_param((unsigned)s, 0, o);
    text_center(bx + 22, my + 28, t, F_XS, C_TEXT);
    fill(bx + 22, cy + 22, 1, 12, c);
    fill(4, VY + 110, 92, 1, C_LINE);
    text_center(50, VY + 114, "OUT", F_XS, C_DIM);
    for (int k = 0; k < 2; ++k) {                                              /* the envelopes: MOD above, AMP below */
        const float *sh = pw_fm_shape[clampi((int)(m->p[5 - k] + 0.5f), 0, PW_FM_SHAPES - 1)];   /* (k 0: MOD, param 5; k 1: AMP, param 4) */
        float tm = m->p[6], e = k ? e0 : e1;
        int y0 = VY + 2 + (k ? 62 : 0);
        uint16_t oc = k ? c : C_MOD;
        text(108, y0, k ? "AMP" : "MOD", F_XS, (page == 1) ? C_WHITE : oc);
        adsr_shape(108, y0 + 12, 128, 46, sh[0] * tm, sh[1] * tm, sh[2], sh[3] * tm, 0, oc, 1, e > 0.001f ? e : -1.0f);
    }
}
/* --- FM-DX: the algorithm plate (the TX81Z's eight, carriers at the bottom, lit by their envelopes) beside the four operators: ratio, level, and each one's envelope as the chip
 * plays it (middle C, the key held then let go). On an operator's own pages (OP n, OP n ENV) that one up close: ratio, wave, level and its envelope large. */
static float dxg(unsigned w, float u)   /* the OPZ's eight waves at u (0-1 of a cycle): -1..1 */
{
    w &= 7u;
    if (w >= 2u && u >= 0.5f) return 0.0f;
    float ph = w < 4u ? u : w < 6u ? u * 2.0f : (u * 4.0f - (float)(int)(u * 4.0f)) * 0.5f;
    float v = pw_sin1(ph);
    return (w & 1u) ? v * pw_absf(v) : v;
}
static void dx_glyph(int x, int y, int w, int h, unsigned wave, uint16_t c)
{
    int mid = y + h / 2, prev = 0;
    fill(x, mid, w, 1, C_LINE);
    for (int i = 0; i < w; ++i) {
        int yy = -(int)(dxg(wave, (float)i / (float)w) * (float)(h / 2 - 1));
        if (i) line(x + i - 1, mid + prev, x + i, mid + yy, 1, c);
        prev = yy;
    }
}
static char *dx_rat(char *o, float r)   /* a ratio: 1.00, 14.14 */
{
    int v = (int)(r * 100.0f + 0.5f);
    if (v >= 10000) return s_int(o, v / 100);
    o = s_int(o, v / 100); *o++ = '.'; *o++ = (char)('0' + v / 10 % 10); *o++ = (char)('0' + v % 10); *o = 0;
    return o;
}
static void dx_env_box(int x, int y, int w, int h, const float *e, int n, float hold_u, uint16_t c, int thick, float live)
{
    int px[64], py[64];
    for (int i = 0; i < n && i < 64; ++i) { px[i] = x + i * (w - 1) / (n > 1 ? n - 1 : 1); py[i] = y + h - 1 - (int)(pw_clamp(e[i], 0, 1) * (float)(h - 1)); }
    fill(x, y + h, w, 1, C_LINE);
    fill(x + (int)(hold_u * (float)w), y, 1, h, C_LINE);   /* where the key comes up */
    curve(px, py, n < 64 ? n : 64, y + h, c, thick, 1);
    if (live >= 0) fill(x - 6, y + h - 2 - (int)(pw_clamp(live, 0, 1) * (float)(h - 1)), 5, 5, C_WHITE);
}
static void view_dx(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int alg = clampi((int)(m->p[4] + 0.5f), 0, 7), page = (int)ui.param_page[s], op = page >= 2 && page <= 9 ? (page - 2) / 2 : -1;
    int ox = 2, oy = VY + 2;
    float lv[4] = {0, 0, 0, 0}, ra[4] = {1, 1, 1, 1}, sh[4 * 32], secs = 1.0f, hold = 0.5f;
    unsigned wv[4] = {0, 0, 0, 0};
    for (unsigned n = 0; n < 4u; ++n) { unsigned w; if (pw_dx_op((unsigned)s, n, &ra[n], &lv[n], &w)) wv[n] = w; lv[n] = pw_clamp(lv[n] * 3.0f, 0.0f, 1.0f); }   /* (the bar: a level's loudness, stretched) */
    for (int i = 0; i < 4 * 32; ++i) sh[i] = 0.0f;
    pw_dx_shapes((unsigned)s, 32, sh, &secs, &hold);
    float hu = secs > 0.0f ? hold / secs : 0.5f;
    if (op < 0) rfill(0, VY, 100, VH, C_PANEL);
    for (int n = 0; n < 4; ++n)
        for (int k = 0; k < 4; ++k)
            if ((fx_mods_ui[alg][n] >> k) & 1)
                line(ox + fx_pos[alg][k][0], oy + fx_pos[alg][k][1] + 8, ox + fx_pos[alg][n][0], oy + fx_pos[alg][n][1] - 9, 1, C_DIM);
    int x4 = ox + fx_pos[alg][3][0], y4 = oy + fx_pos[alg][3][1];
    if (m->p[5] > 0.5f) {   /* OP 4's feedback loop, brighter with more of it */
        uint16_t fc = op < 0 ? blend(C_DIM, C_AMBER, (unsigned)clampi((int)(m->p[5] * 2.0f), 0, 15)) : C_DIM;
        fill(x4 + 11, y4 - 1, 5, 1, fc); fill(x4 + 15, y4 - 13, 1, 13, fc); fill(x4 - 2, y4 - 13, 18, 1, fc); fill(x4 - 2, y4 - 13, 1, 4, fc);
    }
    for (int n = 0; n < 4; ++n) {
        int bx = ox + fx_pos[alg][n][0] - 11, by = oy + fx_pos[alg][n][1] - 9, car = (fx_car_ui[alg] >> n) & 1;
        float e = pw_clamp(pw_live((unsigned)s, (unsigned)n), 0, 1) * lv[n];
        rfill(bx, by, 22, 18, blend(C_PANEL2, car ? c : C_MOD, (unsigned)(4.0f + 11.0f * e)));
        if (n == op) { frame_rect(bx - 2, by - 2, 26, 22, C_WHITE); frame_rect(bx - 1, by - 1, 24, 20, C_WHITE); }
        char d[2] = {(char)('1' + n), 0};
        text_center(bx + 11, by + 4, d, F_S, C_WHITE);
    }
    fill(ox + 4, oy + 112, 92, 1, C_LINE);
    text_center(ox + 50, oy + 116, "OUT", F_XS, C_DIM);
    char t[28], *o;
    if (op < 0) {   /* the four operators side by side: ratio, level, envelope */
        for (int n = 0; n < 4; ++n) {
            int x = 104 + n * 34, car = (fx_car_ui[alg] >> n) & 1;
            uint16_t oc = car ? c : C_MOD;
            t[0] = (char)('1' + n); t[1] = 0;
            text_center(x + 16, VY + 2, t, F_S, oc);
            o = s_cpy(t, "x"); dx_rat(o, ra[n]);
            text_center(x + 16, VY + 19, t, F_XS, C_TEXT);
            dx_glyph(x + 3, VY + 31, 26, 12, wv[n], mid_of(oc));
            int fy = VY + 46, fh = 38, l = clampi((int)(lv[n] * (float)fh + 0.5f), 0, fh);
            float e = pw_clamp(pw_live((unsigned)s, (unsigned)n), 0, 1);
            fill(x + 11, fy, 11, fh, C_PANEL);
            fill(x + 11, fy + fh - l, 11, l, mid_of(oc));
            int le = (int)(e * (float)l);
            if (le > 0) fill(x + 14, fy + fh - le, 5, le, C_WHITE);
            dx_env_box(x + 2, VY + 92, 30, 28, sh + n * 32, 32, hu, oc, 1, -1.0f);
        }
        return;
    }
    int car = (fx_car_ui[alg] >> op) & 1;
    uint16_t oc = car ? c : C_MOD;
    o = s_cpy(t, "OP "); *o++ = (char)('1' + op); *o = 0;
    text(108, VY + 3, t, F_M, oc);
    text_right(W - 4, VY + 7, car ? "CARRIER" : "MODULATOR", F_XS, C_DIM);
    o = s_cpy(t, "x"); dx_rat(o, ra[op]);
    text(108, VY + 22, t, F_S, C_WHITE);
    dx_glyph(168, VY + 20, 32, 14, wv[op], oc);
    int l = clampi((int)(lv[op] * 128.0f + 0.5f), 0, 128);
    fill(106, VY + 38, 128, 6, C_PANEL);
    fill(106, VY + 38, l, 6, oc);
    float e = pw_clamp(pw_live((unsigned)s, (unsigned)op), 0, 1);
    fill(106, VY + 45, (int)(e * (float)l), 2, C_WHITE);
    dx_env_box(112, VY + 54, 122, 58, sh + op * 32, 32, hu, oc, 2, e * lv[op] > 0.001f ? e : -1.0f);
    { char ts[12], *q = ts; int ms = (int)(secs * 1000.0f + 0.5f); if (ms >= 1000) { q = s_int(q, ms / 1000); if (ms % 1000) { *q++ = '.'; *q++ = (char)('0' + ms % 1000 / 100); } } else q = s_int(q, ms); s_cpy(q, ms >= 1000 ? "S" : "MS"); text_right(W - 4, VY + 114, ts, F_XS, C_DIM); }
}
/* --- DRUM RACK: the eight pads, lit as they sound; a pad's page lights it */
static void view_rack(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int page = (int)ui.param_page[s];
    for (int k = 0; k < 8; ++k) {
        int x = 2 + (k % 4) * 59, y = VY + 4 + (k / 4) * 64;
        float e = pw_clamp(pw_live((unsigned)s, (unsigned)k), 0, 1);
        if (pv_demo) { unsigned ph = (fm1_millis() / 130u + (unsigned)k * 5u) % 12u; e = (k == 0 || k == 3 || k == 5 || k == 6) && ph < 5u ? 1.0f - (float)ph * 0.2f : 0.0f; }
        rfill(x, y, 56, 60, blend(pv_demo ? RGB(34, 35, 46) : C_PANEL, c, (unsigned)(e * 15.0f)));
        if (page == k + 1) { frame_rect(x, y, 56, 60, C_WHITE); frame_rect(x + 1, y + 1, 54, 58, C_WHITE); }
        char d[3] = {(char)('1' + k), 0, 0}, v[16];
        text(x + 4, y + 3, d, F_S, e > 0.4f ? C_BG : C_DIM);
        pw_format_param((unsigned)s, 4u + 4u * (unsigned)k, v);
        text_fit(x + 4, y + 20, v, F_S, e > 0.4f ? C_BG : C_WHITE, 50);
        int lv = clampi((int)(m->p[7 + 4 * k] * 46.0f), 0, 46);
        if (pv_demo) lv = 22 + (k * 7) % 22;
        fill(x + 5, y + 50, 46, 3, C_PANEL2);
        fill(x + 5, y + 50, lv, 3, e > 0.4f ? C_BG : c);
    }
}
/* --- SAMPLER: the sound, its region, loop or slices (even or at its transients), where it plays; on the AMP page
 * its envelope, on the FILTER page the filter's curve, on the WARP page how fast the sound goes */
static uint8_t smp_peaks[220];
static uint32_t smp_sig, smp_frames;
static void view_sampler_pic(int s);
static void view_sampler(int s)
{
    view_sampler_pic(s);
    int st = pw_sampler_rec((unsigned)s, 0);   /* the panel's REC: armed (amber) or recording (red), over the picture */
    if (st == 1 || st == 2) {
        int w = text_w(st == 1 ? "REC ARMED" : "RECORDING", F_S) + 10;
        fill(W - 8 - w, VY + 24, w, 15, st == 1 ? C_AMBER : C_RED);
        text_center(W - 8 - w / 2, VY + 27, st == 1 ? "REC ARMED" : "RECORDING", F_S, C_WELL);
    }
}
static void view_sampler_pic(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int page = (int)ui.param_page[s];
    char v[16];
    pw_format_param((unsigned)s, 0, v);
    text(6, VY + 3, v, F_S, C_WHITE);
    pw_format_param((unsigned)s, 1, v);
    int rx = text_w(v, F_S);
    text_right(W - 6, VY + 3, v, F_S, c);
    if (m->p[24] > 0.5f) { char b[16]; char *o = s_int(b, (int)(m->p[24] + 0.5f)); s_cpy(o, " BEATS"); text_right(W - 12 - rx, VY + 3, b, F_S, C_AMBER); }
    else if (m->p[8] > 0.5f) { char b[16]; char *o = s_cpy(b, "WARP "); pw_format_param((unsigned)s, 9, o); text_right(W - 12 - rx, VY + 3, b, F_S, C_AMBER); }
    if (page == 4) {
        float live = pw_live((unsigned)s, 1);
        adsr_shape(10, VY + 26, 220, 96, m->p[16], m->p[17], m->p[18], m->p[19], 0, c, 1, live >= 0 ? live : -1.0f);
        return;
    }
    if (page == 5) {                             /* a 12 dB low-pass's curve, and how far the envelope opens it */
        float fc = m->p[20], r = m->p[21], k = 2.0f - 1.96f * r;
        int top = VY + 24, bot = VY + VH - 6, zero = top + (bot - top) * 18 / 54;
        int px[56], py[56];
        for (int i = 0; i < 56; ++i) {
            float hz = 20.0f * pw_exp2((float)i * 9.966f / 55.0f), x = hz / fc, re = 1.0f - x * x, im = k * x;
            float db = -3.0103f * pw_log2(pw_maxf(re * re + im * im, 1e-9f));
            px[i] = (int)f_to_x(hz); py[i] = clampi(zero - (int)(db * (float)(bot - top) / 54.0f), top, bot);
        }
        curve(px, py, 56, bot, c, 2, 1);
        float open = fc * pw_exp2(m->p[22] * 6.0f);
        int x0 = (int)f_to_x(fc), x1 = (int)f_to_x(pw_clamp(open, 20.0f, 20000.0f));
        if (x1 != x0) { line(x0, top + 4, x1, top + 4, 2, C_AMBER); fill(x1 - 2, top + 2, 5, 5, C_AMBER); }
        return;
    }
    /* the sound's overview, found again when it changes */
    uint32_t frames = 0;
    { uint8_t tmp[1]; pw_sampler_overview((unsigned)s, tmp, 0, &frames); }
    uint32_t sig = frames * 2654435761u ^ (uint32_t)(m->p[0] * 977.0f) ^ (uint32_t)s * 40503u ^ (pw_sampler_rev((unsigned)s) + 1u) * 2246822519u ^ (uint32_t)(m->p[25] * 3331.0f);
    if (pv_demo) {   /* a sound to look at: a hit, a tail, a second hit */
        for (int i = 0; i < 220; ++i) {
            float u = (float)(i % 110) / 110.0f, env = pw_exp2(-u * 5.0f) * (0.55f + 0.45f * pw_sin1((float)i * 0.31f));
            if (i >= 110) env *= 0.7f;
            smp_peaks[i] = (uint8_t)clampi((int)(pw_absf(env) * 255.0f) + 6, 0, 255);
        }
        smp_frames = 1; smp_sig = ~sig;
    } else if (sig != smp_sig) { pw_sampler_overview((unsigned)s, smp_peaks, 220, &smp_frames); smp_sig = sig; }
    int top = VY + 22, h = VH - 30, mid = top + h / 2, x0 = 10;
    if (!smp_frames) {   /* (what to do about it depends on where this runs, and on the bank) */
        int bk = (int)(m->p[25] + 0.5f);
        if (bk <= 0) text_center(W / 2, mid - 6, "NOTHING BUILT IN: USE RAM OR BANK A-D", F_XS, C_DIM);
#ifdef PW_WEB
        else { static const char *const dz[] = {"", "DROP A FILE ON BANK A BELOW", "DROP A FILE ON BANK B BELOW", "DROP A FILE ON BANK C BELOW", "DROP A FILE ON BANK D BELOW"}; text_center(W / 2, mid - 6, dz[bk > 4 ? 4 : bk], F_XS, C_DIM); }
#else
        else text_center(W / 2, mid - 6, "EMPTY: SEND A SOUND FROM THE WEB PAGE (USB)", F_XS, C_DIM);
#endif
        return;
    }
    float st = m->p[4], en = st + m->p[5] * (1.0f - st);
    int rs = x0 + (int)(st * 220.0f), re = x0 + (int)(en * 220.0f);
    fill(rs, top, re - rs, h, C_PANEL);
    for (int i = 0; i < 220; ++i) {
        int a = smp_peaks[i] * (h / 2) / 255, in = x0 + i >= rs && x0 + i < re;
        fill(x0 + i, mid - a, 1, 2 * a + 1, in ? c : C_LINE);
    }
    int mode = (int)(m->p[1] + 0.5f);
    if (mode == 2) {
        float f; int n = pw_sampler_slice((unsigned)s, 0, &f);
        for (int k = 1; k < n && k < 16; ++k) { pw_sampler_slice((unsigned)s, (unsigned)k, &f); fill(x0 + (int)(f * 220.0f), top, 1, h, C_WHITE); }
        char b[24], *o = s_int(b, n); s_cpy(o, m->p[13] > 0.5f ? " SLICES AT TRANSIENTS" : " EVEN SLICES");
        text(x0, top + h + 1, b, F_XS, C_DIM);
    } else if ((int)(m->p[6] + 0.5f) && mode == 0) {
        fill(rs, top, 2, h, C_AMBER); fill(re - 2, top, 2, h, C_AMBER);
        text(rs + 4, top + 2, (int)(m->p[6] + 0.5f) == 2 ? "<>" : ">", F_XS, C_AMBER);
    } else if (mode == 3) { fill(rs, top, 2, h, C_AMBER); fill(re - 2, top, 2, h, C_AMBER); text(rs + 4, top + 2, "LOOP", F_XS, C_AMBER); }
    if (m->p[24] > 0.5f) {   /* BEATS: a tick for each beat across the region */
        int nb = (int)(m->p[24] + 0.5f);
        for (int k = 0; k <= nb; ++k) { int x = rs + (re - rs) * k / nb; fill(x < x0 + 220 ? x : x0 + 219, top + h - 5, 1, 5, k % 4 ? C_AMBER : C_WHITE); }
    }
    if (m->p[7] > 0.5f) text_right(re - 2, top + 2, "<<", F_XS, C_WHITE);
    float at = pw_live((unsigned)s, 0);
    if (at >= 0) fill(x0 + (int)(at * 220.0f), top - 2, 2, h + 4, C_WHITE);
}
/* --- TRIG SEQ: the grid. Lower keys: the track's steps; OP1-4: the track; GLO + a key: accent, roll */
static void view_trig(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    static const char *const tn[4] = {"A", "B", "C", "D"};
    int x0 = 22, cw = 13, rh = 24, y0 = VY + 4;
    if (!pv_demo) ky_off = 11;   /* (a tall strip: the accent and roll shortcuts get a row) */
    for (int t = 0; t < 4; ++t) {
        int y = y0 + t * rh, len = clampi((int)(m->p[4 + t] + 0.5f), 1, 16), at = (int)pw_live((unsigned)s, (unsigned)t);
        int sel = t == ui.trig_track;
        if (sel) rfill(0, y - 1, W, rh, C_PANEL);
        text_center(10, y + 5, tn[t], F_M, sel ? C_WHITE : C_DIM);
        for (int i = 0; i < 16; ++i) {
            int x = x0 + i * cw + (i / 4) * 2, v = pv_demo ? (m->data[t * 4 + (i >> 2)] >> ((i & 3) * 2)) & 3 : pw_trig_get((unsigned)s, (unsigned)t, (unsigned)i);
            if (i >= len) { fill(x, y + 2, cw - 2, rh - 6, C_WELL); continue; }
            uint16_t bg = (i & 3) == 0 ? C_PANEL2 : C_PANEL;
            if (!v) fill(x, y + 2, cw - 2, rh - 6, bg);
            else {
                fill(x, y + 2, cw - 2, rh - 6, v == 2 ? C_WHITE : c);
                if (v == 3) { fill(x + 2, y + 6, cw - 6, 2, C_BG); fill(x + 2, y + 10, cw - 6, 2, C_BG); fill(x + 2, y + 14, cw - 6, 2, C_BG); }
            }
            if (i == at && (pws.playing || pv_demo)) frame_rect(x - 1, y + 1, cw, rh - 4, C_GREEN);
            if (ui.trig_rec == 1 && i == ui.trig_cur % len) frame_rect(x - 1, y + 1, cw, rh - 4, C_RED);
        }
    }
    if (ui.trig_rec) {
        rfill(150, y0 + 4 * rh + 2, 84, 16, C_RED);
        text_center(192, y0 + 4 * rh + 4, ui.trig_rec == 2 ? "LIVE REC" : "STEP REC", F_S, C_WHITE);
        strip_pre = "KEYS 1-4|Tracks A-D|REC|Stop|";
        strip_post = ui.trig_rec == 2 ? "\nGLO+KEY|Accent|HARD HIT|Accent" : "\nGLO+KEY|Accent|SELECT|Skip a step";
    } else {
        strip_pre = "KEYS|Steps|REC|Record|"; strip_post = "\nGLO+KEY|Accent/Roll|OP1-4|Track";
    }
}
/* --- EUCLID: its steps round a circle, the hits lit */
static void view_euclid(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int steps = clampi((int)(m->p[0] + 0.5f), 1, 32), hits = clampi((int)(m->p[1] + 0.5f), 0, steps), rot = (int)(m->p[2] + 0.5f);
    int cx = W / 2, cy = VY + VH / 2, r = VH / 2 - 10, at = (int)pw_live((unsigned)s, 0);
    for (int i = 0; i < steps; ++i) {
        float a = (float)i / (float)steps;
        int x = cx + (int)(pw_sin1(a) * (float)r), y = cy - (int)(pw_cos1(a) * (float)r);
        int k = ((i + steps - rot % steps) % steps), on = hits > 0 && (k * hits) % steps < hits;
        int sz = steps > 24 ? 6 : 8;
        if (on) rfill(x - sz / 2, y - sz / 2, sz, sz, c); else frame_rect(x - sz / 2, y - sz / 2, sz, sz, C_LINE);
        if (i == at && pws.playing) frame_rect(x - sz / 2 - 2, y - sz / 2 - 2, sz + 4, sz + 4, C_WHITE);
    }
    char b[12], *o = s_int(b, hits); o = s_cpy(o, "/"); s_int(o, steps);
    text_center(cx, cy - 9, b, F_M, C_WHITE);
}
/* --- SEQUENCER: its steps as bars (SEQ: the full editor) */
/* --- SCOPE, MODE SPECTRUM: the last 1024 samples through a 1024-point fixed-point FFT (Hann window), drawn on a
 * log frequency axis, 20 Hz to 20 kHz, and 60 dB of level; each trace falls away slowly, so peaks can be read */
#define SPEC_N 1024u
static int16_t spec_re[SPEC_N] PW_POOL, spec_im[SPEC_N] PW_POOL;
static uint8_t spec_hold[2][W] PW_POOL;
static void spec_fft(void)
{
    for (unsigned i = 1, j = 0; i < SPEC_N; ++i) {          /* bit reversal */
        unsigned bit = SPEC_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { int16_t t = spec_re[i]; spec_re[i] = spec_re[j]; spec_re[j] = t; t = spec_im[i]; spec_im[i] = spec_im[j]; spec_im[j] = t; }
    }
    for (unsigned len = 2; len <= SPEC_N; len <<= 1) {       /* each stage halves, so nothing overflows */
        unsigned half = len >> 1, step = SPEC_N / len;
        for (unsigned k = 0; k < half; ++k) {
            float ang = (float)(k * step) * (1.0f / (float)SPEC_N);
            int32_t c = (int32_t)(pw_cos1(ang) * 32767.0f), sn = (int32_t)(pw_sin1(ang) * 32767.0f);
            for (unsigned i = k; i < SPEC_N; i += len) {
                unsigned j = i + half;
                int32_t tr = (spec_re[j] * c + spec_im[j] * sn) >> 15, ti = (spec_im[j] * c - spec_re[j] * sn) >> 15;
                int32_t ur = spec_re[i], ui = spec_im[i];
                spec_re[i] = (int16_t)((ur + tr) >> 1); spec_im[i] = (int16_t)((ui + ti) >> 1);
                spec_re[j] = (int16_t)((ur - tr) >> 1); spec_im[j] = (int16_t)((ui - ti) >> 1);
            }
        }
    }
}
static float spec_mag(unsigned k)   /* |bin k|, roughly (the larger part plus a third of the smaller) */
{
    float a = pw_absf((float)spec_re[k]), b = pw_absf((float)spec_im[k]);
    return a > b ? a + 0.33f * b : b + 0.33f * a;
}
static void scope_spectrum(const int16_t *const *ring, int probe, uint32_t w, int a_on, int b_on, int top, int h)
{
    static const float freqs[3] = {100.0f, 1000.0f, 10000.0f};
    static const char *const flab[3] = {"100", "1K", "10K"};
    fill(0, top, W, h, RGB(8, 14, 12));
    for (int g = 1; g < 4; ++g) fill(0, top + g * h / 4, W, 1, RGB(24, 40, 32));
    for (int g = 0; g < 3; ++g) {
        int x = (int)f_to_x(freqs[g]);
        fill(x, top, 1, h, RGB(24, 40, 32));
        text(x + 2, top + h - 24, flab[g], F_XS, C_DIM);
    }
    text_right(W - 4, top + 1, "0 DB", F_XS, C_DIM);
    text_right(W - 4, top + h / 4 + 1, "-15", F_XS, C_DIM);
    text_right(W - 4, top + h / 2 + 1, "-30", F_XS, C_DIM);
    const float df = (float)PW_SR / (float)SPEC_N;
    for (int ch = 0; ch < (probe ? 1 : 2); ++ch) {
        if (!probe && !(ch ? b_on : a_on)) continue;
        uint16_t c = probe || !ch ? cat_color[PW_C_UTIL] : cat_color[PW_C_FILTER];
        for (unsigned n = 0; n < SPEC_N; ++n) {   /* the last SPEC_N samples, windowed */
            unsigned i = (w - SPEC_N + n);
            float v = probe ? (float)pws.scope[i & (PW_SCOPE - 1u)] : (float)ring[ch][i & (PW_SCOPE_RING - 1u)];
            spec_re[n] = (int16_t)(v * (0.5f - 0.5f * pw_cos1((float)n * (1.0f / (float)SPEC_N))));
            spec_im[n] = 0;
        }
        spec_fft();
        int ph0 = -1;
        for (int x = 8; x < W - 8; ++x) {
            float f0 = 20.0f * pw_exp2((float)(x - 8) * (9.966f / (float)(W - 16))), f1 = 20.0f * pw_exp2((float)(x - 7) * (9.966f / (float)(W - 16)));
            float b0 = f0 / df, b1 = f1 / df, m;
            if (b1 - b0 < 1.0f) {                       /* below one bin a pixel: interpolate between the bins */
                float bc = 0.5f * (b0 + b1); unsigned k = (unsigned)bc; float fr = bc - (float)k;
                m = spec_mag(k) * (1.0f - fr) + spec_mag(k + 1u) * fr;
            } else {                                     /* above: the loudest bin in it */
                m = 0;
                for (unsigned k = (unsigned)b0; k <= (unsigned)b1 && k < SPEC_N / 2u; ++k) { float v = spec_mag(k); if (v > m) m = v; }
            }
            float db = 6.0206f * pw_log2(pw_maxf(m, 0.5f) * (1.0f / 3750.0f));   /* 0 dB: a 5 V sine */
            int hp = clampi((int)((db + 60.0f) * (float)h * (1.0f / 60.0f)), 0, h - 1);
            uint8_t *hold = &spec_hold[ch][x];
            if (hp >= *hold) *hold = (uint8_t)hp; else if (*hold > 2) *hold = (uint8_t)(*hold - 2); else *hold = (uint8_t)hp;
            int top2 = *hold > hp ? *hold : hp;
            if (top2 > 0 && !ch) fill(x, top + h - top2, 1, top2, RGB(30, 90, 62));   /* (A fills; B is a line over it) */
            { int a = ph0 < 0 ? hp : (ph0 < hp ? ph0 : hp), b = ph0 < 0 ? hp : (ph0 > hp ? ph0 : hp); fill(x, top + h - b - 1, 1, b - a + 2, c); ph0 = hp; }
        }
    }
}
/* --- SCOPE: A and B (or, nothing patched, the output the patch cursor was last on), from a rising edge */
static void view_scope(int s)
{
    const pw_module *m = &pws.mod[s];
    int zoom = 1 << clampi((int)(m->p[0] + 0.5f), 0, 3), trig = clampi((int)(m->p[2] + 0.5f), 0, 2);
    static const float vdivs[3] = {0.5f, 1.0f, 2.5f};
    float vdiv = vdivs[clampi((int)(m->p[1] + 0.5f), 0, 2)];
    int a_on = pw_input_cables((unsigned)s, 0) > 0, b_on = pw_input_cables((unsigned)s, 1) > 0;
    int top = VY + 18, h = VY + VH - 4 - top, mid = top + h / 2, div = h / 6;
    fill(0, top, W, h, RGB(8, 14, 12));
    for (int gx = 0; gx < W; gx += 30) fill(gx, top, 1, h, RGB(24, 40, 32));
    for (int gy = 0; gy <= 6; ++gy) fill(0, top + gy * div, W, 1, gy == 3 ? RGB(40, 70, 55) : RGB(24, 40, 32));
    if (pv_demo) {   /* the add list's preview: two made-up traces, A a wave, B its echo as a square */
        text(6, VY + 2, "A", F_S, cat_color[PW_C_UTIL]); text(22, VY + 2, "B", F_S, cat_color[PW_C_FILTER]);
        float ph0 = pv_t() * 3.0f;
        int pa = mid, pb = mid;
        for (int x = 0; x < W; ++x) {
            float u = (float)x / 60.0f + ph0;
            int ya = mid - (int)((0.7f * pw_sin1(u) + 0.2f * pw_sin1(u * 3.0f)) * (float)div * 2.2f), yb = mid - (int)((pw_sin1(u * 0.5f) > 0 ? 1.0f : -1.0f) * (float)div * 1.2f);
            if (x) { fill(x, pa < ya ? pa : ya, 1, (pa < ya ? ya - pa : pa - ya) + 1, cat_color[PW_C_UTIL]); fill(x, pb < yb ? pb : yb, 1, (pb < yb ? yb - pb : pb - yb) + 1, cat_color[PW_C_FILTER]); }
            pa = ya; pb = yb;
        }
        return;
    }
    char lab[28];
    const int16_t *ring[2] = {0, 0};
    uint32_t w = 0, n = PW_SCOPE_RING;
    int probe = !a_on && !b_on;
    if (probe) {   /* the engine's tap on the probed output */
        int ps = ui.probe_slot, pp = ui.probe_port;
        if (ps < 0 || ps >= PW_MAX_MODULES || pws.mod[ps].used != 1 || pp >= pw_types[pws.mod[ps].type].n_out || ps == s) {
            text_center(W / 2, mid - 14, "PATCH A OR B,", F_S, C_DIM);
            text_center(W / 2, mid + 2, "OR PUT THE PATCH CURSOR ON AN OUTPUT", F_XS, C_DIM);
            return;
        }
        if (pws.scope_mod != ps || pws.scope_port != pp) pw_set_scope((unsigned)ps, (unsigned)pp);
        port_label((unsigned)ps, 1, (unsigned)pp, lab);
        text(6, VY + 2, lab, F_S, cat_color[pw_types[pws.mod[ps].type].cat]);
        text_right(W - 6, VY + 3, "PROBE", F_XS, C_DIM);
        n = PW_SCOPE; w = pws.scope_w;
        if (zoom > 4) zoom = 4;
    } else {
        ring[0] = pw_scope_ring((unsigned)s, 0, &w); ring[1] = pw_scope_ring((unsigned)s, 1, &w);
        if (!ring[0]) { text_center(W / 2, mid, "NO MEMORY", F_S, C_RED); return; }
        if (a_on) { int x = text(6, VY + 2, "A", F_S, cat_color[PW_C_UTIL]); (void)x; }
        if (b_on) text(22, VY + 2, "B", F_S, cat_color[PW_C_FILTER]);
    }
    if (m->p[3] > 0.5f) { scope_spectrum(ring, probe, w, a_on, b_on, top, h); return; }
    /* trigger: the last rising zero crossing of A (or B) that leaves a whole screen after it */
    unsigned span = (unsigned)(W * zoom), mask = n - 1u, start = (w - span - 1u) & mask;
    int tch = trig == 1 && b_on ? 1 : 0;
    #define SMP(ch, i) (probe ? (float)pws.scope[(i) & mask] * (1.0f / 3000.0f) : (float)ring[ch][(i) & mask] * (1.0f / 3000.0f))
    if (trig < 2)
        for (unsigned back = span + 2u; back < n - 2u; ++back) {
            unsigned i = (w - back) & mask;
            if (SMP(tch, i) <= 0 && SMP(tch, i + 1u) > 0) { start = i; break; }
        }
    for (int ch = 0; ch < (probe ? 1 : 2); ++ch) {
        if (!probe && !(ch ? b_on : a_on)) continue;
        uint16_t c = probe || !ch ? cat_color[PW_C_UTIL] : cat_color[PW_C_FILTER];
        int prev = mid;
        for (int x = 0; x < W; ++x) {
            float lo = 1e9f, hi = -1e9f;
            for (int z = 0; z < zoom; ++z) { float v = SMP(ch, start + (unsigned)(x * zoom + z)); if (v < lo) lo = v; if (v > hi) hi = v; }
            int y0 = clampi(mid - (int)(hi / vdiv * (float)div), top, top + h - 1), y1 = clampi(mid - (int)(lo / vdiv * (float)div), top, top + h - 1);
            if (x) { int a = prev < y0 ? prev : y0, b = prev > y1 ? prev : y1; fill(x, a, 1, b - a + 1, c); }
            prev = (y0 + y1) / 2;
        }
    }
    #undef SMP
}
/* --- CV RECORDER: the recorded pass, the playhead running along it */
static void view_cvrec(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int mode = clampi((int)(m->p[0] + 0.5f), 0, 2), rec = mode > 0 && pw_live((unsigned)s, 2) > 0.5f;
    char v[16];
    int x = text(8, VY + 4, rec ? (mode == 2 ? "DUBBING" : "RECORDING") : "PLAYING", F_S, rec ? C_RED : C_TEXT);
    pw_format_param((unsigned)s, 1, v);
    { char t[24], *o = s_cpy(t, v); s_cpy(o, (int)(m->p[1] + 0.5f) == 1 ? " BEAT" : " BEATS"); text(x + 10, VY + 5, t, F_XS, C_DIM); }
    pw_format_param((unsigned)s, 4, v);
    { char t[20], *o = s_cpy(t, "SPEED "); s_cpy(o, v); text_right(W - 8, VY + 5, t, F_XS, C_DIM); }
    int top = VY + 22, bot = VY + VH - 8, mid = (top + bot) / 2, x0 = 8, w = W - 16;
    fill(x0, mid, w, 1, C_PANEL2);
    int px[49], py[49];
    for (int k = 0; k <= 48; ++k) {
        int d = m->data[k % 48];
        px[k] = x0 + k * w / 48;
        py[k] = bot - (d * (bot - top)) / 255;
    }
    curve(px, py, 49, mid, rec ? C_RED : c, 2, 0);
    float ph = pw_live((unsigned)s, 0);
    int hx = x0 + (int)(pw_clamp(ph, 0, 1) * (float)w);
    fill(hx, top, 1, bot - top, C_WHITE);
    float out = pw_live((unsigned)s, 1);
    int oy = clampi(mid - (int)(out * (float)(bot - top) / 10.0f), top, bot);
    fill(hx - 2, oy - 2, 5, 5, C_WHITE);
}
/* --- GRANULAR: the recording, now at the right and the past to the left (or the bank sound), its grains on it */
static void view_grain(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int src = (int)(m->p[8] + 0.5f), frozen = m->p[6] > 0.5f;
    char v[16];
    if (src > 0) { char *o = s_cpy(v, "B"); s_int(o, src); } else s_cpy(v, frozen ? "LIVE IN, FROZEN" : "LIVE IN");
    text(8, VY + 4, v, F_S, frozen ? C_AMBER : C_TEXT);
    text_right(W - 8, VY + 5, src > 0 ? "START > END" : "PAST < NOW", F_XS, C_DIM);
    int top = VY + 24, bot = VY + VH - 8, x0 = 8, w = W - 16, mid = (top + bot) / 2;
    rfill(x0, top, w, bot - top, C_PANEL);
    float pos = m->p[0], spread = m->p[4];
    int wx = src > 0 ? x0 + (int)(pos * (float)w) : x0 + w - (int)(pos * (float)w);
    int ww = (int)(spread * 0.5f * (float)w); if (ww < 2) ww = 2;
    {   /* where the grains come from: POS, give or take half the SPREAD */
        int a = clampi(wx - ww, x0, x0 + w - 2), b = clampi(wx + ww, a + 2, x0 + w);
        fill(a, top + 2, b - a, bot - top - 4, mid_of(c));
        uint8_t pk[56];   /* the sound the grains are cut from, over it: its loudness along the strip (brighter where the grains come from) */
        if (pw_grain_peaks((unsigned)s, pk, 56)) {
            int half = (bot - top) / 2 - 3;
            for (int i = 0; i < 56; ++i) {
                int h = (pk[i] * half + 127) / 255, x = x0 + i * w / 56 + 1; if (h < 1) h = 1;
                fill(x, mid - h, w / 56 - 1, 2 * h, x + 2 > a && x < b ? c : C_LINE);
            }
        }
    }
    float head = pw_live((unsigned)s, 8);
    for (unsigned k = 0; k < 8; ++k) {
        float g = pw_live((unsigned)s, k);
        if (g < 0) continue;
        float at = src > 0 ? g : 1.0f - (head - g < 0 ? head - g + 1.0f : head - g);
        int gx = x0 + (int)(pw_clamp(at, 0, 1) * (float)w), gy = mid - 24 + (int)((k * 37u) % 48u);
        fill(gx - 2, gy - 2, 5, 5, C_WHITE);
    }
    if (src <= 0) fill(x0 + w - 2, top, 2, bot - top, frozen ? C_AMBER : C_RED);
}
/* --- ROOM MODES: the pitches the room rings at, along a pitch axis (low to high, left to right, from just under ROOT to nearly five times it). Each mode is a bar at its own pitch, as
 * tall as it rings now; the left channel's modes go up, the right's down. The ticks along the foot are where a harmonic series on the
 * same root would sit: the modes are not on them, which is what makes it a room and not a string. */
static void view_room(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int nm = clampi((int)(m->p[2] + 0.5f), 1, 8);
    char v[24], *o = s_cpy(v, "ROOT "); o = s_int(o, (int)(m->p[0] + 0.5f)); s_cpy(o, " Hz");
    text(8, VY + 4, v, F_S, C_TEXT);
    o = s_int(v, nm); s_cpy(o, nm == 1 ? " MODE" : " MODES");
    text_right(W - 8, VY + 5, v, F_XS, C_DIM);
    int top = VY + 22, bot = VY + VH - 24, x0 = 8, w = W - 16, mid = (top + bot) / 2, half = (bot - top) / 2 - 3;
    rfill(x0, top, w, bot - top, C_PANEL);
    fill(x0, mid, w, 1, C_LINE);
    text(x0 + 3, top + 1, "L", F_XS, C_DIM); text(x0 + 3, bot - 9, "R", F_XS, C_DIM);
    for (int n = 1; n <= 6; ++n) {   /* the harmonic series, along the foot */
        float u = pw_log2(m->p[0] * (float)n / (m->p[0] * 0.7f)) / 2.78f;
        if (u < 0.0f || u > 1.0f) continue;
        fill(x0 + (int)(u * (float)(w - 1)), bot + 2, 1, 4, n == 1 ? C_DIM : C_LINE);
    }
    for (unsigned k = 0; k < 8u; ++k) {
        float hz = pw_live((unsigned)s, 8u + k);
        if (hz <= 0.0f) continue;
        float u = pw_log2(hz / (m->p[0] * 0.7f)) / 2.78f, lv = pw_live((unsigned)s, k);
        if (u < 0.0f || u > 1.0f) continue;
        lv = pw_clamp(lv, 0.0f, 1.0f); lv = lv * (2.0f - lv);   /* (a little lift, so quiet rings still show) */
        int x = x0 + (int)(u * (float)(w - 1)), up = !(k & 1), h = (int)(lv * (float)half);
        fill(x, up ? mid - half : mid + 1, 1, half, dim_of(c));
        if (h > 0) fill(x - 1, up ? mid - h : mid + 1, 3, h, c);
        fill(x - 1, up ? mid - h - 1 : mid + h + 1, 3, 2, C_WHITE);
    }
    text_right(W - 8, bot + 3, "TICKS: HARMONICS", F_XS, C_DIM);
}
/* --- VOCODER: the twelve bands, low to high; each bar is how loud that band of the modulator is now (what the carrier's band is turned up to). SHIFT slides
 * which modulator band drives which carrier band: the bars stay where the modulator is. With nothing on MOD ("SELF") a band is driven by its neighbour. */
static void view_vocoder(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int self = 1, sh = (int)(m->p[1] + (m->p[1] < 0 ? -0.5f : 0.5f));
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) if (pws.cable[c].used && pws.cable[c].dm == (uint8_t)s && pws.cable[c].di == 1) self = 0;
    text(8, VY + 4, self ? "SELF: bands drive their neighbours" : "MOD shapes the CARRIER", F_S, C_TEXT);
    if (sh) { char v[16], *o = s_cpy(v, "SHIFT "); if (sh > 0) *o++ = '+'; o = s_int(o, sh); *o = 0; text_right(W - 8, VY + 5, v, F_XS, C_AMBER); }
    int top = VY + 22, bot = VY + VH - 26, x0 = 8, w = W - 16, bw = w / 12, h = bot - top;
    rfill(x0, top, w, h, C_PANEL);
    for (int k = 0; k < 12; ++k) {
        float lv = pw_clamp(pw_live((unsigned)s, (unsigned)k), 0.0f, 1.0f);
        lv = lv * (2.0f - lv);   /* (a little lift, so quiet bands still show) */
        int bh = (int)(lv * (float)(h - 4)), x = x0 + k * bw + 2;
        fill(x, top + 2, bw - 4, h - 4, dim_of(c));
        if (bh > 0) fill(x, bot - 2 - bh, bw - 4, bh, c);
        fill(x, bot - 3 - bh, bw - 4, 2, C_WHITE);
    }
    text(x0, bot + 3, "120 Hz", F_XS, C_DIM); text_right(W - 8, bot + 3, "6 kHz", F_XS, C_DIM);
}
/* --- GENDY: the wave itself, one cycle across: its corners (dots) and the lines between them. Dim copies behind are the shape a moment ago, so what the
 * CHAOS knobs do shows as the corners wandering: A CHAOS moves them up and down, T CHAOS left and right. The top and bottom rails are the walls they bounce off.
 * DIST is how the steps are drawn: LINEAR, any size; CAUCHY, mostly small with leaps; LOGIST, small and bell-like; ARCSIN, nearly always big; EXPON, tiny. */
static void view_gendy(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    static const char *const dn[] = {"LINEAR", "CAUCHY", "LOGIST", "ARCSIN", "EXPON"}, *const rn[] = {"AUDIO", "LOW", "CV"};
    int n = clampi((int)(m->p[1] + 0.5f), 3, 16), kd = clampi((int)(m->p[6] + 0.5f), 0, 4), rg = clampi((int)(m->p[7] + 0.5f), 0, 2);
    static float gh[2][33]; static int gt, gs = -1;
    float cur[33];
    for (unsigned k = 0; k < 33u; ++k) cur[k] = pw_live((unsigned)s, k);
    if (gs != s) { for (int g = 0; g < 2; ++g) for (int k = 0; k < 33; ++k) gh[g][k] = cur[k]; gs = s; gt = 0; }
    if (++gt >= 10) { gt = 0; for (int k = 0; k < 33; ++k) { gh[1][k] = gh[0][k]; gh[0][k] = cur[k]; } }
    char v[24], *o = s_cpy(v, "POINTS "); s_int(o, n);
    text(8, VY + 4, v, F_S, C_TEXT);
    o = s_cpy(v, dn[kd]); o = s_cpy(o, " / "); s_cpy(o, rn[rg]);
    text_right(W - 8, VY + 5, v, F_XS, C_DIM);
    int top = VY + 22, bot = VY + VH - 24, x0 = 8, w = W - 16, mid = (top + bot) / 2, half = (bot - top) / 2 - 4;
    rfill(x0, top, w, bot - top, C_PANEL);
    for (int x = x0; x < x0 + w; x += 4) { fill(x, top + 2, 2, 1, C_LINE); fill(x, bot - 3, 2, 1, C_LINE); }   /* the walls */
    fill(x0, mid, w, 1, C_LINE);
    for (int g = 2; g >= 0; --g) {
        const float *q = g == 2 ? cur : gh[g];
        uint16_t lc = g == 2 ? c : blend(C_PANEL, c, g == 0 ? 7u : 4u);
        int px = 0, py = 0;
        for (int i = 0; i <= n; ++i) {
            int j = i == n ? 0 : i;
            int x = x0 + 2 + (int)((i == n ? 1.0f : q[16 + i]) * (float)(w - 5)), y = mid - (int)(pw_clamp(q[j], -1.0f, 1.0f) * (float)half);
            if (i) line(px, py, x, y, g == 2 ? 2 : 1, lc);
            px = x; py = y;
        }
        if (g == 2) for (int i = 0; i < n; ++i) {
            int x = x0 + 2 + (int)(q[16 + i] * (float)(w - 5)), y = mid - (int)(pw_clamp(q[i], -1.0f, 1.0f) * (float)half);
            fill(x - 2, y - 2, 5, 5, C_WHITE);
        }
    }
    int hx = x0 + 2 + (int)(pw_clamp(cur[32], 0.0f, 1.0f) * (float)(w - 5));
    fill(hx, top, 1, bot - top, C_DIM);
    text(x0, bot + 4, "UP/DOWN: A CHAOS", F_XS, C_DIM); text_right(W - 8, bot + 4, "SIDEWAYS: T CHAOS", F_XS, C_DIM);
}
static int seq_legato_of(uint8_t fb) { return (fb >> 2) & 3; }   /* 0 none, 1 tie, 2 glide */
static uint16_t seq_gates_of(const pw_module *m)   /* the steps that play: gate on, and a velocity above 0 (velocity 0 is off) */
{
    uint16_t g = (uint16_t)(m->data[16] | (m->data[17] << 8));
    for (unsigned k = 0; k < 16; ++k) if (!m->data[PW_SEQ_STEPVEL + k]) g = (uint16_t)(g & ~(1u << k));
    return g;
}
static void edit_caption(int y)   /* "Press SEQ to edit steps" under a sequencer's preview, the key on its amber plate as in the strip */
{
    int w1 = text_w("Press", F_S), w2 = key_w("SEQ"), w3 = text_w("to edit steps", F_S), x = (W - (w1 + 5 + w2 + 5 + w3)) / 2;
    text(x, y, "Press", F_S, C_DIM);
    key_tag(x + w1 + 5, y, "SEQ", C_AMBER);
    text(x + w1 + 5 + w2 + 5, y, "to edit steps", F_S, C_DIM);
}
static void view_seq(int s)   /* the SEQUENCER's step page, smaller: a tile a step, the pitch a bar up from the bottom (solid when the gate is on, an outline when off, as solid as the velocity), ties and glides joined */
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[PW_C_SEQ];
    int steps = clampi((int)(m->p[0] + 0.5f), 1, 16), at = (int)pw_live((unsigned)s, 0);
    uint16_t g = seq_gates_of(m);
    int small = ui.page == PG_EDIT;   /* on the module page it is a preview, half size, so the words under it are what you read */
    int tw = small ? 17 : 27, th = small ? 36 : 54, px = small ? 19 : 29, py = small ? 40 : 58, ox = small ? (W - 7 * px - tw) / 2 : 6, oy = small ? VY + (VH - 22 - 2 * th - 4) / 2 : VY + 2;
    int hmax = th - 8;
    for (int i = 0; i < 16; ++i) {
        int x = ox + (i % 8) * px, y = oy + (i / 8) * py, on = (g >> i) & 1, act = i < steps, p = (int8_t)m->data[i];
        int bh = 2 + clampi(36 + p, 0, 72) * hmax / 72, lg = seq_legato_of(m->data[PW_SEQ_STEPFEAT + i]), bx = x + (small ? 2 : 3), bw = tw - (small ? 4 : 6), by = y + th - 3;
        rfill(x, y, tw, th, act ? C_PANEL : C_WELL);
        if (act) {
            uint16_t bc = blend(C_PANEL, c, 4u + (unsigned)m->data[PW_SEQ_STEPVEL + i] * 11u / 255u);
            if (on) fill(bx, by - bh, bw, bh, bc); else frame_rect(bx, by - bh, bw, bh, C_LINE);
            if (on && lg && i > 0 && (i & 7) && ((g >> (i - 1)) & 1)) {
                int pb = 2 + clampi(36 + (int8_t)m->data[i - 1], 0, 72) * hmax / 72;
                line(bx - (small ? 4 : 8), by - pb, bx, by - bh, 2, lg == 2 ? C_AMBER : c);
            }
            if (lg) fill(bx, y + 3, bw, 2, C_AMBER);
        }
        if (i == at && (pws.playing || pv_demo) && act) frame_rect(x - 1, y - 1, tw + 2, th + 2, C_GREEN);
    }
}
/* --- PIANO ROLL: the whole loop in a small picture, a bar a note (the color its voice), the loop's bars marked and the playhead running */
static void view_roll(int s)
{
    const pw_module *m = &pws.mod[s];
    int loop = (int)pw_roll_loop(m), top = VY + 6, bot = VY + VH - 38, x0 = 8, w = W - 16;
    unsigned n = pw_roll_count(m), lo = 127, hi = 0;
    for (unsigned k = 0; k < n; ++k) { unsigned p; pw_roll_get(m, k, 0, 0, &p, 0, 0); if (p < lo) lo = p; if (p > hi) hi = p; }
    int demo = pv_demo && !n;   /* (the add list's preview: a made-up chord and tune) */
    if (demo) { lo = 60; hi = 67; loop = 12; }   /* (few notes, the picture filled) */
    if (!n && !demo) { lo = 55; hi = 72; }
    int span = (int)hi - (int)lo + 3;
    if (span < 13 && !demo) span = 13;
    int base = (int)lo - 1 - (span - ((int)hi - (int)lo + 3)) / 2;
    fill(x0, top, w, bot - top, C_WELL);
    for (int b = 0; b < loop; b += 4) fill(x0 + b * w / loop, top, 1, bot - top, b % 16 ? C_PANEL2 : C_LINE);
    static const uint8_t vcol[4] = {PW_C_SEQ, PW_C_FX, PW_C_FILTER, PW_C_MOD};
    int rh = (bot - top) / span; if (rh < 2) rh = 2;
    unsigned cnt = demo ? 5u : n;
    for (unsigned k = 0; k < cnt; ++k) {
        unsigned st, ln, pt, vl, vo;
        if (demo) { static const uint8_t d[5][4] = {{0, 4, 60, 0}, {0, 4, 64, 1}, {4, 4, 67, 0}, {8, 2, 64, 1}, {10, 2, 62, 0}}; st = d[k][0]; ln = d[k][1]; pt = d[k][2]; vo = d[k][3]; vl = 24; }
        else pw_roll_get(m, k, &st, &ln, &pt, &vl, &vo);
        int y = bot - ((int)pt - base + 1) * (bot - top) / span;
        int xa = x0 + (int)st * w / loop, xb = x0 + (int)(st + ln) * w / loop;
        if ((int)st >= loop) continue;
        fill(xa, y, xb - xa - 1 > 1 ? xb - xa - 1 : 1, rh > 3 ? rh - 1 : rh, cat_color[vcol[vo & 3]]);
    }
    int ph = pv_demo ? (int)(pv_t() * 8.0f) % loop : (int)(pws.sixteenths % (unsigned)loop);
    if (pws.playing || pv_demo) fill(x0 + ph * w / loop, top, 2, bot - top, C_GREEN);
    char b[16], *o = s_int(b, loop / 16); s_cpy(o, loop == 16 ? " BAR" : " BARS");
    text(x0, bot + 4, b, F_S, C_DIM);
    o = s_int(b, (int)(n ? n : (unsigned)cnt)); s_cpy(o, " NOTES");
    text_right(W - 8, bot + 4, b, F_S, C_DIM);
}
/* --- DRUM SYNTH: the pitch falling, the tone's and the noise's fades */
static void view_drum(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    float tune = m->p[0], sweep = m->p[1], st = m->p[2], dec = m->p[3], noise = m->p[5], nd = m->p[7];
    float span = pw_clamp(pw_maxf(dec, nd) * 1.6f, 0.05f, 4.0f);
    int top = VY + 18, bot = VY + VH - 6, h = bot - top, x0 = 8, w = W - 16;
    int px[33], py[33], qx[33], qy[33], fx[33], fy[33];
    for (int i = 0; i <= 32; ++i) {
        float t = (float)i / 32.0f * span;
        px[i] = qx[i] = fx[i] = x0 + i * w / 32;
        py[i] = bot - (int)(pw_exp2(-t / pw_maxf(dec, 0.005f) * 1.4427f * 2.0f) * (float)h);
        qy[i] = bot - (int)(noise * pw_exp2(-t / pw_maxf(nd, 0.005f) * 1.4427f * 2.0f) * (float)h);
        float semis = sweep * pw_exp2(-t / pw_maxf(st, 0.001f) * 1.4427f);
        float oct = pw_clamp(pw_log2(tune * pw_exp2(semis / 12.0f) / 20.0f) / 9.0f, 0, 1);
        fy[i] = bot - (int)(oct * (float)h);
    }
    curve(px, py, 33, bot, c, 2, 1);
    if (noise > 0.01f) curve(qx, qy, 33, bot, C_DIM, 1, 0);
    curve(fx, fy, 33, bot, C_AMBER, 1, 0);
    fill(x0, bot, w, 1, C_LINE);
    text(8, VY + 3, "TONE", F_XS, c); text(44, VY + 3, "NOISE", F_XS, C_DIM); text(84, VY + 3, "PITCH", F_XS, C_AMBER);
    char b[16]; pw_param_text(&pw_types[PW_T_DRUMS].param[3], span, b);
    text_right(W - 8, VY + 3, b, F_XS, C_LINE);
}
/* --- the mixers: a fader per channel, what comes in lit beside it */
static void view_mixer(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    const int n = m->type == PW_T_MIX8 ? 8 : 4, pitch = 224 / n, fw = n == 8 ? 8 : 12, pg = (int)ui.param_page[s], lev = pg < n / 4;
    for (int k = 0; k < n; ++k) {
        int x = 8 + k * pitch + (pitch - fw) / 2, top = VY + 22, h = VH - 34;
        char b[24] = {'I', 'N', ' ', (char)('1' + k), 0};
        int from = -1;   /* what plays into this channel: its module's name over the fader (IN k when nothing does) */
        for (unsigned c = 0; c < PW_MAX_CABLES && from < 0; ++c) {
            const pw_cable *cb = &pws.cable[c];
            if (cb->used && cb->dm == s && cb->di == (unsigned)k) from = cb->sm;
        }
        if (from >= 0) pw_module_label((unsigned)from, b);
        text_fit(8 + k * pitch, VY + 4, b, F_XS, from >= 0 ? C_TEXT : C_DIM, pitch - 1);
        int lv = clampi((int)(m->p[k] * (float)h), 0, h), in = clampi(m->in_level[k] * h / 255, 0, h);
        fill(x, top, fw, h, C_PANEL);
        fill(x, top + h - lv, fw, lv, lev ? c : mid_of(c));
        fill(x + fw + 1, top + h - in, 2, in, C_GREEN);
        int px = x + (int)((m->p[n + k] + 1.0f) * 0.5f * (float)(fw - 3));
        fill(x - 2, VY + VH - 7, fw + 4, 1, C_LINE);
        fill(px, VY + VH - 10, 3, 7, lev ? C_DIM : C_WHITE);
    }
}
static void view_pmod(int s)   /* PARAM MOD: each channel's voltage, the knob it moves, and how far it has moved it */
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int pg = (int)ui.param_page[s];
    for (int k = 0; k < 4; ++k) {
        int y0 = VY + 1 + k * 31;
        char src[16] = {'C', 'V', ' ', (char)('1' + k), 0}, a[16], b[16], t[28], *o;
        int from = -1;
        for (unsigned q = 0; q < PW_MAX_CABLES && from < 0; ++q) {
            const pw_cable *cb = &pws.cable[q];
            if (cb->used && cb->dm == s && cb->di == (unsigned)k) from = cb->sm;
        }
        if (from >= 0) pw_module_label((unsigned)from, src);
        int tg = pw_pm_target((unsigned)s, (unsigned)k), live = tg >= 0 && from >= 0;
        rfill(2, y0, 236, 28, C_PANEL);
        if (k == pg) { frame_rect(1, y0 - 1, 238, 30, C_WHITE); }
        text_fit(6, y0 + 3, src, F_XS, from >= 0 ? C_TEXT : C_DIM, 70);
        line(78, y0 + 8, 90, y0 + 8, 1, C_DIM); line(86, y0 + 5, 90, y0 + 8, 1, C_DIM); line(86, y0 + 11, 90, y0 + 8, 1, C_DIM);
        pw_format_param((unsigned)s, 4u * (unsigned)k, a); pw_format_param((unsigned)s, 4u * (unsigned)k + 1u, b);
        o = s_cpy(t, a); *o++ = ' '; *o++ = ' '; s_cpy(o, b);
        text_fit(96, y0 + 3, tg >= 0 ? t : "-", F_S, tg >= 0 ? C_WHITE : C_DIM, 138);
        float off = pw_clamp(pw_live((unsigned)s, (unsigned)k), -1.0f, 1.0f);
        int cx = 6 + 114, hw = 110, w = (int)(off * (float)hw);
        fill(6, y0 + 19, 2 * hw + 4, 5, C_PANEL2);
        fill(cx + 2, y0 + 17, 1, 9, C_LINE);
        if (live) { if (w >= 0) fill(cx + 2, y0 + 19, w, 5, c); else fill(cx + 2 + w, y0 + 19, -w, 5, c); }
    }
}
/* --- the oscillators: their waves (a dot runs round an LFO's) */
static void wave_shape(int kind, float pw, float ph, int *y, int amp)
{
    switch (kind) {
    case 0: *y = -(int)(pw_sin1(ph) * (float)amp); break;
    case 1: *y = -(int)((ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph) * (float)amp); break;
    case 2: *y = -(int)((2.0f * ph - 1.0f) * (float)amp); break;
    default: *y = ph < pw ? -amp : amp; break;
    }
}
static void view_waves(int s)
{
    const pw_module *m = &pws.mod[s];
    const pw_type *t = &pw_types[m->type];
    uint16_t c = cat_color[t->cat];
    int lfo = m->type == PW_T_LFO;
    float pw = lfo ? 0.5f : m->p[2], ph = lfo ? pw_live((unsigned)s, 0) : -1.0f;
    for (int k = 0; k < 4; ++k) {
        int x = 6 + (k % 2) * 117, y = VY + 3 + (k / 2) * 63, w = 110, mid = y + 34;
        float vy[51];
        rfill(x, y, w, 60, C_PANEL);
        text(x + 4, y + 3, t->out[k], F_XS, C_DIM);
        int prev = 0, a = 15;
        if (!lfo) pw_vco_wave(m->p, k, vy, 51);   /* (ANALOG and CHAR change the picture) */
        for (int i = 0; i <= 50; ++i) {
            int yy;
            if (lfo) wave_shape(k, pw, (float)i / 50.0f, &yy, a);
            else yy = -(int)(pw_clamp(vy[i], -1.8f, 1.8f) * (float)a);
            int xx = x + 6 + i * 2;
            if (i) line(xx - 2, mid + prev, xx, mid + yy, 1, c);
            prev = yy;
        }
        if (ph >= 0) { int yy; wave_shape(k, pw, ph, &yy, a); fill(x + 6 + (int)(ph * 100.0f) - 2, mid + yy - 2, 5, 5, C_WHITE); }
    }
}
#include "pw_vis.inc"
/* does the module page draw a visualization of it (as opposed to just its jacks)? */
int pw_ui_has_vis(unsigned type)
{
    switch (type) {
    case PW_T_ENV: case PW_T_VCF: case PW_T_EQ: case PW_T_COMB: case PW_T_FORMANT: case PW_T_FM4: case PW_T_FMDX: case PW_T_DRUMRACK: case PW_T_SAMPLER:
    case PW_T_TRIGSEQ: case PW_T_EUCLID: case PW_T_SEQ: case PW_T_PROLL: case PW_T_CVREC: case PW_T_SCOPE: case PW_T_GRAIN: case PW_T_ROOM: case PW_T_GENDY: case PW_T_VOCODER:
    case PW_T_DRUMS: case PW_T_STEREO: case PW_T_MIX8: case PW_T_PARAMMOD: case PW_T_VCO: case PW_T_LFO: return 1;
    default: return vis_kind(type) != VIS_NONE || extra_ok(type);
    }
}
/* a module's visualization, as its page shows it (the add list's second page borrows it) */
static void draw_picture(int s)
{
    unsigned type = pws.mod[s].type;
    int vk = vis_kind(type);
    switch (type) {
        case PW_T_ENV: view_env(s); break;
        case PW_T_VCF: view_vcf(s); break;
        case PW_T_EQ: view_eq(s); break;
        case PW_T_COMB: view_comb(s); break;
        case PW_T_FORMANT: view_formant(s); break;
        case PW_T_FM4: view_fm(s); break;
        case PW_T_FMDX: view_dx(s); break;
        case PW_T_DRUMRACK: view_rack(s); break;
        case PW_T_SAMPLER: view_sampler(s); break;
        case PW_T_TRIGSEQ: view_trig(s); break;
        case PW_T_EUCLID: view_euclid(s); break;
        case PW_T_SEQ: view_seq(s); break;
        case PW_T_PROLL: view_roll(s); break;
        case PW_T_CVREC: view_cvrec(s); break;
        case PW_T_SCOPE: view_scope(s); break;
        case PW_T_GRAIN: view_grain(s); break;
        case PW_T_ROOM: view_room(s); break;
        case PW_T_GENDY: view_gendy(s); break;
        case PW_T_VOCODER: view_vocoder(s); break;
        case PW_T_DRUMS: view_drum(s); break;
        case PW_T_STEREO: case PW_T_MIX8: view_mixer(s); break;
        case PW_T_PARAMMOD: view_pmod(s); break;
        case PW_T_VCO: case PW_T_LFO: view_waves(s); break;
        default:
            if (vk) vis_draw(s, vk);
            else if (extra_ok(type)) { vis_panel(); view_extra(s, 6, VY + 6, W - 12, VH - 12); }
            break;
    }
}
static void draw_edit(void)
{
    int s = valid_sel(0);
    if (s < 0) { text_center(W / 2, 90, "NO MODULE", F_M, C_DIM); knob_row(-1, 0); return; }
    ui.sel = s;
    fix_page(s);
    edit_strip(s);
    unsigned type = pws.mod[s].type;
    int vk = vis_kind(type), wired_only = 0;
    pg_row = param_pages(s) > 1 ? 13 : 0;
    if (ui.edit_jacks == 2 && !pw_example_text(type)) ui.edit_jacks = 0;
    if (ui.edit_jacks == 2) {   /* DEMO: an example patch that uses it */
        const char *ex = pw_example_text(type);
        char uses[64];
        int y = wrap_text(6, VY + 4, ex, F_S, C_TEXT, W - 12, 4, 14);
        pw_example_uses(type, uses, sizeof uses);
        text_fit(6, y + 1, uses, F_XS, C_DIM, W - 12);
        text(6, KY - 24, "SEL replaces the whole patch with it.", F_XS, C_DIM);
        edit_hint("SEL|Load demo|EDIT|Back");
    } else if (ui.edit_jacks == 1) {   /* INFO: what it does, and what is wired */
        view_wired(s);
        edit_hint(face_next(s) == 2 ? "EDIT|Demo" : "EDIT|Back");
    } else switch (type) {
        case PW_T_ENV: view_env(s); break;
        case PW_T_VCF: view_vcf(s); break;
        case PW_T_EQ: view_eq(s); break;
        case PW_T_COMB: view_comb(s); break;
        case PW_T_FORMANT: view_formant(s); break;
        case PW_T_FM4: view_fm(s); break;
        case PW_T_FMDX: view_dx(s); break;
        case PW_T_DRUMRACK: view_rack(s); break;
        case PW_T_SAMPLER: view_sampler(s); break;
        case PW_T_TRIGSEQ: view_trig(s); break;
        case PW_T_EUCLID: view_euclid(s); break;
        case PW_T_SEQ: view_seq(s); break;
        case PW_T_PROLL: view_roll(s); break;
        case PW_T_CVREC: view_cvrec(s); break;
        case PW_T_SCOPE: view_scope(s); break;
        case PW_T_GRAIN: view_grain(s); break;
        case PW_T_ROOM: view_room(s); break;
        case PW_T_GENDY: view_gendy(s); break;
        case PW_T_VOCODER: view_vocoder(s); break;
        case PW_T_DRUMS: view_drum(s); break;
        case PW_T_STEREO: case PW_T_MIX8: view_mixer(s); break;
        case PW_T_PARAMMOD: view_pmod(s); break;
        case PW_T_VCO: case PW_T_LFO: view_waves(s); break;
        default:
            if (vk) { vis_draw(s, vk); break; }
            if (type == PW_T_CZ) { vis_panel(); view_extra(s, 6, VY + 4, W - 12, VH - 20); break; }   /* (one row of four knobs, ALGO turns the page; the picture shows both lines) */
            if (param_pages(s) > 1) { pg_row = 0; view_pages_full(s); return; }
            if (extra_ok(pws.mod[s].type)) { vis_panel(); view_extra(s, 6, VY + 6, W - 12, VH - 12); break; }   /* (a module with one page of knobs and a picture) */
            view_wired(s); wired_only = 1; break;
    }
    if (!ui.edit_jacks && type == PW_T_MUTATE && !strip_pre) strip_pre = "SEL|Mutate page|";
    if (!ui.edit_jacks && is_stepper((unsigned)type)) { strip_pre = "SEQ|Edit steps|"; edit_caption(VY + VH - 16); }   /* (a module whose notes are entered on the STEPS page says so) */
    if (!ui.edit_jacks && !wired_only) edit_hint("EDIT|Info");
    else if (!ui.edit_jacks && pw_example_text(type)) edit_hint("EDIT|Demo");
    if (pg_row) edit_pagerow(s);
    pg_row = 0;
    knob_row(s, ui.param_page[s]);
}
/* the add list's preview of a module that shows steps: a made-up pattern in it, a few knobs set so the picture says something */
static void pv_seed(pw_module *m)
{
    static const int8_t notes[16] = {-12, 0, 7, -5, 0, 12, 7, 19, -7, 12, 3, 0, -12, 14, 10, 24};
    switch (m->type) {
    case PW_T_SEQ:
        m->p[0] = 16;
        for (int i = 0; i < 16; ++i) m->data[i] = (uint8_t)notes[i];
        m->data[16] = 0xD5; m->data[17] = 0xB6;
        break;
    case PW_T_MODSEQ:
        for (unsigned k = 0; k < 16; ++k) {
            pw_mseq_set(m, 0, k, (int)(k * 16)); pw_mseq_set(m, 1, k, (int)((15 - k) * 16));
            pw_mseq_set(m, 2, k, (int)(120 + 110 * pw_sin1((float)k / 16.0f * 2.0f)));
            pw_mseq_set(m, 3, k, (int)((k * 7 + 3) % 16 * 16));
        }
        break;
    case PW_T_TRIGSEQ: {
        static const uint16_t pat[4] = {0x1111, 0x0404, 0x5555, 0x8142};   /* kick, snare, hats, odd bits */
        for (unsigned t = 0; t < 4; ++t) m->p[4 + t] = 16;
        for (unsigned t = 0; t < 4; ++t)
            for (unsigned i = 0; i < 16; ++i)
                if ((pat[t] >> i) & 1u) {
                    unsigned v = t == 1 && i == 10 ? 2u : t == 2 && (i & 3) == 0 ? 2u : t == 3 && i == 15 ? 3u : 1u;
                    m->data[t * 4 + (i >> 2)] |= (uint8_t)(v << ((i & 3) * 2));
                }
        break;
    }
    case PW_T_CHORDSTEPS: {
        static const uint8_t deg[16] = {1, 1, 4, 5, 6, 6, 4, 5, 1, 3, 4, 5, 6, 5, 4, 1};
        for (int i = 0; i < 16; ++i) m->data[i] = deg[i];
        m->p[0] = 16;
        break;
    }
    case PW_T_EQ: m->p[0] = 7.0f; m->p[1] = -5.0f; m->p[2] = 8.0f; break;     /* low and high up, the middle scooped */
    case PW_T_GRAIN: m->p[0] = 0.5f; m->p[4] = 0.45f; break;
    default: break;
    }
}
static void draw_picture(int s);
#define ADD_SB 60                /* the patch strip on the add list: where the new module would go, and the cables SEL would make */
static void add_box(int x, int y, int w, int h, const char *name, uint16_t col, int ghost)
{
    rfill(x, y, w, h, ghost ? dim_of(col) : C_PANEL);
    if (ghost) {   /* a dashed outline: not there yet */
        for (int i = 0; i < w; i += 6) { fill(x + i, y, 3, 1, col); fill(x + i, y + h - 1, 3, 1, col); }
        for (int j = 0; j < h; j += 6) { fill(x, y + j, 1, 3, col); fill(x + w - 1, y + j, 1, 3, col); }
    }
    fill(x, y, w, 3, col);
    { int f = text_w(name, F_S) <= w - 8 ? F_S : F_XS; text_fit(x + 4, y + (f == F_S ? 6 : 8), name, f, ghost ? col : C_WHITE, w - 8); }
}
static int glo_now(void) { return ui.glo_on || ((ui.buttons & BIT(B_GLO)) && (ui.glo_used || fm1_millis() - ui.glo_down_ms >= 250u)); }
static void draw_add_strip(unsigned type)
{
    int slot, is_out, left = -1, right = -1; unsigned port, in = 0, out = 0, lp = 0, rp = 0;
    wire_target(ui.page_from_patch, &slot, &is_out, &port);
    int kind = pw_wire_plan(type, slot, is_out, port, -1, &in, &out);
    is_out &= 1;
    const pw_type *t = &pw_types[type];
    if (kind == PW_WIRE_FEED) { left = slot; lp = port; }
    else if (kind == PW_WIRE_DRIVE) { right = slot; rp = port; }
    else if (kind == PW_WIRE_INSERT) {
        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
            const pw_cable *k = &pws.cable[c];
            if (!k->used || !(is_out ? (k->sm == slot && k->so == port) : (k->dm == slot && k->di == port))) continue;
            if (is_out) { left = slot; lp = port; right = k->dm; rp = k->di; } else { right = slot; rp = port; left = k->sm; lp = k->so; }
            break;
        }
    }
    const int bw = 64, gap = 22, by = HDR + 4, bh = ADD_SB - 8, ry = by + 24;
    const int rtg = glo_now() ? replace_target() : -1;   /* GLO held or latched: the new module is shown where the module at the cursor is, with that module's neighbours */
    int kind_ov = 0;
    if (rtg >= 0) {   /* a replace, not an insert or a feed: the new module in the old one's place, between what was before and after it */
        left = right = -1; lp = rp = 0; in = out = 0;
        for (unsigned c = 0; c < PW_MAX_CABLES && (left < 0 || right < 0); ++c) {
            const pw_cable *k = &pws.cable[c];
            if (!k->used) continue;
            if (left < 0 && k->dm == rtg && k->sm != rtg) { left = k->sm; lp = k->so; in = k->di; }
            if (right < 0 && k->sm == rtg && k->dm != rtg) { right = k->dm; rp = k->di; out = k->so; }
        }
        if (in >= t->n_in) in = 0;
        if (out >= t->n_out) out = 0;
        kind_ov = 1;
    }
    int nb = 1 + (left >= 0) + (right >= 0), x = (W - (nb * bw + (nb - 1) * gap)) / 2;
    char lb[12];
    fill(0, HDR, W, ADD_SB, C_BG);
    fill(0, HDR + ADD_SB - 1, W, 1, C_LINE);
    uint16_t gc = cat_color[t->cat];
    int gx = x;
    if (left >= 0) {
        const pw_type *lt = &pw_types[pws.mod[left].type];
        pw_module_label((unsigned)left, lb);
        add_box(x, by, bw, bh, lb, cat_color[lt->cat], 0);
        fill(x + bw - 2, ry, 4, 4, cat_color[lt->cat]); text_right(x + bw - 5, ry - 3, lt->out[lp], F_XS, C_DIM);
        gx = x + bw + gap;
        for (int cx = x + bw + 3; cx < gx - 3; cx += 5) fill(cx, ry + 1, 3, 2, C_GREEN);
    }
    add_box(gx, by, bw, bh, t->tag, gc, 1);
    if (left >= 0 && (kind != PW_WIRE_DRIVE || kind_ov) && t->n_in) { fill(gx - 2, ry, 4, 4, gc); text(gx + 5, ry - 3, t->in[in], F_XS, C_DIM); }
    if (right >= 0) {
        const pw_type *rt = &pw_types[pws.mod[right].type];
        int rx = gx + bw + gap;
        pw_module_label((unsigned)right, lb);
        add_box(rx, by, bw, bh, lb, cat_color[rt->cat], 0);
        fill(rx - 2, ry, 4, 4, cat_color[rt->cat]); text(rx + 5, ry - 3, rt->in[rp], F_XS, C_DIM);
        if (t->n_out) { fill(gx + bw - 2, ry, 4, 4, gc); text_right(gx + bw - 5, ry - 3, t->out[out], F_XS, C_DIM); }
        for (int cx = gx + bw + 3; cx < rx - 3; cx += 5) fill(cx, ry + 1, 3, 2, C_GREEN);
    }
    if (left < 0 && right < 0) text_center(gx + bw / 2, ry - 3, "ALONE", F_XS, C_DIM);
}
/* the add list's second strip row: what each knob jumps by (REC, which stars a favorite, is in the first row) */
static void add_knob_row(void)
{
    static const char row[] = "\nK1-4|List, Category, Favs, Recents";
    unsigned n = (unsigned)strlen(strip_buf);
    if (n + sizeof row > sizeof strip_buf) return;
    char *o = strip_buf + n;
    for (unsigned i = 0; row[i]; ++i) *o++ = row[i];
    *o = 0;
}
static void draw_add(void)
{
    ky_off = 11;   /* (a tall strip: the second row says what the knobs do) */
    const int so = pv_n ? ADD_SB : 0;   /* (the patch strip: on every page, when there is a patch) */
    int cursor = clampi(ui.add_cursor, 0, PW_T_SHOWN - 1);
    const int lh = 17, rows = (STRIP_Y - (HDR + 4 + so)) / lh;   /* (to the shortcut bar: 8 under the patch strip) */
    int top = clampi(cursor - rows / 2, 0, PW_T_SHOWN - rows);
    if (top < 0) top = 0;
    if (so) draw_add_strip(add_order[cursor]);
    for (int i = top, y = HDR + 4 + so; i < PW_T_SHOWN && i < top + rows; ++i, y += lh) {
        const pw_type *t = &pw_types[add_order[i]];
        int sel = i == cursor;
        if (sel) rfill(0, y - 3, 99, lh, C_PANEL2);
        fill(3, y, 3, 11, cat_color[t->cat]);
        int fv = is_fav(add_order[i]), rc = !fv && is_recent(add_order[i]);   /* (a star: a favorite; an R: one of the last six you added) */
        {   /* the name in the room it has; a long one drops to the small face before it is cut */
            int room = fv || rc ? 80 : 90;
            if (text_w(t->name, F_S) > room) text_fit(10, y + 1, t->name, F_XS, C_WHITE, room);
            else text(10, y, t->name, F_S, C_WHITE);
        }
        if (fv) text(93, y, "*", F_S, C_AMBER);
        else if (rc) {   /* a small R, a little taller than the star: six rows of four dots */
            static const uint8_t R6[6] = {14, 9, 9, 14, 10, 9};
            for (int ry = 0; ry < 6; ++ry) for (int rx = 0; rx < 4; ++rx) if (R6[ry] & (8 >> rx)) fill(93 + rx, y + 3 + ry, 1, 1, C_DIM);
        }
    }
    {   /* where the cursor is in the list */
        int tr = rows * lh - 4, bar = tr * rows / PW_T_SHOWN, at = PW_T_SHOWN > rows ? (tr - bar) * top / (PW_T_SHOWN - rows) : 0;
        fill(101, HDR + 4 + so, 2, tr, C_PANEL);
        fill(101, HDR + 4 + so + at, 2, bar < 3 ? 3 : bar, C_DIM);
    }
    const pw_type *t = &pw_types[add_order[cursor]];
    int x = 108;
    fill(x - 2, HDR + so, W - x + 2, H - HDR - so, C_PANEL);
    {   /* the category, short when the page boxes leave no room for its name */
        static const char *const cat_short[PW_C_COUNT] = {"SOURCE", "FILTER", "FX", "MOD", "UTIL", "SEQ", "I/O"};
        const char *cn = pw_cat_names[t->cat];
        if (text_w(cn, F_XS) > W - 4 - page_tabs_wk(3, "ALGO") - x - 10) cn = cat_short[t->cat];
        text(x + 4, HDR + 6 + so, cn, F_XS, cat_color[t->cat]);
    }
    text_fit(x + 4, HDR + 22 + so, t->name, F_M, C_WHITE, W - x - 8);
    {   /* the page number, as the knob pages show it; pages 2 and 3 name themselves where the category was */
        int tx = W - 4 - page_tabs_wk(3, "ALGO");
        page_tabs_k(tx, HDR + 5 + so, 3, (unsigned)ui.add_page, C_PANEL, "ALGO");
        if (ui.add_page) {
            fill(x + 2, HDR + 4 + so, tx - x - 6, 14, C_PANEL);
            text(x + 4, HDR + 6 + so, ui.add_page == 2 ? "VISUAL" : "DEMO", F_XS, C_AMBER);
        }
    }
    if (ui.add_page == 2) {   /* a visualization of it, small, in the panel: its own page drawn on a spare slot, then shrunk */
        int ps = -1;
        for (int i = PW_MAX_MODULES - 1; i >= 0 && ps < 0; --i) if (!pws.mod[i].used) ps = i;
        fill(x - 2, HDR + 42 + so, W - x + 2, 184 - HDR - 42 - so, C_PANEL);
        if (!pw_ui_has_vis(add_order[cursor])) wrap_text(x + 4, 80 + so, "No visualization for this one.", F_S, C_DIM, W - x - 10, 3, 14);
        else if (ps < 0) wrap_text(x + 4, 80 + so, "The patch is full.", F_S, C_DIM, W - x - 10, 2, 14);
        else {
            pw_module *pm = &pws.mod[ps];
            const pw_type *pt = t;
            memset(pm, 0, sizeof *pm);
            pm->type = (uint8_t)add_order[cursor]; pm->n_out = pt->n_out; pm->nv = 1;
            for (unsigned k = 0; k < pt->n_param; ++k) pm->p[k] = pt->param[k].def;
            if (pm->type == PW_T_CVREC) for (int k = 0; k < 48; ++k) pm->data[k] = (uint8_t)clampi(128 + (int)(70.0f * pw_sin1((float)k / 48.0f) + 40.0f * pw_sin1((float)k / 12.0f)), 0, 255);
            unsigned i0 = n_prims, tp = n_text;
            { static int pv_last = -1; if (pv_last != (int)pm->type) { pv_last = (int)pm->type; vh.slot = -1; vh.n = 0; } }   /* (a new module: its traces start over) */
            pv_seed(pm);
            pv_demo = 1; draw_picture(ps); pv_demo = 0;
            memset(pm, 0, sizeof *pm);   /* (never used = 1: the audio never saw it) */
            /* shrink what it drew (x: 0..240, y: HDR..KY) into the panel; its words are left out */
            const int PW = W - x - 3, PX = x + 1 + (W - x - 3 - PW) / 2, PY = so ? HDR + so + 41 : HDR + 44;   /* the visualization's box: 128 wide (112 under the strip) */
            unsigned o = i0;
            for (unsigned i = i0; i < n_prims; ++i) {
                prim q = prims[i];
                if (q.kind == PR_TEXT) continue;
                int x0 = q.x, y0 = q.y, x1 = q.kind == PR_LINE ? q.w : q.x + q.w, y1 = q.kind == PR_LINE ? q.h : q.y + q.h;
                if (y1 < HDR || y0 >= KY) continue;
                #define SX(v) (PX + (v) * PW / W)
                #define SY(v) (PY + ((v) - HDR) * PW / W)
                int nx0 = SX(x0), ny0 = SY(y0), nx1 = SX(x1), ny1 = SY(y1);
                if (q.kind == PR_LINE) { q.x = (int16_t)nx0; q.y = (int16_t)ny0; q.w = (int16_t)nx1; q.h = (int16_t)ny1; q.font = 1; }
                else { q.x = (int16_t)nx0; q.y = (int16_t)ny0; q.w = (int16_t)(nx1 - nx0 < 1 ? 1 : nx1 - nx0); q.h = (int16_t)(ny1 - ny0 < 1 ? 1 : ny1 - ny0); }
                prims[o++] = q;
            }
            n_prims = o; n_text = tp;
            fill(x - 2, PY + PW * (KY - HDR) / W + 1, W - x + 2, 8, C_PANEL);   /* (whatever of it ran below the box) */
        }
        goto plan_bar;
    }
    if (ui.add_page == 1) {   /* the preset: an example patch that uses it, and what it does */
        const char *ex = pw_example_text(add_order[cursor]);
        char uses[64];
        if (so) wrap_text(ADD_HELP_X, HDR + 42 + so, ex ? ex : "No example patch for this one yet.", F_XS, C_TEXT, ADD_HELP_W, ADD_EXS_LINES, ADD_LH);
        else wrap_text(ADD_HELP_X, HDR + 44, ex ? ex : "No example patch for this one yet.", F_S, C_TEXT, ADD_HELP_W, ADD_EX_LINES, 14);
        pw_example_uses(add_order[cursor], uses, sizeof uses);
        text_fit(x + 4, so ? HDR + 42 + so + ADD_EXS_LINES * ADD_LH + 1 : HDR + 44 + ADD_EX_LINES * 14 + 1, uses, F_XS, C_DIM, W - x - 8);
        strip_set(ex ? "SEL|Load Demo (replaces current)|ALGO|Page|GLO+OCT-|Undo" : "ALGO|Page");
        add_knob_row();
        return;
    }
    int y = so ? wrap_text(ADD_HELP_X, HDR + 42 + so, pw_type_help(add_order[cursor]), F_XS, C_TEXT, ADD_HELP_W, ADD_HELPS_LINES, ADD_LH) : wrap_text(ADD_HELP_X, HDR + 44, pw_type_help(add_order[cursor]), F_S, C_TEXT, ADD_HELP_W, ADD_HELP_LINES, 14);   /* (the whole of it: tests check each fits) */
    y += 4;
    if (!so && y < 140) {
        text(x + 4, y, "IN", F_XS, C_LINE);
        text(x + 66, y, "OUT", F_XS, C_LINE);
        y += 14;
        int n = t->n_in > t->n_out ? t->n_in : t->n_out;
        for (int i = 0; i < n && y < 172; ++i, y += 13) {
            if (i < t->n_in) text_fit(x + 4, y, t->in[i], F_XS, C_TEXT, 60);
            if (i < t->n_out) text_fit(x + 66, y, t->out[i], F_XS, C_TEXT, W - x - 70);
        }
    }
    /* the plan: where SEL puts it */
plan_bar:;
    char plan[64];
    const char *sel = wire_plan_text(add_order[cursor], ui.page_from_patch, plan);
    wire_hint(sel, 0);
    add_knob_row();
}
static const char *const roman[8] = {"-", "I", "II", "III", "IV", "V", "VI", "VII"};
static void seq_badge(void)
{
    if (ui.seq_rec) { rfill(150, 3, 84, 18, C_RED); text_center(192, 6, ui.seq_rec == 2 ? "LIVE REC" : "STEP REC", F_S, C_WHITE); }
}
static void draw_seq_steps(int s)   /* SEQUENCER */
{
    pw_module *m = &pws.mod[s];
    int steps = seq_len(s), play = pw_seq_step((unsigned)s);
    uint16_t gates = seq_gates_of(m);
    char label[16], buf[12];
    pw_module_label((unsigned)s, label);
    seq_badge();
    for (int i = 0; i < 16; ++i) {
        int x = 1 + (i % 8) * 30, y = HDR + 4 + (i / 8) * 78;
        int on = (gates >> i) & 1, active = i < steps;
        int pitch = (int8_t)m->data[i];
        uint16_t base = active ? C_PANEL : C_WELL;
        rfill(x, y, 28, 74, base);
        if (active) {
            int bh = 3 + clampi(36 + pitch, 0, 72) * 35 / 72;   /* the pitch: one bar up from the bottom (filled when the step plays, an outline when off) */
            uint16_t bc = blend(C_PANEL, cat_color[PW_C_SEQ], 4u + (unsigned)m->data[PW_SEQ_STEPVEL + i] * 11u / 255u);   /* the velocity: how solid the bar is */
            if (on) fill(x + 4, y + 70 - bh, 20, bh, bc); else frame_rect(x + 4, y + 70 - bh, 20, bh, C_LINE);
            uint8_t fb = m->data[PW_SEQ_STEPFEAT + i];
            int rat = (fb & 3) + 1, ch = (fb >> 4) + 1, lg = seq_legato_of(fb);
            if (lg) text_center(x + 14, y + 13, lg == 2 ? "GLD" : "TIE", F_XS, C_AMBER);   /* (in place of the ratchet's four dashes) */
            else for (int q = 0; q < 4; ++q) fill(x + 4 + q * 5, y + 16, 4, 3, q < rat ? C_AMBER : C_PANEL2);          /* ratchet */
            fill(x + 4, y + 22, 20, 2, C_PANEL2); fill(x + 4, y + 22, ch * 20 / 16, 2, cat_color[PW_C_FX]);        /* chance */
        }
        if (on && active && i > 0 && (i & 7) && seq_legato_of(m->data[PW_SEQ_STEPFEAT + i]) && ((gates >> (i - 1)) & 1)) {   /* a tie or glide: a line from the top of the bar before to the top of this one */
            int pb = 3 + clampi(36 + (int8_t)m->data[i - 1], 0, 72) * 35 / 72, nb = 3 + clampi(36 + pitch, 0, 72) * 35 / 72;
            line(x - 6, y + 70 - pb, x + 4, y + 70 - nb, 2, seq_legato_of(m->data[PW_SEQ_STEPFEAT + i]) == 2 ? C_AMBER : cat_color[PW_C_SEQ]);
        }
        note_text(60 + pitch, buf);
        text_center(x + 14, y + 5, buf, F_XS, active ? (on ? C_TEXT : C_DIM) : C_PANEL2);
        if (i == play && pws.playing) frame_rect(x - 1, y - 1, 30, 76, C_GREEN);
        if (i == ui.seq_cursor) { frame_rect(x, y, 28, 74, C_WHITE); frame_rect(x + 1, y + 1, 26, 72, C_WHITE); }
    }
    char v[16], *o;
    uint8_t fb = m->data[PW_SEQ_STEPFEAT + ui.seq_cursor];
    note_text(60 + (int8_t)m->data[ui.seq_cursor], v);
    knob_cell(0, "NOTE", v, -1, 0);
    o = s_cpy(v, "x"); s_int(o, (fb & 3) + 1);
    if (seq_legato_of(fb)) knob_cell(1, "LEGATO", seq_legato_of(fb) == 2 ? "GLIDE" : "TIE", 1.0f, C_AMBER);
    else knob_cell(1, "RATCHET", v, (float)(fb & 3) / 3.0f, C_AMBER);
    o = s_int(v, ((fb >> 4) + 1) * 100 / 16); s_cpy(o, "%");
    knob_cell(2, "CHANCE", v, (float)((fb >> 4) + 1) / 16.0f, cat_color[PW_C_FX]);
    if (m->data[PW_SEQ_STEPVEL + ui.seq_cursor]) { o = s_int(v, m->data[PW_SEQ_STEPVEL + ui.seq_cursor] * 100 / 255); s_cpy(o, "%"); } else s_cpy(v, "OFF");
    knob_cell(3, "VELOCITY", v, (float)m->data[PW_SEQ_STEPVEL + ui.seq_cursor] / 255.0f, cat_color[PW_C_SEQ]);
}
/* PIANO ROLL's editor: the keyboard down the left, steps across (16 in view), a note a bar, the cursor a frame the size of the snap */
static void roll_init(const pw_module *m)   /* the cursor when the editor is on a roll it was not on: its first note, or middle C */
{
    int id = (int)(m - pws.mod) + 1;
    if (ui.roll_have == id && ui.roll_ser == pws.clear_serial) return;
    ui.roll_ser = pws.clear_serial;
    ui.roll_have = (uint8_t)id; ui.roll_t = 0; ui.roll_vx = 0; ui.roll_p = 60; ui.roll_len = 1;
    unsigned st, ln, pt;
    if (pw_roll_get(m, 0, &st, &ln, &pt, 0, 0)) { ui.roll_t = (int)st; ui.roll_p = (int)pt; ui.roll_len = (int)ln; }
}
static int roll_snap(const pw_module *m) { static const uint8_t sn[4] = {1, 2, 4, 16}; int k = (int)(m->p[1] + 0.5f); return sn[k < 0 ? 0 : k > 3 ? 3 : k]; }
static void roll_range(const pw_module *m, int *lo, int *hi)   /* the pitches in view: the notes' lowest and highest with two more each side (the cursor, if it is beyond, too); twelve around the cursor for an empty roll */
{
    int l = 127, h = 0, n = (int)pw_roll_count(m);
    for (int k = 0; k < n; ++k) { unsigned p; pw_roll_get(m, (unsigned)k, 0, 0, &p, 0, 0); if ((int)p < l) l = (int)p; if ((int)p > h) h = (int)p; }
    if (n) { l -= 2; h += 2; } else { l = ui.roll_p - 5; h = ui.roll_p + 6; }
    if (ui.roll_p < l) l = ui.roll_p;
    if (ui.roll_p > h) h = ui.roll_p;
    while (h - l < 7) { if (l > 0) --l; if (h - l < 7 && h < 127) ++h; }
    *lo = l < 0 ? 0 : l; *hi = h > 127 ? 127 : h;
}
static int roll_folded(const pw_module *m)   /* the view shows only pitches with notes: FOLD, or AUTO when the keys in view would be more than 17 (rows under 7 pixels) */
{
    if (ui.roll_fold != 0) return ui.roll_fold == 1;
    int lo, hi; roll_range(m, &lo, &hi);
    return hi - lo + 1 > 17;
}
static void draw_roll(int s)
{
    enum { GX = 24, CW = 13, GH = 120 };   /* the grid's height; the pitch range is the octaves the notes (and the cursor) are in, at most three */
    const int GY = HDR + 9;
    pw_module *m = &pws.mod[s];
    int loop = (int)pw_roll_loop(m), snap = roll_snap(m);
    roll_init(m);
    ui.roll_t = clampi(ui.roll_t, 0, loop - 1); ui.roll_p = clampi(ui.roll_p, 0, 127);
    ui.roll_len = clampi(ui.roll_len, 1, 64);
    if (ui.roll_t < ui.roll_vx) ui.roll_vx = ui.roll_t;
    if (ui.roll_t + snap > ui.roll_vx + 16) ui.roll_vx = ui.roll_t + snap - 16;
    if (ui.roll_vx > loop - 16) ui.roll_vx = loop - 16;
    if (ui.roll_vx < 0) ui.roll_vx = 0;
    /* the rows (high pitch first): every key from two below the lowest note to two above the highest (the cursor's too), or with FOLD only the
     * pitches that have a note, and the cursor's; more than 20 rows (24 for all KEYS) scroll with the cursor */
    uint8_t rowp[24]; int NROW = 0;
    const int fold = roll_folded(m);
    if (fold) {
        uint8_t used[128] = {0}, asc[128]; int n = 0, at = 0;
        for (unsigned k = 0; k < pw_roll_count(m); ++k) { unsigned p; pw_roll_get(m, k, 0, 0, &p, 0, 0); used[p & 127u] = 1; }
        used[ui.roll_p] = 1;
        for (int q = 0; q < 128; ++q) if (used[q]) { if (q == ui.roll_p) at = n; asc[n++] = (uint8_t)q; }
        int first = n > 20 ? clampi(at - 10, 0, n - 20) : 0, cnt = n > 20 ? 20 : n;
        for (int r = 0; r < cnt; ++r) rowp[r] = asc[first + cnt - 1 - r];
        NROW = cnt;
    } else {
        int lo, hi; roll_range(m, &lo, &hi);
        if (hi - lo + 1 > 24) {
            if (ui.roll_p < ui.roll_wo) ui.roll_wo = ui.roll_p;
            if (ui.roll_p > ui.roll_wo + 23) ui.roll_wo = ui.roll_p - 23;
            ui.roll_wo = clampi(ui.roll_wo, lo, hi - 23); lo = ui.roll_wo; hi = lo + 23;
        }
        NROW = hi - lo + 1;
        for (int r = 0; r < NROW; ++r) rowp[r] = (uint8_t)(hi - r);
    }
    const int RH = GH / NROW > 14 ? 14 : GH / NROW;
    ui.roll_vy = rowp[NROW - 1];
    int vx = ui.roll_vx;
    char buf[16];
    pw_module_label((unsigned)s, buf);
    seq_badge();
    /* rows: the keyboard and the lanes */
    for (int r = 0; r < NROW; ++r) {
        int pitch = rowp[r], pc = pitch % 12, y = GY + r * RH, black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        rfill(0, y, GX - 2, RH - 1, black ? C_PANEL2 : pc == 0 ? C_AMBER : C_TEXT);   /* (the C is amber, with its octave's number) */
        if (black) fill(0, y, GX - 8, RH - 1, C_BG);
        fill(GX, y, 16 * CW, RH - 1, black ? C_WELL : C_PANEL);
        if (pc == 0) fill(GX, y + RH - 1, 16 * CW, 1, C_LINE);
    }
    for (int r = 0; r < NROW; ++r) {   /* the key names (every row folded, if they fit; else the C's), over the keys */
        int pitch = rowp[r], pc = pitch % 12, black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        if ((pc == 0 || (fold && RH >= 10)) && RH >= 8) { note_text(pitch, buf); text(black ? 1 : 3, GY + r * RH + (RH >= 10 ? 1 : 0), buf, F_XS, black ? C_TEXT : C_BG); }
    }
    for (int c = 0; c < 16; ++c) {   /* the beats; past the loop's end dim */
        int st = vx + c, x = GX + c * CW;
        if (st >= loop) { fill(x, GY, CW, NROW * RH, C_BG); continue; }
        if (st % 4 == 0) fill(x, GY, 1, NROW * RH, st % 16 == 0 ? C_AMBER : C_LINE);
        fill(x, GY - (st % 16 == 0 ? 7 : st % 4 == 0 ? 4 : 2), 1, st % 16 == 0 ? 6 : st % 4 == 0 ? 3 : 1, st % 16 == 0 ? C_AMBER : C_DIM);
    }
    static const uint8_t vcol[4] = {PW_C_SEQ, PW_C_FX, PW_C_FILTER, PW_C_MOD};
    for (unsigned k = 0; k < pw_roll_count(m); ++k) {
        unsigned st, ln, pt, vl, vo;
        pw_roll_get(m, k, &st, &ln, &pt, &vl, &vo);
        int r = -1;
        for (int q = 0; q < NROW; ++q) if (rowp[q] == pt) r = q;
        if (r < 0 || (int)st >= vx + 16 || (int)(st + ln) <= vx) continue;
        int x0 = (int)st < vx ? 0 : ((int)st - vx) * CW, x1 = (int)(st + ln) > vx + 16 ? 16 * CW : ((int)(st + ln) - vx) * CW;
        rfill(GX + x0, GY + r * RH, x1 - x0 - 1, RH - 1, cat_color[vcol[vo]]);
        if (RH >= 6) fill(GX + x0 + 1, GY + r * RH + RH - 3, (x1 - x0 - 3) * (int)(vl + 1) / 32, 1, C_BG);   /* its velocity, a line along the bottom */
    }
    if (pws.playing) {
        int ph = (int)((pws.sixteenths + (unsigned)loop - 1u) % (unsigned)loop) - vx;
        if (ph >= 0 && ph < 16) fill(GX + ph * CW, GY, 2, NROW * RH, C_GREEN);
    }
    {
        int cr = 0, cw = snap * CW;
        for (int q = 0; q < NROW; ++q) if (rowp[q] == ui.roll_p) cr = q;
        if (ui.roll_t - vx + snap > 16) cw = (16 - (ui.roll_t - vx)) * CW;
        frame_rect(GX + (ui.roll_t - vx) * CW, GY + cr * RH - 1, cw, RH + 1, C_WHITE);
    }
    /* the note under the cursor, and the knobs */
    int idx = pw_roll_find(m, (unsigned)ui.roll_t, (unsigned)ui.roll_p);
    unsigned st = 0, ln = (unsigned)ui.roll_len, pt = (unsigned)ui.roll_p, vl = 24, vo = 0;
    if (idx >= 0) pw_roll_get(m, (unsigned)idx, &st, &ln, &pt, &vl, &vo);
    char v[16], *o;
    note_text(ui.roll_p, v);
    o = s_cpy(buf, v); o = s_cpy(o, idx >= 0 ? "  NOTE  " : "  EMPTY  "); o = s_cpy(o, "STEP "); o = s_int(o, (idx >= 0 ? (int)st : ui.roll_t) + 1); o = s_cpy(o, "  LOOP "); o = s_int(o, loop / 16); o = s_cpy(o, loop == 16 ? " BAR" : " BARS"); if (fold) o = s_cpy(o, "  FOLD");
    text(GX, GY + GH + 1, buf, F_S, C_DIM);
    o = s_int(v, (int)ln); s_cpy(o, ln == 1 ? " STEP" : " STEPS");
    knob_cell(0, "LENGTH", v, (float)ln / 16.0f > 1.0f ? 1.0f : (float)ln / 16.0f, C_AMBER);
    if (idx >= 0) { note_text((int)pt, v); knob_cell(1, "PITCH", v, (float)pt / 127.0f, cat_color[PW_C_SOURCE]); } else knob_cell(1, "PITCH", "-", -1, 0);
    if (idx >= 0) { o = s_int(v, (int)st + 1); knob_cell(2, "MOVE", v, (float)st / (float)(loop - 1), cat_color[PW_C_FX]); } else knob_cell(2, "MOVE", "-", -1, 0);
    o = s_int(v, (int)(vl + 1) * 100 / 32); s_cpy(o, "%");
    knob_cell(3, "VELOCITY", idx >= 0 ? v : "-", idx >= 0 ? (float)(vl + 1) / 32.0f : -1, cat_color[PW_C_FILTER]);
}
static void draw_seq_mod(int s)   /* MOD SEQ: four lanes of bars */
{
    pw_module *m = &pws.mod[s];
    static const char *const ln[4] = {"A", "B", "C", "D"}, *const kn[4] = {"LANE A", "LANE B", "LANE C", "LANE D"};
    uint16_t lc[4] = {C_AMBER, cat_color[PW_C_FILTER], cat_color[PW_C_FX], cat_color[PW_C_MOD]};
    for (int l = 0; l < 4; ++l) {
        int y = HDR + 3 + l * 39, len = clampi((int)(m->p[l] + 0.5f), 1, 16), at = (int)pw_live((unsigned)s, (unsigned)l);
        text(2, y + 12, ln[l], F_S, lc[l]);
        for (int i = 0; i < 16; ++i) {
            int x = 14 + i * 14, v = (int)pw_mseq_get(m, (unsigned)l, (unsigned)i), h = 3 + v * 30 / 255;
            if (i >= len) { fill(x, y + 34 - 4, 12, 4, C_WELL); continue; }
            fill(x, y, 12, 34, C_PANEL);
            fill(x, y + 34 - h, 12, h, i == at && pws.playing ? C_GREEN : lc[l]);
            if (i == ui.seq_cursor) frame_rect(x - 1, y - 1, 14, 36, C_WHITE);
        }
    }
    char v[16], *o;
    for (unsigned l = 0; l < 4; ++l) {
        unsigned q = pw_mseq_get(m, l, (unsigned)ui.seq_cursor);
        o = s_int(v, (int)(q * 100u / 255u)); s_cpy(o, "%");
        knob_cell((int)l, kn[l], v, (float)q / 255.0f, lc[l]);
    }
}
static void draw_seq_chord(int s)   /* CHORD STEPS */
{
    pw_module *m = &pws.mod[s];
    int steps = seq_len(s), play = (int)pw_live((unsigned)s, 0);
    char nm[10];
    seq_badge();
    for (int i = 0; i < 16; ++i) {
        int x = 1 + (i % 8) * 30, y = HDR + 4 + (i / 8) * 78, active = i < steps;
        uint8_t b = m->data[i];
        int d = b & 15, ty = (b >> 4) & 3, inv = b >> 6;
        rfill(x, y, 28, 74, active ? C_PANEL : C_WELL);
        if (active) {
            uint16_t c = d >= 1 && d <= 7 ? cat_color[PW_C_SEQ] : C_LINE;
            text_center(x + 14, y + 8, d == 8 ? "~" : roman[d <= 7 ? d : 0], F_M, c);
            if (d >= 1 && d <= 7) {
                static const char *const tn[4] = {"", "7", "SUS", "ADD9"};
                if (ty) text_center(x + 14, y + 28, tn[ty], F_XS, C_AMBER);
                for (int q = 0; q < inv; ++q) fill(x + 6 + q * 6, y + 40, 4, 4, C_AMBER);
                pw_chord_name(m, b, nm);
                text_center(x + 14, y + 56, nm, F_S, C_DIM);
            }
        }
        if (i == play && pws.playing) frame_rect(x - 1, y - 1, 30, 76, C_GREEN);
        if (i == ui.seq_cursor) { frame_rect(x, y, 28, 74, C_WHITE); frame_rect(x + 1, y + 1, 26, 72, C_WHITE); }
    }
    uint8_t b = m->data[ui.seq_cursor];
    static const char *const tl[4] = {"TRIAD", "7TH", "SUS4", "ADD9"};
    pw_chord_name(m, b, nm);
    knob_cell(0, "DEGREE", (b & 15) == 8 ? "TIE" : (b & 15) == 0 ? "REST" : roman[(b & 15) <= 7 ? (b & 15) : 0], (float)(b & 15) / 8.0f, cat_color[PW_C_SEQ]);
    knob_cell(1, "TYPE", tl[(b >> 4) & 3], (float)((b >> 4) & 3) / 3.0f, C_AMBER);
    char v[8]; s_int(v, b >> 6);
    knob_cell(2, "INVERT", v, (float)(b >> 6) / 3.0f, C_AMBER);
    knob_cell(3, "CHORD", nm, -1, 0);
}
static void draw_seq_grav(int s)   /* GRAVITY: the notes, and the one the ball last played */
{
    pw_module *m = &pws.mod[s];
    int nnotes = seq_len(s), hit = (int)pw_live((unsigned)s, 0), falling = pw_live((unsigned)s, 1) > 0.5f;
    char buf[12];
    seq_badge();
    for (int i = 0; i < 16; ++i) {
        int x = 1 + (i % 8) * 30, y = HDR + 4 + (i / 8) * 78, active = i < nnotes, pitch = (int8_t)m->data[i];
        rfill(x, y, 28, 74, active ? C_PANEL : C_WELL);
        if (!active) continue;
        int bar = clampi(36 + pitch, 0, 72) * 56 / 72;
        fill(x + 4, y + 62 - bar, 20, 4, i == hit ? C_WHITE : cat_color[PW_C_SEQ]);
        note_text(60 + pitch, buf);
        text_center(x + 14, y + 5, buf, F_XS, i == hit ? C_WHITE : C_DIM);
        if (i == ui.seq_cursor) { frame_rect(x, y, 28, 74, C_WHITE); frame_rect(x + 1, y + 1, 26, 72, C_WHITE); }
    }
    char v[16], *o;
    note_text(60 + (int8_t)m->data[ui.seq_cursor], v);
    knob_cell(0, "NOTE", v, -1, 0);
    o = s_int(v, (int)(m->p[2] * 100.0f + 0.5f)); s_cpy(o, "%");
    knob_cell(1, "ELASTIC", v, (m->p[2] - 0.4f) / 0.55f, C_AMBER);
    pw_format_param((unsigned)s, 1, v);
    knob_cell(2, "FALL", v, (m->p[1] - 0.5f) / 7.5f, C_AMBER);
    knob_cell(3, falling ? "FALLING" : "SEL: DROP", "", -1, 0);
}
static void draw_seq(void)
{
    int s = find_seq();
    if (s < 0) {
        text_center(W / 2, 70, "NO SEQUENCER", F_M, C_TEXT);
        strip_set("FX|Add one");
        return;
    }
    ui.seq_cursor = clampi(ui.seq_cursor, 0, 15);
    if (pws.mod[s].type == PW_T_SEQ || pws.mod[s].type == PW_T_MODSEQ || pws.mod[s].type == PW_T_PROLL) ky_off = 11;   /* (a tall strip: all the shortcuts, randomize and clear among them) */
    switch (pws.mod[s].type) {
    case PW_T_MODSEQ: draw_seq_mod(s); break;
    case PW_T_CHORDSTEPS: draw_seq_chord(s); break;
    case PW_T_GRAVITY: draw_seq_grav(s); break;
    case PW_T_PROLL: draw_roll(s); break;
    default: draw_seq_steps(s); break;
    }
    if (pws.mod[s].type == PW_T_SEQ) strip_set("SEL|On/off|REC|Record|EDIT|Knobs\nK2|Ratch/Tie/Glide|GLO+SEL|Randomize|GLO+REC|Clear");
    else if (pws.mod[s].type == PW_T_MODSEQ) strip_set("SEL|Random column|EDIT|Knobs\nGLO+SEL|Randomize all|GLO+REC|Clear");
    else if (pws.mod[s].type == PW_T_PROLL) strip_set("SELECT|Time|ALGO|Pitch|SEL|Note|REC|Record\nGLO+SEL|Copy|GLO+REC|Clear|GLO+SEQ|View|GLO+K4|Loop");
    else strip_set("EDIT|Module knobs");   /* (CHORD STEPS, GRAVITY) */
}
/* --- Groups: whole wired groups of modules, added at once at a free spot (one undo step) */
typedef struct { const char *name, *what, *mods, *cables; } pw_sub;
static const pw_sub subs[] = {
    {"SUBTRACTIVE VOICE", "Oscillator, envelope, filter and amplifier, wired. Patch V/OCT and GATE in, take VCA out.",
     "VCO ENV VCF VCA", "VCO.SAW>VCF.IN VCF.LP>VCA.IN ENV.ENV>VCA.CV ENV.ENV>VCF.FREQ"},
    {"KEYS VOICE", "The same voice, played from the keyboard: KEYS drive the pitch and the gate.",
     "KEYS VCO ENV VCF VCA", "KEYS.PITCH>VCO.V/OCT KEYS.GATE>ENV.GATE VCO.SAW>VCF.IN VCF.LP>VCA.IN ENV.ENV>VCA.CV ENV.ENV>VCF.FREQ"},
    {"BASS LINE", "A clock, a sequencer and a voice: a bass line that plays itself. Take VCA out.",
     "CLOCK SEQ VCO ENV VCA", "CLOCK.CLK>SEQ.CLOCK SEQ.PITCH>VCO.V/OCT SEQ.GATE>ENV.GATE VCO.SAW>VCA.IN ENV.ENV>VCA.CV"},
    {"DRUM VOICE", "A drum with its own trigger track, ticking on a clock.",
     "CLOCK TRIG DRUM", "CLOCK.CLK>TRIG.CLOCK TRIG.A>DRUM.TRIG"},
    {"ECHO + REVERB", "A delay feeding a reverb: send a sound in, take the reverb out.",
     "DELAY VERB", "DELAY.MIX>VERB.IN_L DELAY.MIX>VERB.IN_R"},
};
#define PW_SUBS ((int)(sizeof subs / sizeof subs[0]))
int pw_ui_sub_count(void) { return PW_SUBS; }
const char *pw_ui_sub_name(unsigned i) { return i < (unsigned)PW_SUBS ? subs[i].name : ""; }
const char *pw_ui_sub_what(unsigned i) { return i < (unsigned)PW_SUBS ? subs[i].what : ""; }
static int sub_tag(const char **p, int *inst)   /* "VCO" or "VCO#2": the type index */
{
    const char *b = *p, *r = b;
    while (*r && *r != ' ' && *r != '#' && *r != '.') ++r;
    int t = -1;
    for (unsigned k = 0; k < PW_T_COUNT; ++k) {
        const char *tag = pw_types[k].tag; unsigned n = 0;
        while (tag[n] && b + n < r && tag[n] == b[n]) ++n;
        if (!tag[n] && b + n == r) t = (int)k;
    }
    *inst = 1;
    if (*r == '#') { ++r; *inst = 0; while (*r >= '0' && *r <= '9') *inst = *inst * 10 + (*r++ - '0'); }
    *p = r;
    return t;
}
static int sub_name_is(const char *a, const char *e, const char *port)   /* "IN_L" against "IN L" */
{
    for (; a < e && *port; ++a, ++port) { char x = *port == ' ' ? '_' : *port; if (*a != x) return 0; }
    return a == e && !*port;
}
static int sub_add(int idx)
{
    const pw_sub *sp = &subs[idx];
    int type[8], inst[8], slot[8], n = 0;
    for (const char *q = sp->mods; *q && n < 8; ) {
        if (*q == ' ') { ++q; continue; }
        type[n] = sub_tag(&q, &inst[n]); slot[n] = -1; ++n;
    }
    /* a row of free cells, side by side (else wherever the next free cell is) */
    int x0 = -1, y0 = 0;
    for (int row = 0; row < 12 && x0 < 0; ++row)
        for (int col = 0; col + n <= 8 && x0 < 0; ++col) {
            int clash = 0;
            for (int k = 0; k < n && !clash; ++k) {
                int cx = 24 + (col + k) * 204, cy = 24 + row * 268;
                for (unsigned i = 0; i < PW_MAX_MODULES && !clash; ++i)
                    if (pws.mod[i].used == 1 && pws.mod[i].x > cx - 150 && pws.mod[i].x < cx + 150 && pws.mod[i].y > cy - 200 && pws.mod[i].y < cy + 200) clash = 1;
            }
            if (!clash) { x0 = 24 + col * 204; y0 = 24 + row * 268; }
        }
    pw_undo_group(1);
    int ok = 1;
    for (int k = 0; k < n; ++k) {
        int x, y;
        if (x0 >= 0) { x = x0 + k * 204; y = y0; } else free_spot(&x, &y);
        slot[k] = type[k] >= 0 ? pw_add((unsigned)type[k], x, y) : -1;
        if (slot[k] < 0) ok = 0;
    }
    for (const char *q = sp->cables; *q && ok; ) {
        if (*q == ' ') { ++q; continue; }
        int ti, tj, a = -1, b = -1, so = -1, di = -1;
        const char *m1 = q; while (*q && *q != '.') ++q;
        const char *p1 = ++q; while (*q && *q != '>') ++q;
        const char *e1 = q; ++q;
        const char *m2 = q; while (*q && *q != '.') ++q;
        const char *p2 = ++q; while (*q && *q != ' ') ++q;
        const char *e2 = q, *mm;
        mm = m1; int t1 = sub_tag(&mm, &ti);
        mm = m2; int t2 = sub_tag(&mm, &tj);
        for (int k = 0; k < n; ++k) { if (type[k] == t1 && inst[k] == ti) a = k; if (type[k] == t2 && inst[k] == tj) b = k; }
        if (a < 0 || b < 0) { ok = 0; break; }
        for (unsigned k = 0; k < pw_types[type[a]].n_out; ++k) if (sub_name_is(p1, e1, pw_types[type[a]].out[k])) so = (int)k;
        for (unsigned k = 0; k < pw_types[type[b]].n_in; ++k) if (sub_name_is(p2, e2, pw_types[type[b]].in[k])) di = (int)k;
        if (so < 0 || di < 0 || pw_connect((unsigned)slot[a], (unsigned)so, (unsigned)slot[b], (unsigned)di) < 0) ok = 0;
    }
    if (ok && n > 1) { grp_set mk = 0; for (int k = 0; k < n; ++k) mk |= (grp_set)1 << slot[k]; if (!grp_make(mk)) ok = 0; }
    pw_undo_group(0);
    if (!ok) return -1;
    return slot[0];
}
void pw_ui_ungroup(int slot) { if (slot >= 0 && slot < PW_MAX_MODULES && pws.mod[slot].used == 1 && pws.mod[slot].group) grp_ungroup(slot); }
int pw_ui_sub_insert(unsigned i) { return i < (unsigned)PW_SUBS ? sub_add((int)i) : -1; }
#define LIST_USER (LIST_TOP + PW_FACTORY_COUNT + PW_SUBS)   /* then Groups, then the user slots */
#define LIST_TOP 2               /* the list: 0 a new empty patch, 1 a random one, then factory, groups, user (HELP and SETTINGS are the next tabs, ALGORITHM) */
static void list_step(int d, int jump)   /* the patch list moves round the ends; ALGORITHM jumps between its four parts: new and system / factory / groups / yours */
{
    int total = LIST_USER + PW_USER_SLOTS;
    if (!jump) { ui.list_cursor = ((ui.list_cursor + d) % total + total) % total; return; }
    static const int start[4] = {0, LIST_TOP, LIST_TOP + PW_FACTORY_COUNT, LIST_USER};
    int c = ui.list_cursor, sec = c >= start[3] ? 3 : c >= start[2] ? 2 : c >= start[1] ? 1 : 0;
    if (d > 0) sec = (sec + 1) % 4;
    else if (c == start[sec]) sec = (sec + 3) % 4;
    ui.list_cursor = start[sec];
}
static uint16_t patch_kind_color(int i, const char *name, int filled)
{
    if (i == 0) return C_GREEN;
    if (i == 1) return cat_color[PW_C_MOD];
    if (i < LIST_TOP) return C_DIM;   /* HELP, SETTINGS, DIAGNOSTICS */
    if (i >= LIST_USER) return filled ? (pw_store_kind((unsigned)(i - LIST_USER)) ? cat_color[PW_C_FILTER] : C_GREEN) : C_LINE;
    if (i >= LIST_TOP + PW_FACTORY_COUNT) return cat_color[PW_C_FILTER];
    if (pw_has(name, "SONG")) return C_AMBER;
    if (pw_has(name, "DRUM") || pw_has(name, "BEAT") || pw_has(name, "KIT") || pw_has(name, "RACK") || pw_has(name, "BREAK") || pw_has(name, "BONGO") || pw_has(name, "SLICE") || pw_has(name, "LOOP") || pw_has(name, "SEQ")) return cat_color[PW_C_SEQ];
    if (pw_has(name, "PAD") || pw_has(name, "CHOIR") || pw_has(name, "STRINGS") || pw_has(name, "BELL") || pw_has(name, "DRONE") || pw_has(name, "RAIN") || pw_has(name, "FROZEN") || pw_has(name, "GRAIN") || pw_has(name, "TAPE")) return cat_color[PW_C_FX];
    return cat_color[PW_C_SOURCE];
}
/* the footer the patch list and the SAVE page share: tempo, the name editor, and the voices (the list) or what is stored (SAVE) */
static void list_footer(int save)
{
    /* (GLO+K1 sets the tempo on the patch screen too) */
    char v[12]; { int q = (int)(pws.bpm * 10.0f + 0.5f); char *o = q % 10 ? s_tenths(v, q) : s_int(v, q / 10); (void)o; }
    knob_cell(0, "TEMPO", v, (pws.bpm - 30.0f) / 270.0f, ui.tempo_ms && fm1_millis() - ui.tempo_ms < 1500u ? C_AMBER : cat_color[PW_C_SEQ]);
    rfill(62, KY, 116, KH, C_PANEL);
    text(66, KY + 2, "NAME", F_S, C_DIM);
    text_right(176, KY + 3, "K2  K3", F_XS, C_LINE);
    for (int i = 0; i < 12; ++i) {
        char ch[2] = {pws.name[i] ? pws.name[i] : ' ', 0};
        int x = 66 + i * 9;
        if (i == ui.name_pos) rfill(x - 1, KY + 14, 10, 17, C_PANEL2);
        text_center(x + 4, KY + 16, ch, F_S, i == ui.name_pos ? C_WHITE : C_TEXT);
        fill(x, KY + 32, 7, 1, C_LINE);
    }
    if (save) knob_cell(3, "KIND", ui.save_group ? "GROUP" : "PATCH", ui.save_group ? 1.0f : 0.0f, ui.save_group ? cat_color[PW_C_FILTER] : C_GREEN);
    else { s_int(v, pws.voices); knob_cell(3, "VOICES", v, (float)(pws.voices - 1) / (float)(PW_MAX_VOICES - 1), C_AMBER); }
}
static void draw_patches(void)
{
    int total = LIST_USER + PW_USER_SLOTS;
    ui.list_cursor = clampi(ui.list_cursor, 0, total - 1);
    const int lh = 17, rows = 9;
    int top = clampi(ui.list_cursor - rows / 2, 0, total - rows);
    char buf[32];
    for (int i = top, y = HDR + 5; i < total && i < top + rows; ++i, y += lh) {
        int sel = i == ui.list_cursor;
        const char *tag, *name;
        int filled = 1;
        const char *kind = "";
        if (i == 0) { tag = "+"; name = "NEW EMPTY PATCH"; }
        else if (i == 1) { tag = "?"; name = "GENERATE PATCH"; }
        else if (i < LIST_TOP + PW_FACTORY_COUNT) { s_int(buf, i); tag = "F"; name = pw_factory_name((unsigned)(i - LIST_TOP)); }
        else if (i < LIST_USER) { tag = "G"; name = subs[i - LIST_TOP - PW_FACTORY_COUNT].name; }
        else { int u = i - LIST_USER; name = pw_store_name((unsigned)u); tag = "U"; if (!name) { name = "(empty)"; filled = 0; } }
        uint16_t kc = patch_kind_color(i, name, filled);
        if (i < LIST_TOP + PW_FACTORY_COUNT && pw_has(name, "SONG")) kind = "SONG";
        else if (i >= LIST_USER) kind = filled ? (pw_store_kind((unsigned)(i - LIST_USER)) ? "GROUP" : "USER") : "FREE";
        else if (i >= LIST_TOP + PW_FACTORY_COUNT) kind = "GROUP";
        if (sel) rfill(0, y - 3, W - 8, lh, C_PANEL2);
        fill(3, y, 3, 11, kc);
        uint16_t c = filled ? C_WHITE : C_LINE;
        char t[8]; char *o = s_cpy(t, tag);
        if (i >= LIST_TOP) s_int(o, i < LIST_TOP + PW_FACTORY_COUNT ? i - LIST_TOP + 1 : i < LIST_USER ? i - LIST_TOP - PW_FACTORY_COUNT + 1 : i - LIST_USER + 1);
        text_right(W - 14, y, t, F_S, filled ? C_DIM : C_LINE);
        text_fit(10, y, name, F_S, c, 110);
        if (i >= LIST_USER && i - LIST_USER == ui.user_slot) text_right(W - 40, y + 1, "CURRENT", F_XS, C_AMBER);
        else if (sel && kind[0]) text_right(W - 40, y + 1, kind, F_XS, kc);
    }
    {   /* where the cursor is in the list */
        int tr = rows * lh - 4, bar = tr * rows / total, at = (tr - bar) * top / (total - rows);
        fill(W - 7, HDR + 4, 3, tr, C_PANEL);
        fill(W - 7, HDR + 4 + at, 3, bar < 3 ? 3 : bar, C_DIM);
    }
    {   /* what the keys do, on the strip */
        int cur = ui.list_cursor, user = cur >= LIST_USER, sub = (!user && cur >= LIST_TOP + PW_FACTORY_COUNT) || (user && pw_store_kind((unsigned)(cur - LIST_USER)));
        char sp[112], *o = s_cpy(sp, "SEL|");
        o = s_cpy(o, cur == 0 ? "Start empty" : cur == 1 ? "Builds a unique patch generatively" : user && !pw_store_name((unsigned)(cur - LIST_USER)) ? "Nothing here yet" : sub ? "Insert" : "Load");
        s_cpy(o, "|SAVE|Save page|ALGO|Pages");
        strip_set(sp);
    }
    list_footer(0);
}

static void name_knobs(const struct fm1_controls *c)   /* the patch's name: K2 moves, K3 changes the letter (the patch list and the SAVE page) */
{
    if (c->encoders[E_K2]) ui.name_pos = clampi(ui.name_pos + c->encoders[E_K2], 0, 11);
    if (c->encoders[E_K3]) {
        static const char chars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-#&+.!";
        int n = (int)sizeof chars - 1, at = 0;
        for (int i = 0; i < n; ++i) if (chars[i] == (pws.name[ui.name_pos] ? pws.name[ui.name_pos] : ' ')) at = i;
        at = ((at + c->encoders[E_K3]) % n + n) % n;
        for (int i = 0; i < ui.name_pos; ++i) if (!pws.name[i]) pws.name[i] = ' ';
        pws.name[ui.name_pos] = chars[at];
        int end = 12; while (end > 0 && (pws.name[end - 1] == ' ' || !pws.name[end - 1])) pws.name[--end] = 0;
        pws.revision++;
    }
}
/* the SAVE page: the eight user slots; SEL stores the patch (or, with KIND on GROUP, stores it as a group that other patches insert) */
static void draw_save(void)
{
    ui.save_cur = (int8_t)clampi(ui.save_cur, 0, PW_USER_SLOTS - 1);
    const int lh = 17;
    for (int u = 0, y = HDR + 5; u < PW_USER_SLOTS; ++u, y += lh) {
        int sel = u == ui.save_cur;
        const char *name = pw_store_name((unsigned)u);
        int filled = name != 0, grp = pw_store_kind((unsigned)u);
        if (!name) name = "(empty)";
        uint16_t kc = filled ? (grp ? cat_color[PW_C_FILTER] : C_GREEN) : C_LINE;
        if (sel) rfill(0, y - 3, W - 8, lh, C_PANEL2);
        fill(3, y, 3, 11, kc);
        text_fit(10, y, name, F_S, filled ? C_WHITE : C_LINE, 110);
        if (u == ui.user_slot) text_right(W - 40, y + 1, "CURRENT", F_XS, C_AMBER);
        else if (filled) text_right(W - 40, y + 1, grp ? "GROUP" : "PATCH", F_XS, kc);
        char t[4]; s_int(s_cpy(t, "U"), u + 1);
        text_right(W - 14, y, t, F_S, filled ? C_DIM : C_LINE);
    }
    text(6, HDR + 5 + PW_USER_SLOTS * lh + 1, ui.save_group ? "A GROUP: other patches insert it as one box" : "A PATCH: loaded whole, with its snapshots", F_XS, C_DIM);
    strip_set("SEL|Store here|K4|Kind|ALGO|Pages");
    list_footer(1);
}
/* the help page: "#" a heading, "~" a paragraph, anything else a key and what it does */
static const char *const help_rows[][2] = {
    {"#KEYS", ""},
    {"$", ""},
    {"#HOW IT WORKS", ""},
    {"~", "PATCHWERK turns the FM-1 into a modular: oscillators, filters, effects, sequencers and modulators, joined by cables. Any output can feed any input."},
    {"~", "Everything is a voltage: audio swings +/-5 V, pitch is 1 V per octave, gates are 0 or 5 V. So a sequencer can play a filter and an envelope can steer a delay."},
    {"~", "HOME is the patch screen. Add modules (FX), cable them (SELECT, SEL), shape them (knobs), play (keys, sequencers) and perform (ENV). Sixteen snapshots keep every knob, step and bypass."},
    {"~", "A button pressed again turns to its next page: FX (the add list's pages), ENV (PERFORM, MUTES, MUTATE), SEQ (the next sequencer), EDIT (the module's faces), SAVE (the menu's pages). ARP and LFO, pressed again, go back. ALGO turns pages too."},
    {"#PATCHING", ""},
    {"SELECT", "jack to jack; turned fast: module to module"},
    {"SEL", "grab a cable; SEL again plugs it in"},
    {"GLO+SEL", "lift the cable plugged in here, to plug it somewhere else (nothing is lost until it lands)"},
    {"REC", "unplug a jack or drop the cable. Held as long as it takes: delete the module"},
    {"FX", "add a module here: a strip on top shows where it goes. SELECT browses, SEL wires it in, ENV adds it unpatched, GLO+EDIT replaces the one there. FX again or ALGO: its pages (what it is, EXAMPLE PATCH: SEL loads it, VISUALIZATION)"},
    {"ARP", "MATRIX: modules, cables and their amounts. ARP again: back"},
    {"HOME", "the patch screen. Tapped there: the selected module's next knob page. Held as long as REC takes to delete: the CPU and the battery in the top bar"},
    {"#SHAPING", ""},
    {"KNOB 1-4", "the module's parameters; ALGO or HOME: the next page"},
    {"EDIT", "the module's own screen (a filter curve, a scope); again: its connections"},
    {"GLO held", "fine knob moves"},
    {"ARP+KNOB", "hold ARP and turn a knob: it goes on a MOD SEQ lane (one is added), every step at its value. Playing, each step you pass takes it as you turn. * marks it. GLO too: let it go"},
    {"#SEQUENCING", ""},
    {"SEQ", "16 steps on the lower keys; again: the next module with steps"},
    {"REC", "(on SEQ or TRIG) step record, live record, off. MONITOR sounds what you play. On a SAMPLER's page: arm, record IN into its RAM (a second at most), stop (PLAY stops it too); STORE keeps the sound with the patch"},
    {"TRIG SEQ", "lower keys: steps; OP1-4: track A-D. REC: first 4 lower keys = tracks, hard hit = accent"},
    {"PLAY", "start / stop"},
    {"MIDI", "clock and start/stop follow; program change 0-15: snapshots; CC 21-28: the PERFORM knobs"},
    {"#PERFORM", ""},
    {"ENV", "PERFORM: eight knobs from anywhere; hold ENV and turn a knob to add it. ENV again (or ALGO): MUTES (mute parts, MORPH between snapshots), then MUTATE (new variations). HOME leaves"},
    {"KEY", "PERFORM: recall snapshot 1-8 (hold SEL: 9-16). On the patch screen SEL+key recalls 1-8"},
    {"GLO+KEY", "SAVE the playing snapshot into that key, on any screen (hold SEL too: 9-16)"},
    {"GLO+SAVE", "knob edits reach all snapshots that share the value, or only this one"},
    {"SEL+MIDI", "hold SEL on PERFORM and move a MIDI knob: it drives that knob"},
    {"S1", "in the header: the snapshot playing; underlined when SONG is on"},
    {"LFO", "SONG: snapshot clips, then automation lanes (ALGO): points on a graph per knob. SEL new / move, K1 value, K2 shape, REC deletes or records. LFO again: back"},
    {"GLO+SEL", "SONG: duplicate the clip, with a snapshot of its own"},
    {"GLO+REC", "SONG: delete the automation lane"},
    {"#WITH GLO", ""},
    {"GLO+OCT-/+", "undo / redo"},
    {"GLO+ENV", "copy the module"},
    {"GLO+LFO", "paste it (a new module)"},
    {"GLO+PLAY", "bypass the module"},
    {"GLO+EDIT", "one voice per note, or shared"},
    {"GLO+FX", "group: SELECT turns through chains going to an output, SEL makes the one outlined a group; on a group box it opens it (HOME leaves), with a cable in hand too: it goes with you"},
    {"GLO+REC", "on a group box: ungroup. Held on any other module: delete it and wire its cables through, whatever ON DELETE says"},
    {"GLO+K1", "the patch screen: the tempo (K1 on the patch list)"},
    {"GLO+SEQ", "the patch screen: its size (AUTO, COMPACT, FULL); the piano roll: its view (AUTO, FOLD, KEYS)"},
    {"GLO+PRESET", "the metronome: turn up for on, down for off (MET in the top bar flashes with each beat)"},
    {"#GENERAL", ""},
    {"PRESETS", "the patch list: SEL loads (a GROUP is inserted into the open patch); K2 K3 name, K4 voices. ALGO: the next page: SAVE, SETTINGS, HELP. HOME closes"},
    {"SAVE", "the SAVE page, from anywhere; again: SETTINGS, HELP, PATCHES. SEL stores in the slot (U1-U8). K4 KIND GROUP: stored as a group, which the list inserts as one box"},
    {"OCT- OCT+", "the keyboard's octave"},
    {"HOME", "back to the patch. Held at power-on: a blank patch, not U1 (safe start)"},
};
#define HELP_ROWS ((int)(sizeof help_rows / sizeof help_rows[0]))
static void draw_perform(void)
{
    /* the snapshots: the first eight lower keys pick 1-8 (hold SEL: 9-16); GLO + a key copies the one playing there */
    for (int v = 0; v < PW_VARS; ++v) {
        int x = 2 + (v % 8) * 30, y = HDR + 2 + (v / 8) * 13;
        char d[4]; s_int(d, v + 1);
        if (v == pws.var_cur) rfill(x, y, 26, 12, C_AMBER); else frame_rect(x, y, 26, 12, v == ui.var_pend ? C_WHITE : C_LINE);
        if (v == ui.var_pend) frame_rect(x + 1, y + 1, 24, 10, C_WHITE);
        text_center(x + 13, y + 2, d, F_XS, v == pws.var_cur ? C_BG : v == ui.var_pend ? C_WHITE : C_DIM);
    }
    /* the eight knobs, two banks of four: the highlighted bank is on KNOB 1-4 (the strip has two rows here: the boxes are 11 px shorter) */
    ky_off = 11;
    const int ch = 72;
    for (int k = 0; k < PW_PERF; ++k) {
        int x = 1 + (k % 4) * 60, y = HDR + 29 + (k / 4) * (ch + 2), bank = k / 4 == ui.perf_bank;
        const uint8_t *a = pws.perf[k];
        rfill(x, y, 58, ch, bank ? C_PANEL2 : C_PANEL);
        if (k == ui.perf_cursor) { frame_rect(x, y, 58, ch, C_WHITE); }
        if (bank) { char kn[4] = {'K', (char)('1' + k % 4), 0, 0}; text_right(x + 55, y + ch - 12, kn, F_XS, C_DIM); }
        if (a[0] == 0xff || pws.mod[a[0]].used != 1) {
            char d[3] = {(char)('1' + k), 0, 0};
            text_center(x + 29, y + 8, d, F_M, C_LINE);
            continue;
        }
        const pw_type *t = &pw_types[pws.mod[a[0]].type];
        char lab[16], v[16];
        pw_module_label(a[0], lab);
        pw_format_param(a[0], a[1], v);
        int learned = pws.perf_cc[k] < 128 && pws.perf_cc[k] != 21 + k;   /* (CC 21-28: the knobs' own, not shown) */
        fill(x + 1, y, 56, 3, bank ? cat_color[t->cat] : mid_of(cat_color[t->cat]));
        text_fit(x + 4, y + 6, lab, F_XS, cat_color[t->cat], 52);
        text_fit(x + 4, y + 17, t->param[a[1]].name, F_S, bank ? C_TEXT : C_DIM, 52);
        int f = text_w(v, F_M) <= 52 ? F_M : F_S;   /* (one size for the numbers: a short one is not bigger) */
        text_center(x + 29, y + 32, v, f, bank ? C_WHITE : C_DIM);
        int bw = clampi((int)(param_norm(a[0], a[1]) * 48.0f + 0.5f), 0, 48);
        fill(x + 5, y + 53, 48, 4, C_PANEL);
        fill(x + 5, y + 53, bw, 4, bank ? cat_color[t->cat] : mid_of(cat_color[t->cat]));
        if (learned) { char cc[8], *o = s_cpy(cc, "CC"); s_int(o, pws.perf_cc[k]); text(x + 4, y + ch - 12, cc, F_XS, C_AMBER); }
    }
    strip_chg = ui.perf_cursor == PW_PERF ? 2 : 1;
    strip_set("KEYS|Snapshots|REC|Delete||Bar Sync\x01\nENV+KNOB|Add|SEL|Open|GLO+KEY|Copy");   /* (Bar Sync: SELECT to it, then KNOB 1 or SEL) */
}
/* the panel's two rows of buttons, as they sit on the FM-1: what each does here, and (grey) what it does with GLO held */
/* a 3x5 pixel alphabet, for the button names on the key map: smaller than the smallest font, so the eye goes to what a button does */
static const uint16_t micro_glyph[26] = {
    0x2BED, 0x6BAE, 0x3923, 0x6B6E, 0x79A7, 0x79A4, 0x396B, 0x5BED, 0x7497, 0x126A, 0x5BAD, 0x4927, 0x5FED,
    0x6B6D, 0x2B6A, 0x6BA4, 0x2B7B, 0x6BAD, 0x388E, 0x7492, 0x5B6F, 0x5B6A, 0x5BFD, 0x5AAD, 0x5A92, 0x72A7};
static int micro_text_w(const char *t) { int n = 0; while (*t++) ++n; return n * 4 - 1; }
static void micro_text(int cx, int y, const char *t, uint16_t col)
{
    int x = cx - micro_text_w(t) / 2;
    for (; *t; ++t, x += 4) {
        if (*t < 'A' || *t > 'Z') continue;
        uint16_t g = micro_glyph[*t - 'A'];
        for (int r = 0; r < 5; ++r) for (int c = 0; c < 3; ++c) if (g >> (14 - r * 3 - c) & 1u) fill(x + c, y + r, 1, 1, col);
    }
}
static void draw_keymap(int y0)
{
    /* each button: its name (tiny), what it does, and in amber what it does with GLO held */
    static const char *const key[2][6][3] = {
        {{"FX", "Add", "Group"}, {"SEL", "Cable", "Lift"}, {"ENV", "Perform", "Copy"}, {"LFO", "Song", "Paste"}, {"EDIT", "Detail", "Voices"}, {"GLO", "Hold", "Fine"}},
        {{"HOME", "Patch", ""}, {"SAVE", "Save", "All snaps"}, {"ARP", "Matrix", ""}, {"SEQ", "Steps", ""}, {"PLAY", "Start", "Bypass"}, {"REC", "Unplug", "Ungroup"}}};
    for (int r = 0; r < 2; ++r)
        for (int i = 0; i < 6; ++i) {
            int x = 4 + i * 39, y = y0 + r * 46;
            rfill(x, y, 35, 27, C_PLATE_E); rfill(x + 1, y + 1, 33, 25, C_PLATE);
            micro_text(x + 17, y + 4, key[r][i][0], C_WHITE);                  /* the button: tiny, only for finding it */
            { int f = text_w(key[r][i][1], F_S) <= 33 ? F_S : F_XS; text_center(x + 17, y + (f == F_S ? 12 : 13), key[r][i][1], f, C_WHITE); }   /* what it does: the point */
            text_center(x + 17, y + 30, key[r][i][2], F_XS, C_AMBER);        /* with GLO held */
        }
    text(6, y0 + 94, "Colored text: with GLO held.", F_S, C_AMBER);
    wrap_text(6, y0 + 108, "SELECT moves, ALGO turns pages, and a button pressed again goes to its next page. PRESETS: the patch list. SAVE: the save page, then the menu. OCT- OCT+: the keyboard's octave.", F_S, C_WHITE, W - 12, 6, 13);
}
#define HELP_KEYW 86                        /* a key's column; its text starts after it */
static int help_height(int i)
{
    const char *k = help_rows[i][0], *t = help_rows[i][1];
    if (k[0] == '$') return 190;
    if (k[0] == '#') return 26;
    if (k[0] == '~') return 13 * wrap_lines(t, F_S, W - 14) + 5;
    int n = wrap_lines(t, F_S, W - HELP_KEYW - 8);
    return 13 * (n < 1 ? 1 : n) + 4;
}
static int help_total(void) { int t = 0; for (int i = 0; i < HELP_ROWS; ++i) t += help_height(i); return t; }
static int help_max_scroll(void)   /* in pixels: the last row's end reaches the strip */
{
    int m = help_total() + 8 - (STRIP_Y - HDR);
    return m < 0 ? 0 : m;
}
static void draw_help(void)
{
    ui.help_scroll = clampi(ui.help_scroll, 0, help_max_scroll());
    int y = HDR + 4 - ui.help_scroll;
    for (int i = 0; i < HELP_ROWS; ++i) {
        const char *k = help_rows[i][0], *t = help_rows[i][1];
        int h = help_height(i);
        if (y + h <= HDR) { y += h; continue; }     /* (scrolled off the top) */
        if (y >= STRIP_Y) break;                    /* (not yet scrolled in) */
        if (k[0] == '$') draw_keymap(y);
        else if (k[0] == '#') {
            text(6, y + 5, k + 1, F_M, RGB(214, 216, 224));   /* (a light grey: a label, not another highlight) */
            fill(6 + text_w(k + 1, F_M) + 6, y + 13, W - 12 - text_w(k + 1, F_M) - 6, 1, C_PANEL2);
        } else if (k[0] == '~') wrap_text(8, y, t, F_S, C_WHITE, W - 14, 16, 13);
        else {
            text_fit(6, y, k, F_S, C_AMBER, HELP_KEYW - 6);
            wrap_text(HELP_KEYW, y, t, F_S, C_WHITE, W - HELP_KEYW - 8, 16, 13);
        }
        y += h;
    }
    header_draw();   /* (a row that is partly scrolled off passes under the header and the strip) */
    strip_set("SELECT|Scroll|ALGO|Patches|HOME|Close");
    int ms = help_max_scroll();
    if (ms > 0) {   /* where we are in it */
        int track = STRIP_Y - HDR - 8, bar = track * 24 / (24 + ms / 4), at = (track - bar) * ui.help_scroll / ms;
        fill(W - 3, HDR + 4 + at, 2, bar < 8 ? 8 : bar, C_LINE);
    }
}

/* ------------------------------------------------------------------ input */
static const uint8_t white_key[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static int white_index(unsigned key)
{
    for (int i = 0; i < 16; ++i) if (white_key[i] == key) return i;
    return -1;
}
static const uint8_t black_key[4] = {1, 3, 5, 8};
static int black_index(unsigned key)
{
    for (int i = 0; i < 4; ++i) if (black_key[i] == key) return i;
    return -1;
}
static void settings_flush(void);
static void go(unsigned page)
{
    if (page == ui.page) return;
    if ((ui.page == PG_SETTINGS && page != PG_SETTINGS) || (ui.page == PG_ADD && page != PG_ADD) || (ui.page == PG_SEQ && page != PG_SEQ)) settings_flush();   /* (the favorites are kept with the settings) */
    if (ui.page != PG_HELP && ui.page != PG_SETTINGS && ui.page != PG_SAVE) ui.prev_page = ui.page;
    ui.page = page;
    ui.confirm_kind = 0;
    if (page != PG_SEQ) ui.seq_rec = 0;
    if (page != PG_EDIT) ui.trig_rec = 0;
    if (page == PG_PATCHES && ui.last_list > 0) ui.list_cursor = ui.last_list;
}
static int confirm(unsigned kind, int arg, const char *ask)
{
    uint32_t now = fm1_millis();
    if (ui.confirm_kind == kind && ui.confirm_arg == arg && (int32_t)(now - ui.confirm_until) < 0) {
        ui.confirm_kind = 0;
        return 1;
    }
    ui.confirm_kind = kind; ui.confirm_arg = arg; ui.confirm_until = now + 3000u;
    notice(ask);
    ui.notice_until = ui.confirm_until;
    return 0;
}

/* ------------------------------------------------------------------ settings
 * One small object in the store (OBJ_SETTINGS): saved when the page is left. The patch list's SETTINGS row opens it. */
#define SET_MAGIC 0x31544553u
#define PW_PAL_WORDS (sizeof(pw_pal) / 2)
typedef struct { uint32_t magic; uint8_t pal, latch, autosave, edit_all, vel, msg, tone, bright; uint8_t fav[8]; uint16_t upal[33]; uint16_t upal_tag; } set_blob;   /* (fav was added later: an older saved blob is 8 bytes shorter; the USER palette after it, 68 more: an older firmware reads the first 20 bytes of a longer one) */
#define SET_OLD 12
#define SET_FAV 20
#define UPAL_TAG 0x5550u    /* (the first USER palette: 33 colors, a field for every ground and panel of the screens) */
#define UPAL_TAG2 0x5551u   /* (now: 18 colors, four greys, in the same room of the blob) */
void pw_ui_pal_from_legacy(const uint16_t *in, uint16_t *out)   /* the old 33-color order to the 18: the four greys, then the signal colors and the module groups */
{
    for (unsigned i = 0; i < 4; ++i) out[i] = in[i];
    for (unsigned i = 4; i < 18; ++i) out[i] = in[i + 15];
}
_Static_assert(sizeof(pw_pal) == 36 && PW_PAL_WORDS == 18, "the USER palette is 18 colors");
static int set_read(set_blob *b)
{
    memset(b, 0, sizeof *b);
    int n = pw_settings_load(b, sizeof *b);
    if (n != SET_OLD && n != SET_FAV && n != (int)sizeof *b) { memset(b, 0, sizeof *b); return 0; }
    return b->magic == SET_MAGIC;
}
/* (in the order shown) */
enum { S_LATCH, S_VEL, S_DEL, S_EDITS, S_CMP, S_AUTO, S_MCLK, S_PCH, S_PAL, S_TONE, S_BRIGHT, S_CTL, S_RATE, S_CORE, S_DIAG, S_RESET, S_N };
static const char *const rate_name[3] = {"44.1 kHz", "22.05 kHz", "AUTO"};   /* AUTO: 44.1, and 22.05 when the CPU overloads */
static const char *const vel_name[4] = {"OFF", "LOW", "NORMAL", "HIGH"};
static const char *const tone_name[3] = {"LIGHT", "NORMAL", "DEEP"};
static const char *const bright_name[4] = {"40%", "60%", "80%", "100%"};
static const uint8_t bright_pct[4] = {40, 60, 80, 100};
static uint16_t tone_c(uint16_t c, int deep, int pct, int surface)   /* a color through the screen settings: the darks darker (the panel lifts them), all of it dimmer */
{
    int ch[3] = {(c >> 11 & 31) * 255 / 31, (c >> 5 & 63) * 255 / 63, (c & 31) * 255 / 31};
    for (int i = 0; i < 3; ++i) {
        int x = ch[i];
        if (surface && deep && x < 64) {
            int y = x * x / 64;
            x = deep == 1 ? (x + y) / 2 : y;
        }
        ch[i] = x * pct / 100;
    }
    return RGB(ch[0], ch[1], ch[2]);
}
static pw_pal cpal;                 /* a custom palette (the web page's palette editor): used while it is on and the unit's palette is still the one it was made over */
static uint8_t cpal_on, cpal_for;
static void settings_apply(void)
{
    const uint16_t *src = (cpal_on && cpal_for == ui.pal_i) ? (const uint16_t *)&cpal : (const uint16_t *)pal_ptr(ui.pal_i);
    uint16_t *dst = (uint16_t *)&npal;
    int cus = cpal_on && cpal_for == ui.pal_i;   /* (a custom palette is shown as typed: no DARKS or BRIGHTNESS on it) */
    for (unsigned i = 0; i < sizeof npal / 2; ++i) dst[i] = cus ? src[i] : tone_c(src[i], ui.tone_i, bright_pct[ui.bright_i & 3], i < 4);   /* (the first four fields: the greys) */
    band_valid = 0;
}
/* PATCHWERK's own faces for the platform's screens (boot, update, crash, calibration: app.c's app_sys_text), so fm1-base's three need not be linked
 * (29 KB of flash). face 0 body, 1 heading, 2 the big one; plot(x, y, coverage 1-15) is called for each lit pixel */
static const uint8_t sys_font[3] = {1, 2, 2};
int pw_sys_h(int face) { return fonts[sys_font[face % 3]]->lh; }
int pw_sys_w(int face, const char *s) { return text_w(s, sys_font[face % 3]); }
void pw_sys_text(int x, int y, int face, const char *s, void (*plot)(int, int, unsigned))
{
    const pw_fontdef *f = fonts[sys_font[face % 3]];
    for (; *s; ++s) {
        const pw_glyph *g = &f->g[glyph_index(f, (unsigned char)*s)];
        int stride = (g->w + 1) / 2;
        for (int r = 0; r < g->h; ++r) {
            const uint8_t *src = f->bits + g->off + r * stride;
            for (int c = 0; c < g->w; ++c) {
                unsigned a = (c & 1) ? (src[c >> 1] & 15u) : (src[c >> 1] >> 4);
                if (a) plot(x + g->x + c, y + g->y + r, a);
            }
        }
        x += g->adv;
    }
}
/* the palette as a screen setting makes it (pal < 0: the defaults, which the web page shows whatever the linked FM-1 is set to) */
int pw_ui_palette(int pal, int tone, int bright, uint16_t *out, int max)
{
    unsigned n = sizeof npal / 2;
    if (pal < 0) { pal = PW_PAL_DEFAULT; tone = PW_TONE_DEFAULT; bright = 3; }
    const uint16_t *src = (const uint16_t *)((unsigned)pal == PAL_COUNT && upal_set ? &upal_p : &pals[(unsigned)pal % PAL_COUNT]);   /* (row PAL_COUNT: the USER palette, when there is one) */
    if ((int)n > max) n = (unsigned)max;
    for (unsigned i = 0; i < n; ++i) out[i] = tone_c(src[i], tone, bright_pct[bright & 3], i < 4);
    return (int)n;
}
void pw_ui_set_palette(int pal) { ui.pal_i = (uint8_t)((unsigned)pal % PAL_COUNT); cpal_on = 0; settings_apply(); }
void pw_ui_custom(int idx, int v) { if (idx >= 0 && (unsigned)idx < sizeof cpal / 2) ((uint16_t *)&cpal)[idx] = (uint16_t)v; }   /* (one color of the custom palette, RGB565, in pw_pal's order) */
void pw_ui_custom_use(int on) { cpal_on = (uint8_t)(on != 0); cpal_for = ui.pal_i; settings_apply(); pw_ui_redraw_all(); }   /* (for tests and screenshots) */
static void set_user_pal(const set_blob *b)   /* the USER palette from the saved settings (a tag and colors that are not noise) */
{
    upal_set = 0;
    if (b->upal_tag == UPAL_TAG2) memcpy(&upal_p, b->upal, sizeof upal_p);
    else if (b->upal_tag == UPAL_TAG) pw_ui_pal_from_legacy(b->upal, (uint16_t *)&upal_p);
    else return;
    upal_set = 1;
}
int pw_ui_user_palette(const uint16_t *v)   /* the web page's USER palette: 18 colors (RGB565, pw_pal's order) made the one in use and kept; 0: removed */
{
    if (v) { memcpy(&upal_p, v, sizeof upal_p); upal_set = 1; ui.pal_i = (uint8_t)PAL_COUNT; }
    else { upal_set = 0; if (ui.pal_i >= PAL_COUNT) ui.pal_i = PW_PAL_DEFAULT; }
    settings_apply(); ui.set_dirty = 1; settings_flush();
    return 0;
}
int pw_ui_user_palette_get(uint16_t *out) { if (!upal_set) return 0; memcpy(out, &upal_p, sizeof upal_p); return 1; }
static void settings_defaults(void)
{
    ui.pal_i = PW_PAL_DEFAULT; ui.tone_i = PW_TONE_DEFAULT; ui.bright_i = 3; ui.glo_latch = 0; ui.glo_on = 0; ui.autosave = 0; ui.edit_def = 0; ui.vel_mode = 2; ui.ctl_i = 0; ui.rate_i = 0; ui.smart_i = 0; ui.cmp_auto = 0; ui.core_i = 0; ui.mclk_i = 0; ui.pch_i = 0;
}
static void settings_load(void)
{
    set_blob b;
    settings_defaults();
    if (set_read(&b)) {
        set_user_pal(&b);
        for (unsigned k = 0; k < 8; ++k) ui.fav[k] = b.fav[k] <= PW_T_COUNT ? b.fav[k] : 0;
        ui.pal_i = (uint8_t)pal_row_of(b.pal); ui.glo_latch = b.latch & 1; ui.cmp_auto = b.latch >> 1 & 1; ui.autosave = b.autosave & 1;
        ui.edit_def = b.edit_all & 1; ui.vel_mode = b.vel & 3; if (b.msg & 0x80) { ui.ctl_i = b.msg & 1; ui.smart_i = b.msg >> 1 & 1; ui.rate_i = (uint8_t)(b.msg >> 2 & 3); if (ui.rate_i > 2) ui.rate_i = 0; ui.core_i = !(b.msg >> 4 & 1); ui.mclk_i = b.msg >> 5 & 1; ui.pch_i = b.msg >> 6 & 1; }   /* (the byte the MESSAGES setting used: 0x80 marks the new meaning) */
        ui.tone_i = b.tone > 2 ? 1 : b.tone; ui.bright_i = (uint8_t)(3 - (b.bright & 3));
    }
    settings_apply();
    pws.edit_all = ui.edit_def; pws.ctl_rate = ui.ctl_i; pws.key_sens = ui.vel_mode; pws.clk_out = ui.mclk_i; pws.pc_patch = ui.pch_i; pw_set_rate(ui.rate_i == 1);
}
static void splash_pal(void)
{
    static uint8_t done;
    set_blob b;
    unsigned i = 0;
    if (done) return;
    done = 1;
    ui.tone_i = PW_TONE_DEFAULT; ui.bright_i = 3; i = PW_PAL_DEFAULT;
    if (set_read(&b)) { set_user_pal(&b); i = (int)pal_row_of(b.pal); ui.tone_i = b.tone > 2 ? 1 : b.tone; ui.bright_i = (uint8_t)(3 - (b.bright & 3)); }
    ui.pal_i = (uint8_t)i;
    settings_apply();
}
static void set_fill(set_blob *b)   /* the settings as they are now, as the store keeps them */
{
    memset(b, 0, sizeof *b);
    memcpy(b->fav, ui.fav, 8);
    b->magic = SET_MAGIC; b->pal = (uint8_t)(ui.pal_i < PAL_COUNT ? pal_id[ui.pal_i] : PAL_USER_ID);
    if (upal_set) { memcpy(b->upal, &upal_p, sizeof upal_p); b->upal_tag = UPAL_TAG2; }
    b->latch = (uint8_t)(ui.glo_latch | ui.cmp_auto << 1); b->autosave = ui.autosave; b->edit_all = ui.edit_def; b->vel = ui.vel_mode;
    b->msg = (uint8_t)(0x80 | ui.ctl_i | ui.smart_i << 1 | ui.rate_i << 2 | (!ui.core_i) << 4 | ui.mclk_i << 5 | ui.pch_i << 6);
    b->tone = ui.tone_i; b->bright = (uint8_t)(3 - ui.bright_i);
}
static void settings_flush(void)
{
    set_blob b;
    if (!ui.set_dirty) return;
    set_fill(&b);
    pw_settings_save(&b, sizeof b);
    ui.set_dirty = 0;
}
int pw_ui_settings_get(uint8_t *out, int max)   /* the settings (palette, USER palette, favorites, every row) for a backup: the bytes, or 0 */
{
    set_blob b;
    if (max < (int)sizeof b) return 0;
    set_fill(&b); memcpy(out, &b, sizeof b);
    return (int)sizeof b;
}
int pw_ui_settings_put(const uint8_t *in, int n)   /* a backup's settings back: kept and in use at once (an older, shorter blob is fine); 0, or < 0 */
{
    set_blob b;
    if (n != SET_OLD && n != SET_FAV && n != (int)sizeof b) return -1;
    memset(&b, 0, sizeof b); memcpy(&b, in, (size_t)n);
    if (b.magic != SET_MAGIC) return -1;
    if (pw_settings_save(&b, sizeof b) < 0) return -2;
    settings_load(); ui.set_dirty = 0; pw_ui_redraw_all();
    return 0;
}
static void core_apply(int announce)   /* the DUAL CORE setting: start CPU1 if it is wanted and not yet running, and let the engine move the heavy modules */
{
    int st;
    if (!ui.core_i) { pw_cpu1_offload = 0; return; }
    st = pw_plat_cpu1(0, 0);
    if (st == 0) { pw_plat_cpu1(0, 1); st = pw_plat_cpu1(0, 0); }
    if (st == 1 && pw_off_state != 3) pw_cpu1_offload = 2;
    else if (announce && st >= 0) notice("DUAL CORE DID NOT START");
}
static void factory_reset(void)
{
    int all = ui.reset_all;
    settings_defaults(); settings_apply(); pws.edit_all = 0; pws.ctl_rate = 0; pws.key_sens = ui.vel_mode; pw_set_rate(0); core_apply(0); ui.set_dirty = 1; settings_flush();
    if (all) {
        for (unsigned k = 0; k < PW_USER_SLOTS; ++k) pw_store_erase(k);
        ui.user_slot = -1; ui.list_cursor = LIST_TOP; ui.last_list = LIST_TOP;
    }
    notice(all ? "SETTINGS AND USER PATCHES RESET" : "SETTINGS RESET");
}
static void set_change(int row, int d)
{
    switch (row) {
    case S_LATCH: ui.glo_latch = d > 0; ui.glo_on = 0; break;
    case S_PAL: ui.pal_i = (uint8_t)wrap_idx(ui.pal_i + d, (int)pal_n()); settings_apply(); break;
    case S_TONE: ui.tone_i = (uint8_t)clampi(ui.tone_i + d, 0, 2); settings_apply(); break;
    case S_BRIGHT: ui.bright_i = (uint8_t)clampi(ui.bright_i + d, 0, 3); settings_apply(); break;
    case S_AUTO: ui.autosave = d > 0; break;
    case S_MCLK: ui.mclk_i = d > 0; pws.clk_out = ui.mclk_i; break;
    case S_PCH: ui.pch_i = d > 0; pws.pc_patch = ui.pch_i; break;
    case S_CMP: ui.cmp_auto = d > 0; if (!ui.cmp_auto && ui.compact == 2) ui.compact = 0; pws.revision++; break;
    case S_EDITS: ui.edit_def = d > 0; pws.edit_all = ui.edit_def; pws.revision++; break;
    case S_VEL: ui.vel_mode = (uint8_t)clampi(ui.vel_mode + d, 0, 3); pws.key_sens = ui.vel_mode; break;
    case S_CTL: ui.ctl_i = d > 0; pws.ctl_rate = ui.ctl_i; break;
    case S_DEL: ui.smart_i = d > 0; break;
    case S_RATE: ui.rate_i = (uint8_t)clampi(ui.rate_i + d, 0, 2); pw_set_rate(ui.rate_i == 1); break;
    case S_CORE: ui.core_i = d > 0; core_apply(1); break;
    case S_RESET: ui.reset_all = d > 0; return;
    }
    ui.set_dirty = 1;
}
static void autosave_tick(uint32_t now)   /* an edited user patch is stored again when you stop (not while playing or holding keys: a write stops the sound) */
{
    if (ui.user_slot != ui.as_slot) { ui.as_slot = (int8_t)ui.user_slot; ui.as_rev = pws.revision; ui.as_dirty = 0; return; }
    if (!ui.autosave || ui.user_slot < 0) { ui.as_rev = pws.revision; ui.as_dirty = 0; return; }
    if (pws.revision != ui.as_rev) { ui.as_rev = pws.revision; ui.as_ms = now; ui.as_dirty = 1; }
    if (ui.as_dirty && now - ui.as_ms > 6000u && !pws.playing && !ui.keys) {
        char m[24], *o = s_cpy(m, "AUTOSAVED U");
        ui.as_dirty = 0;
        if (pw_store_save((unsigned)ui.user_slot) == 0) { s_int(o, ui.user_slot + 1); notice(m); }
    }
}
static void draw_settings(void)
{
    static const char *const label[S_N] = {"GLO LATCH", "KEY SENSITIVITY", "AUTO-REPATCH", "EDITS", "AUTO COMPACT", "AUTOSAVE", "MIDI CLOCK OUT", "PROGRAM CHANGE", "PALETTE", "DARKS", "BRIGHTNESS", "CONTROL RATE", "SAMPLE RATE", "DUAL CORE", "DIAGNOSTICS", "FACTORY RESET"};
    static const char *const desc[S_N] = {
        "Off: hold GLO + a key. On: tap GLO, then the key.",
        "How hard the keys must be hit (the KEYS VEL output only).",
        "On: deleting a module reconnects its neighbours.",
        "Knob edits reach this snapshot only, or all that share the value.",
        "On: a big patch shrinks to tags and dots by itself.",
        "Stores an edited user patch when you stop playing.",
        "On: sends MIDI clock, start and stop while the transport runs (not while a clock comes in).",
        "SNAPSHOT: program 0-15 picks a snapshot. PATCH: 0-7 load U1-U8, 8-57 the factory patches.",
        "The colors of every screen.",
        "The dark greys: deeper if the panel lifts them.",
        "The screen dimmed in software.",
        "Full: slow LFOs and envelopes run every sample. Quarter: less CPU.",
        "22.05 kHz: half the CPU, duller. AUTO: 22.05 when overloaded.",
        "Experimental: heavy modules on both cores. Off to start: some units are less steady with it. HOME at power-on skips it.",
        "Memory, CPU and battery, and what each module costs.",
        "SEL twice. Settings only, or with your patches. Samples stay."};
    header_draw();
    /* the rows are spaced out and the list scrolls with the cursor; under it the row's description, set apart */
    const int lh = 17, desc_y = STRIP_Y - 30, rows_vis = (desc_y - (HDR + 3)) / lh;   /* (the patch list's spacing) */
    int top = ui.set_row - rows_vis / 2;
    if (top > S_N - rows_vis) top = S_N - rows_vis;
    if (top < 0) top = 0;
    int y = HDR + 5;
    for (int i = top; i < top + rows_vis && i < S_N; ++i, y += lh) {
        int on = i == ui.set_row;
        if (on) rfill(0, y - 3, W - 8, lh, C_PANEL2);
        fill(3, y, 3, 11, i <= S_PCH ? cat_color[PW_C_FILTER] : i <= S_BRIGHT ? cat_color[PW_C_FX] : i <= S_CORE ? cat_color[PW_C_SOURCE] : cat_color[PW_C_MOD]);   /* (its group: how it behaves, how it looks, the engine, the system) */
        text(10, y, label[i], F_S, C_WHITE);
        const char *val = "";
        uint16_t col = C_DIM;
        switch (i) {
        case S_LATCH: val = ui.glo_latch ? "ON" : "OFF"; col = ui.glo_latch ? C_AMBER : C_DIM; break;
        case S_PAL: val = pal_label(ui.pal_i); col = C_AMBER; break;
        case S_TONE: val = tone_name[ui.tone_i]; col = C_AMBER; break;
        case S_BRIGHT: val = bright_name[ui.bright_i]; col = ui.bright_i < 3 ? C_AMBER : C_DIM; break;
        case S_CMP: val = ui.cmp_auto ? "ON" : "OFF"; col = ui.cmp_auto ? C_AMBER : C_DIM; break;
        case S_AUTO: val = ui.autosave ? "ON" : "OFF"; col = ui.autosave ? C_AMBER : C_DIM; break;
        case S_MCLK: val = ui.mclk_i ? "ON" : "OFF"; col = ui.mclk_i ? C_AMBER : C_DIM; break;
        case S_PCH: val = ui.pch_i ? "PATCH" : "SNAPSHOT"; col = ui.pch_i ? C_AMBER : C_DIM; break;
        case S_EDITS: val = ui.edit_def ? "ALL SNAPSHOTS" : "THIS SNAPSHOT"; col = ui.edit_def ? C_RED : C_AMBER; break;
        case S_DEL: val = ui.smart_i ? "ON" : "OFF"; col = ui.smart_i ? C_AMBER : C_DIM; break;
        case S_RATE: val = rate_name[ui.rate_i]; col = ui.rate_i ? C_AMBER : C_DIM; break;
        case S_VEL: val = vel_name[ui.vel_mode]; col = ui.vel_mode ? C_AMBER : C_DIM; break;
        case S_CTL: val = ui.ctl_i ? "QUARTER" : "FULL"; col = ui.ctl_i ? C_AMBER : C_DIM; break;
        case S_CORE: val = ui.core_i ? (pw_plat_cpu1(0, 0) == 1 ? "ON" : pw_plat_cpu1(0, 0) < 0 ? "NOT HERE" : "ON (NOT RUNNING)") : "OFF"; col = ui.core_i ? C_AMBER : C_DIM; break;
        case S_DIAG: val = "OPEN"; col = C_AMBER; break;
        case S_RESET: val = ui.reset_all ? "SETTINGS + PATCHES" : "SETTINGS"; col = C_RED; break;
        }
        int vw = text_w(val, F_S);
        text_right(W - 14, y, val, F_S, col);
        if (i == S_PAL) {   /* the colors: four chips */
            const pw_pal *q = pal_ptr(ui.pal_i);
            uint16_t chip[8] = {q->cat[0], q->cat[1], q->cat[2], q->cat[3], q->cat[4], q->cat[5], q->cat[6], q->amber};   /* (the seven module colors and the accent) */
            for (int k = 0; k < 8; ++k) rfill(W - 14 - vw - 8 - (8 - k) * 9, y + 1, 7, 9, chip[k]);
        }
    }
    {   /* where the cursor is in the list */
        int tr = rows_vis * lh - 4, bar = tr * rows_vis / S_N, at = S_N > rows_vis ? (tr - bar) * top / (S_N - rows_vis) : 0;
        fill(W - 7, HDR + 4, 3, tr, C_PANEL);
        fill(W - 7, HDR + 4 + at, 3, bar, C_DIM);
    }
    /* what the selected row does, apart from the numbers above */
    fill(4, desc_y, W - 8, 1, C_LINE);
    wrap_text(8, desc_y + 4, desc[ui.set_row], F_XS, C_TEXT, W - 16, 2, 11);   /* (two lines for it) */
    strip_set(ui.set_row == S_RESET ? "SELECT|Item|K1|Change|SEL|Reset" : ui.set_row == S_DIAG ? "SELECT|Item|SEL|Open|ALGO|Help|HOME|Back" : "SELECT|Item|K1|Change|ALGO|Help|HOME|Back");
}
static void diag_bar(int y, const char *label, unsigned pct, uint16_t col, const char *val)   /* one line: its name, a bar, what it is */
{
    text(8, y, label, F_XS, C_DIM);
    rfill(78, y + 3, 80, 5, C_PANEL);
    int fw = (int)(pct > 100u ? 80u : pct * 80u / 100u);
    if (fw > 0) rfill(78, y + 3, fw, 5, col);
    text_right(W - 8, y, val, F_XS, col == C_GREEN ? C_TEXT : col);
}
static uint32_t diag_snap[PW_MAX_MODULES], diag_snap_all, diag_snap_n;   /* (the last second measured on the CPU page) */
static void draw_diag_cpu(void)   /* DIAGNOSTICS, second page: what each module costs of the audio block, and the second core */
{
    uint32_t *const snap = diag_snap;
    char v[64], *o;
    int y = HDR + 6;
    header_draw();
    if (pw_prof_blocks >= 344u) {   /* a second of blocks: what was measured is shown while the next second is measured */
        pw_plat_lock();
        for (unsigned k = 0; k < PW_MAX_MODULES; ++k) { snap[k] = pw_prof_tk[k]; pw_prof_tk[k] = 0; }
        diag_snap_all = pw_prof_all; diag_snap_n = pw_prof_blocks; pw_prof_all = 0; pw_prof_blocks = 0;
        pw_plat_unlock();
    }
    uint32_t per = pw_plat_ticks_per_us() * 2902u;   /* (the block: 128 frames at 44.1 kHz, or 64 at 22.05) */
    if (!diag_snap_n || !per) { text(8, y + 20, "MEASURING...", F_S, C_DIM); strip_set("ALGO|Screen test|HOME|Back"); return; }
    uint32_t sum = 0;
    for (unsigned k = 0; k < PW_MAX_MODULES; ++k) sum += snap[k];
    const uint32_t den = (per * diag_snap_n) / 100u;   /* (32-bit only: the FM-1 has no 64-bit divide) */
    unsigned eng = den ? (unsigned)(diag_snap_all / den) : 0u, mods = den ? (unsigned)(sum / den) : 0u;
    o = s_cpy(v, "ENGINE "); o = s_int(o, (int)eng); o = s_cpy(o, "%   MODULES "); o = s_int(o, (int)mods); s_cpy(o, "%");
    text(8, y, v, F_S, C_TEXT);
    y += 20;
    uint8_t used[PW_MAX_MODULES] = {0};
    for (int row = 0; row < 7; ++row, y += 17) {
        int best = -1; uint32_t bv = 0;
        for (unsigned k = 0; k < PW_MAX_MODULES; ++k) if (!used[k] && snap[k] > bv) { bv = snap[k]; best = (int)k; }
        if (best < 0) break;
        used[best] = 1;
        unsigned pct = den ? (unsigned)(bv / den) : 0u;
        char lab[12]; pw_module_label((unsigned)best, lab);
        o = s_int(v, (int)pct); s_cpy(o, "%");
        diag_bar(y, lab, pct, pct > 25u ? C_RED : pct > 12u ? C_AMBER : C_GREEN, v);
    }
    {   /* the second core */
        int st = pw_plat_cpu1(0, 0);
        text(8, HDR + 6 + 20 + 7 * 17 + 4, st == 1 ? "CPU1: RUNNING" : "CPU1: NOT RUNNING", F_XS, st == 1 ? C_GREEN : C_DIM);
        if (st == 1) {
            if (pw_off_state == 3) s_cpy(v, "OFFLOAD: CPU1 FAILED");
            else if (!pw_cpu1_offload) s_cpy(v, "OFFLOAD OFF");
            else if (pw_off_n) { o = s_int(v, (int)pw_off_n); o = s_cpy(o, " ON CPU1, "); o = s_int(o, (int)pw_off_us); s_cpy(o, " US A BLOCK"); }
            else s_cpy(v, pw_off_probing ? "TIMING MODULES..." : "NOTHING TO MOVE");
            text(8, HDR + 6 + 20 + 7 * 17 + 4 + 12, v, F_XS, pw_cpu1_offload && pw_off_n ? C_GREEN : C_AMBER);
        }
    }
    strip_set("ALGO|Screen test|HOME|Back");
}
unsigned pw_diag_report(char *buf, unsigned max)   /* the numbers the CPU page shows, as text lines, for the web page to save */
{
    char *o = buf, *end = buf + max - 80;
    #define LN(str) do { if (o < end) { o = s_cpy(o, str); *o++ = '\n'; } } while (0)
    #define NUM(n) (o = s_int(o, (int)(n)))
    #define TXT(t) (o = s_cpy(o, t))
    #define NL() (*o++ = '\n')
    #define HEX(w) do { for (int d_ = 7; d_ >= 0; --d_) *o++ = "0123456789ABCDEF"[((w) >> (d_ * 4)) & 15u]; } while (0)
    LN("PATCHWERK " PW_VERSION "  " PW_IDENT);
    {   /* what the DIAGNOSTICS screen's bars show: the firmware's use of the FM-1's flash, RAM and pool (bytes used / bytes of room) */
        uint32_t bd[6], used = 0, nm = 0, nc = 0;
        pw_plat_budget(bd);
        for (unsigned k = 0; k < PW_MAX_MODULES; ++k) if (pws.mod[k].used) { used += pws.mod[k].reg_len; ++nm; }
        for (unsigned k = 0; k < PW_MAX_CABLES; ++k) nc += pws.cable[k].used;
        TXT("FLASH "); NUM(bd[0]); TXT(" / "); NUM(bd[1]); NL();
        TXT("STATIC RAM "); NUM(bd[2]); TXT(" / "); NUM(bd[3]); NL();
        TXT("POOL "); NUM(bd[4]); TXT(" / "); NUM(bd[5]); NL();
        TXT("MODULE MEM "); NUM(used); TXT(" / "); NUM(PW_POOL_BYTES); NL();
        TXT("MODULES "); NUM(nm); TXT("/"); NUM(PW_MAX_MODULES); TXT("  CABLES "); NUM(nc); TXT("/"); NUM(PW_MAX_CABLES);
        TXT("  SAMPLE FLASH FREE "); NUM(pw_flash_free_kb()); TXT("K"); NL();
        { unsigned cpu = pw_plat_cpu(); TXT("CPU "); if (cpu > 100u) TXT("-"); else { NUM(cpu); TXT("%"); } }
        { int bp = bat_pct(); TXT("  BATTERY "); if (bp > 100) TXT("USB POWER"); else if (bp < 0) TXT("-"); else { NUM(bp); TXT("%"); } NL(); }
    }
    uint32_t per = pw_plat_ticks_per_us() * 2902u;
    uint32_t den = per && diag_snap_n ? (per * diag_snap_n) / 100u : 0u, sum = 0;
    for (unsigned k = 0; k < PW_MAX_MODULES; ++k) sum += diag_snap[k];
    if (den) { TXT("ENGINE "); NUM(diag_snap_all / den); TXT("%  MODULES "); NUM(sum / den); TXT("%  ("); NUM(diag_snap_n); TXT(" BLOCKS)"); NL(); }
    else LN("NO TIMING: OPEN CPU PAGE FIRST");
    { uint32_t n = 0; int st = pw_plat_cpu1(&n, 0); TXT("CPU1 STATE "); NUM(st); TXT(" COUNT "); NUM(n >> 10); NL(); }
    TXT("OFFLOAD MODE "); NUM(pw_cpu1_offload); TXT(" STATE "); NUM(pw_off_state); TXT("  ON CPU1 "); NUM(pw_off_n); TXT("  CPU1 WORK "); NUM(pw_off_us);
    TXT(" US  WAIT "); NUM(pw_off_wait_us); TXT(" US  COPY "); NUM(pw_off_copy_us); TXT(" US  STILL TIMING "); NUM(pw_off_probing); NL();
    LN("SLOT MODULE SHARE% COST-US REASON");
    for (unsigned k = 0; k < PW_MAX_MODULES; ++k) {
        if (pws.mod[k].used != 1) continue;
        char lab[12]; unsigned us = 0, why = pw_off_reason(k, &us);
        pw_module_label(k, lab);
        NUM(k); TXT("  "); TXT(lab); TXT("  "); if (den) NUM(diag_snap[k] / den); else TXT("-"); TXT("%  "); NUM(us); TXT("  R"); NUM(why);
        if (pws.mod[k].flags & PW_F_POLY) TXT("  [POLY]");
        NL();
        if (o >= end) break;
    }
    #undef LN
    #undef NUM
    #undef TXT
    #undef NL
    #undef HEX
    *o = 0;
    return (unsigned)(o - buf);
}
static void draw_screen_test(void)   /* DIAGNOSTICS, third page: greys and the palette's colors as plain swatches, to set the web page's look of the screen against this one */
{
    char v[40], *o;
    header_draw();
    int y = HDR + 4;
    text(8, y, "GREYS 0 TO 255", F_XS, C_DIM);
    for (int k = 0; k < 16; ++k) fill(k * 15, y + 10, 15, 28, RGB(k * 17, k * 17, k * 17));
    y += 44;
    text(8, y, "DARKS: THE SCREEN'S FIRST 16 STEPS", F_XS, C_DIM);   /* (5 bits red and blue, 6 green: the steps the screen has, each one a grey of its own) */
    for (int k = 0; k < 16; ++k) fill(k * 15, y + 10, 15, 28, (uint16_t)((k << 11) | (2 * k << 5) | k));
    y += 44;
    text(8, y, "THE PALETTE'S COLORS", F_XS, C_DIM);
    for (int k = 0; k < 7; ++k) fill(k * 21, y + 10, 21, 24, cat_color[k]);
    fill(147, y + 10, 21, 24, C_GREEN); fill(168, y + 10, 21, 24, C_RED); fill(189, y + 10, 21, 24, C_AMBER); fill(210, y + 10, 21, 24, C_TEXT);
    o = s_cpy(v, pal_label(ui.pal_i)); o = s_cpy(o, "  DARKS "); o = s_cpy(o, tone_name[ui.tone_i % 3]); o = s_cpy(o, "  "); s_cpy(o, bright_name[ui.bright_i & 3]);
    text(8, y + 42, v, F_XS, C_TEXT);
    strip_set("ALGO|Next page|HOME|Back");
}
static void draw_diag(void)
{
    char v[64], *o;
    int y;
    if (ui.diag == 2) { draw_diag_cpu(); return; }
    if (ui.diag == 3) { draw_screen_test(); return; }
    header_draw();
    y = HDR + 10;
    uint32_t bd[6];
    unsigned used = 0, np = 0, nm = 0, nc = 0;
    pw_plat_budget(bd);
    for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used) { used += pws.mod[i].reg_len; ++nm; }
    for (unsigned k = 0; k < PW_MAX_CABLES; ++k) nc += pws.cable[k].used;
    for (unsigned k = 0; k < PW_USER_SLOTS; ++k) if (pw_store_name(k)) ++np;
    static const char *const bname[4] = {"FLASH", "STATIC RAM", "POOL", "MODULE MEM"};
    const uint32_t have[4] = {bd[0], bd[2], bd[4], used}, cap[4] = {bd[1], bd[3], bd[5], PW_POOL_BYTES};
    for (int k = 0; k < 4; ++k, y += 20) {   /* what the firmware takes of what the FM-1 has */
        uint32_t pct = cap[k] ? have[k] * 100u / cap[k] : 0u;
        o = s_int(v, (int)((have[k] + 512u) / 1024u)); o = s_cpy(o, "K / "); o = s_int(o, (int)((cap[k] + 512u) / 1024u)); s_cpy(o, "K");
        diag_bar(y, bname[k], pct, pct >= 95u ? C_RED : pct >= 85u ? C_AMBER : C_GREEN, v);
    }
    {   /* the audio load */
        unsigned cpu = pw_plat_cpu();
        if (cpu > 100u) diag_bar(y, "CPU", 0, C_GREEN, "--");
        else { o = s_int(v, (int)cpu); s_cpy(o, "%"); diag_bar(y, "CPU", cpu, cpu > 85u ? C_RED : cpu > 70u ? C_AMBER : C_GREEN, v); }
        y += 20;
    }
    {   /* the battery: about 3.4 V empty, 4.15 V full (a count is about 6.6 mV); on USB power the reading is the charger's */
        int bp = bat_pct(); unsigned lvl = fm1_battery_level();
        if (bp > 100) diag_bar(y, "BATTERY", 100, C_GREEN, "USB POWER");
        else if (bp < 0) diag_bar(y, "BATTERY", 0, C_GREEN, "--");
        else { o = s_int(v, bp); s_cpy(o, "%"); diag_bar(y, "BATTERY", (unsigned)bp, lvl == 0 ? C_RED : lvl == 1 ? C_AMBER : C_GREEN, v); }
        y += 20;
    }
    o = s_cpy(v, PW_VERSION "  " PW_IDENT);
    text(8, y + 4, v, F_XS, C_TEXT);
    o = s_int(v, (int)nm); o = s_cpy(o, "/"); o = s_int(o, PW_MAX_MODULES); o = s_cpy(o, " MOD  "); o = s_int(o, (int)nc); o = s_cpy(o, "/"); o = s_int(o, PW_MAX_CABLES);
    o = s_cpy(o, " CAB  "); o = s_int(o, (int)np); o = s_cpy(o, "/8 PATCH  "); o = s_int(o, (int)pw_flash_free_kb()); s_cpy(o, "K SMP");
    text(8, y + 16, v, F_XS, C_DIM);
    strip_set("ALGO|CPU by module|HOME|Back");
}
static void add_type(unsigned type, int wired)
{
    int slot = -1, is_out = 0, ctx = wire_context(); unsigned port = 0;
    if (wired) wire_target(ui.page_from_patch, &slot, &is_out, &port);
    int x, y;
    free_spot(&x, &y);
    int n0 = 0; for (unsigned c = 0; c < PW_MAX_CABLES; ++c) n0 += pws.cable[c].used;
    int s = pw_add_wired(type, x, y, slot, is_out, port, -1);
    if (s < 0) { notice("PATCH FULL"); return; }
    int n1 = 0; for (unsigned c = 0; c < PW_MAX_CABLES; ++c) n1 += pws.cable[c].used;
    ui.param_page[s] = 0;
    recent_push(type);
    ui.last_type[wired ? ctx : 3] = (uint8_t)(type + 1);   /* where the add list starts next time here */
    char label[16], msg[44], *o;
    pw_module_label((unsigned)s, label);
    if (pws.mod[s].status == PW_STATUS_NOMEM) { o = s_cpy(msg, label); s_cpy(o, ": OUT OF MEMORY"); }
    else { o = s_cpy(msg, n1 != n0 ? "WIRED IN " : "ADDED "); s_cpy(o, label); }
    notice(msg);
    if (type == PW_T_PROLL) { if (pws.voices < PW_MAX_VOICES) pw_set_voices(PW_MAX_VOICES); else pw_auto_poly(); }   /* (chords need the voices) */
    ui.carrying = 0;
    if (ui.gview && pws.mod[s].used == 1) pw_set_group((unsigned)s, ui.gview);   /* added inside an open group: it joins it */
    if (!wired && ui.page == PG_ADD && !ui.page_from_patch) { ui.sel = s; ui.edit_jacks = 0; go(PG_EDIT); return; }   /* (unpatched, from a list that did not start at the patch screen: its own page) */
    go(PG_RACK);
    pv_goto(s, pw_types[type].n_out ? 1 : 0, 0);   /* on its output: the next one goes after it */
    ui.param_page[s] = 0; ui.page_type[s] = (uint8_t)(type + 1u);   /* (the cursor's jack picked a knob page: a new module starts on its first) */
}
static void add_open(void)   /* the add list, on the module that fits where the cursor is (or the last one added there) */
{
    ui.add_page = 0;
    int ctx = ui.page_from_patch ? wire_context() : 3, at = ui.add_cursor;
    if (ui.last_type[ctx]) {
        for (int i = 0; i < PW_T_SHOWN; ++i) if (add_order[i] == ui.last_type[ctx] - 1) at = i;
    } else if (ctx < 3) {
        unsigned want = ctx == 0 ? PW_C_FILTER : ctx == 1 ? PW_C_MOD : PW_C_SOURCE;
        for (int i = PW_T_SHOWN - 1; i >= 0; --i) if (pw_types[add_order[i]].cat == want) at = i;
    }
    ui.add_cursor = at;
    go(PG_ADD);
}
/* what to do with a patch just loaded */
static const char *load_hint(void)
{
    int beat = 0, keys = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        if (pws.mod[i].used != 1) continue;
        unsigned t = pws.mod[i].type;
        if (t == PW_T_SEQ || t == PW_T_TRIGSEQ || t == PW_T_CLOCK || t == PW_T_EUCLID) beat = 1;
        if (t == PW_T_KEYS || t == PW_T_ARP) keys = 1;
    }
    return beat ? "LOADED - PLAY STARTS IT" : keys ? "LOADED - PLAY THE KEYS" : "LOADED";
}
static void load_selected(void)
{
    ui.var_pend = -1;
    int i = ui.list_cursor;
    ui.last_list = i;
    if (i == 0) {
        pw_clear(); pw_set_playing(0); pw_random_name(pws.name);
        int x, y; free_spot(&x, &y); ui.sel = pw_add(PW_T_OUTPUT, x, y);
        ui.user_slot = -1; ui.carrying = 0;
        { char msg[40], *o = s_cpy(msg, "NEW: "); o = s_cpy(o, pws.name); s_cpy(o, " (FX ADDS)"); notice(msg); } go(PG_RACK);
    } else if (i == 1) {
        pw_random_patch(); ui.sel = first_module(); ui.user_slot = -1; ui.carrying = 0;
        { char msg[40], *o = s_cpy(msg, "GENERATED: "); s_cpy(o, pws.name); notice(msg); } go(PG_RACK);
    } else if (i < LIST_TOP + PW_FACTORY_COUNT) {
        pw_factory_build((unsigned)(i - LIST_TOP)); ui.sel = first_module(); ui.user_slot = -1; ui.carrying = 0;
        notice(load_hint()); go(PG_RACK);
    } else if (i < LIST_USER) {   /* a Group: its modules are added to the patch, wired, at a free spot */
        int sl = sub_add(i - LIST_TOP - PW_FACTORY_COUNT);
        if (sl < 0) notice("NO ROOM FOR THAT GROUP");
        else { ui.sel = sl; ui.carrying = 0; ui.gview = 0; notice("GROUP INSERTED (GLO+FX OPENS)"); go(PG_RACK); pv_layout(); pv_to_box(sl); }
    } else {
        int u = i - LIST_USER;
        if (pw_store_kind((unsigned)u)) {   /* saved as a GROUP: its modules go into the open patch, as one box */
            uint64_t set; pw_undo_group(1);
            int n = pw_store_insert((unsigned)u, &set), g = n > 0 ? grp_make((grp_set)set) : 0, first = -1;
            pw_undo_group(0);
            for (int k = 0; k < PW_MAX_MODULES && first < 0; ++k) if (set >> k & 1u) first = k;
            if (n <= 0 || first < 0) notice(n == 0 || first < 0 ? "NO ROOM FOR THAT GROUP" : "THAT SLOT IS EMPTY");
            else { ui.sel = first; ui.carrying = 0; ui.gview = 0; notice(g ? "GROUP INSERTED (GLO+FX OPENS)" : "INSERTED (TOO MANY GROUPS: NOT GROUPED)"); go(PG_RACK); pv_layout(); pv_to_box(first); }
            return;
        }
        int r = pw_store_load((unsigned)u);
        if (r < 0) notice("THAT SLOT IS EMPTY");
        else { ui.sel = first_module(); ui.user_slot = u; ui.carrying = 0; notice(r > 0 ? "LOADED (SOME MODULES DROPPED)" : load_hint()); go(PG_RACK); }
    }
}
/* SAVE anywhere: the patch list, on the slot to store in; SAVE there stores */
static void save_open(void)   /* the SAVE page: the cursor on the patch's own slot, else the first free one */
{
    int u = ui.user_slot;
    if (u < 0) for (int k = 0; k < PW_USER_SLOTS && u < 0; ++k) if (!pw_store_name((unsigned)k)) u = k;
    if (u < 0) u = 0;
    ui.save_cur = (int8_t)u; ui.save_group = 0;
    go(PG_SAVE);
}
static void save_store(void)   /* SEL on the SAVE page */
{
    char ask[44], *o;
    int u = ui.save_cur;
    int armed = ui.confirm_kind == 2 && ui.confirm_arg == u && (int32_t)(fm1_millis() - ui.confirm_until) < 0;
    if (pw_store_name((unsigned)u) && !(u == ui.user_slot && !ui.save_group) && !armed) {
        o = s_cpy(ask, "SEL AGAIN: OVERWRITE U"); s_int(o, u + 1);
        if (!confirm(2, u, ask)) return;
    }
    ui.confirm_kind = 0;
    pw_save_kind = ui.save_group;
    int r = pw_store_save((unsigned)u);
    pw_save_kind = 0;
    if (r == 0) {
        if (ui.save_group) { if (u == ui.user_slot) ui.user_slot = -1; }   /* (a group is not what the patch autosaves into) */
        else ui.user_slot = u;
        o = s_cpy(ask, ui.save_group ? "STORED AS A GROUP IN U" : "STORED IN U"); s_int(o, u + 1);
        notice(pw_ram_err ? "STORED: THE RECORDED SOUND DID NOT FIT" : ask);
    } else notice("SAVE FAILED");
}
/* SAVE: the menu's pages in turn (PATCHES, SAVE, SETTINGS, HELP); from anywhere else it opens the SAVE page */
static void save_pressed(void)
{
    switch (ui.page) {
    case PG_SAVE: ui.set_row = 0; ui.diag = 0; go(PG_SETTINGS); break;
    case PG_SETTINGS: if (!ui.diag) { ui.help_scroll = 0; go(PG_HELP); } break;
    case PG_HELP: go(PG_PATCHES); break;
    case PG_PATCHES: save_open(); break;
    default: save_open(); break;
    }
}
/* LIVE record: the step a key played now belongs to: the one that is playing, or the next once more than half of it has
 * passed (the transport's sixteenth phase: 0 just after a tick, 1 at the next). `cur` is the playhead, `len` the length. */
static int live_step(int cur, int len)
{
    if (len < 1) len = 1;
    if (cur < 0) cur = 0;
    return (cur + (pws.tick_phase > 0.5f ? 1 : 0)) % len;
}
/* the keyboard mode: the keys play notes (KEYBOARD) or are the BUTTON modules' (BUTTONS). A patch with BUTTON and no KEYS module is always in BUTTONS;
 * with both, GLO + HOME switches. Without a BUTTON module the keys are as they always were. */
static int button_mode_eff(void)
{
    int hb = 0, hk = 0;
    for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) { if (pws.mod[i].type == PW_T_BUTTON) hb = 1; else if (pws.mod[i].type == PW_T_KEYS) hk = 1; }
    return hb && (!hk || ui.kbd_btn);
}
static void kbd_sync(uint32_t keys)
{
    int e = button_mode_eff();
    if (e == ui.kbd_eff) return;
    ui.kbd_eff = (uint8_t)e;
    if (e) {   /* into BUTTONS: the notes held let go, and the keys held now do not count until pressed again */
        for (unsigned k = 0; k < 27; ++k) { if (ui.key_note[k]) pw_key(ui.key_note[k], 0); ui.key_note[k] = 0; }
        if (ui.mon_key) { pw_seq_monitor(ui.mon_slot, 0, 0); ui.mon_key = 0; }
        ui.btn_block = keys;
    } else pw_button_keys(0);   /* out of it: the gates fall, and held keys do not sound until pressed again */
    ui.keys = keys;
}
static void handle_keys(uint32_t keys, const uint8_t *vel, const uint8_t *press)
{
    kbd_sync(keys);
    if (ui.kbd_eff) { ui.btn_block &= keys; pw_button_keys(keys & ~ui.btn_block); ui.keys = keys; return; }
    {   /* the pressure: the hardest-pressed held key's */
        int pr = 0; for (unsigned k = 0; k < 27; ++k) if ((keys >> k & 1u) && press[k] > pr) pr = press[k];
        if (pr != ui.press_last) { ui.press_last = pr; pw_pressure(pr); }
    }
    uint32_t down = keys & ~ui.keys, up = ui.keys & ~keys;
    for (unsigned k = 0; k < 27; ++k) {
        uint32_t bit = 1u << k;
        if (up & bit) { if (ui.key_note[k]) pw_key(ui.key_note[k], 0); ui.key_note[k] = 0; if (ui.mon_key == k + 1u) { pw_seq_monitor(ui.mon_slot, 0, 0); ui.mon_key = 0; } }
        if (!(down & bit)) continue;
        int note = 53 + (int)k + ui.octave * 12;
        if (ui.page == PG_EDIT && ui.sel >= 0 && pws.mod[ui.sel].used == 1 && pws.mod[ui.sel].type == PW_T_TRIGSEQ) {
            int i = white_index(k), b = black_index(k);
            if (ui.trig_rec && i >= 0 && (ui.trig_rec == 1 || pws.playing)) {
                /* STEP record: the lower keys C D E F = tracks A B C D hit at the cursor, which moves on; LIVE: the same on the step
                 * the transport is at (it overdubs); a hard hit (or GLO) is an accent */
                int tr = i & 3, len = (int)(pws.mod[ui.sel].p[4 + tr] + 0.5f);
                int st = ui.trig_rec == 2 ? live_step((int)pw_live((unsigned)ui.sel, (unsigned)tr), len) : ui.trig_cur % (len > 0 ? len : 16);
                ui.trig_track = tr;
                pw_undo_data((unsigned)ui.sel);
                pw_trig_set((unsigned)ui.sel, (unsigned)tr, (unsigned)st, (vel[k] >= 110 || (ui.glo_on || (ui.buttons & BIT(B_GLO)))) ? 2 : 1);
                if (ui.glo_on || (ui.buttons & BIT(B_GLO))) ui.glo_used = 1;
                if (ui.trig_rec == 1) ui.trig_cur = (st + 1) % (len > 0 ? len : 16);
                pw_trig_monitor((unsigned)ui.sel, (unsigned)tr, vel[k] >= 110 || (ui.glo_on || (ui.buttons & BIT(B_GLO))));   /* MONITOR: the hit sounds as you enter it */
                continue;
            }
            if (ui.trig_rec) continue;   /* (recording: the other keys do nothing) */
            if (b >= 0 && b < 4) ui.trig_track = b;
            if (i >= 0) {
                int v = pw_trig_get((unsigned)ui.sel, (unsigned)ui.trig_track, (unsigned)i);
                if (ui.glo_on || (ui.buttons & BIT(B_GLO))) { v = v == 0 ? 1 : v == 3 ? 1 : v + 1; ui.glo_used = 1; }
                else v = v ? 0 : 1;
                pw_trig_set((unsigned)ui.sel, (unsigned)ui.trig_track, (unsigned)i, v);
            }
            continue;   /* the grid's keys do not sound */
        }
        if (ui.page == PG_LIVE) {   /* LIVE: the lower keys mute (SEL held: solo; REC held: on the next bar), GLO + one: MORPH's B */
            int wi = white_index(k);
            if (wi >= 0) live_key(wi, (ui.glo_on || (ui.buttons & BIT(B_GLO))) != 0, (ui.buttons & BIT(B_SEL)) != 0, (ui.buttons & BIT(B_REC)) != 0);
            if (ui.glo_on || (ui.buttons & BIT(B_GLO))) ui.glo_used = 1;
            continue;   /* (no notes from them here) */
        }
        if (ui.page == PG_RACK && (ui.glo_on || (ui.buttons & BIT(B_GLO))) && black_index(k) >= 0) {   /* GLO + OP1-4: favorite 1-4, added where the cursor is */
            unsigned f = ui.fav[black_index(k)];
            ui.glo_used = 1;
            if (!f) notice("NO FAVORITE HERE: REC STARS ONE IN THE ADD LIST");
            else { ui.page_from_patch = 1; add_type(f - 1u, 1); }
            continue;
        }
        /* variations: the first eight lower keys on the performance page (GLO: copy the one playing there);
         * GLO + one of them anywhere else */
        {
            int wi = white_index(k), trig_page = ui.page == PG_EDIT && ui.sel >= 0 && pws.mod[ui.sel].type == PW_T_TRIGSEQ;
            int glo_k = (ui.glo_on || (ui.buttons & BIT(B_GLO))) != 0;
            int sel_k = (ui.buttons & BIT(B_SEL)) != 0, rack_recall = ui.page == PG_RACK && sel_k && !glo_k;   /* (SEL + key on the patch screen: recall) */
            if (wi >= 0 && wi < 8 && !trig_page && (ui.page == PG_PERFORM || glo_k || rack_recall)) {
                char msg[32], *o;
                if (sel_k && !rack_recall) { wi += 8; ui.live_sel_used = 1; }   /* (SEL held: snapshots 9-16) */
                if (rack_recall) ui.rsel_used = 1;
                if (glo_k) {   /* GLO + key: save the snapshot playing into that key, on every page */
                    pw_var_copy(pws.var_cur, (unsigned)wi); pw_var_select((unsigned)wi);
                    o = s_cpy(msg, "SAVED TO SNAPSHOT "); s_int(o, wi + 1);
                } else if (ui.page == PG_PERFORM && ui.var_quant && pws.playing && (unsigned)wi != pws.var_cur) {
                    ui.var_pend = (int8_t)wi; ui.var_pend_bar = (int)(pws.sixteenths / 16u);
                    o = s_cpy(msg, "SNAPSHOT "); o = s_int(o, wi + 1); s_cpy(o, " ON THE NEXT BAR");
                } else { ui.var_pend = -1; pw_var_select((unsigned)wi); o = s_cpy(msg, "SNAPSHOT "); s_int(o, wi + 1); }
                if (glo_k) notice_glo(msg); else notice(msg);   /* (GLO's own hints cover a plain notice) */
                if (glo_k) ui.glo_used = 1;
                continue;
            }
        }
        if (ui.page == PG_SEQ) {
            int s = find_seq();
            int ty = s >= 0 ? pws.mod[s].type : -1;
            if (s >= 0 && ui.seq_rec && ty == PW_T_PROLL) {
                /* record into the roll: STEP puts the note at the cursor (keys together are a chord; a new press after a pause goes on a note's length); LIVE puts it on the
                 * step the loop is at now */
                pw_module *rm = &pws.mod[s];
                uint32_t nowm = fm1_millis();
                int loop = (int)pw_roll_loop(rm), snap = roll_snap(rm);
                pw_undo_data((unsigned)s);
                if (ui.seq_rec == 2 && pws.playing) ui.roll_t = (int)((pws.sixteenths + (unsigned)loop - 1u) % (unsigned)loop);
                else if (ui.roll_ms && nowm - ui.roll_ms > 250u) ui.roll_t = (ui.roll_t + ui.roll_len) % loop;
                ui.roll_ms = nowm; ui.roll_p = clampi(note, 0, 127);
                int old = pw_roll_find(rm, (unsigned)ui.roll_t, (unsigned)ui.roll_p);
                if (old >= 0) pw_roll_del(rm, (unsigned)old);
                if (pw_roll_add(rm, (unsigned)ui.roll_t, (unsigned)(ui.roll_len < snap ? snap : ui.roll_len), (unsigned)ui.roll_p, (unsigned)clampi(vel[k] ? vel[k] / 4 : 24, 0, 31)) < 0) notice("FOUR NOTES AT ONCE AT MOST");
                pws.revision++;
            } else if (s >= 0 && ui.seq_rec && ty != PW_T_MODSEQ) {
                /* record: the note goes on the cursor step (STEP; the cursor moves on) or on the nearest step of the loop (LIVE:
                 * it overdubs, only that step changes); the SEQUENCER's MONITOR sounds it through the sequencer as it is held */
                int steps = seq_len(s), live = ui.seq_rec == 2 && pws.playing && ty != PW_T_GRAVITY;
                int st = live ? live_step((int)pw_live((unsigned)s, 0), steps) : ui.seq_cursor % steps;
                pw_undo_data((unsigned)s);
                if (ty == PW_T_CHORDSTEPS) {
                    uint8_t old = pws.mod[s].data[st];
                    pws.mod[s].data[st] = (uint8_t)((old & 0xf0) | pw_chord_degree(&pws.mod[s], note - 60));
                } else {
                    pws.mod[s].data[st] = (uint8_t)(int8_t)clampi(note - 60, -36, 36);
                    if (ty == PW_T_SEQ) {
                        uint16_t g = (uint16_t)(pws.mod[s].data[16] | (pws.mod[s].data[17] << 8)) | (uint16_t)(1u << st);
                        pws.mod[s].data[16] = (uint8_t)g; pws.mod[s].data[17] = (uint8_t)(g >> 8);
                        pws.mod[s].data[PW_SEQ_STEPVEL + st] = (uint8_t)clampi(vel[k] ? vel[k] * 2 : 200, 20, 255);
                        if (pws.mod[s].p[6] > 0.5f) { pw_seq_monitor((unsigned)s, note - 60, 1); ui.mon_key = (uint8_t)(k + 1u); ui.mon_slot = (uint8_t)s; }
                    }
                }
                ui.seq_cursor = live ? st : (st + 1) % steps;
                if (live) ui.seq_cursor = st;
                pws.revision++;
            } else if (s >= 0 && ty != PW_T_PROLL && white_index(k) >= 0) {
                int i = white_index(k);
                if (ty == PW_T_SEQ) {
                    pw_undo_data((unsigned)s);
                    uint16_t g = (uint16_t)(pws.mod[s].data[16] | (pws.mod[s].data[17] << 8));
                    g ^= (uint16_t)(1u << i);
                    pws.mod[s].data[16] = (uint8_t)g; pws.mod[s].data[17] = (uint8_t)(g >> 8);
                    pws.revision++;
                }
                ui.seq_cursor = i;
                continue; /* step keys do not sound */
            }
        }
        if (note < 0 || note > 127) continue;
        if (ui.page == PG_EDIT && audition_solo(ui.sel)) {
            pw_audition((unsigned)ui.sel, note, (unsigned)k);   /* AUDITION: the key strikes this source on its own */
            continue;
        }
        ui.key_note[k] = (uint8_t)note;
        pw_key(note, vel[k] <= 0 ? 100 : vel[k]);
    }
    ui.keys = keys;
}
/* ARP + a knob: the knob goes on a MOD SEQ lane (one is added when the patch has none): every step gets its value now, and
 * while ARP is held and the song plays each step it passes takes the knob's value as you turn it (see lane_rec_tick). GLO too: let it go. */
static void lane_rec_knob(int slot, unsigned p, int detents, int fine, int glo)
{
    int ms; unsigned ln; char msg[48], lab[16], *o;
    ui.arp_used = 1;
    if (glo) {
        if (pw_mseq_lock_find((unsigned)slot, p, &ms, &ln)) { pw_mseq_lock_clear((unsigned)ms, ln); ui.lr_mseq = 0; notice("KNOB LET GO OF ITS LANE"); }
        else notice("THIS KNOB HAS NO LANE");
        return;
    }
    int r = pw_mseq_lock_assign((unsigned)slot, p, &ms, &ln);
    if (r < 0) { notice(r == -2 ? "MOD SEQ LANES ARE ALL IN USE" : "NO ROOM FOR A MOD SEQ"); return; }
    if (r == 0) {
        pw_module_label((unsigned)slot, lab);
        o = s_cpy(msg, lab); o = s_cpy(o, " "); o = s_cpy(o, pw_types[pws.mod[slot].type].param[p].name);
        o = s_cpy(o, " > MSEQ LANE "); *o++ = (char)('A' + ln); *o = 0;
        notice(msg);
    }
    pw_nudge_param((unsigned)slot, p, detents, fine);
    ui.lr_mseq = (int8_t)(ms + 1); ui.lr_lane = (uint8_t)ln; ui.lr_mod = (uint8_t)slot; ui.lr_par = (uint8_t)p; ui.lr_ms = fm1_millis();
    int pos = (int)pw_live((unsigned)ms, ln);
    if (pws.playing && pos >= 0) { pw_mseq_lock_write((unsigned)ms, ln, pos, 0); ui.lr_pos = (int8_t)pos; }
    else { pw_mseq_lock_write((unsigned)ms, ln, 0, 1); ui.lr_pos = -1; }
}
/* every frame: knobs take their lane's step; while ARP is held on a knob being recorded, each new step takes its value */
static void lane_rec_tick(const struct fm1_controls *c, uint32_t now)
{
    int sm = -1, sp = -1;
    if ((c->buttons & BIT(B_ARP)) && !(c->buttons & BIT(B_GLO)) && !ui.glo_on && ui.lr_mseq > 0 && now - ui.lr_ms < 3000u) {
        sm = ui.lr_mod; sp = ui.lr_par;
        if (pws.playing) {
            int pos = (int)pw_live((unsigned)ui.lr_mseq - 1u, ui.lr_lane);
            if (pos >= 0 && pos != ui.lr_pos) { pw_mseq_lock_write((unsigned)ui.lr_mseq - 1u, ui.lr_lane, pos, 0); ui.lr_pos = (int8_t)pos; }
        }
    }
    pw_mseq_follow(sm, sp);
    pw_snap_follow(now);
    pw_mutate_follow(now);
}
static void knobs_to_params(int slot, const struct fm1_controls *c, int fine)
{
    if (slot < 0 || pws.mod[slot].used != 1) return;
    fix_page(slot);
    for (unsigned k = 0; k < 4; ++k)
        if (c->encoders[E_K1 + k]) {
            unsigned pos = ui.param_page[slot] * 4 + k;
            if (pos >= pw_types[pws.mod[slot].type].n_param) continue;
            unsigned p = pw_param_at(pws.mod[slot].type, pos);
            if (c->buttons & BIT(B_ENV)) {              /* ENV (PERFORM) held: the knob goes on the performance page */
                int at = pw_perf_assign((unsigned)slot, p);
                char msg[40], lab[16]; pw_module_label((unsigned)slot, lab);
                if (at < 0) notice("PERFORM PAGE FULL (REC THERE CLEARS)");
                else { char *o = s_cpy(msg, "PERFORM "); o = s_int(o, at + 1); o = s_cpy(o, ": "); o = s_cpy(o, lab); o = s_cpy(o, " "); s_cpy(o, pw_types[pws.mod[slot].type].param[p].name); notice(msg); }
                ui.env_used = 1;
                continue;
            }
            if (c->buttons & BIT(B_ARP)) { lane_rec_knob(slot, p, c->encoders[E_K1 + k], fine, ui.glo_on || (c->buttons & BIT(B_GLO)) != 0); continue; }   /* ARP held: the knob goes on a MOD SEQ lane */
            pw_nudge_param((unsigned)slot, p, c->encoders[E_K1 + k], fine); song_record((unsigned)slot, p);
        }
}
static void turn_page(int slot, int d)
{
    fix_page(slot);
    unsigned pages = param_pages(slot);
    if (!d || !pages) return;
    ui.param_page[slot] = (unsigned)clampi((int)ui.param_page[slot] + d, 0, (int)pages - 1);
}
static void delete_now(int s, int wire)   /* (REC held: no question; GLO + OCT- brings it back; wire: GLO + REC held, the cables go through whatever the ON DELETE setting says) */
{
    if (s < 0 || s >= PW_MAX_MODULES || pws.mod[s].used != 1) return;
    char label[16], msg[44];
    pw_module_label((unsigned)s, label);
    int next = step_module(s, 1, 0);
    if (next == s) next = step_module(s, -1, 0);
    int gv = ui.page == PG_RACK ? ui.gview : 0;
    if (gv) {   /* inside an open group: stay in it, on a module of it */
        int in = -1;
        for (int i = 0; i < PW_MAX_MODULES; ++i) if (i != s && pws.mod[i].used == 1 && pws.mod[i].group == gv) { if (in < 0 || i > s) { in = i; if (i > s) break; } }
        if (in >= 0) next = in;
    }
    int bridged = (wire || ui.smart_i) ? pw_remove_wired((unsigned)s) : (pw_remove((unsigned)s), 0);
    ui.sel = next == s ? first_module() : next;
    ui.carrying = 0;
    if (gv && ui.page == PG_RACK && ui.sel >= 0) { pv_layout(); ui.cur_slot = ui.sel; ui.cur_out = 0; ui.cur_port = 0; pv_goto(ui.sel, 0, 0); }
    char *o = s_cpy(msg, "DELETED "); o = s_cpy(o, label); s_cpy(o, bridged > 0 ? ", WIRED THROUGH (GLO+OCT-)" : " (GLO+OCT- UNDOES)");
    notice(msg);
    if (ui.page == PG_RACK && ui.sel >= 0) pv_goto(ui.sel, 0, 0);
}
/* ------------------------------------------------------------ MATRIX (LFO): the patch as a list
 * Every module in signal-flow order with the cables leaving it under it. SELECT moves; on a module SEL starts a cable
 * from it (its output, then the module, then the input it goes to), REC deletes it; on a cable SEL sends it somewhere
 * else (the module, then the input), REC unplugs it, KNOB 1 sets how much of it arrives (-100..100 %: attenuated,
 * inverted); SEL held: that place on the patch screen; EDIT: the module page.
 * The add lists' EDIT replaces the module the list is on, keeping what cables it can. */
typedef struct { int16_t slot, cable; } lst_row;
static lst_row lrows[PW_MAX_MODULES * 2 + PW_MAX_CABLES];   /* (a group's header row: slot -1, cable -(10 + group)) */
#define LST_HDR(r) ((r)->cable <= -10)
static int lst_folded(int g) { (void)g; return 0; }   /* (groups always open in the matrix) */
static int lst_build(void)
{
    pv_layout_flat();
    int n = 0;
    uint8_t done[PW_MAX_GROUP + 1] = {0};
    if (pv_n) { lrows[n].slot = -1; lrows[n].cable = -2; ++n; }   /* the first row: + ADD CABLE */
    for (int k = 0; k < pv_n; ++k) {
        int s = pv_order[k], g = pws.mod[s].group;
        if (g && done[g]) continue;
        if (!g) {
            lrows[n].slot = (int16_t)s; lrows[n].cable = -1; ++n;
            for (unsigned c = 0; c < PW_MAX_CABLES; ++c)
                if (pws.cable[c].used && pws.cable[c].sm == s) { lrows[n].slot = (int16_t)s; lrows[n].cable = (int16_t)c; ++n; }
            continue;
        }
        done[g] = 1;   /* a group: a header, then its modules together (flow order) with their cables; folded, only the cables that leave it */
        for (int kk = k; kk < pv_n; ++kk) {
            int s2 = pv_order[kk];
            if (pws.mod[s2].group != g) continue;
            if (!lst_folded(g)) { lrows[n].slot = (int16_t)s2; lrows[n].cable = -1; ++n; }
            for (unsigned c = 0; c < PW_MAX_CABLES; ++c)
                if (pws.cable[c].used && pws.cable[c].sm == s2 && (!lst_folded(g) || pws.mod[pws.cable[c].dm].group != g)) { lrows[n].slot = (int16_t)s2; lrows[n].cable = (int16_t)c; ++n; }
        }
    }
    return n;
}
/* the picker's choices: 1 the source's outputs, 2 the modules with inputs, 3 the destination's inputs */
static int lst_choices(int *out)
{
    int n = 0;
    if (ui.lst_pick == 1) { for (unsigned o = 0; o < pw_types[pws.mod[ui.lst_sm].type].n_out; ++o) out[n++] = (int)o; }
    else if (ui.lst_pick == 2) { pv_layout_flat(); for (int k = 0; k < pv_n; ++k) if (pw_types[pws.mod[pv_order[k]].type].n_in) out[n++] = pv_order[k]; }
    else if (ui.lst_pick == 3) { for (unsigned i = 0; i < pw_types[pws.mod[ui.lst_dm].type].n_in; ++i) out[n++] = (int)i; }
    else if (ui.lst_pick == 4) { pv_layout_flat(); for (int k = 0; k < pv_n; ++k) if (pw_types[pws.mod[pv_order[k]].type].n_out) out[n++] = pv_order[k]; }
    return n;
}
static void draw_list(void)
{
    char a[28], b[28];
    if (ui.lst_pick) {
        int ch[PW_MAX_MODULES + 16], n = lst_choices(ch);
        ui.lst_pcur = clampi(ui.lst_pcur, 0, n ? n - 1 : 0);
        char *o;
        if (ui.lst_pick == 4) s_cpy(a, ui.lst_cable >= 0 ? "CABLE FROM WHICH MODULE?" : "NEW CABLE: FROM WHICH MODULE?");
        else if (ui.lst_pick == 1) { pw_module_label((unsigned)ui.lst_sm, b); o = s_cpy(a, "FROM "); o = s_cpy(o, b); s_cpy(o, ": WHICH OUTPUT?"); }
        else if (ui.lst_pick == 2) { port_label(ui.lst_sm, 1, ui.lst_so, b); o = s_cpy(a, b); s_cpy(o, " TO...?"); }
        else { pw_module_label((unsigned)ui.lst_dm, b); o = s_cpy(a, "INTO "); o = s_cpy(o, b); s_cpy(o, ": WHICH INPUT?"); }
        rfill(0, HDR + 2, W, 20, C_PANEL);
        text_fit(6, HDR + 5, a, F_S, C_AMBER, W - 12);
        const int lh = 17, rows = 9;
        int top = clampi(ui.lst_pcur - rows / 2, 0, n > rows ? n - rows : 0);
        for (int i = top, y = HDR + 27; i < n && i < top + rows; ++i, y += lh) {
            int sel = i == ui.lst_pcur;
            if (sel) rfill(0, y - 3, W, lh, C_PANEL2);
            if (ui.lst_pick == 2 || ui.lst_pick == 4) {
                fill(3, y, 3, 11, cat_color[pw_types[pws.mod[ch[i]].type].cat]);
                pw_module_label((unsigned)ch[i], a);
                int x = text(10, y, a, F_S, sel ? C_WHITE : C_TEXT);
                text_fit(x + 8, y + 1, pw_types[pws.mod[ch[i]].type].name, F_XS, C_DIM, W - x - 40);
                if (pws.mod[ch[i]].group) { char gt[4] = {'G', 0, 0, 0}; s_int(gt + 1, pws.mod[ch[i]].group); text_right(W - 6, y + 1, gt, F_XS, C_DIM); }
            } else {
                const pw_type *t = &pw_types[pws.mod[ui.lst_pick == 1 ? ui.lst_sm : ui.lst_dm].type];
                text(10, y, ui.lst_pick == 1 ? t->out[ch[i]] : t->in[ch[i]], F_S, sel ? C_WHITE : C_TEXT);
                if (ui.lst_pick == 3)   /* what already goes in there */
                    for (unsigned c = 0; c < PW_MAX_CABLES; ++c)
                        if (pws.cable[c].used && pws.cable[c].dm == ui.lst_dm && pws.cable[c].di == ch[i]) {
                            port_label(pws.cable[c].sm, 1, pws.cable[c].so, b); o = s_cpy(a, "< "); s_cpy(o, b);
                            text_fit(96, y + 1, a, F_XS, C_DIM, W - 100); break;
                        }
            }
        }
        strip_set(ui.lst_pick == 4 ? "SEL|Choose|REC|Cancel" : ui.lst_pick == 1 ? "SEL|Choose|REC|Another source" : "SEL|Choose|REC|Back");
        return;
    }
    int n = lst_build();
    if (!n) { text_center(W / 2, 80, "EMPTY PATCH", F_M, C_TEXT); strip_set("FX|Add a module"); return; }
    ui.lst_cursor = clampi(ui.lst_cursor, 0, n - 1);
    const int lh = 17, rows = 11;
    int top = clampi(ui.lst_cursor - rows / 2, 0, n > rows ? n - rows : 0);
    for (int i = top, y = HDR + 5; i < n && i < top + rows; ++i, y += lh) {
        const lst_row *r = &lrows[i];
        int sel = i == ui.lst_cursor;
        { int gg = r->cable == -2 ? 0 : r->slot >= 0 ? pws.mod[r->slot].group : 0;
          if (gg) {   /* a group's rows: a lighter ground (the first row says which group) */
              rfill(0, y - 3, W, lh, C_PANEL2);
              int first = i == 0 || lrows[i - 1].cable == -2 || lrows[i - 1].slot < 0 || pws.mod[lrows[i - 1].slot].group != gg;
              if (first) { char gt[4] = {'G', 0, 0, 0}; s_int(gt + 1, gg); text_right(W - 6, y + 1, gt, F_XS, C_DIM); }
          } }
        if (sel) rfill(0, y - 3, W, lh, C_PANEL2);
        if (r->cable == -2) { text(10, y, "+ ADD CABLE", F_S, sel ? C_WHITE : C_GREEN); continue; }
        if (LST_HDR(r)) {   /* a group: its header (SEL folds it) */
            int g = -10 - r->cable, cnt = 0, ext = 0;
            for (int q = 0; q < PW_MAX_MODULES; ++q) if (pws.mod[q].used == 1 && pws.mod[q].group == g) ++cnt;
            if (!sel) rfill(0, y - 3, W, lh, C_SPANEL);
            fill(0, y - 3, 3, lh, C_SPANEL2);
            char gt[24], *go_ = gt; *go_++ = 'G'; go_ = s_int(go_, g); go_ = s_cpy(go_, "  "); go_ = s_int(go_, cnt); s_cpy(go_, cnt == 1 ? " MODULE" : " MODULES");
            (void)ext;
            text(10, y, gt, F_S, sel ? C_WHITE : C_AMBER);
            text_right(W - 6, y + 1, lst_folded(g) ? "FOLDED" : "OPEN", F_XS, C_DIM);
            continue;
        }
        const pw_module *m = &pws.mod[r->slot];
        if (m->group) fill(0, y - 3, 3, lh, C_SPANEL2);   /* the group's bar down the left */
        if (r->cable < 0) {
            uint16_t cc = cat_color[pw_types[m->type].cat];
            fill(3, y, 3, 11, cc);
            pw_module_label((unsigned)r->slot, a);
            int x = text(10, y, a, F_S, sel ? C_WHITE : cc);
            text_fit(x + 8, y + 1, pw_types[m->type].name, F_XS, C_DIM, (m->flags & PW_F_BYPASS ? 196 : W - 6) - x - 8);
            if (m->flags & PW_F_BYPASS) { rfill(W - 34, y - 1, 30, 13, C_RED); text_center(W - 19, y, "BYP", F_XS, C_WHITE); }
        } else {
            const pw_cable *k = &pws.cable[r->cable];
            if (m->group && pws.mod[k->dm].group != m->group) fill(10, y + 4, 4, 4, C_AMBER);   /* leaves the group */
            int x = text(18, y, pw_types[m->type].out[k->so], F_S, sel ? C_WHITE : C_TEXT);
            x = text(x + 5, y, ">", F_S, C_LINE);
            port_label(k->dm, 0, k->di, b);
            int right = W - 4;
            {   /* the cable's depth: always shown; dim at 100% */
                char av[8], *ao = s_int(av, k->amt); s_cpy(ao, "%");
                int aw = text_w(av, F_XS);
                text(W - 4 - aw, y + 1, av, F_XS, k->amt == 100 ? C_LINE : k->amt < 0 ? C_RED : C_AMBER);
                int cx = W - 10 - aw - 14, bw = (k->amt < 0 ? -k->amt : k->amt) * 12 / 100;
                fill(cx - 12, y + 6, 25, 1, C_LINE);
                if (k->amt >= 0) fill(cx, y + 4, bw + 1, 4, k->amt == 100 ? C_LINE : C_AMBER); else fill(cx - bw, y + 4, bw + 1, 4, C_RED);
                right = cx - 16;
            }
            text_fit(x + 5, y, b, F_S, sel ? C_WHITE : cat_color[pw_types[pws.mod[k->dm].type].cat], right - x - 5);
        }
    }
    strip_set(LST_HDR(&lrows[ui.lst_cursor]) ? "SEL|Fold or open the group" : lrows[ui.lst_cursor].cable == -2 ? "SEL|Patch a new cable" : lrows[ui.lst_cursor].cable < 0 ? "SEL|Cable|REC|Delete|EDIT|Open" : "SEL|Change|REC|Unplug|K1|Depth");
}
static void lst_finish(void)
{
    char a[24], b[24], msg[48], *o;
    int amt = 100;
    pw_undo_group(1);
    if (ui.lst_cable >= 0 && pws.cable[ui.lst_cable].used) { amt = pws.cable[ui.lst_cable].amt; pw_disconnect((unsigned)ui.lst_cable); }
    int r = pw_connect((unsigned)ui.lst_sm, (unsigned)ui.lst_so, (unsigned)ui.lst_dm, (unsigned)ui.lst_di);
    if (r >= 0 && amt != 100) pw_cable_amount((unsigned)r, amt);   /* (a moved cable keeps its amount) */
    pw_undo_group(0);
    port_label(ui.lst_sm, 1, ui.lst_so, a); port_label(ui.lst_dm, 0, ui.lst_di, b);
    o = s_cpy(msg, a); o = s_cpy(o, " > "); s_cpy(o, b);
    notice(r >= 0 ? msg : "NO CABLES LEFT");
    ui.lst_pick = 0;
}
static void lst_short(void)   /* SEL let go before it was held */
{
    if (ui.lst_pick) {
        int ch[PW_MAX_MODULES + 16], n = lst_choices(ch);
        if (!n) return;
        int v = ch[clampi(ui.lst_pcur, 0, n - 1)];
        if (ui.lst_pick == 4) {   /* the source module: then its output (a moved cable: on the one it has now) */
            ui.lst_pick = 1; ui.lst_pcur = 0;
            if (ui.lst_cable >= 0 && pws.cable[ui.lst_cable].used && pws.cable[ui.lst_cable].sm == v) ui.lst_pcur = pws.cable[ui.lst_cable].so;
            ui.lst_sm = v;
        }
        else if (ui.lst_pick == 1) { ui.lst_so = v; ui.lst_pick = 2; ui.lst_pcur = 0;
            if (ui.lst_cable >= 0 && pws.cable[ui.lst_cable].used) { int ch2[PW_MAX_MODULES + 16], m2 = lst_choices(ch2); for (int i = 0; i < m2; ++i) if (ch2[i] == pws.cable[ui.lst_cable].dm) ui.lst_pcur = i; } }
        else if (ui.lst_pick == 2) { ui.lst_dm = v; ui.lst_pick = 3; ui.lst_pcur = 0; }
        else { ui.lst_di = v; lst_finish(); }
        return;
    }
    int n = lst_build();
    if (!n) return;
    const lst_row *r = &lrows[clampi(ui.lst_cursor, 0, n - 1)];
    if (LST_HDR(r)) { int g = -10 - r->cable; ui.lst_coll[g >> 3] ^= (uint8_t)(1u << (g & 7)); return; }   /* a group header: fold or open */
    if (r->cable == -2) { ui.lst_cable = -1; ui.lst_sm = pv_order[0]; ui.lst_pick = 4; ui.lst_pcur = 0; return; }   /* + ADD CABLE: from which module */
    if (r->cable < 0) {
        if (!pw_types[pws.mod[r->slot].type].n_out) { notice("NO OUTPUTS: START FROM ITS SOURCE"); return; }
        ui.lst_sm = r->slot; ui.lst_cable = -1; ui.lst_pick = 1; ui.lst_pcur = 0;
    } else {
        const pw_cable *k = &pws.cable[r->cable];
        ui.lst_sm = k->sm; ui.lst_so = k->so; ui.lst_cable = r->cable; ui.lst_pick = 2; ui.lst_pcur = 0;
        int ch[PW_MAX_MODULES + 16], m = lst_choices(ch);
        for (int i = 0; i < m; ++i) if (ch[i] == k->dm) ui.lst_pcur = i;   /* starts on where it goes now */
    }
}
static void lst_long(void)    /* SEL held: that place on the patch screen */
{
    int n = lst_build();
    if (!n || ui.lst_pick) return;
    const lst_row *r = &lrows[clampi(ui.lst_cursor, 0, n - 1)];
    if (r->cable == -2 || LST_HDR(r)) return;
    go(PG_RACK); ui.carrying = 0;
    if (r->cable < 0) pv_goto(r->slot, 0, 0); else pv_goto(pws.cable[r->cable].dm, 0, pws.cable[r->cable].di);
}
static int lst_slot(void)     /* the module the list's cursor is on (a cable: its source) */
{
    int n = lst_build();
    return n ? lrows[clampi(ui.lst_cursor, 0, n - 1)].slot : -1;
}

/* ------------------------------------------------------------ replacing a module, its cables kept */
static int s_eq(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static int map_port(const char *const *names, unsigned n_old, unsigned old, const char *const *nn, unsigned n_new)
{
    (void)n_old;
    for (unsigned i = 0; i < n_new; ++i) if (s_eq(names[old], nn[i])) return (int)i;
    return old < n_new ? (int)old : -1;
}
static void replace_module(int slot, unsigned type)
{
    if (slot < 0 || pws.mod[slot].used != 1) return;
    const pw_type *to = &pw_types[pws.mod[slot].type], *tn = &pw_types[type];
    pw_cable keep[PW_MAX_CABLES]; int nk = 0, kept = 0;
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) if (pws.cable[c].used && (pws.cable[c].sm == slot || pws.cable[c].dm == slot)) keep[nk++] = pws.cable[c];
    int x = pws.mod[slot].x, y = pws.mod[slot].y;
    char was[16], now_[16], msg[48], *o;
    pw_module_label((unsigned)slot, was);
    pw_undo_group(1);
    pw_remove((unsigned)slot);
    int s = pw_add(type, x, y);
    if (s >= 0) for (int i = 0; i < nk; ++i) {
        pw_cable k = keep[i];
        int sm = k.sm, so = k.so, dm = k.dm, di = k.di;
        if (k.sm == slot) { sm = s; so = map_port(to->out, to->n_out, k.so, tn->out, tn->n_out); }
        if (k.dm == slot) { dm = s; di = map_port(to->in, to->n_in, k.di, tn->in, tn->n_in); }
        if (so >= 0 && di >= 0 && pw_connect((unsigned)sm, (unsigned)so, (unsigned)dm, (unsigned)di) >= 0) ++kept;
    }
    pw_undo_group(0);
    if (s < 0) { pw_undo(); notice("PATCH FULL"); return; }
    pw_module_label((unsigned)s, now_);
    o = s_cpy(msg, was); o = s_cpy(o, " > "); o = s_cpy(o, now_); o = s_cpy(o, ": "); o = s_int(o, kept); o = s_cpy(o, " OF "); o = s_int(o, nk); s_cpy(o, " CABLES");
    notice(msg);
    ui.sel = s; ui.param_page[s] = 0; ui.carrying = 0;
    if (ui.page_from_patch || ui.page == PG_RACK) { go(PG_RACK); pv_goto(s, 0, 0); }
    else if (ui.prev_page == PG_EDIT) { ui.edit_jacks = 0; go(PG_EDIT); }   /* (asked for from a module page: back on that page, now the new module's) */
    else go(ui.prev_page == PG_CABLES ? PG_CABLES : PG_RACK);
}
static int replace_target(void)
{
    int s = (ui.page == PG_RACK || ui.page_from_patch) ? (pv_n ? ui.cur_slot : -1) : valid_sel(0);
    return s >= 0 && pws.mod[s].used == 1 ? s : -1;
}

/* ------------------------------------------------------------ MUTES (ENV, page 2): mutes and morph
 * The patch's voices (sounds, sequencers, effects; up to 16, eight a page, SELECT turns the page): a lower key mutes or
 * unmutes one (its module bypassed: a muted sequencer's notes stop, tails ring on); with SEL held it solos it; with REC
 * held the change waits for the next bar. KNOB 1 is MORPH: every knob between the snapshot playing and B (GLO + a lower
 * key picks B). SEL turns Bar Sync on and off (mutes and snapshot changes then wait for the bar line). */
static int live_pages(int n) { return n > 8 ? (n - 1) / 8 + 1 : 1; }
static int live_voices(int *v)   /* a group is one voice (an instrument): its first sound, sequencer or effect stands for it */
{
    pv_layout_flat();
    int n = 0;
    unsigned long long seen = 0;
    for (int k = 0; k < pv_n && n < 16; ++k) {
        int s = pv_order[k]; unsigned t = pws.mod[s].type, c = pw_types[t].cat;
        if (!(c == PW_C_SOURCE || c == PW_C_FX || (c == PW_C_SEQ && t != PW_T_CLOCK && t != PW_T_QUANT))) continue;
        if (grp_size(s) > 1) { if (seen >> pws.mod[s].group & 1ull) continue; seen |= 1ull << pws.mod[s].group; }
        v[n++] = s;
    }
    return n;
}
static void live_apply(int slot, unsigned bypass)   /* bypass or play a voice: a group, all of its modules */
{
    unsigned g = grp_size(slot) > 1 ? pws.mod[slot].group : 0;
    for (int i = 0; i < PW_MAX_MODULES; ++i) {
        if (pws.mod[i].used != 1 || !(i == slot || (g && pws.mod[i].group == g))) continue;
        if (((pws.mod[i].flags & PW_F_BYPASS) != 0) != (bypass != 0)) pw_set_flags((unsigned)i, (pws.mod[i].flags & ~PW_F_BYPASS) | (bypass ? PW_F_BYPASS : 0u));
    }
}
static void live_toggle(int slot) { live_apply(slot, (pws.mod[slot].flags & PW_F_BYPASS) ? 0u : PW_F_BYPASS); }
static void live_solo(int slot)
{
    int v[16], n = live_voices(v);
    if (ui.solo_slot == slot) {                        /* again: back as it was */
        for (int i = 0; i < n; ++i) {
            unsigned want = (ui.solo_prev >> v[i]) & 1u ? PW_F_BYPASS : 0;
            live_apply(v[i], want);
        }
        ui.solo_slot = -1; notice("SOLO OFF");
        return;
    }
    if (ui.solo_slot < 0) { ui.solo_prev = 0; for (int i = 0; i < n; ++i) if (pws.mod[v[i]].flags & PW_F_BYPASS) ui.solo_prev |= 1ull << v[i]; }
    for (int i = 0; i < n; ++i) {
        unsigned want = v[i] == slot ? 0 : PW_F_BYPASS;
        live_apply(v[i], want);
    }
    ui.solo_slot = slot;
    char lab[16], msg[32], *o; pw_module_label((unsigned)slot, lab); o = s_cpy(msg, "SOLO "); s_cpy(o, lab); notice(msg);
}
static void live_queue_run(void)   /* every frame: the queued mutes land on the bar line */
{
    if (!ui.live_queue) return;
    if (pws.playing && (int)(pws.sixteenths / 16u) <= ui.live_qbar) return;
    for (int s = 0; s < PW_MAX_MODULES; ++s) if ((ui.live_queue >> s) & 1u) { if (pws.mod[s].used == 1) live_toggle(s); }
    ui.live_queue = 0;
}
static void live_key(int wi, int glo_k, int sel_held, int rec_held)
{
    if (glo_k) {
        if (wi < 8) { if (sel_held) { wi += 8; ui.live_sel_used = 1; } ui.morph_b = wi; char msg[24], *o = s_cpy(msg, "MORPH TO S"); s_int(o, wi + 1); notice(msg); }
        return;
    }
    int v[16], n = live_voices(v), i = clampi(ui.live_row, 0, live_pages(n) - 1) * 8 + wi;
    if (wi >= 8 || i >= n) return;
    if (sel_held) { live_solo(v[i]); ui.live_sel_used = 1; }
    else if (rec_held != (ui.var_quant != 0 && pws.playing)) { ui.live_queue ^= 1ull << v[i]; ui.live_qbar = (int)(pws.sixteenths / 16u); ui.live_rec_used = 1; }
    else live_toggle(v[i]);
}
static void draw_live(void)
{
    int v[16], n = live_voices(v);
    static const char *const keyname[8] = {"C", "D", "E", "F", "G", "A", "B", "C"};
    int pages = live_pages(n);
    ui.live_row = clampi(ui.live_row, 0, pages - 1);
    int page = ui.live_row;
    if (!n) { text_center(W / 2, 70, "NO VOICES YET", F_M, C_TEXT); text_center(W / 2, 98, "sounds, sequencers and effects", F_S, C_DIM); }
    for (int k = 0; k < 8; ++k) {
        int i = page * 8 + k; if (i >= n) break;
        int s = v[i], x = 2 + (k % 4) * 59, y = HDR + 3 + (k / 4) * 62;
        const pw_module *m = &pws.mod[s];
        int muted = (m->flags & PW_F_BYPASS) != 0, queued = (int)((ui.live_queue >> s) & 1u);
        uint16_t cc = cat_color[pw_types[m->type].cat];
        rfill(x, y, 56, 58, C_PANEL);
        fill(x, y, 4, 58, muted ? dim_of(cc) : cc);
        text(x + 8, y + 3, keyname[k], F_XS, muted ? C_LINE : C_DIM);
        char lab[20]; pw_module_label((unsigned)s, lab);
        if (grp_size(s) > 1) { char *o = lab; while (*o) ++o; *o++ = '+'; s_int(o, grp_size(s) - 1); }   /* (a group: "VCO 1+4") */
        text_fit(x + 8, y + 18, lab, F_S, muted ? C_LINE : C_WHITE, 44);
        const char *st = queued ? "NEXT BAR" : ui.solo_slot == s ? "SOLO" : muted ? "MUTE" : "ON";
        text(x + 8, y + 40, st, F_XS, queued || ui.solo_slot == s ? C_AMBER : muted ? C_RED : C_GREEN);
    }
    if (pages > 1) { char pg[8], *o = s_int(pg, page + 1); o = s_cpy(o, "/"); s_int(o, pages); text_right(W - 4, HDR + 127, pg, F_XS, C_DIM); }
    /* MORPH: the snapshot playing (A) to the one picked (B) */
    int y = 156;
    rfill(2, y, W - 4, 64, C_PANEL);
    text(8, y + 4, "MORPH", F_XS, C_DIM);
    strip_chg = 1;
    strip_set("KEYS|Mutes|K1|Morph|SEL|Bar Sync\x01");
    char va[6] = "S", vb[6] = "S"; s_int(va + 1, pws.var_cur + 1); s_int(vb + 1, ui.morph_b + 1);
    text(8, y + 24, va, F_M, C_AMBER);
    text_right(W - 8, y + 24, vb, F_M, cat_color[PW_C_FILTER]);
    int bx0 = 44, bx1 = W - 46, by = y + 44;
    for (int xx = bx0; xx < bx1; xx += 2) fill(xx, by - 1, 2, 3, blend(C_AMBER, cat_color[PW_C_FILTER], (unsigned)((xx - bx0) * 16 / (bx1 - bx0))));
    int px = bx0 + (int)(ui.morph * (float)(bx1 - bx0));
    fill(px - 2, by - 8, 5, 17, C_WHITE);
    { char pc[8], *o = s_int(pc, (int)(ui.morph * 100.0f + 0.5f)); s_cpy(o, "%"); text_center(W / 2, y + 22, pc, F_M, C_TEXT); }
    text_right(W - 8, y + 4, "K1", F_XS, C_DIM);
}

/* ---------------------------------------------------------------- MUTATE
 * The third PERFORM page. A list of the modules MUTATE can change (the first row: all of them at once); SEL puts one in or out (a locked module is left
 * alone: its knobs and its steps stay as they are). FX nudges, REC rolls, by AMOUNT (K1) of the knobs and steps DENSITY (K2) picks. GLO + OCT- undoes
 * either (each is one undo step). MUTATE modules in the patch do the same from their jacks, with the same locks. */
static int mut_slots(int *sl)   /* the modules the page lists, in slot order */
{
    int n = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pw_mut_eligible(i)) sl[n++] = (int)i;
    return n;
}
static void mutate_go(int roll)
{
    int changed = pw_mutate(roll, ui.mut_amt, ui.mut_den);
    ui.mut_flash = (uint8_t)(roll ? 2 : 1); ui.mut_ms = fm1_millis(); ui.mut_last = changed;
    if (!changed) notice("NOTHING TO CHANGE: ALL LOCKED, OR DENSITY IS 0");
    else { char msg[40], *o = s_int(msg, changed); s_cpy(o, roll ? " ROLLED (GLO+OCT- UNDOES)" : " NUDGED (GLO+OCT- UNDOES)"); notice(msg); }
}
static void draw_mutate(void)
{
    ky_off = 11;
    header_draw();
    int sl[PW_MAX_MODULES], n = mut_slots(sl), total = n + 1;
    ui.mut_row = (uint8_t)clampi((int)ui.mut_row, 0, n);
    const int lh = 17, rows = 6;
    int top = clampi((int)ui.mut_row - rows / 2, 0, total > rows ? total - rows : 0);
    int locked = 0;
    for (int i = 0; i < n; ++i) locked += (int)((pws.mut_lock >> sl[i]) & 1u);
    for (int r = top, y = HDR + 5; r < total && r < top + rows; ++r, y += lh) {
        int sel = r == (int)ui.mut_row;
        if (sel) rfill(0, y - 3, W - 8, lh, C_PANEL2);
        char lab[20], cnt[12];
        int lk;
        uint16_t bar;
        if (r == 0) {
            text(10, y, n ? "ALL MODULES" : "NO MODULES TO MUTATE", F_S, C_WHITE);
            char *o = s_int(cnt, n - locked); o = s_cpy(o, " OF "); s_int(o, n);
            lk = n > 0 && locked == n; bar = C_LINE;
            if (n) text_right(W - 14, y, cnt, F_S, locked ? C_AMBER : C_DIM);
        } else {
            int s = sl[r - 1];
            lk = (int)((pws.mut_lock >> s) & 1u);
            bar = cat_color[pw_types[pws.mod[s].type].cat];
            pw_module_label((unsigned)s, lab);
            if (grp_size(s) > 1) { char *o = lab; while (*o) ++o; *o++ = '+'; s_int(o, grp_size(s) - 1); }
            text_fit(10, y, lab, F_S, lk ? C_LINE : C_WHITE, 110);
            const char *mk = pws.mod[s].type == PW_T_SEQ || pws.mod[s].type == PW_T_MODSEQ ? "STEPS" : "";
            if (mk[0] && !lk) text_right(W - 66, y + 1, mk, F_XS, C_DIM);
            text_right(W - 14, y, lk ? "LOCKED" : "ON", F_S, lk ? C_AMBER : C_DIM);
        }
        fill(3, y, 3, 11, r == 0 ? C_LINE : bar);
    }
    {   /* where the cursor is in the list */
        int tr = rows * lh - 4, bar = total > rows ? tr * rows / total : tr, at = total > rows ? (tr - bar) * top / (total - rows) : 0;
        fill(W - 7, HDR + 4, 3, tr, C_PANEL);
        fill(W - 7, HDR + 4 + at, 3, bar < 3 ? 3 : bar, C_DIM);
    }
    /* the two actions */
    uint32_t now = fm1_millis();
    int fl = ui.mut_flash && now - ui.mut_ms < 250u ? ui.mut_flash : 0;
    for (int k = 0; k < 2; ++k) {
        int x = 2 + k * 118;
        rfill(x, 130, 116, 38, fl == k + 1 ? C_PANEL2 : C_PANEL);
        fill(x, 130, 4, 38, fl == k + 1 ? C_AMBER : cat_color[k ? PW_C_FX : PW_C_FILTER]);
        text(x + 10, 133, k ? "ROLL" : "NUDGE", F_M, C_WHITE);
        text(x + 10, 154, k ? "REC: new values" : "FX: a little off", F_XS, C_DIM);
    }
    char v[12], *o = s_int(v, (int)(ui.mut_amt * 100.0f + 0.5f)); s_cpy(o, "%");
    knob_cell(0, "AMOUNT", v, ui.mut_amt, C_AMBER);
    o = s_int(v, (int)(ui.mut_den * 100.0f + 0.5f)); s_cpy(o, "%");
    knob_cell(1, "DENSITY", v, ui.mut_den, cat_color[PW_C_MOD]);
    knob_cell(2, "LAST", ui.mut_last < 0 ? "-" : (s_int(v, ui.mut_last), v), -1, 0);
    knob_cell(3, "LOCKED", (s_int(v, locked), v), -1, 0);
    strip_set("SEL|Lock|FX|Nudge|REC|Roll\nK1|Amount|K2|Density|GLO+OCT|Undo");
}

/* ---------------------------------------------------------------- SONG
 * A grid of bars. The SNAP lane holds snapshot clips: the patch plays that snapshot while one covers the bar and glides
 * into the next over GLIDE bars (or, a patch clip, loads a saved patch). Below it, the AUTOMATION LANES: one knob each,
 * breakpoints you place and shape, drawn as a graph; a lane's knob is chosen from a list (or by recording: REC arms it,
 * then turn knobs while it plays). It plays with the transport and loops at the end of the last clip or point. */
static const uint8_t song_step[3] = {4, 2, 1};       /* the cursor's step on an automation lane, in sixteenths of a bar */
static int song_len(void)
{
    int e = 0;
    for (unsigned k = 0; k < PW_SONG_CLIPS; ++k) { const pw_clip *c = &pws.song.clip[k]; if (c->len && c->start + c->len > e) e = c->start + c->len; }
    for (unsigned l = 0, i = 0; l < pws.song.n_lanes; ++l)
        for (unsigned q = 0; q < pws.song.al[l].n; ++q, ++i) { int b = ((pws.song.pt_t[i] & 0xfffu) + 15) / 16; if (b > e) e = b; }
    return e;
}
static int song_at(int bar)
{
    for (unsigned k = 0; k < PW_SONG_CLIPS; ++k) { const pw_clip *c = &pws.song.clip[k]; if (c->len && !c->lane && bar >= c->start && bar < c->start + c->len) return (int)k; }
    return -1;
}
static int song_clash(int start, int len, int skip)
{
    for (int k = 0; k < PW_SONG_CLIPS; ++k) {
        const pw_clip *c = &pws.song.clip[k];
        if (k != skip && c->len && !c->lane && start < c->start + c->len && c->start < start + len) return 1;
    }
    return 0;
}
static int song_edge(int bar, int dir)   /* the next clip edge from bar; none: 8 bars on */
{
    int best = dir > 0 ? 255 : -1, found = 0;
    for (unsigned k = 0; k < PW_SONG_CLIPS; ++k) {
        const pw_clip *c = &pws.song.clip[k];
        if (!c->len || c->lane) continue;
        for (int e = 0; e < 2; ++e) {
            int b = e ? c->start + c->len : c->start;
            if (dir > 0 ? (b > bar && b < best) : (b < bar && b > best)) { best = b; found = 1; }
        }
    }
    return found ? best : clampi(bar + dir * 8, -1, 254);
}
static int lane_pt_at(int lane, int t)   /* the point of an automation lane at time t (its index in the lane), or -1 */
{
    if (lane < 0 || lane >= pws.song.n_lanes) return -1;
    unsigned s = pw_lane_start((unsigned)lane);
    for (unsigned i = 0; i < pws.song.al[lane].n; ++i) if ((int)(pws.song.pt_t[s + i] & 0xfffu) == t) return (int)i;
    return -1;
}
static int lane_edge(int lane, int t, int dir)   /* the next point's time (lane's own), or a bar on */
{
    int best = dir > 0 ? 4095 : -1, found = 0;
    if (lane >= 0 && lane < pws.song.n_lanes) {
        unsigned s = pw_lane_start((unsigned)lane);
        for (unsigned i = 0; i < pws.song.al[lane].n; ++i) {
            int pt = (int)(pws.song.pt_t[s + i] & 0xfffu);
            if (dir > 0 ? (pt > t && pt < best) : (pt < t && pt > best)) { best = pt; found = 1; }
        }
    }
    return found ? best : clampi(t + dir * 16, 0, 4079);
}
/* the knob list: every module's knobs in page order; entry numbers run through them */
static int pick_count(void)
{
    int n = 0;
    for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) n += pw_types[pws.mod[i].type].n_param;
    return n;
}
static int pick_entry(int e, int *mod, int *par)   /* entry e's module and knob; 0 when past the end */
{
    for (int i = 0; i < PW_MAX_MODULES; ++i) {
        if (pws.mod[i].used != 1) continue;
        int np = pw_types[pws.mod[i].type].n_param;
        if (e < np) { *mod = i; *par = (int)pw_param_at(pws.mod[i].type, (unsigned)e); return 1; }
        e -= np;
    }
    return 0;
}
static int pick_of(int mod, int par)   /* the entry of a knob, or 0 */
{
    int e = 0;
    for (int i = 0; i < PW_MAX_MODULES; ++i) {
        if (pws.mod[i].used != 1) continue;
        int np = pw_types[pws.mod[i].type].n_param;
        if (i == mod) { for (int q = 0; q < np; ++q) if ((int)pw_param_at(pws.mod[i].type, (unsigned)q) == par) return e + q; return e; }
        e += np;
    }
    return 0;
}
static int pick_module_edge(int e, int dir)   /* the first knob of the next (previous) module */
{
    int mod = 0, par = 0, m0 = 0;
    if (!pick_entry(e, &mod, &par)) return e;
    for (int i = 0; i < mod; ++i) if (pws.mod[i].used == 1) m0 += pw_types[pws.mod[i].type].n_param;
    if (dir > 0) return clampi(m0 + (int)pw_types[pws.mod[mod].type].n_param, 0, pick_count() - 1);
    return e > m0 ? m0 : clampi(m0 - 1, 0, pick_count() - 1) - 0;
}
static void song_chain_load(unsigned slot, int clip)   /* a patch clip: the saved patch plays on (the song stays, the beat goes on from the bar) */
{
    pw_song keep = pws.song;
    uint32_t s16 = pws.sixteenths;
    ui.song_loading = 1;
    int r = pw_store_load(slot);
    pws.song = keep;
    ui.song_chain = (uint8_t)(clip + 1); ui.song_s16 = 0xffffffffu;
    if (r < 0) { ui.song_loading = 0; notice("SONG: THAT PATCH SLOT IS EMPTY"); return; }
    pws.start_s16 = (s16 / 16u) * 16u;
    pw_set_playing(1);
}
static void song_tick(void)   /* every control tick while the song is on */
{
    pw_song *sg = &pws.song;
    uint32_t s16 = pws.sixteenths, now = fm1_millis();
    if (!sg->on || !pws.playing) { ui.song_s16 = 0xffffffffu; return; }
    int len = song_len();
    if (!len) return;
    int bar = (int)((s16 / 16u) % (unsigned)len), f = (int)(s16 % 16u);
    if (s16 != ui.song_s16) {
        ui.song_s16 = s16; ui.song_t0 = now;
        int c = song_at(bar);
        if (c >= 0) {
            const pw_clip *k = &sg->clip[c];
            if (k->val & 0x80u) { if (ui.song_chain != c + 1) song_chain_load(k->val & 7u, c); }
            else {
                unsigned v = k->val & 15u;
                int pk = k->start > 0 ? song_at(k->start - 1) : -1;
                ui.song_chain = 0;
                float e = (float)(bar - k->start) + (float)f * (1.0f / 16.0f), g = (float)sg->glide;
                if (g > 0 && pk >= 0 && !(sg->clip[pk].val & 0x80u) && (sg->clip[pk].val & 15u) != v && e < g) {   /* gliding: the old snapshot morphs into this one */
                    unsigned a = sg->clip[pk].val & 15u;
                    if (pws.var_cur != a) pw_var_select(a);
                    pw_var_morph(a, v, e / g);
                } else {
                    if (pws.var_cur != v) pw_var_select(v);
                    if (pws.morphing) pw_var_morph(v, v, 0.0f);
                }
            }
        }
    }
    /* the lanes, smoothly between the sixteenths */
    float frac = (float)(now - ui.song_t0) * pws.bpm * (1.0f / 15000.0f);
    pw_lane_skip = ui.song_rec && ui.rec_touch_lane >= 0 && now - ui.rec_touch_ms < 600u ? (uint8_t)(1u << ui.rec_touch_lane) : 0;   /* REC: the lane you are turning is not played back over your hand */
    pw_lane_apply((float)(s16 % (16u * (unsigned)len)) + (frac > 1.0f ? 1.0f : frac));
}
/* REC armed: a knob turned while the song plays lays points on its lane (the nudge sites call this) */
static void song_record(unsigned mod, unsigned par)
{
    pw_song *sg = &pws.song;
    ui.last_mod = (uint8_t)mod; ui.last_par = (uint8_t)par;
    { int tl = pw_lane_find(mod, par); if (tl >= 0 && ui.song_rec) { ui.rec_touch_lane = (int8_t)tl; ui.rec_touch_ms = fm1_millis(); } }   /* (the knob is being held: its lane lets go of it for a moment) */
    if (!ui.song_rec || !pws.playing || !sg->on || mod >= PW_MAX_MODULES || pws.mod[mod].used != 1) return;
    int len = song_len(), l = pw_lane_find(mod, par);
    unsigned t = len ? (unsigned)(pws.sixteenths % (16u * (unsigned)len)) : (unsigned)(pws.sixteenths % 4000u);
    if (l < 0) { l = pw_lane_add(mod, par); if (l < 0) { notice("ALL 8 LANES USED"); return; } ui.rec_t = 0xffff; }
    unsigned q = (unsigned)(pw_lane_norm(mod, par, pws.mod[mod].p[par]) * 255.0f + 0.5f);
    if (ui.rec_lane == l && ui.rec_t != 0xffff && t >= ui.rec_t && t - ui.rec_t < 2 && (int)q - (int)ui.rec_q < 24 && (int)ui.rec_q - (int)q < 24) return;   /* (thinned: a point every other sixteenth, sooner for a jump) */
    if (ui.rec_lane == l && ui.rec_t != 0xffff && t > ui.rec_t + 1u)   /* recording over a lane: what was there between the last point and this one goes */
        for (unsigned tt = ui.rec_t + 1u; tt < t; ++tt) { int pi = lane_pt_at(l, (int)tt); if (pi >= 0) pw_lane_del_pt((unsigned)l, (unsigned)pi); }
    if (pw_lane_set((unsigned)l, t, q, 0) < 0) { notice("POINT POOL FULL"); return; }
    ui.rec_lane = (int8_t)l; ui.rec_t = (uint16_t)t; ui.rec_q = (uint8_t)q;
}
#define SONG_X0 38
#define SONG_Y0 (HDR + 16)
#define SONG_SH 30                   /* the SNAP lane's height */
#define SONG_GY0 (SONG_Y0 + SONG_SH + 18)   /* the graph's top, and bottom */
#define SONG_GY1 (KY - 20)
static const uint8_t song_bw[3] = {25, 50, 100};     /* pixels a bar, by zoom */
static uint16_t song_color(const pw_clip *c)
{
    uint16_t vc[8] = {cat_color[PW_C_SOURCE], cat_color[PW_C_FILTER], cat_color[PW_C_FX], cat_color[PW_C_MOD], C_AMBER, C_GREEN, cat_color[PW_C_SEQ], C_RED};
    return c->val & 0x80u ? C_RED : vc[c->val & 7u];
}
static void song_val_text(const pw_clip *c, char *o)   /* a snapshot clip's name: 5, or U3 for a patch */
{
    if (c->val & 0x80u) { *o++ = 'U'; o = s_int(o, (c->val & 7) + 1); *o = 0; } else s_int(o, (c->val & 15) + 1);
}
static void lane_label(int l, char *o)   /* "VCF CUTOFF" */
{
    const pw_alane *a = &pws.song.al[l];
    if (a->mod < PW_MAX_MODULES && pws.mod[a->mod].used == 1 && a->par < pw_types[pws.mod[a->mod].type].n_param) {
        char lab[16]; pw_module_label(a->mod, lab);
        o = s_cpy(o, lab); o = s_cpy(o, " "); s_cpy(o, pw_types[pws.mod[a->mod].type].param[a->par].name);
    } else s_cpy(o, "(GONE)");
}
static void draw_song_pick(void)
{
    int n = pick_count(), cur = clampi(ui.song_pickc, 0, n ? n - 1 : 0);
    text(6, HDR + 4, ui.song_pick == 2 ? "AUTOMATE THIS KNOB INSTEAD" : "WHICH KNOB TO AUTOMATE?", F_XS, C_AMBER);
    const int rows = 9, rh = 16, y0 = HDR + 16;
    int first = clampi(cur - rows / 2, 0, n > rows ? n - rows : 0);
    for (int r = 0; r < rows && first + r < n; ++r) {
        int mod = 0, par = 0, e = first + r;
        if (!pick_entry(e, &mod, &par)) break;
        int y = y0 + r * rh;
        const pw_type *t = &pw_types[pws.mod[mod].type];
        char lab[16], v[16];
        pw_module_label((unsigned)mod, lab);
        pw_format_param((unsigned)mod, (unsigned)par, v);
        rfill(1, y, W - 2, rh - 1, e == cur ? C_PANEL2 : C_PANEL);
        if (e == cur) frame_rect(1, y, W - 2, rh - 1, C_WHITE);
        fill(2, y + 1, 3, rh - 3, cat_color[t->cat]);
        text_fit(9, y + 3, lab, F_XS, cat_color[t->cat], 70);
        text_fit(82, y + 3, t->param[par].name, F_XS, C_TEXT, 90);
        text_right(W - 6, y + 3, v, F_XS, C_DIM);
        if (pw_lane_find((unsigned)mod, (unsigned)par) >= 0) text(76, y + 3, "*", F_XS, C_AMBER);
    }
    strip_set("K2|Module|SEL|Pick|REC|Back");
}
static void draw_song(void)
{
    pw_song *sg = &pws.song;
    if (ui.song_pick) { draw_song_pick(); return; }
    int nl = sg->n_lanes;
    ui.song_lane = (int8_t)clampi(ui.song_lane, 0, nl + 1);
    int lane = ui.song_lane, z = lane ? ui.song_zoom : 0;
    int bw = song_bw[z], span = 128 >> z;                   /* pixels a bar; sixteenths shown */
    int t = ui.song_bar, vs = ui.song_vs;
    ui.song_bar = (int16_t)(t = clampi(t, -1, 4079));
    if (t >= 0) { if (t < vs) vs = t; if (t >= vs + span) vs = t - span + 1; }
    vs = clampi(vs, 0, 4095 - span); vs -= vs % song_step[z];
    ui.song_vs = (int16_t)vs;
    int len = song_len(), carry = ui.song_carry ? (int)ui.song_carry - 1 : -1;
    text_center(16, HDR + 3, sg->on ? "ON" : "OFF", F_XS, sg->on ? C_GREEN : C_RED);
    if (ui.song_rec) rec_dot(4, HDR + 14 + SONG_SH + 4);
#define SGX(tt) (SONG_X0 + ((tt) - vs) * bw / 16)
    for (int b = vs / 16; b <= (vs + span) / 16 + 1; ++b) {   /* the bar numbers */
        int x = SGX(b * 16);
        if (x < SONG_X0 || x > W - 12) continue;
        char nb[6]; s_int(nb, b + 1);
        text(x + 2, HDR + 3, nb, F_XS, b % 4 ? C_DIM : C_TEXT);
    }
    /* SNAP lane */
    {
        int y = SONG_Y0;
        rfill(1, y, SONG_X0 - 3, SONG_SH - 2, C_PANEL);
        text_fit(3, y + SONG_SH / 2 - 5, "SNAP", F_XS, C_TEXT, SONG_X0 - 7);
        int bx0 = SONG_X0, bx1 = W - 1;
        fill(bx0, y, bx1 - bx0, SONG_SH - 2, C_SG1);
        for (int b = vs / 16; b <= (vs + span) / 16 + 1; ++b) { int x = SGX(b * 16); if (x >= bx0 && x < bx1 && (b / 4) % 2) fill(x, y, bw > bx1 - x ? bx1 - x : bw, SONG_SH - 2, C_SG2); }
        for (int k = 0; k < PW_SONG_CLIPS; ++k) {
            const pw_clip *c = &sg->clip[k];
            if (!c->len) continue;
            int x0 = SGX(c->start * 16), x1 = x0 + c->len * bw - 2;
            if (x1 <= SONG_X0 || x0 >= W) continue;
            int cx0 = x0 < SONG_X0 ? SONG_X0 : x0, cx1 = x1 > W - 1 ? W - 1 : x1;
            rfill(cx0, y, cx1 - cx0, SONG_SH - 2, song_color(c));
            if (x0 >= SONG_X0) { char d[6]; song_val_text(c, d); text(x0 + 5, y + SONG_SH / 2 - 8, d, F_M, C_BG); }
            if (k == carry) frame_rect(cx0, y, cx1 - cx0, SONG_SH - 2, C_AMBER);
        }
        if (lane == 0) {
            if (t < 0) { frame_rect(1, HDR + 1, SONG_X0 - 3, 13, C_WHITE); text_center(16, HDR + 3, sg->on ? "ON" : "OFF", F_XS, sg->on ? C_GREEN : C_RED); }   /* (the cursor is on the ON / OFF switch) */
            else {
                int k = song_at(t / 16);
                int s0 = k >= 0 ? sg->clip[k].start : t / 16, n = k >= 0 ? sg->clip[k].len : 1;
                int x0 = SGX(s0 * 16), x1 = x0 + n * bw - 2;
                int cx0 = x0 < SONG_X0 ? SONG_X0 : x0, cx1 = x1 > W - 1 ? W - 1 : x1;
                frame_rect(cx0, y, cx1 - cx0, SONG_SH - 2, carry >= 0 ? C_AMBER : C_WHITE);
                if (carry >= 0) frame_rect(cx0 + 1, y + 1, cx1 - cx0 - 2, SONG_SH - 4, C_AMBER);
            }
        }
    }
    /* the lane strip: the lanes' names, the selected one lit */
    {
        int y = SONG_Y0 + SONG_SH + 1;
        for (int l = 0; l <= nl; ++l) {
            int x = SONG_X0 + l * 25;
            char d[3] = {(char)('1' + l), 0, 0};
            if (l == nl) { d[0] = '+'; }
            uint16_t col = l == lane - 1 ? C_AMBER : l == nl && lane == nl + 1 ? C_AMBER : C_DIM;
            rfill(x, y, 23, 13, (l == lane - 1 || (l == nl && lane == nl + 1)) ? C_PANEL2 : C_PANEL);
            text_center(x + 11, y + 2, d, F_XS, col);
            if (l < nl && sg->al[l].n) fill(x + 2, y + 11, 19, 1, col);
        }
        if (lane > 0 && lane <= nl) { char lb[32]; lane_label(lane - 1, lb); text_fit(SONG_X0 + (nl + 1) * 25 + 4, y + 2, lb, F_XS, C_TEXT, W - SONG_X0 - (nl + 1) * 25 - 6); }
    }
    /* the graph */
    {
        int gy0 = SONG_GY0, gy1 = SONG_GY1;
        fill(SONG_X0, gy0, W - SONG_X0 - 1, gy1 - gy0, C_SGW);
        rfill(1, gy0, SONG_X0 - 3, gy1 - gy0, C_PANEL);
        for (int b = vs / 16; b <= (vs + span) / 16 + 1; ++b) { int x = SGX(b * 16); if (x >= SONG_X0 && x < W - 1) fill(x, gy0, 1, gy1 - gy0, b % 4 ? C_SG2 : C_SG3); }
        for (int l = 0; l < nl; ++l) {   /* every lane's curve, dimmed (the selected one is drawn over it) */
            if (l == lane - 1 || !sg->al[l].n) continue;
            uint16_t col = mid_of(sg->al[l].mod < PW_MAX_MODULES && pws.mod[sg->al[l].mod].used == 1 ? cat_color[pw_types[pws.mod[sg->al[l].mod].type].cat] : C_DIM);
            int ph = gy1 - gy0 - 8, px = SONG_X0, py = 0, have = 0;
            for (int x = SONG_X0; x < W - 1; x += 3) {
                float tt = (float)vs + (float)(x - SONG_X0) * 16.0f / (float)bw;
                int y = gy1 - 4 - (int)(pw_lane_eval((unsigned)l, tt) * (float)ph / 255.0f);
                if (have) line(px, py, x, y, 1, col);
                px = x; py = y; have = 1;
            }
        }
        if (lane >= 1 && lane <= nl) {
            int l = lane - 1;
            unsigned s = pw_lane_start((unsigned)l), n = sg->al[l].n;
            uint16_t col = sg->al[l].mod < PW_MAX_MODULES && pws.mod[sg->al[l].mod].used == 1 ? cat_color[pw_types[pws.mod[sg->al[l].mod].type].cat] : C_DIM;
            int ph = gy1 - gy0 - 8;
            int px = SONG_X0, py = 0, have = 0;
            for (int x = SONG_X0; x < W - 1; x += 3) {
                float tt = (float)vs + (float)(x - SONG_X0) * 16.0f / (float)bw;
                int y = gy1 - 4 - (int)(pw_lane_eval((unsigned)l, tt) * (float)ph / 255.0f);
                if (n && have) line(px, py, x, y, 1, col);
                px = x; py = y; have = 1;
            }
            for (unsigned i = 0; i < n; ++i) {
                int pt = (int)(sg->pt_t[s + i] & 0xfffu), x = SGX(pt), y = gy1 - 4 - (int)sg->pt_v[s + i] * ph / 255;
                if (x < SONG_X0 - 2 || x > W - 2) continue;
                int on = t == pt && ui.song_pcarry == 0;
                fill(x - 2, y - 2, 5, 5, on ? C_WHITE : col);
            }
            if (ui.song_pcarry) {   /* the point in hand, at the cursor */
                int x = SGX(t), y = gy1 - 4 - (int)ui.song_pv * ph / 255;
                if (x >= SONG_X0 && x < W - 1) { fill(x - 3, y - 3, 7, 7, C_AMBER); frame_rect(x - 4, y - 4, 9, 9, C_WHITE); }
            }
            if (t >= 0) { int x = SGX(t); if (x >= SONG_X0 && x < W - 1) fill(x, gy0, 1, gy1 - gy0, C_WHITE); }
        } else if (lane == nl + 1) {
            text_center((SONG_X0 + W) / 2, gy1 - 30, nl >= PW_LANES ? "ALL 8 LANES USED" : "CHOOSE A KNOB", F_S, C_TEXT);
            text_center((SONG_X0 + W) / 2, gy1 - 16, "OR REC, THEN TURN KNOBS", F_XS, C_DIM);
        }
        text(4, gy0 + 4, "AUTO", F_XS, C_TEXT);
    }
    if (pws.playing && sg->on && len) {   /* the playhead */
        float pos = (float)(pws.sixteenths % (16u * (unsigned)len));
        int x = SGX((int)pos);
        if (x >= SONG_X0 && x < W - 1) fill(x, HDR + 14, 1, SONG_GY1 - HDR - 14, C_RED);
    }
    /* the hint, then the four knobs */
    char v[16], *o;
    int ck = t >= 0 && lane == 0 ? song_at(t / 16) : -1;
    if (carry >= 0) ck = carry;
    int pi = lane >= 1 && lane <= nl ? lane_pt_at(lane - 1, t) : -1;
    const char *hint;
    if (lane == 0) hint = carry >= 0 ? "SELECT|Move|SEL|Drop|REC|Cancel" : t < 0 ? "SEL|Song on / off" : ck >= 0 ? "SEL|Pick up|REC|Delete|GLO+SEL|Duplicate" : "SEL|New clip";
    else if (lane > nl) hint = "SEL|Choose a knob|K4|Favs, Recents";
    else hint = ui.song_pcarry ? "SELECT|Move|SEL|Drop|REC|Cancel" : pi >= 0 ? "K1|Value|K2|Shape|REC|Delete" : "SEL|Add a point|REC|Record";
    { char sp[112], *o = s_cpy(sp, hint); if (lane == 0 && !ui.song_pcarry) o = s_cpy(o, "|ALGO|Lanes"); strip_set(sp); }
    if (lane == 0) {
        unsigned nlen = ck >= 0 ? sg->clip[ck].len : (ui.song_newlen ? ui.song_newlen : 4u);
        o = s_int(v, (int)nlen); s_cpy(o, nlen == 1 ? " BAR" : " BARS");
        knob_cell(0, "LENGTH", v, (float)nlen / 16.0f, C_AMBER);
        if (ck >= 0) { song_val_text(&sg->clip[ck], v); knob_cell(1, sg->clip[ck].val & 0x80u ? "PATCH" : "SNAP", v, (sg->clip[ck].val & 0x80u) ? -1.0f : (float)(sg->clip[ck].val & 15) / 15.0f, song_color(&sg->clip[ck])); }
        else knob_cell(1, "SNAP", "--", -1, C_DIM);
        knob_cell(2, "EDITS", pws.edit_all ? "ALL" : "THIS ONE", -1, C_DIM);
        knob_cell(3, "GLIDE", sg->glide ? (sg->glide == 1 ? "1 BAR" : sg->glide == 2 ? "2 BARS" : "4 BARS") : "OFF", sg->glide / 4.0f, cat_color[PW_C_MOD]);
    } else if (lane <= nl) {
        static const char *const shn[3] = {"LINEAR", "STEP", "SMOOTH"};
        int l = lane - 1;
        float val = t >= 0 ? pw_lane_eval((unsigned)l, (float)t) : 0.0f;
        o = s_int(v, (int)(val * 100.0f / 255.0f + 0.5f)); s_cpy(o, "%");
        knob_cell(0, "VALUE", t >= 0 ? v : "--", val / 255.0f, C_AMBER);
        if (pi >= 0) knob_cell(1, "SHAPE", shn[sg->pt_t[pw_lane_start((unsigned)l) + (unsigned)pi] >> 12], -1, C_DIM); else knob_cell(1, "SHAPE", "--", -1, C_DIM);
        knob_cell(2, "ZOOM", z == 0 ? "8 BARS" : z == 1 ? "4 BARS" : "2 BARS", z / 2.0f, cat_color[PW_C_MOD]);
        { const pw_alane *a = &sg->al[l]; knob_cell(3, "KNOB", a->mod < PW_MAX_MODULES && pws.mod[a->mod].used == 1 && a->par < pw_types[pws.mod[a->mod].type].n_param ? pw_types[pws.mod[a->mod].type].param[a->par].name : "GONE", -1, C_DIM); }
    } else {
        knob_cell(0, 0, 0, -1, 0); knob_cell(1, 0, 0, -1, 0); knob_cell(2, 0, 0, -1, 0);
        knob_cell(3, "KNOB", ui.last_mod < PW_MAX_MODULES && pws.mod[ui.last_mod].used == 1 && ui.last_par < pw_types[pws.mod[ui.last_mod].type].n_param ? pw_types[pws.mod[ui.last_mod].type].param[ui.last_par].name : "--", -1, C_DIM);
    }
#undef SGX
}
static void song_pick_done(int mod, int par)   /* the list's choice: its lane (made if there is none), or the lane's new knob */
{
    int have = pw_lane_find((unsigned)mod, (unsigned)par);
    if (ui.song_pick == 2 && ui.song_lane >= 1 && ui.song_lane <= pws.song.n_lanes && have < 0) {
        pws.song.al[ui.song_lane - 1].mod = (uint8_t)mod; pws.song.al[ui.song_lane - 1].par = (uint8_t)par; pws.revision++;
    } else if (have >= 0) ui.song_lane = (int8_t)(have + 1);
    else {
        int l = pw_lane_add((unsigned)mod, (unsigned)par);
        if (l < 0) { notice("ALL 8 LANES USED"); ui.song_pick = 0; return; }
        ui.song_lane = (int8_t)(l + 1);
        pw_lane_set((unsigned)l, 0, (unsigned)(pw_lane_norm((unsigned)mod, (unsigned)par, pws.mod[mod].p[par]) * 255.0f + 0.5f), 0);
    }
    ui.song_pick = 0; ui.song_zoom = ui.song_zoom > 2 ? 0 : ui.song_zoom; ui.song_pcarry = 0;
}
static void song_input(const struct fm1_controls *c, uint32_t pressed, int glo)
{
    pw_song *sg = &pws.song;
    uint32_t now = fm1_millis();
    int d = c->encoders[E_SELECT], carry = ui.song_carry ? (int)ui.song_carry - 1 : -1;
    if (ui.song_pick) {   /* the knob list */
        int n = pick_count();
        if (d) {
            int raw = ui.raw_select ? ui.raw_select : d, dir = raw > 0 ? 1 : -1, nn = raw > 0 ? raw : -raw;
            if (dir != ui.sel_dir || now - ui.sel_ms > 110u) ui.sel_n = 0;
            ui.sel_dir = dir; ui.sel_ms = now; ui.sel_n += nn;
            if (ui.sel_n >= 3) { for (int i = 0; i < nn; ++i) ui.song_pickc = (int16_t)pick_module_edge(ui.song_pickc, dir); }
            else ui.song_pickc = (int16_t)clampi(ui.song_pickc + d, 0, n ? n - 1 : 0);
        }
        if (c->encoders[E_K2]) ui.song_pickc = (int16_t)pick_module_edge(ui.song_pickc, c->encoders[E_K2] > 0 ? 1 : -1);
        if (pressed & BIT(B_REC)) { ui.song_pick = 0; return; }
        if (pressed & BIT(B_SEL)) { int mod, par; if (pick_entry(ui.song_pickc, &mod, &par)) song_pick_done(mod, par); }
        return;
    }
    int nl = sg->n_lanes;
    int lane = ui.song_lane, z = lane ? ui.song_zoom : 0, step = lane ? song_step[z] : 16;
    if (d) {   /* slowly one step at a time; fast, edge to edge (of the clips, or the lane's points) */
        int raw = ui.raw_select ? ui.raw_select : d, dir = raw > 0 ? 1 : -1, n = raw > 0 ? raw : -raw;
        if (dir != ui.sel_dir || now - ui.sel_ms > 110u) { ui.sel_n = 0; ui.sel_acc = 1; }
        ui.sel_dir = dir; ui.sel_ms = now; ui.sel_n += n;
        int t = ui.song_bar;
        if (carry >= 0) {
            pw_clip *k = &sg->clip[carry];
            int pw_ = clampi((int)k->start + d, 0, 255 - k->len);
            k->start = (uint8_t)pw_; t = pw_ * 16;
        } else if (ui.sel_n >= 3) {
            for (ui.sel_acc += n; ui.sel_acc >= 2; ui.sel_acc -= 2) {
                if (lane == 0) { int b = song_edge(t < 0 ? -1 : t / 16, dir); t = b < 0 ? -1 : b * 16; }
                else if (lane <= nl) t = lane_edge(lane - 1, t < 0 ? 0 : t, dir);
                else t = clampi(t + dir * 16, 0, 4079);
            }
        } else if (t < 0 && dir > 0) t = 0;
        else if (lane == 0) {   /* a clip is one stop (its whole length), empty bars one at a time */
            int b = t < 0 ? -1 : t / 16;
            for (int stp = d > 0 ? d : -d; stp > 0; --stp) {
                if (d > 0) { int k = b >= 0 ? song_at(b) : -1; b = k >= 0 ? sg->clip[k].start + sg->clip[k].len : b + 1; }
                else {
                    int k = b >= 0 ? song_at(b) : -1;
                    b = k >= 0 && b > sg->clip[k].start ? sg->clip[k].start : (k >= 0 ? sg->clip[k].start : b) - 1;
                    int k2 = b >= 0 ? song_at(b) : -1;
                    if (k2 >= 0) b = sg->clip[k2].start;
                }
            }
            t = b < 0 ? -1 : b * 16;
        }
        else t = t + step * d < 0 ? (lane ? 0 : -1) : t + step * d;
        ui.song_bar = (int16_t)clampi(t, -1, lane == 0 ? 254 * 16 : 4079);
    }
    int da = c->encoders[E_ALGO];
    if (da && carry < 0 && !ui.song_pcarry) {   /* the lanes in order: SNAP, the automation lanes, the "+" */
        ui.song_lane = (int8_t)clampi(lane + da, 0, nl + 1);
        if (ui.song_lane == 0) ui.song_bar = (int16_t)(ui.song_bar < 0 ? -1 : ui.song_bar / 16 * 16);
        else if (ui.song_bar < 0) ui.song_bar = 0;
        lane = ui.song_lane;
    }
    int t = ui.song_bar, bar = t < 0 ? -1 : t / 16;
    int ck = carry >= 0 ? carry : (lane == 0 && bar >= 0 ? song_at(bar) : -1);
    int pi = lane >= 1 && lane <= nl && t >= 0 ? lane_pt_at(lane - 1, t) : -1;
    int dk = c->encoders[E_K1];
    if (lane == 0) {
        if (dk) {   /* LENGTH */
            if (ck >= 0) {
                pw_clip *k = &sg->clip[ck];
                int nlen = clampi((int)k->len + dk, 1, 64);
                if (nlen + k->start > 255) nlen = 255 - k->start;
                if (nlen <= k->len || !song_clash(k->start, nlen, ck)) { k->len = (uint8_t)nlen; ui.song_newlen = (uint8_t)nlen; }
            } else ui.song_newlen = (uint8_t)clampi((ui.song_newlen ? ui.song_newlen : 4) + dk, 1, 64);
            pws.revision++;
        }
        if (c->encoders[E_K2] && ck >= 0) {   /* the snapshot 1-16, then the saved patches U1-U8 */
            pw_clip *k = &sg->clip[ck];
            int idx = (k->val & 0x80u) ? 16 + (k->val & 7) : (k->val & 15);
            idx = clampi(idx + c->encoders[E_K2], 0, 16 + PW_USER_SLOTS - 1);
            k->val = (uint8_t)(idx < 16 ? idx : 0x80 | (idx - 16));
            pws.revision++;
        }
        if (c->encoders[E_K3]) { pws.edit_all = c->encoders[E_K3] > 0; pws.revision++; }   /* EDITS: this snapshot only, or all */
        if (c->encoders[E_K4]) {   /* GLIDE: off, 1, 2, 4 bars */
            static const uint8_t gl[4] = {0, 1, 2, 4};
            int at = 0; for (int i = 0; i < 4; ++i) if (gl[i] == sg->glide) at = i;
            sg->glide = gl[clampi(at + c->encoders[E_K4], 0, 3)]; pws.revision++;
        }
    } else if (lane <= nl) {
        int l = lane - 1;
        if (dk && t >= 0) {   /* VALUE: of the point here, or a point made here */
            unsigned s = pw_lane_start((unsigned)l);
            int q = pi >= 0 ? sg->pt_v[s + (unsigned)pi] : (int)(pw_lane_eval((unsigned)l, (float)t) + 0.5f);
            q = clampi(q + dk * (glo ? 1 : 4), 0, 255);
            unsigned sh = pi >= 0 ? sg->pt_t[s + (unsigned)pi] >> 12 : 0;
            if (ui.song_pcarry) ui.song_pv = (uint8_t)q;
            else if (pw_lane_set((unsigned)l, (unsigned)t, (unsigned)q, sh) < 0) notice("POINT POOL FULL");
        }
        if (c->encoders[E_K2] && pi >= 0) {   /* SHAPE of the segment after the point */
            unsigned s = pw_lane_start((unsigned)l) + (unsigned)pi;
            int sh = clampi((int)(sg->pt_t[s] >> 12) + c->encoders[E_K2], 0, PW_SHAPES - 1);
            sg->pt_t[s] = (uint16_t)((sg->pt_t[s] & 0xfffu) | (unsigned)sh << 12); pws.revision++;
        }
        if (c->encoders[E_K3]) ui.song_zoom = (uint8_t)clampi((int)ui.song_zoom + c->encoders[E_K3], 0, 2);
        if (c->encoders[E_K4]) { ui.song_pick = 2; ui.song_pickc = (int16_t)pick_of(sg->al[l].mod, sg->al[l].par); return; }
    } else if (c->encoders[E_K4]) { ui.song_pick = 1; ui.song_pickc = (int16_t)pick_of(ui.last_mod < PW_MAX_MODULES ? ui.last_mod : 0, ui.last_par); return; }
    if (pressed & BIT(B_REC)) {
        if (carry >= 0) { sg->clip[carry].start = ui.song_ostart; ui.song_carry = 0; notice("BACK WHERE IT WAS"); }
        else if (ui.song_pcarry) {
            pw_lane_set((unsigned)(lane - 1), ui.song_pold, ui.song_pv, ui.song_psh); ui.song_pcarry = 0; notice("BACK WHERE IT WAS");
        } else if (lane == 0 && ck >= 0) { sg->clip[ck].len = 0; pws.revision++; notice("CLIP DELETED"); }
        else if (lane >= 1 && lane <= nl && glo) { pw_lane_del((unsigned)(lane - 1)); ui.song_lane = (int8_t)clampi(lane - 1, 0, pws.song.n_lanes + 1); notice("LANE DELETED"); }
        else if (pi >= 0) { pw_lane_del_pt((unsigned)(lane - 1), (unsigned)pi); notice("POINT DELETED"); }
        else { ui.song_rec = !ui.song_rec; ui.rec_t = 0xffff; notice(ui.song_rec ? "RECORDING: PLAY, THEN TURN KNOBS" : "RECORDING OFF"); }
    }
    if (pressed & BIT(B_SEL)) {
        if (lane > nl) {
            if (nl >= PW_LANES) notice("ALL 8 LANES USED"); else { ui.song_pick = 1; ui.song_pickc = (int16_t)pick_of(ui.last_mod < PW_MAX_MODULES ? ui.last_mod : 0, ui.last_par); }
        } else if (lane >= 1) {
            if (ui.song_pcarry) {
                if (pw_lane_set((unsigned)(lane - 1), (unsigned)t, ui.song_pv, ui.song_psh) < 0) notice("POINT POOL FULL"); else ui.song_pcarry = 0;
            } else if (t < 0) { sg->on = !sg->on; pws.revision++; ui.song_s16 = 0xffffffffu; ui.song_chain = 0; notice(sg->on ? "SONG ON" : "SONG OFF"); }
            else if (pi >= 0) {   /* pick the point up: it is put back where the cursor is dropped */
                unsigned s = pw_lane_start((unsigned)(lane - 1)) + (unsigned)pi;
                ui.song_pv = sg->pt_v[s]; ui.song_psh = (uint8_t)(sg->pt_t[s] >> 12); ui.song_pold = (uint16_t)t; ui.song_pcarry = 1;
                pw_lane_del_pt((unsigned)(lane - 1), (unsigned)pi);
            } else if (pw_lane_set((unsigned)(lane - 1), (unsigned)t, (unsigned)(pw_lane_eval((unsigned)(lane - 1), (float)t) + 0.5f), 0) < 0) notice("POINT POOL FULL");
        } else if (carry >= 0) {
            pw_clip *k = &sg->clip[carry];
            if (song_clash(k->start, k->len, carry)) notice("IT OVERLAPS ANOTHER CLIP");
            else { ui.song_carry = 0; pws.revision++; }
        } else if (bar < 0) {
            sg->on = !sg->on; pws.revision++;
            ui.song_s16 = 0xffffffffu; ui.song_chain = 0;
            notice(sg->on ? "SONG ON" : "SONG OFF");
        } else if (ck >= 0 && glo) {   /* duplicate: a copy straight after it, with a snapshot of its own (a copy of this one) */
            pw_clip src = sg->clip[ck];
            int at = -1; for (int k = 0; k < PW_SONG_CLIPS && at < 0; ++k) if (!sg->clip[k].len) at = k;
            if (at < 0) notice("SONG FULL");
            else if (src.start + src.len * 2 > 255 || song_clash(src.start + src.len, src.len, -1)) notice("NO ROOM AFTER IT");
            else {
                int own = -1;
                if (!(src.val & 0x80u))   /* the first snapshot no clip plays, and that is not the one on screen */
                    for (unsigned v = 0; v < PW_VARS && own < 0; ++v) {
                        int used = v == pws.var_cur; for (int k = 0; k < PW_SONG_CLIPS; ++k) if (sg->clip[k].len && !sg->clip[k].lane && !(sg->clip[k].val & 0x80u) && (unsigned)(sg->clip[k].val & 15u) == v) used = 1;
                        if (!used) own = (int)v;
                    }
                sg->clip[at] = src; sg->clip[at].start = (uint8_t)(src.start + src.len);
                if (own >= 0) { pw_var_copy(src.val & 15u, (unsigned)own); sg->clip[at].val = (uint8_t)own; }
                ui.song_bar = (int16_t)(sg->clip[at].start * 16); sg->on = 1; pws.revision++;
                if (own >= 0) { char m[32], *o = s_cpy(m, "DUPLICATED: SNAPSHOT "); s_int(o, own + 1); notice(m); } else notice("DUPLICATED (NO SNAPSHOT FREE: IT SHARES)");
            }
        } else if (ck >= 0) {
            ui.song_carry = (uint8_t)(ck + 1); ui.song_ostart = sg->clip[ck].start;
        } else {
            int at = -1; for (int k = 0; k < PW_SONG_CLIPS && at < 0; ++k) if (!sg->clip[k].len) at = k;
            int n = ui.song_newlen ? ui.song_newlen : 4;
            if (at < 0) notice("SONG FULL");
            else {
                while (n > 1 && song_clash(bar, n, -1)) --n;
                if (song_clash(bar, n, -1) || bar + n > 255) notice("NO ROOM HERE");
                else { sg->clip[at].lane = 0; sg->clip[at].start = (uint8_t)bar; sg->clip[at].len = (uint8_t)n; sg->clip[at].val = pws.var_cur; if (!sg->on) { sg->on = 1; ui.song_s16 = 0xffffffffu; ui.song_chain = 0; notice("SONG ON"); } pws.revision++; }
            }
        }
    }
}
static void handle(const struct fm1_controls *c_in)
{
    struct fm1_controls c_copy = *c_in;
    const struct fm1_controls *c = &c_copy;
    uint32_t pressed = c->buttons & ~ui.buttons, released = ui.buttons & ~c->buttons;
    if (pressed & BIT(B_HOME)) { ui.home_down_ms = fm1_millis(); ui.home_rack = (uint8_t)(ui.page == PG_RACK); ui.home_cyc = (uint8_t)(ui.page == PG_RACK && !ui.gsug && !ui.gview && !(c->buttons & BIT(B_GLO)) && !ui.glo_on); }   /* (before anything below changes the page) */
    int glo_hw = (c->buttons & BIT(B_GLO)) != 0;
    uint32_t now = fm1_millis();
    int any_enc = 0;
    for (int i = 0; i < 7; ++i) any_enc |= c->encoders[i] != 0;
    /* latched GLO (the setting): it clears with the next button or key, or a moment after the knobs stop */
    if (ui.glo_on) {
        if (any_enc) ui.glo_enc_ms = now;
        else if (ui.glo_enc_ms && now - ui.glo_enc_ms > 800u) { ui.glo_on = 0; ui.glo_enc_ms = 0; }
    }
    int glo = glo_hw || ui.glo_on, fine = glo;
    /* GLO + PRESET: the metronome on (turn up) or off (turn down), from every page but the patch list (where it jumps between its parts) */
    if (glo && c_copy.encoders[E_PRESETS] && ui.page != PG_PATCHES && ui.page != PG_SAVE && ui.page != PG_HELP && ui.page != PG_SETTINGS && ui.page != PG_BOOT) {
        int on = c_copy.encoders[E_PRESETS] > 0;
        if (on != pws.metro) { pws.metro = (uint8_t)on; pws.revision++; }
        notice_glo(on ? "METRONOME ON" : "METRONOME OFF");   /* (over GLO's hints, on every turn, like the tempo) */
        c_copy.encoders[E_PRESETS] = 0;
    }
    int glo_latched_use = ui.glo_on && !glo_hw, used_now = (pressed & ~BIT(B_GLO)) != 0 || (c->keys & ~ui.keys) != 0;   /* (a key pressed now uses the latch; it clears after this pass) */
    /* GLO: held with anything else, a modifier; tapped alone, the shortcuts */
    if (pressed & BIT(B_GLO)) { ui.glo_down_ms = now; ui.glo_used = 0; }
    if (glo && ((pressed & ~BIT(B_GLO)) || any_enc || c->keys != ui.keys)) ui.glo_used = 1;
    if ((released & BIT(B_GLO)) && !ui.glo_used && now - ui.glo_down_ms < 600u) {
        if (ui.page == PG_HELP || ui.page == PG_SETTINGS) ui.glo_on = 0;
        else if (ui.glo_latch) { ui.glo_on ^= 1u; ui.glo_enc_ms = 0; }   /* latch: the next key is a GLO key (tapped again: not) */
    }
    if (ui.page == PG_SETTINGS && ui.diag && (pressed & BIT(B_HOME))) { ui.diag = 0; pressed &= ~BIT(B_HOME); }   /* (DIAGNOSTICS: HOME back to the list of settings) */
    if (ui.page == PG_SETTINGS && (pressed & (BIT(B_HOME) | BIT(B_FX) | BIT(B_ARP) | BIT(B_SEQ) | BIT(B_EDIT) | BIT(B_ENV) | BIT(B_LFO)))) {
        go(pressed & BIT(B_HOME) ? PG_RACK : (ui.prev_page ? ui.prev_page : PG_RACK));
        if (!(pressed & BIT(B_HOME))) pressed = 0;
    }
    if (ui.page == PG_HELP && (pressed & ~(BIT(B_GLO) | BIT(B_OCT_DOWN) | BIT(B_OCT_UP) | BIT(B_PLAY) | BIT(B_SAVE)))) {
        go(pressed & BIT(B_HOME) ? PG_RACK : (ui.prev_page ? ui.prev_page : PG_RACK));
        if (!(pressed & BIT(B_HOME))) pressed = 0;
    }
    /* GLO + ENV copy, GLO + LFO paste, GLO + SEL grab a cable, GLO + OCT- undo, GLO + OCT+ redo, GLO + EDIT the module in the voices or not,
     * GLO + PLAY the module bypassed or not */
    if (glo && (pressed & BIT(B_PLAY))) {
        int cur = ui.page == PG_RACK && pv_n ? ui.cur_slot : valid_sel(0);
        if (cur >= 0) {
            char msg[40], lab[16];
            pw_set_flags((unsigned)cur, pws.mod[cur].flags ^ PW_F_BYPASS);
            pw_module_label((unsigned)cur, lab);
            char *o = s_cpy(msg, lab); s_cpy(o, pws.mod[cur].flags & PW_F_BYPASS ? ": BYPASSED" : ": ON");
            notice(msg);
        }
        pressed &= ~BIT(B_PLAY);
    }
    if (glo && ui.page == PG_RACK && pv_n && (!ui.carrying || ui.gview || pv_col_of(ui.cur_slot)) && (pressed & BIT(B_FX))) {   /* GLO + FX: group (suggestions), open a group, or leave it (with a cable in hand it goes with you) */
        pressed &= ~BIT(B_FX);
        if (ui.gsug) ui.gsug = 0;
        else if (ui.gview) { int sl = ui.cur_slot; ui.gview = 0; pv_to_box(sl); }
        else if (pv_col_of(ui.cur_slot)) pv_open_group();
        else if (grp_cands(0)) { ui.gsug = 1; ui.gsug_i = 0; }
        else notice("NO CHAIN TO GROUP: A SOURCE INTO AN OUTPUT OR MIXER");
    }
    if (glo && ui.page == PG_RACK && pv_n && !ui.carrying && (pressed & BIT(B_REC))) {   /* GLO + REC: ungroup */
        if (pv_eg[ui.cur_slot]) pressed &= ~BIT(B_REC);   /* (a group's box: ungroup; any other module: GLO + REC held deletes it, wired through, below) */
        if (pv_eg[ui.cur_slot]) ui.rec_used = 1;
        if (pv_eg[ui.cur_slot]) { int sl = ui.cur_slot; grp_ungroup(sl); ui.gview = 0; pv_goto(sl, ui.cur_out, ui.cur_port); notice("UNGROUPED"); }
    }
    if (glo && ui.page == PG_SEQ && (pressed & (BIT(B_SEL) | BIT(B_REC)))) {   /* GLO + SEL: a new random pattern; GLO + REC: clear it (SEQUENCER, MOD SEQ) */
        int sq = find_seq();
        if (sq >= 0 && pws.mod[sq].type == PW_T_PROLL) {   /* the roll: GLO + SEL copies the bar the cursor is in onto the next one, GLO + REC clears */
            pw_module *rm = &pws.mod[sq];
            ui.rec_used = 1;
            pw_undo_data((unsigned)sq);
            if (pressed & BIT(B_REC)) { pw_roll_clear(rm); notice("CLEARED (GLO+OCT- UNDOES)"); }
            else if (pw_roll_loop(rm) < 32) notice("LOOP IS ONE BAR: SET LOOP TO 2+");
            else {
                unsigned bar = (unsigned)ui.roll_t / 16u, nb = (bar + 1u) % (pw_roll_loop(rm) / 16u), n0 = pw_roll_count(rm);
                for (unsigned k = 0; k < n0; ++k) { unsigned a, l, p, v, vo; pw_roll_get(rm, k, &a, &l, &p, &v, &vo); if (a / 16u == bar) pw_roll_add(rm, a - bar * 16u + nb * 16u, l, p, v); }
                notice("BAR COPIED TO THE NEXT");
            }
            pws.revision++;
            pressed &= ~(BIT(B_SEL) | BIT(B_REC));
        } else if (sq >= 0 && (pws.mod[sq].type == PW_T_SEQ || pws.mod[sq].type == PW_T_MODSEQ)) {
            int clr = (pressed & BIT(B_REC)) != 0;
            pw_seq_fill((unsigned)sq, clr); ui.rec_used = 1;
            notice(clr ? "CLEARED (GLO+OCT- UNDOES)" : "RANDOMIZED (GLO+OCT- UNDOES)");
            pressed &= ~(BIT(B_SEL) | BIT(B_REC));
        }
    }
    uint32_t song_sel = ui.page == PG_SONG ? pressed & BIT(B_SEL) : 0;   /* (GLO + SEL on SONG: duplicate the clip, handled there) */
    if (glo && ui.page != PG_ADD && (pressed & (BIT(B_ENV) | BIT(B_LFO) | BIT(B_SEL) | BIT(B_OCT_DOWN) | BIT(B_OCT_UP) | BIT(B_EDIT)))) {
        int cur = ui.page == PG_RACK && pv_n ? ui.cur_slot : valid_sel(0);
        char msg[40], lab[16];
        if (pressed & BIT(B_SEL)) {
            if (ui.page == PG_RACK && pv_n && !ui.carrying) { pv_pick(); ui.rec_used = 1; }
        } else if (pressed & BIT(B_ENV)) {
            ui.env_used = 1;
            if (cur >= 0 && pw_copy((unsigned)cur) == 0) { pw_module_label((unsigned)cur, lab); char *o = s_cpy(msg, "COPIED "); s_cpy(o, lab); notice_glo(msg); }
        } else if (pressed & BIT(B_LFO)) {
            int x, y; free_spot(&x, &y);
            int n = pw_paste(x, y);
            if (n < 0) notice_glo(pw_clip_type() < 0 ? "NOTHING COPIED (GLO+ENV COPIES)" : "PATCH FULL");
            else { ui.sel = n; ui.param_page[n] = 0; pw_module_label((unsigned)n, lab); char *o = s_cpy(msg, "PASTED "); s_cpy(o, lab); notice_glo(msg); go(PG_RACK); pv_goto(n, 0, 0); }
        } else if (pressed & BIT(B_OCT_DOWN)) notice_glo(pw_undo() ? "UNDONE  (GLO+OCT+ REDOES)" : "NOTHING TO UNDO");
        else if (pressed & BIT(B_OCT_UP)) notice_glo(pw_redo() ? "REDONE  (GLO+OCT- UNDOES)" : "NOTHING TO REDO");
        else if (cur >= 0) {
            if (pws.voices < 2) notice("ONE VOICE: SET VOICES (PATCH LIST, K4)");
            else {
                pw_set_flags((unsigned)cur, pws.mod[cur].flags ^ PW_F_POLY);
                pw_module_label((unsigned)cur, lab);
                char *o = s_cpy(msg, lab); s_cpy(o, pws.mod[cur].flags & PW_F_POLY ? ": ONE PER VOICE" : ": SHARED BY ALL VOICES");
                notice(msg);
            }
        }
        pressed &= ~(BIT(B_ENV) | BIT(B_LFO) | BIT(B_SEL) | BIT(B_OCT_DOWN) | BIT(B_OCT_UP) | BIT(B_EDIT));
        pressed |= song_sel;
    }
    if (glo && (pressed & BIT(B_HOME))) {   /* GLO + HOME: the keys play notes / are the BUTTON modules' */
        int hb = 0;
        for (int i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1 && pws.mod[i].type == PW_T_BUTTON) hb = 1;
        if (!hb) notice("NO BUTTON MODULE IN THIS PATCH");
        else { ui.kbd_btn ^= 1u; notice(button_mode_eff() ? "BUTTON MODE" : "KEYBOARD MODE"); }
        ui.glo_used = 1; pressed &= ~BIT(B_HOME);
    }
    if (released & BIT(B_HOME)) {   /* HOME tapped on the patch screen: the selected module's next page of knobs (a group: its members'); held, the CPU and battery showed instead */
        if (ui.home_cyc && ui.page == PG_RACK && pv_n && now - ui.home_down_ms < 700u) {
            int sl = ui.cur_slot;
            if (pv_col_of(sl)) sp_turn(sl, 1);
            else { fix_page(sl); unsigned pg = param_pages(sl); if (pg > 1) ui.param_page[sl] = (unsigned)((ui.param_page[sl] + 1) % pg); }
        }
        ui.home_cyc = 0; ui.home_rack = 0;
    }
    if ((pressed & BIT(B_HOME)) && ui.page == PG_RACK && (ui.gsug || ui.gview)) {   /* HOME: out of a group (or the suggestions) */
        if (ui.gsug) ui.gsug = 0;
        else { int sl = ui.cur_slot; ui.gview = 0; pv_to_box(sl); }
        pressed &= ~BIT(B_HOME);
    }
    if (pressed & BIT(B_HOME)) { ui.lst_pick = 0; go(PG_RACK); }   /* (a cable half made in the matrix is dropped) */
    if (ui.page == PG_MUTATE && (pressed & BIT(B_FX))) { mutate_go(0); pressed &= ~BIT(B_FX); }   /* (FX nudges here) */
    if (pressed & BIT(B_FX)) {   /* the add list (on the patch screen: where the cursor is); again: the panel's next page */
        if (ui.page == PG_ADD) ui.add_page = (uint8_t)((ui.add_page + 1) % 3);
        else { ui.page_from_patch = ui.page == PG_RACK; add_open(); }
    }
    if (pressed & BIT(B_ENV)) {   /* the add lists: ENV adds it unconnected; else LIVE (again: back) */
        if (ui.page == PG_ADD) { ui.env_used = 1; add_type(add_order[clampi(ui.add_cursor, 0, PW_T_SHOWN - 1)], 0); }
        else { ui.env_down_ms = now; ui.env_used = 0; }   /* PERFORM: on its release (held while a knob turns: that knob goes on the page) */
        pressed &= ~BIT(B_ENV);
    }
    if ((released & BIT(B_ENV)) && !ui.env_used && ui.page != PG_ADD) {   /* PERFORM, then (ENV again) MUTES, then MUTATE, then PERFORM again; HOME is the way out */
        if (ui.page == PG_PERFORM) { ui.page = PG_LIVE; ui.live_row = 0; ui.confirm_kind = 0; }
        else if (ui.page == PG_LIVE) { ui.page = PG_MUTATE; ui.mut_row = 0; ui.confirm_kind = 0; }
        else if (ui.page == PG_MUTATE) { ui.page = PG_PERFORM; ui.perf_cursor = 0; ui.perf_bank = 0; ui.confirm_kind = 0; }
        else go(PG_PERFORM);
    }
    if (pressed & BIT(B_LFO)) {   /* SONG (again: back) */
        if (ui.page == PG_SONG) go(ui.prev_page && ui.prev_page != PG_HELP && ui.prev_page != PG_SONG ? ui.prev_page : PG_RACK);
        else { go(PG_SONG); ui.song_carry = 0; if (ui.song_bar < -1) ui.song_bar = 0; }
        pressed &= ~BIT(B_LFO);
    }
    if (0) { if (ui.page != PG_CABLES) { int s = ui.page == PG_RACK && pv_n ? ui.cur_slot : valid_sel(0); go(PG_CABLES); ui.lst_pick = 0;
                                 int n = lst_build(); for (int i = 0; i < n; ++i) if (lrows[i].slot == s && lrows[i].cable < 0) ui.lst_cursor = i; } }
    live_queue_run();
    if (ui.var_pend >= 0) {   /* a variation chosen on PERFORM lands on the bar line (at once if the transport stopped) */
        if (!pws.playing || (int)(pws.sixteenths / 16u) > ui.var_pend_bar) { pw_var_select((unsigned)ui.var_pend); ui.var_pend = -1; }
    }
    if (!pws.morphing) ui.morph = 0.0f;                   /* (a variation chosen, a patch loaded: MORPH back at A) */
    /* ARP: the performance page on its release (held while a knob turns: that knob goes on the page instead) */
    if (pressed & BIT(B_ARP)) { ui.arp_down_ms = now; ui.arp_used = 0; }
    if ((released & BIT(B_ARP)) && !ui.arp_used) {   /* ARP: the MATRIX (again: back) */
        if (ui.page == PG_CABLES) go(ui.prev_page && ui.prev_page != PG_HELP && ui.prev_page != PG_CABLES ? ui.prev_page : PG_RACK);
        else { int s = ui.page == PG_RACK && pv_n ? ui.cur_slot : valid_sel(0); go(PG_CABLES); ui.lst_pick = 0;
               int n = lst_build(); for (int i = 0; i < n; ++i) if (lrows[i].slot == s && lrows[i].cable < 0) ui.lst_cursor = i; }
    }
    /* REC on the patch screen and the module page: tapped, unplug (or drop the cable in hand); held, delete the module */
    if ((ui.page == PG_RACK && !ui.gsug) || ui.page == PG_EDIT) {
        int cur = ui.page == PG_RACK ? (pv_n ? ui.cur_slot : -1) : valid_sel(0);
        if (pressed & BIT(B_REC)) { ui.rec_down_ms = now; ui.rec_used = 0; ui.rec_warned = 0; ui.rec_wire = (uint8_t)(glo != 0); }
        if ((c->buttons & BIT(B_REC)) && !ui.rec_used && cur >= 0) {
            if (!ui.rec_warned && now - ui.rec_down_ms > 250u) {
                char lab[16], msg[40]; pw_module_label((unsigned)cur, lab);
                char *o = s_cpy(msg, "HOLD TO DELETE "); o = s_cpy(o, lab); if (ui.rec_wire) s_cpy(o, ", WIRED"); notice(msg); ui.rec_warned = 1;
            }
            if (now - ui.rec_down_ms > 700u) { delete_now(cur, ui.rec_wire); ui.rec_used = 1; }
        }
        if ((released & BIT(B_REC)) && !ui.rec_used && ui.rec_wire) notice("HOLD GLO+REC: DELETE, WIRED THROUGH");
        else if ((released & BIT(B_REC)) && !ui.rec_used) {
            ui.notice = 0;
            if (ui.page == PG_RACK) pv_rec();
            else if (slot_is(ui.sel, PW_T_TRIGSEQ)) {   /* TRIG SEQ: REC cycles step record, live record, off (hold it to delete the module) */
                ui.trig_rec = (uint8_t)((ui.trig_rec + 1) % 3);
                if (ui.trig_rec == 1) ui.trig_cur = 0;
                notice(ui.trig_rec == 1 ? "STEP RECORD: KEYS 1-4 = TRACKS A-D" : ui.trig_rec == 2 ? "LIVE RECORD: KEYS 1-4 = TRACKS A-D (PLAY)" : "RECORD OFF");
            } else if (slot_is(ui.sel, PW_T_SAMPLER)) {   /* SAMPLER: REC arms, REC again records IN into the RAM (a second at most), REC again stops */
                int st = pw_sampler_rec((unsigned)ui.sel, 1);
                notice(st < 0 ? "NO MEMORY FOR A RECORDING" : st == 1 ? "REC ARMED: PRESS REC TO RECORD IN (1 S)" : st == 2 ? "RECORDING: REC OR PLAY STOPS" : "STOPPING...");
            } else notice("HOLD REC TO DELETE");
        }
        pressed &= ~BIT(B_REC);
    }
    if (glo && (pressed & BIT(B_SEQ)) && ui.page == PG_RACK) {   /* GLO + SEQ: the patch view's size: AUTO (compact when the patch is big), COMPACT, FULL */
        ui.compact = (uint8_t)(ui.cmp_auto ? (ui.compact + 1) % 3 : ui.compact == 1 ? 0 : 1);   /* (AUTO COMPACT off: full size or compact; on: AUTO, COMPACT, FULL) */
        notice(ui.compact == 0 ? (ui.cmp_auto ? "PATCH VIEW: AUTO SIZE" : "PATCH VIEW: FULL SIZE") : ui.compact == 1 ? "PATCH VIEW: COMPACT" : "PATCH VIEW: FULL SIZE");
        ui.glo_used = 1; pressed &= ~BIT(B_SEQ);
    }
    if (glo && (pressed & BIT(B_SEQ)) && ui.page == PG_SEQ && find_seq() >= 0 && pws.mod[find_seq()].type == PW_T_PROLL) {   /* GLO + SEQ on the roll: its view */
        ui.roll_fold = (uint8_t)((ui.roll_fold + 1) % 3);
        notice(ui.roll_fold == 0 ? "ROLL VIEW: AUTO (FOLDS WHEN WIDE)" : ui.roll_fold == 1 ? "ROLL VIEW: FOLD (PITCHES WITH NOTES)" : "ROLL VIEW: ALL KEYS");
        ui.glo_used = 1; pressed &= ~BIT(B_SEQ);
    }
    if (pressed & BIT(B_SEQ)) {   /* SEQ: the step editor of the selected module, else the first; again: the next one in the patch */
        int on_it = ui.page == PG_SEQ || (ui.page == PG_EDIT && slot_is(ui.sel, PW_T_TRIGSEQ));
        int nx = on_it ? next_stepper(ui.sel) : -1;
        if (nx < 0) {
            int t = find_trig(), f = find_seq();
            nx = slot_is(ui.sel, PW_T_TRIGSEQ) ? ui.sel : f >= 0 ? f : t;
        }
        if (nx < 0) go(PG_SEQ);
        else if (pws.mod[nx].type == PW_T_TRIGSEQ) { ui.sel = nx; ui.edit_jacks = 0; ui.seq_rec = 0; go(PG_EDIT); }
        else { if (nx != ui.sel) ui.seq_rec = 0; ui.sel = nx; go(PG_SEQ); }
    }
    if ((pressed & BIT(B_PLAY)) && ui.page == PG_EDIT && slot_is(ui.sel, PW_T_SAMPLER) && pw_sampler_rec((unsigned)ui.sel, 0)) pw_sampler_rec((unsigned)ui.sel, -1);   /* (PLAY also ends a recording, or disarms) */
    if (ui.page == PG_EDIT) {   /* a recording that has ended: said once */
        static uint8_t prev; static int prev_s = -1;
        int sm = slot_is(ui.sel, PW_T_SAMPLER) ? ui.sel : -1, st = sm >= 0 ? pw_sampler_rec((unsigned)sm, 0) : 0;
        if (sm >= 0 && sm == prev_s && prev == 2 && st == 0) { notice(pw_sampler_rec_done((unsigned)sm) == 2 ? "NOTHING TO RECORD: PATCH A SOURCE INTO IN" : "RECORDED: SAVE THE PATCH TO KEEP IT"); pws.revision++; }
        prev = (uint8_t)st; prev_s = sm;
    }
    pw_prof_on = ui.page == PG_SETTINGS && ui.diag == 2;   /* (the CPU page times the modules while it is open) */
    if (pressed & BIT(B_PLAY)) pw_set_playing(!pws.playing);
    if (pressed & BIT(B_OCT_DOWN)) ui.octave = clampi(ui.octave - 1, -3, 3);
    if (pressed & BIT(B_OCT_UP)) ui.octave = clampi(ui.octave + 1, -3, 3);
    if ((pressed & BIT(B_SAVE)) && glo) {   /* GLO + SAVE: knob edits reach all snapshots, or only the current one */
        pws.edit_all = !pws.edit_all; pws.revision++;
        notice_glo(pws.edit_all ? "KNOBS EDIT ALL SNAPSHOTS" : "KNOBS EDIT THIS SNAPSHOT ONLY");
        pressed &= ~BIT(B_SAVE);
    }
    if (pressed & BIT(B_SAVE)) { save_pressed(); pressed &= ~BIT(B_SAVE); }
    /* PRESETS: the patch list, from anywhere (the first detent opens it) */
    if (c->encoders[E_PRESETS] && ui.page != PG_HELP && ui.page != PG_SETTINGS) {   /* (inside the menu's screens it does nothing: HOME is the way back) */
        int total = LIST_USER + PW_USER_SLOTS;
        if (ui.page != PG_PATCHES) go(PG_PATCHES);
        else list_step(c->encoders[E_PRESETS], glo);
        (void)total;
    }
    if (pressed & BIT(B_EDIT)) {
        if (ui.page == PG_PATCHES) { load_selected(); pressed = 0; }
        else if (ui.page == PG_ADD && !glo) { }   /* (ALGORITHM turns the panel's pages) */
        else if (ui.page == PG_ADD) {   /* (GLO + EDIT replaces the module there too) */
            int t = replace_target();
            unsigned ty = add_order[clampi(ui.add_cursor, 0, PW_T_SHOWN - 1)];
            if (t < 0) notice("NOTHING TO REPLACE HERE"); else replace_module(t, ty);
            pressed &= ~BIT(B_EDIT);
        }
        else if (ui.page == PG_CABLES) { int s = lst_slot(); if (s >= 0) { ui.sel = s; ui.edit_jacks = 0; ui.lst_pick = 0; go(PG_EDIT); } }
        else if (ui.page == PG_EDIT) { int sl = valid_sel(0); if (sl >= 0) ui.edit_jacks = face_next(sl); }
        else { ui.edit_jacks = 0; { int sl = ui.page == PG_RACK && pv_n ? ui.cur_slot : valid_sel(0); if (sl >= 0 && sl < PW_MAX_MODULES) ui.param_page[sl] = 0; } go(PG_EDIT); }   /* (the module's page opens on its first knobs, not the page of the jack the cursor was on) */
    }

    switch (ui.page) {
    case PG_RACK:
        if (ui.gsug) {   /* suggesting groups: SELECT turns through the chains, SEL groups the one outlined */
            int n = grp_cands(0);
            if (!n) { ui.gsug = 0; break; }
            if (c->encoders[E_SELECT]) ui.gsug_i = (int8_t)(((ui.gsug_i + c->encoders[E_SELECT]) % n + n) % n);
            ui.gsug_i = (int8_t)clampi(ui.gsug_i, 0, n - 1);
            if (pressed & BIT(B_SEL)) {
                grp_set set = grp_cand[ui.gsug_i];
                int g = grp_make(set), first = -1;
                for (int i = 0; i < PW_MAX_MODULES && first < 0; ++i) if (set >> i & 1u) first = i;
                ui.gsug = 0;
                if (g && first >= 0) { char msg[40], *o = s_cpy(msg, "GROUPED "); o = s_int(o, __builtin_popcountll((unsigned long long)set)); s_cpy(o, " MODULES (GLO+FX OPENS)"); notice(msg); pv_to_box(first); }
                else notice("TOO MANY GROUPS");
            }
            if (pressed & BIT(B_REC)) ui.gsug = 0;
            break;
        }
        if (c->encoders[E_SELECT]) {
            /* slowly jack to jack; fast (three detents or more, under 110 ms apart: about ten a second), module to
             * module, every two detents; GLO held, module to module at any speed */
            int raw = ui.raw_select ? ui.raw_select : c->encoders[E_SELECT], dir = raw > 0 ? 1 : -1, n = raw > 0 ? raw : -raw;
            if (dir != ui.sel_dir || now - ui.sel_ms > 110u) { ui.sel_n = 0; ui.sel_acc = 1; }
            ui.sel_dir = dir; ui.sel_ms = now; ui.sel_n += n;
            if (glo && pv_n) pv_step_module(c->encoders[E_SELECT]);
            else if (ui.sel_n >= 3 && pv_n) {
                for (ui.sel_acc += n; ui.sel_acc >= 2; ui.sel_acc -= 2) pv_step_module(dir);
            } else pv_step_jack(c->encoders[E_SELECT]);
        }
        if (c->encoders[E_ALGO]) {
            if (pv_n) { if (pv_col_of(ui.cur_slot)) sp_turn(ui.cur_slot, c->encoders[E_ALGO]); else turn_page(ui.cur_slot, c->encoders[E_ALGO]); }   /* ALGORITHM: the module's knob pages (a group: all its members') */
        }
        {   /* GLO + KNOB 1: the tempo (the header's number); the other knobs keep their fine turn under GLO */
            struct fm1_controls kc = *c;
            if (glo && !(ui.buttons & BIT(B_ARP)) && c->encoders[E_K1]) { pw_set_bpm(pws.bpm + (float)c->encoders[E_K1]); ui.tempo_ms = now; kc.encoders[E_K1] = 0; }
            if (pv_n && pws.mod[ui.cur_slot].used == 1) knobs_to_params(sp_member(ui.cur_slot, 0, 0, 0), &kc, fine);
        }
        if (pressed & BIT(B_SEL)) { ui.rsel_down = 1; ui.rsel_used = 0; }
        if (ui.rsel_down && (released & BIT(B_SEL))) { ui.rsel_down = 0; if (!ui.rsel_used) { if (pv_n) pv_sel(); else { ui.page_from_patch = 0; add_open(); } } }
        break;
    case PG_EDIT: {
        if (c->encoders[E_SELECT]) {
            if (ui.trig_rec == 1 && slot_is(ui.sel, PW_T_TRIGSEQ)) ui.trig_cur = ((ui.trig_cur + c->encoders[E_SELECT]) % 16 + 16) % 16;   /* (step record: skip a step) */
            else ui.sel = step_module(valid_sel(0), c->encoders[E_SELECT], 0);
        }
        ui.sel = valid_sel(0);
        if (glo && c->encoders[E_ALGO]) {   /* GLO + ALGORITHM: the module's faces (picture, INFO, DEMO) */
            int sl = valid_sel(0);
            if (sl >= 0) { int cur, nf = face_info(sl, &cur, &(const char *){0}); for (int k = 0, m = c->encoders[E_ALGO] > 0 ? 1 : nf - 1; k < m; ++k) ui.edit_jacks = face_next(sl); }
        } else turn_page(ui.sel, c->encoders[E_ALGO]);
        knobs_to_params(ui.sel, c, fine);
        if ((pressed & BIT(B_SEL)) && ui.sel >= 0 && pws.mod[ui.sel].type == PW_T_MUTATE && !ui.edit_jacks) { ui.page = PG_MUTATE; ui.mut_row = 0; ui.confirm_kind = 0; }   /* SEL on MUTATE: its page */
        else if ((pressed & BIT(B_SEL)) && ui.sel >= 0 && is_stepper(pws.mod[ui.sel].type) && ui.edit_jacks != 2) { ui.seq_rec = 0; go(PG_SEQ); }   /* SEL: this module's steps (the SEQUENCER, MOD SEQ, CHORD STEPS, GRAVITY, PIANO ROLL) */
        else if ((pressed & BIT(B_SEL)) && ui.sel >= 0) {
            if (ui.edit_jacks == 2 && pw_example_text(pws.mod[ui.sel].type)) {
                pw_example_build(pws.mod[ui.sel].type); ui.sel = first_module(); ui.user_slot = -1; ui.carrying = 0; ui.edit_jacks = 0;
                notice("PATCH LOADED: PLAY IT, THEN HOME"); go(PG_RACK);
            } else { go(PG_RACK); pv_goto(ui.sel, 0, 0); }
        }
    } break;
    case PG_ADD: {
        int d = 0, sv = c->encoders[E_SELECT];
        if (c->encoders[E_ALGO]) ui.add_page = (uint8_t)wrap_idx(ui.add_page + (c->encoders[E_ALGO] > 0 ? 1 : -1), 3);   /* ALGORITHM: the panel's pages */
        d += sv + c->encoders[E_K1];   /* (SELECT, and KNOB 1 the same, go module to module however fast they are turned; KNOB 2 jumps between categories) */
        if (c->encoders[E_K2]) {   /* KNOB 2: category to category (the strip says which) */
            ui.add_cursor = cat_jump(ui.add_cursor, c->encoders[E_K2] > 0 ? 1 : -1);
            char nb[40]; s_cpy(s_cpy(nb, "CATEGORY: "), pw_cat_names[pw_types[add_order[ui.add_cursor]].cat]); notice(nb);
        }
        for (int kn = 0; kn < 2; ++kn) {   /* KNOB 3: round the favorites (*); KNOB 4: round the last six added (R) */
            int e = c->encoders[kn ? E_K4 : E_K3];
            if (!e) continue;
            int at[14], n = kn ? fav_range(at, 8, 14) : fav_range(at, 0, 8), k = -1;
            for (int j = 0; j < n; ++j) if (at[j] == ui.add_cursor) k = j;
            if (!n) notice(kn ? "NO RECENTS YET: ADD A MODULE" : "NO FAVORITES YET: REC STARS ONE");
            else {
                int nk = wrap_idx(k < 0 ? (e > 0 ? 0 : n - 1) : k + (e > 0 ? 1 : -1), n);
                ui.add_cursor = at[nk];
                char nb[48], *o = s_cpy(nb, kn ? "RECENT " : "FAVORITE "); o = s_int(o, nk + 1); o = s_cpy(o, " OF "); o = s_int(o, n); o = s_cpy(o, ": "); s_cpy(o, pw_types[add_order[at[nk]]].name); notice(nb);
            }
        }
        ui.add_cursor = wrap_idx(ui.add_cursor + d, PW_T_SHOWN);
        if (pressed & BIT(B_REC)) {   /* REC: make this module a favorite (the star), or take the star off */
            unsigned ty = add_order[clampi(ui.add_cursor, 0, PW_T_SHOWN - 1)];
            if (is_fav(ty)) { for (unsigned k = 0; k < 8; ++k) if (ui.fav[k] == ty + 1u) { for (unsigned j = k; j < 7; ++j) ui.fav[j] = ui.fav[j + 1]; ui.fav[7] = 0; break; } notice("REMOVED FROM FAVORITES"); }
            else if (ui.fav[7]) notice("EIGHT FAVORITES: REMOVE ONE FIRST");
            else { unsigned k = 0; while (ui.fav[k]) ++k; ui.fav[k] = (uint8_t)(ty + 1u); notice(k < 4 ? "FAVORITE: GLO + OP KEY ADDS IT" : "FAVORITE"); }
            ui.set_dirty = 1;
        }
        if (pressed & BIT(B_SEL)) {
            unsigned type = add_order[clampi(ui.add_cursor, 0, PW_T_SHOWN - 1)];
            if (glo) {   /* GLO + SEL: replaces the module the patch cursor is on */
                int t = replace_target();
                if (t < 0) notice("NOTHING TO REPLACE HERE"); else replace_module(t, type);
                ui.glo_used = 1;
            }
            else if (ui.add_page != 1) add_type(type, 1);
            else if (!pw_example_text(type)) notice("NO EXAMPLE PATCH FOR THIS ONE YET");
            else {
                pw_example_build(type); ui.sel = first_module(); ui.user_slot = -1; ui.carrying = 0;
                notice("PATCH LOADED: PLAY IT, THEN HOME"); go(PG_RACK);
            }
        }
    } break;
    case PG_CABLES: {
        int d = c->encoders[E_SELECT] + c->encoders[E_ALGO];
        if (ui.lst_pick) ui.lst_pcur += d;
        else {
            int n = lst_build();
            ui.lst_cursor = clampi(ui.lst_cursor + d, 0, n ? n - 1 : 0);
            if (n && lrows[ui.lst_cursor].slot >= 0) ui.sel = lrows[ui.lst_cursor].slot;    /* (GLO + PLAY bypasses it; the add lists' EDIT replaces it) */
            if (n && lrows[ui.lst_cursor].cable >= 0 && c->encoders[E_K1]) {   /* KNOB 1: the cable's amount, -100..100 % */
                int k = lrows[ui.lst_cursor].cable;
                pw_cable_amount((unsigned)k, pws.cable[k].amt + c->encoders[E_K1] * (fine ? 1 : 5));
            }
            if (n && (pressed & BIT(B_REC))) {
                const lst_row *r = &lrows[ui.lst_cursor];
                if (r->cable >= 0) { pw_disconnect((unsigned)r->cable); notice("UNPLUGGED (GLO+OCT- UNDOES)"); }
                else if (r->cable == -1) delete_now(r->slot, 0);
            }
        }
        if (ui.lst_pick && (pressed & (BIT(B_REC) | BIT(B_HOME)))) {   /* back a step */
            /* back a step: the destination's input, its module, the source's output, the source module, out */
            ui.lst_pick = ui.lst_pick == 4 ? 0 : ui.lst_pick == 1 ? 4 : ui.lst_pick - 1;
            ui.lst_pcur = 0;
            if (ui.lst_pick == 4) { int ch[PW_MAX_MODULES + 16], m = lst_choices(ch); for (int i = 0; i < m; ++i) if (ch[i] == ui.lst_sm) ui.lst_pcur = i; }
            else if (ui.lst_pick == 1) ui.lst_pcur = ui.lst_so;
            else if (ui.lst_pick == 2) { int ch[PW_MAX_MODULES + 16], m = lst_choices(ch); for (int i = 0; i < m; ++i) if (ch[i] == ui.lst_dm) ui.lst_pcur = i; }
            pressed &= ~BIT(B_HOME);
        }
        /* SEL: tapped, the row's action; held, the place on the patch screen */
        if ((pressed & BIT(B_SEL)) && !glo) { ui.lst_sel_down = 1; ui.lst_sel_long = 0; ui.lst_sel_ms = now; }
        if (ui.lst_sel_down && (c->buttons & BIT(B_SEL)) && !ui.lst_sel_long && now - ui.lst_sel_ms >= 600u) { ui.lst_sel_long = 1; ui.lst_sel_down = 0; lst_long(); }
        if (ui.lst_sel_down && (released & BIT(B_SEL))) { ui.lst_sel_down = 0; lst_short(); }
    } break;
    case PG_SONG: song_input(c, pressed, glo); break;
    case PG_MUTATE: {
        int sl[PW_MAX_MODULES], n = mut_slots(sl);
        if (c->encoders[E_ALGO]) { ui.page = c->encoders[E_ALGO] > 0 ? PG_PERFORM : PG_LIVE; if (ui.page == PG_PERFORM) { ui.perf_cursor = 0; ui.perf_bank = 0; } else ui.live_row = 0; ui.confirm_kind = 0; break; }   /* ALGORITHM: the next page, or MUTES the other way */
        if (c->encoders[E_SELECT]) ui.mut_row = (uint8_t)clampi((int)ui.mut_row + c->encoders[E_SELECT], 0, n);
        if (c->encoders[E_K1]) ui.mut_amt = pw_clamp(ui.mut_amt + (float)c->encoders[E_K1] * (fine ? 0.005f : 0.05f), 0.0f, 1.0f);
        if (c->encoders[E_K2]) ui.mut_den = pw_clamp(ui.mut_den + (float)c->encoders[E_K2] * (fine ? 0.005f : 0.05f), 0.0f, 1.0f);
        if (pressed & BIT(B_REC)) mutate_go(1);
        if (pressed & BIT(B_SEL)) {   /* SEL: this module in or out; the first row: all in, or (when all are in) all out */
            if (ui.mut_row == 0) {
                unsigned locked = 0;
                for (int i = 0; i < n; ++i) locked += (unsigned)((pws.mut_lock >> sl[i]) & 1u);
                for (int i = 0; i < n; ++i) { if (locked) pws.mut_lock &= ~((uint64_t)1 << sl[i]); else pws.mut_lock |= (uint64_t)1 << sl[i]; }
            } else if (ui.mut_row <= n) pws.mut_lock ^= (uint64_t)1 << sl[ui.mut_row - 1];
            pws.revision++;
        }
    } break;
    case PG_LIVE: {
        int v[16], n = live_voices(v);
        if (c->encoders[E_ALGO]) {   /* ALGORITHM: the next page (MUTATE), or PERFORM the other way */
            if (c->encoders[E_ALGO] > 0) { ui.page = PG_MUTATE; ui.mut_row = 0; }
            else { ui.page = PG_PERFORM; ui.perf_cursor = 0; ui.perf_bank = 0; }
            ui.confirm_kind = 0; break;
        }
        if (c->encoders[E_SELECT]) ui.live_row = clampi(ui.live_row + c->encoders[E_SELECT], 0, live_pages(n) - 1);
        if (c->encoders[E_K1]) {   /* KNOB 1: MORPH */
            if (ui.morph_b == (int)pws.var_cur) { ui.morph_b = (int)((pws.var_cur + 1u) % PW_VARS); char msg[24], *o = s_cpy(msg, "MORPH TO S"); s_int(o, ui.morph_b + 1); notice(msg); }
            ui.morph = pw_clamp(ui.morph + (float)c->encoders[E_K1] * (fine ? 0.005f : 0.025f), 0.0f, 1.0f); pw_var_morph(pws.var_cur, (unsigned)ui.morph_b, ui.morph);
        }
        /* SEL tapped (not used as solo): Bar Sync on / off */
        if (pressed & BIT(B_SEL)) ui.live_sel_used = 0;
        if ((released & BIT(B_SEL)) && !ui.live_sel_used) { ui.var_quant ^= 1u; if (!ui.var_quant) ui.var_pend = -1; }
    } break;
    case PG_SEQ: {
        int s = find_seq();
        if (s < 0) { if (pressed & BIT(B_SEL)) { ui.page_from_patch = 0; add_open(); } break; }
        int ty = pws.mod[s].type;
        ui.seq_cursor = clampi(ui.seq_cursor + c->encoders[E_SELECT], 0, 15);
        pw_module *mm = &pws.mod[s];
        if (ty == PW_T_SEQ) {
            int dn = c->encoders[E_K1] + c->encoders[E_ALGO] * 12;
            if (dn) { pw_undo_data((unsigned)s); mm->data[ui.seq_cursor] = (uint8_t)(int8_t)clampi((int8_t)mm->data[ui.seq_cursor] + dn, -36, 36); pws.revision++; }
            uint8_t *fb = &mm->data[PW_SEQ_STEPFEAT + ui.seq_cursor], *vl = &mm->data[PW_SEQ_STEPVEL + ui.seq_cursor];
            if (c->encoders[E_K2]) {   /* KNOB 2: ratchet x1-x4, then TIE, then GLIDE (one or the other: they share the step's feature byte) */
                pw_undo_data((unsigned)s);
                int v = seq_legato_of(*fb) ? 3 + seq_legato_of(*fb) : (*fb & 3);
                v = clampi(v + c->encoders[E_K2], 0, 5);
                *fb = (uint8_t)((*fb & 0xf0) | (v <= 3 ? v : (unsigned)(v - 3) << 2));
                pws.revision++;
            }
            if (c->encoders[E_K3]) { pw_undo_data((unsigned)s); *fb = (uint8_t)((*fb & 0x0f) | clampi((*fb >> 4) + c->encoders[E_K3], 0, 15) << 4); pws.revision++; }
            if (c->encoders[E_K4]) {   /* KNOB 4: velocity; 0 is off, and turning up from 0 turns the step back on */
                pw_undo_data((unsigned)s);
                int was0 = *vl == 0;
                *vl = (uint8_t)clampi(*vl + c->encoders[E_K4] * (fine ? 1 : 8), 0, 255);
                if (was0 && *vl) { uint16_t g = (uint16_t)(mm->data[16] | (mm->data[17] << 8)) | (uint16_t)(1u << ui.seq_cursor); mm->data[16] = (uint8_t)g; mm->data[17] = (uint8_t)(g >> 8); }
                pws.revision++;
            }
            if (pressed & BIT(B_SEL)) {   /* SEL: the cursor's step on / off (a step at velocity 0 comes on at 200) */
                pw_undo_data((unsigned)s);
                uint16_t g = (uint16_t)(mm->data[16] | (mm->data[17] << 8));
                if (!*vl) { *vl = 200; g |= (uint16_t)(1u << ui.seq_cursor); }
                else g ^= (uint16_t)(1u << ui.seq_cursor);
                mm->data[16] = (uint8_t)g; mm->data[17] = (uint8_t)(g >> 8);
                pws.revision++;
            }
        } else if (ty == PW_T_MODSEQ) {
            for (unsigned l = 0; l < 4; ++l) {   /* KNOB 1-4: the cursor step's lanes A-D (LEN, MODE, SLEW, RANGE: the module page) */
                int d = c->encoders[E_K1 + l];
                if (!d) continue;
                pw_undo_data((unsigned)s);
                pw_mseq_set(mm, l, (unsigned)ui.seq_cursor, (int)pw_mseq_get(mm, l, (unsigned)ui.seq_cursor) + d * (fine ? 4 : 16));
                pws.revision++;
            }
            if (pressed & BIT(B_SEL)) {   /* SEL: a new random column */
                static uint32_t rs = 0x1234567u;
                pw_undo_data((unsigned)s);
                for (unsigned l = 0; l < 4; ++l) { rs = rs * 1664525u + 1013904223u; pw_mseq_set(mm, l, (unsigned)ui.seq_cursor, (int)(rs >> 24)); }
                pws.revision++;
            }
        } else if (ty == PW_T_CHORDSTEPS) {
            uint8_t *b = &mm->data[ui.seq_cursor];
            if (c->encoders[E_K1]) { pw_undo_data((unsigned)s); *b = (uint8_t)((*b & 0xf0) | clampi((*b & 15) + c->encoders[E_K1], 0, 8)); pws.revision++; }
            if (c->encoders[E_K2]) { pw_undo_data((unsigned)s); *b = (uint8_t)((*b & 0xcf) | clampi(((*b >> 4) & 3) + c->encoders[E_K2], 0, 3) << 4); pws.revision++; }
            if (c->encoders[E_K3]) { pw_undo_data((unsigned)s); *b = (uint8_t)((*b & 0x3f) | clampi((*b >> 6) + c->encoders[E_K3], 0, 3) << 6); pws.revision++; }
            if (pressed & BIT(B_SEL)) {   /* SEL: REST, or the chord back again */
                static uint8_t kept[16];
                pw_undo_data((unsigned)s);
                if ((*b & 15) == 0) *b = kept[ui.seq_cursor & 15] ? kept[ui.seq_cursor & 15] : (uint8_t)1;
                else { kept[ui.seq_cursor & 15] = *b; *b = (uint8_t)(*b & 0xf0); }
                pws.revision++;
            }
        } else if (ty == PW_T_GRAVITY) {
            int dn = c->encoders[E_K1] + c->encoders[E_ALGO] * 12;
            if (dn) { pw_undo_data((unsigned)s); mm->data[ui.seq_cursor] = (uint8_t)(int8_t)clampi((int8_t)mm->data[ui.seq_cursor] + dn, -36, 36); pws.revision++; }
            if (c->encoders[E_K2]) pw_nudge_param((unsigned)s, 2, c->encoders[E_K2], fine);
            if (c->encoders[E_K3]) pw_nudge_param((unsigned)s, 1, c->encoders[E_K3], fine);
            if (pressed & BIT(B_SEL)) pw_kick((unsigned)s);   /* SEL: drop the ball now */
        } else if (ty == PW_T_PROLL) {
            /* SELECT: along the grid (by the snap); ALGORITHM: up and down the keys (GLO: octaves); SEL: a note here, or take it away;
             * KNOB 1 its length, KNOB 2 its velocity, KNOB 3 moves it along, KNOB 4 the loop (BARS). With no note under the cursor KNOB 1
             * sets the length the next one gets. */
            roll_init(mm);
            int snap = roll_snap(mm), loop = (int)pw_roll_loop(mm), d;
            if ((d = c->encoders[E_SELECT])) ui.roll_t = clampi(ui.roll_t + d * (fine ? 1 : snap), 0, loop - 1);
            if ((d = c->encoders[E_ALGO])) {
                if (glo) ui.roll_p = clampi(ui.roll_p + d * 12, 0, 127);
                else for (int k = 0, nn = d < 0 ? -d : d; k < nn; ++k) {   /* (folded: from pitch to pitch that has a note) */
                    int p2 = ui.roll_p + (d > 0 ? 1 : -1);
                    if (roll_folded(mm)) {
                        int best = -1;
                        for (unsigned q = 0; q < pw_roll_count(mm); ++q) { unsigned pp; pw_roll_get(mm, q, 0, 0, &pp, 0, 0); if (d > 0 ? ((int)pp > ui.roll_p && (best < 0 || (int)pp < best)) : ((int)pp < ui.roll_p && (best < 0 || (int)pp > best))) best = (int)pp; }
                        if (best >= 0) p2 = best;
                    }
                    ui.roll_p = clampi(p2, 0, 127);
                }
            }
            int idx = pw_roll_find(mm, (unsigned)ui.roll_t, (unsigned)ui.roll_p);
            unsigned st = 0, ln = 0, pt = 0, vl = 0, vo = 0;
            if (idx >= 0) pw_roll_get(mm, (unsigned)idx, &st, &ln, &pt, &vl, &vo);
            if ((d = c->encoders[E_K1])) {
                int nl = (idx >= 0 ? (int)ln : ui.roll_len) + d * (fine ? 1 : snap);
                if (nl < 1) nl = 1;
                if (idx >= 0) { pw_undo_data((unsigned)s); if (pw_roll_edit(mm, (unsigned)idx, -1, nl, -1) == 0) { ui.roll_len = nl; pws.revision++; } else notice("NO ROOM: FOUR NOTES AT ONCE"); }
                else ui.roll_len = clampi(nl, 1, 64);
            }
            if ((d = c->encoders[E_K2]) && idx >= 0) {   /* the note's pitch (GLO: an octave) */
                pw_undo_data((unsigned)s);
                int np = clampi((int)pt + d * (glo ? 12 : 1), 0, 127);
                if (pw_roll_pitch(mm, (unsigned)idx, (unsigned)np) == 0) { ui.roll_p = np; pws.revision++; } else notice("THAT PITCH IS TAKEN HERE");
            }
            if ((d = c->encoders[E_K3]) && idx >= 0) {
                pw_undo_data((unsigned)s);
                int ns = clampi((int)st + d * (fine ? 1 : snap), 0, loop - 1);
                if (pw_roll_edit(mm, (unsigned)idx, ns, -1, -1) == 0) { ui.roll_t = ns + (ui.roll_t - (int)st); pws.revision++; } else notice("NO ROOM: FOUR NOTES AT ONCE");
            }
            if ((d = c->encoders[E_K4])) {
                if (glo) pw_nudge_param((unsigned)s, 0, d, 0);   /* GLO + KNOB 4: the loop (BARS) */
                else if (idx >= 0) { pw_undo_data((unsigned)s); pw_roll_edit(mm, (unsigned)idx, -1, -1, clampi((int)vl + d * 2, 0, 31)); pws.revision++; }
            }
            if ((pressed & BIT(B_SEL)) && !glo) {
                pw_undo_data((unsigned)s);
                if (idx >= 0) pw_roll_del(mm, (unsigned)idx);
                else if (pw_roll_add(mm, (unsigned)ui.roll_t, (unsigned)ui.roll_len, (unsigned)ui.roll_p, 24) < 0) notice(pw_roll_count(mm) >= PW_ROLL_MAX ? "ROLL IS FULL (30 NOTES)" : "FOUR NOTES AT ONCE AT MOST");
                pws.revision++;
            }
        }
        if (pressed & BIT(B_REC)) {   /* REC: step record, then live record, then off (GRAVITY: step record only; MOD SEQ: none) */
            int modes = ty == PW_T_MODSEQ ? 1 : ty == PW_T_GRAVITY ? 2 : 3;
            ui.seq_rec = (ui.seq_rec + 1) % modes; ui.roll_ms = 0;
            notice(modes == 1 ? "NOTHING TO RECORD HERE" : ui.seq_rec == 1 ? "STEP RECORD: PLAY NOTES" : ui.seq_rec == 2 ? "LIVE RECORD: PLAY OVER THE LOOP (PLAY)" : "RECORD OFF");
        }
        ui.sel = s;
    } break;
    case PG_PATCHES: {
        int total = LIST_USER + PW_USER_SLOTS;
        if (c->encoders[E_SELECT]) list_step(c->encoders[E_SELECT], glo);
        if (c->encoders[E_ALGO]) { if (c->encoders[E_ALGO] > 0) save_open(); else { ui.help_scroll = 0; go(PG_HELP); } break; }   /* ALGORITHM: the next tab, SETTINGS, or HELP the other way (GLO + SELECT jumps between the list's parts) */
        (void)total;
        if (c->encoders[E_K1]) { pw_set_bpm(pws.bpm + (float)c->encoders[E_K1] * (fine ? 0.1f : 1.0f)); ui.tempo_ms = now; }
        name_knobs(c);
        if (c->encoders[E_K4]) pw_set_voices((unsigned)clampi((int)pws.voices + (c->encoders[E_K4] > 0 ? 1 : -1), 1, PW_MAX_VOICES));
        if (pressed & BIT(B_SEL)) load_selected();
    } break;
    case PG_SAVE: {
        if (c->encoders[E_SELECT]) ui.save_cur = (int8_t)wrap_idx(ui.save_cur + (c->encoders[E_SELECT] > 0 ? 1 : -1), PW_USER_SLOTS);
        if (c->encoders[E_ALGO]) { if (c->encoders[E_ALGO] > 0) { ui.set_row = 0; ui.diag = 0; go(PG_SETTINGS); } else go(PG_PATCHES); break; }
        if (c->encoders[E_K1]) { pw_set_bpm(pws.bpm + (float)c->encoders[E_K1] * (fine ? 0.1f : 1.0f)); ui.tempo_ms = now; }
        name_knobs(c);
        if (c->encoders[E_K4]) ui.save_group = c->encoders[E_K4] > 0;
        if (pressed & BIT(B_SEL)) save_store();
    } break;
    case PG_HELP:
        if (c->encoders[E_ALGO]) { if (c->encoders[E_ALGO] > 0) go(PG_PATCHES); else { ui.set_row = 0; ui.diag = 0; go(PG_SETTINGS); } break; }
        { int d = c->encoders[E_SELECT]; ui.help_scroll += d * 13 + (d > 2 || d < -2 ? d * 13 : 0); }   /* a line a detent, faster when it is spun */
        break;
    case PG_SETTINGS: {
        if (ui.diag) {   /* (the diagnostics pages only show; ALGORITHM turns between them, HOME leaves) */
            if (c->encoders[E_ALGO]) ui.diag = (uint8_t)(ui.diag % 3 + 1);
            break;
        }
        if (c->encoders[E_ALGO]) { if (c->encoders[E_ALGO] > 0) { ui.help_scroll = 0; go(PG_HELP); } else go(PG_SAVE); break; }   /* ALGORITHM: the next tab, HELP, or SAVE the other way */
        if (c->encoders[E_SELECT]) ui.set_row = (uint8_t)wrap_idx(ui.set_row + (c->encoders[E_SELECT] > 0 ? 1 : -1), S_N);
        if (c->encoders[E_K1]) set_change(ui.set_row, c->encoders[E_K1] > 0 ? 1 : -1);
        if (pressed & BIT(B_SEL)) {
            if (ui.set_row == S_DIAG) ui.diag = 1;
            else if (ui.set_row == S_RESET) { if (confirm(3, 0, ui.reset_all ? "SEL AGAIN: RESET SETTINGS AND PATCHES" : "SEL AGAIN: RESET SETTINGS")) factory_reset(); }
            else if (ui.set_row == S_RATE) set_change(S_RATE, ui.rate_i == 2 ? -2 : 1);
            else set_change(ui.set_row, (ui.set_row == S_PAL || ui.set_row == S_VEL || ui.set_row == S_TONE || ui.set_row == S_BRIGHT) ? 1 : -1 + 2 * !(ui.set_row == S_LATCH ? ui.glo_latch : ui.set_row == S_AUTO ? ui.autosave : ui.set_row == S_MCLK ? ui.mclk_i : ui.set_row == S_PCH ? ui.pch_i : ui.set_row == S_CMP ? ui.cmp_auto : ui.set_row == S_CTL ? ui.ctl_i : ui.set_row == S_CORE ? ui.core_i : ui.set_row == S_DEL ? ui.smart_i : ui.edit_def));
        }
    } break;
    case PG_PERFORM: {
        if (c->encoders[E_SELECT]) { ui.perf_cursor = clampi(ui.perf_cursor + c->encoders[E_SELECT], 0, PW_PERF); if (ui.perf_cursor < PW_PERF) ui.perf_bank = ui.perf_cursor / 4; }
        if (c->encoders[E_ALGO]) {   /* ALGORITHM: the next page (MUTES), or MUTATE the other way */
            if (c->encoders[E_ALGO] > 0) { ui.page = PG_LIVE; ui.live_row = 0; }
            else { ui.page = PG_MUTATE; ui.mut_row = 0; }
            ui.confirm_kind = 0; break;
        }
        if (ui.perf_cursor == PW_PERF && c->encoders[E_K1]) {   /* KNOB 1 flips it too, like a knob would */
            ui.var_quant = c->encoders[E_K1] > 0; if (!ui.var_quant) ui.var_pend = -1;
        }
        for (int k = 0; k < 4; ++k) {
            const uint8_t *a = pws.perf[ui.perf_bank * 4 + k];
            if (c->encoders[E_K1 + k] && a[0] != 0xff) { pw_nudge_param(a[0], a[1], c->encoders[E_K1 + k], fine); song_record(a[0], a[1]); ui.perf_cursor = ui.perf_bank * 4 + k; }
        }
        if ((pressed & BIT(B_REC)) && ui.perf_cursor < PW_PERF) { pw_perf_clear((unsigned)ui.perf_cursor); notice("TAKEN OFF THE PAGE"); }
        /* SEL held while a MIDI knob moves: that CC drives the knob under the cursor; SEL tapped: its module */
        if (pressed & BIT(B_SEL)) { ui.learn_serial = pws.cc_serial; ui.learn_used = 0; }
        if ((c->buttons & BIT(B_SEL)) && pws.cc_serial != ui.learn_serial) {
            ui.learn_serial = pws.cc_serial; ui.learn_used = 1;
            if (ui.perf_cursor < PW_PERF) pws.perf_cc[ui.perf_cursor] = pws.last_cc;
            pws.revision++;
            char msg[32], *o = s_cpy(msg, "KNOB "); o = s_int(o, ui.perf_cursor + 1); o = s_cpy(o, " FOLLOWS CC "); s_int(o, pws.last_cc);
            notice(msg);
        }
        if ((released & BIT(B_SEL)) && !ui.learn_used && ui.perf_cursor == PW_PERF) {
            ui.var_quant ^= 1u; if (!ui.var_quant) ui.var_pend = -1;
        } else if ((released & BIT(B_SEL)) && !ui.learn_used && ui.perf_cursor < PW_PERF && pws.perf[ui.perf_cursor][0] != 0xff) { ui.sel = pws.perf[ui.perf_cursor][0]; ui.edit_jacks = 0; go(PG_EDIT); }
    } break;
    default: break;
    }
    if (glo_latched_use && used_now) ui.glo_on = 0;   /* the latch is spent on that key */
}
static void update_leds(void)
{
    uint32_t b = 0, k = 0, kd = 0;
    static const uint8_t page_button[] = {0, B_HOME, B_EDIT, B_FX, B_ARP, 0xff, B_SEQ, 0xff, B_GLO, B_ENV, B_ENV, B_LFO, 0xff};
    if (ui.page != PG_BOOT && page_button[ui.page] != 0xff) b |= BIT(page_button[ui.page]);
    if (pws.playing) b |= BIT(B_PLAY);
    if (ui.seq_rec || (ui.page == PG_EDIT && ui.trig_rec) || (ui.rec_warned && !ui.rec_used)) b |= BIT(B_REC);
    if (ui.confirm_kind == 2) b |= BIT(B_SAVE);
    if (ui.buttons & BIT(B_GLO)) b |= BIT(B_GLO);
    if (ui.octave < 0) b |= BIT(B_OCT_DOWN);
    if (ui.octave > 0) b |= BIT(B_OCT_UP);
    if (ui.page == PG_RACK && ui.carrying) b |= BIT(B_SEL) | BIT(B_REC);
    if (ui.page == PG_SEQ && find_seq() >= 0) {
        int s = find_seq(), play = (int)pw_live((unsigned)s, 0);
        uint16_t g = (uint16_t)(pws.mod[s].data[16] | (pws.mod[s].data[17] << 8));
        if (pws.mod[s].type == PW_T_CHORDSTEPS) { g = 0; for (int i = 0; i < 16; ++i) if ((pws.mod[s].data[i] & 15) >= 1) g |= (uint16_t)(1u << i); }
        else if (pws.mod[s].type != PW_T_SEQ) g = 0xffffu;
        if (pws.mod[s].type == PW_T_PROLL) { g = 0; play = -1; }   /* (the keys play notes here) */
        if (pws.mod[s].type == PW_T_MODSEQ || pws.mod[s].type == PW_T_GRAVITY) play = pws.mod[s].type == PW_T_MODSEQ ? play : -1;
        for (int i = 0; i < 16; ++i) {
            int on = (g >> i) & 1;
            if (i == play && pws.playing) on = !on;
            if (on) k |= 1u << white_key[i];
        }
    } else if (ui.page == PG_PERFORM) {
        k = 1u << white_key[pws.var_cur];
    } else if (ui.page == PG_EDIT && ui.sel >= 0 && pws.mod[ui.sel].used == 1 && pws.mod[ui.sel].type == PW_T_TRIGSEQ) {
        int at = (int)pw_live((unsigned)ui.sel, (unsigned)ui.trig_track);
        for (int i = 0; i < 16; ++i) {
            int on = pw_trig_get((unsigned)ui.sel, (unsigned)ui.trig_track, (unsigned)i) != 0;
            if (i == at && pws.playing) on = !on;
            if (on) k |= 1u << white_key[i];
        }
        k |= 1u << black_key[ui.trig_track & 3];
    } else k = ui.keys;
    /* the start of each bar glows softly (steps 1, 5, 9, 13), where the keys are steps */
    if (ui.page == PG_SEQ && find_seq() >= 0 && (pws.mod[find_seq()].type == PW_T_SEQ || pws.mod[find_seq()].type == PW_T_MODSEQ || pws.mod[find_seq()].type == PW_T_CHORDSTEPS)) kd = 1;
    else if (ui.page == PG_EDIT && ui.sel >= 0 && pws.mod[ui.sel].used == 1 && pws.mod[ui.sel].type == PW_T_TRIGSEQ) kd = 1;
    if (kd) { kd = 0; for (int i = 0; i < 16; i += 4) kd |= 1u << white_key[i]; }
    if (ui.leds_valid && b == ui.led_buttons && k == ui.led_keys && kd == ui.led_dim) return;
    for (unsigned i = 0; i < 14; ++i)
        if (!ui.leds_valid || ((b ^ ui.led_buttons) >> i & 1)) fm1_led_set(i, (b >> i) & 1);
    for (unsigned i = 0; i < 27; ++i)
        if (!ui.leds_valid || ((k ^ ui.led_keys) >> i & 1)) fm1_led_set(14 + i, (k >> i) & 1);
    for (unsigned i = 0; i < 27; ++i)
        if (!ui.leds_valid || ((kd ^ ui.led_dim) >> i & 1)) patchwerk_led_dim(14 + i, (kd >> i) & 1);
    ui.led_buttons = b; ui.led_keys = k; ui.led_dim = kd; ui.leds_valid = 1;
}

/* --------------------------------------------------------------- entries */
void pw_ui_init(void)
{
    memset(&ui, 0, sizeof ui);
    ui.mut_amt = 0.25f; ui.mut_den = 0.3f; ui.mut_last = -1;
    ui.last_mod = 0xff;
    build_add_order();
    ui.page = PG_BOOT;
    ui.sel = -1;
    ui.user_slot = -1;
    ui.probe_slot = -1;
    ui.var_pend = -1;

    ui.song_s16 = 0xffffffffu;
    ui.boot_ms = fm1_millis() - anim_ms;   /* (the boot page goes on from where the splash's animation stopped) */
    ui.boot_end = ui.boot_ms + 1800u;   /* (1.8 s of animation and hold in all: from where the splash left off, so the logo is never cut short) */
    band_valid = 0;
    settings_load();
    pw_store_init();
    /* Start with user slot 1 when it holds a patch, else the first factory patch. HOME held at power-on
     * starts with a blank patch (an OUTPUT alone) and without the second core: the way out if a saved patch
     * ever crashed the synth at every start (nothing is deleted; patches survive a firmware reinstall). */
    struct fm1_controls held;
    fm1_controls_read(&held);
    if (held.buttons & BIT(B_HOME)) {
        pw_clear(); pw_set_playing(0); pw_random_name(pws.name);
        int x, y; free_spot(&x, &y); pw_add(PW_T_OUTPUT, x, y);
        notice("HOME HELD: BLANK PATCH, U1 SKIPPED"); ui.notice_until = fm1_millis() + 4000u;
    }
    else if (!pw_store_name(0)) pw_factory_build(0);
    else if (pw_store_load(0) < 0) { pw_factory_build(0); notice("U1 WOULD NOT LOAD: FACTORY PATCH"); ui.notice_until = fm1_millis() + 4000u; }
    else { ui.user_slot = 0; char m[40], *o = s_cpy(m, "STARTED WITH U1: "); s_cpy(o, pws.name); notice(m); ui.notice_until = fm1_millis() + 4000u; }
    if (!(held.buttons & BIT(B_HOME))) core_apply(1);   /* (HOME held at power-on: one start without the second core) */
    ui.sel = first_module();
    ui.list_cursor = ui.user_slot >= 0 ? LIST_USER + ui.user_slot : LIST_TOP;
    ui.last_list = ui.list_cursor;
    ui.solo_slot = -1; ui.morph_b = 1; ui.lst_cable = -1;
}
void pw_ui_redraw_all(void) { band_valid = 0; }
void pw_ui_init_noboot(void)
{
    pw_ui_init();
    ui.page = PG_RACK;
    ui.probe_slot = -1;
}
int pw_ui_page(void) { return (int)ui.page; }
#ifdef PLAT_HOST
int pw_ui_gview(void) { return ui.gview; }                  /* (tests) the group opened, 0: the top */
int pw_ui_carrying(void) { return ui.carrying; }            /* (tests) a cable in hand */
#endif
void pw_ui_select(int slot)
{
    if (slot < 0 || slot >= PW_MAX_MODULES || pws.mod[slot].used != 1) return;
    ui.sel = slot;
    if (ui.page == PG_RACK && !ui.carrying) pv_goto(slot, 0, 0);
}
int pw_ui_slot(void) { return ui.page == PG_RACK ? ui.cur_slot : ui.sel; }
int pw_ui_addcur(void) { return ui.add_cursor; }
int pw_ui_rate_drop(void)   /* the CPU overloaded: AUTO goes to 22.05 kHz (1: it just did) */
{
    if (ui.rate_i != 2 || pw_rate_half()) return 0;
    pw_set_rate(1);
    return 1;
}
static void program_change_tick(void)   /* a MIDI program change in PATCH mode: 0-7 the user patches U1-U8, 8-57 the factory patches */
{
    if (!pws.pc_req) return;
    unsigned n = (unsigned)pws.pc_req - 1u;
    pws.pc_req = 0;
    char msg[40], *o;
    if (n < PW_USER_SLOTS) {
        int r = pw_store_load(n);
        if (r < 0) { o = s_cpy(msg, "PROGRAM "); o = s_int(o, (int)n); s_cpy(o, ": U SLOT EMPTY"); notice(msg); return; }
        ui.user_slot = (int)n;
    } else if (n - PW_USER_SLOTS < PW_FACTORY_COUNT) { pw_factory_build(n - PW_USER_SLOTS); ui.user_slot = -1; }
    else return;
    ui.var_pend = -1; ui.sel = first_module(); ui.carrying = 0; ui.gview = 0;
    o = s_cpy(msg, "PROGRAM "); o = s_int(o, (int)n); o = s_cpy(o, ": "); s_cpy(o, pws.name); notice(msg);
    go(PG_RACK);
}
void pw_ui_control(void)
{
    pw_maintain();
    song_tick();
    program_change_tick();
    { static uint32_t seen_clear = 0xFFFFFFFFu;   /* a new patch: every module's knobs start on page 1 */
      if (seen_clear != pws.clear_serial) {
          seen_clear = pws.clear_serial;
          if (ui.rate_i == 2 && pw_rate_half()) pw_set_rate(0);   /* (AUTO went down to 22.05 kHz for the last patch: this one starts at 44.1) */
          for (int i = 0; i < PW_MAX_MODULES; ++i) { ui.param_page[i] = 0; ui.page_type[i] = 0; }
          ui.var_pend = -1; ui.song_s16 = 0xffffffffu; ui.last_mod = 0xff; ui.song_pick = 0; ui.song_pcarry = 0;
          if (ui.song_loading) ui.song_loading = 0;   /* (a patch clip's load: the song and its cursor stay) */
          else { ui.song_carry = 0; ui.song_bar = 0; ui.song_vs = 0; ui.song_lane = 0; ui.song_chain = 0; }
      } }
    struct fm1_controls c;
    fm1_controls_read(&c);
    uint32_t now = fm1_millis();
    ui.raw_select = c.encoders[E_SELECT];
    /* gentle acceleration for fast turns */
    for (int i = 0; i < 7; ++i) {
        int d = c.encoders[i];
        if (d > 2 || d < -2) c.encoders[i] = (int16_t)(d * 3);
    }
    int changed = 0;
    unsigned level = pw_plat_master(c.knob, &changed);
    if (changed && ui.page != PG_BOOT) {
        char msg[24], *o = s_cpy(msg, "MASTER "); s_int(o, (int)level);
        if (!level) s_cpy(msg, "MASTER MUTED");
        notice(msg); ui.notice_until = now + 350u;   /* (gone as soon as the knob stops) */
    }
    int input = c.buttons != ui.buttons || c.keys != ui.keys;
    for (int i = 0; i < 7; ++i) input |= c.encoders[i] != 0;
    if (ui.page == PG_BOOT) {
        if ((int32_t)(now - ui.boot_end) >= 0 || (input && now - ui.boot_ms > 150u)) {
            ui.page = PG_RACK;
            ui.buttons = c.buttons; /* swallow the waking gesture */
            ui.keys = c.keys;
            band_valid = 0;
        }
    } else {
        handle(&c);
        lane_rec_tick(&c, now);
        ui.buttons = c.buttons;
        handle_keys(c.keys, c.vel, c.press);
        autosave_tick(now);
    }
    ui.buttons = c.buttons;
    update_leds();
    if (now - ui.frame_ms < 33u && !input) return;
    ui.frame_ms = now;
    if (ui.page == PG_BOOT) draw_boot(now - ui.boot_ms);
    else {
        ky_off = 0;
        header();
        switch (ui.page) {
        case PG_RACK: draw_patchview(); break;
        case PG_ADD: draw_add(); break;
        case PG_EDIT: draw_edit(); break;
        case PG_CABLES: draw_list(); break;
        case PG_LIVE: draw_live(); break;
        case PG_SONG: draw_song(); break;
        case PG_SEQ: draw_seq(); break;
        case PG_PATCHES: draw_patches(); break;
        case PG_SAVE: draw_save(); break;
        case PG_HELP: draw_help(); break;
        case PG_SETTINGS: if (ui.diag) draw_diag(); else draw_settings(); break;
        case PG_PERFORM: draw_perform(); break;
        case PG_MUTATE: draw_mutate(); break;
        default: break;
        }
        strip_draw();
    }
    present();
}
