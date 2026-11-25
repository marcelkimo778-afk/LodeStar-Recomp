/* Virtual RomFS image over the extracted roms/romfs folder.
 *
 * The SDK opens the game's assets with fsp-srv OpenDataStorageByCurrentProcess
 * and reads a raw RomFS image through IStorage::Read.  Instead of writing a
 * ~6 GB image to disk, the image is synthesised: header + directory/file hash
 * and metadata tables are built in memory at startup, and reads in the data
 * region are served from the files on disk.
 *
 * Layout (RomFS level 3, switchbrew "RomFS"):
 *   0x00 header (0x50): header_size, dir_hash {off,size}, dir_meta {off,size},
 *                       file_hash {off,size}, file_meta {off,size}, data_off
 *   dir entry : parent, sibling, child_dir, child_file, hash_next, name_len, name
 *   file entry: parent, sibling, data_off(u64), size(u64), hash_next, name_len, name
 *   names are padded to 4 bytes; "none" is 0xFFFFFFFF; entries are referenced
 *   by their byte offset inside their meta table.  Hash:
 *     h = parent ^ 123456789; for c in name: h = ror(h, 5) ^ c; bucket = h % count
 *
 * Mods (sw_mods.c, MODS.md): while the base folder is scanned, every file is looked up in the mod overlay index
 * (one hash lookup; skipped entirely when there are no romfs mods) and a mod file replaces it with its own path and
 * size.  Mod files no base file matched are then inserted into the tree (directories created as needed, names kept
 * sorted).  The guest's SDK parses the merged tables, so open / stat / directory listing / partial reads all see the
 * mod files with their real sizes, and reads go through the same path as base files.
 */

#include "sw_rt.h"
#include "sw_mods.h"

void sw_report(const char *fmt, ...);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define RF_NONE 0xFFFFFFFFu

typedef struct {
    char *name;            /* entry name (no path) */
    char *path;            /* host path, files only */
    char *alt_path;        /* mod files: the base file it replaces (fallback if the mod file becomes unreadable) */
    u64 alt_size;
    u8 utf8, alt_utf8;     /* path is UTF-8 (mod files; base paths are in the ANSI code page) */
    int mod;               /* mod index providing the file, -1 = base game */
    int parent;            /* index in dirs */
    int is_dir;
    u64 size;              /* files */
    u64 data_off;          /* files, relative to data region */
    u32 meta_off;          /* byte offset inside its meta table */
    int first_child_dir, first_child_file, next_sibling;
} RfNode;

static RfNode *g_dirs, *g_files;
static int g_ndirs, g_nfiles, g_cap_dirs, g_cap_files;
static u8 *g_head;          /* header + tables */
static u64 g_head_size, g_data_off, g_total;
static int g_ready = -1;
static int *g_file_order;   /* files sorted by data_off (== creation order) */

static char *dupstr(const char *s) { size_t n = strlen(s) + 1; char *d = (char *)malloc(n); memcpy(d, s, n); return d; }

static int add_node(int is_dir, const char *name, const char *path, int parent, u64 size)
{
    RfNode **arr = is_dir ? &g_dirs : &g_files;
    int *n = is_dir ? &g_ndirs : &g_nfiles, *cap = is_dir ? &g_cap_dirs : &g_cap_files;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 1024;
        *arr = (RfNode *)realloc(*arr, sizeof(RfNode) * (size_t)*cap);
    }
    RfNode *e = &(*arr)[*n];
    memset(e, 0, sizeof *e);
    e->name = dupstr(name);
    e->path = path ? dupstr(path) : NULL;
    e->parent = parent;
    e->is_dir = is_dir;
    e->size = size;
    e->first_child_dir = e->first_child_file = e->next_sibling = -1;
    e->mod = -1;
    return (*n)++;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* a base file at romfs path rel: does a mod replace it? */
static void overlay_file(int fi, const char *rel)
{
    SwModFile *mf = sw_mods_romfs_find(rel);
    if (!mf) return;
    RfNode *e = &g_files[fi];
    mf->used = 1;
    e->alt_path = e->path;
    e->alt_size = e->size;
    e->path = dupstr(mf->host);
    e->utf8 = 1;
    e->size = mf->size;
    e->mod = mf->mod;
    sw_mods_romfs_result(mf->mod, 1, 0, 0);
}

/* Scan one directory (sorted by name) and recurse.  rel: its romfs path ("" = root, "bin", "bin/field", ...). */
static void scan(const char *host_dir, const char *rel, int dir_index)
{
    int overlay = sw_mods_romfs_count() > 0;
    char **names = NULL; int *isdir = NULL; u64 *sizes = NULL;
    int n = 0, cap = 0;
#if defined(_WIN32)
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*", host_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            names = (char **)realloc(names, sizeof(char *) * (size_t)cap);
            isdir = (int *)realloc(isdir, sizeof(int) * (size_t)cap);
            sizes = (u64 *)realloc(sizes, sizeof(u64) * (size_t)cap);
        }
        names[n] = dupstr(fd.cFileName);
        isdir[n] = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        sizes[n] = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(host_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[1024]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", host_dir, de->d_name);
        if (stat(p, &st)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            names = (char **)realloc(names, sizeof(char *) * (size_t)cap);
            isdir = (int *)realloc(isdir, sizeof(int) * (size_t)cap);
            sizes = (u64 *)realloc(sizes, sizeof(u64) * (size_t)cap);
        }
        names[n] = dupstr(de->d_name);
        isdir[n] = S_ISDIR(st.st_mode);
        sizes[n] = (u64)st.st_size;
        n++;
    }
    closedir(d);
#endif
    /* sort by name, keeping isdir/sizes aligned */
    int *idx = (int *)malloc(sizeof(int) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 1; i < n; i++) {                 /* insertion sort: directories are small */
        int v = idx[i], j = i - 1;
        while (j >= 0 && strcmp(names[idx[j]], names[v]) > 0) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    int last_file = -1, last_dir = -1;
    int *subdirs = (int *)malloc(sizeof(int) * (size_t)(n ? n : 1)); int nsub = 0;
    char **subpaths = (char **)malloc(sizeof(char *) * (size_t)(n ? n : 1));
    char **subrels = (char **)malloc(sizeof(char *) * (size_t)(n ? n : 1));
    for (int k = 0; k < n; k++) {
        int i = idx[k];
        char p[1024], r[1024];
        snprintf(p, sizeof p, "%s/%s", host_dir, names[i]);
        if (*rel) snprintf(r, sizeof r, "%s/%s", rel, names[i]); else snprintf(r, sizeof r, "%s", names[i]);
        if (isdir[i]) {
            int di = add_node(1, names[i], NULL, dir_index, 0);
            if (last_dir < 0) g_dirs[dir_index].first_child_dir = di; else g_dirs[last_dir].next_sibling = di;
            last_dir = di;
            subdirs[nsub] = di; subpaths[nsub] = dupstr(p); subrels[nsub] = dupstr(r); nsub++;
        } else {
            int fi = add_node(0, names[i], p, dir_index, sizes[i]);
            if (last_file < 0) g_dirs[dir_index].first_child_file = fi; else g_files[last_file].next_sibling = fi;
            last_file = fi;
            if (overlay) overlay_file(fi, r);
        }
        free(names[i]);
    }
    free(names); free(isdir); free(sizes); free(idx);
    for (int s = 0; s < nsub; s++) { scan(subpaths[s], subrels[s], subdirs[s]); free(subpaths[s]); free(subrels[s]); }
    free(subdirs); free(subpaths); free(subrels);
}

static int name_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        a++, b++;
    }
    return *a == *b;
}

/* child of directory d named name (case-insensitive: a mod's "Bin/Field" merges into the game's "bin/field") */
static int child_dir(int d, const char *name)
{
    for (int c = g_dirs[d].first_child_dir; c >= 0; c = g_dirs[c].next_sibling)
        if (name_ieq(g_dirs[c].name, name)) return c;
    return -1;
}
static int child_file(int d, const char *name)
{
    for (int c = g_dirs[d].first_child_file; c >= 0; c = g_files[c].next_sibling)
        if (name_ieq(g_files[c].name, name)) return c;
    return -1;
}

/* insert a new node into d's child list, keeping names sorted (strcmp, as scan() sorts them) */
static void link_sorted(int d, int idx, int is_dir)
{
    RfNode *arr = is_dir ? g_dirs : g_files;
    int *head = is_dir ? &g_dirs[d].first_child_dir : &g_dirs[d].first_child_file;
    int prev = -1, cur = *head;
    while (cur >= 0 && strcmp(arr[cur].name, arr[idx].name) < 0) { prev = cur; cur = arr[cur].next_sibling; }
    arr[idx].next_sibling = cur;
    if (prev < 0) *head = idx; else arr[prev].next_sibling = idx;
}

/* files only mods provide: add them (and their folders) to the tree */
static void merge_new_files(void)
{
    int n = sw_mods_romfs_count(), added = 0, rejected = 0;
    for (int i = 0; i < n; i++) {
        const SwModFile *mf = sw_mods_romfs_get(i);
        if (!mf || mf->used) continue;
        char rel[1024];
        snprintf(rel, sizeof rel, "%s", mf->rel);
        int d = 0, ok = 1;
        char *part = rel, *slash;
        while (ok && (slash = strchr(part, '/'))) {
            *slash = 0;
            int c = child_dir(d, part);
            if (c < 0) {
                if (child_file(d, part) >= 0) {
                    sw_mods_log("WARNING: %s: romfs/%s needs a folder \"%s\" where the game has a file of that name: "
                                "skipped", sw_mods_name(mf->mod), mf->rel, part);
                    ok = 0;
                    break;
                }
                c = add_node(1, part, NULL, d, 0);
                link_sorted(d, c, 1);
            }
            d = c;
            part = slash + 1;
        }
        if (ok && child_dir(d, part) >= 0) {
            sw_mods_log("WARNING: %s: romfs/%s is a file where the game has a folder of that name: skipped",
                        sw_mods_name(mf->mod), mf->rel);
            ok = 0;
        }
        if (ok && child_file(d, part) >= 0) ok = 0;           /* cannot happen (it would have matched); be safe */
        if (!ok) { sw_mods_romfs_result(mf->mod, 0, 0, 1); rejected++; continue; }
        int fi = add_node(0, part, mf->host, d, mf->size);
        g_files[fi].utf8 = 1;
        g_files[fi].mod = mf->mod;
        link_sorted(d, fi, 0);
        sw_mods_romfs_result(mf->mod, 0, 1, 0);
        added++;
    }
    if (n) sw_mods_log("romfs overlay: %d mod file(s): %d replace game files, %d new, %d rejected", n,
                       n - added - rejected, added, rejected);
}

static u32 rf_hash(u32 parent, const char *name)
{
    u32 h = parent ^ 123456789u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h = (h >> 5) | (h << 27);
        h ^= *p;
    }
    return h;
}

static u32 table_count(int n)
{
    if (n < 3) return 3;
    if (n < 19) return (u32)n | 1;
    u32 c = (u32)n;
    for (;;) {
        int ok = 1;
        for (u32 p = 2; p <= 17; p++) if (c % p == 0) { ok = 0; break; }
        if (ok) return c;
        c++;
    }
}

static u32 entry_size(const RfNode *e) { u32 nl = (u32)strlen(e->name); return (e->is_dir ? 0x18u : 0x20u) + ((nl + 3) & ~3u); }

static void put32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static void put64(u8 *p, u64 v) { memcpy(p, &v, 8); }

/* root: a raw RomFS folder (bin/, system_resource/).  flat_sysres != NULL: root is an unpacked asset dump (the
 * contents of bin/, as some dump tools write it) - it is mounted as bin/, and flat_sysres ("" = not found) as
 * system_resource/. */
static int build(const char *root, const char *flat_sysres)
{
    add_node(1, "", NULL, 0, 0);                  /* root directory, parent = itself (0) */
    if (!flat_sysres) scan(root, "", 0);
    else {
        int bin = add_node(1, "bin", NULL, 0, 0);
        g_dirs[0].first_child_dir = bin;
        if (*flat_sysres) {
            int sr = add_node(1, "system_resource", NULL, 0, 0);   /* sorted after "bin" */
            g_dirs[bin].next_sibling = sr;
            scan(root, "bin", bin);
            scan(flat_sysres, "system_resource", sr);
        } else scan(root, "bin", bin);
    }
    if (g_nfiles == 0) return 0;
    if (sw_mods_romfs_count() > 0) merge_new_files();

    /* data offsets in creation order, 16-byte aligned */
    u64 off = 0;
    for (int i = 0; i < g_nfiles; i++) {
        g_files[i].data_off = off;
        off = (off + g_files[i].size + 15) & ~15ull;
    }
    u64 data_size = off;
    /* meta offsets */
    u32 dm = 0, fm = 0;
    for (int i = 0; i < g_ndirs; i++) { g_dirs[i].meta_off = dm; dm += entry_size(&g_dirs[i]); }
    for (int i = 0; i < g_nfiles; i++) { g_files[i].meta_off = fm; fm += entry_size(&g_files[i]); }
    u32 dcount = table_count(g_ndirs), fcount = table_count(g_nfiles);
    u64 dh_off = 0x200, dh_size = (u64)dcount * 4;
    u64 dm_off = dh_off + dh_size, dm_size = dm;
    u64 fh_off = dm_off + dm_size, fh_size = (u64)fcount * 4;
    u64 fm_off = fh_off + fh_size, fm_size = fm;
    g_data_off = (fm_off + fm_size + 0x1FF) & ~0x1FFull;
    g_head_size = g_data_off;
    g_total = g_data_off + data_size;
    g_head = (u8 *)calloc(1, (size_t)g_head_size);
    u8 *h = g_head;
    put64(h + 0x00, 0x50);
    put64(h + 0x08, dh_off); put64(h + 0x10, dh_size);
    put64(h + 0x18, dm_off); put64(h + 0x20, dm_size);
    put64(h + 0x28, fh_off); put64(h + 0x30, fh_size);
    put64(h + 0x38, fm_off); put64(h + 0x40, fm_size);
    put64(h + 0x48, g_data_off);
    u32 *dhash = (u32 *)(h + dh_off), *fhash = (u32 *)(h + fh_off);
    for (u32 i = 0; i < dcount; i++) dhash[i] = RF_NONE;
    for (u32 i = 0; i < fcount; i++) fhash[i] = RF_NONE;

    for (int i = 0; i < g_ndirs; i++) {
        RfNode *e = &g_dirs[i];
        u8 *p = h + dm_off + e->meta_off;
        u32 parent_off = g_dirs[e->parent].meta_off;
        u32 b = rf_hash(parent_off, e->name) % dcount;
        u32 nl = (u32)strlen(e->name);
        put32(p + 0x00, parent_off);
        put32(p + 0x04, e->next_sibling >= 0 ? g_dirs[e->next_sibling].meta_off : RF_NONE);
        put32(p + 0x08, e->first_child_dir >= 0 ? g_dirs[e->first_child_dir].meta_off : RF_NONE);
        put32(p + 0x0C, e->first_child_file >= 0 ? g_files[e->first_child_file].meta_off : RF_NONE);
        put32(p + 0x10, dhash[b]);
        put32(p + 0x14, nl);
        memcpy(p + 0x18, e->name, nl);
        dhash[b] = e->meta_off;
    }
    for (int i = 0; i < g_nfiles; i++) {
        RfNode *e = &g_files[i];
        u8 *p = h + fm_off + e->meta_off;
        u32 parent_off = g_dirs[e->parent].meta_off;
        u32 b = rf_hash(parent_off, e->name) % fcount;
        u32 nl = (u32)strlen(e->name);
        put32(p + 0x00, parent_off);
        put32(p + 0x04, e->next_sibling >= 0 ? g_files[e->next_sibling].meta_off : RF_NONE);
        put64(p + 0x08, e->data_off);
        put64(p + 0x10, e->size);
        put32(p + 0x18, fhash[b]);
        put32(p + 0x1C, nl);
        memcpy(p + 0x20, e->name, nl);
        fhash[b] = e->meta_off;
    }
    return 1;
}

static int is_dir(const char *d, const char *sub)
{
    char p[1024];
    snprintf(p, sizeof p, sub ? "%s/%s" : "%s", d, sub ? sub : "");
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(p);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
#endif
}

/* Two RomFS layouts are accepted:
 *   raw  : <dir>/bin/..., <dir>/system_resource/...   (a full RomFS dump: console / emulator)
 *   flat : <dir>/field, <dir>/pokemon, ...            (the contents of bin/ only, e.g. roms/romfs)
 * A flat dump is mounted as bin/.  It has no system_resource/ (74 shader files the game needs); those are taken
 * from SW_ROMFS_SYSRES, <dir>/system_resource, <dir>/../system_resource or a raw dump in roms_full/romfs.
 * SW_ROMFS (the launcher's RomFS folder) is tried first. */
int sw_romfs_ready(void)
{
    if (g_ready >= 0) return g_ready;
    sw_mods_init();                              /* mod overlay index (once; nothing when there are no mods) */
    const char *env = getenv("SW_ROMFS");
    const char *cands[] = {env && *env ? env : NULL, "romfs", "../../../Game/romfs", "../../roms/romfs", "../roms/romfs", "roms/romfs",
                           "../../roms_full/romfs", "../roms_full/romfs", "roms_full/romfs"};
    g_ready = 0;
    for (int i = 0; i < (int)(sizeof cands / sizeof *cands); i++) {
        const char *d = cands[i];
        if (!d || !is_dir(d, NULL)) {
            if (d && i == 0) sw_report("[romfs] SW_ROMFS=%s: not a folder\n", d);
            continue;
        }
        static char up[1024];   /* the folder above a dump (<dir>/romfs/...) was chosen: use <dir>/romfs */
        if (!is_dir(d, "bin") && is_dir(d, "romfs")) { snprintf(up, sizeof up, "%s/romfs", d); d = up; }
        int raw = is_dir(d, "bin");
        int flat = !raw && (is_dir(d, "field") || is_dir(d, "pokemon") || is_dir(d, "archive"));
        if (!raw && !flat) {
            sw_report("[romfs] %s: neither bin/ nor game asset folders (field/, pokemon/, ...): skipped\n", d);
            continue;
        }
        const char *sysres = NULL;
        static char sr[1024];
        if (flat) {
            const char *e = getenv("SW_ROMFS_SYSRES");
            char c1[1024], c2[1024];
            snprintf(c1, sizeof c1, "%s/system_resource", d);
            snprintf(c2, sizeof c2, "%s/../system_resource", d);
            const char *sc[] = {e && *e ? e : NULL, c1, c2, "../../roms_full/romfs/system_resource",
                                "../roms_full/romfs/system_resource", "roms_full/romfs/system_resource"};
            sr[0] = 0;
            for (int k = 0; k < (int)(sizeof sc / sizeof *sc) && !sr[0]; k++)
                if (sc[k] && is_dir(sc[k], NULL)) snprintf(sr, sizeof sr, "%s", sc[k]);
            sysres = sr;
        }
        if (build(d, sysres)) {
            g_ready = 1;
            sw_report("[romfs] virtual image over %s (%s layout): %d dirs, %d files, %.2f GB\n", d,
                      raw ? "raw" : "flat, mounted as bin/", g_ndirs, g_nfiles,
                      (double)g_total / (1024.0 * 1024 * 1024));
            if (flat && sr[0]) sw_report("[romfs] system_resource/ from %s\n", sr);
            sw_mods_romfs_built();
        }
        if (g_ready && ((flat && !sr[0]) || (raw && !is_dir(d, "system_resource")))) {
            /* the game crashes at start without these 74 shader files (tested 2026-11-12): say so instead */
            const char *msg =
                "Your RomFS dump has no system_resource folder (74 shader files the game needs).\n\n"
                "Some emulator dumps contain only the bin folder. Dump the full RomFS again\n"
                "(it holds romfs/system_resource), then use 'Import game files' in the launcher.";
            sw_report("[romfs] ERROR: %s: system_resource/ missing - cannot start\n", d);
#if defined(_WIN32)
            if (!getenv("SW_NO_DIALOG")) MessageBoxA(NULL, msg, "Pokemon Legends: Arceus - game files incomplete",
                                                     MB_OK | MB_ICONERROR | MB_TOPMOST);
#endif
            fflush(NULL);
            exit(3);
        }
        break;
    }
    if (!g_ready) sw_report("[romfs] no extracted romfs folder found (set SW_ROMFS)\n");
    return g_ready;
}

u64 sw_romfs_size(void) { return sw_romfs_ready() ? g_total : 0; }

/* small cache of open files */
#define RF_FCACHE 16
static struct { int idx; FILE *f; u64 use; } g_fc[RF_FCACHE];
static u64 g_fc_clock;

static FILE *open_file(int idx)
{
    int victim = 0;
    for (int i = 0; i < RF_FCACHE; i++) {
        if (g_fc[i].f && g_fc[i].idx == idx) { g_fc[i].use = ++g_fc_clock; return g_fc[i].f; }
        if (!g_fc[i].f || g_fc[i].use < g_fc[victim].use) victim = i;
    }
    if (g_fc[victim].f) fclose(g_fc[victim].f);
    RfNode *e = &g_files[idx];
    g_fc[victim].f = e->utf8 ? sw_mods_fopen(e->path) : fopen(e->path, "rb");
    if (!g_fc[victim].f && e->mod >= 0) {
        /* a mod file that became unreadable while the game runs: use the original if it has the same size (the
         * image's tables already carry the mod's size), otherwise the read returns zeros */
        if (e->alt_path && e->alt_size == e->size) {
            sw_mods_log("WARNING: %s: %s cannot be opened any more: using the original file", sw_mods_name(e->mod), e->path);
            e->path = e->alt_path;
            e->utf8 = e->alt_utf8;
            e->alt_path = NULL;
            e->mod = -1;
            g_fc[victim].f = fopen(e->path, "rb");
        } else {
            static int warned;
            if (warned++ < 20)
                sw_mods_log("WARNING: %s: %s cannot be opened any more (the game reads zeros)", sw_mods_name(e->mod), e->path);
        }
    }
    g_fc[victim].idx = idx;
    g_fc[victim].use = ++g_fc_clock;
    return g_fc[victim].f;
}

/* ---- Render distance (built in): the level 0..4 (Original, 2x, 3x, 5x, 10x) --------------------------------
 * SW_DRAW_DISTANCE at start (the launcher), else the in-game menu's last choice (Config/ingame_settings.json).  The
 * game systems are changed by sw_hooks.c (trees live, Jubilife Village people and Pokemon when the village loads);
 * this keeps the level and the choice. */
static volatile int g_dd_level = -1;

/* Config/ingame_settings.json: the in-game menu's choice (the launcher adopts it too) */
static void dd_settings_path(char *p, size_t n)
{
    const char *mc = getenv("SW_MODS_CONFIG");                  /* <Config>/mods.json from the launcher */
    if (mc && *mc) {
        snprintf(p, n, "%s", mc);
        char *s1 = strrchr(p, '\\'), *s2 = strrchr(p, '/'), *sl = s1 > s2 ? s1 : s2;
        if (sl) { snprintf(sl + 1, n - (size_t)(sl + 1 - p), "ingame_settings.json"); return; }
    }
    snprintf(p, n, "../../../Settings/ingame_settings.json");   /* cwd = System/build_work/out */
}
/* The in-game settings file holds every in-game choice ({"draw_distance": N, "frame_gen": N, "changed_at": t}): a
 * write keeps the other keys.  -1 when the key is not there. */
static const char *const kIngameKeys[] = {"draw_distance", "frame_gen", "detail_distance", "shadow_quality", "npc_distance", "perf_overlay", "anisotropy", "present_mode", "render_scale", "upscaler", "sharpness", "colors", "dlaa"};
#define N_INGAME_KEYS (int)(sizeof kIngameKeys / sizeof *kIngameKeys)
static void ingame_read(int out[N_INGAME_KEYS])
{
    for (int i = 0; i < N_INGAME_KEYS; i++) out[i] = -1;
    char p[1024];
    dd_settings_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    char b[1024] = {0};
    fread(b, 1, sizeof b - 1, f);
    fclose(f);
    for (int i = 0; i < N_INGAME_KEYS; i++) {
        char q[64];
        snprintf(q, sizeof q, "\"%s\"", kIngameKeys[i]);
        const char *k = strstr(b, q);
        if (k && (k = strchr(k, ':'))) out[i] = atoi(k + 1);
    }
}
int sw_ingame_get(const char *key)
{
    int v[N_INGAME_KEYS];
    ingame_read(v);
    for (int i = 0; i < N_INGAME_KEYS; i++) if (!strcmp(key, kIngameKeys[i])) return v[i];
    return -1;
}
void sw_ingame_set(const char *key, int value)
{
    static SRWLOCK lk = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lk);
    int v[N_INGAME_KEYS];
    ingame_read(v);
    for (int i = 0; i < N_INGAME_KEYS; i++) if (!strcmp(key, kIngameKeys[i])) v[i] = value;
    char p[1024];
    dd_settings_path(p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (f) {
        fprintf(f, "{\n");
        for (int i = 0; i < N_INGAME_KEYS; i++) if (v[i] >= 0) fprintf(f, "  \"%s\": %d,\n", kIngameKeys[i], v[i]);
        fprintf(f, "  \"changed_at\": %lld\n}\n", (long long)time(NULL));
        fclose(f);
    }
    ReleaseSRWLockExclusive(&lk);
}
int sw_drawdist_get(void)
{
    if (g_dd_level < 0) {
        const char *e = getenv("SW_DRAW_DISTANCE");
        int v = e && *e ? atoi(e) : sw_ingame_get("draw_distance");
        g_dd_level = v < 0 ? 0 : v > 4 ? 4 : v;
    }
    return g_dd_level;
}
void (*sw_drawdist_live_hook)(void);   /* sw_hooks.c: applies the level to the running game (trees at once) */
void sw_drawdist_set(int level)
{
    g_dd_level = level < 0 ? 0 : level > 4 ? 4 : level;
    sw_ingame_set("draw_distance", g_dd_level);       /* kept for the next start too */
    if (sw_drawdist_live_hook) sw_drawdist_live_hook();
    sw_report("[romfs] render distance level %d\n", g_dd_level);
}

/* Detail distance (built in): the level 0..4 (Original, 2x, 3x, 5x, 10x) of the model detail distances - rocks,
 * props and the other field objects (sw_hooks.c, the model LOD picker; live).  SW_DETAIL_DISTANCE at start, else the
 * in-game menu's last choice. */
static volatile int g_detail_level = -1;
int sw_detail_get(void)
{
    if (g_detail_level < 0) {
        const char *e = getenv("SW_DETAIL_DISTANCE");
        int v = e && *e ? atoi(e) : sw_ingame_get("detail_distance");
        g_detail_level = v < 0 ? 0 : v > 4 ? 4 : v;
    }
    return g_detail_level;
}
void (*sw_detail_live_hook)(void);   /* sw_hooks.c: applies the level (the screen-size thresholds) */
void sw_detail_set(int level)
{
    g_detail_level = level < 0 ? 0 : level > 4 ? 4 : level;
    sw_ingame_set("detail_distance", g_detail_level);
    if (sw_detail_live_hook) sw_detail_live_hook();
    sw_report("[romfs] detail distance level %d\n", g_detail_level);
}

/* People & Pokemon distance (built in): level 0..4 - how far around the camera people and Pokemon stay in the scene
 * when out of view (sw_hooks.c cull_wrap; live) and Jubilife Village's load radius (when the village loads).
 * SW_NPC_DISTANCE at start, else the in-game menu's last choice. */
static volatile int g_npc_level = -1;
int sw_npcdist_get(void)
{
    if (g_npc_level < 0) {
        const char *e = getenv("SW_NPC_DISTANCE");
        int v = e && *e ? atoi(e) : sw_ingame_get("npc_distance");
        g_npc_level = v < 0 ? 0 : v > 4 ? 4 : v;
    }
    return g_npc_level;
}
void sw_npcdist_set(int level)
{
    g_npc_level = level < 0 ? 0 : level > 4 ? 4 : level;
    sw_ingame_set("npc_distance", g_npc_level);
    sw_report("[romfs] people and pokemon distance level %d\n", g_npc_level);
}

/* Shadow quality (built in): the sun shadow maps 1..4 times larger on each side (gfx vk_texture.c shadow_scale, the
 * ported TOTK_SHADOW_SCALE).  A change applies live (vk_texture_shadow_set: maps remade at the next submit).  SW_SHADOW_SCALE at start
 * (the launcher), else the in-game menu's last choice; sw_shadow_init puts the choice into SW_SHADOW_SCALE for the
 * renderer before it makes the maps. */
static volatile int g_shadow_start = -1, g_shadow_next = -1;
void sw_shadow_init(void)
{
    if (g_shadow_start >= 0) return;
    const char *e = getenv("SW_SHADOW_SCALE");
    int v = e && *e ? atoi(e) : sw_ingame_get("shadow_quality");
    v = v < 1 ? 1 : v > 4 ? 4 : v;
    char b[32];
    snprintf(b, sizeof b, "SW_SHADOW_SCALE=%d", v);
    _putenv(b);
    g_shadow_start = g_shadow_next = v;
}
int sw_shadow_get(void) { sw_shadow_init(); return g_shadow_next; }          /* the choice (next start) */
int sw_shadow_running(void) { sw_shadow_init(); return g_shadow_start; }     /* the size in use */
void sw_shadow_set(int scale)
{
    sw_shadow_init();
    g_shadow_next = scale < 1 ? 1 : scale > 4 ? 4 : scale;
    sw_ingame_set("shadow_quality", g_shadow_next);
    { extern void vk_texture_shadow_set(int s); vk_texture_shadow_set(g_shadow_next); }   /* live: gfx vk_texture.c */
    g_shadow_start = g_shadow_next;
    sw_report("[romfs] shadow quality %dx (live)\n", g_shadow_next);
}

/* Read [off, off+size) of the virtual image into buf; returns bytes produced. */
static u64 romfs_read_locked(u64 off, void *buf, u64 size);
/* The file-handle cache (g_fc) and the files' positions are shared: one read at a time.  The IPC read calls this
 * without the kernel lock (sw_ipc.c), so a 30 ms read no longer stops every other guest thread's syscalls. */
#if defined(_WIN32)
static SRWLOCK g_read_lock = SRWLOCK_INIT;
#endif
u64 sw_romfs_read(u64 off, void *buf, u64 size)
{
#if defined(_WIN32)
    AcquireSRWLockExclusive(&g_read_lock);
    u64 r = romfs_read_locked(off, buf, size);
    ReleaseSRWLockExclusive(&g_read_lock);
    return r;
#else
    return romfs_read_locked(off, buf, size);
#endif
}

static u64 romfs_read_locked(u64 off, void *buf, u64 size)
{
    if (!sw_romfs_ready() || off >= g_total) return 0;
    if (off + size > g_total) size = g_total - off;
    u8 *out = (u8 *)buf;
    memset(out, 0, (size_t)size);
    u64 end = off + size;
    if (off < g_head_size) {
        u64 n = (end < g_head_size ? end : g_head_size) - off;
        memcpy(out, g_head + off, (size_t)n);
    }
    if (end <= g_data_off) return size;
    u64 d0 = off > g_data_off ? off - g_data_off : 0, d1 = end - g_data_off;
    /* first file whose data ends after d0 (files are in data_off order) */
    int lo = 0, hi = g_nfiles - 1, first = g_nfiles;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_files[mid].data_off + g_files[mid].size > d0) { first = mid; hi = mid - 1; }
        else lo = mid + 1;
    }
    for (int i = first; i < g_nfiles && g_files[i].data_off < d1; i++) {
        RfNode *e = &g_files[i];
        u64 s = e->data_off > d0 ? e->data_off : d0;
        u64 t = e->data_off + e->size < d1 ? e->data_off + e->size : d1;
        if (t <= s) continue;
        static int log_reads = -1;
        if (log_reads < 0) log_reads = getenv("SW_ROMFS_LOG") != NULL;
        if (log_reads) sw_report("[romfs] read %s +0x%llx (0x%llx bytes)\n", e->path,
                                 (unsigned long long)(s - e->data_off), (unsigned long long)(t - s));
        {   /* SW_NEWFN=1: files read for the first time after F8 (sw_runtime.c first-call trace) */
            extern int sw_newfn_mode; double sw_newfn_time(void);
            static u8 *seen;
            if (sw_newfn_mode && !seen) seen = (u8 *)calloc((size_t)g_nfiles, 1);
            if (seen && !seen[i]) {
                seen[i] = 1;
                if (sw_newfn_mode == 2) sw_report("[newfile] t=%.3f %s (0x%llx bytes)\n", sw_newfn_time(), e->path,
                                                  (unsigned long long)e->size);
            }
        }
        FILE *f = open_file(i);
        if (!f) continue;
        _fseeki64(f, (long long)(s - e->data_off), SEEK_SET);
        size_t got = fread(out + (g_data_off + s - off), 1, (size_t)(t - s), f);
        if (got != (size_t)(t - s) && e->mod >= 0) {
            static int warned;
            if (warned++ < 20)
                sw_mods_log("WARNING: %s: %s is shorter than when the game started (changed on disk?): the rest reads "
                            "as zeros", sw_mods_name(e->mod), e->path);
        }
    }
    return size;
}
