#!/bin/sh
# Estimate the device build's RAM (.data+.bss) and pool use without JieLi's toolchain: unity.c and the core compiled
# for a 32-bit target with stock clang (the BSP's inline assembly emptied in the IR), sizes summed per section.
# Only an estimate (another instruction set, another compiler): tools/build.py's numbers are the real ones.
set -e
cd "$(dirname "$0")/.."
B=base; O=${TMPDIR:-/tmp}/patchwerk_ram; mkdir -p $O
FL="--target=riscv32-unknown-elf -march=rv32imf -mabi=ilp32f -Os -ffunction-sections -fdata-sections -fno-builtin
 -fno-common -Wall -Wno-unused-function -I$B/bsp/chip -I$B/bsp/boards/fm1 -I$B/device -I$B/include -I$B/lib -Iapp
 -Iapp/build/gen -Iapp/core -Iapp/devinc -DFELUCCA_ID=\"FM-1_60600\" -DPW_FM1B $EXTRA"
clang $FL -S -emit-llvm -c $B/device/unity.c -o $O/unity.ll
sed -E 's/asm (sideeffect )?(inteldialect )?"[^"]*"/asm \1""/g' $O/unity.ll > $O/unity2.ll
clang --target=riscv32-unknown-elf -march=rv32imf -mabi=ilp32f -c $O/unity2.ll -o $O/unity.o 2>/dev/null
objs=$O/unity.o
for f in app/core/pw_engine.c app/core/pw_modules.c app/core/pw_presets.c app/core/pw_ui.c app/core/pw_link.c app/core/pw_bank.c; do
  clang $FL -O2 -c $f -o $O/$(basename $f .c).o; objs="$objs $O/$(basename $f .c).o"
done
python3 - $objs <<'PY'
import subprocess, sys, re
pool = ram = 0; big = []
for o in sys.argv[1:]:
    out = subprocess.run(["llvm-objdump", "-t", o], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ .{7} (\S+)\s+([0-9a-f]+) (\S+)$", line)
        if not m: continue
        sec, size, name = m.group(1), int(m.group(2), 16), m.group(3)
        if sec.startswith(".pool"): pool += size; big.append((size, "pool", name))
        elif sec.startswith((".bss", ".data", ".sbss", ".sdata")): ram += size; big.append((size, "ram", name))
import subprocess as sp
fl = 0
for o in sys.argv[1:]:
    r = sp.run(["llvm-size", o], capture_output=True, text=True).stdout.split("\n")[1].split()
    fl += int(r[0])
print(f"FLASH (estimate, our own code: text sum) ~{fl} B. The real number is the build's own line ('image N B', room 581564 B, the app region of base/device/app.ld): rc16 was 562068 B, 96.6 % full. Do not trust this estimate for the margin.")
print(f"RAM .data+.bss ~{ram} B of 98304 (spare {98304-ram}; the real build used ~9.4 KB more at v20: keep this spare above 10 KB); pool ~{pool} B of 344064 (spare {344064-pool}, 8192 required)")
for s, k, n in sorted(big, reverse=True)[:25]: print(f"  {k:4} {s:7} {n}")
PY
