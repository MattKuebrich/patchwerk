"""The preset sample banks from assets/samples/<bank>/*.wav: A 808, B 909, C FM drums, D various sounds; eight sounds each, the drums in the
order of the DRUM RACK's pads (1 kick, 2 snare, 3 clap, 4 closed hat, 5 open hat, 6 tom, 7 cowbell, 8 crash). Each sound is mixed to
mono, its silent tail cut (and a long one shortened, with a fade), resampled to a lower rate so all 32 stay small, and normalised to
the sampler's full scale (26000). (The 808 has no crash and the 909 no cowbell: their rimshot stands in. The two drum breaks in D, AMEN and SAFARI, are not trimmed.)

    python3 tools/import_bank.py      -> web/factory_bank.json (the page builds it in) and tests/out/bank/*.wav
"""
import base64, json, pathlib, wave
import numpy as np
from scipy.signal import resample_poly

# per bank, slot order: (file, name on the FM-1 (12 letters), rate Hz, longest seconds; None: not trimmed at all, only resampled)
BANKS = {
    '808': [
        ('808_bd.wav', '808 KICK', 11025, 0.9), ('808_snare.wav', '808 SNARE', 22050, 0.2), ('808_clap.wav', '808 CLAP', 22050, 0.25),
        ('808_hihatclosed.wav', '808 CHAT', 22050, 0.15), ('808_hihatopen.wav', '808 OHAT', 16000, 0.45), ('808_tom_l.wav', '808 TOM', 11025, 0.45),
        ('808_cowbell.wav', '808 BELL', 16000, 0.4), ('808_rimshot.wav', '808 RIM', 22050, 0.1)],
    '909': [
        ('909_kick.wav', '909 KICK', 11025, 0.6), ('909_snare.wav', '909 SNARE', 22050, 0.2), ('909_clap.wav', '909 CLAP', 22050, 0.28),
        ('909_hihatclosed.wav', '909 CHAT', 22050, 0.15), ('909_hihatopen.wav', '909 OHAT', 16000, 0.4), ('909_tom-l.wav', '909 TOM', 11025, 0.6),
        ('909_rimshot.wav', '909 RIM', 22050, 0.07), ('909_crash.wav', '909 CRASH', 16000, 0.7)],
    'fm': [
        ('kd_grav.wav', 'FM KICK', 16000, 0.6), ('sd_tan.wav', 'FM SNARE', 22050, 0.21), ('bin.wav', 'FM CLAP', 22050, 0.3),
        ('kin4.wav', 'FM CHAT', 22050, 0.14), ('pishh.wav', 'FM OHAT', 22050, 0.27), ('paon_s.wav', 'FM TOM', 22050, 0.1),
        ('chin.wav', 'FM BELL', 22050, 0.21), ('gyumo_s.wav', 'FM CRASH', 22050, 0.34)],
    'random': [
        ('piano.wav', 'PIANO', 16000, 1.5), ('choir.wav', 'CHOIR', 16000, 1.6), ('amen.wav', 'AMEN', 22050, None),
        ('safari.wav', 'SAFARI', 22050, None), ('drone.wav', 'DRONE', 11025, 1.0), ('blip.wav', 'BLIP', 22050, 0.2),
        ('marimba.wav', 'MARIMBA', 11025, 2.6), ('vinyl.wav', 'SCRATCH', 22050, 0.3)],
}
BANK = [(d, *e) for d, rows in BANKS.items() for e in rows]
FLASH = 0x4F000   # the FM-1's room for sounds (app/core/pw_bank.h PW_BANK_ROOM, 316 KB)


def read(path):
    w = wave.open(str(path))
    ch, sr, sw = w.getnchannels(), w.getframerate(), w.getsampwidth()
    raw = w.readframes(w.getnframes())
    if sw == 1: x = (np.frombuffer(raw, np.uint8).astype(np.float64) - 128) / 128
    elif sw == 2: x = np.frombuffer(raw, '<i2').astype(np.float64) / 32768
    elif sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int64)
        v = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
        x = np.where(v >= 1 << 23, v - (1 << 24), v) / float(1 << 23)
    else: raise AssertionError(f'{path}: 8, 16 or 24-bit WAV expected')
    return x.reshape(-1, ch).mean(1), sr


def shape(x, sr, rate, longest):
    if longest is None:                                # kept whole (the drum breaks): no cut, no fade
        g = np.gcd(rate, sr)
        y = resample_poly(x, rate // g, sr // g)
        return y / (np.max(np.abs(y)) or 1.0) * 26000
    peak = np.max(np.abs(x)) or 1.0
    loud = np.where(np.abs(x) > peak * 10 ** (-54 / 20))[0]
    x = x[: (loud[-1] + 1) if len(loud) else len(x)]
    n = min(len(x), int(longest * sr))
    if n < len(x):                                     # shortened: a fade over the last 30%
        x = x[:n].copy(); f = int(n * 0.3); x[-f:] *= np.linspace(1, 0, f) ** 2
    else:
        x = x.copy(); f = min(len(x), int(sr * 0.004)); x[-f:] *= np.linspace(1, 0, f)
    g = np.gcd(rate, sr)
    y = resample_poly(x, rate // g, sr // g)
    return y / (np.max(np.abs(y)) or 1.0) * 26000


if __name__ == '__main__':
    root = pathlib.Path(__file__).resolve().parent.parent
    wavs = root / 'tests' / 'out' / 'bank'; wavs.mkdir(parents=True, exist_ok=True)
    out, total, sectors = [], 0, 0
    for i, (d, fn, name, rate, longest) in enumerate(BANK):
        x, sr = read(root / 'assets' / 'samples' / d / fn)
        d = np.round(shape(x, sr, rate, longest)).astype('<i2')
        total += d.nbytes; sectors += -(-(len(d) * 34 // 80 + 1) // 4096) * 4096   # (sent packed with SEA, about 3.4 bits a sample, from a 4 KB sector)
        out.append({'name': name, 'rate': rate, 'd': base64.b64encode(d.tobytes()).decode()})
        with wave.open(str(wavs / f'{"ABCD"[i // 8]}{i % 8 + 1}_{fn}'), 'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(d.tobytes())
        print(f'{"ABCD"[i // 8]}{i % 8 + 1} {name:11s} {rate:6d} Hz {len(d) / rate:5.2f} s {d.nbytes:7d} B')
    (root / 'web' / 'factory_bank.json').write_text(json.dumps(out))
    print(f'total {total} B as samples; all 32 on the FM-1 (SEA, 4 KB sectors) about {sectors} B of {FLASH}')
    assert sectors <= FLASH - 16384 * 2, 'leave some room'
