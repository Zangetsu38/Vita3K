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

#include <module/module.h>

#include "SceGxm.h"

#include <cpu/state.h>
#include <gxm/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/state.h>
#include <mem/util.h>
#include <renderer/functions.h>
#include <renderer/gxm_types.h>
#include <renderer/state.h>

#include <span>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceGxmInternalForVsh);

EXPORT(int, sceGxmVshInitialize, SceGxmInitializeParams *params) {
    TRACY_FUNC(sceGxmVshInitialize, params);

    if (!params)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_POINTER);

    const uint32_t vsh_flags = params->flags;
    if ((vsh_flags & ~0x1FU) || ((vsh_flags & 0xCU) == 0xCU))
        return RET_ERROR(SCE_GXM_ERROR_INVALID_VALUE);

    params->flags = 0;
    const int result = CALL_EXPORT(sceGxmInitialize, params);
    params->flags = vsh_flags;
    if (result != 0)
        return result;

    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_THREAD);
    MemState &mem = caller->cpu->mem;
    if (emuenv.kernel.is_primary_address_space(mem)) {
        emuenv.gxm.vsh_init_flags = vsh_flags;
    } else {
        const std::lock_guard lock(emuenv.gxm.process_states_mutex);
        const auto process = emuenv.gxm.process_states.find(&mem);
        if (emuenv.gxm.process_states_shutdown || process == emuenv.gxm.process_states.end())
            return RET_ERROR(SCE_GXM_ERROR_UNINITIALIZED);
        process->second->vsh_init_flags = vsh_flags;
    }
    return 0;
}

static GxmState *get_gxm_state(EmuEnvState &emuenv, const ThreadStatePtr &caller, std::shared_ptr<GxmState> &process_state) {
    MemState &mem = caller->cpu->mem;
    if (emuenv.kernel.is_primary_address_space(mem))
        return &emuenv.gxm;

    const std::lock_guard lock(emuenv.gxm.process_states_mutex);
    const auto process = emuenv.gxm.process_states.find(&mem);
    if (emuenv.gxm.process_states_shutdown || process == emuenv.gxm.process_states.end())
        return nullptr;
    process_state = process->second;
    return process_state.get();
}

static int check_vsh_sync_initialized(const GxmState *gxm, bool require_shared_sync, const char *export_name) {
    if (!gxm || !gxm->display_queue_thread)
        return RET_ERROR(SCE_GXM_ERROR_UNINITIALIZED);
    if (require_shared_sync && !(gxm->vsh_init_flags & 0x2))
        return RET_ERROR(SCE_GXM_ERROR_UNINITIALIZED);
    return 0;
}

EXPORT(int, sceGxmVshSyncObjectClose, uint32_t key, Ptr<SceGxmSyncObject> syncObject) {
    TRACY_FUNC(sceGxmVshSyncObjectClose, key, syncObject);
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_THREAD);
    const ThreadHostWait host_wait(caller);

    std::shared_ptr<GxmState> process_state;
    GxmState *gxm = get_gxm_state(emuenv, caller, process_state);
    if (const int result = check_vsh_sync_initialized(gxm, false, export_name); result != 0)
        return result;
    if (!syncObject)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_POINTER);
    if (key == 0)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_VALUE);

    MemState &mem = caller->cpu->mem;
    SceGxmSyncObject *sync = syncObject.get(mem);
    {
        const std::lock_guard lock(gxm->sync_objects_mutex);
        if (!gxm->sync_objects.contains(sync))
            return RET_ERROR(SCE_GXM_ERROR_INVALID_POINTER);
        if (gxm->display_sync_refs.contains(sync))
            return RET_ERROR(SCE_GXM_ERROR_DRIVER);
        if (sce_gxm_sync_object_is_within_scene(*gxm, syncObject))
            return RET_ERROR(SCE_GXM_ERROR_WITHIN_SCENE);
        if (sync->state().timestamp_current.load() != sync->state().timestamp_ahead.load())
            return RET_ERROR(SCE_GXM_ERROR_DRIVER);
        const auto opened = gxm->vsh_opened_sync_objects.find(sync);
        if (opened == gxm->vsh_opened_sync_objects.end() || opened->second != key)
            return RET_ERROR(SCE_GXM_ERROR_DRIVER);
        gxm->sync_objects.erase(sync);
        gxm->vsh_opened_sync_objects.erase(sync);
    }
    renderer::destroy(sync, *emuenv.renderer);
    free(mem, syncObject);
    return 0;
}

EXPORT(int, sceGxmVshSyncObjectCreate, uint32_t key, Ptr<SceGxmSyncObject> syncObject) {
    TRACY_FUNC(sceGxmVshSyncObjectCreate, key, syncObject);
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_THREAD);
    const ThreadHostWait host_wait(caller);

    std::shared_ptr<GxmState> process_state;
    GxmState *gxm = get_gxm_state(emuenv, caller, process_state);
    if (const int result = check_vsh_sync_initialized(gxm, true, export_name); result != 0)
        return result;
    if (!syncObject)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_POINTER);
    if (key == 0)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_VALUE);

    SceGxmSyncObject *sync = syncObject.get(caller->cpu->mem);
    const std::lock_guard shared_lock(emuenv.gxm.vsh_sync_objects_mutex);
    if (emuenv.gxm.vsh_sync_objects.contains(key))
        return RET_ERROR(SCE_GXM_ERROR_DRIVER);
    {
        const std::lock_guard sync_lock(gxm->sync_objects_mutex);
        if (!gxm->sync_objects.contains(sync))
            return RET_ERROR(SCE_GXM_ERROR_INVALID_POINTER);
    }

    std::shared_ptr<SceGxmSyncObjectState> shared_state = sync->shared_state;
    if (!shared_state) {
        shared_state = std::make_shared<SceGxmSyncObjectState>();
        SceGxmSyncObjectState &local_state = sync->local_state;
        {
            const std::lock_guard state_lock(local_state.lock);
            shared_state->timestamp_current = local_state.timestamp_current.load();
            shared_state->timestamp_ahead = local_state.timestamp_ahead.load();
            shared_state->last_display = local_state.last_display.load();
            shared_state->last_operation_global = local_state.last_operation_global;
            shared_state->being_deleted = local_state.being_deleted;
        }
        sync->shared_state = shared_state;
    }
    emuenv.gxm.vsh_sync_objects.emplace(key, std::move(shared_state));
    return 0;
}

EXPORT(int, sceGxmVshSyncObjectDestroy, uint32_t key) {
    TRACY_FUNC(sceGxmVshSyncObjectDestroy, key);
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_THREAD);
    const ThreadHostWait host_wait(caller);

    std::shared_ptr<GxmState> process_state;
    GxmState *gxm = get_gxm_state(emuenv, caller, process_state);
    if (const int result = check_vsh_sync_initialized(gxm, false, export_name); result != 0)
        return result;
    if (key == 0)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_VALUE);

    const std::lock_guard lock(emuenv.gxm.vsh_sync_objects_mutex);
    if (!emuenv.gxm.vsh_sync_objects.erase(key))
        return RET_ERROR(SCE_GXM_ERROR_DRIVER);
    return 0;
}

EXPORT(int, sceGxmVshSyncObjectOpen, uint32_t key, Ptr<SceGxmSyncObject> *syncObject) {
    TRACY_FUNC(sceGxmVshSyncObjectOpen, key, syncObject);
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_THREAD);
    const ThreadHostWait host_wait(caller);

    std::shared_ptr<GxmState> process_state;
    GxmState *gxm = get_gxm_state(emuenv, caller, process_state);
    if (const int result = check_vsh_sync_initialized(gxm, true, export_name); result != 0)
        return result;
    if (!syncObject)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_POINTER);
    if (key == 0)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_VALUE);

    std::shared_ptr<SceGxmSyncObjectState> shared_state;
    {
        const std::lock_guard lock(emuenv.gxm.vsh_sync_objects_mutex);
        const auto shared = emuenv.gxm.vsh_sync_objects.find(key);
        if (shared == emuenv.gxm.vsh_sync_objects.end())
            return RET_ERROR(SCE_GXM_ERROR_DRIVER);
        shared_state = shared->second;
    }

    MemState &mem = caller->cpu->mem;
    const Address output_address = caller->cpu->get_reg(1);
    const GuestMemoryRange output{ output_address, sizeof(Address) };
    const uint64_t pin = pin_guest_memory(mem, std::span(&output, 1));
    if (!pin)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_POINTER);
    const Ptr<SceGxmSyncObject> opened = alloc<SceGxmSyncObject>(mem, "sceGxmVshSyncObjectOpen");
    if (!opened) {
        unpin_guest_memory(mem, pin);
        return RET_ERROR(SCE_GXM_ERROR_OUT_OF_MEMORY);
    }
    renderer::create(opened.get(mem), *emuenv.renderer);
    bool valid = true;
    {
        const std::lock_guard protect_lock(mem.protect_mutex);
        const uint64_t end = uint64_t(output.address) + output.size;
        for (const auto &[start, segment] : mem.protect_tree) {
            if ((segment.perm == MemPerm::None || segment.perm == MemPerm::ReadOnly) && uint64_t(start) < end && uint64_t(start) + segment.size > output.address)
                valid = false;
        }
        if (valid) {
            const Address address = opened.address();
            for (size_t i = 0; i < sizeof(address); ++i)
                *Ptr<uint8_t>(output.address + static_cast<Address>(i)).get(mem) = static_cast<uint8_t>(address >> (8 * i));
        }
    }
    unpin_guest_memory(mem, pin);
    if (!valid) {
        renderer::destroy(opened.get(mem), *emuenv.renderer);
        free(mem, opened);
        return RET_ERROR(SCE_GXM_ERROR_INVALID_POINTER);
    }

    SceGxmSyncObject *sync = opened.get(mem);
    sync->shared_state = std::move(shared_state);
    {
        const std::lock_guard lock(gxm->sync_objects_mutex);
        if (!gxm->sync_objects.emplace(sync).second) {
            renderer::destroy(sync, *emuenv.renderer);
            free(mem, opened);
            return RET_ERROR(SCE_GXM_ERROR_DRIVER);
        }
        gxm->vsh_opened_sync_objects.emplace(sync, key);
    }
    return 0;
}
