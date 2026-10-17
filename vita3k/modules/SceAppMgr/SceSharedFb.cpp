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

#include "../SceDisplay/SceDisplay.h"
#include <module/module.h>

#include "SceAppMgr.h"

#include <display/guest_frame.h>
#include <display/process_frames.h>
#include <display/state.h>
#include <gxm/types.h>

#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/state.h>
#include <renderer/state.h>

#include <util/tracy.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

TRACY_MODULE_NAME(SceSharedFB);

typedef struct SceSharedFbInfo { // size is 0x58
    Ptr<void> base1; // cdram base
    int memsize;
    Ptr<void> base2; // cdram base
    int unk_0C;
    Ptr<void> unk_10;
    int unk_14;
    int unk_18;
    int unk_1C;
    int unk_20;
    int pitch; // 960
    int width; // 960
    int height; // 544
    SceGxmColorFormat color_format; // SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR
    int curbuf;
    int unk_38;
    int unk_3C;
    int unk_40;
    int unk_44;
    int vsync;
    int unk_4C;
    int unk_50;
    int unk_54;
} SceSharedFbInfo;

struct SharedFbProcessState {
    std::mutex mutex;
    SceSharedFbInfo info{};
    std::atomic<bool> registered{ false };
    bool vsync = false;
};

struct SharedFbState {
    std::mutex mutex;
    std::unordered_map<const MemState *, std::shared_ptr<SharedFbProcessState>> processes;
};

static std::shared_ptr<SharedFbProcessState> get_shared_fb_process_state(EmuEnvState &emuenv, SceUID thread_id, bool create) {
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return {};
    auto *state = emuenv.kernel.obj_store.get<SharedFbState>();
    if (!state)
        return {};

    const MemState *mem = &caller->cpu->mem;
    const std::lock_guard lock(state->mutex);
    if (const auto it = state->processes.find(mem); it != state->processes.end())
        return it->second;
    if (!create)
        return {};
    auto process_state = std::make_shared<SharedFbProcessState>();
    state->processes.emplace(mem, process_state);
    return process_state;
}

void release_shared_fb_process_state(EmuEnvState &emuenv, const MemState &mem) {
    auto *state = emuenv.kernel.obj_store.get<SharedFbState>();
    if (!state)
        return;
    const std::lock_guard lock(state->mutex);
    state->processes.erase(&mem);
}

LIBRARY_INIT(SceSharedFb) {
    emuenv.kernel.obj_store.create<SharedFbState>();
}

DECL_EXPORT(int, sceSharedFbCreate, int smth);

EXPORT(int, _sceSharedFbOpen, int smth) {
    TRACY_FUNC(_sceSharedFbOpen, smth);
    STUBBED("sceSharedFbCreate");
    return CALL_EXPORT(sceSharedFbCreate, smth);
}

EXPORT(int, sceSharedFbBegin, int id, SceSharedFbInfo *info) {
    TRACY_FUNC(sceSharedFbBegin, id, info);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const bool primary = emuenv.kernel.is_primary_address_space(caller->cpu->mem);
    const auto state = get_shared_fb_process_state(emuenv, thread_id, primary);
    if (!state || (!primary && !state->registered))
        return RET_ERROR(SCE_KERNEL_ERROR_NOT_INITIALIZED);
    const std::lock_guard lock(state->mutex);
    state->vsync = info->vsync != 0;
    state->info.curbuf = 1;
    *info = state->info;
    return 0;
}

EXPORT(int, sceSharedFbClose) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSharedFbCreate, int smth) {
    TRACY_FUNC(sceSharedFbCreate, smth);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const auto state = get_shared_fb_process_state(emuenv, thread_id, true);
    if (!state)
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    const std::lock_guard lock(state->mutex);
    if (!state->registered) {
        constexpr uint32_t alloc_size = 4 * 1024 * 512 * 2;
        const Ptr<uint8_t> data(alloc(caller->cpu->mem, alloc_size, "sharedFB"));
        if (!data)
            return RET_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
        state->info = SceSharedFbInfo{
            .base1 = data,
            .memsize = alloc_size,
            .base2 = data + alloc_size / 2,
            .pitch = 960,
            .width = 960,
            .height = 544,
            .color_format = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
            .curbuf = 1,
        };
        state->registered = true;
    }
    return 1;
}

EXPORT(int, sceSharedFbDelete) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSharedFbEnd) {
    TRACY_FUNC(sceSharedFbEnd);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    MemState &mem = caller->cpu->mem;
    const bool primary = emuenv.kernel.is_primary_address_space(mem);
    const auto state = get_shared_fb_process_state(emuenv, thread_id, primary);
    if (!state || (!primary && !state->registered))
        return RET_ERROR(SCE_KERNEL_ERROR_NOT_INITIALIZED);
    SceSharedFbInfo info{};
    {
        const std::lock_guard lock(state->mutex);
        info = state->info;
    }
    if (!primary) {
        const auto pid = emuenv.kernel.get_thread_process_id(thread_id);
        if (!pid)
            return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
        DisplayFrameInfo frame{};
        frame.base = info.base2;
        frame.pitch = info.pitch;
        frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        frame.image_size = { info.width, info.height };
        frame.owner_pid = *pid;
        try {
            if (!display::capture_software_frame(mem, frame))
                return RET_ERROR(SCE_DISPLAY_ERROR_INVALID_ADDR);
            if (display::submit_process_frame(emuenv.display, *pid, std::move(frame)))
                emuenv.renderer->should_display = true;
        } catch (const std::bad_alloc &) {
            return RET_ERROR(SCE_DISPLAY_ERROR_NO_PIXEL_DATA);
        }
        return SCE_DISPLAY_ERROR_OK;
    }

    SceDisplayFrameBuf frame_buf{
        .size = sizeof(SceDisplayFrameBuf),
        .base = info.base2,
        .pitch = static_cast<uint32_t>(info.pitch),
        .width = static_cast<uint32_t>(info.width),
        .height = static_cast<uint32_t>(info.height)
    };
    return CALL_EXPORT(_sceDisplaySetFrameBuf, &frame_buf, SCE_DISPLAY_SETBUF_NEXTFRAME, nullptr);
}

EXPORT(int, sceSharedFbGetInfo, int id, SceSharedFbInfo *info) {
    TRACY_FUNC(sceSharedFbGetInfo);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const bool primary = emuenv.kernel.is_primary_address_space(caller->cpu->mem);
    const auto state = get_shared_fb_process_state(emuenv, thread_id, primary);
    if (!state || (!primary && !state->registered))
        return RET_ERROR(SCE_KERNEL_ERROR_NOT_INITIALIZED);
    const std::lock_guard lock(state->mutex);
    *info = state->info;
    return 0;
}

EXPORT(int, sceSharedFbGetRenderingInfo, SceUID pid) {
    TRACY_FUNC(sceSharedFbGetRenderingInfo, pid);
    constexpr SceInt32 permission_denied = static_cast<SceInt32>(0x80010058U);
    constexpr SceInt32 not_registered = static_cast<SceInt32>(0x80200004U);
    constexpr SceInt32 not_rendering = static_cast<SceInt32>(0x80200005U);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const auto caller_identity = emuenv.kernel.get_process_identity(caller->cpu->mem);
    if (!caller_identity || !caller_identity->is_firmware_app())
        return permission_denied;

    const auto primary = emuenv.kernel.get_process_identity(emuenv.mem);
    if (primary && primary->pid == pid) {
        const auto state = get_shared_fb_process_state(emuenv, thread_id, false);
        if (!state || !state->registered)
            return not_registered;
        const std::lock_guard lock(state->mutex);
        return state->vsync ? 0 : not_rendering;
    }
    if (!is_secondary_process_id(emuenv, pid))
        return not_registered;

    // Secondary SharedFb is not implemented; a published display frame is the available readiness signal.
    LOG_WARN_ONCE("Secondary SharedFb rendering readiness is approximated using published display frames");
    const std::lock_guard lock(emuenv.display.display_info_mutex);
    const auto frame = emuenv.display.process_frames.find(pid);
    if (frame == emuenv.display.process_frames.end() || !frame->second.software_pixels) {
        LOG_INFO_ONCE("SharedFb is waiting for the first display frame of secondary process {}", pid);
        return not_rendering;
    }
    LOG_INFO_ONCE("SharedFb detected a display frame for secondary process {}", pid);
    return 0;
}

EXPORT(int, sceSharedFbGetShellRenderPort) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSharedFbUpdateProcess) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSharedFbUpdateProcessBegin) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSharedFbUpdateProcessEnd) {
    return UNIMPLEMENTED();
}
