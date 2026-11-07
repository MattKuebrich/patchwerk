# Experiments (things tried that may go back)

## v0.96: ALGORITHM turns pages on the patch screen
- New: on the patch screen ALGORITHM turns the cursor module's knob pages (as on the module page); FX opens the small "add a
  module here" list (SELECT browses, SEL wires it in, ENV adds it unconnected, EDIT replaces; FX again: the long list); in the
  patch list ALGORITHM does nothing.
- Old: ALGORITHM opened the small list and browsed it (SELECT closed it); GLO + ALGORITHM turned the pages; FX opened the long list.
- To go back: in app/core/pw_ui.c set `PW_PATCH_ALGO_ADDS` to 1 (the `#define` right under the encoder enum, or build with
  -DPW_PATCH_ALGO_ADDS=1), then revert the help rows ("ALGO", "FX", "KNOB 1-4") and the scenarios tests/scenarios/patch_from_panel.txt
  and controls.txt (they tap FX twice for the long list now).
