// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Minus Two contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <vector>

#include <Zydis/Zydis.h>

#include "core/godzilla/exact_math.h"

namespace Core::Godzilla::ExactMath {

namespace {

// A reciprocal estimate counts as code when the instructions around it decode as ordinary
// instructions and its destination is used soon after (in the game, at most 14 instructions on).
constexpr int CodeBefore = 4;
constexpr int CodeAfter = 24;
constexpr int UsedWithin = 24;
constexpr u32 JmpBytes = 5;
constexpr u32 StubBytes = 192;

enum class Kind { Rsqrt, Rcp };

struct Site {
    u64 offset;
    u32 length;
    Kind kind;
    bool ymm;
    int dst;
    int src;
    u64 reloc_offset = 0; // instruction moved into the stub (4-byte sites), 0 = none
    u32 reloc_length = 0;
    u32 reloc_disp_at = 0; // offset of its RIP-relative disp32, 0 = none
    bool unsupported = false;
};

struct Check {
    size_t site;
    int seen = 0;
    bool used = false;
};

bool LooksLikeCode(const ZydisDecodedInstruction& ins) {
    switch (ins.mnemonic) {
    case ZYDIS_MNEMONIC_INT:
    case ZYDIS_MNEMONIC_INT1:
    case ZYDIS_MNEMONIC_INTO:
    case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_IN:
    case ZYDIS_MNEMONIC_OUT:
    case ZYDIS_MNEMONIC_INSB:
    case ZYDIS_MNEMONIC_INSW:
    case ZYDIS_MNEMONIC_INSD:
    case ZYDIS_MNEMONIC_OUTSB:
    case ZYDIS_MNEMONIC_OUTSW:
    case ZYDIS_MNEMONIC_OUTSD:
    case ZYDIS_MNEMONIC_IRET:
    case ZYDIS_MNEMONIC_IRETD:
    case ZYDIS_MNEMONIC_IRETQ:
    case ZYDIS_MNEMONIC_CLI:
    case ZYDIS_MNEMONIC_STI:
        return false;
    case ZYDIS_MNEMONIC_RET:
        return ins.meta.branch_type != ZYDIS_BRANCH_TYPE_FAR;
    default:
        return true;
    }
}

int VectorNumber(ZydisRegister reg) {
    const auto cls = ZydisRegisterGetClass(reg);
    if (cls != ZYDIS_REGCLASS_XMM && cls != ZYDIS_REGCLASS_YMM && cls != ZYDIS_REGCLASS_ZMM) {
        return -1;
    }
    return ZydisRegisterGetId(reg);
}

bool NamesVector(const ZydisDecodedInstruction& ins, const ZydisDecodedOperand* ops, int number) {
    for (int i = 0; i < ins.operand_count_visible; ++i) {
        if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            VectorNumber(ops[i].reg.value) == number) {
            return true;
        }
    }
    return false;
}

bool IsEstimate(ZydisMnemonic m) {
    return m == ZYDIS_MNEMONIC_VRSQRTPS || m == ZYDIS_MNEMONIC_VRCPPS;
}

bool IsOtherEstimate(ZydisMnemonic m) {
    return m == ZYDIS_MNEMONIC_RSQRTPS || m == ZYDIS_MNEMONIC_RCPPS ||
           m == ZYDIS_MNEMONIC_RSQRTSS || m == ZYDIS_MNEMONIC_RCPSS ||
           m == ZYDIS_MNEMONIC_VRSQRTSS || m == ZYDIS_MNEMONIC_VRCPSS;
}

// Can be moved into a stub: falls through, no prefixes that matter to placement.
bool IsPlain(const ZydisDecodedInstruction& ins, const ZydisDecodedOperand* ops) {
    if (ins.meta.category == ZYDIS_CATEGORY_COND_BR ||
        ins.meta.category == ZYDIS_CATEGORY_UNCOND_BR || ins.meta.category == ZYDIS_CATEGORY_CALL ||
        ins.meta.category == ZYDIS_CATEGORY_RET || ins.meta.category == ZYDIS_CATEGORY_SYSTEM ||
        ins.meta.category == ZYDIS_CATEGORY_INTERRUPT) {
        return false;
    }
    if (ins.attributes &
        (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE |
         ZYDIS_ATTRIB_HAS_LOCK | ZYDIS_ATTRIB_HAS_SEGMENT_FS | ZYDIS_ATTRIB_HAS_SEGMENT_GS)) {
        return false;
    }
    for (int i = 0; i < ins.operand_count; ++i) {
        if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[i].reg.value == ZYDIS_REGISTER_RIP) {
            return false;
        }
    }
    return !IsEstimate(ins.mnemonic);
}

class Emitter {
public:
    explicit Emitter(u8* at) : start(at), p(at) {}

    void Byte(u8 b) {
        *p++ = b;
    }
    void U32(u32 v) {
        std::memcpy(p, &v, 4);
        p += 4;
    }
    void Bytes(const u8* data, size_t n) {
        std::memcpy(p, data, n);
        p += n;
    }

    // Three-byte VEX, map 0F. rm: register number, or Rsp / Rip for [rsp] / [rip+disp32].
    static constexpr int Rsp = -1;
    static constexpr int Rip = -2;
    u8* Vex(u8 opcode, int pp, bool l, int reg, int vvvv, int rm) {
        const int b = rm >= 0 ? (rm >> 3) & 1 : 0;
        Byte(0xc4);
        Byte(static_cast<u8>((((reg >> 3) & 1) ^ 1) << 7 | 1 << 6 | (b ^ 1) << 5 | 1));
        Byte(static_cast<u8>((~vvvv & 15) << 3 | (l ? 1 : 0) << 2 | pp));
        Byte(opcode);
        if (rm == Rsp) {
            Byte(static_cast<u8>((reg & 7) << 3 | 4));
            Byte(0x24);
            return nullptr;
        }
        if (rm == Rip) {
            Byte(static_cast<u8>((reg & 7) << 3 | 5));
            u8* disp = p;
            U32(0);
            return disp;
        }
        Byte(static_cast<u8>(0xc0 | (reg & 7) << 3 | (rm & 7)));
        return nullptr;
    }

    bool Jmp(const u8* target) {
        const s64 rel = target - (p + JmpBytes);
        if (rel < std::numeric_limits<s32>::min() || rel > std::numeric_limits<s32>::max()) {
            return false;
        }
        Byte(0xe9);
        U32(static_cast<u32>(static_cast<s32>(rel)));
        return true;
    }

    u8* start;
    u8* p;
};

std::vector<Site> Scan(const u8* code, u64 size, u32* other) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

    std::vector<Site> sites;
    std::set<u64> targets;
    std::set<size_t> data;
    std::deque<bool> before;
    std::vector<Check> checks;
    struct Candidate {
        size_t site;
        u64 offset;
        u32 length;
        u32 disp_at;
    };
    std::vector<Candidate> candidates;
    std::optional<size_t> pending;

    ZydisDecodedInstruction ins;
    std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> ops;
    u64 offset = 0;
    while (offset < size) {
        const bool valid = ZYAN_SUCCESS(
            ZydisDecoderDecodeFull(&decoder, code + offset, size - offset, &ins, ops.data()));
        const u32 length = valid ? ins.length : 1;
        const bool code_like = valid && LooksLikeCode(ins);

        if (valid &&
            (ins.meta.category == ZYDIS_CATEGORY_COND_BR ||
             ins.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
             ins.meta.category == ZYDIS_CATEGORY_CALL) &&
            ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[0].imm.is_relative) {
            targets.insert(offset + length + ops[0].imm.value.s);
        }

        for (auto& check : checks) {
            ++check.seen;
            check.used = check.used || (check.seen <= UsedWithin && valid &&
                                        NamesVector(ins, ops.data(), sites[check.site].dst));
            if (!code_like || (check.seen == UsedWithin && !check.used)) {
                data.insert(check.site);
            }
        }
        std::erase_if(checks,
                      [&](const Check& c) { return c.seen >= CodeAfter || data.contains(c.site); });

        if (pending && valid && IsPlain(ins, ops.data()) && offset + length <= size) {
            u32 disp_at = 0;
            if (ins.attributes & ZYDIS_ATTRIB_IS_RELATIVE) {
                disp_at = ins.raw.disp.offset;
                if (ins.raw.disp.size != 32) {
                    disp_at = 0xffffffff;
                }
            }
            if (disp_at != 0xffffffff) {
                candidates.push_back({*pending, offset, length, disp_at});
            }
        }
        pending.reset();

        if (valid && IsOtherEstimate(ins.mnemonic)) {
            ++*other;
        }
        const bool all_before = std::ranges::all_of(before, [](bool b) { return b; });
        if (valid && IsEstimate(ins.mnemonic) && all_before &&
            ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const auto cls = ZydisRegisterGetClass(ops[0].reg.value);
            Site site{
                .offset = offset,
                .length = length,
                .kind = ins.mnemonic == ZYDIS_MNEMONIC_VRSQRTPS ? Kind::Rsqrt : Kind::Rcp,
                .ymm = cls == ZYDIS_REGCLASS_YMM,
                .dst = VectorNumber(ops[0].reg.value),
                .src = VectorNumber(ops[1].reg.value),
            };
            if ((cls == ZYDIS_REGCLASS_XMM || cls == ZYDIS_REGCLASS_YMM) && site.dst < 16 &&
                site.src < 16) {
                site.unsupported = length < JmpBytes;
                sites.push_back(site);
                checks.push_back({sites.size() - 1});
                if (length == 4) {
                    pending = sites.size() - 1;
                }
            }
        }
        before.push_back(code_like);
        if (before.size() > CodeBefore) {
            before.pop_front();
        }
        offset += length;
    }
    for (const auto& check : checks) {
        data.insert(check.site);
    }
    for (const auto& c : candidates) {
        if (!targets.contains(c.offset)) {
            auto& site = sites[c.site];
            site.reloc_offset = c.offset;
            site.reloc_length = c.length;
            site.reloc_disp_at = c.disp_at;
            site.unsupported = false;
        }
    }
    std::vector<Site> result;
    for (size_t i = 0; i < sites.size(); ++i) {
        if (!data.contains(i)) {
            result.push_back(sites[i]);
        }
    }
    return result;
}

bool EmitStub(u8* stub, u8* code, const Site& s) {
    static constexpr std::array<u8, 8> LeaRspDown{0x48, 0x8d, 0xa4, 0x24, 0x60, 0xff, 0xff, 0xff};
    static constexpr std::array<u8, 8> LeaRspUp{0x48, 0x8d, 0xa4, 0x24, 0xa0, 0x00, 0x00, 0x00};
    int t = 15;
    while (t == s.dst || t == s.src) {
        --t;
    }
    Emitter e(stub);
    e.Bytes(LeaRspDown.data(), LeaRspDown.size());
    e.Vex(0x7f, 2, true, t, 0, Emitter::Rsp);
    u8* ones;
    if (s.kind == Kind::Rsqrt) {
        e.Vex(0x51, 0, s.ymm, t, 0, s.src);
        ones = e.Vex(0x10, 0, s.ymm, s.dst, 0, Emitter::Rip);
        e.Vex(0x5e, 0, s.ymm, s.dst, s.dst, t);
    } else {
        ones = e.Vex(0x10, 0, s.ymm, t, 0, Emitter::Rip);
        e.Vex(0x5e, 0, s.ymm, s.dst, t, s.src);
    }
    e.Vex(0x6f, 2, true, t, 0, Emitter::Rsp);
    e.Bytes(LeaRspUp.data(), LeaRspUp.size());

    u64 resume = s.offset + s.length;
    if (s.reloc_length) {
        u8* copy = e.p;
        e.Bytes(code + s.reloc_offset, s.reloc_length);
        if (s.reloc_disp_at) {
            s32 disp;
            std::memcpy(&disp, copy + s.reloc_disp_at, 4);
            const s64 moved = static_cast<s64>(disp) + ((code + s.reloc_offset) - copy);
            if (moved < std::numeric_limits<s32>::min() ||
                moved > std::numeric_limits<s32>::max()) {
                return false;
            }
            const s32 fixed = static_cast<s32>(moved);
            std::memcpy(copy + s.reloc_disp_at, &fixed, 4);
        }
        resume = s.reloc_offset + s.reloc_length;
    }
    if (!e.Jmp(code + resume)) {
        return false;
    }
    while ((e.p - e.start) % 32) {
        e.Byte(0xcc);
    }
    const s32 rel = static_cast<s32>(e.p - (ones + 4));
    std::memcpy(ones, &rel, 4);
    for (int i = 0; i < 8; ++i) {
        e.U32(0x3f800000);
    }
    return e.p - e.start <= StubBytes;
}

} // namespace

Report Apply(u8* code, u64 size, const Allocator& allocate) {
    Report report;
    auto sites = Scan(code, size, &report.other);
    report.found = static_cast<u32>(sites.size());
    if (sites.empty()) {
        return report;
    }
    u8* stubs = allocate(sites.size() * StubBytes);
    if (!stubs) {
        report.skipped = report.found;
        return report;
    }
    for (size_t i = 0; i < sites.size(); ++i) {
        const auto& s = sites[i];
        if (s.unsupported) {
            ++report.skipped;
            continue;
        }
        u8* stub = stubs + i * StubBytes;
        if (!EmitStub(stub, code, s)) {
            ++report.skipped;
            continue;
        }
        Emitter site(code + s.offset);
        if (!site.Jmp(stub)) {
            ++report.skipped;
            continue;
        }
        const u64 end = s.reloc_length ? s.reloc_offset + s.reloc_length : s.offset + s.length;
        while (site.p < code + end) {
            site.Byte(0x90);
        }
        ++report.rewritten;
    }
    return report;
}

} // namespace Core::Godzilla::ExactMath
