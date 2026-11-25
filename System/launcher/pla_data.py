"""Game data for the save editor, read at run time from the user's own RomFS dump.

Nothing from the game is stored in this file; it only knows the file formats:
  bin/message/<lang>/common/*.dat   Gen 8 text archives (XOR-obfuscated UTF-16)
  bin/pml/personal/personal_data_total.perbin    FlatBuffer: per species/form
  bin/pml/waza_oboe/waza_oboe_total.wazaoboe     FlatBuffer: learnsets
  bin/pml/waza/wazaNNNN.wazabin                  FlatBuffer: one move (PP)
  bin/pml/grow_table/grow_NN.bin                 u32 EXP needed for level 0..100

Field numbers below were checked against a real save (stats, height, weight
and PP of six party Pokemon all reproduce exactly).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path

# personal_data_total field numbers
P_SPECIES, P_FORM, P_PRESENT, P_TYPE1, P_TYPE2 = 0, 1, 2, 3, 4
P_ABIL1, P_ABIL2, P_ABILH = 5, 6, 7
P_HP, P_ATK, P_DEF, P_SPA, P_SPD, P_SPE = 8, 9, 10, 11, 12, 13
P_GENDER, P_GROWTH = 14, 15
P_HEIGHT, P_WEIGHT = 20, 21          # cm, hectograms (u16)
WAZA_PP = 7


def read_text(path: Path) -> list[str]:
    """Lines of a Gen 8 text archive (.dat)."""
    d = path.read_bytes()
    _sections, lines, _total, _key0, soff = struct.unpack_from("<HHIII", d)
    out, key = [], 0x7C89
    for i in range(lines):
        off, ln, _ = struct.unpack_from("<IHH", d, soff + 4 + i * 8)
        o, k, cs = soff + off, key, []
        for j in range(ln):
            c = struct.unpack_from("<H", d, o + 2 * j)[0] ^ k
            k = ((k << 3) | (k >> 13)) & 0xFFFF
            cs.append(c)
        key = (key + 0x2983) & 0xFFFF
        # 0x10 starts a variable/format sequence; drop control chars
        s = []
        skip = 0
        for c in cs:
            if skip:
                skip -= 1
                continue
            if c == 0x10:            # [VAR]: next u16 = length of the rest
                skip = 1
                continue
            if c >= 0x20:
                s.append(chr(c))
        out.append("".join(s).rstrip("\0"))
    return out


class _FB:
    """Just enough FlatBuffers to walk tables and vectors of tables."""

    def __init__(self, data: bytes):
        self.d = data

    def u8(self, o): return self.d[o]
    def u16(self, o): return struct.unpack_from("<H", self.d, o)[0]
    def u32(self, o): return struct.unpack_from("<I", self.d, o)[0]
    def i32(self, o): return struct.unpack_from("<i", self.d, o)[0]

    def root(self) -> int:
        return self.u32(0)

    def fields(self, t: int) -> list[int]:
        vt = t - self.i32(t)
        n = (self.u16(vt) - 4) // 2
        return [self.u16(vt + 4 + 2 * i) for i in range(n)]

    def get(self, t: int, f: list[int], k: int, width: int = 1, default: int = 0) -> int:
        if k >= len(f) or not f[k]:
            return default
        return int.from_bytes(self.d[t + f[k]:t + f[k] + width], "little")

    def vec_tables(self, t: int, f: list[int], k: int) -> list[int]:
        if k >= len(f) or not f[k]:
            return []
        p = t + f[k] + self.u32(t + f[k])
        return [p + 4 + 4 * i + self.u32(p + 4 + 4 * i) for i in range(self.u32(p))]


@dataclass
class Personal:
    species: int
    form: int
    present: bool
    types: tuple[int, int]
    abilities: tuple[int, int, int]
    base: tuple[int, int, int, int, int, int]   # HP, Atk, Def, SpA, SpD, Spe (as stored)
    gender: int                                  # 0 male only, 254 female only, 255 genderless
    growth: int
    height: int
    weight: int


@dataclass
class GameData:
    root: Path
    species_names: list[str]
    move_names: list[str]
    ability_names: list[str]
    nature_names: list[str]
    item_names: list[str]
    personal: dict[tuple[int, int], Personal]
    learnset: dict[tuple[int, int], list[tuple[int, int, int]]]   # (level, mastery level, move)
    exp_tables: list[list[int]]
    _pp: dict[int, int] = field(default_factory=dict)

    def present_species(self) -> list[tuple[int, int, str]]:
        """(species, form, label) of every species/form that exists in Legends: Arceus."""
        out = []
        for (sp, form), p in sorted(self.personal.items()):
            if not p.present or sp == 0:
                continue
            name = self.species_names[sp] if sp < len(self.species_names) else f"#{sp}"
            out.append((sp, form, name if form == 0 else f"{name} (form {form})"))
        return out

    def move_pp(self, move: int) -> int:
        if move not in self._pp:
            pp = 0
            f = self.root / "pml" / "waza" / f"waza{move:04d}.wazabin"
            if move and f.exists():
                fb = _FB(f.read_bytes())
                t = fb.root()
                pp = fb.get(t, fb.fields(t), WAZA_PP)
            self._pp[move] = pp
        return self._pp[move]

    def level_for_exp(self, growth: int, exp: int) -> int:
        tbl = self.exp_tables[growth]
        lvl = 1
        for L in range(1, 101):
            if tbl[L] <= exp:
                lvl = L
        return lvl

    def moves_at_level(self, species: int, form: int, level: int) -> list[int]:
        """The last four moves the species learns at or below level (game's own learnset)."""
        ls = self.learnset.get((species, form)) or self.learnset.get((species, 0)) or []
        got: list[int] = []
        for lv, _mastery, mv in sorted(ls, key=lambda x: x[0]):
            if lv <= level and mv:
                if mv in got:
                    got.remove(mv)
                got.append(mv)
        return got[-4:]


def romfs_bin(project_root: Path) -> Path | None:
    for c in (project_root / "roms_full" / "romfs" / "bin", project_root / "roms" / "romfs" / "bin", project_root / "Game" / "romfs" / "bin"):
        if (c / "pml" / "personal" / "personal_data_total.perbin").exists():
            return c
    return None


@lru_cache(maxsize=2)
def load(bin_dir: Path, lang: str = "English") -> GameData:
    msg = bin_dir / "message" / lang / "common"
    if not msg.is_dir():
        msg = bin_dir / "message" / "English" / "common"
    txt = lambda n: read_text(msg / f"{n}.dat") if (msg / f"{n}.dat").exists() else []

    fb = _FB((bin_dir / "pml" / "personal" / "personal_data_total.perbin").read_bytes())
    r = fb.root()
    personal = {}
    for t in fb.vec_tables(r, fb.fields(r), 0):
        f = fb.fields(t)
        g = lambda k, w=1, dflt=0: fb.get(t, f, k, w, dflt)
        p = Personal(g(P_SPECIES, 2), g(P_FORM, 2), bool(g(P_PRESENT)), (g(P_TYPE1), g(P_TYPE2)),
                     (g(P_ABIL1, 2), g(P_ABIL2, 2), g(P_ABILH, 2)),
                     (g(P_HP), g(P_ATK), g(P_DEF), g(P_SPA), g(P_SPD), g(P_SPE)),
                     g(P_GENDER), g(P_GROWTH), g(P_HEIGHT, 2), g(P_WEIGHT, 2))
        personal.setdefault((p.species, p.form), p)

    fb = _FB((bin_dir / "pml" / "waza_oboe" / "waza_oboe_total.wazaoboe").read_bytes())
    r = fb.root()
    learn = {}
    for t in fb.vec_tables(r, fb.fields(r), 0):
        f = fb.fields(t)
        sp, form = fb.get(t, f, 0, 2), fb.get(t, f, 1, 2)
        moves = []
        for e in fb.vec_tables(t, f, 3):          # Legends learnset: level, mastery level, move
            ef = fb.fields(e)
            moves.append((fb.get(e, ef, 0, 2), fb.get(e, ef, 1, 2), fb.get(e, ef, 2, 2)))
        learn.setdefault((sp, form), moves)

    exp = []
    for i in range(8):
        p = bin_dir / "pml" / "grow_table" / f"grow_{i:02d}.bin"
        exp.append(list(struct.unpack_from("<101I", p.read_bytes())) if p.exists() else [0] * 101)

    return GameData(bin_dir, txt("monsname"), txt("wazaname"), txt("tokusei"), txt("seikaku"),
                    txt("itemname"), personal, learn, exp)
