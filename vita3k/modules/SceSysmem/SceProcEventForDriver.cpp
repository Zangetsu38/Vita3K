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

#include <cpu/state.h>
#include <emuenv/state.h>
#include <kernel/kernel_object.h>
#include <kernel/proc_event_handler.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <module/module.h>
#include <util/tracy.h>

#include <array>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

TRACY_MODULE_NAME(SceProcEventForDriver);

struct SceProcEventInvokeParam1 {
    SceSize size; // SceProcEventInvokeParam1 struct size : 0x10
    int unk_0x04;
    int unk_0x08;
    int unk_0x0C;
};
static_assert(sizeof(SceProcEventInvokeParam1) == 0x10, "Size of SceProcEventInvokeParam1 should be 0x10");

struct SceProcEventInvokeParam2 {
    SceSize size; // SceProcEventInvokeParam2 struct size : 0x14
    SceUID pid;
    int unk_0x08;
    int unk_0x0C;
    int unk_0x10;
};
static_assert(sizeof(SceProcEventInvokeParam2) == 0x14, "Size of SceProcEventInvokeParam2 should be 0x14");

struct SceProcEventHandler {
    SceSize size; // SceProcEventHandler struct size : 0x1C
    Ptr<void> create; // int (* create)(SceUID pid, SceProcEventInvokeParam2 *a2, int a3);
    Ptr<void> exit; // int (* exit)(SceUID pid, SceProcEventInvokeParam1 *a2, int a3); // current process exit
    Ptr<void> kill; // int (* kill)(SceUID pid, SceProcEventInvokeParam1 *a2, int a3); // by SceShell
    Ptr<void> stop; // int (* stop)(SceUID pid, int event_type, SceProcEventInvokeParam1 *a3, int a4);
    Ptr<void> start; // int (* start)(SceUID pid, int event_type, SceProcEventInvokeParam1 *a3, int a4);
    Ptr<void> switch_process; // int (* switch_process)(int event_id, int event_type, SceProcEventInvokeParam2 *a3, int a4); // switch display frame?
};
static_assert(sizeof(SceProcEventHandler) == 0x1C, "Size of SceProcEventHandler should be 0x1C");

namespace {

struct ProcEventHandlerSnapshot {
    SceUID uid;
    std::array<Address, 6> callbacks;
    uint32_t argument;
};

bool copy_guest_name(MemState &mem, Ptr<const char> name, std::string &copied_name) {
    if (!name)
        return false;

    copied_name.reserve(SCE_UID_NAMELEN);
    for (size_t i = 0; i < SCE_UID_NAMELEN; ++i) {
        const uint64_t address = static_cast<uint64_t>(name.address()) + i;
        if (address > std::numeric_limits<Address>::max())
            return false;
        const GuestMemoryRange range{ static_cast<Address>(address), 1 };
        const uint64_t pin = pin_guest_memory(mem, std::span(&range, 1));
        if (!pin)
            return false;
        const char ch = *Ptr<const char>(range.address).get(mem);
        unpin_guest_memory(mem, pin);
        if (!ch)
            return true;
        copied_name.push_back(ch);
    }
    return true;
}

bool copy_guest_handler(MemState &mem, Ptr<const SceProcEventHandler> handler, SceProcEventHandler &callbacks) {
    constexpr size_t base_size = sizeof(SceSize) + 5 * sizeof(Ptr<void>);
    if (!handler)
        return false;

    const GuestMemoryRange base_range{ handler.address(), base_size };
    const uint64_t base_pin = pin_guest_memory(mem, std::span(&base_range, 1));
    if (!base_pin)
        return false;
    auto *bytes = reinterpret_cast<uint8_t *>(&callbacks);
    for (size_t i = 0; i < base_size; ++i)
        bytes[i] = *Ptr<const uint8_t>(handler.address() + static_cast<Address>(i)).get(mem);
    unpin_guest_memory(mem, base_pin);

    if (callbacks.size == sizeof(SceProcEventHandler)) {
        const uint64_t switch_address = static_cast<uint64_t>(handler.address()) + base_size;
        if (switch_address > std::numeric_limits<Address>::max())
            return false;
        const GuestMemoryRange switch_range{ static_cast<Address>(switch_address), sizeof(Ptr<void>) };
        const uint64_t switch_pin = pin_guest_memory(mem, std::span(&switch_range, 1));
        if (!switch_pin)
            return false;
        for (size_t i = 0; i < sizeof(Ptr<void>); ++i)
            bytes[base_size + i] = *Ptr<const uint8_t>(switch_range.address + static_cast<Address>(i)).get(mem);
        unpin_guest_memory(mem, switch_pin);
    }
    return true;
}

SceInt32 write_failed_handler(MemState &mem, Ptr<SceUID> failed_handler, SceUID uid) {
    const GuestMemoryRange range{ failed_handler.address(), sizeof(SceUID) };
    const uint64_t pin = pin_guest_memory(mem, std::span(&range, 1));
    if (!pin)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    const auto value = static_cast<uint32_t>(uid);
    for (size_t i = 0; i < sizeof(value); ++i)
        *Ptr<uint8_t>(failed_handler.address() + static_cast<Address>(i)).get(mem) = static_cast<uint8_t>(value >> (i * 8));
    unpin_guest_memory(mem, pin);
    return SCE_KERNEL_OK;
}

} // namespace

SceInt32 invoke_proc_event_handlers(EmuEnvState &emuenv, const ThreadStatePtr &caller,
    uint32_t param1, int event_type, uint32_t param3, uint32_t param4,
    Ptr<SceUID> failed_handler, SceUID stop_at_handler) {
    if (!caller)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    MemState &mem = caller->cpu->mem;

    std::vector<ProcEventHandlerSnapshot> handlers;
    emuenv.kernel.objects.for_each<ProcEventHandlerObject>([&](const ProcEventHandlerObject &handler) {
        if (handler.address_space != &mem)
            return;
        handlers.push_back({ handler.uid, handler.callbacks, handler.argument });
    });

    SceInt32 result = SCE_KERNEL_OK;
    for (const auto &handler : handlers) {
        if (handler.uid == stop_at_handler)
            break;

        size_t callback_index;
        switch (event_type) {
        case 1:
            callback_index = 0;
            break;
        case 2:
            callback_index = 1;
            break;
        case 3:
            callback_index = 2;
            break;
        case 4:
            callback_index = 3;
            break;
        case 5:
            callback_index = 4;
            break;
        default:
            callback_index = 5;
            break;
        }

        const Address callback = handler.callbacks[callback_index];
        if (!callback)
            continue;
        LOG_DEBUG("Invoking process event {} callback for handler {}", event_type, handler.uid);
        if (!is_valid_addr(mem, callback & ~Address{ 1 })) {
            LOG_ERROR("ksceKernelInvokeProcEventHandler: callback address {} for handler {} is not valid in the caller address space", log_hex(callback), handler.uid);
            result = SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        } else if (event_type >= 1 && event_type <= 3) {
            result = static_cast<SceInt32>(caller->call_guest(callback, { param1, param4, handler.argument }));
        } else if (event_type == 4 || event_type == 5) {
            result = static_cast<SceInt32>(caller->call_guest(callback, { param1, param3, param4, handler.argument }));
        } else {
            result = static_cast<SceInt32>(caller->call_guest(callback, { static_cast<uint32_t>(event_type), param3, param4, handler.argument }));
        }
        LOG_DEBUG("Process event handler {} returned {}", handler.uid, log_hex(static_cast<uint32_t>(result)));

        if (result < 0) {
            if (failed_handler) {
                const SceInt32 write_result = write_failed_handler(mem, failed_handler, handler.uid);
                if (write_result < 0)
                    return write_result;
            }
            break;
        }
    }
    return result;
}

void schedule_proc_event_create_callback(EmuEnvState &emuenv, const ThreadStatePtr &main_thread, SceUID pid) {
    if (!main_thread) {
        LOG_ERROR("Cannot schedule process-create callbacks for PID {} without its main thread", pid);
        return;
    }
    const SceUID thread_id = main_thread->id;
    main_thread->set_startup_callback([&emuenv, pid, thread_id] {
        const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
        if (!caller) {
            LOG_ERROR("Cannot dispatch process-create callbacks for PID {} without its main thread", pid);
            return;
        }
        LOG_DEBUG("Dispatching process-create callbacks for PID {}", pid);
        const SceInt32 result = invoke_proc_event_handlers(emuenv, caller,
            static_cast<uint32_t>(pid), 1, 0, 0);
        if (result < 0)
            LOG_ERROR("Process-create callbacks for PID {} failed: {}", pid, log_hex(result));
    });
}

EXPORT(int, ksceKernelRegisterProcEventHandler, Ptr<const char> name, Ptr<const SceProcEventHandler> handler, Ptr<void> a3) {
    TRACY_FUNC(ksceKernelRegisterProcEventHandler, name, handler, a3);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    MemState &mem = caller->cpu->mem;

    std::string copied_name;
    if (!copy_guest_name(mem, name, copied_name))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    SceProcEventHandler callbacks{};
    if (!copy_guest_handler(mem, handler, callbacks))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);

    auto registered = std::make_shared<ProcEventHandlerObject>(copied_name, mem,
        std::array<Address, 6>{
            callbacks.create.address(),
            callbacks.exit.address(),
            callbacks.kill.address(),
            callbacks.stop.address(),
            callbacks.start.address(),
            callbacks.switch_process.address(),
        },
        a3.address());
    return emuenv.kernel.objects.add(std::move(registered), emuenv.kernel.get_next_uid());
}

EXPORT(int, ksceKernelUnregisterProcEventHandler, SceUID handler_uid, int unknown) {
    TRACY_FUNC(ksceKernelUnregisterProcEventHandler, handler_uid, unknown);
    if (!emuenv.kernel.objects.remove<ProcEventHandlerObject>(handler_uid, thread_id))
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_UID);
    return SCE_KERNEL_OK;
}

EXPORT(int, ksceKernelInvokeProcEventHandler, uint32_t param1, int event_type, uint32_t param3, uint32_t param4, Ptr<SceUID> failed_handler, SceUID stop_at_handler) {
    TRACY_FUNC(ksceKernelInvokeProcEventHandler, param1, event_type, param3, param4, failed_handler, stop_at_handler);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    return invoke_proc_event_handlers(emuenv, caller, param1, event_type, param3, param4, failed_handler, stop_at_handler);
}
