// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>

#include "core/godzilla/skin_select.h"

namespace Core::Godzilla::SkinSelect {

namespace {
constexpr u8 TableSize = 0x18;
std::atomic<const volatile u8*> g_table{nullptr};
} // namespace

void SetTable(const volatile u8* table) {
    g_table = table;
}

char Letter(u8 monster) {
    const volatile u8* table = g_table;
    if (!table || monster >= TableSize) {
        return 'a';
    }
    const u8 skin = table[monster];
    return skin < 25 ? static_cast<char>('a' + skin) : 'a';
}

} // namespace Core::Godzilla::SkinSelect
