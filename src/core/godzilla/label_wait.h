// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Minus Two contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Godzilla PS4: the render thread's waits for the GPU sleep instead of spinning.
//
// Before it reuses a command buffer, the game's render thread waits for the GPU's end-of-pipe
// label in four loops without a pause or a yield, which kept a CPU core busy for most of every
// frame. Each loop becomes a jump to a stub that sleeps until the command processor writes a
// label, then leaves the registers and flags as the loop did.
//
// Adapted from Minus Two (https://github.com/gminustwo/minustwo, GPL-2.0):
// src/runtime_label_wait.c and gpu/shim/mt_label_wait.cpp.

#pragma once

#include "core/godzilla/exact_math.h"

namespace Core::Godzilla::LabelWait {

// Returns how many of the four loops were replaced.
u32 Apply(u8* code, u64 size, const ExactMath::Allocator& allocate);

// Called by the command processor after it writes a fence or label.
void NotifyGpuWrite();

} // namespace Core::Godzilla::LabelWait
