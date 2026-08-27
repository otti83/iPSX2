// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "iR3000A.h"
#include "R3000A.h"
#include "BaseblockEx.h"
#include "R5900OpcodeTables.h"
#include "IopBios.h"
#include "IopHw.h"
#include "Common.h"
#include "VMManager.h"

#include <time.h>
#include <set>
#include <string>
#include "common/Darwin/DarwinMisc.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <dlfcn.h>
#endif

#ifndef _WIN32_APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef _WIN32
#include <sys/types.h>
#endif

#include "iCore.h"

#include "Config.h"

#include "common/AlignedMalloc.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Perf.h"
#include "DebugTools/Breakpoints.h"

// #define DUMP_BLOCKS 1
// #define TRACE_BLOCKS 1

#ifdef DUMP_BLOCKS
#include "Zydis/Zydis.h"
#include "Zycore/Format.h"
#include "Zycore/Status.h"
#endif

#ifdef TRACE_BLOCKS
#include <zlib.h>
#endif

#if !defined(__ANDROID__)
using namespace x86Emitter;
#endif

extern void psxBREAK();

u32 g_psxMaxRecMem = 0;

alignas(16) uptr psxRecLUT[0x10000];
u32 psxhwLUT[0x10000];

static __fi u32 HWADDR(u32 mem) { return psxhwLUT[mem >> 16] + mem; }

static BASEBLOCK* recRAM = nullptr; // and the ptr to the blocks here
static BASEBLOCK* recROM = nullptr; // and here
static BASEBLOCK* recROM1 = nullptr; // also here
static BASEBLOCK* recROM2 = nullptr; // also here
static BASEBLOCK* recNULL = nullptr; // [iPSX2] Dummy page for unmapped regions
static BaseBlocks recBlocks;
static u8* recPtr = nullptr;
static u8* recPtrEnd = nullptr;
u32 psxpc; // recompiler psxpc
int psxbranch; // set for branch
u32 g_iopCyclePenalty;

static EEINST* s_pInstCache = nullptr;
static u32 s_nInstCacheSize = 0;

static BASEBLOCK* s_pCurBlock = nullptr;
static BASEBLOCKEX* s_pCurBlockEx = nullptr;

static u32 s_nEndBlock = 0; // what psxpc the current block ends
static u32 s_branchTo;
static bool s_nBlockFF;

static u32 s_saveConstRegs[32];
static u32 s_saveHasConstReg = 0, s_saveFlushedConstReg = 0;
static EEINST* s_psaveInstInfo = nullptr;

u32 s_psxBlockCycles = 0; // cycles of current block recompiling
static u32 s_savenBlockCycles = 0;
static bool s_recompilingDelaySlot = false;

static void iPsxBranchTest(u32 newpc, u32 cpuBranch);
//void psxRecompileNextInstruction(int delayslot);

extern void (*rpsxBSC[64])();
void rpsxpropBSC(EEINST* prev, EEINST* pinst);

static void iopClearRecLUT(BASEBLOCK* base, int count);

#define PSX_GETBLOCK(x) PC_GETBLOCK_(x, psxRecLUT)

#define PSXREC_CLEARM(mem) \
	(((mem) < g_psxMaxRecMem && (psxRecLUT[(mem) >> 16] + (mem))) ? \
			psxRecClearMem(mem) : \
            4)

#ifdef DUMP_BLOCKS
static ZydisFormatterFunc s_old_print_address;

static ZyanStatus ZydisFormatterPrintAddressAbsolute(const ZydisFormatter* formatter,
	ZydisFormatterBuffer* buffer, ZydisFormatterContext* context)
{
	ZyanU64 address;
	ZYAN_CHECK(ZydisCalcAbsoluteAddress(context->instruction, context->operand,
		context->runtime_address, &address));

	char buf[128];
	u32 len = 0;

#define A(x) ((u64)(x))

	if (address >= A(iopMem->Main) && address < A(iopMem->P))
	{
		len = snprintf(buf, sizeof(buf), "iopMem+0x%08X", static_cast<u32>(address - A(iopMem->Main)));
	}
	else if (address >= A(&psxRegs.GPR) && address < A(&psxRegs.CP0))
	{
		len = snprintf(buf, sizeof(buf), "psxRegs.GPR.%s", R3000A::disRNameGPR[static_cast<u32>(address - A(&psxRegs)) / 4u]);
	}
	else if (address == A(&psxRegs.pc))
	{
		len = snprintf(buf, sizeof(buf), "psxRegs.pc");
	}
	else if (address == A(&psxRegs.cycle))
	{
		len = snprintf(buf, sizeof(buf), "psxRegs.cycle");
	}
	else if (address == A(&g_nextEventCycle))
	{
		len = snprintf(buf, sizeof(buf), "g_nextEventCycle");
	}

#undef A

	if (len > 0)
	{
		ZYAN_CHECK(ZydisFormatterBufferAppend(buffer, ZYDIS_TOKEN_SYMBOL));
		ZyanString* string;
		ZYAN_CHECK(ZydisFormatterBufferGetString(buffer, &string));
		return ZyanStringAppendFormat(string, "&%s", buf);
	}

	return s_old_print_address(formatter, buffer, context);
}
#endif

// =====================================================================================================
//  Dynamically Compiled Dispatchers - R3000A style
// =====================================================================================================

static void iopRecRecompile(u32 startpc);

static const void* iopDispatcherEvent = nullptr;
static const void* iopDispatcherReg = nullptr;
static const void* iopJITCompile = nullptr;
static void* iopEnterRecompiledCode = nullptr;
static const void* iopExitRecompiledCode = nullptr;

static void recEventTest()
{
	_cpuEventTest_Shared();
}

// The address for all cleared blocks.  It recompiles the current pc and then
// dispatches to the recompiled block address.
static const void* _DynGen_JITCompile()
{
	pxAssertMsg(iopDispatcherReg != NULL, "Please compile the DispatcherReg subroutine *before* JITComple.  Thanks.");

//	u8* retval = xGetPtr();
    u8* retval = armGetCurrentCodePointer();

//	xBestCall((void*)iopRecRecompile, ptr32[&psxRegs.pc]);
    armLoad(EAX, PTR_CPU(psxRegs.pc));
    
    armEmitCall(reinterpret_cast<void*>(iopRecRecompile));

//	xMOV(eax, ptr[&psxRegs.pc]);
//	xMOV(ebx, eax);
//	xSHR(eax, 16);
//	xMOV(rcx, ptrNative[xComplexAddress(rcx, psxRecLUT, rax * wordsize)]);
//	xJMP(ptrNative[rbx * (wordsize / 4) + rcx]);

    armLoad(EAX, PTR_CPU(psxRegs.pc));

    armAsm->Lsr(ECX, EAX, 16);
    armAsm->Ldr(RCX, a64::MemOperand(RSTATE_x29, RCX, a64::LSL, 3));

    armAsm->Lsr(EAX, EAX, 2);
    armAsm->Ldr(RAX, a64::MemOperand(RCX, RAX, a64::LSL, 3));

    armAsm->Br(RAX);

	return retval;
}

// called when jumping to variable pc address
static const void* _DynGen_DispatcherReg()
{
//	u8* retval = xGetPtr();
    u8* retval = armGetCurrentCodePointer();

//	xMOV(eax, ptr[&psxRegs.pc]);
//	xMOV(ebx, eax);
//	xSHR(eax, 16);
//	xMOV(rcx, ptrNative[xComplexAddress(rcx, psxRecLUT, rax * wordsize)]);
//	xJMP(ptrNative[rbx * (wordsize / 4) + rcx]);

    armLoad(EAX, PTR_CPU(psxRegs.pc));

    armAsm->Lsr(ECX, EAX, 16);
    armAsm->Ldr(RCX, a64::MemOperand(RSTATE_x29, RCX, a64::LSL, 3));

    armAsm->Lsr(EAX, EAX, 2);
    armAsm->Ldr(RAX, a64::MemOperand(RCX, RAX, a64::LSL, 3));

    armAsm->Br(RAX);

	return retval;
}

// --------------------------------------------------------------------------------------
//  EnterRecompiledCode  - dynamic compilation stub!
// --------------------------------------------------------------------------------------
static const void* _DynGen_EnterRecompiledCode()
{
	// Optimization: The IOP never uses stack-based parameter invocation, so we can avoid
	// allocating any room on the stack for it (which is important since the IOP's entry
	// code gets invoked quite a lot).

//	u8* retval = xGetPtr();
    u8* retval = armGetCurrentCodePointer();

	{ // Properly scope the frame prologue/epilogue
#ifdef ENABLE_VTUNE
		xScopedStackFrame frame(true, true);
#else
//		xScopedStackFrame frame(false, true);
        armBeginStackFrame();
#endif
        // [P13] Initialize x27 (RSTATE_CPU) so IOP JIT can run even when EE is Interpreter.
        // Previously x27 was only set by EE JIT dispatcher, blocking Condition C (EE Int + IOP JIT).
        armMoveAddressToReg(RSTATE_CPU, &g_cpuRegistersPack);
        armMoveAddressToReg(RSTATE_x26, iopMem->Main);
        armMoveAddressToReg(RSTATE_x29, &psxRecLUT);
        
//		xJMP((void*)iopDispatcherReg);
        armEmitJmp(iopDispatcherReg);

		// Save an exit point
		iopExitRecompiledCode = armGetCurrentCodePointer();

        armEndStackFrame();
	}

//	xRET();
    armAsm->Ret();

	return retval;
}

static void _DynGen_Dispatchers()
{
//	const u8* start = xGetAlignedCallTarget();
    const u8* start = armGetCurrentCodePointer();

	// Place the EventTest and DispatcherReg stuff at the top, because they get called the
	// most and stand to benefit from strong alignment and direct referencing.
	iopDispatcherEvent = armGetCurrentCodePointer();
//	xFastCall((void*)recEventTest);
    armEmitCall(reinterpret_cast<void *>(recEventTest));
	iopDispatcherReg = _DynGen_DispatcherReg();

	iopJITCompile = _DynGen_JITCompile();
	iopEnterRecompiledCode = (void*)_DynGen_EnterRecompiledCode();

    if (DarwinMisc::iPSX2_CRASH_DIAG)
    {
        static bool s_logged_iop_dispatch_addrs = false;
        if (!s_logged_iop_dispatch_addrs)
        {
            s_logged_iop_dispatch_addrs = true;
            const u32* disp = reinterpret_cast<const u32*>(iopDispatcherReg);
            const u32* jitc = reinterpret_cast<const u32*>(iopJITCompile);
            const u32* entr = reinterpret_cast<const u32*>(iopEnterRecompiledCode);
            Console.WriteLn(
                "@@IOP_DISP_ADDR@@ event=%p reg=%p jitc=%p enter=%p exit=%p",
                iopDispatcherEvent, iopDispatcherReg, iopJITCompile, iopEnterRecompiledCode, iopExitRecompiledCode);
            Console.WriteLn(
                "@@IOP_DISP_WORDS@@ reg=%08x,%08x,%08x jitc=%08x,%08x,%08x enter=%08x,%08x,%08x",
                disp ? disp[0] : 0, disp ? disp[1] : 0, disp ? disp[2] : 0,
                jitc ? jitc[0] : 0, jitc ? jitc[1] : 0, jitc ? jitc[2] : 0,
                entr ? entr[0] : 0, entr ? entr[1] : 0, entr ? entr[2] : 0);
        }
    }

	recBlocks.SetJITCompile(iopJITCompile);

	Perf::any.Register(start, armGetCurrentCodePointer() - start, "IOP Dispatcher");
}

////////////////////////////////////////////////////
using namespace R3000A;

void _psxFlushConstReg(int reg)
{
	if (PSX_IS_CONST1(reg) && !(g_psxFlushedConstReg & (1 << reg)))
	{
//		xMOV(ptr32[&psxRegs.GPR.r[reg]], g_psxConstRegs[reg]);
        armStore(PTR_CPU(psxRegs.GPR.r[reg]), g_psxConstRegs[reg]);
		g_psxFlushedConstReg |= (1 << reg);
	}
}

void _psxFlushConstRegs()
{
	// TODO: Combine flushes

	int i;

	// flush constants

	// ignore r0
	for (i = 1; i < 32; ++i)
	{
		if (g_psxHasConstReg & (1 << i))
		{

			if (!(g_psxFlushedConstReg & (1 << i)))
			{
//				xMOV(ptr32[&psxRegs.GPR.r[i]], g_psxConstRegs[i]);
                armStore(PTR_CPU(psxRegs.GPR.r[i]), g_psxConstRegs[i]);
				g_psxFlushedConstReg |= 1 << i;
			}

			if (g_psxHasConstReg == g_psxFlushedConstReg)
				break;
		}
	}
}

void _psxDeleteReg(int reg, int flush)
{
	if (!reg)
		return;
	if (flush && PSX_IS_CONST1(reg))
		_psxFlushConstReg(reg);

	PSX_DEL_CONST(reg);
	_deletePSXtoX86reg(reg, flush ? DELETE_REG_FREE : DELETE_REG_FREE_NO_WRITEBACK);
}

void _psxMoveGPRtoR(const a64::Register& to, int fromgpr)
{
	if (PSX_IS_CONST1(fromgpr))
	{
//		xMOV(to, g_psxConstRegs[fromgpr]);
        armAsm->Mov(to, g_psxConstRegs[fromgpr]);
	}
	else
	{
		const int reg = EEINST_USEDTEST(fromgpr) ? _allocX86reg(X86TYPE_PSX, fromgpr, MODE_READ) : _checkX86reg(X86TYPE_PSX, fromgpr, MODE_READ);
		if (reg >= 0) {
//            xMOV(to, xRegister32(reg));
            armAsm->Mov(to, HostW(reg)); // [iter140] slot→phys via HostW (same WRegister(slot) bug as iter87/136/137)
        }
		else {
//            xMOV(to, ptr[&psxRegs.GPR.r[fromgpr]]);
            armLoad(to, PTR_CPU(psxRegs.GPR.r[fromgpr]));
        }
	}
}

void _psxMoveGPRtoM(const a64::MemOperand& to, int fromgpr)
{
	if (PSX_IS_CONST1(fromgpr))
	{
//		xMOV(ptr32[(u32*)(to)], g_psxConstRegs[fromgpr]);
        armStorePtr(g_psxConstRegs[fromgpr], to);
	}
	else
	{
		const int reg = EEINST_USEDTEST(fromgpr) ? _allocX86reg(X86TYPE_PSX, fromgpr, MODE_READ) : _checkX86reg(X86TYPE_PSX, fromgpr, MODE_READ);
		if (reg >= 0)
		{
//			xMOV(ptr32[(u32*)(to)], xRegister32(reg));
            armAsm->Str(HostW(reg), to);  // [iter242] slot→phys fix
		}
		else
		{
//			xMOV(eax, ptr[&psxRegs.GPR.r[fromgpr]]);
            armLoad(EAX, PTR_CPU(psxRegs.GPR.r[fromgpr]));
//			xMOV(ptr32[(u32*)(to)], eax);
            armAsm->Str(EAX, to);
		}
	}
}

void _psxFlushCall(int flushtype)
{
	// Free registers that are not saved across function calls (x86-32 ABI):
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (!x86regs[i].inuse)
			continue;

		if (armIsCallerSaved(i) ||
			((flushtype & FLUSH_FREE_NONTEMP_X86) && x86regs[i].type != X86TYPE_TEMP) ||
			((flushtype & FLUSH_FREE_TEMP_X86) && x86regs[i].type == X86TYPE_TEMP))
		{
			_freeX86reg(i);
		}
	}

	if (flushtype & FLUSH_ALL_X86)
		_flushX86regs();

	if (flushtype & FLUSH_CONSTANT_REGS)
		_psxFlushConstRegs();

	if ((flushtype & FLUSH_PC) /*&& !g_cpuFlushedPC*/)
	{
//		xMOV(ptr32[&psxRegs.pc], psxpc);
        armStore(PTR_CPU(psxRegs.pc), psxpc);
		//g_cpuFlushedPC = true;
	}
}

void _psxFlushAllDirty()
{
	// TODO: Combine flushes
	for (u32 i = 0; i < 32; ++i)
	{
		if (PSX_IS_CONST1(i))
			_psxFlushConstReg(i);
	}

	_flushX86regs();
}

void psxSaveBranchState()
{
	s_savenBlockCycles = s_psxBlockCycles;
	memcpy(s_saveConstRegs, g_psxConstRegs, sizeof(g_psxConstRegs));
	s_saveHasConstReg = g_psxHasConstReg;
	s_saveFlushedConstReg = g_psxFlushedConstReg;
	s_psaveInstInfo = g_pCurInstInfo;

	// save all regs
	memcpy(s_saveX86regs, x86regs, sizeof(x86regs));
}

void psxLoadBranchState()
{
	s_psxBlockCycles = s_savenBlockCycles;

	memcpy(g_psxConstRegs, s_saveConstRegs, sizeof(g_psxConstRegs));
	g_psxHasConstReg = s_saveHasConstReg;
	g_psxFlushedConstReg = s_saveFlushedConstReg;
	g_pCurInstInfo = s_psaveInstInfo;

	// restore all regs
	memcpy(x86regs, s_saveX86regs, sizeof(x86regs));
}

////////////////////
// Code Templates //
////////////////////

void _psxOnWriteReg(int reg)
{
	PSX_DEL_CONST(reg);
}

bool psxTrySwapDelaySlot(u32 rs, u32 rt, u32 rd)
{
#if 1
	if (s_recompilingDelaySlot)
		return false;

	const u32 opcode_encoded = iopMemRead32(psxpc);
	if (opcode_encoded == 0)
	{
		psxRecompileNextInstruction(true, true);
		return true;
	}

	const u32 opcode_rs = ((opcode_encoded >> 21) & 0x1F);
	const u32 opcode_rt = ((opcode_encoded >> 16) & 0x1F);
	const u32 opcode_rd = ((opcode_encoded >> 11) & 0x1F);

	switch (opcode_encoded >> 26)
	{
		case 8: // ADDI
		case 9: // ADDIU
		case 10: // SLTI
		case 11: // SLTIU
		case 12: // ANDIU
		case 13: // ORI
		case 14: // XORI
		case 15: // LUI
		case 32: // LB
		case 33: // LH
		case 34: // LWL
		case 35: // LW
		case 36: // LBU
		case 37: // LHU
		case 38: // LWR
		case 39: // LWU
		case 40: // SB
		case 41: // SH
		case 42: // SWL
		case 43: // SW
		case 46: // SWR
		{
			if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
				goto is_unsafe;
		}
		break;

		case 50: // LWC2
		case 58: // SWC2
			break;

		case 0: // SPECIAL
		{
			switch (opcode_encoded & 0x3F)
			{
				case 0: // SLL
				case 2: // SRL
				case 3: // SRA
				case 4: // SLLV
				case 6: // SRLV
				case 7: // SRAV
				case 32: // ADD
				case 33: // ADDU
				case 34: // SUB
				case 35: // SUBU
				case 36: // AND
				case 37: // OR
				case 38: // XOR
				case 39: // NOR
				case 42: // SLT
				case 43: // SLTU
				{
					if ((rs != 0 && rs == opcode_rd) || (rt != 0 && rt == opcode_rd) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
						goto is_unsafe;
				}
				break;

				case 15: // SYNC
				case 24: // MULT
				case 25: // MULTU
				case 26: // DIV
				case 27: // DIVU
					break;

				default:
					goto is_unsafe;
			}
		}
		break;

		case 16: // COP0
		case 17: // COP1
		case 18: // COP2
		case 19: // COP3
		{
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 0: // MFC0
				case 2: // CFC0
				{
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						goto is_unsafe;
				}
				break;

				case 4: // MTC0
				case 6: // CTC0
					break;

				default:
				{
					// swap when it's GTE
					if ((opcode_encoded >> 26) != 18)
						goto is_unsafe;
				}
				break;
			}
			break;
		}
		break;

		default:
			goto is_unsafe;
	}

	RALOG("Swapping delay slot %08X %s\n", psxpc, disR3000AF(iopMemRead32(psxpc), psxpc));
	psxRecompileNextInstruction(true, true);
	return true;

is_unsafe:
	RALOG("NOT SWAPPING delay slot %08X %s\n", psxpc, disR3000AF(iopMemRead32(psxpc), psxpc));
	return false;
#else
	return false;
#endif
}

int psxTryRenameReg(int to, int from, int fromx86, int other, int xmminfo)
{
	// can't rename when in form Rd = Rs op Rt and Rd == Rs or Rd == Rt
	if ((xmminfo & XMMINFO_NORENAME) || fromx86 < 0 || to == from || to == other || !EEINST_RENAMETEST(from))
		return -1;

	RALOG("Renaming %s to %s\n", R3000A::disRNameGPR[from], R3000A::disRNameGPR[to]);

	// flush back when it's been modified
	if (x86regs[fromx86].mode & MODE_WRITE && EEINST_LIVETEST(from))
		_writebackX86Reg(fromx86);

	// remove all references to renamed-to register
	_deletePSXtoX86reg(to, DELETE_REG_FREE_NO_WRITEBACK);
	PSX_DEL_CONST(to);

	// and do the actual rename, new register has been modified.
	x86regs[fromx86].reg = to;
	x86regs[fromx86].mode |= MODE_READ | MODE_WRITE;
	return fromx86;
}

// rd = rs op rt
void psxRecompileCodeConst0(R3000AFNPTR constcode, R3000AFNPTR_INFO constscode, R3000AFNPTR_INFO consttcode, R3000AFNPTR_INFO noconstcode, int xmminfo)
{
	static int s_const0_probe_count = 0;
	if (!_Rd_)
		return;

	// TEMP: force runtime compare/ALU paths for the known BIOS poll loop branch PCs
	// to verify whether constant-propagation is causing a JIT miscompile.
	const bool antigravity_force_noconst_loop =
		(psxpc == 0x9fc41098u || psxpc == 0x9fc410a0u);
	if ((psxpc >= 0x9fc41090u && psxpc <= 0x9fc410a4u) && s_const0_probe_count < 40)
	{
		++s_const0_probe_count;
		Console.WriteLn("@@CONST0_PROBE@@ n=%d psxpc=%08x code=%08x rs=%u rt=%u rd=%u c2=%u c1s=%u c1t=%u force=%u",
			s_const0_probe_count, psxpc, psxRegs.code, _Rs_, _Rt_, _Rd_, PSX_IS_CONST2(_Rs_, _Rt_) ? 1u : 0u,
			PSX_IS_CONST1(_Rs_) ? 1u : 0u, PSX_IS_CONST1(_Rt_) ? 1u : 0u, antigravity_force_noconst_loop ? 1u : 0u);
	}

	// [iter118] @@SCAN_ADDU_COMPILE@@ — compile-time log for ADDU t0,r0,r0 at bfc02640
	// psxpc = bfc02644 when compiling the instruction AT bfc02640
	if (psxpc == 0xBFC02644u)
		Console.WriteLn("@@SCAN_ADDU_COMPILE@@ psxpc=%08x rd=%u rs=%u rt=%u c2=%u c1s=%u c1t=%u hasConst=%08x flushed=%08x t0val=%08x",
			psxpc, _Rd_, _Rs_, _Rt_,
			PSX_IS_CONST2(_Rs_, _Rt_) ? 1u : 0u,
			PSX_IS_CONST1(_Rs_) ? 1u : 0u,
			PSX_IS_CONST1(_Rt_) ? 1u : 0u,
			g_psxHasConstReg, g_psxFlushedConstReg, g_psxConstRegs[8]);

	if (!antigravity_force_noconst_loop && PSX_IS_CONST2(_Rs_, _Rt_))
	{
		_deletePSXtoX86reg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);
		PSX_SET_CONST(_Rd_);
		constcode();
		return;
	}

	// we have to put these up here, because the register allocator below will wipe out const flags
	// for the destination register when/if it switches it to write mode.
	const bool s_is_const = PSX_IS_CONST1(_Rs_);
	const bool t_is_const = PSX_IS_CONST1(_Rt_);
	const bool d_is_const = PSX_IS_CONST1(_Rd_);
	const bool s_is_used = EEINST_USEDTEST(_Rs_);
	const bool t_is_used = EEINST_USEDTEST(_Rt_);

	if (!s_is_const)
		_addNeededGPRtoX86reg(_Rs_);
	if (!t_is_const)
		_addNeededGPRtoX86reg(_Rt_);
	if (!d_is_const)
		_addNeededGPRtoX86reg(_Rd_);

	u64 info = 0;
	int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs < 0 && ((!s_is_const && s_is_used) || _Rs_ == _Rd_))
		regs = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		info |= PROCESS_EE_SET_S(regs);

	int regt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regt < 0 && ((!t_is_const && t_is_used) || _Rt_ == _Rd_))
		regt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	// If S is no longer live, swap D for S. Saves the move.
	int regd = psxTryRenameReg(_Rd_, _Rs_, regs, _Rt_, xmminfo);
	if (regd < 0)
	{
		// TODO: If not live, write direct to memory.
		regd = _allocX86reg(X86TYPE_PSX, _Rd_, MODE_WRITE);
	}
	if (regd >= 0)
		info |= PROCESS_EE_SET_D(regd);

	_validateRegs();

	if (!antigravity_force_noconst_loop && s_is_const && regs < 0)
	{
		// This *must* go inside the if, because of when _Rs_ =  _Rd_
		PSX_DEL_CONST(_Rd_);
		constscode(info /*| PROCESS_CONSTS*/);
		return;
	}

	if (!antigravity_force_noconst_loop && t_is_const && regt < 0)
	{
		PSX_DEL_CONST(_Rd_);
		consttcode(info /*| PROCESS_CONSTT*/);
		return;
	}

	PSX_DEL_CONST(_Rd_);
	noconstcode(info);
}

static void psxRecompileIrxImport()
{
	u32 import_table = irxImportTableAddr(psxpc - 4);
	u16 index = psxRegs.code & 0xffff;
	if (!import_table)
		return;

	const std::string libname = iopMemReadString(import_table + 12, 8);

	irxHLE hle = irxImportHLE(libname, index);
#ifdef PCSX2_DEVBUILD
	const irxDEBUG debug = irxImportDebug(libname, index);
	const char* funcname = irxImportFuncname(libname, index);
#else
	const irxDEBUG debug = 0;
	const char* funcname = nullptr;
#endif

	if (!hle && !debug && (!TraceActive(IOP.Bios) || !funcname))
		return;

//	xMOV(ptr32[&psxRegs.code], psxRegs.code);
    armStore(PTR_CPU(psxRegs.code), psxRegs.code);
//	xMOV(ptr32[&psxRegs.pc], psxpc);
    armStore(PTR_CPU(psxRegs.pc), psxpc);
	_psxFlushCall(FLUSH_NODESTROY);

	if (TraceActive(IOP.Bios))
	{
//		xMOV64(arg3reg, (uptr)funcname);
        armAsm->Mov(RDX, (uptr)funcname);

//		xFastCall((void*)irxImportLog_rec, import_table, index);
        armAsm->Mov(EAX, import_table);
        armAsm->Mov(ECX, index);
        armEmitCall(reinterpret_cast<const void*>(irxImportLog_rec));
	}

	if (debug) {
//        xFastCall((void *) debug);
        armEmitCall(reinterpret_cast<const void*>(debug));
    }

	if (hle)
	{
//		xFastCall((void*)hle);
        armEmitCall(reinterpret_cast<const void*>(hle));
//		xTEST(eax, eax);
//		xJNZ(iopDispatcherReg);
        armEmitCbnz(EAX, iopDispatcherReg);
	}

}

// rt = rs op imm16
void psxRecompileCodeConst1(R3000AFNPTR constcode, R3000AFNPTR_INFO noconstcode, int xmminfo)
{
	static int s_const1_probe_count = 0;
	if (!_Rt_)
	{
		// check for iop module import table magic
		if (psxRegs.code >> 16 == 0x2400) {
			// [iter230] TEMP_DIAG: sifman/sifcmd 特化 IRX magic トレース
			{
				u32 imp_tbl = irxImportTableAddr(psxpc - 4);
				if (imp_tbl) {
					std::string ln = iopMemReadString(imp_tbl + 12, 8);
					static u32 s_irx_sif_n = 0;
					if (ln == "sifman\0\0" || ln == "sifcmd\0\0" ||
					    ln.find("sif") != std::string::npos ||
					    ln.find("SIF") != std::string::npos) {
						if (s_irx_sif_n++ < 20)
							Console.WriteLn("@@IRX_SIF@@ n=%u psxpc=%08x lib=%s idx=%u",
								s_irx_sif_n, psxpc, ln.c_str(), (unsigned)(psxRegs.code & 0xffff));
					}
					// Also log all unique module names (first occurrence only)
					static std::set<std::string> s_seen_mods;
					if (s_seen_mods.find(ln) == s_seen_mods.end()) {
						s_seen_mods.insert(ln);
						// [iter242-diag] dump IOP RAM 0x179A8 at each module load
						u32 iop_179a8 = iopMemRead32(0x179A8);
						Console.WriteLn("@@IRX_MOD@@ psxpc=%08x lib=%s (total_mods=%u) iop[179a8]=%08x",
							psxpc, ln.c_str(), (unsigned)s_seen_mods.size(), iop_179a8);
					}
				}
			}
			psxRecompileIrxImport();
		}
		return;
	}

	// TEMP: force runtime path for the known BIOS poll loop SLTI PC to verify
	// whether incorrect constant-propagation is freezing v0.
	// [iter120] bfc02728 = psxpc when compiling ADDIU t2,t2,16 at bfc02724 (delay slot of BNE at bfc02720).
	// t2 is const-propagated (hasConst bit10 SET from ADDU t2,r0,r0 at bfc0264c).
	// Pure constscode emits NO runtime code → psxRegs.GPR.r[10] never updates.
	// BNE v0,t2 at bfc026a4 always fails (stale t2≠10048) → Phase 2 never starts → scan returns v0=0.
	// [iter125] bfc02714 = ADDIU t1,t1,16 (advance block start). t1 const-prop from prior
	// iteration causes stale slot value; force runtime armLoad so psxRegs.r[9] (correctly
	// flushed from ADDIU t1,t0,12 at bfc02660) is used instead.
	// [iter126] bfc4aac0 = ADDIU $t8,$t8,-0x4ed0 (function dispatch table base setup).
	// LUI $t8,0xbfc5 at bfc4aabc const-folds but emits no runtime code; psxRegs.r[24] stays
	// stale in later blocks. Force flush of LUI+ADDIU computed value to psxRegs here.
	const bool antigravity_force_noconst_loop = (psxpc == 0x9fc4109cu) || (psxpc == 0xBFC02728u) || (psxpc == 0xBFC02714u) || (psxpc == 0xBFC4AAC0u);
	if ((psxpc >= 0x9fc41090u && psxpc <= 0x9fc410a4u) && s_const1_probe_count < 40)
	{
		++s_const1_probe_count;
		Console.WriteLn("@@CONST1_PROBE@@ n=%d psxpc=%08x code=%08x rs=%u rt=%u c1s=%u force=%u",
			s_const1_probe_count, psxpc, psxRegs.code, _Rs_, _Rt_, PSX_IS_CONST1(_Rs_) ? 1u : 0u,
			antigravity_force_noconst_loop ? 1u : 0u);
	}

	if (!antigravity_force_noconst_loop && PSX_IS_CONST1(_Rs_))
	{
		_deletePSXtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
		PSX_SET_CONST(_Rt_);
		constcode();
		return;
	}

	// [iter126] force-noconst + const source (e.g. LUI→ADDIU in same block):
	// The source reg was const-folded by a prior instruction (e.g. LUI) in this block,
	// so psxRegs.r[_Rs_] is stale. Compute the result from g_psxConstRegs and emit an
	// explicit armStore to psxRegs, so cross-block armLoad of _Rt_ is correct.
	if (antigravity_force_noconst_loop && PSX_IS_CONST1(_Rs_) && _Rt_ != 0)
	{
		_deletePSXtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
		const u32 result = g_psxConstRegs[_Rs_] + static_cast<u32>(_Imm_);
		g_psxConstRegs[_Rt_] = result;
		PSX_SET_CONST(_Rt_);
		armAsm->Mov(a64::w10, result);
		armStore(PTR_CPU(psxRegs.GPR.r[_Rt_]), a64::w10);
		return;
	}

	_addNeededPSXtoX86reg(_Rs_);
	_addNeededPSXtoX86reg(_Rt_);

	u64 info = 0;

	const bool s_is_used = EEINST_USEDTEST(_Rs_);
	const int regs = s_is_used ? _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ) : _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		info |= PROCESS_EE_SET_S(regs);

	int regt = psxTryRenameReg(_Rt_, _Rs_, regs, 0, xmminfo);
	if (regt < 0)
	{
		regt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_WRITE);
	}
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	_validateRegs();

	PSX_DEL_CONST(_Rt_);
	noconstcode(info);
}

// rd = rt op sa
void psxRecompileCodeConst2(R3000AFNPTR constcode, R3000AFNPTR_INFO noconstcode, int xmminfo)
{
	if (!_Rd_)
		return;

	if (PSX_IS_CONST1(_Rt_))
	{
		_deletePSXtoX86reg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);
		PSX_SET_CONST(_Rd_);
		constcode();
		return;
	}

	_addNeededPSXtoX86reg(_Rt_);
	_addNeededPSXtoX86reg(_Rd_);

	u64 info = 0;
	const bool s_is_used = EEINST_USEDTEST(_Rt_);
	const int regt = s_is_used ? _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ) : _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	int regd = psxTryRenameReg(_Rd_, _Rt_, regt, 0, xmminfo);
	if (regd < 0)
	{
		regd = _allocX86reg(X86TYPE_PSX, _Rd_, MODE_WRITE);
	}
	if (regd >= 0)
		info |= PROCESS_EE_SET_D(regd);

	_validateRegs();

	PSX_DEL_CONST(_Rd_);
	noconstcode(info);
}

// rd = rt MULT rs  (SPECIAL)
void psxRecompileCodeConst3(R3000AFNPTR constcode, R3000AFNPTR_INFO constscode, R3000AFNPTR_INFO consttcode, R3000AFNPTR_INFO noconstcode, int LOHI)
{
	if (PSX_IS_CONST2(_Rs_, _Rt_))
	{
		if (LOHI)
		{
			_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);
			_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
		}

		constcode();
		return;
	}

	// we have to put these up here, because the register allocator below will wipe out const flags
	// for the destination register when/if it switches it to write mode.
	const bool s_is_const = PSX_IS_CONST1(_Rs_);
	const bool t_is_const = PSX_IS_CONST1(_Rt_);
	const bool s_is_used = EEINST_USEDTEST(_Rs_);
	const bool t_is_used = EEINST_USEDTEST(_Rt_);

	if (!s_is_const)
		_addNeededGPRtoX86reg(_Rs_);
	if (!t_is_const)
		_addNeededGPRtoX86reg(_Rt_);
	if (LOHI)
	{
		if (EEINST_LIVETEST(PSX_LO))
			_addNeededPSXtoX86reg(PSX_LO);
		if (EEINST_LIVETEST(PSX_HI))
			_addNeededPSXtoX86reg(PSX_HI);
	}

	u64 info = 0;
	int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs < 0 && !s_is_const && s_is_used)
		regs = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		info |= PROCESS_EE_SET_S(regs);

	// need at least one in a register
	int regt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regs < 0 || (regt < 0 && !t_is_const && t_is_used))
		regt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	if (LOHI)
	{
		// going to destroy lo/hi, so invalidate if we're writing it back to state
		const bool lo_is_used = EEINST_USEDTEST(PSX_LO);
		const int reglo = lo_is_used ? _allocX86reg(X86TYPE_PSX, PSX_LO, MODE_WRITE) : -1;
		if (reglo >= 0)
			info |= PROCESS_EE_SET_LO(reglo) | PROCESS_EE_LO;
		else
			_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

		const bool hi_is_live = EEINST_USEDTEST(PSX_HI);
		const int reghi = hi_is_live ? _allocX86reg(X86TYPE_PSX, PSX_HI, MODE_WRITE) : -1;
		if (reghi >= 0)
			info |= PROCESS_EE_SET_HI(reghi) | PROCESS_EE_HI;
		else
			_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	}

	_validateRegs();

	if (s_is_const && regs < 0)
	{
		// This *must* go inside the if, because of when _Rs_ =  _Rd_
		constscode(info /*| PROCESS_CONSTS*/);
		return;
	}

	if (t_is_const && regt < 0)
	{
		consttcode(info /*| PROCESS_CONSTT*/);
		return;
	}

	noconstcode(info);
}

static u8* m_recBlockAlloc = NULL;

static const uint m_recBlockAllocSize =
	(((Ps2MemSize::IopRam + Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2 + 0x10000) / 4) * sizeof(BASEBLOCK));

static void recReserve()
{
	recPtr = SysMemory::GetIOPRec();
	recPtrEnd = SysMemory::GetIOPRecEnd() - _64kb;

	// Goal: Allocate BASEBLOCKs for every possible branch target in IOP memory.
	// Any 4-byte aligned address makes a valid branch target as per MIPS design (all instructions are
	// always 4 bytes long).

	if (!m_recBlockAlloc)
	{
		// We're on 64-bit, if these memory allocations fail, we're in real trouble.
		m_recBlockAlloc = (u8*)_aligned_malloc(m_recBlockAllocSize, 4096);
		if (!m_recBlockAlloc)
			pxFailRel("Failed to allocate R3000A BASEBLOCK lookup tables");
	}

	u8* curpos = m_recBlockAlloc;
	recRAM = (BASEBLOCK*)curpos;
	curpos += (Ps2MemSize::IopRam / 4) * sizeof(BASEBLOCK);
	recROM = (BASEBLOCK*)curpos;
	curpos += (Ps2MemSize::Rom / 4) * sizeof(BASEBLOCK);
	recROM1 = (BASEBLOCK*)curpos;
	curpos += (Ps2MemSize::Rom1 / 4) * sizeof(BASEBLOCK);
	recROM2 = (BASEBLOCK*)curpos;
	curpos += (Ps2MemSize::Rom2 / 4) * sizeof(BASEBLOCK);
    // [iPSX2] Allocate recNULL
    recNULL = (BASEBLOCK*)curpos;
    curpos += (0x10000 / 4) * sizeof(BASEBLOCK);

	pxAssertRel(!s_pInstCache, "InstCache not allocated");
	s_nInstCacheSize = 128;
	s_pInstCache = (EEINST*)malloc(sizeof(EEINST) * s_nInstCacheSize);
	if (!s_pInstCache)
		pxFailRel("Failed to allocate R3000 InstCache array.");
}

void recResetIOP()
{
	DevCon.WriteLn("iR3000A Recompiler reset.");

//	xSetPtr(SysMemory::GetIOPRec());
    armSetAsmPtr(SysMemory::GetIOPRec(), _4kb, nullptr);
    armStartBlock();

	_DynGen_Dispatchers();

//	recPtr = xGetPtr();
    recPtr = armEndBlock();

	iopClearRecLUT((BASEBLOCK*)m_recBlockAlloc,
		(((Ps2MemSize::IopRam + Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2) / 4)));

    // [iPSX2] Initialize recNULL with iopJITCompile (Trampoline/Stub)
    // iopRecRecompile is a C++ function and cannot be jumped to directly by JIT.
    // iopJITCompile is the trampoline that handles context saving and calling the compiler.
    if (DarwinMisc::iPSX2_CRASH_DIAG) {
        std::fprintf(stderr, "@@RECNULL_INIT@@ recnull_entry=%p in_jit=%d kind=TRAMPOLINE\n", iopJITCompile, 1); // 1 = presumed safe trampoline
    }

    for (int j = 0; j < (0x10000 / 4); j++) {
        recNULL[j].SetFnptr((uptr)iopJITCompile);
    }

	for (int i = 0; i < 0x10000; i++) {
        // [iPSX2] Point unmapped pages to recNULL.
        // We modify the mapbase pointer so that recNULL + (0-i)<<14 becomes recNULL.
        // mapbase = recNULL + (i << 14) blocks.
		recLUT_SetPage(psxRecLUT, 0, recNULL + (i << 14), 0, i, 0);
    }

	// IOP knows 64k pages, hence for the 0x10000's

	// The bottom 2 bits of PC are always zero, so we <<14 to "compress"
	// the pc indexer into it's lower common denominator.

	// We're only mapping 20 pages here in 4 places.
	// 0x80 comes from : (Ps2MemSize::IopRam / 0x10000) * 4

	for (int i = 0; i < 0x80; i++)
	{
		recLUT_SetPage(psxRecLUT, psxhwLUT, recRAM, 0x0000, i, i & 0x1f);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recRAM, 0x8000, i, i & 0x1f);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recRAM, 0xa000, i, i & 0x1f);
	}

	for (int i = 0x1fc0; i < 0x2000; i++)
	{
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM, 0x0000, i, i - 0x1fc0);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM, 0x8000, i, i - 0x1fc0);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM, 0xa000, i, i - 0x1fc0);
	}
    


	for (int i = 0x1e00; i < 0x1e40; i++)
	{
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM1, 0x0000, i, i - 0x1e00);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM1, 0x8000, i, i - 0x1e00);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM1, 0xa000, i, i - 0x1e00);
	}

	for (int i = 0x1e40; i < 0x1e48; i++)
	{
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM2, 0x0000, i, i - 0x1e40);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM2, 0x8000, i, i - 0x1e40);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM2, 0xa000, i, i - 0x1e40);
	}

	if (s_pInstCache)
		memset(s_pInstCache, 0, sizeof(EEINST) * s_nInstCacheSize);

	recBlocks.Reset();
	g_psxMaxRecMem = 0;

	psxbranch = 0;
}

static void recShutdown()
{
	safe_aligned_free(m_recBlockAlloc);

	safe_free(s_pInstCache);
	s_nInstCacheSize = 0;

	recPtr = nullptr;
	recPtrEnd = nullptr;
}

static void iopClearRecLUT(BASEBLOCK* base, int count)
{
	for (int i = 0; i < count; i++)
		base[i].SetFnptr((uptr)iopJITCompile);
}

static __noinline s32 recExecuteBlock(s32 eeCycles)
{
	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = eeCycles;

#ifdef PCSX2_DEVBUILD
	//if (SysTrace.SIF.IsActive())
	//	SysTrace.IOP.R3000A.Write("Switching to IOP CPU for %d cycles", eeCycles);
#endif

	// [TODO] recExecuteBlock could be replaced by a direct call to the iopEnterRecompiledCode()
	//   (by assigning its address to the psxRec structure).  But for that to happen, we need
	//   to move iopBreak/iopCycleEE update code to emitted assembly code. >_<  --air

	// Likely Disasm, as borrowed from MSVC:

	// Entry:
	// 	mov         eax,dword ptr [esp+4]
	// 	mov         dword ptr [iopBreak (0E88DCCh)],0
	// 	mov         dword ptr [iopCycleEE (832A84h)],eax

	// Exit:
	// 	mov         ecx,dword ptr [iopBreak (0E88DCCh)]
	// 	mov         edx,dword ptr [iopCycleEE (832A84h)]
	// 	lea         eax,[edx+ecx]

	// [P49] Legacy lazy toggle: flip RW→RX before entering JIT code
	DarwinMisc::LegacyEnsureExecutable();

	((void (*)())iopEnterRecompiledCode)();

	return psxRegs.iopBreak + psxRegs.iopCycleEE;
}

// Returns the offset to the next instruction after any cleared memory
static __fi u32 psxRecClearMem(u32 pc)
{
	BASEBLOCK* pblock;

	pblock = PSX_GETBLOCK(pc);
	// if ((u8*)iopJITCompile == pblock->GetFnptr())
	if (pblock->GetFnptr() == (uptr)iopJITCompile)
		return 4;

	pc = HWADDR(pc);

	u32 lowerextent = pc, upperextent = pc + 4;
	int blockidx = recBlocks.Index(pc);
	pxAssert(blockidx != -1);

	while (BASEBLOCKEX* pexblock = recBlocks[blockidx - 1])
	{
		if (pexblock->startpc + pexblock->size * 4 <= lowerextent)
			break;

		lowerextent = std::min(lowerextent, pexblock->startpc);
		blockidx--;
	}

	int toRemoveFirst = blockidx;

	while (BASEBLOCKEX* pexblock = recBlocks[blockidx])
	{
		if (pexblock->startpc >= upperextent)
			break;

		lowerextent = std::min(lowerextent, pexblock->startpc);
		upperextent = std::max(upperextent, pexblock->startpc + pexblock->size * 4);

		blockidx++;
	}

	if (toRemoveFirst != blockidx)
	{
		recBlocks.Remove(toRemoveFirst, (blockidx - 1));
	}

	blockidx = 0;
	while (BASEBLOCKEX* pexblock = recBlocks[blockidx++])
	{
		if (pc >= pexblock->startpc && pc < pexblock->startpc + pexblock->size * 4) [[unlikely]]
		{
			DevCon.Error("[IOP] Impossible block clearing failure");
			pxFail("[IOP] Impossible block clearing failure");
		}
	}

	iopClearRecLUT(PSX_GETBLOCK(lowerextent), (upperextent - lowerextent) / 4);

	return upperextent - pc;
}

static __fi void recClearIOP(u32 Addr, u32 Size)
{
	u32 pc = Addr;
	while (pc < Addr + (Size << 2)) // Size * 4
		pc += PSXREC_CLEARM(pc);
}

void psxSetBranchReg(u32 reg)
{
	psxbranch = 1;

	if (reg != 0xffffffff)
	{
		const bool swap = psxTrySwapDelaySlot(reg, 0, 0);

		if (!swap)
		{
			const int wbreg = _allocX86reg(X86TYPE_PSX_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED); // [iter138] IOP uses PSX_PCWRITEBACK not EE PCWRITEBACK
            auto reg32 = HostW(wbreg); // [iter137] slot→phys via HostW

			_psxMoveGPRtoR(reg32, reg);

			psxRecompileNextInstruction(true, false);

			if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PSX_PCWRITEBACK) // [iter138]
			{
//				xMOV(ptr32[&psxRegs.pc], xRegister32(wbreg));
                armStore(PTR_CPU(psxRegs.pc), reg32);
				x86regs[wbreg].inuse = 0;
			}
			else
			{
//				xMOV(eax, ptr32[&psxRegs.pcWriteback]);
                armLoad(EAX, PTR_CPU(psxRegs.pcWriteback));
//				xMOV(ptr32[&psxRegs.pc], eax);
                armStore(PTR_CPU(psxRegs.pc), EAX);
			}
		}
		else
		{
			if (PSX_IS_DIRTY_CONST(reg) || _hasX86reg(X86TYPE_PSX, reg, 0))
			{
				const int x86reg = _allocX86reg(X86TYPE_PSX, reg, MODE_READ);
//				xMOV(ptr32[&psxRegs.pc], xRegister32(x86reg));
                armStore(PTR_CPU(psxRegs.pc), HostW(x86reg)); // [iter136] slot→phys via HostW, same fix as iter87
			}
			else
			{
				_psxMoveGPRtoM(PTR_CPU(psxRegs.pc), reg);
			}
		}
	}

	_psxFlushCall(FLUSH_EVERYTHING);

	// [P33] "Nuclear Option" delete: const-register JR でサイクルカウントをskipしていた。
	// Android/PC PCSX2 と同じ iPsxBranchTest パスを使用し、正しいサイクル消費と
	// イベントチェックを行う。
	iPsxBranchTest(0xffffffff, 1);

//	JMP32((uptr)iopDispatcherReg - ((uptr)x86Ptr + 5));
    armEmitJmp(iopDispatcherReg);
}

void psxSetBranchImm(u32 imm)
{
	psxbranch = 1;
	pxAssert(imm);

	// end the current block
//	xMOV(ptr32[&psxRegs.pc], imm);
    armStore(PTR_CPU(psxRegs.pc), imm);

	_psxFlushCall(FLUSH_EVERYTHING);
	iPsxBranchTest(imm, imm <= psxpc);

//	recBlocks.Link(HWADDR(imm), xJcc32());
    armAsm->Nop();
    recBlocks.Link(HWADDR(imm), (s32 *) armGetCurrentCodePointer()-1);
}

static __fi u32 psxScaleBlockCycles()
{
	return s_psxBlockCycles;
}

static void iPsxAddEECycles(u32 blockCycles)
{
	if (!(psxHu32(HW_ICFG) & (1 << 3))) [[likely]]
	{
		if (blockCycles != 0xFFFFFFFF) {
//            xSUB(ptr32[&psxRegs.iopCycleEE], blockCycles * 8);
            armSub(PTR_CPU(psxRegs.iopCycleEE), blockCycles * 8, true);
        }
		else {
//            xSUB(ptr32[&psxRegs.iopCycleEE], eax);
            armSub(PTR_CPU(psxRegs.iopCycleEE), EAX, true);
        }
		return;
	}

	// F = gcd(PS2CLK, PSXCLK) = 230400
	const u32 cnum = 1280; // PS2CLK / F
	const u32 cdenom = 147; // PSXCLK / F

	if (blockCycles != 0xFFFFFFFF) {
//        xMOV(eax, blockCycles * cnum);
        armAsm->Mov(EAX, blockCycles * cnum);
    }
//	xADD(eax, ptr32[&psxRegs.iopCycleEECarry]);
    armAsm->Add(EAX, EAX, armLoad(PTR_CPU(psxRegs.iopCycleEECarry)));
//	xMOV(ecx, cdenom);
    armAsm->Mov(ECX, cdenom);
    armAsm->Mov(EEX, EAX);
//	xXOR(edx, edx);
    armAsm->Eor(EDX, EDX, EDX);
//	xUDIV(ecx);
    armAsm->Udiv(EAX, EEX, ECX);
    armAsm->Msub(EDX, EAX, ECX, EEX);
//	xMOV(ptr32[&psxRegs.iopCycleEECarry], edx);
    armStore(PTR_CPU(psxRegs.iopCycleEECarry), EDX);
//	xSUB(ptr32[&psxRegs.iopCycleEE], eax);
    armSub(PTR_CPU(psxRegs.iopCycleEE), EAX, true);
}

static void iPsxBranchTest(u32 newpc, u32 cpuBranch)
{
	u32 blockCycles = psxScaleBlockCycles();

	if (EmuConfig.Speedhacks.WaitLoop && s_nBlockFF && newpc == s_branchTo)
	{
//		xMOV(eax, ptr32[&psxRegs.cycle]);
        armLoad(EAX, PTR_CPU(psxRegs.cycle));
//		xMOV(ecx, eax);
        armAsm->Mov(ECX, EAX);
//		xMOV(edx, ptr32[&psxRegs.iopCycleEE]);
        armLoadsw(EDX, PTR_CPU(psxRegs.iopCycleEE));
//		xADD(edx, 7);
        armAsm->Add(EDX, EDX, 7);
//		xSHR(edx, 3);
        armAsm->Lsr(EDX, EDX, 3);
//		xADD(eax, edx);
        armAsm->Add(EAX, EAX, EDX);
//		xCMP(eax, ptr32[&psxRegs.iopNextEventCycle]);
        armLoadsw(EEX, PTR_CPU(psxRegs.iopNextEventCycle));
        armAsm->Cmp(EAX, EEX);
//		xCMOVNS(eax, ptr32[&psxRegs.iopNextEventCycle]);
        armAsm->Csel(EAX, EEX, EAX, a64::Condition::pl);
//		xMOV(ptr32[&psxRegs.cycle], eax);
        armStore(PTR_CPU(psxRegs.cycle), EAX);
//		xSUB(eax, ecx);
        armAsm->Sub(EAX, EAX, ECX);
//		xSHL(eax, 3);
        armAsm->Lsl(EAX, EAX, 3);
		iPsxAddEECycles(0xFFFFFFFF);
//		xJLE(iopExitRecompiledCode);
        armEmitCondBranch(a64::Condition::le, iopExitRecompiledCode);

//		xFastCall((void*)iopEventTest);
        armEmitCall(reinterpret_cast<const void*>(iopEventTest));

		if (newpc != 0xffffffff)
		{
//			xCMP(ptr32[&psxRegs.pc], newpc);
            armAsm->Cmp(armLoad(PTR_CPU(psxRegs.pc)), newpc);
//			xJNE(iopDispatcherReg);
            armEmitCondBranch(a64::Condition::ne, iopDispatcherReg);
		}
	}
	else
	{
//		xMOV(ebx, ptr32[&psxRegs.cycle]);
//		xADD(ebx, blockCycles);
//		xMOV(ptr32[&psxRegs.cycle], ebx); // update cycles
//		xMOV(ebx, ptr32[&psxRegs.cycle]);
//		xADD(ebx, blockCycles);
//		xMOV(ptr32[&psxRegs.cycle], ebx); // update cycles
//      armAdd(EBX, PTR_CPU(psxRegs.cycle), blockCycles);
        // [iPSX2] Explicit 32-bit update to avoid clobbering PC
        armAsm->Ldr(EBX.W(), PTR_CPU(psxRegs.cycle));
        armAsm->Add(EBX.W(), EBX.W(), blockCycles);
        armAsm->Str(EBX.W(), PTR_CPU(psxRegs.cycle));

		// jump if iopCycleEE <= 0  (iop's timeslice timed out, so time to return control to the EE)
		iPsxAddEECycles(blockCycles);
//		xJLE(iopExitRecompiledCode);
        armEmitCondBranch(a64::Condition::le, iopExitRecompiledCode);

		// check if an event is pending
//		xSUB(ebx, ptr32[&psxRegs.iopNextEventCycle]);
        armAsm->Subs(EBX, EBX, armLoadsw(PTR_CPU(psxRegs.iopNextEventCycle)));
//		xForwardJS<u8> nointerruptpending;
        a64::Label nointerruptpending;
        armAsm->B(&nointerruptpending, a64::Condition::mi);

//		xFastCall((void*)iopEventTest);
        armEmitCall(reinterpret_cast<void*>(iopEventTest));

        if (newpc != 0xffffffff)
        {
//			xCMP(ptr32[&psxRegs.pc], newpc);
            armAsm->Cmp(armLoad(PTR_CPU(psxRegs.pc)), newpc);
//			xJNE(iopDispatcherReg);
            armEmitCondBranch(a64::Condition::ne, iopDispatcherReg);
        }

//		nointerruptpending.SetTarget();
        armBind(&nointerruptpending);
	}
}

#if 0
//static const int *s_pCode;

#if !defined(_MSC_VER)
static void checkcodefn()
{
	int pctemp;

#ifdef _MSC_VER
	__asm mov pctemp, eax;
#else
    __asm__ __volatile__("movl %%eax, %[pctemp]" : [pctemp]"m="(pctemp) );
#endif
	Console.WriteLn("iop code changed! %x", pctemp);
}
#endif
#endif

void rpsxSYSCALL()
{
//	xMOV(ptr32[&psxRegs.code], psxRegs.code);
    armStore(PTR_CPU(psxRegs.code), psxRegs.code);
//	xMOV(ptr32[&psxRegs.pc], psxpc - 4);
    armStore(PTR_CPU(psxRegs.pc), psxpc - 4);
	_psxFlushCall(FLUSH_NODESTROY);

	//xMOV( ecx, 0x20 );			// exception code
    armAsm->Mov( EAX, 0x20 );
	//xMOV( edx, psxbranch==1 );	// branch delay slot?
    armAsm->Mov( ECX, psxbranch==1 );
//	xFastCall((void*)psxException, 0x20, psxbranch == 1);
    armEmitCall(reinterpret_cast<const void*>(psxException));

//	xCMP(ptr32[&psxRegs.pc], psxpc - 4);
    armAsm->Cmp(armLoad(PTR_CPU(psxRegs.pc)), psxpc - 4);
//	j8Ptr[0] = JE8(0);
    a64::Label j8Ptr0;
    armAsm->B(&j8Ptr0, a64::Condition::eq);

//	xADD(ptr32[&psxRegs.cycle], psxScaleBlockCycles());
    armAdd(PTR_CPU(psxRegs.cycle), psxScaleBlockCycles());
	iPsxAddEECycles(psxScaleBlockCycles());
//	JMP32((uptr)iopDispatcherReg - ((uptr)x86Ptr + 5));
    armEmitJmp(iopDispatcherReg);

	// jump target for skipping blockCycle updates
//	x86SetJ8(j8Ptr[0]);
    armBind(&j8Ptr0);

	//if (!psxbranch) psxbranch = 2;
}

void rpsxBREAK()
{
//	xMOV(ptr32[&psxRegs.code], psxRegs.code);
    armStore(PTR_CPU(psxRegs.code), psxRegs.code);
//	xMOV(ptr32[&psxRegs.pc], psxpc - 4);
    armStore(PTR_CPU(psxRegs.pc), psxpc - 4);
	_psxFlushCall(FLUSH_NODESTROY);

	//xMOV( ecx, 0x24 );			// exception code
    armAsm->Mov( EAX, 0x24 );
	//xMOV( edx, psxbranch==1 );	// branch delay slot?
    armAsm->Mov( ECX, psxbranch==1 );
//	xFastCall((void*)psxException, 0x24, psxbranch == 1);
    armEmitCall(reinterpret_cast<const void*>(psxException));

//	xCMP(ptr32[&psxRegs.pc], psxpc - 4);
    armAsm->Cmp(armLoad(PTR_CPU(psxRegs.pc)), psxpc - 4);
//	j8Ptr[0] = JE8(0);
    a64::Label j8Ptr0;
    armAsm->B(&j8Ptr0, a64::Condition::eq);
//	xADD(ptr32[&psxRegs.cycle], psxScaleBlockCycles());
    armAdd(PTR_CPU(psxRegs.cycle), psxScaleBlockCycles());
	iPsxAddEECycles(psxScaleBlockCycles());
//	JMP32((uptr)iopDispatcherReg - ((uptr)x86Ptr + 5));
    armEmitJmp(iopDispatcherReg);
//	x86SetJ8(j8Ptr[0]);
    armBind(&j8Ptr0);

	//if (!psxbranch) psxbranch = 2;
}

static bool psxDynarecCheckBreakpoint()
{
	u32 pc = psxRegs.pc;
	if (CBreakPoints::CheckSkipFirst(BREAKPOINT_IOP, pc) == pc)
		return false;

	int bpFlags = psxIsBreakpointNeeded(pc);
	bool hit = false;
	//check breakpoint at current pc
	if (bpFlags & 1)
	{
		auto cond = CBreakPoints::GetBreakPointCondition(BREAKPOINT_IOP, pc);
		if (cond == NULL || cond->Evaluate())
		{
			hit = true;
		}
	}
	//check breakpoint in delay slot
	if (bpFlags & 2)
	{
		auto cond = CBreakPoints::GetBreakPointCondition(BREAKPOINT_IOP, pc + 4);
		if (cond == NULL || cond->Evaluate())
			hit = true;
	}

	if (!hit)
		return false;

	CBreakPoints::SetBreakpointTriggered(true, BREAKPOINT_IOP);
	VMManager::SetPaused(true);

	// Exit the EE too.
	Cpu->ExitExecution();
	return true;
}

static bool psxDynarecMemcheck(size_t i)
{
	const u32 pc = psxRegs.pc;
	const u32 op = iopMemRead32(pc);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);
	auto mc = CBreakPoints::GetMemChecks(BREAKPOINT_IOP)[i];

	if (CBreakPoints::CheckSkipFirst(BREAKPOINT_IOP, pc) == pc)
		return false;

	if (mc.hasCond)
	{
		if (!mc.cond.Evaluate())
			return false;
	}

	if (mc.result & MEMCHECK_LOG)
	{
		if (opcode.flags & IS_STORE)
			DevCon.WriteLn("Hit R3000 store breakpoint @0x%x", pc);
		else
			DevCon.WriteLn("Hit R3000 load breakpoint @0x%x", pc);
	}

	CBreakPoints::SetBreakpointTriggered(true, BREAKPOINT_IOP);
	VMManager::SetPaused(true);

	// Exit the EE too.
	Cpu->ExitExecution();
	return true;
}

static void psxRecMemcheck(u32 op, u32 bits, bool store)
{
	_psxFlushCall(FLUSH_EVERYTHING | FLUSH_PC);

	// compute accessed address
	_psxMoveGPRtoR(ECX, (op >> 21) & 0x1F);
	if ((s16)op != 0) {
//        xADD(ecx, (s16) op);
        armAsm->Add(ECX, ECX, (s16) op);
    }

//	xMOV(edx, ecx);
    armAsm->Mov(EDX, ECX);
//	xADD(edx, bits / 8);
    armAsm->Add(EDX, EDX, bits >> 3); // bits / 8

	// ecx = access address
	// edx = access address+size

	auto checks = CBreakPoints::GetMemChecks(BREAKPOINT_IOP);
	for (size_t i = 0; i < checks.size(); i++)
	{
		if (checks[i].result == 0)
			continue;
		if ((checks[i].memCond & MEMCHECK_WRITE) == 0 && store)
			continue;
		if ((checks[i].memCond & MEMCHECK_READ) == 0 && !store)
			continue;

		// logic: memAddress < bpEnd && bpStart < memAddress+memSize

//		xMOV(eax, checks[i].end);
        armAsm->Mov(EAX, checks[i].end);
//		xCMP(ecx, eax); // address < end
        armAsm->Cmp(ECX, EAX);
//		xForwardJGE8 next1; // if address >= end then goto next1
        a64::Label next1;
        armAsm->B(&next1, a64::Condition::ge);

//		xMOV(eax, checks[i].start);
        armAsm->Mov(EAX, checks[i].start);
//		xCMP(eax, edx); // start < address+size
        armAsm->Cmp(EAX, EDX);
//		xForwardJGE8 next2; // if start >= address+size then goto next2
        a64::Label next2;
        armAsm->B(&next2, a64::Condition::ge);

		// hit the breakpoint

		if (checks[i].result & MEMCHECK_BREAK)
		{
//			xMOV(eax, i);
            armAsm->Mov(EAX, i);
//			xFastCall((void*)psxDynarecMemcheck, eax);
            armEmitCall(reinterpret_cast<const void*>(psxDynarecMemcheck));
//			xTEST(al, al);
//			xJNZ(iopExitRecompiledCode);
            armEmitCbnz(EAX, iopExitRecompiledCode);
		}

//		next1.SetTarget();
        armBind(&next1);
//		next2.SetTarget();
        armBind(&next2);
	}
}

static void psxEncodeBreakpoint()
{
	if (psxIsBreakpointNeeded(psxpc) != 0)
	{
		_psxFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
//		xFastCall((void*)psxDynarecCheckBreakpoint);
        armEmitCall(reinterpret_cast<void*>(psxDynarecCheckBreakpoint));
//		xTEST(al, al);
//		xJNZ(iopExitRecompiledCode);
        armEmitCbnz(EAX, iopExitRecompiledCode);
	}
}

static void psxEncodeMemcheck()
{
	int needed = psxIsMemcheckNeeded(psxpc);
	if (needed == 0)
		return;

	u32 op = iopMemRead32(needed == 2 ? psxpc + 4 : psxpc);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	bool store = (opcode.flags & IS_STORE) != 0;
	switch (opcode.flags & MEMTYPE_MASK)
	{
		case MEMTYPE_BYTE:
			psxRecMemcheck(op, 8, store);
			break;
		case MEMTYPE_HALF:
			psxRecMemcheck(op, 16, store);
			break;
		case MEMTYPE_WORD:
			psxRecMemcheck(op, 32, store);
			break;
		case MEMTYPE_DWORD:
			psxRecMemcheck(op, 64, store);
			break;
	}
}

void psxRecompileNextInstruction(bool delayslot, bool swapped_delayslot)
{
#if DEBUG
    { u32 op = iopMemRead32(psxpc);
    Console.WriteLn("RecIns: %08X %08X %s", psxpc, op, disR3000AF(op, psxpc)); }
#endif

#ifdef DUMP_BLOCKS
	const bool dump_block = true;

	const u8* instStart = x86Ptr;
	ZydisDecoder disas_decoder;
	ZydisFormatter disas_formatter;
	ZydisDecodedInstruction disas_instruction;

	if (dump_block)
	{
		fprintf(stderr, "Compiling %s%s\n", delayslot ? "delay slot " : "", disR3000AF(iopMemRead32(psxpc), psxpc));
		if (!delayslot)
		{
			ZydisDecoderInit(&disas_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_ADDRESS_WIDTH_64);
			ZydisFormatterInit(&disas_formatter, ZYDIS_FORMATTER_STYLE_INTEL);
			s_old_print_address = (ZydisFormatterFunc)&ZydisFormatterPrintAddressAbsolute;
			ZydisFormatterSetHook(&disas_formatter, ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_ABS, (const void**)&s_old_print_address);
		}
	}
#endif

	const int old_code = psxRegs.code;
	EEINST* old_inst_info = g_pCurInstInfo;
	s_recompilingDelaySlot = delayslot;

	// add breakpoint
	if (!delayslot)
	{
		// Broken on x64
		psxEncodeBreakpoint();
		psxEncodeMemcheck();
	}
	else
	{
		_clearNeededX86regs();
	}

	psxRegs.code = iopMemRead32(psxpc);
	s_psxBlockCycles++;
	psxpc += 4;

	g_pCurInstInfo++;

	g_iopCyclePenalty = 0;
	rpsxBSC[psxRegs.code >> 26]();
	s_psxBlockCycles += g_iopCyclePenalty;

	if (!swapped_delayslot)
		_clearNeededX86regs();

	if (swapped_delayslot)
	{
		psxRegs.code = old_code;
		g_pCurInstInfo = old_inst_info;
	}

#ifdef DUMP_BLOCKS
	if (dump_block && !delayslot)
	{
		const u8* instPtr = instStart;
		ZyanUSize instLength = static_cast<ZyanUSize>(x86Ptr - instStart);
		while (ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&disas_decoder, instPtr, instLength, &disas_instruction)))
		{
			char buffer[256];
			if (ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&disas_formatter, &disas_instruction, buffer, sizeof(buffer), (ZyanU64)instPtr)))
				std::fprintf(stderr, "    %016" PRIX64 "    %s\n", (u64)instPtr, buffer);

			instPtr += disas_instruction.length;
			instLength -= disas_instruction.length;
		}
	}
#endif
}

#ifdef TRACE_BLOCKS
static void PreBlockCheck(u32 blockpc)
{
#if 0
	static FILE* fp = nullptr;
	static bool fp_opened = false;
	if (!fp_opened && psxRegs.cycle >= 0)
	{
		fp = std::fopen("C:\\Dumps\\comp\\ioplog.txt", "wb");
		fp_opened = true;
	}
	if (fp)
	{
		u32 hash = crc32(0, (Bytef*)&psxRegs, offsetof(psxRegisters, pc));

#if 1
		std::fprintf(fp, "%08X (%u; %08X):", psxRegs.pc, psxRegs.cycle, hash);
		for (int i = 0; i < 34; i++)
		{
			std::fprintf(fp, " %s: %08X", R3000A::disRNameGPR[i], psxRegs.GPR.r[i]);
		}
		std::fprintf(fp, "\n");
#else
		std::fprintf(fp, "%08X (%u): %08X\n", psxRegs.pc, psxRegs.cycle, hash);
#endif
		// std::fflush(fp);
	}
#endif
#if 0
	if (psxRegs.cycle == 0)
		__debugbreak();
#endif
}
#endif

static void iopRecRecompile(const u32 startpc)
{
    if (DarwinMisc::iPSX2_CRASH_DIAG) {
         static bool s_diag_enter = false;
         if(!s_diag_enter) {
             s_diag_enter = true;
             std::fprintf(stderr, "@@IOPREC_ENTER@@ pc=%08x\n", startpc);
         }
    }
    DevCon.WriteLn("@@IOP_RECOMPILE@@ pc=%08x", startpc);
	u32 i;
	u32 willbranch3 = 0;

	// When upgrading the IOP, there are two resets, the second of which is a 'fake' reset
	// This second 'reset' involves UDNL calling SYSMEM and LOADCORE directly, resetting LOADCORE's modules
	// This detects when SYSMEM is called and clears the modules then
	if(startpc == 0x890)
	{
		{
			extern u32 g_iop_reboot_count;
			g_iop_reboot_count++;
			u32 iop19600 = (iopMem && iopMem->Main) ? *(u32*)(iopMem->Main + 0x19600) : 0xDEADu;
			Console.WriteLn("@@IOP_REBOOT_890@@ reboot#%u iopcyc=%u [19600]=%08x", g_iop_reboot_count, psxRegs.cycle, iop19600);
		}
		R3000SymbolGuardian.ClearIrxModules();
		extern char g_ioprp_path[];
		extern bool g_ioprp_path_pending;
		if (g_ioprp_path_pending) {
			g_ioprp_path_pending = false;
		}
	}

	// Inject IRX hack
	if (startpc == 0x1630 && EmuConfig.CurrentIRX.length() > 3)
	{
		if (iopMemRead32(0x20018) == 0x1F)
		{
			// FIXME do I need to increase the module count (0x1F -> 0x20)
			iopMemWrite32(0x20094, 0xbffc0000);
		}
	}

	// [iter131] pc=0 soft diagnostic with predecessor tracking
	static u32 s_last_startpc = 0u;
	if (startpc == 0u) {
		static u32 s_pc0_count = 0;
		if (s_pc0_count < 3) {
			++s_pc0_count;
			// Also dump IOP RAM at 0 and 4 bytes around the calling block
			// [iter134] also print $v0/$t8 from psxRegs to see if LW result was stored correctly
		Console.WriteLn("@@IOPREC_PC0@@ n=%u prev_pc=0x%08x gpr_v0=0x%08x gpr_t8=0x%08x",
				s_pc0_count, s_last_startpc,
				psxRegs.GPR.r[2], psxRegs.GPR.r[24]);
			// Dump 8 words of IOP BIOS ROM at prev_pc to see what branch is there
			const u32 prev = s_last_startpc;
			if (prev >= 0xBFC00000u && prev <= 0xBFFFFFFFu) {
				for (int i = 0; i < 8; i++) {
					Console.WriteLn("@@IOPREC_PC0_CODE@@ [0x%08x]=0x%08x",
						prev + i*4, iopMemRead32(prev + i*4));
				}
			}
		}
		return;
	}
	s_last_startpc = startpc;

	// if recPtr reached the mem limit reset whole mem
	if (recPtr >= recPtrEnd)
	{
		recResetIOP();
	}

//	xSetPtr(recPtr);
    armSetAsmPtr(recPtr, _256kb, nullptr);
//	recPtr = xGetAlignedCallTarget();
    recPtr = armStartBlock();

	s_pCurBlock = PSX_GETBLOCK(startpc);

	// [iter139] Soft assertion: if block already compiled, log and skip recompile
	if (s_pCurBlock->GetFnptr() != (uptr)iopJITCompile) {
		static u32 s_dup_count = 0;
		if (s_dup_count < 5) {
			++s_dup_count;
			Console.WriteLn("@@IOPREC_DUP@@ n=%u startpc=0x%08x fnptr=%p",
				s_dup_count, startpc, (void*)s_pCurBlock->GetFnptr());
		}
		armEndBlock(); // balance armStartBlock(); size=0 is safe
		return;
	}


// Cleanup duplicate.
    // [iPSX2] Breadcrumb
    if (DarwinMisc::iPSX2_CRASH_DIAG) {
        static bool s_diag_trace = false;
        // Only log once to avoid spam, or key on startpc if needed
        if (!s_diag_trace) {
            s_diag_trace = true;
            // Assuming startpc, recBlocks etc are visible
            // Log pointers before the suspected crash call
            // Using DevCon or printf. DevCon might be safer if single threaded. 
            // Or std::fprintf(stderr) for immediate output.
            std::fprintf(stderr, "@@IOPREC_L1729_PRE@@ pc=%08x\n", startpc);
        }
    }

	s_pCurBlockEx = recBlocks.Get(HWADDR(startpc));

    if (DarwinMisc::iPSX2_CRASH_DIAG) {
        static bool s_diag_post = false;
        if (!s_diag_post) {
            s_diag_post = true;
             std::fprintf(stderr, "@@IOPREC_L1729_POST@@ ptr=%p\n", s_pCurBlockEx);
        }
    }

	if (!s_pCurBlockEx || s_pCurBlockEx->startpc != HWADDR(startpc))
		s_pCurBlockEx = recBlocks.New(HWADDR(startpc), (uptr)recPtr);

	psxbranch = 0;

	s_pCurBlock->SetFnptr((uptr)armGetCurrentCodePointer());

#ifdef __APPLE__
    // [iPSX2] Record IOP JIT Block
    DarwinMisc::RecordJitBlock(startpc, (void*)armGetCurrentCodePointer(), 0);
#endif

	s_psxBlockCycles = 0;

	// reset recomp state variables
	psxpc = startpc;
	g_psxHasConstReg = g_psxFlushedConstReg = 1;

	_initX86regs();

	if ((psxHu32(HW_ICFG) & 8) && (HWADDR(startpc) == 0xa0 || HWADDR(startpc) == 0xb0 || HWADDR(startpc) == 0xc0))
	{
//		xFastCall((void*)psxBiosCall);
        armEmitCall(reinterpret_cast<void*>(psxBiosCall));
//		xTEST(al, al);
//		xJNZ(iopDispatcherReg);
        armEmitCbnz(EAX, iopDispatcherReg);
	}

#ifdef TRACE_BLOCKS
	xFastCall((void*)PreBlockCheck, psxpc);
#endif

	// go until the next branch
	i = startpc;
	s_nEndBlock = 0xffffffff;
	s_branchTo = -1;

	while (1)
	{
		BASEBLOCK* pblock = PSX_GETBLOCK(i);
		if (i != startpc && pblock->GetFnptr() != (uptr)iopJITCompile)
		{
			// branch = 3
			willbranch3 = 1;
			s_nEndBlock = i;
			break;
		}

		psxRegs.code = iopMemRead32(i);

		switch (psxRegs.code >> 26)
		{
			case 0: // special
				if (_Funct_ == 8 || _Funct_ == 9)
				{ // JR, JALR
					s_nEndBlock = i + 8;
					goto StartRecomp;
				}
				break;

			case 1: // regimm
				if (_Rt_ == 0 || _Rt_ == 1 || _Rt_ == 16 || _Rt_ == 17)
				{
					s_branchTo = _Imm_ * 4 + i + 4;
					if (s_branchTo > startpc && s_branchTo < i)
						s_nEndBlock = s_branchTo;
					else
						s_nEndBlock = i + 8;
					goto StartRecomp;
				}
				break;

			case 2: // J
			case 3: // JAL
				s_branchTo = (_InstrucTarget_ << 2) | ((i + 4) & 0xf0000000);
				s_nEndBlock = i + 8;
				goto StartRecomp;

			// branches
			case 4:
			case 5:
			case 6:
			case 7:
				s_branchTo = _Imm_ * 4 + i + 4;
				if (s_branchTo > startpc && s_branchTo < i)
					s_nEndBlock = s_branchTo;
				else
					s_nEndBlock = i + 8;
				goto StartRecomp;
		}

		i += 4;
	}

StartRecomp:

	// [P16] Enhanced IOP WaitLoop detection — match EE-level pattern recognition
	// Original code only detected NOP-only blocks. IOP idle loops typically use
	// LUI+LW+ANDI+BEQ patterns to poll HW registers (e.g., INTC_STAT at 0x1F801070).
	// Without this, the IOP runs idle loops instruction-by-instruction, causing
	// the emulator to run at ~5% of expected speed during game idle phases.
	s_nBlockFF = false;
	if (s_branchTo == startpc)
	{
		s_nBlockFF = true;

		u32 reads = 0, loads = 1; // bit 0 = r0 (always zero, treated as "loaded")

		for (i = startpc; i < s_nEndBlock; i += 4)
		{
			if (i == s_nEndBlock - 8)
				continue; // skip branch instruction (second-to-last)

			const u32 code = iopMemRead32(i);
			const u32 op   = (code >> 26) & 0x3F;
			const u32 rs   = (code >> 21) & 0x1F;
			const u32 rt   = (code >> 16) & 0x1F;
			const u32 rd   = (code >> 11) & 0x1F;
			const u32 fn   = code & 0x3F;

			// NOP
			if (code == 0)
				continue;
			// IMM arithmetic: ADDI(08) ADDIU(09) SLTI(0A) SLTIU(0B) ANDI(0C) ORI(0D) XORI(0E) LUI(0F)
			else if ((op & 0x38) == 0x08)
			{
				if (loads & (1u << rs))
				{
					loads |= (1u << rt);
					continue;
				}
				else
					reads |= (1u << rs);
				if (reads & (1u << rt))
				{
					s_nBlockFF = false;
					break;
				}
			}
			// REG arithmetic: ADD/ADDU/SUB/SUBU/AND/OR/XOR/NOR/SLT/SLTU
			else if (op == 0 && ((fn >= 0x20 && fn <= 0x27) || fn == 0x2A || fn == 0x2B))
			{
				if ((loads & (1u << rs)) && (loads & (1u << rt)))
				{
					loads |= (1u << rd);
					continue;
				}
				else
					reads |= (1u << rs) | (1u << rt);
				if (reads & (1u << rd))
				{
					s_nBlockFF = false;
					break;
				}
			}
			// Loads: LB(20) LH(21) LWL(22) LW(23) LBU(24) LHU(25) LWR(26)
			else if ((op & 0x38) == 0x20)
			{
				if (loads & (1u << rs))
				{
					loads |= (1u << rt);
					continue;
				}
				else
					reads |= (1u << rs);
				if (reads & (1u << rt))
				{
					s_nBlockFF = false;
					break;
				}
			}
			// MFC0: COP0 register read
			else if (op == 0x10 && rs == 0)
			{
				loads |= (1u << rt);
			}
			else
			{
				s_nBlockFF = false;
				break;
			}
		}
	}

	// rec info //
	{
		EEINST* pcur;

		if (s_nInstCacheSize < (s_nEndBlock - startpc) / 4 + 1)
		{
			free(s_pInstCache);
			s_nInstCacheSize = (s_nEndBlock - startpc) / 4 + 10;
			s_pInstCache = (EEINST*)malloc(sizeof(EEINST) * s_nInstCacheSize);
			pxAssert(s_pInstCache != NULL);
		}

		pcur = s_pInstCache + (s_nEndBlock - startpc) / 4;
		_recClearInst(pcur);
		pcur->info = 0;

		for (i = s_nEndBlock; i > startpc; i -= 4)
		{
			psxRegs.code = iopMemRead32(i - 4);
			pcur[-1] = pcur[0];
			rpsxpropBSC(pcur - 1, pcur);
			pcur--;
		}
	}

	g_pCurInstInfo = s_pInstCache;
	while (!psxbranch && psxpc < s_nEndBlock)
	{
		psxRecompileNextInstruction(false, false);
	}

	pxAssert((psxpc - startpc) >> 2 <= 0xffff);
	s_pCurBlockEx->size = (psxpc - startpc) >> 2;

	if (!(psxpc & 0x10000000))
		g_psxMaxRecMem = std::max((psxpc & ~0xa0000000), g_psxMaxRecMem);

	if (psxbranch == 2)
	{
		_psxFlushCall(FLUSH_EVERYTHING);

		iPsxBranchTest(0xffffffff, 1);

//		JMP32((uptr)iopDispatcherReg - ((uptr)x86Ptr + 5));
        armEmitJmp(iopDispatcherReg);
	}
	else
	{
		if (psxbranch)
			pxAssert(!willbranch3);
		else
		{
//			xADD(ptr32[&psxRegs.cycle], psxScaleBlockCycles());
            armAdd(PTR_CPU(psxRegs.cycle), psxScaleBlockCycles());
			iPsxAddEECycles(psxScaleBlockCycles());
		}

		if (willbranch3 || !psxbranch)
		{
			pxAssert(psxpc == s_nEndBlock);
			_psxFlushCall(FLUSH_EVERYTHING);
//			xMOV(ptr32[&psxRegs.pc], psxpc);
            armStore(PTR_CPU(psxRegs.pc), psxpc);
//			recBlocks.Link(HWADDR(s_nEndBlock), xJcc32());
            armAsm->Nop();
            recBlocks.Link(HWADDR(s_nEndBlock), (s32*)armGetCurrentCodePointer()-1);
			psxbranch = 3;
		}
	}

	pxAssert(armGetCurrentCodePointer() < SysMemory::GetIOPRecEnd());

	pxAssert(armGetCurrentCodePointer() - recPtr < _64kb);
	s_pCurBlockEx->x86size = armGetCurrentCodePointer() - recPtr;

	Perf::iop.RegisterPC((void*)s_pCurBlockEx->fnptr, s_pCurBlockEx->x86size, s_pCurBlockEx->startpc);

//	recPtr = xGetPtr();
    recPtr = armEndBlock();

    // [iPSX2] CRITICAL FIX: Flush Instruction Cache for iOS JIT
    HostSys::FlushInstructionCache((void*)s_pCurBlockEx->fnptr, s_pCurBlockEx->x86size);


	pxAssert((g_psxHasConstReg & g_psxFlushedConstReg) == g_psxHasConstReg);

	s_pCurBlock = NULL;
	s_pCurBlockEx = NULL;
}

R3000Acpu psxRec = {
	recReserve,
	recResetIOP,
	recExecuteBlock,
	recClearIOP,
	recShutdown,
};
