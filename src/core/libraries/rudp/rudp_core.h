// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Reliable, ordered, message-oriented transport used to implement libSceRudp.
//
// This file has no shadPS4 dependencies so the protocol can be unit tested on its own.
// The wire format is private to shadPS4: both ends of a connection must run this code.
// It is not compatible with Sony's RUDP on a real console.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <vector>

namespace Libraries::Rudp::Proto {

// Error codes returned to the guest (same values as the real library).
enum Error : int32_t {
    OK = 0,
    ERROR_NOT_INITIALIZED = static_cast<int32_t>(0x80770001),
    ERROR_ALREADY_INITIALIZED = static_cast<int32_t>(0x80770002),
    ERROR_INVALID_CONTEXT_ID = static_cast<int32_t>(0x80770003),
    ERROR_INVALID_ARGUMENT = static_cast<int32_t>(0x80770004),
    ERROR_INVALID_OPTION = static_cast<int32_t>(0x80770005),
    ERROR_INVALID_MUXMODE = static_cast<int32_t>(0x80770006),
    ERROR_MEMORY = static_cast<int32_t>(0x80770007),
    ERROR_INTERNAL = static_cast<int32_t>(0x80770008),
    ERROR_CONN_RESET = static_cast<int32_t>(0x80770009),
    ERROR_CONN_REFUSED = static_cast<int32_t>(0x8077000a),
    ERROR_CONN_TIMEOUT = static_cast<int32_t>(0x8077000b),
    ERROR_THREAD = static_cast<int32_t>(0x8077000f),
    ERROR_THREAD_IN_USE = static_cast<int32_t>(0x80770010),
    ERROR_NOT_ACCEPTABLE = static_cast<int32_t>(0x80770011),
    ERROR_MSG_TOO_LARGE = static_cast<int32_t>(0x80770012),
    ERROR_NOT_BOUND = static_cast<int32_t>(0x80770013),
    ERROR_INVALID_VPORT = static_cast<int32_t>(0x80770015),
    ERROR_WOULDBLOCK = static_cast<int32_t>(0x80770016),
    ERROR_INVALID_SOCKET = static_cast<int32_t>(0x80770019),
    ERROR_BUFFER_TOO_SMALL = static_cast<int32_t>(0x8077001a),
    ERROR_ALREADY_BOUND = static_cast<int32_t>(0x8077001d),
    ERROR_TOO_MANY_CONTEXTS = static_cast<int32_t>(0x80770020),
    ERROR_IN_PROGRESS = static_cast<int32_t>(0x80770021),
    ERROR_NO_EVENT_HANDLER = static_cast<int32_t>(0x80770022),
    ERROR_END_OF_DATA = static_cast<int32_t>(0x80770024),
    ERROR_ALREADY_ESTABLISHED = static_cast<int32_t>(0x80770025),
    ERROR_KEEP_ALIVE_FAILURE = static_cast<int32_t>(0x80770026),
};

// Context events passed to the per-context handler.
enum ContextEvent : int32_t {
    CONTEXT_EVENT_CLOSED = 1,
    CONTEXT_EVENT_ESTABLISHED = 2,
    CONTEXT_EVENT_ERROR = 3,
    CONTEXT_EVENT_WRITABLE = 4,
    CONTEXT_EVENT_READABLE = 5,
    CONTEXT_EVENT_FLUSHED = 6,
};

// Events passed to the global handler set by sceRudpSetEventHandler.
enum GlobalEvent : int32_t {
    EVENT_SEND_BLOCKED = 1,
    EVENT_SOCKET_RELEASED = 2,
};

enum Option : int32_t {
    OPTION_MAX_PAYLOAD = 1,
    OPTION_SNDBUF = 2,
    OPTION_RCVBUF = 3,
    OPTION_NODELAY = 4,
    OPTION_DELIVERY_CRITICAL = 5,
    OPTION_ORDER_CRITICAL = 6,
    OPTION_NONBLOCK = 7,
    OPTION_STREAM = 8,
    OPTION_CONNECTION_TIMEOUT = 9,
    OPTION_CLOSE_WAIT_TIMEOUT = 10,
    OPTION_AGGREGATION_TIMEOUT = 11,
    OPTION_LAST_ERROR = 14,
    OPTION_READ_TIMEOUT = 15,
    OPTION_WRITE_TIMEOUT = 16,
    OPTION_FLUSH_TIMEOUT = 17,
    OPTION_KEEP_ALIVE_INTERVAL = 18,
    OPTION_KEEP_ALIVE_TIMEOUT = 19,
};

enum class State : int32_t {
    Idle = 0,
    Connecting = 1,
    Established = 2,
    Closed = 3,
    Error = 5,
};

// Remote address as seen on the guest P2P socket. Values are kept exactly as they appear in
// the guest sockaddr (network byte order) and are only compared, never interpreted.
struct PeerKey {
    uint32_t addr = 0;
    uint16_t port = 0;
    uint16_t vport = 0;

    bool operator==(const PeerKey& o) const {
        return addr == o.addr && port == o.port && vport == o.vport;
    }
    bool operator!=(const PeerKey& o) const {
        return !(*this == o);
    }
};

// Something the engine wants the caller to do once the engine lock is released.
struct Action {
    enum class Kind { Send, ContextEvent, SocketReleased };
    Kind kind = Kind::Send;
    int32_t ctx_id = -1;         // ContextEvent
    int32_t event = 0;           // ContextEvent
    int32_t error = 0;           // ContextEvent
    int32_t socket = -1;         // Send / SocketReleased
    PeerKey to{};                // Send
    std::vector<uint8_t> data{}; // Send
};

using Millis = uint64_t;

// Wire constants.
constexpr uint16_t kMagic = 0x5255; // "RU"
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 28;
constexpr size_t kMaxSegmentPayload = 1200;
constexpr size_t kMaxMessage = 64 * 1024;

enum PacketType : uint8_t {
    PKT_SYN = 1,
    PKT_SYNACK = 2,
    PKT_DATA = 3,
    PKT_ACK = 4,
    PKT_FIN = 5,
    PKT_PING = 6,
};

struct Header {
    uint8_t type = 0;
    uint32_t src_id = 0;
    uint32_t dst_id = 0;
    uint32_t seq = 0;
    uint32_t ack = 0;  // next sequence number expected by the sender of this packet
    uint32_t sack = 0; // bit i set = segment ack + 1 + i already received
    uint8_t frag = 0;  // bit 0 = first segment of a message, bit 1 = last
    uint16_t len = 0;
};

constexpr uint8_t kFragFirst = 0x1;
constexpr uint8_t kFragLast = 0x2;

std::vector<uint8_t> EncodePacket(const Header& h, const uint8_t* payload, size_t len);
bool DecodePacket(const uint8_t* data, size_t len, Header* h, const uint8_t** payload);

class Engine {
public:
    struct Tuning {
        Millis syn_interval = 200;
        Millis keepalive_interval = 1000;
        Millis keepalive_timeout = 15000;
        Millis connect_timeout = 30000;
        Millis min_rto = 60;
        Millis max_rto = 2000;
        Millis initial_rto = 250;
        size_t window_segments = 256;
        size_t send_buffer = 8 * 1024 * 1024;
        size_t recv_window_segments = 4096;
        int fin_repeats = 3;
    };

    explicit Engine(uint64_t seed = std::random_device{}());

    Tuning& tuning() {
        return tuning_;
    }

    // Guest API. Each returns OK/positive or a negative Error. Events and packets produced
    // by the call are appended to the action list fetched with TakeActions().
    int32_t CreateContext();
    int32_t SetOption(int32_t ctx, int32_t option, const void* value, size_t len);
    int32_t GetOption(int32_t ctx, int32_t option, void* value, size_t len) const;
    int32_t Bind(int32_t ctx, int32_t socket, uint16_t vport, uint8_t mux_mode);
    int32_t Initiate(int32_t ctx, const PeerKey& peer, Millis now);
    int32_t Write(int32_t ctx, const uint8_t* data, size_t len, Millis now);
    int32_t Read(int32_t ctx, uint8_t* buf, size_t len);
    int32_t GetSizeReadable(int32_t ctx) const;
    int32_t GetNumberOfPacketsToRead(int32_t ctx) const;
    int32_t GetSizeWritable(int32_t ctx) const;
    int32_t GetState(int32_t ctx, State* state, int32_t* last_error) const;
    int32_t GetRemote(int32_t ctx, PeerKey* peer) const;
    int32_t Flush(int32_t ctx);
    int32_t Terminate(int32_t ctx, Millis now);

    // Transport side.
    void OnPacket(int32_t socket, const PeerKey& from, const uint8_t* data, size_t len, Millis now);
    void Tick(Millis now);

    std::set<int32_t> BoundSockets() const;
    bool HasContext(int32_t ctx) const;
    size_t ContextCount() const {
        return contexts_.size();
    }
    std::vector<Action> TakeActions();

private:
    struct Segment {
        uint32_t seq = 0;
        uint8_t frag = 0;
        std::vector<uint8_t> data;
        Millis first_sent = 0;
        Millis last_sent = 0;
        int tx_count = 0;
    };

    struct Context {
        int32_t id = -1;
        uint32_t local_cid = 0;
        uint32_t remote_cid = 0;
        int32_t socket = -1;
        uint16_t vport = 0;
        uint8_t mux_mode = 0;
        PeerKey peer{};
        bool has_peer = false;
        State state = State::Idle;
        int32_t last_error = 0;

        // Options.
        Millis connect_timeout = 0;
        Millis keepalive_interval = 0;
        Millis keepalive_timeout = 0;
        size_t send_buffer = 0;
        int32_t raw_options[20]{};

        // Sending.
        uint32_t next_seq = 1;
        std::deque<Segment> queued;
        std::map<uint32_t, Segment> inflight;
        size_t queued_bytes = 0;
        uint32_t highest_sacked = 0;
        bool flush_requested = false;

        // Receiving.
        uint32_t rcv_next = 1;
        std::map<uint32_t, Segment> out_of_order;
        std::vector<uint8_t> partial;
        bool in_message = false;
        std::deque<std::vector<uint8_t>> messages;
        size_t readable_bytes = 0;
        bool ack_pending = false;

        // RTT estimate (milliseconds).
        double srtt = 0;
        double rttvar = 0;
        Millis rto = 0;
        bool have_rtt = false;

        // Timers.
        Millis connect_start = 0;
        Millis last_syn = 0;
        Millis last_rx = 0;
        Millis last_tx = 0;
    };

    struct Closing {
        int32_t socket;
        PeerKey peer;
        uint32_t local_cid;
        uint32_t remote_cid;
        int repeats_left;
    };

    Context* Find(int32_t ctx);
    const Context* Find(int32_t ctx) const;
    Context* Demux(int32_t socket, const PeerKey& from, const Header& h);

    void Send(Context& c, const Header& h, const uint8_t* payload, size_t len, Millis now);
    void SendControl(Context& c, uint8_t type, Millis now);
    void SendData(Context& c, Segment& s, Millis now);
    void Establish(Context& c);
    void Fail(Context& c, int32_t error);
    void Emit(const Context& c, int32_t event, int32_t error = 0);
    void ProcessAck(Context& c, uint32_t ack, uint32_t sack, Millis now);
    void ProcessData(Context& c, const Header& h, const uint8_t* payload);
    void Deliver(Context& c, Segment&& s);
    void FillAck(const Context& c, Header& h) const;
    void TickContext(Context& c, Millis now);
    void MaybeReleaseSocket(int32_t socket);
    uint32_t NewConnectionId();

    Tuning tuning_;
    std::mt19937_64 rng_;
    int32_t next_ctx_id_ = 1;
    std::map<int32_t, std::unique_ptr<Context>> contexts_;
    std::vector<Closing> closing_;
    std::set<int32_t> sockets_in_use_;
    std::vector<Action> actions_;
};

} // namespace Libraries::Rudp::Proto
