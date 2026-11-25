#!/usr/bin/env python3
"""Build the Maxwell shader translator without the .NET SDK.

The translator is the Ryujinx shader compiler (MIT) with the 15 patches and the
Program.cs front end of totk-pc-recomp (MIT), both vendored under the
reference checkout.  This script

  1. copies Ryujinx.Graphics.Shader, Spv.Generator and the few Ryujinx.Common
     files the translator uses into build_work/xlate/src,
  2. applies the patches with `git apply`,
  3. compiles everything with the Roslyn compiler that ships with Visual Studio
     Build Tools (csc.exe) against the installed .NET runtime's assemblies,
  4. writes build_work/xlate/sw_xlate.dll + sw_xlate.runtimeconfig.json.

Run it with:  dotnet build_work/xlate/sw_xlate.dll --serve
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REF = Path(os.environ.get("SW_REF_TOTK", str(Path(__file__).resolve().parents[3] / "reference" / "totk-pc-recomp")))
VENDOR = REF / "tools" / "vendor" / "ryujinx"
PATCHES = REF / "tools" / "xlate" / "patches"
PROGRAM = REF / "tools" / "xlate" / "Program.cs"
OUT = ROOT / "build_work" / "xlate"
SRC = OUT / "src"
SHIM = ROOT / "Engine" / "Tools" / "xlate_shim"     # our replacements (Logger, ...)

COMMON_FILES = ["Memory/StructArrayHelpers.cs", "Utilities/BitUtils.cs",
                "Memory/IArray.cs", "Memory/MemoryOwner.cs", "Pools/ObjectPool.cs", "Utilities/BitfieldExtensions.cs"]


def find_csc() -> Path:
    vs = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio"
    for p in sorted(vs.glob("*/*/MSBuild/Current/Bin/Roslyn/csc.exe"), reverse=True):
        return p
    sys.exit("csc.exe not found (Visual Studio Build Tools with MSBuild)")


def find_runtime() -> tuple[Path, str]:
    base = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "dotnet" / "shared" / "Microsoft.NETCore.App"
    vers = sorted((p for p in base.iterdir() if p.is_dir()), key=lambda p: [int(x) for x in re.findall(r"\d+", p.name)])
    if not vers:
        sys.exit("no .NET runtime found")
    return vers[-1], vers[-1].name


def prepare() -> None:
    if SRC.exists():
        shutil.rmtree(SRC)
    SRC.mkdir(parents=True)
    for proj in ("Ryujinx.Graphics.Shader", "Spv.Generator"):
        shutil.copytree(VENDOR / "src" / proj, SRC / "src" / proj,
                        ignore=shutil.ignore_patterns("bin", "obj", "*.csproj"))
    for rel in COMMON_FILES:
        dst = SRC / "src" / "Ryujinx.Common" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(VENDOR / "src" / "Ryujinx.Common" / rel, dst)
    # SRC lies inside this project's git repository.  `git apply` run in a
    # subdirectory of a repository silently skips every path outside that
    # subdirectory - all 15 patches "applied" without changing a file.  Stop
    # the repository discovery so git treats SRC as a plain directory.
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(SRC.parent))
    for patch in sorted(PATCHES.glob("*.patch")):
        before = {p: p.read_bytes() for p in (SRC / "src").rglob("*.cs")}
        r = subprocess.run(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=SRC,
                           capture_output=True, text=True, env=env)
        if r.returncode != 0:
            sys.exit(f"patch does not apply: {patch.name}\n{r.stdout}{r.stderr}")
        changed = sum(1 for p in (SRC / "src").rglob("*.cs") if before.get(p) != p.read_bytes())
        if not changed:
            sys.exit(f"patch {patch.name} reported success but changed no file")
        print(f"applied {patch.name} ({changed} file(s))")
    shutil.copy2(PROGRAM, SRC / "Program.cs")
    our_edits()


def _sub_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if text.count(old) != 1:
        sys.exit(f"{path.name}: expected one '{old[:70]}', found {text.count(old)}")
    path.write_text(text.replace(old, new), encoding="utf-8")


def our_edits() -> None:
    """Arceus additions on top of the reference translator.

    Transform feedback (GPU skinning writes its skinned vertices to buffers):
    the job's "xfb" = {"stride": [bytes x4], "locs": [[attribute word, ...] x4]}
    (read from the NVN control section by gfx_shader.c).  The host reports no
    native transform feedback, so Ryujinx *emulates* it: the vertex shader
    stores its outputs into 4 storage buffers ("tfe_data0-3") at
    support-buffer tfe_offset[b] + vertex * stride/4 (words).  Those buffers
    go to set 0, bindings 100-103 (everything else is set 0 from binding 1
    up) and are reported in res.json "sbufs" with slot -100-b, which the
    Vulkan back end binds to the transform feedback buffers.  Works on any
    Vulkan GPU, no VK_EXT_transform_feedback needed.
    """
    shader = SRC / "src" / "Ryujinx.Graphics.Shader"
    _sub_once(shader / "Translation" / "ResourceReservations.cs",
              "                _tfeBufferSbBaseBinding = ReservedStorageBuffers;",
              "                _tfeBufferSbBaseBinding = 100;   // [arceus] set 0, out of the way of the job's bindings")
    _sub_once(shader / "Translation" / "TranslatorContext.cs",
              'new(BufferLayout.Std430, 1, binding, $"tfe_data{i}", tfeDataStruct);',
              'new(BufferLayout.Std430, 0, binding, $"tfe_data{i}", tfeDataStruct);   // [arceus] set 0')
    prog = SRC / "Program.cs"
    _sub_once(prog, "        public bool QueryHostSupportsDepthClipControl() => true;",
              "        public bool QueryHostSupportsDepthClipControl() => true;\n"
              "        // [arceus] transform feedback: emulated with storage buffers (see build_xlate.py our_edits)\n"
              "        public bool QueryHostSupportsTransformFeedback() => false;\n"
              "        public bool QueryTransformFeedbackEnabled() => _job[\"xfb\"] is JsonObject;\n"
              "        public ReadOnlySpan<byte> QueryTransformFeedbackVaryingLocations(int b)\n"
              "        {\n"
              "            if (_job[\"xfb\"] is not JsonObject x || x[\"locs\"] is not JsonArray l || b >= l.Count || l[b] is not JsonArray a) return ReadOnlySpan<byte>.Empty;\n"
              "            var r = new byte[a.Count];\n"
              "            for (int i = 0; i < a.Count; i++) r[i] = (byte)(int)a[i];\n"
              "            return r;\n"
              "        }\n"
              "        public int QueryTransformFeedbackStride(int b) =>\n"
              "            _job[\"xfb\"] is JsonObject x && x[\"stride\"] is JsonArray s && b < s.Count ? (int)s[b] : 0;")
    # Alpha test (nvnColorStateSetAlphaTest; Legends: Arceus's UI uses GREATER, its battle style cut-in dissolves
    # panels with a moving reference): the job's "alpha_func" (NVNalphaFunc = Ryujinx AlphaTestOp, 1..8) specialises
    # the fragment shader, and the reference is read from the support buffer's alpha_test word (vk_exec_draw.c
    # support_fill writes nvnCommandBufferSetAlphaRef there), so a changing reference needs no new translation.
    _sub_once(prog, "false, false, false, depthMinusW, false, 1f, AlphaTestOp.Always, 0f, attr,",
              "false, false, false, depthMinusW, false, 1f, AlphaOp(), 0f, attr,")
    _sub_once(prog, "        public GpuGraphicsState QueryGraphicsState()",
              "        // [arceus] alpha test of the fragment shader (see build_xlate.py our_edits)\n"
              "        AlphaTestOp AlphaOp() => _job[\"alpha_func\"] is JsonValue af && af.TryGetValue<int>(out int ai) && ai >= 1 && ai <= 8\n"
              "            ? (AlphaTestOp)ai : AlphaTestOp.Always;\n"
              "        public GpuGraphicsState QueryGraphicsState()")
    _sub_once(shader / "Translation" / "EmitterContext.cs",
              "Operand alphaRef = ConstF(TranslatorContext.Definitions.AlphaTestReference);",
              "Operand alphaRef = this.Load(StorageKind.ConstantBuffer, SupportBuffer.Binding,   // [arceus] the replay's reference\n"
              "                            Const((int)SupportBufferField.FragmentAlphaTest));   // a scalar uint in the SPIR-V: no component index")
    _sub_once(prog, "                if (acc.QueryMotionPrevious())",
              "                if (acc.QueryTransformFeedbackEnabled())   // [arceus] the emulated transform feedback buffers\n"
              "                    for (int b = 0; b < 4; b++)\n"
              "                        res[\"sbufs\"].AsArray().Add((JsonNode)new JsonObject { [\"binding\"] = 100 + b, [\"slot\"] = -100 - b, [\"cb_slot\"] = -1, [\"cb_offset\"] = 0, [\"flags\"] = 1 });\n"
              "                if (acc.QueryMotionPrevious())")


def is_managed(dll: Path) -> bool:
    """True if the PE file has a CLR header (data directory 14), i.e. is a .NET assembly."""
    import struct
    try:
        data = dll.read_bytes()[:4096]
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        magic = struct.unpack_from("<H", data, pe + 24)[0]
        dd = pe + 24 + (112 if magic == 0x20B else 96)
        rva, size = struct.unpack_from("<II", data, dd + 14 * 8)
        return rva != 0 and size != 0
    except (OSError, struct.error):
        return False


def compile_() -> int:
    csc = find_csc()
    rt, ver = find_runtime()
    sources = [str(p) for p in SRC.rglob("*.cs")] + [str(p) for p in SHIM.glob("*.cs")]
    refs = [f"/r:{p}" for p in sorted(rt.glob("*.dll")) if is_managed(p)]
    resources = [f"/resource:{p},Ryujinx.Graphics.Shader.{p.relative_to(SRC / 'src' / 'Ryujinx.Graphics.Shader').as_posix().replace('/', '.')}"
                 for p in (SRC / "src" / "Ryujinx.Graphics.Shader").rglob("*.glsl")]
    dll = OUT / "sw_xlate.dll"
    rsp = OUT / "csc.rsp"
    args = ["/nologo", "/noconfig", "/nostdlib+", "/target:exe", "/langversion:preview", "/unsafe+",
            "/optimize+", "/nullable:disable", "/nowarn:CS1701,CS1702,CS8981,CS0618,CS0169,CS0649,CS0414",
            f"/out:{dll}", *refs, *resources, *sources]
    rsp.write_text("\n".join(f'"{a}"' if " " in a else a for a in args), encoding="utf-8")
    r = subprocess.run([str(csc), f"@{rsp}"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = r.stdout + r.stderr
    errors = [l for l in out.splitlines() if ": error " in l]
    print(f"csc exit {r.returncode}: {len(errors)} errors")
    seen = set()
    for l in errors:
        k = l.split(": error ", 1)[1]
        if k not in seen:
            seen.add(k)
            print("  " + l[-260:])
        if len(seen) >= 40:
            break
    if r.returncode == 0:
        (OUT / "sw_xlate.runtimeconfig.json").write_text(json.dumps({
            "runtimeOptions": {"tfm": "net" + ".".join(ver.split(".")[:2]),
                               "framework": {"name": "Microsoft.NETCore.App", "version": ver}}}, indent=2))
        print(f"built {dll}")
    return r.returncode


def main() -> int:
    # A clone of LodeStar has build_work/xlate/src already prepared (the Ryujinx sources with the patches and our
    # edits, committed): it compiles from there.  Only a developer with the reference checkout re-prepares it.
    if "--no-prepare" not in sys.argv and VENDOR.is_dir():
        prepare()
    elif not (SRC / "Program.cs").is_file():
        sys.exit(f"no translator sources: {SRC} is missing and the reference checkout ({REF}) is not there")
    return compile_()


if __name__ == "__main__":
    sys.exit(main())
