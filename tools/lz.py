#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""LZ4-style block compressor for the factory patches (the firmware's pw_unlz is its decoder).
Format: a token (high nibble: literal count, low nibble: match length - 4; 15 = more bytes follow, each added until one is
under 255), the literals, then (unless the block ends there) a 2-byte little-endian offset and the match's extra length bytes.
The last sequence has literals only."""
def compress(data, dic=b""):
    buf = dic + data
    n0 = len(dic)
    out = bytearray()
    i = n0
    lit = n0
    def emit(lit_start, lit_end, mlen, off):
        ll = lit_end - lit_start
        tok = (min(ll, 15) << 4) | (min(mlen - 4, 15) if mlen else 0)
        out.append(tok)
        if ll >= 15:
            r = ll - 15
            while r >= 255: out.append(255); r -= 255
            out.append(r)
        out.extend(buf[lit_start:lit_end])
        if mlen:
            out.append(off & 255); out.append(off >> 8)
            if mlen - 4 >= 15:
                r = mlen - 4 - 15
                while r >= 255: out.append(255); r -= 255
                out.append(r)
    n = len(buf)
    heads = {}
    for k in range(0, n0 - 3):
        heads.setdefault(bytes(buf[k:k + 4]), []).append(k)
    while i < n:
        best = 0; boff = 0
        if i + 4 <= n:
            for j in reversed(heads.get(bytes(buf[i:i + 4]), [])):
                l = 4
                while i + l < n and buf[j + l] == buf[i + l]: l += 1
                if l > best: best = l; boff = i - j
                if best > 270: break
        if best >= 4:
            emit(lit, i, best, boff)
            for k in range(i, min(i + best, n - 3)): heads.setdefault(bytes(buf[k:k + 4]), []).append(k)
            i += best; lit = i
        else:
            if i + 4 <= n: heads.setdefault(bytes(buf[i:i + 4]), []).append(i)
            i += 1
    emit(lit, n, 0, 0)
    return bytes(out)
def decompress(comp, dic=b""):
    out = bytearray(dic); i = 0
    while i < len(comp):
        t = comp[i]; i += 1
        ll = t >> 4
        if ll == 15:
            while True:
                b = comp[i]; i += 1; ll += b
                if b != 255: break
        out.extend(comp[i:i + ll]); i += ll
        if i >= len(comp): break
        off = comp[i] | comp[i + 1] << 8; i += 2
        ml = (t & 15) + 4
        if (t & 15) == 15:
            while True:
                b = comp[i]; i += 1; ml += b
                if b != 255: break
        for _ in range(ml): out.append(out[-off])
    return bytes(out[len(dic):])
if __name__ == "__main__":
    import glob, sys
    d = [open(f, 'rb').read() for f in sorted(glob.glob(sys.argv[1] + '/*.bin'))]
    tot = 0
    for x in d:
        c = compress(x); assert decompress(c) == x; tot += len(c)
    print("plain", tot)
    dic = d[0]
    tot = 0
    for x in d[1:]:
        c = compress(x, dic); assert decompress(c, dic) == x; tot += len(c)
    print("dict(first)", tot + len(compress(d[0])))
