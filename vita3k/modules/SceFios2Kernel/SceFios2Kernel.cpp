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

#include "../SceAppMgr/SceAppMgr.h"

#include <cpu/state.h>
#include <io/functions.h>
#include <io/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>

#include <array>
#include <util/tracy.h>
TRACY_MODULE_NAME(SceFios2Kernel);

// The original syscall's undocumented fields are assumed to match the 02 layout.
struct FiosOverlayListArgs {
    Ptr<SceUID> out_ids;
    SceSize max_ids;
    Ptr<SceSize> actual_ids;
    SceSize out_ids_buffer_size;
    SceUInt32 reserved[2];
};

static_assert(sizeof(FiosOverlayListArgs) == 0x18);

struct sceFiosKernelOverlayResolveWithRangeSync_opt {
    Ptr<char> pOutPath;
    SceSize maxPath;
    uint8_t loOrderFilter;
    uint8_t hiOrderFilter;
    char reserved1;
    char reserved2;
    int reserved3;
    int reserved4;
    int reserved5;
    int reserved6;
};

static constexpr int SCE_FIOS_ERROR_NULL_OUT_ID = 0x80820006;

EXPORT(int, _sceFiosKernelOverlayAdd) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayAddForProcess, SceUID pid, const SceFiosProcessOverlay *pOverlay, SceUID *pOutID) {
    TRACY_FUNC(_sceFiosKernelOverlayAddForProcess, pid, pOverlay, pOutID);
    if (!pOutID)
        return SCE_FIOS_ERROR_NULL_OUT_ID;

    // paths are not remapped, so only an overlay that maps a path onto itself is accepted
    if (!pOverlay || strncmp(pOverlay->dst, pOverlay->src, SCE_FIOS_OVERLAY_POINT_MAX) != 0)
        return UNIMPLEMENTED();

    STUBBED("Identity overlay, nothing to remap");
    const std::lock_guard<std::mutex> guard(emuenv.io.overlay_mutex);
    *pOutID = emuenv.io.next_overlay_id++;
    return 0;
}

EXPORT(int, _sceFiosKernelOverlayDHChstatSync) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayDHCloseSync) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayDHOpenSync) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayDHReadSync) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayDHStatSync) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayDHSyncSync) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayGetInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayGetInfoForProcess) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayGetList, SceUID pid, SceUInt8 min_order, SceUInt8 max_order, Ptr<const void> args) {
    TRACY_FUNC(_sceFiosKernelOverlayGetList, pid, min_order, max_order, args);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    MemState &mem = context->thread->cpu->mem;
    const auto identity = emuenv.kernel.get_process_identity(mem);
    if (!identity || identity->pid != pid) {
        LOG_ERROR("{}: overlay enumeration for process {} from another process is not implemented", export_name, pid);
        return RET_ERROR(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
    }
    const GuestMemoryRange args_range{ args.address(), sizeof(FiosOverlayListArgs) };
    const uint64_t args_pin = args ? pin_guest_memory(mem, std::span(&args_range, 1)) : 0;
    if (!args_pin)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    FiosOverlayListArgs options{};
    auto *options_bytes = reinterpret_cast<uint8_t *>(&options);
    for (size_t i = 0; i < sizeof(options); ++i)
        options_bytes[i] = *Ptr<const uint8_t>(args.address() + static_cast<Address>(i)).get(mem);
    unpin_guest_memory(mem, args_pin);

    constexpr SceSize max_buffer_size = 0x200;
    if (options.max_ids > max_buffer_size / sizeof(SceUID) || options.out_ids_buffer_size > max_buffer_size
        || (!options.out_ids && options.max_ids))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);

    const std::array ranges{
        GuestMemoryRange{ options.out_ids.address(), options.out_ids ? options.out_ids_buffer_size : 0 },
        GuestMemoryRange{ options.actual_ids.address(), options.actual_ids ? static_cast<uint32_t>(sizeof(SceSize)) : 0U },
    };
    const uint64_t outputs_pin = options.out_ids || options.actual_ids ? pin_guest_memory(mem, ranges) : 0;
    if ((options.out_ids || options.actual_ids) && !outputs_pin)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);

    std::array<uint8_t, max_buffer_size> ids{};
    SceSize count = 0;
    {
        const std::lock_guard lock(context->io->overlay_mutex);
        for (const auto &overlay : context->io->overlays) {
            if (overlay.order < min_order || overlay.order > max_order)
                continue;
            if (count < options.max_ids) {
                const auto id = static_cast<uint32_t>(overlay.id);
                for (size_t i = 0; i < sizeof(id); ++i)
                    ids[count * sizeof(id) + i] = static_cast<uint8_t>(id >> (i * 8));
            }
            ++count;
        }
    }
    if (options.out_ids) {
        for (SceSize i = 0; i < options.out_ids_buffer_size; ++i)
            *Ptr<uint8_t>(options.out_ids.address() + i).get(mem) = ids[i];
    }
    if (options.actual_ids) {
        for (size_t i = 0; i < sizeof(count); ++i)
            *Ptr<uint8_t>(options.actual_ids.address() + static_cast<Address>(i)).get(mem) = static_cast<uint8_t>(count >> (i * 8));
    }
    if (outputs_pin)
        unpin_guest_memory(mem, outputs_pin);
    return 0;
}

EXPORT(int, _sceFiosKernelOverlayGetRecommendedScheduler) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayModify) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayModifyForProcess) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayRemove) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayRemoveForProcess) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayResolveSync) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayResolveWithRangeSync, SceUID pid, int resolveFlag, Ptr<const char> pInPath, Ptr<const void> opt) {
    TRACY_FUNC(_sceFiosKernelOverlayResolveWithRangeSync, pid, resolveFlag, pInPath, opt);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    MemState &mem = context->thread->cpu->mem;
    const auto identity = emuenv.kernel.get_process_identity(mem);
    if (pid != PSP2_KERNEL_PROCESS_ID_SELF && (!identity || identity->pid != pid)) {
        LOG_ERROR("{}: overlay resolution for another process {} is not implemented", export_name, pid);
        return RET_ERROR(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
    }
    sceFiosKernelOverlayResolveWithRangeSync_opt options{};
    if (!opt || !read_guest_memory(mem, opt.address(), &options, sizeof(options)))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    const int result = resolve_overlay_path_guest(*context->io, mem, pInPath, options.pOutPath, options.maxPath, resolveFlag, options.loOrderFilter, options.hiOrderFilter, emuenv.vita_fs_path, export_name);
    return result < 0 ? RET_ERROR(result) : result;
}

EXPORT(int, _sceFiosKernelOverlayThreadIsDisabled) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceFiosKernelOverlayThreadSetDisabled) {
    return UNIMPLEMENTED();
}
