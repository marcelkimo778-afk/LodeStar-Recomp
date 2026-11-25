/* Mod support test (Engine/Tests/test_mods.bat): sw_mods.c + sw_romfs.c against stubs, no game needed.
 *
 *   test_mods <fixture> <tmp>          full test (mods from <fixture>/mods, config <fixture>/mods.json)
 *   test_mods <fixture> <tmp> nomods   SW_NO_MODS=1: the image must be the plain base romfs
 *
 * The RomFS image sw_romfs.c synthesises is read back the way the game's SDK reads it (header, hash buckets,
 * directory/file entries, data offsets), so the checks see exactly what the guest would see. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../Runtime/src/sw_mods.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

u64 sw_romfs_read(u64 off, void *buf, u64 size);
u64 sw_romfs_size(void);
int sw_romfs_ready(void);

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- stubs for the runtime ---------------- */
int sw_newfn_mode;
double sw_newfn_time(void) { return 0; }
volatile uint64_t sw_hid_buttons_held;
static char g_tmp[1024];

void sw_report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
const char *sw_mods_host_exe_dir(void) { return g_tmp; }

#define BID "aabbccdd11223344556677889900aabbccddeeff000000000000000000000000"
#define MAIN 0x100000ull
#define HEAP 0x80000000ull
static u8 g_main[0x4000];      /* text 0x1000, rodata 0x1000, data 0x1000, (bss) */
static u8 g_heap[0x1000];

int sw_mods_host_module_count(void) { return 2; }
int sw_mods_host_module(int i, SwModuleInfo *o)
{
    if (i == 0) { o->name = "main"; o->base = MAIN; o->text_size = 0xF80; o->ro_size = 0x1000; o->data_size = 0x800;
                  o->bss_size = 0x100; o->module_id = BID; return 1; }
    if (i == 1) { o->name = "imports"; o->base = 0x9000000; o->text_size = 0; o->ro_size = 0; o->data_size = 0;
                  o->bss_size = 0; o->module_id = "0000000000000000000000000000000000000000000000000000000000000000"; return 1; }
    return 0;
}
const char *sw_mods_host_function_at(u64 addr, u64 *start)
{
    if (addr < MAIN || addr >= MAIN + 0xF80) return NULL;
    *start = addr >= MAIN + 0x800 ? MAIN + 0x800 : MAIN;
    return addr >= MAIN + 0x800 ? "sw_f_00000800" : "sw_f_00000000";
}
static u8 *gmem(u64 a, u64 n)
{
    if (a >= MAIN && a + n <= MAIN + sizeof g_main) return g_main + (a - MAIN);
    if (a >= HEAP && a + n <= HEAP + sizeof g_heap) return g_heap + (a - HEAP);
    return NULL;
}
int sw_guest_read(u64 a, void *b, u64 n) { u8 *p = gmem(a, n); if (!p) return 0; memcpy(b, p, (size_t)n); return 1; }
int sw_guest_write(u64 a, const void *b, u64 n) { u8 *p = gmem(a, n); if (!p) return 0; memcpy(p, b, (size_t)n); return 1; }
u64 sw_guest_region_base(int k) { return k == 0 ? MAIN : k == 1 ? HEAP : k == 2 ? 0x1000000000ull : 0x8000000ull; }
void vk_texture_shadow_set(int scale) { (void)scale; }   /* sw_romfs.c sw_shadow_set: live shadow size */
const char *sw_hooks_fps_conflict(u64 fn) { return fn == MAIN + 0x800 ? "frame step (test)" : NULL; }
const char *sw_hooks_fps_data_conflict(u64 lo, u64 hi) { return lo < MAIN + 0x1104 && hi > MAIN + 0x1100 ? "the test frame table" : NULL; }

/* ---------------- RomFS reader (as the guest SDK reads the image) ---------------- */
static u8 *g_img_head;
static u64 g_dh, g_dhs, g_dm, g_fh, g_fhs, g_fm, g_data;

static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 rd64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }

static void img_open(void)
{
    u8 h[0x50];
    sw_romfs_read(0, h, sizeof h);
    g_dh = rd64(h + 0x08); g_dhs = rd64(h + 0x10); g_dm = rd64(h + 0x18);
    g_fh = rd64(h + 0x28); g_fhs = rd64(h + 0x30); g_fm = rd64(h + 0x38); g_data = rd64(h + 0x48);
    free(g_img_head);
    g_img_head = (u8 *)malloc((size_t)g_data);
    sw_romfs_read(0, g_img_head, g_data);
}

static u32 rhash(u32 parent, const char *name, size_t n)
{
    u32 h = parent ^ 123456789u;
    for (size_t i = 0; i < n; i++) { h = (h >> 5) | (h << 27); h ^= (u8)name[i]; }
    return h;
}

/* directory entry offset for path ("" = root), or -1 */
static long long find_dir(const char *path)
{
    u32 cur = 0;
    const char *p = path;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        u32 b = rhash(cur, p, n) % (u32)(g_dhs / 4);
        u32 off = rd32(g_img_head + g_dh + b * 4);
        while (off != 0xFFFFFFFFu) {
            const u8 *d = g_img_head + g_dm + off;
            if (rd32(d) == cur && rd32(d + 0x14) == n && !memcmp(d + 0x18, p, n)) break;
            off = rd32(d + 0x10);
        }
        if (off == 0xFFFFFFFFu) return -1;
        cur = off;
        p += n + (e ? 1 : 0);
    }
    return cur;
}

/* file: data offset/size by full path; 0 = not found */
static int find_file(const char *path, u64 *doff, u64 *size)
{
    char dir[512];
    const char *sl = strrchr(path, '/');
    const char *name = sl ? sl + 1 : path;
    snprintf(dir, sizeof dir, "%.*s", sl ? (int)(sl - path) : 0, path);
    long long d = find_dir(dir);
    if (d < 0) return 0;
    size_t n = strlen(name);
    u32 b = rhash((u32)d, name, n) % (u32)(g_fhs / 4);
    u32 off = rd32(g_img_head + g_fh + b * 4);
    while (off != 0xFFFFFFFFu) {
        const u8 *f = g_img_head + g_fm + off;
        if (rd32(f) == (u32)d && rd32(f + 0x1C) == n && !memcmp(f + 0x20, name, n)) {
            *doff = rd64(f + 0x08);
            *size = rd64(f + 0x10);
            return 1;
        }
        off = rd32(f + 0x18);
    }
    return 0;
}

static int read_file(const char *path, char *out, size_t cap)
{
    u64 o, s;
    if (!find_file(path, &o, &s)) return -1;
    if (s >= cap) s = cap - 1;
    sw_romfs_read(g_data + o, out, s);
    out[s] = 0;
    return (int)s;
}

/* names in a directory (files then dirs, in the image's sibling order), comma separated */
static void list_dir(const char *path, char *out, size_t cap)
{
    out[0] = 0;
    long long d = find_dir(path);
    if (d < 0) { snprintf(out, cap, "<missing>"); return; }
    const u8 *de = g_img_head + g_dm + d;
    for (u32 f = rd32(de + 0x0C); f != 0xFFFFFFFFu; f = rd32(g_img_head + g_fm + f + 0x04)) {
        const u8 *fe = g_img_head + g_fm + f;
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%.*s", l ? "," : "", (int)rd32(fe + 0x1C), (const char *)fe + 0x20);
    }
    for (u32 c = rd32(de + 0x08); c != 0xFFFFFFFFu; c = rd32(g_img_head + g_dm + c + 0x04)) {
        const u8 *ce = g_img_head + g_dm + c;
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%.*s/", l ? "," : "", (int)rd32(ce + 0x14), (const char *)ce + 0x18);
    }
}

static char *g_log;
static int log_has(const char *s) { return g_log && strstr(g_log, s) != NULL; }

static void load_log(void)
{
    char p[1100];
    snprintf(p, sizeof p, "%s/mods.log", g_tmp);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_log = (char *)calloc(1, (size_t)n + 1);
    fread(g_log, 1, (size_t)n, f);
    fclose(f);
}

static void expect_file(const char *path, const char *want)
{
    char buf[256];
    int n = read_file(path, buf, sizeof buf);
    if (!want) { CHECK(n < 0, "%s should not exist (got \"%s\")", path, n >= 0 ? buf : ""); return; }
    CHECK(n >= 0 && !strcmp(buf, want), "%s: want \"%s\", got %s%s%s", path, want, n < 0 ? "<missing>" : "\"",
          n < 0 ? "" : buf, n < 0 ? "" : "\"");
}

static u32 heap32(u32 off) { u32 v; memcpy(&v, g_heap + off, 4); return v; }

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: test_mods <fixture> <tmp> [nomods]\n"); return 2; }
    const char *fx = argv[1];
    snprintf(g_tmp, sizeof g_tmp, "%s", argv[2]);
    int nomods = argc > 3 && !strcmp(argv[3], "nomods");
    int badcfg = argc > 3 && !strcmp(argv[3], "badcfg");
    char p[1100];
    CreateDirectoryA(g_tmp, NULL);
    snprintf(p, sizeof p, "SW_ROMFS=%s/base_romfs", fx); _putenv(p);
    snprintf(p, sizeof p, "SW_MODS_DIR=%s/mods", fx); _putenv(p);
    snprintf(p, sizeof p, "SW_MODS_CONFIG=%s/%s", fx, badcfg ? "broken.json" : "mods.json"); _putenv(p);
    _putenv("SW_NO_DIALOG=1");
    if (nomods) _putenv("SW_NO_MODS=1");
    /* original game bytes: a movz w0,#30 at main+0x800 (the function the pchtxt patches), data 0 */
    u32 movz30 = 0x528003C0u, word10 = 0x11111111u;
    memcpy(g_main + 0x800, &movz30, 4);
    memcpy(g_main + 0x10, &word10, 4);

    char *argv2[] = {"test_mods", NULL};
    sw_mods_cli(1, argv2);
    sw_mods_init();
    sw_mods_apply_exefs();
    CHECK(sw_romfs_ready() == 1, "romfs image not built");
    img_open();

    if (nomods) {
        CHECK(sw_mods_romfs_count() == 0, "no-mods: overlay must be empty (zero cost)");
        expect_file("bin/field/a.bin", "BASE_A");
        expect_file("bin/field/new_alpha.bin", NULL);
        expect_file("bin/data/c.bin", "BASE_C");
        u32 v; memcpy(&v, g_main + 0x1000, 4);
        CHECK(v == 0, "no-mods: exefs patch applied");
        CHECK(sw_mods_cheat_tick() == 0, "no-mods: cheat VM must not run");
        printf("no-mods: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    load_log();
    CHECK(g_log != NULL, "mods.log not written");
    if (badcfg) {
        /* broken config: reported in mods.log (logged before the log file opened), ignored; defaults = every mod
         * on, priority by name, so ModAlpha now wins c.bin and no cheat runs */
        CHECK(log_has("broken.json line") && log_has("the mod config is ignored"), "broken config not in mods.log");
        expect_file("bin/data/c.bin", "ALPHA_C");
        expect_file("bin/field/a.bin", "ALPHA_A_REPLACED_LONGER");
        expect_file("bin/field/off.bin", NULL);
        CHECK(sw_mods_cheat_tick() == 0, "cheats ran without being switched on");
        printf("bad config: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    /* ---- romfs ---- */
    expect_file("bin/field/a.bin", "ALPHA_A_REPLACED_LONGER");    /* replacement, size from the mod file */
    {
        u64 o, s;
        CHECK(find_file("bin/field/a.bin", &o, &s) && s == 23, "a.bin size %llu, want 23", (unsigned long long)s);
        char part[8] = {0};                                     /* partial read at an offset */
        sw_romfs_read(g_data + o + 6, part, 5);
        CHECK(!memcmp(part, "A_REP", 5), "partial read at +6: \"%.5s\"", part);
    }
    expect_file("bin/field/new_alpha.bin", "NEW_ALPHA");           /* new file */
    expect_file("bin/data/list/y.txt", "ALPHA_Y");
    expect_file("bin/newdir/deep/z.bin", "ALPHA_Z");               /* new folders */
    expect_file("bin/data/c.bin", "BETA_C_WINS");                  /* conflict: priority from mods.json */
    expect_file("bin/field/b.bin", "BETA_B_CASE");                 /* mod spelled it Field/B.BIN */
    expect_file("bin/field/B.BIN", NULL);                          /* ...and the game's spelling is kept */
    expect_file("bin/data/list/x.txt", "BASE_X");                  /* empty mod file: original kept */
    expect_file("bin/field/empty.bin", NULL);
    expect_file("bin/field/yuzu.bin", "YUZU");                     /* yuzu load/<TID>/<Mod> */
    expect_file("bin/field/ryu.bin", "RYU");                       /* Ryujinx mods/contents/<TID>/<Mod> */
    expect_file("bin/field/atmo.bin", "ATMO");                     /* Atmosphere contents/<TID> */
    expect_file("bin/field/off.bin", NULL);                        /* .disabled marker */
    expect_file("bin/field/other.bin", NULL);                      /* another game's title id */
    expect_file("bin/escape/secret.bin", NULL);                    /* junction out of the mod folder */
    expect_file("system_resource/shader.bin", "SHADER");
    {
        char l[512];
        list_dir("bin/data/list", l, sizeof l);
        CHECK(!strcmp(l, "x.txt,y.txt"), "bin/data/list lists \"%s\"", l);
        list_dir("bin", l, sizeof l);
        CHECK(!strcmp(l, "data/,field/,newdir/"), "bin lists \"%s\"", l);
        list_dir("bin/field", l, sizeof l);
        CHECK(!strcmp(l, "a.bin,atmo.bin,b.bin,new_alpha.bin,ryu.bin,yuzu.bin"), "bin/field lists \"%s\"", l);
    }
    CHECK(log_has("conflict: romfs/bin/data/c.bin is provided by ModBeta (priority 1, used), ModAlpha (priority 2, not used)"),
          "conflict not logged");
    CHECK(log_has("link/junction"), "junction not reported");
    CHECK(log_has("romfs/field/ is not a folder of the game's RomFS"), "misplaced romfs folder not reported");
    CHECK(log_has("mods for another game"), "other title not reported");
    CHECK(log_has("ModOff [DISABLED]: .disabled file"), "disabled mod not in the summary");
    CHECK(log_has("==== mod summary"), "no summary");

    /* ---- exefs ---- */
    CHECK(!memcmp(g_main + 0x1000, "RODA", 4), "IPS .rodata patch not applied");
    CHECK(!memcmp(g_main + 0x1010, "\xDE\xAD\xBE\xEF", 4), "pchtxt data patch (higher priority) not on top");
    CHECK(!memcmp(g_main + 0x1020, "hi\0", 3), "pchtxt string patch");
    CHECK(!memcmp(g_main + 0x1030, "\0\0\0\0", 4), "@disabled pchtxt block applied");
    CHECK(!memcmp(g_main + 0x1040, "\0\0\0\0", 4), "pchtxt line after @stop applied");
    CHECK(!memcmp(g_main + 0x1100, "IP32", 4), "IPS32 patch not applied");
    CHECK(!memcmp(g_main + 0x1200, "\0\0\0\0", 4), "IPS for another build applied");
    {
        u8 rle[8]; memset(rle, 0x5A, 8);
        CHECK(!memcmp(g_main + 0x2000, rle, 8), "IPS RLE .data patch not applied");
    }
    CHECK(!memcmp(g_main + 0x2010, "PSET", 4), "atmosphere exefs_patches IPS not applied");
    {
        u32 v; memcpy(&v, g_main + 0x10, 4);
        CHECK(v == word10, "IPS .text patch was written (must be skipped)");
        memcpy(&v, g_main + 0x800, 4);
        CHECK(v == movz30, "pchtxt .text patch was written (must be skipped)");
    }
    CHECK(log_has("code patch at main+0x800") && log_has("SKIPPED"), "code patch warning");
    CHECK(log_has("movz w0, #0x1e, lsl #0 -> movz w0, #0x3c, lsl #0"), "code patch decode");
    CHECK(log_has("could be supported via a codegen hook"), "constant-change hint");
    CHECK(log_has("looks like a 60 FPS / camera patch"), "FPS incompatibility warning");
    CHECK(log_has("instructions removed (NOP)"), "NOP patch classification");
    CHECK(log_has("another game version"), "build id mismatch warning");
    CHECK(log_has("which the built-in FPS modes rely on"), "FPS data warning");
    CHECK(log_has("the patch of ModBeta (priority 1, used) overwrites the patch of ModAlpha"), "patch overlap not logged");

    /* ---- cheats ---- */
    CHECK(log_has("cheat: ModBeta: \"Not Enabled\" (1 opcodes) - off"), "disabled cheat listed");
    CHECK(log_has("cheat \"Broken\": opcode 1 (0xe0000000): unknown opcode"), "broken cheat rejected");
    CHECK(log_has("cheat \"Code Cheat\" writes game code"), "code cheat warned at load");
    sw_hid_buttons_held = 0;
    CHECK(sw_mods_cheat_tick() == 1, "cheat VM did not run");
    CHECK(heap32(0x50) == 7, "master code: heap+0x50 = %u", heap32(0x50));
    CHECK(heap32(0x10) == 99999, "Set Money: heap+0x10 = %u", heap32(0x10));
    CHECK(heap32(0x100) == 0xAB && heap32(0x104) == 0xAB && heap32(0x108) == 0xAB && heap32(0x10C) == 0xAB &&
          heap32(0x110) == 0, "Loop Fill: %x %x %x %x %x", heap32(0x100), heap32(0x104), heap32(0x108), heap32(0x10C), heap32(0x110));
    CHECK(heap32(0x20) == 0, "Key Cheat ran without the key");
    CHECK(heap32(0x30) == 2, "Cond (else-branch, heap+0x60 != 5): heap+0x30 = %u", heap32(0x30));
    CHECK(heap32(0x40) == 0, "a cheat that is not enabled ran");
    {
        u32 v; memcpy(&v, g_main + 0x800, 4);
        CHECK(v == movz30, "Code Cheat wrote into .text");
    }
    sw_hid_buttons_held = 1;                                    /* A held */
    g_heap[0x60] = 5;
    sw_mods_cheat_tick();
    CHECK(heap32(0x20) == 1, "Key Cheat did not run with A held");
    CHECK(heap32(0x30) == 1, "Cond (if-branch, heap+0x60 == 5): heap+0x30 = %u", heap32(0x30));
    free(g_log); g_log = NULL;
    load_log();
    CHECK(log_has("cheat \"Code Cheat\" writes game code at 0x100800: skipped"), "runtime code write warning");

    /* ---- in-game mods menu API (ui/sw_overlay.cpp) ---- */
    {
        SwModUi u;
        CHECK(sw_mods_ui_count() == 7, "menu lists %d mods", sw_mods_ui_count());
        CHECK(sw_mods_ui_get(0, &u) && !strcmp(u.name, "ModBeta") && u.enabled_now && u.ncheats == 7 - 1,
              "menu row 0: %s, cheats %d", u.name, u.ncheats);       /* 7 cheats + master - Broken */
        CHECK(sw_mods_ui_get(2, &u) && !strcmp(u.name, "ModOff") && u.marker && !u.enabled_next, "ModOff marker row");
        int on = -1;
        const char *cn = sw_mods_ui_cheat(0, 5, &on);
        CHECK(cn && !strcmp(cn, "Not Enabled") && on == 0, "cheat 5 = %s (%d)", cn ? cn : "-", on);
        CHECK(sw_mods_ui_set_cheat(0, 5, 1) == 1, "live cheat switch refused");
        g_heap[0x40] = 0;
        sw_mods_cheat_tick();
        CHECK(heap32(0x40) == 1, "cheat switched on live did not run");
        sw_mods_ui_set_cheat(0, 5, 0);
        g_heap[0x40] = 0;
        sw_mods_cheat_tick();
        CHECK(heap32(0x40) == 0, "cheat switched off live still ran");
        sw_mods_ui_set_enabled(2, 1);                                 /* marker: cannot be switched on */
        CHECK(sw_mods_ui_get(2, &u) && !u.enabled_next, "marker mod switched on");
        sw_mods_ui_set_enabled(1, 0);                                 /* ModAlpha off (next start) */
        sw_mods_ui_move(1, 1);                                        /* ...and below ModOff */
        CHECK(sw_mods_ui_restart_needed(), "restart flag");
        CHECK(sw_mods_ui_get(2, &u) && !strcmp(u.name, "ModAlpha") && !u.enabled_next && u.enabled_now, "moved row");
        CHECK(sw_mods_ui_save_sync() == 1, "config save failed");
        char cp[1100];
        snprintf(cp, sizeof cp, "%s/mods.json", fx);
        FILE *f = fopen(cp, "rb");
        char cfg[4096] = {0};
        if (f) { fread(cfg, 1, sizeof cfg - 1, f); fclose(f); }
        char *b = strstr(cfg, "\"ModBeta\""), *o = strstr(cfg, "\"ModOff\""), *a = strstr(cfg, "\"ModAlpha\"");
        CHECK(b && o && a && b < o && o < a, "saved order Beta, Off, Alpha:\n%s", cfg);
        CHECK(strstr(cfg, "{\"name\": \"ModAlpha\", \"enabled\": false}") != NULL, "ModAlpha off not saved:\n%s", cfg);
        CHECK(strstr(cfg, "{\"name\": \"ModOff\", \"enabled\": true}") != NULL, "marker copied into the config:\n%s", cfg);
        CHECK(strstr(cfg, "\"cheats\": [\"Set Money\", \"Code Cheat\", \"Loop Fill\", \"Key Cheat\", \"Cond\"]") != NULL,
              "cheats not saved:\n%s", cfg);
        CHECK(strstr(cfg, "\"search_default_dirs\": false") != NULL, "settings lost:\n%s", cfg);
    }

    sw_mods_ui_flush();                          /* no save thread may still run while the process exits */
    printf("mods: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
