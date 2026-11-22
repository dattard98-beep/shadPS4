// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <fmt/format.h>

#include "core/godzilla/mirror_patch.h"

namespace Core::Godzilla::MirrorPatch {

namespace {

// Read by the patched game code directly (guest code runs natively in this process).
alignas(16) volatile u8 g_slots[4] = {};
bool g_active = false;

constexpr char kScriptFormat[] = "ch%02da_00.s";

// mov rax,[rbx+0x818]; movsx edx,word [rax]; lea rsi,[rip+fmt]; lea r14,[rbp-0x30]; xor eax,eax;
// mov rdi,r14; call sprintf; mov rdi,r14; call find_resource; test rax,rax
constexpr int kAny = -1;
constexpr std::array<int, 40> kPattern = {
    0x48, 0x8b, 0x83, 0x18, 0x08, 0x00, 0x00, 0x0f, 0xbf, 0x10, 0x48, 0x8d, 0x35, kAny,
    kAny, kAny, kAny, 0x4c, 0x8d, 0x75, 0xd0, 0x31, 0xc0, 0x4c, 0x89, 0xf7, 0xe8, kAny,
    kAny, kAny, kAny, 0x4c, 0x89, 0xf7, 0xe8, kAny, kAny, kAny, kAny, 0x48,
};
constexpr size_t kFmtDisp = 13;  // rel32 of lea rsi,[rip+fmt]
constexpr size_t kFindCall = 31; // mov rdi,r14; call find_resource (8 bytes)

s32 ReadS32(const u8* p) {
    s32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void WriteS32(u8* p, s32 v) {
    std::memcpy(p, &v, sizeof(v));
}

} // namespace

bool Apply(u8* code, u64 file_size, u64 mem_size, std::string* message) {
    if (g_active) {
        return true;
    }
    u8* site = nullptr;
    for (u64 i = 0; i + kPattern.size() <= file_size; ++i) {
        bool match = true;
        for (size_t k = 0; k < kPattern.size(); ++k) {
            if (kPattern[k] != kAny && code[i + k] != kPattern[k]) {
                match = false;
                break;
            }
        }
        if (!match) {
            continue;
        }
        const u8* fmt = code + i + kFmtDisp + 4 + ReadS32(code + i + kFmtDisp);
        if (fmt >= code && fmt + sizeof(kScriptFormat) <= code + file_size &&
            std::memcmp(fmt, kScriptFormat, sizeof(kScriptFormat)) == 0) {
            site = code + i;
            break;
        }
    }
    if (!site) {
        *message = "character setup code not found; same-monster mods stay off";
        return false;
    }

    // New code goes in the zero-filled tail of the executable segment.
    constexpr size_t kCaveSize = 64;
    const u64 cave_off = (file_size + 0xff) & ~u64{0xff};
    if (cave_off + kCaveSize > mem_size) {
        *message = "no room for the patch; same-monster mods stay off";
        return false;
    }
    u8* cave = code + cave_off;
    for (size_t k = 0; k < kCaveSize; ++k) {
        if (cave[k] != 0) {
            *message = "patch area is not free; same-monster mods stay off";
            return false;
        }
    }

    u8* call_site = site + kFindCall;
    u8* find_resource = call_site + 8 + ReadS32(call_site + 4);
    u8* resume = call_site + 8;

    std::array<u8, 47> c{};
    size_t n = 0;
    auto put = [&](std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            c[n++] = b;
        }
    };
    put({0x48, 0x63, 0x83, 0xd0, 0x01, 0x00, 0x00}); // movsxd rax,[rbx+0x1d0] (player slot)
    put({0x48, 0x83, 0xf8, 0x03});                   // cmp rax,3
    put({0x77, 0x15});                               // ja skip
    put({0x48, 0xb9});                               // mov rcx,imm64 (&g_slots)
    const u64 table = reinterpret_cast<u64>(&g_slots[0]);
    std::memcpy(&c[n], &table, sizeof(table));
    n += sizeof(table);
    put({0x8a, 0x04, 0x01});       // mov al,[rcx+rax]
    put({0x84, 0xc0});             // test al,al
    put({0x74, 0x04});             // jz skip
    put({0x41, 0x88, 0x46, 0x04}); // mov [r14+4],al ("chNNa" -> "chNNz")
    // skip:
    put({0x4c, 0x89, 0xf7}); // mov rdi,r14
    put({0xe8, 0, 0, 0, 0}); // call find_resource
    WriteS32(&c[n - 4], static_cast<s32>(find_resource - (cave + n)));
    put({0xe9, 0, 0, 0, 0}); // jmp resume
    WriteS32(&c[n - 4], static_cast<s32>(resume - (cave + n)));

    std::memcpy(cave, c.data(), n);
    u8 jump[8] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    WriteS32(&jump[1], static_cast<s32>(cave - (call_site + 5)));
    std::memcpy(call_site, jump, sizeof(jump));

    g_active = true;
    *message = fmt::format("same-monster mods enabled (patch at +{:#x}, code at +{:#x})",
                           call_site - code, cave_off);
    return true;
}

bool Active() {
    return g_active;
}

void SetSlot(int slot, char letter) {
    if (slot >= 0 && slot < 4) {
        g_slots[slot] = static_cast<u8>(letter);
    }
}

void Reset() {
    for (auto& s : g_slots) {
        s = 0;
    }
}

} // namespace Core::Godzilla::MirrorPatch
