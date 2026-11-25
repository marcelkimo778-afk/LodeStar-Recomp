"""swrecomp command line interface."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

from .arm64.lift import Lifter
from .errors import SwRecompError
from .formats.exefs import load_npdm, open_exefs
from .formats.nso import Nso
from .formats.romfs import open_romfs
from .pipeline.analyze import analyze_module
from .pipeline.build import build_project, find_toolchain
from .pipeline.recompile import (LoadedModule, Program, build_images, choose_bases,
                                 emit_program, plan_data_slots, resolve_imports)

# Config file name
CONFIG_FILE = "Settings/config.json"


def _project_root() -> Path:
    """Return the project root (the folder with Game/, Settings/ and System/; config paths are relative to it)."""
    # cli.py is at project_root/System/Engine/Source/swrecomp/cli.py (2026-11-25 layout)
    return Path(__file__).resolve().parents[4]


def _load_config() -> dict:
    """Load config.json from project root, return empty dict if not found."""
    config_path = _project_root() / CONFIG_FILE
    if config_path.is_file():
        try:
            return json.loads(config_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pass
    return {}


def _save_config(cfg: dict) -> None:
    """Save config.json to project root."""
    config_path = _project_root() / CONFIG_FILE
    config_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def _resolve_exefs(args_exefs: str | None) -> Path:
    """Resolve exefs directory from args, config, or default."""
    if args_exefs:
        return Path(args_exefs).resolve()
    cfg = _load_config()
    if "exefs_dir" in cfg:
        return (_project_root() / cfg["exefs_dir"]).resolve()
    # Default: project_root/Game/exefs
    return (_project_root() / "Game/exefs").resolve()


def _resolve_romfs(args_romfs: str | None) -> Path | None:
    """Resolve romfs directory from args, config, or default."""
    if args_romfs:
        return Path(args_romfs).resolve()
    cfg = _load_config()
    if "romfs_dir" in cfg:
        return (_project_root() / cfg["romfs_dir"]).resolve()
    # Default: project_root/Game/romfs
    default = _project_root() / "Game/romfs"
    return default.resolve() if default.is_dir() else None


def _resolve_work_dir() -> Path:
    """Resolve work directory from config or default."""
    cfg = _load_config()
    if "work_dir" in cfg:
        return (_project_root() / cfg["work_dir"]).resolve()
    # Default: project_root/System/Build/Cache
    return (_project_root() / "System/Build/Cache").resolve()


WORK = _resolve_work_dir()


def _work_dir() -> Path:
    WORK.mkdir(parents=True, exist_ok=True)
    return WORK


def _read_seed_file(path: Path) -> tuple[dict[str, list[int]], list[int]]:
    """Read addresses recorded by the runtime when a computed branch missed.

    Lines are ``module:0xaddr``; bare addresses (older files) are shared by all
    modules.  Keeping the module matters: sdk offset 0x58a440 and main offset
    0x58a440 are unrelated instructions.
    """
    per_module: dict[str, list[int]] = {}
    shared: list[int] = []
    if not path.is_file():
        return per_module, shared
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        owner = None
        value = line
        if ":" in line:
            head, _, tail = line.partition(":")
            if head and not head.startswith("0x"):
                owner, value = head, tail
        try:
            addr = int(value, 0)
        except ValueError:
            continue
        if owner is None:
            shared.append(addr)
        else:
            per_module.setdefault(owner, []).append(addr)
    # The runtime appends on every run; keep the first occurrence only.
    per_module = {k: list(dict.fromkeys(v)) for k, v in per_module.items()}
    shared = list(dict.fromkeys(shared))
    return per_module, shared


def _load_modules(exefs_dir: Path, names: list[str] | None) -> list[LoadedModule]:
    exefs = open_exefs(exefs_dir)
    out = []
    wanted = names or ["main"]
    for name in wanted:
        if exefs.find(name) is None:
            raise SwRecompError("ExeFS has no module %r (available: %s)"
                                % (name, ", ".join(exefs.module_names)))
        nso = exefs.load_nso(name, _work_dir() / "modules")
        out.append(LoadedModule(name=name, nso=nso, base=0))
    return out


def cmd_inspect(args) -> int:
    exefs_dir = _resolve_exefs(args.exefs)
    exefs = open_exefs(exefs_dir)
    print("ExeFS: %s (%s)" % (exefs.path, "PFS0 container" if exefs.container else "directory"))
    npdm = load_npdm(exefs, _work_dir())
    print("  program id     : %s" % npdm.summary()["program_id"])
    print("  title id range : %s..%s" % (npdm.summary()["title_id_min"], npdm.summary()["title_id_max"]))
    print("  flags          : %s" % ", ".join(npdm.flag_names))
    print("  name           : %r" % npdm.aci0.name if npdm.aci0 else "")
    for name in exefs.module_names:
        nso = exefs.load_nso(name, _work_dir() / "modules")
        s = nso.summary()
        print("  module %-8s id=%s image=0x%x bss=0x%x text=0x%x ro=0x%x data=0x%x"
              % (name, s["module_id"][:16], s["image_size"], s["bss_size"],
                 s["segments"][0]["decompressed_size"], s["segments"][1]["decompressed_size"],
                 s["segments"][2]["decompressed_size"]))
        if s["dynamic"]:
            d = s["dynamic"]
            print("      needed=%s init=0x%x init_array=%d relocations=%d imports=%d"
                  % (d["needed"], d["init"] or 0, d["init_array_size"], d["relocations"],
                     s["symbols"]["imports"]))
    romfs_dir = _resolve_romfs(args.romfs)
    if romfs_dir:
        romfs = open_romfs(romfs_dir)
        print("RomFS: %s" % json.dumps(romfs.summary()))
    return 0


def cmd_validate(args) -> int:
    exefs_dir = _resolve_exefs(args.exefs)
    exefs = open_exefs(exefs_dir)
    ok = True
    for name in exefs.module_names:
        nso = exefs.load_nso(name, _work_dir() / "modules")
        result = nso.verify()
        status = "OK" if result["ok"] else "FAILED"
        ok = ok and result["ok"]
        print("%-8s %-6s %s" % (name, status, ", ".join(
            "%s=%s" % (k, v["status"]) for k, v in result["segments"].items())))
    npdm = load_npdm(exefs, _work_dir())
    print("npdm     OK     program=%s" % npdm.summary()["program_id"])
    romfs_dir = _resolve_romfs(args.romfs)
    if romfs_dir:
        romfs = open_romfs(romfs_dir)
        print("romfs    OK     %s" % json.dumps(romfs.summary()))
    return 0 if ok else 1


def cmd_analyze(args) -> int:
    exefs_dir = _resolve_exefs(args.exefs)
    modules = _load_modules(exefs_dir, args.modules)
    recorded, shared = _read_seed_file(Path(args.seed_file) if args.seed_file else _work_dir() / "missing_seeds.txt")
    reports = []
    for mod in modules:
        seeds = shared + recorded.get(mod.name, [])
        result = analyze_module(mod.nso, mod.name, max_functions=args.max_functions,
                                scan_all=not args.no_scan, extra_seeds=seeds)
        cov = result.coverage()
        reports.append(cov)
        print("module %s: %d functions, %d/%d instructions supported (%.2f%%)"
              % (mod.name, cov["functions"], cov["supported"], cov["instructions"],
                 cov["supported_pct"]))
        if cov["unsupported_instructions"]:
            print("  unsupported: %s" % json.dumps(cov["unsupported_instructions"], indent=2)[:1500])
    if args.out:
        Path(args.out).write_text(json.dumps(reports, indent=2))
        print("wrote %s" % args.out)
    return 0


def cmd_recompile(args) -> int:
    exefs_dir = _resolve_exefs(args.exefs)
    modules = _load_modules(exefs_dir, args.modules)
    seed_path = Path(args.seed_file) if args.seed_file else Path(args.out) / "missing_seeds.txt"
    recorded, shared_seeds = _read_seed_file(seed_path)
    if not args.seed_file:
        # The runtime records misses next to sw_app.exe (<out>/out/), so read
        # that file too; otherwise runtime discoveries never fed back.
        more, more_shared = _read_seed_file(Path(args.out) / "out" / "missing_seeds.txt")
        for mod, addrs in more.items():
            recorded.setdefault(mod, []).extend(addrs)
        shared_seeds += more_shared
    if recorded or shared_seeds:
        print("using %d runtime-recorded seeds from %s"
              % (sum(len(v) for v in recorded.values()) + len(shared_seeds), seed_path))

    # Every export another module relocates against must exist as a recompiled
    # function, otherwise the linked pointer hits a table gap at run time.
    # Collect those addresses per module before analysis seeds the worklists.
    export_target: dict[str, tuple[str, int]] = {}
    for mod in modules:
        for sym in mod.nso.exports:
            if sym.name and sym.value:
                export_target.setdefault(sym.name, (mod.name, sym.value))
    referenced: dict[str, list[int]] = {mod.name: [] for mod in modules}
    for mod in modules:
        syms = mod.nso.symbols()
        for _off, r_type, _add, sym_index in mod.nso.parse_dynamic().relocations:
            if r_type not in (257, 1025, 1026) or sym_index >= len(syms):
                continue
            target = export_target.get(syms[sym_index].name)
            if target:
                # Seed the module that *defines* the symbol.  A relocation in
                # main against an sdk export must force that sdk function into
                # the program, even when sdk's own cap is already full.
                referenced[target[0]].append(target[1])

    import time as _time
    t_start = _time.perf_counter()
    for mod in modules:
        t_mod = _time.perf_counter()
        extra_seeds = shared_seeds + recorded.get(mod.name, [])
        result = analyze_module(mod.nso, mod.name, max_functions=args.max_functions,
                                scan_all=not args.no_scan,
                                extra_seeds=extra_seeds + referenced[mod.name],
                                keep_ir=False, mnemonics=False)
        mod.functions = result.functions
        mod.lifter = result.lifter
        print("module %s: %d functions discovered (%d instructions, %.2f%% supported) [%.1f s]"
              % (mod.name, len(result.functions), result.coverage()["instructions"],
                 result.coverage()["supported_pct"], _time.perf_counter() - t_mod), flush=True)
    t_link = _time.perf_counter()

    choose_bases(modules, start=args.base)
    # Exports only become addresses once each module has its link base, so the
    # symbol table has to be materialised here rather than at load time.
    for mod in modules:
        mod.exports = {sym.name: mod.base + sym.value
                       for sym in mod.nso.exports if sym.name and sym.value}
    tmp = Program(modules=modules, native_bindings={}, native_thunks={}, thunk_by_name={},
                  trap_imports=[])
    export_map = tmp.export_map
    data_slots, data_base = plan_data_slots(modules, export_map)
    native, thunks, thunk_by_name, traps = resolve_imports(modules, export_map, data_slots)
    build_images(modules, export_map, thunk_by_name, data_slots)
    program = Program(modules=modules, native_bindings=native, native_thunks=thunks,
                      thunk_by_name=thunk_by_name, trap_imports=traps,
                      data_slots=data_slots, data_base=data_base)

    out = Path(args.out)
    gen = out / "gen"
    images = out / "images"
    t_emit = _time.perf_counter()
    emit_program(program, gen, images, functions_per_file=args.functions_per_file)
    t_end = _time.perf_counter()
    print("[time] analysis %.1f s, link/images %.1f s, emit %.1f s, total %.1f s"
          % (t_link - t_start, t_emit - t_link, t_end - t_emit, t_end - t_start), flush=True)
    manifest = json.loads((gen / "program.json").read_text())
    print("emitted %d functions in %d chunk files" % (manifest["functions"], len(manifest["chunks"])))
    for m in manifest["modules"]:
        print("  %s: base=0x%x image=0x%x relocations=%d skipped=%d imports=%d"
              % (m["name"], m["base"], m["image_size"], m["relocations_applied"],
                 len(m["relocations_skipped"]), m["imports"]))
    print("native service bindings: %d, unresolved imports (trap stubs): %d"
          % (len(native), len(traps)))
    if traps and args.show_unresolved:
        for name in traps[:40]:
            print("   unresolved: %s" % name)
    return 0


def cmd_build(args) -> int:
    out = Path(args.project)
    tool = find_toolchain()
    print("toolchain: %s (%s)" % (tool.name, tool.compiler))
    result = build_project(out / "Chunks", Path(args.runtime), out)
    print("compiled %d sources -> %s" % (result["sources"], result["executable"]))
    print("log: %s" % result["log"])
    return 0


def cmd_run(args) -> int:
    out = Path(args.project)
    exe = out / "Current" / ("sw_app.exe" if sys.platform == "win32" else "sw_app")
    if not exe.is_file():
        raise SwRecompError("no built executable at %s; run 'build' first" % exe)
    # argv[0] must be absolute: the runtime derives its module-image directory
    # from it, and the process is launched with cwd=out.
    cmd = [str(exe.resolve())]
    if args.call:
        cmd += ["--call", args.call]
    if args.init:
        cmd += ["--run-init"]
    if args.budget:
        cmd += ["--budget", str(args.budget)]
    if args.audit:
        cmd += ["--audit"]
    if args.list_services:
        cmd += ["--list-services"]
    if args.max_dispatch:
        cmd += ["--max-dispatch", str(args.max_dispatch)]
    if args.trace:
        cmd += ["--trace", str(Path(args.trace).resolve())]
    proc = subprocess.run(cmd, cwd=str(out))
    return proc.returncode


def cmd_test(args) -> int:
    cmd = [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-v" if args.verbose else ""]
    cmd = [c for c in cmd if c]
    proc = subprocess.run(cmd)
    return proc.returncode


def cmd_check_paths(args) -> int:
    """Verify that configured paths exist and contain required files."""
    root = _project_root()
    cfg = _load_config()
    
    print("Project root: %s" % root)
    print("Config: %s" % (root / CONFIG_FILE))
    print()
    
    # Resolve paths
    exefs_dir = _resolve_exefs(args.exefs)
    romfs_dir = _resolve_romfs(args.romfs)
    work_dir = _resolve_work_dir()
    
    print("Resolved paths:")
    print("  exefs_dir : %s" % exefs_dir)
    print("  romfs_dir : %s" % (romfs_dir if romfs_dir else "(not configured)"))
    print("  work_dir  : %s" % work_dir)
    print()
    
    # Check ExeFS
    errors = []
    warnings = []
    
    if not exefs_dir.is_dir():
        errors.append("exefs_dir does not exist: %s" % exefs_dir)
    else:
        print("ExeFS check: %s" % exefs_dir)
        try:
            exefs = open_exefs(exefs_dir)
            required_modules = ["main", "main.npdm", "rtld", "sdk", "subsdk0", "subsdk1"]
            for mod in required_modules:
                if mod == "main.npdm":
                    # NPDM is not a module but an entry
                    if exefs.npdm_entry is None:
                        errors.append("  MISSING: main.npdm (NPDM metadata)")
                    else:
                        print("  OK: main.npdm")
                else:
                    if exefs.find(mod) is None:
                        errors.append("  MISSING: module '%s'" % mod)
                    else:
                        print("  OK: module '%s'" % mod)
            # List all modules found
            print("  All modules: %s" % ", ".join(exefs.module_names))
        except Exception as e:
            errors.append("  ExeFS error: %s" % e)
    
    # Check RomFS
    if romfs_dir:
        print()
        print("RomFS check: %s" % romfs_dir)
        if not romfs_dir.is_dir():
            errors.append("romfs_dir does not exist: %s" % romfs_dir)
        else:
            try:
                romfs = open_romfs(romfs_dir)
                summary = romfs.summary()
                print("  OK: RomFS accessible")
                print("  Files: %d, Dirs: %d, Size: %s" % (
                    summary.get("files", 0), summary.get("dirs", 0), summary.get("size", "?")))
            except Exception as e:
                errors.append("  RomFS error: %s" % e)
    else:
        warnings.append("romfs_dir not configured (optional)")
    
    # Check work directory
    print()
    print("Work directory: %s" % work_dir)
    if work_dir.is_dir():
        print("  OK: exists")
        modules_cache = work_dir / "modules"
        if modules_cache.is_dir():
            print("  Module cache: %s (%d entries)" % (modules_cache, len(list(modules_cache.iterdir()))))
        else:
            print("  Module cache: not yet created")
    else:
        warnings.append("work_dir does not exist yet (will be created on first run): %s" % work_dir)
    
    # Print results
    print()
    if warnings:
        for w in warnings:
            print("WARNING: %s" % w)
    if errors:
        for e in errors:
            print("ERROR: %s" % e)
        print()
        print("FAILED: %d error(s)" % len(errors))
        return 1
    else:
        print("SUCCESS: All checks passed")
        return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="swrecomp",
                                description="Nintendo Switch static recompilation toolchain")
    # Global options for exefs/romfs
    p.add_argument("--exefs", help="path to ExeFS directory (or set in config.json)")
    p.add_argument("--romfs", help="path to RomFS directory (or set in config.json)")
    sub = p.add_subparsers(dest="command", required=True)

    q = sub.add_parser("inspect", help="summarise ExeFS/NPDM/RomFS inputs")
    q.add_argument("exefs", nargs="?", help="ExeFS directory (or use --exefs / config.json)")
    q.add_argument("--romfs", help="RomFS directory (or use --romfs / config.json)")
    q.set_defaults(func=cmd_inspect)

    q = sub.add_parser("validate", help="verify segment hashes and container structure")
    q.add_argument("exefs", nargs="?", help="ExeFS directory (or use --exefs / config.json)")
    q.add_argument("--romfs", help="RomFS directory (or use --romfs / config.json)")
    q.set_defaults(func=cmd_validate)

    q = sub.add_parser("analyze", help="discover functions and report instruction coverage")
    q.add_argument("exefs", nargs="?", help="ExeFS directory (or use --exefs / config.json)")
    q.add_argument("--module", dest="modules", action="append")
    q.add_argument("--max-functions", type=int, default=4000)
    q.add_argument("--no-scan", action="store_true")
    q.add_argument("--out")
    q.add_argument("--seed-file")
    q.set_defaults(func=cmd_analyze)

    q = sub.add_parser("recompile", help="link modules and emit native C")
    q.add_argument("exefs", nargs="?", help="ExeFS directory (or use --exefs / config.json)")
    q.add_argument("--module", dest="modules", action="append")
    q.add_argument("--max-functions", type=int, default=4000)
    q.add_argument("--no-scan", action="store_true")
    q.add_argument("--base", type=lambda v: int(v, 0), default=0x100000)
    q.add_argument("--out", default="build")
    q.add_argument("--show-unresolved", action="store_true")
    q.add_argument("--seed-file")
    q.add_argument("--functions-per-file", type=int, default=120)
    q.set_defaults(func=cmd_recompile)

    q = sub.add_parser("build", help="compile and link the standalone application")
    q.add_argument("--project", default="Build")
    q.add_argument("--runtime", default="Engine/Runtime")
    q.set_defaults(func=cmd_build)

    q = sub.add_parser("run", help="launch the standalone application")
    q.add_argument("--project", default="Build")
    q.add_argument("--audit", action="store_true")
    q.add_argument("--call")
    q.add_argument("--init", action="store_true")
    q.add_argument("--budget", type=int)
    q.add_argument("--list-services", action="store_true")
    q.add_argument("--max-dispatch", type=lambda v: int(v, 0))
    q.add_argument("--trace")
    q.set_defaults(func=cmd_run)

    q = sub.add_parser("test", help="run the automated test suite")
    q.add_argument("-v", "--verbose", action="store_true")
    q.set_defaults(func=cmd_test)

    q = sub.add_parser("check-paths", help="verify configured paths and required files")
    q.add_argument("--exefs", help="ExeFS directory (or use --exefs / config.json)")
    q.add_argument("--romfs", help="RomFS directory (or use --romfs / config.json)")
    q.set_defaults(func=cmd_check_paths)

    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except SwRecompError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
