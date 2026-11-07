"""The factory sample bank from assets/samples/*.wav: drums first, then the pitched sounds. Each sound is mixed to
mono, its silent tail cut (and a long one shortened, with a fade), resampled to a lower rate so all 16 fit the
FM-1's 208 KB bank with room to spare, and normalised to the sampler's full scale (26000).

    python3 tools/import_bank.py      -> web/factory_bank.json (the page builds it in) and tests/out/bank/*.wav
"""
import base64, json, pathlib, wave
import numpy as np
from scipy.signal import resample_poly

# slot order: (file, name on the FM-1 (12 letters), rate Hz, longest seconds)
BANK = [
    ('bd.wav', 'BD', 11025, 0.7), ('snare.wav', 'SNARE', 22050, 0.3), ('clap.wav', 'CLAP', 22050, 0.3),
    ('rimshot.wav', 'RIMSHOT', 22050, 0.1), ('hatclosed.wav', 'HAT CLOSED', 22050, 0.2), ('hatopen.wav', 'HAT OPEN', 16000, 0.45),
    ('tom_l.wav', 'TOM L', 11025, 0.5), ('tom_m.wav', 'TOM M', 11025, 0.5), ('tom_h.wav', 'TOM H', 11025, 0.5),
    ('cowbell.wav', 'COWBELL', 11025, 0.4), ('claves.wav', 'CLAVES', 22050, 0.1), ('cabasa.wav', 'CABASA', 22050, 0.1),
    ('crash.wav', 'CRASH', 16000, 0.5),
    ('vibes.wav', 'VIBES', 11025, 0.8), ('soft.wav', 'SOFT', 11025, 1.0), ('future.wav', 'FUTURE', 11025, 0.45),
]
FLASH = 0xDC000 - 0xA8000


def read(path):
    w = wave.open(str(path))
    ch, sr, sw = w.getnchannels(), w.getframerate(), w.getsampwidth()
    assert sw == 2, f'{path}: 16-bit WAV expected'
    x = np.frombuffer(w.readframes(w.getnframes()), '<i2').astype(np.float64).reshape(-1, ch).mean(1) / 32768
    return x, sr


def shape(x, sr, rate, longest):
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
    for i, (fn, name, rate, longest) in enumerate(BANK):
        x, sr = read(root / 'assets' / 'samples' / fn)
        d = np.round(shape(x, sr, rate, longest)).astype('<i2')
        total += d.nbytes; sectors += -(-d.nbytes // 4096) * 4096
        out.append({'name': name, 'rate': rate, 'd': base64.b64encode(d.tobytes()).decode()})
        with wave.open(str(wavs / f'B{i + 1:02d}_{fn}'), 'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(d.tobytes())
        print(f'B{i + 1:<2d} {name:11s} {rate:6d} Hz {len(d) / rate:5.2f} s {d.nbytes:7d} B')
    (root / 'web' / 'factory_bank.json').write_text(json.dumps(out))
    print(f'total {total} B; in the FM-1 flash (4 KB sectors) {sectors} B of {FLASH}')
    assert sectors <= FLASH - 16384, 'leave some room'
