/* Guest threads: cooperative scheduling on host fibers.
 *
 * Every guest thread created with svcCreateThread gets its own SwCpu, TLS
 * page and host fiber.  Exactly one fiber runs at a time; a switch happens
 * only inside a blocking syscall (lock / condition variable / address
 * arbitration waits, sleep, waiting on a thread handle, thread exit).  The
 * runtime's globals therefore never see concurrent access, and runs stay
 * deterministic and debuggable.
 *
 * Kernel semantics follow Horizon:
 *   mutex word   = owner handle | 0x40000000 (waiters bit)
 *   ArbitrateLock(owner, addr, tag)   sleep until the lock is handed over
 *   ArbitrateUnlock(addr)             hand over to one waiter (or clear)
 *   WaitProcessWideKeyAtomic(addr, key, tag, timeout)
 *                                     release addr, sleep on key, relock
 *   SignalProcessWideKey(key, count)  wake count (or all if <= 0) sleepers
 *   WaitForAddress / SignalToAddress  the address arbiter (types 0..2)
 * Thread handles are 0x9000 + index; the main thread is index 0.
 */
#include "sw_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#include <intrin.h>   /* _mm_pause */
#endif

void sw_report(const char *fmt, ...);
int sw_mem_map(SwCpu *c, u64 addr, u64 size, const void *data, u64 data_size);
u8 *sw_mem_ptr(SwCpu *c, u64 addr, u64 size, int write);
u64 sw_tick_now(void);

#define MAX_THREADS     256
#define THREAD_HANDLE   0x9000u
#define TLS_REGION      0x6F000000ull       /* per-thread TLS pages (64 KB each), below the stack region */
#define WAITERS_BIT     0x40000000u
#define RES_TIMEOUT     0xEA01u
#define RES_INVALID     0xE401u

enum { T_FREE, T_CREATED, T_READY, T_BLOCKED, T_DEAD };
enum { W_NONE, W_LOCK, W_CV, W_ADDR, W_THREAD, W_SLEEP, W_SYNC };
#define RES_CANCELLED   0xEC01u

typedef struct {
    int state;
    SwCpu *cpu;
    void *fiber;
    u64 entry, arg;
    int wait;               /* W_* */
    u64 key;                /* mutex / cv key / address / thread index */
    u32 tag;                /* lock tag to take over (W_LOCK) */
    u64 deadline;           /* 0 = none, else sw_tick_now() value */
    int woken;              /* 1 = signalled, 2 = timed out, 3 = cancelled */
    int cancel_pending;     /* CancelSynchronization before the thread waited */
    u64 cv_mutex; u32 cv_tag;  /* condition variable: mutex to retake */
    char name[32];
#if defined(_WIN32)
    HANDLE ev;              /* SW_MULTICORE: auto-reset event a blocked thread waits on */
    HANDLE host;            /* SW_MULTICORE: the host thread */
#endif
    int core;               /* emulated core the game wants (-1 any) */
    int prio;               /* Horizon priority (lower = more urgent) */
    int core_cur;           /* core held now (-1 none) */
    int core_wait;          /* waiting for a core */
    int core_over;          /* runs on core_cur without owning it (100 ms escape below) */
    volatile long spinning; /* SW_MULTICORE: spinning in mc_wait before it sleeps (a wake needs no SetEvent) */
    int waker;              /* SW_WAIT_STATS: the guest thread whose wake() ended the last wait */
} GThread;

#if defined(_MSC_VER)
#define SW_TLS __declspec(thread)
#else
#define SW_TLS _Thread_local
#endif
static GThread g_t[MAX_THREADS];
static volatile int g_t_hi = 1;       /* one past the highest slot ever used: the slots above are T_FREE, so scans stop here */
static SW_TLS int g_cur;               /* running thread index (per host thread) */
static SW_TLS int g_guest;             /* 1 on a host thread that runs guest threads (g_cur means something) */

/* ---- SW_MULTICORE: guest threads on host threads, one kernel lock ---- */
static int g_mc;
#if defined(_WIN32)
static CRITICAL_SECTION g_kcs;
#endif
static SW_TLS int t_kdepth;
/* SW_THREAD_STATS=1 in multi-core mode: 19.2 MHz ticks per guest thread (index MAX_THREADS-1 and up: unused) */
#define MC_NT 64
static int g_mc_stats;
static volatile u64 g_mc_lockwait[MC_NT], g_mc_lockhold[MC_NT], g_mc_corewait[MC_NT], g_mc_blocked[MC_NT];
static volatile u64 g_mc_nwait[MC_NT], g_mc_wakelat[MC_NT], g_mc_nwake[MC_NT];   /* waits; SetEvent -> running */
static volatile u64 g_mc_wake_at[MC_NT];
static volatile u64 g_mc_nspin[MC_NT];                                             /* waits ended by the spin */                                           /* tick of the last wake */
static SW_TLS u64 t_khold_since;
static SW_TLS u64 t_knote;            /* svc (0x5C000000|n) or native thunk address holding the lock */
void sw_klock_note(u64 what) { t_knote = what; }
/* a hold over 50 ms stalls every other thread: name it (SW_THREAD_STATS=1) */
/* kernel lock hold time by what held it (SW_THREAD_STATS): svc n -> slot n (0..0x7f), native thunks hashed into 0x80.. */
#define KH_SLOTS 256
static volatile u64 g_kh_ticks[KH_SLOTS], g_kh_n[KH_SLOTS], g_kh_what[KH_SLOTS];
static void kh_note(u64 ticks)
{
    u64 w = t_knote;
    int k = (w >> 24) == 0x5C ? (int)(w & 0x7F) : 0x80 + (int)(((w >> 3) * 2654435761u) >> 25 & 0x7F);
    g_kh_what[k] = w;
    InterlockedAdd64((volatile LONG64 *)&g_kh_ticks[k], (LONG64)ticks);
    InterlockedIncrement64((volatile LONG64 *)&g_kh_n[k]);
}
static void kh_report(void)
{
    static u64 p_t[KH_SLOTS], p_n[KH_SLOTS];
    u64 d[KH_SLOTS];
    for (int k = 0; k < KH_SLOTS; k++) { u64 t = g_kh_ticks[k]; d[k] = t - p_t[k]; p_t[k] = t; }
    sw_report("[mc] kernel lock held by:");
    for (int r = 0; r < 8; r++) {
        int best = -1;
        for (int k = 0; k < KH_SLOTS; k++) if (d[k] && (best < 0 || d[k] > d[best])) best = k;
        if (best < 0) break;
        u64 n = g_kh_n[best] - p_n[best];
        u64 w = g_kh_what[best];
        extern const char *sw_nvn_proc_name(u64 addr);
        const char *nm = (w >> 24) == 0x5C ? NULL : sw_nvn_proc_name(w);
        if ((w >> 24) == 0x5C) sw_report(" svc 0x%llx %llu ms (%llu)", (unsigned long long)(w & 0xFFFFFF), (unsigned long long)(d[best] / 19200), (unsigned long long)n);
        else sw_report(" thunk 0x%llx%s%s %llu ms (%llu)", (unsigned long long)w, nm ? " " : "", nm ? nm : "", (unsigned long long)(d[best] / 19200), (unsigned long long)n);
        d[best] = 0;
    }
    for (int k = 0; k < KH_SLOTS; k++) p_n[k] = g_kh_n[k];
    {   extern volatile long long g_arb_fast_n, g_arb_slow_n; static long long pf, ps;
        long long f = g_arb_fast_n, sl = g_arb_slow_n;
        sw_report("; ArbitrateLock without the kernel lock %lld, with it %lld", f - pf, sl - ps); pf = f; ps = sl; }
    {   extern volatile long long g_sj_longjmps; static long long pj;
        long long j = g_sj_longjmps;
        sw_report("; guest longjmps %lld", j - pj); pj = j; }
    sw_report("\n");
}
static void mc_long_hold(u64 ticks)
{
    if (ticks < 19200 * 50) return;
    extern const char *sw_nvn_proc_name(u64 addr);
    const char *nm = (t_knote >> 24) == 0x5C ? NULL : sw_nvn_proc_name(t_knote);
    sw_report("[mc] thread %d held the kernel lock %llu ms in %s 0x%llx %s\n", g_cur, (unsigned long long)(ticks / 19200),
              (t_knote >> 24) == 0x5C ? "svc" : "thunk", (unsigned long long)((t_knote >> 24) == 0x5C ? t_knote & 0xFFFFFF : t_knote),
              nm ? nm : "");
}
static int mc_ix(void) { return g_cur >= 0 && g_cur < MC_NT ? g_cur : 0; }
void sw_klock(void)
{
#if defined(_WIN32)
    if (!g_mc) return;
    if (g_mc_stats && !t_kdepth) {
        if (!TryEnterCriticalSection(&g_kcs)) {
            u64 t0 = sw_tick_now();
            EnterCriticalSection(&g_kcs);
            g_mc_lockwait[mc_ix()] += sw_tick_now() - t0;
        }
        t_khold_since = sw_tick_now();
    } else
        EnterCriticalSection(&g_kcs);
    t_kdepth++;
#endif
}
void sw_kunlock(void)
{
#if defined(_WIN32)
    if (!g_mc) return;
    t_kdepth--;
    if (g_mc_stats && !t_kdepth) {
        u64 h = sw_tick_now() - t_khold_since;
        g_mc_lockhold[mc_ix()] += h;
        mc_long_hold(h);
        kh_note(h);
    }
    LeaveCriticalSection(&g_kcs);
#endif
}
/* leave the kernel lock completely (a wait), then take it back as deep as before */
static int klock_drop(void)
{
    int d = t_kdepth;
    if (g_mc_stats && d) g_mc_lockhold[mc_ix()] += sw_tick_now() - t_khold_since;
#if defined(_WIN32)
    for (int i = 0; i < d; i++) LeaveCriticalSection(&g_kcs);
#endif
    t_kdepth = 0;
    return d;
}
static void klock_take(int d)
{
#if defined(_WIN32)
    if (g_mc_stats && d) {
        u64 t0 = sw_tick_now();
        for (int i = 0; i < d; i++) EnterCriticalSection(&g_kcs);
        t_khold_since = sw_tick_now();
        g_mc_lockwait[mc_ix()] += t_khold_since - t0;
    } else
        for (int i = 0; i < d; i++) EnterCriticalSection(&g_kcs);
#endif
    t_kdepth = d;
}
int sw_multicore(void) { return g_mc; }
/* this guest thread's blocked and kernel-lock-wait time so far in ms (SW_THREAD_STATS=1; 0 otherwise): SW_THUNK_SLOW */
void sw_thread_wait_ms(double *blocked, double *lockwait)
{
    int i = g_cur >= 0 && g_cur < MC_NT ? g_cur : 0;
    *blocked = (double)g_mc_blocked[i] / 19200.0, *lockwait = (double)g_mc_lockwait[i] / 19200.0;
}
/* a svc that does slow host work (MapPhysicalMemory commits) leaves the kernel lock meanwhile: other guest threads go on */
int sw_klock_leave(void) { return g_mc ? klock_drop() : 0; }
void sw_klock_return(int d) { if (g_mc) klock_take(d); }
static void mc_yield(void);
static void mc_wait(GThread *t);
static int g_spin_us = 1000;           /* SW_SPIN_US (multi-core) */
#if defined(_WIN32)
static DWORD WINAPI hang_watch(LPVOID arg);   /* SW_HANG_DUMP */
#endif

static int g_inited;                  /* (defined below; tentative definition) */
/* ---- emulated cores (kernel lock held) ---- */
#define MAX_CORES 16
static int g_ncores = 3;
static int g_core_pin = 1;                   /* SW_CORE_PIN=0: ignore the game's core choice, take any free core */
static int g_core_owner[MAX_CORES];          /* thread index, -1 free */
static volatile long g_core_waiters;         /* threads waiting for any core (for the time slice) */

static void core_release(int me)
{
    GThread *t = &g_t[me];
    int c = t->core_cur;
    if (c < 0) return;
    t->core_cur = -1;
    if (t->core_over) { t->core_over = 0; return; }   /* never owned it: the owner keeps its core */
    int best = -1;                            /* hand the core over: best priority, then oldest index */
    for (int i = 0; i < g_t_hi; i++)
        if (g_t[i].core_wait && (!g_core_pin || g_t[i].core == c || g_t[i].core < 0) &&
            (best < 0 || g_t[i].prio < g_t[best].prio)) best = i;
    g_core_owner[c] = best;                   /* reserved for it (or free) */
#if defined(_WIN32)
    if (best >= 0 && g_t[best].ev) SetEvent(g_t[best].ev);
#endif
}

static void core_acquire(int me)
{
    GThread *t = &g_t[me];
    if (t->core_cur >= 0) return;
    u64 since = sw_tick_now();
    for (;;) {
        int pick = -1;
        int want = g_core_pin && t->core >= 0 && t->core < g_ncores ? t->core : -1;
        if (want >= 0) {
            if (g_core_owner[want] < 0 || g_core_owner[want] == me) pick = want;
        } else {
            for (int k = 0; k < g_ncores && pick < 0; k++) if (g_core_owner[k] == me) pick = k;
            for (int k = 0; k < g_ncores && pick < 0; k++) if (g_core_owner[k] < 0) pick = k;
        }
        u64 waited = sw_tick_now() - since;
        if (pick < 0 && want >= 0 && waited > 19200 * 20)      /* 20 ms on its core: any free core */
            for (int k = 0; k < g_ncores && pick < 0; k++) if (g_core_owner[k] < 0) pick = k;
        if (pick >= 0) {
            g_core_owner[pick] = me;
            t->core_cur = pick;
            break;
        }
        if (waited > 19200 * 100) {           /* 100 ms: the holder never lets go (a spin without calls) - run anyway */
            t->core_cur = want >= 0 ? want : 0;
            t->core_over = 1;                 /* without taking g_core_owner: release must not free the owner's core */
            break;
        }
        t->core_wait = 1;
        InterlockedIncrement(&g_core_waiters);
        int d = klock_drop();
#if defined(_WIN32)
        WaitForSingleObject(t->ev, 5);
#endif
        klock_take(d);
        InterlockedDecrement(&g_core_waiters);
        t->core_wait = 0;
    }
    if (g_mc_stats && me < MC_NT) g_mc_corewait[me] += sw_tick_now() - since;
}

/* Graphics host waits (gfx/compat/host.h): the guest thread gives up its emulated
 * core and the kernel lock, so the other guest threads run meanwhile.  Before this
 * a GPU wait or shader translation (up to 200 ms) stalled every thread. */
int kern_core_away(void)
{
#if defined(_WIN32)
    static int nvn_locked = -1;   /* SW_NVN_LOCK=1: graphics keep the kernel lock, so keep the core too (no lock-order cycle) */
    if (nvn_locked < 0) { const char *e = getenv("SW_NVN_LOCK"); nvn_locked = e && *e == '1'; }
    /* a host thread of the graphics code (shader prewarm workers, translators) has g_cur 0 by default: giving up
     * thread 0's core from there made 16 prewarm threads fight over the main thread's core (boot hang) */
    if (nvn_locked || !g_guest || !g_mc || !g_inited || !g_t[g_cur].cpu || g_t[g_cur].core_cur < 0) return 0;
    int d = klock_drop();                     /* normally 0: NVN procs run without it */
    klock_take(1);
    core_release(g_cur);
    klock_drop();
    return d + 1;                             /* > 0: the core was given up */
#else
    return 0;
#endif
}
void kern_core_back(int *p)
{
#if defined(_WIN32)
    if (!p || *p <= 0) return;
    int d = *p - 1;
    *p = 0;
    klock_take(1);
    core_acquire(g_cur);
    klock_drop();
    klock_take(d);
#else
    (void)p;
#endif
}

/* the emulated core the calling guest thread runs on (GetCurrentProcessorNumber) */
/* GetCurrentCoreNumber.  The game keeps per-core data indexed by it (e.g. the
 * font engine instances of the glyph cache: main+0x221cb4 -> nn::os::
 * GetCurrentCoreNumber -> ScalableFontEngine), and on the Switch only the
 * threads the game put on core k (CreateThread / SetThreadCoreMask) ever run
 * there.  So report the thread's own core (0..2) whatever emulated core it
 * runs on: with "the emulated core % 3" (SW_CORES > 3), threads of different
 * cores got the same number at the same time and shared one font engine
 * (access violation in map_font, 2026-11-10).  A thread without an ideal core
 * keeps the emulated core % 3 (always in bounds). */
static volatile long g_core_id_calls;
static volatile long g_core_id_by[MAX_THREADS];   /* per guest thread, for SW_THREAD_STATS */
int sw_thread_core(void)
{
    if (!g_inited || !g_mc) return 0;
    InterlockedIncrement(&g_core_id_calls);
    InterlockedIncrement(&g_core_id_by[g_cur]);
    int ideal = g_t[g_cur].core;
    if (ideal >= 0 && ideal < 3) return ideal;
    int c = g_t[g_cur].core_cur;
    return c < 0 ? 0 : c % 3;
}

/* SetThreadCoreMask(handle, ideal core, mask): remember the ideal core */
void sw_thread_set_core(u32 handle, int ideal)
{
    int i = handle == 0xFFFF8000u ? g_cur : (handle >= THREAD_HANDLE && handle < THREAD_HANDLE + MAX_THREADS)
            ? (int)(handle - THREAD_HANDLE) : -1;
    if (i < 0 || g_t[i].state == T_FREE) return;
    if (ideal >= 0 && ideal < g_ncores) g_t[i].core = ideal;
    else if (ideal == -1) g_t[i].core = -1;
}
static int g_inited;
static u64 g_switches;
static u64 g_cpu_ticks[MAX_THREADS];   /* run time per guest thread, idle sleeps excluded (SW_THREAD_STATS) */
static u64 g_slice_start;
static u64 g_runs[MAX_THREADS];   /* times each thread was switched to (SW_THREAD_STATS) */

/* CPU state of the guest thread running now (graphics layer: kern_cpu). */
SwCpu *sw_thread_current_cpu(void)
{
    return g_inited ? g_t[g_cur].cpu : NULL;
}

static int thread_of(SwCpu *c)
{
    for (int i = 0; i < g_t_hi; i++)
        if (g_t[i].state != T_FREE && g_t[i].cpu == c) return i;
    return 0;
}

void sw_threads_init(SwCpu *main_cpu)
{
    if (g_inited) return;
    g_inited = 1;
    SetThreadDescription(GetCurrentThread(), L"game main (guest-0)");   /* the performance overlay names threads */
    {
        const char *e = getenv("SW_MULTICORE");
        g_mc = e && *e == '1';
#if defined(_WIN32)
#if defined(_WIN32)
        { const char *hd = getenv("SW_HANG_DUMP");
          if (hd && atoi(hd) > 0) CreateThread(NULL, 0, hang_watch, (LPVOID)(intptr_t)atoi(hd), 0, NULL); }
#endif
        if (g_mc) {   /* SW_KLOCK_SPIN: spin count of the kernel lock before it sleeps (default 2000) */
            const char *ks = getenv("SW_KLOCK_SPIN");
            InitializeCriticalSectionAndSpinCount(&g_kcs, ks && atoi(ks) >= 0 ? (DWORD)atoi(ks) : 2000);
        }
        {   /* SW_SPIN_US: how long a blocked guest thread spins before it sleeps (0 = never; default 1000).
             * The game hands jobs between its threads ~27,000 times a second on the main thread alone; a
             * sleep + SetEvent wake costs several us each time, a spin that catches the wake costs ~0. */
            const char *sp = getenv("SW_SPIN_US");
            g_spin_us = sp ? atoi(sp) : 1000;   /* 2026-11-16 sweeps: frames over 8.3 ms 899 (30 us), 362 (400), 281 (1000) */
            if (!sp) {   /* small CPUs: a spinning waiter takes a core from a thread with work (2026-11-25, 4 logical
                          * CPUs: 1000 us 134.9 fps / 2213 frames over 8.3 ms, 200 us 141.9 / 1638, 50 us 140.0 / 1582) */
                DWORD_PTR pm = 0, sm = 0;   /* the CPUs this process may use (an affinity limit counts) */
                int ncpu = GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) ? (int)__popcnt64((unsigned __int64)pm) : 16;
                if (ncpu <= 4) g_spin_us = 50;
                else if (ncpu <= 8) g_spin_us = 200;
            }
            if (g_spin_us < 0) g_spin_us = 0;
            if (g_spin_us > 1000) g_spin_us = 1000;
        }
        { extern int sw_mc_fence; sw_mc_fence = g_mc; }   /* DMB = real fence (sw_runtime.c) */
#else
        g_mc = 0;
#endif
        const char *nc = getenv("SW_CORES");
        if (nc && atoi(nc) > 0) g_ncores = atoi(nc) > MAX_CORES ? MAX_CORES : atoi(nc);
#if defined(_WIN32)
        else if (nc && !_stricmp(nc, "auto")) {   /* as the launcher's "auto": every logical CPU, 3..16 (atoi gave 0 -> 3) */
            SYSTEM_INFO si; GetSystemInfo(&si);
            int n = (int)si.dwNumberOfProcessors;
            g_ncores = n < 3 ? 3 : n > MAX_CORES ? MAX_CORES : n;
        }
#endif
        for (int k = 0; k < MAX_CORES; k++) g_core_owner[k] = -1;
        { const char *cp = getenv("SW_CORE_PIN"); if (cp && *cp == '0') g_core_pin = 0; }
        { const char *st = getenv("SW_THREAD_STATS"); g_mc_stats = g_mc && st && *st == '1'; }
        if (g_mc) sw_report("[thread] SW_MULTICORE=1: guest threads run on host threads, %d emulated cores%s, spin %d us\n", g_ncores, g_core_pin ? "" : ", core pinning off", g_spin_us);
        if (g_mc) { void host_spread_main(void); host_spread_main(); }   /* the main guest thread is this host thread */
    }
    memset(g_t, 0, sizeof g_t);
    g_t[0].state = T_READY;
    g_t[0].cpu = main_cpu;
#if defined(_WIN32)
    g_t[0].fiber = ConvertThreadToFiber(NULL);
#endif
    strcpy(g_t[0].name, "main");
    g_cur = 0;
    g_guest = 1;
    g_t[0].core = 0;
    g_t[0].prio = 44;
    g_t[0].core_cur = -1;
    if (g_mc) { g_core_owner[0] = 0; g_t[0].core_cur = 0; }
}

int sw_thread_is_worker(SwCpu *c) { return g_inited && thread_of(c) != 0; }

static u32 rd32(SwCpu *c, u64 a) { u32 v = 0; u8 *p = sw_mem_ptr(c, a, 4, 0); if (p) memcpy(&v, p, 4); return v; }
static void wr32(SwCpu *c, u64 a, u32 v)
{
    u8 *p = sw_mem_ptr(c, a, 4, 1);
#if defined(_WIN32)
    if (p && !((uintptr_t)p & 3)) { InterlockedExchange((volatile LONG *)p, (LONG)v); return; }
#endif
    if (p) memcpy(p, &v, 4);
}
/* compare and swap a guest word (guest code changes lock words with LDAXR/STLXR meanwhile) */
static int cas32(SwCpu *c, u64 a, u32 expect, u32 v)
{
    u8 *p = sw_mem_ptr(c, a, 4, 1);
    if (!p) return 0;
#if defined(_WIN32)
    if (!((uintptr_t)p & 3))
        return (u32)InterlockedCompareExchange((volatile LONG *)p, (LONG)v, (LONG)expect) == expect;
#endif
    u32 cur; memcpy(&cur, p, 4);
    if (cur != expect) return 0;
    memcpy(p, &v, 4);
    return 1;
}

void sw_gen_state_save(u64 st[3]);
void sw_gen_state_load(const u64 st[3]);
void sw_fpcr_apply(u64 fpcr);   /* sw_runtime.c: FPCR.FZ -> host MXCSR (per host thread) */

/* the fiber running on the main host thread, for the profiler (SW_PROFILE_THREAD) */
volatile int sw_prof_fiber;
static void switch_to(int next)
{
    if (next == g_cur) return;
    int prev = g_cur;
    {
        u64 now = sw_tick_now();
        if (g_slice_start) g_cpu_ticks[prev] += now - g_slice_start;
        g_slice_start = now;
    }
    g_cur = next;
    g_switches++;
    g_runs[next]++;
    u64 gen_state[3];
    sw_gen_state_save(gen_state);   /* trampoline depth is per guest thread */
    sw_prof_fiber = next;
#if defined(_WIN32)
    SwitchToFiber(g_t[next].fiber);
#endif
    g_cur = prev;          /* back on this fiber: it is running again */
    sw_prof_fiber = prev;
    sw_gen_state_load(gen_state);
    if (g_t[prev].cpu) sw_fpcr_apply(g_t[prev].cpu->fpcr);   /* fibers share one host MXCSR */
}

/* SW_HANG_DUMP=<s> (multi-core): when no frame was presented for <s> seconds (after the first present), print
 * every guest thread (state, wait, key, deadline, spinning, emulated core, guest fn / pc / lr), the core owners and
 * the kernel lock owner, every <s> seconds while the hang lasts.  Read-only: it takes no lock. */
volatile long long sw_present_count;
#if defined(_WIN32)
static DWORD WINAPI hang_watch(LPVOID arg)
{
    int secs = (int)(intptr_t)arg;
    long long last = -1; ULONGLONG since = GetTickCount64();
    static const char *wn[] = {"-", "lock", "condvar", "address", "thread", "sleep", "sync"};
    static const char *sn[] = {"free", "created", "ready", "BLOCKED", "dead"};
    for (;;) {
        Sleep(500);
        long long n = sw_present_count;
        ULONGLONG now = GetTickCount64();
        if (n != last || n == 0) { last = n; since = now; continue; }
        if (now - since < (ULONGLONG)secs * 1000) continue;
        since = now;
        u64 tk = sw_tick_now();
        sw_report("\n=== HANG: no present for %d s (present %lld); kernel lock owner host tid %lu, recursion %ld ===\n",
                  secs, n, (unsigned long)(uintptr_t)g_kcs.OwningThread, (long)g_kcs.RecursionCount);
        for (int i = 0; i < g_t_hi; i++) {
            GThread *t = &g_t[i];
            if (t->state == T_FREE || t->state == T_DEAD) continue;
            SwCpu *c = t->cpu;
            sw_report("  %2d %-10s %-7s wait %-7s key 0x%llx dl %s%lld ms spin %ld core %d/%d%s fn 0x%llx pc 0x%llx lr 0x%llx host %lu\n",
                      i, t->name, sn[(unsigned)t->state < 5 ? t->state : 0], wn[t->wait % 7], (unsigned long long)t->key,
                      t->deadline ? "" : "-", t->deadline ? (long long)((s64)(t->deadline - tk) / 19200) : 0ll,
                      (long)t->spinning, t->core_cur, t->core, t->core_wait ? " core-wait" : "",
                      (unsigned long long)(c ? c->cur_fn : 0), (unsigned long long)(c ? c->pc : 0),
                      (unsigned long long)(c ? c->x[30] : 0), (unsigned long)(t->host ? GetThreadId(t->host) : 0));
            if (t->wait == W_LOCK && c && t->key) {   /* who holds it: mutex word = owner handle | waiters bit */
                u32 w = rd32(c, t->key), h = w & ~0x40000000u;
                sw_report("       lock word 0x%x -> owner thread %d\n", w,
                          h >= THREAD_HANDLE && h < THREAD_HANDLE + MAX_THREADS ? (int)(h - THREAD_HANDLE) : h == 0x8001u ? 0 : -1);
            }
            {   /* SW_HANG_STACK=<thread>: code addresses on that thread's stack (likely return addresses) */
                const char *e = getenv("SW_HANG_STACK");
                if (e && atoi(e) == i && c) {
                    int nshow = 0;
                    sw_report("       stack:");
                    for (u64 a = c->sp; a < c->sp + 0x2000 && nshow < 40; a += 8) {
                        u8 *p = sw_mem_ptr(c, a, 8, 0);
                        if (!p) break;
                        u64 v; memcpy(&v, p, 8);
                        if (v >= 0x100000 && v < 0x5a00000 && !(v & 3)) { sw_report(" %llx", (unsigned long long)v); nshow++; }
                    }
                    sw_report("\n       fp chain:");
                    u64 fp = c->x[29];
                    for (int k = 0; k < 24 && fp; k++) {
                        u8 *p = sw_mem_ptr(c, fp, 16, 0);
                        if (!p) break;
                        u64 nfp, lr; memcpy(&nfp, p, 8); memcpy(&lr, p + 8, 8);
                        sw_report(" %llx", (unsigned long long)lr);
                        if (nfp <= fp) break;
                        fp = nfp;
                    }
                    sw_report("\n");
                }
            }
        }
        for (int k = 0; k < g_ncores; k++) sw_report("  core %d owner %d\n", k, g_core_owner[k]);
    }
    return 0;
}
#endif

static void deadlock_report(void)
{
    sw_report("\n=== DEADLOCK: every guest thread is blocked ===\n");
    static const char *wn[] = {"-", "lock", "condvar", "address", "thread", "sleep"};
    for (int i = 0; i < g_t_hi; i++) {
        if (g_t[i].state == T_FREE || g_t[i].state == T_DEAD) continue;
        sw_report("  thread %d (%s): state %d wait %s key 0x%llx fn 0x%llx\n", i, g_t[i].name,
                  g_t[i].state, wn[g_t[i].wait], (unsigned long long)g_t[i].key,
                  (unsigned long long)(g_t[i].cpu ? g_t[i].cpu->cur_fn : 0));
    }
}

/* Wake every blocked thread whose deadline (sleep, timed wait) has passed.  Without this a sleeping
 * thread only woke when no other thread was ready - in the open world the main thread and the job
 * workers are nearly always ready, so sleepers (threads 1 and 20 in the field) never ran again. */
static void wake_expired(void)
{
    u64 now = sw_tick_now();
    for (int i = 0; i < g_t_hi; i++)
        if (g_t[i].state == T_BLOCKED && g_t[i].deadline && now >= g_t[i].deadline) {
            g_t[i].state = T_READY;
            g_t[i].woken = 2;
            g_t[i].wait = W_NONE;
        }
}

static u64 g_idle_ticks[MAX_THREADS];   /* host idle time spent waiting for this thread's deadline */
static int g_idle_wait[MAX_THREADS];

/* Run other threads until the current one is ready again. */
static void schedule(void)
{
    if (g_mc) {                    /* host threads: the OS schedules; only a yield is left */
        if (g_t[g_cur].state == T_READY) mc_yield();
        else mc_wait(&g_t[g_cur]);
        return;
    }
    for (;;) {
        wake_expired();
        /* round robin over ready threads, starting after the current one;
         * SW_SCHED=prio: the most urgent ready thread instead (lowest Horizon
         * priority value; round robin among equals), and a thread passed over 4
         * times runs next whatever its priority - on the Switch the game's worker
         * threads had cores of their own, so none may starve here */
        {
            static int prio_mode = -1;
            static u8 skipped[MAX_THREADS];
            if (prio_mode < 0) { const char *e = getenv("SW_SCHED"); prio_mode = e && !strcmp(e, "prio"); }
            int pick = -1;
            for (int k = 1; k <= MAX_THREADS; k++) {
                int i = (g_cur + k) % MAX_THREADS;
                if (g_t[i].state != T_READY || i == g_cur) continue;
                if (!prio_mode) { pick = i; break; }
                if (skipped[i] >= 4) { pick = i; break; }
                if (pick < 0 || g_t[i].prio < g_t[pick].prio) pick = i;
            }
            if (pick >= 0) {
                if (prio_mode)
                    for (int i = 0; i < g_t_hi; i++)
                        if (g_t[i].state == T_READY && i != g_cur) skipped[i] = i == pick ? 0 : (u8)(skipped[i] + 1);
                switch_to(pick);
                if (g_t[g_cur].state == T_READY) return;
            }
        }
        if (g_t[g_cur].state == T_READY) return;
        /* nobody else ready: advance time to the earliest deadline */
        int best = -1;
        for (int i = 0; i < g_t_hi; i++)
            if (g_t[i].state == T_BLOCKED && g_t[i].deadline &&
                (best < 0 || g_t[i].deadline < g_t[best].deadline)) best = i;
        int any_ready = 0;
        for (int i = 0; i < g_t_hi; i++) if (g_t[i].state == T_READY) any_ready = 1;
        if (any_ready) continue;
        if (best < 0) {
            deadlock_report();
            /* break the deadlock in favour of the current thread */
            g_t[g_cur].state = T_READY;
            g_t[g_cur].woken = 2;
            return;
        }
        u64 now = sw_tick_now();
#if defined(_WIN32)
        if (g_t[best].deadline > now) {
            u64 ms = (g_t[best].deadline - now) / 19200;
            if (ms > 0) Sleep((DWORD)(ms > 100 ? 100 : ms));
            /* idle accounting: every thread blocked, the host waits for `best` */
            g_idle_ticks[best] += sw_tick_now() - now;
            g_slice_start = sw_tick_now();     /* the sleep is nobody's run time */
            g_idle_wait[best] = g_t[best].wait;
        }
#endif
        if (sw_tick_now() >= g_t[best].deadline) {
            g_t[best].state = T_READY;
            g_t[best].woken = 2;
            g_t[best].wait = W_NONE;
        }
    }
}

/* SW_MULTICORE: give the core away for a moment (yield) */
static void mc_yield(void)
{
#if defined(_WIN32)
    core_release(g_cur);
    int d = klock_drop();
    SwitchToThread();
    klock_take(d);
    core_acquire(g_cur);
#endif
}

/* Spin (kernel lock and emulated core released) for up to this thread's spin budget: 1 if it was woken meanwhile.
 * Adaptive per host thread: a spin that catches the wake doubles the budget (up to SW_SPIN_US), one that does not
 * halves it, so threads whose waits are long (idle workers) stop spinning; every 64th wait at budget 0 tries a
 * short spin again.  t->spinning tells wake() that no SetEvent is needed; the interlocked exchange here and the
 * fence in wake() make sure one of the two sides sees the other (no lost wake). */
static SW_TLS int t_spin_budget = -1, t_spin_probe;
static int mc_spin(GThread *t)
{
    if (t_spin_budget < 0) t_spin_budget = g_spin_us;
    int us = t_spin_budget;
    if (!us) {
        if (++t_spin_probe < 64) return 0;
        t_spin_probe = 0;
        us = g_spin_us / 8 + 1;
    }
    u64 end = sw_tick_now() + (u64)us * 192 / 10;
    if (t->deadline && t->deadline < end) end = t->deadline;
    InterlockedExchange(&t->spinning, 1);
    int woke = 0;
    for (int i = 1;; i++) {
        if (*(volatile int *)&t->state != T_BLOCKED) { woke = 1; break; }
        _mm_pause();
        if (!(i & 31) && sw_tick_now() >= end) break;
    }
    InterlockedExchange(&t->spinning, 0);
    if (!woke && *(volatile int *)&t->state != T_BLOCKED) woke = 1;   /* woken just as the spin ended */
    if (woke) {
        if (!(g_spin_us && t_spin_budget >= g_spin_us)) t_spin_budget = t_spin_budget * 2 + 1;
        if (t_spin_budget > g_spin_us) t_spin_budget = g_spin_us;
        if (g_mc_stats && mc_ix() < MC_NT) g_mc_nspin[mc_ix()]++;
    } else
        t_spin_budget /= 2;
    return woke;
}

/* Waiting time per host thread (spinning or sleeping in mc_wait), always counted: the performance overlay shows how
 * much of the busiest thread's CPU time is waiting for other threads (spin waits count as CPU time to Windows). */
#define WT_N 64
static volatile LONG g_wt_tid[WT_N];
static volatile u64 g_wt_ticks[WT_N];
static void wt_add(u64 ticks)
{
    LONG tid = (LONG)GetCurrentThreadId();
    unsigned h = ((unsigned)tid * 2654435761u) >> 26;
    for (unsigned p = 0; p < WT_N; p++) {
        unsigned k = (h + p) & (WT_N - 1);
        LONG o = g_wt_tid[k];
        if (o == tid || (!o && InterlockedCompareExchange(&g_wt_tid[k], tid, 0) == 0) || g_wt_tid[k] == tid) {
            g_wt_ticks[k] += ticks;   /* only this thread writes its slot */
            return;
        }
    }
}
/* waiting time of a host thread so far, in ms (0 when it never waited) */
double sw_host_wait_ms(unsigned long tid)
{
    unsigned h = ((unsigned)tid * 2654435761u) >> 26;
    for (unsigned p = 0; p < WT_N; p++) {
        unsigned k = (h + p) & (WT_N - 1);
        if ((unsigned long)g_wt_tid[k] == tid) return (double)g_wt_ticks[k] / 19200.0;
        if (!g_wt_tid[k]) return 0;
    }
    return 0;
}

/* SW_MULTICORE: the current thread waits (kernel lock released) until it is
 * woken (state READY, event set) or its deadline passes. */
static void mc_wait(GThread *t)
{
#if defined(_WIN32)
    core_release(g_cur);
    u64 b0 = sw_tick_now();
    while (t->state == T_BLOCKED) {
        DWORD ms = INFINITE;
        int no_spin = 0;
        if (t->deadline) {
            u64 now = sw_tick_now();
            if (now >= t->deadline) {
                t->state = T_READY; t->woken = 2; t->wait = W_NONE;
                break;
            }
            /* Timed waits (the frame pacer's, sleeps) end on time: a timer sleep can wake up to ~1 ms late, and rounding
             * the rest up to whole ms added up to 1 ms more - at a 120 fps limit 5% of the frames took 9.5 ms instead of
             * 8.3.  So: sleep whole ms minus one (wake early), and spin the last < 2 ms to the deadline. */
            u64 left = t->deadline - now;
            if (left < 19200ull * 2) {
                int d = klock_drop();
                InterlockedExchange(&t->spinning, 1);   /* wake() sees it: no SetEvent needed */
                for (unsigned i = 1; *(volatile int *)&t->state == T_BLOCKED; i++) {
                    _mm_pause();
                    if (!(i & 63)) {
                        if (sw_tick_now() >= t->deadline) break;
                        if (!(i & 4095)) SwitchToThread();
                    }
                }
                InterlockedExchange(&t->spinning, 0);
                klock_take(d);
                continue;                        /* woken, or the deadline check at the top times it out */
            }
            u64 whole = left / 19200 - 1;
            ms = (DWORD)(whole > 0x7FFFFFFF ? 0x7FFFFFFF : whole);
            /* a plain sleep (the pacer's wait) is not woken early: spinning first only wasted a core and, failing,
             * halved this thread's spin budget for its next job waits */
            no_spin = t->wait == W_SLEEP;
        }
        int d = klock_drop();
        if (g_spin_us && !no_spin && mc_spin(t)) {      /* woken while spinning: no sleep, no SetEvent */
            klock_take(d);
            continue;
        }
        WaitForSingleObject(t->ev, ms);
        if (g_mc_stats && t->state != T_BLOCKED && mc_ix() < MC_NT && g_mc_wake_at[mc_ix()]) {
            g_mc_wakelat[mc_ix()] += sw_tick_now() - g_mc_wake_at[mc_ix()]; g_mc_nwake[mc_ix()]++;
        }
        klock_take(d);
    }
    {
        u64 dt = sw_tick_now() - b0;
        wt_add(dt);
        if (g_mc_stats) { g_mc_blocked[mc_ix()] += dt; g_mc_nwait[mc_ix()]++; }
    }
    core_acquire(g_cur);
#else
    (void)t;
#endif
}

/* Block the current thread; returns 1 if signalled, 2 if timed out. */
/* SW_WAIT_STATS=1 (multi-core): blocked time per (thread, wait kind, key, the thread that woke it), the top entries
 * every 10 s with the [mc] report - which waits make the critical path of a frame.  Updated under the kernel lock. */
#define WS_N 1024
typedef struct { int thr, wait, waker; u64 key, ticks, n; } WaitStat;
static WaitStat g_ws[WS_N];
static int g_ws_on = -1;                    /* 1: SW_WAIT_STATS table, 2: SW_WAIT_LOG only */
static u64 g_ws_log_ticks;
static void ws_add(int thr, int wait, u64 key, int waker, u64 ticks)
{
    unsigned h = (unsigned)((key >> 3) * 2654435761u ^ (unsigned)thr * 40503u ^ (unsigned)wait * 977u ^ (unsigned)(waker + 1) * 31u);
    for (int p = 0; p < 16; p++) {
        WaitStat *w = &g_ws[(h + (unsigned)p) & (WS_N - 1)];
        if (w->n && (w->thr != thr || w->wait != wait || w->key != key || w->waker != waker)) continue;
        if (!w->n) w->thr = thr, w->wait = wait, w->key = key, w->waker = waker;
        w->ticks += ticks, w->n++;
        return;
    }
}
static void ws_report(void)
{
    static const char *const wn[] = {"none", "mutex", "condvar", "address", "thread", "sleep", "sync"};
    if (g_ws_on != 1) return;
    static const int show[] = {0, 2, 3, 10};
    for (unsigned si = 0; si < sizeof show / sizeof show[0]; si++) {
        int thr = show[si];
        u64 tot = 0;
        for (int i = 0; i < WS_N; i++) if (g_ws[i].n && g_ws[i].thr == thr) tot += g_ws[i].ticks;
        sw_report("[wait] thread %d: %llu ms blocked in 10 s; top:", thr, (unsigned long long)(tot / 19200));
        for (int r = 0; r < 8; r++) {
            int best = -1;
            for (int i = 0; i < WS_N; i++)
                if (g_ws[i].n && g_ws[i].thr == thr && (best < 0 || g_ws[i].ticks > g_ws[best].ticks)) best = i;
            if (best < 0 || !g_ws[best].ticks) break;
            WaitStat *w = &g_ws[best];
            sw_report("\n[wait]   %-7s 0x%llx woken by %d: %llu ms, %llu waits (avg %.1f us)", wn[w->wait < 7 ? w->wait : 0],
                      (unsigned long long)w->key, w->waker, (unsigned long long)(w->ticks / 19200), (unsigned long long)w->n,
                      (double)w->ticks / 19.2 / (double)w->n);
            w->ticks = 0;                     /* shown: out of the ranking (cleared below anyway) */
        }
        sw_report("\n");
    }
    memset(g_ws, 0, sizeof g_ws);
}

static int block(int wait, u64 key, s64 timeout_ns)
{
    GThread *t = &g_t[g_cur];
    if (g_ws_on < 0) {
        const char *e = getenv("SW_WAIT_STATS"); g_ws_on = e && *e == '1';
        /* SW_WAIT_LOG=<ms>: also a line for each wait of the main guest thread longer than that (not sleeps) - which
         * thread a slow frame waited for */
        const char *l = getenv("SW_WAIT_LOG");
        if (l && atof(l) > 0) { g_ws_log_ticks = (u64)(atof(l) * 19200.0); if (!g_ws_on) g_ws_on = 2; }
    }
    u64 ws0 = g_ws_on > 0 ? sw_tick_now() : 0;
    t->waker = -1;
    t->state = T_BLOCKED;
    t->wait = wait;
    t->key = key;
    t->woken = 0;
    t->deadline = timeout_ns >= 0 ? sw_tick_now() + (u64)timeout_ns * 192 / 10000 : 0;
    if (timeout_ns == 0) {                        /* poll: just give others a turn */
        t->state = T_READY;
        t->wait = W_NONE;
        if (g_mc) mc_yield(); else schedule();
        return 2;
    }
    if (g_mc) mc_wait(t); else schedule();
    if (ws0) {
        u64 dt = sw_tick_now() - ws0;
        if (g_ws_on == 1) ws_add(g_cur, wait, key, t->woken == 2 ? -2 : t->waker, dt);   /* -2: timed out */
        /* the job threads (main 0, workers 2 and 3): what a slow frame's job was itself waiting for */
        if (g_ws_log_ticks && (g_cur == 0 || g_cur == 2 || g_cur == 3 || g_cur == 10) &&
            (wait != W_SLEEP || g_cur == 10) && dt > g_ws_log_ticks)   /* thread 10: its pacing sleeps too */
            sw_report("[waitlog] thread %d waited %.2f ms on %s 0x%llx, woken by %d (lr 0x%llx)\n", g_cur,
                      (double)dt / 19200.0,
                      wait == W_CV ? "condvar" : wait == W_LOCK ? "mutex" : wait == W_ADDR ? "address" :
                      wait == W_SYNC ? "sync (events / handles)" : wait == W_SLEEP ? "sleep" : "other",
                      (unsigned long long)key, t->woken == 2 ? -2 : t->waker,
                      (unsigned long long)(t->cpu ? t->cpu->x[30] : 0));
    }
    t->wait = W_NONE;
    return t->woken ? t->woken : 1;
}

static void wake(int i, int how)
{
    g_t[i].waker = g_cur;
    g_t[i].state = T_READY;
    g_t[i].woken = how;
    g_t[i].wait = W_NONE;
#if defined(_WIN32)
    if (g_mc && g_t[i].ev) {
        MemoryBarrier();                          /* state READY visible before reading spinning (see mc_spin) */
        if (g_t[i].spinning) return;              /* it sees READY in its spin loop: no kernel call */
        if (g_mc_stats && i < MC_NT) g_mc_wake_at[i] = sw_tick_now();
        SetEvent(g_t[i].ev);
    }
#endif
}

/* A host-side wait made on behalf of the running guest thread (frame pacing in
 * nvnWindowAcquireTexture, ...): the other ready guest threads run meanwhile.
 * On the Switch they run on the other cores while the render thread waits; a
 * plain host sleep here froze the whole game - 17% of the time in the village,
 * which is what pulled it under 30 fps.  Deadline in kern_now_ns() time. */
void sw_thread_wait_until_ns(u64 deadline_ns, u64 now_ns)
{
    extern int sw_lockstep_busy;
    if (deadline_ns <= now_ns) return;
    if (!g_inited || sw_lockstep_busy) {
#if defined(_WIN32)
        u64 ms = (deadline_ns - now_ns) / 1000000;
        if (ms) Sleep((DWORD)ms);
#endif
        return;
    }
    sw_klock();         /* called from NVN procs, which run without the kernel lock (SW_MULTICORE) */
    block(W_SLEEP, 0, (s64)(deadline_ns - now_ns));
    sw_kunlock();
}

/* ------------------------------------------------------------------ */
/* thread lifecycle                                                    */
/* ------------------------------------------------------------------ */
static void sw_thread_exit_current(void);

#if defined(_WIN32)
/* SW_MULTICORE: a guest thread on its own host thread */
/* SW_HOST_SPREAD=1: the game's hot threads (main 0, job workers 2 and 3, the render thread 10) get distinct
 * physical cores as their ideal processor (logical 2, 4, 6, 8: one per SMT pair, 0 left to the system), so two of
 * them do not share a core's execution units.  A hint only: Windows still moves a thread when its core is busy. */
static void host_spread(int i)
{
#if defined(_WIN32)
    static int on = -1;
    if (on < 0) { const char *e = getenv("SW_HOST_SPREAD"); on = e && *e == '1'; }
    if (!on) return;
    int k = i == 0 ? 2 : i == 2 ? 4 : i == 3 ? 6 : i == 10 ? 8 : -1;
    if (k < 0 || k >= (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) return;
    DWORD r = SetThreadIdealProcessor(GetCurrentThread(), (DWORD)k);
    sw_report("[thread] %d: ideal processor %d%s\n", i, k, r == (DWORD)-1 ? " (refused)" : "");
#else
    (void)i;
#endif
}
void host_spread_main(void) { host_spread(0); }

static DWORD WINAPI host_main(LPVOID param)
{
    int i = (int)(intptr_t)param;
    GThread *t = &g_t[i];
    g_cur = i;
    g_guest = 1;
    host_spread(i);
    t->cpu->x[0] = t->arg;
    t->cpu->x[30] = 0;
    sw_fpcr_apply(t->cpu->fpcr);
    sw_klock();
    core_acquire(i);
    sw_kunlock();
    sw_call(t->cpu, t->entry);
    sw_klock();
    sw_thread_exit_current();
    return 0;
}

static void CALLBACK fiber_main(void *param)
{
    int i = (int)(intptr_t)param;
    GThread *t = &g_t[i];
    t->cpu->x[0] = t->arg;
    t->cpu->x[30] = 0;
    { static const u64 fresh[3] = {0, 0, 0}; sw_gen_state_load(fresh); }
    sw_fpcr_apply(t->cpu->fpcr);
    sw_call(t->cpu, t->entry);
    sw_thread_exit_current();
}
#endif

static void sw_thread_exit_current(void)
{
    int me = g_cur;
    g_t[me].state = T_DEAD;
    for (int i = 0; i < g_t_hi; i++)        /* wake joiners */
        if (g_t[i].state == T_BLOCKED && g_t[i].wait == W_THREAD && g_t[i].key == (u64)me) wake(i, 1);
#if defined(_WIN32)
    if (g_mc) { core_release(me); klock_drop(); ExitThread(0); }
#endif
    schedule();                                   /* never returns to a dead fiber */
    for (;;) { schedule(); }
}

/* A trap in a worker: hand the halt to the main thread's CPU and run it. */
void sw_thread_halt_to_main(SwCpu *c)
{
#if defined(_WIN32)
    if (g_mc) {           /* no fiber to hand the halt to: report and stop the process */
        sw_report("[thread] trap in thread %d (%s) at 0x%llx: %s; stopping\n", g_cur, g_t[g_cur].name,
                  (unsigned long long)c->halt_pc, c->halt_reason ? c->halt_reason : "?");
        fflush(stdout); fflush(stderr);
        ExitProcess(3);
    }
#endif
    SwCpu *m = g_t[0].cpu;
    m->halted = 1;
    m->halt_pc = c->halt_pc;
    m->halt_reason = c->halt_reason;
    m->cur_fn = c->cur_fn;
    m->pc = c->pc;
    sw_report("[thread] trap in thread %d (%s); stopping\n", g_cur, g_t[g_cur].name);
    g_t[g_cur].state = T_DEAD;
    g_t[0].state = T_READY;
    g_t[0].woken = 2;
    switch_to(0);
    for (;;) switch_to(0);
}

static u32 create_thread(SwCpu *c, u64 entry, u64 arg, u64 stack_top, int prio, int core)
{
    int i;
    for (i = 1; i < MAX_THREADS; i++) if (g_t[i].state == T_FREE || g_t[i].state == T_DEAD) break;
    if (i == MAX_THREADS) return 0;
    if (i + 1 > g_t_hi) g_t_hi = i + 1;
    GThread *t = &g_t[i];
    if (!t->cpu) t->cpu = (SwCpu *)calloc(1, sizeof(SwCpu));
    else memset(t->cpu, 0, sizeof(SwCpu));
    SwCpu *n = t->cpu;
    n->mem = c->mem;
    n->rt = c->rt;
    n->sp = stack_top & ~15ull;
    n->thread_id = (u64)i + 1;
    n->fpcr = c->fpcr;
    /* own TLS page: IPC buffer at +0, ThreadType pointer at +0x1f8 (set by nn::os) */
    u64 tls = TLS_REGION + (u64)i * 0x10000;
    if (!sw_mem_ptr(c, tls, 1, 0)) sw_mem_map(c, tls, 0x10000, NULL, 0);
    else memset(sw_mem_ptr(c, tls, 0x10000, 1), 0, 0x10000);
    n->tpidrro_el0 = tls;
    n->tpidr_el0 = c->tpidr_el0;                  /* nn::os replaces it from TLS+0x1f0 */
    /* TLS+0x1f0 = ELF TLS pointer: share the creator's block until nn::os sets one */
    u8 *p = sw_mem_ptr(c, tls + 0x1f0, 8, 1);
    if (p) memcpy(p, &c->tpidr_el0, 8);
    t->entry = entry;
    t->arg = arg;
    t->state = T_CREATED;
    {
        u64 fnp = 0, st = 0, sz = 0;
        u8 *q = sw_mem_ptr(c, stack_top, 8, 0); if (q) memcpy(&fnp, q, 8);
        q = sw_mem_ptr(c, arg + 0x50, 8, 0); if (q) memcpy(&st, q, 8);
        q = sw_mem_ptr(c, arg + 0x58, 8, 0); if (q) memcpy(&sz, q, 8);
        sw_report("[thread] create %d: entry 0x%llx arg 0x%llx stack_top 0x%llx [top]=0x%llx tt.stack 0x%llx+0x%llx\n",
                  i, (unsigned long long)entry, (unsigned long long)arg, (unsigned long long)stack_top,
                  (unsigned long long)fnp, (unsigned long long)st, (unsigned long long)sz);
    }
    t->wait = W_NONE;
    t->prio = prio;
    t->core = core >= 0 && core < g_ncores ? core : core == -2 ? g_t[g_cur].core : -1;   /* -2: the creator's */
    t->core_cur = -1;
    t->core_wait = 0;
    sw_report("[thread] %d: priority %d, core %d (asked %d)\n", i, prio, t->core, core);
    snprintf(t->name, sizeof t->name, "thread%d", i);
#if defined(_WIN32)
    if (g_mc) {
        if (!t->ev) t->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        else ResetEvent(t->ev);
        if (t->host) { CloseHandle(t->host); t->host = NULL; }
        t->host = CreateThread(NULL, 64u * 1024 * 1024, host_main, (void *)(intptr_t)i,
                               CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!t->host || !t->ev) { t->state = T_FREE; return 0; }
        {   /* the guest name in profilers / debuggers (SW_PROFILE=all groups samples by it) */
            wchar_t w[24]; swprintf(w, 24, L"guest-%d", i); SetThreadDescription(t->host, w);
        }
    } else {
        if (t->fiber) DeleteFiber(t->fiber);
        t->fiber = CreateFiberEx(64 * 1024, 16 * 1024 * 1024, 0, fiber_main, (void *)(intptr_t)i);
        if (!t->fiber) { t->state = T_FREE; return 0; }
    }
#endif
    return THREAD_HANDLE + (u32)i;
}

static int handle_index(u32 h)
{
    if (h == 0xFFFF8000u) return g_cur;           /* CurrentThread pseudo handle */
    if (h >= THREAD_HANDLE && h < THREAD_HANDLE + MAX_THREADS) return (int)(h - THREAD_HANDLE);
    if (h == 0x8001u) return 0;                   /* main thread handle from nnosInitialize */
    return -1;
}

/* ------------------------------------------------------------------ */
/* lock / condition variable / address arbiter                         */
/* ------------------------------------------------------------------ */
static void lock_take(SwCpu *c, u64 addr, u32 tag)
{
    for (;;) {
        u32 cur = rd32(c, addr);
        if ((cur & ~WAITERS_BIT) == 0 || (cur & ~WAITERS_BIT) == tag) {
            if (cas32(c, addr, cur, tag | (cur & WAITERS_BIT))) return;
            continue;                             /* the word changed under us: look again */
        }
        if (!(cur & WAITERS_BIT) && !cas32(c, addr, cur, cur | WAITERS_BIT)) continue;
        g_t[g_cur].tag = tag;
        block(W_LOCK, addr, -1);
        if ((rd32(c, addr) & ~WAITERS_BIT) == tag) return;   /* handed over */
    }
}

/* svc 0x1A fast path, called by sw_svc BEFORE the kernel lock (multi-core): the mutex is free (or already ours)
 * by the time the svc runs, so it is taken with one CAS, exactly like lock_take's first step.  Safe without the
 * kernel lock: the word is only changed by CAS (guest code, this) or by its owner's release (lock_release), and a
 * free word has no owner to release it.  Otherwise 0: the normal svc path (kernel lock, waiters, blocking).
 * Off by default (SW_ARB_FAST=1 on): only ~1% of calls found the mutex free, no measurable gain. */
volatile long long g_arb_fast_n, g_arb_slow_n;
int sw_arb_lock_fast(SwCpu *c)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("SW_ARB_FAST"); on = e && *e == '1'; }   /* off: hit 1% of calls, no gain (2026-11-11) */
    if (!on || !g_mc || !g_inited) return 0;
    u64 addr = c->x[1];
    u32 tag = (u32)c->x[2];
    u32 cur = rd32(c, addr);
    if (((cur & ~WAITERS_BIT) == 0 || (cur & ~WAITERS_BIT) == tag) && cas32(c, addr, cur, tag | (cur & WAITERS_BIT))) {
        if (g_mc_stats) InterlockedIncrement64(&g_arb_fast_n);
        c->x[0] = 0;
        return 1;
    }
    if (g_mc_stats) InterlockedIncrement64(&g_arb_slow_n);
    return 0;
}

static void lock_release(SwCpu *c, u64 addr)
{
    int first = -1, more = 0;
    for (int i = 0; i < g_t_hi; i++)
        if (g_t[i].state == T_BLOCKED && g_t[i].wait == W_LOCK && g_t[i].key == addr) {
            if (first < 0) first = i; else more = 1;
        }
    if (first < 0) { wr32(c, addr, 0); return; }
    wr32(c, addr, g_t[first].tag | (more ? WAITERS_BIT : 0));
    wake(first, 1);
}

static s64 addr_value(SwCpu *c, u64 a) { return (s32)rd32(c, a); }
static int addr_arb_old(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("SW_ADDR_ARB"); v = e && !strcmp(e, "old"); }
    return v;
}
#define SW_ADDR_ARB_OLD addr_arb_old()

int sw_ipc_event_never(u32 h);

/* sleep the calling guest thread (other threads run meanwhile); before threads exist: host sleep */
void sw_thread_sleep(SwCpu *c, s64 ns)
{
    if (!g_inited) {
#if defined(_WIN32)
        Sleep((DWORD)(ns / 1000000));
#endif
        return;
    }
    g_cur = thread_of(c);
    block(W_SLEEP, 0, ns);
}

int sw_thread_svc(SwCpu *c, u32 num)
{
    if (!g_inited) return 0;
    g_cur = thread_of(c);
    switch (num) {
    case 0x08: {                                  /* CreateThread(entry, arg, stack_top, prio, core) */
        u32 h = create_thread(c, c->x[1], c->x[2], c->x[3], (int)(s32)c->x[4], (int)(s32)c->x[5]);
        c->x[0] = h ? 0 : 0xD201;
        c->x[1] = h;
        return 1;
    }
    case 0x09: {                                  /* StartThread(handle) */
        int i = handle_index((u32)c->x[0]);
        if (i > 0 && g_t[i].state == T_CREATED) {
            g_t[i].state = T_READY;
#if defined(_WIN32)
            if (g_mc && g_t[i].host) ResumeThread(g_t[i].host);
#endif
        }
        c->x[0] = i > 0 ? 0 : RES_INVALID;
        return 1;
    }
    case 0x0A:                                    /* ExitThread */
        if (g_cur == 0) { c->halted = 1; c->halt_reason = "main thread exited"; return 1; }
        sw_thread_exit_current();
        return 1;
    case 0x0B: {                                  /* SleepThread(ns): 0/-1/-2 = yield */
        s64 ns = (s64)c->x[0];
        if (ns <= 0) { g_t[g_cur].state = T_READY; schedule(); }
        else block(W_SLEEP, 0, ns);
        c->x[0] = 0;
        return 1;
    }
    case 0x18: {                                  /* WaitSynchronization(out idx, handles, n, timeout) */
        u64 handles = c->x[1];
        int n = (int)c->x[2];
        s64 timeout = (s64)c->x[3];
        if (g_t[g_cur].cancel_pending) {          /* cancelled before it waited */
            g_t[g_cur].cancel_pending = 0;
            c->x[0] = RES_CANCELLED;
            return 1;
        }
        for (int k = 0; k < n; k++) {             /* thread handles: wait until it exits */
            int i = handle_index(rd32(c, handles + 4ull * (u64)k));
            if (i > 0) {
                if (g_t[i].state != T_DEAD) {
                    int r = block(W_THREAD, (u64)i, timeout);
                    if (r == 2) { c->x[0] = RES_TIMEOUT; c->x[1] = 0; return 1; }
                }
                c->x[0] = 0; c->x[1] = (u64)k;
                return 1;
            }
        }
        /* No handles: nn::os multi-waits on user-space objects (os::Event,
         * message queues) this way; the thread sleeps until a signaller calls
         * CancelSynchronization on it (result Cancelled) or the timeout ends.
         * Returning "index 0 signalled" made WaitAny hand out a garbage holder. */
        if (n == 0) {
            if (g_t[g_cur].cancel_pending) {
                g_t[g_cur].cancel_pending = 0;
                c->x[0] = RES_CANCELLED;
                return 1;
            }
            int r = block(W_SYNC, 0, timeout);
            if (g_t[g_cur].woken == 3 || r == 3) { g_t[g_cur].cancel_pending = 0; c->x[0] = RES_CANCELLED; }
            else c->x[0] = RES_TIMEOUT;
            return 1;
        }
        /* events (IPC, vsync...): not tracked; report the first one that can
         * fire as signalled after a yield.  If only never-signalled events
         * (error notifications) are waited on, time out or sleep. */
        int pick = -1;
        for (int k = 0; k < n; k++)
            if (!sw_ipc_event_never(rd32(c, handles + 4ull * (u64)k))) { pick = k; break; }
        if (pick < 0 && n > 0) {
            block(W_SLEEP, 0, timeout);           /* timeout < 0: sleeps for good */
            c->x[0] = RES_TIMEOUT;
            c->x[1] = 0;
            return 1;
        }
        {   /* timed events (the audout buffer event): signalled when the next buffer is done */
            s64 sw_ipc_event_delay_ns(u32 h);
            s64 best = -1;
            int bk = pick;
            for (int k = 0; k < n; k++) {
                u32 h = rd32(c, handles + 4ull * (u64)k);
                if (sw_ipc_event_never(h)) continue;
                s64 d = sw_ipc_event_delay_ns(h);
                if (best < 0 || d < best) best = d, bk = k;
            }
            if (best > 0) {
                if (timeout == 0) { c->x[0] = RES_TIMEOUT; c->x[1] = 0; return 1; }
                int short_to = timeout > 0 && timeout < best;
                block(W_SLEEP, 0, short_to ? timeout : best);
                if (short_to) { c->x[0] = RES_TIMEOUT; c->x[1] = 0; return 1; }
                c->x[0] = 0;
                c->x[1] = (u64)bk;
                return 1;
            }
            if (best == 0) pick = bk;
        }
        g_t[g_cur].state = T_READY;
        schedule();
        c->x[0] = 0;
        c->x[1] = pick < 0 ? 0 : (u64)pick;
        return 1;
    }
    case 0x19: {                                  /* CancelSynchronization(thread handle) */
        int i = handle_index((u32)c->x[0]);
        if (i < 0) { c->x[0] = RES_INVALID; return 1; }
        if (g_t[i].state == T_BLOCKED && g_t[i].wait == W_SYNC) wake(i, 3);
        else g_t[i].cancel_pending = 1;
        c->x[0] = 0;
        return 1;
    }
    case 0x1A:                                    /* ArbitrateLock(owner, addr, tag) */
        lock_take(c, c->x[1], (u32)c->x[2]);
        c->x[0] = 0;
        return 1;
    case 0x1B:                                    /* ArbitrateUnlock(addr) */
        lock_release(c, c->x[0]);
        c->x[0] = 0;
        return 1;
    case 0x1C: {                                  /* WaitProcessWideKeyAtomic(addr, key, tag, timeout) */
        u64 addr = c->x[0], key = c->x[1];
        u32 tag = (u32)c->x[2];
        s64 timeout = (s64)c->x[3];
        /* "condvar has waiters" BEFORE the mutex is released: guest code on another core can take the freed mutex
         * at once (CAS, no svc), see the flag still 0 and skip SignalProcessWideKey - the waiter then slept forever
         * (soak run 2026-11-11: every guest thread blocked on condvars, no presents). */
        wr32(c, key, 1);
        lock_release(c, addr);
        int r = block(W_CV, key, timeout);
        lock_take(c, addr, tag);
        c->x[0] = r == 2 ? RES_TIMEOUT : 0;
        return 1;
    }
    case 0x1D: {                                  /* SignalProcessWideKey(key, count) */
        u64 key = c->x[0];
        int count = (int)c->x[1], woke = 0, left = 0;
        for (int i = 0; i < g_t_hi; i++) {
            if (g_t[i].state == T_BLOCKED && g_t[i].wait == W_CV && g_t[i].key == key) {
                if (count <= 0 || woke < count) { wake(i, 1); woke++; }
                else left++;
            }
        }
        if (!left) wr32(c, key, 0);
        c->x[0] = 0;
        return 1;
    }
    case 0x34: {                                  /* WaitForAddress(addr, type, value, timeout) */
        u64 a = c->x[0];
        u32 type = (u32)c->x[1];
        s64 value = (s32)c->x[2], timeout = (s64)c->x[3];
        s64 cur = addr_value(c, a);
        int should = 0;
        if (type == 0) should = cur < value;                      /* WaitIfLessThan */
        else if (type == 1) {                                     /* ... and decrement, atomically */
            for (;;) {
                should = cur < value;
                if (!should || cas32(c, a, (u32)cur, (u32)(cur - 1))) break;
                cur = addr_value(c, a);
            }
        }
        else if (type == 2) should = cur == value;                /* WaitIfEqual */
        if (!should) { c->x[0] = 0xFA01; return 1; }              /* InvalidState */
        int r = block(W_ADDR, a, timeout);
        c->x[0] = r == 2 ? RES_TIMEOUT : 0;
        return 1;
    }
    case 0x35: {                                  /* SignalToAddress(addr, type, value, count) */
        u64 a = c->x[0];
        u32 type = (u32)c->x[1];
        s64 value = (s32)c->x[2];
        int count = (int)c->x[3], waiting = 0, woke = 0;
        for (int i = 0; i < g_t_hi; i++)
            if (g_t[i].state == T_BLOCKED && g_t[i].wait == W_ADDR && g_t[i].key == a) waiting++;
        if (type == 1 || type == 2) {
            /* Horizon KAddressArbiter: 1 = SignalAndIncrementIfEqual (value + 1);
             * 2 = SignalAndModifyByWaitingCountIfEqual: no waiters -> value + 1;
             * count <= 0 (wake all) -> value - 2; all waiters woken (waiting <= count)
             * -> value - 1; some left waiting -> value.  One compare-exchange, so a
             * guest store from another core in between is not lost (InvalidState). */
            s64 nv;
            if (type == 1 || waiting == 0) nv = value + 1;
            else if (count <= 0) nv = value - 2;
            else nv = waiting <= count ? value - 1 : value;
            if (SW_ADDR_ARB_OLD) {                               /* SW_ADDR_ARB=old: the earlier rules */
                s64 cur = addr_value(c, a);
                if (cur != value) { c->x[0] = 0xFA01; return 1; }
                if (type == 1 && !cas32(c, a, (u32)cur, (u32)(cur + 1))) { c->x[0] = 0xFA01; return 1; }
                else {
                    nv = cur;
                    if (waiting == 0) nv = cur + 1;
                    else if (count <= 0 || waiting <= count) nv = cur - 1;
                    wr32(c, a, (u32)nv);
                }
            } else if (!cas32(c, a, (u32)value, (u32)nv)) { c->x[0] = 0xFA01; return 1; }
        }
        for (int i = 0; i < g_t_hi && (count <= 0 || woke < count); i++)
            if (g_t[i].state == T_BLOCKED && g_t[i].wait == W_ADDR && g_t[i].key == a) { wake(i, 1); woke++; }
        c->x[0] = 0;
        return 1;
    }
    case 0x25: {                                  /* GetThreadId(handle) */
        int i = handle_index((u32)c->x[1]);
        c->x[0] = 0;
        c->x[1] = (u64)(i >= 0 ? i : g_cur) + 1;
        return 1;
    }
    }
    return 0;
}

/* Time slicing: called on every guest call.  A thread that spins on a flag
 * without a syscall would otherwise starve everyone (cooperative switching
 * only happens in blocking syscalls).  Switching between guest calls is safe:
 * the whole guest state lives in the thread's own SwCpu. */
/* SW_THREAD_STATS=1, multi-core: every 10 s (from the main guest thread), per thread in ms:
 * blocked in guest waits, waiting for an emulated core, waiting for / holding the kernel lock. */
static void mc_report_stats(void)
{
    static u64 last, p_lw[MC_NT], p_lh[MC_NT], p_cw[MC_NT], p_bl[MC_NT], p_nw[MC_NT], p_wl[MC_NT], p_nk[MC_NT], p_ns[MC_NT];
    u64 now = sw_tick_now();
    if (!last) { last = now; return; }
    if (now - last < 19200000ull * 10) return;
    sw_report("[mc] last %llu ms (%d cores, GetCurrentProcessorNumber %ld calls): thread  blocked  core-wait  lock-wait  lock-hold  (ms; the rest is running guest code)  waits  wake latency (avg us)  woken while spinning\n",
              (unsigned long long)((now - last) / 19200), g_ncores, InterlockedExchange(&g_core_id_calls, 0));
    last = now;
    for (int i = 0; i < MC_NT && i < MAX_THREADS; i++) {
        if (g_t[i].state == T_FREE) continue;
        u64 lw = g_mc_lockwait[i], lh = g_mc_lockhold[i], cw = g_mc_corewait[i], bl = g_mc_blocked[i];
        u64 nw = g_mc_nwait[i], wl = g_mc_wakelat[i], nk = g_mc_nwake[i], ns = g_mc_nspin[i];
        sw_report("[mc] %2d %-12s prio %2d core %2d coreid %5ld: %7llu %7llu %7llu %7llu  %7llu %6.1f %7llu\n", i, g_t[i].name, g_t[i].prio,
                  g_t[i].core, InterlockedExchange(&g_core_id_by[i], 0),
                  (unsigned long long)((bl - p_bl[i]) / 19200), (unsigned long long)((cw - p_cw[i]) / 19200),
                  (unsigned long long)((lw - p_lw[i]) / 19200), (unsigned long long)((lh - p_lh[i]) / 19200),
                  (unsigned long long)(nw - p_nw[i]), nk - p_nk[i] ? (double)(wl - p_wl[i]) / 19.2 / (double)(nk - p_nk[i]) : 0.0,
                  (unsigned long long)(ns - p_ns[i]));
        p_ns[i] = ns;
        p_lw[i] = lw; p_lh[i] = lh; p_cw[i] = cw; p_bl[i] = bl; p_nw[i] = nw; p_wl[i] = wl; p_nk[i] = nk;
    }
    kh_report();
    ws_report();
}

/* sw_call counts this down and calls sw_thread_tick only when it reaches 0
 * (the per-call function call was ~1% of all CPU time).  Same slices as
 * before: multi-core every 4096 calls (then every 256 while nobody waits for a
 * core), fibers every 20000 calls. */
/* sw_tick_left = sw_gtls.tick_left (sw_rt.h, defined in sw_runtime.c) */
void sw_thread_tick(SwCpu *c)
{
    extern int sw_lockstep_busy;
    if (g_mc) {
        sw_tick_left = 4096;
        if (g_mc_stats && g_cur == 0) mc_report_stats();
        if (!g_core_waiters) { sw_tick_left = 256; return; }
        sw_klock();
        if (g_t[g_cur].core_cur >= 0) mc_yield();     /* hands the core to a waiter, then queues for it again */
        sw_kunlock();
        return;
    }
    sw_tick_left = 20000;
    if (!g_inited || sw_lockstep_busy) return;
    {   /* SW_THREAD_STATS=1: every 10 s, how often each guest thread ran and what it waits for */
        static int st = -1; static u64 last;
        if (st < 0) { const char *e = getenv("SW_THREAD_STATS"); st = e && *e == '1'; }
        u64 now = sw_tick_now();
        if (st && now - last > 19200000ull * 10) {
            static const char *wn[] = {"-", "lock", "condvar", "address", "thread", "sleep", "sync"};
            static u64 prev[MAX_THREADS], prev_idle[MAX_THREADS], prev_cpu[MAX_THREADS];
            last = now;
            for (int i = 0; i < g_t_hi; i++) {
                u64 d = g_idle_ticks[i] - prev_idle[i];
                if (d > 19200 * 50)        /* > 50 ms of idle in the last 10 s */
                    sw_report("[threads] idle %5llu ms waiting for thread %d (%s, wait %s, fn 0x%llx)\n",
                              (unsigned long long)(d / 19200), i, g_t[i].name,
                              wn[g_idle_wait[i] < 7 ? g_idle_wait[i] : 0],
                              (unsigned long long)(g_t[i].cpu ? g_t[i].cpu->cur_fn : 0));
                prev_idle[i] = g_idle_ticks[i];
            }
            sw_report("[threads] switches %llu\n", (unsigned long long)g_switches);
            for (int i = 0; i < g_t_hi; i++) {
                if (g_t[i].state == T_FREE) continue;
                sw_report("[threads]  %2d %-24s state %d wait %-8s runs +%llu cpu %5llu ms fn 0x%llx\n", i, g_t[i].name,
                          g_t[i].state, wn[g_t[i].wait < 7 ? g_t[i].wait : 0], (unsigned long long)(g_runs[i] - prev[i]),
                          (unsigned long long)((g_cpu_ticks[i] - prev_cpu[i]) / 19200),
                          (unsigned long long)(g_t[i].cpu ? g_t[i].cpu->cur_fn : 0));
                prev[i] = g_runs[i];
                prev_cpu[i] = g_cpu_ticks[i];
            }
        }
    }
    int me = thread_of(c);
    wake_expired();
    for (int k = 1; k < MAX_THREADS; k++) {
        int i = (me + k) % MAX_THREADS;
        if (g_t[i].state == T_READY) {
            g_cur = me;
            schedule();                       /* current stays READY: comes back in turn */
            return;
        }
    }
}

u64 sw_thread_switches(void) { return g_switches; }
int sw_thread_count(void)
{
    int n = 0;
    for (int i = 0; i < g_t_hi; i++) if (g_t[i].state != T_FREE) n++;
    return n;
}
