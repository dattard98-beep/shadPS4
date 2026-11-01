// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// libSceRudp: reliable UDP over a P2P socket.
//
// The protocol lives in rudp_core.cpp. This file adapts it to the guest API: it owns the
// global state, sends and receives through the guest P2P socket (sceNetSendto/Recvfrom), and
// runs the internal I/O thread that also delivers every guest callback. Callbacks are never
// invoked from inside an API call, because games call Write/Flush while holding the same
// mutex their event handler takes.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/libs.h"
#include "core/libraries/net/net.h"
#include "core/libraries/np/np_common.h"
#include "core/libraries/rudp/rudp.h"
#include "core/libraries/rudp/rudp_core.h"

namespace Libraries::Rudp {

namespace {

using ContextHandler = PS4_SYSV_ABI void (*)(s32 ctx_id, s32 event_id, s32 error_code, void* arg);
using EventHandler = PS4_SYSV_ABI s32 (*)(s32 event_id, s32 soc, const u8* data, u64 datalen,
                                          const Net::OrbisNetSockaddr* addr, u32 addrlen,
                                          void* arg);

constexpr u64 kIoThreadMinStack = 256 * 1024;
constexpr size_t kRecvBuffer = 2048;
constexpr int kMaxPacketsPerPoll = 512;

struct PendingCallback {
    enum class Kind { Context, SocketReleased } kind;
    s32 ctx_id = -1;
    s32 event = 0;
    s32 error = 0;
    s32 socket = -1;
};

struct ContextInfo {
    ContextHandler handler = nullptr;
    void* arg = nullptr;
};

struct RudpState {
    std::mutex mutex;
    bool initialized = false;
    std::unique_ptr<Proto::Engine> engine;
    std::map<s32, ContextInfo> contexts;
    EventHandler event_handler = nullptr;
    void* event_arg = nullptr;
    std::deque<PendingCallback> callbacks;

    std::atomic<bool> io_running{false};
    Libraries::Kernel::PthreadT io_thread = nullptr;
};

RudpState& State() {
    static RudpState state;
    return state;
}

Proto::Millis Now() {
    using namespace std::chrono;
    return static_cast<Proto::Millis>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

const char* ContextEventName(s32 event) {
    switch (event) {
    case Proto::CONTEXT_EVENT_CLOSED:
        return "CLOSED";
    case Proto::CONTEXT_EVENT_ESTABLISHED:
        return "ESTABLISHED";
    case Proto::CONTEXT_EVENT_ERROR:
        return "ERROR";
    case Proto::CONTEXT_EVENT_WRITABLE:
        return "WRITABLE";
    case Proto::CONTEXT_EVENT_READABLE:
        return "READABLE";
    case Proto::CONTEXT_EVENT_FLUSHED:
        return "FLUSHED";
    default:
        return "?";
    }
}

Net::OrbisNetSockaddrIn ToSockaddr(const Proto::PeerKey& peer) {
    Net::OrbisNetSockaddrIn sa{};
    sa.sin_len = sizeof(sa);
    sa.sin_family = Net::ORBIS_NET_AF_INET;
    sa.sin_port = peer.port;
    sa.sin_addr = peer.addr;
    sa.sin_vport = peer.vport;
    return sa;
}

bool FromSockaddr(const Net::OrbisNetSockaddr* addr, u32 len, Proto::PeerKey* out) {
    if (addr == nullptr || len < sizeof(Net::OrbisNetSockaddrIn)) {
        return false;
    }
    Net::OrbisNetSockaddrIn sa{};
    std::memcpy(&sa, addr, sizeof(sa));
    if (sa.sin_family != Net::ORBIS_NET_AF_INET) {
        return false;
    }
    out->addr = sa.sin_addr;
    out->port = sa.sin_port;
    out->vport = sa.sin_vport;
    return true;
}

std::string FormatPeer(const Proto::PeerKey& peer) {
    const u8* a = reinterpret_cast<const u8*>(&peer.addr);
    const u16 port = static_cast<u16>((peer.port >> 8) | (peer.port << 8));
    const u16 vport = static_cast<u16>((peer.vport >> 8) | (peer.vport << 8));
    return fmt::format("{}.{}.{}.{}:{} vport {}", a[0], a[1], a[2], a[3], port, vport);
}

// Runs the engine's queued actions. Must be called WITHOUT the state mutex held. Sends happen
// immediately; callbacks are queued for the I/O thread unless `fire` is true.
void RunActions(std::vector<Proto::Action>&& actions, bool fire);

void FirePending() {
    auto& state = State();
    for (;;) {
        PendingCallback cb;
        ContextInfo info;
        EventHandler event_handler = nullptr;
        void* event_arg = nullptr;
        {
            std::scoped_lock lock{state.mutex};
            if (state.callbacks.empty()) {
                return;
            }
            cb = state.callbacks.front();
            state.callbacks.pop_front();
            if (cb.kind == PendingCallback::Kind::Context) {
                const auto it = state.contexts.find(cb.ctx_id);
                if (it == state.contexts.end()) {
                    continue; // terminated before the event could be delivered
                }
                info = it->second;
            } else {
                event_handler = state.event_handler;
                event_arg = state.event_arg;
            }
        }
        if (cb.kind == PendingCallback::Kind::Context) {
            if (cb.event != Proto::CONTEXT_EVENT_READABLE &&
                cb.event != Proto::CONTEXT_EVENT_WRITABLE) {
                LOG_INFO(Lib_Rudp, "ctx {} event {} error {:#x}", cb.ctx_id,
                         ContextEventName(cb.event), static_cast<u32>(cb.error));
            }
            if (info.handler != nullptr) {
                info.handler(cb.ctx_id, cb.event, cb.error, info.arg);
            }
        } else {
            LOG_INFO(Lib_Rudp, "socket {} released", cb.socket);
            if (event_handler != nullptr) {
                event_handler(Proto::EVENT_SOCKET_RELEASED, cb.socket, nullptr, 0, nullptr, 0,
                              event_arg);
            }
        }
    }
}

void RunActions(std::vector<Proto::Action>&& actions, bool fire) {
    auto& state = State();
    bool queued = false;
    for (auto& a : actions) {
        switch (a.kind) {
        case Proto::Action::Kind::Send: {
            const auto sa = ToSockaddr(a.to);
            const s32 r = Net::sceNetSendto(a.socket, a.data.data(), a.data.size(),
                                            Net::ORBIS_NET_MSG_DONTWAIT,
                                            reinterpret_cast<const Net::OrbisNetSockaddr*>(&sa),
                                            sizeof(sa));
            if (r < 0) {
                LOG_DEBUG(Lib_Rudp, "send on socket {} to {} failed: {:#x}", a.socket,
                          FormatPeer(a.to), static_cast<u32>(r));
            }
            break;
        }
        case Proto::Action::Kind::ContextEvent: {
            std::scoped_lock lock{state.mutex};
            state.callbacks.push_back(
                {PendingCallback::Kind::Context, a.ctx_id, a.event, a.error, -1});
            queued = true;
            break;
        }
        case Proto::Action::Kind::SocketReleased: {
            std::scoped_lock lock{state.mutex};
            state.callbacks.push_back({PendingCallback::Kind::SocketReleased, -1, 0, 0, a.socket});
            queued = true;
            break;
        }
        }
    }
    if (fire && queued) {
        FirePending();
    }
}

// Runs one engine operation under the lock, then performs what it produced.
template <typename F>
s32 WithEngine(F&& op) {
    auto& state = State();
    s32 result;
    std::vector<Proto::Action> actions;
    {
        std::scoped_lock lock{state.mutex};
        if (!state.initialized || !state.engine) {
            return Proto::ERROR_NOT_INITIALIZED;
        }
        result = op(*state.engine);
        actions = state.engine->TakeActions();
    }
    RunActions(std::move(actions), false);
    return result;
}

void PollOnce(bool receive) {
    auto& state = State();
    std::vector<s32> sockets;
    {
        std::scoped_lock lock{state.mutex};
        if (!state.engine) {
            return;
        }
        for (const s32 s : state.engine->BoundSockets()) {
            sockets.push_back(s);
        }
    }
    if (receive) {
        std::vector<u8> buf(kRecvBuffer);
        for (const s32 s : sockets) {
            for (int i = 0; i < kMaxPacketsPerPoll; ++i) {
                Net::OrbisNetSockaddrIn from{};
                u32 fromlen = sizeof(from);
                const s32 n = Net::sceNetRecvfrom(s, buf.data(), buf.size(),
                                                  Net::ORBIS_NET_MSG_DONTWAIT,
                                                  reinterpret_cast<Net::OrbisNetSockaddr*>(&from),
                                                  &fromlen);
                if (n <= 0) {
                    break;
                }
                Proto::PeerKey peer;
                if (!FromSockaddr(reinterpret_cast<Net::OrbisNetSockaddr*>(&from), fromlen,
                                  &peer)) {
                    continue;
                }
                std::scoped_lock lock{state.mutex};
                if (state.engine) {
                    state.engine->OnPacket(s, peer, buf.data(), static_cast<size_t>(n), Now());
                }
            }
        }
    }
    std::vector<Proto::Action> actions;
    {
        std::scoped_lock lock{state.mutex};
        if (!state.engine) {
            return;
        }
        state.engine->Tick(Now());
        actions = state.engine->TakeActions();
    }
    RunActions(std::move(actions), false);
    FirePending();
}

PS4_SYSV_ABI void* IoThreadMain(void*) {
    Common::SetCurrentThreadName("SceRudpIo");
    LOG_INFO(Lib_Rudp, "internal I/O thread started");
    auto& state = State();
    while (state.io_running.load()) {
        PollOnce(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    LOG_INFO(Lib_Rudp, "internal I/O thread stopped");
    return nullptr;
}

s32 StartIoThread(u64 stack_size, s32 priority, u64 affinity) {
    auto& state = State();
    {
        std::scoped_lock lock{state.mutex};
        if (!state.initialized) {
            return Proto::ERROR_NOT_INITIALIZED;
        }
    }
    if (state.io_running.exchange(true)) {
        return Proto::ERROR_THREAD_IN_USE;
    }
    const s32 rc = Np::NpCommon::sceNpCreateThread(
        &state.io_thread, IoThreadMain, nullptr, priority, std::max(stack_size, kIoThreadMinStack),
        affinity, "SceRudpIo");
    if (rc < 0) {
        LOG_ERROR(Lib_Rudp, "failed to create I/O thread: {:#x}", static_cast<u32>(rc));
        state.io_running.store(false);
        state.io_thread = nullptr;
        return Proto::ERROR_THREAD;
    }
    return ORBIS_OK;
}

void StopIoThread() {
    auto& state = State();
    if (!state.io_running.exchange(false)) {
        return;
    }
    if (state.io_thread != nullptr) {
        Np::NpCommon::sceNpJoinThread(state.io_thread, nullptr);
        state.io_thread = nullptr;
    }
}

} // namespace

s32 PS4_SYSV_ABI sceRudpInit(void* mem_pool, u64 mem_pool_size) {
    LOG_INFO(Lib_Rudp, "pool = {}, size = {:#x}", mem_pool, mem_pool_size);
    auto& state = State();
    std::scoped_lock lock{state.mutex};
    if (state.initialized) {
        return Proto::ERROR_ALREADY_INITIALIZED;
    }
    state.engine = std::make_unique<Proto::Engine>();
    state.contexts.clear();
    state.callbacks.clear();
    state.event_handler = nullptr;
    state.event_arg = nullptr;
    state.initialized = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpEnd() {
    LOG_INFO(Lib_Rudp, "called");
    auto& state = State();
    {
        std::scoped_lock lock{state.mutex};
        if (!state.initialized) {
            return Proto::ERROR_NOT_INITIALIZED;
        }
    }
    StopIoThread();
    std::scoped_lock lock{state.mutex};
    state.engine.reset();
    state.contexts.clear();
    state.callbacks.clear();
    state.event_handler = nullptr;
    state.event_arg = nullptr;
    state.initialized = false;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpSetEventHandler(void* handler, void* arg) {
    LOG_INFO(Lib_Rudp, "handler = {}, arg = {}", handler, arg);
    auto& state = State();
    std::scoped_lock lock{state.mutex};
    if (!state.initialized) {
        return Proto::ERROR_NOT_INITIALIZED;
    }
    state.event_handler = reinterpret_cast<EventHandler>(handler);
    state.event_arg = arg;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpEnableInternalIOThread(u32 stack_size, u32 priority) {
    LOG_INFO(Lib_Rudp, "stack = {:#x}, priority = {}", stack_size, priority);
    return StartIoThread(stack_size, static_cast<s32>(priority), 0);
}

s32 PS4_SYSV_ABI sceRudpEnableInternalIOThread2(u32 stack_size, u32 priority, u64 affinity) {
    LOG_INFO(Lib_Rudp, "stack = {:#x}, priority = {}, affinity = {:#x}", stack_size, priority,
             affinity);
    return StartIoThread(stack_size, static_cast<s32>(priority), affinity);
}

s32 PS4_SYSV_ABI sceRudpCreateContext(void* handler, void* arg, s32* ctx_id) {
    if (ctx_id == nullptr) {
        return Proto::ERROR_INVALID_ARGUMENT;
    }
    auto& state = State();
    std::scoped_lock lock{state.mutex};
    if (!state.initialized || !state.engine) {
        return Proto::ERROR_NOT_INITIALIZED;
    }
    const s32 id = state.engine->CreateContext();
    if (id < 0) {
        return id;
    }
    state.contexts[id] = {reinterpret_cast<ContextHandler>(handler), arg};
    *ctx_id = id;
    LOG_INFO(Lib_Rudp, "ctx {} created (handler = {}, arg = {})", id, handler, arg);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpSetOption(s32 ctx_id, s32 option, const void* value, u64 len) {
    s32 v = 0;
    if (value != nullptr && len >= sizeof(v)) {
        std::memcpy(&v, value, sizeof(v));
    }
    LOG_INFO(Lib_Rudp, "ctx {} option {} = {}", ctx_id, option, v);
    return WithEngine([&](Proto::Engine& e) { return e.SetOption(ctx_id, option, value, len); });
}

s32 PS4_SYSV_ABI sceRudpGetOption(s32 ctx_id, s32 option, void* value, u64 len) {
    return WithEngine([&](Proto::Engine& e) { return e.GetOption(ctx_id, option, value, len); });
}

s32 PS4_SYSV_ABI sceRudpBind(s32 ctx_id, s32 soc, u16 vport, u8 mux_mode) {
    LOG_INFO(Lib_Rudp, "ctx {} socket {} vport {} mux {}", ctx_id, soc, vport, mux_mode);
    return WithEngine([&](Proto::Engine& e) { return e.Bind(ctx_id, soc, vport, mux_mode); });
}

s32 PS4_SYSV_ABI sceRudpInitiate(s32 ctx_id, const Net::OrbisNetSockaddr* to, u32 tolen,
                                 u16 vport) {
    Proto::PeerKey peer;
    if (!FromSockaddr(to, tolen, &peer)) {
        LOG_ERROR(Lib_Rudp, "ctx {}: unsupported address (len {})", ctx_id, tolen);
        return Proto::ERROR_INVALID_ARGUMENT;
    }
    LOG_INFO(Lib_Rudp, "ctx {} -> {} (rudp vport {})", ctx_id, FormatPeer(peer), vport);
    return WithEngine([&](Proto::Engine& e) { return e.Initiate(ctx_id, peer, Now()); });
}

s32 PS4_SYSV_ABI sceRudpWrite(s32 ctx_id, const void* data, u64 len, u8 flags) {
    const s32 r = WithEngine([&](Proto::Engine& e) {
        return e.Write(ctx_id, static_cast<const u8*>(data), len, Now());
    });
    LOG_TRACE(Lib_Rudp, "ctx {} len {} flags {:#x} -> {:#x}", ctx_id, len, flags,
              static_cast<u32>(r));
    if (r == Proto::ERROR_WOULDBLOCK) {
        LOG_WARNING(Lib_Rudp, "ctx {}: send buffer full, {} bytes not sent", ctx_id, len);
    }
    return r;
}

s32 PS4_SYSV_ABI sceRudpRead(s32 ctx_id, void* data, u64 len, u8 flags, [[maybe_unused]] void* read_info) {
    const s32 r = WithEngine(
        [&](Proto::Engine& e) { return e.Read(ctx_id, static_cast<u8*>(data), len); });
    LOG_TRACE(Lib_Rudp, "ctx {} len {} flags {:#x} -> {:#x}", ctx_id, len, flags,
              static_cast<u32>(r));
    return r;
}

s32 PS4_SYSV_ABI sceRudpGetSizeReadable(s32 ctx_id) {
    return WithEngine([&](Proto::Engine& e) { return e.GetSizeReadable(ctx_id); });
}

s32 PS4_SYSV_ABI sceRudpGetNumberOfPacketsToRead(s32 ctx_id) {
    return WithEngine([&](Proto::Engine& e) { return e.GetNumberOfPacketsToRead(ctx_id); });
}

s32 PS4_SYSV_ABI sceRudpGetSizeWritable(s32 ctx_id) {
    return WithEngine([&](Proto::Engine& e) { return e.GetSizeWritable(ctx_id); });
}

s32 PS4_SYSV_ABI sceRudpFlush(s32 ctx_id) {
    const s32 r = WithEngine([&](Proto::Engine& e) { return e.Flush(ctx_id); });
    LOG_INFO(Lib_Rudp, "ctx {} -> {:#x}", ctx_id, static_cast<u32>(r));
    return r;
}

s32 PS4_SYSV_ABI sceRudpTerminate(s32 ctx_id) {
    LOG_INFO(Lib_Rudp, "ctx {}", ctx_id);
    auto& state = State();
    s32 result;
    std::vector<Proto::Action> actions;
    {
        std::scoped_lock lock{state.mutex};
        if (!state.initialized || !state.engine) {
            return Proto::ERROR_NOT_INITIALIZED;
        }
        result = state.engine->Terminate(ctx_id, Now());
        state.contexts.erase(ctx_id);
        actions = state.engine->TakeActions();
    }
    RunActions(std::move(actions), false);
    return result;
}

s32 PS4_SYSV_ABI sceRudpNetFlush() {
    LOG_DEBUG(Lib_Rudp, "called");
    // Sends are performed as soon as they are produced; nothing is held back.
    auto& state = State();
    std::scoped_lock lock{state.mutex};
    return state.initialized ? ORBIS_OK : Proto::ERROR_NOT_INITIALIZED;
}

s32 PS4_SYSV_ABI sceRudpNetReceived(s32 soc, const u8* data, u64 datalen,
                                    const Net::OrbisNetSockaddr* from, u32 fromlen) {
    Proto::PeerKey peer;
    if (data == nullptr || !FromSockaddr(from, fromlen, &peer)) {
        return Proto::ERROR_INVALID_ARGUMENT;
    }
    return WithEngine([&](Proto::Engine& e) {
        e.OnPacket(soc, peer, data, datalen, Now());
        return ORBIS_OK;
    });
}

s32 PS4_SYSV_ABI sceRudpProcessEvents([[maybe_unused]] u32 timeout_usec) {
    auto& state = State();
    {
        std::scoped_lock lock{state.mutex};
        if (!state.initialized) {
            return Proto::ERROR_NOT_INITIALIZED;
        }
    }
    // With the internal I/O thread running, that thread already does this work.
    if (!state.io_running.load()) {
        PollOnce(false);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpGetContextStatus() {
    LOG_ERROR(Lib_Rudp, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpGetLocalInfo() {
    LOG_ERROR(Lib_Rudp, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpGetMaxSegmentSize() {
    return static_cast<s32>(Proto::kMaxSegmentPayload);
}

s32 PS4_SYSV_ABI sceRudpSetMaxSegmentSize() {
    LOG_WARNING(Lib_Rudp, "ignored; segment size is fixed at {}", Proto::kMaxSegmentPayload);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpGetRemoteInfo() {
    LOG_ERROR(Lib_Rudp, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpGetStatus() {
    LOG_ERROR(Lib_Rudp, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpAccept() {
    LOG_ERROR(Lib_Rudp, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpActivate() {
    LOG_ERROR(Lib_Rudp, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpListen() {
    LOG_ERROR(Lib_Rudp, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpPollCancel() {
    LOG_ERROR(Lib_Rudp, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpPollControl() {
    LOG_ERROR(Lib_Rudp, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpPollCreate() {
    LOG_ERROR(Lib_Rudp, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpPollDestroy() {
    LOG_ERROR(Lib_Rudp, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRudpPollWait() {
    LOG_ERROR(Lib_Rudp, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("uQiK7fjU6y8", "libSceRudp", 1, "libSceRudp", sceRudpAccept);
    LIB_FUNCTION("J-6d0WTjzMc", "libSceRudp", 1, "libSceRudp", sceRudpActivate);
    LIB_FUNCTION("l4SLBpKUDK4", "libSceRudp", 1, "libSceRudp", sceRudpBind);
    LIB_FUNCTION("CAbbX6BuQZ0", "libSceRudp", 1, "libSceRudp", sceRudpCreateContext);
    LIB_FUNCTION("6PBNpsgyaxw", "libSceRudp", 1, "libSceRudp", sceRudpEnableInternalIOThread);
    LIB_FUNCTION("fJ51weR1WAI", "libSceRudp", 1, "libSceRudp", sceRudpEnableInternalIOThread2);
    LIB_FUNCTION("3hBvwqEwqj8", "libSceRudp", 1, "libSceRudp", sceRudpEnd);
    LIB_FUNCTION("Ms0cLK8sTtE", "libSceRudp", 1, "libSceRudp", sceRudpFlush);
    LIB_FUNCTION("wIJsiqY+BMk", "libSceRudp", 1, "libSceRudp", sceRudpGetContextStatus);
    LIB_FUNCTION("2G7-vVz9SIg", "libSceRudp", 1, "libSceRudp", sceRudpGetLocalInfo);
    LIB_FUNCTION("vfrL8gPlm2Y", "libSceRudp", 1, "libSceRudp", sceRudpGetMaxSegmentSize);
    LIB_FUNCTION("Px0miD2LuW0", "libSceRudp", 1, "libSceRudp", sceRudpGetNumberOfPacketsToRead);
    LIB_FUNCTION("mCQIhSmCP6o", "libSceRudp", 1, "libSceRudp", sceRudpGetOption);
    LIB_FUNCTION("Qignjmfgha0", "libSceRudp", 1, "libSceRudp", sceRudpGetRemoteInfo);
    LIB_FUNCTION("sAZqO2+5Qqo", "libSceRudp", 1, "libSceRudp", sceRudpGetSizeReadable);
    LIB_FUNCTION("fRc1ahQppR4", "libSceRudp", 1, "libSceRudp", sceRudpGetSizeWritable);
    LIB_FUNCTION("i3STzxuwPx0", "libSceRudp", 1, "libSceRudp", sceRudpGetStatus);
    LIB_FUNCTION("amuBfI-AQc4", "libSceRudp", 1, "libSceRudp", sceRudpInit);
    LIB_FUNCTION("szEVu+edXV4", "libSceRudp", 1, "libSceRudp", sceRudpInitiate);
    LIB_FUNCTION("tYVWcWDnctE", "libSceRudp", 1, "libSceRudp", sceRudpListen);
    LIB_FUNCTION("+BJ9svDmjYs", "libSceRudp", 1, "libSceRudp", sceRudpNetFlush);
    LIB_FUNCTION("vPzJldDSxXc", "libSceRudp", 1, "libSceRudp", sceRudpNetReceived);
    LIB_FUNCTION("yzeXuww-UWg", "libSceRudp", 1, "libSceRudp", sceRudpPollCancel);
    LIB_FUNCTION("haMpc7TFx0A", "libSceRudp", 1, "libSceRudp", sceRudpPollControl);
    LIB_FUNCTION("MVbmLASjn5M", "libSceRudp", 1, "libSceRudp", sceRudpPollCreate);
    LIB_FUNCTION("LjwbHpEeW0A", "libSceRudp", 1, "libSceRudp", sceRudpPollDestroy);
    LIB_FUNCTION("M6ggviwXpLs", "libSceRudp", 1, "libSceRudp", sceRudpPollWait);
    LIB_FUNCTION("9U9m1YH0ScQ", "libSceRudp", 1, "libSceRudp", sceRudpProcessEvents);
    LIB_FUNCTION("rZqWV3eXgOA", "libSceRudp", 1, "libSceRudp", sceRudpRead);
    LIB_FUNCTION("SUEVes8gvmw", "libSceRudp", 1, "libSceRudp", sceRudpSetEventHandler);
    LIB_FUNCTION("beAsSTVWVPQ", "libSceRudp", 1, "libSceRudp", sceRudpSetMaxSegmentSize);
    LIB_FUNCTION("0yzYdZf0IwE", "libSceRudp", 1, "libSceRudp", sceRudpSetOption);
    LIB_FUNCTION("OMYRTU0uc4w", "libSceRudp", 1, "libSceRudp", sceRudpTerminate);
    LIB_FUNCTION("KaPL3fbTLCA", "libSceRudp", 1, "libSceRudp", sceRudpWrite);
};

} // namespace Libraries::Rudp
