#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A sample bank as core/pw_bank.c keeps it in the flash: tests/out/bank_dir.bin (the directory sector, at
0xA7000) and tests/out/bank_data.bin (the sounds, from 0xA8000). Two sounds: B1 a 440 Hz sine (0.5 s, 44.1 kHz),
B3 a noise burst (0.25 s, 22.05 kHz)."""
import math, random, struct, zlib
from pathlib import Path
out = Path(__file__).parent / "out"
out.mkdir(exist_ok=True)
DATA = 0xA8000
sine = struct.pack("<%dh" % 22050, *[int(20000 * math.sin(2 * math.pi * 440 * i / 44100)) for i in range(22050)])
random.seed(1)
noise = struct.pack("<%dh" % 5512, *[int(random.uniform(-1, 1) * 15000 * (1 - i / 5512)) for i in range(5512)])
data = bytearray(sine)
data += b"\xff" * ((-len(data)) % 4096)
off2 = DATA + len(data)
data += noise
entries = bytearray(b"\xff" * (16 * 32))
def entry(slot, name, off, frames, rate, crc, codec=None):
    e = name.encode().ljust(12, b"\0") + struct.pack("<IIIII", off, frames, rate, crc, 0xFFFFFFFF if codec is None else 0xFFFFFF00 | codec)   # (the byte after the CRC: 1 = SEA)
    entries[slot * 32:slot * 32 + 32] = e
for s in range(16):
    entries[s * 32:s * 32 + 32] = b"\0" * 12 + struct.pack("<IIIII", 0, 0, 0, 0, 0xFFFFFFFF)
entry(0, "SINE440", DATA, 22050, 44100, zlib.crc32(sine))
entry(2, "NOISE", off2, 5512, 22050, zlib.crc32(noise))
d = b"NBK1" + struct.pack("<II", 16, zlib.crc32(bytes(entries))) + b"\xff" * 4 + bytes(entries)
(out / "bank_dir.bin").write_bytes(d)
(out / "bank_data.bin").write_bytes(bytes(data))
print("bank:", len(d), "+", len(data), "bytes")

# a second bank with one compressed sound (SEA, the stream of tests/sea_fixture.inc: 3020 frames at 44.1 kHz) in B1
import re
inc = (Path(__file__).parent / "sea_fixture.inc").read_text()
stream = bytes(int(x) for x in re.search(r"sea_fix_stream\[\d+\] = \{(.*?)\};", inc, re.S).group(1).replace("\n", " ").split(",") if x.strip())
for s in range(16):
    entries[s * 32:s * 32 + 32] = b"\0" * 12 + struct.pack("<IIIII", 0, 0, 0, 0, 0xFFFFFFFF)
entry(0, "SEA HIT", DATA, 3020, 44100, zlib.crc32(stream), codec=1)
d = b"NBK1" + struct.pack("<II", 16, zlib.crc32(bytes(entries))) + b"\xff" * 4 + bytes(entries)
(out / "bank_sea_dir.bin").write_bytes(d)
(out / "bank_sea_data.bin").write_bytes(stream)
print("sea bank:", len(d), "+", len(stream), "bytes")
