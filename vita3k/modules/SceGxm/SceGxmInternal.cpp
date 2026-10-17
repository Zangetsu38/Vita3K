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

#include <util/tracy.h>

TRACY_MODULE_NAME(SceGxmInternal);

EXPORT(int, sceGxmCheckMemoryInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceGxmCreateRenderTargetInternal, const SceGxmRenderTargetParams *params, Ptr<SceGxmRenderTarget> *renderTarget) {
    TRACY_FUNC(sceGxmCreateRenderTargetInternal, params, renderTarget);
    return sce_gxm_create_render_target(emuenv, thread_id, params, renderTarget, true, "sceGxmCreateRenderTargetInternal");
}

EXPORT(SceUID, sceGxmGetDisplayQueueThreadIdInternal) {
    TRACY_FUNC(sceGxmGetDisplayQueueThreadIdInternal);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    MemState &mem = caller->cpu->mem;
    if (emuenv.kernel.is_primary_address_space(mem)) {
        if (!emuenv.gxm.display_queue_thread)
            return RET_ERROR(SCE_GXM_ERROR_UNINITIALIZED);
        return emuenv.gxm.display_queue_thread;
    }

    const std::lock_guard lock(emuenv.gxm.process_states_mutex);
    const auto process = emuenv.gxm.process_states.find(&mem);
    if (emuenv.gxm.process_states_shutdown || process == emuenv.gxm.process_states.end()
        || !process->second->display_queue_thread)
        return RET_ERROR(SCE_GXM_ERROR_UNINITIALIZED);
    return process->second->display_queue_thread;
}

EXPORT(int, sceGxmGetRenderTargetMemSizeInternal, Ptr<const SceGxmRenderTargetParams> params, Ptr<uint32_t> hostMemSize) {
    TRACY_FUNC(sceGxmGetRenderTargetMemSizeInternal, params, hostMemSize);
    return CALL_EXPORT(sceGxmGetRenderTargetMemSize, params, hostMemSize);
}

EXPORT(int, sceGxmGetTopContextInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceGxmInitializedInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceGxmIsInitializationInternal) {
    TRACY_FUNC(sceGxmIsInitializationInternal);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    MemState &mem = caller->cpu->mem;
    if (emuenv.kernel.is_primary_address_space(mem)) {
        const std::lock_guard lock(emuenv.gxm.display_queue_add_mutex);
        return emuenv.gxm.display_queue_thread && !emuenv.gxm.display_queue_closed;
    }

    std::shared_ptr<GxmState> process_state;
    {
        const std::lock_guard lock(emuenv.gxm.process_states_mutex);
        const auto process = emuenv.gxm.process_states.find(&mem);
        if (emuenv.gxm.process_states_shutdown || process == emuenv.gxm.process_states.end())
            return 0;
        process_state = process->second;
    }

    const std::lock_guard lock(process_state->display_queue_add_mutex);
    return process_state->display_queue_thread && !process_state->display_queue_closed;
}

EXPORT(int, sceGxmMapFragmentUsseMemoryInternal, Ptr<void> base, uint32_t size, Ptr<uint32_t> offset) {
    TRACY_FUNC(sceGxmMapFragmentUsseMemoryInternal, base, size, offset);
    return CALL_EXPORT(sceGxmMapFragmentUsseMemory, base, size, offset);
}

EXPORT(int, sceGxmMapVertexUsseMemoryInternal, Ptr<void> base, uint32_t size, Ptr<uint32_t> offset) {
    TRACY_FUNC(sceGxmMapVertexUsseMemoryInternal, base, size, offset);
    return CALL_EXPORT(sceGxmMapVertexUsseMemory, base, size, offset);
}

EXPORT(int, sceGxmRenderingContextIsWithinSceneInternal) {
    TRACY_FUNC(sceGxmRenderingContextIsWithinSceneInternal);
    return sce_gxm_rendering_context_is_within_scene(emuenv, thread_id);
}

EXPORT(int, sceGxmSetCallbackInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceGxmSetInitializeParamInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceGxmUnmapFragmentUsseMemoryInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceGxmUnmapVertexUsseMemoryInternal) {
    return UNIMPLEMENTED();
}
