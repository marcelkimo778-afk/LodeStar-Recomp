"""ExeFS enumeration: either an extracted directory or a PFS0 container.

An ExeFS holds ``main`` (the program NSO), ``main.npdm`` (metadata) and zero or
more additional NSO modules (``rtld``, ``sdk``, ``subsdk*``, ``nro``...).  This
module only *enumerates and validates*; decoding code is the recompiler's job.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path

from ..errors import FormatError, MissingInputError
from .npdm import Npdm, parse_npdm
from .nso import Nso

PFS0_MAGIC = b"PFS0"


@dataclass
class ExeFsEntry:
    name: str
    size: int
    offset: int | None = None  # absolute file offset in a PFS0 container

    @property
    def is_npdm(self) -> bool:
        return self.name.endswith(".npdm")

    @property
    def is_nso(self) -> bool:
        return not self.is_npdm and not self.name.endswith(".nro")


@dataclass
class ExeFs:
    path: Path
    container: bool
    entries: list[ExeFsEntry] = field(default_factory=list)
    _backing: Path | None = None

    def find(self, name: str) -> ExeFsEntry | None:
        for e in self.entries:
            if e.name == name:
                return e
        return None

    @property
    def module_names(self) -> list[str]:
        return [e.name for e in self.entries if e.is_nso]

    @property
    def npdm_entry(self) -> ExeFsEntry | None:
        return self.find("main.npdm") or next((e for e in self.entries if e.is_npdm), None)

    def read(self, name: str) -> bytes:
        entry = self.find(name)
        if entry is None:
            raise MissingInputError("%s: no ExeFS entry %r" % (self.path, name))
        if not self.container:
            return (self.path / entry.name).read_bytes()
        assert self._backing is not None
        raw = self._backing.read_bytes()
        assert entry.offset is not None
        return raw[entry.offset:entry.offset + entry.size]

    def materialize_module(self, name: str, dest_dir: Path) -> Path:
        """Write one module out so the NSO reader can use a real path."""
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest = dest_dir / name
        if not self.container:
            src = self.path / name
            if src.resolve() == dest.resolve():
                return dest
        data = self.read(name)
        dest.write_bytes(data)
        return dest

    def load_nso(self, name: str, work_dir: Path) -> Nso:
        return Nso.load(self.materialize_module(name, work_dir))


def open_exefs(path: str | Path) -> ExeFs:
    path = Path(path)
    if not path.exists():
        raise MissingInputError("ExeFS path does not exist: %s" % path)
    if path.is_dir():
        entries = []
        for child in sorted(path.iterdir()):
            if child.is_file():
                entries.append(ExeFsEntry(name=child.name, size=child.stat().st_size))
        exefs = ExeFs(path=path, container=False, entries=entries)
        _validate(exefs)
        return exefs
    raw = path.read_bytes()
    if raw[:4] != PFS0_MAGIC:
        raise FormatError("%s: neither a directory nor a PFS0 container" % path)
    return _parse_pfs0(path, raw)


def _parse_pfs0(path: Path, raw: bytes) -> ExeFs:
    if len(raw) < 0x10:
        raise FormatError("%s: truncated PFS0 header" % path)
    count, string_table_size, _reserved = struct.unpack_from("<III", raw, 4)
    entries = []
    table_end = 0x10 + count * 0x18 + string_table_size
    if table_end > len(raw):
        raise FormatError("%s: PFS0 entry table past end of file" % path)
    data_start = 0x10 + count * 0x18 + string_table_size
    strings = raw[0x10 + count * 0x18:table_end]
    for i in range(count):
        off, size, name_off = struct.unpack_from("<QQI", raw, 0x10 + i * 0x18)
        name_end = strings.find(b"\0", name_off)
        name = strings[name_off:name_end if name_end >= 0 else None].decode("utf-8", "replace")
        absolute = data_start + off
        if absolute + size > len(raw):
            raise FormatError("%s: PFS0 entry %r past end of file" % (path, name))
        entries.append(ExeFsEntry(name=name, size=size, offset=absolute))
    exefs = ExeFs(path=path, container=True, entries=entries, _backing=path)
    _validate(exefs)
    return exefs


def _validate(exefs: ExeFs) -> None:
    if exefs.find("main") is None:
        raise FormatError("%s: ExeFS has no 'main' module" % exefs.path)
    if exefs.npdm_entry is None:
        raise FormatError("%s: ExeFS has no NPDM metadata" % exefs.path)
    if not exefs.entries:
        raise FormatError("%s: ExeFS is empty" % exefs.path)


def load_npdm(exefs: ExeFs, work_dir: Path) -> Npdm:
    entry = exefs.npdm_entry
    assert entry is not None
    target = work_dir / entry.name
    work_dir.mkdir(parents=True, exist_ok=True)
    target.write_bytes(exefs.read(entry.name))
    return parse_npdm(target)


def read_magic(data: bytes) -> str:
    return data[:4].decode("ascii", "replace")
