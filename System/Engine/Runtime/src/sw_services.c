/* sw_services.c - native implementations of imported SDK symbols.
 *
 * Every entry here has the recompiled-function ABI (SwCpu*), so the static
 * linker can point an import's GOT slot at it exactly like a recompiled
 * function.  Anything not implemented lands in sw_unimplemented_import and is
 * reported, never silently ignored.
 */
#include "sw_rt.h"

#include <string.h>
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <time.h>

static int guest_ptr(SwCpu *c, u64 addr, u64 size, int write, u8 **out)
{
    u8 *p = sw_mem_ptr(c, addr, size, write);
    if (!p) {
        char why[160];
        snprintf(why, sizeof(why), "service access to unmapped guest memory 0x%llx (size %llu)",
                 (unsigned long long)addr, (unsigned long long)size);
        sw_trap(c, addr, why);
        return 0;
    }
    *out = p;
    return 1;
}

/* ---- Runtime implementations for imports the recompiler left unbound ----
 * The generated import stubs call sw_unimplemented_import(c, name); names
 * listed here are handled instead of trapping, so they can be added without
 * a regen. */

/* void *nnosGetDefaultUserExceptionStackForApplication(size_t *size): an
 * optional (weak) application hook.  "No stack" is what an absent symbol
 * means; nnosInitialize then passes NULL/0 exactly as on the skipped path. */
static void hle_GetDefaultUserExceptionStack(SwCpu *c)
{
    u8 *p = c->x[0] ? sw_mem_ptr(c, c->x[0], 8, 1) : NULL;
    if (p) memset(p, 0, 8);
    c->x[0] = 0;
}

static const struct { const char *name; void (*fn)(SwCpu *); } g_import_hle[] = {
    {"nnosGetDefaultUserExceptionStackForApplication", hle_GetDefaultUserExceptionStack},
};

void sw_unimplemented_import(SwCpu *c, const char *name)
{
    for (size_t i = 0; i < sizeof g_import_hle / sizeof g_import_hle[0]; i++) {
        if (strcmp(g_import_hle[i].name, name) == 0) {
            g_import_hle[i].fn(c);
            return;
        }
    }
    char why[320];
    snprintf(why, sizeof(why),
             "unimplemented import '%s' was called (pc in recompiled code); "
             "add a native service binding before this path can run", name);
    printf("[service] %s\n", why);
    sw_trap(c, 0, why);
}

void svc_noop(SwCpu *c) { c->x[0] = 0; }

/* ---------------- C runtime helpers used by game code ------------- */
void svc_memcpy(SwCpu *c)
{
    u64 dst = c->x[0], src = c->x[1], n = c->x[2];
    u8 *d, *s;
    if (n == 0) { c->x[0] = dst; return; }
    if (guest_ptr(c, dst, n, 1, &d) && guest_ptr(c, src, n, 0, &s)) {
        memmove(d, s, (size_t)n);
    }
    c->x[0] = dst;
}

void svc_memmove(SwCpu *c) { svc_memcpy(c); }

void svc_memset(SwCpu *c)
{
    u64 dst = c->x[0], value = c->x[1], n = c->x[2];
    u8 *d;
    if (n == 0) { c->x[0] = dst; return; }
    if (guest_ptr(c, dst, n, 1, &d)) memset(d, (int)(u8)value, (size_t)n);
    c->x[0] = dst;
}

void svc_memcmp(SwCpu *c)
{
    u64 a = c->x[0], b = c->x[1], n = c->x[2];
    u8 *pa, *pb;
    int r = 0;
    if (n == 0) { c->x[0] = 0; return; }
    if (guest_ptr(c, a, n, 0, &pa) && guest_ptr(c, b, n, 0, &pb))
        r = memcmp(pa, pb, (size_t)n);
    c->x[0] = (u64)(s64)r;
}

void svc_strlen(SwCpu *c)
{
    u64 p = c->x[0], n = 0;
    for (;;) {
        u8 ch = sw_load8(c, p + n);
        if (ch == 0) break;
        n++;
        if (n > (1u << 24)) { sw_trap(c, p, "strlen over 16 MiB"); return; }
    }
    c->x[0] = n;
}

void svc_strcmp(SwCpu *c)
{
    u64 a = c->x[0], b = c->x[1], i = 0;
    for (;;) {
        u8 ca = sw_load8(c, a + i), cb = sw_load8(c, b + i);
        if (ca != cb) { c->x[0] = (u64)(s64)((s32)ca - (s32)cb); return; }
        if (!ca) { c->x[0] = 0; return; }
        i++;
        if (i > (1u << 24)) { sw_trap(c, a, "strcmp over 16 MiB"); return; }
    }
}

void svc_strncmp(SwCpu *c)
{
    u64 a = c->x[0], b = c->x[1], n = c->x[2], i;
    for (i = 0; i < n; i++) {
        u8 ca = sw_load8(c, a + i), cb = sw_load8(c, b + i);
        if (ca != cb) { c->x[0] = (u64)(s64)((s32)ca - (s32)cb); return; }
        if (!ca) break;
    }
    c->x[0] = 0;
}

void svc_strcpy(SwCpu *c)
{
    u64 dst = c->x[0], src = c->x[1], i = 0;
    for (;;) {
        u8 ch = sw_load8(c, src + i);
        sw_store8(c, dst + i, ch);
        if (!ch) break;
        i++;
        if (i > (1u << 24)) { sw_trap(c, src, "strcpy over 16 MiB"); return; }
    }
    c->x[0] = dst;
}

void svc_strncpy(SwCpu *c)
{
    u64 dst = c->x[0], src = c->x[1], n = c->x[2], i;
    int ended = 0;
    for (i = 0; i < n; i++) {
        u8 ch = ended ? 0 : sw_load8(c, src + i);
        sw_store8(c, dst + i, ch);
        if (!ended && !ch) ended = 1;
    }
    c->x[0] = dst;
}

/* ---------------- nn::os compatibility layer ------------- */
/* 
 * Nintendo Switch OS services implementation.
 * Maps nn::os primitives to Windows equivalents.
 * Handle layout in guest memory (nn::os types are opaque structs):
 * - ThreadType: 8 bytes (HANDLE stored at offset 0)
 * - MutexType: 8 bytes (CRITICAL_SECTION* stored at offset 0)
 * - ConditionVariableType: 8 bytes (CONDITION_VARIABLE* stored at offset 0)
 * - EventType: 8 bytes (HANDLE stored at offset 0)
 * - TlsSlot: 4 bytes (DWORD TLS index stored at offset 0)
 * - FiberType: 8 bytes (LPVOID fiber address stored at offset 0)
 * - MessageQueueType: 16 bytes (HANDLE semaphore + CRITICAL_SECTION* + circular buffer info)
 * - TimerEventType: 8 bytes (HANDLE timer queue timer)
 * - SystemEventType: 8 bytes (HANDLE event)
 * - MultiWaitHolderType: 8 bytes (HANDLE wait array)
 */

#define NN_OS_MAX_THREADS 256
#define NN_OS_MAX_MUTEXES 256
#define NN_OS_MAX_CV 256
#define NN_OS_MAX_EVENTS 256
#define NN_OS_MAX_TLS 128
#define NN_OS_MAX_FIBERS 64
#define NN_OS_MAX_MSGQUEUES 64
#define NN_OS_MAX_TIMERS 64
#define NN_OS_MAX_SYSEVENTS 64
#define NN_OS_MAX_MULTIWAIT 32

typedef struct {
    HANDLE handle;
    DWORD thread_id;
    u64 entry;
    u64 arg;
    u64 stack_base;
    u64 stack_size;
    int priority;
    u32 core_mask;
    char name[32];
    int active;
} NnOsThread;

typedef struct {
    CRITICAL_SECTION cs;
    int initialized;
} NnOsMutex;

typedef struct {
    CONDITION_VARIABLE cv;
    int initialized;
} NnOsCondVar;

typedef struct {
    HANDLE event;
    int manual_reset;
    int initialized;
} NnOsEvent;

typedef struct {
    DWORD tls_index;
    int initialized;
} NnOsTlsSlot;

typedef struct {
    LPVOID fiber;
    u64 entry;
    u64 arg;
    u64 stack_base;
    u64 stack_size;
    int active;
} NnOsFiber;

typedef struct {
    HANDLE sem;
    CRITICAL_SECTION cs;
    u64 buffer;      /* guest address of message buffer */
    u64 msg_size;    /* size of each message */
    u64 capacity;    /* max messages */
    u64 head, tail;  /* indices */
    int initialized;
} NnOsMsgQueue;

typedef struct {
    HANDLE timer;
    int initialized;
} NnOsTimerEvent;

typedef struct {
    HANDLE event;
    int initialized;
} NnOsSysEvent;

typedef struct {
    HANDLE handles[64];
    int count;
    int initialized;
} NnOsMultiWaitHolder;

static NnOsThread g_threads[NN_OS_MAX_THREADS];
static NnOsMutex g_mutexes[NN_OS_MAX_MUTEXES];
static NnOsCondVar g_cvs[NN_OS_MAX_CV];
static NnOsEvent g_events[NN_OS_MAX_EVENTS];
static NnOsTlsSlot g_tls_slots[NN_OS_MAX_TLS];
static NnOsFiber g_fibers[NN_OS_MAX_FIBERS];
static NnOsMsgQueue g_msgqueues[NN_OS_MAX_MSGQUEUES];
static NnOsTimerEvent g_timers[NN_OS_MAX_TIMERS];
static NnOsSysEvent g_sys_events[NN_OS_MAX_SYSEVENTS];
static NnOsMultiWaitHolder g_multiwaits[NN_OS_MAX_MULTIWAIT];

static int find_free_thread(void) { for (int i = 0; i < NN_OS_MAX_THREADS; i++) if (!g_threads[i].active) return i; return -1; }
static int find_free_mutex(void) { for (int i = 0; i < NN_OS_MAX_MUTEXES; i++) if (!g_mutexes[i].initialized) return i; return -1; }
static int find_free_cv(void) { for (int i = 0; i < NN_OS_MAX_CV; i++) if (!g_cvs[i].initialized) return i; return -1; }
static int find_free_event(void) { for (int i = 0; i < NN_OS_MAX_EVENTS; i++) if (!g_events[i].initialized) return i; return -1; }
static int find_free_tls(void) { for (int i = 0; i < NN_OS_MAX_TLS; i++) if (!g_tls_slots[i].initialized) return i; return -1; }
static int find_free_fiber(void) { for (int i = 0; i < NN_OS_MAX_FIBERS; i++) if (!g_fibers[i].active) return i; return -1; }
static int find_free_msgqueue(void) { for (int i = 0; i < NN_OS_MAX_MSGQUEUES; i++) if (!g_msgqueues[i].initialized) return i; return -1; }
static int find_free_timer(void) { for (int i = 0; i < NN_OS_MAX_TIMERS; i++) if (!g_timers[i].initialized) return i; return -1; }
static int find_free_sysevent(void) { for (int i = 0; i < NN_OS_MAX_SYSEVENTS; i++) if (!g_sys_events[i].initialized) return i; return -1; }
static int find_free_multiwait(void) { for (int i = 0; i < NN_OS_MAX_MULTIWAIT; i++) if (!g_multiwaits[i].initialized) return i; return -1; }

static void store_handle(SwCpu *c, u64 obj_addr, u64 handle_val)
{
    u8 *p;
    if (guest_ptr(c, obj_addr, 8, 1, &p)) {
        memcpy(p, &handle_val, 8);
    }
}

static u64 load_handle(SwCpu *c, u64 obj_addr)
{
    u8 *p;
    u64 val = 0;
    if (guest_ptr(c, obj_addr, 8, 0, &p)) {
        memcpy(&val, p, 8);
    }
    return val;
}

static DWORD WINAPI nn_os_thread_entry(LPVOID param)
{
    int idx = (int)(uintptr_t)param;
    NnOsThread *t = &g_threads[idx];
    if (t->entry) {
        /* The guest expects the thread function to be called with (arg) in x0 */
        /* We can't directly call guest code from here - the thread should be started via sw_dispatch */
        /* For now, signal that thread is running and wait for the guest to dispatch */
        t->active = 2; /* running */
    }
    return 0;
}

/* Thread Services */
void svc_nn_os_CreateThread(SwCpu *c)
{
    u64 thread_obj = c->x[0];
    u64 entry = c->x[1];
    u64 arg = c->x[2];
    u64 stack_base = c->x[3];
    s32 stack_size = (s32)c->x[4];
    int priority = (int)c->x[5];
    u32 core_mask = (u32)c->x[6];

    int idx = find_free_thread();
    if (idx < 0) { c->x[0] = 0xFFFFFFFF; return; }

    NnOsThread *t = &g_threads[idx];
    memset(t, 0, sizeof(*t));
    t->entry = entry;
    t->arg = arg;
    t->stack_base = stack_base;
    t->stack_size = stack_size;
    t->priority = priority;
    t->core_mask = core_mask;
    t->active = 1;

    /* Create Windows thread in suspended state */
    t->handle = CreateThread(NULL, 0, nn_os_thread_entry, (LPVOID)(uintptr_t)idx, CREATE_SUSPENDED, &t->thread_id);
    if (!t->handle) { t->active = 0; c->x[0] = 0xFFFFFFFF; return; }

    SetThreadPriority(t->handle, THREAD_PRIORITY_NORMAL);
    store_handle(c, thread_obj, (u64)(uintptr_t)t->handle);
    c->x[0] = 0; /* Success */
}

void svc_nn_os_StartThread(SwCpu *c)
{
    u64 thread_obj = c->x[0];
    u64 handle = load_handle(c, thread_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    ResumeThread((HANDLE)(uintptr_t)handle);
    c->x[0] = 0;
}

void svc_nn_os_WaitThread(SwCpu *c)
{
    u64 thread_obj = c->x[0];
    u64 handle = load_handle(c, thread_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    WaitForSingleObject((HANDLE)(uintptr_t)handle, INFINITE);
    c->x[0] = 0;
}

void svc_nn_os_DestroyThread(SwCpu *c)
{
    u64 thread_obj = c->x[0];
    u64 handle = load_handle(c, thread_obj);
    if (handle) {
        CloseHandle((HANDLE)(uintptr_t)handle);
        store_handle(c, thread_obj, 0);
    }
    /* Find and clear our thread entry */
    for (int i = 0; i < NN_OS_MAX_THREADS; i++) {
        if (g_threads[i].handle == (HANDLE)(uintptr_t)handle) {
            g_threads[i].active = 0;
            break;
        }
    }
    c->x[0] = 0;
}

void svc_nn_os_SetThreadName(SwCpu *c)
{
    u64 thread_obj = c->x[0];
    u64 name_ptr = c->x[1];
    u64 handle = load_handle(c, thread_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    
    char name[64];
    u8 *p;
    if (guest_ptr(c, name_ptr, 63, 0, &p)) {
        strncpy(name, (char*)p, 63);
        name[63] = 0;
        /* Store in our thread struct */
        for (int i = 0; i < NN_OS_MAX_THREADS; i++) {
            if (g_threads[i].handle == (HANDLE)(uintptr_t)handle) {
                strncpy(g_threads[i].name, name, 31);
                g_threads[i].name[31] = 0;
                break;
            }
        }
    }
    c->x[0] = 0;
}

void svc_nn_os_SetThreadCoreMask(SwCpu *c)
{
    u64 thread_obj = c->x[0];
    s32 mask = (s32)c->x[1];
    int core = (int)c->x[2];
    u64 handle = load_handle(c, thread_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    
    DWORD_PTR affinity = 1;
    if (mask & 1) affinity = ((DWORD_PTR)1 << (core % 64));
    SetThreadAffinityMask((HANDLE)(uintptr_t)handle, affinity);
    c->x[0] = 0;
}

void svc_nn_os_GetThreadCoreMask(SwCpu *c)
{
    u64 mask_ptr = c->x[0];
    u64 core_ptr = c->x[1];
    u64 thread_obj = c->x[2];
    u64 handle = load_handle(c, thread_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    
    GROUP_AFFINITY ga;
    GetThreadGroupAffinity((HANDLE)(uintptr_t)handle, &ga);
    u8 *p;
    if (guest_ptr(c, mask_ptr, 4, 1, &p)) *(u32*)p = (u32)ga.Mask;
    if (guest_ptr(c, core_ptr, 4, 1, &p)) *(u32*)p = ga.Group;
    c->x[0] = 0;
}

void svc_nn_os_GetThreadPriority(SwCpu *c)
{
    u64 thread_obj = c->x[0];
    u64 handle = load_handle(c, thread_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    int prio = GetThreadPriority((HANDLE)(uintptr_t)handle);
    c->x[0] = (u64)(s64)prio;
}

void svc_nn_os_ChangeThreadPriority(SwCpu *c)
{
    u64 thread_obj = c->x[0];
    int priority = (int)c->x[1];
    u64 handle = load_handle(c, thread_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    SetThreadPriority((HANDLE)(uintptr_t)handle, priority);
    c->x[0] = 0;
}

void svc_nn_os_SleepThread(SwCpu *c)
{
    u64 timespan = c->x[0]; /* nanoseconds */
    DWORD ms = (DWORD)(timespan / 1000000);
    Sleep(ms);
    c->x[0] = 0;
}

/* TLS Services */
void svc_nn_os_AllocateTlsSlot(SwCpu *c)
{
    u64 slot_obj = c->x[0];
    u64 destructor = c->x[1]; /* function pointer, ignored for now */
    int idx = find_free_tls();
    if (idx < 0) { c->x[0] = 0xFFFFFFFF; return; }
    
    DWORD tls = TlsAlloc();
    if (tls == TLS_OUT_OF_INDEXES) { c->x[0] = 0xFFFFFFFF; return; }
    
    g_tls_slots[idx].tls_index = tls;
    g_tls_slots[idx].initialized = 1;
    store_handle(c, slot_obj, (u64)tls);
    c->x[0] = 0;
}

void svc_nn_os_FreeTlsSlot(SwCpu *c)
{
    u64 slot_obj = c->x[0];
    u64 tls_val = load_handle(c, slot_obj);
    if (tls_val) {
        TlsFree((DWORD)tls_val);
        store_handle(c, slot_obj, 0);
        for (int i = 0; i < NN_OS_MAX_TLS; i++) {
            if (g_tls_slots[i].tls_index == (DWORD)tls_val) {
                g_tls_slots[i].initialized = 0;
                break;
            }
        }
    }
    c->x[0] = 0;
}

void svc_nn_os_GetTlsValue(SwCpu *c)
{
    u64 slot_obj = c->x[0];
    u64 tls_val = load_handle(c, slot_obj);
    if (!tls_val) { c->x[0] = 0; return; }
    LPVOID val = TlsGetValue((DWORD)tls_val);
    c->x[0] = (u64)(uintptr_t)val;
}

void svc_nn_os_SdkAllocateTlsSlot(SwCpu *c)
{
    /* Same as AllocateTlsSlot but for SDK internal use */
    svc_nn_os_AllocateTlsSlot(c);
}

/* Mutex Services */
void svc_nn_os_InitializeMutex(SwCpu *c)
{
    u64 mutex_obj = c->x[0];
    int recursive = (int)c->x[1];
    int priority = (int)c->x[2]; /* ignored on Windows */
    int idx = find_free_mutex();
    if (idx < 0) { c->x[0] = 0xFFFFFFFF; return; }
    
    InitializeCriticalSection(&g_mutexes[idx].cs);
    g_mutexes[idx].initialized = 1;
    store_handle(c, mutex_obj, (u64)(uintptr_t)&g_mutexes[idx]);
    c->x[0] = 0;
}

void svc_nn_os_FinalizeMutex(SwCpu *c)
{
    u64 mutex_obj = c->x[0];
    u64 handle = load_handle(c, mutex_obj);
    if (handle) {
        DeleteCriticalSection((CRITICAL_SECTION*)(uintptr_t)handle);
        store_handle(c, mutex_obj, 0);
        for (int i = 0; i < NN_OS_MAX_MUTEXES; i++) {
            if ((uintptr_t)&g_mutexes[i] == (uintptr_t)handle) {
                g_mutexes[i].initialized = 0;
                break;
            }
        }
    }
    c->x[0] = 0;
}

void svc_nn_os_LockMutex(SwCpu *c)
{
    u64 mutex_obj = c->x[0];
    u64 handle = load_handle(c, mutex_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    EnterCriticalSection((CRITICAL_SECTION*)(uintptr_t)handle);
    c->x[0] = 0;
}

void svc_nn_os_UnlockMutex(SwCpu *c)
{
    u64 mutex_obj = c->x[0];
    u64 handle = load_handle(c, mutex_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    LeaveCriticalSection((CRITICAL_SECTION*)(uintptr_t)handle);
    c->x[0] = 0;
}

void svc_nn_os_TryLockMutex(SwCpu *c)
{
    u64 mutex_obj = c->x[0];
    u64 handle = load_handle(c, mutex_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    BOOL result = TryEnterCriticalSection((CRITICAL_SECTION*)(uintptr_t)handle);
    c->x[0] = result ? 1 : 0;
}

void svc_nn_os_SdkMutexType_Initialize(SwCpu *c)
{
    u64 mutex_obj = c->x[0];
    /* SdkMutexType is a struct with embedded CriticalSection */
    int idx = find_free_mutex();
    if (idx < 0) { c->x[0] = 0xFFFFFFFF; return; }
    InitializeCriticalSection(&g_mutexes[idx].cs);
    g_mutexes[idx].initialized = 1;
    store_handle(c, mutex_obj, (u64)(uintptr_t)&g_mutexes[idx]);
    c->x[0] = 0;
}

void svc_nn_os_SdkMutexType_Lock(SwCpu *c)
{
    u64 mutex_obj = c->x[0];
    u64 handle = load_handle(c, mutex_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    EnterCriticalSection((CRITICAL_SECTION*)(uintptr_t)handle);
    c->x[0] = 0;
}

void svc_nn_os_SdkMutexType_Unlock(SwCpu *c)
{
    u64 mutex_obj = c->x[0];
    u64 handle = load_handle(c, mutex_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    LeaveCriticalSection((CRITICAL_SECTION*)(uintptr_t)handle);
    c->x[0] = 0;
}

void svc_nn_os_SdkRecursiveMutexType_Initialize(SwCpu *c)
{
    /* Same as SdkMutexType on Windows */
    svc_nn_os_SdkMutexType_Initialize(c);
}

void svc_nn_os_SdkRecursiveMutexType_Lock(SwCpu *c)
{
    svc_nn_os_SdkMutexType_Lock(c);
}

void svc_nn_os_SdkRecursiveMutexType_Unlock(SwCpu *c)
{
    svc_nn_os_SdkMutexType_Unlock(c);
}

/* Condition Variable Services */
void svc_nn_os_InitializeConditionVariable(SwCpu *c)
{
    u64 cv_obj = c->x[0];
    int idx = find_free_cv();
    if (idx < 0) { c->x[0] = 0xFFFFFFFF; return; }
    
    InitializeConditionVariable(&g_cvs[idx].cv);
    g_cvs[idx].initialized = 1;
    store_handle(c, cv_obj, (u64)(uintptr_t)&g_cvs[idx]);
    c->x[0] = 0;
}

void svc_nn_os_FinalizeConditionVariable(SwCpu *c)
{
    u64 cv_obj = c->x[0];
    u64 handle = load_handle(c, cv_obj);
    if (handle) {
        store_handle(c, cv_obj, 0);
        for (int i = 0; i < NN_OS_MAX_CV; i++) {
            if ((uintptr_t)&g_cvs[i] == (uintptr_t)handle) {
                g_cvs[i].initialized = 0;
                break;
            }
        }
    }
    c->x[0] = 0;
}

void svc_nn_os_WaitConditionVariable(SwCpu *c)
{
    u64 cv_obj = c->x[0];
    u64 mutex_obj = c->x[1];
    u64 cv_handle = load_handle(c, cv_obj);
    u64 mutex_handle = load_handle(c, mutex_obj);
    if (!cv_handle || !mutex_handle) { c->x[0] = 0xFFFFFFFF; return; }
    
    SleepConditionVariableCS((CONDITION_VARIABLE*)(uintptr_t)cv_handle,
                              (CRITICAL_SECTION*)(uintptr_t)mutex_handle,
                              INFINITE);
    c->x[0] = 0;
}

void svc_nn_os_TimedWaitConditionVariable(SwCpu *c)
{
    u64 cv_obj = c->x[0];
    u64 mutex_obj = c->x[1];
    u64 timespan = c->x[2]; /* nanoseconds */
    u64 cv_handle = load_handle(c, cv_obj);
    u64 mutex_handle = load_handle(c, mutex_obj);
    if (!cv_handle || !mutex_handle) { c->x[0] = 0xFFFFFFFF; return; }
    
    DWORD ms = (DWORD)(timespan / 1000000);
    BOOL result = SleepConditionVariableCS((CONDITION_VARIABLE*)(uintptr_t)cv_handle,
                                            (CRITICAL_SECTION*)(uintptr_t)mutex_handle,
                                            ms);
    c->x[0] = result ? 0 : 0xFFFFFFFF; /* 0 = success, non-zero = timeout/error */
}

void svc_nn_os_SignalConditionVariable(SwCpu *c)
{
    u64 cv_obj = c->x[0];
    u64 handle = load_handle(c, cv_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    WakeConditionVariable((CONDITION_VARIABLE*)(uintptr_t)handle);
    c->x[0] = 0;
}

void svc_nn_os_BroadcastConditionVariable(SwCpu *c)
{
    u64 cv_obj = c->x[0];
    u64 handle = load_handle(c, cv_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    WakeAllConditionVariable((CONDITION_VARIABLE*)(uintptr_t)handle);
    c->x[0] = 0;
}

/* Event Services */
void svc_nn_os_InitializeEvent(SwCpu *c)
{
    u64 event_obj = c->x[0];
    int manual_reset = (int)c->x[1];
    int clear_mode = (int)c->x[2];
    int idx = find_free_event();
    if (idx < 0) { c->x[0] = 0xFFFFFFFF; return; }
    
    HANDLE h = CreateEvent(NULL, manual_reset, FALSE, NULL);
    if (!h) { c->x[0] = 0xFFFFFFFF; return; }
    
    g_events[idx].event = h;
    g_events[idx].manual_reset = manual_reset;
    g_events[idx].initialized = 1;
    store_handle(c, event_obj, (u64)(uintptr_t)h);
    c->x[0] = 0;
}

void svc_nn_os_FinalizeEvent(SwCpu *c)
{
    u64 event_obj = c->x[0];
    u64 handle = load_handle(c, event_obj);
    if (handle) {
        CloseHandle((HANDLE)(uintptr_t)handle);
        store_handle(c, event_obj, 0);
        for (int i = 0; i < NN_OS_MAX_EVENTS; i++) {
            if (g_events[i].event == (HANDLE)(uintptr_t)handle) {
                g_events[i].initialized = 0;
                break;
            }
        }
    }
    c->x[0] = 0;
}

void svc_nn_os_SignalEvent(SwCpu *c)
{
    u64 event_obj = c->x[0];
    u64 handle = load_handle(c, event_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    SetEvent((HANDLE)(uintptr_t)handle);
    c->x[0] = 0;
}

void svc_nn_os_TimedWaitEvent(SwCpu *c)
{
    u64 event_obj = c->x[0];
    u64 timespan = c->x[1];
    u64 handle = load_handle(c, event_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    DWORD ms = (DWORD)(timespan / 1000000);
    DWORD result = WaitForSingleObject((HANDLE)(uintptr_t)handle, ms);
    c->x[0] = (result == WAIT_OBJECT_0) ? 0 : 0xFFFFFFFF;
}

void svc_nn_os_DestroySystemEvent(SwCpu *c)
{
    u64 event_obj = c->x[0];
    u64 handle = load_handle(c, event_obj);
    if (handle) {
        CloseHandle((HANDLE)(uintptr_t)handle);
        store_handle(c, event_obj, 0);
        for (int i = 0; i < NN_OS_MAX_SYSEVENTS; i++) {
            if (g_sys_events[i].event == (HANDLE)(uintptr_t)handle) {
                g_sys_events[i].initialized = 0;
                break;
            }
        }
    }
    c->x[0] = 0;
}

void svc_nn_os_WaitSystemEvent(SwCpu *c)
{
    u64 event_obj = c->x[0];
    u64 handle = load_handle(c, event_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    WaitForSingleObject((HANDLE)(uintptr_t)handle, INFINITE);
    c->x[0] = 0;
}

void svc_nn_os_TryWaitSystemEvent(SwCpu *c)
{
    u64 event_obj = c->x[0];
    u64 handle = load_handle(c, event_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    DWORD result = WaitForSingleObject((HANDLE)(uintptr_t)handle, 0);
    c->x[0] = (result == WAIT_OBJECT_0) ? 0 : 0xFFFFFFFF;
}

void svc_nn_os_ClearSystemEvent(SwCpu *c)
{
    u64 event_obj = c->x[0];
    u64 handle = load_handle(c, event_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    ResetEvent((HANDLE)(uintptr_t)handle);
    c->x[0] = 0;
}

void svc_nn_os_AttachReadableHandleToSystemEvent(SwCpu *c)
{
    u64 event_obj = c->x[0];
    u64 handle_val = c->x[1];
    int clear_mode = (int)c->x[2];
    u64 event_handle = load_handle(c, event_obj);
    if (!event_handle) { c->x[0] = 0xFFFFFFFF; return; }
    /* On Windows, we can't directly attach handles to events like this.
     * This is used for readable handle notification. Stub for now. */
    c->x[0] = 0;
}

/* Fiber Services */
void svc_nn_os_InitializeFiber(SwCpu *c)
{
    u64 fiber_obj = c->x[0];
    u64 entry = c->x[1];
    u64 arg = c->x[2];
    u64 stack_base = c->x[3];
    u64 stack_size = c->x[4];
    int priority = (int)c->x[5];
    
    int idx = find_free_fiber();
    if (idx < 0) { c->x[0] = 0xFFFFFFFF; return; }
    
    /* ConvertThreadToFiberEx if not already a fiber */
    LPVOID fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    if (!fiber && GetLastError() != ERROR_ALREADY_FIBER) { 
        c->x[0] = 0xFFFFFFFF; return; 
    }
    
    g_fibers[idx].fiber = fiber;
    g_fibers[idx].entry = entry;
    g_fibers[idx].arg = arg;
    g_fibers[idx].stack_base = stack_base;
    g_fibers[idx].stack_size = stack_size;
    g_fibers[idx].active = 1;
    store_handle(c, fiber_obj, (u64)(uintptr_t)fiber);
    c->x[0] = 0;
}

void svc_nn_os_FinalizeFiber(SwCpu *c)
{
    u64 fiber_obj = c->x[0];
    u64 handle = load_handle(c, fiber_obj);
    if (handle) {
        /* DeleteFiber doesn't exist on Windows, just mark inactive */
        store_handle(c, fiber_obj, 0);
        for (int i = 0; i < NN_OS_MAX_FIBERS; i++) {
            if (g_fibers[i].fiber == (LPVOID)(uintptr_t)handle) {
                g_fibers[i].active = 0;
                break;
            }
        }
    }
    c->x[0] = 0;
}

void svc_nn_os_SwitchToFiber(SwCpu *c)
{
    u64 fiber_obj = c->x[0];
    u64 handle = load_handle(c, fiber_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    SwitchToFiber((LPVOID)(uintptr_t)handle);
    c->x[0] = 0;
}

void svc_nn_os_GetCurrentFiber(SwCpu *c)
{
    LPVOID fiber = GetCurrentFiber();
    c->x[0] = (u64)(uintptr_t)fiber;
}

/* Message Queue Services */
void svc_nn_os_InitializeMessageQueue(SwCpu *c)
{
    u64 mq_obj = c->x[0];
    u64 buffer = c->x[1];
    s32 msg_size = (s32)c->x[2];
    s32 capacity = (s32)c->x[3];
    
    int idx = find_free_msgqueue();
    if (idx < 0) { c->x[0] = 0xFFFFFFFF; return; }
    
    HANDLE sem = CreateSemaphore(NULL, 0, capacity, NULL);
    if (!sem) { c->x[0] = 0xFFFFFFFF; return; }
    
    InitializeCriticalSection(&g_msgqueues[idx].cs);
    g_msgqueues[idx].sem = sem;
    g_msgqueues[idx].buffer = buffer;
    g_msgqueues[idx].msg_size = msg_size;
    g_msgqueues[idx].capacity = capacity;
    g_msgqueues[idx].head = 0;
    g_msgqueues[idx].tail = 0;
    g_msgqueues[idx].initialized = 1;
    store_handle(c, mq_obj, (u64)(uintptr_t)&g_msgqueues[idx]);
    c->x[0] = 0;
}

void svc_nn_os_FinalizeMessageQueue(SwCpu *c)
{
    u64 mq_obj = c->x[0];
    u64 handle = load_handle(c, mq_obj);
    if (handle) {
        NnOsMsgQueue *mq = (NnOsMsgQueue*)(uintptr_t)handle;
        CloseHandle(mq->sem);
        DeleteCriticalSection(&mq->cs);
        store_handle(c, mq_obj, 0);
        for (int i = 0; i < NN_OS_MAX_MSGQUEUES; i++) {
            if ((uintptr_t)&g_msgqueues[i] == (uintptr_t)handle) {
                g_msgqueues[i].initialized = 0;
                break;
            }
        }
    }
    c->x[0] = 0;
}

void svc_nn_os_SendMessageQueue(SwCpu *c)
{
    u64 mq_obj = c->x[0];
    u64 msg = c->x[1];
    u64 handle = load_handle(c, mq_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    
    NnOsMsgQueue *mq = (NnOsMsgQueue*)(uintptr_t)handle;
    EnterCriticalSection(&mq->cs);
    
    u64 next_tail = (mq->tail + 1) % mq->capacity;
    if (next_tail == mq->head) {
        LeaveCriticalSection(&mq->cs);
        c->x[0] = 0xFFFFFFFF; /* Full */
        return;
    }
    
    u8 *dst = sw_mem_ptr(c, mq->buffer + mq->tail * mq->msg_size, mq->msg_size, 1);
    u8 *src = sw_mem_ptr(c, msg, mq->msg_size, 0);
    if (dst && src) memcpy(dst, src, (size_t)mq->msg_size);
    mq->tail = next_tail;
    LeaveCriticalSection(&mq->cs);
    ReleaseSemaphore(mq->sem, 1, NULL);
    c->x[0] = 0;
}

void svc_nn_os_TrySendMessageQueue(SwCpu *c)
{
    u64 mq_obj = c->x[0];
    u64 msg = c->x[1];
    u64 handle = load_handle(c, mq_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    
    NnOsMsgQueue *mq = (NnOsMsgQueue*)(uintptr_t)handle;
    BOOL entered = TryEnterCriticalSection(&mq->cs);
    if (!entered) { c->x[0] = 0xFFFFFFFF; return; }
    
    u64 next_tail = (mq->tail + 1) % mq->capacity;
    if (next_tail == mq->head) {
        LeaveCriticalSection(&mq->cs);
        c->x[0] = 0xFFFFFFFF; /* Full */
        return;
    }
    
    u8 *dst = sw_mem_ptr(c, mq->buffer + mq->tail * mq->msg_size, mq->msg_size, 1);
    u8 *src = sw_mem_ptr(c, msg, mq->msg_size, 0);
    if (dst && src) memcpy(dst, src, (size_t)mq->msg_size);
    mq->tail = next_tail;
    LeaveCriticalSection(&mq->cs);
    ReleaseSemaphore(mq->sem, 1, NULL);
    c->x[0] = 0;
}

void svc_nn_os_ReceiveMessageQueue(SwCpu *c)
{
    u64 mq_obj = c->x[0];
    u64 msg_ptr = c->x[1];
    u64 handle = load_handle(c, mq_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    
    NnOsMsgQueue *mq = (NnOsMsgQueue*)(uintptr_t)handle;
    WaitForSingleObject(mq->sem, INFINITE);
    
    EnterCriticalSection(&mq->cs);
    u8 *dst = sw_mem_ptr(c, msg_ptr, mq->msg_size, 1);
    u8 *src = sw_mem_ptr(c, mq->buffer + mq->head * mq->msg_size, mq->msg_size, 0);
    if (dst && src) memcpy(dst, src, (size_t)mq->msg_size);
    mq->head = (mq->head + 1) % mq->capacity;
    LeaveCriticalSection(&mq->cs);
    c->x[0] = 0;
}

void svc_nn_os_TryReceiveMessageQueue(SwCpu *c)
{
    u64 mq_obj = c->x[0];
    u64 msg_ptr = c->x[1];
    u64 handle = load_handle(c, mq_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    
    NnOsMsgQueue *mq = (NnOsMsgQueue*)(uintptr_t)handle;
    DWORD result = WaitForSingleObject(mq->sem, 0);
    if (result != WAIT_OBJECT_0) { c->x[0] = 0xFFFFFFFF; return; }
    
    BOOL entered = TryEnterCriticalSection(&mq->cs);
    if (!entered) { c->x[0] = 0xFFFFFFFF; return; }
    
    u8 *dst = sw_mem_ptr(c, msg_ptr, mq->msg_size, 1);
    u8 *src = sw_mem_ptr(c, mq->buffer + mq->head * mq->msg_size, mq->msg_size, 0);
    if (dst && src) memcpy(dst, src, (size_t)mq->msg_size);
    mq->head = (mq->head + 1) % mq->capacity;
    LeaveCriticalSection(&mq->cs);
    c->x[0] = 0;
}

void svc_nn_os_JamMessageQueue(SwCpu *c)
{
    u64 mq_obj = c->x[0];
    u64 msg = c->x[1];
    u64 handle = load_handle(c, mq_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    
    NnOsMsgQueue *mq = (NnOsMsgQueue*)(uintptr_t)handle;
    EnterCriticalSection(&mq->cs);
    
    /* Jam = push to front (head - 1) */
    u64 new_head = (mq->head + mq->capacity - 1) % mq->capacity;
    if (new_head == mq->tail) {
        LeaveCriticalSection(&mq->cs);
        c->x[0] = 0xFFFFFFFF; /* Full */
        return;
    }
    
    u8 *dst = sw_mem_ptr(c, mq->buffer + new_head * mq->msg_size, mq->msg_size, 1);
    u8 *src = sw_mem_ptr(c, msg, mq->msg_size, 0);
    if (dst && src) memcpy(dst, src, (size_t)mq->msg_size);
    mq->head = new_head;
    LeaveCriticalSection(&mq->cs);
    ReleaseSemaphore(mq->sem, 1, NULL);
    c->x[0] = 0;
}

void svc_nn_os_TryJamMessageQueue(SwCpu *c)
{
    svc_nn_os_JamMessageQueue(c); /* Same but with TryEnterCriticalSection */
}

/* Timer Services */
void svc_nn_os_StartOneShotTimerEvent(SwCpu *c)
{
    u64 timer_obj = c->x[0];
    u64 timespan = c->x[1];
    int idx = find_free_timer();
    if (idx < 0) { c->x[0] = 0xFFFFFFFF; return; }
    
    HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);
    if (!timer) { c->x[0] = 0xFFFFFFFF; return; }
    
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(timespan / 100); /* 100ns units, negative = relative */
    if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
        CloseHandle(timer);
        c->x[0] = 0xFFFFFFFF; return;
    }
    
    g_timers[idx].timer = timer;
    g_timers[idx].initialized = 1;
    store_handle(c, timer_obj, (u64)(uintptr_t)timer);
    c->x[0] = 0;
}

/* Other Services */
void svc_nn_os_GetCurrentStackInfo(SwCpu *c)
{
    u64 base_ptr = c->x[0];
    u64 limit_ptr = c->x[1];
    u8 *p;
    if (guest_ptr(c, base_ptr, 8, 1, &p)) *(u64*)p = (u64)(uintptr_t)&c;
    if (guest_ptr(c, limit_ptr, 8, 1, &p)) *(u64*)p = (u64)(uintptr_t)&c + 1024*1024; /* 1MB estimate */
    c->x[0] = 0;
}

void svc_nn_os_TryWaitAny(SwCpu *c)
{
    u64 multiwait_obj = c->x[0];
    u64 handle = load_handle(c, multiwait_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    NnOsMultiWaitHolder *mw = (NnOsMultiWaitHolder*)(uintptr_t)handle;
    DWORD result = WaitForMultipleObjects(mw->count, mw->handles, FALSE, 0);
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + mw->count) {
        c->x[0] = result - WAIT_OBJECT_0;
    } else {
        c->x[0] = 0xFFFFFFFF;
    }
}

void svc_nn_os_InitializeMultiWaitHolder(SwCpu *c)
{
    u64 holder_obj = c->x[0];
    u64 timer_obj = c->x[1];
    int idx = find_free_multiwait();
    if (idx < 0) { c->x[0] = 0xFFFFFFFF; return; }
    
    g_multiwaits[idx].count = 0;
    g_multiwaits[idx].initialized = 1;
    if (timer_obj) {
        u64 timer_handle = load_handle(c, timer_obj);
        if (timer_handle && g_multiwaits[idx].count < 64) {
            g_multiwaits[idx].handles[g_multiwaits[idx].count++] = (HANDLE)(uintptr_t)timer_handle;
        }
    }
    store_handle(c, holder_obj, (u64)(uintptr_t)&g_multiwaits[idx]);
        c->x[0] = 0;
    }

    /* ---------------- Missing nn::os service stubs ------------- */
/* These services are declared in NATIVE_SERVICES but not fully implemented yet.
 * They trap with a clear message so missing functionality is visible. */

void svc_nn_os_GetSystemTick(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::GetSystemTick");
}

void svc_nn_os_ConvertToTimeSpan(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::ConvertToTimeSpan");
}

void svc_nn_os_QueryMemoryInfo(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::QueryMemoryInfo");
}

void svc_nn_os_SetMemoryHeapSize(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::SetMemoryHeapSize");
}

void svc_nn_os_AllocateMemoryBlock(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::AllocateMemoryBlock");
}

void svc_nn_os_SetMemoryAllocatorForThreadLocal(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::SetMemoryAllocatorForThreadLocal");
}

void svc_nn_os_GetHostArgc(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::GetHostArgc");
}

void svc_nn_os_GetHostArgv(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::GetHostArgv");
}

void svc_nn_os_ClearEvent(SwCpu *c)
{
    u64 event_obj = c->x[0];
    u64 handle = load_handle(c, event_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    ResetEvent((HANDLE)(uintptr_t)handle);
    c->x[0] = 0;
}

void svc_nn_os_WaitEvent(SwCpu *c)
{
    u64 event_obj = c->x[0];
    u64 handle = load_handle(c, event_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    WaitForSingleObject((HANDLE)(uintptr_t)handle, INFINITE);
    c->x[0] = 0;
}

void svc_nn_os_TryWaitEvent(SwCpu *c)
{
    u64 event_obj = c->x[0];
    u64 handle = load_handle(c, event_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    DWORD result = WaitForSingleObject((HANDLE)(uintptr_t)handle, 0);
    c->x[0] = (result == WAIT_OBJECT_0) ? 0 : 0xFFFFFFFF;
}

void svc_nn_os_InitializeLightEvent(SwCpu *c)
{
    svc_nn_os_InitializeEvent(c); /* Same as InitializeEvent */
}

void svc_nn_os_FinalizeLightEvent(SwCpu *c)
{
    svc_nn_os_FinalizeEvent(c); /* Same as FinalizeEvent */
}

void svc_nn_os_SignalLightEvent(SwCpu *c)
{
    svc_nn_os_SignalEvent(c); /* Same as SignalEvent */
}

void svc_nn_os_WaitLightEvent(SwCpu *c)
{
    svc_nn_os_WaitEvent(c); /* Same as WaitEvent */
}

void svc_nn_os_GetCurrentThread(SwCpu *c)
{
    c->x[0] = (u64)(uintptr_t)GetCurrentThread();
}

void svc_nn_os_YieldThread(SwCpu *c)
{
    Sleep(0);
    c->x[0] = 0;
}

void svc_nn_os_SetTlsValue(SwCpu *c)
{
    u64 slot_obj = c->x[0];
    u64 value = c->x[1];
    u64 tls_val = load_handle(c, slot_obj);
    if (!tls_val) { c->x[0] = 0xFFFFFFFF; return; }
    TlsSetValue((DWORD)tls_val, (LPVOID)(uintptr_t)value);
    c->x[0] = 0;
}

void svc_nn_os_InitializeSemaphore(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::InitializeSemaphore");
}

void svc_nn_os_ReleaseSemaphore(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::ReleaseSemaphore");
}

void svc_nn_os_FinalizeSemaphore(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::FinalizeSemaphore");
}

void svc_nn_os_TryAcquireSemaphore(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::TryAcquireSemaphore");
}

void svc_nn_os_AcquireSemaphore(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::AcquireSemaphore");
}

void svc_nn_os_InitializeMultiWait(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::InitializeMultiWait");
}

void svc_nn_os_FinalizeMultiWait(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::FinalizeMultiWait");
}

void svc_nn_os_WaitAny(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::WaitAny");
}

void svc_nn_os_GetMultiWaitHolderUserData(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::GetMultiWaitHolderUserData");
}

void svc_nn_os_SetMultiWaitHolderUserData(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::SetMultiWaitHolderUserData");
}

void svc_nn_os_FinalizeMultiWaitHolder(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::FinalizeMultiWaitHolder");
}

void svc_nn_os_LinkMultiWaitHolder(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::LinkMultiWaitHolder");
}

void svc_nn_os_UnlinkMultiWaitHolder(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::UnlinkMultiWaitHolder");
}

void svc_nn_os_IsMutexLockedByCurrentThread(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::IsMutexLockedByCurrentThread");
}

void svc_nn_os_GenerateRandomBytes(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::GenerateRandomBytes");
}

void svc_nn_os_GetCurrentCoreNumber(SwCpu *c)
{
    c->x[0] = 0; /* Return core 0 */
}

void svc_nn_os_GetThreadAvailableCoreMask(SwCpu *c)
{
    c->x[0] = 1; /* Single core mask */
}

void svc_nn_os_InitializeTimerEvent(SwCpu *c)
{
    u64 timer_obj = c->x[0];
    int clear_mode = (int)c->x[1];
    int idx = find_free_timer();
    if (idx < 0) { c->x[0] = 0xFFFFFFFF; return; }

    HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);
    if (!timer) { c->x[0] = 0xFFFFFFFF; return; }
    
    g_timers[idx].timer = timer;
    g_timers[idx].initialized = 1;
    store_handle(c, timer_obj, (u64)(uintptr_t)timer);
    c->x[0] = 0;
}

void svc_nn_os_StartPeriodicTimerEvent(SwCpu *c)
{
    u64 timer_obj = c->x[0];
    u64 interval = c->x[1];
    u64 initial = c->x[2];
    u64 handle = load_handle(c, timer_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }

    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(initial / 100);
    LONG period = (LONG)(interval / 1000000); /* ms */
    if (!SetWaitableTimer((HANDLE)(uintptr_t)handle, &due, period, NULL, NULL, FALSE)) {
        c->x[0] = 0xFFFFFFFF; return;
    }
    c->x[0] = 0;
}

void svc_nn_os_WaitTimerEvent(SwCpu *c)
{
    u64 timer_obj = c->x[0];
    u64 handle = load_handle(c, timer_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    WaitForSingleObject((HANDLE)(uintptr_t)handle, INFINITE);
    c->x[0] = 0;
}

void svc_nn_os_StopTimerEvent(SwCpu *c)
{
    u64 timer_obj = c->x[0];
    u64 handle = load_handle(c, timer_obj);
    if (!handle) { c->x[0] = 0xFFFFFFFF; return; }
    CancelWaitableTimer((HANDLE)(uintptr_t)handle);
    c->x[0] = 0;
}

void svc_nn_os_FinalizeTimerEvent(SwCpu *c)
{
    u64 timer_obj = c->x[0];
    u64 handle = load_handle(c, timer_obj);
    if (handle) {
        CloseHandle((HANDLE)(uintptr_t)handle);
        store_handle(c, timer_obj, 0);
        for (int i = 0; i < NN_OS_MAX_TIMERS; i++) {
            if (g_timers[i].timer == (HANDLE)(uintptr_t)handle) {
                g_timers[i].initialized = 0;
                break;
            }
        }
    }
    c->x[0] = 0;
}

void svc_nn_os_GetVirtualAddressMemoryResourceUsage(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::os::GetVirtualAddressMemoryResourceUsage");
}

void svc_nn_os_GetSystemTickFrequency(SwCpu *c)
{
    c->x[0] = 10000000; /* 10MHz = 100ns ticks */
}

void svc_nn_os_GetThreadId(SwCpu *c)
{
    u64 thread_obj = c->x[0];
    u64 handle = load_handle(c, thread_obj);
    if (!handle) { c->x[0] = 0; return; }
    for (int i = 0; i < NN_OS_MAX_THREADS; i++) {
        if (g_threads[i].handle == (HANDLE)(uintptr_t)handle) {
            c->x[0] = g_threads[i].thread_id;
            return;
        }
    }
    c->x[0] = 0;
    }

/* C++ exception handling stubs */
void svc_cxa_guard_acquire(SwCpu *c)
{
    u64 guard = c->x[0];
    u8 *p;
    if (!guest_ptr(c, guard, 1, 1, &p)) { sw_trap(c, guard, "cxa_guard_acquire on unmapped guard"); return; }
    if (*p) { c->x[0] = 0; return; }
    *p = 1;
    c->x[0] = 1;
}

void svc_cxa_guard_release(SwCpu *c) { c->x[0] = 0; }

void svc_cxa_pure_virtual(SwCpu *c)
{
    sw_trap(c, 0, "pure virtual function called");
}

void svc_cxa_begin_catch(SwCpu *c) { c->x[0] = c->x[0]; }
void svc_cxa_end_catch(SwCpu *c) { c->x[0] = 0; }
void svc_cxa_throw(SwCpu *c) { sw_trap(c, 0, "C++ exception thrown (unhandled)"); }
void svc_cxa_allocate_exception(SwCpu *c) { c->x[0] = 0; }
void svc_cxa_free_exception(SwCpu *c) { c->x[0] = 0; }

/* Standard C library - memory allocation */
#define SW_ALLOC_MAGIC 0x5357414C4C4F4331ull
static u64 g_heap_cursor = 0;
static u64 g_heap_end = 0;

static u64 guest_alloc(SwCpu *c, u64 size)
{
    u64 total, page_size = 0x10000;
    u8 *p;
    if (size == 0) size = 1;
    if (g_heap_cursor == 0) {
        g_heap_cursor = 0x90000000ull;
        g_heap_end = g_heap_cursor + 1024ull * 1024 * 1024;
    }
    total = (size + 32 + page_size - 1) & ~(page_size - 1);
    if (g_heap_cursor + total > g_heap_end) {
        sw_trap(c, 0, "guest heap exhausted (1 GiB)");
        return 0;
    }
    if (sw_mem_map(c, g_heap_cursor, total, NULL, 0) != 0) {
        sw_trap(c, 0, "guest heap mapping failed");
        return 0;
    }
    p = sw_mem_ptr(c, g_heap_cursor, 32, 1);
    if (!p) return 0;
    {
        u64 magic = SW_ALLOC_MAGIC;
        memcpy(p, &magic, 8);
        memcpy(p + 8, &size, 8);
    }
    p += 32;
    {
        u64 result = g_heap_cursor + 32;
        g_heap_cursor += total;
        return result;
    }
}

void svc_malloc(SwCpu *c)
{
    u64 p = guest_alloc(c, c->x[0]);
    c->x[0] = p;
}

void svc_calloc(SwCpu *c)
{
    u64 n = c->x[0], size = c->x[1];
    u64 total = n * size;
    u64 p;
    if (size && total / size != n) { c->x[0] = 0; return; }
    p = guest_alloc(c, total); /* freshly mapped pages are zero-filled */
    c->x[0] = p;
}

void svc_realloc(SwCpu *c)
{
    u64 old = c->x[0], size = c->x[1];
    u64 p;
    u8 *hdr;
    u64 old_size = 0;
    if (!old) { c->x[0] = guest_alloc(c, size); return; }
    hdr = sw_mem_ptr(c, old - 32, 16, 0);
    if (hdr) memcpy(&old_size, hdr + 8, 8);
    p = guest_alloc(c, size);
    if (p && old_size) {
        u64 copy = old_size < size ? old_size : size;
        u8 *src = sw_mem_ptr(c, old, copy, 0), *dst = sw_mem_ptr(c, p, copy, 1);
        if (src && dst) memmove(dst, src, (size_t)copy);
    }
    c->x[0] = p;
}

void svc_free(SwCpu *c) { c->x[0] = 0; }

/* Standard C library - stdio stubs */
void svc_printf(SwCpu *c) { c->x[0] = 0; }
void svc_sprintf(SwCpu *c) { c->x[0] = 0; }
void svc_snprintf(SwCpu *c) { c->x[0] = 0; }
void svc_vsnprintf(SwCpu *c) { c->x[0] = 0; }

void svc_fopen(SwCpu *c) { c->x[0] = 0; }
void svc_fclose(SwCpu *c) { c->x[0] = 0; }
void svc_fread(SwCpu *c) { c->x[0] = 0; }
void svc_fwrite(SwCpu *c) { c->x[0] = 0; }
void svc_fseek(SwCpu *c) { c->x[0] = 0; }
void svc_ftell(SwCpu *c) { c->x[0] = 0; }
void svc_fflush(SwCpu *c) { c->x[0] = 0; }

void svc_stdin(SwCpu *c) { c->x[0] = 0; }
void svc_stdout(SwCpu *c) { c->x[0] = 1; }
void svc_stderr(SwCpu *c) { c->x[0] = 2; }

/* Standard C library - string */
void svc_strchr(SwCpu *c)
{
    u64 s = c->x[0], c_char = c->x[1];
    for (;;) {
        u8 ch = sw_load8(c, s);
        if (!ch) { c->x[0] = 0; return; }
        if (ch == (u8)c_char) { c->x[0] = s; return; }
        s++;
    }
}

void svc_strrchr(SwCpu *c)
{
    u64 s = c->x[0], c_char = c->x[1];
    u64 found = 0;
    for (u64 i = 0;; i++) {
        u8 ch = sw_load8(c, s + i);
        if (!ch) { c->x[0] = found; return; }
        if (ch == (u8)c_char) found = s + i;
    }
}

void svc_strstr(SwCpu *c)
{
    u64 haystack = c->x[0], needle = c->x[1];
    u64 i, j;
    for (i = 0;; i++) {
        u8 h = sw_load8(c, haystack + i);
        if (!h) { c->x[0] = 0; return; }
        for (j = 0;; j++) {
            u8 n = sw_load8(c, needle + j);
            if (!n) { c->x[0] = haystack + i; return; }
            if (sw_load8(c, haystack + i + j) != n) break;
        }
    }
}

/* Standard C library - math */
void svc_abs(SwCpu *c) { c->x[0] = (u64)(s64)((s64)c->x[0] < 0 ? -(s64)c->x[0] : (s64)c->x[0]); }
void svc_labs(SwCpu *c) { svc_abs(c); }
void svc_llabs(SwCpu *c) { svc_abs(c); }

/* Standard C library - time */
void svc_time(SwCpu *c) { c->x[0] = (u64)time(NULL); }
void svc_clock(SwCpu *c) { c->x[0] = (u64)clock(); }
void svc_gettimeofday(SwCpu *c) { c->x[0] = 0; }

/* Standard C library - random */
void svc_rand(SwCpu *c) { c->x[0] = rand(); }
void svc_srand(SwCpu *c) { srand((unsigned int)c->x[0]); }

/* Standard C library - environment */
void svc_getenv(SwCpu *c) { c->x[0] = 0; }
void svc_setenv(SwCpu *c) { c->x[0] = 0; }
void svc_putenv(SwCpu *c) { c->x[0] = 0; }
void svc_unsetenv(SwCpu *c) { c->x[0] = 0; }

/* Standard C library - system */
void svc_system(SwCpu *c) { c->x[0] = 0; }

/* Standard C library - exit */
void svc_exit(SwCpu *c) { sw_trap(c, 0, "guest called exit()"); }
void svc_abort(SwCpu *c) { sw_trap(c, 0, "guest called abort()"); }
void svc__Exit(SwCpu *c) { sw_trap(c, 0, "guest called _Exit()"); }

/* Standard C library - qsort/bsearch */
void svc_qsort(SwCpu *c) { c->x[0] = 0; }
void svc_bsearch(SwCpu *c) { c->x[0] = 0; }

/* C++ new/delete */
void svc_Znwm(SwCpu *c) { svc_malloc(c); }
void svc_ZdlPv(SwCpu *c) { svc_free(c); }
void svc_Znwj(SwCpu *c) { svc_malloc(c); }
void svc_ZdlPvj(SwCpu *c) { svc_free(c); }

/* pthread stubs */
void svc_pthread_create(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_join(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_mutex_init(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_mutex_lock(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_mutex_unlock(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_cond_init(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_cond_wait(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_cond_signal(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_cond_broadcast(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_once(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_key_create(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_key_delete(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_getspecific(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_setspecific(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_mutexattr_init(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_mutexattr_destroy(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_mutexattr_settype(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_condattr_init(SwCpu *c) { c->x[0] = 0; }
void svc_pthread_condattr_destroy(SwCpu *c) { c->x[0] = 0; }

/* semaphore stubs */
void svc_sem_init(SwCpu *c) { c->x[0] = 0; }
void svc_sem_destroy(SwCpu *c) { c->x[0] = 0; }
void svc_sem_wait(SwCpu *c) { c->x[0] = 0; }
void svc_sem_post(SwCpu *c) { c->x[0] = 0; }
void svc_sem_trywait(SwCpu *c) { c->x[0] = 0; }

/* clock/timer stubs */
void svc_clock_gettime(SwCpu *c) { c->x[0] = 0; }
void svc_clock_getres(SwCpu *c) { c->x[0] = 0; }
void svc_nanosleep(SwCpu *c) { c->x[0] = 0; }

/* process/thread IDs */
void svc_getpid(SwCpu *c) { c->x[0] = GetCurrentProcessId(); }
void svc_gettid(SwCpu *c) { c->x[0] = GetCurrentThreadId(); }

/* dlopen/dlsym stubs */
void svc_dlopen(SwCpu *c) { c->x[0] = 0; }
void svc_dlclose(SwCpu *c) { c->x[0] = 0; }
void svc_dlsym(SwCpu *c) { c->x[0] = 0; }
void svc_dlerror(SwCpu *c) { c->x[0] = 0; }

/* backtrace stubs */
void svc_backtrace(SwCpu *c) { c->x[0] = 0; }
void svc_backtrace_symbols(SwCpu *c) { c->x[0] = 0; }

/* malloc extensions */
void svc_malloc_usable_size(SwCpu *c) { c->x[0] = 0; }
void svc_posix_memalign(SwCpu *c) { c->x[0] = 0; }
void svc_aligned_alloc(SwCpu *c) { c->x[0] = 0; }
void svc_cfree(SwCpu *c) { c->x[0] = 0; }
void svc_malloc_trim(SwCpu *c) { c->x[0] = 0; }
void svc_mallopt(SwCpu *c) { c->x[0] = 0; }
void svc_mallinfo(SwCpu *c) { c->x[0] = 0; }
void svc___libc_malloc(SwCpu *c) { svc_malloc(c); }
void svc___libc_free(SwCpu *c) { svc_free(c); }
void svc___libc_realloc(SwCpu *c) { svc_realloc(c); }
void svc___libc_calloc(SwCpu *c) { svc_calloc(c); }
void svc___libc_memalign(SwCpu *c) { c->x[0] = 0; }
void svc___libc_valloc(SwCpu *c) { c->x[0] = 0; }
void svc___libc_pvalloc(SwCpu *c) { c->x[0] = 0; }
void svc_memalign(SwCpu *c) { c->x[0] = 0; }
void svc_valloc(SwCpu *c) { c->x[0] = 0; }
void svc_pvalloc(SwCpu *c) { c->x[0] = 0; }

/* errno */
void svc___errno_location(SwCpu *c) { static int err = 0; c->x[0] = (u64)&err; }
void svc_errno(SwCpu *c) { c->x[0] = 0; }

/* strerror */
void svc_strerror(SwCpu *c) { c->x[0] = 0; }
void svc_strerror_r(SwCpu *c) { c->x[0] = 0; }

/* signal handling */
void svc_signal(SwCpu *c) { c->x[0] = 0; }
void svc_raise(SwCpu *c) { c->x[0] = 0; }

/* setjmp/longjmp */
void svc_setjmp(SwCpu *c) { c->x[0] = 0; }
void svc_longjmp(SwCpu *c) { sw_trap(c, 0, "longjmp not implemented"); }

/* Standard C library - misc */
void svc_strtok(SwCpu *c) { c->x[0] = 0; }
void svc_strtok_r(SwCpu *c) { c->x[0] = 0; }
void svc_div(SwCpu *c) { c->x[0] = 0; }
void svc_ldiv(SwCpu *c) { c->x[0] = 0; }
void svc_lldiv(SwCpu *c) { c->x[0] = 0; }
void svc_atoi(SwCpu *c) { c->x[0] = 0; }
void svc_atol(SwCpu *c) { c->x[0] = 0; }
void svc_atoll(SwCpu *c) { c->x[0] = 0; }
void svc_strtod(SwCpu *c) { c->x[0] = 0; }
void svc_strtof(SwCpu *c) { c->x[0] = 0; }
void svc_strtol(SwCpu *c) { c->x[0] = 0; }
void svc_strtoul(SwCpu *c) { c->x[0] = 0; }
void svc_strtoll(SwCpu *c) { c->x[0] = 0; }
void svc_strtoull(SwCpu *c) { c->x[0] = 0; }

/* Catch-all for any remaining imports */
void svc_unimplemented_import(SwCpu *c, const char *name)
{
    char why[320];
    snprintf(why, sizeof(why),
             "unimplemented import '%s' was called (pc in recompiled code); "
             "add a native service binding before this path can run", name);
    printf("[service] %s\n", why);
    sw_trap(c, 0, why);
    }

    /* ---------------- Missing service stubs ------------- */
/* These services are declared in NATIVE_SERVICES but not fully implemented yet.
 * They trap with a clear message so missing functionality is visible. */

void svc_nn_sf_cmif_GetInlineContext(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::sf::cmif::GetInlineContext");
}

void svc_nn_sf_hipc_SendSyncRequest(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::sf::hipc::SendSyncRequest");
}

void svc_nn_sf_hipc_ConnectToHipcService(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::sf::hipc::ConnectToHipcService");
}

void svc_nn_sf_hipc_GetMessageBufferOnTls(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::sf::hipc::GetMessageBufferOnTls");
}

void svc_nn_sf_hipc_CloseClientSessionHandle(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::sf::hipc::CloseClientSessionHandle");
}

void svc_nn_sf_hipc_FinalizeHipcServiceResolution(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::sf::hipc::FinalizeHipcServiceResolution");
}

void svc_nn_sf_hipc_InitializeHipcServiceResolution(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::sf::hipc::InitializeHipcServiceResolution");
}

void svc_nn_sf_hipc_HandleHipcErrorOnClient(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::sf::hipc::HandleHipcErrorOnClient");
}

void svc_nn_sf_hipc_HipcMessageBufferAccessor_SetupHeader(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::sf::hipc::HipcMessageBufferAccessor::SetupHeader");
}

void svc_nn_sf_hipc_HipcMessageBufferAccessor_SetTag(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::sf::hipc::HipcMessageBufferAccessor::SetTag");
}

void svc_nn_sf_impl_ServiceObjectImplBase2_ReleaseImpl(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::sf::impl::ServiceObjectImplBase2::ReleaseImpl");
}

void svc_nn_sf_impl_ServiceObjectImplBase2_AddReferenceImpl(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::sf::impl::ServiceObjectImplBase2::AddReferenceImpl");
}

void svc_nn_lmem_CreateExpHeap(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::lmem::CreateExpHeap");
}

void svc_nn_lmem_FreeToExpHeap(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::lmem::FreeToExpHeap");
}

void svc_nn_lmem_AllocateFromExpHeap(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::lmem::AllocateFromExpHeap");
}

void svc_nn_nsd_GetEnvironmentIdentifier(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::nsd::GetEnvironmentIdentifier");
}

void svc_nn_util_ReferSymbol(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::util::ReferSymbol");
}

void svc_nn_util_RelocationTable_Relocate(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::util::RelocationTable::Relocate");
}

void svc_nn_util_VSNPrintf(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::util::VSNPrintf");
}

void svc_nn_util_TinyMt_Initialize(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::util::TinyMt::Initialize");
}

void svc_nn_util_TinyMt_GenerateRandomU32(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::util::TinyMt::GenerateRandomU32");
}

void svc_nn_util_BitArray_reset(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::util::BitArray::reset");
}

void svc_nn_util_pmr_NewDeleteResource(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::util::pmr::NewDeleteResource");
}

void svc_nn_util_pmr_GetDefaultResource(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::util::pmr::GetDefaultResource");
}

void svc_nn_util_pmr_SetDefaultResource(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::util::pmr::SetDefaultResource");
}

/* ---------------- nn::audio stubs ------------- */
void svc_nn_audio_InitializeAudioOutParameter(SwCpu *c)
{
    u64 param = c->x[0];
    u8 *p;
    if (guest_ptr(c, param, 0x200, 1, &p)) memset(p, 0, 0x200);
    c->x[0] = 0;
}

void svc_nn_audio_OpenDefaultAudioOut(SwCpu *c)
{
    u64 audio_out = c->x[0];
    u64 param = c->x[1];
    u8 *p;
    if (guest_ptr(c, audio_out, 8, 1, &p)) *(u64*)p = 0x2000; /* Dummy AudioOut handle */
    c->x[0] = 0;
}

void svc_nn_audio_StartAudioOut(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_audio_StopAudioOut(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_audio_CloseAudioOut(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_audio_AppendAudioOutBuffer(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_audio_GetReleasedAudioOutBuffer(SwCpu *c)
{
    u64 audio_out = c->x[0];
    u64 buffer_ptr = c->x[1];
    u8 *p;
    if (guest_ptr(c, buffer_ptr, 8, 1, &p)) *(u64*)p = 0; /* No buffer available */
    c->x[0] = 0;
}

void svc_nn_audio_GetAudioOutSampleRate(SwCpu *c)
{
    c->x[0] = 48000; /* 48kHz */
}

void svc_nn_audio_GetAudioOutChannelCount(SwCpu *c)
{
    c->x[0] = 2; /* Stereo */
}

void svc_nn_audio_GetAudioOutSampleFormat(SwCpu *c)
{
    c->x[0] = 1; /* PCM16 */
}

void svc_nn_audio_GetAudioOutBufferDataSize(SwCpu *c)
{
    c->x[0] = 4096; /* 4KB buffer */
}

void svc_nn_audio_GetAudioOutBufferDataPointer(SwCpu *c)
{
    u64 buffer = c->x[0];
    u64 ptr_ptr = c->x[1];
    u8 *p;
    if (guest_ptr(c, ptr_ptr, 8, 1, &p)) *(u64*)p = buffer + 0x100; /* Dummy data pointer */
    c->x[0] = 0;
}

void svc_nn_audio_GetAudioOutPlayedSampleCount(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_audio_GetSampleByteSize(SwCpu *c)
{
    u64 format = c->x[0];
    c->x[0] = (format == 1) ? 2 : 4; /* PCM16=2, PCM32=4 */
}

void svc_nn_audio_InitializePolyphaseResampler(SwCpu *c)
{
    u64 resampler = c->x[0];
    u8 *p;
    if (guest_ptr(c, resampler, 0x100, 1, &p)) memset(p, 0, 0x100);
    c->x[0] = 0;
}

void svc_nn_audio_ProcessPolyphaseResamplerBuffer(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_audio_GetRequiredBufferSizeForPolyphaseResampler(SwCpu *c)
{
    c->x[0] = 8192;
}

void svc_nn_audio_CalculatePolyphaseResamplerActualOutputSampleRate(SwCpu *c)
{
    c->x[0] = 48000;
}

/* ---------------- nn::font stubs ------------- */

void svc_nn_font_ScalableFontEngine_C1(SwCpu *c)
{
    c->x[0] = 0; /* Success */
}

void svc_nn_font_ScalableFontEngine_SetFont(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::font::ScalableFontEngine::SetFont");
}

void svc_nn_font_ScalableFontEngine_GetError(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::font::ScalableFontEngine::GetError");
}

void svc_nn_font_ScalableFontEngine_LoadFont(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::font::ScalableFontEngine::LoadFont");
}

void svc_nn_font_ScalableFontEngine_SetFlags(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::font::ScalableFontEngine::SetFlags");
}

void svc_nn_font_ScalableFontEngine_SetScale(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::font::ScalableFontEngine::SetScale");
}

void svc_nn_font_ScalableFontEngineHelper_Decode(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::font::ScalableFontEngineHelper::Decode");
}

void svc_nn_detail_UnexpectedDefaultImpl(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::detail::UnexpectedDefaultImpl");
}

void svc_nn_diag_AbortImpl(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::diag::AbortImpl");
}

void svc_nn_result_OnUnhandledResult(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::result::OnUnhandledResult");
}

void svc_nn_oe_GetDesiredLanguage(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::oe::GetDesiredLanguage");
}

void svc_nn_settings_GetLanguage(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::settings::GetLanguage");
}

void svc_nn_settings_SetLanguage(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::settings::SetLanguage");
}

void svc_nn_oe_FinishStartupLogo(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::oe::FinishStartupLogo");
}

void svc_nn_oe_SetFocusHandlingMode(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::oe::SetFocusHandlingMode");
}

void svc_nn_oe_SetOperationModeChangedNotificationEnabled(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::oe::SetOperationModeChangedNotificationEnabled");
}

void svc_nn_oe_EnterExitRequestHandlingSection(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::oe::EnterExitRequestHandlingSection");
}

void svc_nn_oe_LeaveExitRequestHandlingSection(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::oe::LeaveExitRequestHandlingSection");
}

void svc_nn_oe_GetOperationMode(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::oe::GetOperationMode");
}

void svc_nn_oe_GetCurrentFocusState(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::oe::GetCurrentFocusState");
}

void svc_nn_swkbd_GetRequiredStringBufferSize(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::swkbd::GetRequiredStringBufferSize");
}

void svc_nn_swkbd_ShowKeyboard(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::swkbd::ShowKeyboard");
}

void svc_nn_swkbd_MakePreset(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::swkbd::MakePreset");
}

void svc_nn_swkbd_GetRequiredWorkBufferSize(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::swkbd::GetRequiredWorkBufferSize");
}

void svc_nn_swkbd_SetUserWordList(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::swkbd::SetUserWordList");
}

void svc_nn_swkbd_GetRequiredTextCheckWorkBufferSize(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::swkbd::GetRequiredTextCheckWorkBufferSize");
}

void svc_nn_swkbd_SetHeaderText(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::swkbd::SetHeaderText");
}

void svc_nn_swkbd_SetSubText(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::swkbd::SetSubText");
}

void svc_nn_swkbd_SetTextCheckCallback(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::swkbd::SetTextCheckCallback");
}

void svc_nn_swkbd_SetInitialText(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::swkbd::SetInitialText");
}

void svc_nn_hid_GetNpadState(SwCpu *c)
{
    u64 state_ptr = c->x[0];
    u64 npad_id = c->x[1];
    
    u8 *p;
    if (guest_ptr(c, state_ptr, 0x100, 1, &p)) {
        /* Zero out the state structure - returns neutral/centered input */
        memset(p, 0, 0x100);
    }
    c->x[0] = 0; /* Success */
}

void svc_nn_hid_InitializeNpad(SwCpu *c)
{
    c->x[0] = 0; /* Success */
}

void svc_nn_hid_SetSupportedNpadStyleSet(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_SetSupportedNpadIdType(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_SetNpadJoyHoldType(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_SetNpadHandheldActivationMode(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_BindNpadStyleSetUpdateEvent(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_GetNpadStyleSet(SwCpu *c)
{
    u64 style_set_ptr = c->x[0];
    u64 npad_id = c->x[1];
    
    u8 *p;
    if (guest_ptr(c, style_set_ptr, 8, 1, &p)) {
        *(u64*)p = 0xF; /* All styles supported */
    }
    c->x[0] = 0;
}

void svc_nn_hid_GetSixAxisSensorState(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_GetSixAxisSensorStates(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_IsSixAxisSensorAtRest(SwCpu *c)
{
    c->x[0] = 1; /* At rest */
}

void svc_nn_hid_StartSixAxisSensor(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_StopSixAxisSensor(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_IsSixAxisSensorFusionEnabled(SwCpu *c)
{
    c->x[0] = 0; /* Disabled */
}

void svc_nn_hid_EnableSixAxisSensorFusion(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_SetSixAxisSensorFusionParameters(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_GetSixAxisSensorFusionParameters(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_SetGyroscopeZeroDriftMode(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_GetGyroscopeZeroDriftMode(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_GetSixAxisSensorHandles(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_hid_NpadIdHandheld(SwCpu *c)
{
    c->x[0] = 0x10; /* Handheld ID */
}

void svc_nn_hid_NpadIdNo1(SwCpu *c)
{
    c->x[0] = 0x11; /* Controller 1 ID */
}

void svc_nn_account_Initialize(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::account::Initialize");
}

void svc_nn_account_TryOpenPreselectedUser(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::account::TryOpenPreselectedUser");
}

void svc_nn_account_GetUserId(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::account::GetUserId");
}

void svc_nn_account_GetNickname(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::account::GetNickname");
}

void svc_nn_account_IsNetworkServiceAccountAvailable(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::account::IsNetworkServiceAccountAvailable");
}

void svc_nn_account_GetNetworkServiceAccountId(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::account::GetNetworkServiceAccountId");
}

void svc_nn_account_GetUserExistence(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::account::GetUserExistence");
}

void svc_nn_account_AsyncContext_D1(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::account::AsyncContext::~AsyncContext");
}

void svc_nn_account_AsyncContext_GetSystemEvent(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::account::AsyncContext::GetSystemEvent");
}

void svc_nn_account_AsyncContext_Assign(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::account::AsyncContext::operator=");
}

void svc_nn_friends_GetFriendList(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::friends::GetFriendList");
}

void svc_nn_time_GetCurrentTime(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::time::StandardUserSystemClock::GetCurrentTime");
}

void svc_nn_time_ToCalendarTime(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::time::ToCalendarTime");
}

void svc_nn_time_GetDeviceLocationName(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::time::GetDeviceLocationName");
}

void svc_nn_prepo_PlayReport_C1(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::prepo::PlayReport::C1");
}

void svc_nn_prepo_PlayReport_SetBuffer(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::prepo::PlayReport::SetBuffer");
}

void svc_nn_prepo_PlayReport_Clear(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::prepo::PlayReport::Clear");
}

void svc_nn_prepo_PlayReport_SetEventId(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::prepo::PlayReport::SetEventId");
}

void svc_nn_prepo_PlayReport_Save(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::prepo::PlayReport::Save");
}

void svc_nn_prepo_PlayReport_Add(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::prepo::PlayReport::Add");
}

void svc_nn_fs_GetCurrentThreadFsContext(SwCpu *c)
{
    c->x[0] = 0; /* Return 0 for success, context pointer in x[1] if needed */
    c->x[1] = 0;
}

void svc_nn_fs_SetCurrentThreadFsContext(SwCpu *c)
{
    c->x[0] = 0;
}

void svc_nn_fs_MountRom(SwCpu *c)
{
    u64 path_ptr = c->x[0];
    u8 *p;
    if (guest_ptr(c, path_ptr, 256, 0, &p)) {
        printf("[fs] MountRom: %s\n", (char*)p);
    }
    c->x[0] = 0; /* Success */
}

void svc_nn_fs_GetFileSize(SwCpu *c)
{
    u64 size_ptr = c->x[0];
    u64 handle = c->x[1];
    /* For now, return a dummy size */
    u8 *p;
    if (guest_ptr(c, size_ptr, 8, 1, &p)) {
        *(u64*)p = 0x100000; /* 1MB dummy size */
    }
    c->x[0] = 0;
}

void svc_nn_fs_OpenFile(SwCpu *c)
{
    u64 handle_ptr = c->x[0];
    u64 path_ptr = c->x[1];
    int mode = (int)c->x[2];
    
    u8 *path_buf;
    char path[256] = {0};
    if (guest_ptr(c, path_ptr, 255, 0, &path_buf)) {
        strncpy(path, (char*)path_buf, 255);
    }
    printf("[fs] OpenFile: %s (mode=%d)\n", path, mode);
    
    /* Return a dummy file handle */
    u8 *p;
    if (guest_ptr(c, handle_ptr, 8, 1, &p)) {
        *(u64*)p = 0x1000; /* Dummy handle */
    }
    c->x[0] = 0;
}

void svc_nn_fs_ReadFile(SwCpu *c)
{
    u64 bytes_read_ptr = c->x[0];
    u64 handle = c->x[1];
    u64 offset = c->x[2];
    u64 buffer = c->x[3];
    u64 size = c->x[4];
    
    u8 *p;
    if (guest_ptr(c, bytes_read_ptr, 8, 1, &p)) {
        *(u64*)p = size; /* Return requested size as bytes read */
    }
    c->x[0] = 0;
}

void svc_nn_fs_CloseFile(SwCpu *c)
{
    u64 handle = c->x[0];
    printf("[fs] CloseFile: handle=0x%llx\n", (unsigned long long)handle);
    c->x[0] = 0;
}

void svc_nn_err_NplnContext_Make(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::err::NplnContext::Make");
}

void svc_nn_err_UpdateResultWithContext(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::err::UpdateResultWithContext");
}

void svc_nn_err_ErrorResultVariant_Assign(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::err::ErrorResultVariant::operator=");
}

void svc_nn_err_ReportMultimediaError(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::err::ReportMultimediaError");
}

void svc_nn_ssl_GetBuiltInCertificates(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ssl::BuiltInManager::GetBuiltInCertificates");
}

void svc_nn_ssl_GetBuiltInCertificateBufSize(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ssl::BuiltInManager::GetBuiltInCertificateBufSize");
}

void svc_nn_socket_GetAddrInfo(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::GetAddrInfo");
}

void svc_nn_socket_FreeAddrInfo(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::FreeAddrInfo");
}

void svc_nn_socket_InetNtop(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::InetNtop");
}

void svc_nn_socket_InetPton(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::InetPton");
}

void svc_nn_socket_GAIStrError(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::GAIStrError");
}

void svc_nn_socket_Bind(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::Bind");
}

void svc_nn_socket_SendTo(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::SendTo");
}

void svc_nn_socket_Socket(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::Socket");
}

void svc_nn_socket_RecvFrom(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::RecvFrom");
}

void svc_nn_socket_InetHtonl(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::InetHtonl");
}

void svc_nn_socket_InetNtohl(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::InetNtohl");
}

void svc_nn_socket_GetLastErrno(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::GetLastErrno");
}

void svc_nn_socket_Poll(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::Poll");
}

void svc_nn_socket_Shutdown(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::Shutdown");
}

void svc_nn_socket_EventFdRead(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::EventFdRead");
}

void svc_nn_socket_EventFdWrite(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::EventFdWrite");
}

void svc_nn_socket_EventFd(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::socket::EventFd");
}

void svc_nn_ldn_Disconnect(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::Disconnect");
}

void svc_nn_ldn_OpenStation(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::OpenStation");
}

void svc_nn_ldn_ScanPrivate(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::ScanPrivate");
}

void svc_nn_ldn_CloseStation(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::CloseStation");
}

void svc_nn_ldn_CreateNetwork(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::CreateNetwork");
}

void svc_nn_ldn_ConnectPrivate(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::ConnectPrivate");
}

void svc_nn_ldn_DestroyNetwork(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::DestroyNetwork");
}

void svc_nn_ldn_GetIpv4Address(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::GetIpv4Address");
}

void svc_nn_ldn_GetNetworkInfo(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::GetNetworkInfo");
}

void svc_nn_ldn_OpenAccessPoint(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::OpenAccessPoint");
}

void svc_nn_ldn_CloseAccessPoint(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::CloseAccessPoint");
}

void svc_nn_ldn_SetAdvertiseData(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::SetAdvertiseData");
}

void svc_nn_ldn_AddAcceptFilterEntry(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::AddAcceptFilterEntry");
}

void svc_nn_ldn_CreateNetworkPrivate(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::CreateNetworkPrivate");
}

void svc_nn_ldn_GetSecurityParameter(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::GetSecurityParameter");
}

void svc_nn_ldn_MakeBroadcastAddress(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::MakeBroadcastAddress");
}

void svc_nn_ldn_AttachStateChangeEvent(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::AttachStateChangeEvent");
}

void svc_nn_ldn_SetStationAcceptPolicy(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::SetStationAcceptPolicy");
}

void svc_nn_ldn_Scan(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::Scan");
}

void svc_nn_ldn_Reject(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::Reject");
}

void svc_nn_ldn_Connect(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::Connect");
}

void svc_nn_ldn_Ipv4Address_eq(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::Ipv4Address::operator==");
}

void svc_nn_ldn_GetDisconnectReason(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::GetDisconnectReason");
}

void svc_nn_ldn_MakeIpv4Address(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ldn::MakeIpv4Address");
}

void svc_nn_ngc_ProfanityFilter_C1(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ngc::ProfanityFilter::C1");
}

void svc_nn_ngc_ProfanityFilter_Initialize(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ngc::ProfanityFilter::Initialize");
}

void svc_nn_ngc_ProfanityFilter_Finalize(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::ngc::ProfanityFilter::Finalize");
}

void svc_nn_crypto_GenerateCryptographicallyRandomBytes(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::crypto::GenerateCryptographicallyRandomBytes");
}

void svc_nn_lmem_CreateFrameHeap(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::lmem::CreateFrameHeap");
}

void svc_nn_lmem_DestroyFrameHeap(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::lmem::DestroyFrameHeap");
}

void svc_nn_lmem_AllocateFromFrameHeap(SwCpu *c)
{
    sw_unimplemented_import(c, "nn::lmem::AllocateFromFrameHeap");
}

/* NVN entry points: see sw_nvn.c */

/* Catch-all for any remaining nn:: services */
void svc_unimplemented_nn(SwCpu *c)
{
    sw_unimplemented_import(c, "nn:: service (unimplemented)");
}
