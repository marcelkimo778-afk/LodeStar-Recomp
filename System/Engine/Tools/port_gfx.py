#!/usr/bin/env python3
"""Port the graphics layer of totk-pc-recomp (MIT) into Engine/Runtime/gfx.

Engine/Runtime/gfx/ref/ holds the pristine reference files.  This script
copies the ones listed below into Engine/Runtime/gfx/ and applies the edits
that adapt them to this runtime and to MSVC; anything game-specific to Tears
of the Kingdom is removed.  Run it again after refreshing ref/; files that
were edited by hand afterwards should be listed in HAND_EDITED so they are
not overwritten.

    python Engine/Tools/port_gfx.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GFX = ROOT / "Engine" / "Runtime" / "gfx"
REF = GFX / "ref"

FILES = ["nvn.h", "nvn_obj.h", "nvn_api.h", "nvn_api_core.c", "nvn_api_obj.c", "nvn_api_cmd.c",
         "nvn_ext.c", "nvn_tex.c", "nvn_tex.h", "nvn_hmap.h", "nvn_pool_index.h", "nvn_procs.inc",
         "gfx.h", "gfx_null.c", "tex.h", "tex_deswizzle.c"]
HAND_EDITED: set[str] = set()

# Vulkan back end and shader service: staged into gfx/vk/ (built only once the
# port links; build.py compiles gfx/vk when SW_VULKAN is not 0).
import os  # noqa: E402
REF_RUNTIME = Path(os.environ.get("SW_REF_TOTK", str(Path(__file__).resolve().parents[3] / "reference" / "totk-pc-recomp"))) / "runtime"
VK_DIR = GFX / "vk"
VK_FILES = ["vk.h", "vk_exec.h", "gfx_wsys.h", "vk_backend.c", "vk_device.c", "vk_caps.c", "vk_formats.c",
            "vk_memory.c", "vk_texture.c", "vk_sync.c", "vk_exec.c", "vk_exec_draw.c", "vk_exec_xfer.c",
            "vk_pipeline.c", "vk_astc.c", "tex_astc.c", "tex_astc_mt.c", "gfx_present.c", "gfx_shader.c",
            "gfx_window_win.c", "vk_dump.c", "vk_usnap.c", "astc_decode_spv.h", "bc3_encode_spv.h", "vk_mvobj_fs.h",
            "vk_fsr.c", "fsr_easu_spv.h", "fsr_rcas_spv.h", "vk_color.c", "color_grade_spv.h",   # [arceus] 2026-11-24
            "vk_dlaa.c", "dlaa_mv_spv.h", "dlaa_mask_spv.h",   # [arceus] 2026-11-25 DLAA (Arceus edits in gfx_patches)
            # declarations only; the optional modules behind them are stubbed in sw_vk_stubs.c
            "vk_dlaa.h", "flight.h", "ui/menu.h", "fs.h", "input.h", "input_win.h"]

HEADER = ("// Ported into Arceus Recomp from totk-pc-recomp (MIT, Copyright (c) 2026 Valiantsin Dzerakh; see\n"
          "// ../LICENSE.totk-pc-recomp) by Engine/Tools/port_gfx.py.  Edit the port script, not this file.\n")


def cut_block(text: str, start_pat: str, end_pat: str, name: str) -> str:
    """Remove from the line matching start_pat through the line matching end_pat."""
    lines = text.split("\n")
    s = next((i for i, l in enumerate(lines) if re.search(start_pat, l)), None)
    if s is None:
        raise SystemExit(f"{name}: start pattern not found: {start_pat}")
    e = next((i for i in range(s, len(lines)) if re.search(end_pat, lines[i])), None)
    if e is None:
        raise SystemExit(f"{name}: end pattern not found: {end_pat}")
    return "\n".join(lines[:s] + [f"// [port] removed: {lines[s].strip()[:80]} ..."] + lines[e + 1:])


def cut_function(text: str, start_pat: str, name: str) -> str:
    """Remove one C function definition: the line matching start_pat through its matching closing brace."""
    m = re.search(start_pat, text, re.M)
    if not m:
        raise SystemExit(f"{name}: function not found: {start_pat}")
    i = text.index("{", m.start())
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                end = text.index("\n", j) + 1 if "\n" in text[j:] else len(text)
                first = text[m.start():text.index("\n", m.start())].strip()
                return text[:m.start()] + f"// [port] removed (MSVC has no weak symbols): {first[:90]}\n" + text[end:]
    raise SystemExit(f"{name}: unbalanced braces after {start_pat}")


def guard_function(text: str, start_pat: str, cond: str, name: str) -> str:
    """Wrap one C function definition in #if cond ... #endif."""
    m = re.search(start_pat, text, re.M)
    if not m:
        raise SystemExit(f"{name}: function not found: {start_pat}")
    i = text.index("{", m.start())
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                end = text.index("\n", j) + 1
                return (text[:m.start()] + f"#if {cond}   // [port] weak fallback\n" + text[m.start():end]
                        + "#endif\n" + text[end:])
    raise SystemExit(f"{name}: unbalanced braces after {start_pat}")


def sub_once(text: str, old: str, new: str, name: str) -> str:
    if text.count(old) != 1:
        raise SystemExit(f"{name}: expected one '{old[:60]}', found {text.count(old)}")
    return text.replace(old, new)


def edit(name: str, text: str) -> str:
    if name == "nvn_api_cmd.c":   # [arceus] SW_TRACE_UPD=1: log UpdateUniformBuffer payloads (object matrices)
        old_upd = "    memcpy(r->data, data, n);\n}\n"
        assert text.count(old_upd) == 1, "nvn_api_cmd.c: UpdateUniformBuffer changed"
        text = text.replace(old_upd, old_upd[:-2] + """    {
        static int tu = -1, nu;
        if (tu < 0) { const char *e = getenv("SW_TRACE_UPD"); tu = e && *e == '1'; }
        if (tu && nu < 400 && n >= 64) {
            nu++;
            const float *f = (const float *)data;
            rt_log("upd_ub: dst %#llx off %#llx size %#llx caller %#llx: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g",
                   (unsigned long long)arg_x(c, 1), (unsigned long long)arg_x(c, 3), (unsigned long long)n,
                   (unsigned long long)c->x[30], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11],
                   f[12], f[13], f[14], f[15]);
        }
    }
}
""", 1)
    if name == "nvn_api_core.c":
        # TotK: memcpy override for its grass maps; release-read-lock LOD guard
        text = cut_block(text, r"^// memcpy of pool storage the GPU rendered", r'^HOST_OVERRIDE\("memcpy"', name)
        text = cut_block(text, r"^// The producer \(1\.2\.1 main", r"^HOST_OBSERVER\(", name)
        # weak defaults: the strong ones are in nvn_api_cmd.c (always) and vk_backend.c (with Vulkan)
        text = cut_function(text, r"^__attribute__\(\(weak\)\) void nvn_submit_prepare\(", name)
        text = cut_function(text, r"^__attribute__\(\(weak\)\) void nvn_submit_prepare_async\(", name)
        text = guard_function(text, r"^__attribute__\(\(weak\)\) int gfx_replay_async\(", "!defined(SW_HAVE_VULKAN)", name)
        text = guard_function(text, r"^__attribute__\(\(weak\)\) uint64_t gfx_replayed_submits\(",
                              "!defined(SW_HAVE_VULKAN)", name)
    if name == "nvn_ext.c":
        text = guard_function(text, r"^__attribute__\(\(weak\)\) void nvn_pool_flushed\(", "!defined(SW_HAVE_VULKAN)", name)
    if name == "nvn_api_obj.c":
        text = sub_once(text, "    movie_tick(c);", "    // [port] movie_tick: TotK movie player not ported", name)
        text = sub_once(text, "    res_present(c);", "    // [port] res_present: TotK DLSS scene size not ported", name)
        # [arceus] guest threads share one host thread: the acquire wait runs the other guest
        # threads instead of sleeping the host (sw_gfx_glue.c sw_gfx_wait_until)
        text = sub_once(text, "        sleep_until(at);",
                        "        sw_gfx_wait_until(at);   // [arceus] other guest threads run meanwhile", name)
        text = sub_once(text, "static void sleep_until(uint64_t deadline) {",
                        "void sw_gfx_wait_until(uint64_t deadline);\nstatic void sleep_until(uint64_t deadline) {", name)
    if name == "tex.h":
        text = sub_once(text, '#include "vk.h"',
                        '#if __has_include("vk.h")   // [port] the Vulkan back end (gfx/vk) sees vk.h\n'
                        '#include "vk.h"\n#else\n#include <vulkan/vulkan.h>\n#endif', name)
    if name == "gfx_null.c":
        text = sub_once(text, "(uint64_t)((unsigned __int128)kern_now_ns() * 12 / 625)", "rt_ticks()", name)
        # the Vulkan back end is optional until it is ported: SW_HAVE_VULKAN selects it
        text = sub_once(text, '    b = gpu && !strcmp(gpu, "null") ? &gfx_null_backend : &gfx_vulkan_backend;',
                        '#ifdef SW_HAVE_VULKAN\n'
                        '    b = gpu && !strcmp(gpu, "null") ? &gfx_null_backend : &gfx_vulkan_backend;\n'
                        '#else\n'
                        '    (void)gpu;\n'
                        '    b = &gfx_null_backend;\n'
                        '#endif', name)
    if name == "nvn_obj.h":
        text = text.replace("__builtin_offsetof", "offsetof")
        # [arceus] transform feedback (GPU skinning), see tf_front_end below and edit_vk
        text = sub_once(text, "OP_CLEAR_BUF, OP_COUNT_", "OP_CLEAR_BUF, OP_TF_BUF, OP_TF_BEGIN, OP_TF_END, OP_COUNT_", name)
        text = sub_once(text, "typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;",
                        "typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;\n"
                        "typedef struct { int32_t index, pad; uint64_t addr, size; } p_tf_buf;   "
                        "// [arceus] OP_TF_BUF: transform feedback buffer", name)
    if name == "nvn_api_cmd.c":
        text = sub_once(text, '"DRAW_IDX_IND", "DISPATCH_IND", "CLEAR_BUF",',
                        '"DRAW_IDX_IND", "DISPATCH_IND", "CLEAR_BUF", "TF_BUF", "TF_BEGIN", "TF_END",', name)
        text += TF_FRONT_END
    return text


# [arceus] Transform feedback: Pokémon Legends: Arceus skins characters on the GPU (its [Skinning] pass: a vertex-only
# program writes the skinned vertices to transform feedback buffers that the character draws then read). TotK never
# used it, so these procs were missing and every animated model (player, NPCs, Pokémon) drew nothing. Recorded here;
# the Vulkan back end emulates the capture with storage buffers (edit_vk, Engine/Tools/build_xlate.py our_edits).
# Begin = start at offset 0, Resume (value 2) keeps the write positions; Pause = End.
TF_FRONT_END = r'''
// ---------- [arceus] transform feedback (GPU skinning; Engine/Tools/port_gfx.py TF_FRONT_END)
// void nvnCommandBufferBindTransformFeedbackBuffer(NVNcommandBuffer *cmd, int index, NVNbufferAddress addr, size_t size)
NVN_API(nvnCommandBufferBindTransformFeedbackBuffer) {
    cmd_t *o = cmd_of(c);
    *REC(o, OP_TF_BUF, p_tf_buf) = (p_tf_buf){(int32_t)arg_x(c, 1), 0, arg_x(c, 2), arg_x(c, 3)};
    hw_fix(o, 16);
}
// void nvnCommandBufferBindTransformFeedbackBuffers(NVNcommandBuffer *cmd, int first, int n, const NVNbufferRange *r)
NVN_API(nvnCommandBufferBindTransformFeedbackBuffers) {
    cmd_t *o = cmd_of(c);
    int first = (int)arg_x(c, 1), n = (int)arg_x(c, 2);
    const NVNbufferRange *rg = arg_p(c, 3);
    for (int i = 0; rg && i < n && first + i < 4; i++) {
        *REC(o, OP_TF_BUF, p_tf_buf) = (p_tf_buf){first + i, 0, rg[i].address, rg[i].size};
        hw_fix(o, 16);
    }
}
// void nvnCommandBufferBegin/End/Pause/ResumeTransformFeedback(NVNcommandBuffer *cmd, NVNbufferAddress control)
NVN_API(nvnCommandBufferBeginTransformFeedback) { REC(cmd_of(c), OP_TF_BEGIN, p_flag)->value = 1; hw_fix(cmd_of(c), 8); }
NVN_API(nvnCommandBufferResumeTransformFeedback) { REC(cmd_of(c), OP_TF_BEGIN, p_flag)->value = 2; hw_fix(cmd_of(c), 8); }
NVN_API(nvnCommandBufferEndTransformFeedback) { REC(cmd_of(c), OP_TF_END, p_flag)->value = 0; hw_fix(cmd_of(c), 8); }
NVN_API(nvnCommandBufferPauseTransformFeedback) { REC(cmd_of(c), OP_TF_END, p_flag)->value = 1; hw_fix(cmd_of(c), 8); }
'''

# [arceus] the transform feedback layout of a vertex shader, from its NVN control section (decoded from the game's
# skinning shaders, PROGRESS.md): stride per buffer (u16 bytes at 0x6dc + 2b), varyings per buffer (u8 at 0x6e8 + b,
# in order), enabled (u8 0x6ed); varying k = generic output (u32 at 0xdc + 0x80k) - 0x20 and its component count
# (u32 at 0x4dc + 0x80k). Ryujinx wants attribute words: generic g component c = 0x20 + 4g + c.
TF_JOB = r'''
    if (!strcmp(stage, "vs") && control[0x6ed] == 1) {   // [arceus] transform feedback layout (port_gfx.py TF_JOB)
        uint16_t st[4];
        memcpy(st, control + 0x6dc, sizeof st);
        o += sprintf(o, ",\"xfb\":{\"stride\":[%u,%u,%u,%u],\"locs\":[", st[0], st[1], st[2], st[3]);
        for (int b = 0, k = 0; b < 4; b++) {
            o += sprintf(o, "%s[", b ? "," : "");
            const char *sep = "";
            for (int v = 0; v < control[0x6e8 + b] && k < 8; v++, k++) {
                uint32_t rec, cnt;
                memcpy(&rec, control + 0xdc + 0x80 * k, 4);
                memcpy(&cnt, control + 0x4dc + 0x80 * k, 4);
                if (rec < 0x20 || rec >= 0x40 || cnt > 4) continue;
                for (uint32_t q = 0; q < cnt; q++) o += sprintf(o, "%s%u", sep, 0x20 + 4 * (rec - 0x20) + q), sep = ",";
            }
            o += sprintf(o, "]");
        }
        o += sprintf(o, "]}");
    }
'''

# [arceus] the back end side of the emulated transform feedback (vk_exec_draw.c): the shader's tfe_data0-3 buffers
# (res.json sbufs slot -100-b, binding 100+b) bind to the transform feedback buffers, aligned down to the storage
# buffer alignment with the remainder added to tfe_offset; each capturing draw gets a fresh support buffer with
# tfe_offset[b] = (lead + bytes written so far) / 4 and tfe_vertex_count = its vertex count; after the draw the
# write positions advance by vertices x instances x stride (also for draws skipped while a pipeline builds, so later
# draws keep their places).
SKIP_VS = r'''
// ---- [arceus] SW_SKIP_VS=<hash>[,<hash>...]: drop draws whose vertex shader has that SPIR-V FNV-1a hash (the
// "vs" hashes of the frame dump, passes.txt).  A debugging aid to find which draws break a pass.
static int skip_vs(const GfxShader *vs) {
    static int init, n;
    static uint64_t want[16];
    static struct { const GfxShader *s; int hit; } memo[512];
    static int nmemo;
    if (!init) {
        init = 1;
        const char *e = getenv("SW_SKIP_VS");
        for (const char *q = e; q && *q && n < 16;) {
            want[n++] = strtoull(q, (char **)&q, 16);
            while (*q == ',' || *q == ' ') q++;
        }
    }
    if (!n || !vs) return 0;
    for (int i = 0; i < nmemo; i++) if (memo[i].s == vs) return memo[i].hit;
    uint64_t words = 0, h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)gfx_shader_spirv(vs, &words);
    for (uint64_t i = 0; b && i < words * 4; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    int hit = 0;
    for (int i = 0; i < n; i++) hit |= want[i] == h;
    if (nmemo < 512) memo[nmemo].s = vs, memo[nmemo++].hit = hit;
    return hit;
}
'''

SKIP_T0 = r'''
// ---- [arceus] SW_SKIP_T0=1: in depth-only D32F passes (the sun shadow cascades) drop draws whose vertex c6 block has
// translation exactly 0 (an object centred on the camera); SW_SKIP_T0=2 drops them in every pass.  Debugging aid.
static int skip_t0(VkExec *ex, const VkPipeState *s) {
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("SW_SKIP_T0"); mode = e ? atoi(e) : 0; }
    if (!mode) return 0;
    if ((mode == 1 || mode == 3) && s->depth_fmt != 0x34) return 0;
    const p_bind_buf *u = &ex->ubo[0][3];
    VkMemSpan ms;
    if (!u->addr || u->size < 64 || !vk_mem_resolve(u->addr, 64, &ms) || ms.avail < 64) return 0;
    const float *f = (const float *)(uintptr_t)u->addr;
    int t0 = f[12] == 0.0f && f[13] == 0.0f && f[14] == 0.0f;
    if (mode == 3) {   // arm a store watch on the first such block, keep drawing
        static int armed;
        extern void sw_watch_set(uint64_t, uint64_t);
        if (t0 && !armed) { armed = 1; sw_watch_set(u->addr, 64); }
        return 0;
    }
    return t0;
}
'''

TF_EXEC = r'''
// ---- [arceus] emulated transform feedback (Engine/Tools/port_gfx.py TF_EXEC)
static const uint8_t *tf_control(const prog_t *p) {   // the control section of the stage that captures
    for (uint32_t i = 0; p && i < p->nstage && i < 6; i++) {
        const uint8_t *ctl = p->sd[i].control;
        if (ctl && ctl[0x6ed] == 1) return ctl;
    }
    return NULL;
}
static uint64_t tf_lead(const VkExec *ex, int b) {
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    uint64_t a = LIM.sbo_align ? LIM.sbo_align : 16;
    return ex->tf_buf[b].addr & (a - 1);
}
static VkDescriptorBufferInfo tf_info(VkExec *ex, int b) {
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    uint64_t max = LIM.sbo_max, align = LIM.sbo_align;
    VkDescriptorBufferInfo bi;
    if (b >= 0 && b < 4 && ex->tf_active && ex->tf_buf[b].addr && ex->tf_buf[b].size) {
        uint64_t lead = tf_lead(ex, b);
        if (bind_range(ex, ex->tf_buf[b].addr - lead, ex->tf_buf[b].size + lead, align, max, VK_MUSE_SSBO, 1, &bi)) {
            if (bi.buffer != VK_NULL_HANDLE) note_written(ex, ex->tf_buf[b].addr - lead, bi.range);
            return bi;
        }
        warn_once(W_SSBO_ALIGN, "transform feedback buffer %#llx is not backed: its captures are lost",
                  (unsigned long long)ex->tf_buf[b].addr);
    }
    if (b >= 0 && b < 4 && !ex->tf_active)
        warn_once(W_SSBO_SLOT, "a capturing shader drew outside Begin/EndTransformFeedback: its stores go to the zero buffer");
    return zero_info(max);
}
static void tf_advance(VkExec *ex, int count, int instances) {
    if (!ex->tf_active || count <= 0) return;
    const uint8_t *ctl = tf_control(ex->prog);
    if (!ctl) return;
    uint64_t v = (uint64_t)count * (uint64_t)(instances > 0 ? instances : 1);
    for (int b = 0; b < 4; b++) {
        uint16_t s;
        memcpy(&s, ctl + 0x6dc + 2 * b, 2);
        ex->tf_pos[b] += (uint32_t)(v * s);
    }
}

'''


def write_impl_table() -> None:
    """nvn_impl_table.inc: NVN_IMPL(name) for every NVN_API(name) in the ported sources."""
    names = []
    for name in FILES + ["sw_nvn_extra.c"]:     # sw_nvn_extra.c: our own procs (not generated)
        if name.endswith(".c"):
            names += re.findall(r"^NVN_API\((\w+)\)", (GFX / name).read_text(encoding="utf-8"), re.M)
    text = ("// Generated by Engine/Tools/port_gfx.py: NVN procs implemented by the ported front end.\n"
            + "".join(f"NVN_IMPL({n})\n" for n in sorted(set(names))))
    path = GFX / "nvn_impl_table.inc"
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8", newline="\n")
        print(f"write  nvn_impl_table.inc ({len(set(names))} procs)")


PATCH_DIR = Path(__file__).resolve().parent / "gfx_patches"


def apply_patches(name: str, text: str) -> str:
    """[arceus] Engine/Tools/gfx_patches/<file>.patch: unified-diff hunks applied to the finished text (edits too
    long or too debug-specific for sub_once: the 30/60 fps present interval in nvn_api_obj.c, the SW_OQ_LOG
    occlusion-query instrumentation in vk_exec*).  Each hunk's old lines must occur exactly once, like sub_once."""
    p = PATCH_DIR / (Path(name).name + ".patch")
    if not p.is_file():
        return text
    lines = p.read_text(encoding="utf-8").splitlines(keepends=True)
    hunks, cur = [], None
    for ln in lines:
        if ln.startswith("@@"):
            cur = ([], [])
            hunks.append(cur)
        elif cur is not None and not ln.startswith(("---", "+++")):
            body = ln[1:] if ln[:1] in " +-" else ln
            if ln[:1] in " -":
                cur[0].append(body)
            if ln[:1] in " +":
                cur[1].append(body)
    for old, new in hunks:
        o, n = "".join(old), "".join(new)
        if text.count(o) != 1:
            raise SystemExit(f"{name}: patch hunk from {p.name} matches {text.count(o)} times:\n{o[:400]}")
        text = text.replace(o, n)
    return text


def main() -> int:
    for name in FILES:
        if name in HAND_EDITED:
            print(f"skip   {name} (hand edited)")
            continue
        src = REF / name
        text = src.read_text(encoding="utf-8")
        text = edit(name, text)
        if not name.endswith(".inc"):
            text = HEADER + text
        text = apply_patches(name, text)
        dst = GFX / name
        old = dst.read_text(encoding="utf-8") if dst.exists() else None
        if old != text:
            dst.write_text(text, encoding="utf-8", newline="\n")
            print(f"write  {name}")
        else:
            print(f"same   {name}")
    write_impl_table()
    VK_DIR.mkdir(exist_ok=True)
    for name in VK_FILES:
        if name in HAND_EDITED:
            continue
        text = (REF_RUNTIME / name).read_text(encoding="utf-8")
        text = edit_vk(name, text)
        if not name.endswith("_spv.h") and not name.endswith("_fs.h"):
            text = HEADER.replace("../LICENSE", "../../LICENSE") + text
        text = apply_patches(name, text)
        dst = VK_DIR / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        if not dst.exists() or dst.read_text(encoding="utf-8") != text:
            dst.write_text(text, encoding="utf-8", newline="\n")
            print(f"write  vk/{name}")
    return 0


def edit_vk(name: str, text: str) -> str:
    """Adaptations of the Vulkan back end to MSVC and this runtime."""
    if name == "vk_exec_xfer.c":   # [arceus] occlusion queries: the field overflows 4096 slots ("ring full"),
        # uncounted draws make every conditional draw visible -> culled props drawn at the origin
        assert "#define OQ_N 4096" in text, "vk_exec_xfer.c: OQ_N changed"
        text = text.replace("#define OQ_N 4096", "#define OQ_N 65536   // [arceus] was 4096", 1)
        # [arceus] SW_COND_UNKNOWN=skip: a conditional draw whose samples-passed report is not done yet (the replay
        # decides on the CPU while recording, before the GPU ran the test) is skipped instead of drawn.
        old_cu = "    if (rx == 2 || ry == 2) return 1;   // a samples-passed count not done yet\n"
        assert old_cu in text, "vk_exec_xfer.c: cond_enabled changed"
        text = text.replace(old_cu, "    if (rx == 2 || ry == 2) {   // a samples-passed count not done yet\n"
                                    "        static int cu = -1;\n"
                                    "        if (cu < 0) { const char *e = getenv(\"SW_COND_UNKNOWN\"); cu = e && !strcmp(e, \"skip\"); }\n"
                                    "        return !cu;\n"
                                    "    }\n", 1)
    if name == "vk_exec.c":   # [port] SW_TRACE_CLEAR=1: log depth clears (reverse-Z shadow debugging)
        old_dc = "        case OP_DEPTH_CLAMP: ex->pipe.depth_clamp = ((const p_flag *)a)->value != 0; break;\n"
        assert old_dc in text, "vk_exec.c: OP_DEPTH_CLAMP changed"
        text = text.replace(old_dc, "        case OP_DEPTH_CLAMP: {   // [arceus] SW_NO_DEPTH_CLAMP=1: debugging\n"
                                    "            static int nc = -1; if (nc < 0) { const char *e = getenv(\"SW_NO_DEPTH_CLAMP\"); nc = e && *e == '1'; }\n"
                                    "            ex->pipe.depth_clamp = !nc && ((const p_flag *)a)->value != 0; break;\n"
                                    "        }\n", 1)
        old_dr = "            ex->depth_range[0] = r->n, ex->depth_range[1] = r->f;\n"
        assert old_dr in text, "vk_exec.c: OP_DEPTH_RANGE changed"
        text = text.replace(old_dr, old_dr + """            {
                static int td = -1, nd;
                if (td < 0) { const char *e = getenv("SW_TRACE_CLEAR"); td = e && *e == '1'; }
                if (td && nd < 100) { nd++; rt_log("depth_range: near %g far %g", (double)r->n, (double)r->f); }
            }
""", 1)
        old_po = "            ex->poly_offset = *(const p_poly_offset *)a;\n"
        assert old_po in text, "vk_exec.c: OP_POLY_OFFSET changed"
        text = text.replace(old_po, old_po + """            {
                static int tp = -1, np;
                if (tp < 0) { const char *e = getenv("SW_TRACE_CLEAR"); tp = e && *e == '1'; }
                if (tp && np < 100) { np++; rt_log("poly_offset: factor %g units %g clamp %g",
                    (double)ex->poly_offset.factor, (double)ex->poly_offset.units, (double)ex->poly_offset.clamp); }
            }
""", 1)
        old = "    float d = c->depth < 0.0f ? 0.0f : c->depth > 1.0f ? 1.0f : c->depth;\n"
        assert old in text, "vk_exec.c: clear_ds changed"
        text = text.replace(old, old + """    {
        static int tc = -1, n;
        if (tc < 0) { const char *e = getenv("SW_TRACE_CLEAR"); tc = e && *e == '1'; }
        if (tc && n < 200) { n++; rt_log("clear_ds: depth %g (raw %g) mask %d stencil %d/%#x target %ux%u nvn %#x",
                                       (double)d, (double)c->depth, c->depth_mask, c->stencil, c->stencil_mask,
                                       t->width, t->height, t->nvn_format); }
    }
""", 1)
    if name == "vk_dump.c":   # [port] our compat headers typedef u8
        # [port] SW_DUMP_C6=1: every draw's vertex-stage c6 translation (object position, camera relative)
        old_pd = "    pass_draws++;\n"
        assert text.count(old_pd) == 1, "vk_dump.c: pass_draws++ changed"
        text = text.replace(old_pd, old_pd + """    {
        static int c6 = -1;
        if (c6 < 0) { const char *e = getenv("SW_DUMP_C6"); c6 = e && *e == '1'; }
        const p_bind_buf *u = &ex->ubo[0][3];
        VkMemSpan ms;
        if (c6 && u->addr && u->size >= 64 && vk_mem_resolve(u->addr, 64, &ms) && ms.avail >= 64) {
            const float *f = (const float *)(uintptr_t)u->addr;
            fprintf(passes_f, "  draw %d vs %016llx c6 scale %g %g %g t %g %g %g\\n", pass_draws,
                    (unsigned long long)shader_hash(s->vs), f[0], f[5], f[10], f[12], f[13], f[14]);
        }
    }
""", 1)
        # [port] per pipeline also the raster state (why a pass writes nothing)
        old_fmt = 'prim %d\\n", (unsigned long long)shader_hash(s->vs)'
        assert old_fmt in text, "vk_dump.c: pipe line format changed"
        text = text.replace(old_fmt, 'prim %d rd %d dmw %d dt %d dw %d df %d cull %d front %d clamp %d '
                                     'va0 %#x va1 %#x va2 %#x str0 %u str1 %u str2 %u div0 %u div1 %u div2 %u va12 %#x va13 %#x va3 %#x va4 %#x va5 %#x va6 %#x\\n", '
                                     '(unsigned long long)shader_hash(s->vs)', 1)
        old_args = "s->prim);\n    describe_bindings"
        assert old_args in text, "vk_dump.c: pipe line args changed"
        text = text.replace(old_args, "s->prim, s->raster_discard, s->depth_minus_w, (int)s->ds.dt, (int)s->ds.dw, "
                                      "(int)s->ds.df, (int)s->poly.cull, (int)s->poly.front, s->depth_clamp, "
                                      "s->vattr[0], s->vattr[1], s->vattr[2], s->vstream[0].stride, s->vstream[1].stride, s->vstream[2].stride, "
                                      "s->vstream[0].divisor, s->vstream[1].divisor, s->vstream[2].divisor, s->vattr[12], s->vattr[13], s->vattr[3], s->vattr[4], s->vattr[5], s->vattr[6]);\n"
                                      "    describe_bindings", 1)
        import re as _re
        text = _re.sub(r"(?<![A-Za-z0-9_])u8\(", "dump_u8(", text)
    if name == "vk_exec_xfer.c":
        text = sub_once(text, "(uint64_t)((unsigned __int128)kern_now_ns() * 12 / 625)", "rt_ticks()", name)
    if name == "vk_exec_xfer.c":
        # [port] SW_COPY_CLAMP (default on): a copy whose region starts outside the level is clamped into it
        # instead of skipped.  Arceus' final scene-to-window copy arrives with source offsets {1, INT_MAX}
        # (cause not found yet); skipping it left the window grey.  Diagnostic workaround (git history: PROGRESS.md).
        text = sub_once(text, "    int is3d = img->type == VK_IMAGE_TYPE_3D;\n    uint32_t layers = is3d ? 1",
                        "    if (cfg_bool(\"COPY_CLAMP\") && s->level < img->levels) {\n"
                        "        if (s->x < 0 || (uint32_t)s->x >= lw) s->x = 0;\n"
                        "        if (s->y < 0 || (uint32_t)s->y >= lh) s->y = 0;\n"
                        "        if ((uint64_t)s->x + s->w > lw) s->w = lw - (uint32_t)s->x;\n"
                        "        if ((uint64_t)s->y + s->h > lh) s->h = lh - (uint32_t)s->y;\n"
                        "    }\n"
                        "    int is3d = img->type == VK_IMAGE_TYPE_3D;\n    uint32_t layers = is3d ? 1", name)
    if name == "vk_pipeline.c":
        # [arceus] pipeline-keys.bin: record (and load for the boot prewarm) the pipeline states met in play whether
        # or not async pipelines are on (upstream: only with A.on); TOTK_PIPE_KEYS=0 turns recording off
        text = sub_once(text, "static void keys_note(const VkPipeState *s) {\n    if (!A.on || !K.path[0]) return;",
                        "static int keys_rec_on(void) {   // [arceus] independent of async pipelines\n"
                        "    const char *e = cfg_get(\"TOTK_PIPE_KEYS\");\n"
                        "    return !(e && !strcmp(e, \"0\"));\n"
                        "}\n"
                        "static void keys_note(const VkPipeState *s) {\n    if (!keys_rec_on() || !K.path[0]) return;", name)
        text = sub_once(text, "static void keys_open(void) {\n    if (!A.on || !P.cache_path[0]) return;",
                        "static int keys_rec_on(void);\n"
                        "static void keys_open(void) {\n    if (!keys_rec_on() || !P.cache_path[0]) return;", name)
        text = sub_once(text, ", rt_game_version);", ", 0);", name)   # selftest message only
    if name == "gfx_shader.c":
        # the translator is build_work/xlate/sw_xlate.dll (Engine/Tools/build_xlate.py), run through dotnet
        text = sub_once(text, 'snprintf(probe, sizeof probe, "%s/build/xlate/totk_xlate" PLAT_EXE_SUFFIX, exe);',
                        'snprintf(probe, sizeof probe, "%s/build_work/xlate/sw_xlate.dll", exe);', name)
        text = sub_once(text, 'snprintf(xlate_exe, sizeof xlate_exe, "%s/build/xlate/totk_xlate" PLAT_EXE_SUFFIX, root);',
                        'snprintf(xlate_exe, sizeof xlate_exe, "%s/build_work/xlate/sw_xlate.dll", root);', name)
        text = sub_once(text, '    v->child = plat_child_start(xlate_exe, "--serve", &err);',
                        '    size_t xl = strlen(xlate_exe);\n'
                        '    if (xl > 4 && !_stricmp(xlate_exe + xl - 4, ".dll")) {   // [port] a .NET assembly: dotnet <dll> --serve\n'
                        '        char arg[PATH_CAP + 16];\n'
                        '        snprintf(arg, sizeof arg, "\\"%s\\" --serve", xlate_exe);\n'
                        '        const char *dn = cfg_get("DOTNET");\n'
                        '        v->child = plat_child_start(dn && *dn ? dn : "dotnet", arg, &err);\n'
                        '    } else {\n'
                        '        v->child = plat_child_start(xlate_exe, "--serve", &err);\n'
                        '    }', name)
    if name == "gfx_shader.c":
        # keep the job of a failed translation (one request line + its code) to rerun the translator by hand
        text = sub_once(text, "    int kept = keep && a && a->mvprev;",
                        "    int kept = (keep && a && a->mvprev) || (ok && strncmp(reply, \"ok \", 3));   // [port] failures too",
                        name)
    if name == "gfx_shader.c":
        # [arceus] transform feedback: the layout goes into the job; a capturing vertex shader gets its own cache key
        # (older translations of the same code were made without capture)
        text = sub_once(text, '    if (!strcmp(stage, "ps") && (control[0x718] & 1)) o += sprintf(o, ",\\"early_fragment_tests\\":true");',
                        '    if (!strcmp(stage, "ps") && (control[0x718] & 1)) o += sprintf(o, ",\\"early_fragment_tests\\":true");'
                        + TF_JOB, name)
        text = sub_once(text, "    put_u32(&p, (uint32_t)stage);\n    memcpy(p, control, CONTROL_SIZE), p += CONTROL_SIZE;",
                        "    put_u32(&p, (uint32_t)stage | (stage == GFX_STAGE_VS && control[0x6ed] == 1 ? 0x58460000u : 0u));"
                        "   // [arceus] TF\n    memcpy(p, control, CONTROL_SIZE), p += CONTROL_SIZE;", name)
        # [arceus] record the shader variants met in play (shader-answers.bin + shader-programs.bin) in the shader
        # cache folder, independent of async pipelines (upstream only starts it from vk_pipeline.c keys_open when
        # A.on). Inputs for a later offline re-translation; TOTK_SHADER_RECORD=0 turns it off.
        text = sub_once(text, "    if (cache_dir[0]) pack_open();\n    use_preload_start();\n    return;\n}",
                        "    if (cache_dir[0]) pack_open();\n    use_preload_start();\n"
                        "    {   // [arceus] variants met in play -> <shader cache>/shader-answers.bin, shader-programs.bin\n"
                        "        const char *rec = cfg_get(\"TOTK_SHADER_RECORD\");\n"
                        "        if (cache_dir[0] && !(rec && !strcmp(rec, \"0\"))) {\n"
                        "            char kp[PATH_CAP + 32];\n"
                        "            snprintf(kp, sizeof kp, \"%s/pipeline-keys.bin\", cache_dir);   // only its folder is used\n"
                        "            gfx_shader_record_at(kp);\n"
                        "        }\n"
                        "    }\n"
                        "    return;\n}", name)
    if name == "vk_exec.h":
        text = sub_once(text, "    int has_render_cond;\n",
                        "    int has_render_cond;\n\n"
                        "    // ---- [arceus] transform feedback (OP_TF_BUF / OP_TF_BEGIN / OP_TF_END; vk_exec_draw.c emulates it)\n"
                        "    p_tf_buf tf_buf[4];\n"
                        "    int tf_active;\n"
                        "    uint32_t tf_pos[4];                  // bytes written per buffer since Begin\n"
                        "    uint32_t tf_vcount;                  // vertex count of the draw being prepared\n", name)
    if name == "vk_exec.c":
        text = sub_once(text, "        case OP_BARRIER: vk_exec_barrier(ex); break;\n",
                        "        case OP_BARRIER: vk_exec_barrier(ex); break;\n"
                        "        case OP_TF_BUF: {   // [arceus] transform feedback\n"
                        "            const p_tf_buf *t = a;\n"
                        "            if (t->index >= 0 && t->index < 4) ex->tf_buf[t->index] = *t;\n"
                        "            { static int tl = -1, nl; if (tl < 0) { const char *e = getenv(\"SW_TRACE_TF\"); tl = e && *e == '1'; }\n"
                        "              if (tl && nl < 300) { nl++; rt_log(\"tf_buf %d addr %#llx size %#llx\", t->index,\n"
                        "                  (unsigned long long)t->addr, (unsigned long long)t->size); } }\n"
                        "            break;\n"
                        "        }\n"
                        "        case OP_TF_BEGIN:\n"
                        "            if (((const p_flag *)a)->value != 2) memset(ex->tf_pos, 0, sizeof ex->tf_pos);   // 2: resume\n"
                        "            ex->tf_active = 1;\n"
                        "            break;\n"
                        "        case OP_TF_END:   // the draws that read the captured vertices see them (storage write -> vertex read)\n"
                        "            ex->tf_active = 0;\n"
                        "            vk_exec_barrier(ex);\n"
                        "            break;\n", name)
    if name == "vk_exec_draw.c":
        text = sub_once(text, "static void bind_resources(VkExec *ex, const VkPipeInfo *info) {\n",
                        TF_EXEC + SKIP_VS + SKIP_T0 + "static void bind_resources(VkExec *ex, const VkPipeInfo *info) {\n", name)
        text = sub_once(text, "    vk_mvsurvey_draw(ex);\n",
                        "    if (skip_vs(s.vs)) return 0;   // [arceus] SW_SKIP_VS debugging\n"
                        "    if (skip_t0(ex, &s)) return 0;   // [arceus] SW_SKIP_T0 debugging\n    vk_mvsurvey_draw(ex);\n", name)
        text = sub_once(text, "    int skip = gfx ? bind_skip_on() : 0, same = 0, was_valid = B.res_valid;",
                        "    if (ex->tf_active) B.res_valid = 0;   // [arceus] capturing draws rebuild their descriptors\n"
                        "    int skip = gfx && !ex->tf_active ? bind_skip_on() : 0, same = 0, was_valid = B.res_valid;", name)
        text = sub_once(text, "        if (b->kind == VK_PIPE_SUPPORT) {\n            float in[3];\n            support_in(ex, in);\n"
                        "            if (!B.sup_valid",
                        "        if (b->kind == VK_PIPE_SUPPORT && ex->tf_active) {   // [arceus] a fresh one with the TF offsets\n"
                        "            float in[3];\n"
                        "            support_in(ex, in);\n"
                        "            VkBuffer sb;\n"
                        "            VkDeviceSize so;\n"
                        "            uint8_t *t = scratch(ex, SUPPORT_BYTES, &sb, &so);\n"
                        "            support_fill(t, in[0], in[1], in[2]);\n"
                        "            int32_t off[4], vc = (int32_t)ex->tf_vcount;\n"
                        "            for (int q = 0; q < 4; q++) off[q] = (int32_t)((tf_lead(ex, q) + ex->tf_pos[q]) / 4);\n"
                        "            memcpy(t + 1360, off, sizeof off);   // tfe_offset (words)\n"
                        "            memcpy(t + 1376, &vc, sizeof vc);    // tfe_vertex_count\n"
                        "            B.sup_valid = 0;\n"
                        "            o->buffer = (VkDescriptorBufferInfo){sb, so, SUPPORT_BYTES};\n"
                        "        } else if (b->kind == VK_PIPE_SUPPORT) {\n            float in[3];\n            support_in(ex, in);\n"
                        "            if (!B.sup_valid", name)
        text = sub_once(text, "        } else if (b->kind == GFX_RES_SSBO) {\n",
                        "        } else if (b->kind == GFX_RES_SSBO && r && r->slot <= -100) {   // [arceus] tfe_data0-3\n"
                        "            o->buffer = tf_info(ex, -100 - r->slot);\n"
                        "        } else if (b->kind == GFX_RES_SSBO) {\n", name)
        text = sub_once(text, "    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, 0)) return;\n",
                        "    ex->tf_vcount = d->count > 0 ? (uint32_t)d->count : 0;   // [arceus] TF\n"
                        "    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, 0)) {\n"
                        "        tf_advance(ex, d->count, d->instances);\n"
                        "        return;\n"
                        "    }\n", name)
        text = sub_once(text, "        vkCmdDraw(ex->cb, n, inst, first, (uint32_t)d->base_instance);\n    }\n    ex->recorded++;\n",
                        "        vkCmdDraw(ex->cb, n, inst, first, (uint32_t)d->base_instance);\n    }\n    ex->recorded++;\n"
                        "    tf_advance(ex, d->count, d->instances);   // [arceus] TF\n", name)
    if name == "gfx_window_win.c":
        text = sub_once(text, 'L"TotKRecompWindow"', 'L"ArceusRecompWindow"', name)
        text = sub_once(text, 'L"TotK Recomp"', 'L"LodeStar"', name)   # neutral product name (2026-11-25)
    if name == "vk_device.c":
        text = text.replace('"TotK Recomp"', '"Arceus Recomp"')
    # weak defaults whose strong definitions are always linked
    if name == "vk_exec.c":
        text = cut_function(text, r"^WEAK void vk_exec_op_draw\(", name)
        text = cut_function(text, r"^WEAK void vk_exec_op_xfer\(", name)
    if name == "vk_sync.c":
        text = cut_function(text, r"^WEAK int vk_exec_record\(", name)
    return text


if __name__ == "__main__":
    sys.exit(main())
