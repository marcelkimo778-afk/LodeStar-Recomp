/* NVN entry points (stage 1: discovery).
 *
 * The game reaches the GPU only through NVN.  It resolves every NVN function by
 * name at startup:
 *
 *     PFNNVNGENERICFUNCPTRPROC nvnBootstrapLoader(const char *name);
 *     PFNNVNGENERICFUNCPTRPROC nvnDeviceGetProcAddress(const NVNdevice *, const char *name);
 *
 * Both return a *guest* function pointer.  This file hands out one native thunk
 * address per requested name (NVN_PROC_BASE + 8*i).  When the game calls one,
 * nvn_proc_stub() logs the name and arguments and returns a neutral value, and
 * an exit-time summary lists every NVN function the game requested and called.
 * That list is the work order for the NVN -> OpenGL layer (stage 2), where
 * individual names get real implementations via nvn_bind().
 */

#include "sw_rt.h"
#include "sw_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NVN_PROC_BASE   0xFF100000ULL
#define NVN_MAX_PROCS   4096
#define NVN_NAME_MAX    96
#define NVN_LOG_CALLS   3      /* per-name calls logged in full */

/* Fixed addresses that the relocation pass binds the two imports to. */
#define NVN_BOOTSTRAP_ADDR    0xFF001000ULL
#define NVN_GETPROC_ADDR      0xFF001008ULL

typedef void (*NvnImpl)(SwCpu *c);

typedef struct {
    char name[NVN_NAME_MAX];
    u64 calls;
    NvnImpl impl;              /* NULL -> logging stub */
} NvnProc;

static NvnProc g_procs[NVN_MAX_PROCS];
static u32 g_proc_count;
static int g_report_registered;

static void read_guest_cstr(SwCpu *c, u64 addr, char *out, size_t cap)
{
    size_t i = 0;
    if (addr) {
        for (; i + 1 < cap; i++) {
            u8 *p = sw_mem_ptr(c, addr + i, 1, 0);
            if (!p || !*p) break;
            out[i] = (char)*p;
        }
    }
    out[i] = 0;
}

static int cmp_calls(const void *a, const void *b)
{
    const NvnProc *x = *(const NvnProc *const *)a, *y = *(const NvnProc *const *)b;
    return (x->calls < y->calls) - (x->calls > y->calls);
}

static void nvn_report(void)
{
    if (!g_proc_count) return;
    NvnProc *sorted[NVN_MAX_PROCS];
    u32 called = 0;
    for (u32 i = 0; i < g_proc_count; i++) {
        sorted[i] = &g_procs[i];
        if (g_procs[i].calls) called++;
    }
    qsort(sorted, g_proc_count, sizeof(sorted[0]), cmp_calls);
    fprintf(stderr, "\n=== NVN summary: %u procs requested, %u called ===\n", g_proc_count, called);
    for (u32 i = 0; i < g_proc_count && sorted[i]->calls; i++)
        fprintf(stderr, "  %10llu  %s%s\n", (unsigned long long)sorted[i]->calls,
                sorted[i]->name, sorted[i]->impl ? "" : "  (stub)");
}

static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* Generic handler for every NVN proc without a real implementation. */
static void nvn_proc_stub(SwCpu *c)
{
    u64 idx = (sw_dispatch_target() - NVN_PROC_BASE) / 8;
    if (idx >= g_proc_count) {
        sw_trap(c, sw_dispatch_target(), "NVN thunk out of range");
        return;
    }
    NvnProc *p = &g_procs[idx];
    p->calls++;
    if (p->impl) {
        p->impl(c);
        return;
    }
    if (p->calls <= NVN_LOG_CALLS)
        fprintf(stderr, "nvn: %s(0x%llx, 0x%llx, 0x%llx, 0x%llx) from 0x%llx [stub]\n", p->name,
                (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                (unsigned long long)c->x[30]);
    /* Builders/objects report success so initialisation keeps going; queries
     * return 0.  This is deliberately crude -- it exists to find the next
     * function that needs a real implementation, not to emulate the GPU. */
    c->x[0] = ends_with(p->name, "Initialize") ? 1 : 0;
}

/* ------------------------------------------------------------------------
 * Stage 2 (first pixels): the handful of functions needed for the game's own
 * clear colour to reach the window.  Everything else stays a logging stub.
 * ---------------------------------------------------------------------- */

/* Tegra X1 device constants, indexed by NVNdeviceInfo.  Taken from
 * totk-pc-recomp runtime/nvn_api_core.c (MIT, (c) 2026 Valiantsin Dzerakh),
 * which cites nnheaders nvn_DeviceConstantsNX.h. */
static const int g_devinfo[] = {
    55, 15, 14, 65536, 256, 8, 16, 4, 16, 32, 16, 4, 4, 4, 16, 32, 32, 256, 256,
    4, 8, 512, 256, 2, 256, 4096, 131072, 16, 1536, 1024, 64, 1536, 65535, 65535, 65535,
    8, 1048576, 4096, 16, 4096, 1, 1, 1, 262144, 15, 2048, 1, 1, 14, 5, 1, 8,
    8, 4, 32, 128, 32, 128, 65536, 1, 4, 16384, 134217728, 2048, 16384, 16384, 1, 1, 1,
    1, 5, 1, 1, 1, 1, 1, 1, 16, 1024, 32, 4096, 65536, 65536, 256, 16384,
    262144, 4096, 1, 4, 2, 0, 4096, 16384, 4096, 128, 4096,
};

static void put32(SwCpu *c, u64 addr, u32 v)
{
    u8 *p = addr ? sw_mem_ptr(c, addr, 4, 1) : NULL;
    if (p) memcpy(p, &v, 4);
}

/* Small guest-address -> (storage, size) map for memory pools and builders. */
#define NVN_MAP_SLOTS 4096
typedef struct { u64 key, mem, size; } NvnPoolInfo;
static NvnPoolInfo g_pools[NVN_MAP_SLOTS];

static NvnPoolInfo *pool_slot(u64 key, int create)
{
    u32 h = (u32)((key >> 3) * 2654435761u) & (NVN_MAP_SLOTS - 1);
    for (u32 i = 0; i < NVN_MAP_SLOTS; i++, h = (h + 1) & (NVN_MAP_SLOTS - 1)) {
        if (g_pools[h].key == key) return &g_pools[h];
        if (!g_pools[h].key) {
            if (!create) return NULL;
            g_pools[h].key = key;
            return &g_pools[h];
        }
    }
    return NULL;
}

/* void nvnDeviceGetInteger(const NVNdevice *d, NVNdeviceInfo what, int *out) */
static void nvn_DeviceGetInteger(SwCpu *c)
{
    u32 what = (u32)c->x[1];
    int v = what < sizeof g_devinfo / sizeof g_devinfo[0] ? g_devinfo[what] : 0;
    put32(c, c->x[2], (u32)v);
}

/* void nvnMemoryPoolBuilderSetStorage(NVNmemoryPoolBuilder *b, void *memory, size_t size) */
static void nvn_MemoryPoolBuilderSetStorage(SwCpu *c)
{
    NvnPoolInfo *p = pool_slot(c->x[0], 1);
    if (p) { p->mem = c->x[1]; p->size = c->x[2]; }
}

/* NVNboolean nvnMemoryPoolInitialize(NVNmemoryPool *pool, const NVNmemoryPoolBuilder *b) */
static void nvn_MemoryPoolInitialize(SwCpu *c)
{
    NvnPoolInfo *b = pool_slot(c->x[1], 0), *p = pool_slot(c->x[0], 1);
    if (p && b) { p->mem = b->mem; p->size = b->size; }
    c->x[0] = 1;
}

/* void *nvnMemoryPoolMap(const NVNmemoryPool *pool): the game's own storage */
static void nvn_MemoryPoolMap(SwCpu *c)
{
    NvnPoolInfo *p = pool_slot(c->x[0], 0);
    c->x[0] = p ? p->mem : 0;
}

/* NVNbufferAddress nvnMemoryPoolGetBufferAddress(const NVNmemoryPool *pool):
 * no real GPU address space yet, so the CPU address doubles as the GPU one. */
static void nvn_MemoryPoolGetBufferAddress(SwCpu *c)
{
    NvnPoolInfo *p = pool_slot(c->x[0], 0);
    c->x[0] = p ? p->mem : 0;
}

static void nvn_MemoryPoolGetSize(SwCpu *c)
{
    NvnPoolInfo *p = pool_slot(c->x[0], 0);
    c->x[0] = p ? p->size : 0;
}

/* NVNwindowAcquireTextureResult nvnWindowAcquireTexture(NVNwindow *w, NVNsync *s, int *index)
 * NVNqueueAcquireTextureResult  nvnQueueAcquireTexture(NVNqueue *q, NVNwindow *w, int *index)
 * Two images, alternating; result 0 = success. */
static void nvn_AcquireTexture(SwCpu *c)
{
    static u32 next;
    put32(c, c->x[2], next++ & 1);
    c->x[0] = 0;
}

/* void nvnCommandBufferClearColor(NVNcommandBuffer *cmd, int index, const float *rgba, int mask)
 * Command buffers are replayed at submit on real hardware; for now the most
 * recent clear of render target 0 is taken as the frame's background. */
static float g_clear_rgba[4] = {0, 0, 0, 1};
static int g_clear_seen;
static void nvn_CommandBufferClearColor(SwCpu *c)
{
    u8 *p = c->x[2] ? sw_mem_ptr(c, c->x[2], 16, 0) : NULL;
    if ((int)c->x[1] == 0 && p) {
        memcpy(g_clear_rgba, p, 16);
        if (!g_clear_seen++)
            fprintf(stderr, "nvn: first ClearColor rgba(%.3f, %.3f, %.3f, %.3f)\n",
                    g_clear_rgba[0], g_clear_rgba[1], g_clear_rgba[2], g_clear_rgba[3]);
    }
}

/* void nvnQueuePresentTexture(NVNqueue *q, NVNwindow *w, int index) */
static void nvn_QueuePresentTexture(SwCpu *c)
{
    (void)c;
    sw_gfx_set_clear_color(g_clear_rgba[0], g_clear_rgba[1], g_clear_rgba[2], 1.0f);
    sw_window_present();
    long n = sw_window_present_count();
    if (n == 1 || n == 60 || n % 600 == 0)
        fprintf(stderr, "nvn: present #%ld, clear rgba(%.3f, %.3f, %.3f)\n", n,
                g_clear_rgba[0], g_clear_rgba[1], g_clear_rgba[2]);
}

/* ------------------------------------------------------------------------
 * Stage 3: object state so renderer initialisation completes.  State lives
 * in a host map keyed by the guest object's address (the game's opaque NVN
 * structs are never written).  Values are what a game needs to keep going:
 * real addresses/sizes, always-signalled syncs, generous command memory.
 * ---------------------------------------------------------------------- */
#define NVN_OBJ_SLOTS 65536
typedef struct { u64 key; u64 v[8]; } NvnObj;
static NvnObj *g_objs;

static NvnObj *obj(u64 key, int create)
{
    if (!g_objs) g_objs = (NvnObj *)calloc(NVN_OBJ_SLOTS, sizeof(NvnObj));
    if (!key) return NULL;
    u32 h = (u32)((key >> 3) * 2654435761u) & (NVN_OBJ_SLOTS - 1);
    for (u32 i = 0; i < NVN_OBJ_SLOTS; i++, h = (h + 1) & (NVN_OBJ_SLOTS - 1)) {
        if (g_objs[h].key == key) return &g_objs[h];
        if (!g_objs[h].key) {
            if (!create) return NULL;
            g_objs[h].key = key;
            return &g_objs[h];
        }
    }
    return NULL;
}
static u64 pool_mem(u64 pool) { NvnPoolInfo *p = pool_slot(pool, 0); return p ? p->mem : 0; }

/* buffers: v0 pool, v1 offset, v2 size */
static void nvn_BufferBuilderSetStorage(SwCpu *c)
{ NvnObj *o = obj(c->x[0], 1); if (o) { o->v[0] = c->x[1]; o->v[1] = c->x[2]; o->v[2] = c->x[3]; } }
static void nvn_BufferInitialize(SwCpu *c)
{
    NvnObj *b = obj(c->x[1], 0), *o = obj(c->x[0], 1);
    if (o && b) memcpy(o->v, b->v, sizeof o->v);
    c->x[0] = 1;
}
static void nvn_BufferGetAddress(SwCpu *c)
{ NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? pool_mem(o->v[0]) + o->v[1] : 0; }
static void nvn_BufferGetSize(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[2] : 0; }
static void nvn_BufferBuilderGetSize(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[2] : 0; }

/* textures: v0 width, v1 height, v2 depth, v3 format, v4 levels, v5 pool, v6 offset, v7 target */
static u32 fmt_bits(u32 f)
{
    if (f >= 0x41 && f <= 0x50) return (f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46 || f == 0x49 || f == 0x4A) ? 4 : 8;
    if (f >= 0x79) return 8;                                 /* ASTC: <= 1 byte per pixel */
    if (f <= 0x04 || f == 0x31) return 8;
    if (f <= 0x09 || (f >= 0x0D && f <= 0x10) || f == 0x32 || (f >= 0x39 && f <= 0x3C)) return 16;
    if (f >= 0x16 && f <= 0x18) return 64;
    if (f >= 0x19 && f <= 0x1C) return 32;                   /* RGB8 padded */
    if (f >= 0x1D && f <= 0x21) return 64;
    if (f >= 0x22 && f <= 0x24) return 128;
    if (f >= 0x29 && f <= 0x2D) return 64;
    if (f >= 0x2E && f <= 0x30) return 128;
    if (f == 0x36) return 64;
    return 32;
}
static u64 tex_size(const NvnObj *o)
{
    u64 w = o->v[0] ? o->v[0] : 1, h = o->v[1] ? o->v[1] : 1, d = o->v[2] ? o->v[2] : 1;
    u64 levels = o->v[4] ? o->v[4] : 1, total = 0;
    u32 bits = fmt_bits((u32)o->v[3]);
    for (u64 l = 0; l < levels; l++) {
        u64 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = o->v[7] == 3 ? (d >> l ? d >> l : 1) : d;
        total += ((lw + 3) & ~3ull) * ((lh + 3) & ~3ull) * ld * bits / 8;
    }
    return (total + 0x1FFFF) & ~0x1FFFFull;                  /* generous, 128 KB granular */
}
static void nvn_TextureBuilderSetDefaults(SwCpu *c)
{ NvnObj *o = obj(c->x[0], 1); if (o) { memset(o->v, 0, sizeof o->v); o->v[0] = o->v[1] = o->v[2] = o->v[4] = 1; o->v[3] = 0x25; o->v[7] = 1; } }
static void nvn_TextureBuilderSetSize1D(SwCpu *c) { NvnObj *o = obj(c->x[0], 1); if (o) { o->v[0] = c->x[1]; o->v[1] = o->v[2] = 1; } }
static void nvn_TextureBuilderSetSize2D(SwCpu *c) { NvnObj *o = obj(c->x[0], 1); if (o) { o->v[0] = c->x[1]; o->v[1] = c->x[2]; o->v[2] = 1; } }
static void nvn_TextureBuilderSetSize3D(SwCpu *c) { NvnObj *o = obj(c->x[0], 1); if (o) { o->v[0] = c->x[1]; o->v[1] = c->x[2]; o->v[2] = c->x[3]; } }
static void nvn_TextureBuilderSetFormat(SwCpu *c) { NvnObj *o = obj(c->x[0], 1); if (o) o->v[3] = (u32)c->x[1]; }
static void nvn_TextureBuilderSetLevels(SwCpu *c) { NvnObj *o = obj(c->x[0], 1); if (o) o->v[4] = (u32)c->x[1]; }
static void nvn_TextureBuilderSetTarget(SwCpu *c) { NvnObj *o = obj(c->x[0], 1); if (o) o->v[7] = (u32)c->x[1]; }
static void nvn_TextureBuilderSetStorage(SwCpu *c) { NvnObj *o = obj(c->x[0], 1); if (o) { o->v[5] = c->x[1]; o->v[6] = c->x[2]; } }
static void nvn_TextureBuilderGetStorageSize(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? tex_size(o) : 0x20000; }
static void nvn_TextureBuilderGetStorageAlignment(SwCpu *c) { (void)c; c->x[0] = 0x200; }
static void nvn_TextureBuilderGetWidth(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[0] : 0; }
static void nvn_TextureBuilderGetHeight(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[1] : 0; }
static void nvn_TextureBuilderGetDepth(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[2] : 0; }
static void nvn_TextureBuilderGetFormat(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[3] : 0; }
static void nvn_TextureBuilderGetLevels(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[4] : 0; }
static void nvn_TextureInitialize(SwCpu *c)
{
    NvnObj *b = obj(c->x[1], 0), *o = obj(c->x[0], 1);
    if (o && b) memcpy(o->v, b->v, sizeof o->v);
    c->x[0] = 1;
}
static void nvn_TextureGetWidth(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[0] : 0; }
static void nvn_TextureGetHeight(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[1] : 0; }
static void nvn_TextureGetDepth(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[2] : 0; }
static void nvn_TextureGetFormat(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[3] : 0; }
static void nvn_TextureGetLevels(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[4] : 0; }
static void nvn_TextureGetTarget(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[7] : 1; }
static void nvn_TextureGetStorageSize(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? tex_size(o) : 0; }
static void nvn_TextureGetTextureAddress(SwCpu *c)
{ NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? pool_mem(o->v[5]) + o->v[6] : 0; }

/* descriptor pools: v0 count */
static void nvn_PoolInitialize(SwCpu *c) { NvnObj *o = obj(c->x[0], 1); if (o) o->v[0] = (u32)c->x[3]; c->x[0] = 1; }
static void nvn_PoolGetSize(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o ? o->v[0] : 0; }

/* handles: texture id | sampler id << 20 */
static void nvn_DeviceGetTextureHandle(SwCpu *c) { c->x[0] = (c->x[1] & 0xFFFFF) | ((c->x[2] & 0xFFF) << 20); }
static void nvn_DeviceGetImageHandle(SwCpu *c) { c->x[0] = c->x[1] & 0xFFFFF; }

/* syncs / queues: everything completes immediately */
static void nvn_Ret0(SwCpu *c) { c->x[0] = 0; }
static void nvn_Ret1(SwCpu *c) { c->x[0] = 1; }
static void nvn_SyncWait(SwCpu *c) { c->x[0] = 0; }          /* NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED */
static void nvn_QueueBuilderGetQueueMemorySize(SwCpu *c) { (void)c; c->x[0] = 0x100000; }

/* command buffers: plenty of memory, unique non-zero handles */
static void nvn_CommandBufferGetMemoryFree(SwCpu *c) { (void)c; c->x[0] = 0x100000; }
static void nvn_CommandBufferEndRecording(SwCpu *c) { static u64 h = 0x1000; (void)c; c->x[0] = h += 0x10; }

/* windows: v0 texture count, v1.. texture pointers (first 7) */
static void nvn_WindowBuilderSetTextures(SwCpu *c)
{
    NvnObj *o = obj(c->x[0], 1);
    if (!o) return;
    o->v[0] = (u32)c->x[1];
    for (u32 i = 0; i < 7 && i < (u32)c->x[1]; i++) {
        u8 *p = sw_mem_ptr(c, c->x[2] + 8ull * i, 8, 0);
        if (p) memcpy(&o->v[1 + i], p, 8);
    }
}
static void nvn_WindowInitialize(SwCpu *c)
{
    NvnObj *b = obj(c->x[1], 0), *o = obj(c->x[0], 1);
    if (o && b) memcpy(o->v, b->v, sizeof o->v);
    c->x[0] = 1;
}
static void nvn_WindowGetNumTextures(SwCpu *c) { NvnObj *o = obj(c->x[0], 0); c->x[0] = o && o->v[0] ? o->v[0] : 2; }

static const struct { const char *name; NvnImpl impl; } g_impls[] = {
    {"nvnBufferBuilderSetStorage", nvn_BufferBuilderSetStorage},
    {"nvnBufferInitialize", nvn_BufferInitialize},
    {"nvnBufferGetAddress", nvn_BufferGetAddress},
    {"nvnBufferMap", nvn_BufferGetAddress},
    {"nvnBufferGetSize", nvn_BufferGetSize},
    {"nvnBufferBuilderGetSize", nvn_BufferBuilderGetSize},
    {"nvnTextureBuilderSetDefaults", nvn_TextureBuilderSetDefaults},
    {"nvnTextureBuilderSetSize1D", nvn_TextureBuilderSetSize1D},
    {"nvnTextureBuilderSetSize2D", nvn_TextureBuilderSetSize2D},
    {"nvnTextureBuilderSetSize3D", nvn_TextureBuilderSetSize3D},
    {"nvnTextureBuilderSetFormat", nvn_TextureBuilderSetFormat},
    {"nvnTextureBuilderSetLevels", nvn_TextureBuilderSetLevels},
    {"nvnTextureBuilderSetTarget", nvn_TextureBuilderSetTarget},
    {"nvnTextureBuilderSetStorage", nvn_TextureBuilderSetStorage},
    {"nvnTextureBuilderGetStorageSize", nvn_TextureBuilderGetStorageSize},
    {"nvnTextureBuilderGetStorageAlignment", nvn_TextureBuilderGetStorageAlignment},
    {"nvnTextureBuilderGetWidth", nvn_TextureBuilderGetWidth},
    {"nvnTextureBuilderGetHeight", nvn_TextureBuilderGetHeight},
    {"nvnTextureBuilderGetDepth", nvn_TextureBuilderGetDepth},
    {"nvnTextureBuilderGetFormat", nvn_TextureBuilderGetFormat},
    {"nvnTextureBuilderGetLevels", nvn_TextureBuilderGetLevels},
    {"nvnTextureInitialize", nvn_TextureInitialize},
    {"nvnTextureGetWidth", nvn_TextureGetWidth},
    {"nvnTextureGetHeight", nvn_TextureGetHeight},
    {"nvnTextureGetDepth", nvn_TextureGetDepth},
    {"nvnTextureGetFormat", nvn_TextureGetFormat},
    {"nvnTextureGetLevels", nvn_TextureGetLevels},
    {"nvnTextureGetTarget", nvn_TextureGetTarget},
    {"nvnTextureGetStorageSize", nvn_TextureGetStorageSize},
    {"nvnTextureGetTextureAddress", nvn_TextureGetTextureAddress},
    {"nvnTexturePoolInitialize", nvn_PoolInitialize},
    {"nvnTexturePoolGetSize", nvn_PoolGetSize},
    {"nvnSamplerPoolInitialize", nvn_PoolInitialize},
    {"nvnSamplerPoolGetSize", nvn_PoolGetSize},
    {"nvnDeviceGetTextureHandle", nvn_DeviceGetTextureHandle},
    {"nvnDeviceGetImageHandle", nvn_DeviceGetImageHandle},
    {"nvnSyncWait", nvn_SyncWait},
    {"nvnSyncInitialize", nvn_Ret1},
    {"nvnQueueInitialize", nvn_Ret1},
    {"nvnQueueGetError", nvn_Ret0},
    {"nvnQueueBuilderGetQueueMemorySize", nvn_QueueBuilderGetQueueMemorySize},
    {"nvnCommandBufferInitialize", nvn_Ret1},
    {"nvnCommandBufferGetCommandMemoryFree", nvn_CommandBufferGetMemoryFree},
    {"nvnCommandBufferGetControlMemoryFree", nvn_CommandBufferGetMemoryFree},
    {"nvnCommandBufferEndRecording", nvn_CommandBufferEndRecording},
    {"nvnWindowBuilderSetTextures", nvn_WindowBuilderSetTextures},
    {"nvnWindowInitialize", nvn_WindowInitialize},
    {"nvnWindowGetNumTextures", nvn_WindowGetNumTextures},
    {"nvnProgramInitialize", nvn_Ret1},
    {"nvnProgramSetShaders", nvn_Ret1},
    {"nvnSamplerInitialize", nvn_Ret1},
    {"nvnDeviceInitialize", nvn_Ret1},
    {"nvnDeviceGetInteger", nvn_DeviceGetInteger},
    {"nvnMemoryPoolBuilderSetStorage", nvn_MemoryPoolBuilderSetStorage},
    {"nvnMemoryPoolInitialize", nvn_MemoryPoolInitialize},
    {"nvnMemoryPoolMap", nvn_MemoryPoolMap},
    {"nvnMemoryPoolGetBufferAddress", nvn_MemoryPoolGetBufferAddress},
    {"nvnMemoryPoolGetSize", nvn_MemoryPoolGetSize},
    {"nvnWindowAcquireTexture", nvn_AcquireTexture},
    {"nvnQueueAcquireTexture", nvn_AcquireTexture},
    {"nvnCommandBufferClearColor", nvn_CommandBufferClearColor},
    {"nvnQueuePresentTexture", nvn_QueuePresentTexture},
};

void (*sw_gfx_nvn_impl(const char *name))(SwCpu *);   /* Engine/Runtime/gfx/sw_gfx_glue.c */

static NvnImpl find_impl(const char *name)
{
    /* The ported NVN front end (Engine/Runtime/gfx) owns every proc it
     * implements; the rest stay logging stubs.  SW_NVN=old selects the old
     * stage-3 objects below instead (the two must not be mixed). */
    static int old = -1;
    if (old < 0) { const char *e = getenv("SW_NVN"); old = e && !strcmp(e, "old"); }
    if (!old) return sw_gfx_nvn_impl(name);
    for (size_t i = 0; i < sizeof g_impls / sizeof g_impls[0]; i++)
        if (strcmp(g_impls[i].name, name) == 0) return g_impls[i].impl;
    return NULL;
}

/* The NVN function name of a proc address (NULL if it is not one): diagnostics. */
const char *sw_nvn_proc_name(u64 addr)
{
    if (addr < NVN_PROC_BASE || (addr - NVN_PROC_BASE) / 8 >= g_proc_count) return NULL;
    return g_procs[(addr - NVN_PROC_BASE) / 8].name;
}

/* Return the guest address for an NVN function name (allocating on first use). */
static u64 nvn_proc_address(const char *name)
{
    if (!name[0]) return 0;
    if (strcmp(name, "nvnDeviceGetProcAddress") == 0) return NVN_GETPROC_ADDR;
    if (strcmp(name, "nvnBootstrapLoader") == 0) return NVN_BOOTSTRAP_ADDR;
    for (u32 i = 0; i < g_proc_count; i++)
        if (strcmp(g_procs[i].name, name) == 0) return NVN_PROC_BASE + 8ULL * i;
    if (g_proc_count >= NVN_MAX_PROCS) return 0;
    if (!g_report_registered) {
        atexit(nvn_report);
        g_report_registered = 1;
    }
    u32 i = g_proc_count++;
    strncpy(g_procs[i].name, name, NVN_NAME_MAX - 1);
    g_procs[i].impl = find_impl(name);
    u64 addr = NVN_PROC_BASE + 8ULL * i;
    sw_register_native_thunk(addr, nvn_proc_stub);
    return addr;
}

/* nvnBootstrapLoader(const char *name) -> proc */
void svc_nvnBootstrapLoader(SwCpu *c)
{
    char name[NVN_NAME_MAX];
    read_guest_cstr(c, c->x[0], name, sizeof name);
    c->x[0] = nvn_proc_address(name);
    fprintf(stderr, "nvn: nvnBootstrapLoader(\"%s\") -> 0x%llx\n", name, (unsigned long long)c->x[0]);
}

/* nvnDeviceGetProcAddress(const NVNdevice *device, const char *name) -> proc */
void svc_nvnDeviceGetProcAddress(SwCpu *c)
{
    char name[NVN_NAME_MAX];
    read_guest_cstr(c, c->x[1], name, sizeof name);
    c->x[0] = nvn_proc_address(name);
    if (g_proc_count <= 8 || strstr(name, "Dispatch"))  /* full list: exit summary; compute always (lighting debug) */
        fprintf(stderr, "nvn: nvnDeviceGetProcAddress(\"%s\") -> 0x%llx\n", name,
                (unsigned long long)c->x[0]);
}

void sw_nvn_init(void)
{
    sw_register_native_thunk(NVN_BOOTSTRAP_ADDR, svc_nvnBootstrapLoader);
    sw_register_native_thunk(NVN_GETPROC_ADDR, svc_nvnDeviceGetProcAddress);
}
