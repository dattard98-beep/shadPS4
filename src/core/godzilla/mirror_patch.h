// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Godzilla PS4: lets two players use different versions of the same monster.
//
// Each player's character is set up from the script "chNNa_00.s", looked up by name among
// everything loaded. With the same monster twice, both lookups find the copy loaded first, so
// both players get one version. This patch changes the name the game builds for a chosen player
// slot ("chNNa_00.s" -> "chNNz_00.s"); the opponent's copy of the monster file carries its script
// under that name, pointing at its own renamed model and textures.

#pragma once

#include <functional>
#include <string>

#include "common/types.h"

namespace Core::Godzilla::MirrorPatch {

// Patches the game's character setup in the executable segment [code, code + file_size); new code
// goes in memory from allocate (within rel32 reach). Does nothing unless the expected instructions
// are found. Returns true when applied.
bool Apply(u8* code, u64 file_size, const std::function<u8*(u64)>& allocate, std::string* message);

bool Active();

// Letter the game puts in place of 'a' for this player slot (0 = unchanged).
void SetSlot(int slot, char letter);
void Reset();

} // namespace Core::Godzilla::MirrorPatch
