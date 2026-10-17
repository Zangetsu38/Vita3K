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
#include <util/types.h>

#include <cstdint>

enum SceRegMgrErrorCode : uint32_t {
    SCE_REGMGR_ERROR_INVALID_ARGUMENT = 0x800D0004u,
    SCE_REGMGR_ERROR_LINE_TOO_LONG = 0x800D0006u,
    SCE_REGMGR_ERROR_TOO_MANY_KEYS = 0x800D0007u,
    SCE_REGMGR_ERROR_PATH_TOO_LONG = 0x800D0008u,
    SCE_REGMGR_ERROR_BAD_PATH = 0x800D0009u,
    SCE_REGMGR_ERROR_BAD_CATEGORY = 0x800D000Au,
    SCE_REGMGR_ERROR_BAD_TYPE = 0x800D000Bu,
    SCE_REGMGR_ERROR_BAD_SIZE = 0x800D000Cu,
    SCE_REGMGR_ERROR_BAD_ATTRIBUTE = 0x800D000Du,
    SCE_REGMGR_ERROR_BAD_PERMISSION = 0x800D000Eu,
    SCE_REGMGR_ERROR_BAD_FLAG = 0x800D000Fu,
    SCE_REGMGR_ERROR_BAD_OPTION = 0x800D0010u,
    SCE_REGMGR_ERROR_BAD_INIT = 0x800D0012u,
    SCE_REGMGR_ERROR_BAD_VALUE = 0x800D0013u,
    SCE_REGMGR_ERROR_BAD_NAME = 0x800D0014u,
    SCE_REGMGR_ERROR_NO_ACCESS = 0x800D0015u,
    SCE_REGMGR_ERROR_OUT_OF_RANGE = 0x800D0017u,
    SCE_REGMGR_ERROR_BAD_PRODUCT = 0x800D0018u,
    SCE_REGMGR_ERROR_NVS_SIZE = 0x800D0019u,
    SCE_REGMGR_ERROR_BAD_FLAG2 = 0x800D001Au,
    SCE_REGMGR_ERROR_BAD_FLAG3 = 0x800D001Bu,
    SCE_REGMGR_ERROR_BAD_KEYS_INFO = 0x800D001Cu,
    SCE_REGMGR_ERROR_STR_TOO_LONG = 0x800D001Du,
    SCE_REGMGR_ERROR_BAD_LEVEL = 0x800D0020u,
    SCE_REGMGR_ERROR_BAD_CACHE = 0x800D0021u,
    SCE_REGMGR_ERROR_NO_FILE = 0x800D0022u,
    SCE_REGMGR_ERROR_CALLBACK_ALREADY_REGISTERED = 0x800D0025u,
    SCE_REGMGR_ERROR_CALLBACK_LIMIT_REACHED = 0x800D0026u,
    SCE_REGMGR_ERROR_CALLBACK_NOT_REGISTERED = 0x800D0027u,
    SCE_REGMGR_ERROR_ALREADY_OPENED = 0x800D0030u,
};

struct Keys {
    SceUInt32 size; // sizeof(this)
    Ptr<const char> name;
    SceUInt16 type;
    SceUInt16 valueSize;
    union {
        SceInt32 value;
        Address data;
    };
};

static_assert(sizeof(Keys) == 16);
