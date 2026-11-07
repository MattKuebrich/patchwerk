/* SPDX-License-Identifier: MPL-2.0 */
/* The flash chips the FM-1's storage and update path accept (JEDEC id from command 0x9F).
 * 0x856014: Puya P25Q80, 1 MiB, in the units up to 2026.
 * 0xB36014: another maker's 1 MiB part with the same type and capacity bytes, in the FM-1s sold from late 2026 (the
 *           M-VAVE logo on the front). A firmware that accepts only the first runs without the flash on these units:
 *           it keeps nothing and takes no update. (First found by the ChoralRoot project; the driver uses only
 *           generic SPI NOR commands, which the official firmware uses on the same chip.)
 * Extend it here for another part. */
#ifndef FM1_FLASH_ID_H
#define FM1_FLASH_ID_H
#define FL_JEDEC_OK(id) ((id) == 0x856014u || (id) == 0xB36014u)
#endif
