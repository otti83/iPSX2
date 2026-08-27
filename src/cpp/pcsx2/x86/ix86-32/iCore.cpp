// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "R3000A.h"
#include "VU.h"
#include "Vif.h"
#include "x86/iR3000A.h"
#include "x86/iR5900.h"

#include "common/Console.h"
#include "common/emitter/x86emitter.h"
#include <cstdlib>

#if !defined(__ANDROID__)
using namespace x86Emitter;
#endif

// yay sloppy crap needed until we can remove dependency on this hippopotamic
// landmass of shared code. (air)
extern u32 g_psxConstRegs[32];

// ==========================================
// PHASE 2: Reserved register detection
// ==========================================
#ifdef PCSX2_DEVBUILD
static void AssertValidPhysGpr(int phys, const char* context, u32 pc = 0)
{
    // Check for reserved registers (x25, x27, x28, x29, x30, x31)
    // These are used for: RFASTMEMBASE, RSTATE_CPU, RSTATE_PSX, recLUT, LR, SP
    if (phys == 25 || phys == 27 || phys == 28 || phys == 29 || phys >= 30) {
        Console.Error("@@REG_RESERVED@@ phys=%d context=%s pc=%08x", phys, context, pc);
        pxFailRel("Reserved ARM64 register accessed! See @@REG_RESERVED@@ log.");
    }
    
    // Check for clearly invalid values (too low or negative)
#if defined(__aarch64__) && defined(__APPLE__)
    if (phys >= 0 && phys < 19) {
        // This could be a value that wasn't allocated by _isAllocatableX86reg
        // Log for debugging but don't crash - it might be a scratch register
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            Console.Warning("@@REG_LOW@@ phys=%d in %s pc=%08x (expected 19-26, could be scratch)", phys, context, pc);
        }
    }
#endif
}
#else
#define AssertValidPhysGpr(phys, context, ...) ((void)0)
#endif

// X86 caching

static uint g_x86checknext;

static bool IsRegallocPanicFlushEnabled()
{
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		s_enabled = 1;
		Console.WriteLn("@@CFG@@ iPSX2_REGALLOC_PANIC_FLUSH=%d", s_enabled);
	}
	return (s_enabled == 1);
}

// use special x86 register allocation for ia32

void _initX86regs()
{
	std::memset(x86regs, 0, sizeof(x86regs));
	g_x86AllocCounter = 0;
	g_x86checknext = 0;
}

int _getFreeX86reg(int mode)
{
    int i, tempi = -1;
    u32 bestcount = 0x10000;

    // Pass 1: Prefer Non-Volatile (Callee-Saved) registers (x19-x28) for stability across C++ calls.
    for (int pass = 0; pass < 2; ++pass)
    {
        for (i = 0; i < iREGCNT_GPR; ++i)
        {
            const int reg = (g_x86checknext + i) % iREGCNT_GPR;
            if (x86regs[reg].inuse || !_isAllocatableX86reg(reg))
                continue;

            // [iter663] BUG FIX: use HostGprPhys(reg) (physical register number)
            // instead of raw slot number. Slot 0-4 all pass (id <= 17) check,
            // incorrectly treating callee-saved x19-x24 as caller-saved.
            const int phys = HostGprPhys(reg);
            if (pass == 0 && armIsCallerSaved(phys)) continue;
            // In pass 1, we accept anything (volatile or not) that is allocatable.

            if ((mode & MODE_COP2) && mVUIsReservedCOP2(reg))
                continue;

            if (x86regs[reg].inuse == 0)
            {
                g_x86checknext = (reg + 1) % iREGCNT_GPR;
                return reg;
            }
        }
    }

    for (i = 0; i < iREGCNT_GPR; ++i)
    {
        if (!_isAllocatableX86reg(i))
            continue;

//        if ((mode & MODE_CALLEESAVED) && xRegister32::IsCallerSaved(i))
//            continue;

        // [P48-2] MODE_CALLEESAVED: only evict callee-saved regs.
        // Without the flag: allow ANY register to be evicted (no filter).
        // Prior bug: else branch skipped all callee-saved regs on ARM64,
        // triggering panic flush and stale EEREC_S references.
        if ((mode & MODE_CALLEESAVED) && !armIsCalleeSavedRegister(HostGprPhys(i))) {
            continue;
        }

        if ((mode & MODE_COP2) && mVUIsReservedCOP2(i))
            continue;

        // should have checked inuse in the previous loop.
        pxAssert(x86regs[i].inuse);

        if (x86regs[i].needed)
            continue;

        if (x86regs[i].type != X86TYPE_TEMP)
        {
            if (x86regs[i].counter < bestcount)
            {
                tempi = static_cast<int>(i);
                bestcount = x86regs[i].counter;
            }
            continue;
        }

        _freeX86reg(i);
        return i;
    }

	if (tempi != -1)
	{
		_freeX86reg(tempi);
		return tempi;
	}

	if (IsRegallocPanicFlushEnabled())
	{
		static u32 s_count = 0;
		if (s_count < 20)
		{
			Console.Error("@@REGALLOC_PANIC_FLUSH@@ idx=%u mode=%x", s_count, mode);
			u32 inuse_mask = 0;
			u32 needed_mask = 0;
			for (u32 r = 0; r < iREGCNT_GPR && r < 32; ++r)
			{
				if (x86regs[r].inuse)
					inuse_mask |= (1u << r);
				if (x86regs[r].needed)
					needed_mask |= (1u << r);
			}
			Console.Error("@@REGALLOC_STATE@@ idx=%u pc=%08x inuse=%08x needed=%08x", s_count, pc, inuse_mask, needed_mask);
		}
		s_count++;
		_flushX86regs();
		_freeX86regs();
		for (i = 0; i < iREGCNT_GPR; ++i)
		{
			const int reg = (g_x86checknext + i) % iREGCNT_GPR;
			if (x86regs[reg].inuse || !_isAllocatableX86reg(reg))
				continue;
			if ((mode & MODE_COP2) && mVUIsReservedCOP2(reg))
				continue;
			g_x86checknext = (reg + 1) % iREGCNT_GPR;
			return reg;
		}
	}

	pxFailRel("x86 register allocation error");
	return -1;
}

void _flushConstReg(int reg)
{
	if (GPR_IS_CONST1(reg) && !(g_cpuFlushedConstReg & (1 << reg)))
	{
//		xWriteImm64ToMem(&cpuRegs.GPR.r[reg].UD[0], rax, g_cpuConstRegs[reg].SD[0]);
        armStore64(PTR_CPU(cpuRegs.GPR.r[reg].UD[0]), g_cpuConstRegs[reg].SD[0]);
		g_cpuFlushedConstReg |= (1 << reg);
		if (reg == 0)
			DevCon.Warning("Flushing r0!");
	}
}

void _flushConstRegs(bool delete_const)
{
	int zero_reg_count = 0;
	int minusone_reg_count = 0;
    u32 i;
	for (i = 0; i < 32; ++i)
	{
		if (!GPR_IS_CONST1(i) || g_cpuFlushedConstReg & (1u << i))
			continue;

		if (g_cpuConstRegs[i].SD[0] == 0)
			zero_reg_count++;
		else if (g_cpuConstRegs[i].SD[0] == -1)
			minusone_reg_count++;
	}

	// if we have more than one of zero/minus-one, precompute
	bool rax_is_zero = false;
	if (zero_reg_count > 1)
	{
//		xXOR(eax, eax);
        armAsm->Eor(EAX, EAX, EAX);
		for (i = 0; i < 32; ++i)
		{
			if (!GPR_IS_CONST1(i) || g_cpuFlushedConstReg & (1u << i))
				continue;

			if (g_cpuConstRegs[i].SD[0] == 0)
			{
//				xMOV(ptr64[&cpuRegs.GPR.r[i].UD[0]], rax);
                armStore(PTR_CPU(cpuRegs.GPR.r[i].UD[0]), RAX);
				g_cpuFlushedConstReg |= 1u << i;
				if (delete_const)
					g_cpuHasConstReg &= ~(1u << i);
			}
		}
		rax_is_zero = true;
	}
	if (minusone_reg_count > 1)
	{
		if (!rax_is_zero) {
//            xMOV(rax, -1);
            armAsm->Mov(RAX, -1);
        }
		else {
//            xNOT(rax);
            armAsm->Mvn(RAX, RAX);
        }

		for (i = 0; i < 32; ++i)
		{
			if (!GPR_IS_CONST1(i) || g_cpuFlushedConstReg & (1u << i))
				continue;

			if (g_cpuConstRegs[i].SD[0] == -1)
			{
//				xMOV(ptr64[&cpuRegs.GPR.r[i].UD[0]], rax);
                armStore(PTR_CPU(cpuRegs.GPR.r[i].UD[0]), RAX);
				g_cpuFlushedConstReg |= 1u << i;
				if (delete_const)
					g_cpuHasConstReg &= ~(1u << i);
			}
		}
	}

	// and whatever's left over..
	for (i = 0; i < 32; ++i)
	{
		if (!GPR_IS_CONST1(i) || g_cpuFlushedConstReg & (1u << i))
			continue;

//		xWriteImm64ToMem(&cpuRegs.GPR.r[i].UD[0], rax, g_cpuConstRegs[i].UD[0]);
        armStore64(PTR_CPU(cpuRegs.GPR.r[i].UD[0]), g_cpuConstRegs[i].UD[0]);
		g_cpuFlushedConstReg |= 1u << i;
		if (delete_const)
			g_cpuHasConstReg &= ~(1u << i);
	}
}

void _validateRegs()
{
#ifdef PCSX2_DEVBUILD
#define MODE_STRING(x) ((((x) & MODE_READ)) ? (((x)&MODE_WRITE) ? "readwrite" : "read") : "write")
	// check that no two registers are in write mode in both fprs and gprs
	for (s8 guestreg = 0; guestreg < 32; guestreg++)
	{
        if(guestreg == 3) continue;

		u32 gprreg = 0, gprmode = 0;
		u32 fprreg = 0, fprmode = 0;
		for (u32 hostreg = 0; hostreg < iREGCNT_GPR; hostreg++)
		{
			if (x86regs[hostreg].inuse && x86regs[hostreg].type == X86TYPE_GPR && x86regs[hostreg].reg == guestreg)
			{
				pxAssertMsg(gprreg == 0 && gprmode == 0, "register is not already allocated in a GPR");
				gprreg = hostreg;
				gprmode = x86regs[hostreg].mode;
			}
		}
		for (u32 hostreg = 0; hostreg < iREGCNT_XMM; hostreg++)
		{
			if (xmmregs[hostreg].inuse && xmmregs[hostreg].type == XMMTYPE_GPRREG && xmmregs[hostreg].reg == guestreg)
			{
				pxAssertMsg(fprreg == 0 && fprmode == 0, "register is not already allocated in a XMM");
				fprreg = hostreg;
				fprmode = xmmregs[hostreg].mode;
			}
		}

		if ((gprmode | fprmode) & MODE_WRITE)
			pxAssertMsg((gprmode & MODE_WRITE) != (fprmode & MODE_WRITE), "only one of gpr or fps is in write state");

		if (gprmode & MODE_WRITE)
			pxAssertMsg(fprmode == 0, "when writing to the gpr, fpr is invalid");
		if (fprmode & MODE_WRITE)
			pxAssertMsg(gprmode == 0, "when writing to the fpr, gpr is invalid");
	}
#undef MODE_STRING
#endif
}

int _allocX86reg(int type, int reg, int mode)
{
	if (type == X86TYPE_GPR || type == X86TYPE_PSX)
	{
		pxAssertMsg(reg >= 0 && reg < 34, "Register index out of bounds.");
	}

	int hostXMMreg = (type == X86TYPE_GPR) ? _checkXMMreg(XMMTYPE_GPRREG, reg, 0) : -1;
	if (type != X86TYPE_TEMP)
	{
        int i, e = static_cast<int>(iREGCNT_GPR);
		for (i = 0; i < e; ++i)
		{
			if (!x86regs[i].inuse || x86regs[i].type != type || x86regs[i].reg != reg)
				continue;

			pxAssert(type != X86TYPE_GPR || !GPR_IS_CONST1(reg) || (GPR_IS_CONST1(reg) && g_cpuFlushedConstReg & (1u << reg)));

			// can't go from write to read
			pxAssert(!((x86regs[i].mode & (MODE_READ | MODE_WRITE)) == MODE_WRITE && (mode & (MODE_READ | MODE_WRITE)) == MODE_READ));
			// if (type != X86TYPE_TEMP && !(x86regs[i].mode & MODE_READ) && (mode & MODE_READ))

			if (type == X86TYPE_GPR)
			{
				RALOG("Changing host reg %d for guest reg %d from %s to %s mode\n", i, reg, GetModeString(x86regs[i].mode), GetModeString(x86regs[i].mode | mode));

				if (mode & MODE_WRITE)
				{
					if (GPR_IS_CONST1(reg))
					{
						RALOG("Clearing constant value for guest reg %d on change to write mode\n", reg);
						GPR_DEL_CONST(reg);
					}

					if (hostXMMreg >= 0)
					{
						// ensure upper bits get written
						RALOG("Invalidating host XMM reg %d for guest reg %d due to GPR write transition\n", hostXMMreg, reg);
						pxAssert(!(xmmregs[hostXMMreg].mode & MODE_WRITE));
						_freeXMMreg(hostXMMreg);
					}
				}
			}
			else if (type == X86TYPE_PSX)
			{
				RALOG("Changing host reg %d for guest PSX reg %d from %s to %s mode\n", i, reg, GetModeString(x86regs[i].mode), GetModeString(x86regs[i].mode | mode));

				if (mode & MODE_WRITE)
				{
					if (PSX_IS_CONST1(reg))
					{
						RALOG("Clearing constant value for guest PSX reg %d on change to write mode\n", reg);
						PSX_DEL_CONST(reg);
					}
				}
			}
			else if (type == X86TYPE_VIREG)
			{
				// keep VI temporaries separate
				if (reg < 0)
					continue;
			}

			x86regs[i].counter = g_x86AllocCounter++;
			x86regs[i].mode |= mode & ~MODE_CALLEESAVED;
			x86regs[i].needed = true;
			return i;
		}
	}

	// [iter663] @@ALLOC_EVICT_A0@@ probe: log slot state when eviction is needed at SIF BEQ PC range
	{ extern u32 g_current_diag_pc;
	if (g_current_diag_pc >= 0x80006400u && g_current_diag_pc <= 0x80006700u) {
		// Check if a0 (GPR 4) is about to be evicted
		int a0_slot = -1;
		for (int s = 0; s < (int)iREGCNT_GPR; s++) {
			if (x86regs[s].inuse && x86regs[s].type == X86TYPE_GPR && x86regs[s].reg == 4)
				a0_slot = s;
		}
		if (a0_slot >= 0) {
			static int s_ae_n = 0;
			if (s_ae_n < 15) {
				u32 inuse_mask = 0;
				for (u32 r = 0; r < iREGCNT_GPR; r++)
					if (x86regs[r].inuse) inuse_mask |= (1u << r);
				Console.WriteLn("@@ALLOC_EVICT_A0@@ n=%d pc=%08x req_type=%d req_reg=%d req_mode=%x a0_slot=%d inuse=%x s0=%d/%d s1=%d/%d s2=%d/%d s3=%d/%d s4=%d/%d",
					s_ae_n++, g_current_diag_pc, type, reg, mode, a0_slot, inuse_mask,
					x86regs[0].inuse ? x86regs[0].reg : -1, x86regs[0].type,
					x86regs[1].inuse ? x86regs[1].reg : -1, x86regs[1].type,
					x86regs[2].inuse ? x86regs[2].reg : -1, x86regs[2].type,
					x86regs[3].inuse ? x86regs[3].reg : -1, x86regs[3].type,
					x86regs[4].inuse ? x86regs[4].reg : -1, x86regs[4].type);
			}
		}
	} }

	const int regnum = _getFreeX86reg(mode);

    // PHASE 2: Validate newly allocated physical register (after slot->phys conversion)
    AssertValidPhysGpr(HostGprPhys(regnum), "_allocX86reg", 0);
    
    a64::XRegister new_reg(HostGprPhys(regnum));
	x86regs[regnum].type = type;
	x86regs[regnum].reg = reg;
	x86regs[regnum].mode = mode & ~MODE_CALLEESAVED;
	x86regs[regnum].counter = g_x86AllocCounter++;
	x86regs[regnum].needed = true;
	x86regs[regnum].inuse = true;

	if (type == X86TYPE_GPR)
	{
		RALOG("Allocating host reg %d to guest reg %d in %s mode\n", regnum, reg, GetModeString(mode));
	}


	if (mode & MODE_READ)
	{
		switch (type)
		{
			case X86TYPE_GPR:
			{
				if (reg == 0)
				{
//					xXOR(xRegister32(new_reg), xRegister32(new_reg)); // 32-bit is smaller and zexts anyway
                    armAsm->Eor(new_reg, new_reg, new_reg);
				}
				else
				{
					if (hostXMMreg >= 0)
					{
						// is in a XMM. we don't need to free the XMM since we're not writing, and it's still valid
						RALOG("Copying %d from XMM %d to GPR %d on read\n", reg, hostXMMreg, regnum);
//						xMOVD(new_reg, xRegisterSSE(hostXMMreg)); // actually MOVQ
                        armAsm->Fmov(new_reg, a64::QRegister(hostXMMreg).V1D());

						// if the XMM was dirty, just get rid of it, we don't want to try to sync the values up...
						if (xmmregs[hostXMMreg].mode & MODE_WRITE)
						{
							RALOG("Freeing dirty XMM %d for GPR %d\n", hostXMMreg, reg);
							_freeXMMreg(hostXMMreg);
						}
					}
					else if (GPR_IS_CONST1(reg))
					{
//						xMOV64(new_reg, g_cpuConstRegs[reg].SD[0]);
                        armAsm->Mov(new_reg, g_cpuConstRegs[reg].SD[0]);
						g_cpuFlushedConstReg |= (1u << reg);
						x86regs[regnum].mode |= MODE_WRITE; // reg is dirty

						RALOG("Writing constant value %lld from guest reg %d to host reg %d\n", g_cpuConstRegs[reg].SD[0], reg, regnum);
					}
					else
					{
						// not loaded
						RALOG("Loading guest reg %d to GPR %d\n", reg, regnum);
//						xMOV(new_reg, ptr64[&cpuRegs.GPR.r[reg].UD[0]]);
                        armLoad(new_reg, PTR_CPU(cpuRegs.GPR.r[reg].UD[0]));
					}
				}
			}
			break;

			case X86TYPE_FPRC:
				RALOG("Loading guest reg FPCR %d to GPR %d\n", reg, regnum);
//				xMOV(xRegister32(regnum), ptr32[&fpuRegs.fprc[reg]]);
                armLoad(a64::WRegister(HostGprPhys(regnum)), PTR_CPU(fpuRegs.fprc[reg]));
				break;

			case X86TYPE_PSX:
			{
//				const xRegister32 new_reg32(regnum);
                const a64::WRegister new_reg32(HostGprPhys(regnum));
				if (reg == 0)
				{
//					xXOR(new_reg32, new_reg32);
                    armAsm->Eor(new_reg32, new_reg32, new_reg32);
				}
				else
				{
					if (PSX_IS_CONST1(reg))
					{
//						xMOV(new_reg32, g_psxConstRegs[reg]);
                        armAsm->Mov(new_reg32, g_psxConstRegs[reg]);
						g_psxFlushedConstReg |= (1u << reg);
						x86regs[regnum].mode |= MODE_WRITE; // reg is dirty

						RALOG("Writing constant value %d from guest PSX reg %d to host reg %d\n", g_psxConstRegs[reg], reg, regnum);
					}
					else
					{
						RALOG("Loading guest PSX reg %d to GPR %d\n", reg, regnum);
//						xMOV(new_reg32, ptr32[&psxRegs.GPR.r[reg]]);
                        armLoad(new_reg32, PTR_CPU(psxRegs.GPR.r[reg]));
					}
				}
			}
			break;

			case X86TYPE_VIREG:
			{
				RALOG("Loading guest VI reg %d to GPR %d", reg, regnum);
//				xMOVZX(xRegister32(regnum), ptr16[&VU0.VI[reg].US[0]]);
                armAsm->Ldrh(a64::WRegister(HostGprPhys(regnum)), PTR_CPU(vuRegs[0].VI[reg].US[0]));
			}
			break;

			default:
				// [iter36b] identify alloc-path abort
				Console.Error("@@ABORT_ALLOC_READ@@ type=%d reg=%d mode=0x%x", type, reg, mode);
				abort();
				break;
		}
	}

	if (type == X86TYPE_GPR && (mode & MODE_WRITE))
	{
		if (reg < 32 && GPR_IS_CONST1(reg))
		{
			RALOG("Clearing constant value for guest reg %d on write allocation\n", reg);
			GPR_DEL_CONST(reg);
		}
		if (hostXMMreg >= 0)
		{
			// writing, so kill the xmm allocation. gotta ensure the upper bits gets stored first.
			RALOG("Invalidating %d from XMM %d because of GPR %d write\n", reg, hostXMMreg, regnum);
			_freeXMMreg(hostXMMreg);
		}
	}
	else if (type == X86TYPE_PSX && (mode & MODE_WRITE))
	{
		if (reg < 32 && PSX_IS_CONST1(reg))
		{
			RALOG("Clearing constant value for guest PSX reg %d on write allocation\n", reg);
			PSX_DEL_CONST(reg);
		}
	}

	// Console.WriteLn("Allocating reg %d", regnum);
	return regnum;
}

extern u32 g_current_diag_pc;

void _writebackX86Reg(int x86reg)
{
    // Phase 8.1: Writeback Logging (Dev Only, PC Range Limited)
    // #ifdef PCSX2_DEVBUILD -> Removed to force log
    // [DELETED] Phase 8.1: Writeback Logging removed to Unify Logging.
    // #endif -> Removed
    // Phase B: Log slot->phys mapping (first 50 times only)
    // [DELETED] Phase B: Log slot->phys mapping removed.
    
    // PHASE 2: Validate physical register before writeback (after slot->phys conversion)
    AssertValidPhysGpr(HostGprPhys(x86reg), "_writebackX86Reg", g_current_diag_pc);

     // Diagnosis: Writeback Entry Trace (T0 specific)
    if (x86reg == 21) {
        static bool s_log_wb = false;
        if (!s_log_wb) {
            s_log_wb = true;
            Console.WriteLn("@@WB_T0@@ pc=%08x host=21 type=%d guest_reg=%d mode=%x", 
                g_current_diag_pc, x86regs[x86reg].type, x86regs[x86reg].reg, x86regs[x86reg].mode);
        }
    }

    // Diagnosis C: Writeback Log
    if (x86regs[x86reg].reg == 8) {
        static bool s_log_wb = false;
        if (!s_log_wb) {
            s_log_wb = true;
            Console.WriteLn("@@T0_WRITEBACK@@ pc=%08x reg=8 path=_writebackX86Reg host=%d", g_current_diag_pc, x86reg);
        }
    }
	switch (x86regs[x86reg].type)
	{
		case X86TYPE_GPR:
		{
			// [iter662] @@WB_A0@@ probe: log writeback of a0 (GPR 4) for SIF BEQ debug
			if (x86regs[x86reg].reg == 4 && g_current_diag_pc >= 0x80006400u && g_current_diag_pc <= 0x80006700u) {
				static int s_wb_a0_n = 0;
				if (s_wb_a0_n < 10) {
					Console.WriteLn("@@WB_A0@@ n=%d pc=%08x slot=%d phys=%d mode=%x",
						s_wb_a0_n++, g_current_diag_pc, x86reg, HostGprPhys(x86reg), x86regs[x86reg].mode);
				}
			}
			if (x86regs[x86reg].reg != 0) {
                if (x86regs[x86reg].reg == 8) {
                    static bool s_log_addr = false;
                    if (!s_log_addr) {
                        s_log_addr = true;
                        Console.WriteLn("@@WB_ADDR@@ pc=%08x host=%d target=GPR[%d]", g_current_diag_pc, x86reg, x86regs[x86reg].reg);
                    }
                }
//				xMOV(ptr64[&cpuRegs.GPR.r[x86regs[x86reg].reg].UL[0]], xRegister64(x86reg));
                // [P201 2026-04-26] Replaced P68 STP (which zeroed UD[1]) with STR (writes UD[0] only).
                // Original x86: xMOV ptr64 — writes 8 bytes (UD[0]), preserves UD[1].
                // P68's STP wrote 16 bytes (UD[0] + UD[1]=xzr), corrupting MMI-set UD[1].
                // PS2 R5900: 32/64-bit instructions preserve UD[1]; only MMI writes both halves.
                // iv2/intCpu match HW behavior; this aligns JIT with that.
                {
                    const int greg = x86regs[x86reg].reg;
                    const s64 off = offsetof(cpuRegistersPack, cpuRegs.GPR.r[0].UD[0]) + greg * sizeof(u128);
                    armAsm->Str(a64::XRegister(HostGprPhys(x86reg)),
                        a64::MemOperand(RSTATE_CPU, off));
                }
            }
		}
		break;

		case X86TYPE_FPRC:
			RALOG("Writing back GPR reg %d for guest reg FPCR %d P2\n", x86reg, x86regs[x86reg].reg);
//			xMOV(ptr32[&fpuRegs.fprc[x86regs[x86reg].reg]], xRegister32(x86reg));
            armStore(PTR_CPU(fpuRegs.fprc[x86regs[x86reg].reg]), a64::WRegister(HostGprPhys(x86reg)));
			break;

		case X86TYPE_VIREG:
			RALOG("Writing back VI reg %d for guest reg %d P2\n", x86reg, x86regs[x86reg].reg);
//			xMOV(ptr16[&VU0.VI[x86regs[x86reg].reg].UL], xRegister16(x86reg));
            armAsm->Strh(a64::WRegister(HostGprPhys(x86reg)), PTR_CPU(vuRegs[0].VI[x86regs[x86reg].reg].UL));
			break;

		case X86TYPE_PCWRITEBACK: // [iter35] missing case label caused default:abort() in delay slot
//			xMOV(ptr32[&cpuRegs.pcWriteback], xRegister32(x86reg));
            armStore(PTR_CPU(cpuRegs.pcWriteback), a64::WRegister(HostGprPhys(x86reg)));
			break;

		case X86TYPE_PSX:
			RALOG("Writing back PSX GPR reg %d for guest reg %d P2\n", x86reg, x86regs[x86reg].reg);
//			xMOV(ptr32[&psxRegs.GPR.r[x86regs[x86reg].reg]], xRegister32(x86reg));
            armStore(PTR_CPU(psxRegs.GPR.r[x86regs[x86reg].reg]), a64::WRegister(HostGprPhys(x86reg)));
			break;

		case X86TYPE_PSX_PCWRITEBACK:
			RALOG("Writing back PSX PC writeback in host reg %d\n", x86reg);
//			xMOV(ptr32[&psxRegs.pcWriteback], xRegister32(x86reg));
            armStore(PTR_CPU(psxRegs.pcWriteback), a64::WRegister(HostGprPhys(x86reg)));
			break;

		default:
			// [iter36b] identify writeback-path abort
			Console.Error("@@ABORT_WRITEBACK@@ slot=%d type=%d guest=%d mode=0x%x pc=%08x",
				x86reg, x86regs[x86reg].type, x86regs[x86reg].reg, x86regs[x86reg].mode, g_current_diag_pc);
			abort();
			break;
	}
}

int _checkX86reg(int type, int reg, int mode)
{
    u32 i;
	for (i = 0; i < iREGCNT_GPR; ++i)
	{
		if (x86regs[i].inuse && x86regs[i].reg == reg && x86regs[i].type == type)
		{
			// shouldn't have dirty constants...
			pxAssert((type != X86TYPE_GPR || !GPR_IS_DIRTY_CONST(reg)) &&
					 (type != X86TYPE_PSX || !PSX_IS_DIRTY_CONST(reg)));

			if ((type == X86TYPE_GPR || type == X86TYPE_PSX) && !(x86regs[i].mode & MODE_READ) && (mode & MODE_READ))
				pxFailRel("Somehow ended up with an allocated x86 without mode");

			// ensure constants get deleted once we alloc as write
			if (mode & MODE_WRITE)
			{
				if (type == X86TYPE_GPR)
				{
					// go through the alloc path instead, because we might need to invalidate an xmm.
					return _allocX86reg(X86TYPE_GPR, reg, mode);
				}
				else if (type == X86TYPE_PSX)
				{
					pxAssert(!PSX_IS_DIRTY_CONST(reg));
					PSX_DEL_CONST(reg);
				}
			}

			x86regs[i].mode |= mode;
			x86regs[i].counter = g_x86AllocCounter++;
			x86regs[i].needed = 1;
			return i;
		}
	}

	return -1;
}

void _addNeededX86reg(int type, int reg)
{
    u32 i;
	for (i = 0; i < iREGCNT_GPR; ++i)
	{
		if (!x86regs[i].inuse || x86regs[i].reg != reg || x86regs[i].type != type)
			continue;

		x86regs[i].counter = g_x86AllocCounter++;
		x86regs[i].needed = 1;
	}
}

void _clearNeededX86regs()
{
    u32 i;
	for (i = 0; i < iREGCNT_GPR; ++i)
	{
		if (x86regs[i].needed)
		{
			if (x86regs[i].inuse && (x86regs[i].mode & MODE_WRITE))
				x86regs[i].mode |= MODE_READ;
		}
		x86regs[i].needed = 0;
	}
}

void _freeX86reg(const a64::Register& x86reg)
{
	// [TEMP_FIX] ARM64/VIXL port: Register::GetCode() is a physical host register code
	// (e.g. x19/x20/...), but _freeX86reg(int) expects an allocator slot index [0..iREGCNT_GPR).
	// Convert physical GPRs back to slot IDs, and ignore non-allocatable scratch / non-GPR registers.
	// Removal condition: すべての callsite が slot API のみに統一され、この overload がnot neededになった時点。
	if (!x86reg.IsRegister())
		return;

	const int phys = x86reg.GetCode();
	for (int slot = 0; slot < static_cast<int>(iREGCNT_GPR); ++slot)
	{
		if (HostGprPhys(slot) == phys)
		{
			_freeX86reg(slot);
			return;
		}
	}

	// Scratch regs like EAX/ECX/EDX map to x0/x1/x2 and are not tracked in x86regs[].
}

void _freeX86reg(int x86reg)
{
	// ARM64/VIXL port compatibility: some callsites still pass a physical host GPR code
	// (x19/x20/...) instead of an allocator slot index. Normalize to slot if matched.
	if (x86reg < 0 || x86reg >= (int)iREGCNT_GPR)
	{
		for (int slot = 0; slot < static_cast<int>(iREGCNT_GPR); ++slot)
		{
			if (HostGprPhys(slot) == x86reg)
			{
				x86reg = slot;
				break;
			}
		}
	}

	// [TEMP_DIAG] iter33: fatal assert 直前にrange外 x86reg と guest PC をログ化してoccur源を特定する。
	// Removal condition: @@FREE_X86REG_BAD@@ で x86reg/pc を確定し、呼出し元の最小fixが完了した時点でdelete。
	if (x86reg < 0 || x86reg >= (int)iREGCNT_GPR)
	{
		static int s_cfg = -1;
		static u32 s_n = 0;
		if (s_cfg < 0)
			s_cfg = (std::getenv("iPSX2_DIAG_FREE_X86REG_BAD") != nullptr) ? 1 : 0;
		if (s_cfg && s_n < 20)
		{
			++s_n;
			const uptr caller = reinterpret_cast<uptr>(__builtin_return_address(0));
			const u32 caller_hi = static_cast<u32>((caller >> 32) & 0xffffffffu);
			const u32 caller_lo = static_cast<u32>(caller & 0xffffffffu);
			Console.Error("[TEMP_DIAG] @@FREE_X86REG_BAD@@ n=%u x86reg=%d iREGCNT_GPR=%d pc=%08x ch=%08x cl=%08x",
				s_n, x86reg, (int)iREGCNT_GPR, g_current_diag_pc, caller_hi, caller_lo);
		}
	}
	pxAssert(x86reg >= 0 && x86reg < (int)iREGCNT_GPR);

	if (x86regs[x86reg].inuse && (x86regs[x86reg].mode & MODE_WRITE))
	{
		_writebackX86Reg(x86reg);
		x86regs[x86reg].mode &= ~MODE_WRITE;
	}

	_freeX86regWithoutWriteback(x86reg);
}

void _freeX86regWithoutWriteback(int x86reg)
{
	pxAssert(x86reg >= 0 && x86reg < (int)iREGCNT_GPR);

	x86regs[x86reg].inuse = 0;

	if (x86regs[x86reg].type == X86TYPE_VIREG)
	{
		RALOG("Freeing VI reg %d in host GPR %d\n", x86regs[x86reg].reg, x86reg);
		mVUFreeCOP2GPR(x86reg);
	}
	else if (x86regs[x86reg].inuse && x86regs[x86reg].type == X86TYPE_GPR)
	{
		RALOG("Freeing X86 register %d (was guest %d)...\n", x86reg, x86regs[x86reg].reg);
	}
	else if (x86regs[x86reg].inuse)
	{
		RALOG("Freeing X86 register %d...\n", x86reg);
	}
}

void _freeX86regs()
{
    u32 i;
	for (i = 0; i < iREGCNT_GPR; ++i)
    {
        _freeX86reg(i);
    }
}

#if !defined(_M_ARM64)
void _flushX86regs()
{
    u32 i;
	for (i = 0; i < iREGCNT_GPR; ++i)
	{
		if (x86regs[i].inuse && x86regs[i].mode & MODE_WRITE)
		{
			// shouldn't be const, because if we got to write mode, we should've flushed then
//			pxAssert(x86regs[i].type != X86TYPE_GPR || !GPR_IS_DIRTY_CONST(x86regs[i].reg));

			RALOG("Flushing x86 reg %u in _eeFlushAllDirty()\n", i);
			_writebackX86Reg(i);
			x86regs[i].mode = (x86regs[i].mode & ~MODE_WRITE) | MODE_READ;
		}
	}
}
#endif
