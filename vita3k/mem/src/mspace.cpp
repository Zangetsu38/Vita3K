// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include <mem/functions.h>
#include <mem/state.h>

#include <dlmalloc.h>
#include <util/log.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

size_t mspace_set_footprint_limit(mspace msp, size_t bytes);

namespace {
bool accessible_heap(MemState &mem, Address base, uint32_t size) {
    const uint64_t end = uint64_t(base) + size;
    if (!mem.memory || !mem.alloc_table || !size || base < STANDARD_PAGE_SIZE || end > (uint64_t{ 1 } << 32)
        || !is_valid_addr_len(mem, base, size)) {
        LOG_ERROR("Guest mspace range {:#x} size {:#x} is not fully allocated", base, size);
        return false;
    }
    for (const auto &[start, segment] : mem.protect_tree) {
        if (segment.perm != MemPerm::ReadWrite && uint64_t(start) < end && uint64_t(start) + segment.size > base) {
            LOG_ERROR("Guest mspace range {:#x} size {:#x} overlaps protected range {:#x} size {:#x}", base, size, start, segment.size);
            return false;
        }
    }
    if (mem.use_page_table) {
        const uint64_t first = base / STANDARD_PAGE_SIZE;
        const uint64_t last = (end - 1) / STANDARD_PAGE_SIZE;
        for (uint64_t page = first; page <= last; ++page) {
            if (mem.page_table[page] != mem.memory.get()) {
                const Address page_address = static_cast<Address>(page * STANDARD_PAGE_SIZE);
                LOG_ERROR("Guest mspace range {:#x} size {:#x} contains remapped page {:#x}", base, size, page_address);
                for (const auto &[_, mapping] : mem.external_mapping) {
                    if (mapping.address <= page_address && uint64_t(page_address) < uint64_t(mapping.address) + mapping.size) {
                        LOG_ERROR("Remapped heap page belongs to external mapping {:#x} size {:#x}", mapping.address, mapping.size);
                        break;
                    }
                }
                return false;
            }
        }
    }
    return true;
}

bool is_ancestor(const MemState &mem, Address ancestor, Address heap) {
    for (size_t depth = 0; heap && depth < mem.guest_mspaces.size(); ++depth) {
        if (heap == ancestor)
            return true;
        const auto it = mem.guest_mspaces.find(heap);
        if (it == mem.guest_mspaces.end())
            break;
        heap = it->second.parent;
    }
    return false;
}
} // namespace

GuestMspaceStatus guest_mspace_op(MemState &mem, GuestMspaceOp op, Address space, Address ptr,
    uint32_t size, uint32_t extra, Address &result) {
    result = 0;
    const std::scoped_lock lock(mem.generation_mutex, mem.protect_mutex);
    while (std::erase_if(mem.guest_mspaces, [&](const auto &entry) {
        return entry.second.parent && !mem.guest_mspaces.contains(entry.second.parent);
    })) {
    }

    if (op == GuestMspaceOp::create) {
        const uint64_t end = uint64_t(ptr) + size;
        if (!accessible_heap(mem, ptr, size))
            return GuestMspaceStatus::invalid;
        Address parent = 0;
        Address parent_allocation = 0;
        uint64_t parent_size = uint64_t{ 1 } << 32;
        for (const auto &[handle, heap] : mem.guest_mspaces) {
            for (const auto &[address, allocation] : heap.allocations) {
                if (address <= ptr && end <= uint64_t(address) + allocation.size && heap.size < parent_size) {
                    parent = handle;
                    parent_allocation = address;
                    parent_size = heap.size;
                }
            }
        }
        for (const auto &[handle, heap] : mem.guest_mspaces) {
            if (uint64_t(ptr) < uint64_t(heap.base) + heap.size && uint64_t(heap.base) < end) {
                if (!parent || !is_ancestor(mem, handle, parent)) {
                    LOG_ERROR("Guest mspace range {:#x} size {:#x} overlaps heap {:#x} size {:#x} without a valid parent allocation or overlaps a sibling", ptr, size, heap.base, heap.size);
                    for (const auto &[address, allocation] : heap.allocations) {
                        if (uint64_t(address) < end && uint64_t(address) + allocation.size > ptr)
                            LOG_ERROR("Overlapping tracked allocation {:#x} size {:#x} in heap {:#x}", address, allocation.size, heap.base);
                    }
                    return GuestMspaceStatus::invalid;
                }
            }
        }
        mspace handle = create_mspace_with_base(mem.memory.get() + ptr, size, 0);
        if (!handle)
            return GuestMspaceStatus::exhausted;
        mspace_track_large_chunks(handle, 1);
        mspace_set_footprint_limit(handle, 1);
        result = static_cast<Address>(static_cast<uint8_t *>(handle) - mem.memory.get());
        mem.guest_mspaces.emplace(result, GuestMspace{ ptr, size, parent, parent_allocation, {} });
        return GuestMspaceStatus::success;
    }

    auto it = mem.guest_mspaces.find(space);
    if (it == mem.guest_mspaces.end() || !accessible_heap(mem, it->second.base, it->second.size))
        return GuestMspaceStatus::invalid;
    GuestMspace &heap = it->second;
    mspace handle = mem.memory.get() + space;
    if (op == GuestMspaceOp::destroy) {
        for (const auto &[_, child] : mem.guest_mspaces) {
            if (child.parent == space)
                return GuestMspaceStatus::invalid;
        }
        result = static_cast<Address>(destroy_mspace(handle));
        mem.guest_mspaces.erase(it);
        return GuestMspaceStatus::success;
    }
    if ((op == GuestMspaceOp::free || op == GuestMspaceOp::realloc) && ptr && !heap.allocations.contains(ptr))
        return GuestMspaceStatus::invalid;
    if ((op == GuestMspaceOp::free || op == GuestMspaceOp::realloc) && ptr) {
        for (const auto &[_, child] : mem.guest_mspaces) {
            if (child.parent == space && child.parent_allocation == ptr)
                return GuestMspaceStatus::invalid;
        }
    }
    const auto allocation = heap.allocations.find(ptr);
    const Address backing = allocation == heap.allocations.end() ? 0 : allocation->second.backing;
    void *host_ptr = backing ? mem.memory.get() + backing : nullptr;
    if (op == GuestMspaceOp::free) {
        mspace_free(handle, host_ptr);
        heap.allocations.erase(ptr);
        return GuestMspaceStatus::success;
    }
    void *allocated = nullptr;
    uint32_t allocation_size = size;
    uint64_t guest_alignment = 0;
    switch (op) {
    case GuestMspaceOp::malloc:
        allocated = mspace_malloc(handle, size);
        break;
    case GuestMspaceOp::calloc:
        if (uint64_t(size) * extra > std::numeric_limits<uint32_t>::max())
            return GuestMspaceStatus::exhausted;
        allocated = mspace_calloc(handle, size, extra);
        allocation_size = size * extra;
        break;
    case GuestMspaceOp::memalign:
        guest_alignment = std::bit_ceil(std::max<uint64_t>(extra, 2 * sizeof(void *)));
        if (uint64_t(size) + guest_alignment - 1 > heap.size)
            return GuestMspaceStatus::exhausted;
        allocated = mspace_malloc(handle, size + guest_alignment - 1);
        break;
    case GuestMspaceOp::realloc:
        if (ptr && ptr != backing) {
            allocated = mspace_malloc(handle, size);
            if (allocated) {
                std::memcpy(allocated, mem.memory.get() + ptr, std::min(size, allocation->second.size));
                mspace_free(handle, host_ptr);
            }
        } else {
            allocated = mspace_realloc(handle, host_ptr, size);
        }
        if (allocated)
            heap.allocations.erase(ptr);
        break;
    default:
        return GuestMspaceStatus::invalid;
    }
    if (!allocated)
        return GuestMspaceStatus::exhausted;
    const auto host = reinterpret_cast<uintptr_t>(allocated);
    const auto base = reinterpret_cast<uintptr_t>(mem.memory.get());
    if (host < base + heap.base || host >= base + uint64_t(heap.base) + heap.size)
        return GuestMspaceStatus::invalid;
    const Address allocation_base = static_cast<Address>(host - base);
    const uint64_t aligned = guest_alignment ? (uint64_t(allocation_base) + guest_alignment - 1) & ~(guest_alignment - 1) : allocation_base;
    if (aligned >= uint64_t(heap.base) + heap.size || aligned + allocation_size > uint64_t(heap.base) + heap.size) {
        mspace_free(handle, allocated);
        return GuestMspaceStatus::invalid;
    }
    result = static_cast<Address>(aligned);
    heap.allocations.emplace(result, GuestMspace::Allocation{ allocation_base, allocation_size });
    return GuestMspaceStatus::success;
}

bool guest_mspace_stats(MemState &mem, Address space, GuestMspaceStats &stats) {
    stats = {};
    const std::scoped_lock lock(mem.generation_mutex, mem.protect_mutex);
    while (std::erase_if(mem.guest_mspaces, [&](const auto &entry) {
        return entry.second.parent && !mem.guest_mspaces.contains(entry.second.parent);
    })) {
    }

    const auto it = mem.guest_mspaces.find(space);
    if (it == mem.guest_mspaces.end() || !accessible_heap(mem, it->second.base, it->second.size))
        return false;

    mspace handle = mem.memory.get() + space;
    const struct mallinfo info = mspace_mallinfo(handle);
    stats.capacity = it->second.size;
    stats.footprint = static_cast<uint32_t>(mspace_footprint(handle));
    stats.peak_in_use = static_cast<uint32_t>(info.usmblks);
    stats.current_in_use = static_cast<uint32_t>(info.uordblks);
    return true;
}
