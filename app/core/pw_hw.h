/* SPDX-License-Identifier: GPL-3.0-only
 * PATCHWERK's view of the FM-1 panel, screen and flash. The core (engine, UI)
 * talks only to these; each platform implements them:
 *   app/app.c        on fm1-base (the FM-1, and fm1-base's host simulator)
 *   web/pw_web.c     in the browser emulator */
#ifndef PW_HW_H
#define PW_HW_H
#include <stdint.h>
#include <stddef.h>

#define FM1_SCREEN_WIDTH 240U
#define FM1_SCREEN_HEIGHT 240U

/* Panel state. Buttons, in this bit order: OCT-, OCT+, FX, SEL, ENV, LFO, EDIT,
 * GLO, HOME, SAVE, ARP, SEQ, PLAY, REC. Keys: bit 0 = F3 .. bit 26 = G5.
 * Encoders (detents since the last read): SELECT, ALGORITHM, KNOB 1-4, PRESETS. */
struct fm1_controls {
    uint32_t keys;
    uint32_t buttons;
    int16_t encoders[7];
    uint16_t knob, battery;  /* MASTER 0..1023 */
    uint8_t vel[27];         /* each key's last press's velocity, 1..127 (0: not known: 100) */
    uint8_t press[27];       /* each held key's pressure, 0..127 (keys that sense it: F3..C5) */
};
void fm1_controls_read(struct fm1_controls *state);
/* LEDs: 0..13 the buttons (order above), 14..40 the keys */
void fm1_led_set(unsigned key, int on);
void patchwerk_led_dim(unsigned key, int on);   /* a key's soft glow (the bar markers); a lit LED stays lit */
uint32_t fm1_millis(void);
unsigned fm1_battery_level(void);   /* 0..3 bars (stock thresholds 531/561/591 with hysteresis), 4: on USB power (charging); 0 with raw 0: no reading yet */
unsigned fm1_battery_raw(void);   /* the battery's ADC reading, 0..1023 (0: none, or not now); averaged, 0 if none yet */
/* native-endian RGB565, row r of the w x h rectangle at pixels + r * stride; the pixels may be reused at once */
int fm1_screen_blit(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *pixels, unsigned stride);

/* raw flash, for platforms without an object store (the browser emulator) */
#define FM1_FLASH_SECTOR_BYTES 4096U
enum { FM1_FLASH_OK = 0, FM1_FLASH_INVALID = -1 };
uint32_t fm1_flash_size(void);
int fm1_flash_read(uint32_t offset, void *data, size_t bytes);
int fm1_flash_write(uint32_t offset, const void *data, size_t bytes);
int fm1_flash_erase(uint32_t offset);
#endif
