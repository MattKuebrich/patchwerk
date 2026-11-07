/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PW_STORE_H
#define PW_STORE_H
#include <stdint.h>
#define PW_USER_SLOTS 8
void pw_store_init(void);
const char *pw_store_name(unsigned slot);  /* 0 when the slot is empty */
int pw_store_save(unsigned slot);
int pw_store_load(unsigned slot);
int pw_store_kind(unsigned slot);   /* 1: saved as a GROUP (inserted into the open patch, not loaded) */
int pw_store_insert(unsigned slot, uint64_t *set);   /* a GROUP slot into the open patch: modules made (< 0: nothing), their slots in `set` */
int pw_store_erase(unsigned slot);   /* empties a user slot */
int pw_settings_load(void *dst, unsigned n);   /* the settings object: bytes, or < 0 */
int pw_settings_save(const void *src, unsigned n);
unsigned pw_flash_free_kb(void);
/* what the firmware takes of the FM-1 (bytes): v[0] flash image, v[1] its room; v[2] static RAM, v[3] its room; v[4] pool, v[5] its room */
void pw_plat_budget(uint32_t v[6]);   /* the sample bank's free room */
#endif
