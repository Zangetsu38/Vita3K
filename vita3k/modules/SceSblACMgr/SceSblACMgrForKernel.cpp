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

#include "SceSblACMgrForKernel.h"

#include <kernel/state.h>
#include <util/tracy.h>

TRACY_MODULE_NAME(SceSblACMgrForKernel);

EXPORT(int, sceSblACMgrHasCapability, SceUID pid, int bit) {
    TRACY_FUNC(sceSblACMgrHasCapability, pid, bit);
    if (bit < 0 || bit >= 0x100)
        return 0;
    if (pid == 0) {
        const auto current_pid = emuenv.kernel.get_thread_process_id(thread_id);
        if (!current_pid)
            return 0;
        pid = *current_pid;
    }
    const auto auth_info = emuenv.kernel.get_process_self_auth_info(pid);
    if (!auth_info)
        return 0;
    return (auth_info->capability.bytes[bit >> 3] >> (~bit & 7)) & 1;
}

EXPORT(int, SceSblACMgrForKernel_6C5AB07F, SceUID pid) {
    TRACY_FUNC(SceSblACMgrForKernel_6C5AB07F, pid);
    return CALL_EXPORT(sceSblACMgrHasCapability, pid, 3);
}

EXPORT(int, SceSblACMgrForKernel_A27E47A7) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_F5AE24AC) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_IsGameProgram) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_5C4BC352) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_AD717E7A) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_3B356B98) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_E79C7A8D) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_1948E9DB) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_SetFSAttributeByAttributeWithMountPoint) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_2AE6CF27) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_30575458) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_IsKernelProgram) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_A67E8E5B) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_FF7125DE) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_4DB7F512) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_AE1AF154) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_FD00C72A) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_84604EED) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_4CBD6156) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_456DA7AC) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_8A54DF3A) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_426A4E8C) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_A92CD636) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_6D8A88B7) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_BE5667C5) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_GetPaid) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_5F9AF49C) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_8241AB5C) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_IsSystemProgram) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_2A29453C) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_091F7247) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_962CD237) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_B836CF13) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_0139FC20) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_D0E11C89) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_E87D1777) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_C98D82EE) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_B12CEAA8) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_GetMediaType) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_062CAEB2) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_0924896F) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_0B6E6CD7) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_991FDC15) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_D7AD8471) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForKernel_FBA1A256) {
    return UNIMPLEMENTED();
}
