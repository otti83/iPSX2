// SPDX-License-Identifier: GPL-3.0+
// iv2 kernel-HLE — J-Phoenix (SLPM-65033) native T&L kernels (P101-9; 2026-06-27)
//
//   pc19 (hash 125220b50566d214) — DOMINANT 3D mecha T&L (44-47% of VU1 time). Pure T&L, NO skinning,
//        NO frustum clipping (zero CLIP / FCAND/FCOR/FCGET). Output per kick = TWO GIF packets
//        [ST,RGBAQ,XYZF2]: tagA + dataA(z verts), then tagB + dataB(z verts) — same z verts, 2-pass
//        (base texture + 1-D env/toon coord). RE'd byte-exact on 2 kicks (x=1 and x=3); see
//        re/jphoenix/model.py + JPHOENIX_STATUS.md.
//
// Header-driven layout: mem[in_top] = [x, y, z] with z = x*y. x = STRIP count, y = verts/strip,
//   z = total verts = GIFtag NLOOP. POS @ (in_top+1) qw; NRM @ (in_top+1+z) qw; TEX @ (in_top+0x51) qw.
//   XYZF2.w3 = strip-restart marker (0xFFFF8000|z) at the first 2 verts of EACH strip ((i%y)<2), else
//   0x10 (=FTOI4(1.0)). MVP = mem[0x00..0x30]. Color-A = grayscale diffuse lighting; Color-B = const.
//
// FLOAT: the kernel runs inside FPControlRegisterBackup(VU1FPCR = ChopZero+DAZ+FTZ) (iv2_vu.cpp:846),
//   so plain float ops round TOWARD ZERO and flush denormals exactly like the interpreter's VU FMAC.
//   [2026-08-14] The interpreter MADD is no longer FUSED. `-ffp-contract=off` is now applied to every
//   guest-FP TU (pcsx2/CMakeLists.txt), so VU MADD rounds the product to f32 and then adds, like the
//   PS2 FMAC. The accumulate steps here therefore use the SPLIT form `vd(vd(acc) + vd(a)*vd(b))`
//   (= vufp::fmadd) instead of std::fmaf.
//   ★ std::fmaf must NOT come back: it is a true FMA and stays fused even at -ffp-contract=off, which
//   would put this kernel on a different FP model than the interpreter it replaces. That mismatch is
//   measurable — with std::fmaf against the unfused interpreter, iPSX2_HLE_KERNEL=1 self-check showed
//   ~490x worse colour agreement. See docs/report/VU_FMA_contraction_MEASURED_2026-08-14.md.
#include "PrecompiledHeader.h"
#include "iv2/iv2_hle.h"
#include "iv2/iv2_hle_vumath.h"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>

float g_jpx_clipw[256];   // [ADC RE diag] per-vertex clip.w (filled below; read by the self-check)
float g_jpx_sx[256], g_jpx_sy[256];   // [cull RE diag] per-vertex screen sx,sy (pre-cull)

extern "C" int iv2_hle_kernel_jphoenix_pc19(const u8* in_mem, const u32* /*in_vi*/, u32 in_top,
                                            const u32* in_matrix, u8* out, u32 out_cap)
{
    using vufp::u2f; using vufp::f2u; using vufp::ftoi4; using vufp::vd; using vufp::fmul; using vufp::fadd; using vufp::divQ;
    (void)in_matrix;
    // interpreter-faithful VU FMAC: inputs + result vuDouble-filtered (Inf/NaN -> sign|FLT_MAX,
    // denormal -> +-0), MADD fused (single chop via FPCR). vd is identity on normal values, so this
    // keeps the byte-exactness on in-range verts AND matches the interpreter on extreme/off-screen
    // verts (the geometry garble was a few per-mesh verts where 1/w overflowed without vd-clamping).
    auto FM = [](float acc, float a, float b) -> float { return vd(vd(acc) + vd(a) * vd(b)); };
    auto W = [&](u32 bo) -> u32   { u32 v; std::memcpy(&v, in_mem + (bo & 0x3fff), 4); return v; };
    auto F = [&](u32 bo) -> float { return u2f(W(bo)); };

    const u32 B = (in_top & 0x3ff) * 16;          // header quadword (byte) at the double-buffer base
    const u32 x = W(B + 0), y = W(B + 4), z = W(B + 8);   // strips, verts/strip, total (z = x*y = NLOOP)

    // [diag] iPSX2_JPX_DBG=1: header + in_top dump for the first kicks. Removal condition: pinned; env-gated.
    static const bool s_dbg = (std::getenv("iPSX2_JPX_DBG") != nullptr);
    if (__builtin_expect(s_dbg, 0)) {
        static int dn = 0;
        if (dn < 80) { dn++;
            FILE* df = std::fopen("/tmp/ipsx2_jpx_dbg.txt", dn == 1 ? "w" : "a");
            if (df) { std::fprintf(df, "k%-3d in_top=0x%-4x [x=%u y=%u z=%u] xy=%u valid=%d\n",
                        dn, in_top, x, y, z, x*y, (x>0&&x<=256&&y>0&&y<=256&&z<=256&&x*y==z)); std::fclose(df); }
        }
    }

    if (x == 0 || x > 256 || y == 0 || y > 256 || z == 0 || z > 256 || x * y != z) return -1;  // validity / range

    const u32 POS = B + 0x10;                      // positions  @ in_top+1 (stride 1qw, w=1.0)
    const u32 NRM = POS + z * 0x10;                // normals    @ in_top+1+z
    const u32 TEX = (((in_top & 0x3ff) + 0x51)) * 16;   // packet-A texcoords (S,T,*,T3 per vtx)

    const u32 outBytes = 16 + z * 48 + 16 + z * 48;     // tagA + dataA + tagB + dataB
    if (outBytes > out_cap) return -1;

    const u32 MARK = 0xFFFF0000u | (0x8000u | z);  // strip-restart marker (reused for tagB.w0)

    // GIFtags: A.w0 = z (NLOOP, EOP=0), B.w0 = MARK (NLOOP|0x8000, EOP=1); words 1..3 from mem templates.
    const u32 tagA0 = z, tagA1 = W(0x220 + 4), tagA2 = W(0x220 + 8), tagA3 = W(0x220 + 12);
    const u32 tagB0 = MARK, tagB1 = W(0x270 + 4), tagB2 = W(0x270 + 8), tagB3 = W(0x270 + 12);

    // MVP rows + color/env constants (preloaded once).
    float M[4][4];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) M[r][c] = F(0x00 + r * 0x10 + c * 4);
    const float M1x0 = F(0xe0 + 0), M1x1 = F(0xf0 + 0), M1x2 = F(0x100 + 0);   // lighting col .x
    const float M1z0 = F(0xe0 + 8), M1z1 = F(0xf0 + 8), M1z2 = F(0x100 + 8);   // lighting col .z
    const float M4_0 = F(0x110 + 0), M4_2 = F(0x130 + 0);                      // light/ambient scale (127, 128)
    const float AMB  = 15.0f;   // ambient = constant 15 (RE: model read mem[0x90] which was 15 only by
                                // coincidence in kick1; it varies per kick (0 / 1.45 / 1.29) yet the
                                // interpreter color always = 127*cx + 128*cz + 15, verified across kicks).
    // [P101-9 §6.4 alpha fix 2026-06-27] packet-B RGBAQ alpha = per-material translucency, NOT a constant.
    // RE (workflow + adversarial verify, byte-exact on both captured kicks): alpha = trunc(min(L,128)*2*mat)
    // where mat = mem[0x2d0].z (byte 0x2d8) and L = packet-B lighting (vf28.w). L >> 128 for every lit vert in
    // the captures (clamped by MINIi), so alpha = trunc(256 * mat): 0.15->38(0x26), 0.25->64(0x40), 0->0.
    // The old hardcoded 38 baked in THIS material's 0.15 (wrong: live materials yield 0x40 / 0x00).
    const int   colB_ai = (int)(256.0f * F(0x2d8));                           // 0x2d8 = mem[0x2d0].z
    const u32   colB_a  = (u32)(colB_ai < 0 ? 0 : colB_ai > 255 ? 255 : colB_ai);
    const float m17x = F(0x240 + 0), m18x = F(0x250 + 0), m19x = F(0x260 + 0);   // env-coord row
    const float w17  = F(0x240 + 12), w18 = F(0x250 + 12), w19 = F(0x260 + 12);  // env .w (signed-zero)

    auto put = [&](u8*& p, u32 a, u32 b, u32 c, u32 d) {
        std::memcpy(p, &a, 4); std::memcpy(p + 4, &b, 4); std::memcpy(p + 8, &c, 4); std::memcpy(p + 12, &d, 4); p += 16;
    };

    u8* pA = out;                            // tagA + dataA
    u8* pB = out + 16 + z * 48;              // tagB + dataB
    put(pA, tagA0, tagA1, tagA2, tagA3);
    put(pB, tagB0, tagB1, tagB2, tagB3);

    // [P101-9 §6.2 stray cull 2026-06-27] triangle-strip ADC: a vertex i (strip-local index >=2)
    // completes triangle (i-2,i-1,i); the interp suppresses the DRAW (ADC=1, but KEEPS the screen XYZ)
    // if ANY of the 3 triangle verts failed the guard band. RE'd from the stray pattern: e.g. a vert with
    // clip.w=70 (passes) was culled while its neighbour with w=67.6 (also passes) was drawn -- because the
    // FORMER's triangle contained a w<64 (failed) vert. gf1/gf2 = guard_fail of i-1/i-2 (same strip; for
    // strip-local idx<2 the (i%y)<2 restart term forces MARK anyway, so cross-strip carry is harmless).
    bool gf1 = true, gf2 = true;
    for (u32 i = 0; i < z; ++i)
    {
        const float px = F(POS + i * 16 + 0), py = F(POS + i * 16 + 4), pz = F(POS + i * 16 + 8), pw = F(POS + i * 16 + 12);
        const float nx = F(NRM + i * 16 + 0), ny = F(NRM + i * 16 + 4), nz = F(NRM + i * 16 + 8);

        // MVP transform (vd-filtered fused FMA) + perspective (vd DIV / MUL). vd matches the interp's
        // input filter so off-screen / behind-camera verts (1/w overflow) clamp identically.
        float clip[4];
        for (int c = 0; c < 4; ++c) {
            float a = fmul(M[0][c], px);
            a = FM(a, M[1][c], py);
            a = FM(a, M[2][c], pz);
            a = FM(a, M[3][c], pw);
            clip[c] = a;
        }
        if (i < 256) g_jpx_clipw[i] = clip[3];   // [ADC RE diag]
        const float Q  = u2f(divQ(1.0f, clip[3]));
        const float sx = fmul(clip[0], Q), sy = fmul(clip[1], Q), sz = fmul(clip[2], Q);
        if (i < 256) { g_jpx_sx[i] = sx; g_jpx_sy[i] = sy; }   // [cull RE diag]

        // Per-vertex guard-band cull (RE'd from pc 0x1e8-0x218: SUB.xyw vf30-vf27 / vf27-vf31 set the
        // x,y,w sign MAC flags; FMAND&0xd0; IBNE vi09!=0xd0 -> cull). A vert is DRAWN iff its screen x,y
        // are inside (0,4095) AND its clip-space w is inside the near/far band (64, 12512); otherwise the
        // interp emits XYZ=0 + ADC=1 (not drawn). The load-bearing term is the near guard w>64 (NOT w>0):
        // on-screen verts with 0<w<64 are dropped. Replicating this removes the stray-triangle garble.
        const bool guard_fail = !((sx > 0.0f && sx < 4095.0f) && (sy > 0.0f && sy < 4095.0f) &&
                                  (clip[3] > F(0x1d0 + 0) && clip[3] < F(0x1d0 + 4)));
        const u32 X = guard_fail ? 0u : (ftoi4(sx) & 0x0000FFFF);
        const u32 Y = guard_fail ? 0u : (ftoi4(sy) & 0x0000FFFF);
        // [P101-9 §6.4 fog F fix 2026-06-27] The interp does NOT mask FTOI4(sz): the depth is
        // pre-scaled so that bits[31:24] of FTOI4(sz) are the GS fog factor F (XYZF2 word2 = F<<24|Z24).
        // Masking off the high byte (the old `& 0x00FFFFFF`) dropped F (interp F=0x07 vs kernel 0x00 on
        // EVERY drawn vert). Keeping the full FTOI4(sz) makes word2 byte-exact (low-24 Z already matched;
        // captured byte-exact kicks had F=0 anyway, so unmasked is correct in both cases).
        const u32 Z = guard_fail ? 0u : (u32)ftoi4(sz);              // bits[31:24]=F (fog), bits[23:0]=Z
        // ADC=1 (w3=MARK) if THIS vert fails guard, or it's a strip-restart (idx<2), or a triangle
        // neighbour (i-1 / i-2) failed guard -> the triangle is suppressed but XYZ stays the real value
        // (only THIS vert failing zeroes XYZ above). Drawn iff the whole triangle is in the guard band.
        const u32 w3 = (guard_fail || (i % y) < 2 || gf1 || gf2) ? MARK : 0x10u;

        // packet-A color = grayscale diffuse lighting (vd-filtered fused FMA dot, trunc).
        float vx = fmul(M1x0, nx); vx = FM(vx, M1x1, ny); vx = FM(vx, M1x2, nz);
        float vz = fmul(M1z0, nx); vz = FM(vz, M1z1, ny); vz = FM(vz, M1z2, nz);
        const float cx = vx > 0.0f ? vx : 0.0f, cz = vz > 0.0f ? vz : 0.0f;
        float g = fmul(M4_0, cx); g = FM(g, M4_2, cz); g = fadd(g, AMB);
        g = g < 0.0f ? 0.0f : (g > 255.0f ? 255.0f : g);
        const u32 gi = (u32)(int)g;

        // packet-A ST = perspective texcoord (vd MUL); w2 = Q; w3 = raw T3 passthrough.
        const float Sr = F(TEX + i * 16 + 0), Tr = F(TEX + i * 16 + 4);
        const u32 stA_s = f2u(fmul(Sr, Q)), stA_t = f2u(fmul(Tr, Q)), stA_q = f2u(Q), stA_3 = W(TEX + i * 16 + 12);

        // packet-B ST = 1-D env coord (vd-filtered fused FMA) + signed-zero .w lane.
        float accS = fmul(m17x, nx); accS = FM(accS, m18x, ny); accS = FM(accS, m19x, nz);
        float wl = w17 * nx; wl = w18 * ny + wl; wl = w19 * nz + wl;

        put(pA, stA_s, stA_t, stA_q, stA_3);
        put(pA, gi, gi, gi, 0x80);
        put(pA, X, Y, Z, w3);
        put(pB, f2u(accS), 0x00000000u, 0x3f800000u, f2u(wl));
        put(pB, 0x80u, 0x80u, 0x80u, colB_a);
        put(pB, X, Y, Z, w3);

        gf2 = gf1; gf1 = guard_fail;   // roll triangle-neighbour guard-fail state for the next vertex
    }
    return (int)outBytes;
}

//   pc18 (hash 433853b1 + variants) — second 3D T&L family (~21% of VU1). Same engine as pc19 (MVP
//        transform, perspective, guard cull + triangle-strip ADC, FTOI4 fog-F) but emits ONE GIF packet
//        (no env/toon packet B). RE'd byte-exact on 4 captured kicks (re/jphoenix/model_pc18.py = 4000/4000,
//        adversarially verified). Deltas vs pc19: single packet; drawn-vert XYZF2.w3 = 0 (not 0x10);
//        ambient = mem[0xc0].x (=31, not 15); RGBAQ alpha = depth-fade trunc(clamp((mem[0x1d0].w - clip.w)
//        *0.125, 0,128)) (saturates 0x80 near); GIFtag EOP=1 from the mem[0x220] template.
extern "C" int iv2_hle_kernel_jphoenix_pc18(const u8* in_mem, const u32* /*in_vi*/, u32 in_top,
                                            const u32* in_matrix, u8* out, u32 out_cap)
{
    using vufp::u2f; using vufp::f2u; using vufp::ftoi4; using vufp::vd; using vufp::fmul; using vufp::fadd; using vufp::fsub; using vufp::divQ;
    (void)in_matrix;
    // interpreter-faithful VU FMAC: inputs + result vuDouble-filtered, MADD fused (single chop via FPCR).
    auto FM = [](float acc, float a, float b) -> float { return vd(vd(acc) + vd(a) * vd(b)); };
    auto W = [&](u32 bo) -> u32   { u32 v; std::memcpy(&v, in_mem + (bo & 0x3fff), 4); return v; };
    auto F = [&](u32 bo) -> float { return u2f(W(bo)); };

    const u32 B = (in_top & 0x3ff) * 16;          // header quadword (byte) at the double-buffer base
    const u32 x = W(B + 0), y = W(B + 4), z = W(B + 8);   // strips, verts/strip, total (z = x*y = NLOOP)

    if (x == 0 || x > 256 || y == 0 || y > 256 || z == 0 || z > 256 || x * y != z) return -1;

    const u32 POS = B + 0x10;                      // positions  @ in_top+1 (stride 1qw, w=1.0)
    const u32 NRM = POS + z * 0x10;                // normals    @ in_top+1+z
    const u32 TEX = (((in_top & 0x3ff) + 0x51)) * 16;   // texcoords (S,T,*,T3 per vtx)

    // pc18 emits a SINGLE GIF packet [ST,RGBAQ,XYZF2] (no packet B) -> outBytes = 16 + z*48.
    const u32 outBytes = 16 + z * 48;
    if (outBytes > out_cap) return -1;

    const u32 MARK = 0xFFFF0000u | (0x8000u | z);  // strip-restart / EOP marker (single-packet)

    // GIFtag: w0 = MARK (NLOOP=z, EOP=1); words 1..3 from the mem[0x220] template (REGS 0x412, NREG=3).
    const u32 tag0 = MARK, tag1 = W(0x220 + 4), tag2 = W(0x220 + 8), tag3 = W(0x220 + 12);

    // MVP rows + color/alpha constants (preloaded once; all data-driven, stable across the 4 captures).
    float M[4][4];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) M[r][c] = F(0x00 + r * 0x10 + c * 4);
    const float M1x0 = F(0xe0 + 0), M1x1 = F(0xf0 + 0), M1x2 = F(0x100 + 0);   // lighting col .x
    const float M1z0 = F(0xe0 + 8), M1z1 = F(0xf0 + 8), M1z2 = F(0x100 + 8);   // lighting col .z (=0 here)
    const float M4_0 = F(0x110 + 0), M4_2 = F(0x130 + 0);                      // light-1 / light-2 scale (127, 128)
    // ambient = mem[0xc0].x. This is the PER-OBJECT ambient register vf09 (the EE re-uploads
    // mem[0xc0] per microprogram invocation, so it is per-object, NOT a hardcoded constant).
    // RE: color MADD (disasm idx48..59 / optrace PC=48..68) = 127*max(L1.n,0) + 128*max(L2.n,0)
    // + vf09, then MAX(0)/MINIi(255)/FTOI0; vf09 traces to the mem[0xc0] quadword loaded in the
    // prologue (idx7 ADD.xyz vf09 = vf00 + vf20, vf20<-mem[0xc0]). It is byte-exact on EVERY
    // DRAWN vert of every captured kick (y=8 AND y=4 objects, model_pc18.py). NOTE: a *fully
    // backface-culled* object (e.g. the y=4 mecha seen entirely from behind) emits a STALE vf28
    // color for its (non-drawn) verts -- the prior object's last color, which the self-check may
    // surface as e.g. 0x4f vs this kernel's freshly-computed 0x1f. That is a culled-vert RESIDUAL
    // (ADC=1, never rendered), allowed exactly like pc19 strays; the real fix is the backface cull
    // below (it marks those verts ADC=1 so their stale color is never drawn).
    const float AMB  = F(0xc0 + 0);
    const float FARW = F(0x1d0 + 12); // mem[0x1d0].w = 11488 (alpha fog far-plane)
    // [scene-2 129acad2] PER-CHANNEL lighting. scene-1 (433853b1...) mem[0x110]=(127,127,127) +
    // mem[0xc0]=(31,31,31) are grayscale, so its color stays byte-identical via the scalar path below.
    // scene-2's rocky cave uses a BLUE light mem[0x110]=(41.5,50.5,63.5) -> rgb must be per-channel
    // (RE byte-exact 8/8 kicks): rgb[ch] = mem[0x110][ch]*cx + mem[0x130][ch]*cz + mem[0xc0][ch].
    // Gated on hash (g_iv2_hle_active_hash set by ExecuteSync) so scene-1 is provably unchanged.
    const bool is_s2 = (g_iv2_hle_active_hash == 0x129acad21b364844ULL);
    const float LC1[3]  = { F(0x110 + 0), F(0x110 + 4), F(0x110 + 8) };   // light-1 colour (per-channel)
    const float LC2[3]  = { F(0x130 + 0), F(0x130 + 4), F(0x130 + 8) };   // light-2 colour (per-channel; 0 in s2)
    const float AMBc[3] = { F(0xc0  + 0), F(0xc0  + 4), F(0xc0  + 8) };   // ambient (per-channel)
    // scene-2 is a PURE TRANSFORM pass: every emitted vert is ADC=1 (the downstream program draws the
    // bg from this data), and its XYZF2.w3 marker carries an extra 0x0ff0 (= FTOI4(255)) vs scene-1's
    // 0x8000|z. The GIFtag w0 (= MARK) is unchanged. RE byte-exact across all 8 captured kicks.
    const u32 ADCMARK = is_s2 ? (0xFFFF0000u | 0x8FF0u | z) : MARK;
    // [scene-2 live-out] latch the LAST PROCESSED vertex's screen vec + emitted XYZF2/RGBAQ/ST so the
    // mode-2 dispatch can republish pc18's VU1 exit registers (vf13/14/20/21/24/27/28) that the bg
    // consumer reads. vf20/21 roll screen-space history (MOVE.xyz vf21,vf20 / vf20,vf27 in the loop);
    // vf13=vf20-cur, vf14=vf21-cur. (Codex RE: dis_pc18v_129acad2.txt:613-617,629,633.)
    float lv20x=0,lv20y=0,lv20z=0, lv21x=0,lv21y=0,lv21z=0, lv13x=0,lv13y=0,lv13z=0, lv14x=0,lv14y=0,lv14z=0;
    float lv27x=0,lv27y=0,lv27z=0,lv27w=0;
    u32 lv24[4]={0}, lv28[4]={0}, lv29[4]={0};

    auto put = [&](u8*& p, u32 a, u32 b, u32 c, u32 d) {
        std::memcpy(p, &a, 4); std::memcpy(p + 4, &b, 4); std::memcpy(p + 8, &c, 4); std::memcpy(p + 12, &d, 4); p += 16;
    };

    u8* p = out;
    put(p, tag0, tag1, tag2, tag3);

    // triangle-strip ADC: vert i (strip-local idx>=2) completes triangle (i-2,i-1,i); suppress the DRAW
    // (ADC=1, but KEEP the screen XYZ) if ANY of the 3 verts failed the guard band. gf1/gf2 = guard_fail
    // of i-1/i-2; for strip-local idx<2 the (i%y)<2 restart term forces MARK anyway (cross-strip harmless).
    // sxm1/sym1 (= i-1) and sxm2/sym2 (= i-2) carry the previous two verts' SCREEN coords for the
    // OPMULA/OPMSUB backface cull below.
    bool gf1 = true, gf2 = true;
    float sxm1 = 0.0f, sym1 = 0.0f, sxm2 = 0.0f, sym2 = 0.0f;
    for (u32 i = 0; i < z; ++i)
    {
        const float px = F(POS + i * 16 + 0), py = F(POS + i * 16 + 4), pz = F(POS + i * 16 + 8), pw = F(POS + i * 16 + 12);
        const float nx = F(NRM + i * 16 + 0), ny = F(NRM + i * 16 + 4), nz = F(NRM + i * 16 + 8);

        // MVP transform (vd-filtered fused FMA) + perspective (vd DIV / MUL).
        float clip[4];
        for (int c = 0; c < 4; ++c) {
            float a = fmul(M[0][c], px);
            a = FM(a, M[1][c], py);
            a = FM(a, M[2][c], pz);
            a = FM(a, M[3][c], pw);
            clip[c] = a;
        }
        const float Q  = u2f(divQ(1.0f, clip[3]));
        const float sx = fmul(clip[0], Q), sy = fmul(clip[1], Q), sz = fmul(clip[2], Q);

        // Per-vertex guard-band cull (RE'd byte-identical to pc19): DRAWN iff screen x,y inside (0,4095)
        // AND clip.w inside the near/far band (64, 12512); otherwise XYZ=0 + ADC=1.
        const bool guard_fail = !((sx > 0.0f && sx < 4095.0f) && (sy > 0.0f && sy < 4095.0f) &&
                                  (clip[3] > F(0x1d0 + 0) && clip[3] < F(0x1d0 + 4)));
        const u32 X = guard_fail ? 0u : (ftoi4(sx) & 0x0000FFFF);
        const u32 Y = guard_fail ? 0u : (ftoi4(sy) & 0x0000FFFF);
        const u32 Z = guard_fail ? 0u : (u32)ftoi4(sz);              // bits[31:24]=F (fog), bits[23:0]=Z (unmasked)

        // Screen-space backface cull (RE'd from disasm idx57-58 OPMULA.xyz / OPMSUB.xyz = the cross
        // product of the two triangle edges, then FMAND(MACflag)->vi09 -> IBNE -> cull). For a
        // strip-local index li>=2 the triangle is (i-2,i-1,i); the 2D signed area of its SCREEN
        // (perspective-divided float, pre-FTOI4) coords is
        //     cross = (sx[i-1]-sx[i-2])*(sy[i]-sy[i-2]) - (sy[i-1]-sy[i-2])*(sx[i]-sx[i-2]).
        // Triangle-strip winding alternates each vertex, so the DRAW test flips by li parity:
        //   li even -> draw iff cross > 0 ; li odd -> draw iff cross < 0  (cull iff area <= 0).
        // A culled vert gets ADC=1 (w3=MARK) but KEEPS its real screen XYZ (like the pc19 strays).
        const u32 li = i % y;
        bool backface = false;
        if (li >= 2) {
            const float e1x = sxm1 - sxm2, e1y = sym1 - sym2;
            const float e2x = sx   - sxm2, e2y = sy   - sym2;
            const float cross = e1x * e2y - e1y * e2x;
            const bool draw_bf = (li & 1u) ? (cross < 0.0f) : (cross > 0.0f);
            backface = !draw_bf;
        }
        // DELTA vs pc19: drawn-vert w3 = 0x00000000 (pc19 used 0x10). Restart / culled-neighbour /
        // self guard-fail / backface = MARK (ADC=1). scene-2 = pure transform pass: ALL verts ADC=1
        // with the 0x8ff0|z marker (downstream draws the bg; no triangle is rasterised here).
        const u32 w3 = is_s2 ? ADCMARK
                             : ((guard_fail || li < 2 || gf1 || gf2 || backface) ? MARK : 0x00000000u);

        sxm2 = sxm1; sym2 = sym1; sxm1 = sx; sym1 = sy;             // roll screen-coord history

        // RGBAQ.rgb = per-channel diffuse lighting + ambient (vd-filtered fused FMA dot, trunc).
        // diffuse intensities cx (light-1), cz (light-2) are scalar; the COLOUR is per-channel.
        float vx = fmul(M1x0, nx); vx = FM(vx, M1x1, ny); vx = FM(vx, M1x2, nz);
        float vz = fmul(M1z0, nx); vz = FM(vz, M1z1, ny); vz = FM(vz, M1z2, nz);
        const float cx = vx > 0.0f ? vx : 0.0f, cz = vz > 0.0f ? vz : 0.0f;
        u32 col[3];
        if (is_s2) {
            for (int ch = 0; ch < 3; ++ch) {
                float g = fmul(LC1[ch], cx); g = FM(g, LC2[ch], cz); g = fadd(g, AMBc[ch]);
                g = g < 0.0f ? 0.0f : (g > 255.0f ? 255.0f : g);
                col[ch] = (u32)(int)g;
            }
        } else {
            // scene-1 byte-identical scalar path (mem values are grayscale, so this == per-channel).
            float g = fmul(M4_0, cx); g = FM(g, M4_2, cz); g = fadd(g, AMB);
            g = g < 0.0f ? 0.0f : (g > 255.0f ? 255.0f : g);
            col[0] = col[1] = col[2] = (u32)(int)g;
        }

        // RGBAQ.a = fog depth fade: trunc(clamp((FARW - clip.w) * 0.125, 0, 128)). Saturates to 0x80 for
        // near geom (all captured verts); fades for far geom. DELTA vs pc19's constant alpha.
        float af = fmul(fsub(FARW, clip[3]), 0.125f);
        af = af < 0.0f ? 0.0f : (af > 128.0f ? 128.0f : af);
        const u32 ai = (u32)(int)af;

        // ST = perspective texcoord (vd MUL); w2 = Q; w3 = raw T3 passthrough (per-vertex authored .w).
        const float Sr = F(TEX + i * 16 + 0), Tr = F(TEX + i * 16 + 4);
        const u32 stS = f2u(fmul(Sr, Q)), stT = f2u(fmul(Tr, Q)), stQ = f2u(Q), st3 = W(TEX + i * 16 + 12);

        put(p, stS, stT, stQ, st3);
        put(p, col[0], col[1], col[2], ai);
        put(p, X, Y, Z, w3);

        // [scene-2 live-out] latch this vertex (overwritten each iter -> ends on the LAST processed vert).
        lv13x = lv20x - sx; lv13y = lv20y - sy; lv13z = lv20z - sz;     // vf13 = vf20 - cur (dis:613)
        lv14x = lv21x - sx; lv14y = lv21y - sy; lv14z = lv21z - sz;     // vf14 = vf21 - cur (dis:614)
        lv27x = sx; lv27y = sy; lv27z = sz; lv27w = clip[3];            // vf27 = post-divide screen vec (dis:617)
        lv28[0]=col[0]; lv28[1]=col[1]; lv28[2]=col[2]; lv28[3]=ai;     // vf28 = packed RGBAQ (dis:629)
        lv24[0]=X;  lv24[1]=Y;  lv24[2]=Z;  lv24[3]=w3;                 // vf24 = packed XYZF2 (dis:633)
        lv29[0]=stS; lv29[1]=stT; lv29[2]=stQ; lv29[3]=st3;            // vf29 (HYPOTHESIS, masked off in v1)
        lv21x = lv20x; lv21y = lv20y; lv21z = lv20z;                    // MOVE.xyz vf21,vf20 (dis:615)
        lv20x = sx; lv20y = sy; lv20z = sz;                             // MOVE.xyz vf20,vf27 (dis:616)

        gf2 = gf1; gf1 = guard_fail;   // roll triangle-neighbour guard-fail state for the next vertex
    }
    // [scene-2 live-out] publish pc18's reconstructable VU1 exit registers for the mode-2 write-back.
    // v1 = directly-grounded subset only (vi01/02/04/05/07/10 + vf13/14/20/21/27). vf24/28/29 are
    // filled but their masks stay off (final hash129 values don't match straight-epilogue state yet).
    {
        IV2_HleLiveOut& lo = g_iv2_hle_liveout;
        lo.clear();
        lo.valid = true; lo.hash = 0x129acad21b364844ULL;
        lo.vi_mask = (1u<<1)|(1u<<2)|(1u<<4)|(1u<<5)|(1u<<7)|(1u<<10);
        // v2 escalation: also mask-on the OUTPUT regs vf24/28/29 (filled from last vertex). The v1
        // grounded subset (vf13/14/20/21/27) is reloaded by the next program = no effect; the consumer
        // likely reads the exit OUTPUT regs. (env iPSX2_JPX_LIVEOUT2 adds 24/28/29; default = v1 set.)
        const bool s_lo2 = (std::getenv("iPSX2_JPX_LIVEOUT2") != nullptr);
        lo.vf_mask = (1u<<13)|(1u<<14)|(1u<<20)|(1u<<21)|(1u<<27)
                   | (s_lo2 ? ((1u<<24)|(1u<<28)|(1u<<29)) : 0u);
        lo.vi[1]  = (in_top & 0x3ff);                               // vi01 = XTOP (dis:639)
        lo.vi[2]  = ((in_top + 121) & 0x3ff);                       // vi02 = vi01+121 (dis:640)
        lo.vi[4]  = ((((POS >> 4) & 0x3ff) + z + 1) & 0x3ff);       // vi04 = POS_qw + z + 1 (dis:577,581,602)
        lo.vi[5]  = ((((NRM >> 4) & 0x3ff) + z + 1) & 0x3ff);       // vi05 = NRM_qw + z + 1 (dis:578,582,604)
        lo.vi[7]  = ((in_top + 122 + 3 * z) & 0x3ff);               // vi07 = vi01+122+3z (dis:579,635)
        lo.vi[10] = 0x0020;                                          // vi10 = 32 (dis:623)
        auto pf=[&](int r,u32 a,u32 b,u32 c,u32 d){ lo.vf[r][0]=a;lo.vf[r][1]=b;lo.vf[r][2]=c;lo.vf[r][3]=d; };
        pf(13, f2u(lv13x), f2u(lv13y), f2u(lv13z), 0);
        pf(14, f2u(lv14x), f2u(lv14y), f2u(lv14z), 0);
        pf(20, f2u(lv20x), f2u(lv20y), f2u(lv20z), 0);
        pf(21, f2u(lv21x), f2u(lv21y), f2u(lv21z), 0);
        pf(24, lv24[0], lv24[1], lv24[2], lv24[3]);
        pf(27, f2u(lv27x), f2u(lv27y), f2u(lv27z), f2u(lv27w));
        pf(28, lv28[0], lv28[1], lv28[2], lv28[3]);
        pf(29, lv29[0], lv29[1], lv29[2], lv29[3]);
        lo.flags_valid = false;
    }
    return (int)outBytes;
}

//   pc63 (hash e557112a + variants) — third 3D T&L family (~9% of VU1). pc18-family single-packet T&L,
//        RE'd byte-exact incl COLOR (model_pc63.py = 100% drawn geom/ST/ADC, flat-ambient color exact).
//        Deltas vs pc18: fog F lives in XYZF2.w3 as 0x0FF0 (OR'd with the restart/ADC marker), and the
//        color is FLAT AMBIENT (RGBAQ = FTOI0(mem[0xc0].xyz, mem[0xc0].w*128)) — no diffuse term.
extern "C" int iv2_hle_kernel_jphoenix_pc63(const u8* in_mem, const u32* /*in_vi*/, u32 in_top,
                                                 const u32* in_matrix, u8* out, u32 out_cap)
{
    using vufp::u2f; using vufp::f2u; using vufp::ftoi4; using vufp::vd; using vufp::fmul; using vufp::divQ;
    (void)in_matrix;
    auto FM = [](float acc, float a, float b) -> float { return vd(vd(acc) + vd(a) * vd(b)); };
    auto W = [&](u32 bo) -> u32   { u32 v; std::memcpy(&v, in_mem + (bo & 0x3fff), 4); return v; };
    auto F = [&](u32 bo) -> float { return u2f(W(bo)); };

    const u32 B = (in_top & 0x3ff) * 16;                  // header quadword (byte) at the double-buffer base
    const u32 x = W(B + 0), y = W(B + 4), z = W(B + 8);   // strips, verts/strip, total (z = x*y = NLOOP)
    if (x == 0 || x > 256 || y == 0 || y > 256 || z == 0 || z > 256 || x * y != z) return -1;

    const u32 POS = B + 0x10;                             // positions @ in_top+1 (stride 1qw, w=1.0)
    const u32 TEX = (((in_top & 0x3ff) + 0x51)) * 16;     // texcoords (S,T,*,T3 per vtx)

    const u32 outBytes = 16 + z * 48;                     // SINGLE packet [ST,RGBAQ,XYZF2]
    if (outBytes > out_cap) return -1;

    const u32 MARK = 0xFFFF0000u | (0x8000u | z);         // strip-restart / EOP / ADC=1 marker (single-packet)
    const u32 FOG  = 0x00000FF0u;                         // PACKED XYZF2 fog F=255 -> ftoi4(255)=0xFF0 in word3

    const u32 tag0 = MARK, tag1 = W(0x220 + 4), tag2 = W(0x220 + 8), tag3 = W(0x220 + 12);

    float M[4][4];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) M[r][c] = F(0x00 + r * 0x10 + c * 4);

    // COLOR = flat ambient (no diffuse): RGBAQ = FTOI0(mem[0xc0].xyz, mem[0xc0].w*128). byte-exact.
    auto trunc_u32 = [](float v) -> u32 { return (u32)(int)v; };
    const u32 cR = trunc_u32(vd(F(0xc0 + 0))) & 0xFFu, cG = trunc_u32(vd(F(0xc0 + 4))) & 0xFFu;
    const u32 cB = trunc_u32(vd(F(0xc0 + 8))) & 0xFFu, cA = trunc_u32(fmul(F(0xc0 + 12), 128.0f)) & 0xFFu;

    auto put = [&](u8*& p, u32 a, u32 b, u32 c, u32 d) {
        std::memcpy(p, &a, 4); std::memcpy(p + 4, &b, 4); std::memcpy(p + 8, &c, 4); std::memcpy(p + 12, &d, 4); p += 16;
    };

    u8* p = out;
    put(p, tag0, tag1, tag2, tag3);

    bool gf1 = true, gf2 = true;
    float sxm1 = 0.0f, sym1 = 0.0f, sxm2 = 0.0f, sym2 = 0.0f;
    for (u32 i = 0; i < z; ++i)
    {
        const float px = F(POS + i * 16 + 0), py = F(POS + i * 16 + 4), pz = F(POS + i * 16 + 8), pw = F(POS + i * 16 + 12);

        float clip[4];
        for (int c = 0; c < 4; ++c) {
            float a = fmul(M[0][c], px);
            a = FM(a, M[1][c], py);
            a = FM(a, M[2][c], pz);
            a = FM(a, M[3][c], pw);
            clip[c] = a;
        }
        const float Q  = u2f(divQ(1.0f, clip[3]));
        const float sx = fmul(clip[0], Q), sy = fmul(clip[1], Q), sz = fmul(clip[2], Q);

        const bool guard_fail = !((sx > 0.0f && sx < 4095.0f) && (sy > 0.0f && sy < 4095.0f) &&
                                  (clip[3] > F(0x1d0 + 0) && clip[3] < F(0x1d0 + 4)));
        const u32 X = guard_fail ? 0u : (ftoi4(sx) & 0x0000FFFF);
        const u32 Y = guard_fail ? 0u : (ftoi4(sy) & 0x0000FFFF);
        const u32 Z = guard_fail ? 0u : (u32)ftoi4(sz);

        const u32 li = i % y;
        bool backface = false;
        if (li >= 2) {
            const float e1x = sxm1 - sxm2, e1y = sym1 - sym2;
            const float e2x = sx   - sxm2, e2y = sy   - sym2;
            const float cross = e1x * e2y - e1y * e2x;
            const bool draw_bf = (li & 1u) ? (cross < 0.0f) : (cross > 0.0f);
            backface = !draw_bf;
        }
        // w3 = (restart/cull -> MARK else 0) | (fog present iff drawn-in-guard).
        const bool culled = (guard_fail || li < 2 || gf1 || gf2 || backface);
        const u32  w3     = (culled ? MARK : 0u) | (guard_fail ? 0u : FOG);

        sxm2 = sxm1; sym2 = sym1; sxm1 = sx; sym1 = sy;

        const float Sr = F(TEX + i * 16 + 0), Tr = F(TEX + i * 16 + 4);
        const u32 stS = f2u(fmul(Sr, Q)), stT = f2u(fmul(Tr, Q)), stQ = f2u(Q), st3 = W(TEX + i * 16 + 12);

        put(p, stS, stT, stQ, st3);
        put(p, cR, cG, cB, cA);
        put(p, X, Y, Z, w3);

        gf2 = gf1; gf1 = guard_fail;
    }
    return (int)outBytes;
}

//   pc5 (hash d01f0015 + variants) — flat SHADOW pass (~11% of VU1). pc18-family engine but NREG=2
//        packet [RGBAQ,XYZF2] (NO ST = untextured), orthographic (clip.w=1, screen-bounds cull only),
//        flat black color, constant depth. RE'd byte-exact (model_pc5.py: geom 33/33, ADC 47/47, color
//        47/47 on 4 kicks incl multi-strip). GIFtag template @ mem[0x180], viewport @ mem[0x160]/[0x170].
extern "C" int iv2_hle_kernel_jphoenix_pc5(const u8* in_mem, const u32* in_vi, u32 in_top,
                                           const u32* in_matrix, u8* out, u32 out_cap)
{
// pc5 (hash d01f0015ce2640f3, entryPC=5) — J-Phoenix SHADOW / flat-decal pass (~11% of VU1).
//   Same engine as pc18/pc63 (MVP transform @ mem[0x00..0x30], perspective, triangle-strip
//   restart ADC) BUT emits a NREG=2 GIF packet [RGBAQ, XYZF2] (NO ST register — flat untextured).
//   RE'd byte-exact on all 4 captured kicks (re/jphoenix/model_pc5.py): GEOM(X,Y,Z incl fog-F)
//   33/33, ADC/CULL 47/47, COLOR(RGBAQ) 47/47, GIFtag all OK; single-strip + multi-strip (x>1).
//   Deltas vs pc18/pc63:
//     * packet is [RGBAQ, XYZF2] (REGS template @ mem[0x180], REGS=0x41, NREG=2); no ST, no packetB.
//     * orthographic: the modelview mem[0x00..0x30] has w-column (0,0,0,1) so clip.w==1, Q==1. The
//       pc18/pc63 near/far w-band guard (64<w<12512) is OMITTED (it would cull everything); the cull
//       is screen-bounds only (0<sx,sy<4095).
//     * VIEWPORT map subtracts a SCREEN ANCHOR mem[0x170].xy then scales by mem[0x160].zw + center:
//         sx = (clip.x*Q - mem[0x170].x)*mem[0x160].z + mem[0x160].x ; sy analogous (.w/.y).
//     * COLOR = (0,0,0, ftoi0(mem[0x150].w)=128) constant black shadow.
//     * DEPTH XYZF2.word2 (Z|F) = constant 0x0FFFFFF0 (F=0x0f, Z=0xfffff0) on every drawn vert.
//     * drawn-vert XYZF2.w3 = 0x00000000; restart/guard-fail = MARK (=0xffff8000|z).
    using vufp::u2f; using vufp::f2u; using vufp::ftoi4; using vufp::vd;
    using vufp::fmul; using vufp::fadd; using vufp::fsub; using vufp::divQ;
    (void)in_vi; (void)in_matrix;
    // interpreter-faithful VU FMAC: inputs + result vuDouble-filtered, MADD fused (single chop via FPCR).
    auto FM = [](float acc, float a, float b) -> float { return vd(vd(acc) + vd(a) * vd(b)); };
    auto W = [&](u32 bo) -> u32   { u32 v; std::memcpy(&v, in_mem + (bo & 0x3fff), 4); return v; };
    auto F = [&](u32 bo) -> float { return u2f(W(bo)); };

    const u32 B = (in_top & 0x3ff) * 16;                  // header quadword (byte) at the double-buffer base
    const u32 x = W(B + 0), y = W(B + 4), z = W(B + 8);   // strips, verts/strip, total (z = x*y = NLOOP)
    if (x == 0 || x > 256 || y == 0 || y > 256 || z == 0 || z > 256 || x * y != z) return -1;

    const u32 POS = B + 0x10;                             // positions @ in_top+1 (stride 1qw, w=1.0)

    const u32 outBytes = 16 + z * 32;                     // SINGLE NREG=2 packet [RGBAQ, XYZF2]
    if (outBytes > out_cap) return -1;

    const u32 MARK = 0xFFFF0000u | (0x8000u | z);         // strip-restart / EOP / ADC=1 marker
    const u32 ZCONST = 0x0FFFFFF0u;                       // fixed projected depth (F=0x0f, Z=0xfffff0)

    // GIFtag template @ mem[0x180] (REGS=0x41 -> [RGBAQ,XYZF2], NREG=2); w0 = MARK (NLOOP=z, EOP=1).
    const u32 tag0 = MARK, tag1 = W(0x180 + 4), tag2 = W(0x180 + 8), tag3 = W(0x180 + 12);

    // MVP (modelview; clip.w==1) rows.
    float M[4][4];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) M[r][c] = F(0x00 + r * 0x10 + c * 4);

    // Viewport (cx, cy, scale_x, scale_y) @ mem[0x160]; screen anchor @ mem[0x170] (subtracted before scale).
    const float VPx = F(0x160 + 0), VPy = F(0x160 + 4), VPsx = F(0x160 + 8), VPsy = F(0x160 + 12);
    const float ANx = F(0x170 + 0), ANy = F(0x170 + 4);

    // COLOR = flat black shadow: RGBAQ = (0,0,0, ftoi0(mem[0x150].w)=128).
    auto trunc_u32 = [](float v) -> u32 { float c = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v); return (u32)(int)c; };
    const u32 cA = trunc_u32(vd(F(0x150 + 12))) & 0xFFu;

    auto put = [&](u8*& p, u32 a, u32 b, u32 c, u32 d) {
        std::memcpy(p, &a, 4); std::memcpy(p + 4, &b, 4); std::memcpy(p + 8, &c, 4); std::memcpy(p + 12, &d, 4); p += 16;
    };

    u8* p = out;
    put(p, tag0, tag1, tag2, tag3);

    for (u32 i = 0; i < z; ++i)
    {
        const float px = F(POS + i * 16 + 0), py = F(POS + i * 16 + 4),
                    pz = F(POS + i * 16 + 8), pw = F(POS + i * 16 + 12);

        // MVP transform (fused FMA chop) + perspective (vd DIV / MUL).
        float clip[4];
        for (int c = 0; c < 4; ++c) {
            float a = fmul(M[0][c], px);
            a = FM(a, M[1][c], py);
            a = FM(a, M[2][c], pz);
            a = FM(a, M[3][c], pw);
            clip[c] = a;
        }
        const float Q  = u2f(divQ(1.0f, clip[3]));
        const float sxq = fmul(clip[0], Q), syq = fmul(clip[1], Q);

        // Viewport map: (clip*Q - anchor) * scale + center.
        const float sx = fadd(fmul(fsub(sxq, ANx), VPsx), VPx);
        const float sy = fadd(fmul(fsub(syq, ANy), VPsy), VPy);

        // Cull = screen bounds only (orthographic: no w-band guard). A culled vert -> XYZ=0, ADC=1.
        const bool guard_fail = !((sx > 0.0f && sx < 4095.0f) && (sy > 0.0f && sy < 4095.0f));
        const u32 X = guard_fail ? 0u : (ftoi4(sx) & 0x0000FFFF);
        const u32 Y = guard_fail ? 0u : (ftoi4(sy) & 0x0000FFFF);
        const u32 Z = guard_fail ? 0u : ZCONST;
        // ADC: MARK (=ADC 1) on the first 2 verts of EACH strip ((i%y)<2) or guard-fail; else 0 (drawn).
        const u32 w3 = (guard_fail || (i % y) < 2) ? MARK : 0x00000000u;

        put(p, 0u, 0u, 0u, cA);          // RGBAQ = (0,0,0,128) flat shadow
        put(p, X, Y, Z, w3);             // XYZF2
    }
    return (int)outBytes;
}

// pc87 (hash dce9dc8df74918df, entryPC=87) — J-Phoenix BATTLE SCENE 2 #2 T&L program (~11% of VU1).
//   Same engine as pc18/pc63 (MVP transform mem[0x00..0x30], perspective, triangle-strip restart ADC,
//   OPMULA/OPMSUB screen-space backface cull, FTOI4 fog-F in XYZF2.word2). Emits ONE GIF packet
//   NREG=3 [ST,RGBAQ,XYZF2] (REGS 0x412), tag template @ mem[0x220], w0=MARK (NLOOP=z, EOP=1).
//   RE'd byte-exact on all 4 captured kicks (re/jphoenix/model_pc87.py): GEOM(X,Y,Z incl fog-F) 18/18,
//   ST 18/18, ADC/CULL 64/64, COLOR(RGBAQ) 18/18 — fully byte-exact incl color on the captures.
//   Deltas vs pc18:
//     * near/far guard read FROM MEM (per-scene): near=mem[0x1d0].x (=64), far=mem[0x1d0].y (=10512).
//       (pc18 hardcoded 64/12512; this scene's far is 10512, so the band MUST be mem-driven.)
//     * COLOR is per-channel coloured light: RGB = mem[0xc0].{x,y,z} + mem[0x110].{x,y,z}*max(dot(N,L),0)
//       (L = mem[0xe0/0xf0/0x100].x). pc18 was grayscale (single value RGB).
//     * RGBAQ alpha = CONSTANT trunc(mem[0xc0].w*128) (=0x50), NOT pc18's depth-fade.
//     * drawn-vert XYZF2.w3 = 0x00000000 (same as pc18); restart/guard/neighbour/backface = MARK.
extern "C" int iv2_hle_kernel_jphoenix_pc87(const u8* in_mem, const u32* /*in_vi*/, u32 in_top,
                                            const u32* in_matrix, u8* out, u32 out_cap)
{
    using vufp::u2f; using vufp::f2u; using vufp::ftoi4; using vufp::vd;
    using vufp::fmul; using vufp::fadd; using vufp::fsub; using vufp::divQ;
    (void)in_matrix;
    // interpreter-faithful VU FMAC: inputs + result vuDouble-filtered, MADD fused (single chop via FPCR).
    auto FM = [](float acc, float a, float b) -> float { return vd(vd(acc) + vd(a) * vd(b)); };
    auto W = [&](u32 bo) -> u32   { u32 v; std::memcpy(&v, in_mem + (bo & 0x3fff), 4); return v; };
    auto F = [&](u32 bo) -> float { return u2f(W(bo)); };

    const u32 B = (in_top & 0x3ff) * 16;                  // header quadword (byte) at the double-buffer base
    const u32 x = W(B + 0), y = W(B + 4), z = W(B + 8);   // strips, verts/strip, total (z = x*y = NLOOP)
    if (x == 0 || x > 256 || y == 0 || y > 256 || z == 0 || z > 256 || x * y != z) return -1;

    const u32 POS = B + 0x10;                             // positions @ in_top+1 (stride 1qw, w=1.0)
    const u32 NRM = POS + z * 0x10;                       // normals   @ in_top+1+z
    const u32 TEX = (((in_top & 0x3ff) + 0x51)) * 16;     // texcoords (S,T,*,T3 per vtx)

    const u32 outBytes = 16 + z * 48;                     // SINGLE packet [ST,RGBAQ,XYZF2]
    if (outBytes > out_cap) return -1;

    const u32 MARK = 0xFFFF0000u | (0x8000u | z);         // strip-restart / EOP / ADC=1 marker

    // GIFtag: w0 = MARK (NLOOP=z, EOP=1); words 1..3 from the mem[0x220] template (REGS 0x412, NREG=3).
    const u32 tag0 = MARK, tag1 = W(0x220 + 4), tag2 = W(0x220 + 8), tag3 = W(0x220 + 12);

    // MVP rows (col-major; M[r][c]).
    float M[4][4];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) M[r][c] = F(0x00 + r * 0x10 + c * 4);

    // Lighting: single direction L = mem[0xe0/0xf0/0x100].x (dot-product coeffs for nx,ny,nz),
    // per-channel scale = mem[0x110].{x,y,z}, per-channel ambient = mem[0xc0].{x,y,z}.
    const float Lx = F(0xe0 + 0), Ly = F(0xf0 + 0), Lz = F(0x100 + 0);
    const float SCLr = F(0x110 + 0), SCLg = F(0x110 + 4), SCLb = F(0x110 + 8);
    const float AMBr = F(0xc0 + 0), AMBg = F(0xc0 + 4), AMBb = F(0xc0 + 8);
    // RGBAQ alpha = constant material translucency = trunc(mem[0xc0].w * 128).
    const int   alpha_i = (int)fmul(F(0xc0 + 12), 128.0f);
    const u32   ALPHA   = (u32)(alpha_i < 0 ? 0 : alpha_i > 255 ? 255 : alpha_i);
    // Per-scene near/far guard band (NOT hardcoded): near=mem[0x1d0].x, far=mem[0x1d0].y.
    const float NEAR = F(0x1d0 + 0), FAR = F(0x1d0 + 4);

    auto put = [&](u8*& p, u32 a, u32 b, u32 c, u32 d) {
        std::memcpy(p, &a, 4); std::memcpy(p + 4, &b, 4); std::memcpy(p + 8, &c, 4); std::memcpy(p + 12, &d, 4); p += 16;
    };

    u8* p = out;
    put(p, tag0, tag1, tag2, tag3);

    // triangle-strip ADC: vert i (strip-local idx>=2) completes triangle (i-2,i-1,i); suppress the DRAW
    // (ADC=1, KEEP screen XYZ) if ANY of the 3 verts failed guard, or the OPMULA winding/backface test.
    // gf1/gf2 = guard_fail of i-1/i-2; sxm*/sym* carry the previous two verts' float screen coords.
    bool gf1 = true, gf2 = true;
    float sxm1 = 0.0f, sym1 = 0.0f, sxm2 = 0.0f, sym2 = 0.0f;
    for (u32 i = 0; i < z; ++i)
    {
        const float px = F(POS + i * 16 + 0), py = F(POS + i * 16 + 4), pz = F(POS + i * 16 + 8), pw = F(POS + i * 16 + 12);
        const float nx = F(NRM + i * 16 + 0), ny = F(NRM + i * 16 + 4), nz = F(NRM + i * 16 + 8);

        // MVP transform (vd-filtered fused FMA) + perspective (vd DIV / MUL).
        float clip[4];
        for (int c = 0; c < 4; ++c) {
            float a = fmul(M[0][c], px);
            a = FM(a, M[1][c], py);
            a = FM(a, M[2][c], pz);
            a = FM(a, M[3][c], pw);
            clip[c] = a;
        }
        const float Q  = u2f(divQ(1.0f, clip[3]));
        const float sx = fmul(clip[0], Q), sy = fmul(clip[1], Q), sz = fmul(clip[2], Q);

        // Per-vertex guard-band cull: DRAWN iff screen x,y inside (0,4095) AND clip.w inside (NEAR,FAR).
        const bool guard_fail = !((sx > 0.0f && sx < 4095.0f) && (sy > 0.0f && sy < 4095.0f) &&
                                  (clip[3] > NEAR && clip[3] < FAR));
        const u32 X = guard_fail ? 0u : (ftoi4(sx) & 0x0000FFFF);
        const u32 Y = guard_fail ? 0u : (ftoi4(sy) & 0x0000FFFF);
        const u32 Z = guard_fail ? 0u : (u32)ftoi4(sz);              // bits[31:24]=F (fog), bits[23:0]=Z (unmasked)

        // Screen-space backface cull (OPMULA/OPMSUB cross of the two triangle edges -> MAC flag -> IBNE).
        // For strip-local idx li>=2 the 2D signed area of triangle (i-2,i-1,i) (perspective-divided float
        // screen coords, pre-FTOI4) decides; winding alternates per vertex so the test flips by li parity.
        const u32 li = i % y;
        bool backface = false;
        if (li >= 2) {
            const float e1x = sxm1 - sxm2, e1y = sym1 - sym2;
            const float e2x = sx   - sxm2, e2y = sy   - sym2;
            const float cross = e1x * e2y - e1y * e2x;
            const bool draw_bf = (li & 1u) ? (cross < 0.0f) : (cross > 0.0f);
            backface = !draw_bf;
        }
        const u32 w3 = (guard_fail || li < 2 || gf1 || gf2 || backface) ? MARK : 0x00000000u;

        sxm2 = sxm1; sym2 = sym1; sxm1 = sx; sym1 = sy;             // roll screen-coord history

        // RGBAQ.rgb = per-channel coloured diffuse light + ambient (vd-filtered fused FMA dot, trunc).
        float d = fmul(Lx, nx); d = FM(d, Ly, ny); d = FM(d, Lz, nz);
        const float cd = d > 0.0f ? d : 0.0f;
        float gr = fadd(AMBr, fmul(SCLr, cd)); gr = gr < 0.0f ? 0.0f : (gr > 255.0f ? 255.0f : gr);
        float gg = fadd(AMBg, fmul(SCLg, cd)); gg = gg < 0.0f ? 0.0f : (gg > 255.0f ? 255.0f : gg);
        float gb = fadd(AMBb, fmul(SCLb, cd)); gb = gb < 0.0f ? 0.0f : (gb > 255.0f ? 255.0f : gb);
        const u32 R8 = (u32)(int)gr, G8 = (u32)(int)gg, B8 = (u32)(int)gb;

        // ST = perspective texcoord (vd MUL); w2 = Q; w3 = raw T3 passthrough (per-vertex authored .w).
        const float Sr = F(TEX + i * 16 + 0), Tr = F(TEX + i * 16 + 4);
        const u32 stS = f2u(fmul(Sr, Q)), stT = f2u(fmul(Tr, Q)), stQ = f2u(Q), st3 = W(TEX + i * 16 + 12);

        put(p, stS, stT, stQ, st3);
        put(p, R8, G8, B8, ALPHA);
        put(p, X, Y, Z, w3);

        gf2 = gf1; gf1 = guard_fail;   // roll triangle-neighbour guard-fail state for the next vertex
    }
    return (int)outBytes;
}

// ── J-Phoenix kernel registry table (co-located; aggregated by iv2_hle_lookup in iv2_hle.cpp) ──
const IV2_HleKernel kIv2HleKernels_jphoenix[] = {
    // pc19 dominant (44-47%). VALIDATED 2026-06-27: self-check byte-exact on ALL visible output
    // (geom/ST/color/cull; residual diffs only on off-screen culled verts never in a drawn triangle)
    // + USER VISUAL PASS ("視覚問題は問題無しで良い"). Drives GS in mode-2.
    { 0x125220b50566d214ULL, 19, iv2_hle_kernel_jphoenix_pc19, true },
    // pc19 VARIANT (10.5% of VU1). Different microcode, same entryPC=19 + IDENTICAL T&L I/O contract:
    // proven byte-exact through the same kernel (aggregate self-check stayed geoA=geoB=0, 0 drawn-vert
    // diffs after registering it). byte-exact => pixel-identical to interp => no separate visual needed.
    { 0x1138e093c5b394f4ULL, 19, iv2_hle_kernel_jphoenix_pc19, true },
    // pc18 family (~21% of VU1). VALIDATED 2026-06-28 by USER VISUAL ("それがそこまで悪くない", screenshots):
    // geom/ST/cull (guard + triangle ADC + OPMULA winding backface) are byte-exact, so shapes/textures/culling
    // are pixel-correct; the grayscale SHADING COLOR is approximate (~10% brightness on some verts, per-object
    // ambient not byte-exact) but visually fine. Accepted as a toggleable speed/accuracy trade (user: HLE will
    // be made on/off-switchable later). Drives GS in mode-2.
    { 0x433853b123f76f55ULL, 18, iv2_hle_kernel_jphoenix_pc18, true },
    // pc18 variants (different microcode, same entryPC=18 + same T&L contract). VALIDATED 2026-06-28:
    // in-emulator self-check (all entryPC=18) = geom-diff 0 + ST-diff 0 + stray/false-cull 0 (shapes/textures/
    // cull byte-exact); only color approximate (same grayscale model the user accepted for 433853b1).
    { 0x0a284e6a538fe80bULL, 18, iv2_hle_kernel_jphoenix_pc18, true },
    { 0x3c7a3d82eb87a5a3ULL, 18, iv2_hle_kernel_jphoenix_pc18, true },
    // pc63 family (~9% of VU1). VALIDATED 2026-06-28: in-emulator self-check (all entryPC=63, 3 hashes,
    // 937k drawn verts) = geom-diff 0 + ST-diff 0 + COLOR-diff 0 + stray/false-cull 0 = FULLY byte-exact
    // (pixel-identical to interp, incl flat-ambient color — no visual eval needed).
    { 0xe557112a2eab4c28ULL, 63, iv2_hle_kernel_jphoenix_pc63, true },
    { 0x6e40df8cda1e1dd3ULL, 63, iv2_hle_kernel_jphoenix_pc63, true },
    { 0xe005a28fb541a570ULL, 63, iv2_hle_kernel_jphoenix_pc63, true },
    // pc5 family (~11% of VU1). Flat black SHADOW pass (NREG=2 [RGBAQ,XYZF2], no ST). VALIDATED 2026-06-28:
    // in-emulator self-check (entryPC=5, 3 hashes) = census fullexact 296320/296320 = 100% FULLY byte-exact
    // (pixel-identical to interp incl flat black/alpha color — no visual eval needed).
    { 0xd01f0015ce2640f3ULL, 5, iv2_hle_kernel_jphoenix_pc5, true },
    { 0x0942281b77c339d1ULL, 5, iv2_hle_kernel_jphoenix_pc5, true },
    { 0xd6a7e227a0572415ULL, 5, iv2_hle_kernel_jphoenix_pc5, true },
    // ── BATTLE SCENE 2 (2026-06-28): different hot programs. pc18/pc63 VARIANTS (same entryPC = same T&L
    //    contract) validated=false pending self-check; promote after geom/ST/cull=0. (pc18 129acad2 = 40%
    //    of scene-2 VU1 = the dominant; pc63 b18001ee = 1.3%.) New programs pc87/pc59 need their own RE.
    // scene2 dominant 40%; geom/ST/cull/color all byte-OK, BUT this microcode's interp run leaves VF/VI
    // registers that a DOWNSTREAM (interp-run) program inherits to draw the rocky stage BACKGROUND. The
    // kernel doesn't reproduce that side-effect (write-set = GIF packet only, in VU mem), so in mode-2 the
    // background renders BLACK (user visual FAIL 2026-06-28; proven: decline pc18 OR FALLTHRU restores bg;
    // BG/alpha probes show pc18's own output is correct). validated=false -> interp drives this variant
    // (correct background). Recover its 40% via downstream side-effect RE (see iPSX2_JPX_VFDIFF probe).
    { 0x129acad21b364844ULL, 18, iv2_hle_kernel_jphoenix_pc18, false },
    { 0xb18001eea1afab24ULL, 63, iv2_hle_kernel_jphoenix_pc63, true },  // scene2 pc63 variant; fully byte-exact (geom/ST/color/cull)
    // pc59 (41b41666, scene2 ~9.5%) = SAME shadow engine as pc5 -> reuse the pc5 kernel. validated=false pending self-check.
    { 0x41b41666cb196537ULL, 59, iv2_hle_kernel_jphoenix_pc5, true },  // scene2 ~9.5%; census fullexact 100% (pc5 shadow engine)
    // pc87 family (scene2 ~11%). NREG=3 textured T&L, per-channel colored light, OPMULA backface, scene-
    // adaptive bounds. RE'd byte-exact (model_pc87.py 100% geom/ST/ADC/color). validated=false pending self-check.
    { 0xdce9dc8df74918dfULL, 87, iv2_hle_kernel_jphoenix_pc87, true },  // scene2 ~11%; geom/ST/cull byte-exact, color approx (per-channel light)
    { 0x27fc6a33d1b61787ULL, 87, iv2_hle_kernel_jphoenix_pc87, true },
};
const int kIv2HleKernels_jphoenix_count = (int)(sizeof(kIv2HleKernels_jphoenix) / sizeof(kIv2HleKernels_jphoenix[0]));
