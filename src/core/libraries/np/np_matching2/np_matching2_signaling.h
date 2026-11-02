// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/np/np_matching2/np_matching2.h"

namespace Libraries::Np::NpMatching2 {

struct ContextObject;
struct CallbackPayload;

void StartMatching2SignalingRuntime();

// Ghost rooms: rooms whose host never became reachable after we joined (usually a session
// left behind by a crashed emulator). They are reported as dead to the game and hidden from
// searches for a while.
bool IsGhostRoom(OrbisNpMatching2RoomId room_id);
void CheckGhostRooms();

// Open-host matchmaking (Godzilla only). A player whose P2P port can be reached from the
// internet (UPnP forwarded it, or the user forwarded it by hand and set p2p_port_forwarded) is
// "open"; anyone else is "blocked". Rooms are tagged with their host's status in the last
// searchable int attribute, which the game does not use, and blocked players only see rooms of
// open hosts, since two blocked players can never reach each other directly.
constexpr u16 kOpenHostAttrId = 0x0053; // ROOM_SEARCHABLE_INT_ATTR_EXTERNAL_8_ID
constexpr u32 kOpenHostValueOpen = 1;
constexpr u32 kOpenHostValueBlocked = 2;
bool OpenHostMatchingActive();
bool IsLocalP2PReachable();
void StopMatching2SignalingRuntime();

bool SendMatching2StunPing(const ContextObject& ctx);

void QueueMatching2DeadForRoomPeers(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                                    s32 error_code);
void QueueMatching2SignalingEvent(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                                  OrbisNpMatching2RoomMemberId member_id,
                                  OrbisNpMatching2Event event, s32 error_code);

u32 GetRoomPingUs(const ContextObject& ctx, OrbisNpMatching2RoomId roomId);
void* BuildSignalingGetPingInfoPayload(ContextObject& ctx, CallbackPayload& payload,
                                       OrbisNpMatching2RoomId roomId);
s32 FillMatching2ConnectionInfo(const ContextObject& ctx, OrbisNpMatching2RoomId roomId,
                                OrbisNpMatching2RoomMemberId memberId, u32 infoType, void* connInfo,
                                bool a_variant);
s32 FillMatching2LocalNetInfo(void* info);

} // namespace Libraries::Np::NpMatching2
