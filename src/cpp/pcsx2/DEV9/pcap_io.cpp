// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcap_io.h"

#ifdef PCSX2_NO_PCAP

using namespace PacketReader;

PCAPAdapter::PCAPAdapter() : NetAdapter() {}
PCAPAdapter::~PCAPAdapter() {}
bool PCAPAdapter::recv(NetPacket* pkt) { return false; }
bool PCAPAdapter::send(NetPacket* pkt) { return false; }
void PCAPAdapter::reloadSettings() {}
std::vector<AdapterEntry> PCAPAdapter::GetAdapters() { return {}; }
bool PCAPAdapter::InitPCAP(const std::string& adapter, bool promiscuous) { return false; }
bool PCAPAdapter::blocks() { return false; }
bool PCAPAdapter::isInitialised() { return false; }
AdapterOptions PCAPAdapter::GetAdapterOptions() { return AdapterOptions::None; }
bool PCAPAdapter::SetMACSwitchedFilter(MAC_Address mac) { return false; }
void PCAPAdapter::SetMACBridgedRecv(NetPacket* pkt) {}
void PCAPAdapter::SetMACBridgedSend(NetPacket* pkt) {}
void PCAPAdapter::HandleFrameCheckSequence(NetPacket* pkt) {}
bool PCAPAdapter::ValidateEtherFrame(NetPacket* pkt) { return false; }

#endif
