/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (the entry: base/bsp/chip/fm1_cpu1.S)
 * Written against the public JieLi AC79 SDK (Apache-2.0); see base/bsp/NOTICE.
 * PATCHWERK's second core, EXPERIMENTAL (docs/EXPERIMENTAL.md of the BSP: it has never run on a device). The entry is the BSP's; the
 * two stacks (the SDK's sizes: 0x300 user, 0x1000 supervisor) are here, in the pool, because the linker script has no names for them. */
	.section .pool,"aw",@nobits
	.balign 8
	.globl _cpu1_ustack_top
	.globl _cpu1_sstack_top
_cpu1_ustack:
	.globl _cpu1_ustack
	.skip 0x1000
_cpu1_ustack_top:
_cpu1_sstack:
	.skip 0x1000
_cpu1_sstack_top:

	.section .text.fm1_cpu1,"ax",@progbits
	.globl fm1_cpu1_entry
fm1_cpu1_entry:
	cli
	icfg = 0
	ssp = _cpu1_sstack_top
	usp = _cpu1_ustack_top
	sp = _cpu1_sstack_top
	reti = fm1_cpu1_main
	rti
