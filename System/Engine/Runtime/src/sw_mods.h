/* Mod support (emulator-style mod folders / LayeredFS), see MODS.md.
 *
 * sw_mods.c is self-contained: it only talks to the rest of the runtime through
 * the "host interface" functions below, so Engine/Tests/test_mods.c can build it
 * (with sw_romfs.c) against stubs, without the game or the generated code. */
#ifndef SW_MODS_H
#define SW_MODS_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- host interface (sw_runtime.c / sw_hid.c / sw_hooks.c; stubbed by the test) ---- */
typedef struct SwModuleInfo {
    const char *name;          /* "main", "sdk", ... */
    uint64_t base;             /* guest address of the module image */
    uint64_t text_size, ro_size, data_size, bss_size;
    const char *module_id;     /* build id, 64 hex chars */
} SwModuleInfo;

int sw_mods_host_module_count(void);
int sw_mods_host_module(int i, SwModuleInfo *out);
/* guest function containing addr (recompiled function table): its name and start, NULL if none */
const char *sw_mods_host_function_at(uint64_t addr, uint64_t *start);
/* guest memory access that can never fault the host: 1 = done, 0 = not mapped / failed */
int sw_guest_read(uint64_t addr, void *buf, uint64_t n);
int sw_guest_write(uint64_t addr, const void *buf, uint64_t n);
/* cheat memory regions (Atmosphere MemoryAccessType): 0 main NSO, 1 heap, 2 alias, 3 aslr */
uint64_t sw_guest_region_base(int kind);
const char *sw_mods_host_exe_dir(void);
extern volatile uint64_t sw_hid_buttons_held;      /* HidNpadButton bits of the last pad poll */
/* built-in 60 fps / camera fixes (sw_hooks.c): name of the fix that hooks function fn, or NULL */
const char *sw_hooks_fps_conflict(uint64_t fn);
/* a data range the built-in frame rate modes depend on: its name, or NULL */
const char *sw_hooks_fps_data_conflict(uint64_t lo, uint64_t hi);

/* ---- runtime entry points ---- */
void sw_mods_cli(int argc, char **argv);   /* --no-mods, --mods-dir DIR, --mods-config FILE (before init) */
int sw_mods_list_requested(void);          /* --list-mods given */
int sw_mods_list(void);                    /* print the mod list as JSON on stdout (launcher), returns 0 */
void sw_mods_init(void);                   /* config, discovery, romfs index, patch + cheat parsing (once) */
void sw_mods_apply_exefs(void);            /* after the module images are mapped, before relocation */
void sw_mods_start_cheats(void);           /* cheat VM timer thread, only if a cheat is enabled */
int sw_mods_cheat_tick(void);              /* one VM pass (the thread calls it; the test calls it directly) */

/* ---- romfs overlay (used by sw_romfs.c while it builds the virtual image) ---- */
typedef struct SwModFile {
    const char *rel;           /* romfs path as the mod spells it, '/' separated, no leading '/' */
    const char *host;          /* host path (UTF-8) */
    uint64_t size;
    int mod;                   /* index of the winning mod */
    int used;                  /* matched a file of the base romfs */
} SwModFile;

int sw_mods_romfs_count(void);                         /* overlay files; 0 = no romfs mods (zero cost) */
const SwModFile *sw_mods_romfs_get(int i);
SwModFile *sw_mods_romfs_find(const char *rel);        /* case-insensitive, NULL if no mod provides it */
void sw_mods_romfs_result(int mod, int replaced, int added, int rejected);
void sw_mods_romfs_built(void);                        /* the image is built: print the summary */
const char *sw_mods_name(int mod);
FILE *sw_mods_fopen(const char *utf8_path);           /* opens a mod file (UTF-8 path) for reading */
void sw_mods_log(const char *fmt, ...);                /* console + mods.log */

/* ---- in-game mods menu (ui/sw_overlay.cpp, present thread).  pos = place in the menu's list (priority order, first
 * = highest).  On/off and moves apply from the next start (romfs/exefs are merged at start-up); cheats switch at
 * once.  Every change saves the mod config on a worker thread. */
typedef struct SwModUi {
    const char *name, *layout, *why_off;
    const char *dir;           /* the mod folder (UTF-8) */
    int marker;                /* switched off by a .disabled file in its folder (the menu cannot switch it on) */
    int enabled_now;           /* loaded in this session */
    int enabled_next;          /* on from the next start */
    int romfs_files;           /* -1: no romfs/ */
    int romfs_replaced, romfs_added;
    int patches_applied, code_patches;   /* exefs data records applied / code patches skipped */
    int ncheats;
} SwModUi;
int sw_mods_ui_count(void);
int sw_mods_ui_get(int pos, SwModUi *out);
void sw_mods_ui_set_enabled(int pos, int on);
void sw_mods_ui_move(int pos, int delta);
const char *sw_mods_ui_cheat(int pos, int k, int *on);   /* k-th cheat (no master codes), NULL past the last */
int sw_mods_ui_set_cheat(int pos, int k, int on);         /* live; 0 when the mod is not loaded this session */
int sw_mods_ui_restart_needed(void);                      /* something changed that needs a restart */
const char *sw_mods_ui_folder(void);                      /* the mods folder to show */
int sw_mods_ui_save_sync(void);                           /* tests: write the config now */
void sw_mods_ui_flush(void);                              /* tests: wait for queued saves */

#ifdef __cplusplus
}
#endif

#endif
