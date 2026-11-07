/* SPDX-License-Identifier: GPL-3.0-only
 * Builds every factory patch from tools/factory_src.inc and saves each as the engine saves a patch: <dir>/NN.bin, and
 * <dir>/plays.txt (the numbers of the patches that play at once). tools/gen_factory.py packs them. Run by tools/gen_factory.sh. */
#include <stdio.h>
#define main host_test_main
#include "../tests/host_test.c"   /* (the platform's stand-ins for the host) */
#undef main
int main(int argc, char **argv)
{
    static uint8_t buf[8192];
    char fn[512];
    if (argc < 2) return 2;
    pw_init();
    snprintf(fn, sizeof fn, "%s/plays.txt", argv[1]);
    FILE *pl = fopen(fn, "w");
    for (unsigned i = 0; i < PW_FACTORY_COUNT; ++i) {
        if (pw_factory_build(i) != 0) { fprintf(stderr, "patch %u: build failed\n", i); return 1; }
        int n = pw_save(buf, sizeof buf);
        if (n <= 0) { fprintf(stderr, "patch %u: save failed\n", i); return 1; }
        if (pws.playing) fprintf(pl, "%u\n", i);
        snprintf(fn, sizeof fn, "%s/%02u.bin", argv[1], i);
        FILE *f = fopen(fn, "wb"); fwrite(buf, 1, (size_t)n, f); fclose(f);
    }
    fclose(pl);
    return 0;
}
