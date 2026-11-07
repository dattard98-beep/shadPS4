// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Reader and patcher for Godzilla PS4 ".pac" archives.
//
// Layout (little endian):
//   0x00  "add\0"        magic
//   0x04  u32 version     4
//   0x08  u32 header size 0x20
//   0x0c  u32 entry size  0x20
//   0x10  u32 count
//   0x14  u32 total size  (whole file)
//   0x20  count entries of 0x20 bytes: u32 offset, u32 size, u32 0, u32 stamp,
//         u32 name offset, 12 bytes zero
//   then the NUL-terminated names, then each entry's data (usually LZS compressed) at
//   32-byte aligned offsets.
//
// Entries are copied as stored; nothing here decompresses them.

#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"

namespace Core::Godzilla {

using Digest = std::array<u8, 32>; // SHA-256

std::string DigestHex(const Digest& d);
Digest Sha256(const u8* data, size_t len);

struct PacEntry {
    std::string name;
    u32 offset = 0;
    u32 size = 0;
    u32 stamp = 0;
};

class PacArchive {
public:
    // Reads the table of contents only.
    static std::optional<PacArchive> Open(const std::filesystem::path& path);

    const std::vector<PacEntry>& Entries() const {
        return entries_;
    }
    const std::filesystem::path& Path() const {
        return path_;
    }
    u64 FileSize() const {
        return file_size_;
    }
    const PacEntry* Find(const std::string& name) const;
    // Header, entry table and names, exactly as stored.
    const std::vector<u8>& Head() const {
        return head_;
    }

    // Reads one entry's stored bytes.
    bool ReadEntry(const PacEntry& entry, std::vector<u8>* out) const;
    // SHA-256 of one entry's stored bytes, cached per entry.
    std::optional<Digest> EntryDigest(const PacEntry& entry) const;

private:
    std::filesystem::path path_;
    u64 file_size_ = 0;
    u32 data_start_ = 0; // end of header, entries and names
    std::vector<u8> head_;
    std::vector<PacEntry> entries_;
    mutable std::map<std::string, Digest> digests_;
};

struct PacBlob {
    std::vector<u8> data;
    u32 stamp = 0;
};

// Writes a copy of `base` with the named entries' data replaced. Every name in `replace` must
// exist in `base`. Writes to a temporary file and renames it into place.
bool BuildPatchedPac(const PacArchive& base, const std::map<std::string, PacBlob>& replace,
                     const std::filesystem::path& out, std::string* error);

} // namespace Core::Godzilla
