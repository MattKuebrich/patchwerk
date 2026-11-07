/* SPDX-License-Identifier: GPL-3.0-only */
/* The module page's VISUALIZATIONS: a visualization of what a module is doing, drawn live from its knobs and its jacks.
 * Included by pw_ui.c (it uses that file's drawing helpers). A module with a visualization of its own (ENV, VCF...) keeps it;
 * the rest come from here: a waveform, a strip chart of the outputs, a transfer curve, echoes, a sweep... */

#define VIS_N 96
static struct { int slot; uint32_t last_ms, dt; int w, n; float h[4][VIS_N]; } vh = {-1, 0, 40, 0, 0, {{0}}};

/* "-1.2V", "+3.0V": one decimal */
static char *vis_volts(char *o, float v)
{
    int t = (int)(v * 10.0f + (v < 0 ? -0.5f : 0.5f)), a = t < 0 ? -t : t;
    if (t < 0) *o++ = '-'; else *o++ = '+';
    o = s_int(o, a / 10); *o++ = '.'; *o++ = (char)('0' + a % 10); *o++ = 'V'; *o = 0;
    return o;
}
/* the panel every visualization sits on, and the label row above it */
static void vis_panel(void)
{
    rfill(2, VY + 1, W - 4, VH - 2, RGB(16, 17, 22));
}
/* ---- the output history: one sample per 40 ms (a few seconds of it; vh.dt), for the strip charts */
typedef struct { uint8_t in; uint8_t port; } vis_src;
static void vis_sample(int s, const vis_src *src, int nch)
{
    uint32_t now = fm1_millis();
    if (vh.slot != s) {
        memset(vh.h, 0, sizeof vh.h); vh.slot = s; vh.w = vh.n = 0; vh.last_ms = now;
        if (pv_demo) {   /* the add list's preview: start with a history, as if it had been running */
            for (int k = 0; k < VIS_N; ++k) {
                pv_off = (float)(VIS_N - 1 - k) * (float)vh.dt * 0.001f;
                for (int c = 0; c < nch && c < 4; ++c) vh.h[c][k] = src[c].in ? pw_in_last((unsigned)s, src[c].port) : pw_out_last((unsigned)s, src[c].port);
            }
            pv_off = 0; vh.w = 0; vh.n = VIS_N;
        }
    }
    if (now - vh.last_ms < vh.dt) return;
    vh.last_ms = now;
    for (int c = 0; c < nch && c < 4; ++c)
        vh.h[c][vh.w] = src[c].in ? pw_in_last((unsigned)s, src[c].port) : pw_out_last((unsigned)s, src[c].port);
    vh.w = (vh.w + 1) % VIS_N;
    if (vh.n < VIS_N) ++vh.n;
}
static float vis_at(int c, int k)   /* k = 0 the oldest of what is kept */
{
    int idx = vh.n < VIS_N ? k : (vh.w + k) % VIS_N;
    return k < vh.n ? vh.h[c][idx] : 0;
}
static const uint16_t *vis_pal(int s)
{
    static uint16_t pal[4];
    pal[0] = cat_color[pw_types[pws.mod[s].type].cat]; pal[1] = C_AMBER; pal[2] = C_GREEN; pal[3] = C_MOD;
    return pal;
}

/* a strip chart: up to four traces moving left, the newest on the right */
static void vis_strip(int s, const vis_src *src, int nch, const char *const *names)
{
    const uint16_t *pal = vis_pal(s);
    vis_panel();
    vis_sample(s, src, nch);
    int top = VY + 22, bot = VY + VH - 8, h = bot - top;
    float lo = 1e9f, hi = -1e9f;
    for (int c = 0; c < nch; ++c) for (int k = 0; k < vh.n; ++k) { float v = vis_at(c, k); if (v < lo) lo = v; if (v > hi) hi = v; }
    if (vh.n == 0) { lo = 0; hi = 1; }
    if (hi - lo < 1.0f) { float mid = (hi + lo) * 0.5f; lo = mid - 0.5f; hi = mid + 0.5f; }
    float pad = (hi - lo) * 0.08f; lo -= pad; hi += pad;
    for (int g = 0; g <= 4; ++g) fill(6, top + g * h / 4, W - 12, 1, g == 2 ? C_PANEL2 : C_PANEL);
    if (lo < 0 && hi > 0) fill(6, bot - (int)((0 - lo) / (hi - lo) * (float)h), W - 12, 1, C_LINE);
    int x = 8;
    for (int c = 0; c < nch; ++c) {
        int px = 0, py = 0;
        for (int k = 0; k < vh.n; ++k) {
            int xx = 8 + k * (W - 16) / (VIS_N - 1), yy = bot - (int)((vis_at(c, k) - lo) / (hi - lo) * (float)h);
            yy = clampi(yy, top, bot);
            if (k) line(px, py, xx, yy, 1, pal[c]);
            px = xx; py = yy;
        }
        if (names && names[c]) { fill(x, VY + 6, 6, 6, pal[c]); x = text(x + 9, VY + 4, names[c], F_XS, C_TEXT) + 8; }
    }
    char r[20], *o = s_cpy(r, "TOP ");
    vis_volts(o, hi - pad);
    text_right(W - 8, VY + 5, r, F_XS, C_DIM);
}

/* the waveform an output makes now, steadied by a rising zero crossing; several outputs: each in turn */
static void vis_wave(int s, int fixed_port)
{
    const pw_type *t = &pw_types[pws.mod[s].type];
    uint16_t c = cat_color[t->cat];
    int port = fixed_port;
    if (port < 0) {
        port = 0;
        if (t->n_out > 1) port = (int)((fm1_millis() / 1800u) % (uint32_t)t->n_out);
    }
    vis_panel();
    int top = VY + 22, bot = VY + VH - 8, h = bot - top, mid = top + h / 2;
    for (int g = 0; g <= 4; ++g) fill(6, top + g * h / 4, W - 12, 1, g == 2 ? C_LINE : C_PANEL);
    if (pv_demo) {   /* the add list's preview: a made-up wave (a shifter: its input too, dimmer) */
        for (int w = pws.mod[s].type == PW_T_PSHIFT ? 0 : 1; w < 2; ++w) {
            int px = 0, py = mid;
            for (int k = 0; k < 116; ++k) {
                int xx = 6 + k * 2, yy = clampi(mid - (int)(pv_wave(pws.mod[s].type, (float)k / 115.0f, w) * (float)(h / 2 - 3)), top, bot);
                if (k) line(px, py, xx, yy, 1, w ? c : C_LINE);
                px = xx; py = yy;
            }
        }
        text_fit(8, VY + 4, t->n_out ? t->out[port] : "", F_S, c, 120);
        text_right(W - 8, VY + 5, "PEAK +2.0V", F_XS, C_DIM);
        return;
    }
    if (pws.scope_mod != (uint8_t)s || pws.scope_port != (uint8_t)port) pw_set_scope((unsigned)s, (unsigned)port);
    const unsigned span = 560u, mask = PW_SCOPE - 1u;
    unsigned w = pws.scope_w, start = (w - span - 1u) & mask;
    for (unsigned back = span + 2u; back < PW_SCOPE - 2u; ++back) {
        unsigned i = (w - back) & mask;
        if (pws.scope[i] <= 0 && pws.scope[(i + 1u) & mask] > 0) { start = i; break; }
    }
    float peak = 0;
    for (unsigned k = 0; k < span; k += 4u) { float a = pw_absf((float)pws.scope[(start + k) & mask] * (1.0f / 3000.0f)); if (a > peak) peak = a; }
    float sc = peak < 0.5f ? 0.5f : peak;
    int px = 0, py = mid;
    for (int k = 0; k < 116; ++k) {
        int xx = 6 + k * 2, yy = mid - (int)((float)pws.scope[(start + (unsigned)k * 5u) & mask] * (1.0f / 3000.0f) / sc * (float)(h / 2 - 2));
        yy = clampi(yy, top, bot);
        if (k) line(px, py, xx, yy, 1, c);
        px = xx; py = yy;
    }
    text_fit(8, VY + 4, t->n_out ? t->out[port] : "", F_S, c, 120);
    if (peak < 0.002f) text_right(W - 8, VY + 5, "NO SIGNAL", F_XS, C_DIM);
    else { char r[20], *o = s_cpy(r, "PEAK "); vis_volts(o, peak); text_right(W - 8, VY + 5, r, F_XS, C_DIM); }
}

/* a curve y = f(x) on a square, x and y -1..1, with the signal's own dot on it (input to output, live) */
static void vis_curve_frame(int *cx, int *cy, int *cw)
{
    vis_panel();
    *cw = VH - 24; *cx = 14; *cy = VY + 12;
    rfill(*cx, *cy, *cw, *cw, RGB(10, 11, 15));
    fill(*cx, *cy + *cw / 2, *cw, 1, C_LINE);
    fill(*cx + *cw / 2, *cy, 1, *cw, C_LINE);
}
static float vis_fold_tri(float t) { float u = pw_frac((t + 1.0f) * 0.25f); return 1.0f - 4.0f * pw_absf(u - 0.5f); }
static float vis_xfer(const pw_module *m, float x)
{
    if (m->type == PW_T_DIST) {
        int mode = (int)(m->p[1] + 0.5f);
        float d = m->p[0], g = pw_exp2(d * 6.64f), mix = m->p[3], y;
        switch (mode) {
        case 0: y = pw_tanh(x * g); break;
        case 1: y = pw_clamp(x * g, -1.0f, 1.0f) * 0.85f; break;
        case 2: y = pw_tanh(x * g * 1.5f + 0.35f) - 0.3364f; break;
        default: { float step = pw_exp2(-(15.0f - d * 13.0f)); if (step < 0.02f) step = 0.02f * 0.5f + step * 0.5f; y = (float)pw_floori(x / step + 0.5f) * step; } break;
        }
        return mix * y + (1.0f - mix) * x;
    }
    /* FOLDER */
    float fold = pw_clamp(m->p[0], 1.0f, 24.0f), bias = m->p[1], mix = m->p[3], y;
    int shape = (int)(m->p[2] + 0.5f);
    if (shape == 3) { float v = x * 0.4f * 5.0f * fold * 0.2f + bias * 3.0f; y = vis_fold_tri(v * 0.5f); }
    else if (shape == 2) {
        float order = pw_clamp(m->p[4], 1.0f, 16.0f), t = pw_clamp(x * fold + bias, -1.0f, 1.0f), t0 = 1.0f, t1 = t, ya = t;
        int on = (int)order;
        for (int k = 2; k <= on; ++k) { float tk = 2.0f * t * t1 - t0; t0 = t1; t1 = tk; ya = tk; }
        y = ya;
    } else { float t = x * fold + bias; y = shape == 1 ? pw_sin1(t * 0.25f) : vis_fold_tri(t); }
    return mix * y + (1.0f - mix) * x;
}
static void vis_shaper(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int cx, cy, cw;
    vis_curve_frame(&cx, &cy, &cw);
    int px = 0, py = 0;
    for (int i = 0; i <= 64; ++i) {
        float x = (float)i / 32.0f - 1.0f, y = pw_clamp(vis_xfer(m, x), -1.0f, 1.0f);
        int xx = cx + (int)((x + 1.0f) * 0.5f * (float)(cw - 1)), yy = cy + cw - 1 - (int)((y + 1.0f) * 0.5f * (float)(cw - 1));
        if (i) line(px, py, xx, yy, 2, c);
        px = xx; py = yy;
    }
    float in = pw_clamp(pw_in_last((unsigned)s, 0) * 0.2f, -1.0f, 1.0f), out = pw_clamp(pw_out_last((unsigned)s, 0) * 0.2f, -1.0f, 1.0f);
    int dx = cx + (int)((in + 1.0f) * 0.5f * (float)(cw - 1)), dy = cy + cw - 1 - (int)((out + 1.0f) * 0.5f * (float)(cw - 1));
    if (pw_input_cables((unsigned)s, 0) > 0) { fill(dx - 2, dy - 2, 5, 5, C_WHITE); fill(dx, cy + cw - 3, 1, 3, C_WHITE); fill(cx, dy, 3, 1, C_WHITE); }
    int x0 = cx + cw + 10;
    text(x0, cy + 2, "ACROSS: IN", F_XS, C_DIM);
    text(x0, cy + 16, "UP: OUT", F_XS, C_DIM);
    if (m->type == PW_T_DIST) { char v[12]; pw_format_param((unsigned)s, 1, v); text(x0, cy + 70, v, F_S, c); }
    else { char v[12]; pw_format_param((unsigned)s, 2, v); text(x0, cy + 70, v, F_S, c); }
    if (pw_input_cables((unsigned)s, 0) == 0) text(x0, cy + 92, "PATCH IN", F_XS, C_DIM);
}

/* COMPRESSOR: the gain curve (level in against level out, in dB) and the reduction now */
static void vis_comp(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int cx, cy, cw;
    vis_curve_frame(&cx, &cy, &cw);
    float th = m->p[0], ratio = m->p[1], mk = m->p[4];
    int px = 0, py = 0;
    for (int i = 0; i <= 48; ++i) {
        float db = -48.0f + (float)i, o = db > th ? th + (db - th) / ratio : db;
        o += mk;
        int xx = cx + (int)((db + 48.0f) / 48.0f * (float)(cw - 1)), yy = cy + cw - 1 - (int)(pw_clamp((o + 48.0f) / 48.0f, 0.0f, 1.0f) * (float)(cw - 1));
        if (i) line(px, py, xx, yy, 2, c);
        px = xx; py = yy;
    }
    int tx = cx + (int)((th + 48.0f) / 48.0f * (float)(cw - 1));
    fill(tx, cy, 1, cw, C_AMBER);
    int x0 = cx + cw + 10;
    text(x0, cy + 2, "REDUCING", F_XS, C_DIM);
    float gr = pw_clamp(pw_out_last((unsigned)s, 1) * 0.2f, 0.0f, 1.0f);
    rfill(x0, cy + 16, 90, 10, C_PANEL2); fill(x0, cy + 16, (int)(gr * 90.0f), 10, C_AMBER);
    text(x0, cy + 38, "THRESHOLD", F_XS, C_DIM);
    char v[12]; pw_format_param((unsigned)s, 0, v); text(x0, cy + 50, v, F_S, C_AMBER);
    text(x0, cy + 72, "RATIO", F_XS, C_DIM); pw_format_param((unsigned)s, 1, v); text(x0, cy + 84, v, F_S, c);
}

/* DELAY: the dry hit and its echoes, each softer by the feedback; the echo spacing is the time */
static void vis_echo(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    float time = m->p[0], fb = pw_minf(m->p[1], 1.1f), mix = m->p[3];
    float sy = pw_sync_sec(m->p[4]);
    if (sy > 0) time = sy;                       /* SYNC: the note value rules */
    int ping = m->p[5] > 0.5f, frozen = m->p[8] > 0.5f;
    if (frozen) fb = 1.0f;
    int base = VY + VH - 18, hmax = VH - 50;
    float pxs = 190.0f / pw_maxf(time * 3.4f, 0.12f), amp = 1.0f;
    fill(8, base, W - 16, 1, C_LINE);
    fill(14, base - hmax, 5, hmax, C_WHITE);
    text(10, base + 3, "DRY", F_XS, C_DIM);
    for (int k = 1; k < 12; ++k) {
        amp *= fb;
        int x = 16 + (int)((float)k * time * pxs);
        if (x > W - 12 || amp < 0.02f) break;
        int hh = (int)(amp * mix * 2.0f * (float)hmax); if (hh > hmax) hh = hmax;
        if (hh < 1) hh = 1;
        fill(x, base - hh, 5, hh, ping ? (k & 1 ? C_AMBER : cat_color[PW_C_FILTER]) : c);   /* (ping-pong: left, right, left...) */
    }
    char v[12];
    pw_format_param((unsigned)s, sy > 0 ? 4 : 0, v); text(8, VY + 4, sy > 0 ? "SYNC" : "TIME", F_XS, C_DIM); text(sy > 0 ? 38 : 34, VY + 3, v, F_S, c);
    pw_format_param((unsigned)s, 1, v); text(110, VY + 4, "FEEDBACK", F_XS, C_DIM); text(166, VY + 3, v, F_S, c);
    if (frozen) text_right(W - 8, VY + 22, "FROZEN", F_XS, C_AMBER);
    else if (fb > 1.0f) text_right(W - 8, VY + 22, "RUNS AWAY", F_XS, C_RED);
    if (ping) text_right(W - 8, VY + 34, "PING-PONG", F_XS, C_DIM);
    float wet = pw_clamp(pw_out_last((unsigned)s, 1) * 0.2f, -1.0f, 1.0f);
    int wl = (int)(pw_absf(wet) * 60.0f);
    text(8, VY + 20, "WET", F_XS, C_DIM); fill(30, VY + 21, wl, 4, c);
}

/* REVERB: the tail dying away, a size of early reflections before it, DAMP darkening it, FREEZE holding it */
static void vis_verb(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    float size = m->p[0], decay = m->p[1], damp = m->p[2];
    int base = VY + VH - 18, hmax = VH - 46, frozen = m->p[4] > 0.5f || pw_in_last((unsigned)s, 2) > 1.0f;
    fill(8, base, W - 16, 1, C_LINE);
    fill(14, base - hmax, 4, hmax, C_WHITE);
    for (int k = 0; k < 6; ++k) {   /* early reflections */
        int x = 22 + (int)(m->p[8] * 360.0f) + (int)((float)k * (6.0f + size * 12.0f)) + (int)(pw_frac((float)k * 0.618f) * 8.0f);
        int hh = (int)((0.8f - (float)k * 0.1f) * (float)hmax * 0.6f);
        fill(x, base - hh, 2, hh, blend(RGB(16, 17, 22), c, 10));
    }
    int px = 0, py = 0;
    for (int i = 0; i <= 60; ++i) {
        float t = (float)i / 60.0f, a = frozen ? 0.8f : pw_exp2(-t * 13.0f * 2.5f / pw_maxf(decay, 0.3f));
        int xx = 70 + i * 2, yy = base - (int)(a * (float)hmax * (1.0f - damp * 0.3f * t));
        if (i) line(px, py, xx, yy, 2, c);
        px = xx; py = yy;
    }
    char v[12];
    text(8, VY + 4, "DECAY", F_XS, C_DIM); pw_format_param((unsigned)s, 1, v); text(46, VY + 3, v, F_S, c);
    text(110, VY + 4, "SIZE", F_XS, C_DIM); pw_format_param((unsigned)s, 0, v); text(138, VY + 3, v, F_S, c);
    if (frozen) text_right(W - 8, VY + 22, "FROZEN", F_XS, C_AMBER);
    pw_format_param((unsigned)s, 7, v); text_right(W - 8, VY + 4, v, F_S, c);
    float lv = pw_clamp(pw_absf(pw_out_last((unsigned)s, 0)) * 0.2f, 0.0f, 1.0f);
    text(8, VY + 20, "OUT", F_XS, C_DIM); fill(30, VY + 21, (int)(lv * 60.0f), 4, c);
}

/* FLANGER, PHASER, CHORUS, HARMONIC TREMOLO: the sweep itself, its cursor going round at the rate */
static void vis_sweep(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    int top = VY + 26, bot = VY + VH - 10, h = bot - top, mid = top + h / 2;
    int swirl = m->type == PW_T_SWIRL, ty = swirl ? (int)(m->p[0] + 0.5f) : -1;   /* SWIRL's TYPE: 0 flanger, 1 phaser, 2 chorus */
    float rate = m->p[swirl ? 1 : 0], depth = m->p[swirl ? 2 : 1];
    float ph = pw_frac((float)(fm1_millis() % 600000u) * 0.001f * rate);
    int voices = ty == 2 ? 3 : 1;
    fill(8, mid, W - 16, 1, C_PANEL2);
    for (int v = 0; v < voices; ++v) {
        float off = (float)v / (float)voices;
        int px = 0, py = 0;
        for (int i = 0; i <= 80; ++i) {
            float p = (float)i / 80.0f + off;
            int xx = 8 + i * (W - 16) / 80, yy = mid - (int)(pw_sin1(p) * depth * (float)(h / 2 - 2));
            if (i) line(px, py, xx, yy, 1, v ? blend(RGB(16, 17, 22), c, 9) : c);
            px = xx; py = yy;
        }
        int xx = 8 + (int)(pw_frac(ph) * (float)(W - 16)), yy = mid - (int)(pw_sin1(pw_frac(ph) + off) * depth * (float)(h / 2 - 2));
        fill(xx - 3, yy - 3, 7, 7, C_WHITE);
    }
    const char *what = ty == 0 ? "FLANGER: DELAY TIME" : ty == 1 ? "PHASER: NOTCH FREQ" : ty == 2 ? "CHORUS: THREE VOICES" : "LOWS / HIGHS";
    text(8, VY + 4, what, F_XS, C_DIM);
    char v[12];
    pw_format_param((unsigned)s, swirl ? 1 : 0, v); text_right(W - 70, VY + 3, v, F_S, c);
    pw_format_param((unsigned)s, swirl ? 2 : 1, v); text_right(W - 8, VY + 3, v, F_S, c);
}

/* VOWEL FILTER: the two formant peaks the vowel puts on the spectrum */
static void vis_vowel(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    static const float ff[5][2] = {{800, 1150}, {400, 1700}, {270, 2140}, {450, 800}, {325, 700}};
    float v = pw_clamp(m->p[0] + pw_in_last((unsigned)s, 1) * 0.8f, 0, 4), t; int a = (int)v; if (a > 3) a = 3; t = v - (float)a;
    float sh = pw_exp2(m->p[1] / 12.0f), f1 = pw_lerp(ff[a][0], ff[a + 1][0], t) * sh, f2 = pw_lerp(ff[a][1], ff[a + 1][1], t) * sh, bw = 60.0f * pw_exp2((0.5f + (m->p[2] - 0.5f)) * 4.0f);
    vis_panel();
    int top = VY + 26, bot = VY + VH - 16;
    int px = 0, py = bot;
    for (int i = 0; i <= 100; ++i) {
        float hz = 100.0f + (float)i * 40.0f, d1 = (hz - f1) / bw, d2 = (hz - f2) / bw, g = 1.0f / (1.0f + d1 * d1) + 0.5f / (1.0f + d2 * d2);
        int xx = 8 + i * (W - 16) / 100, yy = bot - (int)(pw_minf(g, 1.0f) * (float)(bot - top));
        if (i) line(px, py, xx, yy, 2, c);
        px = xx; py = yy;
    }
    fill(8, bot, W - 16, 1, C_LINE);
    text(8, bot + 3, "100", F_XS, C_LINE); text_right(W - 8, bot + 3, "4 KHZ", F_XS, C_LINE);
    static const char *const vn[5] = {"A", "E", "I", "O", "U"};
    text(10, VY + 4, vn[clampi((int)(v + 0.5f), 0, 4)], F_M, C_WHITE);
    text(40, VY + 8, "F1", F_XS, C_DIM);
    char b[8], *o = s_int(b, (int)f1); (void)o; text(54, VY + 7, b, F_XS, c);
    text(100, VY + 8, "F2", F_XS, C_DIM); s_int(b, (int)f2); text(114, VY + 7, b, F_XS, c);
}

/* CHAOS: the attractor drawn as it moves: X against Z, the newest dot white, older ones fading */
static void vis_chaos(int s)
{
    static const vis_src src[3] = {{0, 0}, {0, 1}, {0, 2}};
    uint16_t c = cat_color[pw_types[pws.mod[s].type].cat];
    vis_panel();
    vh.dt = 14;
    vis_sample(s, src, 3);
    vh.dt = 40;
    float xl = 1e9f, xh = -1e9f, zl = 1e9f, zh = -1e9f;
    for (int k = 0; k < vh.n; ++k) { float x = vis_at(0, k), z = vis_at(2, k); if (x < xl) xl = x; if (x > xh) xh = x; if (z < zl) zl = z; if (z > zh) zh = z; }
    if (vh.n == 0 || xh - xl < 0.5f) { xl = -3; xh = 3; }
    if (zh - zl < 0.5f) { zl = 0; zh = 6; }
    int top = VY + 20, bot = VY + VH - 6, h = bot - top, left = 10, w = W - 20;
    int px = 0, py = 0;
    for (int k = 0; k < vh.n; ++k) {
        int xx = left + (int)((vis_at(0, k) - xl) / (xh - xl) * (float)w), yy = bot - (int)((vis_at(2, k) - zl) / (zh - zl) * (float)h);
        if (k) line(px, py, xx, yy, 1, blend(RGB(16, 17, 22), c, (unsigned)(2 + k * 13 / VIS_N)));
        px = xx; py = yy;
    }
    if (vh.n) fill(px - 2, py - 2, 5, 5, C_WHITE);
    text(8, VY + 4, "X  /  Z", F_XS, C_DIM);
    text_right(W - 8, VY + 4, "NEVER REPEATS", F_XS, C_DIM);
}

/* MOD SEQ: four lanes of sixteen steps, each lane its own length; the playing step lit */
static void vis_modseq(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t lc[4] = {C_AMBER, cat_color[PW_C_FILTER], cat_color[PW_C_FX], cat_color[PW_C_MOD]};   /* (the lanes' colors on the steps page) */
    int small = ui.page == PG_EDIT;   /* on the module page: half size, the words under it are what you read */
    int sp = small ? 9 : 13, bw = small ? 7 : 10, lh = small ? 18 : 24, lp = small ? 21 : 29, x0 = small ? (W - 16 * sp) / 2 + 6 : 20, y0 = small ? VY + (VH - 22 - 4 * lp) / 2 : VY + 4;
    vis_panel();
    static const char *const ln[4] = {"A", "B", "C", "D"};
    for (int l = 0; l < 4; ++l) {
        int y = y0 + l * lp, len = clampi((int)(m->p[l] + 0.5f), 1, 16), at = (int)pw_live((unsigned)s, (unsigned)l);
        text(x0 - (small ? 12 : 14), y + (small ? 5 : 10), ln[l], small ? F_XS : F_S, lc[l]);
        for (int k = 0; k < 16; ++k) {
            int x = x0 + k * sp, hh = 3 + (int)pw_mseq_get(m, (unsigned)l, (unsigned)k) * (lh - 4) / 255;
            if (k >= len) { fill(x, y + lh - 3, bw, 3, C_WELL); continue; }
            fill(x, y, bw, lh, C_PANEL);
            fill(x, y + lh - hh, bw, hh, k == at && pws.playing ? C_GREEN : lc[l]);
        }
    }
}

/* CHORD STEPS: the progression as roman numerals, the playing chord lit */
static void vis_chordsteps(int s)
{
    const pw_module *m = &pws.mod[s];
    static const char *const rn[9] = {"-", "I", "II", "III", "IV", "V", "VI", "VII", "~"};
    vis_panel();
    int n = clampi((int)(m->p[0] + 0.5f), 1, 16), at = (int)pw_live((unsigned)s, 0);
    for (int k = 0; k < 16; ++k) {
        int x = 6 + (k % 8) * 29, y = VY + 28 + (k / 8) * 46;
        uint8_t b = m->data[k];
        int deg = b & 15; if (deg > 8) deg = 0;
        static const uint16_t hue[7] = {RGB(120, 225, 95), RGB(50, 200, 235), RGB(165, 125, 255), RGB(255, 140, 50), RGB(255, 95, 205), RGB(245, 210, 70), RGB(255, 95, 95)};
        uint16_t hc = deg >= 1 && deg <= 7 ? hue[deg - 1] : C_LINE;   /* each degree its own color */
        uint16_t bg = k >= n ? RGB(14, 15, 19) : k == at ? hc : deg ? blend(C_PANEL2, hc, 6) : C_PANEL2;
        rfill(x, y, 27, 40, bg);
        text_center(x + 13, y + 14, rn[deg], F_S, k == at ? C_BG : k >= n ? C_PANEL : C_TEXT);
    }
    char nm[10];
    if (at >= 0 && at < 16 && (m->data[at] & 15) >= 1 && (m->data[at] & 15) <= 7) { pw_chord_name(m, m->data[at], nm); text(8, VY + 5, nm, F_M, C_WHITE); }
    else text(8, VY + 6, "PROGRESSION", F_XS, C_DIM);
}

/* TURING: the shift register, bit by bit, and the voltage it makes */
static void vis_turing(int s)
{
    static const vis_src src[1] = {{0, 0}};
    static const char *const nm[1] = {"CV"};
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    uint32_t reg = (uint32_t)pw_live((unsigned)s, 0) | (uint32_t)pw_live((unsigned)s, 1) << 16;
    int len = clampi((int)(m->p[0] + 0.5f), 2, 32), rows = len > 16 ? 2 : 1, locked = m->p[3] > 0.5f;
    vis_strip(s, src, 1, nm);
    for (int k = 0; k < 32; ++k) {
        if (k >= 16 && rows == 1) break;
        int x = 8 + (k & 15) * 14, y = rows == 2 ? VY + VH - 35 + (k >> 4) * 15 : VY + VH - 20;
        int on = (reg >> k) & 1u;
        rfill(x, y, 11, 11, k >= len ? C_PANEL : on ? (locked ? C_AMBER : c) : C_PANEL2);
    }
}

/* ATTENUVERTER: the two lines out = in x gain + offset, and where the signals sit on them */
static void vis_atten(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t cols[2] = {cat_color[pw_types[m->type].cat], C_AMBER};
    int cx, cy, cw;
    vis_curve_frame(&cx, &cy, &cw);
    for (int ch = 0; ch < 2; ++ch) {
        float g = m->p[ch * 2], o = m->p[ch * 2 + 1], ex = m->p[4] == 0 ? 1.0f : pw_exp2(m->p[4] * 2.0f);
        int px = 0, py = 0, rect = (int)(m->p[5] + 0.5f);
        for (int i = 0; i <= 24; ++i) {
            float x = (float)i / 12.0f - 1.0f, xin = x * 5.0f;
            if (rect) xin = rect == 1 ? pw_maxf(xin, 0.0f) : pw_absf(xin);
            if (m->p[4] != 0) { float u = pw_absf(xin) * 0.2f; u = u > 0.0001f ? pw_exp2(ex * pw_log2(u)) * 5.0f : 0.0f; xin = xin < 0 ? -u : u; }
            float y = pw_clamp((xin * g + o) / 5.0f, -1.0f, 1.0f);
            int xx = cx + (int)((x + 1.0f) * 0.5f * (float)(cw - 1)), yy = cy + cw - 1 - (int)((y + 1.0f) * 0.5f * (float)(cw - 1));
            if (i) line(px, py, xx, yy, 2, cols[ch]);
            px = xx; py = yy;
        }
        if (pw_input_cables((unsigned)s, (unsigned)ch) > 0) {
            float xi = pw_clamp(pw_in_last((unsigned)s, (unsigned)ch) / 5.0f, -1.0f, 1.0f), yo = pw_clamp(pw_out_last((unsigned)s, (unsigned)ch) / 5.0f, -1.0f, 1.0f);
            fill(cx + (int)((xi + 1.0f) * 0.5f * (float)(cw - 1)) - 2, cy + cw - 1 - (int)((yo + 1.0f) * 0.5f * (float)(cw - 1)) - 2, 5, 5, C_WHITE);
        }
    }
    int x0 = cx + cw + 10;
    text(x0, cy + 4, "A", F_S, cols[0]); text(x0, cy + 40, "B", F_S, cols[1]);
    char v[12]; pw_format_param((unsigned)s, 0, v); text(x0 + 16, cy + 4, v, F_S, cols[0]);
    pw_format_param((unsigned)s, 1, v); text(x0 + 16, cy + 18, v, F_XS, C_DIM);
    pw_format_param((unsigned)s, 2, v); text(x0 + 16, cy + 40, v, F_S, cols[1]);
    pw_format_param((unsigned)s, 3, v); text(x0 + 16, cy + 54, v, F_XS, C_DIM);
}


static char *vis_pct(char *b, int n) { char *o = s_int(b, n); *o++ = '%'; *o = 0; return b; }
/* XFADE: how loud each input is across the MIX knob, and where the knob (and its CV) sits now */
static void vis_xfade(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t cols[2] = {cat_color[pw_types[m->type].cat], C_AMBER};
    int cx, cy, cw, lin = m->p[1] > 0.5f;
    vis_curve_frame(&cx, &cy, &cw);
    for (int ch = 0; ch < 2; ++ch) {
        int px = 0, py = 0;
        for (int i = 0; i <= 24; ++i) {
            float x = (float)i / 24.0f, g = lin ? (ch ? x : 1.0f - x) : (ch ? pw_sin1(x * 0.25f) : pw_cos1(x * 0.25f));
            int xx = cx + (int)(x * (float)(cw - 1)), yy = cy + cw - 1 - (int)(g * (float)(cw - 1));
            if (i) line(px, py, xx, yy, 2, cols[ch]);
            px = xx; py = yy;
        }
    }
    float x = pw_clamp(m->p[0] + (pw_input_cables((unsigned)s, 2) > 0 ? pw_in_last((unsigned)s, 2) * 0.2f * m->p[2] : 0.0f), 0.0f, 1.0f);
    float ga = lin ? 1.0f - x : pw_cos1(x * 0.25f), gb = lin ? x : pw_sin1(x * 0.25f);
    int xx = cx + (int)(x * (float)(cw - 1));
    fill(xx, cy, 1, cw, C_LINE);
    fill(xx - 2, cy + cw - 1 - (int)(ga * (float)(cw - 1)) - 2, 5, 5, cols[0]);
    fill(xx - 2, cy + cw - 1 - (int)(gb * (float)(cw - 1)) - 2, 5, 5, cols[1]);
    int x0 = cx + cw + 10;
    char v[16];
    text(x0, cy + 4, "A", F_S, cols[0]); text(x0 + 16, cy + 4, vis_pct(v, (int)(ga * 100.0f + 0.5f)), F_S, cols[0]);
    text(x0, cy + 22, "B", F_S, cols[1]); text(x0 + 16, cy + 22, vis_pct(v, (int)(gb * 100.0f + 0.5f)), F_S, cols[1]);
    pw_format_param((unsigned)s, 0, v); text(x0, cy + 46, "MIX", F_XS, C_DIM); text(x0 + 28, cy + 44, v, F_S, C_TEXT);
    pw_format_param((unsigned)s, 1, v); text(x0, cy + 62, v, F_XS, C_DIM);
}

/* SPACE: the tail the room makes (its length SIZE's, the highs dying sooner as TONE closes), the four comb delays that make it, and the levels */
static void vis_space(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    float size = 0.45f + 0.55f * m->p[0], t60 = 0.25f + 2.75f * m->p[0] * m->p[0], damp = 0.08f + 0.92f * m->p[1];
    int base = VY + VH - 18, hmax = VH - 46;
    fill(8, base, W - 16, 1, C_LINE);
    fill(14, base - hmax, 4, hmax, C_WHITE);   /* (the sound going in) */
    static const uint16_t len[4] = {1237, 1559, 1277, 1613};
    for (int k = 0; k < 4; ++k) {
        int x = 22 + (int)((float)len[k] * size * (1.0f / 44100.0f) * 1000.0f * 1.6f);
        fill(x, base - hmax * 6 / 10, 2, hmax * 6 / 10, blend(RGB(16, 17, 22), c, 10));
    }
    for (int hi = 0; hi < 2; ++hi) {
        int px = 0, py = 0;
        for (int i = 0; i <= 60; ++i) {
            float t = (float)i / 60.0f * 3.0f, a = pw_exp2(-t * 9.966f / t60 * (hi ? 1.0f + damp * 3.0f : 1.0f));
            int xx = 70 + i * 2, yy = base - (int)(a * (float)hmax * 0.9f);
            if (i) line(px, py, xx, yy, 2, hi ? dim_of(c) : c);
            px = xx; py = yy;
        }
    }
    char v[16];
    text(8, VY + 4, "SIZE", F_XS, C_DIM); pw_format_param((unsigned)s, 0, v); text(40, VY + 3, v, F_S, c);
    text(100, VY + 4, "TONE", F_XS, C_DIM); pw_format_param((unsigned)s, 1, v); text(132, VY + 3, v, F_S, c);
    pw_format_param((unsigned)s, 2, v); text_right(W - 8, VY + 3, v, F_S, c);
    text(8, VY + 20, "OUT", F_XS, C_DIM);
    for (int ch = 0; ch < 2; ++ch) fill(30, VY + 21 + ch * 5, (int)(pw_clamp(pw_absf(pw_out_last((unsigned)s, (unsigned)ch)) * 0.2f, 0.0f, 1.0f) * 60.0f), 4, c);
    text_right(W - 8, VY + 20, "3 S", F_XS, C_DIM);
}

/* ---- piano keys (QUANTIZER, CHORD, KEYS): `state` has one entry per semitone from `lo`: 0 plain, 1 in the scale,
 * 2 an input, 3 sounding. `cols` colors states 1..3 */
static const char *const vis_nn[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static char *vis_note(char *o, int midi)
{
    o = s_cpy(o, vis_nn[((midi % 12) + 12) % 12]);
    return s_int(o, midi / 12 - 1);
}
static void vis_piano(int x, int y, int w, int h, int octs, const uint8_t *state, const uint16_t *cols)
{
    static const uint8_t wsem[7] = {0, 2, 4, 5, 7, 9, 11};
    int nw = 7 * octs, kw = w / nw;
    x += (w - kw * nw) / 2;
    for (int k = 0; k < nw; ++k) {
        int sem = (k / 7) * 12 + wsem[k % 7], st = state[sem];
        rfill(x + k * kw, y, kw - 1, h, st >= 2 ? cols[st - 1] : st == 1 ? blend(RGB(200, 202, 210), cols[0], 7) : RGB(200, 202, 210));
    }
    for (int k = 0; k < nw; ++k) {
        int pc = k % 7;
        if (pc == 2 || pc == 6) continue;   /* no black key after E and B */
        int sem = (k / 7) * 12 + wsem[pc] + 1, st = state[sem], bw = kw * 6 / 10 + 1;
        fill(x + k * kw + kw - bw / 2 - 1, y, bw, h * 6 / 10, st >= 2 ? cols[st - 1] : st == 1 ? blend(RGB(30, 31, 38), cols[0], 7) : RGB(30, 31, 38));
    }
}
static int vis_in_scale(unsigned scale, int root, int midi) { return (pw_scale_mask(scale) >> (((midi - root) % 12 + 12) % 12)) & 1u; }

/* QUANTIZER: the scale on a keyboard, the note coming in and the note it lands on */
static void vis_quant(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat], cols[3] = {c, C_AMBER, C_GREEN};
    vis_panel();
    int root = (int)(m->p[1] + 0.5f);
    float vin = pw_in_last((unsigned)s, 0), vout = pw_out_last((unsigned)s, 0);
    int nin = pw_floori(60.5f + vin * 12.0f), nout = pw_floori(60.5f + vout * 12.0f);
    uint8_t st[12];   /* one octave: the notes repeat */
    for (int k = 0; k < 12; ++k) st[k] = (uint8_t)vis_in_scale((unsigned)(m->p[0] + 0.5f), root, k);
    int connected = pw_input_cables((unsigned)s, 0) > 0;
    if (connected && nin != nout) st[((nin % 12) + 12) % 12] = 2;
    st[((nout % 12) + 12) % 12] = 3;
    vis_piano(10, VY + 44, W - 20, 84, 1, st, cols);
    char b[24], *o = s_cpy(b, "IN ");
    if (connected) vis_note(o, nin); else s_cpy(o, "-");
    text(8, VY + 8, b, F_S, connected ? C_AMBER : C_DIM);
    o = s_cpy(b, "OUT "); vis_note(o, nout);
    text(104, VY + 8, b, F_S, C_WHITE);
    text(8, VY + 26, "AMBER: IN   GREEN: WHERE IT LANDS", F_XS, C_DIM);
}

/* CHORD: its four notes lit on a keyboard (one octave: the notes repeat) */
static void vis_chord(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat], cols[3] = {c, c, C_GREEN};
    vis_panel();
    int n[4];
    uint8_t st[12] = {0};
    for (int k = 0; k < 4; ++k) { n[k] = pw_floori(60.5f + pw_out_last((unsigned)s, (unsigned)k) * 12.0f); st[((n[k] % 12) + 12) % 12] = 3; }
    vis_piano(10, VY + 40, W - 20, 84, 1, st, cols);
    char v[16]; pw_format_param((unsigned)s, 0, v);
    text(8, VY + 6, v, F_M, C_WHITE);
    char b[40], *o = b;
    for (int k = 0; k < 4; ++k) { o = vis_note(o, n[k]); *o++ = ' '; }
    *o = 0;
    text_right(W - 8, VY + 12, b, F_XS, C_DIM);
}

/* ARP: the pattern as bars (height = pitch, in the order it plays) over a keyboard with the held notes and the one sounding lit */
static void vis_arp(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat], cols[3] = {c, C_AMBER, C_GREEN};
    vis_panel();
    int mode = (int)(m->p[0] + 0.5f), octs = (int)(m->p[2] + 0.5f);
    if (octs < 1) octs = 1;
    int n = (int)pw_live((unsigned)s, 0), idx = (int)pw_live((unsigned)s, 1), on = pw_live((unsigned)s, 2) > 0.5f;
    if (n > 16) n = 16;
    int nt[16];
    for (int k = 0; k < n; ++k) nt[k] = (int)pw_live((unsigned)s, 8u + (unsigned)k);
    if (mode != 4) for (int a = 1; a < n; ++a) for (int b = a; b > 0 && nt[b - 1] > nt[b]; --b) { int t = nt[b]; nt[b] = nt[b - 1]; nt[b - 1] = t; }
    if (!n) {
        uint8_t st0[48] = {0};
        vis_piano(6, VY + 34, W - 12, 80, 3, st0, cols);
        text(8, KY - 12, "HOLD KEYS", F_XS, C_DIM);
        return;
    }
    int total = n * octs, show = total > 16 ? 16 : total;
    int lo = 127, hi = 0, pat[16];
    for (int i = 0; i < show; ++i) {
        int at = mode == 1 ? total - 1 - i : i;
        pat[i] = nt[at % n] + 12 * (at / n);
        if (pat[i] < lo) lo = pat[i];
        if (pat[i] > hi) hi = pat[i];
    }
    int cur = -1;
    if (mode == 1) { if (idx < total) cur = total - 1 - idx; } else cur = idx;
    if (mode == 3) cur = -1;
    int bw = (W - 20) / show, bx = 10 + (W - 20 - bw * show) / 2, span = hi - lo < 7 ? 7 : hi - lo;
    for (int i = 0; i < show; ++i) {
        int h = 10 + (pat[i] - lo) * 50 / span;
        int hot = on && (mode == 3 ? 0 : (mode == 1 ? total - 1 - i : i) == cur);
        rfill(bx + i * bw, VY + 64 - h, bw - 2, h, hot ? C_GREEN : blend(C_PANEL2, c, 6));
    }
    uint8_t st[48] = {0};
    int base = clampi(lo / 12 * 12, 0, 120);
    for (int k = 0; k < n; ++k) { int d = nt[k] - base; if (d >= 0 && d < 36) st[d] = 2; }
    if (on && cur >= 0 && cur < show) { int d = pat[mode == 1 ? total - 1 - cur : cur] - base; if (d >= 0 && d < 36) st[d] = 3; }
    vis_piano(6, VY + 74, W - 12, 52, 3, st, cols);
}

/* KEYS: the keys held, and how hard, how far bent, how much leaned on */
static void vis_keys(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat], cols[3] = {c, c, c};
    vis_panel();
    uint8_t st[48] = {0};
    for (unsigned k = 0; k < pws.n_held && k < 16; ++k) {
        int n = clampi((int)pws.held[k], 36, 83);
        st[n - 36] = 3;
    }
    vis_piano(6, VY + 6, W - 12, 64, 4, st, cols);
    struct { const char *n; float v; uint16_t col; } mt[4] = {
        {"VEL", pws.kb_vel, c}, {"MOD", pws.kb_mod, C_AMBER}, {"BEND", pws.kb_bend * 0.5f + 0.5f, C_GREEN}, {"PRESS", pws.kb_press, C_MOD}};
    for (int k = 0; k < 4; ++k) {
        int x = 10 + k * 57, bh = 40;
        text(x, VY + 74, mt[k].n, F_XS, C_DIM);
        rfill(x, VY + 90, 44, 14, C_PANEL2);
        fill(x, VY + 90, (int)(pw_clamp(mt[k].v, 0.0f, 1.0f) * 44.0f), 14, mt[k].col);
        (void)bh;
    }
    if (pws.n_held) { char b[12]; vis_note(b, pws.held[pws.n_held - 1]); text(8, VY + 106, b, F_S, C_WHITE); }
    else text(8, VY + 106, "PLAY A KEY", F_S, C_DIM);
    rfill(W - 30, VY + 108, 16, 12, pws.kb_gate ? C_GREEN : C_PANEL2);
    text_right(W - 36, VY + 106, "GATE", F_XS, C_DIM);
}

/* MIDI IN: the four CCs as sliders, the clock and run lights */
static void vis_midiin(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    for (int k = 0; k < 4; ++k) {
        int cc = clampi((int)(m->p[1 + k] + 0.5f), 0, 127), x = 14 + k * 56, val = pws.cc[cc], hot = pws.last_cc == cc;
        char b[12], *o = s_cpy(b, "CC "); s_int(o, cc);
        text_center(x + 18, VY + 6, b, F_XS, hot ? C_WHITE : C_DIM);
        rfill(x + 12, VY + 22, 12, 64, C_PANEL2);
        int hh = val * 64 / 127;
        fill(x + 12, VY + 86 - hh, 12, hh, hot ? C_WHITE : c);
        o = s_int(b, val); *o = 0;
        text_center(x + 18, VY + 88, b, F_S, hot ? C_WHITE : C_TEXT);
    }
    int clk = pws.mod[s].level[4] > 90, run = pw_out_last((unsigned)s, 5) > 2.5f;
    rfill(14, VY + 112, 12, 10, clk ? C_AMBER : C_PANEL2); text(30, VY + 110, "CLOCK", F_XS, C_DIM);
    rfill(84, VY + 112, 12, 10, run ? C_GREEN : C_PANEL2); text(100, VY + 110, "RUN", F_XS, C_DIM);
    char v[16]; pw_format_param((unsigned)s, 0, v);
    text_right(W - 8, VY + 110, v, F_XS, C_DIM);
}

/* MIDI OUT: the notes it plays (a piano roll moving left), the two CCs, the clock */
static void vis_midiout(int s)
{
    static const vis_src src[3] = {{1, 0}, {1, 1}, {1, 3}};
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    vis_sample(s, src, 3);
    char b[24], *o = s_cpy(b, "CH "); s_int(o, clampi((int)(m->p[0] + 0.5f), 1, 16));
    text(8, VY + 5, b, F_S, C_TEXT);
    text_right(W - 8, VY + 5, m->p[3] > 0.5f ? "CLOCK ON" : "CLOCK OFF", F_XS, m->p[3] > 0.5f ? C_GREEN : C_DIM);
    int top = VY + 24, bot = VY + 88, h = bot - top;
    float lo = 1e9f, hi = -1e9f;
    for (int k = 0; k < vh.n; ++k) if (vis_at(1, k) > 2.5f) { float v = vis_at(0, k) * 12.0f; if (v < lo) lo = v; if (v > hi) hi = v; }
    if (hi < lo) { lo = 0; hi = 12; }
    if (hi - lo < 12.0f) { float mid = (hi + lo) * 0.5f; lo = mid - 6.0f; hi = mid + 6.0f; }
    for (int g = 0; g <= 4; ++g) fill(6, top + g * h / 4, W - 12, 1, g == 2 ? C_PANEL2 : C_PANEL);
    for (int k = 0; k < vh.n; ++k) {
        if (vis_at(1, k) <= 2.5f) continue;
        int xx = 8 + k * (W - 16) / (VIS_N - 1), yy = bot - 3 - (int)((vis_at(0, k) * 12.0f - lo) / (hi - lo) * (float)(h - 6));
        fill(xx, clampi(yy, top, bot - 4), (W - 16) / (VIS_N - 1) + 1, 4, C_WHITE);
        fill(xx, clampi(yy, top, bot - 4) + 4, (W - 16) / (VIS_N - 1) + 1, 1, c);
    }
    static const char *const cn[2] = {"CC A", "CC B"};
    for (int q = 0; q < 2; ++q) {
        int y = VY + 98 + q * 14, cc = clampi((int)(m->p[1 + q] + 0.5f), 0, 127);
        float v = pw_clamp(((q ? pw_in_last((unsigned)s, 4) : pw_in_last((unsigned)s, 3)) + 5.0f) * 0.1f, 0, 1);
        char n2[16]; o = s_cpy(n2, cn[q]); *o++ = ' '; s_int(o, cc);
        text(8, y, n2, F_XS, C_DIM);
        rfill(52, y + 1, W - 62, 7, C_PANEL2);
        fill(52, y + 1, (int)(v * (float)(W - 62)), 7, c);
    }
    int clk = (fm1_millis() / 250u) & 1u;
    if (m->p[3] > 0.5f) fill(8, VY + VH - 10, 5, 5, clk ? C_AMBER : C_PANEL2);
}

/* NOISE: all four noises at once, a few times a second each */
static void vis_noise(int s)
{
    static int port; static uint32_t tap_ms;
    float (*nb)[VIS_N] = vh.h;   /* (the RAM is full: it borrows the strip charts' history) */
    static const char *const nm[6] = {"WHITE", "PINK", "BROWN", "DIGITAL", "DUST", "DTRIG"};
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    uint32_t now = fm1_millis();
    if (pv_demo) {   /* (the add list's preview: made up, a little different every moment) */
        static uint32_t rs;
        rs = (now / 90u) * 2654435761u + 12345u;
        float pk = 0, br = 0;
        vh.slot = -1;
        for (int k = 0; k < VIS_N; ++k) {
            float w;
            rs = rs * 1664525u + 1013904223u; w = (float)(rs >> 9) * (1.0f / 4194304.0f) - 1.0f;
            nb[0][k] = w * 0.9f;
            pk = pk * 0.8f + w * 0.45f; nb[1][k] = pk;
            br = pw_clamp(br + w * 0.12f, -1.0f, 1.0f) * 0.985f; nb[2][k] = br * 2.2f;
            if (k % 6 == 0) nb[3][k] = w;
            else nb[3][k] = nb[3][k - 1];
        }
    } else if (vh.slot != -1000 - s) { memset(vh.h, 0, sizeof vh.h); vh.slot = -1000 - s; port = 0; pw_set_scope((unsigned)s, 0); tap_ms = now; }
    else if (pws.scope_mod != (uint8_t)s || pws.scope_port != (uint8_t)port) { pw_set_scope((unsigned)s, (unsigned)port); tap_ms = now; }
    else if (now - tap_ms >= 70) {
        for (int k = 0; k < VIS_N; ++k) nb[port][k] = (float)pws.scope[(pws.scope_w - 1u - (unsigned)k * 3u) & (PW_SCOPE - 1u)] * (1.0f / 3000.0f);
        port = (port + 1) & 3; pw_set_scope((unsigned)s, (unsigned)port); tap_ms = now;
    }
    float sc = pv_demo ? 1.0f : pw_maxf(m->p[0] * 5.0f, 0.5f);
    int rh = (VH - 4) / 6;
    for (int r = 0; r < 6; ++r) {
        int y0 = VY + 2 + r * rh, mid = y0 + rh / 2;
        rfill(4, y0, W - 8, rh - 2, r & 1 ? RGB(18, 19, 25) : RGB(14, 15, 20));
        text(8, mid - 7, nm[r], F_XS, r >= 3 ? C_AMBER : c);
        if (r >= 4) {   /* the clicks, each where it happened over the last quarter second (the newest at the right) */
            float win = 0.25f;
            line(60, mid, 232, mid, 1, RGB(40, 42, 52));
            for (unsigned k = 0; k < 12; ++k) {
                float age = pv_demo ? (k < 5 ? (float)((k * 53u + 17u + fm1_millis() / 40u) % 250u) * 0.001f : 9.0f) : pw_live((unsigned)s, k);
                if (age > win) break;
                int xx = 230 - (int)(age / win * 170.0f);
                if (r == 4) { float a = pv_demo ? (k & 1 ? 0.8f : -0.5f) : pw_live((unsigned)s, 16u + k); int hh = clampi((int)(pw_absf(a) * (float)(rh / 2 - 2)) + 1, 1, rh / 2 - 2); fill(xx, a >= 0 ? mid - hh : mid, 2, hh, C_AMBER); }
                else fill(xx, y0 + 2, 3, rh - 6, C_AMBER);
            }
            continue;
        }
        int px = 0, py = mid;
        for (int k = 0; k < VIS_N; ++k) {
            int xx = 230 - k * 170 / (VIS_N - 1), yy = mid - (int)(nb[r][k] / sc * (float)(rh / 2 - 3));
            yy = clampi(yy, y0 + 1, y0 + rh - 4);
            if (k) line(px, py, xx, yy, 1, r >= 3 ? C_AMBER : c);
            px = xx; py = yy;
        }
    }
}

/* GRAVITY: the ball's bounces drawn out in time: the fall, then arcs that come lower and faster until it rattles to a stop.
 * The ball rides them while it falls; a mark on the floor at every landing. */
static void vis_gravity(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    int active = pw_live((unsigned)s, 1) > 0.5f;
    float el = pw_live((unsigned)s, 2), rest = pw_live((unsigned)s, 3);   /* the engine's own clock for the ball */
    float t1 = m->p[1] * 15.0f / pws.bpm, e = m->p[2];
    float dur[40], peak[40], T = t1;
    int nb = 0;
    for (float pk = 1.0f, d = 2.0f * t1 * e; nb < 40 && d >= 0.02f; ++nb) { dur[nb] = d; peak[nb] = pk; T += d; pk *= e * e; d *= e; }
    if (T <= 0.0f) T = 1.0f;
    int top = VY + 24, floor_y = VY + VH - 30, h = floor_y - top, x0 = 10, w = W - 20;
    int dropped = el > 0.0f;
    float fade = active ? 1.0f : pw_clamp(1.0f - rest, 0.0f, 1.0f);   /* what it lit stays for a second */
    fill(x0 - 2, floor_y, w + 4, 2, C_LINE);
    float t0 = 0;
    int px = x0, py = top;
    for (int k = -1; k < nb; ++k) {       /* k = -1: the first fall */
        float d = k < 0 ? t1 : dur[k], pk = k < 0 ? 1.0f : peak[k];
        float since = el - (t0 + d);                       /* >= 0: this arc is over */
        float glow = !dropped || el < t0 ? 0.0f : since < 0 ? 1.0f : pw_clamp(1.0f - since, 0.0f, 1.0f);
        if (!active) glow = pw_minf(glow, fade);
        uint16_t col = blend(C_PANEL2, c, (unsigned)(4 + glow * 12.0f));
        if (glow > 0.99f) col = C_WHITE;
        int steps = clampi((int)(d / T * (float)w / 2.0f), 3, 24);
        for (int i = 0; i <= steps; ++i) {
            float u = (float)i / (float)steps, hh = k < 0 ? 1.0f - u * u : 4.0f * u * (1.0f - u) * pk;
            int xx = x0 + (int)((t0 + u * d) / T * (float)w), yy = floor_y - (int)(hh * (float)h);
            if (k >= 0 || i) line(px, py, xx, yy, 1, col);
            px = xx; py = yy;
        }
        t0 += d;
        int hx = x0 + (int)(t0 / T * (float)w);
        rfill(hx - 1, floor_y - 2, 3, 6, dropped && el >= t0 ? blend(C_LINE, C_WHITE, (unsigned)(4 + glow * 12.0f)) : C_LINE);    /* a landing */
        if (active && el >= t0 - d && el < t0) {                          /* the ball, in flight */
            float u = (el - (t0 - d)) / d, hh = k < 0 ? 1.0f - u * u : 4.0f * u * (1.0f - u) * pk;
            int bx = x0 + (int)(el / T * (float)w), by = floor_y - (int)(hh * (float)h);
            rfill(bx - 4, by - 8, 9, 9, C_WHITE);
        }
    }
    if (!active && dropped) {                                             /* at rest on the floor, fading out */
        int bx = x0 + w - 4;
        rfill(bx - 4, floor_y - 8, 9, 9, blend(C_PANEL2, C_WHITE, (unsigned)(2 + fade * 14.0f)));
    } else if (!active) rfill(x0 - 4, top - 8, 9, 9, C_LINE);             /* waiting at the top */
    char b[28], *o = s_int(b, nb + 1); o = s_cpy(o, " BOUNCES");
    text(8, VY + 6, b, F_S, C_TEXT);
    text_right(W - 8, VY + 8, active ? "FALLING" : "WAITING FOR A DROP", F_XS, active ? C_WHITE : C_DIM);
}

/* CLOCK: one bar of quarter-sixteenths per output, a mark wherever it pulsed */
static void vis_clock(int s)
{
    static uint8_t seen[4]; static int last_head = -1;
    uint8_t (*pat)[64] = (uint8_t (*)[64])(void *)vh.h;   /* (the RAM is full: it borrows the strip charts' history) */
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    static const uint8_t ports[4] = {0, 1, 2, 4};   /* CLK, DIV, MULT and the bent WARP (the engine counts these four, in this order) */
    const int rows = 4;
    vis_panel();
    if (vh.slot != -100 - s) { memset(vh.h, 0, sizeof vh.h); last_head = -1; vh.slot = -100 - s; }
    int head = pws.playing ? ((int)((pws.sixteenths + 15u) & 15u) * 4 + (int)(pws.tick_phase * 3.99f)) & 63 : -1;
    if (head >= 0) {
        if (last_head >= 0) for (int j = (last_head + 1) & 63; j != ((head + 1) & 63); j = (j + 1) & 63) for (int r = 0; r < 4; ++r) pat[r][j] = 0;
        else for (int r = 0; r < 4; ++r) pat[r][head] = 0;
        last_head = head;
    } else last_head = -1;
    int rh = 28;
    for (int r = 0; r < rows; ++r) {
        int y = VY + 6 + r * rh, port = ports[r], lvl = m->level[port];
        if (head >= 0 && m->edge[r] != seen[r]) pat[r][head] = 1;
        seen[r] = m->edge[r];
        text(8, y + 1, pw_types[m->type].out[port], F_XS, C_DIM);
        rfill(8, y + 14, 14, 10, blend(C_PANEL2, c, (unsigned)(lvl * 16 / 255)));
        for (int j = 0; j < 64; ++j) {
            int x = 40 + j * 3, beat = (j & 15) == 0, sx = (j & 3) == 0;
            fill(x, y + 4, 2, 20, pat[r][j] ? c : sx ? C_PANEL2 : C_PANEL);
            if (beat && !pat[r][j]) fill(x, y + 4, 2, 20, C_LINE);
        }
        if (head >= 0) fill(40 + head * 3 - 1, y + 1, 4, 26, C_WHITE);
    }
    if (!pws.playing) { rfill(90, VY + 52, 120, 18, C_PANEL2); text_center(150, VY + 55, "PRESS PLAY", F_S, C_TEXT); }
}

/* OUTPUT: level meters with a peak hold and a clip lamp */
static void vis_output(int s)
{
    static float vu[2], hold[2]; static uint32_t hold_ms[2], clip_ms[2];
    vis_panel();
    uint32_t now = fm1_millis();
    float pk[2] = {pws.out_peak_l, pws.out_peak_r};
    for (int ch = 0; ch < 2; ++ch) {
        if (pk[ch] > vu[ch]) vu[ch] = pk[ch]; else vu[ch] *= 0.85f;
        if (pk[ch] >= hold[ch]) { hold[ch] = pk[ch]; hold_ms[ch] = now; }
        else if (now - hold_ms[ch] > 1500u) hold[ch] *= 0.93f;
        if (pk[ch] >= 0.98f) clip_ms[ch] = now;
        int y = VY + 22 + ch * 44, x0 = 34, w = 184, segs = 46;
        text(10, y + 10, ch ? "R" : "L", F_M, C_TEXT);
        float db = 6.0206f * pw_log2(pw_maxf(vu[ch], 1e-4f)), dh = 6.0206f * pw_log2(pw_maxf(hold[ch], 1e-4f));
        int lit = clampi((int)((db + 48.0f) / 48.0f * (float)segs + 0.5f), 0, segs), hp = clampi((int)((dh + 48.0f) / 48.0f * (float)segs + 0.5f), 0, segs);
        for (int k = 0; k < segs; ++k) {
            float d = -48.0f + (float)k * 48.0f / (float)segs;
            uint16_t col = d > -3.0f ? C_RED : d > -12.0f ? C_AMBER : C_GREEN;
            fill(x0 + k * w / segs, y, w / segs - 1, 26, k < lit || k == hp - 1 ? col : C_PANEL2);
        }
        int clipped = now - clip_ms[ch] < 1500u && clip_ms[ch];
        rfill(x0 + w + 4, y, 14, 26, clipped ? C_RED : C_PANEL2);
        char b[16], *o = b;
        if (hold[ch] < 0.003f) o = s_cpy(o, "-inf"); else { if (dh < 0) { *o++ = '-'; o = s_int(o, (int)(-dh + 0.5f)); } else { o = s_int(o, (int)(dh + 0.5f)); } }
        s_cpy(o, " dB");
        text(x0, y + 28, b, F_XS, clipped ? C_RED : C_DIM);
    }
    static const int ticks[5] = {-48, -24, -12, -6, 0};
    for (int k = 0; k < 5; ++k) {
        char b[8], *o = b;
        if (ticks[k]) { *o++ = '-'; o = s_int(o, -ticks[k]); } else o = s_int(o, 0);
        *o = 0;
        text_center(34 + (ticks[k] + 48) * 184 / 48, VY + 112, b, F_XS, C_DIM);
    }
    text(8, VY + 4, "OUTPUT LEVEL", F_XS, C_DIM);
    {   /* CLIP PROTECT: 0.0 dB while the limiter leaves the signal alone, the gain it takes off (-2.4 dB) while it catches a peak */
        const pw_module *om = &pws.mod[s];
        int prot = om->used == 1 && om->type == PW_T_OUTPUT ? om->p[1] >= 0.5f : pws.clip_on;
        char b[24], *o = s_cpy(b, "CLIP PROTECT ");
        int gt = pw_clip_gr_tenths();
        if (!prot) s_cpy(o, "OFF");
        else { o = s_tenths(o, gt); s_cpy(o, " dB"); }
        text_right(W - 8, VY + 4, b, F_XS, !prot ? C_DIM : gt < 0 ? C_AMBER : C_TEXT);
    }
}

static float vis_sqrt(float x) { return x <= 0 ? 0.0f : pw_exp2(0.5f * pw_log2(x)); }
/* TAPE: a reel-to-reel / cassette deck. Both reels turn the same way (anticlockwise in play); the tape pack moves from
 * the supply (left) to the take-up (right), so the supply spins faster as it empties and the take-up slows as it
 * fills; the tape runs round the guides and across the head; wow bends it; STOP brakes the reels, SPEED (varispeed)
 * runs them faster or slower and shows the pitch it makes; a counter turns with the tape. */
static void vis_tape(int s)
{
    static float ang[2], pos, cnt; static uint32_t last_ms;
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    uint32_t now = fm1_millis();
    float spd = pw_live((unsigned)s, 0), eff = pw_live((unsigned)s, 2), dtm = (float)(now - last_ms);
    last_ms = now;
    if (dtm > 200.0f) dtm = 0;
    const float rmax = 30.0f, rmin = 12.0f, a2 = rmax * rmax - rmin * rmin;
    pos += eff * dtm * (1.0f / 60000.0f);
    if (pos > 1.0f) pos -= 1.0f; else if (pos < 0) pos += 1.0f;
    cnt += eff * dtm * 0.002f; if (cnt > 9999.0f) cnt -= 9999.0f;
    float rp[2] = {vis_sqrt(rmax * rmax - a2 * pos), vis_sqrt(rmin * rmin + a2 * pos)};
    int cy = VY + 54, cx[2] = {56, W - 56}, R = 36;
    for (int k = 0; k < 2; ++k) {
        ang[k] -= eff * dtm * 0.0004f * (22.0f / rp[k]);        /* v = omega r: the fuller reel turns slower */
        if (ang[k] < 0) ang[k] += 1.0f; else if (ang[k] > 1.0f) ang[k] -= 1.0f;
        int px = cx[k] + R, py = cy, qx = cx[k] + (int)rp[k], qy = cy;
        for (int q = 1; q <= 32; ++q) {                           /* the flange, and the tape pack on the hub */
            float ph = (float)q / 32.0f;
            float co = pw_sin1(ph + 0.25f), si = pw_sin1(ph);
            int ex = cx[k] + (int)(co * (float)R), ey = cy + (int)(si * (float)R);
            line(px, py, ex, ey, 1, C_LINE);
            px = ex; py = ey;
            ex = cx[k] + (int)(co * rp[k]); ey = cy + (int)(si * rp[k]);
            line(qx, qy, ex, ey, 2, mid_of(c));
            qx = ex; qy = ey;
        }
        for (int ring = 1; ring <= 2; ++ring) {                   /* the windings: rings inside the pack's edge */
            float rr = rp[k] - 4.0f * (float)ring;
            if (rr < 14.0f) break;
            int ox = cx[k] + (int)rr, oy = cy;
            for (int q = 1; q <= 24; ++q) { float ph = (float)q / 24.0f; int ex = cx[k] + (int)(pw_sin1(ph + 0.25f) * rr), ey = cy + (int)(pw_sin1(ph) * rr); line(ox, oy, ex, ey, 1, dim_of(c)); ox = ex; oy = ey; }
        }
        px = cx[k] + 11; py = cy;
        for (int q = 1; q <= 16; ++q) {                           /* the hub, with its six teeth */
            float ph = (float)q / 16.0f;
            int ex = cx[k] + (int)(pw_sin1(ph + 0.25f) * 11.0f), ey = cy + (int)(pw_sin1(ph) * 11.0f);
            line(px, py, ex, ey, 1, C_TEXT);
            px = ex; py = ey;
        }
        for (int sp = 0; sp < 6; ++sp) {
            float ph = ang[k] + (float)sp / 6.0f;
            line(cx[k] + (int)(pw_sin1(ph + 0.25f) * 5.0f), cy + (int)(pw_sin1(ph) * 5.0f), cx[k] + (int)(pw_sin1(ph + 0.25f) * 11.0f), cy + (int)(pw_sin1(ph) * 11.0f), 2, c);
        }
    }
    /* the tape: off the supply pack, round the guide, across the head, round the next guide, onto the take-up pack */
    int gy = cy + R + 6, gx[2] = {cx[0] + 38, cx[1] - 38};
    int ax = cx[0] + (int)(rp[0] * 0.45f), ay = cy + (int)(rp[0] * 0.89f);
    int bx = cx[1] - (int)(rp[1] * 0.45f), by = cy + (int)(rp[1] * 0.89f);
    line(ax, ay, gx[0], gy, 2, C_TEXT);
    float wow = m->p[1], flut = m->p[2], tt = (float)(now % 2500u) / 2500.0f, ft = (float)(now % 230u) / 230.0f;
    int px = gx[0], py = gy;
    for (int i = 1; i <= 16; ++i) {                               /* across the front, swaying with the wow and the flutter */
        float u = (float)i / 16.0f;
        int xx = gx[0] + (int)((float)(gx[1] - gx[0]) * u);
        int yy = gy - (int)(pw_sin1(u * 1.5f + tt) * wow * 5.0f * spd + pw_sin1(u * 6.0f + ft) * flut * 1.5f * spd);
        line(px, py, xx, yy, 2, C_TEXT);
        px = xx; py = yy;
    }
    line(px, py, bx, by, 2, C_TEXT);
    for (int k = 0; k < 2; ++k) rfill(gx[k] - 3, gy - 3, 6, 6, C_PANEL2);
    /* the head: lit amber with the drive, and a level bar under it */
    float lv = pw_clamp(pw_absf(pw_out_last((unsigned)s, 0)) * 0.2f, 0.0f, 1.0f);
    rfill(W / 2 - 14, gy + 3, 28, 9, C_PANEL2);
    rfill(W / 2 - 3, gy + 3, 6, 9, lv > 0.85f ? C_RED : m->p[0] > 0.5f ? C_AMBER : C_LINE);
    fill(W / 2 - 14, gy + 15, (int)(28.0f * lv), 2, lv > 0.85f ? C_RED : c);
    /* the readout: the speed, or the pitch a varispeed makes; a four-digit counter */
    char b[20];
    if (spd < 0.98f) { char *o = s_int(b, (int)(spd * 100.0f + 0.5f)); s_cpy(o, "%"); text_center(W / 2, VY + 6, b, F_M, C_AMBER); }
    else {
        float st = 12.0f * pw_log2(pw_maxf(eff / pw_maxf(spd, 0.001f), 0.01f));
        int sti = (int)(st < 0 ? st - 0.5f : st + 0.5f);
        char *o = sti > 0 ? s_cpy(b, "+") : b;
        o = s_int(o, sti); s_cpy(o, " ST");
        text_center(W / 2, VY + 6, sti ? b : "1X", F_M, sti ? C_AMBER : C_TEXT);
    }
    text_center(W / 2, VY + 30, spd < 0.02f ? "STOPPED" : spd < 0.98f ? "BRAKING" : "SPEED", F_XS, C_DIM);
    { char d[6]; int n = (int)cnt; d[0] = (char)('0' + n / 1000 % 10); d[1] = (char)('0' + n / 100 % 10); d[2] = (char)('0' + n / 10 % 10); d[3] = (char)('0' + n % 10); d[4] = 0;
      text(8, VY + 4, d, F_S, C_DIM); }
    /* hiss and age: specks on the tape, more of them the more of each */
    unsigned h = (unsigned)(m->p[4] * 30.0f);
    for (unsigned k = 0; k < h; ++k) { unsigned r2 = (k * 2654435761u + (now >> 6) * 40503u) >> 8; fill(10 + (int)(r2 % (W - 20)), VY + VH - 18 + (int)((r2 >> 9) % 12), 1, 1, C_LINE); }
}

/* FRACTURE: the last bars as slices; when it fires one slice is held and played again and again, maybe backwards */
static void vis_fracture(int s)
{
    static uint8_t ring[16]; static uint32_t ring_ms; static int was, last_s = -1; static uint8_t frozen[4];
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    uint32_t now = fm1_millis();
    int mode = (int)(m->p[6] + 0.5f);
    int act = (int)pw_live((unsigned)s, 0), rev = act == 2;
    float pos = pw_live((unsigned)s, 1);
    if (last_s != s) { memset(ring, 0, sizeof ring); last_s = s; was = 0; }
    if ((!act || mode == 1) && now - ring_ms >= 90) {            /* what came in, the last 1.4 s */
        ring_ms = now;
        memmove(ring, ring + 1, 15);
        ring[15] = (uint8_t)clampi(m->in_level[0] >= 1 ? m->in_level[0] : (int)(pw_absf(pw_in_last((unsigned)s, 0)) * 51.0f), 0, 255);
    }
    if (act && !was) memcpy(frozen, ring + 12, 4);
    was = act;
    int top = VY + 28, bh = 66;
    for (int k = 0; k < 16; ++k) {
        int x = 8 + k * 14;
        if (mode == 1) {                                          /* REVERSE: the head runs back through what came in */
            int hh = 4 + ring[k] * (bh - 6) / 255, under = (float)(k + 1) / 16.0f <= pos + 0.0625f && (float)k / 16.0f >= pos - 0.0625f;
            fill(x, top + bh - hh, 12, hh, under ? C_WHITE : blend(C_PANEL2, c, 12));
            continue;
        }
        int v = act ? frozen[(rev ? 3 - (k & 3) : (k & 3))] : ring[k], hh = 4 + v * (bh - 6) / 255;
        fill(x, top + bh - hh, 12, hh, act ? C_AMBER : c);
        if (act && (k & 3) == 0) fill(x - 2, top - 2, 1, bh + 4, C_LINE);
    }
    if (mode == 1) {
        fill(8 + (int)(pw_clamp(pos, 0.0f, 1.0f) * 223.0f), top - 4, 3, bh + 8, C_AMBER);
        text(8, VY + 6, "BACKWARDS", F_M, C_AMBER);
    } else if (act) {
        int slot = (int)(pos * 4.0f);
        if (slot > 3) slot = 3;
        for (int rep = 0; rep < 4; ++rep) if (rep * 4 + slot < 16 && ((now / 120u) & 3u) == (unsigned)rep) fill(8 + (rep * 4 + slot) * 14, top - 4, 12, 3, C_WHITE);
        text(8, VY + 6, mode == 2 ? (rev ? "OUT OF ORDER, BACK" : "OUT OF ORDER") : rev ? "BACKWARDS" : "REPEAT", F_M, C_AMBER);
    } else text(8, VY + 6, "LISTENING", F_M, C_DIM);
    static const char *const l1[3] = {"SLICE", "TIME", "CHANCE"}, *const l2[3] = {"CHANCE", "FBK", "DEPTH"};
    static const uint8_t k1[3] = {0, 7, 1}, k2[3] = {1, 8, 9};
    text(8, VY + 110, l1[mode], F_XS, C_DIM);
    char v[16]; pw_format_param((unsigned)s, k1[mode], v);
    text(58, VY + 106, v, F_S, C_TEXT);
    char b[12]; pw_format_param((unsigned)s, k2[mode], b);
    text(120, VY + 110, l2[mode], F_XS, C_DIM); text(170, VY + 106, b, F_S, C_TEXT);
}

/* SEQ SWITCH: the inputs that are being passed to OUT, and where IN 1 is being sent */
static void vis_switch(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    int steps = clampi((int)(m->p[0] + 0.5f), 1, 4);
    text(8, VY + 4, "4 > 1", F_XS, C_DIM);
    text(8, VY + 66, "1 > 4", F_XS, C_DIM);
    for (int k = 0; k < 4; ++k) {
        float g = pw_live((unsigned)s, (unsigned)k);
        int y = VY + 16 + k * 12, on = k < steps;
        /* 4 > 1: IN k to the one OUT */
        char nm[8] = "IN "; nm[3] = (char)('1' + k); nm[4] = 0;
        text(10, y - 2, nm, F_XS, on ? C_TEXT : C_LINE);
        line(46, y + 5, 150, VY + 34, g > 0.5f ? 3 : 1, blend(C_PANEL2, c, (unsigned)(g * 16.0f)));
        /* 1 > 4: IN 1 to OUT k */
        int y2 = VY + 80 + k * 12;
        line(46, VY + 96, 150, y2 + 5, g > 0.5f ? 3 : 1, blend(C_PANEL2, c, (unsigned)(g * 16.0f)));
        char on_[8] = "OUT "; on_[4] = (char)('1' + k); on_[5] = 0;
        text(160, y2 - 2, on_, F_XS, on ? C_TEXT : C_LINE);
    }
    text(160, VY + 26, "OUT", F_S, C_WHITE);
    text(10, VY + 89, "IN 1", F_XS, C_TEXT);
    if (!pw_input_cables((unsigned)s, 4)) text_right(W - 8, VY + 4, "CLOCK IT TO STEP", F_XS, C_DIM);
}

/* LIMITER: input against output, the ceiling, how hard it is working and the pump */
static void vis_limiter(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int cx, cy, cw;
    vis_curve_frame(&cx, &cy, &cw);
    float drive = m->p[0], ceil_db = m->p[1];
    int mode = (int)(m->p[3] + 0.5f);
    int px = 0, py = 0;
    for (int i = 0; i <= 48; ++i) {
        float db = -36.0f + (float)i, x = pw_exp2((db + drive) * 0.16609640f), y;     /* (in units of 5 V) */
        float cl = pw_exp2(ceil_db * 0.16609640f);
        if (mode == 2) { float u = x / cl, f = pw_frac((u + 1.0f) * 0.25f); y = cl * (1.0f - 4.0f * pw_absf(f - 0.5f)); y = pw_absf(y); }
        else if (mode == 1) y = cl * pw_tanh(pw_minf(x, cl * 1.5f) / cl);
        else y = pw_minf(x, cl);
        float odb = 6.0206f * pw_log2(pw_maxf(y, 1e-4f));
        int xx = cx + (int)((db + 36.0f) / 48.0f * (float)(cw - 1)), yy = cy + cw - 1 - (int)(pw_clamp((odb + 36.0f) / 48.0f, 0.0f, 1.0f) * (float)(cw - 1));
        if (i) line(px, py, xx, yy, 2, c);
        px = xx; py = yy;
    }
    int cyl = cy + cw - 1 - (int)(pw_clamp((ceil_db + 36.0f) / 48.0f, 0.0f, 1.0f) * (float)(cw - 1));
    fill(cx, cyl, cw, 1, C_AMBER);
    int x0 = cx + cw + 10;
    text(x0, cy + 2, "REDUCING", F_XS, C_DIM);
    float gr = pw_clamp(pw_out_last((unsigned)s, 1) * 0.2f, 0.0f, 1.0f);
    rfill(x0, cy + 16, 90, 10, C_PANEL2); fill(x0, cy + 16, (int)(gr * 90.0f), 10, C_AMBER);
    text(x0, cy + 34, "CEILING", F_XS, C_DIM);
    char v[16]; pw_format_param((unsigned)s, 1, v); text(x0, cy + 46, v, F_S, C_AMBER);
    text(x0, cy + 66, "PUMP", F_XS, C_DIM);
    float pump = m->p[4], pp = pw_live((unsigned)s, 1);
    int ox = -1, oy = 0;
    for (int i = 0; i <= 30; ++i) {
        float u = (float)i / 30.0f, g = 1.0f - pump * (1.0f - u) * (1.0f - u);
        int xx = x0 + i * 3, yy = cy + 96 - (int)(g * 24.0f);
        if (ox >= 0) line(ox, oy, xx, yy, 1, pump > 0.01f ? c : C_LINE);
        ox = xx; oy = yy;
    }
    if (pump > 0.01f && pp < 1.0f) fill(x0 + (int)(pp * 90.0f), cy + 70, 2, 30, C_WHITE);
}

/* OTT: each band's level, the squeeze it gets (up green, down amber) */
static void vis_ott(int s)
{
    static const char *const nm[3] = {"LOW", "MID", "HIGH"};
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    int top = VY + 26, bot = VY + VH - 14, h = bot - top;
    for (int b = 0; b < 3; ++b) {
        int x = 10 + b * 74;
        float lvl = pw_live((unsigned)s, (unsigned)(3 + b)), g = pw_live((unsigned)s, (unsigned)b);
        text_center(x + 30, VY + 6, nm[b], F_S, c);
        /* the level: -60 .. 0 dB, the downward zone (above -18) and the upward (below -36) tinted */
        rfill(x, top, 24, h, C_PANEL2);
        fill(x, top, 24, h * 42 / 60, RGB(46, 34, 20));
        fill(x, top + h * 24 / 60 + h * 0, 0, 0, C_BG);
        fill(x, top + h * 42 / 60, 24, h - h * 42 / 60, RGB(20, 40, 28));
        int lh = clampi((int)((lvl + 60.0f) / 60.0f * (float)h), 0, h);
        fill(x + 4, top + h - lh, 16, lh, c);
        /* the gain: zero in the middle, up or down ±24 dB */
        int gx = x + 32, mid = top + h / 2, gh = clampi((int)(pw_clamp(g, -24.0f, 24.0f) / 24.0f * (float)(h / 2)), -h / 2, h / 2);
        rfill(gx, top, 14, h, C_PANEL);
        fill(gx, mid, 14, 1, C_LINE);
        if (gh >= 0) fill(gx + 2, mid - gh, 10, gh, C_GREEN); else fill(gx + 2, mid, 10, -gh, C_AMBER);
        char t[12], *o = t;
        if (g >= 0) *o++ = '+'; else { *o++ = '-'; }
        o = s_int(o, (int)((g < 0 ? -g : g) + 0.5f)); *o = 0;
        text_center(x + 30, bot + 1, t, F_XS, g >= 0.5f ? C_GREEN : g <= -0.5f ? C_AMBER : C_DIM);
    }
    text(8, VY + 0, "", F_XS, C_DIM);
    text_right(W - 6, VY + 40, "UP", F_XS, C_GREEN);
    text_right(W - 6, VY + 60, "DOWN", F_XS, C_AMBER);
}


/* which visualization a module gets from here (0: none, wired jacks instead) */
enum { VIS_NONE, VIS_WAVE, VIS_STRIP, VIS_SHAPER, VIS_COMP, VIS_ECHO, VIS_VERB, VIS_SWEEP, VIS_VOWEL, VIS_CHAOS, VIS_MODSEQ, VIS_CHORDSTEPS, VIS_GRAVITY, VIS_TURING, VIS_ATTEN,
       VIS_NOISE, VIS_CLOCK, VIS_OUTPUT, VIS_TAPE, VIS_FRACTURE, VIS_SWITCH, VIS_QUANT, VIS_CHORD, VIS_KEYS, VIS_MIDIIN, VIS_LIMITER, VIS_OTT, VIS_ARP, VIS_MIDIOUT, VIS_XFADE, VIS_SPACE, VIS_COMPARE, VIS_SHIFT, VIS_WAVESET, VIS_MUTATE };
static int vis_kind(unsigned type)
{
    switch (type) {
    /* (a scope of the output: only for modules with few knobs; the rest show every knob page at once, which says more) */
    case PW_T_WAVETABLE: case PW_T_COMPLEX: case PW_T_ADDITIVE: case PW_T_MACRO: case PW_T_MICROPERC:
    case PW_T_PSHIFT: case PW_T_RING: case PW_T_AUDIOIN:
    case PW_T_VCA: case PW_T_LPG: return pw_types[type].n_param > 4 ? VIS_NONE : VIS_WAVE;
    case PW_T_FUNC: case PW_T_SH: case PW_T_CVMIX: case PW_T_LOGIC: case PW_T_COIN: case PW_T_ENVFOLLOW:
    case PW_T_LISTENER: case PW_T_GATEDLY: return VIS_STRIP;
    case PW_T_ARP: return VIS_ARP;
    case PW_T_COMPARE: return VIS_COMPARE;
    case PW_T_SHIFTREG: return VIS_SHIFT;
    case PW_T_WAVESET: return VIS_WAVESET;
    case PW_T_MUTATE: return VIS_MUTATE;
    case PW_T_NOISE: return VIS_NOISE;
    case PW_T_CLOCK: return VIS_CLOCK;
    case PW_T_OUTPUT: return VIS_OUTPUT;
    case PW_T_TAPE: return VIS_TAPE;
    case PW_T_FRACTURE: return VIS_FRACTURE;
    case PW_T_SWITCH: return VIS_SWITCH;
    case PW_T_QUANT: return VIS_QUANT;
    case PW_T_CHORD: return VIS_CHORD;
    case PW_T_KEYS: return VIS_KEYS;
    case PW_T_MIDIIN: return VIS_MIDIIN;
    case PW_T_MIDIOUT: return VIS_MIDIOUT;
    case PW_T_LIMITER: return VIS_LIMITER;
    case PW_T_OTT: return VIS_OTT;
    case PW_T_DIST: case PW_T_FOLDER: return VIS_SHAPER;
    case PW_T_COMP: return VIS_COMP;
    case PW_T_DELAY: return VIS_ECHO;
    case PW_T_REVERB: return VIS_VERB;
    case PW_T_SWIRL: case PW_T_HARMTREM: return VIS_SWEEP;
    case PW_T_VOWEL: return VIS_VOWEL;
    case PW_T_CHAOS: return VIS_CHAOS;
    case PW_T_MODSEQ: return VIS_MODSEQ;
    case PW_T_CHORDSTEPS: return VIS_CHORDSTEPS;
    case PW_T_GRAVITY: return VIS_GRAVITY;
    case PW_T_TURING: return VIS_TURING;
    case PW_T_ATTEN: return VIS_ATTEN;
    case PW_T_XFADE: return VIS_XFADE;
    case PW_T_SPACE: return VIS_SPACE;
    default: return VIS_NONE;
    }
}

/* ---- the pictures for modules with many knobs and no picture of their own: they sit in the free space above the knob pages
 * (the edit page) or fill the preview (the add list). Drawn into the box x,y,w,h. */
static int extra_ok(unsigned type)
{
    return vis_kind(type) == VIS_NONE && ((pw_types[type].n_param > 4 && pw_types[type].n_out > 0) || type == PW_T_BURST);   /* (BURST has four knobs but a picture) */
}
static uint32_t ex_hash(uint32_t v) { v ^= v >> 16; v *= 0x7feb352dU; v ^= v >> 15; v *= 0x846ca68bU; v ^= v >> 16; return v; }
static void view_extra(int s, int x, int y, int w, int h)
{
    const pw_module *m = &pws.mod[s];
    const pw_type *t = &pw_types[m->type];
    uint16_t c = cat_color[t->cat];
    rfill(x, y, w, h, RGB(16, 17, 22));
    int ix = x + 4, iw = w - 8, iy = y + 3, ih = h - 6, mid = iy + ih / 2;
    if (ih < 6 || iw < 20) return;
    switch (m->type) {
    case PW_T_ADDITIVE: {   /* the 32 partials: BRIGHT's slope, SHAPE's odds and evens, PEAK and RES, COMB's notches */
        float shape = m->p[1], br = m->p[2], pk = m->p[4], res = m->p[5], comb = m->p[6];
        for (int k = 1; k <= 32; ++k) {
            float a = pw_exp2(-(float)(k - 1) * (1.25f - br) * 0.3f);
            if (k & 1) a *= pw_minf(1.0f, 0.35f + (1.0f - shape) * 1.3f); else a *= pw_minf(1.0f, shape * 2.0f);
            float d = (float)k - (1.0f + pk * 24.0f);
            a *= 1.0f + res * 3.0f * pw_exp2(-d * d * 0.25f);
            a *= 1.0f - comb * 0.9f * (0.5f + 0.5f * pw_sin1((float)k * 0.19f + 0.25f));
            int bh = clampi((int)(a * (float)ih * 0.8f), 1, ih);
            int bx = ix + (k - 1) * iw / 32;
            fill(bx, iy + ih - bh, iw / 32 - 1 > 0 ? iw / 32 - 1 : 1, bh, k == 1 ? C_WHITE : c);
        }
    } break;
    case PW_T_CZ: {   /* the tone as it plays: the wave at its DCW, then the DCW (the brightness) and the loudness of a tap (key down for a second, then up) for both lines */
        float ea[2][2][64], wv[48];
        int same2[2];
        float secs = 8.0f;   /* the window: the shortest of 0.25 s to 8 s in which every curve has died away after the key comes up */
        for (float w = 0.25f; w <= 8.0f; w *= 2.0f) {
            int done = 1;
            for (unsigned kd = 1; kd <= 2; ++kd) for (unsigned ln = 0; ln < 2; ++ln) {
                pw_cz_env((unsigned)s, kd, ln, w, w * 0.45f, ea[0][0], 64);
                for (int k = 58; k < 64; ++k) if (ea[0][0][k] > 0.03f) done = 0;
            }
            if (done) { secs = w; break; }
        }
        float hold = secs * 0.45f;
        same2[0] = pw_cz_env((unsigned)s, 1, 0, secs, hold, ea[0][0], 64);
        pw_cz_env((unsigned)s, 1, 1, secs, hold, ea[0][1], 64);
        same2[1] = pw_cz_env((unsigned)s, 2, 0, secs, hold, ea[1][0], 64);
        pw_cz_env((unsigned)s, 2, 1, secs, hold, ea[1][1], 64);
        pw_cz_wave((unsigned)s, wv, 48);
        int ww = iw / 4, gw = (iw - ww - 8) / 2;
        fill(ix, mid, ww, 1, C_LINE);
        text(ix + 1, iy, "WAVE", F_XS, C_DIM);
        for (int k = 1; k < 48; ++k) line(ix + (k - 1) * ww / 48, mid - (int)(wv[k - 1] * (float)(ih / 2 - 2)), ix + k * ww / 48, mid - (int)(wv[k] * (float)(ih / 2 - 2)), 1, c);
        for (int g = 0; g < 2; ++g) {
            int gx = ix + ww + 8 + g * gw + (g ? 4 : 0), w2 = gw - 4;
            fill(gx - 2, iy, 1, ih, C_LINE);
            fill(gx, iy + ih - 1, w2, 1, C_LINE);
            fill(gx + w2 * 45 / 100, iy, 1, ih, C_LINE);   /* (where the key comes up) */
            text(gx + 2, iy, g ? "LOUDNESS" : "DCW", F_XS, C_DIM);
            if (g) { char ts[12], *o = ts; int ms = (int)(secs * 1000.0f + 0.5f); if (ms >= 1000) { o = s_int(o, ms / 1000); if (ms % 1000) { *o++ = '.'; *o++ = (char)('0' + ms % 1000 / 100); } } else o = s_int(o, ms); s_cpy(o, ms >= 1000 ? "S" : "MS"); text_right(gx + w2, iy, ts, F_XS, C_DIM); }
            int same = same2[g];
            for (int ln = same == 2 ? 0 : 1; ln >= 0; --ln) {
                uint16_t lc = ln ? C_AMBER : c;
                for (int k = 0; k < w2; ++k) {
                    int yy = iy + ih - 2 - (int)(ea[g][ln][k * 63 / (w2 > 1 ? w2 - 1 : 1)] * (float)(ih - 4));
                    if (!ln && same != 2) fill(gx + k, yy, 1, 2, lc);
                    else if (same == 2) { fill(gx + k, yy, 1, iy + ih - 1 - yy, dim_of(lc)); fill(gx + k, yy, 1, 2, lc); }
                    else fill(gx + k, yy, 1, 2, lc);
                }
            }
        }
    } break;
    case PW_T_BUTTON: {   /* the 27 keys: the ones this BUTTON listens to in its color, the ones held now white; its lamps under them */
        unsigned fa = (unsigned)clampi((int)(m->p[0] + 0.5f), 1, 27), fb = (unsigned)clampi((int)(m->p[1] + 0.5f), 1, 27);
        if (fa > fb) { unsigned t = fa; fa = fb; fb = t; }
        int kw = iw / 27, kx = ix + (iw - kw * 27) / 2, ky = iy + 6, kh = ih / 2 - 4;
        for (unsigned k = 0; k < 27; ++k) {
            int on = k + 1u >= fa && k + 1u <= fb, held = (int)((pws.btn_keys >> k) & 1u);
            fill(kx + (int)k * kw, ky, kw - 1, kh, held && on ? C_WHITE : on ? mid_of(c) : C_PANEL2);
        }
        char kt[16], *q = s_int(kt, (int)fa); if (fa != fb) { *q++ = '-'; q = s_int(q, (int)fb); } *q = 0;
        text(ix + 2, ky + kh + 4, kt, F_S, C_DIM);
        static const char *const lamp[4] = {"OUT", "TRIG", "HOLD", "DBL"};
        for (int o = 0; o < 4; ++o) {
            int lx = ix + 70 + o * 40, hot = pw_out_last((unsigned)s, (unsigned)o) > 0.5f;
            fill(lx, ky + kh + 5, 6, 6, hot ? c : C_PANEL2);
            text(lx + 9, ky + kh + 4, lamp[o], F_XS, hot ? C_WHITE : C_DIM);
        }
    } break;
    case PW_T_MICROPERC: {   /* one hit: its ring, DECAY long (CRACKLE: a handful of ticks), TONE sets how fast it rings */
        int model = clampi((int)(m->p[0] + 0.5f), 0, 5);
        float dec = m->p[2], tone = m->p[3], sc = pw_clamp(dec * 2.5f, 0.02f, 1.0f);
        int px = ix, py = mid;
        for (int k = 0; k < iw; ++k) {
            float u = (float)k / (float)iw * sc, env = pw_exp2(-u / pw_maxf(dec, 0.003f) * 4.0f), v;
            if (model == 5) { uint32_t hh = ex_hash((uint32_t)k * 31u); v = (hh & 15u) == 0 ? ((hh >> 8) & 1u ? 1.0f : -1.0f) * env : 0; }
            else if (model == 3) v = k < 3 ? 1.0f : 0;
            else v = env * pw_sin1(u * (20.0f + tone * 400.0f) * (model == 1 ? 1.0f - u * 2.0f : 1.0f) * (model == 4 ? 2.7f : 1.0f));
            int yy = mid - (int)(v * (float)(ih / 2 - 1));
            if (k) line(px, py, ix + k, yy, 1, c);
            px = ix + k; py = yy;
        }
    } break;
    case PW_T_STRING: {   /* MODAL PERC: its partials, where they sit (left low, right high) and how loud (a string: its harmonics) */
        int model = clampi((int)(m->p[4] + 0.5f), 0, 11);
        const float *ratio = pw_modal_ratios((unsigned)model);
        float stretch = (m->p[5] - 0.5f) * 0.5f, pos = pw_clamp(m->p[6], 0.02f, 0.98f), bright = m->p[2];
        fill(ix, iy + ih - 1, iw, 1, C_LINE);
        for (int k = 0; k < (model ? 8 : 12); ++k) {
            float r = model ? ratio[k] * pw_exp2(stretch * pw_log2(ratio[k])) : (float)(k + 1);
            float a = model ? pw_absf(pw_sin1(0.5f * (float)(k + 1) * pos)) / (1.0f + 0.15f * (float)k) : 1.0f / (1.0f + (float)k * (1.2f - bright));
            int x = ix + (int)(pw_log2(r * 2.0f) / 6.0f * (float)iw), bh = clampi((int)(a * (float)(ih - 2)), 1, ih - 1);
            if (x > ix + iw) continue;
            fill(x - 1, iy + ih - 1 - bh, 3, bh, k ? c : C_WHITE);
        }
    } break;
    case PW_T_SNAPSHOT: {   /* the snapshots its CV reaches: the one playing filled, the one it asks for outlined */
        int n = clampi((int)(m->p[0] + 0.5f), 2, PW_VARS), per = n > 8 ? 8 : n, rows = n > 8 ? 2 : 1;
        int want = pv_demo ? 2 : (int)pw_live((unsigned)s, 0), have = pv_demo ? 1 : (int)pws.var_cur;
        int tw = iw / per, th = rows == 2 ? ih / 2 - 3 : clampi(ih - 8, 12, 40);
        int y0 = iy + (ih - rows * (th + 3)) / 2;
        for (int k = 0; k < n; ++k) {
            int tx = ix + (k % per) * tw, ty = y0 + (k / per) * (th + 3);
            char d[4]; s_int(d, k + 1);
            if (k == have) rfill(tx, ty, tw - 3, th, c);
            else frame_rect(tx, ty, tw - 3, th, k == want ? c : C_LINE);
            text_center(tx + (tw - 3) / 2, ty + th / 2 - 4, d, F_XS, k == have ? C_BG : k == want ? c : C_DIM);
        }
    } break;
    case PW_T_INERTIA: {   /* how the weight answers a step of the CV (a kick, in FLICK); the white dot is where it is now */
        float yv[64], T, lo = 0.0f, hi = 1.0f;
        int kick = (int)(m->p[4] + 0.5f) == 2, gx = ix + iw - 10;
        pw_inertia_curve(m->p, yv, 64, &T);
        for (int j = 0; j < 64; ++j) { if (yv[j] < lo) lo = yv[j]; if (yv[j] > hi) hi = yv[j]; }
        if (kick) { lo = lo < -1.0f ? lo : -1.0f; hi = hi > 1.0f ? hi : 1.0f; }
        int gh = ih - 14, gy = iy + 12, pw = gx - ix - 8;
        float span = hi - lo;
        int zy = gy + (int)((hi - (kick ? 0.0f : 1.0f)) / span * (float)gh);
        for (int xx = ix; xx < ix + pw; xx += 4) fill(xx, zy, 2, 1, C_LINE);   /* the target (the middle, in FLICK) */
        int px = ix, py = gy + (int)((hi - yv[0]) / span * (float)gh);
        for (int j = 1; j < 64; ++j) {
            int xx = ix + j * (pw - 1) / 63, yy = gy + (int)((hi - yv[j]) / span * (float)gh);
            line(px, py, xx, yy, 1, c); px = xx; py = yy;
        }
        { char b[16], *o = s_cpy(b, ""); o = s_int(o, (int)(T * 10.0f + 0.5f) / 10); *o++ = '.'; o = s_int(o, (int)(T * 10.0f + 0.5f) % 10); s_cpy(o, " SEC"); text(ix, iy, b, F_XS, C_DIM); }
        text_right(ix + pw, iy, m->p[4] < 0.5f ? "FOLLOW" : m->p[4] < 1.5f ? "PUSH" : "FLICK", F_XS, C_DIM);
        fill(gx, gy, 1, gh, C_LINE);
        { float pos = pv_demo ? 0.4f : pw_live((unsigned)s, 0); int dy = gy + gh / 2 - (int)(pw_clamp(pos, -10.0f, 10.0f) * (float)gh * 0.05f); fill(gx - 2, dy - 2, 5, 5, C_WHITE); }
    } break;
    case PW_T_BURST: {   /* where the hits of one burst fall (tall: the first, falling as LEVEL does); the label is how long it lasts */
        float tt[32], len; int n = pw_burst_hits(m->p, tt, &len);
        fill(ix, iy + ih - 1, iw, 1, C_LINE);
        for (int k = 0; k < n; ++k) {
            int bh = clampi((int)((float)(n - k) / (float)n * (float)(ih - 16)) + 3, 3, ih - 14), bx = ix + 2 + (int)(tt[k] * (float)(iw - 6));
            fill(bx, iy + ih - 1 - bh, 2, bh, k ? c : C_WHITE);
            if (m->p[4] > 0.5f || m->p[4] < -0.5f) { int py = iy + ih / 2 - (int)(pw_clamp(m->p[4] * (float)k / 36.0f, -1.0f, 1.0f) * (float)(ih / 2 - 8)); fill(bx - 1, py - 1, 4, 3, C_AMBER); }   /* (the pitch CV: a dot on each hit) */
        }
        { int ms = (int)(len * 1000.0f + 0.5f); char b[16], *o = s_int(b, ms >= 1000 ? ms / 1000 : ms); if (ms >= 1000) { *o++ = '.'; o = s_int(o, (ms % 1000) / 100); s_cpy(o, " SEC"); } else s_cpy(o, " MS"); text(ix, iy, b, F_XS, C_DIM); }
        text_right(ix + iw, iy, m->p[2] > 0.05f ? "SPEEDS UP" : m->p[2] < -0.05f ? "SLOWS DOWN" : "EVEN", F_XS, C_DIM);
    } break;
    case PW_T_MARKOV: {   /* the chances of the next note, from the note it is on (white): near ones tall, home higher */
        float w[36]; int cur = pv_demo ? 4 : (int)pw_live((unsigned)s, 0);
        int n = pw_markov_row(m, cur, w), bw = iw / (n > 0 ? n : 1);
        float mx = 0.001f;
        for (int j = 0; j < n; ++j) if (w[j] > mx) mx = w[j];
        for (int j = 0; j < n; ++j) {
            int bh = clampi((int)(w[j] / mx * (float)(ih - 4)), 1, ih - 4);
            fill(ix + j * bw, iy + ih - 3 - bh, bw > 2 ? bw - 1 : 1, bh, c);
        }
        if (cur >= 0 && cur < n) fill(ix + (cur % (n > 0 ? n : 1)) * bw, iy + ih - 2, bw > 2 ? bw - 1 : 1, 2, C_WHITE);
    } break;
    default: {   /* the output, as it is now (the preview: a made-up wave) */
        fill(ix, mid, iw, 1, C_LINE);
        const unsigned mask = PW_SCOPE - 1u;
        unsigned start = 0;
        float peak = 0.5f;
        if (!pv_demo) {
            if (pws.scope_mod != (uint8_t)s || pws.scope_port != 0) pw_set_scope((unsigned)s, 0);
            unsigned wr = pws.scope_w; start = (wr - 560u - 1u) & mask;
            for (unsigned back = 562u; back < PW_SCOPE - 2u; ++back) {
                unsigned i = (wr - back) & mask;
                if (pws.scope[i] <= 0 && pws.scope[(i + 1u) & mask] > 0) { start = i; break; }
            }
            for (unsigned k = 0; k < 560u; k += 4u) { float a = pw_absf((float)pws.scope[(start + k) & mask] * (1.0f / 3000.0f)); if (a > peak) peak = a; }
        }
        int px = ix, py = mid;
        for (int k = 0; k < iw; ++k) {
            float v;
            if (pv_demo) { float ph = (float)k / (float)iw * 3.0f; v = 0.55f * pw_sin1(ph) + 0.25f * pw_sin1(ph * 3.0f + 0.2f) + 0.12f * pw_sin1(ph * 5.0f); }
            else v = (float)pws.scope[(start + (unsigned)k * 560u / (unsigned)iw) & mask] * (1.0f / 3000.0f) / peak;
            int yy = clampi(mid - (int)(v * (float)(ih / 2 - 1)), iy, iy + ih);
            if (k) line(px, py, ix + k, yy, 1, c);
            px = ix + k; py = yy;
        }
    } break;
    }
}

/* COMPARE: A against the line it has to get past (B + THRESH, with the HYST band round it; WINDOW: B +- THRESH), and the gate it makes underneath */
static void vis_compare(int s)
{
    const pw_module *m = &pws.mod[s];
    static const char *const mode_n[4] = {"A>B", "A<B", "WINDOW", "CROSS"};
    uint16_t ca = cat_color[pw_types[m->type].cat];
    int mode = (int)(m->p[2] + 0.5f), b_on = pw_input_cables((unsigned)s, 1) > 0 || pv_demo;
    vis_src src[3] = {{1, 0}, {1, 1}, {0, 0}};
    vis_panel();
    vis_sample(s, src, 3);
    int top = VY + 20, bot = VY + VH - 24, h = bot - top;
    float th = m->p[0], hy = m->p[1] * 0.5f, lo = 1e9f, hi = -1e9f;
    for (int k = 0; k < vh.n; ++k) for (int c = 0; c < 2; ++c) { float v = vis_at(c, k); if (v < lo) lo = v; if (v > hi) hi = v; }
    if (vh.n == 0) { lo = -1; hi = 1; }
    if (th < lo) lo = th; if (th > hi) hi = th;
    if (hi - lo < 2.0f) { float mid = (hi + lo) * 0.5f; lo = mid - 1.0f; hi = mid + 1.0f; }
    float pad = (hi - lo) * 0.1f; lo -= pad; hi += pad;
    #define CY(v) clampi(bot - (int)(((v) - lo) / (hi - lo) * (float)h), top, bot)
    for (int g = 0; g <= 4; ++g) fill(6, top + g * h / 4, W - 12, 1, C_PANEL);
    if (lo < 0 && hi > 0) fill(6, CY(0.0f), W - 12, 1, C_LINE);
    int gy = VY + VH - 18;
    fill(6, gy, W - 12, 12, C_PANEL);
    int pxa = 0, pya = 0, pxb[2] = {0, 0}, pyb[2] = {0, 0};
    for (int k = 0; k < vh.n; ++k) {
        int xx = 8 + k * (W - 16) / (VIS_N - 1);
        float a = vis_at(0, k), b = b_on ? vis_at(1, k) : 0.0f;
        int ya = CY(a);
        if (k) line(pxa, pya, xx, ya, 1, ca);
        pxa = xx; pya = ya;
        for (int l = 0; l < (mode == 2 ? 2 : 1); ++l) {   /* the line A has to pass: B + THRESH (WINDOW: B + and - THRESH) */
            float v = mode == 2 ? (l ? b - pw_absf(th) : b + pw_absf(th)) : b + (mode == 1 ? -th : th);
            int yb = CY(v);
            if (k) line(pxb[l], pyb[l], xx, yb, 1, C_AMBER);
            pxb[l] = xx; pyb[l] = yb;
            if (hy > 0.04f && mode != 2 && !(k & 1)) { fill(xx, CY(v + hy), 1, 1, C_DIM); fill(xx, CY(v - hy), 1, 1, C_DIM); }
        }
        if (vis_at(2, k) > 2.5f) fill(xx, gy + 1, (W - 16) / (VIS_N - 1) + 1, 10, ca);
    }
    #undef CY
    fill(8, VY + 5, 6, 6, ca); int x = text(17, VY + 4, "A", F_XS, C_TEXT) + 8;
    fill(x, VY + 5, 6, 6, C_AMBER); x = text(x + 9, VY + 4, !b_on ? "THRESH" : mode == 2 ? "B +- T" : "B + T", F_XS, C_TEXT) + 8;
    text_right(W - 8, VY + 4, mode_n[mode & 3], F_XS, C_DIM);
    text(8, gy + 12, "GATE", F_XS, C_DIM);
}
/* SHIFT REG: the register as boxes, the newest on the left: each holds a value (a bar up or down from the middle); the ones past LENGTH are off.
 * Under them, where NOW, -1, -2, -3 and END are tapped, and the way FEEDBK carries the end back to the start */
static void vis_shift(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    int len = clampi((int)(m->p[0] + 0.5f), 2, 16), cw = (W - 16) / 16;
    vis_panel();
    int top = VY + 22, bot = VY + VH - 30, h = bot - top, mid = top + h / 2, x0 = 8;
    fill(x0, mid, cw * 16, 1, C_LINE);
    float pk = 1.0f;   /* the bars are scaled to the biggest value held (1 V at least: a sequencer's pitches are small) */
    for (int i = 0; i < len; ++i) { float a = pw_absf(pw_live((unsigned)s, (unsigned)i)); if (a > pk) pk = a; }
    for (int i = 0; i < 16; ++i) {
        int x = x0 + i * cw, on = i < len;
        float v = on ? pw_clamp(pw_live((unsigned)s, (unsigned)i) / pk, -1.0f, 1.0f) : 0.0f;
        rfill(x + 1, top, cw - 2, h, on ? RGB(10, 11, 15) : C_PANEL);
        int bh = (int)(v * (float)(h / 2 - 1));
        if (on) { if (bh >= 0) fill(x + 2, mid - bh, cw - 4, bh + 1, i == 0 ? C_AMBER : i == len - 1 ? C_GREEN : c); else fill(x + 2, mid, cw - 4, -bh, i == 0 ? C_AMBER : i == len - 1 ? C_GREEN : c); }
        const char *tag = i == 0 ? "N" : i == len - 1 ? "E" : i < 4 ? (i == 1 ? "1" : i == 2 ? "2" : "3") : 0;
        if (on && tag) text_center(x + cw / 2, bot + 3, tag, F_XS, i == 0 ? C_AMBER : i == len - 1 ? C_GREEN : C_TEXT);
    }
    float fb = m->p[1];
    if (fb > 0.02f) {   /* the end carried back to the start */
        int xe = x0 + (len - 1) * cw + cw / 2, xs = x0 + cw / 2, y = bot + 20;
        uint16_t lc = fb > 0.5f ? C_AMBER : C_DIM;
        line(xe, bot + 15, xe, y, 1, lc); line(xe, y, xs, y, 1, lc); line(xs, y, xs, bot + 15, 1, lc);
        line(xs - 2, bot + 18, xs, bot + 15, 1, lc); line(xs + 2, bot + 18, xs, bot + 15, 1, lc);
    }
    text(8, VY + 4, "NOW  -1  -2  -3  END", F_XS, C_DIM);
    char v[16];
    pw_format_param((unsigned)s, 2, v);
    if (m->p[2] > 0.02f) { char r[24], *o = s_cpy(r, "MUTATE "); s_cpy(o, v); text_right(W - 8, VY + 4, r, F_XS, C_DIM); }
}

/* WAVESET: the waveform it makes, cut where the rising zero crossings are: a tick for each cycle, the cycles shaded in groups of GROUP, so you
 * can see what the mode did to them (the same cycle twice, a cycle backwards, a gap, half-length cycles...) */
static void vis_waveset(int s)
{
    const pw_module *m = &pws.mod[s];
    if (pv_demo) { vis_wave(s, -1); return; }
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    int top = VY + 22, bot = VY + VH - 8, h = bot - top, mid = top + h / 2;
    if (pws.scope_mod != (uint8_t)s || pws.scope_port != 0) pw_set_scope((unsigned)s, 0);
    const unsigned span = 560u, mask = PW_SCOPE - 1u;
    unsigned w = pws.scope_w, start = (w - span - 1u) & mask;
    for (unsigned back = span + 2u; back < PW_SCOPE - 2u; ++back) {
        unsigned i = (w - back) & mask;
        if (pws.scope[i] <= 0 && pws.scope[(i + 1u) & mask] > 0) { start = i; break; }
    }
    float peak = 0;
    for (unsigned k = 0; k < span; k += 4u) { float a = pw_absf((float)pws.scope[(start + k) & mask] * (1.0f / 3000.0f)); if (a > peak) peak = a; }
    float sc = peak < 0.5f ? 0.5f : peak;
    int group = clampi((int)(m->p[1] + 0.5f), 1, 8), cyc = 0, gx = 6;
    fill(6, mid, W - 12, 1, C_LINE);
    int px = 0, py = mid, pos = 0;
    for (int k = 0; k < 116; ++k) {
        unsigned i = (start + (unsigned)k * 5u) & mask;
        int v = pws.scope[i], xx = 6 + k * 2;
        int yy = clampi(mid - (int)((float)v * (1.0f / 3000.0f) / sc * (float)(h / 2 - 2)), top, bot);
        if (!pos && v > 60) {   /* a rising crossing: a cycle ends here */
            pos = 1;
            if (k) {
                fill(xx, top - 2, 1, 4, C_AMBER);
                if (++cyc >= group) { cyc = 0; if ((xx - gx) > 2) { fill(gx, VY + 20, xx - gx, 1, ((gx / 2) & 1) ? c : C_DIM); } gx = xx; }
            }
        } else if (v < -60) pos = 0;
        if (k) line(px, py, xx, yy, 1, c);
        px = xx; py = yy;
    }
    char r[24];
    pw_format_param((unsigned)s, 0, r);
    text_fit(8, VY + 4, r, F_S, c, 110);
    char g[16], *o = s_cpy(g, "GROUP "); s_int(o, group);
    text_right(W - 8, VY + 5, peak < 0.002f ? "NO SIGNAL" : g, F_XS, C_DIM);
}

/* MUTATE: a tile for every module it can change, in the color of its category: bright with a white edge when the latest mutation changed it,
 * fading over the next few; a locked one is an outline with an L. Under them the AMOUNT, the DENSITY (how many of ten dots), and what came last */
static void vis_mutate(int s)
{
    const pw_module *m = &pws.mod[s];
    uint16_t c = cat_color[pw_types[m->type].cat];
    vis_panel();
    char buf[24];
    float amt = m->p[0], den = m->p[1];
    int nc = (int)pw_live((unsigned)s, 0), rc = (int)pw_live((unsigned)s, 1);
    {   char *o = s_cpy(buf, "NUDGE "); o = s_int(o, nc); o = s_cpy(o, "  ROLL "); s_int(o, rc); }
    text(8, VY + 4, buf, F_XS, C_DIM);
    int sl[PW_MAX_MODULES], n = 0;
    if (pv_demo) { for (int i = 0; i < 12; ++i) sl[n++] = i; }
    else for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pw_mut_eligible(i)) sl[n++] = (int)i;
    const int ts = 22, gap = 4, per = (W - 16 + gap) / (ts + gap), top = VY + 18;
    const int rows = (VY + VH - 36 - top + gap) / (ts + gap);
    for (int i = 0; i < n && i < per * rows; ++i) {
        int x = 8 + (i % per) * (ts + gap), y = top + (i / per) * (ts + gap);
        unsigned type = pv_demo ? (unsigned)(i * 7 + 3) % PW_T_COUNT : pws.mod[sl[i]].type;
        uint16_t tc = cat_color[pw_types[type].cat];
        int age = pv_demo ? (i < 6 ? i : 99) : pw_mut_age((unsigned)sl[i]);
        int locked = !pv_demo && sl[i] < 64 && (pws.mut_lock >> sl[i] & 1u);
        char tg[4]; { const char *tag = pw_types[type].tag; int k = 0; while (k < 3 && tag[k]) { tg[k] = tag[k]; ++k; } tg[k] = 0; }
        if (locked) {
            rfill(x, y, ts, ts, C_PANEL);
            frame_rect(x, y, ts, ts, C_DIM);
            text_center(x + ts / 2, y + 3, tg, F_XS, C_DIM);
            text_center(x + ts / 2, y + 11, "LOCK", F_XS, C_DIM);
        } else {
            unsigned a = age == 0 ? 15u : age == 1 ? 11u : age == 2 ? 8u : age < 6 ? 5u : 3u;
            rfill(x, y, ts, ts, blend(RGB(10, 11, 15), tc, a));
            if (age == 0) frame_rect(x, y, ts, ts, C_WHITE);
            text_center(x + ts / 2, y + 8, tg, F_XS, age < 3 ? C_WHITE : C_DIM);
        }
    }
    if (n > per * rows) { char more[12], *o = s_cpy(more, "+"); o = s_int(o, n - per * rows); text_right(W - 8, VY + 4, more, F_XS, C_DIM); }
    else if (!n) text(8, top + 4, "NO MODULES TO MUTATE", F_XS, C_DIM);
    int by = VY + VH - 30;
    text(8, by, "AMOUNT", F_XS, C_DIM);
    rfill(62, by + 1, 60, 6, RGB(10, 11, 15));
    fill(62, by + 1, (int)(amt * 60.0f), 6, c);
    text(132, by, "DENSITY", F_XS, C_DIM);
    for (int d = 0; d < 10; ++d) fill(186 + d * 4, by + 2, 3, 4, (float)d < den * 10.0f - 0.01f ? c : RGB(10, 11, 15));
    int any = 0;
    for (int i = 0; i < n; ++i) if (!pv_demo && pw_mut_age((unsigned)sl[i]) == 0) any = 1;
    text(8, by + 14, any ? (pw_mut_last_roll() ? "LAST: ROLL, NEW VALUES" : "LAST: NUDGE, A LITTLE OFF") : "NOTHING MUTATED YET", F_XS, any ? C_AMBER : C_DIM);
}

static void vis_draw(int s, int kind)
{
    const pw_type *t = &pw_types[pws.mod[s].type];
    switch (kind) {
    case VIS_WAVE: vis_wave(s, -1); break;
    case VIS_STRIP: {
        vis_src src[4]; const char *nm[4]; int n = 0;
        for (unsigned o = 0; o < t->n_out && n < 3; ++o) { src[n].in = 0; src[n].port = (uint8_t)o; nm[n] = t->out[o]; ++n; }
        vis_strip(s, src, n, nm);
    } break;
    case VIS_SHAPER: vis_shaper(s); break;
    case VIS_COMP: vis_comp(s); break;
    case VIS_ECHO: vis_echo(s); break;
    case VIS_VERB: vis_verb(s); break;
    case VIS_SWEEP: vis_sweep(s); break;
    case VIS_VOWEL: vis_vowel(s); break;
    case VIS_CHAOS: vis_chaos(s); break;
    case VIS_MODSEQ: vis_modseq(s); break;
    case VIS_CHORDSTEPS: vis_chordsteps(s); break;
    case VIS_GRAVITY: vis_gravity(s); break;
    case VIS_TURING: vis_turing(s); break;
    case VIS_ATTEN: vis_atten(s); break;
    case VIS_COMPARE: vis_compare(s); break;
    case VIS_SHIFT: vis_shift(s); break;
    case VIS_WAVESET: vis_waveset(s); break;
    case VIS_MUTATE: vis_mutate(s); break;
    case VIS_NOISE: vis_noise(s); break;
    case VIS_CLOCK: vis_clock(s); break;
    case VIS_OUTPUT: vis_output(s); break;
    case VIS_TAPE: vis_tape(s); break;
    case VIS_FRACTURE: vis_fracture(s); break;
    case VIS_SWITCH: vis_switch(s); break;
    case VIS_QUANT: vis_quant(s); break;
    case VIS_CHORD: vis_chord(s); break;
    case VIS_KEYS: vis_keys(s); break;
    case VIS_MIDIIN: vis_midiin(s); break;
    case VIS_MIDIOUT: vis_midiout(s); break;
    case VIS_LIMITER: vis_limiter(s); break;
    case VIS_OTT: vis_ott(s); break;
    case VIS_ARP: vis_arp(s); break;
    case VIS_XFADE: vis_xfade(s); break;
    case VIS_SPACE: vis_space(s); break;
    default: break;
    }
}
