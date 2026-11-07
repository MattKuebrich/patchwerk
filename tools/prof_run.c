/* SPDX-License-Identifier: GPL-3.0-only
 * Renders a factory patch for a while (the host's engine); tools/cpu_profile.sh runs it under valgrind's callgrind. */
#include <stdio.h>
#include <stdlib.h>
#define main host_test_main
#include "../tests/host_test.c"   /* (the platform's stand-ins for the host) */
#undef main
int main(int argc, char **argv)
{
    pw_init();
    unsigned p = argc > 1 ? (unsigned)atoi(argv[1]) : 0;
    if (pw_factory_build(p) != 0) return 1;
    if (getenv("CTL")) pws.ctl_rate = 1;
    pw_set_playing(1);
    for (int b = 0; b < 600; ++b) pw_render(L + (b % 1000) * PW_N, R + (b % 1000) * PW_N);
    return 0;
}
