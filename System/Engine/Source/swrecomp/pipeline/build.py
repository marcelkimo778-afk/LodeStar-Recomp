"""Host C toolchain driver.

The recompiler emits portable C11; building it needs a host compiler.  On Windows
the Visual Studio Build Tools installation is discovered without requiring a
developer prompt; elsewhere cc/gcc/clang is used.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
import sys
from dataclasses import dataclass, field
from pathlib import Path

from ..errors import SwRecompError


@dataclass
class Toolchain:
    name: str
    compiler: str
    env: dict[str, str]
    extra_compile: list[str] = field(default_factory=list)
    extra_link: list[str] = field(default_factory=list)
    obj_suffix: str = ".o"

    def compile_cmd(self, sources: list[Path], objdir: Path, includes: list[Path],
                    opt: str = "") -> list[str]:
        raise NotImplementedError


class MsvcToolchain(Toolchain):
    def compile_cmd(self, sources: list[Path], objdir: Path, includes: list[Path],
                    opt: str = "/O2") -> list[str]:
        build_id = os.environ.get("SW_BUILD_ID", "dev")
        cmd = [self.compiler, "/nologo", "/c", *opt.split(), "/W3", "/GS-", "/bigobj",
               "/MP%d" % (os.cpu_count() or 4),
               "/wd4102",  # generated code has unreferenced block labels
               "/D_CRT_SECURE_NO_WARNINGS", "/D_CRT_NONSTDC_NO_DEPRECATE",
               '/DSW_BUILD_ID="%s"' % build_id]
        for inc in includes:
            cmd.append("/I" + str(inc))
        cmd += ["/Fo" + str(objdir) + os.sep]
        cmd += [str(s) for s in sources]
        return cmd

    def link_cmd(self, objects: list[Path], exe: Path) -> list[str]:
        return ([self.compiler, "/nologo"] + [str(o) for o in objects]
                + ["/Fe:" + str(exe), "user32.lib", "gdi32.lib", "opengl32.lib"])


class GnuToolchain(Toolchain):
    def compile_cmd(self, sources: list[Path], objdir: Path, includes: list[Path],
                    opt: str = "-O2") -> list[str]:
        cmd = [self.compiler, "-c", *opt.split(), "-std=c11", "-Wall", "-Wno-unused-function"]
        for inc in includes:
            cmd += ["-I", str(inc)]
        cmd += ["-o", str(objdir), *[str(s) for s in sources]]
        return cmd

    def link_cmd(self, objects: list[Path], exe: Path) -> list[str]:
        return [self.compiler, "-O2", "-o", str(exe)] + [str(o) for o in objects] + ["-lm"]


def _msvc_env(root: Path, msvc_ver: Path) -> dict[str, str]:
    env = dict(os.environ)
    inc = [msvc_ver / "include"]
    lib = [msvc_ver / "lib" / "x64"]
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10"
    sdk_ver = None
    if (kits / "Include").is_dir():
        versions = sorted(p.name for p in (kits / "Include").iterdir() if p.is_dir())
        if versions:
            sdk_ver = versions[-1]
            inc += [kits / "Include" / sdk_ver / d for d in ("ucrt", "um", "shared", "winrt")]
            lib += [kits / "Lib" / sdk_ver / "ucrt" / "x64", kits / "Lib" / sdk_ver / "um" / "x64"]
    bin_dir = msvc_ver / "bin" / "Hostx64" / "x64"
    env["PATH"] = str(bin_dir) + ";" + r"C:\Windows\System32;C:\Windows" + ";" + env.get("PATH", "")
    env["INCLUDE"] = ";".join(str(p) for p in inc)
    env["LIB"] = ";".join(str(p) for p in lib)
    return env


def find_toolchain() -> Toolchain:
    if sys.platform == "win32":
        vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
            "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        candidates: list[Path] = []
        if vswhere.is_file():
            try:
                out = subprocess.run([str(vswhere), "-latest", "-products", "*", "-property",
                                      "installationPath"], capture_output=True, text=True, timeout=30)
                for line in out.stdout.splitlines():
                    p = Path(line.strip()) / "VC" / "Tools" / "MSVC"
                    if p.is_dir():
                        candidates += sorted([d for d in p.iterdir() if d.is_dir()], reverse=True)
            except (OSError, subprocess.SubprocessError):
                pass
        # Fallback when vswhere is absent: scan known VS/BuildTools install roots.
        if not candidates:
            for vs_root in [r"C:\Program Files (x86)\Microsoft Visual Studio",
                            r"C:\Program Files\Microsoft Visual Studio"]:
                root = Path(vs_root)
                if not root.is_dir():
                    continue
                for edition in sorted(root.iterdir(), reverse=True):
                    msvc_base = edition / "VC" / "Tools" / "MSVC"
                    if msvc_base.is_dir():
                        candidates += sorted([d for d in msvc_base.iterdir() if d.is_dir()],
                                             reverse=True)
        for msvc in candidates:
            cl = msvc / "bin" / "Hostx64" / "x64" / "cl.exe"
            if cl.is_file():
                root = msvc.parent.parent.parent
                return MsvcToolchain(name="msvc", compiler=str(cl),
                                     env=_msvc_env(root, msvc), obj_suffix=".obj")
        raise SwRecompError(
            "no MSVC toolchain found. Install Visual Studio Build Tools with the C++ workload, "
            "or run the build on Linux/macOS where cc is used.")
    for cc in ("cc", "gcc", "clang"):
        path = shutil.which(cc)
        if path:
            return GnuToolchain(name=cc, compiler=path, env=dict(os.environ))
    raise SwRecompError("no C compiler found (expected cc/gcc/clang on PATH)")


def _run(cmd: list[str], env: dict[str, str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, env=env, capture_output=True, encoding="utf-8", errors="replace")


def build_project(gen_dir: Path, runtime_dir: Path, out_dir: Path, jobs: int = 0) -> dict:
    tool = find_toolchain()
    objdir = out_dir / "obj"
    objdir.mkdir(parents=True, exist_ok=True)
    sources = sorted(gen_dir.glob("*.c")) + sorted((runtime_dir / "src").glob("*.c"))
    if not sources:
        raise SwRecompError("no sources to compile in %s" % gen_dir)
    includes = [runtime_dir / "include", gen_dir]
    log: list[str] = []
    objects: list[Path] = []

    def write_log() -> None:
        (out_dir / "build.log").write_text("\n".join(log), encoding="utf-8", errors="replace")

    def record(cmd: list[str], proc) -> None:
        log.append("$ " + " ".join(cmd))
        if proc.stdout and proc.stdout.strip():
            log.append(proc.stdout.strip())
        if proc.stderr and proc.stderr.strip():
            log.append(proc.stderr.strip())

    exe = out_dir / ("sw_app.exe" if tool.name == "msvc" else "sw_app")

    header_deps = list((runtime_dir / "include").glob("*.h")) + [gen_dir / "sw_program.h"]

    def compile_one(src: Path):
        obj = objdir / (src.stem + tool.obj_suffix)
        # Incremental: a rebuild after a runtime-only edit must not recompile
        # hundreds of megabytes of generated chunks.
        try:
            obj_mtime = obj.stat().st_mtime
            if all(d.stat().st_mtime <= obj_mtime for d in [src, *header_deps] if d.exists()):
                return src, obj, [], None
        except OSError:
            pass
        if tool.name == "msvc":
            cmd = tool.compile_cmd([src], objdir, includes) + ["/Fo:" + str(obj)]
        else:
            cmd = [tool.compiler, "-c", "-O2", "-std=c11", "-w"]
            for inc in includes:
                cmd += ["-I", str(inc)]
            cmd += ["-o", str(obj), str(src)]
        return src, obj, cmd, _run(cmd, tool.env)

    # Chunk files are independent translation units; compiling them serially
    # dominated every rebuild once the function count grew into the tens of
    # thousands.
    workers = max(1, min(16, jobs or (os.cpu_count() or 4)))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(compile_one, sources))
    for src, obj, cmd, proc in results:
        if proc is None:
            objects.append(obj)
            continue
        record(cmd, proc)
        if proc.returncode != 0:
            write_log()
            raise SwRecompError("compile failed for %s (see %s/build.log)" % (src.name, out_dir))
        objects.append(obj)

    if tool.name == "msvc":
            cmd = tool.link_cmd(objects, exe)
            # Windows caps a command line at 32767 characters.  A program with
            # thousands of translation units overflows that as soon as the startup
            # closure grows, so hand the object list to the linker through a
            # response file; it expands to exactly the same arguments.
            if sum(len(c) + 1 for c in cmd) > 30000:
                rsp = out_dir / "objects.rsp"
                # Quote paths with spaces for MSVC response file
                rsp.write_text("\n".join(f'"{o}"' for o in objects), encoding="utf-8")
                cmd = [tool.compiler, "/nologo", "/O2", "@" + str(rsp), "/Fe:" + str(exe), "user32.lib", "gdi32.lib"]
    else:
        cmd = tool.link_cmd(objects, exe)
    proc = _run(cmd, tool.env)
    record(cmd, proc)
    if proc.returncode != 0:
        write_log()
        raise SwRecompError("link failed (see %s/build.log)" % out_dir)
    write_log()
    return {"toolchain": tool.name, "sources": len(sources), "objects": len(objects),
            "executable": str(exe), "log": str(out_dir / "build.log")}
