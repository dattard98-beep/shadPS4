// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/file_sys/fs.h"
#include "core/godzilla/mod_sync.h"
#include "core/libraries/net/net_p2p.h"
#include "core/libraries/np/np_godzilla_mods.h"
#include "core/libraries/np/np_handler/np_handler.h"
#include "core/libraries/np/np_matching2/np_matching2_internal.h"
#include "core/libraries/np/np_matching2/np_matching2_signaling.h"

namespace Libraries::Np::GodzillaMods {

namespace ModSync = Core::Godzilla::ModSync;
using namespace Libraries::Np::NpMatching2;

namespace {

constexpr OrbisNpMatching2AttributeId kMemberBinAttr = 0x59;
constexpr size_t kMonsterByte = 0x1f;
constexpr s32 kConnActive = 2;

std::once_flag g_init;
bool g_active = false;

std::string OnlineIdString(const Libraries::Np::OrbisNpOnlineId& id) {
    return std::string(id.data, strnlen(id.data, sizeof(id.data)));
}

std::string LanguageFileFromSettings() {
    switch (EmulatorSettings.GetConsoleLanguage()) {
    case 2:  // French
    case 22: // French (Canada)
        return "game_fr.pac";
    case 17: // Portuguese (Brazil)
        return "game_bp.pac";
    default:
        return "game_en.pac";
    }
}

void Init() {
    auto& m = ModSync::Instance();
    ModSync::Hooks hooks;
    hooks.send = [](u32 addr, u16 port, const u8* data, size_t len) {
        Net::P2PModSyncSendTo(data, static_cast<u32>(len), addr, port);
    };
    hooks.log = [](ModSync::LogLevel level, const std::string& text) {
        switch (level) {
        case ModSync::LogLevel::Info:
            LOG_INFO(Lib_NpMatching2, "{}", text);
            break;
        case ModSync::LogLevel::Warning:
            LOG_WARNING(Lib_NpMatching2, "{}", text);
            break;
        case ModSync::LogLevel::Error:
            LOG_ERROR(Lib_NpMatching2, "{}", text);
            break;
        }
    };
    m.SetHooks(std::move(hooks));
    const auto game_dir =
        Common::Singleton<Core::FileSys::MntPoints>::Instance()->GetHostPath("/app0");
    const auto cache_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "mod_cache";
    m.Configure(game_dir, cache_dir);
    m.SetLanguageFile(LanguageFileFromSettings());
    m.SetEnabled(EmulatorSettings.IsModSync());
    LOG_INFO(Lib_NpMatching2, "ModSync: {} (game folder {}, cache {})",
             EmulatorSettings.IsModSync() ? "on" : "off (mod_sync is false)", game_dir.string(),
             cache_dir.string());
}

void PollRoom() {
    auto& manager = ModSync::Instance();
    auto& handler = NpHandler::GetInstance();
    for (OrbisNpMatching2ContextId ctx_id = 1; ctx_id <= ContextManager::kMaxContexts; ++ctx_id) {
        const auto ctx = handler.GetMatching2ContextManager().Get(ctx_id);
        if (!ctx || ctx->room_id == 0) {
            continue;
        }
        std::vector<ModSync::PeerInfo> peers;
        u8 my_monster = 0;
        {
            auto cache = handler.LockMatching2Cache(ctx_id);
            const auto room_it = cache->rooms.find(ctx->room_id);
            if (room_it == cache->rooms.end()) {
                continue;
            }
            for (const auto& [member_id, member] : room_it->second.members) {
                u8 monster = 0;
                if (const auto bin = member.bins.find(kMemberBinAttr);
                    bin != member.bins.end() && bin->second.data.size() > kMonsterByte) {
                    monster = bin->second.data[kMonsterByte];
                }
                if (member_id == ctx->my_member_id) {
                    my_monster = monster;
                    continue;
                }
                ModSync::PeerInfo info;
                info.member_id = member_id;
                info.name = OnlineIdString(member.np_id.handle);
                info.monster = monster;
                if (const auto peer = cache->peers.find(member_id);
                    peer != cache->peers.end() && peer->second.status == kConnActive &&
                    peer->second.addr != 0) {
                    info.connected = true;
                    info.addr = peer->second.addr;
                    info.port = peer->second.port;
                }
                peers.push_back(std::move(info));
            }
        }
        manager.SetRoom(ctx->room_id, ctx->my_member_id, my_monster, peers);
        return;
    }
    manager.ClearRoom();
}

} // namespace

void Tick() {
    if (!OpenHostMatchingActive()) {
        return;
    }
    std::call_once(g_init, Init);
    g_active = true;
    static auto last_poll = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_poll >= std::chrono::milliseconds(200)) {
        last_poll = now;
        PollRoom();
    }
    auto& manager = ModSync::Instance();
    std::array<u8, 2048> buf;
    for (int i = 0; i < 512; ++i) {
        u32 addr = 0;
        u16 port = 0;
        const int n =
            Net::P2PModSyncRecvFrom(buf.data(), static_cast<u32>(buf.size()), &addr, &port);
        if (n <= 0) {
            break;
        }
        manager.OnPacket(addr, port, buf.data(), static_cast<size_t>(n));
    }
    manager.Tick();
}

bool HoldReady() {
    return g_active && ModSync::Instance().HoldReady();
}

void OnMatchStart() {
    if (g_active) {
        ModSync::Instance().OnMatchStart();
    }
}

std::optional<std::filesystem::path> Redirect(const std::string& guest_path) {
    if (!g_active || !guest_path.starts_with("/app0/") || !guest_path.ends_with(".pac")) {
        return std::nullopt;
    }
    return ModSync::Instance().Redirect(guest_path);
}

std::optional<std::filesystem::path> PeekRedirect(const std::string& guest_path) {
    if (!g_active || !guest_path.starts_with("/app0/") || !guest_path.ends_with(".pac")) {
        return std::nullopt;
    }
    return ModSync::Instance().Redirect(guest_path, true);
}

} // namespace Libraries::Np::GodzillaMods
