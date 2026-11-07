# Building and installing PATCHWERK (macOS)

PATCHWERK is an app on [fm1-base](https://github.com/charlesvestal/fm1-base) (`base/`, with
[felucca-bsp](https://github.com/hugelton/felucca-bsp) inside it). `./build.sh` runs fm1-base's build on
`app/`, whose settings are in `app/fm1.toml`. No Windows anywhere: JieLi's compiler for the FM-1 is Linux
x86-64 only, and on a Mac the build runs it in a Docker container by itself.

## 1. Set up once

1. **Docker Desktop**: install it from docker.com and start it (on Apple silicon it runs the Linux tools
   through Rosetta; Docker offers to turn that on).
2. **Python 3 with Pillow and the MIDI modules** (the fonts are drawn at build time; the installer talks MIDI),
   and **FriBiDi**, which Pillow's text layout loads at run time:

   ```sh
   brew install fribidi
   pip3 install --upgrade Pillow mido python-rtmidi
   ```

   A Python from pyenv must have been built with OpenSSL (errors about `blake2b` mean it wasn't; harmless
   here, but `brew install openssl` and reinstalling that Python version clears them).

3. **JieLi's toolchain** (clang 4.0.1 for the chip), to `~/.jieli/toolchain`:

   ```sh
   sh base/tools/get_toolchain.sh
   ```

4. **Three files of JieLi's AC79 SDK** (Apache-2.0; the package builder uses them, nothing of it is linked):

   ```sh
   git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
       https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
   ```

Keep this folder somewhere Docker can share, e.g. under your home folder (`/Users/...`).

## 2. Build

```sh
./build.sh                 # app/build/patchwerk.fwsc
```

The build prints the image, RAM and pool sizes and refuses anything that would break an FM-1: too much RAM,
a `double` in the code, a hardware register touched outside the BSP, a call into the chip ROM.
(`tools/ram_estimate.sh` estimates the RAM and pool with stock clang, for a check before a Docker build:
about 88 KB of 96 KB RAM and 13 KB of pool to spare with this version.)

## 3. Install

Back up your FM-1 projects and samples first. Connect the FM-1 by USB (a data cable, not a charging one), then:

```sh
python3 base/bsp/tools/fm1_install.py app/build/patchwerk.fwsc
```

It asks before it writes and restarts the FM-1 into PATCHWERK. `--info` says what is on the connected FM-1.

**From Chrome instead of a terminal:** make a release build and fm1-base's installer page around it, then open
it from `localhost` (Web MIDI needs a secure page):

```sh
./build.sh --release 0.1
python3 base/tools/make_site.py app app/build/patchwerk-0.1.fwsc 0.1 /tmp/patchwerk-site
cd /tmp/patchwerk-site && python3 -m http.server 8000      # open http://localhost:8000/install/
```

## 4. If it goes wrong

- **It crashes at start because of a saved patch:** hold **HOME** while switching on. PATCHWERK starts with a
  factory patch instead of user patch 1 (patches survive reinstalling the firmware).
- **It crashes at start twice:** the FM-1 starts in fm1-base's SAFE MODE (no sound, the updater works): install
  a fixed build, or the stock firmware, from there.
- **The web page:** `./build.sh` (or `python3 web/build_page.py`) writes `patchwerk-vX.Y.Z.fwsc` (its version in the name) beside the page (web/build/ and the project folder) and the page fetches it, checking its SHA-256. Put the page and `patchwerk.fwsc` in the same folder when you host them; opened straight from disk, the page asks you to choose the file.
- **Back to M-VAVE's firmware:** the installer page's last section writes it back.
- **It won't start at all:** felucca-bsp's rescue puts the stock firmware back through the chip's own update mode:

  ```sh
  bash -c "$(curl -fsSL https://raw.githubusercontent.com/hugelton/felucca-bsp/main/tools/fm1_rescue.sh)"
  ```

Send the build's output or the crash screen's numbers (the PC) when you ask for help: `app/build/` has the
disassembly to look them up.

## USB audio on a Mac

PATCHWERK presents the FM-1 as a USB sound card both ways (the computer's sound into AUDIO IN; PATCHWERK's output
recordable on the computer). fm1-base's plain USB layout left the audio part without a driver on macOS 14, so
PATCHWERK builds felucca-bsp with `FELUCCA_USB_MAC_LAYOUT` (app.h; `patches/felucca-bsp-usb-mac-layout.patch`,
applied by `./build.sh`): audio and MIDI as two USB functions, the shape class-compliant audio + MIDI devices
use. It should then show as **PATCHWERK FM-1** in Audio MIDI Setup and System Settings → Sound. The computer's
sound is not mixed into the FM-1's output by itself (`APP_UAC_GAIN`, `patches/fm1-base-uac-gain.patch`): an AUDIO
IN module brings it into the patch.

## Linking the web page to the FM-1

Open the emulator page (`index.html`, or the published one at https://mattkuebrich.github.io/patchwerk/) in desktop Chrome or
Edge, connect the FM-1 running PATCHWERK by USB, and press **Link FM-1** (allow MIDI with SysEx when Chrome asks).
The page then shows the FM-1's screen (fm1-base's mirror), presses its buttons and keys, and keeps its patch bay
and the FM-1's patch the same both ways (`app/core/pw_link.c`). The sample bank's **Send** writes a sound into
the FM-1's flash. Inside the Claude artifact viewer Web MIDI with SysEx is not allowed: save the page first.

## Without an FM-1

```sh
./build.sh --host                                                   # fm1-base's simulator (cc only)
app/build/host/patchwerk_host tests/scenarios/basic.txt app/build/out  # screenshots, WAVs, checks
sh tests/run_tests.sh                                               # everything below
```

The simulator runs this exact app against a simulated FM-1, driven by a script (commands at the top of
`base/host/plat_host.c`). The browser emulator (`web/`) runs the same core with its own patch bay.

## Publishing

RELEASING.md: `publish.command` builds the firmware and the page, pushes to GitHub, turns on Pages and makes the release.

## Mac: double-click

`build_web.command` (in the patchwerk folder) does the Zig build of the browser page for you: it installs ziglang and Pillow with pip if they are missing, checks for `wasm2js` (`brew install binaryen`), runs `web/build_zig.sh`, copies the page to `emulator.html` and opens it. The first time macOS may refuse it: right-click, Open (or `chmod +x "build_web.command"` once if the unzip lost the executable bit).

## Factory patches

The factory patches are kept packed in `app/core/pw_factory_data.inc` (generated; the firmware just unpacks and loads them).
Their source is `tools/factory_src.inc`, ordinary code that builds each patch through the engine. After changing or adding one
there, run `tools/gen_factory.sh` (it needs only a C compiler and Python 3; after editing `app/core/pw_examples.inc`, also `python3 tools/gen_examples.py`, which packs it into `pw_examples_z.inc`) and build as usual; `tests/run_tests.sh` fails if the
data file is out of date. The same goes whenever a module or parameter changes in a way that moves a saved patch's bytes.
