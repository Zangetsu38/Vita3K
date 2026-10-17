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

#include <mem/ptr.h>
#include <mem/state.h>
#include <regmgr/state.h>
#include <regmgr/types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace regmgr {

enum class RegMgrSetResult {
    success,
    invalid_value,
    no_access,
    io_error,
};

void reset_regmgr(RegMgrState &regmgr);
void init_regmgr(RegMgrState &regmgr, const fs::path &vita_fs_path);

// Returns failure on an invalid or inconsistent registry or an I/O failure. Both calls
// require the loaded registry to match disk. The three-argument overload can
// preserve the original as system.dreg.bak; an existing backup is never replaced.
// A backup contains private registry data and remains even if replacement fails.
// Failure to synchronize the parent directory can be reported after replacement;
// in that case the loaded account ID has already been updated.
std::optional<std::array<uint8_t, 8>> get_account_id(RegMgrState &regmgr);
bool set_account_id(RegMgrState &regmgr, const std::array<uint8_t, 8> &account_id);
bool set_account_id(RegMgrState &regmgr, const std::array<uint8_t, 8> &account_id, bool backup_original);

void reset_callback_registrations(RegMgrState &regmgr);
uint32_t register_callback(RegMgrState &regmgr, int32_t callback_id, int32_t owner_thread_id);
bool callback_registered(RegMgrState &regmgr, int32_t callback_id, int32_t owner_thread_id);
uint32_t add_registry_callback(RegMgrState &regmgr, int32_t callback_id, int32_t owner_thread_id, const std::string &path);
uint32_t start_callback(RegMgrState &regmgr, int32_t callback_id, int32_t owner_thread_id);
uint32_t stop_callback(RegMgrState &regmgr, int32_t callback_id, int32_t owner_thread_id);
uint32_t unregister_callback(RegMgrState &regmgr, int32_t callback_id, int32_t owner_thread_id);
std::vector<RegMgrCallbackNotification> callbacks_for_key(RegMgrState &regmgr, const std::string &category, const std::string &name);

int32_t set_result_to_error(RegMgrSetResult result);
int32_t is_blue_screen_error(int32_t error);
int32_t get_key_type_error(RegMgrState &regmgr, const std::string &category, const std::string &name, RegType expected_type);

// Setters return false for invalid input, an inconsistent registry, or an I/O failure.
// After a directory synchronization failure, the replacement may already be visible.
// Getters and setters for binary values
bool category_exists(RegMgrState &regmgr, const std::string &category);
std::optional<RegValue> get_value_info(RegMgrState &regmgr, const std::string &category, const std::string &name);
std::optional<uint32_t> get_value_size(RegMgrState &regmgr, const std::string &category, const std::string &name, RegType type);
bool get_bin_value(RegMgrState &regmgr, const std::string &category, const std::string &name, void *buf, uint32_t bufSize);
bool set_bin_value(RegMgrState &regmgr, const std::string &category, const std::string &name, const void *buf, uint32_t bufSize);
RegMgrSetResult set_bin_value_result(RegMgrState &regmgr, const std::string &category, const std::string &name, const void *buf, uint32_t bufSize);

// Getters and setters for int values
int32_t get_int_value(RegMgrState &regmgr, const std::string &category, const std::string &name);
bool set_int_value(RegMgrState &regmgr, const std::string &category, const std::string &name, const int32_t value);
RegMgrSetResult set_int_value_result(RegMgrState &regmgr, const std::string &category, const std::string &name, int32_t value);
void ensure_pstv_ethernet_profile(RegMgrState &regmgr, bool pstv_mode);

// Getters and setters for string values
std::string get_str_value(RegMgrState &regmgr, const std::string &category, const std::string &name);
bool set_str_value(RegMgrState &regmgr, const std::string &category, const std::string &name, const char *value, const uint32_t bufSize);
RegMgrSetResult set_str_value_result(RegMgrState &regmgr, const std::string &category, const std::string &name, const char *value, uint32_t bufSize);

// Getters of category and name by id
std::pair<std::string, std::string> get_category_and_name_by_id(const int id, const std::string &export_name);

} // namespace regmgr

namespace sce_regmgr {
int32_t copy_from_guest(MemState &mem, Address address, void *destination, uint32_t size);
int32_t copy_to_guest(MemState &mem, Address address, const void *source, uint32_t size);
int32_t copy_padded_to_guest(MemState &mem, Address address, const void *source, uint32_t source_size, uint32_t size);
int32_t copy_guest_c_string(MemState &mem, Address address, const std::string &source, uint32_t size);
int32_t copy_guest_string(MemState &mem, Ptr<const char> source, std::string &destination, size_t max_size = 0x100);
} // namespace sce_regmgr
