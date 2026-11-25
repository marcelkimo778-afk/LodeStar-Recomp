/* sw_hid.c - nn::hid controller functions answered on the host.
 *
 * The game's imports of nn::hid::GetNpadState / GetNpadStyleSet / ... are
 * bound to these functions at relocation (sw_hid_override), the way
 * totk-pc-recomp's runtime/ov_input.c (MIT) does it, instead of emulating the
 * HID shared memory.  One controller is connected: a full-key controller as
 * player 1 (id 0), fed by an XInput gamepad (sw_input.c) merged with the
 * keyboard while the game window has the focus.  State layout follows libnx
 * (HidNpadCommonState, 40 bytes).
 *
 * Keyboard (Switch buttons):
 *   A = Enter / Space / K      B = Backspace / L     X = I      Y = J
 *   L = Q    R = E    ZL = 1   ZR = 3   + = Tab / P  - = M
 *   left stick = W A S D       d-pad = arrow keys    right stick = numpad 8 4 5 6
 * SW_INPUT=none reports nothing pressed.
 */
#include "sw_rt.h"
#include "sw_menu.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#if defined(_WIN32)
#include <windows.h>
#endif


/* libnx HidNpadButton bits */
enum {
    NB_A = 1u << 0, NB_B = 1u << 1, NB_X = 1u << 2, NB_Y = 1u << 3,
    NB_STICKL = 1u << 4, NB_STICKR = 1u << 5, NB_L = 1u << 6, NB_R = 1u << 7,
    NB_ZL = 1u << 8, NB_ZR = 1u << 9, NB_PLUS = 1u << 10, NB_MINUS = 1u << 11,
    NB_LEFT = 1u << 12, NB_UP = 1u << 13, NB_RIGHT = 1u << 14, NB_DOWN = 1u << 15,
    NB_SL_LEFT = 1u << 16, NB_SL_UP = 1u << 17, NB_SL_RIGHT = 1u << 18, NB_SL_DOWN = 1u << 19,
    NB_SR_LEFT = 1u << 20, NB_SR_UP = 1u << 21, NB_SR_RIGHT = 1u << 22, NB_SR_DOWN = 1u << 23,
};
enum { STYLE_FULL_KEY = 1, ATTR_CONNECTED = 1, STICK_MAX = 32767, STICK_DIR = 16384 };
volatile float sw_force_rx;   /* debug: right stick x forced by SW_FIND_RATE (sw_hooks.c), 0 = off */

typedef struct { int64_t sampling; u64 buttons; int32_t lx, ly, rx, ry; u32 attributes, pad; } NpadState;

static int64_t g_sample;
static int g_disabled = -1;

#if defined(_WIN32)
static int window_focused(void)
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

/* Keyboard bindings: action -> up to 4 Windows virtual-key codes.  Defaults
 * below; SW_KEYMAP=<file> overrides them with lines `NAME=vk,vk,...`
 * (decimal codes, `NAME=` alone unbinds).  The launcher's Controls window
 * writes that file.  Names must match launcher/controls.py. */
enum { K_A, K_B, K_X, K_Y, K_L, K_R, K_ZL, K_ZR, K_PLUS, K_MINUS, K_L3, K_R3,
       K_UP, K_DOWN, K_LEFT, K_RIGHT, K_LS_LEFT, K_LS_RIGHT, K_LS_UP, K_LS_DOWN,
       K_RS_LEFT, K_RS_RIGHT, K_RS_UP, K_RS_DOWN, K_COUNT };
static const char *const k_names[K_COUNT] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LSTICK", "RSTICK",
    "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "LS_LEFT", "LS_RIGHT", "LS_UP", "LS_DOWN",
    "RS_LEFT", "RS_RIGHT", "RS_UP", "RS_DOWN"};
static int k_map[K_COUNT][4] = {
    {VK_RETURN, VK_SPACE, 'K'}, {VK_BACK, 'L'}, {'I'}, {'J'}, {'Q'}, {'E'}, {'1'}, {'3'},
    {VK_TAB, 'P'}, {'M'}, {VK_LSHIFT}, {'R'},
    {VK_UP}, {VK_DOWN}, {VK_LEFT}, {VK_RIGHT}, {'A'}, {'D'}, {'W'}, {'S'},
    {VK_NUMPAD4}, {VK_NUMPAD6}, {VK_NUMPAD8}, {VK_NUMPAD5, VK_NUMPAD2}};

static void keymap_load(void)
{
    static int done;
    if (done) return;
    done = 1;
    const char *path = getenv("SW_KEYMAP");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[hid] SW_KEYMAP: cannot open %s, default keys\n", path); return; }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        for (int a = 0; a < K_COUNT; a++) {
            if (strcmp(line, k_names[a])) continue;
            memset(k_map[a], 0, sizeof k_map[a]);
            char *p = eq + 1;
            for (int i = 0; i < 4 && *p;) {
                char *end;
                long v = strtol(p, &end, 10);
                if (end == p) break;
                if (v > 0 && v < 256) k_map[a][i++] = (int)v;
                p = *end == ',' ? end + 1 : end;
            }
            n++;
        }
    }
    fclose(f);
    fprintf(stderr, "[hid] key bindings from %s (%d actions)\n", path, n);
}

static int down(int action)
{
    for (int i = 0; i < 4; i++)
        if (k_map[action][i] && key(k_map[action][i])) return 1;
    return 0;
}

/* Mouse camera (SW_MOUSE_CAMERA=1, set by the launcher's Keyboard / Mouse tab):
 * while the game window has the focus the cursor is hidden, held in the middle
 * of the window and its movement drives the right stick.  SW_MOUSE_SENS
 * (0.1..10, default 1) scales it, SW_MOUSE_INVERT_Y=1 flips up/down.  F2
 * frees / captures the cursor again.  Mouse buttons are bound like keys
 * (VK_LBUTTON=1, VK_RBUTTON=2, VK_MBUTTON=4, X1=5, X2=6) in SW_KEYMAP. */
static int g_mouse_on = -1, g_mouse_inv, g_mouse_captured = 1, g_mouse_active;
static double g_mouse_sens = 1.0, g_mouse_vx, g_mouse_vy;
static HCURSOR g_mouse_blank, g_mouse_old;
static HWND g_mouse_wnd;
static LARGE_INTEGER g_mouse_t, g_mouse_fq;

static void mouse_release(void)
{
    if (!g_mouse_active) return;
    g_mouse_active = 0;
    ClipCursor(NULL);
    if (g_mouse_wnd && g_mouse_old) SetClassLongPtrA(g_mouse_wnd, GCLP_HCURSOR, (LONG_PTR)g_mouse_old);
    g_mouse_vx = g_mouse_vy = 0;
}

static void mouse_look(NpadState *s, int focused)
{
    if (g_mouse_on < 0) {
        const char *e = getenv("SW_MOUSE_CAMERA");
        g_mouse_on = e && *e == '1';
        e = getenv("SW_MOUSE_SENS");
        if (e && atof(e) > 0) g_mouse_sens = atof(e);
        e = getenv("SW_MOUSE_INVERT_Y");
        g_mouse_inv = e && *e == '1';
        QueryPerformanceFrequency(&g_mouse_fq);
        if (g_mouse_on) {
            BYTE andm[128], xorm[128];
            memset(andm, 0xFF, sizeof andm); memset(xorm, 0, sizeof xorm);
            g_mouse_blank = CreateCursor(GetModuleHandleA(NULL), 0, 0, 32, 32, andm, xorm);
        }
    }
    if (!g_mouse_on) return;
    if (!focused) { mouse_release(); return; }
    {   /* F2: free / capture the cursor */
        static int prev;
        int f2 = key(VK_F2);
        if (f2 && !prev) { g_mouse_captured = !g_mouse_captured; if (!g_mouse_captured) mouse_release(); }
        prev = f2;
    }
    if (!g_mouse_captured) return;
    HWND w = GetForegroundWindow();
    RECT rc;
    if (!w || !GetClientRect(w, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    POINT ctr = {rc.right / 2, rc.bottom / 2}, tl = {0, 0}, br = {rc.right, rc.bottom};
    ClientToScreen(w, &ctr); ClientToScreen(w, &tl); ClientToScreen(w, &br);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!g_mouse_active || w != g_mouse_wnd) {
        /* (re)capture: hide the cursor, keep it inside the window, no jump */
        if (g_mouse_active) mouse_release();
        g_mouse_wnd = w;
        g_mouse_old = (HCURSOR)GetClassLongPtrA(w, GCLP_HCURSOR);
        if (g_mouse_blank) { SetClassLongPtrA(w, GCLP_HCURSOR, (LONG_PTR)g_mouse_blank); SetCursor(g_mouse_blank); }
        RECT clip = {tl.x, tl.y, br.x, br.y};
        ClipCursor(&clip);
        SetCursorPos(ctr.x, ctr.y);
        g_mouse_active = 1;
        g_mouse_t = now;
        return;
    }
    POINT p;
    if (!GetCursorPos(&p)) return;
    double dt = (double)(now.QuadPart - g_mouse_t.QuadPart) / (double)g_mouse_fq.QuadPart;
    g_mouse_t = now;
    int dx = p.x - ctr.x, dy = p.y - ctr.y;
    if (dx || dy) SetCursorPos(ctr.x, ctr.y);
    if (dt < 0.002) dt = 0.002;
    if (dt > 0.1) dt = 0.1;
    /* pixels per second -> stick: 1200 px/s is a full deflection at sens 1;
     * light smoothing so polls between mouse reports do not flicker to 0 */
    double tx = dx / dt / 1200.0 * g_mouse_sens, ty = -dy / dt / 1200.0 * g_mouse_sens;
    if (g_mouse_inv) ty = -ty;
    g_mouse_vx = g_mouse_vx * 0.35 + tx * 0.65;
    g_mouse_vy = g_mouse_vy * 0.35 + ty * 0.65;
    double vx = g_mouse_vx > 1 ? 1 : g_mouse_vx < -1 ? -1 : g_mouse_vx;
    double vy = g_mouse_vy > 1 ? 1 : g_mouse_vy < -1 ? -1 : g_mouse_vy;
    int32_t mx = (int32_t)(vx * STICK_MAX), my = (int32_t)(vy * STICK_MAX);
    if (abs(mx) > abs(s->rx)) s->rx = mx;
    if (abs(my) > abs(s->ry)) s->ry = my;
}
#endif

static void stick_dirs(u64 *b, int32_t x, int32_t y, u64 left, u64 up, u64 right, u64 down)
{
    if (x < -STICK_DIR) *b |= left;
    if (x > STICK_DIR) *b |= right;
    if (y > STICK_DIR) *b |= up;
    if (y < -STICK_DIR) *b |= down;
}

volatile uint64_t sw_hid_buttons_held;
static void announce(void);
/* ---- benchmark routes: record real play once, replay it in every test (2026-11-16) -----------------------------
 * F9 in the game window starts a recording: build_work/out/routes/route_<date>_<time>/ gets a copy of the save files
 * (save in the game first: that is the route's start) and input.txt, one line per input change "t_ms buttons lx ly rx
 * ry" (the merged controller + keyboard + mouse state, before the stick-direction bits).  F9 again stops it.
 * SW_INPUT_REPLAY=<route folder or input.txt> replays it: from SW_INPUT_REPLAY_AT seconds after start-up (default 60:
 * the bench gets into the world with A presses first) the input is the recording's, by time; afterwards neutral. */
typedef struct { u32 t; u64 b; int32_t lx, ly, rx, ry; } RouteEv;
static FILE *g_rec;
static LARGE_INTEGER g_rec_t0, g_route_fq;
static RouteEv g_rec_last;
static RouteEv *g_rep;
static size_t g_nrep, g_rep_i;
static int g_rep_state = -1;                  /* -1 not loaded, 0 off, 1 waiting, 2 playing, 3 done */
static double g_rep_at = 1.5;               /* seconds after the first field-camera frame */
static LARGE_INTEGER g_rep_t0;

static void route_copy(const char *from_dir, const char *to_dir, const char *name)
{
    char a[MAX_PATH], b[MAX_PATH];
    snprintf(a, sizeof a, "%s\\%s", from_dir, name);
    snprintf(b, sizeof b, "%s\\%s", to_dir, name);
    CopyFileA(a, b, FALSE);
}

static void route_load(void)
{
    const char *e = getenv("SW_INPUT_REPLAY");
    g_rep_state = 0;
    if (!e || !*e) return;
    const char *at = getenv("SW_INPUT_REPLAY_AT");
    if (at && atof(at) >= 0) g_rep_at = atof(at);
    char path[MAX_PATH];
    DWORD attr = GetFileAttributesA(e);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) snprintf(path, sizeof path, "%s\\input.txt", e);
    else snprintf(path, sizeof path, "%s", e);
    FILE *f = fopen(path, "r");
    if (!f) { sw_report("[route] SW_INPUT_REPLAY: cannot open %s\n", path); return; }
    size_t cap = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        RouteEv ev; unsigned long long bb;
        if (line[0] == '#' || sscanf(line, "%u %llx %d %d %d %d", &ev.t, &bb, &ev.lx, &ev.ly, &ev.rx, &ev.ry) != 6) continue;
        ev.b = bb;
        if (g_nrep == cap) { cap = cap ? cap * 2 : 4096; g_rep = (RouteEv *)realloc(g_rep, cap * sizeof *g_rep); }
        g_rep[g_nrep++] = ev;
    }
    fclose(f);
    QueryPerformanceFrequency(&g_route_fq);
    QueryPerformanceCounter(&g_rep_t0);
    g_rep_state = g_nrep ? 1 : 0;
    sw_report("[route] replaying %s: %zu input changes, %.1f s, from %.0f s after start-up\n", path, g_nrep,
              g_nrep ? g_rep[g_nrep - 1].t / 1000.0 : 0.0, g_rep_at);
}

void sw_route_io(NpadState *s, u64 *b, int focused, int scripted)
{
    if (g_rep_state < 0) route_load();
    if (!g_route_fq.QuadPart) QueryPerformanceFrequency(&g_route_fq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    {   /* SW_INPUT_LIVE=<file> (test tools that watch the screen, e.g. press A once a battle starts): the file holds
         * "buttons(hex) lx ly rx ry ms"; that input replaces every other one (a route too) for ms milliseconds from
         * when it is read, and the file is removed.  Read every 4th poll. */
        static const char *live = (const char *)1;
        static LARGE_INTEGER until;
        static u64 lb;
        static int llx, lly, lrx, lry;
        static unsigned polls;
        if (live == (const char *)1) live = getenv("SW_INPUT_LIVE");
        if (live && *live) {
            if ((++polls & 3) == 0) {
                FILE *f = fopen(live, "rb");
                if (f) {
                    unsigned long long bb;
                    int x, y, z, w;
                    unsigned ms;
                    if (fscanf(f, "%llx %d %d %d %d %u", &bb, &x, &y, &z, &w, &ms) == 6) {
                        lb = bb, llx = x, lly = y, lrx = z, lry = w;
                        until.QuadPart = now.QuadPart + (LONGLONG)ms * g_route_fq.QuadPart / 1000;
                    }
                    fclose(f);
                    remove(live);
                }
            }
            if (now.QuadPart < until.QuadPart) {
                *b = lb; s->lx = llx; s->ly = lly; s->rx = lrx; s->ry = lry;
                return;
            }
        }
    }
    if (g_rep_state == 1 || g_rep_state == 2) {   /* replay: the recording replaces every other input */
        /* start: SW_INPUT_REPLAY_AT seconds (default 1.5) after the first field-camera frame (sw_hooks.c: the save is
         * loaded and the player stands in the world), so every run starts at the same moment however long the load
         * took; without that signal 150 s after start-up */
        extern volatile long long sw_field_since;
        if (g_rep_state == 1) {
            double up = (double)(now.QuadPart - g_rep_t0.QuadPart) / (double)g_route_fq.QuadPart;
            double since = sw_field_since ? (double)(now.QuadPart - sw_field_since) / (double)g_route_fq.QuadPart : -1;
            if (!(since >= g_rep_at || up >= 150)) return;
            g_rep_t0 = now;
            g_rep_state = 2;
            sw_report("[route] replay started (%s)\n", since >= 0 ? "in the world" : "no field signal: 150 s");
            {   /* SW_PROFILE_ROUTE=<s> (with SW_PROFILE=all): profile only the first s seconds of the replay */
                const char *pr = getenv("SW_PROFILE_ROUTE");
                if (pr && atof(pr) > 0) { extern void sw_prof_window_now(double); sw_prof_window_now(atof(pr)); }
            }
        }
        double t = (double)(now.QuadPart - g_rep_t0.QuadPart) / (double)g_route_fq.QuadPart;
        u32 ms = (u32)(t * 1000.0);
        while (g_rep_i + 1 < g_nrep && g_rep[g_rep_i + 1].t <= ms) g_rep_i++;
        if (g_rep_i + 1 >= g_nrep && ms > g_rep[g_nrep - 1].t + 500) {
            g_rep_state = 3;
            sw_report("[route] replay finished\n");
            { extern void sw_pgo_write(void); sw_pgo_write(); }   /* PGO-instrumented build: write the profile now */
            {   /* SW_INPUT_REPLAY_EXIT=1 (bench.py ROUTE=): the test ends with the route - close the game cleanly */
                const char *x = getenv("SW_INPUT_REPLAY_EXIT");
                if (x && *x == '1') { extern void gfx_window_request_close(void); gfx_window_request_close(); }
            }
            *b = 0; s->lx = s->ly = s->rx = s->ry = 0;
            return;
        }
        const RouteEv *ev = &g_rep[g_rep_i];
        *b = ev->b; s->lx = ev->lx; s->ly = ev->ly; s->rx = ev->rx; s->ry = ev->ry;
        return;
    }
    if (g_rep_state == 3) { *b = 0; s->lx = s->ly = s->rx = s->ry = 0; return; }
    if (scripted) return;
    {   /* F9 (game window focused): start / stop a recording */
        static int prev;
        int f9 = focused && (GetAsyncKeyState(VK_F9) & 0x8001) != 0;   /* 0x0001: a tap between two polls counts too */
        if (f9 && !prev) {
            if (g_rec) {
                double dur = (double)(now.QuadPart - g_rec_t0.QuadPart) / (double)g_route_fq.QuadPart;
                fprintf(g_rec, "%u %llx 0 0 0 0\n", (u32)(dur * 1000.0), 0ull);
                fclose(g_rec);
                g_rec = NULL;
                sw_report("[route] recording stopped after %.1f s\n", dur);
            } else {
                SYSTEMTIME st;
                GetLocalTime(&st);
                char dir[MAX_PATH];
                CreateDirectoryA("routes", NULL);
                snprintf(dir, sizeof dir, "routes\\route_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                         st.wMinute, st.wSecond);
                CreateDirectoryA(dir, NULL);
                char sdir[MAX_PATH];
                snprintf(sdir, sizeof sdir, "%s\\save", dir);
                CreateDirectoryA(sdir, NULL);
                const char *sd = getenv("SW_SAVE_DIR");
                const char *from = sd && *sd ? sd : "save";
                route_copy(from, sdir, "main"); route_copy(from, sdir, "main2"); route_copy(from, sdir, "backup");
                char path[MAX_PATH];
                snprintf(path, sizeof path, "%s\\input.txt", dir);
                g_rec = fopen(path, "w");
                if (g_rec) {
                    fprintf(g_rec, "# benchmark route recorded %04u-%02u-%02u %02u:%02u:%02u; t_ms buttons(hex) lx ly rx ry; "
                            "the save it starts from is in save\\\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                            st.wSecond);
                    g_rec_t0 = now;
                    memset(&g_rec_last, 0xff, sizeof g_rec_last);
                    sw_report("[route] recording to %s (F9 stops)\n", dir);
                }
            }
        }
        prev = f9;
    }
    if (g_rec) {
        RouteEv ev = {(u32)((double)(now.QuadPart - g_rec_t0.QuadPart) * 1000.0 / (double)g_route_fq.QuadPart), *b & ~0ull,
                      s->lx, s->ly, s->rx, s->ry};
        if (ev.b != g_rec_last.b || ev.lx != g_rec_last.lx || ev.ly != g_rec_last.ly || ev.rx != g_rec_last.rx ||
            ev.ry != g_rec_last.ry) {
            fprintf(g_rec, "%u %llx %d %d %d %d\n", ev.t, (unsigned long long)ev.b, ev.lx, ev.ly, ev.rx, ev.ry);
            fflush(g_rec);   /* the game may exit without the stop F9: every line is on disk at once */
            g_rec_last = ev;
        }
    }
}

static void poll(NpadState *s)
{
    announce();
    memset(s, 0, sizeof *s);
    s->sampling = ++g_sample;
    s->attributes = ATTR_CONNECTED;
    if (g_disabled < 0) {
        const char *e = getenv("SW_INPUT");
        g_disabled = e && !strcmp(e, "none");
    }
    if (g_disabled) return;
    u64 b = 0;
    SwPadState p;
    static int scripted = -1;   /* SW_INPUT=script: only SW_AUTO_* input (repeatable test runs) */
    if (scripted < 0) { const char *e = getenv("SW_INPUT"); scripted = e && !strcmp(e, "script"); }
    if (!scripted && sw_input_poll(&p) && p.connected) {
        static const struct { u32 from; u64 to; } map[] = {
            {SW_BTN_A, NB_A}, {SW_BTN_B, NB_B}, {SW_BTN_X, NB_X}, {SW_BTN_Y, NB_Y},
            {SW_BTN_L, NB_L}, {SW_BTN_R, NB_R}, {SW_BTN_ZL, NB_ZL}, {SW_BTN_ZR, NB_ZR},
            {SW_BTN_PLUS, NB_PLUS}, {SW_BTN_MINUS, NB_MINUS}, {SW_BTN_L3, NB_STICKL}, {SW_BTN_R3, NB_STICKR},
            {SW_BTN_UP, NB_UP}, {SW_BTN_DOWN, NB_DOWN}, {SW_BTN_LEFT, NB_LEFT}, {SW_BTN_RIGHT, NB_RIGHT},
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (p.buttons & map[i].from) b |= map[i].to;
        s->lx = p.lx; s->ly = p.ly; s->rx = p.rx; s->ry = p.ry;
    }
#if defined(_WIN32)
    keymap_load();
    /* the overlay (mods menu / browser) needs the mouse: the mouse camera lets the cursor go while it is open */
    int focused = !scripted && window_focused() && !sw_menu_overlay_open();
    if (focused) {
        static const u64 btn[K_UP + 4] = {NB_A, NB_B, NB_X, NB_Y, NB_L, NB_R, NB_ZL, NB_ZR, NB_PLUS, NB_MINUS,
                                          NB_STICKL, NB_STICKR, NB_UP, NB_DOWN, NB_LEFT, NB_RIGHT};
        for (int a = 0; a <= K_RIGHT; a++)
            if (down(a)) b |= btn[a];
        if (down(K_LS_LEFT)) s->lx = -STICK_MAX;
        if (down(K_LS_RIGHT)) s->lx = STICK_MAX;
        if (down(K_LS_UP)) s->ly = STICK_MAX;
        if (down(K_LS_DOWN)) s->ly = -STICK_MAX;
        if (down(K_RS_LEFT)) s->rx = -STICK_MAX;
        if (down(K_RS_RIGHT)) s->rx = STICK_MAX;
        if (down(K_RS_UP)) s->ry = STICK_MAX;
        if (down(K_RS_DOWN)) s->ry = -STICK_MAX;
    }
    mouse_look(s, focused);
    {   void sw_route_io(NpadState *s, u64 *b, int focused, int scripted); sw_route_io(s, &b, focused, scripted); }
#endif
#if defined(_WIN32)
    {   /* SW_AUTO_A=start,period[,stop] (seconds since start-up): press A for
         * 150 ms once per period - scripted test runs through dialogue */
        static int init; static double t0, start, period, stop, hold = 0.15;   /* 4th value: hold seconds */
        static LARGE_INTEGER f0, fq;
        if (!init) {
            init = 1;
            const char *e = getenv("SW_AUTO_A");
            QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&f0);
            start = -1; stop = 1e30;
            if (e) { start = 0; period = 2; sscanf(e, "%lf,%lf,%lf,%lf", &start, &period, &stop, &hold); }
            (void)t0;
        }
        extern volatile long long sw_field_since;
        if (start >= 0 && period > 0 && !(g_rep_state > 0 && sw_field_since)) {   /* a route replay: no A once in the world */
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            double t = (double)(now.QuadPart - f0.QuadPart) / (double)fq.QuadPart;
            if (t >= start && t <= stop && (t - start) - period * (double)(long long)((t - start) / period) < hold) b |= NB_A;
        }
    }
    {   /* SW_AUTO_RS=start,stop[,x,y] / SW_AUTO_LS=...: hold a stick (-1..1, default right) between the times -
         * scripted camera turns / walking for test runs */
        static int init; static double rs[4] = {-1, -1, 1, 0}, ls[4] = {-1, -1, 0, 1};
        static LARGE_INTEGER f0, fq;
        if (!init) {
            init = 1;
            QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&f0);
            const char *e = getenv("SW_AUTO_RS");
            if (e) sscanf(e, "%lf,%lf,%lf,%lf", &rs[0], &rs[1], &rs[2], &rs[3]);
            e = getenv("SW_AUTO_LS");
            if (e) sscanf(e, "%lf,%lf,%lf,%lf", &ls[0], &ls[1], &ls[2], &ls[3]);
        }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - f0.QuadPart) / (double)fq.QuadPart;
        if (rs[0] >= 0 && t >= rs[0] && t <= rs[1]) { s->rx = (int32_t)(rs[2] * STICK_MAX); s->ry = (int32_t)(rs[3] * STICK_MAX); }
        if (ls[0] >= 0 && t >= ls[0] && t <= ls[1]) { s->lx = (int32_t)(ls[2] * STICK_MAX); s->ly = (int32_t)(ls[3] * STICK_MAX); }
    }
#endif
    if (sw_force_rx != 0.0f) { s->rx = (int32_t)(sw_force_rx * STICK_MAX); s->ry = 0; }   /* SW_FIND_RATE (sw_hooks.c) */
    stick_dirs(&b, s->lx, s->ly, NB_SL_LEFT, NB_SL_UP, NB_SL_RIGHT, NB_SL_DOWN);
    stick_dirs(&b, s->rx, s->ry, NB_SR_LEFT, NB_SR_UP, NB_SR_RIGHT, NB_SR_DOWN);
    {   /* in-game menu (sw_menu.c): Minus+Plus toggles it; while open the game gets no input */
        int neutral = 0;
        b = sw_menu_filter(b, &neutral);
        if (neutral) s->lx = s->ly = s->rx = s->ry = 0;
    }
    s->buttons = b;
    sw_hid_buttons_held = b;                     /* cheat VM key conditions (sw_mods.c) */
    {
        static u64 last;
        if (b != last && getenv("SW_INPUT_LOG")) sw_report("[hid] buttons 0x%llx\n", (unsigned long long)b);
        extern int sw_newfn_mode; extern void sw_newfn_apress(void);
        if (sw_newfn_mode && (b & NB_A) && !(last & NB_A)) sw_newfn_apress();   /* SW_NEWFN=1: [afn] lines */
        last = b;
    }
}

static u32 read_id(SwCpu *c, u64 p)
{
    u32 *id = p ? (u32 *)sw_mem_ptr(c, p, 4, 0) : NULL;
    return id ? *id : 0xFFFFFFFFu;
}

/* The sdk keeps its own InitializeNpad / SetSupported... / vibration set-up
 * (they run over the stubbed hid service); only the state readers are ours. */
static void announce(void)
{
    static int done;
    if (done) return;
    done = 1;
    sw_report("[hid] controllers initialised: one full-key controller as player 1 "
              "(gamepad + keyboard: Enter/Space=A Backspace=B WASD=stick arrows=d-pad Tab=+)\n");
}

/* NpadStyleSet GetNpadStyleSet(const NpadIdType &id) */
static void hid_style_set(SwCpu *c)
{
    c->x[0] = read_id(c, c->x[0]) == 0 ? STYLE_FULL_KEY : 0;
}

/* void GetNpadState(NpadXxxState *out, const NpadIdType &id) */
static void hid_get_state(SwCpu *c)
{
    NpadState *out = (NpadState *)sw_mem_ptr(c, c->x[0], sizeof(NpadState), 1);
    u32 id = read_id(c, c->x[1]);
    if (!out) return;
    if (id == 0) {
        poll(out);
    } else {
        memset(out, 0, sizeof *out);
        out->sampling = g_sample;
    }
}

/* FullKey is the only connected style; the other style readers see nothing. */
static void hid_get_state_other(SwCpu *c)
{
    NpadState *out = (NpadState *)sw_mem_ptr(c, c->x[0], sizeof(NpadState), 1);
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->sampling = g_sample;
}

/* int GetNpadStates(NpadXxxState *out, int count, const NpadIdType &id) */
static void hid_get_states(SwCpu *c)
{
    int n = (int)c->x[1];
    u32 id = read_id(c, c->x[2]);
    if (n < 1) { c->x[0] = 0; return; }
    NpadState *out = (NpadState *)sw_mem_ptr(c, c->x[0], sizeof(NpadState), 1);
    if (!out) { c->x[0] = 0; return; }
    if (id == 0) poll(out);
    else { memset(out, 0, sizeof *out); out->sampling = g_sample; }
    c->x[0] = 1;
}

static const struct { const char *name; void (*fn)(SwCpu *); } g_hid[] = {
    {"_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set},
    {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_get_state},
    {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_get_state_other},
    {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_get_state_other},
    {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_get_state_other},
    {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_get_state_other},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", hid_get_states},
};

/* ---- sdk-internal nn::hid::detail functions, replaced by address ----
 * With a controller reported connected the game sets up rumble and motion
 * for it; the sdk's detail layer talks to hid service objects our stubbed
 * service never created (null calls).  These answer instead: vibration is
 * accepted and dropped, the six-axis sensor reads a controller lying still. */
typedef struct { int64_t delta_ns, sampling; float acc[3], gyro[3], angle[3], dir[9]; u32 attributes, reserved; } MotionState;
typedef struct { float amp_low, freq_low, amp_high, freq_high; } VibrationValue;

static void ok0(SwCpu *c) { c->x[0] = 0; }

/* Result GetActualVibrationValue(VibrationValue *out, const VibrationDeviceHandle &) */
static void vib_actual(SwCpu *c)
{
    VibrationValue *v = (VibrationValue *)sw_mem_ptr(c, c->x[0], sizeof *v, 1);
    if (v) { v->amp_low = 0; v->freq_low = 160; v->amp_high = 0; v->freq_high = 320; }
    c->x[0] = 0;
}

/* Result GetVibrationDeviceInfo(VibrationDeviceInfo *out, const VibrationDeviceHandle &h) */
static void vib_info(SwCpu *c)
{
    u32 *out = (u32 *)sw_mem_ptr(c, c->x[0], 8, 1);
    u32 h = read_id(c, c->x[1]);
    if (out) { out[0] = 1; out[1] = ((h >> 16) & 1) + 1; }
    c->x[0] = 0;
}

/* Result IsVibrationDeviceMounted(bool *out, const VibrationDeviceHandle &) / IsVibrationPermitted(bool *out) */
static void bool_true(SwCpu *c)
{
    u8 *out = sw_mem_ptr(c, c->x[0], 1, 1);
    if (out) *out = 1;
    c->x[0] = 0;
}

static void six_rest(MotionState *s, int n)
{
    for (int i = 0; i < n; i++) {
        memset(&s[i], 0, sizeof s[i]);
        s[i].sampling = g_sample;
        s[i].acc[2] = -1.0f;
        s[i].dir[0] = s[i].dir[4] = s[i].dir[8] = 1.0f;
    }
}

/* Result GetSixAxisSensorStates(int *count, SixAxisSensorState *out, int max, const SixAxisSensorHandle &) */
static void six_states(SwCpu *c)
{
    int max = (int)c->x[2];
    int32_t *count = (int32_t *)sw_mem_ptr(c, c->x[0], 4, 1);
    MotionState *s = max > 0 ? (MotionState *)sw_mem_ptr(c, c->x[1], (u64)max * sizeof(MotionState), 1) : NULL;
    if (s) six_rest(s, max);
    if (count) *count = s ? max : 0;
    c->x[0] = 0;
}

/* Result GetSixAxisSensorState(SixAxisSensorState *out, const SixAxisSensorHandle &) */
static void six_state(SwCpu *c)
{
    MotionState *s = (MotionState *)sw_mem_ptr(c, c->x[0], sizeof(MotionState), 1);
    if (s) six_rest(s, 1);
    c->x[0] = 0;
}

void sw_hle_show_keyboard(SwCpu *c);   /* sw_swkbd.c */

static const struct { const char *name; void (*fn)(SwCpu *); } g_hid_detail[] = {
    {"_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", sw_hle_show_keyboard},
    {"_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", ok0},
    {"_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", ok0},
    {"_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi", ok0},
    {"_ZN2nn3hid6detail23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE", vib_actual},
    {"_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", vib_info},
    {"_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE", bool_true},
    {"_ZN2nn3hid6detail20IsVibrationPermittedEPb", bool_true},
    {"_ZN2nn3hid6detail15PermitVibrationEb", ok0},
    {"_ZN2nn3hid6detail18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", ok0},
    {"_ZN2nn3hid6detail17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", ok0},
    {"_ZN2nn3hid6detail22GetSixAxisSensorStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", six_states},
    {"_ZN2nn3hid6detail21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", six_state},
};

int sw_hid_detail_count(void)
{
    const char *e = getenv("SW_HID_HLE");
    if (e && !strcmp(e, "0")) return 0;
    return (int)(sizeof g_hid_detail / sizeof g_hid_detail[0]);
}
const char *sw_hid_detail_name(int i) { return g_hid_detail[i].name; }
void (*sw_hid_detail_fn(int i))(SwCpu *) { return g_hid_detail[i].fn; }

/* Called by the relocator for every imported symbol: returns the host
 * function that replaces it, or NULL. */
void (*sw_hid_override(const char *name))(SwCpu *)
{
    if (strncmp(name, "_ZN2nn3hid", 10) != 0) return NULL;
    {
        const char *e = getenv("SW_HID_HLE");
        if (e && !strcmp(e, "0")) return NULL;
    }
    for (size_t i = 0; i < sizeof g_hid / sizeof g_hid[0]; i++)
        if (!strcmp(g_hid[i].name, name)) return g_hid[i].fn;
    return NULL;
}
