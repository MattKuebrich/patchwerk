/* SPDX-License-Identifier: MPL-2.0 */
/* The handshake's identity (M-UPGRADE 3.11 takes only NAME_ and one to three digits): the plain query is answered with the
 * name and three digits, the installers' query (the same cmd with the body 01) with the whole identity, and the update
 * loader's own identity (ota-FM-1_015, already short) is the same either way. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_606010000"
#endif
#define FELUCCA_OTA_DRYRUN 0
static uint8_t rx[64];
static uint32_t rx_len, rx_full, replies;
static uint8_t last_id[40];
static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) { acc |= (uint32_t)*in++ << nb; nb += 8; while (nb >= 7) { out[o++] = acc & 0x7F; acc >>= 7; nb -= 7; } }
    if (nb) out[o++] = acc & 0x7F;
    return o;
}
static uint32_t unpack7h(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) { acc |= (uint32_t)*in++ << nb; nb += 7; if (nb >= 8) { out[o++] = (uint8_t)acc; acc >>= 8; nb -= 8; } }
    return o;
}
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    uint8_t u[64];
    unpack7h(p + 1, n - 2, u);
    if (u[2] == 0x11) { replies++; memcpy(last_id, u + 6, 27); last_id[27] = 0; }
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n) { if (!rx_full) return 0; *p = rx; *n = rx_len; return 1; }
static void ota_frame_done(void) { rx_full = 0; }
static uint32_t ota_now_ms(void) { return 0; }
static void ota_idle(void) {}
static int ota_erase(uint32_t off) { (void)off; return -1; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static int ota_fread(uint32_t off, void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; }
#include "../base/bsp/chip/ota.c"

static int bad;
static void check(const char *what, int ok) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); bad += !ok; }
static void ask(const uint8_t *m, uint32_t n) { rx_len = pack7(m, n, rx); rx_full = 1; replies = 0; ota_service(); }

int main(void)
{
    uint8_t plain[7] = {0, 0x59, 0x11, 0, 0, 0, 0xFF};
    uint8_t full[8] = {0, 0x59, 0x11, 1, 0, 0, 1, 0xFE};
    uint8_t other[8] = {0, 0x59, 0x11, 1, 0, 0, 2, 0xFD};   /* some other body: not the installers' */
    const char *want_short = "FM-1_606";
    char id[40];
    ask(plain, 7);
    snprintf(id, sizeof id, "%s", (char *)last_id);
    check("plain query answered", replies == 1);
    check("plain query: name + three digits only", !strcmp(id, sizeof FELUCCA_ID > 13 ? want_short : FELUCCA_ID));
    {
        const char *u = strchr(id, '_');
        int digits = u ? (int)strlen(u + 1) : 99;
        check("plain query: one to three digits (M-UPGRADE 3.11)", u && digits >= 1 && digits <= 3);
    }
    ask(full, 8);
    snprintf(id, sizeof id, "%s", (char *)last_id);
    check("installers' query answered", replies == 1);
    check("installers' query: the whole identity " FELUCCA_ID, !strcmp(id, FELUCCA_ID));
    ask(other, 8);
    check("another body: not answered", replies == 0);
    printf(bad ? "OTA IDENT FAILED\n" : "OTA IDENT PASSED\n");
    return bad;
}
