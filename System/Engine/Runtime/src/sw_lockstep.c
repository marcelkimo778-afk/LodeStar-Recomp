/* sw_lockstep.c - dev tool: run guest functions in Unicorn (the original
 * ARM64 code) and in the recompiled C on the same live state, then compare.
 *
 *   SW_LOCKSTEP=<runtime addr>[,<skip>[,<count>]]   one function, e.g. 0x2afac0c,100,3
 *   SW_LOCKSTEP=auto[,<period>[,<max per fn>]]      sweep: every <period>-th
 *       guest call (default 3000) is checked, each function at most
 *       <max per fn> times (default 8); only mismatches are printed, plus a
 *       summary every 500 checks.
 *   SW_LOCKSTEP_DLL=<path to unicorn.dll>  e.g. the Python unicorn package's
 *       ...\site-packages\unicorn\lib\unicorn.dll
 *
 * At the selected sw_call: the CPU state is copied, Unicorn runs the guest
 * code on lazily copied 4 KB pages of the live memory until the function
 * returns (lr = sentinel), then the recompiled function runs for real.  The
 * registers and every page Unicorn touched are compared; differences are
 * reported with the guest PCs that wrote the bytes in Unicorn.
 * Calls into host thunks (0xFF......) or svc end the Unicorn run (skipped).
 * Never part of a normal run: nothing happens unless SW_LOCKSTEP is set.
 */
#include "sw_rt.h"
#include "sw_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#if defined(_WIN32)
#include <windows.h>
#endif

void sw_report(const char *fmt, ...);
extern const SwFuncEntry sw_functions[];
extern const u32 sw_function_count;
u8 *sw_mem_ptr(SwCpu *c, u64 addr, u64 size, int write);
int sw_lockstep_busy;

#if defined(_WIN32)
typedef void uc_engine;
typedef size_t uc_hook;
typedef int (*p_uc_open)(int arch, int mode, uc_engine **uc);
typedef int (*p_uc_close)(uc_engine *uc);
typedef int (*p_uc_mem_map)(uc_engine *uc, uint64_t a, size_t n, uint32_t perms);
typedef int (*p_uc_mem_write)(uc_engine *uc, uint64_t a, const void *b, size_t n);
typedef int (*p_uc_mem_read)(uc_engine *uc, uint64_t a, void *b, size_t n);
typedef int (*p_uc_reg_write)(uc_engine *uc, int r, const void *v);
typedef int (*p_uc_reg_read)(uc_engine *uc, int r, void *v);
typedef int (*p_uc_emu_start)(uc_engine *uc, uint64_t b, uint64_t u, uint64_t t, size_t cnt);
typedef int (*p_uc_emu_stop)(uc_engine *uc);
typedef int (*p_uc_hook_add)(uc_engine *uc, uc_hook *h, int type, void *cb, void *ud, uint64_t b, uint64_t e, ...);
typedef const char *(*p_uc_strerror)(int);

static p_uc_open u_open; static p_uc_close u_close; static p_uc_mem_map u_map;
static p_uc_mem_write u_wr; static p_uc_mem_read u_rd; static p_uc_reg_write u_rw;
static p_uc_reg_read u_rr; static p_uc_emu_start u_start; static p_uc_emu_stop u_stop;
static p_uc_hook_add u_hook; static p_uc_strerror u_err;

enum { R_X29 = 1, R_X30 = 2, R_NZCV = 3, R_SP = 4, R_Q0 = 104, R_X0 = 199, R_PC = 260,
       R_TPIDR = 262, R_TPIDRRO = 263, R_FPCR = 291 };
enum { H_INTR = 1, H_MEM_WRITE = 1024, H_MEM_UNMAPPED = 112 };
#define SENTINEL 0x7FFFFFF000ull
#define PG 0x1000ull
#define MAX_PAGES 200000
#define MAX_WLOG 4000000

static u64 g_target, g_skip, g_count = 1, g_seen;
int sw_lockstep_state = -1;   /* -1 not yet read, 0 off, 1 on (checked inline by sw_call) */
#define g_on sw_lockstep_state
static int g_auto; static u64 g_period = 3000, g_per_fn = 8, g_tick;
static u64 g_ht_key[1 << 16]; static u16 g_ht_n[1 << 16];
static u64 g_st_tested, g_st_match, g_st_skip, g_st_bad, g_st_regonly;
static SwCpu *g_live;
static u64 *g_pages; static u32 g_npages;
typedef struct { u64 pc, addr, val; u32 size; } WLog;
static WLog *g_wlog; static u32 g_nwlog;
static u64 g_blk[256]; static u32 g_nblk; static u64 g_cur_target;
static int g_fail; static u64 g_fail_pc; static char g_fail_why[160];
/* SW_LOCKSTEP_STEP=1: step over host thunks and svc instead of skipping the check */
static int g_step = -1; static u64 g_nstep; static u64 g_thunk[8]; static u32 g_nthunk;

/* report buffer: printed only when the check is interesting */
static char g_out[32768]; static size_t g_outn;
static void outf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_outn < sizeof g_out - 512) {
        int k = vsnprintf(g_out + g_outn, sizeof g_out - g_outn, fmt, ap);
        if (k > 0) g_outn += (size_t)k;
    }
    va_end(ap);
}

static int reg_x(int i) { return i == 29 ? R_X29 : i == 30 ? R_X30 : R_X0 + i; }

static int load_dll(void)
{
    const char *p = getenv("SW_LOCKSTEP_DLL");
    HMODULE h = LoadLibraryA(p && *p ? p : "unicorn.dll");
    if (!h) { sw_report("[lockstep] cannot load unicorn.dll (set SW_LOCKSTEP_DLL)\n"); return 0; }
#define G(v, n) v = (void *)GetProcAddress(h, n); if (!v) { sw_report("[lockstep] missing %s\n", n); return 0; }
    G(u_open, "uc_open") G(u_close, "uc_close") G(u_map, "uc_mem_map") G(u_wr, "uc_mem_write")
    G(u_rd, "uc_mem_read") G(u_rw, "uc_reg_write") G(u_rr, "uc_reg_read") G(u_start, "uc_emu_start")
    G(u_stop, "uc_emu_stop") G(u_hook, "uc_hook_add") G(u_err, "uc_strerror")
#undef G
    return 1;
}

int sw_lockstep_match(u64 target)
{
    if (g_on == 0) return 0;
    if (g_on < 0) {
        const char *e = getenv("SW_LOCKSTEP");
        g_on = 0;
        if (e && *e) {
            char *q;
            if (!strncmp(e, "auto", 4)) {
                g_auto = 1; q = (char *)e + 4;
                if (*q == ',') g_period = strtoull(q + 1, &q, 0);
                if (*q == ',') g_per_fn = strtoull(q + 1, &q, 0);
                if (!g_period) g_period = 1;
            } else {
                g_target = strtoull(e, &q, 0);
                if (*q == ',') g_skip = strtoull(q + 1, &q, 0);
                if (*q == ',') g_count = strtoull(q + 1, &q, 0);
            }
            g_on = load_dll();
            g_pages = (u64 *)malloc(sizeof(u64) * MAX_PAGES);
            g_wlog = (WLog *)malloc(sizeof(WLog) * MAX_WLOG);
            if (!g_pages || !g_wlog) g_on = 0;
            if (g_on) sw_report("[lockstep] armed: %s 0x%llx skip/period %llu count/per-fn %llu\n",
                                g_auto ? "auto" : "fn", (unsigned long long)g_target,
                                (unsigned long long)(g_auto ? g_period : g_skip),
                                (unsigned long long)(g_auto ? g_per_fn : g_count));
        }
        if (!g_on) return 0;
    }
    if (sw_lockstep_busy) return 0;
    if (g_auto) {
        /* SW_LOCKSTEP_KEY=1: the sweep waits for F9 (the player stands where the bug is, then
         * presses it); each F9 press restarts the per-function counts */
        static int key_mode = -1; static u64 key_poll; static int armed, was_down;
        if (key_mode < 0) { const char *k = getenv("SW_LOCKSTEP_KEY"); key_mode = k && *k == '1';
                            if (key_mode) sw_report("[lockstep] waiting for F9 to start the sweep\n"); }
        if (key_mode && (++key_poll & 0xFFF) == 0) {
            int down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            if (down && !was_down) {
                memset(g_ht_key, 0, sizeof g_ht_key); memset(g_ht_n, 0, sizeof g_ht_n);
                armed = 1; g_tick = 0;
                sw_report("[lockstep] F9: sweep (re)started after %llu checks\n", (unsigned long long)g_st_tested);
            }
            was_down = down;
        }
        if (key_mode && !armed) return 0;
        {   /* SW_LOCKSTEP_DELAY=<s>: the sweep starts that many seconds after the first call */
            static int dl = -1; static ULONGLONG t0;
            if (dl < 0) { const char *d = getenv("SW_LOCKSTEP_DELAY"); dl = d ? atoi(d) : 0; t0 = GetTickCount64();
                          if (dl) sw_report("[lockstep] sweep starts after %d s\n", dl); }
            if (dl > 0) {
                if ((++key_poll & 0xFFF) != 0 && !armed) return 0;
                if (!armed && GetTickCount64() - t0 < (ULONGLONG)dl * 1000) return 0;
                if (!armed) { armed = 1; sw_report("[lockstep] delay over: sweep started\n"); }
            }
        }
        if (++g_tick < g_period || target >= 0xFF000000ull || target < 0x100000) return 0;
        g_tick = 0;
        u32 hsh = (u32)((target * 0x9E3779B97F4A7C15ull) >> 48);
        while (g_ht_key[hsh] && g_ht_key[hsh] != target) hsh = (hsh + 1) & 0xFFFF;
        if (g_ht_n[hsh] >= g_per_fn) return 0;
        g_ht_key[hsh] = target; g_ht_n[hsh]++;
        g_seen++;
        return 1;
    }
    if (target != g_target) return 0;
    g_seen++;
    if (g_seen <= g_skip || g_seen > g_skip + g_count) return 0;
    return 1;
}

static bool cb_unmapped(uc_engine *uc, int type, uint64_t addr, int size, int64_t value, void *ud)
{
    (void)value; (void)ud; (void)size;
    u64 pg = addr & ~(PG - 1);
    if (pg == (SENTINEL & ~(PG - 1))) return false;
    if (g_step && pg >= 0xFF000000ull && pg < 0x100000000ull) {
        /* SW_LOCKSTEP_STEP=1: host thunks (nn::os mutexes, services...) are stepped over in
         * Unicorn - a page of RET; their results/effects are missing on the arm side */
        static u32 rets[PG / 4];
        if (!rets[0]) for (u32 i = 0; i < PG / 4; i++) rets[i] = 0xD65F03C0u;
        if (u_map(uc, pg, PG, 7) != 0) return false;
        u_wr(uc, pg, rets, PG);
        return true;
    }
    u8 *host = sw_mem_ptr(g_live, pg, PG, 0);
    if (!host || g_npages >= MAX_PAGES) {
        u64 pc = 0; u_rr(uc, R_PC, &pc);
        g_fail = 1; g_fail_pc = pc;
        snprintf(g_fail_why, sizeof g_fail_why, "unmapped %s at 0x%llx (type %d)",
                 type == 21 ? "fetch" : "access", (unsigned long long)addr, type);
        return false;
    }
    if (u_map(uc, pg, PG, 7) != 0) return false;
    u_wr(uc, pg, host, PG);
    g_pages[g_npages++] = pg;
    return true;
}

static void cb_intr(uc_engine *uc, uint32_t intno, void *ud)
{
    (void)ud;
    u64 pc = 0; u_rr(uc, R_PC, &pc);
    if (g_step && intno == 2) {          /* svc: stepped over, result 0 (success) */
        u64 z = 0; u_rw(uc, reg_x(0), &z);
        g_nstep++;
        return;
    }
    g_fail = 1; g_fail_pc = pc;
    snprintf(g_fail_why, sizeof g_fail_why, "exception/svc %u", intno);
    u_stop(uc);
}

static void cb_thunk(uc_engine *uc, uint64_t addr, uint32_t size, void *ud)
{
    (void)size; (void)ud;
    g_nstep++;
    /* side-effect-free getters run natively on the arm-side registers, so their
     * result is right; everything else (locks, events...) just returns */
    static u64 ck[64]; static const SwFuncEntry *cf[64];
    const SwFuncEntry *fe = NULL;
    u32 h = (u32)(addr >> 3) & 63;
    if (ck[h] == addr) fe = cf[h];
    else {
        for (u32 i = sw_function_count; i-- > 0;) if (sw_functions[i].addr == addr) { fe = &sw_functions[i]; break; }
        ck[h] = addr; cf[h] = fe;
    }
    if (fe && fe->name) {
        static const char *pure[] = {"_ZN2nn2os11GetTlsValue", "_ZN2nn2os16GetCurrentThread", "_ZN2nn2os11GetThreadId",
            "_ZN2nn2os20GetCurrentCoreNumber", "_ZN2nn2os22GetSystemTickFrequency", "_ZN2nn2os13GetSystemTick",
            "_ZN2nn2os17ConvertToTimeSpan", "_ZN2nn2os28IsMutexLockedByCurrentThread", "_ZN2nn2os15GetCurrentFiber",
            "_ZN2nn2os17GetThreadPriority", "_ZN2nn2os26GetThreadAvailableCoreMask", NULL};
        for (int k = 0; pure[k]; k++)
            if (!strncmp(fe->name, pure[k], strlen(pure[k]))) {
                static SwCpu tmp;
                tmp = *g_live;
                for (int i = 0; i < 31; i++) u_rr(uc, reg_x(i), &tmp.x[i]);
                fe->fn(&tmp);
                u_rw(uc, reg_x(0), &tmp.x[0]);
                return;
            }
        if (!strncmp(fe->name, "_ZN2nn2os12TryLockMutex", 23)) { u64 one = 1; u_rw(uc, reg_x(0), &one); }
    }
    for (u32 i = 0; i < g_nthunk; i++) if (g_thunk[i] == addr) return;
    if (g_nthunk < 8) g_thunk[g_nthunk++] = addr;
}

static void cb_write(uc_engine *uc, int type, uint64_t addr, int size, int64_t value, void *ud)
{
    (void)type; (void)ud;
    if (g_nwlog >= MAX_WLOG) return;
    u64 pc = 0; u_rr(uc, R_PC, &pc);
    WLog *w = &g_wlog[g_nwlog++];
    w->pc = pc; w->addr = addr; w->size = (u32)size; w->val = (u64)value;
}

static void cb_block(uc_engine *uc, uint64_t addr, uint32_t size, void *ud)
{
    (void)uc; (void)size; (void)ud;
    if (addr >= g_cur_target && addr < g_cur_target + 0x800) return;
    for (u32 i = 0; i < g_nblk; i++) if (g_blk[i] == addr) return;
    if (g_nblk < 256) g_blk[g_nblk++] = addr;
}

static int cmp_u64(const void *a, const void *b)
{
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

static void summary(void)
{
    sw_report("[lockstep] summary: %llu checked, %llu match, %llu MISMATCH (+%llu scratch-register only), %llu skipped (thunk/svc/limit)\n",
              (unsigned long long)g_st_tested, (unsigned long long)g_st_match, (unsigned long long)g_st_bad,
              (unsigned long long)g_st_regonly, (unsigned long long)g_st_skip);
}

void sw_lockstep_run(SwCpu *c, u64 target)
{
    uc_engine *uc = NULL;
    SwCpu pre = *c;
    uc_hook h1, h2, h3;
    int err;
    u64 n = g_seen;
    sw_lockstep_busy = 1;
    g_live = c; g_npages = 0; g_nwlog = 0; g_fail = 0; g_outn = 0;
    if (g_step < 0) { const char *s = getenv("SW_LOCKSTEP_STEP"); g_step = s && *s == '1';
                      if (g_step) sw_report("[lockstep] stepping over host thunks and svc (their effects are missing on the arm side)\n"); }
    g_nstep = 0; g_nthunk = 0;
    if (u_open(2, 0, &uc) != 0) { sw_report("[lockstep] uc_open failed\n"); sw_lockstep_busy = 0; sw_call(c, target); return; }
    u_hook(uc, &h1, H_MEM_UNMAPPED, (void *)cb_unmapped, NULL, 1, 0);
    u_hook(uc, &h2, H_INTR, (void *)cb_intr, NULL, 1, 0);
    u_hook(uc, &h3, H_MEM_WRITE, (void *)cb_write, NULL, 1, 0);
    if (g_step) { uc_hook h5; u_hook(uc, &h5, 4 /* UC_HOOK_CODE */, (void *)cb_thunk, NULL, 0xFF000000ull, 0xFFFFFFFFull); }
    g_nblk = 0; g_cur_target = target;
    if (!g_auto) { uc_hook h4; u_hook(uc, &h4, 8, (void *)cb_block, NULL, 1, 0); }
    for (int i = 0; i < 31; i++) { u64 v = i == 30 ? SENTINEL : c->x[i]; u_rw(uc, reg_x(i), &v); }
    u_rw(uc, R_SP, &c->sp);
    { u64 nz = ((u64)c->nf << 31) | ((u64)c->zf << 30) | ((u64)c->cf << 29) | ((u64)c->vf << 28); u_rw(uc, R_NZCV, &nz); }
    for (int i = 0; i < 32; i++) u_rw(uc, R_Q0 + i, &c->v[i]);
    u_rw(uc, R_TPIDR, &c->tpidr_el0);
    u_rw(uc, R_TPIDRRO, &c->tpidrro_el0);
    u_rw(uc, R_FPCR, &c->fpcr);
    err = u_start(uc, target, SENTINEL, 0, g_auto ? 3000000 : 400000000);
    u64 upc = 0; u_rr(uc, R_PC, &upc);

    /* the real (recompiled) run; lr as the caller set it */
    sw_call(c, target);

    g_st_tested++;
    outf("[lockstep] #%llu 0x%llx (main+0x%llx): unicorn %s (pc 0x%llx%s%s), %u pages, %u writes\n",
         (unsigned long long)n, (unsigned long long)target, (unsigned long long)(target - 0x100000),
         err ? u_err(err) : "ok", (unsigned long long)upc, g_fail ? ", " : "", g_fail ? g_fail_why : "",
         g_npages, g_nwlog);
    if (g_nstep) {
        outf("[lockstep]   stepped over %llu thunk/svc calls:", (unsigned long long)g_nstep);
        for (u32 i = 0; i < g_nthunk; i++) outf(" 0x%llx", (unsigned long long)g_thunk[i]);
        outf("\n");
    }
    if (g_fail || upc != SENTINEL) {
        g_st_skip++;
        if (!g_auto) {
            outf("[lockstep]   unicorn did not reach the return - comparison skipped (first failing pc 0x%llx)\n",
                 (unsigned long long)g_fail_pc);
            sw_report("%s", g_out);
        }
        goto done;
    }
    {
        int bad = 0, badimp = 0;
        static const char *imp[] = {"", "*"};
        for (int i = 0; i < 30; i++) {
            u64 v; u_rr(uc, reg_x(i), &v);
            if (v != c->x[i]) {
                int important = i == 0 || (i >= 19 && i <= 29);
                outf("[lockstep]   %sx%d: arm 0x%llx  native 0x%llx (entry 0x%llx)\n", imp[important], i,
                     (unsigned long long)v, (unsigned long long)c->x[i], (unsigned long long)pre.x[i]);
                bad++; badimp += important;
            }
        }
        { u64 v; u_rr(uc, R_SP, &v); if (v != c->sp) { outf("[lockstep]   *sp: arm 0x%llx native 0x%llx\n", (unsigned long long)v, (unsigned long long)c->sp); bad++; badimp++; } }
        for (int i = 0; i < 32; i++) {
            sw_v128 v; u_rr(uc, R_Q0 + i, &v);
            int important = i <= 3 || (i >= 8 && i <= 15);
            /* d8-d15: only the low 64 bits are callee-saved */
            int differs = (i >= 8 && i <= 15) ? v.d[0] != c->v[i].d[0] : memcmp(&v, &c->v[i], 16) != 0;
            if (differs) {
                outf("[lockstep]   %sv%d: arm %08x %08x %08x %08x (%g %g %g %g)  native %08x %08x %08x %08x (%g %g %g %g)\n",
                     imp[important], i, v.s[0], v.s[1], v.s[2], v.s[3], v.f[0], v.f[1], v.f[2], v.f[3],
                     c->v[i].s[0], c->v[i].s[1], c->v[i].s[2], c->v[i].s[3],
                     c->v[i].f[0], c->v[i].f[1], c->v[i].f[2], c->v[i].f[3]);
                bad++; badimp += important;
            }
        }
        /* memory */
        qsort(g_pages, g_npages, sizeof(u64), cmp_u64);
        static u8 buf[PG];
        int shown = 0, ndiff = 0, ndiff_armw = 0;
        for (u32 k = 0; k < g_npages; k++) {
            u64 pg = g_pages[k];
            u8 *host = sw_mem_ptr(c, pg, PG, 0);
            if (!host) continue;
            u_rd(uc, pg, buf, PG);
            if (!memcmp(buf, host, PG)) continue;
            for (u64 o = 0; o < PG; o += 8) {
                if (!memcmp(buf + o, host + o, 8)) continue;
                u64 a = pg + o, av, nv;
                int wl = 0;
                ndiff++;
                memcpy(&av, buf + o, 8); memcpy(&nv, host + o, 8);
                if (av == SENTINEL && nv == pre.x[30]) { ndiff--; continue; }   /* saved lr */
                for (u32 w = g_nwlog; w-- > 0;) {
                    WLog *e = &g_wlog[w];
                    if (e->addr < a + 8 && e->addr + e->size > a) { wl = 1; break; }
                }
                ndiff_armw += wl;
                if (shown >= 24) continue;
                shown++;
                float af[2], nf2[2]; memcpy(af, &av, 8); memcpy(nf2, &nv, 8);
                outf("[lockstep]   mem 0x%llx: arm %016llx (%g %g)  native %016llx (%g %g)%s\n",
                     (unsigned long long)a, (unsigned long long)av, af[0], af[1],
                     (unsigned long long)nv, nf2[0], nf2[1], a < c->sp && a >= c->sp - 0x100000 ? "  [below sp]" : "");
                wl = 0;
                for (u32 w = g_nwlog; w-- > 0 && wl < 2;) {
                    WLog *e = &g_wlog[w];
                    if (e->addr < a + 8 && e->addr + e->size > a) {
                        outf("[lockstep]       arm write: pc 0x%llx (main+0x%llx) addr 0x%llx size %u val 0x%llx\n",
                             (unsigned long long)e->pc, (unsigned long long)(e->pc - 0x100000), (unsigned long long)e->addr,
                             e->size, (unsigned long long)e->val);
                        wl++;
                    }
                }
                if (!wl) outf("[lockstep]       (no arm write: native wrote it, or another thread did)\n");
            }
        }
        outf("[lockstep]   %d register diffs (%d important), %d memory granules differ (%d written by arm)%s\n",
             bad, badimp, ndiff, ndiff_armw, bad || ndiff ? "" : " -> MATCH");
        if (!bad && !ndiff) g_st_match++;
        else if (!badimp && !ndiff) g_st_regonly++;
        else g_st_bad++;
        if (!g_auto) { outf("[lockstep]   blocks outside the function:"); for (u32 i = 0; i < g_nblk; i++) outf(" %llx", (unsigned long long)g_blk[i]); outf("\n"); }
        if (!g_auto || badimp || ndiff) sw_report("%s", g_out);
    }
done:
    if (g_auto && g_st_tested % 500 == 0) summary();
    u_close(uc);
    sw_lockstep_busy = 0;
}
#else
int sw_lockstep_match(u64 target) { (void)target; return 0; }
void sw_lockstep_run(SwCpu *c, u64 target) { sw_call(c, target); }
#endif
