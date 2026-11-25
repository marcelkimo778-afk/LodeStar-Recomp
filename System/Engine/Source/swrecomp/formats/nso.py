"""NSO0 executable module parsing.

Header layout per https://switchbrew.org/wiki/NSO0, verified field-by-field
against all five modules in the reference ExeFS (see docs/architecture-audit.md):

    0x000  char  magic "NSO0"
    0x004  u32   version
    0x008  u32   reserved
    0x00C  u32   flags
                       0 .text lz4      1 .rodata lz4     2 .data lz4
                       3 .text sha256   4 .rodata sha256 5 .data sha256
                       6 execute-only memory
                       7 zstd instead of lz4 (unsupported here)
    0x010  u32[3] .text   (file_off, mem_off, decompressed_size)
    0x01C  u32   module name offset        (== sizeof(header), 0x100)
    0x020  u32[3] .rodata
    0x02C  u32   module name size
    0x030  u32[3] .data
    0x03C  u32   bss size
    0x040  u8[32] module id
    0x060  u32[3] compressed sizes (.text/.rodata/.data)
    0x06C  u8[0x1C] reserved
    0x088  u32   embedded offset (rel. .rodata), 0x08C embedded size
    0x090  u32   dynstr offset (rel. .rodata),  0x094 dynstr size
    0x098  u32   dynsym offset (rel. .rodata),  0x09C dynsym size
    0x0A0  u8[32] .text sha256
    0x0C0  u8[32] .rodata sha256
    0x0E0  u8[32] .data sha256

MOD0 (located by scanning .text, never at a fixed address) carries image-relative
offsets to the .dynamic array, .bss and .eh_frame_hdr.  In the reference modules
those offsets are relative to the MOD0 header itself; the parser accepts either
base but only after the referenced .dynamic array parses coherently, so a wrong
base can never be silently used.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field
from pathlib import Path

from ..errors import FormatError, UnsupportedError
from .lz4 import decompress_block

NSO_MAGIC = b"NSO0"
HEADER_SIZE = 0x100

SEG_TEXT = 0
SEG_RODATA = 1
SEG_DATA = 2
SEGMENT_NAMES = (".text", ".rodata", ".data")

# ELF64 dynamic tags that may legally appear in an NSO .dynamic array.
_KNOWN_TAGS = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
    0x6FFFFEF5, 0x6FFFFFF0, 0x6FFFFFF9, 0x6FFFFFFA, 0x6FFFFFFB, 0x6FFFFFFC,
    0x6FFFFFFD, 0x6FFFFFFE, 0x6FFFFFFF, 0x70000000, 0x7FFFFFFD, 0x7FFFFFFF,
}
_ANCHOR_TAGS = {1, 4, 5, 6, 7, 12, 17, 23, 25, 0x6FFFFEF5}

DT_NULL = 0
DT_NEEDED = 1
DT_PLTRELSZ = 2
DT_PLTGOT = 3
DT_HASH = 4
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_INIT = 12
DT_FINI = 13
DT_SONAME = 14
DT_REL = 17
DT_RELSZ = 18
DT_RELENT = 19
DT_PLTREL = 20
DT_JMPREL = 23
DT_INIT_ARRAY = 25
DT_FINI_ARRAY = 26
DT_INIT_ARRAYSZ = 27
DT_FINI_ARRAYSZ = 28
DT_FLAGS = 30
DT_FLAGS_1 = 0x6FFFFFFB
DT_GNU_HASH = 0x6FFFFEF5
DT_PLTREL = 20
DT_RELACOUNT = 0x6FFFFFF9
DT_RELCOUNT = 0x6FFFFFFA

TAG_NAMES = {
    DT_NEEDED: "NEEDED", DT_PLTRELSZ: "PLTRELSZ", DT_PLTGOT: "PLTGOT",
    DT_HASH: "HASH", DT_STRTAB: "STRTAB", DT_SYMTAB: "SYMTAB",
    DT_RELA: "RELA", DT_RELASZ: "RELASZ", DT_RELAENT: "RELAENT", DT_STRSZ: "STRSZ",
    DT_SYMENT: "SYMENT", DT_INIT: "INIT", DT_FINI: "FINI", DT_SONAME: "SONAME",
    DT_REL: "REL", DT_RELSZ: "RELSZ", DT_RELENT: "RELENT", DT_PLTREL: "PLTREL",
    DT_JMPREL: "JMPREL", DT_INIT_ARRAY: "INIT_ARRAY", DT_FINI_ARRAY: "FINI_ARRAY",
    DT_INIT_ARRAYSZ: "INIT_ARRAYSZ", DT_FINI_ARRAYSZ: "FINI_ARRAYSZ",
    DT_FLAGS: "FLAGS", DT_FLAGS_1: "FLAGS_1", DT_GNU_HASH: "GNU_HASH",
    DT_RELACOUNT: "RELACOUNT", DT_RELCOUNT: "RELCOUNT",
}


@dataclass
class NsoSegment:
    index: int
    file_offset: int
    mem_offset: int
    size: int
    compressed_size: int
    compressed: bool
    has_hash: bool
    stored_hash: bytes

    @property
    def name(self) -> str:
        return SEGMENT_NAMES[self.index]

    @property
    def stored_size(self) -> int:
        return self.compressed_size if self.compressed else self.size

    @property
    def file_end(self) -> int:
        return self.file_offset + self.stored_size


@dataclass
class Mod0:
    text_offset: int
    base: int                  # value added to the raw offsets below
    dynamic_offset: int
    bss_start_offset: int
    bss_end_offset: int
    eh_frame_hdr_start: int
    eh_frame_hdr_end: int
    module_object_offset: int


@dataclass
class DynamicInfo:
    array_offset: int = 0
    needed: list[str] = field(default_factory=list)
    soname: str | None = None
    init: int | None = None
    fini: int | None = None
    init_array_offset: int | None = None
    init_array: list[int] = field(default_factory=list)

    def resolved_init_array(self) -> list[int]:
        """Init-array entries with R_AARCH64_RELATIVE relocations applied.

        Switch executables store zero (or module-relative) entries in
        .init_array and fill the real function addresses with RELATIVE
        relocations, so reading the raw array alone finds no functions.
        """
        if not self.init_array_offset:
            return list(self.init_array)
        lo = self.init_array_offset
        hi = lo + len(self.init_array) * 8
        relocs = {r[0]: r for r in self.relocations if lo <= r[0] < hi}
        out: list[int] = []
        for i, value in enumerate(self.init_array):
            rel = relocs.get(lo + i * 8)
            if rel is not None and rel[1] == 1027:  # R_AARCH64_RELATIVE
                value = rel[2] & 0xFFFFFFFFFFFFFFFF
            out.append(value)
        return out
    fini_array: list[int] = field(default_factory=list)
    relocations: list[tuple[int, int, int, int]] = field(default_factory=list)  # (offset, type, addend, symbol)
    tags: dict[int, int] = field(default_factory=dict)
    plt_relocations: int = 0


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    info: int
    shndx: int

    @property
    def binding(self) -> int:
        return self.info >> 4

    @property
    def kind(self) -> int:
        return self.info & 0xF

    @property
    def is_import(self) -> bool:
        return self.shndx == 0

    @property
    def binding_name(self) -> str:
        return {0: "LOCAL", 1: "GLOBAL", 2: "WEAK"}.get(self.binding, str(self.binding))

    @property
    def kind_name(self) -> str:
        return {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION", 4: "FILE"}.get(self.kind, str(self.kind))


class Nso:
    """A parsed NSO0 module.  Segment bytes are loaded lazily and cached."""

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self._data: bytes | None = None
        self._segments: dict[int, bytes] = {}
        self.segments: list[NsoSegment] = []
        self.flags = 0
        self.version = 0
        self.module_id = b""
        self.module_name = ""
        self.bss_size = 0
        self.header = b""
        self.file_size = 0
        self.embedded: tuple[int, int] = (0, 0)
        self.dynstr: tuple[int, int] = (0, 0)
        self.dynsym: tuple[int, int] = (0, 0)
        self.mod0: Mod0 | None = None
        self.dynamic: DynamicInfo | None = None
        self._symbols: list[Symbol] | None = None

    @classmethod
    def load(cls, path: str | Path) -> "Nso":
        nso = cls(Path(path))
        nso._read_header()
        return nso

    # -- raw access ----------------------------------------------------
    def _raw(self) -> bytes:
        if self._data is None:
            self._data = self.path.read_bytes()
        return self._data

    def _read_header(self) -> None:
        raw = self._raw()
        self.file_size = len(raw)
        if len(raw) < HEADER_SIZE:
            raise FormatError("%s: too small for an NSO0 header" % self.path)
        header = raw[:HEADER_SIZE]
        self.header = header
        if header[:4] != NSO_MAGIC:
            raise FormatError("%s: bad magic %r (expected NSO0)" % (self.path, header[:4]))
        self.version = struct.unpack_from("<I", header, 0x04)[0]
        self.flags = struct.unpack_from("<I", header, 0x0C)[0]
        if self.flags & 0x80:
            raise UnsupportedError(
                "%s: NSO uses zstd (flag bit 7), which this build does not decode" % self.path,
                {"module": str(self.path), "flags": self.flags},
            )
        self.module_id = header[0x40:0x60]
        comp_sizes = struct.unpack_from("<3I", header, 0x60)
        hashes = (header[0xA0:0xC0], header[0xC0:0xE0], header[0xE0:0x100])
        name_off = struct.unpack_from("<I", header, 0x1C)[0]
        name_size = struct.unpack_from("<I", header, 0x2C)[0]
        if name_off and name_size:
            if name_off + name_size > len(raw):
                raise FormatError("%s: module name outside file" % self.path)
            self.module_name = raw[name_off:name_off + name_size].split(b"\0")[0].decode("utf-8", "replace")
        self.embedded = struct.unpack_from("<2I", header, 0x88)
        self.dynstr = struct.unpack_from("<2I", header, 0x90)
        self.dynsym = struct.unpack_from("<2I", header, 0x98)

        for i in range(3):
            file_off, mem_off, size = struct.unpack_from("<3I", header, 0x10 + i * 0x10)
            seg = NsoSegment(
                index=i,
                file_offset=file_off,
                mem_offset=mem_off,
                size=size,
                compressed_size=comp_sizes[i],
                compressed=bool((self.flags >> i) & 1),
                has_hash=bool((self.flags >> (i + 3)) & 1),
                stored_hash=hashes[i],
            )
            if size and seg.file_end > self.file_size:
                raise FormatError(
                    "%s: %s segment ends at 0x%x past EOF (0x%x)"
                    % (self.path, seg.name, seg.file_end, self.file_size)
                )
            self.segments.append(seg)
        self.bss_size = struct.unpack_from("<I", header, 0x3C)[0]
        if not self.segments[SEG_TEXT].size:
            raise FormatError("%s: empty .text segment" % self.path)

    # -- accessors -----------------------------------------------------
    @property
    def module_id_hex(self) -> str:
        return self.module_id.hex()

    @property
    def image_size(self) -> int:
        last = 0
        for seg in self.segments:
            if seg.size:
                last = max(last, seg.mem_offset + seg.size)
        return last + self.bss_size

    def segment_bytes(self, index: int) -> bytes:
        if index in self._segments:
            return self._segments[index]
        seg = self.segments[index]
        raw = self._raw()
        if not seg.size:
            data = b""
        elif seg.compressed:
            data = decompress_block(raw[seg.file_offset:seg.file_end], seg.size)
        else:
            data = raw[seg.file_offset:seg.file_offset + seg.size]
        self._segments[index] = data
        return data

    @property
    def text(self) -> bytes:
        return self.segment_bytes(SEG_TEXT)

    @property
    def rodata(self) -> bytes:
        return self.segment_bytes(SEG_RODATA)

    @property
    def data(self) -> bytes:
        return self.segment_bytes(SEG_DATA)

    def verify(self) -> dict:
        result: dict = {"path": str(self.path), "segments": {}, "ok": True}
        for seg in self.segments:
            entry = {"size": seg.size, "stored_size": seg.stored_size, "compressed": seg.compressed}
            if not seg.size:
                entry["status"] = "empty"
            else:
                try:
                    digest = hashlib.sha256(self.segment_bytes(seg.index)).digest()
                except FormatError as exc:
                    entry.update(status="error", error=str(exc))
                    result["ok"] = False
                    result["segments"][seg.name] = entry
                    continue
                entry["sha256"] = digest.hex()
                if seg.has_hash:
                    match = digest == seg.stored_hash
                    entry["status"] = "valid" if match else "invalid"
                    entry["expected"] = seg.stored_hash.hex()
                    if not match:
                        result["ok"] = False
                else:
                    entry["status"] = "unhashed"
            result["segments"][seg.name] = entry
        return result

    # -- memory --------------------------------------------------------
    def read_vaddr(self, offset: int, size: int) -> bytes | None:
        for seg in self.segments:
            if seg.size and seg.mem_offset <= offset and offset + size <= seg.mem_offset + seg.size:
                rel = offset - seg.mem_offset
                return self.segment_bytes(seg.index)[rel:rel + size]
        return None

    def read_cstring(self, offset: int, limit: int = 4096) -> str | None:
        out = bytearray()
        for i in range(limit):
            b = self.read_vaddr(offset + i, 1)
            if b is None:
                return None
            if b == b"\0":
                return out.decode("utf-8", "replace")
            out += b
        return None

    # -- MOD0 / dynamic ------------------------------------------------
    def _parse_dynamic_array(self, addr: int) -> list[tuple[int, int]] | None:
        entries: list[tuple[int, int]] = []
        for i in range(0, 0x40 * 16, 16):
            raw = self.read_vaddr(addr + i, 16)
            if raw is None:
                return None
            tag, val = struct.unpack("<qQ", raw)
            if tag == DT_NULL:
                if len(entries) < 3:
                    return None
                anchors = sum(1 for t, _ in entries if t in _ANCHOR_TAGS)
                if anchors < 2:
                    return None
                return entries
            if tag not in _KNOWN_TAGS:
                return None
            entries.append((tag, val))
        return None

    def find_mod0(self) -> Mod0 | None:
        """Locate the MOD0 header inside .text by scanning and validating."""
        if self.mod0 is not None:
            return self.mod0
        text = self.text
        limit = min(len(text), 0x1000)
        start = 0
        while True:
            pos = text.find(b"MOD0", start, limit)
            if pos < 0:
                return None
            start = pos + 1
            if pos % 4 or pos + 28 > len(text):
                continue
            dyn_raw, bss_s, bss_e, eh_s, eh_e, mod = struct.unpack_from("<6I", text, pos + 4)
            # Observed modules store offsets relative to the MOD0 header; the
            # image-base interpretation is tried as well and both are validated
            # against the .dynamic array before being accepted.
            for base in (pos, 0):
                entries = self._parse_dynamic_array(dyn_raw + base)
                if entries is None:
                    continue
                if not (bss_s + base <= bss_e + base <= self.image_size + 0x1000):
                    continue
                if eh_s + base >= eh_e + base or eh_e + base > self.image_size:
                    continue
                self.mod0 = Mod0(
                    text_offset=pos,
                    base=base,
                    dynamic_offset=dyn_raw + base,
                    bss_start_offset=bss_s + base,
                    bss_end_offset=bss_e + base,
                    eh_frame_hdr_start=eh_s + base,
                    eh_frame_hdr_end=eh_e + base,
                    module_object_offset=mod + base,
                )
                self._dynamic_entries = entries
                return self.mod0
        return None

    def parse_dynamic(self) -> DynamicInfo:
        if self.dynamic is not None:
            return self.dynamic
        info = DynamicInfo()
        mod0 = self.find_mod0()
        if mod0 is None:
            self.dynamic = info
            return info
        entries = getattr(self, "_dynamic_entries", None) or self._parse_dynamic_array(mod0.dynamic_offset) or []
        info.array_offset = mod0.dynamic_offset
        for tag, val in entries:
            info.tags[tag] = val

        strtab = info.tags.get(DT_STRTAB)
        strsz = info.tags.get(DT_STRSZ, 0)

        def str_at(off: int) -> str:
            if strtab is None or off >= max(strsz, 1):
                return "<bad string offset 0x%x>" % off
            s = self.read_cstring(strtab + off, min(strsz - off, 4096))
            return s if s is not None else "<unreadable string at 0x%x>" % (strtab + off)

        for tag, val in entries:
            if tag == DT_NEEDED:
                info.needed.append(str_at(val))
            elif tag == DT_SONAME:
                info.soname = str_at(val)
            elif tag == DT_INIT:
                info.init = val
            elif tag == DT_FINI:
                info.fini = val
            elif tag == DT_INIT_ARRAY:
                info.init_array_offset = val
        size = info.tags.get(DT_INIT_ARRAYSZ, 0)
        if info.init_array_offset and size:
            for i in range(0, size, 8):
                raw = self.read_vaddr(info.init_array_offset + i, 8)
                if raw is None:
                    raise FormatError("%s: .init_array outside image" % self.path)
                info.init_array.append(struct.unpack("<Q", raw)[0])
        fini_off = info.tags.get(DT_FINI_ARRAY)
        fini_size = info.tags.get(DT_FINI_ARRAYSZ, 0)
        if fini_off and fini_size:
            for i in range(0, fini_size, 8):
                raw = self.read_vaddr(fini_off + i, 8)
                if raw is None:
                    raise FormatError("%s: .fini_array outside image" % self.path)
                info.fini_array.append(struct.unpack("<Q", raw)[0])

        rela = info.tags.get(DT_RELA)
        relasz = info.tags.get(DT_RELASZ, 0)
        if rela and relasz:
            for i in range(0, relasz, 24):
                raw = self.read_vaddr(rela + i, 24)
                if raw is None:
                    raise FormatError("%s: .rela.dyn outside image" % self.path)
                r_offset, r_info, r_addend = struct.unpack("<QQq", raw)
                info.relocations.append((r_offset, r_info & 0xFFFFFFFF, r_addend, r_info >> 32))

        # PLT (JUMP_SLOT) relocations live in their own table on Switch; the
        # loader reads DT_JMPREL/DT_PLTRELSZ rather than merging it into
        # .rela.dyn.  Without it every GOT slot keeps pointing at the lazy
        # binding trampoline instead of the imported function.
        jmprel = info.tags.get(DT_JMPREL)
        pltsz = info.tags.get(DT_PLTRELSZ, 0)
        if jmprel and pltsz:
            for i in range(0, pltsz, 24):
                raw = self.read_vaddr(jmprel + i, 24)
                if raw is None:
                    raise FormatError("%s: .rela.plt outside image" % self.path)
                r_offset, r_info, r_addend = struct.unpack("<QQq", raw)
                info.relocations.append((r_offset, r_info & 0xFFFFFFFF, r_addend, r_info >> 32))
                info.plt_relocations += 1
        self.dynamic = info
        return info

    # -- symbols -------------------------------------------------------
    def symbols(self) -> list[Symbol]:
        if self._symbols is not None:
            return self._symbols
        entries: list[tuple[int, int, str]] = []
        df_off, df_size = self.dynstr
        ds_off, ds_size = self.dynsym
        ro_mem = self.segments[SEG_RODATA].mem_offset
        rodata = self.rodata
        out: list[Symbol] = []
        if df_size and ds_size:
            strtab = rodata[df_off:df_off + df_size] if df_off + df_size <= len(rodata) else b""
            symtab = rodata[ds_off:ds_off + ds_size] if ds_off + ds_size <= len(rodata) else b""
            for i in range(0, len(symtab) - 23, 24):
                name_off, sinfo, _other, shndx, value, size = struct.unpack_from("<IBBHQQ", symtab, i)
                if name_off >= len(strtab):
                    continue
                end = strtab.find(b"\0", name_off)
                name = strtab[name_off:end if end >= 0 else None].decode("utf-8", "replace")
                out.append(Symbol(name=name, value=value, size=size, info=sinfo, shndx=shndx))
            self._symbols = out
            return out
        # Fall back to the .dynamic symbol table when the header has no
        # embedded symbol info.
        info = self.parse_dynamic()
        symtab = info.tags.get(DT_SYMTAB)
        strtab_off = info.tags.get(DT_STRTAB)
        strs = info.tags.get(DT_STRSZ, 0)
        if symtab and strtab_off:
            for i in range(64):
                raw = self.read_vaddr(symtab + i * 24, 24)
                if raw is None:
                    break
                name_off, sinfo, _other, shndx, value, size = struct.unpack("<IBBHQQ", raw)
                if not name_off:
                    continue
                name = self.read_cstring(strtab_off + name_off, min(strs or 4096, 4096)) if name_off < max(strs, 1) else None
                out.append(Symbol(name=name or "<bad>", value=value, size=size, info=sinfo, shndx=shndx))
        self._symbols = out
        return out

    @property
    def imports(self) -> list[Symbol]:
        return [s for s in self.symbols() if s.is_import and s.name]

    @property
    def exports(self) -> list[Symbol]:
        return [s for s in self.symbols() if not s.is_import and s.name and s.kind in (0, 2)]

    # -- reporting -----------------------------------------------------
    def summary(self) -> dict:
        mod0 = self.find_mod0()
        dynamic = self.parse_dynamic() if mod0 else None
        return {
            "path": str(self.path),
            "version": self.version,
            "flags": self.flags,
            "module_id": self.module_id_hex,
            "module_name": self.module_name,
            "file_size": self.file_size,
            "image_size": self.image_size,
            "bss_size": self.bss_size,
            "segments": [
                {
                    "name": s.name,
                    "file_offset": s.file_offset,
                    "mem_offset": s.mem_offset,
                    "decompressed_size": s.size,
                    "stored_size": s.stored_size,
                    "compressed": s.compressed,
                    "has_hash": s.has_hash,
                }
                for s in self.segments
            ],
            "mod0": None if mod0 is None else {
                "text_offset": mod0.text_offset,
                "offset_base": mod0.base,
                "dynamic_offset": mod0.dynamic_offset,
                "bss_start": mod0.bss_start_offset,
                "bss_end": mod0.bss_end_offset,
                "eh_frame_hdr_start": mod0.eh_frame_hdr_start,
                "eh_frame_hdr_end": mod0.eh_frame_hdr_end,
                "module_object": mod0.module_object_offset,
            },
            "dynamic": None if dynamic is None else {
                "array_offset": dynamic.array_offset,
                "needed": dynamic.needed,
                "soname": dynamic.soname,
                "init": dynamic.init,
                "init_array_offset": dynamic.init_array_offset,
                "init_array_size": len(dynamic.init_array),
                "relocations": len(dynamic.relocations),
                "tags": {TAG_NAMES.get(k, hex(k)): v for k, v in dynamic.tags.items()},
            },
            "symbols": {
                "total": len(self.symbols()),
                "imports": len(self.imports),
                "exports": len(self.exports),
            },
            "dynstr": {"offset": self.dynstr[0], "size": self.dynstr[1]},
            "dynsym": {"offset": self.dynsym[0], "size": self.dynsym[1]},
        }
