/* Horizon IPC (HIPC + CMIF) server side, high-level emulated.
 *
 * The SDK talks to system services through
 *     svcConnectToNamedPort("sm:")  -> session handle
 *     svcSendSyncRequest(handle)     message in the thread's TLS buffer
 * This file plays kernel + service: it parses the request in TLS, routes it to
 * an emulated interface object, and writes the reply back in place.
 *
 * Message layout (switchbrew "IPC_Marshalling"):
 *   word0  type:16 | numX:4 | numA:4 | numB:4 | numW:4
 *   word1  numDataWords:10 | recvStaticMode:4 | pad:6 | recvListOff:11 | hasSpecial:1
 *   [special] sendPid:1 | numCopy:4 | numMove:4, [pid u64], copy handles, move handles
 *   X descriptors (2 words), A/B/W descriptors (3 words each)
 *   raw data, 16-byte aligned from the message start:
 *     [domain header 16 bytes] "SFCI" version cmdId token  args...
 *   reply: "SFCO" version result token payload ([domain] out object ids)
 *
 * Interfaces without a real handler answer every command with success and
 * zeroed output, and each (interface, command) is logged once, so a boot log
 * lists exactly which services need real work.  Commands that return a
 * sub-interface are listed in g_obj_cmds; returning nothing there makes the
 * client abort on a null handle.
 */

#include "sw_rt.h"

void sw_report(const char *fmt, ...);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

u64 sw_tick_now(void);

#define IPC_MAX_OBJS     2048
#define IPC_MAX_SESS     1024
#define IPC_SESS_BASE    0x0000A000u
#define IPC_EVENT_BASE   0x0000E000u
#define IPC_SHMEM_BASE   0x0000F000u
#define IPC_NAME_MAX     48

typedef struct {
    char name[IPC_NAME_MAX];   /* interface name, e.g. "fsp-srv" or "IStorage" */
    int used;
    u64 user;                  /* per-object state (e.g. romfs base offset) */
    char path[260];            /* host path: IFileSystem root dir / IFile path */
} IpcObj;

typedef struct {
    int used;
    int obj;                   /* object index for non-domain sessions */
    int domain;
    int closed;                /* Close message received: the handle stays until svcCloseHandle */
} IpcSess;

static IpcObj g_objs[IPC_MAX_OBJS];
static IpcSess g_sess[IPC_MAX_SESS];
static u32 g_next_event;
/* Events that never fire here (error notifications).  Every other event is
 * reported as signalled when waited on; for these a poll must time out, or
 * the SDK believes e.g. the GPU crashed and aborts. */
static unsigned char g_event_never[0x1000];

int sw_ipc_event_never(u32 h)
{
    return h >= IPC_EVENT_BASE && h < IPC_EVENT_BASE + 0x1000u && g_event_never[h - IPC_EVENT_BASE];
}
static int g_ipc_log = -1;

static int ipc_verbose(void)
{
    if (g_ipc_log < 0) g_ipc_log = getenv("SW_IPC_LOG") != NULL;
    return g_ipc_log;
}

static int obj_new(const char *name)
{
    for (int i = 1; i < IPC_MAX_OBJS; i++) {
        if (!g_objs[i].used) {
            memset(&g_objs[i], 0, sizeof g_objs[i]);
            g_objs[i].used = 1;
            strncpy(g_objs[i].name, name, IPC_NAME_MAX - 1);
            return i;
        }
    }
    return 0;
}

/* Round-robin over the slots instead of the lowest free one: a closed session's
 * handle number is not handed out again until all other slots were used, so a
 * late request or second close on a stale handle (Horizon rejects those through
 * the handle's generation bits) no longer hits a new, unrelated session - with
 * SW_MULTICORE=1 SW_CORES=6 that ended in "request on unknown handle" + abort. */
static int g_sess_next = 1;
static int g_stale_close_log;        /* "close of a handle that is not open" lines printed */
static void sess_close(IpcSess *s);
static u32 sess_new(int obj)
{
    for (int k = 0; k < IPC_MAX_SESS - 1; k++) {
        int i = g_sess_next;
        g_sess_next = i + 1 < IPC_MAX_SESS ? i + 1 : 1;
        if (!g_sess[i].used) {
            g_sess[i].used = 1;
            g_sess[i].obj = obj;
            g_sess[i].domain = 0;
            g_sess[i].closed = 0;
            return IPC_SESS_BASE + (u32)i;
        }
    }
    /* full: reuse the oldest slot whose session got a Close message but no
     * svcCloseHandle (a handle the game leaked) */
    for (int k = 0; k < IPC_MAX_SESS - 1; k++) {
        int i = g_sess_next;
        g_sess_next = i + 1 < IPC_MAX_SESS ? i + 1 : 1;
        if (g_sess[i].closed) {
            sess_close(&g_sess[i]);           /* releases its object if nothing else uses it */
            g_sess[i].used = 1;
            g_sess[i].obj = obj;
            g_sess[i].domain = 0;
            g_sess[i].closed = 0;
            return IPC_SESS_BASE + (u32)i;
        }
    }
    return 0;
}

static IpcSess *sess_get(u32 h)
{
    if (h < IPC_SESS_BASE || h >= IPC_SESS_BASE + IPC_MAX_SESS) return NULL;
    IpcSess *s = &g_sess[h - IPC_SESS_BASE];
    return s->used ? s : NULL;
}

/* Close a session; its object goes too once no other session refers to it
 * (GetService creates one object per connection, and some games reconnect
 * in a loop: without this the object table filled up after ~2000). */
static void sess_close(IpcSess *s)
{
    int obj = s->obj;
    s->used = 0;
    if (obj <= 0 || obj >= IPC_MAX_OBJS) return;
    for (int i = 1; i < IPC_MAX_SESS; i++)
        if (g_sess[i].used && g_sess[i].obj == obj) return;
    g_objs[obj].used = 0;
}

/* ------------------------------------------------------------------ */
/* Request context                                                     */
/* ------------------------------------------------------------------ */
typedef struct { u64 addr, size; } IpcBuf;

typedef struct {
    SwCpu *c;
    int obj;                   /* target object */
    const char *name;
    u32 cmd;
    const u8 *in;              /* CMIF arguments (after SFCI header) */
    u32 in_len;
    u64 pid;
    u32 copy_in[8], move_in[8];
    int ncopy_in, nmove_in;
    IpcBuf x[8], a[8], b[8], cbuf[8];
    int nx, na, nb, nc;
    /* reply */
    u32 result;
    u8 out[0x200];
    u32 out_len;
    int out_objs[8];
    int nout_objs;
    u32 out_copy[8];
    int nout_copy;
    u64 c_written[8];          /* bytes a handler wrote into each C (receive) buffer */
} IpcCtx;

static void out_u8(IpcCtx *x, u8 v) { if (x->out_len + 1 <= sizeof x->out) x->out[x->out_len] = v; x->out_len += 1; }
static void out_u32(IpcCtx *x, u32 v) { x->out_len = (x->out_len + 3) & ~3u; if (x->out_len + 4 <= sizeof x->out) memcpy(x->out + x->out_len, &v, 4); x->out_len += 4; }
static void out_u64(IpcCtx *x, u64 v) { x->out_len = (x->out_len + 7) & ~7u; if (x->out_len + 8 <= sizeof x->out) memcpy(x->out + x->out_len, &v, 8); x->out_len += 8; }
static u64 in_u64(IpcCtx *x, u32 off) { u64 v = 0; if (off + 8 <= x->in_len) memcpy(&v, x->in + off, 8); return v; }
static u32 in_u32(IpcCtx *x, u32 off) { u32 v = 0; if (off + 4 <= x->in_len) memcpy(&v, x->in + off, 4); return v; }

static void ret_obj(IpcCtx *x, const char *iface)
{
    if (x->nout_objs < 8) x->out_objs[x->nout_objs++] = obj_new(iface);
}

static void ret_event(IpcCtx *x, int never)
{
    u32 i = g_next_event++ & 0xFFF;
    g_event_never[i] = (unsigned char)(never != 0);
    if (x->nout_copy < 8) x->out_copy[x->nout_copy++] = IPC_EVENT_BASE + i;
}

/* write into the guest buffer of a B (or C) descriptor */
#if defined(_MSC_VER)
#define SW_TLS_IPC __declspec(thread)
#else
#define SW_TLS_IPC _Thread_local
#endif
static u64 buf_write(IpcCtx *x, const IpcBuf *b, const void *data, u64 len)
{
    if (!b->addr) return 0;
    if (len > b->size) len = b->size;
    u64 done = 0;
    while (done < len) {                          /* page-wise: never cross a 64 KB guest page */
        u64 a = b->addr + done;
        u64 n = 0x10000 - (a & 0xFFFF);
        if (n > len - done) n = len - done;
        u8 *p = sw_mem_ptr(x->c, a, n, 1);
        if (!p) return done;
        memcpy(p, (const u8 *)data + done, (size_t)n);
        done += n;
    }
    /* a pointer (C) buffer: the reply must say how much landed (X descriptor) */
    for (int i = 0; i < x->nc && i < 8; i++)
        if (b->addr >= x->cbuf[i].addr && b->addr < x->cbuf[i].addr + x->cbuf[i].size) {
            u64 end = b->addr - x->cbuf[i].addr + done;
            if (end > x->c_written[i]) x->c_written[i] = end;
        }
    return len;
}

/* ------------------------------------------------------------------ */
/* Commands that return interfaces (switchbrew)                        */
/* ------------------------------------------------------------------ */
typedef struct { const char *iface; u32 cmd; const char *ret; } ObjCmd;
static const ObjCmd g_obj_cmds[] = {
    {"appletOE", 0, "IApplicationProxy"},
    {"appletAE", 200, "ILibraryAppletProxy"}, {"appletAE", 300, "IOverlayAppletProxy"},
    {"appletAE", 350, "ISystemAppletProxy"},
    {"IApplicationProxy", 0, "ICommonStateGetter"}, {"IApplicationProxy", 1, "ISelfController"},
    {"IApplicationProxy", 2, "IWindowController"}, {"IApplicationProxy", 3, "IAudioController"},
    {"IApplicationProxy", 4, "IDisplayController"}, {"IApplicationProxy", 10, "IProcessWindingController"},
    {"IApplicationProxy", 11, "ILibraryAppletCreator"}, {"IApplicationProxy", 20, "IApplicationFunctions"},
    {"IApplicationProxy", 1000, "IDebugFunctions"},
    {"ICommonStateGetter", 7, "ILockAccessor"}, {"ICommonStateGetter", 300, "IAppletCommonFunctions"},
    {"IApplicationFunctions", 1, "IStorage_launch"}, {"IApplicationFunctions", 10, "IStorage_am"},
    {"ILibraryAppletCreator", 0, "ILibraryAppletAccessor"}, {"ILibraryAppletCreator", 10, "IStorage_am"},
    {"fsp-srv", 18, "IFileSystem_sd"}, {"fsp-srv", 51, "IFileSystem_save"},
    {"fsp-srv", 52, "IFileSystem_save"}, {"fsp-srv", 53, "IFileSystem_save"},
    {"fsp-srv", 200, "IStorage_romfs"}, {"fsp-srv", 202, "IStorage_romfs"},
    {"fsp-srv", 203, "IStorage_patch"}, {"fsp-srv", 400, "IDeviceOperator"},
    {"fsp-srv", 500, "IEventNotifier"}, {"fsp-srv", 501, "IEventNotifier"},
    {"fsp-srv", 61, "ISaveDataInfoReader"}, {"fsp-srv", 1, "_none"},
    {"hid", 0, "IAppletResource"}, {"hid", 306, "IActiveVibrationDeviceList"},
    {"irs", 0, "_none"},
    {"vi:m", 2, "IApplicationDisplayService"}, {"vi:u", 0, "IApplicationDisplayService"},
    {"vi:s", 1, "IApplicationDisplayService"},
    {"IApplicationDisplayService", 100, "IHOSBinderDriver"},
    {"IApplicationDisplayService", 101, "ISystemDisplayService"},
    {"IApplicationDisplayService", 102, "IManagerDisplayService"},
    {"IApplicationDisplayService", 103, "IHOSBinderDriver"},
    {"time:u", 0, "ISystemClock"}, {"time:u", 1, "ISystemClock"}, {"time:u", 2, "ISteadyClock"},
    {"time:u", 3, "ITimeZoneService"}, {"time:u", 4, "ISystemClock"}, {"time:u", 5, "ISystemClock"},
    {"time:a", 0, "ISystemClock"}, {"time:a", 1, "ISystemClock"}, {"time:a", 2, "ISteadyClock"},
    {"time:a", 3, "ITimeZoneService"}, {"time:a", 4, "ISystemClock"}, {"time:a", 5, "ISystemClock"},
    {"time:s", 0, "ISystemClock"}, {"time:s", 1, "ISystemClock"}, {"time:s", 2, "ISteadyClock"},
    {"time:s", 3, "ITimeZoneService"}, {"time:s", 4, "ISystemClock"}, {"time:s", 5, "ISystemClock"},
    {"apm", 0, "ISession"}, {"apm:p", 0, "ISession"},
    {"acc:u0", 5, "IProfile"}, {"acc:u0", 101, "IManagerForApplication"},
    {"acc:u1", 5, "IProfile"}, {"acc:u1", 101, "IManagerForApplication"},
    {"pctl:a", 0, "IParentalControlService"}, {"pctl:a", 1, "IParentalControlService"},
    {"pctl", 0, "IParentalControlService"}, {"pctl", 1, "IParentalControlService"},
    {"pctl:s", 0, "IParentalControlService"}, {"pctl:s", 1, "IParentalControlService"},
    {"pctl:r", 0, "IParentalControlService"}, {"pctl:r", 1, "IParentalControlService"},
    {"friend:u", 0, "IFriendService"}, {"friend:u", 1, "INotificationService"},
    {"bcat:u", 0, "IBcatService"}, {"bcat:u", 1, "IDeliveryCacheStorageService"},
    {"IDeliveryCacheStorageService", 0, "IDeliveryCacheFileService"},
    {"IDeliveryCacheStorageService", 1, "IDeliveryCacheDirectoryService"},
    {"audren:u", 0, "IAudioRenderer"}, {"audren:u", 2, "IAudioDevice"}, {"audren:u", 4, "IAudioDevice"},
    {"audout:u", 1, "IAudioOut"}, {"audout:u", 3, "IAudioOut"},
    {"audin:u", 1, "IAudioIn"}, {"audin:u", 3, "IAudioIn"},
    {"nifm:u", 4, "IGeneralService"}, {"nifm:u", 5, "IGeneralService"},
    {"IGeneralService", 4, "IRequest"}, {"IGeneralService", 21, "IScanRequest"},
    {"lm", 0, "ILogger"}, {"prepo:u", 10101, "_none"},
    {"aoc:u", 8, "_none"}, {"ns:am2", 7996, "IApplicationManagerInterface"},
    {"ns:ec", 0, "IShopServiceAccessServerInterface"},
    {"ssl", 0, "ISslContext"}, {"ISslContext", 4, "ISslConnection"},
    {"nfp:user", 0, "IUser"}, {"nfc:user", 0, "IUser"}, {"mm:u", 0, "_none"},
    {"caps:su", 0, "_none"}, {"ectx:aw", 0, "IContextRegistrar"},
    {"ovln:snd", 0, "ISender"}, {"ldn:u", 0, "IUserLocalCommunicationService"},
    {"nim:eca", 0, "IShopServiceAccessServerInterface"},
    {"IFriendService", 0, "_none"},
    {NULL, 0, NULL},
};

/* Commands that return an event handle */
typedef struct { const char *iface; u32 cmd; int never; } EvCmd;
static const EvCmd g_event_cmds[] = {
    {"ICommonStateGetter", 0}, {"ICommonStateGetter", 61}, {"ICommonStateGetter", 62},
    {"ISelfController", 19}, {"ISelfController", 44}, {"ISelfController", 45},
    {"IApplicationFunctions", 130, 1} /* GetGpuErrorDetectedSystemEvent */, {"IApplicationFunctions", 131}, {"IApplicationFunctions", 210},
    {"IAudioController", 0}, {"IAudioRenderer", 7}, {"IAudioDevice", 2}, {"IAudioDevice", 4},
    {"IAudioDevice", 11}, {"IAudioDevice", 12, 1} /* QueryAudioDeviceOutputEvent: the device never changes */,
    {"IAudioOut", 4}, {"IAudioIn", 4},
    {"IApplicationDisplayService", 5202}, {"IApplicationDisplayService", 1016},
    {"IHOSBinderDriver", 2}, {"IRequest", 2}, {"IRequest", 3},
    {"IAppletResource", 0}, {"IBcatService", 0},
    {"hid", 70}, {"hid", 71}, {"hid", 306}, {"hid", 1001}, {"hid", 1002},
    {"INotificationService", 1}, {"IUser", 17}, {"IUser", 18}, {"IUser", 19}, {"IUser", 24},
    {"ILibraryAppletAccessor", 0}, {"ILibraryAppletAccessor", 106, 1} /* interactive out data: applets here produce none */,
    {"ITimeZoneService", 0x7fffffff}, {"IEventNotifier", 0}, {"ILockAccessor", 3},
    {"time:u", 20}, {"time:a", 20}, {"time:s", 20},
    {"IDeviceOperator", 0x7fffffff},
    {NULL, 0},
};

/* ------------------------------------------------------------------ */
/* Specific handlers                                                   */
/* ------------------------------------------------------------------ */
static int name_is(IpcCtx *x, const char *n) { return strcmp(x->name, n) == 0; }

/* The one local account: its nn::account::Uid (two u64, little endian). */
static const u8 ACC_UID[16] = {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
                               0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe};
static int g_launch_user_popped;

/* romfs: IStorage reads a raw RomFS image; sw_romfs.c synthesises one over
 * the extracted roms/romfs folder. */
u64 sw_romfs_size(void);
u64 sw_romfs_read(u64 off, void *buf, u64 size);


/* ------------------------------------------------------------------ */
/* Host-backed file systems: save data and SD card                     */
/* ------------------------------------------------------------------ */
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define fs_mkdir(p) _mkdir(p)
#else
#define fs_mkdir(p) mkdir(p, 0777)
#endif

#define FS_PATH_NOT_FOUND   0x202u      /* fs: PathNotFound */
#define FS_PATH_EXISTS      0x402u      /* fs: PathAlreadyExists */

static void mkdirs(const char *path)
{
    char tmp[260];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/' || *q == '\\') { char ch = *q; *q = 0; fs_mkdir(tmp); *q = ch; }
    fs_mkdir(tmp);
}

/* guest path from the first X descriptor (fs paths are 0x301-byte strings) */
static int guest_path(IpcCtx *x, int which, char *out, size_t cap)
{
    out[0] = 0;
    if (x->nx <= which || !x->x[which].addr) return 0;
    size_t i = 0;
    for (; i + 1 < cap && i < x->x[which].size; i++) {
        u8 *p = sw_mem_ptr(x->c, x->x[which].addr + i, 1, 0);
        if (!p || !*p) break;
        out[i] = (char)*p;
    }
    out[i] = 0;
    return 1;
}

/* host path = root + guest path ("save:/a/b" or "/a/b"), no ".." escapes */
static int host_path(IpcCtx *x, int which, char *out, size_t cap)
{
    char g[0x302];
    if (!guest_path(x, which, g, sizeof g)) return 0;
    const char *rel = strchr(g, ':') ? strchr(g, ':') + 1 : g;
    while (*rel == '/') rel++;
    if (strstr(rel, "..")) return 0;
    snprintf(out, cap, "%s/%s", g_objs[x->obj].path, rel);
    size_t n = strlen(out);
    while (n > 1 && (out[n - 1] == '/')) out[--n] = 0;
    return 1;
}

static int handle_fs(IpcCtx *x)
{
    int is_fs = !strncmp(x->name, "IFileSystem_", 12);
    if (is_fs && !g_objs[x->obj].path[0]) {
        /* SW_SAVE_DIR=<folder>: the save data lives there instead of out/save
         * (test runs and benchmarks use a copy and never touch the player's save) */
        const char *sd = getenv("SW_SAVE_DIR");
        snprintf(g_objs[x->obj].path, sizeof g_objs[x->obj].path, "%s",
                 !strcmp(x->name, "IFileSystem_sd") ? "sdmc" : (sd && *sd ? sd : "save"));
        mkdirs(g_objs[x->obj].path);
    }
    if (is_fs) {
        char hp[260], hp2[260];
        struct stat st;
        switch (x->cmd) {
        case 0: {                                             /* CreateFile(u32 option, u64 size) */
            if (!host_path(x, 0, hp, sizeof hp)) { x->result = FS_PATH_NOT_FOUND; return 1; }
            if (!stat(hp, &st)) { x->result = FS_PATH_EXISTS; return 1; }
            FILE *f = fopen(hp, "wb");
            if (!f) { x->result = FS_PATH_NOT_FOUND; return 1; }
            u64 size = in_u64(x, 8);
            if (size) { _fseeki64(f, (long long)size - 1, SEEK_SET); fputc(0, f); }
            fclose(f);
            return 1;
        }
        case 1:                                               /* DeleteFile */
            if (!host_path(x, 0, hp, sizeof hp) || remove(hp)) x->result = FS_PATH_NOT_FOUND;
            return 1;
        case 2:                                               /* CreateDirectory */
            if (!host_path(x, 0, hp, sizeof hp)) { x->result = FS_PATH_NOT_FOUND; return 1; }
            if (!stat(hp, &st)) { x->result = FS_PATH_EXISTS; return 1; }
            mkdirs(hp);
            return 1;
        case 3: case 4: case 13:                              /* DeleteDirectory(+Recursively), Clean */
            return 1;
        case 5: case 6:                                       /* RenameFile / RenameDirectory */
            if (host_path(x, 0, hp, sizeof hp) && host_path(x, 1, hp2, sizeof hp2) && rename(hp, hp2))
                x->result = FS_PATH_NOT_FOUND;
            return 1;
        case 7:                                               /* GetEntryType -> u32 (0 dir, 1 file) */
            if (!host_path(x, 0, hp, sizeof hp) || stat(hp, &st)) { x->result = FS_PATH_NOT_FOUND; return 1; }
            out_u32(x, (st.st_mode & S_IFDIR) ? 0 : 1);
            return 1;
        case 8: {                                             /* OpenFile(u32 mode) -> IFile */
            if (!host_path(x, 0, hp, sizeof hp) || stat(hp, &st) || (st.st_mode & S_IFDIR)) {
                x->result = FS_PATH_NOT_FOUND; return 1;
            }
            int o = obj_new("IFile");
            snprintf(g_objs[o].path, sizeof g_objs[o].path, "%s", hp);
            if (x->nout_objs < 8) x->out_objs[x->nout_objs++] = o;
            return 1;
        }
        case 9: {                                             /* OpenDirectory(u32 filter) -> IDirectory */
            if (!host_path(x, 0, hp, sizeof hp) || stat(hp, &st) || !(st.st_mode & S_IFDIR)) {
                x->result = FS_PATH_NOT_FOUND; return 1;
            }
            int o = obj_new("IDirectory");
            snprintf(g_objs[o].path, sizeof g_objs[o].path, "%s", hp);
            if (x->nout_objs < 8) x->out_objs[x->nout_objs++] = o;
            return 1;
        }
        case 10: return 1;                                    /* Commit */
        case 11: out_u64(x, 32ull << 30); return 1;           /* GetFreeSpaceSize */
        case 12: out_u64(x, 32ull << 30); return 1;           /* GetTotalSpaceSize */
        }
        return 0;
    }
    if (!strcmp(x->name, "IFile")) {
        const char *hp = g_objs[x->obj].path;
        switch (x->cmd) {
        case 0: {                                             /* Read(u32 opt, u64 off, u64 size) -> B */
            u64 off = in_u64(x, 8), size = in_u64(x, 16), got = 0;
            FILE *f = fopen(hp, "rb");
            if (f && x->nb) {
                static u8 tmp[1 << 20];
                if (size > x->b[0].size) size = x->b[0].size;
                _fseeki64(f, (long long)off, SEEK_SET);
                while (got < size) {
                    size_t want = (size_t)(size - got < sizeof tmp ? size - got : sizeof tmp);
                    size_t n = fread(tmp, 1, want, f);
                    if (!n) break;
                    IpcBuf part = {x->b[0].addr + got, n};
                    buf_write(x, &part, tmp, n);
                    got += n;
                }
            }
            if (f) fclose(f);
            out_u64(x, got);
            return 1;
        }
        case 1: {                                             /* Write(u32 opt, u64 off, u64 size) <- A */
            u64 off = in_u64(x, 8), size = in_u64(x, 16);
            FILE *f = fopen(hp, "r+b");
            if (f && x->na) {
                if (size > x->a[0].size) size = x->a[0].size;
                _fseeki64(f, (long long)off, SEEK_SET);
                for (u64 i = 0; i < size; i++) {
                    u8 *p = sw_mem_ptr(x->c, x->a[0].addr + i, 1, 0);
                    fputc(p ? *p : 0, f);
                }
            }
            if (f) fclose(f);
            return 1;
        }
        case 2: return 1;                                     /* Flush */
        case 3: {                                             /* SetSize(u64) */
            u64 want = in_u64(x, 0);
            FILE *f = fopen(hp, "r+b");
            if (f) {
                _fseeki64(f, 0, SEEK_END);
                if ((u64)_ftelli64(f) < want && want) { _fseeki64(f, (long long)want - 1, SEEK_SET); fputc(0, f); }
                fclose(f);
            }
            return 1;
        }
        case 4: {                                             /* GetSize -> u64 */
            struct stat st;
            out_u64(x, stat(hp, &st) ? 0 : (u64)st.st_size);
            return 1;
        }
        }
        return 0;
    }
    if (!strcmp(x->name, "IDirectory")) {
        if (x->cmd == 0) { out_u64(x, 0); return 1; }         /* Read: no entries (yet) */
        if (x->cmd == 1) { out_u64(x, 0); return 1; }         /* GetEntryCount */
        return 0;
    }
    if (!strcmp(x->name, "fsp-srv")) {
        if (x->cmd == 22 || x->cmd == 23) return 1;           /* CreateSaveDataFileSystem(BySystem) */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Display (vi): displays, layers, native window parcel                */
/* ------------------------------------------------------------------ */
/* Parcel {header, NativeWindow{magic 2, pid 1, binder id, pad, "dispdrv"}}
 * as returned by OpenLayer / CreateStrayLayer (nn::vi parses it into an
 * ANativeWindow; the NVN layer is emulated, so the binder is never used). */
static u64 write_native_window_parcel(IpcCtx *x, u32 binder_id)
{
    u8 parcel[0x10 + 0x28 + 4];
    memset(parcel, 0, sizeof parcel);
    u32 data_size = 0x28, data_off = 0x10, obj_size = 4, obj_off = 0x10 + 0x28;
    memcpy(parcel + 0, &data_size, 4);
    memcpy(parcel + 4, &data_off, 4);
    memcpy(parcel + 8, &obj_size, 4);
    memcpy(parcel + 12, &obj_off, 4);
    u32 magic = 2, pid = 1;
    memcpy(parcel + 0x10, &magic, 4);
    memcpy(parcel + 0x14, &pid, 4);
    memcpy(parcel + 0x18, &binder_id, 4);
    memcpy(parcel + 0x28, "dispdrv", 8);
    if (x->nb) buf_write(x, &x->b[0], parcel, sizeof parcel);
    return sizeof parcel;
}

/* ------------------------------------------------------------------ */
/* pl:u - shared system fonts                                          */
/* ------------------------------------------------------------------ */
/* The console keeps its system fonts in a 17 MB shared memory block.  Each
 * font is an 8-byte header (magic and big-endian size, XOR'd with a fixed
 * key) followed by the TrueType data; pl:u reports offsets and sizes per
 * font type.  The game's own text uses fonts from its RomFS, so one host
 * font (SW_FONT or a Windows font) stands in for every type here. */
#define PL_SHMEM_SIZE  0x1100000u
#define PL_FONT_TYPES  6
#define PL_SHMEM_FONT  (IPC_SHMEM_BASE + 1u)
static u8 *g_pl_shmem;
static u32 g_pl_off, g_pl_size;               /* data offset (after header), size */

static void pl_build(void)
{
    if (g_pl_shmem) return;
    g_pl_shmem = (u8 *)calloc(1, PL_SHMEM_SIZE);
    if (!g_pl_shmem) return;
    const char *cands[] = { getenv("SW_FONT"), "C:\\Windows\\Fonts\\segoeui.ttf",
                            "C:\\Windows\\Fonts\\arial.ttf", NULL };
    for (int i = 0; i < 3; i++) {
        if (!cands[i]) continue;
        FILE *f = fopen(cands[i], "rb");
        if (!f) continue;
        size_t n = fread(g_pl_shmem + 8, 1, PL_SHMEM_SIZE - 8, f);
        fclose(f);
        if (n > 0) {
            g_pl_size = (u32)n;
            sw_report("[pl] shared font: %s (%u bytes)\n", cands[i], g_pl_size);
            break;
        }
    }
    const u32 key = 0x49621806u, magic = 0x18029a7fu;
    u32 m = magic ^ key, sz = g_pl_size ^ key;
    u32 sz_be = (sz >> 24) | ((sz >> 8) & 0xFF00u) | ((sz << 8) & 0xFF0000u) | (sz << 24);
    memcpy(g_pl_shmem, &m, 4);
    memcpy(g_pl_shmem + 4, &sz_be, 4);
    g_pl_off = 8;
}

/* Called by MapSharedMemory: fills the mapped range for handles we own. */
void sw_ipc_shmem_map(SwCpu *c, u32 handle, u64 addr, u64 size)
{
    if (handle != PL_SHMEM_FONT) return;
    pl_build();
    if (!g_pl_shmem) return;
    if (size > PL_SHMEM_SIZE) size = PL_SHMEM_SIZE;
    for (u64 done = 0; done < size; ) {
        u64 a = addr + done, n = 0x10000 - (a & 0xFFFF);
        if (n > size - done) n = size - done;
        u8 *p = sw_mem_ptr(c, a, n, 1);
        if (p) memcpy(p, g_pl_shmem + done, (size_t)n);
        done += n;
    }
}

static int handle_pl(IpcCtx *x)
{
    if (!name_is(x, "pl:u") && !name_is(x, "pl:s")) return 0;
    pl_build();
    switch (x->cmd) {
    case 0: return 1;                                         /* RequestLoad */
    case 1: out_u32(x, 1); return 1;                          /* GetLoadState: Loaded */
    case 2: out_u32(x, g_pl_size); return 1;                  /* GetSize */
    case 3: out_u32(x, g_pl_off); return 1;                   /* GetSharedMemoryAddressOffset */
    case 4:                                                   /* GetSharedMemoryNativeHandle */
        if (x->nout_copy < 8) x->out_copy[x->nout_copy++] = PL_SHMEM_FONT;
        return 1;
    case 5: {                                                 /* GetSharedFontInOrderOfPriority */
        u32 types[PL_FONT_TYPES], offs[PL_FONT_TYPES], sizes[PL_FONT_TYPES];
        for (u32 i = 0; i < PL_FONT_TYPES; i++) { types[i] = i; offs[i] = g_pl_off; sizes[i] = g_pl_size; }
        const u32 *arr[3] = { types, offs, sizes };
        for (int i = 0; i < 3; i++) {
            IpcBuf *b = i < x->nc ? &x->cbuf[i] : (i < x->nb ? &x->b[i] : NULL);
            if (b) buf_write(x, b, arr[i], sizeof types);
        }
        out_u32(x, 1);                                        /* loaded (bool, padded) */
        out_u32(x, PL_FONT_TYPES);                            /* count */
        return 1;
    }
    }
    return 0;
}

/* What the game is told about its display.  SW_OPERATION_MODE=docked: docked + boost performance mode (default:
 * handheld, as before); SW_DISPLAY_RES=<w>x<h>: the display / default display resolution (default 1280x720). */
static int g_docked = -1;
static u32 g_disp_w, g_disp_h;
static void display_init(void)
{
    if (g_docked >= 0) return;
    const char *m = getenv("SW_OPERATION_MODE");
    g_docked = m && !strcmp(m, "docked");
    g_disp_w = 1280, g_disp_h = 720;
    const char *r = getenv("SW_DISPLAY_RES");
    unsigned w, h;
    if (r && sscanf(r, "%ux%u", &w, &h) == 2 && w >= 640 && h >= 360 && w <= 7680 && h <= 4320) g_disp_w = w, g_disp_h = h;
    if (g_docked || g_disp_w != 1280)
        sw_report("[display] %s, display %ux%u\n", g_docked ? "docked" : "handheld", g_disp_w, g_disp_h);
}

static int handle_vi(IpcCtx *x)
{
    display_init();
    static u64 next_layer = 1;
    if (!strcmp(x->name, "IApplicationDisplayService")) {
        switch (x->cmd) {
        case 1010: out_u64(x, 0); return 1;                       /* OpenDisplay -> display id */
        case 1011: out_u64(x, 0); return 1;                       /* OpenDefaultDisplay */
        case 1020: return 1;                                      /* CloseDisplay */
        case 1101: return 1;                                      /* SetDisplayEnabled */
        case 1102: out_u64(x, g_disp_w); out_u64(x, g_disp_h); return 1;   /* GetDisplayResolution */
        case 2020: out_u64(x, write_native_window_parcel(x, 1)); return 1;     /* OpenLayer */
        case 2021: return 1;                                      /* CloseLayer */
        case 2030: out_u64(x, next_layer++);                      /* CreateStrayLayer */
                   out_u64(x, write_native_window_parcel(x, 1)); return 1;
        case 2031: return 1;                                      /* DestroyStrayLayer */
        case 2101: return 1;                                      /* SetLayerScalingMode */
        case 2102: out_u64(x, 0); return 1;                       /* ConvertScalingMode */
        case 2450: out_u64(x, (u64)g_disp_w * g_disp_h * 4); out_u64(x, 0x1000); return 1;   /* GetIndirectLayerImageMap */
        case 2460: out_u64(x, (u64)g_disp_w * g_disp_h * 4); out_u64(x, 0x1000); return 1;   /* ...RequiredMemoryInfo */
        case 1000: out_u64(x, 1);                                 /* ListDisplays: one "Default" */
            if (x->nb) {
                u8 info[0x60]; memset(info, 0, sizeof info);
                memcpy(info, "Default", 8);
                info[0x40] = 1;                                   /* has_limited_layers */
                u64 maxl = 1, w = 1920, h = 1080;
                memcpy(info + 0x48, &maxl, 8); memcpy(info + 0x50, &w, 8); memcpy(info + 0x58, &h, 8);
                buf_write(x, &x->b[0], info, sizeof info);
            }
            return 1;
        }
        return 0;
    }
    if (!strcmp(x->name, "IManagerDisplayService")) {
        if (x->cmd == 2010) { out_u64(x, next_layer++); return 1; }  /* CreateManagedLayer */
        if (x->cmd == 2011 || x->cmd == 6000 || x->cmd == 6001 || x->cmd == 6002) return 1;
        return 0;
    }
    if (!strcmp(x->name, "ISystemDisplayService")) {
        if (x->cmd == 3200) {                                      /* GetDisplayMode */
            u32 w = g_disp_w, h = g_disp_h; float rate = 60.0f; u32 unk = 0;
            out_u32(x, w); out_u32(x, h); x->out_len = (x->out_len + 3) & ~3u;
            memcpy(x->out + x->out_len, &rate, 4); x->out_len += 4; out_u32(x, unk);
            return 1;
        }
        return 0;
    }
    if (!strcmp(x->name, "ISelfController")) {
        if (x->cmd == 40) { out_u64(x, next_layer++); return 1; }  /* CreateManagedDisplayLayer */
        if (x->cmd == 44) { out_u64(x, next_layer++); out_u64(x, next_layer++); return 1; } /* CreateManagedDisplaySeparableLayer */
        return 0;
    }
    if (!strcmp(x->name, "IHOSBinderDriver")) {
        if (x->cmd == 0 || x->cmd == 3) {                          /* TransactParcel(Auto): empty reply parcel */
            u8 reply[0x14]; memset(reply, 0, sizeof reply);
            u32 hdr[4] = {4, 0x10, 0, 0x14};
            memcpy(reply, hdr, sizeof hdr);
            IpcBuf *b = x->nb ? &x->b[0] : (x->nc ? &x->cbuf[0] : NULL);
            if (b) buf_write(x, b, reply, sizeof reply);
            return 1;
        }
        if (x->cmd == 1) return 1;                                 /* AdjustRefcount */
        return 0;
    }
    return 0;
}

/* ---- am storages (applet parameters) and library applets -----------------
 * IStorage_am: a host buffer created by CreateStorage(size); Open gives an
 * IStorageAccessor_am over it (GetSize / Write / Read).  Library applets
 * (error dialog, player select, ...) are not run: Start completes at once
 * and PopOutData hands back a storage that reports success. */
static struct { u8 *data; u64 size; int borrowed; } g_am_store[IPC_MAX_OBJS];

/* Transfer memory (svcCreateTransferMemory): a guest range handed to a
 * service by handle.  Guest addresses are host addresses, so a storage over
 * it simply points at the guest buffer. */
#define IPC_TMEM_BASE 0x0000D000u
#define IPC_TMEM_MAX  256
static struct { u64 addr, size; int used; } g_tmem[IPC_TMEM_MAX];

static u32 tmem_new(u64 addr, u64 size)
{
    for (int i = 0; i < IPC_TMEM_MAX; i++)
        if (!g_tmem[i].used) { g_tmem[i].used = 1; g_tmem[i].addr = addr; g_tmem[i].size = size; return IPC_TMEM_BASE + (u32)i; }
    return 0;
}
static int g_am_link[IPC_MAX_OBJS];          /* accessor object -> storage object */
static u32 g_applet_id[IPC_MAX_OBJS];        /* library applet accessor -> applet id */
static int g_kbd_cfg_store;                  /* last written storage big enough for a KeyboardConfig */
int sw_text_entry(const wchar_t *prompt, const wchar_t *initial, int max_len, wchar_t *out);

static int ret_obj_id(IpcCtx *x, const char *iface)
{
    int id = obj_new(iface);
    if (x->nout_objs < 8) x->out_objs[x->nout_objs++] = id;
    return id;
}

static int am_store_new(IpcCtx *x, u64 size)
{
    int id = ret_obj_id(x, "IStorage_am");
    if (id > 0) {
        if (!g_am_store[id].borrowed) free(g_am_store[id].data);
        g_am_store[id].borrowed = 0;
        if (size > 0x4000000) size = 0x4000000;
        g_am_store[id].data = (u8 *)calloc(1, size ? (size_t)size : 1);
        g_am_store[id].size = size;
    }
    return id;
}

static int handle_am_storage(IpcCtx *x)
{
    if ((name_is(x, "IApplicationFunctions") || name_is(x, "ILibraryAppletCreator")) && x->cmd == 10) {
        am_store_new(x, in_u64(x, 0));                         /* CreateStorage(size) */
        return 1;
    }
    if (name_is(x, "ILibraryAppletCreator") && x->cmd == 11) { /* CreateTransferMemoryStorage(h, writable, size) */
        u32 h = x->ncopy_in ? x->copy_in[0] : 0;
        u64 size = in_u64(x, 8);
        int id = ret_obj_id(x, "IStorage_am");
        if (id > 0 && h >= IPC_TMEM_BASE && h < IPC_TMEM_BASE + IPC_TMEM_MAX && g_tmem[h - IPC_TMEM_BASE].used) {
            if (!g_am_store[id].borrowed) free(g_am_store[id].data);
            g_am_store[id].data = (u8 *)(uintptr_t)g_tmem[h - IPC_TMEM_BASE].addr;
            g_am_store[id].size = size ? size : g_tmem[h - IPC_TMEM_BASE].size;
            g_am_store[id].borrowed = 1;
        }
        return 1;
    }
    if (name_is(x, "IStorage_am") && x->cmd == 0) {            /* Open */
        int acc = ret_obj_id(x, "IStorageAccessor_am");
        if (acc > 0) g_am_link[acc] = x->obj;
        return 1;
    }
    if (name_is(x, "IStorageAccessor_am")) {
        int st = g_am_link[x->obj];
        u8 *data = st > 0 ? g_am_store[st].data : NULL;
        u64 size = st > 0 ? g_am_store[st].size : 0;
        if (x->cmd == 0) { out_u64(x, size); return 1; }       /* GetSize */
        if (x->cmd == 10 || x->cmd == 11) {                    /* Write(offset, A) / Read(offset, B) */
            u64 off = in_u64(x, 0);
            if (!data || off > size) { x->result = 0x3E880; return 1; }   /* am: out of range */
            if (x->cmd == 10) {
                IpcBuf *a = x->na ? &x->a[0] : (x->nx ? &x->x[0] : NULL);
                for (u64 i = 0; a && i < a->size && off + i < size; i++) {
                    u8 *p = sw_mem_ptr(x->c, a->addr + i, 1, 0);
                    if (p) data[off + i] = *p;
                }
                if (size >= 0x3c0) g_kbd_cfg_store = st;
                /* applet parameters often carry UTF-16 texts (keyboard header/guide):
                 * log the readable ASCII runs */
                if (a && a->size >= 8) {
                    char line[256]; int k = 0;
                    for (u64 i = 0; i + 1 < a->size && off + i + 1 < size && k < 250; i += 2) {
                        u8 lo = data[off + i], hi = data[off + i + 1];
                        if (!hi && lo >= 0x20 && lo < 0x7f) line[k++] = (char)lo;
                        else if (k && line[k - 1] != '|') line[k++] = '|';
                    }
                    line[k] = 0;
                    if (k > 4) sw_report("[am] storage text: %s\n", line);
                }
            } else {
                IpcBuf *b = x->nb ? &x->b[0] : (x->nc ? &x->cbuf[0] : NULL);
                if (b) buf_write(x, b, data + off, size - off);
            }
            return 1;
        }
    }
    if (name_is(x, "ILibraryAppletCreator") && x->cmd == 0) {   /* CreateLibraryApplet(id, mode) */
        u32 id = in_u32(x, 0), mode = in_u32(x, 4);
        int acc = ret_obj_id(x, "ILibraryAppletAccessor");
        if (acc > 0) g_applet_id[acc] = id;
        sw_report("[am] library applet 0x%x requested (mode %u): not run, completes at once\n", id, mode);
        return 1;
    }
    if (name_is(x, "ILibraryAppletAccessor")) {
        switch (x->cmd) {
        case 10: case 20: case 25: return 1;                    /* Start / RequestExit / Terminate */
        case 1: out_u8(x, 1); return 1;                         /* IsCompleted */
        case 30: return 1;                                      /* GetResult: success */
        case 100: case 103: case 105: return 1;                 /* PushInData / PushInteractiveInData / PushExtraStorage */
        case 104: x->result = 0x480; return 1;                  /* PopInteractiveOutData: none (am: not available) */
        case 101: {                                             /* PopOutData */
            if (g_applet_id[x->obj] == 0x11) {                  /* software keyboard: close result + text */
                /* KeyboardConfig (libnx SwkbdConfigCommon): header text +0x24,
                 * guide +0x1a8 (UTF-16), max length +0x3ac, UTF-8 output +0x3bd */
                wchar_t prompt[0x82] = {0}, text[501] = {0};
                int max_len = 16, utf8 = 0;
                const u8 *cfg = g_kbd_cfg_store > 0 ? g_am_store[g_kbd_cfg_store].data : NULL;
                if (cfg && g_am_store[g_kbd_cfg_store].size >= 0x3c0) {
                    const u8 *src = cfg + 0x24;
                    if (!src[0] && !src[1]) src = cfg + 0x1a8;
                    for (int i = 0; i < 0x80 && (src[2 * i] | src[2 * i + 1]); i++)
                        prompt[i] = (wchar_t)(src[2 * i] | src[2 * i + 1] << 8);
                    u32 ml; memcpy(&ml, cfg + 0x3ac, 4);
                    if (ml > 0 && ml <= 500) max_len = (int)ml;
                    utf8 = cfg[0x3bd] != 0;
                }
                int ok = sw_text_entry(prompt, L"", max_len, text);
                if (ok && !text[0]) wcscpy(text, L"Player");
                int st = am_store_new(x, 0x7D8);
                if (st > 0) {
                    u8 *d = g_am_store[st].data;
                    u32 res = ok ? 0 : 1;                           /* 0 OK, 1 cancelled */
                    memcpy(d, &res, 4);
                    if (utf8) WideCharToMultiByte(CP_UTF8, 0, text, -1, (char *)d + 4, 0x7D4, NULL, NULL);
                    else memcpy(d + 4, text, (wcslen(text) + 1) * 2);
                }
                char shown[512];
                WideCharToMultiByte(CP_UTF8, 0, text, -1, shown, sizeof shown, NULL, NULL);
                sw_report("[am] software keyboard: %s \"%s\" (max %d, %s)\n", ok ? "entered" : "cancelled",
                          shown, max_len, utf8 ? "UTF-8" : "UTF-16");
            } else {
                am_store_new(x, 0x10);                          /* result 0 */
            }
            return 1;
        }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Audio: device queries and one audout session played in real time    */
/* ------------------------------------------------------------------ */
/* No sound reaches the PC yet, but the game's sound engine needs a working
 * output: it opened audout, got sample rate / channels / format 0 from the
 * zeroed stub and never started it, and every IAudioDevice query (active
 * device name, channel count) came back empty.  Here audout answers 48 kHz,
 * the requested channel count (2 if 0), PCM int16; appended buffers are
 * "played" back to back in real time and released when their time is up,
 * and the buffer event counts as signalled only then (sw_ipc_event_delay_ns,
 * used by WaitSynchronization), so the audio thread waits like on hardware.
 * On by default; SW_AUDIO_OUT=0 = the old silent stub. */
#define AO_RATE 48000u
/* a handler can ask for the calling guest thread to sleep after the reply (sw_runtime.c svc 0x21) */
static __declspec(thread) s64 g_ipc_sleep_ns;
s64 sw_ipc_take_sleep(void) { s64 s = g_ipc_sleep_ns; g_ipc_sleep_ns = 0; return s; }
#define AO_MAXQ 64
static struct {
    int open, started;
    u32 channels;
    u64 tag[AO_MAXQ], end_ns[AO_MAXQ];   /* queued buffers, end time of each (0 = not scheduled yet) */
    u64 samples[AO_MAXQ];
    int head, n;
    u64 played;                          /* samples of released buffers */
    u64 clock_ns;                        /* when the last queued buffer ends */
    int event;                           /* IPC event index of RegisterBufferEvent, -1 none */
} g_ao = {.event = -1};
#if defined(_WIN32)
static SRWLOCK g_ao_lock = SRWLOCK_INIT;
#define AO_LOCK() AcquireSRWLockExclusive(&g_ao_lock)
#define AO_UNLOCK() ReleaseSRWLockExclusive(&g_ao_lock)
#else
#define AO_LOCK() (void)0
#define AO_UNLOCK() (void)0
#endif

/* PC sound output (waveOut): each appended guest buffer is copied and queued on the device in the same
 * slot, and with a device a buffer counts as released only when the device has played it (WHDR_DONE), so
 * the game's mixer runs on the sound card's clock.  No device: the old timer release.  5.1 is mixed to
 * stereo.  SW_AUDIO_VOLUME=0..100 (default 100; 0 = no device, silent). */
#if defined(_WIN32)
#include <mmsystem.h>
static HWAVEOUT g_wo;
static int g_wo_state;                   /* 0 not tried, 1 open, -1 failed / off */
static WAVEHDR g_wh[AO_MAXQ];
static s16 *g_wbuf[AO_MAXQ];
static u32 g_wcap[AO_MAXQ];              /* bytes allocated per slot */
static int g_wo_vol = -1;                /* percent */
static void wo_close(void)
{
    if (g_wo_state == 1) {
        waveOutReset(g_wo);
        for (int i = 0; i < AO_MAXQ; i++)
            if (g_wh[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wo, &g_wh[i], sizeof g_wh[i]);
        waveOutClose(g_wo);
    }
    memset(g_wh, 0, sizeof g_wh);
    g_wo_state = 0;
}
static int wo_open(void)
{
    if (g_wo_vol < 0) { const char *e = getenv("SW_AUDIO_VOLUME"); g_wo_vol = e && *e ? atoi(e) : 100;
                        if (g_wo_vol > 100) g_wo_vol = 100; }
    if (g_wo_state) return g_wo_state == 1;
    g_wo_state = -1;
    if (g_wo_vol <= 0) return 0;
    WAVEFORMATEX f = {0};
    f.wFormatTag = WAVE_FORMAT_PCM, f.nChannels = 2, f.nSamplesPerSec = AO_RATE, f.wBitsPerSample = 16;
    f.nBlockAlign = 4, f.nAvgBytesPerSec = AO_RATE * 4;
    MMRESULT r = waveOutOpen(&g_wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) { sw_report("[audio] no sound device (waveOutOpen %u): timer only\n", (unsigned)r); return 0; }
    waveOutPause(g_wo);                  /* until StartAudioOut */
    g_wo_state = 1;
    sw_report("[audio] sound out: 48 kHz stereo, volume %d%%\n", g_wo_vol);
    return 1;
}
/* live volume for the in-game menu (sw_menu.h); 0 keeps the device and plays silence */
int sw_audio_volume(void)
{
    if (g_wo_vol < 0) { const char *e = getenv("SW_AUDIO_VOLUME"); g_wo_vol = e && *e ? atoi(e) : 100;
                        if (g_wo_vol > 100) g_wo_vol = 100; }
    return g_wo_vol < 0 ? 0 : g_wo_vol;
}
void sw_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (g_wo_state == 1 || percent > 0) g_wo_vol = percent;   /* (no device yet: the next open uses it) */
    else g_wo_vol = 0;
}
static s16 wo_clamp(int v) { return (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
/* queue slot i: guest PCM16 (ch channels) -> stereo on the device */
static void wo_queue(int i, const s16 *src, u64 frames, u32 ch)
{
    if (!wo_open() || !frames) return;
    u32 need = (u32)(frames * 4);
    if (g_wh[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wo, &g_wh[i], sizeof g_wh[i]);
    if (g_wcap[i] < need) { free(g_wbuf[i]); g_wbuf[i] = (s16 *)malloc(need); g_wcap[i] = g_wbuf[i] ? need : 0; }
    if (!g_wbuf[i]) return;
    s16 *d = g_wbuf[i];
    int vol = g_wo_vol * 1024 / 100;
    for (u64 k = 0; k < frames; k++) {
        const s16 *s = src + k * ch;
        int l, r;
        if (ch == 6) {                   /* FL FR FC LFE BL BR */
            int c = s[2] * 181 >> 8, lfe = s[3] >> 2;
            l = s[0] + c + (s[4] * 181 >> 8) + lfe, r = s[1] + c + (s[5] * 181 >> 8) + lfe;
        } else if (ch == 1) l = r = s[0];
        else l = s[0], r = s[1];
        d[2 * k] = wo_clamp(l * vol >> 10), d[2 * k + 1] = wo_clamp(r * vol >> 10);
    }
    {   /* SW_AUDIO_LOG=1: loudest sample every 100 buffers */
        static int al = -1, nb, peak;
        if (al < 0) { const char *e = getenv("SW_AUDIO_LOG"); al = e && *e == '1'; }
        if (al) {
            for (u64 k = 0; k < frames * 2; k++) { int v = d[k] < 0 ? -d[k] : d[k]; if (v > peak) peak = v; }
            if (++nb % 100 == 0) { sw_report("[audio] buffers %d, peak %d (%u ch)\n", nb, peak, ch); peak = 0; }
        }
    }
    memset(&g_wh[i], 0, sizeof g_wh[i]);
    g_wh[i].lpData = (LPSTR)d, g_wh[i].dwBufferLength = need;
    if (waveOutPrepareHeader(g_wo, &g_wh[i], sizeof g_wh[i]) == MMSYSERR_NOERROR)
        waveOutWrite(g_wo, &g_wh[i], sizeof g_wh[i]);
}
/* slot i finished on the device (1 without a device: the timer decides) */
static int wo_done(int i)
{
    if (g_wo_state != 1) return 1;
    return !(g_wh[i].dwFlags & WHDR_PREPARED) || (g_wh[i].dwFlags & WHDR_DONE);
}
#else
static void wo_close(void) {}
static void wo_queue(int i, const s16 *src, u64 frames, u32 ch) { (void)i, (void)src, (void)frames, (void)ch; }
static int wo_done(int i) { (void)i; return 1; }
static int g_wo_state;
#endif

static u64 ao_now_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (u64)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
#else
    return (u64)clock() * (1000000000ull / CLOCKS_PER_SEC);
#endif
}
/* schedule queued buffers while started (back to back from now or the last end) */
static void ao_schedule(void)
{
    u64 now = ao_now_ns();
    if (!g_ao.started) return;
    if (g_ao.clock_ns < now) g_ao.clock_ns = now;
    for (int k = 0; k < g_ao.n; k++) {
        int i = (g_ao.head + k) % AO_MAXQ;
        if (g_ao.end_ns[i]) continue;
        g_ao.clock_ns += g_ao.samples[i] * 1000000000ull / AO_RATE;
        g_ao.end_ns[i] = g_ao.clock_ns;
    }
}
/* ns until the audio buffer event fires: 0 = a buffer is done (or nothing to wait for) */
s64 sw_ipc_event_delay_ns(u32 h)
{
    if (h < IPC_EVENT_BASE || h >= IPC_EVENT_BASE + 0x1000u) return 0;
    if (g_ao.event < 0 || (int)(h - IPC_EVENT_BASE) != g_ao.event) return 0;
    AO_LOCK();
    s64 d = 0;
    if (g_ao.started && g_ao.n) {
        u64 end = g_ao.end_ns[g_ao.head], now = ao_now_ns();
        d = end > now ? (s64)(end - now) : 0;
        if (g_wo_state == 1) d = wo_done(g_ao.head) ? 0 : d > 1000000 ? d : 1000000;
    } else if (g_ao.started) {
        d = 5000000;                     /* started, nothing queued: poll every 5 ms */
    }
    AO_UNLOCK();
    return d;
}
static IpcBuf *ao_out_buf(IpcCtx *x) { return x->nb && x->b[0].size ? &x->b[0] : (x->nc ? &x->cbuf[0] : NULL); }
static IpcBuf *ao_in_buf(IpcCtx *x) { return x->na && x->a[0].size ? &x->a[0] : (x->nx ? &x->x[0] : NULL); }
static void ao_name(IpcCtx *x, const char *name, u64 size)
{
    char buf[0x100] = {0};
    strncpy(buf, name, sizeof buf - 1);
    IpcBuf *b = ao_out_buf(x);
    if (b) buf_write(x, b, buf, size);
}
static int handle_audio(IpcCtx *x)
{
    static int stub = -1;
    if (stub < 0) { const char *e = getenv("SW_AUDIO_OUT"); stub = e && *e == '0'; }   /* on unless SW_AUDIO_OUT=0 */
    if (stub) return 0;
    if (name_is(x, "IAudioDevice")) {
        switch (x->cmd) {
        case 0: case 6: ao_name(x, "AudioTvOutput", 0x100); out_u32(x, 1); return 1;   /* ListAudioDeviceName(Auto) */
        case 1: case 7: return 1;                                                      /* SetAudioDeviceOutputVolume */
        case 2: case 8: { float v = 1.0f; u32 b; memcpy(&b, &v, 4); out_u32(x, b); return 1; }   /* GetAudioDeviceOutputVolume */
        case 3: case 10: case 13: ao_name(x, "AudioTvOutput", 0x100); return 1;         /* GetActiveAudioDeviceName(Auto) */
        case 5: out_u32(x, 2); return 1;                                               /* GetActiveChannelCount */
        default: return 0;                                                             /* events: generic table */
        }
    }
    if (name_is(x, "audout:u")) {
        if (x->cmd == 0 || x->cmd == 2) { ao_name(x, "DeviceOut", 0x100); out_u32(x, 1); return 1; }   /* ListAudioOuts(Auto) */
        if (x->cmd == 1 || x->cmd == 3) {                          /* OpenAudioOut(Auto): rate u32, channels u16, aruid */
            u32 ch = in_u32(x, 4) & 0xffff;
            if (ch != 2 && ch != 6) ch = 2;
            AO_LOCK();
            wo_close();
            memset(&g_ao, 0, sizeof g_ao);
            g_ao.event = -1, g_ao.open = 1, g_ao.channels = ch;
            AO_UNLOCK();
            ao_name(x, "DeviceOut", 0x100);
            ret_obj(x, "IAudioOut");
            out_u32(x, AO_RATE); out_u32(x, ch); out_u32(x, 2 /* PcmInt16 */); out_u32(x, 1 /* Stopped */);
            return 1;
        }
        return 0;
    }
    if (!name_is(x, "IAudioOut")) return 0;
    AO_LOCK();
    int handled = 1;
    switch (x->cmd) {
    case 0: out_u32(x, g_ao.started ? 0 : 1); break;               /* GetAudioOutState: 0 started, 1 stopped */
    case 1: g_ao.started = 1; ao_schedule();                       /* StartAudioOut */
#if defined(_WIN32)
        if (g_wo_state == 1) waveOutRestart(g_wo);
#endif
        break;
    case 2: g_ao.started = 0;                                      /* StopAudioOut */
#if defined(_WIN32)
        if (g_wo_state == 1) waveOutPause(g_wo);
#endif
        break;
    case 3: case 7: {                                              /* AppendAudioOutBuffer(Auto)(u64 tag; buffer info) */
        u64 tag = in_u64(x, 0), info[5] = {0};
        IpcBuf *b = ao_in_buf(x);
        u8 *p = b && b->size >= 40 ? sw_mem_ptr(x->c, b->addr, 40, 0) : NULL;
        if (p) memcpy(info, p, 40);                                /* next, samples, capacity, size, offset */
        {
            static int al = -1, nl;
            if (al < 0) { const char *e = getenv("SW_AUDIO_LOG"); al = e && *e == '1'; }
            if (al && nl++ < 20)
                sw_report("[audio] append tag %#llx buf %s %#llx+%#llx: next %#llx samples %#llx cap %#llx size %#llx off %#llx; in_len %u\n",
                          (unsigned long long)tag, b == &x->a[0] ? "A" : "X", (unsigned long long)(b ? b->addr : 0),
                          (unsigned long long)(b ? b->size : 0), (unsigned long long)info[0], (unsigned long long)info[1],
                          (unsigned long long)info[2], (unsigned long long)info[3], (unsigned long long)info[4], x->in_len);
        }
        if (g_ao.n < AO_MAXQ) {
            int i = (g_ao.head + g_ao.n) % AO_MAXQ;
            u64 frames = info[3] / (2ull * (g_ao.channels ? g_ao.channels : 2));
            g_ao.tag[i] = tag, g_ao.samples[i] = frames ? frames : 240, g_ao.end_ns[i] = 0;
            g_ao.n++;
            ao_schedule();
            if (frames && info[1]) {
                u32 ch = g_ao.channels ? g_ao.channels : 2;
                const s16 *pcm = (const s16 *)sw_mem_ptr(x->c, info[1] + info[4], frames * 2ull * ch, 0);
                if (pcm) {   /* waveOutWrite is slow-ish: outside the kernel lock (never take it holding g_ao_lock) */
                    extern int sw_klock_leave(void); extern void sw_klock_return(int d);
                    AO_UNLOCK();
                    int kd = sw_klock_leave();
                    AO_LOCK();
                    wo_queue(i, pcm, frames, ch);
                    AO_UNLOCK();
                    sw_klock_return(kd);
                    AO_LOCK();
                }
            }
        }
        break;
    }
    case 4:                                                        /* RegisterBufferEvent */
        AO_UNLOCK();
        ret_event(x, 0);
        AO_LOCK();
        g_ao.event = (int)(x->out_copy[x->nout_copy - 1] - IPC_EVENT_BASE);
        break;
    case 5: case 8: {                                              /* GetReleasedAudioOutBuffers(Auto) */
        IpcBuf *b = ao_out_buf(x);
        u64 cap = b ? b->size / 8 : 0, out[AO_MAXQ], now = ao_now_ns();
        u32 cnt = 0;
        while (g_ao.n && cnt < cap && cnt < AO_MAXQ && g_ao.end_ns[g_ao.head] &&
               (g_wo_state == 1 ? wo_done(g_ao.head) : g_ao.end_ns[g_ao.head] <= now)) {
            out[cnt++] = g_ao.tag[g_ao.head];
            g_ao.played += g_ao.samples[g_ao.head];
            g_ao.head = (g_ao.head + 1) % AO_MAXQ, g_ao.n--;
        }
        /* the whole output array, unused slots 0 like HOS: nn::audio::GetReleasedAudioOutBuffer
         * ignores the count and returns slot 0 (its own uninitialised stack word if we leave it) */
        if (b && cap) {
            u64 n = cap < AO_MAXQ ? cap : AO_MAXQ;
            for (u64 k = cnt; k < n; k++) out[k] = 0;
            buf_write(x, b, out, 8ull * n);
        }
        out_u32(x, cnt);
        {
            static int al = -1, nl;
            if (al < 0) { const char *e = getenv("SW_AUDIO_LOG"); al = e && *e == '1'; }
            if (al && nl++ < 30)
                sw_report("[audio] released %u (queue %d, head ends in %lld us, out buf %#llx+%#llx, nb %d nc %d)\n", cnt, g_ao.n,
                          g_ao.n ? (long long)((s64)g_ao.end_ns[g_ao.head] - (s64)now) / 1000 : -1LL,
                          (unsigned long long)(b ? b->addr : 0), (unsigned long long)(b ? b->size : 0), x->nb, x->nc);
        }
        if (!cnt) {   /* nothing done yet: the caller polls; sleep it until the next buffer ends (max 20 ms) */
            u64 d = 5000000;
            if (g_ao.started && g_ao.n && g_ao.end_ns[g_ao.head] > now) d = g_ao.end_ns[g_ao.head] - now;
            else if (g_wo_state == 1 && g_ao.started && g_ao.n) d = 1000000;   /* device a little behind the timer */
            g_ipc_sleep_ns = (s64)(d > 20000000 ? 20000000 : d);
        }
        break;
    }
    case 6: {                                                      /* ContainsAudioOutBuffer(u64 tag) */
        u64 tag = in_u64(x, 0);
        int found = 0;
        for (int k = 0; k < g_ao.n; k++) if (g_ao.tag[(g_ao.head + k) % AO_MAXQ] == tag) found = 1;
        out_u8(x, (u8)found);
        break;
    }
    case 9: out_u32(x, (u32)g_ao.n); break;                        /* GetAudioOutBufferCount */
    case 10: out_u64(x, g_ao.played); break;                       /* GetAudioOutPlayedSampleCount */
    case 11: out_u8(x, g_ao.n ? 1 : 0); g_ao.n = 0;                /* FlushAudioOutBuffers */
#if defined(_WIN32)
        if (g_wo_state == 1) waveOutReset(g_wo);
#endif
        break;
    case 12: break;                                                /* SetAudioOutVolume */
    case 13: { float v = 1.0f; u32 bits; memcpy(&bits, &v, 4); out_u32(x, bits); break; }   /* GetAudioOutVolume */
    default: handled = 0;
    }
    AO_UNLOCK();
    return handled;
}

/* ------------------------------------------------------------------ */
/* hwopus: the game's music and voices are Opus streams it decodes     */
/* through the system's "hardware" Opus decoder service.  Decoded here */
/* with libopus, loaded at run time (opus.dll / libopus-0.dll next to  */
/* the exe, or SW_OPUS_DLL=path); without it every decode returns      */
/* silence of the packet's length, so the game keeps running.          */
/* Commands as in switchbrew / nxaevora's services/audio.c.            */
/* ------------------------------------------------------------------ */
typedef struct OpusMSDecoder OpusMSDecoder;
static struct {
    int tried, ok;
    OpusMSDecoder *(*create)(s32 fs, int ch, int streams, int coupled, const unsigned char *map, int *err);
    int (*decode)(OpusMSDecoder *st, const unsigned char *data, s32 len, s16 *pcm, int frame_size, int fec);
    int (*ctl)(OpusMSDecoder *st, int req, ...);
    void (*destroy)(OpusMSDecoder *st);
} g_opus;
typedef struct {
    OpusMSDecoder *dec;
    u32 rate, channels;
} HwOpus;
static HwOpus g_hwopus[IPC_MAX_OBJS];
#if defined(_WIN32)
static SRWLOCK g_opus_lock = SRWLOCK_INIT;
#define OPUS_LOCK() AcquireSRWLockExclusive(&g_opus_lock)
#define OPUS_UNLOCK() ReleaseSRWLockExclusive(&g_opus_lock)
#else
#define OPUS_LOCK() (void)0
#define OPUS_UNLOCK() (void)0
#endif
#define HWOPUS_ERR_PACKET   ((111u) | (17u << 9))     /* InvalidPacket */
#define HWOPUS_ERR_RATE     ((111u) | (1001u << 9))
#define HWOPUS_ERR_CHANNELS ((111u) | (1002u << 9))
#define HWOPUS_ERR_NODEC    ((111u) | (385u << 9))    /* OutOfOpusDecoders */

static int opus_load(void)
{
    if (g_opus.tried) return g_opus.ok;
    g_opus.tried = 1;
#if defined(_WIN32)
    HMODULE m = NULL;
    const char *e = getenv("SW_OPUS_DLL");
    if (e && *e) m = LoadLibraryA(e);
    if (!m) {   /* next to the exe first, never the current directory */
        static const char *names[] = {"opus.dll", "libopus-0.dll", "libopus.dll"};
        char dir[MAX_PATH], path[MAX_PATH + 32];
        DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
        while (n && dir[n - 1] != '\\' && dir[n - 1] != '/') n--;
        dir[n] = 0;
        for (int i = 0; i < 3 && !m; i++) {
            snprintf(path, sizeof path, "%s%s", dir, names[i]);
            m = LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }
    }
    if (m) {
        *(FARPROC *)&g_opus.create = GetProcAddress(m, "opus_multistream_decoder_create");
        *(FARPROC *)&g_opus.decode = GetProcAddress(m, "opus_multistream_decode");
        *(FARPROC *)&g_opus.ctl = GetProcAddress(m, "opus_multistream_decoder_ctl");
        *(FARPROC *)&g_opus.destroy = GetProcAddress(m, "opus_multistream_decoder_destroy");
        g_opus.ok = g_opus.create && g_opus.decode && g_opus.ctl && g_opus.destroy;
    }
#endif
    sw_report(g_opus.ok ? "[audio] opus: libopus loaded (music and voices on)\n"
                        : "[audio] opus: no opus.dll / libopus-0.dll next to the exe - music and voices stay silent\n");
    return g_opus.ok;
}

static int opus_rate_ok(u32 r) { return r == 8000 || r == 12000 || r == 16000 || r == 24000 || r == 48000; }

/* samples per channel at 48 kHz of an Opus packet (RFC 6716 3.1), 0 = not one */
static u32 opus_packet_48k(const u8 *p, u32 len)
{
    static const u16 silk[4] = {480, 960, 1920, 2880}, hybrid[2] = {480, 960}, celt[4] = {120, 240, 480, 960};
    if (!len) return 0;
    u32 cfg = p[0] >> 3, frame = cfg < 12 ? silk[cfg & 3] : cfg < 16 ? hybrid[cfg & 1] : celt[cfg & 3], frames;
    switch (p[0] & 3) {
    case 0: frames = 1; break;
    case 1: case 2: frames = 2; break;
    default: if (len < 2) return 0; frames = p[1] & 0x3F;
    }
    frames *= frame;
    return frames > 5760 ? 0 : frames;
}

/* copy len guest bytes page-wise (never across a 64 KB guest page in one sw_mem_ptr) */
static u64 buf_read(IpcCtx *x, u64 addr, void *dst, u64 len)
{
    u64 done = 0;
    while (done < len) {
        u64 a = addr + done, n = 0x10000 - (a & 0xFFFF);
        if (n > len - done) n = len - done;
        u8 *p = sw_mem_ptr(x->c, a, n, 0);
        if (!p) break;
        memcpy((u8 *)dst + done, p, (size_t)n);
        done += n;
    }
    return done;
}

static u32 hwopus_work_size(u32 rate, u32 ch, u32 streams, int multi, u32 *size)
{
    if (multi ? (!ch || ch > 255) : (ch != 1 && ch != 2)) return HWOPUS_ERR_CHANNELS;
    if (!opus_rate_ok(rate)) return HWOPUS_ERR_RATE;
    *size = 0x8000u + 0x8000u * (multi ? (streams ? streams : 1) : 1);
    return 0;
}

static void hwopus_open(IpcCtx *x, u32 rate, u32 ch, u32 streams, u32 coupled, const u8 *map)
{
    u8 defmap[2] = {0, 1};
    if (!opus_rate_ok(rate)) { x->result = HWOPUS_ERR_RATE; return; }
    if (!ch || ch > 255 || (!map && ch > 2)) { x->result = HWOPUS_ERR_CHANNELS; return; }
    int id = ret_obj_id(x, "IHardwareOpusDecoder");
    if (id <= 0) { x->result = HWOPUS_ERR_NODEC; return; }
    OPUS_LOCK();
    HwOpus *h = &g_hwopus[id];
    if (h->dec && g_opus.destroy) g_opus.destroy(h->dec);   /* stale decoder of a closed object in this slot */
    memset(h, 0, sizeof *h);
    h->rate = rate, h->channels = ch;
    if (opus_load()) {
        int err = 0;
        h->dec = g_opus.create((s32)rate, (int)ch, map ? (int)streams : 1, map ? (int)coupled : (int)ch - 1,
                               map ? map : defmap, &err);
        if (!h->dec) sw_report("[audio] opus: decoder %u Hz %u ch (%u streams) failed: %d\n", rate, ch, streams, err);
    }
    OPUS_UNLOCK();
    static int nl;
    if (nl++ < 16) sw_report("[audio] opus decoder #%d: %u Hz, %u ch%s\n", id, rate, ch, map ? " (multistream)" : "");
}

static int hwopus_decode_req(IpcCtx *x, int reset, int perf);
static int handle_hwopus(IpcCtx *x)
{
    if (name_is(x, "hwopus")) {
        u32 size = 0, rc, v[4] = {0};
        u8 map[256] = {0};
        int multi = x->cmd == 2 || x->cmd == 3 || x->cmd == 6 || x->cmd == 7 || x->cmd == 9;
        int ex = x->cmd == 6 || x->cmd == 7 || x->cmd == 9;
        if (multi) {   /* parameters in a pointer (X) buffer: rate, ch, streams, coupled, [flags u64], mapping */
            IpcBuf *b = x->nx && x->x[0].size ? &x->x[0] : (x->na ? &x->a[0] : NULL);
            if (b && b->size >= 16) {
                u64 mo = ex ? 0x18 : 0x10, ml = b->size > mo ? b->size - mo : 0;
                buf_read(x, b->addr, v, 16);
                buf_read(x, b->addr + mo, map, ml > 255 ? 255 : ml);
            }
        } else {
            v[0] = in_u32(x, 0), v[1] = in_u32(x, 4);
        }
        switch (x->cmd) {
        case 1: case 3: case 5: case 7: case 8: case 9:            /* GetWorkBufferSize* */
            rc = hwopus_work_size(v[0], v[1], v[2], multi, &size);
            if (rc) x->result = rc; else out_u32(x, size);
            return 1;
        case 0: case 4:                                            /* OpenHardwareOpusDecoder(Ex) */
            hwopus_open(x, v[0], v[1], 1, v[1] - 1, NULL);
            return 1;
        case 2: case 6:                                            /* OpenHardwareOpusDecoderForMultiStream(Ex) */
            if (!v[2] || v[3] > v[2] || v[2] + v[3] > 255) { x->result = HWOPUS_ERR_CHANNELS; return 1; }
            hwopus_open(x, v[0], v[1], v[2], v[3], map);
            return 1;
        }
        return 0;
    }
    if (!name_is(x, "IHardwareOpusDecoder")) return 0;
    int reset = 0, perf = 0;
    switch (x->cmd) {
    case 0: case 2: break;                                         /* DecodeInterleaved(ForMultiStream)Old */
    case 4: case 5: perf = 1; break;                               /* ...WithPerfOld */
    case 6: case 7: case 8: case 9: perf = 1; reset = x->in_len ? x->in[0] & 1 : 0; break;   /* ...AndReset / new */
    case 1: case 3: return 1;                                      /* SetContext */
    default: return 0;
    }
    /* decoding takes ~50 us a packet: not under the kernel lock (every other guest thread would wait) */
    extern int sw_klock_leave(void); extern void sw_klock_return(int d);
    int kd = sw_klock_leave();
    int r = hwopus_decode_req(x, reset, perf);
    sw_klock_return(kd);
    return r;
}

static int hwopus_decode_req(IpcCtx *x, int reset, int perf)
{
    IpcBuf *in = ao_in_buf(x), *out = ao_out_buf(x);
    HwOpus *h = &g_hwopus[x->obj > 0 && x->obj < IPC_MAX_OBJS ? x->obj : 0];
    u64 t0 = ao_now_ns();
    u8 hdr[8];
    if (!in || !out || in->size < 8 || buf_read(x, in->addr, hdr, 8) != 8 || !h->channels) { x->result = HWOPUS_ERR_PACKET; return 1; }
    u32 size = (u32)hdr[0] << 24 | (u32)hdr[1] << 16 | (u32)hdr[2] << 8 | hdr[3];
    if (!size || size > in->size - 8 || size > 0x10000) { x->result = HWOPUS_ERR_PACKET; return 1; }
    u8 *pkt = (u8 *)malloc(size);
    if (!pkt || buf_read(x, in->addr + 8, pkt, size) != size) { free(pkt); x->result = HWOPUS_ERR_PACKET; return 1; }
    u32 step = 48000 / h->rate, want = opus_packet_48k(pkt, size) / step;
    u64 cap = out->size / (2ull * h->channels);
    s16 *buf = want && want <= cap ? (s16 *)malloc((size_t)want * h->channels * 2) : NULL;
    if (!buf) { free(pkt); x->result = HWOPUS_ERR_PACKET; return 1; }
    int got = -1;
    OPUS_LOCK();
    if (h->dec) {
        if (reset) g_opus.ctl(h->dec, 4028 /* OPUS_RESET_STATE */);
        got = g_opus.decode(h->dec, pkt, (s32)size, buf, (int)want, 0);
    }
    OPUS_UNLOCK();
    if (got < 0) {   /* no libopus / broken packet: silence of the right length keeps the game's timing */
        static int nl;
        if (h->dec && nl++ < 8) sw_report("[audio] opus decode error %d (packet %u bytes)\n", got, size);
        got = (int)want;
        memset(buf, 0, (size_t)want * h->channels * 2);
    }
    buf_write(x, out, buf, (u64)got * h->channels * 2);
    free(buf);
    free(pkt);
    out_u32(x, 8 + size);                                          /* consumed */
    out_u32(x, (u32)got);                                          /* samples per channel */
    if (perf) out_u64(x, (ao_now_ns() - t0) / 1000);               /* time taken, us */
    return 1;
}

static int handle_specific(IpcCtx *x)
{
    display_init();
    if (handle_hwopus(x)) return 1;
    if (handle_audio(x)) return 1;
    if (handle_am_storage(x)) return 1;
    if (handle_fs(x)) return 1;
    if (handle_vi(x)) return 1;
    if (handle_pl(x)) return 1;
    /* service manager */
    if (name_is(x, "sm:")) {
        if (x->cmd == 0 || x->cmd == 4) return 1;                /* RegisterClient / DetachClient */
        if (x->cmd == 1) {                                       /* GetService(name[8]) */
            char nm[9] = {0};
            memcpy(nm, x->in, x->in_len < 8 ? x->in_len : 8);
            ret_obj(x, nm);
            {   /* each service name once (audren:u is opened ~50 times a second) */
                static char seen[64][9]; static int nseen;
                int k = 0, fresh = 0;
                while (k < nseen && strcmp(seen[k], nm)) k++;
                if (k == nseen && nseen < 64) { memcpy(seen[nseen++], nm, 9); fresh = 1; }
                if (fresh || ipc_verbose())
                    sw_report("[ipc] sm:GetService(\"%s\")\n", nm);
            }
            return 1;
        }
        return 0;
    }
    if (name_is(x, "ILogger") && x->cmd == 0) {                 /* Log(A buffer: lm packet) */
        /* print the printable runs of the packet (text, file, function) */
        /* auto-select buffer: the packet is in A or in X, whichever is non-empty */
        IpcBuf *lb = (x->na && x->a[0].size) ? &x->a[0] : ((x->nx && x->x[0].size) ? &x->x[0] : NULL);
        if (lb) {
            u64 n = lb->size < 4096 ? lb->size : 4096;
            char line[4200]; int k = 0, run = 0;
            for (u64 i = 0; i < n; i++) {
                u8 *p = sw_mem_ptr(x->c, lb->addr + i, 1, 0);
                u8 ch = p ? *p : 0;
                if (ch >= 0x20 && ch < 0x7F) { line[k++] = (char)ch; run++; }
                else if (ch == '\n') { line[k++] = ' '; }
                else if (run) { if (run < 3) k -= run; else line[k++] = '|'; run = 0; }
            }
            line[k] = 0;
            if (k) sw_report("[game log] %s\n", line);
        }
        return 1;
    }
    if (name_is(x, "ICommonStateGetter")) {
        if (x->cmd == 1) {                                       /* ReceiveMessage */
            /* AM queues FocusStateChanged (15) for a freshly started
             * application; nn::oe waits for it before continuing. */
            static const u32 boot_msgs[] = {15};
            static u32 next_msg;
            if (next_msg < sizeof boot_msgs / sizeof boot_msgs[0]) { out_u32(x, boot_msgs[next_msg++]); return 1; }
            x->result = 0x680;                                   /* no message */
            return 1;
        }
        if (getenv("SW_RT_TRACE") && (x->cmd == 5 || x->cmd == 6 || x->cmd == 60)) {   /* who asks (resolution RE) */
            u64 fp = x->c->x[29], r[6] = {0};
            for (int i = 0; i < 6 && fp && !(fp & 7); i++) { r[i] = sw_load64(x->c, fp + 8); fp = sw_load64(x->c, fp); }
            sw_report("[rt] am cmd %u lr %llx chain %llx %llx %llx %llx %llx %llx\n", x->cmd,
                      (unsigned long long)x->c->x[30], (unsigned long long)r[0], (unsigned long long)r[1],
                      (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)r[4], (unsigned long long)r[5]);
        }
        if (x->cmd == 5) { out_u8(x, g_docked ? 1 : 0); return 1; }   /* GetOperationMode: 0 handheld, 1 docked */
        if (x->cmd == 6) { out_u32(x, g_docked ? 1 : 0); return 1; }  /* GetPerformanceMode: 0 normal, 1 boost */
        if (x->cmd == 9) { out_u8(x, 1); return 1; }             /* GetCurrentFocusState: in focus */
        if (x->cmd == 60) { out_u32(x, g_disp_w); out_u32(x, g_disp_h); return 1; }   /* GetDefaultDisplayResolution */
    }
    if (name_is(x, "IApplicationFunctions")) {
        if (x->cmd == 1) {                                        /* PopLaunchParameter(kind) */
            /* kind 2 (PreselectedUser): the account the console picked at
             * launch; other kinds: nothing queued -> ResultNotAvailable */
            if (in_u32(x, 0) == 2 && !g_launch_user_popped) {
                g_launch_user_popped = 1;
                ret_obj(x, "IStorage_launch");
            } else {
                x->result = 0x480;                                /* am: (128, 2) not available */
            }
            return 1;
        }
        if (x->cmd == 21) { out_u64(x, 0x00002d53552d6e65ull); return 1; }      /* GetDesiredLanguage "en-US" */
        if (x->cmd == 40) { out_u8(x, 0); return 1; }                           /* NotifyRunning */
        if (x->cmd == 20) { out_u64(x, 0); return 1; }                          /* EnsureSaveData */
    }
    if (name_is(x, "IWindowController") && x->cmd == 1) { out_u64(x, 0x1000); return 1; }   /* ARUID */
    if (name_is(x, "set") || name_is(x, "set:sys")) {
        if (x->cmd == 0 || x->cmd == 3) { out_u64(x, 0x00002d53552d6e65ull); return 1; }   /* language code */
        if (x->cmd == 1 || x->cmd == 5) {                         /* GetAvailableLanguageCodes */
            u64 en = 0x00002d53552d6e65ull;
            IpcBuf *b = x->nc ? &x->cbuf[0] : (x->nb ? &x->b[0] : NULL);
            if (b) buf_write(x, b, &en, 8);
            out_u32(x, 1);
            return 1;
        }
        if (x->cmd == 4) { out_u32(x, 1); return 1; }             /* GetRegionCode: USA */
    }
    if (name_is(x, "apm") || name_is(x, "apm:p")) {
        if (x->cmd == 1) { out_u32(x, g_docked ? 1 : 0); return 1; }   /* GetPerformanceMode */
    }
    if (name_is(x, "ISession") && x->cmd == 1) { out_u32(x, 0x00020003); return 1; }  /* GetPerformanceConfiguration */
    if (name_is(x, "ISteadyClock")) {
        if (x->cmd == 0) {                                        /* GetCurrentTimePoint: u64 ticks(s), uuid */
            out_u64(x, (u64)(sw_tick_now() / 19200000ull));
            out_u64(x, 0x1111111111111111ull); out_u64(x, 0x2222222222222222ull);
            return 1;
        }
    }
    if (name_is(x, "ISystemClock") && x->cmd == 0) {              /* GetCurrentTime: unix seconds (real clock) */
        out_u64(x, (u64)time(NULL));
        return 1;
    }
    if (name_is(x, "fsp-srv") && x->cmd == 1005) { out_u32(x, 0); return 1; }     /* GetGlobalAccessLogMode */
    if ((strncmp(x->name, "IStorage_", 9) == 0 && !name_is(x, "IStorage_launch") && !name_is(x, "IStorage_am"))
        || name_is(x, "IStorage")) {
        /* fs IStorage (romfs / patch); am's IStorage_launch is handled elsewhere */
        /* romfs and patch storage both see the (already updated) extracted tree */
        int romfs = !strcmp(x->name, "IStorage_romfs") || !strcmp(x->name, "IStorage_patch");
        if (x->cmd == 4) { out_u64(x, romfs ? sw_romfs_size() : 0); return 1; }   /* GetSize */
        if (x->cmd == 0) {                                        /* Read(offset, size) -> B buffer */
            u64 off = in_u64(x, 0), size = in_u64(x, 8);
            IpcBuf *b = x->nb ? &x->b[0] : NULL;
            if (ipc_verbose()) sw_report("[ipc] %s read off 0x%llx size 0x%llx buf 0x%llx/0x%llx\n", x->name,
                                         (unsigned long long)off, (unsigned long long)size,
                                         (unsigned long long)(b ? b->addr : 0), (unsigned long long)(b ? b->size : 0));
            if (b && romfs) {
                /* outside the kernel lock (a load reads up to 66 MB at once, ~30 ms, and every other guest thread's
                 * syscalls waited meanwhile): a buffer per host thread instead of one shared static buffer.
                 * SW_ROMFS_LOCKED=1: the old way, under the kernel lock. */
                static SW_TLS_IPC u8 *tmp_tls;
                enum { TMP_SIZE = 1 << 20 };
                if (!tmp_tls) tmp_tls = (u8 *)malloc(TMP_SIZE);
                u8 *tmp = tmp_tls;
                static int locked = -1;
                if (locked < 0) { const char *e = getenv("SW_ROMFS_LOCKED"); locked = e && *e == '1'; }
                extern int sw_klock_leave(void); extern void sw_klock_return(int d);
                int kd = locked || !tmp ? -1 : sw_klock_leave();
                if (!tmp) { static u8 fallback[TMP_SIZE]; tmp = fallback; }
                u64 done = 0;
                if (size > b->size) size = b->size;
                /* SW_ROMFS_SLOW=<ms>: log reads slower than that (they hold the kernel lock: every svc of every other
                 * guest thread waits meanwhile) */
                static double slow_ms = -1;
                if (slow_ms < 0) { const char *e = getenv("SW_ROMFS_SLOW"); slow_ms = e && *e ? atof(e) : 0; }
                LARGE_INTEGER q0, q1, qf;
                if (slow_ms > 0) QueryPerformanceCounter(&q0);
                while (done < size) {
                    u64 want = size - done < TMP_SIZE ? size - done : TMP_SIZE;
                    u64 got = sw_romfs_read(off + done, tmp, want);
                    if (!got) break;
                    IpcBuf part = {b->addr + done, got};
                    buf_write(x, &part, tmp, got);
                    done += got;
                }
                if (kd >= 0) sw_klock_return(kd);
                if (slow_ms > 0) {
                    QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
                    double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart;
                    if (ms > slow_ms) sw_report("[romfs] slow read: 0x%llx bytes at 0x%llx took %.2f ms\n",
                                                (unsigned long long)size, (unsigned long long)off, ms);
                }
            }
            return 1;
        }
    }
    if (name_is(x, "IManagerForApplication") && x->cmd == 0) { out_u8(x, 0); return 1; }  /* CheckAvailability */
    if (name_is(x, "IStorage_launch") && x->cmd == 0) { ret_obj(x, "IStorageAccessor_launch"); return 1; }  /* Open */
    if (name_is(x, "IStorageAccessor_launch")) {
        u8 rec[0x88];
        memset(rec, 0, sizeof rec);
        u32 magic = 0xC79497CAu;
        memcpy(rec, &magic, 4);
        rec[4] = 1;                                               /* is_account_selected */
        memcpy(rec + 8, ACC_UID, 16);
        if (x->cmd == 0) { out_u64(x, sizeof rec); return 1; }    /* GetSize */
        if (x->cmd == 11) {                                       /* Read(offset) -> C/B buffer */
            u64 off = 0;
            if (x->in_len >= 8) memcpy(&off, x->in, 8);
            IpcBuf *b = x->nc ? &x->cbuf[0] : (x->nb ? &x->b[0] : NULL);
            if (b && off < sizeof rec) buf_write(x, b, rec + off, sizeof rec - off);
            return 1;
        }
    }
    if (name_is(x, "acc:u0") || name_is(x, "acc:u1")) {
        if (x->cmd == 2 || x->cmd == 3 || x->cmd == 140) {        /* ListAllUsers / ListOpenUsers / ListQualifiedUsers */
            IpcBuf *b = x->nc ? &x->cbuf[0] : (x->nb ? &x->b[0] : NULL);
            if (b) {
                /* the whole array: one user, then invalid (zero) Uids; the
                 * client counts entries up to the first zero one */
                u8 list[0x100];
                memset(list, 0, sizeof list);
                memcpy(list, ACC_UID, 16);
                buf_write(x, b, list, b->size < sizeof list ? b->size : sizeof list);
            }
            if (ipc_verbose() && b)
                sw_report("[ipc] acc list users -> buf 0x%llx size 0x%llx\n",
                          (unsigned long long)b->addr, (unsigned long long)b->size);
            return 1;
        }
        if (x->cmd == 0) { out_u32(x, 1); return 1; }             /* GetUserCount */
        if (x->cmd == 1) { out_u8(x, 1); return 1; }              /* GetUserExistence */
        if (x->cmd == 4 || x->cmd == 110) {                       /* GetLastOpenedUser / ... : uid */
            u64 uid[2];
            memcpy(uid, ACC_UID, 16);
            out_u64(x, uid[0]); out_u64(x, uid[1]);
            return 1;
        }
    }
    if (name_is(x, "csrng") && x->cmd == 0) {                     /* GenerateRandomBytes -> B buffer */
        if (x->nb) {
            u8 rnd[256];
            for (u64 off = 0; off < x->b[0].size; off += sizeof rnd) {
                for (int i = 0; i < (int)sizeof rnd; i++) rnd[i] = (u8)rand();
                IpcBuf part = {x->b[0].addr + off, x->b[0].size - off};
                buf_write(x, &part, rnd, sizeof rnd);
            }
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */
typedef struct { char key[IPC_NAME_MAX + 12]; } SeenKey;
static SeenKey g_seen[4096];
static int g_seen_n;

static void log_once(IpcCtx *x, const char *what)
{
    char key[IPC_NAME_MAX + 12];
    snprintf(key, sizeof key, "%s#%u", x->name, x->cmd);
    for (int i = 0; i < g_seen_n; i++)
        if (strcmp(g_seen[i].key, key) == 0) return;
    if (g_seen_n < 4096) strcpy(g_seen[g_seen_n++].key, key);
    sw_report("[ipc] %s cmd %u (%s) in=%u bytes x=%d a=%d b=%d c=%d\n", x->name, x->cmd, what,
              x->in_len, x->nx, x->na, x->nb, x->nc);
}

static void dispatch(IpcCtx *x)
{
    x->result = 0;
    {   /* SW_AUDIO_LOG=1: every audio service call (first 400) */
        static int al = -1, n;
        if (al < 0) { const char *e = getenv("SW_AUDIO_LOG"); al = e && *e == '1'; }
        if (al && n < 400 && (strstr(x->name, "Audio") || strstr(x->name, "aud") || strstr(x->name, "Opus"))) {
            n++;
            sw_report("[audio] %s cmd %u in=%u x=%d a=%d b=%d c=%d\n", x->name, x->cmd, x->in_len, x->nx, x->na, x->nb, x->nc);
        }
    }
    if (handle_specific(x)) {
        log_once(x, "handled");
        return;
    }
    for (int i = 0; g_obj_cmds[i].iface; i++) {
        if (strcmp(g_obj_cmds[i].iface, x->name) == 0 && g_obj_cmds[i].cmd == x->cmd) {
            if (strcmp(g_obj_cmds[i].ret, "_none") != 0) ret_obj(x, g_obj_cmds[i].ret);
            log_once(x, "returns object");
            return;
        }
    }
    for (int i = 0; g_event_cmds[i].iface; i++) {
        if (strcmp(g_event_cmds[i].iface, x->name) == 0 && g_event_cmds[i].cmd == x->cmd) {
            ret_event(x, g_event_cmds[i].never);
            log_once(x, g_event_cmds[i].never ? "returns never-signalled event" : "returns event");
            return;
        }
    }
    /* default: success, zeroed output */
    x->out_len = 0x40;
    log_once(x, "stub");
}

/* ------------------------------------------------------------------ */
/* Message parsing / reply                                             */
/* ------------------------------------------------------------------ */
static u64 desc_addr(u32 lo, u32 hi_bits_36_38, u32 hi_bits_32_35)
{
    return (u64)lo | ((u64)hi_bits_32_35 << 32) | ((u64)hi_bits_36_38 << 36);
}

static void write_reply(u8 *msg, IpcCtx *x, int domain, u32 move_handles[], int nmove)
{
    u32 *w = (u32 *)msg;
    int has_special = (nmove + x->nout_copy) > 0;
    int p = 2;
    memset(msg, 0, 0x100);
    if (has_special) {
        w[p++] = ((u32)x->nout_copy << 1) | ((u32)nmove << 5);
        for (int i = 0; i < x->nout_copy; i++) w[p++] = x->out_copy[i];
        for (int i = 0; i < nmove; i++) w[p++] = move_handles[i];
    }
    /* X (send static) descriptors for the C buffers the handler filled: the
     * client's CMIF layer takes the out-pointer size from them */
    int nxd = 0;
    for (int i = 0; i < x->nc && i < 8; i++) {
        if (!x->c_written[i]) continue;
        u64 a = x->cbuf[i].addr, sz = x->c_written[i] > 0xFFFF ? 0xFFFF : x->c_written[i];
        w[p++] = ((u32)i & 0x3F) | ((u32)((a >> 36) & 7) << 6) | ((u32)((a >> 32) & 0xF) << 12) | ((u32)sz << 16);
        w[p++] = (u32)a;
        nxd++;
    }
    int data_word = p;
    int raw = ((p * 4 + 15) & ~15) / 4;                       /* 16-byte aligned */
    int q = raw;
    if (domain) {                                             /* domain out header */
        w[q++] = (u32)x->nout_objs;
        w[q++] = 0; w[q++] = 0; w[q++] = 0;
    }
    w[q++] = 0x4F434653;                                      /* "SFCO" */
    w[q++] = 0;
    w[q++] = x->result;
    w[q++] = 0;
    u32 len = (x->out_len + 3) & ~3u;
    if (len > 0x200) len = 0x200;
    if ((u32)q * 4 + len > 0x100 - 32) len = 0x100 - 32 - (u32)q * 4;
    memcpy(&w[q], x->out, len);
    q += (int)(len / 4);
    if (domain)
        for (int i = 0; i < x->nout_objs; i++) w[q++] = (u32)x->out_objs[i];
    int ndw = (q - data_word);                                /* includes alignment padding */
    w[0] = (u32)nxd << 16;                                    /* type 0, nxd X descriptors */
    w[1] = (u32)ndw | (has_special ? 0x80000000u : 0);
}

static u32 handle_request(SwCpu *c, u32 handle, u64 msg_addr)
{
    u8 *msg = sw_mem_ptr(c, msg_addr, 0x100, 1);
    IpcSess *s = sess_get(handle);
    if (!msg) return 0xE401;
    if (!s) {
        sw_report("[ipc] request on unknown handle 0x%x\n", handle);
        return 0xE401;                                        /* invalid handle */
    }
    u32 *w = (u32 *)msg;
    u32 type = w[0] & 0xFFFF;
    int nx = (w[0] >> 16) & 0xF, na = (w[0] >> 20) & 0xF, nb = (w[0] >> 24) & 0xF, nw = (w[0] >> 28) & 0xF;
    u32 ndw = w[1] & 0x3FF;
    u32 recv_mode = (w[1] >> 10) & 0xF;
    u32 recv_off = (w[1] >> 20) & 0x7FF;
    int has_special = (w[1] >> 31) & 1;
    IpcCtx *x = (IpcCtx *)calloc(1, sizeof(IpcCtx));
    x->c = c;
    int p = 2;
    if (has_special) {
        u32 sh = w[p++];
        int send_pid = sh & 1;
        x->ncopy_in = (sh >> 1) & 0xF;
        x->nmove_in = (sh >> 5) & 0xF;
        if (send_pid) { x->pid = (u64)w[p] | ((u64)w[p + 1] << 32); p += 2; }
        for (int i = 0; i < x->ncopy_in; i++) { u32 v = w[p++]; if (i < 8) x->copy_in[i] = v; }
        for (int i = 0; i < x->nmove_in; i++) { u32 v = w[p++]; if (i < 8) x->move_in[i] = v; }
    }
    for (int i = 0; i < nx; i++, p += 2) {
        u32 a = w[p], lo = w[p + 1];
        if (i < 8) { x->x[i].addr = desc_addr(lo, (a >> 6) & 7, (a >> 12) & 0xF); x->x[i].size = a >> 16; }
    }
    x->nx = nx < 8 ? nx : 8;
    for (int k = 0; k < na + nb + nw; k++, p += 3) {
        u32 s0 = w[p], lo = w[p + 1], f = w[p + 2];
        IpcBuf b = {desc_addr(lo, (f >> 2) & 7, (f >> 28) & 0xF), (u64)s0 | ((u64)((f >> 24) & 0xF) << 32)};
        if (k < na) { if (x->na < 8) x->a[x->na++] = b; }
        else if (k < na + nb) { if (x->nb < 8) x->b[x->nb++] = b; }
    }
    int raw_start = ((p * 4 + 15) & ~15);                     /* bytes */
    u8 *raw = msg + raw_start;
    /* C (receive list) descriptors */
    if (recv_mode >= 2) {
        int nc = recv_mode == 2 ? 1 : (int)recv_mode - 2;
        int off = recv_off ? (int)recv_off : p + (int)ndw;
        for (int i = 0; i < nc && i < 8 && off + 1 < 64; i++, off += 2) {
            u32 lo = w[off], hi = w[off + 1];
            x->cbuf[x->nc].addr = (u64)lo | ((u64)(hi & 0xFFFF) << 32);
            x->cbuf[x->nc].size = hi >> 16;
            x->nc++;
        }
    }

    u32 rc = 0;
    if (type == 2) {                                          /* Close */
        /* The client closes in two steps: this message (server side), then
         * svcCloseHandle (its handle).  Freeing the slot here let another core
         * get the same handle number in between, and the CloseHandle that
         * followed then closed that new session ("request on unknown handle"). */
        s->closed = 1;
        free(x);
        return 0;
    }
    if (type == 5 || type == 7) {                             /* Control */
        u32 cmd;
        memcpy(&cmd, raw + 8, 4);
        x->name = "control";
        x->result = 0;
        u32 move[1]; int nmove = 0;
        if (cmd == 0) {                                       /* ConvertCurrentObjectToDomain */
            s->domain = 1;
            out_u32(x, (u32)s->obj);
        } else if (cmd == 2 || cmd == 4) {                    /* CloneCurrentObject(Ex) */
            move[nmove++] = sess_new(s->obj);
            if (s->domain) g_sess[move[0] - IPC_SESS_BASE].domain = 1;
        } else if (cmd == 3) {                                /* QueryPointerBufferSize */
            out_u32(x, 0x8000);
        } else {
            sw_report("[ipc] control cmd %u\n", cmd);
        }
        write_reply(msg, x, 0, move, nmove);
        free(x);
        return 0;
    }
    if (type != 4 && type != 6) {
        sw_report("[ipc] unsupported message type %u\n", type);
        free(x);
        return 0;
    }

    int obj = s->obj;
    if (s->domain) {
        u8 dcmd = raw[0];
        u16 dlen; memcpy(&dlen, raw + 2, 2);
        u32 oid; memcpy(&oid, raw + 4, 4);
        if (dcmd == 2) {                                      /* close domain object */
            if (oid > 0 && oid < IPC_MAX_OBJS) g_objs[oid].used = 0;
            x->result = 0;
            write_reply(msg, x, 1, NULL, 0);
            free(x);
            return 0;
        }
        obj = (oid > 0 && oid < IPC_MAX_OBJS && g_objs[oid].used) ? (int)oid : 0;
        raw += 16;
        x->in_len = dlen >= 16 ? dlen - 16 : 0;
    } else {
        x->in_len = ndw * 4 > 32 ? ndw * 4 - 32 : 0;
    }
    if (!obj) {
        sw_report("[ipc] request to dead object (handle 0x%x)\n", handle);
        free(x);
        return 0xE401;
    }
    u32 magic; memcpy(&magic, raw, 4);
    memcpy(&x->cmd, raw + 8, 4);
    x->in = raw + 16;
    if (x->in_len > 0x100) x->in_len = 0x100;
    x->obj = obj;
    x->name = g_objs[obj].name;
    if (magic != 0x49434653)
        sw_report("[ipc] %s: bad CMIF magic 0x%08x\n", x->name, magic);
    if (ipc_verbose())
        sw_report("[ipc] -> %s cmd %u\n", x->name, x->cmd);

    dispatch(x);

    u32 move[8]; int nmove = 0;
    if (!s->domain) {
        /* non-domain: returned objects travel as move handles */
        for (int i = 0; i < x->nout_objs; i++) move[nmove++] = sess_new(x->out_objs[i]);
        x->nout_objs = 0;
    }
    write_reply(msg, x, s->domain, move, nmove);
    free(x);
    return rc;
}

/* svc entry points (called from sw_svc) */
int sw_ipc_svc(SwCpu *c, u32 num)
{
    switch (num) {
    case 0x1F: {                                              /* ConnectToNamedPort(name=x1) */
        char name[16] = {0};
        for (int i = 0; i < 12; i++) {
            u8 *p = sw_mem_ptr(c, c->x[1] + (u64)i, 1, 0);
            if (!p || !*p) break;
            name[i] = (char)*p;
        }
        int obj = obj_new(name);
        u32 h = sess_new(obj);
        sw_report("[ipc] ConnectToNamedPort(\"%s\") -> handle 0x%x\n", name, h);
        c->x[0] = h ? 0 : 0xD201;
        c->x[1] = h;
        return 1;
    }
    case 0x21:                                                /* SendSyncRequest(handle=x0) */
        c->x[0] = handle_request(c, (u32)c->x[0], c->tpidrro_el0);
        return 1;
    case 0x22:                                                /* SendSyncRequestWithUserBuffer(buf, size, handle) */
        c->x[0] = handle_request(c, (u32)c->x[2], c->x[0]);
        return 1;
    case 0x15: {                                              /* CreateTransferMemory(out h, addr, size, perm) */
        u32 h = tmem_new(c->x[1], c->x[2]);
        c->x[0] = h ? 0 : 0xD201;
        c->x[1] = h;
        return 1;
    }
    case 0x16: {                                              /* CloseHandle */
        u32 hc = (u32)c->x[0];
        IpcSess *s = sess_get(hc);
        if (s) sess_close(s);
        else if (hc >= IPC_SESS_BASE && hc < IPC_SESS_BASE + IPC_MAX_SESS && g_stale_close_log++ < 20)
            sw_report("[ipc] close of a session handle that is not open: 0x%x (lr 0x%llx)\n", hc, (unsigned long long)c->x[30]);
        if (hc >= IPC_TMEM_BASE && hc < IPC_TMEM_BASE + IPC_TMEM_MAX) g_tmem[hc - IPC_TMEM_BASE].used = 0;
        c->x[0] = 0;
        return 1;
    }
    }
    return 0;
}
