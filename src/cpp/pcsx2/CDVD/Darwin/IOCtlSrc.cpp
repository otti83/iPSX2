// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/CDVDdiscReader.h"
#include "CDVD/CDVD.h"

#include "common/Console.h"
#include "common/Error.h"

#ifdef __APPLE__
#include <IOKit/storage/IOCDMediaBSDClient.h>
#include <IOKit/storage/IODVDMediaBSDClient.h>
#endif

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#if !TARGET_OS_IPHONE
IOCtlSrc::IOCtlSrc(std::string filename)
	: m_filename(std::move(filename))
{
}
IOCtlSrc::~IOCtlSrc()
{
	if (m_device != -1)
	{
		SetSpindleSpeed(true);
		close(m_device);
	}
}

bool IOCtlSrc::Reopen(Error* error)
{
	if (m_device != -1)
		close(m_device);

	m_device = open(m_filename.c_str(), O_RDONLY | O_NONBLOCK);
	if (m_device == -1)
	{
		Error::SetErrno(error, errno);
		return false;
	}

	if (ReadDVDInfo() || ReadCDInfo())
		SetSpindleSpeed(false);

	return true;
}

void IOCtlSrc::SetSpindleSpeed(bool restore_defaults) const
{
#ifdef __APPLE__
	u16 speed = restore_defaults ? 0xFFFF : m_media_type >= 0 ? 5540 : 3600;
	int ioctl_code = m_media_type >= 0 ? DKIOCDVDSETSPEED : DKIOCCDSETSPEED;
	ioctl(m_device, ioctl_code, &speed);
#endif
}

u32 IOCtlSrc::GetSectorCount() const { return m_sectors; }
u32 IOCtlSrc::GetLayerBreakAddress() const { return m_layer_break; }
s32 IOCtlSrc::GetMediaType() const { return m_media_type; }
const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const { return m_toc; }

bool IOCtlSrc::ReadSectors2048(u32 sector, u32 count, u8* buffer) const
{
	const ssize_t bytes_to_read = 2048 * count;
	return pread(m_device, buffer, bytes_to_read, sector * 2048ULL) == bytes_to_read;
}

bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
#ifdef __APPLE__
	dk_cd_read_t desc;
	memset(&desc, 0, sizeof(dk_cd_read_t));
	desc.sectorArea = kCDSectorAreaSync | kCDSectorAreaHeader | kCDSectorAreaSubHeader | kCDSectorAreaUser | kCDSectorAreaAuxiliary;
	desc.sectorType = kCDSectorTypeUnknown;
	for (u32 i = 0; i < count; ++i)
	{
		desc.offset = (sector + i) * 2352ULL;
		desc.buffer = buffer + i * 2352;
		desc.bufferLength = 2352;
		if (ioctl(m_device, DKIOCCDREAD, &desc) == -1) return false;
	}
	return true;
#else
	return false;
#endif
}

bool IOCtlSrc::ReadDVDInfo() { return false; }
bool IOCtlSrc::ReadCDInfo() { return false; }
bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subQ) const { return false; }
bool IOCtlSrc::DiscReady() { return !!m_sectors; }
#else
// iOS Stubs
#include "CDVD/CDVDisoReader.h"

IOCtlSrc::IOCtlSrc(std::string filename) : m_filename(std::move(filename)) {}
IOCtlSrc::~IOCtlSrc() {}
bool IOCtlSrc::Reopen(Error* error) { return false; }
void IOCtlSrc::SetSpindleSpeed(bool restore_defaults) const {}
u32 IOCtlSrc::GetSectorCount() const { return 0; }
u32 IOCtlSrc::GetLayerBreakAddress() const { return 0; }
s32 IOCtlSrc::GetMediaType() const { return -1; }
const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const { return m_toc; }
bool IOCtlSrc::ReadSectors2048(u32 sector, u32 count, u8* buffer) const { return false; }
bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const { return false; }
bool IOCtlSrc::ReadDVDInfo() { return false; }
bool IOCtlSrc::ReadCDInfo() { return false; }
bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subQ) const { return false; }
bool IOCtlSrc::DiscReady() { return false; }
#endif

