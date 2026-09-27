#!/usr/bin/env python3
# TEMPORARY crash symbolizer: maps raw crash-log frame addresses to symbols
# via the exe's COFF symbol table (nm -C) + the boot-time base recorded by
# CrashHandler (---- BOOT base=... ----). Diagnostic tooling, not product code.
import re
import subprocess
import sys
from bisect import bisect_right

CRASH = sys.argv[1]
BASE = int(sys.argv[2], 16)          # runtime base of the exe image
EXE = sys.argv[3] if len(sys.argv) > 3 else "build/appVGRPresenterUI.exe"

nm = subprocess.run([r"C:\msys64\ucrt64\bin\nm.exe", "-C", EXE],
                    capture_output=True, text=True).stdout
syms = []                             # sorted (va, name)
pat = re.compile(r"^([0-9a-fA-F]{8,16})\s+[TtWwBbDdRr]\s+(.+)$")
for line in nm.splitlines():
    m = pat.match(line)
    if m:
        syms.append((int(m.group(1), 16), m.group(2)))
syms.sort()
vas = [v for v, _ in syms]

def resolve(addr):
    rva = addr - BASE
    va = 0x140000000 + rva            # exe ImageBase
    i = bisect_right(vas, va) - 1
    if i < 0:
        return f"rva=0x{rva:X}  <no symbol>"
    sym_va, name = syms[i]
    off = va - sym_va
    tag = "" if 0 <= off < 0x8000 else "  (far offset — fuzzy)"
    return f"rva=0x{rva:X}  {name}+0x{off:X}{tag}"

text = open(CRASH, encoding="utf-8", errors="replace").read()
print(f"=== {CRASH} @ base=0x{BASE:X} ===")
for line in text.splitlines():
    line = line.strip()
    m = re.match(r"code=0x[0-9a-f]+ address=([0-9a-f]+)", line)
    if m:
        print(f"FAULTING IP  {resolve(int(m.group(1), 16))}")
        continue
    m = re.match(r"#(\d+)\s+([0-9a-f]+)", line)
    if m:
        addr = int(m.group(2), 16)
        if BASE <= addr < BASE + 0x4000000:
            print(f"  #{m.group(1):>2} {resolve(addr)}")
        else:
            print(f"  #{m.group(1):>2} 0x{addr:X}  (system dll)")
