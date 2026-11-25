/* Settings of the graphics layer: environment variables over a defaults table.
 * The table is the settings table of totk-pc-recomp runtime/cfg.c (MIT,
 * Copyright (c) 2026 Valiantsin Dzerakh), with this port's defaults where
 * they differ (marked [port]). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"

#define LIVE 1
#define RESTART 0
static const CfgMeta metas[] = {
    {"FULLSCREEN", "0", "0|1", LIVE}, {"WINDOW_SIZE", "1280x720", NULL, LIVE},            /* [port] 1280x720 */
    {"VK_PRESENT", "fifo", "mailbox|fifo|immediate", LIVE}, {"GPU_INDEX", "", NULL, RESTART},  /* [port] fifo */
    {"PERF_OVERLAY", "off", "off|fps|full", LIVE},
    {"OUTPUT_RES", "1280x720", NULL, RESTART}, {"RENDER_SCALE", "game", "game|50|67|75|100|125|150|200", RESTART},
    {"UPSCALER", "bilinear", "fsr|bilinear", LIVE}, {"SHARPNESS", "80", "0..100", LIVE},   /* [port] no FSR */
    {"DLAA", "0", "0|1", RESTART}, {"DLSS", "dlaa", "dlaa|quality|balanced|performance|ultra_performance", LIVE},
    {"DLSS_PRESET", "default", "default|k|j", LIVE},
    {"DLSS_CHAR_MASK", "65", "0|35|65|100", LIVE}, {"FXAA", "0", "0|1", LIVE},             /* [port] no FXAA */
    {"ASPECT", "fit", "fit|stretch", LIVE}, {"FOV", "100", "70..140", LIVE},
    {"ANISO", "game", "game|2|4|8|16", RESTART},
    {"COLOR_PRESET", "default", "default|vivid|clarity|custom", LIVE}, {"COLOR_BRIGHTNESS", "0", "-30..30", LIVE},
    {"COLOR_CONTRAST", "100", "70..150", LIVE}, {"COLOR_SATURATION", "100", "50..200", LIVE},
    {"COLOR_GAMMA", "1", "0.8..1.2", LIVE},
    {"HDR", "0", "0|1", RESTART}, {"HDR_PEAK_NITS", "1000", "400..10000", LIVE}, {"HDR_GAME_NITS", "203", "80..500", LIVE},
    {"HDR_UI_NITS", "203", "80..500", LIVE}, {"HDR_HUE", "80", "0..100", LIVE},
    {"DEVICE_PROFILE", "auto", "auto|desktop|deck", RESTART},
    {"SHADOW_RES", "game", "game|256|512|768|1024|1536|2048", RESTART},
    {"SHADOW_SCALE", "1", "1..4", RESTART}, {"RES_SCALE", "1", "1..4", RESTART},   /* [port] render resolution (vk_texture.c) */
    {"SHADOW_RANGE", "100", "50..400", RESTART}, {"CONTACT_SHADOWS", "0", "0..100", RESTART},
    {"PCSS", "0", "0..200", RESTART},
    {"FPS_LIMIT", "60", "30|40|45|60|90|120|144|display", LIVE}, {"FPS60", "0", "0|1", LIVE},
    {"REALTIME", "0", "0|1", RESTART}, {"CLOTH_REALTIME", "0", "0|1", RESTART},
    {"MODS_REBUILD", "off", "auto|off", RESTART},
    {"HIGHFPS_FIX", "0", "0|1", RESTART},
    {"LANG", "", "ja|en-US|fr|de|it|es|zh-CN|ko|nl|pt|ru|zh-TW|en-GB|fr-CA|es-419|zh-Hans|zh-Hant|pt-BR", RESTART},
    {"OPERATION_MODE", "docked", "docked|handheld", RESTART},
    {"ASSUME_FOCUS", "1", "0|1", LIVE},
    {"VOLUME", "100", "0..100", LIVE}, {"AUDIO_CHANNELS", "stereo", "auto|stereo|5.1", RESTART},
    {"MOTION", "1", "0|1", LIVE}, {"RUMBLE_STRENGTH", "130", "0..300", LIVE}, {"RUMBLE_CURVE", "0.6", "0.2..1.5", LIVE},
    {"RUMBLE_TRIGGERS", "0", "0|1", LIVE}, {"PAD_LAYOUT", "auto", "auto|nintendo|xbox", LIVE},
    {"PROMPTS", "nintendo", "nintendo|xbox", RESTART},
    {"STICK_DEADZONE", "8", "0..50", LIVE}, {"STICK_SENS", "1", "0.1..3", RESTART},
    {"LSTICK_DEADZONE", "", "0..50", LIVE}, {"RSTICK_DEADZONE", "", "0..50", LIVE},
    {"MOUSE_SENS", "1", "0..10", LIVE},
    {"WALK_KEY", "none", NULL, LIVE}, {"WALK_SCALE", "0.51", "0.2..0.9", LIVE},
    {"MENU_LANG", "", "en|ru|pt-BR|zh-Hans", LIVE},
    {"AMIIBO_DIR", "", NULL, LIVE},
    {"DISCORD_RPC", "0", "0|1", LIVE},
    {"DISCORD_APP_ID", "", NULL, LIVE},
    {"DISCORD_IMAGE", "", NULL, LIVE},
    {"ASYNC_PREEMPT", "0", "0|1|2", RESTART}, {"REPLAY_ASYNC", "0", "0|1", RESTART},     /* [port] synchronous replay */
    {"FREE_CORES", "0", "0|1", RESTART}, {"TIMER_1MS", "1", "0|1", RESTART},
    {"PIPE_WAIT", "default", "default|draw", RESTART}, {"PIPE_SKIP", "strict", "strict|any|fullscreen", RESTART},   /* [port] never skip: correctness first */
    {"LIB_PREBUILD", "1", "0|1", RESTART}, {"LIB_PREBUILD_THREADS", "", "1..64", RESTART},
    {"PIPE_PREBUILD", "0", "0|1", RESTART}, {"PREWARM_XLATE", "", "1..64", RESTART}, {"GPL", "1", "0|1", RESTART},
    {"ASTC", "auto", "auto|gpu|native|cpu", RESTART}, {"MEMORY", "auto", "auto|import|mirror|verify", RESTART},
    {"TEX_ALIAS", "1", "0|1", RESTART}, {"TEX_VRAM_PRESSURE", "90", "0..100", RESTART}, {"ASTC_BC", "", "0|1", RESTART},
    {"ZSTD_NATIVE", "0", "0|1", RESTART}, {"ZSTD_CHECK", "0", "0|1", RESTART},
    {"COPY_CLAMP", "1", "0|1", RESTART},   /* [port] clamp out-of-range copy regions (see port_gfx.py) */
    {"LOD_GUARD", "0", "0|1", RESTART}, {"FIX_C0", "1", "0|1", RESTART}, {"FIX_ZERO", "1", "0|1", RESTART},
    {"BIND_SKIP", "1", "0|1", RESTART}, {"SHADER_MEMO", "1", "0|1", RESTART}, {"PIPE_MEMO", "1", "0|1", RESTART},
    {"TEX_MEMO", "1", "0|1", RESTART},
    {"DESC_TEMPLATE", "1", "0|1", RESTART},
    {"PUSH_SKIP", "1", "0|1", RESTART}, {"SPLIT_DRAWS", "64", "0..1000", RESTART},
    {"SPLIT_US", "1000", "0..20000", RESTART},
    {"VK_VALIDATION", "0", "0|1", RESTART}, {"VK_DYN_VERTEX", "", "0|1", RESTART},
    {"VK_DYN_A2C", "", "0|1", RESTART}, {"VK_SUBGROUP_PIN", "1", "0|1", RESTART},
    {"TRACE", "", NULL, RESTART}, {"FLIGHT", "0", "0|1", RESTART}, {"FLIGHT_SPIKE_MS", "100", "0..60000", RESTART},
    {"WB_FENCE_LIMIT", "65536", "0..1073741824", RESTART}, {"WB_EXACT", "1", "0|1", RESTART},
    {"PASS_CRUMBS", "", "0|1", RESTART},
    {"INPUT_RECORD", "", NULL, RESTART},
};

static const char *strip(const char *name) { return !strncmp(name, "TOTK_", 5) ? name + 5 : name; }

/* cfg_set: values changed while the game runs (in-game menu), over the environment.  Each set makes a new string and
 * the old one is kept, so a pointer a reader already has stays valid; the table is small and only grows. */
static struct { char key[32]; const char *volatile val; } g_over[32];
static volatile LONG g_nover;
static volatile LONG g_cfg_version;   /* bumped by every cfg_set: readers re-read their values (vk_color.c, gfx_present.c) */
unsigned cfg_version(void) { return (unsigned)g_cfg_version; }
static SRWLOCK g_over_lock = SRWLOCK_INIT;

void cfg_set(const char *name, const char *value)
{
    const char *n = strip(name);
    size_t len = strlen(value ? value : "") + 1;
    char *v = (char *)malloc(len);
    if (!v) return;
    memcpy(v, value ? value : "", len);
    AcquireSRWLockExclusive(&g_over_lock);
    int i = 0;
    while (i < g_nover && _stricmp(g_over[i].key, n)) i++;
    if (i == g_nover && i < 32) {
        _snprintf(g_over[i].key, sizeof g_over[i].key, "%s", n);
        g_over[i].key[sizeof g_over[i].key - 1] = 0;
        g_over[i].val = v;
        MemoryBarrier();
        InterlockedIncrement(&g_nover);
    } else if (i < 32) {
        g_over[i].val = v;
    }
    ReleaseSRWLockExclusive(&g_over_lock);
    InterlockedIncrement(&g_cfg_version);
}
int cfg_save(void) { return 1; }   /* the runtime keeps its own settings file (sw_romfs.c sw_ingame_set) */

const char *cfg_get(const char *name)
{
    char buf[128];
    const char *n = strip(name);
    for (LONG i = 0, k = g_nover; i < k; i++)
        if (!_stricmp(g_over[i].key, n)) { const char *o = g_over[i].val; return o && *o ? o : NULL; }
    _snprintf(buf, sizeof buf, "SW_%s", n);
    buf[sizeof buf - 1] = 0;
    const char *v = getenv(buf);
    if (!v) {
        _snprintf(buf, sizeof buf, "TOTK_%s", n);
        buf[sizeof buf - 1] = 0;
        v = getenv(buf);
    }
    return v && *v ? v : NULL;
}

const CfgMeta *cfg_meta(const char *key)
{
    const char *k = strip(key);
    for (size_t i = 0; i < sizeof metas / sizeof metas[0]; i++)
        if (!_stricmp(metas[i].key, k)) return &metas[i];
    return NULL;
}

const CfgMeta *cfg_meta_at(int i)
{
    return i >= 0 && (size_t)i < sizeof metas / sizeof metas[0] ? &metas[i] : NULL;
}

const char *cfg_value(const char *name)
{
    const char *v = cfg_get(name);
    if (v) return v;
    const CfgMeta *m = cfg_meta(name);
    return m ? m->def : "";
}

int cfg_int(const char *name)
{
    const char *v = cfg_value(name);
    char *end;
    long n = strtol(v, &end, 10);
    if (*v && !*end) return (int)n;
    const CfgMeta *m = cfg_meta(name);
    return m ? atoi(m->def) : 0;
}

double cfg_double(const char *name)
{
    const char *v = cfg_value(name);
    char *end;
    double d = strtod(v, &end);
    if (*v && !*end) return d;
    const CfgMeta *m = cfg_meta(name);
    return m ? atof(m->def) : 0.0;
}

int cfg_bool(const char *name)
{
    const char *v = cfg_value(name);
    return *v && strcmp(v, "0") != 0;
}
