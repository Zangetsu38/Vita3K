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

#include <mem/allocator.h>
#include <mem/functions.h>
#include <mem/util.h>

#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

struct AllocMemPage {
    uint32_t allocated : 4;
    uint32_t size : 28;
};

static_assert(sizeof(AllocMemPage) == 4);

using PagePtr = uint8_t *;
using Memory = std::unique_ptr<uint8_t[], std::function<void(uint8_t *)>>;
using AllocPageTable = std::unique_ptr<AllocMemPage[]>;
using PageTable = std::unique_ptr<PagePtr[]>;
using PageNameMap = std::map<int, std::string>;

struct ProtectBlockInfo {
    uint32_t size = 0;
    ProtectCallback callback;
    bool cache_invalidation = false;
};

struct ProtectSegmentInfo {
    std::multimap<Address, ProtectBlockInfo> blocks;
    uint32_t size = 0;
    MemPerm perm = MemPerm::None;

    explicit ProtectSegmentInfo() = default;
    explicit ProtectSegmentInfo(uint32_t size, MemPerm perm)
        : size(size)
        , perm(perm) {
    }
};

using ProtectSegmentTrees = std::map<Address, ProtectSegmentInfo, std::greater<>>;

struct MemExternalMapping {
    Address address;
    uint32_t size;
    std::vector<uint8_t *> previous_page_table_entries;
};

struct GuestMspace {
    Address base;
    uint32_t size;
    Address parent = 0;
    Address parent_allocation = 0;
    struct Allocation {
        Address backing;
        uint32_t size;
    };
    std::unordered_map<Address, Allocation> allocations;
};

struct ClibMspace {
    uint32_t capacity;
};

struct MemState {
    std::mutex secondary_import_warning_mutex;
    std::unordered_map<uint32_t, uint64_t> secondary_import_warning_counts;

    uint64_t record_secondary_import_warning(uint32_t nid) {
        const std::lock_guard lock(secondary_import_warning_mutex);
        auto &count = secondary_import_warning_counts[nid];
        if (count != std::numeric_limits<uint64_t>::max())
            ++count;
        return count;
    }

    std::mutex generation_mutex;
    std::unordered_map<Address, GuestMspace> guest_mspaces;
    std::unordered_map<Address, ClibMspace> clib_mspaces;
    std::condition_variable pins_changed;
    uint64_t next_pin = 1;
    std::map<uint64_t, std::vector<GuestMemoryRange>> pinned_ranges;
    std::mutex protect_mutex;

    uint32_t host_page_size = 0;
    Memory memory;
    AllocPageTable alloc_table;
    BitmapAllocator allocator;
    ProtectSegmentTrees protect_tree;

    PageNameMap page_name_map;

    bool use_page_table = false;
    PageTable page_table;
    std::map<uint64_t, MemExternalMapping, std::greater<>> external_mapping;

    bool trace_silent_hill = false;
    bool silent_hill_scheduler_experiment = false;
    bool silent_hill_late_wait_experiment = false;
    std::atomic_flag invalid_access_logged;
};
