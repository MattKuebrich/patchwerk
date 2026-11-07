# SPDX-License-Identifier: GPL-3.0-only
# base/tools/fm1pkg_make.py boot_check_errors(): the build's mirror of the firmware's start-up check, on a synthetic app area.
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "base", "tools"))
import fm1pkg_make as f

def region(app):
    app = app + b"\xFF" * (f.APP_SLOT - len(app))
    r = bytearray(b"\xFF" * (0x120 + f.APP_SLOT))
    r[0x120:] = app
    r[0x20:0x40] = f.entry(f.crc16(app), 0x120, f.APP_SLOT, 0x82, 0xFF, 0, "app.bin")
    return r

bad = 0
def check(what, ok):
    global bad
    if not ok:
        print("FAIL", what); bad += 1

r = region(bytes(range(256)) * 400)
check("whole image judged whole", f.boot_check_errors(r) == [])
r2 = bytearray(r); r2[0x120 + 5000] ^= 1
check("a changed byte is caught", len(f.boot_check_errors(r2)) == 1)
r3 = bytearray(r); r3[0x20 + 3] ^= 1
check("a damaged entry is caught", len(f.boot_check_errors(r3)) == 1)
r4 = bytearray(r); r4[0x20 + 16] = ord("x")
check("a wrong name is caught", len(f.boot_check_errors(r4)) == 1)
r5 = bytearray(r); r5[0x20:0x40] = f.entry(f.crc16(bytes(r[0x120:])) ^ 0x5A5A, 0x120, f.APP_SLOT, 0x82, 0xFF, 0, "app.bin")
check("the --badcrc-test header fails on the CRC16 alone", f.boot_check_errors(r5) == ["boot check: the app.bin CRC16 in the header does not match the image"])
print("OTA HEADER FAILED" if bad else "OTA HEADER PASSED")
sys.exit(1 if bad else 0)
