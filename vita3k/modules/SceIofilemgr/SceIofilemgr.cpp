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

#include "SceIofilemgr.h"

#include <../SceAppMgr/SceAppMgr.h>
#include <../SceKernelThreadMgr/lightweight.h>
#include <cpu/state.h>
#include <io/functions.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <kernel/types.h>

#include <io/io.h>

#include <map>
#include <mutex>
#include <string>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceIofilemgr);

namespace {

constexpr int IO_FLOCK_UNLOCK = 0x110;
constexpr int IO_FLOCK_SHARED = 0x101;
constexpr int IO_FLOCK_EXCLUSIVE = 0x102;
constexpr int IO_FLOCK_RESERVED = 0x104;

using FileLockOwner = std::pair<SceUID, SceUID>;
using FileLockOwners = std::map<FileLockOwner, int>;

std::mutex file_flocks_mutex;
std::map<std::string, FileLockOwners> file_flocks;

void release_file_flock(SceUID process_id, SceUID fd) {
    const std::lock_guard lock(file_flocks_mutex);
    const FileLockOwner owner{ process_id, fd };
    for (auto file = file_flocks.begin(); file != file_flocks.end();) {
        file->second.erase(owner);
        if (file->second.empty())
            file = file_flocks.erase(file);
        else
            ++file;
    }
}

} // namespace

EXPORT(int, _sceIoChstat) {
    TRACY_FUNC(_sceIoChstat);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoChstatAsync) {
    TRACY_FUNC(_sceIoChstatAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoChstatByFd) {
    TRACY_FUNC(_sceIoChstatByFd);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoCompleteMultiple) {
    TRACY_FUNC(_sceIoCompleteMultiple);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoDevctl) {
    TRACY_FUNC(_sceIoDevctl);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoDevctlAsync) {
    TRACY_FUNC(_sceIoDevctlAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoDopen, Ptr<const char> dir) {
    TRACY_FUNC(_sceIoDopen, dir);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    return open_dir_guest(*context->io, context->thread->cpu->mem, dir, emuenv.vita_fs_path, export_name);
}

EXPORT(int, _sceIoDread, const SceUID fd, Ptr<SceIoDirent> dir) {
    TRACY_FUNC(_sceIoDread, fd, dir);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    return read_dir_guest(*context->io, context->thread->cpu->mem, fd, dir, emuenv.vita_fs_path, export_name);
}

EXPORT(int, _sceIoGetstat, Ptr<const char> file, Ptr<SceIoStat> stat) {
    TRACY_FUNC(_sceIoGetstat, file, stat);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    return stat_file_guest(*context->io, context->thread->cpu->mem, file, stat, emuenv.vita_fs_path, export_name);
}

EXPORT(int, _sceIoGetstatAsync) {
    TRACY_FUNC(_sceIoGetstatAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoGetstatByFd, const SceUID fd, Ptr<SceIoStat> stat) {
    TRACY_FUNC(_sceIoGetstatByFd, fd, stat);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    return stat_file_by_fd_guest(*context->io, context->thread->cpu->mem, fd, stat, emuenv.vita_fs_path, export_name);
}

EXPORT(int, _sceIoIoctl) {
    TRACY_FUNC(_sceIoIoctl);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoIoctlAsync) {
    TRACY_FUNC(_sceIoIoctlAsync);
    return UNIMPLEMENTED();
}

EXPORT(SceOff, _sceIoLseek, const SceUID fd, Ptr<_sceIoLseekOpt> opt) {
    TRACY_FUNC(_sceIoLseek, fd, opt);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    _sceIoLseekOpt options{};
    if (!opt || !threadmgr::copy_sync_guest_bytes(context->thread->cpu->mem, opt.address(), &options, sizeof(options), false))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    return seek_file(fd, options.offset, options.whence, *context->io, export_name);
}

EXPORT(int, _sceIoLseekAsync) {
    TRACY_FUNC(_sceIoLseekAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoMkdir, Ptr<const char> dir, const SceMode mode) {
    TRACY_FUNC(_sceIoMkdir, dir, mode);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    return create_dir_guest(*context->io, context->thread->cpu->mem, dir, mode, emuenv.vita_fs_path, export_name);
}

EXPORT(int, _sceIoMkdirAsync) {
    TRACY_FUNC(_sceIoMkdirAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoOpen, Ptr<const char> file, const int flags, const SceMode mode) {
    TRACY_FUNC(_sceIoOpen, file, flags, mode);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    return open_file_guest(*context->io, context->thread->cpu->mem, file, flags, emuenv.vita_fs_path, export_name);
}

EXPORT(int, _sceIoOpenAsync) {
    TRACY_FUNC(_sceIoOpenAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoPread) {
    TRACY_FUNC(_sceIoPread);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoPreadAsync) {
    TRACY_FUNC(_sceIoPreadAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoPwrite) {
    TRACY_FUNC(_sceIoPwrite);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoPwriteAsync) {
    TRACY_FUNC(_sceIoPwriteAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoRemove) {
    TRACY_FUNC(_sceIoRemove);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoRemoveAsync) {
    TRACY_FUNC(_sceIoRemoveAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoRename) {
    TRACY_FUNC(_sceIoRename);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoRenameAsync) {
    TRACY_FUNC(_sceIoRenameAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoRmdir) {
    TRACY_FUNC(_sceIoRmdir);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoRmdirAsync) {
    TRACY_FUNC(_sceIoRmdirAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoSync) {
    TRACY_FUNC(_sceIoSync);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceIoSyncAsync) {
    TRACY_FUNC(_sceIoSyncAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoCancel) {
    TRACY_FUNC(sceIoCancel);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoChstatByFdAsync) {
    TRACY_FUNC(sceIoChstatByFdAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoClose, const SceUID fd) {
    TRACY_FUNC(sceIoClose, fd);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    const auto identity = emuenv.kernel.get_process_identity(context->thread->cpu->mem);
    const int result = close_file(*context->io, fd, export_name);
    if (result >= 0 && identity)
        release_file_flock(identity->pid, fd);
    return result;
}

EXPORT(int, sceIoCloseAsync) {
    TRACY_FUNC(sceIoCloseAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoComplete, SceUID request_id) {
    TRACY_FUNC(sceIoComplete, request_id);
    return complete_async_io(emuenv.io, request_id, export_name);
}

EXPORT(int, sceIoDclose, const SceUID fd) {
    TRACY_FUNC(sceIoDclose, fd);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    return close_dir(*context->io, fd, export_name);
}

EXPORT(int, sceIoDcloseAsync) {
    TRACY_FUNC(sceIoDcloseAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoDopenAsync) {
    TRACY_FUNC(sceIoDopenAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoDreadAsync) {
    TRACY_FUNC(sceIoDreadAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoFlockForSystem, SceUID fd, int operation) {
    TRACY_FUNC(sceIoFlockForSystem, fd, operation);
    if (operation != IO_FLOCK_UNLOCK && operation != IO_FLOCK_SHARED
        && operation != IO_FLOCK_RESERVED && operation != IO_FLOCK_EXCLUSIVE) {
        LOG_ERROR("sceIoFlockForSystem received unsupported operation {}", log_hex(operation));
        return RET_ERROR(SCE_ERROR_ERRNO_EINVAL);
    }

    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    const auto identity = emuenv.kernel.get_process_identity(context->thread->cpu->mem);
    if (!identity) {
        LOG_ERROR("sceIoFlockForSystem could not identify the calling process");
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    }

    std::shared_ptr<FileStats> file;
    {
        const std::lock_guard lock(context->io->fd_mutex);
        const auto it = context->io->std_files.find(fd);
        if (it == context->io->std_files.end())
            return RET_ERROR(SCE_ERROR_ERRNO_EBADFD);
        file = it->second;
    }
    std::string path;
    {
        const std::lock_guard lock(file->mutex);
        if (file->closed || !file->is_regular_file())
            return RET_ERROR(SCE_ERROR_ERRNO_EBADFD);
        path = file->get_system_location().generic_string();
    }
    if (path.empty()) {
        LOG_ERROR("sceIoFlockForSystem received a file descriptor without a system path");
        return RET_ERROR(SCE_ERROR_ERRNO_EBADFD);
    }

    const FileLockOwner owner{ identity->pid, fd };
    const std::lock_guard lock(file_flocks_mutex);
    auto file_locks = file_flocks.find(path);
    if (operation == IO_FLOCK_UNLOCK) {
        if (file_locks != file_flocks.end()) {
            file_locks->second.erase(owner);
            if (file_locks->second.empty())
                file_flocks.erase(file_locks);
        }
        LOG_TRACE("sceIoFlockForSystem released fd {} for process {}", fd, identity->pid);
        return 0;
    }

    auto &owners = file_flocks[path];
    for (const auto &[other_owner, other_operation] : owners) {
        if (other_owner == owner)
            continue;
        if (operation == IO_FLOCK_EXCLUSIVE
            || other_operation == IO_FLOCK_EXCLUSIVE
            || (operation == IO_FLOCK_RESERVED && other_operation == IO_FLOCK_RESERVED)) {
            LOG_DEBUG("sceIoFlockForSystem operation {} would block for fd {} (held by process {})",
                log_hex(operation), fd, other_owner.first);
            return RET_ERROR(SCE_KERNEL_ERROR_IO_WOULD_BLOCK);
        }
    }
    owners[owner] = operation;
    LOG_TRACE("sceIoFlockForSystem operation {} acquired for fd {} in process {}",
        log_hex(operation), fd, identity->pid);
    return 0;
}

EXPORT(int, sceIoGetPriority) {
    TRACY_FUNC(sceIoGetPriority);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoGetPriorityForSystem) {
    TRACY_FUNC(sceIoGetPriorityForSystem);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoGetProcessDefaultPriority) {
    TRACY_FUNC(sceIoGetProcessDefaultPriority);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoGetThreadDefaultPriority) {
    TRACY_FUNC(sceIoGetThreadDefaultPriority);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoGetThreadDefaultPriorityForSystem) {
    TRACY_FUNC(sceIoGetThreadDefaultPriorityForSystem);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoGetstatByFdAsync) {
    TRACY_FUNC(sceIoGetstatByFdAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoLseek32, const SceUID fd, const int32_t offset, const SceIoSeekMode whence) {
    TRACY_FUNC(sceIoLseek32, fd, offset, whence);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    return static_cast<int>(seek_file(fd, offset, whence, *context->io, export_name));
}

EXPORT(int, sceIoRead, const SceUID fd, Ptr<uint8_t> data, const SceSize size) {
    TRACY_FUNC(sceIoRead, fd, data, size);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    return read_file_guest(*context->io, context->thread->cpu->mem, fd, data, size, export_name);
}

EXPORT(int, sceIoReadAsync, SceUID fd, Ptr<uint8_t> data, SceSize size, Ptr<uint8_t> result) {
    TRACY_FUNC(sceIoReadAsync, fd, data.address(), size, result.address());
    return read_file_async(emuenv.io, emuenv.kernel, emuenv.mem, thread_id, fd, data, size, result, export_name);
}

EXPORT(int, sceIoSetPriority) {
    TRACY_FUNC(sceIoSetPriority);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoSetPriorityForSystem) {
    TRACY_FUNC(sceIoSetPriorityForSystem);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoSetProcessDefaultPriority) {
    TRACY_FUNC(sceIoSetProcessDefaultPriority);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoSetThreadDefaultPriority) {
    TRACY_FUNC(sceIoSetThreadDefaultPriority);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoSetThreadDefaultPriorityForSystem) {
    TRACY_FUNC(sceIoSetThreadDefaultPriorityForSystem);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    if (emuenv.kernel.is_primary_address_space(caller->cpu->mem))
        return UNIMPLEMENTED();
    STUBBED("System I/O thread priorities are not emulated");
    return SCE_KERNEL_OK;
}

EXPORT(int, sceIoSyncByFd, const SceUID fd, const int flag) {
    TRACY_FUNC(sceIoSyncByFd, fd, flag);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    return sync_file(*context->io, fd, flag, export_name);
}

EXPORT(int, sceIoSyncByFdAsync) {
    TRACY_FUNC(sceIoSyncByFdAsync);
    return UNIMPLEMENTED();
}

EXPORT(int, sceIoWrite, const SceUID fd, Ptr<const uint8_t> data, const SceSize size) {
    TRACY_FUNC(sceIoWrite, fd, data, size);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    return write_file_guest(*context->io, context->thread->cpu->mem, fd, data, size, export_name);
}

EXPORT(int, sceIoWriteAsync) {
    TRACY_FUNC(sceIoWriteAsync);
    return UNIMPLEMENTED();
}
