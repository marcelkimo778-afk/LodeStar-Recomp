/* sw_runtime.c - standalone compatibility runtime.
 *
 * Loads the statically linked program image produced by the recompiler,
 * dispatches into native recompiled functions, and provides the kernel/service
 * surface those functions need.  No ARM64 interpreter or JIT exists in this
 * binary; that invariant is reported by the execution audit.
 */
#define SW_RT_IMPL   /* this file defines the real load/store functions: no inline macros */
#include "sw_rt.h"
#include "sw_program.h"
#include "sw_window.h"
#include "sw_mods.h"

#include <stdarg.h>
#include <setjmp.h>
#include <string.h>
#include <time.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <intrin.h>     /* __rdtsc (SW_PROFILE=all) */
#include <xmmintrin.h>   /* _mm_getcsr / _mm_setcsr: FPCR.FZ (sw_fpcr_apply) */
#endif
#if defined(_WIN32)
#include <windows.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

/* ------------------------------------------------------------------ */
/* Guest memory: two-level page table, 64 KiB pages                   */
/* ------------------------------------------------------------------ */
#define SW_PAGE_BITS 16
#define SW_PAGE_SIZE (1u << SW_PAGE_BITS)
#define SW_L1_BITS 12
#define SW_L1_SIZE (1u << SW_L1_BITS)
#define SW_L2_SIZE (1u << SW_L1_BITS)

/* ELF symbol binding values */
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

/* Relocation types */
#define R_AARCH64_RELATIVE    1027
#define R_AARCH64_GLOB_DAT    1025
#define R_AARCH64_JUMP_SLOT   1026
#define R_AARCH64_ABS64       257

#ifndef SW_BUILD_ID
#define SW_BUILD_ID "dev"
#endif

typedef struct SwPageTable {
    u8 **l1[SW_L1_SIZE];
} SwPageTable;

struct SwMem {
    SwPageTable *table;
    u64 mapped_pages;
    u64 mapped_bytes;
};

typedef struct SwMapping {
    u64 base;
    u64 size;
    u8 *host;
} SwMapping;

static SwMapping g_mappings[64];
static int g_mapping_count;

static jmp_buf g_trap_env;
static int g_trap_armed;

/* sw_thread.c: guest threads (fibers, or host threads with SW_MULTICORE=1) */
void sw_klock(void);
void sw_klock_note(u64 what);   /* what holds the kernel lock (SW_THREAD_STATS long-hold report) */
void sw_kunlock(void);
int sw_thread_core(void);
void sw_thread_set_core(u32 handle, int ideal);
void sw_threads_init(SwCpu *main_cpu);
int sw_thread_svc(SwCpu *c, u32 num);
int sw_thread_is_worker(SwCpu *c);
void sw_thread_halt_to_main(SwCpu *c);

/* Leave the guest after a halt.  Only the main fiber owns g_trap_env; a
 * worker thread hands its halt to the main thread instead (never returns). */
static void sw_trap_jump(SwCpu *c)
{
    if (sw_thread_is_worker(c)) sw_thread_halt_to_main(c);
    g_trap_armed = 0;
    longjmp(g_trap_env, 1);
}
static int g_exit_code;

/* per host thread (SW_TLS below): with SW_MULTICORE, shared per-call counters and
 * rings made every guest call of every core write the same cache lines */
#if defined(_MSC_VER)
#define SW_TLS __declspec(thread)
#else
#define SW_TLS _Thread_local
#endif
static u64 g_dispatch_count;           /* slow path only (the fast path is off with the watchdog / budget) */
static u64 g_missing_dispatch_count;
static u64 g_svc_count;
static u64 g_unimplemented_count;
static u64 g_trap_count;
static int g_verbose = 1;
static u64 g_step_budget = 0;
static u64 g_call_budget = 0;
/* Diagnostic limits.  Both are opt-in: a normal run must never depend on them.
 * --max-dispatch turns a non-terminating or runaway guest loop into a named
 * trap instead of an app that has to be killed, and --trace records every
 * inter-function edge so the call that led there survives a hard kill. */
static u64 g_max_dispatch = 0;
static FILE *g_trace_file = NULL;
static int g_reloc_audit = 0;

/* Relocation audit stats: per-module, per-type counts and zero-write tracking. */
typedef struct {
    const char *type_name;
    u64 total_count;
    u64 zero_count;
    u64 sample_count;
    struct { u64 offset; u32 sym_index; const char *name; u8 binding; u16 shndx; u64 S; u64 A; u64 old_val; u64 new_val; } samples[20];
} RelocTypeStats;

typedef struct {
    const char *module_name;
    RelocTypeStats abs64;
    RelocTypeStats glob_dat;
    RelocTypeStats jump_slot;
    RelocTypeStats relative;
} RelocModuleStats;

static RelocModuleStats *g_reloc_stats = NULL;
static u32 g_reloc_stat_count = 0;

static RelocModuleStats *find_reloc_stats(const char *module_name)
{
    for (u32 i = 0; i < g_reloc_stat_count; i++) {
        if (strcmp(g_reloc_stats[i].module_name, module_name) == 0)
            return &g_reloc_stats[i];
    }
    return NULL;
}

static RelocModuleStats *add_reloc_stats(const char *module_name)
{
    if (g_reloc_stat_count == 0) {
        g_reloc_stats = (RelocModuleStats *)malloc(8 * sizeof(RelocModuleStats));
    } else if ((g_reloc_stat_count & 7) == 0) {
        g_reloc_stats = (RelocModuleStats *)realloc(g_reloc_stats,
            (g_reloc_stat_count + 8) * sizeof(RelocModuleStats));
    }
    RelocModuleStats *ms = &g_reloc_stats[g_reloc_stat_count];
    ms->module_name = module_name;
    ms->abs64 = (RelocTypeStats){"ABS64", 0, 0, 0, {{0}}};
    ms->glob_dat = (RelocTypeStats){"GLOB_DAT", 0, 0, 0, {{0}}};
    ms->jump_slot = (RelocTypeStats){"JUMP_SLOT", 0, 0, 0, {{0}}};
    ms->relative = (RelocTypeStats){"RELATIVE", 0, 0, 0, {{0}}};
    g_reloc_stat_count++;
    return ms;
}

static void record_reloc(RelocModuleStats *ms, u32 reloc_type, u32 sym_index,
                         const char *sym_name, u8 binding, u16 shndx,
                         u64 S, u64 A, u64 old_val, u64 new_val)
{
    RelocTypeStats *ts = NULL;
    switch (reloc_type) {
    case R_AARCH64_ABS64: ts = &ms->abs64; break;
    case R_AARCH64_GLOB_DAT: ts = &ms->glob_dat; break;
    case R_AARCH64_JUMP_SLOT: ts = &ms->jump_slot; break;
    case R_AARCH64_RELATIVE: ts = &ms->relative; break;
    default: return;
    }
    ts->total_count++;
    if (new_val == 0) {
        ts->zero_count++;
        if (ts->sample_count < 20) {
            ts->samples[ts->sample_count].offset = 0; /* filled by caller */
            ts->samples[ts->sample_count].sym_index = sym_index;
            ts->samples[ts->sample_count].name = sym_name;
            ts->samples[ts->sample_count].binding = binding;
            ts->samples[ts->sample_count].shndx = shndx;
            ts->samples[ts->sample_count].S = S;
            ts->samples[ts->sample_count].A = A;
            ts->samples[ts->sample_count].old_val = old_val;
            ts->samples[ts->sample_count].new_val = new_val;
            ts->sample_count++;
        }
    }
}

static void print_reloc_audit(void)
{
    printf("\n=== PHASE 0 RELOCATION AUDIT ===\n");
    for (u32 m = 0; m < g_reloc_stat_count; m++) {
        RelocModuleStats *ms = &g_reloc_stats[m];
        printf("\n--- Module: %s ---\n", ms->module_name);
        RelocTypeStats *types[] = {&ms->abs64, &ms->glob_dat, &ms->jump_slot, &ms->relative};
        for (int t = 0; t < 4; t++) {
            RelocTypeStats *ts = types[t];
            printf("  %s: total=%llu zero=%llu\n",
                   ts->type_name,
                   (unsigned long long)ts->total_count,
                   (unsigned long long)ts->zero_count);
            for (u64 s = 0; s < ts->sample_count; s++) {
                printf("    [%llu] sym=%u name=%s binding=%s shndx=%u S=0x%llx A=0x%llx old=0x%llx new=0x%llx\n",
                       (unsigned long long)s,
                       ts->samples[s].sym_index,
                       ts->samples[s].name ? ts->samples[s].name : "(none)",
                       ts->samples[s].binding == STB_GLOBAL ? "GLOBAL" :
                       ts->samples[s].binding == STB_WEAK ? "WEAK" : "LOCAL",
                       ts->samples[s].shndx,
                       (unsigned long long)ts->samples[s].S,
                       (unsigned long long)ts->samples[s].A,
                       (unsigned long long)ts->samples[s].old_val,
                       (unsigned long long)ts->samples[s].new_val);
            }
        }
    }
    /* Deduplicated unresolved list */
    printf("\n--- Unresolved strong undefined symbols (deduplicated) ---\n");
    for (u32 m = 0; m < g_reloc_stat_count; m++) {
        RelocModuleStats *ms = &g_reloc_stats[m];
        RelocTypeStats *types[] = {&ms->abs64, &ms->glob_dat, &ms->jump_slot, &ms->relative};
        for (int t = 0; t < 4; t++) {
            RelocTypeStats *ts = types[t];
            for (u64 s = 0; s < ts->sample_count; s++) {
                if (ts->samples[s].new_val == 0 && ts->samples[s].shndx == 0 &&
                    ts->samples[s].name && ts->samples[s].binding == STB_GLOBAL) {
                    printf("  %s (from %s)\n", ts->samples[s].name, ms->module_name);
                }
            }
        }
    }
    printf("=== END PHASE 0 RELOCATION AUDIT ===\n");
}

/* Forward declaration for sw_report (used in profile code before definition) */
void sw_report(const char *fmt, ...);

/* Profiling (SW_PROFILE=1): per-dispatch timing and memory access counts. */
static int g_profile_enabled = 0;
static u64 g_load_store_count = 0;
static u64 g_prev_load_store_count = 0;
static u64 g_prev_dispatch_ts = 0;
static u64 g_prev_target = 0;

#define SW_PROFILE_MAX_GAPS 64
static struct SwProfileGap {
    u64 from_target;
    u64 to_target;
    u64 elapsed_ns;
    u64 load_store_delta;
    int valid;
} g_profile_gaps[SW_PROFILE_MAX_GAPS];
static int g_profile_gap_count = 0;

/* Ring buffer of last N dispatch edges for crash diagnostics */
#define SW_DISPATCH_RING_SIZE 64
static SW_TLS struct { u64 from_fn, to_fn; } g_dispatch_ring[SW_DISPATCH_RING_SIZE];  /* of the faulting thread */
static SW_TLS int g_dispatch_ring_pos;
static SW_FORCEINLINE void dispatch_ring_add(u64 from_fn, u64 to_fn)
{
    g_dispatch_ring[g_dispatch_ring_pos & (SW_DISPATCH_RING_SIZE - 1)].from_fn = from_fn;
    g_dispatch_ring[g_dispatch_ring_pos & (SW_DISPATCH_RING_SIZE - 1)].to_fn = to_fn;
    g_dispatch_ring_pos++;
}

/* Windows vectored exception handler - catches segfaults and prints diagnostics */
#if defined(_WIN32)
static LONG WINAPI sw_crash_handler(EXCEPTION_POINTERS *ep)
{
    /* informational / debugger notifications (thread names 0x406d1388,
     * OutputDebugString 0x40010006, C++ exceptions handled by the gfx back
     * end, ...) are not crashes: let them pass silently */
    if ((ep->ExceptionRecord->ExceptionCode & 0xF0000000u) != 0xC0000000u)
        return EXCEPTION_CONTINUE_SEARCH;
    {   /* Unicorn (SW_LOCKSTEP dev tool) takes and handles its own write faults */
        extern int sw_lockstep_busy;
        if (sw_lockstep_busy) return EXCEPTION_CONTINUE_SEARCH;
    }
    fprintf(stderr, "\n=== CRASH HANDLER INVOKED ===\n");
    fprintf(stderr, "Exception code: 0x%08lx\n", ep->ExceptionRecord->ExceptionCode);
    fprintf(stderr, "Exception address: 0x%p (exe rva 0x%llx: look up in sw_app.map)\n",
            (void *)ep->ExceptionRecord->ExceptionAddress,
            (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)GetModuleHandleA(NULL)));
    {   /* which module holds the address (a driver or system DLL rather than the exe) */
        HMODULE mod = NULL;
        char mname[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &mod) && mod)
            GetModuleFileNameA(mod, mname, sizeof mname);
        fprintf(stderr, "Exception module: %s + 0x%llx\n", mname,
                (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)mod));
    }
    fprintf(stderr, "Number of parameters: %lu\n", ep->ExceptionRecord->NumberParameters);
    for (DWORD i = 0; i < ep->ExceptionRecord->NumberParameters; i++) {
        fprintf(stderr, "  Parameter[%lu]: 0x%p\n", i, (void *)ep->ExceptionRecord->ExceptionInformation[i]);
    }

    {   /* the guest thread that faulted: registers and the frame-pointer chain (return addresses) */
        extern SwCpu *sw_thread_current_cpu(void);
        SwCpu *gc = sw_thread_current_cpu();
        if (gc) {
            fprintf(stderr, "Guest thread %lu cur_fn 0x%llx sp 0x%llx\n", GetCurrentThreadId(), (unsigned long long)gc->cur_fn,
                    (unsigned long long)gc->sp);
            for (int r = 0; r < 31; r += 4)
                fprintf(stderr, "  x%-2d 0x%016llx x%-2d 0x%016llx x%-2d 0x%016llx x%-2d 0x%016llx\n", r,
                        (unsigned long long)gc->x[r], r + 1, (unsigned long long)(r + 1 < 31 ? gc->x[r + 1] : 0), r + 2,
                        (unsigned long long)(r + 2 < 31 ? gc->x[r + 2] : 0), r + 3, (unsigned long long)(r + 3 < 31 ? gc->x[r + 3] : 0));
            u64 fp = gc->x[29];
            fprintf(stderr, "Guest call chain (lr, then frame records):\n  0x%llx\n", (unsigned long long)gc->x[30]);
            for (int d = 0; d < 24 && fp > 0x1000 && fp < (1ull << 40) && !(fp & 7); d++) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery((void *)(uintptr_t)fp, &mi, sizeof mi) || mi.State != MEM_COMMIT) break;
                u64 nfp = ((u64 *)(uintptr_t)fp)[0], lr = ((u64 *)(uintptr_t)fp)[1];
                fprintf(stderr, "  0x%llx\n", (unsigned long long)lr);
                if (nfp <= fp) break;
                fp = nfp;
            }
        }
    }
    fprintf(stderr, "Last %d dispatch edges:\n", SW_DISPATCH_RING_SIZE);
    unsigned pos = (unsigned)g_dispatch_ring_pos;
    unsigned cnt = pos < SW_DISPATCH_RING_SIZE ? pos : SW_DISPATCH_RING_SIZE;
    for (unsigned k = 0; k < cnt; k++) {
        unsigned idx = (pos - cnt + k) & (SW_DISPATCH_RING_SIZE - 1);
        fprintf(stderr, "  [%u] 0x%llx -> 0x%llx\n", k,
                (unsigned long long)g_dispatch_ring[idx].from_fn,
                (unsigned long long)g_dispatch_ring[idx].to_fn);
    }

    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        /* Scan the overflowed stack for return addresses inside the exe and
         * print them as RVAs (resolve with build_work/out/sw_app.map).  The
         * most frequent ones are the recursing functions. */
        static u64 rva[4096]; static u32 cnt[4096]; static int nr;
        HMODULE exe_mod = GetModuleHandleA(NULL);
        u64 lo = (u64)(uintptr_t)exe_mod;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(lo + ((IMAGE_DOS_HEADER *)exe_mod)->e_lfanew);
        u64 hi = lo + nt->OptionalHeader.SizeOfImage;
        ULONG_PTR slo = 0, shi = 0;
        GetCurrentThreadStackLimits(&slo, &shi);
        u64 *sp = (u64 *)(uintptr_t)ep->ContextRecord->Rsp;
        u64 *end = (u64 *)(uintptr_t)shi;
        nr = 0;
        int printed = 0;
        fprintf(stderr, "Host stack scan (rsp %p, top %p), first return addresses (RVA):\n", (void *)sp, (void *)end);
        for (u64 *p = sp; p < end; p++) {
            u64 v = *p;
            if (v <= lo + 0x1000 || v >= hi) continue;
            u64 r = v - lo;
            if (printed < 48) { fprintf(stderr, "  rva 0x%llx\n", (unsigned long long)r); printed++; }
            int k;
            for (k = 0; k < nr; k++) if (rva[k] == r) { cnt[k]++; break; }
            if (k == nr && nr < 4096) { rva[nr] = r; cnt[nr] = 1; nr++; }
        }
        fprintf(stderr, "Most frequent return addresses on the stack:\n");
        for (int t = 0; t < 24; t++) {
            int best = -1;
            for (int k = 0; k < nr; k++) if (cnt[k] && (best < 0 || cnt[k] > cnt[best])) best = k;
            if (best < 0) break;
            fprintf(stderr, "  rva 0x%llx x%u\n", (unsigned long long)rva[best], cnt[best]);
            cnt[best] = 0;
        }
    }
    fprintf(stderr, "=== END CRASH DIAGNOSTICS ===\n");
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static inline u64 sw_now_ns(void)
{
#if defined(_MSC_VER)
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (u64)((counter.QuadPart * 1000000000ull) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
}

static void profile_record_gap(u64 from_target, u64 to_target, u64 elapsed_ns, u64 load_store_delta)
{
    if (!g_profile_enabled) return;
    if (g_profile_gap_count < SW_PROFILE_MAX_GAPS) {
        g_profile_gaps[g_profile_gap_count].from_target = from_target;
        g_profile_gaps[g_profile_gap_count].to_target = to_target;
        g_profile_gaps[g_profile_gap_count].elapsed_ns = elapsed_ns;
        g_profile_gaps[g_profile_gap_count].load_store_delta = load_store_delta;
        g_profile_gaps[g_profile_gap_count].valid = 1;
        g_profile_gap_count++;
    } else {
        /* Find smallest gap and replace if new one is larger */
        int min_idx = -1;
        u64 min_ns = ULLONG_MAX;
        for (int i = 0; i < SW_PROFILE_MAX_GAPS; i++) {
            if (g_profile_gaps[i].valid && g_profile_gaps[i].elapsed_ns < min_ns) {
                min_ns = g_profile_gaps[i].elapsed_ns;
                min_idx = i;
            }
        }
        if (min_idx >= 0 && elapsed_ns > min_ns) {
            g_profile_gaps[min_idx].from_target = from_target;
            g_profile_gaps[min_idx].to_target = to_target;
            g_profile_gaps[min_idx].elapsed_ns = elapsed_ns;
            g_profile_gaps[min_idx].load_store_delta = load_store_delta;
        }
    }
}

static void profile_print_top_gaps(void)
{
    if (!g_profile_enabled || g_profile_gap_count == 0) return;
    /* Sort by elapsed_ns descending */
    for (int i = 0; i < g_profile_gap_count - 1; i++) {
        for (int j = i + 1; j < g_profile_gap_count; j++) {
            if (g_profile_gaps[j].elapsed_ns > g_profile_gaps[i].elapsed_ns) {
                struct SwProfileGap tmp = g_profile_gaps[i];
                g_profile_gaps[i] = g_profile_gaps[j];
                g_profile_gaps[j] = tmp;
            }
        }
    }
    int limit = g_profile_gap_count < 10 ? g_profile_gap_count : 10;
    sw_report("=== PROFILE: top %d longest dispatch gaps ===\n", limit);
    sw_report("%-4s  %-18s  %-18s  %-12s  %s\n", "#", "from", "to", "ms", "load/store");
    for (int i = 0; i < limit; i++) {
        double ms = g_profile_gaps[i].elapsed_ns / 1000000.0;
        sw_report("%-4d  0x%-16llx  0x%-16llx  %-12.3f  %llu\n",
                  i + 1,
                  (unsigned long long)g_profile_gaps[i].from_target,
                  (unsigned long long)g_profile_gaps[i].to_target,
                  ms,
                  (unsigned long long)g_profile_gaps[i].load_store_delta);
    }
}

/* Transfers to the SW_TRACE_FILE log.  The in-memory history printed when a trap fires is the dispatch ring
 * (dispatch_ring_add, written on the same calls): a second ring here cost a few TLS stores on every guest call. */
#define SW_TRACE 32
static void trace_edge(u64 from, u64 to)
{
    if (g_trace_file) {
        fprintf(g_trace_file, "0x%llx -> 0x%llx\n", (unsigned long long)from,
                (unsigned long long)to);
        fflush(g_trace_file);
    }
}

/* Returns non-zero when the dispatch budget has been reached.  Called from
 * sw_dispatch/sw_call so every inter-function transfer is accounted for. */
static int dispatch_budget_exhausted(SwCpu *c)
{
    if (g_max_dispatch && g_dispatch_count >= g_max_dispatch) {
        char reason[192];
        snprintf(reason, sizeof(reason),
                 "dispatch budget (%llu) exhausted in native function 0x%llx; "
                 "the guest is looping or waiting on an unimplemented service",
                 (unsigned long long)g_max_dispatch,
                 (unsigned long long)c->cur_fn);
        sw_trap(c, c->cur_fn, reason);
        return 1;
    }
    return 0;
}

static void dump_trace(void)
{
    unsigned i;
    printf("dispatch trace (oldest first):\n");
    unsigned pos = (unsigned)g_dispatch_ring_pos;
    unsigned cnt = pos < SW_TRACE ? pos : SW_TRACE;
    for (i = 0; i < cnt; i++) {
        unsigned idx = (pos - cnt + i) & (SW_DISPATCH_RING_SIZE - 1);
        if (!g_dispatch_ring[idx].to_fn) continue;
        printf("  0x%llx -> 0x%llx\n", (unsigned long long)g_dispatch_ring[idx].from_fn,
               (unsigned long long)g_dispatch_ring[idx].to_fn);
    }
}

static void sw_sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static void note_missing_seed(u64 target);

static int discover_skip(SwCpu *c);

/* Directory of the running executable.  Module image paths in the emitted
 * program table are relative to it, so the application can be launched from
 * any working directory.  SW_MODULE_DIR overrides it. */
static char g_base_dir[1024] = ".";

/* Debug print gating: set SW_DEBUG=1 to enable debug logs.
 * Default off so fprintf(stderr) doesn't kill performance. */
static int g_debug_enabled = 0;

/* Guest memory layout (also described near setup_thread_pointer). */
#define SW_TLS_BASE 0x71000000ull
#define SW_TLS_SIZE 0x10000ull
#define SW_THREAD_BASE 0x71100000ull
#define SW_THREAD_SIZE 0x10000ull
#define SW_ELF_TLS_BASE 0x72000000ull
#define SW_ELF_TLS_SIZE 0x800000ull

/* SW_TRACE_DISPATCH=1 prints every dispatch edge (very slow; off by default). */
static int g_trace_dispatch = 0;

/* nn::diag::detail::VAbortImpl(condition, function, file, line, result*, ...)
 * is where every SDK assertion / result check ends up.  Print its arguments
 * so an abort says what failed instead of just "svcBreak". */
static u64 g_vabort_addr;

static void guest_str(SwCpu *c, u64 addr, char *out, size_t cap)
{
    size_t i = 0;
    for (; addr && i + 1 < cap; i++) {
        u8 *p = sw_mem_ptr(c, addr + i, 1, 0);
        if (!p || !*p) break;
        out[i] = (char)*p;
    }
    out[i] = 0;
}

static void report_sdk_abort(SwCpu *c)
{
    char cond[256], func[256], file[256];
    u32 result = 0;
    u8 *rp = c->x[4] ? sw_mem_ptr(c, c->x[4], 4, 0) : NULL;
    if (rp) memcpy(&result, rp, 4);
    guest_str(c, c->x[0], cond, sizeof cond);
    guest_str(c, c->x[1], func, sizeof func);
    guest_str(c, c->x[2], file, sizeof file);
    fprintf(stderr, "\n=== SDK ABORT ===\n  condition: %s\n  function : %s\n  file     : %s:%llu\n"
            "  result   : 0x%x (module %u, description %u)\n  caller   : 0x%llx\n",
            cond, func, file, (unsigned long long)c->x[3], result, result & 0x1FF,
            (result >> 9) & 0x1FFF, (unsigned long long)c->cur_fn);
    printf("SDK ABORT: %s in %s (%s:%llu) result 0x%x\n", cond, func, file,
           (unsigned long long)c->x[3], result);
}

/* Frame tracking for vsync sleep gating.
 * Incremented on each entry to main loop (0x4987ff8).
 * NULL handler only sleeps once per frame. */
static unsigned g_frame_counter = 0;
static unsigned g_last_slept_frame = 0;

/* Inner timer loop tracking (0x4988324).
 * When in this loop, WaitTimer/WaitCallback should sleep to simulate timer wait. */
static int g_in_inner_timer_loop = 0;

void sw_report(const char *fmt, ...)
{
    /* Skip debug: messages unless SW_DEBUG=1 */
    if (fmt && (strncmp(fmt, "debug:", 6) == 0 || strncmp(fmt, "debug ", 6) == 0)) {
        if (!g_debug_enabled) return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Program tables (emitted by the recompiler)                         */
/* ------------------------------------------------------------------ */
extern const SwModuleDesc sw_modules[];
extern const u32 sw_module_count;
extern const SwFuncEntry sw_functions[];
extern const u32 sw_function_count;
extern const SwNativeEntry sw_native_entries[];
extern const u32 sw_native_count;

/* ------------------------------------------------------------------ */
/* Memory API                                                         */
/* ------------------------------------------------------------------ */
int sw_ipc_svc(SwCpu *c, u32 num);
int sw_ipc_event_never(u32 h);
void sw_ipc_shmem_map(SwCpu *c, u32 handle, u64 addr, u64 size);
/* ranges mapped by svcMapMemory (thread alias stacks), reported by QueryMemory */
static struct { u64 lo, hi; } g_alias[512];
static int g_alias_n;
static SwMem g_mem;

/* Guest addresses are host addresses (identity mapping).  At start-up every
 * free block of host address space below 2^39 (the guest's 39-bit space) is
 * reserved, so later host allocations (heaps, thread stacks, the graphics
 * driver) land above it; a guest page is committed at its own address when
 * it is mapped (zero-filled by the OS).  The page table only records which
 * 64 KB pages exist.  The graphics layer can then use guest pointers (memory
 * pools, buffers, NVN objects) as host pointers, like the Switch GPU shares
 * memory with the CPU.  g_guest_base stays 0: host = g_guest_base + guest.
 * The reservation has MEM_WRITE_WATCH so the graphics layer can ask which
 * pages the CPU wrote (textures and buffers the game fills). */
#define SW_GUEST_SPAN (1ull << 39)          /* 39-bit guest address space */
#if defined(_WIN32)
u8 sw_pmap[SW_GUEST_SPAN >> SW_PAGE_BITS];
#define g_pmap sw_pmap
int sw_mem_fast_on = 1;   /* cleared by SW_MEM_TRACE=1 / --watch */
#endif
u8 *g_guest_base;
static u64 g_guest_reserved;                /* bytes of host VA reserved for the guest */

#if defined(_WIN32)
/* SW_RESERVE_CHUNK_MB (see mem_reserve_guest_space); 0 = one reservation per free region.  sw_gfx_glue.c splits
 * GetWriteWatch queries at these block boundaries. */
uint64_t sw_reserve_chunk(void)
{
    static u64 chunk = (u64)-1;
    /* default 256 MB (2026-11-11: pool imports 6.6 s -> 0.7 s per load, walk fps unchanged; 32/64 MB made the walk
     * slower), SW_RESERVE_CHUNK_MB=0: one reservation per free region as before */
    if (chunk == (u64)-1) { const char *e = getenv("SW_RESERVE_CHUNK_MB"); chunk = e ? (atoi(e) > 0 ? (u64)atoi(e) << 20 : 0) : 256ull << 20; }
    return chunk;
}
static void mem_reserve_guest_space(void)
{
    u64 a = 0x10000;
    int blocks = 0;
    while (a < SW_GUEST_SPAN) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        u64 base = (u64)(uintptr_t)mi.BaseAddress, end = base + mi.RegionSize;
        if (end > SW_GUEST_SPAN) end = SW_GUEST_SPAN;
        if (mi.State == MEM_FREE) {
            u64 lo = (base + 0xFFFF) & ~0xFFFFull, hi = end & ~0xFFFFull;
            /* SW_RESERVE_CHUNK_MB=<n>: reserve in aligned blocks of n MB instead of one block per free region.
             * Importing a guest memory pool into Vulkan took the NVIDIA driver ~19 ms per pool whatever its size
             * (1,564 pools = 6.6 s while a save loads): it walks the reservation the pointer is in. */
            u64 chunk = sw_reserve_chunk();
            for (u64 s = lo; s < hi;) {
                u64 e2 = chunk ? ((s / chunk) + 1) * chunk : hi;
                if (e2 > hi) e2 = hi;
                if (VirtualAlloc((void *)(uintptr_t)s, (SIZE_T)(e2 - s), MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS)) {
                    g_guest_reserved += e2 - s;
                    blocks++;
                }
                s = e2;
            }
        }
        if (end <= a) break;
        a = end;
    }
    sw_report("memory: guest addresses are host addresses; reserved %llu GB below 2^39 in %d block(s); "
              "exe at %p\n", (unsigned long long)(g_guest_reserved >> 30), blocks, (void *)GetModuleHandleW(NULL));
}
#endif

static SwMem *mem_init(void)
{
    memset(&g_mem, 0, sizeof(g_mem));
    g_guest_base = NULL;
#if defined(_WIN32)
    mem_reserve_guest_space();
#endif
    g_mem.table = (SwPageTable *)calloc(1, sizeof(SwPageTable));
    if (!g_mem.table) {
        sw_report("fatal: out of memory allocating page table\n");
        exit(70);
    }
    return &g_mem;
}

static u8 **mem_l2(SwMem *m, u64 page, int create)
{
    u64 l1 = (page >> SW_L1_BITS) & (SW_L1_SIZE - 1);
    if (!m->table->l1[l1]) {
        if (!create) return NULL;
        m->table->l1[l1] = (u8 **)calloc(SW_L2_SIZE, sizeof(u8 *));
        if (!m->table->l1[l1]) {
            sw_report("fatal: out of memory allocating page table level 2\n");
            exit(70);
        }
    }
    return m->table->l1[l1];
}

int sw_mem_map(SwCpu *c, u64 addr, u64 size, const void *data, u64 data_size)
{
    SwMem *m = c->mem;
    u64 start = addr >> SW_PAGE_BITS;
    u64 pages = (size + SW_PAGE_SIZE - 1) >> SW_PAGE_BITS;
    u64 i;
    u64 off = 0;
    if (size == 0) return 0;
    if (g_mapping_count < (int)(sizeof(g_mappings) / sizeof(g_mappings[0]))) {
        g_mappings[g_mapping_count].base = addr;
        g_mappings[g_mapping_count].size = size;
        g_mappings[g_mapping_count].host = NULL;
        g_mapping_count++;
    }
#if defined(_WIN32)
    /* Bulk commit (no data to copy): one VirtualAlloc for the whole range instead of one per 64 KB page.
     * MapPhysicalMemory (svc 0x2C) maps large heap ranges under the kernel lock; page by page it held the lock
     * for ~1.8 s while the save loads into the world (the 2 s freeze).  Same checks as the loop below; if the
     * range cannot be committed in one piece, the loop does it page by page as before. */
    if (!data && pages > 1 && ((start + pages) << SW_PAGE_BITS) <= SW_GUEST_SPAN) {
        int clean = 1;
        for (i = 0; i < pages && clean; i++) {
            u8 **l2 = mem_l2(m, start + i, 1);
            if (l2[(start + i) & (SW_L2_SIZE - 1)]) clean = 0;
        }
        void *want = (void *)(uintptr_t)(start << SW_PAGE_BITS);
        if (clean && VirtualAlloc(want, (SIZE_T)(pages << SW_PAGE_BITS), MEM_COMMIT, PAGE_READWRITE) == want) {
            for (i = 0; i < pages; i++) {
                u64 page = start + i;
                mem_l2(m, page, 1)[page & (SW_L2_SIZE - 1)] = (u8 *)(uintptr_t)(page << SW_PAGE_BITS);
                g_pmap[page] = 1;
            }
            m->mapped_pages += pages;
            m->mapped_bytes += pages << SW_PAGE_BITS;
            return 0;
        }
    }
#endif
    for (i = 0; i < pages; i++) {
        u64 page = start + i;
        u8 **l2 = mem_l2(m, page, 1);
        u64 slot = page & (SW_L2_SIZE - 1);
        u8 *host;
        if (l2[slot]) {
            sw_report("error: guest page 0x%llx mapped twice\n", (unsigned long long)(page << SW_PAGE_BITS));
            return -1;
        }
#if defined(_WIN32)
        if (((page << SW_PAGE_BITS) + SW_PAGE_SIZE) > SW_GUEST_SPAN) {
            sw_report("error: guest page 0x%llx outside the guest address space\n",
                      (unsigned long long)(page << SW_PAGE_BITS));
            return -1;
        }
        {
            void *want = (void *)(uintptr_t)(page << SW_PAGE_BITS);
            host = (u8 *)VirtualAlloc(want, SW_PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);
            DWORD err1 = host ? 0 : GetLastError();
            if (!host)              /* free but not reserved by us (freed since start-up) */
                host = (u8 *)VirtualAlloc(want, SW_PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!host) {
                MEMORY_BASIC_INFORMATION mi;
                MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
                /* 1455 = ERROR_COMMITMENT_LIMIT: Windows' commit limit (RAM + page file) is used up, not a clash */
                sw_report("fatal: committing guest page 0x%llx failed: error %lu; commit free %llu MB of %llu MB\n",
                          (unsigned long long)(page << SW_PAGE_BITS), (unsigned long)err1,
                          (unsigned long long)(ms.ullAvailPageFile >> 20), (unsigned long long)(ms.ullTotalPageFile >> 20));
                memset(&mi, 0, sizeof mi);
                VirtualQuery(want, &mi, sizeof mi);
                sw_report("fatal: guest page 0x%llx is occupied by host memory (state 0x%lx, type 0x%lx, "
                          "allocation base %p)\n", (unsigned long long)(page << SW_PAGE_BITS),
                          (unsigned long)mi.State, (unsigned long)mi.Type, mi.AllocationBase);
                exit(70);
            }
        }
#else
        host = (u8 *)calloc(1, SW_PAGE_SIZE);
#endif
        if (!host) {
            sw_report("fatal: out of memory mapping guest page\n");
            exit(70);
        }
        if (data && off < data_size) {
            u64 n = data_size - off;
            if (n > SW_PAGE_SIZE) n = SW_PAGE_SIZE;
            /* Debug: check if this page contains the critical address */
            u64 page_addr = page << SW_PAGE_BITS;
            if (page_addr <= 0x43ac408 && page_addr + SW_PAGE_SIZE > 0x43ac408) {
                u64 src_off = off + (0x43ac408 - page_addr);
                u64 val = *(u64 *)((const u8 *)data + src_off);
                sw_report("debug sw_mem_map: page 0x%llx off=0x%llx src_off=0x%llx val=0x%llx\n",
                          (unsigned long long)page_addr, (unsigned long long)off,
                          (unsigned long long)src_off, (unsigned long long)val);
            }
            memcpy(host, (const u8 *)data + off, n);
            /* Verify after copy */
            if (page_addr <= 0x43ac408 && page_addr + SW_PAGE_SIZE > 0x43ac408) {
                u64 dst_off = 0x43ac408 - page_addr;
                u64 val = *(u64 *)(host + dst_off);
                sw_report("debug sw_mem_map: after copy, guest 0x43ac408 = 0x%llx\n", (unsigned long long)val);
            }
        }
        l2[slot] = host;
#if defined(_WIN32)
        if ((u64)(uintptr_t)host == (page << SW_PAGE_BITS)) g_pmap[page] = 1;
#endif
        m->mapped_pages++;
        m->mapped_bytes += SW_PAGE_SIZE;
        off += SW_PAGE_SIZE;
    }
    return 0;
}

static u8 *mem_host(SwCpu *c, u64 addr, u64 size, int write);

/* Fast path for guest memory (Windows: guest addresses are host addresses).
 * g_pmap[page] = 1 once a 64 KB guest page is committed in place; pages are
 * never unmapped.  SW_MEM_TRACE=1 keeps the old slow path for every access
 * (instruction trace ring for crash reports). */
#if defined(_WIN32)
static int g_mem_slow = -1;
static inline int mem_fast_ok(u64 addr, u64 size)
{
    if (addr >= SW_GUEST_SPAN - 16 || !g_pmap[addr >> SW_PAGE_BITS]) return 0;
    if ((addr & (SW_PAGE_SIZE - 1)) + size <= SW_PAGE_SIZE) return 1;
    return g_pmap[(addr + size - 1) >> SW_PAGE_BITS];
}
static inline int mem_fast(void)
{
    if (g_mem_slow < 0) { const char *e = getenv("SW_MEM_TRACE"); g_mem_slow = e && *e == '1'; if (g_mem_slow) sw_mem_fast_on = 0; }
    return !g_mem_slow;
}
#define MEM_FAST_OK(a, n) (mem_fast_ok((a), (n)))
#else
#define MEM_FAST_OK(a, n) 0
static inline int mem_fast(void) { return 0; }
#endif

u8 *sw_mem_ptr(SwCpu *c, u64 addr, u64 size, int write)
{
    return mem_host(c, addr, size, write);
}

static u8 *mem_host(SwCpu *c, u64 addr, u64 size, int write)
{
    if (MEM_FAST_OK(addr, size)) return (u8 *)(uintptr_t)addr;
    SwMem *m = c->mem;
    u64 page = addr >> SW_PAGE_BITS;
    u8 **l2 = mem_l2(m, page, 0);
    u64 off, slot;
    if (!l2) return NULL;
    off = addr & (SW_PAGE_SIZE - 1);
    slot = page & (SW_L2_SIZE - 1);
    if (!l2[slot]) return NULL;
    (void)write;
    if (off + size > SW_PAGE_SIZE) {
#if defined(_WIN32)
        /* contiguous host memory: valid if every touched page is mapped */
        u64 last = (addr + size - 1) >> SW_PAGE_BITS;
        for (u64 pg = page + 1; pg <= last; pg++) {
            u8 **l2b = mem_l2(m, pg, 0);
            if (!l2b || !l2b[pg & (SW_L2_SIZE - 1)]) return NULL;
        }
#else
        return NULL;
#endif
    }
    return l2[slot] + off;
}

/* Accesses that straddle two guest pages (64 KB) have no single host pointer;
 * copy byte by byte.  Returns 0 if any byte is unmapped.  Unaligned loads and
 * stores across a page boundary are legal on AArch64 (memchr's ldp q did it). */
static int mem_copy_slow(SwCpu *c, u64 addr, void *buf, unsigned size, int write)
{
    unsigned i;
    for (i = 0; i < size; i++)
        if (!mem_host(c, addr + i, 1, write)) return 0;
    for (i = 0; i < size; i++) {
        u8 *b = mem_host(c, addr + i, 1, write);
        if (write) *b = ((u8 *)buf)[i]; else ((u8 *)buf)[i] = *b;
    }
    return 1;
}

static void *sw_fault(SwCpu *c, u64 addr, unsigned size, int write, const char *what)
{
    /* Print detailed fault information */
    const char *module_name = "<unknown>";
    const char *segment_name = "?";
    for (u32 mi = 0; mi < sw_module_count; mi++) {
        const SwModuleDesc *m = &sw_modules[mi];
        u64 text_start = m->base;
        u64 ro_start = m->base + m->text_size;
        u64 data_start = m->base + m->text_size + m->ro_size;
        u64 bss_start = m->base + m->text_size + m->ro_size + m->data_size;
        u64 text_end = ro_start;
        u64 ro_end = data_start;
        u64 data_end = bss_start;
        u64 bss_end = bss_start + m->bss_size;

        if (addr >= text_start && addr < text_end) {
            module_name = m->name;
            segment_name = ".text";
        } else if (addr >= ro_start && addr < ro_end) {
            module_name = m->name;
            segment_name = ".rodata";
        } else if (addr >= data_start && addr < data_end) {
            module_name = m->name;
            segment_name = ".data";
        } else if (addr >= bss_start && addr < bss_end) {
            module_name = m->name;
            segment_name = ".bss";
        }
    }

    /* Dump 128 bytes around faulting address */
    sw_report("fault addr 0x%llx: module=%s segment=%s\n",
              (unsigned long long)addr, module_name, segment_name);
    if (addr >= 0x80) {
        sw_report("memory at 0x%llx-0x%llx:\n",
                  (unsigned long long)(addr - 0x80), (unsigned long long)(addr + 0x80));
        for (int offset = -0x80; offset < 0x80; offset += 16) {
            u64 dump_addr = addr + offset;
            u8 bytes[16];
            int valid = 0;
            for (int b = 0; b < 16; b++) {
                u8 *p = sw_mem_ptr(c, dump_addr + b, 1, 0);
                bytes[b] = p ? *p : 0xFF;
                if (p) valid = 1;
            }
            if (!valid) continue;
            char hex[49];
            char ascii[17];
            hex[0] = 0;
            ascii[0] = 0;
            for (int b = 0; b < 16; b++) {
                if (b == 8) strcat(hex, " ");
                char byte_hex[4];
                snprintf(byte_hex, sizeof(byte_hex), "%02X ", bytes[b]);
                strcat(hex, byte_hex);
                ascii[b] = (bytes[b] >= 0x20 && bytes[b] <= 0x7E) ? bytes[b] : '.';
            }
            ascii[16] = 0;
            sw_report("  0x%llx: %s  %s\n", (unsigned long long)dump_addr, hex, ascii);
        }
    }

    char reason[256];
    snprintf(reason, sizeof(reason), "%s at 0x%llx (%u bytes, %s)", what,
             (unsigned long long)addr, size, write ? "write" : "read");
    sw_trap(c, addr, reason);
    return NULL;
}

/* Address watch (--watch ADDR[:LEN]): every guest store into the range is
 * reported together with the writing function and instruction.  This is how a
 * half-built object is traced back to the constructor that should have
 * initialised it; it costs one test when unused. */
static u64 g_watch_addr;
static u64 g_watch_len;
/* Arm the store watch at run time (graphics debugging: SW_SKIP_T0=3). */
void sw_watch_set(u64 addr, u64 len)
{
    g_watch_addr = addr; g_watch_len = len;
    sw_mem_fast_on = 0;
    sw_report("[watch] armed at 0x%llx len 0x%llx\n", (unsigned long long)addr, (unsigned long long)len);
}
static u64 g_watch_hits;
static u64 g_zero_page_accesses;
static int g_run_init_entry;

static void watch_store(SwCpu *c, u64 addr, unsigned size, u64 value)
{
    if (!g_watch_len) return;
    if (addr + size <= g_watch_addr) return;
    if (addr >= g_watch_addr + g_watch_len) return;
    g_watch_hits++;
    {   /* SW_WATCH_FOLLOW=<memcpy addr>: a hit from that copy routine moves the watch to its source (x1 at
         * entry + the same offset), once - follows a value back to the code that computes it */
        static int f = -1; static u64 copy_fn;
        if (f < 0) { const char *e = getenv("SW_WATCH_FOLLOW"); f = e != NULL; copy_fn = e ? strtoull(e, NULL, 0) : 0; }
        if (f == 1 && c->cur_fn == copy_fn && addr >= c->x[0] && addr - c->x[0] < 0x10000) {
            u64 src = c->x[1] + (g_watch_addr - c->x[0]);
            fprintf(stderr, "[watch] following copy (caller lr 0x%llx): source 0x%llx\n",
                    (unsigned long long)c->x[30], (unsigned long long)src);
            g_watch_addr = src;
            f = 2;
            return;
        }
    }
    fprintf(stderr, "[watch] store 0x%llx (%u bytes) = 0x%llx from insn 0x%llx "
            "in function 0x%llx (entry x0 0x%llx x1 0x%llx x2 0x%llx lr 0x%llx)\n", (unsigned long long)addr, size,
            (unsigned long long)value, (unsigned long long)c->pc,
            (unsigned long long)c->cur_fn, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
            (unsigned long long)c->x[2], (unsigned long long)c->x[30]);
}

#define SW_LOAD(TYPE, NAME, SIZE, SIGNED) \
    TYPE NAME(SwCpu *c, u64 addr) { \
        if (MEM_FAST_OK(addr, SIZE) && mem_fast()) { TYPE fv; memcpy(&fv, (const void *)(uintptr_t)addr, SIZE); return fv; } \
        static int dbg_loads = -1; \
        if (dbg_loads < 0) dbg_loads = getenv("SW_DEBUG_LOADS") != NULL; \
        if (dbg_loads) { \
            sw_report("debug: %s called with addr=0x%llx\n", #NAME, (unsigned long long)addr); \
        } \
        /* Kernel memory region (canonical addresses with top 16 bits set): return 0 */ \
        if ((addr & 0xffff000000000000ull) == 0xffff000000000000ull) { \
            return 0; \
        } \
        /* Record instruction trace before the memory access */ \
        { \
            int pos = c->insn_trace_pos & (SW_INSN_TRACE_SIZE - 1); \
            c->insn_trace[pos].pc = c->pc; \
            c->insn_trace[pos].x0 = c->x[0]; \
            c->insn_trace[pos].x1 = c->x[1]; \
            c->insn_trace[pos].x2 = c->x[2]; \
            c->insn_trace[pos].x3 = c->x[3]; \
            c->insn_trace[pos].lr = c->prev_fn; \
            c->insn_trace[pos].cur_fn = c->cur_fn; \
            c->insn_trace_pos++; \
        } \
        /* Log first 100 accesses to the zero page (0x0-0xFFF) to verify \
         * the removal of the 0x1d0 fill did not hide NULL dereferences. */ \
        if (addr < 0x1000ull && g_zero_page_accesses < 100) { \
            sw_report("zero-page: %s addr=0x%llx size=%u pc=0x%llx fn=0x%llx\n", \
                      #NAME, (unsigned long long)addr, (unsigned)SIZE, \
                      (unsigned long long)c->pc, (unsigned long long)c->cur_fn); \
            g_zero_page_accesses++; \
        } \
        u8 *p = mem_host(c, addr, SIZE, 0); \
        TYPE v; \
        if (!p) { \
            if (mem_copy_slow(c, addr, &v, SIZE, 0)) return v; \
            sw_fault(c, addr, SIZE, 0, "unmapped load"); return 0; } \
        memcpy(&v, p, SIZE); \
        if (g_profile_enabled) g_load_store_count++; \
        return v; }

SW_LOAD(u8, sw_load8, 1, 0)
SW_LOAD(u16, sw_load16, 2, 0)
SW_LOAD(u32, sw_load32, 4, 0)
SW_LOAD(u64, sw_load64, 8, 0)
SW_LOAD(s8, sw_load8s, 1, 1)
SW_LOAD(s16, sw_load16s, 2, 1)
SW_LOAD(s32, sw_load32s, 4, 1)

#define SW_STORE(TYPE, NAME, SIZE) \
    void NAME(SwCpu *c, u64 addr, u64 raw) { \
        TYPE v = (TYPE)raw; \
        if (!g_watch_len && MEM_FAST_OK(addr, SIZE) && mem_fast()) { memcpy((void *)(uintptr_t)addr, &v, SIZE); return; } \
        watch_store(c, addr, SIZE, raw); \
        /* Record instruction trace before the memory access */ \
        { \
            int pos = c->insn_trace_pos & (SW_INSN_TRACE_SIZE - 1); \
            c->insn_trace[pos].pc = c->pc; \
            c->insn_trace[pos].x0 = c->x[0]; \
            c->insn_trace[pos].x1 = c->x[1]; \
            c->insn_trace[pos].x2 = c->x[2]; \
            c->insn_trace[pos].x3 = c->x[3]; \
            c->insn_trace[pos].lr = c->prev_fn; \
            c->insn_trace[pos].cur_fn = c->cur_fn; \
            c->insn_trace_pos++; \
        } \
        /* Log first 100 accesses to the zero page (0x0-0xFFF) to verify \
         * the removal of the 0x1d0 fill did not hide NULL dereferences. */ \
        if (addr < 0x1000ull && g_zero_page_accesses < 100) { \
            sw_report("zero-page: %s addr=0x%llx size=%u pc=0x%llx fn=0x%llx\n", \
                      #NAME, (unsigned long long)addr, (unsigned)SIZE, \
                      (unsigned long long)c->pc, (unsigned long long)c->cur_fn); \
            g_zero_page_accesses++; \
        } \
        u8 *p = mem_host(c, addr, SIZE, 1); \
        if (!p) { \
            if (mem_copy_slow(c, addr, &v, SIZE, 1)) return; \
            sw_fault(c, addr, SIZE, 1, "unmapped store"); return; } \
        memcpy(p, &v, SIZE); \
        if (g_profile_enabled) g_load_store_count++; }

SW_STORE(u8, sw_store8, 1)
SW_STORE(u16, sw_store16, 2)
SW_STORE(u32, sw_store32, 4)
SW_STORE(u64, sw_store64, 8)

sw_v128 sw_loadv(SwCpu *c, u64 addr, unsigned bytes)
{
    sw_v128 v;
    u8 *p;
    memset(&v, 0, sizeof(v));
    if (bytes > 16) bytes = 16;
    if (MEM_FAST_OK(addr, bytes) && mem_fast()) { memcpy(&v, (const void *)(uintptr_t)addr, bytes); return v; }
    /* Record instruction trace */
    {
        int pos = c->insn_trace_pos & (SW_INSN_TRACE_SIZE - 1);
        c->insn_trace[pos].pc = c->pc;
        c->insn_trace[pos].x0 = c->x[0];
        c->insn_trace[pos].x1 = c->x[1];
        c->insn_trace[pos].x2 = c->x[2];
        c->insn_trace[pos].x3 = c->x[3];
        c->insn_trace[pos].lr = c->prev_fn;
        c->insn_trace[pos].cur_fn = c->cur_fn;
        c->insn_trace_pos++;
    }
    p = mem_host(c, addr, bytes, 0);
    if (!p) {
        if (mem_copy_slow(c, addr, &v, bytes, 0)) return v;
        sw_fault(c, addr, bytes, 0, "unmapped vector load"); return v; }
    memcpy(&v, p, bytes);
    if (g_profile_enabled) g_load_store_count++;
    return v;
}

void sw_storev(SwCpu *c, u64 addr, sw_v128 v, unsigned bytes)
{
    u8 *p;
    if (bytes > 16) bytes = 16;
    if (!g_watch_len && MEM_FAST_OK(addr, bytes) && mem_fast()) { memcpy((void *)(uintptr_t)addr, &v, bytes); return; }
    watch_store(c, addr, bytes, v.d[0]);   /* low 8 bytes of the stored vector */
    /* Record instruction trace */
    {
        int pos = c->insn_trace_pos & (SW_INSN_TRACE_SIZE - 1);
        c->insn_trace[pos].pc = c->pc;
        c->insn_trace[pos].x0 = c->x[0];
        c->insn_trace[pos].x1 = c->x[1];
        c->insn_trace[pos].x2 = c->x[2];
        c->insn_trace[pos].x3 = c->x[3];
        c->insn_trace[pos].lr = c->prev_fn;
        c->insn_trace[pos].cur_fn = c->cur_fn;
        c->insn_trace_pos++;
    }
    p = mem_host(c, addr, bytes, 1);
    if (!p) {
        if (mem_copy_slow(c, addr, &v, bytes, 1)) return;
        sw_fault(c, addr, bytes, 1, "unmapped vector store"); return; }
    memcpy(p, &v, bytes);
    if (g_profile_enabled) g_load_store_count++;
}

/* ------------------------------------------------------------------ */
/* Traps and diagnostics                                              */
/* ------------------------------------------------------------------ */
static char g_halt_reason[512];

void sw_trap(SwCpu *c, u64 at, const char *why)
{
    g_trap_count++;
    c->halted = 1;
    c->halt_pc = at;
    /* Callers often pass a stack buffer; keep the message alive for the audit. */
    snprintf(g_halt_reason, sizeof(g_halt_reason), "%s", why ? why : "(no reason)");
    c->halt_reason = g_halt_reason;
    sw_report("[trap] %s (in native function 0x%llx at insn 0x%llx)\n", g_halt_reason,
              (unsigned long long)c->cur_fn, (unsigned long long)c->pc);

    /* Dump instruction trace ring buffer */
    if (c->insn_trace_pos > 0) {
        int count = c->insn_trace_pos < SW_INSN_TRACE_SIZE ? c->insn_trace_pos : SW_INSN_TRACE_SIZE;
        int start = c->insn_trace_pos - count;
        sw_report("=== instruction trace (last %d entries, oldest first) ===\n", count);
        for (int i = 0; i < count; i++) {
            int idx = (start + i) & (SW_INSN_TRACE_SIZE - 1);
            struct SwInsnTraceEntry *e = &c->insn_trace[idx];
            sw_report("  [%3d] pc=0x%llx cur_fn=0x%llx lr=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n",
                      i, (unsigned long long)e->pc, (unsigned long long)e->cur_fn,
                      (unsigned long long)e->lr, (unsigned long long)e->x0,
                      (unsigned long long)e->x1, (unsigned long long)e->x2,
                      (unsigned long long)e->x3);
        }
        sw_report("=== end instruction trace ===\n");
    }

    /* Dump memory around X0 if it looks like a valid pointer */
    if (c->x[0] >= 0x100000 && c->x[0] < 0x70200000) {
        sw_report("X0=0x%llx appears to be a guest pointer. Dumping 128 bytes around it:\n",
                  (unsigned long long)c->x[0]);
        u64 base = c->x[0] & ~0x3FULL;
        for (int offset = -0x40; offset < 0x80; offset += 16) {
            u64 dump_addr = base + offset;
            u8 bytes[16];
            int valid = 0;
            for (int b = 0; b < 16; b++) {
                u8 *p = sw_mem_ptr(c, dump_addr + b, 1, 0);
                bytes[b] = p ? *p : 0xFF;
                if (p) valid = 1;
            }
            if (!valid) continue;
            char hex[49];
            char ascii[17];
            hex[0] = 0;
            ascii[0] = 0;
            for (int b = 0; b < 16; b++) {
                if (b == 8) strcat(hex, " ");
                char byte_hex[4];
                snprintf(byte_hex, sizeof(byte_hex), "%02X ", bytes[b]);
                strcat(hex, byte_hex);
                ascii[b] = (bytes[b] >= 0x20 && bytes[b] <= 0x7E) ? bytes[b] : '.';
            }
            ascii[16] = 0;
            sw_report("  0x%llx: %s  %s\n", (unsigned long long)dump_addr, hex, ascii);
        }
    }

    if (g_trap_armed || sw_thread_is_worker(c)) {
        sw_trap_jump(c);
    }
    exit(70);
}

/* DMB / DSB.  With SW_MULTICORE guest threads run on several host cores, and x86
 * may let a later load pass an earlier store (the only reordering TSO allows):
 * a real fence then, as DMB ISH orders it on the Switch.  One host core (fibers):
 * a compiler barrier is enough.  Set by sw_threads_init. */
int sw_mc_fence;
void sw_memory_barrier(void)
{
#if defined(_MSC_VER)
    if (sw_mc_fence) MemoryBarrier();
    else _ReadWriteBarrier();
#else
    __sync_synchronize();
#endif
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */
#define FN_HASH_BITS 21
static u32 *g_fn_hash;            /* index + 1 into sw_functions, 0 = empty */
static const SwFuncEntry *find_function_bs(u64 addr);
static const SwFuncEntry *find_function(u64 addr)
{
    if (!g_fn_hash) {
        g_fn_hash = (u32 *)calloc((size_t)1 << FN_HASH_BITS, sizeof(u32));
        if (!g_fn_hash) return find_function_bs(addr);
        for (u32 i = 0; i < sw_function_count; i++) {
            u32 h = (u32)((sw_functions[i].addr * 0x9E3779B97F4A7C15ull) >> (64 - FN_HASH_BITS));
            while (g_fn_hash[h]) h = (h + 1) & ((1u << FN_HASH_BITS) - 1);
            g_fn_hash[h] = i + 1;
        }
    }
    u32 h = (u32)((addr * 0x9E3779B97F4A7C15ull) >> (64 - FN_HASH_BITS));
    for (;;) {
        u32 v = g_fn_hash[h];
        if (!v) return NULL;
        if (sw_functions[v - 1].addr == addr) return &sw_functions[v - 1];
        h = (h + 1) & ((1u << FN_HASH_BITS) - 1);
    }
}

static const SwFuncEntry *find_function_bs(u64 addr)
{
    u32 lo = 0, hi = sw_function_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (sw_functions[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < sw_function_count && sw_functions[lo].addr == addr) return &sw_functions[lo];
    return NULL;
}

static const char *module_name_for(u64 addr)
{
    u32 i;
    for (i = 0; i < sw_module_count; i++) {
        if (addr >= sw_modules[i].base && addr < sw_modules[i].base + sw_modules[i].image_size)
            return sw_modules[i].name;
    }
    return "<runtime>";
}

/* Forward declarations */
static void (*find_native_thunk(u64 addr))(SwCpu *c);
static void init_critical_native_thunks(void);
static void tick_caller(SwCpu *c);

/* Tail transfers (`b other_function`, computed branches) are trampolined.
 * Generated code always does `sw_dispatch(c, X); return;` for them; while a
 * recompiled function or native thunk runs (g_gen_depth > 0) sw_dispatch only
 * records X, the function returns into the runtime, and the runtime runs X
 * there.  Before, every tail transfer was a nested C call, so a guest loop
 * that crosses function boundaries (jump-table case blocks are separate
 * functions) grew the host stack each iteration until it overflowed.
 * One pending slot is enough: guest threads switch fibers only inside
 * syscalls, never between recording a target and running it. */
/* per host thread: with SW_MULTICORE every guest thread is a host thread
 * (fibers all share the main thread's copy, as before) */
SW_TLS SwGenTls sw_gtls = {1, 0, 0, 0};   /* tick_left, gen_depth, tail_set: read by direct calls in generated code (sw_rt.h) */
/* Per host thread (SW_MULTICORE): sw_nvn.c picks the NVN proc from it, and it is set
 * before the kernel lock is taken, so a shared global let thread A run thread B's proc. */
static SW_TLS u64 g_dispatch_target;
static SW_TLS u64 g_tail_target;
/* function index + 1 of g_tail_target when the generated code knew it (sw_dispatch_i), 0 = unknown;
 * only a hint: it is used only where sw_functions[idx].addr == g_tail_target */
static SW_TLS u32 g_tail_idx;

/* The trampoline state belongs to the guest thread that is running: a fiber
 * switch inside a native call (where g_gen_depth is 0) must not hand depth 0
 * to the next thread, or that thread's cross-function loops recurse on the
 * host stack until it overflows.  sw_thread.c saves/restores it per switch. */
void sw_gen_state_save(u64 st[3]) { st[0] = (u64)g_gen_depth; st[1] = (u64)g_tail_set; st[2] = g_tail_target; }
void sw_gen_state_load(const u64 st[3]) { g_gen_depth = (int)st[0]; g_tail_set = (int)st[1]; g_tail_target = st[2]; }
static void dispatch_run(SwCpu *c, u64 target);

/* ---- guest setjmp / longjmp (Lua's error handling) -------------------------
 * A guest longjmp resumes in the middle of the function that called setjmp,
 * which a recompiled C function cannot be entered at.  Instead:
 *  - every function that calls setjmp (found by scanning the code for
 *    `bl setjmp` / `bl <plt stub of setjmp>` at start-up) runs under a host
 *    setjmp checkpoint in sw_call;
 *  - longjmp is overridden: it restores the guest registers from the guest
 *    jmp_buf (sdk layout: x19-x30 at 0, sp at 0x68, d8-d15 at 0x70), host-
 *    longjmps to the checkpoint of the function that made the setjmp, and
 *    the runtime continues there at the instruction after `bl setjmp` (a
 *    recompiled entry: the scan seeds these addresses for the recompiler).
 * The C frames between the two are unwound without running, like the
 * guest's own frames. */
#define SJ_MAX_CALLERS 64
#define SJ_MAX_FRAMES 256
static u64 g_setjmp_addr, g_longjmp_addr;
static u64 g_sj_callers[SJ_MAX_CALLERS];
static int g_sj_ncallers;
typedef struct { jmp_buf env; SwCpu *cpu; u64 entry_sp; int depth; } SjFrame;
static SW_TLS SjFrame g_sj_frames[SJ_MAX_FRAMES];
static SW_TLS int g_sj_nframes;
static SW_TLS u64 g_sj_resume;

static int sj_is_caller(u64 fn)
{
    for (int i = 0; i < g_sj_ncallers; i++) if (g_sj_callers[i] == fn) return 1;
    return 0;
}

static const SwFuncEntry *function_containing(u64 addr)
{
    u32 lo = 0, hi = sw_function_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (sw_functions[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    return lo ? &sw_functions[lo - 1] : NULL;
}

static void note_missing_seed(u64 target);

static void sj_scan(SwCpu *c)
{
    u64 lookup_symbol_in_all_modules_fwd(const char *name);
    g_setjmp_addr = lookup_symbol_in_all_modules_fwd("setjmp");
    g_longjmp_addr = lookup_symbol_in_all_modules_fwd("longjmp");
    if (!g_setjmp_addr) return;
    for (u32 mi = 0; mi < sw_module_count; mi++) {
        const SwModuleDesc *m = &sw_modules[mi];
        u64 n = m->text_size / 4;
        /* PLT stubs whose GOT slot holds setjmp: adrp x16; ldr x17,[x16,#o]; add; br x17 */
        u64 stubs[8]; int nst = 0;
        for (u64 k = 0; k + 3 < n && nst < 8; k++) {
            u32 *w = (u32 *)mem_host(c, m->base + k * 4, 16, 0);
            if (!w) continue;
            if ((w[0] & 0x9F00001F) != 0x90000010 || (w[1] & 0xFFC003FF) != 0xF9400211 || w[3] != 0xD61F0220) continue;
            u64 pc = m->base + k * 4;
            s64 imm = (s64)((((w[0] >> 5) & 0x7FFFF) << 2) | ((w[0] >> 29) & 3));
            if (imm & (1 << 20)) imm -= 1 << 21;
            u64 got = (pc & ~0xFFFull) + (u64)(imm << 12) + (((w[1] >> 10) & 0xFFF) * 8);
            u64 *v = (u64 *)mem_host(c, got, 8, 0);
            if (v && *v == g_setjmp_addr) stubs[nst++] = pc;
        }
        for (u64 k = 0; k < n; k++) {
            u32 *w = (u32 *)mem_host(c, m->base + k * 4, 4, 0);
            if (!w || (*w & 0xFC000000) != 0x94000000) continue;    /* bl */
            s64 off = (s64)(*w & 0x3FFFFFF);
            if (off & (1 << 25)) off -= 1 << 26;
            u64 pc = m->base + k * 4, t = pc + (u64)(off * 4);
            int hit = t == g_setjmp_addr;
            for (int j = 0; j < nst && !hit; j++) hit = t == stubs[j];
            if (!hit) continue;
            const SwFuncEntry *f = function_containing(pc);
            if (f && !sj_is_caller(f->addr) && g_sj_ncallers < SJ_MAX_CALLERS) g_sj_callers[g_sj_ncallers++] = f->addr;
            if (!find_function(pc + 4)) note_missing_seed(pc + 4);   /* resume point */
            sw_report("setjmp: call at 0x%llx in 0x%llx (resume 0x%llx %s)\n", (unsigned long long)pc,
                      (unsigned long long)(f ? f->addr : 0), (unsigned long long)(pc + 4),
                      find_function(pc + 4) ? "recompiled" : "seeded for the next recompile");
        }
    }
}

/* longjmp(buf, val): restore the guest registers and unwind to the checkpoint */
volatile long long g_sj_longjmps;   /* guest longjmps (SW_THREAD_STATS report) */
static void sj_longjmp(SwCpu *c)
{
    u64 buf = c->x[0], val = c->x[1];
    InterlockedIncrement64(&g_sj_longjmps);
    u8 *b = mem_host(c, buf, 0xb0, 0);
    if (!b) { sw_trap(c, buf, "longjmp: jmp_buf not mapped"); return; }
    u64 regs[12], sp;
    memcpy(regs, b, sizeof regs);
    memcpy(&sp, b + 0x68, 8);
    for (int i = 0; i < 12; i++) c->x[19 + i] = regs[i];
    c->sp = sp;
    for (int i = 0; i < 8; i++) {
        memset(&c->v[8 + i], 0, sizeof c->v[8 + i]);
        memcpy(&c->v[8 + i].d[0], b + 0x70 + 8 * i, 8);
    }
    c->x[0] = val ? val : 1;
    /* innermost checkpoint of this thread whose function was entered with an sp
     * at or above the saved one (the function that called setjmp) */
    for (int i = g_sj_nframes - 1; i >= 0; i--) {
        SjFrame *f = &g_sj_frames[i];
        if (f->cpu != c || f->entry_sp < sp) continue;
        g_sj_resume = c->x[30];
        g_sj_nframes = i + 1;
        longjmp(f->env, 1);
    }
    sw_trap(c, c->x[30], "longjmp without a matching setjmp checkpoint");
}

/* Guest functions replaced by host code at their address (sw_hid.c's
 * nn::hid::detail functions, called from inside the sdk where no relocation
 * can rebind them).  Built on the first call, once every module is loaded. */
int sw_hid_detail_count(void);
const char *sw_hid_detail_name(int i);
void (*sw_hid_detail_fn(int i))(SwCpu *);
static u64 lookup_symbol_in_all_modules(const char *name);
static void register_native_thunk(u64 placeholder_addr, void (*func)(SwCpu *c));
#define SW_REDIRECT_SLOTS 64
static struct { u64 from, to; } g_redirect[SW_REDIRECT_SLOTS];
static int g_redirect_state;   /* 0 not built, 1 built */
static u64 g_redirect_lo = ~0ULL, g_redirect_hi;
static void redirect_build(void)
{
    g_redirect_state = 1;
    int n = sw_hid_detail_count();
    for (int i = 0; i < n && i < SW_REDIRECT_SLOTS; i++) {
        u64 a = lookup_symbol_in_all_modules(sw_hid_detail_name(i));
        if (!a) { sw_report("[hle] %s not found\n", sw_hid_detail_name(i)); continue; }
        u64 t = 0xFF00D000ULL + 8ULL * (u64)i;
        register_native_thunk(t, sw_hid_detail_fn(i));
        g_redirect[i].from = a;
        g_redirect[i].to = t;
        if (a < g_redirect_lo) g_redirect_lo = a;
        if (a > g_redirect_hi) g_redirect_hi = a;
        sw_report("[hle] 0x%llx %s -> host\n", (unsigned long long)a, sw_hid_detail_name(i));
    }
}
static inline u64 redirect(u64 target)
{
    if (!g_redirect_state) redirect_build();
    if (target < g_redirect_lo || target > g_redirect_hi) return target;
    for (int i = 0; i < SW_REDIRECT_SLOTS; i++)
        if (g_redirect[i].from == target) return g_redirect[i].to;
    return target;
}

/* Call-target cache: guest target -> (redirected target, function entry).
 * The redirect table is fixed once built and functions never move, so an
 * entry stays valid; native thunks and longjmp are never cached (the checks
 * in sw_call/dispatch_one still run on the redirected target). */
#define CALL_CACHE_BITS 16
/* One 64-bit word per entry, so guest threads on several host cores never see
 * half an entry: bit 63 valid, 62 plain (may take the sw_call fast path: has
 * code, not longjmp, not a setjmp caller as of `sjn` known callers, not
 * redirected), 61 redirected, 54..60 sjn, 20..51 key >> 2, 0..19 index into
 * sw_functions. */
static volatile u64 g_cc[1u << CALL_CACHE_BITS];
static inline u32 call_hash(u64 k) { return (u32)((k >> 2) ^ (k >> 18)) & ((1u << CALL_CACHE_BITS) - 1); }
#define CC_IDX(e)   ((u32)((e) & 0xFFFFFu))
#define CC_PLAIN(e) (((e) >> 62) & 1)
#define CC_REDIR(e) (((e) >> 61) & 1)
#define CC_SJN(e)   ((int)(((e) >> 54) & 0x7F))
static SW_FORCEINLINE u64 cc_get(u64 k)
{
    u64 e = g_cc[call_hash(k)];
    return (e >> 63) && ((e >> 20) & 0xFFFFFFFFull) == ((k >> 2) & 0xFFFFFFFFull) && !(k & 3) &&
           k < (1ull << 34) ? e : 0;
}
static inline const SwFuncEntry *call_lookup(u64 *target)
{
    u64 k = *target;
    u64 e = cc_get(k);
    if (e) {
        if (CC_REDIR(e)) *target = redirect(k);
        return &sw_functions[CC_IDX(e)];
    }
    u64 t = redirect(k);
    *target = t;
    if (t >= 0xFF000000ULL) return NULL;
    const SwFuncEntry *fn = find_function(t);
    if (fn && !(k & 3) && k < (1ull << 34) && sw_function_count <= 0xFFFFFu && g_sj_ncallers < 128) {
        u64 idx = (u64)(fn - sw_functions);
        int plain = fn->fn && t == k && t != g_longjmp_addr && t != 0x4988324ULL &&   /* inner timer loop count */
                    !(g_sj_ncallers && sj_is_caller(t));
        g_cc[call_hash(k)] = (1ull << 63) | ((u64)plain << 62) | ((u64)(t != k) << 61) |
                             ((u64)g_sj_ncallers << 54) | (((k >> 2) & 0xFFFFFFFFull) << 20) | idx;
    }
    return fn;
}

/* sw_call's fast path is used when no debug / trace / budget option is on
 * (decided once before the guest starts, see g_call_fast below) */
static int g_call_fast;
static void dispatch_run(SwCpu *c, u64 target);

void sw_dispatch(SwCpu *c, u64 target)
{
    if (g_gen_depth > 0) {
        g_tail_target = target;
        g_tail_idx = 0;
        g_tail_set = 1;
        return;
    }
    dispatch_run(c, target);
}
static void dispatch_run_h(SwCpu *c, u64 target, u32 hint);
/* a tail call (`b f`) to a function whose sw_functions index the recompiler knew */
void sw_dispatch_i(SwCpu *c, u64 target, u32 idx)
{
    if (g_gen_depth > 0) {
        g_tail_target = target;
        g_tail_idx = idx + 1;
        g_tail_set = 1;
        return;
    }
    dispatch_run_h(c, target, idx + 1);
}

/* ---- SW_NEWFN=1: first-call trace (gameplay-logic debugging) --------------
 * Every guest function is marked on its first call.  Once logging is on (F8 in
 * the game window, see sw_hooks.c, or SW_NEWFN_DELAY=<s> after start) two kinds
 * of lines are printed:
 *   [newfn] t=<s> #<n> fn <addr> from <caller> lr <x30> x0.. thr <id>
 *       the first call ever of a function (native thunks 0xFF...: "thunk")
 *   [afn] A+<frames> fn <addr> from <caller> lr <x30> x0..
 *       functions run in the 4 frames from an A press that did not run in the
 *       16 frames before it (input handlers run only on the press)
 * Pressing A at a door after F8 then shows the code that ran because of the
 * press - or that nothing did.  Off by default: one flag test per call. */
int sw_newfn_mode;                        /* 0 off, 1 marking, 2 marking + logging */
static u8 *g_newfn_seen;                  /* one byte per sw_functions entry */
static u32 *g_newfn_last;                 /* frame of the last call, per entry (mode 2) */
static u64 g_newfn_thunk[4096];
static volatile long g_newfn_n, g_afn_n;
static u64 g_newfn_t0;
static volatile u32 g_newfn_frame = 1, g_afn_frame;
static u64 newfn_ms(void)
{
#if defined(_WIN32)
    return (u64)GetTickCount64();
#else
    return (u64)clock() * 1000 / CLOCKS_PER_SEC;
#endif
}
double sw_newfn_time(void) { return (double)(newfn_ms() - g_newfn_t0) / 1000.0; }
void sw_newfn_init(void)
{
    const char *e = getenv("SW_NEWFN");
    if (!e || *e != '1') return;
    g_newfn_seen = (u8 *)calloc(sw_function_count ? sw_function_count : 1, 1);
    g_newfn_last = (u32 *)calloc(sw_function_count ? sw_function_count : 1, sizeof(u32));
    if (!g_newfn_seen || !g_newfn_last) return;
    g_newfn_t0 = newfn_ms();
    sw_newfn_mode = 1;
    { extern u8 *g_direct_idx; g_direct_idx = NULL; }   /* direct calls skip the first-call marking: the runtime path only (sw_rt.h) */
    sw_report("[newfn] marking first calls of %u functions; logging starts with F8\n", sw_function_count);
}
void sw_newfn_start(void)
{
    if (sw_newfn_mode != 1) return;
    sw_newfn_mode = 2;
    sw_report("[newfn] logging from t=%.3f frame %u\n", sw_newfn_time(), g_newfn_frame);
}
void sw_newfn_present(void) { if (sw_newfn_mode) g_newfn_frame++; }
void sw_newfn_apress(void)
{
    if (sw_newfn_mode != 2) return;
    g_afn_frame = g_newfn_frame;
    sw_report("[afn] A pressed at t=%.3f frame %u\n", sw_newfn_time(), g_newfn_frame);
}
static void newfn_line(SwCpu *c, const char *tag, const char *kind, u64 target, long n)
{
    fprintf(stderr, "%s t=%.3f #%ld %s 0x%llx from 0x%llx lr 0x%llx x0 0x%llx x1 0x%llx x2 0x%llx thr %llu\n", tag,
            sw_newfn_time(), n, kind, (unsigned long long)target, (unsigned long long)c->cur_fn,
            (unsigned long long)c->x[30], (unsigned long long)c->x[0], (unsigned long long)c->x[1],
            (unsigned long long)c->x[2], (unsigned long long)c->thread_id);
}
static void newfn_note(SwCpu *c, u32 idx, u64 target)
{
    if (!g_newfn_seen[idx]) {
        g_newfn_seen[idx] = 1;
        if (sw_newfn_mode == 2) {
            long n = InterlockedIncrement(&g_newfn_n);
            if (n <= 60000) newfn_line(c, "[newfn]", "fn", target, n);
        }
    }
    if (sw_newfn_mode != 2) return;
    u32 f = g_newfn_frame, last = g_newfn_last[idx];
    if (last == f) return;
    g_newfn_last[idx] = f;
    u32 a = g_afn_frame;
    if (a && f - a < 4 && last + 16 < a) {
        long n = InterlockedIncrement(&g_afn_n);
        if (n <= 60000) {
            char kind[16];
            snprintf(kind, sizeof kind, "A+%u fn", f - a);
            newfn_line(c, "[afn]", kind, target, n);
        }
    }
}
static void newfn_thunk(SwCpu *c, u64 target)
{
    u32 h = (u32)((target >> 3) * 2654435761u) & 4095;
    for (int k = 0; k < 4096; k++, h = (h + 1) & 4095) {
        if (g_newfn_thunk[h] == target) return;
        if (!g_newfn_thunk[h]) { g_newfn_thunk[h] = target; break; }
    }
    if (sw_newfn_mode == 2) {
        long n = InterlockedIncrement(&g_newfn_n);
        if (n <= 60000) newfn_line(c, "[newfn]", "thunk", target, n);
    }
}
#define NEWFN(c, idx, t) do { if (sw_newfn_mode) newfn_note((c), (u32)(idx), (t)); } while (0)

#if defined(_MSC_VER)
#define SW_NOINLINE __declspec(noinline)
#else
#define SW_NOINLINE __attribute__((noinline))
#endif
static u64 sw_tick(void);   /* defined with the system registers below (SW_THUNK_SLOW timing) */
static SW_NOINLINE void dispatch_one_slow(SwCpu *c, u64 target);

/* Tail dispatch.  The fast path is kept small on purpose: with the slow path
 * (debug output, char buffers, traps) in the same function MSVC saved 7
 * registers and a 450-byte frame on every tail call.  It no longer counts
 * g_dispatch_count: that plain global was written by every guest thread on
 * every tail call (one cache line bouncing between all cores, the hottest
 * instruction of dispatch_one); its readers (watchdog, --max-dispatch) turn
 * the fast path off anyway. */
/* Direct-index calls (SWRECOMP_CALL_IDX, default on): the generated code passes the sw_functions index of
 * a known call / tail-call target, so the fast path reads one byte of g_plain_idx and the entry itself
 * instead of the call-cache hash (whose load was the hottest instruction of sw_call and dispatch_run: a
 * cache miss into a 512 KB table, then a dependent load of the entry).  g_plain_idx[i] = 1 where entry i
 * may take the fast path (same rules as CC_PLAIN, decided once after sj_scan: has code, not redirected,
 * not longjmp, not a setjmp caller, not the inner timer loop or VAbort).  The index is only a hint: it is
 * used only if sw_functions[idx].addr == target (same cache line as ->fn), else the old path runs.
 * NULL = off (debug options, SW_CALL_IDX=0). */
u8 *g_plain_idx;
/* the guard of direct calls and of the indirect-call cache in generated code (sw_rt.h): g_plain_idx, or NULL with
 * SW_DIRECT=0 (A/B: those calls then take the sw_call_i / sw_call path as before) */
u8 *g_direct_idx;
static int g_ic_on = 1;       /* SW_IC=0: indirect-call sites never cache a target */
/* A hook (sw_hooks.c install) swaps sw_functions[i].fn for a wrapper: direct calls in generated code would skip
 * the wrapper, so a hooked entry leaves the direct path (now, or when g_plain_idx is built). */
#define SW_HOOKED_MAX 256
static u32 g_hooked[SW_HOOKED_MAX];
static int g_nhooked;
void sw_fn_hooked(u32 i)
{
    if (g_nhooked < SW_HOOKED_MAX) g_hooked[g_nhooked++] = i;
    else sw_report("[hook] more than %d hooks: direct calls off\n", SW_HOOKED_MAX), g_plain_idx = g_direct_idx = NULL;
    if (g_plain_idx && i < sw_function_count) g_plain_idx[i] = 0;   /* g_direct_idx is the same array */
}

static SW_FORCEINLINE void dispatch_fast_body(SwCpu *c, const SwFuncEntry *fn, u64 t)
{
    u64 caller_fn = c->cur_fn;
    NEWFN(c, fn - sw_functions, t);
    dispatch_ring_add(caller_fn, t);
    c->prev_fn = caller_fn;
    c->call_depth++;
    g_gen_depth++;
    fn->fn(c);
    g_gen_depth--;
    c->call_depth--;
    c->cur_fn = caller_fn;
    if (c->halted && (g_trap_armed || sw_thread_is_worker(c))) sw_trap_jump(c);
}

/* Tail dispatch.  The fast path is kept small on purpose: with the slow path
 * (debug output, char buffers, traps) in the same function MSVC saved 7
 * registers and a 450-byte frame on every tail call.  It no longer counts
 * g_dispatch_count: that plain global was written by every guest thread on
 * every tail call (one cache line bouncing between all cores, the hottest
 * instruction of dispatch_one); its readers (watchdog, --max-dispatch) turn
 * the fast path off anyway. */
static SW_FORCEINLINE void dispatch_one(SwCpu *c, u64 target)
{
    if (g_call_fast) {
        u64 e = cc_get(target);
        if (e && CC_PLAIN(e) && CC_SJN(e) == g_sj_ncallers) {
            if (target == g_vabort_addr && target) report_sdk_abort(c);
            dispatch_fast_body(c, &sw_functions[CC_IDX(e)], target);
            return;
        }
    }
    dispatch_one_slow(c, target);
}

/* hint: sw_functions index + 1 of target, 0 = unknown */
static void dispatch_run_h(SwCpu *c, u64 target, u32 hint)
{
    for (;;) {
        const u8 *pl = g_plain_idx;
        if (hint && pl && hint <= sw_function_count && pl[hint - 1] && sw_functions[hint - 1].addr == target)
            dispatch_fast_body(c, &sw_functions[hint - 1], target);
        else
            dispatch_one(c, target);
        if (!g_tail_set) return;
        g_tail_set = 0;
        target = g_tail_target;
        hint = g_tail_idx;
        if (c->halted) return;
    }
}

static void dispatch_run(SwCpu *c, u64 target)
{
    dispatch_run_h(c, target, 0);
}

static SW_NOINLINE void dispatch_one_slow(SwCpu *c, u64 target)
{
    const SwFuncEntry *fn_cached = call_lookup(&target);
    if (target == g_longjmp_addr && target) { sj_longjmp(c); return; }
    /* Check for native thunks in the 0xFF000000+ range first */
    if (target >= 0xFF000000ULL) {
        void (*native_thunk)(SwCpu *c) = find_native_thunk(target);
        if (native_thunk) {
            if (sw_newfn_mode) newfn_thunk(c, target);
            if (g_trace_dispatch)
                fprintf(stderr, "DISPATCH: 0x%llx -> 0x%llx (native thunk)\n",
                        (unsigned long long)c->cur_fn, (unsigned long long)target);
            dispatch_ring_add(c->cur_fn, target);
            g_dispatch_count++;
            if (dispatch_budget_exhausted(c)) return;
            c->prev_fn = c->cur_fn;
            trace_edge(c->cur_fn, target);
            g_dispatch_target = target;
            g_gen_depth++;
            /* HLE thunks: one guest thread at a time (kernel lock).  NVN procs
             * (0xFF100000 + 8*i) do not take it: the graphics layer has its own
             * locks and gives the emulated core up around its waits (kern_core_away);
             * holding the kernel lock there stalled every thread for up to 200 ms. */
            static int nvn_locked = -1;   /* SW_NVN_LOCK=1: NVN procs under the kernel lock too (race hunting) */
            if (nvn_locked < 0) { const char *e = getenv("SW_NVN_LOCK"); nvn_locked = e && *e == '1'; }
            int nvn_proc = !nvn_locked && target >= 0xFF100000ULL && target < 0xFF200000ULL;
            if (!nvn_proc) { sw_klock(); sw_klock_note(target); }
            /* SW_THUNK_SLOW=<ms>: a line for each host function (NVN proc / HLE thunk) slower than that - which host
             * call a hitch was in (the render thread's 190 ms at the start of the village route) */
            static double thunk_slow = -1;
            if (thunk_slow < 0) { const char *e = getenv("SW_THUNK_SLOW"); thunk_slow = e && *e ? atof(e) : 0; }
            u64 th0 = thunk_slow > 0 ? sw_tick() : 0;
            extern void sw_perf_thread_ms(double out[7]);   /* PERF_N kinds (gfx/compat/host.h) */
            double pf0[7], bl0 = 0, lw0 = 0;
            extern void sw_thread_wait_ms(double *blocked, double *lockwait);
            extern void sw_tex_up_counts(unsigned long long *n, unsigned long long *bytes);
            unsigned long long un0 = 0, ub0 = 0;
            if (th0) sw_perf_thread_ms(pf0), sw_thread_wait_ms(&bl0, &lw0), sw_tex_up_counts(&un0, &ub0);
            native_thunk(c);
            if (th0) {
                double ms = (double)(sw_tick() - th0) / 19200.0;
                if (ms > thunk_slow) {
                    extern const char *sw_nvn_proc_name(u64 addr);
                    const char *nm = sw_nvn_proc_name(target);
                    double pf1[7], bl1, lw1;
                    sw_perf_thread_ms(pf1);
                    sw_thread_wait_ms(&bl1, &lw1);
                    unsigned long long un1, ub1;
                    sw_tex_up_counts(&un1, &ub1);
                    sw_report("[thunk]   texture uploads in it: %llu, %.1f MiB\n", un1 - un0,
                              (double)(ub1 - ub0) / 1048576.0);
                    sw_report("[thunk] 0x%llx %s took %.1f ms (lr 0x%llx): pace %.1f fence %.1f drain %.1f pipe %.1f "
                              "xlate %.1f tex %.1f astc %.1f ms; guest-blocked %.1f kernel-lock %.1f ms\n",
                              (unsigned long long)target, nm ? nm : "", ms,
                              (unsigned long long)c->x[30], pf1[0] - pf0[0], pf1[1] - pf0[1], pf1[2] - pf0[2],
                              pf1[3] - pf0[3], pf1[4] - pf0[4], pf1[5] - pf0[5], pf1[6] - pf0[6], bl1 - bl0, lw1 - lw0);
                }
            }
            if (!nvn_proc) sw_kunlock();
            g_gen_depth--;
            return;
        }
    }

    const SwFuncEntry *fn = fn_cached ? fn_cached : find_function(target);

    if (g_trace_dispatch)
    {
        /* also show x0/x1 as text when they point at a printable string
         * (paths, names): makes nn::fs / service calls readable in traces */
        char s0[48], s1[48];
        guest_str(c, c->x[0] > 0x100000 ? c->x[0] : 0, s0, sizeof s0);
        guest_str(c, c->x[1] > 0x100000 ? c->x[1] : 0, s1, sizeof s1);
        int p0 = strlen(s0) >= 3, p1 = strlen(s1) >= 3;
        for (char *q = s0; *q && p0; q++) if (*q < 0x20 || *q > 0x7e) p0 = 0;
        for (char *q = s1; *q && p1; q++) if (*q < 0x20 || *q > 0x7e) p1 = 0;
        fprintf(stderr, "DISPATCH: 0x%llx -> 0x%llx (%s)%s%s%s%s%s\n",
                (unsigned long long)c->cur_fn, (unsigned long long)target,
                fn ? fn->name : "(missing)", p0 ? " s0=\"" : "", p0 ? s0 : "", p0 ? "\"" : "",
                p1 ? " s1=\"" : "", p1 ? s1 : "");
    }

    /* Ring buffer for crash diagnostics */
    dispatch_ring_add(c->cur_fn, target);

    /* Profiling: record gap from previous dispatch */
    if (g_profile_enabled) {
        u64 now = sw_now_ns();
        u64 load_store_delta = g_load_store_count - g_prev_load_store_count;
        if (g_prev_dispatch_ts != 0) {
            profile_record_gap(g_prev_target, target, now - g_prev_dispatch_ts, load_store_delta);
        }
        g_prev_dispatch_ts = now;
        g_prev_target = target;
        g_prev_load_store_count = g_load_store_count;
    }

    g_dispatch_count++;
    /* Track frames: increment on entry to main loop (0x4987ff8) */
    if (target == 0x4987ff8ULL) {
        g_frame_counter++;
        fprintf(stderr, "FRAME %llu starting (cur_fn=0x%llx)\n",
                (unsigned long long)g_frame_counter, (unsigned long long)c->cur_fn);
    }
    /* Track inner timer loop (0x4988324) - use counter for nested calls */
    if (target == 0x4988324ULL) {
        g_in_inner_timer_loop++;
    }
    if (target == g_vabort_addr && target) report_sdk_abort(c);
    if (dispatch_budget_exhausted(c)) return;
    if (!fn) {
        char reason[256];
        g_missing_dispatch_count++;
        note_missing_seed(target);
        sw_report("debug: missing dispatch target 0x%llx (module %s)\n",
                  (unsigned long long)target, module_name_for(target));
        if (discover_skip(c))
            return;
        /* Gracefully handle NULL function pointers (target == 0) by halting */
        if (target == 0) {
            sw_report("FATAL: NULL function pointer dispatched (caller pc=0x%llx target=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx)\n",
                      (unsigned long long)c->cur_fn, (unsigned long long)target,
                      (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
            sw_trap(c, 0, "NULL function pointer dispatch - halting per strict mode");
        }
        snprintf(reason, sizeof(reason),
                 "no recompiled function for 0x%llx (module %s); this address was reached "
                 "by a computed branch - add it as a seed or fix the analysis",
                 (unsigned long long)target, module_name_for(target));
        sw_trap(c, target, reason);
        return;
    }
    c->prev_fn = c->cur_fn;
    trace_edge(c->cur_fn, target);
    NEWFN(c, fn - sw_functions, target);
    /* Actually run the target.  This call was missing, so every tail call
     * (`b other_function`) and computed jump silently did nothing and the
     * caller carried on with stale registers. */
    {
        u64 caller_fn = c->cur_fn;
        c->call_depth++;
        g_gen_depth++;
        if (fn->fn)
            fn->fn(c);
        g_gen_depth--;
        c->call_depth--;
        c->cur_fn = caller_fn;
        if (target == 0x4988324ULL && g_in_inner_timer_loop > 0)
            g_in_inner_timer_loop--;
        if (c->halted && (g_trap_armed || sw_thread_is_worker(c))) {
            sw_trap_jump(c);
        }
    }
}

/* Record a computed-branch target that was not part of the recompiled set.
 * The analysis step reads this file back so the next recompile includes it;
 * this keeps static discovery converging without ever executing ARM64 code. */
static void note_missing_seed(u64 target)
{
    char path[1200];
    FILE *f;
    u32 i;
    u64 relative = target;
    const char *owner = "?";
    for (i = 0; i < sw_module_count; i++) {
        if (target >= sw_modules[i].base && target < sw_modules[i].base + sw_modules[i].image_size) {
            relative = target - sw_modules[i].base;
            owner = sw_modules[i].name;
        }
    }
    snprintf(path, sizeof(path), "%s/missing_seeds.txt", g_base_dir);
    f = fopen(path, "a");
    if (!f) return;
    /* The address is only meaningful inside its own module: analysis must not
       hand a sdk offset to main. */
    fprintf(f, "%s:0x%llx\n", owner, (unsigned long long)relative);
    fclose(f);
}

/* Discovery mode: instead of trapping on the first unknown target, record it
 * and let the caller continue with x0 = 0.  The guest state after such a call
 * is not trustworthy, but it lets one run surface many missing seeds that
 * would otherwise each cost a full recompile/build cycle to find. */
static u32 g_discover_budget;

static int discover_skip(SwCpu *c)
{
    if (!g_discover_budget)
        return 0;
    g_discover_budget--;
    c->x[0] = 0;
    return 1;
}

void sw_thread_tick(SwCpu *c);
/* sw_thread.c: guest calls left until the next time-slice check (per host thread) */
/* SW_CALL_DUMP=addr,reg,off,len: on each of the first 5 calls of addr, dump
 * len bytes at x<reg>+off (as hex words and floats) - a debugging aid. */
static void call_dump(SwCpu *c, u64 target)
{
    static int init, n, reg; static u64 addr, off, len;
    if (!init) {
        init = 1;
        const char *e = getenv("SW_CALL_DUMP");
        if (e) sscanf(e, "%llx,%d,%llx,%llx", (unsigned long long *)&addr, &reg, (unsigned long long *)&off,
                      (unsigned long long *)&len);
    }
    static long long skip = -1;
    if (skip < 0) { const char *k = getenv("SW_CALL_DUMP_SKIP"); skip = k ? atoll(k) : 0; }
    if (!addr || target != addr || reg < 0 || reg > 30) return;
    if (skip > 0) { skip--; return; }   /* SW_CALL_DUMP_SKIP=N: ignore the first N calls */
    if (n >= 5) return;
    n++;
    u64 base = c->x[reg] + off;
    sw_report("[call-dump] 0x%llx: x%d=0x%llx +0x%llx (caller fn 0x%llx lr 0x%llx):\n", (unsigned long long)target, reg,
              (unsigned long long)c->x[reg], (unsigned long long)off, (unsigned long long)c->cur_fn,
              (unsigned long long)c->x[30]);
    for (u64 i = 0; i + 4 <= len; i += 4) {
        u8 *p = mem_host(c, base + i, 4, 0);
        u32 v = 0; float f = 0;
        if (p) { memcpy(&v, p, 4); memcpy(&f, p, 4); }
        sw_report("  +0x%03llx: %08x  %g\n", (unsigned long long)(off + i), v, (double)f);
    }
}

int sw_lockstep_match(u64 target);
extern int sw_lockstep_state;
void sw_lockstep_run(SwCpu *c, u64 target);

/* sw_call's slow path: lockstep, uncached targets, native thunks, setjmp
 * callers, all debug options.  It is a separate non-inlined function because
 * MSVC never inlines a function that calls setjmp: sw_call used to be a
 * wrapper calling this body, which (debug buffers, setjmp frame) saved 7
 * registers and a 400-byte frame on every guest call. */
static SW_NOINLINE void sw_call_slow(SwCpu *c, u64 target)
{
    if (sw_lockstep_state && sw_lockstep_match(target)) { sw_lockstep_run(c, target); return; }
    const SwFuncEntry *fn_cached = call_lookup(&target);
    call_dump(c, target);
    if (--sw_tick_left <= 0) sw_thread_tick(c);   /* time slice between guest threads */
    /* Native thunks (NVN procs, HLE imports) handed out as function pointers
     * are reached by `blr` too, not only by tail dispatch. */
    if (target == g_longjmp_addr && target) { sj_longjmp(c); return; }
    if (target >= 0xFF000000ULL && find_native_thunk(target)) {
        u64 caller_fn = c->cur_fn;
        int saved_depth = g_gen_depth;
        g_gen_depth = 0;                  /* run it (and any tail chain) now */
        dispatch_run(c, target);
        g_gen_depth = saved_depth;
        c->cur_fn = caller_fn;
        return;
    }
    const SwFuncEntry *fn = fn_cached ? fn_cached : find_function(target);

    if (g_trace_dispatch)
        fprintf(stderr, "SW_CALL: 0x%llx -> 0x%llx (%s) x0=0x%llx x1=0x%llx x2=0x%llx sp=0x%llx\n",
                (unsigned long long)c->cur_fn, (unsigned long long)target,
                fn ? fn->name : "(missing)",
                (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                (unsigned long long)c->sp);

    /* DEBUG: Log entry to 0x1079470 with full register state */
    if (target == 0x1079470ULL) {
        sw_report("debug: ENTRY 0x1079470: x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx sp=0x%llx tpidr=0x%llx\n",
                  (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                  (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                  (unsigned long long)c->sp, (unsigned long long)c->tpidr_el0);
    }

    /* Ring buffer for crash diagnostics */
    dispatch_ring_add(c->cur_fn, target);



    /* Profiling: record gap from previous dispatch */
    if (g_profile_enabled) {
        u64 now = sw_now_ns();
        u64 load_store_delta = g_load_store_count - g_prev_load_store_count;
        if (g_prev_dispatch_ts != 0) {
            profile_record_gap(g_prev_target, target, now - g_prev_dispatch_ts, load_store_delta);
        }
        g_prev_dispatch_ts = now;
        g_prev_target = target;
        g_prev_load_store_count = g_load_store_count;
    }
    
    g_dispatch_count++;
    if (target == g_vabort_addr && target) report_sdk_abort(c);
    if (dispatch_budget_exhausted(c)) return;
    if (!fn) {
                char reason[256];
                g_missing_dispatch_count++;
                note_missing_seed(target);
                if (target == 0) {
                    u64 *p0 = (u64 *)mem_host(c, 0x432e3f8ULL, 8, 0);
                    u64 v0 = p0 ? *p0 : 0xFFFFFFFFFFFFFFFFULL;
                    u64 *p1 = v0 != 0xFFFFFFFFFFFFFFFFULL ? (u64 *)mem_host(c, v0, 8, 0) : NULL;
                    u64 v1 = p1 ? *p1 : 0xFFFFFFFFFFFFFFFFULL;
                    sw_report("debug: NULL call: [0x432e3f8]=0x%llx [[0x432e3f8]]=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x19=0x%llx x20=0x%llx\n",
                              (unsigned long long)v0, (unsigned long long)v1,
                              (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                              (unsigned long long)c->x[19], (unsigned long long)c->x[20]);
                }
                sw_report("debug: missing dispatch target 0x%llx (module %s)\\n", (unsigned long long)target, module_name_for(target));
                if (discover_skip(c))
                    return;
        snprintf(reason, sizeof(reason),
                 "call to non-recompiled address 0x%llx (module %s)",
                 (unsigned long long)target, module_name_for(target));
        sw_trap(c, target, reason);
        return;
    }
    c->prev_fn = c->cur_fn;
        trace_edge(c->cur_fn, target);
        NEWFN(c, fn - sw_functions, target);
        c->call_depth++;
        /* DEBUG: Log 0x495b390 (memset) args when dispatched */
        if (target == 0x495b390ULL && g_debug_enabled) {
            sw_report("debug: 0x495b390 called: dest=0x%llx fill=0x%llx size=0x%llx\n",
                      (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
        }
        /* cur_fn must name the function that is running *now*.  It was written
                 * on entry and never restored, so a trap taken after any call named the
                 * last callee that had already returned - which is how 0x47610b0 came to
                 * be blamed for a fault in its caller's caller. */
            if (target != 0) {
                u64 caller_fn = c->cur_fn;
                if (fn->fn && g_sj_ncallers && sj_is_caller(target) && g_sj_nframes < SJ_MAX_FRAMES) {
                    /* a function that calls setjmp: run it under a host checkpoint */
                    int slot = g_sj_nframes++;
                    SjFrame *fr = &g_sj_frames[slot];
                    fr->cpu = c;
                    fr->entry_sp = c->sp;
                    fr->depth = g_gen_depth;
                    if (setjmp(fr->env) == 0) {
#if defined(_MSC_VER) && defined(_M_X64)
                        /* Frame 0: the CRT longjmp restores the registers directly instead of an SEH unwind through
                         * every frame in between (RtlUnwindEx + __GSHandlerCheck per frame: ~2% of the render
                         * thread with Lua's coroutine / error longjmps).  Nothing between here and sj_longjmp
                         * needs unwinding: generated code and the runtime's call path have no __try / __finally. */
                        ((_JUMP_BUFFER *)fr->env)->Frame = 0;
#endif
                        g_gen_depth++;
                        fn->fn(c);
                        g_gen_depth--;
                        if (g_tail_set && !c->halted) {
                            u64 t = g_tail_target;
                            int saved_depth = g_gen_depth;
                            g_tail_set = 0;
                            g_gen_depth = 0;
                            dispatch_run(c, t);
                            g_gen_depth = saved_depth;
                        }
                        g_tail_set = 0;
                    } else {
                        /* landed from a guest longjmp: registers are restored; go on
                         * after the `bl setjmp` with x0 = the longjmp value */
                        fr = &g_sj_frames[slot];
                        g_gen_depth = fr->depth;
                        g_tail_set = 0;
                        u64 resume = g_sj_resume;
                        int saved = g_gen_depth;
                        g_gen_depth = 0;
                        dispatch_run(c, resume);
                        g_gen_depth = saved;
                    }
                    g_sj_nframes = slot;
                } else if (fn->fn) {
                    g_gen_depth++;
                    fn->fn(c);
                    g_gen_depth--;
                    if (g_tail_set && !c->halted) {   /* the callee ended in a tail transfer */
                        u64 t = g_tail_target;
                        int saved_depth = g_gen_depth;
                        g_tail_set = 0;
                        g_gen_depth = 0;
                        dispatch_run(c, t);
                        g_gen_depth = saved_depth;
                    }
                    g_tail_set = 0;
                } else {
                    sw_report("warning: function 0x%llx has NULL implementation pointer, skipping\\n", (unsigned long long)target);
                    c->x[0] = 0;
                }
                c->cur_fn = caller_fn;
                if (g_trace_dispatch)
                    fprintf(stderr, "SW_RET: 0x%llx <- 0x%llx x0=0x%llx\n", (unsigned long long)caller_fn,
                            (unsigned long long)target, (unsigned long long)c->x[0]);
            }
            c->call_depth--;
            /* Decrement inner timer loop counter on exit from 0x4988324 */
            if (target == 0x4988324ULL && g_in_inner_timer_loop > 0) {
                g_in_inner_timer_loop--;
            }
            if (c->halted) {
                if (g_trap_armed || sw_thread_is_worker(c)) sw_trap_jump(c);
            }
        }

/* The fast-path body of a guest call (sw_call / sw_call_i): fn is a plain entry. */
static SW_FORCEINLINE void call_fast_body(SwCpu *c, const SwFuncEntry *fn, u64 target)
{
    u64 caller_fn = c->cur_fn;
    NEWFN(c, fn - sw_functions, target);
    if (--sw_tick_left <= 0) sw_thread_tick(c);
    dispatch_ring_add(caller_fn, target);
    c->prev_fn = caller_fn;
    c->call_depth++;
    g_gen_depth++;
    fn->fn(c);
    g_gen_depth--;
    if (g_tail_set && !c->halted) {   /* the callee ended in a tail transfer */
        u64 tt = g_tail_target;
        u32 th = g_tail_idx;
        int saved_depth = g_gen_depth;
        g_tail_set = 0;
        g_gen_depth = 0;
        dispatch_run_h(c, tt, th);
        g_gen_depth = saved_depth;
    }
    g_tail_set = 0;
    c->cur_fn = caller_fn;
    c->call_depth--;
    if (c->halted && (g_trap_armed || sw_thread_is_worker(c))) sw_trap_jump(c);
}

/* Every guest `bl`/`blr`.  Fast path: a cached plain target, no debug option
 * on (sw_call_slow has the same effects for such a target, plus its per-call
 * debug checks).  sw_call hands c back (sw_rt.h: generated code continues with
 * the returned pointer, which avoids an MSVC miscompile). */
SwCpu *sw_call(SwCpu *c, u64 target)
{
    u64 e;
    if (!sw_lockstep_state && g_call_fast && (e = cc_get(target)) != 0 && CC_PLAIN(e) &&
        CC_SJN(e) == g_sj_ncallers) {
        if (target == g_vabort_addr && target) report_sdk_abort(c);
        call_fast_body(c, &sw_functions[CC_IDX(e)], target);
        return c;
    }
    sw_call_slow(c, target);
    return c;
}

/* `bl f` where the recompiler knew f's sw_functions index (see g_plain_idx) */
SwCpu *sw_call_i(SwCpu *c, u64 target, u32 idx)
{
    const u8 *pl = g_plain_idx;
    if (pl && idx < sw_function_count && pl[idx] && sw_functions[idx].addr == target) {
        const SwFuncEntry *fn = &sw_functions[idx];
        call_fast_body(c, fn, target);
        return c;
    }
    return sw_call(c, target);
}

SwCpu *sw_call_reg(SwCpu *c, u64 target)
{
    return sw_call(c, target);
}

/* The rare ends of a direct call (sw_rt.h sw_call_d_, generated code): the same steps as call_fast_body after
 * fn->fn(c) - the tail chain the callee queued, and a halted guest thread. */
void sw_call_d_tail(SwCpu *c)
{
    if (g_tail_set && !c->halted) {
        u64 tt = g_tail_target;
        u32 th = g_tail_idx;
        int saved_depth = g_gen_depth;
        g_tail_set = 0;
        g_gen_depth = 0;
        dispatch_run_h(c, tt, th);
        g_gen_depth = saved_depth;
    }
    g_tail_set = 0;
}
/* an indirect call whose site cache (sw_rt.h sw_call_r_) did not match: the normal call, then the site keeps
 * this target's entry if it may take the direct path */
/* fill a 2-entry site cache with target's entry (newest first) if it may take the direct path */
static void ic_fill(u64 target, const void **ic)
{
    const u8 *pl = g_direct_idx;
    if (pl && g_ic_on && !sw_lockstep_state && !sw_newfn_mode) {
        u64 e = cc_get(target);
        if (e && CC_PLAIN(e) && !CC_REDIR(e) && CC_IDX(e) < sw_function_count && pl[CC_IDX(e)] &&
            sw_functions[CC_IDX(e)].addr == target) {
            const void *ent = &sw_functions[CC_IDX(e)];
            if (ic[0] != ent) { ic[1] = ic[0]; ic[0] = ent; }
        }
    }
}
SwCpu *sw_call_ic_miss(SwCpu *c, u64 target, const void **ic)
{
    ic_fill(target, ic);
    return sw_call(c, target);
}
/* an indirect tail jump whose site cache missed (sw_rt.h SW_TAIL_R) */
void sw_dispatch_ic_miss(SwCpu *c, u64 target, const void **ic)
{
    ic_fill(target, ic);
    sw_dispatch(c, target);
}
void sw_call_d_halt(SwCpu *c)
{
    if (c->halted && (g_trap_armed || sw_thread_is_worker(c))) sw_trap_jump(c);
}

/* ------------------------------------------------------------------ */
/* System register access                                             */
/* ------------------------------------------------------------------ */
/* CNTVCT / GetSystemTick: 19.2 MHz ticks since "boot", as on the console.
 * This used to be the Unix wall clock in ticks (~3.4e16, an uptime of ~56
 * years): code that turns ticks into float seconds then had 128 s steps, so
 * short game timers (e.g. the talk / interaction cool-downs) never ran out.
 * Now: monotonic host time since the first call + 60 s (a console booted a
 * minute ago; 0 is avoided because games use it as "not set"). */
#define SW_TICK_BOOT_OFFSET (60ull * 19200000ull)
/* Fast path (2026-11-16): QueryPerformanceCounter plus two 64-bit divisions cost ~2.6% of the main guest thread (the
 * game reads its tick often; the multi-core spin loop reads it too).  After 250 ms of QPC time the TSC rate is measured
 * against QPC once and later reads are rdtsc + one 128-bit multiply, continuing from the QPC value at the switch.
 * Only with an invariant TSC (CPUID 0x80000007 EDX bit 8: constant rate, synchronised cores); SW_TICK_QPC=1 keeps QPC. */
#if defined(_WIN32)
#include <intrin.h>
volatile LONG g_tsc_mode;                          /* 0 measuring / QPC, 1 TSC, 2 QPC for good (also read inline */
u64 g_tsc_base_tick, g_tsc_base, g_tsc_mul;        /* by generated code: sw_rt.h sw_cntvct_); tick = base_tick + */
                                                   /* ((tsc - base) * mul) >> 40 */
static u64 g_tsc_cal_tsc;
static LONG64 g_tsc_cal_qpc;
#endif
static u64 sw_tick(void)
{
#if defined(_WIN32)
    if (g_tsc_mode == 1) {
        u64 hi, lo = _umul128(__rdtsc() - g_tsc_base, g_tsc_mul, &hi);
        return g_tsc_base_tick + ((hi << 24) | (lo >> 40));
    }
    static LARGE_INTEGER freq;
    static volatile LONG64 t0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!t0) {
        QueryPerformanceFrequency(&freq);
        if (InterlockedCompareExchange64(&t0, now.QuadPart, 0) == 0) {
            const char *e = getenv("SW_TICK_QPC");
            int inv[4];
            __cpuid(inv, (int)0x80000007);
            if ((e && *e == '1') || !(inv[3] & (1 << 8))) g_tsc_mode = 2;
            g_tsc_cal_tsc = __rdtsc(), g_tsc_cal_qpc = now.QuadPart;
        }
    }
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    u64 el = (u64)(now.QuadPart - t0), f = (u64)freq.QuadPart;
    u64 tick = SW_TICK_BOOT_OFFSET + (el / f) * 19200000ull + (el % f) * 19200000ull / f;
    if (g_tsc_mode == 0 && g_tsc_cal_qpc && (u64)(now.QuadPart - g_tsc_cal_qpc) > f / 4) {
        static volatile LONG once;
        if (!InterlockedExchange(&once, 1)) {
            u64 tsc = __rdtsc();
            double tsc_hz = (double)(tsc - g_tsc_cal_tsc) * (double)f / (double)(u64)(now.QuadPart - g_tsc_cal_qpc);
            if (tsc_hz > 1e8) {
                g_tsc_mul = (u64)(19200000.0 / tsc_hz * 1099511627776.0);   /* 2^40 */
                g_tsc_base = tsc, g_tsc_base_tick = tick;
                MemoryBarrier();
                InterlockedExchange(&g_tsc_mode, 1);
                fprintf(stderr, "[tick] TSC clock: %.1f MHz\n", tsc_hz / 1e6);
            } else
                InterlockedExchange(&g_tsc_mode, 2);
        }
    }
    return tick;
#else
    static u64 t0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 ns = (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
    if (!t0) t0 = ns;
    return SW_TICK_BOOT_OFFSET + (ns - t0) / 1000000000ull * 19200000ull + (ns - t0) % 1000000000ull * 192ull / 10000ull;
#endif
}
u64 sw_tick_now(void) { return sw_tick(); }

/* SW_TICK_CALLERS=1: which guest code reads the clock, how often (60 fps work: the
 * game's frame step).  GetSystemTick is a leaf, so x30 is the caller.  Every 10 s
 * the 16 busiest callers with calls per second. */
static void tick_caller(SwCpu *c)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("SW_TICK_CALLERS"); on = e && *e == '1'; }
    if (!on) return;
    enum { N = 4096 };
    static volatile u64 key[N]; static volatile long cnt[N];
    static u64 last; static volatile long busy;
    u64 lr = c->x[30];
    u32 h = (u32)((lr >> 2) * 2654435761u) & (N - 1);
    for (int k = 0; k < 32; k++, h = (h + 1) & (N - 1)) {
        if (key[h] == lr) { InterlockedIncrement(&cnt[h]); break; }
        if (!key[h] && !InterlockedCompareExchange64((volatile LONG64 *)&key[h], (LONG64)lr, 0)) { InterlockedIncrement(&cnt[h]); break; }
    }
    u64 now = sw_tick();
    if (!last) last = now;
    if (now - last < 19200000ull * 10 || InterlockedExchange(&busy, 1)) return;
    double secs = (double)(now - last) / 19200000.0;
    last = now;
    long snap[N]; for (int i = 0; i < N; i++) snap[i] = InterlockedExchange(&cnt[i], 0);
    for (int r = 0; r < 16; r++) {
        int best = -1;
        for (int i = 0; i < N; i++) if (snap[i] && (best < 0 || snap[i] > snap[best])) best = i;
        if (best < 0) break;
        sw_report("[tick] caller 0x%llx  %.1f/s\n", (unsigned long long)key[best], snap[best] / secs);
        snap[best] = 0;
    }
    busy = 0;
}   /* 19.2 MHz counter, for sw_ipc.c */

u64 sw_mrs(SwCpu *c, int reg)
{
    switch (reg) {
    case SW_SYS_NZCV:
        return ((u64)c->nf << 31) | ((u64)c->zf << 30) | ((u64)c->cf << 29) | ((u64)c->vf << 28);
    case SW_SYS_FPCR: return c->fpcr;
    case SW_SYS_FPSR: return c->fpsr;
    case SW_SYS_TPIDR_EL0: {
        u64 val = c->tpidr_el0;
        sw_report("debug: sw_mrs TPIDR_EL0 = 0x%llx\n", (unsigned long long)val);
        return val;
    }
    case SW_SYS_TPIDRRO_EL0: return c->tpidrro_el0;
    case SW_SYS_CNTFRQ_EL0: return 19200000ull;
    case SW_SYS_CNTVCT_EL0: tick_caller(c); return sw_tick();
    case SW_SYS_CTR_EL0: return 0x8444C004ull;
    case SW_SYS_DCZID_EL0: return 4;
    case SW_SYS_SCTLR_EL2: return 0x30C50838ull; /* reasonable default for SCTLR_EL2 */
    case SW_SYS_SWITCH_SYSREG_0_3_3_11_5: return 0; /* Nintendo Switch-specific sysreg, stub */
    default: return 0;
    }
}

/* FPCR.FZ (bit 24, flush-to-zero) on the host: x86 MXCSR FTZ (bit 15) + DAZ
 * (bit 6) give the same result - denormal inputs read as zero, denormal
 * results become zero.  The game sets FZ on its threads (main+0x2bc89e4,
 * main+0x328b994); without it denormals stayed non-zero natively, so e.g.
 * x/y with a denormal gave -0 where Arm gives 0/0 = NaN, and near-zero
 * checks took other branches than on the Switch.  MXCSR is per host thread,
 * so this is applied again whenever a guest thread starts or resumes
 * (sw_thread.c).  FPCR.RMode is not mapped: sw_rt.h's FRINTN relies on the
 * host rounding mode staying round-to-nearest. */
void sw_fpcr_apply(u64 fpcr)
{
#if defined(_M_X64) || defined(__x86_64__)
    unsigned int want = (fpcr >> 24) & 1 ? 0x8040u : 0u;
    unsigned int cur = _mm_getcsr();
    if ((cur & 0x8040u) != want) _mm_setcsr((cur & ~0x8040u) | want);
#else
    (void)fpcr;
#endif
}

void sw_msr(SwCpu *c, int reg, u64 value)
{
    switch (reg) {
    case SW_SYS_NZCV:
        c->nf = (u8)((value >> 31) & 1);
        c->zf = (u8)((value >> 30) & 1);
        c->cf = (u8)((value >> 29) & 1);
        c->vf = (u8)((value >> 28) & 1);
        break;
    case SW_SYS_FPCR: c->fpcr = value; sw_fpcr_apply(value); break;
    case SW_SYS_FPSR: c->fpsr = value; break;
    case SW_SYS_TPIDR_EL0: c->tpidr_el0 = value; break;
    case SW_SYS_TPIDRRO_EL0: c->tpidrro_el0 = value; break;
    case SW_SYS_SCTLR_EL2: /* stub: ignore write to SCTLR_EL2 */ break;
    case SW_SYS_SWITCH_SYSREG_0_3_3_11_5: /* stub: ignore write */ break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Kernel system calls (svc #0 with x8 = number)                      */
/* ------------------------------------------------------------------ */
static u64 g_symbol_table_base;  /* imported symbol pointers (rtld use) */

static int svc_ok(SwCpu *c) { (void)c; return 1; }

static void sw_svc_locked(SwCpu *c, u32 imm);
/* Syscalls run under the kernel lock (sw_thread.c); a blocking wait releases it. */
SwCpu *sw_svc(SwCpu *c, u32 imm)
{
    if (imm == 0x1A) { extern int sw_arb_lock_fast(SwCpu *c); if (sw_arb_lock_fast(c)) return c; }   /* free mutex: no kernel lock */
    sw_klock();
    sw_klock_note(0x5C000000ull | (imm ? imm : (u32)c->x[8]));
    sw_svc_locked(c, imm);
    sw_kunlock();
    return c;
}

static void sw_svc_locked(SwCpu *c, u32 imm)
{
    /* Horizon encodes the syscall number in the SVC immediate (svc #0x1b),
     * not in x8.  Keep the x8 form as a fallback for svc #0 so older test
     * fixtures continue to work. */
    u32 num = imm ? imm : (u32)c->x[8];
    g_svc_count++;
    c->svc_count++;
    if (num > 0xBF) {
        char reason[128];
        snprintf(reason, sizeof(reason), "svc #%u is outside the Horizon syscall table", num);
        sw_trap(c, 0, reason);
        return;
    }
    if (sw_thread_svc(c, num)) {                      /* sw_thread.c: threads, locks, waits */
        if (svc_ok(c) && g_call_budget) g_call_budget--;
        return;
    }
    if (sw_ipc_svc(c, num)) {                         /* sw_ipc.c: ports, sessions, IPC */
        if (svc_ok(c) && g_call_budget) g_call_budget--;
        {   /* a handler asked to sleep the caller (audout polling: wait for the next buffer) */
            s64 sw_ipc_take_sleep(void);
            void sw_thread_sleep(SwCpu *c, s64 ns);
            s64 s = sw_ipc_take_sleep();
            if (s > 0) sw_thread_sleep(c, s);
        }
        return;
    }
    switch (num) {
    case 0x01: { /* SetHeapSize */
        u64 size = c->x[1];
        static u64 heap_base = 0x80000000ull;
        static u64 heap_next = 0x80000000ull;
        static u64 heap_end = 0;
        if (heap_end == 0) {
            u64 reserve = 512ull * 1024 * 1024;
            if (sw_mem_map(c, heap_base, reserve, NULL, 0) != 0) {
                c->x[0] = 1;
                break;
            }
            heap_end = heap_base + reserve;
        }
        if (heap_next + size > heap_end) { c->x[0] = 0xE01; break; }
        c->x[0] = 0;
        c->x[1] = heap_next;
        heap_next += (size + 0xFFF) & ~0xFFFull;
        break;
    }
    case 0x05: {                                      /* UnmapMemory(dst, src, size) */
        for (int i = 0; i < g_alias_n; i++)
            if (g_alias[i].lo == c->x[0]) { g_alias[i] = g_alias[--g_alias_n]; break; }
        c->x[0] = 0;
        break;
    }
    case 0x04: {                                      /* MapMemory(dst, src, size) */
        /* nn::os maps thread stacks to an alias address and runs the thread
         * on the alias only.  64 KB guest pages cannot alias 4 KB-aligned
         * ranges, so back dst with pages and copy src into it. */
        u64 dst = c->x[0], src = c->x[1], size = c->x[2];
        /* remember it: QueryMemory must report it, or nn::os's address
         * allocator hands the same range to the next thread's alias stack */
        if (g_alias_n < (int)(sizeof g_alias / sizeof g_alias[0])) {
            g_alias[g_alias_n].lo = dst;
            g_alias[g_alias_n].hi = dst + size;
            g_alias_n++;
        }
        u64 a = dst & ~(u64)(SW_PAGE_SIZE - 1);
        for (; a < dst + size; a += SW_PAGE_SIZE)
            if (!mem_host(c, a, 1, 0)) sw_mem_map(c, a, SW_PAGE_SIZE, NULL, 0);
        for (u64 i = 0; i < size; i++) {
            u8 *sp_ = mem_host(c, src + i, 1, 0), *dp = mem_host(c, dst + i, 1, 1);
            if (dp) *dp = sp_ ? *sp_ : 0;
        }
        c->x[0] = 0;
        break;
    }
    case 0x02: case 0x03:
        /* memory permission/attribute/unmap: no observable effect here */
        c->x[0] = 0;
        break;
    case 0x06: { /* QueryMemory(MemoryInfo *out=x0, -, addr=x2) -> w0 result, w1 page flags */
        /* Describe the region containing addr from this runtime's own map.
         * It used to always fail, and nn::os's ThreadManager (which queries
         * its own stack to learn the main thread's stack bounds) aborted. */
        typedef struct { u64 lo, hi; u32 type, perm; } Region;
        Region r[96];
        int n = 0;
        u64 addr = c->x[2];
        for (u32 mi = 0; mi < sw_module_count && n < 56; mi++) {
            const SwModuleDesc *m = &sw_modules[mi];
            if (!m->image_size) continue;
            u64 t_end = (m->text_size + 0xFFF) & ~0xFFFull;
            u64 r_end = (t_end + m->ro_size + 0xFFF) & ~0xFFFull;
            if (m->text_size) r[n++] = (Region){m->base, m->base + t_end, 3, 5};       /* code static, r-x */
            if (r_end > t_end) r[n++] = (Region){m->base + t_end, m->base + r_end, 3, 1}; /* r-- */
            if (m->image_size > r_end)
                r[n++] = (Region){m->base + r_end, m->base + m->image_size, 4, 3};      /* code mutable, rw- */
        }
        for (int ai = 0; ai < g_alias_n && n < 60; ai++)                                 /* alias stacks */
            r[n++] = (Region){g_alias[ai].lo, g_alias[ai].hi, 0xB, 3};
        r[n++] = (Region){0x70000000ull, 0x70200000ull, 0xB, 3};                        /* stack */
        r[n++] = (Region){SW_TLS_BASE, SW_TLS_BASE + SW_TLS_SIZE, 0xC, 3};              /* thread local */
        r[n++] = (Region){SW_THREAD_BASE, SW_THREAD_BASE + SW_THREAD_SIZE, 0xC, 3};
        r[n++] = (Region){SW_ELF_TLS_BASE, SW_ELF_TLS_BASE + SW_ELF_TLS_SIZE, 0xC, 3};
        r[n++] = (Region){0x80000000ull, 0x80000000ull + 512ull * 1024 * 1024, 5, 3};   /* heap */
        u64 lo = 0, hi = 0x8000000000ull;   /* 39-bit address space */
        u32 type = 0, perm = 0;
        for (int i = 0; i < n; i++) {
            if (addr >= r[i].lo && addr < r[i].hi) {
                lo = r[i].lo; hi = r[i].hi; type = r[i].type; perm = r[i].perm;
                break;
            }
            if (r[i].hi <= addr && r[i].hi > lo) lo = r[i].hi;     /* unmapped gap bounds */
            if (r[i].lo > addr && r[i].lo < hi) hi = r[i].lo;
        }
        u8 *out = sw_mem_ptr(c, c->x[0], 0x28, 1);
        if (out) {
            u64 size = hi - lo;
            u32 zero = 0;
            memcpy(out + 0x00, &lo, 8);
            memcpy(out + 0x08, &size, 8);
            memcpy(out + 0x10, &type, 4);
            memcpy(out + 0x14, &zero, 4);   /* attribute */
            memcpy(out + 0x18, &perm, 4);
            memcpy(out + 0x1C, &zero, 4);   /* ipc refcount */
            memcpy(out + 0x20, &zero, 4);   /* device refcount */
            memcpy(out + 0x24, &zero, 4);
        }
        c->x[0] = out ? 0 : 0xCC01;         /* 0xCC01: InvalidPointer */
        c->x[1] = 0;
        break;
    }
    case 0x07: /* ExitProcess */
        g_exit_code = (int)c->x[0];
        c->halted = 1;
        c->halt_reason = "svcExitProcess";
        break;
    case 0x08: case 0x09: case 0x0A: /* thread create/start/exit */
        c->x[0] = 0;
        break;
    case 0x0B: /* SleepThread */
        c->x[0] = 0;
        break;
    case 0x0C: c->x[0] = 0; break;                    /* GetThreadPriority */
    case 0x0D: c->x[0] = 0; break;                    /* SetThreadPriority */
    case 0x0E: c->x[0] = 0; c->x[1] = 3; break;       /* GetThreadCoreMask */
    case 0x0F: sw_thread_set_core((u32)c->x[0], (int)(s32)c->x[1]); c->x[0] = 0; break;   /* SetThreadCoreMask */
    case 0x10: c->x[0] = (u64)sw_thread_core(); break;   /* GetCurrentProcessorNumber: the emulated core */
    case 0x11: case 0x12: c->x[0] = 0; break;         /* signal / clear event */
    case 0x17:                                        /* ResetSignal: 0 = was signalled */
        c->x[0] = sw_ipc_event_never((u32)c->x[0]) ? 0xFA01 /* invalid state */ : 0;
        break;
    case 0x13: {                                      /* MapSharedMemory(handle, addr, size, perm) */
        /* back the range with zeroed pages (hid / time / font shared memory) */
        u64 a = c->x[1] & ~(u64)(SW_PAGE_SIZE - 1);
        u64 end = c->x[1] + c->x[2];
        for (; a < end; a += SW_PAGE_SIZE)
            if (!mem_host(c, a, 1, 0) && sw_mem_map(c, a, SW_PAGE_SIZE, NULL, 0) != 0) break;
        sw_ipc_shmem_map(c, (u32)c->x[0], c->x[1], c->x[2]);   /* e.g. shared fonts */
        c->x[0] = 0;
        break;
    }
    case 0x14: case 0x15: c->x[0] = 0; break;         /* unmap shared / transfer mem */
    case 0x16: c->x[0] = 0; break;                    /* CloseHandle */
    case 0x18: c->x[0] = 0; break;                    /* WaitSynchronization */
    case 0x19: c->x[0] = 0; break;                    /* CancelSynchronization */
    /* Kernel futex primitives used by nn::os critical sections and libc++.
     * Everything here is cooperative and single-threaded: an uncontended
     * ArbitrateLock/Unlock updates the 32-bit owner word exactly like the
     * kernel, while a would-block wait is released immediately instead of
     * stopping the whole recompiled program.  A real scheduler will replace
     * the wait/signal pair; the call sites stay identical. */
    case 0x1A: { /* ArbitrateLock(owner_handle, mutex_addr, requester_tag) */
        /* The kernel sleeps until the owner releases the mutex, then makes
         * the requester the owner.  With a single guest thread no other
         * owner can ever run to release it, so complete the hand-over now.
         * (Granting only when the word was 0 or already ours left a word
         * with the waiters bit set untouched, and nn::os's Enter loop then
         * spun forever in a constructor.)  Revisit once real threads exist. */
        u64 addr = c->x[1];
        u32 tag = (u32)c->x[2];
        u32 cur = 0;
        memcpy(&cur, mem_host(c, addr, 4, 0), 4);
        if ((cur & ~0x40000000u) != tag && cur != 0)
            sw_report("debug: ArbitrateLock 0x%llx held by 0x%x, granting to 0x%x\n",
                      (unsigned long long)addr, cur, tag);
        memcpy(mem_host(c, addr, 4, 1), &tag, 4);
        c->x[0] = 0;
        break;
    }
    case 0x1B: { /* ArbitrateUnlock(mutex_addr) */
        u64 addr = c->x[0];
        u32 zero = 0;
        memcpy(mem_host(c, addr, 4, 1), &zero, 4);
        c->x[0] = 0;
        break;
    }
    case 0x1C: { /* WaitProcessWideKeyAtomic(addr, cv_addr, tag, timeout) */
        u64 addr = c->x[0];
        u32 tag = (u32)c->x[2];
        u32 cur = 0;
        u32 zero = 0;
        memcpy(&cur, mem_host(c, addr, 4, 0), 4);
        /* The kernel atomically releases the mutex before sleeping.  No
         * peer thread can signal yet, so do not block the process. */
        if (cur == tag) memcpy(mem_host(c, addr, 4, 1), &zero, 4);
        c->x[0] = 0;
        g_unimplemented_count++;
        break;
    }
    case 0x1D: c->x[0] = 0; break;                    /* SignalProcessWideKey */
    case 0x1E: c->x[0] = sw_tick(); break;            /* GetSystemTick */
    case 0x1F:                                        /* ConnectToNamedPort */
        c->x[0] = 0xE01;
        sw_report("[svc] ConnectToNamedPort(0x%llx) not implemented\n",
                  (unsigned long long)c->x[1]);
        g_unimplemented_count++;
        break;
    case 0x21:                                        /* SendSyncRequest */
        c->x[0] = 0;
        sw_report("[svc] SendSyncRequest(0x%x) -> stub success (service layer not implemented)\n",
                  (unsigned)c->x[0]);
        g_unimplemented_count++;
        break;
    case 0x24: c->x[0] = 0; c->x[1] = 1; break;        /* GetProcessId */
    case 0x25: c->x[0] = 0;                            /* GetThreadId */
               c->x[1] = c->thread_id ? c->thread_id : 1; break;
    case 0x26:                                        /* Break */
        sw_report("[svc] Break called from guest\n");
        c->halted = 1;
        c->halt_reason = "svcBreak";
        break;
    case 0x27: {                                      /* OutputDebugString */
        u64 str = c->x[0], len = c->x[1];
        if (len > 256) len = 256;
        if (str && len) {
            const char *p = (const char *)mem_host(c, str, len, 0);
            sw_report("[guest] %.*s", (int)len, p);
        }
        c->x[0] = 0;
        break;
    }
    case 0x2C: case 0x2D: {                           /* Map/UnmapPhysicalMemory(addr, size) */
        /* nn::os virtual address memory (StandardAllocator reserves 32 GB
         * and commits pieces on demand) backs reserved ranges with these.
         * Map every not-yet-mapped page; unmapping keeps the memory. */
        if (num == 0x2C) {
            u64 a = c->x[0] & ~(u64)(SW_PAGE_SIZE - 1);
            u64 end = c->x[0] + c->x[1];
            static int maplog = -1;       /* SW_MAP_LOG=1: calls over 5 ms (size, new bytes, time) */
            if (maplog < 0) { const char *e = getenv("SW_MAP_LOG"); maplog = e && *e == '1'; }
            u64 t0 = maplog ? sw_now_ns() : 0, before = c->mem->mapped_bytes;
#if defined(_WIN32)
            /* SW_MAP_UNLOCKED=1 (experiment, off): commit the range first, outside the kernel lock.  While the world
             * loads, a 256 KB commit takes 10-18 ms (the OS address-space lock is busy), which under the kernel lock
             * stalls every guest thread.  The first test looked much worse, but it ran while another program's
             * build loaded the PC: to be measured again on a quiet PC before it can become the default. */
            static int unlocked = -1;
            if (unlocked < 0) { const char *e = getenv("SW_MAP_UNLOCKED"); unlocked = e && *e == '1'; }
            if (unlocked) {
                extern int sw_klock_leave(void); extern void sw_klock_return(int d);
                u64 lo = a, hi = (end + SW_PAGE_SIZE - 1) & ~(u64)(SW_PAGE_SIZE - 1);
                if (hi > lo && hi <= SW_GUEST_SPAN) {
                    int d = sw_klock_leave();
                    VirtualAlloc((void *)(uintptr_t)lo, (SIZE_T)(hi - lo), MEM_COMMIT, PAGE_READWRITE);
                    sw_klock_return(d);
                }
            }
#endif
            while (a < end) {             /* each run of not-yet-mapped pages in one sw_mem_map (one commit) */
                if (mem_host(c, a, 1, 0)) { a += SW_PAGE_SIZE; continue; }
                u64 r = a + SW_PAGE_SIZE;
                while (r < end && !mem_host(c, r, 1, 0)) r += SW_PAGE_SIZE;
                if (sw_mem_map(c, a, r - a, NULL, 0) != 0) break;
                a = r;
            }
            if (maplog) {
                u64 ns = sw_now_ns() - t0;
                if (ns > 5000000)
                    sw_report("[map] MapPhysicalMemory 0x%llx size 0x%llx: %llu KB new, %.1f ms\n",
                              (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                              (unsigned long long)((c->mem->mapped_bytes - before) >> 10), ns / 1e6);
            }
        }
        c->x[0] = 0;
        break;
    }
    case 0x28:                                        /* ReturnFromException */
        c->x[0] = 0;
        break;
    case 0x29: {                                      /* GetInfo(id0=w1, handle=w2, id1=x3) -> x1 */
        /* GetInfo is 0x29 on Horizon (it was implemented as 0x28, so every
         * call fell through as unimplemented and nnosInitialize aborted).
         * The layout matches what this runtime maps: heap from 0x80000000,
         * stack + TLS around 0x70000000, modules from 0x100000. */
        u32 id0 = (u32)c->x[1];
        u64 id1 = c->x[3];
        u64 v = 0;
        int ok = 1;
        switch (id0) {
        case 0: {                                         /* allowed CPU cores 0-2 */
            /* SW_CORE_MASK=0x3f (test): report more cores, for a game that sizes its job system from the mask */
            static u64 mask;
            if (!mask) { const char *e = getenv("SW_CORE_MASK"); mask = e && *e ? strtoull(e, NULL, 0) : 0x7; if (!mask) mask = 0x7; }
            v = mask;
            break;
        }
        case 1:  v = 0xFFFFFFFFFF000000ull; break;        /* allowed priorities 24..63 */
        case 2:  v = 0x1000000000ull; break;              /* alias region */
        case 3:  v = 0x1000000000ull; break;
        case 4:  v = 0x80000000ull; break;                /* heap region */
        case 5:  v = 0x180000000ull; break;
        case 6:  case 21: v = 0xCD500000ull; break;       /* total memory */
        case 7:  case 22: v = 0x08000000ull; break;       /* used memory */
        case 8:  v = 0; break;                            /* debugger attached */
        case 9:  v = 0; break;                            /* resource limit handle */
        case 10: v = 0; break;                            /* idle ticks */
        case 11: v = ((u64)rand() << 48) ^ ((u64)rand() << 32) ^ ((u64)rand() << 16)
                     ^ (u64)rand() ^ sw_tick() ^ (id1 * 0x9E3779B97F4A7C15ull); break;
        case 12: v = 0x8000000ull; break;                 /* ASLR region (39-bit) */
        case 13: v = 0x7FF8000000ull; break;
        case 14: v = 0x70000000ull; break;                /* stack region */
        case 15: v = 0x04000000ull; break;
        case 16: v = 0x2000000ull; break;                 /* system resource size: non-zero */
        case 17: v = 0; break;                            /* enables virtual address memory */
        case 18: v = 0x01001F5010DFA000ull; break;        /* program id */
        case 20: v = 0; break;                            /* user exception context */
        case 23: v = 1; break;                            /* is application */
        case 24: v = 64; break;                           /* free thread count */
        default:
            if (id0 == 0xF0000002u) { v = sw_tick(); break; }   /* thread tick count */
            ok = 0;
        }
        if (!ok)
            sw_report("[svc] GetInfo: unknown info type 0x%x (sub 0x%llx)\n", id0,
                      (unsigned long long)id1);
        c->x[0] = ok ? 0 : 0xF001;  /* 0xF001: InvalidEnumValue */
        c->x[1] = v;
        break;
    }
    default:
        sw_report("[svc] unimplemented syscall 0x%x (x0=0x%llx x1=0x%llx)\n", num,
                  (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
        g_unimplemented_count++;
        c->x[0] = 0;
        break;
    }
    if (svc_ok(c) && g_call_budget) g_call_budget--;
}

/* ------------------------------------------------------------------ */
/* Image loading and audit                                            */
/* ------------------------------------------------------------------ */

/* ---- host interface for the mod loader (sw_mods.c) ---- */
int sw_mods_host_module_count(void) { return (int)sw_module_count; }
int sw_mods_host_module(int i, SwModuleInfo *o)
{
    if (i < 0 || (u32)i >= sw_module_count) return 0;
    const SwModuleDesc *m = &sw_modules[i];
    o->name = m->name;
    o->base = m->base;
    o->text_size = m->text_size;
    o->ro_size = m->ro_size;
    o->data_size = m->data_size;
    o->bss_size = m->bss_size;
    o->module_id = m->module_id;
    return 1;
}
const char *sw_mods_host_function_at(u64 addr, u64 *start)
{
    u32 lo = 0, hi = sw_function_count;                  /* last function starting at or before addr */
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (sw_functions[mid].addr <= addr) lo = mid + 1; else hi = mid;
    }
    if (!lo) return NULL;
    const SwFuncEntry *f = &sw_functions[lo - 1];
    for (u32 i = 0; i < sw_module_count; i++) {          /* both in the same module's .text */
        const SwModuleDesc *m = &sw_modules[i];
        if (f->addr >= m->base && f->addr < m->base + m->text_size && addr < m->base + m->text_size) {
            if (start) *start = f->addr;
            return f->name;
        }
    }
    return NULL;
}
/* Cheats and exefs patches touch guest memory through these: never a host fault (unmapped pages are refused, and
 * Read/WriteProcessMemory report an error instead of raising one). */
static int guest_range_mapped(u64 a, u64 n)
{
#if defined(_WIN32)
    if (!n || a >= SW_GUEST_SPAN || n > SW_GUEST_SPAN - a) return 0;
    for (u64 pg = a >> SW_PAGE_BITS; pg <= (a + n - 1) >> SW_PAGE_BITS; pg++)
        if (!g_pmap[pg]) return 0;
    return 1;
#else
    (void)a; (void)n;
    return 0;
#endif
}
int sw_guest_read(u64 a, void *buf, u64 n)
{
#if defined(_WIN32)
    SIZE_T got = 0;
    return guest_range_mapped(a, n) && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, buf, (SIZE_T)n, &got) &&
           got == n;
#else
    (void)a; (void)buf; (void)n;
    return 0;
#endif
}
int sw_guest_write(u64 a, const void *buf, u64 n)
{
#if defined(_WIN32)
    SIZE_T put = 0;
    return guest_range_mapped(a, n) && WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)a, buf, (SIZE_T)n, &put) &&
           put == n;
#else
    (void)a; (void)buf; (void)n;
    return 0;
#endif
}
/* the region bases svcGetInfo reports (case 0x29 below): main NSO, heap, alias, aslr */
u64 sw_guest_region_base(int kind)
{
    switch (kind) {
    case 0: return sw_module_count ? sw_modules[0].base : 0;
    case 1: return 0x80000000ull;
    case 2: return 0x1000000000ull;
    case 3: return 0x8000000ull;
    }
    return 0;
}
const char *sw_mods_host_exe_dir(void) { return g_base_dir; }

static void set_base_dir(const char *argv0)
{
    const char *env = getenv("SW_MODULE_DIR");
    const char *slash, *bslash;
    size_t n;
    if (env && *env) {
        snprintf(g_base_dir, sizeof(g_base_dir), "%s", env);
        return;
    }
    if (!argv0 || !*argv0) return;
    slash = strrchr(argv0, '/');
    bslash = strrchr(argv0, '\\');
    if (bslash > slash) slash = bslash;
    if (!slash) return;
    n = (size_t)(slash - argv0);
    if (n == 0 || n >= sizeof(g_base_dir)) return;
    memcpy(g_base_dir, argv0, n);
    g_base_dir[n] = 0;
}

static int load_image(const char *path, u8 **out, u64 *out_size)
{
    char full[2048];
    FILE *f;
    long size;
    u8 *buf;
    if (path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':')) {
        snprintf(full, sizeof(full), "%s", path);
    } else {
        snprintf(full, sizeof(full), "%s/%s", g_base_dir, path);
    }
    f = fopen(full, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return -1; }
    buf = (u8 *)malloc((size_t)size);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) { free(buf); fclose(f); return -1; }
    fclose(f);
    *out = buf;
    *out_size = (u64)size;
    return 0;
}

/* Horizon exposes the current thread's control block through
 * TPIDRRO_EL0/TPIDR_EL0.  Recompiled code dereferences it directly, so it
 * must be guest memory: a zero thread pointer turns the very first
 * ``mrs x8, tpidrro_el0; ldr xN, [x8, #off]`` into a host fault.  The SDK
 * uses +0x100/+0x102 for 16-bit lock counters and +0x1f8 for the thread id;
 * libc TLS variables sit at positive offsets from the same pointer. */
#define SW_TLS_BASE 0x71000000ull
#define SW_TLS_SIZE 0x10000ull
#define SW_TLS_LOCK_COUNT 0x100ull
#define SW_TLS_THREAD_PTR 0x1f8ull
/* TPIDRRO_EL0+0x1f8 points at the current nn::os::ThreadType, not at an
 * integer.  Its id lives at +0x1b0 and mutex owners store this pointer. */
#define SW_THREAD_BASE 0x71100000ull
#define SW_THREAD_SIZE 0x10000ull
#define SW_THREAD_ID 0x1b0ull
/* TPIDR_EL0 is the libc (ELF) thread pointer: local-exec TLS variables sit at
 * fixed offsets from it.  This game reaches tp+0x38d488 (3.5 MB) during
 * nninitStartup, so it gets its own zeroed block instead of sharing the
 * 64 KB ThreadType page that TPIDRRO_EL0 points at. */
#define SW_ELF_TLS_BASE 0x72000000ull
#define SW_ELF_TLS_SIZE 0x800000ull

static int setup_thread_pointer(SwCpu *c)
{
    u64 tid = c->thread_id ? c->thread_id : 1;
    if (sw_mem_map(c, SW_TLS_BASE, SW_TLS_SIZE, NULL, 0) != 0) return -1;
    if (sw_mem_map(c, SW_THREAD_BASE, SW_THREAD_SIZE, NULL, 0) != 0) return -1;
    if (sw_mem_map(c, SW_ELF_TLS_BASE, SW_ELF_TLS_SIZE, NULL, 0) != 0) return -1;
    /* TPIDRRO_EL0 -> thread control area (ThreadType pointer at +0x1f8);
     * TPIDR_EL0  -> libc TLS block (local-exec TLS offsets). */
    c->tpidr_el0 = SW_ELF_TLS_BASE;
    c->tpidrro_el0 = SW_THREAD_BASE;
    sw_report("debug: setup_thread_pointer: tpidr_el0 = 0x%llx\n", (unsigned long long)c->tpidr_el0);
    /* ThreadType.id is a 32-bit id; recursive mutexes compare the ThreadType
     * pointer, so both a stable pointer and a non-zero id are required.
     * The ThreadType self-pointer lives at +0x1f8 (read by TPIDRRO_EL0+0x1f8). */
    sw_store32(c, SW_THREAD_BASE + SW_THREAD_ID, (u32)tid);
    sw_store64(c, SW_THREAD_BASE + SW_TLS_THREAD_PTR, SW_THREAD_BASE);
    /* TLS+0x1f0 holds the thread's ELF TLS pointer: nn::os's ThreadManager
     * does `msr tpidr_el0, [tpidrro_el0 + 0x1f0]`.  It was 0, so after nn::os
     * init every thread_local resolved against address 0 (inside main's code)
     * and the game's deferred-free counter read code bytes -> abort(). */
    sw_store64(c, SW_THREAD_BASE + 0x1f0, SW_ELF_TLS_BASE);
    sw_store64(c, SW_TLS_BASE + SW_TLS_THREAD_PTR, SW_THREAD_BASE);
    (void)SW_TLS_LOCK_COUNT;
    return 0;
}

static void dump_pointer(SwCpu *c, const char *label, u64 addr)
{
    u64 *p = (u64 *)mem_host(c, addr, 8, 0);
    if (p) {
        sw_report("  PTR %s @ 0x%llx = 0x%llx\n", label, (unsigned long long)addr, (unsigned long long)*p);
    } else {
        sw_report("  PTR %s @ 0x%llx = UNAVAILABLE\n", label, (unsigned long long)addr);
    }
}

/* SW_PEEK=0x4335f40,0x4363930,... prints the 64-bit guest values at those
 * addresses after loading and again when execution stops. */
static void peek_env(SwCpu *c, const char *stage)
{
    const char *env = getenv("SW_PEEK");
    char *end;
    if (!env || !*env) return;
    fprintf(stderr, "=== SW_PEEK (%s) ===\n", stage);
    while (*env) {
        u64 addr = strtoull(env, &end, 0);
        if (end == env) break;
        u64 *p = (u64 *)mem_host(c, addr, 8, 0);
        if (p) fprintf(stderr, "  [0x%llx] = 0x%llx\n", (unsigned long long)addr, (unsigned long long)*p);
        else   fprintf(stderr, "  [0x%llx] = (unmapped)\n", (unsigned long long)addr);
        env = (*end == ',') ? end + 1 : end;
    }
}

/* SW_WATCHDOG=N: if no guest function is entered for N seconds the guest is
 * spinning inside one function (lock, wait loop).  Report where and exit,
 * instead of hanging silently.  Reads the CPU racily - diagnostics only. */
#if defined(_WIN32)
static SwCpu *g_watch_cpu;
static DWORD WINAPI watchdog_thread(LPVOID arg)
{
    unsigned secs = (unsigned)(uintptr_t)arg, idle = 0;
    u64 last = (u64)-1;
    for (;;) {
        Sleep(1000);
        u64 now = g_dispatch_count;
        idle = (now == last) ? idle + 1 : 0;
        last = now;
        if (idle >= secs) {
            SwCpu *c = g_watch_cpu;
            fprintf(stderr, "\n=== WATCHDOG: no function entered for %u s (dispatches=%llu) ===\n",
                    secs, (unsigned long long)now);
            fprintf(stderr, "  cur_fn=0x%llx last memory op pc=0x%llx svc_count=%llu\n",
                    (unsigned long long)c->cur_fn, (unsigned long long)c->pc,
                    (unsigned long long)c->svc_count);
            for (int i = 0; i < 31; i += 4)
                fprintf(stderr, "  x%-2d=0x%llx x%-2d=0x%llx x%-2d=0x%llx x%-2d=0x%llx\n",
                        i, (unsigned long long)c->x[i], i + 1, (unsigned long long)c->x[i + 1],
                        i + 2, (unsigned long long)c->x[i + 2],
                        i + 3, (unsigned long long)(i + 3 < 31 ? c->x[i + 3] : c->sp));
            peek_env(c, "watchdog");
            printf("=== execution stopped ===\nreason   : watchdog: guest spinning in 0x%llx (pc 0x%llx)\n",
                   (unsigned long long)c->cur_fn, (unsigned long long)c->pc);
            fflush(stdout);
            fflush(stderr);
            ExitProcess(4);
        }
    }
    return 0;
}

/* SW_PROFILE=1: sample the main host thread (all guest fibers run on it)
 * about 1000x per second and print the hottest exe RVAs every 20 s:
 *   [prof] <rva> <samples> <percent>   (symbolize with sw_app.map)
 * Samples outside the exe (driver, ntdll waits, other DLLs) are counted as
 * "outside".  Cheap: one SuspendThread/GetThreadContext per sample. */
#define PROF_SLOTS (1u << 17)
static uint64_t g_prof_key[PROF_SLOTS];
static uint32_t g_prof_cnt[PROF_SLOTS];
static int g_prof_stack;                     /* SW_PROFILE_STACK=1 */
static HMODULE g_prof_mod[64];
static uint64_t g_prof_modcnt[65];           /* [64]: no module (JIT code, stack) */
static int g_prof_nmod;
static int g_prof_thr = -1;
static double g_prof_win_from, g_prof_win_to;
static ULONGLONG g_prof_t0;
/* the profile window from now on for secs seconds (sw_hid.c: SW_PROFILE_ROUTE=<s>, the start of a route replay) */
void sw_prof_window_now(double secs)
{
    double t = (double)(GetTickCount64() - g_prof_t0) / 1000.0;
    g_prof_win_from = t, g_prof_win_to = t + secs;
}

/* SW_PROFILE_OUT=<file>: the whole RVA histogram ("rva count" lines) and the
 * per-DLL split of samples outside the exe, rewritten every 20 s */
static void prof_dump_full(uint64_t total, uint64_t outside)
{
    const char *path = getenv("SW_PROFILE_OUT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# total %llu outside %llu\n", (unsigned long long)total, (unsigned long long)outside);
    for (int k = 0; k < g_prof_nmod; k++) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(g_prof_mod[k], name, sizeof name);
        const char *b = strrchr(name, '\\');
        fprintf(f, "module %llu %s\n", (unsigned long long)g_prof_modcnt[k], b ? b + 1 : name);
    }
    fprintf(f, "module %llu (none)\n", (unsigned long long)g_prof_modcnt[64]);
    for (uint32_t i = 0; i < PROF_SLOTS; i++)
        if (g_prof_cnt[i]) fprintf(f, "%llx %u\n", (unsigned long long)(g_prof_key[i] & ~(1ull << 63)), g_prof_cnt[i]);
    fclose(f);
}

/* SW_PROFILE=all: every thread of the process (multi-core guest threads, Vulkan replay, present, shader
 * workers), every 2 ms; a thread is sampled only when its cycle counter moved since the last tick (it ran),
 * so the counts are CPU time, not wall time.  The global histogram is the same as SW_PROFILE=1 (hot chunks);
 * per thread: samples, share in the exe, and its top RVAs -> SW_PROFILE_OUT.threads, every 20 s. */
#include <tlhelp32.h>
#define PA_MAX 96
#define PA_SLOTS (1u << 19)
typedef struct { DWORD tid; HANDLE h; ULONG64 cyc, run; uint64_t n, inexe; char name[40]; } PaThr;
static double g_pa_tsc_ms;   /* cycle-time units (TSC ticks) per ms */
static ULONG64 g_pa_win_cyc;  /* TSC ticks inside the sampling window */
static PaThr g_pa[PA_MAX];
static int g_npa;
static uint64_t g_pa_key[PA_SLOTS];
static uint32_t g_pa_cnt[PA_SLOTS];
static void pa_refresh(DWORD self)
{
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (sn == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(sn, &te); ok; ok = Thread32Next(sn, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        int k;
        for (k = 0; k < g_npa && g_pa[k].tid != te.th32ThreadID; k++) {}
        if (k < g_npa) {   /* a known id: the name may have been set after we first saw it */
            if (g_pa[k].name[0] == '#') {
                PWSTR w = NULL;
                if (SUCCEEDED(GetThreadDescription(g_pa[k].h, &w)) && w && *w)
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, g_pa[k].name, sizeof g_pa[k].name, NULL, NULL);
                if (w) LocalFree(w);
            }
            continue;
        }
        if (g_npa >= PA_MAX) break;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        PaThr *p = &g_pa[g_npa++];
        memset(p, 0, sizeof *p);
        p->tid = te.th32ThreadID; p->h = h;
        QueryThreadCycleTime(h, &p->cyc);
        snprintf(p->name, sizeof p->name, "#%lu", (unsigned long)p->tid);
        PWSTR w = NULL;
        if (SUCCEEDED(GetThreadDescription(h, &w)) && w && *w)
            WideCharToMultiByte(CP_UTF8, 0, w, -1, p->name, sizeof p->name, NULL, NULL);
        if (w) LocalFree(w);
    }
    CloseHandle(sn);
}
static void pa_dump(uint64_t ticks)
{
    const char *path = getenv("SW_PROFILE_OUT");
    char fn[MAX_PATH];
    FILE *f = NULL;
    if (path && *path) { snprintf(fn, sizeof fn, "%s.threads", path); f = fopen(fn, "w"); }
    double win = g_pa_win_cyc ? (double)g_pa_win_cyc : 1.0;
    fprintf(stderr, "[prof] per thread (%llu ticks of 2 ms; %% = cycle time as a share of one core):\n", (unsigned long long)ticks);
    for (int k = 0; k < g_npa; k++) {
        if (!g_pa[k].run) continue;
        fprintf(stderr, "[prof]   %-24s %6.1f%% core, %llu samples, %5.1f%% of them in exe\n", g_pa[k].name,
                100.0 * g_pa[k].run / win, (unsigned long long)g_pa[k].n,
                g_pa[k].n ? 100.0 * g_pa[k].inexe / g_pa[k].n : 0.0);
        if (f) fprintf(f, "thread %d %s %.2f %llu %llu\n", k, g_pa[k].name, 100.0 * g_pa[k].run / win,
                       (unsigned long long)g_pa[k].n, (unsigned long long)g_pa[k].inexe);
    }
    if (f) {
        {   /* ntdll exports (offset name): names the bit-38 samples */
            uint8_t *nt = (uint8_t *)GetModuleHandleA("ntdll.dll");
            IMAGE_NT_HEADERS *h = (IMAGE_NT_HEADERS *)(nt + ((IMAGE_DOS_HEADER *)nt)->e_lfanew);
            IMAGE_DATA_DIRECTORY dd = h->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (dd.VirtualAddress) {
                IMAGE_EXPORT_DIRECTORY *ed = (IMAGE_EXPORT_DIRECTORY *)(nt + dd.VirtualAddress);
                DWORD *names = (DWORD *)(nt + ed->AddressOfNames), *funcs = (DWORD *)(nt + ed->AddressOfFunctions);
                WORD *ords = (WORD *)(nt + ed->AddressOfNameOrdinals);
                for (DWORD i = 0; i < ed->NumberOfNames; i++)
                    fprintf(f, "ntexp %lx %s\n", (unsigned long)funcs[ords[i]], (const char *)(nt + names[i]));
            }
        }
        fprintf(f, "ticks %llu\n", (unsigned long long)ticks);
        for (uint32_t i = 0; i < PA_SLOTS; i++)
            if (g_pa_cnt[i]) fprintf(f, "t%llu %llx %u\n", (unsigned long long)(g_pa_key[i] >> 40),
                                     (unsigned long long)(g_pa_key[i] & ((1ull << 40) - 1)), g_pa_cnt[i]);
        fclose(f);
    }
}
static void prof_count(uint64_t key);
static DWORD WINAPI profile_all_thread(LPVOID arg)
{
    (void)arg;
    HMODULE exe = GetModuleHandleA(NULL);
    uint64_t lo = (uint64_t)(uintptr_t)exe, hi;
    {   IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)exe;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)exe + dos->e_lfanew);
        hi = lo + nt->OptionalHeader.SizeOfImage; }
    DWORD self = GetCurrentThreadId();
    uint64_t ticks = 0, total = 0, outside = 0;
    ULONGLONG last_dump = GetTickCount64(), last_ref = 0;
    {   LARGE_INTEGER f0, q0, q1; QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&q0);
        unsigned long long t0 = __rdtsc(); Sleep(50); unsigned long long t1 = __rdtsc(); QueryPerformanceCounter(&q1);
        g_pa_tsc_ms = (double)(t1 - t0) / ((double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)f0.QuadPart); }
    /* Suspending a thread runs a little kernel code on it, so its cycle counter moves a bit after each sample:
     * only a thread that used more than ~4% of the 2 ms tick since the last one is sampled (it really ran). */
    ULONG64 min_run = (ULONG64)(g_pa_tsc_ms * 0.08);
    unsigned long long tsc_last = __rdtsc();
    for (;;) {
        Sleep(2);
        ULONGLONG now = GetTickCount64();
        unsigned long long tsc_now = __rdtsc(), tsc_d = tsc_now - tsc_last; tsc_last = tsc_now;
        if (now - last_ref >= 500) { pa_refresh(self); last_ref = now; }
        if (g_prof_win_to > 0) {
            double t = (double)(now - g_prof_t0) / 1000.0;
            if (t < g_prof_win_from || t > g_prof_win_to) { for (int k = 0; k < g_npa; k++) QueryThreadCycleTime(g_pa[k].h, &g_pa[k].cyc); continue; }
        }
        ticks++;
        g_pa_win_cyc += tsc_d;
        for (int k = 0; k < g_npa; k++) {
            PaThr *p = &g_pa[k];
            ULONG64 cyc = 0;
            if (!QueryThreadCycleTime(p->h, &cyc)) continue;
            ULONG64 d = cyc - p->cyc;
            p->cyc = cyc;
            if (d < g_pa_tsc_ms * 100) p->run += d;            /* (a first delta can be the time since thread start) */
            if (d < min_run) continue;                         /* did not really run: not CPU time */
            if (SuspendThread(p->h) == (DWORD)-1) continue;
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_FULL;
            BOOL ok = GetThreadContext(p->h, &ctx);
            uint64_t caller = 0;
            if (ok && (ctx.Rip < lo || ctx.Rip >= hi)) {
                /* outside the exe: the first exe return address up the stack (which runtime code called out) */
                CONTEXT u = ctx;
                for (int dd = 0; dd < 48; dd++) {
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(u.Rip, &base, NULL);
                    if (!fe) { if (!u.Rsp) break; u.Rip = *(DWORD64 *)(uintptr_t)u.Rsp; u.Rsp += 8; }
                    else { PVOID hd = NULL; DWORD64 ef = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, u.Rip, fe, &u, &hd, &ef, NULL); }
                    if (!u.Rip) break;
                    if (u.Rip >= lo && u.Rip < hi) { caller = u.Rip - lo; break; }
                }
            }
            ResumeThread(p->h);
            QueryThreadCycleTime(p->h, &p->cyc);               /* the suspend's own cost is not the thread's work */
            if (!ok) continue;
            p->n++; total++;
            uint64_t rip = ctx.Rip;
            if (caller || (rip < lo || rip >= hi)) {
                /* per thread: bit 39 = "called out to a DLL from here" (caller rva), bit 38 = ntdll offset */
                uint64_t keys[2]; int nk = 0;
                if (caller) keys[nk++] = ((uint64_t)(k + 1) << 40) | (1ull << 39) | caller;
                static uint64_t ntb, nte;
                if (!ntb) { HMODULE nt = GetModuleHandleA("ntdll.dll"); IMAGE_NT_HEADERS *h = (IMAGE_NT_HEADERS *)((uint8_t *)nt + ((IMAGE_DOS_HEADER *)nt)->e_lfanew);
                            ntb = (uint64_t)(uintptr_t)nt; nte = ntb + h->OptionalHeader.SizeOfImage; }
                if (rip >= ntb && rip < nte) keys[nk++] = ((uint64_t)(k + 1) << 40) | (1ull << 38) | (rip - ntb);
                for (int q = 0; q < nk; q++) {
                    uint32_t hh = (uint32_t)((keys[q] * 0x9E3779B97F4A7C15ull) >> 45) & (PA_SLOTS - 1);
                    for (uint32_t n = 0; n < PA_SLOTS; n++, hh = (hh + 1) & (PA_SLOTS - 1)) {
                        if (g_pa_key[hh] == keys[q]) { g_pa_cnt[hh]++; break; }
                        if (!g_pa_key[hh]) { g_pa_key[hh] = keys[q]; g_pa_cnt[hh] = 1; break; }
                    }
                }
            }
            if (rip >= lo && rip < hi) {
                p->inexe++;
                prof_count(rip - lo);
                uint64_t key = ((uint64_t)(k + 1) << 40) | (rip - lo);
                uint32_t hh = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 45) & (PA_SLOTS - 1);
                for (uint32_t n = 0; n < PA_SLOTS; n++, hh = (hh + 1) & (PA_SLOTS - 1)) {
                    if (g_pa_key[hh] == key) { g_pa_cnt[hh]++; break; }
                    if (!g_pa_key[hh]) { g_pa_key[hh] = key; g_pa_cnt[hh] = 1; break; }
                }
            } else {
                outside++;
                HMODULE m = NULL;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)(uintptr_t)rip, &m) && m) {
                    int j;
                    for (j = 0; j < g_prof_nmod && g_prof_mod[j] != m; j++) {}
                    if (j == g_prof_nmod && j < 64) { g_prof_mod[j] = m; g_prof_nmod++; }
                    if (j < 64) g_prof_modcnt[j]++;
                } else g_prof_modcnt[64]++;
            }
        }
        if (now - last_dump >= 20000) {
            last_dump = now;
            prof_dump_full(total, outside);
            pa_dump(ticks);
            fflush(stderr);
        }
    }
    return 0;
}
static void prof_count(uint64_t rva)
{
    uint32_t h = (uint32_t)((rva * 0x9E3779B97F4A7C15ull) >> 47) & (PROF_SLOTS - 1);
    for (uint32_t n = 0; n < PROF_SLOTS; n++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (g_prof_key[h] == rva) { g_prof_cnt[h]++; break; }
        if (!g_prof_key[h]) { g_prof_key[h] = rva; g_prof_cnt[h] = 1; break; }
    }
}

static DWORD WINAPI profile_thread(LPVOID arg)
{
    HANDLE th = (HANDLE)arg;
    HMODULE exe = GetModuleHandleA(NULL);
    uint64_t lo = (uint64_t)(uintptr_t)exe, hi = lo;
    {   IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)exe;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)exe + dos->e_lfanew);
        hi = lo + nt->OptionalHeader.SizeOfImage; }
    uint64_t total = 0, outside = 0;
    DWORD last = GetTickCount();
    /* 1 ms sleeps: the gfx layer sets timeBeginPeriod(1) (TIMER_1MS) */
    for (;;) {
        Sleep(1);
        if (SuspendThread(th) == (DWORD)-1) break;
        CONTEXT ctx; ctx.ContextFlags = g_prof_stack ? CONTEXT_FULL : CONTEXT_CONTROL;
        BOOL ok = GetThreadContext(th, &ctx);
        uint64_t caller = 0;
        if (ok && g_prof_stack && (ctx.Rip < lo || ctx.Rip >= hi)) {
            /* SW_PROFILE_STACK=1: a sample outside the exe (a wait, the driver) is also
             * counted at the first exe return address up the stack, so the report says
             * which runtime function waited.  Unwound while the thread is suspended. */
            CONTEXT u = ctx;
            for (int d = 0; d < 48; d++) {
                DWORD64 base = 0;
                PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(u.Rip, &base, NULL);
                if (!fe) { if (!u.Rsp) break; u.Rip = *(DWORD64 *)(uintptr_t)u.Rsp; u.Rsp += 8; }
                else {
                    PVOID hd = NULL; DWORD64 ef = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, u.Rip, fe, &u, &hd, &ef, NULL);
                }
                if (!u.Rip) break;
                if (u.Rip >= lo && u.Rip < hi) { caller = u.Rip - lo; break; }
            }
        }
        ResumeThread(th);
        {   /* SW_PROFILE_THREAD=<guest thread index>: only samples while that fiber runs;
             * SW_PROFILE_WINDOW=<from s>,<to s>: only samples in that time window */
            extern volatile int sw_prof_fiber;
            if (g_prof_thr >= 0 && sw_prof_fiber != g_prof_thr) continue;
            if (g_prof_win_to > 0) {
                double t = (double)(GetTickCount64() - g_prof_t0) / 1000.0;
                if (t < g_prof_win_from || t > g_prof_win_to) continue;
            }
        }
        if (caller) {
            uint64_t key = caller | (1ull << 62);        /* "waited / called out from here" */
            uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 47) & (PROF_SLOTS - 1);
            for (uint32_t n = 0; n < PROF_SLOTS; n++, h = (h + 1) & (PROF_SLOTS - 1)) {
                if (g_prof_key[h] == key) { g_prof_cnt[h]++; break; }
                if (!g_prof_key[h]) { g_prof_key[h] = key; g_prof_cnt[h] = 1; break; }
            }
        }
        if (!ok) continue;
        total++;
        uint64_t rip = ctx.Rip;
        if (rip < lo || rip >= hi) {
            outside++;
            /* which DLL: driver work (nvoglv64 / amdvlk / igvk) vs waits (ntdll, kernelbase, win32u) */
            HMODULE m = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)rip, &m) && m) {
                int k;
                for (k = 0; k < g_prof_nmod && g_prof_mod[k] != m; k++) {}
                if (k == g_prof_nmod && k < 64) { g_prof_mod[k] = m; g_prof_nmod++; }
                if (k < 64) g_prof_modcnt[k]++;
            } else g_prof_modcnt[64]++;
        }
        else {
            uint64_t rva = rip - lo;
            uint32_t h = (uint32_t)((rva * 0x9E3779B97F4A7C15ull) >> 47) & (PROF_SLOTS - 1);
            for (uint32_t n = 0; n < PROF_SLOTS; n++, h = (h + 1) & (PROF_SLOTS - 1)) {
                if (g_prof_key[h] == rva) { g_prof_cnt[h]++; break; }
                if (!g_prof_key[h]) { g_prof_key[h] = rva; g_prof_cnt[h] = 1; break; }
            }
        }
        if (GetTickCount() - last >= 20000) {
            last = GetTickCount();
            fprintf(stderr, "[prof] %llu samples, outside exe %llu (%.1f%%); top RVAs:\n",
                    (unsigned long long)total, (unsigned long long)outside, total ? 100.0 * outside / total : 0.0);
            for (int k = 0; k < 60; k++) {           /* top 60 by repeated max scan (rare, fine) */
                uint32_t best = 0, bi = 0;
                for (uint32_t i = 0; i < PROF_SLOTS; i++)
                    if (g_prof_cnt[i] > best && !(g_prof_key[i] >> 63)) { best = g_prof_cnt[i]; bi = i; }
                if (!best) break;
                fprintf(stderr, "[prof] %llx %u %.2f\n", (unsigned long long)g_prof_key[bi], best, 100.0 * best / total);
                g_prof_key[bi] |= 1ull << 63;        /* mark printed */
            }
            for (uint32_t i = 0; i < PROF_SLOTS; i++) g_prof_key[i] &= ~(1ull << 63);
            prof_dump_full(total, outside);
            fflush(stderr);
        }
    }
    return 0;
}

static void start_profiler(void)
{
    (void)mem_fast();             /* SW_MEM_TRACE=1 -> sw_mem_fast_on = 0 before any guest code runs */
    const char *env = getenv("SW_PROFILE");
    if (env && !strcmp(env, "all")) {
        { const char *s = getenv("SW_PROFILE_WINDOW"); if (s) sscanf(s, "%lf,%lf", &g_prof_win_from, &g_prof_win_to); }
        g_prof_t0 = GetTickCount64();
        HANDLE pt = CreateThread(NULL, 0, profile_all_thread, NULL, 0, NULL);
        if (pt) { SetThreadPriority(pt, THREAD_PRIORITY_TIME_CRITICAL); SetThreadDescription(pt, L"profiler"); }
        fprintf(stderr, "[prof] sampling every thread (SW_PROFILE=all)\n");
        return;
    }
    if (!env || *env != '1') return;
    { const char *s = getenv("SW_PROFILE_STACK"); g_prof_stack = s && *s == '1'; }
    { const char *s = getenv("SW_PROFILE_THREAD"); if (s && *s) g_prof_thr = atoi(s); }
    { const char *s = getenv("SW_PROFILE_WINDOW"); if (s) sscanf(s, "%lf,%lf", &g_prof_win_from, &g_prof_win_to); }
    g_prof_t0 = GetTickCount64();
    HANDLE th = NULL;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &th,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (th) CreateThread(NULL, 0, profile_thread, th, 0, NULL);
}

static void start_watchdog(SwCpu *c)
{
    start_profiler();
    const char *env = getenv("SW_WATCHDOG");
    unsigned secs = env ? (unsigned)strtoul(env, NULL, 10) : 0;
    if (!secs) return;
    g_watch_cpu = c;
    CreateThread(NULL, 0, watchdog_thread, (LPVOID)(uintptr_t)secs, 0, NULL);
}
#else
static void start_watchdog(SwCpu *c) { (void)c; }
#endif

static void dump_pointers_before(SwCpu *c, const char *stage)
{
    sw_report("=== Pointer dump BEFORE %s ===\n", stage);
    dump_pointer(c, "0x432c000 (table 0x3364ef0 loads from)", 0x432c000ULL);
    dump_pointer(c, "0x5093c00 (wait slot 0)", 0x5093c00ULL);
    dump_pointer(c, "0x5093c08 (wait slot 1)", 0x5093c08ULL);
    dump_pointer(c, "0x5096670", 0x5096670ULL);
    dump_pointer(c, "0x5096688", 0x5096688ULL);
    dump_pointer(c, "0x5085f20 (timer callback reg)", 0x5085f20ULL);
    dump_pointer(c, "0x511f008 (fn ptr loaded by 0x476c8d0)", 0x511f008ULL);
    sw_report("=== End pointer dump BEFORE %s ===\n", stage);
}

static void dump_pointers_after(SwCpu *c, const char *stage)
{
    sw_report("=== Pointer dump AFTER %s ===\n", stage);
    dump_pointer(c, "0x432c000 (table 0x3364ef0 loads from)", 0x432c000ULL);
    dump_pointer(c, "0x5093c00 (wait slot 0)", 0x5093c00ULL);
    dump_pointer(c, "0x5093c08 (wait slot 1)", 0x5093c08ULL);
    dump_pointer(c, "0x5096670", 0x5096670ULL);
    dump_pointer(c, "0x5096688", 0x5096688ULL);
    dump_pointer(c, "0x5085f20 (timer callback reg)", 0x5085f20ULL);
    dump_pointer(c, "0x511f008 (fn ptr loaded by 0x476c8d0)", 0x511f008ULL);
    sw_report("=== End pointer dump AFTER %s ===\n", stage);
}

/* NSO/ELF dynamic tags */
#define DT_NULL           0
#define DT_NEEDED         1
#define DT_PLTRELSZ       2
#define DT_PLTGOT         3
#define DT_HASH           4
#define DT_STRTAB         5
#define DT_SYMTAB         6
#define DT_RELA           7
#define DT_RELASZ         8
#define DT_RELAENT        9
#define DT_STRSZ          10
#define DT_SYMENT         11
#define DT_INIT           12
#define DT_FINI           13
#define DT_SONAME         14
#define DT_REL            17
#define DT_RELSZ          18
#define DT_RELENT         19
#define DT_PLTREL         20
#define DT_JMPREL         23
#define DT_INIT_ARRAY     25
#define DT_FINI_ARRAY     26
#define DT_INIT_ARRAYSZ   27
#define DT_FINI_ARRAYSZ   28
#define DT_FLAGS          30
#define DT_GNU_HASH       0x6FFFFEF5
#define DT_FLAGS_1        0x6FFFFFFB
#define DT_RELACOUNT      0x6FFFFFF9
#define DT_RELCOUNT       0x6FFFFFFA

/* Relocation types */
#define R_AARCH64_RELATIVE    1027
#define R_AARCH64_GLOB_DAT    1025
#define R_AARCH64_JUMP_SLOT   1026
#define R_AARCH64_ABS64       257

/* MOD0 header (embedded in .text segment) */
typedef struct {
    u32 magic;          /* "MOD0" */
    u32 dyn_raw;        /* offset to .dynamic array (relative to MOD0 header or image base) */
    u32 bss_start;      /* .bss start offset (relative to MOD0 header base) */
    u32 bss_end;        /* .bss end offset */
    u32 eh_frame_hdr_start;
    u32 eh_frame_hdr_end;
    u32 module_object;
} Mod0Header;

/* Parse MOD0 header from module image.
 * Returns 1 if found, 0 if not found (e.g., rtld, imports modules).
 * On success, fills out dyn_offset (file offset of .dynamic), bss_start, bss_end (image-relative).
 */
static int parse_mod0(const u8 *image, u64 image_size, u64 text_size, u64 *out_dyn_offset, u64 *out_bss_start, u64 *out_bss_end)
{
    /* MOD0 is at offset 0x8 in the .text segment (which starts at file offset 0) */
    if (image_size < 0x30) return 0;
    if (memcmp(image + 8, "MOD0", 4) != 0) return 0;

    Mod0Header *mod0 = (Mod0Header *)(image + 8);
    u32 dyn_raw = mod0->dyn_raw;
    u32 bss_s = mod0->bss_start;
    u32 bss_e = mod0->bss_end;

    /* MOD0 offsets are relative to the MOD0 header itself (base = 0x8) */
    u64 mod0_base = 8;
    u64 dyn_offset = dyn_raw + mod0_base;
    u64 bss_start = bss_s + mod0_base;
    u64 bss_end = bss_e + mod0_base;

    if (dyn_offset >= image_size) return 0;
    if (bss_end > image_size) return 0;

    *out_dyn_offset = dyn_offset;
    *out_bss_start = bss_start;
    *out_bss_end = bss_end;
    return 1;
}

/* Parse .dynamic array at given file offset.
 * Fills out DT_RELA, DT_RELASZ, DT_JMPREL, DT_PLTRELSZ, DT_INIT_ARRAY, DT_INIT_ARRAYSZ.
 * Returns 1 on success, 0 on failure.
 */
static int parse_dynamic(const u8 *image, u64 image_size, u64 dyn_offset,
                         u64 *out_rela_off, u64 *out_rela_sz,
                         u64 *out_jmprel_off, u64 *out_jmprel_sz,
                         u64 *out_init_arr_off, u64 *out_init_arr_sz)
{
    if (dyn_offset + 16 > image_size) return 0;

    *out_rela_off = 0;
    *out_rela_sz = 0;
    *out_jmprel_off = 0;
    *out_jmprel_sz = 0;
    *out_init_arr_off = 0;
    *out_init_arr_sz = 0;

    for (u64 off = dyn_offset; off + 16 <= image_size; off += 16) {
            int64_t tag;
            u64 val;
            memcpy(&tag, image + off, 8);
            memcpy(&val, image + off + 8, 8);

        if (tag == DT_NULL) break;

        switch (tag) {
        case DT_RELA:
            *out_rela_off = val;
            break;
        case DT_RELASZ:
            *out_rela_sz = val;
            break;
        case DT_JMPREL:
            *out_jmprel_off = val;
            break;
        case DT_PLTRELSZ:
            *out_jmprel_sz = val;
            break;
        case DT_INIT_ARRAY:
            *out_init_arr_off = val;
            break;
        case DT_INIT_ARRAYSZ:
            *out_init_arr_sz = val;
            break;
        }
    }
    return 1;
}

/* DT_INIT of a loaded module, read from guest memory: base+4 holds the MOD0
 * offset, MOD0+4 the dynamic section offset (relative to MOD0).  0 if none. */
static u64 module_dt_init(SwCpu *c, u64 base)
{
    u32 mod0_off = 0, dyn_rel = 0;
    u8 *p = mem_host(c, base + 4, 4, 0);
    if (!p) return 0;
    memcpy(&mod0_off, p, 4);
    u64 mod0 = base + mod0_off;
    p = mem_host(c, mod0, 8, 0);
    if (!p || memcmp(p, "MOD0", 4) != 0) return 0;
    memcpy(&dyn_rel, p + 4, 4);
    u64 dyn = mod0 + (u64)(s32)dyn_rel;
    for (int i = 0; i < 512; i++) {
        u8 *e = mem_host(c, dyn + 16ull * (u64)i, 16, 0);
        if (!e) return 0;
        int64_t tag; u64 val;
        memcpy(&tag, e, 8);
        memcpy(&val, e + 8, 8);
        if (tag == DT_NULL) return 0;
        if (tag == DT_INIT) return base + val;
    }
    return 0;
}

/* Symbol table entry for dynsym parsing */
typedef struct {
    u32 name_hash;  /* simple hash of name for fast lookup */
    u64 placeholder_addr;  /* address from native_thunks (0xFF000000+) */
} NativeSymbol;

/* Simple hash for symbol names */
static u32 hash_name(const char *name)
{
    u32 h = 5381;
    while (*name) {
        h = ((h << 5) + h) + (u8)*name;  /* h * 33 + c */
        name++;
    }
    return h;
}

/* Global native symbol map (built from sw_native_entries) */
static NativeSymbol *g_native_symbols = NULL;
static u32 g_native_symbol_count = 0;

/* Native thunk dispatch: maps placeholder address -> host function pointer */
typedef struct {
    u64 placeholder_addr;
    void (*func)(SwCpu *c);
} NativeThunk;

static NativeThunk *g_native_thunks = NULL;
static u32 g_native_thunk_count = 0;

/* Per-module symbol tables kept after relocation for cross-module lookup. */
/* Symbol info for dynsym parsing */
typedef struct {
    char *name;
    u32 name_hash;
    u16 shndx;
    u64 value;
    u8 is_import;
    u8 binding;  /* STB_LOCAL/STB_GLOBAL/STB_WEAK */
} DynSymbol;
typedef struct {
    const char *name;
    u64 base;
    DynSymbol *symbols;
    u32 sym_count;
} ModuleSymbols;
static ModuleSymbols *g_module_symbols = NULL;
static u32 g_module_symbol_count = 0;

/* Look up a symbol by name across all loaded modules' dynsym exports.
 * Returns the guest address or 0 if not found. */
static u64 lookup_symbol_in_all_modules(const char *name);
u64 lookup_symbol_in_all_modules_fwd(const char *name) { return lookup_symbol_in_all_modules(name); }
static u64 lookup_symbol_in_all_modules(const char *name)
{
    /* Host overrides win over guest exports: sdk ships its own
     * nvnBootstrapLoader that would try to reach the real GPU driver. */
    if (strcmp(name, "nvnBootstrapLoader") == 0) return 0xFF001000ULL;
    if (strcmp(name, "nvnDeviceGetProcAddress") == 0) return 0xFF001008ULL;
    for (u32 m = 0; m < g_module_symbol_count; m++) {
        ModuleSymbols *ms = &g_module_symbols[m];
        for (u32 i = 0; i < ms->sym_count; i++) {
            DynSymbol *sym = &ms->symbols[i];
            if (!sym->name || sym->is_import) continue;
            if (strcmp(sym->name, name) == 0) {
                return ms->base + sym->value;
            }
        }
    }
    return 0;
}

/* Store a module's symbol table for cross-module lookup. */
static void store_module_symbols(const char *name, u64 base,
                                 DynSymbol *symbols, u32 sym_count)
{
    if (g_module_symbol_count == 0) {
        g_module_symbols = (ModuleSymbols *)malloc(8 * sizeof(ModuleSymbols));
    } else if ((g_module_symbol_count & 7) == 0) {
        g_module_symbols = (ModuleSymbols *)realloc(g_module_symbols,
            (g_module_symbol_count + 8) * sizeof(ModuleSymbols));
    }
    g_module_symbols[g_module_symbol_count].name = name;
    g_module_symbols[g_module_symbol_count].base = base;
    g_module_symbols[g_module_symbol_count].symbols = symbols;
    g_module_symbols[g_module_symbol_count].sym_count = sym_count;
    g_module_symbol_count++;
}

/* Register a native thunk at a placeholder address.
 * Placeholders 0xFF000000..0xFF1FFFFF (imports, HID, NVN procs at 0xFF100000 + 8i)
 * live in a flat table indexed by the address: one load per lookup (the old
 * linear list put the NVN procs, registered last, behind every import - each
 * NVN call scanned the whole list) and no realloc that another core could read
 * through (SW_MULTICORE).  Anything else goes to a fixed-size list. */
#define THUNK_FLAT_BASE 0xFF000000ULL
#define THUNK_FLAT_SPAN 0x200000ULL
#define THUNK_LIST_MAX 4096
static void (**g_thunk_flat)(SwCpu *c);
static void register_native_thunk(u64 placeholder_addr, void (*func)(SwCpu *c))
{
    if (!g_thunk_flat) {
#if defined(_WIN32)
        g_thunk_flat = (void (**)(SwCpu *))VirtualAlloc(NULL, THUNK_FLAT_SPAN * sizeof *g_thunk_flat,
                                                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        g_thunk_flat = (void (**)(SwCpu *))calloc(THUNK_FLAT_SPAN, sizeof *g_thunk_flat);
#endif
        g_native_thunks = (NativeThunk *)calloc(THUNK_LIST_MAX, sizeof(NativeThunk));
        if (!g_thunk_flat || !g_native_thunks) { fprintf(stderr, "native thunk table: out of memory\n"); exit(1); }
    }
    if (placeholder_addr - THUNK_FLAT_BASE < THUNK_FLAT_SPAN) {
        g_thunk_flat[placeholder_addr - THUNK_FLAT_BASE] = func;   /* one aligned pointer store */
        return;
    }
    if (g_native_thunk_count >= THUNK_LIST_MAX) { fprintf(stderr, "native thunk list full\n"); exit(1); }
    g_native_thunks[g_native_thunk_count].placeholder_addr = placeholder_addr;
    g_native_thunks[g_native_thunk_count].func = func;
#if defined(_WIN32)
    MemoryBarrier();                          /* the entry before the count that publishes it */
#endif
    g_native_thunk_count++;
}

/* Find native thunk by placeholder address */
static void (*find_native_thunk(u64 addr))(SwCpu *c)
{
    if (addr - THUNK_FLAT_BASE < THUNK_FLAT_SPAN) return g_thunk_flat ? g_thunk_flat[addr - THUNK_FLAT_BASE] : NULL;
    u32 n = *(volatile u32 *)&g_native_thunk_count;
    for (u32 i = 0; i < n; i++) {
        if (g_native_thunks[i].placeholder_addr == addr) {
            return g_native_thunks[i].func;
        }
    }
    return NULL;
}

/* The address currently being dispatched to.  Set by sw_dispatch just before
 * calling a native thunk, so logging thunks can report which symbol was called. */
/* (g_dispatch_target is declared thread-local near g_gen_depth) */

/* Unresolved-import logging thunk table: placeholder address -> symbol name.
 * Strong undefined symbols that cannot be resolved are bound to a logging
 * thunk instead of being left as 0, so the first call reports the missing
 * symbol by name. */
typedef struct { u64 addr; const char *name; } UnresolvedLogEntry;
static UnresolvedLogEntry *g_unresolved_log = NULL;
static u32 g_unresolved_log_count = 0;
static u32 g_unresolved_log_cap = 0;

static void register_unresolved_log(u64 addr, const char *name)
{
    if (g_unresolved_log_count == g_unresolved_log_cap) {
        u32 ncap = g_unresolved_log_cap ? g_unresolved_log_cap * 2 : 64;
        UnresolvedLogEntry *n = (UnresolvedLogEntry *)realloc(g_unresolved_log,
            ncap * sizeof(UnresolvedLogEntry));
        if (!n) return;
        g_unresolved_log = n;
        g_unresolved_log_cap = ncap;
    }
    g_unresolved_log[g_unresolved_log_count].addr = addr;
    g_unresolved_log[g_unresolved_log_count].name = name;
    g_unresolved_log_count++;
}

static const char *lookup_unresolved_log(u64 addr)
{
    for (u32 i = 0; i < g_unresolved_log_count; i++)
        if (g_unresolved_log[i].addr == addr) return g_unresolved_log[i].name;
    return NULL;
}

/* Logging thunk for unresolved strong undefined imports.
 * Default behaviour: log symbol name + caller PC once per symbol, then halt
 * (trap) so an unresolved import is never silently executed with a bogus 0.
 * Only with SW_DEBUG_CONTINUE=1 does it log and return 0 instead. */
static void sw_thunk_unresolved(SwCpu *c)
{
    const char *name = lookup_unresolved_log(g_dispatch_target);
    static int g_logged_continuation = 0;
    const char *cont = getenv("SW_DEBUG_CONTINUE");
    sw_report("[unresolved] import %s called from caller PC 0x%llx",
              name ? name : "(unknown)",
              (unsigned long long)c->pc);
    if (cont && *cont == '1') {
        if (!g_logged_continuation) {
            sw_report(" (SW_DEBUG_CONTINUE=1: returning 0, continuing)\n");
            g_logged_continuation = 1;
        }
        c->x[0] = 0;
        return;
    }
    sw_report(" — halting (trap). Set SW_DEBUG_CONTINUE=1 to log and return 0 instead.\n");
    sw_trap(c, g_dispatch_target, name ? name : "unresolved import");
}

/* Build native symbol map from sw_native_entries */
static void build_native_symbol_map(void)
{
    if (g_native_symbols) return;  /* already built */

    g_native_symbol_count = sw_native_count;
    if (g_native_symbol_count == 0) return;

    g_native_symbols = (NativeSymbol *)calloc(g_native_symbol_count, sizeof(NativeSymbol));
    if (!g_native_symbols) return;

    for (u32 i = 0; i < sw_native_count; i++) {
        const char *name = sw_native_entries[i].name;
        u64 addr = sw_native_entries[i].addr;
        g_native_symbols[i].name_hash = hash_name(name);
        g_native_symbols[i].placeholder_addr = addr;
        /* Also register as thunk if we have the function pointer */
        /* The thunk functions are named sw_import_XXXXXXXX */
    }
    sw_report("debug: built native symbol map with %u entries\n", g_native_symbol_count);
}

/* Exported so other runtime files (sw_nvn.c) can add thunks at run time. */
void sw_register_native_thunk(u64 placeholder_addr, void (*func)(SwCpu *c))
{
    register_native_thunk(placeholder_addr, func);
}

u64 sw_dispatch_target(void)
{
    return g_dispatch_target;
}

/* Initialize critical native thunks (NVN entry points live in sw_nvn.c). */
static void init_critical_native_thunks(void)
{
    sw_nvn_init();
}

/* Look up placeholder address by symbol name */
/* Imports answered by host code (sw_hid.c): each name gets one native thunk
 * at 0xFF00E000 + 8*i, shared by every relocation that names it. */
void (*sw_hid_override(const char *name))(SwCpu *);
static u64 hle_override_addr(const char *name)
{
    static struct { const char *name; u64 addr; } seen[64];
    static int n;
    void (*fn)(SwCpu *) = sw_hid_override(name);
    if (!fn) return 0;
    for (int i = 0; i < n; i++) if (!strcmp(seen[i].name, name)) return seen[i].addr;
    if (n >= 64) return 0;
    u64 a = 0xFF00E000ULL + 8ULL * (u64)n;
    register_native_thunk(a, fn);
    seen[n].name = _strdup(name); seen[n].addr = a; n++;
    sw_report("[hle] %s -> host (0x%llx)\n", name, (unsigned long long)a);
    return a;
}

static u64 lookup_native_placeholder(const char *name)
{
    if (!g_native_symbols) build_native_symbol_map();
    if (!g_native_symbols) return 0;

    /* Special handling for linker-generated symbols that should not go through thunks */
    if (strcmp(name, "__rel_dyn_start") == 0 ||
        strcmp(name, "__rel_dyn_end") == 0 ||
        strcmp(name, "__rel_plt_start") == 0 ||
        strcmp(name, "__rel_plt_end") == 0) {
        return 0;  /* Will be handled specially in apply_reloc_table */
    }

    /* Manually registered critical NVN symbols */
    if (strcmp(name, "nvnBootstrapLoader") == 0) return 0xFF001000;
    if (strcmp(name, "nvnDeviceGetProcAddress") == 0) return 0xFF001008;

    u32 h = hash_name(name);
    for (u32 i = 0; i < g_native_symbol_count; i++) {
        if (g_native_symbols[i].name_hash == h && strcmp(name, sw_native_entries[i].name) == 0) {
            return g_native_symbols[i].placeholder_addr;
        }
    }
    return 0;
}


/* Parse dynsym/dynstr for a module to get symbol index -> DynSymbol mapping.
 * Returns number of symbols parsed, or 0 on failure.
 * The caller must free the returned array.
 */
static DynSymbol *parse_module_symbols(const u8 *image, u64 image_size, u64 dyn_offset, u32 *out_count)
{
    if (!dyn_offset || dyn_offset + 16 > image_size) return NULL;

    /* Find DT_SYMTAB and DT_STRTAB from dynamic section */
    u64 symtab_off = 0, strtab_off = 0, strtab_sz = 0;
    for (u64 off = dyn_offset; off + 16 <= image_size; off += 16) {
        int64_t tag;
        u64 val;
        memcpy(&tag, image + off, 8);
        memcpy(&val, image + off + 8, 8);
        if (tag == DT_NULL) break;
        if (tag == DT_SYMTAB) symtab_off = val;
        else if (tag == DT_STRTAB) strtab_off = val;
        else if (tag == DT_STRSZ) strtab_sz = val;
    }
    if (!symtab_off || !strtab_off || !strtab_sz) return NULL;
    if (symtab_off >= image_size || strtab_off + strtab_sz > image_size) return NULL;

    /* Count symbols */
    u32 sym_count = 0;
    for (u64 off = symtab_off; off + 24 <= image_size; off += 24) {
        /* sinfo/shndx are 1- and 2-byte fields: zero-init so the partial
         * memcpy doesn't leave stack garbage in the upper bytes (that made
         * imports such as nvnBootstrapLoader look defined -> bound to base). */
        u32 name_off = 0, sinfo = 0, shndx = 0;
        u64 value, size;
        memcpy(&name_off, image + off, 4);
        memcpy(&sinfo, image + off + 4, 1);
        memcpy(&shndx, image + off + 6, 2);
        memcpy(&value, image + off + 8, 8);
        memcpy(&size, image + off + 16, 8);
        if (sym_count > 0 && name_off == 0 && sinfo == 0 && shndx == 0 && value == 0 && size == 0) break;
        sym_count++;
        if (sym_count > 200000) break;
    }
    if (sym_count == 0) return NULL;

    /* Allocate array */
    DynSymbol *symbols = (DynSymbol *)calloc(sym_count, sizeof(DynSymbol));
    if (!symbols) return NULL;

    /* Read string table */
    const u8 *strtab = image + strtab_off;

    /* Parse symbols */
    u32 idx = 0;
    for (u64 off = symtab_off; off + 24 <= image_size && idx < sym_count; off += 24, idx++) {
        /* sinfo/shndx are 1- and 2-byte fields: zero-init so the partial
         * memcpy doesn't leave stack garbage in the upper bytes (that made
         * imports such as nvnBootstrapLoader look defined -> bound to base). */
        u32 name_off = 0, sinfo = 0, shndx = 0;
        u64 value, size;
        memcpy(&name_off, image + off, 4);
        memcpy(&sinfo, image + off + 4, 1);
        memcpy(&shndx, image + off + 6, 2);
        memcpy(&value, image + off + 8, 8);
        memcpy(&size, image + off + 16, 8);

        if (idx > 0 && name_off == 0 && sinfo == 0 && shndx == 0 && value == 0 && size == 0) break;

        symbols[idx].shndx = shndx;
        symbols[idx].value = value;
        symbols[idx].is_import = (shndx == 0);
        symbols[idx].binding = (u8)(sinfo >> 4);

        if (name_off < strtab_sz) {
            const char *str = (const char *)(strtab + name_off);
            symbols[idx].name = strdup(str);
            symbols[idx].name_hash = hash_name(str);
        } else {
            symbols[idx].name = NULL;
            symbols[idx].name_hash = 0;
        }
    }

    *out_count = sym_count;
    return symbols;
}

/* Free symbol array */
static void free_module_symbols(DynSymbol *symbols, u32 count)
{
    if (!symbols) return;
    for (u32 i = 0; i < count; i++) {
        free(symbols[i].name);
    }
    free(symbols);
}

/* Apply relocations from a relocation table (RELA or JMPREL).
 * reloc_off: file offset of relocation table (image-relative)
 * reloc_sz: size in bytes
 * base: module load base (guest address)
 * type_filter: if non-zero, only apply this relocation type
 * symbols: array of DynSymbol from dynsym (index -> symbol info), or NULL
 * sym_count: number of symbols in symbols
 * rela_off, rela_sz: RELA table offset/size (for __rel_dyn_* symbols)
 * jmprel_off, jmprel_sz: JMPREL table offset/size (for __rel_plt_* symbols)
 */
static u64 apply_reloc_table(SwCpu *c, const u8 *image, u64 image_size,
                             u64 reloc_off, u64 reloc_sz, u64 base, u32 type_filter,
                             DynSymbol *symbols, u32 sym_count,
                             u64 rela_off, u64 rela_sz,
                             u64 jmprel_off, u64 jmprel_sz,
                             RelocModuleStats *ms)
{
    if (reloc_off == 0 || reloc_sz == 0) return 0;
    if (reloc_off + reloc_sz > image_size) return 0;
    if (reloc_sz % 24 != 0) return 0;

    u64 applied = 0;
    u64 count = reloc_sz / 24;

    for (u64 i = 0; i < count; i++) {
        u64 entry_off = reloc_off + i * 24;
        if (entry_off + 24 > image_size) break;

        u64 r_offset, r_info, r_addend;
        memcpy(&r_offset, image + entry_off, 8);
        memcpy(&r_info, image + entry_off + 8, 8);
        memcpy(&r_addend, image + entry_off + 16, 8);

        u32 reloc_type = (u32)(r_info & 0xFFFFFFFFull);
        u32 sym_index = (u32)(r_info >> 32);

        if (type_filter && reloc_type != type_filter) continue;

        /* r_offset is image-relative (relative to module base) */
        u64 addr_to_patch = base + r_offset;

        u64 *patch_ptr = (u64 *)mem_host(c, addr_to_patch, 8, 0);
        if (!patch_ptr) continue;

        u64 old_val = *patch_ptr;
        u64 new_val = old_val;

        const char *sym_name = NULL;
        u8 binding = STB_LOCAL;
        u16 shndx = 0;
        u64 S = 0;

        if (symbols && sym_index < sym_count && symbols[sym_index].name) {
            DynSymbol *sym = &symbols[sym_index];
            sym_name = sym->name;
            binding = sym->binding;
            shndx = sym->shndx;
        }

        switch (reloc_type) {
        case R_AARCH64_RELATIVE:
            new_val = base + r_addend;
            break;
        case R_AARCH64_GLOB_DAT:
                case R_AARCH64_JUMP_SLOT:
                    /* Resolve symbol: check if it's an export from this module or an import */
                    if (symbols && sym_index < sym_count && symbols[sym_index].name) {
                        DynSymbol *sym = &symbols[sym_index];

                        /* Special handling for linker-generated symbols */
                        if (strcmp(sym->name, "__rel_dyn_start") == 0) {
                            new_val = base + rela_off;
                            if (!g_reloc_audit)
                            sw_report("  RELOC type=%u @ 0x%llx: %s -> 0x%llx (RELA start)\n",
                                      reloc_type, (unsigned long long)addr_to_patch,
                                      sym->name, (unsigned long long)new_val);
                        } else if (strcmp(sym->name, "__rel_dyn_end") == 0) {
                            new_val = base + rela_off + rela_sz;
                            if (!g_reloc_audit)
                            sw_report("  RELOC type=%u @ 0x%llx: %s -> 0x%llx (RELA end)\n",
                                      reloc_type, (unsigned long long)addr_to_patch,
                                      sym->name, (unsigned long long)new_val);
                        } else if (strcmp(sym->name, "__rel_plt_start") == 0) {
                            new_val = base + jmprel_off;
                            if (!g_reloc_audit)
                            sw_report("  RELOC type=%u @ 0x%llx: %s -> 0x%llx (JMPREL start)\n",
                                      reloc_type, (unsigned long long)addr_to_patch,
                                      sym->name, (unsigned long long)new_val);
                        } else if (strcmp(sym->name, "__rel_plt_end") == 0) {
                            new_val = base + jmprel_off + jmprel_sz;
                            if (!g_reloc_audit)
                            sw_report("  RELOC type=%u @ 0x%llx: %s -> 0x%llx (JMPREL end)\n",
                                      reloc_type, (unsigned long long)addr_to_patch,
                                      sym->name, (unsigned long long)new_val);
                        } else if (!sym->is_import) {
                            /* Symbol is an export from this module - resolve to its actual address */
                            new_val = base + sym->value;
                            if (!g_reloc_audit)
                            sw_report("  RELOC type=%u @ 0x%llx: %s (local export) -> 0x%llx\n",
                                      reloc_type, (unsigned long long)addr_to_patch,
                                      sym->name, (unsigned long long)new_val);
                        } else {
                            /* Symbol is an import: host overrides (nn::hid), then cross-module lookup */
                            u64 found = hle_override_addr(sym->name);
                            if (!found) found = lookup_symbol_in_all_modules(sym->name);
                            if (found) {
                                new_val = found;
                                if (!g_reloc_audit)
                                sw_report("  RELOC type=%u @ 0x%llx: %s (import) -> cross-module 0x%llx\n",
                                          reloc_type, (unsigned long long)addr_to_patch,
                                          sym->name, (unsigned long long)found);
                            } else {
                                /* Then native thunk */
                                u64 placeholder = lookup_native_placeholder(sym->name);
                                if (placeholder) {
                                    new_val = placeholder;
                                    if (!g_reloc_audit)
                                    sw_report("  RELOC type=%u @ 0x%llx: %s (import) -> placeholder 0x%llx\n",
                                              reloc_type, (unsigned long long)addr_to_patch,
                                              sym->name, (unsigned long long)placeholder);
                                } else if (sym->binding == STB_WEAK) {
                                    /* Only weak undefined may be 0 */
                                    new_val = 0;
                                    if (!g_reloc_audit)
                                    sw_report("  RELOC type=%u @ 0x%llx: %s (weak import) -> 0\n",
                                              reloc_type, (unsigned long long)addr_to_patch,
                                              sym->name);
                                } else {
                                    /* Strong undefined: bind to a logging thunk */
                                    u64 placeholder = 0xFF00F000ULL + g_unresolved_log_count;
                                    register_unresolved_log(placeholder, sym->name);
                                    register_native_thunk(placeholder, sw_thunk_unresolved);
                                    new_val = placeholder;
                                    sw_report("  RELOC type=%u @ 0x%llx: %s (strong import) -> logging thunk 0x%llx\n",
                                              reloc_type, (unsigned long long)addr_to_patch,
                                              sym->name, (unsigned long long)placeholder);
                                }
                            }
                        }
                    } else {
                        /* Fallback: treat as RELATIVE */
                        new_val = base + r_addend;
                        if (!g_reloc_audit)
                        sw_report("  RELOC type=%u @ 0x%llx: sym=%u (no name) -> base+addend 0x%llx\n",
                                  reloc_type, (unsigned long long)addr_to_patch,
                                  sym_index, (unsigned long long)new_val);
                    }
                    break;
        case R_AARCH64_ABS64: {
            u64 S = 0;
            if (symbols && sym_index < sym_count && symbols[sym_index].name) {
                DynSymbol *sym = &symbols[sym_index];
                if (!sym->is_import) {
                    /* Local export - resolve to actual address */
                    S = base + sym->value;
                } else {
                    /* Import - host overrides, cross-module lookup, then native thunk */
                    S = hle_override_addr(sym->name);
                    if (S == 0) S = lookup_symbol_in_all_modules(sym->name);
                    if (S == 0) {
                        u64 placeholder = lookup_native_placeholder(sym->name);
                        if (placeholder) {
                            S = placeholder;
                        } else if (sym->binding == STB_WEAK) {
                            /* Only weak undefined may be 0 */
                            S = 0;
                        } else {
                            /* Strong undefined: bind to a logging thunk */
                            u64 placeholder = 0xFF00F000ULL + g_unresolved_log_count;
                            register_unresolved_log(placeholder, sym->name);
                            register_native_thunk(placeholder, sw_thunk_unresolved);
                            S = placeholder;
                            sw_report("  RELOC ABS64 @ 0x%llx: %s (strong import) -> logging thunk 0x%llx\n",
                                      (unsigned long long)addr_to_patch,
                                      sym->name, (unsigned long long)placeholder);
                        }
                    }
                }
            }
            new_val = S + r_addend;
            break;
        }
        default:
            break;
        }

        /* Record for audit */
        if (ms) {
            u64 A = r_addend;
            if (reloc_type == R_AARCH64_JUMP_SLOT) A = 0;
            record_reloc(ms, reloc_type, sym_index, sym_name, binding, shndx,
                         S, A, old_val, new_val);
        }

        if (old_val != new_val) {
            u64 *write_ptr = (u64 *)mem_host(c, addr_to_patch, 8, 1);
            if (write_ptr) {
                *write_ptr = new_val;
                if (!g_reloc_audit &&
                    (reloc_type != R_AARCH64_GLOB_DAT && reloc_type != R_AARCH64_JUMP_SLOT)) {
                    sw_report("  RELOC type=%u @ 0x%llx: 0x%llx -> 0x%llx\n",
                              reloc_type, (unsigned long long)addr_to_patch,
                              (unsigned long long)old_val, (unsigned long long)new_val);
                }
                applied++;
            }
        }
    }
    return applied;
}

static DynSymbol *find_module_symbols(const char *name, u32 *count)
{
    for (u32 i = 0; i < g_module_symbol_count; i++) {
        if (strcmp(g_module_symbols[i].name, name) == 0) {
            *count = g_module_symbols[i].sym_count;
            return g_module_symbols[i].symbols;
        }
    }
    *count = 0;
    return NULL;
}

/* Parse and store one module's dynsym.  Runs for every module before any
 * relocation is applied: storing them per module right before its own
 * relocations meant main (relocated first) could not see sdk's exports, and
 * its libc imports were bound to trap stubs. */
static void collect_module_symbols(const SwModuleDesc *m, const u8 *image, u64 image_size)
{
    u64 dyn_offset, bss_start, bss_end;
    u32 sym_count = 0;
    DynSymbol *symbols;
    if (!parse_mod0(image, image_size, m->text_size, &dyn_offset, &bss_start, &bss_end))
        return;
    symbols = parse_module_symbols(image, image_size, dyn_offset, &sym_count);
    if (symbols) {
        sw_report("debug: %s: parsed %u symbols from dynsym\n", m->name, sym_count);
        store_module_symbols(m->name, m->base, symbols, sym_count);
    }
}

/* Apply relocations for a module by parsing its NSO structure */
static void apply_relocations(SwCpu *c, const SwModuleDesc *m, const u8 *image, u64 image_size)
{
    u64 dyn_offset, bss_start, bss_end;
    if (!parse_mod0(image, image_size, m->text_size, &dyn_offset, &bss_start, &bss_end)) {
        sw_report("debug: %s: no MOD0 found, skipping relocations\n", m->name);
        return;
    }

    u64 rela_off, rela_sz, jmprel_off, jmprel_sz, init_arr_off, init_arr_sz;
    if (!parse_dynamic(image, image_size, dyn_offset,
                           &rela_off, &rela_sz, &jmprel_off, &jmprel_sz,
                           &init_arr_off, &init_arr_sz)) {
            sw_report("debug: %s: failed to parse dynamic\n", m->name);
            return;
        }

        /* Symbol tables of ALL modules were collected before any relocation
         * (collect_module_symbols), so imports from modules later in the load
         * order (main -> sdk's strlen, memset, ...) resolve. */
        u32 sym_count = 0;
        DynSymbol *symbols = find_module_symbols(m->name, &sym_count);

        u64 base = m->base;
        u64 applied = 0;
        RelocModuleStats *ms = add_reloc_stats(m->name);

        sw_report("=== Relocations for %s: RELA=0x%llx/0x%llx JMPREL=0x%llx/0x%llx INIT_ARRAY=0x%llx/0x%llx ===\n",
                  m->name, (unsigned long long)rela_off, (unsigned long long)rela_sz,
                  (unsigned long long)jmprel_off, (unsigned long long)jmprel_sz,
                  (unsigned long long)init_arr_off, (unsigned long long)init_arr_sz);

        /* Apply RELA relocations (all types) */
        applied += apply_reloc_table(c, image, image_size, rela_off, rela_sz, base, 0,
                                     symbols, sym_count, rela_off, rela_sz, jmprel_off, jmprel_sz,
                                     ms);

        /* Apply JMPREL (PLT) relocations */
        applied += apply_reloc_table(c, image, image_size, jmprel_off, jmprel_sz, base, 0,
                                     symbols, sym_count, rela_off, rela_sz, jmprel_off, jmprel_sz,
                                     ms);

        /* Note: symbols are retained in g_module_symbols for cross-module lookup;
         * do not free them here. */
        sw_report("=== Relocations for %s: applied=%llu ===\n",
                  m->name, (unsigned long long)applied);
    }

static u64 g_bss_lo[16], g_bss_hi[16];   /* module-relative .bss range per module */

/* Call a startup function exported by one of the modules.  Returns 0 if the
 * guest halted; a missing symbol is logged and skipped. */
static int run_named_stage(SwCpu *c, const char *name, u64 a0, u64 a1)
{
    u64 addr = lookup_symbol_in_all_modules(name);
    if (!addr) {
        printf("%s: not exported by any module, skipped\n", name);
        return 1;
    }
    printf("%s: calling 0x%llx\n", name, (unsigned long long)addr);
    c->x[0] = a0;
    c->x[1] = a1;
    sw_call(c, addr);
    if (c->halted) {
        printf("%s: HALTED at 0x%llx: %s\n", name, (unsigned long long)c->halt_pc,
               c->halt_reason ? c->halt_reason : "(none)");
        return 0;
    }
    printf("%s: returned X0=0x%llx\n", name, (unsigned long long)c->x[0]);
    return 1;
}

static int load_program(SwCpu *c)
{
    u32 i;

    /* Initialize critical native thunks (NVN, etc.) */
    init_critical_native_thunks();
    { extern void sw_hooks_init(void); sw_hooks_init(); }   /* SW_HOOK_LOG (sw_hooks.c) */

    /* Phase 1: Map all module images */
    for (i = 0; i < sw_module_count; i++) {
        const SwModuleDesc *m = &sw_modules[i];
        u8 *image = NULL;
        u64 size = 0;
        if (load_image(m->image_path, &image, &size) != 0) {
            sw_report("error: cannot read module image %s\n", m->image_path);
            return -1;
        }
        if (size != m->image_size) {
            sw_report("error: module %s image size mismatch (%llu != %llu)\n", m->name,
                      (unsigned long long)size, (unsigned long long)m->image_size);
            free(image);
            return -1;
        }
        if (sw_mem_map(c, m->base, size, image, size) != 0) {
            free(image);
            return -1;
        }
        free(image);
    }

    /* Mods (sw_mods.c, MODS.md): config + discovery, then exefs data patches into the mapped images, before
     * relocation as Atmosphere's loader does.  Nothing happens when there are no mods. */
    sw_mods_init();
    sw_mods_apply_exefs();

    /* Dump pointers BEFORE relocation (modules are now mapped) */
    dump_pointers_before(c, "relocation");

    /* Phase 2a: collect every module's symbol table first */
    for (i = 0; i < sw_module_count; i++) {
        const SwModuleDesc *m = &sw_modules[i];
        u8 *img = NULL;
        u64 img_size = 0;
        if (load_image(m->image_path, &img, &img_size) == 0) {
            u64 dyn, bs, be;
            collect_module_symbols(m, img, img_size);
            /* MOD0 holds the real .bss range (segments are page-aligned). */
            if (i < 16 && parse_mod0(img, img_size, m->text_size, &dyn, &bs, &be)) {
                g_bss_lo[i] = bs;
                g_bss_hi[i] = be;
            }
            free(img);
        }
    }

    /* Phase 2b: Apply relocations from extra data for each module */
    for (i = 0; i < sw_module_count; i++) {
        const SwModuleDesc *m = &sw_modules[i];
        u8 *reload = NULL;
        u64 reload_size = 0;
        if (load_image(m->image_path, &reload, &reload_size) == 0) {
            apply_relocations(c, m, reload, reload_size);
            free(reload);
        }
    }

    /* Phase 3: Zero .bss for each module (page by page) */
    for (i = 0; i < sw_module_count; i++) {
        const SwModuleDesc *m = &sw_modules[i];
        /* .bss range from MOD0.  The old base+text+ro+data sum ignored segment
         * page alignment and zeroed the tail of each data segment: main lost
         * init_array entries from #1057 on, and sdk/subsdk lost all of theirs
         * (plus GOT entries), so their constructors never ran. */
        if (i >= 16 || g_bss_hi[i] <= g_bss_lo[i]) continue;
        u64 bss_start = m->base + g_bss_lo[i];
        u64 bss_size = g_bss_hi[i] - g_bss_lo[i];
        if (bss_size > 0) {
            u64 zeroed = 0;
            u64 off = 0;
            while (off < bss_size) {
                u64 page_addr = bss_start + off;
                u64 page_off = page_addr & (SW_PAGE_SIZE - 1);
                u64 chunk = SW_PAGE_SIZE - page_off;
                if (chunk > bss_size - off) chunk = bss_size - off;
                u8 *bss_ptr = (u8 *)mem_host(c, page_addr, chunk, 1);
                if (bss_ptr) {
                    memset(bss_ptr, 0, (size_t)chunk);
                    zeroed += chunk;
                } else {
                    sw_report("warning: cannot zero .bss page for %s at 0x%llx\n",
                              m->name, (unsigned long long)page_addr);
                }
                off += chunk;
            }
            sw_report("debug: zeroed .bss for %s at 0x%llx size=0x%llx (%llu bytes zeroed)\n",
                      m->name, (unsigned long long)bss_start,
                      (unsigned long long)bss_size, (unsigned long long)zeroed);
        }
    }

    /* Dump pointers AFTER relocation and .bss zeroing */
        dump_pointers_after(c, "relocation + .bss zeroing");

        /* Memory map overlap check */
        {
            u64 bases[64];
            u64 ends[64];
            const char *names[64];
            int n = 0;
            u32 mi;
            for (mi = 0; mi < sw_module_count; mi++) {
                const SwModuleDesc *m = &sw_modules[mi];
                bases[n] = m->base;
                ends[n] = m->base + m->text_size;
                names[n] = m->name;
                n++;
                bases[n] = m->base + m->text_size;
                ends[n] = m->base + m->text_size + m->ro_size;
                names[n] = m->name;
                n++;
                bases[n] = m->base + m->text_size + m->ro_size;
                ends[n] = m->base + m->text_size + m->ro_size + m->data_size;
                names[n] = m->name;
                n++;
                if (m->bss_size) {
                    u64 bss_start = m->base + m->text_size + m->ro_size + m->data_size;
                    bases[n] = bss_start;
                    ends[n] = bss_start + m->bss_size;
                    names[n] = m->name;
                    n++;
                }
            }
            bases[n] = 0x70000000ull;
            ends[n] = 0x70200000ull;
            names[n] = "stack";
            n++;
            bases[n] = 0x71000000ull;
            ends[n] = 0x71100000ull;
            names[n] = "TLS";
            n++;
            bases[n] = 0x71100000ull;
            ends[n] = 0x71110000ull;
            names[n] = "ThreadType";
            n++;

            {
                int a, b;
                for (a = 0; a < n; a++) {
                    for (b = a + 1; b < n; b++) {
                        if (bases[a] < ends[b] && bases[b] < ends[a]) {
                            sw_report("FATAL: Memory overlap: %s [0x%llx-0x%llx] overlaps %s [0x%llx-0x%llx]\n",
                                      names[a], (unsigned long long)bases[a], (unsigned long long)ends[a],
                                      names[b], (unsigned long long)bases[b], (unsigned long long)ends[b]);
                        }
                    }
                }
            }
            sw_report("debug: memory map overlap check passed (%d regions)\n", n);
                            }

                            /* guest stack */
    if (sw_mem_map(c, 0x70000000ull, 0x200000ull, NULL, 0) != 0) return -1;
    c->sp = 0x701FF000ull;
    if (setup_thread_pointer(c) != 0) return -1;

    /* Map zero page to catch NULL pointer accesses.  Left zero-filled so that
     * NULL function pointer dispatches are caught by sw_dispatch's target==0
     * check, and NULL data reads return 0 instead of a fake pointer value. */
        if (sw_mem_map(c, 0x0ull, 0x100000ull, NULL, 0) != 0) {
            sw_report("warning: failed to map low memory\n");
        } else {
            sw_report("debug: mapped low memory at 0x0 (size 0x100000)\n");
        }

    sw_report("debug: load_program complete\n");
    if (g_reloc_audit) print_reloc_audit();
    return 0;
}

static void audit_report(SwCpu *c)
{
    u32 i;
    printf("=== standalone execution audit ===\n");
    printf("runtime           : sw_runtime ABI v%d\n", SW_ABI_VERSION);
    printf("arm64 interpreter : none linked (no interpreter/JIT in this binary)\n");
    printf("modules loaded    : %u\n", (unsigned)sw_module_count);
    for (i = 0; i < sw_module_count; i++) {
        const SwModuleDesc *m = &sw_modules[i];
        printf("  %-8s base=0x%08llx image=0x%llx text=0x%llx ro=0x%llx data=0x%llx bss=0x%llx entry=0x%llx\n",
               m->name, (unsigned long long)m->base, (unsigned long long)m->image_size,
               (unsigned long long)m->text_size, (unsigned long long)m->ro_size,
               (unsigned long long)m->data_size, (unsigned long long)m->bss_size,
               (unsigned long long)m->entry);
        printf("           module_id=%s\n", m->module_id);
    }
    printf("recompiled funcs  : %u\n", (unsigned)sw_function_count);
    printf("native services   : %u\n", (unsigned)sw_native_count);
    printf("guest memory      : %llu KiB mapped\n",
           (unsigned long long)(c->mem->mapped_bytes / 1024));
    printf("dispatch calls    : %llu (missing: %llu)\n",
           (unsigned long long)g_dispatch_count, (unsigned long long)g_missing_dispatch_count);
    printf("syscalls          : %llu (unimplemented: %llu)\n",
           (unsigned long long)g_svc_count, (unsigned long long)g_unimplemented_count);
    printf("traps             : %llu\n", (unsigned long long)g_trap_count);
    if (g_watch_len)
        printf("watch hits        : %llu\n", (unsigned long long)g_watch_hits);
    printf("mode              : standalone native (emulator launch path: not used)\n");
}

static void print_regs(SwCpu *c)
{
    int i;
    for (i = 0; i < 31; i++) {
        if (i % 4 == 0) printf("  ");
        printf("x%-2d=%016llx%s", i, (unsigned long long)c->x[i],
               (i % 4 == 3) ? "\n" : " ");
    }
    printf("  sp =%016llx  n=%u z=%u c=%u v=%u\n", (unsigned long long)c->sp,
           c->nf, c->zf, c->cf, c->vf);
}

static void usage(const char *argv0)
{
    printf("usage: %s [options]\n", argv0);
    printf("  --audit                 print the execution audit and exit\n");
    printf("  --call ADDR[,a0,a1,..]  run one recompiled function\n");
    printf("  --run-init [--budget N] run module .init_array entries in order\n");
    printf("  --run-init-entry        dispatch each module's init function first\n");
    printf("  --watch ADDR[:LEN]      report every store into the range\n");
    printf("  --discover N            keep going past N unknown calls (seed discovery)\n");
    printf("  --max-dispatch N        trap after N inter-function calls (runaway guard)\n");
    printf("  --trace FILE            append every dispatch edge to FILE\n");
    printf("  --list-services         list native service bindings\n");
    printf("  --real-start            run real process startup (init_array + nnMain)\n");
    printf("  --no-mods               load no mods (MODS.md)\n");
    printf("  --mods-dir DIR          also search DIR for mods (repeatable; SW_MODS_DIR=a;b works too)\n");
    printf("  --mods-config FILE      mod config (default Config/mods.json or mods.txt)\n");
    printf("  --list-mods             print the mods found as JSON and exit\n");
    printf("  --reloc-audit           print relocation audit summary and exit\n");
    printf("  -v/-q                   verbose/quiet runtime logging\n");
    printf("  env SW_PROFILE=1        report the 10 longest dispatch gaps\n");
    }

/* sw_rt.h fast SIMD paths: FMLA/FMLS use FMA3 only when the CPU and the OS (YMM state saved) support it */
int sw_cpu_fma;
static void detect_cpu_features(void)
{
#if defined(_M_X64)
    int r[4];
    __cpuid(r, 1);
    int fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    sw_cpu_fma = fma && osxsave && avx && ((_xgetbv(0) & 6) == 6);
    { const char *e = getenv("SW_NO_FMA"); if (e && *e == '1') sw_cpu_fma = 0; }   /* test the fallback */
#endif
}

#include "sw_build_cfg.h"
/* Chunks built for x86-64-v3 (clang, build.py) execute AVX2/FMA instructions anywhere: on a CPU without them the game
 * would die with an illegal instruction somewhere in the boot. Say so instead. */
static void sw_check_cpu(void)
{
#if SW_CHUNKS_NEED_AVX2 && defined(_WIN32)
    int r[4];
    __cpuid(r, 0);
    int ok = 0;
    if (r[0] >= 7) {
        int f1[4], f7[4];
        __cpuid(f1, 1);
        __cpuidex(f7, 7, 0);
        int osxsave = (f1[2] >> 27) & 1, avx = (f1[2] >> 28) & 1, fma = (f1[2] >> 12) & 1, avx2 = (f7[1] >> 5) & 1;
        ok = osxsave && avx && fma && avx2 && (_xgetbv(0) & 6) == 6;   /* the OS saves the YMM state */
    }
    if (!ok) {
        MessageBoxA(NULL, "This build of the game needs a CPU with AVX2 and FMA (Intel Haswell / AMD Zen or newer).\n\n"
                    "Rebuild it for older CPUs with SW_CHUNK_CC=cl (MSVC) in the launcher's build settings.",
                    "Pokemon Legends: Arceus recomp", MB_OK | MB_ICONERROR);
        exit(1);
    }
#endif
}

int main(int argc, char **argv)
{
    sw_check_cpu();
    SwCpu cpu;
    int i;
    const char *mode = "";
    u64 call_target = 0;
    u64 call_args[8];
    int call_argc = 0;
    int rc = 0;

    memset(&cpu, 0, sizeof(cpu));
    detect_cpu_features();
        /* Progress must survive a hard kill while diagnosing a runaway guest. */
        setvbuf(stdout, NULL, _IONBF, 0);
        /* Initialize debug gating from environment */
        const char *debug_env = getenv("SW_DEBUG");
        g_debug_enabled = (debug_env && *debug_env == '1');
        g_trace_dispatch = getenv("SW_TRACE_DISPATCH") != NULL;
        set_base_dir(argv[0]);
        sw_mods_cli(argc, argv);               /* --no-mods, --mods-dir, --mods-config, --list-mods */
        if (sw_mods_list_requested()) return sw_mods_list();   /* launcher: no guest memory needed */
        cpu.mem = mem_init();
    cpu.thread_id = 1;
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, sw_crash_handler);
#endif
    set_base_dir(argv[0]);

    /* Print build id and executable timestamp so results can never come from
     * a stale binary. */
    {
        char exe_path[2048];
#if defined(_WIN32)
        if (!GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)))   /* the exe's own name (LodeStar Game.exe) */
#endif
            snprintf(exe_path, sizeof(exe_path), "%s/sw_app.exe", g_base_dir);
        struct stat st;
        if (stat(exe_path, &st) == 0) {
            printf("BUILD ID: %s\n", SW_BUILD_ID);
            printf("EXE TIMESTAMP: %llu  (size %llu bytes)\n",
                   (unsigned long long)st.st_mtime,
                   (unsigned long long)st.st_size);
        } else {
            printf("BUILD ID: %s\n", SW_BUILD_ID);
            printf("EXE TIMESTAMP: unknown (stat failed for %s)\n", exe_path);
        }
    }

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--prepare-shaders")) {   /* offline shader preparation (gfx/sw_gfx_glue.c); no game runs */
            int sw_gfx_prepare_shaders(void);
            return sw_gfx_prepare_shaders();
        }
        if (!strcmp(argv[i], "--warm-shaders")) {      /* driver cache only, after a driver change */
            int sw_gfx_warm_shaders(void);
            return sw_gfx_warm_shaders();
        }
        if (!strcmp(argv[i], "--gpu-info")) {          /* GPU/driver identity for the launcher's cache check */
            int sw_gfx_gpu_info(void);
            return sw_gfx_gpu_info();
        }
        if (!strcmp(argv[i], "--audit")) mode = "--audit";
        else if (!strcmp(argv[i], "--call") && i + 1 < argc) {
            char *spec = argv[++i];
            char *tok = strtok(spec, ",");
            call_target = strtoull(tok, NULL, 0);
            while ((tok = strtok(NULL, ",")) != NULL && call_argc < 8)
                call_args[call_argc++] = strtoull(tok, NULL, 0);
            mode = "--call";
        } else if (!strcmp(argv[i], "--run-init")) mode = "--run-init";
        else if (!strcmp(argv[i], "--run-init-entry")) { mode = "--run-init"; g_run_init_entry = 1; }
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) {
            char *spec = argv[++i];
            char *colon = strchr(spec, ':');
            g_watch_addr = strtoull(spec, NULL, 0);
            g_watch_len = colon ? strtoull(colon + 1, NULL, 0) : 8;
            sw_mem_fast_on = 0;
        }
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc) g_step_budget = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--discover") && i + 1 < argc) g_discover_budget = (u32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--max-dispatch") && i + 1 < argc) g_max_dispatch = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) {
            g_trace_file = fopen(argv[++i], "a");
            if (!g_trace_file) { fprintf(stderr, "error: cannot open trace file\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--list-services")) mode = "--list-services";
        else if (!strcmp(argv[i], "--input-test")) mode = "--input-test";
        else if (!strcmp(argv[i], "--real-start")) mode = "--real-start";
        else if (!strcmp(argv[i], "--reloc-audit")) { mode = "--reloc-audit"; g_reloc_audit = 1; }
        else if (!strcmp(argv[i], "-q")) g_verbose = 0;
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if ((!strcmp(argv[i], "--mods-dir") || !strcmp(argv[i], "--mods-config")) && i + 1 < argc) i++;   /* sw_mods_cli */
    }

    if (!strcmp(mode, "--input-test")) {
        SwPadState pad;
        char desc[256];
        int frame;
        sw_input_init();
        printf("controller test: polling for 5 seconds\n");
        for (frame = 0; frame < 300; frame++) {
            sw_input_poll(&pad);
            sw_input_describe(&pad, desc, sizeof(desc));
            printf("\r%-40s LX=%6d LY=%6d RX=%6d RY=%6d", desc, pad.lx, pad.ly, pad.rx, pad.ry);
            fflush(stdout);
            sw_sleep_ms(16);
        }
        printf("\ncontroller connected: %s\n", pad.connected ? "yes" : "no");
        return 0;
    }

    if (load_program(&cpu) != 0) return 2;
    peek_env(&cpu, "after load");
    start_watchdog(&cpu);
    {   /* sw_call fast path unless a per-call debug option is on (SW_CALL_FAST=0 forces it off) */
        const char *cf = getenv("SW_CALL_FAST");
        g_call_fast = !g_trace_dispatch && !g_profile_enabled && !g_max_dispatch && !g_trace_file &&
                      !g_debug_enabled && !getenv("SW_CALL_DUMP") && !(cf && *cf == '0') &&
                      !getenv("SW_WATCHDOG");          /* the watchdog reads g_dispatch_count */
    }
    g_vabort_addr = lookup_symbol_in_all_modules("_ZN2nn4diag6detail10VAbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_RSt9__va_list");
    sj_scan(&cpu);   /* setjmp callers and resume points */
    {   /* direct-index call table (see g_plain_idx); SW_CALL_IDX=0 turns it off */
        const char *ci = getenv("SW_CALL_IDX"), *ls = getenv("SW_LOCKSTEP");
        if (g_call_fast && !(ci && *ci == '0') && !(ls && *ls && *ls != '0')) {
            u8 *pl = (u8 *)calloc(sw_function_count ? sw_function_count : 1, 1);
            u32 n = 0;
            for (u32 i = 0; pl && i < sw_function_count; i++) {
                u64 a = sw_functions[i].addr;
                pl[i] = sw_functions[i].fn && a < 0xFF000000ULL && redirect(a) == a && a != g_longjmp_addr &&
                        a != 0x4988324ULL && a != g_vabort_addr && !(g_sj_ncallers && sj_is_caller(a));
                n += pl[i];
            }
            for (int k = 0; pl && k < g_nhooked; k++)   /* hooked before the table existed */
                if (g_hooked[k] < sw_function_count && pl[g_hooked[k]]) pl[g_hooked[k]] = 0, n--;
            if (g_nhooked < SW_HOOKED_MAX) g_plain_idx = pl;
            {   const char *d = getenv("SW_DIRECT"), *ic = getenv("SW_IC");
                g_direct_idx = (d && *d == '0') || sw_newfn_mode ? NULL : g_plain_idx;
                g_ic_on = !(ic && *ic == '0');
                sw_report("direct calls %s, indirect-call cache %s\n", g_direct_idx ? "on" : "off (SW_DIRECT=0)",
                          g_direct_idx && g_ic_on ? "on" : "off"); }
            sw_report("call index: %u of %u functions on the direct path\n", n, sw_function_count);
        }
    }

        /* Initialize window after program load, before entering game loop */
        sw_report("debug: initializing window...\\n");
        /* With the Vulkan back end (Engine/Runtime/gfx/vk) the graphics layer
         * opens its own window at the first present; the OpenGL window is
         * only for SW_GPU=null (it shows the clear colour) or SW_GPU=gl. */
        int sw_gfx_uses_vulkan(void);
        int win_rc = sw_gfx_uses_vulkan() ? 0 : sw_window_init(1280, 720, "Pokémon Legends: Arceus");
        sw_report("debug: sw_window_init returned %d\\n", win_rc);
        if (win_rc != 0) {
            sw_report("warning: failed to create window, continuing headless\\n");
        }

        if (!strcmp(mode, "--audit")) {
        audit_report(&cpu);
        return 0;
    }
    if (!strcmp(mode, "--list-services")) {
        for (i = 0; i < (int)sw_native_count; i++)
            printf("%-64s 0x%llx\n", sw_native_entries[i].name,
                   (unsigned long long)sw_native_entries[i].addr);
        return 0;
    }

    g_trap_armed = 1;
    if (setjmp(g_trap_env) != 0) {
        printf("=== execution stopped ===\n");
        /* halt_pc is the *data address* that faulted: sw_trap() is handed the
         * access address, not an instruction address.  cpu.pc is the guest
         * instruction, and cur_fn the function containing it. */
        printf("reason   : %s\n", cpu.halt_reason ? cpu.halt_reason : "(none)");
        printf("fault at : 0x%llx\n", (unsigned long long)cpu.halt_pc);
        printf("insn     : 0x%llx\n", (unsigned long long)cpu.pc);
        printf("function : 0x%llx (caller 0x%llx)\n",
               (unsigned long long)cpu.cur_fn, (unsigned long long)cpu.prev_fn);
        peek_env(&cpu, "at stop");
        dump_trace();
        audit_report(&cpu);
        sw_window_shutdown();
        return 3;
    }

    if (!strcmp(mode, "--call")) {
        const SwFuncEntry *fn = find_function(call_target);
        if (!fn) {
            sw_report("error: no recompiled function at 0x%llx\n", (unsigned long long)call_target);
            return 2;
        }
        printf("calling %s @ 0x%llx\n", fn->name, (unsigned long long)call_target);
        /* Game loop: call the entry point repeatedly, incrementing x0 each frame.
         * The main loop (0x4987ff8) checks x0 at entry; non-zero means "keep running". */
        int frame = 0;
        while (frame < g_max_dispatch / 10 && !cpu.halted) {
            for (i = 0; i < call_argc; i++) cpu.x[i] = call_args[i];
            for (i = call_argc; i < 8; i++) cpu.x[i] = 0;
            cpu.x[0] = 0;  /* entry arg: x0=0 passed to main's entry; init_array walker uses it */
            cpu.x[20] = 0; /* x20: table pointer base for init_array walker */
            fn->fn(&cpu);
            frame++;
            fprintf(stderr, "FRAME %d completed, halted=%d\n", frame, cpu.halted);
            fflush(stderr);
            if (cpu.halted) break;
        }
        printf("returned%s after %d frames\n", cpu.halted ? " (halted)" : "", frame);
                print_regs(&cpu);
                dump_trace();
                audit_report(&cpu);
                sw_window_shutdown();
                return cpu.halted ? 3 : 0;
    }

    if (!strcmp(mode, "--run-init")) {
        u64 total = 0;
        u32 pass;
        int stop = 0;
        /* Constructors run dependency-first.  sw_modules is in load order
         * (main first, then the modules it imports), so walk it backwards:
         * the synthetic `imports` table has no constructors and is skipped
         * naturally by its zero init_array_count. */
        for (pass = sw_module_count; pass-- > 0 && !stop; ) {
            u32 mi = pass;
            const SwModuleDesc *m = &sw_modules[mi];
            u64 k;
            printf("module %s: %llu init entries\n", m->name,
                   (unsigned long long)m->init_array_count);
            /* On the console the loader calls the module's init function, which is
             * what runs its static constructors; the array alone is only the list
             * it walks.  Dispatch it first so both paths can be compared. */
            if (g_run_init_entry && strcmp(m->name, "imports") != 0) {
                u64 mod_init = m->entry ? m->entry : m->base;
                printf("  module init function -> 0x%llx\n", (unsigned long long)mod_init);
                sw_dispatch(&cpu, mod_init);
                if (cpu.halted) { stop = 1; break; }
            }
            for (k = 0; k < m->init_array_count; k++) {
                u64 entry = 0;
                memcpy(&entry, mem_host(&cpu, m->init_array + k * 8, 8, 0), 8);
                if (entry == 0) continue;
                printf("  init[%llu] -> 0x%llx\n", (unsigned long long)k, (unsigned long long)entry);
                sw_dispatch(&cpu, entry);
                total++;
                if (g_step_budget && total >= g_step_budget) {
                    printf("step budget reached (%llu)\n", (unsigned long long)g_step_budget);
                    stop = 1;
                    break;
                }
                if (cpu.halted) { stop = 1; break; }
            }
        }
        audit_report(&cpu);
        sw_window_shutdown();
        return cpu.halted ? 3 : 0;
    }

    if (!strcmp(mode, "--real-start")) {
        /* Real process startup: set up Horizon-like initial state, run
         * init_array for all modules in dependency order, then enter nnMain. */
        printf("=== REAL STARTUP ===\n");

        /* Log initial register state */
        printf("ENTRY: initial register state\n");
        printf("  X0 (loader config) = 0x%llx (0 = no config)\n",
               (unsigned long long)cpu.x[0]);
        printf("  X1 (main thread handle) = 0x%llx\n",
               (unsigned long long)cpu.x[1]);
        printf("  SP = 0x%llx\n", (unsigned long long)cpu.sp);
        printf("  TPIDRRO_EL0 = 0x%llx\n", (unsigned long long)cpu.tpidrro_el0);
        printf("  TPIDR_EL0 = 0x%llx\n", (unsigned long long)cpu.tpidr_el0);
        printf("  thread_id = %llu\n", (unsigned long long)cpu.thread_id);

        /* X0 = 0 (no loader config needed; the code doesn't read it),
         * X1 = thread handle (already = thread_id) */
        cpu.x[0] = 0;
        cpu.x[1] = cpu.thread_id ? cpu.thread_id : 1;
        printf("ENTRY: X0=0x%llx X1=0x%llx SP=0x%llx TPIDRRO_EL0=0x%llx TPIDR_EL0=0x%llx\n",
               (unsigned long long)cpu.x[0], (unsigned long long)cpu.x[1],
               (unsigned long long)cpu.sp,
               (unsigned long long)cpu.tpidrro_el0,
               (unsigned long long)cpu.tpidr_el0);

        /* __nnmusl_init: not present as a dynsym export in any module.
         * It resolves to the __nnmusl_init_dso native thunk at 0xff000000
         * (svc_noop). Log it as a stage. */
        printf("__nnmusl_init: resolving to native thunk 0xff000000 (svc_noop)\n");
        sw_dispatch(&cpu, 0xff000000ULL);
        if (cpu.halted) {
            printf("__nnmusl_init: HALTED at 0x%llx: %s\n",
                   (unsigned long long)cpu.halt_pc,
                   cpu.halt_reason ? cpu.halt_reason : "(none)");
            sw_window_shutdown();
            return 3;
        }
        printf("__nnmusl_init: returned X0=0x%llx\n",
               (unsigned long long)cpu.x[0]);

        /* nnosInitialize(loader_arg, main_thread_handle): the SDK startup
         * sets up nn::os (ThreadManager, TLS slots, ...) before any static
         * constructor runs; sdk constructors use it. */
        /* x0 is stored into nn::os::detail::g_OsBootParamter (main thread
         * handle, copied into ThreadType+0x1b0 and used as the mutex owner
         * tag).  It was 0, so every fresh mutex looked "locked by current
         * thread" and SdkMutexType::Lock aborted in nn::oe::Initialize. */
        sw_threads_init(&cpu);
        sw_mods_start_cheats();                /* cheat VM timer thread (only when a cheat is enabled) */
        if (!run_named_stage(&cpu, "nnosInitialize", 0x8001, cpu.thread_id ? cpu.thread_id : 1)) {
            sw_window_shutdown();
            return 3;
        }
        /* Programs with a system resource budget use nn::os virtual address
         * memory (the game's StandardAllocator reserves 32 GB through it);
         * the VammManager must be initialised before nninitStartup. */
        if (!run_named_stage(&cpu, "_ZN2nn2os30InitializeVirtualAddressMemoryEv", 0, 0)) {
            sw_window_shutdown();
            return 3;
        }

        /* SDK order: nninitStartup (the app sets up its heap/allocator) and
         * nninitInitializeSdkModule run BEFORE static constructors, which
         * allocate through that allocator (a constructor called a null
         * allocator vtable when this ran afterwards). */
        /* Call nninitStartup first (fallback path per Phase 1 step 4) */
        {
            const u64 nninitStartup_addr = 0x0283cf10ULL; /* main+0x0273cf10 */
            const SwFuncEntry *fn = find_function(nninitStartup_addr);
            if (!fn) {
                printf("nninitStartup: no recompiled function at 0x%llx\n",
                       nninitStartup_addr);
                sw_window_shutdown();
                return 2;
            }
            printf("nninitStartup: calling 0x%llx\n", nninitStartup_addr);
            cpu.x[0] = 0;  /* no arguments expected */
            cpu.x[1] = 0;
            sw_call(&cpu, nninitStartup_addr);
            if (cpu.halted) {
                printf("nninitStartup: HALTED at 0x%llx: %s\n",
                       (unsigned long long)cpu.halt_pc,
                       cpu.halt_reason ? cpu.halt_reason : "(none)");
                sw_window_shutdown();
                return 3;
            }
            printf("nninitStartup: returned X0=0x%llx\n",
                   (unsigned long long)cpu.x[0]);
        }

        /* nninitInitializeSdkModule: SDK module setup between nninitStartup
         * and nnMain. */
        if (!run_named_stage(&cpu, "nninitInitializeSdkModule", 0, 0)) {
            sw_window_shutdown();
            return 3;
        }

        /* Module constructors.  On the console rtld calls every module's
         * DT_INIT (main, the subsdks, sdk), which registers the module with
         * musl (__nnmusl_init_dso: its TLS template among others) and then
         * runs its init_array.  Walking init_array directly skipped that
         * registration, so thread_local variables with initial values read
         * zero (a RapidJSON pool allocator pointer at tp+0x10 was NULL).
         * SW_INIT=array keeps the old walk. */
        u64 total = 0;
        u32 pass;
        int stop = 0;
        const char *init_mode = getenv("SW_INIT");
        if (!init_mode || strcmp(init_mode, "array") != 0) {
            static const char *order[] = {"main", "subsdk0", "subsdk1", "subsdk2", "sdk"};
            int ran = 0;
            for (u32 o = 0; o < sizeof order / sizeof order[0] && !stop; o++) {
                for (u32 mi = 0; mi < sw_module_count; mi++) {
                    const SwModuleDesc *m = &sw_modules[mi];
                    if (strcmp(m->name, order[o]) != 0) continue;
                    u64 init = module_dt_init(&cpu, m->base);
                    if (!init) { printf("  %s: no DT_INIT\n", m->name); continue; }
                    printf("  DT_INIT %s -> 0x%llx (%llu init_array entries)\n", m->name,
                           (unsigned long long)init, (unsigned long long)m->init_array_count);
                    cpu.x[0] = 0;
                    sw_dispatch(&cpu, init);
                    total += m->init_array_count;
                    ran++;
                    if (cpu.halted) { stop = 1; break; }
                }
            }
            if (ran) pass = 0;                     /* skip the init_array walk */
            else pass = sw_module_count;
        } else {
            pass = sw_module_count;
        }
        for (; pass-- > 0 && !stop; ) {
            u32 mi = pass;
            const SwModuleDesc *m = &sw_modules[mi];
            u64 k;
            for (k = 0; k < m->init_array_count; k++) {
                u64 entry = 0;
                memcpy(&entry, mem_host(&cpu, m->init_array + k * 8, 8, 0), 8);
                if (entry == 0) continue;
                printf("  init[%s:%llu] -> 0x%llx\n", m->name,
                       (unsigned long long)k, (unsigned long long)entry);
                sw_dispatch(&cpu, entry);
                total++;
                if (cpu.halted) { stop = 1; break; }
            }
        }
        printf("init_array: %llu constructors dispatched\n",
               (unsigned long long)total);

        /* Call nnMain */
        {
            const u64 nnMain_addr = 0x0283d160ULL; /* main+0x0273d160 */
            const SwFuncEntry *fn = find_function(nnMain_addr);
            if (!fn) {
                printf("nnMain: no recompiled function at 0x%llx\n",
                       nnMain_addr);
                sw_window_shutdown();
                return 2;
            }
            printf("nnMain: calling 0x%llx\n", nnMain_addr);
            cpu.x[0] = 0;
            cpu.x[1] = 0;
            sw_call(&cpu, nnMain_addr);
            if (cpu.halted) {
                printf("nnMain: HALTED at 0x%llx: %s\n",
                       (unsigned long long)cpu.halt_pc,
                       cpu.halt_reason ? cpu.halt_reason : "(none)");
                sw_window_shutdown();
                return 3;
            }
            printf("nnMain: returned X0=0x%llx\n",
                   (unsigned long long)cpu.x[0]);
        }

        audit_report(&cpu);
        sw_window_shutdown();
        return cpu.halted ? 3 : 0;
    }

    /* Default mode (no flag): run the rendering loop until the window closes */
    if (mode[0] == '\0') {
        sw_report("debug: entering render loop (window open=%d)\n", sw_window_is_open());
        sw_window_set_test_pattern(1);
        while (sw_window_is_open()) {
            sw_window_present();
            sw_window_pump();
            sw_sleep_ms(16);  /* ~60 FPS cap */
        }
        sw_window_shutdown();
        audit_report(&cpu);
        return 0;
    }

    usage(argv[0]);
    return rc;
    }

/* PGO (build.py SW_PGO=gen): chunks instrumented by clang write their counters with __llvm_profile_write_file
 * (clang_rt.profile).  In a normal build the name resolves to the no-op below (/alternatename), so this links
 * either way.  Called when a benchmark route ends (sw_hid.c): the game may not leave through the CRT exit. */
#if defined(_MSC_VER)
int sw_pgo_write_nop(void) { return -1; }
int __llvm_profile_write_file(void);
#pragma comment(linker, "/alternatename:__llvm_profile_write_file=sw_pgo_write_nop")
void sw_pgo_write(void)
{
    if ((void *)__llvm_profile_write_file == (void *)sw_pgo_write_nop) return;
    sw_report("[pgo] profile written (%d)", __llvm_profile_write_file());
    sw_report("%c", 10);
}
#else
void sw_pgo_write(void) {}
#endif
