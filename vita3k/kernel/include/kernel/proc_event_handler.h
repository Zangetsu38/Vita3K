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

#include <kernel/kernel_object.h>
#include <kernel/thread/thread_state.h>
#include <kernel/types.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>

struct EmuEnvState;
struct MemState;

class ProcEventHandlerObject final : public WithUidClass<KernelObject, UidClass::proc_event_handler> {
public:
    ProcEventHandlerObject(const std::string &name, MemState &address_space, std::array<Address, 6> callbacks, uint32_t argument)
        : WithUidClass(name.c_str())
        , address_space(&address_space)
        , callbacks(std::move(callbacks))
        , argument(argument) {}

    const MemState *const address_space;
    const std::array<Address, 6> callbacks;
    const uint32_t argument;
};

SceInt32 invoke_proc_event_handlers(EmuEnvState &emuenv, const ThreadStatePtr &caller,
    uint32_t param1, int event_type, uint32_t param3, uint32_t param4,
    Ptr<SceUID> failed_handler = {}, SceUID stop_at_handler = 0);

void schedule_proc_event_create_callback(EmuEnvState &emuenv, const ThreadStatePtr &main_thread, SceUID pid);
