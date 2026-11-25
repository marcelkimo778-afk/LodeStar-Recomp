/* sw_rt.h - compatibility runtime ABI for statically recompiled Switch modules.
 *
 * Generated code includes only this header.  Everything here is deterministic
 * and contains no ARM64 interpreter: a standalone build links no CPU emulator.
 */
#ifndef SW_RT_H
#define SW_RT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#if defined(_MSC_VER) && !defined(__clang__)
/* Generated chunks build with /O1, which (unlike /O2) has no /Oi: without
 * this every fixed-size memcpy (guest loads/stores, bit casts) was a call
 * into the CRT - ~18% of all CPU time in the village. */
#include <intrin.h>
#pragma intrinsic(memcpy, memset, memcmp)
#endif
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
/* FPCR.FZ is mapped to MXCSR FTZ+DAZ (sw_fpcr_apply).  Hardware SSE arithmetic
 * then flushes like Arm, but helpers that look at the bits (min/max, estimates)
 * must flush denormal inputs themselves: sw_fz() reads the mode (only called
 * on the rare denormal path). */
static int sw_fz(void) { return (_mm_getcsr() & 0x40u) != 0; }
static float sw_ftz32(float x) { uint32_t b; memcpy(&b, &x, 4); if (!(b & 0x7F800000u) && (b & 0x7FFFFFu) && sw_fz()) { b &= 0x80000000u; memcpy(&x, &b, 4); } return x; }
static double sw_ftz64(double x) { uint64_t b; memcpy(&b, &x, 8); if (!(b & 0x7FF0000000000000ull) && (b & 0xFFFFFFFFFFFFFull) && sw_fz()) { b &= 0x8000000000000000ull; memcpy(&x, &b, 8); } return x; }
/* FABS / FNEG only change the sign bit (never flushed, even with FZ); FSQRT is
 * the hardware instruction (sqrt(-0) = -0, DAZ applies).  The CRT versions went
 * through double / software checks: fabs of a denormal became 0 with FZ, and
 * sqrt of a negative denormal became NaN.  Generated code calls fabsf/fabs/
 * sqrtf/sqrt, so the names are mapped here. */
static float sw_fabs_bits32(float x) { uint32_t b; memcpy(&b, &x, 4); b &= 0x7FFFFFFFu; memcpy(&x, &b, 4); return x; }
static double sw_fabs_bits64(double x) { uint64_t b; memcpy(&b, &x, 8); b &= 0x7FFFFFFFFFFFFFFFull; memcpy(&x, &b, 8); return x; }
static float sw_sqrt_hw32(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
static double sw_sqrt_hw64(double x) { __m128d v = _mm_set_sd(x); return _mm_cvtsd_f64(_mm_sqrt_sd(v, v)); }
#define fabsf(x) sw_fabs_bits32(x)
#define fabs(x) sw_fabs_bits64(x)
#define sqrtf(x) sw_sqrt_hw32(x)
#define sqrt(x) sw_sqrt_hw64(x)
#else
static float sw_ftz32(float x) { return x; }
static double sw_ftz64(double x) { return x; }
static int sw_fz(void) { return 0; }
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define SW_ABI_VERSION 3

typedef union sw_v128 {
    u8 b[16];
    u16 h[8];
    u32 s[4];
    u64 d[2];
    float f[4];
    double df[2];
} sw_v128;

typedef struct SwMem SwMem;
typedef struct SwRuntime SwRuntime;

/* Host controller state, mapped to the Switch HID button layout. */
enum sw_button {
    SW_BTN_A = 1u << 0, SW_BTN_B = 1u << 1, SW_BTN_X = 1u << 2, SW_BTN_Y = 1u << 3,
    SW_BTN_L = 1u << 4, SW_BTN_R = 1u << 5, SW_BTN_ZL = 1u << 6, SW_BTN_ZR = 1u << 7,
    SW_BTN_PLUS = 1u << 8, SW_BTN_MINUS = 1u << 9, SW_BTN_HOME = 1u << 10,
    SW_BTN_CAPTURE = 1u << 11, SW_BTN_L3 = 1u << 12, SW_BTN_R3 = 1u << 13,
    SW_BTN_UP = 1u << 14, SW_BTN_DOWN = 1u << 15, SW_BTN_LEFT = 1u << 16,
    SW_BTN_RIGHT = 1u << 17
};

typedef struct SwPadState {
    u32 buttons;
    s16 lx, ly, rx, ry;
    u8 connected;
} SwPadState;

void sw_input_init(void);
int sw_input_poll(SwPadState *out);
const char *sw_input_button_name(unsigned bit);
void sw_input_describe(SwPadState *pad, char *buf, size_t size);

typedef struct SwCpu {
    u64 x[31];
    u64 sp;
    u8 nf, zf, cf, vf;
    sw_v128 v[32];
    u64 tpidr_el0;
    u64 tpidrro_el0;
    u64 fpcr, fpsr;
    u64 thread_id;
    int halted;
    u64 halt_pc;
    const char *halt_reason;
    SwMem *mem;
    SwRuntime *rt;
    u64 svc_count;
    u64 call_depth;
    u64 cur_fn;          /* entry address of the native function executing */
    u64 prev_fn;         /* function that dispatched into cur_fn */
    u64 pc;              /* guest instruction address of the last memory op */

    /* Instruction trace ring buffer for crash diagnostics */
    #define SW_INSN_TRACE_SIZE 256
    struct SwInsnTraceEntry {
        u64 pc;
        u64 x0, x1, x2, x3;
        u64 lr;
        u64 cur_fn;
    } insn_trace[SW_INSN_TRACE_SIZE];
    int insn_trace_pos;
    /* exclusive monitor (LDXR/STXR): excl_tag = address + 1 of the last
     * exclusive load, 0 = clear; excl_v0/v1 the value(s) it read */
    u64 excl_tag, excl_v0, excl_v1;
} SwCpu;

enum sw_sysreg {
    SW_SYS_NZCV = 0, SW_SYS_FPCR, SW_SYS_FPSR, SW_SYS_TPIDR_EL0,
    SW_SYS_TPIDRRO_EL0, SW_SYS_CNTFRQ_EL0, SW_SYS_CNTVCT_EL0,
    SW_SYS_CTR_EL0, SW_SYS_DCZID_EL0, SW_SYS_SCTLR_EL2,
    SW_SYS_SWITCH_SYSREG_0_3_3_11_5, SW_SYS__COUNT
};

SwCpu *sw_call(SwCpu *c, u64 target);       /* return c (see below) */
SwCpu *sw_call_reg(SwCpu *c, u64 target);
void sw_dispatch(SwCpu *c, u64 target);
/* the same, for a target whose sw_functions index the recompiler knew (SWRECOMP_CALL_IDX) */
SwCpu *sw_call_i(SwCpu *c, u64 target, u32 idx);
void sw_dispatch_i(SwCpu *c, u64 target, u32 idx);
SwCpu *sw_svc(SwCpu *c, u32 imm);
#if !defined(SW_RT_IMPL)
/* Generated code continues with the CPU pointer a call returns (the same
 * pointer).  Reason: MSVC miscompile (cl 19.44 and 19.51, /Og, i.e. /O1 and
 * /O2).  Generated code reads every call result back from c->x[0], so the
 * results of consecutive calls are loads of the same lvalue; when copies of
 * them are bit-tested in a chain (`bl a; mov w20, w0; bl b; mov w21, w0; ...
 * tbz w20; tbz w21`), the optimiser treats the loads as one value and drops
 * tests.  Guest 0x1642064 (field subsystems ready?) tested only the first of
 * ten results and returned "ready" with four subsystems not ready, so the
 * field message hub never registered a handler: no world interaction worked.
 * A read through the returned pointer is a new value the optimiser keeps.
 * Regression test: Engine/Tests/test_msvc_call_reload.bat. */
#define sw_call(c, t)     ((void)((c) = (sw_call)((c), (t))))
#define sw_call_reg(c, t) ((void)((c) = (sw_call_reg)((c), (t))))
#define sw_call_i(c, t, i) ((void)((c) = (sw_call_i)((c), (t), (i))))
#define sw_svc(c, i)      ((void)((c) = (sw_svc)((c), (i))))
#endif
void sw_trap(SwCpu *c, u64 at, const char *why);
u64 sw_mrs(SwCpu *c, int reg);
void sw_msr(SwCpu *c, int reg, u64 value);
void sw_memory_barrier(void);
u8 *sw_mem_ptr(SwCpu *c, u64 addr, u64 size, int write);
int sw_mem_map(SwCpu *c, u64 addr, u64 size, const void *data, u64 data_size);
void sw_unimplemented_import(SwCpu *c, const char *name);
void sw_register_native_thunk(u64 placeholder_addr, void (*func)(SwCpu *c));
u64 sw_dispatch_target(void);   /* address of the native thunk being executed */
void sw_nvn_init(void);

u8  sw_load8(SwCpu *c, u64 addr);
u16 sw_load16(SwCpu *c, u64 addr);
u32 sw_load32(SwCpu *c, u64 addr);
u64 sw_load64(SwCpu *c, u64 addr);
s8  sw_load8s(SwCpu *c, u64 addr);
s16 sw_load16s(SwCpu *c, u64 addr);
s32 sw_load32s(SwCpu *c, u64 addr);
void sw_store8(SwCpu *c, u64 addr, u64 v);
void sw_store16(SwCpu *c, u64 addr, u64 v);
void sw_store32(SwCpu *c, u64 addr, u64 v);
void sw_store64(SwCpu *c, u64 addr, u64 v);
sw_v128 sw_loadv(SwCpu *c, u64 addr, unsigned bytes);
void sw_storev(SwCpu *c, u64 addr, sw_v128 v, unsigned bytes);

/* Inline fast path for guest memory in generated code (Windows: guest
 * addresses are host addresses).  sw_pmap[page] = 1 for committed 64 KB pages;
 * sw_mem_fast_on = 0 with SW_MEM_TRACE=1 or --watch (then every access takes
 * the out-of-line path above).  sw_runtime.c defines SW_RT_IMPL and keeps the
 * real functions; the (name)(...) calls below bypass the macros. */
#if defined(_MSC_VER) && !defined(__clang__)
#define SW_FORCEINLINE __forceinline
#else
#define SW_FORCEINLINE inline __attribute__((always_inline))
#endif
#if defined(_WIN32) && !defined(SW_RT_IMPL)
extern u8 sw_pmap[];
extern int sw_mem_fast_on;
#if defined(SW_MEM_CHECKED)
/* every access checked; unmapped -> the out-of-line path reports the fault
 * (needed by SW_MEM_TRACE / --watch).  Build with SW_CFLAGS=/DSW_MEM_CHECKED. */
#define SW_INL_OK(a, n) (sw_mem_fast_on && (a) < (1ull << 39) - 16 && sw_pmap[(a) >> 16] && \
                         (((a) & 0xffffu) + (n) <= 0x10000u))
#else
/* Default: guest addresses are host addresses (sw_mem_map commits every guest
 * page at its own address or exits), so a guest access is one host access; a
 * stray one faults into the crash handler as an unmapped access did before.
 * Compiling the checked form cost 3x the compile time and 2x the code size. */
#define SW_INL_OK(a, n) 1
#endif
/* Direct (unaligned) accesses: x64 handles any alignment, and a plain
 * dereference is one mov even where memcpy is not expanded inline. */
#if defined(_MSC_VER) && !defined(__clang__)
#define SW_RD(T, p) (*(const T *)(p))
#define SW_WR(T, p, v) (*(T *)(p) = (v))
#else
#define SW_RD(T, p) ({ T _t; memcpy(&_t, (p), sizeof(T)); _t; })
#define SW_WR(T, p, v) do { T _t = (v); memcpy((p), &_t, sizeof(T)); } while (0)
#endif
#define SW_INL_LOAD(T, NAME, N) \
    static SW_FORCEINLINE T sw_inl_##NAME(SwCpu *c, u64 a) { \
        if (SW_INL_OK(a, N)) return SW_RD(T, (const void *)(uintptr_t)a); \
        return (NAME)(c, a); }
#define SW_INL_STORE(T, NAME, N) \
    static SW_FORCEINLINE void sw_inl_##NAME(SwCpu *c, u64 a, u64 v) { \
        if (SW_INL_OK(a, N)) { SW_WR(T, (void *)(uintptr_t)a, (T)v); return; } \
        (NAME)(c, a, v); }
SW_INL_LOAD(u8, sw_load8, 1)
SW_INL_LOAD(u16, sw_load16, 2)
SW_INL_LOAD(u32, sw_load32, 4)
SW_INL_LOAD(u64, sw_load64, 8)
SW_INL_LOAD(s8, sw_load8s, 1)
SW_INL_LOAD(s16, sw_load16s, 2)
SW_INL_LOAD(s32, sw_load32s, 4)
SW_INL_STORE(u8, sw_store8, 1)
SW_INL_STORE(u16, sw_store16, 2)
SW_INL_STORE(u32, sw_store32, 4)
SW_INL_STORE(u64, sw_store64, 8)
static SW_FORCEINLINE sw_v128 sw_inl_loadv(SwCpu *c, u64 a, unsigned n)
{
    if (n <= 16 && SW_INL_OK(a, n)) {
        const u8 *p = (const u8 *)(uintptr_t)a;
        sw_v128 v;
        switch (n) {   /* n is a constant at every generated call site */
        case 16: v.d[0] = SW_RD(u64, p); v.d[1] = SW_RD(u64, p + 8); return v;
        case 8: v.d[0] = SW_RD(u64, p); v.d[1] = 0; return v;
        case 4: v.d[0] = SW_RD(u32, p); v.d[1] = 0; return v;
        case 2: v.d[0] = SW_RD(u16, p); v.d[1] = 0; return v;
        case 1: v.d[0] = p[0]; v.d[1] = 0; return v;
        default: memset(&v, 0, sizeof v); memcpy(&v, p, n); return v;
        }
    }
    return (sw_loadv)(c, a, n);
}
static SW_FORCEINLINE void sw_inl_storev(SwCpu *c, u64 a, sw_v128 v, unsigned n)
{
    if (n <= 16 && SW_INL_OK(a, n)) {
        u8 *p = (u8 *)(uintptr_t)a;
        switch (n) {
        case 16: SW_WR(u64, p, v.d[0]); SW_WR(u64, p + 8, v.d[1]); return;
        case 8: SW_WR(u64, p, v.d[0]); return;
        case 4: SW_WR(u32, p, v.s[0]); return;
        case 2: SW_WR(u16, p, v.h[0]); return;
        case 1: p[0] = v.b[0]; return;
        default: memcpy(p, &v, n); return;
        }
    }
    (sw_storev)(c, a, v, n);
}
#define sw_load8(c, a)   sw_inl_sw_load8((c), (a))
#define sw_load16(c, a)  sw_inl_sw_load16((c), (a))
#define sw_load32(c, a)  sw_inl_sw_load32((c), (a))
#define sw_load64(c, a)  sw_inl_sw_load64((c), (a))
#define sw_load8s(c, a)  sw_inl_sw_load8s((c), (a))
#define sw_load16s(c, a) sw_inl_sw_load16s((c), (a))
#define sw_load32s(c, a) sw_inl_sw_load32s((c), (a))
#define sw_store8(c, a, v)  sw_inl_sw_store8((c), (a), (v))
#define sw_store16(c, a, v) sw_inl_sw_store16((c), (a), (v))
#define sw_store32(c, a, v) sw_inl_sw_store32((c), (a), (v))
#define sw_store64(c, a, v) sw_inl_sw_store64((c), (a), (v))
#define sw_loadv(c, a, n)     sw_inl_loadv((c), (a), (n))
#define sw_storev(c, a, v, n) sw_inl_storev((c), (a), (v), (n))
#endif

/* ---------------- exclusive monitor (LDXR / STXR, LDXP / STXP) ----------------
 * The exclusive store succeeds (status 0) only if the location still holds the
 * value the exclusive load read, checked and written atomically (compare and
 * swap) - correct with guest threads on several host cores, ABA aside as in
 * every recompiler/emulator that does this.  A different address or a cleared
 * monitor fails (status 1), as on hardware; the guest's loop retries. */
#if defined(_WIN32) && defined(_MSC_VER) && !defined(SW_RT_IMPL)
#include <intrin.h>
#define SW_CAS8(p, e, n)  ((u8)_InterlockedCompareExchange8((volatile char *)(p), (char)(n), (char)(e)) == (u8)(e))
#define SW_CAS16(p, e, n) ((u16)_InterlockedCompareExchange16((volatile short *)(p), (short)(n), (short)(e)) == (u16)(e))
#define SW_CAS32(p, e, n) ((u32)_InterlockedCompareExchange((volatile long *)(p), (long)(n), (long)(e)) == (u32)(e))
#define SW_CAS64(p, e, n) ((u64)_InterlockedCompareExchange64((volatile __int64 *)(p), (__int64)(n), (__int64)(e)) == (u64)(e))
#define SW_EXCL_ATOMIC 1
#endif
#define SW_EXCL(N, T, LOAD, STORE) \
    static SW_FORCEINLINE u64 sw_excl_load##N(SwCpu *c, u64 a) { \
        u64 v = (T)LOAD(c, a); c->excl_tag = a + 1; c->excl_v0 = v; return v; } \
    static SW_FORCEINLINE u64 sw_excl_load_hi##N(SwCpu *c, u64 a) { \
        u64 v = (T)LOAD(c, a); c->excl_v1 = v; return v; }
SW_EXCL(1, u8, sw_load8, sw_store8)
SW_EXCL(2, u16, sw_load16, sw_store16)
SW_EXCL(4, u32, sw_load32, sw_store32)
SW_EXCL(8, u64, sw_load64, sw_store64)
#ifdef SW_EXCL_ATOMIC
#define SW_EXCL_ST(N, BITS) \
    static SW_FORCEINLINE u64 sw_excl_store##N(SwCpu *c, u64 a, u64 v) { \
        int ok = c->excl_tag == a + 1 && SW_CAS##BITS((void *)(uintptr_t)a, c->excl_v0, v); \
        c->excl_tag = 0; return ok ? 0 : 1; }
SW_EXCL_ST(1, 8) SW_EXCL_ST(2, 16) SW_EXCL_ST(4, 32) SW_EXCL_ST(8, 64)
/* pair of 32-bit values: one 64-bit compare and swap */
static SW_FORCEINLINE u64 sw_excl_store2_4(SwCpu *c, u64 a, u64 lo, u64 hi)
{
    u64 e = (c->excl_v0 & 0xffffffffull) | (c->excl_v1 << 32), n = (lo & 0xffffffffull) | (hi << 32);
    int ok = c->excl_tag == a + 1 && SW_CAS64((void *)(uintptr_t)a, e, n);
    c->excl_tag = 0; return ok ? 0 : 1;
}
/* pair of 64-bit values: CMPXCHG16B (needs 16-byte alignment, as LDXP/STXP of a 128-bit pair) */
static SW_FORCEINLINE u64 sw_excl_store2_8(SwCpu *c, u64 a, u64 lo, u64 hi)
{
    int ok = 0;
    if (c->excl_tag == a + 1) {
        __int64 cmp[2] = {(__int64)c->excl_v0, (__int64)c->excl_v1};
        if ((a & 15) == 0)
            ok = _InterlockedCompareExchange128((volatile __int64 *)(uintptr_t)a, (__int64)hi, (__int64)lo, cmp);
        else if (SW_CAS64((void *)(uintptr_t)a, c->excl_v0, lo))   /* unaligned pair: best effort */
            { *(volatile u64 *)(uintptr_t)(a + 8) = hi; ok = 1; }
    }
    c->excl_tag = 0; return ok ? 0 : 1;
}
#else
/* no host atomics (test harness, other hosts): compare then store */
#define SW_EXCL_ST(N, BITS, T, LOAD, STORE) \
    static SW_FORCEINLINE u64 sw_excl_store##N(SwCpu *c, u64 a, u64 v) { \
        int ok = c->excl_tag == a + 1 && (u64)(T)LOAD(c, a) == (u64)(T)c->excl_v0; \
        if (ok) STORE(c, a, v); \
        c->excl_tag = 0; return ok ? 0 : 1; }
SW_EXCL_ST(1, 8, u8, sw_load8, sw_store8) SW_EXCL_ST(2, 16, u16, sw_load16, sw_store16)
SW_EXCL_ST(4, 32, u32, sw_load32, sw_store32) SW_EXCL_ST(8, 64, u64, sw_load64, sw_store64)
static SW_FORCEINLINE u64 sw_excl_store2_4(SwCpu *c, u64 a, u64 lo, u64 hi)
{
    int ok = c->excl_tag == a + 1 && (u32)sw_load32(c, a) == (u32)c->excl_v0 &&
             (u32)sw_load32(c, a + 4) == (u32)c->excl_v1;
    if (ok) { sw_store32(c, a, lo); sw_store32(c, a + 4, hi); }
    c->excl_tag = 0; return ok ? 0 : 1;
}
static SW_FORCEINLINE u64 sw_excl_store2_8(SwCpu *c, u64 a, u64 lo, u64 hi)
{
    int ok = c->excl_tag == a + 1 && sw_load64(c, a) == c->excl_v0 && sw_load64(c, a + 8) == c->excl_v1;
    if (ok) { sw_store64(c, a, lo); sw_store64(c, a + 8, hi); }
    c->excl_tag = 0; return ok ? 0 : 1;
}
#endif

/* ---------------- scalar helpers ---------------- */
static int sw_cond(unsigned cond, unsigned n, unsigned z, unsigned c, unsigned v)
{
    switch (cond & 0xF) {
    case 0x0: return z; break;
    case 0x1: return !z; break;
    case 0x2: return c; break;
    case 0x3: return !c; break;
    case 0x4: return n; break;
    case 0x5: return !n; break;
    case 0x6: return v; break;
    case 0x7: return !v; break;
    case 0x8: return c && !z; break;
    case 0x9: return !c || z; break;
    case 0xA: return n == v; break;
    case 0xB: return n != v; break;
    case 0xC: return !z && (n == v); break;
    case 0xD: return z || (n != v); break;
    default:  return 1;
    }
}

static u64 sw_bswap64(u64 v)
{
    v = ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull);
    v = ((v & 0x0000FFFF0000FFFFull) << 16) | ((v >> 16) & 0x0000FFFF0000FFFFull);
    return (v << 32) | (v >> 32);
}
static u32 sw_bswap32(u32 v)
{
    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    return (v << 16) | (v >> 16);
}
static u64 sw_clz64(u64 v) { u64 n = 0; if (!v) return 64; while (!(v & 0x8000000000000000ull)) { v <<= 1; n++; } return n; }
static u64 sw_clz32(u32 v) { u64 n = 0; if (!v) return 32; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n; }
static u64 sw_cls64(u64 v)
{
    u64 n = 0, sign = v >> 63;
    while (n < 63 && ((v >> (62 - n)) & 1) == sign) n++;
    return n;
}
static u64 sw_cls32(u32 v)
{
    u64 n = 0, sign = v >> 31;
    while (n < 31 && ((v >> (30 - n)) & 1) == sign) n++;
    return n;
}
static u64 sw_rbit64(u64 v)
{
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
    return sw_bswap64(v);
}
static u64 sw_rbit32(u32 v)
{
    u32 r;
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    r = sw_bswap32(v);
    return r;
}
static u64 sw_rev16_32(u32 v) { return (u32)(((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu)); }
static u64 sw_rev16_64(u64 v) { return ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull); }
static u64 sw_rev32_64(u64 v)
{
    return (u64)sw_bswap32((u32)v) | ((u64)sw_bswap32((u32)(v >> 32)) << 32);
}

static u64 sw_ror(u64 value, unsigned amount)
{
    amount &= 63u;
    return amount ? ((value >> amount) | (value << (64 - amount))) : value;
}

static u32 sw_ror32(u32 value, unsigned amount)
{
    amount &= 31u;
    return amount ? ((value >> amount) | (value << (32 - amount))) : value;
}

static s64 sw_sdiv(s64 a, s64 b)
{
    if (b == 0) return 0;
    if (b == -1) return (s64)(0ull - (u64)a);
    return a / b;
}

static s32 sw_sdiv32(s32 a, s32 b)
{
    if (b == 0) return 0;
    if (b == -1) return (s32)(0u - (u32)a);
    return a / b;
}

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
static u64 sw_umulh(u64 a, u64 b) { u64 hi; _umul128(a, b, &hi); return hi; }
static s64 sw_smulh(s64 a, s64 b) { s64 hi; _mul128(a, b, &hi); return hi; }
#else
static u64 sw_umulh(u64 a, u64 b)
{
    unsigned __int128 p = (unsigned __int128)a * b;
    return (u64)(p >> 64);
}
static s64 sw_smulh(s64 a, s64 b)
{
    __int128 p = (__int128)a * (__int128)b;
    return (s64)(p >> 64);
}
#endif

static u64 sw_f64_as_u64(double v) { u64 r; memcpy(&r, &v, 8); return r; }
static double sw_u64_as_f64(u64 v) { double r; memcpy(&r, &v, 8); return r; }
static u32 sw_f32_as_u32(float v) { u32 r; memcpy(&r, &v, 4); return r; }
static float sw_u32_as_f32(u32 v) { float r; memcpy(&r, &v, 4); return r; }

static double sw_rint64(double v) { return nearbyint(v); }
static double sw_round64(double v)
{
    double t = trunc(v), frac = v - t;
    if (fabs(frac) >= 0.5) t += (v < 0 ? -1.0 : 1.0);
    return t;
}

static s64 sw_sat_s64(double v)
{
    if (v != v) return 0;
    if (v >= 9223372036854775808.0) return INT64_MAX;
    if (v <= -9223372036854775808.0) return INT64_MIN;
    return (s64)v;
}
static u64 sw_sat_u64(double v)
{
    if (v != v) return 0;
    if (v <= 0.0) return 0;
    if (v >= 18446744073709551616.0) return UINT64_MAX;
    return (u64)v;
}
static s32 sw_sat_s32(double v)
{
    if (v != v) return 0;
    if (v >= 2147483648.0) return INT32_MAX;
    if (v <= -2147483649.0) return INT32_MIN;
    return (s32)v;
}
static u32 sw_sat_u32(double v)
{
    if (v != v) return 0;
    if (v <= 0.0) return 0;
    if (v >= 4294967296.0) return UINT32_MAX;
    return (u32)v;
}

static double sw_s32_to_f64(s32 v) { return (double)v; }
static double sw_u32_to_f64(u32 v) { return (double)v; }
static double sw_s64_to_f64(s64 v) { return (double)v; }
static double sw_u64_to_f64(u64 v) { return (double)v; }
static float sw_s32_to_f32(s32 v) { return (float)v; }
static float sw_u32_to_f32(u32 v) { return (float)v; }
static float sw_s64_to_f32(s64 v) { return (float)v; }
static float sw_u64_to_f32(u64 v) { return (float)v; }

static s64 sw_f64_to_s64(double v) { return sw_sat_s64(trunc(v)); }
static u64 sw_f64_to_u64(double v) { return sw_sat_u64(trunc(v)); }
static s64 sw_f64_to_s32(double v) { return sw_sat_s32(trunc(v)); }
static u64 sw_f64_to_u32(double v) { return sw_sat_u32(trunc(v)); }
static s64 sw_f32_to_s64(float v) { return sw_sat_s64(trunc((double)v)); }
static u64 sw_f32_to_u64(float v) { return sw_sat_u64(trunc((double)v)); }
static s64 sw_f32_to_s32(float v) { return sw_sat_s32(trunc((double)v)); }
static u64 sw_f32_to_u32(float v) { return sw_sat_u32(trunc((double)v)); }

/* FMAX/FMIN/FMAXNM/FMINNM (Arm): with FZ denormal inputs count as zero; +0 > -0
 * (max(+0,-0) = +0, min(+0,-0) = -0); FMAX/FMIN propagate a NaN (first NaN
 * operand, quietened), the NM forms return the number if only one is NaN. */
#define SW_FMINMAX_DEF(SFX, T, U, FTZ) \
    static T sw_fmax##SFX(T a, T b) { a = FTZ(a); b = FTZ(b); if (a != a) return a + a; if (b != b) return b + b; \
        if (a == b) { U x, y; memcpy(&x, &a, sizeof x); memcpy(&y, &b, sizeof y); x &= y; memcpy(&a, &x, sizeof x); return a; } \
        return a > b ? a : b; } \
    static T sw_fmin##SFX(T a, T b) { a = FTZ(a); b = FTZ(b); if (a != a) return a + a; if (b != b) return b + b; \
        if (a == b) { U x, y; memcpy(&x, &a, sizeof x); memcpy(&y, &b, sizeof y); x |= y; memcpy(&a, &x, sizeof x); return a; } \
        return a < b ? a : b; } \
    static T sw_fmaxnm##SFX(T a, T b) { if (a != a && b == b) return FTZ(b); if (b != b && a == a) return FTZ(a); return sw_fmax##SFX(a, b); } \
    static T sw_fminnm##SFX(T a, T b) { if (a != a && b == b) return FTZ(b); if (b != b && a == a) return FTZ(a); return sw_fmin##SFX(a, b); }
SW_FMINMAX_DEF(64, double, u64, sw_ftz64)
SW_FMINMAX_DEF(32, float, u32, sw_ftz32)

/* ---------------- SIMD helpers ---------------- */
#define SW_LANES(FIELD, T) ((int)(sizeof(((sw_v128 *)0)->FIELD) / sizeof(T)))

#define SW_BINF(NAME, FIELD, T, EXPR) \
    static sw_v128 NAME(sw_v128 a, sw_v128 b) { \
        sw_v128 r; int i; \
        for (i = 0; i < SW_LANES(FIELD, T); i++) { T x = a.FIELD[i], y = b.FIELD[i]; r.FIELD[i] = (T)(EXPR); } \
        return r; }
#define SW_UNF(NAME, FIELD, T, EXPR) \
    static sw_v128 NAME(sw_v128 a) { \
        sw_v128 r; int i; \
        for (i = 0; i < SW_LANES(FIELD, T); i++) { T x = a.FIELD[i]; r.FIELD[i] = (T)(EXPR); } \
        return r; }

#define SW_INT_OPS(SFX, FIELD, T, ST, UT, WIDE, SWIDE) \
    SW_BINF(sw_v_add_##SFX, FIELD, T, x + y) \
    SW_BINF(sw_v_sub_##SFX, FIELD, T, x - y) \
    SW_BINF(sw_v_mul_##SFX, FIELD, T, x * y) \
    SW_BINF(sw_v_umax_##SFX, FIELD, T, x > y ? x : y) \
    SW_BINF(sw_v_umin_##SFX, FIELD, T, x < y ? x : y) \
    SW_BINF(sw_v_smax_##SFX, FIELD, T, (s64)(ST)x > (s64)(ST)y ? x : y) \
    SW_BINF(sw_v_smin_##SFX, FIELD, T, (s64)(ST)x < (s64)(ST)y ? x : y) \
    SW_BINF(sw_v_cmeq_##SFX, FIELD, T, x == y ? (T)~(T)0 : (T)0) \
    SW_BINF(sw_v_cmhi_##SFX, FIELD, T, x > y ? (T)~(T)0 : (T)0) \
    SW_BINF(sw_v_cmhs_##SFX, FIELD, T, x >= y ? (T)~(T)0 : (T)0) \
    SW_BINF(sw_v_cmgt_##SFX, FIELD, T, (ST)x > (ST)y ? (T)~(T)0 : (T)0) \
    SW_BINF(sw_v_cmge_##SFX, FIELD, T, (ST)x >= (ST)y ? (T)~(T)0 : (T)0) \
    SW_BINF(sw_v_cmtst_##SFX, FIELD, T, (x & y) ? (T)~(T)0 : (T)0) \
    SW_UNF(sw_v_abs_##SFX, FIELD, T, (ST)x < 0 ? (T)(0 - x) : x) \
    SW_UNF(sw_v_neg_##SFX, FIELD, T, (T)(0 - x)) \
    SW_UNF(sw_v_xtn_##SFX, FIELD, T, x) \
    static sw_v128 sw_v_addp_##SFX(sw_v128 a, sw_v128 b) { \
        sw_v128 r; T both[SW_LANES(FIELD, T) * 2]; int i, n = SW_LANES(FIELD, T); \
        for (i = 0; i < n; i++) { both[i] = a.FIELD[i]; both[n + i] = b.FIELD[i]; } \
        for (i = 0; i < n; i++) r.FIELD[i] = (T)(both[2 * i] + both[2 * i + 1]); \
        return r; } \
    static sw_v128 sw_v_shadd_##SFX(sw_v128 a, sw_v128 b) { \
        sw_v128 r; int i; \
        for (i = 0; i < SW_LANES(FIELD, T); i++) r.FIELD[i] = (T)(((SWIDE)(ST)a.FIELD[i] + (SWIDE)(ST)b.FIELD[i]) >> 1); \
        return r; } \
    static sw_v128 sw_v_uhadd_##SFX(sw_v128 a, sw_v128 b) { \
        sw_v128 r; int i; \
        for (i = 0; i < SW_LANES(FIELD, T); i++) r.FIELD[i] = (T)(((WIDE)a.FIELD[i] + (WIDE)b.FIELD[i]) >> 1); \
        return r; } \
    static sw_v128 sw_v_sabd_##SFX(sw_v128 a, sw_v128 b) { \
        sw_v128 r; int i; \
        for (i = 0; i < SW_LANES(FIELD, T); i++) { SWIDE x = (SWIDE)(ST)a.FIELD[i], y = (SWIDE)(ST)b.FIELD[i]; r.FIELD[i] = (T)(x > y ? x - y : y - x); } \
        return r; } \
    static sw_v128 sw_v_uabd_##SFX(sw_v128 a, sw_v128 b) { \
        sw_v128 r; int i; \
        for (i = 0; i < SW_LANES(FIELD, T); i++) { WIDE x = (WIDE)a.FIELD[i], y = (WIDE)b.FIELD[i]; r.FIELD[i] = (T)(x > y ? x - y : y - x); } \
        return r; } \
    static sw_v128 sw_v_sshl_##SFX(sw_v128 a, sw_v128 b) { \
        sw_v128 r; int i; \
        for (i = 0; i < SW_LANES(FIELD, T); i++) { int sh = (int)(s8)b.FIELD[i]; SWIDE v = (SWIDE)(ST)a.FIELD[i]; \
            if (sh < 0) { sh = -sh; r.FIELD[i] = (T)(sh >= (int)(sizeof(T) * 8) ? (v < 0 ? -1 : 0) : (WIDE)(v >> sh)); } \
            else { r.FIELD[i] = (T)(sh >= (int)(sizeof(T) * 8) ? 0 : (WIDE)(v << sh)); } } \
        return r; } \
    static sw_v128 sw_v_ushl_##SFX(sw_v128 a, sw_v128 b) { \
        sw_v128 r; int i; \
        for (i = 0; i < SW_LANES(FIELD, T); i++) { int sh = (int)(s8)b.FIELD[i]; WIDE v = (WIDE)a.FIELD[i]; \
            if (sh < 0) { sh = -sh; r.FIELD[i] = (T)(sh >= (int)(sizeof(T) * 8) ? 0 : (v >> sh)); } \
            else { r.FIELD[i] = (T)(sh >= (int)(sizeof(T) * 8) ? 0 : (v << sh)); } } \
        return r; } \
    static sw_v128 sw_v_mla_##SFX(sw_v128 d, sw_v128 a, sw_v128 b) { \
        sw_v128 r; int i; \
        for (i = 0; i < SW_LANES(FIELD, T); i++) r.FIELD[i] = (T)(d.FIELD[i] + a.FIELD[i] * b.FIELD[i]); \
        return r; } \
    static sw_v128 sw_v_mls_##SFX(sw_v128 d, sw_v128 a, sw_v128 b) { \
        sw_v128 r; int i; \
        for (i = 0; i < SW_LANES(FIELD, T); i++) r.FIELD[i] = (T)(d.FIELD[i] - a.FIELD[i] * b.FIELD[i]); \
        return r; }

SW_INT_OPS(8, b, u8, s8, u8, u32, s32)
SW_INT_OPS(16, h, u16, s16, u16, u32, s32)
SW_INT_OPS(32, s, u32, s32, u32, u64, s64)
SW_INT_OPS(64, d, u64, s64, u64, u64, s64)

static sw_v128 sw_v_and(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = a.b[i] & b.b[i]; return r; }
static sw_v128 sw_v_orr(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = a.b[i] | b.b[i]; return r; }
static sw_v128 sw_v_eor(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = a.b[i] ^ b.b[i]; return r; }
static sw_v128 sw_v_bic(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = a.b[i] & (u8)~b.b[i]; return r; }
static sw_v128 sw_v_orn(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = a.b[i] | (u8)~b.b[i]; return r; }
static sw_v128 sw_v_not(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = (u8)~a.b[i]; return r; }
static sw_v128 sw_v_mvn(sw_v128 a) { return sw_v_not(a); }
static sw_v128 sw_v_bsl(sw_v128 d, sw_v128 n, sw_v128 m) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = (d.b[i] & n.b[i]) | ((u8)~d.b[i] & m.b[i]); return r; }
static sw_v128 sw_v_bit(sw_v128 d, sw_v128 n, sw_v128 m) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = d.b[i] ^ ((d.b[i] ^ n.b[i]) & m.b[i]); return r; }
static sw_v128 sw_v_bif(sw_v128 d, sw_v128 n, sw_v128 m) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = d.b[i] ^ ((d.b[i] ^ n.b[i]) & (u8)~m.b[i]); return r; }

static sw_v128 sw_v_cnt_8(sw_v128 a) { sw_v128 r; int i, k; for (i = 0; i < 16; i++) { u8 v = a.b[i]; k = 0; while (v) { k += v & 1; v >>= 1; } r.b[i] = (u8)k; } return r; }
static sw_v128 sw_v_clz_8(sw_v128 a) { sw_v128 r; int i, k; for (i = 0; i < 16; i++) { u8 v = a.b[i]; k = 0; while (k < 8 && !(v & (0x80u >> k))) k++; r.b[i] = (u8)k; } return r; }
static sw_v128 sw_v_clz_16(sw_v128 a) { sw_v128 r; int i, k; for (i = 0; i < 8; i++) { u16 v = a.h[i]; k = 0; while (k < 16 && !(v & (0x8000u >> k))) k++; r.h[i] = (u16)k; } return r; }
static sw_v128 sw_v_clz_32(sw_v128 a) { sw_v128 r; int i, k; for (i = 0; i < 4; i++) { u32 v = a.s[i]; k = 0; while (k < 32 && !(v & (0x80000000u >> k))) k++; r.s[i] = (u32)k; } return r; }
/* REV64 reverses elements within each 64-bit half (rev64_8 used to swap byte pairs, i.e. REV16). */
static sw_v128 sw_v_rev64_8(sw_v128 a) { sw_v128 r; int i, j; for (i = 0; i < 2; i++) for (j = 0; j < 8; j++) r.b[8 * i + j] = a.b[8 * i + 7 - j]; return r; }
static sw_v128 sw_v_rev64_16(sw_v128 a) { sw_v128 r; int i, j; for (i = 0; i < 2; i++) for (j = 0; j < 4; j++) r.h[4 * i + j] = a.h[4 * i + 3 - j]; return r; }
static sw_v128 sw_v_rev64_32(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 2; i++) { r.s[2 * i] = a.s[2 * i + 1]; r.s[2 * i + 1] = a.s[2 * i]; } return r; }
static sw_v128 sw_v_rev32_16(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 4; i++) { r.h[2 * i] = a.h[2 * i + 1]; r.h[2 * i + 1] = a.h[2 * i]; } return r; }
static sw_v128 sw_v_rev32_8(sw_v128 a) { sw_v128 r; int i, j; for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) r.b[4 * i + j] = a.b[4 * i + 3 - j]; return r; }
static sw_v128 sw_v_rev16_8(sw_v128 a) { sw_v128 r; int i, j; for (i = 0; i < 8; i++) for (j = 0; j < 2; j++) r.b[2 * i + j] = a.b[2 * i + 1 - j]; return r; }

static sw_v128 sw_v_movi_16(u64 v) { sw_v128 r; int i; for (i = 0; i < 8; i++) r.h[i] = (u16)v; return r; }
static sw_v128 sw_v_movi_32(u64 v) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.s[i] = (u32)v; return r; }
static sw_v128 sw_v_movi_64(u64 v) { sw_v128 r; r.d[0] = v; r.d[1] = v; return r; }
static sw_v128 sw_v_movi_8(u64 v) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = (u8)v; return r; }

static sw_v128 sw_v_mvni_16(u64 v) { sw_v128 r; int i; for (i = 0; i < 8; i++) r.h[i] = (u16)~v; return r; }
static sw_v128 sw_v_mvni_32(u64 v) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.s[i] = (u32)~v; return r; }
static sw_v128 sw_v_mvni_64(u64 v) { sw_v128 r; r.d[0] = ~v; r.d[1] = ~v; return r; }
static sw_v128 sw_v_mvni_8(u64 v) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = (u8)~v; return r; }

static sw_v128 sw_v_zero_hi(sw_v128 a) { a.d[1] = 0; return a; }

/* x86 produces a negative "indefinite" NaN for invalid operations (0/0,
 * sqrt(-1), inf-inf); AArch64 produces the positive default NaN. */
static float sw_dn32(float x) { u32 b; memcpy(&b, &x, 4); if (b == 0xFFC00000u) b = 0x7FC00000u; memcpy(&x, &b, 4); return x; }
static double sw_dn64(double x) { u64 b; memcpy(&b, &x, 8); if (b == 0xFFF8000000000000ull) b = 0x7FF8000000000000ull; memcpy(&x, &b, 8); return x; }

/* FMAXP / FMINP (vector): pairwise over n lanes, results of a then b. */
#define SW_FPAIR(NAME, T, FIELD, OP) \
    static sw_v128 NAME(sw_v128 a, sw_v128 b, int n) { \
        sw_v128 r; int i; memset(&r, 0, sizeof r); \
        for (i = 0; i < n / 2; i++) { \
            r.FIELD[i] = OP(a.FIELD[2 * i], a.FIELD[2 * i + 1]); \
            r.FIELD[n / 2 + i] = OP(b.FIELD[2 * i], b.FIELD[2 * i + 1]); } \
        return r; }

static sw_v128 sw_v_fmla_32(sw_v128 d, sw_v128 a, sw_v128 b)
{
    sw_v128 r; int i;
    for (i = 0; i < 4; i++) r.f[i] = sw_dn32(fmaf(a.f[i], b.f[i], d.f[i]));   /* fused, as FMLA */
    return r;
}
static sw_v128 sw_v_fmla_64(sw_v128 d, sw_v128 a, sw_v128 b)
{
    sw_v128 r; int i;
    for (i = 0; i < 2; i++) r.df[i] = sw_dn64(fma(a.df[i], b.df[i], d.df[i]));
    return r;
}
static sw_v128 sw_v_fmls_32(sw_v128 d, sw_v128 a, sw_v128 b)
{
    sw_v128 r; int i;
    for (i = 0; i < 4; i++) r.f[i] = sw_dn32(fmaf(-a.f[i], b.f[i], d.f[i]));  /* fused, as FMLS */
    return r;
}
static sw_v128 sw_v_fmls_64(sw_v128 d, sw_v128 a, sw_v128 b)
{
    sw_v128 r; int i;
    for (i = 0; i < 2; i++) r.df[i] = sw_dn64(fma(-a.df[i], b.df[i], d.df[i]));
    return r;
}

static sw_v128 sw_v_dup_gpr_8(u64 v) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = (u8)v; return r; }
static sw_v128 sw_v_dup_gpr_16(u64 v) { sw_v128 r; int i; for (i = 0; i < 8; i++) r.h[i] = (u16)v; return r; }
static sw_v128 sw_v_dup_gpr_32(u64 v) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.s[i] = (u32)v; return r; }
static sw_v128 sw_v_dup_gpr_64(u64 v) { sw_v128 r; r.d[0] = v; r.d[1] = v; return r; }

static sw_v128 sw_v_dup_lane_8(sw_v128 a, int lane) { sw_v128 r; int i; for (i = 0; i < 16; i++) r.b[i] = a.b[lane]; return r; }
static sw_v128 sw_v_dup_lane_16(sw_v128 a, int lane) { sw_v128 r; int i; for (i = 0; i < 8; i++) r.h[i] = a.h[lane]; return r; }
static sw_v128 sw_v_dup_lane_32(sw_v128 a, int lane) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.s[i] = a.s[lane]; return r; }
static sw_v128 sw_v_dup_lane_64(sw_v128 a, int lane) { sw_v128 r; r.d[0] = a.d[lane]; r.d[1] = a.d[lane]; return r; }

static u64 sw_vlane_u_8(sw_v128 a, int lane) { return a.b[lane]; }
static u64 sw_vlane_u_16(sw_v128 a, int lane) { return a.h[lane]; }
static u64 sw_vlane_u_32(sw_v128 a, int lane) { return a.s[lane]; }
static u64 sw_vlane_u_64(sw_v128 a, int lane) { return a.d[lane]; }
static u64 sw_vlane_s_8(sw_v128 a, int lane) { return (u64)(s64)(s8)a.b[lane]; }
static u64 sw_vlane_s_16(sw_v128 a, int lane) { return (u64)(s64)(s16)a.h[lane]; }
static u64 sw_vlane_s_32(sw_v128 a, int lane) { return (u64)(s64)(s32)a.s[lane]; }
static u64 sw_vlane_s_64(sw_v128 a, int lane) { return a.d[lane]; }

static void sw_vgpr_to_lane_8(sw_v128 *d, int lane, u64 v) { d->b[lane] = (u8)v; }
static void sw_vgpr_to_lane_16(sw_v128 *d, int lane, u64 v) { d->h[lane] = (u16)v; }
static void sw_vgpr_to_lane_32(sw_v128 *d, int lane, u64 v) { d->s[lane] = (u32)v; }
static void sw_vgpr_to_lane_64(sw_v128 *d, int lane, u64 v) { d->d[lane] = v; }
static void sw_vlane_copy_8(sw_v128 *d, sw_v128 s, int lane) { d->b[0] = s.b[lane]; }
static void sw_vlane_copy_16(sw_v128 *d, sw_v128 s, int lane) { d->h[0] = s.h[lane]; }
static void sw_vlane_copy_32(sw_v128 *d, sw_v128 s, int lane) { d->s[0] = s.s[lane]; }
static void sw_vlane_copy_64(sw_v128 *d, sw_v128 s, int lane) { d->d[0] = s.d[lane]; }

#define SW_FP_BIN(NAME, FIELD, T, EXPR) \
    static sw_v128 NAME(sw_v128 a, sw_v128 b) { \
        sw_v128 r; int i; \
        for (i = 0; i < SW_LANES(FIELD, T); i++) { T x = a.FIELD[i], y = b.FIELD[i]; r.FIELD[i] = (T)(EXPR); } \
        return r; }

/* invalid operations give AArch64's positive default NaN (sw_dn), not x86's negative one:
 * a singular 4x4 inverse (main 0x2a8b4e0, 0 * inf) left -NaN where the game has +NaN */
SW_FP_BIN(sw_v_fadd_32, f, float, sw_dn32(x + y))
SW_FP_BIN(sw_v_fadd_64, df, double, sw_dn64(x + y))
SW_FP_BIN(sw_v_fsub_32, f, float, sw_dn32(x - y))
SW_FP_BIN(sw_v_fsub_64, df, double, sw_dn64(x - y))
SW_FP_BIN(sw_v_fmul_32, f, float, sw_dn32(x * y))
SW_FP_BIN(sw_v_fmul_64, df, double, sw_dn64(x * y))
SW_FP_BIN(sw_v_fdiv_32, f, float, sw_dn32(x / y))
SW_FP_BIN(sw_v_fdiv_64, df, double, sw_dn64(x / y))
SW_FP_BIN(sw_v_fmax_32, f, float, sw_fmax32(x, y))
SW_FP_BIN(sw_v_fmax_64, df, double, sw_fmax64(x, y))
SW_FP_BIN(sw_v_fmin_32, f, float, sw_fmin32(x, y))
SW_FP_BIN(sw_v_fmin_64, df, double, sw_fmin64(x, y))

static sw_v128 sw_v_fcmeq_32(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.s[i] = a.f[i] == b.f[i] ? 0xFFFFFFFFu : 0; return r; }
static sw_v128 sw_v_fcmeq_64(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.d[i] = a.df[i] == b.df[i] ? ~0ull : 0; return r; }
static sw_v128 sw_v_fcmge_32(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.s[i] = a.f[i] >= b.f[i] ? 0xFFFFFFFFu : 0; return r; }
static sw_v128 sw_v_fcmge_64(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.d[i] = a.df[i] >= b.df[i] ? ~0ull : 0; return r; }
static sw_v128 sw_v_fcmgt_32(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.s[i] = a.f[i] > b.f[i] ? 0xFFFFFFFFu : 0; return r; }
static sw_v128 sw_v_fcmgt_64(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.d[i] = a.df[i] > b.df[i] ? ~0ull : 0; return r; }

static sw_v128 sw_v_frintn_32(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.f[i] = (float)sw_rint64((double)a.f[i]); return r; }
static sw_v128 sw_v_frintn_64(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.df[i] = sw_rint64(a.df[i]); return r; }
static sw_v128 sw_v_frintm_32(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.f[i] = floorf(a.f[i]); return r; }
static sw_v128 sw_v_frintm_64(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.df[i] = floor(a.df[i]); return r; }
static sw_v128 sw_v_frintp_32(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.f[i] = ceilf(a.f[i]); return r; }
static sw_v128 sw_v_frintp_64(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.df[i] = ceil(a.df[i]); return r; }
static sw_v128 sw_v_frintz_32(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.f[i] = truncf(a.f[i]); return r; }
static sw_v128 sw_v_frintz_64(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.df[i] = trunc(a.df[i]); return r; }
static sw_v128 sw_v_frinta_32(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.f[i] = (float)sw_round64((double)a.f[i]); return r; }
static sw_v128 sw_v_frinta_64(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.df[i] = sw_round64(a.df[i]); return r; }

static sw_v128 sw_v_scvtf_32(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.f[i] = (float)(s32)a.s[i]; return r; }
static sw_v128 sw_v_scvtf_64(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.df[i] = (double)(s64)a.d[i]; return r; }
static sw_v128 sw_v_ucvtf_32(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.f[i] = (float)a.s[i]; return r; }
static sw_v128 sw_v_ucvtf_64(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.df[i] = (double)a.d[i]; return r; }
static sw_v128 sw_v_fcvtzs_32(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.s[i] = (u32)(s32)sw_sat_s32(trunc((double)a.f[i])); return r; }
static sw_v128 sw_v_fcvtzs_64(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.d[i] = (u64)sw_sat_s64(trunc(a.df[i])); return r; }
static sw_v128 sw_v_fcvtzu_32(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 4; i++) r.s[i] = sw_sat_u32(trunc((double)a.f[i])); return r; }
static sw_v128 sw_v_fcvtzu_64(sw_v128 a) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.d[i] = sw_sat_u64(trunc(a.df[i])); return r; }

/* Permutes over the first n lanes (n = 16/8/4/2 for a 128-bit form, half that
 * for the 64-bit .8B/.4H/.2S forms, whose upper half is zeroed).  These used
 * to always work on all lanes, so 64-bit forms read the wrong half, and
 * TRN1/TRN2 were defined as ZIP1/ZIP2. */
#define SW_PERM(SFX, FIELD, T) \
    static sw_v128 sw_v_zip1_##SFX(sw_v128 a, sw_v128 b, int idx, int n) { \
        sw_v128 r; int i; (void)idx; memset(&r, 0, sizeof r); \
        for (i = 0; i < n / 2; i++) { r.FIELD[2 * i] = a.FIELD[i]; r.FIELD[2 * i + 1] = b.FIELD[i]; } \
        return r; } \
    static sw_v128 sw_v_zip2_##SFX(sw_v128 a, sw_v128 b, int idx, int n) { \
        sw_v128 r; int i; (void)idx; memset(&r, 0, sizeof r); \
        for (i = 0; i < n / 2; i++) { r.FIELD[2 * i] = a.FIELD[n / 2 + i]; r.FIELD[2 * i + 1] = b.FIELD[n / 2 + i]; } \
        return r; } \
    static sw_v128 sw_v_uzp1_##SFX(sw_v128 a, sw_v128 b, int idx, int n) { \
        sw_v128 r; T both[32]; int i; (void)idx; memset(&r, 0, sizeof r); \
        for (i = 0; i < n; i++) { both[i] = a.FIELD[i]; both[n + i] = b.FIELD[i]; } \
        for (i = 0; i < n; i++) r.FIELD[i] = both[2 * i]; \
        return r; } \
    static sw_v128 sw_v_uzp2_##SFX(sw_v128 a, sw_v128 b, int idx, int n) { \
        sw_v128 r; T both[32]; int i; (void)idx; memset(&r, 0, sizeof r); \
        for (i = 0; i < n; i++) { both[i] = a.FIELD[i]; both[n + i] = b.FIELD[i]; } \
        for (i = 0; i < n; i++) r.FIELD[i] = both[2 * i + 1]; \
        return r; } \
    static sw_v128 sw_v_trn1_##SFX(sw_v128 a, sw_v128 b, int idx, int n) { \
        sw_v128 r; int i; (void)idx; memset(&r, 0, sizeof r); \
        for (i = 0; i < n / 2; i++) { r.FIELD[2 * i] = a.FIELD[2 * i]; r.FIELD[2 * i + 1] = b.FIELD[2 * i]; } \
        return r; } \
    static sw_v128 sw_v_trn2_##SFX(sw_v128 a, sw_v128 b, int idx, int n) { \
        sw_v128 r; int i; (void)idx; memset(&r, 0, sizeof r); \
        for (i = 0; i < n / 2; i++) { r.FIELD[2 * i] = a.FIELD[2 * i + 1]; r.FIELD[2 * i + 1] = b.FIELD[2 * i + 1]; } \
        return r; } \
    static sw_v128 sw_v_ext_##SFX(sw_v128 a, sw_v128 b, int idx, int n) { \
        sw_v128 r; T both[32]; int i; memset(&r, 0, sizeof r); \
        for (i = 0; i < n; i++) { both[i] = a.FIELD[i]; both[n + i] = b.FIELD[i]; } \
        for (i = 0; i < n; i++) r.FIELD[i] = both[idx + i]; \
        return r; }

SW_PERM(8, b, u8)
SW_PERM(16, h, u16)
SW_PERM(32, s, u32)
SW_PERM(64, d, u64)

SW_FPAIR(sw_v_fmaxp_32, float, f, sw_fmax32)
SW_FPAIR(sw_v_fminp_32, float, f, sw_fmin32)
SW_FPAIR(sw_v_fmaxp_64, double, df, sw_fmax64)
SW_FPAIR(sw_v_fminp_64, double, df, sw_fmin64)


/* ---- ARMv8 crypto extension: SHA1 / SHA256 (Arm ARM pseudocode) ------- */
static u32 sw_rol32(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
static u32 sw_ror32c(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
static sw_v128 sw_sha1_hash(sw_v128 x, sw_v128 yv, sw_v128 w, int kind)
{
    u32 y = yv.s[0]; int e;
    for (e = 0; e < 4; e++) {
        u32 b = x.s[1], c = x.s[2], d = x.s[3], t;
        if (kind == 0) t = ((c ^ d) & b) ^ d;               /* SHA1C: choose */
        else if (kind == 1) t = b ^ c ^ d;                  /* SHA1P: parity */
        else t = (b & c) | ((b | c) & d);                   /* SHA1M: majority */
        y = y + sw_rol32(x.s[0], 5) + t + w.s[e];
        x.s[1] = sw_rol32(x.s[1], 30);
        { u32 top = x.s[3]; x.s[3] = x.s[2]; x.s[2] = x.s[1]; x.s[1] = x.s[0]; x.s[0] = y; y = top; }
    }
    return x;
}
static sw_v128 sw_sha1c(sw_v128 d, sw_v128 n, sw_v128 m) { return sw_sha1_hash(d, n, m, 0); }
static sw_v128 sw_sha1p(sw_v128 d, sw_v128 n, sw_v128 m) { return sw_sha1_hash(d, n, m, 1); }
static sw_v128 sw_sha1m(sw_v128 d, sw_v128 n, sw_v128 m) { return sw_sha1_hash(d, n, m, 2); }
static sw_v128 sw_sha1h(sw_v128 n) { sw_v128 r; memset(&r, 0, sizeof r); r.s[0] = sw_rol32(n.s[0], 30); return r; }
static sw_v128 sw_sha1su0(sw_v128 d, sw_v128 n, sw_v128 m)
{
    sw_v128 r; r.d[0] = d.d[1] ^ d.d[0] ^ m.d[0]; r.d[1] = n.d[0] ^ d.d[1] ^ m.d[1]; return r;
}
static sw_v128 sw_sha1su1(sw_v128 d, sw_v128 n)
{
    u32 t0 = d.s[0] ^ n.s[1], t1 = d.s[1] ^ n.s[2], t2 = d.s[2] ^ n.s[3], t3 = d.s[3];
    sw_v128 r;
    r.s[0] = sw_rol32(t0, 1); r.s[1] = sw_rol32(t1, 1); r.s[2] = sw_rol32(t2, 1);
    r.s[3] = sw_rol32(t3, 1) ^ sw_rol32(t0, 2);
    return r;
}
static sw_v128 sw_sha256_hash(sw_v128 x, sw_v128 y, sw_v128 w, int part1)
{
    int e;
    for (e = 0; e < 4; e++) {
        u32 chs = ((y.s[1] ^ y.s[2]) & y.s[0]) ^ y.s[2];
        u32 maj = (x.s[0] & x.s[1]) | ((x.s[0] | x.s[1]) & x.s[2]);
        u32 t1 = y.s[3] + (sw_ror32c(y.s[0], 6) ^ sw_ror32c(y.s[0], 11) ^ sw_ror32c(y.s[0], 25)) + chs + w.s[e];
        u32 sig0 = sw_ror32c(x.s[0], 2) ^ sw_ror32c(x.s[0], 13) ^ sw_ror32c(x.s[0], 22);
        x.s[3] = t1 + x.s[3];
        y.s[3] = t1 + sig0 + maj;
        { /* <Y, X> = ROL(Y:X, 32) */
            u32 ytop = y.s[3], xtop = x.s[3];
            y.s[3] = y.s[2]; y.s[2] = y.s[1]; y.s[1] = y.s[0]; y.s[0] = xtop;
            x.s[3] = x.s[2]; x.s[2] = x.s[1]; x.s[1] = x.s[0]; x.s[0] = ytop;
        }
    }
    return part1 ? x : y;
}
static sw_v128 sw_sha256h(sw_v128 d, sw_v128 n, sw_v128 m) { return sw_sha256_hash(d, n, m, 1); }
static sw_v128 sw_sha256h2(sw_v128 d, sw_v128 n, sw_v128 m) { return sw_sha256_hash(n, d, m, 0); }
static sw_v128 sw_sha256su0(sw_v128 d, sw_v128 n)
{
    u32 t[4] = { d.s[1], d.s[2], d.s[3], n.s[0] }; sw_v128 r; int e;
    for (e = 0; e < 4; e++) r.s[e] = (sw_ror32c(t[e], 7) ^ sw_ror32c(t[e], 18) ^ (t[e] >> 3)) + d.s[e];
    return r;
}
static sw_v128 sw_sha256su1(sw_v128 d, sw_v128 n, sw_v128 m)
{
    u32 t0[4] = { n.s[1], n.s[2], n.s[3], m.s[0] }; sw_v128 r; int e;
    for (e = 0; e < 2; e++) {
        u32 x = m.s[2 + e];
        r.s[e] = (sw_ror32c(x, 17) ^ sw_ror32c(x, 19) ^ (x >> 10)) + d.s[e] + t0[e];
    }
    for (e = 2; e < 4; e++) {
        u32 x = r.s[e - 2];
        r.s[e] = (sw_ror32c(x, 17) ^ sw_ror32c(x, 19) ^ (x >> 10)) + d.s[e] + t0[e];
    }
    return r;
}


/* ---- SIMD batch 4: by-element FP, reductions, FP two-reg misc, TBL ----- */
static float sw_fadd32_(float a, float b) { return sw_dn32(a + b); }
static double sw_fadd64_(double a, double b) { return sw_dn64(a + b); }
SW_FPAIR(sw_v_faddp_32, float, f, sw_fadd32_)
SW_FPAIR(sw_v_faddp_64, double, df, sw_fadd64_)

/* FMUL/FMLA/FMLS (by element): Vd[i] (op)= Vn[i] * Vm[idx] for n lanes */
#define SW_FELEM(SFX, T, F, FMA) \
    static sw_v128 sw_v_fmul_elem_##SFX(sw_v128 d, sw_v128 a, sw_v128 m, int idx, int n) { \
        sw_v128 r; int i; T s = m.F[idx]; (void)d; memset(&r, 0, sizeof r); \
        for (i = 0; i < n; i++) r.F[i] = sw_dn##SFX(a.F[i] * s); return r; } \
    static sw_v128 sw_v_fmla_elem_##SFX(sw_v128 d, sw_v128 a, sw_v128 m, int idx, int n) { \
        sw_v128 r; int i; T s = m.F[idx]; memset(&r, 0, sizeof r); \
        for (i = 0; i < n; i++) r.F[i] = sw_dn##SFX(FMA(a.F[i], s, d.F[i])); return r; } \
    static sw_v128 sw_v_fmls_elem_##SFX(sw_v128 d, sw_v128 a, sw_v128 m, int idx, int n) { \
        sw_v128 r; int i; T s = m.F[idx]; memset(&r, 0, sizeof r); \
        for (i = 0; i < n; i++) r.F[i] = sw_dn##SFX(FMA(-a.F[i], s, d.F[i])); return r; }
SW_FELEM(32, float, f, fmaf)
SW_FELEM(64, double, df, fma)

/* ADDV: sum of n lanes into element 0, rest zero */
#define SW_ADDV(SFX, T, F) \
    static sw_v128 sw_v_addv_##SFX(sw_v128 a, int n) { \
        sw_v128 r; int i; T s = 0; memset(&r, 0, sizeof r); \
        for (i = 0; i < n; i++) s = (T)(s + a.F[i]); r.F[0] = s; return r; }
SW_ADDV(8, u8, b)
SW_ADDV(16, u16, h)
SW_ADDV(32, u32, s)

/* FP two-register misc over n lanes (upper lanes zeroed) */
#define SW_VF1(NAME, T, F, EXPR) \
    static sw_v128 NAME(sw_v128 a, int n) { sw_v128 r; int i; memset(&r, 0, sizeof r); \
        for (i = 0; i < n; i++) { T x = a.F[i]; r.F[i] = (EXPR); } return r; }
#define SW_VFCMP(NAME, T, F, U, UF, COND) \
    static sw_v128 NAME(sw_v128 a, int n) { sw_v128 r; int i; memset(&r, 0, sizeof r); \
        for (i = 0; i < n; i++) { T x = a.F[i]; r.UF[i] = (COND) ? (U)~(U)0 : 0; } return r; }
SW_VF1(sw_v_fabs2_32, float, f, fabsf(x))
SW_VF1(sw_v_fabs2_64, double, df, fabs(x))
SW_VF1(sw_v_fneg2_32, float, f, -x)
SW_VF1(sw_v_fneg2_64, double, df, -x)
SW_VF1(sw_v_fsqrt2_32, float, f, sw_dn32(sqrtf(x)))
SW_VF1(sw_v_fsqrt2_64, double, df, sw_dn64(sqrt(x)))
/* FRECPE / FRSQRTE: the Arm ARM 8-bit estimate algorithms (RecipEstimate,
 * RecipSqrtEstimate) so results match hardware bit-for-bit for normal inputs.
 * Denormal inputs use a full-precision fallback. */
static u32 sw_recip_est(u32 a) { a = a * 2 + 1; return ((1u << 19) / a + 1) / 2; }
/* RecipSqrtEstimate: sw_rsqrt_est_loop is the Arm ARM search (up to ~500
 * iterations, 3.7% of all CPU time in the village); sw_rsqrt_tab holds its result
 * for every input below 512 (callers pass 128..511), generated from that loop and
 * checked for all inputs by Engine/Tests/test_rsqrt_table.c. */
static u32 sw_rsqrt_est_loop(u32 a)
{
    u64 b = 512;
    if (a < 256) a = a * 2 + 1; else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    while ((u64)a * (b + 1) * (b + 1) < (1ull << 28)) b++;
    return (u32)((b + 1) / 2);
}
static const unsigned short sw_rsqrt_tab[512] = {
    8192,4730,3664,3096,2731,2470,2272,2115,1987,1879,1788,1708,1638,1577,1521,1471,
    1426,1385,1347,1312,1279,1249,1221,1195,1170,1147,1125,1105,1085,1067,1049,1032,
    1016,1001,986,972,959,946,934,922,910,899,889,878,868,859,849,840,
    832,823,815,807,799,792,785,778,771,764,757,751,745,739,733,727,
    721,716,710,705,700,695,690,685,680,676,671,667,662,658,654,650,
    646,642,638,634,630,626,623,619,616,612,609,606,602,599,596,593,
    590,587,584,581,578,575,572,569,567,564,561,559,556,554,551,549,
    546,544,541,539,537,534,532,530,528,526,523,521,519,517,515,513,
    511,509,507,505,503,501,499,498,496,494,492,490,489,487,485,484,
    482,480,479,477,475,474,472,471,469,468,466,465,463,462,460,459,
    457,456,454,453,452,450,449,448,446,445,444,442,441,440,439,437,
    436,435,434,432,431,430,429,428,426,425,424,423,422,421,420,419,
    418,416,415,414,413,412,411,410,409,408,407,406,405,404,403,402,
    401,400,399,398,397,396,396,395,394,393,392,391,390,389,388,387,
    387,386,385,384,383,382,382,381,380,379,378,377,377,376,375,374,
    374,373,372,371,370,370,369,368,367,367,366,365,365,364,363,362,
    361,361,360,360,359,359,357,357,356,356,355,355,353,353,352,352,
    351,351,349,349,348,348,347,347,346,346,344,344,343,343,342,342,
    341,341,340,340,338,338,337,337,336,336,335,335,334,334,333,333,
    332,332,331,331,330,330,328,328,327,327,326,326,325,325,324,324,
    323,323,322,322,321,321,320,320,319,319,318,318,317,317,316,316,
    316,316,315,315,314,314,313,313,312,312,311,311,310,310,309,309,
    308,308,307,307,307,307,306,306,305,305,304,304,303,303,302,302,
    302,302,301,301,300,300,299,299,298,298,298,298,297,297,296,296,
    295,295,294,294,294,294,293,293,292,292,291,291,291,291,290,290,
    289,289,289,289,288,288,287,287,286,286,286,286,285,285,284,284,
    284,284,283,283,282,282,282,282,281,281,280,280,280,280,279,279,
    278,278,278,278,277,277,276,276,276,276,275,275,275,275,274,274,
    273,273,273,273,272,272,272,272,271,271,270,270,270,270,269,269,
    269,269,268,268,267,267,267,267,266,266,266,266,265,265,265,265,
    264,264,264,264,263,263,262,262,262,262,261,261,261,261,260,260,
    260,260,259,259,259,259,258,258,258,258,257,257,257,257,256,256,
};
static u32 sw_rsqrt_est(u32 a) { return a < 512 ? sw_rsqrt_tab[a] : sw_rsqrt_est_loop(a); }
static float sw_frecpe32(float x)
{
    u32 v, sign, e, frac, r, re; memcpy(&v, &x, 4);
    sign = v & 0x80000000u; e = (v >> 23) & 0xFF; frac = v & 0x7FFFFF;
    if (e == 0xFF) { if (frac) return sw_dn32(x + x); v = sign; memcpy(&x, &v, 4); return x; }
    if (e == 0 && frac && sw_fz()) frac = 0;                     /* FZ: a denormal input is zero */
    if (e == 0) { if (!frac) { v = sign | 0x7F800000u; memcpy(&x, &v, 4); return x; } return 1.0f / x; }
    if (e >= 253 && sw_fz()) { v = sign; memcpy(&x, &v, 4); return x; }   /* FZ: denormal result -> 0 */
    r = sw_recip_est(0x100 | (frac >> 15));
    re = 253 - e;
    frac = (r & 0xFF) << 15;
    if ((s32)re <= 0) { frac = (0x800000u | frac) >> (1 - (s32)re); re = 0; }
    v = sign | (re << 23) | frac; memcpy(&x, &v, 4); return x;
}
static double sw_frecpe64(double x)
{
    u64 v, sign, e, frac, r, re; memcpy(&v, &x, 8);
    sign = v & 0x8000000000000000ull; e = (v >> 52) & 0x7FF; frac = v & 0xFFFFFFFFFFFFFull;
    if (e == 0x7FF) { if (frac) return sw_dn64(x + x); v = sign; memcpy(&x, &v, 8); return x; }
    if (e == 0 && frac && sw_fz()) frac = 0;
    if (e == 0) { if (!frac) { v = sign | 0x7FF0000000000000ull; memcpy(&x, &v, 8); return x; } return 1.0 / x; }
    if (e >= 2045 && sw_fz()) { v = sign; memcpy(&x, &v, 8); return x; }
    r = sw_recip_est((u32)(0x100 | (frac >> 44)));
    re = 2045 - e;
    frac = (r & 0xFF) << 44;
    if ((s64)re <= 0) { frac = (0x10000000000000ull | frac) >> (1 - (s64)re); re = 0; }
    v = sign | (re << 52) | frac; memcpy(&x, &v, 8); return x;
}
static float sw_frsqrte32(float x)
{
    u32 v, e, frac, sc, r; memcpy(&v, &x, 4);
    e = (v >> 23) & 0xFF; frac = v & 0x7FFFFF;
    if (e == 0xFF && frac) return sw_dn32(x + x);
    if (e == 0 && frac && sw_fz()) frac = 0, v &= 0x80000000u;     /* FZ: denormal input is (signed) zero */
    if (e == 0 && !frac) { v = (v & 0x80000000u) | 0x7F800000u; memcpy(&x, &v, 4); return x; }
    if (v & 0x80000000u) { v = 0x7FC00000u; memcpy(&x, &v, 4); return x; }   /* negative: default NaN */
    if (e == 0xFF) return 0.0f;
    if (e == 0) return 1.0f / sqrtf(x);
    sc = (e & 1) ? (0x80 | (frac >> 16)) : (0x100 | (frac >> 15));   /* even exp: 1:frac, odd: 01:frac */
    r = sw_rsqrt_est(sc);
    v = (((380 - e) / 2) << 23) | ((r & 0xFF) << 15); memcpy(&x, &v, 4); return x;
}
static double sw_frsqrte64(double x)
{
    u64 v, e, frac, sc, r; memcpy(&v, &x, 8);
    e = (v >> 52) & 0x7FF; frac = v & 0xFFFFFFFFFFFFFull;
    if (e == 0x7FF && frac) return sw_dn64(x + x);
    if (e == 0 && frac && sw_fz()) frac = 0, v &= 0x8000000000000000ull;
    if (e == 0 && !frac) { v = (v & 0x8000000000000000ull) | 0x7FF0000000000000ull; memcpy(&x, &v, 8); return x; }
    if (v & 0x8000000000000000ull) { v = 0x7FF8000000000000ull; memcpy(&x, &v, 8); return x; }
    if (e == 0x7FF) return 0.0;
    if (e == 0) return 1.0 / sqrt(x);
    sc = (e & 1) ? (0x80 | (frac >> 45)) : (0x100 | (frac >> 44));
    r = sw_rsqrt_est((u32)sc);
    v = (((3068 - e) / 2) << 52) | ((r & 0xFF) << 44); memcpy(&x, &v, 8); return x;
}
SW_VF1(sw_v_frecpe2_32, float, f, sw_frecpe32(x))
SW_VF1(sw_v_frecpe2_64, double, df, sw_frecpe64(x))
SW_VF1(sw_v_frsqrte2_32, float, f, sw_frsqrte32(x))
SW_VF1(sw_v_frsqrte2_64, double, df, sw_frsqrte64(x))
SW_VFCMP(sw_v_fcmeqz_32, float, f, u32, s, x == 0.0f)
SW_VFCMP(sw_v_fcmeqz_64, double, df, u64, d, x == 0.0)
SW_VFCMP(sw_v_fcmgtz_32, float, f, u32, s, x > 0.0f)
SW_VFCMP(sw_v_fcmgtz_64, double, df, u64, d, x > 0.0)
SW_VFCMP(sw_v_fcmgez_32, float, f, u32, s, x >= 0.0f)
SW_VFCMP(sw_v_fcmgez_64, double, df, u64, d, x >= 0.0)
SW_VFCMP(sw_v_fcmltz_32, float, f, u32, s, x < 0.0f)
SW_VFCMP(sw_v_fcmltz_64, double, df, u64, d, x < 0.0)
SW_VFCMP(sw_v_fcmlez_32, float, f, u32, s, x <= 0.0f)
SW_VFCMP(sw_v_fcmlez_64, double, df, u64, d, x <= 0.0)
static sw_v128 sw_v_scvtf2_32(sw_v128 a, int n) { sw_v128 r; int i; memset(&r, 0, sizeof r); for (i = 0; i < n; i++) r.f[i] = (float)(s32)a.s[i]; return r; }
static sw_v128 sw_v_scvtf2_64(sw_v128 a, int n) { sw_v128 r; int i; memset(&r, 0, sizeof r); for (i = 0; i < n; i++) r.df[i] = (double)(s64)a.d[i]; return r; }
static sw_v128 sw_v_ucvtf2_32(sw_v128 a, int n) { sw_v128 r; int i; memset(&r, 0, sizeof r); for (i = 0; i < n; i++) r.f[i] = (float)a.s[i]; return r; }
static sw_v128 sw_v_ucvtf2_64(sw_v128 a, int n) { sw_v128 r; int i; memset(&r, 0, sizeof r); for (i = 0; i < n; i++) r.df[i] = (double)a.d[i]; return r; }

/* FRECPS / FRSQRTS (exact): 2 - a*b and (3 - a*b) / 2, fused.  Arm special
 * case: infinity times zero (either order) gives exactly 2.0 / 1.5, not NaN -
 * the Newton step after FRECPE/FRSQRTE(0) = inf, e.g. normalising a zero
 * vector.  NaN operands are handled first (they propagate). */
#define SW_INF0(x, y) ((isinf(x) && (y) == 0) || ((x) == 0 && isinf(y)))
/* f32 forms in double: exact product, one rounding (fused, no false overflow) */
static sw_v128 sw_v_frecps_32(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 4; i++) { float x = sw_ftz32(a.f[i]), y = sw_ftz32(b.f[i]); r.f[i] = (!isnan(x) && !isnan(y) && SW_INF0(x, y)) ? 2.0f : sw_dn32((float)(2.0 - (double)x * (double)y)); } return r; }
static sw_v128 sw_v_frecps_64(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.df[i] = (!isnan(a.df[i]) && !isnan(b.df[i]) && SW_INF0(a.df[i], b.df[i])) ? 2.0 : fma(-a.df[i], b.df[i], 2.0); return r; }
static sw_v128 sw_v_frsqrts_32(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 4; i++) { float x = sw_ftz32(a.f[i]), y = sw_ftz32(b.f[i]); r.f[i] = (!isnan(x) && !isnan(y) && SW_INF0(x, y)) ? 1.5f : sw_dn32((float)((3.0 - (double)x * (double)y) * 0.5)); } return r; }
static sw_v128 sw_v_frsqrts_64(sw_v128 a, sw_v128 b) { sw_v128 r; int i; for (i = 0; i < 2; i++) r.df[i] = (!isnan(a.df[i]) && !isnan(b.df[i]) && SW_INF0(a.df[i], b.df[i])) ? 1.5 : fma(-a.df[i], b.df[i], 3.0) * 0.5; return r; }
/* the low 64 bits of a and of b side by side: 64-bit (.8B/.4H/.2S) pairwise ops */
static sw_v128 sw_v_catlo(sw_v128 a, sw_v128 b) { sw_v128 r; r.d[0] = a.d[0]; r.d[1] = b.d[0]; return r; }

/* TBL: byte table lookup over n consecutive registers; out-of-range -> 0 */
static sw_v128 sw_v_tbl(const sw_v128 *tab, int n, sw_v128 idx, int q)
{
    sw_v128 r; int i; memset(&r, 0, sizeof r);
    for (i = 0; i < (q ? 16 : 8); i++) {
        unsigned k = idx.b[i];
        r.b[i] = k < 16u * (unsigned)n ? tab[k >> 4].b[k & 15] : 0;
    }
    return r;
}


/* ---- ARMv8 AES: AESE/AESD/AESMC/AESIMC (Arm ARM pseudocode) ----------- */
static const u8 sw_aes_sbox[256] = {0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
static const u8 sw_aes_isbox[256] = {0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d};

static u8 sw_aes_xt(u8 x) { return (u8)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
static u8 sw_aes_mul(u8 a, u8 b) { u8 r = 0; while (b) { if (b & 1) r ^= a; a = sw_aes_xt(a); b >>= 1; } return r; }
static sw_v128 sw_aese(sw_v128 d, sw_v128 n)
{
    sw_v128 s, r; int i, c, rr;
    for (i = 0; i < 16; i++) s.b[i] = d.b[i] ^ n.b[i];
    for (c = 0; c < 4; c++) for (rr = 0; rr < 4; rr++)       /* ShiftRows + SubBytes */
        r.b[4 * c + rr] = sw_aes_sbox[s.b[4 * ((c + rr) & 3) + rr]];
    return r;
}
static sw_v128 sw_aesd(sw_v128 d, sw_v128 n)
{
    sw_v128 s, r; int i, c, rr;
    for (i = 0; i < 16; i++) s.b[i] = d.b[i] ^ n.b[i];
    for (c = 0; c < 4; c++) for (rr = 0; rr < 4; rr++)       /* InvShiftRows + InvSubBytes */
        r.b[4 * ((c + rr) & 3) + rr] = sw_aes_isbox[s.b[4 * c + rr]];
    return r;
}
static sw_v128 sw_aesmc(sw_v128 n)
{
    sw_v128 r; int c;
    for (c = 0; c < 4; c++) {
        u8 a0 = n.b[4 * c], a1 = n.b[4 * c + 1], a2 = n.b[4 * c + 2], a3 = n.b[4 * c + 3];
        r.b[4 * c]     = (u8)(sw_aes_mul(a0, 2) ^ sw_aes_mul(a1, 3) ^ a2 ^ a3);
        r.b[4 * c + 1] = (u8)(a0 ^ sw_aes_mul(a1, 2) ^ sw_aes_mul(a2, 3) ^ a3);
        r.b[4 * c + 2] = (u8)(a0 ^ a1 ^ sw_aes_mul(a2, 2) ^ sw_aes_mul(a3, 3));
        r.b[4 * c + 3] = (u8)(sw_aes_mul(a0, 3) ^ a1 ^ a2 ^ sw_aes_mul(a3, 2));
    }
    return r;
}
static sw_v128 sw_aesimc(sw_v128 n)
{
    sw_v128 r; int c;
    for (c = 0; c < 4; c++) {
        u8 a0 = n.b[4 * c], a1 = n.b[4 * c + 1], a2 = n.b[4 * c + 2], a3 = n.b[4 * c + 3];
        r.b[4 * c]     = (u8)(sw_aes_mul(a0, 14) ^ sw_aes_mul(a1, 11) ^ sw_aes_mul(a2, 13) ^ sw_aes_mul(a3, 9));
        r.b[4 * c + 1] = (u8)(sw_aes_mul(a0, 9) ^ sw_aes_mul(a1, 14) ^ sw_aes_mul(a2, 11) ^ sw_aes_mul(a3, 13));
        r.b[4 * c + 2] = (u8)(sw_aes_mul(a0, 13) ^ sw_aes_mul(a1, 9) ^ sw_aes_mul(a2, 14) ^ sw_aes_mul(a3, 11));
        r.b[4 * c + 3] = (u8)(sw_aes_mul(a0, 11) ^ sw_aes_mul(a1, 13) ^ sw_aes_mul(a2, 9) ^ sw_aes_mul(a3, 14));
    }
    return r;
}

#include "sw_simd6.h"   /* SIMD batch 6, CRC32 */

/* ---- fast paths for the hottest vector helpers (profile 2026-11-11: ~6% of all exe samples in sw_v_*,
 * called out of line at /O1 with sw_v128 passed through memory).  Same results lane for lane: MSVC x64
 * scalar float math is SSE too (same rounding, same MXCSR FTZ/DAZ), and the default-NaN fix is the same
 * bit test as sw_dn32.  FMLA/FMLS use FMA3 only when the CPU has it (sw_cpu_fma, set by the runtime at
 * start-up); otherwise the fmaf loop above.  SW_NO_FAST_SIMD: the generic helpers only. */
#if (defined(_M_X64) || defined(__x86_64__)) && !defined(SW_NO_FAST_SIMD) && !defined(SW_RT_IMPL)
#include <immintrin.h>
extern int sw_cpu_fma;
static SW_FORCEINLINE __m128 sw_x_ld(sw_v128 v) { return _mm_loadu_ps(v.f); }
static SW_FORCEINLINE sw_v128 sw_x_st(__m128 x) { sw_v128 r; _mm_storeu_ps(r.f, x); return r; }
static SW_FORCEINLINE __m128 sw_x_dn32(__m128 x)   /* -NaN default (0xFFC00000) -> +default NaN, as sw_dn32 */
{
    __m128i xi = _mm_castps_si128(x);
    __m128i m = _mm_cmpeq_epi32(xi, _mm_set1_epi32((int)0xFFC00000u));
    return _mm_castsi128_ps(_mm_or_si128(_mm_andnot_si128(m, xi), _mm_and_si128(m, _mm_set1_epi32(0x7FC00000))));
}
/* any NaN lane among the inputs: the FMA paths leave those to the generic helper, whose CRT fmaf picks a
 * different NaN than the FMA instruction when several inputs are NaN */
static SW_FORCEINLINE int sw_x_anynan3(__m128 a, __m128 b, __m128 d)
{
    return _mm_movemask_ps(_mm_or_ps(_mm_or_ps(_mm_cmpunord_ps(a, a), _mm_cmpunord_ps(b, b)), _mm_cmpunord_ps(d, d))) != 0;
}
static SW_FORCEINLINE sw_v128 sw_vx_fadd_32(sw_v128 a, sw_v128 b) { return sw_x_st(sw_x_dn32(_mm_add_ps(sw_x_ld(a), sw_x_ld(b)))); }
static SW_FORCEINLINE sw_v128 sw_vx_fsub_32(sw_v128 a, sw_v128 b) { return sw_x_st(sw_x_dn32(_mm_sub_ps(sw_x_ld(a), sw_x_ld(b)))); }
static SW_FORCEINLINE sw_v128 sw_vx_fmul_32(sw_v128 a, sw_v128 b) { return sw_x_st(sw_x_dn32(_mm_mul_ps(sw_x_ld(a), sw_x_ld(b)))); }
static SW_FORCEINLINE sw_v128 sw_vx_fmla_32(sw_v128 d, sw_v128 a, sw_v128 b)
{
    if (sw_cpu_fma && !sw_x_anynan3(sw_x_ld(a), sw_x_ld(b), sw_x_ld(d)))
        return sw_x_st(sw_x_dn32(_mm_fmadd_ps(sw_x_ld(a), sw_x_ld(b), sw_x_ld(d))));
    return sw_v_fmla_32(d, a, b);
}
static SW_FORCEINLINE sw_v128 sw_vx_fmls_32(sw_v128 d, sw_v128 a, sw_v128 b)
{   /* fmaf(-a, b, d): a's sign flipped first (as Arm's FPNeg, a NaN in a changes sign), then one fused
     * multiply-add.  FNMADD would negate the product instead and keep a NaN's sign. */
    if (sw_cpu_fma && !sw_x_anynan3(sw_x_ld(a), sw_x_ld(b), sw_x_ld(d))) {
        __m128 na = _mm_xor_ps(sw_x_ld(a), _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u)));
        return sw_x_st(sw_x_dn32(_mm_fmadd_ps(na, sw_x_ld(b), sw_x_ld(d))));
    }
    return sw_v_fmls_32(d, a, b);
}
/* by element: n = 2 or 4 lanes, lanes >= n are zero */
static SW_FORCEINLINE sw_v128 sw_vx_elem_mask(__m128 x, int n)
{
    sw_v128 r = sw_x_st(x);
    if (n == 2) r.d[1] = 0;
    return r;
}
static SW_FORCEINLINE sw_v128 sw_vx_fmul_elem_32(sw_v128 d, sw_v128 a, sw_v128 m, int idx, int n)
{
    if (n != 2 && n != 4) return sw_v_fmul_elem_32(d, a, m, idx, n);
    return sw_vx_elem_mask(sw_x_dn32(_mm_mul_ps(sw_x_ld(a), _mm_set1_ps(m.f[idx]))), n);
}
static SW_FORCEINLINE sw_v128 sw_vx_fmla_elem_32(sw_v128 d, sw_v128 a, sw_v128 m, int idx, int n)
{
    if ((n != 2 && n != 4) || !sw_cpu_fma) return sw_v_fmla_elem_32(d, a, m, idx, n);
    __m128 s = _mm_set1_ps(m.f[idx]);
    if (sw_x_anynan3(sw_x_ld(a), s, sw_x_ld(d))) return sw_v_fmla_elem_32(d, a, m, idx, n);
    return sw_vx_elem_mask(sw_x_dn32(_mm_fmadd_ps(sw_x_ld(a), s, sw_x_ld(d))), n);
}
/* EXT (bytes): n = 8 or 16, idx < n */
static SW_FORCEINLINE sw_v128 sw_vx_ext_8(sw_v128 a, sw_v128 b, int idx, int n)
{
    sw_v128 r; u8 both[32];
    if (n == 16 && (unsigned)idx < 16u) {
        memcpy(both, a.b, 16); memcpy(both + 16, b.b, 16); memcpy(r.b, both + idx, 16); return r;
    }
    if (n == 8 && (unsigned)idx < 8u) {
        memcpy(both, a.b, 8); memcpy(both + 8, b.b, 8); memcpy(r.b, both + idx, 8); r.d[1] = 0; return r;
    }
    return sw_v_ext_8(a, b, idx, n);
}
#define sw_v_fadd_32(a, b)              sw_vx_fadd_32((a), (b))
#define sw_v_fsub_32(a, b)              sw_vx_fsub_32((a), (b))
#define sw_v_fmul_32(a, b)              sw_vx_fmul_32((a), (b))
#define sw_v_fmla_32(d, a, b)           sw_vx_fmla_32((d), (a), (b))
#define sw_v_fmls_32(d, a, b)           sw_vx_fmls_32((d), (a), (b))
#define sw_v_fmul_elem_32(d, a, m, i, n) sw_vx_fmul_elem_32((d), (a), (m), (i), (n))
#define sw_v_fmla_elem_32(d, a, m, i, n) sw_vx_fmla_elem_32((d), (a), (m), (i), (n))
#define sw_v_ext_8(a, b, i, n)          sw_vx_ext_8((a), (b), (i), (n))
#endif

/* ---- Direct calls (recompiler SWRECOMP_DIRECT, clang chunks only) ----
 * `bl f` to a known function: the runtime's call fast path (sw_call_i -> call_fast_body) inlined, with a direct
 * C call of f instead of a table load + indirect call.  The guard is the same as sw_call_i's: g_plain_idx[idx]
 * (no hook / redirect / setjmp / longjmp at f), and SW_NEWFN off; otherwise the runtime path runs.  `b f`:
 * SW_TAIL_D jumps to f (clang musttail) where sw_dispatch_i would have queued a tail dispatch.  MSVC chunks keep
 * the old calls (the sw_call_i reload workaround above). */
/* Per host thread call state, one thread-local block so a direct call looks the TLS slot up once (three separate
 * __declspec(thread) ints cost two TLS lookups = 6 loads per call).  tick_left: sw_thread_tick countdown
 * (sw_thread.c); gen_depth / tail_set: trampoline state (sw_runtime.c, saved per guest thread on fiber switches). */
typedef struct SwGenTls { int tick_left; int gen_depth; int tail_set; int pad_; } SwGenTls;
extern __declspec(thread) SwGenTls sw_gtls;
#define sw_tick_left (sw_gtls.tick_left)
#define g_gen_depth (sw_gtls.gen_depth)
#define g_tail_set (sw_gtls.tail_set)
void sw_thread_tick(SwCpu *c);
void sw_call_d_tail(SwCpu *c);   /* after a direct call: run the tail chain the callee queued */
void sw_call_d_halt(SwCpu *c);   /* after a direct call: the guest thread stopped (trap) */
SwCpu *sw_call_ic_miss(SwCpu *c, u64 target, const void **ic);
void sw_dispatch_ic_miss(SwCpu *c, u64 target, const void **ic);
u64 sw_tick_now(void);
#if !defined(SW_RT_IMPL) && defined(__clang__)
extern u8 *g_direct_idx;   /* g_plain_idx, or NULL with SW_DIRECT=0 (sw_runtime.c) */
extern int sw_newfn_mode;
static SW_FORCEINLINE SwCpu *sw_call_d_(SwCpu *c, u64 t, u32 i, void (*f)(SwCpu *))
{
    const u8 *pl = g_direct_idx;   /* NULL while SW_NEWFN marks first calls (sw_newfn_init) or with SW_DIRECT=0 */
    if (__builtin_expect(!pl || !pl[i], 0)) return (sw_call_i)(c, t, i);
    u64 caller = c->cur_fn;
    SwGenTls *g = &sw_gtls;        /* same host thread for the whole call (fibers switch on one host thread) */
    if (__builtin_expect(--g->tick_left <= 0, 0)) sw_thread_tick(c);
    /* prev_fn / call_depth: diagnostics only (insn trace, crash report); the runtime's slow paths still set them */
    g->gen_depth++;
    f(c);
    g->gen_depth--;
    if (__builtin_expect(g->tail_set != 0, 0)) sw_call_d_tail(c);
    c->cur_fn = caller;
    if (__builtin_expect(c->halted != 0, 0)) sw_call_d_halt(c);
    return c;
}
#define sw_call_d(c, t, i, f) ((void)((c) = sw_call_d_((c), (t), (i), (f))))
/* `bl f` to a leaf (recompiler facts: f makes no call, tail call, indirect jump, svc or trap, so it can neither
 * switch guest threads, queue a tail chain nor halt): a plain C call, no time slice / trampoline bookkeeping.
 * Still behind g_direct_idx (hooked / setjmp targets take the runtime path).  The callee set c->cur_fn, so the
 * caller's own address (self) goes back.  2026-11-25. */
#define sw_call_leaf(c, t, i, f, self) do { const u8 *pl_ = g_direct_idx; \
        if (__builtin_expect(pl_ != 0 && pl_[i] != 0, 1)) { f(c); (c)->cur_fn = (self); } \
        else sw_call_d((c), (t), (i), (f)); } while (0)
/* `blr xN`: each call site keeps the sw_functions entry of its last target (*ic, one pointer: written whole);
 * the same target again is a compare and a call of entry->fn, no call-cache hash.  The entry must still be on
 * the direct path (g_direct_idx), else (and on a new target) sw_call_ic_miss = sw_call + refill. */
typedef struct SwFnEntRt { u64 addr; void (*fn)(SwCpu *); const char *name; } SwFnEntRt;
extern const SwFnEntRt sw_functions[];
static SW_FORCEINLINE SwCpu *sw_call_r_(SwCpu *c, u64 t, const void **ic)
{
    const SwFnEntRt *e = (const SwFnEntRt *)ic[0];      /* 2 entries: sites that alternate between two targets */
    if (!e || e->addr != t) e = (const SwFnEntRt *)ic[1];
    const u8 *pl = g_direct_idx;
    if (__builtin_expect(!e || e->addr != t || !pl || !pl[e - sw_functions], 0))
        return sw_call_ic_miss(c, t, ic);
    u64 caller = c->cur_fn;
    SwGenTls *g = &sw_gtls;
    if (__builtin_expect(--g->tick_left <= 0, 0)) sw_thread_tick(c);
    g->gen_depth++;
    e->fn(c);
    g->gen_depth--;
    if (__builtin_expect(g->tail_set != 0, 0)) sw_call_d_tail(c);
    c->cur_fn = caller;
    if (__builtin_expect(c->halted != 0, 0)) sw_call_d_halt(c);
    return c;
}
#define sw_call_r(c, t, ic) ((void)((c) = sw_call_r_((c), (t), (ic))))
/* `br xN` (an indirect tail jump: import stubs, virtual tail calls): the same 2-entry site cache; a hit jumps
 * straight to the cached function (musttail), a miss is sw_dispatch + refill (sw_dispatch_ic_miss). */
#define SW_TAIL_R(c, t, ic) do { const SwFnEntRt *e_ = (const SwFnEntRt *)(ic)[0]; const u8 *pl_ = g_direct_idx; \
        if (!e_ || e_->addr != (t)) e_ = (const SwFnEntRt *)(ic)[1]; \
        if (e_ && e_->addr == (t) && pl_ && pl_[e_ - sw_functions]) { \
            __attribute__((musttail)) return e_->fn(c); } \
        sw_dispatch_ic_miss((c), (t), (ic)); } while (0)
/* CNTVCT (the game's clock): with the invariant-TSC clock (sw_runtime.c sw_tick) read inline - rdtsc and one
 * multiply instead of sw_mrs -> sw_tick; else the runtime's path */
extern volatile long g_tsc_mode;
extern u64 g_tsc_base_tick, g_tsc_base, g_tsc_mul;
static SW_FORCEINLINE u64 sw_cntvct_(SwCpu *c)
{
    if (__builtin_expect(g_tsc_mode == 1, 1)) {
        u64 hi, lo = _umul128(__rdtsc() - g_tsc_base, g_tsc_mul, &hi);
        return g_tsc_base_tick + ((hi << 24) | (lo >> 40));
    }
    return sw_mrs(c, SW_SYS_CNTVCT_EL0);
}
#define sw_cntvct(c) sw_cntvct_(c)
#define SW_TAIL_D(c, t, i, f) do { const u8 *pl_ = g_direct_idx; \
        if (pl_ && pl_[i]) { __attribute__((musttail)) return f(c); } \
        sw_dispatch_i((c), (t), (i)); } while (0)
#else
#define sw_call_d(c, t, i, f) sw_call_i(c, t, i)
#define sw_call_leaf(c, t, i, f, self) sw_call_i(c, t, i)
#define sw_call_r(c, t, ic) sw_call_reg(c, t)
#define SW_TAIL_R(c, t, ic) sw_dispatch((c), (t))
#define sw_cntvct(c) sw_mrs((c), SW_SYS_CNTVCT_EL0)
#define SW_TAIL_D(c, t, i, f) sw_dispatch_i((c), (t), (i))
#endif

#endif /* SW_RT_H */
