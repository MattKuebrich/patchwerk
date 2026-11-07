# PATCHWERK v1.0.0 for The Firmware Hub

Identity: FM-1_606 (this release: FM-1_606010000). Licence: GPL-3.0-only.
Repo: https://github.com/MattKuebrich/patchwerk. Playable: https://mattkuebrich.github.io/patchwerk/ (the emulator; it also installs the firmware).

## Description (2-3 sentences)

PATCHWERK turns the FM-1 into a patchable modular synthesizer: over 80 modules (oscillators, a Casio CZ and a TX81Z-style FM voice, six filter circuits, drums, a sampler, sequencers, effects, envelopes and LFOs) joined by virtual cables on the FM-1's own screen and knobs. Each patch keeps 16 snapshots, a performance page and a song mode that plays snapshots and automates knobs; MIDI plays it over USB and TRS. A browser page runs the same code, links to a real FM-1, loads sounds into its sample bank and installs the firmware.

## Steps still yours

1. Install v1.0.0 on a real FM-1 and play it (the hub asks for a real-device test). Check the free RAM line in SETTINGS and that a patch saves and loads.
2. Publish it: `publish.command` (see RELEASING.md) builds `patchwerk.fwsc`, pushes the repo, turns on the page and makes the v1.0.0 release with `patchwerk.fwsc` attached.
3. Email zven@thefirmwarehub.com with the repo and release links, the description above and the screenshots in `submission/screenshots/` (240x240).

Identity rules checked: not 9NN, not 015, not 47 (FRASIL); FM-1_606 is unused in the hub's table at the time of writing.
