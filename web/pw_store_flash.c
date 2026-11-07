/* SPDX-License-Identifier: GPL-3.0-only
 * Eight user patch slots, one 4 KiB Flash sector each, at the end of the
 * FM-1 user partition. Each patch carries its own CRC (see pw_save). */
#include "pw_store.h"
#include "pw_engine.h"
#include "pw_hw.h"
#include <string.h>

static char names[PW_USER_SLOTS][13];
static uint8_t valid[PW_USER_SLOTS], kind[PW_USER_SLOTS];
static uint8_t sector[FM1_FLASH_SECTOR_BYTES];

static int slot_offset(unsigned slot, uint32_t *offset)
{
    uint32_t size = fm1_flash_size();
    if (slot >= PW_USER_SLOTS || size < (PW_USER_SLOTS + 1u) * FM1_FLASH_SECTOR_BYTES) return -1;
    *offset = (size / FM1_FLASH_SECTOR_BYTES - PW_USER_SLOTS + slot) * FM1_FLASH_SECTOR_BYTES;
    return 0;
}
static void refresh(unsigned slot)
{
    uint32_t at;
    uint8_t head[24];
    valid[slot] = 0;
    names[slot][0] = 0;
    if (slot_offset(slot, &at) || fm1_flash_read(at, head, sizeof head) != FM1_FLASH_OK) return;
    if (memcmp(head, "NDL1", 4)) return;
    valid[slot] = 1;
    kind[slot] = head[9] >> 7;   /* (the bpm's top bit: saved as a GROUP) */
    unsigned n = 12;
    while (n && head[10 + n - 1] == ' ') --n;
    for (unsigned i = 0; i < n; ++i) names[slot][i] = (head[10 + i] >= 32 && head[10 + i] < 127) ? (char)head[10 + i] : '?';
    names[slot][n] = 0;
    if (!n) memcpy(names[slot], "UNTITLED", 9);
}
void pw_store_init(void)
{
    for (unsigned i = 0; i < PW_USER_SLOTS; ++i) refresh(i);
}
const char *pw_store_name(unsigned slot)
{
    return slot < PW_USER_SLOTS && valid[slot] ? names[slot] : 0;
}
int pw_store_save(unsigned slot)
{
    uint32_t at;
    if (slot_offset(slot, &at)) return -1;
    int n = pw_save(sector, sizeof sector);
    if (n <= 0) return -2;
    while (n & 3) sector[n++] = 0xff;
    pw_plat_lock();
    int r = fm1_flash_erase(at);
    if (r == FM1_FLASH_OK) r = fm1_flash_write(at, sector, (size_t)n);
    pw_plat_unlock();
    refresh(slot);
    return r == FM1_FLASH_OK ? 0 : -3;
}
int pw_store_kind(unsigned slot) { return slot < PW_USER_SLOTS && valid[slot] ? kind[slot] : 0; }
int pw_store_insert(unsigned slot, uint64_t *set)
{
    uint32_t at;
    *set = 0;
    if (slot_offset(slot, &at) || !valid[slot]) return -1;
    if (fm1_flash_read(at, sector, sizeof sector) != FM1_FLASH_OK) return -2;
    return pw_insert(sector, sizeof sector, set);
}
int pw_store_load(unsigned slot)
{
    uint32_t at;
    if (slot_offset(slot, &at) || !valid[slot]) return -1;
    if (fm1_flash_read(at, sector, sizeof sector) != FM1_FLASH_OK) return -2;
    return pw_load(sector, sizeof sector);
}

/* the page's backup: a slot's stored patch as it is (its length; 0 empty), and one written back as it was */
int pw_store_raw_get(unsigned slot, uint8_t *dst, unsigned cap)
{
    uint32_t at;
    if (slot_offset(slot, &at) || !valid[slot] || fm1_flash_read(at, sector, sizeof sector) != FM1_FLASH_OK) return 0;
    unsigned total = sector[22] | (unsigned)sector[23] << 8;
    if (total < 28 || total > sizeof sector || total > cap) return 0;
    memcpy(dst, sector, total);
    return (int)total;
}
int pw_store_raw_put(unsigned slot, const uint8_t *src, unsigned n)
{
    uint32_t at;
    if (slot_offset(slot, &at) || n < 28 || n > sizeof sector || memcmp(src, "NDL1", 4)) return -1;
    memcpy(sector, src, n);
    while (n & 3) sector[n++] = 0xff;
    pw_plat_lock();
    int r = fm1_flash_erase(at);
    if (r == FM1_FLASH_OK) r = fm1_flash_write(at, sector, (size_t)n);
    pw_plat_unlock();
    refresh(slot);
    return r == FM1_FLASH_OK ? 0 : -3;
}

/* the settings (kept in memory here: the page is a demo) and the rest of the store's contract */
static uint8_t set_mem[128]; static int set_len = -1;
int pw_settings_load(void *dst, unsigned n) { if (set_len < 0 || (unsigned)set_len > n) return -1; memcpy(dst, set_mem, (size_t)set_len); return set_len; }
int pw_settings_save(const void *src, unsigned n) { if (n > sizeof set_mem) return -1; memcpy(set_mem, src, n); set_len = (int)n; return 0; }
int pw_store_erase(unsigned slot)
{
    uint32_t at;
    if (slot_offset(slot, &at)) return -1;
    pw_plat_lock(); fm1_flash_erase(at); pw_plat_unlock();
    refresh(slot);
    return 0;
}
unsigned pw_flash_free_kb(void) { return 0; }
void pw_plat_budget(uint32_t v[6]) { v[0] = 562068u; v[1] = 0x8DFBCu; v[2] = 97264u; v[3] = 98304u; v[4] = 288288u; v[5] = 344064u; }
