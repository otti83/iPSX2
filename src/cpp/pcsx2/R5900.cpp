// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "common/Error.h" // [P15] for ELF preload Error parameter
#include <sys/time.h> // [TEMP_DIAG] @@IOP_STUCK@@

#include "common/StringUtil.h"
#include "ps2/BiosTools.h"
#include "R5900.h"
#include "R3000A.h"
#include "x86/iR5900.h"
#include "ps2/pgif.h" // pgif init
#include "VUmicro.h"
#include "COP0.h"
#include "MTVU.h"
#include "VMManager.h"

#include "Hardware.h"
#include "IPU/IPUdma.h"
#include "IopMem.h"  // [TEMP_DIAG] for iopMem->Main direct access
#include <sys/mman.h>  // [TEMP_DIAG] for mprotect IOP guard

#include "Elfheader.h"
#include "CDVD/CDVD.h"
#include "Patch.h"
#include "GameDatabase.h"
#include "GSDumpReplayer.h"

#include "DebugTools/Breakpoints.h"
#include "DebugTools/MIPSAnalyst.h"
#include "DebugTools/SymbolGuardian.h"
#include "R5900OpcodeTables.h"
#include "common/Darwin/DarwinMisc.h"

#include "fmt/format.h"

using namespace R5900;	// for R5900 disasm tools

// [iv2 accel Phase 0 2026-06-07] EE event-test 診断 probe の compile-gate。
// default 0 = release で完全除去。対象は _cpuEventTest_Shared 内の **ungated** な per-call
// 診断のみ (EE_EVT_PC/GAP_PC/EE_PERIODIC/FNPTR_WATCHPOINT/IOP_EXCVEC_HFCHECK/dbg_mask/
// DMAC_INT_CHECK/INTC_INT_CHECK/IOP_STUCK timing wrapper)。これらは read-only 診断
// (Console 出力 + 静的 counter + memory 読込) で emulation 状態を変えない (logic-preserving)。
// 注: intcInterrupt() は INTC_STAT&2 時 counters[].hold を書くが、診断呼出 (dbg_mask/
// DMAC/INTC_CHECK) は機能呼出 mask_check (944) と同 cpuRegs.cycle で同値を書くため、除去後も
// 最終状態は mask_check 由来で不変 = bit-exact。@@IOP_STUCK@@ は ExecuteBlock(=機能) を
// timing で包むだけなので #else で ExecuteBlock を保持。退行調査時は -DIPSX2_EE_EVT_DIAG=1。
// 既存の `if (diag_enabled)` runtime-gated ブロック群 (env iPSX2_ENABLE_DIAG_FLAGS) は別系統で対象外。
#ifndef IPSX2_EE_EVT_DIAG
#define IPSX2_EE_EVT_DIAG 0
#endif

extern "C" void iPSX2_DumpLfCmpStoreRing();

s32 EEsCycle;		// used to sync the IOP to the EE
u32 EEoCycle;

alignas(64) cpuRegistersPack g_cpuRegistersPack;
alignas(16) tlbs tlb[48];
cachedTlbs_t cachedTlbs;

R5900cpu *Cpu = NULL;

static constexpr uint eeWaitCycles = 3072;

bool eeEventTestIsActive = false;
EE_intProcessStatus eeRunInterruptScan = INT_NOT_RUNNING;

u32 g_eeloadMain = 0, g_eeloadExec = 0, g_osdsys_str = 0;

// [P15] ELF ポストロード: EntryPointCompilingOnCPUThread() からcall。
// EELOAD after completedにゲーム ELF を再適用して欠落データを補完する。
// Removal condition: IOP→EE SIF DMA transferのデータ欠落root causeafter fixed
static std::string s_elf_preload_path;
extern void (*g_armsx2_elf_postload_fn)();

static void iPSX2_ELF_Postload_Impl()
{
	if (s_elf_preload_path.empty() || !eeMem) return;
	ElfObject elfo;
	Error error;
	if (!cdvdLoadElf(&elfo, s_elf_preload_path, false, &error)) {
		Console.WriteLn("@@ELF_POSTLOAD@@ FAILED: %s", error.GetDescription().c_str());
		return;
	}
	const u8* elfdata = elfo.GetData().data();
	const size_t elfsize = elfo.GetData().size();
	const ELF_HEADER* ehdr = reinterpret_cast<const ELF_HEADER*>(elfdata);
	u32 bytes_loaded = 0;
	if (ehdr->e_phoff + ehdr->e_phnum * ehdr->e_phentsize <= elfsize) {
		for (int i = 0; i < ehdr->e_phnum; i++) {
			const ELF_PHR* ph = reinterpret_cast<const ELF_PHR*>(
				elfdata + ehdr->e_phoff + i * ehdr->e_phentsize);
			if (ph->p_type != 1) continue;
			const u32 dst = ph->p_vaddr & 0x1FFFFFFFu;
			if (dst + ph->p_memsz > Ps2MemSize::MainRam) continue;
			if (ph->p_filesz > 0 && ph->p_offset + ph->p_filesz <= elfsize) {
				std::memcpy(eeMem->Main + dst, elfdata + ph->p_offset, ph->p_filesz);
				bytes_loaded += ph->p_filesz;
			}
			if (ph->p_memsz > ph->p_filesz)
				std::memset(eeMem->Main + dst + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
		}
	}
	// Check zero pages right after postload to determine if ELF file itself has zeros
	u32 zero_pages_after = 0;
	u32 first_zp = 0;
	for (u32 pg = 0x100000; pg < 0x54D000; pg += 0x1000) {
		bool all_zero = true;
		for (u32 off = 0; off < 0x1000; off += 4) {
			if (*(u32*)(eeMem->Main + pg + off) != 0) { all_zero = false; break; }
		}
		if (all_zero) { zero_pages_after++; if (!first_zp) first_zp = pg; }
	}
	Console.WriteLn("@@ELF_POSTLOAD@@ reloaded %u bytes, zero_pages_after=%u first=%08x from '%s'",
		bytes_loaded, zero_pages_after, first_zp, s_elf_preload_path.c_str());
	// Also check 0x26FDF0 specifically
	Console.WriteLn("@@ELF_POSTLOAD@@ mem[26FDF0]=%08x %08x %08x %08x [270000]=%08x %08x %08x %08x",
		*(u32*)(eeMem->Main + 0x26FDF0), *(u32*)(eeMem->Main + 0x26FDF4),
		*(u32*)(eeMem->Main + 0x26FDF8), *(u32*)(eeMem->Main + 0x26FDFC),
		*(u32*)(eeMem->Main + 0x270000), *(u32*)(eeMem->Main + 0x270004),
		*(u32*)(eeMem->Main + 0x270008), *(u32*)(eeMem->Main + 0x27000C));
	s_elf_preload_path.clear();
}

static s32 iPSX2_FindAsciiInRom(const u8* rom, size_t rom_size, const char* needle)
{
	if (!rom || !needle || !needle[0] || rom_size == 0)
		return -1;
	const size_t nlen = std::strlen(needle);
	if (nlen == 0 || rom_size < nlen)
		return -1;
	for (size_t i = 0; i + nlen <= rom_size; i++)
	{
		if (std::memcmp(rom + i, needle, nlen) == 0)
			return static_cast<s32>(i);
	}
	return -1;
}

static bool iPSX2_ReadRomBuf32(u32 guest_addr, u32* out)
{
	if (!eeMem || !eeMem->ROM || !out)
		return false;
	if (guest_addr < 0xBFC00000 || guest_addr >= 0xC0000000)
		return false;
	const u32 off = guest_addr - 0xBFC00000;
	if (off + 4 > Ps2MemSize::Rom)
		return false;
	*out = *reinterpret_cast<const u32*>(&eeMem->ROM[off]);
	return true;
}

/* I don't know how much space for args there is in the memory block used for args in full boot mode,
but in fast boot mode, the block we use can fit at least 16 argv pointers (varies with BIOS version).
The second EELOAD call during full boot has three built-in arguments ("EELOAD rom0:PS2LOGO <ELF>"),
meaning that only the first 13 game arguments supplied by the user can be added on and passed through.
In fast boot mode, 15 arguments can fit because the only call to EELOAD is "<ELF> <<args>>". */
const int kMaxArgs = 16;
uptr g_argPtrs[kMaxArgs];
#define DEBUG_LAUNCHARG 0 // show lots of helpful console messages as the launch arguments are passed to the game

void cpuReset()
{
    //// cpuRegistersPack -> VIFregisters
    g_cpuRegistersPack.vifRegs[0] = vif0Regs;
    g_cpuRegistersPack.vifRegs[1] = vif1Regs;
    ////
	std::memset(&cpuRegs, 0, sizeof(cpuRegs));
	std::memset(&fpuRegs, 0, sizeof(fpuRegs));
	std::memset(&tlb, 0, sizeof(tlb));
	cachedTlbs.count = 0;

	cpuRegs.pc				= 0xbfc00000; //set pc reg to stack
	cpuRegs.CP0.n.Config	= 0x440;
	cpuRegs.CP0.n.Status.val= 0x70400004; //0x10900000 <-- wrong; // COP0 enabled | BEV = 1 | TS = 1
	cpuRegs.CP0.n.PRid		= 0x00002e20; // PRevID = Revision ID, same as R5900
	fpuRegs.fprc[0]			= 0x00002e30; // fpu Revision..
	fpuRegs.fprc[31]		= 0x01000001; // fpu Status/Control

	cpuRegs.nextEventCycle = cpuRegs.cycle + 4;
	EEsCycle = 0;
	EEoCycle = cpuRegs.cycle;

	psxReset();
	pgifInit();

	extern void Deci2Reset();		// lazy, no good header for it yet.
	Deci2Reset();

	AllowParams1 = !VMManager::Internal::IsFastBootInProgress();
	AllowParams2 = !VMManager::Internal::IsFastBootInProgress();
	ParamsRead = false;

	g_eeloadMain = 0;
	g_eeloadExec = 0;
	g_osdsys_str = 0;

	CBreakPoints::ClearSkipFirst();

	// [iPSX2] P0 Audit
	static bool s_p0_audit_logged = false;
	if (!s_p0_audit_logged) {
		s_p0_audit_logged = true;
		Console.WriteLn("@@P0_AUDIT@@ enable_p0_behavior_patches=%d", iPSX2_ENABLE_P0_BEHAVIOR_PATCHES);
		if (iPSX2_ENABLE_P0_BEHAVIOR_PATCHES == 0) {
			Console.WriteLn("@@P0_AUDIT@@ patches_will_not_run");
		}
		// Detailed site status (derived from P0 master gate)
		Console.WriteLn("@@P0_AUDIT@@ sites={copybios_ext0:%d, copybios_nop:%d, memreset_rom_patch:%d, rec_tbin_patch:0, delay_slot_override:0}",
			iPSX2_ENABLE_P0_BEHAVIOR_PATCHES, iPSX2_ENABLE_P0_BEHAVIOR_PATCHES, iPSX2_ENABLE_P0_BEHAVIOR_PATCHES);
	}
}

// [iter202] cpuException がconfigした正しい EPC をsave (probe_mtc0_epc が使用)
u32 g_armsx2_last_real_epc = 0;

// Set Exception and branch to handler
void cpuException(u32 code, u32 bd)
{
    // [iter89] cap SYSCALL (code=0x20) exception flood to first 20; add $ra/$v1 for call-chain diagnosis
    // Removal condition: SYSCALL loop (0x82064/0x82080) root cause確定時
    {
        const bool is_syscall = (code == 0x20);
        static int s_syscall_n = 0;
#if DEBUG
        static int s_other_exc_n = 0;
        const bool log_it = is_syscall ? (s_syscall_n < 20) : (s_other_exc_n < 20);
        if (log_it) {
            if (is_syscall) s_syscall_n++; else s_other_exc_n++;
            Console.WriteLn("DEBUG: cpuException Triggered. Code=0x%x, BD=%d, PC=0x%08x, Status=0x%08x ra=%08x v1=%08x",
                code, bd, cpuRegs.pc, cpuRegs.CP0.n.Status.val,
                cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v1.UL[0]);
        }
#endif
        // [iter94] @@EXECPS2_SYSCALL@@ – ExecPS2(v1==7) 専用ログ (20-cap バイパス)
        // Removal condition: EELOAD→OSDSYS ExecPS2 パス（success/failcause）確定時
        if (is_syscall && cpuRegs.GPR.n.v1.UL[0] == 7) {
            static int s_exec_n = 0;
            if (s_exec_n++ < 5) {
                const char* path = nullptr;
                const u32 a0_ptr = cpuRegs.GPR.n.a0.UL[0];
                if (a0_ptr && a0_ptr < 0x02000000u)
                    path = (const char*)PSM(a0_ptr);
                Console.WriteLn("@@EXECPS2_SYSCALL@@ n=%d pc=%08x a0ptr=%08x path=%.64s",
                    s_exec_n, cpuRegs.pc, a0_ptr, path ? path : "(null)");
            }
        }
    }

    // [iter_EXCHDLR] @@EXC_HANDLER_DUMP@@ SYSCALL at EELOAD (0x82008): dump handler code 0x80000280-0x80000360
    // Removal condition: JIT exception handler failure root cause identified (800002bc→80000324 gap)
    if ((code & 0x7C) == 0x20 && cpuRegs.pc == 0x00082008u)
    {
        static bool s_exc_hdlr_done = false;
        if (!s_exc_hdlr_done && eeMem) {
            s_exc_hdlr_done = true;
            const bool jit = (Cpu != &intCpu);
            // Dump exception handler code at 0x80000280–0x80000370 (physical 0x280–0x370)
            u32 buf[36];
            for (int i = 0; i < 36; i++) buf[i] = *(u32*)(eeMem->Main + 0x280 + i*4);
            Console.WriteLn("@@EXC_HANDLER_DUMP@@ [%s] 80000280: %08x %08x %08x %08x %08x %08x %08x %08x",
                jit?"JIT":"Interp", buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
            Console.WriteLn("@@EXC_HANDLER_DUMP@@ [%s] 800002a0: %08x %08x %08x %08x %08x %08x %08x %08x",
                jit?"JIT":"Interp", buf[8],buf[9],buf[10],buf[11],buf[12],buf[13],buf[14],buf[15]);
            Console.WriteLn("@@EXC_HANDLER_DUMP@@ [%s] 800002c0: %08x %08x %08x %08x %08x %08x %08x %08x",
                jit?"JIT":"Interp", buf[16],buf[17],buf[18],buf[19],buf[20],buf[21],buf[22],buf[23]);
            Console.WriteLn("@@EXC_HANDLER_DUMP@@ [%s] 800002e0: %08x %08x %08x %08x %08x %08x %08x %08x",
                jit?"JIT":"Interp", buf[24],buf[25],buf[26],buf[27],buf[28],buf[29],buf[30],buf[31]);
            Console.WriteLn("@@EXC_HANDLER_DUMP@@ [%s] 80000300: %08x %08x %08x %08x",
                jit?"JIT":"Interp", buf[32],buf[33],buf[34],buf[35]);
            // Also dump 0x80000310-0x80000370
            u32 buf2[24];
            for (int i = 0; i < 24; i++) buf2[i] = *(u32*)(eeMem->Main + 0x310 + i*4);
            Console.WriteLn("@@EXC_HANDLER_DUMP@@ [%s] 80000310: %08x %08x %08x %08x %08x %08x %08x %08x",
                jit?"JIT":"Interp", buf2[0],buf2[1],buf2[2],buf2[3],buf2[4],buf2[5],buf2[6],buf2[7]);
            Console.WriteLn("@@EXC_HANDLER_DUMP@@ [%s] 80000330: %08x %08x %08x %08x %08x %08x %08x %08x",
                jit?"JIT":"Interp", buf2[8],buf2[9],buf2[10],buf2[11],buf2[12],buf2[13],buf2[14],buf2[15]);
            Console.WriteLn("@@EXC_HANDLER_DUMP@@ [%s] 80000350: %08x %08x %08x %08x %08x %08x %08x %08x",
                jit?"JIT":"Interp", buf2[16],buf2[17],buf2[18],buf2[19],buf2[20],buf2[21],buf2[22],buf2[23]);
            // Also capture k0/k1 registers at exception entry
            Console.WriteLn("@@EXC_HANDLER_DUMP@@ [%s] k0=%08x k1=%08x sp=%08x ra=%08x v0=%08x v1=%08x",
                jit?"JIT":"Interp",
                cpuRegs.GPR.n.k0.UL[0], cpuRegs.GPR.n.k1.UL[0],
                cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.ra.UL[0],
                cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0]);
            // [iter_PHYS82] Physical EE RAM at 0x82000-0x8206F + VTLB-mapped 0x82000-0x8200c
            // 目的: JIT SYSCALL at 0x82008 時点での物理memory内容と TLB mapping先の比較
            // Removal condition: JIT SYSCALL at 0x82008 の root cause after identified
            u32 p82[28];
            for (int i = 0; i < 28; i++) p82[i] = *(u32*)(eeMem->Main + 0x82000 + i*4);
            Console.WriteLn("@@EXC_HDLR_PHYS82@@ [%s] 82000: %08x %08x %08x %08x %08x %08x %08x %08x",
                jit?"JIT":"Interp", p82[0],p82[1],p82[2],p82[3],p82[4],p82[5],p82[6],p82[7]);
            Console.WriteLn("@@EXC_HDLR_PHYS82@@ [%s] 82020: %08x %08x %08x %08x %08x %08x %08x %08x",
                jit?"JIT":"Interp", p82[8],p82[9],p82[10],p82[11],p82[12],p82[13],p82[14],p82[15]);
            Console.WriteLn("@@EXC_HDLR_PHYS82@@ [%s] 82040: %08x %08x %08x %08x %08x %08x %08x %08x",
                jit?"JIT":"Interp", p82[16],p82[17],p82[18],p82[19],p82[20],p82[21],p82[22],p82[23]);
            Console.WriteLn("@@EXC_HDLR_PHYS82@@ [%s] 82060: %08x %08x %08x %08x",
                jit?"JIT":"Interp", p82[24],p82[25],p82[26],p82[27]);
            Console.WriteLn("@@EXC_HDLR_TLB82@@ [%s] vtlb[82000]=%08x [82004]=%08x [82008]=%08x [8200c]=%08x",
                jit?"JIT":"Interp",
                memRead32(0x82000u), memRead32(0x82004u), memRead32(0x82008u), memRead32(0x8200cu));
        }
    }

    // [iter345] @@OSDSYS_SYSCALL@@ OSDSYS (0x100000-0x200000) SYSCALL を捕捉。
    // OSDSYS が呼ぶ kernel SYSCALL number (v1) を特定し、dispatch table にneededなエントリを判明させる。
    // Removal condition: OSDSYS→kernel dispatch 経路が正常化した後
    if ((code & 0x7C) == 0x20) // ExcCode=8 (SYSCALL)
    {
        const u32 sc_pc = cpuRegs.pc;
        if (sc_pc >= 0x00100000u && sc_pc < 0x00200000u) {
            static int s_osdsys_sc_n = 0;
            if (s_osdsys_sc_n < 20) {
                const s32 v1_raw = cpuRegs.GPR.n.v1.SL[0];
                const u8 call_num = (v1_raw < 0) ? (u8)(-v1_raw) : (u8)v1_raw;
                Console.WriteLn("@@OSDSYS_SYSCALL@@ n=%d epc=%08x v1=%d call=0x%02x a0=%08x a1=%08x a2=%08x ra=%08x",
                    s_osdsys_sc_n++, sc_pc, v1_raw, (u32)call_num,
                    cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
                    cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.ra.UL[0]);
            }
        }
    }

	bool errLevel2, checkStatus;
	u32 offset = 0;

    cpuRegs.branch = 0;		// Tells the interpreter that an exception occurred during a branch.
	cpuRegs.CP0.n.Cause = code & 0xffff;

    // [P11] @@EXCENTRY@@ — 例外突入時の Status キャプチャ (SYSCALL, JIT only, cap=20)
    // 目的: どの SYSCALL の前後で Status が 0x70010400→0x30010000 に変化するか確定
    // Removal condition: JIT Status corruptcauseafter identified
    {
        static const bool s_ckpt_en = iPSX2_GetRuntimeEnvBool("iPSX2_CHECKPOINT_LOG", false);
        if (s_ckpt_en && Cpu != &intCpu && (code & 0x7C) == 0x20) // SYSCALL only
        {
            static int s_n = 0;
            if (s_n < 20) {
                Console.WriteLn("@@EXCENTRY@@ n=%d pc=%08x cause=%02x status=%08x",
                    s_n++, cpuRegs.pc, code & 0x7C, cpuRegs.CP0.n.Status.val);
            }
        }
    }



	// =========================================================================
	// [iter223] @@FORCE_BEV0@@ BEV=1 で SYSCALL/例外がoccurした場合、BEV=0 にforcechange。
	// PS2 BIOS ROM の BEV=1 handlerテーブル (BFC00740) は全 ExcCode エントリが
	// NOP 無限loop (エラーhandler) を指している。KERNEL が RAM に例外handlerを
	// 設置して BEV=0 にするはずだが、JIT バグで KERNEL init が未完了のため BEV=1 のまま。
	// RAM 側の例外ベクタ (0x80000180 等) に KERNEL が部分的にでもhandlerを書き込んで
	// いれば、BEV=0 に切り替えることで正常なdispatchが可能になる。
	// Removal condition: JIT の KERNEL init バグafter fixed (KERNEL 自身が BEV=0 をconfigするようになった時)
	// [P9 TEMP_DIAG] interpretermodeでは BIOS が BEV=1 handlerで TLB をconfigするためdisabled化
	// =========================================================================
	// [iter652] BEV forcechangeハック撤去。BIOS は BEV=1 でbootし、
	// カーネル初期化after completedに自力で BEV=0 にconfigする。forcechangeは自然bootを阻害する。
	// 旧: if (BEV==1 && JIT) → BEV=0 force

	if(cpuRegs.CP0.n.Status.b.ERL == 0)
	{
		//Error Level 0-1
		errLevel2 = false;
		checkStatus = (cpuRegs.CP0.n.Status.b.BEV == 0); //  for TLB/general exceptions

		if (((code & 0x7C) >= 0x8) && ((code & 0x7C) <= 0xC))
			offset = 0x0; //TLB Refill
		else if ((code & 0x7C) == 0x0)
			offset = 0x200; //Interrupt
		else
			offset = 0x180; // Everything else
	}
	else
	{
		//Error Level 2
		errLevel2 = true;
		checkStatus = (cpuRegs.CP0.n.Status.b.DEV == 0); // for perf/debug exceptions

		Console.Error("*PCSX2* FIX ME: Level 2 cpuException");
		if ((code & 0x38000) <= 0x8000 )
		{
			//Reset / NMI
			cpuRegs.pc = 0xBFC00000;
			Console.Warning("Reset request");
			cpuUpdateOperationMode();
			return;
		}
		else if ((code & 0x38000) == 0x10000)
			offset = 0x80; //Performance Counter
		else if ((code & 0x38000) == 0x18000)
			offset = 0x100; //Debug
		else
			Console.Error("Unknown Level 2 Exception!! Cause %x", code);
	}

	if (cpuRegs.CP0.n.Status.b.EXL == 0)
	{
		cpuRegs.CP0.n.Status.b.EXL = 1;
		if (bd)
		{
			Console.Warning("branch delay!!");
			cpuRegs.CP0.n.EPC = cpuRegs.pc - 4;
			cpuRegs.CP0.n.Cause |= 0x80000000;
		}
		else
		{
			cpuRegs.CP0.n.EPC = cpuRegs.pc;
			cpuRegs.CP0.n.Cause &= ~0x80000000;
		}
		// [iter202] JIT flush バグ回避: ERET stub の MFC0→ADDIU→MTC0 チェーンで
		// k1 registerが ARM64 物理registerから cpuRegs.GPR.r[27] に flush されない。
		// MTC0 が stale な古い EPC を書き込むため、正しい EPC をここでsave。
		// probe_mtc0_epc がこの値を使って EPC を正しい次命令に補正する。
		// Removal condition: JIT k1 flush バグafter fixed
		extern u32 g_armsx2_last_real_epc;
		g_armsx2_last_real_epc = cpuRegs.CP0.n.EPC;
	}
	else
	{
		offset = 0x180; //Override the cause
		if (errLevel2) Console.Warning("cpuException: Status.EXL = 1 cause %x", code);
	}

	if (checkStatus)
		cpuRegs.pc = 0x80000000 + offset;
	else
		cpuRegs.pc = 0xBFC00200 + offset;

    // [iter653] @@SYSCALL_TRACE@@ — SYSCALL 例外時の v1 (syscallnumber) を JIT/Interpreter 両方で記録。
    // Removal condition: JIT 0x80001578 loopcauseの syscall numberafter identified
    if ((code & 0x7C) == 0x20) // ExcCode=8 (SYSCALL)
    {
        static int s_sc_n = 0;
        if (s_sc_n < 50) {
            const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
            const u32 v1_val = cpuRegs.GPR.n.v1.UL[0];
            const u32 sc_num = (v1_val >> 2) & 0xFFu;
            Console.WriteLn("@@SYSCALL_TRACE@@ [%s] n=%d epc=%08x newpc=%08x v1=%08x sc#=%u a0=%08x a1=%08x status=%08x cyc=%u ra=%08x",
                mode, s_sc_n, cpuRegs.CP0.n.EPC, cpuRegs.pc, v1_val, sc_num,
                cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
                cpuRegs.CP0.n.Status.val, cpuRegs.cycle,
                cpuRegs.GPR.n.ra.UL[0]);
            s_sc_n++;
        }
    }

    // [iter666] @@INT_EXCVEC_DUMP@@ — 割り込み例外 (code=0x800) occur時に割り込みhandler全体ダンプ
    // Removal condition: DMAC dispatch chain JIT bug after determined
    if ((code & 0x7C) == 0x0 && code != 0) // Interrupt exception
    {
        static int s_int_dump_n = 0;
        if (s_int_dump_n == 0) {
            const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
            // Dump full interrupt handler at 0x80000200 (32 words = 128 bytes)
            Console.WriteLn("@@INT_HANDLER@@ [%s] code=%x epc=%08x "
                "[200]=%08x %08x %08x %08x %08x %08x %08x %08x",
                mode, code, cpuRegs.CP0.n.EPC,
                memRead32(0x80000200u), memRead32(0x80000204u), memRead32(0x80000208u),
                memRead32(0x8000020Cu), memRead32(0x80000210u), memRead32(0x80000214u),
                memRead32(0x80000218u), memRead32(0x8000021Cu));
            Console.WriteLn("@@INT_HANDLER@@ [%s] [220]=%08x %08x %08x %08x %08x %08x %08x %08x",
                mode,
                memRead32(0x80000220u), memRead32(0x80000224u), memRead32(0x80000228u),
                memRead32(0x8000022Cu), memRead32(0x80000230u), memRead32(0x80000234u),
                memRead32(0x80000238u), memRead32(0x8000023Cu));
            Console.WriteLn("@@INT_HANDLER@@ [%s] [240]=%08x %08x %08x %08x %08x %08x %08x %08x",
                mode,
                memRead32(0x80000240u), memRead32(0x80000244u), memRead32(0x80000248u),
                memRead32(0x8000024Cu), memRead32(0x80000250u), memRead32(0x80000254u),
                memRead32(0x80000258u), memRead32(0x8000025Cu));
            Console.WriteLn("@@INT_HANDLER@@ [%s] [260]=%08x %08x %08x %08x %08x %08x %08x %08x",
                mode,
                memRead32(0x80000260u), memRead32(0x80000264u), memRead32(0x80000268u),
                memRead32(0x8000026Cu), memRead32(0x80000270u), memRead32(0x80000274u),
                memRead32(0x80000278u), memRead32(0x8000027Cu));
            // Also dump DMAC handler dispatch table and the registered handler pointer
            // Kernel DMAC handlers are at a table in EE RAM.
            // Check what the handler reads from Cause and where it dispatches.
            Console.WriteLn("@@INT_HANDLER@@ [%s] [280]=%08x %08x %08x %08x %08x %08x %08x %08x",
                mode,
                memRead32(0x80000280u), memRead32(0x80000284u), memRead32(0x80000288u),
                memRead32(0x8000028Cu), memRead32(0x80000290u), memRead32(0x80000294u),
                memRead32(0x80000298u), memRead32(0x8000029Cu));
            Console.WriteLn("@@INT_HANDLER@@ [%s] [2a0]=%08x %08x %08x %08x %08x %08x %08x %08x",
                mode,
                memRead32(0x800002A0u), memRead32(0x800002A4u), memRead32(0x800002A8u),
                memRead32(0x800002ACu), memRead32(0x800002B0u), memRead32(0x800002B4u),
                memRead32(0x800002B8u), memRead32(0x800002BCu));
            // Dump the interrupt handler dispatch table entries
            // Table at 0x80015380 + index*4, index=0..7 for IP[0..7]
            Console.WriteLn("@@INT_DISPATCH_TABLE@@ [%s] [15380]=%08x %08x %08x %08x %08x %08x %08x %08x "
                "[153a0]=%08x %08x %08x %08x",
                mode,
                memRead32(0x80015380u), memRead32(0x80015384u), memRead32(0x80015388u),
                memRead32(0x8001538Cu), memRead32(0x80015390u), memRead32(0x80015394u),
                memRead32(0x80015398u), memRead32(0x8001539Cu),
                memRead32(0x800153A0u), memRead32(0x800153A4u), memRead32(0x800153A8u),
                memRead32(0x800153ACu));
            // [iter666] DMAC handler at dispatch_table[3] = 0x8001538C (30-PLZCW(8)=3)
            u32 dmac_handler = memRead32(0x8001538Cu);
            if (dmac_handler >= 0x80000000u && dmac_handler < 0x82000000u) {
                Console.WriteLn("@@DMAC_HANDLER@@ [%s] addr=%08x", mode, dmac_handler);
                for (int row = 0; row < 8; row++) {
                    u32 base = dmac_handler + row * 32;
                    Console.WriteLn("@@DMAC_HANDLER@@ [%s] [%08x]: %08x %08x %08x %08x %08x %08x %08x %08x",
                        mode, base,
                        memRead32(base+0), memRead32(base+4), memRead32(base+8), memRead32(base+12),
                        memRead32(base+16), memRead32(base+20), memRead32(base+24), memRead32(base+28));
                }
                // Also dump the per-channel handler table that the DMAC handler uses
                // BIOS typically has a handler table at some known address
                // Dump 0x80015400-0x80015480 as likely DMAC per-channel table
                Console.WriteLn("@@DMAC_CHTABLE@@ [%s] [15400]=%08x %08x %08x %08x %08x %08x %08x %08x",
                    mode,
                    memRead32(0x80015400u), memRead32(0x80015404u), memRead32(0x80015408u),
                    memRead32(0x8001540Cu), memRead32(0x80015410u), memRead32(0x80015414u),
                    memRead32(0x80015418u), memRead32(0x8001541Cu));
                Console.WriteLn("@@DMAC_CHTABLE@@ [%s] [15420]=%08x %08x %08x %08x %08x %08x %08x %08x",
                    mode,
                    memRead32(0x80015420u), memRead32(0x80015424u), memRead32(0x80015428u),
                    memRead32(0x8001542Cu), memRead32(0x80015430u), memRead32(0x80015434u),
                    memRead32(0x80015438u), memRead32(0x8001543Cu));
            }
        }
        s_int_dump_n++;
    }

    // [iter653] @@EE_INTC_TRACE@@ — EE 割り込み例外トレース (cycle>24M=vsync6以降)。
    // カーネルがアイドルから抜ける割り込みの種類を特定。JIT で EELOAD ロードされないcause追跡。
    // Removal condition: EELOAD ロードissue解決後
    if ((code & 0x7C) == 0x0) // ExcCode=0 (Interrupt)
    {
        if (cpuRegs.cycle > 24000000u) {
            static int s_intc_n = 0;
            if (s_intc_n < 30) {
                const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
                Console.WriteLn("@@EE_INTC_TRACE@@ [%s] n=%d epc=%08x newpc=%08x status=%08x cause=%08x INTC_STAT=%08x INTC_MASK=%08x cyc=%u phys82000=%08x",
                    mode, s_intc_n,
                    cpuRegs.CP0.n.EPC, cpuRegs.pc,
                    cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause,
                    psHu32(INTC_STAT), psHu32(INTC_MASK), cpuRegs.cycle,
                    eeMem ? *(u32*)(eeMem->Main + 0x82000) : 0xDEADu);
                s_intc_n++;
            }
        }
    }

	cpuUpdateOperationMode();
}

void cpuTlbMiss(u32 addr, u32 bd, u32 excode)
{
	// [P9 TEMP_DIAG] interpretermodeでは TLB miss が連鎖して 100MB+ ログになるためキャップ (cap=50)
	static int s_tlbmiss_cap = 0;
	if (s_tlbmiss_cap < 50) {
		s_tlbmiss_cap++;
		Console.Error("cpuTlbMiss pc:%x, cycl:%x, addr: %x, status=%x, code=%x [%d/50]",
				cpuRegs.pc, cpuRegs.cycle, addr, cpuRegs.CP0.n.Status.val, excode, s_tlbmiss_cap);
	}

	cpuRegs.CP0.n.BadVAddr = addr;
	cpuRegs.CP0.n.Context &= 0xFF80000F;
	cpuRegs.CP0.n.Context |= (addr >> 9) & 0x007FFFF0;
	cpuRegs.CP0.n.EntryHi = (addr & 0xFFFFE000) | (cpuRegs.CP0.n.EntryHi & 0x1FFF);

	cpuRegs.pc -= 4;
	cpuException(excode, bd);
}

void cpuTlbMissR(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE_TLBL);
}

void cpuTlbMissW(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE_TLBS);
}

// sets a branch test to occur some time from an arbitrary starting point.
__fi void cpuSetNextEvent( u32 startCycle, s32 delta )
{
	// SAFETY: Ensure we don't schedule an event for "right now" or in the past,
	// which causes the JIT to infinite loop calling checks.
	// if (delta < 32) delta = 32; // Android alignment: Removed hack

	// typecast the conditional to signed so that things don't blow up
	// if startCycle is greater than our next branch cycle.

	if( (int)(cpuRegs.nextEventCycle - startCycle) > delta )
	{
		cpuRegs.nextEventCycle = startCycle + delta;
	}
}

// sets a branch to occur some time from the current cycle
__fi void cpuSetNextEventDelta( s32 delta )
{
	cpuSetNextEvent( cpuRegs.cycle, delta );
}

__fi int cpuGetCycles(int interrupt)
{
	if(interrupt == VU_MTVU_BUSY && (!THREAD_VU1 || INSTANT_VU1))
		return 1;
	else
	{
		const int cycles = (cpuRegs.sCycle[interrupt] + cpuRegs.eCycle[interrupt]) - cpuRegs.cycle;
		return std::max(1, cycles);
	}

}

// tests the cpu cycle against the given start and delta values.
// Returns true if the delta time has passed.
__fi int cpuTestCycle( u32 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't explode
	// if the startCycle is ahead of our current cpu cycle.

	return (int)(cpuRegs.cycle - startCycle) >= delta;
}

// tells the EE to run the branch test the next time it gets a chance.
__fi void cpuSetEvent()
{
	cpuRegs.nextEventCycle = cpuRegs.cycle;
}

__fi void cpuClearInt( uint i )
{
	pxAssume( i < 32 );
	cpuRegs.interrupt &= ~(1 << i);
	cpuRegs.dmastall &= ~(1 << i);
}

static __fi void TESTINT( u8 n, void (*callback)() )
{
	if( !(cpuRegs.interrupt & (1 << n)) ) return;

	if(CHECK_INSTANTDMAHACK || cpuTestCycle( cpuRegs.sCycle[n], cpuRegs.eCycle[n] ) )
	{
		cpuClearInt( n );
		callback();
	}
	else
		cpuSetNextEvent( cpuRegs.sCycle[n], cpuRegs.eCycle[n] );
}

// [TODO] move this function to Dmac.cpp, and remove most of the DMAC-related headers from
// being included into R5900.cpp.
static __fi bool _cpuTestInterrupts()
{

	if (!dmacRegs.ctrl.DMAE || (psHu8(DMAC_ENABLER+2) & 1))
	{
		//Console.Write("DMAC Disabled or suspended");
		return false;
	}

	eeRunInterruptScan = INT_RUNNING;

	while (eeRunInterruptScan == INT_RUNNING)
	{
		/* These are 'pcsx2 interrupts', they handle asynchronous stuff
		   that depends on the cycle timings */
		TESTINT(VU_MTVU_BUSY, MTVUInterrupt);
		TESTINT(DMAC_VIF1, vif1Interrupt);
		TESTINT(DMAC_GIF, gifInterrupt);
		TESTINT(DMAC_SIF0, EEsif0Interrupt);
		TESTINT(DMAC_SIF1, EEsif1Interrupt);
		// Profile-guided Optimization (sorta)
		// The following ints are rarely called.  Encasing them in a conditional
		// as follows helps speed up most games.

		if (cpuRegs.interrupt & ((1 << DMAC_VIF0) | (1 << DMAC_FROM_IPU) | (1 << DMAC_TO_IPU)
			| (1 << DMAC_FROM_SPR) | (1 << DMAC_TO_SPR) | (1 << DMAC_MFIFO_VIF) | (1 << DMAC_MFIFO_GIF)
			| (1 << VIF_VU0_FINISH) | (1 << VIF_VU1_FINISH) | (1 << IPU_PROCESS)))
		{
			TESTINT(DMAC_VIF0, vif0Interrupt);

			TESTINT(DMAC_FROM_IPU, ipu0Interrupt);
			TESTINT(DMAC_TO_IPU, ipu1Interrupt);
			TESTINT(IPU_PROCESS, ipuCMDProcess);

			TESTINT(DMAC_FROM_SPR, SPRFROMinterrupt);
			TESTINT(DMAC_TO_SPR, SPRTOinterrupt);

			TESTINT(DMAC_MFIFO_VIF, vifMFIFOInterrupt);
			TESTINT(DMAC_MFIFO_GIF, gifMFIFOInterrupt);

			TESTINT(VIF_VU0_FINISH, vif0VUFinish);
			TESTINT(VIF_VU1_FINISH, vif1VUFinish);
		}

		if (eeRunInterruptScan == INT_REQ_LOOP)
			eeRunInterruptScan = INT_RUNNING;
		else
			break;
	}

	eeRunInterruptScan = INT_NOT_RUNNING;

	if ((cpuRegs.interrupt & 0x1FFFF) & ~cpuRegs.dmastall)
		return true;
	else
		return false;
}

static __fi void _cpuTestTIMR()
{
	cpuRegs.CP0.n.Count += cpuRegs.cycle - cpuRegs.lastCOP0Cycle;
	cpuRegs.lastCOP0Cycle = cpuRegs.cycle;

	// fixme: this looks like a hack to make up for the fact that the TIMR
	// doesn't yet have a proper mechanism for setting itself up on a nextEventCycle.
	// A proper fix would schedule the TIMR to trigger at a specific cycle anytime
	// the Count or Compare registers are modified.

	if ( (cpuRegs.CP0.n.Status.val & 0x8000) &&
		cpuRegs.CP0.n.Count >= cpuRegs.CP0.n.Compare && cpuRegs.CP0.n.Count < cpuRegs.CP0.n.Compare+1000 )
	{
		Console.WriteLn( Color_Magenta, "timr intr: %x, %x", cpuRegs.CP0.n.Count, cpuRegs.CP0.n.Compare);
		cpuException(0x808000, cpuRegs.branch);
	}
}

static __fi void _cpuTestPERF()
{
	// Perfs are updated when read by games (COP0's MFC0/MTC0 instructions), so we need
	// only update them at semi-regular intervals to keep cpuRegs.cycle from wrapping
	// around twice on us btween updates.  Hence this function is called from the cpu's
	// Counters update.

	COP0_UpdatePCCR();
}

// Checks the COP0.Status for exception enablings.
// Exception handling for certain modes is *not* currently supported, this function filters
// them out.  Exceptions while the exception handler is active (EIE), or exceptions of any
// level other than 0 are ignored here.

static bool cpuIntsEnabled(int Interrupt)
{
	bool IntType = !!(cpuRegs.CP0.n.Status.val & Interrupt); //Choose either INTC or DMAC, depending on what called it

	return IntType && cpuRegs.CP0.n.Status.b.EIE && cpuRegs.CP0.n.Status.b.IE &&
		!cpuRegs.CP0.n.Status.b.EXL && (cpuRegs.CP0.n.Status.b.ERL == 0);
}

// Shared portion of the branch test, called from both the Interpreter
// and the recompiler.  (moved here to help alleviate redundant code)
__fi void _cpuEventTest_Shared()
{
#if IPSX2_EE_EVT_DIAG
	// [iter218] TEMP_DIAG: periodic EE PC sample to find hang location
	{
		static u32 s_evt_cnt = 0;
		++s_evt_cnt;
		if ((s_evt_cnt % 10) == 0 && s_evt_cnt <= 1000) {
			Console.WriteLn("@@EE_EVT_PC@@ n=%u pc=%08x cycle=%u", s_evt_cnt, cpuRegs.pc, cpuRegs.cycle);
		}
	}
	// [TEMP_DIAG] @@GAP_PC_SAMPLE@@ — VIF1 gap 期間中の PC サンプリング
	// Removal condition: ギャップcauseafter identified
	{
		extern u32 g_gap_sample_active, g_gap_sample_start_cyc;
		// Activate sampling when time since last VIF1 DMA exceeds 200K cycles
		u32 gap_since_dma = cpuRegs.cycle - g_gap_sample_start_cyc;
		if (gap_since_dma > 200000 && g_gap_sample_start_cyc != 0) {
			static u32 s_gap_samples = 0;
			if (s_gap_samples < 200) {
				Console.WriteLn("@@GAP_PC@@ n=%u pc=%08x cyc=%u gap_cyc=%u",
					s_gap_samples, cpuRegs.pc, cpuRegs.cycle, gap_since_dma);
				s_gap_samples++;
			}
		}
	}
	// [iter337] TEMP_DIAG: cycle-based EE PC sample every ~1s EE-time to find post-vsync=60 hang
	{
		static u32 s_cyc_n = 0;
		static u32 s_last_cyc = 0;
		if (cpuRegs.cycle - s_last_cyc >= 294912000u) { // ~1s at 294MHz
			s_last_cyc = cpuRegs.cycle;
			if (s_cyc_n++ < 60)
				Console.WriteLn("@@EE_PERIODIC@@ n=%u pc=%08x cycle=%u", s_cyc_n, cpuRegs.pc, cpuRegs.cycle);
		}
	}
	// [iter657] @@FNPTR_WATCHPOINT@@ — 0x991F0 の変化をdetect
	// vsync=3 でゼロになった後、JIT でのみ "rom0:OSDSYS" が復活する
	// この復活タイミングの EE PC を特定する
	// Removal condition: 0x991F0 差異のcauseafter identified
	if (eeMem) {
		static u32 s_fnptr_prev = 0;
		static u32 s_prev_pc = 0;
		static u32 s_prev_cycle = 0;
		static bool s_fnptr_done = false;
		if (!s_fnptr_done) {
			const u32 cur = *(u32*)(eeMem->Main + 0x991F0);
			if (cur != s_fnptr_prev) {
				const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
				Console.WriteLn("@@FNPTR_WATCHPOINT@@ [%s] 0x991F0 changed: %08x → %08x  pc=%08x cycle=%u ra=%08x prev_pc=%08x prev_cycle=%u",
					mode, s_fnptr_prev, cur, cpuRegs.pc, cpuRegs.cycle,
					cpuRegs.GPR.n.ra.UL[0], s_prev_pc, s_prev_cycle);
				Console.WriteLn("@@FNPTR_DMA_STATE@@ D5_CHCR=%08x D5_MADR=%08x D5_QWC=%08x D5_TADR=%08x D_STAT=%08x D_CTRL=%08x F220=%08x F230=%08x",
					psHu32(0xC400), psHu32(0xC410), psHu32(0xC430), psHu32(0xC420),
					psHu32(0xE010), psHu32(0xE000), psHu32(0xF220), psHu32(0xF230));
				s_fnptr_prev = cur;
				static u32 s_change_cnt = 0;
				if (++s_change_cnt >= 4) s_fnptr_done = true;
			}
			s_prev_pc = cpuRegs.pc;
			s_prev_cycle = cpuRegs.cycle;
		}
	}

	// [TEMP_DIAG] @@IOP_EXCVEC_HFCHECK@@ — high-frequency IOP excvec corruption detection
	// Removal condition: IOP excvec 破損causeafter identified
	{
		static bool s_hf_done = false;
		if (!s_hf_done && iopMem) {
			// Direct host memory access — bypasses all APIs
			const u32 excvec80 = *(const u32*)(iopMem->Main + 0x80);
			// Expected: ac010400 (SW at, 0x400(zero))
			// Or 00000000 (before init)
			if (excvec80 != 0xac010400 && excvec80 != 0x00000000) {
				s_hf_done = true;
				Console.Error("@@IOP_EXCVEC_HFCHECK@@ CORRUPTION DETECTED! excvec80=%08x ee_pc=%08x ee_cyc=%u iop_pc=%08x iop_cyc=%u ee_ra=%08x",
					excvec80, cpuRegs.pc, cpuRegs.cycle, psxRegs.pc, psxRegs.cycle,
					cpuRegs.GPR.n.ra.UL[0]);
				// Dump EE GPR context for forensics
				Console.Error("@@IOP_EXCVEC_HFCHECK@@ ee_v0=%08x ee_v1=%08x ee_a0=%08x ee_a1=%08x ee_a2=%08x ee_a3=%08x ee_t0=%08x ee_t1=%08x",
					cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
					cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.a3.UL[0],
					cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0]);
				Console.Error("@@IOP_EXCVEC_HFCHECK@@ ee_s0=%08x ee_s1=%08x ee_s2=%08x ee_s3=%08x ee_s4=%08x ee_s5=%08x ee_s6=%08x ee_s7=%08x",
					cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0],
					cpuRegs.GPR.n.s2.UL[0], cpuRegs.GPR.n.s3.UL[0],
					cpuRegs.GPR.n.s4.UL[0], cpuRegs.GPR.n.s5.UL[0],
					cpuRegs.GPR.n.s6.UL[0], cpuRegs.GPR.n.s7.UL[0]);
				// Dump nearby IOP memory
				Console.Error("@@IOP_EXCVEC_HFCHECK@@ iop[80]=%08x %08x %08x %08x iop[90]=%08x %08x %08x %08x",
					*(const u32*)(iopMem->Main + 0x80), *(const u32*)(iopMem->Main + 0x84),
					*(const u32*)(iopMem->Main + 0x88), *(const u32*)(iopMem->Main + 0x8c),
					*(const u32*)(iopMem->Main + 0x90), *(const u32*)(iopMem->Main + 0x94),
					*(const u32*)(iopMem->Main + 0x98), *(const u32*)(iopMem->Main + 0x9c));
				// Dump EE instructions around ee_pc and ee_ra for forensics
				{
					const u32 pc = cpuRegs.pc;
					const u32 ra = cpuRegs.GPR.n.ra.UL[0];
					u32* pPC = (u32*)PSM(pc & ~0xF);
					u32* pRA = (u32*)PSM(ra & ~0xF);
					if (pPC) {
						Console.Error("@@IOP_EXCVEC_HFCHECK@@ ee_insn[%08x]=%08x %08x %08x %08x %08x %08x %08x %08x",
							pc & ~0xF, pPC[0], pPC[1], pPC[2], pPC[3], pPC[4], pPC[5], pPC[6], pPC[7]);
						u32* pPC2 = (u32*)PSM((pc & ~0xF) - 0x20);
						if (pPC2) Console.Error("@@IOP_EXCVEC_HFCHECK@@ ee_insn[%08x]=%08x %08x %08x %08x %08x %08x %08x %08x",
							(pc & ~0xF) - 0x20, pPC2[0], pPC2[1], pPC2[2], pPC2[3], pPC2[4], pPC2[5], pPC2[6], pPC2[7]);
					}
					if (pRA) {
						Console.Error("@@IOP_EXCVEC_HFCHECK@@ ee_ra_insn[%08x]=%08x %08x %08x %08x %08x %08x %08x %08x",
							ra & ~0xF, pRA[0], pRA[1], pRA[2], pRA[3], pRA[4], pRA[5], pRA[6], pRA[7]);
						u32* pRA2 = (u32*)PSM((ra & ~0xF) + 0x20);
						if (pRA2) Console.Error("@@IOP_EXCVEC_HFCHECK@@ ee_ra_insn[%08x]=%08x %08x %08x %08x %08x %08x %08x %08x",
							(ra & ~0xF) + 0x20, pRA2[0], pRA2[1], pRA2[2], pRA2[3], pRA2[4], pRA2[5], pRA2[6], pRA2[7]);
						u32* pRA3 = (u32*)PSM((ra & ~0xF) + 0x40);
						if (pRA3) Console.Error("@@IOP_EXCVEC_HFCHECK@@ ee_ra_insn[%08x]=%08x %08x %08x %08x %08x %08x %08x %08x",
							(ra & ~0xF) + 0x40, pRA3[0], pRA3[1], pRA3[2], pRA3[3], pRA3[4], pRA3[5], pRA3[6], pRA3[7]);
						u32* pRA4 = (u32*)PSM((ra & ~0xF) + 0x60);
						if (pRA4) Console.Error("@@IOP_EXCVEC_HFCHECK@@ ee_ra_insn[%08x]=%08x %08x %08x %08x %08x %08x %08x %08x",
							(ra & ~0xF) + 0x60, pRA4[0], pRA4[1], pRA4[2], pRA4[3], pRA4[4], pRA4[5], pRA4[6], pRA4[7]);
						u32* pRA5 = (u32*)PSM((ra & ~0xF) + 0x80);
						if (pRA5) Console.Error("@@IOP_EXCVEC_HFCHECK@@ ee_ra_insn[%08x]=%08x %08x %08x %08x %08x %08x %08x %08x",
							(ra & ~0xF) + 0x80, pRA5[0], pRA5[1], pRA5[2], pRA5[3], pRA5[4], pRA5[5], pRA5[6], pRA5[7]);
						u32* pRA6 = (u32*)PSM((ra & ~0xF) + 0xa0);
						if (pRA6) Console.Error("@@IOP_EXCVEC_HFCHECK@@ ee_ra_insn[%08x]=%08x %08x %08x %08x %08x %08x %08x %08x",
							(ra & ~0xF) + 0xa0, pRA6[0], pRA6[1], pRA6[2], pRA6[3], pRA6[4], pRA6[5], pRA6[6], pRA6[7]);
					}
					// Also dump host pointers for iopMem and eeMem for comparison
					Console.Error("@@IOP_EXCVEC_HFCHECK@@ iopMem_Main=%p eeMem_Main=%p diff=%lld",
						(void*)iopMem->Main, (void*)eeMem->Main,
						(long long)((uintptr_t)iopMem->Main - (uintptr_t)eeMem->Main));
				}
			}
		}
	}
#endif // IPSX2_EE_EVT_DIAG (span A: EE_EVT_PC..IOP_EXCVEC_HFCHECK)

	const bool safe_only = iPSX2_IsSafeOnlyEnabled();
	const bool diag_enabled = (!safe_only && iPSX2_GetRuntimeEnvBool("iPSX2_ENABLE_DIAG_FLAGS", false));

    // [TEMP_DIAG][REMOVE_AFTER=ROM_RESET_SCAN_V1] One-shot ROM scan for ROMDIR "RESET" entry
    if (diag_enabled)
    {
        static bool s_rom_reset_scanned = false;
        if (!s_rom_reset_scanned) {
            s_rom_reset_scanned = true;
            bool found = false;
            u32 found_addr = 0;
            u8 first16[16] = {};
            
            // Scan BIOS ROM range for 16-byte aligned "RESET" entry
            for (u32 addr = 0xBFC00000; addr < 0xBFC80000 && !found; addr += 16) {
                u32* pMem = (u32*)PSM(addr);
                if (pMem) {
                    // Check for "RESE" (0x45534552) + 'T' (0x54)
                    if (pMem[0] == 0x45534552) {  // "RESE" little-endian
                        u8* bytes = (u8*)pMem;
                        if (bytes[4] == 0x54) {  // 'T' at offset 4
                            found = true;
                            found_addr = addr;
                            for (int i = 0; i < 16; i++) first16[i] = bytes[i];
                        }
                    }
                }
            }
            
            if (found) {
                Console.WriteLn("@@ROM_RESET_FOUND@@ addr=%08x bytes=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                    found_addr,
                    first16[0], first16[1], first16[2], first16[3],
                    first16[4], first16[5], first16[6], first16[7],
                    first16[8], first16[9], first16[10], first16[11],
                    first16[12], first16[13], first16[14], first16[15]);
            } else {
                Console.WriteLn("@@ROM_RESET_NOT_FOUND@@");
            }
        }
    }

#if IPSX2_EE_EVT_DIAG
	// [span B] dbg_mask は intcInterrupt()|dmacInterrupt() を毎回 2 回呼ぶが純診断。
	// 機能 mask_check (後段) と同 cycle で同値のため除去は bit-exact。
	static int evt_log_limit = 0;
	uint dbg_mask = intcInterrupt() | dmacInterrupt();
	if (diag_enabled && dbg_mask && (evt_log_limit++ % 600 == 0)) { // Log occasionally if interrupts are pending
		Console.WriteLn("DEBUG: _cpuEventTest. Mask=%x, Enabled=%d, Status=%x, CAUSE=%x",
			dbg_mask, cpuIntsEnabled(dbg_mask), cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause);
	}
#endif // IPSX2_EE_EVT_DIAG (span B: dbg_mask)

	eeEventTestIsActive = true;
	cpuRegs.nextEventCycle = cpuRegs.cycle + eeWaitCycles;
	cpuRegs.lastEventCycle = cpuRegs.cycle;
	// ---- INTC / DMAC (CPU-level Exceptions) -----------------
	// [TEMP_DIAG][STEP4] ROM Spotcheck
    static bool s_rom_spotcheck_done = false;
    if (diag_enabled && !s_rom_spotcheck_done) {
        s_rom_spotcheck_done = true;
        u32 val0 = memRead32(0xBFC00000);
        u32 val1 = memRead32(0xBFC02740);
        Console.WriteLn("@@ROM_SPOTCHECK@@ 0xBFC00000=%08x (expect 401A7800) 0xBFC02740=%08x (expect 45534552)", val0, val1);
    }

	// Done first because exceptions raised during event tests need to be postponed a few
	// cycles (fixes Grandia II [PAL], which does a spin loop on a vsync and expects to

	// be able to read the value before the exception handler clears it).

	// Debug logging removed from here (handled above)

	uint mask_check = intcInterrupt() | dmacInterrupt();
#if IPSX2_EE_EVT_DIAG
	// [span C] DMAC/INTC_INT_CHECK は dmac/intcInterrupt() を追加で呼ぶが純診断。
	// 上の機能 mask_check と同 cycle = 同値で除去は bit-exact。
	// [TEMP_DIAG] @@DMAC_INT_CHECK@@ — log when DMAC interrupt is pending
	{
		static int s_dmac_chk_n = 0;
		uint dmac_only = dmacInterrupt();
		if (dmac_only && s_dmac_chk_n < 20) {
			Console.WriteLn("@@DMAC_INT_CHECK@@ n=%d mask=%x dmac=%x enabled=%d DMAC_STAT=%08x COP0_Status=%08x ee_pc=%08x cyc=%u",
				s_dmac_chk_n++, mask_check, dmac_only,
				(int)cpuIntsEnabled(mask_check),
				psHu32(0xE010), cpuRegs.CP0.n.Status.val, cpuRegs.pc, cpuRegs.cycle);
		}
	}
	// [TEMP_DIAG] @@INTC_INT_CHECK@@ — log INTC interrupt delivery status
	// Removal condition: BIOS browserafter confirmed
	{
		static int s_intc_chk_n = 0;
		uint intc_only = intcInterrupt();
		if (intc_only && s_intc_chk_n < 30) {
			u32 stat = cpuRegs.CP0.n.Status.val;
			Console.WriteLn("@@INTC_INT_CHECK@@ n=%d intc=%x mask_all=%x enabled=%d IE=%d EIE=%d EXL=%d ERL=%d IntType=%d INTC_STAT=%08x INTC_MASK=%08x pc=%08x cyc=%u",
				s_intc_chk_n++, intc_only, mask_check,
				(int)cpuIntsEnabled(mask_check),
				(int)((stat >> 0) & 1),   // IE
				(int)((stat >> 16) & 1),  // EIE
				(int)((stat >> 1) & 1),   // EXL
				(int)((stat >> 2) & 1),   // ERL
				(int)!!(stat & intc_only), // IntType check
				psHu32(INTC_STAT), psHu32(INTC_MASK),
				cpuRegs.pc, cpuRegs.cycle);
		}
	}
#endif // IPSX2_EE_EVT_DIAG (span C: DMAC/INTC_INT_CHECK)
	if (mask_check && cpuIntsEnabled(mask_check))
		cpuException(mask_check, cpuRegs.branch);

	// * The IOP cannot always be run.  If we run IOP code every time through the
	//   cpuEventTest, the IOP generally starts to run way ahead of the EE.

	// It's also important to sync up the IOP before updating the timers, since gates will depend on starting/stopping in the right place!
	EEsCycle += cpuRegs.cycle - EEoCycle;
	EEoCycle = cpuRegs.cycle;

	if (EEsCycle > 0)
		iopEventAction = true;

	if (iopEventAction)
	{
#if IPSX2_EE_EVT_DIAG
		// [TEMP_DIAG] @@IOP_STUCK@@ wall-clock check around IOP execution
		{
			static uint64_t s_iop_wall_prev = 0;
			static int s_iop_stuck_n = 0;
			struct timeval tv0;
			gettimeofday(&tv0, nullptr);
			uint64_t before_us = (uint64_t)tv0.tv_sec * 1000000ULL + tv0.tv_usec;
			EEsCycle = psxCpu->ExecuteBlock(EEsCycle);
			struct timeval tv1;
			gettimeofday(&tv1, nullptr);
			uint64_t after_us = (uint64_t)tv1.tv_sec * 1000000ULL + tv1.tv_usec;
			uint64_t elapsed_us = after_us - before_us;
			if (elapsed_us > 500000ULL && s_iop_stuck_n < 5) { // > 0.5 second
				Console.WriteLn("@@IOP_STUCK@@ n=%d elapsed_ms=%u ee_pc=%08x ee_cyc=%u iop_pc=%08x iop_cyc=%u frame=%u",
					s_iop_stuck_n++, (unsigned)(elapsed_us / 1000),
					cpuRegs.pc, cpuRegs.cycle,
					psxRegs.pc, psxRegs.cycle, g_FrameCount);
			}
		}
#else
		// [span D] @@IOP_STUCK@@ の wall-clock 計測 wrapper のみ除去 (gettimeofday x2/event-test)。
		// 機能 EE→IOP catch-up ExecuteBlock は必ず保持。
		EEsCycle = psxCpu->ExecuteBlock(EEsCycle);
#endif // IPSX2_EE_EVT_DIAG (span D: IOP_STUCK wrapper)
		iopEventAction = false;
	}

	iopEventTest();

	static int s_fix_event_cycle_stall_enabled = -1;
	if (s_fix_event_cycle_stall_enabled < 0)
		s_fix_event_cycle_stall_enabled = (diag_enabled && iPSX2_GetRuntimeEnvBool("iPSX2_FIX_EVENT_CYCLE_STALL", false)) ? 1 : 0;
	if (s_fix_event_cycle_stall_enabled)
	{
		static u32 s_prev_pc = 0;
		static u32 s_prev_cycle = 0;
		static u32 s_stall_hits = 0;
		if (cpuRegs.pc == s_prev_pc && cpuRegs.cycle == s_prev_cycle)
		{
			cpuRegs.cycle++;
			if (s_stall_hits < 8)
			{
				Console.WriteLn("@@FIX_EVENT_CYCLE_STALL@@ pc=%08x cycle=%u->%u", cpuRegs.pc, s_prev_cycle, cpuRegs.cycle);
			}
			s_stall_hits++;
		}
		s_prev_pc = cpuRegs.pc;
		s_prev_cycle = cpuRegs.cycle;
	}

	if (cpuTestCycle(nextStartCounter, nextDeltaCounter))
	{
		rcntUpdate();
		_cpuTestPERF();
	}

	_cpuTestTIMR();

	// ---- Interrupts -------------
	// These are basically just DMAC-related events, which also piggy-back the same bits as
	// the PS2's own DMA channel IRQs and IRQ Masks.

	if (cpuRegs.interrupt)
	{
		// This is a BIOS hack because the coding in the BIOS is terrible but the bug is masked by Data Cache
		// where a DMA buffer is overwritten without waiting for the transfer to end, which causes the fonts to get all messed up
		// so to fix it, we run all the DMA's instantly when in the BIOS.
		// Only use the lower 17 bits of the cpuRegs.interrupt as the upper bits are for VU0/1 sync which can't be done in a tight loop
		if (CHECK_INSTANTDMAHACK && dmacRegs.ctrl.DMAE && !(psHu8(DMAC_ENABLER + 2) & 1) && (cpuRegs.interrupt & 0x1FFFF))
		{
			while ((cpuRegs.interrupt & 0x1FFFF) && _cpuTestInterrupts())
				;
		}
		else
			_cpuTestInterrupts();
	}

	// ---- VU Sync -------------
	// We're in a EventTest.  All dynarec registers are flushed
	// so there is no need to freeze registers here.
	CpuVU0->ExecuteBlock();
	CpuVU1->ExecuteBlock();

	if (diag_enabled)
	{
    // @@EVENT_POST@@ - Log only when Status/Cause/EPC changes or every 1000 events
    static u64 s_event_post_count = 0;
    static u32 s_last_status = 0, s_last_cause = 0, s_last_epc = 0;
    s_event_post_count++;
    
    bool changed = (cpuRegs.CP0.n.Status.val != s_last_status || 
                    cpuRegs.CP0.n.Cause != s_last_cause ||
                    cpuRegs.CP0.n.EPC != s_last_epc);

    // [iPSX2] Step 41: Dump ROMDIR (STEP41_ROMDIR_DUMP)
    static bool s_step41_done = false;
    
    if (!s_step41_done && cpuRegs.pc == 0xBFC02664) {
        s_step41_done = true;
        
        // Dump the word at 26E8 for sanity (should be native now)
        Console.WriteLn("@@STEP41_PROVE@@ insn_26E8=%08x", memRead32(0xBFC026E8));
        
        // Dump ROMDIR starting from 2840 (continued from Step 41)
        Console.WriteLn("@@ROMDIR_DUMP_START_P2@@");
        for (u32 addr = 0xBFC02840; addr < 0xBFC02A40; addr += 16) {
            u8 bytes[16];
            for (int i=0; i<16; i++) bytes[i] = (u8)memRead8(addr + i);
            char hex[64];
            snprintf(hex, 64, "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
            
            char ascii[17];
            for (int i=0; i<16; i++) ascii[i] = (bytes[i] >= 32 && bytes[i] < 127) ? (char)bytes[i] : '.';
            ascii[16] = '\0';
            
            Console.WriteLn("@@ROMDIR@@ %08x: %s | %s", addr, hex, ascii);
        }
    }
    
	    static u32 s_event_post_log_count = 0;
	    constexpr u32 k_event_post_log_max = 50;
	    if ((s_event_post_count <= 5 || changed || (s_event_post_count % 1000) == 0) &&
	        s_event_post_log_count < k_event_post_log_max) {
				if (s_event_post_count == 1 || (s_event_post_count % 1000) == 0) {
		             Console.WriteLn("@@BUILD_ID@@ STEP42_ROMDIR_EXPAND");
		        }
		        Console.WriteLn("@@EVENT_POST@@ pc=%08x Status=%08x Cause=%08x EPC=%08x%s",
		            cpuRegs.pc, cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.EPC,
		            changed ? " CHANGED" : "");
		        s_event_post_log_count++;
		        if (s_event_post_log_count == k_event_post_log_max)
		        	Console.WriteLn("@@EVENT_POST@@ capped_after=%u", k_event_post_log_max);
		        s_last_status = cpuRegs.CP0.n.Status.val;
		        s_last_cause = cpuRegs.CP0.n.Cause;
		        s_last_epc = cpuRegs.CP0.n.EPC;
		    } // End of s_event_post_count loop

	    static int s_probe_41048_cycle_enabled = -1;
	    if (s_probe_41048_cycle_enabled < 0)
	    {
	        const bool safe_only = iPSX2_IsSafeOnlyEnabled();
	        const bool diag_enabled = (!safe_only && iPSX2_GetRuntimeEnvBool("iPSX2_ENABLE_DIAG_FLAGS", false));
	        s_probe_41048_cycle_enabled = (diag_enabled && iPSX2_GetRuntimeEnvBool("iPSX2_PROBE_EE41048_CYCLE", false)) ? 1 : 0;
	        Console.WriteLn("@@CFG@@ iPSX2_PROBE_EE41048_CYCLE=%d", s_probe_41048_cycle_enabled);
	    }
		    if (s_probe_41048_cycle_enabled && cpuRegs.pc == 0x9fc41048)
	    {
	        static int s_probe_41048_cycle_count = 0;
	        if (s_probe_41048_cycle_count < 8)
	        {
	            const bool hsync_due = cpuTestCycle(nextStartCounter, nextDeltaCounter);
	            Console.WriteLn(
	                "@@EE41048_CYCLE@@ pc=%08x cycle=%u nextEvent=%u nextStart=%u nextDelta=%d hsync_due=%d",
	                cpuRegs.pc, cpuRegs.cycle, cpuRegs.nextEventCycle, nextStartCounter, nextDeltaCounter, hsync_due ? 1 : 0);
	            s_probe_41048_cycle_count++;
	        }
	        static int s_probe_41048_gpr_count = 0;
	        if (s_probe_41048_gpr_count < 20)
	        {
	            Console.WriteLn("@@EE41048_GPR@@ v0=%08x v1=%08x a3=%08x", cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0], cpuRegs.GPR.n.a3.UL[0]);
	            s_probe_41048_gpr_count++;
	        }
		    }

	    static int s_event_loadfile_probes_enabled = -1;
	    if (s_event_loadfile_probes_enabled < 0)
	    {
	        s_event_loadfile_probes_enabled = (diag_enabled && iPSX2_GetRuntimeEnvBool("iPSX2_EVENT_LOADFILE_PROBES", false)) ? 1 : 0;
	        Console.WriteLn("@@CFG@@ iPSX2_EVENT_LOADFILE_PROBES=%d", s_event_loadfile_probes_enabled);
	    }
	    if (s_event_loadfile_probes_enabled == 1)
	    {
	    // One-shot probe for BEQ-to-panic source at 0xBFC023D8.
	    static bool s_probe_23d8_logged = false;
    if (!s_probe_23d8_logged && cpuRegs.pc == 0xBFC023D8)
    {
        s_probe_23d8_logged = true;
        Console.WriteLn(
            "@@BFC023D8_PROBE@@ pc=%08x ra=%08x v0=%08x a0=%08x a1=%08x a2=%08x t0=%08x t1=%08x t2=%08x",
            cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.a0.UL[0],
            cpuRegs.GPR.n.a1.UL[0], cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.t0.UL[0],
            cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.t2.UL[0]);
        Console.WriteLn(
            "@@BFC023D8_CODE@@ w23d0=%08x w23d4=%08x w23d8=%08x w23dc=%08x",
            memRead32(0xBFC023D0), memRead32(0xBFC023D4), memRead32(0xBFC023D8), memRead32(0xBFC023DC));
    }

	    // One-shot probe: capture first entry into LoadFile prologue at 0xBFC02640.
	    static bool s_probe_2640_logged = false;
    if (!s_probe_2640_logged && cpuRegs.pc == 0xBFC02640)
    {
        s_probe_2640_logged = true;
        Console.WriteLn(
            "@@BFC02640_ENTER@@ pc=%08x ra=%08x a0=%08x a1=%08x t0=%08x t1=%08x t2=%08x",
            cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
            cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.t2.UL[0]);
	        Console.WriteLn(
	            "@@BFC02640_CODE@@ w2640=%08x w2644=%08x w2648=%08x w264c=%08x",
	            memRead32(0xBFC02640), memRead32(0xBFC02644), memRead32(0xBFC02648), memRead32(0xBFC0264C));
	    }

    // One-shot probe: capture first entry into LoadFile scan loop head at 0xBFC02650.
    static bool s_probe_2650_logged = false;
    if (!s_probe_2650_logged && cpuRegs.pc == 0xBFC02650)
    {
        s_probe_2650_logged = true;
        Console.WriteLn(
            "@@BFC02650_ENTER@@ pc=%08x ra=%08x v0=%08x t0=%08x t1=%08x t2=%08x t3=%08x t5=%08x t6=%08x",
            cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.t0.UL[0],
            cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.t2.UL[0], cpuRegs.GPR.n.t3.UL[0],
            cpuRegs.GPR.n.t5.UL[0], cpuRegs.GPR.n.t6.UL[0]);
        Console.WriteLn(
            "@@BFC02650_CODE@@ w2650=%08x w2654=%08x w2658=%08x w265c=%08x w2660=%08x w2664=%08x w2668=%08x w266c=%08x w2670=%08x w2674=%08x w2678=%08x w267c=%08x w2680=%08x w2684=%08x w2688=%08x w268c=%08x w2690=%08x w2694=%08x w2698=%08x w269c=%08x w26a0=%08x w26a4=%08x w26a8=%08x w26ac=%08x w26b0=%08x w26b4=%08x w26b8=%08x w26bc=%08x w26c0=%08x w26c4=%08x w26c8=%08x w26cc=%08x w26d0=%08x",
            memRead32(0xBFC02650), memRead32(0xBFC02654), memRead32(0xBFC02658), memRead32(0xBFC0265C),
            memRead32(0xBFC02660), memRead32(0xBFC02664), memRead32(0xBFC02668), memRead32(0xBFC0266C),
            memRead32(0xBFC02670), memRead32(0xBFC02674), memRead32(0xBFC02678), memRead32(0xBFC0267C),
            memRead32(0xBFC02680), memRead32(0xBFC02684), memRead32(0xBFC02688), memRead32(0xBFC0268C),
            memRead32(0xBFC02690), memRead32(0xBFC02694), memRead32(0xBFC02698), memRead32(0xBFC0269C),
            memRead32(0xBFC026A0), memRead32(0xBFC026A4), memRead32(0xBFC026A8), memRead32(0xBFC026AC),
            memRead32(0xBFC026B0), memRead32(0xBFC026B4), memRead32(0xBFC026B8), memRead32(0xBFC026BC),
            memRead32(0xBFC026C0), memRead32(0xBFC026C4), memRead32(0xBFC026C8), memRead32(0xBFC026CC),
            memRead32(0xBFC026D0));
    }

    // Event-side probe: trace BIOS read site at 0xBFC02664 with a hard cap.
    if (cpuRegs.pc == 0xBFC02664)
    {
        static u32 s_probe_2664_evt_count = 0;
        if (s_probe_2664_evt_count < 50)
        {
            const u32 t0 = cpuRegs.GPR.n.t0.UL[0];
            const u32 a0 = cpuRegs.GPR.n.a0.UL[0];
            Console.WriteLn(
                "@@BFC02664_EVT@@ n=%u t0=%08x t2=%08x a0=%08x a1=%08x t5=%08x t6=%08x t4=%08x m_t0=%08x m268c=%08x m26a4=%08x m26a8=%08x",
                s_probe_2664_evt_count, t0, cpuRegs.GPR.n.t2.UL[0], a0,
                cpuRegs.GPR.n.a1.UL[0], cpuRegs.GPR.n.t5.UL[0],
                cpuRegs.GPR.n.t6.UL[0], cpuRegs.GPR.n.t4.UL[0], memRead32(t0),
                memRead32(0xBFC0268C), memRead32(0xBFC026A4), memRead32(0xBFC026A8));
            s_probe_2664_evt_count++;
        }
    }

    // One-shot probe: capture real operands at BNE site (0xBFC0266C) before entering panic loop.
    static bool s_probe_266c_logged = false;
    if (!s_probe_266c_logged && cpuRegs.pc == 0xBFC0266C)
    {
        s_probe_266c_logged = true;
        Console.WriteLn(
            "@@BFC0266C_PROBE@@ pc=%08x ra=%08x v0=%08x t5=%08x t0=%08x t1=%08x m_t0=%08x m_t1m8=%08x",
            cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.t5.UL[0],
            cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0],
            memRead32(cpuRegs.GPR.n.t0.UL[0]), memRead32(cpuRegs.GPR.n.t1.UL[0] - 8));
    }

    // One-shot probe: capture operands at ROMDIR size-mask check branch (0xBFC026A4).
    static bool s_probe_26a4_logged = false;
    if (!s_probe_26a4_logged && cpuRegs.pc == 0xBFC026A4)
    {
        s_probe_26a4_logged = true;
        Console.WriteLn(
            "@@BFC026A4_PROBE@@ pc=%08x v0=%08x t2=%08x t0=%08x t1=%08x m_t0=%08x m_t1m4=%08x m_t1m8=%08x",
            cpuRegs.pc, cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.t2.UL[0], cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0],
            memRead32(cpuRegs.GPR.n.t0.UL[0]), memRead32(cpuRegs.GPR.n.t1.UL[0] - 4), memRead32(cpuRegs.GPR.n.t1.UL[0] - 8));
    }

    // [TEMP_HACK][REMOVE_AFTER=JIT_ADDU_T0A0_ROOTFIX] Semantic guard for BIOS LoadFile entry.
    // Root evidence: BIOS opcode "addu t0,a0,zero" is at bfc02640, but runtime reaches bfc02650 with stale t0.
    // Keep t0 synchronized with a0 at entry/loop-head until the dynarec root fix is in place.
    static bool s_temp_hack_t0a0_logged = false;
    static int s_temp_hack_t0a0_enabled = -1;
    if (s_temp_hack_t0a0_enabled < 0)
    {
        s_temp_hack_t0a0_enabled = (diag_enabled && iPSX2_GetRuntimeEnvBool("iPSX2_TEMP_HACK_T0A0_SYNC", false)) ? 1 : 0;
        Console.WriteLn("@@CFG@@ iPSX2_TEMP_HACK_T0A0_SYNC=%d", s_temp_hack_t0a0_enabled);
    }
    if (s_temp_hack_t0a0_enabled == 1 && (cpuRegs.pc == 0xBFC02640 || cpuRegs.pc == 0xBFC02650))
    {
        cpuRegs.GPR.n.t0.UL[0] = cpuRegs.GPR.n.a0.UL[0];
        if (!s_temp_hack_t0a0_logged)
        {
            s_temp_hack_t0a0_logged = true;
            Console.WriteLn("@@TEMP_HACK@@ name=SYNC_T0_FROM_A0_AT_2640_2650 enabled=1 remove_when=JIT_ADDU_T0A0_FIXED");
        }
    }

    // One-shot probe: capture operands at extension compare branch (0xBFC026C0).
    static bool s_probe_26c0_logged = false;
    if (!s_probe_26c0_logged && cpuRegs.pc == 0xBFC026C0)
    {
        s_probe_26c0_logged = true;
        Console.WriteLn(
            "@@BFC026C0_PROBE@@ pc=%08x v0=%08x t3=%08x t0=%08x t1=%08x t2=%08x m_t0=%08x m_t1m8=%08x m_t1p4=%08x",
            cpuRegs.pc, cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.t3.UL[0], cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.t2.UL[0],
            memRead32(cpuRegs.GPR.n.t0.UL[0]), memRead32(cpuRegs.GPR.n.t1.UL[0] - 8), memRead32(cpuRegs.GPR.n.t1.UL[0] + 4));
    }

    // Focused trace: TBIN compare stage around 0xBFC02684.
    if (cpuRegs.pc == 0xBFC02684)
    {
        static u32 s_probe_2684_evt_count = 0;
        if (s_probe_2684_evt_count < 32)
        {
            const u32 t0 = cpuRegs.GPR.n.t0.UL[0];
            const u32 t1 = cpuRegs.GPR.n.t1.UL[0];
            Console.WriteLn(
                "@@BFC02684_EVT@@ n=%u pc=%08x v0=%08x t6=%08x t0=%08x t1=%08x m_t0=%08x m_t1m8=%08x",
                s_probe_2684_evt_count, cpuRegs.pc, cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.t6.UL[0],
                t0, t1, memRead32(t0), memRead32(t1 - 8));
            s_probe_2684_evt_count++;
        }
    }

    // [FIX9 VERIFY] bfc026a4 実行時probe: JITがNOPを実行しているかBNEを実行しているかをverify
    if (cpuRegs.pc == 0xBFC026A4) {
        static int s_26a4_cnt = 0;
        if (s_26a4_cnt < 4) {
            s_26a4_cnt++;
            u32 rom_insn = (eeMem && eeMem->ROM) ? *(u32*)&eeMem->ROM[0x26A4] : 0xDEADBEEF;
            Console.WriteLn("@@BFC026A4_EXEC@@ cnt=%d rom_insn=%08x v0=%08x t2=%08x t0=%08x",
                s_26a4_cnt, rom_insn,
                cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.t2.UL[0], cpuRegs.GPR.n.t0.UL[0]);
        }
    }

    // One-shot probe for BIOS panic-loop branch inputs at 0xBFC02454.
    static bool s_panic_loop_probe_done = false;
    if (!s_panic_loop_probe_done && cpuRegs.pc == 0xBFC02454)
    {
        s_panic_loop_probe_done = true;
        Console.WriteLn(
            "@@PANIC_LOOP_PROBE@@ pc=%08x t0=%08x t1=%08x v0=%08x at_2454=%08x at_2458=%08x at_246c=%08x",
            cpuRegs.pc, cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.v0.UL[0],
            memRead32(0xBFC02454), memRead32(0xBFC02458), memRead32(0xBFC0246C));

        // One-shot ROM scan + two-path probe for RESET signature addressing.
        if (eeMem && eeMem->ROM)
        {
            const s32 reset_off = iPSX2_FindAsciiInRom(eeMem->ROM, Ps2MemSize::Rom, "RESET");
            const s32 romdir_off = iPSX2_FindAsciiInRom(eeMem->ROM, Ps2MemSize::Rom, "ROMDIR");
            const s32 tbin_off = iPSX2_FindAsciiInRom(eeMem->ROM, Ps2MemSize::Rom, "TBIN");
            Console.WriteLn("@@ROM_SCAN@@ reset_off=0x%08x romdir_off=0x%08x tbin_off=0x%08x size=0x%08x",
                static_cast<u32>(reset_off), static_cast<u32>(romdir_off), static_cast<u32>(tbin_off), Ps2MemSize::Rom);

            const u32 current_t0 = cpuRegs.GPR.n.t0.UL[0];
            const u32 expected_addr = (reset_off >= 0) ? (0xBFC00000u + static_cast<u32>(reset_off)) : 0xFFFFFFFFu;
            if (expected_addr != 0xFFFFFFFFu)
            {
                const u32 vtlb_expected = memRead32(expected_addr);
                u32 buf_expected = 0;
                if (iPSX2_ReadRomBuf32(expected_addr, &buf_expected))
                {
                    Console.WriteLn("@@ROM_PROBE@@ kind=expected addr=%08x vtlb=%08x buf=%08x", expected_addr, vtlb_expected, buf_expected);
                }
                else
                {
                    Console.WriteLn("@@ROM_PROBE@@ kind=expected addr=%08x vtlb=%08x buf=unmapped", expected_addr, vtlb_expected);
                }
            }
            const u32 vtlb_t0 = memRead32(current_t0);
            u32 buf_t0 = 0;
            if (iPSX2_ReadRomBuf32(current_t0, &buf_t0))
                Console.WriteLn("@@ROM_PROBE@@ kind=current_t0 addr=%08x vtlb=%08x buf=%08x", current_t0, vtlb_t0, buf_t0);
            else
                Console.WriteLn("@@ROM_PROBE@@ kind=current_t0 addr=%08x vtlb=%08x buf=unmapped", current_t0, vtlb_t0);

            Console.WriteLn("@@BIOS_DISASM_START@@ window=bfc02640..bfc02690 source=rom_buffer");
            for (u32 addr = 0xBFC02640; addr <= 0xBFC02690; addr += 4)
            {
                u32 opcode = 0;
                if (iPSX2_ReadRomBuf32(addr, &opcode))
                {
                    std::string disasm;
                    disR5900Fasm(disasm, opcode, addr, false);
                    Console.WriteLn("@@BIOS_DISASM@@ %08x: %08x  %s", addr, opcode, disasm.c_str());
                }
                else
                {
                    Console.WriteLn("@@BIOS_DISASM@@ %08x: ????????  <unmapped>", addr);
                }
            }
            Console.WriteLn("@@BIOS_DISASM_END@@");

            Console.WriteLn("@@BIOS_DISASM_START@@ window=bfc023b8..bfc023d8 source=rom_buffer");
            for (u32 addr = 0xBFC023B8; addr <= 0xBFC023D8; addr += 4)
            {
                u32 opcode = 0;
                if (iPSX2_ReadRomBuf32(addr, &opcode))
                {
                    std::string disasm;
                    disR5900Fasm(disasm, opcode, addr, false);
                    Console.WriteLn("@@BIOS_DISASM@@ %08x: %08x  %s", addr, opcode, disasm.c_str());
                }
                else
                {
                    Console.WriteLn("@@BIOS_DISASM@@ %08x: ????????  <unmapped>", addr);
                }
            }
            Console.WriteLn("@@BIOS_DISASM_END@@");

            Console.WriteLn("@@BIOS_DISASM_START@@ window=bfc02408..bfc02428 source=rom_buffer");
            for (u32 addr = 0xBFC02408; addr <= 0xBFC02428; addr += 4)
            {
                u32 opcode = 0;
                if (iPSX2_ReadRomBuf32(addr, &opcode))
                {
                    std::string disasm;
                    disR5900Fasm(disasm, opcode, addr, false);
                    Console.WriteLn("@@BIOS_DISASM@@ %08x: %08x  %s", addr, opcode, disasm.c_str());
                }
                else
                {
                    Console.WriteLn("@@BIOS_DISASM@@ %08x: ????????  <unmapped>", addr);
                }
            }
            Console.WriteLn("@@BIOS_DISASM_END@@");
        }
	        iPSX2_DumpLfCmpStoreRing();
	    }
	    }

	    // [ROMDIR_LOOP_PROBE] Runtime-gated loop probe for LoadFile scan branch points.
    // Uses existing flag so default behavior remains unchanged.
    if (DarwinMisc::iPSX2_FORCE_JIT_VERIFY &&
        (cpuRegs.pc == 0xBFC02664 || cpuRegs.pc == 0xBFC02684 || cpuRegs.pc == 0xBFC0268C || cpuRegs.pc == 0xBFC02714))
    {
        static u32 s_romdir_probe_count = 0;
        if (s_romdir_probe_count < 50)
        {
            const u32 t0 = cpuRegs.GPR.n.t0.UL[0];
            const u32 t1 = cpuRegs.GPR.n.t1.UL[0];
            const u32 t2 = cpuRegs.GPR.n.t2.UL[0];
            const u32 v0 = cpuRegs.GPR.n.v0.UL[0];
            const u32 t5 = cpuRegs.GPR.n.t5.UL[0];
            const u32 t6 = cpuRegs.GPR.n.t6.UL[0];
            const u32 mem_t0 = memRead32(t0);
            const u32 mem_t1m8 = memRead32(t1 - 8);
            const u32 mem_t1m4 = memRead32(t1 - 4);
            s_romdir_probe_count++;
            Console.WriteLn(
                "@@ROMDIR_LOOP_PROBE@@ #%u pc=%08x t0=%08x t1=%08x t2=%08x v0=%08x t5=%08x t6=%08x m[t0]=%08x m[t1-8]=%08x m[t1-4]=%08x",
                s_romdir_probe_count, cpuRegs.pc, t0, t1, t2, v0, t5, t6, mem_t0, mem_t1m8, mem_t1m4);
        }
    }

    // [STEP2] Flight Recorder Dump Logic moved to Step2_CheckDump (called from JIT)


    
    // [TEMP_DIAG][REMOVE_AFTER=LF_ENTRY_CAPTURE_V1] Dump LoadFile entry a0/ra once captured
    {
        extern volatile u32 g_lf_entry_a0;
        extern volatile u32 g_lf_entry_ra;
        extern volatile u32 g_lf_entry_seen;
        static bool s_lf_entry_dumped = false;
        if (g_lf_entry_seen && !s_lf_entry_dumped) {
            s_lf_entry_dumped = true;
            u32 callsite = g_lf_entry_ra - 8;  // JAL is at ra-8
            Console.WriteLn("@@LF_ENTRY@@ a0=%08x ra=%08x callsite_pc=%08x",
                g_lf_entry_a0, g_lf_entry_ra, callsite);
        }
    }
    
    // [TEMP_DIAG][REMOVE_AFTER=LF_RESE_CHECK_V1] Dump LoadFile BNE compare t0/v0 + ROM bytes
    {
        extern volatile u32 g_lf_match_seen;
        extern volatile u32 g_lf_match_t0;
        extern volatile u32 g_lf_match_v0;
        static bool s_lf_match_dumped = false;
        if (g_lf_match_seen && !s_lf_match_dumped) {
            s_lf_match_dumped = true;
            u32 t0 = g_lf_match_t0;
            u32 v0 = g_lf_match_v0;
            
            // Read 16 bytes from ROM at t0 address
            u8 bytes[16] = {};
            char ascii[17] = {};
            u32* pMem = (u32*)PSM(t0);
            if (pMem) {
                for (int i = 0; i < 16; i++) {
                    bytes[i] = ((u8*)pMem)[i];
                    ascii[i] = (bytes[i] >= 0x20 && bytes[i] < 0x7F) ? bytes[i] : '.';
                }
                ascii[16] = '\0';
            }
            
            u32 le32_from_bytes = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
            
            Console.WriteLn("@@LF_RESE_CHECK@@ t0=%08x v0=%08x bytes=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x ascii=%s le32=%08x",
                t0, v0,
                bytes[0], bytes[1], bytes[2], bytes[3],
                bytes[4], bytes[5], bytes[6], bytes[7],
                bytes[8], bytes[9], bytes[10], bytes[11],
                bytes[12], bytes[13], bytes[14], bytes[15],
                ascii, le32_from_bytes);
        }
    }

    // [TEMP_DIAG][REMOVE_AFTER=LF_PRECALL_CAPTURE_V1] Dump LoadFile callsite pre-JAL registers once.
    {
        extern volatile u32 g_lf_precall_seen;
        extern volatile u32 g_lf_precall_a0;
        extern volatile u32 g_lf_precall_v0;
        extern volatile u32 g_lf_precall_t0;
        extern volatile u32 g_lf_precall_ra;
        extern volatile u32 g_lf_precall_flags;
        static bool s_lf_precall_dumped = false;
        if (g_lf_precall_seen && !s_lf_precall_dumped) {
            s_lf_precall_dumped = true;
            Console.WriteLn("@@LF_PRECALL@@ pc=bfc023d0 a0=%08x v0=%08x t0=%08x ra=%08x flags=%08x",
                g_lf_precall_a0, g_lf_precall_v0, g_lf_precall_t0, g_lf_precall_ra, g_lf_precall_flags);
        }
    }
    
    // [TEMP_DIAG][REMOVE_AFTER=LF_CMP_USED_V1] Dump BNE compare operands captured from branch emit
    {
        extern volatile u32 g_lf_cmp_seen;
        extern volatile u32 g_lf_cmp_pc;
        extern volatile u32 g_lf_cmp_t0;
        extern volatile u32 g_lf_cmp_rs;
        extern volatile u32 g_lf_cmp_rt;
        extern volatile u32 g_lf_cmp_flags;
        static bool s_lf_cmp_dumped = false;
        if (g_lf_cmp_seen && !s_lf_cmp_dumped) {
            s_lf_cmp_dumped = true;
            u32 t0 = g_lf_cmp_t0;
            u32 rs = g_lf_cmp_rs;  // v0 value as used in compare
            u32 rt = g_lf_cmp_rt;  // t5 value as used in compare
            u32 flags = g_lf_cmp_flags;
            
            // Read 16 bytes from ROM at t0 address
            u8 bytes[16] = {};
            char ascii[17] = {};
            u32* pMem = (u32*)PSM(t0);
            if (pMem) {
                for (int i = 0; i < 16; i++) {
                    bytes[i] = ((u8*)pMem)[i];
                    ascii[i] = (bytes[i] >= 0x20 && bytes[i] < 0x7F) ? bytes[i] : '.';
                }
                ascii[16] = '\0';
            }
            
            u32 le32_from_bytes = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
            
            // flags: bit0=Rs from hostreg, bit1=Rt from hostreg, bit2=Rt is const
            const char* rs_src = (flags & 1) ? "HOSTREG" : "CPUREGS";
            const char* rt_src = (flags & 4) ? "CONST" : ((flags & 2) ? "HOSTREG" : "CPUREGS");
            
            Console.WriteLn("@@LF_CMP_USED@@ pc=%08x t0=%08x rs=%08x(%s) rt=%08x(%s) bytes=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x ascii=%s le32=%08x",
                g_lf_cmp_pc, t0, rs, rs_src, rt, rt_src,
                bytes[0], bytes[1], bytes[2], bytes[3],
                bytes[4], bytes[5], bytes[6], bytes[7],
                bytes[8], bytes[9], bytes[10], bytes[11],
                bytes[12], bytes[13], bytes[14], bytes[15],
                ascii, le32_from_bytes);
        }
    }
    
    // [TEMP_DIAG][REMOVE_AFTER=LW_ONESHOT_CAP_V1] Dump LW one-shot capture
    {
        extern volatile u32 g_lw_seen;
        extern volatile u32 g_lw_guest_pc;
        extern volatile u32 g_lw_addr;
        extern volatile u32 g_lw_val;
        extern volatile u32 g_lw_path;
        static bool s_lw_dumped = false;
        if (g_lw_seen && !s_lw_dumped) {
            s_lw_dumped = true;
            u32 addr = g_lw_addr;
            u32 val = g_lw_val;
            u32 path = g_lw_path;
            u32 guest_pc = g_lw_guest_pc;
            
            // Read 16 bytes from ROM at addr
            u8 bytes[16] = {};
            char ascii[17] = {};
            u32* pMem = (u32*)PSM(addr);
            if (pMem) {
                for (int i = 0; i < 16; i++) {
                    bytes[i] = ((u8*)pMem)[i];
                    ascii[i] = (bytes[i] >= 0x20 && bytes[i] < 0x7F) ? bytes[i] : '.';
                }
                ascii[16] = '\0';
            }
            
            u32 le32_from_bytes = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
            
            Console.WriteLn("@@LW_ONESHOT@@ pc=%08x addr=%08x path=%u lw_val=%08x le32=%08x bytes=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x ascii=%s",
                guest_pc, addr, path, val, le32_from_bytes,
                bytes[0], bytes[1], bytes[2], bytes[3],
                bytes[4], bytes[5], bytes[6], bytes[7],
                bytes[8], bytes[9], bytes[10], bytes[11],
                bytes[12], bytes[13], bytes[14], bytes[15],
                ascii);
        }
    }
    
    // [TEMP_DIAG][REMOVE_AFTER=LOADFILE_CMP_PACK_V1] Dump LoadFile BNE compare ring buffer
    {
        // LfCmpEntry layout: tag, branch_pc, t0_val, rs_used, rt_used, taken (6 u32s each, 32 entries = 192 u32s)
        // We access as raw u32 array via reinterpret_cast on the actual storage
        extern volatile u32 g_lf_cmp_ring[];  // actually LfCmpEntry[32], accessed as u32[192]
        extern volatile u32 g_lf_cmp_ring_idx;
        extern volatile u32 g_lf_cmp_freeze;
        static bool s_cmp_pack_dumped = false;
        
        u32 idx = g_lf_cmp_ring_idx;
        if (idx > 0 && !s_cmp_pack_dumped) {
            s_cmp_pack_dumped = true;
            Console.WriteLn("@@LF_CMP_PACK_DUMP@@ idx=%u", idx);
            
            int count = (idx < 32) ? idx : 32;
            for (int i = 0; i < count; i++) {
                int slot = i & 31;
                const volatile u32* e = &g_lf_cmp_ring[slot * 6];
                u32 tag = e[0];
                u32 branch_pc = e[1];
                u32 t0_val = e[2];
                u32 rs_used = e[3];
                u32 rt_used = e[4];
                u32 taken = e[5];
                
                // Read ROM bytes at t0 address
                u8 bytes[4] = {};
                u32* pMem = (u32*)PSM(t0_val);
                u32 le32 = 0;
                if (pMem) {
                    for (int j = 0; j < 4; j++) bytes[j] = ((u8*)pMem)[j];
                    le32 = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
                }
                
                const char* tag_name = (tag == 1) ? "RESE" : "T";
                Console.WriteLn("@@LF_CMP_PACK@@ tag=%s pc=%08x t0=%08x rs=%08x rt=%08x taken=%u le32=%08x",
                    tag_name, branch_pc, t0_val, rs_used, rt_used, taken, le32);
            }
        }
    }
    
    // [TEMP_DIAG][REMOVE_AFTER=LF_PROBE_PACK_V3] Dump LoadFile V3 probe entries
    {
        // V3 entry layout: tag, guest_pc, t0_val, rs_val, rt_val, taken, t2_val (7 u32s)
        extern volatile u32 g_lf_v3_ring[];  // actually LfProbeV3Entry[8]
        extern volatile u32 g_lf_v3_idx;
        extern volatile u32 g_lf_v3_done;
        static bool s_v3_dumped = false;
        
        u32 idx = g_lf_v3_idx;
        if (idx > 0 && !s_v3_dumped && g_lf_v3_done) {
            s_v3_dumped = true;
            Console.WriteLn("@@LF_PACK_V3_DUMP@@ count=%u", idx);
            
            int count = (idx < 8) ? idx : 8;
            for (int i = 0; i < count; i++) {
                int slot = i & 7;
                const volatile u32* e = &g_lf_v3_ring[slot * 7];
                u32 tag = e[0];
                u32 guest_pc = e[1];
                u32 t0_val = e[2];
                u32 rs_val = e[3];
                u32 rt_val = e[4];
                u32 taken = e[5];
                u32 t2_val = e[6];
                
                // Convert tag to string
                char tag_str[5] = {};
                tag_str[0] = (tag >> 0) & 0xFF;
                tag_str[1] = (tag >> 8) & 0xFF;
                tag_str[2] = (tag >> 16) & 0xFF;
                tag_str[3] = (tag >> 24) & 0xFF;
                tag_str[4] = '\0';
                
                Console.WriteLn("@@LF_PACK_V3@@ tag=%s pc=%08x t0=%08x rs=%08x rt=%08x taken=%u t2=%08x",
                    tag_str, guest_pc, t0_val, rs_val, rt_val, taken, t2_val);
            }
        }
    }
    
    // [TEMP_DIAG][REMOVE_AFTER=LF_DISASM_V1] One-shot BIOS disassembly dump for LoadFile region
    {
        static bool s_disasm_dumped = false;
        // Trigger: when we reach any PC in the LoadFile scan region
        if (!s_disasm_dumped && cpuRegs.pc >= 0xBFC02660 && cpuRegs.pc <= 0xBFC026A0) {
            s_disasm_dumped = true;
            Console.WriteLn("@@BIOS_DISASM_START@@ window=bfc02660..bfc026a0");
            
            // Dump 0xBFC02660..0xBFC026A0 (17*4 = 68 bytes, 17 instructions)
            for (u32 addr = 0xBFC02660; addr <= 0xBFC026A0; addr += 4) {
                u32* pMem = (u32*)PSM(addr);
                if (pMem) {
                    u32 opcode = *pMem;
                    
                    // Basic MIPS opcode decode for common instructions
                    u32 op = (opcode >> 26) & 0x3F;
                    u32 rs = (opcode >> 21) & 0x1F;
                    u32 rt = (opcode >> 16) & 0x1F;
                    u32 rd = (opcode >> 11) & 0x1F;
                    s16 imm = (s16)(opcode & 0xFFFF);
                    u32 funct = opcode & 0x3F;
                    
                    const char* regnames[] = {
                        "zero","at","v0","v1","a0","a1","a2","a3",
                        "t0","t1","t2","t3","t4","t5","t6","t7",
                        "s0","s1","s2","s3","s4","s5","s6","s7",
                        "t8","t9","k0","k1","gp","sp","fp","ra"
                    };
                    
                    char mnem[64];
                    switch (op) {
                        case 0x00: // SPECIAL
                            switch (funct) {
                                case 0x00: snprintf(mnem, sizeof(mnem), "SLL %s,%s,%d", regnames[rd], regnames[rt], (opcode >> 6) & 0x1F); break;
                                case 0x02: snprintf(mnem, sizeof(mnem), "SRL %s,%s,%d", regnames[rd], regnames[rt], (opcode >> 6) & 0x1F); break;
                                case 0x03: snprintf(mnem, sizeof(mnem), "SRA %s,%s,%d", regnames[rd], regnames[rt], (opcode >> 6) & 0x1F); break;
                                case 0x08: snprintf(mnem, sizeof(mnem), "JR %s", regnames[rs]); break;
                                case 0x21: snprintf(mnem, sizeof(mnem), "ADDU %s,%s,%s", regnames[rd], regnames[rs], regnames[rt]); break;
                                case 0x25: snprintf(mnem, sizeof(mnem), "OR %s,%s,%s", regnames[rd], regnames[rs], regnames[rt]); break;
                                default: snprintf(mnem, sizeof(mnem), "SPECIAL 0x%02x", funct); break;
                            }
                            break;
                        case 0x04: snprintf(mnem, sizeof(mnem), "BEQ %s,%s,%d", regnames[rs], regnames[rt], imm); break;
                        case 0x05: snprintf(mnem, sizeof(mnem), "BNE %s,%s,%d", regnames[rs], regnames[rt], imm); break;
                        case 0x08: snprintf(mnem, sizeof(mnem), "ADDI %s,%s,%d", regnames[rt], regnames[rs], imm); break;
                        case 0x09: snprintf(mnem, sizeof(mnem), "ADDIU %s,%s,%d", regnames[rt], regnames[rs], imm); break;
                        case 0x0A: snprintf(mnem, sizeof(mnem), "SLTI %s,%s,%d", regnames[rt], regnames[rs], imm); break;
                        case 0x0C: snprintf(mnem, sizeof(mnem), "ANDI %s,%s,0x%04x", regnames[rt], regnames[rs], (u16)imm); break;
                        case 0x0F: snprintf(mnem, sizeof(mnem), "LUI %s,0x%04x", regnames[rt], (u16)imm); break;
                        case 0x20: snprintf(mnem, sizeof(mnem), "LB %s,%d(%s)", regnames[rt], imm, regnames[rs]); break;
                        case 0x21: snprintf(mnem, sizeof(mnem), "LH %s,%d(%s)", regnames[rt], imm, regnames[rs]); break;
                        case 0x23: snprintf(mnem, sizeof(mnem), "LW %s,%d(%s)", regnames[rt], imm, regnames[rs]); break;
                        case 0x24: snprintf(mnem, sizeof(mnem), "LBU %s,%d(%s)", regnames[rt], imm, regnames[rs]); break;
                        case 0x25: snprintf(mnem, sizeof(mnem), "LHU %s,%d(%s)", regnames[rt], imm, regnames[rs]); break;
                        case 0x28: snprintf(mnem, sizeof(mnem), "SB %s,%d(%s)", regnames[rt], imm, regnames[rs]); break;
                        case 0x29: snprintf(mnem, sizeof(mnem), "SH %s,%d(%s)", regnames[rt], imm, regnames[rs]); break;
                        case 0x2B: snprintf(mnem, sizeof(mnem), "SW %s,%d(%s)", regnames[rt], imm, regnames[rs]); break;
                        default: snprintf(mnem, sizeof(mnem), "OP 0x%02x", op); break;
                    }
                    
                    Console.WriteLn("@@BIOS_DISASM@@ %08x: %08x  %s", addr, opcode, mnem);
                } else {
                    Console.WriteLn("@@BIOS_DISASM@@ %08x: ????????  <unmapped>", addr);
                }
            }
            Console.WriteLn("@@BIOS_DISASM_END@@");
        }
    }

    // [TEMP_DIAG][REMOVE_AFTER=BFC02454_DISASM_V1] One-shot disasm for the current stall block.
    // Runtime-gated (default OFF) to avoid hot-path logging side effects.
    {
        static int s_probe_panic_disasm = -1;
        if (s_probe_panic_disasm < 0)
        {
            const bool safe_only = iPSX2_IsSafeOnlyEnabled();
            const bool diag_enabled = (!safe_only && iPSX2_GetRuntimeEnvBool("iPSX2_ENABLE_DIAG_FLAGS", false));
            s_probe_panic_disasm = (diag_enabled && iPSX2_GetRuntimeEnvBool("iPSX2_PROBE_PANIC_DISASM", false)) ? 1 : 0;
            Console.WriteLn("@@CFG@@ iPSX2_PROBE_PANIC_DISASM=%d", s_probe_panic_disasm);
        }

        static bool s_bfc02454_disasm_dumped = false;
        if (s_probe_panic_disasm == 1 && !s_bfc02454_disasm_dumped && cpuRegs.pc == 0xBFC02454)
        {
            s_bfc02454_disasm_dumped = true;
            Console.WriteLn("@@BFC02454_DISASM_START@@ window=bfc02454..bfc02474");
            for (u32 addr = 0xBFC02454; addr <= 0xBFC02474; addr += 4)
            {
                u32* pMem = (u32*)PSM(addr);
                if (!pMem)
                {
                    Console.WriteLn("@@BFC02454_DISASM@@ %08x: ????????", addr);
                    continue;
                }

                const u32 opcode = *pMem;
                const u32 op = (opcode >> 26) & 0x3F;
                const s16 imm = static_cast<s16>(opcode & 0xFFFF);
                if (op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07)
                {
                    const u32 target = addr + 4 + (static_cast<s32>(imm) << 2);
                    Console.WriteLn("@@BFC02454_DISASM@@ %08x: %08x op=%02x imm=%d target=%08x",
                        addr, opcode, op, static_cast<s32>(imm), target);
                }
                else
                {
                    Console.WriteLn("@@BFC02454_DISASM@@ %08x: %08x op=%02x", addr, opcode, op);
                }
            }
            Console.WriteLn("@@BFC02454_DISASM_END@@");
        }
    }

    // [TEMP_DIAG][REMOVE_AFTER=LF_WATCH_V1] LoadFile PC Watchpoints
    // Log once for each target PC to confirm control flow
    {
        static u32 s_lf_watch_hit = 0;  // Bitmask for which pcs have been logged
        static u32 s_lf_watch_2714_count = 0; // capped multi-sample for scan-loop progression
        constexpr u32 kWatchPcs[] = { 0xBFC02684, 0xBFC026A4, 0xBFC026E8, 0xBFC02714, 0xBFC0272C, 0xBFC02730 };
        constexpr int kNumWatch = 6;
        
        for (int i = 0; i < kNumWatch; i++) {
            const bool is_2714 = (kWatchPcs[i] == 0xBFC02714);
            const bool should_log = is_2714 ? (cpuRegs.pc == kWatchPcs[i] && s_lf_watch_2714_count < 16)
                                            : (cpuRegs.pc == kWatchPcs[i] && !(s_lf_watch_hit & (1u << i)));
            if (should_log) {
                if (is_2714)
                    s_lf_watch_2714_count++;
                else
                    s_lf_watch_hit |= (1u << i);
                
                u32 ra = cpuRegs.GPR.n.ra.UL[0];
                u32 a0 = cpuRegs.GPR.n.a0.UL[0];
                u32 a1 = cpuRegs.GPR.n.a1.UL[0];
                u32 a2 = cpuRegs.GPR.n.a2.UL[0];
                u32 t0 = cpuRegs.GPR.n.t0.UL[0];
                u32 t1 = cpuRegs.GPR.n.t1.UL[0];
                u32 t2 = cpuRegs.GPR.n.t2.UL[0];
                u32 v0 = cpuRegs.GPR.n.v0.UL[0];
                u32 v1 = cpuRegs.GPR.n.v1.UL[0];

                Console.WriteLn("@@LF_WATCH@@ pc=%08x ra=%08x a0=%08x a1=%08x a2=%08x t0=%08x t1=%08x t2=%08x v0=%08x v1=%08x",
                    cpuRegs.pc, ra, a0, a1, a2, t0, t1, t2, v0, v1);

                if (cpuRegs.pc == 0xBFC02684 && t1 >= 4) {
                    const u32 lh_addr = t1 - 4;
                    const u16 lh16 = memRead16(lh_addr);
                    const u8 b0 = memRead8(lh_addr);
                    const u8 b1 = memRead8(lh_addr + 1);
                    Console.WriteLn("@@LF_LH_PROBE@@ pc=%08x t1=%08x addr=%08x mem16=%04x b0=%02x b1=%02x v0=%08x",
                        cpuRegs.pc, t1, lh_addr, lh16, b0, b1, v0);
                }
                
                // Dump 16 bytes from t0 if in BIOS range
                if (t0 >= 0xBFC00000 && t0 < 0xBFC80000) {
                    Console.WriteLn("@@LF_MEM@@ tag=t0 addr=%08x bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                        t0, memRead8(t0), memRead8(t0+1), memRead8(t0+2), memRead8(t0+3),
                        memRead8(t0+4), memRead8(t0+5), memRead8(t0+6), memRead8(t0+7),
                        memRead8(t0+8), memRead8(t0+9), memRead8(t0+10), memRead8(t0+11),
                        memRead8(t0+12), memRead8(t0+13), memRead8(t0+14), memRead8(t0+15));
                }
                
                // Dump 16 bytes from a2 if in BIOS range
                if (a2 >= 0xBFC00000 && a2 < 0xBFC80000) {
                    Console.WriteLn("@@LF_MEM@@ tag=a2 addr=%08x bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                        a2, memRead8(a2), memRead8(a2+1), memRead8(a2+2), memRead8(a2+3),
                        memRead8(a2+4), memRead8(a2+5), memRead8(a2+6), memRead8(a2+7),
                        memRead8(a2+8), memRead8(a2+9), memRead8(a2+10), memRead8(a2+11),
                        memRead8(a2+12), memRead8(a2+13), memRead8(a2+14), memRead8(a2+15));
                }
            }
        }
    }

    // One-shot: verify compare behavior at expected TBIN ROMDIR entry.
    {
        static bool s_tbin_cmp_window_logged = false;
        if (!s_tbin_cmp_window_logged)
        {
            const u32 t0 = cpuRegs.GPR.n.t0.UL[0];
            const bool in_tbin_window = (t0 >= 0xBFC4DF20 && t0 <= 0xBFC4DF80);
            if (in_tbin_window &&
                (cpuRegs.pc == 0xBFC02664 || cpuRegs.pc == 0xBFC026B8 || cpuRegs.pc == 0xBFC026C0 || cpuRegs.pc == 0xBFC026D0))
            {
                s_tbin_cmp_window_logged = true;
                const u32 a2 = cpuRegs.GPR.n.a2.UL[0];
                Console.WriteLn("@@TBIN_CMP_WIN@@ pc=%08x t0=%08x t1=%08x t2=%08x v0=%08x v1=%08x t3=%08x a2=%08x",
                    cpuRegs.pc, t0, cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.t2.UL[0],
                    cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0], cpuRegs.GPR.n.t3.UL[0], a2);
                Console.WriteLn("@@TBIN_CMP_MEM@@ t0=%08x w0=%08x w1=%08x a2=%08x a2w0=%08x a2w1=%08x",
                    t0, memRead32(t0), memRead32(t0 + 4), a2, memRead32(a2), memRead32(a2 + 4));
            }
        }
    }

    // Focused trace: first compare sequence around RESET entry window.
    {
        static u32 s_reset_cmp_trace_count = 0;
        if (s_reset_cmp_trace_count < 24)
        {
            const u32 t0 = cpuRegs.GPR.n.t0.UL[0];
            const bool in_reset_window = (t0 >= 0xBFC026E0 && t0 <= 0xBFC02840);
            const bool watch_pc = (cpuRegs.pc == 0xBFC02694 || cpuRegs.pc == 0xBFC026A4 ||
                                   cpuRegs.pc == 0xBFC026C0 || cpuRegs.pc == 0xBFC026E8);
            if (in_reset_window && watch_pc)
            {
                s_reset_cmp_trace_count++;
                const u32 t1 = cpuRegs.GPR.n.t1.UL[0];
                const u32 t2 = cpuRegs.GPR.n.t2.UL[0];
                const u32 v0 = cpuRegs.GPR.n.v0.UL[0];
                const u32 v1 = cpuRegs.GPR.n.v1.UL[0];
                Console.WriteLn("@@RESET_CMP_TRACE@@ n=%u pc=%08x t0=%08x t1=%08x t2=%08x v0=%08x v1=%08x w_t0=%08x w_t1m8=%08x h_t1m4=%04x w_t1=%08x",
                    s_reset_cmp_trace_count - 1, cpuRegs.pc, t0, t1, t2, v0, v1,
                    memRead32(t0), memRead32(t1 - 8), memRead16(t1 - 4), memRead32(t1));
            }
        }
    }

    // First-hit trace: raw stream at LoadFile compare head (pc=0xBFC02664).
    {
        static u32 s_lf_2664_trace = 0;
        if (s_lf_2664_trace < 48 && cpuRegs.pc == 0xBFC02664)
        {
            s_lf_2664_trace++;
            const u32 t0 = cpuRegs.GPR.n.t0.UL[0];
            const u32 t1 = cpuRegs.GPR.n.t1.UL[0];
            const u32 t2 = cpuRegs.GPR.n.t2.UL[0];
            Console.WriteLn("@@LF2664_TRACE@@ n=%u t0=%08x t1=%08x t2=%08x w0=%08x w4=%08x h8=%04x w12=%08x",
                s_lf_2664_trace - 1, t0, t1, t2, memRead32(t0), memRead32(t0 + 4), memRead16(t0 + 8), memRead32(t0 + 12));
        }
    }

    // One-shot disasm around TBIN decision region.
    {
        static bool s_bfc02714_disasm_dumped = false;
        if (!s_bfc02714_disasm_dumped && cpuRegs.pc == 0xBFC02714)
        {
            s_bfc02714_disasm_dumped = true;
            Console.WriteLn("@@BFC02714_DISASM_START@@ window=bfc02700..bfc02734");
            for (u32 addr = 0xBFC02700; addr <= 0xBFC02734; addr += 4)
            {
                const u32 op = memRead32(addr);
                const u32 opc = (op >> 26) & 0x3F;
                const s16 imm = static_cast<s16>(op & 0xFFFF);
                if (opc == 0x04 || opc == 0x05 || opc == 0x06 || opc == 0x07)
                {
                    const u32 target = addr + 4 + (static_cast<s32>(imm) << 2);
                    Console.WriteLn("@@BFC02714_DISASM@@ %08x: %08x op=%02x imm=%d target=%08x",
                        addr, op, opc, static_cast<s32>(imm), target);
                }
                else
                {
                    Console.WriteLn("@@BFC02714_DISASM@@ %08x: %08x op=%02x", addr, op, opc);
                }
            }
            Console.WriteLn("@@BFC02714_DISASM_END@@");
        }
    }
    
    // [TEMP_DIAG][REMOVE_AFTER=LF_BNE_RING_V1] Dump LoadFile BNE ring buffer (on POST loop)
#ifndef iPSX2_ENABLE_BRANCH_RUNTIME_PROBES
#define iPSX2_ENABLE_BRANCH_RUNTIME_PROBES 0
#endif
#if iPSX2_ENABLE_BRANCH_RUNTIME_PROBES
    if (cpuRegs.pc == 0xBFC02454) {
        static bool s_bne_ring_dumped = false;
        if (!s_bne_ring_dumped) {
            s_bne_ring_dumped = true;
            void LfBneRingDump();
            LfBneRingDump();
        }
    }
#endif
	}

    // ---- Schedule Next Event Test --------------
    // [P33] Android と同じ固定 8x 比を使用。R3000A.cpp の PSX_INT() と統一。
    const s32 nextIopEventDeta = (psxRegs.iopNextEventCycle - psxRegs.cycle) << 3;

    // 8 or more cycles behind and there's an event scheduled
    if (EEsCycle >= nextIopEventDeta)
    {
        // EE's running way ahead of the IOP still, so we should branch quickly to give the
        // IOP extra timeslices in short order.

        cpuSetNextEventDelta(48);
        //Console.Warning( "EE ahead of the IOP -- Rapid Event!  %d", EEsCycle );
    }
    else
    {
        // Otherwise IOP is caught up/not doing anything so we can wait for the next event.
        cpuSetNextEventDelta(nextIopEventDeta - EEsCycle);
    }

	// Apply vsync and other counter nextCycles
	cpuSetNextEvent(nextStartCounter, nextDeltaCounter);

	eeEventTestIsActive = false;
}

__ri void cpuTestINTCInts()
{
	// Check the COP0's Status register for general interrupt disables, and the 0x400
	// bit (which is INTC master toggle).
	if (!cpuIntsEnabled(0x400))
		return;

	if ((psHu32(INTC_STAT) & psHu32(INTC_MASK)) == 0)
		return;

	cpuSetNextEventDelta(4);
	if (eeEventTestIsActive && (psxRegs.iopCycleEE > 0))
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}
}

__fi void cpuTestDMACInts()
{
	// Check the COP0's Status register for general interrupt disables, and the 0x800
	// bit (which is the DMAC master toggle).
	if (!cpuIntsEnabled(0x800))
		return;

	if (((psHu16(0xe012) & psHu16(0xe010)) == 0) &&
		((psHu16(0xe010) & 0x8000) == 0))
		return;

	cpuSetNextEventDelta(4);
	if (eeEventTestIsActive && (psxRegs.iopCycleEE > 0))
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}
}

__fi void cpuTestTIMRInts()
{
	if ((cpuRegs.CP0.n.Status.val & 0x10007) == 0x10001)
	{
		_cpuTestPERF();
		_cpuTestTIMR();
	}
}

__fi void cpuTestHwInts()
{
	cpuTestINTCInts();
	cpuTestDMACInts();
	cpuTestTIMRInts();
}

__fi void CPU_SET_DMASTALL(EE_EventType n, bool set)
{
	if (set)
		cpuRegs.dmastall |= 1 << n;
	else
		cpuRegs.dmastall &= ~(1 << n);
}

__fi void CPU_INT( EE_EventType n, s32 ecycle)
{
	// If it's retunning too quick, just rerun the DMA, there's no point in running the EE for < 4 cycles.
	// This causes a huge uplift in performance for ONI FMV's.
	if (ecycle < 4 && !(cpuRegs.dmastall & (1 << n)) && eeRunInterruptScan != INT_NOT_RUNNING)
	{
		eeRunInterruptScan = INT_REQ_LOOP;
		cpuRegs.interrupt |= 1 << n;
		cpuRegs.sCycle[n] = cpuRegs.cycle;
		cpuRegs.eCycle[n] = 0;
		return;
	}

	// EE events happen 8 cycles in the future instead of whatever was requested.
	// This can be used on games with PATH3 masking issues for example, or when
	// some FMV look bad.
	if (CHECK_EETIMINGHACK && n < VIF_VU0_FINISH)
		ecycle = 8;

	cpuRegs.interrupt |= 1 << n;
	cpuRegs.sCycle[n] = cpuRegs.cycle;
	cpuRegs.eCycle[n] = ecycle;

	// Interrupt is happening soon: make sure both EE and IOP are aware.

	if (ecycle <= 28 && psxRegs.iopCycleEE > 0)
	{
		// If running in the IOP, force it to break immediately into the EE.
		// the EE's branch test is due to run.

		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}

	cpuSetNextEventDelta(cpuRegs.eCycle[n]);
}

// Count arguments, save their starting locations, and replace the space separators with null terminators so they're separate strings
int ParseArgumentString(u32 arg_block)
{
	if (!arg_block)
		return 0;

	int argc = 0;
	bool wasSpace = true; // status of last char. scanned
	int args_len = strlen((char *)PSM(arg_block));
	for (int i = 0; i < args_len; i++)
	{
		char curchar = *(char *)PSM(arg_block + i);
		if (curchar == '\0')
			break; // should never reach this

		bool isSpace = (curchar == ' ');
		if (isSpace)
			memset(PSM(arg_block + i), 0, 1);
		else if (wasSpace) // then we're at a new arg
		{
			if (argc < kMaxArgs)
			{
				g_argPtrs[argc] = arg_block + i;
				argc++;
			}
			else
			{
				Console.WriteLn("ParseArgumentString: Discarded additional arguments beyond the maximum of %d.", kMaxArgs);
				break;
			}
		}
		wasSpace = isSpace;
	}
#if DEBUG_LAUNCHARG
	// Check our args block
	Console.WriteLn("ParseArgumentString: Saving these strings:");
	for (int a = 0; a < argc; a++)
		Console.WriteLn("%p -> '%s'.", g_argPtrs[a], (char *)PSM(g_argPtrs[a]));
#endif
	return argc;
}

// Called from recompilers; define is mandatory.
void eeloadHook()
{
	std::string elfname;
	int argc = cpuRegs.GPR.n.a0.SD[0];
	if (argc) // calls to EELOAD *after* the first one during the startup process will come here
	{
#if DEBUG_LAUNCHARG
		Console.WriteLn("eeloadHook: EELOAD was called with %d arguments according to $a0 and %d according to vargs block:",
			argc, memRead32(cpuRegs.GPR.n.a1.UD[0] - 4));
		for (int a = 0; a < argc; a++)
			Console.WriteLn("argv[%d]: %p -> %p -> '%s'", a, cpuRegs.GPR.n.a1.UL[0] + (a * 4),
				memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4)), (char *)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4))));
#endif
		if (argc > 1)
			elfname = (char*)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + 4)); // argv[1] in OSDSYS's invocation "EELOAD <game ELF>"

		// [P29] iPSX2_ELF_OSDSYS_OVERRIDE: NoDisc ELFboot時の二重EELOADコール対策。
		// NoDiscmodeでは1回目(argc=0)にeeloadHookがELF名を書き込んだ後、BIOSカーネルが
		// 2回目のEELOADcall(argc>1, elfname='rom0:OSDSYS')を発行しBIOSbrowserがdisplayされる。
		// このフラグがONかつelf_overrideconfig済みの場合のみ、argv[1]='rom0:OSDSYS'を
		// host:ELFパスでoverwriteし、自然なBIOSbootシーケンスを迂回せずにELFを直接bootする。
		// デフォルトOFF — 通常BIOSbootシーケンスおよびFF10等のゲームに影響しない。
		// Removal condition: 正式なNoDisc ELF直接bootパスがimplされた後。
		static const bool s_elf_osdsys_override = []() {
			const char* v = getenv("iPSX2_ELF_OSDSYS_OVERRIDE");
			return v && atoi(v) != 0;
		}();
		if (s_elf_osdsys_override && elfname == "rom0:OSDSYS")
		{
			const std::string& elf_override = VMManager::Internal::GetELFOverride();
			if (!elf_override.empty())
			{
				const std::string new_elfname = fmt::format("host:{}", elf_override);
				// argv[1]のPS2memory上の文字列を直接overwriteしてEELOADの実行対象を差し替える
				u32 argv1_ptr = memRead32(cpuRegs.GPR.n.a1.UD[0] + 4);
				strcpy((char*)PSM(argv1_ptr), new_elfname.c_str());
				elfname = new_elfname;
				Console.WriteLn("@@OSDSYS_OVERRIDE@@ Redirected 'rom0:OSDSYS' -> '%s' (argv1_ptr=%08x)",
					new_elfname.c_str(), argv1_ptr);
			}
		}

		// This code fires if the user chooses "full boot". First the Sony Computer Entertainment screen appears. This is the result
		// of an EELOAD call that does not want to accept launch arguments (but we patch it to do so in eeloadHook2() in fast boot
		// mode). Then EELOAD is called with the argument "rom0:PS2LOGO". At this point, we do not need any additional tricks
		// because EELOAD is now ready to accept launch arguments. So in full-boot mode, we simply wait for PS2LOGO to be called,
		// then we add the desired launch arguments. PS2LOGO passes those on to the game itself as it calls EELOAD a third time.
		if (!EmuConfig.CurrentGameArgs.empty() && elfname == "rom0:PS2LOGO")
		{
			const char *argString = EmuConfig.CurrentGameArgs.c_str();
			Console.WriteLn("eeloadHook: Supplying launch argument(s) '%s' to module '%s'...", argString, elfname.c_str());

			// Join all arguments by space characters so they can be processed as one string by ParseArgumentString(), then add the
			// user's launch arguments onto the end
			u32 arg_ptr = 0;
			int arg_len = 0;
			for (int a = 0; a < argc; a++)
			{
				arg_ptr = memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4));
				arg_len = strlen((char *)PSM(arg_ptr));
				memset(PSM(arg_ptr + arg_len), 0x20, 1);
			}
			strcpy((char *)PSM(arg_ptr + arg_len + 1), EmuConfig.CurrentGameArgs.c_str());
			u32 first_arg_ptr = memRead32(cpuRegs.GPR.n.a1.UD[0]);
#if DEBUG_LAUNCHARG
			Console.WriteLn("eeloadHook: arg block is '%s'.", (char *)PSM(first_arg_ptr));
#endif
			argc = ParseArgumentString(first_arg_ptr);

			// Write pointer to next slot in $a1
			for (int a = 0; a < argc; a++)
				memWrite32(cpuRegs.GPR.n.a1.UD[0] + (a * 4), g_argPtrs[a]);
			cpuRegs.GPR.n.a0.SD[0] = argc;
#if DEBUG_LAUNCHARG
			// Check our work
			Console.WriteLn("eeloadHook: New arguments are:");
			for (int a = 0; a < argc; a++)
				Console.WriteLn("argv[%d]: %p -> '%s'", a, memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4)),
				(char *)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4))));
#endif
		}
		// else it's presumed that the invocation is "EELOAD <game ELF> <<launch args>>", coming from PS2LOGO, and we needn't do
		// anything more
	}
#if DEBUG_LAUNCHARG
	// This code fires in full/fast boot mode when EELOAD is called the first/only time. When EELOAD is not given any arguments,
	// it calls rom0:OSDSYS by default, which displays the Sony Computer Entertainment screen. OSDSYS then calls "EELOAD
	// rom0:PS2LOGO" and we end up above.
	else
		Console.WriteLn("eeloadHook: EELOAD was called with no arguments.");
#endif

	// If "fast boot" was chosen, then on EELOAD's first call we won't yet know what the game's ELF is. Find the name and write it
	// into EELOAD's memory.
	if (VMManager::Internal::IsFastBootInProgress() && elfname.empty())
	{
		const std::string& elf_override = VMManager::Internal::GetELFOverride();
		if (!elf_override.empty())
		{
			elfname = fmt::format("host:{}", elf_override);
		}
		else
		{
			CDVDDiscType disc_type;
			std::string disc_elf;
			cdvdGetDiscInfo(nullptr, &disc_elf, nullptr, nullptr, &disc_type);
			if (disc_type == CDVDDiscType::PS2Disc)
			{
				// only allow fast boot for PS2 games
				elfname = std::move(disc_elf);
			}
			else
			{
				Console.Warning(fmt::format("Not allowing fast boot for non-PS2 ELF {}", disc_elf));
			}
		}

		// When fast-booting, we insert the game's ELF name into EELOAD so that the game is called instead of the default call of
		// "rom0:OSDSYS"; any launch arguments supplied by the user will be inserted into EELOAD later by eeloadHook2()
		if (!elfname.empty())
		{
			// Find and save location of default/fallback call "rom0:OSDSYS"; to be used later by eeloadHook2()
			int osdsys_found = 0;
			for (g_osdsys_str = EELOAD_START; g_osdsys_str < EELOAD_START + EELOAD_SIZE; g_osdsys_str += 8) // strings are 64-bit aligned
			{
				if (!strcmp((char*)PSM(g_osdsys_str), "rom0:OSDSYS"))
				{
					// Overwrite OSDSYS with game's ELF name
					strcpy((char*)PSM(g_osdsys_str), elfname.c_str());
					osdsys_found++;
				}
			}
			Console.WriteLn("@@P33_OSDSYS_OVERWRITE@@ elfname='%s' osdsys_found=%d EELOAD=%08x-%08x",
				elfname.c_str(), osdsys_found, EELOAD_START, EELOAD_START + EELOAD_SIZE);
		}
		else
		{
			// Stop fast forwarding if we're doing that for boot.
			VMManager::Internal::DisableFastBoot();
			AllowParams1 = true;
			AllowParams2 = true;
		}
	}

	// [P15] @@P15_MEM_TIMELINE@@ — exception vector + game code presence at EELOAD hook time
	// Removal condition: exception vector / game code zeros issue解消後
	{
		static int s_mem_tl_n = 0;
		if (s_mem_tl_n++ < 3 && eeMem) {
			Console.WriteLn("@@P15_MEM_TIMELINE@@ stage=EELOAD_HOOK n=%d pc=%08x elfname='%s' fastboot=%d",
				s_mem_tl_n, cpuRegs.pc, elfname.c_str(),
				(int)VMManager::Internal::IsFastBootInProgress());
			Console.WriteLn("@@P15_MEM_TIMELINE@@ excvec[80]=%08x [84]=%08x [180]=%08x [184]=%08x [188]=%08x [18C]=%08x",
				*(u32*)(eeMem->Main + 0x80), *(u32*)(eeMem->Main + 0x84),
				*(u32*)(eeMem->Main + 0x180), *(u32*)(eeMem->Main + 0x184),
				*(u32*)(eeMem->Main + 0x188), *(u32*)(eeMem->Main + 0x18C));
			Console.WriteLn("@@P15_MEM_TIMELINE@@ mem[26FDE0]=%08x %08x %08x %08x [26FDF0]=%08x %08x %08x %08x",
				*(u32*)(eeMem->Main + 0x26FDE0), *(u32*)(eeMem->Main + 0x26FDE4),
				*(u32*)(eeMem->Main + 0x26FDE8), *(u32*)(eeMem->Main + 0x26FDEC),
				*(u32*)(eeMem->Main + 0x26FDF0), *(u32*)(eeMem->Main + 0x26FDF4),
				*(u32*)(eeMem->Main + 0x26FDF8), *(u32*)(eeMem->Main + 0x26FDFC));
			Console.WriteLn("@@P15_MEM_TIMELINE@@ mem[270000]=%08x %08x %08x %08x %08x %08x %08x %08x",
				*(u32*)(eeMem->Main + 0x270000), *(u32*)(eeMem->Main + 0x270004),
				*(u32*)(eeMem->Main + 0x270008), *(u32*)(eeMem->Main + 0x27000C),
				*(u32*)(eeMem->Main + 0x270010), *(u32*)(eeMem->Main + 0x270014),
				*(u32*)(eeMem->Main + 0x270018), *(u32*)(eeMem->Main + 0x27001C));
		}
	}

	// [P15] @@ELF_PRELOAD@@ — fast_boot 時にゲーム ELF PT_LOAD セグメントを EE RAM に直接プリロード。
	// EELOAD が CDVD 経由でロードする際に IOP→EE SIF DMA で ~40% のデータが欠落するissueを回避。
	// Removal condition: IOP→EE SIF DMA transferのデータ欠落root causeがfixされた後
	{
		static bool s_preload_done = false;
		if (!s_preload_done && VMManager::Internal::IsFastBootInProgress() &&
			!elfname.empty() && eeMem)
		{
			s_preload_done = true;
			ElfObject elfo;
			Error error;
			if (cdvdLoadElf(&elfo, elfname, false, &error)) {
				const u8* elfdata = elfo.GetData().data();
				const size_t elfsize = elfo.GetData().size();
				const ELF_HEADER* ehdr = reinterpret_cast<const ELF_HEADER*>(elfdata);
				u32 segs_loaded = 0, bytes_loaded = 0;
				if (ehdr->e_phoff + ehdr->e_phnum * ehdr->e_phentsize <= elfsize) {
					for (int i = 0; i < ehdr->e_phnum; i++) {
						const ELF_PHR* ph = reinterpret_cast<const ELF_PHR*>(
							elfdata + ehdr->e_phoff + i * ehdr->e_phentsize);
						if (ph->p_type != 1) continue;
						const u32 dst = ph->p_vaddr & 0x1FFFFFFFu;
						if (dst + ph->p_memsz > Ps2MemSize::MainRam) continue;
						if (ph->p_filesz > 0 && ph->p_offset + ph->p_filesz <= elfsize) {
							std::memcpy(eeMem->Main + dst, elfdata + ph->p_offset, ph->p_filesz);
							bytes_loaded += ph->p_filesz;
						}
						if (ph->p_memsz > ph->p_filesz)
							std::memset(eeMem->Main + dst + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
						segs_loaded++;
					}
				}
				Console.WriteLn("@@ELF_PRELOAD@@ loaded %u segments, %u bytes from '%s'",
					segs_loaded, bytes_loaded, elfname.c_str());
				s_elf_preload_path = elfname;
				g_armsx2_elf_postload_fn = &iPSX2_ELF_Postload_Impl;
			} else {
				Console.WriteLn("@@ELF_PRELOAD@@ FAILED: %s", error.GetDescription().c_str());
			}
		}
	}

	// [iter98] ELF Loading フラッド抑制: elfname が空の場合 (no disc = BIOS loop) は最大 3 回のみ記録
	// Removal condition: DMAC_STAT bit5 (SIF0) がconfigされて EELOAD loopが解消した時
	{
		static int s_elf_load_empty_n = 0;
		if (!elfname.empty() || s_elf_load_empty_n++ < 3)
			VMManager::Internal::ELFLoadingOnCPUThread(std::move(elfname));
	}

	// [iter91] @@EELOAD_HOOK_STATE@@ – g_eeloadExec / rom0:OSDSYS 存在verify (loop脱出condition調査)
	// Removal condition: OSDSYS ロードパスroot cause確定時
	{
		static int s_n = 0;
		if (s_n++ < 2) {
			// g_eeloadExec=0 → ExecPS2 の直接インターセプトdisabledをverify
			Console.WriteLn("@@EELOAD_HOOK_STATE@@ n=%d argc=%d g_main=%08x g_exec=%08x g_osdsys=%08x fastboot=%d",
				s_n, argc, g_eeloadMain, g_eeloadExec, g_osdsys_str,
				(int)VMManager::Internal::IsFastBootInProgress());
			// EELOAD バイナリ内 "rom0:OSDSYS" 文字列の所在を検索 (fast boot 外でもverify)
			u32 found_addr = 0;
			for (u32 a = EELOAD_START; a < EELOAD_START + 0x10000u && !found_addr; a += 8) {
				const char* p = (const char*)PSM(a);
				if (p && !strncmp(p, "rom0:OSDSYS", 11))
					found_addr = a;
			}
			Console.WriteLn("@@EELOAD_HOOK_STATE@@ rom0:OSDSYS_addr=%08x", found_addr);
		}
	}

	if (CHECK_EXTRAMEM)
	{
		// Map extra memory.
		vtlb_VMap(Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);

		// Map RAM mirrors for extra memory.
		vtlb_VMap(0x20000000 | Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);
		vtlb_VMap(0x30000000 | Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);
	}
}

// Called from recompilers; define is mandatory.
// Only called if g_SkipBiosHack is true
void eeloadHook2()
{
	if (EmuConfig.CurrentGameArgs.empty())
		return;

	if (!g_osdsys_str)
	{
		Console.WriteLn("eeloadHook2: Called before \"rom0:OSDSYS\" was found by eeloadHook()!");
		return;
	}

	const char *argString = EmuConfig.CurrentGameArgs.c_str();
	Console.WriteLn("eeloadHook2: Supplying launch argument(s) '%s' to ELF '%s'.", argString, (char *)PSM(g_osdsys_str));

	// Add args string after game's ELF name that was written over "rom0:OSDSYS" by eeloadHook(). In between the ELF name and args
	// string we insert a space character so that ParseArgumentString() has one continuous string to process.
	int game_len = strlen((char *)PSM(g_osdsys_str));
	memset(PSM(g_osdsys_str + game_len), 0x20, 1);
	strcpy((char *)PSM(g_osdsys_str + game_len + 1), EmuConfig.CurrentGameArgs.c_str());
#if DEBUG_LAUNCHARG
	Console.WriteLn("eeloadHook2: arg block is '%s'.", (char *)PSM(g_osdsys_str));
#endif
	int argc = ParseArgumentString(g_osdsys_str);

	// Back up 4 bytes from start of args block for every arg + 4 bytes for start of argv pointer block, write pointers
	uptr block_start = g_osdsys_str - (argc * 4);
	for (int a = 0; a < argc; a++)
	{
#if DEBUG_LAUNCHARG
		Console.WriteLn("eeloadHook2: Writing address %p to location %p.", g_argPtrs[a], block_start + (a * 4));
#endif
		memWrite32(block_start + (a * 4), g_argPtrs[a]);
	}

	// Save argc and argv as incoming arguments for EELOAD function which calls ExecPS2()
#if DEBUG_LAUNCHARG
	Console.WriteLn("eeloadHook2: Saving %d and %p in $a0 and $a1.", argc, block_start);
#endif
	cpuRegs.GPR.n.a0.SD[0] = argc;
	cpuRegs.GPR.n.a1.UD[0] = block_start;
}

inline bool isBranchOrJump(u32 addr)
{
	u32 op = memRead32(addr);
	const OPCODE& opcode = GetInstruction(op);

	// Return false for eret & syscall as they are branch type in pcsx2 debugging tools,
	// but shouldn't have delay slot in isBreakpointNeeded/isMemcheckNeeded.
	if ((opcode.flags == (IS_BRANCH | BRANCHTYPE_SYSCALL)) || (opcode.flags == (IS_BRANCH | BRANCHTYPE_ERET)))
		return false;

	return (opcode.flags & IS_BRANCH) != 0;
}

// The next two functions return 0 if no breakpoint is needed,
// 1 if it's needed on the current pc, 2 if it's needed in the delay slot
// 3 if needed in both

int isBreakpointNeeded(u32 addr)
{
	int bpFlags = 0;
	if (CBreakPoints::IsAddressBreakPoint(BREAKPOINT_EE, addr))
		bpFlags += 1;

	// there may be a breakpoint in the delay slot
	if (isBranchOrJump(addr) && CBreakPoints::IsAddressBreakPoint(BREAKPOINT_EE, addr+4))
		bpFlags += 2;

	return bpFlags;
}

int isMemcheckNeeded(u32 pc)
{
	if (CBreakPoints::GetNumMemchecks() == 0)
		return 0;

	u32 addr = pc;
	if (isBranchOrJump(addr))
		addr += 4;

	u32 op = memRead32(addr);
	const OPCODE& opcode = GetInstruction(op);

	if (opcode.flags & IS_MEMORY)
		return addr == pc ? 1 : 2;

	return 0;
}

extern "C" void Step2_CheckDump()
{
    // Need to access Console and Globals
    // Globals are extern in iR5900.h, which is included.
    
    // We need to handle the struct vs u32 issue.
    // g_step2_ring is declared as Step2FlightRecEntry[] in header.
    // We want to access it as u32[].
    
    volatile u32* ring_u32 = (volatile u32*)g_step2_ring;
    constexpr int K_RING_SIZE = 128;
    constexpr int K_ENTRY_U32_COUNT = 8;
    
    // Safety: ensure we only dump once
    static bool s_step2_dumped = false;
    u32 idx = g_step2_idx;
    
    if (idx >= 16 && !s_step2_dumped) {
        s_step2_dumped = true;
        Console.WriteLn("@@STEP2_FLIGHTREC_DUMP@@ count=%u", idx);
        
        int start = (idx > 64) ? (idx - 64) : 0;
        int end = idx;
        
        for (int i = start; i < end; i++) {
            int slot = i & (K_RING_SIZE - 1);
            int base = slot * K_ENTRY_U32_COUNT;
            
            u32 kind = ring_u32[base + 0];
            u32 guest_pc = ring_u32[base + 1];
            u32 addr = ring_u32[base + 2];
            u32 val = ring_u32[base + 3];
            
            if (kind == 1) {
                Console.WriteLn("@@FLIGHTREC@@ [%03d] READ  pc=%08x addr=%08x val=%08x", i, guest_pc, addr, val);
            } else if (kind == 2) {
                Console.WriteLn("@@FLIGHTREC@@ [%03d] BRANCH pc=%08x target=%08x taken=%d", i, guest_pc, addr, val);
            }
        }
    }
}
