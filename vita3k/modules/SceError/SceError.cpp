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

#include "SceError.h"

#include "../SceKernelThreadMgr/lightweight.h"

#include <io/functions.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <modules/system_version.h>
#include <rtc/rtc.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

TRACY_MODULE_NAME(SceError);

namespace {

struct ErrorGroup {
    uint16_t start;
    uint16_t count;
    char name[4];
};

struct ErrorTable {
    char prefix[4]{};
    uint16_t max_index = 0;
    uint16_t base = 0;
    std::vector<ErrorGroup> groups;
    std::vector<uint32_t> codes;
};

struct ErrorTableHeader {
    char prefix[4];
    uint16_t group_count;
    uint16_t max_index;
};

static_assert(sizeof(ErrorGroup) == 8);
static_assert(sizeof(ErrorTableHeader) == 8);

std::once_flag error_table_once;
ErrorTable error_table;
std::mutex history_mutex;
std::array<SceErrorHistoryInfo, 10> history_entries{};
uint8_t history_head = 0;
uint8_t history_sequence = 0;

bool load_error_table(EmuEnvState &emuenv, ErrorTable &table) {
    std::vector<uint8_t> data;
    if (!read_file_to_buffer(emuenv.io, "os0:kd/error_table.bin", emuenv.vita_fs_path, data, "_sceErrorGetExternalString", 80000))
        return false;

    size_t offset = 0;
    const auto read = [&data, &offset](void *destination, size_t size) {
        if (offset > data.size() || size > data.size() - offset)
            return false;
        std::memcpy(destination, data.data() + offset, size);
        offset += size;
        return true;
    };

    ErrorTableHeader header{};
    if (!read(&header, sizeof(header)) || header.group_count > 32)
        return false;

    std::vector<ErrorGroup> groups(header.group_count);
    if (!groups.empty() && !read(groups.data(), groups.size() * sizeof(ErrorGroup)))
        return false;

    uint16_t code_count = 0;
    if (!read(&table.base, sizeof(table.base)) || !read(&code_count, sizeof(code_count)) || code_count > 20000)
        return false;

    std::vector<uint32_t> codes(code_count);
    if (!codes.empty() && !read(codes.data(), codes.size() * sizeof(uint32_t)))
        return false;

    std::memcpy(table.prefix, header.prefix, sizeof(table.prefix));
    table.max_index = header.max_index;
    table.groups = std::move(groups);
    table.codes = std::move(codes);
    return true;
}

const ErrorTable &get_error_table(EmuEnvState &emuenv) {
    std::call_once(error_table_once, [&emuenv] {
        if (!load_error_table(emuenv, error_table))
            LOG_WARN_ONCE("Failed to load os0:kd/error_table.bin; SceError will use fallback error strings");
    });
    return error_table;
}

size_t fixed_string_length(const char *text, size_t max_length) {
    return static_cast<size_t>(std::find(text, text + max_length, '\0') - text);
}

void format_external_error_code(const ErrorTable &table, uint32_t code, char (&output)[16]) {
    std::memset(output, 0, sizeof(output));
    const auto found = std::find(table.codes.begin(), table.codes.end(), code);
    if (table.codes.empty() || found == table.codes.end()) {
        std::snprintf(output, sizeof(output), "E-%08x", code);
        return;
    }

    const size_t index = static_cast<size_t>(found - table.codes.begin());
    if (index > table.max_index) {
        std::snprintf(output, sizeof(output), "*-%08x", code);
        return;
    }

    const uint32_t facility = (code >> 16) & 0xFFF;
    std::string prefix(table.prefix, fixed_string_length(table.prefix, sizeof(table.prefix)));
    if (facility - 0x200 <= 0xFF) {
        prefix = "NS";
    } else {
        for (const auto &group : table.groups) {
            if (facility >= group.start && facility < uint32_t(group.start) + group.count) {
                prefix.assign(group.name, fixed_string_length(group.name, sizeof(group.name)));
                break;
            }
        }
    }

    const uint32_t number = uint32_t(table.base) + static_cast<uint32_t>(index);
    uint32_t digit_sum = 0;
    for (uint32_t digits = number; digits != 0; digits /= 10)
        digit_sum += digits % 10;
    std::snprintf(output, sizeof(output), "%s-%u-%u", prefix.c_str(), number, digit_sum % 10);
}

bool address_range_valid(Address address, size_t size) {
    return size != 0 && uint64_t(address) + size - 1 <= std::numeric_limits<Address>::max();
}

} // namespace

EXPORT(SceInt32, _sceErrorGetExternalString, Ptr<char> result, uint32_t err) {
    TRACY_FUNC(_sceErrorGetExternalString, result.address(), err);
    if (!result)
        return SCE_KERNEL_OK;

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    char output[16]{};
    format_external_error_code(get_error_table(emuenv), err, output);
    LOG_INFO("_sceErrorGetExternalString: thread {} PC {} LR {}, error {} formats as {}",
        thread_id, log_hex(caller->cpu->get_pc()), log_hex(caller->cpu->get_lr()), log_hex(err), output);
    auto &mem = caller->cpu->mem;
    if (!address_range_valid(result.address(), sizeof(output)) || !threadmgr::copy_sync_guest_bytes(mem, result.address(), output, sizeof(output), true))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    return SCE_KERNEL_OK;
}

EXPORT(int, _sceErrorHistoryClearError) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceErrorHistoryGetError) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceErrorHistoryPostError, Ptr<const SceErrorHistoryPostInfo> info) {
    TRACY_FUNC(_sceErrorHistoryPostError, info.address());
    if (!info)
        return RET_ERROR(SCE_ERROR_ERRNO_EINVAL);

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    auto &mem = caller->cpu->mem;
    SceErrorHistoryPostInfo record{};
    if (!address_range_valid(info.address(), sizeof(record)) || !threadmgr::copy_sync_guest_bytes(mem, info.address(), &record, sizeof(record), false))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);

    std::memset(record.reserved2, 0, sizeof(record.reserved2));
    record.version = EMULATED_SYSTEM_VERSION;
    record.system_software_version_unk24 = 0;

    SceErrorHistoryInfo entry{};
    entry.post = record;
    entry.time = static_cast<int64_t>(rtc_get_ticks(emuenv.kernel.base_tick.tick));
    format_external_error_code(get_error_table(emuenv), static_cast<uint32_t>(record.error_code_hex), entry.error_code.s);

    std::lock_guard lock(history_mutex);
    for (auto &previous : history_entries) {
        if (previous.post.error_code_hex == record.error_code_hex && previous.post.application_code == record.application_code && std::memcmp(previous.post.titleid, record.titleid, 10) == 0)
            previous = {};
    }
    if (history_sequence == 0)
        history_sequence = 1;
    entry.sequence_id = history_sequence++;
    history_entries[history_head] = entry;
    history_head = static_cast<uint8_t>((history_head + 1) % history_entries.size());
    return SCE_KERNEL_OK;
}

EXPORT(int, _sceErrorHistorySetDefaultFormat) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceErrorHistoryUpdateSequenceInfo) {
    return UNIMPLEMENTED();
}
