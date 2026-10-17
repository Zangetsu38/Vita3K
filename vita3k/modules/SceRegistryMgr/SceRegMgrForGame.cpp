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

#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <kernel/types.h>
#include <regmgr/functions.h>

#include <array>
#include <vector>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceRegMgrForGame);

EXPORT(int, sceRegMgrSystemIsBlueScreen, SceInt32 error) {
    TRACY_FUNC(sceRegMgrSystemIsBlueScreen, error);
    return regmgr::is_blue_screen_error(error);
}

EXPORT(int, sceRegMgrSystemParamGetBin, const int id, Ptr<void> buf, SceSize bufSize) {
    TRACY_FUNC(sceRegMgrSystemParamGetBin, id, buf.address(), bufSize);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const auto [category, name] = regmgr::get_category_and_name_by_id(id, export_name);
    if (const SceInt32 result = regmgr::get_key_type_error(emuenv.regmgr, category, name, REG_TYPE_BIN); result < 0)
        return RET_ERROR(result);
    const auto info = regmgr::get_value_info(emuenv.regmgr, category, name);
    if (!info)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_NAME);
    if (bufSize < info->size)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
    std::vector<uint8_t> value(info->size);
    if (!regmgr::get_bin_value(emuenv.regmgr, category, name, value.data(), info->size))
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_VALUE);
    if (const SceInt32 result = sce_regmgr::copy_to_guest(caller->cpu->mem, buf.address(), value.data(), info->size); result < 0)
        return RET_ERROR(result);

    return 0;
}

EXPORT(int, sceRegMgrSystemParamGetInt, const int id, Ptr<SceInt32> buf) {
    TRACY_FUNC(sceRegMgrSystemParamGetInt, id, buf.address());
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const auto [category, name] = regmgr::get_category_and_name_by_id(id, export_name);
    if (const SceInt32 result = regmgr::get_key_type_error(emuenv.regmgr, category, name, REG_TYPE_INT); result < 0)
        return RET_ERROR(result);
    const auto info = regmgr::get_value_info(emuenv.regmgr, category, name);
    if (!info || info->size < sizeof(SceInt32))
        return RET_ERROR(info ? SCE_REGMGR_ERROR_BAD_SIZE : SCE_REGMGR_ERROR_BAD_NAME);
    std::array<uint8_t, sizeof(SceInt32)> value_bytes;
    if (!regmgr::get_bin_value(emuenv.regmgr, category, name, value_bytes.data(), value_bytes.size()))
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_VALUE);
    const SceInt32 value = regmgr::get_int_value(emuenv.regmgr, category, name);
    if (const SceInt32 result = sce_regmgr::copy_to_guest(caller->cpu->mem, buf.address(), &value, sizeof(value)); result < 0)
        return RET_ERROR(result);

    return 0;
}

EXPORT(int, sceRegMgrSystemParamGetStr, const int id, Ptr<char> buf, const SceSize bufSize) {
    TRACY_FUNC(sceRegMgrSystemParamGetStr, id, buf.address(), bufSize);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const auto [category, name] = regmgr::get_category_and_name_by_id(id, export_name);
    if (const SceInt32 result = regmgr::get_key_type_error(emuenv.regmgr, category, name, REG_TYPE_STR); result < 0)
        return RET_ERROR(result);
    const auto info = regmgr::get_value_info(emuenv.regmgr, category, name);
    if (!info)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_NAME);
    if (bufSize < info->size)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
    std::vector<char> value_bytes(info->size);
    if (!regmgr::get_bin_value(emuenv.regmgr, category, name, value_bytes.data(), info->size))
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_VALUE);
    const std::string value(value_bytes.begin(), value_bytes.end());
    if (const SceInt32 result = sce_regmgr::copy_guest_c_string(caller->cpu->mem, buf.address(), value, bufSize); result < 0)
        return RET_ERROR(result);

    return 0;
}

EXPORT(int, sceRegMgrSystemParamSetBin, const int id, Ptr<const void> buf, const SceSize bufSize) {
    TRACY_FUNC(sceRegMgrSystemParamSetBin, id, buf.address(), bufSize);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const auto [category, name] = regmgr::get_category_and_name_by_id(id, export_name);
    if (const SceInt32 result = regmgr::get_key_type_error(emuenv.regmgr, category, name, REG_TYPE_BIN); result < 0)
        return RET_ERROR(result);
    const auto value_size = regmgr::get_value_size(emuenv.regmgr, category, name, REG_TYPE_BIN);
    if (!value_size || !bufSize || bufSize > *value_size)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
    std::vector<uint8_t> value(bufSize);
    if (const SceInt32 result = sce_regmgr::copy_from_guest(caller->cpu->mem, buf.address(), value.data(), bufSize); result < 0)
        return RET_ERROR(result);
    const auto set_result = regmgr::set_bin_value_result(emuenv.regmgr, category, name, value.data(), bufSize);
    if (set_result != regmgr::RegMgrSetResult::success)
        return RET_ERROR(regmgr::set_result_to_error(set_result));
    sce_regmgr::notify_callbacks(emuenv, category, name);
    return 0;
}

EXPORT(int, sceRegMgrSystemParamSetInt, const int id, const SceInt32 buf) {
    TRACY_FUNC(sceRegMgrSystemParamSetInt, id, buf);
    const auto [category, name] = regmgr::get_category_and_name_by_id(id, export_name);
    if (const SceInt32 result = regmgr::get_key_type_error(emuenv.regmgr, category, name, REG_TYPE_INT); result < 0)
        return RET_ERROR(result);
    const auto set_result = regmgr::set_int_value_result(emuenv.regmgr, category, name, buf);
    if (set_result != regmgr::RegMgrSetResult::success)
        return RET_ERROR(regmgr::set_result_to_error(set_result));
    sce_regmgr::notify_callbacks(emuenv, category, name);
    return 0;
}

EXPORT(int, sceRegMgrSystemParamSetStr, const int id, Ptr<const char> buf, const SceSize bufSize) {
    TRACY_FUNC(sceRegMgrSystemParamSetStr, id, buf.address(), bufSize);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const auto [category, name] = regmgr::get_category_and_name_by_id(id, export_name);
    if (const SceInt32 result = regmgr::get_key_type_error(emuenv.regmgr, category, name, REG_TYPE_STR); result < 0)
        return RET_ERROR(result);
    const auto value_size = regmgr::get_value_size(emuenv.regmgr, category, name, REG_TYPE_STR);
    if (!value_size || !bufSize || bufSize > *value_size)
        return RET_ERROR(SCE_REGMGR_ERROR_BAD_SIZE);
    std::vector<char> value(bufSize);
    if (const SceInt32 result = sce_regmgr::copy_from_guest(caller->cpu->mem, buf.address(), value.data(), bufSize); result < 0)
        return RET_ERROR(result);
    const auto set_result = regmgr::set_str_value_result(emuenv.regmgr, category, name, value.data(), bufSize);
    if (set_result != regmgr::RegMgrSetResult::success)
        return RET_ERROR(regmgr::set_result_to_error(set_result));
    sce_regmgr::notify_callbacks(emuenv, category, name);
    return 0;
}
