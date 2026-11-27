#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""What drives table word 0x00? Offline analysis of capture_table.py output.

1. Under load, how often power (0x0C) equals 0x00, 0x08 and 0x10: which cap binds.
2. Every table word and every sensor correlated with 0x00 over the whole
   capture, with lag search: the top candidates are what 0x00 follows.
3. The values 0x00 settles at, compared with the BIOS STT power limits
   (DSDT AL2E per profile: 25, 35, 45, 60 W).
Run: python3 tests/analyze_capture.py capture-PROFILE.bin
"""
import math
import struct
import sys

TABLE_SIZE = 0x1028
WORDS = TABLE_SIZE // 4
STT_W = {25.0: "AL2E 0x61A8", 35.0: "AL2E 0x88B8", 45.0: "AL2E 0xAFC8 (balanced)", 60.0: "AL2E 0xEA60"}
LAGS = (0, 10, 50, 100, 300)


def load(path):
    labels = open(path[:-4] + ".txt").read().splitlines()
    rec = 8 * (1 + len(labels)) + TABLE_SIZE
    data = open(path, "rb").read()
    rows = []
    for i in range(len(data) // rec):
        off = i * rec
        head = struct.unpack_from(f"<{1 + len(labels)}d", data, off)
        words = struct.unpack_from(f"<{WORDS}f", data, off + 8 * (1 + len(labels)))
        rows.append((head[0], head[1:], words))
    return labels, rows


def corr(a, b):
    pairs = [(x, y) for x, y in zip(a, b) if math.isfinite(x) and math.isfinite(y)]
    if len(pairs) < 50:
        return 0.0
    xs, ys = zip(*pairs)
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    sx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy = math.sqrt(sum((y - my) ** 2 for y in ys))
    return 0.0 if not sx or not sy else sum((x - mx) * (y - my) for x, y in pairs) / (sx * sy)


def best_lag(target, series):
    """Highest |r| between target and series shifted earlier by each lag (samples)."""
    best = (0.0, 0)
    for lag in LAGS:
        r = corr(target[lag:], series[:len(series) - lag])
        if abs(r) > abs(best[0]):
            best = (r, lag)
    return best


def main():
    labels, rows = load(sys.argv[1])
    w0 = [r[2][0] for r in rows]
    print(f"{len(rows)} records over {rows[-1][0]:.0f} s; 0x00 range {min(w0):.2f}-{max(w0):.2f} W")

    loaded = [r for r in rows if r[2][3] > 20]
    if loaded:
        binds = {"0x00": 0, "0x08": 0, "0x10": 0, "none": 0}
        for r in loaded:
            caps = {"0x00": r[2][0], "0x08": r[2][2], "0x10": r[2][4]}
            k = min(caps, key=caps.get)
            binds[k if abs(r[2][3] - caps[k]) < 0.5 else "none"] += 1
        print("binding cap under load (0x0C within 0.5 W of the lowest):",
              ", ".join(f"{k} {100 * v / len(loaded):.0f}%" for k, v in binds.items()))

    cands = []
    for i in range(1, WORDS):
        s = [r[2][i] for r in rows]
        if max(s) - min(s) < 1e-3 or not all(math.isfinite(x) and abs(x) < 1e6 for x in s):
            continue
        r, lag = best_lag(w0, s)
        cands.append((abs(r), r, lag, f"word {i * 4:#06x}"))
    for j, label in enumerate(labels):
        s = [r[1][j] for r in rows]
        finite = [x for x in s if math.isfinite(x)]
        if len(finite) < 50 or max(finite) - min(finite) < 1e-3:
            continue
        r, lag = best_lag(w0, s)
        cands.append((abs(r), r, lag, f"sensor {label}"))
    print("strongest relations with 0x00 (r, lag in samples of 0.1 s):")
    for _, r, lag, what in sorted(cands, reverse=True)[:20]:
        print(f"  r {r:+.3f}  lag {lag:3d}  {what}")

    plateaus = {}
    for x in w0:
        k = round(x, 1)
        plateaus[k] = plateaus.get(k, 0) + 1
    common = sorted(plateaus.items(), key=lambda kv: -kv[1])[:5]
    print("most frequent 0x00 values (W: samples):", ", ".join(f"{k}: {v}" for k, v in common))
    for k, _ in common:
        for w, src in STT_W.items():
            if abs(k - w) < 0.15:
                print(f"  {k} W matches BIOS STT power limit {src}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
