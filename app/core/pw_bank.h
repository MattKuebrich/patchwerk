/* SPDX-License-Identifier: GPL-3.0-only
 * PATCHWERK's sample bank in the FM-1's flash: PW_BANK_SLOTS slots: two banks of sounds a SAMPLER, GRANULAR and the DRUM RACK play (A = slots 0-15, B = 24-39; INTERNAL is built into the firmware), then the CZ and FM-DX banks; sent from the
 * web page over USB. A directory sector, then the sounds, each from a 4 KiB sector boundary.
 *
 * Directory (one sector): "NBK1", the entry count (40, u32; a directory with 16, 17, 20 or 24 entries from an older version is read), a CRC-32 of the entries (u32), 4 bytes spare; then
 * per slot 32 bytes: name[12], offset u32 (in the flash), frames u32, rate u32 (Hz), CRC-32 of the stored bytes u32,
 * codec u8 (1: SEA; 0 or 0xFF: samples), 3 bytes spare. Samples: signed 16-bit mono, little-endian; SEA: the chunk
 * stream of pw_sea.inc (about 3.4 bits a frame, frames a multiple of 20). A slot with 0 or 0xFFFFFFFF frames is empty.
 *
 * Receiving a sound (pw_bank_receive): the slot is emptied in the directory first, its sectors erased, the samples
 * pulled from the host 512 bytes at a time (the update's read requests), checked, and only then listed. Power lost
 * half way leaves the slot empty, never a half sound. */
#ifndef PW_BANK_H
#define PW_BANK_H
#include <stdint.h>

#define PW_BANK_DIR     0xA7000u            /* (the user patches end here: app.h APP_STORE_MAP) */
#define PW_BANK_DATA_LO 0xA8000u
#define PW_BANK_DATA_HI 0xDC000u            /* (the panel calibration: OBJ_PANEL) */
/* two more windows of the data area (plat.h PLAT_XDATA0 and PLAT_XDATA1: the FM-1 is a 1 MiB part): sounds go there when the main
 * window is full. The update loader clears an update record it finds at bytes 0xF00..0xF07 of a sector there ("AT" at 0xF06), so a
 * sound that would put "AT" there is refused (PW_BANK_E_REC): about 1 in 65,000 sectors. */
#define PW_BANK_X0_LO   0x93000u            /* 16 KB just above the app area */
#define PW_BANK_X0_HI   0x97000u
#define PW_BANK_X1_LO   0xE5000u            /* 92 KB above the update's staging (and the autosave Felucca keeps there) */
#define PW_BANK_X1_HI   0xFC000u
#define PW_BANK_ROOM    ((PW_BANK_DATA_HI - PW_BANK_DATA_LO) + (PW_BANK_X0_HI - PW_BANK_X0_LO) + (PW_BANK_X1_HI - PW_BANK_X1_LO))   /* 316 KB */
/* slots past the 40 the page sees: the SAMPLER's RAM sound kept with a user patch (patch u, its k-th sampler: slot 40 + 2u + k), a 4-bit ADPCM stream of at most a second (pw_ram_save / pw_ram_load) */
#define PW_BANK_RAM0  40u
#define PW_BANK_TOTAL 56u
enum { PW_BANK_OK = 0, PW_BANK_E_ARG = 1, PW_BANK_E_FULL = 2, PW_BANK_E_FLASH = 3, PW_BANK_E_HOST = 4, PW_BANK_E_CRC = 5, PW_BANK_E_REC = 6 };

void pw_bank_init(void);                     /* reads the directory */
uint32_t pw_bank_free(void);                 /* bytes in the largest free gap (a sound must fit in one) */
uint32_t pw_bank_free_total(void);           /* bytes free in all the windows */
int pw_bank_receive(unsigned slot, uint32_t frames, uint32_t rate, const char *name, uint32_t crc, unsigned codec);   /* codec 1: SEA, frames a multiple of 20 */
uint32_t pw_bank_stored(unsigned slot, uint32_t *frames, unsigned *codec, const uint8_t **at);   /* its bytes as stored (0: empty) */
int pw_bank_delete(unsigned slot);
void pw_ram_save(unsigned patch);            /* SAMPLERs' RAM sounds of the patch that is being stored into user slot `patch` (an empty RAM frees its slots) */
void pw_ram_load(unsigned patch);            /* ... and back, after the patch is loaded */
void pw_ram_erase(unsigned patch);
int pw_bank_erase_all(void);
int pw_bank_erase_kind(unsigned kind);   /* 1 the sounds, 2 the CZ banks, 3 the FM-DX banks */
/* the slot's listing: 20 bytes (frames u32, rate u32, name[12]) */
void pw_bank_entry(unsigned slot, uint8_t out[20]);

/* platform: the flash (offsets as in the directory), the host's data, a progress report */
const uint8_t *pw_plat_flash(uint32_t off);
int pw_plat_flash_erase(uint32_t off);       /* a 4 KiB sector; 0 = done */
int pw_plat_flash_write(uint32_t off, const void *src, uint32_t n);   /* 0 = done */
int pw_plat_host_read(uint32_t addr, uint8_t *dst, uint32_t n);       /* n <= 512; 0 = done */
void pw_plat_progress(const char *what, unsigned percent);
#endif
