/* sw_menu.c - in-game menu state and input.  See sw_menu.h.
 *
 * Input path: sw_hid.c poll() -> sw_menu_filter() on every npad read.
 *  - Minus+Plus going down together toggles the menu.
 *  - A lone Plus or Minus is held back from the game for HOLD_MS, because the
 *    player rarely presses both in the same poll; if the partner comes in time
 *    it is a menu combo and the game never sees the button.  A tap released
 *    inside that window is replayed to the game as a short press, so single
 *    Plus (game menu) and Minus (map) still work.
 *  - While open: d-pad / left stick choose (with repeat), left/right change,
 *    A selects, B closes; the game sees no buttons and centred sticks.
 *  - After the menu closes nothing reaches the game until every button and
 *    stick direction is released (the closing B or A must not leak). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sw_menu.h"

void sw_fps_set(int fps);   /* sw_hooks.c */
int  sw_fps_get(void);

enum {
    B_A = 1u << 0, B_B = 1u << 1, B_PLUS = 1u << 10, B_MINUS = 1u << 11,
    B_LEFT = 1u << 12, B_UP = 1u << 13, B_RIGHT = 1u << 14, B_DOWN = 1u << 15,
    B_SL_LEFT = 1u << 16, B_SL_UP = 1u << 17, B_SL_RIGHT = 1u << 18, B_SL_DOWN = 1u << 19,
};
#define HOLD_MS    200.0   /* lone Plus/Minus held back this long, waiting for the partner */
#define REPLAY_MS  100.0   /* a tap released inside the hold is replayed this long */
#define REPEAT_MS1 400.0   /* held up/down: first repeat */
#define REPEAT_MS  110.0   /* then every */

static SRWLOCK g_lock = SRWLOCK_INIT;
int sw_ingame_get(const char *key);          /* sw_romfs.c: Config/ingame_settings.json */
void sw_ingame_set(const char *key, int value);
static volatile LONG g_open, g_sel, g_quit_armed, g_action, g_counter = -1, g_fullscreen;
static volatile LONG g_ovl;                    /* the ImGui overlay is open (sw_menu_overlay_set) */
static volatile LONG g_ovl_page = SW_OVL_MENU;  /* its page: SW_OVL_MENU (the game menu) or SW_OVL_MODS */
static volatile LONG64 g_raw;                  /* buttons of the last poll, before filtering */

/* guarded by g_lock */
static uint64_t s_last;            /* buttons of the previous poll */
static int s_combo;                /* Minus+Plus held in the previous poll */
static int s_gate;                 /* menu closed: swallow input until all released */
static uint64_t s_held;            /* Plus/Minus held back from the game */
static double s_held_t0;
static uint64_t s_replay;          /* tapped while held back: replayed */
static double s_replay_until;
static double s_repeat_at;         /* next auto-repeat of a held up/down */

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void set_open_locked(int open)
{
    if (open == (int)g_open) return;
    InterlockedExchange(&g_open, open);
    InterlockedExchange(&g_quit_armed, 0);
    if (open) InterlockedExchange(&g_sel, SW_MENU_CONTINUE);
    s_gate = 1;                     /* the buttons that opened / closed it stay with the menu */
    s_held = 0; s_replay = 0;
    fprintf(stderr, "[menu] %s\n", open ? "opened" : "closed");
}

static void change_locked(int item, int dir)
{
    switch (item) {
    case SW_MENU_FPS: {   /* 30 -> 60 -> 90 -> 120 -> 30; left goes back (dir < 0) */
        static const int rates[] = {30, 60, 90, 120, 240};
        int cur = sw_fps_get(), k = 0;
        for (int i = 0; i < 5; i++) if (rates[i] == cur) k = i;
        int f = rates[(k + (dir < 0 ? 4 : 1)) % 5];
        sw_fps_set(f);
        fprintf(stderr, "[menu] frame rate %d fps\n", f);
        break;
    }
    case SW_MENU_COUNTER:
        sw_menu_set_fps_counter(sw_menu_fps_counter() ? 0 : 1);
        break;
    case SW_MENU_FULLSCREEN:
        InterlockedExchange(&g_action, SW_MENU_ACT_FULLSCREEN);
        break;
    default:
        break;
    }
}

static void activate_locked(int item)
{
    switch (item) {
    case SW_MENU_CONTINUE:
        set_open_locked(0);
        break;
    case SW_MENU_SHOT:
        InterlockedExchange(&g_action, SW_MENU_ACT_SCREENSHOT);
        set_open_locked(0);        /* the shot shows the game, not the menu */
        break;
    case SW_MENU_MODS:                  /* the overlay's mods menu takes over from this one */
        set_open_locked(0);
        InterlockedExchange(&g_ovl, 1);
        fprintf(stderr, "[menu] mods menu opened\n");
        break;
    case SW_MENU_QUIT:
        if (g_quit_armed) InterlockedExchange(&g_action, SW_MENU_ACT_QUIT);
        else InterlockedExchange(&g_quit_armed, 1);
        break;
    default:
        change_locked(item, 1);
        break;
    }
}

static void move_locked(int d)
{
    int s = ((int)g_sel + d + SW_MENU_ITEMS) % SW_MENU_ITEMS;
    InterlockedExchange(&g_sel, s);
    InterlockedExchange(&g_quit_armed, 0);
}

static void navigate_locked(uint64_t b, double t)
{
    const uint64_t up = B_UP | B_SL_UP, down = B_DOWN | B_SL_DOWN;
    const uint64_t left = B_LEFT | B_SL_LEFT, right = B_RIGHT | B_SL_RIGHT;
    uint64_t pressed = b & ~s_last;
    if (pressed & (up | down)) {
        move_locked(pressed & up ? -1 : 1);
        s_repeat_at = t + REPEAT_MS1;
    } else if ((b & (up | down)) && t >= s_repeat_at) {
        move_locked(b & up ? -1 : 1);
        s_repeat_at = t + REPEAT_MS;
    }
    if (pressed & (left | right)) change_locked((int)g_sel, pressed & left ? -1 : 1);
    if (pressed & B_A) activate_locked((int)g_sel);
    else if (pressed & B_B) set_open_locked(0);
}

/* Lone Plus / Minus: held back HOLD_MS waiting for the partner (see top). */
static uint64_t hold_back_locked(uint64_t b, double t)
{
    uint64_t pm = b & (B_PLUS | B_MINUS);
    uint64_t newly = pm & ~s_last;
    if (newly) {
        if (!s_held) s_held_t0 = t;
        s_held |= newly;
    }
    if (s_held) {
        if (t - s_held_t0 >= HOLD_MS) {
            s_held = 0;                         /* no combo: the game gets the button from now on */
        } else {
            uint64_t released = s_held & ~pm;   /* a quick tap: replay it as a short press */
            if (released) {
                s_replay |= released;
                s_replay_until = t + REPLAY_MS;
                s_held &= ~released;
            }
        }
    }
    uint64_t out = b & ~s_held;
    if (s_replay) {
        if (t < s_replay_until) out |= s_replay;
        else s_replay = 0;
    }
    return out;
}

uint64_t sw_menu_filter(uint64_t b, int *neutral_sticks)
{
    uint64_t out;
    double t = now_ms();
    AcquireSRWLockExclusive(&g_lock);
    int combo = (b & (B_PLUS | B_MINUS)) == (B_PLUS | B_MINUS);
    InterlockedExchange64(&g_raw, (LONG64)b);
    if (g_ovl) {                         /* overlay open: it reads g_raw; Minus+Plus closes it */
        if (combo && !s_combo) sw_menu_overlay_set(0);
        s_combo = combo;
        s_last = b;
        *neutral_sticks = 1;
        ReleaseSRWLockExclusive(&g_lock);
        return 0;
    }
    if (combo && !s_combo) {             /* Minus+Plus: the game menu (the ImGui overlay's menu page) */
        if (g_open) set_open_locked(0);
        sw_menu_overlay_open_page(SW_OVL_MENU);
    }
    s_combo = combo;
    *neutral_sticks = 0;
    if (g_open) {
        if (!combo) navigate_locked(b, t);
        out = 0;
        *neutral_sticks = 1;
    } else if (s_gate) {
        if (!b) s_gate = 0;                  /* b includes the stick directions */
        out = 0;
        *neutral_sticks = 1;
    } else {
        out = hold_back_locked(b, t);
    }
    s_last = b;
    ReleaseSRWLockExclusive(&g_lock);
    return out;
}

int sw_menu_is_open(void) { return (int)g_open; }

void sw_menu_toggle(void)                /* F1 / the test hook: the game menu on / off */
{
    if (g_ovl) { sw_menu_overlay_set(0); return; }
    sw_menu_overlay_open_page(SW_OVL_MENU);
}

int sw_menu_overlay_page(void) { return (int)g_ovl_page; }
void sw_menu_overlay_open_page(int page)
{
    InterlockedExchange(&g_ovl_page, page);
    sw_menu_overlay_set(1);
}
void sw_menu_request(int act) { InterlockedExchange(&g_action, act); }

int sw_menu_selected(void) { return (int)g_sel; }
int sw_menu_quit_armed(void) { return (int)g_quit_armed; }
int sw_menu_take_action(void) { return (int)InterlockedExchange(&g_action, SW_MENU_ACT_NONE); }

int sw_menu_fps_counter(void)
{
    if (g_counter < 0) {   /* SW_FPS_OVERLAY=0/1/2 (the launcher), else the in-game menu's last choice */
        const char *e = getenv("SW_FPS_OVERLAY");
        int v = e && *e ? atoi(e) : sw_ingame_get("perf_overlay");
        InterlockedCompareExchange(&g_counter, v < 0 ? 0 : v > 2 ? 2 : v, -1);
    }
    return (int)g_counter;
}
int sw_menu_overlay_open(void) { return (int)g_ovl; }
uint64_t sw_menu_pad_raw(void) { return (uint64_t)g_raw; }
void sw_menu_overlay_set(int open)
{
    /* called with g_lock held (sw_menu_filter) or not (overlay, present thread): only atomics here, and the
     * release gate is set on the next filter pass */
    if (InterlockedExchange(&g_ovl, open ? 1 : 0) != (open ? 1 : 0)) {
        s_gate = 1;                      /* the closing button must not leak to the game (benign race: a flag) */
        fprintf(stderr, "[menu] %s %s\n", g_ovl_page == SW_OVL_MENU ? "game menu" : "mods menu",
                open ? "opened" : "closed");
    }
}

void sw_menu_set_fps_counter(int mode)
{
    mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    InterlockedExchange(&g_counter, mode);
    sw_ingame_set("perf_overlay", mode);   /* kept for the next start; the launcher takes it over */
}
void sw_menu_set_fullscreen_state(int on) { InterlockedExchange(&g_fullscreen, on ? 1 : 0); }
int sw_menu_fullscreen_state(void) { return (int)g_fullscreen; }

/* ---- graphics settings the in-game menu changes live (saved in Config/ingame_settings.json)
 * Anisotropic filtering: level 0 = the game's, 1..4 = 2x/4x/8x/16x for the tiling surface textures (gfx vk_texture.c
 * vk_sampler_aniso_set: new samplers at once).  SW_ANISO=game|2|4|8|16 at start (the launcher), else the saved choice,
 * else 16x (nearly free on any GPU with Vulkan 1.3; the ground stays sharp into the distance).
 * Present mode: 0 V-Sync (fifo), 1 fast V-Sync (mailbox: no tearing, no waiting for the screen), 2 off (immediate:
 * lowest delay, tearing).  SW_VK_PRESENT at start, else the saved choice; the swapchain is rebuilt on a change
 * (gfx_present.c reads TOTK_VK_PRESENT every present). */
void vk_sampler_aniso_set(float a);                  /* gfx/vk/vk_texture.c */
void cfg_set(const char *name, const char *value);   /* gfx/sw_cfg.c */
static volatile LONG g_aniso = -1, g_present = -1;
static const int kAniso[5] = {0, 2, 4, 8, 16};
static const char *const kPresent[3] = {"fifo", "mailbox", "immediate"};

int sw_aniso_get(void)
{
    if (g_aniso < 0) {
        const char *e = getenv("SW_ANISO");
        int v = sw_ingame_get("anisotropy");
        if (e && *e) {
            int a = atoi(e);
            v = a >= 16 ? 4 : a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0;
        }
        if (v < 0) v = 4;
        InterlockedCompareExchange(&g_aniso, v > 4 ? 4 : v, -1);
    }
    return (int)g_aniso;
}
void sw_aniso_set(int level)
{
    level = level < 0 ? 0 : level > 4 ? 4 : level;
    InterlockedExchange(&g_aniso, level);
    vk_sampler_aniso_set((float)kAniso[level]);
    sw_ingame_set("anisotropy", level);
    fprintf(stderr, "[menu] anisotropic filtering %s\n", level ? "on" : "the game's");
}
int sw_present_get(void)
{
    if (g_present < 0) {
        const char *e = getenv("SW_VK_PRESENT");
        int v = sw_ingame_get("present_mode");
        if (e && *e) v = !strcmp(e, "mailbox") ? 1 : !strcmp(e, "immediate") ? 2 : 0;
        if (v < 0) v = 0;
        InterlockedCompareExchange(&g_present, v > 2 ? 2 : v, -1);
    }
    return (int)g_present;
}
void sw_present_set(int mode)
{
    mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    InterlockedExchange(&g_present, mode);
    cfg_set("VK_PRESENT", kPresent[mode]);
    sw_ingame_set("present_mode", mode);
}
/* at start, before the renderer makes samplers or the swapchain (sw_hooks.c, next to sw_shadow_init) */
int sw_res_get(void);
void sw_gfx_settings_init(void)
{
    char b[32];   /* the renderer reads SW_RES_SCALE (cfg RES_SCALE) when it makes its first target */
    snprintf(b, sizeof b, "SW_RES_SCALE=%d", sw_res_get());
    _putenv(b);
    { extern int sw_dlaa_get(void); snprintf(b, sizeof b, "SW_DLAA=%d", sw_dlaa_get()); _putenv(b); }   /* before the device */
    vk_sampler_aniso_set((float)kAniso[sw_aniso_get()]);
    cfg_set("VK_PRESENT", kPresent[sw_present_get()]);
    { extern void sw_picture_init(void); sw_picture_init(); }
}

/* Resolution scale: 1 = the game's own (1920x1080 docked), 2..4 = its screen-sized render targets that many times larger
 * (gfx vk_texture.c res_scale_of; 2x docked = 3840x2160).  Live: the targets are made again at the next submit.
 * SW_RES_SCALE at start (the launcher), else the saved choice, else 1. */
void vk_texture_res_set(int s);   /* gfx/vk/vk_texture.c */
static volatile LONG g_res = -1;
int sw_res_get(void)
{
    if (g_res < 0) {
        const char *e = getenv("SW_RES_SCALE");
        int v = e && *e ? atoi(e) : sw_ingame_get("render_scale");
        InterlockedCompareExchange(&g_res, v < 1 ? 1 : v > 4 ? 4 : v, -1);
    }
    return (int)g_res;
}
void sw_res_set(int scale)
{
    scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
    InterlockedExchange(&g_res, scale);
    vk_texture_res_set(scale);
    sw_ingame_set("render_scale", scale);
    fprintf(stderr, "[menu] resolution scale %dx\n", scale);
}

/* Picture settings on the present (gfx vk_fsr.c, vk_color.c; live through cfg_set):
 * upscaling 0 bilinear / 1 FSR 1 (when the picture is drawn larger than the game's output), sharpness 0..10 (x10, the
 * FSR RCAS pass), colours 0 original / 1 vivid / 2 rich (saturation and contrast of the final picture). */
static volatile LONG g_ups = -1, g_sharp = -1, g_colors = -1;
static int pic_get(volatile LONG *v, const char *env, const char *key, int def, int most)
{
    if (*v < 0) {
        const char *e = getenv(env);
        int x = e && *e ? atoi(e) : sw_ingame_get(key);
        if (x < 0) x = def;
        InterlockedCompareExchange(v, x > most ? most : x, -1);
    }
    return (int)*v;
}
static void pic_apply(void)
{
    static const char *const sat[3] = {"100", "125", "140"}, *const con[3] = {"100", "106", "112"};
    char b[16];
    cfg_set("UPSCALER", g_ups == 1 ? "fsr" : "bilinear");
    snprintf(b, sizeof b, "%d", (int)g_sharp * 10);
    cfg_set("SHARPNESS", b);
    cfg_set("COLOR_SATURATION", sat[g_colors]);
    cfg_set("COLOR_CONTRAST", con[g_colors]);
}
int sw_upscale_get(void) { return pic_get(&g_ups, "SW_UPSCALE", "upscaler", 1, 1); }
int sw_sharp_get(void) { return pic_get(&g_sharp, "SW_SHARP", "sharpness", 5, 10); }
int sw_colors_get(void) { return pic_get(&g_colors, "SW_COLORS", "colors", 0, 2); }
void sw_upscale_set(int v) { InterlockedExchange(&g_ups, v ? 1 : 0); sw_ingame_set("upscaler", (int)g_ups); pic_apply(); }
void sw_sharp_set(int v) { InterlockedExchange(&g_sharp, v < 0 ? 0 : v > 10 ? 10 : v); sw_ingame_set("sharpness", (int)g_sharp); pic_apply(); }
void sw_colors_set(int v) { InterlockedExchange(&g_colors, v < 0 ? 0 : v > 2 ? 2 : v); sw_ingame_set("colors", (int)g_colors); pic_apply(); }
void sw_picture_init(void) { sw_upscale_get(); sw_sharp_get(); sw_colors_get(); pic_apply(); }

/* DLAA (NVIDIA DLSS at the game's own resolution, gfx vk_dlaa.c): NGX is set up when the GPU device is made, so turning
 * it on needs a start with it on; after that it switches live (cfg DLAA, read every frame).  Needs an RTX card, the
 * driver's NGX core and nvngx_dlss.dll next to the game (the launcher copies the player's own).  Saved as "dlaa";
 * SW_DLAA at start overrides. */
int vk_dlaa_available(void);   /* gfx/vk/vk_dlaa.c: NGX came up at start with DLSS */
static volatile LONG g_dlaa = -1, g_dlaa_start = -1;
int sw_dlaa_get(void)
{
    if (g_dlaa < 0) {
        const char *e = getenv("SW_DLAA");
        int v = e && *e ? atoi(e) != 0 : sw_ingame_get("dlaa") > 0;
        InterlockedCompareExchange(&g_dlaa, v, -1);
        InterlockedCompareExchange(&g_dlaa_start, v, -1);
    }
    return (int)g_dlaa;
}
/* 0 off, 1 on, 2 on but needs a restart (not on at start), 3 on but unavailable (no RTX / no NGX / no DLL) */
int sw_dlaa_state(void)
{
    if (!sw_dlaa_get()) return 0;
    if (!g_dlaa_start) return 2;
    return vk_dlaa_available() ? 1 : 3;
}
void sw_dlaa_set(int on)
{
    sw_dlaa_get();
    InterlockedExchange(&g_dlaa, on ? 1 : 0);
    cfg_set("DLAA", on ? "1" : "0");
    sw_ingame_set("dlaa", on ? 1 : 0);
}
