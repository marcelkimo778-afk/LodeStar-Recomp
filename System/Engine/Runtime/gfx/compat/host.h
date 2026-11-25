/* Host runtime interface expected by the graphics layer ported from
 * totk-pc-recomp (MIT, see ../LICENSE.totk-pc-recomp), implemented over this
 * project's runtime (Engine/Runtime/src, sw_rt.h) in ../sw_gfx_glue.c.
 * Declarations follow the original runtime/host.h; the parts of it that are
 * about TotK's own kernel, loader and patches are left out.
 *
 * Guest addresses are host addresses (sw_runtime.c reserves the guest's
 * 39-bit space at start-up and commits pages at their own address), so guest
 * pointers can be dereferenced directly, as the original code expects. */
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"

/* ---- logging and results */
void rt_log(const char *fmt, ...);              /* "[gfx] <msg>\n" to stderr */
void rt_logf(const char *fmt, ...);
void rt_vlogf(const char *fmt, va_list ap);
void rt_log_raw(const char *p, size_t n);
const char *rt_log_path(void);
__declspec(noreturn) void rt_fatal(const char *fmt, ...);
enum { RT_TRACE_SVC = 1, RT_TRACE_HOSTCALL = 2, RT_TRACE_IPC = 4, RT_TRACE_FS = 8, RT_TRACE_GFX = 16, RT_TRACE_PERF = 32,
       RT_TRACE_SCHED = 64, RT_TRACE_AUDIO = 128, RT_TRACE_PRESENT = 256 };
extern unsigned rt_trace;                       /* SW_GFX_TRACE=gfx,perf,present (or all) */
#define RT_RESULT(module, desc) ((uint32_t)(module) | ((uint32_t)(desc) << 9))
void rt_sha256(const void *p, size_t n, uint8_t out[32]);

/* ---- profiling (no-op implementations) */
enum { PERF_PACE, PERF_FENCE, PERF_DRAIN, PERF_PIPE, PERF_XLATE, PERF_TEX, PERF_ASTC, PERF_N };
uint64_t perf_begin(void);
void perf_end(int kind, uint64_t t0);
void perf_replay(uint64_t t0);
void perf_replay_thread(void);
void perf_main_thread(void);
enum { PERF_CNT_PIPE_ASYNC, PERF_CNT_PIPE_SKIP, PERF_CNT_TEX_UP, PERF_CNT_TEX_REUP, PERF_CNT_SHADER_SKIP,
       PERF_CNT_PIPE_WAIT, PERF_CNT_ONESHOT_WAIT, PERF_CNT_PIPE_BUILT, PERF_CNT_TEX_NEW, PERF_CNT_TEX_ALIAS,
       PERF_CNT_WB, PERF_CNT_WB_SKIP, PERF_CNT_N };
void perf_count(int kind);
void perf_present(uint64_t frame);
void perf_summary_flush(void);
void gfx_exit_log(void);
void perf_gpu_wait(uint64_t t0);
typedef struct {
    int have_gpu, have_vram, vram_exact;
    uint64_t gpu_ns, gpu_parts, gpu_missed;
    uint64_t vram_used, vram_budget;
    int have_idle;
    uint64_t idle_ns, starved_ns, queued_ns;
    uint64_t idle_gaps, max_gap_ns;
} PerfGpu;
extern void (*perf_gpu_stats)(PerfGpu *out);
void perf_set_overlay(int mode);
int perf_gpu_timing(void);
void perf_gpu_busy(uint64_t ns);
uint64_t perf_gpu_busy_total(void);

/* ---- guest address map */
#define RT_ADDRESS_LIMIT (1ull << 39)

/* ---- guest memory: write watch over the identity-mapped guest space */
/* GetWriteWatch(RESET) over [addr, addr+size): fills pages[] with the 4 KiB
 * pages written since the last call, returns their count (at most max), or
 * UINT64_MAX when the range is not watched (callers treat it as written). */
uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max);
uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max);
/* region freezing (the original's write-watch optimisation): never frozen here */
int mem_freeze(uint64_t a);
int mem_frozen(uint64_t a);
void mem_thaw(uint64_t a, uint64_t n);
void mem_freeze_stats(uint64_t *frozen, uint64_t *freezes, uint64_t *fault_thaws, uint64_t *other_thaws);

/* ---- time */
uint64_t kern_now_ns(void);                     /* monotonic */
uint64_t rt_ticks(void);                        /* 19.2 MHz guest counter */
Cpu *kern_cpu(void);                            /* current guest thread's Cpu (NULL on host threads) */

/* ---- guest calls */
/* host -> guest call of fn with n (<= 8) integer arguments; returns x0. */
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...);

/* ---- cores: around host waits (GPU, display, shader translation) the calling
 * guest thread gives up its emulated core and the kernel lock (SW_MULTICORE;
 * fiber mode: nothing to give up, returns 0).  sw_thread.c */
int kern_core_away(void);
void kern_core_back(int *p);

/* ---- frames (rt_frame_presented is declared in gfx.h) */
/* TotK's real-time frame step is not used: presents follow the window's
 * swap interval at 60 Hz. */
static inline int frame_step_realtime(void) { return 0; }
static inline uint64_t frame_step_cap_ns(void) { return 16666667ull; }
/* Arceus: the display time of each present feeds the 60 fps world step (src/sw_hooks.c sw_frame_step_present). */
void sw_frame_step_present(uint64_t slot);
static inline void frame_step_present(uint64_t slot, int interval) { (void)interval; sw_frame_step_present(slot); }
static inline double frame_step_limit_hz(void) { return 60.0; }

/* ---- A/B switches of the original (always A here) */
int rt_ab(void);
int rt_knob(const char *name, const char *def);
#define RT_KNOB_ON(mode) ((mode) == 2 ? rt_ab() : (mode))

/* ---- arguments (AAPCS64) */
static inline uint64_t arg_x(const Cpu *c, int i)
{
    if (i < 8) return c->x[i];
    return *(const uint64_t *)(uintptr_t)(c->sp + 8 * (uint64_t)(i - 8));
}
static inline void *arg_p(const Cpu *c, int i) { return (void *)(uintptr_t)arg_x(c, i); }
static inline float arg_s(const Cpu *c, int i) { return c->v[i].f[0]; }
static inline double arg_d(const Cpu *c, int i) { return c->v[i].df[0]; }
static inline void ret_x(Cpu *c, uint64_t v) { c->x[0] = v; }
static inline void ret_s(Cpu *c, float f) { memset(&c->v[0], 0, sizeof c->v[0]); c->v[0].f[0] = f; }
static inline void ret_d(Cpu *c, double d) { memset(&c->v[0], 0, sizeof c->v[0]); c->v[0].df[0] = d; }

/* ---- configuration: environment variables */
typedef struct { const char *gpu; } RtConfig;
const RtConfig *rt_config(void);                /* SW_GPU=null|vulkan */
