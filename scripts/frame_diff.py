#!/usr/bin/env python3
"""research/109: unambiguous frame-diff for Mango frame dumps (PPM/PNG).

Usage: frame_diff.py A B [A2 B2 ...]   (pairs; also: --prefix P compares the
standard OpenTTD input pairs of frame dump prefix P)

For every pair it prints BOTH metrics on the full frame, with the bbox:
  maxch>T : pixels where max(|dR|,|dG|,|dB|) > T   (research/107-108 executor)
  lumad>T : pixels where PIL L(|dRGB|) > T, L = (299R+587G+114B)/1000 of the
            per-channel absolute difference image (Lab /108 gate; reproduces
            146,284 for 108 frame-D input-0-before vs input-1-scroll)
Default T=16. Report numbers as "<metric>: N px (pct of W*H)".
"""
import sys
from PIL import Image, ImageChops
import numpy as np

PAIRS = [("input-0-before", "input-1-scroll", "scroll"),
         ("input-1-scroll", "input-2-click", "click"),
         ("input-2-click", "input-3-closed", "closed"),
         ("input-3-closed", "input-4-wheel", "wheel")]


def diff(a_path, b_path, t):
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    d = ImageChops.difference(a, b)
    arr = np.asarray(d).astype(int)
    n = arr.shape[0] * arr.shape[1]
    maxch = int((arr.max(axis=2) > t).sum())
    lumad = int((np.asarray(d.convert("L")) > t).sum())
    return n, maxch, lumad, d.getbbox()


def main(argv):
    t = 16
    if "--t" in argv:
        i = argv.index("--t"); t = int(argv[i + 1]); del argv[i:i + 2]
    pairs = []
    if argv and argv[0] == "--prefix":
        p = argv[1]
        pairs = [(f"{p}.{x}.ppm", f"{p}.{y}.ppm", lab) for x, y, lab in PAIRS]
    else:
        pairs = [(argv[i], argv[i + 1], f"pair{i // 2}") for i in range(0, len(argv) - 1, 2)]
    print(f"threshold T={t}; metrics: maxch=max-channel|d|>T, lumad=L(|dRGB|)>T")
    for a, b, lab in pairs:
        try:
            n, m, l, bb = diff(a, b, t)
        except FileNotFoundError as e:
            print(f"{lab}: missing {e.filename}"); continue
        print(f"{lab}: {a} vs {b}: maxch={m} ({100.0*m/n:.1f}%) lumad={l} ({100.0*l/n:.1f}%) bbox={bb}")


if __name__ == "__main__":
    main(sys.argv[1:])
