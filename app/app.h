/* SPDX-License-Identifier: GPL-3.0-only */
/* PATCHWERK on fm1-base: the app's half of the contract with the platform
 * (base/device/unity.c on the FM-1, base/host/plat_host.c in the simulator).
 * The synth itself is in core/ (engine, modules, UI); app.c binds it to plat.h. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "PATCHWERK"
#define APP_TAGLINE "MODULAR SYNTH"
#define APP_SPLASH_RGB RGB(255, 140, 50)
#define APP_SYS_TEXT                     /* the platform's own screens (boot, update, crash, calibration) are written in PATCHWERK's faces (core/pw_ui.c): fm1-base's three are not linked, 29 KB of flash. Remove this line to go back to theirs */
#define APP_BIG_FONT FONT_M              /* (used only without APP_SYS_TEXT) */
#define APP_BLOCK 256u                   /* frames per audio block: two of the engine's 128-frame blocks */
#define APP_OVERLOAD                     /* app_overload(): the render ran over budget */
#define APP_REDRAW                       /* app_redraw(): send the whole screen again */
#define APP_SPLASH                       /* app_splash(): the boot screen (pw_ui_splash) */
#define APP_SPLASH_FRAME                 /* app_splash_frame(): called every 20 ms during the power-on settle delays: the cables draw in at once */
#define APP_SYSEX                        /* app_sysex(): the live link's frames from the web page (core/pw_link.c) */
#define APP_USB_CMD                      /* app_usb_cmd(): the sample bank's commands (cmd 0x60, core/pw_bank.c) */
#define APP_TOUCH                        /* velocity and pressure from the keys (base/lib/touch.h) */
/* USB: audio and MIDI as two functions (base/bsp/chip/usb.c FELUCCA_USB_MAC_LAYOUT): macOS 14 gave the plain
 * layout's audio no driver, so the FM-1 was no sound card on a Mac */
#define FELUCCA_USB_MAC_LAYOUT 1
/* the computer's audio over USB is never mixed straight into the output: it is heard only where an AUDIO IN
 * module's cables send it (base/device/main_fm1.c APP_UAC_GAIN, patches/fm1-base-uac-gain.patch) */
#define APP_UAC_GAIN(g) ((void)(g), 0u)

/* the store: eight user patches in the data area (two 4 KiB sectors each), the panel calibration
 * (the platform's) where the template keeps it; between them the sample bank (core/pw_bank.h: 0xA7000..0xDC000) */
enum { OBJ_PATCH0, OBJ_PATCH1, OBJ_PATCH2, OBJ_PATCH3, OBJ_PATCH4, OBJ_PATCH5, OBJ_PATCH6, OBJ_PATCH7,
       OBJ_PANEL, OBJ_SETTINGS, OBJ_NOBJ };
#define APP_STORE_MAP {0x97000u, 0x99000u, 0x9B000u, 0x9D000u, 0x9F000u, 0xA1000u, 0xA3000u, 0xA5000u, 0xDC000u, 0xDE000u}

/* the platform's drawing canvas (its own text screens: a crash notice, the calibration, safe mode) is 48 rows of 240, not
 * 144: PATCHWERK draws its screens itself, in bands (pw_ui.c), and the app's splash is app_splash; this frees 46 KB of pool */
#define APP_CANVAS_PX (240u * 48u)

/* the second core, experimental (cpu1/entry.S, DIAGNOSTICS' CPU page): without this line the build has no code for it */
#define APP_CPU1 1
#define APP_FLASH_HOOK                  /* app_flash_begin/end(): the second core stays out of the flash code while the flash is erased or written (app.c) */

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
void app_overload(void);
void app_redraw(void);
void app_splash(void);
void app_splash_frame(void);
int app_sysex(const uint8_t *p, uint32_t n);
void app_usb_cmd(const uint8_t *d, uint32_t n);

/* the host simulator's checks (expect WHAT VALUE): modules, cables, playing, peak, page */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
