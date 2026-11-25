/* Host hooks on guest functions: the entry in sw_functions (the table every guest
 * call goes through) is pointed at a host wrapper that calls the original.
 *
 * Used to find and fix the game's frame step for 60 fps:
 *   SW_HOOK_LOG=0xADDR[,0xADDR...]  log calls (x0..x2, s0, lr) and returns (x0), first 200 each,
 *                                   + a rate every 10 s; up to 16 functions
 *
 * Direct C calls between generated functions would bypass the table; the
 * recompiler does not emit any (every bl goes through sw_call). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "sw_rt.h"
#include "sw_program.h"

u64 sw_tick_now(void);
extern const SwFuncEntry sw_functions[];
extern const u32 sw_function_count;
extern const SwModuleDesc sw_modules[];

#define MAX_HOOKS 16
typedef struct {
    u64 addr;
    void (*orig)(SwCpu *c);
    volatile long calls, logged, rets;
} Hook;
static Hook g_hooks[MAX_HOOKS];
static int g_nhooks;

static void hook_log(int i, SwCpu *c)
{
    Hook *h = &g_hooks[i];
    long n = InterlockedIncrement(&h->calls);
    if (InterlockedIncrement(&h->logged) <= 200)
        fprintf(stderr, "[hook] 0x%llx #%ld x0 0x%llx x1 0x%llx x2 0x%llx s0 %g s1 %g lr 0x%llx\n",
                (unsigned long long)h->addr, n, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                (unsigned long long)c->x[2], c->v[0].f[0], c->v[1].f[0], (unsigned long long)c->x[30]);
    static u64 last;
    u64 now = sw_tick_now();
    if (!last) last = now;
    if (now - last > 19200000ull * 10) {
        last = now;
        for (int k = 0; k < g_nhooks; k++)
            fprintf(stderr, "[hook] 0x%llx: %ld calls\n", (unsigned long long)g_hooks[k].addr,
                    InterlockedExchange(&g_hooks[k].calls, 0));
    }
}

static void hook_ret(int i, SwCpu *c)
{
    Hook *h = &g_hooks[i];
    long n = InterlockedIncrement(&h->rets);
    if (n <= 200)
        fprintf(stderr, "[hook] 0x%llx ret #%ld x0 0x%llx\n", (unsigned long long)h->addr, n, (unsigned long long)c->x[0]);
}

#define WRAP(i) static void wrap_##i(SwCpu *c) { hook_log(i, c); g_hooks[i].orig(c); hook_ret(i, c); }
WRAP(0) WRAP(1) WRAP(2) WRAP(3) WRAP(4) WRAP(5) WRAP(6) WRAP(7)
WRAP(8) WRAP(9) WRAP(10) WRAP(11) WRAP(12) WRAP(13) WRAP(14) WRAP(15)
static void (*const g_wraps[MAX_HOOKS])(SwCpu *) = {wrap_0, wrap_1, wrap_2, wrap_3, wrap_4, wrap_5, wrap_6, wrap_7,
                                                    wrap_8, wrap_9, wrap_10, wrap_11, wrap_12, wrap_13, wrap_14, wrap_15};

static int install(u64 addr, void (*wrapper)(SwCpu *), void (**orig)(SwCpu *))
{
    for (u32 i = 0; i < sw_function_count; i++) {
        if (sw_functions[i].addr != addr) continue;
        SwFuncEntry *e = (SwFuncEntry *)&sw_functions[i];
#if defined(_WIN32)
        DWORD old;
        if (!VirtualProtect(&e->fn, sizeof e->fn, PAGE_READWRITE, &old)) return 0;
        *orig = e->fn;
        e->fn = wrapper;
        VirtualProtect(&e->fn, sizeof e->fn, old, &old);
        { extern void sw_fn_hooked(u32 i); sw_fn_hooked(i); }   /* off the direct-call path */
        return 1;
#else
        return 0;
#endif
    }
    return 0;
}

/* SW_DT_PROBE=1: 0x2863924 steps the world by (i64 ns at [vfn0([[x21]], 1) + 8]) / 1e9 and passes it to
 * 0x2868434 (x1 = that x21).  Log the timer object and its virtual function (finding the ns writer). */
static void (*g_dtp_orig)(SwCpu *c);
static void dtp_wrap(SwCpu *c)
{
    static volatile long n;
    if (InterlockedIncrement(&n) <= 6) {
        u64 holder = c->x[1], obj = *(u64 *)(uintptr_t)holder, vt = *(u64 *)(uintptr_t)obj;
        fprintf(stderr, "[dtp] s0 %g holder 0x%llx obj 0x%llx vtable 0x%llx vfn0 0x%llx lr 0x%llx\n", c->v[0].f[0],
                (unsigned long long)holder, (unsigned long long)obj, (unsigned long long)vt,
                (unsigned long long)*(u64 *)(uintptr_t)vt, (unsigned long long)c->x[30]);
        for (int k = 0; k < 8; k++)
            fprintf(stderr, "[dtp]   obj+%#x: 0x%llx\n", k * 8, (unsigned long long)((u64 *)(uintptr_t)obj)[k]);
        if (n == 6 && getenv("SW_DT_PROBE")[1] == 'w') { extern void sw_watch_writes(u64); sw_watch_writes(obj + 0x20); }
    }
    g_dtp_orig(c);
}

static void (*g_dts_orig)(SwCpu *c);
static void dts_wrap(SwCpu *c)
{
    static volatile long n;
    if (n == 0) {
        const u64 *vt = *(const u64 **)(uintptr_t)c->x[0];
        for (int k = 0; k < 8; k++) fprintf(stderr, "[dts] vtable 0x%llx [%d] = 0x%llx\n", (unsigned long long)(uintptr_t)vt, k, (unsigned long long)vt[k]);
    }
    if (InterlockedIncrement(&n) <= 40)
        fprintf(stderr, "[dts] set obj 0x%llx slot %d ns %lld lr 0x%llx x19 0x%llx x20 0x%llx\n", (unsigned long long)c->x[0],
                (int)c->x[1], (long long)c->x[2], (unsigned long long)c->x[30], (unsigned long long)c->x[19],
                (unsigned long long)c->x[20]);
    g_dts_orig(c);
}

/* ---- Render resolution ----------------------------------------------------
 * 0x2aadd98 (build 7FCAD279) picks the 3D render size: after GetPerformanceMode (0 normal / handheld, 1 boost /
 * docked) it reads two config values and, where those are 0, the renderer's table this+0x3f0 {w normal, w boost},
 * this+0x3f8 {h normal, h boost}; the result goes to [0x43aa1d8]+0x128 (w) / +0x12c (h).
 * SW_RENDER_RES=<w>x<h> writes that size into both table entries before the function runs. */
static int build_ok(void);
static void (*g_rres_orig)(SwCpu *c);
static unsigned g_rres_w, g_rres_h;
static void rres_wrap(SwCpu *c)
{
    u32 *tab = (u32 *)(uintptr_t)(c->x[0] + 0x3f0);
    fprintf(stderr, "[res] render size table w %u/%u h %u/%u\n", tab[0], tab[1], tab[2], tab[3]);
    if (g_rres_w) tab[0] = tab[1] = g_rres_w, tab[2] = tab[3] = g_rres_h;
    g_rres_orig(c);
    u64 g = *(u64 *)(uintptr_t)0x43aa1d8ull;
    if (g) fprintf(stderr, "[res] render size now %ux%u\n", *(u32 *)(uintptr_t)(g + 0x128), *(u32 *)(uintptr_t)(g + 0x12c));
}
static void rres_init(void)
{
    const char *e = getenv("SW_RENDER_RES");
    unsigned w, h;
    if (!e || !*e) { if (!getenv("SW_RT_TRACE")) return; }
    else if (sscanf(e, "%ux%u", &w, &h) == 2 && w >= 640 && h >= 360 && w <= 7680 && h <= 4320) g_rres_w = w & ~1u, g_rres_h = h & ~1u;
    if (!build_ok()) return;
    if (install(0x2aadd98ull, rres_wrap, &g_rres_orig))
        fprintf(stderr, "[res] render size hook (%ux%u)\n", g_rres_w, g_rres_h);
}

static void fps_hooks_init(void);
static void lua_probe_init(void);
void sw_hooks_init(void)
{
    rres_init();
    { extern void sw_newfn_init(void); sw_newfn_init(); }   /* SW_NEWFN=1 (sw_runtime.c) */
    fps_hooks_init();
    lua_probe_init();
    { const char *p = getenv("SW_DT_PROBE"); if (p && *p == '1' && install(0x2868434ull, dtp_wrap, &g_dtp_orig)) fprintf(stderr, "[dtp] probing\n"); }
    { const char *p = getenv("SW_DT_PROBE"); if (p && *p == '1' && install(0x283f0e4ull, dts_wrap, &g_dts_orig)) fprintf(stderr, "[dtp] probing setter\n"); }
    const char *e = getenv("SW_HOOK_LOG");
    while (e && *e && g_nhooks < MAX_HOOKS) {
        char *end;
        u64 a = strtoull(e, &end, 0);
        if (end == e) break;
        Hook *h = &g_hooks[g_nhooks];
        h->addr = a;
        if (install(a, g_wraps[g_nhooks], &h->orig)) {
            fprintf(stderr, "[hook] logging calls of 0x%llx\n", (unsigned long long)a);
            g_nhooks++;
        } else
            fprintf(stderr, "[hook] 0x%llx: no such recompiled function\n", (unsigned long long)a);
        e = *end == ',' ? end + 1 : end;
    }
}

/* ---- Frame rate (30 / 60 fps) ----------------------------------------------
 * How the game steps its world (build 7FCAD279, 1.0.0; found 2026-11-07 with
 * SW_BISECT=3 + SW_DT_PROBE): every frame 0x2844600.. calls the frame timer's
 * virtual 0x2846238(timer, slot, index) with index 1 for slots 0..3.  It looks up
 * ns = table[index] (rodata 0x3a3d3a0: 1/60, 1/30, 1/20, 1/15, 1/12 s) and stores it
 * via 0x283f0e4 at timer + slot*16 + 0x10.  Readers turn it into seconds
 * (ns / 1e9, e.g. 0x2863dd4, 0x28446a4).  So the step is a fixed 1/30 s per frame:
 * showing frames faster (pacer only) ran the whole game at double speed.
 *
 * 60 fps: the hook replaces index 1 with the real display interval of the last
 * frames (moving average of 5 presents, from the pacer's display times), at least
 * 1/60 s and at most 1/30 s: game speed follows the wall clock at any frame rate
 * between 30 and 60; below 30 the game slows down, as on the console.  Index 0
 * (paused timers) and other indexes are left alone.  30 fps / off: untouched.
 * SW_FPS=60 at start; sw_fps_set() from the in-game menu.  SW_FPS_STEP=fixed: always
 * 1/60 s at 60 fps (no measuring). */
#define STEP_FN      0x2846238ull   /* timer vfn 1: set(slot, table index) */
#define STEP_SETTER  0x283f0e4ull   /* timer vfn 2: set(slot, ns) */
#define STEP_NS_60   16666667ll
#define STEP_NS_30   33333334ll
static volatile int g_fps_target = -1;      /* -1: read SW_FPS once */
static volatile long long g_step_ns = STEP_NS_60;
static int g_step_fixed;
static int g_build_ok = -1;

static int build_ok(void)
{
    if (g_build_ok < 0) {
        g_build_ok = strcmp(sw_modules[0].module_id, "7fcad279539de183b25c11834fd4a030591cfe25000000000000000000000000") == 0;
        if (!g_build_ok) fprintf(stderr, "[fps] unknown game build: frame rate option off\n");
    }
    return g_build_ok;
}
static void fps_init(void)
{
    if (g_fps_target >= 0) return;
    const char *e = getenv("SW_FPS");
    int f = e ? atoi(e) : 0;
    /* 240: the highest option (menu and launcher); on most PCs above what the game reaches, so it also shows the
     * headroom above 120 instead of the cap */
    g_fps_target = f == 60 || f == 30 || f == 90 || f == 120 || f == 240 ? f : 0;
    e = getenv("SW_FPS_STEP");
    g_step_fixed = e && !strcmp(e, "fixed");
}
void sw_fps_set(int fps) { fps_init(); g_fps_target = fps == 60 || fps == 30 || fps == 90 || fps == 120 || fps == 240 ? fps : 0; extern volatile int g_fps_mode; g_fps_mode = g_fps_target; }
int sw_fps_get(void) { fps_init(); return g_fps_target; }

/* Called once per present: the interval the pacer should use (vsyncs per frame;
 * 0 = the game's own nvnWindowSetPresentInterval value, 2). */
int sw_fps_interval(void)
{
    fps_init();
    if (!g_fps_target || !build_ok()) return 0;
    return g_fps_target >= 60 ? 1 : 2;
}

/* 90 / 120 fps are not a whole number of 60 Hz vsyncs: the pacer (nvn_api_obj.c present_period) uses this
 * period instead of interval * 1/60 s.  0 = the vsync interval above (30 / 60 fps as before).  On a 60 Hz
 * display with vsync (FIFO) the frames still reach the screen at most 60 times a second. */
unsigned long long sw_fps_period_ns(void)
{
    fps_init();
    if (!build_ok() || g_fps_target <= 60) return 0;
    return 1000000000ull / (unsigned long long)g_fps_target;
}

/* Display time of each present (nvn_api_obj.c, the pacer's slot, ns): the world step at 60 / 90 / 120 / 240 fps,
 * between 1/target s and 1/30 s (below 30 fps the game slows down, as on the console), in whole fractions of a
 * 30 fps frame (below). */
void sw_frame_step_present(uint64_t slot)
{
    enum { N = 5 };
    static uint64_t last; static long long smp[N]; static int cnt, next;
    if (last && slot > last) {
        long long d = (long long)(slot - last);
        if (d > 250000000ll) cnt = 0;                     /* loading / a stall: start over */
        else { smp[next] = d; next = (next + 1) % N; if (cnt < N) cnt++; }
    }
    last = slot;
    long long lo = g_fps_target > 60 ? 1000000000ll / g_fps_target : STEP_NS_60;
    long long avg = lo;
    if (cnt) { long long s = 0; for (int i = 0; i < cnt; i++) s += smp[i]; avg = s / cnt; }
    if (avg < lo) avg = lo;
    if (avg > STEP_NS_30) avg = STEP_NS_30;
    {   /* Whole-fraction step (default; SW_FPS_STEP=avg: the plain average above).  Game code adds the step to
         * animation frames and checks them against whole frames (UI animations, measured with SW_UI_PROBE: at 120 fps
         * 0.25 per call, but every off step such as 0.326 after a late frame put the animation off the grid of whole
         * frames for good, so "frame == N" checks never fired again).
         * Only while the game holds a rate: whole units at a lower real rate (e.g. 1,1,1,1,2 quarters at ~100 fps
         * with a 120 target) made everything visibly stutter (user, 2026-11-18).  So the 5-frame average stays the
         * step, and it is put on the exact value of the nearest of 240/120/60/30 fps (8333333 ns -> 0.25 of a
         * frame, 16666667 -> 0.5, 33333334 -> 1, measured) only when it is within 8 % of it. */
        static int mode = -1;
        if (mode < 0) { const char *e = getenv("SW_FPS_STEP"); mode = !(e && !strcmp(e, "avg")); }
        if (mode && cnt) {
            static const long long exact[4] = {4166667ll, 8333333ll, STEP_NS_60, STEP_NS_30};
            for (int i = 0; i < 4; i++)
                if (llabs(avg - exact[i]) * 100 <= exact[i] * 8 && exact[i] >= lo) { avg = exact[i]; break; }
        }
    }
    g_step_ns = avg;
    {   /* SW_DD_SWITCH=<s>: test of the in-game render distance change - level 4 (Max) s seconds after the first present */
        static double at = -1; static ULONGLONG t0; static int done;
        if (at < 0) { const char *e = getenv("SW_DD_SWITCH"); at = e ? atof(e) : 0; }
        if (at > 0 && !done) {
            ULONGLONG now = GetTickCount64();
            if (!t0) t0 = now;
            if ((double)(now - t0) / 1000.0 >= at) { extern void sw_drawdist_set(int); sw_drawdist_set(4); done = 1; }
        }
    }
    {   /* [fps] speed: game time (sum of steps) over display time, every 10 s at 60+ fps (1.00 = wall clock) */
        static double gsum, rsum; static ULONGLONG t0;
        if (g_fps_target >= 60 && cnt) {
            gsum += (double)avg, rsum += (double)smp[(next + N - 1) % N];
            ULONGLONG now = GetTickCount64();
            if (!t0) t0 = now;
            if (now - t0 >= 10000) {
                fprintf(stderr, "[fps] speed %.3f of the wall clock (step %.2f ms, %d fps target)\n", gsum / rsum,
                        avg / 1e6, g_fps_target);
                gsum = rsum = 0, t0 = now;
            }
        }
    }
}

static void (*g_step_orig)(SwCpu *c);
/* Debug tab "Game speed" (sw_overlay.cpp): the world step is multiplied by it - slow motion below 1, fast forward
 * above.  Effects and frame-counted parts keep their own pace. */
static volatile float g_game_speed = 1.0f;
void sw_game_speed_set(float s) { g_game_speed = s < 0.1f ? 0.1f : s > 4.0f ? 4.0f : s; }
float sw_game_speed_get(void) { return g_game_speed; }
static void step_wrap(SwCpu *c)
{
    {   /* SW_STEP_LOG=1: each distinct (timer class setter, slot, index) that reaches the frame step (60 fps work) */
        static int on = -1; static u64 seen[64][3]; static volatile long n;
        if (on < 0) { const char *e = getenv("SW_STEP_LOG"); on = e && *e == '1'; }
        if (on && n < 64) {
            const u64 *vt = *(const u64 **)(uintptr_t)c->x[0];
            u64 k0 = vt[2], k1 = c->x[1], k2 = c->x[2];
            int found = 0;
            for (long i = 0; i < n && !found; i++) found = seen[i][0] == k0 && seen[i][1] == k1 && seen[i][2] == k2;
            if (!found) {
                long i = InterlockedIncrement(&n) - 1;
                if (i < 64) seen[i][0] = k0, seen[i][1] = k1, seen[i][2] = k2;
                fprintf(stderr, "[fps] step call: timer 0x%llx setter 0x%llx slot %lld index %lld lr 0x%llx\n",
                        (unsigned long long)c->x[0], (unsigned long long)k0, (long long)k1, (long long)k2,
                        (unsigned long long)c->x[30]);
            }
        }
    }
    if (g_fps_target >= 60 && (int)c->x[2] == 1) {
        const u64 *vt = *(const u64 **)(uintptr_t)c->x[0];
        if (vt[2] == STEP_SETTER) {                       /* the timer class we know: store as 0x283f0e4 does */
            long long ns = g_step_fixed ? 1000000000ll / g_fps_target : g_step_ns;
            if (g_game_speed != 1.0f) ns = (long long)((double)ns * g_game_speed);
            *(volatile long long *)(uintptr_t)(c->x[0] + (u64)((long long)(int)c->x[1] * 16) + 0x10) = ns;
            static volatile long logged;
            if (InterlockedIncrement(&logged) == 1)
                fprintf(stderr, "[fps] %d fps: world step %s (now %.2f ms)\n", g_fps_target,
                        g_step_fixed ? "fixed 1/target s" : "follows the display time", ns / 1e6);
            return;
        }
    }
    g_step_orig(c);
    if (g_game_speed != 1.0f && (int)c->x[2] == 1) {   /* 30 fps: the game stored its own step; scale it */
        const u64 *vt = *(const u64 **)(uintptr_t)c->x[0];
        if (vt[2] == STEP_SETTER) {
            volatile long long *at = (volatile long long *)(uintptr_t)(c->x[0] + (u64)((long long)(int)c->x[1] * 16) + 0x10);
            *at = (long long)((double)*at * g_game_speed);
        }
    }
}
/* SW_ARGLOG=addr[,addr2]: log s0..s3 (as floats), x0..x2 and lr of the first 12 calls to each (60 fps work:
 * where a fixed per-frame time step is passed in) */
static u64 g_al_addr[2];
static void (*g_al_orig[2])(SwCpu *c);
static volatile long g_al_n[2];
static void al_log(SwCpu *c, int k)
{
    static long every = -1;   /* SW_ARGLOG_EVERY=N: every Nth call (12 lines per address) */
    if (every < 0) { const char *e = getenv("SW_ARGLOG_EVERY"); every = e && atol(e) > 0 ? atol(e) : 1; }
    long n = InterlockedIncrement(&g_al_n[k]);
    if (n % every == 0 && n / every <= 12)
        fprintf(stderr, "[arglog] 0x%llx: s0 %g s1 %g s2 %g s3 %g x0 0x%llx x1 0x%llx x2 0x%llx lr 0x%llx\n",
                (unsigned long long)g_al_addr[k], (double)c->v[0].f[0], (double)c->v[1].f[0], (double)c->v[2].f[0],
                (double)c->v[3].f[0], (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                (unsigned long long)c->x[2], (unsigned long long)c->x[30]);
}
static void al_wrap0(SwCpu *c) { al_log(c, 0); g_al_orig[0](c); }
static void al_wrap1(SwCpu *c) { al_log(c, 1); g_al_orig[1](c); }
static void arglog_init(void)
{
    const char *e = getenv("SW_ARGLOG");
    if (!e) return;
    unsigned long long a = 0, b = 0;
    int n = sscanf(e, "%llx,%llx", &a, &b);
    g_al_addr[0] = a, g_al_addr[1] = b;
    if (n >= 1 && install(a, al_wrap0, &g_al_orig[0])) fprintf(stderr, "[arglog] on 0x%llx\n", a);
    if (n >= 2 && install(b, al_wrap1, &g_al_orig[1])) fprintf(stderr, "[arglog] on 0x%llx\n", b);
}

/* WaterCurrentTime (water animation): main 0x2abe17c reads the render parameter, adds a fixed 1/30 s (0x3d088889),
 * wraps it at 10000 and stores it back through the parameter setter (main 0x2ab06d4, called via blr from 0x2abe1cc,
 * return address 0x2abe1d0).  At 60/90/120 fps that made water move 2-4x too fast.  The setter is wrapped and, for
 * that call site only, the 1/30 s just added becomes the real frame step (g_step_ns, as for the world step).
 * 30 fps / off: untouched.  SW_WATER_STEP=0 turns it off.  Build 7FCAD279 only (build_ok). */
#define WATER_SETTER 0x2ab06d4ull
#define WATER_SITE   0x2abe1d0ull
static void (*g_water_orig)(SwCpu *c);
static void water_wrap(SwCpu *c)
{
    if (c->x[30] == WATER_SITE && g_fps_target >= 60) {
        float *v = (float *)(uintptr_t)c->x[2];
        const float step30 = 1.0f / 30.0f;
        if (v && *v != 0.0f) {          /* 0: it just wrapped at 10000 */
            long long ns = g_step_fixed ? 1000000000ll / g_fps_target : g_step_ns;
            *v = *v - step30 + (float)((double)ns / 1e9);
        }
    }
    g_water_orig(c);
}

/* This frame's world step in units of the original 1/30 s frame (1 at 30 fps / off; 0.5 at a steady 60). */
static double frame_scale(void)
{
    if (g_fps_target < 60) return 1.0;
    long long ns = g_step_fixed ? 1000000000ll / g_fps_target : g_step_ns;
    return (double)ns * 30.0 / 1e9;
}

/* Camera turn speed (build 7FCAD279, found 2026-11-11 with SW_FIND_RATE): the field camera (virtual main 0x1a0bce4,
 * once per frame) does yaw += P * stickX * ..., pitch += P * stickY * ... where P = main 0x1a0ae3c(), a tuning
 * parameter looked up by name hash: a fixed angle per frame, no frame time.  Measured: 1.96 rad/s at 30 fps,
 * 4.1 rad/s at ~60.  Two other camera modes use P the same way (sites 0x1a0444c, 0x1a047e8: [..] += P * ... per
 * frame).  The hook scales P by frame_scale() for those three call sites only.  SW_CAM_STEP=0 turns it off. */
#define CAM_SPEED_FN 0x1a0ae3cull
static void (*g_camspd_orig)(SwCpu *c);
volatile long long sw_field_since;   /* QPC time of the first field-camera frame (benchmark routes start from it) */
static void camspd_wrap(SwCpu *c)
{
    u64 lr = c->x[30];
    g_camspd_orig(c);
    if ((lr == 0x1a0bde8ull || lr == 0x1a0444cull || lr == 0x1a047e8ull) && g_fps_target >= 60)
        c->v[0].f[0] = (float)(c->v[0].f[0] * frame_scale());
}

/* Camera auto-turn (main 0x1a05724, called once per frame from the camera update 0x1a05668): a ramp at
 * this+0x3ec moves by a fixed amount per frame (+[0x450] / -[0x454], clamped 0..1) and the yaw at this+0x360 moves by
 * [0x44c] * ramp per frame (wrapped to +-pi).  At 60+ fps both ran 2x as fast.  Wrapped: the changes of the ramp
 * and the yaw made by this call are scaled by frame_scale() (exact once the ramp is at its limit; while it ramps the
 * yaw step uses the unscaled ramp of this frame - a slight difference during the first frames only). */
#define CAM_AUTO_FN 0x1a05724ull
static void (*g_camauto_orig)(SwCpu *c);
static void camauto_wrap(SwCpu *c)
{
    if (!sw_field_since) {   /* the field camera update calls this every frame: the player is in the world */
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        sw_field_since = q.QuadPart;
        fprintf(stderr, "[field] first field camera frame: in the world\n");
    }
    if (g_fps_target < 60) { g_camauto_orig(c); return; }
    float *yaw = (float *)(uintptr_t)(c->x[0] + 0x360), *ramp = (float *)(uintptr_t)(c->x[0] + 0x3ec);
    float y0 = *yaw, r0 = *ramp;
    g_camauto_orig(c);
    double k = frame_scale();
    if (k >= 1.0) return;
    float y1 = *yaw, r1 = *ramp;
    if (r1 != r0) *ramp = (float)(r0 + (r1 - r0) * k);
    if (y1 != y0) {
        const double PI = 3.14159265358979323846;
        double d = remainder((double)y1 - (double)y0, 2 * PI);    /* the turn made by this call, without wraps */
        double y = (double)y0 + d * k;
        if (fabs((double)y1) <= PI) y = remainder(y, 2 * PI);     /* the game wrapped its result: keep it wrapped */
        *yaw = (float)y;
    }
}

/* ---- Frame-counted systems above 30 fps (build 7FCAD279) ------------------------------------------------------
 * The game advances these by a fixed amount per rendered frame, not by the frame step, so above 30 fps they run
 * fps/30 times fast (2x at 60, 4x at 120): battle move sequences ran ahead of the Pokemon and their effects (moves
 * not shown), fades ended in a few frames.  Functions found by masked code match with the 1.1.1 sites described in
 * NXAevora docs/arceus-timing.md (ideas only; own code).  Each object gets the time of the rendered frames (k =
 * step * 30 sequence frames per call) instead of one whole frame per call.  SW_FIX_BATTLE / SW_FIX_FADE /
 * SW_FIX_SCROLL=0 turn one off; at 30 fps (or k >= 1) all of them call the game unchanged. */
#define WBIN_UPDATE  0xad7facull    /* battle .wbin sequence update(seq): +E8 frame f32, +EC end f32, +F0 speed f32 */
#define FADE_UPDATE  0xd1cd00ull    /* common fade(obj): +500 mode (0 linear), +504 state, +538 frame i32, +53C length,
                                       +540 color f32x4, +550 delta f32x4 */
#define LIST_SCROLL  0x27f2350ull   /* UI list scroller(obj): eases and free scrolls counted in calls */

typedef struct { u64 obj; double acc; float last; int used; } FixSlot;
static volatile long g_fix_lock;
static void fix_lock(void) { while (InterlockedCompareExchange(&g_fix_lock, 1, 0)) YieldProcessor(); }
static void fix_unlock(void) { InterlockedExchange(&g_fix_lock, 0); }
/* the slot of obj (a free or the oldest one when it has none); called with the lock held */
static FixSlot *fix_slot(FixSlot *t, int n, u64 obj, int *fresh)
{
    static int clock_hand;
    for (int i = 0; i < n; i++) if (t[i].used && t[i].obj == obj) { *fresh = 0; return &t[i]; }
    for (int i = 0; i < n; i++) if (!t[i].used) { t[i].used = 1, t[i].obj = obj, t[i].acc = 0; *fresh = 1; return &t[i]; }
    FixSlot *s = &t[clock_hand++ % n];
    s->obj = obj, s->acc = 0; *fresh = 1;
    return s;
}
static int fix_on(const char *env, int *cache)
{
    if (*cache < 0) { const char *e = getenv(env); *cache = !(e && *e == '0'); }
    return *cache;
}
/* k = sequence frames per rendered frame (1 at 30 fps); 0: leave the game unchanged */
static double fix_k(void)
{
    if (g_fps_target < 60) return 0;
    double k = frame_scale();
    return k > 0.01 && k < 0.999 ? k : 0;
}
/* a 30 Hz tick on the rendered frame nearest to it: acc collects k per call */
static int fix_tick(double *acc, double k)
{
    *acc += k;
    if (*acc >= 1.0 - k * 0.5) { *acc -= 1.0; if (*acc < -1.0) *acc = 0; return 1; }
    if (*acc > 2.0) *acc = 0;
    return 0;
}

/* Battle sequences: frame += speed once per call (main+9D7FAC; 1.1.1 main+9DBEAC).  The call gets speed * k, and
 * a call that would pass a whole frame ends on it (the commands and the end check fire only on whole frames); the
 * difference is carried, so the sequence keeps its authored length in seconds.  Speeds above 1 per 30 fps frame
 * already skip whole frames in the game: those are only scaled. */
static void (*g_wbin_orig)(SwCpu *c);
static FixSlot g_wbin_slots[64];
static volatile long g_wbin_calls;
static void wbin_wrap(SwCpu *c)
{
    static int on = -1;
    double k = fix_on("SW_FIX_BATTLE", &on) ? fix_k() : 0;
    u64 o = c->x[0];
    float *pf = (float *)(uintptr_t)(o + 0xe8), *pe = (float *)(uintptr_t)(o + 0xec), *ps = (float *)(uintptr_t)(o + 0xf0);
    float s = *ps, f = *pf;
    if (!k || !(s > 0.0f && s <= 4.0f) || !(f < *pe)) { g_wbin_orig(c); return; }
    fix_lock();
    int fresh;
    FixSlot *sl = fix_slot(g_wbin_slots, 64, o, &fresh);
    if (!fresh && sl->last != f) sl->acc = 0;          /* the game set the frame (a new sequence or a jump) */
    double share = (double)s * k, want = share + sl->acc;
    float adv;
    if (s <= 1.0f) {
        float whole = floorf(f) + 1.0f, dist = whole - f;
        adv = want >= (double)dist - share * 0.5 ? dist : (float)(want > 0 ? want : 0);
        sl->acc = want - (double)adv;
        if (sl->acc > share) sl->acc = share;
        if (sl->acc < -share) sl->acc = -share;
    } else {
        adv = (float)share;
        sl->acc = 0;
    }
    fix_unlock();
    if (adv <= 0.0f) { sl->last = f; return; }          /* nothing due this frame: the call is made on a later one */
    *ps = adv;
    g_wbin_orig(c);
    if (*ps == adv) *ps = s;                             /* the authored speed back, unless the call set a new one */
    fix_lock();
    if (sl->obj == o) sl->last = *pf;
    fix_unlock();
    if (InterlockedIncrement(&g_wbin_calls) == 1)
        fprintf(stderr, "[fps] battle sequence fix: first sequence frame step %.3f (k %.3f)\n", (double)adv, k);
}

/* Fades (main+C1CD00; 1.1.1 main+C21490): in linear mode each call adds delta to the color and 1 to the frame.  Each
 * call now adds delta * k, and the frame (which ends the fade) moves on 30 Hz ticks only. */
static void (*g_fade_orig)(SwCpu *c);
static FixSlot g_fade_slots[32];
static void fade_wrap(SwCpu *c)
{
    static int on = -1;
    double k = fix_on("SW_FIX_FADE", &on) ? fix_k() : 0;
    u64 o = c->x[0];
    u32 mode = *(u32 *)(uintptr_t)(o + 0x500), st = *(u32 *)(uintptr_t)(o + 0x504);
    int32_t *pfr = (int32_t *)(uintptr_t)(o + 0x538), len = *(int32_t *)(uintptr_t)(o + 0x53c);
    float *delta = (float *)(uintptr_t)(o + 0x550);
    if (!k || mode != 0 || st < 2 || st == 4 || *pfr >= len) { g_fade_orig(c); return; }
    fix_lock();
    int fresh;
    FixSlot *sl = fix_slot(g_fade_slots, 32, o, &fresh);
    if (!fresh && sl->last != (float)*pfr) sl->acc = 0;   /* a new fade (the start functions reset the frame) */
    int tick = fix_tick(&sl->acc, k);
    fix_unlock();
    float d0[4], d1[4];
    memcpy(d0, delta, 16);
    for (int i = 0; i < 4; i++) d1[i] = (float)(d0[i] * k);
    memcpy(delta, d1, 16);
    if (!tick) *pfr -= 1;                                /* the call adds 1 back: the frame stays */
    g_fade_orig(c);
    if (!memcmp(delta, d1, 16)) memcpy(delta, d0, 16);
    fix_lock();
    if (sl->obj == o) { sl->last = (float)*pfr; if (*pfr >= len) sl->used = 0; }
    fix_unlock();
}

/* UI list scrolling (main+26F2350; 1.1.1 main+272F150): the one-row ease and the free scroll count calls, so above
 * 30 fps a tap ran the free scroll.  The scroller runs on 30 Hz ticks of each list. */
static void (*g_scroll_orig)(SwCpu *c);
static FixSlot g_scroll_slots[32];
static void scroll_wrap(SwCpu *c)
{
    static int on = -1;
    double k = fix_on("SW_FIX_SCROLL", &on) ? fix_k() : 0;
    if (!k) { g_scroll_orig(c); return; }
    fix_lock();
    int fresh;
    FixSlot *sl = fix_slot(g_scroll_slots, 32, c->x[0], &fresh);
    int tick = fresh || fix_tick(&sl->acc, k);
    fix_unlock();
    if (tick) g_scroll_orig(c);
}

/* Physics (main+2AB57A4; 1.1.1 main+2AF25B4 in NXAevora's notes, found by its instruction guard): the main rigid-body
 * world wrapper gets the elapsed time in s0, but while byte this+0x121 is 0 (always, here) it steps a fixed 1/30 s, once
 * per rendered frame: above 30 fps the physics world ran fps/30 times fast - characters standing at the edge of an NPC's
 * talk collision were pushed in and out (the A "Talk" prompt flickered after the player moved).  The call gets the byte
 * set and min(1/30, elapsed) (NXAevora's FMINNM).  SW_FIX_PHYSICS=0 turns it off. */
#define PHYS_STEP 0x2bb57a4ull
static void (*g_phys_orig)(SwCpu *c);
static void phys_wrap(SwCpu *c)
{
    static int on = -1;
    double k = fix_on("SW_FIX_PHYSICS", &on) ? fix_k() : 0;
    u8 *flag = (u8 *)(uintptr_t)(c->x[0] + 0x121);
    if (!k || *flag) { g_phys_orig(c); return; }
    float dt = (float)(k / 30.0), in = c->v[0].f[0];
    if (in == in && in > 0 && in < dt) dt = in;          /* the smaller of the step and the elapsed input */
    c->v[0].f[0] = dt;
    *flag = 1;
    g_phys_orig(c);
    *flag = 0;
    static volatile long logged;
    if (InterlockedIncrement(&logged) == 1)
        fprintf(stderr, "[fps] physics step %.4f s (input %.4f, k %.3f)\n", (double)dt, (double)in, k);
}

/* Thrown-object flight (main+1A4014C; 1.1.1 main+1A7191C, NXAevora notes, guard words identical): with the physics step
 * on real time, each call still applied drag (+5A0), lift (+5A4) and the flight clock (+618 elapsed, +61C planned) as
 * for 1/30 s, so balls flew in slow motion.  While in flight (+5F8 == 1) a call sees them scaled to its share r of
 * 1/30 s (drag as 1-(1-d)^r), and the authored values are put back after it.  Only with the physics fix. */
#define BALL_FLIGHT 0x1b4014cull
static void (*g_ball_orig)(SwCpu *c);
static void ball_wrap(SwCpu *c)
{
    static int on = -1;
    double k = fix_on("SW_FIX_PHYSICS", &on) ? fix_k() : 0;
    u64 o = c->x[0];
    if (!k || *(u32 *)(uintptr_t)(o + 0x5f8) != 1) { g_ball_orig(c); return; }
    float r = (float)k;
    if (r < 0.0625f) r = 0.0625f;
    float *drag = (float *)(uintptr_t)(o + 0x5a0), *lift = (float *)(uintptr_t)(o + 0x5a4);
    float *el = (float *)(uintptr_t)(o + 0x618), *pl = (float *)(uintptr_t)(o + 0x61c);
    float d0 = *drag, l0 = *lift, p0 = *pl;
    float d1 = d0 >= 0.0f && d0 < 1.0f ? 1.0f - powf(1.0f - d0, r) : d0 * r, l1 = l0 * r, p1 = p0 / r;
    *el = *el / r, *pl = p1, *drag = d1, *lift = l1;
    g_ball_orig(c);
    if (*drag == d1) *drag = d0;
    if (*lift == l1) *lift = l0;
    if (*pl == p1) *pl = p0, *el = *el * r;
}

/* ---- Render distance (built in; level from sw_romfs.c: Original, 2x, 3x, 5x, 10x) ---------------------------
 * The game's misc/app_config/field_common_config.bin settings are parsed once at start into FNV-1a keyed maps (offset
 * basis 0xCBF29CE484222645); the users read them through getters.  Found 2026-11-18 (code constants of the key
 * hashes): the tree system's setup (main+0x242B41C) reads kTreeVisibleDistance, kTreeFadeDistanceFar and
 * kDetailFadeCoefficient once and keeps them in its object (+0x430 / +0x448 float ranges, +0x460), which the tree
 * drawing (main+0x298E068) reads every frame - so they are changed there, at once.  Jubilife Village's people and
 * Pokemon load radius (kArea00NpcLoadRadius / kArea00NpcPokeLoadRadius, ints) are read when the village loads: the
 * int getter (main+0xC747C0) returns them scaled.  The GameBanana mod "Trees/NPC draw distance + LoD" changes the
 * same settings (10x trees and village radius, detail fade 0.75 -> 0.12): level 4 matches it. */
static const float kDDFactor[5] = {1.0f, 2.0f, 3.0f, 5.0f, 10.0f};
static float dd_factor(void) { extern int sw_drawdist_get(void); return kDDFactor[sw_drawdist_get()]; }

#define CFG_GETI 0xd747c0ull                /* int getter: x0 = config object, x1 -> {setting hash, ...}; w0 */
#define KEY_AREA00_NPC_POKE 0xb44dd4cfc8371ff1ull
#define KEY_AREA00_NPC      0xb8a9a4a4b807b964ull
static void (*g_cfggeti_orig)(SwCpu *c);
static void cfggeti_wrap(SwCpu *c)
{
    u64 key = c->x[1] ? *(u64 *)(uintptr_t)c->x[1] : 0;
    g_cfggeti_orig(c);
    if (key == KEY_AREA00_NPC_POKE || key == KEY_AREA00_NPC) {
        extern int sw_npcdist_get(void);
        float m = kDDFactor[sw_npcdist_get()];   /* the people & pokemon distance level (was the tree level) */
        if (m != 1.0f) c->x[0] = (u64)(u32)(int32_t)((float)(int32_t)c->x[0] * m + 0.5f);
    }
}

#define TREE_SETUP 0x252b41cull
static void (*g_tree_orig)(SwCpu *c);
static volatile u64 g_tree_obj[8];
static float g_tree_base[8][5];             /* each system's own values: visible range, fade far range, detail fade */
void sw_drawdist_apply_trees(void)
{
    float m = dd_factor();
    for (int i = 0; i < 8; i++) {
        u64 o = g_tree_obj[i];
        if (!o) continue;
        float *vis = (float *)(uintptr_t)(o + 0x430), *far_ = (float *)(uintptr_t)(o + 0x448), *det = (float *)(uintptr_t)(o + 0x460);
        vis[0] = g_tree_base[i][0] * m, vis[1] = g_tree_base[i][1] * m;
        far_[0] = g_tree_base[i][2] * m, far_[1] = g_tree_base[i][3] * m;
        *det = g_tree_base[i][4] / (1.0f + (m - 1.0f) * 0.6f);      /* lower fades details later (mod: 0.75 -> 0.12) */
    }
}
static void tree_wrap(SwCpu *c)
{
    u64 o = c->x[0];
    g_tree_orig(c);
    int slot = -1;
    for (int i = 0; i < 8; i++) if (g_tree_obj[i] == o) { slot = i; break; }
    for (int i = 0; slot < 0 && i < 8; i++) if (!g_tree_obj[i]) slot = i;
    if (slot < 0) slot = 7;
    const float *vis = (const float *)(uintptr_t)(o + 0x430), *far_ = (const float *)(uintptr_t)(o + 0x448);
    g_tree_base[slot][0] = vis[0], g_tree_base[slot][1] = vis[1];
    g_tree_base[slot][2] = far_[0], g_tree_base[slot][3] = far_[1];
    g_tree_base[slot][4] = *(const float *)(uintptr_t)(o + 0x460);
    g_tree_obj[slot] = o;
    sw_drawdist_apply_trees();
    static volatile long logged;
    if (InterlockedIncrement(&logged) <= 4)
        fprintf(stderr, "[fps] tree system 0x%llx: visible %g-%g, fade far %g-%g, detail fade %g (render distance x%g)\n",
                (unsigned long long)o, (double)vis[0], (double)vis[1], (double)far_[0], (double)far_[1],
                (double)*(const float *)(uintptr_t)(o + 0x460), (double)dd_factor());
}

/* Effects: nn::vfx::System::Calculate(system, group, frameRate, swap) (main+1837D0; NintendoWare Vfx 11.4, found from the
 * RTTI of N2nn3vfx6SystemE: it walks the group's emitter sets at +0xB0 + group*8 and updates each with frameRate).
 * SW_VFX_PROBE=1 logs the distinct frame rates the game passes and their callers.
 * Measured 2026-11-18: the game passes whole 60 Hz effect frames per rendered frame - 2 at 30 fps, 1 at 60 fps, and at
 * 120 fps 0 (1/120 s rounded down): effects stood still, so moves such as Flame Wheel lost their fire and other effects
 * never ended.  The fix (SW_FIX_VFX=0 turns it off) passes the effect frames that are due by the frame step instead,
 * per group, kept as a remainder: 1,0,1,0 at 120 fps (60 effect frames a second, as at 60 fps). */
/* SW_OODLE_DUMP=<folder> (research switch): every buffer the game decompresses with OodleLZ_Decompress (main+0x4FC0,
 * found 2026-11-24 from its error strings; x0 comp, x1 comp size, x2 raw, x3 raw size; returns the raw size) is written
 * to <folder>/<FNV-1a 64 of the compressed bytes>_<raw size>.bin, once per name.  The gfpak tool computes the same hash
 * over an archive entry's compressed bytes, so a dump maps back to its file in the archive. */
#define OODLE_DECOMP 0x104fc0ull
static void (*g_oodle_orig)(SwCpu *c);
static void oodle_wrap(SwCpu *c)
{
    u64 src = c->x[0], n = c->x[1], dst = c->x[2];
    g_oodle_orig(c);
    s64 got = (s64)c->x[0];
    const char *dir = getenv("SW_OODLE_DUMP");
    if (got <= 0 || !src || !dst) return;
    u64 h = 0xCBF29CE484222645ull;
    for (u64 i = 0; i < n; i++) h = (h ^ ((const u8 *)(uintptr_t)src)[i]) * 0x100000001B3ull;
    {   /* SW_OODLE_F32=<from>=<to> with SW_OODLE_ONLY=<hash>;...: research switch - in those decompressed files every
         * 4-byte aligned float equal to <from> becomes <to> (testing which data value is a distance) */
        static int init;
        static float from, to;
        static char only[2048];
        if (!init) {
            init = 1;
            const char *e = getenv("SW_OODLE_F32"), *o = getenv("SW_OODLE_ONLY");
            if (!e || sscanf(e, "%f=%f", &from, &to) != 2) from = to = 0;
            snprintf(only, sizeof only, "%s", o ? o : "");
        }
        char hx[17];
        snprintf(hx, sizeof hx, "%016llx", (unsigned long long)h);
        if (from != to && only[0] && strstr(only, hx)) {
            /* SW_OODLE_FBF=<fields>:<index>: only floats that are field <index> of a FlatBuffer table whose vtable
             * lists <fields> fields */
            static int fb_n = -2, fb_i;
            if (fb_n == -2) { const char *e2 = getenv("SW_OODLE_FBF"); if (!e2 || sscanf(e2, "%d:%d", &fb_n, &fb_i) != 2) fb_n = -1; }
            const u8 *b = (const u8 *)(uintptr_t)dst;
            float *v = (float *)(uintptr_t)dst;
            int k = 0;
            for (s64 i = 0; i + 4 <= got; i += 4, v++) {
                if (*v != from) continue;
                int ok = fb_n < 0;
                for (s64 t = i - 4; !ok && t >= 0 && t > i - 200; t -= 4) {
                    s64 vt = t - *(const int32_t *)(b + t);
                    if (vt < 0 || vt + 4 > got) continue;
                    u16 vsz = *(const u16 *)(b + vt), osz = *(const u16 *)(b + vt + 2);
                    if (vsz < 4 || (vsz & 1) || vsz > 200 || vt + vsz > got || osz < 4 || i - t >= osz) continue;
                    int nf = (vsz - 4) / 2;
                    const u16 *offs = (const u16 *)(b + vt + 4);
                    int hit = -1;
                    for (int q = 0; q < nf; q++) if (offs[q] == (u16)(i - t)) hit = q;
                    if (hit < 0) continue;
                    ok = nf == fb_n && hit == fb_i;
                    break;   /* the nearest table that holds it decides */
                }
                if (ok) *v = to, k++;
            }
            fprintf(stderr, "[oodle] %s: %d floats %g -> %g\n", hx, k, (double)from, (double)to);
        }
    }
    if (!dir) return;
    char p[512];
    snprintf(p, sizeof p, "%s/%016llx_%lld.bin", dir, (unsigned long long)h, (long long)got);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) return;
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fwrite((const void *)(uintptr_t)dst, 1, (size_t)got, f);
    fclose(f);
}

#define VFX_CALC 0x2837d0ull
static void (*g_vfx_orig)(SwCpu *c);
static void vfx_wrap(SwCpu *c)
{
    static int probe = -1;
    if (probe < 0) { const char *e = getenv("SW_VFX_PROBE"); probe = e && *e == '1'; }
    if (probe) {
        static u64 seen[32][2]; static volatile long n, calls;
        float fr = c->v[0].f[0];
        u32 fb; memcpy(&fb, &fr, 4);
        long k = InterlockedIncrement(&calls);
        int found = 0;
        for (long i = 0; i < n && i < 32 && !found; i++) found = seen[i][0] == fb && seen[i][1] == c->x[30];
        if (!found && n < 32) {
            long i = InterlockedIncrement(&n) - 1;
            if (i < 32) {
                seen[i][0] = fb, seen[i][1] = c->x[30];
                fprintf(stderr, "[vfx] frame rate %.6g group %d from lr 0x%llx (call %ld, k %.3f)\n", (double)fr,
                        (int)c->x[1], (unsigned long long)c->x[30], k, fix_k());
            }
        }
    }
    static int on = -1;
    double k = fix_on("SW_FIX_VFX", &on) ? fix_k() : 0;
    if (k) {
        static double acc[64];
        int g = (int)(c->x[1] & 63);
        acc[g] += 2.0 * k;                       /* 60 Hz effect frames of this rendered frame */
        double n = floor(acc[g] + 1e-4);
        acc[g] -= n;
        if (acc[g] < 0) acc[g] = 0;
        if (n > 4) n = 4;
        c->v[0].f[0] = (float)n;
    }
    g_vfx_orig(c);
}

/* SW_UI_PROBE=1: ui2d AnimTransformBasic animate entries (vtable 0x40e4d28 slots 5..7: main+128560, +128620,
 * +1286F0, x0 = the anim transform, frame at +20; class found via the RTTI of N2nn4ui2d18AnimTransformBasicE): how
 * far each object's frame moved since its last call, as a histogram every 5 s, and the exact steps ([ui-probe]).
 * Result 2026-11-18: the UI follows the frame step (0.25 per call at 120 fps); off steps put animations off the grid
 * of whole frames for good - hence the whole-fraction step in sw_frame_step_present.  (ui2d Animator::UpdateFrame,
 * main+1395A4, is never called by the game.) */
static void (*g_uiap_orig[3])(SwCpu *c);
static void uiap_common(SwCpu *c, int which)
{
    enum { N = 256, B = 12 };
    static u64 objs[N]; static float last[N]; static volatile long hist[B], lastprint;
    u64 o = c->x[0];
    float f = *(float *)(uintptr_t)(o + 0x20);
    unsigned h = (unsigned)((o >> 4) * 2654435761u) % N;
    if (objs[h] == o) {
        float d = f - last[h];
        int b = d == 0 ? 0 : d < 0 ? 1 : d < 0.2f ? 2 : d < 0.3f ? 3 : d < 0.45f ? 4 : d < 0.55f ? 5 : d < 0.9f ? 6
              : d < 1.1f ? 7 : d < 2.1f ? 8 : 9;
        InterlockedIncrement(&hist[b]);
        static u32 dv[8]; static volatile long dn[8], dk;
        u32 bits; memcpy(&bits, &d, 4);
        if (d > 0) {
            int i = 0;
            for (; i < 8 && i < dk; i++) if (dv[i] == bits) { InterlockedIncrement(&dn[i]); break; }
            if (i == dk && dk < 8) { long j = InterlockedIncrement(&dk) - 1; if (j < 8) dv[j] = bits, dn[j] = 1; }
        }
        static volatile long pr;
        if (InterlockedIncrement(&pr) % 1500 == 0)
            for (int i = 0; i < 8 && i < dk; i++) { float x; memcpy(&x, &dv[i], 4);
                fprintf(stderr, "[ui-probe] step %.9g (0x%08x): %ld calls\n", (double)x, dv[i], dn[i]); }
    }
    {   /* SW_UI_PROBE=2: the first animation whose frame restarts (back to < 0.5) 6 times within 3 s gets its frame
         * field watched (sw_watch_writes): the writers show who restarts it (the NPC talk prompt fade-in) */
        static int mode = -1;
        if (mode < 0) { const char *e = getenv("SW_UI_PROBE"); mode = e ? atoi(e) : 0; }
        static u64 robj[N]; static int rcnt[N]; static ULONGLONG rt0[N]; static int watching;
        if (mode == 2 && !watching && objs[h] == o && f < 0.5f && last[h] > 1.0f) {
            ULONGLONG t = GetTickCount64();
            if (robj[h] != o || t - rt0[h] > 3000) robj[h] = o, rcnt[h] = 0, rt0[h] = t;
            if (++rcnt[h] >= 6) {
                watching = 1;
                fprintf(stderr, "[ui-probe] animation 0x%llx restarted %d times in %llu ms (lr 0x%llx): watching its frame\n",
                        (unsigned long long)o, rcnt[h], (unsigned long long)(t - rt0[h]), (unsigned long long)c->x[30]);
                extern void sw_watch_writes(u64);
                sw_watch_writes(o + 0x20);
            }
        }
    }
    objs[h] = o, last[h] = f;
    long now = (long)(GetTickCount64() / 5000);
    if (now != lastprint && InterlockedExchange(&lastprint, now) != now)
        fprintf(stderr, "[ui-probe] frame moved per call: same %ld, back %ld, <0.2 %ld, ~0.25 %ld, ~0.33 %ld, ~0.5 %ld, "
                "0.5-0.9 %ld, ~1 %ld, 1-2 %ld, >2 %ld (k %.3f)\n", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5],
                hist[6], hist[7], hist[8], hist[9], fix_k());
    g_uiap_orig[which](c);
}
static void uiap0(SwCpu *c) { uiap_common(c, 0); }
static void uiap1(SwCpu *c) { uiap_common(c, 1); }
static void uiap2(SwCpu *c) { uiap_common(c, 2); }

/* SW_CHANGED_BY=<fn>,<addr>: wrap guest function <fn>; when a call changes the 32-bit value at <addr>, log the
 * caller (lr) and the value (first 16 distinct callers).  Finds who feeds a value through a generic setter. */
static u64 g_cb_fn, g_cb_addr;
static void (*g_cb_orig)(SwCpu *c);
static void cb_wrap(SwCpu *c)
{
    u64 lr = c->x[30];
    u32 before = 0, after = 0;
    int ok = 1;
    __try { before = *(volatile u32 *)(uintptr_t)g_cb_addr; } __except (1) { ok = 0; }
    g_cb_orig(c);
    if (!ok) return;
    __try { after = *(volatile u32 *)(uintptr_t)g_cb_addr; } __except (1) { return; }
    if (after == before) return;
    static u64 seen[16]; static volatile long n;
    for (long i = 0; i < n && i < 16; i++) if (seen[i] == lr) return;
    long k = InterlockedIncrement(&n) - 1;
    if (k >= 16) return;
    seen[k] = lr;
    float fb, fa; memcpy(&fb, &before, 4); memcpy(&fa, &after, 4);
    fprintf(stderr, "[changed-by] 0x%llx changed by 0x%llx from lr 0x%llx: %g -> %g\n", (unsigned long long)g_cb_addr,
            (unsigned long long)g_cb_fn, (unsigned long long)lr, (double)fb, (double)fa);
}

/* SW_CFG_PROBE=1: the app_config getters (main+0xC746A4.. one per value type; x0 = the config object, x1 -> the
 * FNV-1a hash of the setting name): logs each distinct (setting, caller) with the value it returned - who reads
 * which setting (found 2026-11-19 for the render distance settings). */
static const char *const kCfgNames[] = {
    "kArea00NpcPokeLoadRadius", "kArea00NpcLoadRadius", "kDetailFadeDistanceNear", "kDetailFadeCoefficient",
    "kTreeFadeDistanceFar", "kTreeFadeDistanceNear", "kTreeVisibleDistance", "kDistanceOfCameraToTerrainLodPoint",
    "kDistanceOfCameraToStreamLoadPoint", "terrain_radius", "bg_distance", "npc_distance", "default_far",
    "player_dither_distance_max", "player_dither_distance_min", "distanceBaseValue", "poke_spawn_distance",
    "kWhSpawnDistance", "kWhSpawnDistanceRide", "kSubSpawnerPokeSpawnDistance", "Outdoor_ResetRange_Distance",
    "Indoor_ResetRange_Distance", "range_draw_seconds", "unique_oybn_leave_count", "find_notify_distance",
};
static u64 cfg_fnv(const char *n)
{
    u64 h = 0xCBF29CE484222645ull;
    for (; *n; n++) h = (h ^ (u8)*n) * 0x100000001B3ull;
    return h;
}
/* ---- Detail distance (built in; level from sw_romfs.c: Original, 2x, 3x, 5x, 10x) ------------------------------
 * The model LOD picker (main+0x1B99550; found 2026-11-19 from the script function SetLodLevelZero, which sets the
 * mode at +0x1F0 to 4): x0 -> {the LOD object, ...}, x1 = the instance (+0x10 position).  The LOD object: +0x1F0 mode
 * (0 top, 1 one step per 100 m from the camera, 2 screen size against the 3 thresholds of preset +0x234 in the table
 * at main+0x3A3B5C0, 3 the last, 4 forced top), +0x204 LOD count, +0x228 an optional remap (-> int[]), +0x238 the
 * chosen LOD, which it also returns.  In the field nearly everything (rocks, props, far trees and bushes) is mode 1:
 * past 200 m all of it is at its lowest LOD, and thin things vanish.  The level scales the 100 m step (the picker's
 * choice is redone after it) and divides the screen-size thresholds.  The camera is the one the picker looks up
 * (main+0x2AD5170 from main+0x1B997F8; its position at +0x40).
 * SW_LOD_PROBE=1 logs per 5 s the calls by mode / preset, LOD counts and choices; SW_LOD_FORCE=1 (test) puts
 * everything at the top LOD. */
static const float kDetailFactor[5] = {1.0f, 2.0f, 3.0f, 5.0f, 10.0f};
static float detail_factor(void) { extern int sw_detail_get(void); return kDetailFactor[sw_detail_get()]; }
#define LOD_PICK 0x1b99550ull
#define LOD_CAMERA 0x2ad5170ull
#define LOD_CAMERA_LR 0x1b997fcull
#define LOD_TABLE 0x3a3b5c0ull
static void (*g_lodp_orig)(SwCpu *c), (*g_lodcam_orig)(SwCpu *c);
static __declspec(thread) u64 t_lod_cam;   /* the camera of this thread's current pick */
static volatile u64 g_last_cam;            /* the last camera any pick used (Debug tab: camera position) */
static volatile float g_fov_scale = 1.0f, g_fov_mine = -1.0f;   /* field of view factor; the angle last written */
void sw_fov_scale_set(float s) { g_fov_scale = s < 0.6f ? 0.6f : s > 1.6f ? 1.6f : s; g_fov_mine = -1.0f; }
float sw_fov_scale_get(void) { return g_fov_scale; }
int sw_camera_pos(float out[3])
{
    u64 cam = g_last_cam;
    if (!cam) return 0;
    const float *q = (const float *)(uintptr_t)(cam + 0x40);
    out[0] = q[0], out[1] = q[1], out[2] = q[2];
    return 1;
}
static float g_lod_table_base[9];
static int g_lod_table_ok;
static void lodcam_wrap(SwCpu *c)
{
    u64 lr = c->x[30];
    g_lodcam_orig(c);
    if (lr == LOD_CAMERA_LR || lr == 0x1b995f0ull) t_lod_cam = c->x[0], g_last_cam = c->x[0];   /* mode 1 / mode 2 call sites */
    if (g_fov_scale != 1.0f && (lr == LOD_CAMERA_LR || lr == 0x1b995f0ull) && c->x[0]) {
        /* Field of view: the field camera (main+0x2AD5170's result) keeps its vertical angle in radians at +0x1C (near
         * +0x14, far +0x18, aspect +0x20, position +0x40, projection +0x9C; dump 2026-11-25).  The game sets it as the
         * camera moves (zoom, aim); a value other than the one written here is the game's, and is scaled again. */
        float *fov = (float *)(uintptr_t)(c->x[0] + 0x1c);
        float v = *fov;
        if (v != g_fov_mine && v > 0.05f && v < 2.5f) {
            float w = v * g_fov_scale;
            w = w > 2.6f ? 2.6f : w;
            g_fov_mine = w;
            *fov = w;
        }
    }
    {   /* SW_FOV_WATCH=<n>: research - at the n-th pick, a read watch on the camera's angle (+0x1C): who reads it */
        static long at = -2; static volatile long k;
        if (at == -2) { const char *e = getenv("SW_FOV_WATCH"); at = e ? atol(e) : -1; }
        if (at > 0 && c->x[0] && InterlockedIncrement(&k) == at) {
            extern void sw_watch_access(u64); sw_watch_access(c->x[0] + 0x1c);
            fprintf(stderr, "[fov-watch] camera 0x%llx\n", (unsigned long long)c->x[0]);
        }
    }
    {   /* SW_CAM_DUMP=<n>: research - at the n-th pick, the camera object's first 1 KiB as floats */
        static long at = -2; static volatile long k;
        if (at == -2) { const char *e = getenv("SW_CAM_DUMP"); at = e ? atol(e) : -1; }
        if (at > 0 && g_last_cam && InterlockedIncrement(&k) == at) {
            const float *f = (const float *)(uintptr_t)g_last_cam;
            for (int i = 0; i < 256; i += 8)
                fprintf(stderr, "[cam-dump] +%03x %g %g %g %g %g %g %g %g\n", i * 4, (double)f[i], (double)f[i + 1],
                        (double)f[i + 2], (double)f[i + 3], (double)f[i + 4], (double)f[i + 5], (double)f[i + 6], (double)f[i + 7]);
        }
    }
}
void sw_detail_apply(void)
{
    if (!g_lod_table_ok) return;
    float f = detail_factor(), *t = (float *)(uintptr_t)LOD_TABLE;
    DWORD old;
    if (!VirtualProtect(t, sizeof g_lod_table_base, PAGE_READWRITE, &old)) return;
    for (int i = 0; i < 9; i++) t[i] = g_lod_table_base[i] / f;
    VirtualProtect(t, sizeof g_lod_table_base, old, &old);
}
static void lodp_probe(u64 obj);
extern volatile long long sw_present_count;
static void lod_table_init(void);
static void lodp_wrap(SwCpu *c)
{
    static int probe = -1, force = -1;
    if (probe < 0) { const char *e = getenv("SW_LOD_PROBE"); probe = e && *e == '1'; }
    if (force < 0) { const char *e = getenv("SW_LOD_FORCE"); force = e && *e == '1'; }
    u64 obj = *(u64 *)(uintptr_t)c->x[0], inst = c->x[1];
    if (!g_lod_table_ok) lod_table_init();
    t_lod_cam = 0;
    g_lodp_orig(c);
    int mode = *(int *)(uintptr_t)(obj + 0x1f0);
    float f = detail_factor();
    if (force) { *(int *)(uintptr_t)(obj + 0x238) = 0; c->x[0] = 0; }
    else if (mode == 1 && f != 1.0f && t_lod_cam) {
        const float *p = (const float *)(uintptr_t)(inst + 0x10), *q = (const float *)(uintptr_t)(t_lod_cam + 0x40);
        float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        int n = *(int *)(uintptr_t)(obj + 0x204), lod = 0;
        while (lod < n - 1 && d >= (float)(lod + 1) * 100.0f * f) lod++;   /* the game's loop, with the step scaled */
        u64 remap = *(u64 *)(uintptr_t)(obj + 0x228);
        if (remap) lod = ((const int *)(uintptr_t)*(u64 *)(uintptr_t)remap)[lod];
        *(int *)(uintptr_t)(obj + 0x238) = lod;
        c->x[0] = (u64)(u32)lod;
    }
    if (probe) lodp_probe(obj);
    if (probe && t_lod_cam) {   /* SW_LOD_PROBE: the camera distance at which objects stop being picked (despawn) */
        static struct { u64 obj; ULONGLONG t; float d; } seen[8192];
        static volatile long hist[64], lastp, lock;
        const float *p = (const float *)(uintptr_t)(inst + 0x10), *q = (const float *)(uintptr_t)(t_lod_cam + 0x40);
        float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        ULONGLONG now = GetTickCount64();
        while (InterlockedExchange(&lock, 1)) YieldProcessor();
        unsigned h = (unsigned)((obj >> 4) * 2654435761u) % 8192;
        if (seen[h].obj && seen[h].obj != obj && now - seen[h].t > 2000) {   /* evicted after vanishing */
            int b = (int)(seen[h].d / 10.0f); InterlockedIncrement(&hist[b < 63 ? b : 63]);
        }
        static u64 chars[4096];   /* characters (screen-size mode): logged once each, open addressing */
        int newchar = 0;
        if (*(int *)(uintptr_t)(obj + 0x1f0) != 1) {
            unsigned k = (unsigned)((obj >> 4) * 2654435761u) % 4096;
            for (int tries = 0; tries < 4096; tries++, k = (k + 1) % 4096) {
                if (chars[k] == obj) break;
                if (!chars[k]) { chars[k] = obj; newchar = 1; break; }
            }
        }
        if (newchar) {   /* characters: each first pick */
            static volatile long nlog;
            if (InterlockedIncrement(&nlog) <= 600)
                fprintf(stderr, "[lod-first] frame %lld t %llu mode %d preset %d d %.1f pos %.1f %.1f %.1f obj 0x%llx inst 0x%llx\n",
                        sw_present_count, (unsigned long long)now, *(int *)(uintptr_t)(obj + 0x1f0), *(int *)(uintptr_t)(obj + 0x234),
                        d, p[0], p[1], p[2], (unsigned long long)obj, (unsigned long long)inst);
            (void)0;
        }
        if (seen[h].obj != obj) {   /* first pick: the distance at which it appeared (spawn / stream-in) */
            static volatile long app[64];
            int b = (int)(d / 5.0f); InterlockedIncrement(&app[b < 63 ? b : 63]);
            static ULONGLONG lastapp;
            if (now - lastapp > 10000) {
                lastapp = now;
                fprintf(stderr, "[lod-appear] first pick distance (5 m bins):");
                for (int i = 0; i < 64; i++) if (app[i]) fprintf(stderr, " %d:%ld", i * 5, app[i]);
                fprintf(stderr, "\n");
            }
        }
        seen[h].obj = obj, seen[h].t = now, seen[h].d = d;
        if (now / 10000 != (ULONGLONG)lastp) {
            lastp = (long)(now / 10000);
            for (int i = 0; i < 8192; i++)
                if (seen[i].obj && now - seen[i].t > 2000) {
                    int b = (int)(seen[i].d / 10.0f); InterlockedIncrement(&hist[b < 63 ? b : 63]); seen[i].obj = 0;
                }
            fprintf(stderr, "[lod-vanish] last pick distance of vanished objects (10 m bins):");
            for (int i = 0; i < 64; i++) if (hist[i]) fprintf(stderr, " %d:%ld", i * 10, hist[i]);
            fprintf(stderr, "\n");
        }
        InterlockedExchange(&lock, 0);
    }
}
static void lodp_probe(u64 obj)
{
    static volatile long calls, hist[5][4], nlod[8], pick[8];
    static volatile long lastp;
    InterlockedIncrement(&calls);
    int mode = *(int *)(uintptr_t)(obj + 0x1f0), pre = *(int *)(uintptr_t)(obj + 0x234), n = *(int *)(uintptr_t)(obj + 0x204);
    int got = *(int *)(uintptr_t)(obj + 0x238);
    if (mode >= 0 && mode < 5 && pre >= 0 && pre < 4) InterlockedIncrement(&hist[mode][pre]);
    InterlockedIncrement(&nlod[n & 7]);
    InterlockedIncrement(&pick[got & 7]);
    long now = (long)(GetTickCount64() / 5000);
    if (now != lastp && InterlockedExchange(&lastp, now) != now) {
        fprintf(stderr, "[lod-probe] %ld calls (x%g); mode/preset:", calls, (double)detail_factor());
        for (int m = 0; m < 5; m++) for (int q = 0; q < 4; q++) if (hist[m][q]) fprintf(stderr, " m%d/p%d %ld", m, q, hist[m][q]);
        fprintf(stderr, "; lods:"); for (int i = 0; i < 8; i++) if (nlod[i]) fprintf(stderr, " %d:%ld", i, nlod[i]);
        fprintf(stderr, "; chosen:"); for (int i = 0; i < 8; i++) if (pick[i]) fprintf(stderr, " %d:%ld", i, pick[i]);
        fprintf(stderr, "\n");
    }
}
/* The table, at the first pick (the game's image is not mapped yet when the hooks go in) */
static void lod_table_init(void)
{
    static volatile long once;
    if (InterlockedCompareExchange(&once, 1, 0)) return;
    const float *t = (const float *)(uintptr_t)LOD_TABLE;
    static const float expect[9] = {0.01f, 0.003f, 0.001f, 0.01f, 0.005f, 0.002f, 1.8f, 0.5f, 0.2f};
    for (int i = 0; i < 9; i++) if (t[i] != expect[i]) { fprintf(stderr, "[fps] detail distance: LOD table not found\n"); return; }
    memcpy(g_lod_table_base, t, sizeof g_lod_table_base);
    g_lod_table_ok = 1;
    sw_detail_apply();
}
static int detail_hooks_init(void)
{
    if (!install(LOD_CAMERA, lodcam_wrap, &g_lodcam_orig) || !install(LOD_PICK, lodp_wrap, &g_lodp_orig)) return 0;
    extern void (*sw_detail_live_hook)(void);
    sw_detail_live_hook = sw_detail_apply;
    return 1;
}

#define N_CFG_GET 9
static const u64 kCfgGet[N_CFG_GET] = {0xd746a4ull, 0xd747c0ull, 0xd748ccull, 0xd749d4ull, 0xd74ae8ull,
                                        0xd74bfcull, 0xd74d14ull, 0xd74e28ull, 0xd74f38ull};
static void (*g_cfgp_orig[N_CFG_GET])(SwCpu *c);
static void cfgp_common(SwCpu *c, int which)
{
    u64 key = c->x[1] ? *(u64 *)(uintptr_t)c->x[1] : 0, lr = c->x[30], obj = c->x[0];
    g_cfgp_orig[which](c);
    {   /* SW_CFG_SET=<name>=<value>[;...] (test switch): that setting reads as the value (w0 and s0 both set) */
        static int n_set = -1;
        static u64 set_key[16];
        static float set_val[16];
        if (n_set < 0) {
            int k = 0;
            const char *e = getenv("SW_CFG_SET");
            while (e && *e && k < 16) {
                char name[96];
                float v;
                int used = 0;
                if (sscanf(e, "%95[^=]=%f%n", name, &v, &used) != 2) break;
                set_key[k] = cfg_fnv(name), set_val[k] = v, k++;
                fprintf(stderr, "[cfg-set] %s = %g\n", name, (double)v);
                e += used;
                if (*e == ';') e++;
            }
            n_set = k;
        }
        for (int i = 0; i < n_set; i++)
            if (set_key[i] == key) { c->x[0] = (u64)(u32)(int32_t)set_val[i]; c->v[0].f[0] = set_val[i]; }
    }
    static int probe = -1;
    if (probe < 0) probe = getenv("SW_CFG_PROBE") != NULL;
    if (!probe) return;
    static u64 seen[512][2]; static volatile long n;
    for (long i = 0; i < n && i < 512; i++) if (seen[i][0] == key && seen[i][1] == lr) return;
    long k = InterlockedIncrement(&n) - 1;
    if (k >= 512) return;
    seen[k][0] = key, seen[k][1] = lr;
    const char *name = "?";
    for (size_t i = 0; i < sizeof kCfgNames / sizeof *kCfgNames; i++) if (cfg_fnv(kCfgNames[i]) == key) name = kCfgNames[i];
    fprintf(stderr, "[cfg-probe] getter 0x%llx key 0x%016llx (%s) obj 0x%llx lr 0x%llx -> w0 %d s0 %g\n",
            (unsigned long long)kCfgGet[which], (unsigned long long)key, name, (unsigned long long)obj,
            (unsigned long long)lr, (int)(int32_t)c->x[0], (double)c->v[0].f[0]);
}
#define CFGP(i) static void cfgp##i(SwCpu *c) { cfgp_common(c, i); }
CFGP(0) CFGP(1) CFGP(2) CFGP(3) CFGP(4) CFGP(5) CFGP(6) CFGP(7) CFGP(8)
static void (*const g_cfgp_w[N_CFG_GET])(SwCpu *) = {cfgp0, cfgp1, cfgp2, cfgp3, cfgp4, cfgp5, cfgp6, cfgp7, cfgp8};

/* ---- People & Pokemon distance (built in; level from sw_romfs.c) ------------------------------------------------
 * Found 2026-11-19 from the Nexus mod "Improved Pokemon De-Spawn Radius" (its only change: event_culling_config.bin
 * npc_distance 20 -> 999).  The field object culling manager (the tree system object, setup main+0x252B41C) runs
 * main+0x2531BB4 every frame: in its normal mode (+0x234 == 1) every object of its lists (+0x2D0: nodes, node[6] ->
 * group, group+0x278..+0x280 -> objects) whose bounding sphere (+0x290: centre xyz, radius w) is outside one of the
 * six planes of the camera's view is culled (hidden; the characters among them leave the scene), the others shown.
 * Event scripts (AppCommand.CullingDisable(x, z, npc_distance)) switch it to mode 2: shown within the radius of a
 * point, the rest culled.  The level keeps everything within N metres of the view shown as well, whatever direction
 * the camera faces: the radius the plane test uses is N larger during the game's pass (restored after), so nothing
 * the game shows is lost.  SW_CULL_PROBE=1 logs per 5 s the objects tested and culled. */
static const float kNpcKeep[5] = {0.0f, 150.0f, 200.0f, 300.0f, 999.0f};   /* event radius per level (game: 20 m) */
static float npc_keep(void) { extern int sw_npcdist_get(void); return kNpcKeep[sw_npcdist_get()]; }
/* 2026-11-19 retest (camera turns, screenshots): in normal play nothing pops in at Original, so keeping the out-of-view
 * cells active (SW_CULL_KEEP=1, the old behaviour) has no visible effect and only costs work - off by default.  What
 * the mod changes is the event radius: AppCommand.CullingDisable (main+0xE82C04: s0 x, s1 z, s2 radius) is called by
 * every event script with event_cfg_culling npc_distance (20 m): outside it people and Pokemon are culled during the
 * event.  The level raises that radius to its distance (Max 999 m, the mod's value). */
#define CULL_DISABLE 0xe82c04ull
static void (*g_culldis_orig)(SwCpu *c);
static void culldis_wrap(SwCpu *c)
{
    float keep = npc_keep(), r = c->v[2].f[0];
    if (keep > 0 && r < keep) c->v[2].f[0] = keep;
    static volatile long logged;
    if (InterlockedIncrement(&logged) <= 4)
        fprintf(stderr, "[fps] event culling radius %g -> %g m\n", (double)r, (double)c->v[2].f[0]);
    g_culldis_orig(c);
}
#define CULL_PASS 0x2531bb4ull
static void (*g_cull_orig)(SwCpu *c);
static void cull_wrap(SwCpu *c)
{
    u64 m = c->x[0];
    static int keep_on = -1;
    if (keep_on < 0) { const char *e = getenv("SW_CULL_KEEP"); keep_on = e && *e == '1'; }
    float keep = keep_on ? npc_keep() : 0.0f;
    static int probe = -1;
    if (probe < 0) { const char *e = getenv("SW_CULL_PROBE"); probe = e && *e == '1'; }
    enum { MAXO = 8192 };
    static float *held[MAXO]; static float was[MAXO];
    int n = 0, tested = 0, live = 0;
    int on = m && *(int *)(uintptr_t)(m + 0x68) == 1 && *(char *)(uintptr_t)(m + 0x230) && *(int *)(uintptr_t)(m + 0x234) == 1;
    if (on && (keep > 0 || probe)) {
        for (u64 node = *(u64 *)(uintptr_t)(m + 0x2d0); node; node = *(u64 *)(uintptr_t)node) {
            u64 grp = ((u64 *)(uintptr_t)node)[6];
            if (!grp) continue;
            u64 *o = *(u64 **)(uintptr_t)(grp + 0x278), *e = *(u64 **)(uintptr_t)(grp + 0x280);
            for (; o && o < e; o++) {
                float *r = (float *)(uintptr_t)(*o + 0x29c);
                tested++;
                live += *r > 0;
                if (probe) {   /* each kind of object once: its vtable, sphere and the name hash at +0x8 (if any) */
                    static u64 vts[32]; static int nv;
                    u64 vt = *(u64 *)(uintptr_t)*o; int k = 0;
                    while (k < nv && vts[k] != vt) k++;
                    static int nsp;
                    if (*r > 0 && nsp < 16) {
                        const float *sp = (const float *)(uintptr_t)(*o + 0x290); nsp++;
                        fprintf(stderr, "[cull-probe] sphere %.1f %.1f %.1f r %.1f\n", sp[0], sp[1], sp[2], sp[3]);
                    }
                    if (k == nv && nv < 32) {
                        vts[nv++] = vt;
                        const float *sp = (const float *)(uintptr_t)(*o + 0x290);
                        fprintf(stderr, "[cull-probe] object 0x%llx vtable 0x%llx sphere %.1f %.1f %.1f r %.1f\n",
                                (unsigned long long)*o, (unsigned long long)vt, sp[0], sp[1], sp[2], sp[3]);
                    }
                }
                if (keep > 0 && *r > 0 && n < MAXO) { held[n] = r, was[n] = *r, *r += keep, n++; }
            }
        }
    }
    g_cull_orig(c);
    for (int i = 0; i < n; i++) if (*held[i] == was[i] + keep) *held[i] = was[i];
    if (probe && on) {
        static volatile long calls, sum_tested, sum_live, lastp;
        InterlockedIncrement(&calls); InterlockedAdd(&sum_tested, tested); InterlockedAdd(&sum_live, live);
        long now = (long)(GetTickCount64() / 5000);
        if (now != lastp && InterlockedExchange(&lastp, now) != now)
            fprintf(stderr, "[cull-probe] %ld passes, %.0f objects per pass (%.1f with a sphere), keep %g m\n", calls,
                    calls ? (double)sum_tested / calls : 0.0, calls ? (double)sum_live / calls : 0.0, (double)keep);
    }
}

/* SW_NPC_PROBE=1: the area set-up (main+0x1D05D48, x1 -> the area object; Jubilife's sets the people / Pokemon load
 * radius there with main+0x1813AF4: +0x450 r, +0x454 r-10, +0x458 r+20): logs each area object and its three values
 * after the set-up; SW_NPC_PROBE=2 also watches the reads of +0x450 of the first one (who uses the radius). */
#define AREA_SETUP 0x1d05d48ull
static void (*g_area_orig)(SwCpu *c);
static void area_wrap(SwCpu *c)
{
    u64 objp = c->x[1];
    u64 obj = objp ? *(u64 *)(uintptr_t)objp : 0;
    g_area_orig(c);
    if (!obj) return;
    const int *r = (const int *)(uintptr_t)(obj + 0x450);
    fprintf(stderr, "[npc-probe] area object 0x%llx (vtable 0x%llx): +0x450 %d, +0x454 %d, +0x458 %d\n",
            (unsigned long long)obj, (unsigned long long)*(u64 *)(uintptr_t)obj, r[0], r[1], r[2]);
    static int watched;
    const char *e = getenv("SW_NPC_PROBE");
    if (e && *e == '2' && !watched++) { extern void sw_watch_access(u64); sw_watch_access(obj + 0x450); }
}

/* SW_BT_PROBE=<fn>[,<fn>...] (hex guest addresses, up to 4): each distinct call chain into those functions is logged
 * once (up to 64): the lr and 10 return addresses from the guest's frame-pointer chain (x29 -> {fp, lr}). */
static u64 g_btp_fn[4];
static void (*g_btp_orig[4])(SwCpu *c);
static void btp_common(SwCpu *c, int k)
{
    u64 chain[12]; int n = 0;
    chain[n++] = c->x[30];
    u64 fp = c->x[29];
    __try {
        for (; n < 12 && fp > 0x10000 && !(fp & 7); n++) { chain[n] = *(u64 *)(uintptr_t)(fp + 8); fp = *(u64 *)(uintptr_t)fp; }
    } __except (1) {}
    u64 h = 0xcbf29ce484222645ull;
    for (int i = 0; i < n; i++) h = (h ^ chain[i]) * 0x100000001b3ull;
    static u64 seen[64]; static volatile long ns;
    int dup = 0;
    for (long i = 0; i < ns && i < 64; i++) dup |= seen[i] == h;
    if (!dup) {
        long i = InterlockedIncrement(&ns) - 1;
        if (i < 64) {
            seen[i] = h;
            fprintf(stderr, "[bt-probe] 0x%llx x0 0x%llx x1 0x%llx:", (unsigned long long)g_btp_fn[k],
                    (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
            for (int j = 0; j < n; j++) fprintf(stderr, " 0x%llx", (unsigned long long)chain[j]);
            fprintf(stderr, "\n");
        }
    }
    g_btp_orig[k](c);
}
static void btp0(SwCpu *c) { btp_common(c, 0); }
static void btp1(SwCpu *c) { btp_common(c, 1); }
static void btp2(SwCpu *c) { btp_common(c, 2); }
static void btp3(SwCpu *c) { btp_common(c, 3); }
static void btp_init(void)
{
    const char *e = getenv("SW_BT_PROBE");
    static void (*const w[4])(SwCpu *) = {btp0, btp1, btp2, btp3};
    for (int k = 0; e && *e && k < 4; k++) {
        g_btp_fn[k] = strtoull(e, NULL, 16);
        if (!install(g_btp_fn[k], w[k], &g_btp_orig[k])) fprintf(stderr, "[bt-probe] 0x%llx: no such function\n", (unsigned long long)g_btp_fn[k]);
        e = strchr(e, ',');
        if (e) e++;
    }
}

/* The world streaming object's update (main+0x1E4BB94, x0 = the object): it collects the world cells within the
 * radius +0x6C of the stream point +0x110 (quadtree query main+0x1E4D150(x, z, radius)); the constructor
 * (main+0x1E4AB84) sets +0x68, +0x6C from its arguments and +0x70 = 60.  Measured 2026-11-19: +0x68 = 100 m (the
 * load radius: people, Pokemon and objects of a cell appear when it comes within it - characters first seen at
 * 100-110 m), +0x6C = 120 m (kept until beyond it).  The people & pokemon level multiplies both during the update
 * (restored after, so live): 1, 1.5, 2, 3, 4 (100 to 400 m).  Verified on the user's bridge save: at 1x the guard's
 * camp (torches, guard) is not there from the hill, at 2x / 3x it is.  SW_STREAM_SCALE=<f> overrides (tests). */
static const float kStreamScale[5] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
#define STREAM_UPDATE 0x1e4bb94ull
static void (*g_stream_orig)(SwCpu *c);
static void stream_wrap(SwCpu *c)
{
    u64 o = c->x[0];
    static float force = -1;
    if (force < 0) { const char *e = getenv("SW_STREAM_SCALE"); force = e && *e ? (float)atof(e) : 0.0f; }
    extern int sw_npcdist_get(void);
    float scale = force > 0 ? force : kStreamScale[sw_npcdist_get()];
    float *r = o ? (float *)(uintptr_t)(o + 0x6c) : NULL, base = r ? *r : 0, base_in = r ? r[-1] : 0;
    static u64 seen[16]; static int ns;
    int k = 0;
    while (k < ns && seen[k] != o) k++;
    if (k == ns && ns < 16 && o) {
        seen[ns++] = o;
        fprintf(stderr, "[stream] object 0x%llx: +0x68 %g, radius +0x6c %g, +0x70 %g (scale %g)\n", (unsigned long long)o,
                (double)r[-1], (double)base, (double)r[1], (double)scale);
    }
    if (r && scale != 1.0f) *r = base * scale, r[-1] = base_in * scale;   /* load (+0x68) and keep (+0x6c) radii */
    g_stream_orig(c);
    if (r && scale != 1.0f) {
        if (*r == base * scale) *r = base;
        if (r[-1] == base_in * scale) r[-1] = base_in;
    }
}

/* SW_STREAM_AHEAD=<m> (test): field_common_config kDistanceOfCameraToStreamLoadPoint (7 m: how far ahead of the
 * camera the world streaming is centred; read by main+0x198E50C with the float getter main+0xD748CC) set to m. */
#define CFG_GETF 0xd748ccull
#define KEY_STREAM_POINT 0x5d45b190e47082b0ull
static void (*g_cfggetf_orig)(SwCpu *c);
static void cfggetf_wrap(SwCpu *c)
{
    u64 key = c->x[1] ? *(u64 *)(uintptr_t)c->x[1] : 0;
    g_cfggetf_orig(c);
    if (key == KEY_STREAM_POINT) {
        static float ahead = -1;
        if (ahead < 0) { const char *e = getenv("SW_STREAM_AHEAD"); ahead = e && *e ? (float)atof(e) : 0; }
        static volatile long logged;
        if (InterlockedIncrement(&logged) <= 2) fprintf(stderr, "[fps] stream point %g m ahead -> %g\n", (double)c->v[0].f[0], (double)ahead);
        if (ahead > 0) c->v[0].f[0] = ahead;
    }
}

/* SW_CHAR_WATCH=1: the character creation sequence (main+0x1D21C88, x0 = the character) stores 100.0 at +0x5C0
 * (main+0x1D210AC); the first character that has it gets a read watch there (sw_watch_access: who reads it). */
static void (*g_charc_orig)(SwCpu *c);
static void charc_wrap(SwCpu *c)
{
    u64 o = c->x[0];
    g_charc_orig(c);
    static int done;
    if (!done && o && *(float *)(uintptr_t)(o + 0x5c0) == 100.0f) {
        done = 1;
        fprintf(stderr, "[char-watch] character 0x%llx: +0x5c0 = 100\n", (unsigned long long)o);
        extern void sw_watch_access(u64); sw_watch_access(o + 0x5c0);
    }
}

static void fps_hooks_init(void)
{
    { const char *e = getenv("SW_GAME_SPEED"); if (e && *e) sw_game_speed_set((float)atof(e)); }   /* test switch */
    { const char *e = getenv("SW_FOV_SCALE"); if (e && *e) sw_fov_scale_set((float)atof(e)); }
    btp_init();
    if (getenv("SW_CHAR_WATCH")) install(0x1d21c88ull, charc_wrap, &g_charc_orig);
    if (getenv("SW_STREAM_AHEAD")) install(CFG_GETF, cfggetf_wrap, &g_cfggetf_orig);
    if (install(STREAM_UPDATE, stream_wrap, &g_stream_orig))
        fprintf(stderr, "[fps] world streaming radius hook on 0x%llx\n", (unsigned long long)STREAM_UPDATE);
    if (getenv("SW_NPC_PROBE")) install(AREA_SETUP, area_wrap, &g_area_orig);
    { extern void sw_shadow_init(void); sw_shadow_init(); }   /* sw_romfs.c: SW_SHADOW_SCALE before the renderer starts */
    { extern void sw_gfx_settings_init(void); sw_gfx_settings_init(); }   /* sw_menu.c: anisotropy, present mode */
    {   const char *e = getenv("SW_CHANGED_BY");
        unsigned long long f, a;
        if (e && sscanf(e, "%llx,%llx", &f, &a) == 2 && install(f, cb_wrap, &g_cb_orig)) { g_cb_fn = f; g_cb_addr = a; }
    }
    arglog_init();
    fps_init();
    if (!build_ok()) return;
    {   const char *e = getenv("SW_WATER_STEP");
        if (!(e && *e == '0') && !(getenv("SW_CHANGED_BY") && strtoull(getenv("SW_CHANGED_BY"), NULL, 16) == WATER_SETTER)) {
            if (install(WATER_SETTER, water_wrap, &g_water_orig))
                fprintf(stderr, "[fps] water time step hook on 0x%llx\n", (unsigned long long)WATER_SETTER);
        }
    }
    {   const char *e = getenv("SW_CAM_STEP");
        if (!(e && *e == '0')) {
            if (install(CAM_SPEED_FN, camspd_wrap, &g_camspd_orig))
                fprintf(stderr, "[fps] camera turn speed hook on 0x%llx\n", (unsigned long long)CAM_SPEED_FN);
            if (install(CAM_AUTO_FN, camauto_wrap, &g_camauto_orig))
                fprintf(stderr, "[fps] camera auto-turn hook on 0x%llx\n", (unsigned long long)CAM_AUTO_FN);
        }
    }
    if (install(WBIN_UPDATE, wbin_wrap, &g_wbin_orig))
        fprintf(stderr, "[fps] battle sequence fix on 0x%llx\n", (unsigned long long)WBIN_UPDATE);
    if (install(FADE_UPDATE, fade_wrap, &g_fade_orig))
        fprintf(stderr, "[fps] fade fix on 0x%llx\n", (unsigned long long)FADE_UPDATE);
    if (install(LIST_SCROLL, scroll_wrap, &g_scroll_orig))
        fprintf(stderr, "[fps] list scroll fix on 0x%llx\n", (unsigned long long)LIST_SCROLL);
    if (install(PHYS_STEP, phys_wrap, &g_phys_orig))
        fprintf(stderr, "[fps] physics step fix on 0x%llx\n", (unsigned long long)PHYS_STEP);
    if (install(BALL_FLIGHT, ball_wrap, &g_ball_orig))
        fprintf(stderr, "[fps] thrown-object flight fix on 0x%llx\n", (unsigned long long)BALL_FLIGHT);
    if (install(TREE_SETUP, tree_wrap, &g_tree_orig) && install(CFG_GETI, cfggeti_wrap, &g_cfggeti_orig)) {
        extern void (*sw_drawdist_live_hook)(void);
        sw_drawdist_live_hook = sw_drawdist_apply_trees;
        fprintf(stderr, "[fps] render distance hooks on 0x%llx, 0x%llx\n", (unsigned long long)TREE_SETUP,
                (unsigned long long)CFG_GETI);
    }
    if (install(VFX_CALC, vfx_wrap, &g_vfx_orig))
        fprintf(stderr, "[fps] effect system hook on 0x%llx\n", (unsigned long long)VFX_CALC);
    if (install(CULL_DISABLE, culldis_wrap, &g_culldis_orig))
        fprintf(stderr, "[fps] event culling hook on 0x%llx\n", (unsigned long long)CULL_DISABLE);
    if (install(CULL_PASS, cull_wrap, &g_cull_orig))
        fprintf(stderr, "[fps] people and pokemon distance hook on 0x%llx (+%g m)\n", (unsigned long long)CULL_PASS,
                (double)npc_keep());
    if (detail_hooks_init())
        fprintf(stderr, "[fps] detail distance hooks on 0x%llx, 0x%llx (x%g)\n", (unsigned long long)LOD_PICK,
                (unsigned long long)LOD_CAMERA, (double)detail_factor());
    if ((getenv("SW_OODLE_DUMP") || getenv("SW_OODLE_F32")) && install(OODLE_DECOMP, oodle_wrap, &g_oodle_orig))
        fprintf(stderr, "[fps] oodle dump on 0x%llx\n", (unsigned long long)OODLE_DECOMP);
    if (getenv("SW_CFG_PROBE") || getenv("SW_CFG_SET"))
        for (int i = 0; i < N_CFG_GET; i++) install(kCfgGet[i], g_cfgp_w[i], &g_cfgp_orig[i]);
    if (getenv("SW_UI_PROBE")) {
        static const u64 ap[3] = {0x228560ull, 0x228620ull, 0x2286f0ull};
        static void (*const w[3])(SwCpu *) = {uiap0, uiap1, uiap2};
        for (int i = 0; i < 3; i++) install(ap[i], w[i], &g_uiap_orig[i]);
        fprintf(stderr, "[fps] ui animation probe on\n");
    }
    if (install(STEP_FN, step_wrap, &g_step_orig))
        fprintf(stderr, "[fps] frame step hook on 0x%llx (target %d fps)\n", (unsigned long long)STEP_FN, g_fps_target);
    else
        fprintf(stderr, "[fps] frame step function 0x%llx not found: 60 fps would run at double speed\n", (unsigned long long)STEP_FN);
}

/* ---- Lua errors -----------------------------------------------------------
 * NPC behaviour (NpcTalkManager, NpcAI, LookAt...) is Lua 5.3 script code
 * (Haxe compiled to Lua, "__haxe_export_*").  A script error stops that script
 * silently, so a lifter/runtime bug shows up as NPCs that do nothing.  Every
 * error goes through luaG_errormsg(L) (main 0x159dd0, build 7FCAD279; found via
 * the "attempt to compare two %s values" xref -> luaG_runerror 0x1599d0) with
 * the message at L->top[-1].  Logged as "[lua] error: ..." (first 200).
 * Lua 5.3 layout: L->top +0x10; TValue {value, int tt}; TString: shrlen u8
 * +0xb, lnglen +0x10, chars +0x18.  SW_LUA_ERRORS=0 turns it off. */
#define LUA_ERRORMSG 0x159dd0ull
static void (*g_luaerr_orig)(SwCpu *c);
static void luaerr_wrap(SwCpu *c)
{
    static volatile long n;
    long k = InterlockedIncrement(&n);
    if (k <= 200) {
        char msg[400] = "(not a string)";
#if defined(_WIN32)
        __try {
            u64 top = *(const u64 *)(uintptr_t)(c->x[0] + 0x10);
            u64 val = *(const u64 *)(uintptr_t)(top - 16);
            int tt = *(const int *)(uintptr_t)(top - 8);
            if ((tt & 0x0f) == 4 && val) {
                size_t len = (tt & 0x3f) == 0x04 ? *(const u8 *)(uintptr_t)(val + 0xb)
                                                 : (size_t)*(const u64 *)(uintptr_t)(val + 0x10);
                if (len > sizeof msg - 1) len = sizeof msg - 1;
                memcpy(msg, (const void *)(uintptr_t)(val + 0x18), len);
                msg[len] = 0;
                for (size_t i = 0; i < len; i++) if (msg[i] == '\n' || msg[i] == '\r') msg[i] = ' ';
            }
        } __except (1) { strcpy(msg, "(unreadable)"); }
#endif
        fprintf(stderr, "[lua] error #%ld: %s  (lr 0x%llx)\n", k, msg, (unsigned long long)c->x[30]);
    }
    g_luaerr_orig(c);
}
/* ---- SW_LUA_TRACE=1: native functions called by Lua scripts ----------------
 * luaD_precall (main 0x15abc0, build 7FCAD279: `blr x23` at 0x15af8c calls the
 * C function, then L->top - n*16) with x0 = L, x1 = func, w2 = nresults.  For
 * C functions (tag 0x16 light, 0x26 closure: f at closure + 0x18) the call is
 * complete when it returns 1, the first result at the old func slot.  Counted
 * per f: calls and first results nil / false / true / number / other; printed
 * every 20 s (NPC talk / door event debugging: a check that always says no). */
#define LUA_PRECALL 0x15abc0ull
static void (*g_precall_orig)(SwCpu *c);
#define LT_N 4096
static struct { u64 f; u32 calls, nil, f0, t1, num, other, none; double last; } g_lt[LT_N];
static u32 lt_slot(u64 f)
{
    u32 h = (u32)((f >> 2) * 2654435761u) % LT_N;
    for (u32 k = 0; k < LT_N; k++, h = (h + 1) % LT_N)
        if (g_lt[h].f == f || !g_lt[h].f) { g_lt[h].f = f; return h; }
    return UINT32_MAX;
}
/* Names: walk _G (registry[2]) and one level of sub-tables for C function values.
 * Lua 5.3: L->l_G +0x18; global_State.l_registry TValue +0x40; Table: sizearray u32
 * +0xc, array +0x10, node +0x18, lsizenode u8 +0xb; Node 32 bytes {i_val, key TValue,
 * next}; TString short: len u8 +0xb, chars +0x18 (long: len u64 +0x10). */
static char g_lt_name[LT_N][64];
static void lt_str(u64 ts, int tt, char *out, size_t n)
{
    size_t len = (tt & 0x3f) == 0x04 ? *(const u8 *)(uintptr_t)(ts + 0xb) : (size_t)*(const u64 *)(uintptr_t)(ts + 0x10);
    if (len > n - 1) len = n - 1;
    memcpy(out, (const void *)(uintptr_t)(ts + 0x18), len);
    out[len] = 0;
}
static void lt_walk(u64 t, const char *prefix, int depth)
{
    u32 sizenode = 1u << *(const u8 *)(uintptr_t)(t + 0xb);
    u64 node = *(const u64 *)(uintptr_t)(t + 0x18);
    for (u32 i = 0; i < sizenode && i < (1u << 16); i++) {
        u64 nd = node + 32ull * i;
        int vt = *(const int *)(uintptr_t)(nd + 8) & 0x3f, kt = *(const int *)(uintptr_t)(nd + 0x18) & 0x3f;
        if ((kt & 0x0f) != 4) continue;
        char key[48], full[64];
        lt_str(*(const u64 *)(uintptr_t)(nd + 0x10), kt, key, sizeof key);
        snprintf(full, sizeof full, "%s%s", prefix, key);
        u64 v = *(const u64 *)(uintptr_t)nd, f = 0;
        if (vt == 0x16) f = v;
        else if (vt == 0x26) f = *(const u64 *)(uintptr_t)(v + 0x18);
        else if (vt == 0x06) f = v;   /* Lua closure: keyed by its pointer */
        else if (vt == 5 && depth == 0) {   /* table: one level down */
            char p2[64];
            snprintf(p2, sizeof p2, "%s.", key);
            lt_walk(v, p2, 1);
        }
        if (f) {
            u32 h = lt_slot(f);
            if (h != UINT32_MAX && !g_lt_name[h][0]) strncpy(g_lt_name[h], full, 63);
        }
    }
}
static int lt_cmp(const void *a, const void *b)
{
    u32 x = g_lt[*(const u32 *)a].calls, y = g_lt[*(const u32 *)b].calls;
    return x < y ? 1 : x > y ? -1 : 0;
}
static void lt_walk_globals(u64 L)
{
    /* names from _G: registry (G + 0x40) array[1] (LUA_RIDX_GLOBALS = 2) */
    u64 G = *(const u64 *)(uintptr_t)(L + 0x18), reg = *(const u64 *)(uintptr_t)(G + 0x40);
    u64 gt = *(const u64 *)(uintptr_t)(*(const u64 *)(uintptr_t)(reg + 0x10) + 16);
    lt_walk(gt, "", 0);
}
/* SW_NEWFN=1 (first-call trace, sw_runtime.c): Lua functions and the native
 * functions Lua calls are marked from start-up; after F8 each one that runs
 * for the first time is printed, so a press of A shows which scripts started:
 *   [newlua] t=<s> lua proto <p>: "k1" "k2" ...  (its string constants name it)
 *   [newlua] t=<s> C <name> f <addr>              (then its first 3 results)
 * Lua 5.3: LClosure.p +0x18; Proto.sizek +0x14, Proto.k +0x30 (TValue 16 bytes). */
extern int sw_newfn_mode;
double sw_newfn_time(void);
static int g_lt_on;                       /* SW_LUA_TRACE=1: the 20 s statistics */
#define NL_N 65536
static u64 g_nl_key[NL_N];
static int nl_mark(u64 k)                 /* 1: seen for the first time */
{
    u32 h = (u32)((k >> 3) * 2654435761u) & (NL_N - 1);
    for (u32 i = 0; i < NL_N; i++, h = (h + 1) & (NL_N - 1)) {
        if (g_nl_key[h] == k) return 0;
        if (!g_nl_key[h]) { g_nl_key[h] = k; return 1; }
    }
    return 0;
}
static struct { u64 f; int left; } g_nl_res[512];
static int g_nl_nres, g_nl_walked;
#if defined(_WIN32)
static void nl_proto(u64 proto)
{
    char line[1024];
    int n = snprintf(line, sizeof line, "[newlua] t=%.3f lua proto 0x%llx:", sw_newfn_time(), (unsigned long long)proto);
    __try {
        int sizek = *(const int *)(uintptr_t)(proto + 0x14);
        u64 k = *(const u64 *)(uintptr_t)(proto + 0x30);
        for (int i = 0, shown = 0; i < sizek && shown < 14 && n < (int)sizeof line - 72; i++) {
            int tt = *(const int *)(uintptr_t)(k + 16ull * i + 8) & 0x3f;
            if ((tt & 0x0f) != 4) continue;
            char s[60];
            lt_str(*(const u64 *)(uintptr_t)(k + 16ull * i), tt, s, sizeof s);
            n += snprintf(line + n, sizeof line - n, " \"%s\"", s);
            shown++;
        }
    } __except (1) { snprintf(line + n, sizeof line - n, " (unreadable)"); }
    fprintf(stderr, "%s\n", line);
}
#endif
static void precall_wrap(SwCpu *c)
{
    u64 L = c->x[0], func = c->x[1], f = 0, off = 0;
    int nres = (int)(u32)c->x[2];
    int ok = 0;
    u64 lua_fn = 0;
#if defined(_WIN32)
    __try {
        int tt = *(const int *)(uintptr_t)(func + 8) & 0x3f;
        if (tt == 0x16) f = *(const u64 *)(uintptr_t)func;
        else if (tt == 0x26) f = *(const u64 *)(uintptr_t)(*(const u64 *)(uintptr_t)func + 0x18);
        else if (tt == 0x06) lua_fn = *(const u64 *)(uintptr_t)func;   /* Lua closure: counted by pointer */
        off = func - *(const u64 *)(uintptr_t)(L + 0x38);   /* L->stack may move */
        ok = f != 0;
    } __except (1) { ok = 0; }
    int nl = -1;                          /* g_nl_res slot of a new C function */
    if (sw_newfn_mode) {
        __try {
            if (lua_fn) {
                u64 proto = *(const u64 *)(uintptr_t)(lua_fn + 0x18);
                if (nl_mark(proto) && sw_newfn_mode == 2) nl_proto(proto);
            } else if (f && nl_mark(f) && sw_newfn_mode == 2) {
                if (!g_nl_walked) { g_nl_walked = 1; lt_walk_globals(L); }
                u32 h = lt_slot(f);
                fprintf(stderr, "[newlua] t=%.3f C %s f 0x%llx\n", sw_newfn_time(),
                        h != UINT32_MAX && g_lt_name[h][0] ? g_lt_name[h] : "?", (unsigned long long)f);
                if (g_nl_nres < 512) g_nl_res[g_nl_nres].f = f, g_nl_res[g_nl_nres].left = 3, g_nl_nres++;
            }
            if (f && sw_newfn_mode == 2)
                for (int i = 0; i < g_nl_nres; i++) if (g_nl_res[i].f == f && g_nl_res[i].left > 0) { nl = i; break; }
        } __except (1) { }
    }
#endif
    g_precall_orig(c);
    if (lua_fn && g_lt_on) {   /* a Lua function: count the call only (it runs after precall returns) */
        u32 h = lt_slot(lua_fn);
        if (h != UINT32_MAX) g_lt[h].calls++, g_lt[h].other++;
    }
    if (!ok || (u32)c->x[0] != 1) return;
#if defined(_WIN32)
    if (nl >= 0) {   /* first results of a new C function */
        g_nl_res[nl].left--;
        char r[64] = "none";
        __try {
            u64 res = *(const u64 *)(uintptr_t)(L + 0x38) + off, top = *(const u64 *)(uintptr_t)(L + 0x10);
            if (!(nres == 0 || (nres < 0 && top <= res))) {
                int tt = *(const int *)(uintptr_t)(res + 8) & 0x3f;
                if (tt == 0) strcpy(r, "nil");
                else if (tt == 1) strcpy(r, *(const int *)(uintptr_t)res ? "true" : "false");
                else if (tt == 3) snprintf(r, sizeof r, "%g", *(const double *)(uintptr_t)res);
                else if (tt == 0x13) snprintf(r, sizeof r, "%lld", *(const long long *)(uintptr_t)res);
                else if ((tt & 0x0f) == 4) { char s[48]; lt_str(*(const u64 *)(uintptr_t)res, tt, s, sizeof s); snprintf(r, sizeof r, "\"%s\"", s); }
                else snprintf(r, sizeof r, "type %#x", tt);
            }
        } __except (1) { strcpy(r, "?"); }
        fprintf(stderr, "[newlua]   f 0x%llx -> %s\n", (unsigned long long)f, r);
    }
#endif
    if (!g_lt_on) return;
    u32 h = lt_slot(f);
    if (h == UINT32_MAX) return;
    g_lt[h].calls++;
#if defined(_WIN32)
    __try {
        u64 res = *(const u64 *)(uintptr_t)(L + 0x38) + off, top = *(const u64 *)(uintptr_t)(L + 0x10);
        if (nres == 0 || (nres < 0 && top <= res)) g_lt[h].none++;
        else {
            int tt = *(const int *)(uintptr_t)(res + 8) & 0x3f;
            if (tt == 0) g_lt[h].nil++;
            else if (tt == 1) { if (*(const int *)(uintptr_t)res) g_lt[h].t1++; else g_lt[h].f0++; }
            else if (tt == 3) g_lt[h].num++, g_lt[h].last = *(const double *)(uintptr_t)res;
            else if (tt == 0x13) g_lt[h].num++, g_lt[h].last = (double)*(const long long *)(uintptr_t)res;
            else g_lt[h].other++;
        }
    } __except (1) {}
    static ULONGLONG next;
    ULONGLONG now = GetTickCount64();
    if (!next) next = now + 20000;
    if (now < next) return;
    next = now + 20000;
    __try { lt_walk_globals(L); } __except (1) { fprintf(stderr, "[luatrace] could not walk _G\n"); }
    static u32 idx[LT_N];
    u32 n = 0;
    for (u32 i = 0; i < LT_N; i++) if (g_lt[i].calls) idx[n++] = i;
    qsort(idx, n, sizeof idx[0], lt_cmp);
    fprintf(stderr, "[luatrace] %u native functions called in the last 20 s:\n", n);
    for (u32 k = 0; k < n; k++) {
        u32 i = idx[k];
        if (!g_lt_name[i][0] && g_lt[i].other == g_lt[i].calls) continue;   /* unnamed Lua functions: noise */
        fprintf(stderr, "[luatrace]   %-44s f 0x%llx: %u calls; nil %u false %u true %u num %u (last %g) other %u none %u\n",
                g_lt_name[i][0] ? g_lt_name[i] : "?", (unsigned long long)g_lt[i].f, g_lt[i].calls, g_lt[i].nil, g_lt[i].f0, g_lt[i].t1, g_lt[i].num,
                g_lt[i].last, g_lt[i].other, g_lt[i].none);
    }
    memset(g_lt, 0, sizeof g_lt);
    memset(g_lt_name, 0, sizeof g_lt_name);
#endif
}

/* SW_COMP_PROBE=<type hash>: GetComponent (main 0x1d4c368: x0 object, x1 -> {u64 hash,
 * const char *name, ...}, result ref in [x8]) for that type: log the component, its
 * vtable and the first 40 entries, and the caller (first 3 hits).  PlayerTalkCandidacy
 * = 0x18255554c7d12183 (main 0x1d34d98). */
#define GET_COMPONENT 0x1d4c368ull
static void (*g_getcomp_orig)(SwCpu *c);
static u64 g_comp_hash;
static void getcomp_wrap(SwCpu *c)
{
    u64 x1 = c->x[1], x8 = c->x[8], lr = c->x[30], hash = 0, obj = c->x[0];
#if defined(_WIN32)
    __try { hash = *(const u64 *)(uintptr_t)x1; } __except (1) { hash = 0; }
#endif
    g_getcomp_orig(c);
    if (hash != g_comp_hash) return;
    static volatile long n;
    long k = InterlockedIncrement(&n);
    if (k > 3) return;
#if defined(_WIN32)
    __try {
        u64 comp = *(const u64 *)(uintptr_t)x8, vt = comp ? *(const u64 *)(uintptr_t)comp : 0;
        fprintf(stderr, "[comp] GetComponent(%#llx) from lr %#llx on object %#llx -> %#llx, vtable %#llx\n",
                (unsigned long long)hash, (unsigned long long)lr, (unsigned long long)obj, (unsigned long long)comp,
                (unsigned long long)vt);
        for (int i = 0; vt && i < 40; i++)
            fprintf(stderr, "[comp]   vt[%d] = %#llx\n", i, (unsigned long long)*(const u64 *)(uintptr_t)(vt + 8 * i));
    } __except (1) { fprintf(stderr, "[comp] unreadable result\n"); }
#endif
}

/* luaD_throw (main 0x15a320: x0 L, w1 errcode; L->errorJmp +0x58).  luaG_errormsg
 * (above) sees runtime errors only; load errors of precompiled chunks ("truncated
 * precompiled chunk", "bad binary format"), out-of-memory and errors in __gc / the
 * message handler reach luaD_throw directly.  Logged as [luathrow] (first 300;
 * code 1 = yield is not an error and not logged). */
#define LUA_THROW 0x15a320ull
static void (*g_throw_orig)(SwCpu *c);
static void throw_wrap(SwCpu *c)
{
    int code = (int)(u32)c->x[1];
    if (code != 1) {
        static volatile long n;
        long k = InterlockedIncrement(&n);
        if (k <= 300) {
            static const char *names[] = {"ok", "yield", "run", "syntax", "mem", "gcmm", "err"};
            char msg[300] = "(no message)";
#if defined(_WIN32)
            __try {
                u64 top = *(const u64 *)(uintptr_t)(c->x[0] + 0x10);
                int tt = *(const int *)(uintptr_t)(top - 8) & 0x3f;
                if ((tt & 0x0f) == 4) lt_str(*(const u64 *)(uintptr_t)(top - 16), tt, msg, sizeof msg);
                for (char *p = msg; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
            } __except (1) { strcpy(msg, "(unreadable)"); }
#endif
            fprintf(stderr, "[luathrow] #%ld code %d (%s): %s  (lr 0x%llx)\n", k, code,
                    code >= 0 && code <= 6 ? names[code] : "?", msg, (unsigned long long)c->x[30]);
        }
    }
    g_throw_orig(c);
}

/* SW_MSG_PROBE=1: the field message hub (build 7FCAD279).  An A press on a door
 * prompt is handled by main 0x19353c8 (action types 14/15/33): it plays
 * PLAY_UI_COMMON_DECIDE and broadcasts a message with 0x164d4b0(&msg_hash,
 * target_id, 4 x empty hash); 0x164d9c0 is a second broadcast of the same shape.
 * Hub: P = [GOT 0x43357a0], G = *P, hub = [[[G+0x100]+0x100]+0x60]; listeners =
 * vector<Listener*> at hub+0x58.  Each listener holds an unordered_map<u64 msg,
 * vector<handler>> (libc++: buckets +0x78, count +0x80, first node +0x88, size
 * +0x90; node {next, hash, key +0x10, value begin +0x18, end +0x20}).  Logged per
 * call (first 400): message, target, every listener and whether it handles the
 * message ("HAS n"); a listener's keys on the first call and whenever its key
 * count changes. */
#define MSG_SEND_A 0x164d4b0ull
#define MSG_SEND_B 0x164d9c0ull
static void (*g_msg_orig_a)(SwCpu *c), (*g_msg_orig_b)(SwCpu *c);
#if defined(_WIN32)
static int msg_rd(u64 a, u64 *v)  /* fault-free read: ReadProcessMemory, no exception */
{
    SIZE_T n = 0;
    *v = 0;
    return a >= 0x10000 && a < 0x0000800000000000ull && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, v, 8, &n) && n == 8;
}
#endif
static void msg_probe(SwCpu *c, const char *which, int full)
{
#if defined(_WIN32)
    static volatile long n, nb;
    static u64 seen_b[256], last_size[64], last_l[64];
    static int dumps;
    u64 msg = 0;
    if (!msg_rd(c->x[0], &msg)) msg = 0;
    if (!full) {                       /* B: log each distinct message once */
        for (int i = 0; i < 256; i++) {
            if (seen_b[i] == msg) return;
            if (!seen_b[i]) { if (InterlockedCompareExchange64((volatile long long *)&seen_b[i], (long long)msg, 0) == 0) break; return; }
        }
        if (InterlockedIncrement(&nb) > 256) return;
    }
    long k = InterlockedIncrement(&n);
    if (k > 300) return;
    u64 pg = 0, g = 0, x19 = 0, x0 = 0, hub = 0, lb = 0, le = 0;
    int ok = msg_rd(0x43357a0ull, &pg) && msg_rd(pg, &g) && msg_rd(g + 0x100, &x19) && msg_rd(x19 + 0x100, &x0) &&
             msg_rd(x0 + 0x60, &hub) && msg_rd(hub + 0x58, &lb) && msg_rd(hub + 0x60, &le);
    int nl = ok && le >= lb ? (int)((le - lb) / 8) : -1;
    char line[1600];
    int m = snprintf(line, sizeof line, "[msg] #%ld %s t=%.3f msg 0x%016llx x1 0x%llx x2 0x%llx lr 0x%llx thr %u chain %llx>%llx>%llx>%llx>hub %llx listeners %d:",
                     k, which, sw_newfn_time(), (unsigned long long)msg, (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                     (unsigned long long)c->x[30], (unsigned)GetCurrentThreadId(), (unsigned long long)pg, (unsigned long long)g,
                     (unsigned long long)x19, (unsigned long long)x0, (unsigned long long)hub, nl);
    int nhas = 0, nempty = 0;
    for (int i = 0; i < nl && i < 4096 && m < (int)sizeof line - 100; i++) {
        u64 L = 0, vt = 0, size = 0, node = 0, nh = 0;
        int has = 0;
        if (!msg_rd(lb + 8ull * i, &L) || !L) { m += snprintf(line + m, sizeof line - m, " L%d(null)", i); continue; }
        msg_rd(L, &vt), msg_rd(L + 0x90, &size), msg_rd(L + 0x88, &node);
        {   /* same lookup as main 0x16522e0: buckets [L+0x78], bucket count [L+0x80], identity hash */
            u64 nb = 0, bk = 0, nd = 0;
            msg_rd(L + 0x80, &nb), msg_rd(L + 0x78, &bk);
            if (nb) {
                int pow2 = (nb & (nb - 1)) == 0;
                u64 idx = pow2 ? (msg & (nb - 1)) : (msg % nb);
                if (msg_rd(bk + idx * 8, &nd) && nd && msg_rd(nd, &nd)) {
                    for (int j = 0; nd && j < 4096; j++) {
                        u64 h = 0, key = 0, vb = 0, ve = 0;
                        if (!msg_rd(nd + 8, &h) || !msg_rd(nd + 0x10, &key)) break;
                        if (h == msg && key == msg) { msg_rd(nd + 0x18, &vb), msg_rd(nd + 0x20, &ve), nh = (ve - vb) / 8, has = 1; break; }
                        if ((pow2 ? (h & (nb - 1)) : (h % nb)) != idx) break;
                        if (!msg_rd(nd, &nd)) break;
                    }
                }
            }
            size = nb;   /* report the bucket count below */
        }
        if (i < 3 && k == 1) {  /* raw dump of the first listeners, once */
            char raw[1200];
            int q = 0;
            for (int w = 0; w < 0xE0 / 8; w++) { u64 v = 0; msg_rd(L + 8ull * w, &v); q += snprintf(raw + q, sizeof raw - q, " +%02x=%llx", w * 8, (unsigned long long)v); }
            fprintf(stderr, "[msg]   raw L%d 0x%llx:%s\n", i, (unsigned long long)L, raw);
        }
        nempty += size == 0;
        if (has) nhas++, m += snprintf(line + m, sizeof line - m, " L%d(vt %llx nb %llu handlers %llu)", i, (unsigned long long)vt,
                                       (unsigned long long)size, (unsigned long long)nh);
        if (0 && i < 64 && (last_l[i] != L || last_size[i] != size) && dumps < 60) {
            dumps++;
            last_l[i] = L, last_size[i] = size;
            char keys[1600];
            int q = 0;
            u64 nd = 0;
            msg_rd(L + 0x88, &nd);
            for (u64 j = 0; nd && j < size && j < 100 && q < (int)sizeof keys - 40; j++) {
                u64 key = 0, vb = 0, ve = 0;
                msg_rd(nd + 0x10, &key), msg_rd(nd + 0x18, &vb), msg_rd(nd + 0x20, &ve);
                q += snprintf(keys + q, sizeof keys - q, " %llx:%llu", (unsigned long long)key, (unsigned long long)((ve - vb) / 8));
                if (!msg_rd(nd, &nd)) break;
            }
            keys[q] = 0;
            fprintf(stderr, "[msg]   listener %d 0x%llx vt 0x%llx: %llu keys (msg:handlers)%s\n", i, (unsigned long long)L,
                    (unsigned long long)vt, (unsigned long long)size, keys);
        }
    }
    fprintf(stderr, "%s | handled by %d, empty maps %d\n", line, nhas, nempty);
#else
    (void)c, (void)which, (void)full;
#endif
}
static void msg_wrap_a(SwCpu *c) { msg_probe(c, "A", 1); g_msg_orig_a(c); }
static void msg_wrap_b(SwCpu *c) { msg_probe(c, "B", 0); g_msg_orig_b(c); }

static void lua_probe_init(void)
{
    {   /* SW_MSG_PROBE=1: field message hub broadcasts (see msg_probe_wrap) */
        const char *m = getenv("SW_MSG_PROBE");
        if (m && *m == '1' && build_ok()) {
            if (install(MSG_SEND_A, msg_wrap_a, &g_msg_orig_a)) fprintf(stderr, "[msg] probing broadcast 0x%llx\n", (unsigned long long)MSG_SEND_A);
            if (install(MSG_SEND_B, msg_wrap_b, &g_msg_orig_b)) fprintf(stderr, "[msg] probing broadcast 0x%llx\n", (unsigned long long)MSG_SEND_B);
        }
    }
    {
        const char *h = getenv("SW_COMP_PROBE");
        if (h && *h && build_ok() && (g_comp_hash = strtoull(h, NULL, 16)) && install(GET_COMPONENT, getcomp_wrap, &g_getcomp_orig))
            fprintf(stderr, "[comp] probing GetComponent for %#llx\n", (unsigned long long)g_comp_hash);
    }
    {
        const char *t = getenv("SW_LUA_TRACE");
        g_lt_on = t && *t == '1';
        if ((g_lt_on || sw_newfn_mode) && build_ok() && install(LUA_PRECALL, precall_wrap, &g_precall_orig))
            fprintf(stderr, "[luatrace] %s (luaD_precall 0x%llx)\n",
                    g_lt_on ? "tracing native calls from Lua" : "first calls of Lua functions (SW_NEWFN)",
                    (unsigned long long)LUA_PRECALL);
    }
    {
        const char *t = getenv("SW_LUA_THROW");
        if (((t && *t == '1') || sw_newfn_mode) && build_ok() && install(LUA_THROW, throw_wrap, &g_throw_orig))
            fprintf(stderr, "[luathrow] logging Lua throws other than yield (luaD_throw 0x%llx)\n", (unsigned long long)LUA_THROW);
    }
    const char *e = getenv("SW_LUA_ERRORS");
    if ((e && *e == '0') || !build_ok()) return;
    if (install(LUA_ERRORMSG, luaerr_wrap, &g_luaerr_orig))
        fprintf(stderr, "[lua] logging script errors (luaG_errormsg 0x%llx)\n", (unsigned long long)LUA_ERRORMSG);
}

/* ---- Finding the frame step in memory ---------------------------------------
 * SW_SCAN_F32=<value>  at presents 1800 and 2400 (in game, walking) list every
 *                      guest f32 equal to <value> (e.g. 0.0333333) both times
 *                      -> logs/scan_f32.txt
 * SW_POKE=0xADDR:<float>[,...]  write those floats every present (test a candidate:
 *                      1/60 into the 30 fps step must halve the game speed) */
#if defined(_WIN32)
static u64 *g_scan; static size_t g_nscan, g_capscan;
static void scan_region(u64 lo, u64 hi, u32 bits, int first)
{
    MEMORY_BASIC_INFORMATION mi;
    for (u64 a = lo; a < hi && VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi); a = (u64)(uintptr_t)mi.BaseAddress + mi.RegionSize) {
        if (mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mi.Protect & PAGE_GUARD))
            continue;
        u64 s = (u64)(uintptr_t)mi.BaseAddress, e = s + mi.RegionSize;
        if (s < lo) s = lo;
        if (e > hi) e = hi;
        if (!first) continue;
        for (u64 p = s; p + 4 <= e; p += 4)
            if (*(const u32 *)(uintptr_t)p == bits) {
                if (g_nscan == g_capscan) { g_capscan = g_capscan ? g_capscan * 2 : 4096; g_scan = realloc(g_scan, g_capscan * 8); }
                g_scan[g_nscan++] = p;
            }
    }
}
static void scan_f32(u64 presents)
{
    static int on = -1; static u32 bits; static int stage;
    if (on < 0) { const char *e = getenv("SW_SCAN_F32"); on = e != NULL; if (e) { float f = (float)atof(e); memcpy(&bits, &f, 4); } }
    if (!on) return;
    if (stage == 0 && presents >= 1800) {
        static const u64 R[][2] = {{0x100000ull, 0x6200000ull}, {0x80000000ull, 0xA0000000ull}, {0x1000000000ull, 0x8000000000ull}};
        for (int i = 0; i < 3; i++) scan_region(R[i][0], R[i][1], bits, 1);
        fprintf(stderr, "[scan] %zu guest f32 == 0x%08x at present %llu\n", g_nscan, bits, (unsigned long long)presents);
        stage = 1;
    } else if (stage == 1) {
        size_t k = 0;
        __try {
            for (size_t i = 0; i < g_nscan; i++) if (*(const u32 *)(uintptr_t)g_scan[i] == bits) g_scan[k++] = g_scan[i];
        } __except (1) { }
        g_nscan = k;
        FILE *f = fopen("logs/scan_f32.txt", "w");
        for (size_t i = 0; f && i < g_nscan; i++) fprintf(f, "0x%llx\n", (unsigned long long)g_scan[i]);
        if (f) fclose(f);
        fprintf(stderr, "[scan] %zu still equal at present %llu -> logs/scan_f32.txt\n", g_nscan, (unsigned long long)presents);
        stage = 2;
    }
}
static void poke(void)
{
    static int n = -1; static u64 addr[16]; static float val[16];
    if (n < 0) {
        n = 0;
        const char *e = getenv("SW_POKE");
        while (e && *e && n < 16) {
            char *end; addr[n] = strtoull(e, &end, 0);
            if (*end != ':') break;
            val[n++] = (float)strtod(end + 1, &end);
            e = *end == ',' ? end + 1 : end;
        }
        for (int i = 0; i < n; i++) fprintf(stderr, "[poke] 0x%llx = %g every present\n", (unsigned long long)addr[i], val[i]);
    }
    for (int i = 0; i < n; i++) __try { *(volatile float *)(uintptr_t)addr[i] = val[i]; } __except (1) { }
}
#endif

#if defined(_WIN32)
static void scan_clock(u64 presents);
static void bisect_step(u64 presents);
#endif
void sw_fps_debug_present(void)
{
#if defined(_WIN32)
    static volatile u64 presents;
    u64 p = ++presents;
    { extern volatile long long sw_present_count; sw_present_count = (long long)p; }   /* SW_HANG_DUMP (sw_thread.c) */
    {   /* SW_FIND_F32=<tA>,<tB>,<lo>,<hi>: guest f32 in [lo,hi] at tA seconds after the first present that are 0 at
         * tB (e.g. a held stick axis, released in between) -> "[find]" lines.  Finds input values to watch. */
        static int st = -1; static double ta, tb, lo, hi; static ULONGLONG t0; static u64 *cand; static float *cval; static size_t n, cap;
        if (st < 0) { const char *e = getenv("SW_FIND_F32"); st = e && sscanf(e, "%lf,%lf,%lf,%lf", &ta, &tb, &lo, &hi) == 4 ? 0 : 3; t0 = GetTickCount64(); }
        double t = (GetTickCount64() - t0) / 1000.0;
        if (st == 0 && t >= ta) {
            static const u64 R[][2] = {{0x100000ull, 0x6200000ull}, {0x80000000ull, 0xA0000000ull}, {0x1000000000ull, 0x8000000000ull}};
            MEMORY_BASIC_INFORMATION mi;
            __try {
                for (int r = 0; r < 3; r++)
                    for (u64 a = R[r][0]; a < R[r][1] && VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi); a = (u64)(uintptr_t)mi.BaseAddress + mi.RegionSize) {
                        if (mi.State != MEM_COMMIT || !(mi.Protect & PAGE_READWRITE) || (mi.Protect & PAGE_GUARD)) continue;
                        u64 s0 = (u64)(uintptr_t)mi.BaseAddress, e0 = s0 + mi.RegionSize;
                        for (u64 q = s0; q + 4 <= e0; q += 4) {
                            float f = *(const float *)(uintptr_t)q;
                            if (f >= lo && f <= hi) { if (n == cap) { cap = cap ? cap * 2 : 4096; cand = realloc(cand, cap * 8); cval = realloc(cval, cap * 4); } cval[n] = f; cand[n++] = q; }
                        }
                    }
            } __except (1) { }
            fprintf(stderr, "[find] %zu f32 in [%g,%g] at %.1f s\n", n, lo, hi, t);
            st = 1;
        } else if (st == 1 && t >= tb) {
            size_t k = 0;
            __try { for (size_t i = 0; i < n; i++) { float f = *(const float *)(uintptr_t)cand[i]; if (f == 0.0f) { cval[k] = cval[i]; cand[k++] = cand[i]; } } } __except (1) { }
            fprintf(stderr, "[find] %zu of them are 0 at %.1f s\n", k, t);
            for (size_t i = 0; i < k && i < 4000; i++)
                fprintf(stderr, "[find] 0x%llx was %g\n", (unsigned long long)cand[i], (double)cval[i]);
            {   /* SW_FIND_WATCH=<lo>,<hi>,<n>: then watch reads/writes of the n-th candidate whose value was in [lo,hi]
                 * (heap addresses differ between runs, so find + watch must happen in one run) */
                const char *w = getenv("SW_FIND_WATCH"); double wl, wh; int wn;
                if (w && sscanf(w, "%lf,%lf,%d", &wl, &wh, &wn) == 3)
                    for (size_t i = 0; i < k; i++)
                        if (cval[i] >= wl && cval[i] <= wh && wn-- == 0) {
                            extern void sw_watch_access(u64);
                            fprintf(stderr, "[find] watching 0x%llx (was %g)\n", (unsigned long long)cand[i], (double)cval[i]);
                            sw_watch_access(cand[i]);
                            break;
                        }
            }
            st = 2;
        }
    }
    {   /* SW_FIND_RATE=<tA>,<dt>: stick-driven values (camera angle, ...) and their speed.  The right stick is forced
         * to x = SW_FIND_RATE_X (default 1) from tA-2 s on.  Stage 0 (tA) copies guest RW memory, stage 1 lists the
         * f32 that changed since (copying takes seconds, so only a candidate list); then three quick timed reads
         * dt apart while held (v1..v3: both steps the same sign, rates within 25%), release, and two reads dt apart
         * from 1 s later (v4 == v5: stopped).  "[rate] addr v1 -> v3 rate/s".  SW_RATE_ADDR=<addr>: skip the search,
         * report that value's rate (camera speed check at 30/60/90/120). */
        typedef struct { u64 a; float v[5]; } RC;
        static int st = -1; static double ta, dt, tm[7], t7; static LARGE_INTEGER f0, fq;
        static struct { u64 base; size_t size; float *copy; } *rg; static size_t nrg;
        static RC *rc; static size_t nrc;
        if (st < 0) {
            const char *e = getenv("SW_FIND_RATE");
            st = e && sscanf(e, "%lf,%lf", &ta, &dt) == 2 ? 0 : 9;
            QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&f0);
            const char *one = getenv("SW_RATE_ADDR");
            if (st == 0 && one) { rc = calloc(1, sizeof *rc); rc[0].a = strtoull(one, NULL, 16); nrc = 1; }
        }
        if (st == 7 || st == 8) {   /* trace: rest 1.5 s, then hold 2 s from rest; log the yaw-like candidates each present */
            extern volatile float sw_force_rx;
            static u64 tr[32]; static int ntr;
            LARGE_INTEGER nw; QueryPerformanceCounter(&nw);
            double t = (double)(nw.QuadPart - f0.QuadPart) / (double)fq.QuadPart;
            if (st == 7 && !ntr) {
                for (size_t i = 0; i < nrc && ntr < 32; i++) {
                    float r = (rc[i].v[2] - rc[i].v[0]) / (float)(tm[4] - tm[2]);
                    if (fabsf(r) >= 1.0f && fabsf(r) <= 20.0f && (fabsf(rc[i].v[2]) > 1.05f || fabsf(rc[i].v[0]) > 1.05f) && fabsf(rc[i].v[2]) < 20.0f && fabsf(rc[i].v[0]) < 20.0f) tr[ntr++] = rc[i].a;
                }
                t7 = t; if (!ntr) st = 9;
            }
            if (st == 7 && t >= t7 + 1.5) { st = 8; t7 = t; }
            if (st == 8) {
                static float dir = 2; if (dir > 1) { const char *e = getenv("SW_FIND_RATE_X"); dir = e ? (float)atof(e) : 1.0f; }
                sw_force_rx = t < t7 + 2.0 ? dir : 0.0f;
                char line[2048]; int o = snprintf(line, sizeof line, "[trace] %.4f", t - t7);
                __try { for (int i = 0; i < ntr; i++) o += snprintf(line + o, sizeof line - o, " %llx=%.5f", (unsigned long long)tr[i] & 0xffffff, (double)*(const float *)(uintptr_t)tr[i]); } __except (1) { }
                fprintf(stderr, "%s\n", line);
                static float mn[32], mx[32], fv[32], lv[32];
                __try { for (int i = 0; i < ntr; i++) { float v = *(const float *)(uintptr_t)tr[i]; if (t - t7 < 0.05) mn[i] = mx[i] = fv[i] = v; lv[i] = v; if (v < mn[i]) mn[i] = v; if (v > mx[i]) mx[i] = v; } } __except (1) { }
                if (t >= t7 + 3.0) {
                    st = 9;
                    /* SW_FIND_RATE_WATCH=1: watch the writers of the candidate that moved the most beyond +-1.3
                     * (the largest monotonic move below 40: an unwrapped angle, not a sin/cos term) while the stick is held again for 15 s */
                    const char *w = getenv("SW_FIND_RATE_WATCH");
                    int best = -1;
                    for (int i = 0; i < ntr; i++)
                        if (mx[i] - mn[i] < 40.0f && fabsf(lv[i] - fv[i]) > 0.9f * (mx[i] - mn[i]) && (mx[i] > 1.3f || mn[i] < -1.3f) &&
                            (best < 0 || fabsf(lv[i] - fv[i]) > fabsf(lv[best] - fv[best]))) best = i;   /* monotonic, angle-sized */
                    if (w && *w == '1' && best >= 0) {
                        extern void sw_watch_writes(u64);
                        fprintf(stderr, "[trace] watching writes of 0x%llx (range %g..%g)\n", (unsigned long long)tr[best], (double)mn[best], (double)mx[best]);
                        { const char *o = getenv("SW_FIND_RATE_WATCH_OFF"); long long off = o ? strtoll(o, NULL, 0) : 0;   /* watch a field next to it */
                          const char *l12 = getenv("SW_FIND_RATE_WATCH_LOW12");   /* or: same page, this offset */
                          sw_watch_writes(l12 ? (tr[best] & ~0xFFFull) | (strtoull(l12, NULL, 0) & 0xFFF) : tr[best] + (u64)off); }
                        st = 10; t7 = t;
                    }
                }
            }
        }
        if (st == 10) {   /* SW_FIND_RATE_WATCH: stick held while the writers are logged */
            extern volatile float sw_force_rx;
            LARGE_INTEGER nw; QueryPerformanceCounter(&nw);
            double t = (double)(nw.QuadPart - f0.QuadPart) / (double)fq.QuadPart;
            sw_force_rx = t < t7 + 15.0 ? 1.0f : 0.0f;
            if (t >= t7 + 15.0) st = 11;
        }
        if (st < 7) {
            extern volatile float sw_force_rx;
            LARGE_INTEGER nw; QueryPerformanceCounter(&nw);
            double t = (double)(nw.QuadPart - f0.QuadPart) / (double)fq.QuadPart;
            {   static float dir = 2; if (dir > 1) { const char *e = getenv("SW_FIND_RATE_X"); dir = e ? (float)atof(e) : 1.0f; }
                sw_force_rx = st <= 4 && t >= ta - 2 ? dir : 0.0f; }
            if (st == 0 && nrc == 1) st = 2, tm[1] = t;          /* SW_RATE_ADDR: no search */
            double due = st == 0 ? ta : tm[st - 1] + (st == 5 ? dt + 1.0 : dt);
            if (t >= due) {
                tm[st] = t;
                __try {
                    if (st == 0) {
                        static const u64 R[][2] = {{0x100000ull, 0x6200000ull}, {0x80000000ull, 0xA0000000ull}, {0x1000000000ull, 0x8000000000ull}};
                        MEMORY_BASIC_INFORMATION mi; size_t cap = 0, bytes = 0;
                        for (int r = 0; r < 3; r++)
                            for (u64 a = R[r][0]; a < R[r][1] && VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi); a = (u64)(uintptr_t)mi.BaseAddress + mi.RegionSize) {
                                if (mi.State != MEM_COMMIT || !(mi.Protect & PAGE_READWRITE) || (mi.Protect & PAGE_GUARD)) continue;
                                if (nrg == cap) { cap = cap ? cap * 2 : 1024; rg = realloc(rg, cap * sizeof *rg); }
                                rg[nrg].base = (u64)(uintptr_t)mi.BaseAddress; rg[nrg].size = mi.RegionSize;
                                rg[nrg].copy = malloc(mi.RegionSize);
                                if (!rg[nrg].copy) continue;
                                memcpy(rg[nrg].copy, mi.BaseAddress, mi.RegionSize); bytes += mi.RegionSize; nrg++;
                            }
                        fprintf(stderr, "[rate] snapshot %zu regions, %.0f MB at %.2f s\n", nrg, bytes / 1048576.0, t);
                    } else if (st == 1) {
                        size_t cap = 0;
                        for (size_t r = 0; r < nrg; r++) {
                            const float *o = rg[r].copy, *n = (const float *)(uintptr_t)rg[r].base;
                            for (size_t i = 0; i < rg[r].size / 4; i++) {
                                float a = o[i], b = n[i];
                                if (a == b || !(a > -1e5f && a < 1e5f) || !(b > -1e5f && b < 1e5f)) continue;
                                if (nrc == cap) { cap = cap ? cap * 2 : 65536; rc = realloc(rc, cap * sizeof *rc); }
                                rc[nrc++].a = rg[r].base + 4 * i;
                            }
                            free(rg[r].copy);
                        }
                        fprintf(stderr, "[rate] %zu f32 changed at %.2f s\n", nrc, t);
                    } else {
                        int k0 = st - 2;                         /* v[k0] */
                        size_t k = 0;
                        for (size_t i = 0; i < nrc; i++) {
                            RC c = rc[i]; float v = *(const float *)(uintptr_t)c.a;
                            c.v[k0] = v;
                            if (nrc > 1 && st == 4) {
                                float d1 = c.v[1] - c.v[0], d2 = c.v[2] - c.v[1];
                                float r1 = d1 / (float)(tm[3] - tm[2]), r2 = d2 / (float)(tm[4] - tm[3]);
                                if (d1 == 0 || (d1 > 0) != (d2 > 0) || fabsf(r1 - r2) > 0.25f * fabsf(r1)) continue;
                            }
                            if (nrc > 1 && st == 6 && c.v[4] != c.v[3]) continue;
                            rc[k++] = c;
                        }
                        nrc = k;
                        if (st == 4 || st == 6) fprintf(stderr, "[rate] %zu left at %.2f s\n", nrc, t);
                        if (st == 6)
                            for (size_t i = 0; i < nrc && i < 2000; i++)
                                fprintf(stderr, "[rate] 0x%llx %g -> %g -> %g  rate %g/s (%.3f s held), after release %g\n",
                                        (unsigned long long)rc[i].a, (double)rc[i].v[0], (double)rc[i].v[1], (double)rc[i].v[2],
                                        (double)((rc[i].v[2] - rc[i].v[0]) / (tm[4] - tm[2])), tm[4] - tm[2], (double)rc[i].v[4]);
                    }
                } __except (1) { fprintf(stderr, "[rate] fault in stage %d\n", st); }
                st++;
            }
        }
    }
    {   /* SW_WATCHR=<addr>,<s>: as SW_WATCHW, reads too */
        static int st = -1; static unsigned long long wa; static double wt; static ULONGLONG t0;
        if (st < 0) { const char *e = getenv("SW_WATCHR"); st = e && sscanf(e, "%llx,%lf", &wa, &wt) == 2 ? 0 : 2; t0 = GetTickCount64(); }
        if (st == 0 && (GetTickCount64() - t0) / 1000.0 >= wt) {
            extern void sw_watch_access(u64);
            sw_watch_access((u64)wa); st = 1;
        }
    }
    {   /* SW_WATCHW=<addr>,<s>: <s> seconds after the first present, log the guest code writing <addr> (SW_BISECT=2 tool) */
        static int st = -1; static unsigned long long wa; static double wt; static ULONGLONG t0;
        if (st < 0) { const char *e = getenv("SW_WATCHW"); st = e && sscanf(e, "%llx,%lf", &wa, &wt) == 2 ? 0 : 2; t0 = GetTickCount64(); }
        if (st == 0 && (GetTickCount64() - t0) / 1000.0 >= wt) {
            extern void sw_watch_writes(u64);
            fprintf(stderr, "[watchw] watching writes to 0x%llx\n", wa);
            sw_watch_writes((u64)wa); st = 1;
        }
    }
    if (sw_newfn_mode) {   /* SW_NEWFN=1: frame count; F8 (game window in front) or SW_NEWFN_DELAY=<s> starts logging */
        extern void sw_newfn_present(void), sw_newfn_start(void);
        sw_newfn_present();
        static int was, delay = -2;
        if (delay == -2) { const char *e = getenv("SW_NEWFN_DELAY"); delay = e ? atoi(e) : -1; }
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        int down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if ((down && !was) || (delay >= 0 && sw_newfn_time() >= delay)) sw_newfn_start();
        was = down;
    }
    scan_f32(p);
    scan_clock(p);
    bisect_step(p);
    poke();
#endif
}

/* SW_SCAN_CLOCK=1: game clocks - guest f32 / f64 that grow by about the frames'
 * game time between presents 1800 and 2400 (600 frames: 20 s at 30 fps) and keep
 * growing at that rate to present 3000 -> logs/scan_clock.txt (addr type v1 v2 v3) */
#if defined(_WIN32)
typedef struct { u64 a; double v; u8 dbl; } ClockC;
static ClockC *g_cc; static size_t g_ncc, g_capcc;
static void clock_add(u64 a, double v, u8 dbl)
{
    if (g_ncc == g_capcc) { g_capcc = g_capcc ? g_capcc * 2 : 1 << 20; g_cc = realloc(g_cc, g_capcc * sizeof *g_cc); }
    g_cc[g_ncc++] = (ClockC){a, v, dbl};
}
static void clock_snap(void)
{
    static const u64 R[][2] = {{0x100000ull, 0x6200000ull}, {0x80000000ull, 0xA0000000ull}, {0x1000000000ull, 0x8000000000ull}};
    MEMORY_BASIC_INFORMATION mi;
    for (int r = 0; r < 3; r++)
        for (u64 a = R[r][0]; a < R[r][1] && VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi); a = (u64)(uintptr_t)mi.BaseAddress + mi.RegionSize) {
            if (mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mi.Protect & PAGE_GUARD)) continue;
            u64 s = (u64)(uintptr_t)mi.BaseAddress, e = s + mi.RegionSize;
            for (u64 p = s; p + 8 <= e; p += 4) {
                float f = *(const float *)(uintptr_t)p;
                if (f > 1.0f && f < 1.0e6f) clock_add(p, f, 0);
                if (!(p & 7)) { double d = *(const double *)(uintptr_t)p; if (d > 1.0 && d < 1.0e7) clock_add(p, d, 1); }
            }
        }
}
static double clock_read(const ClockC *c) { return c->dbl ? *(const double *)(uintptr_t)c->a : *(const float *)(uintptr_t)c->a; }
static void scan_clock(u64 presents)
{
    /* SW_SCAN_AT=<s>[,<span s>]: start <s> seconds after the first present and compare two spans of <span> s
     * (default 20) instead of presents 1800/2400/3000 - at 90/120 fps those presents are still the title screen. */
    static int on = -1, stage; static double *v2; static ULONGLONG w1, w2, w0; static double at = -1, span = 20;
    static u64 p1, p2;
    if (on < 0) {
        const char *e = getenv("SW_SCAN_CLOCK"); on = e && *e == '1';
        if ((e = getenv("SW_SCAN_AT"))) { at = atof(e); const char *c = strchr(e, ','); if (c && atof(c + 1) > 1) span = atof(c + 1); }
    }
    if (!on) return;
    ULONGLONG now = GetTickCount64();
    if (!w0) w0 = now;
    double t = (now - w0) / 1000.0;
    int go = at < 0 ? presents >= (stage == 0 ? 1800u : stage == 1 ? 2400u : 3000u)
                    : t >= at + span * stage;
    if (!go) return;
    if (stage == 0) {
        clock_snap(); w1 = GetTickCount64(); p1 = presents;
        fprintf(stderr, "[scan] clock: %zu candidates at present 1800\n", g_ncc);
        stage = 1;
    } else if (stage == 1) {
        size_t k = 0;
        __try {
            for (size_t i = 0; i < g_ncc; i++) {
                double d = clock_read(&g_cc[i]) - g_cc[i].v;
                static double rlo = -1, rhi;   /* SW_SCAN_RANGE=lo,hi: |growth| over the first span (default 4..80) */
                if (rlo < 0) { const char *e = getenv("SW_SCAN_RANGE"); rlo = 4; rhi = 80; if (e) sscanf(e, "%lf,%lf", &rlo, &rhi); }
                double ad = d < 0 ? -d : d;
                if (ad > rlo && ad < rhi) g_cc[k++] = g_cc[i];
            }
        } __except (1) { }
        g_ncc = k;
        v2 = malloc((g_ncc + 1) * sizeof *v2);
        for (size_t i = 0; i < g_ncc; i++) v2[i] = clock_read(&g_cc[i]);
        w2 = GetTickCount64(); p2 = presents;
        fprintf(stderr, "[scan] clock: %zu grew 4..80 by present %llu\n", g_ncc, (unsigned long long)presents);
        stage = 2;
    } else if (stage == 2 && getenv("SW_SCAN_SWITCH") && atoi(getenv("SW_SCAN_SWITCH")) > 0) {
        /* SW_SCAN_SWITCH=<fps>: two spans at the start rate keep the steady clocks (one-off changes of the load drop
         * out), then a third span at <fps> (same run, same addresses): logs/scan_clock.txt lists the clocks that
         * slowed down or stopped there - a step that breaks above 60 fps */
        static double *r1;
        static ULONGLONG w3;
        if (!r1) {
            w3 = GetTickCount64();
            double s1 = (w2 - w1) / 1000.0, s2 = (w3 - w2) / 1000.0;
            size_t k = 0;
            r1 = malloc((g_ncc + 1) * sizeof *r1);
            __try {   /* the game frees memory meanwhile: a candidate can be unmapped by now (crashed the scan) */
                for (size_t i = 0; i < g_ncc; i++) {
                    double v3 = clock_read(&g_cc[i]), a = (v2[i] - g_cc[i].v) / s1, b = (v3 - v2[i]) / s2;
                    if (a > 0 ? (b > 0.9 * a && b < 1.1 * a) : (b < 0.9 * a && b > 1.1 * a)) {
                        g_cc[k] = g_cc[i]; g_cc[k].v = v3; r1[k] = b; k++;
                    }
                }
            } __except (1) { }
            g_ncc = k;
            int fps = atoi(getenv("SW_SCAN_SWITCH"));
            sw_fps_set(fps);
            fprintf(stderr, "[scan] clock: %zu steady at the start rate; frame rate now %d\n", g_ncc, fps);
            stage = 2;   /* again next span: the report below */
            at += span;  /* the third span starts now */
            return;
        }
        ULONGLONG w4 = GetTickCount64();
        double s3 = (w4 - w3) / 1000.0;
        FILE *f = fopen("logs/scan_clock.txt", "w");
        size_t k = 0;
        if (f) fprintf(f, "steady clocks that slowed down after the switch (addr type value rate-before rate-after per s)\n");
        __try {
            for (size_t i = 0; i < g_ncc; i++) {
                double v4 = clock_read(&g_cc[i]), b = (v4 - g_cc[i].v) / s3, a = r1[i];
                if (a > 0 ? b < 0.75 * a : b > 0.75 * a) {
                    k++;
                    if (f) fprintf(f, "0x%llx %s %.4f %.4f %.4f\n", (unsigned long long)g_cc[i].a, g_cc[i].dbl ? "f64" : "f32", v4, a, b);
                }
            }
        } __except (1) { }
        if (f) fclose(f);
        fprintf(stderr, "[scan] clock: %zu of %zu steady clocks slowed down after the switch -> logs/scan_clock.txt\n", k, g_ncc);
        stage = 3;
    } else if (stage == 2) {
        FILE *f = fopen("logs/scan_clock.txt", "w");
        ULONGLONG w3 = GetTickCount64();
        if (f) fprintf(f, "wall seconds: %.3f, %.3f; presents %llu->%llu->%llu (a clock in game seconds grows like the "
                       "wall seconds; one counted in 1/30 s frames grows like presents/30)\n",
                       (w2 - w1) / 1000.0, (w3 - w2) / 1000.0, (unsigned long long)p1, (unsigned long long)p2,
                       (unsigned long long)presents);
        fprintf(stderr, "[scan] wall seconds %.3f, %.3f\n", (w2 - w1) / 1000.0, (w3 - w2) / 1000.0);
        size_t k = 0;
        const char *swe = getenv("SW_SCAN_SWITCH");
        int slowed = swe && atoi(swe) > 0;
        double w1s = (w2 - w1) / 1000.0, w2s = (w3 - w2) / 1000.0;
        for (size_t i = 0; i < g_ncc; i++) {
            double v3 = clock_read(&g_cc[i]), d1 = v2[i] - g_cc[i].v, d2 = v3 - v2[i];
            double r1 = d1 / w1s, r2 = d2 / w2s;   /* per wall second */
            int keep = slowed ? (r1 > 0 ? r2 < 0.75 * r1 && r2 > -0.1 * r1 : r2 > 0.75 * r1 && r2 < -0.1 * r1)
                              : (d1 > 0 ? (d2 > 0.9 * d1 && d2 < 1.1 * d1) : (d2 < 0.9 * d1 && d2 > 1.1 * d1));
            if (keep) {
                k++;
                if (f) fprintf(f, "0x%llx %s %.4f %.4f %.4f  +%.3f +%.3f\n", (unsigned long long)g_cc[i].a, g_cc[i].dbl ? "f64" : "f32",
                               g_cc[i].v, v2[i], v3, d1, d2);
            }
        }
        if (f) fclose(f);
        fprintf(stderr, "[scan] clock: %zu steady clocks -> logs/scan_clock.txt\n", k);
        stage = 3;
    }
}
#endif

/* SW_BISECT=1: find the game's frame step inside one run.  Present 1800: all guest
 * f32 == 1/30 (candidates) and all f32 clocks; 1800..2100 keep the clocks growing
 * 10.0 (+-0.05) per 300 frames.  Then rounds of 150 frames: 1/60 written into a
 * half of the candidates every present; the clock growing ~2.5 instead of ~5.0
 * keeps that half, else the other half (whole set first).  Values are restored
 * after each round.  Log lines [bisect]. */
#if defined(_WIN32)
static void bisect_step(u64 presents)
{
    static int on = -1, stage;
    static u64 *cand; static size_t n, lo, hi, mid;   /* current set cand[lo..hi) ; testing [lo..mid) */
    static u64 clk[64]; static float clk0[64]; static int nclk;
    static u64 round_start; static float c_start;
    static const u32 STEP30 = 0x3d088889u;            /* 1/30 */
    const float s60 = 1.0f / 60.0f;
    if (on < 0) { const char *e = getenv("SW_BISECT"); on = e ? atoi(e) : 0; }
    if (!on || stage < 0) return;
    __try {
        if (stage == 0 && presents >= 1800) {
            scan_region(0x100000ull, 0x6200000ull, STEP30, 1);
            scan_region(0x80000000ull, 0xA0000000ull, STEP30, 1);
            scan_region(0x1000000000ull, 0x8000000000ull, STEP30, 1);
            cand = g_scan; n = g_nscan; g_scan = NULL; g_nscan = g_capscan = 0;
            if (on == 3) { extern void sw_multi_watch(u64 *, size_t, int); sw_multi_watch(cand, n, 1); round_start = presents; stage = 10; return; }
            clock_snap();
            fprintf(stderr, "[bisect] %zu step candidates, %zu clock candidates\n", n, g_ncc);
            stage = 1;
        } else if (stage == 1 && presents >= 2100) {
            for (size_t i = 0; i < g_ncc && nclk < 64; i++) {
                if (g_cc[i].dbl) continue;
                double d = clock_read(&g_cc[i]) - g_cc[i].v;
                if (d > 9.95 && d < 10.05) { clk[nclk] = g_cc[i].a; clk0[nclk] = *(float *)(uintptr_t)clk[nclk]; nclk++; }
            }
            free(g_cc); g_cc = NULL; g_ncc = g_capcc = 0;
            fprintf(stderr, "[bisect] %d clocks at 1/30 per frame (first 0x%llx)\n", nclk, nclk ? (unsigned long long)clk[0] : 0ull);
            if (!nclk) { stage = -1; return; }
            if (on == 2) {   /* the highest-address clock (0x18.. objects: not the shader-parameter copies) */
                extern void sw_watch_writes(u64);
                for (int i = 0; i < nclk; i++) fprintf(stderr, "[bisect] clock 0x%llx\n", (unsigned long long)clk[i]);
                u64 best = clk[0];
                for (int i = 1; i < nclk; i++) if (clk[i] > best) best = clk[i];
                const char *pk = getenv("SW_WATCHW_PICK");   /* SW_WATCHW_PICK=n: watch clock n of the list, no copy following */
                if (pk && atoi(pk) >= 0 && atoi(pk) < nclk) best = clk[atoi(pk)];
                else { extern void sw_follow_copies(void); sw_follow_copies(); }
                sw_watch_writes(best); stage = -1; return;
            }
            lo = 0; hi = n; mid = n;                    /* first round: the whole set */
            round_start = presents; c_start = *(float *)(uintptr_t)clk[0];
            stage = 2;
        } else if (stage == 2) {
            for (size_t i = lo; i < mid; i++) *(volatile float *)(uintptr_t)cand[i] = s60;
            if (presents - round_start >= 150) {
                float grow = *(float *)(uintptr_t)clk[0] - c_start;
                for (size_t i = lo; i < mid; i++) *(volatile u32 *)(uintptr_t)cand[i] = STEP30;
                int slow = grow < 3.75f;
                fprintf(stderr, "[bisect] [%zu..%zu) of [%zu..%zu): clock +%.3f in 150 frames -> %s\n", lo, mid, lo, hi, grow,
                        slow ? "STEP INSIDE" : "not inside");
                if (mid == hi && !slow) { fprintf(stderr, "[bisect] the step is not one of the 1/30 floats\n"); stage = -1; return; }
                if (slow) hi = mid; else lo = mid;
                if (hi - lo <= 1) {
                    fprintf(stderr, "[bisect] FOUND frame step at 0x%llx\n", (unsigned long long)cand[lo]);
                    stage = -1; return;
                }
                mid = lo + (hi - lo) / 2;
                round_start = presents + 30;           /* 30 frames to settle */
                stage = 3;
            }
        } else if (stage == 10 && presents >= round_start + 60) {
            extern void sw_multi_watch(u64 *, size_t, int);
            sw_multi_watch(NULL, 0, 0);
            stage = -1;
        } else if (stage == 3 && presents >= round_start) {
            c_start = *(float *)(uintptr_t)clk[0];
            stage = 2;
        }
    } __except (1) { fprintf(stderr, "[bisect] memory fault, stopping\n"); stage = -1; }
}
#endif

/* SW_BISECT=2: instead of bisecting, catch the code that writes the first clock:
 * the page is made read-only; each write faults, is logged (host RIP -> guest
 * function via sw_functions), the page is opened for one instruction (trap flag)
 * and closed again.  Stops after 400 hits on the clock itself. */
#if defined(_WIN32)
static volatile u64 g_ww_addr, g_ww_page;
static volatile int g_ww_read;               /* SW_WATCHR: reads too (page no-access) */
static volatile long g_ww_hits;
static __declspec(thread) int t_ww_pending;
static u64 g_ww_rip[64]; static volatile long g_ww_cnt[64];
static u64 guest_fn_of_host(u64 rip, u64 *host_fn)
{
    u64 best = 0, bestg = 0;
    for (u32 i = 0; i < sw_function_count; i++) {
        u64 f = (u64)(uintptr_t)sw_functions[i].fn;
        if (f <= rip && f > best) { best = f; bestg = sw_functions[i].addr; }
    }
    if (host_fn) *host_fn = best;
    return bestg;
}
static LONG CALLBACK ww_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD old;
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && t_ww_pending) {
        t_ww_pending = 0;
        if (g_ww_page) VirtualProtect((void *)(uintptr_t)g_ww_page, 4096, g_ww_read ? PAGE_NOACCESS : PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 ||
        (er->ExceptionInformation[0] != 1 && !(g_ww_read && er->ExceptionInformation[0] == 0)))
        return EXCEPTION_CONTINUE_SEARCH;
    u64 a = (u64)er->ExceptionInformation[1];
    if (!g_ww_page || (a & ~0xFFFull) != g_ww_page) return EXCEPTION_CONTINUE_SEARCH;
    if (a + 8 > g_ww_addr && a < g_ww_addr + 4) {
        u64 rip = ep->ContextRecord->Rip;
        if (g_ww_hits < 24) {   /* the first accesses at once (a watch over a range read only a few times) */
            u64 hf; u64 gfn = guest_fn_of_host(rip, &hf);
            fprintf(stderr, "[watchw] %s 0x%llx by guest fn 0x%llx (host +0x%llx)\n",
                    er->ExceptionInformation[0] == 1 ? "write to" : "read of", (unsigned long long)a,
                    (unsigned long long)gfn, (unsigned long long)(rip - hf));
        }
        for (int i = 0; i < 64; i++) {
            if (g_ww_rip[i] == rip) { InterlockedIncrement(&g_ww_cnt[i]); break; }
            if (!g_ww_rip[i]) { g_ww_rip[i] = rip; InterlockedIncrement(&g_ww_cnt[i]); break; }
        }
        if (InterlockedIncrement(&g_ww_hits) >= 400) {
            u64 pg = g_ww_page; g_ww_page = 0;
            VirtualProtect((void *)(uintptr_t)pg, 4096, PAGE_READWRITE, &old);
            for (int i = 0; i < 64 && g_ww_rip[i]; i++) {
                u64 hf; u64 g = guest_fn_of_host(g_ww_rip[i], &hf);
                fprintf(stderr, g_ww_read ? "[watchw] access to 0x%llx: host rip" : "[watchw] write to 0x%llx: host rip 0x%llx (+0x%llx into host fn of guest 0x%llx) x%ld\n",
                        (unsigned long long)g_ww_addr, (unsigned long long)g_ww_rip[i],
                        (unsigned long long)(g_ww_rip[i] - hf), (unsigned long long)g, g_ww_cnt[i]);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    VirtualProtect((void *)(uintptr_t)g_ww_page, 4096, PAGE_READWRITE, &old);
    t_ww_pending = 1;
    ep->ContextRecord->EFlags |= 0x100;             /* trap flag: re-protect after this instruction */
    return EXCEPTION_CONTINUE_EXECUTION;
}
void sw_watch_writes(u64 addr)
{
    DWORD old;
    static int added;
    if (!added) { added = 1; AddVectoredExceptionHandler(1, ww_handler); }
    g_ww_addr = addr;
    g_ww_page = addr & ~0xFFFull;
    VirtualProtect((void *)(uintptr_t)g_ww_page, 4096, PAGE_READONLY, &old);
    fprintf(stderr, "[watchw] watching writes to 0x%llx\n", (unsigned long long)addr);
}
void sw_watch_access(u64 addr)
{
    DWORD old;
    static int added;
    if (!added) { added = 1; AddVectoredExceptionHandler(1, ww_handler); }
    g_ww_read = 1;
    g_ww_addr = addr;
    g_ww_page = addr & ~0xFFFull;
    VirtualProtect((void *)(uintptr_t)g_ww_page, 4096, PAGE_NOACCESS, &old);
    fprintf(stderr, "[watchw] watching reads and writes of 0x%llx\n", (unsigned long long)addr);
}
#endif

/* With SW_BISECT=2: memcpy (sdk 0x494acf8) copies that end on the watched clock
 * move the watch to their source (up to 4 times), so the watch walks back from
 * copies to the code that adds the step. */
#if defined(_WIN32)
static void (*g_memcpy_orig)(SwCpu *c);
static volatile long g_follow_n;
static void memcpy_wrap(SwCpu *c)
{
    u64 w = g_ww_addr, dst = c->x[0], src = c->x[1], n = c->x[2];
    if (w && g_ww_page && w >= dst && w < dst + n && g_follow_n < 4) {
        u64 s = src + (w - dst);
        long k = InterlockedIncrement(&g_follow_n);
        fprintf(stderr, "[watchw] copy #%ld: memcpy(dst 0x%llx, src 0x%llx, %llu) from lr 0x%llx -> watching source 0x%llx\n",
                k, (unsigned long long)dst, (unsigned long long)src, (unsigned long long)n, (unsigned long long)c->x[30],
                (unsigned long long)s);
        DWORD old;
        u64 pg = g_ww_page;
        g_ww_page = 0;
        VirtualProtect((void *)(uintptr_t)pg, 4096, PAGE_READWRITE, &old);
        g_memcpy_orig(c);
        g_ww_hits = 0;
        memset((void *)g_ww_rip, 0, sizeof g_ww_rip);
        for (int i = 0; i < 64; i++) g_ww_cnt[i] = 0;
        sw_watch_writes(s);
        return;
    }
    g_memcpy_orig(c);
}
void sw_follow_copies(void)
{
    if (!g_memcpy_orig && install(0x494acf8ull, memcpy_wrap, &g_memcpy_orig))
        fprintf(stderr, "[watchw] following memcpy copies\n");
}
#endif

/* SW_BISECT=3: which 1/30 floats are rewritten every frame, and by whom.  The
 * pages of all candidates are read-only for 60 frames; each write to a candidate
 * is counted per (candidate, host rip), then the report lists them with the guest
 * function (rewritten each frame = a freshly computed frame step). */
#if defined(_WIN32)
static u64 *g_mw_cand; static size_t g_mw_n;
static u64 *g_mw_pages; static size_t g_mw_np;
static volatile long g_mw_on;
typedef struct { u64 cand, rip; volatile long n; } MwHit;
static MwHit g_mw_hits[512];
static __declspec(thread) u64 t_mw_reprotect;
static int u64_find(const u64 *a, size_t n, u64 v)
{
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (a[m] < v) lo = m + 1; else hi = m; }
    return lo < n && a[lo] == v;
}
static int u64_cmp(const void *x, const void *y) { u64 a = *(const u64 *)x, b = *(const u64 *)y; return a < b ? -1 : a > b; }
static LONG CALLBACK mw_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD old;
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && t_mw_reprotect) {
        if (g_mw_on) VirtualProtect((void *)(uintptr_t)t_mw_reprotect, 4096, PAGE_READONLY, &old);
        t_mw_reprotect = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    u64 a = (u64)er->ExceptionInformation[1], pg = a & ~0xFFFull;
    if (!u64_find(g_mw_pages, g_mw_np, pg)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_mw_on) {
        for (u64 c4 = a & ~3ull; c4 < a + 16 && c4 < pg + 4096; c4 += 4) {    /* stores up to 16 bytes */
            if (c4 + 4 <= a || !u64_find(g_mw_cand, g_mw_n, c4)) continue;
            u64 rip = ep->ContextRecord->Rip;
            for (int i = 0; i < 512; i++) {
                if (g_mw_hits[i].cand == c4 && g_mw_hits[i].rip == rip) { InterlockedIncrement(&g_mw_hits[i].n); break; }
                if (!g_mw_hits[i].cand) { g_mw_hits[i].cand = c4; g_mw_hits[i].rip = rip; InterlockedIncrement(&g_mw_hits[i].n); break; }
            }
        }
    }
    VirtualProtect((void *)(uintptr_t)pg, 4096, PAGE_READWRITE, &old);
    if (g_mw_on) { t_mw_reprotect = pg; ep->ContextRecord->EFlags |= 0x100; }
    return EXCEPTION_CONTINUE_EXECUTION;
}
void sw_multi_watch(u64 *cand, size_t n, int start)
{
    DWORD old;
    if (start) {
        qsort(cand, n, 8, u64_cmp);
        g_mw_cand = cand; g_mw_n = n;
        g_mw_pages = malloc(n * 8);
        for (size_t i = 0; i < n; i++) g_mw_pages[i] = cand[i] & ~0xFFFull;
        qsort(g_mw_pages, n, 8, u64_cmp);
        size_t k = 0;
        for (size_t i = 0; i < n; i++) if (!k || g_mw_pages[k - 1] != g_mw_pages[i]) g_mw_pages[k++] = g_mw_pages[i];
        g_mw_np = k;
        AddVectoredExceptionHandler(1, mw_handler);
        g_mw_on = 1;
        for (size_t i = 0; i < g_mw_np; i++) VirtualProtect((void *)(uintptr_t)g_mw_pages[i], 4096, PAGE_READONLY, &old);
        fprintf(stderr, "[mwatch] %zu candidates on %zu pages read-only\n", n, g_mw_np);
        return;
    }
    g_mw_on = 0;
    for (size_t i = 0; i < g_mw_np; i++) VirtualProtect((void *)(uintptr_t)g_mw_pages[i], 4096, PAGE_READWRITE, &old);
    for (int i = 0; i < 512 && g_mw_hits[i].cand; i++) {
        if (g_mw_hits[i].n < 30) continue;
        u64 hf, g = guest_fn_of_host(g_mw_hits[i].rip, &hf);
        fprintf(stderr, "[mwatch] 0x%llx written %ld times by guest fn 0x%llx (host +0x%llx) value now %g\n",
                (unsigned long long)g_mw_hits[i].cand, g_mw_hits[i].n, (unsigned long long)g,
                (unsigned long long)(g_mw_hits[i].rip - hf), *(float *)(uintptr_t)g_mw_hits[i].cand);
    }
    fprintf(stderr, "[mwatch] done\n");
}
#endif

/* Mod loader (sw_mods.c): which built-in frame rate / camera fix depends on a function or a data range, so a mod
 * that patches the same thing is reported as incompatible with the built-in FPS modes. */
const char *sw_hooks_fps_conflict(uint64_t fn)
{
    if (!build_ok()) return NULL;
    switch (fn) {
    case STEP_FN: case STEP_SETTER: return "frame step (60/90/120 fps game speed)";
    case WATER_SETTER: return "water animation speed at high fps";
    case CAM_SPEED_FN: return "camera turn speed at high fps";
    case CAM_AUTO_FN: return "camera auto-turn at high fps";
    }
    return NULL;
}
const char *sw_hooks_fps_data_conflict(uint64_t lo, uint64_t hi)
{
    /* rodata 0x3a3d3a0: frame interval table (i64 ns: 1/60, 1/30, 1/20, 1/15, 1/12 s), see STEP_FN above */
    if (build_ok() && lo < 0x3a3d3a0ull + 5 * 8 && hi > 0x3a3d3a0ull) return "the frame interval table (main 0x3a3d3a0)";
    return NULL;
}
