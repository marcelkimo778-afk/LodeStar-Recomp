"""RomFS access.

Two backends share one interface:

* ``DirectoryRomFs``  - a RomFS that was already extracted to a directory.
* ``ContainerRomFs``  - a real RomFS container (romfs header + IVFC levels +
  directory/file metadata tables + data region).

The compatibility runtime consumes ``RomFs`` objects, so extracted and packaged
titles go through identical code.  Paths are validated: absolute paths, ``..``
and backslash separators are rejected rather than silently normalised, because a
game asking for such a path is a bug we want reported.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import BinaryIO, Iterator

from ..errors import FormatError, MissingInputError

ROMFS_HEADER_SIZE = 0x50
IVFC_MAGIC = b"IVFC"
NO_ENTRY = 0xFFFFFFFF


@dataclass
class RomFsEntry:
    path: str
    is_dir: bool
    size: int = 0
    data_offset: int = 0


class RomFs:
    """Common RomFS interface."""

    kind = "unknown"

    def list(self, path: str) -> list[RomFsEntry]:
        raise NotImplementedError

    def read(self, path: str) -> bytes:
        raise NotImplementedError

    def open(self, path: str) -> BinaryIO:
        raise NotImplementedError

    def exists(self, path: str) -> bool:
        raise NotImplementedError

    def stat(self, path: str) -> RomFsEntry:
        raise NotImplementedError

    def walk(self, path: str = "") -> Iterator[RomFsEntry]:
        for entry in self.list(path):
            yield entry
            if entry.is_dir:
                yield from self.walk(entry.path)

    def summary(self) -> dict:
        raise NotImplementedError


def check_path(path: str) -> str:
    """Validate and normalise a RomFS path (no leading slash, no ``..``)."""
    if path in ("", "/", "."):
        return ""
    if "\\" in path:
        raise FormatError("romfs: backslash in path %r" % path)
    if path.startswith("/"):
        path = path.lstrip("/")
    parts = []
    for part in path.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            raise FormatError("romfs: path traversal rejected: %r" % path)
        parts.append(part)
    return "/".join(parts)


class DirectoryRomFs(RomFs):
    kind = "directory"

    def __init__(self, root: str | Path) -> None:
        self.root = Path(root)
        if not self.root.is_dir():
            raise MissingInputError("RomFS directory does not exist: %s" % self.root)
        self._files = 0
        self._bytes = 0
        self._dirs = 0
        for p in self.root.rglob("*"):
            if p.is_file():
                self._files += 1
                self._bytes += p.stat().st_size
            elif p.is_dir():
                self._dirs += 1

    def _resolve(self, path: str) -> Path:
        p = check_path(path)
        target = self.root / p if p else self.root
        resolved = target.resolve()
        root = self.root.resolve()
        if resolved != root and root not in resolved.parents:
            raise FormatError("romfs: path escapes root: %r" % path)
        return resolved

    def list(self, path: str) -> list[RomFsEntry]:
        target = self._resolve(path)
        if not target.exists():
            raise MissingInputError("romfs: no such path %r" % path)
        if not target.is_dir():
            raise FormatError("romfs: not a directory: %r" % path)
        base = check_path(path)
        out = []
        for child in sorted(target.iterdir(), key=lambda c: (not c.is_dir(), c.name)):
            rel = child.name if not base else base + "/" + child.name
            if child.is_dir():
                out.append(RomFsEntry(rel, True))
            else:
                out.append(RomFsEntry(rel, False, child.stat().st_size))
        return out

    def read(self, path: str) -> bytes:
        target = self._resolve(path)
        if not target.is_file():
            raise MissingInputError("romfs: no such file %r" % path)
        return target.read_bytes()

    def open(self, path: str) -> BinaryIO:
        target = self._resolve(path)
        if not target.is_file():
            raise MissingInputError("romfs: no such file %r" % path)
        return target.open("rb")

    def exists(self, path: str) -> bool:
        try:
            return self._resolve(path).exists()
        except FormatError:
            return False

    def stat(self, path: str) -> RomFsEntry:
        target = self._resolve(path)
        if not target.exists():
            raise MissingInputError("romfs: no such path %r" % path)
        rel = check_path(path)
        if target.is_dir():
            return RomFsEntry(rel, True)
        return RomFsEntry(rel, False, target.stat().st_size)

    def summary(self) -> dict:
        return {
            "kind": self.kind,
            "root": str(self.root),
            "files": self._files,
            "directories": self._dirs,
            "bytes": self._bytes,
        }


@dataclass
class IvfcLevel:
    offset: int
    size: int
    block_size: int


@dataclass
class _DirEntry:
    parent: int
    sibling: int
    child: int
    file_offset: int
    hash_offset: int
    name: str


@dataclass
class _FileEntry:
    parent: int
    sibling: int
    data_offset: int
    data_size: int
    hash_offset: int
    name: str


class ContainerRomFs(RomFs):
    kind = "container"

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self._data: bytes | None = None
        raw = self.path.read_bytes()
        self._raw = raw
        if len(raw) < ROMFS_HEADER_SIZE + 0xA0:
            raise FormatError("%s: too small for a RomFS header" % self.path)
        (self.header_size, self.dir_hash_off, self.dir_hash_size,
         self.dir_meta_off, self.dir_meta_size,
         self.file_hash_off, self.file_hash_size,
         self.file_meta_off, self.file_meta_size,
         self.data_off) = struct.unpack_from("<10Q", raw, 0)
        if self.header_size != ROMFS_HEADER_SIZE:
            raise FormatError("%s: unexpected RomFS header size 0x%x" % (self.path, self.header_size))
        if raw[0x50:0x54] != IVFC_MAGIC:
            raise FormatError("%s: missing IVFC header at 0x50" % self.path)
        version, master_hash_size, num_levels = struct.unpack_from("<III", raw, 0x54)
        if num_levels != 6:
            raise FormatError("%s: IVFC has %d levels (expected 6)" % (self.path, num_levels))
        self.ivfc_version = version
        self.master_hash_size = master_hash_size
        self.levels = []
        for i in range(6):
            off, size, block_size, _res = struct.unpack_from("<QQII", raw, 0x60 + i * 0x18)
            self.levels.append(IvfcLevel(off, size, block_size))
        for name, off, size in (
            ("dir_meta", self.dir_meta_off, self.dir_meta_size),
            ("file_meta", self.file_meta_off, self.file_meta_size),
            ("data", self.data_off, len(raw) - self.data_off),
        ):
            if off + size > len(raw):
                raise FormatError("%s: RomFS %s region past EOF" % (self.path, name))
        self._dir_cache: dict[int, _DirEntry] = {}
        self._file_cache: dict[int, _FileEntry] = {}
        self._by_path: dict[str, RomFsEntry] = {}
        self._build_index()

    # -- metadata ------------------------------------------------------
    def _dir_entry(self, off: int) -> _DirEntry:
        if off in self._dir_cache:
            return self._dir_cache[off]
        base = self.dir_meta_off + off
        if off == 0:
            base = self.dir_meta_off
        if base + 0x18 > self.dir_meta_off + self.dir_meta_size or base < self.dir_meta_off:
            raise FormatError("%s: directory entry 0x%x outside metadata" % (self.path, off))
        parent, sibling, child, file_off, hash_off, name_size = struct.unpack_from("<6I", self._raw, base)
        if base + 0x18 + name_size > len(self._raw):
            raise FormatError("%s: directory name past EOF" % self.path)
        name = self._raw[base + 0x18:base + 0x18 + name_size].decode("utf-8", "replace")
        entry = _DirEntry(parent, sibling, child, file_off, hash_off, name)
        self._dir_cache[off] = entry
        return entry

    def _file_entry(self, off: int) -> _FileEntry:
        if off in self._file_cache:
            return self._file_cache[off]
        base = self.file_meta_off + off
        if base + 0x20 > self.file_meta_off + self.file_meta_size:
            raise FormatError("%s: file entry 0x%x outside metadata" % (self.path, off))
        parent, sibling, data_off, data_size, hash_off, name_size = struct.unpack_from("<IIQQII", self._raw, base)
        if base + 0x20 + name_size > len(self._raw):
            raise FormatError("%s: file name past EOF" % self.path)
        name = self._raw[base + 0x20:base + 0x20 + name_size].decode("utf-8", "replace")
        if self.data_off + data_off + data_size > len(self._raw):
            raise FormatError("%s: file %r data past EOF" % (self.path, name))
        entry = _FileEntry(parent, sibling, data_off, data_size, hash_off, name)
        self._file_cache[off] = entry
        return entry

    def _build_index(self) -> None:
        root = self._dir_entry(0)
        self._walk_dir(root, "", 0)
        self._count_files = sum(1 for e in self._by_path.values() if not e.is_dir)
        self._count_dirs = sum(1 for e in self._by_path.values() if e.is_dir)
        self._total_bytes = sum(e.size for e in self._by_path.values() if not e.is_dir)

    def _walk_dir(self, root: _DirEntry, prefix: str, depth: int) -> None:
        if depth > 64:
            raise FormatError("%s: directory nesting deeper than 64 levels" % self.path)
        # Children of this directory.
        child_off = root.child
        seen: set[int] = set()
        while child_off != NO_ENTRY:
            if child_off in seen:
                raise FormatError("%s: directory metadata cycle" % self.path)
            seen.add(child_off)
            child = self._dir_entry(child_off)
            path = child.name if not prefix else prefix + "/" + child.name
            self._by_path[path] = RomFsEntry(path, True)
            self._walk_dir(child, path, depth + 1)
            child_off = child.sibling
        # Files of this directory.
        file_off = root.file_offset
        seen_files: set[int] = set()
        while file_off != NO_ENTRY:
            if file_off in seen_files:
                raise FormatError("%s: file metadata cycle" % self.path)
            seen_files.add(file_off)
            f = self._file_entry(file_off)
            path = f.name if not prefix else prefix + "/" + f.name
            self._by_path[path] = RomFsEntry(path, False, f.data_size, f.data_offset)
            file_off = f.sibling

    # -- interface -----------------------------------------------------
    def list(self, path: str) -> list[RomFsEntry]:
        base = check_path(path)
        if base and base not in self._by_path:
            raise MissingInputError("romfs: no such path %r" % path)
        if base and not self._by_path[base].is_dir:
            raise FormatError("romfs: not a directory: %r" % path)
        prefix = base + "/" if base else ""
        out = []
        for p, entry in self._by_path.items():
            if not p.startswith(prefix):
                continue
            rest = p[len(prefix):]
            if "/" not in rest:
                out.append(entry)
        out.sort(key=lambda e: (not e.is_dir, e.path))
        return out

    def _data(self) -> bytes:
        return self._raw

    def read(self, path: str) -> bytes:
        key = check_path(path)
        entry = self._by_path.get(key)
        if entry is None:
            raise MissingInputError("romfs: no such file %r" % path)
        if entry.is_dir:
            raise FormatError("romfs: %r is a directory" % path)
        start = self.data_off + entry.data_offset
        return self._raw[start:start + entry.size]

    def open(self, path: str) -> BinaryIO:
        import io
        return io.BytesIO(self.read(path))

    def exists(self, path: str) -> bool:
        try:
            return check_path(path) in self._by_path or check_path(path) == ""
        except FormatError:
            return False

    def stat(self, path: str) -> RomFsEntry:
        key = check_path(path)
        if not key:
            return RomFsEntry("", True)
        entry = self._by_path.get(key)
        if entry is None:
            raise MissingInputError("romfs: no such path %r" % path)
        return entry

    def summary(self) -> dict:
        return {
            "kind": self.kind,
            "path": str(self.path),
            "files": self._count_files,
            "directories": self._count_dirs,
            "bytes": self._total_bytes,
            "ivfc_version": hex(self.ivfc_version),
            "levels": [
                {"offset": lv.offset, "size": lv.size, "block_size": lv.block_size}
                for lv in self.levels
            ],
        }


def open_romfs(path: str | Path) -> RomFs:
    path = Path(path)
    if not path.exists():
        raise MissingInputError("RomFS path does not exist: %s" % path)
    if path.is_dir():
        return DirectoryRomFs(path)
    raw = b""
    with path.open("rb") as f:
        raw = f.read(0x100)
    if raw[0x50:0x54] == IVFC_MAGIC:
        return ContainerRomFs(path)
    raise FormatError("%s: unrecognised RomFS input" % path)


def build_romfs(files: dict[str, bytes]) -> bytes:
    """Build a minimal valid RomFS container (used by tests and fixtures)."""
    # Group paths into a directory tree.
    class Node:
        __slots__ = ("name", "dirs", "files")

        def __init__(self) -> None:
            self.name = ""
            self.dirs: dict[str, "Node"] = {}
            self.files: dict[str, bytes] = {}

    root = Node()
    for path, data in files.items():
        clean = check_path(path)
        if not clean:
            continue
        parts = clean.split("/")
        node = root
        for part in parts[:-1]:
            node = node.dirs.setdefault(part, Node())
            node.name = part
        node.files[parts[-1]] = data

    dir_meta = bytearray()
    file_meta = bytearray()
    data_region = bytearray()
    dir_offsets: dict[int, int] = {}

    def emit_dir(node: Node, parent: int) -> int:
        off = len(dir_meta)
        dir_offsets[id(node)] = off
        name_bytes = node.name.encode()
        dir_meta.extend(struct.pack("<IIIIII", parent, NO_ENTRY, NO_ENTRY, NO_ENTRY, 0, len(name_bytes)))
        dir_meta.extend(name_bytes)
        dir_meta.extend(b"\0" * ((4 - len(dir_meta) % 4) % 4))
        child_offsets = []
        for name in sorted(node.dirs):
            child_offsets.append((name, emit_dir(node.dirs[name], off)))
        file_offsets = []
        for name in sorted(node.files):
            foff = len(file_meta)
            payload = node.files[name]
            data_off = len(data_region)
            data_region.extend(payload)
            data_region.extend(b"\0" * ((0x10 - len(data_region) % 0x10) % 0x10))
            fname = name.encode()
            file_meta.extend(struct.pack("<IIQQII", off, NO_ENTRY, data_off, len(payload), 0, len(fname)) + fname)
            file_meta.extend(b"\0" * ((4 - len(file_meta) % 4) % 4))
            file_offsets.append(foff)
        # link siblings
        for i in range(len(file_offsets)):
            sib = file_offsets[i + 1] if i + 1 < len(file_offsets) else NO_ENTRY
            struct.pack_into("<I", file_meta, file_offsets[i] + 4, sib)
        child_offsets.sort()
        for i in range(len(child_offsets)):
            sib = child_offsets[i + 1][1] if i + 1 < len(child_offsets) else NO_ENTRY
            struct.pack_into("<I", dir_meta, child_offsets[i][1] + 4, sib)
        child_first = child_offsets[0][1] if child_offsets else NO_ENTRY
        file_first = file_offsets[0] if file_offsets else NO_ENTRY
        struct.pack_into("<II", dir_meta, off + 8, child_first, file_first)
        return off

    root.name = ""
    emit_dir(root, 0)
    # metadata tables are addressed relative to their table start, and the root
    # entry must live at offset 0 - emit_dir emits it first, so this holds.

    dir_hash_size = 0
    file_hash_size = 0
    data_offset = 0x1000
    level_offsets = [0x1000, 0x1000, 0x1000, 0x1000, 0x1000, data_offset]
    dir_meta_off = level_offsets[3]
    file_meta_off = dir_meta_off + len(dir_meta)
    file_meta_off += (0x1000 - file_meta_off % 0x1000) % 0x1000
    data_offset = file_meta_off + len(file_meta)
    data_offset += (0x1000 - data_offset % 0x1000) % 0x1000

    header = struct.pack("<10Q", ROMFS_HEADER_SIZE, 0x200, dir_hash_size,
                         dir_meta_off, len(dir_meta), 0x300, file_hash_size,
                         file_meta_off, len(file_meta), data_offset)
    ivfc = bytearray()
    ivfc += struct.pack("<4sIII", IVFC_MAGIC, 0x20000, 0x20, 6)
    for i in range(6):
        ivfc += struct.pack("<QQII", level_offsets[i], 0, 0, 0)
    out = bytearray(data_offset + len(data_region))
    out[:len(header)] = header
    out[0x50:0x50 + len(ivfc)] = ivfc
    out[dir_meta_off:dir_meta_off + len(dir_meta)] = dir_meta
    out[file_meta_off:file_meta_off + len(file_meta)] = file_meta
    out[data_offset:data_offset + len(data_region)] = data_region
    return bytes(out)
