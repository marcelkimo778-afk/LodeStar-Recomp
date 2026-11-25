/* Regression test for an MSVC optimiser bug the generated code ran into (cl 19.44
 * and 19.51, /Og: the chunk flags /O1 /Oi and /O2 are affected).
 *
 * Generated code reads every call result back from c->x[0].  When copies of
 * several such results are bit-tested in a chain (guest `bl a; mov w20, w0;
 * bl b; mov w21, w0; ... tbz w20, #0, out; tbz w21, #0, out`), the optimiser
 * treated the loads of c->x[0] as one value and dropped tests: guest 0x1642064
 * (are all field subsystems ready?) returned "ready" while four were not, so
 * the field message hub never registered a handler (doors, NPCs, items).
 * sw_rt.h now makes generated code continue with the CPU pointer each call
 * returns; this test runs the generated-code shape with the real header.
 *
 * The file is compiled twice: once with SW_TEST_STUBS (the runtime stand-ins,
 * in their own object so the optimiser cannot see into them), once without.
 * SW_TEST_OLD_RELOAD=1 compiles the call as before the fix (expected to fail on
 * an affected compiler).  Build + run: Engine/Tests/test_msvc_call_reload.bat */
#include <stdio.h>
#include <string.h>
#include "sw_rt.h"

#define N_CALLS 6
#define CALLEE(i) (0x1000ull + (i))

#ifdef SW_TEST_STUBS
u8 sw_pmap[1];
int sw_mem_fast_on = 1;
u64 g_not_ready;   /* the callee that answers 0 ("not ready"); 0 = none */
SwCpu *(sw_call)(SwCpu *c, u64 target) { c->x[0] = target == g_not_ready ? 0 : 1; return c; }
SwCpu *(sw_call_reg)(SwCpu *c, u64 target) { return (sw_call)(c, target); }
SwCpu *(sw_svc)(SwCpu *c, u32 imm) { (void)imm; return c; }
void sw_dispatch(SwCpu *c, u64 target) { (void)c; (void)target; }
void sw_trap(SwCpu *c, u64 at, const char *why) { (void)c; printf("trap 0x%llx %s\n", (unsigned long long)at, why); }
u64 (sw_load64)(SwCpu *c, u64 a) { (void)c; return *(const u64 *)(uintptr_t)a; }
void (sw_store64)(SwCpu *c, u64 a, u64 v) { (void)c; *(u64 *)(uintptr_t)a = v; }
#else
extern u64 g_not_ready;
#if defined(SW_TEST_OLD_RELOAD)
#undef sw_call
#define sw_call(c, t) (sw_call)((c), (t))
#endif

/* The shape swrecomp emits for guest 0x1642064 (spill, call, reload, `mov wN, w0`,
 * then one tbz per result), shortened to six calls. */
#define CALL_STEP(i, xr)                                              \
    x0 = sw_load64(c, x19 + 0x58 + 8 * (i));                          \
    c->x[0] = x0; c->x[19] = x19; c->x[20] = x20; c->x[21] = x21;    \
    c->x[22] = x22; c->x[23] = x23; c->x[24] = x24; c->x[25] = x25;  \
    c->sp = sp;                                                       \
    sw_call(c, CALLEE(i));                                            \
    x0 = c->x[0];                                                     \
    x1 = c->x[1];                                                     \
    sp = c->sp;                                                       \
    xr = (u64)(u32)((u32)0 | (u32)x0);
__declspec(noinline) void gen_shape(SwCpu *c)
{
    u64 x0 = c->x[0], x1 = c->x[1], x19 = c->x[19], x20 = c->x[20], x21 = c->x[21], x22 = c->x[22],
        x23 = c->x[23], x24 = c->x[24], x25 = c->x[25];
    u64 sp = c->sp;
    x19 = (u64)(0 | x0);
    CALL_STEP(0, x20)
    CALL_STEP(1, x21)
    CALL_STEP(2, x22)
    CALL_STEP(3, x23)
    CALL_STEP(4, x24)
    CALL_STEP(5, x25)
    x0 = (u64)(u32)((u32)0 | (u32)0);
    if (!(((((u64)x20) >> 0) & 1))) goto L_out;
L_a:;
    if (!(((((u64)x21) >> 0) & 1))) goto L_out;
L_b:;
    if (!(((((u64)x22) >> 0) & 1))) goto L_out;
L_c:;
    if (!(((((u64)x23) >> 0) & 1))) goto L_out;
L_d:;
    if (!(((((u64)x24) >> 0) & 1))) goto L_out;
L_e:;
    if (((((u64)x25) >> 0) & 1)) goto L_ready;
L_out:;
    goto L_ret;
L_ready:;
    x0 = (u64)(0x0000000000000001ULL);
    goto L_out;
L_ret:
    c->x[0] = x0; c->x[1] = x1; c->x[19] = x19; c->sp = sp;
}

int main(void)
{
    static u64 obj[64], stack[512];
    int fails = 0;
    for (int z = -1; z < N_CALLS; z++) {
        SwCpu cpu;
        memset(&cpu, 0, sizeof cpu);
        cpu.x[0] = (u64)(uintptr_t)obj;
        cpu.sp = (u64)(uintptr_t)((char *)stack + sizeof stack);
        g_not_ready = z < 0 ? 0 : CALLEE(z);
        gen_shape(&cpu);
        u64 want = z < 0 ? 1 : 0;
        if (cpu.x[0] != want) {
            printf("FAIL: callee %d not ready, result %llu (want %llu)\n", z, (unsigned long long)cpu.x[0],
                   (unsigned long long)want);
            fails++;
        }
    }
    printf(fails ? "%d FAILED\n" : "call result reload: all %d cases passed\n", fails ? fails : N_CALLS + 1);
    return fails != 0;
}
#endif
