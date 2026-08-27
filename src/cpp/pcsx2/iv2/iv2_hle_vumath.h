// SPDX-License-Identifier: GPL-3.0+
// iv2 kernel-HLE — VU1 non-IEEE float primitive library (shared by all native T&L kernels)
//
// Reproduces the PS2 VU FMAC pipeline's non-IEEE arithmetic so a native C++ kernel can match
// the interpreter's GIF output to within FTOI4 quantization:
//   vuDouble  : denormal -> signed zero, Inf/NaN -> sign|0x7f7fffff (overflow-check ON)
//   fmul/fadd/fsub/fmadd : FMAC ops with vuDouble-filtered inputs + MAC-clamped result
//   ftoi4 / itof12 / divQ / erleng3 / fminU / fmaxU : packed-vertex output primitives
//
// Formerly `namespace ff10k` (FF10-only origin); renamed `vufp` (VU float-prim) since the
// GTA VC kernels share the same primitives. Header-only (static inline) = zero ship cost.
#pragma once

#include "common/Pcsx2Types.h"
#include <cstring>
#include <cmath>

namespace vufp {
    static inline u32  f2u(float f){ u32 u; std::memcpy(&u,&f,4); return u; }
    static inline float u2f(u32 u){ float f; std::memcpy(&f,&u,4); return f; }
    // vuDouble (VU1: overflow check ON): denormal->signed zero, Inf/NaN->sign|0x7f7fffff.
    static inline u32 vuDoubleU(u32 u){
        const u32 e = u & 0x7f800000;
        if (e == 0)            return u & 0x80000000;                 // denormal -> ±0
        if (e == 0x7f800000)   return (u & 0x80000000) | 0x7f7fffff;  // Inf/NaN -> ±FLT_MAX
        return u;
    }
    static inline float vd(float f){ return u2f(vuDoubleU(f2u(f))); }
    // FMAC primitives (inputs filtered by vuDouble, result MAC-clamped by vuDouble).
    static inline float fmul(float a,float b){ return vd(vd(a)*vd(b)); }
    static inline float fadd(float a,float b){ return vd(vd(a)+vd(b)); }
    static inline float fsub(float a,float b){ return vd(vd(a)-vd(b)); }
    static inline float fmadd(float acc,float a,float b){ return vd(vd(acc)+vd(a)*vd(b)); } // _vuOpMADD
    // [P101-9] NEON 4-lane matvec was tried (byte-exact) but measured 0.97-0.99x = NO gain: the kernel
    // matvec is not the heavy-scene bottleneck (Amdahl; GS/other-VU1/dispatch dominate) and the vuDouble
    // per-op vectorization overhead offsets the 4-lane FMA savings. Reverted to scalar. See progress report.
    // ERLENG: P = 1/sqrt(x^2+y^2+z^2). Sum uses the same fused MADC chain as the FMAC matvec
    // (proven byte-exact for the rest of the kernel); sqrt is the host double sqrt narrowed to
    // float; the 1/p reciprocal matches _vuERLENG (no final vuDouble on the P register).
    static inline float erleng3(float x,float y,float z){
        // *** SUM ORDER = x,y,z *** (matches interp _vuERLENG; verified bit-exact vs live vf15.x P
        // register on all 57 verts. zyx order is 1 ULP high on 8 verts -> the prior "EFU ceiling").
        const float s = fmadd(fmadd(fmul(x,x), y,y), z,z);  // x*x + y*y + z*z
        float p = (float)std::sqrt((double)vd(s));
        if (p != 0.0f) p = 1.0f / p;
        return p;
    }
    // _vuDIV: q = fs/ft with vuDouble; div-by-zero -> sign|0x7f7fffff.
    static inline u32 divQ(float num, float den){
        const u32 du = vuDoubleU(f2u(den));
        if ((du & 0x7fffffff) == 0) return ((f2u(num) ^ du) & 0x80000000) | 0x7f7fffff;
        return f2u(vd(num) / vd(den));
    }
    // FTOI4: float*16 truncated to s32, saturate at exp>=0x4f000000.
    static inline u32 ftoi4(float f){
        const float v = vd(f) * 16.0f;
        const u32 b = f2u(v);
        if ((b & 0x7f800000) >= 0x4f000000) return (b & 0x80000000) ? 0x80000000u : 0x7fffffffu;
        return (u32)(s32)v;
    }
    static inline float itof12(u32 u){ return (float)(s32)u * 0.000244140625f; } // *2^-12 (no vuDouble)
    // MINIi/MAXi use raw s32-magnitude compare (interp applyMinMax: raw bits, not vuDouble).
    static inline u32 fminU(u32 a,u32 b){
        const bool na=(a>>31)&1, nb=(b>>31)&1;
        if (na!=nb) return na? a : b;                  // negative < positive
        const bool less = na ? ((a&0x7fffffff)>(b&0x7fffffff)) : ((a&0x7fffffff)<(b&0x7fffffff));
        return less? a : b;
    }
    static inline u32 fmaxU(u32 a,u32 b){
        const bool na=(a>>31)&1, nb=(b>>31)&1;
        if (na!=nb) return na? b : a;
        const bool gt = na ? ((a&0x7fffffff)<(b&0x7fffffff)) : ((a&0x7fffffff)>(b&0x7fffffff));
        return gt? a : b;
    }
}
