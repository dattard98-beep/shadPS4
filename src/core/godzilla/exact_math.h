// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Minus Two contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Godzilla PS4: makes the game's vector math give the same bits on every CPU.
//
// Online, each PC computes the whole fight from both players' input, so both must compute it
// alike. The game normalizes vectors with the reciprocal estimates vrsqrtps and vrcpps, whose
// last bits differ between Intel and AMD: an Intel PC and a Ryzen drift apart until one ends the
// match while the other still shows both players' health. This rewrites each of them, at load,
// to the exact value (vsqrtps then vdivps, or vdivps), which is correctly rounded on every CPU.
//
// Adapted from Minus Two (https://github.com/gminustwo/minustwo, GPL-2.0): the finding, the
// rules for telling code from data in the game's code segment and the stub layout come from its
// scripts/isa_rewrite.py and src/isa_rewrite.c.

#pragma once

#include <functional>
#include <string>

#include "common/types.h"

namespace Core::Godzilla::ExactMath {

struct Report {
    u32 found = 0;     // reciprocal estimates in code
    u32 rewritten = 0; // now computed exactly
    u32 skipped = 0;   // left as they are (no room for the jump)
    u32 other = 0;     // other estimate forms seen (scalar or SSE), not rewritten
};

// Allocates executable memory within rel32 reach of the code; nullptr when there is none.
using Allocator = std::function<u8*(u64 size)>;

// Scans the executable segment [code, code + size) and rewrites its reciprocal estimates.
Report Apply(u8* code, u64 size, const Allocator& allocate);

} // namespace Core::Godzilla::ExactMath
