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
#include <module/bridge.h>
#include <util/types.h>

struct CPUState;
struct EmuEnvState;
struct KernelState;
struct KernelModule;
struct IOState;

void init_libraries(EmuEnvState &emuenv);
bool init_exported_vars(EmuEnvState &emuenv, MemState &mem);
CallResult call_import(EmuEnvState &emuenv, CPUState &cpu, uint32_t nid, SceUID thread_id);

// Returns true if the NID has an HLE (C++) implementation in nids.inc.
// Used to determine if an LLE export should be overridden with HLE dispatch.
bool has_hle_implementation(uint32_t nid);

/**
 * \brief Loads a dynamic module into memory if it wasn't already loaded. If it was, find it and return it.
 * \param emuenv PlayStation Vita emulated environment
 * \param module_path Full path of module file (with device)
 * \return UID of the loaded module object or SCE_ERROR on failure
 */
SceUID load_module(EmuEnvState &emuenv, const std::string &module_path);
SceUID load_module(EmuEnvState &emuenv, MemState &mem, IOState &io, const std::string &module_path, const uint8_t *license_key = nullptr, bool allow_process_tls = true, uint32_t module_load_flags = 0, bool is_shared = false);
SceUID load_hle_module_stub(EmuEnvState &emuenv, MemState &mem, const std::string &module_path, uint32_t module_load_flags = 0);
bool preload_process_modules(EmuEnvState &emuenv, MemState &mem, IOState &io, const uint8_t *license_key = nullptr);
int unload_module(EmuEnvState &emuenv, SceUID module_id);
int unload_module(EmuEnvState &emuenv, MemState &mem, SceUID module_id);

uint32_t start_module(EmuEnvState &emuenv, const KernelModule &module, SceSize args = 0, Ptr<const void> argp = Ptr<const void>{});
uint32_t stop_module(EmuEnvState &emuenv, const KernelModule &module, SceSize args = 0, Ptr<const void> argp = Ptr<const void>{});

/**
 * \brief Loads and run a system module
 * \param emuenv PlayStation Vita emulated environment
 * \param mem Address space into which the module is loaded
 * \param io Process IO state used to load module files
 * \param module_id System Module ID of the module to load
 * \return False on failure, true on success
 */
bool load_sys_module(EmuEnvState &emuenv, MemState &mem, IOState &io, SceSysmoduleModuleId module_id);
int unload_sys_module(EmuEnvState &emuenv, SceSysmoduleModuleId module_id);
bool load_sys_module_internal_with_arg(EmuEnvState &emuenv, MemState &mem, IOState &io, SceSysmoduleInternalModuleId module_id, SceSize args, Ptr<void> argp, int *retcode);

void load_taihen_plugins_for_title(EmuEnvState &emuenv, const std::string &titleid);
