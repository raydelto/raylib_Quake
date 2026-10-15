#!/usr/bin/env python3
"""Compare two evidence dirs from `verify_macos.py frames`: per-frame differing-pixel fraction.
Run-to-run noise on one build is tiny (particles use rand()); a demo desync diverges the whole frame.
usage: compare_frames.py DIR_A DIR_B [max-diff-fraction, default 0.01]"""
import sys
from pathlib import Path


def pcx(path):
    d = path.read_bytes()
    w = d[8] + d[9] * 256 + 1
    h = d[10] + d[11] * 256 + 1
    out = bytearray()
    i = 128
    while len(out) < w * h:
        c = d[i]
        i += 1
        if c >= 0xC0:
            out += bytes([d[i]]) * (c & 0x3F)
            i += 1
        else:
            out.append(c)
    return w, h, bytes(out[:w * h])


def main():
    a, b = Path(sys.argv[1]), Path(sys.argv[2])
    limit = float(sys.argv[3]) if len(sys.argv) > 3 else 0.01
    bad = 0
    for fa in sorted(a.glob('*.pcx')):
        fb = b / fa.name
        if not fb.exists():
            print('MISSING', fa.name)
            bad += 1
            continue
        wa, ha, pa = pcx(fa)
        wb, hb, pb = pcx(fb)
        if (wa, ha) != (wb, hb):
            print('SIZE', fa.name, (wa, ha), (wb, hb))
            bad += 1
            continue
        diff = sum(x != y for x, y in zip(pa, pb)) / len(pa)
        ok = diff <= limit
        bad += not ok
        print('%s %-12s differing %.3f%% (%d px)' % ('PASS' if ok else 'FAIL', fa.stem, diff * 100, int(diff * len(pa))))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
