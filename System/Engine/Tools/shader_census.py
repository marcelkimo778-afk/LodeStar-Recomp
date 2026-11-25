#!/usr/bin/env python3
"""Shader census of the player's own RomFS: every NVN shader program embedded in the game's effect files (.ptcl) and
archives (.arc), as default-state variants for `sw_app.exe --prepare-shaders`.

    python Engine/Tools/shader_census.py [--romfs DIR] [--out DIR]

Both file kinds embed BNSH shader containers (NintendoWare); the BNSH walk is adapted from totk-pc-recomp
tools/shader_census.py (bnsh_shaders, MIT; itself from MacNativeGames' prepare_shader_seed.py), the record layouts
from its prepare_shaders.py and Engine/Runtime/gfx/vk/gfx_shader.c ("answers met in play"). Writes into the shader
cache folder (default build_work/out/shader-cache):

    census-programs.bin  "TOTKSPRG": stage, length, c1 length, code hash, control section, code, c1 (one per program)
    census-answers.bin   "TOTKSANS": stage, code hash, zero input id, default answers, priority 2 (one per program)
    census.json          counts and the RomFS it came from

Coverage: one variant per program, for the default bind state (every texture slot a plain 2D texture, float vertex
inputs and colour outputs). Play can need other variants of the same program (other texture kinds, formats); those
come from the recording in play. Shaders in other files (.gfpak archives are compressed) are not found. Game data
stays in the player's install (the cache folder); nothing here is distributed.
Progress lines for the launcher: "[census] scanning N/M files" and "[census] done ...".
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONTROL = 0x880
CONTROL_MAGIC = b'\x34\x12\x76\x98'
RUNTIME_STAGE = {'vs': 0, 'ps': 4, 'cs': 5}          # gfx.h GFX_STAGE_*
NVN_STAGE = {'vs': 0, 'ps': 1, 'cs': 5}              # control+0x714
ANS_PACKED = 147
ANS_REC = 4 + 32 + 32 + ANS_PACKED + 1
FORMAT = 1


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('range outside container')
    return data[offset:offset + size]


def bnsh_shaders(data):
    """(stage, control[0x880], code[:length]) of every program of the BNSH at data[0]."""
    if data[:4] != b'BNSH' or checked(data, 12, 2) != b'\xff\xfe' or checked(data, 96, 4) != b'grsc':
        raise ValueError('unsupported BNSH header')
    size = struct.unpack_from('<I', data, 28)[0]
    data = checked(data, 0, size)
    count, variations = struct.unpack_from('<IQ', data, 124)
    checked(data, variations, count * 64)
    for i in range(count):
        program = struct.unpack_from('<Q', data, variations + i * 64 + 16)[0]
        checked(data, program, 56)
        for stage, relative in (('vs', 8), ('ps', 40), ('cs', 48)):
            offset = struct.unpack_from('<Q', data, program + relative)[0]
            if not offset:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from('<QQII', checked(data, offset + 8, 24))
            control = checked(data, control_off, control_size)
            code = checked(data, code_off, code_size)
            if len(control) < CONTROL or control[:4] != CONTROL_MAGIC:
                continue                                  # older SDK layouts: not what NVN gets at run time
            control = control[:CONTROL]
            c1, length = struct.unpack_from('<II', control, 0x700)
            if struct.unpack_from('<I', control, 0x714)[0] != NVN_STAGE[stage] or not 0 <= c1 <= length <= len(code) \
                    or not length:
                continue
            yield stage, control, code[:length]


def code_hash(stage, control, code, length, c1):
    """gfx_shader.c build_code_hash, with the port's transform-feedback mark on capturing vertex shaders."""
    word = stage | (0x58460000 if stage == 0 and control[0x6ed] == 1 else 0)
    return hashlib.sha256(struct.pack('<I', word) + control + struct.pack('<Q', length) + code[:length] +
                          struct.pack('<I', len(c1)) + c1).digest()


def default_answers(stage, control):
    """pack_answers layout: every texture slot 2D (kind 4), no images, float inputs/outputs (0)."""
    tex = bytes([4]) * 32
    local = [0, 0, 0]
    if stage == 5:
        local = [v or 1 for v in struct.unpack_from('<III', control, 0x748)]
    packed = tex + bytes(32) + bytes(32) + bytes(32) + bytes(8) + struct.pack('<HHHIB', *local, 0, 0)
    assert len(packed) == ANS_PACKED
    return packed


def romfs_default():
    try:
        user = ROOT.parent           # the folder with Game/ and Settings/ (ROOT = System/)
        cfg = json.loads((user / 'Settings' / 'config.json').read_text(encoding='utf-8'))
        r = cfg.get('romfs_dir') or ''
        cand = (user / 'Game' / 'romfs') if r in ('', '.') else (user / r)
        if cand.is_dir():
            return cand
    except (OSError, ValueError):
        pass
    return ROOT.parent / 'Game' / 'romfs'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--romfs', type=Path, default=None)
    ap.add_argument('--out', type=Path, default=ROOT / 'build_work' / 'out' / 'shader-cache')
    a = ap.parse_args()
    romfs = a.romfs or romfs_default()
    if not romfs.is_dir():
        print(f'[census] error: no RomFS at {romfs}', flush=True)
        return 1
    files = sorted(p for p in romfs.rglob('*') if p.is_file() and p.suffix.lower() in ('.ptcl', '.arc'))
    t0 = time.monotonic()
    progs, bad, containers = {}, 0, 0
    for i, p in enumerate(files):
        if i % 100 == 0:
            print(f'[census] scanning {i}/{len(files)} files, {len(progs)} shaders', flush=True)
        try:
            data = p.read_bytes()
        except OSError:
            bad += 1
            continue
        at = data.find(b'BNSH')
        while at >= 0:
            try:
                for stage, control, code in bnsh_shaders(memoryview(data)[at:].tobytes()):
                    st = RUNTIME_STAGE[stage]
                    c1_off, length = struct.unpack_from('<II', control, 0x700)
                    c1 = code[c1_off:length]
                    h = code_hash(st, control, code, length, c1)
                    if h not in progs:
                        progs[h] = (st, control, code, length, c1)
                containers += 1
            except (ValueError, struct.error):
                bad += 1
            at = data.find(b'BNSH', at + 4)
    a.out.mkdir(parents=True, exist_ok=True)
    stages = {0: 0, 4: 0, 5: 0}
    prg = bytearray(b'TOTKSPRG' + struct.pack('<II', 1, 0))
    ans = bytearray(b'TOTKSANS' + struct.pack('<II', 1, ANS_REC))
    for h, (st, control, code, length, c1) in progs.items():
        prg += struct.pack('<III', st, length, len(c1)) + h + control + code[:length] + c1
        rec = struct.pack('<I', st) + h + bytes(32) + default_answers(st, control) + bytes([2])
        assert len(rec) == ANS_REC
        ans += rec
        stages[st] += 1
    # written beside the final names, then renamed: a killed census never leaves a half file under the real name
    for name, body in (('census-programs.bin', prg), ('census-answers.bin', ans)):
        tmp = a.out / (name + '.tmp')
        tmp.write_bytes(body)
        os.replace(tmp, a.out / name)
    info = {'format': FORMAT, 'romfs': str(romfs), 'files': len(files), 'containers': containers,
            'unreadable': bad, 'programs': len(progs), 'vertex': stages[0], 'fragment': stages[4],
            'compute': stages[5], 'variants': len(progs), 'coverage': 'default bind state of shaders in .ptcl/.arc',
            'seconds': round(time.monotonic() - t0, 1)}
    tmp = a.out / 'census.json.tmp'
    tmp.write_text(json.dumps(info, indent=1), encoding='utf-8')
    os.replace(tmp, a.out / 'census.json')
    print(f'[census] done: {len(progs)} shader programs (vertex {stages[0]}, fragment {stages[4]}, compute '
          f'{stages[5]}) from {containers} containers in {len(files)} files, {bad} unreadable, '
          f'{info["seconds"]} s', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
