"""LodeStar "Build now" setup: the installer starts it when the player chooses Build now (it can be run again).

    pyw System/launcher/setup_wizard.py

Steps: 1 controller -> 2 settings (picked for this PC, pc_advice.py) -> 3 game files (Import, no decryption) ->
4 build (missing build tools installed with winget first, then translate + compile: one progress bar, a timer and
the time left) -> 5 done.  Everything it writes goes into the install folder (Settings/, Game/, System/)."""
from __future__ import annotations

import ctypes
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox

sys.path.insert(0, str(Path(__file__).resolve().parent))
import launcher as L  # noqa: E402  (paths, dump checks, settings, build progress, Task)
import pc_advice  # noqa: E402
import ui_kit as ui  # noqa: E402

ROOT = L._project_root()
SYSTEM = ROOT / L.SYSTEM_DIR
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
STEPS = ("Controller", "Settings", "Game files", "Build", "Done")

# ---- what the build needs, and how winget installs it -------------------------------------------------------------
WINGET = {
    "msvc": ["Microsoft.VisualStudio.BuildTools", "--override",
             "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"],
    "llvm": ["LLVM.LLVM"],
    "dotnet": ["Microsoft.DotNet.Runtime.10"],
    "vulkan": ["KhronosGroup.VulkanSDK"],
}


def has_avx2() -> bool:
    try:
        return bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(40))   # PF_AVX2_INSTRUCTIONS_AVAILABLE
    except (AttributeError, OSError):
        return True


def check_pc() -> list[dict]:
    """[{key, name, ok, detail, fix}] - fix: a winget key, or "" when nothing can be installed for it."""
    rows = []
    rows.append({"key": "cpu", "name": "Processor with AVX2", "ok": has_avx2(),
                 "detail": "Intel Haswell / AMD Ryzen or newer", "fix": ""})
    vk = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "vulkan-1.dll"
    rows.append({"key": "gpu", "name": "Graphics driver with Vulkan", "ok": vk.is_file(),
                 "detail": "update your graphics driver if this is missing", "fix": ""})
    msvc = False
    try:
        sys.path.insert(0, str(SYSTEM / "Engine" / "Source"))
        from swrecomp.pipeline import build as b
        msvc = b.find_toolchain().name == "msvc"
    except Exception:
        msvc = False
    rows.append({"key": "msvc", "name": "Visual Studio Build Tools (C++)", "ok": msvc,
                 "detail": "compiles the game (about 3 GB download)", "fix": "msvc"})
    llvm = Path(r"C:\Program Files\LLVM\bin\clang-cl.exe").is_file()
    rows.append({"key": "llvm", "name": "LLVM / clang", "ok": llvm, "detail": "makes the game fast", "fix": "llvm"})
    dotnet = False
    try:
        r = subprocess.run(["dotnet", "--list-runtimes"], capture_output=True, text=True, timeout=30, creationflags=NO_WINDOW)
        dotnet = "Microsoft.NETCore.App 10." in r.stdout
    except (OSError, subprocess.SubprocessError):
        dotnet = False
    rows.append({"key": "dotnet", "name": ".NET 10 runtime", "ok": dotnet, "detail": "the shader translator", "fix": "dotnet"})
    vks = os.environ.get("VULKAN_SDK") or ""
    sdk = bool(vks and Path(vks).is_dir()) or any(Path(r"C:\VulkanSDK").glob("*/Lib/vulkan-1.lib"))
    rows.append({"key": "vulkan", "name": "Vulkan SDK", "ok": sdk, "detail": "graphics headers for the build", "fix": "vulkan"})
    return rows


def winget_cmd(key: str) -> list[str]:
    pkg = WINGET[key]
    return ["winget", "install", "-e", "--id", pkg[0], *pkg[1:], "--accept-source-agreements",
            "--accept-package-agreements", "--silent", "--disable-interactivity"]


def xinput_pads() -> int:
    """How many XInput controllers are connected (0..4)."""
    class State(ctypes.Structure):
        _fields_ = [("packet", ctypes.c_uint32), ("buttons", ctypes.c_uint16), ("lt", ctypes.c_ubyte),
                    ("rt", ctypes.c_ubyte), ("lx", ctypes.c_short), ("ly", ctypes.c_short), ("rx", ctypes.c_short),
                    ("ry", ctypes.c_short)]
    for dll in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
        try:
            x = getattr(ctypes.windll, dll)
            break
        except OSError:
            x = None
    if x is None:
        return 0
    s = State()
    return sum(1 for i in range(4) if x.XInputGetState(i, ctypes.byref(s)) == 0)


def fmt_time(s: float) -> str:
    s = int(max(0, s))
    return f"{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}" if s >= 3600 else f"{s // 60}:{s % 60:02d}"


class Wizard:
    def __init__(self, root: tk.Tk):
        self.root = root
        st = L._load_settings()
        ui.set_theme("dark" if st.get("theme") == "Dark" else "paper")
        self.c = L._colors()
        self.T = ui.T
        root.title("LodeStar - build the game")
        root.configure(bg=self.T["page"])
        root.geometry("920x640")
        root.minsize(860, 600)
        try:
            root.iconbitmap(str(SYSTEM / "launcher" / "assets" / "launcher.ico"))
        except tk.TclError:
            pass
        self.step = 0
        self.q: queue.Queue = queue.Queue()
        self.task = None
        self.hw = None
        self.pc_rows: list[dict] = []
        self.busy = False
        # layout: step list on the left, the page on the right, buttons at the bottom
        side = tk.Frame(root, bg=self.T["canvas"], width=220)
        side.pack(side="left", fill="y")
        side.pack_propagate(False)
        tk.Label(side, text="LodeStar", font=ui.F(20, 700), bg=self.T["canvas"], fg=self.T["ink"]).pack(anchor="w", padx=24, pady=(26, 2))
        tk.Label(side, text="Setup", font=ui.F(11), bg=self.T["canvas"], fg=self.T["inkMuted48"]).pack(anchor="w", padx=24)
        self.step_labels = []
        lst = tk.Frame(side, bg=self.T["canvas"])
        lst.pack(anchor="w", padx=18, pady=(28, 0))
        for i, name in enumerate(STEPS):
            lab = tk.Label(lst, text=f"  {i + 1}   {name}", font=ui.F(12), anchor="w", width=18, bg=self.T["canvas"],
                           fg=self.T["inkMuted48"], pady=7)
            lab.pack(anchor="w")
            self.step_labels.append(lab)
        main = tk.Frame(root, bg=self.T["page"])
        main.pack(side="left", fill="both", expand=True)
        self.page = tk.Frame(main, bg=self.T["page"], padx=36, pady=28)
        self.page.pack(fill="both", expand=True)
        nav = tk.Frame(main, bg=self.T["page"], padx=36, pady=18)
        nav.pack(fill="x", side="bottom")
        self.btn_next = ui.PillButton(nav, "Next", self.next, kind="primary", bg=self.T["page"])
        self.btn_next.pack(side="right")
        self.btn_back = ui.PillButton(nav, "Back", self.back, kind="secondary", bg=self.T["page"])
        self.btn_back.pack(side="right", padx=10)
        self.nav_note = tk.StringVar()
        tk.Label(nav, textvariable=self.nav_note, font=ui.F(10), bg=self.T["page"], fg=self.T["inkMuted48"]).pack(side="left")
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(100, self.pump)
        self.show()

    # ---- frame helpers
    def clear(self):
        for w in self.page.winfo_children():
            w.destroy()

    def title(self, text: str, sub: str = ""):
        tk.Label(self.page, text=text, font=ui.F(24, 700), bg=self.T["page"], fg=self.T["ink"], anchor="w").pack(anchor="w")
        if sub:
            tk.Label(self.page, text=sub, font=ui.F(11), bg=self.T["page"], fg=self.T["inkMuted48"], wraplength=600,
                     justify="left", anchor="w").pack(anchor="w", pady=(4, 18))

    def card(self):
        cd = ui.Card(self.page, bg=self.T["page"])
        cd.pack(fill="x", pady=(0, 12))
        return cd.body

    def label(self, parent, text="", var=None, size=11, weight=400, muted=False, wrap=560):
        return tk.Label(parent, text=text, textvariable=var, font=ui.F(size, weight), bg=self.T["canvas"],
                        fg=self.T["inkMuted48"] if muted else self.T["ink"], wraplength=wrap, justify="left", anchor="w")

    def show(self):
        for i, lab in enumerate(self.step_labels):
            cur = i == self.step
            lab.configure(fg=self.T["ink"] if cur or i < self.step else self.T["inkMuted48"],
                          font=ui.F(12, 700 if cur else 400),
                          text=f"  {'✓' if i < self.step else i + 1}   {STEPS[i]}")
        self.clear()
        self.nav_note.set("")
        self.btn_back.configure(text="Back")
        self.btn_next.configure(text="Next")
        [self.page_pad, self.page_settings, self.page_files, self.page_build, self.page_done][self.step]()

    def next(self):
        if self.busy:
            return
        if self.step == len(STEPS) - 1:
            return self.finish()
        self.step += 1
        self.show()

    def back(self):
        if self.busy or self.step == 0:
            return
        self.step -= 1
        self.show()

    def close(self):
        if self.task and self.task.proc.poll() is None:
            if not messagebox.askyesno("LodeStar setup", "The build is still running. Stop it and close?", parent=self.root):
                return
            self.task.stop()
        self.root.destroy()

    # ---- 1. your PC
    def page_pc(self):
        self.title("Your PC", "LodeStar builds the game on your PC from your own dump. These are the tools it needs; "
                              "anything missing can be installed here with one click (Windows' winget).")
        body = self.card()
        self.pc_box = tk.Frame(body, bg=self.T["canvas"])
        self.pc_box.pack(fill="x")
        self.pc_status = tk.StringVar(value="Checking...")
        self.label(body, var=self.pc_status, size=10, muted=True).pack(anchor="w", pady=(10, 0))
        self.btn_fix = ui.PillButton(body, "Install missing", self.install_missing, kind="primary", size="sm",
                                     bg=self.T["canvas"])
        self.btn_fix.pack(anchor="w", pady=(10, 0))
        threading.Thread(target=lambda: self.q.put(("pc", check_pc())), daemon=True).start()

    def draw_pc(self, rows):
        self.pc_rows = rows
        if self.step != 0 or not self.pc_box.winfo_exists():
            return
        for w in self.pc_box.winfo_children():
            w.destroy()
        for r in rows:
            line = tk.Frame(self.pc_box, bg=self.T["canvas"])
            line.pack(fill="x", pady=3)
            tk.Label(line, text="✓" if r["ok"] else "✗", font=ui.F(13, 700), width=2, bg=self.T["canvas"],
                     fg=self.c["good"] if r["ok"] else self.c["error"]).pack(side="left")
            self.label(line, r["name"], size=11, weight=600).pack(side="left")
            self.label(line, "   " + r["detail"], size=10, muted=True).pack(side="left")
        missing = [r for r in rows if not r["ok"] and r["fix"]]
        hard = [r for r in rows if not r["ok"] and not r["fix"]]
        if hard:
            self.pc_status.set("This PC cannot run LodeStar: " + ", ".join(r["name"] for r in hard) + ".")
        elif missing:
            self.pc_status.set(f"{len(missing)} tool(s) missing. Install them now, or continue and build later.")
        else:
            self.pc_status.set("Everything the build needs is installed.")
        self.btn_fix.configure(text=f"Install missing ({len(missing)})" if missing else "Nothing to install")

    def install_missing(self):
        missing = [r for r in self.pc_rows if not r["ok"] and r["fix"]]
        if not missing or self.busy:
            return
        self.busy = True
        self.pc_status.set("Installing... Windows may ask for permission (the C++ build tools need it).")

        def work():
            for r in missing:
                self.q.put(("pcmsg", f"Installing {r['name']}... (this can take several minutes)"))
                try:
                    p = subprocess.run(winget_cmd(r["fix"]), capture_output=True, text=True, errors="replace",
                                       timeout=3 * 3600, creationflags=NO_WINDOW)
                    ok = p.returncode == 0 or "already installed" in (p.stdout + p.stderr).lower()
                except (OSError, subprocess.SubprocessError) as e:
                    ok, p = False, None
                    self.q.put(("pcmsg", f"{r['name']}: {e}"))
                if not ok and p is not None:
                    self.q.put(("pcmsg", f"{r['name']} did not install (winget exit {p.returncode})."))
            self.q.put(("pcdone", check_pc()))
        threading.Thread(target=work, daemon=True).start()

    # ---- 2. controller
    def page_pad(self):
        self.title("Controller", "Plug in your controller now (Xbox and most PC controllers work), or play with "
                                 "keyboard and mouse. Every button can be changed here or later in the launcher.")
        body = self.card()
        self.pad_var = tk.StringVar(value="Looking for controllers...")
        self.label(body, var=self.pad_var, size=13, weight=600).pack(anchor="w")
        self.label(body, "Keyboard: WASD to move, mouse camera with F2, Enter = A, Backspace = B, Tab = +, M = -. "
                         "In the game, F1 (or Minus + Plus) opens the settings menu.", size=10, muted=True).pack(anchor="w", pady=(8, 0))
        row = tk.Frame(body, bg=self.T["canvas"])
        row.pack(anchor="w", pady=(14, 0))
        ui.PillButton(row, "Set up the controller", lambda: self.open_controls(1), kind="primary", size="sm",
                      bg=self.T["canvas"]).pack(side="left")
        ui.PillButton(row, "Keyboard & mouse keys", lambda: self.open_controls(0), kind="secondary", size="sm",
                      bg=self.T["canvas"]).pack(side="left", padx=8)
        self.poll_pad()

    def poll_pad(self):
        if self.step != 0 or not hasattr(self, "pad_var"):
            return
        n = xinput_pads()
        self.pad_var.set(f"✓  {n} controller{'s' if n != 1 else ''} connected" if n else
                         "No controller connected - keyboard and mouse work too")
        self.root.after(1000, self.poll_pad)

    def open_controls(self, tab: int):
        import controls_ui
        try:
            controls_ui.ControlsWindow(self.root, ROOT / "Settings", self.c, start_tab=tab)
        except Exception as e:   # the controls window is the launcher's: never let it stop the setup
            messagebox.showerror("Controls", f"The controls window could not open: {e}", parent=self.root)

    # ---- 3. game files
    def page_files(self):
        self.title("Game files", "Pick the folder with your own extracted dump of Pokémon Legends: Arceus 1.0.0: it "
                                 "holds an exefs folder (main, main.npdm, ...) and a romfs folder (bin, "
                                 "system_resource). Dump them from your Switch, e.g. with nxdumptool. LodeStar never "
                                 "decrypts game packages and no game files come with it.")
        body = self.card()
        self.files_var = tk.StringVar()
        self.label(body, var=self.files_var, size=12, weight=600).pack(anchor="w")
        self.files_note = tk.StringVar()
        self.label(body, var=self.files_note, size=10, muted=True).pack(anchor="w", pady=(6, 0))
        ui.PillButton(body, "Import game files...", self.import_files, kind="primary", size="sm",
                      bg=self.T["canvas"]).pack(anchor="w", pady=(14, 0))
        self.refresh_files()

    def refresh_files(self):
        ok, text = L.game_files_check(ROOT)
        ver = L.exefs_version(ROOT / "Game" / "exefs") if (ROOT / "Game" / "exefs" / "main").is_file() else ""
        self.files_var.set(("✓  " if ok else "") + ("Game files ready" if ok else "No game files yet"))
        self.files_note.set(text + (f"  ·  game version {ver}" if ver else ""))
        self.nav_note.set("" if ok else "You can import later in the launcher, but the game cannot be built without them.")

    def import_files(self):
        if self.busy:
            return
        src = filedialog.askdirectory(title="Folder with your extracted game files (holds exefs and romfs)", parent=self.root)
        if not src:
            return
        found = L.find_dump(Path(src))
        if not found["exefs"] or not found["romfs"]:
            messagebox.showerror("Import game files", found["why"], parent=self.root)
            return
        ok, text, _d = L.romfs_check(found["romfs"], ROOT)
        if not ok:
            messagebox.showerror("Import game files", "RomFS: " + text, parent=self.root)
            return
        ver = L.exefs_version(found["exefs"])
        if ver != "1.0.0" and not messagebox.askyesno(
                "Import game files", f"This is game version {ver}; LodeStar supports 1.0.0 (the base game without "
                                     "updates). Import anyway?", parent=self.root):
            return
        self.busy = True
        self.files_var.set("Copying your game files... (the RomFS is about 6 GB)")
        game = ROOT / "Game"

        def work():
            stamp = time.strftime("%Y%m%d_%H%M%S")
            try:
                game.mkdir(exist_ok=True)
                for name in ("exefs", "romfs"):
                    dst, s = game / name, found[name]
                    if dst.exists() and dst.resolve() == s.resolve():
                        continue
                    if dst.exists():
                        old = game / f"_old_{stamp}"
                        old.mkdir(exist_ok=True)
                        shutil.move(str(dst), str(old / name))
                    shutil.copytree(s, dst)
                cfg = L._load_config()
                cfg["exefs_dir"], cfg["romfs_dir"] = "Game/exefs", "Game/romfs"
                L._save_config(cfg)
                self.q.put(("files", ""))
            except OSError as e:
                self.q.put(("files", str(e)))
        threading.Thread(target=work, daemon=True).start()

    # ---- 4. settings
    def page_settings(self):
        self.title("Settings", "Picked for your PC. The game's own work always runs on the processor; choose where the "
                               "extra quality goes. Everything can be changed later (launcher Options, or F1 in the game).")
        if self.hw is None:
            try:
                self.hw = pc_advice.detect()
            except Exception:
                self.hw = {"cpu": "unknown processor", "threads": os.cpu_count() or 4, "ram_gb": 16, "gpu": "unknown", "vram_gb": 0}
        body = self.card()
        self.label(body, pc_advice.summary(self.hw), size=11, weight=600).pack(anchor="w")
        self.work_var = tk.StringVar(value=pc_advice.recommend_workload(self.hw))
        row = tk.Frame(body, bg=self.T["canvas"])
        row.pack(anchor="w", pady=(14, 4))
        for w in pc_advice.WORKLOADS:
            tk.Radiobutton(row, text=w + ("  (recommended)" if w == pc_advice.recommend_workload(self.hw) else ""),
                           variable=self.work_var, value=w, command=self.draw_settings, bg=self.T["canvas"],
                           fg=self.T["ink"], selectcolor=self.T["canvas"], activebackground=self.T["canvas"],
                           font=ui.F(11)).pack(anchor="w")
        self.set_var = tk.StringVar()
        self.label(body, var=self.set_var, size=10, muted=True).pack(anchor="w", pady=(8, 0))
        self.draw_settings()

    def draw_settings(self):
        s = pc_advice.settings_for(self.hw, self.work_var.get())
        self.set_var.set(f"{s['fps']} fps  ·  resolution {s['res_scale']}  ·  DLAA {s['dlaa']}  ·  shadows "
                         f"{s['shadow_quality']}  ·  tree / detail distance {s['draw_distance']}  ·  simulation "
                         f"{s['npc_distance']}  ·  texture filtering {s['anisotropy']}")

    def save_settings(self):
        if self.hw is None or not hasattr(self, "work_var"):
            return
        st = L._load_settings()
        st.update(pc_advice.settings_for(self.hw, self.work_var.get()))
        st["cpu_mode"], st["cpu_cores"] = "multi", "auto"
        L._save_settings(st)

    # ---- 5. build
    def page_build(self):
        self.save_settings()
        self.title("Build", "LodeStar now translates the game's code and compiles it on your PC - once. Depending on "
                            "the processor this takes 10 to 30 minutes. You can also do it later from the launcher.")
        ok_files, _t = L.game_files_check(ROOT)
        body = self.card()
        self.build_head = tk.StringVar(value="Ready to build" if ok_files else "Import your game files first (step 3)")
        self.label(body, var=self.build_head, size=14, weight=700).pack(anchor="w")
        self.bar = tk.Canvas(body, height=18, bg=self.T["canvas"], highlightthickness=0)
        self.bar.pack(fill="x", pady=(14, 6))
        self.bar_info = tk.StringVar(value="")
        self.label(body, var=self.bar_info, size=11, weight=600).pack(anchor="w")
        self.bar_line = tk.StringVar(value="")
        self.label(body, var=self.bar_line, size=9, muted=True).pack(anchor="w", pady=(4, 0))
        row = tk.Frame(body, bg=self.T["canvas"])
        row.pack(anchor="w", pady=(16, 0))
        self.btn_build = ui.PillButton(row, "Build now", self.build_now, kind="primary", bg=self.T["canvas"])
        self.btn_build.pack(side="left")
        ui.PillButton(row, "Later, in the launcher", self.next, kind="secondary", bg=self.T["canvas"]).pack(side="left", padx=10)
        if not ok_files:
            self.btn_build.configure(state="disabled")
        self.btn_next.configure(text="Skip")
        self.draw_bar(0)

    def draw_bar(self, frac: float):
        if not hasattr(self, "bar") or not self.bar.winfo_exists():
            return
        self.bar.update_idletasks()
        w = max(10, self.bar.winfo_width())
        self.bar.delete("all")
        self.bar.create_rectangle(0, 0, w, 18, fill=self.T["hairline"], width=0)
        self.bar.create_rectangle(0, 0, int(w * max(0.0, min(1.0, frac))), 18, fill=self.T["primary"], width=0)

    def build_now(self):
        if self.busy:
            return
        ok_files, _t = L.game_files_check(ROOT)
        if not ok_files:
            messagebox.showinfo("Build", "Import your game files first (step 3).", parent=self.root)
            return
        rows = check_pc()
        hard = [r["name"] for r in rows if not r["ok"] and not r["fix"]]
        if hard:
            messagebox.showerror("Build", "This PC cannot run LodeStar: " + ", ".join(hard) + ".", parent=self.root)
            return
        self.busy = True
        self.build_t0 = time.time()
        py = sys.executable.replace("pythonw.exe", "python.exe")
        steps = [(f"Installing {r['name']} (Windows may ask for permission)", winget_cmd(r["fix"]), 0.08, None)
                 for r in rows if not r["ok"] and r["fix"]]
        if not (SYSTEM / "build_work" / "xlate" / "sw_xlate.dll").is_file():
            steps.append(("Shader translator", [py, "-u", str(SYSTEM / "Engine" / "Tools" / "build_xlate.py")], 0.05, None))
        exefs = L._resolve_exefs()
        rec = [py, "-u", str(SYSTEM / "Engine" / "swrecomp.py"), "recompile", str(exefs)]
        for m in L.MODULES:
            rec += ["--module", m]
        rec += ["--max-functions", "10000000", "--out", str(SYSTEM / "build_work")]
        steps.append(("Translating the game's code", rec, 0.30, L.RecompileProgress))
        steps.append(("Compiling", [py, "-u", str(SYSTEM / "build_work" / "build.py")], 0.65, L.BuildProgress))
        total_w = sum(s[2] for s in steps)
        self.build_steps = [(n, c, w / total_w, p) for n, c, w, p in steps]
        self.build_i = -1
        self.build_base = 0.0
        self.tick()
        self.start_next_step()

    def start_next_step(self):
        if self.build_i >= 0:
            self.build_base += self.build_steps[self.build_i][2]
        self.build_i += 1
        if self.build_i >= len(self.build_steps):
            self.busy = False
            self.build_head.set("✓  Built - ready to play")
            self.bar_info.set(f"Done in {fmt_time(time.time() - self.build_t0)}")
            self.draw_bar(1.0)
            self.btn_next.configure(text="Next")
            return
        name, cmd, _w, prog = self.build_steps[self.build_i]
        self.build_name = name
        self.build_prog = prog() if prog else None
        self.build_head.set(f"Step {self.build_i + 1} of {len(self.build_steps)}: {name}")
        log = SYSTEM / "build_work" / "out" / "logs" / f"setup_{self.build_i}.log"
        try:
            self.task = L.Task(name, cmd, SYSTEM, None, self.q, log)
        except OSError as e:
            self.busy = False
            self.build_head.set(f"Could not start: {e}")

    def build_fraction(self) -> float:
        if self.build_i < 0 or self.build_i >= len(self.build_steps):
            return 1.0 if self.build_i >= 0 else 0.0
        w = self.build_steps[self.build_i][2]
        f = self.build_prog.fraction if self.build_prog else 0.5
        return self.build_base + w * max(0.0, min(1.0, f))

    def tick(self):
        if not self.busy or not hasattr(self, "bar_info"):
            return
        el = time.time() - self.build_t0
        frac = self.build_fraction()
        left = el / frac - el if frac > 0.03 else None
        self.bar_info.set(f"{frac * 100:.0f} %   ·   {fmt_time(el)} so far" +
                          (f"   ·   about {fmt_time(left)} left" if left is not None else "   ·   estimating time left..."))
        self.draw_bar(frac)
        self.root.after(500, self.tick)

    # ---- 6. done
    def page_done(self):
        built = (SYSTEM / "build_work" / "out" / L.GAME_EXE).is_file()
        self.title("All set", "LodeStar is installed. Start it from the Start menu or the desktop shortcut.")
        body = self.card()
        self.label(body, "✓  The game is built - press Play in the launcher." if built else
                   "Build the game in the launcher: Game files -> Build -> Play.", size=13, weight=600).pack(anchor="w")
        self.label(body, "In the game: F1 settings menu  ·  F2 mouse camera  ·  F10 performance overlay  ·  F11 fullscreen",
                   size=10, muted=True).pack(anchor="w", pady=(8, 0))
        self.btn_back.configure(text="Back")
        self.btn_next.configure(text="Open LodeStar")

    def finish(self):
        exe = ROOT / "LodeStar.exe"
        try:
            if exe.is_file():
                subprocess.Popen([str(exe)], cwd=str(ROOT))
            else:
                subprocess.Popen([sys.executable, str(SYSTEM / "launcher" / "launcher.py")], cwd=str(ROOT))
        except OSError:
            pass
        self.root.destroy()

    # ---- background messages
    def pump(self):
        try:
            while True:
                kind, data = self.q.get_nowait()
                if kind == "pc":
                    self.draw_pc(data)
                elif kind == "pcmsg":
                    if self.step == 0:
                        self.pc_status.set(data)
                elif kind == "pcdone":
                    self.busy = False
                    self.draw_pc(data)
                elif kind == "files":
                    self.busy = False
                    if data:
                        messagebox.showerror("Import game files", f"Copying failed: {data}", parent=self.root)
                    if self.step == 2:
                        self.refresh_files()
                elif kind == "line":
                    if self.build_prog is not None and self.build_i >= 0:
                        self.build_prog.feed(data)
                    if self.step == 3 and data.strip():
                        self.bar_line.set(data.strip()[:110])
                elif kind == "done":
                    t = self.task
                    if t and t.returncode not in (0, None) and not t.stopped:
                        self.busy = False
                        self.build_head.set(f"{self.build_name} failed (exit {t.returncode})")
                        self.bar_info.set(f"The log is in {t.log_path}. You can retry, or build later in the launcher.")
                        self.btn_build.configure(text="Retry")
                    elif t and not t.stopped:
                        self.start_next_step()
        except queue.Empty:
            pass
        self.root.after(100, self.pump)


def main() -> int:
    if sys.platform == "win32":
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("LodeStar.Setup")
        except (AttributeError, OSError):
            pass
    root = tk.Tk()
    Wizard(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
