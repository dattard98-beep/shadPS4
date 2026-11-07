// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Godzilla PS4 per-match mod sharing.
//
// When two players meet in an online room, each sends the other a list of the entries in the
// files of the monster it picked (chNNa_00.pac, plus that monster's entries in game_<lang>.pac),
// with a SHA-256 per entry. Each side asks for the entries that differ from its own copy, keeps
// them in a cache keyed by hash, and builds patched copies of the files. While the match loads,
// the opponent's monster files are read from those copies; the game folder is never changed.
//
// Messages travel as reliable ordered messages (rudp_core) over the P2P channel, so they take
// the same path as the match itself, direct or relayed.

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"

namespace Core::Godzilla::ModSync {

enum class LogLevel { Info, Warning, Error };

struct Hooks {
    // Sends one packet on the mod-sync P2P channel (address and port in network order).
    std::function<void(u32 addr, u16 port, const u8* data, size_t len)> send;
    std::function<void(LogLevel, const std::string&)> log;
};

struct PeerInfo {
    u16 member_id = 0;
    std::string name;
    u32 addr = 0; // network order, valid once connected
    u16 port = 0;
    u8 monster = 0; // chNN number, 0 = unknown
    bool connected = false;
};

class Manager {
public:
    Manager();
    ~Manager();

    void SetHooks(Hooks hooks);
    // game_dir: host folder of /app0. cache_dir: where downloads and patched files live.
    void Configure(const std::filesystem::path& game_dir, const std::filesystem::path& cache_dir);
    void SetEnabled(bool enabled);
    // File name of the language archive the game uses, e.g. "game_en.pac".
    void SetLanguageFile(const std::string& name);

    // Room state, polled from the matching cache.
    void SetRoom(u32 room_id, u16 my_member_id, u8 my_monster, const std::vector<PeerInfo>& peers);
    void ClearRoom();
    // The match is loading: start counting monster file opens again.
    void OnMatchStart();

    void OnPacket(u32 addr, u16 port, const u8* data, size_t len);
    void Tick();

    // True while the local player should not report ready yet.
    bool HoldReady();
    // Host path to read instead of the game's own file, for a guest path like
    // "/app0/ch01a_00.pac".
    // Each call for a monster file counts as one open; with peek set it only looks (for stat).
    std::optional<std::filesystem::path> Redirect(const std::string& guest_path, bool peek = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Manager& Instance();

// Header byte that marks a mod-sync packet on the P2P signaling channel ("SHAD" + this).
constexpr u8 kChannelTag = 0x40;

} // namespace Core::Godzilla::ModSync
