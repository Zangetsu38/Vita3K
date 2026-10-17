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

#include <mem/block.h>
#include <mem/util.h>

#include <span>

struct MemState;

struct GuestMemoryRange {
    Address address;
    uint32_t size;
};

constexpr Address user_main_memory_start = 0x80000000U;
constexpr uint32_t STANDARD_PAGE_SIZE = KiB(4);

// Permission when protecting a memory range
// Note: WriteOnly is actually not supported (ReadWrite used instead)
enum struct MemPerm {
    None = 0,
    ReadOnly = 1 << 0,
    WriteOnly = 1 << 1,
    ReadWrite = ReadOnly | WriteOnly
};

constexpr MemPerm most_restrictive_perm(MemPerm a, MemPerm b) {
    if (a == MemPerm::None || b == MemPerm::None)
        return MemPerm::None;
    if (a == MemPerm::ReadOnly || b == MemPerm::ReadOnly)
        return MemPerm::ReadOnly;
    if (a == MemPerm::WriteOnly || b == MemPerm::WriteOnly)
        return MemPerm::WriteOnly;
    return MemPerm::ReadWrite;
}

bool init(MemState &state, const bool use_page_table);
void deinit_mem(MemState &state);
Address alloc(MemState &state, uint32_t size, const char *name, Address start_addr = user_main_memory_start);
Address alloc_aligned(MemState &state, uint32_t size, const char *name, unsigned int alignment, Address start_addr = user_main_memory_start);
void protect_inner(MemState &state, Address addr, uint32_t size, const MemPerm perm);
void unprotect_inner(MemState &state, Address addr, uint32_t size);
bool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback, bool cache_invalidation = false);
void open_access_parent_protect_segment(MemState &state, Address addr);
void close_access_parent_protect_segment(MemState &state, Address addr);
void add_external_mapping(MemState &mem, Address addr, uint32_t size, uint8_t *addr_ptr);
bool map_external_memory(MemState &mem, Address addr, uint32_t size, uint8_t *addr_ptr);
void remove_external_mapping(MemState &mem, uint8_t *addr_ptr, uint32_t size);
bool is_protecting(MemState &state, Address addr, MemPerm *perm = nullptr);
bool is_valid_addr(const MemState &state, Address addr);
bool is_valid_addr_range(const MemState &state, Address start, Address end);
bool is_valid_addr_len(const MemState &state, Address addr, uint32_t len);
uint64_t pin_guest_memory(MemState &state, std::span<const GuestMemoryRange> ranges);
void unpin_guest_memory(MemState &state, uint64_t pin);
bool read_guest_memory(MemState &state, Address source, void *destination, uint32_t size);
bool compare_guest_memory(MemState &state, Address left, Address right, uint32_t size, bool stop_at_null, int &result);
bool write_guest_memory(MemState &state, Address destination, const void *source, uint32_t size);
bool write_guest_memory_with_cache_invalidation(MemState &state, Address destination, const void *source, uint32_t size);
bool write_guest_memory_padded(MemState &state, Address destination, const void *source, uint32_t source_size, uint32_t total_size);
bool copy_guest_memory(MemState &state, Address destination, Address source, uint32_t size);
bool move_guest_memory(MemState &state, Address destination, Address source, uint32_t size);
bool fill_guest_memory(MemState &state, Address destination, uint8_t value, uint32_t size);
enum class GuestMspaceOp { create,
    destroy,
    malloc,
    calloc,
    memalign,
    realloc,
    free };
enum class GuestMspaceStatus { success,
    invalid,
    exhausted };
struct GuestMspaceStats {
    uint32_t capacity;
    uint32_t footprint;
    uint32_t peak_in_use;
    uint32_t current_in_use;
};
GuestMspaceStatus guest_mspace_op(MemState &state, GuestMspaceOp op, Address space, Address ptr,
    uint32_t size, uint32_t extra, Address &result);
bool guest_mspace_stats(MemState &state, Address space, GuestMspaceStats &stats);
bool handle_access_violation(MemState &state, uint8_t *addr, bool write) noexcept;
Block alloc_block(MemState &mem, uint32_t size, const char *name, Address start_addr = user_main_memory_start);
Address alloc_at(MemState &state, Address address, uint32_t size, const char *name);
Address try_alloc_at(MemState &state, Address address, uint32_t size, const char *name);
void free(MemState &state, Address address);
uint32_t mem_available(MemState &state);
const char *mem_name(Address address, MemState &state);
