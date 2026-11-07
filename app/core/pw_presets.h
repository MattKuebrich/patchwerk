/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PW_PRESETS_H
#define PW_PRESETS_H
#define PW_FACTORY_COUNT 50
const char *pw_factory_name(unsigned index);
int pw_factory_build(unsigned index);
void pw_random_name(char out[13]);
int pw_random_patch(void);                /* ROLL: a random patch that plays */        /* two words for a new patch */
/* The add list's example page: a small patch per module type (pw_examples.inc). */
const char *pw_type_help(unsigned type);                    /* a module's description (valid until the next call) */
const char *pw_example_text(unsigned type);                 /* what it does, 0 none */
void pw_example_uses(unsigned type, char *out, unsigned cap);   /* the modules in it: "KEYS VCO ENV VCA OUT" */
int  pw_example_build(unsigned type);                         /* the patch becomes it: 0 done, -1 none, -2 a recipe mistake */
#endif
