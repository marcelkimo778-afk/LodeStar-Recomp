"""NPDM (META/ACI0/ACID) metadata parsing.

NPDMs describe a program: title id range, program id, filesystem access, service
access control and kernel capabilities.  The recompiler needs the program id and
name for diagnostics and the filesystem mount policy; the ACID signature is
reported but not validated (the console does that with console-unique keys, which
this tool intentionally does not handle).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path

from ..errors import FormatError

META_MAGIC = b"META"
ACI0_MAGIC = b"ACI0"
ACID_MAGIC = b"ACID"

FLAG_MASK = {
    0x1: "PROGRAM",
    0x2: "CODE_GENERATION",
    0x4: "SYSTEM",
    0x8: "APPLICATION",
    0x10: "APPLET",
}


@dataclass
class Aci0:
    offset: int
    program_id: int
    fs_access_header_off: int = 0
    fs_access_header_size: int = 0
    sacs_off: int = 0
    sacs_size: int = 0
    kcaps_off: int = 0
    kcaps_size: int = 0
    name: str = ""
    service_names: list[str] = field(default_factory=list)


@dataclass
class Npdm:
    path: Path
    signature_key_generation: int
    flags: int
    title_id_min: int
    title_id_max: int
    system_resource_size: int
    version: int
    aci0_offset: int
    aci0_size: int
    acid_offset: int
    acid_size: int
    aci0: Aci0 | None = None

    @property
    def flag_names(self) -> list[str]:
        return [name for bit, name in FLAG_MASK.items() if self.flags & bit]

    def summary(self) -> dict:
        return {
            "path": str(self.path),
            "signature_key_generation": self.signature_key_generation,
            "flags": self.flags,
            "flag_names": self.flag_names,
            "title_id_min": "%016x" % self.title_id_min,
            "title_id_max": "%016x" % self.title_id_max,
            "system_resource_size": self.system_resource_size,
            "version": self.version,
            "aci0_offset": self.aci0_offset,
            "aci0_size": self.aci0_size,
            "acid_offset": self.acid_offset,
            "acid_size": self.acid_size,
            "program_id": None if self.aci0 is None else "%016x" % self.aci0.program_id,
            "name": None if self.aci0 is None else self.aci0.name,
            "services": [] if self.aci0 is None else self.aci0.service_names,
        }


def _cstr(raw: bytes, off: int, limit: int) -> str:
    end = raw.find(b"\0", off, off + limit)
    if end < 0:
        end = min(off + limit, len(raw))
    return raw[off:end].decode("utf-8", "replace")


def parse_npdm(path: str | Path) -> Npdm:
    path = Path(path)
    raw = path.read_bytes()
    if len(raw) < 0x80:
        raise FormatError("%s: too small for META header" % path)
    if raw[:4] != META_MAGIC:
        raise FormatError("%s: bad magic %r (expected META)" % (path, raw[:4]))
    keygen, = struct.unpack_from("<I", raw, 0x04)
    flags = raw[0x0F]
    title_min, title_max = struct.unpack_from("<QQ", raw, 0x10)
    sysres, version = struct.unpack_from("<II", raw, 0x28)
    aci0_off, aci0_size, acid_off, acid_size = struct.unpack_from("<IIII", raw, 0x70)
    if aci0_off + aci0_size > len(raw) or aci0_size < 0x40:
        raise FormatError("%s: ACI0 outside file" % path)
    if raw[aci0_off:aci0_off + 4] != ACI0_MAGIC:
        raise FormatError("%s: bad ACI0 magic" % path)
    # ACID carries a 0x100-byte RSA signature before its header magic.
    acid_ok = (acid_off + acid_size <= len(raw)
               and raw[acid_off + 0x100:acid_off + 0x104] == ACID_MAGIC)

    # ACI0: version patch, program id at +0x10, then four (off,size) sections.
    program_id, = struct.unpack_from("<Q", raw, aci0_off + 0x10)
    fs_off, fs_size, sacs_off, sacs_size, kc_off, kc_size = struct.unpack_from("<6I", raw, aci0_off + 0x18)
    aci = Aci0(offset=aci0_off, program_id=program_id,
               fs_access_header_off=fs_off, fs_access_header_size=fs_size,
               sacs_off=sacs_off, sacs_size=sacs_size,
               kcaps_off=kc_off, kcaps_size=kc_size)
    name_off = aci0_off + 0x40
    aci.name = _cstr(raw, name_off, 0x10)

    if sacs_off and sacs_size and aci0_off + sacs_off + sacs_size <= len(raw):
        base = aci0_off + sacs_off
        count, = struct.unpack_from("<I", raw, base)
        pos = base + 4
        for _ in range(count):
            if pos + 8 > len(raw):
                break
            _, caps = struct.unpack_from("<II", raw, pos)
            pos += 8
            # The NintendoSDK service name is stored as a fixed 8-byte field.
            field_raw = raw[pos:pos + 8] if pos + 8 <= len(raw) else b""
            pos += 8
            name = field_raw.split(b"\0")[0].decode("ascii", "replace") if field_raw else ""
            if name:
                aci.service_names.append(name)

    if not acid_ok:
        # Older/homebrew metadata may omit the ACID block; report but continue.
        pass

    return Npdm(
        path=path,
        signature_key_generation=keygen,
        flags=flags,
        title_id_min=title_min,
        title_id_max=title_max,
        system_resource_size=sysres,
        version=version,
        aci0_offset=aci0_off,
        aci0_size=aci0_size,
        acid_offset=acid_off,
        acid_size=acid_size,
        aci0=aci,
    )
