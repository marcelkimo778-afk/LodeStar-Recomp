"""Mods window: the mods the game finds (sw_app.exe --list-mods), on/off, priority order, cheats, mod folders.
Writes Settings/mods.json (mods_cfg.py, format in MODS.md).  Changes apply the next time the game starts."""
from __future__ import annotations

import ui_kit
import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import mods_cfg
import modbrowser_cli as mbc


def _open(path: Path) -> None:
    try:
        if sys.platform == "win32":
            os.startfile(str(path))  # noqa: S606 - opens a folder / log in Explorer
        else:
            subprocess.Popen(["xdg-open", str(path)])
    except OSError as e:
        messagebox.showerror("Mods", f"Cannot open {path}:\n{e}")


class ModsWindow:
    """The mod list.  parent=None: its own window; parent=<frame>: built inside it (the launcher's Mods tab)."""

    def __init__(self, root: tk.Misc, root_dir: Path, exe: Path, colors: dict, parent: tk.Misc | None = None,
                 cli: "mbc.Cli | None" = None):
        self.c = colors
        self.cli = cli                                  # mb_cli.exe: uninstall / updates of browser-installed mods
        self.updates: dict[str, str] = {}               # folder -> newer version
        self.embedded = parent is not None
        self.root_dir = Path(root_dir)
        self.exe = Path(exe)
        self.cfg_path = self.root_dir / "Settings" / "mods.json"
        self.mods_dir = self.root_dir / "Mods"
        self.cfg = mods_cfg.load(self.cfg_path)
        self.rows: list[dict] = []
        self.listing: dict = {}
        self._results: queue.Queue = queue.Queue()     # worker thread -> UI thread (Tk is not thread-safe)

        if self.embedded:
            w = self.win = parent
        else:
            w = self.win = tk.Toplevel(root)
            w.title("Mods")
            w.configure(bg=colors["card"])
            w.transient(root)
            w.geometry("980x640")
            w.minsize(860, 520)
        f = ttk.Frame(w, style="Card.TFrame", padding=12)
        f.pack(fill="both", expand=True)
        ttk.Label(f, text="Mods", style="Head.TLabel").pack(anchor="w")
        ttk.Label(f, text=(f"Put mods into {self.mods_dir} - any emulator layout works (yuzu / Eden load/, Ryujinx "
                           "mods/contents/, Atmosphere contents/, or one folder per mod with romfs/ exefs/ cheats/). "
                           "First in the list = highest priority. Code (exefs) patches cannot work in the recompiled "
                           "game: they are reported in the log and skipped. See MODS.md."),
                  style="Muted.TLabel", wraplength=930, justify="left").pack(anchor="w", pady=(2, 8))

        opts = ttk.Frame(f, style="Card.TFrame")
        opts.pack(fill="x")
        self.var_on = tk.BooleanVar(value=bool(self.cfg.get("enabled", True)))
        self.var_emu = tk.BooleanVar(value=bool(self.cfg.get("search_emulator_dirs", False)))
        ttk.Checkbutton(opts, text="Load mods", variable=self.var_on).pack(side="left")
        ttk.Checkbutton(opts, text="Also use the mod folders of installed emulators (yuzu, Eden, suyu, Ryujinx ...)",
                        variable=self.var_emu, command=self.refresh).pack(side="left", padx=(16, 0))
        extra = ttk.Frame(f, style="Card.TFrame")
        extra.pack(fill="x", pady=(6, 0))
        ttk.Label(extra, text="Extra mods folder:").pack(side="left")
        dirs = self.cfg.get("mods_dirs") or []
        self.var_dir = tk.StringVar(value=dirs[0] if isinstance(dirs, list) and dirs else str(dirs or ""))
        ttk.Entry(extra, textvariable=self.var_dir, width=70).pack(side="left", padx=6, fill="x", expand=True)
        ttk.Button(extra, text="Browse...", command=self._browse).pack(side="left")

        cols = ("prio", "on", "name", "what", "notes", "layout")
        # bottom rows first (side=bottom): they always stay visible, the list gets what is left
        bottom = ttk.Frame(f, style="Card.TFrame")
        bottom.pack(side="bottom", fill="x", pady=(8, 0))
        ttk.Label(bottom, text="Changes apply the next time the game starts.", style="Muted.TLabel").pack(side="left")
        if not self.embedded:
            ttk.Button(bottom, text="Close", command=self.close).pack(side="right")
            w.protocol("WM_DELETE_WINDOW", self.close)
        ttk.Button(bottom, text="Save", command=self.save).pack(side="right", padx=(0, 6))
        ttk.Button(bottom, text="Mod log", command=self._open_log).pack(side="right", padx=(0, 6))
        self.status = tk.StringVar(value="")
        ttk.Label(f, textvariable=self.status, style="Muted.TLabel", wraplength=930,
                  justify="left").pack(side="bottom", anchor="w", pady=(8, 0))
        btns = ttk.Frame(f, style="Card.TFrame")
        btns.pack(side="bottom", fill="x", pady=(8, 0))
        holder = ttk.Frame(f, style="Card.TFrame")
        holder.pack(fill="both", expand=True, pady=(8, 0))
        self.tree = ttk.Treeview(holder, columns=cols, show="headings", height=5, selectmode="browse")
        for col, title, width in (("prio", "#", 36), ("on", "On", 46), ("name", "Mod", 230), ("what", "Contents", 200),
                                  ("notes", "Notes", 260), ("layout", "Found as", 220)):
            self.tree.heading(col, text=title)
            self.tree.column(col, width=width, anchor="w", stretch=col in ("name", "layout"))
        sb = ttk.Scrollbar(holder, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="left", fill="y")
        self.tree.bind("<Double-1>", lambda _e: self.toggle())
        self.tree.bind("<space>", lambda _e: self.toggle())

        for text, cmd in (("On / off", self.toggle), ("Move up", lambda: self.move(-1)),
                          ("Move down", lambda: self.move(1)), ("Cheats...", self.cheats), ("Refresh", self.refresh)):
            ttk.Button(btns, text=text, command=cmd).pack(side="left", padx=(0, 6))
        for text, cmd in (("Uninstall", self.uninstall), ("Update", self.update), ("Check updates", self.check_updates)):
            ttk.Button(btns, text=text, command=cmd).pack(side="left", padx=(0, 6))
        ttk.Button(btns, text="Folder", command=self._open_dir).pack(side="right")          # opens the mods folder

        self.refresh()

    # ---- data ----
    def _current_cfg(self) -> dict:
        cfg = mods_cfg.config_from_rows(self.cfg, self.rows)
        cfg["enabled"] = bool(self.var_on.get())
        cfg["search_emulator_dirs"] = bool(self.var_emu.get())
        d = self.var_dir.get().strip()
        cfg["mods_dirs"] = [d] if d else []
        return cfg

    def refresh(self) -> None:
        if not self.exe.is_file():
            self.status.set(f"The game is not built yet ({self.exe.name} missing): build it first to see your mods.")
            return
        cfg = self._current_cfg() if self.rows or self.listing else dict(self.cfg)
        cfg["search_emulator_dirs"] = bool(self.var_emu.get())
        d = self.var_dir.get().strip()
        cfg["mods_dirs"] = [d] if d else []
        tmp = self.cfg_path.with_name("mods.preview.json")       # list with the unsaved settings
        try:
            mods_cfg.save(tmp, cfg)
        except OSError as e:
            self.status.set(f"Cannot write {tmp}: {e}")
            return
        self.status.set("Looking for mods...")

        def work():
            try:
                res = mods_cfg.list_mods(self.exe, tmp)
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as e:
                res = e
            self._results.put(res)
        threading.Thread(target=work, daemon=True).start()
        self.win.after(100, self._poll)

    def _poll(self) -> None:
        try:
            res = self._results.get_nowait()
            if isinstance(res, tuple) and res and res[0] == "updates":
                if isinstance(res[1], Exception):
                    self.status.set(f"Update check: {res[1]}")
                else:
                    self.updates = {u["folder"]: u["latest"] for u in res[1].get("mods", [])}
                    self.status.set(f"{len(self.updates)} update(s) available." if self.updates else "All up to date.")
                    self._fill()
                return
            if isinstance(res, tuple) and res and res[0] == "updated":
                name, out = res[1]
                self.status.set(f"{name}: {out.get('message') or out.get('error') or 'done'}")
                self.updates.clear()
                self.refresh()
                return
        except queue.Empty:
            try:
                self.win.after(100, self._poll)
            except tk.TclError:
                pass                                   # window closed meanwhile
            return
        self._listed(res)

    def _listed(self, res) -> None:
        try:
            self.cfg_path.with_name("mods.preview.json").unlink()
        except OSError:
            pass
        if isinstance(res, Exception):
            self.status.set(f"Could not list the mods: {res}")
            return
        self.listing = res
        self.rows = mods_cfg.rows_from_listing(res)
        self._fill()
        warn = [m for m in res.get("messages", []) if m.startswith("WARNING") or m.startswith("conflict")]
        where = ", ".join(res.get("search", [])) or "(no folder)"
        text = f"{len(self.rows)} mod(s) found in: {where}."
        if res.get("conflicts"):
            text += f" {res['conflicts']} file(s) are in more than one mod: the one higher in the list wins."
        if warn:
            text += f" {len(warn)} warning(s) - details in the mod log after the next start. First: {warn[0][:220]}"
        self.status.set(text)

    def _fill(self, select: int | None = None) -> None:
        self.tree.delete(*self.tree.get_children())
        for i, r in enumerate(self.rows):
            on = "yes" if r["enabled"] else ("marker" if r["marker"] else "no")
            r["update"] = self.updates.get(r.get("path", ""), "")
            self.tree.insert("", "end", iid=str(i), values=(i + 1, on, r["name"], mods_cfg.describe(r), mods_cfg.notes(r),
                                                            r["layout"]))
        if select is not None and 0 <= select < len(self.rows):
            self.tree.selection_set(str(select))
            self.tree.see(str(select))

    def _sel(self) -> int:
        s = self.tree.selection()
        return int(s[0]) if s else -1

    # ---- actions ----
    def toggle(self) -> None:
        i = self._sel()
        if i < 0:
            return
        r = self.rows[i]
        if r["marker"]:
            messagebox.showinfo("Mods", f"{r['name']} is switched off by a .disabled file in its folder:\n{r['path']}\n\n"
                                "Delete that file to use the mod.", parent=self.win)
            return
        r["enabled"] = not r["enabled"]
        self._fill(i)

    def move(self, delta: int) -> None:
        i = self._sel()
        if i >= 0:
            self._fill(mods_cfg.move(self.rows, i, delta))

    def cheats(self) -> None:
        i = self._sel()
        if i < 0:
            return
        r = self.rows[i]
        if not r["cheats"]:
            messagebox.showinfo("Mods", f"{r['name']} has no cheats for this game version.", parent=self.win)
            return
        dlg = tk.Toplevel(self.win)
        dlg.title(f"Cheats - {r['name']}")
        ui_kit.window_icon(dlg, "saves")
        dlg.configure(bg=self.c["card"])
        dlg.transient(self.win)
        fr = ttk.Frame(dlg, style="Card.TFrame", padding=12)
        fr.pack(fill="both", expand=True)
        ttk.Label(fr, text="Cheats to run (Atmosphere format). Cheats that write game code are skipped.",
                  style="Muted.TLabel").pack(anchor="w", pady=(0, 6))
        vars_ = []
        for ch in r["cheats"]:
            v = tk.BooleanVar(value=bool(ch.get("enabled")))
            ttk.Checkbutton(fr, text=ch["name"], variable=v).pack(anchor="w")
            vars_.append((ch, v))

        def ok():
            for ch, v in vars_:
                ch["enabled"] = bool(v.get())
            self._fill(i)
            dlg.destroy()
        ttk.Button(fr, text="OK", command=ok).pack(anchor="e", pady=(8, 0))
        dlg.grab_set()

    # ---- mods the browser installed (they have a mod_meta.json): uninstall / update through mb_cli.exe
    def _meta_row(self, need: str):
        i = self._sel()
        if i < 0:
            return None
        r = self.rows[i]
        if not r.get("meta") or self.cli is None or not self.cli.available():
            messagebox.showinfo("Mods", f"{need} works for mods installed by the mod browser (Mods > Browse). "
                                        "Other mods: delete or replace their folder yourself.", parent=self.win)
            return None
        return r

    def uninstall(self) -> None:
        r = self._meta_row("Uninstall")
        if not r or not messagebox.askyesno("Uninstall", f"Remove \"{r['name']}\"?\n\nOnly its folder is deleted:\n{r['path']}",
                                            parent=self.win):
            return
        try:
            self.cli.call("uninstall", folder=r["path"])
            self.status.set(f"{r['name']}: removed.")
        except mbc.CliError as e:
            self.status.set(f"{r['name']}: {e}")
        self.refresh()

    def check_updates(self) -> None:
        if self.cli is None or not self.cli.available():
            return
        self.status.set("Checking for updates...")

        def work():
            try:
                res = self.cli.call("check-updates", timeout=300)
            except mbc.CliError as e:
                res = e
            self._results.put(("updates", res))
        threading.Thread(target=work, daemon=True).start()
        self.win.after(100, self._poll)

    def update(self) -> None:
        r = self._meta_row("Update")
        if not r:
            return
        if not self.updates.get(r["path"]):
            self.status.set(f"{r['name']}: no update known - click Check updates first.")
            return
        m = r["meta"]
        self.status.set(f"Updating {r['name']}...")

        def work():
            try:
                proc = self.cli.start_install(source=m.get("source"), mod_id=m.get("mod_id"))
                last = {}
                for msg in mbc.read_lines(proc):
                    last = msg
                res = last
            except OSError as e:
                res = {"type": "error", "error": str(e)}
            self._results.put(("updated", (r["name"], res)))
        threading.Thread(target=work, daemon=True).start()
        self.win.after(100, self._poll)

    def save(self) -> bool:
        try:
            mods_cfg.save(self.cfg_path, self._current_cfg())
        except OSError as e:
            messagebox.showerror("Mods", f"Cannot save {self.cfg_path}:\n{e}", parent=self.win)
            return False
        self.cfg = mods_cfg.load(self.cfg_path)
        self.status.set(f"Saved {self.cfg_path}.")
        return True

    def close(self) -> None:
        if self.rows or self.listing:
            new = self._current_cfg()
            if new != mods_cfg.config_from_rows(self.cfg, mods_cfg.rows_from_listing(self.listing)) or \
                    new.get("enabled") != self.cfg.get("enabled") or \
                    new.get("search_emulator_dirs") != self.cfg.get("search_emulator_dirs") or \
                    new.get("mods_dirs") != (self.cfg.get("mods_dirs") or []):
                a = messagebox.askyesnocancel("Mods", "Save the changes?", parent=self.win)
                if a is None or (a and not self.save()):
                    return
        self.win.destroy()

    def _browse(self) -> None:
        d = filedialog.askdirectory(parent=self.win, title="Mods folder (e.g. yuzu's load folder)")
        if d:
            self.var_dir.set(d)
            self.refresh()

    def _open_dir(self) -> None:
        self.mods_dir.mkdir(exist_ok=True)
        _open(self.mods_dir)

    def _open_log(self) -> None:
        log = self.exe.parent / "mods.log"
        if log.is_file():
            _open(log)
        else:
            messagebox.showinfo("Mods", "No mod log yet: it is written when the game starts with mods.", parent=self.win)


class ModsPanel:
    """The launcher's Mods tab.  Built the first time the tab is shown (nothing runs before), and only when the
    game files work: files_check() -> (ok, text) - the same ExeFS / RomFS checks Play uses."""

    def __init__(self, parent: tk.Frame, root_dir: Path, exe: Path, colors: dict, files_check, exefs=None):
        self.parent, self.root_dir, self.exe, self.c, self.files_check = parent, root_dir, exe, colors, files_check
        self.exefs = exefs                               # callable -> the ExeFS folder (the browser's target build id)
        self.view: ModsWindow | None = None
        self.browse = None
        self.nb: ttk.Notebook | None = None
        self.blocker: tk.Frame | None = None

    def show(self) -> None:
        ok, text = self.files_check()
        if not ok:
            if self.view:                                # game files went away: hide the list again
                for ch in self.parent.winfo_children():
                    ch.destroy()
                self.view = None
            self._block(text)
            return
        if self.blocker:
            self.blocker.destroy()
            self.blocker = None
        if self.view is None:
            import modbrowser_ui                         # the Browse page: only loaded when the Mods tab is used
            self.nb = ttk.Notebook(self.parent)
            self.nb.pack(fill="both", expand=True)
            inst = ttk.Frame(self.nb, style="Card.TFrame")
            brw = ttk.Frame(self.nb, style="Card.TFrame")
            self.nb.add(inst, text="  Installed  ")
            self.nb.add(brw, text="  Browse  ")
            out_dir = Path(self.exe).parent
            exefs = self.exefs() if self.exefs else None
            self.browse = modbrowser_ui.BrowsePanel(brw, self.root_dir, out_dir, self.c, exefs=exefs)
            self.view = ModsWindow(self.parent, self.root_dir, self.exe, self.c, parent=inst, cli=self.browse.cli)
            self.nb.bind("<<NotebookTabChanged>>", self._tab_changed)
        else:
            self.view.refresh()

    def _tab_changed(self, _e=None) -> None:
        if self.nb is None:
            return
        if self.nb.index("current") == 1:
            self.browse.show()                           # nothing runs before the Browse page is opened
        else:
            self.view.refresh()                          # what the browser installed shows up here

    def _block(self, text: str) -> None:
        if self.blocker:
            self.blocker.destroy()
        b = self.blocker = ttk.Frame(self.parent, style="Card.TFrame", padding=24)
        b.pack(fill="both", expand=True)
        ttk.Label(b, text="Mods need working game files", style="Head.TLabel").pack(anchor="w")
        ttk.Label(b, text=text + "\n\nFix the ExeFS / RomFS folders in Overview (Game files), then come back here.",
                  style="Muted.TLabel", wraplength=860, justify="left").pack(anchor="w", pady=(6, 12))
        ttk.Button(b, text="Check again", command=self.show).pack(anchor="w")
