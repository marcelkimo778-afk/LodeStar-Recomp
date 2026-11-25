"""Save editor model: Pokemon (PA8) records and a wrapper over the save's blocks.

PA8 layout/crypto follows the public PKHeX documentation of the format; it was
checked against a real save (checksums, stats, height/weight reproduce).
"""
from __future__ import annotations

import math
import random
import struct
from pathlib import Path

from pla_save import Keys, decrypt, encrypt

PK_STORED, PK_PARTY, PK_BLOCK = 0x168, 0x178, 0x58
_SHUFFLE = [0, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1,
            1, 0, 2, 3, 1, 0, 3, 2, 2, 0, 1, 3, 3, 0, 1, 2, 2, 0, 3, 1, 3, 0, 2, 1,
            1, 2, 0, 3, 1, 3, 0, 2, 2, 1, 0, 3, 3, 1, 0, 2, 2, 3, 0, 1, 3, 2, 0, 1,
            1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1, 0]
GV_MUL = [0, 2, 3, 4, 7, 8, 9, 14, 15, 16, 25]

K_PARTY, K_BOX = 0x2985FE5D, 0x47E1CEAB
K_ITEMS, K_KEY_ITEMS, K_STORAGE = 0x9FE2790A, 0x59A4D0C3, 0x8E434F0D


def _lcrng(data: bytes, seed: int) -> bytes:
    out = bytearray(data)
    for i in range(0, len(out) - 1, 2):
        seed = (0x41C64E6D * seed + 0x6073) & 0xFFFFFFFF
        out[i] ^= (seed >> 16) & 0xFF
        out[i + 1] ^= seed >> 24
    return bytes(out)


def _order(pv: int) -> list[int]:
    sv = (pv >> 13) & 31
    return _SHUFFLE[(sv % 24) * 4:(sv % 24) * 4 + 4]


def pk_decrypt(raw: bytes) -> bytearray:
    pv = struct.unpack_from("<I", raw)[0]
    order = _order(pv)
    body = _lcrng(raw[8:PK_STORED], pv)
    blocks = [body[i * PK_BLOCK:(i + 1) * PK_BLOCK] for i in range(4)]
    plain = b"".join(blocks[order[i]] for i in range(4))
    tail = _lcrng(raw[PK_STORED:], pv) if len(raw) > PK_STORED else b""
    return bytearray(raw[:8] + plain + tail)


def pk_encrypt(plain: bytes) -> bytes:
    pv = struct.unpack_from("<I", plain)[0]
    order = _order(pv)
    blocks = [plain[8 + i * PK_BLOCK:8 + (i + 1) * PK_BLOCK] for i in range(4)]
    shuffled = [b""] * 4
    for i in range(4):                   # inverse of pk_decrypt
        shuffled[order[i]] = blocks[i]
    body = _lcrng(b"".join(shuffled), pv)
    tail = _lcrng(plain[PK_STORED:], pv) if len(plain) > PK_STORED else b""
    return bytes(plain[:8]) + body + tail


class Pokemon:
    def __init__(self, plain: bytearray):
        self.d = plain

    def _u16(self, o): return struct.unpack_from("<H", self.d, o)[0]
    def _w16(self, o, v): struct.pack_into("<H", self.d, o, v & 0xFFFF)
    def _u32(self, o): return struct.unpack_from("<I", self.d, o)[0]
    def _w32(self, o, v): struct.pack_into("<I", self.d, o, v & 0xFFFFFFFF)

    species = property(lambda s: s._u16(0x08), lambda s, v: s._w16(0x08, v))
    form = property(lambda s: s.d[0x24], lambda s, v: s._w16(0x24, v))
    exp = property(lambda s: s._u32(0x10), lambda s, v: s._w32(0x10, v))
    pid = property(lambda s: s._u32(0x1C), lambda s, v: s._w32(0x1C, v))
    tid = property(lambda s: s._u16(0x0C))
    sid = property(lambda s: s._u16(0x0E))
    ability = property(lambda s: s._u16(0x14))

    @property
    def level(self) -> int:
        return self.d[0x168] if len(self.d) > 0x168 else 0

    @property
    def nature(self) -> int:
        return self.d[0x21]

    @nature.setter
    def nature(self, v: int) -> None:
        self.d[0x20] = self.d[0x21] = v

    @property
    def is_empty(self) -> bool:
        return self.species == 0

    @property
    def alpha(self) -> bool:
        return bool(self.d[0x16] & 0x20)

    @alpha.setter
    def alpha(self, v: bool) -> None:
        self.d[0x16] = (self.d[0x16] & ~0x20) | (0x20 if v else 0)

    @property
    def shiny(self) -> bool:
        p = self.pid
        return (self.tid ^ self.sid ^ (p >> 16) ^ (p & 0xFFFF)) < 16

    @shiny.setter
    def shiny(self, v: bool) -> None:
        p = self.pid
        if v and not self.shiny:
            hi = (self.tid ^ self.sid ^ (p & 0xFFFF)) & 0xFFFF
            self.pid = (hi << 16) | (p & 0xFFFF)
        elif not v and self.shiny:
            self.pid = p ^ 0x10000000

    @property
    def nickname(self) -> str:
        return bytes(self.d[0x60:0x60 + 26]).decode("utf-16le", "replace").split("\0")[0]

    @nickname.setter
    def nickname(self, s: str) -> None:
        raw = s[:12].encode("utf-16le")
        self.d[0x60:0x60 + 26] = raw + b"\0" * (26 - len(raw))

    @property
    def moves(self) -> list[int]:
        return list(struct.unpack_from("<4H", self.d, 0x54))

    @moves.setter
    def moves(self, mv: list[int]) -> None:
        struct.pack_into("<4H", self.d, 0x54, *((list(mv) + [0, 0, 0, 0])[:4]))

    @property
    def ivs(self) -> list[int]:          # HP Atk Def Spe SpA SpD
        v = self._u32(0x94)
        return [(v >> (5 * i)) & 31 for i in range(6)]

    @ivs.setter
    def ivs(self, ivs: list[int]) -> None:
        v = self._u32(0x94) & 0xC0000000
        for i, iv in enumerate(ivs):
            v |= (max(0, min(31, iv)) & 31) << (5 * i)
        self._w32(0x94, v)

    @property
    def gvs(self) -> list[int]:          # effort levels 0..10
        return list(self.d[0xA4:0xAA])

    @gvs.setter
    def gvs(self, g: list[int]) -> None:
        self.d[0xA4:0xAA] = bytes(max(0, min(10, x)) for x in g)

    def stats(self) -> list[int]:
        return list(struct.unpack_from("<6H", self.d, 0x16A)) if len(self.d) >= PK_PARTY else []

    def checksum(self) -> int:
        return sum(struct.unpack_from("<%dH" % ((PK_STORED - 8) // 2), self.d, 8)) & 0xFFFF

    def checksum_ok(self) -> bool:
        return self.checksum() == self._u16(0x06)

    def _personal(self, gd):
        return gd.personal.get((self.species, self.form)) or gd.personal[(self.species, 0)]

    def refresh(self, gd, size: bool = False) -> None:
        """Recompute level, stats, HP, PP and checksum (and height/weight if size) from the game data."""
        p = self._personal(gd)
        lvl = gd.level_for_exp(p.growth, self.exp)
        hp, atk, df, spa, spd, spe = p.base
        base = [hp, atk, df, spe, spa, spd]          # PA8 stat order
        ivs, gvs, ht, nat = self.ivs, self.gvs, self.d[0x13E], self.d[0x21]
        out = []
        for i in range(6):
            iv = 31 if (ht >> i) & 1 else ivs[i]
            if i == 0:
                s = int(((lvl / 100.0) + 1.0) * base[0] + lvl)
            else:
                s = int((((lvl / 50.0) + 1.0) * base[i]) / 1.5)
                inc, dec = nat // 5, nat % 5
                if inc != dec:
                    if i - 1 == inc:
                        s = s * 110 // 100
                    elif i - 1 == dec:
                        s = s * 90 // 100
            ivb = 3 if iv >= 31 else 2 if iv >= 26 else 1 if iv >= 20 else 0
            mul = GV_MUL[min(10, gvs[i] + ivb)]
            s += int(math.floor((math.sqrt(base[i]) * mul + lvl) / 2.5 + 0.5))
            out.append(s)
        if len(self.d) >= PK_PARTY:
            self.d[0x168] = lvl
            struct.pack_into("<6H", self.d, 0x16A, *out)
        self._w16(0x92, out[0])                       # full HP
        self._w32(0x9C, 0)                            # no status condition
        if size:
            hr = self.d[0x50] / 255 * 0.4 + 0.8
            wr = self.d[0x51] / 255 * 0.4 + 0.8
            struct.pack_into("<2f", self.d, 0xAC, p.height * hr, p.weight * hr * wr)
        for i, m in enumerate(self.moves):
            self.d[0x5C + i] = gd.move_pp(m) if m else 0
        self._w16(0x06, self.checksum())

    def make_species(self, gd, species: int, form: int, level: int, rng: random.Random | None = None) -> None:
        """Turn this record into a fresh species/form at level; keeps OT, ball, met data, language."""
        r = rng or random.Random()
        p = gd.personal.get((species, form)) or gd.personal[(species, 0)]
        self.species, self.form = species, form
        self._w32(0x00, r.getrandbits(32))                         # encryption constant
        self.pid = r.getrandbits(32)
        self._w16(0x0A, 0)                                         # held item
        self._w16(0x14, p.abilities[0])
        self.d[0x16] = (self.d[0x16] & ~0x67) | 1                  # ability slot 1, not alpha/noble
        g = 2 if p.gender == 255 else 1 if p.gender == 254 else 0 if p.gender == 0 else \
            (1 if r.randrange(253) + 1 < p.gender else 0)
        self.d[0x22] = (self.d[0x22] & 0xF3) | (g << 2)
        self.nature = r.randrange(25)
        self.d[0x26:0x2C] = bytes(6)                               # EVs (unused in this game)
        self.gvs = [0] * 6
        self.d[0x13E] = 0                                          # hyper training
        self.ivs = [r.randrange(32) for _ in range(6)]
        self._w32(0x94, self._u32(0x94) & 0x3FFFFFFF)              # not an egg, not nicknamed
        self.nickname = gd.species_names[species] if species < len(gd.species_names) else ""
        self.d[0x50] = self.d[0x52] = r.randrange(256)
        self.d[0x51] = r.randrange(256)
        self._w16(0x3E, 0)                                         # alpha move
        self.exp = gd.exp_tables[p.growth][max(1, min(100, level))]
        self.moves = gd.moves_at_level(species, form, level) or [33]
        self.d[0x86:0x8A] = bytes(4)                               # PP ups
        struct.pack_into("<4H", self.d, 0x8A, 0, 0, 0, 0)          # relearn moves
        self.d[0x13F:0x165] = bytes(0x165 - 0x13F)                 # move shop / mastery records
        self.refresh(gd, size=True)

    def set_level(self, gd, level: int) -> None:
        self.exp = gd.exp_tables[self._personal(gd).growth][max(1, min(100, level))]
        self.refresh(gd)


class SaveFile:
    """The blocks of one save plus helpers for the editor."""

    def __init__(self, path: Path, keys: Keys):
        self.path, self.keys = path, keys
        self.blocks = decrypt(path.read_bytes(), keys)
        self.by_key = {b.key: b for b in self.blocks}

    def has(self, key: int) -> bool:
        return key in self.by_key

    def get(self, key: int):
        b = self.by_key.get(key)
        return None if b is None else b.value

    def set(self, key: int, v) -> None:
        self.by_key[key].set_value(v)

    def party(self) -> list[Pokemon]:
        d = self.by_key[K_PARTY].data
        return [Pokemon(pk_decrypt(d[i * PK_PARTY:(i + 1) * PK_PARTY])) for i in range(6)]

    def party_count(self) -> int:
        return self.by_key[K_PARTY].data[6 * PK_PARTY]

    def set_party(self, mons: list[Pokemon]) -> None:
        """Write the party back; filled slots are packed to the front (the game expects that)."""
        b = self.by_key[K_PARTY]
        d = bytearray(b.data)
        filled = [m for m in mons[:6] if not m.is_empty]
        empty = [m for m in mons[:6] if m.is_empty]
        for i, m in enumerate(filled + empty):
            m._w16(0x06, m.checksum())
            d[i * PK_PARTY:(i + 1) * PK_PARTY] = pk_encrypt(bytes(m.d))
        d[6 * PK_PARTY] = len(filled)
        b.data = bytes(d)

    def items(self, key: int) -> list[tuple[int, int]]:
        d = self.by_key[key].data
        return [struct.unpack_from("<HH", d, i) for i in range(0, len(d) - 3, 4)]

    def set_items(self, key: int, entries: list[tuple[int, int]]) -> None:
        b = self.by_key[key]
        d = bytearray(len(b.data))
        for i, (it, n) in enumerate([e for e in entries if e[0]][:len(d) // 4]):
            struct.pack_into("<HH", d, i * 4, it, n)
        b.data = bytes(d)

    def to_bytes(self) -> bytes:
        data = encrypt(self.blocks, self.keys)
        decrypt(data, self.keys)                 # self-check: must parse again
        return data

    def save(self) -> None:
        data = self.to_bytes()
        tmp = self.path.with_name(self.path.name + ".tmp")
        tmp.write_bytes(data)
        tmp.replace(self.path)
