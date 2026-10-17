// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "SceSysmemForDriver.h"
#include "memblock_validation.h"

#include <kernel/sysmem_state.h>

#include <kernel/types.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <util/log.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

struct SecondaryMemblock {
    std::shared_ptr<MemBlockOpenLease> base_lease;
    Block allocation;
    SceKernelMemBlockInfo info{};
    std::string name;
    bool vm = false;
    std::optional<MemBlockClientInfo> client_info;
    uint8_t *shared_backing = nullptr;
    SceUInt32 opened_with_flags = 0;
};

struct SecondaryMemblockState {
    std::mutex mutex;
    std::map<SceUID, SecondaryMemblock> blocks;
    uint64_t allocated_user = 0;
    SceSize vm_space_used = 0;
};

inline bool secondary_guest_copy(MemState &mem, Address address, void *data, uint32_t size, bool write) {
    const GuestMemoryRange range{ address, size };
    const uint64_t pin = pin_guest_memory(mem, std::span(&range, 1));
    if (!pin)
        return false;
    auto *bytes = static_cast<uint8_t *>(data);
    for (uint32_t i = 0; i < size; ++i) {
        auto *guest = Ptr<uint8_t>(address + i).get(mem);
        if (write)
            *guest = bytes[i];
        else
            bytes[i] = *guest;
    }
    unpin_guest_memory(mem, pin);
    return true;
}

inline bool secondary_block_name(MemState &mem, Address address, std::string &name) {
    if (!address)
        return false;
    for (uint32_t i = 0; i < SCE_UID_NAMELEN; ++i) {
        if (static_cast<uint64_t>(address) + i > UINT32_MAX)
            return false;
        char c;
        if (!secondary_guest_copy(mem, address + i, &c, 1, false))
            return false;
        if (!c)
            return true;
        name.push_back(c);
    }
    return true;
}

inline SceInt32 secondary_prepare_alloc(MemState &mem, Ptr<const char> name_ptr,
    SceKernelMemBlockType type, SceSize size, Ptr<SceKernelAllocMemBlockOpt> opt_ptr,
    bool vm, std::string &name, SceSize &alignment, std::optional<MemBlockClientInfo> &client_info) {
    if (!secondary_block_name(mem, name_ptr.address(), name))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    SceKernelAllocMemBlockOpt opt{};
    if (opt_ptr) {
        if (!secondary_guest_copy(mem, opt_ptr.address(), &opt.size, sizeof(opt.size), false))
            return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        constexpr SceSize sizes[] = { 0x14, 0x38, 0x3C, 0x40 };
        if (!std::ranges::contains(sizes, opt.size))
            return SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE;
        const SceSize requested_size = opt.size;
        if (!secondary_guest_copy(mem, opt_ptr.address(), &opt, requested_size, false))
            return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        if (opt.size != requested_size)
            return SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE;
        constexpr SceUInt32 allowed = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT | SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT
            | SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_ENABLE_OPEN | SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_CLIENT_INFO;
        if ((opt.attr & ~allowed)
            || (opt.size == sizeof(opt) && opt.extAttr))
            return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
        if ((opt.attr & SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_CLIENT_INFO) && opt.size < 0x38)
            return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    }
    if (!vm) {
        if (const auto error = type_error(type))
            return error;
        if (const auto error = size_error(type, size))
            return error;
    } else if (size % MiB(1)) {
        return SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK_SIZE;
    } else if (!size) {
        return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    }
    alignment = 0;
    if (opt.attr & SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT) {
        if (const auto error = alignment_error(type, opt.alignment))
            return error;
        alignment = opt.alignment;
    }
    if (opt.attr & SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_ENABLE_OPEN) {
        client_info.emplace();
        if (opt.attr & SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_CLIENT_INFO) {
            client_info->open_flags = opt.openFlags;
            std::copy(std::begin(opt.capabilities), std::end(opt.capabilities), client_info->capabilities.begin());
        }
    }
    if (opt.attr & SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT)
        LOG_WARN("Secondary memblock '{}' requests physical contiguity; its effect is not emulated", name);
    return 0;
}

inline Address secondary_block_region(SceKernelMemBlockType type) {
    const auto partition = type & MEMBLOCK_TYPE_PARTITION_INDEX_MASK;
    const bool uncached = (type & MEMBLOCK_TYPE_MEMORY_ATTR_MASK) == MEMBLOCK_TYPE_MEMORY_ATTR_NORMAL_NC;
    if (partition == MEMBLOCK_TYPE_PARTITION_INDEX_USER_CDRAM)
        return USER_CDRAM_BASE;
    if (partition == MEMBLOCK_TYPE_PARTITION_INDEX_USER_CDIALOG)
        return uncached ? USER_CDIALOG_UNCACHED_BASE : USER_CDIALOG_BASE;
    return uncached ? USER_UNCACHED_BASE : USER_MAIN_BASE;
}

inline bool secondary_takes_user(SceKernelMemBlockType type) {
    const auto part = type & MEMBLOCK_TYPE_PHYMEMPART_MASK;
    return (type & MEMBLOCK_TYPE_PARTITION_INDEX_MASK) != MEMBLOCK_TYPE_PARTITION_INDEX_USER_CDIALOG
        && part != MEMBLOCK_TYPE_PHYMEMPART_CDRAM && part != MEMBLOCK_TYPE_PHYMEMPART_PHYCONT
        && part != MEMBLOCK_TYPE_PHYMEMPART_CDIALOG;
}

inline SceInt32 secondary_alloc_block(SecondaryMemblockState &state, MemState &mem, SceUID uid,
    const std::string &name, SceKernelMemBlockType type, SceSize size, SceSize requested_alignment, bool vm,
    std::optional<MemBlockClientInfo> client_info = std::nullopt,
    std::shared_ptr<MemBlockOpenLease> base_lease = {}, uint8_t *shared_backing = nullptr, SceUInt32 opened_with_flags = 0) {
    std::lock_guard lock(state.mutex);
    if (vm && size > MiB(16) - state.vm_space_used)
        return SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW;
    if (secondary_takes_user(type) && state.allocated_user + size > MiB(256))
        return SCE_KERNEL_ERROR_NO_MEMORY;
    const auto part = type & MEMBLOCK_TYPE_PHYMEMPART_MASK;
    SceSize alignment = part == MEMBLOCK_TYPE_PHYMEMPART_CDRAM ? MEMBLOCK_CDRAM_MIN_ALIGNMENT
        : part == MEMBLOCK_TYPE_PHYMEMPART_PHYCONT             ? MEMBLOCK_PHYCONT_MIN_ALIGNMENT
                                                               : MEMBLOCK_MIN_ALIGNMENT;
    alignment = std::max({ alignment, requested_alignment, size & -size });
    const Address address = alloc_aligned(mem, size, name.c_str(), alignment, secondary_block_region(type));
    if (!address)
        return SCE_KERNEL_ERROR_NO_MEMORY;
    if (shared_backing && !map_external_memory(mem, address, size, shared_backing)) {
        LOG_ERROR("Could not map shared memblock '{}' into the secondary address space", name);
        free(mem, address);
        return SCE_KERNEL_ERROR_ERROR;
    }
    SecondaryMemblock block;
    block.allocation = Block(address, [&mem, shared_backing, size](Address addr) {
        if (shared_backing)
            remove_external_mapping(mem, shared_backing, size);
        free(mem, addr);
    });
    block.info.size = sizeof(SceKernelMemBlockInfo);
    block.info.type = type;
    block.info.mappedBase = Ptr<void>(address);
    block.info.mappedSize = size;
    block.info.memoryType = ((type & MEMBLOCK_TYPE_MEMORY_ATTR_MASK) >> MEMBLOCK_TYPE_MEMORY_ATTR_SHIFT) & SCE_KERNEL_MEMORY_TYPE_NORMAL;
    block.info.access = ((type & MEMBLOCK_TYPE_USER_ACCESS_MASK) >> MEMBLOCK_TYPE_USER_ACCESS_SHIFT) & (SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
    block.name = name;
    block.vm = vm;
    block.client_info = std::move(client_info);
    block.base_lease = std::move(base_lease);
    block.shared_backing = shared_backing;
    block.opened_with_flags = opened_with_flags;
    state.blocks.emplace(uid, std::move(block));
    if (secondary_takes_user(type))
        state.allocated_user += size;
    if (vm)
        state.vm_space_used += size;
    if (shared_backing)
        LOG_DEBUG("Shared memblock mirror '{}' uid={} mapped at {} size={}", name, log_hex(uid), log_hex(address), log_hex(size));
    return uid;
}

inline SceInt32 secondary_open_block(SecondaryMemblockState &state, std::string_view name) {
    std::lock_guard lock(state.mutex);
    for (const auto &[uid, block] : state.blocks) {
        if (block.name == name)
            return uid;
    }
    return SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME;
}

inline SceInt32 secondary_free_block(SecondaryMemblockState &state, SceUID uid, bool vm_only,
    const std::function<bool(Address, SceSize)> &in_use = {}) {
    std::lock_guard lock(state.mutex);
    const auto it = state.blocks.find(uid);
    if (it == state.blocks.end())
        return vm_only ? SCE_KERNEL_ERROR_INVALID_ARGUMENT : SCE_KERNEL_ERROR_INVALID_UID;
    if (vm_only && !it->second.vm)
        return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    if (in_use && in_use(it->second.info.mappedBase.address(), it->second.info.mappedSize))
        return SCE_KERNEL_ERROR_BLOCK_IN_USE;
    if (secondary_takes_user(it->second.info.type))
        state.allocated_user -= it->second.info.mappedSize;
    if (it->second.vm)
        state.vm_space_used -= it->second.info.mappedSize;
    state.blocks.erase(it);
    return SCE_KERNEL_OK;
}

inline SceInt32 secondary_block_base(SecondaryMemblockState &state, MemState &mem, SceUID uid, Address output) {
    std::lock_guard lock(state.mutex);
    const auto it = state.blocks.find(uid);
    if (it == state.blocks.end())
        return SCE_KERNEL_ERROR_INVALID_UID;
    Address base = it->second.info.mappedBase.address();
    return secondary_guest_copy(mem, output, &base, sizeof(base), true) ? SCE_KERNEL_OK : SCE_KERNEL_ERROR_ILLEGAL_ADDR;
}

inline SceInt32 secondary_find_block(SecondaryMemblockState &state, MemState &mem, Address address, Address output, bool info) {
    std::lock_guard lock(state.mutex);
    for (const auto &[uid, block] : state.blocks) {
        const uint64_t base = block.info.mappedBase.address();
        if (address >= base && static_cast<uint64_t>(address) < base + block.info.mappedSize) {
            if (!info)
                return uid;
            auto copy = block.info;
            return secondary_guest_copy(mem, output, &copy, sizeof(copy), true) ? SCE_KERNEL_OK : SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        }
    }
    return SCE_KERNEL_ERROR_BLOCK_ERROR;
}
