// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <system_error>

#include "core/godzilla/mod_sync.h"
#include "core/godzilla/pac.h"
#include "core/libraries/rudp/rudp_core.h"

namespace Core::Godzilla::ModSync {

namespace fs = std::filesystem;
namespace Proto = Libraries::Rudp::Proto;

namespace {

using Clock = std::chrono::steady_clock;

constexpr u16 kVport = 0x4d53; // "MS"
constexpr int32_t kSocket = 1;
constexpr u8 kProtocolVersion = 1;
constexpr size_t kChunk = 48 * 1024;
constexpr u32 kMaxEntryBytes = 64u << 20;
constexpr u32 kMaxWholeBytes = 96u << 20;
constexpr u64 kMaxPeerBytes = 160ull << 20;
constexpr Proto::Millis kNoManifestGiveUp = 12'000;
constexpr Proto::Millis kHoldLimit = 120'000;

enum FileKind : u8 { kCharFile = 0, kGameFile = 1 };
enum MsgType : u8 { kMsgManifest = 'M', kMsgRequest = 'R', kMsgData = 'D', kMsgDone = 'Z' };
const std::string kWholeName = "*";

Proto::Millis NowMs() {
    return static_cast<Proto::Millis>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
            .count());
}

std::string Hex2(u8 v) {
    char b[4];
    std::snprintf(b, sizeof(b), "%02u", static_cast<unsigned>(v));
    return b;
}

std::string MonsterName(u8 ch) {
    switch (ch) {
    case 1:
        return "Heisei Godzilla";
    case 2:
        return "Hollywood Godzilla";
    case 3:
        return "Mothra (Larva)";
    case 4:
        return "Mothra (Adult)";
    case 5:
        return "King Ghidorah";
    case 6:
        return "Mecha-King Ghidorah";
    case 7:
        return "Hedorah";
    case 9:
        return "Mechagodzilla (1974)";
    case 10:
        return "Mechagodzilla (1975)";
    case 12:
        return "Kiryu";
    case 13:
        return "Biollante";
    case 14:
        return "Destoroyah";
    case 15:
        return "Gigan";
    case 16:
        return "Jet Jaguar";
    case 17:
        return "Mechagodzilla 2";
    case 18:
        return "ch18";
    case 19:
        return "SpaceGodzilla";
    case 20:
        return "Anguirus";
    case 21:
        return "Rodan";
    case 22:
        return "Battra (Larva)";
    case 23:
        return "Battra (Adult)";
    default:
        return "ch" + Hex2(ch);
    }
}

std::string CharFileName(u8 ch) {
    return "ch" + Hex2(ch) + "a_00.pac";
}

std::string Mb(u64 bytes) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return b;
}

class Writer {
public:
    void U8(u8 v) {
        buf.push_back(v);
    }
    void U16(u16 v) {
        U8(static_cast<u8>(v));
        U8(static_cast<u8>(v >> 8));
    }
    void U32(u32 v) {
        U16(static_cast<u16>(v));
        U16(static_cast<u16>(v >> 16));
    }
    void Str(const std::string& s) {
        U16(static_cast<u16>(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }
    void Bytes(const u8* p, size_t n) {
        buf.insert(buf.end(), p, p + n);
    }
    std::vector<u8> buf;
};

class Reader {
public:
    Reader(const u8* p, size_t n) : p_(p), n_(n) {}
    bool U8(u8* v) {
        if (n_ - pos_ < 1) {
            return false;
        }
        *v = p_[pos_++];
        return true;
    }
    bool U16(u16* v) {
        u8 a, b;
        if (!U8(&a) || !U8(&b)) {
            return false;
        }
        *v = static_cast<u16>(a | b << 8);
        return true;
    }
    bool U32(u32* v) {
        u16 a, b;
        if (!U16(&a) || !U16(&b)) {
            return false;
        }
        *v = static_cast<u32>(a) | static_cast<u32>(b) << 16;
        return true;
    }
    bool Str(std::string* s) {
        u16 len;
        if (!U16(&len) || n_ - pos_ < len) {
            return false;
        }
        s->assign(reinterpret_cast<const char*>(p_ + pos_), len);
        pos_ += len;
        return true;
    }
    bool Fixed(u8* out, size_t len) {
        if (n_ - pos_ < len) {
            return false;
        }
        std::memcpy(out, p_ + pos_, len);
        pos_ += len;
        return true;
    }
    const u8* Rest(size_t* len) const {
        *len = n_ - pos_;
        return p_ + pos_;
    }

private:
    const u8* p_;
    size_t n_;
    size_t pos_ = 0;
};

struct ManifestEntry {
    std::string name;
    u32 size = 0;
    u32 stamp = 0;
    Digest digest{};
};

struct ManifestFile {
    u8 kind = kCharFile;
    std::string file;
    std::vector<ManifestEntry> entries;
};

struct Manifest {
    u8 monster = 0;
    std::vector<ManifestFile> files;

    const ManifestFile* File(u8 kind) const {
        for (const auto& f : files) {
            if (f.kind == kind) {
                return &f;
            }
        }
        return nullptr;
    }
};

std::vector<u8> EncodeManifest(const Manifest& m) {
    Writer w;
    w.U8(kMsgManifest);
    w.U8(kProtocolVersion);
    w.U8(m.monster);
    w.U16(static_cast<u16>(m.files.size()));
    for (const auto& f : m.files) {
        w.U8(f.kind);
        w.Str(f.file);
        w.U16(static_cast<u16>(f.entries.size()));
        for (const auto& e : f.entries) {
            w.Str(e.name);
            w.U32(e.size);
            w.U32(e.stamp);
            w.Bytes(e.digest.data(), e.digest.size());
        }
    }
    return std::move(w.buf);
}

bool DecodeManifest(Reader& r, Manifest* m) {
    u8 version;
    u16 nfiles;
    if (!r.U8(&version) || version != kProtocolVersion || !r.U8(&m->monster) || !r.U16(&nfiles) ||
        nfiles > 4) {
        return false;
    }
    for (u16 i = 0; i < nfiles; ++i) {
        ManifestFile f;
        u16 n;
        if (!r.U8(&f.kind) || !r.Str(&f.file) || !r.U16(&n) || n > 4096) {
            return false;
        }
        for (u16 j = 0; j < n; ++j) {
            ManifestEntry e;
            if (!r.Str(&e.name) || !r.U32(&e.size) || !r.U32(&e.stamp) ||
                !r.Fixed(e.digest.data(), e.digest.size())) {
                return false;
            }
            f.entries.push_back(std::move(e));
        }
        m->files.push_back(std::move(f));
    }
    return true;
}

// Identity of a manifest file: names, stamps and digests of every entry.
Digest FileKey(const ManifestFile& f) {
    Writer w;
    w.Str(f.file);
    for (const auto& e : f.entries) {
        w.Str(e.name);
        w.U32(e.stamp);
        w.Bytes(e.digest.data(), e.digest.size());
    }
    return Sha256(w.buf.data(), w.buf.size());
}

struct Incoming {
    u8 kind = 0;
    std::string name;
    u32 total = 0;
    u32 stamp = 0;
    Digest digest{};
    std::vector<u8> data;
    bool complete = false;
};

struct Outgoing {
    u8 kind = 0;
    std::string name;
    std::vector<u8> data;
    size_t offset = 0;
};

struct Peer {
    PeerInfo info;
    int32_t ctx = -1;
    bool channel_up = false;
    Proto::Millis connected_at = 0;
    bool sent_manifest = false;
    std::optional<Manifest> manifest;
    bool sent_request = false;
    std::map<std::pair<u8, std::string>, Incoming> incoming;
    u64 bytes_expected = 0;
    u64 bytes_received = 0;
    Proto::Millis last_progress_log = 0;
    bool got_done = false;
    bool ready = false;
    bool gave_up = false;
    std::optional<fs::path> patched_char;
    std::map<std::string, std::pair<Digest, u32>> game_entries; // name -> digest, stamp
    std::deque<std::pair<u8, std::string>> requested;
    std::optional<Outgoing> sending;
    bool done_pending = false;
};

} // namespace

struct Manager::Impl {
    std::mutex mutex;
    Hooks hooks;
    bool enabled = true;
    fs::path game_dir;
    fs::path cache_dir;
    std::string lang_file = "game_en.pac";

    Proto::Engine engine;
    u32 room_id = 0;
    u16 my_member = 0;
    u8 my_monster = 0;
    Proto::Millis room_since = 0;
    bool hold_timeout_logged = false;
    std::map<u16, Peer> peers;
    std::map<u8, u32> opens;
    std::map<fs::path, PacArchive> archives;

    void Log(LogLevel level, const std::string& text) {
        if (hooks.log) {
            hooks.log(level, "ModSync: " + text);
        }
    }

    const PacArchive* Archive(const fs::path& path) {
        if (const auto it = archives.find(path); it != archives.end()) {
            return &it->second;
        }
        auto pac = PacArchive::Open(path);
        if (!pac) {
            return nullptr;
        }
        return &archives.emplace(path, std::move(*pac)).first->second;
    }

    fs::path EntryCachePath(const Digest& d) const {
        return cache_dir / "entries" / (DigestHex(d) + ".bin");
    }
    fs::path WholeCachePath(const Digest& key) const {
        return cache_dir / "whole" / (DigestHex(key) + ".pac");
    }

    bool HaveCached(const Digest& d, u32 size) const {
        std::error_code ec;
        return fs::file_size(EntryCachePath(d), ec) == size && !ec;
    }

    bool LoadCached(const Digest& d, std::vector<u8>* out) const {
        std::ifstream in(EntryCachePath(d), std::ios::binary);
        if (!in) {
            return false;
        }
        out->assign(std::istreambuf_iterator<char>(in), {});
        return Sha256(out->data(), out->size()) == d;
    }

    bool StoreFile(const fs::path& path, const std::vector<u8>& data) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        const auto tmp = path.string() + ".part";
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            o.write(reinterpret_cast<const char*>(data.data()), data.size());
            if (!o) {
                return false;
            }
        }
        fs::rename(tmp, path, ec);
        return !ec;
    }

    std::optional<Manifest> LocalManifest(u8 monster) {
        Manifest m;
        m.monster = monster;
        const PacArchive* ch = Archive(game_dir / CharFileName(monster));
        if (!ch) {
            return std::nullopt;
        }
        ManifestFile cf{kCharFile, CharFileName(monster), {}};
        std::set<std::string> names;
        for (const auto& e : ch->Entries()) {
            const auto d = ch->EntryDigest(e);
            if (!d) {
                return std::nullopt;
            }
            cf.entries.push_back({e.name, e.size, e.stamp, *d});
            names.insert(e.name);
        }
        m.files.push_back(std::move(cf));
        // The monster's copies in the language archive (Godzilla keeps its textures there too).
        if (const PacArchive* game = Archive(game_dir / lang_file)) {
            ManifestFile gf{kGameFile, lang_file, {}};
            const std::string sound = "sound_ch" + Hex2(monster) + ".snd";
            for (const auto& e : game->Entries()) {
                if (e.name == sound || (names.contains(e.name) && e.name != "DefaultEnvMap.dds")) {
                    const auto d = game->EntryDigest(e);
                    if (d) {
                        gf.entries.push_back({e.name, e.size, e.stamp, *d});
                    }
                }
            }
            if (!gf.entries.empty()) {
                m.files.push_back(std::move(gf));
            }
        }
        return m;
    }

    void SendMessage(Peer& p, const std::vector<u8>& msg) {
        const int32_t rc = engine.Write(p.ctx, msg.data(), msg.size(), NowMs());
        if (rc < 0) {
            Log(LogLevel::Warning,
                "could not send to '" + p.info.name + "' (error " + std::to_string(rc) + ")");
        }
    }

    void FlushEngine() {
        for (auto& a : engine.TakeActions()) {
            if (a.kind == Proto::Action::Kind::Send && hooks.send) {
                std::vector<u8> pkt{'S', 'H', 'A', 'D', kChannelTag};
                pkt.insert(pkt.end(), a.data.begin(), a.data.end());
                hooks.send(a.to.addr, a.to.port, pkt.data(), pkt.size());
            } else if (a.kind == Proto::Action::Kind::ContextEvent) {
                for (auto& [id, p] : peers) {
                    if (p.ctx != a.ctx_id) {
                        continue;
                    }
                    if (a.event == Proto::CONTEXT_EVENT_ESTABLISHED && !p.channel_up) {
                        p.channel_up = true;
                    } else if (a.event == Proto::CONTEXT_EVENT_ERROR ||
                               a.event == Proto::CONTEXT_EVENT_CLOSED) {
                        if (!p.ready && !p.gave_up) {
                            Log(LogLevel::Warning, "lost the mod channel to '" + p.info.name +
                                                       "'; playing without their mods");
                            p.gave_up = true;
                        }
                    }
                }
            }
        }
    }

    void StartPeer(Peer& p) {
        p.ctx = engine.CreateContext();
        if (p.ctx < 0) {
            p.gave_up = true;
            return;
        }
        engine.Bind(p.ctx, kSocket, kVport, 1);
        engine.Initiate(p.ctx, Proto::PeerKey{p.info.addr, p.info.port, kVport}, NowMs());
        p.connected_at = NowMs();
        Log(LogLevel::Info,
            "opening mod channel to '" + p.info.name + "' (" + MonsterName(p.info.monster) + ")");
    }

    void StopPeer(Peer& p) {
        if (p.ctx >= 0) {
            engine.Terminate(p.ctx, NowMs());
            p.ctx = -1;
        }
    }

    void HandleManifest(Peer& p, Reader& r) {
        Manifest m;
        if (!DecodeManifest(r, &m)) {
            Log(LogLevel::Warning, "bad manifest from '" + p.info.name + "'");
            p.gave_up = true;
            return;
        }
        p.manifest = std::move(m);
        PlanDownloads(p);
    }

    void PlanDownloads(Peer& p) {
        const Manifest& m = *p.manifest;
        Writer req;
        req.U8(kMsgRequest);
        std::vector<std::pair<u8, std::string>> wanted;
        u64 need_bytes = 0;
        size_t cached = 0;

        if (const ManifestFile* cf = m.File(kCharFile)) {
            const PacArchive* local = Archive(game_dir / cf->file);
            bool same_layout = local && local->Entries().size() == cf->entries.size();
            if (same_layout) {
                for (size_t i = 0; i < cf->entries.size(); ++i) {
                    if (local->Entries()[i].name != cf->entries[i].name) {
                        same_layout = false;
                        break;
                    }
                }
            }
            if (!same_layout) {
                const Digest key = FileKey(*cf);
                std::error_code ec;
                if (fs::exists(WholeCachePath(key), ec)) {
                    p.patched_char = WholeCachePath(key);
                    ++cached;
                } else {
                    wanted.emplace_back(kCharFile, kWholeName);
                    Incoming in;
                    in.kind = kCharFile;
                    in.name = kWholeName;
                    p.incoming[{kCharFile, kWholeName}] = std::move(in);
                }
            } else {
                for (const auto& e : cf->entries) {
                    const PacEntry* le = local->Find(e.name);
                    if (le && local->EntryDigest(*le) == e.digest && le->stamp == e.stamp) {
                        continue;
                    }
                    if (e.size > kMaxEntryBytes) {
                        Log(LogLevel::Warning, "skipping oversized entry " + e.name);
                        continue;
                    }
                    if (HaveCached(e.digest, e.size)) {
                        ++cached;
                    } else {
                        wanted.emplace_back(kCharFile, e.name);
                        need_bytes += e.size;
                    }
                    Incoming in{kCharFile, e.name, e.size, e.stamp, e.digest, {}, false};
                    in.complete = HaveCached(e.digest, e.size);
                    p.incoming[{kCharFile, e.name}] = std::move(in);
                }
            }
        }
        if (const ManifestFile* gf = m.File(kGameFile)) {
            const PacArchive* local = Archive(game_dir / lang_file);
            for (const auto& e : gf->entries) {
                const PacEntry* le = local ? local->Find(e.name) : nullptr;
                if (!le || (local->EntryDigest(*le) == e.digest && le->stamp == e.stamp)) {
                    continue;
                }
                if (e.size > kMaxEntryBytes) {
                    continue;
                }
                p.game_entries[e.name] = {e.digest, e.stamp};
                if (HaveCached(e.digest, e.size)) {
                    ++cached;
                    continue;
                }
                wanted.emplace_back(kGameFile, e.name);
                need_bytes += e.size;
                p.incoming[{kGameFile, e.name}] =
                    Incoming{kGameFile, e.name, e.size, e.stamp, e.digest, {}, false};
            }
        }
        if (need_bytes > kMaxPeerBytes) {
            Log(LogLevel::Warning, "'" + p.info.name + "' mod is too large (" + Mb(need_bytes) +
                                       "); playing without it");
            p.gave_up = true;
            return;
        }
        req.U16(static_cast<u16>(wanted.size()));
        for (const auto& [kind, name] : wanted) {
            req.U8(kind);
            req.Str(name);
        }
        SendMessage(p, req.buf);
        p.sent_request = true;
        p.bytes_expected = need_bytes;
        if (wanted.empty()) {
            Log(LogLevel::Info, "'" + p.info.name + "' (" + MonsterName(m.monster) +
                                    "): nothing to download" +
                                    (cached ? " (" + std::to_string(cached) + " from cache)" : ""));
        } else {
            Log(LogLevel::Info,
                "'" + p.info.name + "' (" + MonsterName(m.monster) + "): downloading " +
                    std::to_string(wanted.size()) + " file(s)" +
                    (need_bytes ? ", " + Mb(need_bytes) : std::string{", whole monster file"}) +
                    (cached ? ", " + std::to_string(cached) + " already cached" : ""));
        }
    }

    void HandleRequest(Peer& p, Reader& r) {
        u16 n;
        if (!r.U16(&n) || n > 4096) {
            return;
        }
        for (u16 i = 0; i < n; ++i) {
            u8 kind;
            std::string name;
            if (!r.U8(&kind) || !r.Str(&name)) {
                return;
            }
            p.requested.emplace_back(kind, name);
        }
        p.done_pending = true;
        if (n != 0) {
            Log(LogLevel::Info, "'" + p.info.name + "' asked for " + std::to_string(n) +
                                    " file(s) of our " + MonsterName(my_monster));
        }
    }

    bool LoadOutgoing(u8 kind, const std::string& name, Outgoing* out) {
        out->kind = kind;
        out->name = name;
        out->offset = 0;
        if (kind == kCharFile && name == kWholeName) {
            const fs::path path = game_dir / CharFileName(my_monster);
            std::error_code ec;
            const auto size = fs::file_size(path, ec);
            if (ec || size > kMaxWholeBytes) {
                return false;
            }
            std::ifstream in(path, std::ios::binary);
            out->data.assign(std::istreambuf_iterator<char>(in), {});
            return out->data.size() == size;
        }
        const fs::path path =
            kind == kCharFile ? game_dir / CharFileName(my_monster) : game_dir / lang_file;
        const PacArchive* pac = Archive(path);
        const PacEntry* e = pac ? pac->Find(name) : nullptr;
        return e && e->size <= kMaxEntryBytes && pac->ReadEntry(*e, &out->data);
    }

    void PumpSends(Peer& p) {
        if (!p.channel_up || p.ctx < 0) {
            return;
        }
        while (true) {
            if (!p.sending) {
                if (p.requested.empty()) {
                    if (p.done_pending) {
                        Writer w;
                        w.U8(kMsgDone);
                        SendMessage(p, w.buf);
                        p.done_pending = false;
                    }
                    return;
                }
                const auto [kind, name] = p.requested.front();
                p.requested.pop_front();
                Outgoing out;
                if (!LoadOutgoing(kind, name, &out)) {
                    Log(LogLevel::Warning, "cannot read requested " + name);
                    continue;
                }
                p.sending = std::move(out);
            }
            Outgoing& o = *p.sending;
            const size_t n = std::min(kChunk, o.data.size() - o.offset);
            Writer w;
            w.U8(kMsgData);
            w.U8(o.kind);
            w.Str(o.name);
            w.U32(static_cast<u32>(o.data.size()));
            w.U32(static_cast<u32>(o.offset));
            w.Bytes(o.data.data() + o.offset, n);
            if (engine.GetSizeWritable(p.ctx) < static_cast<int32_t>(w.buf.size())) {
                return;
            }
            if (engine.Write(p.ctx, w.buf.data(), w.buf.size(), NowMs()) < 0) {
                return;
            }
            o.offset += n;
            if (o.offset >= o.data.size()) {
                p.sending.reset();
            }
        }
    }

    void HandleData(Peer& p, Reader& r) {
        u8 kind;
        std::string name;
        u32 total, offset;
        if (!r.U8(&kind) || !r.Str(&name) || !r.U32(&total) || !r.U32(&offset)) {
            return;
        }
        size_t len;
        const u8* bytes = r.Rest(&len);
        const auto it = p.incoming.find({kind, name});
        if (it == p.incoming.end() || it->second.complete) {
            return;
        }
        Incoming& in = it->second;
        const bool whole = name == kWholeName;
        if (whole) {
            if (total > kMaxWholeBytes) {
                return;
            }
            if (in.total == 0) {
                in.total = total;
                p.bytes_expected += total;
            }
        }
        if (total != in.total || offset != in.data.size() || offset + len > total) {
            Log(LogLevel::Warning, "out-of-order data for " + name + " from '" + p.info.name +
                                       "'; dropping their mods");
            p.gave_up = true;
            return;
        }
        in.data.insert(in.data.end(), bytes, bytes + len);
        p.bytes_received += len;
        if (in.data.size() == in.total) {
            FinishIncoming(p, in);
        }
        const Proto::Millis now = NowMs();
        if (now - p.last_progress_log > 2000 && p.bytes_expected > 0) {
            p.last_progress_log = now;
            Log(LogLevel::Info, "receiving from '" + p.info.name + "': " + Mb(p.bytes_received) +
                                    " / " + Mb(p.bytes_expected));
        }
    }

    void FinishIncoming(Peer& p, Incoming& in) {
        if (in.name == kWholeName) {
            const ManifestFile* cf = p.manifest->File(kCharFile);
            const Digest key = FileKey(*cf);
            const fs::path path = WholeCachePath(key);
            if (!StoreFile(path, in.data)) {
                p.gave_up = true;
                return;
            }
            // Check every entry of the received file against the manifest.
            auto pac = PacArchive::Open(path);
            bool ok = pac && pac->Entries().size() == cf->entries.size();
            for (size_t i = 0; ok && i < cf->entries.size(); ++i) {
                const auto& e = pac->Entries()[i];
                ok = e.name == cf->entries[i].name && pac->EntryDigest(e) == cf->entries[i].digest;
            }
            if (!ok) {
                std::error_code ec;
                fs::remove(path, ec);
                Log(LogLevel::Warning, "monster file from '" + p.info.name +
                                           "' did not match its manifest; ignoring it");
                p.gave_up = true;
                return;
            }
            p.patched_char = path;
        } else {
            if (Sha256(in.data.data(), in.data.size()) != in.digest) {
                Log(LogLevel::Warning,
                    in.name + " from '" + p.info.name + "' failed its hash check; ignoring");
                p.gave_up = true;
                return;
            }
            StoreFile(EntryCachePath(in.digest), in.data);
        }
        in.complete = true;
        in.data.clear();
        in.data.shrink_to_fit();
    }

    void MaybeFinish(Peer& p) {
        if (p.ready || p.gave_up || !p.manifest || !p.sent_request) {
            return;
        }
        for (const auto& [key, in] : p.incoming) {
            if (!in.complete) {
                return;
            }
        }
        if (!p.patched_char) {
            std::map<std::string, PacBlob> rep;
            for (const auto& [key, in] : p.incoming) {
                if (key.first != kCharFile) {
                    continue;
                }
                PacBlob blob;
                if (!LoadCached(in.digest, &blob.data)) {
                    p.gave_up = true;
                    return;
                }
                blob.stamp = in.stamp;
                rep[in.name] = std::move(blob);
            }
            if (!rep.empty()) {
                const ManifestFile* cf = p.manifest->File(kCharFile);
                const fs::path out =
                    cache_dir / "patched" /
                    (cf->file + "." + DigestHex(FileKey(*cf)).substr(0, 16) + ".pac");
                std::error_code ec;
                std::string error;
                if (!fs::exists(out, ec)) {
                    const PacArchive* local = Archive(game_dir / cf->file);
                    if (!local || !BuildPatchedPac(*local, rep, out, &error)) {
                        Log(LogLevel::Warning, "cannot build " + cf->file + ": " + error);
                        p.gave_up = true;
                        return;
                    }
                }
                p.patched_char = out;
            }
        }
        p.ready = true;
        if (!p.game_entries.empty() && p.info.monster != my_monster) {
            GamePatchFor(lang_file); // build now so the match load doesn't wait for it
        }
        Log(LogLevel::Info,
            "'" + p.info.name + "' (" + MonsterName(p.manifest->monster) + ") is ready: " +
                (p.patched_char || !p.game_entries.empty() ? "their version will be shown"
                                                           : "same files as ours"));
    }

    std::optional<fs::path> GamePatchFor(const std::string& lang) {
        std::map<std::string, PacBlob> rep;
        Writer key;
        key.Str(lang);
        for (auto& [id, p] : peers) {
            if (!p.ready || p.game_entries.empty() || p.info.monster == my_monster) {
                continue;
            }
            for (const auto& [name, ds] : p.game_entries) {
                PacBlob blob;
                if (!LoadCached(ds.first, &blob.data)) {
                    return std::nullopt;
                }
                blob.stamp = ds.second;
                rep[name] = std::move(blob);
                key.Str(name);
                key.Bytes(ds.first.data(), ds.first.size());
            }
        }
        if (rep.empty()) {
            return std::nullopt;
        }
        const PacArchive* local = Archive(game_dir / lang);
        if (!local) {
            return std::nullopt;
        }
        for (auto it = rep.begin(); it != rep.end();) {
            it = local->Find(it->first) ? std::next(it) : rep.erase(it);
        }
        const fs::path out =
            cache_dir / "patched" /
            (lang + "." + DigestHex(Sha256(key.buf.data(), key.buf.size())).substr(0, 16) + ".pac");
        std::error_code ec;
        if (!fs::exists(out, ec)) {
            std::string error;
            if (!BuildPatchedPac(*local, rep, out, &error)) {
                Log(LogLevel::Warning, "cannot build " + lang + ": " + error);
                return std::nullopt;
            }
        }
        return out;
    }
};

Manager::Manager() : impl_(std::make_unique<Impl>()) {}
Manager::~Manager() = default;

void Manager::SetHooks(Hooks hooks) {
    std::scoped_lock lock{impl_->mutex};
    impl_->hooks = std::move(hooks);
}

void Manager::Configure(const fs::path& game_dir, const fs::path& cache_dir) {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->game_dir != game_dir) {
        impl_->archives.clear();
    }
    impl_->game_dir = game_dir;
    impl_->cache_dir = cache_dir;
}

void Manager::SetEnabled(bool enabled) {
    std::scoped_lock lock{impl_->mutex};
    impl_->enabled = enabled;
}

void Manager::SetLanguageFile(const std::string& name) {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->lang_file != name) {
        impl_->lang_file = name;
        impl_->Log(LogLevel::Info, "language archive is " + name);
    }
}

void Manager::SetRoom(u32 room_id, u16 my_member_id, u8 my_monster,
                      const std::vector<PeerInfo>& peers) {
    auto& d = *impl_;
    std::scoped_lock lock{d.mutex};
    if (!d.enabled || d.game_dir.empty()) {
        return;
    }
    if (d.room_id != room_id) {
        for (auto& [id, p] : d.peers) {
            d.StopPeer(p);
        }
        d.peers.clear();
        d.opens.clear();
        d.room_id = room_id;
        d.room_since = NowMs();
        d.hold_timeout_logged = false;
    }
    d.my_member = my_member_id;
    if (d.my_monster != my_monster && my_monster != 0) {
        d.Log(LogLevel::Info, "we picked " + MonsterName(my_monster));
    }
    d.my_monster = my_monster;

    std::set<u16> present;
    for (const auto& info : peers) {
        present.insert(info.member_id);
        auto [it, inserted] = d.peers.try_emplace(info.member_id);
        Peer& p = it->second;
        const bool was_connected = p.info.connected;
        const u8 old_monster = p.info.monster;
        p.info = info;
        if (inserted || old_monster != info.monster) {
            if (info.monster != 0) {
                d.Log(LogLevel::Info, "'" + info.name + "' picked " + MonsterName(info.monster));
            }
        }
        if (info.connected && !was_connected && p.ctx < 0 && !p.gave_up) {
            d.StartPeer(p);
        }
    }
    for (auto it = d.peers.begin(); it != d.peers.end();) {
        if (!present.contains(it->first)) {
            d.StopPeer(it->second);
            it = d.peers.erase(it);
        } else {
            ++it;
        }
    }
    d.FlushEngine();
}

void Manager::ClearRoom() {
    auto& d = *impl_;
    std::scoped_lock lock{d.mutex};
    for (auto& [id, p] : d.peers) {
        d.StopPeer(p);
    }
    d.FlushEngine();
    d.peers.clear();
    d.opens.clear();
    d.room_id = 0;
}

void Manager::OnMatchStart() {
    std::scoped_lock lock{impl_->mutex};
    impl_->opens.clear();
}

void Manager::OnPacket(u32 addr, u16 port, const u8* data, size_t len) {
    auto& d = *impl_;
    std::scoped_lock lock{d.mutex};
    if (len < 5 || d.room_id == 0) {
        return;
    }
    d.engine.OnPacket(kSocket, Proto::PeerKey{addr, port, kVport}, data + 5, len - 5, NowMs());
    d.FlushEngine();
}

void Manager::Tick() {
    auto& d = *impl_;
    std::scoped_lock lock{d.mutex};
    if (d.room_id == 0) {
        return;
    }
    const Proto::Millis now = NowMs();
    d.engine.Tick(now);
    d.FlushEngine();
    for (auto& [id, p] : d.peers) {
        if (p.ctx < 0 || p.gave_up) {
            continue;
        }
        if (p.channel_up && !p.sent_manifest && d.my_monster != 0) {
            if (auto m = d.LocalManifest(d.my_monster)) {
                d.SendMessage(p, EncodeManifest(*m));
            }
            p.sent_manifest = true;
        }
        // Read everything the peer sent.
        std::vector<u8> buf(Proto::kMaxMessage);
        while (true) {
            const int32_t n = d.engine.Read(p.ctx, buf.data(), buf.size());
            if (n <= 0) {
                break;
            }
            Reader r(buf.data(), static_cast<size_t>(n));
            u8 type;
            if (!r.U8(&type)) {
                continue;
            }
            switch (type) {
            case kMsgManifest:
                if (!p.manifest) {
                    d.HandleManifest(p, r);
                }
                break;
            case kMsgRequest:
                d.HandleRequest(p, r);
                break;
            case kMsgData:
                d.HandleData(p, r);
                break;
            case kMsgDone:
                p.got_done = true;
                break;
            default:
                break;
            }
            if (p.gave_up) {
                break;
            }
        }
        d.PumpSends(p);
        d.MaybeFinish(p);
        if (!p.manifest && !p.gave_up && p.connected_at != 0 &&
            now - p.connected_at > kNoManifestGiveUp) {
            p.gave_up = true;
            d.Log(LogLevel::Info,
                  "'" + p.info.name + "' does not share mods (older build?); using our files");
        }
    }
    d.FlushEngine();
}

bool Manager::HoldReady() {
    auto& d = *impl_;
    std::scoped_lock lock{d.mutex};
    if (!d.enabled || d.room_id == 0) {
        return false;
    }
    bool waiting = false;
    for (auto& [id, p] : d.peers) {
        if (p.info.connected && !p.ready && !p.gave_up) {
            waiting = true;
        }
    }
    if (waiting && NowMs() - d.room_since > kHoldLimit) {
        if (!d.hold_timeout_logged) {
            d.hold_timeout_logged = true;
            d.Log(LogLevel::Warning, "mod downloads are taking too long; continuing without them");
        }
        return false;
    }
    return waiting;
}

std::optional<fs::path> Manager::Redirect(const std::string& guest_path, bool peek) {
    auto& d = *impl_;
    std::scoped_lock lock{d.mutex};
    if (!d.enabled || d.room_id == 0) {
        return std::nullopt;
    }
    const std::string name = guest_path.substr(guest_path.find_last_of('/') + 1);
    if (name.starts_with("game_") && name.ends_with(".pac") && name.size() == 11) {
        if (d.lang_file != name && !peek) {
            d.lang_file = name;
        }
        auto path = d.GamePatchFor(name);
        if (path && !peek) {
            d.Log(LogLevel::Info, "loading " + name + " with the opponent's monster textures");
        }
        return path;
    }
    if (!(name.size() == 12 && name.starts_with("ch") && name.ends_with("a_00.pac"))) {
        return std::nullopt;
    }
    const u8 monster = static_cast<u8>((name[2] - '0') * 10 + (name[3] - '0'));
    // The game opens one file per player, in member order, even when two pick the same monster.
    std::vector<std::pair<u16, Peer*>> order; // member id, peer (nullptr = us)
    if (d.my_monster == monster) {
        order.emplace_back(d.my_member, nullptr);
    }
    for (auto& [id, p] : d.peers) {
        if (p.info.monster == monster) {
            order.emplace_back(id, &p);
        }
    }
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    const u32 index = peek ? d.opens[monster] : d.opens[monster]++;
    if (index >= order.size() || order[index].second == nullptr) {
        return std::nullopt;
    }
    Peer& p = *order[index].second;
    if (!p.ready || !p.patched_char) {
        return std::nullopt;
    }
    if (!peek) {
        d.Log(LogLevel::Info, "loading '" + p.info.name + "''s " + MonsterName(monster));
    }
    return p.patched_char;
}

Manager& Instance() {
    static Manager manager;
    return manager;
}

} // namespace Core::Godzilla::ModSync
