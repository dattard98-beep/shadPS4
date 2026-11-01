// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Net {
struct OrbisNetSockaddr;
}

namespace Libraries::Rudp {

s32 PS4_SYSV_ABI sceRudpAccept();
s32 PS4_SYSV_ABI sceRudpActivate();
s32 PS4_SYSV_ABI sceRudpBind(s32 ctx_id, s32 soc, u16 vport, u8 mux_mode);
s32 PS4_SYSV_ABI sceRudpCreateContext(void* handler, void* arg, s32* ctx_id);
s32 PS4_SYSV_ABI sceRudpEnableInternalIOThread(u32 stack_size, u32 priority);
s32 PS4_SYSV_ABI sceRudpEnableInternalIOThread2(u32 stack_size, u32 priority, u64 affinity);
s32 PS4_SYSV_ABI sceRudpEnd();
s32 PS4_SYSV_ABI sceRudpFlush(s32 ctx_id);
s32 PS4_SYSV_ABI sceRudpGetContextStatus();
s32 PS4_SYSV_ABI sceRudpGetLocalInfo();
s32 PS4_SYSV_ABI sceRudpGetMaxSegmentSize();
s32 PS4_SYSV_ABI sceRudpGetNumberOfPacketsToRead(s32 ctx_id);
s32 PS4_SYSV_ABI sceRudpGetOption(s32 ctx_id, s32 option, void* value, u64 len);
s32 PS4_SYSV_ABI sceRudpGetRemoteInfo();
s32 PS4_SYSV_ABI sceRudpGetSizeReadable(s32 ctx_id);
s32 PS4_SYSV_ABI sceRudpGetSizeWritable(s32 ctx_id);
s32 PS4_SYSV_ABI sceRudpGetStatus();
s32 PS4_SYSV_ABI sceRudpInit(void* mem_pool, u64 mem_pool_size);
s32 PS4_SYSV_ABI sceRudpInitiate(s32 ctx_id, const Net::OrbisNetSockaddr* to, u32 tolen,
                                 u16 vport);
s32 PS4_SYSV_ABI sceRudpListen();
s32 PS4_SYSV_ABI sceRudpNetFlush();
s32 PS4_SYSV_ABI sceRudpNetReceived(s32 soc, const u8* data, u64 datalen,
                                    const Net::OrbisNetSockaddr* from, u32 fromlen);
s32 PS4_SYSV_ABI sceRudpPollCancel();
s32 PS4_SYSV_ABI sceRudpPollControl();
s32 PS4_SYSV_ABI sceRudpPollCreate();
s32 PS4_SYSV_ABI sceRudpPollDestroy();
s32 PS4_SYSV_ABI sceRudpPollWait();
s32 PS4_SYSV_ABI sceRudpProcessEvents(u32 timeout_usec);
s32 PS4_SYSV_ABI sceRudpRead(s32 ctx_id, void* data, u64 len, u8 flags, void* read_info);
s32 PS4_SYSV_ABI sceRudpSetEventHandler(void* handler, void* arg);
s32 PS4_SYSV_ABI sceRudpSetMaxSegmentSize();
s32 PS4_SYSV_ABI sceRudpSetOption(s32 ctx_id, s32 option, const void* value, u64 len);
s32 PS4_SYSV_ABI sceRudpTerminate(s32 ctx_id);
s32 PS4_SYSV_ABI sceRudpWrite(s32 ctx_id, const void* data, u64 len, u8 flags);

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Rudp
