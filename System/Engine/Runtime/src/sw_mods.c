/* Mod support: emulator-style mod folders (LayeredFS romfs, exefs IPS/IPS32/pchtxt, Atmosphere cheats).
 * User documentation: MODS.md.  Test: Engine/Tests/test_mods.bat (no game needed).
 *
 * romfs: the game's SDK parses a RomFS *image* that sw_romfs.c synthesises at start-up.  Mods are merged into
 *   that image's tables when it is built (sw_romfs.c asks sw_mods_romfs_find() for every base file and adds the
 *   files no base file matched), so the guest's own open / stat / directory enumeration / partial reads see the
 *   merged tree with correct sizes and offsets.  The overlay index is built once here (one hash map, path ->
 *   winning mod file); nothing is scanned or looked up while the game runs.
 * exefs: the game's code is compiled to C, so patches to .text cannot take effect.  Patches to .rodata/.data are
 *   written into guest memory after the images are mapped and before relocation (the order Atmosphere's loader
 *   uses); .text patches are reported (with a decode of what they change) and skipped.
 * cheats: Atmosphere dmnt cheat VM subset on a timer thread; writes into code are skipped.
 *
 * No mods found / --no-mods: nothing is allocated, sw_mods_romfs_count() is 0 and sw_romfs.c builds the image
 * exactly as before. */
#include "sw_mods.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

void sw_report(const char *fmt, ...);
void sw_mods_print_summary(void);
#if !defined(_WIN32)
#define _ftelli64 ftello
#endif

#define ARCEUS_TID "01001F5010DFA000"
#define MAX_PATH_U8 1024
#define MAX_REL 768          /* longest romfs path accepted from a mod */
#define MAX_DEPTH 32
#define MAX_MOD_FILES 2000000
#define MAX_PATCH_FILE (64u << 20)
#define MAX_CHEAT_FILE (4u << 20)
#define MAX_CHEAT_WORDS 0x100 /* Atmosphere's per-cheat opcode limit */

/* ================================================================== */
/* small helpers                                                      */
/* ================================================================== */

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (!d) { sw_report("[mods] out of memory\n"); exit(70); }
    memcpy(d, s, n);
    return d;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { sw_report("[mods] out of memory\n"); exit(70); }
    return q;
}

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ieq(const char *a, const char *b)
{
    while (*a && *b && lc((u8)*a) == lc((u8)*b)) a++, b++;
    return lc((u8)*a) == lc((u8)*b);
}

static int icmp(const char *a, const char *b)
{
    while (*a && lc((u8)*a) == lc((u8)*b)) a++, b++;
    return lc((u8)*a) - lc((u8)*b);
}

static int iends(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && ieq(s + n - m, suf);
}

/* case-insensitive substring */
static int icontains(const char *s, const char *needle)
{
    size_t m = strlen(needle);
    for (; *s; s++) {
        size_t i = 0;
        while (i < m && s[i] && lc((u8)s[i]) == lc((u8)needle[i])) i++;
        if (i == m) return 1;
    }
    return 0;
}

static void join(char *out, size_t n, const char *a, const char *b)
{
    size_t la = strlen(a);
    if (la && (a[la - 1] == '/' || a[la - 1] == '\\')) snprintf(out, n, "%s%s", a, b);
    else snprintf(out, n, "%s/%s", a, b);
}

static const char *base_name(const char *p)
{
    const char *s = p;
    for (const char *q = p; *q; q++)
        if ((*q == '/' || *q == '\\') && q[1]) s = q + 1;
    return s;
}

static int is_hex(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) if (!isxdigit((u8)s[i])) return 0;
    return n > 0;
}

static u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

/* ================================================================== */
/* logging: console (sw_report) + mods.log                            */
/* ================================================================== */

static FILE *g_logf;
static char *g_early;          /* lines logged before mods.log is opened (config errors ...) */
static size_t g_early_n;

void sw_mods_log(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sw_report("[mods] %s\n", buf);
    if (g_logf) { fprintf(g_logf, "%s\n", buf); fflush(g_logf); }
    else if (g_early_n < (64u << 10)) {
        size_t l = strlen(buf);
        char *e = (char *)realloc(g_early, g_early_n + l + 2);
        if (e) {
            g_early = e;
            memcpy(g_early + g_early_n, buf, l);
            g_early_n += l;
            g_early[g_early_n++] = '\n';
            g_early[g_early_n] = 0;
        }
    }
}
#define mlog sw_mods_log

/* ================================================================== */
/* file system (UTF-8 paths; Windows uses the wide API)               */
/* ================================================================== */

typedef struct { char *name; int is_dir, is_link; u64 size; } DirEnt;

#if defined(_WIN32)
static wchar_t *to_w(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)xrealloc(NULL, sizeof(wchar_t) * (size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static char *from_w(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return xstrdup("");
    char *s = (char *)xrealloc(NULL, (size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
/* command line / environment strings are in the ANSI code page */
static char *acp_to_utf8(const char *s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (n <= 0) return xstrdup(s);
    wchar_t *w = (wchar_t *)xrealloc(NULL, sizeof(wchar_t) * (size_t)n);
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    char *r = from_w(w);
    free(w);
    return r;
}
#else
static char *acp_to_utf8(const char *s) { return xstrdup(s); }
#endif

static int dirent_cmp(const void *a, const void *b) { return strcmp(((const DirEnt *)a)->name, ((const DirEnt *)b)->name); }

/* entries of dir, sorted by name (without . and ..); returns count (0 if it cannot be read) */
static int fs_list(const char *dir, DirEnt **out)
{
    DirEnt *v = NULL;
    int n = 0, cap = 0;
#if defined(_WIN32)
    char pat[MAX_PATH_U8 + 4];
    join(pat, sizeof pat, dir, "*");
    wchar_t *wp = to_w(pat);
    if (!wp) { *out = NULL; return 0; }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) { *out = NULL; return 0; }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; v = (DirEnt *)xrealloc(v, sizeof *v * (size_t)cap); }
        v[n].name = from_w(fd.cFileName);
        v[n].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        v[n].is_link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        v[n].size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) { *out = NULL; return 0; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[MAX_PATH_U8];
        struct stat st, lst;
        join(p, sizeof p, dir, de->d_name);
        if (lstat(p, &lst)) continue;
        int link = S_ISLNK(lst.st_mode);
        if (stat(p, &st)) st = lst;
        if (n == cap) { cap = cap ? cap * 2 : 32; v = (DirEnt *)xrealloc(v, sizeof *v * (size_t)cap); }
        v[n].name = xstrdup(de->d_name);
        v[n].is_dir = S_ISDIR(st.st_mode);
        v[n].is_link = link;
        v[n].size = (u64)st.st_size;
        n++;
    }
    closedir(d);
#endif
    if (n > 1) qsort(v, (size_t)n, sizeof *v, dirent_cmp);
    *out = v;
    return n;
}

static void fs_free(DirEnt *v, int n)
{
    for (int i = 0; i < n; i++) free(v[i].name);
    free(v);
}

/* 0 missing, 1 file, 2 directory */
static int fs_kind(const char *p)
{
#if defined(_WIN32)
    wchar_t *w = to_w(p);
    if (!w) return 0;
    DWORD a = GetFileAttributesW(w);
    free(w);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
#else
    struct stat st;
    if (stat(p, &st)) return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
#endif
}

FILE *sw_mods_fopen(const char *p)
{
#if defined(_WIN32)
    wchar_t *w = to_w(p);
    if (!w) return NULL;
    FILE *f = _wfopen(w, L"rb");
    free(w);
    return f;
#else
    return fopen(p, "rb");
#endif
}

/* canonical absolute path (for de-duplicating mod folders reached through two roots) */
static void fs_canon(const char *p, char *out, size_t n)
{
#if defined(_WIN32)
    wchar_t *w = to_w(p), buf[MAX_PATH_U8];
    DWORD r = w ? GetFullPathNameW(w, MAX_PATH_U8, buf, NULL) : 0;
    free(w);
    if (r && r < MAX_PATH_U8) { char *u = from_w(buf); snprintf(out, n, "%s", u); free(u); }
    else snprintf(out, n, "%s", p);
#else
    char buf[4096];
    if (realpath(p, buf)) snprintf(out, n, "%s", buf); else snprintf(out, n, "%s", p);
#endif
    for (char *q = out; *q; q++) { if (*q == '\\') *q = '/'; *q = (char)lc((u8)*q); }
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
}

/* child of dir whose name equals name case-insensitively; kind 1 file / 2 dir / 0 any.  out = full path */
static int find_child(const char *dir, const char *name, int kind, char *out, size_t n)
{
    DirEnt *v;
    int k = fs_list(dir, &v), found = 0;
    for (int i = 0; i < k && !found; i++) {
        if (v[i].is_link || !ieq(v[i].name, name)) continue;
        if (kind == 1 && v[i].is_dir) continue;
        if (kind == 2 && !v[i].is_dir) continue;
        join(out, n, dir, v[i].name);
        found = 1;
    }
    fs_free(v, k);
    return found;
}

static u8 *read_file(const char *p, u64 limit, u64 *size)
{
    FILE *f = sw_mods_fopen(p);
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long long n = _ftelli64(f);
    if (n < 0 || (u64)n > limit) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    u8 *d = (u8 *)xrealloc(NULL, (size_t)n + 1);
    size_t got = fread(d, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(d); return NULL; }
    d[n] = 0;
    *size = (u64)n;
    return d;
}

/* ================================================================== */
/* minimal JSON (config only; // and # line comments allowed)          */
/* ================================================================== */

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct JVal {
    int type, b, n;
    double num;
    char *str;
    struct JVal **items;
    char **keys;
} JVal;
typedef struct { const char *p, *end, *err; int depth; } JP;

static void jws(JP *j)
{
    for (;;) {
        while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++;
        if (j->p + 1 < j->end && j->p[0] == '/' && j->p[1] == '/') { while (j->p < j->end && *j->p != '\n') j->p++; continue; }
        if (j->p < j->end && *j->p == '#') { while (j->p < j->end && *j->p != '\n') j->p++; continue; }
        break;
    }
}

static void put_utf8(char **b, size_t *n, size_t *cap, u32 cp)
{
    char t[4];
    int k;
    if (cp < 0x80) { t[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[2] = (char)(0x80 | (cp & 0x3F)); k = 3; }
    else { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); k = 4; }
    if (*n + (size_t)k + 1 > *cap) { *cap = (*cap + (size_t)k + 1) * 2; *b = (char *)xrealloc(*b, *cap); }
    memcpy(*b + *n, t, (size_t)k);
    *n += (size_t)k;
}

static int hex4(const char *p, u32 *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        int c = p[i], d = c >= '0' && c <= '9' ? c - '0' : lc(c) >= 'a' && lc(c) <= 'f' ? lc(c) - 'a' + 10 : -1;
        if (d < 0) return 0;
        *v = *v * 16 + (u32)d;
    }
    return 1;
}

static char *jstring(JP *j)
{
    size_t n = 0, cap = 32;
    char *b = (char *)xrealloc(NULL, cap);
    j->p++;                                      /* opening quote */
    while (j->p < j->end && *j->p != '"') {
        u32 cp = (u8)*j->p++;
        if (cp == '\\') {
            if (j->p >= j->end) break;
            char e = *j->p++;
            switch (e) {
            case 'n': cp = '\n'; break;
            case 't': cp = '\t'; break;
            case 'r': cp = '\r'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'u':
                if (j->end - j->p < 4 || !hex4(j->p, &cp)) { j->err = "bad \\u escape"; free(b); return NULL; }
                j->p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                    u32 lo;
                    if (hex4(j->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); j->p += 6; }
                }
                put_utf8(&b, &n, &cap, cp);
                continue;
            default: cp = (u8)e; break;           /* \" \\ \/ */
            }
        }
        if (n + 2 > cap) { cap *= 2; b = (char *)xrealloc(b, cap); }
        b[n++] = (char)cp;
    }
    if (j->p >= j->end) { j->err = "unterminated string"; free(b); return NULL; }
    j->p++;
    b[n] = 0;
    return b;
}

static void jfree(JVal *v)
{
    if (!v) return;
    for (int i = 0; i < v->n; i++) { jfree(v->items[i]); if (v->keys) free(v->keys[i]); }
    free(v->items); free(v->keys); free(v->str); free(v);
}

static JVal *jvalue(JP *j)
{
    jws(j);
    if (j->p >= j->end) { j->err = "unexpected end"; return NULL; }
    if (++j->depth > 32) { j->err = "nested too deep"; return NULL; }
    JVal *v = (JVal *)calloc(1, sizeof *v);
    if (!v) exit(70);
    char c = *j->p;
    if (c == '{' || c == '[') {
        int obj = c == '{';
        v->type = obj ? J_OBJ : J_ARR;
        j->p++;
        int cap = 0;
        for (;;) {
            jws(j);
            if (j->p < j->end && *j->p == (obj ? '}' : ']')) { j->p++; break; }
            char *key = NULL;
            if (obj) {
                if (j->p >= j->end || *j->p != '"') { j->err = "expected a \"key\""; jfree(v); return NULL; }
                if (!(key = jstring(j))) { jfree(v); return NULL; }
                jws(j);
                if (j->p >= j->end || *j->p != ':') { j->err = "expected ':'"; free(key); jfree(v); return NULL; }
                j->p++;
            }
            JVal *it = jvalue(j);
            if (!it) { free(key); jfree(v); return NULL; }
            if (v->n == cap) {
                cap = cap ? cap * 2 : 8;
                v->items = (JVal **)xrealloc(v->items, sizeof(JVal *) * (size_t)cap);
                if (obj) v->keys = (char **)xrealloc(v->keys, sizeof(char *) * (size_t)cap);
            }
            v->items[v->n] = it;
            if (obj) v->keys[v->n] = key;
            v->n++;
            jws(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == (obj ? '}' : ']')) { j->p++; break; }
            j->err = obj ? "expected ',' or '}'" : "expected ',' or ']'";
            jfree(v);
            return NULL;
        }
    } else if (c == '"') {
        v->type = J_STR;
        if (!(v->str = jstring(j))) { free(v); return NULL; }
    } else if (!strncmp(j->p, "true", 4)) { v->type = J_BOOL; v->b = 1; j->p += 4; }
    else if (!strncmp(j->p, "false", 5)) { v->type = J_BOOL; j->p += 5; }
    else if (!strncmp(j->p, "null", 4)) { v->type = J_NULL; j->p += 4; }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        char *e;
        v->type = J_NUM;
        v->num = strtod(j->p, &e);
        if (e == j->p) { j->err = "bad number"; free(v); return NULL; }
        j->p = e;
    } else { j->err = "unexpected character"; free(v); return NULL; }
    j->depth--;
    return v;
}

static JVal *jget(const JVal *o, const char *key)
{
    if (!o || o->type != J_OBJ) return NULL;
    for (int i = 0; i < o->n; i++) if (!strcmp(o->keys[i], key)) return o->items[i];
    return NULL;
}
static int jbool(const JVal *v, int def) { return !v ? def : v->type == J_BOOL ? v->b : v->type == J_NUM ? v->num != 0 : def; }

/* ================================================================== */
/* configuration                                                      */
/* ================================================================== */

typedef struct {
    char *name;
    int enabled;               /* -1 not given */
    int all_cheats;            /* "cheats": true / "all" */
    char **cheats;
    int ncheats;
} CfgMod;

static struct {
    int no_mods, list;
    char *cli_dirs[16];
    int ncli_dirs;
    char *cli_config;
    int enabled;               /* config "enabled" */
    char *dirs[32];
    int ndirs;
    int search_default, search_emulator;
    CfgMod mods[512];
    int nmods;
    int cheat_hz;
    char path[MAX_PATH_U8];    /* config file used ("" = none) */
    char log_path[MAX_PATH_U8];
} C;
static int g_cfg_defaults;
static void cfg_defaults(void)
{
    if (g_cfg_defaults) return;
    g_cfg_defaults = 1;
    C.enabled = 1;
    C.search_default = 1;
    C.cheat_hz = 12;
}

void sw_mods_cli(int argc, char **argv)
{
    cfg_defaults();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-mods")) C.no_mods = 1;
        else if (!strcmp(argv[i], "--list-mods")) C.list = 1;
        else if (!strcmp(argv[i], "--mods-dir") && i + 1 < argc) {
            if (C.ncli_dirs < 16) C.cli_dirs[C.ncli_dirs++] = acp_to_utf8(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "--mods-config") && i + 1 < argc) C.cli_config = acp_to_utf8(argv[++i]);
    }
}
int sw_mods_list_requested(void) { return C.list; }

static CfgMod *cfg_mod(const char *name)
{
    for (int i = 0; i < C.nmods; i++) if (ieq(C.mods[i].name, name)) return &C.mods[i];
    if (C.nmods >= (int)(sizeof C.mods / sizeof *C.mods)) return NULL;
    CfgMod *m = &C.mods[C.nmods++];
    memset(m, 0, sizeof *m);
    m->name = xstrdup(name);
    m->enabled = -1;
    return m;
}

static void cfg_cheats(CfgMod *m, const JVal *v)
{
    if (!m || !v) return;
    if (v->type == J_BOOL) { m->all_cheats = v->b; return; }
    if (v->type == J_STR) { m->all_cheats = ieq(v->str, "all"); return; }
    if (v->type != J_ARR) return;
    for (int i = 0; i < v->n; i++) {
        if (v->items[i]->type != J_STR) continue;
        m->cheats = (char **)xrealloc(m->cheats, sizeof(char *) * (size_t)(m->ncheats + 1));
        m->cheats[m->ncheats++] = xstrdup(v->items[i]->str);
    }
}

static int load_json_config(const char *path, const char *text, size_t n)
{
    JP j = {text, text + n, NULL, 0};
    JVal *root = jvalue(&j);
    if (root) { jws(&j); if (j.p < j.end) { j.err = "text after the end"; jfree(root); root = NULL; } }
    if (!root || root->type != J_OBJ) {
        int line = 1;
        for (const char *q = text; q < j.p && q < text + n; q++) line += *q == '\n';
        mlog("ERROR: %s line %d: %s - the mod config is ignored (all mods load with default settings)", path, line,
             j.err ? j.err : "the file must be a JSON object { ... }");
        jfree(root);
        return 0;
    }
    C.enabled = jbool(jget(root, "enabled"), 1);
    C.search_default = jbool(jget(root, "search_default_dirs"), 1);
    C.search_emulator = jbool(jget(root, "search_emulator_dirs"), 0);
    const JVal *v = jget(root, "cheat_hz");
    if (v && v->type == J_NUM && v->num >= 1 && v->num <= 240) C.cheat_hz = (int)v->num;
    v = jget(root, "log_file");
    if (v && v->type == J_STR && *v->str) snprintf(C.log_path, sizeof C.log_path, "%s", v->str);
    v = jget(root, "mods_dirs");
    if (v && v->type == J_STR && *v->str && C.ndirs < 32) C.dirs[C.ndirs++] = xstrdup(v->str);
    if (v && v->type == J_ARR)
        for (int i = 0; i < v->n && C.ndirs < 32; i++)
            if (v->items[i]->type == J_STR && *v->items[i]->str) C.dirs[C.ndirs++] = xstrdup(v->items[i]->str);
    v = jget(root, "mods");          /* priority order, first = highest */
    if (v && v->type == J_ARR)
        for (int i = 0; i < v->n; i++) {
            const JVal *e = v->items[i];
            if (e->type == J_STR) { CfgMod *m = cfg_mod(e->str); if (m && m->enabled < 0) m->enabled = 1; }
            else if (e->type == J_OBJ) {
                const JVal *nm = jget(e, "name");
                if (!nm || nm->type != J_STR) continue;
                CfgMod *m = cfg_mod(nm->str);
                if (!m) continue;
                m->enabled = jbool(jget(e, "enabled"), 1);
                cfg_cheats(m, jget(e, "cheats"));
            }
        }
    v = jget(root, "disabled");
    if (v && v->type == J_ARR)
        for (int i = 0; i < v->n; i++)
            if (v->items[i]->type == J_STR) { CfgMod *m = cfg_mod(v->items[i]->str); if (m) m->enabled = 0; }
    v = jget(root, "cheats");        /* {"ModName": ["Cheat", ...] | true} */
    if (v && v->type == J_OBJ)
        for (int i = 0; i < v->n; i++) cfg_cheats(cfg_mod(v->keys[i]), v->items[i]);
    jfree(root);
    return 1;
}

/* mods.txt: one mod per line in priority order (first = highest); "-Name" = disabled; "# ..." comment */
static void load_txt_config(const char *text)
{
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[512];
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t l = strlen(s);
        while (l && (s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
        if (*s && *s != '#' && *s != ';') {
            int off = *s == '-' || *s == '!';
            if (off) { s++; while (*s == ' ') s++; }
            if (*s) { CfgMod *m = cfg_mod(s); if (m) m->enabled = !off; }
        }
        if (!e) break;
        p = e + 1;
    }
}

static void load_config(void)
{
    const char *exe = sw_mods_host_exe_dir();
    char cands[16][MAX_PATH_U8];
    int nc = 0;
    const char *env = getenv("SW_MODS_CONFIG");
    if (C.cli_config) snprintf(cands[nc++], MAX_PATH_U8, "%s", C.cli_config);
    else if (env && *env) { char *u = acp_to_utf8(env); snprintf(cands[nc++], MAX_PATH_U8, "%s", u); free(u); }
    else {
        /* the exe is in System/build_work/out: Settings/ is three folders up (2026-11-25 layout); then the old
         * Config/ places */
        static const char *rel[] = {"mods.json", "../../../Settings/mods.json", "../../Config/mods.json", "../Config/mods.json",
                                    "Config/mods.json", "mods.txt", "../../../Settings/mods.txt", "../../Config/mods.txt",
                                    "../Config/mods.txt", "Config/mods.txt"};
        for (int i = 0; i < 10; i++) join(cands[nc++], MAX_PATH_U8, exe, rel[i]);
    }
    for (int i = 0; i < nc; i++) {
        if (fs_kind(cands[i]) != 1) {
            if (i == 0 && (C.cli_config || (env && *env))) mlog("WARNING: mod config %s not found: using defaults", cands[i]);
            continue;
        }
        u64 n;
        u8 *t = read_file(cands[i], 4u << 20, &n);
        if (!t) { mlog("WARNING: cannot read mod config %s: using defaults", cands[i]); return; }
        snprintf(C.path, sizeof C.path, "%s", cands[i]);
        const char *txt = (const char *)t;
        if (n >= 3 && t[0] == 0xEF && t[1] == 0xBB && t[2] == 0xBF) txt += 3;     /* UTF-8 BOM */
        if (iends(cands[i], ".txt")) load_txt_config(txt);
        else load_json_config(cands[i], txt, strlen(txt));
        free(t);
        return;
    }
}

/* ================================================================== */
/* mods and discovery                                                 */
/* ================================================================== */

typedef struct {
    char name[160];
    char dir[MAX_PATH_U8], canon[MAX_PATH_U8];
    const char *layout;
    char romfs[MAX_PATH_U8], exefs[MAX_PATH_U8], cheats[MAX_PATH_U8];
    int exefs_flat;            /* atmosphere exefs_patches/<name>: the folder itself holds the .ips files */
    int enabled, prio;
    const char *why_off;
    CfgMod *cfg;
    /* statistics */
    int rf_files, rf_replaced, rf_added, rf_rejected, rf_shadowed, rf_bad;
    int px_files, px_applied, px_code, px_other, px_bytes;
    int ch_total, ch_enabled;
} Mod;

static Mod *g_mods;
static int g_nmods, g_capmods;
static int *g_order;           /* mod indexes by priority, highest first */
static char *g_roots[64];
static int g_nroots;
static int g_inited, g_active, g_summary_done;

const char *sw_mods_name(int mod) { return mod >= 0 && mod < g_nmods ? g_mods[mod].name : "?"; }

static void add_mod(const char *dir, const char *name, const char *layout, int exefs_flat)
{
    char canon[MAX_PATH_U8];
    fs_canon(dir, canon, sizeof canon);
    for (int i = 0; i < g_nmods; i++)
        if (!strcmp(g_mods[i].canon, canon)) return;          /* same folder through two search roots */
    if (g_nmods == g_capmods) {
        g_capmods = g_capmods ? g_capmods * 2 : 16;
        g_mods = (Mod *)xrealloc(g_mods, sizeof(Mod) * (size_t)g_capmods);
    }
    Mod *m = &g_mods[g_nmods];
    memset(m, 0, sizeof *m);
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    snprintf(m->canon, sizeof m->canon, "%s", canon);
    snprintf(m->name, sizeof m->name, "%s", name);
    for (int k = 2;; k++) {                                   /* unique names: "Name (2)" */
        int dup = 0;
        for (int i = 0; i < g_nmods && !dup; i++) dup = ieq(g_mods[i].name, m->name);
        if (!dup) break;
        snprintf(m->name, sizeof m->name, "%s (%d)", name, k);
    }
    m->layout = layout;
    m->exefs_flat = exefs_flat;
    if (exefs_flat) snprintf(m->exefs, sizeof m->exefs, "%s", dir);
    else {
        find_child(dir, "romfs", 2, m->romfs, sizeof m->romfs);
        find_child(dir, "exefs", 2, m->exefs, sizeof m->exefs);
        find_child(dir, "cheats", 2, m->cheats, sizeof m->cheats);
    }
    m->enabled = 1;
    char tmp[MAX_PATH_U8];
    if (find_child(dir, ".disabled", 1, tmp, sizeof tmp)) { m->enabled = 0; m->why_off = ".disabled file in the mod folder"; }
    g_nmods++;
}

static int has_mod_content(const char *dir)
{
    char t[MAX_PATH_U8];
    return find_child(dir, "romfs", 2, t, sizeof t) || find_child(dir, "exefs", 2, t, sizeof t) ||
           find_child(dir, "cheats", 2, t, sizeof t);
}

static int is_tid(const char *s) { return strlen(s) == 16 && is_hex(s, 16); }

/* <T>/{romfs,exefs,cheats} (one mod named after the parent) and/or <T>/<Mod>/{romfs,...} */
static void scan_title_dir(const char *t, const char *direct_name, const char *layout_direct, const char *layout_sub)
{
    if (has_mod_content(t)) add_mod(t, direct_name, layout_direct, 0);
    DirEnt *v;
    int n = fs_list(t, &v);
    for (int i = 0; i < n; i++) {
        if (!v[i].is_dir || v[i].is_link || v[i].name[0] == '.') continue;
        if (ieq(v[i].name, "romfs") || ieq(v[i].name, "exefs") || ieq(v[i].name, "cheats")) continue;
        char p[MAX_PATH_U8];
        join(p, sizeof p, t, v[i].name);
        if (has_mod_content(p)) add_mod(p, v[i].name, layout_sub, 0);
    }
    fs_free(v, n);
}

/* a folder whose children are title ids (yuzu load/, Ryujinx mods/contents/, Atmosphere contents/) */
static void scan_tid_parent(const char *d, const char *layout)
{
    DirEnt *v;
    int n = fs_list(d, &v);
    for (int i = 0; i < n; i++) {
        if (!v[i].is_dir || v[i].is_link || !is_tid(v[i].name)) continue;
        char p[MAX_PATH_U8];
        join(p, sizeof p, d, v[i].name);
        if (!ieq(v[i].name, ARCEUS_TID)) { mlog("note: %s: mods for another game (title %s), ignored", p, v[i].name); continue; }
        scan_title_dir(p, v[i].name, layout, layout);
    }
    fs_free(v, n);
}

static void scan_exefs_patches(const char *d)
{
    DirEnt *v;
    int n = fs_list(d, &v);
    for (int i = 0; i < n; i++) {
        if (!v[i].is_dir || v[i].is_link) continue;
        char p[MAX_PATH_U8];
        join(p, sizeof p, d, v[i].name);
        add_mod(p, v[i].name, "atmosphere exefs_patches", 1);
    }
    fs_free(v, n);
}

static void scan_root(const char *root)
{
    char p[MAX_PATH_U8], q[MAX_PATH_U8];
    const char *bn = base_name(root);
    /* the root is itself a title folder or a single mod */
    if (is_tid(bn)) {
        if (!ieq(bn, ARCEUS_TID)) { mlog("note: %s: mods for another game (title %s), ignored", root, bn); return; }
        scan_title_dir(root, bn, "title folder", "LayeredFS <TitleID>/<Mod>");
        return;
    }
    if (has_mod_content(root)) { add_mod(root, bn, "single mod folder", 0); return; }
    /* emulator / CFW user folders */
    if (find_child(root, "atmosphere", 2, p, sizeof p)) {
        if (find_child(p, "contents", 2, q, sizeof q)) {
            char t[MAX_PATH_U8];
            if (find_child(q, ARCEUS_TID, 2, t, sizeof t)) scan_title_dir(t, "atmosphere", "Atmosphere contents", "Atmosphere contents");
        }
        if (find_child(p, "exefs_patches", 2, q, sizeof q)) scan_exefs_patches(q);
    }
    if (find_child(root, "exefs_patches", 2, p, sizeof p)) scan_exefs_patches(p);
    if (find_child(root, "load", 2, p, sizeof p)) scan_tid_parent(p, "yuzu load/<TitleID>/<Mod>");
    if (find_child(root, "mods", 2, p, sizeof p) && find_child(p, "contents", 2, q, sizeof q))
        scan_tid_parent(q, "Ryujinx mods/contents/<TitleID>/<Mod>");
    if (find_child(root, "contents", 2, p, sizeof p)) scan_tid_parent(p, "contents/<TitleID>/<Mod>");
    /* children: <TitleID>/<Mod>/... (LayeredFS style) and <Mod>/... (flat style) */
    DirEnt *v;
    int n = fs_list(root, &v);
    for (int i = 0; i < n; i++) {
        if (!v[i].is_dir || v[i].name[0] == '.') continue;   /* .mb_tmp (browser installs in progress), hidden */
        join(p, sizeof p, root, v[i].name);
        if (v[i].is_link) { mlog("WARNING: %s is a link/junction: skipped (mods must be real folders)", p); continue; }
        int special = ieq(v[i].name, "atmosphere") || ieq(v[i].name, "load") || ieq(v[i].name, "mods") ||
                      ieq(v[i].name, "contents") || ieq(v[i].name, "exefs_patches");
        if (special && !has_mod_content(p)) continue;          /* handled above */
        if (is_tid(v[i].name)) {
            if (!ieq(v[i].name, ARCEUS_TID)) { mlog("note: %s: mods for another game (title %s), ignored", p, v[i].name); continue; }
            scan_title_dir(p, v[i].name, "<TitleID>/{romfs,exefs,cheats}", "LayeredFS <TitleID>/<Mod>");
        } else if (has_mod_content(p)) add_mod(p, v[i].name, "flat <Mod>/romfs", 0);
        else mlog("note: %s has no romfs/, exefs/ or cheats/ folder: not a mod", p);
    }
    fs_free(v, n);
}

static void add_root(const char *r)
{
    char canon[MAX_PATH_U8];
    if (!r || !*r || g_nroots >= 64) return;
    fs_canon(r, canon, sizeof canon);
    for (int i = 0; i < g_nroots; i++) {
        char c2[MAX_PATH_U8];
        fs_canon(g_roots[i], c2, sizeof c2);
        if (!strcmp(c2, canon)) return;
    }
    g_roots[g_nroots++] = xstrdup(r);
}

static void collect_roots(void)
{
    for (int i = 0; i < C.ncli_dirs; i++) add_root(C.cli_dirs[i]);
    const char *env = getenv("SW_MODS_DIR");
    if (env && *env) {
        char *u = acp_to_utf8(env), *s = u;
        for (char *e; s && *s; s = e ? e + 1 : NULL) {
            e = strchr(s, ';');
            if (e) *e = 0;
            add_root(s);
        }
        free(u);
    }
    for (int i = 0; i < C.ndirs; i++) add_root(C.dirs[i]);
    if (C.search_default) {
        const char *exe = sw_mods_host_exe_dir();
        char p[MAX_PATH_U8];
        static const char *rel[] = {"mods", "../mods", "../../mods", "../../../Mods"};   /* Mods/: the 2026-11-25 layout */
        for (int i = 0; i < 4; i++) { join(p, sizeof p, exe, rel[i]); add_root(p); }
        add_root("mods");
    }
    if (C.search_emulator) {
#if defined(_WIN32)
        const char *ad = getenv("APPDATA");
        if (ad && *ad) {
            static const char *rel[] = {"yuzu/load", "suyu/load", "sudachi/load", "citron/load", "eden/load",
                                        "Ryujinx/mods/contents"};
            char *u = acp_to_utf8(ad), p[MAX_PATH_U8];
            for (int i = 0; i < 6; i++) { join(p, sizeof p, u, rel[i]); add_root(p); }
            free(u);
        }
#else
        const char *home = getenv("HOME");
        if (home) {
            static const char *rel[] = {".local/share/yuzu/load", ".local/share/eden/load", ".config/Ryujinx/mods/contents"};
            char p[MAX_PATH_U8];
            for (int i = 0; i < 3; i++) { join(p, sizeof p, home, rel[i]); add_root(p); }
        }
#endif
    }
}

static int prio_cmp(const void *a, const void *b)
{
    const Mod *x = &g_mods[*(const int *)a], *y = &g_mods[*(const int *)b];
    if (x->prio != y->prio) return x->prio < y->prio ? -1 : 1;
    int c = icmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

static void apply_config(void)
{
    for (int i = 0; i < g_nmods; i++) {
        Mod *m = &g_mods[i];
        m->prio = 1 << 20;                                     /* unlisted: after the listed ones, by name */
        for (int k = 0; k < C.nmods; k++)
            if (ieq(C.mods[k].name, m->name)) {
                m->cfg = &C.mods[k];
                m->prio = k;
                /* a .disabled marker file always wins (it is the explicit one) */
                if (C.mods[k].enabled == 0 && m->enabled) { m->enabled = 0; m->why_off = "disabled in the mod config"; }
                break;
            }
    }
    for (int k = 0; k < C.nmods; k++) {
        int found = 0;
        for (int i = 0; i < g_nmods && !found; i++) found = g_mods[i].cfg == &C.mods[k];
        if (!found) mlog("note: the mod config names \"%s\", but no such mod folder was found", C.mods[k].name);
    }
    g_order = (int *)xrealloc(NULL, sizeof(int) * (size_t)(g_nmods ? g_nmods : 1));
    for (int i = 0; i < g_nmods; i++) g_order[i] = i;
    qsort(g_order, (size_t)g_nmods, sizeof(int), prio_cmp);
    for (int r = 0; r < g_nmods; r++) g_mods[g_order[r]].prio = r + 1;   /* 1 = highest */
}

/* ================================================================== */
/* romfs overlay index                                                */
/* ================================================================== */

typedef struct {
    SwModFile f;
    char *key;                 /* lower-case path */
    int *losers;               /* lower-priority mods providing the same path */
    int nlosers;
} RIdx;

static RIdx *g_rf;
static int g_nrf, g_caprf;
static int *g_rfh;             /* open addressing: index + 1, 0 = empty */
static u32 g_rfh_cap;
static int g_rf_conflicts, g_rf_case_notes;

static u32 hash_key(const char *s)
{
    u32 h = 2166136261u;
    for (; *s; s++) h = (h ^ (u8)lc((u8)*s)) * 16777619u;
    return h;
}

static void rfh_insert(int idx)
{
    u32 h = hash_key(g_rf[idx].key) & (g_rfh_cap - 1);
    while (g_rfh[h]) h = (h + 1) & (g_rfh_cap - 1);
    g_rfh[h] = idx + 1;
}

static int rf_lookup(const char *rel)
{
    if (!g_rfh_cap) return -1;
    u32 h = hash_key(rel) & (g_rfh_cap - 1);
    while (g_rfh[h]) {
        if (ieq(g_rf[g_rfh[h] - 1].key, rel)) return g_rfh[h] - 1;
        h = (h + 1) & (g_rfh_cap - 1);
    }
    return -1;
}

static int rf_add(int mod, const char *rel, const char *host, u64 size)
{
    if (g_nrf >= MAX_MOD_FILES) return -1;
    if ((u32)(g_nrf + 1) * 2 > g_rfh_cap) {
        u32 nc = g_rfh_cap ? g_rfh_cap * 2 : 1024;
        free(g_rfh);
        g_rfh = (int *)calloc(nc, sizeof(int));
        if (!g_rfh) exit(70);
        g_rfh_cap = nc;
        for (int i = 0; i < g_nrf; i++) rfh_insert(i);
    }
    if (g_nrf == g_caprf) { g_caprf = g_caprf ? g_caprf * 2 : 1024; g_rf = (RIdx *)xrealloc(g_rf, sizeof(RIdx) * (size_t)g_caprf); }
    RIdx *e = &g_rf[g_nrf];
    memset(e, 0, sizeof *e);
    e->f.rel = xstrdup(rel);
    e->f.host = xstrdup(host);
    e->f.size = size;
    e->f.mod = mod;
    char *k = xstrdup(rel);
    for (char *q = k; *q; q++) *q = (char)lc((u8)*q);
    e->key = k;
    rfh_insert(g_nrf);
    return g_nrf++;
}

static int name_ok(const char *n)
{
    for (const u8 *p = (const u8 *)n; *p; p++)
        if (*p < 0x20 || *p == ':' || *p == '\\' || *p == '/') return 0;
    return strcmp(n, ".") && strcmp(n, "..");
}

static void index_dir(int mod, const char *host, const char *rel, int depth)
{
    Mod *m = &g_mods[mod];
    if (depth > MAX_DEPTH) { mlog("WARNING: %s: %s nested deeper than %d folders: skipped", m->name, rel, MAX_DEPTH); return; }
    DirEnt *v;
    int n = fs_list(host, &v);
    for (int i = 0; i < n; i++) {
        char hp[MAX_PATH_U8], rp[MAX_PATH_U8];
        if (!name_ok(v[i].name)) { mlog("WARNING: %s: odd file name \"%s\" in %s: skipped", m->name, v[i].name, host); continue; }
        join(hp, sizeof hp, host, v[i].name);
        if (*rel) snprintf(rp, sizeof rp, "%s/%s", rel, v[i].name); else snprintf(rp, sizeof rp, "%s", v[i].name);
        if (v[i].is_link) { mlog("WARNING: %s: romfs/%s is a link/junction: skipped (it could point outside the mod folder)", m->name, rp); continue; }
        if (strlen(rp) > MAX_REL || strlen(hp) >= MAX_PATH_U8 - 2) { mlog("WARNING: %s: path too long, skipped: romfs/%s", m->name, rp); continue; }
        if (v[i].is_dir) { index_dir(mod, hp, rp, depth + 1); continue; }
        if (ieq(v[i].name, ".disabled") || ieq(v[i].name, "desktop.ini") || ieq(v[i].name, "thumbs.db") ||
            ieq(v[i].name, ".ds_store")) continue;
        m->rf_files++;
        int prev = rf_lookup(rp);
        if (prev >= 0 && g_rf[prev].f.mod == mod) {
            mlog("WARNING: %s: romfs/%s and romfs/%s differ only in upper/lower case: the second is ignored", m->name,
                 g_rf[prev].f.rel, rp);
            m->rf_bad++;
            continue;
        }
        if (v[i].size == 0) { mlog("WARNING: %s: romfs/%s is empty: ignored (the original file is kept)", m->name, rp); m->rf_bad++; continue; }
        FILE *f = sw_mods_fopen(hp);
        if (!f) { mlog("WARNING: %s: romfs/%s cannot be opened: ignored (the original file is kept)", m->name, rp); m->rf_bad++; continue; }
        fclose(f);
        if (prev >= 0) {                                       /* a higher-priority mod already provides it */
            RIdx *e = &g_rf[prev];
            e->losers = (int *)xrealloc(e->losers, sizeof(int) * (size_t)(e->nlosers + 1));
            e->losers[e->nlosers++] = mod;
            m->rf_shadowed++;
            continue;
        }
        if (rf_add(mod, rp, hp, v[i].size) < 0) { mlog("WARNING: more than %d mod files: the rest are ignored", MAX_MOD_FILES); break; }
    }
    fs_free(v, n);
}

static void build_romfs_index(void)
{
    for (int r = 0; r < g_nmods; r++) {
        int mi = g_order[r];
        Mod *m = &g_mods[mi];
        if (!m->enabled || !*m->romfs) continue;
        index_dir(mi, m->romfs, "", 0);
        /* the game's RomFS root holds bin/ and system_resource/ only */
        DirEnt *v;
        int n = fs_list(m->romfs, &v);
        for (int i = 0; i < n; i++)
            if (!ieq(v[i].name, "bin") && !ieq(v[i].name, "system_resource") && !ieq(v[i].name, ".disabled"))
                mlog("WARNING: %s: romfs/%s%s is not a folder of the game's RomFS (its root has bin/ and "
                     "system_resource/). Its files are added but the game will most likely never read them - "
                     "was the mod packed for another layout (romfs/bin/...)?", m->name, v[i].name, v[i].is_dir ? "/" : "");
        fs_free(v, n);
    }
    for (int i = 0; i < g_nrf; i++) {
        RIdx *e = &g_rf[i];
        if (!e->nlosers) continue;
        char who[1024];
        size_t o = (size_t)snprintf(who, sizeof who, "%s (priority %d, used)", g_mods[e->f.mod].name, g_mods[e->f.mod].prio);
        for (int k = 0; k < e->nlosers && o < sizeof who - 64; k++)
            o += (size_t)snprintf(who + o, sizeof who - o, ", %s (priority %d, not used)", g_mods[e->losers[k]].name,
                                  g_mods[e->losers[k]].prio);
        mlog("conflict: romfs/%s is provided by %s", e->f.rel, who);
        g_rf_conflicts++;
    }
}

int sw_mods_romfs_count(void) { sw_mods_init(); return g_nrf; }
const SwModFile *sw_mods_romfs_get(int i) { return i >= 0 && i < g_nrf ? &g_rf[i].f : NULL; }

SwModFile *sw_mods_romfs_find(const char *rel)
{
    int i = rf_lookup(rel);
    if (i < 0) return NULL;
    if (strcmp(g_rf[i].f.rel, rel) && g_rf_case_notes++ < 20)
        mlog("note: %s: romfs/%s replaces the game's %s (upper/lower case differs)", g_mods[g_rf[i].f.mod].name,
             g_rf[i].f.rel, rel);
    return &g_rf[i].f;
}

void sw_mods_romfs_result(int mod, int replaced, int added, int rejected)
{
    if (mod < 0 || mod >= g_nmods) return;
    g_mods[mod].rf_replaced += replaced;
    g_mods[mod].rf_added += added;
    g_mods[mod].rf_rejected += rejected;
}

/* ================================================================== */
/* exefs patches: IPS / IPS32 / pchtxt                                */
/* ================================================================== */

typedef struct {
    int mod, module;
    char file[160], block[128];
    u64 off;                   /* module offset */
    u32 len;
    u8 *bytes;
} PRec;

static PRec *g_pr;
static int g_npr, g_cappr;
static int g_px_fps_warned;

static int module_by_bid(const char *hex, size_t n)
{
    if (n < 16 || n > 64 || !is_hex(hex, n)) return -1;
    int cnt = sw_mods_host_module_count();
    for (int i = 0; i < cnt; i++) {
        SwModuleInfo mi;
        if (!sw_mods_host_module(i, &mi) || !mi.module_id || !mi.text_size) continue;
        size_t k = 0;                                          /* file names give a prefix of the 32-byte id */
        while (k < n && mi.module_id[k] && lc((u8)hex[k]) == lc((u8)mi.module_id[k])) k++;
        if (k == n) return i;
    }
    return -2;
}

static const char *main_bid(char *buf, size_t n)
{
    SwModuleInfo mi;
    if (sw_mods_host_module(0, &mi) && mi.module_id) {
        size_t l = strlen(mi.module_id);
        while (l > 16 && mi.module_id[l - 1] == '0') l--;
        if (l % 2) l++;
        snprintf(buf, n, "%.*s", (int)l, mi.module_id);
        for (char *q = buf; *q; q++) *q = (char)toupper((u8)*q);
    } else snprintf(buf, n, "?");
    return buf;
}

static void pr_add(int mod, int module, const char *file, const char *block, u64 off, const u8 *b, u32 len)
{
    if (!len) return;
    if (g_npr == g_cappr) { g_cappr = g_cappr ? g_cappr * 2 : 64; g_pr = (PRec *)xrealloc(g_pr, sizeof(PRec) * (size_t)g_cappr); }
    PRec *r = &g_pr[g_npr++];
    r->mod = mod;
    r->module = module;
    snprintf(r->file, sizeof r->file, "%s", file);
    snprintf(r->block, sizeof r->block, "%s", block ? block : "");
    r->off = off;
    r->len = len;
    r->bytes = (u8 *)xrealloc(NULL, len);
    memcpy(r->bytes, b, len);
}

static void parse_ips(int mod, int module, const char *fname, const u8 *d, u64 n)
{
    Mod *m = &g_mods[mod];
    int ips32 = n >= 5 && !memcmp(d, "IPS32", 5);
    if (!ips32 && !(n >= 5 && !memcmp(d, "PATCH", 5))) { mlog("WARNING: %s: %s is not an IPS file (no PATCH/IPS32 header): skipped", m->name, fname); return; }
    u64 p = 5;
    int ow = ips32 ? 4 : 3, nrec = 0;
    for (;;) {
        if (p + (u64)ow > n) { mlog("WARNING: %s: %s ends without an EOF marker (%d records read)", m->name, fname, nrec); break; }
        if (ips32 ? !memcmp(d + p, "EEOF", 4) : !memcmp(d + p, "EOF", 3)) break;
        u64 off = 0;
        for (int i = 0; i < ow; i++) off = (off << 8) | d[p + (u64)i];
        p += (u64)ow;
        if (p + 2 > n) { mlog("WARNING: %s: %s: truncated record", m->name, fname); break; }
        u32 size = ((u32)d[p] << 8) | d[p + 1];
        p += 2;
        u8 *tmp;
        u32 len;
        if (size == 0) {                                       /* RLE: count (2), value (1) */
            if (p + 3 > n) { mlog("WARNING: %s: %s: truncated RLE record", m->name, fname); break; }
            len = ((u32)d[p] << 8) | d[p + 1];
            tmp = (u8 *)xrealloc(NULL, len ? len : 1);
            memset(tmp, d[p + 2], len);
            p += 3;
        } else {
            if (p + size > n) { mlog("WARNING: %s: %s: record at 0x%llx runs past the end of the file", m->name, fname, (unsigned long long)off); break; }
            len = size;
            tmp = (u8 *)xrealloc(NULL, len);
            memcpy(tmp, d + p, len);
            p += size;
        }
        /* IPS offsets count from the NSO header (0x100 bytes) */
        if (off < 0x100) mlog("WARNING: %s: %s: record at 0x%llx patches the NSO header: skipped", m->name, fname, (unsigned long long)off);
        else pr_add(mod, module, fname, "", off - 0x100, tmp, len);
        free(tmp);
        nrec++;
    }
}

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : lc(c) >= 'a' && lc(c) <= 'f' ? lc(c) - 'a' + 10 : -1; }

/* IPSwitch text patches (.pchtxt), as yuzu / Ryujinx read them */
static void parse_pchtxt(int mod, const char *fname, const char *t)
{
    Mod *m = &g_mods[mod];
    char bid_buf[80];
    int module = -1, have_bid = 0, enabled = 0, little = 0, lineno = 0, skip_all = 0;
    long long shift = 0;
    char block[128] = "", last_comment[128] = "";
    const char *p = t;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[1024];
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p = e ? e + 1 : p + n;
        lineno++;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t l = strlen(s);
        while (l && (s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
        if (!*s) continue;
        if ((s[0] == '/' && s[1] == '/') || s[0] == '#' || s[0] == '[') {
            const char *c = s + (s[0] == '/' ? 2 : 1);
            while (*c == ' ') c++;
            snprintf(last_comment, sizeof last_comment, "%s", c);
            l = strlen(last_comment);
            if (l && last_comment[l - 1] == ']') last_comment[l - 1] = 0;
            continue;
        }
        if (s[0] == '@') {
            if (!strncmp(s, "@nsobid-", 8)) {
                const char *h = s + 8;
                size_t hl = 0;
                while (isxdigit((u8)h[hl])) hl++;
                have_bid = 1;
                module = module_by_bid(h, hl);
                snprintf(bid_buf, sizeof bid_buf, "%.*s", (int)(hl < 64 ? hl : 64), h);
                if (module < 0) {
                    char mb[80];
                    mlog("WARNING: %s: %s is for build id %s, but this game is %s (another game version?): skipped",
                         m->name, fname, bid_buf, main_bid(mb, sizeof mb));
                    skip_all = 1;
                }
            } else if (!strncmp(s, "@flag", 5)) {
                char key[64] = "";
                long long v = 0;
                if (sscanf(s + 5, " %63s %lli", key, &v) == 2 && !strcmp(key, "offset_shift")) shift = v;
            } else if (!strcmp(s, "@little-endian")) little = 1;
            else if (!strcmp(s, "@big-endian")) little = 0;
            else if (!strncmp(s, "@enabled", 8) || !strncmp(s, "@disabled", 9)) {
                enabled = s[1] == 'e';
                snprintf(block, sizeof block, "%s", last_comment[0] ? last_comment : "(unnamed)");
                last_comment[0] = 0;
            } else if (!strcmp(s, "@stop")) break;
            continue;
        }
        if (skip_all || !enabled) continue;
        if (!have_bid) {
            mlog("WARNING: %s: %s has no @nsobid- line: assuming it is for the main program", m->name, fname);
            have_bid = 1;
            module = 0;
        }
        /* "<hex address> <hex bytes>" or "<hex address> \"string\"" (+ optional // comment) */
        char *c = strstr(s, "//");
        if (c) *c = 0;
        char *q = s;
        unsigned long long addr = 0;
        int nd = 0;
        while (isxdigit((u8)*q)) { addr = addr * 16 + (unsigned long long)hexval(*q); q++; nd++; }
        if (!nd || nd > 10 || (*q != ' ' && *q != '\t')) { mlog("WARNING: %s: %s line %d not understood: \"%s\"", m->name, fname, lineno, s); continue; }
        while (*q == ' ' || *q == '\t') q++;
        u8 buf[512];
        u32 len = 0;
        if (*q == '"') {
            q++;
            while (*q && *q != '"' && len < sizeof buf - 1) {
                int ch = (u8)*q++;
                if (ch == '\\' && *q) { ch = (u8)*q++; ch = ch == 'n' ? '\n' : ch == 't' ? '\t' : ch == '0' ? 0 : ch; }
                buf[len++] = (u8)ch;
            }
            buf[len++] = 0;                                    /* IPSwitch strings are written NUL-terminated */
        } else {
            int hi = -1;
            for (; *q && len < sizeof buf; q++) {
                if (*q == ' ' || *q == '\t') break;
                int v = hexval((u8)*q);
                if (v < 0) { len = 0; break; }
                if (hi < 0) hi = v; else { buf[len++] = (u8)(hi * 16 + v); hi = -1; }
            }
            if (hi >= 0) len = 0;
            if (len && little) for (u32 i = 0; i < len / 2; i++) { u8 x = buf[i]; buf[i] = buf[len - 1 - i]; buf[len - 1 - i] = x; }
        }
        if (!len) { mlog("WARNING: %s: %s line %d: bad value: \"%s\"", m->name, fname, lineno, s); continue; }
        long long ips = (long long)addr + shift;               /* IPS offset; minus the NSO header = module offset */
        if (ips < 0x100) { mlog("WARNING: %s: %s line %d: address 0x%llx is in the NSO header: skipped", m->name, fname, lineno, addr); continue; }
        pr_add(mod, module, fname, block, (u64)(ips - 0x100), buf, len);
    }
}

static void parse_exefs(int mod)
{
    Mod *m = &g_mods[mod];
    if (!*m->exefs) return;
    DirEnt *v;
    int n = fs_list(m->exefs, &v);
    for (int i = 0; i < n; i++) {
        if (v[i].is_dir || v[i].is_link) continue;
        const char *nm = v[i].name;
        char p[MAX_PATH_U8];
        join(p, sizeof p, m->exefs, nm);
        if (ieq(nm, "main") || ieq(nm, "rtld") || ieq(nm, "sdk") || !strncmp(nm, "subsdk", 6) || ieq(nm, "main.npdm")) {
            mlog("WARNING: %s: exefs/%s replaces compiled program code. This game is a static recompilation (the code "
                 "is already C), so a replacement program cannot be loaded: skipped", m->name, nm);
            m->px_other++;
            continue;
        }
        int ips = iends(nm, ".ips") || iends(nm, ".ips32"), pch = iends(nm, ".pchtxt");
        if (!ips && !pch) continue;
        m->px_files++;
        u64 sz;
        u8 *d = read_file(p, MAX_PATCH_FILE, &sz);
        if (!d) { mlog("WARNING: %s: cannot read exefs/%s (or larger than 64 MB): skipped", m->name, nm); continue; }
        if (pch) parse_pchtxt(mod, nm, (const char *)d);
        else {
            size_t stem = strlen(nm) - (iends(nm, ".ips32") ? 6 : 4);
            int module = module_by_bid(nm, stem);
            if (module < 0) {
                char mb[80];
                if (module == -1) mlog("WARNING: %s: exefs/%s: an IPS file must be named after the build id it is for "
                                       "(this game: %s.ips): skipped", m->name, nm, main_bid(mb, sizeof mb));
                else mlog("WARNING: %s: exefs/%s is for build id %.*s, but this game is %s (another game version?): skipped",
                          m->name, nm, (int)stem, nm, main_bid(mb, sizeof mb));
            } else parse_ips(mod, module, nm, d, sz);
        }
        free(d);
    }
    fs_free(v, n);
}

/* ---- describing a code patch: what does it change? ---- */

enum { CH_SAME, CH_IMM, CH_FIMM, CH_NOP, CH_RET, CH_BRANCH, CH_OTHER };

static double fimm8(u32 imm8)
{
    double mant = 1.0 + (double)(imm8 & 0xF) / 16.0;
    int e = (int)(((imm8 >> 4) & 7) ^ 4) - 3;
    double v = mant;
    while (e > 0) { v *= 2; e--; }
    while (e < 0) { v /= 2; e++; }
    return (imm8 & 0x80) ? -v : v;
}

static void insn_text(u32 w, char *o, size_t n)
{
    const char *r = (w >> 31) ? "x" : "w";
    if (w == 0xD503201Fu) snprintf(o, n, "nop");
    else if (w == 0xD65F03C0u) snprintf(o, n, "ret");
    else if ((w & 0x1F800000u) == 0x12800000u && ((w >> 29) & 3) != 1) {
        static const char *nm[] = {"movn", "?", "movz", "movk"};
        snprintf(o, n, "%s %s%u, #0x%x, lsl #%u", nm[(w >> 29) & 3], r, w & 31, (w >> 5) & 0xFFFF, ((w >> 21) & 3) * 16);
    } else if ((w & 0x1F000000u) == 0x11000000u) {
        int sub = (w >> 30) & 1, s = (w >> 29) & 1;
        u32 imm = ((w >> 10) & 0xFFF) << (((w >> 22) & 1) ? 12 : 0);
        if (s && (w & 31) == 31) snprintf(o, n, "%s %s%u, #0x%x", sub ? "cmp" : "cmn", r, (w >> 5) & 31, imm);
        else snprintf(o, n, "%s%s %s%u, %s%u, #0x%x", sub ? "sub" : "add", s ? "s" : "", r, w & 31, r, (w >> 5) & 31, imm);
    } else if ((w & 0xFF201FE0u) == 0x1E201000u) {
        u32 t = (w >> 22) & 3;
        snprintf(o, n, "fmov %c%u, #%g", t == 0 ? 's' : t == 1 ? 'd' : 'h', w & 31, fimm8((w >> 13) & 0xFF));
    } else if ((w & 0x1F800000u) == 0x12000000u) {
        static const char *nm[] = {"and", "orr", "eor", "ands"};
        snprintf(o, n, "%s %s%u, %s%u, #<bitmask 0x%x>", nm[(w >> 29) & 3], r, w & 31, r, (w >> 5) & 31, (w >> 10) & 0x1FFF);
    } else if ((w & 0xFC000000u) == 0x14000000u || (w & 0xFC000000u) == 0x94000000u) {
        long long imm = (long long)(int32_t)(w << 6) >> 6;
        snprintf(o, n, "%s .%+lld", (w >> 31) ? "bl" : "b", imm * 4);
    } else if ((w & 0xFF000010u) == 0x54000000u) {
        static const char *cc[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
        long long imm = (long long)(int32_t)((w >> 5) << 13) >> 13;
        snprintf(o, n, "b.%s .%+lld", cc[w & 15], imm * 4);
    } else if ((w & 0x7E000000u) == 0x34000000u) snprintf(o, n, "%s %s%u, ...", (w >> 24) & 1 ? "cbnz" : "cbz", r, w & 31);
    else if ((w & 0x1F000000u) == 0x10000000u) snprintf(o, n, "%s x%u, ...", (w >> 31) ? "adrp" : "adr", w & 31);
    else if ((w & 0x3A000000u) == 0x28000000u)
        snprintf(o, n, "%s%s ..., [x%u, ...]", (w >> 22) & 1 ? "ldp" : "stp", (w >> 26) & 1 ? " (fp/simd)" : "", (w >> 5) & 31);
    else if ((w & 0x3B000000u) == 0x39000000u)
        snprintf(o, n, "%s%s ..., [x%u, #0x%x]", (w >> 22) & 1 ? "ldr" : "str", (w >> 26) & 1 ? " (fp/simd)" : "",
                 (w >> 5) & 31, ((w >> 10) & 0xFFF) << (w >> 30));
    else snprintf(o, n, ".word 0x%08x", w);
}

static int is_branch(u32 w)
{
    return (w & 0x7C000000u) == 0x14000000u || (w & 0xFF000010u) == 0x54000000u || (w & 0x7E000000u) == 0x34000000u ||
           (w & 0x7E000000u) == 0x36000000u;
}

static int classify(u32 o, u32 w)
{
    if (o == w) return CH_SAME;
    if (w == 0xD503201Fu) return CH_NOP;
    if (w == 0xD65F03C0u) return CH_RET;
    if ((o & 0xFF201FE0u) == 0x1E201000u && (w & 0xFF201FE0u) == 0x1E201000u && (o & ~0x1FE000u) == (w & ~0x1FE000u)) return CH_FIMM;
    if ((o & 0x1F800000u) == 0x12800000u && (w & 0x1F800000u) == 0x12800000u && (o & ~0x1FFFE0u) == (w & ~0x1FFFE0u)) return CH_IMM;
    if ((o & 0x1F000000u) == 0x11000000u && (w & 0x1F000000u) == 0x11000000u && (o & ~0x3FFC00u) == (w & ~0x3FFC00u)) return CH_IMM;
    if ((o & 0x1F800000u) == 0x12000000u && (w & 0x1F800000u) == 0x12000000u && (o & ~0x7FFC00u) == (w & ~0x7FFC00u)) return CH_IMM;
    if (is_branch(o) || is_branch(w)) return CH_BRANCH;
    return CH_OTHER;
}

/* a code (.text) patch: say what it does, warn, skip */
static void report_code_patch(const PRec *r, const SwModuleInfo *mi, u64 off, u32 len, const u8 *nb)
{
    Mod *m = &g_mods[r->mod];
    u64 ga = mi->base + off;
    u64 fstart = 0;
    const char *fn = sw_mods_host_function_at(ga, &fstart);
    char where[160];
    if (fn) snprintf(where, sizeof where, "function %s (%s+0x%llx)", fn, mi->name, (unsigned long long)(fstart - mi->base));
    else snprintf(where, sizeof where, "no known function");
    char desc[1024];
    size_t o = 0;
    int kinds[8] = {0}, words = 0;
    u64 w0 = off & ~3ull;
    for (u64 a = w0; a < off + len && words < 64; a += 4, words++) {
        u8 ob[4], wb[4];
        if (!sw_guest_read(mi->base + a, ob, 4)) { kinds[CH_OTHER]++; continue; }
        memcpy(wb, ob, 4);
        for (int k = 0; k < 4; k++) if (a + (u64)k >= off && a + (u64)k < off + len) wb[k] = nb[a + (u64)k - off];
        u32 ow = (u32)ob[0] | ((u32)ob[1] << 8) | ((u32)ob[2] << 16) | ((u32)ob[3] << 24);
        u32 nw = (u32)wb[0] | ((u32)wb[1] << 8) | ((u32)wb[2] << 16) | ((u32)wb[3] << 24);
        int k = classify(ow, nw);
        kinds[k]++;
        if (k != CH_SAME && words < 4 && o < sizeof desc - 200) {
            char a1[96], a2[96];
            insn_text(ow, a1, sizeof a1);
            insn_text(nw, a2, sizeof a2);
            o += (size_t)snprintf(desc + o, sizeof desc - o, "%s+0x%llx: %s -> %s; ", mi->name, (unsigned long long)a, a1, a2);
        }
    }
    if (words >= 4 && o < sizeof desc - 40) o += (size_t)snprintf(desc + o, sizeof desc - o, "(%d instructions in all)", words);
    else if (o >= 2) desc[o - 2] = 0;
    if (!o) snprintf(desc, sizeof desc, "no change to the original bytes");
    int changed = words - kinds[CH_SAME];
    const char *what;
    if (!changed) what = "it writes the bytes that are already there (no effect)";
    else if (kinds[CH_IMM] + kinds[CH_FIMM] == changed)
        what = kinds[CH_FIMM] ? "a constant (float immediate) change inside the function: could be supported via a "
                                "codegen hook in that function"
                              : "a constant (immediate) change inside the function: could be supported via a codegen "
                                "hook in that function";
    else if (fn && fstart == ga && kinds[CH_RET] && changed <= 3)
        what = "the function is forced to return early / return a constant: could be supported via a codegen hook "
               "that replaces the function";
    else if (kinds[CH_NOP] == changed) what = "instructions removed (NOP): not supported";
    else if (kinds[CH_BRANCH]) what = "a control-flow (branch) change: not supported";
    else what = "a code change: not supported";
    mlog("WARNING: %s: %s%s%s: code patch at %s+0x%llx (%u bytes, %s) SKIPPED - the game's code is compiled to C, "
         "patches to .text cannot take effect. It is %s. [%s]",
         m->name, r->file, r->block[0] ? " / " : "", r->block, mi->name, (unsigned long long)off, len, where, what, desc);
    const char *hook = fn ? sw_hooks_fps_conflict(fstart) : NULL;
    int fps_name = icontains(m->name, "fps") || icontains(r->file, "fps") || icontains(r->block, "fps") ||
                   icontains(m->name, "frame") || icontains(r->block, "frame") || icontains(m->name, "camera") ||
                   icontains(r->block, "camera") || icontains(m->name, "fov") || icontains(r->block, "fov") ||
                   icontains(r->block, "60hz") || icontains(r->block, "speed");
    if (hook || (fps_name && g_px_fps_warned < 8)) {
        g_px_fps_warned++;
        mlog("WARNING: %s: \"%s\" looks like a 60 FPS / camera patch%s%s%s. It is incompatible with the built-in FPS "
             "modes (and code patches cannot work here anyway): use the launcher's frame rate setting "
             "(30/60/90/120) instead.", m->name, r->block[0] ? r->block : r->file, hook ? " - it changes the function the built-in \"" : "",
             hook ? hook : "", hook ? "\" fix hooks" : "");
    }
}

typedef struct { u64 lo, hi; int mod; } Span;
static Span *g_spans;
static int g_nspans;

void sw_mods_apply_exefs(void)
{
    sw_mods_init();
    if (!g_active || !g_npr) return;
    static int done;
    if (done) return;
    done = 1;
    /* lowest priority first, so that a higher-priority mod's bytes end up in memory */
    for (int rr = g_nmods - 1; rr >= 0; rr--) {
        int mi_ = g_order[rr];
        int logged = 0;
        for (int i = 0; i < g_npr; i++) {
            PRec *r = &g_pr[i];
            if (r->mod != mi_) continue;
            Mod *m = &g_mods[r->mod];
            SwModuleInfo mi;
            if (!sw_mods_host_module(r->module, &mi)) continue;
            u64 ro = align_up(mi.text_size, 0x1000), da = align_up(ro + mi.ro_size, 0x1000), end = da + mi.data_size;
            u64 off = r->off;
            u32 left = r->len;
            const u8 *src = r->bytes;
            while (left) {
                u64 seg_end;
                const char *seg;
                if (off < ro) { seg_end = ro; seg = ".text"; }
                else if (off < da) { seg_end = da; seg = ".rodata"; }
                else if (off < end) { seg_end = end; seg = ".data"; }
                else {
                    mlog("WARNING: %s: %s%s%s: %u bytes at %s+0x%llx are outside the program image (.bss or past the "
                         "end): skipped", m->name, r->file, r->block[0] ? " / " : "", r->block, left, mi.name,
                         (unsigned long long)off);
                    m->px_other++;
                    break;
                }
                u32 n = (u32)((seg_end - off) < left ? (seg_end - off) : left);
                if (seg[1] == 't') { report_code_patch(r, &mi, off, n, src); m->px_code++; }
                else {
                    u64 ga = mi.base + off;
                    const char *fd = sw_hooks_fps_data_conflict(ga, ga + n);
                    if (sw_guest_write(ga, src, n)) {
                        m->px_applied++;
                        m->px_bytes += (int)n;
                        if (logged++ < 20)
                            mlog("applied: %s: %s%s%s: %s+0x%llx (%s, %u bytes)", m->name, r->file, r->block[0] ? " / " : "",
                                 r->block, mi.name, (unsigned long long)off, seg, n);
                        if (fd) mlog("WARNING: %s: the patch at %s+0x%llx changes %s, which the built-in FPS modes rely "
                                     "on. If the game runs too fast or too slow, disable this patch or set the frame rate "
                                     "to 30.", m->name, mi.name, (unsigned long long)off, fd);
                        for (int s = 0; s < g_nspans; s++)
                            if (g_spans[s].mod != r->mod && g_spans[s].lo < ga + n && ga < g_spans[s].hi)
                                mlog("conflict: %s+0x%llx: the patch of %s (priority %d, used) overwrites the patch of "
                                     "%s (priority %d)", mi.name, (unsigned long long)off, m->name, m->prio,
                                     g_mods[g_spans[s].mod].name, g_mods[g_spans[s].mod].prio);
                        g_spans = (Span *)xrealloc(g_spans, sizeof(Span) * (size_t)(g_nspans + 1));
                        g_spans[g_nspans].lo = ga;
                        g_spans[g_nspans].hi = ga + n;
                        g_spans[g_nspans++].mod = r->mod;
                    } else {
                        mlog("WARNING: %s: cannot write %s+0x%llx (%u bytes): skipped", m->name, mi.name, (unsigned long long)off, n);
                        m->px_other++;
                    }
                }
                off += n;
                src += n;
                left -= n;
            }
        }
        if (logged > 20) mlog("applied: %s: ... and %d more data patch records", g_mods[mi_].name, logged - 20);
    }
    if (!g_nrf) sw_mods_print_summary();
}

/* ================================================================== */
/* cheats: Atmosphere cheat VM                                        */
/* ================================================================== */

enum {
    OP_STORE_STATIC = 0, OP_BEGIN_COND = 1, OP_END_COND = 2, OP_LOOP = 3, OP_LOAD_REG_STATIC = 4,
    OP_LOAD_REG_MEM = 5, OP_STORE_TO_REG_ADDR = 6, OP_LEGACY_ARITH = 7, OP_KEY_COND = 8, OP_ARITH = 9,
    OP_STORE_REG = 10, OP_REG_COND = 0xC0, OP_SAVE_RESTORE = 0xC1, OP_SAVE_RESTORE_MASK = 0xC2,
    OP_STATIC_REG = 0xC3, OP_PAUSE = 0xFF0, OP_RESUME = 0xFF1, OP_DEBUG = 0xFFF
};

typedef struct {
    int type, cheat;
    u8 T, M, R, S, X, I, O, C, r, flag;
    u64 addr, val;
} VmOp;

typedef struct {
    char name[96];
    int mod, master, enabled, nops;
    VmOp *ops;
    int warned_code, warned_unmapped;
} Cheat;

static Cheat *g_ch;
static int g_nch;
static VmOp *g_prog;
static int g_nprog;
static u64 g_static_regs[256];
static volatile long g_cheat_runs;

static int width_ok(int t) { return t == 1 || t == 2 || t == 4 || t == 8; }

/* decode one opcode at w[*ip]; 0 = invalid / truncated (err says why) */
static int vm_decode(const u32 *w, int n, int *ip, VmOp *op, const char **err)
{
    memset(op, 0, sizeof *op);
    if (*ip >= n) { *err = "truncated"; return 0; }
    u32 f = w[*ip];
    int type = (int)(f >> 28), len = 1;
#define NEED(k) do { if (*ip + (k) > n) { *err = "opcode needs more words than the cheat has"; return 0; } } while (0)
#define W(k) ((u64)w[*ip + (k)])
    if (type >= 0xC) type = (int)((f >> 24) & 0xFF);
    if (type >= 0xF0) type = (int)((f >> 20) & 0xFFF);
    op->type = type;
    switch (type) {
    case OP_STORE_STATIC: case OP_BEGIN_COND:
        op->T = (u8)((f >> 24) & 0xF); op->M = (u8)((f >> 20) & 0xF);
        if (type == OP_STORE_STATIC) op->R = (u8)((f >> 16) & 0xF);
        else { op->C = (u8)((f >> 16) & 0xF); if (((f >> 12) & 0xF) == 1) { op->flag = 1; op->r = (u8)((f >> 8) & 0xF); } }
        len = op->T == 8 ? 4 : 3;
        NEED(len);
        op->addr = ((u64)(f & 0xFF) << 32) | W(1);
        op->val = op->T == 8 ? (W(2) << 32) | W(3) : W(2);
        if (!width_ok(op->T) || op->M > 3) { *err = "bad width or memory type"; return 0; }
        if (type == OP_BEGIN_COND && (op->C < 1 || op->C > 6)) { *err = "bad condition"; return 0; }
        break;
    case OP_END_COND: op->flag = (u8)(((f >> 24) & 0xF) == 1); break;
    case OP_LOOP:
        op->flag = (u8)(((f >> 24) & 0xF) == 0);               /* 1 = start */
        op->R = (u8)((f >> 16) & 0xF);
        if (op->flag) { len = 2; NEED(2); op->val = W(1); }
        break;
    case OP_LOAD_REG_STATIC: op->R = (u8)((f >> 16) & 0xF); len = 3; NEED(3); op->val = (W(1) << 32) | W(2); break;
    case OP_LOAD_REG_MEM:
        op->T = (u8)((f >> 24) & 0xF); op->M = (u8)((f >> 20) & 0xF); op->R = (u8)((f >> 16) & 0xF);
        op->I = (u8)((f >> 12) & 0xF);
        len = 2; NEED(2);
        op->addr = ((u64)(f & 0xFF) << 32) | W(1);
        if (!width_ok(op->T) || op->M > 3 || op->I > 1) { *err = "bad width, memory type or load mode"; return 0; }
        break;
    case OP_STORE_TO_REG_ADDR:
        op->T = (u8)((f >> 24) & 0xF); op->R = (u8)((f >> 16) & 0xF); op->I = (u8)((f >> 12) & 0xF);
        op->O = (u8)((f >> 8) & 0xF); op->r = (u8)((f >> 4) & 0xF);
        len = 3; NEED(3);
        op->val = (W(1) << 32) | W(2);
        if (!width_ok(op->T)) { *err = "bad width"; return 0; }
        break;
    case OP_LEGACY_ARITH:
        op->T = (u8)((f >> 24) & 0xF); op->R = (u8)((f >> 16) & 0xF); op->C = (u8)((f >> 12) & 0xF);
        len = 2; NEED(2);
        op->val = W(1);
        if (!width_ok(op->T) || op->C > 4) { *err = "bad width or operation"; return 0; }
        break;
    case OP_KEY_COND: op->val = f & 0x0FFFFFFFu; break;
    case OP_ARITH:
        op->T = (u8)((f >> 24) & 0xF); op->C = (u8)((f >> 20) & 0xF); op->R = (u8)((f >> 16) & 0xF);
        op->S = (u8)((f >> 12) & 0xF); op->flag = (u8)((f >> 8) & 0xF); op->r = (u8)((f >> 4) & 0xF);
        if (op->flag > 1 || !width_ok(op->T) || op->C > 9) { *err = "bad width, operation or operand type (float operations are not supported)"; return 0; }
        if (op->flag) { len = op->T == 8 ? 3 : 2; NEED(len); op->val = op->T == 8 ? (W(1) << 32) | W(2) : W(1); }
        break;
    case OP_STORE_REG:
        op->T = (u8)((f >> 24) & 0xF); op->S = (u8)((f >> 20) & 0xF); op->R = (u8)((f >> 16) & 0xF);
        op->I = (u8)((f >> 12) & 0xF); op->O = (u8)((f >> 8) & 0xF); op->X = (u8)((f >> 4) & 0xF);
        if (!width_ok(op->T) || op->O > 5 || (op->O >= 3 && op->X > 3)) { *err = "bad width, offset type or memory type"; return 0; }
        if (op->O == 2 || op->O == 4 || op->O == 5) { len = 2; NEED(2); op->addr = ((u64)(f & 0xF) << 32) | W(1); }
        break;
    case OP_REG_COND:
        op->T = (u8)((f >> 20) & 0xF); op->C = (u8)((f >> 16) & 0xF); op->S = (u8)((f >> 12) & 0xF);
        op->X = (u8)((f >> 8) & 0xF);
        if (!width_ok(op->T) || op->C < 1 || op->C > 6 || op->X > 5) { *err = "bad width, condition or operand type"; return 0; }
        switch (op->X) {
        case 0: op->M = (u8)((f >> 4) & 0xF); len = 2; NEED(2); op->addr = ((u64)(f & 0xF) << 32) | W(1); break;
        case 1: op->M = (u8)((f >> 4) & 0xF); op->r = (u8)(f & 0xF); break;
        case 2: op->R = (u8)((f >> 4) & 0xF); len = 2; NEED(2); op->addr = ((u64)(f & 0xF) << 32) | W(1); break;
        case 3: op->R = (u8)((f >> 4) & 0xF); op->r = (u8)(f & 0xF); break;
        case 4: len = op->T == 8 ? 3 : 2; NEED(len); op->val = op->T == 8 ? (W(1) << 32) | W(2) : W(1); break;
        case 5: op->r = (u8)((f >> 4) & 0xF); break;
        }
        if ((op->X == 0 || op->X == 1) && op->M > 3) { *err = "bad memory type"; return 0; }
        break;
    case OP_SAVE_RESTORE:
        op->R = (u8)((f >> 16) & 0xF); op->S = (u8)((f >> 8) & 0xF); op->X = (u8)((f >> 4) & 0xF);
        if (op->X > 3) { *err = "bad save/restore operation"; return 0; }
        break;
    case OP_SAVE_RESTORE_MASK:
        op->X = (u8)((f >> 20) & 0xF); op->val = f & 0xFFFF;
        if (op->X > 3) { *err = "bad save/restore operation"; return 0; }
        break;
    case OP_STATIC_REG: op->val = (f >> 4) & 0xFF; op->R = (u8)(f & 0xF); break;
    case OP_PAUSE: case OP_RESUME: break;
    case OP_DEBUG: {
        int x = (int)((f >> 8) & 0xF);
        if (x == 0 || x == 2) { len = 2; NEED(2); }
        break;
    }
    default: *err = "unknown opcode"; return 0;
    }
#undef NEED
#undef W
    *ip += len;
    return 1;
}

static int is_begin(const VmOp *o) { return o->type == OP_BEGIN_COND || o->type == OP_KEY_COND || o->type == OP_REG_COND; }

static int in_code(u64 a, u64 n)
{
    int cnt = sw_mods_host_module_count();
    for (int i = 0; i < cnt; i++) {
        SwModuleInfo mi;
        if (!sw_mods_host_module(i, &mi) || !mi.text_size) continue;
        u64 lo = mi.base, hi = mi.base + align_up(mi.text_size, 0x1000);
        if (a < hi && a + n > lo) return 1;
    }
    return 0;
}

static void check_code_writes(Cheat *c);

static void parse_cheat_file(int mod, const char *path, const char *fname, const char *t)
{
    Mod *m = &g_mods[mod];
    Cheat *cur = NULL;
    u32 words[MAX_CHEAT_WORDS];
    int nw = 0, lineno = 0, bad = 0;
    const char *p = t;
#define FINISH() do { if (cur) {                                                                              \
        int ip = 0; const char *err = NULL; VmOp op; cur->ops = NULL; cur->nops = 0;                          \
        while (!bad && ip < nw) {                                                                             \
            int at = ip;                                                                                      \
            if (!vm_decode(words, nw, &ip, &op, &err)) {                                                      \
                mlog("WARNING: %s: cheat \"%s\": opcode %d (0x%08x): %s - the cheat is disabled", m->name,    \
                     cur->name, at + 1, words[at], err); bad = 1; break; }                                    \
            cur->ops = (VmOp *)xrealloc(cur->ops, sizeof(VmOp) * (size_t)(cur->nops + 1));                    \
            cur->ops[cur->nops++] = op; }                                                                     \
        if (bad || !nw) { free(cur->ops); cur->ops = NULL; cur->nops = 0; g_nch--; }                          \
        else check_code_writes(cur); } cur = NULL; nw = 0; bad = 0; } while (0)
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[1024];
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p = e ? e + 1 : p + n;
        lineno++;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t l = strlen(s);
        while (l && (s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
        if (!*s) continue;
        if (*s == '[' || *s == '{') {
            FINISH();
            char close = *s == '[' ? ']' : '}';
            char *q = strchr(s + 1, close);
            if (q) *q = 0;
            g_ch = (Cheat *)xrealloc(g_ch, sizeof(Cheat) * (size_t)(g_nch + 1));
            cur = &g_ch[g_nch++];
            memset(cur, 0, sizeof *cur);
            snprintf(cur->name, sizeof cur->name, "%s", s + 1);
            cur->mod = mod;
            cur->master = close == '}';
            continue;
        }
        if (!cur || bad) continue;
        /* hex words, separated by spaces */
        for (char *q = s; *q;) {
            while (*q == ' ' || *q == '\t') q++;
            if (!*q) break;
            if (q[0] == '/' && q[1] == '/') break;
            int k = 0;
            u32 v = 0;
            while (isxdigit((u8)q[k]) && k < 9) { v = v * 16 + (u32)hexval(q[k]); k++; }
            if (k != 8 || (q[k] && q[k] != ' ' && q[k] != '\t')) {
                mlog("WARNING: %s: %s line %d: \"%s\" is not a list of 8-digit hex words - cheat \"%s\" disabled",
                     m->name, fname, lineno, s, cur->name);
                bad = 1;
                break;
            }
            if (nw >= MAX_CHEAT_WORDS) { mlog("WARNING: %s: cheat \"%s\" has more than %d opcodes - disabled", m->name, cur->name, MAX_CHEAT_WORDS); bad = 1; break; }
            words[nw++] = v;
            q += k;
        }
    }
    FINISH();
#undef FINISH
    (void)path;
}

/* warn at load about writes whose target is known statically (main + constant address) and in code */
static void check_code_writes(Cheat *c)
{
    Mod *m = &g_mods[c->mod];
    for (int i = 0; i < c->nops; i++) {
        const VmOp *o = &c->ops[i];
        int stat = (o->type == OP_STORE_STATIC && o->R == 0) || (o->type == OP_STORE_REG && o->O == 4);
        int mem = o->type == OP_STORE_STATIC ? o->M : o->X;
        if (!stat || mem != 0) continue;                       /* only main-relative writes are known now */
        u64 a = sw_guest_region_base(0) + o->addr;
        if (in_code(a, o->T)) {
            mlog("WARNING: %s: cheat \"%s\" writes game code at main+0x%llx (%u bytes). The game's code is compiled "
                 "to C, so code writes cannot take effect: they are skipped (any data writes of the cheat still run).",
                 m->name, c->name, (unsigned long long)o->addr, o->T);
            break;
        }
    }
}

static void parse_cheats(int mod)
{
    Mod *m = &g_mods[mod];
    if (!*m->cheats) return;
    char mb[80];
    main_bid(mb, sizeof mb);
    DirEnt *v;
    int n = fs_list(m->cheats, &v);
    for (int i = 0; i < n; i++) {
        if (v[i].is_dir || v[i].is_link || !iends(v[i].name, ".txt")) continue;
        size_t stem = strlen(v[i].name) - 4;
        int module = module_by_bid(v[i].name, stem);
        if (module != 0) {
            if (module == -1) mlog("WARNING: %s: cheats/%s: a cheat file must be named after the build id (this game: "
                                   "%.16s.txt): skipped", m->name, v[i].name, mb);
            else mlog("WARNING: %s: cheats/%s is for build id %.*s, but this game is %s (another game version?): skipped",
                      m->name, v[i].name, (int)stem, v[i].name, mb);
            continue;
        }
        char p[MAX_PATH_U8];
        join(p, sizeof p, m->cheats, v[i].name);
        u64 sz;
        u8 *d = read_file(p, MAX_CHEAT_FILE, &sz);
        if (!d) { mlog("WARNING: %s: cannot read cheats/%s: skipped", m->name, v[i].name); continue; }
        int before = g_nch;
        parse_cheat_file(mod, p, v[i].name, (const char *)d);
        free(d);
        for (int k = before; k < g_nch; k++) {
            Cheat *c = &g_ch[k];
            m->ch_total++;
            int on = c->master;
            if (m->cfg) {
                on = on || m->cfg->all_cheats;
                for (int j = 0; j < m->cfg->ncheats && !on; j++) on = ieq(m->cfg->cheats[j], c->name);
            }
            c->enabled = on && m->enabled;
            m->ch_enabled += c->enabled && !c->master;
            mlog("cheat: %s: %s\"%s\" (%d opcodes) - %s", m->name, c->master ? "master code " : "", c->name, c->nops,
                 c->enabled ? "ON" : (m->enabled ? "off (enable it in the mod config)" : "off (mod disabled)"));
        }
    }
    fs_free(v, n);
}

/* --- VM execution --- */

static u64 vm_mask(u64 v, int t) { return t == 8 ? v : v & ((1ull << (t * 8)) - 1); }
static u64 vm_addr(int mem, u64 rel) { return sw_guest_region_base(mem) + rel; }

static u64 vm_read(u64 a, int t, Cheat *c)
{
    u64 v = 0;
    if (!sw_guest_read(a, &v, (u64)t)) {
        if (!c->warned_unmapped++) mlog("note: cheat \"%s\" reads unmapped memory at 0x%llx (read as 0)", c->name, (unsigned long long)a);
        return 0;
    }
    return vm_mask(v, t);
}

static void vm_write(u64 a, int t, u64 v, Cheat *c)
{
    if (in_code(a, (u64)t)) {
        if (!c->warned_code++)
            mlog("WARNING: cheat \"%s\" writes game code at 0x%llx: skipped (code writes cannot take effect in the "
                 "recompiled game)", c->name, (unsigned long long)a);
        return;
    }
    if (!sw_guest_write(a, &v, (u64)t) && !c->warned_unmapped++)
        mlog("note: cheat \"%s\" writes unmapped memory at 0x%llx: skipped", c->name, (unsigned long long)a);
}

static int vm_cond(int cond, u64 a, u64 b)
{
    switch (cond) {
    case 1: return a > b;
    case 2: return a >= b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a == b;
    case 6: return a != b;
    }
    return 0;
}

static u64 vm_arith(int op, u64 a, u64 b)
{
    switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return b >= 64 ? 0 : a << b;
    case 4: return b >= 64 ? 0 : a >> b;
    case 5: return a & b;
    case 6: return a | b;
    case 7: return ~a;
    case 8: return a ^ b;
    case 9: return a;
    }
    return a;
}

static int vm_run(void)        /* sw_mods_cheat_tick: under g_prog_lock (the mods menu rebuilds the program live) */
{
    if (!g_nprog) return 0;
    u64 reg[16] = {0}, saved[16] = {0};
    int loop_top[16] = {0};
    int ip = 0, depth = 0;
    long budget = 1 << 20;
    while (ip < g_nprog) {
        if (--budget < 0) {
            static int warned;
            if (!warned++) mlog("WARNING: the cheats ran more than %d opcodes in one pass (endless loop?): pass stopped", 1 << 20);
            break;
        }
        const VmOp *o = &g_prog[ip++];
        Cheat *c = &g_ch[o->cheat];
        int cond_ok = 1, is_cond = 0;
        switch (o->type) {
        case OP_STORE_STATIC: vm_write(vm_addr(o->M, o->addr + reg[o->R]), o->T, vm_mask(o->val, o->T), c); break;
        case OP_BEGIN_COND:
            is_cond = 1;
            cond_ok = vm_cond(o->C, vm_read(vm_addr(o->M, o->addr + (o->flag ? reg[o->r] : 0)), o->T, c), vm_mask(o->val, o->T));
            break;
        case OP_KEY_COND: is_cond = 1; cond_ok = (sw_hid_buttons_held & o->val) == o->val; break;
        case OP_REG_COND: {
            is_cond = 1;
            u64 v = 0;
            switch (o->X) {
            case 0: v = vm_read(vm_addr(o->M, o->addr), o->T, c); break;
            case 1: v = vm_read(vm_addr(o->M, reg[o->r]), o->T, c); break;
            case 2: v = vm_read(reg[o->R] + o->addr, o->T, c); break;
            case 3: v = vm_read(reg[o->R] + reg[o->r], o->T, c); break;
            case 4: v = vm_mask(o->val, o->T); break;
            case 5: v = vm_mask(reg[o->r], o->T); break;
            }
            cond_ok = vm_cond(o->C, vm_mask(reg[o->S], o->T), v);
            break;
        }
        case OP_END_COND:
            if (o->flag) {                                     /* else reached from the "true" block: skip to end */
                int d = 0;
                while (ip < g_nprog) {
                    const VmOp *k = &g_prog[ip++];
                    if (is_begin(k)) d++;
                    else if (k->type == OP_END_COND && !k->flag) { if (d == 0) break; d--; }
                }
            }
            if (depth > 0) depth--;
            break;
        case OP_LOOP:
            if (o->flag) { reg[o->R] = o->val; loop_top[o->R] = ip; }
            else if (--reg[o->R] != 0) ip = loop_top[o->R];
            break;
        case OP_LOAD_REG_STATIC: reg[o->R] = o->val; break;
        case OP_LOAD_REG_MEM:
            reg[o->R] = vm_read(o->I ? reg[o->R] + o->addr : vm_addr(o->M, o->addr), o->T, c);
            break;
        case OP_STORE_TO_REG_ADDR:
            vm_write(reg[o->R] + (o->O ? reg[o->r] : 0), o->T, vm_mask(o->val, o->T), c);
            if (o->I) reg[o->R] += o->T;
            break;
        case OP_LEGACY_ARITH: reg[o->R] = vm_mask(vm_arith(o->C, reg[o->R], o->val), o->T); break;
        case OP_ARITH: reg[o->R] = vm_mask(vm_arith(o->C, reg[o->S], o->flag ? o->val : reg[o->r]), o->T); break;
        case OP_STORE_REG: {
            u64 a = 0;
            switch (o->O) {
            case 0: a = reg[o->R]; break;
            case 1: a = reg[o->R] + reg[o->X]; break;
            case 2: a = reg[o->R] + o->addr; break;
            case 3: a = vm_addr(o->X, reg[o->R]); break;
            case 4: a = vm_addr(o->X, o->addr); break;
            case 5: a = vm_addr(o->X, reg[o->R] + o->addr); break;
            }
            vm_write(a, o->T, vm_mask(reg[o->S], o->T), c);
            if (o->I) reg[o->R] += o->T;
            break;
        }
        case OP_SAVE_RESTORE:
            switch (o->X) {
            case 0: reg[o->R] = saved[o->S]; break;
            case 1: saved[o->R] = reg[o->S]; break;
            case 2: saved[o->R] = 0; break;
            case 3: reg[o->R] = 0; break;
            }
            break;
        case OP_SAVE_RESTORE_MASK:
            for (int i = 0; i < 16; i++) {
                if (!(o->val & (1ull << i))) continue;
                switch (o->X) {
                case 0: reg[i] = saved[i]; break;
                case 1: saved[i] = reg[i]; break;
                case 2: saved[i] = 0; break;
                case 3: reg[i] = 0; break;
                }
            }
            break;
        case OP_STATIC_REG:
            if (o->val < 0x80) reg[o->R] = g_static_regs[o->val];
            else g_static_regs[o->val] = reg[o->R];
            break;
        default: break;                                        /* pause / resume / debug log: nothing to do */
        }
        if (is_cond) {
            depth++;
            if (!cond_ok) {                                    /* skip to the matching else (continue there) or end */
                int d = 0;
                while (ip < g_nprog) {
                    const VmOp *k = &g_prog[ip++];
                    if (is_begin(k)) d++;
                    else if (k->type == OP_END_COND) {
                        if (d == 0) { if (!k->flag) depth--; break; }
                        if (!k->flag) d--;
                    }
                }
            }
        }
    }
    g_cheat_runs++;
    return 1;
}

#if defined(_WIN32)
static DWORD WINAPI cheat_thread(LPVOID arg)
{
    (void)arg;
    DWORD ms = (DWORD)(1000 / C.cheat_hz);
    for (;;) { Sleep(ms ? ms : 1); sw_mods_cheat_tick(); }
}
#else
static void *cheat_thread(void *arg)
{
    (void)arg;
    for (;;) { usleep((useconds_t)(1000000 / C.cheat_hz)); sw_mods_cheat_tick(); }
    return NULL;
}
#endif

static void build_cheat_program(void)
{
    /* Atmosphere runs one program: the master codes first, then the enabled cheats (file order).  A mod's master
     * codes set things up for its other cheats: they run only when one of those is on. */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < g_nch; i++) {
            Cheat *c = &g_ch[i];
            if (!c->enabled || c->master != (pass == 0)) continue;
            if (c->master) {
                int used = 0;
                for (int k = 0; k < g_nch && !used; k++)
                    used = g_ch[k].mod == c->mod && !g_ch[k].master && g_ch[k].enabled;
                if (!used) continue;
            }
            g_prog = (VmOp *)xrealloc(g_prog, sizeof(VmOp) * (size_t)(g_nprog + c->nops));
            for (int k = 0; k < c->nops; k++) { g_prog[g_nprog] = c->ops[k]; g_prog[g_nprog++].cheat = i; }
        }
}

void sw_mods_start_cheats(void)
{
    sw_mods_init();
    static int started;
    if (started || !g_nprog) return;
    started = 1;
    mlog("cheat VM: %d opcodes, %d times per second", g_nprog, C.cheat_hz);
#if defined(_WIN32)
    HANDLE h = CreateThread(NULL, 64 * 1024, cheat_thread, NULL, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); }
    else mlog("WARNING: cannot start the cheat thread: cheats are off");
#else
    pthread_t t;
    if (pthread_create(&t, NULL, cheat_thread, NULL) == 0) pthread_detach(t);
#endif
}

/* ================================================================== */
/* init, summary, --list-mods                                         */
/* ================================================================== */

void sw_mods_print_summary(void)
{
    if (g_summary_done || !g_inited) return;
    g_summary_done = 1;
    if (!g_nmods) return;
    int en = 0;
    for (int i = 0; i < g_nmods; i++) en += g_mods[i].enabled;
    mlog("==== mod summary: %d mod(s) found, %d enabled ====", g_nmods, en);
    for (int r = 0; r < g_nmods; r++) {
        Mod *m = &g_mods[g_order[r]];
        char rf[160] = "", px[200] = "", ch[96] = "";
        if (*m->romfs) {
            if (m->enabled)
                snprintf(rf, sizeof rf, " | romfs: %d file(s): %d replace game files, %d new, %d used from a higher-priority "
                         "mod instead, %d rejected", m->rf_files, m->rf_replaced, m->rf_added, m->rf_shadowed,
                         m->rf_bad + m->rf_rejected);
            else snprintf(rf, sizeof rf, " | romfs: %d file(s), not used", m->rf_files);
        }
        if (*m->exefs) snprintf(px, sizeof px, " | exefs: %d patch file(s): %d data record(s) applied (%d bytes), %d code "
                                "patch(es) skipped (warned), %d other skipped", m->px_files, m->px_applied, m->px_bytes,
                                m->px_code, m->px_other);
        if (*m->cheats) snprintf(ch, sizeof ch, " | cheats: %d, %d on", m->ch_total, m->ch_enabled);
        mlog("#%d %s [%s]%s%s - %s (%s)%s%s%s", m->prio, m->name, m->enabled ? "enabled" : "DISABLED",
             m->why_off ? ": " : "", m->why_off ? m->why_off : "", m->dir, m->layout, rf, px, ch);
    }
    if (g_rf_conflicts) mlog("%d romfs file(s) are provided by more than one mod: the higher priority wins (\"conflict:\" lines above)", g_rf_conflicts);
    mlog("==== end of mod summary (full log: %s) ====", C.log_path);
}

void sw_mods_romfs_built(void) { sw_mods_print_summary(); }

void sw_mods_init(void)
{
    if (g_inited) return;
    g_inited = 1;
    cfg_defaults();
    const char *exe = sw_mods_host_exe_dir();
    const char *env = getenv("SW_NO_MODS");
    if (env && *env && *env != '0') C.no_mods = 1;
    if (C.no_mods) { sw_report("[mods] mod support off (--no-mods / SW_NO_MODS)\n"); return; }
    load_config();
    /* --list-mods still lists them (the launcher shows the mods while the switch is off) */
    if (!C.enabled && !C.list) { sw_report("[mods] mod support off (\"enabled\": false in %s)\n", C.path); return; }
    collect_roots();
    int any_root = 0;
    for (int i = 0; i < g_nroots; i++) any_root |= fs_kind(g_roots[i]) == 2;
    if (!any_root) return;                                    /* no mods folder at all: nothing to do, no log file */
    if (!C.log_path[0]) join(C.log_path, sizeof C.log_path, exe, "mods.log");
    g_logf = C.list ? NULL : fopen(C.log_path, "w");
    if (g_logf && g_early) { fputs(g_early, g_logf); fflush(g_logf); }
    free(g_early);
    g_early = NULL;
    g_early_n = 0;
    mlog("mod config: %s", C.path[0] ? C.path : "(none, defaults: every mod found is enabled, priority by name)");
    for (int i = 0; i < g_nroots; i++) {
        int k = fs_kind(g_roots[i]);
        if (k == 2) { mlog("searching %s", g_roots[i]); scan_root(g_roots[i]); }
        else if (i < C.ncli_dirs + C.ndirs) mlog("WARNING: mods folder %s does not exist", g_roots[i]);
    }
    if (!g_nmods) { mlog("no mods found"); return; }
    apply_config();
    g_active = 1;
    build_romfs_index();
    for (int r = 0; r < g_nmods; r++) {
        int mi = g_order[r];
        if (!g_mods[mi].enabled) continue;
        parse_exefs(mi);
    }
    for (int r = 0; r < g_nmods; r++) parse_cheats(g_order[r]);
    build_cheat_program();
}

static void json_str(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        u8 ch = (u8)*s;
        if (ch == '"' || ch == '\\') printf("\\%c", ch);
        else if (ch < 0x20) printf("\\u%04x", ch);
        else putchar(ch);
    }
    putchar('"');
}

int sw_mods_list(void)
{
    sw_mods_init();
    char mb[80];
    printf("{\"title_id\":\"%s\",\"build_id\":", ARCEUS_TID);
    json_str(main_bid(mb, sizeof mb));
    printf(",\"enabled\":%s,\"config\":", C.no_mods || !C.enabled ? "false" : "true");
    json_str(C.path);
    printf(",\"search\":[");
    for (int i = 0; i < g_nroots; i++) { if (i) putchar(','); json_str(g_roots[i]); }
    printf("],\"conflicts\":%d,\"mods\":[", g_rf_conflicts);
    for (int r = 0; r < g_nmods; r++) {
        Mod *m = &g_mods[g_order[r]];
        if (r) putchar(',');
        printf("{\"name\":");
        json_str(m->name);
        printf(",\"path\":");
        json_str(m->dir);
        printf(",\"layout\":");
        json_str(m->layout);
        printf(",\"priority\":%d,\"enabled\":%s,\"why_off\":", m->prio, m->enabled ? "true" : "false");
        json_str(m->why_off ? m->why_off : "");
        /* code (.text) patch records and whether one hits a function the built-in FPS / camera fixes hook: the
         * launcher shows a warning (patches are parsed for enabled mods only) */
        int code = 0, fps = 0;
        for (int i = 0; i < g_npr; i++) {
            if (g_pr[i].mod != g_order[r]) continue;
            SwModuleInfo mi;
            if (!sw_mods_host_module(g_pr[i].module, &mi) || g_pr[i].off >= align_up(mi.text_size, 0x1000)) continue;
            code++;
            u64 fs = 0;
            if (sw_mods_host_function_at(mi.base + g_pr[i].off, &fs) && sw_hooks_fps_conflict(fs)) fps = 1;
            if (icontains(g_pr[i].block, "fps") || icontains(g_pr[i].block, "frame") || icontains(g_pr[i].block, "camera") ||
                icontains(g_pr[i].file, "fps") || icontains(m->name, "fps") || icontains(m->name, "60"))
                fps = 1;
        }
        printf(",\"code_patches\":%d,\"fps_conflict\":%s", code, fps ? "true" : "false");
        printf(",\"romfs\":%s,\"romfs_files\":%d,\"romfs_shadowed\":%d,\"exefs\":%s,\"exefs_files\":%d,\"cheats\":[",
               *m->romfs ? "true" : "false", m->rf_files, m->rf_shadowed, *m->exefs ? "true" : "false", m->px_files);
        int first = 1;
        for (int i = 0; i < g_nch; i++) {
            if (g_ch[i].mod != g_order[r] || g_ch[i].master) continue;
            if (!first) putchar(',');
            first = 0;
            printf("{\"name\":");
            json_str(g_ch[i].name);
            printf(",\"enabled\":%s}", g_ch[i].enabled ? "true" : "false");
        }
        printf("]}");
    }
    printf("]}\n");
    fflush(stdout);
    return 0;
}

/* ================================================================== */
/* in-game mods menu (ui/sw_overlay.cpp): list, on/off, priority, live cheats                                       */
/* ================================================================== */
/* romfs / exefs are merged when the game starts, so mod on/off and priority are saved for the next start; cheats
 * switch at once (the VM program is rebuilt under g_prog_lock).  Saving writes the mod config on a worker thread:
 * the menu runs on the present thread, which must never wait for the disk. */

#if defined(_WIN32)
static SRWLOCK g_prog_lock = SRWLOCK_INIT;
#define PROG_RD() AcquireSRWLockShared(&g_prog_lock)
#define PROG_RD_END() ReleaseSRWLockShared(&g_prog_lock)
#define PROG_WR() AcquireSRWLockExclusive(&g_prog_lock)
#define PROG_WR_END() ReleaseSRWLockExclusive(&g_prog_lock)
#else
static pthread_rwlock_t g_prog_lock = PTHREAD_RWLOCK_INITIALIZER;
#define PROG_RD() pthread_rwlock_rdlock(&g_prog_lock)
#define PROG_RD_END() pthread_rwlock_unlock(&g_prog_lock)
#define PROG_WR() pthread_rwlock_wrlock(&g_prog_lock)
#define PROG_WR_END() pthread_rwlock_unlock(&g_prog_lock)
#endif

int sw_mods_cheat_tick(void)
{
    PROG_RD();
    int r = vm_run();
    PROG_RD_END();
    return r;
}

static int *g_ui_order;        /* display order = priority for the next start (mod indexes) */
static u8 *g_ui_next;          /* on / off for the next start */
static int g_ui_ready, g_ui_changed;

static int is_marker(const Mod *m) { return m->why_off && !strcmp(m->why_off, ".disabled file in the mod folder"); }

static void ui_init(void)
{
    if (g_ui_ready) return;
    g_ui_ready = 1;
    g_ui_order = (int *)xrealloc(NULL, sizeof(int) * (size_t)(g_nmods ? g_nmods : 1));
    g_ui_next = (u8 *)xrealloc(NULL, (size_t)(g_nmods ? g_nmods : 1));
    for (int r = 0; r < g_nmods; r++) {
        int i = g_order[r];
        g_ui_order[r] = i;
        /* the config's state, not the marker file's (that is the folder's own switch) */
        g_ui_next[i] = (u8)(g_mods[i].enabled || is_marker(&g_mods[i]));
    }
}

int sw_mods_ui_count(void) { return g_nmods; }

int sw_mods_ui_get(int pos, SwModUi *o)
{
    ui_init();
    if (pos < 0 || pos >= g_nmods) return 0;
    int mi = g_ui_order[pos];
    Mod *m = &g_mods[mi];
    memset(o, 0, sizeof *o);
    o->name = m->name;
    o->layout = m->layout;
    o->dir = m->dir;
    o->why_off = m->why_off ? m->why_off : "";
    o->marker = is_marker(m);
    o->enabled_now = m->enabled;
    o->enabled_next = g_ui_next[mi] && !o->marker;
    o->romfs_files = *m->romfs ? m->rf_files : -1;
    o->romfs_replaced = m->rf_replaced;
    o->romfs_added = m->rf_added;
    o->patches_applied = m->px_applied;
    o->code_patches = m->px_code;
    for (int i = 0; i < g_nch; i++) o->ncheats += g_ch[i].mod == mi && !g_ch[i].master;
    return 1;
}

int sw_mods_ui_restart_needed(void) { return g_ui_changed; }

const char *sw_mods_ui_folder(void)
{
    static char p[MAX_PATH_U8];
    for (int i = 0; i < g_nroots; i++) if (fs_kind(g_roots[i]) == 2) return g_roots[i];
    join(p, sizeof p, sw_mods_host_exe_dir(), "../../mods");
    return p;
}

/* ---- saving the config (worker thread) ---- */
typedef struct { char *b; size_t n, cap; } Sb;

static void sb_put(Sb *s, const char *t, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        s->cap = (s->cap + n + 1) * 2;
        s->b = (char *)xrealloc(s->b, s->cap);
    }
    memcpy(s->b + s->n, t, n);
    s->n += n;
    s->b[s->n] = 0;
}
static void sb_s(Sb *s, const char *t) { sb_put(s, t, strlen(t)); }
static void sb_q(Sb *s, const char *t)
{
    sb_s(s, "\"");
    for (; *t; t++) {
        u8 ch = (u8)*t;
        char e[8];
        if (ch == '"' || ch == '\\') { e[0] = '\\'; e[1] = (char)ch; sb_put(s, e, 2); }
        else if (ch < 0x20) { snprintf(e, sizeof e, "\\u%04x", ch); sb_s(s, e); }
        else sb_put(s, (const char *)t, 1);
    }
    sb_s(s, "\"");
}

/* the keys the launcher (mods_cfg.py) and the runtime read: the mods in the menu's order, then config entries for
 * mods not found this time (kept, so a mod folder that is away for a while keeps its settings) */
static char *config_text(void)
{
    Sb s = {0};
    char t[256];
    snprintf(t, sizeof t, "{\n  \"enabled\": %s,\n  \"search_default_dirs\": %s,\n  \"search_emulator_dirs\": %s,\n"
             "  \"cheat_hz\": %d,\n  \"mods_dirs\": [", C.enabled ? "true" : "false", C.search_default ? "true" : "false",
             C.search_emulator ? "true" : "false", C.cheat_hz);
    sb_s(&s, t);
    for (int i = 0; i < C.ndirs; i++) { if (i) sb_s(&s, ", "); sb_q(&s, C.dirs[i]); }
    sb_s(&s, "],\n  \"mods\": [");
    int first = 1;
    for (int r = 0; r < g_nmods; r++) {
        int i = g_ui_order[r];
        sb_s(&s, first ? "\n    {\"name\": " : ",\n    {\"name\": ");
        first = 0;
        sb_q(&s, g_mods[i].name);
        sb_s(&s, g_ui_next[i] ? ", \"enabled\": true" : ", \"enabled\": false");
        int nc = 0;
        for (int k = 0; k < g_nch; k++) {
            if (g_ch[k].mod != i || g_ch[k].master || !g_ch[k].enabled) continue;
            sb_s(&s, nc++ ? ", " : ", \"cheats\": [");
            sb_q(&s, g_ch[k].name);
        }
        if (!g_mods[i].enabled && g_mods[i].cfg)               /* a mod that is not loaded keeps its chosen cheats */
            for (int k = 0; k < g_mods[i].cfg->ncheats; k++) {
                sb_s(&s, nc++ ? ", " : ", \"cheats\": [");
                sb_q(&s, g_mods[i].cfg->cheats[k]);
            }
        sb_s(&s, nc ? "]}" : "}");
    }
    for (int k = 0; k < C.nmods; k++) {
        int found = 0;
        for (int i = 0; i < g_nmods && !found; i++) found = g_mods[i].cfg == &C.mods[k];
        if (found) continue;
        sb_s(&s, first ? "\n    {\"name\": " : ",\n    {\"name\": ");
        first = 0;
        sb_q(&s, C.mods[k].name);
        sb_s(&s, C.mods[k].enabled == 0 ? ", \"enabled\": false}" : ", \"enabled\": true}");
    }
    sb_s(&s, "\n  ]\n}\n");
    return s.b;
}

static const char *config_target(void)
{
    static char p[MAX_PATH_U8];
    if (C.path[0] && !iends(C.path, ".txt")) return C.path;
    if (C.path[0]) {                                           /* a mods.txt: mods.json next to it (json is read first) */
        snprintf(p, sizeof p, "%.*s.json", (int)(strlen(C.path) - 4), C.path);
        return p;
    }
    char cfgdir[MAX_PATH_U8];
    join(cfgdir, sizeof cfgdir, sw_mods_host_exe_dir(), "../../Config");
    if (fs_kind(cfgdir) == 2) join(p, sizeof p, cfgdir, "mods.json");
    else join(p, sizeof p, sw_mods_host_exe_dir(), "mods.json");
    return p;
}

#if defined(_WIN32)
static SRWLOCK g_save_lock = SRWLOCK_INIT;   /* one writer at a time (worker saves and sw_mods_ui_save_sync) */
#endif

static int write_config_now(void)
{
#if defined(_WIN32)
    AcquireSRWLockExclusive(&g_save_lock);
#endif
    char *text = config_text();
    const char *dst = config_target();
    char tmp[MAX_PATH_U8 + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *f = fopen(tmp, "wb");
    int ok = f && text && fputs(text, f) >= 0;
    if (f) ok = (fclose(f) == 0) && ok;
#if defined(_WIN32)
    ok = ok && MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    ok = ok && rename(tmp, dst) == 0;
#endif
    if (ok) { snprintf(C.path, sizeof C.path, "%s", dst); mlog("mods menu: saved %s", dst); }
    else { remove(tmp); mlog("WARNING: mods menu: cannot save %s", dst); }
    free(text);
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&g_save_lock);
#endif
    return ok;
}

#if defined(_WIN32)
static volatile LONG g_save_pending;
static DWORD WINAPI save_thread(LPVOID a)
{
    (void)a;
    while (InterlockedExchange(&g_save_pending, 0)) write_config_now();
    return 0;
}
#endif

static void save_async(void)
{
#if defined(_WIN32)
    if (InterlockedExchange(&g_save_pending, 1)) return;      /* one is queued: it writes the newest state */
    HANDLE h = CreateThread(NULL, 64 * 1024, save_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else { InterlockedExchange(&g_save_pending, 0); write_config_now(); }
#else
    write_config_now();
#endif
}

int sw_mods_ui_save_sync(void) { ui_init(); return write_config_now(); }   /* tests */

/* wait for queued saves (tests; the game itself can exit at any time: saves go to a temporary file that is renamed) */
void sw_mods_ui_flush(void)
{
#if defined(_WIN32)
    while (g_save_pending) Sleep(5);
    AcquireSRWLockExclusive(&g_save_lock);
    ReleaseSRWLockExclusive(&g_save_lock);
#endif
}

void sw_mods_ui_set_enabled(int pos, int on)
{
    ui_init();
    if (pos < 0 || pos >= g_nmods) return;
    int i = g_ui_order[pos];
    if (is_marker(&g_mods[i]) || g_ui_next[i] == (on != 0)) return;
    g_ui_next[i] = (u8)(on != 0);
    g_ui_changed = 1;
    mlog("mods menu: %s %s (from the next start)", g_mods[i].name, on ? "on" : "off");
    save_async();
}

void sw_mods_ui_move(int pos, int delta)
{
    ui_init();
    int to = pos + delta;
    if (pos < 0 || pos >= g_nmods || to < 0 || to >= g_nmods) return;
    int t = g_ui_order[pos];
    g_ui_order[pos] = g_ui_order[to];
    g_ui_order[to] = t;
    g_ui_changed = 1;
    save_async();
}

/* the k-th cheat (master codes not counted) of the mod at pos */
static int ui_cheat_index(int pos, int k)
{
    if (pos < 0 || pos >= g_nmods) return -1;
    int mi = g_ui_order[pos];
    for (int i = 0; i < g_nch; i++)
        if (g_ch[i].mod == mi && !g_ch[i].master && k-- == 0) return i;
    return -1;
}

const char *sw_mods_ui_cheat(int pos, int k, int *on)
{
    ui_init();
    int i = ui_cheat_index(pos, k);
    if (i < 0) return NULL;
    if (on) *on = g_ch[i].enabled;
    return g_ch[i].name;
}

int sw_mods_ui_set_cheat(int pos, int k, int on)
{
    ui_init();
    int i = ui_cheat_index(pos, k);
    if (i < 0 || !g_mods[g_ch[i].mod].enabled) return 0;    /* not loaded this session: no live cheats */
    PROG_WR();
    g_ch[i].enabled = on != 0;
    for (int j = 0; j < g_nch; j++)                          /* its master codes go with it (used only when one is on) */
        if (g_ch[j].mod == g_ch[i].mod && g_ch[j].master) g_ch[j].enabled = 1;
    free(g_prog);
    g_prog = NULL;
    g_nprog = 0;
    build_cheat_program();
    int any = g_nprog;
    PROG_WR_END();
    mlog("mods menu: cheat \"%s\" %s", g_ch[i].name, on ? "on" : "off");
    if (any) sw_mods_start_cheats();
    save_async();
    return 1;
}
