/* SPDX-License-Identifier: MPL-2.0 */
/* BETA: what the update loader shows on the FM-1's screen. The loader had no screen: an FM-1 sent back to it after a cut-off
 * install (or waiting in it for any reason) was black, and looked dead. This draws, once the loader has waited a while with no
 * install running, "UPDATE MODE / open the installer page and press Install", and "UPDATING / do not unplug" while one runs.
 * Self-contained (the panel's init is device/lcd.c's, copied small), blocking but bounded (the LCD hardware layer has
 * timeouts), RAM only, drawn a line at a time from a 10 KB buffer. A bug here would stall the recovery path, so it is kept
 * small, is only reached from the idle loop, and feeds the watchdog between steps. */
#ifndef LDR_SCREEN_H
#define LDR_SCREEN_H
#include "fm1_lcd_hw.h"
#include "pw_brand.h"            /* the brand colors (brand/brand.json, tools/gen_brand.sh) */

/* a color as the panel wants it: RGB565, high byte first */
#define LS_565(r, g, b) ((uint16_t)((((uint32_t)(r) >> 3) << 11 | ((uint32_t)(g) >> 2) << 5 | (uint32_t)(b) >> 3) << 8 & 0xFF00u | (((uint32_t)(r) >> 3) << 11 | ((uint32_t)(g) >> 2) << 5 | (uint32_t)(b) >> 3) >> 8))
#define LS_565V(...) LS_565(__VA_ARGS__)
#define LS_ACCENT LS_565V(BR_ACCENT)
#define LS_WHITE 0xFFFFu
static uint16_t ls_buf[240 * 21] __attribute__((aligned(4)));   /* 3x text: 7 rows * 3 */
static uint8_t ls_small[8] __attribute__((aligned(4)));

static void ls_cmd(uint8_t c) { fm1_lcd_send_cmd(c); fm1_lcd_wait(); fm1_lcd_deselect(); }
static void ls_data(const void *p, uint32_t n) { fm1_lcd_send_data(p, n); fm1_lcd_wait(); fm1_lcd_deselect(); }
static void ls_window(uint32_t y0, uint32_t y1)           /* full width, rows y0..y1 */
{
    ls_small[0] = 0; ls_small[1] = 0; ls_small[2] = 0; ls_small[3] = 239;
    ls_cmd(0x2A); ls_data(ls_small, 4);
    ls_small[4] = (uint8_t)(y0 >> 8); ls_small[5] = (uint8_t)y0; ls_small[6] = (uint8_t)(y1 >> 8); ls_small[7] = (uint8_t)y1;
    ls_cmd(0x2B); ls_data(ls_small + 4, 4);
    ls_cmd(0x2C);
}

/* 5x7, one byte a column, bit 0 the top row; the standard set: space . - : ! 0-9 A-Z % */
static const uint8_t *ls_glyph(char c)
{
    static const uint8_t F[][5] = {
        {0x00,0x00,0x00,0x00,0x00},{0x00,0x60,0x60,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x36,0x36,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},
        {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
        {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
        {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
        {0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},
        {0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
        {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
        {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},
        {0x61,0x51,0x49,0x45,0x43},{0x23,0x13,0x08,0x64,0x62},
    };
    static const char K[] = " .-:!0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%";
    uint32_t i;
    for (i = 0; K[i]; i++)
        if (K[i] == c)
            return F[i];
    return F[0];
}

static void ls_clear_rows(uint32_t y0, uint32_t rows)         /* black */
{
    uint32_t i;
    for (i = 0; i < 240u * rows; i++)
        ls_buf[i] = 0;
    ls_window(y0, y0 + rows - 1u);
    ls_data(ls_buf, 240u * rows * 2u);
}

static void ls_text(uint32_t y, const char *s, uint32_t sc, uint16_t fg)   /* centred, fg already byte-swapped */
{
    uint32_t n = 0, i, k, col, r, sy, sx, h = 7u * sc, w, x0;
    while (s[n])
        n++;
    w = n * 6u * sc - sc;
    x0 = (240u - w) / 2u;
    for (i = 0; i < 240u * h; i++)
        ls_buf[i] = 0;
    for (k = 0; k < n; k++) {
        const uint8_t *g = ls_glyph(s[k]);
        for (col = 0; col < 5u; col++)
            for (r = 0; r < 7u; r++)
                if ((g[col] >> r) & 1u)
                    for (sy = 0; sy < sc; sy++)
                        for (sx = 0; sx < sc; sx++)
                            ls_buf[(r * sc + sy) * 240u + x0 + (k * 6u + col) * sc + sx] = fg;
    }
    ls_window(y, y + h - 1u);
    ls_data(ls_buf, 240u * h * 2u);
}

#define LS_GREY  0x1084u      /* 0x8410 */

static uint32_t ls_up;                          /* the screen has been started */
static uint32_t ls_pct = 0xFFu;                 /* the percent last shown (0xFF: none yet) */
static uint32_t ls_anim;                        /* the cables are being drawn (an install is running) */
static uint32_t ls_t0;                          /* when this round of the cables began (the loader's ms) */
static uint32_t ls_round_up;                    /* a round has begun */
static int16_t ls_hi[5], ls_lo[5];              /* each cable: columns drawn so far / erased so far */

/* the install screen, as the power-on screen: five patch cables from a column of jacks to another (a smooth S, each leaving
 * level and settling into its place), drawn in from left to right one after another, plug first, the far plug as the cable
 * arrives; then, after a moment, drawn out again the same way, and again for as long as the install runs. Stepped from
 * ldr_poll (every 60 ms); the percent of the write is shown under the name. */
#define LS_XA 30
#define LS_XB 210
#define LS_N (LS_XB - LS_XA)
#define LS_Y0 52
#define LS_DY 14
#define LS_IN 750                               /* ms to draw a cable in or out, and the lag between one cable and the next */
#define LS_LAG 110
#define LS_OUT 1900                             /* ms into the round when they start to come out */
#define LS_ROUND 3500
static const uint8_t LS_RGB[5][3] = { {BR_CABLE0}, {BR_CABLE1}, {BR_CABLE2}, {BR_CABLE3}, {BR_CABLE4} };
static const uint8_t LS_TO[5] = { 3, 4, 0, 1, 2 };
static void ls_win(uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
{
    ls_small[0] = (uint8_t)(x0 >> 8); ls_small[1] = (uint8_t)x0; ls_small[2] = (uint8_t)(x1 >> 8); ls_small[3] = (uint8_t)x1;
    ls_cmd(0x2A); ls_data(ls_small, 4);
    ls_small[4] = (uint8_t)(y0 >> 8); ls_small[5] = (uint8_t)y0; ls_small[6] = (uint8_t)(y1 >> 8); ls_small[7] = (uint8_t)y1;
    ls_cmd(0x2B); ls_data(ls_small + 4, 4);
    ls_cmd(0x2C);
}
static void ls_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i;
    for (i = 0; i < w * h; i++)
        ls_buf[i] = c;
    ls_win(x, x + w - 1u, y, y + h - 1u);
    ls_data(ls_buf, w * h * 2u);
}
static uint16_t ls_rgb(uint32_t k, uint32_t on)    /* cable k's color (or black), in the panel's byte order */
{
    uint16_t c;
    if (!on)
        return 0;
    c = (uint16_t)((LS_RGB[k][0] >> 3) << 11 | (LS_RGB[k][1] >> 2) << 5 | (LS_RGB[k][2] >> 3));
    return (uint16_t)(c << 8 | c >> 8);
}
static uint32_t ls_cy(uint32_t k, int32_t t)       /* the cable's row at column t (the power-on screen's smoothstep) */
{
    int32_t ya = LS_Y0 + (int32_t)k * LS_DY, yb = LS_Y0 + (int32_t)LS_TO[k] * LS_DY;
    int32_t s = 3 * t * t * LS_N - 2 * t * t * t;
    return (uint32_t)(ya + (yb - ya) * s / (LS_N * LS_N * LS_N));
}
static void ls_plug(uint32_t k, uint32_t right, uint32_t on)
{
    ls_rect(right ? LS_XB - 4u : LS_XA - 4u, LS_Y0 + (right ? LS_TO[k] : k) * LS_DY - 4u, 8u, 8u, ls_rgb(k, on));
}
static void ls_cables_start(void)
{
    uint32_t k;
    for (k = 0; k < 5u; k++)
        ls_hi[k] = ls_lo[k] = -1;
    ls_round_up = 0;
    ls_anim = 1;
}
/* how far a cable has got, in columns, k * LS_LAG ms behind the one above, e ms into a pass of LS_IN ms */
static int32_t ls_reach(uint32_t k, int32_t e)
{
    int32_t q = e - (int32_t)(k * LS_LAG);
    return q <= 0 ? -1 : q >= LS_IN ? LS_N : q * LS_N / LS_IN;
}
/* from ldr_poll with the loader's clock in ms: brings every cable up to date, every 60 ms */
static void ldr_screen_tick(uint32_t ms)
{
    static uint32_t last;
    uint32_t k;
    int32_t t, e, hi, lo;
    if (!ls_anim || ms - last < 60u)
        return;
    last = ms;
    if (!ls_round_up || ms - ls_t0 >= LS_ROUND) {
        ls_t0 = ms;
        ls_round_up = 1;
        for (k = 0; k < 5u; k++)
            ls_hi[k] = ls_lo[k] = -1;
    }
    e = (int32_t)(ms - ls_t0);
    for (k = 0; k < 5u; k++) {
        hi = ls_reach(k, e);
        lo = e < LS_OUT ? -1 : ls_reach(k, e - LS_OUT);
        if (ls_hi[k] < 0 && hi >= 0)
            ls_plug(k, 0, 1);                                  /* the left plug as the cable starts */
        for (t = (ls_hi[k] + 2) & ~1; t <= hi; t += 2)         /* drawn in */
            ls_rect((uint32_t)(LS_XA + t), ls_cy(k, t) - 1u, 2u, 3u, ls_rgb(k, 1));
        if (ls_hi[k] < LS_N && hi >= LS_N)
            ls_plug(k, 1, 1);                                  /* the right plug as it arrives */
        if (ls_lo[k] < 0 && lo >= 0)
            ls_plug(k, 0, 0);                                  /* drawn out: the left plug first */
        for (t = (ls_lo[k] + 2) & ~1; t <= lo; t += 2) {
            uint32_t j, y = ls_cy(k, t);
            ls_rect((uint32_t)(LS_XA + t), y - 1u, 2u, 3u, 0);
            for (j = 0; j < 5u; j++) {                         /* a cable still there that crossed this one is put back */
                uint32_t yj;
                if (j == k || ls_lo[j] >= t || ls_hi[j] < t)
                    continue;
                yj = ls_cy(j, t);
                if (yj + 3u > y && yj < y + 3u)
                    ls_rect((uint32_t)(LS_XA + t), yj - 1u, 2u, 3u, ls_rgb(j, 1));
            }
        }
        if (ls_lo[k] < LS_N && lo >= LS_N)
            ls_plug(k, 1, 0);
        if (hi > ls_hi[k])
            ls_hi[k] = (int16_t)hi;
        if (lo > ls_lo[k])
            ls_lo[k] = (int16_t)lo;
    }
    fm1_wdt_feed();
}

static void ldr_screen_init(void)
{
    static const uint8_t SEQ[] = { 0x01, 0, 0x11, 0, 0x3A, 1, 0x55, 0x36, 1, 0x00, 0x21, 0, 0x13, 0 };   /* as device/lcd.c */
    uint32_t r, x;
    fm1_lcd_hw_init();
    fm1_delay_ms(80);
    fm1_wdt_feed();
    for (r = 0; r < sizeof SEQ; r += 2u + SEQ[r + 1u]) {
        ls_cmd(SEQ[r]);
        if (SEQ[r] == 0x01u)
            fm1_delay_ms(150);
        if (SEQ[r] == 0x11u)
            fm1_delay_ms(120);
        for (x = 0; x < SEQ[r + 1u]; x++)
            ls_small[x] = SEQ[r + 2u + x];
        if (SEQ[r + 1u])
            ls_data(ls_small, SEQ[r + 1u]);
        fm1_wdt_feed();
    }
    for (r = 0; r < 240u; r += 20u)
        ls_clear_rows(r, 20u);
    fm1_wdt_feed();
    ls_cmd(0x29);                                   /* display on */
    ls_up = 1;
}

static void ldr_screen_draw(uint32_t installing)
{
    uint32_t r;
    for (r = 0; r < 240u; r += 20u)
        ls_clear_rows(r, 20u);                      /* black first: the other message's lines must go */
    fm1_wdt_feed();
    ls_anim = 0;
    if (installing) {
        ls_text(134, "PATCHWERK", 3, LS_WHITE);
        ls_text(172, "UPDATING", 2, LS_ACCENT);
        ls_text(194, "DO NOT UNPLUG OR POWER OFF", 1, LS_GREY);
        ls_cables_start();
        ls_pct = 0xFFu;
        ls_text(216, "CHECKING THE FILE", 1, LS_GREY);
        return;
    }
    ls_text(10, "PATCHWERK", 3, LS_ACCENT);
    ls_text(40, "UPDATE MODE", 2, LS_WHITE);
    ls_text(68, "AN UPDATE STOPPED", 2, LS_WHITE);
    ls_text(88, "BEFORE IT FINISHED.", 2, LS_WHITE);
    ls_text(106, "YOUR PATCHES AND", 2, LS_GREY);
    ls_text(124, "SOUNDS ARE SAFE.", 2, LS_GREY);
    ls_text(152, "TO FINISH IT:", 2, LS_ACCENT);
    ls_text(170, "1 PLUG IN USB", 2, LS_WHITE);
    ls_text(188, "2 OPEN INSTALLER", 2, LS_WHITE);
    ls_text(206, "3 PRESS INSTALL", 2, LS_WHITE);
}

/* called after each 4 KB sector (ldr_core.c): the percent of the write (the loader checks the whole file first, which has no sectors) */
static void ldr_screen_progress(uint32_t done, uint32_t total)
{
    uint32_t pct;
    char t[5];
    if (!ls_up || !ls_anim || total == 0u)
        return;
    pct = done * 100u / total;
    if (pct > 100u)
        pct = 100u;
    if (pct == ls_pct)
        return;
    ls_pct = pct;
    t[0] = pct >= 100u ? '1' : ' ';
    t[1] = pct >= 10u ? (char)('0' + pct / 10u % 10u) : ' ';
    t[2] = (char)('0' + pct % 10u);
    t[3] = '%';
    t[4] = 0;
    ls_text(212, t, 2, LS_WHITE);
}
#endif
