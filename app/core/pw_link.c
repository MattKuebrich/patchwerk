/* SPDX-License-Identifier: GPL-3.0-only
 * PATCHWERK's live link (pw_link.h): the patch kept the same on the FM-1 and the web page, both ways. */
#include "pw_link.h"
#include "pw_engine.h"
#include <string.h>

#define PAYLOAD_MAX 448u                /* -> 4 + 512 + 1 wire bytes at most */
#define POLL_MS 20u
#define RESEND_MS 400u
#define KEEPALIVE_MS 2000u
#define TIMEOUT_MS 5000u
#define METER_MS 100u
enum { OP_REMOVE = 1, OP_ADD, OP_DISCONNECT, OP_CONNECT, OP_PARAM, OP_DATA, OP_MOVE, OP_GLOBAL, OP_RESYNC, OP_FLAGS, OP_PATCH, OP_AMOUNT };

static struct {
    uint8_t used[PW_MAX_MODULES], type[PW_MAX_MODULES];
    int16_t x[PW_MAX_MODULES], y[PW_MAX_MODULES];
    float p[PW_MAX_MODULES][PW_MAX_PARAM];
    uint32_t pdirty[PW_MAX_MODULES]; uint8_t ddirty[PW_MAX_MODULES];
    uint8_t data[PW_MAX_MODULES][PW_DATA_MAX];
    pw_cable cab[PW_MAX_CABLES];
    float bpm; uint8_t playing, gdirty; char name[13];
    uint8_t flags[PW_MAX_MODULES];
    uint8_t voices, var_cur, perf[PW_PERF][2], perf_cc[PW_PERF], pdirty2;     /* the patch's voices, variation, performance page */
    /* what arrived and could not be kept here at all (a slot past this end's last, a full cable table): sent
     * back as its undoing */
    uint8_t rej[16][5]; unsigned n_rej; uint8_t rej_lost;   /* more than that: the host starts over */
} sh PW_POOL;
static struct {
    int role;
    uint8_t active, hello_ok, tx_seq, rx_seq, rx_any, last_applied, applied_any;
    uint8_t inflight, inflight_seq;
    uint32_t heard_ms, sent_ms, poll_ms, meter_ms;
    unsigned cap_modules, cap_cables;
    int8_t remote_step[PW_MAX_MODULES];
    uint32_t stats[6];
    uint8_t pay[PAYLOAD_MAX + 8];
    unsigned pn;
    uint8_t wire[PW_LINK_WIRE_MAX + 8];  /* the frame in flight (host: kept for a resend) */
    unsigned wn;
} lk PW_POOL;

/* ---------------------------------------------------------------- framing */
static unsigned pack7(const uint8_t *in, unsigned n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0;
    unsigned o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8u;
        while (nb >= 7u) { out[o++] = (uint8_t)(acc & 0x7Fu); acc >>= 7; nb -= 7u; }
    }
    if (nb) out[o++] = (uint8_t)(acc & 0x7Fu);
    return o;
}
static unsigned unpack7(const uint8_t *in, unsigned n, uint8_t *out, unsigned cap)
{
    uint32_t acc = 0, nb = 0;
    unsigned o = 0;
    while (n--) {
        acc |= (uint32_t)(*in++ & 0x7Fu) << nb;
        nb += 7u;
        if (nb >= 8u) {
            if (o < cap) out[o] = (uint8_t)acc;
            o++;
            acc >>= 8; nb -= 8u;
        }
    }
    return o;
}
static void send_payload(void)
{
    lk.wire[0] = 0xF0; lk.wire[1] = 0x7D; lk.wire[2] = 'N'; lk.wire[3] = 'L';
    lk.wn = 4u + pack7(lk.pay, lk.pn, lk.wire + 4);
    lk.wire[lk.wn++] = 0xF7;
    pw_plat_link_send(lk.wire, lk.wn);
    lk.stats[0]++;
}
static void put8(unsigned v) { lk.pay[lk.pn++] = (uint8_t)v; }
static void put16(int v) { put8((unsigned)v & 255u); put8(((unsigned)v >> 8) & 255u); }
static void putf(float f) { union { float f; uint32_t u; } x; x.f = f; for (int i = 0; i < 4; ++i) put8(x.u >> (8 * i)); }
static int get16(const uint8_t *b) { return (int16_t)(b[0] | (b[1] << 8)); }
static float getf(const uint8_t *b) { union { float f; uint32_t u; } x; x.u = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); return x.f; }
static int room(unsigned n) { return lk.pn + n <= PAYLOAD_MAX; }

/* ---------------------------------------------------------------- shadow */
static void shadow_forget(void)
{
    memset(&sh, 0, sizeof sh);
    sh.gdirty = 1; sh.pdirty2 = 1;
}
static void shadow_drop_cables(unsigned slot)
{
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c)
        if (sh.cab[c].used && (sh.cab[c].sm == slot || sh.cab[c].dm == slot)) sh.cab[c].used = 0;
}
static int shadow_find_cable(unsigned sm, unsigned so, unsigned dm, unsigned di)
{
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &sh.cab[c];
        if (k->used && k->sm == sm && k->so == so && k->dm == dm && k->di == di) return (int)c;
    }
    return -1;
}
static int shadow_add_cable(unsigned sm, unsigned so, unsigned dm, unsigned di)
{
    if (shadow_find_cable(sm, so, dm, di) >= 0) return 1;
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c)
        if (!sh.cab[c].used) {
            sh.cab[c].used = 1; sh.cab[c].sm = (uint8_t)sm; sh.cab[c].so = (uint8_t)so; sh.cab[c].dm = (uint8_t)dm; sh.cab[c].di = (uint8_t)di; sh.cab[c].amt = 100;
            return 1;
        }
    return 0;
}
/* after the shadow says a slot holds a new module: everything about it is to be sent */
static void shadow_module(unsigned slot, unsigned type, int x, int y)
{
    sh.used[slot] = 1; sh.type[slot] = (uint8_t)type; sh.x[slot] = (int16_t)x; sh.y[slot] = (int16_t)y;
    memset(sh.p[slot], 0, sizeof sh.p[slot]);
    memset(sh.data[slot], 0, sizeof sh.data[slot]);
    sh.pdirty[slot] = 0xFFFFFFFFu;
    sh.ddirty[slot] = 1;
    sh.flags[slot] = 0;
}
static int live_used(unsigned slot) { return pws.mod[slot].used == 1; }
static void reject(unsigned op, unsigned a, unsigned b, unsigned c, unsigned d)
{
    if (sh.n_rej >= 16) { sh.rej_lost = 1; return; }
    uint8_t *r = sh.rej[sh.n_rej++];
    r[0] = (uint8_t)op; r[1] = (uint8_t)a; r[2] = (uint8_t)b; r[3] = (uint8_t)c; r[4] = (uint8_t)d;
}

/* ---------------------------------------------------------------- the differences, as operations */
static unsigned build_ops(void)
{
    unsigned ops = 0;
    if (sh.rej_lost) {                    /* too much to undo one by one: the host takes this end's patch again */
        put8(OP_RESYNC); sh.rej_lost = 0; sh.n_rej = 0;
        return 1;
    }
    /* what could not be kept here: undone at the other end */
    while (sh.n_rej) {
        const uint8_t *r = sh.rej[sh.n_rej - 1];
        if (!room(5)) return ops;
        if (r[0] == OP_REMOVE) { put8(OP_REMOVE); put8(r[1]); }
        else { put8(OP_DISCONNECT); put8(r[1]); put8(r[2]); put8(r[3]); put8(r[4]); }
        ++ops; --sh.n_rej;
    }
    /* removed (or replaced by another kind) */
    for (unsigned s = 0; s < PW_MAX_MODULES; ++s)
        if (sh.used[s] && (!live_used(s) || pws.mod[s].type != sh.type[s])) {
            if (!room(2)) return ops;
            put8(OP_REMOVE); put8(s); ++ops;
            sh.used[s] = 0;
            shadow_drop_cables(s);
        }
    /* added */
    for (unsigned s = 0; s < PW_MAX_MODULES; ++s)
        if (live_used(s) && !sh.used[s]) {
            if (!room(7)) return ops;
            put8(OP_ADD); put8(s); put8(pws.mod[s].type); put16(pws.mod[s].x); put16(pws.mod[s].y); ++ops;
            shadow_module(s, pws.mod[s].type, pws.mod[s].x, pws.mod[s].y);
        }
    /* cables gone */
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        pw_cable *k = &sh.cab[c];
        if (!k->used || pw_find_cable(k->sm, k->so, k->dm, k->di) >= 0) continue;
        if (!room(5)) return ops;
        put8(OP_DISCONNECT); put8(k->sm); put8(k->so); put8(k->dm); put8(k->di); ++ops;
        k->used = 0;
    }
    /* cables new */
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (!k->used || !sh.used[k->sm] || !sh.used[k->dm] || shadow_find_cable(k->sm, k->so, k->dm, k->di) >= 0) continue;
        if (!room(5)) return ops;
        put8(OP_CONNECT); put8(k->sm); put8(k->so); put8(k->dm); put8(k->di); ++ops;
        shadow_add_cable(k->sm, k->so, k->dm, k->di);
    }
    /* cables' amounts */
    for (unsigned c = 0; c < PW_MAX_CABLES; ++c) {
        const pw_cable *k = &pws.cable[c];
        if (!k->used) continue;
        int j = shadow_find_cable(k->sm, k->so, k->dm, k->di);
        if (j < 0 || sh.cab[j].amt == k->amt) continue;
        if (!room(6)) return ops;
        put8(OP_AMOUNT); put8(k->sm); put8(k->so); put8(k->dm); put8(k->di); put8((uint8_t)k->amt); ++ops;
        sh.cab[j].amt = k->amt;
    }
    /* parameters, sequencer steps, positions */
    for (unsigned s = 0; s < PW_MAX_MODULES; ++s) {
        if (!sh.used[s] || !live_used(s)) continue;
        const pw_module *m = &pws.mod[s];
        const pw_type *t = &pw_types[m->type];
        for (unsigned p = 0; p < t->n_param; ++p) {
            if (!((sh.pdirty[s] >> p) & 1u) && sh.p[s][p] == m->p[p]) continue;
            if (!room(7)) return ops;
            put8(OP_PARAM); put8(s); put8(p); putf(m->p[p]); ++ops;
            sh.p[s][p] = m->p[p];
            sh.pdirty[s] &= ~(1u << p);
        }
        sh.pdirty[s] = 0;
        if (pw_has_data(m->type) && (sh.ddirty[s] || memcmp(sh.data[s], m->data, PW_DATA_MAX))) {
            if (!room(2 + PW_DATA_MAX)) return ops;
            put8(OP_DATA); put8(s);
            for (unsigned i = 0; i < PW_DATA_MAX; ++i) put8(m->data[i]);
            ++ops;
            memcpy(sh.data[s], m->data, PW_DATA_MAX);
        }
        sh.ddirty[s] = 0;
        if (sh.flags[s] != m->flags) {
            if (!room(3)) return ops;
            put8(OP_FLAGS); put8(s); put8(m->flags); ++ops;
            sh.flags[s] = m->flags;
        }
        if (sh.x[s] != m->x || sh.y[s] != m->y) {
            if (!room(6)) return ops;
            put8(OP_MOVE); put8(s); put16(m->x); put16(m->y); ++ops;
            sh.x[s] = m->x; sh.y[s] = m->y;
        }
    }
    if (sh.pdirty2 || sh.voices != pws.voices || sh.var_cur != pws.var_cur || memcmp(sh.perf, pws.perf, sizeof sh.perf) || memcmp(sh.perf_cc, pws.perf_cc, PW_PERF)) {
        if (!room(3 + 3 * PW_PERF)) return ops;
        put8(OP_PATCH); put8(pws.voices); put8(pws.var_cur);
        for (unsigned k = 0; k < PW_PERF; ++k) { put8(pws.perf[k][0]); put8(pws.perf[k][1]); }
        for (unsigned k = 0; k < PW_PERF; ++k) put8(pws.perf_cc[k]);
        ++ops;
        sh.voices = pws.voices; sh.var_cur = pws.var_cur; memcpy(sh.perf, pws.perf, sizeof sh.perf); memcpy(sh.perf_cc, pws.perf_cc, PW_PERF); sh.pdirty2 = 0;
    }
    if (sh.gdirty || sh.bpm != pws.bpm || sh.playing != pws.playing || memcmp(sh.name, pws.name, 12)) {
        if (!room(18)) return ops;
        put8(OP_GLOBAL); putf(pws.bpm); put8(pws.playing);
        for (unsigned i = 0; i < 12; ++i) put8((uint8_t)pws.name[i]);
        ++ops;
        sh.bpm = pws.bpm; sh.playing = pws.playing; memcpy(sh.name, pws.name, 12); sh.gdirty = 0;
    }
    return ops;
}

/* ---------------------------------------------------------------- applying the other end's */
static int apply_ops(const uint8_t *b, unsigned n)
{
    unsigned at = 0;
    int applied = 0;
    while (at < n) {
        unsigned op = b[at], s = at + 1 < n ? b[at + 1] : 0;
        switch (op) {
        case OP_REMOVE:
            if (at + 2 > n) return applied;
            if (s < PW_MAX_MODULES) { if (live_used(s)) pw_remove(s); sh.used[s] = 0; shadow_drop_cables(s); }
            at += 2; break;
        case OP_ADD: {
            if (at + 7 > n) return applied;
            unsigned type = b[at + 2];
            int x = get16(b + at + 3), y = get16(b + at + 5);
            if (s >= PW_MAX_MODULES || type >= PW_T_COUNT) reject(OP_REMOVE, s, 0, 0, 0);
            else {
                if (live_used(s) && pws.mod[s].type != type) pw_remove(s);
                if (!live_used(s)) pw_add_at((int)s, type, x, y);
                else pw_move(s, x, y);
                /* the shadow: what the sender believes this end has now (its defaults; its values follow) */
                shadow_module(s, type, x, y);
                if (live_used(s) && pws.mod[s].type == type) {
                    memcpy(sh.p[s], pws.mod[s].p, sizeof sh.p[s]);
                    memcpy(sh.data[s], pws.mod[s].data, PW_DATA_MAX);
                    sh.pdirty[s] = 0; sh.ddirty[s] = 0;
                }
            }
            at += 7;
        } break;
        case OP_DISCONNECT: case OP_CONNECT: {
            if (at + 5 > n) return applied;
            unsigned sm = b[at + 1], so = b[at + 2], dm = b[at + 3], di = b[at + 4];
            if (op == OP_CONNECT && (sm >= PW_MAX_MODULES || dm >= PW_MAX_MODULES)) reject(OP_DISCONNECT, sm, so, dm, di);
            else if (sm < PW_MAX_MODULES && dm < PW_MAX_MODULES) {
                if (op == OP_CONNECT) {
                    pw_connect(sm, so, dm, di);
                    if (!shadow_add_cable(sm, so, dm, di)) reject(OP_DISCONNECT, sm, so, dm, di);
                }
                else {
                    int c = pw_find_cable(sm, so, dm, di), k = shadow_find_cable(sm, so, dm, di);
                    if (c >= 0) pw_disconnect((unsigned)c);
                    if (k >= 0) sh.cab[k].used = 0;
                }
            }
            at += 5;
        } break;
        case OP_AMOUNT: {
            if (at + 6 > n) return applied;
            unsigned sm = b[at + 1], so = b[at + 2], dm = b[at + 3], di = b[at + 4];
            int c = pw_find_cable(sm, so, dm, di), k = shadow_find_cable(sm, so, dm, di);
            if (c >= 0) pw_cable_amount((unsigned)c, (int8_t)b[at + 5]);
            if (k >= 0) sh.cab[k].amt = (int8_t)b[at + 5];
            at += 6;
        } break;
        case OP_PARAM: {
            if (at + 7 > n) return applied;
            unsigned p = b[at + 2];
            float v = getf(b + at + 3);
            if (s < PW_MAX_MODULES && p < PW_MAX_PARAM) {
                if (live_used(s)) pw_set_param(s, p, v);
                if (v == v) sh.p[s][p] = v;
            }
            at += 7;
        } break;
        case OP_DATA:
            if (at + 2 + PW_DATA_MAX > n) return applied;
            if (s < PW_MAX_MODULES) {
                if (live_used(s)) { memcpy(pws.mod[s].data, b + at + 2, PW_DATA_MAX); pws.revision++; }
                memcpy(sh.data[s], b + at + 2, PW_DATA_MAX);
            }
            at += 2 + PW_DATA_MAX; break;
        case OP_MOVE:
            if (at + 6 > n) return applied;
            if (s < PW_MAX_MODULES) {
                int x = get16(b + at + 2), y = get16(b + at + 4);
                if (live_used(s)) pw_move(s, x, y);
                sh.x[s] = (int16_t)x; sh.y[s] = (int16_t)y;
            }
            at += 6; break;
        case OP_GLOBAL: {
            if (at + 18 > n) return applied;
            float bpm = getf(b + at + 1);
            unsigned playing = b[at + 5];
            if (bpm == bpm && bpm != pws.bpm) pw_set_bpm(bpm);
            if ((playing != 0) != (pws.playing != 0)) pw_set_playing(playing != 0);
            for (unsigned i = 0; i < 12; ++i) {
                char ch = (char)b[at + 6 + i];
                pws.name[i] = (ch >= 32 && ch < 127) ? ch : 0;
            }
            pws.name[12] = 0;
            pws.revision++;
            sh.bpm = bpm; sh.playing = (uint8_t)(playing != 0); memcpy(sh.name, b + at + 6, 12); sh.name[12] = 0;
            for (unsigned i = 0; i < 12; ++i) if (!((uint8_t)sh.name[i] >= 32 && (uint8_t)sh.name[i] < 127)) sh.name[i] = 0;
            at += 18;
        } break;
        case OP_FLAGS:
            if (at + 3 > n) return applied;
            if (s < PW_MAX_MODULES) { if (live_used(s)) pw_set_flags(s, b[at + 2]); sh.flags[s] = b[at + 2]; }
            at += 3; break;
        case OP_PATCH: {
            if (at + 3 + 3 * PW_PERF > n) return applied;
            unsigned voices = b[at + 1], var = b[at + 2];
            if (voices != pws.voices) pw_set_voices(voices);
            /* the variation: the device switches (its values come back); the host only shows which */
            if (var < PW_VARS && var != pws.var_cur) { if (lk.role == PW_LINK_DEVICE) pw_var_select(var); else { pws.var_cur = (uint8_t)var; pws.revision++; } }
            for (unsigned k = 0; k < PW_PERF; ++k) {
                unsigned sl = b[at + 3 + 2 * k], p = b[at + 4 + 2 * k];
                pws.perf[k][0] = sl < PW_MAX_MODULES ? (uint8_t)sl : 0xff; pws.perf[k][1] = sl < PW_MAX_MODULES ? (uint8_t)p : 0xff;
            }
            for (unsigned k = 0; k < PW_PERF; ++k) pws.perf_cc[k] = b[at + 3 + 2 * PW_PERF + k];
            pws.revision++;
            sh.voices = pws.voices; sh.var_cur = (uint8_t)(var < PW_VARS ? var : 0); memcpy(sh.perf, pws.perf, sizeof sh.perf); memcpy(sh.perf_cc, pws.perf_cc, PW_PERF);
            if (lk.role == PW_LINK_DEVICE) sh.var_cur = pws.var_cur;
            at += 3 + 3 * PW_PERF;
        } break;
        case OP_RESYNC:
            return -1;                      /* (the host: hello again) */
        default:
            return applied;                 /* an operation this version does not know: the rest is lost */
        }
        ++applied;
        lk.stats[3]++;
    }
    return applied;
}

static void apply_meters(const uint8_t *b, unsigned n)
{
    if (n < 3) return;
    pws.out_peak_l = b[0] * (1.0f / 255.0f);
    pws.out_peak_r = b[1] * (1.0f / 255.0f);
    for (unsigned s = 0; s < PW_MAX_MODULES; ++s) lk.remote_step[s] = -1;
    unsigned at = 3;
    while (at + 2 <= n) {
        unsigned s = b[at], k = b[at + 1] & 7u;
        if (at + 3 + k > n) break;
        if (s < PW_MAX_MODULES) {
            for (unsigned o = 0; o < k && o < PW_MAX_OUT; ++o) pws.mod[s].level[o] = b[at + 2 + o];
            lk.remote_step[s] = (int8_t)((int)b[at + 2 + k] - 1);
        }
        at += 3 + k;
    }
}

/* ---------------------------------------------------------------- entries */
void pw_link_init(int role)
{
    memset(&lk, 0, sizeof lk);
    lk.role = role;
    for (unsigned s = 0; s < PW_MAX_MODULES; ++s) lk.remote_step[s] = -1;
    shadow_forget();
}
int pw_link_active(void) { return lk.active && (lk.role == PW_LINK_DEVICE || lk.hello_ok); }
int pw_link_capacity(unsigned *modules, unsigned *cables)
{
    if (!lk.hello_ok) return 0;
    *modules = lk.cap_modules; *cables = lk.cap_cables;
    return 1;
}
int pw_link_remote_step(unsigned slot) { return slot < PW_MAX_MODULES && pw_link_active() ? lk.remote_step[slot] : -1; }
uint32_t pw_link_stats(unsigned what) { return what < 6 ? lk.stats[what] : 0; }

void pw_link_hello(uint32_t now)
{
    /* the host's patch is the device's from now: cleared, and the shadow with it */
    pw_clear();
    pw_set_playing(0);
    memset(pws.name, 0, sizeof pws.name);
    shadow_forget();
    sh.gdirty = 0;                          /* (the device's globals come first) */
    sh.bpm = pws.bpm; memcpy(sh.name, pws.name, 12);
    lk.hello_ok = 0; lk.rx_any = 0; lk.inflight = 0;
    lk.pn = 0;
    put8('H'); put8(++lk.tx_seq); put8(PW_LINK_VERSION);
    send_payload();
    lk.inflight = 1; lk.inflight_seq = lk.tx_seq;
    lk.sent_ms = now;
    lk.active = 1; lk.heard_ms = now;
}

int pw_link_rx(const uint8_t *p, unsigned n, uint32_t now)
{
    static uint8_t buf[PAYLOAD_MAX + 64];
    if (n < 5 || p[0] != 0x7D || p[1] != 'N' || p[2] != 'L') return 0;
    unsigned m = unpack7(p + 3, n - 3, buf, sizeof buf);
    if (m < 2 || m > sizeof buf) return 1;
    unsigned kind = buf[0], seq = buf[1];
    lk.stats[1]++;
    lk.heard_ms = now;
    if (lk.role == PW_LINK_DEVICE) {
        if (kind == 'H') {
            shadow_forget();                /* everything this end has is news to the host */
            lk.active = 1; lk.applied_any = 1; lk.last_applied = (uint8_t)seq;
            lk.pn = 0;
            put8('h'); put8(seq); put8(PW_LINK_VERSION); put8(PW_MAX_MODULES); put8(PW_MAX_CABLES); put8(lk.tx_seq);
            send_payload();
            lk.poll_ms = now - POLL_MS;     /* the patch at once */
        } else if (kind == 'U') {
            lk.active = 1;
            if (!lk.applied_any || seq != lk.last_applied) apply_ops(buf + 2, m - 2);
            lk.applied_any = 1; lk.last_applied = (uint8_t)seq;
            lk.pn = 0;
            put8('K'); put8(seq);
            send_payload();
        }
        return 1;
    }
    /* host */
    if (kind == 'h') {
        if (m >= 6 && lk.inflight && seq == lk.inflight_seq) {
            lk.hello_ok = 1; lk.inflight = 0;
            lk.cap_modules = buf[3]; lk.cap_cables = buf[4];
            lk.rx_any = 1; lk.rx_seq = buf[5];      /* the device's frames count on from here */
        }
    } else if (kind == 'K') {
        if (lk.inflight && seq == lk.inflight_seq) lk.inflight = 0;
    } else if (kind == 'U' && lk.hello_ok) {
        if (lk.rx_any && seq != (uint8_t)(lk.rx_seq + 1u)) {
            lk.stats[5]++;
            pw_link_hello(now);             /* a frame went missing: start over */
            return 1;
        }
        lk.rx_any = 1; lk.rx_seq = (uint8_t)seq;
        if (apply_ops(buf + 2, m - 2) < 0) pw_link_hello(now);
    } else if (kind == 'V' && lk.hello_ok) {
        if (seq != lk.rx_seq) {                 /* the meters carry the device's count: one went missing */
            lk.stats[5]++;
            pw_link_hello(now);
            return 1;
        }
        apply_meters(buf + 2, m - 2);
    }
    return 1;
}

static void send_meters(void)
{
    lk.pn = 0;
    put8('V'); put8(lk.tx_seq);
    float l = pws.out_peak_l * 255.0f, r = pws.out_peak_r * 255.0f;
    put8(l > 255 ? 255 : (unsigned)l); put8(r > 255 ? 255 : (unsigned)r); put8(pws.playing);
    for (unsigned s = 0; s < PW_MAX_MODULES; ++s) {
        if (!live_used(s)) continue;
        unsigned k = pws.mod[s].n_out;
        if (!room(3 + k)) break;
        put8(s); put8(k);
        for (unsigned o = 0; o < k; ++o) put8(pws.mod[s].level[o]);
        put8((unsigned)(pw_seq_step(s) + 1));
    }
    send_payload();
}

void pw_link_poll(uint32_t now)
{
    if (!lk.active) return;
    if (now - lk.heard_ms > TIMEOUT_MS) {   /* the other end went quiet */
        lk.active = 0; lk.hello_ok = 0; lk.inflight = 0;
        return;
    }
    if (lk.role == PW_LINK_HOST) {
        if (lk.inflight) {
            if (now - lk.sent_ms >= RESEND_MS) {    /* no answer: the same frame again */
                pw_plat_link_send(lk.wire, lk.wn);
                lk.sent_ms = now; lk.stats[4]++;
            }
            return;
        }
        if (!lk.hello_ok || now - lk.poll_ms < POLL_MS) return;
        lk.poll_ms = now;
        lk.pn = 0;
        put8('U'); put8(++lk.tx_seq);
        unsigned ops = build_ops();
        if (!ops && now - lk.sent_ms < KEEPALIVE_MS) { --lk.tx_seq; return; }
        lk.stats[2] += ops;
        send_payload();
        lk.inflight = 1; lk.inflight_seq = lk.tx_seq; lk.sent_ms = now;
        return;
    }
    /* device: what changed here, a frame or two a poll; the meters */
    if (now - lk.poll_ms < POLL_MS) return;
    lk.poll_ms = now;
    for (int f = 0; f < 3; ++f) {
        lk.pn = 0;
        put8('U'); put8((uint8_t)(lk.tx_seq + 1u));
        unsigned ops = build_ops();
        if (!ops) break;
        ++lk.tx_seq;
        lk.stats[2] += ops;
        send_payload();
    }
    if (now - lk.meter_ms >= METER_MS) { lk.meter_ms = now; send_meters(); }
}
