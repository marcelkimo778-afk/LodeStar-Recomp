"""Save editor window: numbers (money...), story/unlock toggles, team editor, items.

Opens build_work/out/save/main.  Every write backs the save up first
(save_tools.backup) and re-parses the result before replacing the file.
"""
from __future__ import annotations

import random
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

import pla_data
import pla_edit
import pla_save
import save_tools
import ui_kit

# (label, key, max, hint) - u32 values unless the block says otherwise
NUMBERS = [
    ("Money (Pokédollars)", 0x3279D927, 9_999_999, "max 9,999,999"),
    ("Merit points (current)", 0x9D5D1CA5, 999_999, "spend at the Ancient Retreat"),
    ("Merit points (earned total)", 0xC25B0D5A, 999_999, ""),
    ("Satchel upgrades", 0x75CE2CF6, 39, "0-39 (more bag slots)"),
    ("Galaxy Team rank", 0x50FE632A, 10, "0-10 (star rank)"),
    ("Unown found", 0x3EBEE1A7, 28, "0-28"),
    ("Wisps found (total)", 0xB79EF1FE, 107, "0-107"),
    ("Wisps reported to Vessa", 0x8F0D8720, 107, "0-107"),
    ("Starter choice", 0x6960C6EF, 2, "0 Rowlet, 1 Cyndaquil, 2 Oshawott"),
    ("Text speed", 0x92EB0306, 3, "0 slow .. 3 instant"),
]

# (group, label, key)
TOGGLES = [
    ("Areas", "Obsidian Fieldlands open", 0x24C0252D),
    ("Areas", "Crimson Mirelands open", 0x1599C206),
    ("Areas", "Cobalt Coastlands open", 0x408DE1D3),
    ("Areas", "Coronet Highlands open", 0x8C062C9C),
    ("Areas", "Alabaster Icelands open", 0xC08D4C69),
    ("Areas", "Area 6 open", 0x76350E52),
    ("Areas", "Fast travel", 0xFE98F73F),
    ("Areas", "Space-time distortions", 0x7611BFC3),
    ("Ride Pokémon", "Wyrdeer", 0x47365FE8),
    ("Ride Pokémon", "Ursaluna", 0x4736634E),
    ("Ride Pokémon", "Basculegion", 0x473666B4),
    ("Ride Pokémon", "Sneasler", 0x47366A1A),
    ("Ride Pokémon", "Braviary", 0x47334812),
    ("Story (missions done)", "Lord Kleavor calmed", 0x96774421),
    ("Story (missions done)", "Lady Lilligant calmed", 0x3A50000C),
    ("Story (missions done)", "Lord Arcanine calmed", 0xA5981A37),
    ("Story (missions done)", "Lord Electrode calmed", 0x6EF3C712),
    ("Story (missions done)", "Lord Avalugg calmed", 0x424E9F0D),
    ("Story (missions done)", "Origin Dialga defeated", 0x5185ADC0),
    ("Story (missions done)", "Origin Palkia defeated", 0x5E5BFD94),
    ("Story (missions done)", "Arceus defeated (post-game)", 0x2F91EFD3),
    ("Story (missions done)", "Pokédex completed (Azure Flute)", 0xD985E1C2),
    ("Story (missions done)", "Pokédex perfected", 0x98ED661E),
    ("Story (missions done)", "Chose Adaman (Diamond Clan)", 0x669B325F),
    ("Story (missions done)", "Got the other starters", 0x8D45EB90),
    ("Features", "Unown notes", 0xC9127B4E),
    ("Features", "Lost & found satchels", 0xFE837926),
    ("Features", "Mass release in boxes", 0x0C16BEF4),
    ("Features", "Secret box 1", 0xF224CA8E),
    ("Features", "Secret box 2", 0x06924515),
    ("Features", "Secret box 3", 0xF67C6DC8),
    ("Rotom forms", "Fan (Fan Rotom)", 0xC734C80F),
    ("Rotom forms", "Washing machine (Wash Rotom)", 0x62872639),
    ("Rotom forms", "Lawn mower (Mow Rotom)", 0xFB87E941),
    ("Rotom forms", "Microwave (Heat Rotom)", 0xD9A315A0),
    ("Rotom forms", "Refrigerator (Frost Rotom)", 0xAE868040),
    ("Legendary spawns", "Spiritomb", 0x2DC7E4CC),
    ("Legendary spawns", "Uxie", 0x9EC1F2C4),
    ("Legendary spawns", "Mesprit", 0xEF5C95D8),
    ("Legendary spawns", "Azelf", 0xD038BD89),
    ("Legendary spawns", "Heatran", 0x3F6301AC),
    ("Legendary spawns", "Cresselia", 0x85134D02),
    ("Legendary spawns", "Darkrai", 0xEE027506),
    ("Legendary spawns", "Shaymin", 0x0DCE6659),
    ("Legendary spawns", "Tornadus", 0x07D8EC38),
    ("Legendary spawns", "Thundurus", 0x136D3D88),
    ("Legendary spawns", "Landorus", 0xE079071B),
    ("Legendary spawns", "Enamorus", 0x3AA64045),
]


def save_main(work: Path) -> Path:
    return save_tools.save_dir(work) / "main"


class SaveEditor:
    def __init__(self, parent: tk.Misc, colors: dict, root_dir: Path, busy) -> None:
        self.root_dir, self.work, self.busy = root_dir, root_dir / "System" / "build_work", busy
        self.c = colors
        path = save_main(self.work)
        if not path.exists():
            messagebox.showinfo("Save editor", f"No save yet ({path}).\nPlay until the first autosave, "
                                "or import a save in 'Saves & cheats'.", parent=parent)
            return
        bin_dir = pla_data.romfs_bin(root_dir)
        if bin_dir is None:
            messagebox.showerror("Save editor", "The game's RomFS (roms_full/romfs) is needed for "
                                 "Pokémon names and stats. Import your game files first (Game files card).", parent=parent)
            return
        try:
            keys = pla_save.keys_from_game(root_dir / "Game" / "exefs", root_dir / "System" / "Build" / "Cache" / "modules",
                                           sample=path)
            self.gd = pla_data.load(bin_dir)
            self.sf = pla_edit.SaveFile(path, keys)
        except Exception as e:           # noqa: BLE001 - show any load problem to the user
            messagebox.showerror("Save editor", f"Could not open the save:\n{e}", parent=parent)
            return
        self.party = self.sf.party()
        self.dirty = False

        win = self.win = tk.Toplevel(parent)
        win.title("Save editor & cheats")
        ui_kit.window_icon(win, "saves")
        win.configure(bg=self.c["card"])
        win.geometry("900x680")
        win.transient(parent)
        top = ttk.Frame(win, style="Card.TFrame", padding=(12, 10, 12, 0))
        top.pack(fill="x")
        ttk.Label(top, text="Save editor & cheats", style="Head.TLabel").pack(side="left")
        self.state_lbl = ttk.Label(top, text=f"{path}", style="Muted.TLabel")
        self.state_lbl.pack(side="left", padx=12)
        nb = ttk.Notebook(win)
        nb.pack(fill="both", expand=True, padx=12, pady=8)
        self._tab_numbers(nb)
        self._tab_toggles(nb)
        self._tab_team(nb)
        self._tab_items(nb)
        bottom = ttk.Frame(win, style="Card.TFrame", padding=(12, 0, 12, 10))
        bottom.pack(fill="x")
        ttk.Label(bottom, text="A backup is made before every save (Saves & cheats > Restore).",
                  style="Muted.TLabel").pack(side="left")
        ttk.Button(bottom, text="Close", command=self.close).pack(side="right")
        ttk.Button(bottom, text="💾  Save changes", command=self.write).pack(side="right", padx=6)
        win.protocol("WM_DELETE_WINDOW", self.close)

    # -- helpers ---------------------------------------------------------------
    def _frame(self, nb, title) -> ttk.Frame:
        outer = ttk.Frame(nb, style="Card.TFrame", padding=10)
        nb.add(outer, text=title)
        return outer

    def _scroll(self, parent) -> ttk.Frame:
        canvas = tk.Canvas(parent, bg=self.c["card"], highlightthickness=0)
        sb = ttk.Scrollbar(parent, orient="vertical", command=canvas.yview)
        inner = ttk.Frame(canvas, style="Card.TFrame")
        inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=sb.set)
        canvas.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        canvas.bind_all("<MouseWheel>", lambda e: canvas.yview_scroll(int(-e.delta / 120), "units")
                        if canvas.winfo_exists() else None)
        return inner

    def _changed(self) -> None:
        self.dirty = True
        self.state_lbl.configure(text="unsaved changes")

    # -- numbers ---------------------------------------------------------------
    def _tab_numbers(self, nb) -> None:
        f = self._frame(nb, "Money & numbers")
        self.num_vars = {}
        for r, (label, key, mx, hint) in enumerate(NUMBERS):
            if not self.sf.has(key):
                continue
            ttk.Label(f, text=label, style="Card.TLabel").grid(row=r, column=0, sticky="w", pady=3)
            v = tk.StringVar(value=str(self.sf.get(key)))
            self.num_vars[key] = (v, mx)
            ttk.Entry(f, textvariable=v, width=14).grid(row=r, column=1, padx=8)
            ttk.Button(f, text="Max", width=6,
                       command=lambda v=v, mx=mx: (v.set(str(mx)), self._changed())).grid(row=r, column=2)
            ttk.Label(f, text=hint, style="Muted.TLabel").grid(row=r, column=3, sticky="w", padx=8)
            v.trace_add("write", lambda *_: self._changed())

    def _apply_numbers(self) -> str | None:
        for key, (v, mx) in self.num_vars.items():
            try:
                n = int(v.get().replace(",", "").replace(".", "").strip())
            except ValueError:
                return f"'{v.get()}' is not a number"
            n = max(0, min(mx, n))
            b = self.sf.by_key[key]
            if b.type == 8:
                n = min(n, 255)
            self.sf.set(key, n)
        return None

    # -- toggles ---------------------------------------------------------------
    def _tab_toggles(self, nb) -> None:
        f = self._frame(nb, "Story & unlocks")
        bar = ttk.Frame(f, style="Card.TFrame")
        bar.pack(fill="x")
        ttk.Label(bar, text="Ticked = done / unlocked. Skipping story steps can confuse the game's "
                  "scripts - keep a backup.", style="Muted.TLabel").pack(side="left")
        inner = self._scroll(f)
        self.tog_vars = {}
        groups: dict[str, list] = {}
        for g, label, key in TOGGLES:
            groups.setdefault(g, []).append((label, key))
        row = 0
        for g, items in groups.items():
            ttk.Label(inner, text=g, style="Head.TLabel").grid(row=row, column=0, sticky="w", pady=(10, 2))
            ttk.Button(inner, text="All on", width=8,
                       command=lambda it=items: self._set_group(it, True)).grid(row=row, column=1, sticky="w")
            ttk.Button(inner, text="All off", width=8,
                       command=lambda it=items: self._set_group(it, False)).grid(row=row, column=2, sticky="w")
            row += 1
            for label, key in items:
                b = self.sf.by_key.get(key)
                if b is None or b.type not in (1, 2):
                    continue
                v = tk.BooleanVar(value=bool(b.value))
                v.trace_add("write", lambda *_: self._changed())
                self.tog_vars[key] = v
                ttk.Checkbutton(inner, text=label, variable=v).grid(row=row, column=0, columnspan=3,
                                                                    sticky="w", padx=14)
                row += 1

    def _set_group(self, items, on: bool) -> None:
        for _label, key in items:
            if key in self.tog_vars:
                self.tog_vars[key].set(on)

    def _apply_toggles(self) -> None:
        for key, v in self.tog_vars.items():
            self.sf.set(key, bool(v.get()))

    # -- team ------------------------------------------------------------------
    def _tab_team(self, nb) -> None:
        f = self._frame(nb, "Team")
        left = ttk.Frame(f, style="Card.TFrame")
        left.pack(side="left", fill="y")
        self.team_list = tk.Listbox(left, width=30, height=8, bg=self.c["bg"], fg=self.c["text"],
                                    selectbackground=self.c["accent"], highlightthickness=0,
                                    font=("Segoe UI", 11), activestyle="none", exportselection=False)
        self.team_list.pack(fill="y")
        self._selected_slot = 0
        self.team_list.bind("<<ListboxSelect>>", self._on_slot_select)
        right = ttk.Frame(f, style="Card.TFrame", padding=(14, 0))
        right.pack(side="left", fill="both", expand=True)
        self.species = self.gd.present_species()
        labels = [f"{s[2]}  (#{s[0]})" for s in self.species]
        self.sp_var = tk.StringVar()
        self.lvl_var = tk.StringVar(value="50")
        self.shiny_var, self.alpha_var = tk.BooleanVar(), tk.BooleanVar()
        self.nat_var = tk.StringVar()
        self.nick_var = tk.StringVar()

        ttk.Label(right, text="Pokémon (type to search)", style="Card.TLabel").grid(row=0, column=0, sticky="w")
        self.sp_box = ttk.Combobox(right, textvariable=self.sp_var, values=labels, width=34)
        self.sp_box.grid(row=0, column=1, sticky="w", pady=3)
        self.sp_box.bind("<KeyRelease>", lambda e: self._filter_species(labels))
        ttk.Label(right, text="Level (1-100)", style="Card.TLabel").grid(row=1, column=0, sticky="w")
        ttk.Spinbox(right, from_=1, to=100, textvariable=self.lvl_var, width=6).grid(row=1, column=1, sticky="w", pady=3)
        ttk.Label(right, text="Nickname", style="Card.TLabel").grid(row=2, column=0, sticky="w")
        ttk.Entry(right, textvariable=self.nick_var, width=16).grid(row=2, column=1, sticky="w", pady=3)
        ttk.Label(right, text="Nature", style="Card.TLabel").grid(row=3, column=0, sticky="w")
        ttk.Combobox(right, textvariable=self.nat_var, values=self.gd.nature_names[:25], width=14,
                     state="readonly").grid(row=3, column=1, sticky="w", pady=3)
        ttk.Checkbutton(right, text="Shiny", variable=self.shiny_var).grid(row=4, column=1, sticky="w")
        ttk.Checkbutton(right, text="Alpha", variable=self.alpha_var).grid(row=5, column=1, sticky="w")
        btns = ttk.Frame(right, style="Card.TFrame")
        btns.grid(row=6, column=0, columnspan=2, sticky="w", pady=(10, 4))
        ttk.Button(btns, text="Put this Pokémon in the slot", command=self._replace_slot).pack(side="left")
        ttk.Button(btns, text="Apply level/nature/shiny/alpha", command=self._edit_slot).pack(side="left", padx=6)
        btns2 = ttk.Frame(right, style="Card.TFrame")
        btns2.grid(row=7, column=0, columnspan=2, sticky="w")
        ttk.Button(btns2, text="Perfect IVs + max effort levels", command=self._max_slot).pack(side="left")
        ttk.Button(btns2, text="Heal team", command=self._heal).pack(side="left", padx=6)
        self.info = ttk.Label(right, text="", style="Muted.TLabel", justify="left", wraplength=480)
        self.info.grid(row=8, column=0, columnspan=2, sticky="w", pady=(12, 0))
        ttk.Label(right, text="New Pokémon get the moves they learn up to their level, a random nature, "
                  "and your trainer data (OT, ball) from the slot they replace.\nEmpty slots copy your "
                  "first Pokémon's trainer data.", style="Muted.TLabel", justify="left",
                  wraplength=480).grid(row=9, column=0, columnspan=2, sticky="w", pady=(12, 0))
        self._refresh_team()

    def _filter_species(self, labels) -> None:
        t = self.sp_var.get().lower()
        self.sp_box.configure(values=[x for x in labels if t in x.lower()] if t else labels)

    def _on_slot_select(self, event=None) -> None:
        sel = self.team_list.curselection()
        if sel:
            self._selected_slot = sel[0]
        self._show_slot()

    def _refresh_team(self) -> None:
        idx = self._selected_slot
        self.team_list.delete(0, "end")
        for i, m in enumerate(self.party):
            if m.is_empty:
                self.team_list.insert("end", f"{i + 1}.  (empty)")
            else:
                tag = (" ★" if m.shiny else "") + (" α" if m.alpha else "")
                self.team_list.insert("end", f"{i + 1}.  {m.nickname}  Lv {m.level}{tag}")
        # clamp index to actual party size
        idx = max(0, min(idx, len(self.party) - 1))
        self._selected_slot = idx
        self.team_list.selection_clear(0, "end")
        self.team_list.selection_set(idx)
        self.team_list.activate(idx)
        self.team_list.see(idx)
        self._show_slot()

    def _slot(self) -> int:
        return self._selected_slot

    def _show_slot(self) -> None:
        m = self.party[self._slot()]
        if m.is_empty:
            self.info.configure(text="Empty slot - pick a Pokémon and press 'Put this Pokémon in the slot'.")
            return
        g = self.gd
        name = g.species_names[m.species] if m.species < len(g.species_names) else str(m.species)
        self.sp_var.set(next((f"{s[2]}  (#{s[0]})" for s in self.species
                              if s[0] == m.species and s[1] == m.form), name))
        self.lvl_var.set(str(m.level))
        self.nick_var.set(m.nickname)
        self.nat_var.set(g.nature_names[m.nature] if m.nature < len(g.nature_names) else "")
        self.shiny_var.set(m.shiny)
        self.alpha_var.set(m.alpha)
        moves = ", ".join(g.move_names[x] for x in m.moves if x and x < len(g.move_names))
        st = m.stats()
        self.info.configure(text=f"{name}  Lv {m.level}   Ability: {g.ability_names[m.ability] if m.ability < len(g.ability_names) else m.ability}\n"
                                 f"Moves: {moves}\nStats HP/Atk/Def/Spe/SpA/SpD: {'/'.join(map(str, st))}\n"
                                 f"IVs: {'/'.join(map(str, m.ivs))}   Effort levels: {'/'.join(map(str, m.gvs))}")

    def _picked(self):
        t = self.sp_var.get()
        for s in self.species:
            if f"{s[2]}  (#{s[0]})" == t:
                return s
        matches = [s for s in self.species if t.lower() and t.lower() in s[2].lower()]
        return matches[0] if len(matches) == 1 else None

    def _level(self) -> int:
        try:
            return max(1, min(100, int(self.lvl_var.get())))
        except ValueError:
            return 50

    def _replace_slot(self) -> None:
        s = self._picked()
        if s is None:
            messagebox.showinfo("Team", "Pick a Pokémon from the list first.", parent=self.win)
            return
        i = self._slot()
        m = self.party[i]
        if m.is_empty:                    # copy trainer data from the first real Pokémon
            src = next((x for x in self.party if not x.is_empty), None)
            if src is None:
                messagebox.showinfo("Team", "Your team is empty - catch one Pokémon in the game first.",
                                    parent=self.win)
                return
            m = pla_edit.Pokemon(bytearray(src.d))
            self.party[i] = m
        m.make_species(self.gd, s[0], s[1], self._level(), random.Random())
        self._apply_slot_opts(m, nickname=False)
        self._changed()
        self._refresh_team()

    def _apply_slot_opts(self, m, nickname=True) -> None:
        if self.nat_var.get() in self.gd.nature_names[:25] and nickname:
            m.nature = self.gd.nature_names.index(self.nat_var.get())
        if nickname and self.nick_var.get().strip():
            new = self.nick_var.get().strip()[:12]
            if new != m.nickname:
                m.nickname = new
                m._w32(0x94, m._u32(0x94) | 0x80000000)    # nicknamed
        m.shiny = self.shiny_var.get()
        m.alpha = self.alpha_var.get()
        m.set_level(self.gd, self._level())

    def _edit_slot(self) -> None:
        m = self.party[self._slot()]
        if m.is_empty:
            return
        self._apply_slot_opts(m)
        self._changed()
        self._refresh_team()

    def _max_slot(self) -> None:
        m = self.party[self._slot()]
        if m.is_empty:
            return
        m.ivs = [31] * 6
        m.gvs = [10] * 6
        m.refresh(self.gd)
        self._changed()
        self._refresh_team()

    def _heal(self) -> None:
        for m in self.party:
            if not m.is_empty:
                m.refresh(self.gd)
        self._changed()
        self._refresh_team()

    # -- items -----------------------------------------------------------------
    def _tab_items(self, nb) -> None:
        f = self._frame(nb, "Items")
        ttk.Label(f, text="Satchel (bag)", style="Head.TLabel").grid(row=0, column=0, sticky="w")
        self.bag_count = tk.StringVar(value="999")
        ttk.Label(f, text="Set the count of every item in the satchel to", style="Card.TLabel").grid(
            row=1, column=0, sticky="w")
        ttk.Spinbox(f, from_=1, to=999, textvariable=self.bag_count, width=6).grid(row=1, column=1, padx=6)
        ttk.Button(f, text="Apply", command=self._bag_all).grid(row=1, column=2)
        ttk.Label(f, text="Add an item to the pasture storage (no satchel space needed)",
                  style="Head.TLabel").grid(row=2, column=0, columnspan=3, sticky="w", pady=(16, 2))
        owned = {it for k in (pla_edit.K_ITEMS, pla_edit.K_STORAGE, 0xF5D9F4A5) if self.sf.has(k)
                 for it, n in self.sf.items(k) if it}
        names = self.gd.item_names
        self.item_labels = sorted({f"{names[i]}  (#{i})" for i in owned if i < len(names) and names[i]})
        self.all_items = sorted({f"{n}  (#{i})" for i, n in enumerate(names) if n and i})
        self.item_var, self.item_n = tk.StringVar(), tk.StringVar(value="999")
        self.item_all = tk.BooleanVar(value=False)
        box = ttk.Combobox(f, textvariable=self.item_var, values=self.item_labels, width=34)
        box.grid(row=3, column=0, sticky="w")
        ttk.Spinbox(f, from_=1, to=999, textvariable=self.item_n, width=6).grid(row=3, column=1, padx=6)
        ttk.Button(f, text="Add", command=self._add_item).grid(row=3, column=2)
        ttk.Checkbutton(f, text="Show every item name (some may not exist in this game)",
                        variable=self.item_all,
                        command=lambda: box.configure(values=self.all_items if self.item_all.get()
                                                      else self.item_labels)).grid(row=4, column=0, sticky="w")
        self.item_info = ttk.Label(f, text="", style="Muted.TLabel")
        self.item_info.grid(row=5, column=0, columnspan=3, sticky="w", pady=(10, 0))

    def _bag_all(self) -> None:
        try:
            n = max(1, min(999, int(self.bag_count.get())))
        except ValueError:
            return
        items = self.sf.items(pla_edit.K_ITEMS)
        self.sf.set_items(pla_edit.K_ITEMS, [(it, n if it else 0) for it, _ in items])
        self.item_info.configure(text=f"{sum(1 for it, _ in items if it)} satchel items set to {n}.")
        self._changed()

    def _add_item(self) -> None:
        t = self.item_var.get()
        if "(#" not in t:
            return
        it = int(t.rsplit("(#", 1)[1].rstrip(")"))
        try:
            n = max(1, min(999, int(self.item_n.get())))
        except ValueError:
            return
        entries = [e for e in self.sf.items(pla_edit.K_STORAGE) if e[0]]
        for j, (x, _c) in enumerate(entries):
            if x == it:
                entries[j] = (it, n)
                break
        else:
            entries.append((it, n))
        self.sf.set_items(pla_edit.K_STORAGE, entries)
        self.item_info.configure(text=f"{t.split('  (#')[0]} x{n} is in the pasture storage.")
        self._changed()

    # -- write -----------------------------------------------------------------
    def write(self) -> None:
        if self.busy():
            return
        err = self._apply_numbers()
        if err:
            messagebox.showerror("Save editor", err, parent=self.win)
            return
        self._apply_toggles()
        self.sf.set_party(self.party)
        try:
            save_tools.backup(self.work, "before_edit")
            self.sf.save()
        except Exception as e:           # noqa: BLE001
            messagebox.showerror("Save editor", f"Not saved: {e}", parent=self.win)
            return
        self.dirty = False
        self.state_lbl.configure(text="saved ✓ (backup made)")

    def close(self) -> None:
        if self.dirty and not messagebox.askyesno("Save editor", "Discard unsaved changes?", parent=self.win):
            return
        self.win.destroy()
