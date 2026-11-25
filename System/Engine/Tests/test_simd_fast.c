/* The sw_rt.h fast SIMD paths (sw_vx_*) must give bit-identical results to the generic helpers they replace,
 * for random bit patterns and the special values (NaN payloads, the x86 default NaN, +-inf, denormals, +-0),
 * with MXCSR FTZ/DAZ off and on (FPCR.FZ), and with FMA3 on and off.  (name)(...) calls the generic function
 * past the redirecting macro.  Build + run: Engine/Tests/test_simd_fast.bat */
#include <stdio.h>
#include <stdint.h>
#include <xmmintrin.h>
#include "sw_rt.h"

int sw_cpu_fma;

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static const uint32_t special[] = {0, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7FA00001u,
                                   0xFFA00001u, 0x00000001u, 0x807FFFFFu, 0x00400000u, 0x3F800000u, 0xBF800000u,
                                   0x7F7FFFFFu, 0x00800000u, 0x80800000u, 0x3EAAAAABu};
static uint32_t val(void)
{
    uint32_t r = rnd();
    if ((r & 3) == 0) return special[rnd() % (sizeof special / sizeof special[0])];
    if ((r & 7) == 1) return rnd() & 0x807FFFFFu;              /* denormal */
    return rnd();
}
static sw_v128 vec(void) { sw_v128 v; for (int i = 0; i < 4; i++) v.s[i] = val(); return v; }
static long fails;
static void cmp(const char *what, sw_v128 a, sw_v128 b)
{
    if (memcmp(&a, &b, 16) && fails++ < 20)
        printf("FAIL %s: %08x %08x %08x %08x vs %08x %08x %08x %08x\n", what, a.s[0], a.s[1], a.s[2], a.s[3],
               b.s[0], b.s[1], b.s[2], b.s[3]);
}

int main(void)
{
    int r[4]; __cpuid(r, 1);
    int have_fma = ((r[2] >> 12) & 1) && ((r[2] >> 27) & 1) && ((r[2] >> 28) & 1) && ((_xgetbv(0) & 6) == 6);
    unsigned csr0 = _mm_getcsr();
    long n = 0;
    for (int fz = 0; fz < 2; fz++)
        for (int fma = 0; fma <= have_fma; fma++) {
            _mm_setcsr(fz ? (csr0 | 0x8040u) : (csr0 & ~0x8040u));
            sw_cpu_fma = fma;
            for (int it = 0; it < 400000; it++, n++) {
                sw_v128 a = vec(), b = vec(), d = vec();
                int idx = (int)(rnd() % 4), nl = (rnd() & 1) ? 4 : 2, bi = (int)(rnd() % 16), bn = (rnd() & 1) ? 16 : 8;
                if (bn == 8) bi &= 7;
                cmp("fadd", sw_v_fadd_32(a, b), (sw_v_fadd_32)(a, b));
                cmp("fsub", sw_v_fsub_32(a, b), (sw_v_fsub_32)(a, b));
                cmp("fmul", sw_v_fmul_32(a, b), (sw_v_fmul_32)(a, b));
                cmp("fmla", sw_v_fmla_32(d, a, b), (sw_v_fmla_32)(d, a, b));
                cmp("fmls", sw_v_fmls_32(d, a, b), (sw_v_fmls_32)(d, a, b));
                cmp("fmul_elem", sw_v_fmul_elem_32(d, a, b, idx, nl), (sw_v_fmul_elem_32)(d, a, b, idx, nl));
                cmp("fmla_elem", sw_v_fmla_elem_32(d, a, b, idx, nl), (sw_v_fmla_elem_32)(d, a, b, idx, nl));
                cmp("ext_8", sw_v_ext_8(a, b, bi, bn), (sw_v_ext_8)(a, b, bi, bn));
            }
        }
    _mm_setcsr(csr0);
    printf("%ld vector cases x 8 helpers (FTZ/DAZ off+on, FMA %s): %ld failures\n", n,
           have_fma ? "off+on" : "off only (CPU has none)", fails);
    return fails != 0;
}
