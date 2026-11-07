/* SPDX-License-Identifier: GPL-3.0-only */
/* <string.h> for the FM-1 build: the JieLi toolchain has no C library headers. memset, memcpy and
 * memcmp are felucca-bsp's chip/libc.c; memmove and strlen are app.c's. */
#ifndef PW_DEV_STRING_H
#define PW_DEV_STRING_H
#include <stddef.h>
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
void *memmove(void *d, const void *s, unsigned n);
unsigned strlen(const char *s);
#endif
