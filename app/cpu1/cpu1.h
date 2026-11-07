/* PATCHWERK's second core, EXPERIMENTAL: its start (register level, so it lives here: the build's register check leaves cpu1/ alone, fm1.toml 'vendored').
 * After Melodee's (keremimo/melodee, GPL-3.0-only, firmware/hal/fm1_multicore.h, fm1_core1_stop.h), which ships two cores on the FM-1. Included by app.c. */
#pragma once
#include <stdint.h>
static volatile uint32_t cpu1_count;
static volatile uint32_t cpu1_flag;
static volatile uint32_t cpu1_stage;       /* how far CPU1 got: 1 in main, 2 stack window set, 3 reported in */
static uint8_t cpu1_state;
extern uint8_t _cpu1_ustack[], _cpu1_sstack_top[];
extern void fm1_cpu1_entry(void);
#define C1_REG(a) (*(volatile uint32_t *)(a))
/* CPU1's loop, in RAM, calls nothing. As Melodee's (keremimo/melodee, GPL-3.0, firmware/hal/fm1_multicore.h): CPU1 has its own debug bank
 * (0x1EEF2xx), and its stack window is set here, else its first push can count as an overflow and fault it before it reports in. */
/* modules' blocks for CPU1 (the engine's offload): a ring of jobs run in the order posted; a ticket is the job's number, done when c1_done has reached it */
typedef struct {
    void (*fn)(void *, const float *const *, float *const *, unsigned);
    void *m;
    const float *const *in;
    float *const *out;
    unsigned conn;
    uint32_t tk;
} c1_ring_t;
static c1_ring_t c1_ring[16];
static volatile uint32_t c1_posted, c1_done;
__attribute__((section(".c1_text"), noinline, used)) void fm1_cpu1_main(void)
{
    uint32_t lo = (uint32_t)(uintptr_t)_cpu1_ustack, hi = (uint32_t)(uintptr_t)_cpu1_sstack_top - 1u;
    cpu1_stage = 1;
    C1_REG(0x1EEF2D8u) = hi;
    C1_REG(0x1EEF2DCu) = lo;
    C1_REG(0x1EEF2E0u) = hi;
    C1_REG(0x1EEF2E4u) = lo;
    C1_REG(0x1EEF2D0u) = (C1_REG(0x1EEF2D0u) & ~((1u << 2) | (0x1Fu << 16))) | (1u << 3);
    cpu1_stage = 2;
    cpu1_flag = 1;
    uint32_t spins = 0;
    for (;;) {
        /* idle gently: a short register-only pause between looks at the shared RAM (as Melodee's: a core polling flat out slows the other one's memory) */
        for (uint32_t i = 0; i < 16u; i++) __asm__ volatile("nop; nop; nop; nop" ::: "memory");
        if (++spins >= 64u) { spins = 0; cpu1_count = cpu1_count + 64u; }
        while (c1_done != c1_posted) {
            c1_ring_t *e = &c1_ring[(c1_done + 1u) & 15u];
            __asm__ volatile("csync" ::: "memory");
            uint32_t g0 = *(volatile uint32_t *)0x10804u;
            e->fn(e->m, e->in, e->out, e->conn);
            e->tk = *(volatile uint32_t *)0x10804u - g0;
            __asm__ volatile("csync" ::: "memory");
            c1_done = c1_done + 1u;
        }
    }
}
/* Hold CPU1: bus-error traps masked while it is stopped (a held core can leave a late bus flag on CPU0), as Melodee's fm1_core1_stop (from Felucca). */
static void cpu1_hold(void)
{
    uint32_t locked = !(C1_REG(0x1EEE240u) & 1u), en, bus = 0x003F0030u;
    volatile uint32_t n;
    __asm__("" : "+r"(bus));
    if (locked) C1_REG(0x1EEE240u) = 0xE7u;
    en = C1_REG(0x1EEE340u);
    C1_REG(0x1EEE340u) = en & ~bus;
    __asm__ volatile("csync" ::: "memory");
    C1_REG(0x1EEE004u) |= 2u;
    C1_REG(0x1EEE004u) &= ~8u;
    __asm__ volatile("csync" ::: "memory");
    for (n = 0; n < 256u; n++) ;
    C1_REG(0x1EEE248u) = bus;
    __asm__ volatile("csync" ::: "memory");
    C1_REG(0x1EEE340u) = en;
    if (locked && (C1_REG(0x1EEE240u) & 1u)) C1_REG(0x1EEE240u) = 0xE7u;
    __asm__ volatile("csync" ::: "memory");
}
static int cpu1_launch(void)
{
    uint32_t div = C1_REG(0x10008u), t = 0, i, f;
    cpu1_flag = 0;
    cpu1_stage = 0;
    f = irq_save();
    cpu1_hold();                              /* (held first, as Melodee's fm1_core1_stop before its start) */
    /* CPU1 begins in the chip's mask ROM, outside the PC limits: with them set it faults at once (DBG bit 10, c1_pc_limit_err; Melodee's first
     * finding). They are opened for the start and set again after it. */
    fm1__dbg_unlock();
    FM1_PC_LIMIT0_L = 0u; FM1_PC_LIMIT0_H = 0xFFFFFFFFu;
    FM1_PC_LIMIT1_L = 0u; FM1_PC_LIMIT1_H = 0xFFFFFFFFu;
    FM1_DBG_MSG_CLR = 0xFFFFFFFFu;
    fm1__dbg_lock();                           /* (only the register writes; the wait below runs with the interrupts on, so the sound's buffers are refilled) */
    for (i = 0; i < 32u; i++) C1_REG(0x1EEF300u + 4u * i) = 0;   /* (CPU1's interrupt bank: all off) */
    C1_REG(0x01C7FFF8u) = (uint32_t)(uintptr_t)fm1_cpu1_entry;
    C1_REG(0x10008u) = div | 8u;
    __asm__ volatile("csync" ::: "memory");
    C1_REG(0x1EEE004u) |= 8u;
    C1_REG(0x1EEE004u) &= ~2u;
    irq_restore(f);
    while (!cpu1_flag && t < 100000u) { fm1_delay_us(1); t++; }
    f = irq_save();
    if (!cpu1_flag) cpu1_hold();
    C1_REG(0x10008u) = div;
    fm1_guard_enable(FM1_GUARD_PC);           /* (the limits again) */
    irq_restore(f);
    return cpu1_flag ? 0 : -1;
}
int pw_plat_cpu1(uint32_t *count, int start)
{
    if (start && !cpu1_state) {
        pw_plat_lock();                       /* (the main loop waits; the sound goes on) */
        fm1_guard_unlock_top();               /* (the entry word is in the reserved top of RAM) */
        int r = cpu1_launch();
        fm1_guard_lock_top();
        pw_plat_unlock();
        if (r) cpu1_state = 2;
        else { uint32_t a = cpu1_count; fm1_delay_us(5000u); cpu1_state = cpu1_count != a ? 1 : 3; }
    }
    if (count) *count = cpu1_count;
    return cpu1_state;
}
void pw_plat_cpu1_jobs(uint32_t *w) { w[0] = c1_done; }
uint32_t pw_plat_cpu1_post(pw_cpu1_fn fn, void *m, const float *const *in, float *const *out, unsigned connected)
{
    if (cpu1_state != 1) return 0;
    uint32_t t = c1_posted + 1u;
    c1_ring_t *e = &c1_ring[t & 15u];
    e->fn = fn; e->m = m; e->in = in; e->out = out; e->conn = connected;
    __asm__ volatile("csync" ::: "memory");
    c1_posted = t;
    return t;
}
int pw_plat_cpu1_wait(uint32_t ticket, uint32_t *us)
{
    uint32_t t0 = pw_plat_ticks();
    while ((int32_t)(c1_done - ticket) < 0)
        if ((uint32_t)(pw_plat_ticks() - t0) > 6000u * pw_plat_ticks_per_us()) return -1;
    __asm__ volatile("csync" ::: "memory");
    if (us) *us = c1_ring[ticket & 15u].tk / pw_plat_ticks_per_us();
    return 0;
}
void pw_plat_cpu1_stop(void) { cpu1_hold(); cpu1_state = 3; }
static void cpu1_quiesce(void)   /* before the flash is written or erased: CPU1 must not be inside flash code */
{
    uint32_t t0 = pw_plat_ticks();
    if (cpu1_state != 1) return;
    while (c1_done != c1_posted && (uint32_t)(pw_plat_ticks() - t0) < 6000u * pw_plat_ticks_per_us()) ;
}
pw_cpu1_fn pw_plat_cpu1_dummy(void) { return 0; }
