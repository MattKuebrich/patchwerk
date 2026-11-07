/* SPDX-License-Identifier: GPL-3.0-only
 * PATCHWERK patch engine: graph editing, render plan, pool memory, transport,
 * keyboard state and patch serialization.
 *
 * Threading: the control task edits the graph while the audio callback
 * renders. Edits build a new render plan in the idle slot of a double buffer
 * and publish it with one word store; the audio side acknowledges the plan it
 * picked up at the start of every block. Memory owned by a removed module is
 * released only after that acknowledgement, so most edits never interrupt the
 * audio. Only pool compaction and whole-patch loads stop the callback. */
#define PW_NVAR 1   /* in the core PW_N is the engine block (64 or 128 frames, see pw_set_rate) */
#include "pw_dsp.h"
#include <string.h>
int pw_sr = 44100, pw_blk = 128;
float pw_srf = 44100.0f, pw_isr = 1.0f / 44100.0f;
static volatile uint8_t rate_want;   /* the SAMPLE RATE the next block runs at (taken at its start: the audio interrupt owns the change) */
static uint8_t rate_half;
static float up_h[2][3];             /* the upsampler's last three input frames, per side */
void pw_set_rate(int half) { rate_want = half ? 1u : 0u; }
int pw_rate_half(void) { return rate_half; }


pw_engine pws;
volatile uint32_t pw_trace;
uint8_t pw_pool[PW_POOL_BYTES] PW_POOL;
static float outbuf[PW_MAX_OUTBUF][PW_NMAX] PW_POOL;
static uint8_t outbuf_used[PW_MAX_OUTBUF];
static float zero_buf[PW_NMAX];
static float sum_buf[PW_MAX_IN][PW_NMAX] PW_POOL;   /* (in the pool: the RAM is short) */
static float voice_acc[8][PW_NMAX] PW_POOL;     /* what leaves the voice area, summed over the voices */
static unsigned lock_depth;
extern float pw_mix_l[PW_NMAX], pw_mix_r[PW_NMAX];
void pw_modules_init(void);

static void lock(void) { if (lock_depth++ == 0) pw_plat_lock(); }
static void unlock(void) { if (lock_depth && --lock_depth == 0) pw_plat_unlock(); }

/* ------------------------------------------------------------ render plan */
static uint8_t in_voice_area(unsigned m) { return pws.voices > 1 && (pws.mod[m].flags & PW_F_POLY) && pws.mod[m].nv == pws.voices; }
PW_COLD static void build_plan(pw_plan *p)
{
    uint8_t indeg[PW_MAX_MODULES], done[PW_MAX_MODULES], base[PW_MAX_MODULES];
    memset(indeg, 0, sizeof indeg);
    memset(done, 0, sizeof done);
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (k->used && k->sm != k->dm) ++indeg[k->dm];
    }
    /* a PARAM MOD runs before the modules it moves a knob of (so it has this block's voltage, not last block's) */
    for (unsigned j = 0; j < PW_MAX_MODULES; ++j)
        for (unsigned c = 0; c < 4; ++c) { int t = pw_pm_target(j, c); if (t >= 0) ++indeg[t]; }
    unsigned n = 0;
    for (;;) {
        int progressed = 0;
        for (unsigned m = 0; m < PW_MAX_MODULES; ++m) {
            if (pws.mod[m].used != 1 || done[m] || indeg[m]) continue;
            done[m] = 1; base[n++] = (uint8_t)m; progressed = 1;
            for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
                const pw_cable *k = &pws.cable[c];
                if (k->used && k->sm == m && k->dm != m && indeg[k->dm]) --indeg[k->dm];
            }
            for (unsigned c = 0; c < 4; ++c) { int t = pw_pm_target(m, c); if (t >= 0 && indeg[t]) --indeg[t]; }
        }
        if (progressed) continue;
        /* A feedback loop: run the lowest remaining slot first (its inputs
         * from later modules arrive one block late, ~2.9 ms). */
        int pick = -1;
        for (unsigned m = 0; m < PW_MAX_MODULES && pick < 0; ++m)
            if (pws.mod[m].used == 1 && !done[m]) pick = (int)m;
        if (pick < 0) break;
        indeg[pick] = 0;
    }
    /* The voice area runs as one stretch: first what it does not hear (LFOs, clocks), then the voice area,
     * then what hears it (effects, the output). */
    uint8_t down[PW_MAX_MODULES];
    memset(down, 0, sizeof down);
    for (unsigned m = 0; m < PW_MAX_MODULES; ++m) if (pws.mod[m].used == 1 && in_voice_area(m)) down[m] = 2;
    for (int changed = 1; changed;) {
        changed = 0;
        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
            const pw_cable *k = &pws.cable[c];
            if (k->used && down[k->sm] && !down[k->dm]) { down[k->dm] = 1; changed = 1; }
        }
    }
    unsigned o = 0;
    for (unsigned i = 0; i < n; ++i) if (!down[base[i]]) p->order[o++] = base[i];
    p->poly_first = (uint8_t)o;
    for (unsigned i = 0; i < n; ++i) if (down[base[i]] == 2) p->order[o++] = base[i];
    p->poly_count = (uint8_t)(o - p->poly_first);
    for (unsigned i = 0; i < n; ++i) if (down[base[i]] == 1) p->order[o++] = base[i];
    p->count = (uint8_t)o;
    /* the outputs that leave the voice area: summed over the voices */
    p->n_bound = 0;
    for (unsigned c = 0; c < PW_MAX_CABLES && p->poly_count; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (!k->used || down[k->sm] != 2 || down[k->dm] == 2) continue;
        uint16_t b = pws.mod[k->sm].outbuf[k->so];
        unsigned j = 0;
        while (j < p->n_bound && p->bound[j] != b) ++j;
        if (j == p->n_bound && j < 8) p->bound[p->n_bound++] = b;
    }
    memset(p->omask, 0, sizeof p->omask);
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) if (pws.cable[c].used) p->omask[pws.cable[c].sm] |= (uint16_t)(1u << pws.cable[c].so);
    unsigned s = 0;
    for (unsigned m = 0; m < PW_MAX_MODULES; ++m)
        for (unsigned i = 0; i < PW_MAX_IN; ++i) {
            p->in_first[m][i] = (uint16_t)s;
            p->in_count[m][i] = 0;
            if (pws.mod[m].used != 1) continue;
            for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
                const pw_cable *k = &pws.cable[c];
                if (!k->used || k->dm != m || k->di != i) continue;
                p->gain[s] = k->amt;
                p->src[s++] = pws.mod[k->sm].outbuf[k->so];
                ++p->in_count[m][i];
            }
        }
}
static void wait_for_audio(uint32_t plan)
{
    if (lock_depth || !pw_plat_audio_running()) return;
    uint32_t start = pw_plat_millis();
    while (pws.plan_ack != plan && pw_plat_millis() - start < 30u) { }
}
static void publish(void)
{
    uint32_t cur = pws.plan_cur, next = cur ^ 1u;
    wait_for_audio(cur);                 /* audio no longer reads `next` */
    build_plan(&pws.plan[next]);
    pws.plan_cur = next;
    pws.revision++;
}

/* ------------------------------------------------------------------- pool
 * Each module one region: its audio memory once per voice, the other voices' states, then its variations
 * (PW_VARS x each parameter as 16 bits, and its steps when it has them). */
#define R16(x) (((x) + 15u) & ~15u)
static uint32_t var_bytes(const pw_type *t, unsigned type)
{
    return R16(PW_VARS * t->n_param * 2u + (pw_has_data(type) ? PW_VARS * pw_data_len(type) : 0u));
}
/* (the wavetable module's tables are read only: one copy for all its voices) */
#define AUDIO_COPIES(m) ((m)->type == PW_T_WAVETABLE ? 1u : (unsigned)(m)->nv)
static uint8_t *var_store(const pw_module *m) { return pw_pool + m->reg_off + m->audio_len * AUDIO_COPIES(m) + (m->nv - 1u) * PW_STATE_BYTES; }
static uint8_t *voice_state(const pw_module *m, unsigned v) { return pw_pool + m->reg_off + m->audio_len * AUDIO_COPIES(m) + (v - 1u) * PW_STATE_BYTES; }
/* room for `want` bytes for m (its own region not counted): the offset, or -1 */
static int64_t region_find(pw_module *m, uint32_t want)
{
    uint32_t used = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i)
        if (pws.mod[i].used && &pws.mod[i] != m) used += pws.mod[i].reg_len;
    if (PW_POOL_BYTES - used < want) return -1;
    uint32_t at = 0;
    for (;;) {
        int clash = 0;
        uint32_t next_end = 0;
        for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
            const pw_module *o = &pws.mod[i];
            if (!o->used || o == m || !o->reg_len) continue;
            if (o->reg_off < at + want && at < o->reg_off + o->reg_len) {
                clash = 1;
                if (o->reg_off + o->reg_len > next_end) next_end = o->reg_off + o->reg_len;
            }
        }
        if (!clash) break;
        at = next_end;
        if (at + want > PW_POOL_BYTES) break;
    }
    if (at + want <= PW_POOL_BYTES) return at;
    /* Fragmented: compact every other region toward zero (audio stopped). */
    lock();
    uint32_t cursor = 0;
    for (;;) {
        pw_module *lowest = 0;
        for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
            pw_module *o = &pws.mod[i];
            if (!o->used || o == m || !o->reg_len || o->reg_off < cursor) continue;
            if (!lowest || o->reg_off < lowest->reg_off) lowest = o;
        }
        if (!lowest) break;
        if (lowest->reg_off != cursor) memmove(pw_pool + cursor, pw_pool + lowest->reg_off, lowest->reg_len);
        lowest->mem_off = lowest->mem_off - lowest->reg_off + cursor;
        lowest->reg_off = cursor;
        cursor += lowest->reg_len;
    }
    unlock();
    return cursor + want <= PW_POOL_BYTES ? (int64_t)cursor : -1;
}
/* (re)make m's region for m->nv voices; its variations kept when keep. Audio stopped by the caller when the
 * module is live. */
static uint8_t var_keep[PW_VARS * PW_MAX_PARAM * 2 + PW_VARS * PW_DATA_MAX] PW_POOL;
static int module_alloc(pw_module *m, int keep, unsigned old_nv)
{
    const pw_type *t = &pw_types[m->type];
    uint32_t vb = var_bytes(t, m->type), vs = (m->nv - 1u) * PW_STATE_BYTES;
    if (keep && m->reg_len) {                  /* the variations, from where they were (old_nv voices before them) */
        uint8_t nv = m->nv; m->nv = (uint8_t)old_nv;
        memcpy(var_keep, var_store(m), vb);
        m->nv = nv;
    }
    uint32_t tries[3] = {R16(t->mem_ideal), R16(t->mem_min), 0};
    m->status = PW_STATUS_OK;
    for (int k = 0; k < 3; ++k) {
        uint32_t a = tries[k], want = a * AUDIO_COPIES(m) + vs + vb;
        if (k && a == tries[k - 1]) continue;
        int64_t at = region_find(m, want);
        if (at < 0) continue;
        m->reg_off = (uint32_t)at; m->reg_len = want; m->audio_len = a;
        m->mem_off = m->reg_off; m->mem_len = a;
        memset(pw_pool + at, 0, want);
        if (t->mem_ideal && !a) m->status = PW_STATUS_NOMEM;
        if (keep) memcpy(var_store(m), var_keep, vb);
        return 0;
    }
    m->reg_off = m->reg_len = m->audio_len = m->mem_off = m->mem_len = 0;
    m->status = PW_STATUS_NOMEM;
    m->nv = 1;
    return -1;
}

/* a clean song and no snapshot bypasses */
static void song_reset(void)
{
    memset(&pws.song, 0, sizeof pws.song);
    for (unsigned l = 0; l < PW_LANES; ++l) pws.song.al[l].mod = 0xff;
    pws.song.on = 0;   /* off until a clip is added */
    pws.byp_has = 0; memset(pws.byp, 0, sizeof pws.byp);
    pws.mut_lock = 0;
}
/* each snapshot keeps which modules are bypassed: left, the current one's are written down; arriving, they are set */
static uint64_t byp_now(void)
{
    uint64_t b = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1 && (pws.mod[i].flags & PW_F_BYPASS)) b |= (uint64_t)1 << i;
    return b;
}
static void byp_apply(unsigned v)
{
    if (!(pws.byp_has >> v & 1u)) return;
    int ch = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        pw_module *m = &pws.mod[i];
        if (m->used != 1) continue;
        unsigned want = pws.byp[v] >> i & 1u ? PW_F_BYPASS : 0u;
        if ((m->flags & PW_F_BYPASS) != want) { m->flags = (uint8_t)((m->flags & ~PW_F_BYPASS) | want); ch = 1; }
    }
    if (ch) publish();
}
/* ------------------------------------------------------------ the song's automation lanes */
unsigned pw_lane_start(unsigned l)
{
    unsigned at = 0;
    for (unsigned k = 0; k < l && k < pws.song.n_lanes; ++k) at += pws.song.al[k].n;
    return at;
}
static unsigned lane_total(void) { return pw_lane_start(pws.song.n_lanes); }
int pw_lane_find(unsigned mod, unsigned par)
{
    for (unsigned l = 0; l < pws.song.n_lanes; ++l) if (pws.song.al[l].mod == mod && pws.song.al[l].par == par) return (int)l;
    return -1;
}
int pw_lane_add(unsigned mod, unsigned par)
{
    if (pws.song.n_lanes >= PW_LANES || mod >= PW_MAX_MODULES) return -1;
    pw_alane *a = &pws.song.al[pws.song.n_lanes];
    a->mod = (uint8_t)mod; a->par = (uint8_t)par; a->n = 0; a->spare = 0;
    pws.revision++;
    return (int)pws.song.n_lanes++;
}
void pw_lane_del(unsigned l)
{
    if (l >= pws.song.n_lanes) return;
    unsigned s = pw_lane_start(l), n = pws.song.al[l].n, tot = lane_total();
    memmove(pws.song.pt_t + s, pws.song.pt_t + s + n, (tot - s - n) * sizeof pws.song.pt_t[0]);
    memmove(pws.song.pt_v + s, pws.song.pt_v + s + n, (tot - s - n) * sizeof pws.song.pt_v[0]);
    for (unsigned k = l; k + 1 < pws.song.n_lanes; ++k) pws.song.al[k] = pws.song.al[k + 1];
    --pws.song.n_lanes;
    pws.song.al[pws.song.n_lanes].mod = 0xff;
    pws.revision++;
}
int pw_lane_set(unsigned l, unsigned t, unsigned v, unsigned shape)
{
    if (l >= pws.song.n_lanes) return -1;
    pw_alane *a = &pws.song.al[l];
    unsigned s = pw_lane_start(l), i = 0;
    if (t > 4095) t = 4095;
    if (shape >= PW_SHAPES) shape = 0;
    while (i < a->n && (pws.song.pt_t[s + i] & 0xfffu) < t) ++i;
    if (i < a->n && (pws.song.pt_t[s + i] & 0xfffu) == t) { pws.song.pt_t[s + i] = (uint16_t)(t | shape << 12); pws.song.pt_v[s + i] = (uint8_t)(v > 255 ? 255 : v); pws.revision++; return (int)(s + i); }
    unsigned tot = lane_total();
    if (tot >= PW_LANE_PTS) return -1;
    memmove(pws.song.pt_t + s + i + 1, pws.song.pt_t + s + i, (tot - s - i) * sizeof pws.song.pt_t[0]);
    memmove(pws.song.pt_v + s + i + 1, pws.song.pt_v + s + i, (tot - s - i) * sizeof pws.song.pt_v[0]);
    pws.song.pt_t[s + i] = (uint16_t)(t | shape << 12); pws.song.pt_v[s + i] = (uint8_t)(v > 255 ? 255 : v);
    ++a->n;
    pws.revision++;
    return (int)(s + i);
}
void pw_lane_del_pt(unsigned l, unsigned i)
{
    if (l >= pws.song.n_lanes || i >= pws.song.al[l].n) return;
    unsigned s = pw_lane_start(l) + i, tot = lane_total();
    memmove(pws.song.pt_t + s, pws.song.pt_t + s + 1, (tot - s - 1) * sizeof pws.song.pt_t[0]);
    memmove(pws.song.pt_v + s, pws.song.pt_v + s + 1, (tot - s - 1) * sizeof pws.song.pt_v[0]);
    --pws.song.al[l].n;
    pws.revision++;
}
PW_COLD int pw_lane_move_pt(unsigned l, unsigned i, unsigned t)
{
    if (l >= pws.song.n_lanes || i >= pws.song.al[l].n) return -1;
    unsigned s = pw_lane_start(l);
    unsigned v = pws.song.pt_v[s + i], sh = pws.song.pt_t[s + i] >> 12;
    pw_lane_del_pt(l, i);
    int r = pw_lane_set(l, t, v, sh);
    return r < 0 ? r : r - (int)s;
}
float pw_lane_eval(unsigned l, float pos)
{
    if (l >= pws.song.n_lanes || !pws.song.al[l].n) return 0.0f;
    unsigned s = pw_lane_start(l), n = pws.song.al[l].n;
    const uint16_t *pt = pws.song.pt_t + s; const uint8_t *pv = pws.song.pt_v + s;
    if (pos <= (float)(pt[0] & 0xfffu)) return (float)pv[0];
    for (unsigned i = 0; i + 1 < n; ++i) {
        float t0 = (float)(pt[i] & 0xfffu), t1 = (float)(pt[i + 1] & 0xfffu);
        if (pos >= t1) continue;
        unsigned sh = pt[i] >> 12;
        float u = t1 > t0 ? (pos - t0) / (t1 - t0) : 1.0f;
        if (sh == 1) u = 0.0f;
        else if (sh == 2) u = u * u * (3.0f - 2.0f * u);
        return (float)pv[i] + ((float)pv[i + 1] - (float)pv[i]) * u;
    }
    return (float)pv[n - 1];
}
float pw_lane_norm(unsigned mod, unsigned par, float v)
{
    if (mod >= PW_MAX_MODULES || !pws.mod[mod].used) return 0.0f;
    const pw_type *ty = &pw_types[pws.mod[mod].type];
    if (par >= ty->n_param) return 0.0f;
    const pw_param_desc *d = &ty->param[par];
    if (d->kind == PW_EXP) { float lo = d->min > 0 ? d->min : 0.001f; return v > lo ? pw_clamp(pw_log2(v / lo) / pw_log2(d->max / lo), 0.0f, 1.0f) : 0.0f; }
    return d->max > d->min ? pw_clamp((v - d->min) / (d->max - d->min), 0.0f, 1.0f) : 0.0f;
}
static float norm_to_value(const pw_param_desc *d, float n)   /* 0-1 of a knob's range as its value */
{
    if (d->kind == PW_ENUM || d->kind == PW_INT) return (float)pw_floori(d->min + (d->max - d->min) * n + 0.5f);
    if (d->kind == PW_EXP) { float lo = d->min > 0 ? d->min : 0.001f; return lo * pw_exp2(n * pw_log2(d->max / lo)); }
    return d->min + (d->max - d->min) * n;
}
/* ---- a knob that follows a MOD SEQ lane ("parameter lock"): REC + a knob lays the knob's value on the lane's steps. A lane's
 * knob is kept in the MOD SEQ's spare data bytes (data[48 + 2 lane]: the module + 1, 0 none; data[49 + 2 lane]: the knob),
 * so it is saved with the patch. The knob takes the step's value as each step plays (pw_mseq_follow, from the screen's loop). */
#define LK(L) (48u + 2u * (L))
int pw_mseq_lock_of(const pw_module *m, unsigned lane, unsigned *mod, unsigned *par)
{
    if (m->type != PW_T_MODSEQ || lane > 3 || !m->data[LK(lane)]) return 0;
    *mod = (unsigned)m->data[LK(lane)] - 1u; *par = m->data[LK(lane) + 1u];
    return 1;
}
int pw_mseq_lock_find(unsigned mod, unsigned par, int *mseq, unsigned *lane)
{
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        if (pws.mod[i].used != 1 || pws.mod[i].type != PW_T_MODSEQ) continue;
        for (unsigned l = 0; l < 4; ++l) {
            unsigned a, b;
            if (pw_mseq_lock_of(&pws.mod[i], l, &a, &b) && a == mod && b == par) { *mseq = (int)i; *lane = l; return 1; }
        }
    }
    return 0;
}
void pw_mseq_lock_clear(unsigned mseq, unsigned lane)
{
    if (mseq >= PW_MAX_MODULES || pws.mod[mseq].used != 1 || pws.mod[mseq].type != PW_T_MODSEQ || lane > 3) return;
    pws.mod[mseq].data[LK(lane)] = pws.mod[mseq].data[LK(lane) + 1u] = 0;
    pws.revision++;
}
/* the lane a knob follows: the one it has, or a free lane (no knob, no cable from it) of a MOD SEQ (one is added when the patch has none),
 * all 16 steps set to the knob's value now. 1: it had one, 0: made, -1: no room for a MOD SEQ, -2: every lane is in use */
PW_COLD int pw_mseq_lock_assign(unsigned mod, unsigned par, int *mseq, unsigned *lane)
{
    if (mod >= PW_MAX_MODULES || pws.mod[mod].used != 1 || par >= pw_types[pws.mod[mod].type].n_param) return -1;
    if (pw_mseq_lock_find(mod, par, mseq, lane)) return 1;
    int pick = -1; unsigned pl = 0, seen = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES && pick < 0; ++i) {
        if (pws.mod[i].used != 1 || pws.mod[i].type != PW_T_MODSEQ) continue;
        ++seen;
        for (unsigned l = 0; l < 4 && pick < 0; ++l) {
            int cabled = 0;
            for (unsigned c = 0; c < PW_MAX_CABLES; ++c) if (pws.cable[c].used && pws.cable[c].sm == i && pws.cable[c].so == l) cabled = 1;
            if (!pws.mod[i].data[LK(l)] && !cabled) { pick = (int)i; pl = l; }
        }
    }
    if (pick < 0) {
        if (seen) return -2;
        pick = pw_add(PW_T_MODSEQ, pws.mod[mod].x, pws.mod[mod].y + 270);
        if (pick < 0) return -1;
        pl = 0;
    }
    unsigned q = (unsigned)(pw_lane_norm(mod, par, pws.mod[mod].p[par]) * 255.0f + 0.5f);
    for (unsigned k = 0; k < 16; ++k) pw_mseq_set(&pws.mod[pick], pl, k, (int)q);
    pws.mod[pick].data[LK(pl)] = (uint8_t)(mod + 1u); pws.mod[pick].data[LK(pl) + 1u] = (uint8_t)par;
    pws.revision++;
    *mseq = pick; *lane = pl;
    return 0;
}
void pw_mseq_lock_write(unsigned mseq, unsigned lane, int step, int all)   /* the knob's value now, on a step (or all 16) */
{
    unsigned mod, par;
    if (mseq >= PW_MAX_MODULES || pws.mod[mseq].used != 1 || !pw_mseq_lock_of(&pws.mod[mseq], lane, &mod, &par)) return;
    int q = (int)(pw_lane_norm(mod, par, pws.mod[mod].p[par]) * 255.0f + 0.5f);
    if (all) for (unsigned k = 0; k < 16; ++k) pw_mseq_set(&pws.mod[mseq], lane, k, q);
    else if (step >= 0) pw_mseq_set(&pws.mod[mseq], lane, (unsigned)step & 15u, q);
    pws.revision++;
}
void pw_mseq_follow(int skip_mod, int skip_par)
{
    static int8_t last[PW_MAX_MODULES][4];   /* the step each lane played last, + 1 */
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        const pw_module *m = &pws.mod[i];
        if (m->used != 1 || m->type != PW_T_MODSEQ) continue;
        for (unsigned l = 0; l < 4; ++l) {
            unsigned a, b;
            if (!pw_mseq_lock_of(m, l, &a, &b)) continue;
            int pos = (int)pw_live(i, l);
            if (pos < 0 || pos + 1 == last[i][l]) continue;
            last[i][l] = (int8_t)(pos + 1);
            if ((int)a == skip_mod && (int)b == skip_par) continue;
            if (a >= PW_MAX_MODULES || pws.mod[a].used != 1 || b >= pw_types[pws.mod[a].type].n_param) continue;
            float v = norm_to_value(&pw_types[pws.mod[a].type].param[b], (float)pw_mseq_get(m, l, (unsigned)pos) * (1.0f / 255.0f));
            if (v != pws.mod[a].p[b]) { pws.mod[a].p[b] = v; pws.revision++; }
        }
    }
}
/* SNAPSHOT: its CV picks the patch's snapshot (the module decides; this recalls it, on the beat or bar if CHANGE says so, gliding over MORPH; SONG playing wins) */
void pw_snap_follow(uint32_t now)
{
    static int seen = -1, want = -1, mto = -1, mark;
    static unsigned mfrom;
    static uint32_t last_ms, mt0;
    int sl = -1;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1 && pws.mod[i].type == PW_T_SNAPSHOT) { sl = (int)i; break; }
    if (sl < 0 || (pws.song.on && pws.playing)) { seen = want = mto = -1; return; }
    const pw_module *m = &pws.mod[sl];
    int req = (int)pw_live((unsigned)sl, 0), mode = (int)(m->p[3] + 0.5f);
    unsigned div = mode == 1 ? 4u : mode == 2 ? 16u : 1u;
    float mt = m->p[4] * 1000.0f;
    if (req >= 0 && req != seen) { seen = req; want = req; mark = (int)(pws.sixteenths / div); }
    if (mto >= 0) {
        float t = (float)(now - mt0) / mt;
        if (t >= 1.0f) { pw_var_select((unsigned)mto); mto = -1; last_ms = now; }
        else pw_var_morph(mfrom, (unsigned)mto, t);
        return;
    }
    if (want < 0 || now - last_ms < 40u) return;
    if (div > 1u && pws.playing && (int)(pws.sixteenths / div) == mark) return;
    int to = want; want = -1;
    if (to >= PW_VARS || (unsigned)to == pws.var_cur) return;
    last_ms = now;
    if (mt < 20.0f) pw_var_select((unsigned)to);
    else { mto = to; mfrom = pws.var_cur; mt0 = now; }
}
uint8_t pw_lane_skip;
void pw_lane_apply(float pos)
{
    for (unsigned l = 0; l < pws.song.n_lanes; ++l) {
        if (pw_lane_skip & (1u << l)) continue;
        const pw_alane *a = &pws.song.al[l];
        if (!a->n || a->mod >= PW_MAX_MODULES || pws.mod[a->mod].used != 1) continue;
        const pw_type *ty = &pw_types[pws.mod[a->mod].type];
        if (a->par >= ty->n_param) continue;
        const pw_param_desc *d = &ty->param[a->par];
        float v = norm_to_value(d, pw_lane_eval(l, pos) * (1.0f / 255.0f));
        if (v != pws.mod[a->mod].p[a->par]) { pws.mod[a->mod].p[a->par] = v; pws.revision++; }
    }
}

/* ------------------------------------------------------------ variations */
static uint16_t quant(const pw_param_desc *d, float v)
{
    float u = d->max > d->min ? (v - d->min) / (d->max - d->min) : 0.0f;
    return (uint16_t)(pw_clamp(u, 0.0f, 1.0f) * 65535.0f + 0.5f);
}
static float dequant(const pw_param_desc *d, uint16_t q) { return d->min + (d->max - d->min) * ((float)q / 65535.0f); }
PW_COLD static void var_put(pw_module *m, unsigned v)
{
    const pw_type *t = &pw_types[m->type];
    if (!m->reg_len) return;
    uint8_t *vs = var_store(m);
    for (unsigned p = 0; p < t->n_param; ++p) {
        uint16_t q = quant(&t->param[p], m->p[p]);
        vs[(v * t->n_param + p) * 2] = (uint8_t)q; vs[(v * t->n_param + p) * 2 + 1] = (uint8_t)(q >> 8);
    }
    if (pw_has_data(m->type)) memcpy(vs + PW_VARS * t->n_param * 2u + v * pw_data_len(m->type), m->data, pw_data_len(m->type));
}
static float var_get(const pw_module *m, unsigned v, unsigned p)
{
    const pw_type *t = &pw_types[m->type];
    const uint8_t *vs = var_store(m);
    return dequant(&t->param[p], (uint16_t)(vs[(v * t->n_param + p) * 2] | (vs[(v * t->n_param + p) * 2 + 1] << 8)));
}
static void var_take(pw_module *m, unsigned v)
{
    const pw_type *t = &pw_types[m->type];
    if (!m->reg_len || m->type == PW_T_SNAPSHOT) return;   /* (SNAPSHOT's own knobs are not part of any snapshot) */
    for (unsigned p = 0; p < t->n_param; ++p) {
        float x = var_get(m, v, p);
        /* (a value equal after rounding stays exact: no drift from saving and loading) */
        if (quant(&t->param[p], m->p[p]) != quant(&t->param[p], x)) m->p[p] = x;
        if (t->param[p].kind == PW_ENUM || t->param[p].kind == PW_INT) m->p[p] = (float)pw_floori(m->p[p] + 0.5f);
    }
    if (pw_has_data(m->type)) memcpy(m->data, var_store(m) + PW_VARS * t->n_param * 2u + v * pw_data_len(m->type), pw_data_len(m->type));
}

/* --------------------------------------------------------------- undo
 * A log of edits, each with what undoes it; undoing one logs what redoes it. Edits in one step share a group. */
#ifdef PW_WEB
#define UNDO_BYTES 16384u
#else
#define UNDO_BYTES 1536u
#endif
enum { U_ADD = 1, U_REMOVE, U_CONN, U_DISC, U_PARAM, U_DATA, U_MOVE, U_FLAGS, U_VOICES, U_VAR, U_PERF, U_CAMT, U_GROUP };
typedef struct { uint8_t b[UNDO_BYTES]; unsigned n; } ulog;
static ulog undo_log PW_POOL, redo_log PW_POOL;
static uint8_t rec_on = 1, rec_mode;              /* rec_mode 0 an edit, 1 undoing (logs redo), 2 redoing */
static uint16_t group_id, group_open;
static uint32_t last_ms; static uint8_t last_kind, last_slot, last_p;
static ulog *rec_log(void) { return rec_mode == 1 ? &redo_log : &undo_log; }
/* a record: kind, length, group (2), payload */
static void rec_push(unsigned kind, const uint8_t *pay, unsigned n)
{
    if (!rec_on || n + 4u > 255u) return;
    ulog *L = rec_log();
    if (rec_mode == 0) redo_log.n = 0;
    if (!group_open && rec_mode == 0) ++group_id;
    while (L->n + n + 4u > UNDO_BYTES && L->n) {            /* full: the oldest step goes */
        unsigned first = L->b[1], g = L->b[2] | (L->b[3] << 8), cut = 0;
        while (cut < L->n && (unsigned)(L->b[cut + 2] | (L->b[cut + 3] << 8)) == g) cut += L->b[cut + 1];
        (void)first;
        memmove(L->b, L->b + cut, L->n - cut); L->n -= cut;
    }
    uint8_t *r = L->b + L->n;
    r[0] = (uint8_t)kind; r[1] = (uint8_t)(n + 4u); r[2] = (uint8_t)group_id; r[3] = (uint8_t)(group_id >> 8);
    memcpy(r + 4, pay, n);
    L->n += n + 4u;
}
/* the last record of the log (0: none) */
static uint8_t *rec_last(ulog *L)
{
    unsigned at = 0, last = 0;
    if (!L->n) return 0;
    while (at < L->n) { last = at; at += L->b[at + 1]; }
    return L->b + last;
}
/* coalesce: the same knob turned again soon after adds no step */
static int rec_same(unsigned kind, unsigned slot, unsigned p, uint32_t ms)
{
    uint32_t now = pw_plat_millis();
    int same = rec_on && rec_mode == 0 && !group_open && last_kind == kind && last_slot == slot && last_p == p &&
               now - last_ms < ms && rec_last(&undo_log);
    last_kind = (uint8_t)kind; last_slot = (uint8_t)slot; last_p = (uint8_t)p; last_ms = now;
    return same;
}
static void putf_(uint8_t *b, float f) { union { float f; uint32_t u; } x = { f }; for (int i = 0; i < 4; ++i) b[i] = (uint8_t)(x.u >> (8 * i)); }
static float getf_(const uint8_t *b) { union { float f; uint32_t u; } x; x.u = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); return x.f; }
static void rec_remove(unsigned slot)
{
    if (!rec_on) return;
    const pw_module *m = &pws.mod[slot];
    const pw_type *t = &pw_types[m->type];
    static uint8_t r[400];
    unsigned n = 0;
    r[n++] = (uint8_t)slot; r[n++] = m->type; r[n++] = (uint8_t)m->x; r[n++] = (uint8_t)((uint16_t)m->x >> 8);
    r[n++] = (uint8_t)m->y; r[n++] = (uint8_t)((uint16_t)m->y >> 8); r[n++] = m->flags; r[n++] = t->n_param;
    for (unsigned p = 0; p < t->n_param; ++p) { uint16_t q = quant(&t->param[p], m->p[p]); r[n++] = (uint8_t)q; r[n++] = (uint8_t)(q >> 8); }
    if (pw_has_data(m->type)) { memcpy(r + n, m->data, pw_data_len(m->type)); n += pw_data_len(m->type); }
    unsigned nc = n++, k = 0;
    for (unsigned c = 0; c < PW_MAX_CABLES && n + 5 <= sizeof r; ++c) {
        const pw_cable *q = &pws.cable[c];
        if (!q->used || (q->sm != slot && q->dm != slot)) continue;
        r[n++] = q->sm; r[n++] = q->so; r[n++] = q->dm; r[n++] = q->di; r[n++] = (uint8_t)q->amt; ++k;
    }
    r[nc] = (uint8_t)k;
    rec_push(U_REMOVE, r, n);
}

/* --------------------------------------------------------------- editing */
void pw_init(void)
{
    pw_modules_init();
    memset(&pws, 0, sizeof pws);
    pws.key_sens = 2;
    memset(outbuf_used, 0, sizeof outbuf_used);
    pws.bpm = 120.0f;
    memcpy(pws.name, "INIT", 5);
    pws.scope_mod = 0xff;
    pws.rng = 0x1234abcdu;
    pws.last_note = 60;
    pws.kb_pitch_note = 60;
    pws.voices = 1;
    memset(pws.perf, 0xff, sizeof pws.perf);
    for (unsigned k = 0; k < PW_PERF; ++k) pws.perf_cc[k] = (uint8_t)(21u + k);
    song_reset();
    undo_log.n = redo_log.n = 0;
    build_plan(&pws.plan[0]);
    build_plan(&pws.plan[1]);
}
int pw_count(void)
{
    int n = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) n += pws.mod[i].used == 1;
    return n;
}
int pw_cable_count(void)
{
    int n = 0;
    for (unsigned i = 0; i < PW_MAX_CABLES; ++i) n += pws.cable[i].used;
    return n;
}
int pw_add(unsigned type, int x, int y) { return pw_add_at(-1, type, x, y); }
PW_COLD int pw_add_at(int at, unsigned type, int x, int y)
{
    if (type >= PW_T_COUNT || at >= PW_MAX_MODULES || !pw_types[type].proc) return -1;   /* (a removed type has no proc) */
    const pw_type *t = &pw_types[type];
    int slot = -1;
    if (at >= 0) slot = pws.mod[at].used ? -1 : at;
    else for (unsigned i = 0; i < PW_MAX_MODULES && slot < 0; ++i) if (!pws.mod[i].used) slot = (int)i;
    if (slot < 0) return -1;
    unsigned free_out = 0;
    for (unsigned b = 0; b < PW_MAX_OUTBUF; ++b) free_out += !outbuf_used[b];
    if (free_out < t->n_out) return -1;
    pw_module *m = &pws.mod[slot];
    pws.pre_eff[slot] = 0xff; pws.pre_seen[slot] = 0xff;
    memset(m, 0, sizeof *m);
    m->type = (uint8_t)type;
    m->x = (int16_t)x; m->y = (int16_t)y;
    m->n_out = t->n_out;
    m->nv = 1;
    m->stp = m->st.raw;
    for (unsigned o = 0, b = 0; o < t->n_out; ++o) {
        while (outbuf_used[b]) ++b;
        outbuf_used[b] = 1;
        m->outbuf[o] = (uint16_t)b;
        memset(outbuf[b], 0, sizeof outbuf[b]);
    }
    for (unsigned p = 0; p < t->n_param; ++p) m->p[p] = t->param[p].def;
    if (type == PW_T_CLOCK) m->p[1] = pws.bpm;
    m->used = 3; /* reserved: not rendered, but owns memory during allocation */
    module_alloc(m, 0, 1);
    if (t->init) t->init(m);
    for (unsigned v = 0; v < PW_VARS; ++v) var_put(m, v);
    m->used = 1;
    { uint8_t r = (uint8_t)slot; rec_push(U_ADD, &r, 1); }
    publish();
    return slot;
}
PW_COLD int pw_remove(unsigned slot)
{
    if (slot < 64u) pws.mut_lock &= ~((uint64_t)1 << slot);
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1) return -1;
    rec_remove(slot);
    pw_module *m = &pws.mod[slot];
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c)
        if (pws.cable[c].used && (pws.cable[c].sm == slot || pws.cable[c].dm == slot)) pws.cable[c].used = 0;
    for (unsigned k = 0; k < PW_PERF; ++k) if (pws.perf[k][0] == slot) pws.perf[k][0] = pws.perf[k][1] = 0xff;
    for (unsigned l = 0; l < pws.song.n_lanes; ++l) if (pws.song.al[l].mod == slot) pws.song.al[l].mod = 0xff;
    for (unsigned j = 0; j < PW_MAX_MODULES; ++j)   /* a PARAM MOD that moved this module's knobs lets go */
        if (pws.mod[j].used == 1 && pws.mod[j].type == PW_T_PARAMMOD)
            for (unsigned c = 0; c < 4; ++c) if ((unsigned)(pws.mod[j].p[4 * c] + 0.5f) == slot + 1u) pws.mod[j].p[4 * c] = 0;
    for (unsigned v = 0; v < PW_VARS; ++v) pws.byp[v] &= ~((uint64_t)1 << slot);
    m->used = 2; /* excluded from the next plan, memory still valid */
    publish();
    wait_for_audio(pws.plan_cur);
    for (unsigned o = 0; o < m->n_out; ++o) outbuf_used[m->outbuf[o]] = 0;
    m->mem_len = m->reg_len = 0;
    m->used = 0;
    if (pws.scope_mod == slot) pws.scope_mod = 0xff;
    pws.revision++;
    return 0;
}
/* A module out of a chain, the chain closed: what fed its first signal input (an IN, not a control) goes on to what its first
 * used output fed (osc > reverb > output: the oscillator feeds the output). Not always what you meant: one undo brings it all back. */
int pw_remove_wired(unsigned slot)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1) return -1;
    const pw_type *t = &pw_types[pws.mod[slot].type];
    struct { uint8_t m, o, amt; } src[8];
    struct { uint8_t m, i, amt; } dd[8];
    unsigned pw_ = 0, nd = 0;
    int ip = -1, op = -1;
    for (unsigned i = 0; i < t->n_in && ip < 0; ++i) {
        if (!pw_audio_jack(t->in[i])) continue;
        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) if (pws.cable[c].used && pws.cable[c].dm == slot && pws.cable[c].di == i) { ip = (int)i; break; }
    }
    for (unsigned o = 0; o < t->n_out && op < 0; ++o)
        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) if (pws.cable[c].used && pws.cable[c].sm == slot && pws.cable[c].so == o) { op = (int)o; break; }
    if (ip >= 0 && op >= 0)
        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
            const pw_cable *k = &pws.cable[c];
            if (!k->used) continue;
            if (k->dm == slot && k->di == (unsigned)ip && k->sm != slot && pw_ < 8) { src[pw_].m = k->sm; src[pw_].o = k->so; src[pw_].amt = (uint8_t)k->amt; ++pw_; }
            if (k->sm == slot && k->so == (unsigned)op && k->dm != slot && nd < 8) { dd[nd].m = k->dm; dd[nd].i = k->di; dd[nd].amt = (uint8_t)k->amt; ++nd; }
        }
    pw_undo_group(1);
    int r = pw_remove(slot), made = 0;
    if (r == 0)
        for (unsigned a = 0; a < pw_; ++a)
            for (unsigned b = 0; b < nd; ++b) {
                if (src[a].m == dd[b].m) continue;
                int k = pw_connect(src[a].m, src[a].o, dd[b].m, dd[b].i);
                if (k >= 0) { ++made; int amt = (int)(int8_t)src[a].amt * (int)(int8_t)dd[b].amt / 100; if (amt != 100) pw_cable_amount((unsigned)k, amt); }
            }
    pw_undo_group(0);
    return r < 0 ? -1 : made;
}
void pw_clear(void)
{
    lock();
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) pws.cable[c].used = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) { pws.mod[i].used = 0; pws.mod[i].reg_len = 0; }
    memset(outbuf_used, 0, sizeof outbuf_used);
    pws.scope_mod = 0xff;
    pws.voices = 1; pws.var_cur = 0; pws.morphing = 0; pws.clear_serial++;
    memset((void *)pws.pre_eff, 0xff, sizeof pws.pre_eff); memset(pws.pre_seen, 0xff, sizeof pws.pre_seen);
    memset(pws.perf, 0xff, sizeof pws.perf);
    for (unsigned k = 0; k < PW_PERF; ++k) pws.perf_cc[k] = (uint8_t)(21u + k);
    song_reset();
    memset(pws.voice, 0, sizeof pws.voice);
    publish();
    pws.plan_ack = pws.plan_cur;
    unlock();
    pw_undo_reset();
}
int pw_find_cable(unsigned sm, unsigned so, unsigned dm, unsigned di)
{
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (k->used && k->sm == sm && k->so == so && k->dm == dm && k->di == di) return (int)c;
    }
    return -1;
}
static void poly_fixup(void);
int pw_connect(unsigned sm, unsigned so, unsigned dm, unsigned di)
{
    if (sm >= PW_MAX_MODULES || dm >= PW_MAX_MODULES || pws.mod[sm].used != 1 || pws.mod[dm].used != 1) return -1;
    if (so >= pw_types[pws.mod[sm].type].n_out || di >= pw_types[pws.mod[dm].type].n_in) return -1;
    int existing = pw_find_cable(sm, so, dm, di);
    if (existing >= 0) return existing;
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        if (pws.cable[c].used) continue;
        pw_cable *k = &pws.cable[c];
        k->sm = (uint8_t)sm; k->so = (uint8_t)so; k->dm = (uint8_t)dm; k->di = (uint8_t)di;
        k->used = 1; k->amt = 100; pws.cable_born[c] = ++pws.cable_clock;
        { uint8_t r[4] = {k->sm, k->so, k->dm, k->di}; rec_push(U_CONN, r, 4); }
        if (pws.voices > 1) poly_fixup();
        publish();
        return (int)c;
    }
    return -1;
}
int pw_disconnect(unsigned cable)
{
    if (cable >= PW_MAX_CABLES || !pws.cable[cable].used) return -1;
    const pw_cable *k = &pws.cable[cable];
    { uint8_t r[5] = {k->sm, k->so, k->dm, k->di, (uint8_t)k->amt}; rec_push(U_DISC, r, 5); }
    pws.cable[cable].used = 0;
    publish();
    return 0;
}
void pw_cable_amount(unsigned cable, int amt)
{
    if (cable >= PW_MAX_CABLES || !pws.cable[cable].used) return;
    amt = amt < -100 ? -100 : amt > 100 ? 100 : amt;
    pw_cable *k = &pws.cable[cable];
    if (amt == k->amt) return;
    if (!rec_same(U_CAMT, k->dm, (unsigned)(k->di * 64u + k->sm), 1500u)) { uint8_t r[5] = {k->sm, k->so, k->dm, k->di, (uint8_t)k->amt}; rec_push(U_CAMT, r, 5); }
    k->amt = (int8_t)amt;
    publish();
    pws.revision++;
}
/* display reads (control side): the last sample a module's output made, and what its input last received (every
 * patched cable at its amount, summed) */
float pw_out_last(unsigned slot, unsigned port)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || port >= pw_types[pws.mod[slot].type].n_out) return 0;
    return outbuf[pws.mod[slot].outbuf[port]][PW_N - 1];
}
float pw_in_last(unsigned slot, unsigned in)
{
    float v = 0;
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (k->used && k->dm == slot && k->di == in) v += pw_out_last(k->sm, k->so) * (float)k->amt * 0.01f;
    }
    return v;
}
int pw_input_cables(unsigned slot, unsigned in)
{
    int n = 0;
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c)
        n += pws.cable[c].used && pws.cable[c].dm == slot && pws.cable[c].di == in;
    return n;
}
static float sanitize(const pw_param_desc *d, float v)
{
    if (!(v == v)) v = d->def;
    v = pw_clamp(v, d->min, d->max);
    if (d->kind == PW_ENUM || d->kind == PW_INT) v = (float)pw_floori(v + 0.5f);
    return v;
}
void pw_set_param(unsigned slot, unsigned p, float v)
{
    if (slot >= PW_MAX_MODULES || !pws.mod[slot].used) return;
    const pw_type *t = &pw_types[pws.mod[slot].type];
    if (p >= t->n_param) return;
    v = sanitize(&t->param[p], v);
    if ((pws.mod[slot].type == PW_T_CZ || pws.mod[slot].type == PW_T_FMDX) && (p == 0 || p == 1) && rec_mode == 0 && rec_on) v = pw_cz_clamp(slot, p, v);   /* (not while a patch loads: its bank may be gone) */   /* (a bank that holds tones, a preset that is there) */
    int kit = ((int)p == pw_preset_param(pws.mod[slot].type) || ((pws.mod[slot].type == PW_T_CZ || pws.mod[slot].type == PW_T_FMDX) && p == 0)) && v != pws.mod[slot].p[p] && rec_mode == 0 && rec_on;   /* (not while a patch loads) */
    if (kit) pw_undo_group(1);                       /* PRESET (a DRUM RACK's KIT): the other knobs follow, all one undo */
    if (v != pws.mod[slot].p[p] && !rec_same(U_PARAM, slot, p, 1500u)) {
        uint8_t r[6] = {(uint8_t)slot, (uint8_t)p}; putf_(r + 2, pws.mod[slot].p[p]); rec_push(U_PARAM, r, 6);
    }
    if (pws.edit_all && rec_on && !kit && pws.mod[slot].reg_len) {   /* EDIT ALL: the snapshots that still held the old value follow */
        uint8_t *vs = var_store(&pws.mod[slot]);
        uint16_t qo = quant(&t->param[p], pws.mod[slot].p[p]), qn = quant(&t->param[p], v);
        for (unsigned w = 0; w < PW_VARS; ++w) {
            if (w == pws.var_cur) continue;
            uint8_t *q = vs + (w * t->n_param + p) * 2;
            if ((uint16_t)(q[0] | (q[1] << 8)) == qo) { q[0] = (uint8_t)qn; q[1] = (uint8_t)(qn >> 8); }
        }
    }
    int pm_moved = pws.mod[slot].type == PW_T_PARAMMOD && (p & 3u) == 0 && v != pws.mod[slot].p[p] && rec_mode == 0 && rec_on;
    pws.mod[slot].p[p] = v;
    if (pm_moved) publish();   /* (which module runs before which changes) */
    if (kit) { pw_module_preset(slot, (int)(v + 0.5f)); pw_undo_group(0); }
    if (pws.mod[slot].type == PW_T_CLOCK && p == 1 && pws.mod[slot].p[0] < 0.5f) pws.bpm = v;
    if (pws.mod[slot].type == PW_T_CLOCK && p == 0 && v < 0.5f) pws.mod[slot].p[1] = pws.bpm;
    pws.revision++;
}
void pw_preset_apply(unsigned slot, int k)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1) return;
    uint8_t was = rec_on; rec_on = 0;
    pw_module_preset(slot, k);
    rec_on = was;
    pws.revision++;
}
float pw_get_param(unsigned slot, unsigned p)
{
    if (slot >= PW_MAX_MODULES || !pws.mod[slot].used || p >= PW_MAX_PARAM) return 0;
    return pws.mod[slot].p[p];
}
void pw_undo_data(unsigned slot)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || rec_same(U_DATA, slot, 0, 2000u)) return;
    uint8_t r[1 + PW_DATA_MAX];
    r[0] = (uint8_t)slot; memcpy(r + 1, pws.mod[slot].data, PW_DATA_MAX);
    rec_push(U_DATA, r, 1u + pw_data_len(pws.mod[slot].type));
}
/* SEQUENCER / MOD SEQ: a new random pattern (SEQUENCER: pitches from a minor pentatonic over two octaves, most steps on, its ratchets, chances and
 * velocities left as they are; MOD SEQ: every step of every lane), or a clean slate (notes and gates off / every lane at 0, step features back to normal). */
PW_COLD void pw_seq_fill(unsigned slot, int clear)
{
    static uint32_t rs = 0x2468ACEu;
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1) return;
    pw_module *m = &pws.mod[slot];
    static const int8_t pent[11] = {-12, -9, -7, -5, -2, 0, 3, 5, 7, 10, 12};
    if (m->type == PW_T_SEQ) {
        pw_undo_data(slot);
        unsigned g = 0;
        for (unsigned i = 0; i < 16; ++i) {
            rs = rs * 1664525u + 1013904223u;
            unsigned r = rs >> 16;
            m->data[i] = clear ? 0 : (uint8_t)pent[(r >> 4) % 11u];
            if (!clear && (i == 0 || (r & 7u) < 5u)) g |= 1u << i;
            if (clear) { m->data[PW_SEQ_STEPFEAT + i] = 0xF0; m->data[PW_SEQ_STEPVEL + i] = 200; }
        }
        m->data[16] = (uint8_t)g; m->data[17] = (uint8_t)(g >> 8);
    } else if (m->type == PW_T_MODSEQ) {
        pw_undo_data(slot);
        for (unsigned l = 0; l < 4; ++l)
            for (unsigned i = 0; i < 16; ++i) { rs = rs * 1664525u + 1013904223u; pw_mseq_set(m, l, i, clear ? 0 : (int)(rs >> 24)); }
    } else return;
    pws.revision++;
}
/* MUTATE: a nudge or a roll of the patch's knobs and sequencer steps (the MUTATE page, or a MUTATE module's jacks). Every module that is not locked
 * (pws.mut_lock) and has something to change takes part; each knob or step is touched with the chance DENSITY. A NUDGE moves a knob a little each
 * way (AMOUNT 100% is a quarter of its range); a ROLL moves it toward a new random value (100%: all the way). Choices (enums, whole numbers) change
 * with the chance AMOUNT (a nudge: one step, half as often). Left alone: levels, tuning and presets (so the patch stays in tune and does not
 * jump in loudness), and feedback and resonance are kept below 80% of their range. One undo step. */
int pw_mut_eligible(unsigned slot)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1) return 0;
    unsigned ty = pws.mod[slot].type;
    switch (ty) {
    case PW_T_OUTPUT: case PW_T_KEYS: case PW_T_MIDIIN: case PW_T_MIDIOUT: case PW_T_AUDIOIN: case PW_T_PARAMMOD: case PW_T_MUTATE: case PW_T_SNAPSHOT:
    case PW_T_CLOCK: case PW_T_BUTTON: case PW_T_SCOPE: case PW_T_LISTENER: return 0;
    default: break;
    }
    if (pw_type_hidden(ty)) return 0;
    return pw_types[ty].n_param > 0;
}
/* small string helpers (the freestanding builds have no strstr or strcmp) */
static int mut_has(const char *s, const char *sub)
{
    for (; *s; ++s) { const char *a = s, *b = sub; while (*a && *b && *a == *b) { ++a; ++b; } if (!*b) return 1; }
    return 0;
}
static int mut_is(const char *s, const char *w) { while (*s && *s == *w) { ++s; ++w; } return *s == *w; }

static int mut_skip(const char *nm)
{
    static const char *const bad[] = {"LEVEL", "VOL", "TUNE", "FINE", "TRANSP", "OCTAVE", "COARSE", "BANK", "PRESET", "KIT", "TEMPO", "BPM", "MASTER", "PROTECT", "LVL", "ROOT", "SAMPLE", "SYNC", "MODULE", "PARAM"};
    for (unsigned k = 0; k < sizeof bad / sizeof *bad; ++k) if (mut_has(nm, bad[k])) return 1;
    return 0;
}
static float mut_rnd(void)   /* 0..1 */
{
    static uint32_t rs = 0x1F123BB5u;
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return (float)(rs >> 8) * (1.0f / 16777216.0f);
}
static uint16_t mut_gen, mut_mark[PW_MAX_MODULES];   /* for the visualization: which call last changed each module */
static uint8_t mut_last_roll;
/* how many mutations ago a module was last changed (0 = the latest; 99 = not lately or never) */
int pw_mut_age(unsigned slot) { if (slot >= PW_MAX_MODULES || !mut_mark[slot]) return 99; unsigned a = (uint16_t)(mut_gen - mut_mark[slot]); return a > 98u ? 99 : (int)a; }
int pw_mut_last_roll(void) { return mut_last_roll; }
PW_COLD int pw_mutate(int roll, float amount, float density)
{
    static const int8_t pent[11] = {-12, -9, -7, -5, -2, 0, 3, 5, 7, 10, 12};
    int changed = 0;
    amount = amount < 0 ? 0 : amount > 1 ? 1 : amount;
    density = density < 0 ? 0 : density > 1 ? 1 : density;
    pw_undo_group(1);
    if (++mut_gen == 0) mut_gen = 1;
    mut_last_roll = (uint8_t)(roll != 0);
    for (unsigned slot = 0; slot < PW_MAX_MODULES; ++slot) {
        if (!pw_mut_eligible(slot) || (slot < 64u && (pws.mut_lock >> slot & 1u))) continue;
        pw_module *m = &pws.mod[slot];
        const int changed0 = changed;
        const pw_type *t = &pw_types[m->type];
        for (unsigned p = 0; p < t->n_param; ++p) {
            const pw_param_desc *d = &t->param[p];
            if (mut_skip(d->name) || mut_rnd() >= density) continue;
            float v = m->p[p], nv = v;
            if (d->kind == PW_ENUM || d->kind == PW_INT) {
                if (roll) { if (mut_rnd() < amount) nv = (float)pw_floori(d->min + (d->max - d->min + 1.0f) * mut_rnd()); }
                else if (mut_rnd() < amount * 0.5f) nv = v + (mut_rnd() < 0.5f ? -1.0f : 1.0f);
                if (nv < d->min) nv = d->min;
                if (nv > d->max) nv = d->max;
            } else {
                float n = pw_lane_norm(slot, p, v);
                if (roll) n += (mut_rnd() - n) * amount;
                else n += (mut_rnd() * 2.0f - 1.0f) * amount * 0.25f;
                if (n < 0) n = 0;
                if (n > 1) n = 1;
                if ((mut_has(d->name, "FEEDB") || mut_is(d->name, "RES")) && n > 0.8f) n = 0.8f;
                nv = norm_to_value(d, n);
            }
            if (nv != v) { pw_set_param(slot, p, nv); ++changed; }
        }
        if (m->type == PW_T_SEQ) {   /* the steps: a roll draws new notes and gates, a nudge moves a note a little and now and then flips a gate */
            unsigned g = (unsigned)(m->data[16] | (m->data[17] << 8)), g0 = g;
            int moved = 0;
            pw_undo_data(slot);
            for (unsigned i = 0; i < 16; ++i) {
                if (mut_rnd() >= density) continue;
                int note = (int8_t)m->data[i], was = note;
                if (roll) { if (mut_rnd() < amount) note = pent[(unsigned)(mut_rnd() * 11.0f) % 11u]; if (mut_rnd() < amount * 0.4f) g ^= 1u << i; }
                else { int step = 1 + (int)(amount * 3.0f); note += (mut_rnd() < 0.5f ? -step : step); if (mut_rnd() < amount * 0.15f) g ^= 1u << i; }
                {   /* a 303 line: now and then a step turns into a glide or a tie (no ratchet on it), or goes back to normal */
                    uint8_t *fb = &m->data[PW_SEQ_STEPFEAT + i], was_fb = *fb;
                    if (roll) { if (mut_rnd() < amount * 0.25f) { float r = mut_rnd(); *fb = (uint8_t)((*fb & 0xf0) | (r < 0.5f ? 0u : r < 0.75f ? 4u : 8u)); } }
                    else if (mut_rnd() < amount * 0.12f) *fb = (uint8_t)((*fb & 0xf0) | (((*fb >> 2) & 3) == 2 ? 0u : 8u));
                    if (*fb != was_fb) moved = 1;
                }
                note = note < -36 ? -36 : note > 36 ? 36 : note;
                if (note != was) moved = 1;
                m->data[i] = (uint8_t)(int8_t)note;
            }
            m->data[16] = (uint8_t)g; m->data[17] = (uint8_t)(g >> 8);
            if (moved || g != g0) ++changed;
        } else if (m->type == PW_T_MODSEQ) {
            int moved = 0;
            pw_undo_data(slot);
            for (unsigned l = 0; l < 4; ++l)
                for (unsigned i = 0; i < 16; ++i) {
                    if (mut_rnd() >= density) continue;
                    int cur = (int)pw_mseq_get(m, l, i), was = cur;
                    if (roll) cur += (int)(((float)(int)(mut_rnd() * 255.0f) - (float)cur) * amount);
                    else cur += (int)((mut_rnd() * 2.0f - 1.0f) * amount * 64.0f);
                    cur = cur < 0 ? 0 : cur > 255 ? 255 : cur;
                    if (cur != was) { pw_mseq_set(m, l, i, cur); moved = 1; }
                }
            if (moved) ++changed;
        }
        if (changed != changed0) mut_mark[slot] = mut_gen;
    }
    pw_undo_group(0);
    if (changed) pws.revision++;
    return changed;
}
/* the MUTATE modules: each counts its NUDGE and ROLL edges; the screen's loop does the mutating (it changes knobs and the undo log, which the audio side must not) */
void pw_mutate_follow(uint32_t now)
{
    static float seen[PW_MAX_MODULES][2];
    static uint32_t last_ms;
    (void)now;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        if (pws.mod[i].used != 1 || pws.mod[i].type != PW_T_MUTATE) { seen[i][0] = seen[i][1] = -1.0f; continue; }
        float nc = pw_live(i, 0), rc = pw_live(i, 1);
        if (seen[i][0] < 0) { seen[i][0] = nc; seen[i][1] = rc; continue; }   /* (just added: what was counted before is not ours to do) */
        float amt = pw_live(i, 2), den = pws.mod[i].p[1];
        if (nc != seen[i][0]) { seen[i][0] = nc; pw_mutate(0, amt, den); last_ms = now; }
        if (rc != seen[i][1]) { seen[i][1] = rc; pw_mutate(1, amt, den); last_ms = now; }
    }
    (void)last_ms;
}
/* A knob detent: about 40 across a parameter's whole range (so a full sweep is a turn or two of the
 * FM-1's encoders); GLO held (fine): a tenth of that. Exponential parameters (frequencies, times) move
 * by a fortieth of their octaves; semitones, cents and tempo by musical steps. */
void pw_nudge_param(unsigned slot, unsigned p, int detents, int fine)
{
    if (slot >= PW_MAX_MODULES || !pws.mod[slot].used || !detents) return;
    const pw_type *t = &pw_types[pws.mod[slot].type];
    if (p >= t->n_param) return;
    const pw_param_desc *d = &t->param[p];
    float v = pws.mod[slot].p[p];
    switch (d->kind) {
    case PW_EXP: {
        float lo = d->min > 0 ? d->min : 0.001f;
        float octaves = pw_log2(d->max / lo);
        if (octaves < 1.0f) octaves = 1.0f;
        if (v < lo) v = lo;
        v *= pw_exp2((float)detents * octaves / (fine ? 400.0f : 40.0f));
    } break;
    case PW_ENUM: case PW_INT:
        if (pws.mod[slot].type == PW_T_PARAMMOD && (p & 3u) == 0) {   /* MODULE: on to the next module there is (0: none) */
            int cur = (int)(v + 0.5f), dir = detents > 0 ? 1 : -1;
            for (int n = detents < 0 ? -detents : detents; n > 0; --n) {
                int c = cur + dir;
                while (c >= 1 && c <= PW_MAX_MODULES && !(pws.mod[c - 1].used == 1 && (unsigned)(c - 1) != slot)) c += dir;
                if (c < 0 || c > PW_MAX_MODULES) break;
                cur = c;
            }
            v = (float)cur;
        } else if (pws.mod[slot].type == PW_T_PARAMMOD && (p & 3u) == 1) {   /* PARAM: the knobs the target has */
            int tg = pw_pm_target(slot, p >> 2), top = tg >= 0 ? (int)pw_types[pws.mod[tg].type].n_param - 1 : 0;
            v += (float)detents;
            if (v > (float)top) v = (float)top;
        } else v += (float)detents;
        break;
    default: {
        float step = (d->max - d->min) / 40.0f;
        if (d->unit == PW_U_SEMI) step = 1.0f;
        else if (d->unit == PW_U_CENT) step = 5.0f;
        else if (d->unit == PW_U_BPM) step = 2.0f;
        if (fine) step *= 0.1f;
        v += step * (float)detents;
        if (pw_absf(v) < step * 0.01f) v = 0;
    } break;
    }
    pw_set_param(slot, p, v);
    if (pws.mod[slot].type == PW_T_DRUMRACK && p >= 4 && p <= 32 && (p & 3) == 0 && pws.mod[slot].p[p] >= 39.5f) pw_rack_voice(slot, p);
}
void pw_move(unsigned slot, int x, int y)
{
    if (slot >= PW_MAX_MODULES || !pws.mod[slot].used) return;
    if (!rec_same(U_MOVE, slot, 0, 1500u)) {
        uint8_t r[5] = {(uint8_t)slot, (uint8_t)pws.mod[slot].x, (uint8_t)((uint16_t)pws.mod[slot].x >> 8),
                        (uint8_t)pws.mod[slot].y, (uint8_t)((uint16_t)pws.mod[slot].y >> 8)};
        rec_push(U_MOVE, r, 5);
    }
    pws.mod[slot].x = (int16_t)x; pws.mod[slot].y = (int16_t)y;
    pws.revision++;
}

/* -------------------------------------------------------- adding where the cursor is
 * A jack with cables: the new module goes into them (from an output: into every cable it has; into an input: the
 * cables feeding it go into the new module, and the new module feeds the input), when it has an input and an output
 * and the jack carries a signal (an IN, not a control like FREQ: there the new module's output joins in). A jack with
 * no cable: an output feeds the new module, an input is fed by it. A module without the jack it needs: alone. */
int pw_audio_jack(const char *name)
{
    if (name[0] == 'I' && name[1] == 'N') return name[2] == 0 || name[2] == ' ';
    if ((name[0] == 'X' || name[0] == 'L' || name[0] == 'R') && !name[1]) return 1;
    static const char ex[] = "EXCITE";
    for (unsigned i = 0; i < sizeof ex; ++i) if (name[i] != ex[i]) return 0;
    return 1;
}
int pw_wire_plan(unsigned type, int slot, int is_out, unsigned port, int cable, unsigned *in, unsigned *out)
{
    const pw_type *t = &pw_types[type];
    int plain = is_out & 2;   /* carrying a cable: plug into the loose end, never insert */
    is_out &= 1;
    *in = 0; *out = 0;
    for (unsigned i = 0; i < t->n_in; ++i) if (pw_audio_jack(t->in[i])) { *in = i; break; }
    if (cable >= 0 && cable < PW_MAX_CABLES && pws.cable[cable].used)
        return t->n_in && t->n_out ? PW_WIRE_INSERT : PW_WIRE_ALONE;
    if (slot < 0 || slot >= PW_MAX_MODULES || pws.mod[slot].used != 1) return PW_WIRE_ALONE;
    const pw_type *jt = &pw_types[pws.mod[slot].type];
    int cables = 0;
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (k->used && (is_out ? (k->sm == slot && k->so == port) : (k->dm == slot && k->di == port))) ++cables;
    }
    if (is_out) {
        if (!t->n_in) return PW_WIRE_ALONE;
        return cables && t->n_out && !plain ? PW_WIRE_INSERT : PW_WIRE_FEED;
    }
    if (!t->n_out) return PW_WIRE_ALONE;
    if (cables && !plain && t->n_in && port < jt->n_in && pw_audio_jack(jt->in[port])) return PW_WIRE_INSERT;
    return PW_WIRE_DRIVE;
}
int pw_add_wired(unsigned type, int x, int y, int slot, int is_out, unsigned port, int cable)
{
    unsigned in, out;
    int kind = pw_wire_plan(type, slot, is_out, port, cable, &in, &out);
    is_out &= 1;
    pw_undo_group(1);
    int s = pw_add(type, x, y);
    if (s >= 0 && kind != PW_WIRE_ALONE) {
        pw_cable list[PW_MAX_CABLES];
        unsigned n = 0;
        if (cable >= 0) {
            list[n++] = pws.cable[cable];
            pw_disconnect((unsigned)cable);
            pw_connect(list[0].sm, list[0].so, (unsigned)s, in); pw_connect((unsigned)s, out, list[0].dm, list[0].di);
        } else if (kind == PW_WIRE_INSERT) {
            for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
                const pw_cable *k = &pws.cable[c];
                if (k->used && (is_out ? (k->sm == slot && k->so == port) : (k->dm == slot && k->di == port))) { list[n++] = *k; pw_disconnect(c); }
            }
            if (is_out) {
                pw_connect((unsigned)slot, port, (unsigned)s, in);
                for (unsigned i = 0; i < n; ++i) pw_connect((unsigned)s, out, list[i].dm, list[i].di);
            } else {
                for (unsigned i = 0; i < n; ++i) pw_connect(list[i].sm, list[i].so, (unsigned)s, in);
                pw_connect((unsigned)s, out, (unsigned)slot, port);
            }
        } else if (kind == PW_WIRE_FEED) pw_connect((unsigned)slot, port, (unsigned)s, in);
        else pw_connect((unsigned)s, out, (unsigned)slot, port);
    }
    pw_undo_group(0);
    return s;
}

/* ------------------------------------------------------------- polyphony */
/* every module in the voice area holds pws.voices voices (its region remade, its sound restarted) */
static void poly_realloc(void)
{
    lock();
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        pw_module *m = &pws.mod[i];
        if (m->used != 1) continue;
        unsigned want = (m->flags & PW_F_POLY) && pws.voices > 1 ? pws.voices : 1;
        if (m->nv == want) continue;
        unsigned old_nv = m->nv;
        m->nv = (uint8_t)want;
        uint8_t keep[PW_STATE_BYTES];
        memcpy(keep, m->st.raw, PW_STATE_BYTES);
        module_alloc(m, 1, old_nv);
        if (m->type == PW_T_WAVETABLE && m->status == PW_STATUS_OK) pw_wt_build(m);   /* (the region was cleared) */
        memcpy(m->st.raw, keep, PW_STATE_BYTES);
        for (unsigned v = 1; v < m->nv; ++v) memcpy(voice_state(m, v), m->st.raw, PW_STATE_BYTES);
        m->stp = m->st.raw;
    }
    publish();
    unlock();
}
/* what sits between two voice modules joins the voice area (it could not hear each voice otherwise) */
static void poly_fixup(void)
{
    if (pws.voices < 2) return;
    uint8_t down[PW_MAX_MODULES], up[PW_MAX_MODULES];
    int grew = 0;
    for (;;) {
        memset(down, 0, sizeof down); memset(up, 0, sizeof up);
        for (unsigned m = 0; m < PW_MAX_MODULES; ++m) if (pws.mod[m].used == 1 && (pws.mod[m].flags & PW_F_POLY)) down[m] = up[m] = 1;
        for (int ch = 1; ch;) {
            ch = 0;
            for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
                const pw_cable *k = &pws.cable[c];
                if (!k->used) continue;
                if (down[k->sm] && !down[k->dm]) { down[k->dm] = 1; ch = 1; }
                if (up[k->dm] && !up[k->sm]) { up[k->sm] = 1; ch = 1; }
            }
        }
        int added = 0;
        for (unsigned m = 0; m < PW_MAX_MODULES; ++m)
            if (pws.mod[m].used == 1 && !(pws.mod[m].flags & PW_F_POLY) && down[m] && up[m]) { pws.mod[m].flags |= PW_F_POLY; added = grew = 1; }
        if (!added) break;
    }
    if (grew) poly_realloc();
}
void pw_set_voices(unsigned n)
{
    if (n < 1) n = 1;
    if (n > PW_MAX_VOICES) n = PW_MAX_VOICES;
    if (n == pws.voices) return;
    { uint8_t r = pws.voices; rec_push(U_VOICES, &r, 1); }
    pws.voices = (uint8_t)n;
    memset(pws.voice, 0, sizeof pws.voice);
    if (n > 1) {
        int any = 0;
        for (unsigned i = 0; i < PW_MAX_MODULES; ++i) any |= pws.mod[i].used == 1 && (pws.mod[i].flags & PW_F_POLY);
        if (!any) { uint8_t was = rec_on; rec_on = 0; pw_auto_poly(); rec_on = was; }
        poly_fixup();
    }
    poly_realloc();
    pws.revision++;
}
void pw_set_flags(unsigned slot, unsigned flags)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || pws.mod[slot].flags == flags) return;
    { uint8_t r[2] = {(uint8_t)slot, pws.mod[slot].flags}; rec_push(U_FLAGS, r, 2); }
    unsigned was = pws.mod[slot].flags;
    pws.mod[slot].flags = (uint8_t)flags;
    if ((was ^ flags) & PW_F_POLY) { poly_fixup(); poly_realloc(); }
    publish();
    pws.revision++;
}
void pw_set_group(unsigned slot, unsigned g)   /* the module's group, 0 none (a view of the patch; the sound is unchanged) */
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || g > PW_MAX_GROUP || pws.mod[slot].group == g) return;
    { uint8_t r[2] = {(uint8_t)slot, pws.mod[slot].group}; rec_push(U_GROUP, r, 2); }
    pws.mod[slot].group = (uint8_t)g;
    pws.revision++;
}
/* the keys' voice: KEYS and what it reaches through sources, filters, envelopes, amplifiers and shapers; not
 * mixers, effects or the output (they hear every voice at once) */
void pw_auto_poly(void)
{
    uint8_t in[PW_MAX_MODULES];
    memset(in, 0, sizeof in);
    for (unsigned m = 0; m < PW_MAX_MODULES; ++m)
        if (pws.mod[m].used == 1 && (pws.mod[m].type == PW_T_KEYS || pws.mod[m].type == PW_T_ARP || pws.mod[m].type == PW_T_PROLL)) in[m] = 1;
    for (int ch = 1; ch;) {
        ch = 0;
        for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
            const pw_cable *k = &pws.cable[c];
            if (!k->used || !in[k->sm] || in[k->dm]) continue;
            unsigned t = pws.mod[k->dm].type, cat = pw_types[t].cat;
            int voice = cat == PW_C_SOURCE || cat == PW_C_MOD || t == PW_T_VCF || t == PW_T_VCA || t == PW_T_LPG ||
                        t == PW_T_VOWEL || t == PW_T_COMB || t == PW_T_FOLDER || t == PW_T_DIST || t == PW_T_RING ||
                        t == PW_T_QUANT || t == PW_T_CHORD;
            if (voice) { in[k->dm] = 1; ch = 1; }
        }
    }
    for (unsigned m = 0; m < PW_MAX_MODULES; ++m)
        if (pws.mod[m].used == 1 && in[m] && pws.mod[m].type != PW_T_ARP) pw_set_flags(m, pws.mod[m].flags | PW_F_POLY);
}

/* ------------------------------------------------------------ variations */
void pw_var_select(unsigned v)
{
    if (v >= PW_VARS || v == pws.var_cur) return;
    { uint8_t r = pws.var_cur; rec_push(U_VAR, &r, 1); }
    if (!pws.morphing) for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) var_put(&pws.mod[i], pws.var_cur);
    pws.morphing = 0;
    pws.byp[pws.var_cur] = byp_now(); pws.byp_has |= (uint16_t)(1u << pws.var_cur);
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) var_take(&pws.mod[i], v);
    pws.var_cur = (uint8_t)v;
    byp_apply(v);
    pws.revision++;
}
/* MORPH: every knob part way from variation a (the current one) to b; t 0 is a exactly. The current variation's
 * store is written once, as the morph begins, and kept (pws.morphing) until it ends or another variation is chosen. */
PW_COLD void pw_var_morph(unsigned a, unsigned b, float t)
{
    if (a >= PW_VARS || b >= PW_VARS) return;
    if (!pws.morphing) {
        if (t <= 0.0f) return;
        for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1) var_put(&pws.mod[i], a);
        pws.morphing = 1;
    }
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        pw_module *m = &pws.mod[i];
        if (m->used != 1 || !m->reg_len || m->type == PW_T_SNAPSHOT) continue;
        const pw_type *ty = &pw_types[m->type];
        for (unsigned p = 0; p < ty->n_param; ++p) {
            const pw_param_desc *d = &ty->param[p];
            float va = var_get(m, a, p), vb = var_get(m, b, p), v;
            if (d->kind == PW_ENUM || d->kind == PW_INT) v = t < 0.5f ? va : vb;
            else if (d->kind == PW_EXP && va > 0 && vb > 0) v = va * pw_exp2(t * pw_log2(vb / va));
            else v = va + (vb - va) * t;
            m->p[p] = v;
        }
    }
    if (t <= 0.0f) pws.morphing = 0;
    pws.revision++;
}
void pw_var_copy(unsigned from, unsigned to)
{
    if (from >= PW_VARS || to >= PW_VARS || from == to) return;
    {   /* (the bypassed modules too) */
        uint64_t b = from != pws.var_cur && (pws.byp_has >> from & 1u) ? pws.byp[from] : byp_now();
        pws.byp[to] = b; pws.byp_has |= (uint16_t)(1u << to);
        if (to == pws.var_cur) byp_apply(to);
    }
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        pw_module *m = &pws.mod[i];
        if (m->used != 1 || !m->reg_len) continue;
        if (from == pws.var_cur) var_put(m, to);
        else if (to == pws.var_cur) var_take(m, from);
        else {
            const pw_type *t = &pw_types[m->type];
            uint8_t *vs = var_store(m);
            memcpy(vs + to * t->n_param * 2u, vs + from * t->n_param * 2u, t->n_param * 2u);
            if (pw_has_data(m->type)) memcpy(vs + PW_VARS * t->n_param * 2u + to * pw_data_len(m->type), vs + PW_VARS * t->n_param * 2u + from * pw_data_len(m->type), pw_data_len(m->type));
        }
    }
    pws.revision++;
}

/* ------------------------------------------------------- performance page */
PW_COLD int pw_perf_assign(unsigned slot, unsigned p)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || p >= pw_types[pws.mod[slot].type].n_param) return -1;
    for (unsigned k = 0; k < PW_PERF; ++k) if (pws.perf[k][0] == slot && pws.perf[k][1] == p) return (int)k;
    for (unsigned k = 0; k < PW_PERF; ++k)
        if (pws.perf[k][0] == 0xff) {
            uint8_t r[3] = {(uint8_t)k, 0xff, 0xff}; rec_push(U_PERF, r, 3);
            pws.perf[k][0] = (uint8_t)slot; pws.perf[k][1] = (uint8_t)p; pws.revision++;
            return (int)k;
        }
    return -1;
}
void pw_perf_clear(unsigned k)
{
    if (k >= PW_PERF || pws.perf[k][0] == 0xff) return;
    uint8_t r[3] = {(uint8_t)k, pws.perf[k][0], pws.perf[k][1]}; rec_push(U_PERF, r, 3);
    pws.perf[k][0] = pws.perf[k][1] = 0xff; pws.revision++;
}

/* --------------------------------------------------------- undo and redo */
void pw_undo_reset(void) { undo_log.n = redo_log.n = 0; last_kind = 0; }
void pw_undo_group(int open)
{
    if (open) { if (!group_open++) ++group_id; }
    else if (group_open) --group_open;
}
/* one record undone (or redone): what it changed put back, its own undoing logged on the other side */
static void rec_apply(const uint8_t *r)
{
    const uint8_t *a = r + 4;
    switch (r[0]) {
    case U_ADD: if (pws.mod[a[0]].used == 1) pw_remove(a[0]); break;
    case U_REMOVE: {
        unsigned slot = a[0], type = a[1], np = a[7], n = 8;
        int x = (int16_t)(a[2] | (a[3] << 8)), y = (int16_t)(a[4] | (a[5] << 8));
        if (pws.mod[slot].used) break;
        uint8_t was = rec_on;
        if (pw_add_at((int)slot, type, x, y) < 0) break;
        rec_on = 0;
        pw_module *m = &pws.mod[slot];
        const pw_type *t = &pw_types[type];
        for (unsigned p = 0; p < np && p < t->n_param; ++p) m->p[p] = sanitize(&t->param[p], dequant(&t->param[p], (uint16_t)(a[n + 2 * p] | (a[n + 2 * p + 1] << 8))));
        n += np * 2u;
        if (pw_has_data(type)) { memcpy(m->data, a + n, pw_data_len(type)); n += pw_data_len(type); }
        for (unsigned v = 0; v < PW_VARS; ++v) var_put(m, v);
        unsigned nc = a[n++];
        for (unsigned c = 0; c < nc; ++c, n += 5) { int k = pw_connect(a[n], a[n + 1], a[n + 2], a[n + 3]); if (k >= 0) pws.cable[k].amt = (int8_t)a[n + 4]; }
        if (a[6] & PW_F_POLY) pw_set_flags(slot, a[6]);
        rec_on = was;
    } break;
    case U_CONN: { int c = pw_find_cable(a[0], a[1], a[2], a[3]); if (c >= 0) pw_disconnect((unsigned)c); } break;
    case U_DISC: { int k = pw_connect(a[0], a[1], a[2], a[3]); if (k >= 0 && pws.cable[k].amt != (int8_t)a[4]) { pws.cable[k].amt = (int8_t)a[4]; publish(); } } break;
    case U_CAMT: { int k = pw_find_cable(a[0], a[1], a[2], a[3]); if (k >= 0) { last_kind = 0; pw_cable_amount((unsigned)k, (int8_t)a[4]); } } break;
    case U_PARAM: last_kind = 0; pw_set_param(a[0], a[1], getf_(a + 2)); break;
    case U_DATA:
        if (pws.mod[a[0]].used == 1) { last_kind = 0; pw_undo_data(a[0]); memcpy(pws.mod[a[0]].data, a + 1, pw_data_len(pws.mod[a[0]].type)); pws.revision++; }
        break;
    case U_MOVE: last_kind = 0; pw_move(a[0], (int16_t)(a[1] | (a[2] << 8)), (int16_t)(a[3] | (a[4] << 8))); break;
    case U_FLAGS: pw_set_flags(a[0], a[1]); break;
    case U_GROUP: pw_set_group(a[0], a[1]); break;
    case U_VOICES: pw_set_voices(a[0]); break;
    case U_VAR: pw_var_select(a[0]); break;
    case U_PERF: {
        uint8_t back[3] = {a[0], pws.perf[a[0]][0], pws.perf[a[0]][1]}; rec_push(U_PERF, back, 3);
        pws.perf[a[0]][0] = a[1]; pws.perf[a[0]][1] = a[2]; pws.revision++;
    } break;
    default: break;
    }
}
PW_COLD static int undo_step(ulog *from, unsigned mode)
{
    uint8_t *last = rec_last(from);
    if (!last) return 0;
    unsigned g = last[2] | (last[3] << 8);
    rec_mode = (uint8_t)mode;
    ++group_id; ++group_open;              /* the step's undoing is one step on the other side */
    for (;;) {
        last = rec_last(from);
        if (!last || (unsigned)(last[2] | (last[3] << 8)) != g) break;
        static uint8_t r[256];
        memcpy(r, last, last[1]);
        from->n = (unsigned)(last - from->b);
        rec_apply(r);
    }
    --group_open;
    rec_mode = 0;
    last_kind = 0;
    return 1;
}
int pw_undo(void) { return undo_step(&undo_log, 1); }
int pw_redo(void) { return undo_step(&redo_log, 2); }

/* --------------------------------------------------------------- the clipboard */
static struct { int type; uint8_t flags; float p[PW_MAX_PARAM]; uint8_t data[PW_DATA_MAX]; } clip = {-1, 0, {0}, {0}};
int pw_copy(unsigned slot)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1) return -1;
    clip.type = pws.mod[slot].type; clip.flags = pws.mod[slot].flags;
    memcpy(clip.p, pws.mod[slot].p, sizeof clip.p);
    memcpy(clip.data, pws.mod[slot].data, PW_DATA_MAX);
    return 0;
}
int pw_clip_type(void) { return clip.type; }
int pw_paste(int x, int y)
{
    if (clip.type < 0) return -1;
    int s = pw_add((unsigned)clip.type, x, y);
    if (s < 0) return -1;
    uint8_t was = rec_on; rec_on = 0;
    pw_module *m = &pws.mod[s];
    for (unsigned p = 0; p < pw_types[clip.type].n_param; ++p) m->p[p] = clip.p[p];
    memcpy(m->data, clip.data, PW_DATA_MAX);
    if (clip.type == PW_T_PARAMMOD) for (unsigned c = 0; c < 4; ++c) m->p[4 * c] = 0;   /* (its targets were other modules' places) */
    for (unsigned v = 0; v < PW_VARS; ++v) var_put(m, v);
    if (clip.flags) pw_set_flags((unsigned)s, clip.flags);
    rec_on = was;
    pws.revision++;
    return s;
}
void pw_set_bpm(float bpm)
{
    pws.bpm = pw_clamp(bpm, 30.0f, 300.0f);
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i)
        if (pws.mod[i].used == 1 && pws.mod[i].type == PW_T_CLOCK && pws.mod[i].p[0] < 0.5f) pws.mod[i].p[1] = pws.bpm;
    pws.revision++;
}
void pw_set_playing(int on)
{
    if (on && !pws.playing) pws.start_pending = 1;
    pws.playing = on ? 1 : 0;
    pws.revision++;
}
void pw_set_scope(unsigned slot, unsigned port) { pws.scope_port = (uint8_t)port; pws.scope_mod = (uint8_t)slot; }

/* -------------------------------------------------------------- keyboard */
void pw_key(int note, int velocity)
{
    uint8_t w = pws.kq_w, next = (uint8_t)((w + 1u) & 31u);
    if (next == pws.kq_r) return; /* full: drop */
    pws.kq[w][0] = (uint8_t)note; pws.kq[w][1] = (uint8_t)(velocity < 0 ? 0 : velocity > 127 ? 127 : velocity);
    pws.kq_w = next;
}
void pw_all_notes_off(void) { pw_key(255, 0); }
void pw_pressure(int value) { pws.kb_press = (float)(value < 0 ? 0 : value > 127 ? 127 : value) * (1.0f / 127.0f); }
/* MIDI in (control side): notes and bend to the keys, the mod wheel, the sustain pedal (CC 64), all notes off; any
 * CC kept for MIDI IN modules and moving the performance page's knob it is mapped to; program change 0-7 picks a
 * variation; clock (24 a beat), start, stop and continue drive the transport when they come. */
static uint8_t sus_held[128];
void pw_midi(uint8_t status, uint8_t d1, uint8_t d2)
{
    if (status >= 0xF8) {
        uint32_t now = pw_plat_millis();
        switch (status) {
        case 0xF8:
            if (pws.clk_ms && now - pws.clk_ms < 250u) {
                uint32_t us = (now - pws.clk_ms) * 1000u;
                pws.clk_avg_us = pws.clk_avg_us ? (pws.clk_avg_us * 7u + us) / 8u : us;
                float bpm = 60000000.0f / (24.0f * (float)pws.clk_avg_us);
                if (bpm >= 30.0f && bpm <= 300.0f && pw_absf(bpm - pws.bpm) > 0.5f) pws.bpm = (float)pw_floori(bpm + 0.5f);
            }
            pws.clk_ms = now; pws.ext_sync = 1;
            pws.clk_q = (uint8_t)(pws.clk_q + 1u);
            break;
        case 0xFA: pws.clk_count = 0; pws.clk_q = 0; pws.ext_sync = 1; pws.clk_ms = now; pw_set_playing(1); break;
        case 0xFB: pws.ext_sync = 1; pws.playing = 1; pws.revision++; break;
        case 0xFC: pw_set_playing(0); break;
        default: break;
        }
        return;
    }
    switch (status & 0xf0) {
    case 0x90:
        if (d2 && pws.sustain) sus_held[d1 & 127] = 0;
        if (!d2 && pws.sustain) { sus_held[d1 & 127] = 1; break; }
        pw_key(d1 & 127, d2); break;
    case 0x80:
        if (pws.sustain) { sus_held[d1 & 127] = 1; break; }
        pw_key(d1 & 127, 0); break;
    case 0xb0:
        pws.cc[d1 & 127] = d2; pws.cc_ch[d1 & 127] = status & 15; pws.last_cc = d1 & 127; pws.cc_serial++;
        if (d1 == 1) pws.kb_mod = d2 / 127.0f;
        else if (d1 == 64) {
            pws.sustain = d2 >= 64;
            if (!pws.sustain) for (int n = 0; n < 128; ++n) if (sus_held[n]) { sus_held[n] = 0; pw_key(n, 0); }
        }
        else if (d1 == 120 || d1 == 123) pw_all_notes_off();
        for (unsigned k = 0; k < PW_PERF; ++k) {
            if (pws.perf_cc[k] != d1 || pws.perf[k][0] == 0xff || pws.mod[pws.perf[k][0]].used != 1) continue;
            const pw_param_desc *d = &pw_types[pws.mod[pws.perf[k][0]].type].param[pws.perf[k][1]];
            float u = (float)d2 / 127.0f, v = d->kind == PW_EXP && d->min > 0 ? d->min * pw_exp2(u * pw_log2(d->max / d->min)) : d->min + u * (d->max - d->min);
            pw_set_param(pws.perf[k][0], pws.perf[k][1], v);
        }
        break;
    case 0xd0: pw_pressure(d1); break;                 /* channel pressure */
    case 0xa0: pw_pressure(d2); break;                 /* poly pressure: the last key's */
    case 0xc0: if (pws.pc_patch) pws.pc_req = (uint8_t)(d1 + 1u); else if (d1 < PW_VARS) pw_var_select(d1); break;
    case 0xe0: pws.kb_bend = ((float)((d2 << 7) | d1) - 8192.0f) / 8192.0f; break;
    default: break;
    }
}
static PW_DSP void keyboard_events(void)
{
    while (pws.kq_r != pws.kq_w) {
        uint8_t note = pws.kq[pws.kq_r][0], vel = pws.kq[pws.kq_r][1];
        pws.kq_r = (uint8_t)((pws.kq_r + 1u) & 31u);
        if (note == 255) { pws.n_held = 0; pws.kb_gate = 0; for (unsigned v = 0; v < PW_MAX_VOICES; ++v) pws.voice[v].gate = 0; continue; }
        if (pws.voices > 1) {                /* the voices: a new note takes a free one (the longest free), else the oldest */
            if (vel) {
                int pick = -1;
                for (unsigned v = 0; v < pws.voices && pick < 0; ++v) if (pws.voice[v].note == note) pick = (int)v;
                for (unsigned v = 0; v < pws.voices && pick < 0; ++v)
                    if (!pws.voice[v].gate && (pick < 0 || pws.voice[v].age < pws.voice[pick].age)) pick = (int)v;
                if (pick < 0) { pick = 0; for (unsigned v = 1; v < pws.voices; ++v) if (pws.voice[v].age < pws.voice[pick].age) pick = (int)v; }
                pws.voice[pick].note = note; pws.voice[pick].vel = vel; pws.voice[pick].gate = 1;
                pws.voice[pick].serial++; pws.voice[pick].age = ++pws.voice_clock;
            } else for (unsigned v = 0; v < pws.voices; ++v) if (pws.voice[v].note == note) pws.voice[v].gate = 0;
        }
        unsigned j = 0;
        for (unsigned i = 0; i < pws.n_held; ++i)
            if (pws.held[i] != note) { pws.held[j] = pws.held[i]; pws.held_vel[j] = pws.held_vel[i]; ++j; }
        pws.n_held = (uint8_t)j;
        if (vel) {
            if (pws.n_held >= 16) {
                for (unsigned i = 1; i < 16; ++i) { pws.held[i - 1] = pws.held[i]; pws.held_vel[i - 1] = pws.held_vel[i]; }
                pws.n_held = 15;
            }
            pws.held[pws.n_held] = note; pws.held_vel[pws.n_held] = vel; ++pws.n_held;
            pws.kb_pitch_note = note; pws.kb_vel = vel / 127.0f; pws.kb_gate = 1; pws.kb_serial++;
            pws.last_note = (int8_t)note;
        } else if (pws.n_held) {
            pws.kb_pitch_note = pws.held[pws.n_held - 1];
        } else pws.kb_gate = 0;
    }
}

/* ----------------------------------------------------------------- render */
static PW_DSP uint8_t peak_level(const float *x, uint8_t old)
{
    float peak = 0;
    for (int i = 0; i < PW_N; i += 4) { float a = x[i] < 0 ? -x[i] : x[i]; if (a > peak) peak = a; }   /* (every fourth sample: a meter does not need them all, and this ran on every jack of every module) */
    float v = peak * (255.0f / 5.0f);
    if (v > 255.0f) v = 255.0f;
    uint8_t now = (uint8_t)v, decay = (uint8_t)(old - (old >> 2) - (old ? 2 : 0));   /* (two blocks have passed since the last look) */
    return now > decay ? now : decay;
}
/* ---- AUDITION: a sound source struck on its own, to hear what it makes before (or without) patching it. The engine feeds
 * the module a trigger or gate (and a pitch) as if a cable brought them, and if nothing is cabled from its output, mixes
 * it into the speaker for a moment. */
typedef struct { uint8_t type, trig, hold, pitch, out0, out1; } aud_def;   /* trig/pitch: the input (255: none); hold: a gate held (else a pulse, then it rings); out1 255: the same on both sides */
static const aud_def aud_defs[] = {
    {PW_T_VCO, 255, 1, 0, 2, 255}, {PW_T_NOISE, 255, 1, 255, 0, 255}, {PW_T_WAVETABLE, 255, 1, 0, 0, 255},
    {PW_T_COMPLEX, 255, 1, 0, 1, 255}, {PW_T_FORMANT, 255, 1, 0, 0, 255}, {PW_T_SAMPLER, 1, 1, 0, 0, 255},
    {PW_T_FM4, 1, 1, 0, 0, 255}, {PW_T_FMDX, 1, 1, 0, 0, 255}, {PW_T_CZ, 1, 1, 0, 0, 255}, {PW_T_DRUMS, 0, 0, 255, 0, 255}, {PW_T_STRING, 0, 0, 1, 0, 255},
    {PW_T_DRUMRACK, 0, 0, 255, 0, 1}, {PW_T_ADDITIVE, 1, 1, 0, 0, 1}, {PW_T_MACRO, 1, 1, 0, 0, 255},
    {PW_T_MICROPERC, 0, 0, 1, 0, 255},
};
static const aud_def *aud_find(unsigned type)
{
    for (unsigned i = 0; i < sizeof aud_defs / sizeof aud_defs[0]; ++i) if (aud_defs[i].type == type) return &aud_defs[i];
    return 0;
}
int pw_audition_ok(unsigned type) { return aud_find(type) != 0; }
void pw_audition(unsigned slot, int note, unsigned pad)
{
    if (slot >= PW_MAX_MODULES || pws.mod[slot].used != 1 || !aud_find(pws.mod[slot].type)) return;
    const aud_def *d = aud_find(pws.mod[slot].type);
    uint8_t mix = 1;
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) if (pws.cable[c].used && pws.cable[c].sm == slot) mix = 0;
    pw_plat_lock();
    pws.aud_mod = (uint8_t)slot; pws.aud_note = (int8_t)(note < 0 ? 0 : note > 127 ? 127 : note - 60 < -128 ? -128 : note - 60); pws.aud_pad = (uint8_t)(pad & 7u);
    pws.aud_mix = mix; pws.aud_age = 0;
    pws.aud_gate = d->hold ? (uint32_t)(0.45f * PW_SRF) : 64u;
    pws.aud_window = d->trig == 255 ? pws.aud_gate : (uint32_t)((d->hold ? 1.4f : 1.5f) * PW_SRF);
    pws.aud_on = 1;
    pw_plat_unlock();
}
static float aud_in_buf[2][PW_NMAX];   /* [0] the trigger input, [1] the pitch input */
/* DIAGNOSTICS' CPU page: while it is open (pw_prof_on) each module's proc is timed with the platform's tick counter, summed over its voices and blocks */
uint32_t pw_prof_tk[PW_MAX_MODULES] PW_POOL, pw_prof_blocks, pw_prof_all;
uint8_t pw_prof_on;
static PW_DSP void pw_render_inner(float *left, float *right);
static unsigned run_voice;   /* the voice area's current voice (0 outside it) */

/* The second core (EXPERIMENTAL, DIAGNOSTICS > CPU). With pw_cpu1_offload on (1: effects, 2: effects, filters and sources), the heavy modules whose code touches only their own
 * memory (the table off_ok) run on CPU1, each one block later than on one core: a module's block is posted at its turn (its inputs copied) and its result is taken at its turn
 * in the next block. A module is first timed on this core for OFF_PROBE blocks; if it costs OFF_MIN_US or more and CPU1 has room (OFF_BUDGET_US of work a block), it moves.
 * Nothing moves back and forth while playing (a move is a one-block jump in that module's sound); a change of the switch starts over. Anything that goes wrong (CPU1 does not
 * answer in 6 ms) holds CPU1 and every module is this core's again (pw_off_state 3). */
#define OFF_PROBE 8
#define OFF_MIN_US 25
#define OFF_BUDGET_US 2000
#define OFF_BUFS 20
uint8_t pw_cpu1_offload;            /* 0 off, 1 effects, 2 effects, filters, sources (the CPU page's SEL) */
uint8_t pw_off_state;               /* 0 idle, 1 modules run on CPU1, 3 CPU1 failed (everything is CPU0's) */
uint32_t pw_off_us, pw_off_n, pw_off_probing, pw_off_wait_us, pw_off_copy_us;   /* CPU1's work a block (us), modules on CPU1, modules still being timed; what the main core spent waiting for CPU1 and copying blocks, last block, us */
static uint32_t off_wait_tk, off_copy_tk;
static float off_mem[OFF_BUFS][PW_NMAX] PW_POOL;
static float off_dump[PW_NMAX];     /* where CPU1 writes an output nobody uses */
static uint32_t off_free = (1u << OFF_BUFS) - 1u;
static uint8_t off_seen;
typedef struct {
    uint8_t mode, type, n_in, n_out, n;   /* mode 0 timing, 3 timed (waiting for the others), 1 on CPU1, 2 stays here */
    uint16_t cmask, omask;                /* the inputs that had a cable and the outputs that were patched while timing; later, the ones CPU1 has buffers for */
    uint32_t sum, ticket, us;
    const float *inp[6]; float *outp[4];
    int8_t bin[6], bout[4];
} off_st;
static off_st off[PW_MAX_MODULES];
static const uint8_t off_ok[] = {PW_T_VCO, PW_T_NOISE, PW_T_WAVETABLE, PW_T_COMPLEX, PW_T_VCF, PW_T_VCA, PW_T_FOLDER, PW_T_HARMTREM, PW_T_DIST, PW_T_DELAY, PW_T_REVERB, PW_T_RING,
    PW_T_LPG, PW_T_SWIRL, PW_T_FM4, PW_T_DRUMS, PW_T_STRING, PW_T_VOWEL, PW_T_COMP, PW_T_FORMANT, PW_T_COMB, PW_T_EQ, PW_T_PSHIFT, PW_T_TAPE, PW_T_ADDITIVE, PW_T_OTT, PW_T_SPACE,
    PW_T_ROOM, PW_T_GENDY};
static uint64_t pm_cur;   /* PARAM MOD: the modules whose knobs it moved last block (this block's are collected as the PARAM MODs run) */
static int off_eligible(const pw_module *m, unsigned slot)
{
    if (!pw_cpu1_offload || pw_off_state == 3 || run_voice || (m->flags & PW_F_BYPASS) || m->status != PW_STATUS_OK) return 0;
    if (pws.voices > 1 && (m->flags & PW_F_POLY)) return 0;
    if (pws.aud_on && slot == pws.aud_mod) return 0;
    if ((pm_cur >> slot) & 1u) return 0;   /* (PARAM MOD moves its knobs on this core) */
    unsigned cat = pw_types[m->type].cat;
    if (cat != PW_C_FX && (pw_cpu1_offload < 2 || (cat != PW_C_FILTER && cat != PW_C_SOURCE))) return 0;
    for (unsigned k = 0; k < sizeof off_ok; ++k) if (off_ok[k] == m->type) return 1;
    return 0;
}
/* (a loop of words the compiler cannot turn into the library's memcpy, which on this chip copies about 30 bytes a microsecond) */
static PW_DSP void off_cp(float *d, const float *s, int n)
{
    for (int i = 0; i + 4 <= n; i += 4) {
        float a = s[i], b = s[i + 1], c = s[i + 2], e = s[i + 3];
        __asm__ volatile("" ::: "memory");
        d[i] = a; d[i + 1] = b; d[i + 2] = c; d[i + 3] = e;
    }
}
static void off_give_back(off_st *st)
{
    for (unsigned k = 0; k < st->n_in; ++k) if (st->bin[k] >= 0) off_free |= 1u << st->bin[k];
    for (unsigned k = 0; k < st->n_out; ++k) if (st->bout[k] >= 0) off_free |= 1u << st->bout[k];
}
static void off_release(off_st *st)   /* CPU1 is done with the module (its last block waited for), its buffers are free */
{
    if (st->mode == 1) {
        if (st->ticket) pw_plat_cpu1_wait(st->ticket, 0);
        off_give_back(st);
    }
    st->mode = 0; st->n = 0; st->sum = 0; st->ticket = 0; st->cmask = st->omask = 0;
}
static void off_fail(void)   /* CPU1 did not answer: hold it, every module is this core's again */
{
    pw_plat_cpu1_stop();
    pw_off_state = 3; pw_cpu1_offload = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) { if (off[i].mode == 1) off_give_back(&off[i]); off[i].mode = 2; off[i].ticket = 0; }
}
static unsigned off_pop(unsigned x) { unsigned n = 0; while (x) { n += x & 1u; x >>= 1; } return n; }
static void off_assign(void)   /* every module that was being timed is timed: the costliest go to CPU1 first, while it has room and buffers */
{
    uint32_t mine = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (off[i].mode == 1) mine += off[i].us;
    for (;;) {
        int best = -1; uint32_t bu = 0;
        for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (off[i].mode == 3) { uint32_t u = off[i].sum / off[i].n; if (best < 0 || u > bu) { best = (int)i; bu = u; } }
        if (best < 0) break;
        off_st *st = &off[best];
        uint32_t us = bu / pw_plat_ticks_per_us();
        unsigned ni = st->n_in, no = st->n_out, ci = off_pop(st->cmask & ((1u << ni) - 1u)), co = off_pop(st->omask & ((1u << no) - 1u));
        st->mode = 2;
        if (us >= OFF_MIN_US + 2u * (ci + co) && mine + us + us / 5 <= OFF_BUDGET_US && ci + co <= OFF_BUFS) {
            unsigned need = ci + co, got = 0; int8_t bi[10];
            for (unsigned b = 0; b < OFF_BUFS && got < need; ++b) if (off_free & (1u << b)) bi[got++] = (int8_t)b;
            if (got == need) {
                unsigned g = 0;
                for (unsigned k = 0; k < 6; ++k) st->bin[k] = -1;
                for (unsigned k = 0; k < 4; ++k) st->bout[k] = -1;
                for (unsigned k = 0; k < ni; ++k) if (st->cmask & (1u << k)) st->bin[k] = bi[g++];
                for (unsigned k = 0; k < no; ++k) if (st->omask & (1u << k)) st->bout[k] = bi[g++];
                for (unsigned k = 0; k < got; ++k) { off_free &= ~(1u << bi[k]); memset(off_mem[(uint8_t)bi[k]], 0, sizeof off_mem[0]); }
                st->mode = 1; st->ticket = 0; st->us = us; mine += us;
            }
        }
    }
}
static void off_check(void)   /* at the start of each block: a changed switch starts over; a module that is gone or no longer fit is released (CPU1 finishes with it first) */
{
    if (pw_cpu1_offload != off_seen) {
        for (unsigned i = 0; i < PW_MAX_MODULES; ++i) { if (off[i].mode == 1) off_release(&off[i]); off[i].mode = 0; off[i].n = 0; off[i].sum = 0; off[i].cmask = off[i].omask = 0; }
        off_seen = pw_cpu1_offload;
        if (pw_off_state != 3) pw_off_state = 0;
    }
    uint32_t us = 0, n = 0, pr = 0, timed = 0;
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        off_st *st = &off[i];
        const pw_module *m = &pws.mod[i];
        if (st->mode && st->type != m->type) off_release(st);
        if (st->mode == 1 && (m->used != 1 || !off_eligible(m, i))) off_release(st);
        if (st->mode == 1) { us += st->us; ++n; }
        else if (st->mode == 3) ++timed;
        else if (st->mode == 0 && pw_cpu1_offload && m->used == 1 && off_eligible(m, i)) ++pr;
    }
    if (!pr && timed) off_assign();
    pw_off_us = us; pw_off_n = n; pw_off_probing = pr;
    pw_off_wait_us = off_wait_tk / pw_plat_ticks_per_us(); pw_off_copy_us = off_copy_tk / pw_plat_ticks_per_us(); off_wait_tk = off_copy_tk = 0;
    if (pw_off_state != 3) pw_off_state = n ? 1 : 0;
}
unsigned pw_off_reason(unsigned slot, unsigned *us)   /* for the diagnostics file: why a module is or is not on CPU1 (0 offload off, 1 CPU1 failed, 2 on CPU1, 3 timed, 4 stays: ports, 5 stays: cheap or no room, 6 bypassed, 7 status, 8 voice area, 9 audition, 10 category, 11 type, 12 timing it) */
{
    const off_st *st = &off[slot];
    const pw_module *m = &pws.mod[slot];
    unsigned tpu = pw_plat_ticks_per_us();
    *us = st->mode == 1 ? st->us : (st->n && tpu) ? st->sum / st->n / tpu : 0u;
    if (!pw_cpu1_offload) return 0;
    if (pw_off_state == 3) return 1;
    if (st->mode) return st->mode == 1 ? 2u : st->mode == 3 ? 3u : st->n_in > 6 || st->n_out > 4 ? 4u : 5u;
    if (m->flags & PW_F_BYPASS) return 6;
    if (m->status != PW_STATUS_OK) return 7;
    if (pws.voices > 1 && (m->flags & PW_F_POLY)) return 8;
    if (pws.aud_on && slot == pws.aud_mod) return 9;
    return off_eligible(m, slot) ? 12u : (pw_types[m->type].cat != PW_C_FX && pw_cpu1_offload < 2) ? 10u : 11u;
}
static int off_try(pw_module *m, unsigned slot, const float *const *in, float *const *out, unsigned connected)
{
    off_st *st = &off[slot];
    if (st->mode == 1 && ((pm_cur >> slot) & 1u)) off_release(st);   /* (a PARAM MOD began moving its knobs: back to this core) */
    if (!off_eligible(m, slot)) return 0;
    const pw_type *t = &pw_types[m->type];
    unsigned nmask = (1u << t->n_out) - 1u, om = m->omask & nmask;
    if (st->mode == 2 || st->mode == 3) return 0;
    if (st->mode == 0) {   /* timing it here before deciding */
        uint32_t t0 = pw_plat_ticks();
        t->proc(m, in, out, connected);
        uint32_t dt = pw_plat_ticks() - t0;
        st->sum += dt; st->type = m->type; st->n_in = t->n_in; st->n_out = t->n_out;
        st->cmask |= (uint16_t)connected; st->omask |= (uint16_t)om;
        if (pw_prof_on) pw_prof_tk[slot] += dt;
        if (++st->n >= OFF_PROBE) st->mode = (t->n_in <= 6 && t->n_out <= 4) ? 3 : 2;
        return 1;
    }
    /* on CPU1. A cable plugged in that it has no buffer for: start over (that block is this core's) */
    if ((connected & ~(unsigned)st->cmask) || (om & ~(unsigned)st->omask)) { off_release(st); return 0; }
    /* take last block's result, send this block's inputs */
    uint32_t w0 = pw_plat_ticks();
    if (st->ticket) {
        uint32_t us = 0;
        if (pw_plat_cpu1_wait(st->ticket, &us)) { off_fail(); return 0; }
        st->us = us;
        off_wait_tk += pw_plat_ticks() - w0;
        w0 = pw_plat_ticks();
        for (unsigned o = 0; o < t->n_out; ++o) if (st->bout[o] >= 0) off_cp(out[o], off_mem[(uint8_t)st->bout[o]], (int)PW_N);
    } else {
        w0 = pw_plat_ticks();
        for (unsigned o = 0; o < t->n_out; ++o) if (st->bout[o] >= 0) memset(out[o], 0, sizeof(float) * PW_N);
    }
    for (unsigned i = 0; i < t->n_in; ++i) {
        if (st->bin[i] >= 0 && ((connected >> i) & 1u)) { off_cp(off_mem[(uint8_t)st->bin[i]], in[i], (int)PW_N); st->inp[i] = off_mem[(uint8_t)st->bin[i]]; }
        else st->inp[i] = zero_buf;
    }
    for (unsigned o = 0; o < t->n_out; ++o) st->outp[o] = st->bout[o] >= 0 ? off_mem[(uint8_t)st->bout[o]] : off_dump;
    off_copy_tk += pw_plat_ticks() - w0;
    st->ticket = pw_plat_cpu1_post((pw_cpu1_fn)t->proc, m, st->inp, st->outp, connected);
    if (!st->ticket) { off_fail(); return 1; }
    return 1;
}

unsigned pw_cur_voice(void) { return run_voice; }
/* before a module runs: the audition's trigger / gate and pitch added to what its jacks have */
static PW_DSP void aud_feed(unsigned slot, const float **in, unsigned *connected)
{
    const aud_def *d = aud_find(pws.mod[slot].type);
    if (!d) return;
    unsigned on = pws.aud_gate > pws.aud_age ? (unsigned)(pws.aud_gate - pws.aud_age < (unsigned)PW_N ? pws.aud_gate - pws.aud_age : (unsigned)PW_N) : 0;
    unsigned ti = d->type == PW_T_DRUMRACK ? pws.aud_pad : d->trig;
    if (ti != 255) {
        float *b = aud_in_buf[0];
        for (unsigned k = 0; k < (unsigned)PW_N; ++k) b[k] = in[ti][k] + (k < on ? 5.0f : 0.0f);
        in[ti] = b; *connected |= 1u << ti;
    }
    if (d->pitch != 255) {
        float *b = aud_in_buf[1], v = (float)pws.aud_note * (1.0f / 12.0f);
        for (unsigned k = 0; k < (unsigned)PW_N; ++k) b[k] = in[d->pitch][k] + v;
        in[d->pitch] = b; *connected |= 1u << d->pitch;
    }
}
/* after it ran: its output into the speaker (when nothing is cabled from it), a short fade at the ends of an oscillator's note */
static PW_DSP void aud_mixout(unsigned slot, float *const *out)
{
    const aud_def *d = aud_find(pws.mod[slot].type);
    if (!d || !pws.aud_mix) return;
    const float *a = out[d->out0], *b = out[d->out1 == 255 ? d->out0 : d->out1];
    for (int k = 0; k < PW_N; ++k) {
        uint32_t age = pws.aud_age + (uint32_t)k;
        float g = 1.0f;
        if (d->trig == 255) { g = age < 220u ? (float)age / 220.0f : 1.0f; if (age + 2200u > pws.aud_gate) g *= pw_clamp((float)((int32_t)pws.aud_gate - (int32_t)age) / 2200.0f, 0.0f, 1.0f); }
        else if (age + 2000u > pws.aud_window) g = pw_clamp((float)((int32_t)pws.aud_window - (int32_t)age) / 2000.0f, 0.0f, 1.0f);
        pw_mix_l[k] += a[k] * g * 0.8f; pw_mix_r[k] += b[k] * g * 0.8f;
    }
}
static PW_DSP void run_module(const pw_plan *p, unsigned slot)
{
    const float *in[PW_MAX_IN];
    float *out[PW_MAX_OUT] = {0};
    pw_module *m = &pws.mod[slot];
    const pw_type *t = &pw_types[m->type];
    unsigned connected = 0;
    for (unsigned i = 0; i < t->n_in; ++i) {
        unsigned count = p->in_count[slot][i], first = p->in_first[slot][i];
        if (!count) in[i] = zero_buf;
        else if (count == 1 && p->gain[first] == 100) { in[i] = outbuf[p->src[first]]; connected |= 1u << i; }
        else {   /* several cables summed, or one at another amount */
            float *s = sum_buf[i];
            const float *a = outbuf[p->src[first]];
            float g = (float)p->gain[first] * 0.01f;
            for (int k = 0; k < PW_N; ++k) s[k] = a[k] * g;
            for (unsigned c = 1; c < count; ++c) {
                const float *b = outbuf[p->src[first + c]];
                float gc = (float)p->gain[first + c] * 0.01f;
                for (int k = 0; k < PW_N; ++k) s[k] += b[k] * gc;
            }
            in[i] = s; connected |= 1u << i;
        }
    }
    for (unsigned i = t->n_in; i < PW_MAX_IN; ++i) in[i] = zero_buf;
    for (unsigned o = 0; o < t->n_out; ++o) out[o] = outbuf[m->outbuf[o]];
    int aud = pws.aud_on && slot == pws.aud_mod && !(m->flags & PW_F_BYPASS) && run_voice == 0;
    m->omask = (uint16_t)(aud || (pws.aud_on && slot == pws.aud_mod) ? 0xFFFFu : p->omask[slot] | (pws.scope_mod == slot ? 1u << (pws.scope_port & 15u) : 0u));
    if (aud) aud_feed(slot, in, &connected);
    pw_pm_undo pmu[PW_PM_MAX];
    unsigned pmn = 0;
    int pm_step = 0;
    if ((pm_cur >> slot) & 1u) {
        pm_step = !(m->flags & PW_F_BYPASS) && PW_N >= 32 && !(PW_N & 15) && pw_pm_steppable(m->type) && !aud;
        if (!pm_step) pmn = pw_pm_apply(slot, pmu, 0, (unsigned)PW_N);
    }
    if (pm_step) {   /* a knob moved by PARAM MOD: the module runs sixteen samples at a time, each with the knob where the voltage has it */
        const float *cin[PW_MAX_IN];
        float *cout[PW_MAX_OUT] = {0};
        int full = pw_blk;
        for (int c0 = 0; c0 < full; c0 += 16) {
            for (unsigned i = 0; i < t->n_in; ++i) cin[i] = in[i] + c0;
            for (unsigned i = t->n_in; i < PW_MAX_IN; ++i) cin[i] = zero_buf;
            for (unsigned o = 0; o < t->n_out; ++o) cout[o] = out[o] + c0;
            unsigned n = pw_pm_apply(slot, pmu, (unsigned)c0, 16u);
            pw_blk = 16;
            t->proc(m, cin, cout, connected);
            pw_blk = full;
            if (n) pw_pm_restore(slot, pmu, n);
        }
    } else if (m->flags & PW_F_BYPASS) {   /* bypassed: the sound in goes straight out (L, R: the left and right in); else silence */
        int a[2] = {-1, -1}, na = 0;
        for (unsigned i = 0; i < t->n_in && na < 2; ++i) if (pw_audio_jack(t->in[i])) a[na++] = (int)i;
        for (unsigned o = 0; o < t->n_out; ++o) {
            int src = o == 0 ? a[0] : (o == 1 && t->out[1][0] == 'R' && !t->out[1][1]) ? (a[1] >= 0 ? a[1] : a[0]) : -1;
            for (int k = 0; k < PW_N; ++k) out[o][k] = src >= 0 ? in[src][k] : 0.0f;
        }
    } else if (off_try(m, slot, in, out, connected)) {   /* (on the second core: its result is last block's) */
    } else if (pw_prof_on) {
        uint32_t t0 = pw_plat_ticks();
        t->proc(m, in, out, connected);
        pw_prof_tk[slot] += pw_plat_ticks() - t0;
    } else t->proc(m, in, out, connected);
    if (pmn) pw_pm_restore(slot, pmu, pmn);
    if (aud) aud_mixout(slot, out);
    int meter = ((pws.blocks + slot) & 1u) == 0;   /* the meters look at every second block (the screen is drawn far less often), each module on its own turn */
    if (meter) for (unsigned o = 0; o < t->n_out; ++o) m->level[o] = peak_level(out[o], m->level[o]);
    if (m->type == PW_T_CLOCK) {   /* pulses counted on CLK, DIV, MULT and WARP (for the page's bars), and which are high */
        static const uint8_t cp[4] = {0, 1, 2, 4};
        for (unsigned k = 0; k < 4; ++k)
            for (int i = 0; i < PW_N; ++i) {
                unsigned hi = out[cp[k]][i] > 2.5f;
                if (hi && !((m->edge_hi >> k) & 1u)) ++m->edge[k];
                m->edge_hi = (uint8_t)((m->edge_hi & ~(1u << k)) | (hi << k));
            }
    }
    if (m->type == PW_T_OUTPUT || m->type == PW_T_FRACTURE || m->type == PW_T_VCA || m->type == PW_T_VCF || m->type == PW_T_STEREO || m->type == PW_T_MIX8)
        if (meter) for (unsigned i = 0; i < t->n_in; ++i) m->in_level[i] = peak_level(in[i], m->in_level[i]);
}
static PW_DSP void render_block(float *left, float *right)
{
    pm_cur = pw_pm_mask; pw_pm_mask = 0;
    uint32_t idx = pws.plan_cur & 1u;
    pws.plan_ack = idx;
    const pw_plan *p = &pws.plan[idx];
    off_check();
    keyboard_events();
    /* Transport: global sixteenth-note ticks while playing. */
    pws.reset_now = 0;
    for (int i = 0; i < PW_N; ++i) pws.tick_at[i] = 0;
    if (pws.start_pending) { pws.start_pending = 0; pws.reset_now = 1; pws.tick_phase = 1.0f; pws.sixteenths = pws.start_s16; pws.start_s16 = 0; }
    if (pws.ext_sync && pws.clk_ms && pw_plat_millis() - pws.clk_ms > 600u) pws.ext_sync = 0;   /* the clock went away */
    if (pws.ext_sync) {
        /* following a MIDI clock: a sixteenth every six clocks, at the start of the block they came in */
        uint8_t q = pws.clk_q; pws.clk_q = 0;
        int at = 0;
        while (q--) {
            if (pws.playing && pws.clk_count == 0 && at < PW_N) pws.tick_at[at] = 1, at += 8;
            pws.clk_count = (uint8_t)((pws.clk_count + 1u) % 6u);
        }
    } else if (pws.playing) {
        float inc = pws.bpm * (4.0f / 60.0f) * PW_INV_SR, ph = pws.tick_phase;
        for (int i = 0; i < PW_N; ++i) {
            if (ph >= 1.0f) { ph -= 1.0f; pws.tick_at[i] = 1; }
            ph += inc;
        }
        pws.tick_phase = ph;
    }
    if (pws.clk_out && !pws.ext_sync) {   /* MIDI clock out (a setting): start with the first sixteenth, six clocks to each, stop with the transport */
        if (pws.reset_now) pw_plat_midi_out(0xFA, 0, 0);
        if (pws.cko_play && !pws.playing) pw_plat_midi_out(0xFC, 0, 0);
        if (pws.playing) {
            int32_t gap = (int32_t)(PW_SRF * 60.0f / (pw_maxf(pws.bpm, 20.0f) * 24.0f));
            for (int i = 0; i < PW_N; ++i) {
                if (pws.tick_at[i]) { pws.cko_next = 0; pws.cko_count = 0; }
                if (pws.cko_next >= 0 && pws.cko_next-- == 0) {
                    pw_plat_midi_out(0xF8, 0, 0);
                    pws.cko_next = ++pws.cko_count < 6u ? gap : -1;
                }
            }
        }
    }
    pws.cko_play = pws.playing;
    for (int i = 0; i < PW_N; ++i) if (pws.tick_at[i]) pws.sixteenths++;
    for (int i = 0; i < PW_N; ++i) { pw_mix_l[i] = 0; pw_mix_r[i] = 0; }
    int poly = pws.voices > 1 && p->poly_count;
    unsigned voices = poly ? pws.voices : 1;
    for (unsigned n = 0; n < p->count; ++n) {
        if (!poly || n != p->poly_first) { run_module(p, p->order[n]); continue; }
        /* the voice area, once per voice: each its keys, state and memory; what leaves it summed */
        float kp = pws.kb_pitch_note, kv = pws.kb_vel; uint8_t kg = pws.kb_gate; uint32_t ks = pws.kb_serial;
        for (unsigned v = 0; v < voices; ++v) {
            run_voice = v;
            pws.kb_pitch_note = (float)pws.voice[v].note; pws.kb_vel = (float)pws.voice[v].vel * (1.0f / 127.0f);
            pws.kb_gate = pws.voice[v].gate; pws.kb_serial = pws.voice[v].serial;
            for (unsigned j = 0; j < p->poly_count; ++j) {
                pw_module *m = &pws.mod[p->order[p->poly_first + j]];
                if (m->nv > v && v) { m->stp = voice_state(m, v); m->mem_off = m->reg_off + m->audio_len * v; }
                run_module(p, p->order[p->poly_first + j]);
                m->stp = m->st.raw; m->mem_off = m->reg_off;
            }
            for (unsigned b = 0; b < p->n_bound; ++b) {
                float *acc = voice_acc[b];
                const float *x = outbuf[p->bound[b]];
                if (!v) for (int i = 0; i < PW_N; ++i) acc[i] = x[i];
                else for (int i = 0; i < PW_N; ++i) acc[i] += x[i];
            }
        }
        for (unsigned b = 0; b < p->n_bound; ++b) memcpy(outbuf[p->bound[b]], voice_acc[b], sizeof voice_acc[b]);
        pws.kb_pitch_note = kp; pws.kb_vel = kv; pws.kb_gate = kg; pws.kb_serial = ks;
        run_voice = 0;
        n += p->poly_count - 1u;
    }
    /* Scope tap */
    if (pws.scope_mod < PW_MAX_MODULES && pws.mod[pws.scope_mod].used == 1 &&
        pws.scope_port < pws.mod[pws.scope_mod].n_out) {
        const float *src = outbuf[pws.mod[pws.scope_mod].outbuf[pws.scope_port]];
        unsigned w = pws.scope_w;
        for (int i = 0; i < PW_N; ++i) { float q = src[i] * 3000.0f; pws.scope[w] = (int16_t)(q > 32767.0f ? 32767.0f : q < -32767.0f ? -32767.0f : q); w = (w + 1u) & (PW_SCOPE - 1); }
        pws.scope_w = (uint16_t)w;
    }
    /* Metronome: a short click on each beat (a higher one on the bar's first), mixed in before the master; never in a patch or its meters' way */
    if (pws.metro && pws.playing) {
        static float mc_ph, mc_env, mc_f;
        int after = 0;
        unsigned s16[PW_N];
        for (int i = PW_N - 1; i >= 0; --i) { s16[i] = pws.sixteenths - (unsigned)after; if (pws.tick_at[i]) ++after; }
        for (int i = 0; i < PW_N; ++i) {
            if (pws.tick_at[i] && ((s16[i] - 1u) & 3u) == 0u) { mc_env = 1.0f; mc_ph = 0; mc_f = ((s16[i] - 1u) & 15u) == 0u ? 1760.0f : 1175.0f; }
            if (mc_env > 0.001f) {
                float x = pw_sin1(mc_ph) * mc_env * 2.2f;
                mc_ph += mc_f * PW_INV_SR; if (mc_ph >= 1.0f) mc_ph -= 1.0f;
                mc_env *= 0.9985f;
                pw_mix_l[i] += x; pw_mix_r[i] += x;
            }
        }
    }
    /* Master: volts to full scale. With CLIP PROTECT off a gentle bend above 0.89 (about -1 dBFS) keeps it from clipping hard;
     * with it on there is no bend: the brickwall limiter after the block does it, by gain reduction, and the signal below its
     * ceiling goes through untouched. */
    {
        int prot = 0, outs = 0;
        for (unsigned i = 0; i < PW_MAX_MODULES; ++i)
            if (pws.mod[i].used == 1 && pws.mod[i].type == PW_T_OUTPUT) { ++outs; if (pws.mod[i].p[1] >= 0.5f) prot = 1; }
        pws.clip_on = (uint8_t)(!outs || prot);   /* (no OUTPUT module: the audition's sound is protected too) */
    }
    for (int i = 0; i < PW_N; ++i) {
        float l = pw_mix_l[i] * 0.2f, r = pw_mix_r[i] * 0.2f;
        if (!(l == l)) l = 0;
        if (!(r == r)) r = 0;
        if (pws.clip_on) {   /* (only the grossest: the limiter must not be handed infinity) */
            l = pw_clamp(l, -16.0f, 16.0f); r = pw_clamp(r, -16.0f, 16.0f);
        } else {
            if (l > 0.89f || l < -0.89f) l = (l > 0 ? 0.89f : -0.89f) + 0.11f * pw_tanh((l - (l > 0 ? 0.89f : -0.89f)) * 9.0f);
            if (r > 0.89f || r < -0.89f) r = (r > 0 ? 0.89f : -0.89f) + 0.11f * pw_tanh((r - (r > 0 ? 0.89f : -0.89f)) * 9.0f);
        }
        left[i] = l; right[i] = r;
    }
    pws.kb_trig = 0;
    if (pws.aud_on) { pws.aud_age += PW_N; if (pws.aud_age >= pws.aud_window) pws.aud_on = 0; }
    pws.blocks++;
}


/* ------------------------------------------------------------ CLIP PROTECT
 * A lightweight brickwall peak limiter on the final stereo output (after every module, the mix and OUTPUT's LEVEL; before the
 * MASTER knob, which only turns it down, so the speaker, the headphones and the USB audio all get the limited signal).
 *   ceiling -0.5 dBFS; lookahead 88 frames (2 ms at 44.1 kHz); release automatic, 50 ms for a shallow reduction to 100 ms for
 *   a deep one; hard knee, ratio infinite; one gain for both channels (the image stays put); no make-up gain, no oversampling.
 * How it keeps the promise: a frame that would pass the ceiling asks for a gain r = ceiling / peak. The smallest ask within the
 * last 89 frames is held (e), released slowly (env) and then averaged over the same 89 frames (g): the average of values that
 * are each no larger than the ask of the frame being played means g is never larger than that ask, and the average makes the
 * gain slide down over the 2 ms before the peak instead of stepping. Below the ceiling every ask is 1, g is exactly 1 and the
 * output is the input, 2 ms later. Switching it on or off crossfades over one block (no click). */
#define LIM_N 88
#define LIM_W (LIM_N + 1)
#define LIM_CEIL 0.94406f                      /* -0.5 dBFS */
#define LIM_RC_FAST (1.0f / (0.050f * 44100.0f))
#define LIM_RC_SLOW (1.0f / (0.100f * 44100.0f))
static float lim_dl[2][LIM_N];                 /* the audio, LIM_N frames late */
static uint32_t lim_dq_i[LIM_W]; static float lim_dq_v[LIM_W];   /* the smallest asks of the window (a monotonic queue: frame number, ask) */
static unsigned lim_dq_h, lim_dq_n;
static float lim_av[LIM_W], lim_sum, lim_env = 1.0f;   /* the held, released ask, and its last 89 values with their sum */
static unsigned lim_ai, lim_below, lim_rs, lim_w;
static uint32_t lim_t;
static float lim_mix;                          /* 0: bypassed (the input as it is), 1: limited; moves a block's length */
static uint8_t lim_cold = 1;                   /* the analysis is not up to date (it has not been running): prime it before use */
static float lim_step(float r)                 /* one frame's ask in; the gain to play it with out */
{
    /* the smallest ask in the last LIM_W frames, this one included */
    while (lim_dq_n && lim_t - lim_dq_i[lim_dq_h] >= (uint32_t)LIM_W) { lim_dq_h = (lim_dq_h + 1u) % LIM_W; --lim_dq_n; }
    while (lim_dq_n && lim_dq_v[(lim_dq_h + lim_dq_n - 1u) % LIM_W] >= r) --lim_dq_n;
    lim_dq_i[(lim_dq_h + lim_dq_n) % LIM_W] = lim_t; lim_dq_v[(lim_dq_h + lim_dq_n) % LIM_W] = r; ++lim_dq_n;
    ++lim_t;
    float e = lim_dq_v[lim_dq_h];
    /* down at once (it is already LIM_N frames ahead of the audio); up slowly: faster for a shallow reduction, slower for a deep one */
    if (e < lim_env) lim_env = e;
    else {
        float deep = pw_clamp((1.0f - lim_env) * 2.0f, 0.0f, 1.0f);
        lim_env += (e - lim_env) * (LIM_RC_FAST - (LIM_RC_FAST - LIM_RC_SLOW) * deep);
        if (lim_env > 0.9999f) lim_env = 1.0f;
    }
    /* the average of the last LIM_W values (a running sum; made exact whenever none of them is below 1, and every 1024 frames) */
    float old = lim_av[lim_ai]; lim_av[lim_ai] = lim_env;
    lim_ai = lim_ai + 1u == LIM_W ? 0u : lim_ai + 1u;
    lim_below = lim_below - (old < 1.0f) + (lim_env < 1.0f);
    lim_sum += lim_env - old;
    if (!lim_below) lim_sum = (float)LIM_W;
    else if (++lim_rs >= 1024u) { lim_rs = 0; lim_sum = 0; for (unsigned k = 0; k < LIM_W; ++k) lim_sum += lim_av[k]; }
    float g = lim_sum * (1.0f / (float)LIM_W);
    return g > 1.0f ? 1.0f : g;
}
static void lim_prime(void)   /* the frames in the delay line, oldest first, go through the analysis (nothing is played) */
{
    lim_dq_h = 0; lim_dq_n = 0; lim_env = 1.0f; lim_sum = (float)LIM_W; lim_below = 0; lim_rs = 0;
    for (unsigned k = 0; k < LIM_W; ++k) lim_av[k] = 1.0f;
    lim_ai = 0;
    for (unsigned k = 0; k < LIM_N; ++k) {
        unsigned at = (lim_w + k) % LIM_N;
        float pk = pw_maxf(pw_absf(lim_dl[0][at]), pw_absf(lim_dl[1][at]));
        (void)lim_step(pk > LIM_CEIL ? LIM_CEIL / pk : 1.0f);
    }
    lim_cold = 0;
}
static void clip_protect(float *left, float *right)   /* PW_NMAX frames, in place */
{
    int on = pws.clip_on;
    float step = 1.0f / (float)PW_NMAX, gmin = 1.0f;
    if (on && lim_cold) lim_prime();
    for (int i = 0; i < PW_NMAX; ++i) {
        float xl = left[i], xr = right[i];
        float dl = lim_dl[0][lim_w], dr = lim_dl[1][lim_w];   /* what went in LIM_N frames ago */
        lim_dl[0][lim_w] = xl; lim_dl[1][lim_w] = xr;
        lim_w = lim_w + 1u == LIM_N ? 0u : lim_w + 1u;
        if (on) lim_mix = lim_mix + step > 1.0f ? 1.0f : lim_mix + step; else lim_mix = lim_mix - step < 0.0f ? 0.0f : lim_mix - step;
        if (lim_mix == 0.0f) continue;                    /* bypassed: the signal as it is */
        float pk = pw_maxf(pw_absf(xl), pw_absf(xr));
        float g = lim_step(pk > LIM_CEIL ? LIM_CEIL / pk : 1.0f);
        if (g < gmin) gmin = g;
        float wl = pw_clamp(dl * g, -LIM_CEIL, LIM_CEIL), wr = pw_clamp(dr * g, -LIM_CEIL, LIM_CEIL);   /* (the clamp only if rounding left a hair over) */
        left[i] = xl + (wl - xl) * lim_mix; right[i] = xr + (wr - xr) * lim_mix;
    }
    if (!on && lim_mix == 0.0f) lim_cold = 1;
    /* what the display shows: the deepest reduction, held a little, then it lets go */
    static unsigned hold;
    if (!on) { pws.clip_gr = 1.0f; hold = 0; }
    else if (gmin < pws.clip_gr) { pws.clip_gr = gmin; hold = 80; }
    else if (hold) --hold;
    else { pws.clip_gr += (1.0f - pws.clip_gr) * 0.03f; if (pws.clip_gr > 0.9995f) pws.clip_gr = 1.0f; }
}
float pw_clip_gr_db(void)
{
    if (!pws.clip_on || pws.clip_gr >= 0.9995f) return 0.0f;
    return 6.0206f * pw_log2(pws.clip_gr);
}
int pw_clip_gr_tenths(void)   /* the same in tenths of a dB, 0 below a tenth */
{
    int q = (int)(pw_clip_gr_db() * 10.0f - 0.5f);
    return q > -1 ? 0 : q;
}
static void meter_out(const float *left, const float *right)
{
    float pl = 0, pr = 0;
    for (int i = 0; i < PW_NMAX; ++i) { if (pw_absf(left[i]) > pl) pl = pw_absf(left[i]); if (pw_absf(right[i]) > pr) pr = pw_absf(right[i]); }
    pws.out_peak_l = pw_maxf(pl, pws.out_peak_l * 0.92f);
    pws.out_peak_r = pw_maxf(pr, pws.out_peak_r * 0.92f);
}

/* The engine block to the platform's 128 frames: at 22.05 kHz the engine renders 64 frames and each pair of output frames is
 * a frame and a point between it and the next (a four-point interpolation: the images above 11 kHz are well down; two frames late). */
PW_DSP void pw_render(float *left, float *right)
{
    uint32_t prof_t0 = pw_prof_on ? pw_plat_ticks() : 0;
    pw_render_inner(left, right);
    if (pw_prof_on) { pw_prof_all += pw_plat_ticks() - prof_t0; ++pw_prof_blocks; }
}
static PW_DSP void pw_render_inner(float *left, float *right)
{
    pw_sea_block();
    if (rate_want != rate_half) {
        rate_half = rate_want;
        pw_sr = rate_half ? 22050 : 44100; pw_srf = (float)pw_sr; pw_isr = 1.0f / pw_srf; pw_blk = rate_half ? PW_NMAX / 2 : PW_NMAX;
        for (int c = 0; c < 2; ++c) for (int k = 0; k < 3; ++k) up_h[c][k] = 0;
    }
    if (!rate_half) { render_block(left, right); clip_protect(left, right); meter_out(left, right); return; }
    static float t[2][PW_NMAX / 2];
    render_block(t[0], t[1]);
    for (int c = 0; c < 2; ++c) {
        float *o = c ? right : left, a = up_h[c][0], b = up_h[c][1], d = up_h[c][2];
        for (int i = 0; i < PW_NMAX / 2; ++i) {
            float x = t[c][i];   /* a, b, d: the three before it */
            o[2 * i] = b;
            o[2 * i + 1] = (9.0f * (b + d) - a - x) * (1.0f / 16.0f);
            a = b; b = d; d = x;
        }
        up_h[c][0] = a; up_h[c][1] = b; up_h[c][2] = d;
    }
    clip_protect(left, right); meter_out(left, right);   /* (after the upsampling, whose interpolation can overshoot a little) */
}

/* ------------------------------------------------------------ formatting */
static char *put_str(char *o, const char *s) { while (*s) *o++ = *s++; *o = 0; return o; }
static char *put_int(char *o, int v)
{
    char tmp[12]; int n = 0;
    if (v < 0) { *o++ = '-'; v = -v; }
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n) *o++ = tmp[--n];
    *o = 0;
    return o;
}
PW_COLD static char *put_fixed(char *o, float v, int decimals)
{
    if (v < 0) { *o++ = '-'; v = -v; }
    int scale = decimals == 2 ? 100 : decimals == 1 ? 10 : 1;
    int q = (int)(v * (float)scale + 0.5f);
    o = put_int(o, q / scale);
    if (decimals) {
        *o++ = '.';
        int f = q % scale;
        if (decimals == 2 && f < 10) *o++ = '0';
        o = put_int(o, f);
    }
    return o;
}
PW_COLD void pw_param_text(const pw_param_desc *d, float v, char out[12])
{
    char *o = out;
    *o = 0;
    if (d->kind == PW_ENUM) {
        int i = pw_floori(v + 0.5f);
        if (i < 0) i = 0;
        if (i > (int)d->max) i = (int)d->max;
        put_str(o, d->labels[i]);
        return;
    }
    switch (d->unit) {
    case PW_U_HZ: {
        float a = pw_absf(v);
        if (a >= 1000.0f) { o = put_fixed(o, v / 1000.0f, a >= 10000.0f ? 1 : 2); put_str(o, "K"); }
        else { o = put_fixed(o, v, a < 10.0f ? 2 : a < 100.0f ? 1 : 0); put_str(o, "HZ"); }
    } break;
    case PW_U_HZV: o = put_int(o, (int)(v + 0.5f)); put_str(o, "HZ/V"); break;
    case PW_U_SEC:
        if (v < 1.0f) { o = put_fixed(o, v * 1000.0f, v < 0.01f ? 1 : 0); put_str(o, "MS"); }
        else { o = put_fixed(o, v, 2); put_str(o, "S"); }
        break;
    case PW_U_PCT: o = put_int(o, pw_floori(v * 100.0f + 0.5f)); put_str(o, "%"); break;
    case PW_U_VOLT: if (v >= 0) *o++ = '+'; o = put_fixed(o, v, 2); put_str(o, "V"); break;
    case PW_U_SEMI:
        if (d->kind == PW_INT) { o = put_int(o, pw_floori(v + 0.5f)); put_str(o, "ST"); break; }
        if (v > 0.001f) *o++ = '+';
        o = put_fixed(o, v, (pw_absf(v - (float)pw_floori(v + 0.5f)) < 0.05f) ? 0 : 1); put_str(o, "ST");
        break;
    case PW_U_CENT: if (v > 0.001f) *o++ = '+'; o = put_int(o, pw_floori(v + 0.5f)); put_str(o, "C"); break;
    case PW_U_BPM: o = put_fixed(o, v, (pw_absf(v - (float)pw_floori(v + 0.5f)) < 0.05f) ? 0 : 1); break;
    case PW_U_X: o = put_fixed(o, v, v < 10.0f ? 2 : 1); put_str(o, "X"); break;
    case PW_U_DIV: {   /* a clock rate: 1, /2, /3 ... divide; x2, x3 ... multiply */
        int n = pw_floori(v + (v < 0 ? -0.5f : 0.5f));
        if (n >= 2) { *o++ = '/'; put_int(o, n); } else if (n == 1) put_str(o, "1"); else { *o++ = 'X'; put_int(o, 2 - n); }
    } break;
    case PW_U_DB: if (v > 0.05f) *o++ = '+'; o = put_fixed(o, v, 1); put_str(o, "DB"); break;
    default:
        if (d->kind == PW_INT) { if (d->min < 0 && v > 0.5f) *o++ = '+'; put_int(o, pw_floori(v + 0.5f)); }
        else put_fixed(o, v, 2);
        break;
    }
}
/* a sound id as the DRUM RACK and GRANULAR show it: I3 CLAP (INTERNAL), A-3 NAME .. D-8 NAME */
static void sound_label(char out[12], unsigned id)
{
    char *o = out;
    if (id >= PW_SOUNDS) { put_str(o, "-"); return; }
    if (id < 16u) { *o++ = 'I'; o = put_int(o, (int)id + 1); } else { *o++ = (char)('A' + (id - 16u) / 8u); *o++ = '-'; o = put_int(o, (int)((id - 16u) % 8u) + 1); }
    const char *name = pw_sample_name(id);
    if (name) { *o++ = ' '; for (unsigned i = 0; name[i] && o < out + 11; ++i) *o++ = name[i]; }
    *o = 0;
}
PW_COLD void pw_format_param(unsigned slot, unsigned p, char out[12])
{
    out[0] = 0;
    if (slot >= PW_MAX_MODULES || !pws.mod[slot].used) return;
    const pw_type *t = &pw_types[pws.mod[slot].type];
    if (p >= t->n_param) return;
    float v = pws.mod[slot].p[p];
    if (pws.mod[slot].type == PW_T_SAMPLER && p == 0) {     /* SAMPLE: RAM, or its number in the BANK and the sound's name: 3 TOM L */
        int b = pw_floori(v + 0.5f);
        char *o = out;
        int bank = pw_floori(pws.mod[slot].p[25] + 0.5f);
        if (b <= 0 && bank > 0) b = 1;   /* (RAM is the INTERNAL bank's: A-D start at 1) */
        if (b <= 0) { put_str(o, "RAM"); return; }
        const char *name = b > 8 ? 0 : pw_sample_name(pw_snd_id(bank, b));
        o = put_int(o, b);
        *o++ = name ? ' ' : '-'; *o = 0;
        for (unsigned i = 0; name && name[i] && o < out + 11; ++i) *o++ = name[i];
        *o = 0;
        return;
    }
    if (pws.mod[slot].type == PW_T_PARAMMOD && (p & 3u) < 2u) {   /* a channel's MODULE ("VCF 1", or "-") and PARAM (the knob's name) */
        int md = pw_floori(v + 0.5f);
        if ((p & 3u) == 0u) { if (md >= 1 && md <= PW_MAX_MODULES && pws.mod[md - 1].used == 1) pw_module_tag_label((unsigned)md - 1u, out); else put_str(out, "-"); return; }
        int tg = pw_pm_target(slot, p >> 2);
        const char *nm = tg >= 0 && (unsigned)md < pw_types[pws.mod[tg].type].n_param ? pw_types[pws.mod[tg].type].param[md].name : 0;
        char *o = out;
        for (unsigned i = 0; nm && nm[i] && o < out + 11; ++i) *o++ = nm[i];
        if (!nm) *o++ = '-';
        *o = 0;
        return;
    }
    if (pws.mod[slot].type == PW_T_GRAIN && p == 8 && v > 0.5f) { sound_label(out, (unsigned)pw_floori(v + 0.5f) - 1u); return; }   /* SOURCE: a bank sound */
    const int is_dx = pws.mod[slot].type == PW_T_FMDX;
    if ((pws.mod[slot].type == PW_T_CZ || is_dx) && p == 0) {          /* CZ, FM-DX: the bank's name: INTERNAL, then the .syx banks by their file names */
        int b = pw_floori(v + 0.5f);
        const char *nm = b == 0 ? "INTERNAL" : pw_plat_sample_name((is_dx ? PW_DX_SLOT : PW_CZ_SLOT) + (unsigned)b - 1u);
        if (b > 0 && !(is_dx ? pw_dx_voices((unsigned)b) : pw_cz_tones((unsigned)b))) nm = b == (int)pw_clamp(pws.mod[slot].p[0] + 0.5f, 0.0f, 4.0f) && pw_cz_embedded(slot) ? "SAVED" : "EMPTY";
        char *o = out;
        for (unsigned i = 0; nm && nm[i] && o < out + 11; ++i) *o++ = nm[i];
        *o = 0;
        return;
    }
    if ((pws.mod[slot].type == PW_T_CZ || is_dx) && p == 1) {          /* CZ, FM-DX: the preset's number and name, and a * when a knob has changed it: 12 STRINGS 2* */
        char *o = out;
        int k = pw_floori(v + 0.5f), star = pw_cz_edited(slot);
        const uint8_t *nm = pw_cz_embedded_name(slot);   /* (the tone the patch carried, else the bank's) */
        if (!nm) nm = is_dx ? pw_dx_voice_name((unsigned)pw_clamp(pws.mod[slot].p[0] + 0.5f, 0.0f, 4.0f), (unsigned)(k < 1 ? 1 : k))
                            : pw_cz_tone_name((unsigned)pw_clamp(pws.mod[slot].p[0] + 0.5f, 0.0f, 4.0f), (unsigned)(k < 1 ? 1 : k));
        o = put_int(o, k);
        if (nm) {
            unsigned a = 0, z = 16;
            while (a < z && nm[a] == ' ') ++a;
            while (z > a && nm[z - 1u] == ' ') --z;
            *o++ = ' ';
            for (; a < z && o < out + (star ? 10 : 11); ++a) *o++ = (nm[a] >= 32 && nm[a] < 127) ? (char)nm[a] : '?';
        }
        if (star) *o++ = '*';
        *o = 0;
        return;
    }
    if (pws.mod[slot].type == PW_T_DRUMRACK && p >= 4 && (p & 3) == 0 && p <= 32 && v >= 7.5f && v < 39.5f) { sound_label(out, (unsigned)pw_floori(v + 0.5f) + 8u); return; }   /* a bank sound: A-3 and its name (kit voices are named by the list) */
    if ((pws.mod[slot].type == PW_T_VOWEL && p == 0) || (pws.mod[slot].type == PW_T_FORMANT && p == 1)) {   /* VOWEL: the letters it is between */
        static const char L[5] = {'A', 'E', 'I', 'O', 'U'};
        int a = pw_floori(v), pct = (int)((v - (float)a) * 100.0f + 0.5f);
        if (a >= 4 || pct < 8) { out[0] = L[a > 4 ? 4 : a]; out[1] = 0; }
        else if (pct > 92) { out[0] = L[a + 1]; out[1] = 0; }
        else { out[0] = L[a]; out[1] = '>'; out[2] = L[a + 1]; out[3] = 0; }
        return;
    }
    pw_param_text(&t->param[p], v, out);
}
int pw_module_index(unsigned slot)
{
    if (slot >= PW_MAX_MODULES || !pws.mod[slot].used) return 0;
    int n = 0;
    for (unsigned i = 0; i <= slot; ++i) n += pws.mod[i].used == 1 && pws.mod[i].type == pws.mod[slot].type;
    return n;
}
const char *pw_module_preset_name(unsigned slot)   /* the preset a module is on ("CLAP", "E.PIANO"), 0 when it has none or is on "-" */
{
    if (slot >= PW_MAX_MODULES || !pws.mod[slot].used) return 0;
    const pw_module *m = &pws.mod[slot];
    int pp = m->type == PW_T_DRUMRACK ? -1 : pw_preset_param(m->type);
    if (pp < 0) return 0;
    int k = pw_floori(m->p[pp] + 0.5f);
    const pw_param_desc *d = &pw_types[m->type].param[pp];
    return d->labels && k > 0 && k <= (int)d->max && d->labels[k] ? d->labels[k] : 0;   /* (no labels: the CZ's TONE is a number into its bank) */
}
PW_COLD static void label_of(unsigned slot, char out[12], int with_preset)
{
    out[0] = 0;
    if (slot >= PW_MAX_MODULES || !pws.mod[slot].used) return;
    const char *name = pw_types[pws.mod[slot].type].tag, *pn = with_preset ? pw_module_preset_name(slot) : 0;
    if (pn) name = pn;   /* a module on a preset says which: "CLAP 2", "E.PIANO 1" (the number still tells two of a kind apart) */
    int idx = pw_module_index(slot), digits = idx >= 10 ? 2 : 1, room = 10 - digits;   /* (11 letters at most, with the number) */
    char *o = out;
    for (int i = 0; name[i] && i < room; ++i) *o++ = name[i];
    *o++ = ' ';
    put_int(o, idx);
}
void pw_module_label(unsigned slot, char out[12]) { label_of(slot, out, 1); }
void pw_module_tag_label(unsigned slot, char out[12]) { label_of(slot, out, 0); }   /* "MODAL 1", whatever its preset */

/* --------------------------------------------------------- serialization */
uint32_t pw_crc32(uint32_t crc, const void *data, unsigned n)
{
    const uint8_t *b = (const uint8_t *)data;
    while (n--) {
        crc ^= *b++;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}
static void put16(uint8_t *b, unsigned v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static unsigned get16(const uint8_t *b) { return b[0] | (b[1] << 8); }
static void putf(uint8_t *b, float f) { union { float f; uint32_t u; } x = { f }; for (int i = 0; i < 4; ++i) b[i] = (uint8_t)(x.u >> (8 * i)); }
static float getf(const uint8_t *b) { union { float f; uint32_t u; } x; x.u = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); return x.f; }

#define PW_MAGIC "NDL1"
/* Version 2: each module's flags after its data; after the cables the voices, the variation playing, the
 * performance page, then for each module and each other variation what differs from the one playing (as many
 * as fit: pw_save_trimmed says when some did not). */
int pw_save_trimmed;
uint8_t pw_save_kind;
PW_COLD int pw_save(uint8_t *buf, unsigned cap)
{
    uint8_t map[PW_MAX_MODULES], emb_slot[PW_MAX_MODULES];
    uint16_t poff[PW_MAX_MODULES];   /* where each module's knobs are in the file (PARAM MOD's targets are written as the file's module numbers) */
    unsigned n = 0, at = 24, cables = 0, nemb = 0;
    if (cap < 64) return -1;
    pw_save_trimmed = 0;
    {   /* the CZ tones the patch carries (they close the patch, before its checksum: 1 + 144 bytes each) */
        uint8_t probe[144];
        for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if (pws.mod[i].used == 1 && pw_cz_embed(i, probe)) emb_slot[nemb++] = (uint8_t)i;
        if (cap < 64u + nemb * 145u) return -1;
        cap -= nemb * 145u;
    }
    memset(map, 0xff, sizeof map);
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        const pw_module *m = &pws.mod[i];
        if (m->used != 1) continue;
        const pw_type *t = &pw_types[m->type];
        unsigned data = pw_has_data(m->type) ? pw_data_len(m->type) : 0;
        unsigned need = 8u + t->n_param * 4u + data;
        if (at + need + 8 > cap) return -1;
        map[i] = (uint8_t)n++;
        buf[at++] = m->type;
        put16(buf + at, (uint16_t)m->x); put16(buf + at + 2, (uint16_t)m->y); at += 4;
        buf[at++] = t->n_param;
        poff[i] = (uint16_t)at;
        for (unsigned p = 0; p < t->n_param; ++p) { putf(buf + at, m->p[p]); at += 4; }
        buf[at++] = (uint8_t)data;
        memcpy(buf + at, m->data, data); at += data;
        buf[at++] = (uint8_t)(m->flags | (m->group << 2));   /* (the group rides in the flags' upper bits) */
    }
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i)
        if (pws.mod[i].used == 1 && pws.mod[i].type == PW_T_PARAMMOD)
            for (unsigned c = 0; c < 4; ++c) {
                unsigned md = (unsigned)(pws.mod[i].p[4 * c] + 0.5f);
                putf(buf + poff[i] + 16u * c, md >= 1 && md <= PW_MAX_MODULES && map[md - 1] != 0xff ? (float)(map[md - 1] + 1u) : 0.0f);
            }
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (!k->used || map[k->sm] == 0xff || map[k->dm] == 0xff) continue;
        if (at + 5 + 4 > cap) return -1;
        buf[at++] = map[k->sm]; buf[at++] = k->so; buf[at++] = map[k->dm]; buf[at++] = k->di; buf[at++] = (uint8_t)k->amt;
        ++cables;
    }
    if (at + 2 + 3 * PW_PERF + 3 + 4 * PW_SONG_CLIPS + PW_SUBLANES + 1 + 3 * PW_LANES + 3 * PW_LANE_PTS + 2 + 8 * PW_VARS + 8 > cap) return -1;
    buf[at++] = pws.voices; buf[at++] = pws.var_cur;
    for (unsigned k = 0; k < PW_PERF; ++k) {
        unsigned sl = pws.perf[k][0];
        buf[at++] = sl < PW_MAX_MODULES && map[sl] != 0xff ? map[sl] : 0xff; buf[at++] = pws.perf[k][1];
    }
    for (unsigned k = 0; k < PW_PERF; ++k) buf[at++] = pws.perf_cc[k];
    {   /* the song (version 9): on, glide, the clips, the ramps; then which modules the other snapshots bypass */
        unsigned nc = 0;
        buf[at++] = pws.song.on; buf[at++] = pws.song.glide;
        unsigned cat = at++;
        for (unsigned k = 0; k < PW_SONG_CLIPS; ++k) {
            const pw_clip *c = &pws.song.clip[k];
            if (!c->len) continue;
            buf[at++] = c->lane; buf[at++] = c->start; buf[at++] = c->len; buf[at++] = c->val; ++nc;
        }
        buf[cat] = (uint8_t)nc;
        for (unsigned k = 0; k < PW_SUBLANES; ++k) buf[at++] = pws.song.sub[k];   /* (kept in the file: version 11 had a subpatch behind each clip lane; no lane is played any more) */
        buf[at++] = pws.song.n_lanes;
        for (unsigned l = 0, s = 0; l < pws.song.n_lanes; ++l) {
            const pw_alane *u = &pws.song.al[l];
            buf[at++] = u->mod < PW_MAX_MODULES && map[u->mod] != 0xff ? map[u->mod] : 0xff; buf[at++] = u->par; buf[at++] = u->n;
            for (unsigned i = 0; i < u->n; ++i, ++s) { buf[at++] = (uint8_t)pws.song.pt_t[s]; buf[at++] = (uint8_t)(pws.song.pt_t[s] >> 8); buf[at++] = pws.song.pt_v[s]; }
        }
        unsigned has = pws.byp_has & ~(1u << pws.var_cur);
        buf[at++] = (uint8_t)has; buf[at++] = (uint8_t)(has >> 8);
        for (unsigned v = 0; v < PW_VARS; ++v) {
            if (!(has >> v & 1u)) continue;
            uint64_t o = 0;
            for (unsigned i = 0; i < PW_MAX_MODULES; ++i) if ((pws.byp[v] >> i & 1u) && map[i] != 0xff) o |= (uint64_t)1 << map[i];
            for (unsigned k = 0; k < 8; ++k) buf[at++] = (uint8_t)(o >> (8 * k));
        }
        {   /* (version 13: the modules MUTATE leaves alone) */
            uint64_t o = 0;
            for (unsigned i = 0; i < PW_MAX_MODULES && i < 64u; ++i) if ((pws.mut_lock >> i & 1u) && map[i] != 0xff) o |= (uint64_t)1 << map[i];
            for (unsigned k = 0; k < 8; ++k) buf[at++] = (uint8_t)(o >> (8 * k));
        }
    }
    /* the other variations, as differences */
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i) {
        pw_module *m = &pws.mod[i];
        if (m->used != 1) continue;
        const pw_type *t = &pw_types[m->type];
        for (unsigned v = 0; v < PW_VARS; ++v) {
            if (v == pws.var_cur) continue;
            unsigned cnt = at, nd = 0;
            if (at + 2 + 4 > cap) { pw_save_trimmed = 1; break; }
            buf[at++] = 0;
            for (unsigned p = 0; p < t->n_param && m->reg_len; ++p) {
                uint16_t q = (uint16_t)(var_store(m)[(v * t->n_param + p) * 2] | (var_store(m)[(v * t->n_param + p) * 2 + 1] << 8));
                if (q == quant(&t->param[p], m->p[p])) continue;
                if (at + 3 + 2 + 4 > cap) { pw_save_trimmed = 1; break; }
                buf[at++] = (uint8_t)p; put16(buf + at, q); at += 2; ++nd;
            }
            buf[cnt] = (uint8_t)nd;
            const unsigned dl = pw_data_len(m->type);
            const uint8_t *d = m->reg_len && pw_has_data(m->type) ? var_store(m) + PW_VARS * t->n_param * 2u + v * dl : 0;
            if (d && memcmp(d, m->data, dl) && at + 1 + dl + 4 <= cap) {
                buf[at++] = 1; memcpy(buf + at, d, dl); at += dl;
            } else { if (d && memcmp(d, m->data, dl)) pw_save_trimmed = 1; if (at + 1 + 4 > cap) return -1; buf[at++] = 0; }
        }
    }
    for (unsigned k = 0; k < nemb; ++k) {
        buf[at++] = map[emb_slot[k]];
        pw_cz_embed(emb_slot[k], buf + at); at += 144;
    }
    memcpy(buf, PW_MAGIC, 4);
    buf[4] = 13; buf[5] = (uint8_t)n; buf[6] = (uint8_t)cables; buf[7] = (uint8_t)nemb;
    put16(buf + 8, (unsigned)(pws.bpm * 10.0f + 0.5f) | (pw_save_kind ? 0x8000u : 0u));   /* (the top bit: saved as a GROUP, to be inserted, not loaded) */
    memset(buf + 10, ' ', 12);
    for (unsigned i = 0; i < 12 && pws.name[i]; ++i) buf[10 + i] = (uint8_t)pws.name[i];
    put16(buf + 22, at + 4);
    uint32_t crc = ~pw_crc32(~0u, buf, at);
    for (int i = 0; i < 4; ++i) buf[at + i] = (uint8_t)(crc >> (8 * i));
    return (int)(at + 4);
}
/* A saved patch put into the patch that is open, as one new group: its modules (OUTPUT left out), their knobs and cables, at a free place;
 * `set` gets the slots it made. Returns how many, or < 0 (not a patch of this version, or no room). */
PW_COLD int pw_insert(const uint8_t *buf, unsigned len, uint64_t *set)
{
    uint16_t poff[PW_MAX_MODULES], doff[PW_MAX_MODULES];
    uint8_t ty[PW_MAX_MODULES], np[PW_MAX_MODULES], dl[PW_MAX_MODULES], fl[PW_MAX_MODULES];
    int16_t px[PW_MAX_MODULES], py[PW_MAX_MODULES];
    int8_t map[PW_MAX_MODULES];
    *set = 0;
    if (len < 28 || memcmp(buf, PW_MAGIC, 4) || buf[4] < 9 || buf[4] > 13) return -1;
    unsigned total = get16(buf + 22);
    if (total > len || total < 28) return -1;
    uint32_t crc = ~pw_crc32(~0u, buf, total - 4);
    if (crc != (buf[total - 4] | (buf[total - 3] << 8) | (buf[total - 2] << 16) | ((uint32_t)buf[total - 1] << 24))) return -2;
    unsigned n = buf[5], cables = buf[6], at = 24, end = total - 4, nemb = buf[7];
    if (n > PW_MAX_MODULES) return -1;
    if (nemb * 145u > end - 24u) nemb = 0;
    end -= nemb * 145u;
    int minx = 32767, miny = 32767, count = 0;
    for (unsigned i = 0; i < n; ++i) {
        map[i] = -1;
        if (at + 7 > end) return -3;
        ty[i] = buf[at++]; px[i] = (int16_t)get16(buf + at); py[i] = (int16_t)get16(buf + at + 2); at += 4;
        np[i] = buf[at++]; poff[i] = (uint16_t)at;
        if (at + np[i] * 4u + 1u > end) return -3;
        at += np[i] * 4u;
        dl[i] = buf[at++]; doff[i] = (uint16_t)at;
        if (dl[i] > PW_DATA_MAX || at + dl[i] + 1u > end) return -3;
        at += dl[i]; fl[i] = buf[at++];
        if (ty[i] < PW_T_COUNT && ty[i] != PW_T_OUTPUT && pw_types[ty[i]].proc) { ++count; if (px[i] < minx) minx = px[i]; if (py[i] < miny) miny = py[i]; }
    }
    if (!count) return -1;
    int ox = minx, oy = miny, found = 0;   /* a free place for the whole shape (else where it was saved) */
    for (int row = 0; row < 12 && !found; ++row)
        for (int col = 0; col < 8 && !found; ++col) {
            int cx0 = 24 + col * 204, cy0 = 24 + row * 268, clash = 0;
            for (unsigned i = 0; i < n && !clash; ++i) {
                if (ty[i] >= PW_T_COUNT || ty[i] == PW_T_OUTPUT) continue;
                int cx = cx0 + px[i] - minx, cy = cy0 + py[i] - miny;
                for (unsigned j = 0; j < PW_MAX_MODULES && !clash; ++j)
                    if (pws.mod[j].used == 1 && pws.mod[j].x > cx - 150 && pws.mod[j].x < cx + 150 && pws.mod[j].y > cy - 200 && pws.mod[j].y < cy + 200) clash = 1;
            }
            if (!clash) { ox = cx0; oy = cy0; found = 1; }
        }
    lock();
    int made = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (ty[i] >= PW_T_COUNT || ty[i] == PW_T_OUTPUT || !pw_types[ty[i]].proc) continue;
        int slot = pw_add(ty[i], ox + px[i] - minx, oy + py[i] - miny);
        if (slot < 0) continue;
        map[i] = (int8_t)slot; ++made; *set |= (uint64_t)1 << slot;
        for (unsigned p = 0; p < np[i]; ++p) pw_set_param((unsigned)slot, p, getf(buf + poff[i] + 4u * p));
        memcpy(pws.mod[slot].data, buf + doff[i], dl[i]);
        pw_set_flags((unsigned)slot, fl[i] & 3u);
    }
    for (unsigned c = 0; c < cables && at + 5u <= end; ++c, at += 5u) {
        if (buf[at] >= n || buf[at + 2] >= n) continue;
        int sm = map[buf[at]], dm = map[buf[at + 2]];
        if (sm < 0 || dm < 0) continue;
        int k = pw_connect((unsigned)sm, buf[at + 1], (unsigned)dm, buf[at + 3]);
        if (k >= 0) { int a = (int8_t)buf[at + 4]; pws.cable[k].amt = (int8_t)(a < -100 ? -100 : a > 100 ? 100 : a); }
    }
    for (unsigned i = 0; i < n; ++i)   /* PARAM MOD's targets: the file's module numbers become the slots made */
        if (map[i] >= 0 && ty[i] == PW_T_PARAMMOD)
            for (unsigned c = 0; c < 4; ++c) {
                unsigned f = (unsigned)(pws.mod[map[i]].p[4 * c] + 0.5f);
                pws.mod[map[i]].p[4 * c] = f >= 1 && f <= n && map[f - 1] >= 0 ? (float)(map[f - 1] + 1) : 0.0f;
            }
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i)
        if ((*set >> i & 1u) && pws.mod[i].used == 1) for (unsigned v = 0; v < PW_VARS; ++v) var_put(&pws.mod[i], v);
    unlock();
    pws.revision++;
    return made;
}

PW_COLD int pw_load(const uint8_t *buf, unsigned len)
{
    if (len < 28 || memcmp(buf, PW_MAGIC, 4) || buf[4] < 9 || buf[4] > 13) return -1;
    unsigned version = buf[4];
    unsigned total = get16(buf + 22);
    if (total > len || total < 28) return -1;
    uint32_t crc = ~pw_crc32(~0u, buf, total - 4);
    uint32_t stored = buf[total - 4] | (buf[total - 3] << 8) | (buf[total - 2] << 16) | ((uint32_t)buf[total - 1] << 24);
    if (crc != stored) return -2;
    unsigned n = buf[5], cables = buf[6], at = 24, end = total - 4, nemb = buf[7], tail = end;
    if (nemb * 145u > end - 24u) nemb = 0;
    end -= nemb * 145u;   /* (the CZ tones the patch carries are the last bytes) */
    int8_t map[256];
    uint8_t flags[256];
    uint32_t ofm[8] = {0};   /* (bit i: module i is an old four-operator FM4, its variations not carried over) */
    lock();
    pw_clear();
    rec_on = 0;
    pw_set_playing(0);
    pws.bpm = pw_clamp((get16(buf + 8) & 0x7fffu) / 10.0f, 30.0f, 300.0f);
    unsigned name_len = 12;
    while (name_len && buf[10 + name_len - 1] == ' ') --name_len;
    for (unsigned i = 0; i < 12; ++i) {
        char ch = i < name_len ? (char)buf[10 + i] : 0;
        pws.name[i] = (ch >= 32 && ch < 127) ? ch : (i < name_len ? '?' : 0);
    }
    pws.name[12] = 0;
    int result = 0;
    for (unsigned i = 0; i < n; ++i) {
        map[i] = -1; flags[i] = 0;
        if (at + 7 > end) { result = -3; break; }
        unsigned type = buf[at++];
        int x = (int16_t)get16(buf + at), y = (int16_t)get16(buf + at + 2); at += 4;
        unsigned np = buf[at++];
        if (at + np * 4u + 1u > end) { result = -3; break; }
        int slot = type < PW_T_COUNT && pw_types[type].proc ? pw_add(type, x, y) : -1;   /* (a type that is gone, or unknown: left out) */
        map[i] = (int8_t)slot;
        if (slot < 0) result = 1; /* partially loaded: capacity, or a module this firmware does not have */
        if (slot >= 0 && type == PW_T_FM4 && np == 21u) {   /* (the four-operator FM4 of before FM-2: operator 1 is the carrier, 2 the modulator) */
            static const uint8_t from[8] = {5, 9, 1, 2, 12, 13, 3, 20};   /* RATIO INDEX FBK TUNE AMP MOD TIME PRESET */
            for (unsigned p = 0; p < 8; ++p) pw_set_param((unsigned)slot, p, p == 7 ? 0.0f : getf(buf + at + 4u * from[p]));
            ofm[i >> 5] |= 1u << (i & 31);
            at += np * 4u;
        } else
        for (unsigned p = 0; p < np; ++p, at += 4) if (slot >= 0) pw_set_param((unsigned)slot, p, getf(buf + at));
        if (slot >= 0 && type == PW_T_SAMPLER && np < 26u && pws.mod[slot].p[0] >= 1.0f) {   /* (a patch from before the BANK knob: SAMPLE 1-16 was the flash's slot, now A and B of eight) */
            int s = pw_floori(pws.mod[slot].p[0] + 0.5f) - 1;
            pw_set_param((unsigned)slot, 25, (float)(1 + s / 8)); pw_set_param((unsigned)slot, 0, (float)(1 + s % 8));
        }
        if (slot >= 0 && type == PW_T_DRUMRACK && version < 12u)   /* (before 12 a pad's SOUND was 8-23 INTERNAL, 24-55 banks A-D: the banks are 8-39 now, INTERNAL's pads are the first drum) */
            for (unsigned q = 4; q <= 32 && q < np; q += 4) { float sv = pws.mod[slot].p[q]; pw_set_param((unsigned)slot, q, sv >= 23.5f ? sv - 16.0f : sv >= 7.5f ? 0.0f : sv); }
        unsigned data = buf[at++];
        if (data > PW_DATA_MAX || at + data > end) { result = -3; break; }
        if (slot >= 0) memcpy(pws.mod[slot].data, buf + at, data);
        at += data;
        if (at + 1 > end) { result = -3; break; }
        flags[i] = buf[at++];
    }
    const unsigned cstep = 5u;   /* (each cable: from, output, to, input, amount) */
    for (unsigned c = 0; c < cables && result >= 0 && at + cstep <= end; ++c, at += cstep) {
        if (buf[at] >= n || buf[at + 2] >= n) continue;
        int sm = map[buf[at]], dm = map[buf[at + 2]];
        unsigned so = buf[at + 1], di = buf[at + 3];
        if (sm >= 0 && dm >= 0) { int k = pw_connect((unsigned)sm, so, (unsigned)dm, di); if (k >= 0) { int a = (int8_t)buf[at + 4]; pws.cable[k].amt = (int8_t)(a < -100 ? -100 : a > 100 ? 100 : a); } }
    }
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i)   /* PARAM MOD: its targets, file module numbers, become the slots they were loaded into */
        if (pws.mod[i].used == 1 && pws.mod[i].type == PW_T_PARAMMOD)
            for (unsigned c = 0; c < 4; ++c) {
                unsigned f = (unsigned)(pws.mod[i].p[4 * c] + 0.5f);
                pws.mod[i].p[4 * c] = f >= 1 && f <= n && map[f - 1] >= 0 ? (float)(map[f - 1] + 1) : 0.0f;
            }
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i)
        if (pws.mod[i].used == 1) for (unsigned v = 0; v < PW_VARS; ++v) var_put(&pws.mod[i], v);
    if (result >= 0 && at + 2 + 2 * PW_PERF <= end) {
        unsigned voices = buf[at++], cur = buf[at++];
        for (unsigned k = 0; k < PW_PERF; ++k, at += 2) {
            unsigned sl = buf[at];
            pws.perf[k][0] = sl < n && map[sl] >= 0 ? (uint8_t)map[sl] : 0xff;
            pws.perf[k][1] = pws.perf[k][0] == 0xff ? 0xff : buf[at + 1];
        }
        if (at + PW_PERF <= end) for (unsigned k = 0; k < PW_PERF; ++k) pws.perf_cc[k] = buf[at++];
        if (at + 3 <= end) {   /* the song (9: its ramps as clips on a second lane, dropped) */
            pws.song.on = buf[at] ? 1 : 0; pws.song.glide = buf[at + 1] > 8 ? 0 : buf[at + 1];
            unsigned nc = buf[at + 2]; at += 3;
            for (unsigned k = 0, o = 0; k < nc && at + 4 <= end; ++k, at += 4) {
                if (o >= PW_SONG_CLIPS || buf[at] || !buf[at + 2]) continue;
                pws.song.clip[o].lane = buf[at]; pws.song.clip[o].start = buf[at + 1]; pws.song.clip[o].len = buf[at + 2]; pws.song.clip[o].val = buf[at + 3]; ++o;
            }
            if (version >= 11 && at + PW_SUBLANES <= end) for (unsigned k = 0; k < PW_SUBLANES; ++k) pws.song.sub[k] = buf[at++] <= PW_MAX_GROUP ? buf[at - 1] : 0;
            if (version == 9) at += 4 * 8;
            else if (at < end) {
                unsigned nl = buf[at++], total = 0;
                for (unsigned l = 0; l < nl && at + 3 <= end; ++l) {
                    unsigned q = buf[at], par = buf[at + 1], np = buf[at + 2]; at += 3;
                    int lane = l < PW_LANES ? (int)l : -1;
                    unsigned mod = q < n && map[q] >= 0 ? (unsigned)map[q] : 0xffu;
                    if (lane >= 0) { pws.song.al[lane].mod = (uint8_t)mod; pws.song.al[lane].par = (uint8_t)par; pws.song.al[lane].n = 0; pws.song.n_lanes = (uint8_t)(lane + 1); }
                    for (unsigned i = 0; i < np && at + 3 <= end; ++i, at += 3) {
                        if (lane < 0 || total >= PW_LANE_PTS) continue;
                        pws.song.pt_t[total] = (uint16_t)((buf[at] | (buf[at + 1] << 8)) & 0x3fffu); pws.song.pt_v[total] = buf[at + 2];
                        ++total; ++pws.song.al[lane].n;
                    }
                }
            }
            if (at + 2 <= end) {
                unsigned has = buf[at] | (buf[at + 1] << 8); at += 2;
                for (unsigned v = 0; v < PW_VARS; ++v) {
                    if (!(has >> v & 1u) || at + 8 > end) continue;
                    uint64_t o = 0, m = 0;
                    for (unsigned k = 0; k < 8; ++k) o |= (uint64_t)buf[at + k] << (8 * k);
                    at += 8;
                    for (unsigned i = 0; i < n && i < 64; ++i) if ((o >> i & 1u) && map[i] >= 0) m |= (uint64_t)1 << map[i];
                    pws.byp[v] = m; pws.byp_has |= (uint16_t)(1u << v);
                }
            }
            if (version >= 13 && at + 8 <= end) {
                uint64_t o = 0, m = 0;
                for (unsigned k = 0; k < 8; ++k) o |= (uint64_t)buf[at + k] << (8 * k);
                at += 8;
                for (unsigned i = 0; i < n && i < 64; ++i) if ((o >> i & 1u) && map[i] >= 0) m |= (uint64_t)1 << map[i];
                pws.mut_lock = m;
            }
        }
        pws.var_cur = cur < PW_VARS ? (uint8_t)cur : 0;
        for (unsigned i = 0; i < n; ++i)
            if (map[i] >= 0) { if (flags[i] & 3u) pws.mod[map[i]].flags = (uint8_t)(flags[i] & 3u); pws.mod[map[i]].group = (uint8_t)(flags[i] >> 2); }
        pws.voices = 1;
        if (voices > 1) { pws.voices = (uint8_t)(voices > PW_MAX_VOICES ? PW_MAX_VOICES : voices); poly_realloc(); }
        for (unsigned i = 0; i < n && at < end; ++i)
            for (unsigned v = 0; v < PW_VARS; ++v) {
                if (v == pws.var_cur) continue;
                if (at + 1 > end) break;
                unsigned nd = buf[at++];
                pw_module *m = map[i] >= 0 ? &pws.mod[map[i]] : 0;
                const pw_type *t = m ? &pw_types[m->type] : 0;
                for (unsigned d = 0; d < nd && at + 3 <= end; ++d, at += 3) {
                    unsigned p = buf[at];
                    if (m && m->reg_len && p < t->n_param && !(ofm[i >> 5] >> (i & 31) & 1u)) { var_store(m)[(v * t->n_param + p) * 2] = buf[at + 1]; var_store(m)[(v * t->n_param + p) * 2 + 1] = buf[at + 2]; }
                }
                if (at + 1 > end) break;
                const unsigned vd = m ? pw_data_len(m->type) : PW_DATA_BYTES;
                if (buf[at++] && at + vd <= end) {
                    if (m && m->reg_len && pw_has_data(m->type)) {
                        uint8_t *d = var_store(m) + PW_VARS * t->n_param * 2u + v * vd;
                        memset(d, 0, vd); memcpy(d, buf + at, vd);
                    }
                    at += vd;
                }
            }
    }
    for (unsigned k = 0; k < nemb && result >= 0; ++k) {   /* (BANK and PRESET are set: the tone the patch carried plays, its bank or not) */
        unsigned q = buf[end + k * 145u];
        if (q < n && map[q] >= 0) pw_cz_embed_set((unsigned)map[q], buf + end + k * 145u + 1u);
    }
    (void)tail;
    pws.plan_ack = pws.plan_cur;
    rec_on = 1;
    pw_undo_reset();
    unlock();
    pws.revision++;
    return result;
}

#include "pw_sea.inc"
