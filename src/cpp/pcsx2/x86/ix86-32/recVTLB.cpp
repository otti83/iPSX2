// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "vtlb.h"
#include "x86/iCore.h"
#include "x86/iR5900.h"

#include "common/Perf.h"
#include "common/Darwin/DarwinMisc.h"

using namespace vtlb_private;
#if !defined(__ANDROID__)
using namespace x86Emitter;
#endif

// [V3.8 Phase A.1 2026-05-02] fastmem-disable env 群 (sub-bisect 用)。
// 真因解明用に bit-size / READ-WRITE 別に fastmem fast-path を slow path に流す env を提供。
// 修正完了後、 default OFF で残置 (regression test infra として永続)。
namespace ipsx2_fastmem_dbg {
    static int s_init = 0;
    static int s_d8 = 0, s_d16 = 0, s_d32 = 0, s_d64 = 0, s_d128 = 0;
    static int s_d_w = 0, s_d_r = 0;
    static int s_dw8 = 0, s_dw16 = 0, s_dw32 = 0, s_dw64 = 0, s_dw128 = 0;
    static int s_dr8 = 0, s_dr16 = 0, s_dr32 = 0, s_dr64 = 0, s_dr128 = 0;
    static void ensure_init() {
        if (s_init) return;
        s_init = 1;
        s_d8   = std::getenv("iPSX2_FASTMEM_DISABLE_8")   ? 1 : 0;
        s_d16  = std::getenv("iPSX2_FASTMEM_DISABLE_16")  ? 1 : 0;
        s_d32  = std::getenv("iPSX2_FASTMEM_DISABLE_32")  ? 1 : 0;
        s_d64  = std::getenv("iPSX2_FASTMEM_DISABLE_64")  ? 1 : 0;
        s_d128 = std::getenv("iPSX2_FASTMEM_DISABLE_128") ? 1 : 0;
        s_d_w  = std::getenv("iPSX2_FASTMEM_DISABLE_WRITE") ? 1 : 0;
        s_d_r  = std::getenv("iPSX2_FASTMEM_DISABLE_READ")  ? 1 : 0;
        // [Phase A.2] bit-size + read/write 同時指定の細粒 disable
        s_dw8   = std::getenv("iPSX2_FASTMEM_DISABLE_WRITE_8")   ? 1 : 0;
        s_dw16  = std::getenv("iPSX2_FASTMEM_DISABLE_WRITE_16")  ? 1 : 0;
        s_dw32  = std::getenv("iPSX2_FASTMEM_DISABLE_WRITE_32")  ? 1 : 0;
        s_dw64  = std::getenv("iPSX2_FASTMEM_DISABLE_WRITE_64")  ? 1 : 0;
        s_dw128 = std::getenv("iPSX2_FASTMEM_DISABLE_WRITE_128") ? 1 : 0;
        s_dr8   = std::getenv("iPSX2_FASTMEM_DISABLE_READ_8")    ? 1 : 0;
        s_dr16  = std::getenv("iPSX2_FASTMEM_DISABLE_READ_16")   ? 1 : 0;
        s_dr32  = std::getenv("iPSX2_FASTMEM_DISABLE_READ_32")   ? 1 : 0;
        s_dr64  = std::getenv("iPSX2_FASTMEM_DISABLE_READ_64")   ? 1 : 0;
        s_dr128 = std::getenv("iPSX2_FASTMEM_DISABLE_READ_128")  ? 1 : 0;
        Console.WriteLn("@@FASTMEM_DBG@@ b8=%d b16=%d b32=%d b64=%d b128=%d w=%d r=%d",
            s_d8, s_d16, s_d32, s_d64, s_d128, s_d_w, s_d_r);
        Console.WriteLn("@@FASTMEM_DBG_FINE@@ w8=%d w16=%d w32=%d w64=%d w128=%d r8=%d r16=%d r32=%d r64=%d r128=%d",
            s_dw8, s_dw16, s_dw32, s_dw64, s_dw128, s_dr8, s_dr16, s_dr32, s_dr64, s_dr128);
    }
    static bool disable_for_bits(u32 bits) {
        ensure_init();
        switch (bits) {
            case 8: return s_d8 != 0;
            case 16: return s_d16 != 0;
            case 32: return s_d32 != 0;
            case 64: return s_d64 != 0;
            case 128: return s_d128 != 0;
        }
        return false;
    }
    static bool disable_write_path() { ensure_init(); return s_d_w != 0; }
    static bool disable_read_path()  { ensure_init(); return s_d_r != 0; }
    static bool disable_write_for_bits(u32 bits) {
        ensure_init();
        switch (bits) {
            case 8: return s_dw8 != 0;
            case 16: return s_dw16 != 0;
            case 32: return s_dw32 != 0;
            case 64: return s_dw64 != 0;
            case 128: return s_dw128 != 0;
        }
        return false;
    }
    static bool disable_read_for_bits(u32 bits) {
        ensure_init();
        switch (bits) {
            case 8: return s_dr8 != 0;
            case 16: return s_dr16 != 0;
            case 32: return s_dr32 != 0;
            case 64: return s_dr64 != 0;
            case 128: return s_dr128 != 0;
        }
        return false;
    }
    // [Phase A.3] GPR (xmm=false) vs FPR/XMM (xmm=true) 区別
    static int s_dw32_xmm = -1;
    static int s_dw32_gpr = -1;
    static bool disable_write_32_xmm() {
        ensure_init();
        if (s_dw32_xmm < 0) s_dw32_xmm = std::getenv("iPSX2_FASTMEM_DISABLE_WRITE_32_XMM") ? 1 : 0;
        return s_dw32_xmm != 0;
    }
    static bool disable_write_32_gpr() {
        ensure_init();
        if (s_dw32_gpr < 0) s_dw32_gpr = std::getenv("iPSX2_FASTMEM_DISABLE_WRITE_32_GPR") ? 1 : 0;
        return s_dw32_gpr != 0;
    }
    static bool disable_write_split(u32 sz, bool xmm) {
        if (sz != 32) return false;
        if (xmm && disable_write_32_xmm()) return true;
        if (!xmm && disable_write_32_gpr()) return true;
        return false;
    }
    // [Phase A.4 2026-05-02] ★ SW (32-bit GPR STORE) fastmem path 修正 ★
    // P1-B sub-bisect で 32-bit GPR write fastmem path のみが character vertex transform 用 memory
    // を破壊と確定 (read OK、 8/16/64/128 OK、 SWC1 (xmm) OK、 SW (non-xmm sz=32) のみ NG)。
    // 詳細 emit logic bug 解明前の暫定 fix: SW を always slow path (memWrite32) 経由化。
    // iPSX2_DISABLE_SW_FASTMEM_FIX=1 で fix OFF (debug/regression 用)。 default ON (= fix 適用)。
    static int s_disable_sw_fix = -1;
    static bool apply_sw_fastmem_fix(u32 sz, bool xmm) {
        if (s_disable_sw_fix < 0) {
            s_disable_sw_fix = std::getenv("iPSX2_DISABLE_SW_FASTMEM_FIX") ? 1 : 0;
            Console.WriteLn("@@SW_FASTMEM_FIX@@ apply=%d (1=fix ON、 0=fix OFF debug)", !s_disable_sw_fix);
        }
        return !xmm && sz == 32 && !s_disable_sw_fix;
    }
}

extern "C" void vtlb_MemWrite32_KSEG1(u32 addr, u32 data);
extern "C" u32 recMemRead32_KSEG1(u32 addr);
// we need enough for a 32-bit jump forwards (5 bytes)
//static constexpr u32 LOADSTORE_PADDING = 5;

//#define LOG_STORES

static u32 GetAllocatedGPRBitmask()
{
	u32 i, mask = 0;
	for (i = 0; i < iREGCNT_GPR; ++i)
	{
		if (x86regs[i].inuse)
			mask |= (1u << i);
	}
	return mask;
}

static u32 GetAllocatedXMMBitmask()
{
	u32 i, mask = 0;
	for (i = 0; i < iREGCNT_XMM; ++i)
	{
		if (xmmregs[i].inuse)
			mask |= (1u << i);
	}
	return mask;
}

/*
	// Pseudo-Code For the following Dynarec Implementations -->

	u32 vmv = vmap[addr>>VTLB_PAGE_BITS].raw();
	sptr ppf=addr+vmv;
	if (!(ppf<0))
	{
		data[0]=*reinterpret_cast<DataType*>(ppf);
		if (DataSize==128)
			data[1]=*reinterpret_cast<DataType*>(ppf+8);
		return 0;
	}
	else
	{
		//has to: translate, find function, call function
		u32 hand=(u8)vmv;
		u32 paddr=(ppf-hand) << 1;
		//Console.WriteLn("Translated 0x%08X to 0x%08X",params addr,paddr);
		return reinterpret_cast<TemplateHelper<DataSize,false>::HandlerType*>(RWFT[TemplateHelper<DataSize,false>::sidx][0][hand])(paddr,data);
	}

	// And in ASM it looks something like this -->

	mov eax,ecx;
	shr eax,VTLB_PAGE_BITS;
	mov rax,[rax*wordsize+vmap];
	add rcx,rax;
	js _fullread;

	//these are wrong order, just an example ...
	mov [rax],ecx;
	mov ecx,[rdx];
	mov [rax+4],ecx;
	mov ecx,[rdx+4];
	mov [rax+4+4],ecx;
	mov ecx,[rdx+4+4];
	mov [rax+4+4+4+4],ecx;
	mov ecx,[rdx+4+4+4+4];
	///....

	jmp cont;
	_fullread:
	movzx eax,al;
	sub   ecx,eax;
	call [eax+stuff];
	cont:
	........

*/

#ifdef LOG_STORES
static std::FILE* logfile;
static bool CheckLogFile()
{
	if (!logfile)
		logfile = std::fopen("C:\\Dumps\\comp\\memlog.bad.txt", "wb");
	return (logfile != nullptr);
}

static void LogWrite(u32 addr, u64 val)
{
	if (!CheckLogFile())
		return;

	std::fprintf(logfile, "%08X @ %u: %llx\n", addr, cpuRegs.cycle, val);
	std::fflush(logfile);
}

static void __vectorcall LogWriteQuad(u32 addr, __m128i val)
{
	if (!CheckLogFile())
		return;

	std::fprintf(logfile, "%08X @ %u: %llx %llx\n", addr, cpuRegs.cycle, val.m128i_u64[0], val.m128i_u64[1]);
	std::fflush(logfile);
}
#endif

namespace vtlb_private
{
	// ------------------------------------------------------------------------
	// Prepares eax, ecx, and, ebx for Direct or Indirect operations.
	// Returns the writeback pointer for ebx (return address from indirect handling)
	//
	static void DynGen_PrepRegs(int addr_reg, int value_reg, u32 sz, bool xmm)
	{
		EE::Profiler.EmitMem();

//		_freeX86reg(arg1regd);
        _freeX86reg(ECX);
//		xMOV(arg1regd, xRegister32(addr_reg));
        // [iPSX2] @@PREPAREGS_PHYS_FIX@@: addr_reg is a physical ARM64 register number
        // (ECX.GetCode()=1 = physical w1), not a JIT slot. HostW(1)=w20 (kSlotToPhys[1])
        // would overwrite the EA already in ECX (w1) with the wrong slot register value.
        // Using WRegister(addr_reg) directly avoids the kSlotToPhys mismatch.
        const a64::WRegister addr_w = a64::WRegister(addr_reg);
        armAsm->Mov(ECX, addr_w);

		if (value_reg >= 0)
		{
			if (sz == 128)
			{
				pxAssert(xmm);
//				_freeXMMreg(xRegisterSSE::GetArgRegister(1, 0).GetId());
                _freeXMMreg(armQRegister(1).GetCode());
//				xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg));
                armAsm->Mov(armQRegister(1), armQRegister(value_reg));
			}
			else if (xmm)
			{
				// 32bit xmms are passed in GPRs
				pxAssert(sz == 32);
//				_freeX86reg(arg2regd);
                _freeX86reg(EDX);
//				xMOVD(arg2regd, xRegisterSSE(value_reg));
                armAsm->Fmov(EDX, a64::QRegister(value_reg).S());
			}
			else
			{
//				_freeX86reg(arg2regd);
                _freeX86reg(EDX);
//				xMOV(arg2reg, xRegister64(value_reg));
                armAsm->Mov(RDX, HostX(value_reg));
			}
		}

//		xMOV(eax, arg1regd);
        // CRITICAL FIX: Zero-extend the address in RCX to 64-bit.
        armAsm->Uxtw(RCX, RCX);

        armAsm->Mov(EAX, ECX);
//		xSHR(eax, VTLB_PAGE_BITS);
        armAsm->Lsr(EAX, EAX, VTLB_PAGE_BITS);
//		xMOV(rax, ptrNative[xComplexAddress(arg3reg, vtlbdata.vmap, rax * wordsize)]);
        armAsm->Ldr(RXVIXLSCRATCH, PTR_CPU(vtlbdata.vmap));
        armAsm->Ldr(RAX, a64::MemOperand(RXVIXLSCRATCH, RAX, a64::LSL, 3));
//		xADD(arg1reg, rax);
        armAsm->Adds(RCX, RCX, RAX);
	}

	// ------------------------------------------------------------------------
	static void DynGen_DirectRead(u32 bits, bool sign)
	{
		pxAssert(bits == 8 || bits == 16 || bits == 32 || bits == 64 || bits == 128);

        auto mop = a64::MemOperand(RCX);
		switch (bits)
		{
            case 8:
                if (sign) {
//                    xMOVSX(rax, ptr8[arg1reg]);
                    // [P30/R105] Ldrb + Sxtb for correct sign extension
                    armAsm->Ldrb(EAX, mop); armAsm->Sxtb(RAX, EAX);
                }
                else {
//                    xMOVZX(rax, ptr8[arg1reg]);
                    armAsm->Ldrb(EAX, mop);
                }
                break;

            case 16:
                if (sign) {
//                    xMOVSX(rax, ptr16[arg1reg]);
                    armAsm->Ldrh(EAX, mop); armAsm->Sxth(RAX, EAX);
                }
                else {
//                    xMOVZX(rax, ptr16[arg1reg]);
                    armAsm->Ldrh(EAX, mop);
                }
                break;

            case 32:
                if (sign) {
//                    xMOVSX(rax, ptr32[arg1reg]);
                    armAsm->Ldrsw(RAX, mop);
                }
                else {
//					xMOV(eax, ptr32[arg1reg]);
                    armAsm->Ldr(EAX, mop);
                }
                break;

            case 64:
//				xMOV(rax, ptr64[arg1reg]);
                armAsm->Ldr(RAX, mop);
                break;

            case 128:
//				xMOVAPS(xmm0, ptr128[arg1reg]);
                armAsm->Ldr(xmm0.Q(), mop);
                break;

			jNO_DEFAULT
		}
	}

	// ------------------------------------------------------------------------
	static void DynGen_DirectWrite(u32 bits)
	{
        auto mop = a64::MemOperand(RCX);
		switch (bits)
		{
            case 8:
//				xMOV(ptr[arg1reg], xRegister8(arg2regd));
                armAsm->Strb(EDX, mop);
                break;

            case 16:
//				xMOV(ptr[arg1reg], xRegister16(arg2regd));
                armAsm->Strh(EDX, mop);
                break;

            case 32:
//				xMOV(ptr[arg1reg], arg2regd);
                armAsm->Str(EDX, mop);
                break;

            case 64:
//				xMOV(ptr[arg1reg], arg2reg);
                armAsm->Str(RDX, mop);
                break;

            case 128:
//				xMOVAPS(ptr[arg1reg], xRegisterSSE::GetArgRegister(1, 0));
                armAsm->Str(armQRegister(1).Q(), mop);
                break;
		}
	}
} // namespace vtlb_private

static bool hasBeenCalled = false;
static constexpr u32 INDIRECT_DISPATCHER_SIZE = 128;
static constexpr u32 INDIRECT_DISPATCHERS_SIZE = 20 * INDIRECT_DISPATCHER_SIZE;
alignas(__pagesize) static u8 m_IndirectDispatchers[__pagesize];
std::atomic<u8*> g_vtlb_dispatcher_base{nullptr};

static bool IsFixBios414xNoFastmemEnabled()
{
    static int s_enabled = -1;
    if (s_enabled < 0)
    {
        // [P47] Device: mach_vm_remap shares physical pages → 期待 fastmem safe → default OFF だった。
        // Simulator: mmap(MAP_ANON) non-shared pages → fastmem reads stale → default ON (slow path)。
        // [V15 真因] ユーザー観察: 実機で BIOS 乱れ、 sim 正常。 dual_map fastmem 「safe」 前提が誤り。
        // env override default true に変更 = 実機でも default 強制 ON、 BIOS area slow path 化で乱れ解消。
        // env=0 で dual_map 環境では従来動作 (fastmem 経由) に override 可。
        const bool is_dual_map = (DarwinMisc::g_code_rw_offset != 0);
        const bool env_override = iPSX2_GetRuntimeEnvBool("iPSX2_FIX_BIOS_414X_NO_FASTMEM", true);
        s_enabled = (!is_dual_map || env_override) ? 1 : 0;
        Console.WriteLn("@@CFG@@ iPSX2_FIX_BIOS_414X_NO_FASTMEM=%d (dual_map=%d offset=%td, V15 default ON)",
            s_enabled, (int)is_dual_map, DarwinMisc::g_code_rw_offset);
    }
    return (s_enabled == 1);
}

// ------------------------------------------------------------------------
// mode        - 0 for read, 1 for write!
// operandsize - 0 thru 4 represents 8, 16, 32, 64, and 128 bits.
//
static u8* GetIndirectDispatcherPtr(int mode, int operandsize, int sign = 0)
{
	pxAssert(mode || operandsize >= 3 ? !sign : true);

	const u32 offset = (mode * (8 * INDIRECT_DISPATCHER_SIZE)) + (sign * 5 * INDIRECT_DISPATCHER_SIZE) +
	                   (operandsize * INDIRECT_DISPATCHER_SIZE);
	// [iter190] m_IndirectDispatchers は BSS 静的配列 (mprotect ExecOnly が iOS simulatorでdisabled)。
	// vtlb_DynGenDispatchers() が JIT 領域へのコピーを g_vtlb_dispatcher_base にsaveするので、
	// 初期化後はそちら (MAP_JIT = 実行可能) を優先して使う。
	// Removal condition: IFETCH 0x10e2b8380 が消滅し BIOS browserbootafter confirmed
	u8* jit_base = g_vtlb_dispatcher_base.load(std::memory_order_acquire);
	if (jit_base != nullptr)
		return jit_base + offset;
	return &m_IndirectDispatchers[offset];
}

// ------------------------------------------------------------------------
// Generates a JS instruction that targets the appropriate templated instance of
// the vtlb Indirect Dispatcher.
//
template <typename GenDirectFn>
static void DynGen_HandlerTest(const GenDirectFn& gen_direct, int mode, int bits, bool sign = false)
{
	int szidx = 0;
	switch (bits)
	{
		case   8: szidx = 0; break;
		case  16: szidx = 1; break;
		case  32: szidx = 2; break;
		case  64: szidx = 3; break;
		case 128: szidx = 4; break;
		jNO_DEFAULT;
	}
	// [iPSX2][iter165] @@HANDLER_TEST@@: log dispatcher ptr to diagnose SIGILL BL target mismatch
	const void* disp_ptr = GetIndirectDispatcherPtr(mode, szidx, sign);
	{
		static std::atomic<int> s_ht_n{0};
		int hn = s_ht_n.fetch_add(1, std::memory_order_relaxed);
		if (hn < 10) {
			u8* base = g_vtlb_dispatcher_base.load(std::memory_order_acquire);
			Console.WriteLn("@@HANDLER_TEST@@ n=%d mode=%d sz=%d sign=%d ptr=%p base=%p off=%d",
				hn, mode, szidx, (int)sign, disp_ptr, (void*)base,
				base ? (int)((const u8*)disp_ptr - base) : -1);
		}
	}
	a64::Label to_handler;
	armAsm->B(&to_handler, a64::Condition::mi);
	gen_direct();
	a64::Label done;
	armAsm->B(&done);
	armBind(&to_handler);
	armEmitCall(disp_ptr);
	armBind(&done);
}

// ------------------------------------------------------------------------
// Generates the various instances of the indirect dispatchers
// In: arg1reg: vtlb entry, arg2reg: data ptr (if mode >= 64), rbx: function return ptr
// Out: eax: result (if mode < 64)
static void DynGen_IndirectTlbDispatcher(int mode, int bits, bool sign)
{
	// fixup stack
#ifdef _WIN32
	xSUB(rsp, 32 + 8);
#else
	armAsm->Push(a64::lr, a64::xzr);
#endif

	armAsm->Uxtb(EEX, EAX);
	// [P15] Mask handler index to 7 bits (0-127) to prevent OOB access on RWFT[128].
	// Valid handler IDs are 0-127 (VTLB_HANDLER_ITEMS=128). A corrupted vtlb entry
	// with low byte >= 128 would read past the array boundary.
	// Removal condition: なし（恒久fix）
	armAsm->And(EEX, EEX, 0x7F);
	if (wordsize != 8)
		armAsm->Sub(ECX, ECX, 0x80000000);
	armAsm->Sub(ECX, ECX, EEX);

	armAsm->Mov(RAX, RCX); // ecx is address
	armAsm->Mov(RCX, RDX); // edx is data

	armAsm->Mov(RXVIXLSCRATCH, (sptr)vtlbdata.RWFT[bits][mode]);
	armAsm->Ldr(REX, a64::MemOperand(RXVIXLSCRATCH, REX, a64::LSL, 3));
	// [P15] Null-check: if handler pointer is null, skip BLR and return 0 (for reads).
	// Removal condition: なし（恒久fix）
	{
		a64::Label handler_valid, after_blr;
		armAsm->Cbnz(REX, &handler_valid);
		// Handler is null — return 0 for reads, nop for writes
		if (!mode)
			armAsm->Mov(RAX, 0);
		armAsm->B(&after_blr);
		armBind(&handler_valid);
		armAsm->Blr(REX);
		armBind(&after_blr);
	}


	if (!mode)
	{
		if (bits == 0)
		{
			if (sign) {
//                xMOVSX(rax, al);
                armAsm->Sxtb(RAX, RAX);
            }
			else {
//                xMOVZX(rax, al);
                armAsm->Uxtb(RAX, RAX);
            }
		}
		else if (bits == 1)
		{
			if (sign) {
//                xMOVSX(rax, ax);
                armAsm->Sxth(RAX, RAX);
            }
			else {
//                xMOVZX(rax, ax);
                armAsm->Uxth(RAX, RAX);
            }
		}
		else if (bits == 2)
		{
			if (sign) {
//                xCDQE();
                armAsm->Sxtw(RAX, RAX);
            }
		}
	}

#ifdef _WIN32
	xADD(rsp, 32 + 8);
#else
	armAsm->Pop(a64::lr, a64::xzr); // [iter191] Push(lr,xzr) → [SP-16]=lr,[SP-8]=0; Pop(lr,xzr) → lr←[SP]=lr ✓
#endif

//	xRET();
	armAsm->Ret();
}

void vtlb_DynGenDispatchers()
{
    u8* code_start = armEndBlock();
    if (!hasBeenCalled)
    {
        hasBeenCalled = true;
        HostSys::MemProtect(m_IndirectDispatchers, __pagesize, PageAccess_ReadWrite());

        // clear the buffer to 0xcc (easier debugging).
        std::memset(m_IndirectDispatchers, 0xcc, __pagesize);

        int mode, bits, sign;
        for (mode = 0; mode < 2; ++mode) {
            for (bits = 0; bits < 5; ++bits) {
                for (sign = 0; sign < (!mode && bits < 3 ? 2 : 1); ++sign) {
                    armSetAsmPtr(GetIndirectDispatcherPtr(mode, bits, !!sign), INDIRECT_DISPATCHERS_SIZE, nullptr);
                    armStartBlock();
                    ////
                    DynGen_IndirectTlbDispatcher(mode, bits, !!sign);
                    ////
                    armEndBlock();
                }
            }
        }
        // [iter192] @@VTLB_BSS_CHECK@@ BSS content after init loop (before ExecOnly)
        Console.WriteLn("@@VTLB_BSS_CHECK@@ bss=%p bss_off896=%08x bss_off256=%08x bss_off640=%08x",
            (void*)m_IndirectDispatchers,
            *(const uint32_t*)(m_IndirectDispatchers + 896),
            *(const uint32_t*)(m_IndirectDispatchers + 256),
            *(const uint32_t*)(m_IndirectDispatchers + 640));
#if !defined(iPSX2_MACOS)
        // [P63] macOS: BSS staging buffer cannot be made executable; dispatchers are used from JIT cache
        HostSys::MemProtect(m_IndirectDispatchers, __pagesize, PageAccess_ExecOnly());
#endif
    }

    Perf::any.Register(m_IndirectDispatchers, __pagesize, "TLB Dispatcher");
    //// copy code
    {
        // [P45-2] dual-mapping: write through RW view, code_start is RX
        u8* write_ptr = code_start + DarwinMisc::g_code_rw_offset;
        HostSys::AutoCodeWrite writer(code_start, INDIRECT_DISPATCHERS_SIZE);
        memcpy(write_ptr, m_IndirectDispatchers, INDIRECT_DISPATCHERS_SIZE);
    }

    // Update Atomic Pointer (Release)
    g_vtlb_dispatcher_base.store(code_start, std::memory_order_release);
    // [iter192] @@VTLB_JIT_CHECK@@ JIT copy content after memcpy
    Console.WriteLn("@@VTLB_JIT_CHECK@@ jit=%p jit_off896=%08x jit_off256=%08x",
        (void*)code_start,
        *(const uint32_t*)(code_start + 896),
        *(const uint32_t*)(code_start + 256));
    
    // HostSys::EndCodeWrite(); // Handled by AutoCodeWrite
    ////
    armSetAsmPtr(code_start + INDIRECT_DISPATCHERS_SIZE, INDIRECT_DISPATCHERS_SIZE, nullptr);
    armStartBlock();
}

//////////////////////////////////////////////////////////////////////////////////////////
//                            Dynarec Load Implementations
// ------------------------------------------------------------------------
// Recompiled input registers:
//   ecx - source address to read from
//   Returns read value in eax.
int vtlb_DynGenReadNonQuad(u32 bits, bool sign, bool xmm, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
    pxAssume(bits <= 64);

    int x86_dest_reg;
    // [iter218] Extended bits check to include 8/16 (LBU/LHU) to avoid fastmem SIGBUS
    // for TLB-mapped BIOS addresses that resolve to unmapped physical memory (>512MB).
    // [iter282] PC range extended to KSEG0 EE RAM (0x80000000-0x81FFFFFF) to handle
    // LOGO/bootstrap fastmem SIGBUS. Fastmem backpatch is unreliable for EE RAM code;
    // direct memRead is safer. Covers BIOS ROM KSEG1 (9FC00000-9FC80000) + EE RAM KSEG0.
    // Removal condition: fastmem backpatch が EE RAM コードで安定してbehaviorすることをafter confirmed
    // [FIX] force_bios_414x_no_fastmem: on device (dual-mapping), vmap direct load
    // returns wrong data. Must cover all PC ranges: BIOS ROM (KSEG0/KSEG1),
    // kernel/RAM, and user-space (SIF wrappers, BIOS browser code, etc).
    // [FIX] Removed !xmm: LWC1 dynamic-address reads (xmm=true, bits=32) were
    // bypassing force check, using fastmem which returns wrong data on device.
    // [P48-2 FIX] READ dynamic: must match WRITE dynamic ranges to avoid store-load mismatch.
    // Without (pc < 0x02000000u), user-space stores go through vtlb (memWriteN) but loads
    // use fastmem/direct path — if these map different pages, LD reads stale data.
    const bool force_bios_414x_no_fastmem =
        IsFixBios414xNoFastmemEnabled() &&
        (bits == 8 || bits == 16 || bits == 32 || bits == 64) &&
        ((pc >= 0x9FC00000u && pc < 0x9FC80000u) ||
         (pc >= 0xBFC00000u && pc < 0xBFC80000u) ||
         (pc >= 0x80000000u && pc < 0x82000000u) ||
         (pc < 0x02000000u));
    static int s_ee_t0_force_cfg_dyn = -1;
    if (s_ee_t0_force_cfg_dyn < 0)
    {
        s_ee_t0_force_cfg_dyn = iPSX2_GetRuntimeEnvBool("iPSX2_EE_T0_FORCE_MEMREAD32", true) ? 1 : 0;
    }
    const bool ee_bios_t0_force_memread32_dyn =
        (s_ee_t0_force_cfg_dyn == 1 && !xmm && bits == 32 &&
         (pc == 0x9FC41044u || pc == 0x9FC4104Cu || pc == 0x9FC4107Cu || pc == 0x9FC41084u));

    // [iter220] FIX: vtlb slow-path handler dispatch return is broken on ARM64.
    // SIO_ISR poll at 9FC433F0 (JIT pc=9FC433F4) hangs because handler return
    // doesn't reach the code after DynGen_HandlerTest. Bypass via direct memRead32.
    const bool force_sio_isr_memread32 =
        (s_ee_t0_force_cfg_dyn == 1 && !xmm && bits == 32 && pc == 0x9FC433F4u);

    // [iter220] FIX: vtlb slow-path handler dispatch return is broken on ARM64.
    // DynGen_IndirectTlbDispatcher's handler call + Ret doesn't return to the
    // JIT code after DynGen_HandlerTest's armEmitCall. Bypass ALL force_bios_414x
    // reads by calling memReadN directly, which goes through C-level vtlb dispatch.
    if (force_bios_414x_no_fastmem || force_sio_isr_memread32)
    {
        iFlushCall(FLUSH_FULLVTLB);
        if (!xmm && bits <= 64)
        {
            // Pass address from addr_reg to first arg (EAX = w0)
            armAsm->Mov(EAX, a64::WRegister(addr_reg));
            switch (bits)
            {
                case  8: armEmitCall((void*)(sign ? (void*)memRead8  : (void*)memRead8)); break;
                case 16: armEmitCall((void*)(sign ? (void*)memRead16 : (void*)memRead16)); break;
                case 32: armEmitCall((void*)memRead32); break;
                case 64: armEmitCall((void*)memRead64); break;
            }
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), -1);
            auto regX = (x86_dest_reg < 0 ? RAX : HostX(x86_dest_reg));
            if (bits == 64)
                armAsm->Mov(regX, RAX);
            else if (sign) {
                // [P30/R105] memRead8/16/32 returns zero-extended u32.
                // Sign-extend from actual load width, not always 32-bit.
                switch (bits) {
                    case  8: armAsm->Sxtb(regX, EAX); break;
                    case 16: armAsm->Sxth(regX, EAX); break;
                    case 32: armAsm->Sxtw(regX, EAX); break;
                }
            }
            else
                armAsm->Mov(regX.W(), EAX);
        }
        else
        {
            // [P15] FIX: DynGen_HandlerTest broken on ARM64. Use direct vtlb_memRead calls.
            // Removal condition: なし（恒久fix）
            if (bits == 128)
            {
                armAsm->Mov(EAX, a64::WRegister(addr_reg));
                armEmitCall((void*)vtlb_memRead128);
                x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
                if (x86_dest_reg != 0)
                    armAsm->Mov(a64::QRegister(x86_dest_reg), a64::QRegister(0));
            }
            else
            {
                armAsm->Mov(EAX, a64::WRegister(addr_reg));
                switch (bits) {
                    case  8: armEmitCall((void*)memRead8); break;
                    case 16: armEmitCall((void*)memRead16); break;
                    case 32: armEmitCall((void*)memRead32); break;
                    case 64: armEmitCall((void*)memRead64); break;
                }
                if (xmm)
                {
                    x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
                    armAsm->Fmov(a64::QRegister(x86_dest_reg).S(), EAX);
                }
                else
                {
                    x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), -1);
                    if (x86_dest_reg >= 0) {
                        if (sign && bits < 64) armAsm->Sxtw(HostX(x86_dest_reg), EAX);
                        else if (bits == 64) armAsm->Mov(HostX(x86_dest_reg), RAX);
                        else armAsm->Mov(HostX(x86_dest_reg).W(), EAX);
                    }
                }
            }
        }
        return x86_dest_reg;
    }

    // NORMAL FASTMEM CHECK (vtlb_IsFaultingPC only, since force_bios handled above).
    // [iter229] FIX: DynGen_HandlerTest is broken on ARM64 (handler call+Ret doesn't
    // return to JIT). Use direct memReadN calls instead (same pattern as force_bios path).
    if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
    {
        iFlushCall(FLUSH_FULLVTLB);

        if (!xmm && bits <= 64)
        {
            armAsm->Mov(EAX, a64::WRegister(addr_reg));
            switch (bits)
            {
                case  8: armEmitCall((void*)(sign ? (void*)memRead8  : (void*)memRead8)); break;
                case 16: armEmitCall((void*)(sign ? (void*)memRead16 : (void*)memRead16)); break;
                case 32: armEmitCall((void*)memRead32); break;
                case 64: armEmitCall((void*)memRead64); break;
            }
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), -1);
            auto regX = (x86_dest_reg < 0 ? RAX : HostX(x86_dest_reg));
            if (bits == 64)
                armAsm->Mov(regX, RAX);
            else if (sign) {
                // [P30/R105] memRead8/16/32 returns zero-extended u32.
                // Sign-extend from actual load width, not always 32-bit.
                switch (bits) {
                    case  8: armAsm->Sxtb(regX, EAX); break;
                    case 16: armAsm->Sxth(regX, EAX); break;
                    case 32: armAsm->Sxtw(regX, EAX); break;
                }
            }
            else
                armAsm->Mov(regX.W(), EAX);
        }
        else
        {
            // [P15] FIX: DynGen_HandlerTest broken on ARM64. Use direct vtlb_memRead calls.
            // Removal condition: なし（恒久fix）
            if (bits == 128)
            {
                armAsm->Mov(EAX, a64::WRegister(addr_reg));
                armEmitCall((void*)vtlb_memRead128);
                x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
                if (x86_dest_reg != 0)
                    armAsm->Mov(a64::QRegister(x86_dest_reg), a64::QRegister(0));
            }
            else
            {
                armAsm->Mov(EAX, a64::WRegister(addr_reg));
                switch (bits) {
                    case  8: armEmitCall((void*)memRead8); break;
                    case 16: armEmitCall((void*)memRead16); break;
                    case 32: armEmitCall((void*)memRead32); break;
                    case 64: armEmitCall((void*)memRead64); break;
                }
                if (xmm)
                {
                    x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
                    armAsm->Fmov(a64::QRegister(x86_dest_reg).S(), EAX);
                }
                else
                {
                    x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), -1);
                    if (x86_dest_reg >= 0) {
                        if (sign && bits < 64) armAsm->Sxtw(HostX(x86_dest_reg), EAX);
                        else if (bits == 64) armAsm->Mov(HostX(x86_dest_reg), RAX);
                        else armAsm->Mov(HostX(x86_dest_reg).W(), EAX);
                    }
                }
            }
        }

        return x86_dest_reg;
    }

    if (ee_bios_t0_force_memread32_dyn)
    {
        iFlushCall(FLUSH_FULLVTLB);
        // [FIX_CONST_WRITEBACK] Do NOT use addr_reg: cpuRegs.GPR.r[7] is stale (const-fold
        // write-back missing in preceding block). These PCs always read EE Timer0 at 0xB0000000.
        armAsm->Mov(EAX, 0xB0000000u);
        armEmitCall((void*)memRead32);
        x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), -1);
        auto regX = (x86_dest_reg < 0 ? RAX : HostX(x86_dest_reg));
        sign ? armAsm->Sxtw(regX, EAX) : armAsm->Mov(regX.W(), EAX);
        return x86_dest_reg;
    }

    const u8* codeStart;
//	const xAddressReg x86addr(addr_reg);
    // [iPSX2] @@ADDR_REG_PHYS_FIX@@: addr_reg is always a physical ARM64 register number
    // (ECX.GetCode()=1 = physical w1/x1), never a JIT slot number. With ENABLE_SLOT7_GUARD active,
    // HostX(1)=XRegister(kSlotToPhys[1])=x20 (s3's JIT slot) instead of x1=ECX.
    // Using XRegister(addr_reg) directly treats it as a physical register → correct fastmem LDR.
    // Without fix: LDR used x20 (s3=0x00160000+corruption) as address → s3 corrupted by wrong data.
    const a64::XRegister x86addr = a64::XRegister(addr_reg);
    // UXTW: force 32-bit zero-extension to prevent 0xBFCxxxxx sign-extending to 64-bit.
    auto mop = a64::MemOperand(RFASTMEMBASE, x86addr.W(), a64::UXTW);
    if (!xmm)
    {
//		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
        x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), -1);
//		const xRegister64 x86reg(x86_dest_reg);

        auto x86reg = (x86_dest_reg < 0 ? RAX : HostX(x86_dest_reg));
        a64::Label kseg1_do_fastmem;
        a64::Label kseg1_read_done;
        a64::Label spad_do_fastmem;
        a64::Label spad_read_done;
        // [iter229] FIX: extend KSEG1 bypass to 8/16-bit reads (was 32-bit only).
        // LHU from 0xBA000006 (DVE status) bypassed vtlb handler → poll returned 0 forever.
        const bool emit_kseg1_read_bypass = (bits == 8 || bits == 16 || bits == 32);
        const bool emit_spad_read_bypass = true;

        if (emit_kseg1_read_bypass)
        {
            // KSEG1 MMIO reads must bypass fastmem direct loads.
            armAsm->Mov(a64::w10, x86addr.W());
            armAsm->And(a64::w10, a64::w10, 0xE0000000);
            armAsm->Cmp(a64::w10, 0xA0000000);
            armAsm->B(&kseg1_do_fastmem, a64::Condition::ne);

            armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
            armAsm->Push(a64::x29, a64::lr);
            armAsm->Mov(a64::w0, x86addr.W());
            // [iter229] per-bits dispatch (was hardcoded recMemRead32_KSEG1)
            switch (bits)
            {
                case  8: armEmitCall((void*)memRead8); break;
                case 16: armEmitCall((void*)memRead16); break;
                case 32: armEmitCall((void*)recMemRead32_KSEG1); break;
                jNO_DEFAULT
            }
            armAsm->Mov(a64::w10, a64::w0);
            armAsm->Pop(a64::lr, a64::x29);
            armAsm->Pop(a64::x3, a64::x2, a64::x1, a64::x0);
            switch (bits)
            {
                case 8:
                    sign ? armAsm->Sxtb(x86reg, a64::w10) : armAsm->Mov(x86reg.W(), a64::w10);
                    break;
                case 16:
                    sign ? armAsm->Sxth(x86reg, a64::w10) : armAsm->Mov(x86reg.W(), a64::w10);
                    break;
                case 32:
                    sign ? armAsm->Sxtw(x86reg, a64::w10) : armAsm->Mov(x86reg.W(), a64::w10);
                    break;
                jNO_DEFAULT
            }
            armAsm->B(&kseg1_read_done);

            armBind(&kseg1_do_fastmem);
        }

        // [P11 fix] EE hardware registers (0x10000000-0x1FFFFFFF): bypass fastmem direct load.
        // vtlb maps this range to hw handlers (not fastmem). Fastmem unmapped page returns 0,
        // causing hardware register polls (e.g. SIF_F230 at 0x1000F230) to loop forever.
        // Mirror scratchpad bypass pattern exactly.
        {
            a64::Label ee_hw_done;
            armAsm->Mov(a64::w10, x86addr.W());
            armAsm->And(a64::w10, a64::w10, 0xF0000000);
            armAsm->Cmp(a64::w10, 0x10000000);
            armAsm->B(&ee_hw_done, a64::Condition::ne);

            armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
            armAsm->Push(a64::x29, a64::lr);
            armAsm->Mov(a64::w0, x86addr.W());
            switch (bits)
            {
                case  8: armEmitCall((void*)memRead8);  break;
                case 16: armEmitCall((void*)memRead16); break;
                case 32: armEmitCall((void*)memRead32); break;
                case 64: armEmitCall((void*)memRead64); break;
                jNO_DEFAULT
            }
            armAsm->Mov(a64::x10, a64::x0);
            armAsm->Pop(a64::lr, a64::x29);
            armAsm->Pop(a64::x3, a64::x2, a64::x1, a64::x0);
            switch (bits)
            {
                case  8: sign ? armAsm->Sxtb(x86reg, a64::w10) : armAsm->Mov(x86reg.W(), a64::w10); break;
                case 16: sign ? armAsm->Sxth(x86reg, a64::w10) : armAsm->Mov(x86reg.W(), a64::w10); break;
                case 32: sign ? armAsm->Sxtw(x86reg, a64::w10) : armAsm->Mov(x86reg.W(), a64::w10); break;
                case 64: armAsm->Mov(x86reg, a64::x10); break;
                jNO_DEFAULT
            }
            armAsm->B(&spad_read_done); // skip fastmem load; spad_read_done always bound

            armBind(&ee_hw_done);
        }

        if (emit_spad_read_bypass)
        {
            // Scratchpad (0x70000000-0x7fffffff) must bypass fastmem direct loads.
            armAsm->Mov(a64::w10, x86addr.W());
            armAsm->And(a64::w10, a64::w10, 0xF0000000);
            armAsm->Cmp(a64::w10, 0x70000000);
            armAsm->B(&spad_do_fastmem, a64::Condition::ne);

            armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
            armAsm->Push(a64::x29, a64::lr);
            armAsm->Mov(a64::w0, x86addr.W());
            switch (bits)
            {
                case 8: armEmitCall((void*)memRead8); break;
                case 16: armEmitCall((void*)memRead16); break;
                case 32: armEmitCall((void*)memRead32); break;
                case 64: armEmitCall((void*)memRead64); break;
                jNO_DEFAULT
            }
            // [iter184] BUG FIX: save memReadXX result to x10 BEFORE Pop restores x0.
            // Without this, Pop(x3,x2,x1,x0) overwrites x0 with its pre-call value,
            // and the switch below reads stale x0 instead of the actual memRead result.
            // KSEG1 bypass already does this correctly (Mov w10, w0 before Pop).
            armAsm->Mov(a64::x10, a64::x0);
            armAsm->Pop(a64::lr, a64::x29);
            armAsm->Pop(a64::x3, a64::x2, a64::x1, a64::x0);
            switch (bits)
            {
                case 8:
                    sign ? armAsm->Sxtb(x86reg, a64::w10) : armAsm->Mov(x86reg.W(), a64::w10);
                    break;
                case 16:
                    sign ? armAsm->Sxth(x86reg, a64::w10) : armAsm->Mov(x86reg.W(), a64::w10);
                    break;
                case 32:
                    sign ? armAsm->Sxtw(x86reg, a64::w10) : armAsm->Mov(x86reg.W(), a64::w10);
                    break;
                case 64:
                    armAsm->Mov(x86reg, a64::x10);
                    break;
                jNO_DEFAULT
            }
            armAsm->B(&spad_read_done);

            armBind(&spad_do_fastmem);
        }

        // [iter71] capture fastmem instr addr AFTER bypass checks
        codeStart = armGetCurrentCodePointer();

        switch (bits)
        {
            case 8:
//			    sign ? xMOVSX(x86reg, ptr8[RFASTMEMBASE + x86addr]) : xMOVZX(xRegister32(x86reg), ptr8[RFASTMEMBASE + x86addr]);
                // [P30/R105] Ldrsb/Ldrsh with 64-bit dest doesn't sign-extend correctly on ARM64.
                // Load into 32-bit temp first, then Sxtb to 64-bit (matching KSEG1 bypass pattern).
                if (sign) { armAsm->Ldrb(x86reg.W(), mop); armAsm->Sxtb(x86reg, x86reg.W()); }
                else { armAsm->Ldrb(x86reg.W(), mop); }
                break;
            case 16:
//			    sign ? xMOVSX(x86reg, ptr16[RFASTMEMBASE + x86addr]) : xMOVZX(xRegister32(x86reg), ptr16[RFASTMEMBASE + x86addr]);
                if (sign) { armAsm->Ldrh(x86reg.W(), mop); armAsm->Sxth(x86reg, x86reg.W()); }
                else { armAsm->Ldrh(x86reg.W(), mop); }
                break;
            case 32:
//			    sign ? xMOVSX(x86reg, ptr32[RFASTMEMBASE + x86addr]) : xMOV(xRegister32(x86reg), ptr32[RFASTMEMBASE + x86addr]);
                sign ? armAsm->Ldrsw(x86reg, mop) : armAsm->Ldr(x86reg.W(), mop);
                break;
            case 64:
//			    xMOV(x86reg, ptr64[RFASTMEMBASE + x86addr]);
                armAsm->Ldr(x86reg, mop);
                break;

            jNO_DEFAULT
        }

        if (emit_kseg1_read_bypass)
            armBind(&kseg1_read_done);
        if (emit_spad_read_bypass)
            armBind(&spad_read_done);
    }
    else
    {
        pxAssert(bits == 32);
        x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
//		codeStart = x86Ptr;
        codeStart = armGetCurrentCodePointer();
//		const xRegisterSSE xmmreg(x86_dest_reg);
        const a64::QRegister xmmreg(x86_dest_reg);
//		xMOVSSZX(xmmreg, ptr32[RFASTMEMBASE + x86addr]);
        armAsm->Ldr(xmmreg.S(), mop);
    }

//	vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(x86Ptr - codeStart),
    vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(armGetCurrentCodePointer() - codeStart),
                          pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
                          static_cast<u8>(addr_reg), static_cast<u8>(x86_dest_reg < 0 ? 0 : x86_dest_reg),
                          static_cast<u8>(bits), sign, true, xmm);

    return x86_dest_reg;
}

// ------------------------------------------------------------------------
// Recompiled input registers:
//   ecx - source address to read from
//   Returns read value in eax.
//
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
//
int vtlb_DynGenReadNonQuad_Const(u32 bits, bool sign, bool xmm, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
    int x86_dest_reg;
    auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
    // [FIX] EE BIOS timer wait-loop: const-path timer reads need memRead32 helper bypass.
    static int s_ee_t0_force_cfg = -1;
    if (s_ee_t0_force_cfg < 0)
    {
        s_ee_t0_force_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_EE_T0_FORCE_MEMREAD32", false) ? 1 : 0;
        Console.WriteLn("@@CFG@@ iPSX2_EE_T0_FORCE_MEMREAD32=%d", s_ee_t0_force_cfg);
    }
    const bool ee_bios_t0_force_memread32 =
        (s_ee_t0_force_cfg == 1 && !xmm && bits == 32 && addr_const == 0xB0000000u &&
         (pc == 0x9FC41044u || pc == 0x9FC4104Cu || pc == 0x9FC4107Cu || pc == 0x9FC41084u));
    // [iter21] 9FC43150 function内の KSEG0 ROM LW もmemRead32 経由へ
    // [iter283] KSEG0 EE RAM (0x80000000-0x81FFFFFF) もadd — LOGO bootstrap/decompressor の
    //           LW/LD が fastmem shadow 未マップで SIGBUS になるため memRead 経由へ迂回する。
    // [FIX] force_bios_414x_no_fastmem_const: on device (dual-mapping), vmap direct load
    // returns wrong data. Must cover ALL PC ranges that other DynGen functions cover:
    // KSEG0 BIOS, KSEG1 BIOS, KSEG0 kernel/RAM, and user-space (SIF wrappers at 0x83xxx etc).
    // [FIX] Removed !xmm restriction: LWC1 (xmm=true, bits=32) const-address reads were
    // bypassing force check, using vmap direct load which returns wrong data on device.
    // BIOS animation code uses FPU ops (LWC1/SWC1) for coordinate calculations — wrong FPR
    // reads cause animations/CG to not render.
    // [P48-2 FIX] READ const: must match WRITE const ranges to avoid store-load mismatch.
    // Without (pc < 0x02000000u), user-space stores go through vtlb (memWriteN) but loads
    // use direct vmap path — if these map different physical pages, LD reads stale data.
    // This caused diagonal line artifacts: SH wrote correct XY to stack, but LD read zeros.
    const bool force_bios_414x_no_fastmem_const =
        IsFixBios414xNoFastmemEnabled() &&
        (bits == 8 || bits == 16 || bits == 32 || bits == 64) &&
        ((pc >= 0x9FC00000u && pc < 0x9FC80000u) ||
         (pc >= 0xBFC00000u && pc < 0xBFC80000u) ||
         (pc >= 0x80000000u && pc < 0x82000000u) ||
         (pc < 0x02000000u));
    // [iter229] FIX: extend KSEG1 MMIO force to ALL bit widths (was bits==32 only).
    // LHU from 0xBA000006 (DVE status) bypassed vtlb handler → ba0R16 never called.
    const bool force_kseg1_mmio_memread =
        (!xmm && (bits == 8 || bits == 16 || bits == 32) && ((addr_const & 0xE0000000u) == 0xA0000000u)) ||
        ee_bios_t0_force_memread32 || force_bios_414x_no_fastmem_const;
    const bool force_spad_memread =
        (!xmm && (bits == 8 || bits == 16 || bits == 32 || bits == 64) &&
         ((addr_const & 0xF0000000u) == 0x70000000u));

    if (force_kseg1_mmio_memread || force_spad_memread)
    {
        iFlushCall(FLUSH_FULLVTLB);
        armAsm->Mov(EAX, addr_const);
        // [iter229] unified per-bits dispatch for both KSEG1 MMIO and scratchpad
        switch (bits)
        {
            case  8: armEmitCall((void*)memRead8); break;
            case 16: armEmitCall((void*)memRead16); break;
            case 32: armEmitCall((void*)memRead32); break;
            case 64: armEmitCall((void*)memRead64); break;
            jNO_DEFAULT
        }

        if (!xmm)
        {
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), -1);
            auto regX = (x86_dest_reg < 0 ? RAX : HostX(x86_dest_reg));
            switch (bits)
            {
                case 8:
                    sign ? armAsm->Sxtb(regX, EAX) : armAsm->Mov(regX.W(), EAX);
                    break;
                case 16:
                    sign ? armAsm->Sxth(regX, EAX) : armAsm->Mov(regX.W(), EAX);
                    break;
                case 32:
                    sign ? armAsm->Sxtw(regX, EAX) : armAsm->Mov(regX.W(), EAX);
                    break;
                case 64:
                    armAsm->Mov(regX, RAX);
                    break;
                jNO_DEFAULT
            }
        }
        else
        {
            // [FIX] XMM (FPR) read: memRead32 returned raw bits in w0 (EAX).
            // Move to XMM register via FMOV (bitwise copy, no float conversion).
            // This handles LWC1 const-address reads on device where vmap direct load
            // returns wrong data.
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
            armAsm->Fmov(a64::SRegister(x86_dest_reg), EAX);
        }
    }
    else if (!CHECK_FASTMEM)
    {
        // [P49] Fastmem OFF: all reads must go through memReadN (same as Dynamic !CHECK_FASTMEM path).
        // Vmap direct load can return stale data when fastmem fault handler is not available.
        iFlushCall(FLUSH_FULLVTLB);
        armAsm->Mov(EAX, addr_const);
        if (!xmm)
        {
            switch (bits)
            {
                case  8: armEmitCall((void*)(sign ? (void*)memRead8  : (void*)memRead8)); break;
                case 16: armEmitCall((void*)(sign ? (void*)memRead16 : (void*)memRead16)); break;
                case 32: armEmitCall((void*)memRead32); break;
                case 64: armEmitCall((void*)memRead64); break;
                jNO_DEFAULT
            }
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), -1);
            auto regX = (x86_dest_reg < 0 ? RAX : HostX(x86_dest_reg));
            if (bits == 64)
                armAsm->Mov(regX, RAX);
            else if (sign) {
                switch (bits) {
                    case  8: armAsm->Sxtb(regX, EAX); break;
                    case 16: armAsm->Sxth(regX, EAX); break;
                    case 32: armAsm->Sxtw(regX, EAX); break;
                }
            }
            else
                armAsm->Mov(regX.W(), EAX);
        }
        else
        {
            switch (bits) {
                case 32: armEmitCall((void*)memRead32); break;
                case 64: armEmitCall((void*)memRead64); break;
                jNO_DEFAULT
            }
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
            armAsm->Fmov(a64::SRegister(x86_dest_reg), EAX);
        }
    }
    else if (!vmv.isHandler(addr_const))
    {

        armAsm->Mov(ECX, addr_const);
        armAsm->Mov(EAX, ECX);
        armAsm->Lsr(EAX, EAX, VTLB_PAGE_BITS);
        armAsm->Ldr(RXVIXLSCRATCH, PTR_CPU(vtlbdata.vmap));
        armAsm->Ldr(RAX, a64::MemOperand(RXVIXLSCRATCH, RAX, a64::LSL, 3));
        armAsm->Add(RCX, RCX, RAX);
        auto mop = a64::MemOperand(RCX);

        if (!xmm)
        {
//			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), -1);

            auto regX = (x86_dest_reg < 0 ? RAX : HostX(x86_dest_reg));
            switch (bits)
            {
                case 8:
//				sign ? xMOVSX(xRegister64(x86_dest_reg), ptr8[(u8*)ppf]) : xMOVZX(xRegister32(x86_dest_reg), ptr8[(u8*)ppf]);
                    // [P30/R105] Ldrb + Sxtb pattern for correct sign extension
                    if (sign) { armAsm->Ldrb(regX.W(), mop); armAsm->Sxtb(regX, regX.W()); }
                    else { armAsm->Ldrb(regX.W(), mop); }
                    break;

                case 16:
//				sign ? xMOVSX(xRegister64(x86_dest_reg), ptr16[(u16*)ppf]) : xMOVZX(xRegister32(x86_dest_reg), ptr16[(u16*)ppf]);
                    if (sign) { armAsm->Ldrh(regX.W(), mop); armAsm->Sxth(regX, regX.W()); }
                    else { armAsm->Ldrh(regX.W(), mop); }
                    break;

                case 32:
//				sign ? xMOVSX(xRegister64(x86_dest_reg), ptr32[(u32*)ppf]) : xMOV(xRegister32(x86_dest_reg), ptr32[(u32*)ppf]);
                    sign ? armAsm->Ldrsw(regX, mop) : armAsm->Ldr(regX.W(), mop);
                    break;

                case 64:
//				xMOV(xRegister64(x86_dest_reg), ptr64[(u64*)ppf]);
                    armAsm->Ldr(regX, mop);
                    break;
            }
        }
        else
        {
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
//			xMOVSSZX(xRegisterSSE(x86_dest_reg), ptr32[(float*)ppf]);
            armAsm->Ldr(a64::QRegister(x86_dest_reg).S(), mop);
        }
    }
    else
    {
        // has to: translate, find function, call function
        u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

        int szidx = 0;
        switch (bits)
        {
            case  8: szidx = 0; break;
            case 16: szidx = 1; break;
            case 32: szidx = 2; break;
            case 64: szidx = 3; break;
        }

        // Shortcut for the INTC_STAT register, which many games like to spin on heavily.
        if ((bits == 32) && !EmuConfig.Speedhacks.IntcStat && (paddr == INTC_STAT))
        {
            auto mop = armMemOperandPtr(&psHu32(INTC_STAT));

//			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), -1);
            if (!xmm)
            {
                auto regX = (x86_dest_reg < 0 ? RAX : HostX(x86_dest_reg));
                if (sign) {
//                    xMOVSX(xRegister64(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
                    armAsm->Ldr(regX, mop);
                }
                else {
//                    xMOV(xRegister32(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
                    armAsm->Ldr(regX.W(), mop);
                }
            }
            else
            {
//				xMOVDZX(xRegisterSSE(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
                armAsm->Ldr(a64::QRegister(x86_dest_reg).S(), mop);
            }
        }
        else
        {
            iFlushCall(FLUSH_FULLVTLB);
//			xFastCall(vmv.assumeHandlerGetRaw(szidx, false), paddr);

            void* handler_func = vmv.assumeHandlerGetRaw(szidx, false);
            armAsm->Mov(EAX, paddr); // Handler arg1 = paddr
            armEmitCall(handler_func); // read

            // Debug Logs Removed


            if (!xmm)
            {
//				x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
                x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), -1);

                auto regX = (x86_dest_reg < 0 ? RAX : HostX(x86_dest_reg));
                switch (bits)
                {
                    // save REX prefix by using 32bit dest for zext
                    case 8:
//					sign ? xMOVSX(xRegister64(x86_dest_reg), al) : xMOVZX(xRegister32(x86_dest_reg), al);
                        sign ? armAsm->Sxtb(regX, EAX) : armAsm->Uxtb(regX.W(), EAX);
                        break;

                    case 16:
//					sign ? xMOVSX(xRegister64(x86_dest_reg), ax) : xMOVZX(xRegister32(x86_dest_reg), ax);
                        sign ? armAsm->Sxth(regX, EAX) : armAsm->Uxth(regX.W(), EAX);
                        break;

                    case 32:
//					sign ? xMOVSX(xRegister64(x86_dest_reg), eax) : xMOV(xRegister32(x86_dest_reg), eax);
                        sign ? armAsm->Sxtw(regX, EAX) : armAsm->Mov(regX.W(), EAX);
                        break;

                    case 64:
//					xMOV(xRegister64(x86_dest_reg), rax);
                        armAsm->Mov(regX , RAX);
                        break;
                }
            }
            else
            {
                x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
//				xMOVDZX(xRegisterSSE(x86_dest_reg), eax);
                armAsm->Fmov(a64::QRegister(x86_dest_reg).S(), EAX);
            }
        }
    }

    return x86_dest_reg;
}

int vtlb_DynGenReadQuad(u32 bits, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	pxAssume(bits == 128);

	// [FIX] Add force_bios_414x_no_fastmem check for 128-bit reads (LQ).
	// Without this, LQ from BIOS ROM reads through fastmem which returns wrong data
	// on device (dual-mapping), causing exception vector code to be all zeros.
	// [P48] READ128: same range as READ dynamic (BIOS ROM + kernel RAM).
	const bool force_bios_read128 =
		IsFixBios414xNoFastmemEnabled() &&
		((pc >= 0x9FC00000u && pc < 0x9FC80000u) ||
		 (pc >= 0xBFC00000u && pc < 0xBFC80000u) ||
		 (pc >= 0x80000000u && pc < 0x82000000u));

	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc) || force_bios_read128)
	{
		iFlushCall(FLUSH_FULLVTLB);

		// [P15] FIX: DynGen_HandlerTest broken on ARM64. Call vtlb_memRead128 directly.
		// Removal condition: なし（恒久fix）
		armAsm->Mov(EAX, a64::WRegister(ECX.GetCode()));
		armEmitCall((void*)vtlb_memRead128);

		const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		if (reg >= 0 && reg != 0) {
            armAsm->Mov(a64::QRegister(reg), a64::QRegister(0));
        }

		return reg;
	}

	const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0); // Handler returns in xmm0
	const u8* codeStart = armGetCurrentCodePointer();

//	xMOVAPS(xRegisterSSE(reg), ptr128[RFASTMEMBASE + arg1reg]);
    armAsm->Ldr(a64::QRegister(reg).Q(), a64::MemOperand(RFASTMEMBASE, RCX.W(), a64::UXTW));

	vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(armGetCurrentCodePointer() - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(RCX.GetCode()), static_cast<u8>(reg),
		static_cast<u8>(bits), false, true, true);

	return reg;
}


// ------------------------------------------------------------------------
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
int vtlb_DynGenReadQuad_Const(u32 bits, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	pxAssert(bits == 128);

	EE::Profiler.EmitConstMem(addr_const);

	// [FIX] force_bios_read128_const: same as vtlb_DynGenReadQuad's force_bios_read128.
	// On device (dual-mapping), vmap direct load returns wrong data for BIOS ROM reads.
	// [P48] READ128 const: same range as READ dynamic (BIOS ROM + kernel RAM).
	const bool force_bios_read128_const =
		IsFixBios414xNoFastmemEnabled() &&
		((pc >= 0x9FC00000u && pc < 0x9FC80000u) ||
		 (pc >= 0xBFC00000u && pc < 0xBFC80000u) ||
		 (pc >= 0x80000000u && pc < 0x82000000u));

	int reg;
	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	// [P49] When fastmem is OFF, all reads must go through vtlb_memRead128 (same as Dynamic path).
	// Without this, const-addr LQ from user-space uses vmap direct load which can return stale data.
	if (!CHECK_FASTMEM)
	{
		// [P49] Fastmem OFF: all 128-bit reads must go through vtlb_memRead128.
		iFlushCall(FLUSH_FULLVTLB);
		armAsm->Mov(EAX, addr_const);
		armEmitCall((void*)vtlb_memRead128);
		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		if (reg >= 0 && reg != 0)
			armAsm->Mov(a64::QRegister(reg), a64::QRegister(0));
	}
	else if (!vmv.isHandler(addr_const) && !force_bios_read128_const)
	{
//		void* ppf = reinterpret_cast<void*>(vmv.assumePtr(addr_const));
		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		if (reg >= 0) {
//            xMOVAPS(xRegisterSSE(reg), ptr128[ppf]);
//            armAsm->Ldr(a64::QRegister(reg).Q(), armMemOperandPtr(ppf));

            armAsm->Mov(ECX, addr_const);
            armAsm->Mov(EAX, ECX);
            armAsm->Lsr(EAX, EAX, VTLB_PAGE_BITS);
            armAsm->Ldr(RXVIXLSCRATCH, PTR_CPU(vtlbdata.vmap));
            armAsm->Ldr(RAX, a64::MemOperand(RXVIXLSCRATCH, RAX, a64::LSL, 3));
            armAsm->Add(RCX, RCX, RAX);
            armAsm->Ldr(a64::QRegister(reg).Q(), a64::MemOperand(RCX));
        }
	}
	else if (force_bios_read128_const)
	{
		// Slow path: call vtlb_memRead128 to avoid vmap direct load issues on device
		iFlushCall(FLUSH_FULLVTLB);
		armAsm->Mov(EAX, addr_const);
		armEmitCall((void*)vtlb_memRead128);
		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		if (reg >= 0 && reg != 0) {
			armAsm->Mov(a64::QRegister(reg), a64::QRegister(0));
		}
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		const int szidx = 4;
		iFlushCall(FLUSH_FULLVTLB);

//		xFastCall(vmv.assumeHandlerGetRaw(szidx, 0), paddr);
        armAsm->Mov(EAX, paddr);
        armEmitCall(vmv.assumeHandlerGetRaw(szidx, 0));

		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
//		xMOVAPS(xRegisterSSE(reg), xmm0);
        armAsm->Mov(a64::QRegister(reg), xmm0);
	}

	return reg;
}

//////////////////////////////////////////////////////////////////////////////////////////
//                            Dynarec Store Implementations

void vtlb_DynGenWrite(u32 sz, bool xmm, int addr_reg, int value_reg)
{
#ifdef LOG_STORES
	{
		xSUB(rsp, 16 * 16);
		for (u32 i = 0; i < 16; i++)
			xMOVAPS(ptr[rsp + i * 16], xRegisterSSE::GetInstance(i));
		for (const auto& reg : {rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15, rbp})
			xPUSH(reg);

		xPUSH(xRegister64(addr_reg));
		xPUSH(xRegister64(value_reg));
		xPUSH(arg1reg);
		xPUSH(arg2reg);
		xMOV(arg1regd, xRegister32(addr_reg));
		if (xmm)
		{
			xSUB(rsp, 32 + 32);
			xMOVAPS(ptr[rsp + 32], xRegisterSSE::GetInstance(value_reg));
			xMOVAPS(ptr[rsp + 48], xRegisterSSE::GetArgRegister(1, 0));
			if (sz < 128)
				xPSHUF.D(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg), 0);
			else
				xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg));
			xFastCall((void*)LogWriteQuad);
			xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), ptr[rsp + 48]);
			xMOVAPS(xRegisterSSE::GetInstance(value_reg), ptr[rsp + 32]);
			xADD(rsp, 32 + 32);
		}
		else
		{
			xMOV(arg2reg, xRegister64(value_reg));
			if (sz == 8)
				xAND(arg2regd, 0xFF);
			else if (sz == 16)
				xAND(arg2regd, 0xFFFF);
			else if (sz == 32)
				xAND(arg2regd, -1);
			xSUB(rsp, 32);
			xFastCall((void*)LogWrite);
			xADD(rsp, 32);
		}
		xPOP(arg2reg);
		xPOP(arg1reg);
		xPOP(xRegister64(value_reg));
		xPOP(xRegister64(addr_reg));

		for (const auto& reg : {rbp, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rbx})
			xPOP(reg);

		for (u32 i = 0; i < 16; i++)
			xMOVAPS(xRegisterSSE::GetInstance(i), ptr[rsp + i * 16]);
		xADD(rsp, 16 * 16);
	}
#endif

    // [P12-ROOT0] Extend to sz==8 and sz==16 as well:
    // BIOS ROM code (0x9FC...) may do 8/16-bit stores to HW regs; fastmem fails silently
    // because DynGen_HandlerTest is broken on ARM64 (iter220 finding).
    // Covering all write sizes ensures memWriteN is used for all BIOS ROM stores.
    // Also extend to KSEG0 EE RAM range (0x80000000-0x82000000) for kernel init stores.
    // [force_bios SQ128 fix] Also cover sz==128 (SQ/SQC2) from BIOS ROM:
    // - BIOS ROM kernel copy uses SQ from PC=0xBFC... (KSEG1 mirror).
    // - fastmem SIGBUS + DynGen_HandlerTest (broken on ARM64) = silent write failure.
    // - Adding xmm&&sz==128 here bypasses fastmem entirely; paired with direct
    //   vtlb_memWrite128 call below instead of broken DynGen_HandlerTest.
    // [iter681] Also cover kuseg PCs (0x00000000-0x02000000): BIOS SIF code dispatched
    // via SYSCALL runs from kuseg function pointers. Stores from these PCs to DMA tag
    // memory silently fail via fastmem (backpatch broken on ARM64).
    // [FIX] Added (xmm && sz == 32) for SWC1: FPR 32-bit stores were bypassing force check.
    const bool force_bios_414x_no_fastmem =
        IsFixBios414xNoFastmemEnabled() &&
        ((!xmm && (sz == 8 || sz == 16 || sz == 32 || sz == 64)) || (xmm && (sz == 32 || sz == 128))) &&
        ((pc >= 0x9FC00000u && pc < 0x9FC80000u) ||
         (pc >= 0xBFC00000u && pc < 0xBFC80000u) ||
         (pc >= 0x80000000u && pc < 0x82000000u) ||
         (pc < 0x02000000u));

	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc) || force_bios_414x_no_fastmem
		|| ipsx2_fastmem_dbg::disable_for_bits(sz)
		|| ipsx2_fastmem_dbg::disable_write_path()
		|| ipsx2_fastmem_dbg::disable_write_for_bits(sz)
		|| ipsx2_fastmem_dbg::disable_write_split(sz, xmm)
		|| ipsx2_fastmem_dbg::apply_sw_fastmem_fix(sz, xmm))
	{
		iFlushCall(FLUSH_FULLVTLB);

		// [iter230] FIX: DynGen_HandlerTest is broken on ARM64 (handler call+Ret doesn't
		// return to JIT code). Use direct memWriteN calls for non-XMM, sz<=64.
		if (!xmm && sz <= 64)
		{
			armAsm->Mov(a64::w0, a64::WRegister(addr_reg));
			switch (sz)
			{
				case 8:
					armAsm->Mov(a64::w1, HostW(value_reg));
					armAsm->And(a64::w1, a64::w1, 0xFF);
					armEmitCall((void*)memWrite8);
					break;
				case 16:
					armAsm->Mov(a64::w1, HostW(value_reg));
					armAsm->And(a64::w1, a64::w1, 0xFFFF);
					armEmitCall((void*)memWrite16);
					break;
				case 32:
					armAsm->Mov(a64::w1, HostW(value_reg));
					armEmitCall((void*)memWrite32);
					break;
				case 64:
					armAsm->Mov(a64::x1, HostX(value_reg));
					armEmitCall((void*)memWrite64);
					break;
				jNO_DEFAULT
			}
		}
		else if (xmm && sz == 128)
		{
			// [force_bios SQ128 fix] DynGen_HandlerTest is broken on ARM64 for sz=128.
			// Call vtlb_memWrite128 directly: ARM64 AAPCS64 w0=mem(u32), v0=value(r128/uint32x4_t).
			// addr_reg is a physical ARM64 register (ECX.GetCode()=1=w1).
			// value_reg is an XMM slot number; q(value_reg) holds the 128-bit value.
			armAsm->Mov(a64::w0, a64::WRegister(addr_reg));
			if (value_reg != 0)
				armAsm->Mov(armQRegister(0), armQRegister(value_reg));
			armEmitCall((void*)vtlb_memWrite128);
		}
		else
		{
			// [P15] FIX: DynGen_HandlerTest broken on ARM64. Use direct memWrite calls.
			// This handles remaining non-XMM non-128 cases that fell through.
			// Removal condition: なし（恒久fix）
			armAsm->Mov(a64::w0, a64::WRegister(addr_reg));
			if (xmm)
			{
				// FPR store
				if (sz == 64)
					armAsm->Fmov(a64::x1, a64::DRegister(value_reg));
				else
					armAsm->Fmov(a64::w1, a64::SRegister(value_reg));
			}
			else
			{
				if (sz == 64)
					armAsm->Mov(a64::x1, HostX(value_reg));
				else
					armAsm->Mov(a64::w1, HostW(value_reg));
			}
			switch (sz) {
				case  8: armAsm->And(a64::w1, a64::w1, 0xFF); armEmitCall((void*)memWrite8); break;
				case 16: armAsm->And(a64::w1, a64::w1, 0xFFFF); armEmitCall((void*)memWrite16); break;
				case 32: armEmitCall((void*)memWrite32); break;
				case 64: armEmitCall((void*)memWrite64); break;
				default: pxAssert(false); break;
			}
		}
		return;
	}

//	const xAddressReg vaddr_reg(addr_reg);
    // [iPSX2] @@WRITE_ADDR_REG_PHYS_FIX@@ (iter174): addr_reg is a physical ARM64 register
    // number (ECX.GetCode()=1 = physical w1/x1), never a JIT slot. With ENABLE_SLOT7_GUARD active,
    // HostX(1)=XRegister(kSlotToPhys[1])=x20 (s3 JIT slot) instead of x1=ECX. Same bug as iter172
    // (READ fastmem). Fix: treat addr_reg as physical register number throughout vtlb_DynGenWrite.
    const a64::XRegister vaddr_reg = a64::XRegister(addr_reg);
    auto mop = a64::MemOperand(RFASTMEMBASE, vaddr_reg.W(), a64::UXTW);

    a64::Label kseg1_do_fastmem;
    a64::Label kseg1_store_done;
    a64::Label spad_do_fastmem;
    a64::Label spad_store_done;
    const bool emit_spad_write_bypass = (!xmm && (sz == 8 || sz == 16 || sz == 32 || sz == 64));
    if (!xmm && sz == 32)
    {
        // KSEG1 MMIO writes must not go through fastmem direct stores.
        armAsm->Mov(a64::w10, a64::WRegister(addr_reg));
        armAsm->And(a64::w10, a64::w10, 0xE0000000);
        armAsm->Cmp(a64::w10, 0xA0000000);
        armAsm->B(&kseg1_do_fastmem, a64::Condition::ne);

        armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
        armAsm->Push(a64::x29, a64::lr);
        armAsm->Mov(a64::w0, a64::WRegister(addr_reg));
        // [iter226] value_reg is a slot index on iOS/ARM64. Use HostW for slot→phys.
        armAsm->Mov(a64::w1, HostW(value_reg));
        armEmitCall((void*)vtlb_MemWrite32_KSEG1);
        armAsm->Pop(a64::lr, a64::x29);
        armAsm->Pop(a64::x3, a64::x2, a64::x1, a64::x0);
        armAsm->B(&kseg1_store_done);

        armBind(&kseg1_do_fastmem);
    }
    // [iter672] EE HW register write bypass: addresses 0x10000000-0x1000FFFF
    // must go through memWrite32 → hwWrite32 which has special semantics
    // (e.g. SBUS_F230 does &= ~value for bit-clear, not direct store).
    // Without this, fastmem direct stores bypass hwWrite32 entirely.
    // Removal condition: なし (permanent fix — HW register writes must always use handler)
    a64::Label eehw_do_fastmem;
    a64::Label eehw_store_done;
    if (emit_spad_write_bypass)
    {
        armAsm->Mov(a64::w10, a64::WRegister(addr_reg));
        armAsm->Lsr(a64::w10, a64::w10, 16);  // top 16 bits
        armAsm->Cmp(a64::w10, 0x1000);         // 0x1000xxxx range
        armAsm->B(&eehw_do_fastmem, a64::Condition::ne);

        armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
        armAsm->Push(a64::x29, a64::lr);
        armAsm->Mov(a64::w0, a64::WRegister(addr_reg));
        switch (sz) {
            case 8:
                armAsm->Mov(a64::w1, HostW(value_reg));
                armAsm->And(a64::w1, a64::w1, 0xFF);
                armEmitCall((void*)memWrite8);
                break;
            case 16:
                armAsm->Mov(a64::w1, HostW(value_reg));
                armAsm->And(a64::w1, a64::w1, 0xFFFF);
                armEmitCall((void*)memWrite16);
                break;
            case 32:
                armAsm->Mov(a64::w1, HostW(value_reg));
                armEmitCall((void*)memWrite32);
                break;
            case 64:
                armAsm->Mov(a64::x1, HostX(value_reg));
                armEmitCall((void*)memWrite64);
                break;
        }
        armAsm->Pop(a64::lr, a64::x29);
        armAsm->Pop(a64::x3, a64::x2, a64::x1, a64::x0);
        armAsm->B(&eehw_store_done);

        armBind(&eehw_do_fastmem);
    }
    if (emit_spad_write_bypass)
    {
        // Scratchpad (0x70000000-0x7fffffff) must bypass fastmem direct stores.
        armAsm->Mov(a64::w10, a64::WRegister(addr_reg));
        armAsm->And(a64::w10, a64::w10, 0xF0000000);
        armAsm->Cmp(a64::w10, 0x70000000);
        armAsm->B(&spad_do_fastmem, a64::Condition::ne);

        armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
        armAsm->Push(a64::x29, a64::lr);
        armAsm->Mov(a64::w0, a64::WRegister(addr_reg));
        switch (sz)
        {
            // [iter226] value_reg is a slot index on iOS/ARM64. Use HostW/HostX for slot→phys.
            case 8:
                armAsm->Mov(a64::w1, HostW(value_reg));
                armAsm->And(a64::w1, a64::w1, 0xFF);
                armEmitCall((void*)memWrite8);
                break;
            case 16:
                armAsm->Mov(a64::w1, HostW(value_reg));
                armAsm->And(a64::w1, a64::w1, 0xFFFF);
                armEmitCall((void*)memWrite16);
                break;
            case 32:
                armAsm->Mov(a64::w1, HostW(value_reg));
                armEmitCall((void*)memWrite32);
                break;
            case 64:
                armAsm->Mov(a64::x1, HostX(value_reg));
                armEmitCall((void*)memWrite64);
                break;
            jNO_DEFAULT
        }
        armAsm->Pop(a64::lr, a64::x29);
        armAsm->Pop(a64::x3, a64::x2, a64::x1, a64::x0);
        armAsm->B(&spad_store_done);

        armBind(&spad_do_fastmem);
    }

    // [iter71] capture fastmem instr addr AFTER bypass checks so vtlb_AddLoadStoreInfo
    // registers the address of the actual Str/Ldr, not the start of the bypass sequence.
    const u8* codeStart = armGetCurrentCodePointer();

    if (!xmm)
    {
        // [iter226] FIX: value_reg is a SLOT index on iOS/ARM64 (from _allocX86reg),
        // NOT a physical register number. The iter176 comment was wrong.
        // DynGen_PrepRegs line 170 already uses HostX(value_reg) correctly.
        // Fastmem path must use the same slot→phys conversion.
        auto regX = HostX(value_reg);
        switch (sz)
        {
            case 8:
//			    xMOV(ptr8[RFASTMEMBASE + vaddr_reg], xRegister8(xRegister32(value_reg)));
                armAsm->Strb(regX.W(), mop);
                break;
            case 16:
//			    xMOV(ptr16[RFASTMEMBASE + vaddr_reg], xRegister16(value_reg));
                armAsm->Strh(regX.W(), mop);
                break;
            case 32:
//			    xMOV(ptr32[RFASTMEMBASE + vaddr_reg], xRegister32(value_reg));
                armAsm->Str(regX.W(), mop);
                break;
            case 64:
//			    xMOV(ptr64[RFASTMEMBASE + vaddr_reg], xRegister64(value_reg));
                armAsm->Str(regX, mop);
                break;

            jNO_DEFAULT
        }
    }
    else
    {
        pxAssert(sz == 32 || sz == 128);

        auto regQ = a64::QRegister(value_reg);
        switch (sz)
        {
            case 32:
//			    xMOVSS(ptr32[RFASTMEMBASE + vaddr_reg], xRegisterSSE(value_reg));
                armAsm->Str(regQ.S(), mop);
                break;
            case 128:
//			    xMOVAPS(ptr128[RFASTMEMBASE + vaddr_reg], xRegisterSSE(value_reg));
                armAsm->Str(regQ.Q(), mop);
                break;

            jNO_DEFAULT
        }
    }

    if (!xmm && sz == 32)
        armBind(&kseg1_store_done);
    if (emit_spad_write_bypass)
    {
        armBind(&eehw_store_done);
        armBind(&spad_store_done);
    }

	vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(armGetCurrentCodePointer() - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(addr_reg), static_cast<u8>(value_reg),
		static_cast<u8>(sz), false, false, xmm);
}


// ------------------------------------------------------------------------
// Generates code for a store instruction, where the address is a known constant.
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
void vtlb_DynGenWrite_Const(u32 bits, bool xmm, u32 addr_const, int value_reg)
{
	EE::Profiler.EmitConstMem(addr_const);

#ifdef LOG_STORES
	{
		xSUB(rsp, 16 * 16);
		for (u32 i = 0; i < 16; i++)
			xMOVAPS(ptr[rsp + i * 16], xRegisterSSE::GetInstance(i));
		for (const auto& reg : { rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15, rbp })
			xPUSH(reg);

		xPUSH(xRegister64(value_reg));
		xPUSH(xRegister64(value_reg));
		xPUSH(arg1reg);
		xPUSH(arg2reg);
		xMOV(arg1reg, addr_const);
		if (xmm)
		{
			xSUB(rsp, 32 + 32);
			xMOVAPS(ptr[rsp + 32], xRegisterSSE::GetInstance(value_reg));
			xMOVAPS(ptr[rsp + 48], xRegisterSSE::GetArgRegister(1, 0));
			if (bits < 128)
				xPSHUF.D(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg), 0);
			else
				xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg));
			xFastCall((void*)LogWriteQuad);
			xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), ptr[rsp + 48]);
			xMOVAPS(xRegisterSSE::GetInstance(value_reg), ptr[rsp + 32]);
			xADD(rsp, 32 + 32);
		}
		else
		{
			xMOV(arg2reg, xRegister64(value_reg));
			if (bits == 8)
				xAND(arg2regd, 0xFF);
			else if (bits == 16)
				xAND(arg2regd, 0xFFFF);
			else if (bits == 32)
				xAND(arg2regd, -1);
			xSUB(rsp, 32);
			xFastCall((void*)LogWrite);
			xADD(rsp, 32);
		}
		xPOP(arg2reg);
		xPOP(arg1reg);
		xPOP(xRegister64(value_reg));
		xPOP(xRegister64(value_reg));

		for (const auto& reg : {rbp, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rbx})
			xPOP(reg);

		for (u32 i = 0; i < 16; i++)
			xMOVAPS(xRegisterSSE::GetInstance(i), ptr[rsp + i * 16]);
		xADD(rsp, 16 * 16);
	}
#endif

    // @@WRITE64_CONST_ENTRY@@ vtlb_DynGenWrite_Const エントリprobe（bits==64 全経路）
    // Removal condition: SD $ra のcompile経路が確定した時点
    if (bits == 64 && !xmm)
    {
        static int s_wce_cnt = 0;
        if (s_wce_cnt < 20)
        {
            bool is_hdlr = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS].isHandler(addr_const);
            bool is_spad = (addr_const & 0xF0000000u) == 0x70000000u;
            Console.WriteLn("@@WRITE64_CONST_ENTRY@@ #%d pc=%08x addr=%08x is_handler=%d is_spad=%d vr=%d",
                s_wce_cnt, pc, addr_const, (int)is_hdlr, (int)is_spad, value_reg);
            s_wce_cnt++;
        }
    }

	// [iter251] @@KSEG3_WRITE_CONST@@ phys 0x78000 ストアの JIT compile時detect
	// Removal condition: BIOS ROM → phys 0x78000 ストア経路after confirmed
	{
		u32 phys_const = addr_const & 0x1FFFFFFFu;
		if (phys_const >= 0x00078000u && phys_const < 0x00079000u) {
			static u32 s_kwc_n = 0;
			if (s_kwc_n < 20)
				Console.WriteLn("@@KSEG3_WRITE_CONST@@ n=%u pc=%08x addr=%08x phys=%08x bits=%u",
					s_kwc_n++, pc, addr_const, phys_const, bits);
		}
	}

	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];

	if (!vmv.isHandler(addr_const) && (!CHECK_FASTMEM
		|| ipsx2_fastmem_dbg::disable_for_bits(bits)
		|| ipsx2_fastmem_dbg::disable_write_path()
		|| ipsx2_fastmem_dbg::disable_write_for_bits(bits)
		|| ipsx2_fastmem_dbg::disable_write_split(bits, xmm)
		|| ipsx2_fastmem_dbg::apply_sw_fastmem_fix(bits, xmm)))
	{
		// [P49] Fastmem OFF: all writes must go through memWriteN.
		iFlushCall(FLUSH_FULLVTLB);
		armAsm->Mov(a64::w0, addr_const);
		if (!xmm)
		{
			switch (bits)
			{
				case 8:
					armAsm->Mov(a64::w1, HostW(value_reg));
					armAsm->And(a64::w1, a64::w1, 0xFF);
					armEmitCall((void*)memWrite8);
					break;
				case 16:
					armAsm->Mov(a64::w1, HostW(value_reg));
					armAsm->And(a64::w1, a64::w1, 0xFFFF);
					armEmitCall((void*)memWrite16);
					break;
				case 32:
					armAsm->Mov(a64::w1, HostW(value_reg));
					armEmitCall((void*)memWrite32);
					break;
				case 64:
					armAsm->Mov(a64::x1, HostX(value_reg));
					armEmitCall((void*)memWrite64);
					break;
				jNO_DEFAULT
			}
		}
		else
		{
			if (bits == 32)
			{
				armAsm->Fmov(ECX, a64::SRegister(value_reg));
				armEmitCall((void*)memWrite32);
			}
			else
			{
				if (value_reg != 0)
					armAsm->Mov(armQRegister(0), armQRegister(value_reg));
				armEmitCall((void*)vtlb_memWrite128);
			}
		}
		return;
	}

	if (!vmv.isHandler(addr_const))
	{
        const bool force_spad_memwrite =
            (!xmm && (bits == 8 || bits == 16 || bits == 32 || bits == 64) &&
             ((addr_const & 0xF0000000u) == 0x70000000u));
        // [FIX] force_bios_414x_no_fastmem for WRITE const path.
        // vtlb_DynGenWrite (non-const) already has this check, but vtlb_DynGenWrite_Const
        // was missing it. Without this, const-address stores from BIOS ROM to KSEG0 RAM
        // (e.g., exception vector writes to 0x80000080/0x80000180) use the vmap direct store
        // which silently fails on device (dual-mapping). Route through memWriteN instead.
        // [FIX] Added (xmm && bits == 32) for SWC1 const-address FPR stores.
        const bool force_bios_write_const =
            IsFixBios414xNoFastmemEnabled() &&
            ((!xmm && (bits == 8 || bits == 16 || bits == 32 || bits == 64)) || (xmm && (bits == 32 || bits == 128))) &&
            ((pc >= 0x9FC00000u && pc < 0x9FC80000u) ||
             (pc >= 0xBFC00000u && pc < 0xBFC80000u) ||
             (pc >= 0x80000000u && pc < 0x82000000u) ||
             (pc < 0x02000000u));
        if (force_spad_memwrite || force_bios_write_const)
        {
            iFlushCall(FLUSH_FULLVTLB);
            armAsm->Mov(EAX, addr_const);
            if (!xmm)
            {
                switch (bits)
                {
                    case 8:
                        armAsm->Mov(ECX, HostW(value_reg));
                        armAsm->And(ECX, ECX, 0xFF);
                        armEmitCall((void*)memWrite8);
                        break;
                    case 16:
                        armAsm->Mov(ECX, HostW(value_reg));
                        armAsm->And(ECX, ECX, 0xFFFF);
                        armEmitCall((void*)memWrite16);
                        break;
                    case 32:
                        armAsm->Mov(ECX, HostW(value_reg));
                        armEmitCall((void*)memWrite32);
                        break;
                    case 64:
                        armAsm->Mov(RCX, HostX(value_reg));
                        armEmitCall((void*)memWrite64);
                        break;
                    jNO_DEFAULT
                }
            }
            else
            {
                armAsm->Mov(a64::w0, addr_const);
                if (bits == 32)
                {
                    // [FIX] SWC1 const-address: 32-bit FPR store via memWrite32.
                    // FMOV copies float bits to GPR w1 (no conversion).
                    armAsm->Fmov(ECX, a64::SRegister(value_reg));
                    armEmitCall((void*)memWrite32);
                }
                else
                {
                    // 128-bit (SQ) const-address store
                    if (value_reg != 0)
                        armAsm->Mov(armQRegister(0), armQRegister(value_reg));
                    armEmitCall((void*)vtlb_memWrite128);
                }
            }
            return;
        }

//		auto ppf = vmv.assumePtr(addr_const);
//        a64::MemOperand mop = armMemOperandPtr((u8*)ppf);

        armAsm->Mov(ECX, addr_const);
        armAsm->Mov(EAX, ECX);
        armAsm->Lsr(EAX, EAX, VTLB_PAGE_BITS);
        armAsm->Ldr(RXVIXLSCRATCH, PTR_CPU(vtlbdata.vmap));
        armAsm->Ldr(RAX, a64::MemOperand(RXVIXLSCRATCH, RAX, a64::LSL, 3));
        armAsm->Add(RCX, RCX, RAX);
        auto mop = a64::MemOperand(RCX);

		if (!xmm)
		{
            auto regX = HostX(value_reg);
			switch (bits)
			{
				case 8:
//					xMOV(ptr[(void*)ppf], xRegister8(xRegister32(value_reg)));
                    armAsm->Strb(regX.W(), mop);
					break;

				case 16:
//					xMOV(ptr[(void*)ppf], xRegister16(value_reg));
                    armAsm->Strh(regX.W(), mop);
					break;

				case 32:
//					xMOV(ptr[(void*)ppf], xRegister32(value_reg));
                    armAsm->Str(regX.W(), mop);
					break;

				case 64:
//					xMOV(ptr64[(void*)ppf], xRegister64(value_reg));
                    armAsm->Str(regX, mop);
					break;

					jNO_DEFAULT
			}
		}
		else
		{
            auto regQ = a64::QRegister(value_reg);
			switch (bits)
			{
				case 32:
//					xMOVSS(ptr[(void*)ppf], xRegisterSSE(value_reg));
                    armAsm->Str(regQ.S(), mop);
					break;

				case 128:
//					xMOVAPS(ptr128[(void*)ppf], xRegisterSSE(value_reg));
                    armAsm->Str(regQ.Q(), mop);
					break;

					jNO_DEFAULT
			}
		}
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		int szidx = 0;
		switch (bits)
		{
			case 8:
				szidx = 0;
				break;
			case 16:
				szidx = 1;
				break;
			case 32:
				szidx = 2;
				break;
			case 64:
				szidx = 3;
				break;
			case 128:
				szidx = 4;
				break;
		}

        // @@WRITE64_CONST@@ JITcompile時probe: bits==64 consthandler経路verify
        // Removal condition: SPAD書き込み不具合のroot causeがverifyされた時点
        if (bits == 64 && !xmm)
        {
            static int s_w64c_cnt = 0;
            if (s_w64c_cnt < 10)
            {
                void* hdlr = vmv.assumeHandlerGetRaw(szidx, true);
                Console.WriteLn("@@WRITE64_CONST@@ #%d pc=%08x addr=%08x paddr=%08x hdlr=%p vr=%d",
                    s_w64c_cnt, pc, addr_const, paddr, hdlr, value_reg);
                s_w64c_cnt++;
            }
        }

		iFlushCall(FLUSH_FULLVTLB);

//		_freeX86reg(arg1regd);
        _freeX86reg(ECX);

//		xMOV(arg1regd, paddr);
        armAsm->Mov(ECX, paddr);

		if (bits == 128)
		{
			pxAssert(xmm);
//			const xRegisterSSE argreg(xRegisterSSE::GetArgRegister(1, 0));
            const a64::VRegister argreg(armQRegister(1));
//			_freeXMMreg(argreg.GetId());
            _freeXMMreg(argreg.GetCode());
//			xMOVAPS(argreg, xRegisterSSE(value_reg));
            armAsm->Mov(argreg, a64::QRegister(value_reg));
		}
		else if (xmm)
		{
			pxAssert(bits == 32);
//			_freeX86reg(arg2regd);
            _freeX86reg(EDX);
//			xMOVD(arg2regd, xRegisterSSE(value_reg));
            armAsm->Fmov(EDX,  a64::QRegister(value_reg).S());
		}
		else
		{
//			_freeX86reg(arg2regd);
            _freeX86reg(EDX);
//			xMOV(arg2reg, xRegister64(value_reg));
            armAsm->Mov(RDX,  HostX(value_reg));
		}

//		xFastCall(vmv.assumeHandlerGetRaw(szidx, true));
        armAsm->Mov(EAX, ECX);
        armAsm->Mov(RCX, RDX);
        armEmitCall(vmv.assumeHandlerGetRaw(szidx, true));
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
//							Extra Implementations

//   ecx - virtual address
//   Returns physical address in eax.
//   Clobbers edx
void vtlb_DynV2P()
{
//	xMOV(eax, ecx);
    armAsm->Mov(EAX, ECX);
//	xAND(ecx, VTLB_PAGE_MASK); // vaddr & VTLB_PAGE_MASK
    armAsm->And(ECX, ECX, VTLB_PAGE_MASK);

//	xSHR(eax, VTLB_PAGE_BITS);
    armAsm->Lsr(EAX, EAX, VTLB_PAGE_BITS);
//	xMOV(eax, ptr[xComplexAddress(rdx, vtlbdata.ppmap, rax * 4)]);
    armAsm->Ldr(RXVIXLSCRATCH, PTR_CPU(vtlbdata.ppmap));
    armAsm->Ldr(EAX, a64::MemOperand(RXVIXLSCRATCH, RAX, a64::LSL, 2));

//	xOR(eax, ecx);
    armAsm->Orr(EAX, EAX, ECX);
}

void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc, u32 guest_addr,
	u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register,
	u8 size_in_bits, bool is_signed, bool is_load, bool is_xmm)
{
	static constexpr u32 GPR_SIZE = 8;
	static constexpr u32 XMM_SIZE = 16;

	// on win32, we need to reserve an additional 32 bytes shadow space when calling out to C
#ifdef _WIN32
	static constexpr u32 SHADOW_SIZE = 32;
#else
	static constexpr u32 SHADOW_SIZE = 0;
#endif

	DevCon.WriteLn("Backpatching %s at %p[%u] (pc %08X vaddr %08X): Bitmask %08X %08X Addr %u Data %u Size %u Flags %02X %02X",
		is_load ? "load" : "store", (void*)code_address, code_size, guest_pc, guest_addr, gpr_bitmask, fpr_bitmask,
		address_register, data_register, size_in_bits, is_signed, is_load);

    std::bitset<iREGCNT_GPR> stack_gpr;
    std::bitset<iREGCNT_XMM> stack_xmm;

	u8* thunk = recBeginThunk();

	// save regs
	u32 i, stack_offset;
    u32 num_gprs = 0, num_fprs = 0;

	for (i = 0; i < iREGCNT_GPR; ++i)
	{
        if ((gpr_bitmask & (1u << i)) && armIsCallerSaved(i) && (!is_load || is_xmm || data_register != i)) {
            num_gprs++;
            stack_gpr.set(i);
        }
	}
	for (i = 0; i < iREGCNT_XMM; ++i)
	{
		if (fpr_bitmask & (1u << i) && armIsCallerSavedXmm(i) && (!is_load || !is_xmm || data_register != i)) {
            num_fprs++;
            stack_xmm.set(i);
        }
	}

	const u32 stack_size = (((num_gprs + 1) & ~1u) * GPR_SIZE) + (num_fprs * XMM_SIZE) + SHADOW_SIZE;
	if (stack_size > 0)
	{
//		xSUB(rsp, stack_size);
        armAsm->Sub(a64::sp, a64::sp, stack_size);

		stack_offset = SHADOW_SIZE;
        for (i = 0; i < iREGCNT_XMM; ++i)
        {
            if(stack_xmm[i])
            {
//				xMOVAPS(ptr128[rsp + stack_offset], xRegisterSSE(i));
                armAsm->Str(a64::DRegister(i), a64::MemOperand(a64::sp, stack_offset));
                stack_offset += XMM_SIZE;
            }
        }
        ////
        for (i = 0; i < iREGCNT_GPR; ++i)
        {
            if(stack_gpr[i])
            {
//				xMOV(ptr64[rsp + stack_offset], xRegister64(i));
                armAsm->Str(HostX(i), a64::MemOperand(a64::sp, stack_offset));
                stack_offset += GPR_SIZE;
            }
        }
	}

	if (is_load)
	{
		// [iter229+P15] FIX: DynGen_HandlerTest broken on ARM64. Use direct memReadN.
		// [P15] Extended to handle FPR (is_xmm) loads <= 64-bit too, since DynGen_HandlerTest
		// causes SIGSEGV at PC=0 when backpatching FPR loads (e.g. LWC1 at vaddr=0x53C).
		// Removal condition: なし（恒久fix）
		if (size_in_bits <= 64)
		{
			armAsm->Mov(EAX, a64::WRegister(address_register));
			switch (size_in_bits)
			{
				case  8: armEmitCall((void*)memRead8); break;
				case 16: armEmitCall((void*)memRead16); break;
				case 32: armEmitCall((void*)memRead32); break;
				case 64: armEmitCall((void*)memRead64); break;
			}
			if (is_xmm)
			{
				// FPR load: move result from GPR to FPR
				if (size_in_bits == 64)
					armAsm->Fmov(a64::DRegister(data_register), RAX);
				else
					armAsm->Fmov(a64::SRegister(data_register), EAX);
			}
			else if (data_register != EAX.GetCode())
			{
				if (size_in_bits == 64)
					armAsm->Mov(HostX(data_register), RAX);
				else
					armAsm->Mov(HostX(data_register), RAX);
			}
		}
		else
		{
			// [P15] 128-bit: call vtlb_memRead128 directly (DynGen_HandlerTest broken on ARM64).
			// vtlb_memRead128 returns r128 in v0 (AAPCS64: NEON return in q0).
			// Removal condition: なし（恒久fix）
			pxAssert(size_in_bits == 128);
			armAsm->Mov(EAX, a64::WRegister(address_register));
			armEmitCall((void*)vtlb_memRead128);
			// Result in q0 (xmm0)
			if (data_register != 0)
				armAsm->Mov(a64::QRegister(data_register), a64::QRegister(0));
		}
	}
	else
	{
		// [iter230+P15] FIX: DynGen_HandlerTest is broken on ARM64 for writes too.
		// Use direct memWriteN calls for size<=64 (including FPR stores).
		// Removal condition: なし（恒久fix）
		if (size_in_bits <= 64)
		{
			armAsm->Mov(a64::w0, a64::WRegister(address_register));
			if (is_xmm)
			{
				// FPR store: move data from FPR to GPR for memWrite call
				if (size_in_bits == 64)
					armAsm->Fmov(a64::x1, a64::DRegister(data_register));
				else
					armAsm->Fmov(a64::w1, a64::SRegister(data_register));
			}
			else
			{
				switch (size_in_bits)
				{
					case 8:
						armAsm->Mov(a64::w1, HostW(data_register));
						armAsm->And(a64::w1, a64::w1, 0xFF);
						break;
					case 16:
						armAsm->Mov(a64::w1, HostW(data_register));
						armAsm->And(a64::w1, a64::w1, 0xFFFF);
						break;
					case 32:
						armAsm->Mov(a64::w1, HostW(data_register));
						break;
					case 64:
						armAsm->Mov(a64::x1, HostX(data_register));
						break;
					jNO_DEFAULT
				}
			}
			switch (size_in_bits)
			{
				case  8: armEmitCall((void*)memWrite8); break;
				case 16: armEmitCall((void*)memWrite16); break;
				case 32: armEmitCall((void*)memWrite32); break;
				case 64: armEmitCall((void*)memWrite64); break;
				jNO_DEFAULT
			}
		}
		else
		{
			// [P15] 128-bit: call vtlb_memWrite128 directly (DynGen_HandlerTest broken on ARM64).
			// AAPCS64: w0=addr(u32), v0=data(r128/uint32x4_t).
			// Removal condition: なし（恒久fix）
			pxAssert(size_in_bits == 128);
			armAsm->Mov(a64::w0, a64::WRegister(address_register));
			if (data_register != 0)
				armAsm->Mov(a64::QRegister(0), a64::QRegister(data_register));
			armEmitCall((void*)vtlb_memWrite128);
		}
	}

	// restore regs
	if (stack_size > 0)
	{
		stack_offset = SHADOW_SIZE;
		for (i = 0; i < iREGCNT_XMM; ++i)
		{
            if(stack_xmm[i])
			{
//				xMOVAPS(xRegisterSSE(i), ptr128[rsp + stack_offset]);
                armAsm->Ldr(a64::DRegister(i), a64::MemOperand(a64::sp, stack_offset));
				stack_offset += XMM_SIZE;
			}
		}
        ////
		for (i = 0; i < iREGCNT_GPR; ++i)
		{
            if(stack_gpr[i])
			{
//				xMOV(xRegister64(i), ptr64[rsp + stack_offset]);
                armAsm->Ldr(HostX(i), a64::MemOperand(a64::sp, stack_offset));
				stack_offset += GPR_SIZE;
			}
		}

//		xADD(rsp, stack_size);
        armAsm->Add(a64::sp, a64::sp, stack_size);
	}

//	xJMP((void*)(code_address + code_size));
    armEmitJmp(reinterpret_cast<const void*>(code_address + code_size), true);

	recEndThunk();

	// backpatch to a jump to the slowmem handler
//	x86Ptr = (u8*)code_address;
//	xJMP(thunk);
    armEmitJmpPtr((void*)code_address, thunk, true);
}
