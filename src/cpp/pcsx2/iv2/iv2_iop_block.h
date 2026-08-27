// SPDX-License-Identifier: GPL-3.0+
// P101-2 Phase R: iv2 IOP (R3000A) ブロック構造 + decoder
//
// 方針: EE iv2 と別ブロックキャッシュ・別 pool を持つ。
//   - R3000A は 32-bit GPR, TLB なし, MMI/FPU なし
//   - 分岐命令は Fallback のまま (psxBSC 経由)
//     → doBranch() が delay-slot + iopEventTest() まで面倒を見る
//   - ブロック末尾 = 分岐命令 or MAX_BLOCK_INSTS 到達

#pragma once

#include "R3000A.h"

namespace iv2_iop {

using FallbackFn = void(*)();

enum Op : u8 {
    Op_Fallback = 0,
    Op_Nop,
    Op_SetConst,   // rt = (s32)imm32  (LUI: imm<<16)
    Op_AddIU,      // rt = rs + sign_ext(imm16)   ; ADDI は overflow trap 稀なので共通
    Op_AndI,       // rt = rs & zero_ext(imm16)
    Op_OrI,        // rt = rs | zero_ext(imm16)
    Op_XorI,       // rt = rs ^ zero_ext(imm16)
    Op_SltI,       // rt = (s32)rs < (s32)imm  ? 1 : 0
    Op_SltIU,      // rt = (u32)rs < (u32)(s32)imm ? 1 : 0
    Op_AddU,       // rd = rs + rt
    Op_SubU,       // rd = rs - rt
    Op_And,        // rd = rs & rt
    Op_Or,         // rd = rs | rt
    Op_Xor,        // rd = rs ^ rt
    Op_Nor,        // rd = ~(rs | rt)
    Op_Slt,        // rd = (s32)rs < (s32)rt
    Op_SltU,       // rd = (u32)rs < (u32)rt
    Op_Sll,        // rd = rt << sa
    Op_Srl,        // rd = rt >> sa
    Op_Sra,        // rd = (s32)rt >> sa
    Op_Sllv,       // rd = rt << (rs & 0x1F)
    Op_Srlv,       // rd = rt >> (rs & 0x1F)
    Op_Srav,       // rd = (s32)rt >> (rs & 0x1F)
    Op_Mfhi, Op_Mflo, Op_Mthi, Op_Mtlo,
    Op_Mult, Op_Multu, Op_Div, Op_Divu,
    // Load/Store (iopMemRead/Write 経由)
    Op_Lb, Op_Lbu, Op_Lh, Op_Lhu, Op_Lw,
    Op_Sb, Op_Sh, Op_Sw,

    // ─── P8: Native branches (block 末尾 3-op pattern) ────────
    // [BCheck] → [delay slot (pre-decoded)] → [BFinalize] をブロック末尾に emit。
    // BCheck は s_branch_taken / s_branch_target を設定する (pc は触らない)。
    // delay slot は通常命令として実行。
    // BFinalize が pc 更新 + iopEventTest gating を行い block を抜ける。
    Op_BCheckEq,        // src1=rs src2=rt imm=target_pc  (BEQ)
    Op_BCheckNe,        // (BNE)
    Op_BCheckLez,       // src1=rs imm=target_pc  (BLEZ, tests (s32)rs <= 0)
    Op_BCheckGtz,       // (BGTZ, tests (s32)rs > 0)
    Op_BCheckLtz,       // (REGIMM BLTZ)
    Op_BCheckGez,       // (REGIMM BGEZ)
    Op_BCheckLtzLink,   // BLTZAL: ALWAYS sets $31 = pc_of_branch+8, then tests (s32)rs < 0
    Op_BCheckGezLink,   // BGEZAL: ALWAYS sets $31 = pc_of_branch+8, then tests (s32)rs >= 0
    Op_JumpConst,       // J / JAL (unconditional). dest=link_reg (0 or 31). imm=target_pc
    Op_JumpReg,         // JR / JALR. src1=rs (target). dest=rd (0 for JR)
    Op_BFinalize,       // imm=fall_through_pc (= pc_of_branch+8)

    Op_COUNT
};

// 1 命令 = 16 bytes (align 8)
struct CachedInst {
    u32 raw;
    s32 imm;
    FallbackFn handler;   // Fallback 時: psxBSC[raw>>26]。native 時: nullptr
    u8  op;               // iv2_iop::Op
    u8  dest;             // rt (I-form) or rd (R-form)
    u8  src1;             // rs
    u8  src2;             // rt (R-form)
};
static_assert(sizeof(CachedInst) == 24, "CachedInst must be 24 bytes");

static constexpr u32 MAX_BLOCK_INSTS  = 32;
static constexpr u32 BLOCK_HASH_BITS  = 15;             // IOP 2MB -> 512K insts -> hash 32K
static constexpr u32 BLOCK_HASH_SIZE  = 1u << BLOCK_HASH_BITS;
static constexpr u32 BLOCK_HASH_MASK  = BLOCK_HASH_SIZE - 1;
static constexpr u32 BLOCK_POOL_CAP   = 8192;
static constexpr u32 INST_POOL_CAP    = 128 * 1024;     // 128K 命令 = 3MB

struct Block {
    u32 entry_pc;
    u32 end_pc;
    u32 inst_offset;
    u16 inst_count;
    u16 generation;
};

extern Block* g_block_map[BLOCK_HASH_SIZE];
extern Block  g_block_pool[BLOCK_POOL_CAP];
extern u32    g_block_pool_next;
extern CachedInst g_inst_pool[INST_POOL_CAP];
extern u32    g_inst_pool_next;
extern u16    g_generation;

static inline u32 block_hash(u32 pc) { return (pc >> 2) & BLOCK_HASH_MASK; }

Block* GetOrCompileBlock(u32 pc);
Block* CompileBlock(u32 pc);
void   InvalidateAll();


// [P8] Native branch mode (captured once at static init from env).
extern bool g_iop_native_branch_enabled;

// raw → CachedInst へのデコード。native 化できない命令は op=Op_Fallback + handler=psxBSC[raw>>26]
void DecodeInst(u32 raw, CachedInst& out);

// raw が block 終端 (分岐/jump/syscall/break/eret) か判定
bool IsTerminator(u32 raw);

} // namespace iv2_iop
