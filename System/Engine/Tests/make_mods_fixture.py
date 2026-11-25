"""One-off generator for Engine/Tests/mods_fixture (the files are committed; this only wrote them)."""
import os, struct, sys

root = sys.argv[1]
TID = "01001F5010DFA000"
BID = "AABBCCDD11223344556677889900AABBCCDDEEFF"


def w(rel, data):
    p = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    if isinstance(data, str):
        data = data.encode()
    with open(p, "wb") as f:
        f.write(data)


def ips(records, ips32=False):
    out = bytearray(b"IPS32" if ips32 else b"PATCH")
    for off, payload in records:
        out += off.to_bytes(4 if ips32 else 3, "big")
        if isinstance(payload, tuple):                  # RLE (count, value)
            out += b"\0\0" + payload[0].to_bytes(2, "big") + bytes([payload[1]])
        else:
            out += len(payload).to_bytes(2, "big") + payload
    out += b"EEOF" if ips32 else b"EOF"
    return bytes(out)


# base game RomFS (raw layout: bin/ + system_resource/)
w("base_romfs/bin/field/a.bin", "BASE_A")
w("base_romfs/bin/field/b.bin", "BASE_B")
w("base_romfs/bin/data/c.bin", "BASE_C")
w("base_romfs/bin/data/list/x.txt", "BASE_X")
w("base_romfs/system_resource/shader.bin", "SHADER")

M = "mods"
# LayeredFS style <TitleID>/<Mod>
A = f"{M}/{TID}/ModAlpha"
w(f"{A}/romfs/bin/field/a.bin", "ALPHA_A_REPLACED_LONGER")       # replacement with another size
w(f"{A}/romfs/bin/field/new_alpha.bin", "NEW_ALPHA")             # new file
w(f"{A}/romfs/bin/data/list/y.txt", "ALPHA_Y")                   # directory listing merge
w(f"{A}/romfs/bin/newdir/deep/z.bin", "ALPHA_Z")                 # new folders
w(f"{A}/romfs/bin/data/c.bin", "ALPHA_C")                        # conflict (ModBeta has priority)
w(f"{A}/romfs/field/misplaced.bin", "WRONG_LAYOUT")              # not under bin/: warned
w(f"{A}/exefs/{BID}.ips", ips([
    (0x100 + 0x1000, b"RODA"),                                    # .rodata: applied
    (0x100 + 0x1010, b"AAAA"),                                    # overlaps ModBeta's patch: Beta wins
    (0x100 + 0x2000, (8, 0x5A)),                                  # .data, RLE: applied
    (0x100 + 0x10, bytes.fromhex("1F2003D5")),                    # .text: warned + skipped
]))
w(f"{A}/exefs/AABBCCDD11223344.ips32", ips([(0x100 + 0x1100, b"IP32")], ips32=True))  # touches the "fps table"
w(f"{A}/exefs/0123456789ABCDEF0123456789ABCDEF01234567.ips", ips([(0x100 + 0x1200, b"XXXX")]))  # other build

# flat style <Mod>
B = f"{M}/ModBeta"
w(f"{B}/romfs/bin/data/c.bin", "BETA_C_WINS")
w(f"{B}/romfs/Bin/Field/B.BIN", "BETA_B_CASE")                    # other upper/lower case than the game
w(f"{B}/romfs/bin/field/empty.bin", b"")                          # empty new file: ignored
w(f"{B}/romfs/bin/data/list/x.txt", b"")                          # empty replacement: original kept
w(f"{B}/exefs/fps60.pchtxt", f"""@nsobid-{BID}
@flag offset_shift 0x100

// 60 FPS
@enabled
00000800 80078052

// Data tweak
@enabled
00001010 DEADBEEF
00001020 "hi"

// Disabled tweak
@disabled
00001030 CAFEBABE
@stop
00001040 CAFEBABE
""")
w(f"{B}/cheats/AABBCCDD11223344.txt", """{Master}
04100000 00000050 00000007

[Set Money]
04100000 00000010 0001869F

[Code Cheat]
04000000 00000800 12345678

[Loop Fill]
400F0000 00000000 80000100
30000000 00000004
640F1000 00000000 000000AB
31000000

[Key Cheat]
80000001
04100000 00000020 00000001
20000000

[Cond]
14150000 00000060 00000005
04100000 00000030 00000001
21000000
04100000 00000030 00000002
20000000

[Not Enabled]
04100000 00000040 00000001

[Broken]
E0000000
""")

# disabled by a marker file
w(f"{M}/ModOff/romfs/bin/field/off.bin", "OFF")
w(f"{M}/ModOff/.disabled", "")
# yuzu / Ryujinx / Atmosphere layouts
w(f"{M}/load/{TID}/ModYuzu/romfs/bin/field/yuzu.bin", "YUZU")
w(f"{M}/mods/contents/{TID}/ModRyu/romfs/bin/field/ryu.bin", "RYU")
w(f"{M}/atmosphere/contents/{TID}/romfs/bin/field/atmo.bin", "ATMO")
w(f"{M}/atmosphere/exefs_patches/PatchSet/{BID}.ips", ips([(0x100 + 0x2010, b"PSET")]))
# another game's mod, and a folder that is not a mod
w(f"{M}/0100000000010000/OtherGame/romfs/bin/field/other.bin", "OTHER")
w(f"{M}/readme_folder/notes.txt", "not a mod")
# for the junction test (test_mods.bat links ModBeta/romfs/bin/escape -> outside)
w("outside/secret.bin", "SECRET")

w("mods.json", """{
  // Engine/Tests/test_mods.c config
  "enabled": true,
  "search_default_dirs": false,
  "mods": [
    "ModBeta",
    {"name": "ModAlpha", "enabled": true},
    {"name": "ModOff", "enabled": true}
  ],
  "cheats": {"ModBeta": ["Set Money", "Code Cheat", "Loop Fill", "Key Cheat", "Cond", "Broken"]}
}
""")
# a broken config (test_mods badcfg): reported, ignored, mods load with the defaults (priority by name)
w("broken.json", '{\n  "enabled": true,\n  "mods": ["ModBeta",\n')
print("fixture written to", root)
