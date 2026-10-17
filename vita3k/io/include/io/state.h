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
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <io/VitaIoDevice.h>
#include <io/filesystem.h>
#include <io/types.h>
#include <io/util.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

class SimpleEvent;

struct AsyncIoRequest {
    std::shared_ptr<SimpleEvent> event;
    std::thread worker;
    std::function<void()> cleanup;
    bool finished = false;

    ~AsyncIoRequest();
};

// Class for all needed information to access files on Vita3K.
class FileStats : public VitaStats {
    // Shared file pointer
    FilePtr wrapped_file;
    std::shared_ptr<std::mutex> operations_mutex = std::make_shared<std::mutex>();

public:
    // Constructor used for files
    // Based on https://codereview.stackexchange.com/questions/4679/
    explicit FileStats(const char *vita, const std::string &t, const fs::path &file, const int open) {
        wrapped_file = create_shared_file(file, open);

        file_info.vita_loc = vita;
        file_info.translated = t;
        file_info.sys_loc = file;
        file_info.open_mode = open;
        file_info.file_mode = SCE_SO_IFREG | SCE_SO_IROTH;
        file_info.access_mode = SCE_S_IFREG;
    }

    bool is_regular_file() const {
        return file_info.file_mode & SCE_SO_IFREG;
    }

    // Check if the file is writable
    bool can_write_file() const {
        if (!is_regular_file())
            return false;

        return can_write(file_info.open_mode);
    }

    // File operations
    FILE *get_file_pointer() const {
        return wrapped_file.get();
    }

    std::unique_lock<std::mutex> lock_operations() const {
        return std::unique_lock<std::mutex>(*operations_mutex);
    }

    // File functions
    SceOff read(void *input_data, int element_size, SceSize element_count) const;
    SceOff write(const void *data, SceSize size, int count) const;
    int truncate(const SceSize size) const;
    bool seek(SceOff offset, SceIoSeekMode seek_mode) const;
    SceOff tell() const;
};

// Class for implementing Directory structure; path names are wide for Windows, normal for else
class DirStats : public VitaStats {
    // Shared directory pointer
    std::vector<DirPtr> dir_ptrs;
    size_t dir_index = 0;
    std::unordered_set<std::string> seen_entries;

public:
    DirStats(const char *vita, const std::string &t, const fs::path &file, std::vector<DirPtr> ptrs) {
        dir_ptrs = std::move(ptrs);

        file_info.vita_loc = vita;
        file_info.translated = t;
        file_info.sys_loc = file;
        file_info.open_mode = SCE_O_RDONLY;
        file_info.file_mode = SCE_SO_IFDIR | SCE_SO_IROTH;
        file_info.access_mode = SCE_S_IFDIR | SCE_S_IRUSR;
    }

    auto get_dir_ptr() {
        using EntryPtr = decltype(get_system_dir_ptr(dir_ptrs.front()));
        while (dir_index < dir_ptrs.size()) {
            if (auto entry = get_system_dir_ptr(dir_ptrs[dir_index]))
                return entry;
            ++dir_index;
        }
        return EntryPtr{};
    }

    bool add_entry(const std::string &name) {
        return seen_entries.insert(name).second;
    }

    bool is_directory() const {
        return file_info.file_mode & SCE_SO_IFDIR;
    }
};

using TtyFiles = std::map<SceUID, TtyType>;
using StdFiles = std::map<SceUID, FileStats>;
using DirEntries = std::map<SceUID, DirStats>;

struct IOState {
    struct DevicePaths {
        std::string app0;
        VitaIoDevice app0_device = VitaIoDevice::_INVALID;
        std::string savedata0;
        std::string addcont0;
        std::string cache0;
        std::string user_id;
    } device_paths;

    std::string addcont;
    std::string content_id;
    std::string savedata;
    std::string title_id;
    std::string app_path;

    std::string user_id;
    std::string user_name;

    bool redirect_stdio;

    std::atomic<SceUID> next_fd = 0;
    std::mutex files_mutex;
    TtyFiles tty_files;
    StdFiles std_files;
    DirEntries dir_entries;

    std::mutex async_mutex;
    bool async_stopping = false;
    std::unordered_map<SceUID, std::shared_ptr<AsyncIoRequest>> async_requests;

    std::unordered_map<std::string, std::string> cachemap;
    bool case_isens_find_enabled = false;

    std::mutex appmeta_mutex;
    struct AppmetaMount {
        fs::path source;
        fs::path canonical_source;
        fs::path patch;
        fs::path canonical_patch;
    };
    std::unordered_map<std::string, AppmetaMount> appmeta_mounts;

    std::mutex overlay_mutex;
    SceUID next_overlay_id = 1;
    SceUID app_overlay_id = -1;
    // overlay in the order they should be applied
    std::vector<FiosOverlay> overlays;
};
