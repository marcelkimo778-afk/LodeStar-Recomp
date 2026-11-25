/* sw_swkbd.c - the software keyboard library applet's text entry.
 *
 * The game opens the keyboard (applet 0x11) to ask for the player's name;
 * sw_ipc.c's PopOutData calls sw_text_entry, which shows a small Win32
 * window with the game's prompt and an edit box and waits (the calling guest
 * thread blocks, as on the Switch; the render window keeps its own thread).
 * Enter / OK confirms, Esc / Cancel cancels.  Idea and layout of the request
 * follow totk-pc-recomp's runtime/swkbd_win.c (MIT).
 *
 * SW_SWKBD_ANSWER=<text> answers without a window (scripted runs).
 */
#include "sw_rt.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#if defined(_WIN32)
#include <windows.h>

enum { ID_EDIT = 100, ID_OK = IDOK, ID_CANCEL = IDCANCEL };

static struct {
    HWND wnd, edit;
    int done, ok, max_len;
    wchar_t *out;
} g_kb;

static LRESULT CALLBACK kb_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == ID_OK || LOWORD(w) == ID_CANCEL) {
            g_kb.ok = LOWORD(w) == ID_OK;
            if (g_kb.ok) GetWindowTextW(g_kb.edit, g_kb.out, g_kb.max_len + 1);
            g_kb.done = 1;
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        g_kb.ok = 0; g_kb.done = 1;
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int sw_text_entry(const wchar_t *prompt, const wchar_t *initial, int max_len, wchar_t *out)
{
    const char *ans = getenv("SW_SWKBD_ANSWER");
    if (max_len <= 0 || max_len > 500) max_len = 500;
    if (ans) {
        MultiByteToWideChar(CP_UTF8, 0, ans, -1, out, max_len + 1);
        out[max_len] = 0;
        return 1;
    }
    static int registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = kb_proc;
        wc.hInstance = inst;
        wc.lpszClassName = L"SwKeyboard";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        registered = 1;
    }
    memset(&g_kb, 0, sizeof g_kb);
    g_kb.out = out;
    g_kb.max_len = max_len;
    out[0] = 0;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int W = 460, H = 170;
    g_kb.wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"SwKeyboard",
                               L"Pok\x00e9mon Legends: Arceus - name entry",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               (sw - W) / 2, (sh - H) / 2, W, H, NULL, NULL, inst, NULL);
    if (!g_kb.wnd) return 0;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND st = CreateWindowW(L"STATIC", prompt && prompt[0] ? prompt : L"Enter a name:",
                            WS_CHILD | WS_VISIBLE, 14, 12, W - 40, 20, g_kb.wnd, NULL, inst, NULL);
    g_kb.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial ? initial : L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                14, 38, W - 44, 26, g_kb.wnd, (HMENU)(INT_PTR)ID_EDIT, inst, NULL);
    HWND ok = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                            W - 200, 80, 80, 28, g_kb.wnd, (HMENU)(INT_PTR)ID_OK, inst, NULL);
    HWND cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                W - 110, 80, 80, 28, g_kb.wnd, (HMENU)(INT_PTR)ID_CANCEL, inst, NULL);
    SendMessageW(st, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_kb.edit, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(ok, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_kb.edit, EM_LIMITTEXT, (WPARAM)max_len, 0);
    SendMessageW(g_kb.edit, EM_SETSEL, 0, -1);
    SetForegroundWindow(g_kb.wnd);
    SetFocus(g_kb.edit);
    MSG msg;
    while (!g_kb.done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { SendMessageW(g_kb.wnd, WM_COMMAND, ID_OK, 0); continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { SendMessageW(g_kb.wnd, WM_COMMAND, ID_CANCEL, 0); continue; }
        if (!IsDialogMessageW(g_kb.wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return g_kb.ok;
}

/* ---- nn::swkbd::ShowKeyboard(String *out, const ShowKeyboardArg &arg) ----
 * Replaced by address (sw_runtime.c redirect, listed in sw_hid.c): the
 * applet protocol is skipped.  ShowKeyboardArg = KeyboardConfig (0x4d0
 * bytes: offsets as totk-pc-recomp's swkbd.c / libnx) + work buffer
 * {pointer, size}; String = {pointer, byte size}. */
enum {
    KB_TYPE = 0x00, KB_HEADER = 0x24, KB_SUB = 0xa6, KB_GUIDE = 0x1a8, KB_MAX_LEN = 0x3ac, KB_MIN_LEN = 0x3b0,
    KB_UTF8 = 0x3bd, KB_INIT_OFF = 0x3c0, KB_INIT_LEN = 0x3c4, KB_WORK = 0x4d0, SWKBD_CANCELED = 0x29f,
};

static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 rd64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }

static void copy_u16(wchar_t *dst, const u8 *src, int max)
{
    int i = 0;
    for (; i < max && (src[2 * i] | src[2 * i + 1]); i++) dst[i] = (wchar_t)(src[2 * i] | src[2 * i + 1] << 8);
    dst[i] = 0;
}

void sw_hle_show_keyboard(SwCpu *c)
{
    u64 *str = (u64 *)sw_mem_ptr(c, c->x[0], 16, 1);
    const u8 *arg = sw_mem_ptr(c, c->x[1], KB_WORK + 16, 0);
    if (!str || !arg) { c->x[0] = SWKBD_CANCELED; return; }
    wchar_t prompt[0x101] = {0}, initial[501] = {0}, text[501] = {0};
    copy_u16(prompt, arg + KB_HEADER, 0x40);
    if (!prompt[0]) copy_u16(prompt, arg + KB_GUIDE, 0x100);
    if (!prompt[0]) copy_u16(prompt, arg + KB_SUB, 0x80);
    int max_len = (int)rd32(arg + KB_MAX_LEN), min_len = (int)rd32(arg + KB_MIN_LEN);
    if (max_len <= 0 || max_len > 500) max_len = 500;
    if (min_len < 0 || min_len > max_len) min_len = 0;
    int utf8 = arg[KB_UTF8] != 0;
    u64 work = rd64(arg + KB_WORK), work_size = rd64(arg + KB_WORK + 8);
    int init_off = (int)rd32(arg + KB_INIT_OFF), init_len = (int)rd32(arg + KB_INIT_LEN);
    if (init_len > 0 && work && init_off >= 0 && (u64)init_off + (u64)init_len * 2 <= work_size) {
        const u8 *t = sw_mem_ptr(c, work + (u64)init_off, (u64)init_len * 2, 0);
        if (t) copy_u16(initial, t, init_len > 500 ? 500 : init_len);
    }
    int ok = sw_text_entry(prompt, initial, max_len, text);
    if (ok && (int)wcslen(text) < min_len) ok = 0;
    if (ok && !text[0] && min_len > 0) ok = 0;
    u64 buf = str[0], size = str[1];
    u8 *out = buf && size ? sw_mem_ptr(c, buf, size, 1) : NULL;
    if (out) {
        if (!ok) text[0] = 0;
        if (utf8) {
            int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, (char *)out, (int)size, NULL, NULL);
            if (n <= 0) out[0] = 0;
        } else {
            size_t units = (size_t)size / 2, n = wcslen(text);
            if (units) {
                if (n > units - 1) n = units - 1;
                memcpy(out, text, n * 2);
                ((u16 *)out)[n] = 0;
            }
        }
    }
    char p8[0x400], t8[0x600];
    WideCharToMultiByte(CP_UTF8, 0, prompt, -1, p8, sizeof p8, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, text, -1, t8, sizeof t8, NULL, NULL);
    sw_report("[swkbd] \"%s\" (%d-%d chars, %s) -> %s%s%s\n", p8, min_len, max_len, utf8 ? "UTF-8" : "UTF-16",
              ok ? "\"" : "cancelled", ok ? t8 : "", ok ? "\"" : "");
    c->x[0] = ok ? 0 : SWKBD_CANCELED;
}
#else
int sw_text_entry(const wchar_t *prompt, const wchar_t *initial, int max_len, wchar_t *out)
{
    (void)prompt; (void)initial; (void)max_len;
    wcscpy(out, L"Player");
    return 1;
}
void sw_hle_show_keyboard(SwCpu *c) { c->x[0] = 0x29f; }
#endif
