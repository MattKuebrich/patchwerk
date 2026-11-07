/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PW_UI_H
#define PW_UI_H
#include <stdint.h>
int pw_ui_user_palette(const uint16_t *v);      /* the USER palette (18 colors, RGB565): set and kept in the settings, or 0 to remove it */
void pw_ui_pal_from_legacy(const uint16_t *in33, uint16_t *out18);   /* the first USER palette's 33 colors as the 18 */
int pw_ui_user_palette_get(uint16_t *out);       /* 1 and the 18 colors, or 0 when there is none */
int pw_ui_settings_get(uint8_t *out, int max);          /* the settings as the store keeps them (backup): the length, or 0 */
int pw_ui_settings_put(const uint8_t *in, int n);         /* a backup's settings back in use and kept: 0, or < 0 */
void pw_ui_custom(int idx, int v);   /* web page palette editor: set one field of the custom palette */
void pw_ui_custom_use(int on);       /* ... and show it (over the current palette) or stop */
void pw_ui_set_palette(int pal);   /* select a palette by number (tests) */
int pw_ui_palette(int pal, int tone, int bright, uint16_t *out, int max);   /* the palette's colors as screen settings make them (pal < 0: the defaults) */
int pw_ui_has_vis(unsigned type);   /* the module page draws a visualization of this module */
#define PW_VERSION "v1.0.1"
#define PW_IDENT "FM-1_606010000"   /* the release identity (fm1.toml id_prefix + the version) */                 /* shown at power-on; CHANGELOG.txt says what each brought */
void pw_ui_init(void);          /* control task, after pw_init(): with PATCHWERK's boot screen */
void pw_ui_init_noboot(void);   /* the same, straight to the patch (the platform showed pw_ui_splash) */
void pw_ui_splash_frame(void); /* one frame of the boot animation (the platform calls it while it settles at power-on: it starts at once) */
void pw_ui_splash(void);        /* the boot screen, drawn and sent at once (the platform's splash) */
void pw_ui_notice(const char *msg);
int pw_ui_rate_drop(void);   /* the CPU overloaded: in AUTO the engine goes to 22.05 kHz (1: it did) */
int pw_ui_help_lines_xs(unsigned type), pw_ui_example_lines_xs(unsigned type);   /* the same, in the small text under the patch strip (7 and 6 fit) */
int pw_ui_example_lines(unsigned type);   /* its example's text: its lines (7 fit) */
int pw_ui_sub_count(void);                 /* the Groups: the patch list's S1.., the web page's list */
const char *pw_ui_sub_name(unsigned i);
const char *pw_ui_sub_what(unsigned i);
int pw_ui_sub_insert(unsigned i);
void pw_ui_ungroup(int slot);              /* take the group a module is in apart (one undo step) */          /* add one to the patch as a group: its first module's slot, or -1 */
int pw_ui_help_lines(unsigned type);   /* the add list's description of a module: its lines (the list has room for 8) */   /* a message on the screen for a moment */
void pw_ui_progress(const char *what, const char *detail, unsigned percent);   /* a long job's screen, at once */
void pw_ui_redraw_all(void);    /* send every tile again at the next frame */
void pw_ui_control(void);       /* control task, called continuously */
#ifdef PLAT_HOST
int pw_ui_gview(void);
int pw_ui_carrying(void);
#endif
int pw_ui_page(void);           /* the page shown (tests): 1 patch, 2 module, 3 add, 4 cables, 5 scope, 6 seq,
                                 * 7 patches, 8 shortcuts */
void pw_ui_select(int slot);     /* show this module (the web page's selection) */
int pw_ui_slot(void);           /* the module under the cursor (tests) */
int pw_ui_addcur(void);         /* the add list's cursor (tests) */
/* Platform: physical MASTER pot -> output level 0..100. */
unsigned pw_plat_master(unsigned adc, int *changed);
/* Platform: audio render load in percent, or >100 when unknown. */
unsigned pw_plat_cpu(void);
#endif
