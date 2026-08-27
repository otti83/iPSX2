// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R3000A.h" // [iter467] for psxReset(), psxRegs
#include "IopMem.h" // [iter474] for psxHu32()
#include "IopHw.h"  // [iter476] for HW_PS1_GPU_STATUS

#include <float.h>

#include "R5900.h"
#include "R5900OpcodeTables.h"
#include "GS.h"
#include "ps2/BiosTools.h"
#include "Elfheader.h"
#include "DebugTools/DebugInterface.h"
#include "DebugTools/Breakpoints.h"
#include "Host.h"
#include "VMManager.h"
#include "GS/Renderers/Common/GSRenderer.h"

// [iter672] sceSifGetReg(0x80000002) tracking flag for COP0.cpp ERET probe
int g_sifgetreg_track = 0;

// [FIX] sceSifIopReset IOPRP path — IOP SIF DMA でパスが届かないissueの HLE
// R5900OpcodeImpl.cpp でdetect・save、iR3000A.cpp の IOP_REBOOT_890 で IOP memoryに注入
// Removal condition: IOP SIF DMA 割り込みが正常behaviorし IOPRP パスが自然にhandlingされるようになった後
char g_ioprp_path[128] = {};
bool g_ioprp_path_pending = false;
bool armsx2_ioprp_path_set() { return g_ioprp_path[0] != 0; }

// [R59] Last SYSCALL tracking for 0x22000000 diagnosis
// Removal condition: 0x22000000 ジャンプ元after identified
u32 g_last_syscall_pc = 0;
u32 g_last_syscall_call = 0;
u32 g_last_syscall_a0 = 0;
u32 g_last_syscall_ra = 0;
u32 g_last_syscall_cycle = 0;

// [R59] Syscall ring buffer — last 32 syscalls
struct SyscallEntry {
	u32 pc, call, a0, ra, v1_raw, cycle;
};
static constexpr int SYSCALL_RING_SIZE = 32;
SyscallEntry g_syscall_ring[SYSCALL_RING_SIZE] = {};
int g_syscall_ring_idx = 0;

#include "fmt/format.h"

// [iter191] JIT cache full reset (forward declaration)
extern "C" void recReset();

GS_VideoMode gsVideoMode = GS_VideoMode::Uninitialized;
bool gsIsInterlaced = false;

static __fi bool _add64_Overflow( s64 x, s64 y, s64 &ret )
{
	const s64 result = x + y;

	// Let's all give gigaherz a big round of applause for finding this gem,
	// which apparently works, and generates compact/fast x86 code too (the
	// other method below is like 5-10 times slower).

	if( ((~(x^y))&(x^result)) < 0 ) {
		cpuException(0x30, cpuRegs.branch);		// fixme: is 0x30 right for overflow??
		return true;
	}

	// the not-as-fast style!
	//if( ((x >= 0) && (y >= 0) && (result <  0)) ||
	//	((x <  0) && (y <  0) && (result >= 0)) )
	//	cpuException(0x30, cpuRegs.branch);

	ret = result;
	return false;
}

static __fi bool _add32_Overflow( s32 x, s32 y, s64 &ret )
{
	GPR_reg64 result;  result.SD[0] = (s64)x + y;

	// This 32bit method can rely on the MIPS documented method of checking for
	// overflow, whichs imply compares bit 32 (rightmost bit of the upper word),
	// against bit 31 (leftmost of the lower word).

	// If bit32 != bit31 then we have an overflow.
	if( (result.UL[0]>>31) != (result.UL[1] & 1) ) {
		cpuException(0x30, cpuRegs.branch);
		return true;
	}

	ret = result.SD[0];

	return false;
}


const R5900::OPCODE& R5900::GetCurrentInstruction()
{
	const OPCODE* opcode = &R5900::OpcodeTables::tbl_Standard[_Opcode_];

	while( opcode->getsubclass != NULL )
		opcode = &opcode->getsubclass(cpuRegs.code);

	return *opcode;
}

const R5900::OPCODE& R5900::GetInstruction(u32 op)
{
	const OPCODE* opcode = &R5900::OpcodeTables::tbl_Standard[op >> 26];

	while( opcode->getsubclass != NULL )
		opcode = &opcode->getsubclass(op);

	return *opcode;
}

const char * const R5900::bios[256]=
{
//0x00
	"RFU000_FullReset", "ResetEE",				"SetGsCrt",				"RFU003",
	"Exit",				"RFU005",				"LoadExecPS2",			"ExecPS2",
	"RFU008",			"RFU009",				"AddSbusIntcHandler",	"RemoveSbusIntcHandler",
	"Interrupt2Iop",	"SetVTLBRefillHandler", "SetVCommonHandler",	"SetVInterruptHandler",
//0x10
	"AddIntcHandler",	"RemoveIntcHandler",	"AddDmacHandler",		"RemoveDmacHandler",
	"_EnableIntc",		"_DisableIntc",			"_EnableDmac",			"_DisableDmac",
	"_SetAlarm",		"_ReleaseAlarm",		"_iEnableIntc",			"_iDisableIntc",
	"_iEnableDmac",		"_iDisableDmac",		"_iSetAlarm",			"_iReleaseAlarm",
//0x20
	"CreateThread",			"DeleteThread",		"StartThread",			"ExitThread",
	"ExitDeleteThread",		"TerminateThread",	"iTerminateThread",		"DisableDispatchThread",
	"EnableDispatchThread",		"ChangeThreadPriority", "iChangeThreadPriority",	"RotateThreadReadyQueue",
	"iRotateThreadReadyQueue",	"ReleaseWaitThread",	"iReleaseWaitThread",		"GetThreadId",
//0x30
	"ReferThreadStatus","iReferThreadStatus",	"SleepThread",		"WakeupThread",
	"_iWakeupThread",   "CancelWakeupThread",	"iCancelWakeupThread",	"SuspendThread",
	"iSuspendThread",   "ResumeThread",		"iResumeThread",	"JoinThread",
	"RFU060",	    "RFU061",			"EndOfHeap",		 "RFU063",
//0x40
	"CreateSema",	    "DeleteSema",	"SignalSema",		"iSignalSema",
	"WaitSema",	    "PollSema",		"iPollSema",		"ReferSemaStatus",
	"iReferSemaStatus", "RFU073",		"SetOsdConfigParam", 	"GetOsdConfigParam",
	"GetGsHParam",	    "GetGsVParam",	"SetGsHParam",		"SetGsVParam",
//0x50
	"RFU080_CreateEventFlag",	"RFU081_DeleteEventFlag",
	"RFU082_SetEventFlag",		"RFU083_iSetEventFlag",
	"RFU084_ClearEventFlag",	"RFU085_iClearEventFlag",
	"RFU086_WaitEventFlag",		"RFU087_PollEventFlag",
	"RFU088_iPollEventFlag",	"RFU089_ReferEventFlagStatus",
	"RFU090_iReferEventFlagStatus", "RFU091_GetEntryAddress",
	"EnableIntcHandler_iEnableIntcHandler",
	"DisableIntcHandler_iDisableIntcHandler",
	"EnableDmacHandler_iEnableDmacHandler",
	"DisableDmacHandler_iDisableDmacHandler",
//0x60
	"KSeg0",				"EnableCache",	"DisableCache",			"GetCop0",
	"FlushCache",			"RFU101",		"CpuConfig",			"iGetCop0",
	"iFlushCache",			"RFU105",		"iCpuConfig", 			"sceSifStopDma",
	"SetCPUTimerHandler",	"SetCPUTimer",	"SetOsdConfigParam2",	"GetOsdConfigParam2",
//0x70
	"GsGetIMR_iGsGetIMR",				"GsGetIMR_iGsPutIMR",	"SetPgifHandler", 				"SetVSyncFlag",
	"RFU116",							"print", 				"sceSifDmaStat_isceSifDmaStat", "sceSifSetDma_isceSifSetDma",
	"sceSifSetDChain_isceSifSetDChain", "sceSifSetReg",			"sceSifGetReg",					"ExecOSD",
	"Deci2Call",						"PSMode",				"MachineType",					"GetMemorySize",
};

static u32 deci2addr = 0;
static u32 deci2handler = 0;
static char deci2buffer[256];

void Deci2Reset()
{
	deci2handler	= 0;
	deci2addr		= 0;
	std::memset(deci2buffer, 0, sizeof(deci2buffer));
}

bool SaveStateBase::deci2Freeze()
{
	if (!FreezeTag("deci2"))
		return false;

	Freeze( deci2addr );
	Freeze( deci2handler );
	Freeze( deci2buffer );

	return IsOkay();
}

/*
 *	int Deci2Call(int, u_int *);
 *
 *  HLE implementation of the Deci2 interface.
 */

static int __Deci2Call(int call, u32 *addr)
{
	if (call > 0x10)
		return -1;

	switch (call)
	{
		case 1: // open
			if( addr != NULL )
			{
				deci2addr = addr[1];
				BIOS_LOG("deci2open: %x,%x,%x,%x",
						 addr[3], addr[2], addr[1], addr[0]);
				deci2handler = addr[2];
			}
			else
			{
				deci2handler = 0;
				DevCon.Warning( "Deci2Call.Open > NULL address ignored." );
			}
			return 1;

		case 2: // close
			deci2addr = 0;
			deci2handler = 0;
			return 1;

		case 3: // reqsend
		{
			char reqaddr[128];
			if( addr != NULL )
				std::snprintf(reqaddr, std::size(reqaddr), "%x %x %x %x", addr[3], addr[2], addr[1], addr[0]);

			if (!deci2addr) return 1;

			const u32* d2ptr = (u32*)PSM(deci2addr);

			BIOS_LOG("deci2reqsend: %s: deci2addr: %x,%x,%x,buf=%x %x,%x,len=%x,%x",
				(( addr == NULL ) ? "NULL" : reqaddr),
				d2ptr[7], d2ptr[6], d2ptr[5], d2ptr[4],
				d2ptr[3], d2ptr[2], d2ptr[1], d2ptr[0]);

//			cpuRegs.pc = deci2handler;
//			Console.WriteLn("deci2msg: %s",  (char*)PSM(d2ptr[4]+0xc));

			if (d2ptr[1]>0xc){
				// this looks horribly wrong, justification please?
				u8* pdeciaddr = (u8*)dmaGetAddr(d2ptr[4]+0xc, false);
				if( pdeciaddr == NULL )
					pdeciaddr = (u8*)PSM(d2ptr[4]+0xc);
				else
					pdeciaddr += (d2ptr[4]+0xc) % 16;

				const int copylen = std::min<uint>(255, d2ptr[1]-0xc);
				memcpy(deci2buffer, pdeciaddr, copylen );
				deci2buffer[copylen] = '\0';

				eeConLog( ShiftJIS_ConvertString(deci2buffer) );
			}
			((u32*)PSM(deci2addr))[3] = 0;
			return 1;
		}

		case 4: // poll
			if( addr != NULL )
				BIOS_LOG("deci2poll: %x,%x,%x,%x\n", addr[3], addr[2], addr[1], addr[0]);
			return 1;

		case 5: // exrecv
			return 1;

		case 6: // exsend
			return 1;

		case 0x10://kputs
			if( addr != NULL )
			{
				eeDeci2Log( ShiftJIS_ConvertString((char*)PSM(*addr)) );
			}
			return 1;
	}

	return 0;
}


namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {

void COP2()
{
	//std::string disOut;
	//disR5900Fasm(disOut, cpuRegs.code, cpuRegs.pc);

	//VU0_LOG("%s", disOut.c_str());
	Int_COP2PrintTable[_Rs_]();
}

void Unknown() {
	CPU_LOG("%8.8lx: Unknown opcode called", cpuRegs.pc);
}

void MMI_Unknown() { Console.Warning("Unknown MMI opcode called"); }
void COP0_Unknown() { Console.Warning("Unknown COP0 opcode called"); }
void COP1_Unknown() { Console.Warning("Unknown FPU/COP1 opcode called"); }



/*********************************************************
* Arithmetic with immediate operand                      *
* Format:  OP rt, rs, immediate                          *
*********************************************************/

// Implementation Notes:
//  * It is important that instructions perform overflow checks prior to shortcutting on
//    the zero register (when it is used as a destination).  Overflow exceptions are still
//    handled even though the result is discarded.

// Rt = Rs + Im signed [exception on overflow]
void ADDI()
{
	s64 result;
	bool overflow = _add32_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], _Imm_, result );
	if (overflow || !_Rt_) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = result;
}

// Rt = Rs + Im signed !!! [overflow ignored]
// This instruction is effectively identical to ADDI.  It is not a true unsigned operation,
// but rather it is a signed operation that ignores overflows.
void ADDIU()
{
	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = u64(s64(s32(cpuRegs.GPR.r[_Rs_].UL[0] + u32(s32(_Imm_)))));
}

// Rt = Rs + Im [exception on overflow]
// This is the full 64 bit version of ADDI.  Overflow occurs at 64 bits instead
// of at 32 bits.
void DADDI()
{
	s64 result;
	bool overflow = _add64_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], _Imm_, result );
	if (overflow || !_Rt_) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = result;
}

// Rt = Rs + Im [overflow ignored]
// This instruction is effectively identical to DADDI.  It is not a true unsigned operation,
// but rather it is a signed operation that ignores overflows.
void DADDIU()
{
	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0] + u64(s64(_Imm_));
}
void ANDI() 	{ if (!_Rt_) return; cpuRegs.GPR.r[_Rt_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0] & (u64)_ImmU_; } // Rt = Rs And Im (zero-extended)
void ORI() 	    { if (!_Rt_) return; cpuRegs.GPR.r[_Rt_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0] | (u64)_ImmU_; } // Rt = Rs Or  Im (zero-extended)
void XORI() 	{ if (!_Rt_) return; cpuRegs.GPR.r[_Rt_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0] ^ (u64)_ImmU_; } // Rt = Rs Xor Im (zero-extended)
void SLTI()     { if (!_Rt_) return; cpuRegs.GPR.r[_Rt_].UD[0] = (cpuRegs.GPR.r[_Rs_].SD[0] < (s64)(_Imm_)) ? 1 : 0; } // Rt = Rs < Im (signed)
void SLTIU()    { if (!_Rt_) return; cpuRegs.GPR.r[_Rt_].UD[0] = (cpuRegs.GPR.r[_Rs_].UD[0] < (u64)(_Imm_)) ? 1 : 0; } // Rt = Rs < Im (unsigned)

/*********************************************************
* Register arithmetic                                    *
* Format:  OP rd, rs, rt                                 *
*********************************************************/

// Rd = Rs + Rt		(Exception on Integer Overflow)
void ADD()
{
	s64 result;
	bool overflow = _add32_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], cpuRegs.GPR.r[_Rt_].SD[0], result );
	if (overflow || !_Rd_) return;
	cpuRegs.GPR.r[_Rd_].SD[0] = result;
}

void DADD()
{
	s64 result;
	bool overflow = _add64_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], cpuRegs.GPR.r[_Rt_].SD[0], result );
	if (overflow || !_Rd_) return;
	cpuRegs.GPR.r[_Rd_].SD[0] = result;
}

// Rd = Rs - Rt		(Exception on Integer Overflow)
void SUB()
{
	s64 result;
	bool overflow = _add32_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], -cpuRegs.GPR.r[_Rt_].SD[0], result );
	if (overflow || !_Rd_) return;
	cpuRegs.GPR.r[_Rd_].SD[0] = result;
}

// Rd = Rs - Rt		(Exception on Integer Overflow)
void DSUB()
{
	s64 result;
	bool overflow = _add64_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], -cpuRegs.GPR.r[_Rt_].SD[0], result );
	if (overflow || !_Rd_) return;
	cpuRegs.GPR.r[_Rd_].SD[0] = result;
}

void ADDU() 	{ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = u64(s64(s32(cpuRegs.GPR.r[_Rs_].UL[0]  + cpuRegs.GPR.r[_Rt_].UL[0]))); }	// Rd = Rs + Rt
void DADDU()    { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  + cpuRegs.GPR.r[_Rt_].UD[0]; }
void SUBU() 	{ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = u64(s64(s32(cpuRegs.GPR.r[_Rs_].UL[0]  - cpuRegs.GPR.r[_Rt_].UL[0]))); }	// Rd = Rs - Rt
void DSUBU() 	{ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  - cpuRegs.GPR.r[_Rt_].UD[0]; }
void AND() 	    { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  & cpuRegs.GPR.r[_Rt_].UD[0]; }	// Rd = Rs And Rt
void OR() 	    { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  | cpuRegs.GPR.r[_Rt_].UD[0]; }	// Rd = Rs Or  Rt
void XOR() 	    { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  ^ cpuRegs.GPR.r[_Rt_].UD[0]; }	// Rd = Rs Xor Rt
void NOR() 	    { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] =~(cpuRegs.GPR.r[_Rs_].UD[0] | cpuRegs.GPR.r[_Rt_].UD[0]); }// Rd = Rs Nor Rt
void SLT()		{ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = (cpuRegs.GPR.r[_Rs_].SD[0] < cpuRegs.GPR.r[_Rt_].SD[0]) ? 1 : 0; }	// Rd = Rs < Rt (signed)
void SLTU()		{ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = (cpuRegs.GPR.r[_Rs_].UD[0] < cpuRegs.GPR.r[_Rt_].UD[0]) ? 1 : 0; }	// Rd = Rs < Rt (unsigned)

/*********************************************************
* Register mult/div & Register trap logic                *
* Format:  OP rs, rt                                     *
*********************************************************/

// Signed division "overflows" on (0x80000000 / -1), here (LO = 0x80000000, HI = 0) is returned by MIPS
// in division by zero on MIPS, it appears that:
// LO gets 1 if rs is negative (and the division is signed) and -1 otherwise.
// HI gets the value of rs.

// Result is stored in HI/LO [no arithmetic exceptions]
void DIV()
{
	if (cpuRegs.GPR.r[_Rs_].UL[0] == 0x80000000 && cpuRegs.GPR.r[_Rt_].UL[0] == 0xffffffff)
	{
		cpuRegs.LO.SD[0] = (s32)0x80000000;
		cpuRegs.HI.SD[0] = (s32)0x0;
	}
    else if (cpuRegs.GPR.r[_Rt_].SL[0] != 0)
    {
        cpuRegs.LO.SD[0] = cpuRegs.GPR.r[_Rs_].SL[0] / cpuRegs.GPR.r[_Rt_].SL[0];
        cpuRegs.HI.SD[0] = cpuRegs.GPR.r[_Rs_].SL[0] % cpuRegs.GPR.r[_Rt_].SL[0];
    }
	else
	{
		cpuRegs.LO.SD[0] = (cpuRegs.GPR.r[_Rs_].SL[0] < 0) ? 1 : -1;
		cpuRegs.HI.SD[0] = cpuRegs.GPR.r[_Rs_].SL[0];
	}
}

// Result is stored in HI/LO [no arithmetic exceptions]
void DIVU()
{
	if (cpuRegs.GPR.r[_Rt_].UL[0] != 0)
	{
		// note: DIVU has no sign extension when assigning back to 64 bits
		// note 2: reference material strongly disagrees. (air)
		cpuRegs.LO.SD[0] = (s32)(cpuRegs.GPR.r[_Rs_].UL[0] / cpuRegs.GPR.r[_Rt_].UL[0]);
		cpuRegs.HI.SD[0] = (s32)(cpuRegs.GPR.r[_Rs_].UL[0] % cpuRegs.GPR.r[_Rt_].UL[0]);
	}
	else
	{
		cpuRegs.LO.SD[0] = -1;
		cpuRegs.HI.SD[0] = cpuRegs.GPR.r[_Rs_].SL[0];
	}
}

// Result is written to both HI/LO and to the _Rd_ (Lo only)
void MULT()
{
	s64 res = (s64)cpuRegs.GPR.r[_Rs_].SL[0] * cpuRegs.GPR.r[_Rt_].SL[0];

	// Sign-extend into 64 bits:
	cpuRegs.LO.SD[0] = (s32)(res & 0xffffffff);
	cpuRegs.HI.SD[0] = (s32)(res >> 32);

	if( _Rd_ ) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.LO.UD[0];
}

// Result is written to both HI/LO and to the _Rd_ (Lo only)
void MULTU()
{
	u64 res = (u64)cpuRegs.GPR.r[_Rs_].UL[0] * cpuRegs.GPR.r[_Rt_].UL[0];

	// Note: sign-extend into 64 bits even though it's an unsigned mult.
	cpuRegs.LO.SD[0] = (s32)(res & 0xffffffff);
	cpuRegs.HI.SD[0] = (s32)(res >> 32);

	if( _Rd_ ) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.LO.UD[0];
}

/*********************************************************
* Load higher 16 bits of the first word in GPR with imm  *
* Format:  OP rt, immediate                              *
*********************************************************/
void LUI() {
	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = (s32)(cpuRegs.code << 16);
}

/*********************************************************
* Move from HI/LO to GPR                                 *
* Format:  OP rd                                         *
*********************************************************/
void MFHI() { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.HI.UD[0]; } // Rd = Hi
void MFLO() { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.LO.UD[0]; } // Rd = Lo

/*********************************************************
* Move to GPR to HI/LO & Register jump                   *
* Format:  OP rs                                         *
*********************************************************/
void MTHI() { cpuRegs.HI.UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]; } // Hi = Rs
void MTLO() { cpuRegs.LO.UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]; } // Lo = Rs


/*********************************************************
* Shift arithmetic with constant shift                   *
* Format:  OP rd, rt, sa                                 *
*********************************************************/
void SRA()   { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s32)(cpuRegs.GPR.r[_Rt_].SL[0] >> _Sa_); } // Rd = Rt >> sa (arithmetic)
void SRL()   { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s32)(cpuRegs.GPR.r[_Rt_].UL[0] >> _Sa_); } // Rd = Rt >> sa (logical) [sign extend!!]
void SLL()   { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s32)(cpuRegs.GPR.r[_Rt_].UL[0] << _Sa_); } // Rd = Rt << sa
void DSLL()  { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = (u64)(cpuRegs.GPR.r[_Rt_].UD[0] << _Sa_); }
void DSLL32(){ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = (u64)(cpuRegs.GPR.r[_Rt_].UD[0] << (_Sa_+32));}
void DSRA()  { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = cpuRegs.GPR.r[_Rt_].SD[0] >> _Sa_; }
void DSRA32(){ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = cpuRegs.GPR.r[_Rt_].SD[0] >> (_Sa_+32);}
void DSRL()  { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rt_].UD[0] >> _Sa_; }
void DSRL32(){ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rt_].UD[0] >> (_Sa_+32);}

/*********************************************************
* Shift arithmetic with variant register shift           *
* Format:  OP rd, rt, rs                                 *
*********************************************************/
void SLLV() { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s32)(cpuRegs.GPR.r[_Rt_].UL[0] << (cpuRegs.GPR.r[_Rs_].UL[0] &0x1f));} // Rd = Rt << rs
void SRAV() { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s32)(cpuRegs.GPR.r[_Rt_].SL[0] >> (cpuRegs.GPR.r[_Rs_].UL[0] &0x1f));} // Rd = Rt >> rs (arithmetic)
void SRLV() { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s32)(cpuRegs.GPR.r[_Rt_].UL[0] >> (cpuRegs.GPR.r[_Rs_].UL[0] &0x1f));} // Rd = Rt >> rs (logical)
void DSLLV(){ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = (u64)(cpuRegs.GPR.r[_Rt_].UD[0] << (cpuRegs.GPR.r[_Rs_].UL[0] &0x3f));}
void DSRAV(){ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s64)(cpuRegs.GPR.r[_Rt_].SD[0] >> (cpuRegs.GPR.r[_Rs_].UL[0] &0x3f));}
void DSRLV(){ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = (u64)(cpuRegs.GPR.r[_Rt_].UD[0] >> (cpuRegs.GPR.r[_Rs_].UL[0] &0x3f));}

/*********************************************************
* Load and store for GPR                                 *
* Format:  OP rt, offset(base)                           *
*********************************************************/

// Implementation Notes Regarding Memory Operations:
//  * It it 'correct' to do all loads into temp variables, even if the destination GPR
//    is the zero reg (which nullifies the result).  The memory needs to be accessed
//    regardless so that hardware registers behave as expected (some clear on read) and
//    so that TLB Misses are handled as expected as well.
//
//  * Low/High varieties of instructions, such as LWL/LWH, do *not* raise Address Error
//    exceptions, since the lower bits of the address are used to determine the portions
//    of the address/register operations.

__noinline static void RaiseAddressError(u32 addr, bool store)
{
	const std::string message(fmt::format("Address Error, addr=0x{:x} [{}]", addr, store ? "store" : "load"));

	// TODO: This doesn't actually get raised in the CPU yet.
	Console.Error(message);

	Cpu->CancelInstruction();
}

void LB()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	s8 temp = memRead8(addr);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = temp;
}

void LBU()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u8 temp = memRead8(addr);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = temp;
}

void LH()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 1) [[unlikely]]
		RaiseAddressError(addr, false);

	s16 temp = memRead16(addr);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = temp;
}

void LHU()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 1) [[unlikely]]
		RaiseAddressError(addr, false);

	u16 temp = memRead16(addr);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = temp;
}

void LW()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 3) [[unlikely]]
		RaiseAddressError(addr, false);

	u32 temp = memRead32(addr);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = (s32)temp;
}

void LWU()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 3) [[unlikely]]
		RaiseAddressError(addr, false);

	u32 temp = memRead32(addr);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = temp;
}

static const u32 LWL_MASK[4] = { 0xffffff, 0x0000ffff, 0x000000ff, 0x00000000 };
static const u32 LWR_MASK[4] = { 0x000000, 0xff000000, 0xffff0000, 0xffffff00 };
static const u8 LWL_SHIFT[4] = { 24, 16, 8, 0 };
static const u8 LWR_SHIFT[4] = { 0, 8, 16, 24 };

void LWL()
{
	s32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 3;

	u32 mem = memRead32(addr & ~3);

	if (!_Rt_) return;

	// ensure the compiler does correct sign extension into 64 bits by using s32
	cpuRegs.GPR.r[_Rt_].SD[0] =	(s32)((cpuRegs.GPR.r[_Rt_].UL[0] & LWL_MASK[shift]) |
								(mem << LWL_SHIFT[shift]));

	/*
	Mem = 1234.  Reg = abcd
	(result is always sign extended into the upper 32 bits of the Rt)

	0   4bcd   (mem << 24) | (reg & 0x00ffffff)
	1   34cd   (mem << 16) | (reg & 0x0000ffff)
	2   234d   (mem <<  8) | (reg & 0x000000ff)
	3   1234   (mem      ) | (reg & 0x00000000)
	*/
}

void LWR()
{
	s32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 3;

	u32 mem = memRead32(addr & ~3);

	if (!_Rt_) return;

	// Use unsigned math here, and conditionally sign extend below, when needed.
	mem = (cpuRegs.GPR.r[_Rt_].UL[0] & LWR_MASK[shift]) | (mem >> LWR_SHIFT[shift]);

	if( shift == 0 )
	{
		// This special case requires sign extension into the full 64 bit dest.
		cpuRegs.GPR.r[_Rt_].SD[0] =	(s32)mem;
	}
	else
	{
		// This case sets the lower 32 bits of the target register.  Upper
		// 32 bits are always preserved.
		cpuRegs.GPR.r[_Rt_].UL[0] =	mem;
	}

	/*
	Mem = 1234.  Reg = abcd

	0   1234   (mem      ) | (reg & 0x00000000)	[sign extend into upper 32 bits!]
	1   a123   (mem >>  8) | (reg & 0xff000000)
	2   ab12   (mem >> 16) | (reg & 0xffff0000)
	3   abc1   (mem >> 24) | (reg & 0xffffff00)
	*/
}

// dummy variable used as a destination address for writes to the zero register, so
// that the zero register always stays zero.
alignas(16) static GPR_reg m_dummy_gpr_zero;

// Returns the x86 address of the requested GPR, which is safe for writing. (includes
// special handling for returning a dummy var for GPR0(zero), so that it's value is
// always preserved)
static GPR_reg* gpr_GetWritePtr( uint gpr )
{
	return (( gpr == 0 ) ? &m_dummy_gpr_zero : &cpuRegs.GPR.r[gpr]);
}

void LD()
{
    s32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 7) [[unlikely]]
		RaiseAddressError(addr, false);

	cpuRegs.GPR.r[_Rt_].UD[0] = memRead64(addr);
}

static const u64 LDL_MASK[8] =
{	0x00ffffffffffffffULL, 0x0000ffffffffffffULL, 0x000000ffffffffffULL, 0x00000000ffffffffULL,
	0x0000000000ffffffULL, 0x000000000000ffffULL, 0x00000000000000ffULL, 0x0000000000000000ULL
};
static const u64 LDR_MASK[8] =
{	0x0000000000000000ULL, 0xff00000000000000ULL, 0xffff000000000000ULL, 0xffffff0000000000ULL,
	0xffffffff00000000ULL, 0xffffffffff000000ULL, 0xffffffffffff0000ULL, 0xffffffffffffff00ULL
};

static const u8 LDR_SHIFT[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
static const u8 LDL_SHIFT[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };


void LDL()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 7;

	u64 mem = memRead64(addr & ~7);

	if( !_Rt_ ) return;
	cpuRegs.GPR.r[_Rt_].UD[0] =	(cpuRegs.GPR.r[_Rt_].UD[0] & LDL_MASK[shift]) |
								(mem << LDL_SHIFT[shift]);
}

void LDR()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 7;

	u64 mem = memRead64(addr & ~7);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] =	(cpuRegs.GPR.r[_Rt_].UD[0] & LDR_MASK[shift]) |
								(mem >> LDR_SHIFT[shift]);
}

void LQ()
{
	// MIPS Note: LQ and SQ are special and "silently" align memory addresses, thus
	// an address error due to unaligned access isn't possible like it is on other loads/stores.

	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	memRead128(addr & ~0xf, (u128*)gpr_GetWritePtr(_Rt_));
}

void SB()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	memWrite8(addr, cpuRegs.GPR.r[_Rt_].UC[0]);
}

void SH()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 1) [[unlikely]]
		RaiseAddressError(addr, true);

	memWrite16(addr, cpuRegs.GPR.r[_Rt_].US[0]);
}

void SW()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 3) [[unlikely]]
		RaiseAddressError(addr, true);

  memWrite32(addr, cpuRegs.GPR.r[_Rt_].UL[0]);
}

static const u32 SWL_MASK[4] = { 0xffffff00, 0xffff0000, 0xff000000, 0x00000000 };
static const u32 SWR_MASK[4] = { 0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff };

static const u8 SWR_SHIFT[4] = { 0, 8, 16, 24 };
static const u8 SWL_SHIFT[4] = { 24, 16, 8, 0 };

void SWL()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 3;
	u32 mem = memRead32( addr & ~3 );

	memWrite32( addr & ~3,
		(cpuRegs.GPR.r[_Rt_].UL[0] >> SWL_SHIFT[shift]) |
		(mem & SWL_MASK[shift])
	);

	/*
	Mem = 1234.  Reg = abcd

	0   123a   (reg >> 24) | (mem & 0xffffff00)
	1   12ab   (reg >> 16) | (mem & 0xffff0000)
	2   1abc   (reg >>  8) | (mem & 0xff000000)
	3   abcd   (reg      ) | (mem & 0x00000000)
	*/
}

void SWR() {
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 3;
	u32 mem = memRead32(addr & ~3);

	memWrite32( addr & ~3,
		(cpuRegs.GPR.r[_Rt_].UL[0] << SWR_SHIFT[shift]) |
		(mem & SWR_MASK[shift])
	);

	/*
	Mem = 1234.  Reg = abcd

	0   abcd   (reg      ) | (mem & 0x00000000)
	1   bcd4   (reg <<  8) | (mem & 0x000000ff)
	2   cd34   (reg << 16) | (mem & 0x0000ffff)
	3   d234   (reg << 24) | (mem & 0x00ffffff)
	*/
}

void SD()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 7) [[unlikely]]
		RaiseAddressError(addr, true);

    memWrite64(addr,cpuRegs.GPR.r[_Rt_].UD[0]);
}

static const u64 SDL_MASK[8] =
{	0xffffffffffffff00ULL, 0xffffffffffff0000ULL, 0xffffffffff000000ULL, 0xffffffff00000000ULL,
	0xffffff0000000000ULL, 0xffff000000000000ULL, 0xff00000000000000ULL, 0x0000000000000000ULL
};
static const u64 SDR_MASK[8] =
{	0x0000000000000000ULL, 0x00000000000000ffULL, 0x000000000000ffffULL, 0x0000000000ffffffULL,
	0x00000000ffffffffULL, 0x000000ffffffffffULL, 0x0000ffffffffffffULL, 0x00ffffffffffffffULL
};

static const u8 SDL_SHIFT[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };
static const u8 SDR_SHIFT[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };

void SDL()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 7;
	u64 mem = memRead64(addr & ~7);
	mem = (cpuRegs.GPR.r[_Rt_].UD[0] >> SDL_SHIFT[shift]) |
		  (mem & SDL_MASK[shift]);
	memWrite64(addr & ~7, mem);
}


void SDR()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 7;
	u64 mem = memRead64(addr & ~7);
	mem = (cpuRegs.GPR.r[_Rt_].UD[0] << SDR_SHIFT[shift]) |
		  (mem & SDR_MASK[shift]);
	memWrite64(addr & ~7, mem );
}

void SQ()
{
	// MIPS Note: LQ and SQ are special and "silently" align memory addresses, thus
	// an address error due to unaligned access isn't possible like it is on other loads/stores.

	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	memWrite128(addr & ~0xf, cpuRegs.GPR.r[_Rt_].UQ);
}

/*********************************************************
* Conditional Move                                       *
* Format:  OP rd, rs, rt                                 *
*********************************************************/

void MOVZ() {
	if (!_Rd_) return;
	if (cpuRegs.GPR.r[_Rt_].UD[0] == 0) {
		cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0];
	}
}
void MOVN() {
	if (!_Rd_) return;
	if (cpuRegs.GPR.r[_Rt_].UD[0] != 0) {
		cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0];
	}
}
void MOVCI() {
	// MOVF (tf=0): if FPU CC == 0, rd = rs
	// MOVT (tf=1): if FPU CC != 0, rd = rs
	if (!_Rd_) return;
	const u32 tf = (cpuRegs.code >> 16) & 1;
	const bool cc = (fpuRegs.fprc[31] & 0x00800000) != 0;
	if (cc == (tf != 0))
		cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0];
}

/*********************************************************
* Special purpose instructions                           *
* Format:  OP                                            *
*********************************************************/


// This function is the only one that uses Sifcmd.h in Pcsx2.
#include "Sifcmd.h"

// [iter672] g_sifgetreg_track は名前空間外（ファイル先頭付近）に移動

void SYSCALL()
{
	u8 call;

	if (cpuRegs.GPR.n.v1.SL[0] < 0)
		call = (u8)(-cpuRegs.GPR.n.v1.SL[0]);
	else
		call = cpuRegs.GPR.n.v1.UC[0];

	BIOS_LOG("Bios call: %s (%x)", R5900::bios[call], call);

	// [R59] Update last syscall tracking globals + ring buffer
	g_last_syscall_pc = cpuRegs.pc;
	g_last_syscall_call = call;
	g_last_syscall_a0 = cpuRegs.GPR.n.a0.UL[0];
	g_last_syscall_ra = cpuRegs.GPR.n.ra.UL[0];
	g_last_syscall_cycle = cpuRegs.cycle;
	{
		auto& e = g_syscall_ring[g_syscall_ring_idx & (SYSCALL_RING_SIZE - 1)];
		e.pc = cpuRegs.pc; e.call = call;
		e.a0 = cpuRegs.GPR.n.a0.UL[0]; e.ra = cpuRegs.GPR.n.ra.UL[0];
		e.v1_raw = cpuRegs.GPR.n.v1.UL[0]; e.cycle = cpuRegs.cycle;
		g_syscall_ring_idx++;
	}

	// [R60] @@STUB_SNAPSHOT@@ capture stub code at 0x081fc0-0x082000 on first SleepThread call
	// Also capture when abnormal syscall number (call > 128) to compare
	// Removal condition: warm reboot stub corruptcauseafter identified
	{
		// [R60] @@STUB_GUARD_SC@@ Check and restore stub before EVERY SleepThread call
		// This catches corruption that happens between vsync checks (< 1000 cycles before SYSCALL)
		if (eeMem) {
			u32 stub_fec = *(u32*)(eeMem->Main + 0x081fec);
			// Save stub on first correct observation
			static u8 s_sc_saved_stub[20] = {};
			static bool s_sc_stub_saved = false;
			if (!s_sc_stub_saved && stub_fec == 0x2403fffb) {
				std::memcpy(s_sc_saved_stub, eeMem->Main + 0x081fe0, 20);
				s_sc_stub_saved = true;
			}
			// Restore if corrupted (for ANY syscall, not just SleepThread)
			if (s_sc_stub_saved && stub_fec != 0x2403fffb) {
				std::memcpy(eeMem->Main + 0x081fe0, s_sc_saved_stub, 20);
				static int s_sc_restore_n = 0;
				if (s_sc_restore_n++ < 20) {
					Console.WriteLn("@@STUB_GUARD_SC@@ RESTORED stub before syscall call=%02x (was %08x, #%d) pc=%08x cycle=%u",
						(u32)call, stub_fec, s_sc_restore_n, cpuRegs.pc, cpuRegs.cycle);
				}
			}
		}
		static bool s_first_sleep_dumped = false;
		if (!s_first_sleep_dumped && call == 5 && cpuRegs.pc >= 0x081ff0 && cpuRegs.pc <= 0x081ff8) {
			s_first_sleep_dumped = true;
			if (eeMem) {
				for (u32 base = 0x081fc0; base < 0x082020; base += 0x20) {
					const u32* p = reinterpret_cast<const u32*>(eeMem->Main + base);
					Console.WriteLn("@@STUB_SNAPSHOT_GOOD@@ [0x%06x]: %08x %08x %08x %08x %08x %08x %08x %08x",
						base, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
				}
				Console.WriteLn("@@STUB_SNAPSHOT_GOOD@@ t0=%08x v1=%08x a0=%08x ra=%08x sp=%08x gp=%08x s0=%08x s1=%08x cycle=%u",
					cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.v1.UL[0],
					cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.ra.UL[0],
					cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.gp.UL[0],
					cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0],
					cpuRegs.cycle);
			}
		}
		// Dump on abnormal syscall (call > 128) — this is the BAD case
		if (call > 128 && cpuRegs.cycle > 100000000u) {
			static int s_bad_sc_n = 0;
			if (s_bad_sc_n++ < 3) {
				if (eeMem) {
					for (u32 base = 0x081fc0; base < 0x082020; base += 0x20) {
						const u32* p = reinterpret_cast<const u32*>(eeMem->Main + base);
						Console.WriteLn("@@STUB_SNAPSHOT_BAD@@ [0x%06x]: %08x %08x %08x %08x %08x %08x %08x %08x",
							base, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
					}
					Console.WriteLn("@@STUB_SNAPSHOT_BAD@@ t0=%08x v1=%08x a0=%08x ra=%08x sp=%08x gp=%08x s0=%08x s1=%08x cycle=%u epc=%08x",
						cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.v1.UL[0],
						cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.ra.UL[0],
						cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.gp.UL[0],
						cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0],
						cpuRegs.cycle, cpuRegs.CP0.r[14]);
				}
			}
		}
	}

	// [iter672] sceSifGetReg 追跡フラグ (reg 4 add)
	{
		if (call == 0x7A && cpuRegs.GPR.n.a0.UL[0] == 0x80000002u)
			g_sifgetreg_track = 1;
		else if (call == 0x7A && cpuRegs.GPR.n.a0.UL[0] == 4u)
			g_sifgetreg_track = 4; // reg 4 tracking
		else
			g_sifgetreg_track = 0;
	}

	// [iter103] @@EE_SYSCALL_TRACE@@ – log first 200 non-SIF SYSCALLs (skip 0x3c/0x3d + sceSifGetReg polling)
	// [iter674] @@SYSCALL_UNFILTERED@@ n=8-20 のフィルタなし全 SYSCALL ログ
	{
		static u32 s_sc_all = 0;
		++s_sc_all;
		if (s_sc_all >= 120 && s_sc_all <= 155) {
			const char* mode2 = (Cpu != &intCpu) ? "JIT" : "Interp";
			// [iter675] bufferバイト追跡: *MEM[0x93698] ポインタと先頭バイト
			u32 buf_ptr = *(u32*)(eeMem->Main + 0x93698u);
			u8 buf_byte = 0;
			u32 buf_w0 = 0;
			if (buf_ptr) {
				u32 bp = buf_ptr & 0x01FFFFFFu;
				if (bp + 4 <= 0x02000000u) {
					buf_byte = *(u8*)(eeMem->Main + bp);
					buf_w0 = *(u32*)(eeMem->Main + bp);
				}
			}
			// Also check 0x93880 (2nd DMA buffer)
			u8  b93880 = *(u8*)(eeMem->Main + 0x93880u);
			u32 w93880 = *(u32*)(eeMem->Main + 0x93880u);
			Console.WriteLn("@@SYSCALL_UNFILTERED@@ [%s] N=%u pc=%08x call=%02x a0=%08x v0=%08x ra=%08x eecyc=%u iopcyc=%u b680=%02x b880=%02x w880=%08x",
				mode2, s_sc_all, cpuRegs.pc, (u32)call,
				cpuRegs.GPR.n.a0.UL[0],
				cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.ra.UL[0],
				cpuRegs.cycle, psxRegs.cycle,
				*(u8*)(eeMem->Main + 0x93680u), b93880, w93880);
		}
	}
	// [R59] @@EXECPS2_WARMREBOOT@@ ExecPS2(call=0x07) always log — warm reboot 後のエントリポイント追跡
	// Removal condition: 0x22000000 ジャンプ元after identified
	if (call == 0x07) {
		static u32 s_execps2_n = 0;
		if (s_execps2_n++ < 30) {
			const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
			Console.WriteLn("@@EXECPS2_WARMREBOOT@@ [%s] n=%u pc=%08x a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x sp=%08x cycle=%u",
				mode, s_execps2_n, cpuRegs.pc,
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
				cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.a3.UL[0],
				cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.sp.UL[0],
				cpuRegs.cycle);
		}

		// [R103 FIX] Clear JIT cache on ExecPS2 warm reboot.
		// Root cause: EELOAD/ExecPS2 loads new code into EE RAM via DMA/memcpy
		// which bypasses the vtlb slow path (no recClear). The JIT cache retains
		// stale compiled blocks for the overwritten code regions.
		// [P36 FIX] ExecPS2 のターゲットaddress周辺の JIT キャッシュをクリア。
		// ゲーム ELF が 0x00100000+ にロードされた後、古い PS2LOGO/OSDSYS の
		// JIT ブロックが実行されるのを防ぐ。a0 がエントリポイント。
		{
			const u32 entry = cpuRegs.GPR.n.a0.UL[0];
			const u32 hw_entry = entry & 0x1FFFFFFFu;
			// Dump first 256 bytes at entry point to verify code content
			if (eeMem && hw_entry + 0x100 < 0x02000000u) {
				const u32* code = reinterpret_cast<const u32*>(eeMem->Main + hw_entry);
				for (u32 off = 0; off < 0x100; off += 0x20) {
					Console.WriteLn("@@EXECPS2_CODE@@ [%08x+%02x]: %08x %08x %08x %08x %08x %08x %08x %08x",
						entry, off,
						code[off/4+0], code[off/4+1], code[off/4+2], code[off/4+3],
						code[off/4+4], code[off/4+5], code[off/4+6], code[off/4+7]);
				}
			}
			// Clear 2MB from the entry point
			if (Cpu != &intCpu) {
				const u32 clear_size = 0x200000u / 4u;
				const u32 clear_start = (hw_entry < 0x100000u) ? 0u : (hw_entry - 0x100000u);
				Console.WriteLn("@@EXECPS2_JITCLEAR@@ entry=%08x clear=[%08x..%08x]",
					entry, clear_start, clear_start + clear_size * 4);
				Cpu->Clear(clear_start, clear_size);
			}
		}

	}

	// [TEMP_DIAG] 2nd boot syscall trace — ゲーム2回目boot後の全SYSCALL
	// Removal condition: 2回目bootの INTC_MASK divergence 解決後
	{
		static u32 s_execps2_count_local = 0;
		static bool s_2nd_boot_started_local = false;
		if (call == 0x07) s_execps2_count_local++;
		// ExecPS2 が5回目 (2nd boot reload) 以降をキャプチャ
		if (s_execps2_count_local >= 5) s_2nd_boot_started_local = true;
		if (s_2nd_boot_started_local) {
			static u32 s_2nd_n = 0;
			static u32 s_reg4_poll_n = 0;
			bool is_reg4_poll = (call == 0x7a && cpuRegs.GPR.n.a0.UL[0] == 4);
			// [P37] sceSifGetReg(4) polling counter + diagnostic (H2 removed)
			if (is_reg4_poll) {
				s_reg4_poll_n++;
				if (s_reg4_poll_n == 1 || s_reg4_poll_n == 10 || s_reg4_poll_n == 100
					|| s_reg4_poll_n == 1000 || s_reg4_poll_n % 10000 == 0)
					Console.WriteLn("@@REG4_POLL@@ n=%u pc=%08x v0=%08x reg4_mem=%08x f230=%08x intc=%08x/%08x iopcyc=%u cycle=%u",
						s_reg4_poll_n, cpuRegs.pc, cpuRegs.GPR.n.v0.UL[0],
						eeMem ? *(u32*)(eeMem->Main + 0x1A764) : 0xDEADu,
						psHu32(SBUS_F230), psHu32(INTC_STAT), psHu32(INTC_MASK),
						psxRegs.cycle, cpuRegs.cycle);
			}
			if (!is_reg4_poll && s_2nd_n++ < 500) {
				Console.WriteLn("@@2ND_BOOT_SYSCALL@@ n=%u pc=%08x call=%02x(%s) a0=%08x a1=%08x v0=%08x ra=%08x cycle=%u intc_mask=%04x",
					s_2nd_n, cpuRegs.pc, (u32)call, (call < 256 ? R5900::bios[call] : "?"),
					cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.ra.UL[0],
					cpuRegs.cycle, psHu32(INTC_MASK) & 0xFFFF);
			}
		}
	}
	// [R59] @@SYSCALL_WARMREBOOT@@ warm reboot 後(frame>850)の全 SYSCALL トレース
	// Removal condition: warm reboot 後の kernel init divergence after identified
	{
		// Use cpuRegs.cycle as warm reboot indicator (cycle > 180000000 ≈ post-reboot)
		if (cpuRegs.cycle > 180000000u) {
			static u32 s_wr_sc_n = 0;
			if (s_wr_sc_n++ < 50) {
				const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
				Console.WriteLn("@@SYSCALL_WARMREBOOT@@ [%s] n=%u pc=%08x call=%02x(%s) a0=%08x a1=%08x v0=%08x v1=%08x ra=%08x sp=%08x cycle=%u",
					mode, s_wr_sc_n, cpuRegs.pc,
					(u32)call, (call < 256 ? R5900::bios[call] : "?"),
					cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
					cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.sp.UL[0],
					cpuRegs.cycle);
			}
		}
	}

	// [FIX] sceSifIopReset IOPRP path capture — IOP に SIF DMA で届かないパスをsave
	// sceSifStopDma (0x6b) 直後の sceSifSetDma (0x77) データをダンプ
	{
		static bool s_after_sif_stop = false;
		if (call == 0x6b) s_after_sif_stop = true;
		if (call == 0x77 && s_after_sif_stop && eeMem) {
			s_after_sif_stop = false;
			u32 desc_addr = cpuRegs.GPR.n.a0.UL[0] & 0x01FFFFFFu;
			u32 n_xfer = cpuRegs.GPR.n.a1.UL[0];
			if (desc_addr + 16 <= Ps2MemSize::MainRam) {
				u32 src  = *(u32*)(eeMem->Main + desc_addr + 0);
				u32 dest = *(u32*)(eeMem->Main + desc_addr + 4);
				u32 size = *(u32*)(eeMem->Main + desc_addr + 8);
				u32 attr = *(u32*)(eeMem->Main + desc_addr + 12);
				Console.WriteLn("@@IOPRP_DMA@@ desc=%08x n=%u src=%08x dest=%08x size=%u attr=%08x",
					desc_addr, n_xfer, src, dest, size, attr);
				// Dump src data as string (IOPRP path)
				u32 src_phys = src & 0x01FFFFFFu;
				if (src_phys + 64 <= Ps2MemSize::MainRam) {
					char path[65] = {};
					std::memcpy(path, eeMem->Main + src_phys, 64);
					path[64] = 0;
					// Replace non-printable chars with dots
					for (int i = 0; i < 64; i++) {
						if (path[i] != 0 && (path[i] < 0x20 || path[i] > 0x7e)) path[i] = '.';
					}
					Console.WriteLn("@@IOPRP_DMA@@ src_data='%s'", path);
					// Also dump as hex
					const u32* hw = (const u32*)(eeMem->Main + src_phys);
					Console.WriteLn("@@IOPRP_DMA@@ src_hex[0-7]: %08x %08x %08x %08x %08x %08x %08x %08x",
						hw[0], hw[1], hw[2], hw[3], hw[4], hw[5], hw[6], hw[7]);
					Console.WriteLn("@@IOPRP_DMA@@ src_hex[8-15]: %08x %08x %08x %08x %08x %08x %08x %08x",
						hw[8], hw[9], hw[10], hw[11], hw[12], hw[13], hw[14], hw[15]);
					// Also dump the string starting at offset 24 (the module path)
					char modpath[81] = {};
					std::memcpy(modpath, eeMem->Main + src_phys + 24, 80);
					modpath[80] = 0;
					for (int i = 0; i < 80; i++) {
						if (modpath[i] == 0) break;
						if (modpath[i] < 0x20 || modpath[i] > 0x7e) modpath[i] = '.';
					}
					Console.WriteLn("@@IOPRP_DMA@@ ioprp_path='%s'", modpath);
					// [FIX] cdrom0: IOPRP パスdetect → SIF_CMD_RESET パケットを IOP に直接コピー
				// root cause: sceSifSetDma の SIF1 DMA が IOP reboot 前に完了しない。
				// SIF FIFO にデータが到達する前に IOP がresetされ、sifcmd が
				// SIF_CMD_RESET を受信できない。
				// 対策: EE の DMA src data を IOP の dest address に直接 memcpy。
				// これにより IOP reboot 後の sifcmd が SIF FIFO/DMA を介さずにデータをhandling可能。
				// Removal condition: SIF DMA タイミングがfixされ自然にデータが到達するようになった後
					if (std::strstr(modpath, "cdrom0:") || std::strstr(modpath, "cdrom0\\")) {
						std::strncpy(g_ioprp_path, modpath, sizeof(g_ioprp_path) - 1);
						g_ioprp_path[sizeof(g_ioprp_path) - 1] = 0;
						g_ioprp_path_pending = true;
						Console.WriteLn("@@IOPRP_DETECTED@@ path='%s' src=%08x dest=%08x size=%u",
							g_ioprp_path, src_phys, dest, size);
						// SIF_CMD_INJECT disabled — SIF DMA は自然にbehaviorする。
						// UDNL の CDVD 読み取りがissueの本命。
					}
				}
			}
		}
		if (call != 0x6b && call != 0x77 && call != 0x7a && call != 0x79) s_after_sif_stop = false;
	}

	// [TEMP_DIAG] Exit/ExecPS2 全callキャプチャ — Removal condition: ゲーム終了causeafter identified
	if (call == 0x04 || call == 0x07 || call == 0x7b || call == 0x24) {
		Console.WriteLn("@@GAME_EXIT_TRACE@@ pc=%08x call=%02x(%s) a0=%08x a1=%08x ra=%08x cycle=%u",
			cpuRegs.pc, (u32)call, (call < 256 ? R5900::bios[call] : "?"),
			cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
			cpuRegs.GPR.n.ra.UL[0], cpuRegs.cycle);
	}

	// [TEMP_DIAG] @@EXIT_BACKTRACE@@ — ExitDeleteThread full backtrace during 1st boot
	// Purpose: identify function chain leading to early game exit in JIT (not seen in Interp)
	// Removal condition: 1st boot ExitDeleteThread root cause after identified
	if (call == 0x24 && cpuRegs.cycle > 50000000u && cpuRegs.cycle < 200000000u) {
		static int s_ebt_n = 0;
		if (s_ebt_n++ < 3) {
			const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
			u32 sp = cpuRegs.GPR.n.sp.UL[0];
			u32 sp_phys = sp & 0x01FFFFFFu;
			Console.WriteLn("@@EXIT_BACKTRACE@@ [%s] n=%d pc=%08x ra=%08x sp=%08x v0=%08x v1=%08x a0=%08x cycle=%u",
				mode, s_ebt_n, cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], sp,
				cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.cycle);
			Console.WriteLn("@@EXIT_BACKTRACE@@ [%s] s0-s7=%08x %08x %08x %08x %08x %08x %08x %08x fp=%08x gp=%08x",
				mode,
				cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0],
				cpuRegs.GPR.n.s2.UL[0], cpuRegs.GPR.n.s3.UL[0],
				cpuRegs.GPR.n.s4.UL[0], cpuRegs.GPR.n.s5.UL[0],
				cpuRegs.GPR.n.s6.UL[0], cpuRegs.GPR.n.s7.UL[0],
				cpuRegs.GPR.n.s8.UL[0], cpuRegs.GPR.n.gp.UL[0]);
			// Dump 512 bytes of stack for backtrace analysis
			if (eeMem && sp_phys + 512 < 0x02000000u) {
				const u32* stk = reinterpret_cast<const u32*>(eeMem->Main + sp_phys);
				for (int row = 0; row < 16; row++) {
					Console.WriteLn("@@EXIT_STACK@@ [sp+%03x=%08x]: %08x %08x %08x %08x %08x %08x %08x %08x",
						row*32, sp + row*32,
						stk[row*8+0], stk[row*8+1], stk[row*8+2], stk[row*8+3],
						stk[row*8+4], stk[row*8+5], stk[row*8+6], stk[row*8+7]);
				}
			}
			// Dump MIPS code around SYSCALL call site (pc-0x80 to pc+0x20)
			u32 pc_phys = cpuRegs.pc & 0x01FFFFFFu;
			if (eeMem && pc_phys >= 0x80 && pc_phys + 0x20 < 0x02000000u) {
				const u32* code = reinterpret_cast<const u32*>(eeMem->Main + pc_phys - 0x80);
				for (int row = 0; row < 5; row++) {
					u32 base = cpuRegs.pc - 0x80 + row * 0x20;
					Console.WriteLn("@@EXIT_CODE@@ [%08x]: %08x %08x %08x %08x %08x %08x %08x %08x",
						base, code[row*8+0], code[row*8+1], code[row*8+2], code[row*8+3],
						code[row*8+4], code[row*8+5], code[row*8+6], code[row*8+7]);
				}
			}
			// Dump MIPS code around caller (ra-0x40 to ra+0x40)
			u32 ra = cpuRegs.GPR.n.ra.UL[0];
			u32 ra_phys = ra & 0x01FFFFFFu;
			if (eeMem && ra_phys >= 0x40 && ra_phys + 0x40 < 0x02000000u) {
				const u32* racode = reinterpret_cast<const u32*>(eeMem->Main + ra_phys - 0x40);
				for (int row = 0; row < 4; row++) {
					u32 base = ra - 0x40 + row * 0x20;
					Console.WriteLn("@@EXIT_RA_CODE@@ [%08x]: %08x %08x %08x %08x %08x %08x %08x %08x",
						base, racode[row*8+0], racode[row*8+1], racode[row*8+2], racode[row*8+3],
						racode[row*8+4], racode[row*8+5], racode[row*8+6], racode[row*8+7]);
				}
			}
		}
	}


	// [P12] cap 30→200, sceSifGetReg(0x7a) reg=4 polling をskip
	if (call != 0x3c && call != 0x3d && !(call == 0x7a && cpuRegs.GPR.n.a0.UL[0] == 4)) {
		static u32 s_sc_n = 0;
		if (s_sc_n < 500) {
			Console.WriteLn("@@EE_SYSCALL_TRACE@@ n=%u pc=%08x call=%02x(%s) a0=%08x a1=%08x a2=%08x v0=%08x ra=%08x",
				s_sc_n, cpuRegs.pc,
				(u32)call, (call < 256 ? R5900::bios[call] : "?"),
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0], cpuRegs.GPR.n.a2.UL[0],
				cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.ra.UL[0]);
			// [iter657] @@SYSCALL_REGDUMP@@ first 5 SYSCALLs: full register state for JIT vs Interp comparison
			// Removal condition: EELOAD code path divergence root cause after identified
			if (s_sc_n < 5) {
				const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
				Console.WriteLn("@@SYSCALL_REGDUMP@@ [%s] n=%u v1=%08x a3=%08x t0=%08x t1=%08x t2=%08x t3=%08x",
					mode, s_sc_n,
					cpuRegs.GPR.n.v1.UL[0], cpuRegs.GPR.n.a3.UL[0],
					cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0],
					cpuRegs.GPR.n.t2.UL[0], cpuRegs.GPR.n.t3.UL[0]);
				Console.WriteLn("@@SYSCALL_REGDUMP@@ [%s] n=%u s0=%08x s1=%08x s2=%08x s3=%08x s4=%08x s5=%08x s6=%08x s7=%08x",
					mode, s_sc_n,
					cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0],
					cpuRegs.GPR.n.s2.UL[0], cpuRegs.GPR.n.s3.UL[0],
					cpuRegs.GPR.n.s4.UL[0], cpuRegs.GPR.n.s5.UL[0],
					cpuRegs.GPR.n.s6.UL[0], cpuRegs.GPR.n.s7.UL[0]);
				Console.WriteLn("@@SYSCALL_REGDUMP@@ [%s] n=%u sp=%08x ra=%08x gp=%08x fp=%08x hi=%08x lo=%08x status=%08x",
					mode, s_sc_n,
					cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.ra.UL[0],
					cpuRegs.GPR.n.gp.UL[0], cpuRegs.GPR.n.s0.UL[0], /* s8/fp not in struct, reuse s0 */
					cpuRegs.HI.UL[0], cpuRegs.LO.UL[0],
					cpuRegs.CP0.r[12]);
			}
			s_sc_n++;
		}
		// [iter674] @@SIF_DMA_STATE@@ sceSifSetDma/sceSifGetReg 時の SIF/DMA5 stateキャプチャ
		if (s_sc_n <= 15 && (call == 0x77 || call == 0x7A)) {
			const char* mode2 = (Cpu != &intCpu) ? "JIT" : "Interp";
			// SIF RPC buffer pointer at 0x93698
			const u32 buf_ptr = *(u32*)(eeMem->Main + (0x93698u & 0x01FFFFFFu));
			// DMA5 (SIF0 EE→IOP) registers
			const u32 d5_chcr = psHu32(0xC000);
			const u32 d5_madr = psHu32(0xC010);
			const u32 d5_qwc  = psHu32(0xC020);
			const u32 d_stat  = psHu32(0xE010);
			u32 buf_data[4] = {};
			if (buf_ptr && buf_ptr < 0x02000000u) {
				const u32 bp = buf_ptr & 0x01FFFFFFu;
				buf_data[0] = *(u32*)(eeMem->Main + bp);
				buf_data[1] = *(u32*)(eeMem->Main + bp + 4);
				buf_data[2] = *(u32*)(eeMem->Main + bp + 8);
				buf_data[3] = *(u32*)(eeMem->Main + bp + 12);
			}
			Console.WriteLn("@@SIF_DMA_STATE@@ [%s] n=%u call=%02x buf_ptr=%08x buf[0-3]=%08x %08x %08x %08x D5_CHCR=%08x MADR=%08x QWC=%08x DSTAT=%08x",
				mode2, s_sc_n - 1, (u32)call, buf_ptr,
				buf_data[0], buf_data[1], buf_data[2], buf_data[3],
				d5_chcr, d5_madr, d5_qwc, d_stat);
			// [iter674] DMA descriptor dump for sceSifSetDma
			if (call == 0x77) {
				const u32 dma_desc = cpuRegs.GPR.n.a0.UL[0] & 0x01FFFFFFu;
				if (dma_desc + 32 <= 0x02000000u) {
					Console.WriteLn("@@SIF_DMA_DESC@@ [%s] a0=%08x desc: %08x %08x %08x %08x %08x %08x %08x %08x",
						mode2, cpuRegs.GPR.n.a0.UL[0],
						*(u32*)(eeMem->Main + dma_desc), *(u32*)(eeMem->Main + dma_desc+4),
						*(u32*)(eeMem->Main + dma_desc+8), *(u32*)(eeMem->Main + dma_desc+12),
						*(u32*)(eeMem->Main + dma_desc+16), *(u32*)(eeMem->Main + dma_desc+20),
						*(u32*)(eeMem->Main + dma_desc+24), *(u32*)(eeMem->Main + dma_desc+28));
				}
				// [iter675] スタックフレーム ra ダンプ: call元特定用
				const u32 sp_val = cpuRegs.GPR.n.sp.UL[0] & 0x01FFFFFFu;
				if (sp_val + 0xA4 <= 0x02000000u) {
					// wrapper ra at sp+0x90 (wrapper frame starts at sp+0x90, ra saved at [0])
					u32 wrapper_ra = *(u32*)(eeMem->Main + sp_val + 0x90);
					// caller ra at sp+0x90+0x10 (wrapper frame=0x10, caller saved ra further up)
					u32 caller_sp = sp_val + 0x90 + 0x10; // caller's sp
					u32 caller_ra = (caller_sp + 0x30 <= 0x02000000u) ? *(u32*)(eeMem->Main + caller_sp + 0x30) : 0;
					Console.WriteLn("@@SIF_DMA_STACK@@ [%s] sp=%08x ra_cur=%08x stk80=%08x wrapper_ra=%08x caller_ra=%08x s0=%08x s3=%08x byte=%02x s1=%08x s2=%08x",
						mode2, cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.ra.UL[0],
						*(u32*)(eeMem->Main + sp_val + 0x80),
						wrapper_ra, caller_ra,
						cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s3.UL[0],
						*(u8*)(eeMem->Main + (cpuRegs.GPR.n.s3.UL[0] & 0x01FFFFFFu)),
						cpuRegs.GPR.n.s1.UL[0], cpuRegs.GPR.n.s2.UL[0]);
				}
			}
		}
	}
	// [iter675] @@SIF_DCHAIN_STATE@@ sceSifSetDChain 時のbufferstate + 全register
	if (call == 0x78) {
		static int s_dchain_n = 0;
		if (s_dchain_n < 3) {
			const char* mode3 = (Cpu != &intCpu) ? "JIT" : "Interp";
			u8  b93880 = *(u8*)(eeMem->Main + 0x93880u);
			u32 w93880 = *(u32*)(eeMem->Main + 0x93880u);
			u32 w93884 = *(u32*)(eeMem->Main + 0x93884u);
			u32 w93888 = *(u32*)(eeMem->Main + 0x93888u);
			u8  b93680 = *(u8*)(eeMem->Main + 0x93680u);
			Console.WriteLn("@@SIF_DCHAIN_STATE@@ [%s] n=%d ra=%08x sp=%08x buf93680=%02x buf93880=[%02x %08x %08x %08x]",
				mode3, s_dchain_n, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.sp.UL[0],
				b93680, b93880, w93880, w93884, w93888);
			Console.WriteLn("@@SIF_DCHAIN_REGS@@ [%s] n=%d v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x a3=%08x s0=%08x s1=%08x s2=%08x s3=%08x s4=%08x s5=%08x",
				mode3, s_dchain_n++,
				cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
				cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.a3.UL[0],
				cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0],
				cpuRegs.GPR.n.s2.UL[0], cpuRegs.GPR.n.s3.UL[0],
				cpuRegs.GPR.n.s4.UL[0], cpuRegs.GPR.n.s5.UL[0]);
		}
	}
	// [iter157] @@EE_EXECPS2_PROBE@@ – log first 5 LoadExecPS2(0x3c)/ExecPS2(0x3d) calls + filename bytes at a0
	// Removal condition: ExecPS2/LoadExecPS2 の引数 (filename ptr, argc, argv) およびcall頻度確定次第delete
	else {
		static u32 s_ep_n = 0;
		if (s_ep_n < 5) {
			const u32 fn_ptr = cpuRegs.GPR.n.a0.UL[0];
			Console.WriteLn("@@EE_EXECPS2_PROBE@@ n=%u call=%02x pc=%08x a0(fn_ptr)=%08x a1=%08x a2=%08x fn[0]=%08x fn[4]=%08x fn[8]=%08x fn[12]=%08x",
				s_ep_n++, (u32)call, cpuRegs.pc,
				fn_ptr, cpuRegs.GPR.n.a1.UL[0], cpuRegs.GPR.n.a2.UL[0],
				memRead32(fn_ptr+0), memRead32(fn_ptr+4),
				memRead32(fn_ptr+8), memRead32(fn_ptr+12));
		}
	}

	// [iter232] HLE ExecPS2: EELOAD が ExecPS2("rom0:OSDSYS") を呼んだ時に
	// OSDSYS を BiosRom から直接 EE RAM にロードする。
	// SIF DMA が完全停止 (DMAC_CTRL=0, sbus_f200=0) のため、
	// カーネルの通常パス (IOP 経由 ROM FS) は使用不可。
	// Removal condition: SIF DMA が正常behaviorするようになった後
	if (call == 0x3c || call == 0x3d) {
		const u32 fn_ptr = cpuRegs.GPR.n.a0.UL[0];
		char fn_buf[64] = {};
		if (fn_ptr < 0x02000000u) {
			for (int i = 0; i < 63; i++) {
				fn_buf[i] = static_cast<char>(memRead8(fn_ptr + i));
				if (!fn_buf[i]) break;
			}
		}


		// [iter239] PS2LOGO skip: disabled化 (P12)
		// 理由: PS2LOGO = Sony ロゴ + ディスプレイモジュール (0x260000) のロードを担当。
		// iter239 時点では JIT 6% 速度のためskipしていたが、現在 60fps のためnot needed。
		// skipすると PMODE=0x66 がconfigされず Sony ロゴがdisplayされない。
		// Removal condition: PS2LOGO ロードが安定してbehaviorafter confirmed、このコメントブロック自体をdelete
		if (std::strncmp(fn_buf, "rom0:PS2LOGO", 12) == 0) {
			static int s_logo_pass = 0;
			if (s_logo_pass++ < 3)
				Console.WriteLn("@@PS2LOGO_PASS@@ fn='%s' pc=%08x → allowing PS2LOGO to load (P12 fix)",
					fn_buf, cpuRegs.pc);
			// フォールスルーして自然なローディングパスを通す
		}
	}




	switch (static_cast<Syscall>(call))
	{
		case Syscall::SetGsCrt:
		{
			//Function "SetGsCrt(Interlace, Mode, Field)"
			//Useful for fetching information of interlace/video/field display parameters of the Graphics Synthesizer

			gsIsInterlaced = cpuRegs.GPR.n.a0.UL[0] & 1;
			bool gsIsFrameMode = cpuRegs.GPR.n.a2.UL[0] & 1;
			const char* inter = (gsIsInterlaced) ? "Interlaced" : "Progressive";
			const char* field = (gsIsFrameMode) ? "FRAME" : "FIELD";
			std::string mode;
			// Warning info might be incorrect!
			switch (cpuRegs.GPR.n.a1.UC[0])
			{
				case 0x0:
				case 0x2:
					mode = "NTSC 640x448 @ 59.940 (59.82)"; gsSetVideoMode(GS_VideoMode::NTSC); break;

				case 0x1:
				case 0x3:
					mode = "PAL  640x512 @ 50.000 (49.76)"; gsSetVideoMode(GS_VideoMode::PAL); break;

				case 0x1A: mode = "VESA 640x480 @ 59.940"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x1B: mode = "VESA 640x480 @ 72.809"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x1C: mode = "VESA 640x480 @ 75.000"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x1D: mode = "VESA 640x480 @ 85.008"; gsSetVideoMode(GS_VideoMode::VESA); break;

				case 0x2A: mode = "VESA 800x600 @ 56.250"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x2B: mode = "VESA 800x600 @ 60.317"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x2C: mode = "VESA 800x600 @ 72.188"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x2D: mode = "VESA 800x600 @ 75.000"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x2E: mode = "VESA 800x600 @ 85.061"; gsSetVideoMode(GS_VideoMode::VESA); break;

				case 0x3B: mode = "VESA 1024x768 @ 60.004"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x3C: mode = "VESA 1024x768 @ 70.069"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x3D: mode = "VESA 1024x768 @ 75.029"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x3E: mode = "VESA 1024x768 @ 84.997"; gsSetVideoMode(GS_VideoMode::VESA); break;

				case 0x4A: mode = "VESA 1280x1024 @ 63.981"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x4B: mode = "VESA 1280x1024 @ 79.976"; gsSetVideoMode(GS_VideoMode::VESA); break;

				case 0x50: mode = "SDTV   720x480 @ 59.94"; gsSetVideoMode(GS_VideoMode::SDTV_480P); break;
				case 0x51: mode = "HDTV 1920x1080 @ 60.00"; gsSetVideoMode(GS_VideoMode::HDTV_1080I); break;
				case 0x52: mode = "HDTV  1280x720 @ ??.???"; gsSetVideoMode(GS_VideoMode::HDTV_720P); break;
				case 0x53: mode = "SDTV   768x576 @ ??.???"; gsSetVideoMode(GS_VideoMode::SDTV_576P); break;
				case 0x54: mode = "HDTV 1920x1080 @ ??.???"; gsSetVideoMode(GS_VideoMode::HDTV_1080P); break;

				case 0x72:
				case 0x82:
					mode = "DVD NTSC 640x448 @ ??.???"; gsSetVideoMode(GS_VideoMode::DVD_NTSC); break;
				case 0x73:
				case 0x83:
					mode = "DVD PAL 720x480 @ ??.???"; gsSetVideoMode(GS_VideoMode::DVD_PAL); break;

				default:
					DevCon.Error("Mode %x is not supported. Report me upstream", cpuRegs.GPR.n.a1.UC[0]);
					gsSetVideoMode(GS_VideoMode::Unknown);
			}
			DevCon.Warning("Set GS CRTC configuration. %s %s (%s)",mode.c_str(), inter, field);
		}
		break;
		case Syscall::ExecPS2:
		{
			if (DebugInterface::getPauseOnEntry())
			{
				CBreakPoints::AddBreakPoint(BREAKPOINT_EE, cpuRegs.GPR.n.a0.UL[0], true);
				DebugInterface::setPauseOnEntry(false);
			}
		}
		break;
		case Syscall::RFU060:
			if (CHECK_EXTRAMEM && cpuRegs.GPR.n.a1.UL[0] == 0xFFFFFFFF)
			{
				cpuRegs.GPR.n.a1.UL[0] = Ps2MemSize::ExposedRam - cpuRegs.GPR.n.a2.SL[0];
			}
			break;
		case Syscall::SetOsdConfigParam:
			// The whole thing gets written back to BIOS memory, so it'll be in the right place, no need to continue HLEing
			AllowParams1 = true;
			break;
		case Syscall::GetOsdConfigParam:
			if (!NoOSD && !AllowParams1)
			{
				ReadOSDConfigParames();

				u32 memaddr = cpuRegs.GPR.n.a0.UL[0];

				memWrite32(memaddr, configParams1.UL[0]);
				
				// Call the set function, as we need to set this back to the BIOS storage position.
				if (cpuRegs.GPR.n.v1.SL[0] < 0)
					cpuRegs.GPR.n.v1.SL[0] = -Syscall::SetOsdConfigParam;
				else
					cpuRegs.GPR.n.v1.UC[0] = Syscall::SetOsdConfigParam;

				AllowParams1 = true;
			}
			break;
		case Syscall::SetOsdConfigParam2:
			if (!AllowParams2)
			{
				ReadOSDConfigParames();

				u32 memaddr = cpuRegs.GPR.n.a0.UL[0];
				u32 size = cpuRegs.GPR.n.a1.UL[0];
				u32 offset = cpuRegs.GPR.n.a2.UL[0];

				if (offset == 0 && size >= 4)
					AllowParams2 = true;

				for (u32 i = 0; i < size; i++)
				{
					if (offset >= 4)
						break;

					configParams2.UC[offset++] = memRead8(memaddr++);
				}
			}
			break;
		case Syscall::GetOsdConfigParam2:
			if (!NoOSD && !AllowParams2)
			{
				ReadOSDConfigParames();

				u32 memaddr = cpuRegs.GPR.n.a0.UL[0];
				u32 size = cpuRegs.GPR.n.a1.UL[0];
				u32 offset = cpuRegs.GPR.n.a2.UL[0];

				if (offset + size > 2)
					Console.Warning("Warning: GetOsdConfigParam2 Reading extended language/version configs, may be incorrect!");

				for (u32 i = 0; i < size; i++)
				{
					if (offset >= 4)
						memWrite8(memaddr++, 0);
					else
						memWrite8(memaddr++, configParams2.UC[offset++]);
				}
				return;
			}
			break;
		case Syscall::SetVTLBRefillHandler:
			DevCon.Warning("A tlb refill handler is set. New handler %x", (u32*)PSM(cpuRegs.GPR.n.a1.UL[0]));
			break;
		case Syscall::StartThread:
		case Syscall::ChangeThreadPriority:
		{
			if (CurrentBiosInformation.eeThreadListAddr == 0)
			{
				u32 offset = 0x0;
				// Suprisingly not that slow :)
				while (offset < 0x5000) // I find that the instructions are in between 0x4000 -> 0x5000
				{
					u32 addr = 0x80000000 + offset;
					const u32 inst1 = memRead32(addr);
					const u32 inst2 = memRead32(addr += 4);
					const u32 inst3 = memRead32(addr += 4);

					if (ThreadListInstructions[0] == inst1 && // sw v0,0x0(v0)
						ThreadListInstructions[1] == inst2 && // no-op
						ThreadListInstructions[2] == inst3) // no-op
					{
						// We've found the instruction pattern!
						// We (well, I) know that the thread address is always 0x8001 + the immediate of the 6th instruction from here
						const u32 op = memRead32(0x80000000 + offset + (sizeof(u32) * 6));
						CurrentBiosInformation.eeThreadListAddr = 0x80010000 + static_cast<u16>(op) - 8; // Subtract 8 because the address here is offset by 8.
						DevCon.WriteLn("BIOS: Successfully found the instruction pattern. Assuming the thread list is here: %0x", CurrentBiosInformation.eeThreadListAddr);
						break;
					}
					offset += 4;
				}
				if (!CurrentBiosInformation.eeThreadListAddr)
				{
					// We couldn't find the address
					CurrentBiosInformation.eeThreadListAddr = -1;
					// If you're here because a user has reported this message, this means that the instruction pattern is not present on their bios, or it is aligned weirdly.
					Console.Warning("BIOS Warning: Unable to get a thread list offset. The debugger thread and stack frame views will not be functional.");
				}
			}
		}
		break;
		case Syscall::sceSifSetDma:
			// The only thing this code is used for is the one log message, so don't execute it if we aren't logging bios messages.
			if (TraceActive(EE.Bios))
			{
				//struct t_sif_cmd_header	*hdr;
				//struct t_sif_rpc_bind *bind;
				//struct t_rpc_server_data *server;
				int n_transfer;
				u32 addr;
				//int sid;

				n_transfer = cpuRegs.GPR.n.a1.UL[0] - 1;
				if (n_transfer >= 0)
				{
					addr = cpuRegs.GPR.n.a0.UL[0] + n_transfer * sizeof(t_sif_dma_transfer);
					t_sif_dma_transfer* dmat = (t_sif_dma_transfer*)PSM(addr);

					BIOS_LOG("bios_%s: n_transfer=%d, size=%x, attr=%x, dest=%x, src=%x",
							R5900::bios[cpuRegs.GPR.n.v1.UC[0]], n_transfer,
							dmat->size, dmat->attr,
							dmat->dest, dmat->src);
				}
			}
			break;

		case Syscall::Deci2Call:
		{
			if (cpuRegs.GPR.n.a0.UL[0] == 0x10)
			{
				eeConLog(ShiftJIS_ConvertString((char*)PSM(memRead32(cpuRegs.GPR.n.a1.UL[0]))));
			}
			else
				__Deci2Call(cpuRegs.GPR.n.a0.UL[0], (u32*)PSM(cpuRegs.GPR.n.a1.UL[0]));

			break;
		}
		case Syscall::sysPrintOut:
		{
			if (cpuRegs.GPR.n.a0.UL[0] != 0)
			{
				// TODO: Only supports 7 format arguments. Need to read from the stack for more.
				// Is there a function which collects PS2 arguments?
				char* fmt = (char*)PSM(cpuRegs.GPR.n.a0.UL[0]);

				u64 regs[7] = {
					cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.GPR.n.a2.UL[0],
					cpuRegs.GPR.n.a3.UL[0],
					cpuRegs.GPR.n.t0.UL[0],
					cpuRegs.GPR.n.t1.UL[0],
					cpuRegs.GPR.n.t2.UL[0],
					cpuRegs.GPR.n.t3.UL[0],
				};

				// Pretty much what this does is find instances of string arguments and remaps them.
				// Instead of the addresse(s) being relative to the PS2 address space, make them relative to program memory.
				// (This fixes issue #2865)
				int curRegArg = 0;
				for (int i = 0; 1; i++)
				{
					if (fmt[i] == '\0')
						break;

					if (fmt[i] == '%')
					{
						// The extra check here is to be compatible with "%%s"
						if (i == 0 || fmt[i - 1] != '%') {
							if (fmt[i + 1] == 's') {
								regs[curRegArg] = (u64)PSM(regs[curRegArg]); // PS2 Address -> PCSX2 Address
							}
							curRegArg++;
						}
					}
				}
				char buf[2048];
				snprintf(buf, sizeof(buf), fmt,
					regs[0],
					regs[1],
					regs[2],
					regs[3],
					regs[4],
					regs[5],
					regs[6]
				);

				eeConLog(buf);
			}
			break;
		}
		case Syscall::GetMemorySize:
			if (CHECK_EXTRAMEM)
			{
				cpuRegs.GPR.n.v0.UL[0] = Ps2MemSize::ExposedRam;
				return;
			}
			break;


		default:
			break;
	}


	// [iter681] FIX: FlushCache (SYSCALL 0x64) must clear all EE JIT blocks for I-cache flush.
	// Without this, blocks compiled from all-zero memory (before kernel code copy)
	// remain in the JIT cache as NOP sleds. When the BIOS later calls these functions
	// (e.g. sceSifSetDma at 0x80006410), the stale NOP block executes instead of
	// the real code, preventing SIF DMA tag setup and BIOS browser display.
	// [R60_FIX] Only clear JIT for I-cache operations (a0 & 2). D-cache writeback (a0==0)
	// does not affect compiled code and was causing 60 unnecessary full-RAM JIT clears per 30 vsyncs.
	if (call == 0x64 || call == 0x68) { // FlushCache (0x64) or iFlushCache (0x68)
		const u32 fc_mode = cpuRegs.GPR.n.a0.UL[0];
		static int s_flush_n = 0;
		if (s_flush_n < 10)
			Console.WriteLn("@@FLUSHCACHE_JIT_CLEAR@@ n=%d call=%02x a0=%08x cycle=%u %s",
				s_flush_n++, (u32)call, fc_mode, cpuRegs.cycle,
				(fc_mode & 2) ? "ICACHE_CLEAR" : "DCACHE_SKIP");
		if (fc_mode & 2) { // bit 1 = I-cache invalidation
			Cpu->Clear(0, Ps2MemSize::MainRam / 4);
		}
	}

	// [P15] @@GAME_SYSCALL_LATE@@ ゲームフェーズ (cycle>500M) の全 SYSCALL を記録
	// 目的: ゲームメインloop段階で何の SYSCALL がoccurしているか全体像を把握
	// Removal condition: VIF1 DMA draws>0 達成後
	{
		if (cpuRegs.cycle > 500000000u && call != 0x05) { // skip SleepThread(RFU005)
			static int s_glate_n = 0;
			if (s_glate_n < 100) {
				Console.WriteLn("@@GAME_SYSCALL_LATE@@ n=%d call=%02x(%s) a0=%08x a1=%08x pc=%08x ra=%08x cycle=%u",
					s_glate_n,
					(u32)call, (call < 256 ? R5900::bios[call] : "?"),
					cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.cycle);
				// [P15] sceSifSetDma 記述子ダンプ — VSync RPC の内容を特定
				if (call == 0x77) {
					static int s_sif_dump_n = 0;
					if (s_sif_dump_n++ < 5) {
						const u32 desc = cpuRegs.GPR.n.a0.UL[0] & 0x01FFFFFFu;
						if (desc + 32 <= 0x02000000u && eeMem) {
							Console.WriteLn("@@GAME_SIF_DESC@@ n=%d desc: %08x %08x %08x %08x %08x %08x %08x %08x",
								s_glate_n,
								*(u32*)(eeMem->Main + desc + 0), *(u32*)(eeMem->Main + desc + 4),
								*(u32*)(eeMem->Main + desc + 8), *(u32*)(eeMem->Main + desc + 12),
								*(u32*)(eeMem->Main + desc + 16), *(u32*)(eeMem->Main + desc + 20),
								*(u32*)(eeMem->Main + desc + 24), *(u32*)(eeMem->Main + desc + 28));
							// RPC data buffer: desc[0]=src_addr (EE) → dump first 64 bytes
							const u32 src = *(u32*)(eeMem->Main + desc) & 0x01FFFFFFu;
							if (src + 64 <= 0x02000000u) {
								Console.WriteLn("@@GAME_SIF_DATA@@ n=%d src=%08x: %08x %08x %08x %08x %08x %08x %08x %08x",
									s_glate_n, *(u32*)(eeMem->Main + desc),
									*(u32*)(eeMem->Main + src + 0), *(u32*)(eeMem->Main + src + 4),
									*(u32*)(eeMem->Main + src + 8), *(u32*)(eeMem->Main + src + 12),
									*(u32*)(eeMem->Main + src + 16), *(u32*)(eeMem->Main + src + 20),
									*(u32*)(eeMem->Main + src + 24), *(u32*)(eeMem->Main + src + 28));
								Console.WriteLn("@@GAME_SIF_DATA2@@ n=%d +32: %08x %08x %08x %08x %08x %08x %08x %08x",
									s_glate_n,
									*(u32*)(eeMem->Main + src + 32), *(u32*)(eeMem->Main + src + 36),
									*(u32*)(eeMem->Main + src + 40), *(u32*)(eeMem->Main + src + 44),
									*(u32*)(eeMem->Main + src + 48), *(u32*)(eeMem->Main + src + 52),
									*(u32*)(eeMem->Main + src + 56), *(u32*)(eeMem->Main + src + 60));
							}
						}
					}
				}
				s_glate_n++;
			}
		}
	}

	cpuRegs.pc -= 4;
	cpuException(0x20, cpuRegs.branch);
}

void BREAK() {
	cpuRegs.pc -= 4;
	cpuException(0x24, cpuRegs.branch);
}

void MFSA() {
	if (!_Rd_) return;
	cpuRegs.GPR.r[_Rd_].UD[0] = (u64)cpuRegs.sa;
}

void MTSA() {
	cpuRegs.sa = (u32)cpuRegs.GPR.r[_Rs_].UD[0];
}

// SNY supports three basic modes, two which synchronize memory accesses (related
// to the cache) and one which synchronizes the instruction pipeline (effectively
// a stall in either case).  Our emulation model does not track EE-side pipeline
// status or stalls, nor does it implement the CACHE.  Thus SYNC need do nothing.
void SYNC()
{
}

// Used to prefetch data into the EE's cache, or schedule a dirty write-back.
// CACHE is not emulated at this time (nor is there any need to emulate it), so
// this function does nothing in the context of our emulator.
void PREF()
{
}

static void trap(u16 code=0)
{
	cpuRegs.pc -= 4;
	// [iter197] Trap 命令診断: insn encoding と rs/rt 値
	// Removal condition: 0x001588c8 Trap causeafter determined
	{
		static int s_trap_diag = 0;
		if (s_trap_diag < 5) {
			s_trap_diag++;
			const u32 insn = memRead32(cpuRegs.pc);
			const u32 rs_idx = (insn >> 21) & 0x1Fu;
			const u32 rt_idx = (insn >> 16) & 0x1Fu;
			Console.WriteLn("@@TRAP_DIAG@@ n=%d pc=%08x insn=%08x rs[%u]=%08x_%08x rt[%u]=%08x_%08x v0=%08x",
				s_trap_diag, cpuRegs.pc, insn,
				rs_idx, cpuRegs.GPR.r[rs_idx].UL[1], cpuRegs.GPR.r[rs_idx].UL[0],
				rt_idx, cpuRegs.GPR.r[rt_idx].UL[1], cpuRegs.GPR.r[rt_idx].UL[0],
				cpuRegs.GPR.n.v0.UL[0]);
		}
	}
	// [iter209] TRAP_NOP disabled化: 自然bootフロー移行（OSDSYS 非 HLE bootのためnot needed）
	// [iter197] cap trap log to 50 to prevent 30000+ line flood
	{
		static int s_trap_warn = 0;
		if (s_trap_warn < 50) {
			s_trap_warn++;
			Console.Warning("Trap exception at 0x%08x", cpuRegs.pc);
		}
	}
	cpuException(0x34, cpuRegs.branch);
}

/*********************************************************
* Register trap                                          *
* Format:  OP rs, rt                                     *
*********************************************************/
void TGE()  { if (cpuRegs.GPR.r[_Rs_].SD[0] >= cpuRegs.GPR.r[_Rt_].SD[0]) trap(_TrapCode_); }
void TGEU() { if (cpuRegs.GPR.r[_Rs_].UD[0] >= cpuRegs.GPR.r[_Rt_].UD[0]) trap(_TrapCode_); }
void TLT()  { if (cpuRegs.GPR.r[_Rs_].SD[0] <  cpuRegs.GPR.r[_Rt_].SD[0]) trap(_TrapCode_); }
void TLTU() { if (cpuRegs.GPR.r[_Rs_].UD[0] <  cpuRegs.GPR.r[_Rt_].UD[0]) trap(_TrapCode_); }
void TEQ()  { if (cpuRegs.GPR.r[_Rs_].SD[0] == cpuRegs.GPR.r[_Rt_].SD[0]) trap(_TrapCode_); }
void TNE()  { if (cpuRegs.GPR.r[_Rs_].SD[0] != cpuRegs.GPR.r[_Rt_].SD[0]) trap(_TrapCode_); }

/*********************************************************
* Trap with immediate operand                            *
* Format:  OP rs, rt                                     *
*********************************************************/
void TGEI()  { if (cpuRegs.GPR.r[_Rs_].SD[0] >= _Imm_) trap(); }
void TLTI()  { if (cpuRegs.GPR.r[_Rs_].SD[0] <  _Imm_) trap(); }
void TEQI()  { if (cpuRegs.GPR.r[_Rs_].SD[0] == _Imm_) trap(); }
void TNEI()  { if (cpuRegs.GPR.r[_Rs_].SD[0] != _Imm_) trap(); }
void TGEIU() { if (cpuRegs.GPR.r[_Rs_].UD[0] >= (u64)_Imm_) trap(); }
void TLTIU() { if (cpuRegs.GPR.r[_Rs_].UD[0] <  (u64)_Imm_) trap(); }

/*********************************************************
* Sa intructions                                         *
* Format:  OP rs, rt                                     *
*********************************************************/

void MTSAB() {
	cpuRegs.sa = ((cpuRegs.GPR.r[_Rs_].UL[0] & 0xF) ^ (_Imm_ & 0xF));
}

void MTSAH() {
	cpuRegs.sa = ((cpuRegs.GPR.r[_Rs_].UL[0] & 0x7) ^ (_Imm_ & 0x7)) << 1;
}

} }	} // end namespace R5900::Interpreter::OpcodeImpl
