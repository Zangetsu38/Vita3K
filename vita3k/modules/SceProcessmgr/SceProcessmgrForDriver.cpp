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

#include "kernel/state.h"

#include "../SceSysmem/secondary_memblock.h"

#include <cpu/state.h>
#include <kernel/thread/thread_state.h>
#include <module/module.h>
#include <util/tracy.h>

TRACY_MODULE_NAME(SceProcessmgrForDriver)

EXPORT(int, ksceKernelCreateProcessLocalStorage, Ptr<const char> name, SceSize size) {
    TRACY_FUNC(ksceKernelCreateProcessLocalStorage, name, size);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    MemState &mem = caller->cpu->mem;
    if (!emuenv.kernel.get_process_identity(mem)) {
        LOG_ERROR("{}: caller has no process identity", export_name);
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    }
    std::string copied_name;
    if (!secondary_block_name(mem, name.address(), copied_name))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    if (!size || size > UINT32_MAX - 0xFFF)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);
    const std::lock_guard lock(emuenv.kernel.mutex);
    const Address address = emuenv.kernel.process_local_storage.create(mem, size, copied_name.c_str());
    if (!address)
        return RET_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
    return static_cast<int>(address >> 1);
}

EXPORT(int, ksceKernelGetProcessInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetProcessLocalStorageAddr) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetProcessLocalStorageAddrForPid, SceUID pid, int key, Ptr<Ptr<void>> out_addr, int create_if_doesnt_exist) {
    TRACY_FUNC(ksceKernelGetProcessLocalStorageAddrForPid, pid, key, out_addr, create_if_doesnt_exist);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    MemState &mem = caller->cpu->mem;
    const auto identity = emuenv.kernel.get_process_identity(mem);
    if (!identity || (pid && pid != identity->pid)) {
        LOG_ERROR("{}: process-local storage lookup for PID {} from another process is not implemented", export_name, pid);
        return RET_ERROR(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
    }
    const GuestMemoryRange range{ out_addr.address(), sizeof(Address) };
    const uint64_t pin = out_addr ? pin_guest_memory(mem, std::span(&range, 1)) : 0;
    if (!pin)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    Address address;
    {
        const std::lock_guard lock(emuenv.kernel.mutex);
        address = key > 0 ? emuenv.kernel.process_local_storage.find(mem, static_cast<uint32_t>(key)) : 0;
    }
    if (!address) {
        unpin_guest_memory(mem, pin);
        LOG_ERROR("{}: unknown process-local storage key {} for PID {}", export_name, key, identity->pid);
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_UID);
    }
    for (size_t i = 0; i < sizeof(address); ++i)
        *Ptr<uint8_t>(out_addr.address() + static_cast<Address>(i)).get(mem) = static_cast<uint8_t>(address >> (i * 8));
    unpin_guest_memory(mem, pin);
    return 0;
}

EXPORT(int, ksceKernelGetProcessStatus) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetProcessTimeCore) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetProcessTimeLowCore) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetProcessTimeWideCore) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetRemoteProcessTime) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelIsCDialogAvailable) {
    return 1;
}

EXPORT(int, ksceKernelIsGameBudget) {
    return UNIMPLEMENTED();
}

// kubridge-required stubs
EXPORT(SceUID, ksceKernelAllocRemoteProcessHeap, SceUID pid, uint32_t size, Ptr<void> opt) {
    TRACY_FUNC(ksceKernelAllocRemoteProcessHeap, pid, size, opt);
    STUBBED("ksceKernelAllocRemoteProcessHeap");
    return emuenv.kernel.get_next_uid();
}

EXPORT(int, ksceKernelFreeRemoteProcessHeap, SceUID pid, Ptr<void> ptr) {
    TRACY_FUNC(ksceKernelFreeRemoteProcessHeap, pid, ptr);
    STUBBED("ksceKernelFreeRemoteProcessHeap");
    return 0;
}
