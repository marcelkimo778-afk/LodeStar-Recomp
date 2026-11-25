/* Glue between the ported graphics layer (totk-pc-recomp, MIT) and this
 * runtime: logging, time, guest calls, settings, frame hooks and the NVN
 * proc table that sw_nvn.c consults. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "sw_window.h"

u64 sw_tick_now(void);

/* ---- logging */
unsigned rt_trace;

static void trace_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    const char *t = cfg_get("GFX_TRACE");
    if (!t) return;
    if (strstr(t, "all")) rt_trace = ~0u;
    if (strstr(t, "gfx")) rt_trace |= RT_TRACE_GFX;
    if (strstr(t, "perf")) rt_trace |= RT_TRACE_PERF;
    if (strstr(t, "present")) rt_trace |= RT_TRACE_PRESENT;
}

void rt_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[gfx] %s\n", buf);
}

void rt_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

__declspec(noreturn) void rt_fatal(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n=== GRAPHICS FATAL ===\n%s\n", buf);
    fflush(stderr);
    exit(71);
}

void rt_vlogf(const char *fmt, va_list ap) { vfprintf(stderr, fmt, ap); }
void rt_log_raw(const char *p, size_t n) { fwrite(p, 1, n, stderr); }
const char *rt_log_path(void) { return NULL; }

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
void rt_sha256(const void *p, size_t n, uint8_t out[32])
{
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)p, (ULONG)n, out, 32) != 0)
        rt_fatal("rt_sha256: BCryptHash failed");
}

/* ---- profiling: not collected in this port */
/* perf_begin / perf_end: the time each thread spends per PERF_* kind (pacing, GPU fence waits, pipeline builds, shader
 * translation, texture uploads, ASTC).  Only with SW_THUNK_SLOW (sw_runtime.c prints the split of a slow host call);
 * otherwise free. */
static int g_perf_on = -1;
static __declspec(thread) uint64_t t_perf_ticks[PERF_N];
uint64_t perf_begin(void)
{
    if (g_perf_on < 0) { const char *e = getenv("SW_THUNK_SLOW"); g_perf_on = e && atof(e) > 0; }
    if (!g_perf_on) return 0;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return (uint64_t)q.QuadPart;
}
void perf_end(int kind, uint64_t t0)
{
    if (!t0 || kind < 0 || kind >= PERF_N) return;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    t_perf_ticks[kind] += (uint64_t)q.QuadPart - t0;
}
/* texture uploads of this thread (count, guest bytes) for the SW_THUNK_SLOW report */
static __declspec(thread) uint64_t t_up_n, t_up_bytes;
void sw_tex_up_note(uint64_t bytes) { t_up_n++, t_up_bytes += bytes; }
void sw_tex_up_counts(uint64_t *n, uint64_t *bytes) { *n = t_up_n, *bytes = t_up_bytes; }
/* this thread's totals per kind in ms (QPC based) */
void sw_perf_thread_ms(double out[PERF_N])
{
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    for (int i = 0; i < PERF_N; i++) out[i] = (double)t_perf_ticks[i] * 1000.0 / (double)f.QuadPart;
}
void perf_replay(uint64_t t0) { (void)t0; }
void perf_replay_thread(void) {}
void perf_main_thread(void) {}
/* counters (pipelines built, ...) for the performance overlay (ui/sw_perfhud.cpp): one atomic add */
static volatile LONG64 g_perf_cnt[PERF_CNT_N];
void perf_count(int kind) { if (kind >= 0 && kind < PERF_CNT_N) InterlockedIncrement64(&g_perf_cnt[kind]); }
uint64_t sw_perf_count_get(int kind) { return kind >= 0 && kind < PERF_CNT_N ? (uint64_t)g_perf_cnt[kind] : 0; }
void perf_present(uint64_t frame) { (void)frame; }
void perf_summary_flush(void) {}
#ifndef SW_HAVE_VULKAN
void gfx_exit_log(void) {}   /* vk_backend.c has the real one */
#endif
void perf_gpu_wait(uint64_t t0) { (void)t0; }
void (*perf_gpu_stats)(PerfGpu *out);
void perf_set_overlay(int mode) { (void)mode; }
/* SW_GPU_TIME=1 or the detailed performance overlay: GPU timestamps around every submission (vk_sync.c); the [perf]
 * line then shows GPU busy ms per frame. Off by default (it costs a query pair
 * per command buffer). */
int sw_perfhud_gpu_wanted(void);   /* ui/sw_perfhud.cpp: the detailed performance overlay is shown */
int perf_gpu_timing(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("SW_GPU_TIME"); on = e && *e == '1'; }
    return on || sw_perfhud_gpu_wanted();
}
static uint64_t g_gpu_busy;
void perf_gpu_busy(uint64_t ns) { g_gpu_busy += ns; }
uint64_t perf_gpu_busy_total(void) { return g_gpu_busy; }

int rt_ab(void) { return 0; }
int rt_knob(const char *name, const char *def)
{
    const char *v = cfg_get(name);
    if (!v) v = def;
    if (!v || !*v || !strcmp(v, "0")) return 0;
    return !_stricmp(v, "ab") ? 2 : 1;
}

/* ---- guest memory write watch (sw_runtime.c reserves guest space with
 * MEM_WRITE_WATCH; a range outside such a reservation reports UINT64_MAX) */
static uint64_t written(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max, DWORD flags)
{
    if (!size) return 0;
    ULONG_PTR count = (ULONG_PTR)max;
    ULONG gran = 0;
    uint64_t lo = addr & ~0xFFFull, hi = (addr + size + 0xFFF) & ~0xFFFull;
    extern uint64_t sw_reserve_chunk(void);
    uint64_t ch = sw_reserve_chunk();
    if (ch && lo / ch != (hi - 1) / ch) {
        /* the guest space is reserved in blocks of ch bytes (SW_RESERVE_CHUNK_MB) and GetWriteWatch fails on a range
         * that crosses a reservation: ask each block's part and join the page lists */
        uint64_t got = 0;
        for (uint64_t s = lo; s < hi;) {
            uint64_t e = (s / ch + 1) * ch;
            if (e > hi) e = hi;
            ULONG_PTR c = (ULONG_PTR)(max - got);
            if (GetWriteWatch(flags, (void *)(uintptr_t)s, (SIZE_T)(e - s), (PVOID *)(pages + got), &c, &gran) != 0)
                return UINT64_MAX;
            got += c;
            if (max && got >= max) return got;        /* full: the caller asks again (the rest stays marked) */
            s = e;
        }
        return got;
    }
    if (GetWriteWatch(flags, (void *)(uintptr_t)lo, (SIZE_T)(hi - lo), (PVOID *)pages, &count, &gran) != 0)
        return UINT64_MAX;
    if (count == max && max) {
        /* more pages than fit: the caller asks again, so keep the rest marked */
        return count;
    }
    return (uint64_t)count;
}
uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max)
{ return written(addr, size, pages, max, WRITE_WATCH_FLAG_RESET); }
uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max)
{ return written(addr, size, pages, max, 0); }
/* ---- frozen regions (ref/host.h mem_freeze): a 2 MiB guest region that nothing wrote for a while (vk_memory.c
 * freeze_pass decides) is made read-only, and the write-watch queries skip it (they were ~1 ms a frame on the render
 * thread: every frame asked GetWriteWatch about all ~370 MiB of pool memory, mostly static meshes and textures).  The
 * first write to it faults: the hook below makes it writable again, the write runs again and sets its write-watch
 * bit as usual.  Kernel writes into guest memory would fail instead of faulting; the runtime has none (file reads go
 * through a host buffer and a memcpy).  SW_FREEZE=0 turns it off. */
#define FZ_SHIFT 21
#define FZ_N (1u << (39 - FZ_SHIFT))
static volatile LONG g_fz[FZ_N];             /* 1: frozen (read-only) */
static SRWLOCK g_fz_lock = SRWLOCK_INIT;     /* protection changes */
static volatile LONG64 g_fz_now, g_fz_freezes, g_fz_fault_thaws, g_fz_other_thaws;
static volatile LONG g_fz_ever;           /* a region was frozen at least once */
static int g_fz_on = -1;

static void fz_thaw_region(uint32_t i, volatile LONG64 *counter)
{
    AcquireSRWLockExclusive(&g_fz_lock);
    if (g_fz[i]) {
        DWORD old;
        if (VirtualProtect((void *)((uintptr_t)i << FZ_SHIFT), (SIZE_T)1 << FZ_SHIFT, PAGE_READWRITE, &old)) {
            InterlockedExchange(&g_fz[i], 0);
            InterlockedDecrement64(&g_fz_now);
            InterlockedIncrement64(counter);
        }
    }
    ReleaseSRWLockExclusive(&g_fz_lock);
}

/* a write access violation (platform_win32.c fault_veh, ahead of the crash handler): 1 = handled, run the write again */
static int fz_fault(uint64_t addr)
{
    if (addr >> 39) return 0;
    uint32_t i = (uint32_t)(addr >> FZ_SHIFT);
    if (g_fz[i]) { fz_thaw_region(i, &g_fz_fault_thaws); return 1; }
    /* another thread thawed it between this write's fault and here: the page is writable now, so run the write again */
    MEMORY_BASIC_INFORMATION mi;
    if (g_fz_ever && VirtualQuery((void *)(uintptr_t)addr, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
        mi.Protect == PAGE_READWRITE)
        return 1;
    return 0;
}

/* the whole region committed read-write in one private (write-watched) reservation */
static int fz_region_ok(uint64_t a)
{
    uint64_t e = a + (1ull << FZ_SHIFT);
    MEMORY_BASIC_INFORMATION mi;
    void *base = NULL;
    for (uint64_t p = a; p < e; p = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize) {
        if (!VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi)) return 0;
        if (mi.State != MEM_COMMIT || mi.Protect != PAGE_READWRITE || mi.Type != MEM_PRIVATE) return 0;
        if (!base) base = mi.AllocationBase;
        else if (mi.AllocationBase != base) return 0;
    }
    return 1;
}

int mem_freeze(uint64_t a)
{
    if (g_fz_on < 0) {
        const char *e = getenv("SW_FREEZE");
        g_fz_on = !(e && *e == '0');
        if (g_fz_on) plat_mem_fault_hook(fz_fault);   /* registered first: runs before the crash handler */
        fprintf(stderr, "[mem] frozen regions %s\n", g_fz_on ? "on (SW_FREEZE=0 turns them off)" : "off (SW_FREEZE=0)");
    }
    if (!g_fz_on || (a & ((1ull << FZ_SHIFT) - 1)) || (a >> 39)) return 0;
    uint32_t i = (uint32_t)(a >> FZ_SHIFT);
    if (g_fz[i]) return 1;
    int ok = 0;
    AcquireSRWLockExclusive(&g_fz_lock);
    if (g_fz[i]) ok = 1;
    else if (fz_region_ok(a)) {
        DWORD old;
        if (VirtualProtect((void *)(uintptr_t)a, (SIZE_T)1 << FZ_SHIFT, PAGE_READONLY, &old)) {
            InterlockedExchange(&g_fz[i], 1);
            InterlockedIncrement64(&g_fz_now);
            InterlockedIncrement64(&g_fz_freezes);
            InterlockedExchange(&g_fz_ever, 1);
            ok = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_fz_lock);
    return ok;
}
int mem_frozen(uint64_t a) { return !(a >> 39) && g_fz[(uint32_t)(a >> FZ_SHIFT)] != 0; }
void mem_thaw(uint64_t a, uint64_t n)
{
    if (!n || !g_fz_ever) return;
    uint64_t e = a + n;
    if (e > (1ull << 39)) e = 1ull << 39;
    for (uint64_t r = a >> FZ_SHIFT; (r << FZ_SHIFT) < e; r++)
        if (g_fz[(uint32_t)r]) fz_thaw_region((uint32_t)r, &g_fz_other_thaws);
}
void mem_freeze_stats(uint64_t *frozen, uint64_t *freezes, uint64_t *fault_thaws, uint64_t *other_thaws)
{
    if (frozen) *frozen = (uint64_t)g_fz_now;
    if (freezes) *freezes = (uint64_t)InterlockedExchange64(&g_fz_freezes, 0);
    if (fault_thaws) *fault_thaws = (uint64_t)InterlockedExchange64(&g_fz_fault_thaws, 0);
    if (other_thaws) *other_thaws = (uint64_t)InterlockedExchange64(&g_fz_other_thaws, 0);
}

/* current guest thread's CPU state: the runtime keeps one SwCpu per guest
 * fiber; sw_thread.c reports it */
SwCpu *sw_thread_current_cpu(void);
Cpu *kern_cpu(void) { return sw_thread_current_cpu(); }

/* ---- time */
uint64_t kern_now_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
}

/* Frame-pacing wait (nvnWindowAcquireTexture, via port_gfx.py): the other guest
 * threads run until the deadline (sw_thread_wait_until_ns); the last fraction
 * of a millisecond is spun so the present cadence stays exact.
 * SW_PACE_WAIT=sleep restores the old plain host sleep. */
void sw_thread_wait_until_ns(unsigned long long deadline_ns, unsigned long long now_ns);
void sw_gfx_wait_until(uint64_t deadline)
{
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("SW_PACE_WAIT"); mode = e && !strcmp(e, "sleep") ? 0 : 1; }
    for (;;) {
        uint64_t now = kern_now_ns();
        if (now >= deadline) return;
        uint64_t d = deadline - now;
        if (mode && d > 1500000) sw_thread_wait_until_ns(deadline - 500000, now);
        else if (d >= 2000000) Sleep((DWORD)(d / 1000000 - 1));
        else SwitchToThread();
    }
}

uint64_t rt_ticks(void) { return sw_tick_now(); }

/* ---- guest calls: x0.. = arguments, sw_call runs the function, x0 = result */
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n && i < 8; i++) c->x[i] = va_arg(ap, uint64_t);
    va_end(ap);
    u64 saved_pc = c->pc;
    sw_call(c, fn);
    c->pc = saved_pc;
    return c->x[0];
}

/* ---- settings (sw_cfg.c holds cfg_*) */
const RtConfig *rt_config(void)
{
    static RtConfig cfg;
    static int done;
    if (!done) {
        done = 1;
        cfg.gpu = cfg_get("GPU");
        trace_init();
    }
    return &cfg;
}


/* ---- frames */
void rt_frame_presented(const tex_t *t, uint64_t n)
{
    (void)t;
    if (n == 1 || n % 300 == 0)
        fprintf(stderr, "nvn: present #%llu (%ux%u)\n", (unsigned long long)n,
                t ? t->d.width : 0, t ? t->d.height : 0);
    sw_window_present();
    {   /* [perf] every 5 s: fps, average and worst frame time, frame-time percentiles
         * and 1% low (SW_PERF=0 off); SW_FRAMETIME_LOG=<file>: every frame time
         * ("present ms" per line) for benchmarks; SW_GPU_TIME=1 adds GPU ms/frame. */
        enum { PF_MAX = 4096 };
        static LARGE_INTEGER f, last, win_start;
        static double worst, sum, pf[PF_MAX];
        static unsigned frames, slow;
        static int off = -1;
        static FILE *ftlog;
        if (off < 0) {
            const char *e = getenv("SW_PERF");
            off = e && *e == '0';
            QueryPerformanceFrequency(&f);
            e = getenv("SW_FRAMETIME_LOG");
            if (e && *e) ftlog = fopen(e, "w");
        }
        if (off) return;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (last.QuadPart) {
            double ms = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)f.QuadPart;
            if (frames < PF_MAX) pf[frames] = ms;
            sum += ms; frames++;
            if (ms > worst) worst = ms;
            if (ms > 50.0) slow++;                 /* hitch: below 20 fps for that frame */
            if (ftlog) fprintf(ftlog, "%llu %.3f\n", (unsigned long long)n, ms);
            {   /* SW_SPIKE_LOG=<ms>: a line for each frame slower than that, in order with the other log lines
                 * (what was logged just before a spike is what the frame waited for) */
                static double spike_ms = -1;
                if (spike_ms < 0) { const char *e = getenv("SW_SPIKE_LOG"); spike_ms = e && *e ? atof(e) : 0; }
                if (spike_ms > 0 && ms > spike_ms)
                    fprintf(stderr, "[spike] present #%llu took %.1f ms\n", (unsigned long long)n, ms);
            }
        } else win_start = now;
        last = now;
        double span = (double)(now.QuadPart - win_start.QuadPart) / (double)f.QuadPart;
        if (span >= 5.0 && frames) {
            unsigned m = frames < PF_MAX ? frames : PF_MAX;
            for (unsigned i = 1; i < m; i++) {       /* insertion sort: m is a few hundred */
                double v = pf[i];
                unsigned j = i;
                while (j && pf[j - 1] > v) { pf[j] = pf[j - 1]; j--; }
                pf[j] = v;
            }
            unsigned k1 = m / 100 ? m / 100 : 1;     /* 1% low: average fps of the slowest 1% of frames */
            double wsum = 0;
            for (unsigned i = m - k1; i < m; i++) wsum += pf[i];
            char gpu[64] = "";
            if (perf_gpu_timing() && perf_gpu_stats) {
                PerfGpu g;
                memset(&g, 0, sizeof g);
                perf_gpu_stats(&g);
                if (g.have_gpu) snprintf(gpu, sizeof gpu, ", gpu %.2f ms/frame", (double)g.gpu_ns / 1e6 / frames);
            }
            fprintf(stderr, "[perf] %.1f fps, frame avg %.1f ms, worst %.1f ms, hitches(>50ms) %u, present #%llu, "
                            "p50 %.1f p95 %.1f p99 %.1f ms, 1%% low %.1f fps%s\n",
                    frames / span, sum / frames, worst, slow, (unsigned long long)n,
                    pf[m / 2], pf[(m * 95) / 100], pf[(m * 99) / 100], 1000.0 * k1 / wsum, gpu);
            if (ftlog) fflush(ftlog);
            win_start = now; sum = worst = 0; frames = slow = 0;
        }
    }
}

/* ---- proc table: name -> implementation, for sw_nvn.c */
#define NVN_IMPL(n) void nvnp_##n(Cpu *c);
#include "nvn_impl_table.inc"
#undef NVN_IMPL

typedef struct { const char *name; void (*fn)(Cpu *); } GfxProc;
static const GfxProc g_gfx_procs[] = {
#define NVN_IMPL(n) {#n, nvnp_##n},
#include "nvn_impl_table.inc"
#undef NVN_IMPL
};

/* ---- SW_DUMP_CONTROL=1: research aid for transform feedback (GPU skinning).
 * Dumps every shader control section handed to nvnProgramSetShaders(Ext) to
 * ctrl_dump/p<program>_<n>_s<stage>.bin and logs, per command buffer, which
 * program is bound when BeginTransformFeedback is recorded and the buffers
 * bound for it.  Off by default; wraps the real procs, changes nothing. */
static void (*g_real_set_shaders)(SwCpu *), (*g_real_set_shaders_ext)(SwCpu *), (*g_real_bind_program)(SwCpu *),
            (*g_real_tf_begin)(SwCpu *), (*g_real_tf_bind)(SwCpu *), (*g_real_tf_binds)(SwCpu *);
static struct { u64 cmd, prog; } g_dc_bound[64];

static size_t readable(const void *p, size_t want)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    size_t left = (size_t)((const char *)mi.BaseAddress + mi.RegionSize - (const char *)p);
    return left < want ? left : want;
}

static void dc_dump_shaders(SwCpu *c, size_t stride)
{
    static unsigned n;
    u64 prog = c->x[0];
    int count = (int)c->x[1];
    const uint8_t *sd = (const uint8_t *)(uintptr_t)c->x[2];
    CreateDirectoryA("ctrl_dump", NULL);
    for (int i = 0; i < count && i < 6; i++) {
        const void *ctl;
        memcpy(&ctl, sd + (size_t)i * stride + 8, sizeof ctl);
        size_t len = readable(ctl, 0x4000);
        char path[MAX_PATH];
        snprintf(path, sizeof path, "ctrl_dump/p%llx_%u_s%d.bin", (unsigned long long)prog, n, i);
        FILE *f = len ? fopen(path, "wb") : NULL;
        if (f) { fwrite(ctl, 1, len, f); fclose(f); }
        /* the code too (NVNshaderData.data: a GPU address = host address of the pool storage) */
        u64 code;
        memcpy(&code, sd + (size_t)i * stride, sizeof code);
        len = readable((const void *)(uintptr_t)code, 0x10000);
        snprintf(path, sizeof path, "ctrl_dump/p%llx_%u_s%d.code", (unsigned long long)prog, n, i);
        f = len ? fopen(path, "wb") : NULL;
        if (f) { fwrite((const void *)(uintptr_t)code, 1, len, f); fclose(f); }
    }
    fprintf(stderr, "[dc] program %#llx: %d stages (dump #%u)\n", (unsigned long long)prog, count, n++);
}
static void dc_set_shaders(SwCpu *c) { dc_dump_shaders(c, 0x10); g_real_set_shaders(c); }
static void dc_set_shaders_ext(SwCpu *c) { dc_dump_shaders(c, 0x18); g_real_set_shaders_ext(c); }
static void dc_bind_program(SwCpu *c)
{
    unsigned slot = (unsigned)((c->x[0] >> 4) % 64);
    g_dc_bound[slot].cmd = c->x[0];
    g_dc_bound[slot].prog = c->x[1];
    g_real_bind_program(c);
}
static void dc_tf_begin(SwCpu *c)
{
    unsigned slot = (unsigned)((c->x[0] >> 4) % 64);
    static unsigned logged;
    if (logged++ < 200)
        fprintf(stderr, "[dc] TF begin cmd %#llx program %#llx control %#llx\n", (unsigned long long)c->x[0],
                (unsigned long long)(g_dc_bound[slot].cmd == c->x[0] ? g_dc_bound[slot].prog : 0),
                (unsigned long long)c->x[1]);
    if (g_real_tf_begin) g_real_tf_begin(c);
}
/* recent transform feedback ranges, to spot draws that read them as vertex buffers */
static struct { u64 lo, hi; } g_dc_tf[128];
static unsigned g_dc_ntf;
static void dc_note_tf(u64 addr, u64 size)
{
    g_dc_tf[g_dc_ntf % 128].lo = addr;
    g_dc_tf[g_dc_ntf % 128].hi = addr + size;
    g_dc_ntf++;
}
static void dc_check_vb(SwCpu *c, int index, u64 addr, u64 size)
{
    static unsigned logged, hits, misses;
    int hit = 0;
    for (unsigned i = 0; i < 128 && i < g_dc_ntf; i++)
        if (addr < g_dc_tf[i].hi && addr + size > g_dc_tf[i].lo) hit = 1;
    if (hit) hits++; else misses++;
    if (hit && logged++ < 40)
        fprintf(stderr, "[dc] vertex buffer %d %#llx+%#llx reads TF output (cmd %#llx; %u hits / %u other binds so far)\n",
                index, (unsigned long long)addr, (unsigned long long)size, (unsigned long long)c->x[0], hits, misses);
}
static void (*g_real_vb)(SwCpu *), (*g_real_vbs)(SwCpu *);
static void dc_vb(SwCpu *c) { dc_check_vb(c, (int)c->x[1], c->x[2], c->x[3]); g_real_vb(c); }
static void dc_vbs(SwCpu *c)
{
    const u64 *r = (const u64 *)(uintptr_t)c->x[3];   /* NVNbufferRange {address, size} */
    for (int i = 0; r && i < (int)c->x[2] && i < 16; i++) dc_check_vb(c, (int)c->x[1] + i, r[2 * i], r[2 * i + 1]);
    g_real_vbs(c);
}

static void dc_tf_bind(SwCpu *c)
{
    static unsigned logged;
    dc_note_tf(c->x[2], c->x[3]);
    if (logged++ < 200)
        fprintf(stderr, "[dc] TF buffer cmd %#llx index %llu addr %#llx size %#llx\n", (unsigned long long)c->x[0],
                (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
    if (g_real_tf_bind) g_real_tf_bind(c);
}
static void dc_tf_binds(SwCpu *c)
{
    static unsigned logged;
    if (logged++ < 50)
        fprintf(stderr, "[dc] TF buffers cmd %#llx first %llu n %llu\n", (unsigned long long)c->x[0],
                (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    if (g_real_tf_binds) g_real_tf_binds(c);
}

static void (*gfx_lookup(const char *name))(SwCpu *)
{
    size_t lo = 0, hi = sizeof g_gfx_procs / sizeof g_gfx_procs[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = strcmp(name, g_gfx_procs[mid].name);
        if (!r) return g_gfx_procs[mid].fn;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

/* NULL when the ported front end does not implement name (sorted table). */
void (*sw_gfx_nvn_impl(const char *name))(SwCpu *)
{
    rt_config();
    {   /* Async graphics pipelines (default; SW_PIPE_ASYNC=0 off): background workers build pipelines, a draw
         * meanwhile gets a fast-linked pipeline from the stage libraries (graphics pipeline library).  Off: draws
         * wait for vkCreateGraphicsPipelines.  totk boot.c does this at boot; here before the game's first NVN
         * lookup, so before any replay.  Then the boot prewarm (vk_pipeline_prewarm; TOTK_PIPE_PREBUILD=0 off):
         * the pipeline states recorded in earlier runs (pipeline-keys.bin next to pipeline-cache.bin) are built
         * on every core before the game draws.  It needs the async workers, so it runs only with async on. */
        static int async_done;
        if (!async_done) {
            async_done = 1;
            const char *e = getenv("SW_PIPE_ASYNC");
            if (!(e && *e == '0')) {
                /* Stage libraries of the variants recorded in play: built at boot on idle threads and KEPT
                 * (vk_pipeline.c "stage library prebuild"; upstream default 0 relies on the driver's disk cache,
                 * which on AMD rebuilds a library in 0.1 ms - on NVIDIA here fragment libraries still took
                 * 4.6 ms on average and up to 78 ms in play, and 202 draws found their libraries missing).
                 * ~140-460 KB each; TOTK_LIB_PREBUILD_KEEP=0 restores the upstream behaviour. */
                if (!getenv("TOTK_LIB_PREBUILD_KEEP") && !getenv("SW_LIB_PREBUILD_KEEP")) _putenv("TOTK_LIB_PREBUILD_KEEP=2000");
                void vk_pipeline_async_enable(void);
                void vk_pipeline_prewarm(void);
                vk_pipeline_async_enable();
                fprintf(stderr, "[gfx] async graphics pipelines on (SW_PIPE_ASYNC=0 turns them off)\n");
                LARGE_INTEGER f, t0, t1;
                QueryPerformanceFrequency(&f);
                QueryPerformanceCounter(&t0);
                vk_pipeline_prewarm();
                QueryPerformanceCounter(&t1);
                fprintf(stderr, "[gfx] pipeline prewarm took %.0f ms\n",
                        (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
            } else fprintf(stderr, "[gfx] async graphics pipelines off (SW_PIPE_ASYNC=0)\n");
        }
    }
    void (*fn)(SwCpu *) = gfx_lookup(name);
    static int dc = -1;
    if (dc < 0) { const char *e = getenv("SW_DUMP_CONTROL"); dc = e && *e == '1'; }
    if (dc) {
        static const struct { const char *name; void (**real)(SwCpu *); void (*wrap)(SwCpu *); } w[] = {
            {"nvnProgramSetShaders", &g_real_set_shaders, dc_set_shaders},
            {"nvnProgramSetShadersExt", &g_real_set_shaders_ext, dc_set_shaders_ext},
            {"nvnCommandBufferBindProgram", &g_real_bind_program, dc_bind_program},
            {"nvnCommandBufferBeginTransformFeedback", &g_real_tf_begin, dc_tf_begin},
            {"nvnCommandBufferBindTransformFeedbackBuffer", &g_real_tf_bind, dc_tf_bind},
            {"nvnCommandBufferBindTransformFeedbackBuffers", &g_real_tf_binds, dc_tf_binds},
            {"nvnCommandBufferBindVertexBuffer", &g_real_vb, dc_vb},
            {"nvnCommandBufferBindVertexBuffers", &g_real_vbs, dc_vbs},
        };
        for (size_t i = 0; i < sizeof w / sizeof w[0]; i++)
            if (!strcmp(name, w[i].name) && (fn || strstr(name, "TransformFeedback"))) { *w[i].real = fn; return w[i].wrap; }
    }
    return fn;
}

/* 1 when presents go through the Vulkan back end (its own window). */
int sw_gfx_uses_vulkan(void)
{
#ifdef SW_HAVE_VULKAN
    return gfx_backend() == &gfx_vulkan_backend;
#else
    return 0;
#endif
}

/* ---- sw_app.exe --prepare-shaders / --warm-shaders / --gpu-info ---------------
 * Offline shader-stage preparation.  Inputs, in priority order:
 *   1. variants recorded in play: <shader cache>/shader-answers.bin + shader-programs.bin (gfx_shader.c
 *      "answers met in play"; the exact states the game used)
 *   2. the census of the player's RomFS: census-answers.bin + census-programs.bin (Engine/Tools/
 *      shader_census.py; one default-state variant per shader found in .ptcl/.arc)
 * Both use the same record layout, so they are concatenated into seed-answers.bin / seed-programs.bin next
 * to pipeline-cache.bin and handed to the ported preparation unchanged (vk_pipeline_prepare_shaders): every
 * variant missing from the shader cache is translated (cached ones are reused), shader-prepared.bin is
 * written and each variant's stage library is built once into the GPU driver's own cache.  This prepares
 * shader STAGES; full pipelines (state combinations) are built at the first draw (async, fast-linked) or by
 * the optional boot prewarm from pipeline-keys.bin.  No game code runs.
 *
 * Safety: translations are content-addressed and written by rename, so an interrupted run leaves the old
 * entries valid.  <shader cache>/prepared.json is replaced (atomically) only after a finished run; a failed
 * or killed run keeps the previous one.  The seed copies are removed afterwards (a leftover one is replaced
 * by the next run and is never read by the game).
 * Launcher output (stdout): "[prep] <stage> ..." lines; translation progress is the stderr log line
 * "gfx_shader: prepare: N of M pairs" (every 5 s).  Exit code 0 = done, 1 = failed, 2 = nothing to prepare.
 * SW_PREP_XLATE_ID: the translator identity the launcher saw (stored in the manifest). */
#include "vk/vk.h"
int vk_pipeline_prepare_shaders(void);
int vk_pipeline_warm_shaders(void);

typedef struct { char sdir[PLAT_PATH_MAX * 2], pdir[PLAT_PATH_MAX * 2]; } PrepDirs;

static int prep_dirs(PrepDirs *d)
{
    char exe[PLAT_PATH_MAX * 2];
    if (!plat_exe_path(exe, sizeof exe) || !plat_last_sep(exe)) return 0;
    *plat_last_sep(exe) = 0;
    const char *sc = cfg_get("TOTK_SHADER_CACHE"), *pc = cfg_get("TOTK_PIPELINE_CACHE");
    if (sc) snprintf(d->sdir, sizeof d->sdir, "%s", sc); else snprintf(d->sdir, sizeof d->sdir, "%s/shader-cache", exe);
    if (pc) {                                         /* the folder of the pipeline cache (vk_pipeline.c cache_dir) */
        snprintf(d->pdir, sizeof d->pdir, "%s", pc);
        char *cut = plat_last_sep(d->pdir);
        if (cut) *cut = 0; else snprintf(d->pdir, sizeof d->pdir, ".");
    } else snprintf(d->pdir, sizeof d->pdir, "%s", exe);
    return 1;
}

static uint8_t *prep_read(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    *n = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = len > 0 ? malloc((size_t)len) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) free(b), b = NULL;
    fclose(f);
    if (b) *n = len;
    return b;
}

/* dst = header of the first valid source + the records of every valid source; returns the record bytes */
static long prep_merge(const char *dst, const char *magic, const char *src[], int nsrc)
{
    uint8_t *out = NULL;
    long total = 0;
    for (int i = 0; i < nsrc; i++) {
        long n;
        uint8_t *b = prep_read(src[i], &n);
        if (b && n >= 16 && !memcmp(b, magic, 8)) {
            uint8_t *t = realloc(out, (size_t)(total ? total : 16) + (size_t)(n - 16));
            if (t) {
                out = t;
                if (!total) memcpy(out, b, 16), total = 16;
                memcpy(out + total, b + 16, (size_t)(n - 16));
                total += n - 16;
            }
        }
        free(b);
    }
    int ok = total > 16 && plat_file_write_atomic(dst, out, (size_t)total);
    free(out);
    return ok ? total - 16 : 0;
}

static void gpu_ident(char *key, size_t kn, char *name, size_t nn)
{
    VkCtx *c = vk_ctx();
    key[0] = name[0] = 0;
    if (!c) return;
    const VkPhysicalDeviceProperties *p = &c->props;
    char uuid[2 * VK_UUID_SIZE + 1];
    for (int i = 0; i < VK_UUID_SIZE; i++) snprintf(uuid + 2 * i, 3, "%02x", p->pipelineCacheUUID[i]);
    snprintf(key, kn, "%04x:%04x:%08x:%s", p->vendorID, p->deviceID, p->driverVersion, uuid);
    snprintf(name, nn, "%s", p->deviceName);
    for (char *q = name; *q; q++) if (*q == '"' || *q == '\\') *q = ' ';
}

static long long rec_count(long bytes) { return bytes > 0 ? bytes / 216 : 0; }   /* gfx_shader.c ANS_REC */

int sw_gfx_prepare_shaders(void)
{
    PrepDirs d;
    char a_dst[PLAT_PATH_MAX * 2 + 40], p_dst[PLAT_PATH_MAX * 2 + 40], man[PLAT_PATH_MAX * 2 + 40];
    char ra[PLAT_PATH_MAX * 2 + 40], rp[PLAT_PATH_MAX * 2 + 40], ca[PLAT_PATH_MAX * 2 + 40], cp[PLAT_PATH_MAX * 2 + 40];
    if (!prep_dirs(&d)) { printf("[prep] error: no exe path\n"); return 1; }
    snprintf(ra, sizeof ra, "%s/shader-answers.bin", d.sdir);
    snprintf(rp, sizeof rp, "%s/shader-programs.bin", d.sdir);
    snprintf(ca, sizeof ca, "%s/census-answers.bin", d.sdir);
    snprintf(cp, sizeof cp, "%s/census-programs.bin", d.sdir);
    snprintf(a_dst, sizeof a_dst, "%s/seed-answers.bin", d.pdir);
    snprintf(p_dst, sizeof p_dst, "%s/seed-programs.bin", d.pdir);
    snprintf(man, sizeof man, "%s/prepared.json", d.sdir);

    long n;
    uint8_t *b = prep_read(ra, &n);
    long long recorded = b && n >= 16 && !memcmp(b, "TOTKSANS", 8) ? rec_count(n - 16) : 0;
    free(b);
    b = prep_read(ca, &n);
    long long census = b && n >= 16 && !memcmp(b, "TOTKSANS", 8) ? rec_count(n - 16) : 0;
    free(b);
    printf("[prep] start: %lld variants recorded in play, %lld from the game files (census) in %s\n", recorded, census,
           d.sdir);
    const char *asrc[2], *psrc[2];
    int k = 0;
    if (recorded > 0 && plat_file_exists(rp)) asrc[k] = ra, psrc[k++] = rp;   /* recorded first: priority 0 */
    if (census > 0 && plat_file_exists(cp)) asrc[k] = ca, psrc[k++] = cp;
    if (!k) {
        printf("[prep] nothing to prepare: no census and nothing recorded yet\n");
        return 2;
    }
    long long total = rec_count(prep_merge(a_dst, "TOTKSANS", asrc, k));
    if (!total || !prep_merge(p_dst, "TOTKSPRG", psrc, k)) {
        printf("[prep] error: cannot write the seed files in %s\n", d.pdir);
        DeleteFileA(a_dst), DeleteFileA(p_dst);
        return 1;
    }
    printf("[prep] translating: %lld variants (already translated ones are reused)\n", total);
    LARGE_INTEGER f0, t0, t1;
    QueryPerformanceFrequency(&f0);
    QueryPerformanceCounter(&t0);
    int rc = vk_pipeline_prepare_shaders();
    QueryPerformanceCounter(&t1);
    DeleteFileA(a_dst), DeleteFileA(p_dst);           /* our own temporary copies */
    double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)f0.QuadPart;
    if (rc) {
        printf("[prep] failed after %.1f s (rc %d); the previous preparation record is kept\n", secs, rc);
        return 1;
    }
    char gkey[160], gname[256], js[2048];
    gpu_ident(gkey, sizeof gkey, gname, sizeof gname);
    const char *xl = getenv("SW_PREP_XLATE_ID");
    SYSTEMTIME st;
    GetLocalTime(&st);
    int m = snprintf(js, sizeof js,
                     "{\"format\": 2, \"status\": \"complete\", \"variants\": %lld, \"recorded\": %lld, \"census\": %lld,\n"
                     " \"coverage\": \"shader stages: variants recorded in play + default-state variants of shaders found in "
                     ".ptcl/.arc; not every variant or pipeline\",\n"
                     " \"translator\": \"%s\", \"gpu\": \"%s\", \"gpu_name\": \"%s\", \"build\": \"%s\",\n"
                     " \"date\": \"%04d-%02d-%02d %02d:%02d\", \"seconds\": %.1f}\n",
                     total, recorded, census, xl ? xl : "", gkey, gname, SW_BUILD_ID, st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, secs);
    if (m <= 0 || m >= (int)sizeof js || !plat_file_write_atomic(man, js, (size_t)m)) {
        printf("[prep] error: cannot write %s\n", man);
        return 1;
    }
    printf("[prep] done: %lld variants (%lld recorded, %lld census) in %.1f s; %s\n", total, recorded, census, secs, man);
    return 0;
}

/* --warm-shaders: after a GPU driver change only the driver's cache is stale; the translated shaders stay
 * valid.  Rebuilds every prepared variant's stage library (shader-prepared.bin) without translating, then
 * records the driver it was done for in <shader cache>/warmed.json (prepared.json is left as it is). */
int sw_gfx_warm_shaders(void)
{
    PrepDirs d;
    if (!prep_dirs(&d)) { printf("[prep] error: no exe path\n"); return 1; }
    printf("[prep] warming: driver cache from %s/shader-prepared.bin\n", d.pdir);
    int rc = vk_pipeline_warm_shaders();
    if (rc) { printf("[prep] failed: nothing prepared yet (run Build Shaders first)\n"); return 1; }
    char gkey[160], gname[256], js[512], path[PLAT_PATH_MAX * 2 + 40];
    gpu_ident(gkey, sizeof gkey, gname, sizeof gname);
    int m = snprintf(js, sizeof js, "{\"format\": 1, \"gpu\": \"%s\", \"gpu_name\": \"%s\"}\n", gkey, gname);
    snprintf(path, sizeof path, "%s/warmed.json", d.sdir);
    if (m <= 0 || !plat_file_write_atomic(path, js, (size_t)m)) { printf("[prep] error: cannot write %s\n", path); return 1; }
    printf("[prep] done: driver cache warmed for %s\n", gname);
    return 0;
}

/* --gpu-info: the GPU/driver identity the caches depend on (the launcher compares it with prepared.json) */
int sw_gfx_gpu_info(void)
{
    if (!vk_init()) { printf("[gpu] error: no Vulkan device\n"); return 1; }
    char gkey[160], gname[256];
    gpu_ident(gkey, sizeof gkey, gname, sizeof gname);
    printf("[gpu] key %s\n[gpu] name %s\n", gkey, gname);
    return 0;
}
