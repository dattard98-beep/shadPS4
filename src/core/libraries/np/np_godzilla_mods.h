// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Emulator side of Godzilla mod sharing (see core/godzilla/mod_sync.h).

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace Libraries::Np::GodzillaMods {

// Called from the signaling thread loop: moves packets and follows the current room.
void Tick();
// True while our ready flag should wait for downloads.
bool HoldReady();
// The match is loading (room data update after everyone is ready).
void OnMatchStart();
// Host file to read instead of the game's own, for a guest path being opened.
std::optional<std::filesystem::path> Redirect(const std::string& guest_path);
// Same, without counting it as an open (for stat).
std::optional<std::filesystem::path> PeekRedirect(const std::string& guest_path);

} // namespace Libraries::Np::GodzillaMods
