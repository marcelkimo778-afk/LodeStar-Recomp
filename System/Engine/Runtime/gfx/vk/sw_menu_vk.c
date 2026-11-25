/* sw_menu_vk.c - draws the in-game menu (state and input: src/sw_menu.c) and
 * the FPS counter over the game image, and runs the menu's actions.
 *
 * Called by the present path (gfx_present.c): menu_overlay_frame() lays the
 * frame out before recording, menu_overlay_draw() records it inside dynamic
 * rendering on the swapchain image.  Everything is vkCmdClearAttachments
 * rectangles (an 8x8 bitmap font, runs of lit pixels merged per row): no
 * pipeline, no shaders, nothing to create or release.
 *
 * Keys (only while one of this process's windows is in front):
 *   F1  menu on / off (as Minus+Plus on the pad)
 *   F3  FPS counter on / off (SW_FPS_OVERLAY=1 starts with it on)
 *   F10 performance overlay: off, FPS, detailed (ui/sw_perfhud.cpp)
 *   F4  mods menu on / off (the Dear ImGui overlay, ui/sw_overlay.cpp; also "Mods" in this menu)
 * Screenshot hook (test tools): SW_SHOT_FRAMES=300,600,... / SW_SHOT_EVERY=N /
 * SW_SHOT_RANGE=a-b save presented frames as screenshots/frame_NNNNN.png.bmp;
 * the menu's "Take screenshot" saves screenshots/shot_<date>_<time>.png.bmp. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "gfx_wsys.h"
#include "ui/menu.h"
#include "../../src/sw_menu.h"

int sw_fps_get(void);   /* src/sw_hooks.c */
/* the Dear ImGui overlay (ui/sw_overlay.cpp): drawn after this menu */
int ovl_frame(const MenuTarget *t);
void perfhud_present(double ms);   /* ui/sw_perfhud.cpp: every present's frame time */
void ovl_draw(VkCommandBuffer cb);
void ovl_release(void);
int ovl_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res);
static int g_ovl_draw;      /* the overlay has something to draw this frame */

/* ---- 8x8 font, ASCII 32..126 (bit 7 = left) */
static const unsigned char g_font[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* # */
    {0x0C,0x3E,0x0C,0x7F,0x0C,0x3E,0x0C,0x00}, /* $ */
    {0x00,0x63,0x46,0x0C,0x18,0x31,0xC6,0x00}, /* % */
    {0x1C,0x36,0x1C,0x38,0x16,0x36,0x1C,0x00}, /* & */
    {0x0C,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* ( */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* ) */
    {0x00,0x18,0x3C,0x7E,0x3C,0x18,0x00,0x00}, /* * */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* , */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
    {0x00,0x06,0x0C,0x18,0x30,0x60,0xC0,0x00}, /* / */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 1 */
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, /* 2 */
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, /* 3 */
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}, /* 4 */
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, /* 5 */
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, /* 6 */
    {0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x00}, /* 7 */
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, /* 8 */
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, /* 9 */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* : */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ; */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* < */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* = */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* > */
    {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00}, /* ? */
    {0x3C,0x66,0x6E,0x7A,0x72,0x60,0x3C,0x00}, /* @ */
    {0x18,0x3C,0x66,0x7E,0x66,0x66,0x66,0x00}, /* A */
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, /* B */
    {0x1C,0x36,0x60,0x60,0x60,0x36,0x1C,0x00}, /* C */
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, /* D */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, /* E */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, /* F */
    {0x1C,0x36,0x60,0x6E,0x66,0x36,0x1C,0x00}, /* G */
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, /* H */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* I */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00}, /* J */
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, /* K */
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, /* L */
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, /* M */
    {0x63,0x73,0x7B,0x6F,0x67,0x63,0x63,0x00}, /* N */
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* O */
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, /* P */
    {0x3C,0x66,0x66,0x66,0x6E,0x3C,0x06,0x00}, /* Q */
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, /* R */
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, /* S */
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* T */
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* U */
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, /* V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, /* X */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, /* Y */
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, /* Z */
    {0x3E,0x30,0x30,0x30,0x30,0x30,0x3E,0x00}, /* [ */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x03,0x00}, /* \ */
    {0x3E,0x0C,0x0C,0x0C,0x0C,0x0C,0x3E,0x00}, /* ] */
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E}, /* _ */
    {0x18,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3B,0x00}, /* a */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, /* b */
    {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00}, /* c */
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, /* d */
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, /* e */
    {0x1C,0x36,0x60,0x7C,0x60,0x60,0x60,0x00}, /* f */
    {0x00,0x00,0x3C,0x66,0x66,0x3E,0x06,0x3C}, /* g */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, /* h */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* i */
    {0x0C,0x00,0x1C,0x0C,0x0C,0x0C,0x0C,0x38}, /* j */
    {0x60,0x60,0x6C,0x78,0x6C,0x66,0x66,0x00}, /* k */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* l */
    {0x00,0x00,0xB6,0xD6,0xEE,0xC6,0xC6,0x00}, /* m */
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, /* n */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, /* o */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, /* p */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06}, /* q */
    {0x00,0x00,0x7C,0x60,0x60,0x60,0x60,0x00}, /* r */
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, /* s */
    {0x18,0x30,0x7C,0x30,0x30,0x33,0x1E,0x00}, /* t */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, /* u */
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, /* v */
    {0x00,0x00,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* w */
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, /* x */
    {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, /* y */
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, /* z */
    {0x0C,0x18,0x18,0x30,0x18,0x18,0x0C,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x30,0x18,0x18,0x0C,0x18,0x18,0x30,0x00}, /* } */
    {0x36,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}  /* DEL */
};

/* ---- draw list: rectangles per colour, drawn in colour order (= depth order) */
enum { C_BORDER, C_PANEL, C_BAR, C_TEXT, C_DIM, C_TITLE, C_SELTEXT, C_WARN, C_FPSBG, C_FPS, C_COUNT };
#define MAX_RECTS 6144
static float g_col[C_COUNT][4] = {
    {0.95f, 0.75f, 0.25f, 1.0f},   /* border: Arceus gold */
    {0.06f, 0.07f, 0.11f, 1.0f},   /* panel */
    {0.95f, 0.75f, 0.25f, 1.0f},   /* selection bar */
    {0.92f, 0.92f, 0.95f, 1.0f},   /* text */
    {0.55f, 0.58f, 0.65f, 1.0f},   /* hints */
    {0.95f, 0.75f, 0.25f, 1.0f},   /* title */
    {0.05f, 0.05f, 0.08f, 1.0f},   /* text on the bar */
    {1.00f, 0.35f, 0.30f, 1.0f},   /* warning */
    {0.0f, 0.0f, 0.0f, 1.0f},      /* FPS counter background */
    {0.3f, 1.0f, 0.4f, 1.0f},      /* FPS counter text (green / yellow / red) */
};
static VkClearRect g_rects[C_COUNT][MAX_RECTS];
static uint32_t g_nrects[C_COUNT];
static uint32_t g_w, g_h;          /* swapchain image */

static void add_rect(int c, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || x >= (int)g_w || y >= (int)g_h) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g_w) w = (int)g_w - x;
    if (y + h > (int)g_h) h = (int)g_h - y;
    if (w <= 0 || h <= 0 || g_nrects[c] >= MAX_RECTS) return;
    VkClearRect *r = &g_rects[c][g_nrects[c]++];
    r->rect.offset.x = x; r->rect.offset.y = y;
    r->rect.extent.width = (uint32_t)w; r->rect.extent.height = (uint32_t)h;
    r->baseArrayLayer = 0; r->layerCount = 1;
}

/* text at (x, y) top left, s screen pixels per font pixel; returns its width */
static int add_text(int c, int x, int y, int s, const char *t)
{
    int cx = x;
    for (; *t; t++, cx += 8 * s) {
        unsigned ch = (unsigned char)*t;
        if (ch < 32 || ch > 126) continue;
        const unsigned char *g = g_font[ch - 32];
        for (int row = 0; row < 8; row++) {
            unsigned bits = g[row];
            for (int col = 0; col < 8;) {
                if (!(bits & (0x80u >> col))) { col++; continue; }
                int run = col;
                while (run < 8 && (bits & (0x80u >> run))) run++;
                add_rect(c, cx + col * s, y + row * s, (run - col) * s, s);
                col = run;
            }
        }
    }
    return cx - x;
}
static int text_w(const char *t, int s) { return (int)strlen(t) * 8 * s; }

/* ---- the game window (for fullscreen): the visible top-level window of this process */
static BOOL CALLBACK find_window_cb(HWND h, LPARAM l)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
        *(HWND *)l = h;
        return FALSE;
    }
    return TRUE;
}
static HWND game_window(void)
{
    static HWND h;
    if (!h || !IsWindow(h)) { h = NULL; EnumWindows(find_window_cb, (LPARAM)&h); }
    return h;
}
static int window_is_fullscreen(HWND h) { return h && !(GetWindowLongW(h, GWL_STYLE) & WS_OVERLAPPEDWINDOW); }

static int our_window_in_front(void)
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* ---- FPS counter: "30 FPS 33.3 ms  worst 41 ms" top left, refreshed twice a second
 * (average frame time and the slowest frame of that half second = stutters).
 * Green >= 55 fps, yellow >= 28, red below.  Measured always: the menu shows it too. */
static char g_fps_text[64] = "-- FPS";
static double g_fps_now;

static void fps_measure(void)
{
    static LARGE_INTEGER fq, last, win0;
    static unsigned frames;
    static double sum, worst;
    if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        double ms = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)fq.QuadPart;
        perfhud_present(ms);
        sum += ms; frames++;
        if (ms > worst) worst = ms;
    } else win0 = now;
    last = now;
    double span = (double)(now.QuadPart - win0.QuadPart) / (double)fq.QuadPart;
    if (span >= 0.5 && frames) {
        g_fps_now = frames / span;
        snprintf(g_fps_text, sizeof g_fps_text, "%.0f FPS  %.1f ms  worst %.0f ms", g_fps_now, sum / frames, worst);
        static const float gr[4] = {0.3f, 1.0f, 0.4f, 1.0f}, ye[4] = {1.0f, 0.85f, 0.2f, 1.0f}, re[4] = {1.0f, 0.3f, 0.25f, 1.0f};
        memcpy(g_col[C_FPS], g_fps_now >= 55 ? gr : g_fps_now >= 28 ? ye : re, sizeof g_col[C_FPS]);
        win0 = now; frames = 0; sum = worst = 0;
    }
}

static void layout_fps(void)
{
    int s = g_h >= 1000 ? 2 : 1, pad = 4 * s;
    if (g_h >= 1400) s = 3;
    int x0 = 10, y0 = 10, tw = text_w(g_fps_text, s);
    if (x0 + tw + 2 * pad > (int)g_w) return;
    add_rect(C_FPSBG, x0, y0, tw + 2 * pad, 8 * s + 2 * pad);
    add_text(C_FPS, x0 + pad, y0 + pad, s, g_fps_text);
}

/* ---- the menu */
static void layout_menu(void)
{
    static const char *labels[SW_MENU_ITEMS] = {
        "Continue", "Frame rate", "FPS counter", "Fullscreen", "Mods", "Take screenshot", "Quit game"};
    int s = (int)g_h / 360;                 /* font pixel: 2 at 720p, 3 at 1080p, 4 at 1440p */
    if (s < 1) s = 1;
    const int cols = 44, line = 13 * s, pad = 12 * s, b = s > 1 ? s : 1;
    int pw = cols * 8 * s + 2 * pad, rows = 2 + SW_MENU_ITEMS + 4;
    int ph = rows * line + 2 * pad;
    if (pw > (int)g_w - 8) { s = 1; pw = cols * 8 + 24; }
    int px = ((int)g_w - pw) / 2, py = ((int)g_h - ph) / 2;
    if (py < 4) py = 4;
    add_rect(C_BORDER, px - 2 * b, py - 2 * b, pw + 4 * b, ph + 4 * b);
    add_rect(C_PANEL, px, py, pw, ph);

    int x = px + pad, y = py + pad, right = px + pw - pad;
    add_text(C_TITLE, x, y, s, "POKEMON LEGENDS: ARCEUS");
    {
        char now[32];
        snprintf(now, sizeof now, g_fps_now > 0 ? "%.0f fps" : "-- fps", g_fps_now);
        add_text(C_DIM, right - text_w(now, s), y, s, now);
    }
    y += line;
    add_rect(C_BORDER, x, y + line / 3, pw - 2 * pad, b);
    y += line;

    int sel = sw_menu_selected(), fps = sw_fps_get();
    for (int i = 0; i < SW_MENU_ITEMS; i++, y += line) {
        const char *val = NULL;
        switch (i) {
        case SW_MENU_FPS: val = fps == 120 ? "<  120 FPS  >" : fps == 90 ? "<  90 FPS  >" : fps == 60 ? "<  60 FPS  >"
                                                                                                 : "<  30 FPS  >"; break;
        case SW_MENU_COUNTER: val = sw_menu_fps_counter() ? "<  On  >" : "<  Off  >"; break;
        case SW_MENU_FULLSCREEN: val = sw_menu_fullscreen_state() ? "<  On  >" : "<  Off  >"; break;
        case SW_MENU_QUIT: val = sw_menu_quit_armed() ? "Press A again" : NULL; break;
        }
        int on = i == sel;
        if (on) add_rect(C_BAR, px + pad / 2, y - 2 * s, pw - pad, line);
        int tc = on ? C_SELTEXT : C_TEXT;
        add_text(tc, x + 2 * 8 * s, y, s, labels[i]);
        if (on) add_text(tc, x, y, s, ">");
        if (val) add_text(on ? C_SELTEXT : (i == SW_MENU_QUIT ? C_WARN : C_DIM), right - text_w(val, s), y, s, val);
    }
    y += line / 2;
    add_rect(C_BORDER, x, y + line / 3, pw - 2 * pad, b);
    y += line;
    if (sel == SW_MENU_FPS)
        add_text(C_DIM, x, y, s, fps > 60 ? "Above 60 needs a 90/120 Hz display" : "60+ FPS: game speed follows the real clock");
    else if (sel == SW_MENU_QUIT && sw_menu_quit_armed())
        add_text(C_WARN, x, y, s, "Unsaved progress is lost. A = quit");
    y += line;
    add_text(C_DIM, x, y, s, "Up/Down choose   Left/Right/A change");
    y += line;
    add_text(C_DIM, x, y, s, "B or Minus+Plus or F1: close");
}

/* ---- actions the menu queued (run here, on the present thread) */
static uint64_t g_shot_frame;      /* take a screenshot at this presented frame */
static char g_shot_name[MAX_PATH];

static void run_actions(uint64_t frame)
{
    HWND h = game_window();
    sw_menu_set_fullscreen_state(window_is_fullscreen(h));
    switch (sw_menu_take_action()) {
    case SW_MENU_ACT_FULLSCREEN:
        if (h) PostMessageW(h, WM_KEYDOWN, VK_F11, 0);   /* the window's own F11 toggle */
        break;
    case SW_MENU_ACT_SCREENSHOT: {
        SYSTEMTIME st;
        GetLocalTime(&st);
        CreateDirectoryA("screenshots", NULL);
        snprintf(g_shot_name, sizeof g_shot_name, "screenshots/shot_%04u%02u%02u_%02u%02u%02u.png",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        g_shot_frame = frame + 2;   /* the menu is closed by then */
        rt_log("menu: screenshot %s.bmp", g_shot_name);
        break;
    }
    case SW_MENU_ACT_QUIT:
        rt_log("menu: quit game");
        gfx_window_request_close();
        break;
    default:
        break;
    }
}

/* ---- the present path's hooks (ui/menu.h) */
int menu_overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot)
{
    static char path[MAX_PATH];
    static int init, every, f1_was, f3_was, f4_was, f10_was;
    static unsigned long long r0 = 1, r1 = 0;
    static const char *list;
    if (!init) {
        init = 1;
        list = getenv("SW_SHOT_FRAMES");
        const char *e = getenv("SW_SHOT_EVERY");
        every = e ? atoi(e) : 0;
        const char *r = getenv("SW_SHOT_RANGE");      /* a-b: every frame in a..b */
        if (r) sscanf(r, "%llu-%llu", &r0, &r1);
        if (r) rt_log("gfx: shots for frames %llu-%llu", r0, r1);
        if (list || every || r) CreateDirectoryA("screenshots", NULL);
    }
    {   /* F1 menu, F3 FPS counter: only while this process's window is in front */
        int front = our_window_in_front();
        int f1 = front && (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        int f3 = front && (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        int f4 = front && (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        int f10 = front && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f1 && !f1_was && !sw_menu_overlay_open()) sw_menu_toggle();
        if (f3 && !f3_was) sw_menu_set_fps_counter(sw_menu_fps_counter() ? 0 : 1);
        if (f10 && !f10_was) sw_menu_set_fps_counter((sw_menu_fps_counter() + 1) % 3);   /* off, FPS, detailed */
        if (f4 && !f4_was) {                      /* the mods page; F4 on it closes the overlay */
            if (sw_menu_overlay_open() && sw_menu_overlay_page() == SW_OVL_MODS) sw_menu_overlay_set(0);
            else sw_menu_overlay_open_page(SW_OVL_MODS);
        }
        f1_was = f1; f3_was = f3; f4_was = f4; f10_was = f10;
    }
    fps_measure();
    {   /* test hook: SW_MENU_TEST=<frame> opens the menu at that presented frame (with SW_SHOT_FRAMES) */
        static long long test_at = -2;
        if (test_at == -2) { const char *e = getenv("SW_MENU_TEST"); test_at = e ? atoll(e) : -1; }
        if (test_at >= 0 && frame >= (uint64_t)test_at) {
            test_at = -1;
            if (!sw_menu_overlay_open()) sw_menu_toggle();
        }
    }
    run_actions(frame);
    {   /* test switch SW_RES_AT=<frame>:<scale>[,...]: resolution scale changes at those frames, as the menu makes them */
        static const char *at = (const char *)1;
        if (at == (const char *)1) at = getenv("SW_RES_AT");
        unsigned long long f;
        int s, used;
        if (at && *at && sscanf(at, "%llu:%d%n", &f, &s, &used) == 2 && frame >= f) {
            extern void sw_res_set(int scale);
            sw_res_set(s);
            at += used;
            if (*at == ',') at++;
        }
    }

    int want = (every > 0 && frame > 0 && frame % (uint64_t)every == 0) || (frame >= r0 && frame <= r1);
    for (const char *p = list; p && *p && !want;) {
        char *end;
        unsigned long long n = strtoull(p, &end, 10);
        if (end == p) break;
        if (n == frame) want = 1;
        p = *end ? end + 1 : end;
    }
    if (want) {
        snprintf(path, sizeof path, "screenshots/frame_%05llu.png", (unsigned long long)frame);
        *shot = path;
    } else if (g_shot_frame && frame >= g_shot_frame) {
        g_shot_frame = 0;
        *shot = g_shot_name;
    }

    memset(g_nrects, 0, sizeof g_nrects);
    g_w = t ? t->width : 0;
    g_h = t ? t->height : 0;
    g_ovl_draw = 0;
    if (!g_w || !g_h) return 0;
    int menu = sw_menu_is_open(), ovl = sw_menu_overlay_open();
    if (sw_menu_fps_counter() == 1 && !menu && !ovl) layout_fps();
    if (menu) layout_menu();
    g_ovl_draw = ovl_frame(t);                    /* one load when the overlay is closed */
    if (g_ovl_draw) return 1;
    for (int c = 0; c < C_COUNT; c++)
        if (g_nrects[c]) return 1;
    return 0;
}

void menu_overlay_draw(VkCommandBuffer cb)
{
    for (int c = 0; c < C_COUNT; c++) {
        if (!g_nrects[c]) continue;
        VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {0}}}};
        memcpy(a.clearValue.color.float32, g_col[c], sizeof g_col[c]);
        vkCmdClearAttachments(cb, 1, &a, g_nrects[c], g_rects[c]);
    }
    if (g_ovl_draw) ovl_draw(cb);
}

void menu_vk_release(void) { ovl_release(); }
int menu_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res) { return ovl_window_msg(h, m, w, l, res); }
