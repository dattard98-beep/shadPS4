// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// UDP relay fallback for P2P traffic.
//
// When two players cannot reach each other directly (both behind routers that block incoming
// traffic), every packet for that peer is wrapped and sent to a relay server, which forwards it
// to the other player. The relay pairs the two players by a session id that both compute from
// their online ids. Everything above the transport (signaling, Rudp, the game) keeps using the
// peer's normal address and never sees the relay.
//
// Wire format, every relay packet:
//   0  "GZRY"         magic
//   4  u8 version     1
//   5  u8 type        1 = bind (register/keepalive), 2 = bind ack, 3 = data
//   6  u8 info        bind ack: number of other players registered in the session
//   7  u8 reserved
//   8  u64 session    little endian
//  16  payload        data packets only: the original UDP payload

#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/net/p2p_transport.h"

namespace Core::Net::P2P::Relay {

constexpr size_t kHeaderSize = 16;
constexpr u8 kTypeBind = 1;
constexpr u8 kTypeBindAck = 2;
constexpr u8 kTypeData = 3;

enum class LogLevel { Info, Warning, Error };
// The emulator routes relay messages into its log; tests leave this unset.
void SetLogSink(std::function<void(LogLevel, const std::string&)> sink);

// "host:port" from the settings; empty disables the relay. Resolves the name when it changes.
void SetServer(const std::string& server);
bool Configured();

// Same id on both sides: derived from the two online ids, order independent.
u64 SessionFor(std::string_view online_id_a, std::string_view online_id_b);

// A peer we are trying to reach. Registering starts keepalives to the relay so the other side
// can switch to it at any time. While not active, the peer endpoint follows `peer`.
void RegisterPeer(u64 session, const Endpoint& peer, std::string_view peer_name);
// Route this peer through the relay from now on.
void Activate(u64 session, std::string_view reason);
bool IsActive(u64 session);
void Forget(u64 session);

// Transport hooks (transport mutex held).
// If `to` is a relayed peer, returns the relay server and writes the wrapped packet.
std::optional<Endpoint> Route(const Endpoint& to, std::span<const u8> packet,
                              std::vector<u8>* wrapped);
enum class Inbound { NotRelay, Consumed, Data };
// Data packets from the relay come back as `payload` from the peer's registered endpoint.
Inbound HandleInbound(std::span<const u8> packet, const Endpoint& from, std::vector<u8>* payload,
                      Endpoint* peer);

// Keepalive packets that are due now, all addressed to the relay server.
std::vector<std::vector<u8>> DueControlPackets(Endpoint* server);

} // namespace Core::Net::P2P::Relay
