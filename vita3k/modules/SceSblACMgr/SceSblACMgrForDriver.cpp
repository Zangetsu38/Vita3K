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

#include "SceSblACMgrForDriver.h"

#include "SceSblACMgrForKernel.h"

#include <cpu/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <util/tracy.h>

TRACY_MODULE_NAME(SceSblACMgrForDriver);

EXPORT(int, SceSblACMgrForDriver_6C5AB07F, SceUID pid) {
    TRACY_FUNC(SceSblACMgrForDriver_6C5AB07F, pid);
    return CALL_EXPORT(sceSblACMgrHasCapability, pid, 3);
}

EXPORT(int, SceSblACMgrForDriver_sceSblACMgrHasCapability, SceUID pid, int bit) {
    TRACY_FUNC(SceSblACMgrForDriver_sceSblACMgrHasCapability, pid, bit);
    return CALL_EXPORT(sceSblACMgrHasCapability, pid, bit);
}

EXPORT(int, SceSblACMgrForDriver_0139FC20) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_GetFSAttributeByThreadAccessLevel) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_062CAEB2) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_091F7247) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_0924896F) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_IsKernelProgram) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_0B6E6CD7) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_IsGameProgram) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_2A29453C) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_2E992B02) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_3B356B98) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_426A4E8C) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_GetMediaType) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_456DA7AC) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_SetFSAttributeByAttributeWithMountPoint) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_4CBD6156) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_4DB7F512) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_5C4BC352) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_5F9AF49C) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_2AE6CF27) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_6D8A88B7) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_84604EED) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_IsSystemProgram, SceUID pid) {
    TRACY_FUNC(SceSblACMgrForDriver_IsSystemProgram, pid);
    const int has_system_capability = CALL_EXPORT(sceSblACMgrHasCapability, pid, 1);
    if (has_system_capability == 1 || pid != 0)
        return has_system_capability;

    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return 0;
    const auto identity = emuenv.kernel.get_process_identity(thread->cpu->mem);
    return identity && identity->is_firmware_app() ? 1 : 0;
}

EXPORT(int, SceSblACMgrForDriver_8A54DF3A) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_962CD237) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_GetPaid) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_991FDC15) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_A27E47A7) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_A67E8E5B) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_A92CD636) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_AD717E7A) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_AE1AF154) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_B12CEAA8) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_B836CF13) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_BE5667C5) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_C98D82EE) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_D0E11C89) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_D7AD8471) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_E79C7A8D) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_E87D1777) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_F5AE24AC) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_FD00C72A) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceSblACMgrForDriver_FF7125DE) {
    return UNIMPLEMENTED();
}
