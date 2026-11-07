// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <fstream>
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

} // namespace Core::Godzilla
