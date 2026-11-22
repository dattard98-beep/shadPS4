// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Godzilla PS4: reads the skin a player picked with the Kaiju Skin Select mod.
//
// The mod (a game patch plus extra monster files chNNb_00.pac, chNNc_00.pac, ... in the game's
// -mods folder) keeps one byte per kaiju in game memory, the skin picked for it (0 = the
// original). Whenever the game builds a monster file name, the patch puts 'a' + that byte in
// place of the 'a'. The choice only exists on that player's PC, so mod sync sends it along.

#pragma once

#include "common/types.h"

namespace Core::Godzilla::SkinSelect {

// Called at load with the address of the mod's table when its patch is in the game code.
void SetTable(const volatile u8* table);

// Letter of the file the local game loads for this kaiju: 'a', or the picked skin's letter.
char Letter(u8 monster);

} // namespace Core::Godzilla::SkinSelect
