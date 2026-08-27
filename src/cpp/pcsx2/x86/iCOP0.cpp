// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Important Note to Future Developers:
//   None of the COP0 instructions are really critical performance items,
//   so don't waste time converting any more them into recompiled code
//   unless it can make them nicely compact.  Calling the C versions will
//   suffice.

#include "Common.h"
#include "R5900OpcodeTables.h"
#define pc ee_recomp_pc_isolated
#include "iR5900.h"
#undef pc
#include "iCOP0.h"

extern "C" u32 GetRecompilerPC();
namespace Interp = R5900::Interpreter::OpcodeImpl::COP0;
#if !defined(__ANDROID__)
using namespace x86Emitter;
#endif

// [iter202] cpuException がconfigした正しい EPC (R5900.cpp で定義)
extern u32 g_armsx2_last_real_epc;

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {
namespace COP0 {

/*********************************************************
*   COP0 opcodes                                         *
*                                                        *
*********************************************************/

// emits "setup" code for a COP0 branch test.  The instruction immediately following
// this should be a conditional Jump -- JZ or JNZ normally.
static void _setupBranchTest()
{
	_eeFlushAllDirty();

	// COP0 branch conditionals are based on the following equation:
	//  (((psHu16(DMAC_STAT) | ~psHu16(DMAC_PCR)) & 0x3ff) == 0x3ff)
	// BC0F checks if the statement is false, BC0T checks if the statement is true.

	// note: We only want to compare the 16 bit values of DMAC_STAT and PCR.
	// But using 32-bit loads here is ok (and faster), because we mask off
	// everything except the lower 10 bits away.

//	xMOV(eax, ptr[(&psHu32(DMAC_PCR))]);
    armLoadPtr(EAX, &psHu32(DMAC_PCR));
//	xMOV(ecx, 0x3ff); // ECX is our 10-bit mask var
    armAsm->Mov(ECX, 0x3ff);
//	xNOT(eax);
    armAsm->Mvn(EAX, EAX);
//	xOR(eax, ptr[(&psHu32(DMAC_STAT))]);
    armAsm->Orr(EAX, EAX, armLoadPtr(&psHu32(DMAC_STAT)));
//	xAND(eax, ecx);
    armAsm->And(EAX, EAX, ECX);
//	xCMP(eax, ecx);
    armAsm->Cmp(EAX, ECX);
}

void recBC0F()
{
    const u32 pc = GetRecompilerPC();
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const bool swap = TrySwapDelaySlot(0, 0, 0, false);
	_setupBranchTest();
//	recDoBranchImm(branchTo, JE32(0), false, swap);

    a64::Label label;
    armAsm->B(&label, a64::eq);
    recDoBranchImm(branchTo, &label, false, swap);
}

void recBC0T()
{
    const u32 pc = GetRecompilerPC();
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const bool swap = TrySwapDelaySlot(0, 0, 0, false);
	_setupBranchTest();
//	recDoBranchImm(branchTo, JNE32(0), false, swap);

    a64::Label label;
    armAsm->B(&label, a64::ne);
    recDoBranchImm(branchTo, &label, false, swap);
}

void recBC0FL()
{
    const u32 pc = GetRecompilerPC();
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
//	recDoBranchImm(branchTo, JE32(0), true, false);

    a64::Label label;
    armAsm->B(&label, a64::eq);
    recDoBranchImm(branchTo, &label, true, false);
}

void recBC0TL()
{
    const u32 pc = GetRecompilerPC();
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
//	recDoBranchImm(branchTo, JNE32(0), true, false);

    a64::Label label;
    armAsm->B(&label, a64::ne);
    recDoBranchImm(branchTo, &label, true, false);
}

void recTLBR() { recCall(Interp::TLBR); }
void recTLBP() { recCall(Interp::TLBP); }
void recTLBWI() { recCall(Interp::TLBWI); }
void recTLBWR() { recCall(Interp::TLBWR); }

void recERET()
{
	recBranchCall(Interp::ERET);
}

void recEI()
{
	// must branch after enabling interrupts, so that anything
	// pending gets triggered properly.
	recBranchCall(Interp::EI);
}

void recDI()
{
	//// No need to branch after disabling interrupts...

	//iFlushCall(0);

	//xMOV(eax, ptr[&cpuRegs.cycle ]);
	//xMOV(ptr[&g_nextBranchCycle], eax);

	//xFastCall((void*)(uptr)Interp::DI );

	// Fixes booting issues in the following games:
	// Jak X, Namco 50th anniversary, Spongebob the Movie, Spongebob Battle for Bikini Bottom,
	// The Incredibles, The Incredibles rize of the underminer, Soukou kihei armodyne, Garfield Saving Arlene, Tales of Fandom Vol. 2.
	if (!g_recompilingDelaySlot)
		recompileNextInstruction(false, false); // DI execution is delayed by one instruction

//	xMOV(eax, ptr[&cpuRegs.CP0.n.Status]);
    armLoad(EAX, PTR_CPU(cpuRegs.CP0.n.Status));
//	xTEST(eax, 0x20006); // EXL | ERL | EDI
    armAsm->Tst(EAX, 0x20006);
//	xForwardJNZ8 iHaveNoIdea;
    a64::Label iHaveNoIdea;
    armAsm->B(&iHaveNoIdea, a64::Condition::ne);
//	xTEST(eax, 0x18); // KSU
    armAsm->Tst(EAX, 0x18);
//	xForwardJNZ8 inUserMode;
    a64::Label inUserMode;
    armAsm->B(&inUserMode, a64::Condition::ne);
//	iHaveNoIdea.SetTarget();
    armBind(&iHaveNoIdea);
//	xAND(eax, ~(u32)0x10000); // EIE
    armAsm->And(EAX, EAX, ~(u32)0x10000);
//	xMOV(ptr[&cpuRegs.CP0.n.Status], eax);
    armStore(PTR_CPU(cpuRegs.CP0.n.Status), EAX);
//	inUserMode.SetTarget();
    armBind(&inUserMode);
}


#ifndef CP0_RECOMPILE

REC_SYS(MFC0);
REC_SYS(MTC0);

#else

void recMFC0()
{


	if (_Rd_ == 9)
	{
		// This case needs to be handled even if the write-back is ignored (_Rt_ == 0 )
//		xMOV(ecx, ptr32[&cpuRegs.cycle]);
//		xADD(ecx, scaleblockcycles_clear());
//		xMOV(ptr32[&cpuRegs.cycle], ecx); // update cycles
        armAdd(ECX, PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
//		xMOV(eax, ecx);
        armAsm->Mov(EAX, ECX);
//		xSUB(eax, ptr[&cpuRegs.lastCOP0Cycle]);
        armAsm->Sub(EAX, EAX, armLoad(PTR_CPU(cpuRegs.lastCOP0Cycle)));
//		xADD(ptr[&cpuRegs.CP0.n.Count], eax);
        armAdd(PTR_CPU(cpuRegs.CP0.n.Count), EAX);
//		xMOV(ptr[&cpuRegs.lastCOP0Cycle], ecx);
        armStore(PTR_CPU(cpuRegs.lastCOP0Cycle), ECX);

		if (!_Rt_)
			return;

		const int regt = _Rt_ ? _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE) : -1;
//		xMOVSX(xRegister64(regt), ptr32[&cpuRegs.CP0.r[_Rd_]]);
        armLoadsw(HostX(regt), PTR_CPU(cpuRegs.CP0.r[_Rd_]));  // [iter241] slot→phys fix
		return;
	}

	if (!_Rt_)
		return;

	if (_Rd_ == 25)
	{
		if (0 == (_Imm_ & 1)) // MFPS, register value ignored
		{
			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
//			xMOVSX(xRegister64(regt), ptr32[&cpuRegs.PERF.n.pccr]);
            armLoadsw(HostX(regt), PTR_CPU(cpuRegs.PERF.n.pccr));  // [iter241] slot→phys fix
		}
		else if (0 == (_Imm_ & 2)) // MFPC 0, only LSB of register matters
		{
			iFlushCall(FLUSH_INTERPRETER);
//			xMOV(eax, ptr32[&cpuRegs.cycle]);
//			xADD(eax, scaleblockcycles_clear());
//			xMOV(ptr32[&cpuRegs.cycle], eax); // update cycles
            armAdd(PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
//			xFastCall((void*)COP0_UpdatePCCR);
            armEmitCall(reinterpret_cast<void*>(COP0_UpdatePCCR));

			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
//			xMOVSX(xRegister64(regt), ptr32[&cpuRegs.PERF.n.pcr0]);
            armLoadsw(HostX(regt), PTR_CPU(cpuRegs.PERF.n.pcr0));  // [iter241] slot→phys fix
		}
		else // MFPC 1
		{
			iFlushCall(FLUSH_INTERPRETER);
//			xMOV(eax, ptr32[&cpuRegs.cycle]);
//			xADD(eax, scaleblockcycles_clear());
//			xMOV(ptr32[&cpuRegs.cycle], eax); // update cycles
            armAdd(PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
//			xFastCall((void*)COP0_UpdatePCCR);
            armEmitCall(reinterpret_cast<void*>(COP0_UpdatePCCR));

			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
//			xMOVSX(xRegister64(regt), ptr32[&cpuRegs.PERF.n.pcr1]);
            armLoadsw(HostX(regt), PTR_CPU(cpuRegs.PERF.n.pcr1));  // [iter241] slot→phys fix
		}

		return;
	}
	else if (_Rd_ == 24)
	{
		COP0_LOG("MFC0 Breakpoint debug Registers code = %x\n", cpuRegs.code & 0x3FF);
		return;
	}

	const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
//	xMOVSX(xRegister64(regt), ptr32[&cpuRegs.CP0.r[_Rd_]]);
    armLoadsw(HostX(regt), PTR_CPU(cpuRegs.CP0.r[_Rd_]));  // [iter241] slot→phys fix
}

// [iter92] @@MTC0_EPC@@ – MTC0 $Rt, EPC (COP0 r14) ランタイム監視probe
// Removal condition: EPC 書き込みパス（BIOS カーネルが MTC0 EPC を発行するか否か）確定時
static void probe_mtc0_epc()
{
    // [iter202] cpuException がsaveした正しい EPC を使って補正。
    // JIT flush バグ: ERET stub の MFC0→ADDIU→MTC0 で k1 が ARM64 物理registerから
    // cpuRegs.GPR.r[27] に書き戻されない → MTC0 が stale EPC を書く。
    // g_armsx2_last_real_epc には cpuException がconfigした正しい EPC が入っている。
    // Removal condition: JIT k1 flush バグafter fixed
    {
        const u32 original_epc = cpuRegs.CP0.n.EPC;
        // [iter646] ExecPS2 がconfigした正しい KUSEG エントリポイント (例: 0x82180) は信頼する。
        // BIOS が MTC0 EPC にenabledな KUSEG 値を書いた場合は iter202 補正を適用しない。
        const bool bios_set_valid_kuseg = (original_epc >= 0x00001000u && original_epc < 0x02000000u);
        bool should_override = (!bios_set_valid_kuseg &&
                                g_armsx2_last_real_epc != 0 &&
                                g_armsx2_last_real_epc < 0x20000000u);
        if (should_override) {
            cpuRegs.CP0.r[14] = g_armsx2_last_real_epc + 4u;
        } else if (!bios_set_valid_kuseg) {
            // fallback: $ra redirect
            const u32 ra_val = cpuRegs.GPR.r[31].UL[0];
            if (ra_val >= 0x00100000u && ra_val < 0x02000000u)
                cpuRegs.CP0.r[14] = ra_val;
            else
                cpuRegs.CP0.r[14] += 4u;
        }
        // bios_set_valid_kuseg == true: original_epc をそのまま保持
    }

    static int s_n = 0;
    if (s_n < 20) {
        s_n++;
        // [iter177] *v1 (functionポインタテーブルの先頭エントリ) をadd
        const u32 sc_pc = cpuRegs.CP0.n.EPC - 4u;
        const u32 v1val = cpuRegs.GPR.n.v1.UL[0];
        Console.WriteLn("@@MTC0_EPC@@ n=%d new_epc=%08x v1=%08x a0=%08x a1=%08x a2=%08x | stub[-8]=%08x stub[-4]=%08x stub[0]=%08x stub[+4]=%08x | *v1=%08x *(v1+4)=%08x",
            s_n, cpuRegs.CP0.n.EPC,
            v1val,
            cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0], cpuRegs.GPR.n.a2.UL[0],
            memRead32(sc_pc - 8u), memRead32(sc_pc - 4u), memRead32(sc_pc), memRead32(sc_pc + 4u),
            memRead32(v1val), memRead32(v1val + 4u));
        // [iter195] *v1 (OSDSYS が監視するモジュール完了フラグ) に 1 を書き込む。
        // OSDSYS は v1 (=0x000C8F74) が指すmemoryをポーリングして IOP 側モジュールロード完了を待つ。
        // 実際は IOP SIF RPC 経由で書き込まれるべきだが、HLE では書き込まれない。
        // Removal condition: BIOS 画面displayafter confirmed
        // [iter196] rangeチェックfix: 0x000C8F74 は 0x00100000 以下なのでcondition緩和
        if (v1val >= 0x00080000u && v1val < 0x02000000u) {
            memWrite32(v1val, 1u);
            Console.WriteLn("@@V1_WRITE@@ wrote 1 to *v1=%08x (module-ready HLE)", v1val);
        }
        // [iter190] n==1 のとき OSDSYS コールサイト(ra-4 周辺) と Trap functionエントリをダンプ
        if (s_n == 1) {
            const u32 ra = cpuRegs.GPR.r[31].UL[0]; // = 0x00158890
            Console.WriteLn("@@CALLSITE@@ ra=%08x | site[-C]=%08x [-8]=%08x [-4]=%08x [0]=%08x [+4]=%08x [+8]=%08x [+C]=%08x",
                ra,
                memRead32(ra - 0xCu), memRead32(ra - 0x8u), memRead32(ra - 0x4u),
                memRead32(ra),        memRead32(ra + 0x4u), memRead32(ra + 0x8u), memRead32(ra + 0xCu));
            Console.WriteLn("@@TRAPFN@@ epc=%08x | fn[-4]=%08x fn[0]=%08x fn[+4]=%08x fn[+8]=%08x",
                sc_pc,
                memRead32(sc_pc - 4u), memRead32(sc_pc), memRead32(sc_pc + 4u), memRead32(sc_pc + 8u));
            // [iter647] EELOAD ELF e_entry (0x82018) and a1 struct dump to diagnose why EPC=0x82000 not 0x82180
            // Removal condition: root causeafter identified
            const u32 a1_val = cpuRegs.GPR.r[5].UL[0];
            Console.WriteLn("@@MTC0_EPC_EXTRA@@ MEM[82000]=%08x [82018]=%08x a1=%08x [a1+0]=%08x [a1+4]=%08x [a1+8]=%08x [a1+C]=%08x [a1+10]=%08x [a1+14]=%08x",
                memRead32(0x82000u), memRead32(0x82018u),
                a1_val,
                memRead32(a1_val),      memRead32(a1_val + 4u),
                memRead32(a1_val + 8u), memRead32(a1_val + 0xCu),
                memRead32(a1_val + 0x10u), memRead32(a1_val + 0x14u));
            // [iter649] KSEG0 版addressで同じ場所を読む — TLB 未マップ vs EELOAD 未ロードの判別
            Console.WriteLn("@@MTC0_EPC_EXTRA2@@ KSEG0[80082000]=%08x [80082018]=%08x",
                memRead32(0x80082000u), memRead32(0x80082018u));
        }
    }
}

void recMTC0()
{


	if (GPR_IS_CONST1(_Rt_))
	{
		switch (_Rd_)
		{
			case 12:
				iFlushCall(FLUSH_INTERPRETER);
//				xMOV(eax, ptr32[&cpuRegs.cycle]);
//				xADD(eax, scaleblockcycles_clear());
//				xMOV(ptr32[&cpuRegs.cycle], eax); // update cycles
                armAdd(PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
//				xFastCall((void*)WriteCP0Status, g_cpuConstRegs[_Rt_].UL[0]);
                armAsm->Mov(EAX, g_cpuConstRegs[_Rt_].UL[0]);
                armEmitCall(reinterpret_cast<void*>(WriteCP0Status));
				break;

			case 16:
				iFlushCall(FLUSH_INTERPRETER);
//				xFastCall((void*)WriteCP0Config, g_cpuConstRegs[_Rt_].UL[0]);
                armAsm->Mov(EAX, g_cpuConstRegs[_Rt_].UL[0]);
                armEmitCall(reinterpret_cast<void*>(WriteCP0Config));
				break;

			case 9:
//				xMOV(ecx, ptr32[&cpuRegs.cycle]);
//				xADD(ecx, scaleblockcycles_clear());
//				xMOV(ptr32[&cpuRegs.cycle], ecx); // update cycles
                armAdd(ECX, PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
//				xMOV(ptr[&cpuRegs.lastCOP0Cycle], ecx);
                armStore(PTR_CPU(cpuRegs.lastCOP0Cycle), ECX);
//				xMOV(ptr32[&cpuRegs.CP0.r[9]], g_cpuConstRegs[_Rt_].UL[0]);
                armStore(PTR_CPU(cpuRegs.CP0.r[9]), g_cpuConstRegs[_Rt_].UL[0]);
				break;

			case 25:
				if (0 == (_Imm_ & 1)) // MTPS
				{
					if (0 != (_Imm_ & 0x3E)) // only effective when the register is 0
						break;
					// Updates PCRs and sets the PCCR.
					iFlushCall(FLUSH_INTERPRETER);
//					xMOV(eax, ptr32[&cpuRegs.cycle]);
//					xADD(eax, scaleblockcycles_clear());
//					xMOV(ptr32[&cpuRegs.cycle], eax); // update cycles
                    armAdd(PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
//					xFastCall((void*)COP0_UpdatePCCR);
                    armEmitCall(reinterpret_cast<void*>(COP0_UpdatePCCR));
//					xMOV(ptr32[&cpuRegs.PERF.n.pccr], g_cpuConstRegs[_Rt_].UL[0]);
                    armStore(PTR_CPU(cpuRegs.PERF.n.pccr), g_cpuConstRegs[_Rt_].UL[0]);
//					xFastCall((void*)COP0_DiagnosticPCCR);
                    armEmitCall(reinterpret_cast<void*>(COP0_DiagnosticPCCR));
				}
				else if (0 == (_Imm_ & 2)) // MTPC 0, only LSB of register matters
				{
//					xMOV(eax, ptr32[&cpuRegs.cycle]);
//					xADD(eax, scaleblockcycles_clear());
//					xMOV(ptr32[&cpuRegs.cycle], eax); // update cycles
                    armAdd(EAX, PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
//					xMOV(ptr32[&cpuRegs.PERF.n.pcr0], g_cpuConstRegs[_Rt_].UL[0]);
                    armStore(PTR_CPU(cpuRegs.PERF.n.pcr0), g_cpuConstRegs[_Rt_].UL[0]);
//					xMOV(ptr[&cpuRegs.lastPERFCycle[0]], eax);
                    armStore(PTR_CPU(cpuRegs.lastPERFCycle[0]), EAX);
				}
				else // MTPC 1
				{
//					xMOV(eax, ptr32[&cpuRegs.cycle]);
//					xADD(eax, scaleblockcycles_clear());
//					xMOV(ptr32[&cpuRegs.cycle], eax); // update cycles
                    armAdd(EAX, PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
//					xMOV(ptr32[&cpuRegs.PERF.n.pcr1], g_cpuConstRegs[_Rt_].UL[0]);
                    armStore(PTR_CPU(cpuRegs.PERF.n.pcr1), g_cpuConstRegs[_Rt_].UL[0]);
//					xMOV(ptr[&cpuRegs.lastPERFCycle[1]], eax);
                    armStore(PTR_CPU(cpuRegs.lastPERFCycle[1]), EAX);
				}
				break;

			case 24:
				COP0_LOG("MTC0 Breakpoint debug Registers code = %x\n", cpuRegs.code & 0x3FF);
				break;

			default:
//				xMOV(ptr32[&cpuRegs.CP0.r[_Rd_]], g_cpuConstRegs[_Rt_].UL[0]);
                armStore(PTR_CPU(cpuRegs.CP0.r[_Rd_]), g_cpuConstRegs[_Rt_].UL[0]);
                if (_Rd_ == 14) armEmitCall(reinterpret_cast<void*>(probe_mtc0_epc)); // [iter92]
				break;
		}
	}
	else
	{
		switch (_Rd_)
		{
			case 12:
				iFlushCall(FLUSH_INTERPRETER);
				_eeMoveGPRtoR(RAX, _Rt_, false); // [P29-2.1] flush後ロード; allow_preload=false でw0汚染回避
//				xMOV(eax, ptr32[&cpuRegs.cycle]);
//				xADD(eax, scaleblockcycles_clear());
//				xMOV(ptr32[&cpuRegs.cycle], eax); // update cycles
                armAdd(PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
//				xFastCall((void*)WriteCP0Status);
                armEmitCall(reinterpret_cast<void*>(WriteCP0Status));
				break;

			case 16:
				iFlushCall(FLUSH_INTERPRETER);
				_eeMoveGPRtoR(RAX, _Rt_, false); // [P29-2.1] flush後ロード; allow_preload=false でw0汚染回避
//				xFastCall((void*)WriteCP0Config);
                armEmitCall(reinterpret_cast<const void*>(WriteCP0Config));
				break;

			case 9:
//				xMOV(ecx, ptr32[&cpuRegs.cycle]);
//				xADD(ecx, scaleblockcycles_clear());
//				xMOV(ptr32[&cpuRegs.cycle], ecx); // update cycles
                armAdd(ECX, PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
				_eeMoveGPRtoM(PTR_CPU(cpuRegs.CP0.r[9]), _Rt_);
//				xMOV(ptr[&cpuRegs.lastCOP0Cycle], ecx);
                armStore(PTR_CPU(cpuRegs.lastCOP0Cycle), ECX);
				break;

			case 25:
				if (0 == (_Imm_ & 1)) // MTPS
				{
					if (0 != (_Imm_ & 0x3E)) // only effective when the register is 0
						break;
					iFlushCall(FLUSH_INTERPRETER);
//					xMOV(eax, ptr32[&cpuRegs.cycle]);
//					xADD(eax, scaleblockcycles_clear());
//					xMOV(ptr32[&cpuRegs.cycle], eax); // update cycles
                    armAdd(PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
//					xFastCall((void*)COP0_UpdatePCCR);
                    armEmitCall(reinterpret_cast<void*>(COP0_UpdatePCCR));
					_eeMoveGPRtoM(PTR_CPU(cpuRegs.PERF.n.pccr), _Rt_);
//					xFastCall((void*)COP0_DiagnosticPCCR);
                    armEmitCall(reinterpret_cast<void*>(COP0_DiagnosticPCCR));
				}
				else if (0 == (_Imm_ & 2)) // MTPC 0, only LSB of register matters
				{
//					xMOV(ecx, ptr32[&cpuRegs.cycle]);
//					xADD(ecx, scaleblockcycles_clear());
//					xMOV(ptr32[&cpuRegs.cycle], ecx); // update cycles
                    armAdd(ECX, PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
					_eeMoveGPRtoM(PTR_CPU(cpuRegs.PERF.n.pcr0), _Rt_);
//					xMOV(ptr[&cpuRegs.lastPERFCycle[0]], ecx);
                    armStore(PTR_CPU(cpuRegs.lastPERFCycle[0]), ECX);
				}
				else // MTPC 1
				{
//					xMOV(ecx, ptr32[&cpuRegs.cycle]);
//					xADD(ecx, scaleblockcycles_clear());
//					xMOV(ptr32[&cpuRegs.cycle], ecx); // update cycles
                    armAdd(ECX, PTR_CPU(cpuRegs.cycle), scaleblockcycles_clear());
					_eeMoveGPRtoM(PTR_CPU(cpuRegs.PERF.n.pcr1), _Rt_);
//					xMOV(ptr[&cpuRegs.lastPERFCycle[1]], ecx);
                    armStore(PTR_CPU(cpuRegs.lastPERFCycle[1]), ECX);
				}
				break;

			case 24:
				COP0_LOG("MTC0 Breakpoint debug Registers code = %x\n", cpuRegs.code & 0x3FF);
				break;

			case 14: // EPC
				// [iter192] _eeMoveGPRtoM は EEINST が Rt を dead とdetermineした場合、
				// キャッシュミスでmemoryの古い値を読む（ADDIU +4 が反映されない）。
				// _eeMoveGPRtoR で ARM64 registerキャッシュから確実に読み出す。
				// Removal condition: BIOS 画面displayafter confirmed
				_eeMoveGPRtoR(EAX, _Rt_);
				armStore(PTR_CPU(cpuRegs.CP0.r[14]), EAX);
				armEmitCall(reinterpret_cast<void*>(probe_mtc0_epc)); // [iter92]
				break;

			default:
				// [iter215] Use _eeMoveGPRtoR to read from host register cache,
				// not _eeMoveGPRtoM which can read stale memory (same bug as EPC iter192).
				_eeMoveGPRtoR(EAX, _Rt_);
				armStore(PTR_CPU(cpuRegs.CP0.r[_Rd_]), EAX);
				break;
		}
	}
}
#endif

/*void rec(COP0) {
}

void rec(BC0F) {
}

void rec(BC0T) {
}

void rec(BC0FL) {
}

void rec(BC0TL) {
}

void rec(TLBR) {
}

void rec(TLBWI) {
}

void rec(TLBWR) {
}

void rec(TLBP) {
}*/

} // namespace COP0
} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
