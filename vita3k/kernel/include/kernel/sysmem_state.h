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

#include <kernel/types.h>

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

struct EmuEnvState;
struct MemState;

struct MemBlockClientInfo {
    SceUInt32 open_flags = 0;
    std::array<SceUInt32, 8> capabilities{};
};

struct MemBlockOpenLease;

struct KernelMemBlock : SceKernelMemBlockInfo {
    char name[SCE_UID_NAMELEN + 1];
    std::optional<MemBlockClientInfo> client_info;
    std::atomic<SceUInt32> open_clients{ 0 };
    std::shared_ptr<MemBlockOpenLease> base_lease;
    uint8_t *shared_backing = nullptr;
    bool mirror = false;
};

using KernelMemBlockPtr = std::shared_ptr<KernelMemBlock>;
using Blocks = std::map<SceUID, KernelMemBlockPtr>;

struct MemBlockOpenLease {
    KernelMemBlockPtr base;

    explicit MemBlockOpenLease(KernelMemBlockPtr base)
        : base(std::move(base)) {
        this->base->open_clients.fetch_add(1, std::memory_order_relaxed);
    }

    ~MemBlockOpenLease() {
        base->open_clients.fetch_sub(1, std::memory_order_relaxed);
    }
};

struct SysmemState {
    std::mutex mutex;
    Blocks blocks;

    uint32_t allocated_user = 0;
    uint32_t allocated_cdram = 0;
    uint32_t allocated_phycont = 0;

    // The blocks for code. Only sceKernelFreeMemBlockForVM takes one out and gives its space back.
    std::map<SceUID, SceKernelVARange> vm_blocks;
    // The address space they take, of the 16 MiB they share
    SceSize vm_space_used = 0;
    bool vm_domain_open = false;
};

// The shell keeps these fonts in shared blocks that libpvf and WebKit open by name before an app starts.
void create_system_font_blocks(EmuEnvState &emuenv);
