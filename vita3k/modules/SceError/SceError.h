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

#include <module/module.h>
#include <util/tracy.h>

#include <cstdint>

struct SceErrorString {
    char s[16];
};

struct SceErrorHistoryPostInfo {
    char error_message[0x100];
    uint32_t suggested_actions[15];
    uint8_t reserved0[2];
    uint8_t suggested_action_count;
    uint8_t reserved1;
    int32_t error_code_hex;
    uint32_t application_code;
    uint32_t version;
    int32_t network_status;
    char titleid[0xC];
    uint32_t system_software_version_unk24;
    uint8_t reserved2[0x20];
};

struct SceErrorHistoryInfo {
    uint8_t reserved0;
    uint8_t flags;
    uint8_t sequence_id;
    uint8_t reserved1;
    uint32_t reserved2;
    int64_t time;
    SceErrorString error_code;
    uint8_t reserved3[0x10];
    SceErrorHistoryPostInfo post;
};

static_assert(sizeof(SceErrorHistoryPostInfo) == 0x180);
static_assert(sizeof(SceErrorHistoryInfo) == 0x1B0);

DECL_EXPORT(SceInt32, _sceErrorGetExternalString, Ptr<char> result, uint32_t err);
DECL_EXPORT(SceInt32, _sceErrorHistoryPostError, Ptr<const SceErrorHistoryPostInfo> info);
