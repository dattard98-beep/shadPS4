// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/libraries/rudp/rudp_core.h"

namespace Libraries::Rudp::Proto {

namespace {

void Put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

uint16_t Get16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t Get32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Wrap-safe sequence comparison.
bool SeqLess(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) < 0;
}

} // namespace

std::vector<uint8_t> EncodePacket(const Header& h, const uint8_t* payload, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + len);
    Put16(out, kMagic);
    out.push_back(kVersion);
    out.push_back(h.type);
    Put32(out, h.src_id);
    Put32(out, h.dst_id);
    Put32(out, h.seq);
    Put32(out, h.ack);
    Put32(out, h.sack);
    out.push_back(h.frag);
    out.push_back(0);
    Put16(out, static_cast<uint16_t>(len));
    if (len != 0) {
        out.insert(out.end(), payload, payload + len);
    }
    return out;
}

bool DecodePacket(const uint8_t* data, size_t len, Header* h, const uint8_t** payload) {
    if (data == nullptr || len < kHeaderSize) {
        return false;
    }
    if (Get16(data) != kMagic || data[2] != kVersion) {
        return false;
    }
    h->type = data[3];
    h->src_id = Get32(data + 4);
    h->dst_id = Get32(data + 8);
    h->seq = Get32(data + 12);
    h->ack = Get32(data + 16);
    h->sack = Get32(data + 20);
    h->frag = data[24];
    h->len = Get16(data + 26);
    if (h->type < PKT_SYN || h->type > PKT_PING) {
        return false;
    }
    if (kHeaderSize + h->len != len || h->len > kMaxSegmentPayload) {
        return false;
    }
    *payload = data + kHeaderSize;
    return true;
}

Engine::Engine(uint64_t seed) : rng_(seed) {}

uint32_t Engine::NewConnectionId() {
    for (;;) {
        const uint32_t id = static_cast<uint32_t>(rng_());
        if (id == 0) {
            continue;
        }
        bool used = false;
        for (const auto& [_, c] : contexts_) {
            used |= c->local_cid == id;
        }
        if (!used) {
            return id;
        }
    }
}

Engine::Context* Engine::Find(int32_t ctx) {
    const auto it = contexts_.find(ctx);
    return it == contexts_.end() ? nullptr : it->second.get();
}

const Engine::Context* Engine::Find(int32_t ctx) const {
    const auto it = contexts_.find(ctx);
    return it == contexts_.end() ? nullptr : it->second.get();
}

bool Engine::HasContext(int32_t ctx) const {
    return Find(ctx) != nullptr;
}

std::set<int32_t> Engine::BoundSockets() const {
    return sockets_in_use_;
}

std::vector<Action> Engine::TakeActions() {
    std::vector<Action> out;
    out.swap(actions_);
    return out;
}

void Engine::Emit(const Context& c, int32_t event, int32_t error) {
    Action a{Action::Kind::ContextEvent};
    a.ctx_id = c.id;
    a.event = event;
    a.error = error;
    actions_.push_back(std::move(a));
}

int32_t Engine::CreateContext() {
    if (contexts_.size() >= 1024) {
        return ERROR_TOO_MANY_CONTEXTS;
    }
    auto c = std::make_unique<Context>();
    c->id = next_ctx_id_++;
    if (next_ctx_id_ <= 0) {
        next_ctx_id_ = 1;
    }
    c->local_cid = NewConnectionId();
    c->connect_timeout = tuning_.connect_timeout;
    c->keepalive_interval = tuning_.keepalive_interval;
    c->keepalive_timeout = tuning_.keepalive_timeout;
    c->send_buffer = tuning_.send_buffer;
    c->rto = tuning_.initial_rto;
    const int32_t id = c->id;
    contexts_[id] = std::move(c);
    return id;
}

int32_t Engine::SetOption(int32_t ctx, int32_t option, const void* value, size_t len) {
    Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    if (option <= 0 || option >= 20 || option == 12 || option == 13 ||
        option == OPTION_LAST_ERROR) {
        return ERROR_INVALID_OPTION;
    }
    if (value == nullptr || len < sizeof(int32_t)) {
        return ERROR_INVALID_ARGUMENT;
    }
    int32_t v = 0;
    std::memcpy(&v, value, sizeof(v));
    c->raw_options[option] = v;
    switch (option) {
    case OPTION_CONNECTION_TIMEOUT:
        c->connect_timeout = v > 0 ? static_cast<Millis>(v) : tuning_.connect_timeout;
        break;
    case OPTION_KEEP_ALIVE_INTERVAL:
        c->keepalive_interval = v > 0 ? static_cast<Millis>(v) : tuning_.keepalive_interval;
        break;
    case OPTION_KEEP_ALIVE_TIMEOUT:
        c->keepalive_timeout = v > 0 ? static_cast<Millis>(v) : tuning_.keepalive_timeout;
        break;
    case OPTION_SNDBUF:
        // The guest silently drops writes that would block, so never shrink below our default.
        c->send_buffer = std::max(tuning_.send_buffer, static_cast<size_t>(v > 0 ? v : 0));
        break;
    default:
        // Everything else is accepted and stored. Delivery is always reliable and ordered,
        // which satisfies every combination of the DELIVERY/ORDER_CRITICAL flags.
        break;
    }
    return OK;
}

int32_t Engine::GetOption(int32_t ctx, int32_t option, void* value, size_t len) const {
    const Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    if (option <= 0 || option >= 20) {
        return ERROR_INVALID_OPTION;
    }
    if (value == nullptr || len < sizeof(int32_t)) {
        return ERROR_INVALID_ARGUMENT;
    }
    int32_t v = c->raw_options[option];
    switch (option) {
    case OPTION_MAX_PAYLOAD:
        v = static_cast<int32_t>(kMaxSegmentPayload);
        break;
    case OPTION_CONNECTION_TIMEOUT:
        v = static_cast<int32_t>(c->connect_timeout);
        break;
    case OPTION_KEEP_ALIVE_INTERVAL:
        v = static_cast<int32_t>(c->keepalive_interval);
        break;
    case OPTION_KEEP_ALIVE_TIMEOUT:
        v = static_cast<int32_t>(c->keepalive_timeout);
        break;
    case OPTION_SNDBUF:
        v = static_cast<int32_t>(c->send_buffer);
        break;
    case OPTION_LAST_ERROR:
        v = c->last_error;
        break;
    default:
        break;
    }
    std::memcpy(value, &v, sizeof(v));
    return OK;
}

int32_t Engine::Bind(int32_t ctx, int32_t socket, uint16_t vport, uint8_t mux_mode) {
    Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    if (socket < 0) {
        return ERROR_INVALID_SOCKET;
    }
    if (c->socket >= 0) {
        return ERROR_ALREADY_BOUND;
    }
    c->socket = socket;
    c->vport = vport;
    c->mux_mode = mux_mode;
    sockets_in_use_.insert(socket);
    return OK;
}

int32_t Engine::Initiate(int32_t ctx, const PeerKey& peer, Millis now) {
    Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    if (c->socket < 0) {
        return ERROR_NOT_BOUND;
    }
    if (c->state == State::Established) {
        return ERROR_ALREADY_ESTABLISHED;
    }
    if (c->state == State::Connecting) {
        return ERROR_IN_PROGRESS;
    }
    if (c->state != State::Idle) {
        return ERROR_NOT_ACCEPTABLE;
    }
    c->peer = peer;
    c->has_peer = true;
    c->state = State::Connecting;
    c->connect_start = now;
    c->last_rx = now;
    SendControl(*c, PKT_SYN, now);
    c->last_syn = now;
    return OK;
}

int32_t Engine::Write(int32_t ctx, const uint8_t* data, size_t len, Millis now) {
    Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    if (c->state == State::Error) {
        return c->last_error != 0 ? c->last_error : ERROR_CONN_RESET;
    }
    if (c->state == State::Closed) {
        return ERROR_CONN_RESET;
    }
    if (c->state != State::Established) {
        return ERROR_NOT_ACCEPTABLE;
    }
    if (len == 0 || data == nullptr) {
        return ERROR_INVALID_ARGUMENT;
    }
    if (len > kMaxMessage) {
        return ERROR_MSG_TOO_LARGE;
    }
    if (c->queued_bytes + len > c->send_buffer) {
        return ERROR_WOULDBLOCK;
    }
    size_t offset = 0;
    while (offset < len) {
        const size_t n = std::min(kMaxSegmentPayload, len - offset);
        Segment s;
        s.seq = c->next_seq++;
        s.frag = (offset == 0 ? kFragFirst : 0) | (offset + n == len ? kFragLast : 0);
        s.data.assign(data + offset, data + offset + n);
        c->queued.push_back(std::move(s));
        offset += n;
    }
    c->queued_bytes += len;
    // Send immediately when the window allows; anything left goes out on the next tick.
    while (!c->queued.empty() && c->inflight.size() < tuning_.window_segments) {
        Segment s = std::move(c->queued.front());
        c->queued.pop_front();
        auto& slot = c->inflight[s.seq];
        slot = std::move(s);
        SendData(*c, slot, now);
    }
    return static_cast<int32_t>(len);
}

int32_t Engine::Read(int32_t ctx, uint8_t* buf, size_t len) {
    Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    if (c->messages.empty()) {
        if (c->state == State::Closed) {
            return ERROR_END_OF_DATA;
        }
        if (c->state == State::Error) {
            return c->last_error != 0 ? c->last_error : ERROR_CONN_RESET;
        }
        return ERROR_WOULDBLOCK;
    }
    if (buf == nullptr && len != 0) {
        return ERROR_INVALID_ARGUMENT;
    }
    std::vector<uint8_t> msg = std::move(c->messages.front());
    c->messages.pop_front();
    c->readable_bytes -= msg.size();
    const size_t n = std::min(len, msg.size());
    if (n != 0) {
        std::memcpy(buf, msg.data(), n);
    }
    return static_cast<int32_t>(n);
}

int32_t Engine::GetSizeReadable(int32_t ctx) const {
    const Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    return static_cast<int32_t>(std::min<size_t>(c->readable_bytes, INT32_MAX));
}

int32_t Engine::GetNumberOfPacketsToRead(int32_t ctx) const {
    const Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    return static_cast<int32_t>(std::min<size_t>(c->messages.size(), INT32_MAX));
}

int32_t Engine::GetSizeWritable(int32_t ctx) const {
    const Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    const size_t free = c->send_buffer > c->queued_bytes ? c->send_buffer - c->queued_bytes : 0;
    return static_cast<int32_t>(std::min<size_t>(free, INT32_MAX));
}

int32_t Engine::GetState(int32_t ctx, State* state, int32_t* last_error) const {
    const Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    if (state != nullptr) {
        *state = c->state;
    }
    if (last_error != nullptr) {
        *last_error = c->last_error;
    }
    return OK;
}

int32_t Engine::GetRemote(int32_t ctx, PeerKey* peer) const {
    const Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    if (!c->has_peer) {
        return ERROR_NOT_ACCEPTABLE;
    }
    *peer = c->peer;
    return OK;
}

int32_t Engine::Flush(int32_t ctx) {
    Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    if (c->state != State::Established) {
        return ERROR_NOT_ACCEPTABLE;
    }
    // FLUSHED is reported from Tick() once every written byte has been acknowledged.
    c->flush_requested = true;
    return OK;
}

int32_t Engine::Terminate(int32_t ctx, Millis now) {
    Context* c = Find(ctx);
    if (c == nullptr) {
        return ERROR_INVALID_CONTEXT_ID;
    }
    const int32_t socket = c->socket;
    if (socket >= 0 && c->has_peer && c->remote_cid != 0 &&
        (c->state == State::Established || c->state == State::Connecting)) {
        SendControl(*c, PKT_FIN, now);
        closing_.push_back({socket, c->peer, c->local_cid, c->remote_cid, tuning_.fin_repeats - 1});
    }
    contexts_.erase(ctx);
    if (socket >= 0) {
        MaybeReleaseSocket(socket);
    }
    return OK;
}

void Engine::MaybeReleaseSocket(int32_t socket) {
    for (const auto& [_, c] : contexts_) {
        if (c->socket == socket) {
            return;
        }
    }
    if (sockets_in_use_.erase(socket) != 0) {
        Action a{Action::Kind::SocketReleased};
        a.socket = socket;
        actions_.push_back(std::move(a));
    }
}

void Engine::FillAck(const Context& c, Header& h) const {
    h.ack = c.rcv_next;
    uint32_t sack = 0;
    for (const auto& [seq, _] : c.out_of_order) {
        const uint32_t bit = seq - c.rcv_next - 1;
        if (bit < 32) {
            sack |= 1u << bit;
        }
    }
    h.sack = sack;
}

void Engine::Send(Context& c, const Header& h, const uint8_t* payload, size_t len, Millis now) {
    if (c.socket < 0 || !c.has_peer) {
        return;
    }
    Action a{Action::Kind::Send};
    a.socket = c.socket;
    a.to = c.peer;
    a.data = EncodePacket(h, payload, len);
    actions_.push_back(std::move(a));
    c.last_tx = now;
}

void Engine::SendControl(Context& c, uint8_t type, Millis now) {
    Header h;
    h.type = type;
    h.src_id = c.local_cid;
    h.dst_id = type == PKT_SYN ? 0 : c.remote_cid;
    FillAck(c, h);
    Send(c, h, nullptr, 0, now);
    if (type == PKT_ACK || type == PKT_PING) {
        c.ack_pending = false;
    }
}

void Engine::SendData(Context& c, Segment& s, Millis now) {
    Header h;
    h.type = PKT_DATA;
    h.src_id = c.local_cid;
    h.dst_id = c.remote_cid;
    h.seq = s.seq;
    h.frag = s.frag;
    FillAck(c, h);
    if (s.tx_count == 0) {
        s.first_sent = now;
    }
    s.last_sent = now;
    s.tx_count++;
    Send(c, h, s.data.data(), s.data.size(), now);
    c.ack_pending = false; // the data packet carries our acknowledgement
}

void Engine::Establish(Context& c) {
    if (c.state != State::Connecting) {
        return;
    }
    c.state = State::Established;
    Emit(c, CONTEXT_EVENT_ESTABLISHED);
    Emit(c, CONTEXT_EVENT_WRITABLE);
}

void Engine::Fail(Context& c, int32_t error) {
    if (c.state == State::Error || c.state == State::Closed) {
        return;
    }
    c.state = State::Error;
    c.last_error = error;
    c.queued.clear();
    c.inflight.clear();
    c.queued_bytes = 0;
    c.flush_requested = false;
    Emit(c, CONTEXT_EVENT_ERROR, error);
}

Engine::Context* Engine::Demux(int32_t socket, const PeerKey& from, const Header& h) {
    if (h.dst_id != 0) {
        for (auto& [_, c] : contexts_) {
            if (c->socket == socket && c->local_cid == h.dst_id) {
                return c.get();
            }
        }
        return nullptr;
    }
    // SYN: match on the address the guest gave to sceRudpInitiate.
    Context* candidate = nullptr;
    int unmatched = 0;
    for (auto& [_, c] : contexts_) {
        if (c->socket != socket || !c->has_peer) {
            continue;
        }
        if (c->peer == from) {
            return c.get();
        }
        if (c->state == State::Connecting && c->remote_cid == 0) {
            candidate = c.get();
            unmatched++;
        }
    }
    // The peer's packets can arrive from a different address than the one signaling reported
    // (NAT, LAN vs public address). Adopt it only when the choice is unambiguous.
    if (unmatched == 1) {
        return candidate;
    }
    return nullptr;
}

void Engine::OnPacket(int32_t socket, const PeerKey& from, const uint8_t* data, size_t len,
                      Millis now) {
    Header h;
    const uint8_t* payload = nullptr;
    if (!DecodePacket(data, len, &h, &payload)) {
        return;
    }
    Context* c = Demux(socket, from, h);
    if (c == nullptr) {
        return;
    }
    if (c->state == State::Idle || c->state == State::Closed || c->state == State::Error) {
        return;
    }
    // A known peer that restarted its side (new connection id) resets the connection.
    if (c->remote_cid != 0 && h.src_id != c->remote_cid) {
        if (c->state == State::Established && h.type == PKT_SYN) {
            Fail(*c, ERROR_CONN_RESET);
        }
        return;
    }
    c->peer = from;
    c->last_rx = now;

    switch (h.type) {
    case PKT_SYN:
        c->remote_cid = h.src_id;
        SendControl(*c, PKT_SYNACK, now);
        Establish(*c);
        break;
    case PKT_SYNACK:
        c->remote_cid = h.src_id;
        Establish(*c);
        ProcessAck(*c, h.ack, h.sack, now);
        break;
    case PKT_DATA:
        c->remote_cid = h.src_id;
        Establish(*c);
        ProcessAck(*c, h.ack, h.sack, now);
        ProcessData(*c, h, payload);
        break;
    case PKT_ACK:
        if (c->state == State::Connecting) {
            c->remote_cid = h.src_id;
            Establish(*c);
        }
        ProcessAck(*c, h.ack, h.sack, now);
        break;
    case PKT_PING:
        ProcessAck(*c, h.ack, h.sack, now);
        c->ack_pending = true;
        break;
    case PKT_FIN:
        if (c->state == State::Established || c->state == State::Connecting) {
            c->state = State::Closed;
            c->queued.clear();
            c->inflight.clear();
            c->queued_bytes = 0;
            c->flush_requested = false;
            Emit(*c, CONTEXT_EVENT_CLOSED, 0);
        }
        break;
    default:
        break;
    }
}

void Engine::ProcessAck(Context& c, uint32_t ack, uint32_t sack, Millis now) {
    if (c.state != State::Established) {
        return;
    }
    const auto sample = [&](const Segment& s) {
        if (s.tx_count != 1 || now < s.last_sent) {
            return; // Karn's rule: skip retransmitted segments
        }
        const double r = static_cast<double>(now - s.last_sent);
        if (!c.have_rtt) {
            c.srtt = r;
            c.rttvar = r / 2;
            c.have_rtt = true;
        } else {
            c.rttvar = 0.75 * c.rttvar + 0.25 * std::fabs(c.srtt - r);
            c.srtt = 0.875 * c.srtt + 0.125 * r;
        }
        const double rto = c.srtt + std::max(4 * c.rttvar, 10.0);
        c.rto = std::clamp(static_cast<Millis>(rto), tuning_.min_rto, tuning_.max_rto);
    };
    size_t freed = 0;
    for (auto it = c.inflight.begin(); it != c.inflight.end();) {
        const uint32_t seq = it->first;
        bool acked = SeqLess(seq, ack);
        if (!acked && sack != 0) {
            const uint32_t bit = seq - ack - 1;
            acked = bit < 32 && (sack & (1u << bit)) != 0;
            if (acked && SeqLess(c.highest_sacked, seq)) {
                c.highest_sacked = seq;
            }
        }
        if (acked) {
            sample(it->second);
            freed += it->second.data.size();
            it = c.inflight.erase(it);
        } else {
            ++it;
        }
    }
    c.queued_bytes -= std::min(c.queued_bytes, freed);
}

void Engine::ProcessData(Context& c, const Header& h, const uint8_t* payload) {
    if (c.state != State::Established) {
        return;
    }
    c.ack_pending = true;
    if (SeqLess(h.seq, c.rcv_next)) {
        return; // duplicate; the ack we send tells the peer to stop retransmitting it
    }
    if (h.seq - c.rcv_next >= tuning_.recv_window_segments) {
        return;
    }
    Segment s;
    s.seq = h.seq;
    s.frag = h.frag;
    s.data.assign(payload, payload + h.len);
    if (h.seq != c.rcv_next) {
        c.out_of_order.emplace(h.seq, std::move(s));
        return;
    }
    Deliver(c, std::move(s));
    for (auto it = c.out_of_order.find(c.rcv_next); it != c.out_of_order.end();
         it = c.out_of_order.find(c.rcv_next)) {
        Segment next = std::move(it->second);
        c.out_of_order.erase(it);
        Deliver(c, std::move(next));
    }
}

void Engine::Deliver(Context& c, Segment&& s) {
    c.rcv_next = s.seq + 1;
    if (s.frag & kFragFirst) {
        c.partial.clear();
        c.in_message = true;
    }
    if (!c.in_message) {
        return; // tail of a message whose start we never saw; cannot happen with ordering
    }
    c.partial.insert(c.partial.end(), s.data.begin(), s.data.end());
    if (s.frag & kFragLast) {
        const bool was_empty = c.messages.empty();
        c.readable_bytes += c.partial.size();
        c.messages.push_back(std::move(c.partial));
        c.partial.clear();
        c.in_message = false;
        if (was_empty) {
            Emit(c, CONTEXT_EVENT_READABLE);
        }
    }
}

void Engine::TickContext(Context& c, Millis now) {
    if (c.state == State::Connecting) {
        if (now - c.connect_start >= c.connect_timeout) {
            Fail(c, ERROR_CONN_TIMEOUT);
            return;
        }
        if (now - c.last_syn >= tuning_.syn_interval) {
            SendControl(c, PKT_SYN, now);
            c.last_syn = now;
        }
        return;
    }
    if (c.state != State::Established) {
        return;
    }
    if (now - c.last_rx >= c.keepalive_timeout) {
        Fail(c, ERROR_KEEP_ALIVE_FAILURE);
        return;
    }
    // Retransmit what has timed out, or what the peer reported missing.
    for (auto& [seq, s] : c.inflight) {
        const int backoff = std::min(s.tx_count - 1, 5);
        const Millis rto = std::min<Millis>(c.rto << backoff, tuning_.max_rto);
        const bool timed_out = now - s.last_sent >= rto;
        const Millis fast_gap = std::max<Millis>(static_cast<Millis>(c.srtt), 20);
        const bool hole = SeqLess(seq, c.highest_sacked) && now - s.last_sent >= fast_gap;
        if (timed_out || hole) {
            SendData(c, s, now);
        }
    }
    // Fill the window with new data.
    while (!c.queued.empty() && c.inflight.size() < tuning_.window_segments) {
        Segment s = std::move(c.queued.front());
        c.queued.pop_front();
        auto& slot = c.inflight[s.seq];
        slot = std::move(s);
        SendData(c, slot, now);
    }
    if (c.ack_pending) {
        SendControl(c, PKT_ACK, now);
    } else if (now - c.last_tx >= c.keepalive_interval) {
        SendControl(c, PKT_PING, now);
    }
    if (c.flush_requested && c.inflight.empty() && c.queued.empty()) {
        c.flush_requested = false;
        Emit(c, CONTEXT_EVENT_FLUSHED);
    }
}

void Engine::Tick(Millis now) {
    for (auto& [_, c] : contexts_) {
        TickContext(*c, now);
    }
    for (auto it = closing_.begin(); it != closing_.end();) {
        if (it->repeats_left <= 0) {
            it = closing_.erase(it);
            continue;
        }
        Header h;
        h.type = PKT_FIN;
        h.src_id = it->local_cid;
        h.dst_id = it->remote_cid;
        Action a{Action::Kind::Send};
        a.socket = it->socket;
        a.to = it->peer;
        a.data = EncodePacket(h, nullptr, 0);
        actions_.push_back(std::move(a));
        it->repeats_left--;
        ++it;
    }
}

} // namespace Libraries::Rudp::Proto
