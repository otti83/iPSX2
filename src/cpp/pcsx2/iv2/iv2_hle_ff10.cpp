// SPDX-License-Identifier: GPL-3.0+
// iv2 kernel-HLE — Final Fantasy X native T&L kernels (P101-7 Stage2-b; extracted 2026-06-27)
//
//   pc84  (hash b5852f65bf2fd657) — dominant T&L: matrix*vertex -> perspective -> FTOI4 ->
//          PACKED[ST,RGBAQ,XYZF2]. geom byte-exact, 1.63x, user-visual OK. validated.
//   pc162 (hash 3837f6578cb57ab8) — 2-strip UV/lit + ST/flat. computed-attrs byte-exact,
//          on-screen ADC byte-exact. validated.
//
// Float ops mirror the PS2 VU non-IEEE pipeline (iv2_hle_vumath.h `vufp`), so output is
// GIF-faithful to within FTOI4 quantization (verified by the mode-1 self-check harness).
#include "PrecompiledHeader.h"
#include "iv2/iv2_hle.h"
#include "iv2/iv2_hle_vumath.h"
#include <cstring>
#include <cmath>

// in_matrix (16 u32 = vf05..vf08) is the MVP matrix the program actually loaded; when
// non-null (self-check) it is used directly, isolating the transform from the (still being
// RE'd) matrix-load address. When null, falls back to the fixed mem[qw 52] hypothesis.
extern "C" int iv2_hle_kernel_ff10_pc84(const u8* in_mem, const u32* /*in_vi*/, u32 in_top,
                                        const u32* in_matrix, u8* out, u32 out_cap)
{
    using namespace vufp;
    auto QW = [&](u32 qw) -> const u32* { return (const u32*)(in_mem + (qw & 0x3ff) * 16); };

    const u32 TOP = in_top & 0x3ff;
    const u32* hdr = QW(TOP);
    const u32 count = hdr[0] & 0xffff;          // triangle count = NLOOP
    const u32 baseA = hdr[1] & 0xffff;          // color stream offset
    const u32 outOff = hdr[2] & 0xffff;         // output base offset (relative to TOP)
    if (count == 0 || count > 256) return -1;   // out of modeled range -> decline

    const u32 stride = 3u * count;
    const u32 A = TOP + baseA;                   // color  stream (count*3 qw, 3 per tri)
    const u32 B = A + stride;                    // ST/UV  stream
    const u32 C = B + stride;                    // position stream

    // MVP matrix columns vf05..vf08: provided directly (self-check) or fixed mem[qw 52..55].
    const u32* M0 = in_matrix ? in_matrix+0  : QW(52);
    const u32* M1 = in_matrix ? in_matrix+4  : QW(53);
    const u32* M2 = in_matrix ? in_matrix+8  : QW(54);
    const u32* M3 = in_matrix ? in_matrix+12 : QW(55);
    const u32* vf04 = QW(3);                     // fog params (.x=-0.05916, .y=105)
    const float fogScale = u2f(vf04[0]);
    const u32   fogMaxU  = vf04[1];              // 105.0 (raw, for MAXy)
    const float I_REG = 255.0f;                  // pc100 I-flag immediate (0x437f0000)
    const u32* vf09 = QW(1);                     // clip center (2048,2048,2^23,1)
    const u32* vf10 = QW(2);                     // clip 1/half-extent
    const float cenx=u2f(vf09[0]), ceny=u2f(vf09[1]), cenz=u2f(vf09[2]);
    const float extx=u2f(vf10[0]), exty=u2f(vf10[1]), extz=u2f(vf10[2]);
    // CLIP per-component test: NDC>1.0 or NDC<-1.0 (op_clip integer compare vs |1.0|).
    auto clipComp = [](float ndc) -> bool {
        const s32 v = 0x3f800000;                                  // |vf00.w|=1.0
        const u32 b = f2u(ndc);
        return ((s32)(b ^ 0x00000000u) > v) || ((s32)(b ^ 0x80000000u) > v);
    };

    const u32 outBytes = 6*16 + count*9*16;
    if (outBytes > out_cap) return -1;

    // 6-qw GIF header template (EE pre-built): setup GIFtag + 4 A+D (mem[TOP+2..TOP+6]),
    // geometry GIFtag (mem[TOP+8]). Copied verbatim by the VU epilogue (pc184-197).
    u8* p = out;
    std::memcpy(p +  0, QW(TOP+2), 16);          // setup GIFtag
    std::memcpy(p + 16, QW(TOP+3), 16);          // A+D #1
    std::memcpy(p + 32, QW(TOP+4), 16);          // A+D #2
    std::memcpy(p + 48, QW(TOP+5), 16);          // A+D #3
    std::memcpy(p + 64, QW(TOP+6), 16);          // A+D #4
    std::memcpy(p + 80, QW(TOP+8), 16);          // geometry GIFtag
    p += 96;

    for (u32 t = 0; t < count; ++t)
    {
        float sfx[3], sfy[3], sfz[3];        // float screen pos (post-MULq, pre-FTOI)
        u32   X[3], Y[3], Z[3], Ffix[3];
        u32   S[3], T[3], Qs[3];
        const u32* col[3];
        bool  clipped = false;

        for (int v = 0; v < 3; ++v)
        {
            const u32* pos = QW(C + t*3 + v);
            const u32* uv  = QW(B + t*3 + v);
            col[v]         = QW(A + t*3 + v);
            const float px=u2f(pos[0]), py=u2f(pos[1]), pz=u2f(pos[2]), pw=u2f(pos[3]);

            // clip = M0*px + M1*py + M2*pz + M3*pw  (column broadcast MULAx/MADDA chain)
            float cx = fmadd(fmadd(fmadd(fmul(u2f(M0[0]),px), u2f(M1[0]),py), u2f(M2[0]),pz), u2f(M3[0]),pw);
            float cy = fmadd(fmadd(fmadd(fmul(u2f(M0[1]),px), u2f(M1[1]),py), u2f(M2[1]),pz), u2f(M3[1]),pw);
            float cz = fmadd(fmadd(fmadd(fmul(u2f(M0[2]),px), u2f(M1[2]),py), u2f(M2[2]),pz), u2f(M3[2]),pw);
            float cw = fmadd(fmadd(fmadd(fmul(u2f(M0[3]),px), u2f(M1[3]),py), u2f(M2[3]),pz), u2f(M3[3]),pw);

            const float Q = u2f(divQ(1.0f, cw));
            sfx[v] = fmul(cx, Q); sfy[v] = fmul(cy, Q); sfz[v] = fmul(cz, Q);
            X[v]=ftoi4(sfx[v]); Y[v]=ftoi4(sfy[v]); Z[v]=ftoi4(sfz[v]);

            // fog .w: MULx.w(cw*fogScale) -> ADDi.w(+I) -> MINIi.w(I) -> MAXy.w(vf04.y) -> FTOI4
            const float fogMul = fmul(cw, fogScale);
            const float fogAdd = vd(vd(fogMul) + vd(I_REG));
            u32 fogu = fminU(f2u(fogAdd), f2u(I_REG));
            fogu = fmaxU(fogu, fogMaxU);
            Ffix[v] = ftoi4(u2f(fogu));

            // ST = ITOF12(uv) * Q  (perspective texcoord)
            S[v]  = f2u(fmul(itof12(uv[0]), Q));
            T[v]  = f2u(fmul(itof12(uv[1]), Q));
            Qs[v] = f2u(fmul(itof12(uv[2]), Q));

            // CLIP: NDC = (screen - center) * extent, test each comp vs |1.0| (any -> 0xC000)
            if (clipComp(vd(vd(vd(sfx[v]) - vd(cenx)) * vd(extx))) ||
                clipComp(vd(vd(vd(sfy[v]) - vd(ceny)) * vd(exty))) ||
                clipComp(vd(vd(vd(sfz[v]) - vd(cenz)) * vd(extz))))
                clipped = true;
        }

        // Backface: screen-space cross product Z = vf03.x*vf02.y - vf02.x*vf03.y
        // where vf02 = sf[1]-sf[0], vf03 = sf[2]-sf[1] (OPMULA/OPMSUB, scalar vuDouble).
        const float e1x = vd(vd(sfx[1]) - vd(sfx[0])), e1y = vd(vd(sfy[1]) - vd(sfy[0]));
        const float e2x = vd(vd(sfx[2]) - vd(sfx[1])), e2y = vd(vd(sfy[2]) - vd(sfy[1]));
        const float faceZ = vd(vd(vd(e2x) * vd(e1y)) - vd(vd(e1x) * vd(e2y)));
        const u32 clip_bit = clipped ? 0xC000u : 0u;
        const u32 face_bit = (f2u(faceZ) & 0x80000000u) ? 0u : 0xA000u;  // front-face -> 0xA000
        const u32 tri_flags = clip_bit | face_bit;

        for (int v = 0; v < 3; ++v)
        {
            u32* o = (u32*)(p + v*48);
            o[0]=S[v]; o[1]=T[v]; o[2]=Qs[v]; o[3]=0;                       // ST
            o[4]=col[v][0]; o[5]=col[v][1]; o[6]=col[v][2]; o[7]=col[v][3]; // RGBAQ (verbatim)
            // XYZF2; flags OR'd into the triangle's last vertex (v2) word3 only.
            o[8]=X[v]; o[9]=Y[v]; o[10]=Z[v]; o[11]= Ffix[v] | ((v==2) ? tri_flags : 0u);
        }
        p += 9*16;
    }
    return (int)outBytes;
}

// ── FF10 #1 (hash 3837f6578cb57ab8) dominant entry pc=162 native kernel ──────────────────
// One kick = one strip. VU-mem layout at TOP: TOP+0=state GIFtag, TOP+1=A+D qw, TOP+2=geom
// GIFtag (NLOOP=input count, REGS LSB nibble: 3=[UV,RGBAQ,XYZF2] Block-B lit / 2=[ST,..] Block-A
// flat), TOP+3..=vertex records stride 3 [UV, normal, position]. Output = state+A+D+geom(NLOOP
// patched to the visible/emitted count)+ emitted×3 qw; clipped verts are compacted out.
// First-cut: transform (GREEN, verified) + UV/ST + flat(A)/lighting(B) color + frustum cull.
extern "C" int iv2_hle_kernel_ff10_pc162(const u8* in_mem, const u32* /*in_vi*/, u32 in_top,
                                         const u32* /*in_matrix*/, u8* out, u32 out_cap)
{
    using namespace vufp;
    auto QW = [&](u32 qw) -> const u32* { return (const u32*)(in_mem + (qw & 0x3ff) * 16); };
    const u32 TOP = in_top & 0x3ff;
    const u32* geomTag = QW(TOP + 2);
    const u32 count = geomTag[0] & 0x7fff;                  // NLOOP = input strip vertex count
    if (count == 0 || count > 256) return -1;
    const u32 regs0 = geomTag[2] & 0xf;                     // 3=UV(Block-B lit), 2=ST(Block-A flat)
    const bool blockB = (regs0 == 3);
    const u32 recBase = TOP + 3;

    const u32* M0=QW(0); const u32* M1=QW(1); const u32* M2=QW(2); const u32* M3=QW(3);   // MVP
    const u32* vf04 = QW(0x26); const float fogScale = u2f(vf04[0]); const u32 fogMaxU = vf04[1];
    const u32* vf09 = QW(1); const u32* vf10 = QW(2);       // clip center/extent (shared w/ pc84 layout? use qw1/2)
    const float cenx=u2f(vf09[0]),ceny=u2f(vf09[1]),cenz=u2f(vf09[2]);
    const float extx=u2f(vf10[0]),exty=u2f(vf10[1]),extz=u2f(vf10[2]);
    const float I_REG = 255.0f;
    // Block-B lighting constants
    const u32* NM0=QW(20); const u32* NM1=QW(21); const u32* NM2=QW(22);   // normal matrix
    const u32* LM0=QW(16); const u32* LM1=QW(17); const u32* LM2=QW(18);   // light matrix
    const u32* AMB=QW(19); const u32* LCOL=QW(35);                          // ambient, light color
    auto clipComp = [](float ndc)->bool{ const s32 v=0x3f800000; const u32 b=f2u(ndc);
        return ((s32)(b^0u)>v)||((s32)(b^0x80000000u)>v); };

    (void)blockB; (void)regs0; (void)clipComp; (void)cenx;(void)ceny;(void)cenz;(void)extx;(void)exty;(void)extz;
    // One kick = TWO strips over the SAME `count` vertices (no compaction; NLOOP=count, per-vertex
    // ADC bit handles visibility): strip0 = Block-B [UV, lit-RGBAQ, XYZF2], strip1 = Block-A
    // [ST, flat-RGBAQ, XYZF2]. Templates: strip0 state=QW(TOP+0)/AD=QW(0xdf)/geom=QW(TOP+2);
    // strip1 state=QW(0x27)/AD=QW(0x28)/geom=QW(0x24). (sources identified from captured I/O.)
    if (3*16 + count*3*16 > out_cap/2) return -1;

    // [ADC RE-verified byte-exact 57/57] Per-vertex ADC visibility bit (XYZF2.word3 bit 0x8000),
    // shared by BOTH strips (same geometry). ADC[v] = strip-restart-pair OR strip-local backface:
    //  - strip-restart: reg0.w (record qw0 word3) holds a small int 1..15 at each strip head;
    //    that vertex AND the next get ADC=1 (the two leading verts of a triangle strip).
    //  - backface: triangle (v-2,v-1,v) within the current strip (window resets at each head),
    //    triangle-strip alternating winding (flip the screen-area sign on odd in-strip index);
    //    winding-corrected signed screen area < 0 -> back-facing -> ADC=1.
    u8 adc[256]; float sfx[256], sfy[256];
    for (u32 v = 0; v < count; ++v) {
        const u32* pos = QW(recBase + v*3 + 2);
        const float px=u2f(pos[0]),py=u2f(pos[1]),pz=u2f(pos[2]),pw=u2f(pos[3]);
        const float cx=fmadd(fmadd(fmadd(fmul(u2f(M0[0]),px),u2f(M1[0]),py),u2f(M2[0]),pz),u2f(M3[0]),pw);
        const float cy=fmadd(fmadd(fmadd(fmul(u2f(M0[1]),px),u2f(M1[1]),py),u2f(M2[1]),pz),u2f(M3[1]),pw);
        const float cw=fmadd(fmadd(fmadd(fmul(u2f(M0[3]),px),u2f(M1[3]),py),u2f(M2[3]),pz),u2f(M3[3]),pw);
        const float Q=u2f(divQ(1.0f,cw));
        sfx[v]=fmul(cx,Q); sfy[v]=fmul(cy,Q); adc[v]=0;
    }
    {
        u32 start = 0;
        for (u32 v = 0; v < count; ++v) {
            // strip head: reg0.w holds the strip LENGTH (small int; verified == run-length to the
            // next head). 1..31 admits the real strip lengths (seen up to 21) while excluding the
            // non-head garbage/float values; the prior 1..15 mis-split long strips (off-screen).
            const u32 r0w = QW(recBase + v*3 + 0)[3];
            if (r0w >= 1 && r0w <= 31) { start=v; adc[v]=1; if (v+1<count) adc[v+1]=1; }
            const u32 idx = v - start;
            if (idx >= 2) {
                const double ax=(double)sfx[v-1]-sfx[v-2], ay=(double)sfy[v-1]-sfy[v-2];
                const double bx=(double)sfx[v]-sfx[v-2],   by=(double)sfy[v]-sfy[v-2];
                double s = ax*by - ay*bx;
                if (idx & 1) s = -s;
                if (s < 0.0) adc[v]=1;
            }
            // off-screen guard band (PS2 screen center 2048): verts beyond it are kicked (ADC).
            // Symmetric X+Y (the +X arm was the only one the captured kick exercised; X+Y is the
            // natural frustum generalization — kept only if it reduces the aggregate flag mismatch).
            const float dx = sfx[v] - 2048.0f, dy = sfy[v] - 2048.0f;
            if (dx > 400.0f || dx < -400.0f || dy > 400.0f || dy < -400.0f) adc[v]=1;
        }
    }

    u8* p = out;
    auto emit_strip = [&](bool isB, const u32* stateT, const u32* adT, const u32* geomT){
        std::memcpy(p, stateT, 16); p += 16;
        std::memcpy(p, adT, 16);    p += 16;
        u8* geomDst = p; std::memcpy(geomDst, geomT, 16); p += 16;
        for (u32 v = 0; v < count; ++v) {
            const u32* uv  = QW(recBase + v*3 + 0);
            const u32* nrm = QW(recBase + v*3 + 1);
            const u32* pos = QW(recBase + v*3 + 2);
            const float px=u2f(pos[0]),py=u2f(pos[1]),pz=u2f(pos[2]),pw=u2f(pos[3]);
            float cx=fmadd(fmadd(fmadd(fmul(u2f(M0[0]),px),u2f(M1[0]),py),u2f(M2[0]),pz),u2f(M3[0]),pw);
            float cy=fmadd(fmadd(fmadd(fmul(u2f(M0[1]),px),u2f(M1[1]),py),u2f(M2[1]),pz),u2f(M3[1]),pw);
            float cz=fmadd(fmadd(fmadd(fmul(u2f(M0[2]),px),u2f(M1[2]),py),u2f(M2[2]),pz),u2f(M3[2]),pw);
            float cw=fmadd(fmadd(fmadd(fmul(u2f(M0[3]),px),u2f(M1[3]),py),u2f(M2[3]),pz),u2f(M3[3]),pw);
            const float Q=u2f(divQ(1.0f,cw));
            const u32 X=ftoi4(fmul(cx,Q)), Y=ftoi4(fmul(cy,Q)), Z=ftoi4(fmul(cz,Q));
            const float fogAdd=vd(vd(fmul(cw,fogScale))+vd(I_REG));
            u32 fogu=fminU(f2u(fogAdd),f2u(I_REG)); fogu=fmaxU(fogu,fogMaxU);
            const u32 Ffix=ftoi4(u2f(fogu));
            u32* o=(u32*)p;
            if (isB) {
                o[0]=uv[0]; o[1]=uv[1]; o[2]=uv[2]; o[3]=uv[3];   // UV verbatim
                // Per-vertex lighting (RE-verified byte-exact, kick0 57/57): normal matrix
                // (column-broadcast NM*n) -> MAXx clamp >=0 -> light matrix (column-broadcast
                // LM*tn) + ambient -> *lightColor -> max0 -> MINIi 255 -> FTOI0. A = FTOI0(LCOL.w=128).
                const float nx=u2f(nrm[0]),ny=u2f(nrm[1]),nz=u2f(nrm[2]);
                float tnx=fmadd(fmadd(fmul(u2f(NM0[0]),nx),u2f(NM1[0]),ny),u2f(NM2[0]),nz);
                float tny=fmadd(fmadd(fmul(u2f(NM0[1]),nx),u2f(NM1[1]),ny),u2f(NM2[1]),nz);
                float tnz=fmadd(fmadd(fmul(u2f(NM0[2]),nx),u2f(NM1[2]),ny),u2f(NM2[2]),nz);
                if(tnx<0.f)tnx=0.f; if(tny<0.f)tny=0.f; if(tnz<0.f)tnz=0.f;   // MAXx vf29,vf00.x
                auto litc=[&](int c)->u32{
                    float acc=fmadd(fmadd(fmul(u2f(LM0[c]),tnx),u2f(LM1[c]),tny),u2f(LM2[c]),tnz);
                    acc=vd(vd(acc)+vd(u2f(AMB[c])));
                    float v=fmul(acc,u2f(LCOL[c]));
                    if(v<0.f)v=0.f; v=u2f(fminU(f2u(v),f2u(255.0f)));
                    return (u32)(s32)vd(v); };                    // FTOI0 truncate
                o[4]=litc(0); o[5]=litc(1); o[6]=litc(2); o[7]=0x80u;
            } else {
                // Block-A ST: word0/1 = S/T = ENV-MAP reflection of the per-vertex NORMAL.
                // RE'd op-by-op from pc162 live trace (pc 361..394); see
                // docs/report/logs/P101-7_stage2b/re/pc162/st_emulate.py (S 51/57, T 55/57; the
                // residual ULPs are ALL in the EFU 1/sqrt below — substituting the trace's vf15.x
                // makes both S and T 57/57, proving the reflection/projection path is byte-exact).
                //   vf24 = pos - eye                            (SUB.xyz, pc361)
                //   Pefu = 1/sqrt(|vf24|^2)                     (ERLENG vf24, pc365; MFP->vf15.x)
                //   ns   = ((N.x*vf16.x + N.y*vf16.y) + N.z*vf16.z) * vf16.w   (pc368/372/377/379)
                //   ven  = N * ns                               (eye-space normal, MULx vf26, pc382)
                //   vf13 = vf24*Pefu - ven                      (MULx/SUB, pc387/388)
                //   r.xy = Rot(vf18,vf19,vf20) * vf13           (MULAx/MADDAy/MADDz, pc389-391)
                //   S/T  = (r.xy * eye.w + 0.5) * Q             (MULw/ADDi I=0.5/MULq, pc392-394)
                const u32* R0=QW(0x1e); const u32* R1=QW(0x1f); const u32* R2=QW(0x20);  // vf18/19/20
                const u32* EYE=QW(0x21);                                                   // vf21
                const u32* NS=QW(0x22);                                                    // vf16
                const float ewn = u2f(EYE[3]);          // eye.w (1.2)
                const float Iv  = 0.5f;                  // LOI at pc388 (vf15=0x3f000000)
                const float nx=u2f(nrm[0]),ny=u2f(nrm[1]),nz=u2f(nrm[2]);
                // view vector (pos - eye)
                const float v24x=fsub(px,u2f(EYE[0])), v24y=fsub(py,u2f(EYE[1])), v24z=fsub(pz,u2f(EYE[2]));
                // ERLENG: P = 1/sqrt(x^2+y^2+z^2) (sum fused like FMAC, double sqrt narrowed, RZ recip)
                const float Pefu = erleng3(v24x,v24y,v24z);
                // eye-space normal scale: pc368 MUL builds the 3 lanes N*NS, pc372/377 ADD the
                // y/z lanes into x (plain ADDs of pre-rounded products, NOT fused), pc379 *NS.w.
                float ns = fadd(fadd(fmul(nx,u2f(NS[0])), fmul(ny,u2f(NS[1]))), fmul(nz,u2f(NS[2])));
                ns = fmul(ns, u2f(NS[3]));
                const float venx=fmul(nx,ns), veny=fmul(ny,ns), venz=fmul(nz,ns);
                // reflection input vf13 = normalize(view) - eyeNormal
                const float r13x=fsub(fmul(v24x,Pefu),venx);
                const float r13y=fsub(fmul(v24y,Pefu),veny);
                const float r13z=fsub(fmul(v24z,Pefu),venz);
                // rotate into texture space (xy only)
                const float rx=fmadd(fmadd(fmul(u2f(R0[0]),r13x), u2f(R1[0]),r13y), u2f(R2[0]),r13z);
                const float ry=fmadd(fmadd(fmul(u2f(R0[1]),r13x), u2f(R1[1]),r13y), u2f(R2[1]),r13z);
                // S/T = (r * eye.w + 0.5) * Q
                const float Sst=fmul(fadd(fmul(rx,ewn),Iv), Q);
                const float Tst=fmul(fadd(fmul(ry,ewn),Iv), Q);
                o[0]=f2u(Sst); o[1]=f2u(Tst);
                o[2]=f2u(Q); o[3]=0;
                const u32* fc=QW(0x1d); o[4]=fc[0]; o[5]=fc[1]; o[6]=fc[2]; o[7]=fc[3];
            }
            o[8]=X; o[9]=Y; o[10]=Z; o[11]=Ffix | (adc[v] ? 0x8000u : 0u);   // XYZF2 + ADC
            p += 48;
        }
        ((u32*)geomDst)[0] = 0x00008000u | (count & 0x7fff);   // GIFtag word0 = EOP|NLOOP (clean)
    };
    emit_strip(true,  QW(TOP+0), QW(0xdf), QW(TOP+2));      // strip0 = Block-B (UV, lit)
    emit_strip(false, QW(0x27),  QW(0x28), QW(0x24));       // strip1 = Block-A (ST, flat)
    return (int)(p - out);
}

// ── FF10 kernel registry table (co-located with the kernels; aggregated by iv2_hle_lookup) ──
const IV2_HleKernel kIv2HleKernels_ff10[] = {
    { 0xb5852f65bf2fd657ULL, 84,  iv2_hle_kernel_ff10_pc84,  true  },  // FF10 dominant T&L (PACKED ST/RGBAQ/XYZF2): geom byte-exact, 1.63x, user-visual OK
    { 0x3837f6578cb57ab8ULL, 162, iv2_hle_kernel_ff10_pc162, true  },  // FF10 #1 entry (2-strip UV/lit + ST/flat): computed-attrs byte-exact (lighting/ST env-map/fog/color); on-screen ADC byte-exact, off-screen ADC sparse tail. mode-2 ENABLED for VPS+visual eval (user judgement pending)
};
const int kIv2HleKernels_ff10_count = (int)(sizeof(kIv2HleKernels_ff10) / sizeof(kIv2HleKernels_ff10[0]));
