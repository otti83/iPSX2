// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <cstdio>
#include <cstring>

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "CDVD/CDVD.h"

#include "Common.h"
#include "BiosTools.h"
#include "Config.h"
#include "Memory.h"
#include "vtlb.h"

extern "C" void LogUnified(const char* fmt, ...);

// [iter230] EELOAD late re-copy support
static u32 s_eeload_rom_offset = 0;
static u32 s_eeload_rom_size = 0;
static bool s_eeload_recopy_done = false;

static constexpr u32 MIN_BIOS_SIZE = 4 * _1mb;
static constexpr u32 MAX_BIOS_SIZE = 8 * _1mb;
static constexpr u32 DIRENTRY_SIZE = 16;

// --------------------------------------------------------------------------------------
// romdir structure (packing required!)
// --------------------------------------------------------------------------------------
//
#pragma pack(push, 1)

struct romdir
{
	char fileName[10];
	u16 extInfoSize;
	u32 fileSize;
};

#pragma pack(pop)

static_assert(sizeof(romdir) == DIRENTRY_SIZE, "romdir struct not packed to 16 bytes");

u32 BiosVersion;
u32 BiosChecksum;
u32 BiosRegion;
ConfigParam configParams1;
Config2Param configParams2;
bool ParamsRead;
bool NoOSD;
bool AllowParams1;
bool AllowParams2;
std::string BiosDescription;
std::string BiosZone;
std::string BiosSerial;
std::string BiosPath;
BiosDebugInformation CurrentBiosInformation;
std::vector<u8> BiosRom;

void ReadOSDConfigParames()
{
	if (ParamsRead)
		return;

	ParamsRead = true;

	u8 params[16];
	cdvdReadLanguageParams(params);

	configParams1.UC[0] = params[1] & 0x1F; // SPDIF, Screen mode, RGB/Comp, Jap/Eng Switch (Early bios).
	configParams1.ps1drvConfig = params[0]; // PS1 Mode Settings.
	configParams1.version = (params[2] & 0xE0) >> 5; // OSD Ver (Not sure but best guess).
	configParams1.language = params[2] & 0x1F; // Language.
	configParams1.timezoneOffset = params[4] | ((u32)(params[3] & 0x7) << 8);  // Timezone offset in minutes.

	// Region settings for time/date and extended language
	configParams2.UC[1] = ((u32)params[3] & 0x78) << 1; // Daylight Savings, 24hr clock, Date format
	// FIXME: format, version and language are set manually by the bios. Not sure if any game needs them, but it seems to set version to 2 and duplicate the language value.
	configParams2.version = 2;
	configParams2.language = configParams1.language;
}

static bool LoadBiosVersion(std::FILE* fp, u32& version, std::string& description, u32& region, std::string& zone, std::string& serial)
{
	romdir rd;
	for (u32 i = 0; i < 512 * 1024; i++)
	{
		if (std::fread(&rd, sizeof(rd), 1, fp) != 1)
			return false;

		if (std::strncmp(rd.fileName, "RESET", sizeof(rd.fileName)) == 0)
			break; /* found romdir */
	}

	s64 fileOffset = 0;
	s64 fileSize = FileSystem::FSize64(fp);
	bool foundRomVer = false;
	char romver[14 + 1] = {}; // ascii version loaded from disk.
	char extinfo[15 + 1] = {}; // ascii version loaded from disk.

	// ensure it's a null-terminated and not zero-length string
	while (rd.fileName[0] != '\0' && strnlen(rd.fileName, sizeof(rd.fileName)) != sizeof(rd.fileName))
	{
		if (std::strncmp(rd.fileName, "EXTINFO", sizeof(rd.fileName)) == 0)
		{
			s64 pos = FileSystem::FTell64(fp);
			if (FileSystem::FSeek64(fp, fileOffset + 0x10, SEEK_SET) != 0 ||
				std::fread(extinfo, 15, 1, fp) != 1 || FileSystem::FSeek64(fp, pos, SEEK_SET) != 0)
			{
				break;
			}
			serial = extinfo;
		}

		if (std::strncmp(rd.fileName, "ROMVER", sizeof(rd.fileName)) == 0)
		{

			s64 pos = FileSystem::FTell64(fp);
			if (FileSystem::FSeek64(fp, fileOffset, SEEK_SET) != 0 ||
				std::fread(romver, 14, 1, fp) != 1 || FileSystem::FSeek64(fp, pos, SEEK_SET) != 0)
			{
				break;
			}

			foundRomVer = true;
		}

		if ((rd.fileSize % 0x10) == 0)
			fileOffset += rd.fileSize;
		else
			fileOffset += (rd.fileSize + 0x10) & 0xfffffff0;

		if (std::fread(&rd, sizeof(rd), 1, fp) != 1)
			break;
	}

	fileOffset -= ((rd.fileSize + 0x10) & 0xfffffff0) - rd.fileSize;

	if (foundRomVer)
	{
		switch (romver[4])
		{
			// clang-format off
			case 'J': zone = "Japan";  region = 0;  break;
			case 'A': zone = "USA";    region = 1;  break;
			case 'E': zone = "Europe"; region = 2;  break;
			// case 'E': zone = "Oceania";region = 3;  break; // Not implemented
			case 'H': zone = "Asia";   region = 4;  break;
			// case 'E': zone = "Russia"; region = 3;  break; // Not implemented
			case 'C': zone = "China";  region = 6;  break;
			// case 'A': zone = "Mexico"; region = 7;  break; // Not implemented
			case 'T': zone = (romver[5]=='Z') ? "COH-H" : "T10K";   region = 8;  break;
			case 'X': zone = "Test";   region = 9;  break;
			case 'P': zone = "Free";   region = 10; break;
			// clang-format on
			default:
				zone.clear();
				zone += romver[4];
				region = 0;
				break;
		}
		// TODO: some regions can be detected only from rom1
		/* switch (rom1:DVDID[4])
		{
			// clang-format off
			case 'O': zone = "Oceania";region = 3;  break;
			case 'R': zone = "Russia"; region = 5;  break;
			case 'M': zone = "Mexico"; region = 7;  break;
			// clang-format on
		} */

		char vermaj[3] = {romver[0], romver[1], 0};
		char vermin[3] = {romver[2], romver[3], 0};
		description = StringUtil::StdStringFromFormat("%-7s v%s.%s(%c%c/%c%c/%c%c%c%c)  %s %s",
			zone.c_str(),
			vermaj, vermin,
			romver[12], romver[13], // day
			romver[10], romver[11], // month
			romver[6], romver[7], romver[8], romver[9], // year!
			(romver[5] == 'C') ? "Console" : (romver[5] == 'D') ? "Devel" :
																  "",
			serial.c_str());

		version = static_cast<u32>(strtol(vermaj, (char**)NULL, 0) << 8);
		version |= strtol(vermin, (char**)NULL, 0);

		Console.WriteLn("BIOS Found: %s", description.c_str());
	}
	else
		return false;

	if (fileSize < (int)fileOffset)
	{
		description += StringUtil::StdStringFromFormat(" %d%%", (((int)fileSize * 100) / (int)fileOffset));
		// we force users to have correct bioses,
		// not that lame scph10000 of 513KB ;-)
	}

	return true;
}

static void ChecksumIt(u32& result, u32 offset, u32 size)
{
	const u8* srcdata = &BiosRom[offset];
	pxAssume((size & 3) == 0);
	for (size_t i = 0; i < size / 4; ++i)
		result ^= reinterpret_cast<const u32*>(srcdata)[i];
}

// Attempts to load a BIOS rom sub-component, by trying multiple combinations of base
// filename and extension.  The bios specified in the user's configuration is used as
// the base.
//
// Parameters:
//   ext - extension of the sub-component to load. Valid options are rom1 and rom2.
//
static void LoadExtraRom(const char* ext, u32 offset, u32 size)
{
	// Try first a basic extension concatenation (normally results in something like name.bin.rom1)
	std::string Bios1(StringUtil::StdStringFromFormat("%s.%s", BiosPath.c_str(), ext));

	s64 filesize;
	if ((filesize = FileSystem::GetPathFileSize(Bios1.c_str())) <= 0)
	{
		// Try the name properly extensioned next (name.rom1)
		Bios1 = Path::ReplaceExtension(BiosPath, ext);
		if ((filesize = FileSystem::GetPathFileSize(Bios1.c_str())) <= 0)
		{
			Console.WriteLn(Color_Gray, "BIOS %s module not found, skipping...", ext);
			return;
		}
	}

	BiosRom.resize(offset + size);

	auto fp = FileSystem::OpenManagedCFileTryIgnoreCase(Bios1.c_str(), "rb");
	if (!fp || std::fread(&BiosRom[offset], static_cast<size_t>(std::min<s64>(size, filesize)), 1, fp.get()) != 1)
	{
		Console.Warning("BIOS Warning: %s could not be read (permission denied?)", ext);
		return;
	}
	// Checksum for ROM1, ROM2?  Rama says no, Gigaherz says yes.  I'm not sure either way.  --air
	//ChecksumIt( BiosChecksum, dest );
}

static void LoadIrx(const std::string& filename, u8* dest, size_t maxSize)
{
	auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "rb");
	if (fp)
	{
		const s64 filesize = FileSystem::FSize64(fp.get());
		const s64 readSize = std::min(filesize, static_cast<s64>(maxSize));
		if (std::fread(dest, readSize, 1, fp.get()) == 1)
			return;
	}

	Console.Warning("IRX Warning: %s could not be read", filename.c_str());
	return;
}

static std::string FindBiosImage()
{
	Console.WriteLn("Searching for a BIOS image in '%s'...", EmuFolders::Bios.c_str());

	FileSystem::FindResultsArray results;
	if (!FileSystem::FindFiles(EmuFolders::Bios.c_str(), "*", FILESYSTEM_FIND_FILES, &results))
		return std::string();

	u32 version, region;
	std::string description, zone;
	for (const FILESYSTEM_FIND_DATA& fd : results)
	{
		if (fd.Size < MIN_BIOS_SIZE || fd.Size > MAX_BIOS_SIZE)
			continue;

		if (IsBIOS(fd.FileName.c_str(), version, description, region, zone))
		{
			Console.WriteLn("Using BIOS '%s' (%s %s)", fd.FileName.c_str(), description.c_str(), zone.c_str());
			return std::move(fd.FileName);
		}
	}

	Console.Error("Unable to auto locate a BIOS image");
	return std::string();
}

bool IsBIOS(const char* filename, u32& version, std::string& description, u32& region, std::string& zone)
{
	std::string serial;
	const auto fp = FileSystem::OpenManagedCFile(filename, "rb");
	if (!fp)
		return false;

	// FPS2BIOS is smaller and of variable size
	//if (inway.Length() < 512*1024) return false;
	return LoadBiosVersion(fp.get(), version, description, region, zone, serial);
}

bool IsBIOSAvailable(const std::string& full_path)
{
	// We can't use EmuConfig here since it may not be loaded yet.
	if (!full_path.empty() && FileSystem::FileExists(full_path.c_str()))
		return true;

	// No bios configured or the configured name is missing, check for one in the BIOS directory.
	const std::string auto_path(FindBiosImage());
	return !auto_path.empty() && FileSystem::FileExists(auto_path.c_str());
}

// Loads the configured bios rom file into PS2 memory.  PS2 memory must be allocated prior to
// this method being called.
//
// Remarks:
//   This function does not fail if rom1 or rom2 files are missing, since none are
//   explicitly required for most emulation tasks.
//
// Exceptions:
//   BadStream - Thrown if the primary bios file (usually .bin) is not found, corrupted, etc.
//
bool LoadBIOS()
{
	pxAssertMsg(eeMem->ROM, "PS2 system memory has not been initialized yet.");

	std::string path = EmuConfig.FullpathToBios();
	if (path.empty() || !FileSystem::FileExists(path.c_str()))
	{
		if (!path.empty())
		{
			Console.Warning("Configured BIOS '%s' does not exist, trying to find an alternative.",
				EmuConfig.BaseFilenames.Bios.c_str());
		}

		path = FindBiosImage();
		if (path.empty())
			return false;
	}

	auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
	if (!fp)
		return false;

	const s64 filesize = FileSystem::FSize64(fp.get());
	if (filesize <= 0)
		return false;

	LoadBiosVersion(fp.get(), BiosVersion, BiosDescription, BiosRegion, BiosZone, BiosSerial);

	BiosRom.resize(Ps2MemSize::Rom);

	if (FileSystem::FSeek64(fp.get(), 0, SEEK_SET) ||
		std::fread(BiosRom.data(), static_cast<size_t>(std::min<s64>(Ps2MemSize::Rom, filesize)), 1, fp.get()) != 1)
	{
		return false;
	}

	// If file is less than 2mb it doesn't have an OSD (Devel consoles)
	// So skip HLEing OSDSys Param stuff
	if (filesize < 2465792)
		NoOSD = true;
	else
		NoOSD = false;

	BiosChecksum = 0;
	ChecksumIt(BiosChecksum, 0, Ps2MemSize::Rom);
	BiosPath = std::move(path);

	//injectIRX("host.irx");	//not fully tested; still buggy

	LoadExtraRom("rom1", Ps2MemSize::Rom, Ps2MemSize::Rom1);
	LoadExtraRom("rom2", Ps2MemSize::Rom + Ps2MemSize::Rom1, Ps2MemSize::Rom2);
	return true;
}

void CopyBIOSToMemory()
{
	if (BiosRom.size() >= Ps2MemSize::Rom)
	{
		std::memcpy(eeMem->ROM, BiosRom.data(), sizeof(eeMem->ROM));

		if (BiosRom.size() >= (Ps2MemSize::Rom + Ps2MemSize::Rom1))
		{
			std::memcpy(eeMem->ROM1, BiosRom.data() + Ps2MemSize::Rom, sizeof(eeMem->ROM1));
			if (BiosRom.size() >= (Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2))
				std::memcpy(eeMem->ROM2, BiosRom.data() + Ps2MemSize::Rom + Ps2MemSize::Rom1, sizeof(eeMem->ROM2));
		}
	}

	// ROM1 fallback: if ROM1 isn't populated, mirror a ROM0 window so 0x1fc80000 reads return real data.
	bool rom1_empty = (BiosRom.size() < (Ps2MemSize::Rom + Ps2MemSize::Rom1));
	if (!rom1_empty)
	{
		size_t nonzero = 0;
		const size_t probe = 256;
		for (size_t i = 0; i < probe; i++)
		{
			if (eeMem->ROM1[i] != 0)
			{
				nonzero = 1;
				break;
			}
		}
		rom1_empty = (nonzero == 0);
	}

	if (rom1_empty)
	{
		std::memcpy(eeMem->ROM1, eeMem->ROM, sizeof(eeMem->ROM1));
		LogUnified("@@ROM1_FALLBACK@@ mode=COPY reason=EMPTY\n");
	}

	if (EmuConfig.CurrentIRX.length() > 3)
		LoadIrx(EmuConfig.CurrentIRX, &eeMem->ROM[0x3C0000], sizeof(eeMem->ROM) - 0x3C0000);

	CurrentBiosInformation.eeThreadListAddr = 0;

	// [iter69] @@EELOAD_HLE_COPY@@ – HLE: scan ROMDIR and copy EELOAD to EE RAM 0x82000
	// IOP DMA9 が BIOSboot完了前に EE が 0x82000 に到達するため、boot時にHLEで直接コピー。
	// Removal condition: psxDma9 が正always EELOADを SIF0 DMA 経由でロードできるようfixされた後。
	Console.WriteLn("@@EELOAD_HLE_ENTRY@@ BiosRom.size()=%zu eeMem=%p", BiosRom.size(), (void*)eeMem);
	if (BiosRom.size() >= sizeof(romdir))
	{
		const u8* romdata = BiosRom.data();
		const size_t romsize = BiosRom.size();

		// Find RESET entry (start of ROMDIR table)
		u32 dir_offset = UINT32_MAX;
		for (u32 i = 0; i + sizeof(romdir) <= romsize; i++)
		{
			const romdir* rd = reinterpret_cast<const romdir*>(romdata + i);
			if (std::strncmp(rd->fileName, "RESET", sizeof(rd->fileName)) == 0)
			{
				dir_offset = i;
				break;
			}
		}

		Console.WriteLn("@@EELOAD_HLE_SCAN@@ dir_offset=%08x", dir_offset);
		if (dir_offset != UINT32_MAX)
		{
			u32 file_offset = 0;
			const romdir* entry = reinterpret_cast<const romdir*>(romdata + dir_offset);
			const u32 max_entries = (romsize - dir_offset) / sizeof(romdir);
			u32 found_n = 0;
			for (u32 n = 0; n < max_entries && entry->fileName[0] != '\0'; n++, entry++)
			{
				found_n = n;
				if (std::strncmp(entry->fileName, "EELOAD", sizeof(entry->fileName)) == 0)
				{
					Console.WriteLn("@@EELOAD_HLE_FOUND@@ n=%u file_offset=%08x fileSize=%08x", n, file_offset, entry->fileSize);
					if (entry->fileSize > 0 && file_offset + entry->fileSize <= romsize)
					{
						static constexpr u32 EELOAD_DEST = 0x82000;
						const u32 copy_size = std::min<u32>(entry->fileSize,
							Ps2MemSize::MainRam - EELOAD_DEST);
						std::memcpy(eeMem->Main + EELOAD_DEST, romdata + file_offset, copy_size);
						s_eeload_rom_offset = file_offset;
						s_eeload_rom_size = copy_size;
						s_eeload_recopy_done = false;
						LogUnified("@@EELOAD_HLE_COPY@@ rom_offset=0x%05x size=0x%04x dest=0x%05x\n",
							file_offset, entry->fileSize, EELOAD_DEST);
						// [iter209] EELOAD データセクション再配置:
						// EELOAD はフラットバイナリだが、コードはデータ(文字列等)を
						// フラットコピーとは異なるaddressで参照する。
						// 1) フラットコピー内で "rom0:OSDSYS" を検索
						// 2) コード先頭256バイトから LUI+ADDIU(a0参照)で期待addressを抽出
						// 3) 差分があればデータセクションを期待addressにもコピー
						{
							// Find "rom0:OSDSYS" in flat-copied EELOAD
							const u8* eeload_data = romdata + file_offset;
							u32 str_foff = UINT32_MAX;
							for (u32 i = 0; i + 12 <= copy_size; i++) {
								if (std::memcmp(eeload_data + i, "rom0:OSDSYS", 12) == 0) {
									str_foff = i;
									break;
								}
							}
							if (str_foff != UINT32_MAX) {
								const u32 str_flat_addr = EELOAD_DEST + str_foff;
								// Scan first 512 bytes of EELOAD code for LUI $a0 + ADDIU $a0 sequence
								u32 str_expected = 0;
								for (u32 i = 0; i + 8 <= copy_size && i < 512; i += 4) {
									u32 insn0, insn1;
									std::memcpy(&insn0, eeload_data + i, 4);
									// LUI $a0, imm = 3C04xxxx
									if ((insn0 & 0xFFFF0000u) == 0x3C040000u) {
										u32 lui_imm = insn0 & 0xFFFFu;
										// Search subsequent 32 bytes for ADDIU $a0, $a0, imm = 2484xxxx
										for (u32 j = i + 4; j + 4 <= copy_size && j < i + 36; j += 4) {
											std::memcpy(&insn1, eeload_data + j, 4);
											if ((insn1 & 0xFFFF0000u) == 0x24840000u) {
												s16 addiu_imm = static_cast<s16>(insn1 & 0xFFFFu);
												str_expected = (lui_imm << 16) + addiu_imm;
												break;
											}
										}
										if (str_expected != 0) break;
									}
								}
								if (str_expected != 0 && str_expected != str_flat_addr) {
									// Data section needs relocation
									const u32 data_size = copy_size - str_foff;
									if (str_expected + data_size <= Ps2MemSize::MainRam) {
										std::memcpy(eeMem->Main + str_expected, eeload_data + str_foff, data_size);
										LogUnified("@@EELOAD_DATA_RELOC@@ str_foff=0x%x flat=0x%x expected=0x%x slide=0x%x data_size=0x%x\n",
											str_foff, str_flat_addr, str_expected, str_expected - str_flat_addr, data_size);
									}
								} else {
									LogUnified("@@EELOAD_DATA_RELOC@@ no_reloc str_foff=0x%x flat=0x%x expected=0x%x\n",
										str_foff, str_flat_addr, str_expected);
								}
							}
						}
					}
					break;
				}
				// Advance file_offset (16-byte aligned)
				if ((entry->fileSize % 0x10) == 0)
					file_offset += entry->fileSize;
				else
					file_offset += (entry->fileSize + 0x10) & 0xFFFFFFF0u;
			}
			Console.WriteLn("@@EELOAD_HLE_SCAN_DONE@@ scanned=%u entries, last_foff=%08x", found_n, file_offset);
		}
	}

	// [iter213] @@BIOS_BLTZ_BYPASS@@ – BFC0089C の BLTZ v0 を NOP にパッチ
	// cause: 9FC41000 (HW init) が負の値を返すため、BIOS が 9FC00C00 (カーネル初期化) をskipする。
	// DMAC_CTRL=0, INTC_MASK=0, 例外ベクタ未config, SIF 未初期化でデッドロック。
	// このパッチにより BLTZ を NOP 化し、カーネル初期化をforce実行する。
	// Removal condition: 9FC41000 が正常な値を返すようfixされた後にdelete
	{
		constexpr u32 BLTZ_OFFSET = 0x089C; // BFC0089C - BFC00000
		u32 insn;
		std::memcpy(&insn, eeMem->ROM + BLTZ_OFFSET, 4);
		if (insn == 0x0440002Bu) { // BLTZ $v0, +43
			u32 nop = 0x00000000;
			std::memcpy(eeMem->ROM + BLTZ_OFFSET, &nop, 4);
			Console.WriteLn("@@BIOS_BLTZ_BYPASS@@ patched BFC0089C: BLTZ v0 -> NOP (was %08x)", insn);
		} else {
			Console.WriteLn("@@BIOS_BLTZ_BYPASS@@ BFC0089C instruction mismatch: %08x (expected 0440002b)", insn);
		}
	}
}

// [iter230] @@EELOAD_LATE_RECOPY@@ – カーネル初期化後の EELOAD 再コピー
// カーネル初期化が EE RAM をクリアし、最初の HLE コピーをcorruptするため、
// カーネルが polling stateに入った時点で再コピーする。
// Removal condition: SIF0 DMA (psxDma9) が正常behaviorするようになった後
bool BiosRetriggerEeloadCopy()
{
	if (s_eeload_recopy_done || s_eeload_rom_size == 0)
		return false;

	static constexpr u32 EELOAD_DEST = 0x82000;

	if (BiosRom.size() < s_eeload_rom_offset + s_eeload_rom_size)
		return false;

	const u8* romdata = BiosRom.data();
	std::memcpy(eeMem->Main + EELOAD_DEST, romdata + s_eeload_rom_offset, s_eeload_rom_size);

	// Data section relocation (same as initial copy)
	const u8* eeload_data = romdata + s_eeload_rom_offset;
	u32 str_foff = UINT32_MAX;
	for (u32 i = 0; i + 12 <= s_eeload_rom_size; i++) {
		if (std::memcmp(eeload_data + i, "rom0:OSDSYS", 12) == 0) {
			str_foff = i;
			break;
		}
	}
	if (str_foff != UINT32_MAX) {
		const u32 str_flat_addr = EELOAD_DEST + str_foff;
		u32 str_expected = 0;
		for (u32 i = 0; i + 8 <= s_eeload_rom_size && i < 512; i += 4) {
			u32 insn0;
			std::memcpy(&insn0, eeload_data + i, 4);
			if ((insn0 & 0xFFFF0000u) == 0x3C040000u) {
				u32 lui_imm = insn0 & 0xFFFFu;
				for (u32 j = i + 4; j + 4 <= s_eeload_rom_size && j < i + 36; j += 4) {
					u32 insn1;
					std::memcpy(&insn1, eeload_data + j, 4);
					if ((insn1 & 0xFFFF0000u) == 0x24840000u) {
						s16 addiu_imm = static_cast<s16>(insn1 & 0xFFFFu);
						str_expected = (lui_imm << 16) + addiu_imm;
						break;
					}
				}
				if (str_expected != 0) break;
			}
		}
		if (str_expected != 0 && str_expected != str_flat_addr) {
			const u32 data_size = s_eeload_rom_size - str_foff;
			if (str_expected + data_size <= Ps2MemSize::MainRam) {
				std::memcpy(eeMem->Main + str_expected, eeload_data + str_foff, data_size);
			}
		}
	}

	s_eeload_recopy_done = true;
	Console.WriteLn("@@EELOAD_LATE_RECOPY@@ re-copied EELOAD to 0x%05x size=0x%04x (kern init cleared original)",
		EELOAD_DEST, s_eeload_rom_size);
	return true;
}

// [R57] warm reboot 後に EELOAD の再コピーを許可する
void BiosResetEeloadCopyFlag()
{
	s_eeload_recopy_done = false;
}
