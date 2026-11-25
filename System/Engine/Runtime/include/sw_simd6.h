/* SIMD batch 6 helpers, included at the end of sw_rt.h.
 *
 * Uniform signature (the lifter emits one call per instruction):
 *     Vd = sw6_<name>(d, n, m, imm, q, esz)
 * d = old Vd (accumulate / insert / "2" narrowing forms keep its lower half),
 * n, m = sources, imm = shift / index, q = Q bit, esz = element bits:
 *   - same-width ops: the lane size
 *   - widening / narrowing ops: the NARROW element size
 *   - fcvtl / fcvtn: the WIDE element size
 * Every helper returns the full 128-bit register value with the bits the Arm
 * ARM zeroes already zero (upper half for Q=0, everything but element 0 for
 * scalar results).  Semantics follow the Arm ARM pseudocode; saturating ops
 * do not model FPSR.QC.
 */
#ifndef SW_SIMD6_H
#define SW_SIMD6_H

#define SW6(NAME) static sw_v128 sw6_##NAME(sw_v128 d, sw_v128 n, sw_v128 m, int imm, int q, int esz)

static u64 sw6_gu(sw_v128 v, int esz, int i)
{
    switch (esz) { case 8: return v.b[i]; case 16: return v.h[i]; case 32: return v.s[i]; default: return v.d[i]; }
}
static s64 sw6_gs(sw_v128 v, int esz, int i)
{
    switch (esz) { case 8: return (s8)v.b[i]; case 16: return (s16)v.h[i]; case 32: return (s32)v.s[i]; default: return (s64)v.d[i]; }
}
static void sw6_put(sw_v128 *v, int esz, int i, u64 x)
{
    switch (esz) { case 8: v->b[i] = (u8)x; break; case 16: v->h[i] = (u16)x; break;
                   case 32: v->s[i] = (u32)x; break; default: v->d[i] = x; break; }
}
static int sw6_lanes(int q, int esz) { return (q ? 128 : 64) / esz; }
static u64 sw6_umax(int esz) { return esz >= 64 ? ~0ull : ((1ull << esz) - 1); }
static s64 sw6_slimhi(int esz) { return esz >= 64 ? (s64)0x7FFFFFFFFFFFFFFFull : (s64)((1ull << (esz - 1)) - 1); }
static s64 sw6_slimlo(int esz) { return esz >= 64 ? (s64)0x8000000000000000ull : -(s64)(1ull << (esz - 1)); }
static s64 sw6_ssat(s64 v, int esz) { s64 hi = sw6_slimhi(esz), lo = sw6_slimlo(esz); return v > hi ? hi : v < lo ? lo : v; }
static u64 sw6_usat_s(s64 v, int esz) { return v < 0 ? 0 : (u64)v > sw6_umax(esz) ? sw6_umax(esz) : (u64)v; }
static u64 sw6_usat_u(u64 v, int esz) { return v > sw6_umax(esz) ? sw6_umax(esz) : v; }
static sw_v128 sw6_zero(void) { sw_v128 r; memset(&r, 0, sizeof r); return r; }
/* shifts that are well defined for counts up to 64 */
static s64 sw6_asr(s64 v, int sh) { return sh >= 64 ? (v < 0 ? -1 : 0) : (v >> sh); }
static u64 sw6_lsr(u64 v, int sh) { return sh >= 64 ? 0 : (v >> sh); }
static s64 sw6_rasr(s64 v, int sh) { return sw6_asr(v, sh) + (s64)((u64)sw6_asr(v, sh - 1) & 1); }
static u64 sw6_rlsr(u64 v, int sh) { return sw6_lsr(v, sh) + (sw6_lsr(v, sh - 1) & 1); }

/* ---- three same, integer ------------------------------------------------ */
#define SW6_BIN(NAME, EXPR_TYPE, GET, EXPR) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = sw6_lanes(q, esz); (void)d; (void)imm; \
        for (i = 0; i < L; i++) { EXPR_TYPE a = GET(n, esz, i), b = GET(m, esz, i); \
            EXPR_TYPE dv = GET(d, esz, i); (void)dv; sw6_put(&r, esz, i, (u64)(EXPR)); } \
        return r; }
SW6_BIN(shadd, s64, sw6_gs, (a + b) >> 1)
SW6_BIN(uhadd, u64, sw6_gu, (a + b) >> 1)
SW6_BIN(sabd, s64, sw6_gs, a > b ? a - b : b - a)
SW6_BIN(uabd, u64, sw6_gu, a > b ? a - b : b - a)
SW6_BIN(saba, s64, sw6_gs, dv + (a > b ? a - b : b - a))
SW6_BIN(uaba, u64, sw6_gu, dv + (a > b ? a - b : b - a))
SW6_BIN(mls, u64, sw6_gu, dv - a * b)

static s64 sw6_sqadd64(s64 a, s64 b)
{
    s64 r = (s64)((u64)a + (u64)b);
    if (((a ^ r) & (b ^ r)) < 0) return a < 0 ? sw6_slimlo(64) : sw6_slimhi(64);
    return r;
}
static s64 sw6_sqsub64(s64 a, s64 b)
{
    s64 r = (s64)((u64)a - (u64)b);
    if (((a ^ b) & (a ^ r)) < 0) return a < 0 ? sw6_slimlo(64) : sw6_slimhi(64);
    return r;
}
SW6_BIN(sqadd, s64, sw6_gs, esz == 64 ? sw6_sqadd64(a, b) : sw6_ssat(a + b, esz))
SW6_BIN(sqsub, s64, sw6_gs, esz == 64 ? sw6_sqsub64(a, b) : sw6_ssat(a - b, esz))
SW6_BIN(uqadd, u64, sw6_gu, esz == 64 ? (a + b < a ? ~0ull : a + b) : sw6_usat_u(a + b, esz))
SW6_BIN(uqsub, u64, sw6_gu, a < b ? 0 : a - b)

static s64 sw6_sqdmulh1(s64 a, s64 b, int esz, int round)
{
    s64 p;
    if (a == sw6_slimlo(esz) && b == sw6_slimlo(esz)) return sw6_slimhi(esz);
    p = 2 * a * b + (round ? (s64)1 << (esz - 1) : 0);
    return p >> esz;
}
SW6_BIN(sqdmulh, s64, sw6_gs, sw6_sqdmulh1(a, b, esz, 0))
SW6_BIN(sqrdmulh, s64, sw6_gs, sw6_sqdmulh1(a, b, esz, 1))

/* pairwise over the concatenation n:m */
#define SW6_PAIR(NAME, T, GET, OP) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = sw6_lanes(q, esz); (void)d; (void)imm; \
        for (i = 0; i < L; i++) { sw_v128 s = i < L / 2 ? n : m; int k = 2 * (i % (L / 2)); \
            T a = GET(s, esz, k), b = GET(s, esz, k + 1); sw6_put(&r, esz, i, (u64)(OP)); } \
        return r; }
SW6_PAIR(smaxp, s64, sw6_gs, a > b ? a : b)
SW6_PAIR(sminp, s64, sw6_gs, a < b ? a : b)
SW6_PAIR(umaxp, u64, sw6_gu, a > b ? a : b)
SW6_PAIR(uminp, u64, sw6_gu, a < b ? a : b)

/* by element: Vd[i] = Vn[i] * Vm[imm] (+/- Vd[i]) */
SW6_BIN(mul_elem, u64, sw6_gu, a * sw6_gu(m, esz, imm))
SW6_BIN(mla_elem, u64, sw6_gu, dv + a * sw6_gu(m, esz, imm))
SW6_BIN(mls_elem, u64, sw6_gu, dv - a * sw6_gu(m, esz, imm))

/* ---- FP min/max with Arm zero/NaN rules ---------------------------------- */
#define SW6_FMINMAX(SFX, T, U) \
    static T sw6_fmax##SFX(T a, T b, int nm) { \
        a = sw_ftz##SFX(a); b = sw_ftz##SFX(b); \
        if (a != a || b != b) { if (nm && a != a && b == b) return b; if (nm && b != b && a == a) return a; \
            return a != a ? a + a : b + b; } \
        if (a == b) { U x, y; memcpy(&x, &a, sizeof x); memcpy(&y, &b, sizeof y); x &= y; memcpy(&a, &x, sizeof x); return a; } \
        return a > b ? a : b; } \
    static T sw6_fmin##SFX(T a, T b, int nm) { \
        a = sw_ftz##SFX(a); b = sw_ftz##SFX(b); \
        if (a != a || b != b) { if (nm && a != a && b == b) return b; if (nm && b != b && a == a) return a; \
            return a != a ? a + a : b + b; } \
        if (a == b) { U x, y; memcpy(&x, &a, sizeof x); memcpy(&y, &b, sizeof y); x |= y; memcpy(&a, &x, sizeof x); return a; } \
        return a < b ? a : b; }
SW6_FMINMAX(32, float, u32)
SW6_FMINMAX(64, double, u64)

/* ---- three same, FP ------------------------------------------------------ */
#define SW6_FBIN(NAME, E32, E64) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = sw6_lanes(q, esz); (void)d; (void)imm; \
        if (esz == 32) { for (i = 0; i < L; i++) { float a = n.f[i], b = m.f[i]; r.f[i] = (E32); } } \
        else { for (i = 0; i < L; i++) { double a = n.df[i], b = m.df[i]; r.df[i] = (E64); } } \
        return r; }
SW6_FBIN(fabd, fabsf(a - b), fabs(a - b))
SW6_FBIN(fmaxnm, sw6_fmax32(a, b, 1), sw6_fmax64(a, b, 1))
SW6_FBIN(fminnm, sw6_fmin32(a, b, 1), sw6_fmin64(a, b, 1))
#define SW6_FPAIR(NAME, E32, E64) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = sw6_lanes(q, esz); (void)d; (void)imm; \
        for (i = 0; i < L; i++) { sw_v128 s = i < L / 2 ? n : m; int k = 2 * (i % (L / 2)); \
            if (esz == 32) { float a = s.f[k], b = s.f[k + 1]; r.f[i] = (E32); } \
            else { double a = s.df[k], b = s.df[k + 1]; r.df[i] = (E64); } } \
        return r; }
SW6_FPAIR(fmaxnmp, sw6_fmax32(a, b, 1), sw6_fmax64(a, b, 1))
SW6_FPAIR(fminnmp, sw6_fmin32(a, b, 1), sw6_fmin64(a, b, 1))

/* ---- scalar pairwise (element 0 = op(Vn[0], Vn[1])) ------------------------ */
#define SW6_SPAIR(NAME, E32, E64) \
    SW6(NAME) { sw_v128 r = sw6_zero(); (void)d; (void)m; (void)imm; (void)q; \
        if (esz == 32) { float a = n.f[0], b = n.f[1]; r.f[0] = (E32); } \
        else { double a = n.df[0], b = n.df[1]; r.df[0] = (E64); } \
        return r; }
SW6_SPAIR(faddp_scalar, sw_dn32(a + b), sw_dn64(a + b))
SW6_SPAIR(fmaxnmp_scalar, sw6_fmax32(a, b, 1), sw6_fmax64(a, b, 1))
SW6_SPAIR(fminnmp_scalar, sw6_fmin32(a, b, 1), sw6_fmin64(a, b, 1))
SW6_SPAIR(fmaxp_scalar, sw6_fmax32(a, b, 0), sw6_fmax64(a, b, 0))
SW6_SPAIR(fminp_scalar, sw6_fmin32(a, b, 0), sw6_fmin64(a, b, 0))
SW6(addp_scalar) { sw_v128 r = sw6_zero(); (void)d; (void)m; (void)imm; (void)q; (void)esz; r.d[0] = n.d[0] + n.d[1]; return r; }

/* ---- two-register misc, integer ------------------------------------------ */
#define SW6_UN(NAME, EXPR) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = sw6_lanes(q, esz); (void)d; (void)m; (void)imm; \
        for (i = 0; i < L; i++) { s64 a = sw6_gs(n, esz, i); sw6_put(&r, esz, i, (u64)(EXPR)); } \
        return r; }
SW6_UN(cmgt0, a > 0 ? ~0ull : 0)
SW6_UN(cmge0, a >= 0 ? ~0ull : 0)
SW6_UN(cmeq0, a == 0 ? ~0ull : 0)
SW6_UN(cmle0, a <= 0 ? ~0ull : 0)
SW6_UN(cmlt0, a < 0 ? ~0ull : 0)
SW6_UN(abs, a < 0 ? 0 - (u64)a : (u64)a)
SW6_UN(neg, 0 - (u64)a)
SW6_UN(sqabs, a == sw6_slimlo(esz) ? (u64)sw6_slimhi(esz) : (a < 0 ? 0 - (u64)a : (u64)a))
SW6_UN(sqneg, a == sw6_slimlo(esz) ? (u64)sw6_slimhi(esz) : 0 - (u64)a)

/* shift by register: the count is the signed low byte of each Vm lane;
 * positive shifts left (saturating for the q forms), negative shifts right
 * (rounding for the r forms) - Arm ARM SQSHL/UQSHL/SQRSHL/UQRSHL (register) */
static s64 sw6_sqshlr(s64 a, int sh, int esz, int round)
{
    if (sh >= 0) {
        if (a == 0) return 0;
        s64 hi = sw6_slimhi(esz), lo = sw6_slimlo(esz);
        if (sh >= esz) return a > 0 ? hi : lo;
        if (a > (hi >> sh)) return hi;
        if (a < (lo >> sh)) return lo;
        return (s64)((u64)a << sh);
    }
    return round ? sw6_rasr(a, -sh) : sw6_asr(a, -sh);
}
static u64 sw6_uqshlr(u64 a, int sh, int esz, int round)
{
    if (sh >= 0) {
        if (a == 0) return 0;
        u64 mx = sw6_umax(esz);
        if (sh >= esz) return mx;
        if (a > (mx >> sh)) return mx;
        return a << sh;
    }
    return round ? sw6_rlsr(a, -sh) : sw6_lsr(a, -sh);
}
SW6_BIN(sqshl, s64, sw6_gs, sw6_sqshlr(a, (s8)(u8)b, esz, 0))
SW6_BIN(sqrshl, s64, sw6_gs, sw6_sqshlr(a, (s8)(u8)b, esz, 1))
SW6_BIN(uqshl, u64, sw6_gu, sw6_uqshlr(a, (s8)(u8)b, esz, 0))
SW6_BIN(uqrshl, u64, sw6_gu, sw6_uqshlr(a, (s8)(u8)b, esz, 1))

/* pairwise long add: esz = source element, results are 2*esz */
#define SW6_ADDLP(NAME, GET, ACC) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = sw6_lanes(q, 2 * esz); (void)m; (void)imm; \
        for (i = 0; i < L; i++) { u64 v = (u64)(GET(n, esz, 2 * i) + GET(n, esz, 2 * i + 1)); \
            if (ACC) v += sw6_gu(d, 2 * esz, i); sw6_put(&r, 2 * esz, i, v); } \
        return r; }
SW6_ADDLP(saddlp, sw6_gs, 0)
SW6_ADDLP(uaddlp, sw6_gu, 0)
SW6_ADDLP(sadalp, sw6_gs, 1)
SW6_ADDLP(uadalp, sw6_gu, 1)

/* narrowing: esz = narrow element; source = n (2*esz elements); Q=1 writes the
 * upper half and keeps the lower half of d */
#define SW6_NARROW(NAME, EXPR) \
    SW6(NAME) { sw_v128 r = q ? d : sw6_zero(); int i, L = 64 / esz, off = q ? L : 0; (void)m; \
        if (q) r.d[1] = 0; \
        for (i = 0; i < L; i++) { s64 s = sw6_gs(n, 2 * esz, i); u64 u = sw6_gu(n, 2 * esz, i); \
            (void)s; (void)u; sw6_put(&r, esz, off + i, (u64)(EXPR)); } \
        return r; }
SW6_NARROW(xtn, u)
SW6_NARROW(sqxtn, sw6_ssat(s, esz))
SW6_NARROW(uqxtn, sw6_usat_u(u, esz))
SW6_NARROW(sqxtun, sw6_usat_s(s, esz))
SW6_NARROW(shrn, sw6_lsr(u, imm))
SW6_NARROW(rshrn, sw6_rlsr(u, imm))
SW6_NARROW(sqshrn, sw6_ssat(sw6_asr(s, imm), esz))
SW6_NARROW(sqrshrn, sw6_ssat(sw6_rasr(s, imm), esz))
SW6_NARROW(uqshrn, sw6_usat_u(sw6_lsr(u, imm), esz))
SW6_NARROW(uqrshrn, sw6_usat_u(sw6_rlsr(u, imm), esz))
SW6_NARROW(sqshrun, sw6_usat_s(sw6_asr(s, imm), esz))
SW6_NARROW(sqrshrun, sw6_usat_s(sw6_rasr(s, imm), esz))

/* widening from the lower (Q=0) or upper (Q=1) half: esz = narrow element */
#define SW6_WIDEN(NAME, GET, SHIFT) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = 64 / esz, off = q ? L : 0; (void)d; (void)m; \
        for (i = 0; i < L; i++) sw6_put(&r, 2 * esz, i, (u64)GET(n, esz, off + i) << (SHIFT)); \
        return r; }
SW6_WIDEN(sshll, sw6_gs, imm)
SW6_WIDEN(ushll, sw6_gu, imm)
SW6_WIDEN(shll, sw6_gu, esz)

/* ---- half precision <-> single (IEEE, round to nearest even) --------------- */
static float sw6_h2f(u16 h)
{
    u32 s = (u32)(h >> 15) << 31, e = (h >> 10) & 0x1F, f = h & 0x3FF, v; float x;
    if (e == 0) {
        x = ldexpf((float)f, -24);
        return s ? -x : x;
    }
    if (e == 31) v = s | 0x7F800000u | (f << 13) | (f ? 0x400000u : 0);
    else v = s | ((e - 15 + 127) << 23) | (f << 13);
    memcpy(&x, &v, 4); return x;
}
static u16 sw6_f2h(float x)
{
    u32 v, s, f, hf, rem, half, r; s32 e; int sh;
    memcpy(&v, &x, 4);
    s = (v >> 16) & 0x8000; e = (s32)((v >> 23) & 0xFF); f = v & 0x7FFFFF;
    if (e == 0xFF) return (u16)(s | 0x7C00 | (f ? (0x200 | (f >> 13)) : 0));
    if (e == 0) return (u16)s;                      /* float denormals underflow to 0 */
    e = e - 127 + 15;
    if (e >= 31) return (u16)(s | 0x7C00);
    if (e <= 0) {
        if (e < -10) return (u16)s;
        f |= 0x800000; sh = 14 - e;
        hf = f >> sh; rem = f & ((1u << sh) - 1); half = 1u << (sh - 1);
        if (rem > half || (rem == half && (hf & 1))) hf++;
        return (u16)(s | hf);
    }
    hf = f >> 13; rem = f & 0x1FFF;
    r = s | ((u32)e << 10) | hf;
    if (rem > 0x1000 || (rem == 0x1000 && (hf & 1))) r++;
    return (u16)r;
}
/* FCVTL: esz = wide element (32: half->single, 64: single->double) */
SW6(fcvtl) { sw_v128 r = sw6_zero(); int i; (void)d; (void)m; (void)imm;
    if (esz == 32) { for (i = 0; i < 4; i++) r.f[i] = sw6_h2f(n.h[(q ? 4 : 0) + i]); }
    else { for (i = 0; i < 2; i++) r.df[i] = (double)n.f[(q ? 2 : 0) + i]; }
    return r; }
/* FCVTN: esz = wide element; Q=1 writes the upper half, keeps the lower */
SW6(fcvtn) { sw_v128 r = q ? d : sw6_zero(); int i; (void)m; (void)imm;
    if (q) r.d[1] = 0;
    if (esz == 32) { for (i = 0; i < 4; i++) r.h[(q ? 4 : 0) + i] = sw6_f2h(n.f[i]); }
    else { for (i = 0; i < 2; i++) r.f[(q ? 2 : 0) + i] = (float)n.df[i]; }
    return r; }

/* ---- FP two-register misc: rounding and conversions ------------------------ */
static double sw6_fround(double x, char mode)
{
    switch (mode) {
    case 'n': return sw_rint64(x);
    case 'm': return floor(x);
    case 'p': return ceil(x);
    case 'z': return trunc(x);
    case 'a': return sw_round64(x);
    default: return sw_rint64(x);           /* x / i: current mode, round-to-nearest */
    }
}
#define SW6_FRINT(NAME, MODE) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = sw6_lanes(q, esz); (void)d; (void)m; (void)imm; \
        if (esz == 32) { for (i = 0; i < L; i++) r.f[i] = n.f[i] != n.f[i] ? n.f[i] + n.f[i] : (float)sw6_fround((double)n.f[i], MODE); } \
        else { for (i = 0; i < L; i++) r.df[i] = n.df[i] != n.df[i] ? n.df[i] + n.df[i] : sw6_fround(n.df[i], MODE); } \
        return r; }
SW6_FRINT(f2x_frintn, 'n')
SW6_FRINT(f2x_frintm, 'm')
SW6_FRINT(f2x_frintp, 'p')
SW6_FRINT(f2x_frintz, 'z')
SW6_FRINT(f2x_frinta, 'a')
SW6_FRINT(f2x_frintx, 'x')
SW6_FRINT(f2x_frinti, 'i')
/* FP -> integer with saturation; the scale (2^imm) is exact in double */
#define SW6_FCVT(NAME, MODE, SIGNED) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = sw6_lanes(q, esz); double k = ldexp(1.0, imm); (void)d; (void)m; \
        for (i = 0; i < L; i++) { double x = esz == 32 ? (double)n.f[i] * k : n.df[i] * k; x = sw6_fround(x, MODE); \
            if (esz == 32) r.s[i] = SIGNED ? (u32)sw_sat_s32(x) : sw_sat_u32(x); \
            else r.d[i] = SIGNED ? (u64)sw_sat_s64(x) : sw_sat_u64(x); } \
        return r; }
SW6_FCVT(f2x_fcvtns, 'n', 1)
SW6_FCVT(f2x_fcvtms, 'm', 1)
SW6_FCVT(f2x_fcvtas, 'a', 1)
SW6_FCVT(f2x_fcvtps, 'p', 1)
SW6_FCVT(f2x_fcvtzs, 'z', 1)
SW6_FCVT(f2x_fcvtnu, 'n', 0)
SW6_FCVT(f2x_fcvtmu, 'm', 0)
SW6_FCVT(f2x_fcvtau, 'a', 0)
SW6_FCVT(f2x_fcvtpu, 'p', 0)
SW6_FCVT(f2x_fcvtzu, 'z', 0)
SW6_FCVT(fcvtzsx, 'z', 1)
SW6_FCVT(fcvtzux, 'z', 0)
/* fixed point -> FP: round the integer conversion, then the exact 2^-imm scale */
SW6(scvtfx) { sw_v128 r = sw6_zero(); int i, L = sw6_lanes(q, esz); (void)d; (void)m;
    for (i = 0; i < L; i++) {
        if (esz == 32) r.f[i] = (float)((double)(float)(s32)n.s[i] / ldexp(1.0, imm));
        else r.df[i] = (double)(s64)n.d[i] / ldexp(1.0, imm); }
    return r; }
SW6(ucvtfx) { sw_v128 r = sw6_zero(); int i, L = sw6_lanes(q, esz); (void)d; (void)m;
    for (i = 0; i < L; i++) {
        if (esz == 32) r.f[i] = (float)((double)(float)n.s[i] / ldexp(1.0, imm));
        else r.df[i] = sw_u64_to_f64(n.d[i]) / ldexp(1.0, imm); }
    return r; }

/* ---- across lanes ---------------------------------------------------------- */
#define SW6_ACROSS(NAME, T, GET, RES_ESZ, INIT, STEP) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = sw6_lanes(q, esz); T acc = INIT; (void)d; (void)m; (void)imm; \
        for (i = 0; i < L; i++) { T v = GET(n, esz, i); acc = (STEP); } \
        sw6_put(&r, RES_ESZ, 0, (u64)acc); return r; }
SW6_ACROSS(saddlv, s64, sw6_gs, 2 * esz, 0, acc + v)
SW6_ACROSS(uaddlv, u64, sw6_gu, 2 * esz, 0, acc + v)
SW6_ACROSS(smaxv, s64, sw6_gs, esz, sw6_gs(n, esz, 0), v > acc ? v : acc)
SW6_ACROSS(sminv, s64, sw6_gs, esz, sw6_gs(n, esz, 0), v < acc ? v : acc)
SW6_ACROSS(umaxv, u64, sw6_gu, esz, 0, v > acc ? v : acc)
SW6_ACROSS(uminv, u64, sw6_gu, esz, ~0ull, v < acc ? v : acc)
/* FP reductions are a pairwise tree in the Arm ARM (Reduce()) */
#define SW6_FACROSS(NAME, OP, NM) \
    SW6(NAME) { sw_v128 r = sw6_zero(); (void)d; (void)m; (void)imm; (void)q; (void)esz; \
        r.f[0] = OP(OP(n.f[0], n.f[1], NM), OP(n.f[2], n.f[3], NM), NM); return r; }
SW6_FACROSS(fmaxnmv, sw6_fmax32, 1)
SW6_FACROSS(fminnmv, sw6_fmin32, 1)
SW6_FACROSS(fmaxv, sw6_fmax32, 0)
SW6_FACROSS(fminv, sw6_fmin32, 0)

/* ---- shift by immediate (same width) --------------------------------------- */
#define SW6_SH(NAME, EXPR) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = sw6_lanes(q, esz); u64 em = sw6_umax(esz); (void)m; (void)em; \
        for (i = 0; i < L; i++) { s64 s = sw6_gs(n, esz, i); u64 u = sw6_gu(n, esz, i), dv = sw6_gu(d, esz, i); \
            (void)s; (void)u; (void)dv; sw6_put(&r, esz, i, (u64)(EXPR)); } \
        return r; }
SW6_SH(sshr, sw6_asr(s, imm))
SW6_SH(ushr, sw6_lsr(u, imm))
SW6_SH(ssra, dv + (u64)sw6_asr(s, imm))
SW6_SH(usra, dv + sw6_lsr(u, imm))
SW6_SH(srshr, sw6_rasr(s, imm))
SW6_SH(urshr, sw6_rlsr(u, imm))
SW6_SH(srsra, dv + (u64)sw6_rasr(s, imm))
SW6_SH(ursra, dv + sw6_rlsr(u, imm))
SW6_SH(sri, imm >= esz ? dv : ((dv & ~(em >> imm)) | (u >> imm)))
SW6_SH(shl, u << imm)
SW6_SH(sli, (dv & ~((em << imm) & em)) | (u << imm))
static s64 sw6_sqshl1(s64 s, int sh, int esz)
{
    s64 hi = sw6_slimhi(esz), lo = sw6_slimlo(esz);
    if (sh == 0) return s;
    if (s > (hi >> sh)) return hi;
    if (s < (lo >> sh)) return lo;
    return (s64)((u64)s << sh);
}
static u64 sw6_uqshl1(u64 u, int sh, int esz)
{
    if (sh == 0) return u;
    if (u > (sw6_umax(esz) >> sh)) return sw6_umax(esz);
    return u << sh;
}
SW6_SH(sqshli, sw6_sqshl1(s, imm, esz))
SW6_SH(uqshli, sw6_uqshl1(u, imm, esz))
SW6_SH(sqshlui, s < 0 ? 0 : sw6_uqshl1((u64)s, imm, esz))

/* ---- three different: esz = narrow element ---------------------------------- */
#define SW6_LONG(NAME, T, GET, EXPR) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = 64 / esz, off = q ? L : 0; (void)imm; \
        for (i = 0; i < L; i++) { T a = GET(n, esz, off + i), b = GET(m, esz, off + i); \
            u64 dv = sw6_gu(d, 2 * esz, i); (void)dv; sw6_put(&r, 2 * esz, i, (u64)(EXPR)); } \
        return r; }
SW6_LONG(saddl, s64, sw6_gs, a + b)
SW6_LONG(uaddl, u64, sw6_gu, a + b)
SW6_LONG(ssubl, s64, sw6_gs, a - b)
SW6_LONG(usubl, u64, sw6_gu, a - b)
SW6_LONG(sabdl, s64, sw6_gs, a > b ? a - b : b - a)
SW6_LONG(uabdl, u64, sw6_gu, a > b ? a - b : b - a)
SW6_LONG(sabal, s64, sw6_gs, dv + (u64)(a > b ? a - b : b - a))
SW6_LONG(uabal, u64, sw6_gu, dv + (a > b ? a - b : b - a))
SW6_LONG(smull, s64, sw6_gs, a * b)
SW6_LONG(umull, u64, sw6_gu, a * b)
SW6_LONG(smlal, s64, sw6_gs, dv + (u64)(a * b))
SW6_LONG(umlal, u64, sw6_gu, dv + a * b)
SW6_LONG(smlsl, s64, sw6_gs, dv - (u64)(a * b))
SW6_LONG(umlsl, u64, sw6_gu, dv - a * b)
/* wide (n) op narrow (m half) */
#define SW6_WIDE(NAME, T, GET, EXPR) \
    SW6(NAME) { sw_v128 r = sw6_zero(); int i, L = 64 / esz, off = q ? L : 0; (void)d; (void)imm; \
        for (i = 0; i < L; i++) { T a = GET(n, 2 * esz, i), b = GET(m, esz, off + i); \
            sw6_put(&r, 2 * esz, i, (u64)(EXPR)); } \
        return r; }
SW6_WIDE(saddw, s64, sw6_gs, a + b)
SW6_WIDE(uaddw, u64, sw6_gu, a + b)
SW6_WIDE(ssubw, s64, sw6_gs, a - b)
SW6_WIDE(usubw, u64, sw6_gu, a - b)
/* high narrow: (n op m [+ round]) >> esz, n and m wide */
#define SW6_HN(NAME, SUB, ROUND) \
    SW6(NAME) { sw_v128 r = q ? d : sw6_zero(); int i, L = 64 / esz, off = q ? L : 0; (void)imm; \
        if (q) r.d[1] = 0; \
        for (i = 0; i < L; i++) { u64 a = sw6_gu(n, 2 * esz, i), b = sw6_gu(m, 2 * esz, i); \
            u64 v = (SUB ? a - b : a + b) + (ROUND ? 1ull << (esz - 1) : 0); \
            sw6_put(&r, esz, off + i, (v & sw6_umax(2 * esz)) >> esz); } \
        return r; }
SW6_HN(addhn, 0, 0)
SW6_HN(raddhn, 0, 1)
SW6_HN(subhn, 1, 0)
SW6_HN(rsubhn, 1, 1)
/* PMULL / PMULL2: esz 8 (8b -> 8h) or 64 (1d -> 1q), carry-less */
SW6(pmull) { sw_v128 r = sw6_zero(); int i, j; (void)d; (void)imm;
    if (esz == 8) {
        for (i = 0; i < 8; i++) { u32 a = n.b[(q ? 8 : 0) + i], b = m.b[(q ? 8 : 0) + i], p = 0;
            for (j = 0; j < 8; j++) if ((b >> j) & 1) p ^= a << j;
            r.h[i] = (u16)p; }
    } else {
        u64 a = n.d[q], b = m.d[q], lo = 0, hi = 0;
        for (j = 0; j < 64; j++) if ((b >> j) & 1) { lo ^= a << j; if (j) hi ^= a >> (64 - j); }
        r.d[0] = lo; r.d[1] = hi;
    }
    return r; }

/* ---- CRC32 / CRC32C (reflected, no pre/post inversion, as the instructions) */
static u32 sw_crc32(u32 acc, u64 val, int nbytes, int castagnoli)
{
    u32 poly = castagnoli ? 0x82F63B78u : 0xEDB88320u; int i, k;
    for (i = 0; i < nbytes; i++) {
        acc ^= (u32)(val >> (8 * i)) & 0xFF;
        for (k = 0; k < 8; k++) acc = (acc >> 1) ^ (poly & (0u - (acc & 1)));
    }
    return acc;
}

#endif /* SW_SIMD6_H */
