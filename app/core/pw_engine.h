/* SPDX-License-Identifier: GPL-3.0-only
 * PATCHWERK — a modular synthesizer firmware for the M-VAVE FM-1.
 *
 * Portable patch engine. The same sources build for the FM-1 (PI32v2),
 * for host tests and for the WebAssembly emulator.
 *
 * Signal convention ("volts"):
 *   audio   +-5 V
 *   pitch   1 V/oct, 0 V = C4 (261.63 Hz)
 *   gates   0 / 5 V, rising edge above 1 V, falling below 0.5 V
 *   CV      0..5 V unipolar or +-5 V bipolar
 */
#ifndef PW_ENGINE_H
#define PW_ENGINE_H
#include <stdint.h>
#include <stddef.h>

#define PW_OUT     128          /* frames per block the platform takes, = FM1_BLOCK_FRAMES */
#define PW_NMAX    128          /* the most frames an engine block holds (buffers are this big) */
/* The engine runs at 44.1 kHz in blocks of 128, or at 22.05 kHz in blocks of 64 (SAMPLE RATE setting: half the work, the
 * output upsampled): pw_sr / pw_srf / pw_isr and the block length are variables, set by pw_set_rate. Outside the core
 * (app, web, tests) PW_N is the platform's block of 128. */
extern int pw_sr, pw_blk;
extern float pw_srf, pw_isr;
#define PW_SR      (pw_sr)
#define PW_SRF     (pw_srf)
#ifdef PW_NVAR
#define PW_N       (pw_blk)
#else
#define PW_N       PW_OUT
#endif

/* Capacity. The device keeps a light footprint; the browser gets more room. */
#if defined(PW_WEB) && !defined(PW_FM1_CAPS)   /* (PW_FM1_CAPS: the emulator with the FM-1's limits, for tests) */
#define PW_MAX_MODULES 64
#define PW_MAX_CABLES  192
#define PW_MAX_OUTBUF  256
#define PW_POOL_BYTES  (12u << 20)
#else
#ifndef PW_MAX_MODULES
#define PW_MAX_MODULES 24
#endif
#ifndef PW_MAX_CABLES
#define PW_MAX_CABLES  64
#endif
#ifndef PW_MAX_OUTBUF
#define PW_MAX_OUTBUF  72   /* (the busiest of the 55 factory patches and 40 random ones uses 50; 24 modules of 3 outputs each is 72) */
#endif
#ifndef PW_POOL_BYTES
#define PW_POOL_BYTES  (144u * 1024u)   /* delay, reverb and sampler memory (and the modules' variations, voices) */
#endif
#endif

#define PW_MAX_IN     10   /* (DRUM RACK: T1-T8, ACC, PRESET) */
#define PW_MAX_OUT    9   /* (MOD SEQ: four lanes, EOS, four triggers) */
#define PW_MAX_PARAM  42
#define PW_MAX_VOICES 4             /* polyphony: the voice area runs this many times at most */
#define PW_VARS       16            /* snapshots per patch (before patch version 9: 8) */
/* SONG: the SNAP lane's clips (val 0-15: the snapshot; 0x80|n: patch slot n is loaded), on bars; len 0 is a free entry.
 * AUTOMATION LANES: each moves one knob along the song: breakpoints (time in sixteenths of a bar, value 0-255 of the knob's
 * range, the shape of the segment after the point). The points of all lanes share one pool, in lane order. */
#define PW_SONG_CLIPS 32
#define PW_SONG_LANES 1
#define PW_LANES      8
#define PW_SUBLANES   6             /* (the saved format still carries a subpatch byte for each old group lane; only SNAP is played or made now, and clips an old patch has on the others are dropped when it loads) */
#define PW_LANE_PTS   128
#define PW_SHAPES     3             /* LINEAR, STEP, SMOOTH */
typedef struct { uint8_t lane, start, len, val; } pw_clip;
typedef struct { uint8_t mod, par, n, spare; } pw_alane;   /* mod 0xff: its module is gone (the lane stays, silent) */
typedef struct { pw_clip clip[PW_SONG_CLIPS]; pw_alane al[PW_LANES]; uint8_t n_lanes, on, glide, sub[PW_SUBLANES]; uint16_t pt_t[PW_LANE_PTS]; uint8_t pt_v[PW_LANE_PTS]; } pw_song;
/* a point's time: pt_t & 0xfff (sixteenths of a bar), its shape: pt_t >> 12 */
#define PW_PERF       8             /* the performance page's knobs */
#define PW_DATA_BYTES 56   /* (before patch version 5: 48) */
#define PW_ROLL_BYTES 128  /* PIANO ROLL: a count, then four bytes a note */
#define PW_DATA_MAX   PW_ROLL_BYTES   /* the most any module stores */
#define PW_STATE_BYTES 264

/* Placement: on the FM-1 every function reached from the audio ISR must live
 * in SRAM (Flash may be busy programming). Hosts ignore these. */
/* On fm1-base the code runs from flash (the platform stops the sound while the
 * flash is written), and big zero-initialised buffers go in its .pool RAM. */
#define PW_DSP
#define PW_DSP_DATA
#if defined(__clang__)
#define PW_COLD __attribute__((minsize))   /* control-rate and rarely heavy code: built for size even in the -O2 files (about a third smaller) */
#else
#define PW_COLD
#endif
#if defined(PW_FM1B) && !defined(PLAT_HOST)
#define PW_POOL __attribute__((section(".pool"), aligned(16)))
#else
#define PW_POOL __attribute__((aligned(16)))
#endif

enum pw_kind { PW_LIN, PW_EXP, PW_ENUM, PW_INT };
enum pw_unit { PW_U_NONE, PW_U_HZ, PW_U_SEC, PW_U_PCT, PW_U_VOLT, PW_U_SEMI,
               PW_U_CENT, PW_U_BPM, PW_U_X, PW_U_HZV, PW_U_DB, PW_U_DIV };
enum pw_cat  { PW_C_SOURCE, PW_C_FILTER, PW_C_FX, PW_C_MOD, PW_C_UTIL,
               PW_C_SEQ, PW_C_IO, PW_C_COUNT };

typedef struct {
    const char *name;           /* <= 7 chars, shown under a knob */
    uint8_t kind, unit;
    float min, max, def;
    const char *const *labels;  /* PW_ENUM */
} pw_param_desc;

struct pw_module;
typedef void (*pw_proc_fn)(struct pw_module *m, const float *const *in,
                           float *const *out, unsigned connected);
typedef void (*pw_init_fn)(struct pw_module *m);

typedef struct {
    const char *name;           /* full name, e.g. "VCF" */
    const char *tag;            /* tile label prefix, <= 6 chars */
    uint8_t cat, n_in, n_out, n_param;
    const char *in[PW_MAX_IN];
    const char *out[PW_MAX_OUT];
    const pw_param_desc *param;  /* n_param of them (<= PW_MAX_PARAM) */
    uint32_t mem_ideal, mem_min; /* pool bytes requested */
    pw_init_fn init;
    pw_proc_fn proc;
    const uint8_t *order;        /* the knob pages' order of the parameters (0: as listed) */
    const char *const *pages;    /* the knob pages' names (0: from the parameters' names) */
} pw_type;
unsigned pw_param_at(unsigned type, unsigned pos);
const uint8_t *pw_cz_tone_name(unsigned bank, unsigned tone);   /* CZ: a bank tone's (1-128) 16 name bytes, or 0 */
int pw_cz_env(unsigned slot, unsigned kind, unsigned line, float secs, float hold, float *out, unsigned n);   /* the CZ's picture: a line's DCW (kind 1) or loudness (2) over time; 0 no tone, 2 both lines the same */
void pw_cz_wave(unsigned slot, float *wave, unsigned nw);   /* one cycle of its wave at its DCW */
int pw_cz_edited(unsigned slot);                  /* a knob has changed the tone (PRESET shows a *) */
extern uint8_t pw_save_kind;   /* 1 while a patch is saved as a GROUP (a flag in its header) */
int pw_insert(const uint8_t *buf, unsigned len, uint64_t *set);   /* a saved patch as a new group in this one; the slots made in `set`; their count, or < 0 */
int pw_cz_embed(unsigned slot, uint8_t *out);   /* the tone a saved patch carries (1 when it must) */
void pw_cz_embed_set(unsigned slot, const uint8_t *tone);
int pw_cz_embedded(unsigned slot);
const uint8_t *pw_cz_embedded_name(unsigned slot);
float pw_cz_norm(unsigned slot, unsigned p);   /* BANK's or PRESET's bar: its place among the banks / tones that are there */
float pw_cz_clamp(unsigned slot, unsigned p, float v);   /* a BANK or PRESET the module can take */
const uint8_t *pw_dx_voice_name(unsigned bank, unsigned voice);   /* FM-DX: a bank voice's (1-128) 16 name bytes, or 0 */
unsigned pw_dx_voices(unsigned bank);             /* FM-DX: how many voices a bank holds (0 INTERNAL, 1-4 the loaded .syx banks) */
int pw_dx_shapes(unsigned slot, unsigned n, float *out, float *secs, float *hold);   /* FM-DX's picture: the four operators' (OP 1-4) loudness, n <= 64 points each in out[4 * n], over *secs with the key down for the first *hold; 0: no voice */
void pw_dx_wave(unsigned slot, float *wave, unsigned nw);   /* one cycle of the voice at full level */
int pw_dx_op(unsigned slot, unsigned op, float *ratio, float *level, unsigned *wave);   /* an operator's ratio, level (0-1) and wave */
unsigned pw_cz_tones(unsigned bank);              /* CZ: how many tones a bank holds (0 INTERNAL, 1 USER, 2-5 the loaded .syx banks) */
const float *pw_modal_ratios(unsigned model);   /* MODAL PERC: a model's eight resonances, as ratios of the pitch (model 1-11) */
/* The modules folded into others in v28 (FLANGER, PHASER, CHORUS, REVERSER, SHUFFLER, CLOCK WARP, AUDIO MIXER) are gone: their type numbers
 * stay taken (PW_T_GONE_*), they are not listed and cannot be added, and a patch that names one loads without it. */
int pw_type_hidden(unsigned type);
#define PW_T_HIDDEN 9                         /* (how many: FLANGER PHASER CHORUS REVERSER SHUFFLER CLOCKWARP MIXER PDOSC FMTX) */
#define PW_T_SHOWN (PW_T_COUNT - PW_T_HIDDEN)   /* the modules the add list offers */                /* an old type: kept so old patches load, not offered to add */    /* the parameter at a knob position (page * 4 + knob) */

enum pw_type_id {
    PW_T_VCO, PW_T_LFO, PW_T_NOISE, PW_T_WAVETABLE, PW_T_COMPLEX, PW_T_VCF,
    PW_T_VCA, PW_T_FOLDER, PW_T_DIST, PW_T_DELAY, PW_T_REVERB, PW_T_RING,
    PW_T_ENV, PW_T_FUNC, PW_T_SH, PW_T_ATTEN, PW_T_GONE_MIXER, PW_T_CVMIX,
    PW_T_LOGIC, PW_T_SWITCH, PW_T_SEQ, PW_T_CLOCK, PW_T_QUANT, PW_T_LPG,
    PW_T_SAMPLER, PW_T_OUTPUT, PW_T_KEYS,
    /* (new types go at the end: a saved patch stores the numbers. PW_T_GONE_*: modules folded into others in v28 and removed in rc15; their
     * numbers stay taken so that no other type moves, and a patch that names one loads without it; FM4 and REVERSER took the places of the rungler
     * and the ladder filter, now the VCF's MOOG type) */
    PW_T_GONE_FLANGER, PW_T_GONE_PHASER, PW_T_GONE_CHORUS, PW_T_FM4, PW_T_GONE_REVERSER, PW_T_DRUMS, PW_T_FRACTURE,
    PW_T_AUDIOIN, PW_T_EUCLID, PW_T_STRING, PW_T_VOWEL, PW_T_COMP, PW_T_ARP, PW_T_TURING, PW_T_STEREO,
    PW_T_CHAOS, PW_T_COIN, PW_T_CHORD, PW_T_FORMANT, PW_T_COMB, PW_T_EQ, PW_T_PSHIFT, PW_T_TRIGSEQ, PW_T_DRUMRACK, PW_T_MIDIIN, PW_T_MIDIOUT,
    PW_T_CVREC, PW_T_GRAIN, PW_T_TAPE, PW_T_SCOPE, PW_T_GATEDLY, PW_T_ADDITIVE, PW_T_MACRO, PW_T_GONE_CLOCKWARP, PW_T_MICROPERC, PW_T_GONE_SHUFFLER, PW_T_HARMTREM, PW_T_ENVFOLLOW,
    PW_T_MODSEQ, PW_T_CHORDSTEPS, PW_T_GRAVITY, PW_T_LISTENER, PW_T_LIMITER, PW_T_OTT, PW_T_SWIRL, PW_T_LOOPER,
    PW_T_GONE_PDOSC, PW_T_MARKOV, PW_T_XFADE, PW_T_SNAPSHOT, PW_T_INERTIA, PW_T_BURST, PW_T_SPACE, PW_T_ROOM, PW_T_GENDY, PW_T_GONE_FMTX, PW_T_CZ, PW_T_BUTTON, PW_T_FMDX, PW_T_MIX8, PW_T_PROLL, PW_T_PARAMMOD, PW_T_COMPARE, PW_T_SHIFTREG, PW_T_WAVESET, PW_T_MUTATE, PW_T_VOCODER, PW_T_COUNT
};

enum { PW_F_POLY = 1, PW_F_BYPASS = 2 };  /* a module's flags: in the voice area (runs once per voice); bypassed */
typedef struct pw_module {
    uint8_t used, type, status, n_out;
    uint8_t flags, nv, group;           /* flags; the group (0 none); the voices its region holds (1, or pws.voices in the voice area) */
    uint16_t omask;                      /* which outputs are patched (bit k: output k), set before each run: modules skip the work of the rest */
    int16_t x, y;                       /* browser patch-bay position */
    uint16_t outbuf[PW_MAX_OUT];
    float p[PW_MAX_PARAM];
    uint8_t data[PW_DATA_MAX];        /* per-type stored data (sequencer steps) */
    uint32_t mem_off, mem_len;          /* the audio memory the module sees (the voice being run's), bytes */
    uint32_t reg_off, reg_len;          /* its whole pool region: audio x nv, the other voices' states, variations */
    uint32_t audio_len;                 /* audio memory per voice */
    uint8_t *stp;                       /* the state being run: st, or a voice's copy */
    uint8_t level[PW_MAX_OUT];          /* output peak meters, 0..255 */
    uint8_t edge[4], edge_hi;           /* CLOCK and CLOCK WARP: pulses counted on their first four outputs (for the page's bar), and which are high */
    uint8_t in_level[PW_MAX_IN];
    union { uint8_t raw[PW_STATE_BYTES]; uint64_t align; } st;
} pw_module;
enum { PW_STATUS_OK, PW_STATUS_NOMEM };

typedef struct { uint8_t used, sm, so, dm, di; int8_t amt; } pw_cable;   /* amt: -100..100 %, what of the signal arrives */

typedef struct {
    uint8_t count, poly_first, poly_count, n_bound;   /* order[poly_first ..] run once per voice */
    uint16_t bound[8];                  /* outputs leaving the voice area: summed over the voices */
    uint8_t order[PW_MAX_MODULES];
    uint16_t in_first[PW_MAX_MODULES][PW_MAX_IN];
    uint8_t in_count[PW_MAX_MODULES][PW_MAX_IN];
    uint16_t omask[PW_MAX_MODULES];      /* the outputs that have a cable */
    uint16_t src[PW_MAX_CABLES];
    int8_t gain[PW_MAX_CABLES];         /* each source's cable amount, % */
} pw_plan;

#define PW_SCOPE 1024
typedef struct {
    pw_module mod[PW_MAX_MODULES];
    pw_cable cable[PW_MAX_CABLES];
    uint16_t cable_born[PW_MAX_CABLES], cable_clock;   /* when each cable was plugged (a count: REC on a jack unplugs the oldest there first); not saved */
    pw_plan plan[2];
    volatile uint32_t plan_cur, plan_ack, blocks;
    float bpm;
    uint8_t playing, start_pending;
    uint8_t metro;                      /* the metronome: a click on every beat (an accent on the bar) while the transport plays; not part of a patch */
    char name[13];
    uint32_t revision;                  /* bumps on every structural/param edit */
    /* transport (audio side) */
    float tick_phase;                   /* 16th-note phase accumulator (no soft-double in the ISR) */
    uint8_t tick_at[PW_NMAX];              /* 1 = global 16th tick at this frame */
    uint8_t reset_now;                  /* transport start this block */
    uint8_t kick_slot, kick_ser;        /* the UI drops GRAVITY's ball by hand */
    uint8_t tmon_slot, tmon_ser, tmon_track, tmon_acc;   /* TRIG SEQ MONITOR: a hit being recorded sounds through its outputs */
    uint8_t mon_on, mon_ser; int8_t mon_note; uint8_t mon_slot;   /* SEQUENCER MONITOR: the key being recorded sounds through it */
    /* keyboard (shared by every KEYS module) */
    volatile uint8_t kq_w, kq_r;
    uint8_t kq[32][3];
    uint8_t held[16], held_vel[16], n_held;
    int8_t last_note;
    float kb_pitch_note, kb_vel, kb_mod, kb_bend, kb_press;   /* (kb_press: the keys' pressure, aftertouch, 0..1) */
    uint8_t kb_gate, kb_trig;           /* trig: new note this block */
    uint8_t aud_on, aud_mod, aud_pad, aud_mix; int8_t aud_note;   /* AUDITION: a source module struck on its own (see pw_audition) */
    uint32_t aud_age, aud_gate, aud_window;
    uint32_t kb_serial;
    /* scope tap */
    volatile uint8_t scope_mod, scope_port;
    uint16_t scope_w;
    int16_t scope[PW_SCOPE];            /* the probe tap, volts x 3000 (as the SCOPE module's ring) */
    float out_peak_l, out_peak_r;
    float clip_gr;                      /* the limiter's gain reduction, held for the display (1: none) */
    uint32_t rng;
    /* polyphony: the voices the keyboard's notes go to */
    uint8_t voices;
    struct { uint8_t note, vel, gate; uint32_t serial, age; } voice[PW_MAX_VOICES];
    uint32_t voice_clock;
    /* variations and the performance page */
    uint8_t var_cur;
    uint8_t key_sens;                   /* the KEYS module's VEL out: 0 fixed, 1 low (a hard hit), 2 normal, 3 high (a light touch is loud); a setting */
    uint8_t clip_on;                    /* CLIP PROTECT is working: a brickwall limiter on the final output (an OUTPUT module's setting; on when there is none) */
    uint8_t ctl_rate;                   /* 1: slow modulators (LFO) are worked out every fourth sample (a setting; less CPU) */
    uint8_t edit_all;                   /* knob edits go to every snapshot that still shares the value (else only the current one) */
    uint64_t mut_lock;                  /* MUTATE: the modules it leaves alone (bit per slot); saved with the patch (version 13) */
    uint16_t byp_has;                   /* snapshots that have their own bypassed modules (bit per snapshot) */
    uint64_t byp[PW_VARS];              /* which modules each snapshot bypasses (the current one's lives in the modules' flags) */
    volatile uint8_t pre_eff[PW_MAX_MODULES];   /* a PRESET jack's preset now (the audio side writes it; 0xff: no cable) */
    uint8_t pre_seen[PW_MAX_MODULES];           /* the one pw_maintain has applied (0xff: not yet looked) */
    uint32_t clear_serial;              /* counts every pw_clear (a patch loaded, new, random): the UI starts each module on page 1 */
    uint8_t morphing;                   /* the LIVE page's MORPH is between variations: the current one's store is kept */
    uint32_t start_s16;                 /* the transport's count when PLAY next starts it (a song's patch clip carries on from its bar) */
    volatile uint32_t sixteenths;       /* transport ticks since PLAY (audio side counts; the LIVE page's bar line) */
    uint8_t perf[PW_PERF][2];           /* slot, parameter (0xff: empty) */
    uint8_t perf_cc[PW_PERF];           /* the MIDI CC each follows (0xff: none) */
    pw_song song;                       /* the SONG page: clips on bars (saved with the patch) */
    /* MIDI: the CCs' last values (and channels), the sustain pedal, the clock in */
    uint8_t cc[128], cc_ch[128], last_cc, sustain;
    uint32_t cc_serial;
    volatile uint8_t clk_q;             /* MIDI clocks not yet turned into ticks (control side adds, audio takes) */
    uint8_t clk_count, ext_sync;        /* clocks since the last sixteenth; following a MIDI clock */
    uint8_t clk_out, pc_patch, cko_play, cko_count; volatile uint8_t pc_req; int32_t cko_next;   /* MIDI clock out (a setting) and its state; program change picks patches (a setting), the request for the screen side (program + 1) */
    uint32_t clk_ms, clk_avg_us;
    volatile uint32_t bank_rev;         /* counts changes of the sample bank (a CZ keeps its tone in its own RAM until this moves)  */
    volatile uint32_t btn_keys;         /* the keys the BUTTON modules see (bit k: key k+1 held), set by the keyboard dispatcher */
    uint8_t btn_virt[PW_MAX_MODULES];   /* each BUTTON's on-screen button, held */
} pw_engine;

extern pw_engine pws;
/* a breadcrumb trail for a crash screen: the last two steps the main loop took through the bank, (older << 16) | latest */
extern volatile uint32_t pw_trace;
#define PW_TRACE(x) (pw_trace = (pw_trace << 16) | (uint32_t)(x))
extern const pw_type pw_types[PW_T_COUNT];
extern const char *const pw_cat_names[PW_C_COUNT];

/* ---- control-side API (control task / UI thread only) ---- */
void pw_init(void);
void pw_clear(void);
int  pw_add(unsigned type, int x, int y);            /* slot or -1 */
int  pw_add_at(int slot, unsigned type, int x, int y);   /* into that slot (free), or the first free (-1) */
int  pw_remove(unsigned slot);
int  pw_remove_wired(unsigned slot);               /* pw_remove, then the cables it carried go straight on (how many); -1: no module */
int  pw_connect(unsigned sm, unsigned so, unsigned dm, unsigned di); /* cable or -1 */
int  pw_find_cable(unsigned sm, unsigned so, unsigned dm, unsigned di);
int  pw_disconnect(unsigned cable);
void pw_cable_amount(unsigned cable, int amt);      /* -100..100 %: attenuate, invert (one undo per sweep) */
void pw_set_param(unsigned slot, unsigned p, float v);
int  pw_audition_ok(unsigned type);                  /* a sound source that pw_audition can strike */
void pw_audition(unsigned slot, int note, unsigned pad);   /* strike it on its own: a note (pitched sources), a pad 0-7 (DRUM RACK); it sounds through the patch if its output is cabled, else straight to the speaker */
int  pw_preset_param(unsigned type);                /* the type's PRESET knob (DRUM RACK: KIT), -1 none */
int  pw_preset_count(unsigned type);                  /* how many presets the type's PRESET knob has (0 none) */
void pw_preset_apply(unsigned slot, int k);          /* a preset set from a PRESET jack: the knobs follow, no undo step */
void pw_module_preset(unsigned slot, int k);         /* sets the module's other knobs from its preset k (1..) */
float pw_get_param(unsigned slot, unsigned p);
void pw_nudge_param(unsigned slot, unsigned p, int detents, int fine);
void pw_move(unsigned slot, int x, int y);
void pw_set_bpm(float bpm);
void pw_set_playing(int on);
void pw_key(int note, int velocity);
void pw_button_keys(uint32_t mask);                  /* BUTTON: the keys held (bit k: key k+1), 27 bits */
void pw_button_virtual(unsigned slot, int down);     /* BUTTON: its on-screen button down / up */
void pw_pressure(int value);                         /* the keys' pressure (aftertouch), 0..127 */                 /* velocity 0 = release */
void pw_midi(uint8_t status, uint8_t d1, uint8_t d2);
void pw_all_notes_off(void);
int  pw_module_index(unsigned slot);                 /* 1-based number among same type */
void pw_module_label(unsigned slot, char out[12]);   /* "VCO 1" */
void pw_module_tag_label(unsigned slot, char out[12]);
const char *pw_module_preset_name(unsigned slot);
float pw_out_last(unsigned slot, unsigned port);
unsigned pw_scale_mask(unsigned scale);   /* the notes of a QUANTIZER scale (bit 0 = the root) */              /* for displays: the last sample of an output */
float pw_in_last(unsigned slot, unsigned in);                 /* ... of an input (its cables summed at their amounts) */
int  pw_input_cables(unsigned slot, unsigned in);    /* number of cables */
int  pw_count(void);
int  pw_cable_count(void);
void pw_format_param(unsigned slot, unsigned p, char out[12]);
void pw_set_scope(unsigned slot, unsigned port);
int  pw_seq_step(unsigned slot);                     /* current step or -1 */
int  pw_trig_get(unsigned slot, unsigned track, unsigned step);   /* TRIG SEQ: 0 off, 1 on, 2 accent, 3 roll */
void pw_trig_set(unsigned slot, unsigned track, unsigned step, int v);
/* PIANO ROLL: notes are four bytes in data[2..]: start (a 16th), length (16ths), pitch (MIDI), velocity (5 bits) << 2 | voice (0-3); data[0] counts them */
#define PW_ROLL_MAX 30
static inline unsigned pw_data_len(unsigned type) { return type == PW_T_PROLL ? PW_ROLL_BYTES : PW_DATA_BYTES; }
unsigned pw_roll_count(const pw_module *m);
int  pw_roll_get(const pw_module *m, unsigned i, unsigned *start, unsigned *len, unsigned *pitch, unsigned *vel, unsigned *voice);   /* 0: no such note */
int  pw_roll_find(const pw_module *m, unsigned t, unsigned pitch);        /* the note at that step and pitch, or -1 */
unsigned pw_roll_loop(const pw_module *m);                                /* the loop's length in 16ths (BARS x 16) */
int  pw_roll_add(pw_module *m, unsigned start, unsigned len, unsigned pitch, unsigned vel);   /* its index, or -1: eight... no room, or more than four at once */
void pw_roll_del(pw_module *m, unsigned i);
int  pw_roll_edit(pw_module *m, unsigned i, int start, int len, int vel);  /* move / resize / velocity (-1: keep); 0 done, -1 would overlap on every voice */
int  pw_roll_pitch(pw_module *m, unsigned i, unsigned pitch);               /* a note's pitch (0 done; -1: the same pitch is already sounding there) */
void pw_roll_clear(pw_module *m);
unsigned pw_cur_voice(void);                                              /* the voice area's current voice (0 outside it) */
int  pw_has_data(unsigned type);                     /* the type keeps steps in data[] (saved with the patch) */
/* What a module is doing now, for the screen (the control side reads the audio side's state; a value may be a
 * block old): ENVELOPE 0 its level 0..1, 1 its stage; FM SYNTH k operator k's envelope; TRIG SEQ t track t's
 * step (-1 none); EUCLID and SEQUENCER 0 the step; LFO 0 its phase; anything else 0. */
float pw_live(unsigned slot, unsigned what);
/* PARAM MOD: any knob of any module moved by a voltage, once a block. The audio side adds each channel's offset to the target's knob
 * for the sixteen samples it is about to run (or for the block), and puts the knob back afterwards (pw_pm_apply / pw_pm_restore, pw_pm_mask: the modules it moves). */
#define PW_PM_MAX 16
typedef struct { float orig, wr; uint8_t par; } pw_pm_undo;
extern uint64_t pw_pm_mask;
unsigned pw_pm_apply(unsigned slot, pw_pm_undo *u, unsigned c0, unsigned cn);   /* the offsets for samples c0..c0+cn of the block */
int pw_pm_steppable(unsigned type);
void pw_pm_restore(unsigned slot, const pw_pm_undo *u, unsigned n);
int pw_pm_target(unsigned slot, unsigned ch);   /* a PARAM MOD channel's target module (slot), or -1 */
int pw_grain_peaks(unsigned slot, uint8_t *pk, unsigned n);   /* GRANULAR: its sound (a bank sound, or the live recording past to now) as n peaks 0-255; 0: nothing to show */
int pw_burst_hits(const float *p, float *t, float *len);   /* BURST: where its hits fall, for the screen */
void pw_inertia_curve(const float *p, float *y, int n, float *t);   /* INERTIA's step (or kick) response for the screen */
void pw_vco_wave(const float *p, int k, float *y, int n);   /* one cycle of a VCO output as it is made, for the screen */
int pw_param_cv(unsigned type, unsigned param, float *k, int *mode, int *amt);   /* the CV jack that moves this knob (-1: none) and how (a display aid) */
/* a bank sound's overview for the screen: n columns of its peak, 0..255 (0 when the slot is empty) */
void pw_maintain(void);                              /* control side, often: a sliced sampler finds its transients */
int  pw_sampler_slice(unsigned slot, unsigned k, float *frac);   /* a SAMPLER's slices: their count, slice k's start */
int  pw_sampler_overview(unsigned slot, uint8_t *peaks, unsigned n, uint32_t *frames);   /* a SAMPLER's sound */
int  pw_sample_overview(unsigned bank, uint8_t *peaks, unsigned n, uint32_t *frames);
float pw_sync_sec(float param);                      /* a SYNC knob's note value in seconds at the tempo (0: OFF) */
float pw_eq_response(const float *p, float hz);      /* an EQ's gain in dB at hz, from its parameters */

/* Polyphony: VOICES 1..PW_MAX_VOICES; the modules flagged PW_F_POLY (and whatever sits between two of them) run
 * once per voice, each voice its own state and audio memory, the keys spread over the voices. */
void pw_set_voices(unsigned n);
void pw_set_flags(unsigned slot, unsigned flags);
void pw_set_group(unsigned slot, unsigned g);        /* 0: none; a group is shown as one box in the patch view */
#define PW_MAX_GROUP 63
void pw_auto_poly(void);                             /* flag the keys' voice: KEYS and what it plays, up to effects */
/* Variations: PW_VARS sets of every parameter (and sequencer steps); the patch plays one. */
void pw_var_select(unsigned v);
void pw_var_copy(unsigned from, unsigned to);
extern uint8_t pw_lane_skip;                          /* lanes (bit per lane) pw_lane_apply leaves alone: the one a hand is recording over */
void pw_lane_apply(float pos);                       /* the song's lanes at pos (sixteenths of a bar): each knob moves there */
int  pw_lane_find(unsigned mod, unsigned par);       /* the lane of a knob, or -1 */
int  pw_lane_add(unsigned mod, unsigned par);        /* a new lane for it (its index), -1 when full */
void pw_lane_del(unsigned l);
unsigned pw_lane_start(unsigned l);                  /* the lane's first point in the pool */
int  pw_lane_set(unsigned l, unsigned t, unsigned v, unsigned shape);   /* a point at t (replacing one there): its index in the pool, -1 when full */
void pw_lane_del_pt(unsigned l, unsigned i);         /* the lane's ith point */
int  pw_lane_move_pt(unsigned l, unsigned i, unsigned t);   /* the ith point to time t (a point there is replaced): its new index, -1 none */
float pw_lane_eval(unsigned l, float pos);           /* 0-255 */
float pw_lane_norm(unsigned mod, unsigned par, float v);   /* a knob's value as 0-1 of its range */
/* a knob that follows a MOD SEQ lane (REC + knob): saved in the MOD SEQ's spare data bytes */
int  pw_mseq_lock_of(const pw_module *m, unsigned lane, unsigned *mod, unsigned *par);
int  pw_mseq_lock_find(unsigned mod, unsigned par, int *mseq, unsigned *lane);
int  pw_mseq_lock_assign(unsigned mod, unsigned par, int *mseq, unsigned *lane);   /* 1 had one, 0 made one, -1 no room, -2 lanes full */
void pw_mseq_lock_clear(unsigned mseq, unsigned lane);
void pw_mseq_lock_write(unsigned mseq, unsigned lane, int step, int all);
int  pw_mut_eligible(unsigned slot);                /* MUTATE can change this module (it has knobs or steps worth it) */
int  pw_mutate(int roll, float amount, float density);   /* NUDGE (roll 0: a little off where each knob is) or ROLL (1: toward a new value); AMOUNT how far, DENSITY the share of knobs and steps touched; the number of changes. One undo step. */
int  pw_mut_age(unsigned slot);                      /* how many mutations ago a module was last changed (0 = the latest, 99 = not lately), for the MUTATE picture */
int  pw_mut_last_roll(void);                         /* 1 if the latest mutation was a roll, 0 a nudge */
void pw_mutate_follow(uint32_t now_ms);             /* the MUTATE modules' NUDGE and ROLL jacks (the screen's loop calls this) */
void pw_snap_follow(uint32_t now_ms);              /* SNAPSHOT: its CV picks the snapshot (the screen's loop calls this) */
void pw_mseq_follow(int skip_mod, int skip_par);   /* each knob takes its lane's step as it plays (the screen's loop calls this) */
void pw_var_morph(unsigned a, unsigned b, float t); /* every knob between variations a and b (t 0..1); a is the current one */
/* The performance page: PW_PERF parameters from anywhere in the patch. */
int  pw_perf_assign(unsigned slot, unsigned p);      /* the page's place it took (-1 full); already there: its place */
void pw_perf_clear(unsigned k);
/* Undo and redo (a log of the edits); copy and paste a module. */
void pw_seq_fill(unsigned slot, int clear);          /* SEQUENCER / MOD SEQ: randomize, or clear */
void pw_undo_data(unsigned slot);                    /* before changing a module's data[] */
void pw_undo_group(int open);                        /* 1: the edits from here are one step; 0: done */
int  pw_undo(void);                                  /* 1 done, 0 nothing to undo */
int  pw_redo(void);
void pw_undo_reset(void);
int  pw_copy(unsigned slot);
int  pw_paste(int x, int y);                         /* the new slot, or -1 */
int  pw_clip_type(void);                             /* what is copied (-1 nothing) */
/* Adding a module into the patch where the cursor is: at a jack (slot, is_out, port), or into one cable (cable >= 0)
 * or nowhere (slot < 0); is_out | 2: plug into a carried cable's loose end. What it would do (PW_WIRE_*), and its ports that would be used: */
#define PW_FM_SHAPES 12
extern const float pw_fm_shape[PW_FM_SHAPES][4];   /* FM4's envelope shapes: attack, decay, sustain, release */
int  pw_audio_jack(const char *name);
/* MOD SEQ: four lanes of 16 steps, six bits a step in data[0..47]; values 0..255 */
int pw_markov_row(const pw_module *m, int cur, float *w);   /* MARKOV: the chances of each note after note cur (36 floats at most); how many notes */
unsigned pw_mseq_get(const pw_module *m, unsigned lane, unsigned step);
void pw_mseq_set(pw_module *m, unsigned lane, unsigned step, int v);
/* SEQUENCER steps: data[0..15] note, data[16..17] gates, data[18+k] ratchet (bits 0-1: 1-4) and chance (bits 4-7: 1/16..16/16),
 * data[34+k] velocity 0..255. The default of a step's byte is 0xF0. */
#define PW_SEQ_STEPFEAT 18
#define PW_SEQ_STEPVEL  34
void pw_seq_upgrade(uint8_t *data);   /* its step data made new */
void pw_chord_name(const pw_module *m, uint8_t step_byte, char out[10]);
int  pw_chord_degree(const pw_module *m, int semis_from_c);
void pw_kick(unsigned slot);   /* GRAVITY: drop the ball now */
void pw_trig_monitor(unsigned slot, unsigned track, int accent);   /* the TRIG SEQ fires this track's output now */
void pw_rack_voice(unsigned slot, unsigned p);   /* DRUM RACK: a pad's SOUND was turned to a kit voice: its tune, decay and level go on the pad */
void pw_seq_monitor(unsigned slot, int note_rel, int on);   /* the SEQUENCER sounds this note (semitones from C4) while a key is held */
/* CHORD STEPS: data[k] = degree (0 rest, 1-7, 8 tie) | type << 4 (triad, seventh, sus4, add9) | inversion << 6 */
/* GRAVITY: data[k] = note k of the list, semitones */
/* the SCOPE module's two traces: a ring of PW_SCOPE_RING 16-bit samples each (volts x 3000), its next write at *w; 0 none */
#define PW_SCOPE_RING 2048u
const int16_t *pw_scope_ring(unsigned slot, unsigned ch, uint32_t *w);
void pw_seq_lane_set(pw_module *m, unsigned lane, unsigned step, int v);                /* an input that carries a signal (IN, L, R...) */
enum { PW_WIRE_ALONE, PW_WIRE_INSERT, PW_WIRE_FEED, PW_WIRE_DRIVE };
int  pw_wire_plan(unsigned type, int slot, int is_out, unsigned port, int cable, unsigned *in, unsigned *out);
int  pw_add_wired(unsigned type, int x, int y, int slot, int is_out, unsigned port, int cable);   /* the new slot or -1 */

/* Patch serialization (versioned, little endian, < 4 KiB for device sizes). */
int  pw_save(uint8_t *buf, unsigned cap);
int  pw_load(const uint8_t *buf, unsigned len);
extern int pw_save_trimmed;                          /* the last save left out some variations (no room) */

/* Sampler access for hosts that can load audio files. */
int16_t *pw_sampler_buffer(unsigned slot, unsigned *frames);
void pw_sampler_commit(unsigned slot, unsigned frames);
int  pw_sampler_rec(unsigned slot, int press);       /* the panel's REC: press 1 steps off > armed > recording > stopped; 0 asks the state */
unsigned pw_sampler_rev(unsigned slot);
int  pw_sampler_rec_done(unsigned slot);              /* 2: a recording found IN unpatched (once) */
int  pw_sampler_recorded(unsigned slot, unsigned *frames);   /* the RAM sound kept with the patch (frames), or 0 */
void pw_sampler_unmark(unsigned slot);
int  pw_sampler_marked(unsigned slot);                /* a SAMPLER whose RAM sound is kept with its patch */
extern uint8_t pw_ram_err;                            /* the last pw_ram_save: 0, or why a sound did not fit (PW_BANK_E_*) */
#define PW_SMP_SCALE 26000.0f                         /* a sample's full scale: +-5 V at +-26000 */

/* The sample bank: PW_BANK_SLOTS sounds kept by the platform (the FM-1's flash, the emulator's memory), played
 * by a SAMPLER whose SOURCE is B1..B16. Signed 16-bit mono at `rate` Hz. Audio side: the pointer stays valid
 * while the slot is not being rewritten (the platform empties the slot first). */
#define PW_SAMPLE_SLOTS 16                           /* B1..B16: the sounds a SAMPLER and the DRUM RACK play */
#define PW_CZ_SLOT 16                                /* the slots after them: the CZ module's .syx banks of tones (not sounds; the page fills them) */
#define PW_CZ_BANKS 4
#define PW_DX_SLOT 20                                /* and after those, FM-DX's: .syx banks of TX81Z voices (128 bytes each) */
#define PW_DX_BANKS 4
#define PW_A2_SLOT 24                                /* and the bank's second set of sounds: B (the first is slots 0-15: A) */
#define PW_BANK_SLOTS 40
#define PW_SOUNDS 48                                 /* the sound ids a SAMPLER, GRANULAR and the DRUM RACK use: 0-15 INTERNAL (built in), 16-31 A, 32-47 B */
const int16_t *pw_plat_sample(unsigned slot, uint32_t *frames, uint32_t *rate);   /* 0: empty, or a compressed sound (pw_plat_sample_sea) */
const uint8_t *pw_plat_sample_sea(unsigned slot, uint32_t *frames, uint32_t *rate); /* a compressed sound's chunk stream (pw_sea.inc), else 0 */
/* A sound to read, whatever it is stored as (pw_sea.inc): the bank's raw 16-bit or compressed, or a module's own RAM. */
typedef struct { const int16_t *raw; const uint8_t *sea; uint32_t frames, rate; } pw_smp;
typedef struct pw_rdr pw_rdr;                               /* a voice's place in a compressed sound */
/* the sound id of a SAMPLER's BANK (0 INTERNAL, 1-4 A-D) and SAMPLE (1-8): INTERNAL's are ids 0-7, A-D 16-47 (eight each: A and B are the bank's slots 0-15, C and D 24-39) */
static inline unsigned pw_snd_id(int bank, int n) { bank = bank < 0 ? 0 : bank > 4 ? 4 : bank; n = n < 1 ? 1 : n > 8 ? 8 : n; return bank ? 16u + 8u * (unsigned)(bank - 1) + (unsigned)n - 1u : (unsigned)n - 1u; }
int pw_smp_open(unsigned id, pw_smp *s);                    /* a sound id 0..PW_SOUNDS-1 (INTERNAL, A, B); 0: empty */
const char *pw_sample_name(unsigned id);                    /* its name (0: empty) */
void pw_smp_ram(pw_smp *s, const int16_t *ram, uint32_t frames, uint32_t rate);
pw_rdr *pw_rdr_get(unsigned key, const pw_smp *s);          /* the audio side: a voice's reader (key: a number the voice keeps), 0 for a raw sound or none free */
int16_t pw_smp_get_z(const pw_smp *s, pw_rdr *r, uint32_t i);   /* (a compressed sound's frame: through the reader, decoding forward) */
/* one frame of a sound, raw or compressed (r from pw_rdr_get: 0 for a raw sound) */
static inline __attribute__((always_inline)) int16_t pw_smp_get(const pw_smp *s, pw_rdr *r, uint32_t i)
{
    return s->raw ? (i < s->frames ? s->raw[i] : (int16_t)0) : pw_smp_get_z(s, r, i);
}
void pw_sea_block(void);                                    /* once a block: readers not asked for in two blocks are free */
void pw_smp_read(const pw_smp *s, uint32_t from, uint32_t n, int16_t *dst);   /* the display, the slice finder: a run, cache untouched */
void pw_smp_peaks(const pw_smp *s, uint8_t *pk, unsigned n, int unused);   /* n columns of peaks 0-255 */
uint32_t pw_sea_bytes(uint32_t frames);                     /* the stored size of a compressed sound (frames a multiple of 20) */
const char *pw_plat_sample_name(unsigned slot);                                    /* 0: empty */
/* AUDIO IN: the platform's audio input for this block (PW_N frames, +-1 full scale); 0 when there is none */
int pw_plat_audio_in(float *left, float *right);
uint32_t pw_plat_ticks(void);          /* a free-running counter, and how many of its ticks are a microsecond (DIAGNOSTICS' CPU page times each module with it) */
uint32_t pw_plat_ticks_per_us(void);
extern uint32_t pw_prof_tk[PW_MAX_MODULES], pw_prof_blocks, pw_prof_all;   /* ticks each module's proc took since pw_prof_on was set; blocks rendered; ticks the whole render took */
extern uint8_t pw_prof_on;
void pw_plat_cpu1_jobs(uint32_t *w);   /* the test job on the second core: jobs done, mismatches, timeouts, then us: CPU1 and CPU0 in RAM, CPU1 and CPU0 in flash; 7 words */
void pw_plat_cpu1_info(uint32_t *w);   /* six words read back when the second core's start gave up (CORE_CON, entry word, CPU1 EMU_MSG, DBG_MSG, SYS_DIV, how far CPU1 got) */
typedef void (*pw_cpu1_fn)(void *m, const float *const *in, float *const *out, unsigned connected);
uint32_t pw_plat_cpu1_post(pw_cpu1_fn fn, void *m, const float *const *in, float *const *out, unsigned connected);   /* a module's block to the second core, in order: a ticket (0: CPU1 is not running) */
int pw_plat_cpu1_wait(uint32_t ticket, uint32_t *us);   /* until that block is done (us: what it took on CPU1): 0, or -1 if CPU1 did not answer in time */
pw_cpu1_fn pw_plat_cpu1_dummy(void);   /* a job that only waits (the OFFLOAD test mode): 0 if there is none */
void pw_plat_cpu1_stop(void);          /* hold CPU1 (it did not answer) */
extern uint8_t pw_cpu1_offload, pw_off_state;   /* the switch: 0 off, 1 effects, 2 effects, filters, sources; 0 idle, 1 modules run on CPU1, 3 it failed and everything is CPU0's again */
extern uint32_t pw_off_us, pw_off_n, pw_off_probing, pw_off_wait_us, pw_off_copy_us;   /* CPU1's work a block (us), the modules on it, the modules still being timed */
unsigned pw_off_reason(unsigned slot, unsigned *us);   /* why a module is or is not on the second core, as a code (the diagnostics report) */
unsigned pw_plat_bootprobe(char *buf, unsigned max);    /* device: read-only first-sector bytes + sector CRCs (host: nothing) */
unsigned pw_diag_report(char *buf, unsigned max);        /* the CPU page's numbers as text (the web page saves it) */
int pw_plat_cpu1(uint32_t *count, int start);   /* the second core (experimental): 0 never tried, 1 running (count grows), 2 did not report in, 3 up but not counting, -1 not on this platform; start: try it (once) */

/* ---- audio side ---- */
void pw_set_rate(int half);                         /* 0: 44.1 kHz, 1: 22.05 kHz (the engine's own; the output stays 44.1 kHz) */
int pw_rate_half(void);
PW_DSP void pw_render(float *left, float *right);
int pw_clip_gr_tenths(void);                       /* CLIP PROTECT's gain reduction in tenths of a dB (-24: -2.4 dB; 0 when off or not touching the signal) */
float pw_clip_gr_db(void);   /* PW_N frames each, +-1 */

/* Platform hooks (implemented by the device glue or the emulator). */
void pw_plat_lock(void);      /* hold the audio render (it outputs silence); nests */
void pw_plat_unlock(void);
void pw_plat_midi_out(uint8_t status, uint8_t d1, uint8_t d2);   /* MIDI out (the audio side may call it) */
int  pw_plat_audio_running(void);
uint32_t pw_plat_millis(void);

/* Shared helpers */
void pw_param_text(const pw_param_desc *d, float v, char out[12]);
uint32_t pw_crc32(uint32_t crc, const void *data, unsigned n);
void pw_wt_build(pw_module *m);   /* the WAVETABLE module's four tables, into its region (when it is added, and when its region is remade) */
#endif
