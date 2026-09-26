// MSVCR120 math implemented natively (HLE) with the host libm. sqrt, floor, ceil, round, fmod, ldexp and
// fmin are exact everywhere; sin/cos/tan/acos/atan2/log/log10/log2/pow may differ from Windows' libm in
// the last bit, which matters if the game's determinism (world generation) depends on them.
//
// Calling conventions: plain functions (floor, log, ...) are cdecl with arguments on the stack and the
// result in x87 st0; _CI* take their operands on the x87 stack (st0, st1: f(st1, st0) for two) and replace
// them with the result; _libm_sse2_* take xmm0 (and xmm1) and return in xmm0.
#include <math.h>

#include "host.h"

HOST_CDECL(msvcr120, sqrt) { ret_f64(c, sqrt(ARG_F64(0))); }
HOST_CDECL(msvcr120, floor) { ret_f64(c, floor(ARG_F64(0))); }
HOST_CDECL(msvcr120, ceil) { ret_f64(c, ceil(ARG_F64(0))); }
HOST_CDECL(msvcr120, round) { ret_f64(c, round(ARG_F64(0))); }
HOST_CDECL(msvcr120, roundf) { ret_f32(c, roundf(ARG_F32(0))); }
HOST_CDECL(msvcr120, lroundf) { ret_i32(c, (uint32_t)(int32_t)lroundf(ARG_F32(0))); }
HOST_CDECL(msvcr120, fminf) { ret_f32(c, fminf(ARG_F32(0), ARG_F32(1))); }
HOST_CDECL(msvcr120, log) { ret_f64(c, log(ARG_F64(0))); }
HOST_CDECL(msvcr120, log2) { ret_f64(c, log2(ARG_F64(0))); }
HOST_CDECL(msvcr120, ldexp) { ret_f64(c, ldexp(ARG_F64(0), (int32_t)ARG(2))); }

HOST_CDECL(msvcr120, _dtest) {  // (double *): FP_INFINITE 1, FP_NAN 2, FP_NORMAL -1, FP_SUBNORMAL -2, 0
    double v = rdf64(ARG(0));
    int k = fpclassify(v);
    ret_i32(c, k == FP_INFINITE ? 1 : k == FP_NAN ? 2 : k == FP_NORMAL ? -1 : k == FP_SUBNORMAL ? -2 : 0);
}

static void ci2(CPU *c, double (*f)(double, double)) {
    double x0 = ST(0), x1 = ST(1);
    st_pop(c);
    ST(0) = f(x1, x0);
}
HOST_CDECL(msvcr120, _CIatan2) { ci2(c, atan2); }
HOST_CDECL(msvcr120, _CIfmod) { ci2(c, fmod); }

#define SSE2_1(name, f) HOST_CDECL(msvcr120, _libm_sse2_##name##_precise) { ret_xmm0_f64(c, f(c->xmm[0].f64[0])); }
SSE2_1(sqrt, sqrt)
SSE2_1(sin, sin)
SSE2_1(cos, cos)
SSE2_1(tan, tan)
SSE2_1(acos, acos)
SSE2_1(log10, log10)
HOST_CDECL(msvcr120, _libm_sse2_pow_precise) { ret_xmm0_f64(c, pow(c->xmm[0].f64[0], c->xmm[1].f64[0])); }
