/* GCC/Clang builtins used by the graphics layer (ported from totk-pc-recomp),
 * mapped onto MSVC.  Force-included (/FI) into every Engine/Runtime/gfx file,
 * compiled with /std:clatest /experimental:c11atomics. */
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* attributes: only the ones that change meaning are emulated */
#define __attribute__(x)
#define __builtin_offsetof(t, m) offsetof(t, m)
#define __builtin_expect(x, v) (x)
#define __builtin_unreachable() __assume(0)
#define __builtin_bswap16 _byteswap_ushort
#define __builtin_bswap32 _byteswap_ulong
#define __builtin_bswap64 _byteswap_uint64

static __forceinline int __builtin_clz(unsigned x) { unsigned long i; _BitScanReverse(&i, x); return 31 - (int)i; }
static __forceinline int __builtin_clzll(unsigned long long x) { unsigned long i; _BitScanReverse64(&i, x); return 63 - (int)i; }
static __forceinline int __builtin_ctz(unsigned x) { unsigned long i; _BitScanForward(&i, x); return (int)i; }
static __forceinline int __builtin_ctzll(unsigned long long x) { unsigned long i; _BitScanForward64(&i, x); return (int)i; }
static __forceinline int __builtin_popcount(unsigned x) { return (int)__popcnt(x); }
static __forceinline int __builtin_popcountll(unsigned long long x) { return (int)__popcnt64(x); }
static __forceinline uint32_t __builtin_bitreverse32(uint32_t x) {
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4);
    return _byteswap_ulong(x);
}
static __forceinline uint8_t __builtin_bitreverse8(uint8_t x) { return (uint8_t)(__builtin_bitreverse32(x) >> 24); }

/* __atomic_* on plain objects (x64: aligned loads/stores up to 8 bytes are
 * atomic; read-modify-write goes through the Interlocked intrinsics).  Memory
 * orders are accepted and treated as sequentially consistent. */
#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5

static __forceinline uint64_t gcc_atomic_load(volatile void *p, size_t n) {
    uint64_t v;
    _ReadWriteBarrier();
    switch (n) {
    case 1: v = *(volatile uint8_t *)p; break;
    case 2: v = *(volatile uint16_t *)p; break;
    case 4: v = *(volatile uint32_t *)p; break;
    default: v = *(volatile uint64_t *)p; break;
    }
    _ReadWriteBarrier();
    return v;
}
static __forceinline uint64_t gcc_atomic_xchg(volatile void *p, size_t n, uint64_t v) {
    switch (n) {
    case 1: return (uint8_t)_InterlockedExchange8((volatile char *)p, (char)v);
    case 2: return (uint16_t)_InterlockedExchange16((volatile short *)p, (short)v);
    case 4: return (uint32_t)_InterlockedExchange((volatile long *)p, (long)v);
    default: return (uint64_t)_InterlockedExchange64((volatile long long *)p, (long long)v);
    }
}
static __forceinline uint64_t gcc_atomic_fadd(volatile void *p, size_t n, uint64_t v) {
    switch (n) {
    case 1: return (uint8_t)_InterlockedExchangeAdd8((volatile char *)p, (char)v);
    case 2: return (uint16_t)_InterlockedExchangeAdd16((volatile short *)p, (short)v);
    case 4: return (uint32_t)_InterlockedExchangeAdd((volatile long *)p, (long)v);
    default: return (uint64_t)_InterlockedExchangeAdd64((volatile long long *)p, (long long)v);
    }
}
static __forceinline uint64_t gcc_atomic_cas(volatile void *p, size_t n, uint64_t expected, uint64_t desired) {
    switch (n) {   /* returns the old value */
    case 1: return (uint8_t)_InterlockedCompareExchange8((volatile char *)p, (char)desired, (char)expected);
    case 2: return (uint16_t)_InterlockedCompareExchange16((volatile short *)p, (short)desired, (short)expected);
    case 4: return (uint32_t)_InterlockedCompareExchange((volatile long *)p, (long)desired, (long)expected);
    default: return (uint64_t)_InterlockedCompareExchange64((volatile long long *)p, (long long)desired, (long long)expected);
    }
}
static __forceinline uint64_t gcc_mask(size_t n, uint64_t v) {
    return n >= 8 ? v : (v & ((1ull << (8 * n)) - 1));
}
static __forceinline int gcc_atomic_cas_bool(volatile void *p, size_t n, void *expected, uint64_t desired) {
    uint64_t exp = 0;
    memcpy(&exp, expected, n);
    uint64_t old = gcc_atomic_cas(p, n, exp, desired);
    if (gcc_mask(n, old) == gcc_mask(n, exp)) return 1;
    memcpy(expected, &old, n);
    return 0;
}

#define GCC_AT_CAST(p, v) ((typeof(*(p)))(uintptr_t)(v))
#define __atomic_load_n(p, o) GCC_AT_CAST(p, gcc_atomic_load((volatile void *)(p), sizeof *(p)))
#define __atomic_store_n(p, v, o) ((void)gcc_atomic_xchg((volatile void *)(p), sizeof *(p), (uint64_t)(uintptr_t)(v)))
#define __atomic_exchange_n(p, v, o) GCC_AT_CAST(p, gcc_atomic_xchg((volatile void *)(p), sizeof *(p), (uint64_t)(uintptr_t)(v)))
#define __atomic_fetch_add(p, v, o) GCC_AT_CAST(p, gcc_atomic_fadd((volatile void *)(p), sizeof *(p), (uint64_t)(v)))
#define __atomic_fetch_sub(p, v, o) GCC_AT_CAST(p, gcc_atomic_fadd((volatile void *)(p), sizeof *(p), (uint64_t)0 - (uint64_t)(v)))
#define __atomic_add_fetch(p, v, o) GCC_AT_CAST(p, gcc_atomic_fadd((volatile void *)(p), sizeof *(p), (uint64_t)(v)) + (uint64_t)(v))
#define __atomic_sub_fetch(p, v, o) GCC_AT_CAST(p, gcc_atomic_fadd((volatile void *)(p), sizeof *(p), (uint64_t)0 - (uint64_t)(v)) - (uint64_t)(v))
#define __atomic_compare_exchange_n(p, e, d, w, s, f) \
    gcc_atomic_cas_bool((volatile void *)(p), sizeof *(p), (void *)(e), (uint64_t)(uintptr_t)(d))
#define __atomic_thread_fence(o) _ReadWriteBarrier(), __faststorefence()

static __forceinline uint64_t gcc_atomic_bitop(volatile void *p, size_t n, uint64_t v, int op) {
    /* op 0 or, 1 and, 2 xor; returns the old value */
    switch (n) {
    case 1: return (uint8_t)(op == 0 ? _InterlockedOr8((volatile char *)p, (char)v)
                           : op == 1 ? _InterlockedAnd8((volatile char *)p, (char)v)
                                     : _InterlockedXor8((volatile char *)p, (char)v));
    case 2: return (uint16_t)(op == 0 ? _InterlockedOr16((volatile short *)p, (short)v)
                            : op == 1 ? _InterlockedAnd16((volatile short *)p, (short)v)
                                      : _InterlockedXor16((volatile short *)p, (short)v));
    case 4: return (uint32_t)(op == 0 ? _InterlockedOr((volatile long *)p, (long)v)
                            : op == 1 ? _InterlockedAnd((volatile long *)p, (long)v)
                                      : _InterlockedXor((volatile long *)p, (long)v));
    default: return (uint64_t)(op == 0 ? _InterlockedOr64((volatile long long *)p, (long long)v)
                             : op == 1 ? _InterlockedAnd64((volatile long long *)p, (long long)v)
                                       : _InterlockedXor64((volatile long long *)p, (long long)v));
    }
}
#define __atomic_fetch_or(p, v, o) GCC_AT_CAST(p, gcc_atomic_bitop((volatile void *)(p), sizeof *(p), (uint64_t)(v), 0))
#define __atomic_fetch_and(p, v, o) GCC_AT_CAST(p, gcc_atomic_bitop((volatile void *)(p), sizeof *(p), (uint64_t)(v), 1))
#define __atomic_fetch_xor(p, v, o) GCC_AT_CAST(p, gcc_atomic_bitop((volatile void *)(p), sizeof *(p), (uint64_t)(v), 2))
#define __atomic_or_fetch(p, v, o) GCC_AT_CAST(p, gcc_atomic_bitop((volatile void *)(p), sizeof *(p), (uint64_t)(v), 0) | (uint64_t)(v))
#define __atomic_and_fetch(p, v, o) GCC_AT_CAST(p, gcc_atomic_bitop((volatile void *)(p), sizeof *(p), (uint64_t)(v), 1) & (uint64_t)(v))
#define __thread _Thread_local
#define strcasecmp _stricmp
#define __builtin_ia32_rdtsc __rdtsc
#define strncasecmp _strnicmp
#endif
