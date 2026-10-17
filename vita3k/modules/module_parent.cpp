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

#include "io/functions.h"
#include "io/io.h"

#include <modules/module_parent.h>
#include <modules/secondary_imports.h>

#include <cpu/state.h>
#include <emuenv/state.h>
#include <io/device.h>
#include <io/state.h>
#include <io/vfs.h>
#include <kernel/load_self.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <module/load_module.h>
#include <nids/functions.h>
#include <packages/license.h>
#include <packages/sce_types.h>
#include <util/find.h>
#include <util/lock_and_find.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <array>
#include <limits>
#include <optional>
#include <unordered_set>

static constexpr bool LOG_UNK_NIDS_ALWAYS = false;
static constexpr bool STOP_ON_UNISOLATED_SECONDARY_IMPORT = false;

#define LIBRARY(name) extern const LibraryInitFn import_library_init_##name;
#include <modules/library_init_list.inc>
#undef LIBRARY

#define VAR_NID(name, nid) extern const ImportVarFactory import_##name;
#define NID(name, nid) extern const ImportFn import_##name;
#include <nids/nids.inc>
#undef NID
#undef VAR_NID

struct EmuEnvState;

static const ImportFn *resolve_import(uint32_t nid) {
    switch (nid) {
#define VAR_NID(name, nid)
#define NID(name, nid) \
    case nid:          \
        return &import_##name;
#include <nids/nids.inc>
#undef NID
#undef VAR_NID
    default:
        return nullptr;
    }
}

bool has_hle_implementation(uint32_t nid) {
    return resolve_import(nid) != nullptr;
}

struct VarExport {
    uint32_t nid;
    ImportVarFactory factory;
};

bool init_exported_vars(EmuEnvState &emuenv, MemState &mem) {
    const auto var_exports = std::to_array<VarExport>({
#define NID(name, nid)
#define VAR_NID(name, nid) \
    {                      \
        nid,               \
        import_##name      \
    },
#include <nids/nids.inc>
#undef VAR_NID
#undef NID
    });

    std::vector<ExportedVariableDefinition> definitions;
    definitions.reserve(var_exports.size());
    for (const auto &var : var_exports) {
        definitions.push_back({ var.nid, [&emuenv, factory = var.factory](MemState &target_mem) {
                                   return factory(emuenv, target_mem);
                               } });
    }
    return initialize_exported_variables(emuenv.kernel, mem, definitions);
}

static void log_import_call(char emulation_level, uint32_t nid, SceUID thread_id, const std::unordered_set<uint32_t> &nid_blacklist, Address lr) {
    if (!nid_blacklist.contains(nid)) {
        const char *const name = import_name(nid);
        LOG_TRACE("[{}LE] TID: {:<3} FUNC: {} {} at {}", emulation_level, thread_id, log_hex(nid), name, log_hex(lr));
    }
}

CallResult call_import(EmuEnvState &emuenv, CPUState &cpu, uint32_t nid, SceUID thread_id) {
    if (!emuenv.kernel.is_primary_address_space(cpu.mem) && !secondary_import_allowed(nid, cpu.get_reg(0), cpu.get_reg(2))) {
        if (STOP_ON_UNISOLATED_SECONDARY_IMPORT) {
            LOG_ERROR("Stopping secondary thread {}: HLE import {} ({}) is not isolated (r0={}, r1={}, r2={}, r3={})",
                thread_id, import_name(nid), log_hex(nid), log_hex(cpu.get_reg(0)), log_hex(cpu.get_reg(1)),
                log_hex(cpu.get_reg(2)), log_hex(cpu.get_reg(3)));
            const auto thread = emuenv.kernel.get_thread(thread_id);
            if (thread)
                thread->exit(static_cast<SceInt32>(SCE_KERNEL_ERROR_NOT_IMPLEMENTED));
            return { .regs = { static_cast<uint32_t>(SCE_KERNEL_ERROR_NOT_IMPLEMENTED) }, .count = 1 };
        }
        const uint64_t occurrence = cpu.mem.record_secondary_import_warning(nid);
        if (secondary_import_should_log(occurrence)) {
            LOG_WARN("Executing non-isolated HLE import {} ({}) on secondary thread {} (r0={}, r1={}, r2={}, r3={}); further occurrences suppressed for this process",
                import_name(nid), log_hex(nid), thread_id, log_hex(cpu.get_reg(0)), log_hex(cpu.get_reg(1)),
                log_hex(cpu.get_reg(2)), log_hex(cpu.get_reg(3)));
        }
    }
    // HLE - call our C++ function
    if (emuenv.kernel.debugger.watch_import_calls) {
        const std::unordered_set<uint32_t> hle_nid_blacklist = {
            0xB295EB61, // sceKernelGetTLSAddr
            0x46E7BE7B, // sceKernelLockLwMutex
            0x91FA6614, // sceKernelUnlockLwMutex
        };
        auto lr = cpu.get_lr();
        log_import_call('H', nid, thread_id, hle_nid_blacklist, lr);
    }
    const ImportFn *fn = resolve_import(nid);
    if (fn)
        return (*fn)(emuenv, cpu, thread_id);

    if (!emuenv.missing_nids.contains(nid) || LOG_UNK_NIDS_ALWAYS) {
        const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
        LOG_ERROR("Import function for NID {} not found (thread name: {}, thread ID: {}, PC: {}, LR: {})",
            log_hex(nid), thread->name, thread_id, log_hex(cpu.get_pc()), log_hex(cpu.get_lr()));
        if (!LOG_UNK_NIDS_ALWAYS)
            emuenv.missing_nids.insert(nid);
    }
    // A missing function returns 0
    return { .regs = { 0 }, .count = 1 };
}

struct SceKernelBootimageModules {
    Ptr<const char> path;
    Ptr<const void> data;
    SceSize size;
};

struct SceKernelBootimageInfo {
    SceSize number;
    Ptr<const SceKernelBootimageModules> list;
};

constexpr uint32_t nid_sceKernelBootimageInfo = 0x9C08E88A;

SceUID load_module(EmuEnvState &emuenv, const std::string &module_path) {
    return load_module(emuenv, emuenv.mem, emuenv.io, module_path);
}

SceUID load_hle_module_stub(EmuEnvState &emuenv, MemState &mem, const std::string &module_path, uint32_t module_load_flags) {
    if (const auto loaded = emuenv.kernel.find_module_by_path(mem, module_path))
        return loaded->info.modid;

    const auto module_name = fs::path(module_path).stem().string();
    const SceKernelModulePtr kernel_module = std::make_shared<KernelModule>(mem);
    kernel_module->module_flags = (module_load_flags & 0x8000) != 0 ? 0x1000 : 0;
    auto *module_info = &kernel_module->info;
    module_info->size = sizeof(*module_info);
    strncpy(module_info->path, module_path.c_str(), sizeof(module_info->path) - 1);
    strncpy(module_info->module_name, module_name.c_str(), 28);

    const SceUID uid = emuenv.kernel.get_next_uid();
    module_info->modid = uid;
    {
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        emuenv.kernel.loaded_modules[uid] = kernel_module;
    }
    return uid;
}

SceUID load_module(EmuEnvState &emuenv, MemState &mem, IOState &io, const std::string &module_path, const uint8_t *license_key, bool allow_process_tls, uint32_t module_load_flags, bool is_shared) {
    if (const auto loaded = emuenv.kernel.find_module_by_path(mem, module_path))
        return loaded->info.modid;

    if (is_shared) {
        SceKernelModulePtr shared_source;
        {
            const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
            for (const auto &[_, module] : emuenv.kernel.loaded_modules) {
                if (module->address_space != &mem && module->shared_module_original && module_path == module->info.path) {
                    shared_source = module;
                    break;
                }
            }
        }
        if (shared_source) {
            LOG_INFO("Loading shared module {} from process image {}", module_path, shared_source->info.modid);
            const SceUID module_id = load_self_from_initial_segments(emuenv.kernel, mem, shared_source, module_load_flags);
            if (module_id < 0) {
                if (module_id != SCE_KERNEL_ERROR_NO_MEMORY) {
                    LOG_ERROR("Failed to instantiate shared module {} from {}", module_path, shared_source->info.modid);
                    return module_id;
                }
                LOG_WARN("Shared module {} cannot use its source addresses; retrying with the target process image", module_path);
            } else {
                const auto module = lock_and_find(module_id, emuenv.kernel.loaded_modules, emuenv.kernel.mutex);
                LOG_INFO("Shared module {} (at \"{}\") loaded", module->info.module_name, module_path);
                return module_id;
            }
        }
    }

    LOG_INFO("Loading module \"{}\"", module_path);
    const auto app_device = device::get_device(io.app_path);
    const bool is_firmware_app = app_device == VitaIoDevice::vs0 || app_device == VitaIoDevice::pd0;
    if (!is_firmware_app && module_path.starts_with("vs0:sys/external/")) {
        // check if module is LLEd or not
        // check only for this path because app modules are always LLEd and os0 modules can be loaded only by developer.
        const auto module_name = fs::path(module_path).stem().string();
        if (!is_lle_module(module_name, emuenv)) {
            LOG_INFO("Module {} is HLE. Skipping load.", module_name);
            return load_hle_module_stub(emuenv, mem, module_path, module_load_flags);
        }
    }

    const auto load_module_data = [&](const void *module_data) -> SceUID {
        const bool capture_shared_initial_segments = is_shared || (module_path == "vs0:sys/external/libpaf.suprx" && emuenv.kernel.is_primary_address_space(mem));
        SceUID module_id = load_self(emuenv.kernel, mem, module_data, module_path, emuenv.log_path / "elfdumps" / io.title_id, capture_shared_initial_segments, allow_process_tls, module_load_flags);

        if (module_id >= 0) {
            const auto module = lock_and_find(module_id, emuenv.kernel.loaded_modules, emuenv.kernel.mutex);
            LOG_INFO("Module {} (at \"{}\") loaded", module->info.module_name, module_path);
        } else {
            LOG_ERROR("Failed to load module {}", module_path);
        }

        return module_id;
    };

    const auto system_path = expand_path(io, module_path.c_str(), emuenv.vita_fs_path);

    if (module_path.starts_with("os0:kd/")) {
        if (!fs::exists(system_path)) {
            Address bootimage_address = 0;
            {
                const std::lock_guard<std::mutex> guard(emuenv.kernel.export_nids_mutex);
                const auto bindings = emuenv.kernel.module_bindings(mem);
                const auto it = bindings.exports.find(nid_sceKernelBootimageInfo);
                if (it != bindings.exports.end())
                    bootimage_address = it->second;
            }
            SceKernelBootimageInfo *bootimage_info = Ptr<SceKernelBootimageInfo>(bootimage_address).get(mem);
            if (bootimage_info) {
                for (SceSize i = 0; i < bootimage_info->number; i++) {
                    const SceKernelBootimageModules &module_content = bootimage_info->list.get(mem)[i];
                    if (module_content.path && module_content.data && module_content.size > 0) {
                        if (module_content.path.get(mem) == module_path) {
                            // Load the module from the boot image
                            return load_module_data(module_content.data.get(mem));
                        }
                    }
                }
            }
        }
    }

    vfs::FileBuffer module_buffer;
    const bool read_module = (module_path.starts_with("app0:") && !io.app_game_base_mount.empty()) || module_path.starts_with("addcont0:")
        ? read_file_to_buffer(io, module_path.c_str(), emuenv.vita_fs_path, module_buffer, "load_module")
        : fs_utils::read_data(system_path, module_buffer);
    if (!read_module) {
        LOG_ERROR("Failed to read module file {}", module_path);
        return SCE_ERROR_ERRNO_ENOENT;
    }

    // Firmware processes do not need a title license; avoid modifying the shared license map.
    const uint8_t firmware_key[16]{};
    const uint8_t *key = license_key ? license_key : is_firmware_app && !emuenv.kernel.is_primary_address_space(mem) ? firmware_key
                                                                                                                     : emuenv.license.rif[io.title_id].key;
    const auto identity = emuenv.kernel.get_process_identity(mem);
    const auto module_name_start = module_path.find_last_of(":/");
    const auto module_name = module_path.substr(module_name_start == std::string::npos ? 0 : module_name_start + 1);
    const bool is_main_executable = identity && module_name == identity->self_name;
    std::optional<SceSelfAuthInfo> auth_info;
    module_buffer = decrypt_fself(module_buffer, key, is_main_executable ? &auth_info : nullptr);
    if (module_buffer.empty()) {
        LOG_ERROR("Failed to decrypt module file {}", module_path);
        return SCE_ERROR_ERRNO_ENOENT;
    }

    if (is_main_executable) {
        if (!auth_info) {
            LOG_WARN("SELF authentication info was not available for main executable {}", module_path);
        } else if (!emuenv.kernel.set_process_identity_auth_info(mem, *auth_info)) {
            LOG_ERROR("Failed to register SELF authentication info for {}", module_path);
        } else {
            LOG_DEBUG("Loaded SELF authentication info for process {}", identity->pid);
        }
    }

    return load_module_data(module_buffer.data());
}

bool preload_process_modules(EmuEnvState &emuenv, MemState &mem, IOState &io, const uint8_t *license_key) {
    SceUInt32 disabled = 0;
    const auto process_param = emuenv.kernel.get_process_param(mem);
    if (process_param) {
        constexpr Address process_preload_disabled_offset = static_cast<Address>(offsetof(SceProcessParam, process_preload_disabled));
        if (process_param.address() > std::numeric_limits<Address>::max() - process_preload_disabled_offset) {
            LOG_ERROR("Invalid process parameter address {}", log_hex(process_param.address()));
            return false;
        }
        const Address address = process_param.address() + process_preload_disabled_offset;
        const GuestMemoryRange param_range{ address, sizeof(SceUInt32) };
        const auto param_pin = pin_guest_memory(mem, std::span(&param_range, 1));
        if (!param_pin) {
            LOG_ERROR("Invalid process preload parameters at {}", log_hex(address));
            return false;
        }
        SceUInt32 disabled_address = 0;
        for (size_t i = 0; i < sizeof(disabled_address); ++i)
            disabled_address |= static_cast<SceUInt32>(*Ptr<const uint8_t>(address + static_cast<Address>(i)).get(mem)) << (i * 8);
        unpin_guest_memory(mem, param_pin);
        if (disabled_address) {
            const GuestMemoryRange disabled_range{ disabled_address, sizeof(disabled) };
            const auto disabled_pin = pin_guest_memory(mem, std::span(&disabled_range, 1));
            if (!disabled_pin) {
                LOG_ERROR("Invalid process preload flags at {}", log_hex(disabled_address));
                return false;
            }
            for (size_t i = 0; i < sizeof(disabled); ++i)
                disabled |= static_cast<SceUInt32>(*Ptr<const uint8_t>(disabled_address + static_cast<Address>(i)).get(mem)) << (i * 8);
            unpin_guest_memory(mem, disabled_pin);
        }
    }

    std::vector<std::string> paths;
    const auto add = [&](SceUInt32 flag, SceSysmoduleModuleId id, const char *name, bool from_app) {
        if (disabled & flag)
            return;
        if (is_lle_module(name, emuenv)) {
            const auto filename = fmt::format("{}.suprx", name);
            const auto app_path = fmt::format("app0:sce_module/{}", filename);
            if (from_app && fs::exists(expand_path(io, app_path.c_str(), emuenv.vita_fs_path)))
                paths.push_back(app_path);
            else if (fs::exists(emuenv.vita_fs_path / "vs0/sys/external" / filename))
                paths.push_back(fmt::format("vs0:sys/external/{}", filename));
        }
        if (id != SCE_SYSMODULE_INVALID) {
            const std::lock_guard<std::mutex> guard(emuenv.kernel.mutex);
            emuenv.kernel.loaded_sysmodules[&mem][id] = {};
        }
    };
    const auto app_device = device::get_device(io.app_path);
    paths.emplace_back("os0:kd/bootimage.skprx");
    paths.emplace_back("os0:kd/sysmodule.skprx");
    paths.emplace_back("os0:kd/rtc.skprx");
    add(0x00010000, SCE_SYSMODULE_INVALID, "libc", true);
    add(0x00020000, SCE_SYSMODULE_DBG, "libdbg", false);
    if (emuenv.io.app_path == "vs0:vsh/shell" && io.app_path != "vs0:vsh/shell") {
        add(0x00080000, SCE_SYSMODULE_INVALID, "libshellsvc", false);
        add(0x00100000, SCE_SYSMODULE_INVALID, "libcdlg", false);
    }
    add(0x00200000, SCE_SYSMODULE_FIOS2, "libfios2", true);
    add(0x00400000, SCE_SYSMODULE_APPUTIL, "apputil", false);
    add(0x00800000, SCE_SYSMODULE_INVALID, "libSceFt2", false);
    add(0x01000000, SCE_SYSMODULE_INVALID, "libpvf", false);
    add(0x02000000, SCE_SYSMODULE_PERF, "libperf", false);

    bool success = true;
    const auto identity = emuenv.kernel.get_process_identity(mem);
    for (const auto &path : paths) {
        if (identity)
            LOG_INFO("Process {} preloading module {}", identity->pid, path);
        const auto result = load_module(emuenv, mem, io, path, license_key);
        if (result < 0) {
            LOG_ERROR("Failed to load preloaded module {}", path);
            if (!path.starts_with("os0:kd/"))
                success = false;
        }
    }
    return success;
}

int unload_module(EmuEnvState &emuenv, SceUID module_id) {
    return unload_module(emuenv, emuenv.mem, module_id);
}

int unload_module(EmuEnvState &emuenv, MemState &mem, SceUID module_id) {
    const auto module = emuenv.kernel.find_module(mem, module_id);
    if (!module) {
        const char *export_name = __FUNCTION__;
        return RET_ERROR(SCE_KERNEL_ERROR_MODULEMGR_NO_MOD);
    }
    LOG_INFO("Unloading module {} ({})", module_id, module->info.module_name);
    if (module->info.segments[0].memsz == 0) {
        const std::lock_guard<std::mutex> linker_lock(emuenv.kernel.module_linker_mutex);
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        const auto it = emuenv.kernel.loaded_modules.find(module_id);
        if (it == emuenv.kernel.loaded_modules.end() || it->second != module) {
            LOG_ERROR("Cannot unload module {} because it is no longer registered", module_id);
            return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
        }
        if (module->startup_references.load() != 0) {
            LOG_ERROR("Cannot unload module {} while process startup is using it", module_id);
            return SCE_KERNEL_ERROR_MODULEMGR_IN_USE;
        }
        LOG_WARN("Module {} has no segments, skipping unload", module->info.module_name);
        emuenv.kernel.loaded_modules.erase(it);
        return 0;
    }
    return unload_self(emuenv.kernel, mem, *module);
}

uint32_t start_module(EmuEnvState &emuenv, const KernelModule &owner, SceSize args, Ptr<const void> argp) {
    const auto &module = owner.info;
    const auto module_start = module.start_entry;
    if (module_start) {
        const auto module_name = module.module_name;

        LOG_DEBUG("Running module_start of library: {} at address {}", module_name, log_hex(module_start.address()));
        const uint32_t ret = emuenv.kernel.run_module_entry(owner, true, args, argp);

        LOG_INFO("Module {} (at \"{}\") module_start returned {}", module_name, module.path, log_hex(ret));
        if (ret != SCE_KERNEL_START_SUCCESS)
            LOG_ERROR("Module {} did not return successfully!", module_name);

        return ret;
    }
    return 0;
}

uint32_t stop_module(EmuEnvState &emuenv, const KernelModule &owner, SceSize args, Ptr<const void> argp) {
    const auto &module = owner.info;
    const auto module_stop = module.stop_entry;
    if (module_stop) {
        const auto module_name = module.module_name;

        LOG_DEBUG("Running module_stop of library: {} at address {}", module_name, log_hex(module_stop.address()));
        const uint32_t ret = emuenv.kernel.run_module_entry(owner, false, args, argp);

        LOG_INFO("Module {} (at \"{}\") module_stop returned {}", module_name, module.path, log_hex(ret));
        if (ret != SCE_KERNEL_STOP_SUCCESS)
            LOG_ERROR("Module {} did not stop successfully!", module_name);
        return ret;
    }
    return 0;
}

/**
 * \return False on failure, true on success
 */
bool load_sys_module(EmuEnvState &emuenv, MemState &mem, IOState &io, SceSysmoduleModuleId module_id) {
    const auto &module_paths = sysmodule_paths[module_id];
    std::vector<SceUID> loaded_uids;
    std::vector<SceUID> started_uids;
    const auto rollback = [&] {
        for (auto it = started_uids.rbegin(); it != started_uids.rend(); ++it) {
            if (const auto module = emuenv.kernel.find_module(mem, *it))
                stop_module(emuenv, *module);
        }
        for (auto it = loaded_uids.rbegin(); it != loaded_uids.rend(); ++it) {
            if (unload_module(emuenv, mem, *it) < 0)
                LOG_ERROR("Could not unload system module dependency {}", *it);
        }
    };

    for (const auto module_filename : module_paths) {
        if (!is_lle_module(module_filename, emuenv)) {
            LOG_INFO("Skipping system module component {} because it is not selected for LLE", module_filename);
            continue;
        }

        std::string module_path;
        if (module_id == SCE_SYSMODULE_SMART || module_id == SCE_SYSMODULE_FACE || (module_id == SCE_SYSMODULE_ULT && strcmp(module_filename, "libult") == 0)) {
            module_path = fmt::format("app0:sce_module/{}.suprx", module_filename);
        } else {
            module_path = fmt::format("vs0:sys/external/{}.suprx", module_filename);
        }

        auto loaded_module_uid = load_module(emuenv, mem, io, module_path);
        if (loaded_module_uid < 0) {
            if (module_id == SCE_SYSMODULE_ULT && loaded_module_uid == SCE_ERROR_ERRNO_ENOENT) {
                module_path = fmt::format("vs0:sys/external/{}.suprx", module_filename);
                loaded_module_uid = load_module(emuenv, mem, io, module_path);
                if (loaded_module_uid < 0) {
                    rollback();
                    return false;
                }
            } else {
                rollback();
                return false;
            }
        }
        loaded_uids.push_back(loaded_module_uid);

        const auto module = emuenv.kernel.find_module(mem, loaded_module_uid);
        if (!module) {
            LOG_ERROR("Loaded system module dependency {} is not registered", loaded_module_uid);
            rollback();
            return false;
        }
        if (start_module(emuenv, *module) != SCE_KERNEL_START_SUCCESS) {
            LOG_ERROR("Failed to start system module dependency {}", module_path);
            rollback();
            return false;
        }
        started_uids.push_back(loaded_module_uid);
    }

    std::lock_guard<std::mutex> guard(emuenv.kernel.mutex);
    emuenv.kernel.loaded_sysmodules[&mem][module_id] = std::move(loaded_uids);
    return true;
}

int unload_sys_module(EmuEnvState &emuenv, SceSysmoduleModuleId module_id) {
    const char *export_name = __FUNCTION__;

    std::vector<SceUID> loaded_uids;
    {
        std::lock_guard<std::mutex> guard(emuenv.kernel.mutex);
        auto process = emuenv.kernel.loaded_sysmodules.find(&emuenv.mem);
        if (process == emuenv.kernel.loaded_sysmodules.end())
            return RET_ERROR(SCE_SYSMODULE_ERROR_UNLOADED);
        auto it = process->second.find(module_id);
        if (it == process->second.end())
            return RET_ERROR(SCE_SYSMODULE_ERROR_UNLOADED);
        loaded_uids = std::move(it->second);
        process->second.erase(it);
    }

    // unload the modules in the reverse order they were loaded
    std::ranges::reverse(loaded_uids);

    // first stop everything, then unload
    for (SceUID uid : loaded_uids) {
        const auto module = lock_and_find(uid, emuenv.kernel.loaded_modules, emuenv.kernel.mutex);
        if (!module)
            return RET_ERROR(SCE_SYSMODULE_ERROR_FATAL);

        stop_module(emuenv, *module);
    }
    for (SceUID uid : loaded_uids) {
        int ret = unload_module(emuenv, uid);
        if (ret < 0)
            return RET_ERROR(SCE_SYSMODULE_ERROR_FATAL);
    }

    return 0;
}

bool load_sys_module_internal_with_arg(EmuEnvState &emuenv, MemState &mem, IOState &io, SceSysmoduleInternalModuleId module_id, SceSize args, Ptr<void> argp, int *retcode) {
    LOG_INFO("Loading internal module ID: {}", log_hex(module_id));

    if (!sysmodule_internal_paths.contains(module_id))
        return false;

    const auto &module_paths = sysmodule_internal_paths.at(module_id);
    std::vector<SceUID> loaded_uids;
    std::vector<SceUID> started_uids;
    const auto rollback = [&] {
        for (auto it = started_uids.rbegin(); it != started_uids.rend(); ++it) {
            if (const auto module = emuenv.kernel.find_module(mem, *it))
                stop_module(emuenv, *module);
        }
        for (auto it = loaded_uids.rbegin(); it != loaded_uids.rend(); ++it) {
            if (unload_module(emuenv, mem, *it) < 0)
                LOG_ERROR("Could not unload internal system module {} after a load failure", *it);
        }
    };

    for (auto module_filename : module_paths) {
        std::string module_path = fmt::format("vs0:sys/external/{}.suprx", module_filename);
        const auto loaded_module_uid = load_module(emuenv, mem, io, module_path);

        if (loaded_module_uid < 0) {
            rollback();
            return false;
        }
        loaded_uids.push_back(loaded_module_uid);
        const auto module = emuenv.kernel.find_module(mem, loaded_module_uid);
        if (!module) {
            LOG_ERROR("Loaded internal system module {} is not registered", loaded_module_uid);
            rollback();
            return false;
        }
        const auto ret = start_module(emuenv, *module, args, argp);
        if (ret == SCE_KERNEL_START_SUCCESS)
            started_uids.push_back(loaded_module_uid);
        if (retcode)
            *retcode = static_cast<int>(ret);
    }
    {
        const std::lock_guard<std::mutex> guard(emuenv.kernel.mutex);
        emuenv.kernel.loaded_internal_sysmodules[&mem].push_back(module_id);
    }
    return true;
}

void init_libraries(EmuEnvState &emuenv) {
#define LIBRARY(name) import_library_init_##name(emuenv);
#include <modules/library_init_list.inc>
#undef LIBRARY
}
