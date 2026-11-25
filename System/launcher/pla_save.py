"""Pokémon Legends: Arceus save file ("main"): decrypt, list blocks, edit, re-encrypt.

Format (SwishCrypto, as the game's own code does it; constants are read from
the user's own game binary at run time, nothing is copied into this file):

  file   = body ^ pad (127-byte static pad, repeated) || SHA-256(intro || encrypted body || outro)
           pad   at main+0x3a37974 (0x7f bytes)  - loaded by the verify routine main+0x10c0968
           intro at main+0x3a37cc4 (64 bytes), outro at main+0x3a37d04 (64 bytes) - main+0x113fd0c / 0x113fdc0
  body   = blocks back to back.  Block: u32 key (plain), then every following byte XORed with a
           keystream from xorshift32 seeded by the key:
             type u8: 1 bool false, 2 bool true, 3 bool array?, 4 object (u32 size + bytes),
                      5 array (u32 count + u8 sub-type + count * size(sub-type)),
                      8 u8, 9 u16, 10 u32, 11 u64, 12 s8, 13 s16, 14 s32, 15 s64, 16 f32, 17 f64
"""
from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field
from pathlib import Path

PAD_ADDR, PAD_LEN = 0x3A37974, 0x7F
INTRO_ADDR, OUTRO_ADDR, HASH_PART = 0x3A37CC4, 0x3A37D04, 64
MAIN_BASE = 0x100000

T_BOOL_FALSE, T_BOOL_TRUE, T_BOOL3, T_OBJECT, T_ARRAY = 1, 2, 3, 4, 5
SIZES = {8: 1, 9: 2, 10: 4, 11: 8, 12: 1, 13: 2, 14: 4, 15: 8, 16: 4, 17: 8}
FMT = {8: "<B", 9: "<H", 10: "<I", 11: "<Q", 12: "<b", 13: "<h", 14: "<i", 15: "<q", 16: "<f", 17: "<d"}
TYPE_NAMES = {1: "bool", 2: "bool", 3: "bool3", 4: "object", 5: "array", 8: "u8", 9: "u16", 10: "u32",
              11: "u64", 12: "s8", 13: "s16", 14: "s32", 15: "s64", 16: "f32", 17: "f64"}


@dataclass
class Keys:
    pad: bytes
    intro: bytes
    outro: bytes


# Where the constants sit in the game builds seen so far: (pad, intro) addresses; outro = intro + 64.
# A build not listed here is searched once (find_keys) and the result kept in <cache>/save_keys.json.
KNOWN_LAYOUTS = [(PAD_ADDR, INTRO_ADDR), (0x3A78D24, 0x3A791A0)]
KEYS_CACHE = "save_keys.json"


def _load_main(exefs_dir: Path, cache_dir: Path):
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Engine" / "Source"))
    from swrecomp.formats.exefs import open_exefs
    return open_exefs(exefs_dir).load_nso("main", cache_dir)


def _keys_at(nso, pad: int, intro: int) -> Keys:
    rd = lambda a, n: nso.read_vaddr(a - MAIN_BASE, n)
    return Keys(rd(pad, PAD_LEN), rd(intro, HASH_PART), rd(intro + HASH_PART, HASH_PART))


def _opens(raw: bytes, keys: Keys) -> bool:
    try:
        decrypt(raw, keys)
        return True
    except (ValueError, struct.error, IndexError):
        return False


def find_keys(nso, raw: bytes) -> tuple[int, int] | None:
    """Search the game's read-only data for the constants that open this save: the hash parts
    first (128 high-entropy bytes whose SHA-256 over the save matches), then the pad near them."""
    ro_seg = nso.segments[1]
    ro = nso.read_vaddr(ro_seg.mem_offset, ro_seg.size)
    body, h = raw[:-0x20], raw[-0x20:]
    intro_off = None
    for off in range(0, len(ro) - 2 * HASH_PART, 4):
        w = ro[off:off + 2 * HASH_PART]
        if w.count(0) > 3 or len(set(w)) < 75:
            continue
        if hashlib.sha256(w[:HASH_PART] + body + w[HASH_PART:]).digest() == h:
            intro_off = off
            break
    if intro_off is None:
        return None
    intro = ro[intro_off:intro_off + HASH_PART]
    outro = ro[intro_off + HASH_PART:intro_off + 2 * HASH_PART]
    ok_types = {T_BOOL_FALSE, T_BOOL_TRUE, T_BOOL3, T_OBJECT, T_ARRAY} | set(SIZES)
    lo, hi = max(0, intro_off - 0x10000), min(len(ro) - PAD_LEN, intro_off + 0x10000)
    for off in range(lo, hi):
        pad = ro[off:off + PAD_LEN]
        if pad.count(0) > 8:
            continue
        first = bytes(a ^ b for a, b in zip(body[:5], pad))          # quick check: the first block's type
        if first[4] ^ XorShift(struct.unpack_from("<I", first)[0]).byte() not in ok_types:
            continue
        if _opens(raw, Keys(pad, intro, outro)):
            base = ro_seg.mem_offset + MAIN_BASE
            return base + off, base + intro_off
    return None


def keys_from_game(exefs_dir: Path, cache_dir: Path, sample: Path | None = None) -> Keys:
    """The constants from the user's own main NSO (via the recompiler's loader). Where they sit
    depends on the game build; with a save to test against (sample) the right place is picked,
    or searched for once in a build this code does not know yet."""
    import json
    nso = _load_main(exefs_dir, cache_dir)
    raw = sample.read_bytes() if sample is not None and sample.is_file() else None
    cache = Path(cache_dir) / KEYS_CACHE
    try:
        known = json.loads(cache.read_text()).get(nso.module_id_hex)
    except (OSError, ValueError, AttributeError):
        known = None
    layouts = ([tuple(known)] if known else []) + KNOWN_LAYOUTS
    for pad, intro in layouts:
        try:
            keys = _keys_at(nso, pad, intro)
        except Exception:          # noqa: BLE001 - an address outside this build's image
            continue
        if raw is None:
            if known:              # no save to test: a layout found for this build before
                return keys
            continue
        if _opens(raw, keys):
            return keys
    if raw is None:                # nothing to test with: the known layout that looks like keys
        for pad, intro in KNOWN_LAYOUTS:
            keys = _keys_at(nso, pad, intro)
            if len(set(keys.pad)) > 90 and len(set(keys.intro)) > 40:
                return keys
        return _keys_at(nso, *KNOWN_LAYOUTS[0])
    found = find_keys(nso, raw)
    if found is None:
        raise ValueError("this game build's save keys were not found (is this a Legends: Arceus save?)")
    try:
        data = json.loads(cache.read_text()) if cache.is_file() else {}
        data[nso.module_id_hex] = list(found)
        cache.write_text(json.dumps(data, indent=1))
    except (OSError, ValueError):
        pass
    return _keys_at(nso, *found)


class XorShift:
    """Keystream of one block: xorshift32 advanced popcount(key) times, 4 bytes per state."""

    def __init__(self, key: int):
        s = key & 0xFFFFFFFF
        for _ in range(bin(s).count("1")):
            s = self._adv(s)
        self.s, self.i = s, 0

    @staticmethod
    def _adv(s: int) -> int:
        s ^= (s << 2) & 0xFFFFFFFF
        s ^= s >> 15
        s ^= (s << 13) & 0xFFFFFFFF
        return s

    def byte(self) -> int:
        b = (self.s >> (8 * self.i)) & 0xFF
        if self.i == 3:
            self.s, self.i = self._adv(self.s), 0
        else:
            self.i += 1
        return b

    def bytes(self, n: int) -> bytes:
        return bytes(self.byte() for _ in range(n))


@dataclass
class Block:
    key: int
    type: int
    data: bytes = b""          # objects / arrays / numbers: the plain payload (without size fields)
    sub: int = 0               # arrays: element type

    @property
    def value(self):
        if self.type in (T_BOOL_FALSE, T_BOOL_TRUE):
            return self.type == T_BOOL_TRUE
        if self.type in FMT:
            return struct.unpack(FMT[self.type], self.data)[0]
        return None

    def set_value(self, v) -> None:
        if self.type in (T_BOOL_FALSE, T_BOOL_TRUE):
            self.type = T_BOOL_TRUE if v else T_BOOL_FALSE
        elif self.type in FMT:
            self.data = struct.pack(FMT[self.type], v)
        else:
            raise TypeError(f"block {self.key:08X} ({TYPE_NAMES.get(self.type)}) has no single value")


def _xor_pad(data: bytes, pad: bytes) -> bytes:
    p = (pad * (len(data) // len(pad) + 1))[:len(data)]
    return bytes(a ^ b for a, b in zip(data, p))


def _hash(keys: Keys, enc_body: bytes) -> bytes:
    return hashlib.sha256(keys.intro + enc_body + keys.outro).digest()


def decrypt(raw: bytes, keys: Keys) -> list[Block]:
    body, h = raw[:-0x20], raw[-0x20:]
    if _hash(keys, body) != h:
        raise ValueError("not a Legends: Arceus save, or damaged (hash mismatch)")
    plain = _xor_pad(body, keys.pad)
    blocks, o = [], 0
    while o < len(plain):
        key = struct.unpack_from("<I", plain, o)[0]
        o += 4
        x = XorShift(key)
        t = plain[o] ^ x.byte()
        o += 1
        if t in (T_BOOL_FALSE, T_BOOL_TRUE, T_BOOL3):
            blocks.append(Block(key, t))
        elif t == T_OBJECT:
            n = struct.unpack_from("<I", bytes(b ^ x.byte() for b in plain[o:o + 4]))[0]
            o += 4
            blocks.append(Block(key, t, bytes(b ^ x.byte() for b in plain[o:o + n])))
            o += n
        elif t == T_ARRAY:
            n = struct.unpack_from("<I", bytes(b ^ x.byte() for b in plain[o:o + 4]))[0]
            o += 4
            sub = plain[o] ^ x.byte()
            o += 1
            size = n * (1 if sub in (T_BOOL_FALSE, T_BOOL_TRUE, T_BOOL3) else SIZES.get(sub, 0))
            if sub not in SIZES and sub not in (T_BOOL_FALSE, T_BOOL_TRUE, T_BOOL3):
                raise ValueError(f"block {key:08X}: unknown array element type {sub} at {o:#x}")
            blocks.append(Block(key, t, bytes(b ^ x.byte() for b in plain[o:o + size]), sub))
            o += size
        elif t in SIZES:
            n = SIZES[t]
            blocks.append(Block(key, t, bytes(b ^ x.byte() for b in plain[o:o + n])))
            o += n
        else:
            raise ValueError(f"block {key:08X}: unknown type {t} at {o - 1:#x}")
    if o != len(plain):
        raise ValueError(f"blocks end at {o:#x}, data at {len(plain):#x}")
    return blocks


def encrypt(blocks: list[Block], keys: Keys) -> bytes:
    out = bytearray()
    for b in blocks:
        out += struct.pack("<I", b.key)
        x = XorShift(b.key)
        enc = lambda bs: bytes(c ^ x.byte() for c in bs)
        out += enc(bytes([b.type]))
        if b.type == T_OBJECT:
            out += enc(struct.pack("<I", len(b.data))) + enc(b.data)
        elif b.type == T_ARRAY:
            es = 1 if b.sub in (T_BOOL_FALSE, T_BOOL_TRUE, T_BOOL3) else SIZES[b.sub]
            out += enc(struct.pack("<I", len(b.data) // es)) + enc(bytes([b.sub])) + enc(b.data)
        elif b.type in SIZES:
            out += enc(b.data)
    body = _xor_pad(bytes(out), keys.pad)
    return body + _hash(keys, body)
