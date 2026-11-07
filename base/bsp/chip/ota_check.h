/* SPDX-License-Identifier: MPL-2.0 */
/* BETA: is the app in flash whole? (the check that finds an interrupted install)
 *
 * An install that is cut off partway leaves the first sectors of the new image
 * (the loader writes in order) and the old rest. That mixed image can hang
 * before any recovery code runs, and the chip's boot code does not start the
 * loader from the record in flash on its own: only a record in RAM, set by a
 * running app, does that. So the new image checks itself, as the very first
 * thing it does, and sends the unit back to the loader it left staged.
 *
 * Everything here is pure: bytes in, verdict out, no globals and no calls, so it
 * can run before .data is copied (from a mixed image) and be tested on the host.
 * Each function is inline: nothing of it lives in a section the cut-off
 * install may have left half old.
 *
 * `area` is the flash from 0x4000 as the CPU sees it (the XIP window, decrypted):
 *   +0x00  app_area_head entry (32 B)
 *   +0x20  app.bin entry:  crc16(body) u16, then body = data crc16 u16, offset u32, size u32,
 *          flags u8, reserved u8, index u16, name[16]   (tools/fm1pkg_make.py entry())
 *   +0x120 app.bin, `size` bytes, CRC16 (poly 0x1021, init 0) = the entry's data crc
 */
#ifndef OTA_CHECK_H
#define OTA_CHECK_H
#include <stdint.h>

#ifndef OTA_CHECK_FEED
#define OTA_CHECK_FEED() ((void)0)        /* the watchdog, on the device */
#endif
#define OTA_CK_INL static inline __attribute__((always_inline))

OTA_CK_INL uint16_t ota_ck_crc16(uint16_t c, const volatile uint8_t *p, uint32_t n)
{
    uint32_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= (uint16_t)((uint16_t)p[i] << 8);
        for (k = 0; k < 8; k++)
            c = (uint16_t)((c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1));
        if (!(i & 0xFFFu))
            OTA_CHECK_FEED();
    }
    return c;
}

/* 1 = the app matches its header; 0 = it does not (cut-off install); -1 = cannot judge
 * (a header that is not the expected layout: then nothing is done) */
OTA_CK_INL int ota_ck_image(const volatile uint8_t *area, uint32_t slot_max)
{
    uint8_t e[32];
    uint32_t i, off, size;
    uint16_t dcrc;
    for (i = 0; i < 32u; i++)
        e[i] = area[0x20u + i];
    if (e[16] != 'a' || e[17] != 'p' || e[18] != 'p' || e[19] != '.' || e[20] != 'b' || e[21] != 'i' || e[22] != 'n' || e[23] != 0)
        return -1;
    if (ota_ck_crc16(0, e + 2, 30) != (uint16_t)(e[0] | e[1] << 8))
        return -1;                             /* the header entry itself is damaged: not judged */
    dcrc = (uint16_t)(e[2] | e[3] << 8);
    off = (uint32_t)e[4] | (uint32_t)e[5] << 8 | (uint32_t)e[6] << 16 | (uint32_t)e[7] << 24;
    size = (uint32_t)e[8] | (uint32_t)e[9] << 8 | (uint32_t)e[10] << 16 | (uint32_t)e[11] << 24;
    if (off != 0x120u || size < 0x1000u || size > slot_max)
        return -1;
    return ota_ck_crc16(0, area + off, size) == dcrc;
}

/* The update record kept in flash (112 B, ota.c): 1 = a valid record that points at the staged loader */
OTA_CK_INL int ota_ck_record(const volatile uint8_t *r)
{
    uint8_t p[112];
    uint32_t i;
    for (i = 0; i < 112u; i++)
        p[i] = r[i];
    if (ota_ck_crc16(0, p + 2, 78) != (uint16_t)(p[0] | p[1] << 8))
        return 0;
    if (p[2] != 0x0Du || p[3] != 0x5Au || p[4] != 0x01u || p[5] != 0x5Au)
        return 0;                              /* 0x5A0D, 0x5A01 (ota.c) */
    if (p[72] != 0 || p[73] != 0 || p[74] != 0x0Eu || p[75] != 0)
        return 0;                              /* the staged loader at 0xE0000 */
    return 1;
}
#endif
