#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# The biggest symbols in the firmware's ELF (no toolchain needed: it reads the symbol table itself).
#   python3 tools/symsize.py [app/build/patchwerk.elf] [how many, default 40]
# Prints size, kind (code / data), name; then the totals by section.
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].isdigit() else "app/build/patchwerk.elf"
top = int(sys.argv[-1]) if sys.argv[-1].isdigit() else 40
d = open(path, "rb").read()
if d[:4] != b"\x7fELF":
    raise SystemExit(path + " is not an ELF file")
c64 = d[4] == 2
e = "<" if d[5] == 1 else ">"
if c64:
    shoff, = struct.unpack_from(e + "Q", d, 0x28); shentsize, shnum, shstr = struct.unpack_from(e + "HHH", d, 0x3A)
else:
    shoff, = struct.unpack_from(e + "I", d, 0x20); shentsize, shnum, shstr = struct.unpack_from(e + "HHH", d, 0x2E)
secs = []
for i in range(shnum):
    o = shoff + i * shentsize
    if c64:
        name, typ, flags, addr, off, size, link, info, al, es = struct.unpack_from(e + "IIQQQQIIQQ", d, o)
    else:
        name, typ, flags, addr, off, size, link, info, al, es = struct.unpack_from(e + "IIIIIIIIII", d, o)
    secs.append(dict(name=name, typ=typ, flags=flags, addr=addr, off=off, size=size, link=link, es=es))
def cstr(tab, n):
    s = secs[tab]["off"] + n
    return d[s:d.index(b"\0", s)].decode("latin1")
for s in secs:
    s["nm"] = cstr(shstr, s["name"])
syms = []
for s in secs:
    if s["typ"] != 2:   # SHT_SYMTAB
        continue
    n = s["size"] // (24 if c64 else 16)
    for i in range(n):
        o = s["off"] + i * (24 if c64 else 16)
        if c64:
            nm, info, other, shndx, val, sz = struct.unpack_from(e + "IBBHQQ", d, o)
        else:
            nm, val, sz, info, other, shndx = struct.unpack_from(e + "IIIBBH", d, o)
        if sz and 0 < shndx < len(secs) and (info & 15) in (1, 2):
            sec = secs[shndx]["nm"]
            code = (secs[shndx]["flags"] & 4) != 0
            syms.append((sz, "code" if code else "data", sec, cstr(s["link"], nm)))
syms.sort(reverse=True)
print("%8s  %-5s %-14s %s" % ("bytes", "kind", "section", "name"))
for sz, k, sec, nm in syms[:top]:
    print("%8d  %-5s %-14s %s" % (sz, k, sec, nm))
print()
tot = {}
for sz, k, sec, nm in syms:
    tot[sec] = tot.get(sec, 0) + sz
for sec, sz in sorted(tot.items(), key=lambda x: -x[1])[:8]:
    print("%8d  total in %s" % (sz, sec))
