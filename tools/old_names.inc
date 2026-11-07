/* SPDX-License-Identifier: GPL-3.0-only
 * Factory-patch building only (PW_FACTORY_GEN): tools/factory_src.inc is written with the names of seven modules that no longer exist
 * (FLANGER, PHASER, CHORUS, REVERSER, SHUFFLER, CLOCK WARP, AUDIO MIXER). Here each is built as the module that took its place, with
 * its knobs, jacks and outputs moved to where they are now. The device does not have this: nothing it saves or loads names an old type. */
enum { PW_T_FLANGER = 200, PW_T_PHASER, PW_T_CHORUS, PW_T_REVERSER, PW_T_SHUFFLER, PW_T_CLOCKWARP, PW_T_MIXER };
typedef struct {
    uint8_t to, mode_param;          /* the module that took its place; the knob that picks the old behaviour (255: none) */
    uint8_t mode, np, ni, no;        /* the value for that knob; how many entries each map has */
    const int8_t *pmap, *imap, *omap;   /* old knob / input / output -> new (-1: gone) */
    const float *def;                /* the old knobs' defaults */
} old_merge_t;
static const int8_t mg_fl_p[5] = {1, 2, 5, 4, 3}, mg_ph_p[5] = {1, 2, 6, 4, 3}, mg_ch_p[4] = {1, 2, 3, 7};
static const int8_t mg_sw_i[2] = {0, 1}, mg_fl_o[1] = {0}, mg_ch_o[2] = {0, 1};
static const int8_t mg_rv_p[3] = {7, 8, 5}, mg_rv_i[2] = {0, 3}, mg_sh_p[6] = {1, 9, 2, 10, 11, 12}, mg_sh_i[3] = {0, 2, 4};
static const int8_t mg_cw_p[6] = {5, 6, 7, 8, 9, 10}, mg_cw_i[4] = {0, 1, 2, 3}, mg_cw_o[3] = {4, 5, 6};
static const int8_t mg_mx_p[4] = {0, 1, 2, 3}, mg_mx_i[4] = {0, 1, 2, 3}, mg_mx_o[1] = {0};
static const float df_fl[5] = {0.25f, 0.7f, 0.003f, 0.6f, 0.5f}, df_ph[5] = {0.3f, 0.8f, 700.0f, 0.5f, 0.6f}, df_ch[4] = {0.5f, 0.5f, 0.5f, 0.8f};
static const float df_rv[3] = {0.4f, 0.0f, 1.0f}, df_sh[6] = {0.4f, 8.0f, 0.25f, 0.0f, 0.2f, 0.0f}, df_cw[6] = {16.0f, 16.0f, 0.0f, 0.0f, 0.0f, 0.3f};
static const float df_mx[4] = {0.8f, 0.8f, 0.8f, 0.8f};
static const old_merge_t old_merges[] = {
    {PW_T_SWIRL, 0, 0, 5, 2, 1, mg_fl_p, mg_sw_i, mg_fl_o, df_fl},      /* FLANGER */
    {PW_T_SWIRL, 0, 1, 5, 2, 1, mg_ph_p, mg_sw_i, mg_fl_o, df_ph},      /* PHASER */
    {PW_T_SWIRL, 0, 2, 4, 2, 2, mg_ch_p, mg_sw_i, mg_ch_o, df_ch},      /* CHORUS */
    {PW_T_FRACTURE, 6, 1, 3, 2, 1, mg_rv_p, mg_rv_i, mg_fl_o, df_rv},   /* REVERSER */
    {PW_T_FRACTURE, 6, 2, 6, 3, 1, mg_sh_p, mg_sh_i, mg_fl_o, df_sh},   /* SHUFFLER */
    {PW_T_CLOCK, 255, 0, 6, 4, 3, mg_cw_p, mg_cw_i, mg_cw_o, df_cw},    /* CLOCK WARP */
    {PW_T_STEREO, 255, 0, 4, 4, 1, mg_mx_p, mg_mx_i, mg_mx_o, df_mx},   /* AUDIO MIXER */
};
static const old_merge_t *old_merge_of(unsigned t) { return t >= PW_T_FLANGER && t <= PW_T_MIXER ? &old_merges[t - PW_T_FLANGER] : 0; }
