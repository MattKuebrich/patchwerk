/* SPDX-License-Identifier: GPL-3.0-only
 * PATCHWERK's live link: the web page's patch bay and the FM-1 kept the same patch, both ways, over USB-MIDI SysEx.
 *
 * Both ends run this same code on their own engine. Each keeps a "shadow": what it believes the other end has.
 * Every poll it compares its live patch with the shadow and sends the differences as operations (remove, add,
 * connect, disconnect, parameter, sequencer steps, position, tempo / transport / name), updating the shadow as
 * it sends. What arrives is applied to the live patch and written to the shadow as the other end claimed it: if
 * applying it did not work out the same (a full FM-1, a value out of range), the next poll sees the difference
 * and sends the correction back. So the two converge whatever happens first, and nothing echoes.
 *
 * Frames: F0 7D 'N' 'L' pack7(payload) F7 (7D: the non-commercial manufacturer id; pack7: felucca-bsp's 8-to-7,
 * LSB first). Payload: kind, seq, body.
 *   host -> device  'H' hello (version): the device forgets its shadow (so it sends its whole patch) and answers
 *                       'h' (version, its module and cable capacity); the host cleared its own patch first
 *                   'U' operations; answered 'K' seq. One frame in flight (the FM-1 keeps one SysEx frame from the
 *                       host): the next only after the answer, the same again after 400 ms without one (the
 *                       device applies a seq once). An empty 'U' every 2 s keeps the link alive (5 s: it is off)
 *   device -> host  'U' operations (seq counts them: a gap, and the host says hello again), 'V' meters (output
 *                       levels, sequencer steps) ten times a second
 * Operations: 1 remove slot | 2 add slot type x16 y16 | 3 disconnect sm so dm di | 4 connect sm so dm di |
 *             5 param slot p f32 | 6 data slot [32] | 7 move slot x16 y16 | 8 global bpm f32, playing, name[12] |
 *             9 resync (device -> host: more arrived than it could keep or undo one by one; the host says hello) */
#ifndef PW_LINK_H
#define PW_LINK_H
#include <stdint.h>
enum { PW_LINK_DEVICE, PW_LINK_HOST };
#define PW_LINK_VERSION 7
#define PW_LINK_WIRE_MAX 600u          /* bytes in a frame, F0..F7 (the FM-1 takes 640 from the host) */

void pw_link_init(int role);
int pw_link_rx(const uint8_t *p, unsigned n, uint32_t now_ms);   /* p: the bytes between F0 and F7; 1 = the link's */
void pw_link_poll(uint32_t now_ms);  /* sends what changed (through pw_plat_link_send); main loop */
int pw_link_active(void);            /* frames from the other end lately (the host: and its hello answered) */
void pw_link_hello(uint32_t now_ms); /* host: (re)start: clears the patch, asks for the device's */
int pw_link_capacity(unsigned *modules, unsigned *cables);   /* host: the device's (0 before its answer) */
int pw_link_remote_step(unsigned slot);   /* host: the device's sequencer step for that slot (-1: none) */
uint32_t pw_link_stats(unsigned what);    /* 0 frames sent, 1 received, 2 ops sent, 3 ops applied, 4 resends, 5 gaps */
/* platform: one whole frame (F0 .. F7) out */
void pw_plat_link_send(const uint8_t *frame, unsigned n);
#endif
