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

#include <module/module.h>

#include "SceRegMgr.h"

#include <kernel/callback.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <kernel/types.h>
#include <mem/ptr.h>
#include <regmgr/functions.h>
#include <regmgr/types.h>
#include <util/bytes.h>

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceRegMgr);

static SceInt32 get_callback_owner_thread(EmuEnvState &emuenv, SceUID caller_thread_id, SceUID callback_id, SceUID &owner_thread_id) {
    const auto caller = emuenv.kernel.get_thread(caller_thread_id);
    if (!caller)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    const auto callback = emuenv.kernel.objects.find<Callback>(callback_id);
    const auto owner = callback ? emuenv.kernel.get_thread(callback->thread_id) : nullptr;
    if (!owner || &caller->cpu->mem != &owner->cpu->mem)
        return SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID;
    owner_thread_id = callback->thread_id;
    return 0;
}

static SceInt32 get_caller_memory(EmuEnvState &emuenv, SceUID thread_id, MemState *&mem) {
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    mem = &caller->cpu->mem;
    return 0;
}

static uint32_t get_registry_key_type_error(EmuEnvState &emuenv, const std::string &category, const std::string &name, RegType expected_type) {
    return regmgr::get_key_type_error(emuenv.regmgr, category, name, expected_type);
}

static SceInt32 copy_keys_from_guest(MemState &mem, Ptr<const Keys> guest_keys, SceUInt32 count, std::vector<Keys> &keys) {
    constexpr SceUInt32 max_keys = 0x1000;
    if (count > max_keys)
        return SCE_REGMGR_ERROR_TOO_MANY_KEYS;
    const uint64_t byte_count = uint64_t(count) * sizeof(Keys);
    if (byte_count > std::numeric_limits<SceSize>::max())
        return SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE;
    if (!count)
        return 0;
    if (!guest_keys)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    keys.resize(count);
    return sce_regmgr::copy_from_guest(mem, guest_keys.address(), keys.data(), static_cast<SceSize>(byte_count));
}

namespace sce_regmgr {
void notify_callbacks(EmuEnvState &emuenv, const std::string &category, const std::string &name) {
    for (const auto &notification : regmgr::callbacks_for_key(emuenv.regmgr, category, name)) {
        const auto callback = emuenv.kernel.objects.find<Callback>(notification.callback_id);
        if (!callback || callback->thread_id != notification.owner_thread_id) {
            LOG_WARN_ONCE("Ignoring stale RegMgr callback {}", notification.callback_id);
            regmgr::unregister_callback(emuenv.regmgr, notification.callback_id, notification.owner_thread_id);
            continue;
        }
        const auto owner = emuenv.kernel.get_thread(notification.owner_thread_id);
        if (!owner) {
            LOG_WARN_ONCE("Ignoring RegMgr callback {} whose owner thread no longer exists", notification.callback_id);
            regmgr::unregister_callback(emuenv.regmgr, notification.callback_id, notification.owner_thread_id);
            continue;
        }
        owner->notify_callback(*callback, notification.notify_arg);
    }
}
} // namespace sce_regmgr

EXPORT(int, sceRegMgrAddRegistryCallback, SceUID callback_id, Ptr<const char> path) {
    TRACY_FUNC(sceRegMgrAddRegistryCallback, callback_id, path);
    SceUID owner_thread_id;
    const int owner_result = get_callback_owner_thread(emuenv, thread_id, callback_id, owner_thread_id);
    if (owner_result < 0)
        return RET_ERROR(owner_result);
    if (!regmgr::callback_registered(emuenv.regmgr, callback_id, owner_thread_id))
        return RET_ERROR(SCE_REGMGR_ERROR_CALLBACK_NOT_REGISTERED);
    if (!path)
        return RET_ERROR(SCE_REGMGR_ERROR_INVALID_ARGUMENT);

    const auto owner = emuenv.kernel.get_thread(owner_thread_id);
    if (!owner)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    std::string path_copy;
    const SceInt32 path_result = sce_regmgr::copy_guest_string(owner->cpu->mem, path, path_copy);
    if (path_result < 0)
        return RET_ERROR(path_result == SCE_REGMGR_ERROR_PATH_TOO_LONG ? path_result : SCE_KERNEL_ERROR_INVALID_MEMORY_ACCESS);
    const auto result = regmgr::add_registry_callback(emuenv.regmgr, callback_id, owner_thread_id, path_copy);
    if (result >= 0x80000000u)
        return RET_ERROR(result);
    return result;
}

EXPORT(int, sceRegMgrDbBackup, Ptr<const char> path, SceInt32 level) {
    TRACY_FUNC(sceRegMgrDbBackup, path, level);
    return UNIMPLEMENTED();
}

EXPORT(int, sceRegMgrDbRestore, Ptr<const char> path) {
    TRACY_FUNC(sceRegMgrDbRestore, path);
    return UNIMPLEMENTED();
}

EXPORT(int, sceRegMgrGetInitVals, Ptr<const char> category, Ptr<Keys> keys, const SceInt32 elementsNumber) {
    TRACY_FUNC(sceRegMgrGetInitVals, category, keys, elementsNumber);
    if (!category || !keys || elementsNumber < 1)
        return RET_ERROR(SCE_REGMGR_ERROR_INVALID_ARGUMENT);
    MemState *mem;
    if (const SceInt32 result = get_caller_memory(emuenv, thread_id, mem); result < 0)
        return RET_ERROR(result);
    std::string category_copy;
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, category, category_copy); result < 0)
        return RET_ERROR(result);
    if (!regmgr::category_exists(emuenv.regmgr, category_copy))
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_CATEGORY);
    std::vector<Keys> key_values;
    if (const SceInt32 result = copy_keys_from_guest(*mem, keys, static_cast<SceUInt32>(elementsNumber), key_values); result < 0)
        return RET_ERROR(result);
    for (auto &key : key_values) {
        if (key.size != sizeof(Keys))
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_KEYS_INFO);
        if (!key.name)
            return RET_ERROR(SCE_REGMGR_ERROR_INVALID_ARGUMENT);
        std::string name;
        if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, key.name, name); result < 0)
            return RET_ERROR(result);
        const auto info = regmgr::get_value_info(emuenv.regmgr, category_copy, name);
        if (!info)
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_NAME);
        if (key.type != info->type)
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_TYPE);
        if (key.valueSize < info->init_value.size())
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
        if (info->type == REG_TYPE_INT) {
            if (info->init_value.size() != sizeof(SceInt32))
                return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
            SceInt32 value;
            std::memcpy(&value, info->init_value.data(), sizeof(value));
            key.value = byte_swap(value);
        } else {
            if (!key.data)
                return RET_ERROR(SCE_REGMGR_ERROR_INVALID_ARGUMENT);
            if (const SceInt32 result = sce_regmgr::copy_to_guest(*mem, key.data, info->init_value.data(), static_cast<SceSize>(info->init_value.size())); result < 0)
                return RET_ERROR(result);
        }
    }
    if (!key_values.empty()) {
        const SceSize byte_count = static_cast<SceSize>(key_values.size() * sizeof(Keys));
        if (const SceInt32 result = sce_regmgr::copy_to_guest(*mem, keys.address(), key_values.data(), byte_count); result < 0)
            return RET_ERROR(result);
    }
    return 0;
}

EXPORT(int, sceRegMgrGetKeyBin, Ptr<const char> category, Ptr<const char> name, Ptr<void> buf, SceSize bufSize) {
    TRACY_FUNC(sceRegMgrGetKeyBin, category, name, buf, bufSize);
    MemState *mem;
    if (const SceInt32 result = get_caller_memory(emuenv, thread_id, mem); result < 0)
        return RET_ERROR(result);
    std::string category_copy;
    std::string name_copy;
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, category, category_copy); result < 0)
        return RET_ERROR(result);
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, name, name_copy); result < 0)
        return RET_ERROR(result);
    if (const SceInt32 result = regmgr::get_key_type_error(emuenv.regmgr, category_copy, name_copy, REG_TYPE_BIN); result < 0)
        return RET_ERROR(result);
    const auto info = regmgr::get_value_info(emuenv.regmgr, category_copy, name_copy);
    if (!info)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_NAME);
    if (bufSize < info->size)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
    if (!buf)
        return RET_ERROR(SCE_REGMGR_ERROR_INVALID_ARGUMENT);
    std::vector<uint8_t> value(info->size);
    if (!regmgr::get_bin_value(emuenv.regmgr, category_copy, name_copy, value.data(), info->size))
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_VALUE);
    if (const SceInt32 result = sce_regmgr::copy_to_guest(*mem, buf.address(), value.data(), info->size); result < 0)
        return RET_ERROR(result);

    return 0;
}

EXPORT(int, sceRegMgrGetKeyInt, Ptr<const char> category, Ptr<const char> name, Ptr<SceInt32> buf) {
    TRACY_FUNC(sceRegMgrGetKeyInt, category, name, buf);
    MemState *mem;
    if (const SceInt32 result = get_caller_memory(emuenv, thread_id, mem); result < 0)
        return RET_ERROR(result);
    std::string category_copy;
    std::string name_copy;
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, category, category_copy); result < 0)
        return RET_ERROR(result);
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, name, name_copy); result < 0)
        return RET_ERROR(result);
    if (const SceInt32 result = regmgr::get_key_type_error(emuenv.regmgr, category_copy, name_copy, REG_TYPE_INT); result < 0)
        return RET_ERROR(result);
    const auto info = regmgr::get_value_info(emuenv.regmgr, category_copy, name_copy);
    if (!info)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_NAME);
    if (info->size < sizeof(SceInt32))
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
    std::array<uint8_t, sizeof(SceInt32)> value_bytes;
    if (!regmgr::get_bin_value(emuenv.regmgr, category_copy, name_copy, value_bytes.data(), value_bytes.size()))
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_VALUE);
    const SceInt32 value = regmgr::get_int_value(emuenv.regmgr, category_copy, name_copy);
    if (const SceInt32 result = sce_regmgr::copy_to_guest(*mem, buf.address(), &value, sizeof(value)); result < 0)
        return RET_ERROR(result);

    return 0;
}

EXPORT(int, sceRegMgrGetKeyStr, Ptr<const char> category, Ptr<const char> name, Ptr<char> buf, const SceSize bufSize) {
    TRACY_FUNC(sceRegMgrGetKeyStr, category, name, buf, bufSize);
    MemState *mem;
    if (const SceInt32 result = get_caller_memory(emuenv, thread_id, mem); result < 0)
        return RET_ERROR(result);
    std::string category_copy;
    std::string name_copy;
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, category, category_copy); result < 0)
        return RET_ERROR(result);
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, name, name_copy); result < 0)
        return RET_ERROR(result);
    if (const SceInt32 result = regmgr::get_key_type_error(emuenv.regmgr, category_copy, name_copy, REG_TYPE_STR); result < 0)
        return RET_ERROR(result);
    const auto info = regmgr::get_value_info(emuenv.regmgr, category_copy, name_copy);
    if (!info)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_NAME);
    if (bufSize < info->size)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
    std::vector<char> value_bytes(info->size);
    if (!regmgr::get_bin_value(emuenv.regmgr, category_copy, name_copy, value_bytes.data(), info->size))
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_VALUE);
    const std::string value(value_bytes.begin(), value_bytes.end());
    if (const SceInt32 result = sce_regmgr::copy_guest_c_string(*mem, buf.address(), value, bufSize); result < 0)
        return RET_ERROR(result);

    return 0;
}

EXPORT(int, sceRegMgrGetKeys, Ptr<const char> category, Ptr<Keys> keys, const SceUInt32 elementsNumber) {
    TRACY_FUNC(sceRegMgrGetKeys, category, keys, elementsNumber);
    if (!keys || !elementsNumber)
        return RET_ERROR(SCE_REGMGR_ERROR_INVALID_ARGUMENT);
    MemState *mem;
    if (const SceInt32 result = get_caller_memory(emuenv, thread_id, mem); result < 0)
        return RET_ERROR(result);
    std::string category_copy;
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, category, category_copy); result < 0)
        return RET_ERROR(result);
    if (!regmgr::category_exists(emuenv.regmgr, category_copy))
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_CATEGORY);
    std::vector<Keys> key_values;
    if (const SceInt32 result = copy_keys_from_guest(*mem, keys, elementsNumber, key_values); result < 0)
        return RET_ERROR(result);
    for (auto &key : key_values) {
        if (key.size != sizeof(Keys))
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_KEYS_INFO);
        if (!key.name)
            return RET_ERROR(SCE_REGMGR_ERROR_INVALID_ARGUMENT);
        std::string name;
        if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, key.name, name); result < 0)
            return RET_ERROR(result);
        const auto info = regmgr::get_value_info(emuenv.regmgr, category_copy, name);
        if (!info)
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_NAME);
        if (key.type != info->type)
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_TYPE);
        if (key.valueSize < info->size)
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
        if (info->type == REG_TYPE_INT) {
            if (key.valueSize != sizeof(SceInt32) || info->size < sizeof(SceInt32))
                return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
            std::array<uint8_t, sizeof(SceInt32)> value_bytes;
            if (!regmgr::get_bin_value(emuenv.regmgr, category_copy, name, value_bytes.data(), value_bytes.size()))
                return RET_ERROR(SCE_REGMGR_ERROR_BAD_VALUE);
            key.value = regmgr::get_int_value(emuenv.regmgr, category_copy, name);
        } else {
            if (!key.data)
                return RET_ERROR(SCE_REGMGR_ERROR_BAD_VALUE);
            std::vector<uint8_t> value(info->size);
            if (!regmgr::get_bin_value(emuenv.regmgr, category_copy, name, value.data(), info->size))
                return RET_ERROR(SCE_REGMGR_ERROR_BAD_VALUE);
            if (const SceInt32 result = sce_regmgr::copy_to_guest(*mem, key.data, value.data(), info->size); result < 0)
                return RET_ERROR(result);
            key.valueSize = static_cast<SceUInt16>(info->size);
        }
    }
    if (!key_values.empty()) {
        const SceSize byte_count = static_cast<SceSize>(key_values.size() * sizeof(Keys));
        if (const SceInt32 result = sce_regmgr::copy_to_guest(*mem, keys.address(), key_values.data(), byte_count); result < 0)
            return RET_ERROR(result);
    }

    return 0;
}

EXPORT(int, sceRegMgrGetKeysInfo, Ptr<const char> category, Ptr<Keys> keys, const SceInt32 elementsNumber) {
    TRACY_FUNC(sceRegMgrGetKeysInfo, category, keys, elementsNumber);
    if (!keys || elementsNumber < 1)
        return RET_ERROR(SCE_REGMGR_ERROR_INVALID_ARGUMENT);
    MemState *mem;
    if (const SceInt32 result = get_caller_memory(emuenv, thread_id, mem); result < 0)
        return RET_ERROR(result);
    std::string category_copy;
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, category, category_copy); result < 0)
        return RET_ERROR(result);
    if (!regmgr::category_exists(emuenv.regmgr, category_copy))
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_CATEGORY);
    std::vector<Keys> key_values;
    if (const SceInt32 result = copy_keys_from_guest(*mem, keys, static_cast<SceUInt32>(elementsNumber), key_values); result < 0)
        return RET_ERROR(result);
    for (auto &key : key_values) {
        if (key.size != sizeof(Keys))
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_KEYS_INFO);
        if (!key.name)
            return RET_ERROR(SCE_REGMGR_ERROR_INVALID_ARGUMENT);
        std::string name;
        if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, key.name, name); result < 0)
            return RET_ERROR(result);
        const auto info = regmgr::get_value_info(emuenv.regmgr, category_copy, name);
        if (!info)
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_NAME);
        if (info->size > std::numeric_limits<SceUInt16>::max())
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
        key.type = static_cast<SceUInt16>(info->type);
        key.valueSize = static_cast<SceUInt16>(info->size);
    }
    if (!key_values.empty()) {
        const SceSize byte_count = static_cast<SceSize>(key_values.size() * sizeof(Keys));
        if (const SceInt32 result = sce_regmgr::copy_to_guest(*mem, keys.address(), key_values.data(), byte_count); result < 0)
            return RET_ERROR(result);
    }

    return 0;
}

EXPORT(int, sceRegMgrGetRegVersion) {
    TRACY_FUNC(sceRegMgrGetRegVersion);
    const auto info = regmgr::get_value_info(emuenv.regmgr, "/REGISTRY", "category_version");
    if (!info)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_NAME);
    if (info->type != REG_TYPE_INT)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_TYPE);
    if (info->size < sizeof(SceInt32))
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
    std::array<uint8_t, sizeof(SceInt32)> value_bytes;
    if (!regmgr::get_bin_value(emuenv.regmgr, "/REGISTRY", "category_version", value_bytes.data(), value_bytes.size()))
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_VALUE);
    return regmgr::get_int_value(emuenv.regmgr, "/REGISTRY", "category_version");
}

EXPORT(int, sceRegMgrIsBlueScreen, SceInt32 error) {
    TRACY_FUNC(sceRegMgrIsBlueScreen, error);
    return regmgr::is_blue_screen_error(error);
}

EXPORT(int, sceRegMgrRegisterCallback, SceUID callback_id) {
    TRACY_FUNC(sceRegMgrRegisterCallback, callback_id);
    SceUID owner_thread_id;
    const int owner_result = get_callback_owner_thread(emuenv, thread_id, callback_id, owner_thread_id);
    if (owner_result < 0)
        return RET_ERROR(owner_result);
    const auto result = regmgr::register_callback(emuenv.regmgr, callback_id, owner_thread_id);
    if (result >= 0x80000000u)
        return RET_ERROR(result);
    return result;
}

EXPORT(int, sceRegMgrRegisterDrvErrCallback, SceUID callback_id) {
    TRACY_FUNC(sceRegMgrRegisterDrvErrCallback, callback_id);
    return UNIMPLEMENTED();
}

EXPORT(int, sceRegMgrResetRegistryLv, Ptr<const char> category, SceInt32 level) {
    TRACY_FUNC(sceRegMgrResetRegistryLv, category, level);
    return UNIMPLEMENTED();
}

EXPORT(int, sceRegMgrSetKeyBin, Ptr<const char> category, Ptr<const char> name, Ptr<const void> buf, SceSize bufSize) {
    TRACY_FUNC(sceRegMgrSetKeyBin, category, name, buf, bufSize);
    MemState *mem;
    if (const SceInt32 result = get_caller_memory(emuenv, thread_id, mem); result < 0)
        return RET_ERROR(result);
    std::string category_copy;
    std::string name_copy;
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, category, category_copy); result < 0)
        return RET_ERROR(result);
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, name, name_copy); result < 0)
        return RET_ERROR(result);
    const uint32_t type_error = get_registry_key_type_error(emuenv, category_copy, name_copy, REG_TYPE_BIN);
    if (type_error)
        return RET_ERROR(type_error);
    const auto value_size = regmgr::get_value_size(emuenv.regmgr, category_copy, name_copy, REG_TYPE_BIN);
    if (!value_size || !bufSize || bufSize > *value_size)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
    std::vector<uint8_t> value(bufSize);
    if (const SceInt32 result = sce_regmgr::copy_from_guest(*mem, buf.address(), value.data(), bufSize); result < 0)
        return RET_ERROR(result);
    const auto set_result = regmgr::set_bin_value_result(emuenv.regmgr, category_copy, name_copy, value.data(), bufSize);
    if (set_result != regmgr::RegMgrSetResult::success)
        return RET_ERROR(regmgr::set_result_to_error(set_result));
    sce_regmgr::notify_callbacks(emuenv, category_copy, name_copy);
    return 0;
}

EXPORT(int, sceRegMgrSetKeyInt, Ptr<const char> category, Ptr<const char> name, const SceInt32 buf) {
    TRACY_FUNC(sceRegMgrSetKeyInt, category, name, buf);
    MemState *mem;
    if (const SceInt32 result = get_caller_memory(emuenv, thread_id, mem); result < 0)
        return RET_ERROR(result);
    std::string category_copy;
    std::string name_copy;
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, category, category_copy); result < 0)
        return RET_ERROR(result);
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, name, name_copy); result < 0)
        return RET_ERROR(result);
    const uint32_t type_error = get_registry_key_type_error(emuenv, category_copy, name_copy, REG_TYPE_INT);
    if (type_error) {
        LOG_ERROR("sceRegMgrSetKeyInt: registry key {}{} is missing or is not an integer", category_copy, name_copy);
        return RET_ERROR(type_error);
    }
    const auto value_size = regmgr::get_value_size(emuenv.regmgr, category_copy, name_copy, REG_TYPE_INT);
    if (!value_size)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_NAME);
    if (*value_size < sizeof(buf)) {
        LOG_ERROR("sceRegMgrSetKeyInt: integer registry key {}{} has invalid size {}", category_copy, name_copy, *value_size);
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
    }
    const auto set_result = regmgr::set_int_value_result(emuenv.regmgr, category_copy, name_copy, buf);
    if (set_result != regmgr::RegMgrSetResult::success)
        return RET_ERROR(regmgr::set_result_to_error(set_result));
    sce_regmgr::notify_callbacks(emuenv, category_copy, name_copy);
    return 0;
}

EXPORT(int, sceRegMgrSetKeyStr, Ptr<const char> category, Ptr<const char> name, Ptr<const char> buf, const SceSize bufSize) {
    TRACY_FUNC(sceRegMgrSetKeyStr, category, name, buf, bufSize);
    MemState *mem;
    if (const SceInt32 result = get_caller_memory(emuenv, thread_id, mem); result < 0)
        return RET_ERROR(result);
    std::string category_copy;
    std::string name_copy;
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, category, category_copy); result < 0)
        return RET_ERROR(result);
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, name, name_copy); result < 0)
        return RET_ERROR(result);
    const uint32_t type_error = get_registry_key_type_error(emuenv, category_copy, name_copy, REG_TYPE_STR);
    if (type_error)
        return RET_ERROR(type_error);
    const auto value_size = regmgr::get_value_size(emuenv.regmgr, category_copy, name_copy, REG_TYPE_STR);
    if (!value_size || !bufSize || bufSize > *value_size)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
    std::vector<char> value(bufSize);
    if (const SceInt32 result = sce_regmgr::copy_from_guest(*mem, buf.address(), value.data(), bufSize); result < 0)
        return RET_ERROR(result);
    const auto set_result = regmgr::set_str_value_result(emuenv.regmgr, category_copy, name_copy, value.data(), bufSize);
    if (set_result != regmgr::RegMgrSetResult::success)
        return RET_ERROR(regmgr::set_result_to_error(set_result));
    sce_regmgr::notify_callbacks(emuenv, category_copy, name_copy);
    return 0;
}

EXPORT(int, sceRegMgrSetKeys, Ptr<const char> category, Ptr<const Keys> keys, const SceInt32 elementsNumber) {
    TRACY_FUNC(sceRegMgrSetKeys, category, keys, elementsNumber);
    if (!keys || elementsNumber <= 0)
        return RET_ERROR(SCE_REGMGR_ERROR_INVALID_ARGUMENT);
    MemState *mem;
    if (const SceInt32 result = get_caller_memory(emuenv, thread_id, mem); result < 0)
        return RET_ERROR(result);
    std::string category_copy;
    if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, category, category_copy); result < 0)
        return RET_ERROR(result);
    std::vector<Keys> key_values;
    if (const SceInt32 result = copy_keys_from_guest(*mem, keys, static_cast<SceUInt32>(elementsNumber), key_values); result < 0)
        return RET_ERROR(result);
    for (const auto &key : key_values) {
        if (key.size != sizeof(Keys))
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_KEYS_INFO);
        if (!key.name)
            return RET_ERROR(SCE_REGMGR_ERROR_INVALID_ARGUMENT);
        if (!key.valueSize)
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);

        std::string name;
        if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, key.name, name); result < 0)
            return RET_ERROR(result);

        switch (key.type) {
        case REG_TYPE_INT: {
            if (key.valueSize != sizeof(key.value))
                return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
            const uint32_t type_error = get_registry_key_type_error(emuenv, category_copy, name, REG_TYPE_INT);
            if (type_error)
                return RET_ERROR(type_error);
            const auto value_size = regmgr::get_value_size(emuenv.regmgr, category_copy, name, REG_TYPE_INT);
            if (!value_size || key.valueSize > *value_size)
                return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
            const auto result = regmgr::set_int_value_result(emuenv.regmgr, category_copy, name, key.value);
            if (result != regmgr::RegMgrSetResult::success)
                return RET_ERROR(regmgr::set_result_to_error(result));
            break;
        }
        case REG_TYPE_STR: {
            const uint32_t type_error = get_registry_key_type_error(emuenv, category_copy, name, REG_TYPE_STR);
            if (type_error)
                return RET_ERROR(type_error);
            const auto value_size = regmgr::get_value_size(emuenv.regmgr, category_copy, name, REG_TYPE_STR);
            if (!value_size || key.valueSize > *value_size)
                return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
            std::string value;
            if (const SceInt32 result = sce_regmgr::copy_guest_string(*mem, Ptr<const char>(key.data), value, key.valueSize); result < 0) {
                if (result == SCE_REGMGR_ERROR_PATH_TOO_LONG)
                    return RET_ERROR(SCE_REGMGR_ERROR_STR_TOO_LONG);
                return RET_ERROR(result);
            }
            value.push_back('\0');
            const auto result = regmgr::set_str_value_result(emuenv.regmgr, category_copy, name, value.data(), static_cast<uint32_t>(value.size()));
            if (result != regmgr::RegMgrSetResult::success)
                return RET_ERROR(regmgr::set_result_to_error(result));
            break;
        }
        case REG_TYPE_BIN: {
            const uint32_t type_error = get_registry_key_type_error(emuenv, category_copy, name, REG_TYPE_BIN);
            if (type_error)
                return RET_ERROR(type_error);
            const auto value_size = regmgr::get_value_size(emuenv.regmgr, category_copy, name, REG_TYPE_BIN);
            if (!value_size || key.valueSize > *value_size)
                return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
            std::vector<uint8_t> value(key.valueSize);
            if (const SceInt32 result = sce_regmgr::copy_from_guest(*mem, key.data, value.data(), key.valueSize); result < 0)
                return RET_ERROR(result);
            const auto result = regmgr::set_bin_value_result(emuenv.regmgr, category_copy, name, value.data(), key.valueSize);
            if (result != regmgr::RegMgrSetResult::success)
                return RET_ERROR(regmgr::set_result_to_error(result));
            break;
        }
        default:
            return RET_ERROR(SCE_REGMGR_ERROR_BAD_TYPE);
        }
        sce_regmgr::notify_callbacks(emuenv, category_copy, name);
    }

    return 0;
}

EXPORT(int, sceRegMgrStartCallback, SceUID callback_id) {
    TRACY_FUNC(sceRegMgrStartCallback, callback_id);
    SceUID owner_thread_id;
    const int owner_result = get_callback_owner_thread(emuenv, thread_id, callback_id, owner_thread_id);
    if (owner_result < 0)
        return RET_ERROR(owner_result);
    const auto result = regmgr::start_callback(emuenv.regmgr, callback_id, owner_thread_id);
    if (result >= 0x80000000u)
        return RET_ERROR(result);
    return result;
}

EXPORT(int, sceRegMgrStopCallback, SceUID callback_id) {
    TRACY_FUNC(sceRegMgrStopCallback, callback_id);
    SceUID owner_thread_id;
    const int owner_result = get_callback_owner_thread(emuenv, thread_id, callback_id, owner_thread_id);
    if (owner_result < 0)
        return RET_ERROR(owner_result);
    const auto result = regmgr::stop_callback(emuenv.regmgr, callback_id, owner_thread_id);
    if (result >= 0x80000000u)
        return RET_ERROR(result);
    return result;
}

EXPORT(int, sceRegMgrUnregisterCallback, SceUID callback_id) {
    TRACY_FUNC(sceRegMgrUnregisterCallback, callback_id);
    SceUID owner_thread_id;
    const int owner_result = get_callback_owner_thread(emuenv, thread_id, callback_id, owner_thread_id);
    if (owner_result < 0)
        return RET_ERROR(owner_result);
    const auto result = regmgr::unregister_callback(emuenv.regmgr, callback_id, owner_thread_id);
    if (result >= 0x80000000u)
        return RET_ERROR(result);
    return result;
}

EXPORT(int, sceRegMgrUnregisterDrvErrCallback, SceUID callback_id) {
    TRACY_FUNC(sceRegMgrUnregisterDrvErrCallback, callback_id);
    return UNIMPLEMENTED();
}
