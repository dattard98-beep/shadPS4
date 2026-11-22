// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Minus Two contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/godzilla/label_wait.h"

namespace Core::Godzilla::LabelWait {

namespace {

struct Signature {
    std::string_view pattern;
    u32 jmp_at;
};

// Each loop from its jmp to the test, with the loads of the label and the buffer's fence value.
constexpr std::array<Signature, 4> Loops{{
    {"48 8b 10 43 8b 8c 3c d0 e7 00 00 eb 0b 66 90 ff 85 ?? ?? ?? ?? 48 8b 10 39 ca 78 f3", 11},
    {"48 8b 39 48 69 d2 f8 e7 00 00 41 8b b4 14 d0 e7 00 00 eb 09 ff 85 ?? ?? ?? ?? 48 8b 39 39 "
     "f7 78 f3",
     18},
    {"48 8b 39 48 69 d2 f8 e7 00 00 8b b4 10 d0 e7 00 00 eb 0d 0f 1f 40 00 ff 85 ?? ?? ?? ?? 48 "
     "8b 39 39 f7 78 f3",
     17},
    {"48 8b 10 43 8b 8c 25 d0 e7 00 00 4c 89 e7 4c 89 ee eb 0c 0f 1f 00 ff 85 ?? ?? ?? ?? 48 8b "
     "10 39 ca 78 f3",
     17},
}};

constexpr u32 StubBytes = 512;
constexpr u32 JmpBytes = 5;

struct Loop {
    u8* site; // the jmp into the loop
    u8* end;  // the instruction after the js
    int label;
    int target;
    int value;
};

std::mutex g_mutex;
std::condition_variable g_written;
std::atomic<u32> g_waiters{0};

bool Reached(const volatile u64* label, u32 target) {
    return static_cast<s32>(static_cast<u32>(*label) - target) >= 0;
}

PS4_SYSV_ABI void WaitLabel(const volatile u64* label, u32 target) {
    if (Reached(label, target)) {
        return;
    }
    g_waiters.fetch_add(1, std::memory_order_seq_cst);
    {
        std::unique_lock lock{g_mutex};
        // A write that does not notify still ends the wait, up to a millisecond late.
        while (!Reached(label, target)) {
            g_written.wait_for(lock, std::chrono::milliseconds(1));
        }
    }
    g_waiters.fetch_sub(1, std::memory_order_relaxed);
}

std::optional<u64> Find(const u8* code, u64 size, std::string_view pattern) {
    std::vector<int> bytes;
    for (size_t i = 0; i + 1 < pattern.size(); i += 3) {
        bytes.push_back(
            pattern[i] == '?' ? -1 : std::stoi(std::string(pattern.substr(i, 2)), nullptr, 16));
    }
    std::optional<u64> found;
    for (u64 at = 0; at + bytes.size() <= size; ++at) {
        bool match = true;
        for (size_t i = 0; i < bytes.size() && match; ++i) {
            match = bytes[i] < 0 || code[at + i] == bytes[i];
        }
        if (match) {
            if (found) {
                return std::nullopt; // not unique: leave it alone
            }
            found = at;
        }
    }
    return found;
}

std::optional<Loop> Decode(u8* site) {
    if (site[0] != 0xeb) {
        return std::nullopt;
    }
    u8* test = site + 2 + static_cast<s8>(site[1]);
    u8* end = test + 4;
    u8* again = end - 13;
    if (again < site + 2 || again[0] != 0xff || again[1] != 0x85 || again[6] != 0x48 ||
        again[7] != 0x8b || test[0] != 0x39 || end[-2] != 0x78 || end[-1] != 0xf3) {
        return std::nullopt;
    }
    const u8 load = again[8];
    const u8 compare = test[1];
    if ((load >> 6) != 0 || (load & 7) == 4 || (load & 7) == 5 || (compare >> 6) != 3 ||
        (compare & 7) != ((load >> 3) & 7)) {
        return std::nullopt;
    }
    Loop loop{site, end, load & 7, (compare >> 3) & 7, (load >> 3) & 7};
    if (loop.target == 4 || loop.target == 5 || loop.value == 4 || loop.value == 5) {
        return std::nullopt;
    }
    return loop;
}

void Put(u8*& p, std::initializer_list<u8> bytes) {
    for (const u8 b : bytes) {
        *p++ = b;
    }
}

void Put32(u8*& p, u32 v) {
    std::memcpy(p, &v, 4);
    p += 4;
}

bool Jmp(u8*& p, const u8* target) {
    const s64 rel = target - (p + JmpBytes);
    if (rel < std::numeric_limits<s32>::min() || rel > std::numeric_limits<s32>::max()) {
        return false;
    }
    *p++ = 0xe9;
    Put32(p, static_cast<u32>(static_cast<s32>(rel)));
    return true;
}

// Saves what a SysV call may change (general registers and xmm0-15), skips the red zone, calls
// WaitLabel(label, target), restores, then loads the label and compares it as the loop did.
bool EmitStub(u8* stub, const Loop& loop) {
    // Offsets from rbx after the pushes below: rax rcx rdx rbx - - rsi rdi.
    static constexpr std::array<u8, 8> Slot{72, 64, 56, 0, 0, 0, 48, 40};
    u8* p = stub;
    Put(p, {0x48, 0x8d, 0x64, 0x24, 0x80}); // lea rsp, [rsp - 0x80]
    Put(p, {0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x53});
    Put(p, {0x48, 0x89, 0xe3});                         // mov rbx, rsp
    Put(p, {0x48, 0x83, 0xe4, 0xf0});                   // and rsp, -16
    Put(p, {0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00}); // sub rsp, 0x100
    for (int i = 0; i < 16; ++i) {
        // movdqu [rsp + 16 * i], xmm_i
        *p++ = 0xf3;
        if (i >= 8) {
            *p++ = 0x44;
        }
        Put(p, {0x0f, 0x7f, static_cast<u8>(0x84 | (i & 7) << 3), 0x24});
        Put32(p, static_cast<u32>(16 * i));
    }
    Put(p, {0x48, 0x8b, 0x7b, Slot[loop.label]}); // mov rdi, [rbx + label]
    Put(p, {0x8b, 0x73, Slot[loop.target]});      // mov esi, [rbx + target]
    Put(p, {0x48, 0xb8});                         // movabs rax, WaitLabel
    const auto function = reinterpret_cast<u64>(&WaitLabel);
    std::memcpy(p, &function, 8);
    p += 8;
    Put(p, {0xff, 0xd0}); // call rax
    for (int i = 0; i < 16; ++i) {
        // movdqu xmm_i, [rsp + 16 * i]
        *p++ = 0xf3;
        if (i >= 8) {
            *p++ = 0x44;
        }
        Put(p, {0x0f, 0x6f, static_cast<u8>(0x84 | (i & 7) << 3), 0x24});
        Put32(p, static_cast<u32>(16 * i));
    }
    Put(p, {0x48, 0x89, 0xdc}); // mov rsp, rbx
    Put(p, {0x5b, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    Put(p, {0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00});              // lea rsp, [rsp + 0x80]
    Put(p, {0x48, 0x8b, static_cast<u8>(loop.value << 3 | loop.label)});   // mov value, [label]
    Put(p, {0x39, static_cast<u8>(0xc0 | loop.target << 3 | loop.value)}); // cmp value32, target32
    if (!Jmp(p, loop.end)) {
        return false;
    }
    return p - stub <= StubBytes;
}

} // namespace

u32 Apply(u8* code, u64 size, const ExactMath::Allocator& allocate) {
    std::vector<Loop> found;
    for (const auto& signature : Loops) {
        if (const auto at = Find(code, size, signature.pattern)) {
            if (const auto loop = Decode(code + *at + signature.jmp_at)) {
                found.push_back(*loop);
            }
        }
    }
    if (found.empty()) {
        return 0;
    }
    u8* stubs = allocate(found.size() * StubBytes);
    if (!stubs) {
        return 0;
    }
    u32 replaced = 0;
    for (size_t i = 0; i < found.size(); ++i) {
        const auto& loop = found[i];
        u8* stub = stubs + i * StubBytes;
        if (!EmitStub(stub, loop)) {
            continue;
        }
        u8* p = loop.site;
        if (!Jmp(p, stub)) {
            continue;
        }
        std::memset(p, 0xcc, loop.end - p);
        ++replaced;
    }
    return replaced;
}

void NotifyGpuWrite() {
    if (g_waiters.load(std::memory_order_seq_cst) != 0) {
        std::scoped_lock lock{g_mutex};
        g_written.notify_all();
    }
}

} // namespace Core::Godzilla::LabelWait
