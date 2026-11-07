/* SPDX-License-Identifier: GPL-3.0-only */
/* The interrupted-install check (base/bsp/chip/ota_check.h) on the host: the header entries are the real ones read off an FM-1 running rc124. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ota_check.h"

static int bad;
static void check(const char *what, int ok) { if (!ok) { printf("FAIL %s\n", what); bad++; } }
static void hex(uint8_t *d, const char *h) { for (; h[0] && h[1]; h += 2, d++) { unsigned v; sscanf(h, "%2x", &v); *d = (uint8_t)v; } }

#define SIZE 0x8DFBCu
int main(void)
{
    static uint8_t area[0x120 + SIZE], keep[0x120 + SIZE];
    uint32_t i, a, b;
    uint8_t rec[112];
    /* the CRC16 of ota.c / fm1pkg_make.py (poly 0x1021, init 0): the XMODEM check value */
    check("crc16 check value", ota_ck_crc16(0, (const uint8_t *)"123456789", 9) == 0x31C3u);
    memset(area, 0xFF, sizeof area);
    hex(area, "72B3F73F200100025BE2080083FF00006170705F617265615F6865616400FFFF");   /* app_area_head, as read off the unit */
    hex(area + 0x20, "A1E5E15920010000BCDF080082FF00006170702E62696E00FFFFFFFFFFFFFFFF");   /* app.bin: crc 0x59E1, offset 0x120, size 0x8DFBC */
    check("the real app.bin entry's own CRC16 is right", ota_ck_crc16(0, area + 0x22, 30) == 0xE5A1u);
    srand(7);
    for (i = 0; i < SIZE; i++) area[0x120 + i] = (uint8_t)(rand() >> 7);
    /* make the body hit the entry's CRC (0x59E1): the CRC of all but the last two bytes once, then try those two */
    { uint16_t pre = ota_ck_crc16(0, area + 0x120, SIZE - 2);
      for (a = 0; a < 256; a++) for (b = 0; b < 256; b++) {
        uint8_t t[2] = {(uint8_t)a, (uint8_t)b};
        if (ota_ck_crc16(pre, t, 2) == 0x59E1u) { area[0x120 + SIZE - 2] = t[0]; area[0x120 + SIZE - 1] = t[1]; goto found; }
      } }
    printf("FAIL could not forge a matching body\n"); return 1;
found:
    memcpy(keep, area, sizeof area);
    check("whole image: 1", ota_ck_image(area, 0x90000u) == 1);
    /* an install cut off after the first sectors: new first part, old rest (here: a changed byte beyond them) */
    area[0x120 + 0x30000] ^= 0x10; check("old rest: 0", ota_ck_image(area, 0x90000u) == 0); memcpy(area, keep, sizeof area);
    area[0x120 + 5] ^= 1; check("first sector differs: 0", ota_ck_image(area, 0x90000u) == 0); memcpy(area, keep, sizeof area);
    area[0x120 + SIZE - 1] ^= 0x80; check("last byte: 0", ota_ck_image(area, 0x90000u) == 0); memcpy(area, keep, sizeof area);
    memset(area + 0x120 + 0x20000, 0xFF, 0x1000); check("an erased sector: 0", ota_ck_image(area, 0x90000u) == 0); memcpy(area, keep, sizeof area);
    /* headers it must not judge: -1 (then the boot goes on as before) */
    area[0x20 + 5] ^= 1; check("entry damaged: -1", ota_ck_image(area, 0x90000u) == -1); memcpy(area, keep, sizeof area);
    area[0x20 + 16] = 'x'; check("not app.bin: -1", ota_ck_image(area, 0x90000u) == -1); memcpy(area, keep, sizeof area);
    check("slot smaller than the size: -1", ota_ck_image(area, 0x80000u) == -1);
    memset(area, 0xFF, 0x120); check("blank header: -1", ota_ck_image(area, 0x90000u) == -1); memcpy(area, keep, sizeof area);
    /* the update record ota.c writes (UPDATA_PARM, 112 B) */
    memset(rec, 0, sizeof rec);
    rec[2] = 0x0D; rec[3] = 0x5A; rec[4] = 0x01; rec[5] = 0x5A; rec[6] = 0x41; rec[7] = 0x54;
    memcpy(rec + 8, "ota-FM-1_015", 12); rec[74] = 0x0E;
    { uint16_t c = ota_ck_crc16(0, rec + 2, 78); rec[0] = (uint8_t)c; rec[1] = (uint8_t)(c >> 8); }
    check("record: 1", ota_ck_record(rec) == 1);
    rec[30] ^= 1; check("record damaged: 0", ota_ck_record(rec) == 0); rec[30] ^= 1;
    memset(rec, 0xFF, sizeof rec); check("erased record: 0", ota_ck_record(rec) == 0);
    memset(rec, 0, sizeof rec); check("zero record: 0", ota_ck_record(rec) == 0);
    printf(bad ? "OTA CHECK FAILED\n" : "OTA CHECK PASSED\n");
    return bad != 0;
}
