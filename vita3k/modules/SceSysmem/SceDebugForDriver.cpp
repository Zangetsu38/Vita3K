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

#include "../SceLibKernel/SceLibKernel.h"
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <module/guest_printf.h>
#include <module/module.h>
#include <util/tracy.h>

TRACY_MODULE_NAME(SceDebugForDriver)

EXPORT(int, ksceDebugPrintKernelAssertion) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceDebugPrintKernelPanic) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceDebugPrintf, Ptr<const char> fmt, module::vargs args) {
    TRACY_FUNC(ksceDebugPrintf, fmt);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    MemState &mem = thread->cpu->mem;
    if (emuenv.kernel.is_primary_address_space(mem))
        return CALL_EXPORT(sceClibPrintf, fmt, args);

    args.enable_checked_reads();
    auto read_arg = [&]<typename T>(T &value) {
        value = args.next<T>(*thread->cpu, mem);
        return args.valid();
    };
    std::string output;
    const auto error = module::format_guest_printf(mem, fmt.address(), read_arg, output);
    if (error != module::GuestPrintfError::none || output.empty()) {
        const char *reason = "empty output";
        switch (error) {
        case module::GuestPrintfError::memory: reason = "invalid or unreadable guest memory"; break;
        case module::GuestPrintfError::unsupported: reason = "unsupported format"; break;
        case module::GuestPrintfError::limit: reason = "formatting limit exceeded"; break;
        case module::GuestPrintfError::formatting: reason = "host formatting failure"; break;
        case module::GuestPrintfError::none: break;
        }
        LOG_ERROR("ksceDebugPrintf: secondary formatting failed ({}, format {})", reason, log_hex(fmt.address()));
        return error == module::GuestPrintfError::memory ? SCE_KERNEL_ERROR_ILLEGAL_ADDR : SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    }
    LOG_INFO("{}", output);
    return SCE_KERNEL_OK;
}

EXPORT(int, ksceDebugPrintf2) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceDebugPrintfKernelAssertion) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceDebugPrintfKernelPanic) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceEventLogGetInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetMinimumAssertionLevel) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetTtyInfo) {
    return UNIMPLEMENTED();
}
