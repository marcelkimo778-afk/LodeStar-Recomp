"""Writes Engine/Tests/modbrowser_fixture: the mock mod source (index.json, images/, files/) for
test_modbrowser.bat and for trying the in-game browser offline (SW_MB_MOCK=<this folder>).

Test target build id: AABBCCDD11223344556677889900AABBCCDDEEFF (same as mods_fixture), version 1.0.0.
Every archive here is made up: nothing is game content."""
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import zipfile

root = sys.argv[1]
BID = "AABBCCDD11223344556677889900AABBCCDDEEFF"
OTHER = "0123456789ABCDEF0123456789ABCDEF01234567"
F = os.path.join(root, "files")
I = os.path.join(root, "images")
os.makedirs(F, exist_ok=True)
os.makedirs(I, exist_ok=True)


def zf(name, entries, symlink=None):
    p = os.path.join(F, name)
    with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
        for n, data in entries:
            z.writestr(n, data)
        if symlink:
            info = zipfile.ZipInfo(symlink[0])
            info.create_system = 3
            info.external_attr = (0o120777 << 16)
            z.writestr(info, symlink[1])
    return p


def png(name, rgb):
    try:
        from PIL import Image
        Image.new("RGB", (320, 180), rgb).save(os.path.join(I, name))
    except ImportError:                           # a 1x1 PNG
        import base64
        open(os.path.join(I, name), "wb").write(base64.b64decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="))


PCH_OK = f"@nsobid-{BID}\n@flag offset_shift 0x100\n\n// data tweak\n@enabled\n00001010 DEADBEEF\n@stop\n"
zf("good_root.zip", [("romfs/bin/field/mock_a.bin", "MOCK_A_V1"), ("readme.txt", "Shiny textures by Tester")])
zf("good_root_v2.zip", [("romfs/bin/field/mock_a.bin", "MOCK_A_V2"), ("romfs/bin/field/mock_new.bin", "NEW_IN_V2")])
zf("nested.zip", [("Cool Mod v2/romfs/bin/field/cool.bin", "COOL"), ("Cool Mod v2/notes.md", "# notes")])
zf("tid.zip", [(f"01001F5010DFA000/romfs/bin/field/tid.bin", "TID"), ("01001F5010DFA000/exefs/fps.pchtxt", PCH_OK)])
zf("bare_bin.zip", [("bin/field/bare.bin", "BARE")])
zf("slip.zip", [("romfs/bin/ok.bin", "OK"), ("../evil.txt", "EVIL")])
zf("bomb.zip", [("romfs/bin/zero.bin", b"\0" * (64 << 20))])
zf("exe_inside.zip", [("romfs/bin/field/exe_mod.bin", "EXEMOD"), ("tools/installer.exe", "MZ fake"), ("run.bat", "@echo hi"),
                      ("romfs/bin/field/payload.dll", "MZ fake")])
zf("mixed_ok.zip", [("romfs/bin/field/mixed.bin", "MIXED"), ("exefs/tweak.pchtxt", PCH_OK)])
zf("wrong_bid.zip", [("romfs/bin/field/wrong.bin", "WRONG"), (f"exefs/{OTHER}.ips", b"PATCH" + bytes([0, 1, 0x10, 0, 2]) + b"AB" + b"EOF")])
zf("cheats_ok.zip", [(f"cheats/{BID[:16]}.txt", "[Money]\n04100000 00000010 0001869F\n")])
zf("variants.zip", [("Option A/romfs/bin/field/v.bin", "A"), ("Option B/romfs/bin/field/v.bin", "B")])
zf("symlink.zip", [("romfs/bin/field/s.bin", "S")], symlink=("romfs/bin/field/link.bin", "../../../../etc/passwd"))
zf("nothing.zip", [("readme.txt", "no mod in here")])
zf("hash_bad.zip", [("romfs/bin/field/h.bin", "HASH")])
# 7z through Windows' bsdtar (libarchive writes 7-Zip); skipped where it is not available
seven = None
tar = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "tar.exe")
if os.path.isfile(tar):
    src = os.path.join(root, "_7zsrc")
    os.makedirs(os.path.join(src, "romfs", "bin", "field"), exist_ok=True)
    open(os.path.join(src, "romfs", "bin", "field", "seven.bin"), "w").write("SEVEN")
    out = os.path.join(F, "seven.7z")
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run([tar, "--format", "7zip", "-cf", out, "-C", src, "romfs"], capture_output=True)
    shutil.rmtree(src)
    seven = "seven.7z" if r.returncode == 0 and os.path.isfile(out) else None

for n, c in (("t1.png", (200, 150, 60)), ("t2.png", (60, 120, 200)), ("t3.png", (120, 60, 160)), ("s1.png", (30, 30, 30))):
    png(n, c)


def md5(name):
    return hashlib.md5(open(os.path.join(F, name), "rb").read()).hexdigest()


def mod(i, name, files, **kw):
    m = {"id": str(i), "name": name, "author": kw.get("author", "Tester"), "version": kw.get("version", "1.0"),
         "category": kw.get("category", "Textures"), "summary": kw.get("summary", name + " (mock)"),
         "description": kw.get("description", "A mock mod for the offline tests."), "changelog": kw.get("changelog", ""),
         "downloads": kw.get("downloads", 100 * i), "likes": kw.get("likes", 10 * i), "added": i, "updated": 1000 + i,
         "game_version": kw.get("game_version", ""), "thumb": "images/t%d.png" % (1 + i % 3),
         "images": ["images/t%d.png" % (1 + i % 3), "images/s1.png"], "files": []}
    for f in files:
        fe = {"id": "f%d_%s" % (i, f[0].split(".")[0]), "name": f[0], "file": "files/" + f[0], "version": f[1] if len(f) > 1 else m["version"],
              "md5": md5(f[0])}
        if len(f) > 2:
            fe.update(f[2])
        m["files"].append(fe)
    return m


mods = [
    mod(1, "Shiny Textures", [("good_root.zip", "1.0")], game_version="1.0.0", likes=500, downloads=9000),
    mod(2, "Cool Mod", [("nested.zip",)], description="Made for game version 1.1.1 only.", category="Models"),
    mod(3, "Title Folder Mod", [("tid.zip",)], category="Gameplay"),
    mod(4, "Bare Bin Mod", [("bare_bin.zip",)]),
    mod(5, "Zip Slip", [("slip.zip",)], category="Bad"),
    mod(6, "Zip Bomb", [("bomb.zip",)], category="Bad"),
    mod(7, "Has An Exe", [("exe_inside.zip",)], category="Bad"),
    mod(8, "Mixed Romfs And Patch", [("mixed_ok.zip",)], category="Gameplay"),
    mod(9, "Wrong Build Patch", [("wrong_bid.zip",)], category="Gameplay"),
    mod(10, "Money Cheats", [("cheats_ok.zip",)], category="Cheats"),
    mod(11, "Two Variants", [("variants.zip",)]),
    mod(12, "Symlink Mod", [("symlink.zip",)], category="Bad"),
    mod(13, "Empty Mod", [("nothing.zip",)], category="Bad"),
    mod(14, "Bad Hash", [("hash_bad.zip", "1.0", {"md5": "0" * 32})], category="Bad"),
]
if seven:
    mods.append(mod(15, "Seven Zip Mod", [(seven,)]))
for k in range(16, 40):                        # enough for a few pages
    mods.append(mod(k, "Filler Mod %02d" % k, [("good_root.zip",)], category="Filler", game_version="1.0.0"))
# an update of mod 1: test_modbrowser switches the index to this one (index_v2.json)
json.dump({"mods": mods}, open(os.path.join(root, "index.json"), "w"), indent=1)
v2 = json.loads(json.dumps(mods))
v2[0]["version"] = "1.1"
v2[0]["files"] = [{"id": "f1_v2", "name": "good_root_v2.zip", "file": "files/good_root_v2.zip", "version": "1.1",
                   "md5": md5("good_root_v2.zip")}]
json.dump({"mods": v2}, open(os.path.join(root, "index_v2.json"), "w"), indent=1)
print("fixture written to", root, "(7z:", bool(seven), ")")
