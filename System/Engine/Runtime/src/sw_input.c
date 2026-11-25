/* sw_input.c - host game controller -> Switch HID state.
 *
 * The standalone runtime owns input: game code reads the Switch HID shared
 * memory through the compatibility HID service, and this module fills it from
 * a real PC controller.  Two Windows back ends, both part of Windows (nothing
 * to install or bundle):
 *   - XInput: Xbox and Xbox-compatible pads, slots 0..3 (wired or wireless).
 *   - winmm joystick API: every other HID game controller - PlayStation 4/5,
 *     Switch Pro, 8BitDo, generic pads - over USB *or Bluetooth*, ids 0..15.
 * Other platforms report "no controller" until a back end is added.
 *
 * Mapping follows the Switch physical layout:
 *   A/B/X/Y, L/R, ZL/ZR, +/-, Home, Capture, L3/R3, d-pad, both sticks.
 */
#include "sw_rt.h"

void sw_input_init(void)
{
}

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")

typedef struct SwXinputGamepad {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} SwXinputGamepad;

typedef struct SwXinputState {
    DWORD dwPacketNumber;
    SwXinputGamepad Gamepad;
} SwXinputState;

typedef DWORD(WINAPI *SwXInputGetStateFn)(DWORD, SwXinputState *);

static SwXInputGetStateFn g_xinput_get_state;
static int g_xinput_tried;

static void load_xinput(void)
{
    static const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    unsigned i;
    HMODULE mod;
    if (g_xinput_tried) return;
    g_xinput_tried = 1;
    for (i = 0; i < sizeof(dlls) / sizeof(dlls[0]); i++) {
        mod = LoadLibraryA(dlls[i]);
        if (!mod) continue;
        g_xinput_get_state = (SwXInputGetStateFn)(void *)GetProcAddress(mod, "XInputGetState");
        if (g_xinput_get_state) return;
        FreeLibrary(mod);
    }
}

/* XInput button bits. */
#define SW_XB_DPAD_UP 0x0001
#define SW_XB_DPAD_DOWN 0x0002
#define SW_XB_DPAD_LEFT 0x0004
#define SW_XB_DPAD_RIGHT 0x0008
#define SW_XB_START 0x0010
#define SW_XB_BACK 0x0020
#define SW_XB_L3 0x0040
#define SW_XB_R3 0x0080
#define SW_XB_LB 0x0100
#define SW_XB_RB 0x0200
#define SW_XB_A 0x1000
#define SW_XB_B 0x2000
#define SW_XB_X 0x4000
#define SW_XB_Y 0x8000

/* Controller mapping (SW_PADMAP=<file>, written by the launcher's Controls >
 * Controller tab; names must match launcher/controls.py PAD_INPUTS/GEN_INPUTS/PAD_ACTIONS):
 *   DEVICE=any | x0..x3 (XInput slot; plain 0..3 too) | j:VVVV:PPPP[:n] (HID pad by
 *          USB vendor/product id, n-th of that kind) | j0..j15 (winmm id)
 *   A=B,RT          Xbox layout: Switch button = XInput inputs (up to 4)
 *   GA=B3,POV_RIGHT HID layout (G prefix): Switch button = HID inputs (up to 4)
 *   G_LX=X G_LY=Y G_RX=Z G_RY=R   HID axes used as the sticks (X Y Z R U V, - = none)
 *   SWAP_STICKS=0|1  INVERT_LY=0|1  INVERT_RY=0|1  DEADZONE=0..50 (percent)
 * XInput inputs: A B X Y LB RB LT RT START BACK LS RS DUP DDOWN DLEFT DRIGHT.
 * HID inputs: B1..B32, POV_UP/DOWN/LEFT/RIGHT, axis halves X+ X- Y+ ... V+ V-.
 * Default = Switch layout by position (Xbox B / PlayStation Circle -> Switch A). */
enum { PI_A, PI_B, PI_X, PI_Y, PI_LB, PI_RB, PI_LT, PI_RT, PI_START, PI_BACK, PI_LS, PI_RS,
       PI_DUP, PI_DDOWN, PI_DLEFT, PI_DRIGHT, PI_COUNT };
static const char *const pi_names[PI_COUNT] = {
    "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "BACK", "LS", "RS", "DUP", "DDOWN", "DLEFT", "DRIGHT"};
static const WORD pi_bits[PI_COUNT] = {
    SW_XB_A, SW_XB_B, SW_XB_X, SW_XB_Y, SW_XB_LB, SW_XB_RB, 0, 0, SW_XB_START, SW_XB_BACK, SW_XB_L3, SW_XB_R3,
    SW_XB_DPAD_UP, SW_XB_DPAD_DOWN, SW_XB_DPAD_LEFT, SW_XB_DPAD_RIGHT};
/* HID inputs: 0..31 = B1..B32, 32..35 = POV up/down/left/right, 36.. = axis halves */
#define GI_POV 32
#define GI_AXIS 36
#define GI_COUNT (GI_AXIS + 12)
static const char ax_names[6] = {'X', 'Y', 'Z', 'R', 'U', 'V'};
static const char *const pov_names[4] = {"POV_UP", "POV_DOWN", "POV_LEFT", "POV_RIGHT"};

static int gi_parse(const char *s)
{
    if (s[0] == 'B' && s[1] >= '0' && s[1] <= '9') {
        int n = atoi(s + 1);
        return n >= 1 && n <= 32 ? n - 1 : -1;
    }
    for (int i = 0; i < 4; i++)
        if (!strcmp(s, pov_names[i])) return GI_POV + i;
    for (int i = 0; i < 6; i++)
        if (s[0] == ax_names[i] && (s[1] == '+' || s[1] == '-') && !s[2]) return GI_AXIS + i * 2 + (s[1] == '-');
    return -1;
}

/* Switch buttons in SW_BTN bit order 0..17 (Home/Capture unmapped by default on Xbox) */
static const char *const sb_names[18] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "HOME", "CAPTURE", "LSTICK", "RSTICK",
    "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"};
static signed char g_pm[18][4] = {
    {PI_B, -1, -1, -1}, {PI_A, -1, -1, -1}, {PI_Y, -1, -1, -1}, {PI_X, -1, -1, -1},
    {PI_LB, -1, -1, -1}, {PI_RB, -1, -1, -1}, {PI_LT, -1, -1, -1}, {PI_RT, -1, -1, -1},
    {PI_START, -1, -1, -1}, {PI_BACK, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1},
    {PI_LS, -1, -1, -1}, {PI_RS, -1, -1, -1},
    {PI_DUP, -1, -1, -1}, {PI_DDOWN, -1, -1, -1}, {PI_DLEFT, -1, -1, -1}, {PI_DRIGHT, -1, -1, -1}};
/* HID default: the PlayStation (DirectInput) numbering most HID pads use -
 * 1 Square 2 Cross 3 Circle 4 Triangle 5 L1 6 R1 7 L2 8 R2 9 Share 10 Options
 * 11 L3 12 R3 13 PS 14 Touchpad, d-pad = POV */
static signed char g_gm[18][4] = {
    {2, -1, -1, -1}, {1, -1, -1, -1}, {3, -1, -1, -1}, {0, -1, -1, -1},
    {4, -1, -1, -1}, {5, -1, -1, -1}, {6, -1, -1, -1}, {7, -1, -1, -1},
    {9, -1, -1, -1}, {8, -1, -1, -1}, {12, -1, -1, -1}, {13, -1, -1, -1},
    {10, -1, -1, -1}, {11, -1, -1, -1},
    {GI_POV + 0, -1, -1, -1}, {GI_POV + 1, -1, -1, -1}, {GI_POV + 2, -1, -1, -1}, {GI_POV + 3, -1, -1, -1}};
static signed char g_gax[4] = {0, 1, 2, 3}; /* HID axis index for LX LY RX RY (-1 none) */

enum { DEV_ANY, DEV_XINPUT, DEV_JOY_ID, DEV_JOY_VIDPID };
static int g_dev_kind = DEV_ANY, g_dev_slot, g_dev_vid, g_dev_pid, g_dev_nth;
static int g_pm_swap, g_pm_inv_ly, g_pm_inv_ry, g_pm_dead;

static void parse_list(char *val, signed char out[4], int generic)
{
    int n = 0;
    memset(out, -1, 4);
    for (char *tok = strtok(val, ","); tok && n < 4; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        if (generic) {
            int g = gi_parse(tok);
            if (g >= 0) out[n++] = (signed char)g;
            continue;
        }
        for (int i = 0; i < PI_COUNT; i++)
            if (!strcmp(tok, pi_names[i])) { out[n++] = (signed char)i; break; }
    }
}

static void parse_device(const char *v)
{
    unsigned vid, pid; int nth = 0;
    g_dev_kind = DEV_ANY;
    if (!*v || !strcmp(v, "any")) return;
    if (v[0] >= '0' && v[0] <= '3' && !v[1]) { g_dev_kind = DEV_XINPUT; g_dev_slot = v[0] - '0'; return; }
    if (v[0] == 'x' && v[1] >= '0' && v[1] <= '3') { g_dev_kind = DEV_XINPUT; g_dev_slot = v[1] - '0'; return; }
    if (v[0] == 'j' && v[1] == ':' && sscanf(v + 2, "%x:%x:%d", &vid, &pid, &nth) >= 2) {
        g_dev_kind = DEV_JOY_VIDPID; g_dev_vid = (int)vid; g_dev_pid = (int)pid; g_dev_nth = nth < 0 ? 0 : nth;
        return;
    }
    if (v[0] == 'j' && v[1] >= '0' && v[1] <= '9') { g_dev_kind = DEV_JOY_ID; g_dev_slot = atoi(v + 1) & 15; return; }
}

static void padmap_load(void)
{
    static int done;
    if (done) return;
    done = 1;
    const char *path = getenv("SW_PADMAP");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[input] SW_PADMAP: cannot open %s, default mapping\n", path); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = 0;
        if (!strcmp(line, "DEVICE")) { parse_device(val); continue; }
        if (!strcmp(line, "SWAP_STICKS")) { g_pm_swap = atoi(val) != 0; continue; }
        if (!strcmp(line, "INVERT_LY")) { g_pm_inv_ly = atoi(val) != 0; continue; }
        if (!strcmp(line, "INVERT_RY")) { g_pm_inv_ry = atoi(val) != 0; continue; }
        if (!strcmp(line, "DEADZONE")) { g_pm_dead = atoi(val); if (g_pm_dead < 0) g_pm_dead = 0; if (g_pm_dead > 50) g_pm_dead = 50; continue; }
        if (!strncmp(line, "G_", 2)) {
            static const char *const st[4] = {"G_LX", "G_LY", "G_RX", "G_RY"};
            for (int i = 0; i < 4; i++) {
                if (strcmp(line, st[i])) continue;
                g_gax[i] = -1;
                for (int a = 0; a < 6; a++)
                    if (val[0] == ax_names[a]) g_gax[i] = (signed char)a;
            }
            continue;
        }
        int generic = line[0] == 'G';
        const char *name = generic ? line + 1 : line;
        for (int b = 0; b < 18; b++)
            if (!strcmp(name, sb_names[b])) { parse_list(val, generic ? g_gm[b] : g_pm[b], generic); break; }
    }
    fclose(f);
    fprintf(stderr, "[input] controller mapping from %s (device kind %d)\n", path, g_dev_kind);
}

/* ---- winmm HID joysticks ------------------------------------------------ */
typedef struct JoyInfo {
    int present;
    int vid, pid;
    int xinput;               /* an Xbox pad (read through XInput instead) */
    DWORD mn[6], mx[6];       /* axis ranges X Y Z R U V */
    int has[6];
    char name[64];
} JoyInfo;
static JoyInfo g_joy[16];
static int g_joy_scanned;

static void joy_name(int vid, int pid, const char *fallback, char *out, size_t size)
{
    char key[160];
    HKEY h;
    snprintf(key, sizeof key,
             "System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_%04X&PID_%04X",
             vid, pid);
    for (int root = 0; root < 2; root++) {
        if (RegOpenKeyExA(root ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, key, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        DWORD type = 0, len = (DWORD)size - 1;
        LONG rc = RegQueryValueExA(h, "OEMName", NULL, &type, (BYTE *)out, &len);
        RegCloseKey(h);
        if (rc == ERROR_SUCCESS && type == REG_SZ && len > 1) { out[len < size ? len : size - 1] = 0; return; }
    }
    snprintf(out, size, "%s", fallback && *fallback ? fallback : "Game controller");
}

/* Scanning (joyConfigChanged + joyGetDevCaps on 16 ids) can take many
 * milliseconds, so it runs on its own thread every 2 s and publishes the table
 * under a lock; the game thread only reads it (and marks a pad that stopped
 * answering as lost until the next scan). */
static SRWLOCK g_joy_lock = SRWLOCK_INIT;
static volatile LONG g_joy_lost[16];
static volatile LONG g_joy_gen;

static void joy_scan_into(JoyInfo out[16])
{
    UINT n = joyGetNumDevs();
    if (n > 16) n = 16;
    for (UINT id = 0; id < 16; id++) {
        JoyInfo *j = &out[id];
        JOYINFOEX ji;
        JOYCAPSA caps;
        memset(j, 0, sizeof *j);
        if (id >= n) continue;
        memset(&ji, 0, sizeof ji);
        ji.dwSize = sizeof ji;
        ji.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(id, &ji) != JOYERR_NOERROR) continue;
        if (joyGetDevCapsA(id, &caps, sizeof caps) != JOYERR_NOERROR) continue;
        j->present = 1;
        j->vid = caps.wMid;
        j->pid = caps.wPid;
        j->mn[0] = caps.wXmin; j->mx[0] = caps.wXmax; j->has[0] = 1;
        j->mn[1] = caps.wYmin; j->mx[1] = caps.wYmax; j->has[1] = 1;
        j->mn[2] = caps.wZmin; j->mx[2] = caps.wZmax; j->has[2] = (caps.wCaps & JOYCAPS_HASZ) != 0;
        j->mn[3] = caps.wRmin; j->mx[3] = caps.wRmax; j->has[3] = (caps.wCaps & JOYCAPS_HASR) != 0;
        j->mn[4] = caps.wUmin; j->mx[4] = caps.wUmax; j->has[4] = (caps.wCaps & JOYCAPS_HASU) != 0;
        j->mn[5] = caps.wVmin; j->mx[5] = caps.wVmax; j->has[5] = (caps.wCaps & JOYCAPS_HASV) != 0;
        joy_name(j->vid, j->pid, caps.szPname, j->name, sizeof j->name);
        for (size_t k = strlen(j->name); k > 0 && (j->name[k - 1] == ' ' || j->name[k - 1] == '\t'); k--)
            j->name[k - 1] = 0;
        j->xinput = j->vid == 0x045E || strstr(j->name, "XBOX") || strstr(j->name, "Xbox") ||
                    strstr(j->name, "XInput") || strstr(j->name, "XINPUT");
    }
}

static void joy_publish(const JoyInfo t[16])
{
    AcquireSRWLockExclusive(&g_joy_lock);
    for (int id = 0; id < 16; id++) {
        if (t[id].present && (!g_joy[id].present || g_joy[id].vid != t[id].vid || g_joy[id].pid != t[id].pid))
            fprintf(stderr, "[input] HID controller %d: %s (%04X:%04X)%s\n", id, t[id].name, t[id].vid, t[id].pid,
                    t[id].xinput ? " - Xbox pad, read through XInput" : "");
        g_joy[id] = t[id];
        g_joy_lost[id] = 0;
    }
    g_joy_scanned = 1;
    InterlockedIncrement(&g_joy_gen);
    ReleaseSRWLockExclusive(&g_joy_lock);
}

static DWORD WINAPI joy_thread(LPVOID arg)
{
    static JoyInfo t[16];
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (unsigned tick = 0;; tick++) {
        if (tick) joyConfigChanged(0);   /* lets winmm see pads connected / paired after start-up */
        joy_scan_into(t);
        joy_publish(t);
        Sleep(2000);
    }
}

static void joy_start(void)
{
    static volatile LONG started;
    if (started || InterlockedExchange(&started, 1)) return;
    HANDLE h = CreateThread(NULL, 64 * 1024, joy_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

/* the winmm id to read, or -1 */
static int joy_pick(void)
{
    if (g_dev_kind == DEV_JOY_ID) return g_joy[g_dev_slot].present && !g_joy_lost[g_dev_slot] ? g_dev_slot : -1;
    int nth = 0;
    for (int id = 0; id < 16; id++) {
        const JoyInfo *j = &g_joy[id];
        if (!j->present || g_joy_lost[id]) continue;
        if (g_dev_kind == DEV_JOY_VIDPID) {
            if (j->vid == g_dev_vid && j->pid == g_dev_pid && nth++ == g_dev_nth) return id;
        } else if (!j->xinput) return id;   /* any: the first non-Xbox HID pad */
    }
    return -1;
}

static SHORT joy_axis(const JoyInfo *j, int a, DWORD raw)
{
    double lo = (double)j->mn[a], hi = (double)j->mx[a];
    if (hi <= lo) return 0;
    double v = ((double)raw - lo) / (hi - lo) * 2.0 - 1.0;   /* -1..1 */
    if (v < -1) v = -1;
    if (v > 1) v = 1;
    return (SHORT)(v * 32767.0);
}

/* read winmm joystick id: 1 = ok (pressed[] over GI_COUNT, sticks raw) */
static int joy_read(int id, int pressed[GI_COUNT], SHORT ax[6])
{
    JOYINFOEX ji;
    memset(&ji, 0, sizeof ji);
    ji.dwSize = sizeof ji;
    ji.dwFlags = JOY_RETURNALL | JOY_RETURNPOVCTS;
    if (joyGetPosEx((UINT)id, &ji) != JOYERR_NOERROR) return 0;
    const JoyInfo *j = &g_joy[id];
    DWORD raw[6] = {ji.dwXpos, ji.dwYpos, ji.dwZpos, ji.dwRpos, ji.dwUpos, ji.dwVpos};
    for (int i = 0; i < 32; i++) pressed[i] = (ji.dwButtons >> i) & 1;
    DWORD pov = ji.dwPOV;
    int c = pov == JOY_POVCENTERED || pov > 36000;
    pressed[GI_POV + 0] = !c && (pov > 27000 || pov < 9000);
    pressed[GI_POV + 1] = !c && pov > 9000 && pov < 27000;
    pressed[GI_POV + 2] = !c && pov > 18000;
    pressed[GI_POV + 3] = !c && pov > 0 && pov < 18000;
    for (int a = 0; a < 6; a++) {
        ax[a] = j->has[a] ? joy_axis(j, a, raw[a]) : 0;
        pressed[GI_AXIS + a * 2] = ax[a] > 16384;
        pressed[GI_AXIS + a * 2 + 1] = ax[a] < -16384;
    }
    return 1;
}

static SHORT dead(SHORT v)
{
    int lim = g_pm_dead * 32767 / 100;
    if (v > -lim && v < lim) return 0;
    return v;
}
static SHORT inv(SHORT v) { return v == -32768 ? 32767 : (SHORT)-v; }

/* swap: Xbox pads only (a HID pad's G_* axis lines already say which stick is which) */
static void finish_sticks(SwPadState *out, SHORT lx, SHORT ly, SHORT rx, SHORT ry, int swap)
{
    if (swap) { SHORT t = lx; lx = rx; rx = t; t = ly; ly = ry; ry = t; }
    if (g_pm_inv_ly) ly = inv(ly);
    if (g_pm_inv_ry) ry = inv(ry);
    out->lx = dead(lx); out->ly = dead(ly); out->rx = dead(rx); out->ry = dead(ry);
}

static int poll_xinput(SwPadState *out, int any)
{
    SwXinputState state;
    DWORD rc = 1;
    if (!g_xinput_get_state) return 0;
    memset(&state, 0, sizeof(state));
    if (!any) rc = g_xinput_get_state((DWORD)g_dev_slot, &state);
    else {
        /* any: the first connected slot; re-probe the others only now and then
         * (XInputGetState on an empty slot is slow) */
        static DWORD slot; static unsigned miss;
        rc = g_xinput_get_state(slot, &state);
        if (rc != 0 && (++miss & 63) == 1)
            for (DWORD i = 0; i < 4; i++)
                if (i != slot && g_xinput_get_state(i, &state) == 0) { slot = i; rc = 0; break; }
    }
    if (rc != 0) return 0; /* ERROR_SUCCESS == 0; not connected otherwise */
    out->connected = 1;
    WORD w = state.Gamepad.wButtons;
    int pressed[PI_COUNT];
    for (int i = 0; i < PI_COUNT; i++) pressed[i] = (w & pi_bits[i]) != 0;
    pressed[PI_LT] = state.Gamepad.bLeftTrigger > 64;
    pressed[PI_RT] = state.Gamepad.bRightTrigger > 64;
    for (int b = 0; b < 18; b++)
        for (int k = 0; k < 4; k++)
            if (g_pm[b][k] >= 0 && pressed[(int)g_pm[b][k]]) { out->buttons |= 1u << b; break; }
    finish_sticks(out, state.Gamepad.sThumbLX, state.Gamepad.sThumbLY, state.Gamepad.sThumbRX, state.Gamepad.sThumbRY,
                  g_pm_swap);
    return 1;
}

static int poll_joy(SwPadState *out)
{
    static int cur = -1;
    static LONG gen = -1;
    int pressed[GI_COUNT];
    SHORT ax[6];
    int ok = 0;
    joy_start();
    AcquireSRWLockShared(&g_joy_lock);     /* held by a publish only for a 16-entry copy */
    if (!g_joy_scanned) { ReleaseSRWLockShared(&g_joy_lock); return 0; }
    if (gen != g_joy_gen) { gen = g_joy_gen; cur = -1; }   /* new table: pick again */
    if (cur >= 0) {
        ok = joy_read(cur, pressed, ax);
        if (!ok) { g_joy_lost[cur] = 1; cur = -1; }
    }
    if (!ok && (cur = joy_pick()) >= 0) {
        ok = joy_read(cur, pressed, ax);
        if (!ok) { g_joy_lost[cur] = 1; cur = -1; }
    }
    if (ok) {
        out->connected = 1;
        for (int b = 0; b < 18; b++)
            for (int k = 0; k < 4; k++)
                if (g_gm[b][k] >= 0 && pressed[(int)g_gm[b][k]]) { out->buttons |= 1u << b; break; }
        SHORT s[4];
        for (int i = 0; i < 4; i++) s[i] = g_gax[i] >= 0 ? ax[(int)g_gax[i]] : 0;
        /* HID Y axes grow downwards; the Switch's (and XInput's) grow upwards */
        finish_sticks(out, s[0], inv(s[1]), s[2], inv(s[3]), 0);
    }
    ReleaseSRWLockShared(&g_joy_lock);
    return ok;
}
#endif

int sw_input_poll(SwPadState *out)
{
    memset(out, 0, sizeof(*out));
#if defined(_WIN32)
    load_xinput();
    padmap_load();
    switch (g_dev_kind) {
    case DEV_XINPUT: return poll_xinput(out, 0);
    case DEV_JOY_ID:
    case DEV_JOY_VIDPID: return poll_joy(out);
    default: {
        /* any: whichever kind answered last, so a HID pad is not probed
         * against empty XInput slots every frame (and the reverse) */
        static int prefer_joy;
        if (prefer_joy) {
            if (poll_joy(out)) return 1;
            memset(out, 0, sizeof *out);
            if (poll_xinput(out, 1)) { prefer_joy = 0; return 1; }
        } else {
            if (poll_xinput(out, 1)) return 1;
            memset(out, 0, sizeof *out);
            if (poll_joy(out)) { prefer_joy = 1; return 1; }
        }
        memset(out, 0, sizeof *out);
        return 0;
    }
    }
#else
    (void)out;
    return 0;
#endif
}

const char *sw_input_button_name(unsigned bit)
{
    switch (bit) {
    case 0: return "A"; case 1: return "B"; case 2: return "X"; case 3: return "Y";
    case 4: return "L"; case 5: return "R"; case 6: return "ZL"; case 7: return "ZR";
    case 8: return "Plus"; case 9: return "Minus"; case 10: return "Home";
    case 11: return "Capture"; case 12: return "L3"; case 13: return "R3";
    case 14: return "Up"; case 15: return "Down"; case 16: return "Left"; case 17: return "Right";
    default: return "?";
    }
}

void sw_input_describe(SwPadState *pad, char *buf, size_t size)
{
    size_t used = 0;
    unsigned i;
    if (!pad->connected) {
        snprintf(buf, size, "no controller");
        return;
    }
    buf[0] = 0;
    for (i = 0; i < 18; i++) {
        if (pad->buttons & (1u << i)) {
            int n = snprintf(buf + used, size - used, "%s%s",
                             used ? "+" : "", sw_input_button_name(i));
            if (n <= 0) break;
            used += (size_t)n;
            if (used >= size) break;
        }
    }
    if (!buf[0]) snprintf(buf, size, "(no buttons)");
}
