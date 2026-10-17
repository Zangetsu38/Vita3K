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

#include <util/fs.h>

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

enum RegType {
    REG_TYPE_INT,
    REG_TYPE_STR,
    REG_TYPE_BIN,
};

struct RegValue {
    std::string name;
    RegType type;
    uint32_t size;
    std::vector<char> init_value;
};

struct RegMgrCallback {
    int32_t callback_id;
    int32_t owner_thread_id;
    bool started = false;
    std::vector<std::string> paths;
};

struct RegMgrCallbackNotification {
    int32_t callback_id;
    int32_t owner_thread_id;
    int32_t notify_arg;
};

struct RegMgrState {
    std::mutex mutex;
    std::mutex callback_mutex;
    fs::path system_dreg_path;
    bool system_dreg_valid = false;

    std::map<std::string, std::map<std::string, std::vector<char>>> system_dreg;
    std::vector<std::string> reg_category_template;
    std::map<std::string, std::vector<RegValue>> reg_template;
    std::array<std::optional<RegMgrCallback>, 16> callbacks;
};
