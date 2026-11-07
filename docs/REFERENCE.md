# PATCHWERK reference

The long form: every control, module and setting, and the older release notes (the newest are in `CHANGELOG.txt`). `README.md`
is the short version; the unit's own HELP page and `cheatsheet.html` list the shortcuts.

## Brand colors (one place)

`brand/brand.json` holds the colors that make PATCHWERK look like itself: the accent, the seven module-group colors, ok / warning / error, the five cables of the mark and their order, and the name. Edit it and run `sh tools/gen_brand.sh` (the release does too; the tests fail if the generated files are out of date). They reach: the FM-1's default palette (the "brand" row in core/pw_ui.c, set as PW_PAL_DEFAULT), the update-mode screen and the update screen (base/bsp/loader, base/device/unity.c), the site's mark and tab icon, and the page's colors and cables. The other palettes are their own; the page's own module cards use the group colors.

## On the FM-1

HOME is the patch: every module a box, laid out by signal flow, with its inputs on the left edge and outputs on
the right and every cable drawn. The HELP page (SAVE pages through the menu to it, or ALGORITHM from the patch list) shows this list on the screen.

**A button pressed again turns to its next page.** FX: the add list's pages (what it is, example patch, visual). ENV: PERFORM, then MUTES, then MUTATE (HOME leaves). SEQ: the next sequencer. EDIT: the module's connections, then its faces. SAVE: the menu's pages in turn (SAVE, SETTINGS, HELP, PATCHES). ARP and LFO (MATRIX and SONG are single pages) go back. ALGORITHM turns pages as well, and on the patch screen a tap of HOME steps the selected module's knob pages.

| Control | |
| --- | --- |
| SELECT | the cursor, jack to jack; turned fast (about ten detents a second), module to module (GLO + SELECT: module to module at any speed) |
| ALGORITHM | the module's knob pages (the ALGO plate and a numbered box for each), on the patch screen as on the module page. (Experiment, v0.96: it used to open the small add list; see docs/EXPERIMENTS.md to go back.) |
| FX | add a module right where the cursor is: ADD MODULE, with a thin strip of the patch along the top showing where it would go (a dashed ghost of the new module and dashed green cables for what SEL would wire; the old cable it replaces is hidden behind it). SELECT browses the list, KNOB 2 jumps a category; SEL wires it in at the highlighted jack, ENV adds it unconnected, EDIT replaces the one there; ALGORITHM turns the pages: what it is (with the strip), EXAMPLE PATCH, VISUALIZATION |
| KNOB 1-4 | the parameters of the module under the cursor: about 40 detents across a range; GLO held, a tenth of that. The page follows the cursor where a jack has its own parameters (a VCF's FREQ: CUTOFF; a DRUM RACK's T3: pad 3; a SAMPLER's START: its region page); elsewhere it stays where you left it; ALGORITHM turns it |
| SEL | the thing on the screen: grab a cable at the cursor, turn to another jack (it stops only where the cable fits), SEL plugs it in (on an existing connection: removes it; on a group's box, with a cable in hand, it opens the group and the cable goes in with you). In the module list: add it. In the patch list: load. In the cable list: show it |
| REC | tapped: unplug a cable of the jack under the cursor (one a press, the one patched first first), or drop the cable in hand. Held (a moment): delete the module, no question asked (GLO + OCT- brings it back) |
| FX | every module in a long list, with each one's whole description, starting where a module fits the cursor's jack (filters and effects on an audio output, modulators on a control input), ALGORITHM jumps a category. The bar at the bottom shows where it would go: SEL wires it in there, ENV adds it unconnected |
| EDIT | the module page: a visualization of what it does (an envelope's shape, a filter's curve, the FM algorithm and its operators, an EQ's response, the drum grid...) lit where the current knob page acts, every parameter page at once for the rest. GLO + ALGORITHM turns its faces: what feeds each input, where each output goes, a demo patch |
| ARP | MATRIX: every module with its cables under it; patch, move cables, set their depth (attenuate, invert), delete, bypass, replace; again: back |
| LFO | SONG: snapshot clips and automation lanes on a bar grid (below); again: back |
| ENV | PERFORM: the performance page; ALGORITHM turns between its three pages (the ALGO plate and boxes 1, 2, 3): the eight knobs, MUTES with the lower keys, solo and MORPH between snapshots (KNOB 1), and MUTATE; in the add lists: the module unconnected |
| SEQ | the 16-step sequencer on the lower keys: KNOB 1 the step's note, KNOB 2-4 its ratchet (then TIE and GLIDE), chance and velocity (STEPS and GATE: the module page). No SEQUENCER but a TRIG SEQ: its grid |
| PRESETS | patches: new (a random two-word name), GENERATE PATCH (builds a unique patch generatively that plays), 50 factory (each with performance knobs and variations), 8 user slots; name, VOICES (knob 4); the tempo (knob 1; GLO+K1 on the patch screen too) |
| (PERFORM, ENV) | the performance page: eight knobs from anywhere (hold ENV and turn a knob to put it there); the first eight lower keys pick snapshots (SEL held: 9-16; Bar Sync, the last row of the page: they wait for the bar line); SEL held + a MIDI controller's knob: that CC moves the knob |
| lower key 1-8 | on PERFORM: recall snapshot 1-8 (SEL held: 9-16); on the patch screen, SEL + key recalls 1-8 |
| GLO + lower key 1-8 | on any page: SAVE the playing snapshot into that key (SEL held too: 9-16) |
| GLO + SAVE | knob edits reach all snapshots that share the value (ALL in the header), or only the current one |
| GLO + ENV / LFO | copy the module under the cursor / paste it |
| GLO + SEL | on the patch: grab the cable plugged in at the cursor, to plug it somewhere else |
| GLO + FX | group: suggests chains going to an output (SELECT turns through them, SEL makes one a group); on a group box it opens it, HOME leaves; with a cable in hand it opens the group with the cable (plug it into any jack inside: the box gets that jack) and HOME leaves with it (a cable grabbed inside becomes a new output of the box) |
| GLO + REC | ungroup |
| REC (on a SAMPLER's module page) | arm, then record IN into the RAM (a second at most), then stop (PLAY also stops it) |
| GLO + KNOB 1 | the tempo (the other knobs keep their fine turn) |
| GLO + PRESETS | the metronome: turn up for on, down for off; MET in the top bar (any page but the patch list) |
| GLO + OCT- / OCT+ | undo / redo |
| GLO + EDIT | the module in the voices (one per note) or shared |
| GLO + HOME | the keys play notes / are the BUTTON modules' (a patch with BUTTON and no KEYS module is always in buttons) |
| GLO + PLAY | bypass the module: an effect or filter passes its sound straight through, anything else goes quiet (BYP on its box; again: back on) |
| SAVE | the SAVE page (SAVE again: SETTINGS, HELP, PATCHES): SELECT a slot, SEL stores; K4 KIND stores it as a GROUP, which the patch list inserts into the open patch as one box |
| PLAY | the transport (sequencers, clocks, Euclid, Turing without a clock cable follow it) |
| GLO | the modifier: held, the knobs move in fine steps and the other buttons take their GLO job (with GLO LATCH on, tap it, then the key) |
| keys, OCT | the KEYS module (and ARP): pitch, gate, velocity, trigger |
| HOME at power-on | safe start: a blank patch (an OUTPUT alone) instead of user patch 1 (U1), and without the second core; nothing is deleted |

Signals: audio ±5 V, pitch 1 V/oct (0 V = C4), gates 0/5 V. Several cables into one input add up.

**The drum grid.** A TRIG SEQ's module page is its grid: four tracks (the OP 1-4 keys pick A-D) of 16
steps on the lower keys. A key turns its step on or off; GLO + a key steps it through on, accent, roll. Each track
has its own length (polymeter), chance and shift; SWING, ROLL (hits in a roll) and FILL (a gate into FILL: every hit
rolls, double time, or extra hits by chance) are on the first page. ACC goes high with every accented hit: patch it
to a DRUM SYNTH's ACCENT.

**FM-2.** A two-operator FM voice in eight knobs on two pages: a modulator into a carrier. TONE: PRESET, RATIO (the modulator's
frequency against the carrier's, 0.25-16), INDEX (how hard it modulates; the INDEX jack adds to it) and FBK (the modulator's
feedback: sine toward saw toward noise). ENVELOPE: AMP (the carrier's envelope, the loudness), MOD (the modulator's envelope, the
brightness), TIME (stretches or shortens both, 0.25-4 times) and TUNE. AMP and MOD pick from twelve shapes rather than setting
stages: ORGAN, PLUCK, PERC, PIANO, BELL, BRASS, BOW, PAD, SWELL (two seconds in), GATE, BLOOM, TICK (a click: gone in a few hundredths of a
second). A bell is two long shapes, a pluck a short MOD under a longer AMP, a pad two slow ones. The view shows the two operators,
lit by their envelopes, and both envelopes drawn at their time. (Patches saved with the old four-operator FM4 load as FM-2 with their
operators 1 and 2 carried over; their variations are not.)

**PARAM MOD.** Any knob of any module, moved by a voltage: four channels, a page each. Patch a source (an LFO, an envelope, a sequencer lane...)
into CV 1-4, turn MODULE to the module you want to move (it steps through the modules there are; 0 is none) and PARAM to its knob (the
name shows: "VCF 1  CUTOFF"). DEPTH is how far 5 V moves it, of the knob's whole range either way (negative turns it over), SMOOTH how
slowly it follows. The knob keeps its own value; the voltage's offset is added just before the module runs and taken off after, so
what you hear is the knob plus the voltage, and what is saved is the knob. The voltage is read 2.7 thousand times a second: oscillators,
filters, effects, envelopes and the other modules that can run in short pieces follow it at that rate (a fast LFO moving a filter or a
pitch is smooth); the rest follow once a block. Not audio-rate FM (the modules' own FM jacks do that). Up to 16 knobs at once; a module with a knob moved this way
runs on the first core. The page draws each channel's source, its target and how far it has moved it. A deleted target lets go; a copy of
a PARAM MOD has no targets; a saved patch keeps them.

**COMPARE.** Two signals in (CV or audio), a gate out. A is compared with B plus THRESH (with B unpatched, with THRESH alone). MODE: A>B, A<B, WINDOW (on while A is within THRESH of B) or CROSS
(a trigger each time A crosses B, either way). HYST stops a noisy signal chattering; PULSE 0 makes the gate follow the comparison, above 0 it is a trigger of that length on each turn-on. Outputs: GATE,
INV and A-B. With CV it makes events from how two modulators relate; with audio it makes pulse waves and hard digital distortion. LOGIC works on gates; COMPARE on levels.

**SHIFT REG.** A CV shift register: each CLOCK pulse takes IN (FEEDBK mixes in the value that fell off the end, so the pattern goes round) and moves every stored value one place along a register
LENGTH (2-16) long. NOW is the newest value, -1, -2, -3 the ones before it, END the oldest, MIX crossfades NOW to END. MUTATE adds a random nudge to what goes in; RESET empties it. Patch a
sequencer's pitch into IN and END into a second oscillator and it is a canon; with FEEDBK up and MUTATE a little, a pattern that slowly changes. TURING makes its own values; this shifts any CV.

**Finding a module in the ADD list.** REC makes the highlighted module a favorite (a star; eight at most, kept with the settings); KNOB 3 steps round the favorites; KNOB 4 steps round the last six you added (marked R); KNOB 2 jumps a category. On the patch screen, GLO + OP1-OP4 (the first four favorites) adds favorite 1-4 where the cursor is, wired like SEL in the list.

**Random and clear.** On the SEQUENCER and MOD SEQ step page, GLO + SEL fills the whole pattern at random (SEQUENCER: notes from a minor pentatonic over two octaves, most steps on; its ratchets, chances and velocities stay; MOD SEQ: every step of every lane) and GLO + REC clears it (SEQUENCER: notes and gates off, step features back to normal; MOD SEQ: every lane to zero). GLO + OCT- undoes either. On the web page the cards have Randomize and Clear buttons.

**MOD SEQ.** Four lanes of 16 steps of voltage, out of LANE A-D: patch them to anything (a filter's FREQ, a delay's TIME,
a sampler's SLICE). Each lane has its own length (LEN A-D) so they drift against each other; RANGE, SLEW, CHANCE and
direction are shared. Old patches that used the SEQUENCER's MOD lanes load with a MOD SEQ added and the cables moved.

**New in v1.0.0-rc120.** (rc63's link error fixed.) DIAGNOSTICS' second page (ALGORITHM): the CPU cost of each module and an experimental start of the second core (SEL). *(rc62:)* SAMPLER: RAM is the INTERNAL bank's first entry only; the waveform follows a recording; a new SAMPLER starts as 1SHOT; the unit's update screen is PATCHWERK's. *(rc61:)* SAMPLER: REC on its page arms, records IN into the RAM (a second at most) and stops; SAVE keeps that sound with the patch (about 22 KB of the bank's flash for each). *(rc60:)* DRUM RACK's first kit is CUSTOM and each pad can take any kit's sound or any bank sound; AUTO COMPACT is a setting; REC unplugs one cable at a time; EDIT opens a module's first page; the web banner is calmer. *(rc59:)* The METRONOME is GLO+PRESET (turn up: on, down: off) on any page but the patch list, so GLO+KNOB 2 and the fine knobs are back; the PIANO ROLL's view is GLO+SEQ (AUTO, FOLD, KEYS) and shows every key from two below its lowest note to two above its highest (it folds when that is more than 17 keys); a new SAMPLER starts with a one-second drum break (from Jungle.wav, 22.05 kHz, 10 KB) in its RAM instead of the synthesized loop; INTERNAL is empty again (the firmware did not fit with JUNGLE built in). **v1.0.0-rc58.** A METRONOME: GLO+KNOB 2 turns it on (up) or off (down), a click on each beat (higher on the bar) while the transport plays, and MET shows in the top bar and flashes on the beat. The PIANO ROLL's view: AUTO folds (rows only for pitches that have a note, so they stay big) when the notes are in more than two octaves; GLO+KNOB 3 picks AUTO, FOLD or all KEYS (two octaves that follow the cursor); a roll that starts playing in the middle of a note now takes that note's pitch (it sat at -5 V, very low) and its playhead no longer runs a sixteenth ahead; the PIANO ROLL is listed under SEQUENCER. SAMPLER and DRUM RACK have a built-in sound again: INTERNAL 1 is JUNGLE, a drum break (22.05 kHz mono, SEA-packed, about 27 KB of the firmware). **v1.0.0-rc57.** The PIANO ROLL shows the octaves its notes are in (from the lowest note's octave up to the highest, at most three; one octave when empty). **v1.0.0-rc56.** The PIANO ROLL shows one octave at a time, KNOB 2 changes a note's pitch (KNOB 4 velocity, GLO+KNOB 4 the loop), its module page draws the loop, there is an example and a factory patch (ROLL CHORDS); the compact patch view comes on only for a big patch. **v1.0.0-rc55.** PIANO ROLL (86 modules): notes on a grid with chords, edited on the SEQ page (keyboard down the side, SELECT in time, ALGORITHM in pitch, SEL a note, REC records from the keys); one PITCH / GATE / VEL cable carries up to four voices. MOD SEQ has TRIG A-D outputs. The patch screen has a compact view for big patches (GLO+SEQ: AUTO, COMPACT, FULL). **v1.0.0-rc54.** The SAMPLER has BANK and SAMPLE knobs: INTERNAL (reserved for sounds built into the firmware: none for now, as the code region is full), A, B, C and D (your sounds, 8 each, sent from the web page; SAMPLE 1-8); the DRUM RACK's pads show them as I, A-D and its KIT list ends with KIT 9-12 (all pads from bank A, B, C or D). The tempo is back on the patch list (GLO+K1 still sets it from the patch screen); a module you add opens on its first page; SELECT on the add list no longer jumps categories when turned fast (KNOB 2 does). The sample bank is 316 KB (the main 208 KB window plus the two extra data windows the FM-1's platform gives an app). The web page has four banks with a Clear bank button each, drops up to 8 files onto a bank, shows every sound's size and what is on the FM-1, and shows the flash used out of the total. MOD SEQ has TRIG A-D outputs (a pulse each time a lane steps). The patch screen goes compact (tags and dots) when a patch does not fit; GLO+SEQ sets AUTO, COMPACT or FULL. **v1.0.0-rc53.** Samples are trimmed and compressed: the web page cuts the silence off both ends of a sound and packs it with SEA (about 3.4 bits a sample, a quarter of the room: roughly 11 s at 44.1 kHz in the FM-1's 208 KB, at the sound's own sample rate), and the emulator plays it exactly as the FM-1 will. A slot's **Trim** and **Compress** boxes turn either off (whole, 16-bit). **v1.0.0-rc51.** CLIP PROTECT: OUTPUT's PROTECT knob (on by default) is a brickwall limiter at -0.5 dBFS with 2 ms lookahead that replaces the SOFT LIMITER setting, and the OUTPUT picture shows the gain it takes off; the tempo is SETTINGS' first row and GLO+K1 on the patch screen (gone from the patch list); SELECT scrolls HELP, SETTINGS and DIAGNOSTICS, the PRESET knob does nothing inside them; TRIG SEQ's strip says OP1-4. **v1.0.0-rc50.** MIXER 8 (78 modules): eight channels with level and pan; a SOFT LIMITER setting (replaced by CLIP PROTECT in rc51); TRIG SEQ shows its accent and roll shortcuts in a second strip row; the settings page loses ALGO group and the boot screen its GLO hint; DX ENSEMBLE loses its reverb (less CPU); the web page loads the firmware beside it even when opened from a folder. **v1.0.0-rc49.** FM-DX: a TX81Z / DX21 four-operator voice on the YM2414's own arithmetic (ymfm), with BANK and PRESET like the CZ (INTERNAL has 32 voices, INIT first; load TX81Z .syx banks on the web page), a picture of the algorithm and each operator's envelope, 42 knobs in 11 pages, and a DX ENSEMBLE factory patch (54). **v1.0.0-rc48.** Fixed the crash on ADD MODULE's visual page for CZ; XFADE and SPACE have pictures (every module has one now), and the web page shows BUTTON and CZ pictures. **v1.0.0-rc47.** Web mirror: whole-frame snapshots (the tearing), pieces wait briefly for the link instead of being dropped, faster catch-up after a drop (needs the new firmware for the last two). **v1.0.0-rc46.** A saved patch carries its CZ tone when the tone is from a .syx bank or has been edited, so it sounds the same after power off even if the bank is gone. **v1.0.0-rc45.** The CZ picture fits its time window to the tone and draws what the voice does (key follow, the line that is heard, ring as a product); a test proves a CZ with edited knobs sounds the same after save and load. **v1.0.0-rc44.** CZ BANK and PRESET bars scale to the banks and tones there are; CZ ENSEMBLE is quieter; the unit marks a mirrored frame whole only when none of its pieces were dropped (tearing on the web page). **v1.0.0-rc43.** The CZ is editable: BANK then PRESET, the other knobs show the preset and moving one changes the sound (PRESET shows a *), the picture shows both lines' DCW and loudness envelopes, one row of four knobs with ALGO for the pages; INTERNAL is the only built-in bank (81 tones, INIT first); ring mod follows MAME; the web mirror paints on a frame-complete marker (less tearing). **v1.0.0-rc42.** The CZ has a BANK knob: INTERNAL (64 tones picked from the CZ-230S presets), USER (16 made here) and four .syx banks of your own; the web page loads several .syx files at once under the sample bank; a new factory patch, CZ ENSEMBLE; the web mirror follows the unit faster. **v1.0.0-rc41.** Fixed the crash when a CZ is on a bank tone (the rack looked up a preset name the TONE knob does not have). **v1.0.0-rc40.** The crash screen shows a sixth line: the last two steps the bank took (a breadcrumb trail, to find the crash when a CZ bank is loaded); the linked page waits a little longer for a whole new screen. **v1.0.0-rc39.** CZ keeps its bank tone in its own RAM and the audio reads nothing from the sample bank while its flash is erased or written (a suspected cause of crashes when a CZ bank is loaded onto the FM-1); the linked page waits longer for a whole new screen before painting it. **v1.0.0-rc38.** ADD MODULE with GLO held shows the new module in the place of the one being replaced (between what was before and after it), also where SEL would insert; the web page keeps its own palette and brightness while linked (the FM-1's screen settings are undone on the mirrored screen) and paints a screen change once its pieces have arrived (no tearing). **v1.0.0-rc37.** ADD MODULE with GLO held shows a replace: the module that goes (dashed, filled), an arrow, the one that takes its place, and the bar reads "Replace A with B"; the web page's meters hold their level and fall slowly instead of flickering. **v1.0.0-rc36.** CZ, checked against MAME's uPD933 chip emulation: noise modulation draws its random pitch every 8 chip samples (5 kHz) as the chip does, not every sample (it was far too hissy); DCW is capped as the pitch climbs, as on the chip. **v1.0.0-rc35.** CZ key follow uses the chip's own amounts per setting 0-9 (DCA and DCW tables from the sysex format); ADD MODULE: while replacing, only the module being replaced is marked. **v1.0.0-rc34.** BUTTON (76 modules): the keys as gates, triggers, toggles, repeats, bursts, taps and a sample-and-hold, with TRIG, HOLD and DBL outputs and a pressable button on the web page; GLO + HOME swaps the keys between notes and buttons; the CZ knobs show their page tabs; ADD MODULE names the module GLO+SEL would replace and fills its box in the strip. **v1.0.0-rc33.** CZ-1 is renamed CZ and shows its knobs two rows at a time; GLO+SEL in ADD MODULE replaces a module and the patch strip outlines the one it replaces; the source is renamed from noodle to patchwerk (pw_ files and prefixes). **v1.0.0-rc32.** PD OSC and FM TX are gone (75 modules; saved patches that name them load without them); CZ keeps all its knobs, paginated, with a picture, and loading a tone puts its wave, DCW, envelopes, pitch drop and vibrato on the knobs (TONE 0 plays them); CZ ring modulation rings as long as line 1 (as Arturia's CZ V does); **v1.0.0-rc31.** Tones with no sustain step release at their END step (a sitar no longer rings for 20 seconds); ADD MODULE always offers Replace and its patch strip is taller. **v1.0.0-rc30.** CZ has a tone of its own built from the knobs (TONE 0, the default) and one bank for Casio tones, apart from the sampler's; a taller patch strip on ADD MODULE; shortcuts for randomize and clear on SEQUENCER and MOD SEQ; more adventurous generated patches. **v1.0.0-rc29.** CZ: plays Casio CZ / CZ01 tones. **v1.0.0-rc28.** The FM TX .syx loader now reads real TX81Z banks correctly (names, waves, fine tuning, fixed-frequency operators). **v1.0.0-rc27.** ADD MODULE drops the plan box: more rows, more room for text and a larger visualization. **v1.0.0-rc26.** Module descriptions are packed in flash (smaller firmware; same text). **v1.0.0-rc25.** ADD MODULE keeps its patch strip on every page and spaces the description more; the screen test is fixed. **v1.0.0-rc24.** FM TX (a TX81Z / DX21 / DX100-style FM voice with a .syx loader on the web page); the quick add list is gone: ADD MODULE has a patch strip with a ghost of the new module and the cables it would make, its description in smaller text, and the visualization moved to page 3; randomize and clear on SEQUENCER and MOD SEQ (GLO + SEL, GLO + REC).

**v1.0.0-rc23.** GENDY (stochastic oscillator, audio or CV); the patch list is now the MENU (HELP, SETTINGS, DIAGNOSTICS rows); ALGORITHM jumps between its sections and between SETTINGS groups; SETTINGS shows every row with a two-line description; DIAGNOSTICS has CPU and battery lines (they are gone from help). rc21: HELP and SETTINGS are rows of the patch list, GLO is only a modifier, a clearer KEYS help, 53 factory patches. rc20: Flash savings (packed examples, UI at -Os), ALGORITHM turns pages, a DIAGNOSTICS page, AUTO-REPATCH, KEY SENSITIVITY. rc19: Settings flash readout no longer uses a literal address. rc18: Device link fix (no 64-bit divide) and singular category names. rc17: The settings page shows what the firmware takes of the FM-1 (flash, static RAM, pool, module memory, CPU) as bars; the screen's drawing bands moved to the pool (23 KB less static RAM); one emulator.html and one cheatsheet.html; build_web.command. **v1.0.0-rc16.** Old formats removed: the seven folded modules (FLANGER, PHASER, CHORUS, REVERSER, SHUFFLER, CLOCK WARP, AUDIO MIXER) are gone for good, and patches saved before patch format 9 no longer load (a patch that names a removed module loads without it); the flash and code they took is freed. (rc15) ROOM MODES (a room's standing waves as a resonator bank, with a picture of its modes ringing).

**v1.0.0-rc14.** SAMPLE RATE setting (44.1 kHz, 22.05 kHz, or AUTO: drops to 22.05 on a CPU overload so the music goes on); ON DELETE setting (a deleted module can close the gap in the chain); the matrix and module page show each cable's DEPTH; computer-keyboard panel mode on the web page (Caps Lock); SONG BEATS and SONG BREAK with more variation; CZ KEYS replaces CZ BELLS; GRANULAR shows its sound behind the grains. (rc13: SONG BEATS; a richer sampler loop (22 hits); CONTROL RATE setting; less CPU for envelopes at rest. (rc12: less CPU again (reverb, meters, VCO, mixer). rc11: less CPU for the same sound (meters, VCO, DRUM SYNTH, CLOCK WARP), a quieter SONG DUB hat and snare, tools/cpu_profile.sh. rc10: SONG DUB without the reverb (it overloaded); all SONG patches together in the list; the CPU message no longer hides the shortcut strip; REC held inside a group deletes the module and stays in the group; BRIGHTNESS goes up as you turn right.) (rc9: a smaller platform canvas, BEAT FIT, no MESSAGES setting, cheatsheet.html.))

**v1.0.0-rc8.** 3.4 KB more pool free (the pool headroom check).

**v1.0.0-rc7.** RAM back under the limit (rc6 was 76 bytes over).

**v1.0.0-rc6.** SAMPLER BEATS (a break in time at any tempo); SONG DUB, a fourth song example built from groups; the emulator starts on the ORIGINAL palette.

**v1.0.0-rc5.** DARKS has three steps; the emulator has a screen brightness slider.

**v1.0.0-rc4.** SETTINGS: DARKS and BRIGHTNESS; the emulator's "Device look" switch.

**v1.0.0-rc3.** Darker, higher-contrast palettes (NIGHT new, ORIGINAL kept; 7 in all); the boot animation goes on without a jump.

**v1.0.0-rc2.** Power-on shows the logo and type together; NIGHT's ground is true black.

**v1.0.0-rc1.** The first release candidate for 1.0 (identity FM-1_606). A SETTINGS page (GLO + HOME, or ALGORITHM on the help page): GLO latch, six palettes, autosave, the default for EDITS, pad sensitivity, how long messages stay, a factory reset and the unit's version, identity and free memory. The logo animates from power-on; ADD MODULE and the quick-add list wrap around; SONG: GLO + REC deletes an automation lane; the matrix shows groups unfolded; keys of a module with an output cable play the whole patch while you audition it; the help lists how to save and recall snapshots.

**v0.999997.** Groups in the matrix (headers, fold); SONG REC no longer fights the knob and shows a blinking dot; the cursor steps by clip; ACID with a snare and lighter on CPU. **New in v0.999996.** GLO LATCH setting (help page, K1); SONG horizontal again, no group lanes, GLO+SEL duplicates a clip; firmware Download link always on the page. **New in v0.999995.** SONG is vertical (time down, SNAP column, automation beside it); GLO+SEL duplicates a clip with a snapshot of its own; no group lanes. **New in v0.999994.** SONG examples built from groups; SONG lanes size to the group count; FM page fits; logo cable. **New in v0.999993.** A Download link for the firmware on the page. **New in v0.999992.** Groups again (G1, G2 on SONG, four group lanes in view), grey-on-dark group boxes, groups in the web patch bay, one row of knobs on module pages. **New in v0.999991.** SONG lanes for groups; two rows of knobs on module pages; grey groups; "Grab cable"; INERTIA MIX; firmware versions kept and listed on the page. **New in v0.99999.** Groups (the old groups) arrive as one box with their jacks, ALGORITHM scrolls all their knobs, a warm ground inside; DRUMS METAL no longer hisses.

**v0.99997.** Groups (GLO + FX suggests chains, GLO + REC ungroups); Groups live in the patch list; SPACE, a small three-knob reverb (73 modules).

**v0.99996.** A better REVERB (MOD, DIFFUSE, PRE-DLY, LOW CUT and a MODE: room, hall, plate, gated, swell); the add list pages on EDIT and replaces on GLO + EDIT; the module page labels its faces.

**v0.99995.** Groups (GLO + FX); the module page has INFO and DEMO faces on EDIT; GLO + SEL picks up a cable, GLO + ENV / LFO copy and paste.

**v0.99994.** Eight new factory patches that show the newer modules (56 in all); a chevron beside the knobs when there are more pages of them; PERFORM's two-row strip and Bar Sync beside its words. **New in v0.99993.** BURST has a PITCH output: STEP semitones on every hit. **New in v0.99992.** BURST (one trigger fires a roll of hits that speeds up or slows down); PERFORM's second page is MUTES (mutes and MORPH on knob 1, SEL turns Bar Sync on and off); the strips name REC as delete on PERFORM and KEYS as snapshots or mutes; snapshots are S1, S2 on the morph bar; PERFORM numbers are one size; add-list wording and where ENV and EDIT land. **New in v0.99991.** The add list's category no longer overlaps the page boxes. **New in v0.9999.** The page line above the knobs counts them 1-4 (or fewer on a short page); a larger module name in the strip; "Module will be added unpatched". **New in v0.9998.** INERTIA (CV with mass: it lags, overshoots and rings; FOLLOW, PUSH and FLICK); the module page's page boxes sit on a line above the knobs; DRIFT moves the pulse width too. **New in v0.9997.** VCO: DRIFT (the pitch only) and ANALOG (all four waves, scope-style; CHAR is gone); air between a module's name and preset on the patch screen; no CONNECTIONS label. **New in v0.9996.** SNAPSHOT (a CV picks the snapshot: noise, an LFO or a MOD SEQ arranges the patch); VCO rebuilt from scope pictures, with its waves redrawn as you turn ANALOG or CHAR; ALGO plate and numbered boxes for every set of pages; GENERATE PATCH; the factory patches packed (flash). **New in v0.9995.** VCO CHAR and a stronger ANALOG; XFADE; the patch screen names a module's preset on its own line; ALGO plates by every page number; long preset names on two lines; the web page loads patchwerk.fwsc from beside it. **New in v0.9994.** Flash use trimmed (module tables); no change in behaviour. **New in v0.9993.** SPECTRAL is gone. MODAL PERC's page shows all its knobs, its partials drawn in the strip above. PD OSC has its own loudness envelope (GATE in, no VCA needed; no ENV output; new LOUD and RELEASE knobs). **New in v0.999.** Two new modules and one rebuilt: PD OSC (Casio CZ phase distortion, 13 presets) and MARKOV (a melody from chances). RESONATOR is now MODAL PERC, with wood, metal sheet, spring, pipe, gong and timpani models and 25 presets. Hold ARP and turn a knob and a MOD SEQ lane records it step by step (GLO + ARP + the knob lets it go). When a screen has more than four shortcuts, the strip has two rows (no more 1/2 and GLO + ALGORITHM). **New in v0.998.** The full module list names the module EDIT replaces. **New in v0.997.** The strip shows 1/2 when it has two sets (GLO+ALGORITHM flips it); smooth HELP scrolling; fifteen redone add-list previews and a MIDI OUT visualization; the web notes section removed. **New in v0.996.** GLO + REC picks up a plugged cable to move it; FX then EDIT replaces a module (both in the strip); SONG starts off and switches on with the first clip (underlined S1 = on); tidier download. **New in v0.995.** Messages in the bottom strip, not an amber bar; LPG BONGOS retuned to the article's recording; a short snare in SONG BERLIN; SONG AMBIENT removed; the header underlines S1 while a SONG plays; the install section folds the developer steps away and a page built after ./build.sh installs that firmware directly. **New in v0.994.** GLO+ shortcuts spelled out in the strip; turn ALGORITHM on the patch screen for more shortcuts; P1/2 beside the cable line, a bigger cable view; KEYS map puts the function first. **New in v0.993.** A KEYS map at the top of HELP; hold GLO to see its shortcuts in the bottom strip; LPG BONGOS rebuilt after Peter Barata's article. **New in v0.992.** Bottom shortcut strip (left-aligned, four hints at most); patch list says User; DRUM SYNTH gets a MODES knob and 10 more presets. **New in v0.991.** AUDITION: on a sound source's module page a key strikes it on its own; patch list wraps, GLO + knob jumps between new / factory / yours, opens on your last patch; one hint style everywhere; battery percent; LOOPER BEATS down to 1/8; WEST COAST tamed; SONG BREAK (was DETROIT), SONG AMBIENT, ACID and BERLIN redone; a shorter DRUM SYNTH clap. **New in v0.98.** SONG DETROIT and SONG AMBIENT redone, drums on SONG BERLIN, DRUM SYNTH gets METAL and BURST and better cymbals, claps and percussion; mixers name their sources; previews move like the real thing. **New in v0.97.** The small add list is QUICK ADD MODULE (FX again: the full list); better visualization previews (SAMPLER, NOISE, DRUM RACK); a small visualization above the knobs of ADDITIVE, MICRO PERC, CLOCK WARP, SHUFFLER, MACRO OSC and the like; 808 drums in SONG ACID; the new SONG DETROIT.

**New in v0.96 (an experiment).** On the patch screen ALGORITHM turns the module's knob pages, FX opens the small add list (SELECT browses it; FX again: the long list). Before, ALGORITHM opened the small list and GLO + ALGORITHM turned the pages. Going back is one line (docs/EXPERIMENTS.md). Also: MIX is knob 4 on FLANGER, PHASER, CHORUS, HARM TREMOLO and TAPE.

**v0.95.** SONG's automation lanes. Below the SNAP lane (snapshot clips, as before) ALGORITHM walks through up to 8 lanes, each one knob moving along the song: a graph with breakpoints. SEL on an empty spot makes a point (or turn KNOB 1: VALUE); KNOB 2 sets the shape of the segment after it (LINEAR, STEP, SMOOTH); KNOB 3 zooms (8, 4, 2 bars); KNOB 4 points the lane at another knob. SEL on a point picks it up (SELECT moves, SEL drops), REC deletes it, GLO + REC deletes the lane. The "+" lane opens the knob list: every knob of every module, SELECT scrolls (fast: module to module), KNOB 2 jumps module, SEL picks. Or record: REC where there is no point arms recording (REC shows in the header); play, turn knobs on PERFORM or any module page, and points are laid on their lanes, thinned. All lanes share 128 points; the song loops at the end of its last clip or point. The four song examples are F46-F49 (SONG BERLIN, SONG AMBIENT, SONG ACID, SONG DETROIT): snapshots that bring sounds in and out, a song that plays them, lanes on the filter.

**v0.94.** Snapshots. A patch keeps 16 (was 8 variations): each its own knobs, sequencer steps and which modules are bypassed (GLO + PLAY), so a snapshot can bring a sound in or take it out (tails and filters may sound strange across a change). Knob edits change the current snapshot only; GLO + SAVE (or K3 on SONG) flips EDITS to ALL: an edit then also reaches every snapshot still holding the old value (the header shows ALL). The first eight lower keys pick snapshots 1-8 (SEL held: 9-16; GLO+key copies, in PERFORM). The SONG page has two lanes now: SNAP (snapshot clips: K2 picks snapshot 1-16 then the saved patches U1-U8, which load when the clip starts, the beat carrying on from the bar; K4 glide morphs between snapshots) and AUTO (SEL makes a ramp for the knob last turned; K2 start, K3 end; the knob glides over the clip). The part lanes are gone: a snapshot's bypassed modules do that. Patch format 9 (older patches load; v8 songs keep their snapshot clips).

**v0.93.** PERFORM has two pages, P1/2 and P2/2, turned with ALGORITHM like any paged screen (MORPH is a row on the second page: KNOB 1).

**v0.92.** SONG (LFO), a grid of bars: clips on lanes, saved with the patch, played by the transport (looping at the last clip). See v0.94 for its current lanes.

**v0.91.** LOOPER (a phrase looper: REC layers over BEATS at the tempo, FBK fades older layers, TONE darkens them, SPEED, REV, CLEAR; the loop is kept at 11 kHz so a few seconds fit); REVERB SHIMMER (the tail fed back shifted up or down; SHIM picks the interval); PERFORM 2 is a page after PERFORM (ALGORITHM scrolls to it), and LFO is kept for the SONG page.

**v0.9.** The version is numbered now (0.9, then 0.91 ... 1.0). The boot mark animates on the device; the FX list's right pane has three pages (what it is, its visualization, a preset; SEL loads it) and SELECT turned fast or KNOB 2 jump category to category; LPG is a vactrol gate (ATTACK, MEMORY, STRIKE, DRIVE, SLOPE, CV AMT, VAC out); a bolt over the battery icon when plugged in; NOISE shows DUST and DTRIG.

**Beta 1.** The first beta. The build's files, the page and the installer say PATCHWERK everywhere (patchwerk.fwsc), the
licence is GPL-3.0, SWIRL is MOD FX.
- *Tempo sync.* A SYNC knob (OFF, 4 BAR ... 1/32, triplets 1/2T-1/16T, dotted 1/2.-1/16.) on LFO (its RATE), DELAY (TIME), HARM
  TREMOLO, MOD FX (RATE), S&H (RATE with no trigger), GRANULAR (DENSITY), FRACTURE (REVERSE's TIME) and GATE DELAY (SYNC A, SYNC B).
  It follows the transport's tempo (or a MIDI clock); OFF leaves the module's own rate.
- *VCO.* DRIFT (0 is steady): the pitch wanders about 20 cents (two slow random walks), the 1 V/oct input tracks a
  little wide and the pulse width breathes. ANALOG (0 is the exact maths) is how the four waves look on a scope: the saw's ramp is bowed and its reset rounded, the
  sine and triangle rounded off at their peaks, the square's top and bottom tilt and its corners round, overshoot and ring (the shape
  is relative to the pitch, so it looks the same on every note). It redraws the four waves on the module page as you turn it. FM MODE: EXP (1 V/oct pitch FM,
  as before), LIN (linear FM: the frequency times 1 + FM x AMT) or TZ (through-zero: that factor may go negative and the phase
  runs backwards, so the pitch holds as the modulator swings).
- *NOISE.* DUST: random clicks of random size and sign, DENSITY a second (0.5-5000), and DTRIG, a gate for each.
- *VCF.* A KEY input (a pitch) with KEYTRK: the cutoff follows it by that part of an octave a volt (1 = tracks the keyboard).
- *DELAY.* SYNC; TAP (a gate: the time between two taps is the delay time); PING (ping-pong: the echoes alternate between the new L
  and R outputs; the longest time is half); WOBBLE and WOB HZ (the time swims, as a tape's does); FREEZE (a knob or a gate into
  FREEZE: the echo is held, nothing new goes in, forever).
- *ENV.* LOOP (cycles as an LFO; with a gate patched it cycles while the gate is high), A, D and R CURVE (above 0 towards a
  straight line, below 0 bowed the other way; 0 is the classic exponential), TRIGGER (SOFT: attack from where it is, HARD: from
  zero, LEGATO: no new attack while it still sounds), a VEL input with VEL AMT (how much a key's velocity scales it), and RANGE
  (FAST x0.1, NORMAL, SLOW x10 on all the times). Its page draws the curves, LOOP and the range.
- *TAPE.* SPEED (varispeed, +-12 semitones, and a SPEED input at 1 V an octave); its page is a deck now: the supply reel empties
  and speeds up, the take-up fills and slows, the tape runs round its guides over the head, a level bar, a counter.
- *SCOPE.* MODE SPECTRUM: a 1024-point FFT of A and B on a log axis, 20 Hz-20 kHz, 60 dB, falling away slowly so peaks read.
- *TURING.* LENGTH to 32, and LOCK (a knob or a gate into LOCK): the loop stops changing.
- *MIX* (dry/wet) on every effect: HARM TREMOLO, RING/SHIFT (on all three outputs) and COMPRESSOR (parallel compression) gain
  it; the others already had one.
- The boot mark's jacks lose their grey rings, and its cables are drawn in from left to right at power-on.
- The web page's on-the-device section keeps every control but folds the long ones (one line, "more") and the performance
  controls (a section of their own, folded).

**New in v28.** Renamed PATCHWERK, with a new boot mark. Four merges, each with every knob and jack of the modules it
replaces (rc16: the old modules and the loading of patches that named them are gone):
MOD FX (TYPE: FLANGER, PHASER or CHORUS), FRACTURE (MODE: GLITCH, REVERSE or SHUFFLE), CLOCK (gains CLOCK WARP's WARP, RAMP and
EOC outputs and its LENGTH, COUNT, WARP, W SWING, DRIFT and GATE knobs) and MIXER (the old STEREO MIXER; centred, L alone is the
old AUDIO MIXER). GRAVITY: the engine reports the ball's real time, each bounce lights its arc and fades over a second, the
ball rests on the floor at the end; its PEGS are now NOTES. QUANTIZER and CHORD show one octave (the notes repeat), ARP
draws its pattern as bars over a keyboard with the held notes and the one sounding lit. HELP shows the battery the way
fm1-base's plat_battery() reads it (ADC channel 3, averaged; four bars at raw 531, 561 and 591 with a little hysteresis;
on USB power: PLUGGED IN, the cable charges it) with the raw count and volts to check against a meter. The patch view
scrolls so the module under the cursor is whole (the last module was cut off at the right edge after a patch loaded).

**New in v27.** LIMITER (CEILING, DRIVE, BRICK / SOFT / FOLD; PUMP ducks it on every beat or on a gate into DUCK and swells
back over RECOVER) and OTT (three-band compression both ways: DEPTH, TIME, SKEW toward the loud or the quiet side, LOW /
MID / HIGH gains). NOISE gains a DIGITAL output (4-bit crackle) and TONE (dark below the middle, thin above it). PERFORM 2
has the same Change on Next Bar row as PERFORM. Visualizations for NOISE, GRAVITY (the bounce), CLOCK, OUTPUT (VU meters),
TAPE, FRACTURE, SEQ SWITCH, QUANTIZER, CHORD, KEYS and MIDI IN; modules with many knobs show all their pages at once.

**New in v25.** CHORD STEPS (a progression: degree, rest or tie, type, inversion), GRAVITY, LISTENER (pitch, loudness or
brightness of audio as a voltage). PERFORM ends in a Change on Next Bar row (ALGORITHM down to it, SEL toggles). In the add list,
hold GLO to peek at the example patch; SEL while GLO is held loads it. SEQUENCER MONITOR (on by default) sounds the notes
as you record them.

**CV RECORDER.** Records a voltage, or its own KNOB, over LENGTH beats and loops it in time. Set MODE to REC and move
the knob (or what is patched into IN): it records one pass and goes back to PLAY. DUB blends a new pass over the old;
a gate into REC records while it is high. SPEED plays it at half or double speed or backwards, SMOOTH rounds it,
LEVEL and OFFSET scale it. Each variation keeps its own recording.

**GRANULAR.** A cloud of short grains from what comes in (the last 0.6 s on the FM-1) or from a bank sound (SOURCE
INTERNAL 1-16, A 1-16, B 1-16). POS how far back (or where in the sound), SIZE, DENSITY (or a grain each TRIG), PITCH (+ V/OCT) and a random
DETUNE, SPREAD scatters them, WIDTH spreads them across the stereo field, FREEZE (or its gate) holds what is there.

**TAPE.** Saturation (DRIVE), WOW and FLUTTER, AGE (the top end dulled), HISS; a gate into STOP brakes the tape to a
halt over STOP's time, the pitch falling with it, and lets go with a quick splice.

**SCOPE.** An oscilloscope you patch: A and B drawn on its module page from a rising edge (ZOOM, V/DIV, TRIG). With
nothing patched it shows the output the patch screen's cursor was last on, so one SCOPE in a patch is a probe you move
with SELECT.

**MODAL PERC (it was RESONATOR; old patches load as it).** Struck things that ring, a bank of tuned resonances with a hit on top.
MODEL: STRING (Karplus-Strong), BELL, BAR, PLATE, DRUM, TUBE, WOOD (a block), SHEET (thin metal), SPRING (a dispersive boing), PIPE
(a hollow tube), GONG and TIMPANI (a drum whose pitch falls after the hit). STRUCT stretches the spacing of the resonances, POS is
where it is struck, STRIKE how hard, BRIGHT how much ring the upper ones keep, EXCITER the hit (NOISE, CLICK or MALLET), BEND the
pitch drop. TRIG strikes it, EXCITE rings it with any sound. PRESET (25): GLOCK, KALIMBA, STEEL DRUM, BOWL, HARP, NYLON, CHIME, TABLA,
GAMELAN, WOOD, WOODBLOCK, CLAVES, TEMPLE BLOCK, TOM, TIMPANI, CONGA, SPRING BOING, THUNDER SHEET, GONG, TUBE BELLS, ORGAN PIPE,
STEEL PLATE, ANVIL, WINE GLASS.

pulse or, with RESO, a resonant sweep, all without a filter. WAVE is the shape of the bend (SAW, SQUARE, PULSE, DBL SINE, SAW PULSE,
RESO SAW, RESO TRI, RESO TRAP). One envelope (ATTACK, DECAY, SUSTAIN, RELEASE, from GATE) shapes both the sound and its loudness, so there is no VCA to patch: ENV is how far
it moves DCW, LOUD the level a held note settles at (0: it dies away like a pluck), LEVEL the output. With no GATE patched it drones. V/OCT and DCW take CV; TRACK makes the bend follow the pitch, TUNE and BEND shift it. PRESET: CZ BASS, SQUARE BASS, BRASS, LEAD,
E.PIANO, ORGAN, VOX, STRINGS, PLUCK, ZAP, KICK, TOM, BELL.

**MARKOV.** A melody from a chain of chances. Each CLOCK step picks the next note of SCALE (over RANGE octaves) by weights: STEP is how
much it likes near notes (low: leaps), HOME the pull to the root, fifth and thirds, REPEAT keeping the same note, REST silences a step.
LEARN: patch a pitch (the keys, a sequencer) into the LEARN jack and it counts which scale degree followed which; TRANSP shifts the
output; LEARN at 100 % follows the tune's habits, at 0 only the knobs. Outputs PITCH, GATE and TRIG; its page draws the
chances of the next note.

**INERTIA.** CV with mass: a weight on a spring (MIX blends the CV as it came with the weight, 100% = the weight alone). MASS, SPRING and FRICTN set how it moves (a light weight on a stiff spring is quick, a
heavy one slow; with little friction it overshoots and rings, with a lot it creeps up); FORCE sizes the kick of an IMPULSE trigger (and
drives PUSH and FLICK). MODE: FOLLOW (the CV is the target it is pulled toward; with nothing patched, the middle), PUSH (the CV is a force,
a spring to the middle holds it) or FLICK (it is kicked by how fast the CV changes and rings on after the CV settles). RESET puts it back
to the middle at rest. The MASS, FRICTN, SPRING and FORCE jacks add CV to their knobs (5 V is the whole way). POS is the position, VEL
and ACC the speed and acceleration in units of the swing, so all three stay within +-10 V whatever the settings. The module page draws how
it answers a step of the CV (a kick, in FLICK) and where it is now. LFO > INERTIA > a filter turns a rigid sweep into a springy one; a
step sequencer through it glides and overshoots between notes; a trigger sequencer on IMPULSE gives hits that ring.

**BURST.** One trigger makes a rapid series of triggers: COUNT of them (1-32), SPEED the rate in the middle of the burst, CURVE bends it
(above 0 the gaps shrink, like a bouncing ball or a roll that tightens; below 0 they grow), JITTER shakes each gap. A new trigger starts it
again. Jacks: TRIG, and COUNT (2 hits a volt) and SPEED (1 V/oct) read when it starts; TRIG out, LEVEL (a CV that falls from 5 V to
nothing over the burst) and PITCH (1 V/oct, moving the PITCH knob's semitones on every hit, held after the last). The module page draws where the hits fall. Drum rolls, flams, glitchy fills.

**SNAPSHOT.** A CV picks the patch's snapshot: 0-5 V (or -5..5 V, RANGE) is spread over SCENES snapshots (2-16). With a clock patched in
it reads the CV only on its ticks. Noise through a clock gives a random snapshot every beat, a slow LFO drifts through them, a MOD SEQ
lane writes an arrangement, a quantized pitch plays them from the keys. HYST keeps a CV sitting on a boundary from flicking between two,
CHANGE waits for the next beat or bar (NOW does not), MORPH glides there. SCENE (a CV) and CHANGE (a trigger) come out. SONG, while it
plays, has the last word. The module's own knobs are in no snapshot. Snapshots you never edited are copies of the patch.

**XFADE.** A blended with B: MIX is the knob, the MIX jack adds CV (0-5 V is the whole way, CV AMT scales or flips it). CURVE: EQ POWER
keeps the loudness steady through the middle, LINEAR is for CV.

**ADDITIVE.** 32 sine partials (after NI's Razor), shaped by a few controls instead of a filter: SHAPE morphs the
spectrum (sine, triangle, saw, square, buzz), BRIGHT a steep spectral low-pass, PEAK and RES a resonant bump you can
sweep like a formant, COMB notches moving through the partials (a phaser made of partials), STRETCH pulls them apart
into bells (or together). A GATE plucks it: BRIGHT opens four octaves and falls back over PLUCK's time. Odd partials
lean left, even right (L and R).

**MACRO OSC.** Eight oscillators in one (after Mutable Instruments' Plaits); HARM, TIMBRE and MORPH mean something in
each, AUX is a variation: VA (two oscillators: detune, saw to square, hard sync; AUX a sub), FOLD (sine to triangle
through a folder), FM (two operators: ratio, index, feedback), FORMANT (two formants' pulses), CHORD (four saws: the
chord, its voicing, saw to square), SWARM (seven detuned saws, drifting), NOISE (resonant filter at the note, low to
band to high, clocked), PARTICLE (random impulses ringing two resonators). The MODEL input picks the model (0.625 V
a model). TRIG patched: its own low pass gate strikes (DECAY, COLOR from VCA only to fully filtered).

**GATE DELAY.** Two gates or triggers, each later by TIME (up to 2 s), their lengths kept; CV A and B add +-1 s at
5 V. B unpatched is a second tap of A. (After o_C Hemisphere's Gate Delay.)

**CLOCK.** The tempo (SOURCE: the transport, or its own TEMPO) out as CLK, DIV (slower), MULT (faster) and RST; and the
same ticks bent in time, out as WARP: over every LENGTH ticks it plays COUNT pulses spread by a curve. WARP 0 is even, up
rushes (each gap shorter), down drags; W SWING delays every other pulse; DRIFT lets each one wander early or late,
slowly. COUNT against LENGTH gives polyrhythms (5 over 7). RAMP is where the cycle is (0-5 V), EOC its start. The WARP and
COUNT inputs move them, taken at each cycle's start.

**MICRO PERC.** The small sounds between the drums: CLICK (a struck resonance), ZAP (a falling sine), BLIP (a short FM
beep), TICK (a flick of filtered noise), METAL (two squares ring-modulated), CRACKLE (dust ringing at the pitch). TONE is
each one's brightness or depth; SCATTER moves every hit's pitch, length and tone a little (how far: out at RND).

**WAVEFOLDER.** SHAPE picks FOLD (a triangle fold), SINE, CHEBY or 259. CHEBY is a Chebyshev waveshaper: FOLD is the gain
into it and ORDER the polynomial (between two whole orders it blends them): order 3 turns a sine into its third harmonic,
order 2 into its octave. 259 is after the Buchla 259 as modelled in Esqueda, Pontynen, Valimaki and Parker, "Virtual Analog
Buchla 259 Wavefolder" (DAFx-2017): five folding stages and the direct path, summed, then the output's low pass (TONE,
1.33 kHz as built). FOLD drives the input; BIAS adds even harmonics. It is not oversampled.

**HARM TREMOLO.** A harmonic tremolo with its own LFO: the sound splits at SPLIT into lows and highs and the two swell in
turn (opposite directions), so a held note swirls rather than pulses. RATE (a CV into RATE bends it, 1 V an octave), DEPTH,
SHAPE (SINE or TRI).

**ENV FOLLOWER.** The loudness of what comes in as a voltage: ENV follows it (ATTACK and RELEASE, GAIN into it), INV is its
opposite (a VCA's CV that ducks a sound under a kick), GATE is high above THRESH.

**PRESET jacks.** FM-2, DRUM SYNTH, MODAL PERC, ADDITIVE, MACRO OSC and DRUM RACK have a PRESET input: 0-5 V steps through
the module's presets (added to the PRESET knob; a DRUM RACK's KITs), and the knobs follow. Patched, it holds the preset
the voltage says; the patch's own knobs stand until the voltage first moves.

**The FX list.** The right pane has three pages (ALGORITHM turns them; the pane shows the ALGO plate and boxes 1-3; KNOB 2 jumps category to category, KNOB 3 round the favorites, KNOB 4 round the recents; the small list browses with KNOB 1 like SELECT): what the module is and its jacks, its visualization (live, from a module of that kind in the patch), and an example patch: a small patch built around it (a few lines on what it does, the modules it uses). SEL on the example patch page (Load this patch) builds it in place of the patch, with no question (GLO + OCT− undoes). SELECT goes module to module; turned fast it jumps category to category, as KNOB 2 does.

**Recording a sequence.** SEQ page: REC cycles STEP REC (each key writes the cursor step and moves on), LIVE REC (with PLAY
running, each key writes its note on the nearest step: a key past halfway through a step goes to the next; it loops and
overdubs, only the steps you hit change) and off. TRIG SEQ: GLO + REC on its module page turns on LIVE REC; the first four
lower keys are tracks A B C D (and again up the keyboard), a hard hit (or GLO held) is an accent. The keys still sound, so a
KEYS module can play the drum while you record. With a clock patched into the sequencer the step is judged against the
transport's sixteenths.

**FRACTURE.** MODE GLITCH: keeps the last slice of the beat and, while GATE is high or by CHANCE on each CLOCK, repeats a SIZE
slice, maybe REVERSEd, sliding by PITCH, fading. MODE REVERSE: plays everything backwards, TIME at a time (a TIME input
moves it; FBK feeds it back). MODE SHUFFLE: at each CLOCK step, by CHANCE (an input adds), plays a slice from up to DEPTH
steps back instead, sometimes REVERSEd, an octave up or down (OCT), or stuttered into four (STUTTER); DRY keeps the dry
sound under it. (Half rate in SHUFFLE: 1.2 s on the FM-1.)

**MOD FX.** TYPE FLANGER (a short delay swept by an LFO and fed back: FBK, DELAY), PHASER (six all-pass stages: FREQ, FBK) or
CHORUS (three slow voices spread left and right: WIDTH). RATE, DEPTH and MIX are shared; a voltage into RATE speeds it.
Both L and R carry the sound; the chorus is the stereo one.

**CZ.** A Casio CZ phase-distortion voice: two lines, each a wave bent by DCW, with the CZ's eight-step pitch, wave and loudness envelopes, vibrato, ring and noise modulation. BANK picks a bank and PRESET a tone in it: INTERNAL (81 tones: INIT, a plain saw to start from; 64 picked from the Casio CZ-230S presets; 16 made for PATCHWERK; assets/cz/INTERNAL.syx) or one of four .syx banks of your own (up to 128 tones each, kept apart from the sampler's B1-B16; a bank shows only the tones it has): on the web page, drop CZ .syx files (CZ-101, CZ-1000, CZ-3000, CZ-5000, CZ-1; several at once, each its own bank) on the CZ banks section under the sample bank, or on a CZ card; a linked FM-1 gets them too (about 9 KB for 64 tones). The other knobs show the preset: LEVEL, TUNE, LINE, MOD (off, ring, noise), DETUNE, OCTAVE, WAVE 1 and WAVE 2 (saw, square, pulse, double sine, saw pulse, resonance 1-3: the two alternate on each cycle), DCW (how far the wave bends) with D.ATK, D.DEC, D.SUS and D.REL, the loudness with A.ATK, A.DEC, A.SUS and A.REL, PIT AMT / PIT TIM (a pitch drop), VIB. Move a knob off what the preset says and that part of the sound is yours (the bank is never changed; edits apply to both lines, and the preset's multi-step envelopes become attack, decay, sustain and release); PRESET shows a * until the knobs are back. FOLLOW (key follow: 0 turns it off), V RATE, BRIGHT (DCW's reach), SPEED, AMP SPD and REL (the envelope rates) scale the preset and sit at 1 by default. ALGO turns the knob page; the picture shows the wave and both lines' DCW and loudness. Inputs V/OCT, GATE, DCW CV. The CZ ENSEMBLE factory patch plays two INTERNAL presets. The engine is a port of the CZ engine in Melodee (Kerem Kilic, GPL-3.0), which builds on MAME's uPD933 model (Devin Acker, BSD-3-Clause), ring modulation included; it has not been checked against a real CZ.

**FM-DX.** A Yamaha TX81Z / DX21 voice: four operators on the arithmetic of the YM2414 (OPZ) chip, ported from ymfm. The eight TX81Z algorithms with feedback on OP 4, the eight OPZ waves (sine, half and rectified sines, and the squashed ones), the chip's envelope (ATTACK, DECAY 1 to SUS LVL, DECAY 2, RELEASE: rates 0-31, the TX81Z's own), the TX81Z's 64 frequency ratios (with the fine and fixed-frequency settings a loaded voice carries), the chip's detune, and its LFO (VIB and TREM, with LFO speed and wave). BANK picks a bank and PRESET a voice in it: INTERNAL (32 voices made for PATCHWERK: pianos, mallets, basses, organs, brass and reeds, pads, leads, plucks, a taiko and a snare; INIT first, a plain sine to build on; assets/dx/INTERNAL.syx, made by tools/dx_voices.py) or one of four .syx banks of your own (up to 128 voices each; on the web page drop TX81Z, DX21, DX27 or DX100 .syx files, 32-voice dumps or single voices, on the FM-DX banks section or an FM-DX card). The other knobs show the voice and moving one changes it (PRESET shows a *; the bank is never changed; a saved patch carries a loaded or edited voice, 145 bytes). Pages: PRESET (BANK, PRESET, LEVEL, TUNE), VOICE (ALGO, FBK, VIB, TREM), then OP 1-4 (RATIO, LEVEL, WAVE, ATTACK) each with an ENV page (DECAY 1, SUS LVL, DECAY 2, RELEASE), and LFO. Inputs V/OCT, GATE, INDEX (CV that opens or closes the modulators) and FM (into OP 4); the picture shows the plate with each operator lit by its envelope, its ratio, wave and level, and its envelope as the chip plays it. Not played: the voice's pitch EG, velocity and EBS sensitivity, the reverb stage; level scaling and LFO depth are close, not exact. The INTERNAL voices were designed from the chip's rates, not checked against a real TX81Z.

**GENDY.** Xenakis' dynamic stochastic synthesis, after SuperCollider's Gendy1. One cycle is a ring of POINTS corners (3-16) joined by straight lines; every cycle each corner's height (A CHAOS) and the time it takes (T CHAOS) take a random step and bounce off fixed walls. At 0 it holds one shape, a lumpy oscillator; turned up it drifts and never repeats, from a living tone to rough noise with a pitch. SMOOTH rounds the corners. DIST is how the random steps are drawn (LINEAR, CAUCHY, LOGIST, ARCSIN, EXPON: from even odds to mostly-tiny with rare leaps). RANGE: AUDIO tracks pitch, LOW sits three octaves down, CV is a slow wandering voltage (about 2 Hz at C4, V/OCT sets the rate, no DC block). Inputs: V/OCT and RESET (a gate puts back the first shape); outputs: OUT and CYCLE (a short trigger each cycle, a chaotic clock in CV range). The picture shows the wave's corners with their recent past behind them. One voice costs less than most oscillators and uses no pool.

**VOCODER.** Twelve bands from 120 Hz to 6 kHz. Each band of CARRIER is turned up and down by how loud the same band of MOD is, so the carrier takes the modulator's shape: a voice, drums, a sample, anything whose spectrum moves (a plain sine on MOD does nothing interesting, and a sine as the carrier has nothing for the bands to shape: use a saw, pulse, noise or a chord). With nothing on MOD each band is driven by its neighbour (odd bands by even ones, after Spectra's spectrum transfer in VCV Rack), so one sound shapes itself. RELEASE is how long a band holds after its modulator falls, SHIFT moves the modulator's bands up or down (a formant shift), FOCUS is how narrow the bands are, BRIGHT tilts the carrier toward the highs (consonants live there), GAIN and MIX (wet against the dry carrier). The page shows the twelve modulator levels. About 60 floats of state; each band is two state-variable filters and an envelope follower.

**ROOM MODES.** The standing waves of a room: up to eight resonances that are not a harmonic series (the longest wall, the other walls, the faces between them), which whatever comes in sets ringing; a click of noise leaves a low chord, music through it takes on a room's hum. ROOT (the longest wall's mode), SHAPE (how far apart the other walls are: near each other the modes crowd and beat), MODES, DECAY, MIX; a V/OCT input moves the room. The high modes die sooner, the modes drift a little and go left and right by turns, all fixed. The picture shows the modes at their own pitches (left channel up, right down), each as tall as it rings now, with ticks where a harmonic series on the same root would sit. It is not a reverb: it rings at pitches. (Cheap when idle: with nothing in and the modes gone it does nothing.)

**SPACE.** A small, cheap stereo reverb: SIZE (room and tail together), TONE, MIX. For the places a full REVERB is too much.

**REVERB.** Three knob pages: SIZE, DECAY, DAMP, MIX; FREEZE, SHIMMER, SHIM, MODE (ROOM, HALL, PLATE, GATED, SWELL: the last two shape the tail with a gate); PRE-DLY, DIFFUSE, MOD (the lines wander a little, so the tail does not ring), LOW CUT.

**REVERB FREEZE.** FREEZE (a knob, or a gate into the FREEZE input) holds the reverb's tail: what comes in is shut out
and the tail rings on (fading over about 40 s) while you play over it.

**MATRIX (LFO).** The patch as a list: every module in signal-flow order with the cables leaving it under it. SELECT
moves. On a module, SEL starts a cable from it in three short picks (its output, the module, the input), REC deletes
it (GLO + OCT- undoes), GLO + PLAY bypasses it; on a cable, SEL sends it somewhere else (the module, then the input),
REC unplugs it, KNOB 1 sets its depth (-100..100 %: attenuated, or inverted below 0; saved with the patch). SEL held
shows that place on the patch screen; EDIT opens the module page.

**Replacing a module.** In the add lists (ALGORITHM's small one, FX's long one), GLO + EDIT puts the chosen module in place of
the one under the cursor (on the patch screen) or MATRIX's, keeping the cables it can: each goes to the jack
of the same name, else the one at the same place.

**MUTES (ENV, page 2).** The patch's sounds, sequencers and effects, eight tiles a page (SELECT turns the page). A lower key mutes or
unmutes one (its module bypassed: a muted sequencer stops its notes, tails ring on); with SEL held it solos it (again: back as it was);
with REC held the change waits for the next bar. KNOB 1 is MORPH: every knob of the patch part way from the snapshot playing (A) to B
(GLO + a lower key picks B; if none is picked yet, B is the next snapshot); choosing a snapshot ends it. SEL tapped turns Bar Sync on and
off: with it on, snapshot changes and mutes wait for the bar line.

**SEQUENCER ties and glides (acid lines).** On the step page KNOB 2 goes x1, x2, x3, x4 (ratchets), then TIE, then GLIDE. A TIE step has no new trigger: the gate of the step before runs on into it, so a note is held longer (with the same pitch) or changes pitch legato. A GLIDE step does the same and the pitch also slides into it (60 ms, or the SLIDE knob if that is longer; SLIDE alone slides every step). The tile says TIE or GLD and its gate bar joins the step before (a glide's pitch bar slopes into it); the step before must be on, and a step that is off or fails its CHANCE plays no tie. Put TIE or GLIDE on the notes of a fast pattern with a short ENV on GATE and a high-resonance filter and it is a TB-303. TRIG does not pulse on a tied step; VEL keeps the step before's value on a TIE. MUTATE rolls and nudges move ties and glides, GLO + REC clears them.

**MUTATE (ENV, page 3).** Shake the patch up, and take it back. Two buttons: FX **nudges** (every knob moves a little off where it is) and REC
**rolls** (new values, part way to a random one). KNOB 1 is AMOUNT, KNOB 2 DENSITY (how many of the knobs are touched). The sequencers' steps
are included: a roll draws new notes (from a pentatonic scale) and flips gates, a nudge moves notes a semitone or three and flips a few gates. The list shows
every module that can be mutated; SELECT moves, SEL locks one (LOCKED: it is left alone), SEL on the top row locks or unlocks all. Levels, tuning,
transpose, banks, presets and tempo are never touched, and resonance and feedback stay below the point of howling. GLO + OCT- undoes a mutation in
one step. Locks are saved with the patch (patch format 13; older patches load with nothing locked).

**MUTATE module.** The same, from the patch: a trigger into NUDGE or ROLL mutates (a clock, a EUCLID, a button, a random gate); AMOUNT takes a CV (so a
lane can make it wilder in the chorus) and sets how far; DENSITY how many knobs. TRIG gives a short pulse each time it fires. It obeys the
page's locks and leaves itself alone. Turn it off by removing it or by locking what must stay.

**WAVESET.** Cuts the sound into wavesets (from one rising zero crossing to the next) and plays them back rearranged, as in the CDP waveset
programs. MODE: REPEAT (each group of GROUP cycles played 1 to 8 times), REVERSE, OMIT (groups dropped), DIVIDE (every Nth cycle upside down: an
octave-down growl with no delay), FREEZE (one group held), SCRAMBLE (now and then an older group), AVERAGE (the mean of the last one to four groups), TELESCOPE (each group squeezed to a half to a quarter: an octave or two up), INTERP (the group before slides into the newest) and FRACTAL (copies of it at double and quadruple speed laid over it). AMOUNT is how many repeats or how often a group is changed (a CV jack too), MIX the dry/wet.
A gate on FREEZE holds the current group in any mode. About 0.15 s of memory and a few ms of delay; best on a single voice, a bass or a
mono lead; noisy or chordal sound is cut at random places, which is also fun.

**Module presets.** FM-2, ADDITIVE, MACRO OSC, MODAL PERC, CZ and FM-DX have a PRESET knob, first on their first page (FM
TUNE is on the last page): turning it sets the module's other knobs to a sound to start from (FM-2:
E.PIANO, BELL, BRASS, BASS, MARIMBA, ORGAN, PAD, PLUCK, GLASS, GROWL, LOG DRUM; ADDITIVE: GLASS PAD, ORGAN, VOX, BELLS,
PLUCK, HOLLOW, BUZZ, SWEEP, GONG, STRINGS; MACRO OSC: SUPERSAW, SYNC LEAD, WOODBLOCK, VOX, CHORD STAB, NOISE SWEEP,
RAIN, FM BELL, WOBBLE, GRIT; MODAL PERC: GLOCK, KALIMBA, STEEL DRUM, BOWL, HARP, NYLON, CHIME, TABLA, GAMELAN, WOOD, WOODBLOCK, CLAVES, TEMPLE BLOCK, TOM, TIMPANI, CONGA, SPRING BOING, THUNDER SHEET, GONG, TUBE BELLS, ORGAN PIPE, STEEL PLATE, ANVIL, WINE GLASS; PD OSC has its own 13).
Only that module changes, and one undo takes it back. A DRUM RACK's KIT works the same way; its drawn kits are BOOM,
PUNCH, LO-FI, PERC, GLITCH, METAL, DEEP and TOY (they were 808 and 909: drawn here, not those machines).

**Velocity and pressure.** The FM-1's keys play with velocity (how hard a key lands), read from the key rows with no
change to the hardware (fm1-base's touch library: the twenty keys F3 to C5 sense it well, the top seven a little), and
F3 to C5 give pressure while held. KEYS has VEL (with VEL CRV: softer or harder) and PRESS (with its PRESS
sensitivity); MIDI aftertouch comes out at PRESS too.

**LFO.** A DEPTH input scales its level (0-5 V), beside RATE (an octave a volt) and RESET.

**PROBABILITY** (was COIN TOSS): each trigger goes on to A, or by CHANCE to B instead.

**Adding a module where it goes.** On the patch screen the cursor says where: on a jack with cables the new module goes
into them (from an output: into every cable it has; an input: into its cable), on an empty output the output feeds the
new module's input, on an empty input the new module's output feeds it, and with a cable in hand the new module takes
its loose end. A signal input (IN, L, R) gets the module inserted; a control input (FREQ, RATE...) gets its output
added. One undo takes the whole thing back. On the web page, drag a module from the library onto a cable (inserted),
onto a jack (wired to it) or into empty space (on its own).

**Samplers.** DRUM RACK: eight pads, each a drawn drum (kick, snare, clap, hats, tom, rim, cowbell) or a bank sound,
tuned, decayed and levelled on its own page; T1-T8, ACC and the keys (C to G) strike them; choke groups. SAMPLER: a sound as
an instrument (SAMPLE RAM: its own memory, a drum loop to begin with or what REC records from IN; BANK INTERNAL, A, B, C or D picks one of 8 sounds).
CLASSIC (follows the keys, loops while held, forward or ping-pong), 1SHOT, SLICE (the region cut into 2-16 slices,
EVEN or at its TRANSIENTS, picked by the note or a CV) or LOOP; start, length, REVERSE, WARP (time-stretch: SPEED
apart from pitch), an ADSR, a filter the envelope opens, velocity, glide. Its page draws the sound with its region,
loop or slices, and where it plays. Load your own sounds on the web page: onto a SAMPLER card or a DRUM RACK pad.

**MIDI.** Notes, bend, mod wheel and sustain pedal play KEYS; a MIDI clock (start, stop) drives the transport;
program change 0-15 picks a snapshot; CC 21-28 move the performance page's knobs (on that page hold SEL and move a
controller's knob for another CC). MIDI IN: four CCs as voltages, the clock and run. MIDI OUT: notes, two CCs and
clock out over USB to other gear.

**New patches** get a two-word name (AMBER DRIFT, NEON TOWER ...).

**Polyphony, variations.** VOICES (patch list, knob 4) runs the keys' voice once per note, up to 4; mixers, effects
and the output hear the voices together. If the FM-1 runs out of time it drops a voice and says so. Each patch keeps
16 snapshots of every setting, sequencer step and module bypass; switch with GLO + the first eight lower keys (SEL held: 9-16).

**Samples.** A SAMPLER plays its own memory (SAMPLE RAM: what its REC gate recorded from IN) or one of 32 sounds (banks A-D) in
the FM-1's flash (BANK A or B), sent from the web page's two sample banks over USB; BANK INTERNAL is for sounds built into the firmware (none for now). A sound goes **trimmed** (the silence cut off
both ends: the tail where the level falls 50 dB under the peak, and a fade of 8 ms) and **compressed** with SEA (Simple Embedded Audio codec by
Dani Biro, MIT; mono, 3-bit residuals, a 4-bit scale factor every 20 frames, a 500-frame chunk that carries the predictor's state, about 3.4 bits
a frame; `core/pw_sea.inc`), at the sound's own sample rate: roughly 17 s at 44.1 kHz in all (316 KB of flash). The SAMPLER, DRUM RACK and GRANULAR decode it as they
play (a few dozen cycles a frame, one small reader per voice: 16 voices read compressed sounds at once; a seventeenth hears silence). A slot's
**Trim** and **Compress** boxes turn either off: the sound is then stored whole and 16-bit (and then, as before, the page sends a long one at 22 or 11 kHz to fit).
The 16 factory sounds (assets/samples, made by tools/import_bank.py) are on the page: "Factory to A" / "Factory to B" copy them into a user bank. tools/gen_internal.mjs can build some into the firmware (core/pw_internal.inc) once there is room: name them in BUILT_IN. Drop up to 16 files on a bank and they fill it from sound 1 in name order.
A SAMPLER's SAMPLE input picks its sound at each note (0.3125 V a sound: 5 V spans the bank, added to the
knob). A DRUM RACK's KIT (its first knob) sets all eight pads at once: 808, 909, LO-FI, PERC (drawn), SAMPLES and
SMPL PERC (the factory bank's drums in the pads' parts), I 1-8, I 9-16, A 1-8, A 9-16, B 1-8, B 9-16; the pads keep their parts (1 kick,
2 snare, 3 clap, 4 closed hat, 5 open hat, 6 tom, 7 rim, 8 bell) so a beat plays on any kit. One undo takes it back. To record on the FM-1: AUDIO IN (the computer's sound: the FM-1 is
a USB sound card) into a SAMPLER's IN, a gate into its REC. The 3.5 mm jack is MIDI only: no audio comes in there.
The computer's sound is heard only where an AUDIO IN module's cables send it.

**Backup.** The web page's Backup section downloads one file (JSON) with the eight user patches, the patch playing, the settings (palette, USER palette, favorites and every row of SETTINGS; "Include the settings", and back with "The settings" when you restore)
and, if you like, the sample bank: from the emulator or from a linked FM-1 (the FM-1 answers bank sub-commands 5,
read a user patch, 6, write one, pulled as a sound is, and 8, read a sound back as it is stored: a compressed one goes into the file as its
SEA stream beside its decoding, and back to an FM-1 as the same bytes). Restore puts any of it back on
either; slots the file does not have are left as they are. A single patch saves and opens as a file of its own too
(drop it on the patch bay).

## Settings

The SETTINGS row of the patch list (PRESETS knob) opens SETTINGS; SELECT picks a row, KNOB 1 changes it, HOME leaves. They are kept in the FM-1's flash.

| Setting | |
| --- | --- |
| GLO LATCH | off: hold GLO while you press the key. On: tap GLO, then the key (GLO shows in the header until the key, or the knobs stop); tap GLO again to cancel. The shortcut hints change to match |
| PALETTE | HARVEST, MONO, RISO (the brand palette, the default), BONE, GRAPHITE, SOFT, CIRCUIT, MULBERRY, NANO, HAZE, MID, BALANCE, and USER once one is sent from the web page (the palette editor's "Send to FM-1 as USER": no build needed; the editor can also give the C row for building a palette in) |
| DARKS | LIGHT, NORMAL (the default), DEEP: how deep the dark greys are (the FM-1's panel lifts dark tones, so deeper looks right on it) |
| BRIGHTNESS | 100, 80, 60 or 40 %: the screen dimmed in software |
| MIDI CLOCK OUT | OFF, or ON: MIDI clock (24 a quarter note), start and stop go out over USB while the transport runs (not while a clock comes in). A MIDI OUT module's own CLOCK switch is then left to this one, so the clock is not sent twice |
| PROGRAM CHANGE | SNAPSHOT (a program change 0-15 picks a snapshot, as before) or PATCH: programs 0-7 load the user patches U1-U8, 8-57 the 50 factory patches (a program past them, or an empty user slot, does nothing) |
| AUTOSAVE | an edited user patch (U1-U8) is stored again six seconds after you stop (never while playing or holding keys: a write stops the sound for a moment) |
| EDITS | the default for knob edits: this snapshot only, or all snapshots that share the value (GLO + SAVE flips it in a session) |
| KEY SENSITIVITY | The KEYS module's VEL output only: OFF (always the same), LOW (it takes a hard hit), NORMAL, HIGH (a light touch is already loud) |
| DIAGNOSTICS | Opens a full-screen page: flash, static RAM, pool, module memory and CPU against what the FM-1 has, the version, the counts. HOME leaves |
| CONTROL RATE | FULL, or QUARTER: slow LFOs (no rate cable, under 60 Hz) are worked out every fourth sample and held: a little less CPU in patches with LFOs |
| AUTO-REPATCH | OFF (cables go with the module), or ON: what fed the module's first signal input goes on to what its first used output fed (osc > reverb > output: delete the reverb, the oscillator feeds the output). One undo (GLO + OCT-) brings it all back |
| SAMPLE RATE | 44.1 kHz; 22.05 kHz (the engine renders half as many frames: about half the CPU, duller highs, a little more aliasing on bright saws and noise; the output is upsampled); AUTO: 44.1, and 22.05 until the next patch when the CPU overloads |
| FACTORY RESET | SEL twice: settings back to the defaults, or settings and the eight user patches. The sample bank is kept |
| (info) | the version, the identity (FM-1_606010000), free pool RAM, patch slots in use, free sample flash |

## Compatibility with other firmwares

PATCHWERK is a firmware: installing it replaces the one on the FM-1, and the installer page puts the stock one back. It uses the data area (0x97000 to 0xDFFFF) for its own things only: eight user patches (`NDL1`), the sample bank (16 sounds, `NBK1`) and the settings. It does not read another firmware's projects, patterns or samples, and another firmware will not read PATCHWERK's; what a firmware keeps in that area can be overwritten when the other one stores something, so keep your own files off the FM-1 first (the web page backs up and restores patches and sounds). It leaves the panel calibration (shared by all firmwares) alone. MIDI: it plays from USB and TRS MIDI like any instrument.

## Layout

```
app/            the firmware (fm1-base's APP_DIR): app.c binds the core to fm1-base's plat.h; fm1.toml
app/core/       the synth, platform-free: engine, 85 modules, presets, the screen UI, the live link

A new module needs: its enum entry and table row (pw_engine.h, pw_modules.c), a place in the add list (add_pref in pw_ui.c), a help line (pw_help.inc), an example patch (pw_examples.inc), a visualization that shows what it is doing (vis_kind in pw_vis.inc: a waveform, a grid, a picture of its state), tests in tests/host_test.c, and the docs/REFERENCE.md and cheatsheet entries. tools/check_text.sh checks the text fits the add list in seconds; tools/release.sh NNN does a whole delivery.
                (pw_link.c), the flash sample bank (pw_bank.c), the fonts (pw_fonts.h: tools/gen_fonts.py)
base/           fm1-base (git submodule), with felucca-bsp
web/            the browser emulator and its page
tests/          host tests, simulator scenarios, the link between two engines, browser tests (one with a
                simulated FM-1 behind a mock Web MIDI: tests/link_page_test.py)
tools/          gen_fonts.py (the screen fonts), ram_estimate.sh (RAM and pool without JieLi's compiler)
```
