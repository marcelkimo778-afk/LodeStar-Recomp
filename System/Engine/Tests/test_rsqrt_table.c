/* Checks that the FRSQRTE estimate table in sw_rt.h (sw_rsqrt_tab) gives exactly
 * the result of the Arm ARM search loop (sw_rsqrt_est_loop) for every input, and
 * that FRSQRTE (32/64-bit, FZ off and on) built on it matches the loop version.
 * Build + run: Engine/Tests/test_rsqrt_table.bat */
#include <stdio.h>
#include <math.h>
#include "sw_rt.h"

/* FRSQRTE exactly as before the table (sw_rt.h 2026-11-10), estimate by the loop */
static float ref_frsqrte32(float x)
{
    u32 v, e, frac, sc, r; memcpy(&v, &x, 4);
    e = (v >> 23) & 0xFF; frac = v & 0x7FFFFF;
    if (e == 0xFF && frac) return sw_dn32(x + x);
    if (e == 0 && frac && sw_fz()) frac = 0, v &= 0x80000000u;
    if (e == 0 && !frac) { v = (v & 0x80000000u) | 0x7F800000u; memcpy(&x, &v, 4); return x; }
    if (v & 0x80000000u) { v = 0x7FC00000u; memcpy(&x, &v, 4); return x; }
    if (e == 0xFF) return 0.0f;
    if (e == 0) return 1.0f / sqrtf(x);
    sc = (e & 1) ? (0x80 | (frac >> 16)) : (0x100 | (frac >> 15));
    r = sw_rsqrt_est_loop(sc);
    v = (((380 - e) / 2) << 23) | ((r & 0xFF) << 15); memcpy(&x, &v, 4); return x;
}
static double ref_frsqrte64(double x)
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
    r = sw_rsqrt_est_loop((u32)sc);
    v = (((3068 - e) / 2) << 52) | ((r & 0xFF) << 44); memcpy(&x, &v, 8); return x;
}

static u64 rng = 0x9E3779B97F4A7C15ull;
static u64 next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main(void)
{
    int fails = 0;
    for (u32 a = 0; a < 4096; a++)
        if (sw_rsqrt_est(a) != sw_rsqrt_est_loop(a)) { if (fails++ < 10) printf("FAIL est(%u): %u vs %u\n", a, sw_rsqrt_est(a), sw_rsqrt_est_loop(a)); }
    for (int fz = 0; fz < 2; fz++) {
        _mm_setcsr(fz ? (_mm_getcsr() | 0x8040u) : (_mm_getcsr() & ~0x8040u));
        /* every float with exponent 0..2 and 125..129 and 253..255 (all mantissas, both signs), then random bits */
        static const u32 exps[] = {0, 1, 2, 125, 126, 127, 128, 129, 253, 254, 255};
        for (unsigned k = 0; k < sizeof exps / sizeof *exps; k++)
            for (u32 m = 0; m < (1u << 23); m += 1) for (u32 s = 0; s < 2; s++) {
                u32 v = (s << 31) | (exps[k] << 23) | m, a, b; float x;
                memcpy(&x, &v, 4);
                float p = sw_frsqrte32(x), q = ref_frsqrte32(x);
                memcpy(&a, &p, 4); memcpy(&b, &q, 4);
                if (a != b) { if (fails++ < 10) printf("FAIL f32 %08x fz%d: %08x vs %08x\n", v, fz, a, b); }
            }
        for (int i = 0; i < 20000000; i++) {
            u32 v = (u32)next(), a, b; float x; memcpy(&x, &v, 4);
            float p = sw_frsqrte32(x), q = ref_frsqrte32(x);
            memcpy(&a, &p, 4); memcpy(&b, &q, 4);
            if (a != b) { if (fails++ < 10) printf("FAIL f32 %08x fz%d: %08x vs %08x\n", v, fz, a, b); }
            u64 w = next(), c, d; double y; memcpy(&y, &w, 8);
            double pp = sw_frsqrte64(y), qq = ref_frsqrte64(y);
            memcpy(&c, &pp, 8); memcpy(&d, &qq, 8);
            if (c != d) { if (fails++ < 10) printf("FAIL f64 %016llx fz%d\n", (unsigned long long)w, fz); }
        }
        /* SIMD lane form */
        sw_v128 in, o1; memset(&in, 0, sizeof in);
        in.f[0] = 2.0f; in.f[1] = 0.37f; in.f[2] = 1e-20f; in.f[3] = 12345.0f;
        o1 = sw_v_frsqrte2_32(in, 4);
        for (int l = 0; l < 4; l++) {
            float q = ref_frsqrte32(in.f[l]);
            if (memcmp(&o1.f[l], &q, 4)) { fails++; printf("FAIL lane %d fz%d\n", l, fz); }
        }
    }
    printf(fails ? "test_rsqrt_table: %d FAILED\n" : "test_rsqrt_table: all inputs match (%d failures)\n", fails);
    return fails != 0;
}
