"""Repeatable benchmark: python bench.py <name> <seconds> [SAVE=<save_backups folder>] [SHOT=<every n frames>] [ENV=VAL ...]

Never touches the player's save: every run starts from a fresh copy of a test
save (default newgame_20261107_beach) in build_work/out/bench_save via
SW_SAVE_DIR.  Only scripted input (SW_INPUT=script + SW_AUTO_A), so the real
controller and keyboard cannot move the player.  Prints a summary of the
[perf] lines of the last 60 s."""
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / "build_work" / "out"
name, secs = sys.argv[1], float(sys.argv[2])
save = "newgame_20261107_beach"
exe = OUT / "Pokemon Arceus.exe"
env = dict(os.environ)
env.update({"SW_AUTO_A": "20,2.5", "SW_SWKBD_ANSWER": "Test", "SW_INPUT": "script", "SW_SHOT_EVERY": "0",
            # an off-screen window that never takes the focus (gfx_window_win.c): a test does not cover the
            # user's game. BENCH_PRIORITY=normal keeps the normal CPU priority (default below normal).
            "SW_BACKGROUND_WINDOW": "1"})
low = os.environ.get("BENCH_PRIORITY", "low") != "normal"
affinity = 0
for kv in sys.argv[3:]:
    k, v = kv.split("=", 1)
    if k == "SAVE":
        save = v
    elif k == "EXE":          # another build (e.g. a backup) run from build_work/out
        exe = Path(v).resolve()
    elif k == "AFFINITY":     # hex CPU mask (e.g. F = 4 logical CPUs): a weaker PC on this one
        affinity = int(v, 16)
    elif k == "SHOT":
        env["SW_SHOT_EVERY"] = v
    elif k == "ROUTE":        # a recorded route (F9 in the game, bench_routes/<name>): its save, then its input
        route = Path(v).resolve()
        env["SW_INPUT_REPLAY"] = str(route)
        env["SW_INPUT_REPLAY_EXIT"] = "1"   # the game closes when the route is done: secs is only a time-out
        env["SW_AUTO_A"] = "20,2.5"         # A presses get into the world; they stop there (sw_hid.c) and the
                                            # route starts 1.5 s after the first field-camera frame
    else:
        env[k] = v
bench_save = OUT / "bench_save"
if bench_save.exists():
    shutil.rmtree(bench_save)
route = Path(env["SW_INPUT_REPLAY"]) if env.get("SW_INPUT_REPLAY") else None
shutil.copytree(route / "save" if route else OUT / "save_backups" / save, bench_save)
env["SW_SAVE_DIR"] = "bench_save"
user_save = {p.name: p.stat().st_mtime for p in (OUT / "save").iterdir()}
log = OUT / "logs" / f"bench_{name}.log"
with open(log, "wb") as f:
    p = subprocess.Popen([str(exe), "--real-start", "--max-dispatch", "0"], cwd=OUT, env=env,
                         stdout=f, stderr=subprocess.STDOUT,
                         creationflags=0x4000 if low else 0)   # BELOW_NORMAL_PRIORITY_CLASS
    if affinity and sys.platform == "win32":   # set at once: the game's threads start later and inherit it
        import ctypes
        ok = ctypes.windll.kernel32.SetProcessAffinityMask(ctypes.c_void_p(int(p._handle)), ctypes.c_size_t(affinity))
        print(f"affinity {affinity:#x}: {'set' if ok else 'FAILED'}")
    t0 = time.time()
    while time.time() - t0 < secs and p.poll() is None:
        time.sleep(1)
    alive = p.poll() is None
    if alive:
        p.kill()
    p.wait()
changed = [n for n, t in user_save.items() if (OUT / "save" / n).stat().st_mtime != t]
rows = []
text = log.read_text(errors="replace")
for line in text.splitlines():
    m = re.match(r"\[perf\] ([\d.]+) fps, frame avg ([\d.]+) ms, worst ([\d.]+) ms, hitches\(>50ms\) (\d+)", line)
    if m:
        rows.append(tuple(float(x) for x in m.groups()))
tail = rows[-6:]
traps = [l for l in text.splitlines() if l.startswith("[trap]") or "CRASH HANDLER" in l]
note = ("; TRAP " + traps[0][:90] if traps else "") + ("; USER SAVE CHANGED " + str(changed) if changed else "")
if tail:
    print(f"{name}: fps {sum(r[0] for r in tail) / len(tail):.1f} (min {min(r[0] for r in tail):.1f}), "
          f"worst frame {max(r[2] for r in tail):.0f} ms, hitches {sum(r[3] for r in tail):.0f} "
          f"over the last {len(tail)} intervals{note}")
else:
    print(f"{name}: no perf lines{note}")
