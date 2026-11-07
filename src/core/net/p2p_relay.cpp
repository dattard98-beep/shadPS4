// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "core/net/p2p_relay.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#endif

namespace Core::Net::P2P::Relay {

namespace {

using Clock = std::chrono::steady_clock;

std::function<void(LogLevel, const std::string&)>& Sink() {
    static std::function<void(LogLevel, const std::string&)> sink;
    return sink;
}

void Log(LogLevel level, const std::string& text) {
    if (Sink()) {
        Sink()(level, text);
    }
}

std::string Hex(u64 value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

std::string Quoted(std::string_view name) {
    return "'" + std::string(name) + "'";
}

constexpr auto kBindInterval = std::chrono::seconds{2};
constexpr auto kAckWarnAfter = std::chrono::seconds{6};

struct Session {
    Endpoint peer;
    std::string name;
    bool active = false;
    bool acked = false;
    bool warned_no_ack = false;
    u8 others = 0;
    Clock::time_point registered{};
    Clock::time_point last_bind{};
    u64 relayed_out = 0;
    u64 relayed_in = 0;
};

struct State {
    std::mutex mutex;
    std::string server_text;
    std::optional<Endpoint> server;
    std::map<u64, Session> sessions;
};

State& Instance() {
    static State state;
    return state;
}

std::string EndpointText(const Endpoint& e) {
    if (e.Family() != AF_INET) {
        return "?";
    }
    char text[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&e.addr)->sin_addr, text,
              sizeof(text));
    return std::string(text) + ":" + std::to_string(e.Port());
}

std::optional<Endpoint> Resolve(const std::string& text) {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return std::nullopt;
    }
    const std::string host = text.substr(0, colon);
    const int port = std::atoi(text.c_str() + colon + 1);
    if (port <= 0 || port > 0xffff) {
        return std::nullopt;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        return std::nullopt;
    }
    Endpoint e =
        Endpoint::FromSockaddr(result->ai_addr, static_cast<socklen_t>(result->ai_addrlen));
    freeaddrinfo(result);
    reinterpret_cast<sockaddr_in*>(&e.addr)->sin_port = htons(static_cast<u16>(port));
    return e;
}

void WriteHeader(u8* out, u8 type, u64 session) {
    std::memcpy(out, "GZRY", 4);
    out[4] = 1;
    out[5] = type;
    out[6] = 0;
    out[7] = 0;
    for (int i = 0; i < 8; ++i) {
        out[8 + i] = static_cast<u8>(session >> (8 * i));
    }
}

u64 ReadSession(const u8* in) {
    u64 session = 0;
    for (int i = 0; i < 8; ++i) {
        session |= static_cast<u64>(in[8 + i]) << (8 * i);
    }
    return session;
}

} // namespace

void SetLogSink(std::function<void(LogLevel, const std::string&)> sink) {
    Sink() = std::move(sink);
}

void SetServer(const std::string& server) {
    auto& state = Instance();
    {
        std::scoped_lock lock{state.mutex};
        if (server == state.server_text) {
            return;
        }
        state.server_text = server;
        state.server.reset();
    }
    if (server.empty()) {
        Log(LogLevel::Info, "Relay: off (relay_server is empty)");
        return;
    }
    const auto resolved = Resolve(server);
    std::scoped_lock lock{state.mutex};
    if (state.server_text != server) {
        return;
    }
    state.server = resolved;
    if (resolved) {
        Log(LogLevel::Info,
            "Relay: using relay server " + server + " (" + EndpointText(*resolved) + ")");
    } else {
        Log(LogLevel::Error,
            "Relay: cannot resolve relay_server '" + server + "' (expected host:port)");
    }
}

bool Configured() {
    auto& state = Instance();
    std::scoped_lock lock{state.mutex};
    return state.server.has_value();
}

u64 SessionFor(std::string_view online_id_a, std::string_view online_id_b) {
    const auto& lo = online_id_a < online_id_b ? online_id_a : online_id_b;
    const auto& hi = online_id_a < online_id_b ? online_id_b : online_id_a;
    u64 hash = 0xcbf29ce484222325ull; // FNV-1a
    const auto mix = [&hash](std::string_view text) {
        for (const char c : text) {
            hash ^= static_cast<u8>(c);
            hash *= 0x100000001b3ull;
        }
    };
    mix("gzrelay1|");
    mix(lo);
    mix("|");
    mix(hi);
    return hash;
}

void RegisterPeer(u64 session, const Endpoint& peer, std::string_view peer_name) {
    auto& state = Instance();
    std::scoped_lock lock{state.mutex};
    if (!state.server) {
        return;
    }
    auto [it, inserted] = state.sessions.try_emplace(session);
    auto& s = it->second;
    if (inserted) {
        s.name = std::string(peer_name);
        s.registered = Clock::now();
        Log(LogLevel::Info,
            "Relay: standing by for peer " + Quoted(s.name) + " (session " + Hex(session) + ")");
    }
    if (!s.active) {
        s.peer = peer;
    }
}

void Activate(u64 session, std::string_view reason) {
    auto& state = Instance();
    std::scoped_lock lock{state.mutex};
    const auto it = state.sessions.find(session);
    if (!state.server || it == state.sessions.end() || it->second.active) {
        return;
    }
    it->second.active = true;
    it->second.last_bind = {}; // bind right away
    Log(LogLevel::Info, "Relay: switching peer " + Quoted(it->second.name) + " (" +
                            EndpointText(it->second.peer) +
                            ") to the relay: " + std::string(reason));
}

bool IsActive(u64 session) {
    auto& state = Instance();
    std::scoped_lock lock{state.mutex};
    const auto it = state.sessions.find(session);
    return it != state.sessions.end() && it->second.active;
}

void Forget(u64 session) {
    auto& state = Instance();
    std::scoped_lock lock{state.mutex};
    const auto it = state.sessions.find(session);
    if (it == state.sessions.end()) {
        return;
    }
    if (it->second.active) {
        Log(LogLevel::Info, "Relay: done with peer " + Quoted(it->second.name) + " (relayed " +
                                std::to_string(it->second.relayed_out) + " packets out, " +
                                std::to_string(it->second.relayed_in) + " in)");
    }
    state.sessions.erase(it);
}

std::optional<Endpoint> Route(const Endpoint& to, std::span<const u8> packet,
                              std::vector<u8>* wrapped) {
    auto& state = Instance();
    std::scoped_lock lock{state.mutex};
    if (!state.server) {
        return std::nullopt;
    }
    for (auto& [session, s] : state.sessions) {
        if (!s.active || !(s.peer == to)) {
            continue;
        }
        wrapped->resize(kHeaderSize + packet.size());
        WriteHeader(wrapped->data(), kTypeData, session);
        std::memcpy(wrapped->data() + kHeaderSize, packet.data(), packet.size());
        ++s.relayed_out;
        return state.server;
    }
    return std::nullopt;
}

Inbound HandleInbound(std::span<const u8> packet, const Endpoint& from, std::vector<u8>* payload,
                      Endpoint* peer) {
    auto& state = Instance();
    std::scoped_lock lock{state.mutex};
    if (!state.server || !(from == *state.server) || packet.size() < kHeaderSize ||
        std::memcmp(packet.data(), "GZRY", 4) != 0 || packet[4] != 1) {
        return Inbound::NotRelay;
    }
    const u64 session = ReadSession(packet.data());
    const auto it = state.sessions.find(session);
    if (it == state.sessions.end()) {
        return Inbound::Consumed; // a session we already finished
    }
    auto& s = it->second;
    switch (packet[5]) {
    case kTypeBindAck:
        if (!s.acked || s.others != packet[6]) {
            Log(LogLevel::Info, "Relay: server reached for peer " + Quoted(s.name) + " (" +
                                    std::to_string(packet[6]) + " other player" +
                                    (packet[6] == 1 ? "" : "s") + " there)");
        }
        s.acked = true;
        s.others = packet[6];
        return Inbound::Consumed;
    case kTypeData:
        if (!s.active) {
            // The other side switched to the relay; follow it so replies take the same path.
            s.active = true;
            Log(LogLevel::Info,
                "Relay: peer " + Quoted(s.name) + " is talking through the relay; switching too");
        }
        ++s.relayed_in;
        payload->assign(packet.begin() + kHeaderSize, packet.end());
        *peer = s.peer;
        return Inbound::Data;
    default:
        return Inbound::Consumed;
    }
}

std::vector<std::vector<u8>> DueControlPackets(Endpoint* server) {
    auto& state = Instance();
    std::scoped_lock lock{state.mutex};
    std::vector<std::vector<u8>> out;
    if (!state.server) {
        return out;
    }
    *server = *state.server;
    const auto now = Clock::now();
    for (auto& [session, s] : state.sessions) {
        if (!s.acked && !s.warned_no_ack && now - s.registered >= kAckWarnAfter) {
            s.warned_no_ack = true;
            Log(LogLevel::Warning, "Relay: no answer from relay server " + state.server_text +
                                       " yet. Is it running, and is UDP port " +
                                       std::to_string(state.server->Port()) + " open on it?");
        }
        if (now - s.last_bind < kBindInterval) {
            continue;
        }
        s.last_bind = now;
        std::vector<u8> bind(kHeaderSize);
        WriteHeader(bind.data(), kTypeBind, session);
        out.push_back(std::move(bind));
    }
    return out;
}

} // namespace Core::Net::P2P::Relay
