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
#include "SceThreadmgr.h"
#include "lightweight.h"
#include <module/module.h>
#include <util/tracy.h>

TRACY_MODULE_NAME(SceThreadmgrForDriver);

#include <kernel/state.h>
#include <kernel/sync_primitives.h>

#include <memory>
#include <string>

EXPORT(int, ksceKernelCancelCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCancelMsgPipe) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCancelMutex) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelChangeCurrentThreadAttr) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelChangeThreadCpuAffinityMask) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelChangeThreadPriority) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelChangeThreadSuspendStatus) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelClearEvent) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelClearEventFlag) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCreateCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCreateCond) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCreateEventFlag) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCreateMsgPipe) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCreateMutex, Ptr<const char> name_ptr, SceUInt attr, int init_count, Ptr<const SceKernelMutexOptParam> opt_param) {
    TRACY_FUNC(ksceKernelCreateMutex, name_ptr, attr, init_count, opt_param);
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    MemState &mem = caller->cpu->mem;
    const bool secondary = !emuenv.kernel.is_primary_address_space(mem);
    std::string copied_name;
    const char *name = nullptr;
    SceKernelMutexOptParam copied_options{};
    const SceKernelMutexOptParam *options = nullptr;
    if (secondary) {
        if (!threadmgr::copy_lw_mutex_name(mem, name_ptr, copied_name))
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        name = copied_name.c_str();
        if (opt_param) {
            if (!threadmgr::copy_sync_guest_bytes(mem, opt_param.address(), &copied_options, sizeof(copied_options), false))
                return RET_ERROR(SCE_KERNEL_ERROR_INVALID_MEMORY_ACCESS);
            options = &copied_options;
        }
    } else {
        name = name_ptr.get(mem);
        options = opt_param.get(mem);
    }
    // Kernel modules use attributes games can't, like 0x8000 for SceSysmodule's mutex
    if (const SceInt32 error = Mutex::check_create(name, attr, init_count, options, true))
        return RET_ERROR(error);

    // The ceilings of kernel mutexes aren't applied
    const auto mutex = std::make_shared<HeavyMutex>(attr, name, init_count, caller, SCE_KERNEL_LOWEST_PRIORITY_KERNEL, secondary ? &mem : nullptr);
    const SceUID uid = emuenv.kernel.objects.add(mutex, emuenv.kernel.get_next_uid());
    LOG_DEBUG("Created driver mutex '{}' with UID {} for thread {}", name, log_hex(uid), thread_id);
    return uid;
}

EXPORT(int, ksceKernelCreateSema) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCreateSimpleEvent) {
    return UNIMPLEMENTED();
}

EXPORT(SceUID, ksceKernelCreateThread, const char *name, SceKernelThreadEntry entry, int initPriority, SceSize stackSize, SceUInt attr, int cpuAffinityMask, void *pOptParam) {
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const ThreadStatePtr thread = emuenv.kernel.create_thread_from_caller(caller, name, entry.cast<void>(), initPriority, cpuAffinityMask, stackSize, 0);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    return thread->id;
}

EXPORT(int, ksceKernelDeleteCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteCond) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteEventFlag) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteFastMutex) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteMsgPipe) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteMutex) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteSema) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteThread) {
    return UNIMPLEMENTED();
}

EXPORT(SceUID, ksceKernelEnqueueWorkQueue, SceUID work_queue_id, Ptr<const char> name_ptr, Ptr<void> entry, Ptr<void> arg) {
    TRACY_FUNC(ksceKernelEnqueueWorkQueue, work_queue_id, name_ptr, entry, arg);
    constexpr SceUID global_work_queue_id = 0x10023;
    if (work_queue_id != global_work_queue_id)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_WORK_QUEUE_ID);

    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const char *name = name_ptr.get(caller->cpu->mem);
    if (!name || !entry || !entry.get(caller->cpu->mem))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);

    const std::string copied_name(name);
    const ThreadStatePtr task_thread = emuenv.kernel.create_thread_from_caller(caller, copied_name.c_str(), entry.cast<void>(), 0xBF,
        SCE_KERNEL_CPU_MASK_SYSTEM, 0x2000, 0);
    if (!task_thread)
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);

    const std::weak_ptr<ThreadState> weak_task_thread = task_thread;
    task_thread->set_startup_callback([weak_task_thread, entry, arg] {
        const ThreadStatePtr task_thread = weak_task_thread.lock();
        if (!task_thread)
            return;
        const uint32_t result = task_thread->call_guest(entry.address(), { arg.address() });
        task_thread->exit_delete(static_cast<SceInt32>(result));
    });
    const SceInt32 start_result = task_thread->start(0, Ptr<void>{}, true);
    if (start_result != SCE_KERNEL_OK) {
        task_thread->remove();
        return RET_ERROR(start_result);
    }
    return task_thread->id;
}

EXPORT(int, ksceKernelGetCallbackCount) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetMutexInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetProcessIdFromTLS) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetSystemTimeLow) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetTLSAddr) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadCpuAffinityMask) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadCpuRegisters) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadCurrentPriority) {
    TRACY_FUNC(ksceKernelGetThreadCurrentPriority);
    return CALL_EXPORT(sceKernelGetThreadCurrentPriority);
}

EXPORT(int, ksceKernelGetThreadId) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadIdList) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadStackFreeSize) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadTLSAddr) {
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, ksceKernelGetThreadmgrUIDClass, SceUID uid) {
    TRACY_FUNC(ksceKernelGetThreadmgrUIDClass, uid);
    return CALL_EXPORT(sceKernelGetThreadmgrUIDClass, uid);
}

EXPORT(int, ksceKernelGetTimerBaseWide) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetTimerTimeWide) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelInitializeFastMutex) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelLockFastMutex) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelLockMutex, SceUID mutexid, int lock_count, unsigned int *timeout) {
    TRACY_FUNC(ksceKernelLockMutex, mutexid, lock_count, timeout);
    return CALL_EXPORT(_sceKernelLockMutex, mutexid, lock_count, timeout);
}

EXPORT(int, ksceKernelLockMutexCB_089) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelNotifyCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelPollEventFlag) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelPollSema) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelPulseEvent) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelPulseEventWithNotifyCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelReceiveMsgPipeVector) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelReceiveMsgPipeVectorCB) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelRegisterCallbackToEvent) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelRegisterTimer) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelRunWithStack) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSendMsgPipeVector) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSetEvent) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSetEventFlag) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSetPermission) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSetProcessId) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSetTimerTimeWide) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSignalCond) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSignalCondAll) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSignalCondTo) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSignalSema) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelStartThread, SceUID thid, SceSize arglen, Ptr<void> argp) {
    return CALL_EXPORT(_sceKernelStartThread, thid, arglen, argp);
}

EXPORT(int, ksceKernelStartTimer) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelStopTimer) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelTryLockMutex) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelTryLockReadRWLock) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelTryLockWriteRWLock) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelTryReceiveMsgPipeVector) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelTrySendMsgPipeVector) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelUnlockFastMutex) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelUnlockMutex, SceUID mutexid, int unlock_count) {
    TRACY_FUNC(ksceKernelUnlockMutex, mutexid, unlock_count);
    return CALL_EXPORT(sceKernelUnlockMutex, mutexid, unlock_count);
}

EXPORT(int, ksceKernelUnlockReadRWLock) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelUnlockWriteRWLock) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelUnregisterCallbackFromEvent) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelUnregisterCallbackFromEventAll) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitCond) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitEvent) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitEventCB) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitEventFlag) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitEventFlagCB) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitSema) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitThreadEnd, SceUID thid, Ptr<SceInt32> stat, Ptr<SceUInt32> timeout) {
    return CALL_EXPORT(_sceKernelWaitThreadEnd, thid, stat, timeout);
}

EXPORT(int, ksceKernelWaitThreadEndCB) {
    return UNIMPLEMENTED();
}

EXPORT(int, SceThreadmgrForDriver_20C228E4) {
    return UNIMPLEMENTED();
}
