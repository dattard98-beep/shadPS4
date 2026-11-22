// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <system_error>

#include <openssl/evp.h>

#include "core/godzilla/pac.h"

namespace Core::Godzilla {

namespace {

constexpr u32 kHeaderSize = 0x20;
constexpr u32 kEntrySize = 0x20;
constexpr u32 kMaxEntries = 4096;

u32 ReadU32(const u8* p) {
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
           static_cast<u32>(p[3]) << 24;
}

void WriteU32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}

u64 Align32(u64 v) {
    return (v + 31) & ~u64{31};
}

} // namespace

std::string DigestHex(const Digest& d) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(d.size() * 2);
    for (const u8 b : d) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 15]);
    }
    return out;
}

Digest Sha256(const u8* data, size_t len) {
    Digest d{};
    unsigned int out_len = 0;
    EVP_Digest(data, len, d.data(), &out_len, EVP_sha256(), nullptr);
    return d;
}

std::optional<PacArchive> PacArchive::Open(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::error_code ec;
    const u64 file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size < kHeaderSize) {
        return std::nullopt;
    }
    std::vector<u8> header(kHeaderSize);
    if (!in.read(reinterpret_cast<char*>(header.data()), kHeaderSize)) {
        return std::nullopt;
    }
    if (std::memcmp(header.data(), "add\0", 4) != 0 || ReadU32(&header[8]) != kHeaderSize ||
        ReadU32(&header[12]) != kEntrySize) {
        return std::nullopt;
    }
    const u32 count = ReadU32(&header[16]);
    if (count == 0 || count > kMaxEntries || ReadU32(&header[20]) != file_size) {
        return std::nullopt;
    }
    std::vector<u8> table(static_cast<size_t>(count) * kEntrySize);
    if (!in.read(reinterpret_cast<char*>(table.data()), table.size())) {
        return std::nullopt;
    }
    u32 data_start = static_cast<u32>(file_size);
    for (u32 i = 0; i < count; ++i) {
        data_start = std::min(data_start, ReadU32(&table[i * kEntrySize]));
    }
    const u32 names_start = kHeaderSize + count * kEntrySize;
    if (data_start < names_start || data_start > file_size) {
        return std::nullopt;
    }
    std::vector<u8> head(data_start);
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(head.data()), head.size())) {
        return std::nullopt;
    }

    PacArchive pac;
    pac.path_ = path;
    pac.file_size_ = file_size;
    pac.data_start_ = data_start;
    for (u32 i = 0; i < count; ++i) {
        const u8* e = &head[kHeaderSize + i * kEntrySize];
        PacEntry entry;
        entry.offset = ReadU32(e);
        entry.size = ReadU32(e + 4);
        entry.stamp = ReadU32(e + 12);
        const u32 name_off = ReadU32(e + 16);
        if (static_cast<u64>(entry.offset) + entry.size > file_size || name_off < names_start ||
            name_off >= data_start) {
            return std::nullopt;
        }
        const auto* name = reinterpret_cast<const char*>(&head[name_off]);
        entry.name.assign(name, strnlen(name, data_start - name_off));
        pac.entries_.push_back(std::move(entry));
    }
    pac.head_ = std::move(head);
    return pac;
}

const PacEntry* PacArchive::Find(const std::string& name) const {
    for (const auto& e : entries_) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

bool PacArchive::ReadEntry(const PacEntry& entry, std::vector<u8>* out) const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return false;
    }
    out->resize(entry.size);
    in.seekg(entry.offset);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out->data()), entry.size));
}

std::optional<Digest> PacArchive::EntryDigest(const PacEntry& entry) const {
    if (const auto it = digests_.find(entry.name); it != digests_.end()) {
        return it->second;
    }
    std::vector<u8> data;
    if (!ReadEntry(entry, &data)) {
        return std::nullopt;
    }
    const Digest d = Sha256(data.data(), data.size());
    digests_[entry.name] = d;
    return d;
}

bool BuildPatchedPac(const PacArchive& base, const std::map<std::string, PacBlob>& replace,
                     const std::filesystem::path& out, std::string* error) {
    for (const auto& [name, blob] : replace) {
        if (!base.Find(name)) {
            *error = "entry " + name + " is not in " + base.Path().filename().string();
            return false;
        }
    }
    std::ifstream in(base.Path(), std::ios::binary);
    if (!in) {
        *error = "cannot read " + base.Path().string();
        return false;
    }

    std::vector<u8> head = base.Head();
    std::vector<std::pair<u64, u32>> layout; // new offset/size per entry
    u64 cursor = Align32(head.size());
    for (const auto& entry : base.Entries()) {
        const auto it = replace.find(entry.name);
        const u32 size =
            it != replace.end() ? static_cast<u32>(it->second.data.size()) : entry.size;
        layout.emplace_back(cursor, size);
        cursor = Align32(cursor + size);
    }
    // The last entry is not padded in the originals; drop trailing padding.
    const u64 total = layout.empty() ? head.size() : layout.back().first + layout.back().second;
    if (total > 0xffffffffull) {
        *error = "patched archive would exceed 4 GiB";
        return false;
    }
    for (size_t i = 0; i < base.Entries().size(); ++i) {
        u8* e = &head[kHeaderSize + i * kEntrySize];
        WriteU32(e, static_cast<u32>(layout[i].first));
        WriteU32(e + 4, layout[i].second);
        const auto it = replace.find(base.Entries()[i].name);
        if (it != replace.end()) {
            WriteU32(e + 12, it->second.stamp);
        }
    }
    WriteU32(&head[20], static_cast<u32>(total));

    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    const auto tmp = out.string() + ".part";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) {
            *error = "cannot write " + tmp;
            return false;
        }
        o.write(reinterpret_cast<const char*>(head.data()), head.size());
        u64 written = head.size();
        std::vector<u8> buf;
        static const std::array<char, 32> zeros{};
        for (size_t i = 0; i < base.Entries().size(); ++i) {
            const auto& entry = base.Entries()[i];
            o.write(zeros.data(), static_cast<std::streamsize>(layout[i].first - written));
            const auto it = replace.find(entry.name);
            if (it != replace.end()) {
                o.write(reinterpret_cast<const char*>(it->second.data.data()),
                        it->second.data.size());
            } else {
                buf.resize(entry.size);
                in.seekg(entry.offset);
                if (!in.read(reinterpret_cast<char*>(buf.data()), entry.size)) {
                    *error = "cannot read entry " + entry.name;
                    return false;
                }
                o.write(reinterpret_cast<const char*>(buf.data()), buf.size());
            }
            written = layout[i].first + layout[i].second;
        }
        if (!o) {
            *error = "write failed for " + tmp;
            return false;
        }
    }
    std::filesystem::rename(tmp, out, ec);
    if (ec) {
        std::filesystem::remove(out, ec);
        std::filesystem::rename(tmp, out, ec);
        if (ec) {
            *error = "cannot move " + tmp + " into place: " + ec.message();
            return false;
        }
    }
    return true;
}

bool IsLzs(const std::vector<u8>& entry) {
    return entry.size() >= 0x20 && std::memcmp(entry.data(), "LZS\0", 4) == 0;
}

bool LzsUnpack(const std::vector<u8>& e, std::vector<u8>* out) {
    if (!IsLzs(e)) {
        return false;
    }
    const u32 header = ReadU32(&e[8]);
    const u32 literals = ReadU32(&e[12]);
    const u32 size = ReadU32(&e[16]);
    if (header > literals || literals > e.size() || size > (1u << 30)) {
        return false;
    }
    out->clear();
    out->reserve(size);
    size_t i = header;
    size_t lit = literals;
    while (out->size() < size) {
        if (i + 2 > literals) {
            return false;
        }
        const u32 flags = e[i] | e[i + 1] << 8;
        i += 2;
        for (int f = 0; f < 16 && out->size() < size; ++f) {
            if (flags >> f & 1) {
                if (lit >= e.size()) {
                    return false;
                }
                out->push_back(e[lit++]);
                continue;
            }
            if (i + 2 > literals) {
                return false;
            }
            const u32 t = e[i] | e[i + 1] << 8;
            i += 2;
            const size_t len = (t & 31) + 2;
            const size_t dist = t >> 5;
            if (dist == 0 || dist > out->size()) {
                return false;
            }
            for (size_t k = 0; k < len; ++k) {
                out->push_back((*out)[out->size() - dist]);
            }
        }
    }
    out->resize(size);
    return true;
}

std::vector<u8> LzsStore(const std::vector<u8>& raw, const std::vector<u8>& original) {
    // Real compression (back-references up to 33 bytes long, 2047 back), so a rewritten
    // copy stays about the size of the original: the game reads a monster's file into a
    // fixed-size buffer, and a copy stored without compression did not fit.
    constexpr size_t kMaxLen = 33;
    constexpr size_t kMinLen = 3;
    constexpr size_t kWindow = 2047;
    constexpr int kChain = 48;
    constexpr u32 kHashBits = 15;
    const u32 header = ReadU32(&original[8]);
    std::vector<u8> e(original.begin(), original.begin() + header);
    std::vector<u8> literals;
    literals.reserve(raw.size() / 2);
    std::vector<s32> head(size_t{1} << kHashBits, -1);
    std::vector<s32> prev(raw.size(), -1);
    const auto hash = [&](size_t i) {
        const u32 v = raw[i] | raw[i + 1] << 8 | raw[i + 2] << 16;
        return (v * 2654435761u) >> (32 - kHashBits);
    };
    const auto insert = [&](size_t i) {
        if (i + 2 < raw.size()) {
            const u32 h = hash(i);
            prev[i] = head[h];
            head[h] = static_cast<s32>(i);
        }
    };
    size_t flag_pos = 0;
    int flag_bit = 16;
    const auto put = [&](bool literal) {
        if (flag_bit == 16) {
            flag_pos = e.size();
            e.push_back(0);
            e.push_back(0);
            flag_bit = 0;
        }
        if (literal) {
            e[flag_pos + (flag_bit >> 3)] |= static_cast<u8>(1u << (flag_bit & 7));
        }
        ++flag_bit;
    };
    size_t i = 0;
    while (i < raw.size()) {
        size_t best_len = 0;
        size_t best_dist = 0;
        if (i + kMinLen <= raw.size()) {
            const size_t limit = std::min(kMaxLen, raw.size() - i);
            s32 c = head[hash(i)];
            for (int n = 0; n < kChain && c >= 0 && i - static_cast<size_t>(c) <= kWindow;
                 ++n, c = prev[c]) {
                const size_t j = static_cast<size_t>(c);
                if (raw[j + best_len] != raw[i + best_len]) {
                    continue;
                }
                size_t len = 0;
                while (len < limit && raw[j + len] == raw[i + len]) {
                    ++len;
                }
                if (len > best_len) {
                    best_len = len;
                    best_dist = i - j;
                    if (len == limit) {
                        break;
                    }
                }
            }
        }
        if (best_len >= kMinLen) {
            put(false);
            const u32 t = static_cast<u32>(best_dist << 5 | (best_len - 2));
            e.push_back(static_cast<u8>(t));
            e.push_back(static_cast<u8>(t >> 8));
            for (size_t k = 0; k < best_len; ++k) {
                insert(i + k);
            }
            i += best_len;
        } else {
            put(true);
            literals.push_back(raw[i]);
            insert(i);
            ++i;
        }
    }
    const u32 literal_start = static_cast<u32>(e.size());
    WriteU32(&e[12], literal_start);
    WriteU32(&e[16], static_cast<u32>(raw.size()));
    WriteU32(&e[20], static_cast<u32>(literal_start + literals.size()));
    e.insert(e.end(), literals.begin(), literals.end());
    return e;
}

namespace {

// Replaces every occurrence of `from` with `to` (same length). Returns the count.
size_t ReplaceAll(std::vector<u8>* data, const std::string& from, const std::string& to) {
    size_t count = 0;
    auto it = data->begin();
    while (true) {
        it = std::search(it, data->end(), from.begin(), from.end());
        if (it == data->end()) {
            return count;
        }
        std::copy(to.begin(), to.end(), it);
        it += static_cast<std::ptrdiff_t>(to.size());
        ++count;
    }
}

// Finds "chr/chNN/<name>" in `data` and, when `to` is given, rewrites <name> (same length).
// Returns how many it found.
size_t ChrPaths(std::vector<u8>* data, const std::string& name, const std::string* to) {
    const std::string needle = "/" + name;
    size_t count = 0;
    auto it = data->begin();
    while (true) {
        it = std::search(it, data->end(), needle.begin(), needle.end());
        if (it == data->end()) {
            return count;
        }
        const size_t pos = static_cast<size_t>(it - data->begin());
        const auto& d = *data;
        if (pos >= 8 && std::memcmp(&d[pos - 8], "chr/ch", 6) == 0 && std::isdigit(d[pos - 2]) &&
            std::isdigit(d[pos - 1])) {
            if (to) {
                std::copy(to->begin(), to->end(), it + 1);
            }
            ++count;
        }
        it += static_cast<std::ptrdiff_t>(needle.size());
    }
}

} // namespace

bool BuildRenamedPac(const PacArchive& base, const std::filesystem::path& out, std::string* error) {
    // Unpack every entry once (LZS ones unpacked, others as stored).
    std::map<std::string, std::vector<u8>> raw;
    std::map<std::string, std::vector<u8>> stored;
    for (const auto& e : base.Entries()) {
        std::vector<u8> data;
        if (!base.ReadEntry(e, &data)) {
            *error = "cannot read entry " + e.name;
            return false;
        }
        std::vector<u8> plain;
        if (IsLzs(data)) {
            if (!LzsUnpack(data, &plain)) {
                continue;
            }
        } else if (e.name.ends_with(".s")) {
            plain = data; // character scripts may be stored as is
        } else {
            continue;
        }
        raw[e.name] = std::move(plain);
        stored[e.name] = std::move(data);
    }
    // Everything the monster names by path ("chr/chNN/<entry>") gets a new name: its
    // textures, and also its model and animation set, which the game otherwise reuses from
    // whichever copy loaded first (with that copy's textures).
    std::set<std::string> referenced;
    for (const auto& e : base.Entries()) {
        for (auto& [name, data] : raw) {
            if (ChrPaths(&data, e.name, nullptr) > 0) {
                referenced.insert(e.name);
                break;
            }
        }
    }
    std::map<std::string, std::string> renames;
    for (const auto& name : referenced) {
        std::string to = name;
        to[0] = to[0] == 'z' ? 'y' : 'z';
        if (base.Find(to)) {
            continue; // would collide with an existing entry; leave it
        }
        renames[name] = to;
    }
    // The character script ("chNNa_00.s") is looked up by name; see mirror_patch.h.
    for (const auto& e : base.Entries()) {
        if (e.name.size() == 10 && e.name.starts_with("ch") && e.name.ends_with("a_00.s")) {
            std::string to = e.name;
            to[4] = 'z';
            if (!base.Find(to)) {
                renames[e.name] = to;
            }
        }
    }
    if (renames.empty()) {
        *error = "nothing to rename";
        return false;
    }
    std::map<std::string, PacBlob> replace;
    for (auto& [name, data] : raw) {
        size_t changed = 0;
        for (const auto& [from, to] : renames) {
            changed += ChrPaths(&data, from, &to);
            if (name.ends_with(".s")) {
                // The script loads the model and animations by entry name.
                changed += ReplaceAll(&data, from, to);
            }
        }
        if (changed == 0) {
            continue;
        }
        std::vector<u8> packed = IsLzs(stored[name]) ? LzsStore(data, stored[name]) : data;
        // Keep the name stored in the LZS header in step too.
        if (const auto r = renames.find(name); r != renames.end() && IsLzs(packed)) {
            const auto end =
                std::find(packed.begin() + 0x20, packed.begin() + ReadU32(&packed[8]), u8{0});
            if (static_cast<size_t>(end - (packed.begin() + 0x20)) == r->second.size()) {
                std::copy(r->second.begin(), r->second.end(), packed.begin() + 0x20);
            }
        }
        PacBlob blob;
        blob.data = std::move(packed);
        blob.stamp = base.Find(name)->stamp;
        replace[name] = std::move(blob);
    }
    const auto tmp = std::filesystem::path(out.string() + ".names");
    if (!BuildPatchedPac(base, replace, tmp, error)) {
        return false;
    }
    // Rename the entries in the table: names are stored once each, same length.
    {
        std::fstream f(tmp, std::ios::binary | std::ios::in | std::ios::out);
        const std::vector<u8>& head = base.Head();
        for (size_t i = 0; i < base.Entries().size(); ++i) {
            const auto r = renames.find(base.Entries()[i].name);
            if (r == renames.end()) {
                continue;
            }
            const u32 name_off = ReadU32(&head[kHeaderSize + i * kEntrySize + 16]);
            f.seekp(name_off);
            f.write(r->second.data(), static_cast<std::streamsize>(r->second.size()));
        }
        if (!f) {
            *error = "cannot rename entries in " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::remove(out, ec);
    std::filesystem::rename(tmp, out, ec);
    if (ec) {
        *error = "cannot move " + tmp.string() + " into place: " + ec.message();
        return false;
    }
    return true;
}

} // namespace Core::Godzilla
