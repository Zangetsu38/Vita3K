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

#include "../SceAppMgr/SceAppMgr.h"
#include "../SceLibKernel/SceLibKernel.h"
#include "SceModulemgr.h"
#include <cpu/state.h>
#include <io/device.h>
#include <io/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <module/load_module.h>
#include <module/module.h>
#include <modules/module_parent.h>

#include <filesystem>
#include <limits>
#include <mutex>
#include <span>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceModulemgrForDriver);

EXPORT(int, ksceKernelGetModuleInfoByAddr, SceUID pid, Ptr<void> module_addr, SceKernelModuleInfo *info) {
    TRACY_FUNC(ksceKernelGetModuleInfoByAddr, pid, module_addr, info);
    return CALL_EXPORT(sceKernelGetModuleInfoByAddr, module_addr, info);
}

EXPORT(SceUID, ksceKernelLoadModule, char *path, int flags, SceKernelLMOption *option) {
    TRACY_FUNC(ksceKernelLoadModule, path, flags, option);
    return CALL_EXPORT(_sceKernelLoadModule, path, flags, option);
}

EXPORT(SceUID, ksceKernelLoadStartModule, const char *path, SceSize args, Ptr<const void> argp, int flags, SceKernelLMOption *option, int *status) {
    TRACY_FUNC(ksceKernelLoadStartModule, path, args, argp, flags, option, status);
    return CALL_EXPORT(_sceKernelLoadStartModule, path, args, argp, flags, option, status);
}

static SceUID load_start_module_for_pid(EmuEnvState &emuenv, SceUID thread_id, const char *export_name, SceUID pid, Ptr<const char> path, SceSize args, Ptr<const void> argp, int flags, Ptr<SceKernelLMOption> option, Ptr<int> status, bool is_shared) {
    const bool start_requested = !is_shared || (static_cast<uint32_t>(flags) & 0x00100000) == 0;
    if (pid == 0)
        return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_INVALID_PROCESS_UID);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    MemState &caller_mem = context->thread->cpu->mem;
    MemState *target_mem = &caller_mem;
    IOState *target_io = context->io;
    const auto caller_identity = emuenv.kernel.get_process_identity(caller_mem);
    if (is_shared && (!caller_identity || caller_identity->pid != pid)) {
        target_mem = find_process_mem_by_pid(emuenv, pid);
        target_io = find_process_io_by_pid(emuenv, pid);
        if (!target_mem || !target_io)
            return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_INVALID_PROCESS_UID);
    }
    MemState &mem = *target_mem;
    const auto identity = emuenv.kernel.get_process_identity(mem);
    if (!path)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    const uint32_t handled_flags = is_shared ? 0x00108000u : 0u;
    const uint32_t ignored_flags = static_cast<uint32_t>(flags) & ~handled_flags;
    const uint32_t module_load_flags = (static_cast<uint32_t>(flags) & 0x8000u) | (is_shared ? 0x8000u : 0u);
    if (ignored_flags != 0)
        LOG_WARN("Ignoring module load flags whose loader behavior is not implemented");
    if (emuenv.kernel.is_primary_address_space(mem) && !is_shared) {
        if (!identity || identity->pid != pid || &caller_mem != &mem) {
            LOG_ERROR("Loading a module for another process {} is not implemented", pid);
            return RET_ERROR(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
        }
        return CALL_EXPORT(ksceKernelLoadStartModule, path.get(caller_mem), args, argp, flags, option.get(caller_mem), status.get(caller_mem));
    }

    if (!identity || identity->pid != pid || (!is_shared && &caller_mem != &mem)) {
        LOG_ERROR("Loading a module for another process {} is not implemented", pid);
        return RET_ERROR(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
    }
    if (&caller_mem != &mem && start_requested && args != 0
        && (!argp || !is_valid_addr_len(caller_mem, argp.address(), args) || !is_valid_addr_len(mem, argp.address(), args)))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    if (!is_shared && (static_cast<uint32_t>(flags) & 0xFFF8260F) != 0)
        return RET_ERROR(SCE_KERNEL_ERROR_INVALID_FLAGS);
    if (!is_shared && option) {
        const GuestMemoryRange range{ option.address(), sizeof(SceKernelLMOption) };
        const uint64_t option_pin = pin_guest_memory(mem, std::span(&range, 1));
        if (!option_pin)
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        const auto option_size = option.get(mem)->size;
        unpin_guest_memory(mem, option_pin);
        if (option_size != sizeof(SceKernelLMOption))
            return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
    }

    std::string module_path;
    for (uint32_t i = 0; i < 256; ++i) {
        if (uint64_t(path.address()) + i > std::numeric_limits<Address>::max())
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        const GuestMemoryRange range{ path.address() + i, 1 };
        const uint64_t pin = pin_guest_memory(caller_mem, std::span(&range, 1));
        if (!pin)
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        const char ch = *Ptr<const char>(range.address).get(caller_mem);
        unpin_guest_memory(caller_mem, pin);
        if (!ch)
            break;
        module_path.push_back(ch);
    }
    if (module_path.size() == 256)
        return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_NAMETOOLONG);
    const auto module_name = std::filesystem::path(module_path).stem().string();
    LOG_TRACE("{}(pid:{}, path: {}; module_name:{}, args:{}, argp:{}, flags:{}, option:{}, status:{})",
        export_name, to_debug_str(caller_mem, pid), to_debug_str(caller_mem, module_path), to_debug_str(caller_mem, module_name), to_debug_str(caller_mem, args), to_debug_str(caller_mem, argp),
        to_debug_str(caller_mem, flags), to_debug_str(caller_mem, option), to_debug_str(caller_mem, status));
    const auto app_device = device::get_device(target_io->app_path);
    const bool restricted_app_device = app_device == VitaIoDevice::vs0 || app_device == VitaIoDevice::pd0;
    const bool system_module = module_path.starts_with("vs0:sys/external/");
    const bool same_process_app_module = caller_identity && caller_identity->pid == pid && module_path.starts_with("app0:sce_module/");
    if ((!system_module && !same_process_app_module) || (restricted_app_device && !identity->is_firmware_app())) {
        LOG_ERROR("PID-targeted dynamic loading only supports same-process app modules and vs0 system modules: {}", module_path);
        return RET_ERROR(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
    }
    if (status && start_requested) {
        const GuestMemoryRange range{ status.address(), sizeof(int) };
        const uint64_t status_pin = pin_guest_memory(caller_mem, std::span(&range, 1));
        if (!status_pin)
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        unpin_guest_memory(caller_mem, status_pin);
    }
    static std::mutex secondary_load_mutex;
    SceUID uid;
    bool start = false;
    SceInt32 start_result = SCE_KERNEL_START_SUCCESS;
    std::optional<ModuleStartLease> lease;
    {
        const std::lock_guard lock(secondary_load_mutex);
        const auto existing = emuenv.kernel.find_module_by_path(mem, module_path);
        if (existing) {
            if (existing->startup_references.load() != 0) {
                LOG_ERROR("Secondary module {} is still starting", module_path);
                return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_BUSY);
            }
            uid = existing->info.modid;
            if (existing->secondary_start_result)
                start_result = *existing->secondary_start_result;
            else if (existing->secondary_start_pending && start_requested) {
                auto acquired = emuenv.kernel.acquire_module_start(mem, existing);
                if (!acquired) {
                    LOG_ERROR("Cannot start secondary module {} after loading", module_path);
                    return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_NO_MOD);
                }
                lease.emplace(std::move(*acquired));
                start = true;
            }
        } else {
            const uint8_t firmware_key[16]{};
            uid = load_module(emuenv, mem, *target_io, module_path, firmware_key, false,
                module_load_flags, is_shared);
            if (uid >= 0 && start_requested) {
                const auto module = emuenv.kernel.find_module(mem, uid);
                auto acquired = emuenv.kernel.acquire_module_start(mem, module);
                if (!acquired) {
                    LOG_ERROR("Cannot start secondary module {} after loading", module_path);
                    return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_NO_MOD);
                }
                lease.emplace(std::move(*acquired));
                start = true;
            } else if (uid >= 0) {
                const auto module = emuenv.kernel.find_module(mem, uid);
                if (!module) {
                    LOG_ERROR("Loaded secondary module {} is not registered", uid);
                    return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_NO_MOD);
                }
                module->secondary_start_pending = true;
            }
        }
    }
    if (uid < 0)
        return uid;
    if (start) {
        start_result = static_cast<SceInt32>(start_module(emuenv, *lease->module(), args, argp));
        const std::lock_guard lock(secondary_load_mutex);
        lease->module()->secondary_start_result = start_result;
        lease->module()->secondary_start_pending = false;
    }
    if (status && start_requested) {
        const GuestMemoryRange range{ status.address(), sizeof(int) };
        const uint64_t status_pin = pin_guest_memory(caller_mem, std::span(&range, 1));
        if (!status_pin)
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        const auto value = static_cast<uint32_t>(start_result);
        for (size_t i = 0; i < sizeof(value); ++i)
            *Ptr<uint8_t>(status.address() + static_cast<Address>(i)).get(caller_mem) = static_cast<uint8_t>(value >> (i * 8));
        unpin_guest_memory(caller_mem, status_pin);
    }
    return uid;
}

static int stop_unload_module_for_pid(EmuEnvState &emuenv, SceUID thread_id, const char *export_name, SceUID pid, SceUID modid, SceSize args, Ptr<const void> argp, int flags, SceKernelULMOption *option, int *status, bool is_shared) {
    if (pid == 0)
        return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_INVALID_PROCESS_UID);
    if (is_shared) {
        if (flags != 0)
            return RET_ERROR(SCE_KERNEL_ERROR_INVALID_FLAGS);
        option = nullptr;
    } else {
        if ((static_cast<uint32_t>(flags) & 0xBFFFFFFFu) != 0)
            return RET_ERROR(SCE_KERNEL_ERROR_INVALID_FLAGS);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    MemState &caller_mem = caller->cpu->mem;
    MemState *target_mem = &caller_mem;
    const auto caller_identity = emuenv.kernel.get_process_identity(caller_mem);
    if (!caller_identity || caller_identity->pid != pid) {
        target_mem = find_process_mem_by_pid(emuenv, pid);
        if (!target_mem)
            return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_INVALID_PROCESS_UID);
    }

    MemState &mem = *target_mem;
    const auto identity = emuenv.kernel.get_process_identity(mem);
    if (!identity || identity->pid != pid)
        return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_INVALID_PROCESS_UID);
    if (&caller_mem == &mem)
        return CALL_EXPORT(ksceKernelStopUnloadModule, modid, args, argp, static_cast<int>(static_cast<uint32_t>(flags) | 0x08000000u), option, status);
    if (args != 0 && (!argp || !is_valid_addr_len(caller_mem, argp.address(), args) || !is_valid_addr_len(mem, argp.address(), args)))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);

    const auto module = emuenv.kernel.find_module(mem, modid);
    if (!module)
        return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_NO_MOD);
    const int stop_result = static_cast<int>(stop_module(emuenv, *module, args, argp));
    if (status)
        *status = stop_result;
    if (option && option->size != sizeof(*option))
        return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
    return unload_module(emuenv, mem, modid);
}

EXPORT(SceUID, ksceKernelLoadStartModuleForPid, SceUID pid, Ptr<const char> path, SceSize args, Ptr<const void> argp, int flags, Ptr<SceKernelLMOption> option, Ptr<int> status) {
    TRACY_FUNC(ksceKernelLoadStartModuleForPid, pid, path, args, argp, flags, option, status);
    return load_start_module_for_pid(emuenv, thread_id, export_name, pid, path, args, argp, flags, option, status, false);
}

EXPORT(SceUID, ksceKernelLoadStartSharedModuleForPid, SceUID pid, Ptr<const char> path, SceSize args, Ptr<const void> argp, int flags, Ptr<SceKernelLMOption> option, Ptr<int> status) {
    TRACY_FUNC(ksceKernelLoadStartSharedModuleForPid, pid, path, args, argp, flags, option, status);
    return load_start_module_for_pid(emuenv, thread_id, export_name, pid, path, args, argp, flags, option, status, true);
}

EXPORT(int, ksceKernelRegisterLibary, const char *library_name, int version, void *entry_table) {
    TRACY_FUNC(ksceKernelRegisterLibary, library_name, version, entry_table);
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelReleaseLibary, const char *library_name) {
    TRACY_FUNC(ksceKernelReleaseLibary, library_name);
    return UNIMPLEMENTED();
}

EXPORT(SceUID, ksceKernelSearchModuleByName, const char *module_name) {
    TRACY_FUNC(ksceKernelSearchModuleByName, module_name);
    const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);

    for (const auto &[module_id, module] : emuenv.kernel.loaded_modules) {
        if (std::strncmp(module->info.module_name, module_name, sizeof(module->info.module_name)) == 0)
            return module_id;
    }

    return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_NO_MOD);
}

EXPORT(int, ksceKernelStartModule, SceUID modid, SceSize args, Ptr<const void> argp, int flags, SceKernelLMOption *option, int *status) {
    TRACY_FUNC(ksceKernelStartModule, modid, args, argp, flags, option, status);
    return CALL_EXPORT(_sceKernelStartModule, modid, args, argp, flags, reinterpret_cast<const SceKernelStartModuleOpt *>(option), status);
}

EXPORT(int, ksceKernelStopModule, SceUID modid, SceSize args, Ptr<const void> argp, int flags, SceKernelULMOption *option, int *status) {
    TRACY_FUNC(ksceKernelStopModule, modid, args, argp, flags, option, status);
    return CALL_EXPORT(_sceKernelStopModule, modid, args, argp, flags, reinterpret_cast<const SceKernelStopModuleOpt *>(option), status);
}

EXPORT(int, ksceKernelStopUnloadModule, SceUID modid, SceSize args, Ptr<const void> argp, int flags, SceKernelULMOption *option, int *status) {
    TRACY_FUNC(ksceKernelStopUnloadModule, modid, args, argp, flags, option, status);
    return CALL_EXPORT(_sceKernelStopUnloadModule, modid, args, argp, flags, option, status);
}

EXPORT(int, ksceKernelStopUnloadModuleForPid, SceUID pid, SceUID modid, SceSize args, Ptr<const void> argp, int flags, SceKernelULMOption *option, int *status) {
    TRACY_FUNC(ksceKernelStopUnloadModuleForPid, pid, modid, args, argp, flags, option, status);
    return stop_unload_module_for_pid(emuenv, thread_id, export_name, pid, modid, args, argp, flags, option, status, false);
}

EXPORT(int, ksceKernelStopUnloadSharedModuleForPid, SceUID pid, SceUID modid, SceSize args, Ptr<const void> argp, int flags, SceKernelULMOption *option, int *status) {
    TRACY_FUNC(ksceKernelStopUnloadSharedModuleForPid, pid, modid, args, argp, flags, option, status);
    return stop_unload_module_for_pid(emuenv, thread_id, export_name, pid, modid, args, argp, flags, option, status, true);
}

EXPORT(int, ksceKernelUnloadModule, SceUID modid, int flags, SceKernelULMOption *option) {
    TRACY_FUNC(ksceKernelUnloadModule, modid, flags, option);
    return CALL_EXPORT(_sceKernelUnloadModule, modid, flags, option);
}
