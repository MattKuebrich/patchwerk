/* SPDX-License-Identifier: MPL-2.0 */
/* BETA: the hardware side of the interrupted-install check (ota_check.h has the verdicts). Included by device/unity.c after
 * fm1_flash.h, fm1_xip.h and fm1_sys.h. All inline, no globals, no calls: it runs first thing in fm1_cstart, before .data is
 * copied, from the first sectors of the image (the only part a cut-off install is sure to have written new). */
#ifndef OTA_EARLY_H
#define OTA_EARLY_H
#include "ota_check.h"

#define FM1_OTA_EARLY_MAGIC 0x0C4E0000u   /* fm1_ota_early (noinit): magic | b0 record seen | b1 image judged | b2-3 verdict+1 | b8 sent back */

/* 1 = send the unit back to the loader with `parm` (the record in flash). *st is the noinit word. */
FM1_INLINE int fm1_ota_early_check(uint32_t *st, uint8_t *parm)
{
    const volatile uint8_t *rec = (const volatile uint8_t *)FM1_XIP(0xE4F00u);
    const volatile uint8_t *stg = (const volatile uint8_t *)FM1_XIP(0xE0000u);
    uint32_t w = *st, acted = ((w >> 16) == (FM1_OTA_EARLY_MAGIC >> 16)) ? (w & 0x100u) : 0u, i;
    int v;
    *st = FM1_OTA_EARLY_MAGIC | acted;
    SFCENC_CON &= (uint8_t)~2u;                     /* the data area as a plain window (fl_plain_window_init, but not a call) */
    SFCENC_UNENC_L = FM1_XIP(0x93000u);
    SFCENC_UNENC_H = 0x07FFFFFFu;
    SFCENC_CON |= 2u;
    fl_inval(0xE0000u, 0x5000u);
    if (!ota_ck_record(rec))
        return 0;                                   /* no record: the usual boot */
    *st |= 1u;
    if ((stg[0] & stg[1] & stg[2] & stg[3]) == 0xFFu)
        return 0;                                   /* no staged loader to go back to */
    v = ota_ck_image((const volatile uint8_t *)FM1_XIP(0x4000u), 0x90000u);
    *st |= 2u | ((uint32_t)(v + 1) << 2);
    if (v != 0 || acted)
        return 0;                                   /* whole, not judged, or already sent back once since power-on */
    *st |= 0x100u;
    for (i = 0; i < 112u; i++)
        parm[i] = rec[i];
    return 1;
}

/* the running app against its header, now (diagnostics): 1 whole, 0 not, -1 not judged */
FM1_INLINE int fm1_ota_image_now(void)
{
    return ota_ck_image((const volatile uint8_t *)FM1_XIP(0x4000u), 0x90000u);
}
#endif
