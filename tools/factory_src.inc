/* SPDX-License-Identifier: GPL-3.0-only
 * The SOURCE of the factory patches: each is built through the engine API, then tools/gen_factory.sh saves it with pw_save and
 * packs it into app/core/pw_factory_data.inc, which is what the firmware loads (pw_presets.c). The firmware does not compile this
 * file: it is included only when PW_FACTORY_GEN is defined (the generator). To change or add a patch: edit it here, then run
 * tools/gen_factory.sh. (It uses the helpers of pw_presets.c: add, cable, set, perf, steps.) */

/* ---- every factory patch's performance page and variations (2-4 differ; the rest are copies of the first) */
__attribute__((noinline)) static int find(unsigned type, int nth)
{
    const old_merge_t *mg = old_merge_of(type);
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i)
        if (pws.mod[i].used == 1 && pws.mod[i].type == (mg ? mg->to : type) && (!mg || old_of[i] == type) && nth-- == 0) return (int)i;
    return -1;
}
static void V(unsigned k) { pw_var_select(k); }
static void notes(int slot, const int8_t n[16]) { if (slot >= 0) for (int i = 0; i < 16; ++i) pws.mod[slot].data[i] = (uint8_t)n[i]; }
static void gates(int slot, unsigned g) { if (slot >= 0) { pws.mod[slot].data[16] = (uint8_t)g; pws.mod[slot].data[17] = (uint8_t)(g >> 8); } }

/* ---- SONG examples: patches with snapshots (some modules bypassed in some) and a song that plays them with automation lanes */
static int build_from_recipe(const char *recipe);
static void snaps_from_now(void) { for (unsigned k = 1; k < PW_VARS; ++k) pw_var_copy(0, k); }
static void clip(unsigned k, unsigned start, unsigned len, unsigned val) { pws.song.clip[k] = (pw_clip){0, (uint8_t)start, (uint8_t)len, (uint8_t)val}; }
static void slices(int ms, const uint8_t v[16]) { if (ms >= 0) for (unsigned i = 0; i < 16; ++i) pw_mseq_set(&pws.mod[ms], 0, i, v[i]); }   /* MOD SEQ lane A: the slice each step plays (0-255 of the slices) */
static void grp(unsigned g, int a, int b, int c, int d)   /* modules a..d (a slot, or -1) become group g */
{
    int m[4] = {a, b, c, d};
    for (int i = 0; i < 4; ++i) if (m[i] >= 0) pw_set_group((unsigned)m[i], g);
}
static void gbyp(unsigned g, int on)   /* every module of group g bypassed (or not) */
{
    for (unsigned i = 0; i < PW_MAX_MODULES; ++i)
        if (pws.mod[i].used == 1 && pws.mod[i].group == g) pw_set_flags(i, on ? pws.mod[i].flags | PW_F_BYPASS : pws.mod[i].flags & ~PW_F_BYPASS);
}
static void lane(int slot, unsigned par, const uint16_t *pt, unsigned n)   /* pt: time (sixteenths of a bar), value 0-255, shape */
{
    if (slot < 0) return;
    int l = pw_lane_add((unsigned)slot, par);
    for (unsigned i = 0; l >= 0 && i < n; ++i) pw_lane_set((unsigned)l, pt[3 * i], pt[3 * i + 1], pt[3 * i + 2]);
}
static void song_patch(unsigned index)
{
    pws.song.on = 1; pws.song.glide = 2;
    if (index == 45) {            /* SONG BERLIN: bass alone, then a soft kick, then a short snare and the hats too and the filter opens; the cutoff and the resonance move all along */
        build_from_recipe("CLOCK SEQ VCO ENV VCF VCA DELAY TRIG DRUM DRUM#2 DRUM#3 MIX OUT|CLOCK.CLK>SEQ.CLOCK CLOCK.CLK>TRIG.CLOCK SEQ.PITCH>VCO.V/OCT SEQ.GATE>ENV.GATE VCO.SAW>VCF.IN VCF.LP>VCA.IN ENV.ENV>VCF.FREQ ENV.ENV>VCA.CV VCA.OUT>DELAY.IN DELAY.MIX>MIX.IN_1 TRIG.A>DRUM.TRIG TRIG.B>DRUM#2.TRIG TRIG.C>DRUM#3.TRIG DRUM.OUT>MIX.IN_2 DRUM#2.OUT>MIX.IN_3 DRUM#3.OUT>MIX.IN_4 MIX.L>OUT.L MIX.R>OUT.R|@PLAY @SEQ=-12,-12,0,-12,3,-12,7,10,-12,-12,0,-12,3,-12,10,7 @TRIG=A:1000100010001000,B:0000000100000010,C:1111111111111111 DRUM.PRESET=8 DRUM#2.PRESET=11 DRUM#3.PRESET=21 ENV.DECAY=0.2 ENV.SUSTAIN=0.05 VCF.CUTOFF=300 VCF.RES=0.5 DELAY.TIME=0.375 DELAY.FBK=0.4 DELAY.MIX=0.3 MIX.LEVEL_1=0.5 MIX.LEVEL_2=0.5 MIX.LEVEL_3=0.3 MIX.LEVEL_4=0.2");
        int vcf = find(PW_T_VCF, 0), dly = find(PW_T_DELAY, 0), dk = find(PW_T_DRUMS, 0), cl = find(PW_T_DRUMS, 1), hh = find(PW_T_DRUMS, 2);
        grp(1, find(PW_T_VCO, 0), find(PW_T_ENV, 0), vcf, find(PW_T_VCA, 0)); grp(2, dk, -1, -1, -1); grp(3, cl, -1, -1, -1); grp(4, hh, -1, -1, -1);
        gbyp(2, 1); gbyp(3, 1); gbyp(4, 1); snaps_from_now();             /* 1: bass alone */
        pw_var_select(1); gbyp(2, 0);                                      /* 2: the kick (G2) comes in */
        pw_var_select(2); gbyp(2, 0); gbyp(3, 0); gbyp(4, 0); set(dly, 3, 0.45f);   /* 3: the snare and hats (G3, G4), a longer echo */
        pw_var_select(0);
        perf(vcf, 0); perf(vcf, 1); perf(dly, 1); perf(dly, 3); perf(find(PW_T_SEQ, 0), 1);
        clip(0, 0, 4, 0); clip(1, 4, 4, 1); clip(2, 8, 4, 2); clip(3, 12, 4, 1);
        static const uint16_t cut[] = {0, 70, 0,  96, 130, 2,  192, 215, 2,  255, 90, 0};
        static const uint16_t res[] = {0, 90, 0,  128, 200, 0,  208, 120, 1};
        lane(vcf, 0, cut, 4); lane(vcf, 1, res, 3);
    } else if (index == 46) {     /* SONG ACID: a clean line over a kick, then driven with a snare and open hats, then a long echo; the cutoff and resonance ride on top */
        build_from_recipe("CLOCK SEQ VCO ENV VCF VCA DIST DELAY TRIG DRUM DRUM#2 DRUM#3 MIX OUT|CLOCK.CLK>SEQ.CLOCK CLOCK.CLK>TRIG.CLOCK SEQ.PITCH>VCO.V/OCT SEQ.GATE>ENV.GATE VCO.SAW>VCF.IN VCF.LP>VCA.IN ENV.ENV>VCF.FREQ ENV.ENV>VCA.CV VCA.OUT>DIST.IN DIST.OUT>DELAY.IN DELAY.MIX>MIX.IN_1 TRIG.A>DRUM.TRIG TRIG.B>DRUM#2.TRIG TRIG.C>DRUM#3.TRIG DRUM.OUT>MIX.IN_2 DRUM#2.OUT>MIX.IN_3 DRUM#3.OUT>MIX.IN_4 MIX.L>OUT.L MIX.R>OUT.R|@PLAY @SEQ=-12,-12,0,-12,-11,-12,12,-12,-12,0,-12,-7,-12,-4,-5,-12 @TRIG=A:1000100010001000,B:0000100000001000,C:0010001000100010 DRUM.PRESET=1 DRUM#2.PRESET=39 DRUM#3.PRESET=22 DRUM#3.METAL=0 DRUM#3.NOISE=0.9 SEQ.SLIDE=0.05 ENV.DECAY=0.18 ENV.SUSTAIN=0 VCF.CUTOFF=250 VCF.RES=0.85 VCF.TYPE=1 DIST.DRIVE=0.3 DIST.MIX=0.6 DELAY.TIME=0.375 DELAY.FBK=0.45 DELAY.MIX=0.35 MIX.LEVEL_1=0.45 MIX.LEVEL_2=0.5 MIX.LEVEL_3=0.4 MIX.LEVEL_4=0.25");
        pw_set_bpm(128);
        int vcf = find(PW_T_VCF, 0), di = find(PW_T_DIST, 0), dly = find(PW_T_DELAY, 0), cl = find(PW_T_DRUMS, 1), hh = find(PW_T_DRUMS, 2);
        gates(find(PW_T_SEQ, 0), 0xB6DB);
        grp(1, find(PW_T_VCO, 0), find(PW_T_ENV, 0), vcf, find(PW_T_VCA, 0)); grp(2, find(PW_T_DRUMS, 0), -1, -1, -1); grp(3, di, -1, -1, -1); grp(4, cl, hh, -1, -1);
        gbyp(3, 1); gbyp(4, 1); snaps_from_now();                          /* 1: the clean line over the kick */
        pw_var_select(1); gbyp(3, 0); gbyp(4, 0); set(di, 0, 0.6f);        /* 2: driven (G3), with the snare and hats (G4) */
        pw_var_select(2); gbyp(3, 0); gbyp(4, 0); set(di, 0, 0.6f); set(dly, 1, 0.75f); set(dly, 3, 0.55f);   /* 3: a long echo */
        pw_var_select(0);
        perf(vcf, 0); perf(vcf, 1); perf(di, 0); perf(dly, 1); perf(dly, 3); perf(find(PW_T_SEQ, 0), 1);
        clip(0, 0, 4, 0); clip(1, 4, 4, 1); clip(2, 8, 4, 2); clip(3, 12, 4, 1);
        static const uint16_t cut[] = {0, 40, 2,  64, 160, 1,  128, 90, 2,  192, 230, 0,  255, 60, 0};
        static const uint16_t res[] = {0, 180, 0,  128, 230, 2,  255, 170, 0};
        lane(vcf, 0, cut, 5); lane(vcf, 1, res, 3);
    } else if (index == 47) {     /* SONG BREAK: a chopped drum break from the sample bank (kick, snare, hats, rim ghosts), a stabby bass and a two-note pad; the break drops out in the last section */
        build_from_recipe("CLOCK SEQ TRIG RACK VCO ENV VCF VCA VCO#2 VCO#3 MIX MIX#2 OUT|CLOCK.CLK>SEQ.CLOCK CLOCK.CLK>TRIG.CLOCK SEQ.PITCH>VCO.V/OCT SEQ.GATE>ENV.GATE VCO.SAW>VCF.IN VCF.LP>VCA.IN ENV.ENV>VCF.FREQ ENV.ENV>VCA.CV VCA.OUT>MIX.IN_1 TRIG.A>RACK.T1 TRIG.B>RACK.T2 TRIG.C>RACK.T4 TRIG.D>RACK.T7 RACK.L>MIX.IN_2 VCO#2.TRI>MIX#2.IN_1 VCO#3.TRI>MIX#2.IN_2 MIX.L>MIX#2.IN_3 MIX#2.L>OUT.L MIX#2.R>OUT.R|@PLAY @SEQ=-12,-12,-12,-24,-12,-12,-9,-12,-12,-12,-12,-24,-12,-10,-12,-7 @TRIG=A:1010000000110000,B:0000100000001001,C:1111111111111111,D:0000001000100100 RACK.KIT=9 ENV.DECAY=0.12 ENV.SUSTAIN=0 VCF.CUTOFF=900 VCF.RES=0.4 VCO#2.TUNE=-12 VCO#3.TUNE=-5 VCO#3.FINE=6 MIX.LEVEL_1=0.7 MIX.LEVEL_2=0.7 MIX#2.LEVEL_1=0.14 MIX#2.LEVEL_2=0.12 MIX#2.LEVEL_3=0.7");
        pw_set_bpm(96);
        int vcf = find(PW_T_VCF, 0), sq = find(PW_T_SEQ, 0), v1 = find(PW_T_VCO, 0), p1 = find(PW_T_VCO, 1), p2 = find(PW_T_VCO, 2), rk = find(PW_T_DRUMRACK, 0);
        int tr = find(PW_T_TRIGSEQ, 0);
        if (tr >= 0) { set(tr, 1, 0.14f); pw_trig_set((unsigned)tr, 0, 0, 2); pw_trig_set((unsigned)tr, 1, 4, 2); pw_trig_set((unsigned)tr, 1, 12, 2); for (unsigned i = 0; i < 16; i += 4) pw_trig_set((unsigned)tr, 2, i, 2); }
        gates(sq, 43337);   /* the bass: steps 1 4 7 9 12 14 16, broken */
        grp(1, v1, find(PW_T_ENV, 0), vcf, find(PW_T_VCA, 0)); grp(2, tr, rk, -1, -1); grp(3, p1, p2, -1, -1);
        gbyp(1, 1); snaps_from_now();                                      /* 1: the break (G2) and the pad (G3) */
        pw_var_select(1); gbyp(1, 0);                                      /* 2: the bass (G1) comes in */
        pw_var_select(2); gbyp(1, 0); set(vcf, 1, 0.6f);                   /* 3: the filter pushed */
        pw_var_select(3); gbyp(1, 1); gbyp(2, 1);                          /* 4: breakdown: the pad alone */
        pw_var_select(4); gbyp(1, 0);                                      /* 5: a busy break: the kick moves, snare ghosts, hats on every eighth */
        if (tr >= 0) {
            static const uint8_t kk[3] = {3, 6, 10}, gs[4] = {7, 10, 11, 15};
            for (unsigned i = 0; i < 3; ++i) pw_trig_set((unsigned)tr, 0, kk[i], 1);
            for (unsigned i = 0; i < 4; ++i) pw_trig_set((unsigned)tr, 1, gs[i], 1);
            for (unsigned i = 2; i < 16; i += 4) pw_trig_set((unsigned)tr, 2, i, 1);
        }
        pw_var_select(5); gbyp(1, 0);                                      /* 6: half time: the kick and one snare, the hats thinned */
        if (tr >= 0) { pw_trig_set((unsigned)tr, 1, 4, 0); pw_trig_set((unsigned)tr, 0, 10, 2); for (unsigned i = 4; i < 16; i += 8) pw_trig_set((unsigned)tr, 2, i, 0); }
        pw_var_select(0);
        perf(vcf, 0); perf(vcf, 1); perf(sq, 1); perf(find(PW_T_ENV, 0), 1); perf(p1, 0); perf(p2, 0); perf(vcf, 3);
        clip(0, 0, 4, 0); clip(1, 4, 4, 1); clip(2, 8, 4, 2); clip(3, 12, 4, 4); clip(4, 16, 4, 5); clip(5, 20, 4, 2); clip(6, 24, 4, 3);   /* 28 bars */
        static const uint16_t cut[] = {0, 60, 2,  64, 110, 2,  128, 200, 2,  192, 90, 2,  255, 60, 0};
        static const uint16_t t1[] = {0, 85, 1,  32, 71, 1,  64, 78, 1,  96, 67, 1,  128, 85, 1,  160, 71, 1,  192, 78, 1,  224, 67, 1};
        static const uint16_t t2[] = {0, 110, 1,  32, 96, 1,  64, 103, 1,  96, 92, 1,  128, 110, 1,  160, 96, 1,  192, 103, 1,  224, 92, 1};
        lane(vcf, 0, cut, 5); lane(p1, 0, t1, 8); lane(p2, 0, t2, 8);
    } else if (index == 58) {     /* SONG BEATS (listed after SONG DUB): BEAT FIT as a song. The drum loop in eight slices (BEATS 4: the loop is a bar at any tempo) played by a MOD SEQ lane, a bass line and a two-note pad, in a little room. G1 the break, G2 the bass, G3 the pad, G4 the room; the snapshots bring them in, the last one is double time (BEATS 2) and the breakdown leaves the pad */
        static const int8_t z[16] = {0};
        static const int8_t bass[16] = {-24, -24, -12, -24, -24, -24, -17, -24, -24, -24, -12, -24, -19, -24, -17, -22};
        /* the slice orders of the snapshots: straight (every other step), chopped, stuttered, ghosted, half time */
        static const uint8_t sA[16] = {0, 0, 32, 0, 64, 0, 96, 0, 128, 0, 160, 0, 192, 0, 224, 0};
        static const uint8_t sB[16] = {0, 32, 64, 96, 128, 160, 192, 224, 0, 32, 224, 96, 64, 160, 192, 32};
        static const uint8_t sC[16] = {0, 0, 64, 96, 128, 128, 128, 224, 0, 32, 32, 32, 64, 160, 192, 224};
        static const uint8_t sD[16] = {0, 224, 64, 224, 128, 32, 192, 96, 0, 224, 64, 160, 128, 96, 192, 224};
        static const uint8_t sE[16] = {0, 0, 0, 0, 64, 0, 0, 0, 128, 0, 0, 0, 192, 0, 0, 0};
        int sq = add(PW_T_SEQ, 0, 0), smp = add(PW_T_SAMPLER, 1, 0), cmp = add(PW_T_COMP, 2, 0), ms = add(PW_T_MODSEQ, 0, 1);
        int sb = add(PW_T_SEQ, 0, 2), vco = add(PW_T_VCO, 1, 2), env = add(PW_T_ENV, 1, 3), vcf = add(PW_T_VCF, 2, 2), vca = add(PW_T_VCA, 3, 2);
        int p1 = add(PW_T_VCO, 1, 4), p2 = add(PW_T_VCO, 1, 5), mx = add(PW_T_STEREO, 4, 0), sp = add(PW_T_SPACE, 5, 0), out = add(PW_T_OUTPUT, 6, 0);
        steps(sq, z, 0x5555u); steps(sb, bass, 0x4455u);
        slices(ms, sA);
        cable(sq, 1, smp, 1); cable(ms, 0, smp, 5); cable(smp, 0, cmp, 0); cable(cmp, 0, mx, 0);
        cable(sb, 0, vco, 0); cable(sb, 1, env, 0); cable(vco, 2, vcf, 0); cable(vcf, 0, vca, 0); cable(env, 0, vcf, 1); cable(env, 0, vca, 1); cable(vca, 0, mx, 1);
        cable(p1, 1, mx, 2); cable(p2, 1, mx, 3); cable(mx, 0, sp, 0); cable(mx, 1, sp, 1); cable(sp, 0, out, 0); cable(sp, 1, out, 1);
        set(sq, 0, 16); set(sq, 1, 0.9f); set(sb, 0, 16); set(sb, 1, 0.45f);
        set(smp, 1, 2); set(smp, 12, 8); set(smp, 13, 0); set(smp, 24, 4); set(smp, 14, 0.6f);
        set(cmp, 0, -18); set(cmp, 1, 3); set(cmp, 4, 4);
        set(env, 0, 0.004f); set(env, 1, 0.25f); set(env, 2, 0.0f); set(env, 3, 0.15f);
        set(vcf, 0, 500); set(vcf, 1, 0.3f);
        set(p1, 0, 3); set(p2, 0, 7);
        set(mx, 0, 0.9f); set(mx, 1, 0.6f); set(mx, 2, 0.14f); set(mx, 3, 0.12f);
        set(sp, 0, 0.55f); set(sp, 2, 0.22f); set(out, 0, 0.8f);
        pw_set_bpm(138); pw_set_playing(1);
        grp(1, smp, cmp, -1, -1); grp(2, vco, env, vcf, vca); grp(3, p1, p2, -1, -1); grp(4, sp, -1, -1, -1);
        gbyp(2, 1); snaps_from_now();                                      /* 1: the break straight, and the pad */
        pw_var_select(1); gbyp(2, 0); slices(ms, sB); gates(sq, 0xFFFFu);  /* 2: the bass comes in, the break chopped */
        pw_var_select(2); gbyp(2, 0); slices(ms, sC); gates(sq, 0xFFFFu);  /* 3: stuttered */
        pw_var_select(3); gbyp(2, 0); slices(ms, sB); gates(sq, 0xFFFFu); set(smp, 24, 2); set(mx, 2, 0.2f);   /* 4: double time (BEATS 2) */
        pw_var_select(4); gbyp(2, 0); slices(ms, sD); gates(sq, 0xB6B6u);  /* 5: ghosted: gaps and another order */
        pw_var_select(5); gbyp(2, 0); slices(ms, sE); gates(sq, 0x1111u); set(smp, 24, 8);   /* 6: half time (BEATS 8): a slice every four steps */
        pw_var_select(6); gbyp(1, 1); gbyp(2, 1); set(sp, 2, 0.45f);       /* 7: breakdown: the pad in the room */
        pw_var_select(7); gbyp(2, 0); slices(ms, sC); gates(sq, 0xFFFFu); set(smp, 24, 2); set(mx, 2, 0.2f);   /* 8: the end: double time, stuttered */
        pw_var_select(0);
        perf(smp, 24); perf(smp, 7); perf(smp, 12); perf(vcf, 0); perf(vcf, 1); perf(sp, 0); perf(sp, 2); perf(sq, 1);
        clip(0, 0, 4, 0); clip(1, 4, 4, 1); clip(2, 8, 4, 2); clip(3, 12, 4, 4); clip(4, 16, 4, 3); clip(5, 20, 4, 5); clip(6, 24, 4, 6); clip(7, 28, 4, 7);   /* 32 bars */
        /* lanes (time 0-255 over the 32 bars, a bar is 8): the bass filter; the pad's four chords (steps); the break's filter closing before each drop;
         * its reverse on the last half bar of every eight; its slices: 8, then 16 (every second slice) for two stretches */
        static const uint16_t cut[] = {0, 90, 0,  64, 150, 2,  128, 210, 2,  192, 110, 2,  255, 90, 0};
        static const uint16_t c1[] = {0, 143, 1,  64, 128, 1,  128, 154, 1,  192, 143, 1};
        static const uint16_t c2[] = {0, 165, 1,  64, 154, 1,  128, 181, 1,  192, 165, 1};
        static const uint16_t bf[] = {0, 255, 0,  28, 255, 2,  31, 120, 2,  32, 255, 0,  60, 255, 2,  63, 110, 2,  64, 255, 0,  124, 255, 2,  127, 100, 2,  128, 255, 0,  188, 255, 2,  191, 100, 2,  192, 255, 0};
        static const uint16_t rv[] = {0, 0, 1,  60, 255, 1,  64, 0, 1,  124, 255, 1,  128, 0, 1,  188, 255, 1,  192, 0, 1,  252, 255, 1};
        static const uint16_t sl[] = {0, 109, 1,  96, 255, 1,  128, 109, 1,  160, 255, 1,  192, 109, 1};
        lane(vcf, 0, cut, 5); lane(p1, 0, c1, 4); lane(p2, 0, c2, 4); lane(smp, 20, bf, 12); lane(smp, 7, rv, 8); lane(smp, 12, sl, 5);
    } else {                      /* SONG DUB (48): a slow dub-techno groove built from five groups: the bass (G1), the kick (G2), the offbeat hat (G3), the chord drone (G4: two oscillators whose pitches follow the chords) and the space (G5: the echo). The snapshots bring the groups in; the cutoff, the chords and the echo's feedback move on lanes */
        build_from_recipe("CLOCK SEQ VCO ENV VCF VCA TRIG DRUM DRUM#2 DRUM#3 VCO#2 VCO#3 MIX MIX#2 DELAY OUT|CLOCK.CLK>SEQ.CLOCK CLOCK.CLK>TRIG.CLOCK SEQ.PITCH>VCO.V/OCT SEQ.GATE>ENV.GATE VCO.SAW>VCF.IN VCF.LP>VCA.IN ENV.ENV>VCF.FREQ ENV.ENV>VCA.CV VCA.OUT>MIX.IN_1 TRIG.A>DRUM.TRIG TRIG.B>DRUM#2.TRIG TRIG.C>DRUM#3.TRIG DRUM.OUT>MIX.IN_2 DRUM#2.OUT>MIX.IN_3 DRUM#3.OUT>MIX#2.IN_3 VCO#2.TRI>MIX#2.IN_1 VCO#3.TRI>MIX#2.IN_2 MIX#2.L>DELAY.IN DELAY.MIX>MIX.IN_4 MIX.L>OUT.L MIX.R>OUT.R|@PLAY @SEQ=-24,-24,-24,-24,-19,-24,-24,-24,-24,-24,-24,-24,-17,-24,-19,-24 @TRIG=A:1000100010001000,B:0010001000100010,C:0000000010000000 DRUM.PRESET=1 DRUM#3.PRESET=13 DRUM#2.PRESET=22 DRUM#2.METAL=0 DRUM#2.NOISE=0.9 ENV.DECAY=0.3 ENV.SUSTAIN=0.1 VCF.CUTOFF=220 VCF.RES=0.35 VCO#2.TUNE=3 VCO#3.TUNE=7 DELAY.TIME=0.375 DELAY.FBK=0.5 DELAY.MIX=0.4 MIX.LEVEL_1=0.55 MIX.LEVEL_2=0.5 MIX.LEVEL_3=0.1 MIX.LEVEL_4=0.7 MIX#2.LEVEL_1=0.12 MIX#2.LEVEL_2=0.1 MIX#2.LEVEL_3=0.25");
        pw_set_bpm(122);
        int vcf = find(PW_T_VCF, 0), dly = find(PW_T_DELAY, 0), p1 = find(PW_T_VCO, 1), p2 = find(PW_T_VCO, 2), sq = find(PW_T_SEQ, 0);
        gates(sq, 0x4141);
        grp(1, find(PW_T_VCO, 0), find(PW_T_ENV, 0), vcf, find(PW_T_VCA, 0)); grp(2, find(PW_T_DRUMS, 0), -1, -1, -1); grp(3, find(PW_T_DRUMS, 1), find(PW_T_DRUMS, 2), -1, -1); grp(4, p1, p2, -1, -1); grp(5, dly, -1, -1, -1);
        gbyp(1, 1); snaps_from_now();                                      /* 1: kick, hat, snare and the chord drone, in the echo (no bass yet) */
        pw_var_select(1); gbyp(1, 0);                                      /* 2: the bass (G1) comes in */
        pw_var_select(2); gbyp(1, 0); gbyp(4, 0); set(dly, 1, 0.62f);      /* 3: everything, a longer echo */
        pw_var_select(3); gbyp(1, 1); gbyp(2, 1); gbyp(3, 1); gbyp(4, 0);  /* 4: breakdown: the chords alone in the space */
        pw_var_select(0);
        perf(vcf, 0); perf(vcf, 1); perf(dly, 0); perf(dly, 1); perf(dly, 3); perf(sq, 1);
        clip(0, 0, 4, 0); clip(1, 4, 8, 1); clip(2, 12, 8, 2); clip(3, 20, 4, 3); clip(4, 24, 4, 2);   /* 28 bars */
        static const uint16_t cut[] = {0, 80, 0,  96, 130, 2,  160, 190, 2,  224, 90, 2,  255, 70, 0};
        static const uint16_t fbk[] = {0, 110, 0,  128, 150, 2,  192, 200, 2,  255, 100, 0};
        static const uint16_t c1[] = {0, 138, 1,  32, 145, 1,  64, 138, 1,  96, 145, 1,  128, 138, 1,  160, 145, 1,  192, 138, 1,  224, 145, 1};
        static const uint16_t c2[] = {0, 152, 1,  32, 156, 1,  64, 152, 1,  96, 156, 1,  128, 152, 1,  160, 156, 1,  192, 152, 1,  224, 156, 1};
        lane(vcf, 0, cut, 5); lane(dly, 1, fbk, 4); lane(p1, 0, c1, 8); lane(p2, 0, c2, 8);
    }
    pws.song.on = 1;   /* (the recipe cleared the song: a new patch starts with it off) */
}
/* ---- the eight patches (49-56) that show the newer modules: BURST, INERTIA, SNAPSHOT, CZ, MARKOV, XFADE and the VCO's DRIFT and ANALOG.
 * Each is a recipe (like the module examples: modules | cables | settings), then decorate gives it a performance page and snapshots. */
static const char *const new_recipes[8] = {
    /* 48 BOUNCE: one Euclid hit a bar drops a tom as a bouncing ball: BURST tightens the hits, LEVEL fades them, PITCH steps them down */
    "CLOCK EUCL BURST DRUM VERB OUT|CLOCK.CLK>EUCL.CLOCK EUCL.TRIG>BURST.TRIG BURST.TRIG>DRUM.TRIG BURST.LEVEL>DRUM.ACCENT BURST.PITCH>DRUM.PITCH DRUM.OUT>VERB.IN_L DRUM.OUT>VERB.IN_R VERB.L>OUT.L VERB.R>OUT.R|@PLAY EUCL.STEPS=16 EUCL.HITS=1 BURST.COUNT=9 BURST.SPEED=9 BURST.CURVE=0.85 BURST.PITCH=-2 DRUM.PRESET=17 VERB.MIX=0.2 OUT.LEVEL=0.7",
    /* 49 DRUM ROLLS: a kick and a hat on a grid; the snare is a BURST roll, once a bar, shaken a little by JITTER */
    "CLOCK TRIG BURST DRUM DRUM#2 DRUM#3 MIX OUT|CLOCK.CLK>TRIG.CLOCK TRIG.A>DRUM.TRIG TRIG.B>BURST.TRIG BURST.TRIG>DRUM#2.TRIG BURST.LEVEL>DRUM#2.ACCENT TRIG.C>DRUM#3.TRIG DRUM.OUT>MIX.IN_1 DRUM#2.OUT>MIX.IN_2 DRUM#3.OUT>MIX.IN_3 MIX.L>OUT.L MIX.R>OUT.R|@PLAY @TRIG=A:1000100010001000,B:0000000000001000,C:0010001000100010 DRUM.PRESET=1 DRUM#2.PRESET=11 DRUM#3.PRESET=20 BURST.COUNT=10 BURST.SPEED=22 BURST.CURVE=0.4 BURST.JITTER=0.2 MIX.LEVEL_1=0.6 MIX.LEVEL_2=0.5 MIX.LEVEL_3=0.3",
    /* 50 SPRING BASS: a square LFO through INERTIA moves the filter like a weight on a spring under a bass line */
    "CLOCK SEQ VCO ENV VCF VCA LFO INERT OUT|CLOCK.CLK>SEQ.CLOCK SEQ.PITCH>VCO.V/OCT SEQ.GATE>ENV.GATE VCO.SAW>VCF.IN LFO.SQR>INERT.CV INERT.POS>VCF.FREQ VCF.LP>VCA.IN ENV.ENV>VCA.CV VCA.OUT>OUT.L VCA.OUT>OUT.R|@PLAY @SEQ=-24,-24,-12,-24,-17,-24,-12,-5,-24,-24,-12,-24,-19,-24,-14,-12 VCO.ANALOG=0.4 ENV.DECAY=0.25 ENV.SUSTAIN=0.2 VCF.CUTOFF=250 VCF.RES=0.45 VCF.CV_AMT=0.8 LFO.RATE=0.5 LFO.LEVEL=0.35 INERT.FRICTN=0.12 INERT.MASS=0.5",
    /* 51 SNAP DICE: noise through the clock picks one of four snapshots on every bar; a bright square arpeggio through a band-pass, each snapshot a different voicing of it */
    "CLOCK SEQ VCO ENV VCF VCA DELAY SNAPSH NOISE OUT|CLOCK.CLK>SEQ.CLOCK CLOCK.CLK>SNAPSH.CLK NOISE.WHITE>SNAPSH.CV SEQ.PITCH>VCO.V/OCT SEQ.GATE>ENV.GATE VCO.SQR>VCF.IN VCF.BP>VCA.IN ENV.ENV>VCA.CV VCA.OUT>DELAY.IN DELAY.MIX>OUT.L DELAY.MIX>OUT.R|@PLAY @SEQ=0,7,12,7,3,10,15,10,5,12,17,12,7,14,19,14 VCO.PW=0.3 SNAPSH.SCENES=4 SNAPSH.RANGE=1 SNAPSH.CHANGE=2 SNAPSH.MORPH=0.4 ENV.DECAY=0.12 ENV.SUSTAIN=0 VCF.CUTOFF=1400 VCF.RES=0.5 DELAY.TIME=0.25 DELAY.FBK=0.45 DELAY.MIX=0.3",
    /* 52 CZ KEYS: the CZ's knob-built E.PIANO from the keys, a chorus and a short reverb */
    "KEYS CZ MODFX VERB OUT|KEYS.PITCH>CZ.V/OCT KEYS.GATE>CZ.GATE CZ.OUT>MODFX.IN MODFX.L>VERB.IN_L MODFX.R>VERB.IN_R VERB.L>OUT.L VERB.R>OUT.R|MODFX.TYPE=2 CZ.WAVE_1=3 CZ.DCW=0.5 CZ.D.DEC=0.9 CZ.D.SUS=0.2 CZ.A.DEC=1.6 CZ.A.SUS=0.25 VERB.SIZE=0.5 VERB.DECAY=1.8 VERB.MIX=0.2",
    /* 53 CZ BASS: the CZ's knob-built bass on a sequence, a little echo */
    "CLOCK SEQ CZ DELAY OUT|CLOCK.CLK>SEQ.CLOCK SEQ.PITCH>CZ.V/OCT SEQ.GATE>CZ.GATE CZ.OUT>DELAY.IN DELAY.MIX>OUT.L DELAY.MIX>OUT.R|@PLAY @SEQ=-24,-24,-12,-24,-24,-17,-24,-12,-24,-24,-12,-24,-19,-24,-14,-12 CZ.WAVE_1=0 CZ.DCW=0.35 CZ.D.DEC=0.3 CZ.D.SUS=0.15 CZ.A.DEC=0.5 CZ.A.SUS=0.4 CZ.A.REL=0.1 DELAY.TIME=0.375 DELAY.FBK=0.35 DELAY.MIX=0.25",
    /* 54 DRIFT TUNE: MARKOV plays two drifting, scope-style oscillators; XFADE moves from a saw to a square on a weight on a spring */
    "CLOCK MARKV VCO VCO#2 LFO INERT XFADE ENV VCF VCA DELAY VERB OUT|CLOCK.CLK>MARKV.CLOCK MARKV.PITCH>VCO.V/OCT MARKV.PITCH>VCO#2.V/OCT MARKV.GATE>ENV.GATE VCO.SAW>XFADE.A VCO#2.SQR>XFADE.B LFO.SQR>INERT.CV INERT.POS>XFADE.MIX XFADE.OUT>VCF.IN VCF.LP>VCA.IN ENV.ENV>VCF.FREQ ENV.ENV>VCA.CV VCA.OUT>DELAY.IN DELAY.MIX>VERB.IN_L DELAY.MIX>VERB.IN_R VERB.L>OUT.L VERB.R>OUT.R|@PLAY MARKV.SCALE=2 VCO.DRIFT=0.6 VCO.ANALOG=0.5 VCO#2.FINE=8 VCO#2.DRIFT=0.6 VCO#2.ANALOG=0.5 LFO.RATE=0.15 LFO.LEVEL=0.5 INERT.FRICTN=0.15 INERT.MASS=0.6 ENV.DECAY=0.25 ENV.SUSTAIN=0.2 VCF.CUTOFF=1200 VCF.RES=0.3 VCF.CV_AMT=0.6 DELAY.TIME=0.375 DELAY.FBK=0.4 DELAY.MIX=0.3 VERB.MIX=0.3 OUT.LEVEL=0.6",
    /* 55 RISING RUNS: each Euclid hit fires a BURST whose PITCH steps up a scale on a short note: a run */
    "CLOCK EUCL BURST QUANT VCO ENV VCA DELAY VERB OUT|CLOCK.CLK>EUCL.CLOCK EUCL.TRIG>BURST.TRIG BURST.TRIG>ENV.GATE BURST.PITCH>QUANT.IN BURST.TRIG>QUANT.TRIG QUANT.OUT>VCO.V/OCT VCO.TRI>VCA.IN ENV.ENV>VCA.CV VCA.OUT>DELAY.IN DELAY.MIX>VERB.IN_L DELAY.MIX>VERB.IN_R VERB.L>OUT.L VERB.R>OUT.R|@PLAY EUCL.STEPS=16 EUCL.HITS=3 BURST.COUNT=8 BURST.SPEED=14 BURST.CURVE=0.3 BURST.PITCH=2 QUANT.SCALE=2 ENV.ATTACK=0.002 ENV.DECAY=0.12 ENV.SUSTAIN=0 DELAY.TIME=0.375 DELAY.FBK=0.4 DELAY.MIX=0.3 VERB.MIX=0.3 OUT.LEVEL=0.6",
};
static void decorate(unsigned index)
{
    for (unsigned k = 1; k < PW_VARS; ++k) pw_var_copy(0, k);
    int vcf = find(PW_T_VCF, 0), env = find(PW_T_ENV, 0), dly = find(PW_T_DELAY, 0), rv = find(PW_T_REVERB, 0);
    int seq = find(PW_T_SEQ, 0), lfo = find(PW_T_LFO, 0), vco = find(PW_T_VCO, 0), clk = find(PW_T_CLOCK, 0);
    int smp = find(PW_T_SAMPLER, 0), fm = find(PW_T_FM4, 0), cx = find(PW_T_COMPLEX, 0), dist = find(PW_T_DIST, 0);
    switch (index) {
    case 0:                                       /* BASIC VOICE: 2 pluck, 3 pad, 4 acid */
        perf(vcf, 0); perf(vcf, 1); perf(env, 0); perf(env, 3); perf(vcf, 4); perf(vcf, 2); perf(env, 1); perf(env, 2);
        V(1); set(env, 0, 0.002f); set(env, 1, 0.15f); set(env, 2, 0); set(env, 3, 0.1f); set(vcf, 0, 250); set(vcf, 1, 0.6f); set(vcf, 2, 0.8f);
        V(2); set(env, 0, 0.6f); set(env, 1, 1.0f); set(env, 2, 0.8f); set(env, 3, 1.5f); set(vcf, 0, 900); set(vcf, 1, 0.2f);
        V(3); set(vcf, 4, 2); set(vcf, 0, 300); set(vcf, 1, 0.85f); set(vcf, 2, 0.7f); set(env, 1, 0.2f); set(env, 2, 0.1f);
        break;
    case 1: {                                     /* BERLIN SEQ: 2 up a fourth, 3 another line, 4 open and washed */
        static const int8_t b[16] = {-12, 0, -12, 3, -12, 7, -12, 10, -12, 0, -12, 3, -12, 12, 10, 7};
        perf(vcf, 0); perf(vcf, 1); perf(dly, 1); perf(dly, 3); perf(seq, 1); perf(seq, 3); perf(lfo, 0); perf(env, 1);
        V(1); { int8_t t[16]; for (int i = 0; i < 16; ++i) t[i] = (int8_t)((int8_t)pws.mod[seq].data[i] + 5); notes(seq, t); } set(vcf, 0, 400);
        V(2); notes(seq, b); set(seq, 3, 0.04f); set(vcf, 1, 0.7f);
        V(3); set(vcf, 0, 900); set(vcf, 1, 0.35f); set(dly, 1, 0.75f); set(dly, 3, 0.5f); set(seq, 1, 0.8f);
    } break;
    case 2:                                       /* WEST COAST: 2 brighter, 3 random steps, 4 long and wet */
        perf(cx, 1); perf(cx, 2); perf(cx, 3); perf(find(PW_T_LPG, 0), 1); perf(rv, 3); perf(seq, 2); perf(lfo, 0); perf(find(PW_T_FUNC, 0), 1);
        V(1); set(cx, 1, 2.5f); set(cx, 2, 3.0f); set(cx, 3, 0.4f);
        V(2); set(seq, 2, 3); set(cx, 1, 1.0f);
        V(3); set(find(PW_T_LPG, 0), 1, 1.5f); set(rv, 1, 4.0f); set(rv, 3, 0.15f); set(cx, 0, -24);
        break;
    case 3:                                       /* DRONE: 2 dark, 3 bright and quick, 4 beating */
        perf(vcf, 0); perf(vcf, 1); perf(lfo, 0); perf(rv, 1); perf(find(PW_T_SH, 0), 3); perf(find(PW_T_WAVETABLE, 0), 3); perf(rv, 3); perf(find(PW_T_VCO, 1), 1);
        V(1); set(vcf, 0, 300); set(vcf, 1, 0.7f);
        V(2); set(vcf, 0, 2000); set(lfo, 0, 0.2f); set(find(PW_T_SH, 0), 3, 0.8f);
        V(3); set(find(PW_T_VCO, 1), 1, 30); set(find(PW_T_WAVETABLE, 0), 2, 3);
        break;
    case 4: {                                     /* RANDOM TUNES: 2 minor, 3 quicker, 4 sparse */
        int sh = find(PW_T_SH, 0), q = find(PW_T_QUANT, 0);
        perf(clk, 2); perf(sh, 2); perf(q, 0); perf(dly, 1); perf(rv, 3); perf(env, 1); perf(q, 1); perf(sh, 1);
        V(1); set(q, 0, 8); set(q, 1, 9);
        V(2); set(clk, 2, 1); set(env, 1, 0.2f);
        V(3); set(sh, 2, 0.35f); set(dly, 1, 0.7f); set(rv, 3, 0.45f);
    } break;
    case 5: case 10: case 17:                     /* the drum loop: 2 reversed, 3 half speed (time-stretched), 4 crushed / pitched */
        perf(smp, 2); perf(smp, 7); perf(smp, 9); perf(smp, 8); perf(vcf, 0); perf(vcf, 1); perf(dist, 0); perf(find(PW_T_FRACTURE, 0), 1);
        V(1); set(smp, 7, 1);
        V(2); set(smp, 8, 1); set(smp, 9, 0.5f);
        V(3); set(smp, 2, -5); if (dist >= 0) { set(dist, 1, 3); set(dist, 0, 0.6f); } else set(smp, 9, 2.0f);
        break;
    case 6:                                       /* FM BELLS: 2 glassy, 3 gong, 4 short */
        perf(fm, 1); perf(fm, 0); perf(fm, 6); perf(fm, 2); perf(fm, 4); perf(fm, 5); perf(rv, 3); perf(rv, 1);
        V(1); set(fm, 0, 12); set(fm, 1, 0.45f);
        V(2); set(fm, 0, 4); set(fm, 1, 0.8f); set(fm, 2, 0.35f); set(fm, 6, 1.8f); set(rv, 1, 9.0f);
        V(3); set(fm, 4, 1); set(fm, 5, 11); set(fm, 6, 0.6f); set(rv, 3, 0.2f);
        break;
    case 7: {                                     /* SHIFTER: 2 wider, 3 down, 4 slow swirl */
        int ring = find(PW_T_RING, 0);
        perf(ring, 0); perf(ring, 1); perf(lfo, 0); perf(dly, 1); perf(dly, 0); perf(dly, 3); perf(env, 3); perf(vco, 0);
        V(1); set(ring, 0, 12); set(ring, 1, 80);
        V(2); set(ring, 0, -8); set(dly, 0, 0.33f);
        V(3); set(lfo, 0, 0.03f); set(dly, 1, 0.8f); set(env, 3, 2.0f);
    } break;
    case 8: {                                     /* EUCLID KIT: 2 busier, 3 broken, 4 half time */
        int e1 = find(PW_T_EUCLID, 0), e2 = find(PW_T_EUCLID, 1), e3 = find(PW_T_EUCLID, 2);
        perf(e1, 1); perf(e2, 1); perf(e3, 1); perf(e3, 2); perf(find(PW_T_DRUMS, 0), 3); perf(find(PW_T_DRUMS, 1), 5); perf(find(PW_T_DRUMS, 2), 3); perf(find(PW_T_DRUMS, 0), 0);
        V(1); set(e1, 1, 5); set(e3, 1, 13);
        V(2); set(e1, 0, 13); set(e2, 0, 11); set(e2, 1, 3);
        V(3); set(e1, 1, 2); set(e2, 1, 1); set(e2, 2, 8); set(e3, 1, 6);
    } break;
    case 9:                                       /* FM E.PIANO: 2 bright tines, 3 bell, 4 soft and bowed */
        perf(fm, 1); perf(fm, 0); perf(fm, 5); perf(fm, 4); perf(find(PW_T_CHORUS, 0), 2); perf(rv, 3); perf(fm, 6); perf(fm, 2);
        V(1); set(fm, 1, 0.7f); set(fm, 5, 11);
        V(2); set(fm, 0, 8); set(fm, 4, 4); set(fm, 5, 4); set(fm, 1, 0.5f);
        V(3); set(fm, 1, 0.15f); set(fm, 4, 6); set(fm, 5, 6); set(fm, 6, 1.5f);
        break;
    case 11: {                                    /* TURING PLUCK: 2 locked loop, 3 always changing, 4 low and dark */
        int tm = find(PW_T_TURING, 0), st = find(PW_T_STRING, 0), q = find(PW_T_QUANT, 0);
        perf(tm, 1); perf(tm, 0); perf(tm, 2); perf(st, 1); perf(st, 2); perf(q, 0); perf(rv, 3); perf(clk, 2);
        V(1); set(tm, 1, 0); V(2); set(tm, 1, 0.5f); set(clk, 2, 1);
        V(3); set(st, 0, 0); set(st, 2, 0.25f); set(q, 0, 2);
    } break;
    case 12: {                                    /* ACID 303: 2 squelch up, 3 another line, 4 dry and driven */
        static const int8_t b[16] = {0, 12, 0, 0, 15, 0, 12, 3, 0, 0, 10, 0, 7, 12, 0, -2};
        perf(vcf, 0); perf(vcf, 1); perf(vcf, 2); perf(env, 1); perf(dist, 0); perf(seq, 3); perf(dly, 3); perf(lfo, 1);
        V(1); set(vcf, 1, 0.95f); set(vcf, 2, 0.9f); set(env, 1, 0.3f);
        V(2); notes(seq, b); gates(seq, 0xBDEFu);
        V(3); set(dist, 0, 0.7f); set(dly, 3, 0.05f); set(vcf, 0, 400);
    } break;
    case 13: case 16: {                           /* the drum grid: 2 sixteenth hats and a roll, 3 sparse, 4 swung fill */
        int tr = find(PW_T_TRIGSEQ, 0), rack = find(PW_T_DRUMRACK, 0);
        perf(tr, 1); perf(tr, 8); perf(tr, 10); perf(tr, 11);
        if (rack >= 0) { perf(rack, 6); perf(rack, 14); perf(rack, 18); perf(dly, 3); }
        else { perf(find(PW_T_DRUMS, 0), 3); perf(find(PW_T_DRUMS, 1), 5); perf(find(PW_T_DRUMS, 2), 3); perf(find(PW_T_EQ, 0), 0); }
        V(1); for (unsigned i = 0; i < 16; ++i) pw_trig_set((unsigned)tr, 3, i, i % 4 == 2 ? 2 : 1); pw_trig_set((unsigned)tr, 0, 14, 3); set(tr, 1, 0.15f);
        V(2); for (unsigned i = 0; i < 16; ++i) { pw_trig_set((unsigned)tr, 2, i, i % 4 == 2 ? 1 : 0); pw_trig_set((unsigned)tr, 3, i, 0); } pw_trig_set((unsigned)tr, 0, 10, 1);
        V(3); set(tr, 1, 0.3f); set(tr, 3, 0); for (unsigned i = 12; i < 16; ++i) pw_trig_set((unsigned)tr, 1, i, i == 15 ? 3 : 1);
    } break;
    case 14: {                                    /* VOX CHOIR: 2 open vowels, 3 a fifth up, 4 whisper */
        int form = find(PW_T_FORMANT, 0), ps = find(PW_T_PSHIFT, 0);
        perf(form, 1); perf(form, 2); perf(form, 3); perf(lfo, 0); perf(ps, 0); perf(ps, 3); perf(env, 0); perf(rv, 3);
        V(1); set(form, 2, 0.15f); set(lfo, 1, 0.3f);
        V(2); set(ps, 0, 7); set(ps, 3, 0.5f);
        V(3); set(form, 2, 0.8f); set(form, 3, 7); set(rv, 3, 0.5f);
    } break;
    case 15:                                      /* POLY STRINGS: 2 brass, 3 slow pad, 4 plucked */
        perf(vcf, 0); perf(vcf, 1); perf(env, 0); perf(env, 3); perf(find(PW_T_CHORUS, 0), 2); perf(rv, 3); perf(find(PW_T_VCO, 1), 1); perf(vcf, 2);
        V(1); set(env, 0, 0.05f); set(vcf, 0, 600); set(vcf, 2, 0.6f); set(vcf, 1, 0.35f);
        V(2); set(env, 0, 1.5f); set(env, 3, 3.0f); set(vcf, 0, 500);
        V(3); set(env, 0, 0.002f); set(env, 1, 0.4f); set(env, 2, 0); set(env, 3, 0.4f); set(vcf, 2, 0.7f);
        break;
    case 18:                                      /* SLICE JAM: 2 other slices, 3 reversed and warped, 4 even slices */
        perf(smp, 14); perf(smp, 2); perf(smp, 7); perf(smp, 9); perf(seq, 1); perf(dist, 0); perf(smp, 17); perf(smp, 12);
        V(1); for (int i = 0; i < 16; ++i) pws.mod[seq].data[18 + i] = (uint8_t)((i * 7 % 8) * 32);
        V(2); set(smp, 7, 1); set(smp, 8, 1); set(smp, 9, 0.75f);
        V(3); set(smp, 13, 0); set(smp, 12, 16);
        break;
    case 57:                                      /* BEAT FIT: 2 other slices, 3 reversed, 4 half time (BEATS 8),  */
        perf(smp, 24); perf(smp, 7); perf(smp, 12); perf(smp, 14); perf(seq, 1); perf(find(PW_T_COMP, 0), 0); perf(find(PW_T_REVERB, 0), 3);
        V(1); { int ms = find(PW_T_MODSEQ, 0); if (ms >= 0) for (unsigned i = 0; i < 16; ++i) pw_mseq_set(&pws.mod[ms], 0, i, (uint8_t)((i * 5 % 8) * 32)); }
        V(2); set(smp, 7, 1);
        V(3); set(smp, 24, 8);
        break;
    case 19: {                                    /* MIDI DUO: 2 another line, 3 sharper, 4 octave up */
        static const int8_t b[16] = {0, 3, 7, 10, 12, 10, 7, 3, 0, 3, 7, 12, 15, 12, 7, 3};
        perf(vcf, 0); perf(vcf, 1); perf(env, 1); perf(seq, 1); perf(seq, 3); perf(vcf, 2); perf(vco, 2); perf(find(PW_T_MIDIOUT, 0), 0);
        V(1); notes(seq, b); gates(seq, 0xFFFFu);
        V(2); set(vcf, 1, 0.75f); set(env, 1, 0.12f);
        V(3); set(vco, 0, -12);
    } break;
    case 20:                                      /* FM BASS: 2 growl, 3 rubber, 4 bell bass */
        perf(fm, 1); perf(fm, 2); perf(fm, 0); perf(fm, 5); perf(fm, 6); perf(dist, 0); perf(seq, 1); perf(fm, 4);
        V(1); set(fm, 2, 0.7f); set(fm, 1, 0.85f); set(dist, 0, 0.6f);
        V(2); set(fm, 0, 5); set(fm, 6, 1.6f); set(fm, 1, 0.4f);
        V(3); set(fm, 0, 8); set(fm, 4, 4); set(fm, 5, 4); set(fm, 6, 0.5f);
        break;
    case 21: {                                    /* DUB CHORDS: 2 major sevenths, 3 dark and long, 4 dry stabs */
        int ch = find(PW_T_CHORD, 0);
        perf(vcf, 0); perf(vcf, 1); perf(dly, 1); perf(dly, 3); perf(env, 1); perf(ch, 0); perf(rv, 3); perf(dly, 0);
        V(1); set(ch, 0, 4);
        V(2); set(vcf, 0, 250); set(env, 1, 0.5f); set(dly, 1, 0.85f);
        V(3); set(dly, 3, 0.05f); set(rv, 3, 0.05f); set(vcf, 0, 900);
    } break;
    case 22: {                                    /* NOISE SURF: 2 storm, 3 calm, 4 ripples */
        int ch = find(PW_T_CHAOS, 0), ph = find(PW_T_PHASER, 0), nz = find(PW_T_NOISE, 0);
        perf(lfo, 0); perf(vcf, 0); perf(vcf, 1); perf(ch, 0); perf(ph, 4); perf(rv, 3); perf(nz, 0); perf(ch, 1);
        V(1); set(lfo, 0, 0.2f); set(vcf, 0, 1800); set(ch, 0, 3.0f);
        V(2); set(lfo, 0, 0.05f); set(vcf, 0, 450); set(ph, 4, 0.3f);
        V(3); set(vcf, 1, 0.75f); set(vcf, 0, 1200); set(ph, 3, 0.85f);
    } break;
    case 23: {                                    /* ARP DREAMS: 2 random, 3 down in triplets, 4 three octaves in order */
        int ar = find(PW_T_ARP, 0), fl = find(PW_T_FLANGER, 0);
        perf(ar, 0); perf(ar, 1); perf(ar, 2); perf(vcf, 0); perf(vcf, 1); perf(env, 1); perf(fl, 4); perf(dly, 3);
        V(1); set(ar, 0, 3);
        V(2); set(ar, 0, 1); set(ar, 1, 4); set(vcf, 0, 1600);
        V(3); set(ar, 0, 4); set(ar, 1, 1); set(ar, 2, 3); set(fl, 3, 0.85f);
    } break;
    case 24: {                                    /* FOLD BASS: 2 sine folds, 3 hard, 4 soft and long */
        int fo = find(PW_T_FOLDER, 0), at = find(PW_T_ATTEN, 0), cmp = find(PW_T_COMP, 0);
        perf(fo, 0); perf(fo, 1); perf(at, 0); perf(env, 1); perf(fo, 2); perf(cmp, 0); perf(seq, 1); perf(vco, 0);
        V(1); set(fo, 2, 1);
        V(2); set(fo, 0, 6); set(at, 0, 1.0f); set(fo, 1, 0.3f);
        V(3); set(fo, 0, 1.3f); set(env, 1, 0.6f); set(seq, 1, 0.9f);
    } break;
    case 25: {                                    /* TALK BOX: 2 deep, 3 chattering, 4 high */
        int vw = find(PW_T_VOWEL, 0), sh = find(PW_T_SH, 0), ch = find(PW_T_CHORUS, 0);
        perf(vw, 0); perf(vw, 1); perf(vw, 2); perf(sh, 0); perf(sh, 1); perf(env, 3); perf(ch, 2); perf(rv, 3);
        V(1); set(vw, 1, -7); set(vco, 0, -24);
        V(2); set(sh, 0, 0.0f); set(vw, 2, 0.9f);
        V(3); set(vw, 1, 9); set(vco, 0, 0);
    } break;
    case 26: {                                    /* COIN DRUMS: 2 busier, 3 sparse, 4 boom */
        int co = find(PW_T_COIN, 0), eu = find(PW_T_EUCLID, 0), kick = find(PW_T_DRUMS, 0), sn = find(PW_T_DRUMS, 1);
        int hat = find(PW_T_DRUMS, 2), sm = find(PW_T_STEREO, 0);
        perf(co, 0); perf(eu, 1); perf(eu, 2); perf(clk, 2); perf(kick, 3); perf(sn, 0); perf(hat, 7); perf(sm, 2);
        V(1); set(eu, 1, 11); set(co, 0, 0.3f);
        V(2); set(clk, 2, 4); set(eu, 1, 5);
        V(3); set(kick, 3, 1.0f); set(kick, 1, 40); set(sn, 3, 0.3f);
    } break;
    case 27: {                                    /* BACKWARDS: 2 short chops, 3 long, 4 half dry */
        int re = find(PW_T_REVERSER, 0), wt = find(PW_T_WAVETABLE, 0);
        perf(re, 0); perf(re, 1); perf(re, 2); perf(wt, 3); perf(lfo, 0); perf(env, 0); perf(env, 3); perf(rv, 3);
        V(1); set(re, 0, 0.3f); set(env, 1, 0.25f);
        V(2); set(re, 0, 1.2f); set(re, 1, 0.6f);
        V(3); set(re, 2, 0.5f); set(wt, 2, 2);
    } break;
    case 28: {                                    /* CHAOS PAD: 2 frantic, 3 still, 4 dark */
        int ch = find(PW_T_CHAOS, 0), cm = find(PW_T_CVMIX, 0);
        perf(ch, 0); perf(ch, 1); perf(cm, 0); perf(cm, 1); perf(vcf, 0); perf(vcf, 1); perf(env, 3); perf(rv, 3);
        V(1); set(ch, 0, 8.0f); set(vcf, 1, 0.5f);
        V(2); set(ch, 0, 0.08f); set(cm, 0, 0.2f);
        V(3); set(vcf, 0, 400); set(rv, 1, 12.0f);
    } break;
    case 29: {                                    /* SWITCH SEQ: 2 pendulum, 3 random and fast, 4 higher */
        int sw = find(PW_T_SWITCH, 0), a1 = find(PW_T_ATTEN, 0), a2 = find(PW_T_ATTEN, 1), lpg = find(PW_T_LPG, 0);
        perf(sw, 1); perf(sw, 0); perf(a1, 3); perf(a2, 1); perf(clk, 2); perf(lpg, 1); perf(dly, 1); perf(vco, 0);
        V(1); set(sw, 1, 2);
        V(2); set(sw, 1, 3); set(clk, 2, 1);
        V(3); set(vco, 0, 0); set(a2, 3, 1.25f);
    } break;
    case 30: {                                    /* COMB PLUCK: 2 metal, 3 soft, 4 hollow */
        int cb = find(PW_T_COMB, 0), nz = find(PW_T_NOISE, 0), keys = find(PW_T_KEYS, 0);
        perf(cb, 1); perf(cb, 2); perf(env, 1); perf(nz, 0); perf(rv, 3); perf(rv, 1); perf(cb, 0); perf(keys, 0);
        V(1); set(cb, 2, 0.0f); set(cb, 1, 0.99f);
        V(2); set(env, 1, 0.08f); set(cb, 2, 0.7f);
        V(3); set(cb, 1, -0.95f);
    } break;
    case 31: {                                    /* INPUT FX: 2 robot, 3 dub, 4 jet */
        int ai = find(PW_T_AUDIOIN, 0), cb = find(PW_T_COMB, 0), fl = find(PW_T_FLANGER, 0);
        perf(ai, 0); perf(cb, 0); perf(cb, 1); perf(cb, 3); perf(fl, 4); perf(dly, 0); perf(dly, 1); perf(rv, 3);
        V(1); set(cb, 1, 0.95f); set(cb, 3, 1.0f);
        V(2); set(dly, 1, 0.8f); set(dly, 0, 0.5f); set(dly, 3, 0.5f);
        V(3); set(fl, 0, 0.05f); set(fl, 3, 0.9f); set(fl, 4, 0.8f);
    } break;
    case 32: {                                    /* LPG BONGOS: 2 congas, 3 metal, 4 talking */
        int lpg = find(PW_T_LPG, 0), fo = find(PW_T_FOLDER, 0), sh = find(PW_T_SH, 0), ea = find(PW_T_ENV, 0), rm = find(PW_T_RING, 0), rv = find(PW_T_REVERB, 0);
        perf(cx, 0); perf(cx, 1); perf(cx, 2); perf(ea, 1); perf(fo, 0); perf(lpg, 1); perf(rm, 2); perf(rv, 3);
        V(1); set(cx, 0, -12); set(cx, 1, 1.32f); set(lpg, 1, 0.3f); set(fo, 0, 1.3f); set(ea, 1, 0.3f);
        V(2); set(cx, 1, 3.73f); set(cx, 2, 3.5f); set(fo, 0, 3.5f); set(rm, 2, 1.0f); set(ea, 1, 0.12f);
        V(3); set(sh, 1, 3.0f); set(lpg, 1, 0.22f); set(rv, 3, 0.5f); set(rm, 2, 0.3f);
    } break;
    case 33: {                                    /* GRAIN CLOUD: 2 an octave up, 3 frozen, 4 sparse and long */
        int g = find(PW_T_GRAIN, 0);
        perf(g, 0); perf(g, 1); perf(g, 2); perf(g, 3); perf(g, 4); perf(g, 6); perf(g, 9); perf(rv, 3);
        V(1); set(g, 3, 12); set(g, 9, 0.1f);
        V(2); set(g, 6, 1);
        V(3); set(g, 2, 4); set(g, 1, 0.45f);
    } break;
    case 34: {                                    /* TAPE DUB: 2 worn out, 3 clean (no brake), 4 long echoes */
        int t = find(PW_T_TAPE, 0);
        perf(t, 1); perf(t, 2); perf(t, 3); perf(t, 4); perf(t, 5); perf(dly, 1); perf(vcf, 0); perf(dly, 3);
        V(1); set(t, 3, 0.9f); set(t, 4, 0.4f);
        V(2); set(t, 1, 0); set(t, 2, 0); { int ms = find(PW_T_MODSEQ, 0); if (ms >= 0) pw_mseq_set(&pws.mod[ms], 0, 15, 0); }
        V(3); set(dly, 1, 0.85f); set(dly, 0, 0.5f);
    } break;
    case 35: {                                    /* BELL GARDEN: 2 more notes, 3 glass, 4 drums */
        int e1 = find(PW_T_EUCLID, 0), e2 = find(PW_T_EUCLID, 1), r1 = find(PW_T_STRING, 0), r2 = find(PW_T_STRING, 1), sh = find(PW_T_SH, 0);
        perf(e1, 1); perf(e2, 1); perf(r1, 4); perf(r2, 4); perf(r1, 1); perf(r1, 6); perf(rv, 3); perf(sh, 1);
        V(1); set(e1, 1, 9);
        V(2); set(r1, 4, 2); set(r2, 4, 3);
        V(3); set(r1, 4, 4); set(r2, 4, 4); set(rv, 3, 0.2f);
    } break;
    case 36: {                                    /* MOTION BASS: 2 the motion twice as fast, 3 backwards, 4 slow and deep */
        int c = find(PW_T_CVREC, 0);
        perf(c, 4); perf(c, 5); perf(c, 1); perf(c, 3); perf(vcf, 0); perf(vcf, 1); perf(dist, 0); perf(c, 0);
        V(1); set(c, 4, 2);
        V(2); set(c, 4, 3);
        V(3); set(c, 4, 0); set(c, 5, 1.0f); set(vcf, 1, 0.8f);
    } break;
    case 37: {                                    /* LO-FI KEYS: 2 cleaner, 3 broken, 4 bell tines */
        int t = find(PW_T_TAPE, 0), ch = find(PW_T_CHORUS, 0);
        perf(t, 1); perf(t, 2); perf(t, 3); perf(t, 4); perf(t, 0); perf(fm, 1); perf(fm, 6); perf(ch, 2);
        V(1); set(t, 1, 0.1f); set(t, 2, 0.05f); set(t, 3, 0.2f); set(t, 4, 0);
        V(2); set(t, 1, 0.9f); set(t, 2, 0.6f); set(t, 3, 0.9f);
        V(3); set(fm, 5, 4); set(fm, 0, 8);
    } break;
    case 38: {                                    /* DRUM RACK: 2 BOOM, 3 GLITCH, 4 the bank's drums */
        int rack = find(PW_T_DRUMRACK, 0), tr = find(PW_T_TRIGSEQ, 0), eu = find(PW_T_EUCLID, 0);
        perf(rack, 36); perf(rack, 0); perf(rack, 5); perf(rack, 6); perf(tr, 1); perf(eu, 1); perf(rack, 22); perf(rack, 3);
        V(1); set(rack, 36, 1);
        V(2); set(rack, 36, 5); set(tr, 1, 0.2f);
        V(3); set(rack, 36, 9);
    } break;
    case 39: {                                    /* GLASS PAD: 2 VOX, 3 BELLS, 4 BUZZ */
        int ad = find(PW_T_ADDITIVE, 0);
        perf(ad, 8); perf(ad, 1); perf(ad, 2); perf(ad, 3); perf(ad, 4); perf(ad, 5); perf(ad, 6); perf(ad, 7);
        V(1); set(ad, 8, 3);
        V(2); set(ad, 8, 4);
        V(3); set(ad, 8, 7); set(ad, 2, 0.45f);
    } break;
    case 40: {                                    /* RAIN MACHINE: 2 FM BELL, 3 SWARM, 4 frozen */
        int mo = find(PW_T_MACRO, 0);
        perf(mo, 0); perf(mo, 2); perf(mo, 3); perf(mo, 4); perf(mo, 5); perf(rv, 4); perf(dly, 1); perf(rv, 3);
        V(1); set(mo, 8, 8);
        V(2); set(mo, 0, 5); set(mo, 5, 0.6f);
        V(3); set(rv, 4, 1);
    } break;
    case 41: {                                    /* IDM SHUFFLE: 2 more shuffled, 3 rushing ticks, 4 the METAL kit */
        int sh = find(PW_T_SHUFFLER, 0), cw = find(PW_T_CLOCKWARP, 0), mp = find(PW_T_MICROPERC, 0), rack = find(PW_T_DRUMRACK, 0);
        perf(sh, 0); perf(sh, 2); perf(sh, 4); perf(cw, 2); perf(cw, 1); perf(mp, 0); perf(mp, 4); perf(rack, 36);
        V(1); set(sh, 0, 0.7f); set(sh, 3, -0.4f);
        V(2); set(cw, 2, 0.6f); set(cw, 1, 11);
        V(3); set(rack, 36, 6);
    } break;
    case 42: {                                    /* GATE ECHOES: 2 closer, 3 bells, 4 far */
        int gd = find(PW_T_GATEDLY, 0), r1 = find(PW_T_STRING, 0), r2 = find(PW_T_STRING, 1), r3 = find(PW_T_STRING, 2);
        perf(gd, 0); perf(gd, 1); perf(r1, 7); perf(r2, 7); perf(r3, 7); perf(rv, 3); perf(seq, 1); perf(rv, 1);
        V(1); set(gd, 0, 0.0625f); set(gd, 1, 0.125f);
        V(2); set(r1, 7, 9); set(r2, 7, 4); set(r3, 7, 1);
        V(3); set(gd, 0, 0.375f); set(gd, 1, 0.75f); set(rv, 3, 0.45f);
    } break;
    case 43: {                                    /* MICRO GLITCH: 2 metal, 3 crackle, 4 straight time */
        int m1 = find(PW_T_MICROPERC, 0), m2 = find(PW_T_MICROPERC, 1), cw = find(PW_T_CLOCKWARP, 0), sh = find(PW_T_SHUFFLER, 0);
        perf(m1, 0); perf(m1, 4); perf(m2, 0); perf(m2, 4); perf(cw, 2); perf(cw, 4); perf(sh, 0); perf(rv, 3);
        V(1); set(m1, 0, 4); set(m2, 0, 3);
        V(2); set(m1, 0, 5); set(m1, 2, 0.15f);
        V(3); set(cw, 1, 4); set(cw, 2, 0); set(cw, 4, 0);
    } break;
    case 44: {                                    /* FROZEN BELLS: 2 GLASS, 3 E.PIANO, 4 frozen from the start */
        perf(rv, 4); perf(fm, 7); perf(rv, 1); perf(rv, 3); perf(fm, 0); perf(fm, 1); perf(find(PW_T_CHORUS, 0), 2); perf(rv, 2);
        V(1); set(fm, 7, 9);
        V(2); set(fm, 7, 1);
        V(3); set(rv, 4, 1);
    } break;
    case 49: {                                    /* BOUNCE: 2 slow and spread, 3 a fast flutter, 4 rising */
        int b = find(PW_T_BURST, 0), d = find(PW_T_DRUMS, 0), eu = find(PW_T_EUCLID, 0);
        perf(b, 0); perf(b, 1); perf(b, 2); perf(b, 4); perf(eu, 1); perf(d, 0); perf(d, 3); perf(rv, 3);
        V(1); set(b, 0, 6); set(b, 2, -0.6f); set(b, 1, 5);
        V(2); set(b, 0, 20); set(b, 1, 40); set(b, 2, 0.5f); set(b, 3, 0.4f);
        V(3); set(b, 4, 2); set(b, 2, 0);
    } break;
    case 50: {                                    /* DRUM ROLLS: 2 a long roll that slows, 3 a quick tight one, 4 loose and shaken */
        int b = find(PW_T_BURST, 0), d = find(PW_T_DRUMS, 1), mix = find(PW_T_STEREO, 0);
        perf(b, 0); perf(b, 1); perf(b, 2); perf(b, 3); perf(d, 0); perf(d, 3); perf(mix, 0); perf(mix, 1);
        V(1); set(b, 0, 16); set(b, 2, -0.5f); set(b, 1, 14);
        V(2); set(b, 0, 6); set(b, 1, 40); set(b, 2, 0.8f);
        V(3); set(b, 3, 0.7f); set(b, 0, 12);
    } break;
    case 51: {                                    /* SPRING BASS: 2 loose, 3 stiff and quick, 4 heavy and slow */
        int in_ = find(PW_T_INERTIA, 0);
        perf(vcf, 0); perf(vcf, 1); perf(in_, 0); perf(in_, 1); perf(in_, 2); perf(in_, 3); perf(lfo, 0); perf(env, 1);
        V(1); set(in_, 1, 0.03f); set(in_, 3, 0.8f);
        V(2); set(in_, 2, 0.8f); set(in_, 0, 0.2f); set(lfo, 0, 1.2f);
        V(3); set(in_, 0, 0.9f); set(in_, 2, 0.25f); set(lfo, 0, 0.25f); set(vcf, 0, 180);
    } break;
    case 52: {                                    /* SNAP DICE: the four snapshots the noise picks from: 1 the line, 2 bright, 3 octave up and long echoes, 4 dark */
        perf(vcf, 0); perf(vcf, 1); perf(env, 1); perf(dly, 1); perf(dly, 3); perf(vco, 0); perf(find(PW_T_SNAPSHOT, 0), 2); perf(find(PW_T_SNAPSHOT, 0), 4);
        V(1); set(vcf, 0, 3200); set(vcf, 1, 0.7f); set(env, 1, 0.08f);
        V(2); set(vco, 0, 12); set(dly, 1, 0.6f); set(dly, 3, 0.5f); set(env, 1, 0.25f);
        V(3); set(vcf, 0, 500); set(vco, 0, -12); set(env, 1, 0.3f); set(env, 2, 0.3f);
    } break;
    case 53: {                                    /* CZ KEYS: 2 BRASS, 3 ORGAN, 4 STRINGS */
        int cz = find(PW_T_CZ, 0), ch = find(PW_T_CHORUS, 0);
        perf(cz, 8); perf(cz, 10); perf(cz, 13); perf(cz, 14); perf(cz, 17); perf(cz, 18); perf(ch, 2); perf(rv, 3);
        V(1); set(cz, 8, 4); set(cz, 10, 0.6f); set(cz, 13, 0.6f); set(cz, 14, 0.6f); set(cz, 17, 3.0f); set(cz, 18, 0.8f);
        V(2); set(cz, 8, 1); set(cz, 10, 0.4f); set(cz, 13, 2.0f); set(cz, 14, 0.9f); set(cz, 17, 4.0f); set(cz, 18, 1.0f);
        V(3); set(cz, 8, 0); set(cz, 9, 3); set(cz, 10, 0.5f); set(cz, 13, 1.5f); set(cz, 14, 0.8f); set(cz, 16, 0.3f); set(cz, 18, 0.9f);
    } break;
    case 54: {                                    /* CZ BASS: 2 square, 3 pluck, 4 brass */
        int cz = find(PW_T_CZ, 0);
        perf(cz, 8); perf(cz, 10); perf(cz, 13); perf(cz, 14); perf(cz, 17); perf(cz, 18); perf(dly, 1); perf(dly, 3);
        V(1); set(cz, 8, 1); set(cz, 10, 0.4f); set(cz, 13, 0.4f); set(cz, 17, 0.8f); set(cz, 18, 0.6f);
        V(2); set(cz, 8, 0); set(cz, 10, 0.6f); set(cz, 13, 0.15f); set(cz, 14, 0.05f); set(cz, 17, 0.25f); set(cz, 18, 0.05f);
        V(3); set(cz, 8, 4); set(cz, 10, 0.55f); set(cz, 13, 0.5f); set(cz, 14, 0.7f); set(cz, 17, 1.0f); set(cz, 18, 0.8f);
    } break;
    case 55: {                                    /* DRIFT TUNE: 2 more drift, 3 a stiff spring that snaps between the waves, 4 clean */
        int mk = find(PW_T_MARKOV, 0), in_ = find(PW_T_INERTIA, 0), v2 = find(PW_T_VCO, 1);
        perf(mk, 3); perf(mk, 6); perf(vco, 4); perf(vco, 6); perf(in_, 1); perf(in_, 2); perf(vcf, 0); perf(dly, 3);
        V(1); set(vco, 4, 1); set(v2, 4, 1); set(vco, 6, 0.8f); set(v2, 6, 0.8f);
        V(2); set(in_, 2, 0.9f); set(in_, 1, 0.5f); set(lfo, 0, 0.4f);
        V(3); set(vco, 4, 0); set(v2, 4, 0); set(vco, 6, 0); set(v2, 6, 0);
    } break;
    case 56: {                                    /* RISING RUNS: 2 falling, 3 long and slowing, 4 short and fast */
        int b = find(PW_T_BURST, 0), eu = find(PW_T_EUCLID, 0);
        perf(b, 0); perf(b, 1); perf(b, 2); perf(b, 4); perf(eu, 1); perf(env, 1); perf(dly, 3); perf(rv, 3);
        V(1); set(b, 4, -2);
        V(2); set(b, 0, 16); set(b, 2, -0.6f); set(b, 4, 1);
        V(3); set(b, 0, 5); set(b, 1, 30); set(b, 2, 0.7f);
    } break;
    case 59: {                                    /* CZ ENSEMBLE: 2 PIANO over an electric bass, 3 strings over a slap bass, 4 a square lead over a saw bass */
        static const int8_t lead[16] = {-12, -5, 0, 3, 7, 3, 0, -5, -12, -7, -2, 2, 5, 2, -2, -7};
        static const int8_t bass[16] = {-24, -24, -12, -24, -24, -24, -17, -24, -22, -22, -10, -22, -19, -19, -14, -19};
        int cz = find(PW_T_CZ, 0), c2 = find(PW_T_CZ, 1), ch = find(PW_T_CHORUS, 0), s2 = find(PW_T_SEQ, 1);
        notes(seq, lead); gates(seq, 0xFFFFu); notes(s2, bass); gates(s2, 0x5D5Du);
        perf(cz, 1); perf(c2, 1); perf(cz, 10); perf(dly, 3); perf(rv, 3); perf(ch, 0);
        V(1); set(cz, 1, 2); set(c2, 1, 26);
        V(2); set(cz, 1, 30); set(c2, 1, 28); set(dly, 3, 0.4f);
        V(3); set(cz, 1, 68); set(c2, 1, 66);
    } break;
    case 60: {                                    /* DX ENSEMBLE: 2 MARIMBA over a slap bass, 3 strings over a synth bass, 4 a glockenspiel over an FM bass */
        static const int8_t lead[16] = {-12, -5, 0, 3, 7, 3, 0, -5, -12, -7, -2, 2, 5, 2, -2, -7};
        static const int8_t bass[16] = {-24, -24, -12, -24, -24, -24, -17, -24, -22, -22, -10, -22, -19, -19, -14, -19};
        int dx = find(PW_T_FMDX, 0), d2 = find(PW_T_FMDX, 1), ch = find(PW_T_CHORUS, 0), s2 = find(PW_T_SEQ, 1);
        notes(seq, lead); gates(seq, 0xFFFFu); notes(s2, bass); gates(s2, 0x5D5Du);
        perf(dx, 1); perf(d2, 1); perf(dx, 2); perf(dly, 3); perf(ch, 0);
        V(1); set(dx, 1, 3); set(d2, 1, 11);
        V(2); set(dx, 1, 5); set(d2, 1, 10); set(dly, 3, 0.4f);
        V(3); set(dx, 1, 23); set(d2, 1, 12);
    } break;
    case 61: {                                    /* ROLL CHORDS: 2 a slow pad, 3 plucked, 4 a bright lead */
        static const uint8_t ch[12][3] = {{0, 8, 57}, {0, 8, 60}, {0, 8, 64}, {8, 8, 53}, {8, 8, 57}, {8, 8, 60}, {16, 8, 55}, {16, 8, 60}, {16, 8, 64}, {24, 8, 55}, {24, 8, 59}, {24, 8, 62}};
        static const uint8_t ld[12][3] = {{0, 2, 76}, {2, 2, 72}, {4, 4, 69}, {8, 2, 72}, {10, 2, 69}, {12, 4, 65}, {16, 2, 67}, {18, 2, 72}, {20, 4, 76}, {24, 2, 74}, {26, 2, 71}, {28, 4, 67}};
        int pr = find(PW_T_PROLL, 0);
        if (pr >= 0) { for (int i = 0; i < 12; ++i) pw_roll_add(&pws.mod[pr], ch[i][0], ch[i][1], ch[i][2], 20); for (int i = 0; i < 12; ++i) pw_roll_add(&pws.mod[pr], ld[i][0], ld[i][1], ld[i][2], 28); }
        perf(vcf, 0); perf(vcf, 1); perf(env, 0); perf(env, 3); perf(dly, 3); perf(rv, 3);
        V(1); set(env, 0, 0.9f); set(env, 3, 2.0f); set(vcf, 0, 700);
        V(2); set(env, 0, 0.002f); set(env, 1, 0.35f); set(env, 2, 0); set(env, 3, 0.3f); set(vcf, 0, 2500);
        V(3); set(vcf, 0, 5000); set(vcf, 1, 0.45f); set(rv, 3, 0.4f);
    } break;
    default: break;
    }
    V(0);
}

/* the patches the list no longer shows (they were close to the module examples): EUCLID KIT, TRIG KIT, COMB PLUCK, LO-FI KEYS,
 * FROZEN BELLS, BOUNCE, DRUM ROLLS, and BREAKBEAT, LPG BONGOS, SONG BEATS, CZ KEYS, BEAT FIT. The source below still numbers the patches as before; this maps the list to it. */
static unsigned old_list_of(unsigned new_index)
{
    static const uint8_t gone[12] = {5, 8, 13, 30, 32, 37, 44, 49, 50, 51, 54, 58};
    unsigned i = new_index;
    for (unsigned k = 0; k < 12; ++k) if (gone[k] <= i) ++i;
    return i;
}
int pw_factory_build(unsigned new_index)
{
    if (new_index >= PW_FACTORY_COUNT) return -1;
    unsigned list_index = old_list_of(new_index);
    /* (the list shows the SONG patches together: SONG BEATS, built as 58 below, is listed at 49, the patches after it one later) */
    unsigned index = list_index == 49 ? 58u : (list_index > 49 && list_index <= 58) ? list_index - 1u : list_index;
    pw_clear();
    pw_set_playing(0);
    pw_set_bpm(120);
    switch (index) {
    case 0: { /* Classic subtractive voice played from the keys */
        int keys = add(PW_T_KEYS, 0, 0), vco = add(PW_T_VCO, 1, 0), env = add(PW_T_ENV, 1, 1);
        int vcf = add(PW_T_VCF, 2, 0), vca = add(PW_T_VCA, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        cable(keys, K_PITCH, vco, VCO_VOCT); cable(vco, VCO_SAW, vcf, 0);
        cable(keys, K_GATE, env, 0); cable(env, 0, vcf, 1); cable(env, 0, vca, 1);
        cable(vcf, 0, vca, 0); cable(vca, 0, out, 0);
        set(vcf, 0, 380); set(vcf, 1, 0.35f); set(vcf, 2, 0.55f);
        set(env, 0, 0.004f); set(env, 1, 0.35f); set(env, 2, 0.45f); set(env, 3, 0.35f);
    } break;
    case 1: { /* Sequenced bass with filter sweep and dotted delay */
        static const int8_t line[16] = {-12, -12, 0, -12, 3, -12, 7, -2, -12, -12, 10, -12, 3, 0, -5, -9};
        int clk = add(PW_T_CLOCK, 0, 0), seq = add(PW_T_SEQ, 1, 0), vco = add(PW_T_VCO, 2, 0);
        int env = add(PW_T_ENV, 2, 1), lfo = add(PW_T_LFO, 3, 1), vcf = add(PW_T_VCF, 3, 0);
        int vca = add(PW_T_VCA, 4, 0), dly = add(PW_T_DELAY, 5, 0), out = add(PW_T_OUTPUT, 6, 0);
        cable(clk, 0, seq, 0); cable(seq, 0, vco, VCO_VOCT); cable(seq, 1, env, 0);
        cable(vco, VCO_SAW, vcf, 0); cable(lfo, 1, vcf, 1); cable(env, 0, vcf, 1);
        cable(vcf, 0, vca, 0); cable(env, 0, vca, 1); cable(vca, 0, dly, 0); cable(dly, 0, out, 0);
        steps(seq, line, 0xffff);
        set(seq, 0, 16); set(seq, 1, 0.4f);
        set(vcf, 0, 260); set(vcf, 1, 0.55f); set(vcf, 2, 0.35f);
        set(lfo, 0, 0.08f); set(env, 0, 0.002f); set(env, 1, 0.2f); set(env, 2, 0.1f); set(env, 3, 0.12f);
        set(dly, 0, 0.375f); set(dly, 1, 0.5f); set(dly, 3, 0.3f);
    } break;
    case 2: { /* Complex oscillator into a pinged low pass gate */
        static const int8_t line[16] = {0, 7, 12, 3, 10, 5, 15, 7, 0, 0, 0, 0, 0, 0, 0, 0};
        int seq = add(PW_T_SEQ, 0, 0), lfo = add(PW_T_LFO, 0, 1), cx = add(PW_T_COMPLEX, 1, 0);
        int fn = add(PW_T_FUNC, 1, 1), lpg = add(PW_T_LPG, 2, 0), rv = add(PW_T_REVERB, 3, 0);
        int out = add(PW_T_OUTPUT, 4, 0);
        cable(seq, 0, cx, 0); cable(seq, 2, lpg, 2); cable(seq, 2, fn, 1);
        cable(fn, 0, cx, 1); cable(lfo, 0, cx, 2); cable(cx, 1, lpg, 0);
        cable(lpg, 0, rv, 0); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        steps(seq, line, 0xff);
        set(seq, 0, 8); set(seq, 2, 2);
        set(cx, 0, -12); set(cx, 1, 1.5f); set(cx, 2, 2.0f); set(cx, 3, 0.15f);
        set(fn, 0, 0.004f); set(fn, 1, 0.35f); set(fn, 2, 0.6f);
        set(lfo, 0, 0.11f); set(lfo, 1, 0.6f);
        set(lpg, 1, 0.5f); set(rv, 1, 2.5f); set(rv, 3, 0.08f); set(out, 0, 0.35f);
    } break;
    case 3: { /* Evolving drone, no gate needed */
        int a = add(PW_T_VCO, 0, 0), b = add(PW_T_VCO, 0, 1), wt = add(PW_T_WAVETABLE, 1, 1);
        int sh = add(PW_T_SH, 1, 2), mix = add(PW_T_MIXER, 1, 0), lfo = add(PW_T_LFO, 2, 1);
        int vcf = add(PW_T_VCF, 2, 0), rv = add(PW_T_REVERB, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        cable(a, VCO_SAW, mix, 0); cable(b, VCO_SAW, mix, 1); cable(wt, 0, mix, 2);
        cable(sh, 1, wt, 1); cable(mix, 0, vcf, 0); cable(lfo, 0, vcf, 1);
        cable(vcf, 0, rv, 0); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(a, 0, -24); set(b, 0, -24); set(b, 1, 9); set(wt, 0, -17); set(wt, 2, 1);
        set(sh, 0, 1.0f); set(sh, 1, 5.0f); set(sh, 3, 0.25f);
        set(mix, 0, 0.5f); set(mix, 1, 0.5f); set(mix, 2, 0.7f);
        set(lfo, 0, 0.05f); set(lfo, 1, 0.7f);
        set(vcf, 0, 700); set(vcf, 1, 0.45f); set(vcf, 2, 0.5f);
        set(rv, 0, 1.0f); set(rv, 1, 12.0f); set(rv, 3, 0.55f); set(out, 0, 0.42f);
    } break;
    case 4: { /* Random notes, quantized to a pentatonic scale */
        int clk = add(PW_T_CLOCK, 0, 0), sh = add(PW_T_SH, 1, 0), q = add(PW_T_QUANT, 2, 0);
        int wt = add(PW_T_WAVETABLE, 3, 0), lfo = add(PW_T_LFO, 2, 1), env = add(PW_T_ENV, 3, 1);
        int vca = add(PW_T_VCA, 4, 0), dly = add(PW_T_DELAY, 5, 0), rv = add(PW_T_REVERB, 6, 0);
        int out = add(PW_T_OUTPUT, 7, 0);
        cable(clk, 1, sh, 1); cable(sh, 0, q, 0); cable(q, 0, wt, 0); cable(lfo, 1, wt, 1);
        cable(sh, 2, env, 0); cable(wt, 0, vca, 0); cable(env, 0, vca, 1);
        cable(vca, 0, dly, 0); cable(dly, 0, rv, 0); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(clk, 2, 2); set(sh, 1, 2.0f); set(sh, 2, 0.75f); set(q, 0, 8); set(q, 2, 0);
        set(wt, 2, 2); set(wt, 3, 0.3f); set(lfo, 0, 0.13f); set(lfo, 1, 0.5f); set(lfo, 2, 1);
        set(env, 0, 0.002f); set(env, 1, 0.4f); set(env, 2, 0.0f); set(env, 3, 0.3f);
        set(out, 0, 1.0f); set(dly, 0, 0.25f); set(dly, 1, 0.45f); set(rv, 1, 3.0f); set(rv, 3, 0.25f);
    } break;
    case 5: { /* Synthesized drum loop through crushing and a moving filter */
        int clk = add(PW_T_CLOCK, 0, 0), smp = add(PW_T_SAMPLER, 1, 0), dist = add(PW_T_DIST, 2, 0);
        int lfo = add(PW_T_LFO, 2, 1), vcf = add(PW_T_VCF, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        cable(clk, 1, smp, 1); cable(smp, 0, dist, 0); cable(dist, 0, vcf, 0);
        cable(lfo, 0, vcf, 1); cable(vcf, 0, out, 0);
        set(clk, 2, 16); set(smp, 1, 3); set(dist, 0, 0.3f); set(dist, 2, 0.8f);
        set(lfo, 0, 0.125f); set(lfo, 1, 0.6f); set(vcf, 0, 2500); set(vcf, 1, 0.3f); set(vcf, 2, 0.6f);
    } break;
    case 6: { /* FM bells from the keys: a modulator at a bell ratio, BELL envelopes */
        int keys = add(PW_T_KEYS, 0, 0), fm = add(PW_T_FM4, 1, 0), rv = add(PW_T_REVERB, 2, 0), out = add(PW_T_OUTPUT, 3, 0);
        cable(keys, K_PITCH, fm, 0); cable(keys, K_GATE, fm, 1); cable(keys, K_VEL, fm, 2);
        cable(fm, 0, rv, 0); cable(fm, 0, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(fm, 0, 8); set(fm, 1, 0.55f); set(fm, 2, 0);                /* the modulator at 3.5 times the carrier */
        set(fm, 4, 4); set(fm, 5, 4); set(fm, 6, 1);                    /* BELL, BELL */
        set(rv, 0, 0.8f); set(rv, 1, 5.0f); set(rv, 3, 0.4f); set(out, 0, 0.6f);
    } break;
    case 7: { /* Frequency shifter swirling a gated saw */
        int keys = add(PW_T_KEYS, 0, 0), vco = add(PW_T_VCO, 1, 0), env = add(PW_T_ENV, 1, 1);
        int vca = add(PW_T_VCA, 2, 0), lfo = add(PW_T_LFO, 2, 1), ring = add(PW_T_RING, 3, 0);
        int dly = add(PW_T_DELAY, 4, 0), out = add(PW_T_OUTPUT, 5, 0);
        cable(keys, K_PITCH, vco, VCO_VOCT); cable(keys, K_GATE, env, 0);
        cable(vco, VCO_SAW, vca, 0); cable(env, 0, vca, 1); cable(vca, 0, ring, 0);
        cable(lfo, 0, ring, 2); cable(ring, 1, dly, 0); cable(dly, 0, out, 0);
        set(ring, 0, 3); set(ring, 1, 30); set(lfo, 0, 0.15f);
        set(env, 0, 0.01f); set(env, 2, 0.8f); set(env, 3, 0.8f);
        set(dly, 0, 0.5f); set(dly, 1, 0.6f); set(dly, 3, 0.4f);
    } break;
    case 8: { /* Three Euclidean rhythms playing a drum kit (PLAY starts it) */
        int e1 = add(PW_T_EUCLID, 0, 0), e2 = add(PW_T_EUCLID, 0, 1), e3 = add(PW_T_EUCLID, 0, 2);
        int kick = add(PW_T_DRUMS, 1, 0), snare = add(PW_T_DRUMS, 1, 1), hat = add(PW_T_DRUMS, 1, 2);
        int mix = add(PW_T_STEREO, 2, 1), out = add(PW_T_OUTPUT, 3, 1);
        cable(e1, 1, kick, 0); cable(e2, 1, snare, 0); cable(e3, 1, hat, 0);
        cable(kick, 0, mix, 0); cable(snare, 0, mix, 1); cable(hat, 0, mix, 2);
        cable(mix, 0, out, 0); cable(mix, 1, out, 1);
        set(e1, 1, 4); set(e2, 1, 2); set(e2, 2, 4); set(e3, 1, 11); set(e3, 2, 1);
        /* kick: a sine swept down 2.5 octaves; snare: a triangle and noise; hat: metallic FM and bright noise */
        set(kick, 0, 50); set(kick, 1, 30); set(kick, 2, 0.04f); set(kick, 3, 0.45f); set(kick, 11, 0.25f);
        set(snare, 0, 190); set(snare, 1, 7); set(snare, 2, 0.02f); set(snare, 3, 0.12f); set(snare, 4, 0.5f);
        set(snare, 5, 0.6f); set(snare, 6, 4000); set(snare, 7, 0.18f);
        set(hat, 0, 420); set(hat, 1, 0); set(hat, 3, 0.03f); set(hat, 4, 1); set(hat, 5, 0.85f); set(hat, 6, 9000);
        set(hat, 7, 0.05f); set(hat, 8, 1); set(hat, 9, 2.7f); set(hat, 10, 0.5f);
        set(mix, 0, 1.0f); set(mix, 1, 0.8f); set(mix, 2, 0.7f); set(mix, 5, -0.2f); set(mix, 6, 0.35f);
        set(out, 0, 0.7f);
    } break;
    case 9: { /* An FM electric piano played from the keys */
        int keys = add(PW_T_KEYS, 0, 0), fm = add(PW_T_FM4, 1, 0), ch = add(PW_T_CHORUS, 2, 0);
        int rv = add(PW_T_REVERB, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        cable(keys, K_PITCH, fm, 0); cable(keys, K_GATE, fm, 1); cable(keys, K_VEL, fm, 2);
        cable(fm, 0, ch, 0); cable(ch, 0, rv, 0); cable(ch, 1, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(fm, 0, 3); set(fm, 1, 0.42f); set(fm, 2, 0.1f);             /* 1:1, a little feedback */
        set(fm, 4, 3); set(fm, 5, 1); set(fm, 6, 1.3f);                 /* PIANO for the body, a PLUCK for the tine */
        set(ch, 2, 0.45f); set(rv, 1, 2.5f); set(rv, 3, 0.25f); set(out, 0, 0.7f);
    } break;
    case 10: { /* The drum loop, stuttered and reversed by chance (PLAY starts it) */
        int clk = add(PW_T_CLOCK, 0, 0), smp = add(PW_T_SAMPLER, 1, 0), fr = add(PW_T_FRACTURE, 2, 0);
        int cmp = add(PW_T_COMP, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        cable(clk, 1, smp, 1); cable(smp, 0, fr, 0); cable(fr, 0, cmp, 0); cable(cmp, 0, out, 0);
        set(clk, 2, 16); set(smp, 1, 3);
        set(fr, 0, 3); set(fr, 1, 0.35f); set(fr, 2, 0.3f); set(fr, 3, -2); set(fr, 4, 0.2f);
        set(cmp, 0, -20); set(cmp, 1, 3); set(cmp, 4, 6);
    } break;
    case 11: { /* A slowly changing melody (Turing) plucking a string (PLAY starts it) */
        int clk = add(PW_T_CLOCK, 0, 0), tm = add(PW_T_TURING, 1, 0), q = add(PW_T_QUANT, 2, 0);
        int st = add(PW_T_STRING, 3, 0), ch = add(PW_T_CHORUS, 4, 0), rv = add(PW_T_REVERB, 5, 0);
        int out = add(PW_T_OUTPUT, 6, 0);
        cable(clk, 1, tm, 0); cable(tm, 0, q, 0); cable(q, 0, st, 1); cable(tm, 2, st, 0);
        cable(st, 0, ch, 0); cable(ch, 0, rv, 0); cable(ch, 1, rv, 1);
        cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(clk, 2, 2); set(tm, 0, 8); set(tm, 1, 0.08f); set(tm, 2, 1.5f);
        set(q, 0, 8); set(st, 0, 12); set(st, 1, 0.8f); set(st, 2, 0.55f);
        set(ch, 2, 0.4f); set(rv, 1, 4.0f); set(rv, 3, 0.35f); set(out, 0, 0.8f);
    } break;
    case 12: { /* A 303 line: a sliding sequence through the diode ladder, its envelope and a slow sweep (PLAY) */
        static const int8_t acid[16] = {0, 0, 12, 0, 3, 0, -2, 10, 0, 12, 0, 7, 0, 3, 15, 12};
        int seq = add(PW_T_SEQ, 0, 0), vco = add(PW_T_VCO, 1, 0), env = add(PW_T_ENV, 1, 1), lfo = add(PW_T_LFO, 1, 2);
        int vcf = add(PW_T_VCF, 2, 0), vca = add(PW_T_VCA, 3, 0), dist = add(PW_T_DIST, 4, 0), dly = add(PW_T_DELAY, 5, 0);
        int out = add(PW_T_OUTPUT, 6, 0);
        steps(seq, acid, 0xEB7Bu);
        cable(seq, 0, vco, VCO_VOCT); cable(seq, 1, env, 0); cable(vco, VCO_SAW, vcf, 0);
        cable(env, 0, vcf, 1); cable(lfo, 0, vcf, 1); cable(env, 0, vca, 1);
        cable(vcf, 0, vca, 0); cable(vca, 0, dist, 0); cable(dist, 0, dly, 0); cable(dly, 0, out, 0);
        set(seq, 0, 16); set(seq, 1, 0.55f); set(seq, 3, 0.05f);
        set(vco, 0, -24);
        set(vcf, 4, 2); set(vcf, 0, 260); set(vcf, 1, 0.85f); set(vcf, 2, 0.7f); set(vcf, 3, 0.25f);
        set(env, 0, 0.002f); set(env, 1, 0.22f); set(env, 2, 0.15f); set(env, 3, 0.08f);
        set(lfo, 0, 0.06f); set(lfo, 1, 0.35f);
        set(dist, 0, 0.3f); set(dist, 3, 0.5f);
        set(dly, 0, 0.375f); set(dly, 1, 0.35f); set(dly, 3, 0.22f); set(out, 0, 0.4f);
    } break;
    case 13: { /* The drum grid playing four drum voices, accents and a little swing (PLAY; SEQ shows the grid) */
        int tr = add(PW_T_TRIGSEQ, 0, 1), kick = add(PW_T_DRUMS, 1, 0), snare = add(PW_T_DRUMS, 1, 1);
        int hat = add(PW_T_DRUMS, 1, 2), tom = add(PW_T_DRUMS, 1, 3), eq = add(PW_T_EQ, 2, 0);
        int mix = add(PW_T_STEREO, 3, 1), out = add(PW_T_OUTPUT, 4, 1);
        cable(tr, 0, kick, 0); cable(tr, 1, snare, 0); cable(tr, 2, hat, 0); cable(tr, 3, tom, 0);
        cable(tr, 4, kick, 1); cable(tr, 4, snare, 1);
        cable(kick, 0, eq, 0); cable(eq, 0, mix, 0); cable(snare, 0, mix, 1); cable(hat, 0, mix, 2); cable(tom, 0, mix, 3);
        cable(mix, 0, out, 0); cable(mix, 1, out, 1);
        if (tr >= 0) { pw_trig_set((unsigned)tr, 3, 10, 1); pw_trig_set((unsigned)tr, 3, 11, 2); pw_trig_set((unsigned)tr, 3, 13, 1); }
        set(tr, 1, 0.12f); set(tr, 10, 0.85f);
        set(kick, 0, 50); set(kick, 1, 30); set(kick, 2, 0.04f); set(kick, 3, 0.45f); set(kick, 11, 0.25f);
        set(snare, 0, 190); set(snare, 1, 7); set(snare, 2, 0.02f); set(snare, 3, 0.12f); set(snare, 4, 0.5f);
        set(snare, 5, 0.6f); set(snare, 6, 4000); set(snare, 7, 0.18f);
        set(hat, 0, 420); set(hat, 1, 0); set(hat, 3, 0.03f); set(hat, 4, 1); set(hat, 5, 0.85f); set(hat, 6, 9000);
        set(hat, 7, 0.05f); set(hat, 8, 1); set(hat, 9, 2.7f); set(hat, 10, 0.5f);
        set(tom, 0, 110); set(tom, 1, 12); set(tom, 2, 0.05f); set(tom, 3, 0.28f);
        set(eq, 0, 4); set(eq, 4, 80); set(eq, 1, -5); set(eq, 3, 400);
        set(mix, 0, 1.0f); set(mix, 1, 0.8f); set(mix, 2, 0.6f); set(mix, 3, 0.7f);
        set(mix, 5, -0.2f); set(mix, 6, 0.35f); set(mix, 7, -0.4f);
        set(out, 0, 0.7f);
    } break;
    case 14: { /* A choir from the keys: the formant oscillator's vowels drifting, an octave above it, reverb */
        int keys = add(PW_T_KEYS, 0, 0), form = add(PW_T_FORMANT, 1, 0), lfo = add(PW_T_LFO, 0, 1), env = add(PW_T_ENV, 1, 1);
        int vca = add(PW_T_VCA, 2, 0), ps = add(PW_T_PSHIFT, 3, 0), ch = add(PW_T_CHORUS, 4, 0);
        int rv = add(PW_T_REVERB, 5, 0), out = add(PW_T_OUTPUT, 6, 0);
        cable(keys, K_PITCH, form, 0); cable(lfo, 0, form, 1); cable(keys, K_GATE, env, 0);
        cable(form, 0, vca, 0); cable(env, 0, vca, 1); cable(vca, 0, ps, 0);
        cable(ps, 0, ch, 0); cable(ch, 0, rv, 0); cable(ch, 1, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(form, 0, -12); set(form, 2, 0.3f);
        set(lfo, 0, 0.12f); set(lfo, 1, 0.8f); set(lfo, 2, 1);
        set(env, 0, 0.25f); set(env, 1, 0.5f); set(env, 2, 0.9f); set(env, 3, 1.2f);
        set(ps, 0, 12); set(ps, 2, 0.06f); set(ps, 3, 0.3f);
        set(ch, 2, 0.5f); set(rv, 1, 3.5f); set(rv, 3, 0.35f); set(out, 0, 0.8f);
    } break;
    case 15: { /* Four voices: two detuned saws through the SEM filter, each note its own envelope; chorus, reverb */
        int keys = add(PW_T_KEYS, 0, 0), v1 = add(PW_T_VCO, 1, 0), v2 = add(PW_T_VCO, 1, 1);
        int env = add(PW_T_ENV, 2, 1), vcf = add(PW_T_VCF, 3, 0), vca = add(PW_T_VCA, 4, 0), lfo = add(PW_T_LFO, 3, 1);
        int ch = add(PW_T_CHORUS, 5, 0), rv = add(PW_T_REVERB, 6, 0), out = add(PW_T_OUTPUT, 7, 0);
        cable(keys, K_PITCH, v1, VCO_VOCT); cable(keys, K_PITCH, v2, VCO_VOCT); cable(keys, K_GATE, env, 0);
        cable(v1, VCO_SAW, vcf, 0); cable(v2, VCO_SAW, vcf, 0); cable(env, 0, vcf, 1); cable(lfo, 0, vcf, 1);
        cable(vcf, 0, vca, 0); cable(env, 0, vca, 1); cable(vca, 0, ch, 0); cable(ch, 0, rv, 0); cable(ch, 1, rv, 1);
        cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(v2, 1, 9);
        set(vcf, 4, 0); set(vcf, 0, 900); set(vcf, 1, 0.2f); set(vcf, 2, 0.35f);
        set(env, 0, 0.35f); set(env, 1, 0.8f); set(env, 2, 0.75f); set(env, 3, 1.4f);
        set(lfo, 0, 0.2f); set(lfo, 1, 0.15f);
        set(ch, 2, 0.6f); set(rv, 1, 3.0f); set(rv, 3, 0.3f); set(out, 0, 1.0f);
        pw_set_voices(4);
    } break;
    case 16: { /* The drum grid striking the drum rack's drawn kit; variation 2 busier (GLO + the 2nd lower key) */
        int tr = add(PW_T_TRIGSEQ, 0, 0), rack = add(PW_T_DRUMRACK, 1, 0), cmp = add(PW_T_COMP, 2, 0);
        int dly = add(PW_T_DELAY, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        for (unsigned k = 0; k < 4; ++k) cable(tr, k, rack, k);
        cable(tr, 4, rack, 8); cable(rack, 0, cmp, 0); cable(cmp, 0, out, 0); cable(rack, 3, dly, 0); cable(dly, 1, out, 1);
        cable(rack, 1, out, 1);
        set(tr, 1, 0.08f); set(rack, 4 + 4 * 3, 3);           /* track D: the closed hat */
        set(rack, 4 + 4 * 2, 2); set(rack, 1, 0);
        if (tr >= 0) {
            pw_trig_set((unsigned)tr, 2, 0, 0); pw_trig_set((unsigned)tr, 2, 4, 1); pw_trig_set((unsigned)tr, 2, 12, 1);
            for (unsigned i = 0; i < 16; ++i) pw_trig_set((unsigned)tr, 3, i, i % 2 == 1 ? 1 : 0);
            pw_trig_set((unsigned)tr, 1, 15, 0); pw_trig_set((unsigned)tr, 1, 7, 3);
        }
        set(dly, 0, 0.375f); set(dly, 1, 0.4f); set(dly, 3, 1.0f);
        set(cmp, 0, -16); set(cmp, 1, 4); set(cmp, 4, 4); set(out, 0, 0.75f);

    } break;
    case 17: { /* The drum loop time-stretched: WARP plays it at another speed, its pitch kept (PLAY) */
        int clk = add(PW_T_CLOCK, 0, 0), smp = add(PW_T_SAMPLER, 1, 0), vcf = add(PW_T_VCF, 2, 0), lfo = add(PW_T_LFO, 2, 1);
        /* (no delay: with the sampler's second of audio, the FM-1's pool holds a reverb or a delay, not both) */
        int rv = add(PW_T_REVERB, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        cable(clk, 1, smp, 1); cable(smp, 0, vcf, 0); cable(lfo, 0, vcf, 1);
        cable(vcf, 0, rv, 0); cable(vcf, 0, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(clk, 2, 16); set(smp, 1, 3); set(smp, 8, 1); set(smp, 9, 1.0f); set(smp, 10, 0.05f);
        set(vcf, 0, 6000); set(vcf, 1, 0.2f); set(vcf, 2, 0.4f); set(lfo, 0, 0.1f); set(lfo, 1, 0.5f);
        set(rv, 1, 2.0f); set(rv, 3, 0.15f); set(out, 0, 0.8f);
    } break;
    case 18: { /* The drum loop cut at its hits; a MOD SEQ lane picks which slice plays on each step (PLAY) */
        static const int8_t z[16] = {0};
        int seq = add(PW_T_SEQ, 0, 0), smp = add(PW_T_SAMPLER, 1, 0), dist = add(PW_T_DIST, 2, 0);
        int cmp = add(PW_T_COMP, 3, 0), out = add(PW_T_OUTPUT, 4, 0), ms = add(PW_T_MODSEQ, 0, 1);
        steps(seq, z, 0xBBBBu);
        static const uint8_t lane[16] = {0, 40, 80, 120, 0, 40, 200, 120, 0, 160, 80, 240, 0, 40, 200, 160};
        if (ms >= 0) for (unsigned i = 0; i < 16; ++i) pw_mseq_set(&pws.mod[ms], 0, i, lane[i]);
        cable(seq, 1, smp, 1); cable(ms, 0, smp, 5); cable(smp, 0, dist, 0); cable(dist, 0, cmp, 0); cable(cmp, 0, out, 0);
        set(seq, 0, 16); set(seq, 1, 0.9f);
        set(smp, 1, 2); set(smp, 12, 8); set(smp, 13, 1); set(smp, 14, 0.95f); set(smp, 17, 0.4f); set(smp, 18, 0.0f); set(smp, 19, 0.05f);
        set(dist, 0, 0.2f); set(dist, 3, 0.4f); set(cmp, 0, -18); set(cmp, 1, 4); set(cmp, 4, 5); set(out, 0, 0.8f);
    } break;
    case 57: { /* The drum loop cut into eight slices, BEATS 4: the whole loop is fitted to one bar at the tempo, so the slices
                * stay in time at any BPM; a MOD SEQ lane picks the slice for each step (PLAY) */
        static const int8_t z[16] = {0};
        int seq = add(PW_T_SEQ, 0, 0), smp = add(PW_T_SAMPLER, 1, 0), cmp = add(PW_T_COMP, 2, 0);
        int rv = add(PW_T_REVERB, 3, 0), out = add(PW_T_OUTPUT, 4, 0), ms = add(PW_T_MODSEQ, 0, 1);
        steps(seq, z, 0xFFFFu);
        static const uint8_t lane[16] = {0, 32, 64, 96, 128, 160, 192, 224, 0, 32, 224, 96, 64, 160, 192, 32};
        if (ms >= 0) for (unsigned i = 0; i < 16; ++i) pw_mseq_set(&pws.mod[ms], 0, i, lane[i]);
        cable(seq, 1, smp, 1); cable(ms, 0, smp, 5); cable(smp, 0, cmp, 0); cable(cmp, 0, rv, 0); cable(cmp, 0, rv, 1);
        cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(seq, 0, 16); set(seq, 1, 0.9f);
        set(smp, 1, 2); set(smp, 12, 8); set(smp, 13, 0); set(smp, 24, 4); set(smp, 14, 0.6f);
        set(cmp, 0, -18); set(cmp, 1, 3); set(cmp, 4, 4); set(rv, 1, 1.2f); set(rv, 3, 0.12f); set(out, 0, 0.8f);
        pw_set_bpm(138);
    } break;
    case 19: { /* A bass line here and the same line out to other gear over USB MIDI, with MIDI clock; a controller's
                * CC 74 (MIDI IN) opens the filter (PLAY) */
        static const int8_t line[16] = {0, 0, 12, 0, 7, 0, 10, 12, 0, 0, 3, 0, 5, 7, 3, 2};
        int seq = add(PW_T_SEQ, 0, 0), vco = add(PW_T_VCO, 1, 0), env = add(PW_T_ENV, 1, 1), vcf = add(PW_T_VCF, 2, 0);
        int vca = add(PW_T_VCA, 3, 0), out = add(PW_T_OUTPUT, 4, 0), mo = add(PW_T_MIDIOUT, 1, 2), mi = add(PW_T_MIDIIN, 0, 2);
        steps(seq, line, 0xF7F7u);
        cable(seq, 0, vco, VCO_VOCT); cable(seq, 1, env, 0); cable(vco, VCO_SQR, vcf, 0); cable(env, 0, vcf, 1);
        cable(mi, 1, vcf, 1); cable(vcf, 0, vca, 0); cable(env, 0, vca, 1); cable(vca, 0, out, 0);
        cable(seq, 0, mo, 0); cable(seq, 1, mo, 1);
        set(seq, 0, 16); set(vco, 0, -24); set(vco, 2, 0.3f);
        set(vcf, 4, 1); set(vcf, 0, 220); set(vcf, 1, 0.4f); set(vcf, 2, 0.5f);
        set(env, 0, 0.002f); set(env, 1, 0.25f); set(env, 2, 0.2f); set(env, 3, 0.1f);
        set(mo, 3, 1); set(out, 0, 0.7f);
    } break;
    case 20: { /* A two-operator FM bass on a sequence through drive and a compressor (PLAY) */
        static const int8_t line[16] = {0, 0, 0, 12, 0, 0, 10, 0, 0, 0, 7, 0, 0, 12, 3, 5};
        int seq = add(PW_T_SEQ, 0, 0), fm = add(PW_T_FM4, 1, 0), dist = add(PW_T_DIST, 2, 0);
        int cmp = add(PW_T_COMP, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        steps(seq, line, 0xDB6Du);
        cable(seq, 0, fm, 0); cable(seq, 1, fm, 1); cable(fm, 0, dist, 0); cable(dist, 0, cmp, 0); cable(cmp, 0, out, 0);
        set(seq, 0, 16); set(seq, 1, 0.5f);
        set(fm, 0, 3); set(fm, 1, 0.55f); set(fm, 2, 0.3f); set(fm, 3, -24);   /* 1:1 with feedback, two octaves down */
        set(fm, 4, 3); set(fm, 5, 2); set(fm, 6, 0.75f);                         /* PIANO under a PERC */
        set(dist, 0, 0.35f); set(dist, 3, 0.6f); set(cmp, 0, -16); set(cmp, 1, 4); set(cmp, 4, 4); set(out, 0, 0.7f);
    } break;
    case 21: { /* Offbeat minor-seventh stabs, four oscillators from CHORD, a Moog filter and a dub delay (PLAY) */
        static const int8_t roots[16] = {0, 0, 0, 0, 0, 0, 0, 0, -2, -2, -2, -2, 3, 3, 3, 3};
        int seq = add(PW_T_SEQ, 0, 0), ch = add(PW_T_CHORD, 1, 0), env = add(PW_T_ENV, 1, 1);
        int v1 = add(PW_T_VCO, 2, 0), v2 = add(PW_T_VCO, 2, 1), v3 = add(PW_T_VCO, 2, 2), v4 = add(PW_T_VCO, 2, 3);
        int vcf = add(PW_T_VCF, 3, 0), vca = add(PW_T_VCA, 4, 0), dly = add(PW_T_DELAY, 5, 0), rv = add(PW_T_REVERB, 6, 0);
        int out = add(PW_T_OUTPUT, 7, 0);
        steps(seq, roots, 0x4444u);
        cable(seq, 0, ch, 0); cable(seq, 1, env, 0);
        cable(ch, 0, v1, VCO_VOCT); cable(ch, 1, v2, VCO_VOCT); cable(ch, 2, v3, VCO_VOCT); cable(ch, 3, v4, VCO_VOCT);
        cable(v1, VCO_SAW, vcf, 0); cable(v2, VCO_SAW, vcf, 0); cable(v3, VCO_SAW, vcf, 0); cable(v4, VCO_SAW, vcf, 0);
        cable(env, 0, vcf, 1); cable(vcf, 0, vca, 0); cable(env, 0, vca, 1); cable(vca, 0, dly, 0);
        cable(dly, 0, rv, 0); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(seq, 0, 16); set(seq, 1, 0.3f); set(ch, 0, 5);
        set(v1, 0, -12); set(v2, 0, -12); set(v3, 0, -12); set(v4, 0, -12); set(v2, 1, 6); set(v4, 1, -6);
        set(vcf, 4, 1); set(vcf, 0, 500); set(vcf, 1, 0.3f); set(vcf, 2, 0.45f);
        set(env, 0, 0.002f); set(env, 1, 0.18f); set(env, 2, 0.0f); set(env, 3, 0.15f);
        set(dly, 0, 0.375f); set(dly, 1, 0.7f); set(dly, 2, 0.45f); set(dly, 3, 0.45f);
        set(rv, 1, 3.0f); set(rv, 3, 0.25f); set(out, 0, 0.45f);
    } break;
    case 22: { /* Pink noise as surf: a slow LFO swells it and opens the filter, CHAOS drifts the filter and the phaser */
        int nz = add(PW_T_NOISE, 0, 0), lfo = add(PW_T_LFO, 0, 1), ch = add(PW_T_CHAOS, 1, 1), vca = add(PW_T_VCA, 1, 0);
        int vcf = add(PW_T_VCF, 2, 0), ph = add(PW_T_PHASER, 3, 0), rv = add(PW_T_REVERB, 4, 0), out = add(PW_T_OUTPUT, 5, 0);
        cable(nz, 1, vca, 0); cable(lfo, 1, vca, 1); cable(vca, 0, vcf, 0); cable(lfo, 1, vcf, 1); cable(ch, 0, vcf, 1);
        cable(ch, 1, ph, 1); cable(vcf, 0, ph, 0); cable(ph, 0, rv, 0); cable(ph, 0, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(lfo, 0, 0.09f); set(lfo, 2, 1); set(ch, 0, 0.3f); set(ch, 2, 0.5f);
        set(vcf, 0, 900); set(vcf, 1, 0.2f); set(vcf, 2, 0.6f);
        set(ph, 0, 0.1f); set(ph, 1, 0.7f); set(ph, 4, 0.5f);
        set(rv, 0, 0.9f); set(rv, 1, 6.0f); set(rv, 3, 0.45f); set(out, 0, 0.7f);
    } break;
    case 23: { /* Hold a chord: the ARPEGGIATOR plays it (latched), through a flanger and a delay */
        int ar = add(PW_T_ARP, 0, 0), vco = add(PW_T_VCO, 1, 0), env = add(PW_T_ENV, 1, 1), vcf = add(PW_T_VCF, 2, 0);
        int vca = add(PW_T_VCA, 3, 0), fl = add(PW_T_FLANGER, 4, 0), dly = add(PW_T_DELAY, 5, 0), out = add(PW_T_OUTPUT, 6, 0);
        cable(ar, 0, vco, VCO_VOCT); cable(ar, 1, env, 0); cable(vco, VCO_SAW, vcf, 0); cable(env, 0, vcf, 1);
        cable(vcf, 0, vca, 0); cable(env, 0, vca, 1); cable(vca, 0, fl, 0); cable(fl, 0, dly, 0); cable(dly, 0, out, 0); cable(dly, 0, out, 1);
        set(ar, 0, 2); set(ar, 1, 3); set(ar, 2, 2); set(ar, 3, 0.4f); set(ar, 4, 1);
        set(vcf, 0, 1000); set(vcf, 1, 0.4f); set(vcf, 2, 0.5f);
        set(env, 0, 0.002f); set(env, 1, 0.2f); set(env, 2, 0.35f); set(env, 3, 0.15f);
        set(fl, 0, 0.15f); set(fl, 1, 0.6f); set(fl, 3, 0.5f); set(fl, 4, 0.5f);
        set(dly, 0, 0.375f); set(dly, 1, 0.45f); set(dly, 3, 0.3f); set(out, 0, 1.0f);
    } break;
    case 24: { /* A sine bass through a wavefolder; its envelope, scaled by the ATTENUVERTER, folds each note (PLAY) */
        static const int8_t line[16] = {0, 0, 12, 0, 3, 0, -2, 0, 0, 0, 7, 0, 5, 3, 0, -5};
        int seq = add(PW_T_SEQ, 0, 0), vco = add(PW_T_VCO, 1, 0), env = add(PW_T_ENV, 1, 1), at = add(PW_T_ATTEN, 2, 1);
        int fo = add(PW_T_FOLDER, 2, 0), vca = add(PW_T_VCA, 3, 0), cmp = add(PW_T_COMP, 4, 0), out = add(PW_T_OUTPUT, 5, 0);
        steps(seq, line, 0xBDBDu);
        cable(seq, 0, vco, VCO_VOCT); cable(seq, 1, env, 0); cable(vco, VCO_TRI, fo, 0); cable(env, 0, at, 0); cable(at, 0, fo, 1);
        cable(fo, 0, vca, 0); cable(env, 0, vca, 1); cable(vca, 0, cmp, 0); cable(cmp, 0, out, 0); cable(cmp, 0, out, 1);
        set(seq, 0, 16); set(seq, 1, 0.6f); set(vco, 0, -24);
        set(fo, 0, 2.0f); set(fo, 1, 0.1f); set(at, 0, 0.6f);
        set(env, 0, 0.002f); set(env, 1, 0.25f); set(env, 2, 0.3f); set(env, 3, 0.1f);
        set(cmp, 0, -16); set(cmp, 1, 4); set(cmp, 4, 4); set(out, 0, 0.7f);
    } break;
    case 25: { /* A saw line talking: random vowels (S&H on each step) through the VOWEL FILTER (PLAY) */
        static const int8_t line[16] = {0, 3, 7, 10, 12, 10, 7, 3, 0, 5, 8, 12, 7, 3, 0, -2};
        int seq = add(PW_T_SEQ, 0, 0), vco = add(PW_T_VCO, 1, 0), env = add(PW_T_ENV, 1, 1), sh = add(PW_T_SH, 2, 1);
        int vw = add(PW_T_VOWEL, 2, 0), vca = add(PW_T_VCA, 3, 0), ch = add(PW_T_CHORUS, 4, 0), rv = add(PW_T_REVERB, 5, 0);
        int out = add(PW_T_OUTPUT, 6, 0);
        steps(seq, line, 0xFFFFu);
        cable(seq, 0, vco, VCO_VOCT); cable(seq, 1, env, 0); cable(seq, 2, sh, 1); cable(vco, VCO_SAW, vw, 0); cable(sh, 1, vw, 1);
        cable(vw, 0, vca, 0); cable(env, 0, vca, 1); cable(vca, 0, ch, 0); cable(ch, 0, rv, 0); cable(ch, 1, rv, 1);
        cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(seq, 0, 16); set(seq, 1, 0.8f); set(vco, 0, -12);
        set(sh, 0, 0.08f); set(sh, 1, 4.0f); set(vw, 2, 0.7f);
        set(env, 0, 0.01f); set(env, 1, 0.3f); set(env, 2, 0.6f); set(env, 3, 0.2f);
        set(ch, 2, 0.4f); set(rv, 1, 2.5f); set(rv, 3, 0.25f); set(out, 0, 0.25f);
    } break;
    case 26: { /* A COIN TOSS sends each eighth to the kick or the snare; LOGIC accents kicks that land with the hats (PLAY) */
        int clk = add(PW_T_CLOCK, 0, 0), co = add(PW_T_COIN, 1, 0), eu = add(PW_T_EUCLID, 1, 1), lg = add(PW_T_LOGIC, 2, 1);
        int kick = add(PW_T_DRUMS, 2, 0), sn = add(PW_T_DRUMS, 3, 0), hat = add(PW_T_DRUMS, 3, 1), sm = add(PW_T_STEREO, 4, 0);
        int out = add(PW_T_OUTPUT, 5, 0);
        cable(clk, 1, co, 0); cable(co, 0, kick, 0); cable(co, 1, sn, 0); cable(eu, 1, hat, 0);
        cable(co, 0, lg, 0); cable(eu, 0, lg, 1); cable(lg, 0, kick, 1);
        cable(kick, 0, sm, 0); cable(sn, 0, sm, 1); cable(hat, 0, sm, 2); cable(sm, 0, out, 0); cable(sm, 1, out, 1);
        set(clk, 2, 2); set(co, 0, 0.4f); set(eu, 0, 16); set(eu, 1, 7); set(eu, 3, 0.2f);
        set(sn, 0, 180); set(sn, 1, 12); set(sn, 3, 0.15f); set(sn, 5, 0.7f); set(sn, 6, 2500); set(sn, 7, 0.18f);
        set(hat, 0, 400); set(hat, 1, 0); set(hat, 3, 0.03f); set(hat, 5, 1.0f); set(hat, 6, 9000); set(hat, 7, 0.04f); set(hat, 10, 0.5f);
        set(sm, 0, 0.9f); set(sm, 1, 0.7f); set(sm, 2, 0.5f); set(sm, 5, -0.3f); set(sm, 6, 0.4f); set(out, 0, 0.75f);
    } break;
    case 27: { /* A plucked wavetable note played backwards: the REVERSER turns each piece around, so each note swells up to a cut, into a reverb */
        int keys = add(PW_T_KEYS, 0, 0), lfo = add(PW_T_LFO, 0, 1), wt = add(PW_T_WAVETABLE, 1, 0), env = add(PW_T_ENV, 1, 1);
        int vca = add(PW_T_VCA, 2, 0), re = add(PW_T_REVERSER, 3, 0), rv = add(PW_T_REVERB, 4, 0), out = add(PW_T_OUTPUT, 5, 0);
        cable(keys, K_PITCH, wt, 0); cable(lfo, 1, wt, 1); cable(keys, K_GATE, env, 0); cable(wt, 0, vca, 0); cable(env, 0, vca, 1);
        cable(vca, 0, re, 0); cable(re, 0, rv, 0); cable(re, 0, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(wt, 2, 1); set(wt, 3, 0.3f); set(lfo, 0, 0.07f); set(lfo, 1, 0.4f); set(lfo, 2, 1);
        set(env, 0, 0.003f); set(env, 1, 0.55f); set(env, 2, 0.0f); set(env, 3, 0.4f);   /* a pluck: it dies away, so its backwards is a swell */
        set(re, 0, 0.9f); set(re, 1, 0.25f); set(re, 2, 1.0f);
        set(rv, 0, 0.9f); set(rv, 1, 4.0f); set(rv, 3, 0.25f); set(out, 0, 0.9f);
    } break;
    case 28: { /* A pad that never sits still: CHAOS through a CV MIXER moves its filter and its wavetable */
        int keys = add(PW_T_KEYS, 0, 0), a = add(PW_T_VCO, 1, 0), b = add(PW_T_VCO, 1, 1), wt = add(PW_T_WAVETABLE, 1, 2);
        int ch = add(PW_T_CHAOS, 0, 2), cm = add(PW_T_CVMIX, 2, 2), mix = add(PW_T_MIXER, 2, 0), env = add(PW_T_ENV, 2, 1);
        int vcf = add(PW_T_VCF, 3, 0), vca = add(PW_T_VCA, 4, 0), cho = add(PW_T_CHORUS, 5, 0), rv = add(PW_T_REVERB, 6, 0);
        int out = add(PW_T_OUTPUT, 7, 0);
        cable(keys, K_PITCH, a, VCO_VOCT); cable(keys, K_PITCH, b, VCO_VOCT); cable(keys, K_PITCH, wt, 0); cable(keys, K_GATE, env, 0);
        cable(a, VCO_SAW, mix, 0); cable(b, VCO_SAW, mix, 1); cable(wt, 0, mix, 2);
        cable(ch, 0, cm, 0); cable(ch, 1, cm, 1); cable(cm, 0, vcf, 1); cable(ch, 2, wt, 1); cable(env, 0, vcf, 1);
        cable(mix, 0, vcf, 0); cable(vcf, 0, vca, 0); cable(env, 0, vca, 1); cable(vca, 0, cho, 0);
        cable(cho, 0, rv, 0); cable(cho, 1, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(b, 1, 9); set(wt, 2, 1); set(mix, 0, 0.5f); set(mix, 1, 0.5f); set(mix, 2, 0.6f);
        set(ch, 0, 0.3f); set(ch, 2, 0.6f); set(cm, 0, 0.5f); set(cm, 1, 0.3f);
        set(env, 0, 0.8f); set(env, 1, 1.0f); set(env, 2, 0.8f); set(env, 3, 2.0f);
        set(vcf, 0, 900); set(vcf, 1, 0.3f); set(vcf, 2, 0.5f);
        set(cho, 2, 0.5f); set(rv, 0, 0.85f); set(rv, 1, 6.0f); set(rv, 3, 0.45f); set(out, 0, 0.5f);
        pw_set_voices(3);
    } break;
    case 29: { /* A SEQ SWITCH steps through four voltages (two ATTENUVERTERs as fixed sources), quantized, pinging a gate (PLAY) */
        int clk = add(PW_T_CLOCK, 0, 0), a1 = add(PW_T_ATTEN, 0, 1), a2 = add(PW_T_ATTEN, 0, 2), sw = add(PW_T_SWITCH, 1, 0);
        int q = add(PW_T_QUANT, 2, 0), vco = add(PW_T_VCO, 3, 0), lpg = add(PW_T_LPG, 4, 0), dly = add(PW_T_DELAY, 5, 0);
        int out = add(PW_T_OUTPUT, 6, 0);
        cable(clk, 1, sw, 4); cable(a1, 0, sw, 0); cable(a1, 1, sw, 1); cable(a2, 0, sw, 2); cable(a2, 1, sw, 3);
        cable(sw, 0, q, 0); cable(q, 0, vco, VCO_VOCT); cable(vco, VCO_TRI, lpg, 0); cable(clk, 1, lpg, 2);
        cable(lpg, 0, dly, 0); cable(dly, 0, out, 0); cable(dly, 0, out, 1);
        set(clk, 2, 2); set(a1, 1, 0); set(a1, 3, 0.25f); set(a2, 1, 0.583f); set(a2, 3, 1.0f);
        set(q, 0, 2); set(vco, 0, -12); set(lpg, 1, 0.3f); set(lpg, 3, 0.4f);
        set(dly, 0, 0.375f); set(dly, 1, 0.5f); set(dly, 3, 0.35f); set(out, 0, 0.7f);
    } break;
    case 30: { /* Noise bursts struck through a COMB FILTER tuned by the keys: plucked, metallic, hollow */
        int keys = add(PW_T_KEYS, 0, 0), nz = add(PW_T_NOISE, 0, 1), env = add(PW_T_ENV, 1, 1), vca = add(PW_T_VCA, 1, 0);
        int cb = add(PW_T_COMB, 2, 0), rv = add(PW_T_REVERB, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        cable(nz, 0, vca, 0); cable(keys, K_GATE, env, 0); cable(env, 0, vca, 1); cable(vca, 0, cb, 0); cable(keys, K_PITCH, cb, 1);
        cable(cb, 0, rv, 0); cable(cb, 0, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(env, 4, 1); set(env, 0, 0.001f); set(env, 1, 0.04f);
        set(cb, 0, 261.6f); set(cb, 1, 0.97f); set(cb, 2, 0.35f); set(cb, 3, 1.0f);
        set(rv, 1, 2.5f); set(rv, 3, 0.3f); set(out, 0, 1.0f);
        pw_set_voices(3);
    } break;
    case 31: { /* The computer's sound (AUDIO IN) through a COMB FILTER the keys tune, a flanger, a delay and a reverb */
        int ai = add(PW_T_AUDIOIN, 0, 0), keys = add(PW_T_KEYS, 0, 1), cb = add(PW_T_COMB, 1, 0), fl = add(PW_T_FLANGER, 2, 0);
        int dly = add(PW_T_DELAY, 3, 0), rv = add(PW_T_REVERB, 4, 0), out = add(PW_T_OUTPUT, 5, 0);
        cable(ai, 0, cb, 0); cable(keys, K_PITCH, cb, 1); cable(cb, 0, fl, 0); cable(fl, 0, dly, 0);
        cable(dly, 0, rv, 0); cable(dly, 0, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(cb, 0, 130.8f); set(cb, 1, 0.8f); set(cb, 3, 0.6f);
        set(fl, 0, 0.2f); set(fl, 3, 0.5f); set(fl, 4, 0.4f);
        set(dly, 0, 0.375f); set(dly, 1, 0.4f); set(dly, 3, 0.3f); set(rv, 1, 3.0f); set(rv, 3, 0.3f); set(out, 0, 0.8f);
    } break;
    case 32: { /* Buchla bongos, after Todd Barton and Peter Barata ("Buchla Bongos with Ring Mod and Wave Folding"): two
                * oscillators detuned by an inharmonic ratio (the complex oscillator's pair), ring modulated, FM index and
                * wavefolder both swung by one short envelope (A), then a low pass gate struck by a second, differently
                * decaying one (B) with velocity on both, and a little spring-like reverb. Each hit differs: TIMBRE
                * from a sample and hold, accents from MOD A */
        static const int8_t hits[16] = {0, 0, 7, 0, 12, 0, 7, 5, 0, 7, 0, 12, 7, 0, 5, 7};
        static const uint8_t acc[16] = {90, 0, 30, 0, 60, 0, 20, 45, 80, 0, 30, 0, 50, 0, 25, 70};
        int seq = add(PW_T_SEQ, 0, 0), sh = add(PW_T_SH, 0, 1), ms = add(PW_T_MODSEQ, 1, 1);
        int cx = add(PW_T_COMPLEX, 1, 0), rm = add(PW_T_RING, 2, 0), fold = add(PW_T_FOLDER, 3, 0);
        int ea = add(PW_T_ENV, 2, 1), eb = add(PW_T_ENV, 4, 1), lpg = add(PW_T_LPG, 4, 0);
        int rv = add(PW_T_REVERB, 5, 0), out = add(PW_T_OUTPUT, 6, 0);
        steps(seq, hits, 0xB6D5u);
        if (ms >= 0) for (unsigned i = 0; i < 16; ++i) pw_mseq_set(&pws.mod[ms], 0, i, acc[i]);
        cable(seq, 0, cx, 0); cable(seq, 2, sh, 1); cable(sh, 0, cx, 1);
        cable(cx, 1, rm, 0); cable(cx, 0, rm, 1);
        cable(rm, 0, fold, 0);
        cable(seq, 2, ea, 0); cable(seq, 2, eb, 0); cable(ms, 0, ea, 2); cable(ms, 0, eb, 2);
        cable(ea, 0, cx, 2); cable(ea, 0, fold, 1);
        cable(fold, 0, lpg, 0); cable(eb, 0, lpg, 1);
        cable(lpg, 0, rv, 0); cable(lpg, 0, rv, 1); cable(lpg, 0, out, 0); cable(lpg, 0, out, 1);
        cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(seq, 0, 16); set(seq, 1, 0.2f);
        set(sh, 0, 0); set(sh, 1, 1.0f);
        set(cx, 0, -2); set(cx, 1, 1.47f); set(cx, 2, 1.2f); set(cx, 3, 0.1f);
        set(rm, 0, 0); set(rm, 2, 0.7f);
        set(fold, 0, 1.1f); set(fold, 1, 0.1f); set(fold, 2, 1); set(fold, 3, 0.7f);
        set(ea, 0, 0.002f); set(ea, 1, 0.22f); set(ea, 2, 0); set(ea, 4, 1); set(ea, 10, 0.4f);
        set(eb, 0, 0.001f); set(eb, 1, 0.25f); set(eb, 2, 0); set(eb, 4, 1); set(eb, 7, -0.6f); set(eb, 10, 0.3f);
        set(lpg, 0, 0); set(lpg, 1, 0.35f); set(lpg, 2, 2); set(lpg, 3, 0.35f);
        set(rv, 0, 0.2f); set(rv, 1, 0.8f); set(rv, 2, 0.7f); set(rv, 3, 0.25f);
        set(lpg, 7, 0.8f); set(out, 0, 1.0f);
    } break;
    case 33: { /* A wavetable pad scattered into a cloud of grains, wide and drifting, into a long reverb */
        int keys = add(PW_T_KEYS, 0, 0), lfo = add(PW_T_LFO, 0, 1), wt = add(PW_T_WAVETABLE, 1, 0), env = add(PW_T_ENV, 1, 1);
        int vca = add(PW_T_VCA, 2, 0), g = add(PW_T_GRAIN, 3, 0), rv = add(PW_T_REVERB, 4, 0), out = add(PW_T_OUTPUT, 5, 0);
        cable(keys, K_PITCH, wt, 0); cable(lfo, 1, wt, 1); cable(keys, K_GATE, env, 0); cable(wt, 0, vca, 0); cable(env, 0, vca, 1);
        cable(vca, 0, g, 0); cable(g, 0, rv, 0); cable(g, 1, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(wt, 2, 3); set(wt, 3, 0.4f); set(lfo, 0, 0.06f); set(lfo, 1, 0.5f); set(lfo, 2, 1);
        set(env, 0, 0.2f); set(env, 1, 0.5f); set(env, 2, 0.8f); set(env, 3, 1.5f);
        set(g, 0, 0.25f); set(g, 1, 0.18f); set(g, 2, 22); set(g, 4, 0.6f); set(g, 5, 1.0f); set(g, 7, 0.8f); set(g, 9, 0.25f);
        set(rv, 0, 0.9f); set(rv, 1, 6.0f); set(rv, 3, 0.4f); set(out, 0, 0.45f);
    } break;
    case 34: { /* Offbeat stabs through a dub delay and a tired TAPE machine; the last step's MOD C brakes the tape (PLAY) */
        static const int8_t stabs[16] = {0, 0, 0, 0, 0, 0, -2, 0, 0, 0, 3, 0, 0, 0, -5, 0};
        int seq = add(PW_T_SEQ, 0, 0), vco = add(PW_T_VCO, 1, 0), env = add(PW_T_ENV, 1, 1), vcf = add(PW_T_VCF, 2, 0);
        int vca = add(PW_T_VCA, 3, 0), dly = add(PW_T_DELAY, 4, 0), t = add(PW_T_TAPE, 5, 0), out = add(PW_T_OUTPUT, 6, 0), ms = add(PW_T_MODSEQ, 4, 1);
        steps(seq, stabs, 0x4444u);
        if (ms >= 0) for (unsigned i = 0; i < 16; ++i) pw_mseq_set(&pws.mod[ms], 0, i, i == 15 ? 255 : 0);
        cable(seq, 0, vco, VCO_VOCT); cable(seq, 1, env, 0); cable(vco, VCO_SAW, vcf, 0); cable(env, 0, vcf, 1);
        cable(vcf, 0, vca, 0); cable(env, 0, vca, 1); cable(vca, 0, dly, 0); cable(dly, 0, t, 0); cable(ms, 0, t, 1);
        cable(t, 0, out, 0); cable(t, 0, out, 1);
        set(seq, 0, 16); set(seq, 1, 0.25f);
        set(vcf, 4, 1); set(vcf, 0, 1200); set(vcf, 1, 0.3f); set(vcf, 2, 0.5f);
        set(env, 0, 0.002f); set(env, 1, 0.18f); set(env, 2, 0); set(env, 3, 0.15f);
        set(dly, 0, 0.375f); set(dly, 1, 0.6f); set(dly, 2, 0.4f); set(dly, 3, 0.45f);
        set(t, 0, 0.4f); set(t, 1, 0.35f); set(t, 2, 0.2f); set(t, 3, 0.5f); set(t, 4, 0.15f); set(t, 5, 0.6f);
        set(out, 0, 1.0f);
    } break;
    case 35: { /* Two EUCLIDs strike a BELL and a TUBE, their notes random but pentatonic, panned apart (PLAY) */
        int e1 = add(PW_T_EUCLID, 0, 0), e2 = add(PW_T_EUCLID, 0, 1), sh = add(PW_T_SH, 1, 1), q = add(PW_T_QUANT, 1, 2);
        int r1 = add(PW_T_STRING, 2, 0), r2 = add(PW_T_STRING, 2, 1), sm = add(PW_T_STEREO, 3, 0), rv = add(PW_T_REVERB, 4, 0);
        int out = add(PW_T_OUTPUT, 5, 0);
        cable(e1, 1, r1, 0); cable(e2, 1, r2, 0); cable(e1, 1, sh, 1); cable(sh, 0, q, 0); cable(q, 0, r1, 1); cable(q, 0, r2, 1);
        cable(r1, 0, sm, 0); cable(r2, 0, sm, 1); cable(sm, 0, rv, 0); cable(sm, 1, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(e1, 0, 16); set(e1, 1, 5); set(e2, 0, 13); set(e2, 1, 3); set(e2, 2, 2);
        set(sh, 1, 2.0f); set(q, 0, 7);
        set(r1, 4, 1); set(r1, 1, 0.7f); set(r1, 2, 0.6f); set(r1, 6, 0.3f);
        set(r2, 4, 5); set(r2, 0, -12); set(r2, 1, 0.5f);
        set(sm, 4, -0.4f); set(sm, 5, 0.4f);
        set(rv, 0, 0.85f); set(rv, 1, 5.0f); set(rv, 3, 0.4f); set(out, 0, 0.8f);
    } break;
    case 36: { /* A bass line whose filter moves as a CV RECORDER plays back a recorded sweep (MODE REC: record your own) (PLAY) */
        static const int8_t line[16] = {0, 0, 12, 0, 0, 7, 0, 10, 0, 0, 3, 0, 5, 0, 7, 12};
        int seq = add(PW_T_SEQ, 0, 0), cv = add(PW_T_CVREC, 0, 1), vco = add(PW_T_VCO, 1, 0), env = add(PW_T_ENV, 1, 1);
        int vcf = add(PW_T_VCF, 2, 0), vca = add(PW_T_VCA, 3, 0), dist = add(PW_T_DIST, 4, 0), out = add(PW_T_OUTPUT, 5, 0);
        steps(seq, line, 0xEFBBu);
        if (cv >= 0) cvr_shape(pws.mod[cv].data);   /* the recorded motion */
        cable(seq, 0, vco, VCO_VOCT); cable(seq, 1, env, 0); cable(vco, VCO_SAW, vcf, 0); cable(cv, 0, vcf, 1);
        cable(vcf, 0, vca, 0); cable(env, 0, vca, 1); cable(vca, 0, dist, 0); cable(dist, 0, out, 0); cable(dist, 0, out, 1);
        set(seq, 0, 16); set(seq, 1, 0.5f); set(vco, 0, -24);
        set(cv, 1, 4); set(cv, 3, 0.08f); set(cv, 5, 1.0f);
        set(vcf, 4, 1); set(vcf, 0, 120); set(vcf, 1, 0.78f); set(vcf, 2, 0.7f);
        set(env, 0, 0.002f); set(env, 1, 0.25f); set(env, 2, 0.4f); set(env, 3, 0.1f);
        set(dist, 0, 0.3f); set(out, 0, 0.7f);
    } break;
    case 37: { /* The FM electric piano through a worn TAPE machine: wow, flutter, hiss */
        int keys = add(PW_T_KEYS, 0, 0), fm = add(PW_T_FM4, 1, 0), ch = add(PW_T_CHORUS, 2, 0), t = add(PW_T_TAPE, 3, 0);
        int out = add(PW_T_OUTPUT, 4, 0);
        cable(keys, K_PITCH, fm, 0); cable(keys, K_GATE, fm, 1); cable(keys, K_VEL, fm, 2);
        cable(fm, 0, ch, 0); cable(ch, 0, t, 0); cable(t, 0, out, 0); cable(t, 0, out, 1);
        set(fm, 0, 3); set(fm, 1, 0.42f); set(fm, 2, 0.1f);
        set(fm, 4, 3); set(fm, 5, 1); set(fm, 6, 1.3f);
        set(ch, 2, 0.4f);
        set(t, 0, 0.5f); set(t, 1, 0.4f); set(t, 2, 0.25f); set(t, 3, 0.6f); set(t, 4, 0.3f);
        set(out, 0, 0.8f);
        pw_set_voices(3);
    } break;
    case 38: { /* The DRUM RACK, front and centre: a TRIG SEQ plays its kick, snare, hats and rim, a EUCLID its tom and
                * bell; KIT PUNCH (the variations: BOOM, GLITCH, and the bank's own drums) (PLAY) */
        int tr = add(PW_T_TRIGSEQ, 0, 0), eu = add(PW_T_EUCLID, 0, 1), rack = add(PW_T_DRUMRACK, 1, 0), cmp = add(PW_T_COMP, 2, 0);
        int out = add(PW_T_OUTPUT, 3, 0);
        cable(tr, 0, rack, 0); cable(tr, 1, rack, 1); cable(tr, 2, rack, 3); cable(tr, 3, rack, 4); cable(tr, 4, rack, 8);
        cable(eu, 1, rack, 5); cable(eu, 2, rack, 7);
        cable(rack, 0, cmp, 0); cable(cmp, 0, out, 0); cable(rack, 1, out, 1);
        set(rack, 36, 2); set(rack, 1, 1); set(rack, 0, 0.5f);
        if (tr >= 0) {
            static const char *const pat[4] = {"x..x..x...x..x..", "....x.......x..r", "x.x.x.x.x.x.x.x.", "...........x...."};
            for (unsigned t = 0; t < 4; ++t) for (unsigned i = 0; i < 16; ++i)
                pw_trig_set((unsigned)tr, t, i, pat[t][i] == 'x' ? 1 : pat[t][i] == 'r' ? 3 : 0);
            pw_trig_set((unsigned)tr, 0, 0, 2); pw_trig_set((unsigned)tr, 2, 6, 2);
        }
        set(tr, 1, 0.12f); set(eu, 0, 12); set(eu, 1, 5); set(eu, 2, 2);
        set(cmp, 0, -14); set(cmp, 1, 3); set(cmp, 4, 0); set(out, 0, 0.7f);
    } break;
    case 39: { /* ADDITIVE's GLASS PAD, its resonant peak drifting with an LFO, plucked brighter by every note, in a long reverb */
        int keys = add(PW_T_KEYS, 0, 0), lfo = add(PW_T_LFO, 0, 1), ad = add(PW_T_ADDITIVE, 1, 0), env = add(PW_T_ENV, 1, 1);
        int vl = add(PW_T_VCA, 2, 0), vr = add(PW_T_VCA, 2, 1), rv = add(PW_T_REVERB, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        cable(keys, K_PITCH, ad, 0); cable(keys, K_GATE, ad, 1); cable(lfo, 0, ad, 3); cable(keys, K_GATE, env, 0);
        cable(ad, 0, vl, 0); cable(ad, 1, vr, 0); cable(env, 0, vl, 1); cable(env, 0, vr, 1);
        cable(vl, 0, rv, 0); cable(vr, 0, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(ad, 8, 1); set(ad, 3, 0.35f);
        set(lfo, 0, 0.07f); set(lfo, 1, 0.35f);
        set(env, 0, 0.35f); set(env, 1, 1.0f); set(env, 2, 0.8f); set(env, 3, 2.0f);
        set(rv, 0, 0.9f); set(rv, 1, 6.0f); set(rv, 3, 0.45f); set(out, 0, 0.7f);
        pw_set_voices(3);
    } break;
    case 40: { /* MACRO OSC's PARTICLE model (RAIN) struck by a TURING's notes, quantized, through a delay into a reverb
                * you can FREEZE (perform page) (PLAY) */
        int tu = add(PW_T_TURING, 0, 0), q = add(PW_T_QUANT, 0, 1), mo = add(PW_T_MACRO, 1, 0), lfo = add(PW_T_LFO, 1, 1);
        int dly = add(PW_T_DELAY, 2, 0), rv = add(PW_T_REVERB, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        cable(tu, 0, q, 0); cable(tu, 2, q, 1); cable(q, 0, mo, 0); cable(tu, 2, mo, 1); cable(lfo, 1, mo, 3);
        cable(mo, 0, dly, 0); cable(dly, 0, rv, 0); cable(dly, 0, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(mo, 8, 7); set(mo, 5, 1.2f);
        set(tu, 1, 0.12f); set(tu, 2, 2.0f); set(q, 0, 8); set(q, 2, 0);
        set(lfo, 0, 0.11f); set(lfo, 1, 0.5f);
        set(dly, 0, 0.375f); set(dly, 1, 0.5f); set(dly, 3, 0.35f);
        set(rv, 0, 0.85f); set(rv, 1, 5.0f); set(rv, 3, 0.4f); set(out, 0, 0.8f);
    } break;
    case 41: { /* IDM: a broken beat on the drum rack's GLITCH kit, MICRO PERC ticks on a dragging CLOCK WARP (7 over 8), and
                * the whole beat through a SHUFFLER (PLAY) */
        int tr = add(PW_T_TRIGSEQ, 0, 0), cw = add(PW_T_CLOCKWARP, 0, 1), rack = add(PW_T_DRUMRACK, 1, 0), mp = add(PW_T_MICROPERC, 1, 1);
        int mix = add(PW_T_MIXER, 2, 0), sh = add(PW_T_SHUFFLER, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        cable(tr, 0, rack, 0); cable(tr, 1, rack, 1); cable(tr, 2, rack, 3); cable(tr, 3, rack, 6); cable(tr, 4, rack, 8);
        cable(cw, 0, mp, 0); cable(rack, 0, mix, 0); cable(rack, 1, mix, 1); cable(mp, 0, mix, 2);
        cable(mix, 0, sh, 0); cable(sh, 0, out, 0);
        set(rack, 36, 5); set(rack, 1, 1);
        if (tr >= 0) {
            static const char *const pat[4] = {"x.....x...x.x...", "....x.......x.r.", "x.xxx.x.x.xx..x.", "..x.....x.....x."};
            for (unsigned t = 0; t < 4; ++t) for (unsigned i = 0; i < 16; ++i)
                pw_trig_set((unsigned)tr, t, i, pat[t][i] == 'x' ? 1 : pat[t][i] == 'r' ? 3 : 0);
        }
        set(cw, 0, 8); set(cw, 1, 7); set(cw, 2, -0.45f); set(cw, 4, 0.3f); set(cw, 5, 0.15f);
        set(mp, 0, 0); set(mp, 1, 14); set(mp, 2, 0.03f); set(mp, 3, 0.7f); set(mp, 4, 0.6f); set(mp, 5, 0.6f);
        set(mix, 0, 0.8f); set(mix, 1, 0.8f); set(mix, 2, 0.6f);
        set(sh, 0, 0.35f); set(sh, 1, 8); set(sh, 2, 0.3f); set(sh, 3, 0.2f); set(sh, 4, 0.35f);
        set(out, 0, 0.6f);
    } break;
    case 42: { /* GATE DELAY: each sequenced note strikes a RESONATOR, and twice more, later, two others (A and B taps),
                * panned apart: echoes that are new notes (PLAY) */
        static const int8_t line[16] = {0, 7, 3, 10, 0, 5, 7, 12, 0, 7, 3, 10, -2, 5, 3, 7};
        int seq = add(PW_T_SEQ, 0, 0), gd = add(PW_T_GATEDLY, 0, 1), r1 = add(PW_T_STRING, 1, 0), r2 = add(PW_T_STRING, 1, 1);
        int r3 = add(PW_T_STRING, 1, 2), sm = add(PW_T_STEREO, 2, 0), rv = add(PW_T_REVERB, 3, 0), out = add(PW_T_OUTPUT, 4, 0);
        steps(seq, line, 0x5555u);
        cable(seq, 0, r1, 1); cable(seq, 0, r2, 1); cable(seq, 0, r3, 1);
        cable(seq, 2, r1, 0); cable(seq, 2, gd, 0); cable(gd, 0, r2, 0); cable(gd, 1, r3, 0);
        cable(r1, 0, sm, 0); cable(r2, 0, sm, 1); cable(r3, 0, sm, 2);
        cable(sm, 0, rv, 0); cable(sm, 1, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(seq, 0, 16); set(gd, 0, 0.1875f); set(gd, 1, 0.5f);
        set(r1, 7, 2); set(r2, 7, 1); set(r3, 7, 7); set(r2, 0, 12); set(r3, 0, 7);
        set(sm, 5, -0.7f); set(sm, 6, 0.7f); set(sm, 1, 0.6f); set(sm, 2, 0.5f);
        set(rv, 1, 3.0f); set(rv, 3, 0.3f); set(out, 0, 0.9f);
    } break;
    case 43: { /* Two MICRO PERCs: CLICKs on a drifting CLOCK WARP (5 over 4), ZAPs on a EUCLID, scattered; a SHUFFLER and a
                * small room (PLAY) */
        int cw = add(PW_T_CLOCKWARP, 0, 0), eu = add(PW_T_EUCLID, 0, 1), m1 = add(PW_T_MICROPERC, 1, 0), m2 = add(PW_T_MICROPERC, 1, 1);
        int sm = add(PW_T_STEREO, 2, 0), sh = add(PW_T_SHUFFLER, 3, 0), rv = add(PW_T_REVERB, 4, 0), out = add(PW_T_OUTPUT, 5, 0);
        cable(cw, 0, m1, 0); cable(eu, 1, m2, 0); cable(m1, 0, sm, 0); cable(m2, 0, sm, 1);
        cable(sm, 0, sh, 0); cable(sh, 0, rv, 0); cable(sm, 1, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(cw, 0, 4); set(cw, 1, 5); set(cw, 2, 0.3f); set(cw, 4, 0.5f); set(cw, 5, 0.1f);
        set(eu, 0, 16); set(eu, 1, 7); set(eu, 2, 3);
        set(m1, 0, 0); set(m1, 1, 10); set(m1, 2, 0.05f); set(m1, 3, 0.4f); set(m1, 4, 0.5f);
        set(m2, 0, 1); set(m2, 1, -8); set(m2, 2, 0.12f); set(m2, 3, 0.6f); set(m2, 4, 0.4f); set(m2, 5, 0.6f);
        set(sm, 4, -0.5f); set(sm, 5, 0.5f);
        set(sh, 0, 0.5f); set(sh, 1, 4); set(sh, 2, 0.4f); set(sh, 4, 0.5f);
        set(rv, 0, 0.4f); set(rv, 1, 1.2f); set(rv, 3, 0.25f); set(out, 0, 0.85f);
    } break;
    case 44: { /* FM SYNTH's BELL preset from the keys into a reverb: FREEZE (performance page) holds the chord while you play over it */
        int keys = add(PW_T_KEYS, 0, 0), fm = add(PW_T_FM4, 1, 0), ch = add(PW_T_CHORUS, 2, 0), rv = add(PW_T_REVERB, 3, 0);
        int out = add(PW_T_OUTPUT, 4, 0);
        cable(keys, K_PITCH, fm, 0); cable(keys, K_GATE, fm, 1); cable(keys, K_VEL, fm, 2);
        cable(fm, 0, ch, 0); cable(ch, 0, rv, 0); cable(ch, 1, rv, 1); cable(rv, 0, out, 0); cable(rv, 1, out, 1);
        set(fm, 7, 2); set(ch, 2, 0.35f);
        set(rv, 0, 0.95f); set(rv, 1, 8.0f); set(rv, 2, 0.3f); set(rv, 3, 0.5f); set(out, 0, 0.5f);
        pw_set_voices(3);
    } break;
    case 45: case 46: case 47: case 48: case 58: song_patch(index); break;
    case 49: case 50: case 51: case 52: case 53: case 54: case 55: case 56: if (build_from_recipe(new_recipes[index - 49]) != 0) return -1; break;   /* (a mistake in a recipe stops the generator) */
    case 59: if (build_from_recipe("CLOCK SEQ SEQ#2 CZ CZ#2 MODFX DELAY VERB OUT|CLOCK.CLK>SEQ.CLOCK CLOCK.CLK>SEQ#2.CLOCK SEQ.PITCH>CZ.V/OCT SEQ.GATE>CZ.GATE SEQ#2.PITCH>CZ#2.V/OCT SEQ#2.GATE>CZ#2.GATE CZ.OUT>MODFX.IN MODFX.L>VERB.IN_L MODFX.R>VERB.IN_R CZ.OUT>DELAY.IN DELAY.MIX>VERB.IN_L DELAY.MIX>VERB.IN_R CZ#2.OUT>OUT.L CZ#2.OUT>OUT.R VERB.L>OUT.L VERB.R>OUT.R|@PLAY SEQ.STEPS=16 SEQ#2.STEPS=16 CZ.PRESET=2 CZ.LEVEL=0.4 CZ#2.PRESET=26 CZ#2.LEVEL=0.4 MODFX.TYPE=2 DELAY.TIME=0.375 DELAY.FBK=0.4 DELAY.MIX=0.3 VERB.SIZE=0.5 VERB.DECAY=2.2 VERB.MIX=0.25") != 0) return -1; break;   /* CZ ENSEMBLE: a piano arpeggio over an electric bass, both from INTERNAL,, a chorus, a dotted echo and a room */
    case 60: if (build_from_recipe("CLOCK SEQ SEQ#2 FMDX FMDX#2 MODFX DELAY OUT|CLOCK.CLK>SEQ.CLOCK CLOCK.CLK>SEQ#2.CLOCK SEQ.PITCH>FMDX.V/OCT SEQ.GATE>FMDX.GATE SEQ#2.PITCH>FMDX#2.V/OCT SEQ#2.GATE>FMDX#2.GATE FMDX.OUT>MODFX.IN MODFX.L>OUT.L MODFX.R>OUT.R FMDX.OUT>DELAY.IN DELAY.MIX>OUT.L DELAY.MIX>OUT.R FMDX#2.OUT>OUT.L FMDX#2.OUT>OUT.R|@PLAY SEQ.STEPS=16 SEQ#2.STEPS=16 FMDX.PRESET=3 FMDX.LEVEL=0.5 FMDX#2.PRESET=11 FMDX#2.LEVEL=0.5 MODFX.TYPE=2 DELAY.TIME=0.375 DELAY.FBK=0.4 DELAY.MIX=0.3") != 0) return -1; break;   /* DX ENSEMBLE: an electric piano arpeggio over an FM bass, both from INTERNAL, a chorus, and a dotted echo */
    case 61: if (build_from_recipe("CLOCK ROLL VCO VCO#2 ENV VCF VCA MODFX DELAY VERB OUT|ROLL.PITCH>VCO.V/OCT ROLL.PITCH>VCO#2.V/OCT ROLL.GATE>ENV.GATE VCO.SAW>VCF.IN VCO#2.SAW>VCF.IN ENV.ENV>VCF.FREQ VCF.LP>VCA.IN ENV.ENV>VCA.CV VCA.OUT>MODFX.IN MODFX.L>VERB.IN_L MODFX.R>VERB.IN_R VCA.OUT>DELAY.IN DELAY.MIX>VERB.IN_L DELAY.MIX>VERB.IN_R VERB.L>OUT.L VERB.R>OUT.R|@PLAY ROLL.BARS=2 VCO#2.TUNE=0.12 VCF.CUTOFF=1800 VCF.RES=0.2 ENV.ATTACK=0.03 ENV.DECAY=0.7 ENV.SUSTAIN=0.6 ENV.RELEASE=0.6 MODFX.TYPE=2 DELAY.TIME=0.375 DELAY.FBK=0.35 DELAY.MIX=0.25 VERB.SIZE=0.6 VERB.DECAY=2.5 VERB.MIX=0.3") != 0) return -1; pw_set_voices(4); break;   /* ROLL CHORDS: a four-bar-style loop (Am F C G) of chords with a lead on top, from the piano roll on one cable */
    default: break;
    }
    if ((index < 45 || index >= 49) && index != 58) decorate(index);
    else pw_undo_reset();
    memset(pws.name, 0, sizeof pws.name);
    for (unsigned i = 0; i < 12 && names[new_index][i]; ++i) pws.name[i] = names[new_index][i];
    pw_undo_reset();
    pws.revision++;
    return 0;
}

