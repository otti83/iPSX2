// SPDX-License-Identifier: GPL-3.0+
// iv2 kernel-HLE — Grand Theft Auto: Vice City native T&L kernels (P101-9; extracted 2026-06-27)
//
//   pc1454 (hash 8ac5daebfa578107) — dominant 4-bone SKINNED T&L + 2-light lighting (37% VU1).
//          geom/ST/header byte-exact (3.2M verts); user-visual PASS. validated.
//   pc1435 (hash 92d542c43042231c) — 4-bone SKINNED, MVP vf28-31, persp folded, color=scale (26%).
//          geom/ST/RGBAQ 100% byte-exact (276k verts). validated.
//   f8ea / 603a — NON-skinned static-geometry T&L. RE'd byte-exact BUT the EXECUTED route is an
//          in-VU frustum CLIPPER (output count = runtime clip survivors), so NOT mode-2-feasible.
//          validated=false (dormant); kept as RE record. See re/gtaf8ea, re/gta603a, P101-9 §3/§5.
//
// Skinned positions/MVP are COMPUTED in-program (§5.7 read/write-set proved eligible); the GTA MVP
// lives in VF regs (g_hle_snap_vf), unlike FF10's mem-resident matrix. Float math = iv2_hle_vumath.h.
#include "PrecompiledHeader.h"
#include "iv2/iv2_hle.h"
#include "iv2/iv2_hle_vumath.h"
#include <cstring>
#include <cmath>

// ── GTA VC (hash 8ac5daebfa578107) dominant entry pc=1454 — DECLINES (architectural blocker) ──
// RE = byte-exact PROVEN (X/Y/Z/F/R/G/B/A 42/42, S/T/Q <=1 ULP) from the live op-trace; the math,
// the global MVP (VU VF regs vf23-26, entry-derivable), and all constants are pinned. See
// docs/report/logs/P101-7_stage2b/re/gta1454/GTA1454_STATUS.md + INDEP_ADVERSARIAL_FINDINGS.txt.
//
// BUT this kernel CANNOT run at the pc=1454 entry hook: GTA delivers the per-vertex stream
// (position/ST/color/normal) via a VIF unpack that completes DURING program execution, so at the
// entry hook those bytes are NOT in VU mem (adversarially confirmed: 0/42 positions present in the
// entry snapshot under any base/stride/matrix). FF10 was HLE-able because the EE pre-built every
// ── GTA VC (hash 8ac5daebfa578107) dominant T&L pc=1454 native kernel ──────────────────────
// [P101-9 Phase 2] 4-BONE SKINNED geometry, RE'd byte-exact from the op-trace (read/write-set
// eligibility PASS: all inputs resident-or-computed-from-resident; P101-7's "absent at entry" was an
// addressing misread of the COMPUTED positions). Pipeline (verified 42/42 XYZF2 byte-exact offline):
//   per-vertex record stride 5 qw at XTOP: qw0=local pos, qw1=ST(S,T,..), qw2=color(raw),
//     qw3=normal, qw4=weights (hi float) + 4 bone indices (low16 of each lane).
//   skin matrix row r (r=0..3) = sum_k bone[(idx_k + roff[r]) & 0x3ff] * weight_k, roff={719,716,717,718}.
//   world = srow0 + srow1*lpx + srow2*lpy + srow3*lpz.
//   clip = MVP(entry vf23-26) * world (z-lane skips x/y MADDA); Q = persp(vf27.z)/clip.w;
//   X=FTOI4(clip.x*Q - vf22.x), Y/Z likewise; F=FTOI4(clamp(clip.w,vf22.w,vf27.w) - vf22.w).
//   ST = rec_st * Q (byte-exact). COLOR = multi-pass lighting (NOT yet RE'd) -> placeholder
//   (geometry self-check unaffected; color is a separate field = computed-attribute residual).
// MVP/tail consts read from g_hle_snap_vf (entry VF, before skinning overwrites vf20-23). Inputs
// fingerprinted (count + GIFtag NLOOP) -> default-deny decline. See GTA1454_STATUS.md.
extern "C" int iv2_hle_kernel_gta_pc1454(const u8* in_mem, const u32* in_vi, u32 in_top,
                                         const u32* /*in_matrix*/, u8* out, u32 out_cap)
{
    using namespace vufp;
    if (!in_mem || !in_vi) return -1;
    auto QW = [&](u32 qw) -> const u32* { return (const u32*)(in_mem + (qw & 0x3ff) * 16); };

    const u32* c23 = &g_hle_snap_vf[23*4]; const u32* c24 = &g_hle_snap_vf[24*4];
    const u32* c25 = &g_hle_snap_vf[25*4]; const u32* c26 = &g_hle_snap_vf[26*4];
    const u32* v22 = &g_hle_snap_vf[22*4]; const u32* v27 = &g_hle_snap_vf[27*4];
    const float persp = u2f(v27[2]);

    const u32 count = in_vi[9] & 0xffff;        // NLOOP (vi9 entry value = loop count)
    if (count == 0 || count > 256) return -1;
    const u32 XTOP = in_top & 0x3ff;
    const u32* gtag = QW(in_vi[13] & 0x3ff);    // GIFtag template (EE/program-built)
    if ((gtag[0] & 0x7fff) != count) return -1; // fingerprint: GIFtag NLOOP must match count

    const u32 outBytes = 16 + count*3*16;
    if (outBytes > out_cap) return -1;

    static const u32 roff[4] = {719u,716u,717u,718u};
    // [P101-9 lighting] 2-light diffuse + ambient (RE'd byte-exact). Light buffer at vi2=0x3d0:
    //   dir_k = -mem[0x3d5/0x3d7], color_k = mem[0x3d4/0x3d6]*255, ambient = mem[0x3d8]*255,
    //   tint = mem[0x3fb].x. Per vertex: skinned normal (ITOF7(qw3)·skin rot) -> dot each light
    //   (x+z then +y order), max(dot,0), acc += color*dot; +ambient; clamp 255; *tint; FTOI0.
    const u32* Ld[2] = { QW(0x3d5), QW(0x3d7) };
    const u32* Lcm[2] = { QW(0x3d4), QW(0x3d6) };
    float Lc[2][3], Lcdir[2][3], amb[3];
    for (int li=0; li<2; ++li) for (int c=0;c<3;++c) { Lc[li][c]=fmul(u2f(Lcm[li][c]),255.0f); Lcdir[li][c]=-u2f(Ld[li][c]); }
    { const u32* A=QW(0x3d8); for (int c=0;c<3;++c) amb[c]=fmul(u2f(A[c]),255.0f); }
    const float tint = u2f(QW(0x3fb)[0]);
    auto ftoi0 = [](float v)->u32 { const u32 b=f2u(v);
        if ((b & 0x7f800000) >= 0x4f000000) return (b & 0x80000000) ? 0x80000000u : 0x7fffffffu;
        return (u32)(s32)v; };
    const u32 alphaB = ftoi0(fmul(255.0f, tint)) & 0xff;   // A = FTOI0(255*tint) = 0x80

    u8* p = out;
    std::memcpy(p, gtag, 16); p += 16;          // GIFtag verbatim

    for (u32 v = 0; v < count; ++v)
    {
        const u32* rec = QW(XTOP + v*5);        // qw0 local pos
        const u32* rst = QW(XTOP + v*5 + 1);    // qw1 ST
        const u32* rcl = QW(XTOP + v*5 + 2);    // qw2 raw color
        const u32* rwq = QW(XTOP + v*5 + 4);    // qw4 weights+indices
        const float lpx=u2f(rec[0]), lpy=u2f(rec[1]), lpz=u2f(rec[2]);
        const float w[4] = { u2f(rwq[0]), u2f(rwq[1]), u2f(rwq[2]), u2f(rwq[3]) };
        const u32   idx[4] = { rwq[0]&0xffff, rwq[1]&0xffff, rwq[2]&0xffff, rwq[3]&0xffff };

        float srow[4][4];
        for (int r = 0; r < 4; ++r) {
            const u32* b0 = QW(idx[0] + roff[r]); const u32* b1 = QW(idx[1] + roff[r]);
            const u32* b2 = QW(idx[2] + roff[r]); const u32* b3 = QW(idx[3] + roff[r]);
            for (int c = 0; c < 4; ++c) {
                float a = fmul(u2f(b0[c]), w[0]);
                a = fmadd(a, u2f(b1[c]), w[1]);
                a = fmadd(a, u2f(b2[c]), w[2]);
                a = fmadd(a, u2f(b3[c]), w[3]);
                srow[r][c] = a;
            }
        }
        float world[4];
        for (int c = 0; c < 4; ++c) {
            float a = fmul(srow[0][c], 1.0f);
            a = fmadd(a, srow[1][c], lpx);
            a = fmadd(a, srow[2][c], lpy);
            a = fmadd(a, srow[3][c], lpz);
            world[c] = a;
        }
        float clip[4];
        for (int c = 0; c < 4; ++c) {
            float a = fmul(u2f(c26[c]), 1.0f);
            if (c != 2) { a = fmadd(a, u2f(c23[c]), world[0]); a = fmadd(a, u2f(c24[c]), world[1]); }
            a = fmadd(a, u2f(c25[c]), world[2]);
            clip[c] = a;
        }
        const float Q = u2f(divQ(persp, clip[3]));
        const u32 X = ftoi4(fsub(fmul(clip[0], Q), u2f(v22[0])));
        const u32 Y = ftoi4(fsub(fmul(clip[1], Q), u2f(v22[1])));
        const u32 Z = ftoi4(fsub(fmul(clip[2], Q), u2f(v22[2])));
        u32 fw = fmaxU(f2u(clip[3]), v22[3]); fw = fminU(fw, v27[3]);
        const u32 F = ftoi4(fsub(u2f(fw), u2f(v22[3])));
        const u32 S = f2u(fmul(u2f(rst[0]), Q));
        const u32 T = f2u(fmul(u2f(rst[1]), Q));

        // [P101-9 lighting] skinned normal (ITOF7 of qw3 normal, skin rotation rows srow1/2/3)
        const u32* rnm = QW(XTOP + v*5 + 3);
        const float nf0 = (float)(s32)rnm[0] * (1.0f/128.0f);
        const float nf1 = (float)(s32)rnm[1] * (1.0f/128.0f);
        const float nf2 = (float)(s32)rnm[2] * (1.0f/128.0f);
        float nrm[3];
        for (int c=0;c<3;++c) nrm[c] = fmadd(fmadd(fmul(srow[1][c],nf0), srow[2][c],nf1), srow[3][c],nf2);
        float accR=0.0f, accG=0.0f, accB=0.0f;
        for (int li=0; li<2; ++li) {
            float dot = fadd(fadd(fmul(nrm[0],Lcdir[li][0]), fmul(nrm[2],Lcdir[li][2])), fmul(nrm[1],Lcdir[li][1]));
            if (dot < 0.0f) dot = 0.0f;                          // MAX(dot,0)
            accR = fmadd(accR, Lc[li][0], dot);
            accG = fmadd(accG, Lc[li][1], dot);
            accB = fmadd(accB, Lc[li][2], dot);
        }
        float cR = fadd(accR, amb[0]), cG = fadd(accG, amb[1]), cB = fadd(accB, amb[2]);
        if (cR > 255.0f) cR = 255.0f; if (cG > 255.0f) cG = 255.0f; if (cB > 255.0f) cB = 255.0f;  // MINIi 255
        const u32 R = ftoi0(fmul(cR, tint)) & 0xff;
        const u32 G = ftoi0(fmul(cG, tint)) & 0xff;
        const u32 B = ftoi0(fmul(cB, tint)) & 0xff;

        u32* o = (u32*)p;
        o[0]=S; o[1]=T; o[2]=f2u(Q); o[3]=rst[3];                 // ST
        o[4]=R; o[5]=G; o[6]=B; o[7]=alphaB;                     // RGBAQ (RE'd lighting)
        o[8]=X; o[9]=Y; o[10]=Z; o[11]=F;                        // XYZF2 (byte-exact)
        (void)rcl;
        p += 48;
    }
    return (int)outBytes;
}

// ── GTA VC (hash 92d542c43042231c) T&L #2 pc=1435 native kernel ─────────────────────────────
// [P101-9 top-3] 4-BONE SKINNED (identical skinning core to pc1454: roff {719,716,717,718}, weight
// blend, world = srow0 + srow1*px + srow2*py + srow3*pz). DIFFERENCES from pc1454:
//   - MVP rows = entry VF vf28,vf29,vf30,vf31 (NOT vf23-26) + vf2; vf22.z/.w from mem qw1014.
//   - Perspective is FOLDED into the MVP .w lanes (M0/M1/M2.w *= vf22.z; M3.w = vf31.w*zc - vf22.w*zc).
//     => NO per-vertex divide, NO Q multiply on position; clip.xyz ARE screen coords; FTOI4 directly.
//   - FULL 4-term clip on ALL lanes (no z-lane special-case).
//   - COLOR = FTOI0( ITOF0(record_color int bytes) * vf4[lane] ), vf4 = mem qw1019 (~0.50235). NO
//     lighting / dot products => fully byte-exact (no fused-float ceiling).
//   - ST: S,T = record+1.xy verbatim; ST.z(Q) = vf0.w (=1.0); ST.w = per-vertex winding flag
//     (even index = vf7.w, odd = vf8.w). Output vertex order == input record order.
// RE'd byte-exact: 3 independent attempts + adversarial synthesis, XYZF2/ST/RGBAQ 42/42, full 2032B
// mismatch=0; intermediates cross-checked (built MVP==entry vf24-27, world==vf13, clip==vf5).
// See re/gta1435/pc1435_model.py + notes. validated=false until in-emulator self-check + user visual.
extern "C" int iv2_hle_kernel_gta_pc1435(const u8* in_mem, const u32* in_vi, u32 in_top,
                                         const u32* /*in_matrix*/, u8* out, u32 out_cap)
{
    using namespace vufp;
    if (!in_mem || !in_vi) return -1;
    auto QW = [&](u32 q) -> const u32* { return (const u32*)(in_mem + (q & 0x3ff) * 16); };

    const u32 count = in_vi[9] & 0xffff;
    if (count == 0 || count > 256) return -1;
    const u32 XTOP = in_top & 0x3ff;
    const u32* giftpl = QW(1018);                  // GIFtag template (mem qw1018)
    if (giftpl[2] != 0x412u) return -1;            // fingerprint: REGS = [ST,RGBAQ,XYZF2]
    const u32 outBytes = 16 + count*3*16;
    if (outBytes > out_cap) return -1;

    const u32* vf0  = &g_hle_snap_vf[0*4];   const u32* vf2  = &g_hle_snap_vf[2*4];
    const u32* vf28 = &g_hle_snap_vf[28*4];  const u32* vf29 = &g_hle_snap_vf[29*4];
    const u32* vf30 = &g_hle_snap_vf[30*4];  const u32* vf31 = &g_hle_snap_vf[31*4];
    const u32 STQ      = vf0[3];                    // ST.z (Q) = 1.0
    const u32 STW_EVEN = g_hle_snap_vf[7*4+3];
    const u32 STW_ODD  = g_hle_snap_vf[8*4+3];
    const u32* m1014 = QW(1014);
    const float ZC = u2f(m1014[2]);                // vf22.z perspective W-fold scale
    const float WC = u2f(m1014[3]);                // vf22.w
    const u32* VF4 = QW(1019);                     // per-lane color scale

    static const u32 roff[4] = {719u,716u,717u,718u};
    auto ftoi0 = [](float val)->u32 { const u32 b=f2u(val);
        if ((b & 0x7f800000) >= 0x4f000000) return (b & 0x80000000) ? 0x80000000u : 0x7fffffffu;
        return (u32)(s32)val; };

    // MVP build (pc506..514, fused): M0=vf28+0, M1=vf29+0, M2=vf30+0, M3.xyz=vf31+vf2; perspective .w fold
    const float z0 = u2f(vf0[0]);                  // vf0.x = 0 (ADDx normalises -0.0 -> +0.0)
    float M0[4],M1[4],M2[4],M3[4];
    for (int l=0;l<4;++l){ M0[l]=fadd(u2f(vf28[l]),z0); M1[l]=fadd(u2f(vf29[l]),z0); M2[l]=fadd(u2f(vf30[l]),z0); }
    for (int l=0;l<3;++l) M3[l]=fadd(u2f(vf31[l]),u2f(vf2[l]));
    M3[3] = fsub(fmul(u2f(vf31[3]), ZC), fmul(WC, ZC));   // MULAz.w then MSUBz.w
    M0[3]=fmul(M0[3],ZC); M1[3]=fmul(M1[3],ZC); M2[3]=fmul(M2[3],ZC);

    u8* p = out;
    { u32* g=(u32*)p; g[0]=(count|0x8000u); g[1]=giftpl[1]; g[2]=giftpl[2]; g[3]=giftpl[3]; }
    p += 16;

    for (u32 i=0;i<count;++i)
    {
        const u32 base = XTOP + i*5;
        const u32* pos = QW(base+0);
        const u32* rst = QW(base+1);
        const u32* rcl = QW(base+2);              // record color (integer bytes per lane)
        const u32* rwq = QW(base+4);              // weights (float) + bone index (low16)
        const float wf[4]={u2f(rwq[0]),u2f(rwq[1]),u2f(rwq[2]),u2f(rwq[3])};
        const u32 idx[4]={rwq[0]&0xffff,rwq[1]&0xffff,rwq[2]&0xffff,rwq[3]&0xffff};

        float srow[4][4];
        for (int r=0;r<4;++r){
            const u32* b0=QW(idx[0]+roff[r]); const u32* b1=QW(idx[1]+roff[r]);
            const u32* b2=QW(idx[2]+roff[r]); const u32* b3=QW(idx[3]+roff[r]);
            for (int c=0;c<4;++c){
                float a=fmul(u2f(b0[c]),wf[0]);
                a=fmadd(a,u2f(b1[c]),wf[1]);
                a=fmadd(a,u2f(b2[c]),wf[2]);
                a=fmadd(a,u2f(b3[c]),wf[3]);
                srow[r][c]=a;
            }
        }
        const float lpx=u2f(pos[0]),lpy=u2f(pos[1]),lpz=u2f(pos[2]);
        float world[4];
        for (int c=0;c<4;++c){
            float a=fmul(srow[0][c],1.0f);
            a=fmadd(a,srow[1][c],lpx);
            a=fmadd(a,srow[2][c],lpy);
            a=fmadd(a,srow[3][c],lpz);
            world[c]=a;
        }
        float clip[4];
        for (int l=0;l<4;++l){
            float a=fmul(M3[l],1.0f);             // MULAw ACC = M3 (* vf0.w = 1)
            a=fmadd(a,M0[l],world[0]);            // += M0 * world.x
            a=fmadd(a,M1[l],world[1]);            // += M1 * world.y
            a=fmadd(a,M2[l],world[2]);            // += M2 * world.z
            clip[l]=a;
        }
        const u32 X=ftoi4(clip[0]); const u32 Y=ftoi4(clip[1]); const u32 Z=ftoi4(clip[2]);
        float fwv=clip[3]; if (fwv>255.0f) fwv=255.0f; if (fwv<0.0f) fwv=0.0f;  // MINIi 255 then MAXx 0
        const u32 F=ftoi4(fwv);
        const u32 R=ftoi0(fmul((float)(s32)rcl[0], u2f(VF4[0])));
        const u32 G=ftoi0(fmul((float)(s32)rcl[1], u2f(VF4[1])));
        const u32 B=ftoi0(fmul((float)(s32)rcl[2], u2f(VF4[2])));
        const u32 A=ftoi0(fmul((float)(s32)rcl[3], u2f(VF4[3])));

        u32* o=(u32*)p;
        o[0]=rst[0]; o[1]=rst[1]; o[2]=STQ; o[3]=((i&1)?STW_ODD:STW_EVEN);   // ST
        o[4]=R; o[5]=G; o[6]=B; o[7]=A;                                       // RGBAQ (scale+convert)
        o[8]=X; o[9]=Y; o[10]=Z; o[11]=F;                                     // XYZF2 (byte-exact)
        p += 48;
    }
    return (int)outBytes;
}

// ── GTA VC static-geometry T&L (603adeb2 / f8ea3ba5) — shared MVP builder ────────────────────────
// [P101-9] These NON-skinned programs build their loop-body MVP (vf22..vf27) in a setup (PC0-474) from
// VU-mem constants (NOT entry VF). Replicated byte-exact (verified by the setup-closure workflow): rows
// from qw1008-1011, persp/scale/screen from qw1014-1017, I-immediates 0.5 then 255.0, fused .w folds.
struct GtaStaticMVP { float v22[4], v23[4], v24[4], v25[4], v26[4], v27z, v27w; };
static void gta_static_build_mvp(const u8* in_mem, GtaStaticMVP& M)
{
    using namespace vufp;
    auto QW = [&](u32 q) -> const u32* { return (const u32*)(in_mem + (q & 0x3ff) * 16); };
    const u32* q08=QW(1008); const u32* q09=QW(1009); const u32* q10=QW(1010); const u32* q11=QW(1011);
    const u32* q14=QW(1014); const u32* q15=QW(1015); const u32* q16=QW(1016); const u32* q17=QW(1017);
    // rows vf28-31 = qw1008-1011 (.xyw), .z forced (28/29/30=0, 31=1)
    float r28[4]={u2f(q08[0]),u2f(q08[1]),0.0f,u2f(q08[3])};
    float r29[4]={u2f(q09[0]),u2f(q09[1]),0.0f,u2f(q09[3])};
    float r30[4]={u2f(q10[0]),u2f(q10[1]),0.0f,u2f(q10[3])};
    float r31[4]={u2f(q11[0]),u2f(q11[1]),1.0f,u2f(q11[3])};
    const float I05=0.5f;
    const float w28=fmul(r28[3],I05), w29=fmul(r29[3],I05), w30=fmul(r30[3],I05), w31=fmul(r31[3],I05);
    float r2[3]={u2f(q16[0]),u2f(q16[1]),u2f(q16[2])};   // screen translate
    const float r5x=u2f(q15[0]), r5y=u2f(q15[1]), r5z=u2f(q15[2]);   // scale
    r28[0]=fsub(r28[0],w28); r28[1]=fsub(r28[1],w28);
    r29[0]=fsub(r29[0],w29); r29[1]=fsub(r29[1],w29);
    r30[0]=fsub(r30[0],w30); r30[1]=fsub(r30[1],w30);
    r31[0]=fsub(r31[0],w31); r31[1]=fsub(r31[1],w31);
    const float r3x=u2f(q17[0]), r3y=u2f(q17[1]);        // screen offset
    r28[0]=fmul(r28[0],r5x); r28[1]=fmul(r28[1],r5y);
    r29[0]=fmul(r29[0],r5x); r29[1]=fmul(r29[1],r5y);
    r30[0]=fmul(r30[0],r5x); r30[1]=fmul(r30[1],r5y);
    r31[0]=fmul(r31[0],r5x); r31[1]=fmul(r31[1],r5y); r31[2]=fmul(r31[2],r5z);
    r2[0]=fadd(r2[0],r3x); r2[1]=fadd(r2[1],r3y);
    // fold (I=255.0): persp .w + screen offset + F clamps
    M.v27z=u2f(q14[2]); const float q14w=u2f(q14[3]);
    for (int k=0;k<3;++k) M.v26[k]=r31[k];               // vf26.xyz = vf31.xyz
    M.v23[0]=r28[0]; M.v23[1]=r28[1]; M.v23[2]=0.0f;     // vf23.z dead
    M.v24[0]=r29[0]; M.v24[1]=r29[1]; M.v24[2]=0.0f;     // vf24.z dead
    M.v25[0]=r30[0]; M.v25[1]=r30[1]; M.v25[2]=0.0f;     // vf25.z = 0
    M.v26[3]=fmul(r31[3],M.v27z); M.v23[3]=fmul(r28[3],M.v27z);
    M.v24[3]=fmul(r29[3],M.v27z); M.v25[3]=fmul(r30[3],M.v27z);
    M.v22[0]=fsub(0.0f,r2[0]); M.v22[1]=fsub(0.0f,r2[1]); M.v22[2]=fsub(0.0f,r2[2]);
    M.v22[3]=fmul(q14w,M.v27z);                          // F-bias = qw1014.w * vf27.z
    M.v27w=fmadd(fmul(1.0f,255.0f), q14w, M.v27z);       // F-clamp hi = 255 + qw1014.w*vf27.z (fused)
}
// Shared per-vertex transform for the static programs: record (pos qw, ST qw) -> [ST][XYZF2].
static inline void gta_static_xform(const GtaStaticMVP& M, const u32* pos, const u32* rst,
                                    u32* o_st, u32* o_xyzf2)
{
    using namespace vufp;
    const float px=u2f(pos[0]), py=u2f(pos[1]), pz=u2f(pos[2]);
    float clip[4];
    for (int l=0;l<4;++l){
        float a=fmul(M.v26[l],1.0f);
        if (l!=2){ a=fmadd(a,M.v23[l],px); a=fmadd(a,M.v24[l],py); }   // z-lane skips x/y
        a=fmadd(a,M.v25[l],pz);
        clip[l]=a;
    }
    const float Q=u2f(divQ(M.v27z, clip[3]));
    const u32 X=ftoi4(fsub(fmul(clip[0],Q), M.v22[0]));
    const u32 Y=ftoi4(fsub(fmul(clip[1],Q), M.v22[1]));
    const u32 Z=ftoi4(fsub(fmul(clip[2],Q), M.v22[2]));
    float fw=clip[3]; if (fw < M.v22[3]) fw=M.v22[3]; if (fw > M.v27w) fw=M.v27w;  // MAX then MIN
    const u32 F=ftoi4(fsub(fw, M.v22[3]));
    o_st[0]=f2u(fmul(u2f(rst[0]),Q)); o_st[1]=f2u(fmul(u2f(rst[1]),Q));
    o_st[2]=f2u(fmul(1.0f,Q)); o_st[3]=0x43001999u;
    o_xyzf2[0]=X; o_xyzf2[1]=Y; o_xyzf2[2]=Z; o_xyzf2[3]=F;
}

// ── GTA VC f8ea3ba5 (static T&L, 60 verts) native kernel ─────────────────────────────────────────
// [P101-9] geometry+ST byte-exact (RE'd 60/60); color = FTOI0(min(ITOF0(record int)+amb,255)*scale),
// scale=qw1019, amb is a per-draw lighting term (captured ~55.0; see notes — variable like pc1454).
// count = VIF1 ITOP (g_hle_snap_itop); record stride 4qw @ qw0 (pos/ST/color). Input fingerprint = REGS
// field of the mem GIFtag template (qw1018) must be 0x412. validated=false until self-check + visual.
extern "C" int iv2_hle_kernel_gta_f8ea(const u8* in_mem, const u32* /*in_vi*/, u32 in_top,
                                       const u32* /*in_matrix*/, u8* out, u32 out_cap)
{
    using namespace vufp;
    if (!in_mem) return -1;
    auto QW = [&](u32 q) -> const u32* { return (const u32*)(in_mem + (q & 0x3ff) * 16); };
    const u32 count = g_hle_snap_itop & 0x7fff;
    if (count == 0 || count > 256) return -1;
    const u32 XTOP = in_top & 0x3ff;                     // record base = VIF double-buffer TOP (alternates per kick)
    const u32* gtpl = QW(1018);                          // GIFtag template (fixed config region)
    if (gtpl[2] != 0x412u) return -1;                    // fingerprint: REGS=[ST,RGBAQ,XYZF2]
    const u32 outBytes = 16 + count*3*16;
    if (outBytes > out_cap) return -1;
    GtaStaticMVP M; gta_static_build_mvp(in_mem, M);
    const u32* sc = QW(1019);                            // per-lane color scale
    const float amb = 54.99999619f;                      // [per-draw lighting term; captured 0x425bfffd]
    auto ftoi0 = [](float val)->u32 { const u32 b=f2u(val);
        if ((b & 0x7f800000) >= 0x4f000000) return (b & 0x80000000) ? 0x80000000u : 0x7fffffffu;
        return (u32)(s32)val; };
    u8* p = out;
    { u32* g=(u32*)p; g[0]=(count|0x8000u); g[1]=gtpl[1]; g[2]=gtpl[2]; g[3]=gtpl[3]; } p += 16;
    for (u32 i=0;i<count;++i){
        const u32* pos=QW(XTOP+i*4+0); const u32* rst=QW(XTOP+i*4+1); const u32* rcl=QW(XTOP+i*4+2);
        u32* o=(u32*)p;
        gta_static_xform(M, pos, rst, o, o+8);           // ST -> o[0..3], XYZF2 -> o[8..11]
        for (int ch=0;ch<3;++ch){ float v=fadd((float)(s32)rcl[ch], amb); if (v>255.0f) v=255.0f;
            o[4+ch]=ftoi0(fmul(v, u2f(sc[ch]))); }
        o[7]=0x80u;                                      // alpha = FTOI0(0x43001999)
        p += 48;
    }
    return (int)outBytes;
}

// ── GTA VC 603adeb2 (static T&L, 47 verts) native kernel ─────────────────────────────────────────
// [P101-9] geometry+ST byte-exact (RE'd 47/47). COLOR = dynamic indexed lighting (deep remainder, computed
// from normals qw+3 — NOT yet closed-form); placeholder gray here so geometry self-checks. count=VIF1 ITOP;
// GIFtag template @ VI[12]... but here read from qw0x312? -> use the mem template the program patches.
extern "C" int iv2_hle_kernel_gta_603a(const u8* in_mem, const u32* in_vi, u32 in_top,
                                       const u32* /*in_matrix*/, u8* out, u32 out_cap)
{
    using namespace vufp;
    if (!in_mem || !in_vi) return -1;
    auto QW = [&](u32 q) -> const u32* { return (const u32*)(in_mem + (q & 0x3ff) * 16); };
    const u32 count = g_hle_snap_itop & 0x7fff;
    if (count == 0 || count > 256) return -1;
    const u32 XTOP = in_top & 0x3ff;                     // record base = VIF double-buffer TOP
    const u32 tplqw = in_vi[12] & 0x3ff;                 // GIFtag template ptr = VI[12]
    const u32* gtpl = QW(tplqw);
    if (gtpl[2] != 0x412u) return -1;                    // fingerprint
    const u32 outBytes = 16 + count*3*16;
    if (outBytes > out_cap) return -1;
    GtaStaticMVP M; gta_static_build_mvp(in_mem, M);
    u8* p = out;
    { u32* g=(u32*)p; g[0]=(count|0x8000u); g[1]=gtpl[1]; g[2]=gtpl[2]; g[3]=gtpl[3]; } p += 16;
    for (u32 i=0;i<count;++i){
        const u32* pos=QW(XTOP+i*4+0); const u32* rst=QW(XTOP+i*4+1);
        u32* o=(u32*)p;
        gta_static_xform(M, pos, rst, o, o+8);
        o[4]=0x80u; o[5]=0x80u; o[6]=0x80u; o[7]=0x80u;  // placeholder gray (color = deep lighting remainder)
        p += 48;
    }
    return (int)outBytes;
}

// ── GTA VC kernel registry table (co-located; rows verbatim from the original kHleKernels[]) ──
const IV2_HleKernel kIv2HleKernels_gtavc[] = {
    // [P101-9 top-3] pc1435 (26%): own kernel (4-bone skinned, MVP vf28-31, perspective folded, color=scale,
    // NO lighting). RE'd byte-exact (3 indep attempts + synth) AND in-emulator self-check = geomXYZ/ST/RGBAQ
    // 100% over 276,948 verts (FULLY byte-exact incl color) -> validated=true (mode-2). default ship dormant.
    { 0x92d542c43042231cULL, 1435, iv2_hle_kernel_gta_pc1435, true  }, // GTA VC T&L #2 (26%): FULLY byte-exact
    // [P101-9 static-T&L] f8ea3ba5 (12.8%) + 603adeb2 (17.5%): non-skinned static-geometry T&L (resident stream,
    // FF10-class). entryPC=0 = hash-only match (these share entry addresses with other programs). geometry+ST
    // byte-exact (RE'd 60/60, 47/47). f8ea color closed-form; 603a color = deep lighting remainder (placeholder).
    // validated=false until in-emulator self-check + user visual.
    { 0xf8ea3ba5a625c699ULL, 0, iv2_hle_kernel_gta_f8ea, false }, // GTA VC world-geom (12.8%): NOT mode-2-feasible = FRUSTUM CLIPPER. Deep-RE 2026-06-25 "B" attempt (QUADRUPLE-confirmed): the EXECUTED route (flags qw1023=0xb -> pc608) is a guard-band clipper: CLIP/FCGET per vertex (pc604/624/631/638), IBNE vi0,vi2 (cull prim) / IBNE vi0,vi3 (->clip handler, Sutherland-Hodgman interp verts). Output NLOOP = frustum-clip survivors (10/19/29 of a 68-vert buffer) = runtime-computed. Count NOT recoverable: (1) not in entry snapshot, (2) != itop/itops, (3) not in any prior-program exit (loader-hook refuted via exit-ring probe). Same class as pc1365 / 603a path-E. Visual: f8ea+603a render buildings/roads. validated=false. See F8EA_STRIP_SPEC.md + report
    { 0x603adeb2bbda23b6ULL, 0, iv2_hle_kernel_gta_603a, false }, // GTA VC strip-T&L (17.5%): NOT mode-2-feasible. Deep-RE 2026-06-25: flags(qw1023)-dispatched 2 paths. Path A (plain T&L, count==itop) recoverable but MINORITY; Path E (dominant, flags=0x0b) = in-VU Sutherland-Hodgman FRUSTUM CLIPPER (runtime-generated interp verts, 1-3 packets, count!=itop) = INELIGIBLE, identical to pc1365. (Also note: self-check over-declines because fingerprint should read QW(1018)[2]==0x412 not QW(vi12)[2] — cosmetic, kernel is dormant.) validated=false. See PC603A_STRIP_SPEC.md
    // [P101-9 top-3] pc1365 (33%): DECLINED (architectural). It is NOT a pure T&L program -- it is a guard-band
    // FRUSTUM-CLIPPING strip emitter (in-VU Sutherland-Hodgman). Of 81 output verts: 27 closed-form DIRECT,
    // 36 clipper-INTERPOLATED (new verts), 18 prior-frame STALE residue (CULL short-path stores nothing) =
    // NOT reproducible from a single entry snapshot. = INELIGIBLE for kernel-HLE (fails the §5.7 eligibility
    // criterion: output depends on a data-dependent clip state machine + cross-frame bank state). Left UNREGISTERED
    // = runs on the interpreter (zero regression). RE record: re/gta1365/GTA1365_STATUS.md + pc1365_model.py.
    { 0x8ac5daebfa578107ULL, 1454, iv2_hle_kernel_gta_pc1454, true  }, // GTA VC dominant T&L (37%): 4-bone skinned geometry byte-exact in-emulator (X/Y/Z/F/ST/header 100% across 3.2M verts) + multi-pass diffuse+ambient lighting RE'd. P101-9 §5.7 read/write-set proved ELIGIBLE (42 pos = 5 resident + 37 computed, 0 delivered); P101-7 "impossible" was an addressing misread of the COMPUTED positions. ★ user visual PASS 2026-06-24 (color+shape no discomfort) → validated=true (mode-2). Color byte-exact ~20% (GTA per-draw-variable lighting; visible scene visual-OK, broader-scene color coverage = future). See GTA1454_STATUS.md
};
const int kIv2HleKernels_gtavc_count = (int)(sizeof(kIv2HleKernels_gtavc) / sizeof(kIv2HleKernels_gtavc[0]));
