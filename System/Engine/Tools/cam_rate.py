"""Camera turn speed from a SW_FIND_RATE run's [trace] lines (the right stick is held 0..2 s of the trace).

    python Engine/Tools/cam_rate.py build_work/out/logs/bench_<name>.log [...]

For each traced value: an unwrapped angle (monotonic, moves > 1.3) -> least-squares slope in rad/s; a sin/cos
term of the view matrix (oscillates) -> pi / mean time between crossings of its mid value.  At the same stick
deflection both should be the same at 30 / 60 / 90 / 120 fps."""
import math
import re
import sys


def analyse(path):
    rows = []
    for line in open(path, errors="replace"):
        if not line.startswith("[trace] ") or "watching" in line:
            continue
        parts = line.split()
        try:
            t = float(parts[1])
        except ValueError:
            continue
        vals = {}
        for p in parts[2:]:
            k, _, v = p.partition("=")
            try:
                vals[k] = float(v)
            except ValueError:
                pass
        rows.append((t, vals))
    held = [(t, v) for t, v in rows if 0.15 <= t <= 1.95]
    if not held:
        return None, []
    out = []
    for key in held[0][1]:
        ser = [(t, v[key]) for t, v in held if key in v and math.isfinite(v[key]) and abs(v[key]) < 1e4]
        if len(ser) < 10:
            continue
        ys = [y for _, y in ser]
        lo, hi = min(ys), max(ys)
        mono = all(b <= a for a, b in zip(ys, ys[1:])) or all(b >= a for a, b in zip(ys, ys[1:]))
        if mono and hi - lo > 1.3 and hi - lo < 40:
            n = len(ser); mt = sum(t for t, _ in ser) / n; my = sum(ys) / n
            slope = sum((t - mt) * (y - my) for t, y in ser) / sum((t - mt) ** 2 for t, _ in ser)
            out.append((key, "angle", abs(slope)))
        elif not mono and 0.3 < hi - lo <= 2.6:
            mid = (lo + hi) / 2
            xs = []
            for (t0, a), (t1, b) in zip(ser, ser[1:]):
                if (a - mid) * (b - mid) < 0:
                    xs.append(t0 + (t1 - t0) * (mid - a) / (b - a))
            if len(xs) >= 2:
                out.append((key, "sincos", math.pi / ((xs[-1] - xs[0]) / (len(xs) - 1))))
            else:   # slow turn: half a period between the maximum and the minimum (both inside the window)
                imax, imin = ys.index(hi), ys.index(lo)
                if 0 < imax < len(ys) - 1 and 0 < imin < len(ys) - 1:
                    out.append((key, "sincos", math.pi / abs(ser[imax][0] - ser[imin][0])))
    fps = None
    for line in open(path, errors="replace"):
        m = re.search(r"target (\d+) fps", line)
        if m:
            fps = int(m.group(1))
    return fps, out


for p in sys.argv[1:]:
    fps, out = analyse(p)
    angles = [r for _, kind, r in out if kind == "angle"]
    sc = sorted(r for _, kind, r in out if kind == "sincos")
    med = sc[len(sc) // 2] if sc else float("nan")
    print(f"{p}: target {fps} fps: unwrapped-angle rate {', '.join(f'{a:.2f}' for a in angles) or '-'} rad/s; "
          f"matrix-term rate median {med:.2f} rad/s ({len(sc)} terms)")
