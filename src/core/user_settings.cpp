// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <string>
#include <common/path_util.h>
#include <common/scm_rev.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "user_settings.h"

using json = nlohmann::json;

namespace {

// shadNet logins are also kept in their own small file. Tools that rewrite users.json without
// knowing about the shadNet fields (for example a launcher's user editor) would otherwise wipe
// them, leaving the player silently signed out. users.json stays the main copy; this file is
// only used to fill a login back in when users.json has lost it.
constexpr const char* kShadNetLoginsFile = "shadnet_logins.json";

std::filesystem::path ShadNetLoginsPath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / kShadNetLoginsFile;
}

json ReadShadNetLogins() {
    std::ifstream in(ShadNetLoginsPath());
    if (!in.good())
        return json::object();
    try {
        json j;
        in >> j;
        return j.is_object() ? j : json::object();
    } catch (...) {
        return json::object();
    }
}

// Stores every user that currently has a complete login. Entries are never removed here, so a
// later wiped users.json cannot erase the backup.
void BackupShadNetLogins(const Users& users) {
    json logins = ReadShadNetLogins();
    bool changed = false;
    for (const auto& u : users.user) {
        if (u.shadnet_npid.empty() || u.shadnet_password.empty())
            continue;
        json entry = {{"shadnet_enabled", u.shadnet_enabled},
                      {"shadnet_npid", u.shadnet_npid},
                      {"shadnet_password", u.shadnet_password},
                      {"shadnet_token", u.shadnet_token},
                      {"shadnet_email", u.shadnet_email}};
        const std::string key = std::to_string(u.user_id);
        if (!logins.contains(key) || logins[key] != entry) {
            logins[key] = std::move(entry);
            changed = true;
        }
    }
    if (!changed)
        return;
    std::ofstream out(ShadNetLoginsPath());
    if (!out) {
        LOG_ERROR(Config, "Failed to write {}", kShadNetLoginsFile);
        return;
    }
    out << std::setw(2) << logins;
}

// Fills in logins that users.json has lost. Returns true if anything was restored.
bool RestoreShadNetLogins(Users& users) {
    const json logins = ReadShadNetLogins();
    bool restored = false;
    for (auto& u : users.user) {
        if (!u.shadnet_npid.empty() && !u.shadnet_password.empty())
            continue;
        const std::string key = std::to_string(u.user_id);
        if (!logins.contains(key) || !logins[key].is_object())
            continue;
        const json& e = logins[key];
        const std::string npid = e.value("shadnet_npid", "");
        const std::string password = e.value("shadnet_password", "");
        if (npid.empty() || password.empty())
            continue;
        u.shadnet_npid = npid;
        u.shadnet_password = password;
        u.shadnet_token = e.value("shadnet_token", "");
        u.shadnet_email = e.value("shadnet_email", "");
        u.shadnet_enabled = e.value("shadnet_enabled", true);
        LOG_WARNING(Config,
                    "users.json had lost the shadNet login for user {} ('{}'); restored it from {}",
                    u.user_id, npid, kShadNetLoginsFile);
        restored = true;
    }
    return restored;
}

} // namespace

// Singleton storage
std::shared_ptr<UserSettingsImpl> UserSettingsImpl::s_instance = nullptr;
std::mutex UserSettingsImpl::s_mutex;

// Singleton
UserSettingsImpl::UserSettingsImpl() = default;

UserSettingsImpl::~UserSettingsImpl() {}

std::shared_ptr<UserSettingsImpl> UserSettingsImpl::GetInstance() {
    std::lock_guard lock(s_mutex);
    if (!s_instance)
        s_instance = std::make_shared<UserSettingsImpl>();
    return s_instance;
}

void UserSettingsImpl::SetInstance(std::shared_ptr<UserSettingsImpl> instance) {
    std::lock_guard lock(s_mutex);
    s_instance = std::move(instance);
}

bool UserSettingsImpl::Save() const {
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "users.json";
    try {
        json j;
        j["Users"] = m_userManager.GetUsers();
        j["Users"]["commit_hash"] = std::string(Common::g_scm_rev);

        json existing = json::object();
        if (std::ifstream existingIn{path}; existingIn.good()) {
            try {
                existingIn >> existing;
            } catch (...) {
                existing = json::object();
            }
        }

        if (existing.contains("Users") && existing["Users"].is_object())
            existing["Users"].update(j["Users"]);
        else
            existing["Users"] = j["Users"];

        std::ofstream out(path);
        if (!out) {
            LOG_ERROR(Config, "Failed to open user settings for writing: {}", path.string());
            return false;
        }
        out << std::setw(2) << existing;
        BackupShadNetLogins(m_userManager.GetUsers());
        return !out.fail();
    } catch (const std::exception& e) {
        LOG_ERROR(Config, "Error saving user settings: {}", e.what());
        return false;
    }
}

bool UserSettingsImpl::Load() {
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "users.json";
    try {
        if (!std::filesystem::exists(path)) {
            if (m_userManager.GetUsers().user.empty())
                m_userManager.GetUsers() = m_userManager.CreateDefaultUsers();
            Save();
            return false;
        }

        std::ifstream in(path);
        if (!in) {
            LOG_ERROR(Config, "Failed to open user settings: {}", path.string());
            return false;
        }

        json j;
        in >> j;

        auto default_users = m_userManager.CreateDefaultUsers();
        json default_json;
        default_json["Users"] = default_users;

        if (j.contains("Users")) {
            json current = default_json["Users"];
            current.update(j["Users"]);
            m_userManager.GetUsers() = current.get<Users>();
        } else {
            m_userManager.GetUsers() = default_users;
        }

        const bool restored = RestoreShadNetLogins(m_userManager.GetUsers());
        BackupShadNetLogins(m_userManager.GetUsers());

        if (restored || m_userManager.GetUsers().commit_hash != Common::g_scm_rev)
            Save();

        return true;
    } catch (const std::exception& e) {
        UNREACHABLE_MSG("Error loading user settings: {}", e.what());
        if (m_userManager.GetUsers().user.empty())
            m_userManager.GetUsers() = m_userManager.CreateDefaultUsers();
        return false;
    }
}