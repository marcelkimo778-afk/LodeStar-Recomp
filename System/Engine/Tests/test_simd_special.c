/* Checks sw_rt.h SIMD helpers on Arm-defined special cases (values per the
 * Arm ARM pseudocode: FPRecipStepFused / FPRSqrtStepFused, ADDP).
 * Build + run: Engine/Tests/test_simd_special.bat */
#include <stdio.h>
#include <math.h>
#include "sw_rt.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    sw_v128 a, b, r;
    float inf = INFINITY;
    a.f[0] = 0.0f; a.f[1] = inf;  a.f[2] = -0.0f; a.f[3] = 2.0f;
    b.f[0] = inf;  b.f[1] = 0.0f; b.f[2] = inf;   b.f[3] = 0.5f;
    r = sw_v_frsqrts_32(a, b);
    CHECK(r.f[0] == 1.5f); CHECK(r.f[1] == 1.5f); CHECK(r.f[2] == 1.5f); CHECK(r.f[3] == 1.0f);
    r = sw_v_frecps_32(a, b);
    CHECK(r.f[0] == 2.0f); CHECK(r.f[1] == 2.0f); CHECK(r.f[2] == 2.0f); CHECK(r.f[3] == 1.0f);
    a.f[0] = NAN;
    r = sw_v_frsqrts_32(a, b);
    CHECK(isnan(r.f[0]));
    a.df[0] = 0.0; a.df[1] = 4.0; b.df[0] = (double)inf; b.df[1] = 0.25;
    r = sw_v_frsqrts_64(a, b); CHECK(r.df[0] == 1.5); CHECK(r.df[1] == 1.0);
    r = sw_v_frecps_64(a, b);  CHECK(r.df[0] == 2.0); CHECK(r.df[1] == 1.0);

    /* addp .8b as emitted: addp_8(catlo(a, b), b), then the top cleared */
    for (int i = 0; i < 16; i++) { a.b[i] = (u8)(i + 1); b.b[i] = (u8)(0x10 * (i + 1)); }
    r = sw_v_addp_8(sw_v_catlo(a, b), b);
    CHECK(r.b[0] == 3); CHECK(r.b[3] == 15);              /* 1+2 .. 7+8 */
    CHECK(r.b[4] == 0x30); CHECK(r.b[7] == (u8)0xF0);     /* 0x10+0x20 .. 0x70+0x80 */

    printf(fails ? "%d FAILED\n" : "all SIMD special-case tests passed\n", fails);
    return fails != 0;
}
