#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Writes assets/dx/INTERNAL.syx: FM-DX's built-in bank, 32 voices made for PATCHWERK in the TX81Z's own bulk dump (VMEM) format.
   python3 tools/make_dx_bank.py [voices.py]   (the voices are in tools/dx_voices.py)"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dx_voices
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
C = [1.0, math.sqrt(2), math.pi / 2, math.sqrt(3)]
ITEMS = sorted(((m if m else 0.5) * C[d], m, d) for m in range(16) for d in range(4))
def fidx(ratio):                      # the F (0-63) whose multiple is nearest `ratio`
    return min(range(64), key=lambda i: abs(ITEMS[i][0] - ratio))
def pack(v):
    b = bytearray(128)
    order = [4, 2, 3, 1]              # the stored order of the operators: OP4, OP2, OP3, OP1
    for nth, n in enumerate(order):
        o = v["ops"][n - 1]
        q = nth * 10
        f = o["F"] if o.get("fix") else fidx(o["F"])
        b[q:q + 10] = bytes([o["ar"], o["d1r"], o["d2r"], o["rr"], o["d1l"], o["ls"], (o["ame"] << 6) | (o["ebs"] << 3) | o["kvs"], o["out"], f, (o["rs"] << 3) | o["det"]])
        x = 73 + nth * 2
        b[x] = (o["egs"] << 4) | (o["fix"] << 3) | o["fixrg"]
        b[x + 1] = (o["osw"] << 4) | o["fine"]
    l = v["lfo"]
    b[40] = (l["sync"] << 6) | (v["fb"] << 3) | (v["alg"] - 1)
    b[41], b[42], b[43], b[44] = l["speed"], l["delay"], l["pmd"], l["amd"]
    b[45] = (l["pms"] << 4) | (l["ams"] << 2) | l["wave"]
    b[46] = v.get("trps", 24); b[47] = 2; b[48] = 0x01
    b[57:67] = v["name"].ljust(10)[:10].encode("ascii")
    b[67:73] = bytes([99, 99, 99, 50, 50, 50])
    return bytes(b)
voices = [pack(v) for v in dx_voices.VOICES]
assert len(voices) == 32, len(voices)
data = b"".join(voices)
msg = bytes([0xF0, 0x43, 0x00, 0x04, 0x20, 0x00]) + data + bytes([(-sum(data)) & 0x7F, 0xF7])
path = os.path.join(root, "assets", "dx", "INTERNAL.syx")
if len(sys.argv) > 1 and sys.argv[1] == "--check":   # (tests/run_tests.sh: the .syx is what dx_voices.py gives)
    if not os.path.exists(path) or open(path, "rb").read() != msg:
        print("assets/dx/INTERNAL.syx is out of date: run tools/make_dx_bank.py"); sys.exit(1)
else:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(msg)
    print("assets/dx/INTERNAL.syx:", len(voices), "voices")
