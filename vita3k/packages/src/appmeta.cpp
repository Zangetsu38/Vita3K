// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include <packages/functions.h>
#include <packages/sfo.h>
#include <util/log.h>
#include <vfs/mounts.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif

static std::error_code publish_without_replacement(const fs::path &source, const fs::path &destination) {
#ifdef _WIN32
    if (MoveFileW(source.c_str(), destination.c_str()))
        return {};
    return { static_cast<int>(GetLastError()), std::system_category() };
#elif defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) == 0)
        return {};
    return { errno, std::generic_category() };
#elif defined(__APPLE__)
    if (renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0)
        return {};
    return { errno, std::generic_category() };
#else
    return std::make_error_code(std::errc::operation_not_supported);
#endif
}

static void validate_sfo_file(const fs::path &path) {
    if (!fs::is_regular_file(fs::symlink_status(path)) || fs::file_size(path) > 32768)
        throw fs::filesystem_error("Missing or oversized appmeta param.sfo", path, std::make_error_code(std::errc::invalid_argument));
}

static void validate_patch(const fs::path &source, const std::string &title_id) {
    const auto path = source / "param.sfo";
    validate_sfo_file(path);
    std::vector<uint8_t> data;
    SfoFile file{};
    std::string actual_id, category;
    if (!fs_utils::read_data(path, data) || !sfo::load(file, data)
        || !sfo::get_data_by_key(actual_id, file, "TITLE_ID") || actual_id != title_id
        || !sfo::get_data_by_key(category, file, "CATEGORY") || !category.starts_with("gp"))
        throw fs::filesystem_error("Invalid patch appmeta param.sfo", path, std::make_error_code(std::errc::invalid_argument));
}

bool create_appmeta(IOState &io, const fs::path &vita_fs_path, const std::string &title_id, AppmetaInstallType type) {
    static std::mutex appmeta_mutex;
    const std::lock_guard lock(appmeta_mutex);
    if (title_id.empty() || title_id.size() > 32 || !std::all_of(title_id.begin(), title_id.end(), [](unsigned char ch) {
            return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
        })) {
        LOG_ERROR("Appmeta: invalid title ID '{}'; metadata not created", title_id);
        return false;
    }

    const auto ux0 = vita_fs_path / "ux0";
    const auto app = ux0 / "app" / title_id;
    const auto source = app / "sce_sys";
    const auto patch = ux0 / "patch" / title_id;
    const auto patch_source = patch / "sce_sys";
    const auto destination = ux0 / "appmeta" / title_id;
    fs::path staging;
    fs::path backup;
    try {
        const bool has_destination = fs::exists(fs::symlink_status(destination));
        if (has_destination && !fs::is_directory(fs::symlink_status(destination)))
            throw fs::filesystem_error("Cannot update invalid appmeta directory", destination, std::make_error_code(std::errc::operation_not_supported));
        const bool reuse_destination = type == AppmetaInstallType::Patch && has_destination;
        if (!reuse_destination) {
            if (!fs::is_directory(fs::symlink_status(app)) || !fs::is_directory(fs::symlink_status(source)))
                throw fs::filesystem_error("Invalid appmeta source directory", source, std::make_error_code(std::errc::not_a_directory));
            validate_sfo_file(source / "param.sfo");
        }
        const bool has_patch = fs::exists(fs::symlink_status(patch));
        if (type == AppmetaInstallType::Patch && !has_patch)
            throw fs::filesystem_error("Installed patch is missing", patch, std::make_error_code(std::errc::no_such_file_or_directory));
        if (has_patch) {
            if (!fs::is_directory(fs::symlink_status(patch)) || !fs::is_directory(fs::symlink_status(patch_source)))
                throw fs::filesystem_error("Invalid patch metadata directory", patch_source, std::make_error_code(std::errc::not_a_directory));
            validate_sfo_file(patch_source / "param.sfo");
        }

        static std::atomic_uint64_t sequence{ 0 };
        for (int attempt = 0; attempt < 128; ++attempt) {
            const auto candidate = ux0 / (".vita3k-appmeta-" + title_id + "-" + std::to_string(sequence++));
            if (fs::create_directory(candidate)) {
                staging = candidate;
                break;
            }
        }
        if (staging.empty())
            throw fs::filesystem_error("Cannot reserve appmeta staging directory", ux0, std::make_error_code(std::errc::file_exists));

        if (!pfsmgr::copy_appmeta(io, vita_fs_path, title_id, staging,
                reuse_destination ? destination : fs::path{}, type == AppmetaInstallType::Patch))
            throw std::runtime_error("Cannot copy mounted game metadata for appmeta: " + title_id);
        if (has_patch) {
            validate_patch(staging, title_id);
        }

        fs::create_directories(destination.parent_path());
        if (has_destination) {
            auto backup_candidate = staging;
            backup_candidate += "-backup";
            if (!fs::create_directory(backup_candidate))
                throw fs::filesystem_error("Cannot reserve appmeta backup directory", backup_candidate, std::make_error_code(std::errc::file_exists));
            backup = backup_candidate;
            fs::rename(destination, backup / "previous");
        }
        const auto error = publish_without_replacement(staging, destination);
        if (error) {
            if (has_destination) {
                const auto restore_error = publish_without_replacement(backup / "previous", destination);
                if (restore_error)
                    LOG_ERROR("Could not restore appmeta for {}; original preserved at {}: {}", title_id, backup / "previous", restore_error.message());
            }
            throw fs::filesystem_error("Cannot publish appmeta without replacing existing data", staging, destination, error);
        }
        if (!backup.empty()) {
            std::error_code cleanup_error;
            fs::remove_all(backup, cleanup_error);
            if (cleanup_error)
                LOG_ERROR("Could not clean appmeta backup directory {}: {}", backup, cleanup_error.message());
        }
        LOG_INFO("Appmeta prepared for {} at {} using {}{}; shell promotion is not emulated",
            title_id, destination, reuse_destination ? "existing metadata" : "application metadata",
            has_patch ? " with separate patch overrides" : "");
        return true;
    } catch (const std::runtime_error &error) {
        LOG_ERROR("Appmeta for {} was not created: {}", title_id, error.what());
        if (!staging.empty()) {
            std::error_code cleanup_error;
            fs::remove_all(staging, cleanup_error);
            if (cleanup_error)
                LOG_ERROR("Could not clean appmeta staging directory {}: {}", staging, cleanup_error.message());
        }
        if (!backup.empty()) {
            std::error_code cleanup_error;
            fs::remove(backup, cleanup_error);
            if (cleanup_error)
                LOG_WARN("Appmeta backup retained at {}: {}", backup, cleanup_error.message());
        }
        return false;
    }
}
