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

#include "SceAppMgr.h"
#include "SceSharedFb.h"
#include "app_param.h"
#include "bgm.h"

#include "../SceDisplay/SceDisplay.h"
#include "../SceGxm/SceGxm.h"
#include "../SceSblACMgr/SceSblACMgrForDriver.h"

#include <app/functions.h>
#include <config/config.h>
#include <cpu/state.h>
#include <display/guest_frame.h>
#include <display/process_frames.h>
#include <gxm/state.h>
#include <io/functions.h>
#include <io/io.h>
#include <io/state.h>
#include <kernel/proc_event_handler.h>
#include <kernel/state.h>
#include <kernel/sync_primitives.h>
#include <kernel/thread/thread_state.h>
#include <mem/state.h>
#include <modules/module_parent.h>
#include <net/state.h>
#include <ngs/state.h>
#include <np/functions.h>
#include <np/state.h>
#include <packages/license.h>
#include <packages/sfo.h>
#include <renderer/state.h>
#include <util/string_utils.h>
#include <util/tracy.h>
#include <vfs/mounts.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

TRACY_MODULE_NAME(SceAppMgr);

static constexpr size_t MAX_UNLOADED_APP_STATUS_COUNT = 19;
static constexpr SceUInt32 MAX_SHELL_NOTIFICATION_COUNT = 4;
static constexpr SceUInt32 APPMGR_PROCESS_ATTR_MASK = 0x7;

struct AppMgrNotificationQueue {
    std::array<SceAppMgrNotificationRequest, MAX_SHELL_NOTIFICATION_COUNT> buffer{};
    SceUInt32 count = 0;
    SceUInt32 write_index = 0;
    SceUInt32 capacity = MAX_SHELL_NOTIFICATION_COUNT;
    std::mutex mutex;
    std::condition_variable write_slots_available;
};

struct SecondaryProcess {
    enum class State { created,
        starting,
        start_failed,
        started };
    MemState mem;
    IOState io{};
    NetState net;
    NetCtlState netctl;
    SceUID pid = 0;
    SceUID app_id = 0;
    SceUID initial_app_id = 0;
    SceUID main_thread_id = 0;
    SceUID main_module_id = 0;
    State state = State::created;
    bool preloads_started = false;
    bool activated = false;
    bool suspended = false;
    bool launch_param_read = false;
    std::vector<std::string> arguments;
    std::string launch_param;
    std::vector<ThreadStatePtr> suspended_threads;
    AppMgrLaunchStatus launch_status{};
    std::string path;
    std::string title_id;
    std::optional<SfoFile> sfo;
    std::unordered_set<SceUInt32> sysmodules_loading;
    SecondaryAppUtilState apputil;
    NpTrophyState trophy_state;
    AppMgrEvents events;

    ~SecondaryProcess() {
        netctl.deinit();
        net.deinit();
        deinit(trophy_state);
        if (mem.memory)
            deinit_mem(mem);
    }
};

struct AppMgrProcesses {
    std::mutex mutex;
    std::mutex activation_mutex;
    std::vector<std::unique_ptr<SecondaryProcess>> processes;
    std::deque<SceAppMgrShellEvent> shell_events;
    AppMgrNotificationQueue shell_notifications;
    // Shell queries an app's status after consuming its unload event.
    std::deque<SecondaryAppStatus> unloaded_apps;
    std::deque<SceUID> pending_destroy_app_ids;
    size_t reserved_shell_events = 0;
    SceUInt32 app_generation = 0;
};

static SceInt32 shell_event_app_id(const SceAppMgrShellEvent &event) {
    switch (event.event) {
    case 2:
    case 8:
        return event.data.loaded.app_id;
    case 3:
    case 4:
        return event.data.lifecycle.app_id;
    case 6:
        return event.data.launch_by_name.app_ids[0];
    case 15:
    case 16:
    case 18:
        return event.data.app_request.app_id;
    default:
        return 0;
    }
}

static AppMgrEvents *get_process_appmgr_events(EmuEnvState &emuenv, SceUID thread_id) {
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        LOG_ERROR("AppMgr events: unknown thread {}", thread_id);
        return nullptr;
    }
    if (emuenv.kernel.is_primary_address_space(thread->cpu->mem))
        return emuenv.kernel.obj_store.get<AppMgrEvents>();
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (processes) {
        const std::lock_guard lock(processes->mutex);
        for (const auto &process : processes->processes) {
            if (&process->mem == &thread->cpu->mem)
                return &process->events;
        }
    }
    LOG_ERROR("AppMgr events: no process state for thread {}", thread_id);
    return nullptr;
}

SecondaryAppUtilState *find_secondary_app_util_state(EmuEnvState &emuenv, const MemState &mem) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return nullptr;
    const std::lock_guard lock(processes->mutex);
    for (const auto &process : processes->processes) {
        if (&process->mem == &mem)
            return &process->apputil;
    }
    return nullptr;
}

IOState *find_secondary_process_io(EmuEnvState &emuenv, const MemState &mem) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return nullptr;
    const std::lock_guard lock(processes->mutex);
    for (const auto &process : processes->processes) {
        if (&process->mem == &mem)
            return &process->io;
    }
    return nullptr;
}

IOState *find_process_io(EmuEnvState &emuenv, const MemState &mem) {
    return emuenv.kernel.is_primary_address_space(mem) ? &emuenv.io : find_secondary_process_io(emuenv, mem);
}

IOState *find_process_io_by_pid(EmuEnvState &emuenv, SceUID pid) {
    const auto primary = emuenv.kernel.get_process_identity(emuenv.mem);
    if (primary && primary->pid == pid)
        return &emuenv.io;
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return nullptr;
    const std::lock_guard lock(processes->mutex);
    const auto it = std::find_if(processes->processes.begin(), processes->processes.end(),
        [pid](const auto &process) { return process->pid == pid; });
    return it == processes->processes.end() ? nullptr : &(*it)->io;
}

MemState *find_process_mem_by_pid(EmuEnvState &emuenv, SceUID pid) {
    const auto primary = emuenv.kernel.get_process_identity(emuenv.mem);
    if (primary && primary->pid == pid)
        return &emuenv.mem;
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return nullptr;
    const std::lock_guard lock(processes->mutex);
    const auto it = std::find_if(processes->processes.begin(), processes->processes.end(),
        [pid](const auto &process) { return process->pid == pid; });
    return it == processes->processes.end() ? nullptr : &(*it)->mem;
}

NpTrophyState *find_process_trophy_state(EmuEnvState &emuenv, const MemState &mem) {
    if (emuenv.kernel.is_primary_address_space(mem))
        return &emuenv.np.trophy_state;
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return nullptr;
    const std::lock_guard lock(processes->mutex);
    for (const auto &process : processes->processes) {
        if (&process->mem == &mem)
            return &process->trophy_state;
    }
    return nullptr;
}

bool is_secondary_process_id(EmuEnvState &emuenv, SceUID pid) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return false;
    const std::lock_guard lock(processes->mutex);
    return std::ranges::any_of(processes->processes, [pid](const auto &process) {
        return process->app_id && process->pid == pid;
    });
}

std::expected<ProcessIoContext, SceInt32> get_process_io_context(EmuEnvState &emuenv, SceUID thread_id, const char *operation) {
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        LOG_ERROR("{}: unknown thread {}", operation, thread_id);
        return std::unexpected(static_cast<SceInt32>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID));
    }
    IOState *io = find_process_io(emuenv, thread->cpu->mem);
    if (!io) {
        LOG_ERROR("{}: no IO state for thread {}", operation, thread_id);
        return std::unexpected(static_cast<SceInt32>(SCE_KERNEL_ERROR_ERROR));
    }
    return ProcessIoContext{ thread, io };
}

std::expected<ProcessNetworkContext, SceInt32> get_process_network_context(EmuEnvState &emuenv, SceUID thread_id, const char *operation) {
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        LOG_ERROR("{}: unknown thread {}", operation, thread_id);
        return std::unexpected(static_cast<SceInt32>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID));
    }
    if (emuenv.kernel.is_primary_address_space(thread->cpu->mem))
        return ProcessNetworkContext{ thread, &emuenv.net, &emuenv.netctl };

    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (processes) {
        const std::lock_guard lock(processes->mutex);
        for (const auto &process : processes->processes) {
            if (&process->mem == &thread->cpu->mem)
                return ProcessNetworkContext{ thread, &process->net, &process->netctl };
        }
    }
    LOG_ERROR("{}: no network state for thread {}", operation, thread_id);
    return std::unexpected(static_cast<SceInt32>(SCE_KERNEL_ERROR_ERROR));
}

static bool is_shell_caller(EmuEnvState &emuenv, SceUID thread_id) {
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return false;
    const auto identity = emuenv.kernel.get_process_identity(thread->cpu->mem);
    return identity && identity->app_path == "vs0:vsh/shell";
}

static bool uses_shell_builtin_app_params(EmuEnvState &emuenv, SceUID thread_id, SceUID pid) {
    const auto primary = emuenv.kernel.get_process_identity(emuenv.mem);
    const auto caller = emuenv.kernel.get_thread_process_id(thread_id);
    return primary && caller && primary->is_shell() && *caller == primary->pid
        && (pid == 0 || pid == primary->pid) && emuenv.sfo_handle.header.magic != 0x46535000;
}

static int get_process_sfo(EmuEnvState &emuenv, SceUID thread_id, SceUID pid, SfoFile &sfo) {
    const auto caller_pid = emuenv.kernel.get_thread_process_id(thread_id);
    if (!caller_pid)
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    if (!pid)
        pid = *caller_pid;
    if (pid != *caller_pid && !is_shell_caller(emuenv, thread_id))
        return static_cast<int>(SCE_APPMGR_ERROR_PERMISSION);
    const auto primary = emuenv.kernel.get_process_identity(emuenv.mem);
    if (primary && primary->pid == pid) {
        if (emuenv.sfo_handle.header.magic != 0x46535000) {
            LOG_ERROR("No valid application SFO for process {}", pid);
            return static_cast<int>(SCE_APPMGR_ERROR_INVALID);
        }
        sfo = emuenv.sfo_handle;
        return 0;
    }
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot resolve application parameters without AppMgr process state");
        return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
    }
    const std::lock_guard lock(processes->mutex);
    for (const auto &process : processes->processes) {
        if (process->pid != pid || !process->app_id)
            continue;
        if (!process->sfo) {
            LOG_ERROR("No valid application SFO for secondary process {}", pid);
            return static_cast<int>(SCE_APPMGR_ERROR_INVALID);
        }
        sfo = *process->sfo;
        return 0;
    }
    return static_cast<int>(SCE_APPMGR_ERROR_APP_NOT_FOUND);
}

static SecondaryAppStatus secondary_status(const SecondaryProcess &process) {
    auto status = process.launch_status;
    status.activated = process.activated;
    status.process_run_requested = process.state == SecondaryProcess::State::started && !process.suspended;
    return { status };
}

static int suspend_secondary_process_threads(EmuEnvState &emuenv, SecondaryProcess &process) {
    std::vector<ThreadStatePtr> threads;
    {
        const std::lock_guard lock(emuenv.kernel.mutex);
        try {
            threads.reserve(emuenv.kernel.threads.size());
            for (const auto &[_, thread] : emuenv.kernel.threads) {
                if (&thread->cpu->mem == &process.mem)
                    threads.push_back(thread);
            }
        } catch (const std::bad_alloc &) {
            LOG_ERROR("Could not allocate thread list while suspending app '{}'", process.title_id);
            return SCE_KERNEL_ERROR_NO_MEMORY;
        }
    }
    if (threads.empty()) {
        LOG_ERROR("Cannot suspend app '{}': it has no registered threads", process.title_id);
        return SCE_KERNEL_ERROR_ERROR;
    }

    std::vector<ThreadStatePtr> suspended;
    try {
        suspended.reserve(threads.size());
    } catch (const std::bad_alloc &) {
        LOG_ERROR("Could not allocate suspend state for app '{}'", process.title_id);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }
    for (const auto &thread : threads) {
        const int result = thread->vm_suspend();
        if (result == SCE_KERNEL_ERROR_DORMANT)
            continue;
        if (result < 0) {
            for (auto it = suspended.rbegin(); it != suspended.rend(); ++it) {
                const int resume_result = (*it)->vm_resume();
                if (resume_result < 0 && resume_result != SCE_KERNEL_ERROR_DORMANT)
                    LOG_ERROR("Could not roll back suspension of thread {} for app '{}': {}",
                        (*it)->id, process.title_id, log_hex(resume_result));
            }
            LOG_ERROR("Could not suspend thread {} for app '{}': {}", thread->id, process.title_id, log_hex(result));
            return result;
        }
        suspended.push_back(thread);
    }
    process.suspended_threads = std::move(suspended);
    process.suspended = true;
    return 0;
}

static int resume_secondary_process_threads(SecondaryProcess &process) {
    std::vector<ThreadStatePtr> still_suspended;
    try {
        still_suspended.reserve(process.suspended_threads.size());
    } catch (const std::bad_alloc &) {
        LOG_ERROR("Could not allocate resume state for app '{}'", process.title_id);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    int first_error = 0;
    for (auto it = process.suspended_threads.rbegin(); it != process.suspended_threads.rend(); ++it) {
        const int result = (*it)->vm_resume();
        if (result == SCE_KERNEL_ERROR_DORMANT)
            continue;
        if (result < 0) {
            LOG_ERROR("Could not resume thread {} for app '{}': {}", (*it)->id, process.title_id, log_hex(result));
            still_suspended.push_back(*it);
            if (first_error == 0)
                first_error = result;
        }
    }
    process.suspended_threads = std::move(still_suspended);
    if (first_error == 0)
        process.suspended = false;
    return first_error;
}

std::optional<SecondaryAppStatus> find_secondary_app_by_id(EmuEnvState &emuenv, SceUID app_id, bool include_initial_id) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return std::nullopt;
    const std::lock_guard lock(processes->mutex);
    for (const auto &process : processes->processes) {
        if (process->app_id && (process->app_id == app_id || (include_initial_id && process->state == SecondaryProcess::State::started && process->initial_app_id == app_id)))
            return secondary_status(*process);
    }
    return std::nullopt;
}

std::optional<SecondaryAppStatus> find_unloaded_app_by_id(EmuEnvState &emuenv, SceUID app_id) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return std::nullopt;
    const std::lock_guard lock(processes->mutex);
    const auto it = std::find_if(processes->unloaded_apps.rbegin(), processes->unloaded_apps.rend(),
        [app_id](const auto &app) { return app.status.app_id == app_id; });
    return it == processes->unloaded_apps.rend() ? std::nullopt : std::optional<SecondaryAppStatus>(*it);
}

std::optional<SecondaryAppStatus> find_secondary_app_by_pid(EmuEnvState &emuenv, SceUID pid) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return std::nullopt;
    const std::lock_guard lock(processes->mutex);
    for (const auto &process : processes->processes) {
        if (process->app_id && process->pid == pid)
            return secondary_status(*process);
    }
    return std::nullopt;
}

std::optional<SecondaryAppStatus> find_active_secondary_app(EmuEnvState &emuenv) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return std::nullopt;
    const std::lock_guard lock(processes->mutex);
    const auto active = std::ranges::find_if(processes->processes, [](const auto &process) {
        return process->activated && process->state == SecondaryProcess::State::started;
    });
    return active == processes->processes.end() ? std::nullopt : std::optional<SecondaryAppStatus>(secondary_status(**active));
}

int appmgr_capture_framebuf_iftu_by_app_id(EmuEnvState &emuenv, SceUID thread_id, SceInt32 app_id, Ptr<const void> frame_buf) {
    if (!is_shell_caller(emuenv, thread_id))
        return static_cast<int>(SCE_APPMGR_ERROR_SHELL_ONLY);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    if (!frame_buf)
        return static_cast<int>(SCE_APPMGR_ERROR_NULL_POINTER);

    auto &mem = caller->cpu->mem;
    const GuestMemoryRange request_range{ frame_buf.address(), sizeof(SceDisplayFrameBuf) };
    const uint64_t request_pin = pin_guest_memory(mem, std::span(&request_range, 1));
    if (!request_pin)
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_POINTER);
    const auto *guest_request = Ptr<const SceDisplayFrameBuf>(frame_buf.address()).get(mem);
    if (!guest_request) {
        unpin_guest_memory(mem, request_pin);
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_POINTER);
    }
    const SceDisplayFrameBuf request = *guest_request;
    unpin_guest_memory(mem, request_pin);
    if (request.size != sizeof(SceDisplayFrameBuf) || !request.base)
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_POINTER);
    if (request.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8
        || !request.width || !request.height || request.width > 1920 || request.height > 1088
        || request.pitch < request.width || request.pitch > 4096)
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER2);

    SceUID pid = 0;
    if (app_id < 0) {
        const std::lock_guard display_lock(emuenv.display.display_info_mutex);
        pid = emuenv.display.active_display_pid;
    } else {
        const auto app = find_secondary_app_by_id(emuenv, app_id, true);
        if (!app) {
            LOG_WARN("Cannot capture framebuffer for app {}: AppMgr process was not found", log_hex(app_id));
            return static_cast<int>(SCE_APPMGR_ERROR_APP_NOT_FOUND);
        }
        pid = app->status.process_id;
    }
    if (pid <= 0)
        return static_cast<int>(SCE_APPMGR_ERROR_STATE);

    DisplayFrameInfo frame;
    SceUID active_display_pid = 0;
    {
        const std::lock_guard display_lock(emuenv.display.display_info_mutex);
        active_display_pid = emuenv.display.active_display_pid;
        const auto it = emuenv.display.process_frames.find(pid);
        if (it == emuenv.display.process_frames.end()) {
            LOG_WARN("Cannot capture framebuffer for app {} (process {}): no display frame was published",
                log_hex(app_id), pid);
            return static_cast<int>(SCE_APPMGR_ERROR_STATE);
        }
        frame = it->second;
    }
    if (!frame.software_pixels || frame.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8
        || frame.image_size.x <= 0 || frame.image_size.y <= 0) {
        LOG_WARN("Cannot capture framebuffer for app {} (process {}): published frame is not a valid software image",
            log_hex(app_id), pid);
        return static_cast<int>(SCE_APPMGR_ERROR_STATE);
    }
    LOG_INFO("CaptureFrameBufIFTUByAppId: requested app {}, selected PID {}, frame owner {}, active display PID {}, frame {}x{}, guest PC {}, LR {}",
        log_hex(app_id), log_hex(pid), log_hex(frame.owner_pid), log_hex(active_display_pid), frame.image_size.x, frame.image_size.y,
        log_hex(caller->cpu->get_pc()), log_hex(caller->cpu->get_lr()));

    const uint64_t source_row_size = uint64_t(frame.image_size.x) * sizeof(uint32_t);
    const uint64_t source_size = source_row_size * static_cast<uint32_t>(frame.image_size.y);
    if (source_size > frame.software_pixels->size()) {
        LOG_ERROR("Cannot capture framebuffer for app {} (process {}): published image buffer is truncated",
            log_hex(app_id), pid);
        return static_cast<int>(SCE_APPMGR_ERROR_STATE);
    }

    const uint32_t destination_row_size = request.width * sizeof(uint32_t);
    const uint64_t destination_stride = uint64_t(request.pitch) * sizeof(uint32_t);
    const uint64_t destination_size = uint64_t(request.height - 1) * destination_stride + destination_row_size;
    if (destination_size > std::numeric_limits<uint32_t>::max()
        || uint64_t(request.base.address()) + destination_size > (uint64_t{ 1 } << 32))
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_POINTER);

    const GuestMemoryRange destination_range{ request.base.address(), static_cast<uint32_t>(destination_size) };
    const uint64_t destination_pin = pin_guest_memory(mem, std::span(&destination_range, 1));
    if (!destination_pin)
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_POINTER);
    struct DestinationPinGuard {
        MemState &mem;
        uint64_t pin;
        ~DestinationPinGuard() { unpin_guest_memory(mem, pin); }
    } destination_guard{ mem, destination_pin };

    std::vector<uint8_t> row;
    try {
        row.resize(destination_row_size);
    } catch (const std::bad_alloc &) {
        return static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
    }

    const auto &source = *frame.software_pixels;
    for (uint32_t y = 0; y < request.height; ++y) {
        const uint32_t source_y = static_cast<uint32_t>(uint64_t(y) * frame.image_size.y / request.height);
        for (uint32_t x = 0; x < request.width; ++x) {
            const uint32_t source_x = static_cast<uint32_t>(uint64_t(x) * frame.image_size.x / request.width);
            const size_t source_offset = (size_t(source_y) * frame.image_size.x + source_x) * sizeof(uint32_t);
            std::memcpy(row.data() + size_t(x) * sizeof(uint32_t), source.data() + source_offset, sizeof(uint32_t));
        }

        const uint64_t row_address = uint64_t(request.base.address()) + uint64_t(y) * destination_stride;
        if (!write_guest_memory_with_cache_invalidation(mem, static_cast<Address>(row_address), row.data(), destination_row_size))
            return static_cast<int>(SCE_APPMGR_ERROR_INVALID_POINTER);
    }

    return 0;
}

std::optional<SecondaryAppStatus> find_secondary_app_by_name(EmuEnvState &emuenv, std::string_view name) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return std::nullopt;
    const std::lock_guard lock(processes->mutex);
    for (const auto &process : processes->processes) {
        if (process->app_id && process->title_id == name)
            return secondary_status(*process);
    }
    return std::nullopt;
}

int appmgr_get_running_app_id_list(EmuEnvState &emuenv, SceUID thread_id, Ptr<SceInt32> app_ids, SceUInt32 count) {
    if (!is_shell_caller(emuenv, thread_id))
        return static_cast<int>(SCE_APPMGR_ERROR_SHELL_ONLY);
    if (!app_ids)
        return static_cast<int>(SCE_APPMGR_ERROR_NULL_POINTER);
    if (count == 0 || count > 20) {
        LOG_ERROR("Invalid running application list capacity {}", count);
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER2);
    }
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot list running applications without AppMgr process state");
        return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
    }
    std::array<SceInt32, 20> ids{};
    size_t found = 0;
    {
        const std::lock_guard lock(processes->mutex);
        for (const auto &process : processes->processes) {
            if (process->app_id && process->state == SecondaryProcess::State::started) {
                ids[found++] = process->app_id;
                if (found == count)
                    break;
            }
        }
    }
    auto &mem = thread->cpu->mem;
    const GuestMemoryRange range{ app_ids.address(), static_cast<uint32_t>(count * sizeof(SceInt32)) };
    const uint64_t pin = pin_guest_memory(mem, std::span(&range, 1));
    if (!pin) {
        LOG_ERROR("Invalid running application list destination {}", log_hex(app_ids.address()));
        return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    }
    const auto *bytes = reinterpret_cast<const uint8_t *>(ids.data());
    for (size_t i = 0; i < range.size; ++i)
        *Ptr<uint8_t>(app_ids.address() + static_cast<Address>(i)).get(mem) = bytes[i];
    unpin_guest_memory(mem, pin);
    return static_cast<int>(found);
}

static int appmgr_destroy_app_by_app_id(EmuEnvState &emuenv, SceUID thread_id, SceInt32 app_id,
    bool pending_only, const char *export_name) {
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    if (!is_shell_caller(emuenv, thread_id))
        return RET_ERROR(SCE_APPMGR_ERROR_NOT_SHELL);
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot destroy an app without AppMgr process state");
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    }

    const std::lock_guard activation_lock(processes->activation_mutex);
    SecondaryProcess *process = nullptr;
    SceUID process_id = 0;
    {
        const std::lock_guard lock(processes->mutex);
        const auto it = std::find_if(processes->processes.begin(), processes->processes.end(),
            [app_id](const auto &candidate) { return app_id != 0 && candidate->app_id == app_id; });
        if (it == processes->processes.end())
            return RET_ERROR(SCE_APPMGR_ERROR_APP_NOT_FOUND);
        if ((*it)->state == SecondaryProcess::State::starting)
            return RET_ERROR(SCE_APPMGR_ERROR_BUSY);
        if (pending_only && (*it)->state != SecondaryProcess::State::created)
            return RET_ERROR(SCE_APPMGR_ERROR_STATE);
        if (processes->shell_events.size() + processes->reserved_shell_events >= 180) {
            LOG_ERROR("Cannot destroy app {}: Shell event queue is full", log_hex(app_id));
            return RET_ERROR(SCE_APPMGR_ERROR_BUSY);
        }
        process = it->get();
        process_id = process->pid;
        ++processes->reserved_shell_events;
        process->state = SecondaryProcess::State::starting;
    }

    const auto release_event_reservation = [&] {
        const std::lock_guard lock(processes->mutex);
        --processes->reserved_shell_events;
    };
    const auto fail_stopped_process = [&](SceInt32 error) {
        release_event_reservation();
        return RET_ERROR(error);
    };
    SceUID shell_pid = 0;
    if (const auto identity = emuenv.kernel.get_process_identity(emuenv.mem))
        shell_pid = identity->pid;
    bool was_displayed = false;
    {
        const std::lock_guard display_lock(emuenv.display.display_info_mutex);
        was_displayed = emuenv.display.active_display_pid == process_id;
    }
    const bool frame_changed = was_displayed && display::select_process_frame(emuenv.display, shell_pid);

    if (auto *bgm = emuenv.kernel.obj_store.get<AppMgrBgm>())
        bgm->release(process_id);
    process->netctl.deinit();
    process->net.deinit();
    {
        ThreadHostWait host_wait(thread);
        emuenv.kernel.stop_threads_in_address_space(process->mem);
    }
    process->suspended_threads.clear();
    const bool frame_cleared = display::clear_process_frame(emuenv.display, process_id);
    if (frame_changed || frame_cleared)
        emuenv.renderer->should_display = true;
    {
        const std::lock_guard lock(processes->mutex);
        process->activated = false;
        process->state = SecondaryProcess::State::start_failed;
    }
    bool has_gxm_state = false;
    {
        const std::lock_guard lock(emuenv.gxm.process_states_mutex);
        has_gxm_state = emuenv.gxm.process_states.contains(&process->mem);
    }
    if (has_gxm_state) {
        const int gxm_result = terminate_secondary_process_gxm(emuenv, process->mem, thread_id);
        if (gxm_result < 0) {
            LOG_ERROR("Could not terminate GXM state while destroying app {}: {}", log_hex(app_id), log_hex(gxm_result));
            return fail_stopped_process(gxm_result);
        }
    }
    ngs::release_process_state(emuenv.ngs, process->mem);

    if (!emuenv.kernel.notify_process_termination_callbacks(process->mem)) {
        LOG_ERROR("Could not notify process termination callbacks while destroying app {}", log_hex(app_id));
        return fail_stopped_process(SCE_KERNEL_ERROR_ERROR);
    }

    if (process->main_module_id > 0) {
        std::vector<SceUID> preload_ids;
        {
            const auto modules = emuenv.kernel.get_process_start_modules(process->mem, process->main_module_id);
            if (!modules) {
                LOG_ERROR("Cannot enumerate modules while destroying app {}", log_hex(app_id));
                return fail_stopped_process(SCE_KERNEL_ERROR_ERROR);
            }
            for (const auto &module : modules->preloaded)
                preload_ids.push_back(module->info.modid);
        }
        for (auto it = preload_ids.rbegin(); it != preload_ids.rend(); ++it) {
            const SceUID module_id = *it;
            if (unload_module(emuenv, process->mem, module_id) < 0) {
                LOG_ERROR("Could not unload module {} while destroying app {}", module_id, log_hex(app_id));
                return fail_stopped_process(SCE_KERNEL_ERROR_ERROR);
            }
        }
        if (unload_module(emuenv, process->mem, process->main_module_id) < 0) {
            LOG_ERROR("Could not unload the main module while destroying app {}", log_hex(app_id));
            return fail_stopped_process(SCE_KERNEL_ERROR_ERROR);
        }
        process->main_module_id = 0;
    }
    release_shared_fb_process_state(emuenv, process->mem);
    if (!emuenv.kernel.release_thread_address_space(process->mem)) {
        LOG_ERROR("Could not release the address space while destroying app {}", log_hex(app_id));
        return fail_stopped_process(SCE_KERNEL_ERROR_ERROR);
    }

    {
        const std::lock_guard lock(processes->mutex);
        SceAppMgrShellEvent unloaded_event{};
        unloaded_event.event = 4;
        unloaded_event.data.lifecycle.process_id = process_id;
        unloaded_event.data.lifecycle.exit_reason = 0;
        unloaded_event.data.lifecycle.source_pid = process->launch_status.launch_source_process_id;
        unloaded_event.data.lifecycle.self_quit = 0;
        unloaded_event.data.lifecycle.app_id = app_id;
        unloaded_event.data.lifecycle.source_app_id = process->launch_status.launch_source_app_id;
        unloaded_event.data.lifecycle.relaunch_pending = 0;
        std::copy_n(process->title_id.begin(),
            std::min(process->title_id.size(), sizeof(unloaded_event.data.lifecycle.app_name)),
            unloaded_event.data.lifecycle.app_name);
        processes->unloaded_apps.push_back(secondary_status(*process));
        if (processes->unloaded_apps.size() > MAX_UNLOADED_APP_STATUS_COUNT)
            processes->unloaded_apps.pop_front();
        processes->shell_events.push_back(unloaded_event);
        --processes->reserved_shell_events;
        const auto it = std::find_if(processes->processes.begin(), processes->processes.end(),
            [process](const auto &candidate) { return candidate.get() == process; });
        if (it == processes->processes.end()) {
            LOG_ERROR("Destroyed app {} disappeared from AppMgr process state", log_hex(app_id));
            return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
        }
        processes->processes.erase(it);
    }
    LOG_DEBUG("Destroyed app {} (process {}) and queued its Shell unload event", log_hex(app_id), process_id);
    return 0;
}

int appmgr_receive_shell_event(EmuEnvState &emuenv, SceUID thread_id, Ptr<SceAppMgrShellEvent> event) {
    if (!is_shell_caller(emuenv, thread_id))
        return static_cast<int>(SCE_APPMGR_ERROR_NOT_SHELL);
    if (!event)
        return static_cast<int>(SCE_APPMGR_ERROR_NULL_POINTER);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    auto &mem = thread->cpu->mem;
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot receive a Shell event without AppMgr process state");
        return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
    }
    std::optional<SceUID> pending_destroy_app_id;
    {
        const std::lock_guard lock(processes->mutex);
        if (!processes->pending_destroy_app_ids.empty()) {
            pending_destroy_app_id = processes->pending_destroy_app_ids.front();
            processes->pending_destroy_app_ids.pop_front();
        }
    }
    if (pending_destroy_app_id) {
        const int destroy_result = appmgr_destroy_app_by_app_id(emuenv, thread_id, *pending_destroy_app_id,
            false, "sceAppMgrDestroyAppByName");
        if (destroy_result < 0) {
            LOG_ERROR("Deferred destruction of app {} failed: {}", log_hex(*pending_destroy_app_id), log_hex(destroy_result));
            return destroy_result;
        }
    }
    int result = static_cast<int>(SCE_APPMGR_ERROR_APP_PARAM_READ);
    {
        const std::lock_guard lock(processes->mutex);
        if (!processes->shell_events.empty()) {
            if (write_appmgr_shell_event(mem, event, processes->shell_events.front())) {
                LOG_DEBUG("Shell received AppMgr event {} for app {}",
                    processes->shell_events.front().event, log_hex(shell_event_app_id(processes->shell_events.front())));
                processes->shell_events.pop_front();
                result = 0;
            } else {
                LOG_ERROR("Invalid Shell event destination {}", log_hex(event.address()));
                result = static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
            }
        }
    }
    return result;
}

int appmgr_queue_secondary_process_exit(EmuEnvState &emuenv, SceUID thread_id) {
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot queue secondary process exit without AppMgr process state");
        return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
    }

    const auto &mem = thread->cpu->mem;
    const std::lock_guard activation_lock(processes->activation_mutex);
    const std::lock_guard lock(processes->mutex);
    const auto it = std::find_if(processes->processes.begin(), processes->processes.end(),
        [&mem](const auto &process) { return &process->mem == &mem; });
    if (it == processes->processes.end() || !(*it)->app_id) {
        LOG_ERROR("Cannot find AppMgr state for exiting secondary thread {}", thread_id);
        return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
    }

    const SceUID app_id = (*it)->app_id;
    if (std::ranges::find(processes->pending_destroy_app_ids, app_id) != processes->pending_destroy_app_ids.end())
        return 0;
    if (processes->shell_events.size() + processes->reserved_shell_events >= 179)
        return static_cast<int>(SCE_APPMGR_ERROR_BUSY);
    try {
        processes->pending_destroy_app_ids.push_back(app_id);
        try {
            SceAppMgrShellEvent event{};
            event.event = 18;
            event.data.app_request.app_id = app_id;
            processes->shell_events.push_back(event);
        } catch (const std::bad_alloc &) {
            processes->pending_destroy_app_ids.pop_back();
            throw;
        }
    } catch (const std::bad_alloc &) {
        LOG_ERROR("Could not queue exit cleanup for secondary app {}", log_hex(app_id));
        return static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
    }
    LOG_DEBUG("Queued cleanup for exiting secondary app {} (process {})", log_hex(app_id), (*it)->pid);
    return 0;
}

IOState *begin_secondary_sysmodule_load(EmuEnvState &emuenv, const MemState &mem, SceUInt32 module_id) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot load a secondary system module without AppMgr process state");
        return nullptr;
    }
    const std::lock_guard lock(processes->mutex);
    for (const auto &process : processes->processes) {
        if (&process->mem != &mem)
            continue;
        if (!process->sysmodules_loading.insert(module_id).second) {
            LOG_ERROR("System module {} is already being loaded in process {}", log_hex(module_id), process->pid);
            return nullptr;
        }
        return &process->io;
    }
    LOG_ERROR("Cannot find the AppMgr process for system module {}", log_hex(module_id));
    return nullptr;
}

void end_secondary_sysmodule_load(EmuEnvState &emuenv, const MemState &mem, SceUInt32 module_id) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot release a secondary system module reservation without AppMgr process state");
        return;
    }
    const std::lock_guard lock(processes->mutex);
    for (const auto &process : processes->processes) {
        if (&process->mem == &mem) {
            if (process->sysmodules_loading.erase(module_id) != 1)
                LOG_ERROR("System module {} had no load reservation in process {}", log_hex(module_id), process->pid);
            return;
        }
    }
    LOG_ERROR("Cannot release system module {}: secondary process not found", log_hex(module_id));
}

SceUID open_secondary_process_terminal(EmuEnvState &emuenv, const MemState &mem, const char *path, int flags, const char *export_name) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot open secondary process terminal without AppMgr process state");
        return SCE_KERNEL_ERROR_ERROR;
    }
    const std::lock_guard lock(processes->mutex);
    for (const auto &process : processes->processes) {
        if (&process->mem == &mem)
            return open_file(process->io, path, flags, emuenv.vita_fs_path, export_name);
    }
    LOG_ERROR("Cannot open terminal: secondary process not found");
    return SCE_KERNEL_ERROR_ERROR;
}

LIBRARY_INIT(SceAppMgr) {
    emuenv.kernel.obj_store.create<AppMgrBgm>();
    emuenv.kernel.obj_store.create<AppMgrEvents>();
    emuenv.kernel.obj_store.create<AppMgrProcesses>();
}

static bool resolve_appmgr_bgm_proxy(EmuEnvState &emuenv, AppMgrBgmState &state) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot resolve BGM proxy without AppMgr process state");
        return false;
    }

    const auto primary = emuenv.kernel.get_process_identity(emuenv.mem);
    const SceUID initial_owner_pid = state.bgmOutPortOwner;
    SceUID owner_pid = initial_owner_pid;
    SceInt32 owner_app_id = -1;
    SceInt32 initial_app_id = -1;
    const std::lock_guard lock(processes->mutex);

    for (size_t hop = 0; hop < 20; ++hop) {
        const auto owner = std::find_if(processes->processes.begin(), processes->processes.end(),
            [owner_pid](const auto &process) { return process->pid == owner_pid; });
        if (owner == processes->processes.end())
            break;

        if (owner_app_id < 0)
            owner_app_id = (*owner)->app_id;
        if (hop == 0)
            initial_app_id = owner_app_id;

        const char *proxy_name = (*owner)->launch_status.bgm_proxy_app_name;
        const auto proxy_name_end = std::find(proxy_name, proxy_name + sizeof((*owner)->launch_status.bgm_proxy_app_name), '\0');
        const std::string_view proxy_name_view(proxy_name, static_cast<size_t>(proxy_name_end - proxy_name));
        if (proxy_name_view.empty())
            break;

        if (primary && primary->pid != owner_pid && emuenv.io.title_id == proxy_name_view) {
            owner_pid = primary->pid;
            owner_app_id = -1;
            break;
        }

        const auto proxy = std::find_if(processes->processes.begin(), processes->processes.end(),
            [proxy_name_view](const auto &process) {
                return process->app_id && process->title_id == proxy_name_view;
            });
        if (proxy == processes->processes.end())
            break;

        owner_pid = (*proxy)->pid;
        owner_app_id = (*proxy)->app_id;
        if (owner_pid == initial_owner_pid || hop == 19) {
            owner_pid = initial_owner_pid;
            owner_app_id = initial_app_id;
            break;
        }
    }

    state.bgmOutPortOwner = owner_pid;
    state.appId = owner_app_id;
    if (owner_pid != initial_owner_pid)
        LOG_DEBUG("Resolved BGM proxy for process {} to process {} (app {})", initial_owner_pid, owner_pid, owner_app_id);
    return true;
}

std::optional<AppMgrBgmState> get_appmgr_bgm_state(EmuEnvState &emuenv, SceUID pid) {
    auto *bgm = emuenv.kernel.obj_store.get<AppMgrBgm>();
    if (!bgm)
        return std::nullopt;
    AppMgrBgmState state = bgm->state(pid);
    if (state.bgmOutPortOwner >= 0 && !resolve_appmgr_bgm_proxy(emuenv, state))
        return std::nullopt;
    return state;
}

static std::string appmgr_mount_log_string(const MemState &mem, Ptr<const char> input, size_t limit = 256) {
    if (!input)
        return "<null>";

    std::string result;
    for (size_t i = 0; i < limit; ++i) {
        if (i > std::numeric_limits<Address>::max() - input.address())
            return result + "<invalid>";
        const Ptr<const char> current(input.address() + static_cast<Address>(i));
        if (!current.valid(mem))
            return result + "<invalid>";
        const unsigned char ch = *current.get(mem);
        if (ch == 0)
            return result;
        result += ch >= 32 && ch < 127 ? static_cast<char>(ch) : '?';
    }
    return result + "<truncated>";
}

static bool read_mount_string(const MemState &mem, Ptr<const char> input, size_t max_length, std::string &value) {
    value.clear();
    if (!input)
        return false;
    for (size_t i = 0; i <= max_length; ++i) {
        if (i > std::numeric_limits<Address>::max() - input.address())
            return false;
        const Ptr<const char> current(input.address() + static_cast<Address>(i));
        if (!current.valid(mem))
            return false;
        const char ch = *current.get(mem);
        if (ch == '\0')
            return true;
        if (i == max_length)
            return false;
        value.push_back(ch);
    }
    return false;
}

static bool valid_mount_output(const MemState &mem, Ptr<char> output) {
    if (!output || output.address() > std::numeric_limits<Address>::max() - 15)
        return false;
    for (Address i = 0; i < 16; ++i) {
        if (!Ptr<char>(output.address() + i).valid(mem))
            return false;
    }
    return true;
}

static bool valid_mount_sfo(EmuEnvState &emuenv, IOState &io, const std::string &mount_point, const char *relative_sfo, const std::string &title_id, const char *export_name) {
    const std::string sfo_path = mount_point + relative_sfo;
    SceIoStat stat{};
    if (stat_file(io, sfo_path.c_str(), &stat, emuenv.vita_fs_path, export_name) < 0
        || !(stat.st_mode & SCE_S_IFREG) || stat.st_size <= 0 || stat.st_size > 32768)
        return false;

    const SceUID fd = open_file(io, sfo_path.c_str(), SCE_O_RDONLY, emuenv.vita_fs_path, export_name);
    if (fd < 0)
        return false;
    vfs::FileBuffer buffer(static_cast<size_t>(stat.st_size));
    const int bytes_read = read_file(buffer.data(), io, fd, static_cast<SceSize>(buffer.size()), export_name);
    close_file(io, fd, export_name);
    sfo::SfoAppInfo info;
    return bytes_read == static_cast<int>(buffer.size())
        && sfo::get_param_info(info, buffer, emuenv.cfg.sys_lang) && info.app_title_id == title_id;
}

EXPORT(SceInt32, __sceAppMgrGetAppState, SceAppMgrAppState *appState, SceUInt32 sizeofSceAppMgrAppState, SceUInt32 buildVersion) {
    TRACY_FUNC(__sceAppMgrGetAppState, appState, sizeofSceAppMgrAppState, buildVersion);
    if (!appState)
        return RET_ERROR(SCE_APPMGR_ERROR_NULL_POINTER);
    constexpr size_t minimum_size = offsetof(SceAppMgrAppState, appEventNum) + sizeof(SceAppMgrAppState::appEventNum);
    if (sizeofSceAppMgrAppState < minimum_size)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER2);

    const auto *events = get_process_appmgr_events(emuenv, thread_id);
    if (!events)
        return RET_ERROR(SCE_APPMGR_ERROR_STATE);
    memset(appState, 0, std::min<size_t>(sizeofSceAppMgrAppState, sizeof(*appState)));
    appState->appEventNum = events->pending_count();

    return 0;
}

EXPORT(int, _sceAppMgrAcidDirSet) {
    TRACY_FUNC(_sceAppMgrAcidDirSet);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrAcquireSoundOutExclusive3) {
    TRACY_FUNC(_sceAppMgrAcquireSoundOutExclusive3);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrAddContAddMount) {
    TRACY_FUNC(_sceAppMgrAddContAddMount);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrAddContMount) {
    TRACY_FUNC(_sceAppMgrAddContMount);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrAppDataMount, int mountId, Ptr<char> mountPoint) {
    TRACY_FUNC(_sceAppMgrAppDataMount, mountId, mountPoint.address());
    const auto context = get_process_io_context(emuenv, thread_id, "AppDataMount");
    if (!context)
        return RET_ERROR(context.error());
    MemState &mem = context->thread->cpu->mem;
    IOState &io = *context->io;
    if (!valid_mount_output(mem, mountPoint))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);

    std::string source_path;
    std::string fixed_mount_point;
    switch (mountId) {
    case 0x64:
        source_path = "ux0:picture";
        fixed_mount_point = "photo0:";
        break;
    case 0x65:
    case 0x66:
    case 0x67:
        if (io.device_paths.user_id.empty())
            return RET_ERROR(SCE_APPMGR_ERROR_STATE);
        source_path = "ur0:user/" + io.device_paths.user_id + (mountId == 0x65 ? "/psnfriend" : mountId == 0x66 ? "/psnmsg"
                                                                                                                : "/near");
        break;
    case 0x69:
        source_path = "ux0:music";
        fixed_mount_point = "music0:";
        break;
    case 0x6C:
        source_path = "ux0:calendar";
        break;
    case 0x6D:
        source_path = "ux0:video";
        fixed_mount_point = "video0:";
        break;
    case 0x6F:
        source_path = "tm0:nphome";
        break;
    case 0x70:
        source_path = "ux0:book";
        break;
    default:
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    }

    const fs::path directory = expand_path(io, source_path.c_str(), emuenv.vita_fs_path);
    if (directory.empty())
        return RET_ERROR(SCE_ERROR_ERRNO_ENOENT);
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        LOG_ERROR("Cannot prepare app data directory '{}': {}", source_path, error.message());
        return RET_ERROR(SCE_ERROR_ERRNO_EIO);
    }

    std::string mount_point;
    const int result = pfsmgr::mount_app_data(io, directory, io.title_id, fixed_mount_point, mount_point);
    if (result != 0)
        return RET_ERROR(result);
    for (Address i = 0; i < 16; ++i)
        *Ptr<char>(mountPoint.address() + i).get(mem) = i < mount_point.size() ? mount_point[i] : '\0';
    const auto pid = emuenv.kernel.get_thread_process_id(thread_id);
    LOG_INFO("Mounted app data type {} at '{}' for process {}", log_hex(mountId), mount_point, pid.value_or(0));
    return 0;
}

EXPORT(int, _sceAppMgrAppDataMountById, int mountId, Ptr<const char> id, Ptr<char> mountPoint) {
    TRACY_FUNC(_sceAppMgrAppDataMountById, mountId, id.address(), mountPoint.address());
    if (mountId != 0x6e) {
        LOG_WARN("Unsupported AppDataMountById type {}", log_hex(mountId));
        return RET_ERROR(SCE_ERROR_ERRNO_ENOSYS);
    }
    std::string title_id;
    if (!read_mount_string(emuenv.mem, id, 32, title_id) || title_id.empty() || !valid_mount_output(emuenv.mem, mountPoint))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    if (title_id == "new") {
        LOG_WARN("Writable appmeta promotion is not implemented");
        return RET_ERROR(SCE_ERROR_ERRNO_ENOSYS);
    }

    std::string mount_point;
    if (!pfsmgr::mount_readonly_appmeta(emuenv.io, emuenv.vita_fs_path, title_id, mount_point)) {
        LOG_WARN("Cannot mount appmeta for '{}'", title_id);
        return RET_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }
    if (mount_point.size() >= 16 || !valid_mount_sfo(emuenv, emuenv.io, mount_point, "/param.sfo", title_id, export_name)) {
        LOG_WARN("Invalid or encrypted appmeta SFO for '{}'", title_id);
        if (!pfsmgr::unmount_appmeta(emuenv.io, mount_point))
            LOG_ERROR("Failed to release rejected appmeta mount '{}'", mount_point);
        return RET_ERROR(SCE_ERROR_ERRNO_EINVAL);
    }
    for (Address i = 0; i < 16; ++i)
        *Ptr<char>(mountPoint.address() + i).get(emuenv.mem) = i < mount_point.size() ? mount_point[i] : '\0';
    LOG_INFO("Mounted appmeta with readable SFO for '{}' at '{}'", title_id, mount_point);
    return 0;
}

EXPORT(int, _sceAppMgrAppMount) {
    TRACY_FUNC(_sceAppMgrAppMount);
    return UNIMPLEMENTED();
}

static int write_app_param(MemState &mem, Address address, std::span<const uint8_t> bytes) {
    const GuestMemoryRange range{ address, static_cast<uint32_t>(bytes.size()) };
    const uint64_t pin = pin_guest_memory(mem, std::span(&range, 1));
    if (!pin) {
        LOG_ERROR("Invalid application parameter destination {}", log_hex(address));
        return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    }
    for (size_t i = 0; i < bytes.size(); ++i)
        *Ptr<uint8_t>(address + static_cast<Address>(i)).get(mem) = bytes[i];
    unpin_guest_memory(mem, pin);
    return 0;
}

int appmgr_get_app_param_int(EmuEnvState &emuenv, SceUID thread_id, SceUID pid, int param, Ptr<SceInt32> value) {
    if (!value)
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    if (param == 1) {
        const auto caller_pid = emuenv.kernel.get_thread_process_id(thread_id);
        if (!caller_pid)
            return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
        const SceUID target_pid = pid ? pid : *caller_pid;
        SceInt32 application_type{};
        if (uses_shell_builtin_app_params(emuenv, thread_id, pid)) {
            application_type = 2;
        } else {
            SfoFile sfo{};
            const int result = get_process_sfo(emuenv, thread_id, target_pid, sfo);
            if (result < 0)
                return result;
            const auto primary = emuenv.kernel.get_process_identity(emuenv.mem);
            if (primary && primary->pid == target_pid && primary->is_shell()) {
                application_type = 2;
            } else {
                const auto category = appmgr_sfo_category(sfo);
                if (!category) {
                    LOG_ERROR("Invalid application category for process {}", target_pid);
                    return static_cast<int>(SCE_APPMGR_ERROR_INVALID);
                }
                application_type = category->starts_with("gd") || category->starts_with("gp") ? 1 : 0;
            }
        }
        const auto thread = emuenv.kernel.get_thread(thread_id);
        if (!thread)
            return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
        const std::array<uint8_t, 4> bytes{ static_cast<uint8_t>(application_type),
            static_cast<uint8_t>(application_type >> 8), static_cast<uint8_t>(application_type >> 16),
            static_cast<uint8_t>(application_type >> 24) };
        LOG_DEBUG("AppParamGetInt pid: {}, parameter: {}, application type: {}", target_pid, param, application_type);
        return write_app_param(thread->cpu->mem, value.address(), bytes);
    }
    if (param == 16) {
        const auto thread = emuenv.kernel.get_thread(thread_id);
        if (!thread)
            return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
        const SceInt32 enter_button = emuenv.cfg.sys_button;
        const std::array<uint8_t, 4> bytes{ static_cast<uint8_t>(enter_button), static_cast<uint8_t>(enter_button >> 8),
            static_cast<uint8_t>(enter_button >> 16), static_cast<uint8_t>(enter_button >> 24) };
        LOG_DEBUG("AppParamGetInt pid: {}, parameter: {}, enter button: {}", pid, param, enter_button);
        return write_app_param(thread->cpu->mem, value.address(), bytes);
    }
    const bool is_attribute_param = param == 15 || param == 17;
    const bool is_savedata_metadata_param = param == 5 || param == 13;
    if (param != 3 && !is_attribute_param && !is_savedata_metadata_param) {
        LOG_ERROR("Application integer parameter {} is not implemented", param);
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_APP_PARAM);
    }
    if (is_attribute_param && !is_shell_caller(emuenv, thread_id))
        return static_cast<int>(SCE_APPMGR_ERROR_PERMISSION);
    if (uses_shell_builtin_app_params(emuenv, thread_id, pid)) {
        const auto thread = emuenv.kernel.get_thread(thread_id);
        if (!thread)
            return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
        const std::array<uint8_t, 4> bytes{};
        LOG_DEBUG("AppParamGetInt pid: {}, parameter: {}, Shell value: 0", pid, param);
        return write_app_param(thread->cpu->mem, value.address(), bytes);
    }
    SfoFile sfo{};
    const int result = get_process_sfo(emuenv, thread_id, pid, sfo);
    if (result < 0)
        return result;
    const auto app_param = param == 3 ? appmgr_sfo_version(sfo)
        : is_savedata_metadata_param  ? std::optional<uint32_t>{ 0 }
                                      : appmgr_sfo_attribute(sfo, param);
    if (!app_param) {
        LOG_ERROR("Invalid application parameter {} for process {}", param, pid);
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID);
    }
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const std::array<uint8_t, 4> bytes{ static_cast<uint8_t>(*app_param), static_cast<uint8_t>(*app_param >> 8),
        static_cast<uint8_t>(*app_param >> 16), static_cast<uint8_t>(*app_param >> 24) };
    if (is_savedata_metadata_param)
        LOG_DEBUG("AppParamGetInt pid: {}, parameter: {}, savedata {} flags: {}", pid, param,
            param == 5 ? "ownership" : "environment", log_hex(*app_param));
    else
        LOG_DEBUG("AppParamGetInt pid: {}, parameter: {}, value: {}", pid, param, log_hex(*app_param));
    return write_app_param(thread->cpu->mem, value.address(), bytes);
}

int appmgr_get_app_param_string(EmuEnvState &emuenv, SceUID thread_id, SceUID pid, int param, Ptr<char> output, SceSize length) {
    LOG_DEBUG("AppParamGetString request pid: {}, parameter: {}, length: {}", pid, param, length);
    if (!output)
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    if (param == 100 || param == 101) {
        LOG_WARN("Application parameter cache is not implemented; using SFO parameter {}", param);
        param = param == 100 ? 6 : 9;
    }
    if (param < 6 || param > 14)
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER2);
    if (uses_shell_builtin_app_params(emuenv, thread_id, pid)) {
        const auto shell_param = appmgr_shell_string_param(param);
        if (!shell_param)
            return static_cast<int>(SCE_APPMGR_ERROR_INVALID_APP_PARAM);
        if (length < shell_param->minimum_size)
            return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER2);
        const auto thread = emuenv.kernel.get_thread(thread_id);
        if (!thread)
            return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
        length = std::min<SceSize>(length, 1024);
        std::vector<uint8_t> bytes(length, 0);
        std::copy(shell_param->value.begin(), shell_param->value.end(), bytes.begin());
        LOG_DEBUG("AppParamGetString pid: {}, parameter: {}, Shell value: {}", pid, param, shell_param->value);
        return write_app_param(thread->cpu->mem, output.address(), bytes);
    }
    SfoFile sfo{};
    const int result = get_process_sfo(emuenv, thread_id, pid, sfo);
    if (result < 0)
        return result;
    std::string value;
    if (param == 8) {
        if (length < 4)
            return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER2);
        const auto category = appmgr_sfo_category(sfo);
        if (!category) {
            LOG_ERROR("Invalid application category for process {}", pid);
            return static_cast<int>(SCE_APPMGR_ERROR_INVALID);
        }
        value = *category;
    } else if (!sfo::get_data_by_id(value, sfo, param)) {
        LOG_ERROR("Application string parameter {} is unavailable for process {}", param, pid);
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID);
    }
    length = std::min<SceSize>(length, 1024);
    if (value.size() >= length)
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER2);
    std::vector<uint8_t> bytes(length, 0);
    std::copy(value.begin(), value.end(), bytes.begin());
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    LOG_DEBUG("AppParamGetString pid: {}, parameter: {}, value: {}", pid, param, value);
    return write_app_param(thread->cpu->mem, output.address(), bytes);
}

EXPORT(int, _sceAppMgrAppParamGetInt, int pid, int param, Ptr<SceInt32> value) {
    TRACY_FUNC(_sceAppMgrAppParamGetInt, pid, param, value);
    return appmgr_get_app_param_int(emuenv, thread_id, pid, param, value);
}

EXPORT(SceInt32, _sceAppMgrAppParamGetString, int pid, int param, Ptr<char> string, Ptr<const sceAppMgrAppParamGetStringOptParam> optParam) {
    TRACY_FUNC(_sceAppMgrAppParamGetString, pid, param, string, optParam);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    auto &mem = thread->cpu->mem;
    const GuestMemoryRange range{ optParam.address(), sizeof(sceAppMgrAppParamGetStringOptParam) };
    const uint64_t pin = pin_guest_memory(mem, std::span(&range, 1));
    if (!optParam || !pin) {
        if (pin)
            unpin_guest_memory(mem, pin);
        LOG_ERROR("Invalid application string parameter options {}", log_hex(optParam.address()));
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    }
    SceSize length = 0;
    for (size_t i = 0; i < sizeof(length); ++i)
        length |= static_cast<SceSize>(*Ptr<const uint8_t>(optParam.address() + static_cast<Address>(i)).get(mem)) << (i * 8);
    unpin_guest_memory(mem, pin);
    return appmgr_get_app_param_string(emuenv, thread_id, pid, param, string, length);
}

EXPORT(int, _sceAppMgrAppParamSetString) {
    TRACY_FUNC(_sceAppMgrAppParamSetString);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrAppUmount) {
    TRACY_FUNC(_sceAppMgrAppUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrBgdlGetQueueStatus) {
    TRACY_FUNC(_sceAppMgrBgdlGetQueueStatus);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrCaptureFrameBufDMACByAppId) {
    TRACY_FUNC(_sceAppMgrCaptureFrameBufDMACByAppId);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrCaptureFrameBufIFTUByAppId, SceInt32 app_id, Ptr<const void> frame_buf) {
    TRACY_FUNC(_sceAppMgrCaptureFrameBufIFTUByAppId, app_id, frame_buf.address());
    return appmgr_capture_framebuf_iftu_by_app_id(emuenv, thread_id, app_id, frame_buf);
}

EXPORT(int, _sceAppMgrCheckRifGD, Ptr<const char> path, Ptr<void> info) {
    TRACY_FUNC(_sceAppMgrCheckRifGD, path.address(), info.address());
    constexpr size_t info_size = 0x70;
    if (!path || !info)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    MemState &mem = caller->cpu->mem;
    if (info.address() > std::numeric_limits<Address>::max() - (info_size - 1))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    for (size_t i = 0; i < info_size; ++i) {
        if (!Ptr<uint8_t>(info.address() + i).valid(mem))
            return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    }

    std::memset(info.get(mem), 0, info_size);

    std::string app_path;
    if (!read_mount_string(mem, path, 0x123, app_path))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);

    if (app_path.empty()) {
        const int result = RET_ERROR(SCE_APPMGR_ERROR_INVALID_APP_PATH);
        LOG_INFO("CheckRifGD returned {} for an empty path; license info remains zeroed", log_hex(result));
        return result;
    }

    LOG_DEBUG("CheckRifGD accepted path '{}' with zeroed info; RIF parsing is not emulated", app_path);
    return 0;
}

EXPORT(int, _sceAppMgrContentInstallPeriodStart) {
    TRACY_FUNC(_sceAppMgrContentInstallPeriodStart);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrContentInstallPeriodStop) {
    TRACY_FUNC(_sceAppMgrContentInstallPeriodStop);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrConvertVs0UserDrivePath) {
    TRACY_FUNC(_sceAppMgrConvertVs0UserDrivePath);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrDeclareShellProcess2) {
    TRACY_FUNC(_sceAppMgrDeclareShellProcess2);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrDestroyAppByName, Ptr<const char> name) {
    TRACY_FUNC(_sceAppMgrDestroyAppByName, name.address());
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const auto caller_pid = emuenv.kernel.get_thread_process_id(thread_id);
    if (!caller_pid)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    if (!is_shell_caller(emuenv, thread_id)
        && CALL_EXPORT(SceSblACMgrForDriver_IsSystemProgram, 0) != 1)
        return RET_ERROR(SCE_APPMGR_ERROR_PERMISSION);

    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot destroy an app by name without AppMgr process state");
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    }
    std::string app_name;
    if (name && !read_mount_string(caller->cpu->mem, name, 32, app_name))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);

    const bool destroy_caller = app_name.empty();
    SceUID app_id = 0;
    {
        const std::lock_guard lock(processes->mutex);
        const auto it = std::find_if(processes->processes.begin(), processes->processes.end(),
            [&](const auto &process) {
                return process->app_id && (destroy_caller ? process->pid == *caller_pid : process->title_id == app_name);
            });
        if (it == processes->processes.end())
            return RET_ERROR(SCE_APPMGR_ERROR_APP_NOT_FOUND);
        if ((*it)->state == SecondaryProcess::State::starting)
            return RET_ERROR(SCE_APPMGR_ERROR_BUSY);
        app_id = (*it)->app_id;
        if (std::ranges::find(processes->pending_destroy_app_ids, app_id) != processes->pending_destroy_app_ids.end())
            return 0;
        if (processes->shell_events.size() + processes->reserved_shell_events >= 179)
            return RET_ERROR(SCE_APPMGR_ERROR_BUSY);
        try {
            processes->pending_destroy_app_ids.push_back(app_id);
            try {
                SceAppMgrShellEvent event{};
                event.event = 18;
                event.data.app_request.app_id = app_id;
                processes->shell_events.push_back(event);
            } catch (const std::bad_alloc &) {
                processes->pending_destroy_app_ids.pop_back();
                throw;
            }
        } catch (const std::bad_alloc &) {
            LOG_ERROR("Could not queue destruction of app {}", log_hex(app_id));
            return RET_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
        }
    }

    if (destroy_caller)
        caller->exit(0);
    return 0;
}

EXPORT(int, _sceAppMgrDrmClose, const SceAppMgrDrmOpenParam *param) {
    TRACY_FUNC(_sceAppMgrDrmClose, param);
    if (!param || param->size != sizeof(*param)
        || !std::memchr(param->dir_name, '\0', sizeof(param->dir_name))
        || !std::memchr(param->mount_point, '\0', sizeof(param->mount_point)))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    const auto context = get_process_io_context(emuenv, thread_id, "DrmClose");
    if (!context)
        return RET_ERROR(context.error());
    return pfsmgr::unmount_addcont(*context->io, param->mount_point, param->dir_name);
}

EXPORT(int, _sceAppMgrDrmOpen, const SceAppMgrDrmOpenParam *param) {
    TRACY_FUNC(_sceAppMgrDrmOpen, param);
    if (!param || param->size != sizeof(*param)
        || !std::memchr(param->dir_name, '\0', sizeof(param->dir_name))
        || !std::memchr(param->mount_point, '\0', sizeof(param->mount_point)))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    const auto context = get_process_io_context(emuenv, thread_id, "DrmOpen");
    if (!context)
        return RET_ERROR(context.error());
    return pfsmgr::mount_addcont(*context->io, emuenv.vita_fs_path, param->mount_point, param->dir_name);
}

EXPORT(int, _sceAppMgrForceUmount) {
    TRACY_FUNC(_sceAppMgrForceUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGameDataMount, Ptr<const char> source, Ptr<const char> patch, Ptr<const void> license, Ptr<char> mountPoint) {
    TRACY_FUNC(_sceAppMgrGameDataMount, source.address(), patch.address(), license.address(), mountPoint.address());
    const auto context = get_process_io_context(emuenv, thread_id, "GameDataMount");
    if (!context)
        return RET_ERROR(context.error());
    MemState &mem = context->thread->cpu->mem;
    IOState &io = *context->io;
    std::string source_path;
    std::string patch_path;
    if (!read_mount_string(mem, source, 0x123, source_path) || !valid_mount_output(mem, mountPoint)
        || (patch && !read_mount_string(mem, patch, 0x123, patch_path)))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    if (license) {
        LOG_WARN("GameDataMount with an explicit license is not implemented");
        return RET_ERROR(SCE_ERROR_ERRNO_ENOSYS);
    }
    if (source_path.starts_with("ux0:/") || source_path.starts_with("pd0:/"))
        source_path.erase(4, 1);
    if (patch_path.starts_with("ux0:/"))
        patch_path.erase(4, 1);
    if (source_path.empty()) {
        LOG_WARN("GameDataMount source path is empty");
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_APP_PATH);
    }
    const bool user_app = source_path.starts_with("ux0:app/");
    const bool firmware_app = source_path.starts_with("pd0:app/");
    if ((!user_app && !firmware_app) || source_path.size() <= 8) {
        LOG_WARN("Unsupported GameDataMount source '{}'", source_path);
        return RET_ERROR(SCE_ERROR_ERRNO_ENOSYS);
    }
    const auto title_id = source_path.substr(8);
    if (user_app && patch && patch_path != "ux0:patch/" + title_id) {
        LOG_WARN("Unsupported GameDataMount patch '{}'", patch_path);
        return RET_ERROR(SCE_ERROR_ERRNO_ENOSYS);
    }
    if (firmware_app && !patch_path.empty()) {
        LOG_WARN("GameDataMount does not support patches for firmware app '{}'", title_id);
        return RET_ERROR(SCE_ERROR_ERRNO_ENOSYS);
    }
    std::string mount_point;
    const bool mounted = firmware_app
        ? pfsmgr::mount_readonly_firmware_app_data(io, emuenv.vita_fs_path, title_id, mount_point)
        : pfsmgr::mount_readonly_game_data(io, emuenv.vita_fs_path, title_id, bool(patch), mount_point);
    if (!mounted) {
        LOG_WARN("Cannot mount game data for '{}'", title_id);
        return RET_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }
    if (!valid_mount_sfo(emuenv, io, mount_point, "/sce_sys/param.sfo", title_id, export_name)) {
        LOG_WARN("Invalid or encrypted game data SFO for '{}'", title_id);
        if (!pfsmgr::unmount_game_data(io, mount_point))
            LOG_ERROR("Failed to release rejected game data mount '{}'", mount_point);
        return RET_ERROR(SCE_ERROR_ERRNO_EINVAL);
    }
    for (Address i = 0; i < 16; ++i)
        *Ptr<char>(mountPoint.address() + i).get(mem) = i < mount_point.size() ? mount_point[i] : '\0';
    LOG_INFO("Mounted game data for '{}' at '{}' in read-only mode", title_id, mount_point);
    return 0;
}

EXPORT(int, _sceAppMgrGetAppInfo) {
    TRACY_FUNC(_sceAppMgrGetAppInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetAppMgrState) {
    TRACY_FUNC(_sceAppMgrGetAppMgrState);
    return UNIMPLEMENTED();
}

// The size sceAppMgrGetAppParam writes at most, without a terminator if the URI fills it
constexpr size_t APP_PARAM_SIZE = 1024;

EXPORT(SceInt32, _sceAppMgrGetAppParam, Ptr<char> param) {
    TRACY_FUNC(_sceAppMgrGetAppParam, param);
    if (!param)
        return RET_ERROR(SCE_APPMGR_ERROR_NULL_POINTER);

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    MemState &mem = caller->cpu->mem;
    if (emuenv.kernel.is_primary_address_space(mem)) {
        const std::optional<std::string> uri = emuenv.take_launch_uri();
        const char *value = uri ? uri->c_str() : "";
        const size_t size = std::min(uri ? uri->size() + 1 : size_t{ 1 }, APP_PARAM_SIZE);
        if (!write_guest_memory(mem, param.address(), value, static_cast<uint32_t>(size)))
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        return uri ? 0 : SCE_APPMGR_ERROR_APP_PARAM_READ;
    }

    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    std::string launch_param;
    bool already_read = false;
    bool manual_app = false;
    {
        const std::lock_guard lock(processes->mutex);
        const auto it = std::find_if(processes->processes.begin(), processes->processes.end(),
            [&mem](const auto &process) { return &process->mem == &mem; });
        if (it == processes->processes.end())
            return RET_ERROR(SCE_APPMGR_ERROR_APP_NOT_FOUND);
        manual_app = (*it)->title_id == "NPXS10027";
        already_read = (*it)->launch_param_read;
        if (!already_read) {
            (*it)->launch_param_read = true;
            launch_param = (*it)->launch_param;
        }
    }
    const char *value = launch_param.c_str();
    const size_t size = std::min(launch_param.size() + 1, APP_PARAM_SIZE);
    if (!write_guest_memory(mem, param.address(), value, static_cast<uint32_t>(size)))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    if (manual_app)
        LOG_INFO("GetAppParam for NPXS10027 wrote '{}' to {}; already read: {}", launch_param, log_hex(param.address()), already_read);
    return already_read ? SCE_APPMGR_ERROR_APP_PARAM_READ : 0;
}

EXPORT(int, _sceAppMgrGetAppParam2) {
    TRACY_FUNC(_sceAppMgrGetAppParam2);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetBootParam) {
    TRACY_FUNC(_sceAppMgrGetBootParam);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetBudgetInfo) {
    TRACY_FUNC(_sceAppMgrGetBudgetInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetCoredumpStateForShell) {
    TRACY_FUNC(_sceAppMgrGetCoredumpStateForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetCurrentBgmState) {
    TRACY_FUNC(_sceAppMgrGetCurrentBgmState);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetCurrentBgmState2, AppMgrBgmState *state) {
    TRACY_FUNC(_sceAppMgrGetCurrentBgmState2, state);
    if (!state)
        return RET_ERROR(SCE_APPMGR_ERROR_NULL_POINTER);
    const auto pid = emuenv.kernel.get_thread_process_id(thread_id);
    if (!pid)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const auto bgm_state = get_appmgr_bgm_state(emuenv, *pid);
    if (!bgm_state) {
        LOG_ERROR("{}: BGM state is not initialized", export_name);
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    }
    *state = *bgm_state;
    return 0;
}

EXPORT(int, _sceAppMgrGetDevInfo) {
    TRACY_FUNC(_sceAppMgrGetDevInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetFgAppInfo) {
    TRACY_FUNC(_sceAppMgrGetFgAppInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetIdByName) {
    TRACY_FUNC(_sceAppMgrGetIdByName);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetMediaTypeFromDrive) {
    TRACY_FUNC(_sceAppMgrGetMediaTypeFromDrive);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetMediaTypeFromDriveByPid) {
    TRACY_FUNC(_sceAppMgrGetMediaTypeFromDriveByPid);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetMountProcessNum) {
    TRACY_FUNC(_sceAppMgrGetMountProcessNum);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetNameById) {
    TRACY_FUNC(_sceAppMgrGetNameById);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetPfsDrive) {
    TRACY_FUNC(_sceAppMgrGetPfsDrive);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetPidListForShell) {
    TRACY_FUNC(_sceAppMgrGetPidListForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetRawPath) {
    TRACY_FUNC(_sceAppMgrGetRawPath);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetRawPathOfApp0ByAppIdForShell) {
    TRACY_FUNC(_sceAppMgrGetRawPathOfApp0ByAppIdForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetRawPathOfApp0ByPidForShell) {
    TRACY_FUNC(_sceAppMgrGetRawPathOfApp0ByPidForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetRecommendedScreenOrientation) {
    TRACY_FUNC(_sceAppMgrGetRecommendedScreenOrientation);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetRunningAppIdListForShell, Ptr<SceInt32> app_ids, SceUInt32 count, Ptr<const SceUInt32> arguments) {
    TRACY_FUNC(_sceAppMgrGetRunningAppIdListForShell, app_ids, count, arguments);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    auto &mem = thread->cpu->mem;
    const GuestMemoryRange range{ arguments.address(), 2 * sizeof(SceUInt32) };
    const uint64_t pin = arguments ? pin_guest_memory(mem, std::span(&range, 1)) : 0;
    if (!pin) {
        LOG_ERROR("Invalid running application list syscall arguments {}", log_hex(arguments.address()));
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    }
    SceUInt32 size = 0;
    for (size_t i = 0; i < sizeof(size); ++i)
        size |= static_cast<SceUInt32>(*Ptr<const uint8_t>(arguments.address() + static_cast<Address>(i)).get(mem)) << (i * 8);
    unpin_guest_memory(mem, pin);
    if (count > 20 || size != count * sizeof(SceInt32)) {
        LOG_ERROR("Invalid running application list syscall size {} for capacity {}", size, count);
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER2);
    }
    return appmgr_get_running_app_id_list(emuenv, thread_id, app_ids, count);
}

EXPORT(int, _sceAppMgrGetSaveDataInfo) {
    TRACY_FUNC(_sceAppMgrGetSaveDataInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetSaveDataInfoForSpecialExport) {
    TRACY_FUNC(_sceAppMgrGetSaveDataInfoForSpecialExport);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetStatusByAppId) {
    TRACY_FUNC(_sceAppMgrGetStatusByAppId);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetStatusById) {
    TRACY_FUNC(_sceAppMgrGetStatusById);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetStatusByName) {
    TRACY_FUNC(_sceAppMgrGetStatusByName);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetSystemDataFilePlayReady) {
    TRACY_FUNC(_sceAppMgrGetSystemDataFilePlayReady);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetUserDirPath) {
    TRACY_FUNC(_sceAppMgrGetUserDirPath);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetUserDirPathById) {
    TRACY_FUNC(_sceAppMgrGetUserDirPathById);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetVs0UserDataDrive) {
    TRACY_FUNC(_sceAppMgrGetVs0UserDataDrive);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrGetVs0UserModuleDrive) {
    TRACY_FUNC(_sceAppMgrGetVs0UserModuleDrive);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrInitSafeMemoryById) {
    TRACY_FUNC(_sceAppMgrInitSafeMemoryById);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrInstallDirMount) {
    TRACY_FUNC(_sceAppMgrInstallDirMount);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrIsCameraActive) {
    TRACY_FUNC(_sceAppMgrIsCameraActive);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrLaunchAppByName) {
    TRACY_FUNC(_sceAppMgrLaunchAppByName);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrLaunchAppByName2, Ptr<const char> name, Ptr<const char> param, Ptr<const void> option) {
    TRACY_FUNC(_sceAppMgrLaunchAppByName2, name, param, option.address());
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        LOG_ERROR("LaunchAppByName2: unknown thread {}", thread_id);
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    }

    const MemState &mem = thread->cpu->mem;
    LOG_INFO("LaunchAppByName2 arguments: name='{}', param='{}', option={}",
        appmgr_mount_log_string(mem, name, 31), appmgr_mount_log_string(mem, param, APP_PARAM_SIZE - 1),
        log_hex(option.address()));

    std::string app_name;
    if (!read_mount_string(mem, name, 31, app_name) || app_name.empty()
        || !std::ranges::all_of(app_name, [](unsigned char ch) {
               return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
           })) {
        LOG_ERROR("LaunchAppByName2 received an invalid application name");
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    }

    std::string launch_param;
    if (param && !read_mount_string(mem, param, APP_PARAM_SIZE - 1, launch_param)) {
        LOG_ERROR("LaunchAppByName2 received an invalid or oversized launch parameter");
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    }

    SceUInt32 launch_mode = 0;
    if (option) {
        std::array<SceUInt32, 16> option_words{};
        const GuestMemoryRange range{ option.address(), sizeof(option_words) };
        const uint64_t pin = pin_guest_memory(thread->cpu->mem, std::span(&range, 1));
        if (!pin) {
            LOG_ERROR("LaunchAppByName2 received an invalid option block at {}", log_hex(option.address()));
            return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        }
        auto *option_bytes = reinterpret_cast<uint8_t *>(option_words.data());
        for (size_t i = 0; i < sizeof(option_words); ++i)
            option_bytes[i] = *Ptr<const uint8_t>(option.address() + static_cast<Address>(i)).get(thread->cpu->mem);
        unpin_guest_memory(thread->cpu->mem, pin);

        LOG_INFO("LaunchAppByName2 option [00..1c]: {}, {}, {}, {}, {}, {}, {}, {}",
            log_hex(option_words[0]), log_hex(option_words[1]), log_hex(option_words[2]), log_hex(option_words[3]),
            log_hex(option_words[4]), log_hex(option_words[5]), log_hex(option_words[6]), log_hex(option_words[7]));
        LOG_INFO("LaunchAppByName2 option [20..3c]: {}, {}, {}, {}, {}, {}, {}, {}",
            log_hex(option_words[8]), log_hex(option_words[9]), log_hex(option_words[10]), log_hex(option_words[11]),
            log_hex(option_words[12]), log_hex(option_words[13]), log_hex(option_words[14]), log_hex(option_words[15]));

        if (std::ranges::any_of(option_words.begin() + 8, option_words.end(), [](SceUInt32 word) { return word != 0; })) {
            LOG_ERROR("LaunchAppByName2 option has non-zero reserved words");
            return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER2);
        }
        if (option_words[3] != 0) {
            LOG_DEBUG("LaunchAppByName2 source process UID {} is represented by the calling process in Path4",
                log_hex(option_words[3]));
        }
        if (option_words[4] != 0
            || ((option_words[1] & 0x840000) != 0 && option_words[5] != 0)
            || option_words[6] != 0 || option_words[7] != 0) {
            LOG_ERROR("LaunchAppByName2 requested target-process or launch options that Path4 cannot represent");
            return static_cast<int>(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
        }
        launch_mode = option_words[1];
    }

    const std::string self_path = "vs0:app/" + app_name + "/eboot.bin";
    auto path_buffer = alloc_block(thread->cpu->mem, static_cast<uint32_t>(self_path.size() + 1), "AppMgr_launch_by_name2_path");
    if (!path_buffer) {
        LOG_ERROR("LaunchAppByName2 could not allocate a path buffer for '{}'", app_name);
        return static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
    }
    if (!write_guest_memory(thread->cpu->mem, path_buffer.get(), self_path.c_str(), static_cast<uint32_t>(self_path.size() + 1))) {
        LOG_ERROR("LaunchAppByName2 could not write the path for '{}'", app_name);
        return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    }

    LOG_INFO("LaunchAppByName2 launching '{}' with parameter '{}'", app_name, launch_param);
    const int result = appmgr_launch_app_by_path(emuenv, thread_id, path_buffer.get_ptr<const char>(), name,
        0, param, 0, {}, launch_mode, true, true);
    return result < 0 ? result : 0;
}

EXPORT(int, _sceAppMgrLaunchAppByName2ForShell) {
    TRACY_FUNC(_sceAppMgrLaunchAppByName2ForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrLaunchAppByName2ndStage, SceInt32 app_id, Ptr<const char> path,
    SceUInt32 budget_size, Ptr<const AppMgrLaunchAppByName2ndStageArgs> arguments) {
    TRACY_FUNC(_sceAppMgrLaunchAppByName2ndStage, app_id, path, budget_size, arguments);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const auto args = read_appmgr_launch_by_name_2nd_stage_args(thread->cpu->mem, arguments);
    if (!args)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    return appmgr_launch_app_by_name_2nd_stage(emuenv, thread_id, app_id, path,
        budget_size, args->process_attr, args->option);
}

static SceInt32 start_app_process(EmuEnvState &emuenv, SecondaryProcess &process) {
    if (!process.preloads_started) {
        const auto modules = emuenv.kernel.get_process_start_modules(process.mem, process.main_module_id);
        if (!modules) {
            LOG_ERROR("Cannot start preloaded modules for process {}", process.pid);
            return SCE_KERNEL_ERROR_ERROR;
        }
        for (const auto &module : modules->preloaded) {
            const auto lease = emuenv.kernel.acquire_module_start(process.mem, module);
            if (!lease) {
                LOG_ERROR("Preloaded module {} disappeared from process {}", module->info.modid, process.pid);
                return SCE_KERNEL_ERROR_ERROR;
            }
            const auto result = start_module(emuenv, *module);
            if (result != SCE_KERNEL_START_SUCCESS) {
                LOG_ERROR("Preloaded module {} failed to start in process {}: {}", module->info.module_name, process.pid, log_hex(result));
                return SCE_KERNEL_ERROR_ERROR;
            }
        }
        process.preloads_started = true;
    }
    const ThreadStatePtr main_thread = emuenv.kernel.get_thread(process.main_thread_id);
    if (!main_thread) {
        LOG_ERROR("Cannot start process {} without its main thread", process.pid);
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    }
    schedule_proc_event_create_callback(emuenv, main_thread, process.pid);
    return emuenv.kernel.start_process_main_thread(main_thread, process.arguments);
}

static SceInt32 appmgr_activate_app(EmuEnvState &emuenv, SceInt32 app_id) {
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot activate an app without AppMgr process state");
        return SCE_KERNEL_ERROR_ERROR;
    }
    LOG_DEBUG("Shell requested activation of app {}", log_hex(app_id));
    const std::lock_guard activation_lock(processes->activation_mutex);
    SecondaryProcess *process = nullptr;
    size_t reserved_event_count = 1;
    {
        const std::lock_guard lock(processes->mutex);
        for (const auto &candidate : processes->processes) {
            if (candidate->app_id == app_id && app_id != 0) {
                process = candidate.get();
                break;
            }
        }
        if (!process)
            return SCE_APPMGR_ERROR_APP_NOT_FOUND;
        if (process->state == SecondaryProcess::State::starting)
            return SCE_APPMGR_ERROR_BUSY;
        if (process->state == SecondaryProcess::State::start_failed)
            return SCE_KERNEL_ERROR_ERROR;
        if (process->state == SecondaryProcess::State::started) {
            auto *bgm = emuenv.kernel.obj_store.get<AppMgrBgm>();
            const bool resumed_bgm = bgm && bgm->resume(process->pid);
            const int result = resume_secondary_process_threads(*process);
            if (result < 0) {
                if (resumed_bgm)
                    bgm->suspend(process->pid);
                return result;
            }
            if (resumed_bgm)
                LOG_DEBUG("Restored BGM port for resumed app {}", log_hex(app_id));
            for (const auto &candidate : processes->processes)
                candidate->activated = candidate.get() == process;
            if (display::select_process_frame(emuenv.display, process->pid))
                emuenv.renderer->should_display = true;
            return 0;
        }
        reserved_event_count += process->initial_app_id != process->app_id;
        if (processes->shell_events.size() + processes->reserved_shell_events > 180 - reserved_event_count) {
            LOG_ERROR("Cannot activate app {}: Shell event queue is full", log_hex(app_id));
            return SCE_APPMGR_ERROR_BUSY;
        }
        processes->reserved_shell_events += reserved_event_count;
        process->state = SecondaryProcess::State::starting;
    }
    const SceInt32 result = start_app_process(emuenv, *process);
    {
        const std::lock_guard lock(processes->mutex);
        processes->reserved_shell_events -= reserved_event_count;
        process->state = result < 0 ? (process->preloads_started ? SecondaryProcess::State::created : SecondaryProcess::State::start_failed) : SecondaryProcess::State::started;
        if (result >= 0) {
            for (const auto &candidate : processes->processes)
                candidate->activated = candidate.get() == process;
            SceAppMgrShellEvent event{};
            event.event = 18;
            event.data.app_request.app_id = app_id;
            processes->shell_events.push_back(event);
            LOG_DEBUG("Queued Shell app-running-status-changed event for process {} and app {}", process->pid, log_hex(app_id));
            if (process->initial_app_id != process->app_id) {
                SceAppMgrShellEvent loaded_event{};
                loaded_event.event = 2;
                loaded_event.data.loaded.process_id = process->pid;
                loaded_event.data.loaded.app_id = process->initial_app_id;
                processes->shell_events.push_back(loaded_event);
            }
            if (display::select_process_frame(emuenv.display, process->pid))
                emuenv.renderer->should_display = true;
        }
    }
    if (result < 0) {
        LOG_ERROR("Could not activate app {} (process {}): {}", log_hex(app_id), process->pid, log_hex(result));
        return result;
    }
    LOG_DEBUG("Shell activated app {} (process {})", log_hex(app_id), process->pid);
    return 0;
}

int appmgr_launch_app_by_name_2nd_stage(EmuEnvState &emuenv, SceUID thread_id, SceInt32 app_id,
    Ptr<const char> path, SceUInt32 budget_size, SceUInt32 process_attr, Ptr<const void> option) {
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        LOG_ERROR("LaunchAppByName2ndStage: unknown thread {}", thread_id);
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    }
    if (!is_shell_caller(emuenv, thread_id)) {
        LOG_ERROR("LaunchAppByName2ndStage requires the Shell process");
        return static_cast<int>(SCE_APPMGR_ERROR_NOT_SHELL);
    }

    std::string self_path;
    if (!read_mount_string(thread->cpu->mem, path, 291, self_path)) {
        LOG_ERROR("LaunchAppByName2ndStage received an invalid path");
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    }
    if (process_attr & ~APPMGR_PROCESS_ATTR_MASK) {
        LOG_ERROR("LaunchAppByName2ndStage received unsupported process attributes {}", log_hex(process_attr));
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID);
    }
    if (budget_size != 0 && !self_path.starts_with("vs0:")) {
        LOG_ERROR("LaunchAppByName2ndStage cannot apply a memory budget to non-firmware app '{}'", self_path);
        return static_cast<int>(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
    }
    if (budget_size != 0)
        LOG_WARN("LaunchAppByName2ndStage ignores requested memory budget {} for firmware app '{}' because per-process budgets are not emulated",
            log_hex(budget_size), self_path);

    std::optional<AppMgrLaunchOption> launch_option;
    if (option) {
        launch_option = read_appmgr_launch_option(thread->cpu->mem, option);
        if (!launch_option) {
            LOG_ERROR("LaunchAppByName2ndStage received an invalid option block at {}", log_hex(option.address()));
            return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        }
        if (!appmgr_launch_option_supported(*launch_option, true)) {
            LOG_ERROR("LaunchAppByName2ndStage unsupported options: flags {}, param result {}, proxy app name present {}, common dialog {}, extra memory {}",
                log_hex(launch_option->flags), log_hex(launch_option->param_result.address()), launch_option->proxy_app_name[0] != '\0',
                log_hex(launch_option->use_common_dialog), log_hex(launch_option->enlarge_game_memory_size));
            return static_cast<int>(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
        }
        if (launch_option->physical_contiguous_size)
            LOG_WARN("LaunchAppByName2ndStage ignores requested physical contiguous memory {} because it is not emulated",
                log_hex(launch_option->physical_contiguous_size));
    }

    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot complete a Shell launch without AppMgr process state");
        return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
    }
    {
        const std::lock_guard lock(processes->mutex);
        const auto it = std::find_if(processes->processes.begin(), processes->processes.end(),
            [app_id](const auto &process) { return app_id != 0 && process->app_id == app_id; });
        if (it == processes->processes.end())
            return static_cast<int>(SCE_APPMGR_ERROR_APP_NOT_FOUND);
        if ((*it)->state == SecondaryProcess::State::starting)
            return static_cast<int>(SCE_APPMGR_ERROR_BUSY);
        if ((*it)->state != SecondaryProcess::State::created) {
            LOG_ERROR("LaunchAppByName2ndStage received app {} in state {}", log_hex(app_id), static_cast<int>((*it)->state));
            return static_cast<int>(SCE_APPMGR_ERROR_STATE);
        }
        if ((*it)->path != self_path) {
            LOG_ERROR("LaunchAppByName2ndStage path '{}' does not match pending app {} path '{}'",
                self_path, log_hex(app_id), (*it)->path);
            return static_cast<int>(SCE_APPMGR_ERROR_INVALID_SELF_PATH);
        }
        const SceInt32 started_app_id = app_id | 0x1000;
        if (started_app_id == app_id) {
            LOG_ERROR("LaunchAppByName2ndStage received app {} with the started-app ID bit already set", log_hex(app_id));
            return static_cast<int>(SCE_APPMGR_ERROR_STATE);
        }
        if (std::ranges::any_of(processes->processes, [started_app_id](const auto &process) {
                return process->app_id == started_app_id;
            })) {
            LOG_ERROR("LaunchAppByName2ndStage cannot assign duplicate app ID {}", log_hex(started_app_id));
            return static_cast<int>(SCE_APPMGR_ERROR_BUSY);
        }
        (*it)->app_id = started_app_id;
        (*it)->launch_status.app_id = started_app_id;
        (*it)->launch_status.process_attr = process_attr;
        if (launch_option) {
            (*it)->launch_status.infobar = launch_option->infobar;
            (*it)->launch_status.launch_argument_marker = launch_option->launch_argument_marker;
            (*it)->launch_status.launch_origin = launch_option->launch_origin == 1 ? 1 : 0;
        }
        app_id = started_app_id;
    }

    const int result = appmgr_activate_app(emuenv, app_id);
    if (result < 0)
        return result;

    if (launch_option && launch_option->status) {
        const auto status = find_secondary_app_by_id(emuenv, app_id);
        if (!status || !write_appmgr_launch_status(thread->cpu->mem, launch_option->status, status->status)) {
            LOG_ERROR("LaunchAppByName2ndStage could not write status for app {}", log_hex(app_id));
            return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        }
    }
    return 0;
}

int appmgr_overwrite_launch_param_for_shell(EmuEnvState &emuenv, SceUID thread_id, SceInt32 app_id, Ptr<const char> param) {
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    if (!is_shell_caller(emuenv, thread_id))
        return static_cast<int>(SCE_APPMGR_ERROR_NOT_SHELL);
    std::string launch_param;
    if (!read_mount_string(thread->cpu->mem, param, APP_PARAM_SIZE - 1, launch_param)) {
        LOG_ERROR("OverwriteLaunchParamForShell received an invalid or oversized launch parameter");
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    }
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
    const std::lock_guard lock(processes->mutex);
    const auto it = std::find_if(processes->processes.begin(), processes->processes.end(),
        [app_id](const auto &process) { return app_id != 0 && process->app_id == app_id; });
    if (it == processes->processes.end())
        return static_cast<int>(SCE_APPMGR_ERROR_APP_NOT_FOUND);
    if ((*it)->state != SecondaryProcess::State::created)
        return static_cast<int>(SCE_APPMGR_ERROR_STATE);
    (*it)->launch_param = launch_param;
    if ((*it)->arguments.empty())
        (*it)->arguments.push_back(std::move(launch_param));
    else
        (*it)->arguments.front() = std::move(launch_param);
    return 0;
}

int appmgr_peek_launch_param_for_shell(EmuEnvState &emuenv, SceUID thread_id, SceInt32 app_id, Ptr<char> output) {
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    if (!is_shell_caller(emuenv, thread_id))
        return static_cast<int>(SCE_APPMGR_ERROR_NOT_SHELL);
    if (!output)
        return static_cast<int>(SCE_APPMGR_ERROR_NULL_POINTER);
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
    std::string launch_param;
    {
        const std::lock_guard lock(processes->mutex);
        const auto it = std::find_if(processes->processes.begin(), processes->processes.end(),
            [app_id](const auto &process) { return app_id != 0 && process->app_id == app_id; });
        if (it == processes->processes.end())
            return static_cast<int>(SCE_APPMGR_ERROR_APP_NOT_FOUND);
        launch_param = (*it)->launch_param;
    }
    if (!write_guest_memory(thread->cpu->mem, output.address(), launch_param.c_str(), static_cast<uint32_t>(launch_param.size() + 1))) {
        LOG_ERROR("PeekLaunchParamForShell received an invalid output buffer at {}", log_hex(output.address()));
        return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    }
    return 0;
}

int appmgr_launch_app_by_name_for_shell(EmuEnvState &emuenv, SceUID thread_id, int flags,
    Ptr<const char> name, Ptr<const char> param) {
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        LOG_ERROR("LaunchAppByNameForShell: unknown thread {}", thread_id);
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    }

    const auto caller = emuenv.kernel.get_process_identity(thread->cpu->mem);
    if (!caller || caller->app_path != "vs0:vsh/shell") {
        LOG_ERROR("LaunchAppByNameForShell requires the Shell process");
        return static_cast<int>(SCE_APPMGR_ERROR_NOT_SHELL);
    }

    std::string app_name;
    if (!read_mount_string(thread->cpu->mem, name, 31, app_name) || app_name.empty()
        || !std::ranges::all_of(app_name, [](unsigned char ch) {
               return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
           })) {
        LOG_ERROR("LaunchAppByNameForShell received an invalid application name");
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    }

    LOG_INFO("LaunchAppByNameForShell flags: {}, name: {}, param: {}",
        log_hex(flags), app_name, log_hex(param.address()));

    const std::string self_path = "vs0:app/" + app_name + "/eboot.bin";
    auto path_buffer = alloc_block(thread->cpu->mem, static_cast<uint32_t>(self_path.size() + 1), "AppMgr_launch_path");
    if (!path_buffer) {
        LOG_ERROR("LaunchAppByNameForShell could not allocate a path buffer for '{}'", app_name);
        return static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
    }
    if (!write_guest_memory(thread->cpu->mem, path_buffer.get(), self_path.c_str(), static_cast<uint32_t>(self_path.size() + 1))) {
        LOG_ERROR("LaunchAppByNameForShell could not write the path for '{}'", app_name);
        return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    }

    const int process_id = appmgr_launch_app_by_path(emuenv, thread_id, path_buffer.get_ptr<const char>(), name,
        0, param, 0, {}, static_cast<SceUInt32>(flags), true);
    if (process_id < 0)
        return process_id;

    const auto app = find_secondary_app_by_name(emuenv, app_name);
    if (!app || app->status.process_id != process_id) {
        LOG_ERROR("LaunchAppByNameForShell created process {} for '{}' without an AppMgr status", process_id, app_name);
        return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
    }
    LOG_DEBUG("LaunchAppByNameForShell created process {} for '{}' with app ID {}", process_id, app_name, app->status.app_id);
    return app->status.app_id;
}

EXPORT(int, _sceAppMgrLaunchAppByNameForShell, int flags, Ptr<const char> name,
    Ptr<const char> param, Ptr<void> arguments) {
    TRACY_FUNC(_sceAppMgrLaunchAppByNameForShell, flags, name, param, arguments);
    return appmgr_launch_app_by_name_for_shell(emuenv, thread_id, flags, name, param);
}

int appmgr_launch_app_by_path(EmuEnvState &emuenv, SceUID thread_id, Ptr<const char> path, Ptr<const char> title_id,
    SceUInt32 budget_size, Ptr<const char> param, SceUInt32 process_attr, Ptr<const void> option,
    SceUInt32 launch_mode, bool queue_launch_by_name_event, bool allow_firmware_caller) {
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        LOG_ERROR("LaunchAppByPath4: unknown thread {}", thread_id);
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    }
    LOG_DEBUG("LaunchAppByPath4 path: {}, title: {}, budget: {}, param: {}, process attributes: {}, option: {}",
        appmgr_mount_log_string(thread->cpu->mem, path, 292), appmgr_mount_log_string(thread->cpu->mem, title_id, 32),
        log_hex(budget_size), log_hex(param.address()), log_hex(process_attr), log_hex(option.address()));
    const auto caller = emuenv.kernel.get_process_identity(thread->cpu->mem);
    if (!caller || (!caller->is_shell() && !(allow_firmware_caller && caller->is_firmware_app()))) {
        LOG_ERROR("LaunchAppByPath4 caller is not allowed to launch apps");
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID);
    }
    std::string self_path;
    std::string name;
    if (!read_mount_string(thread->cpu->mem, path, 291, self_path)
        || !read_mount_string(thread->cpu->mem, title_id, 31, name) || name.empty()) {
        LOG_ERROR("LaunchAppByPath4 received an invalid path or application name");
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    }
    const auto separator = self_path.rfind('/');
    const bool ux0_app = appmgr_is_basic_app_launch(self_path, "ux0", name);
    const bool pd0_app = appmgr_is_basic_app_launch(self_path, "pd0", name);
    const bool firmware_app = self_path.starts_with("vs0:") || pd0_app;
    if ((!firmware_app && !ux0_app) || separator == std::string::npos
        || separator <= 4 || separator + 1 == self_path.size() || self_path.find("..") != std::string::npos) {
        LOG_ERROR("LaunchAppByPath4 cannot create a process for '{}'", self_path);
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_SELF_PATH);
    }
    std::string launch_param;
    if (param && !read_mount_string(thread->cpu->mem, param, APP_PARAM_SIZE - 1, launch_param)) {
        LOG_ERROR("LaunchAppByPath4 received an invalid or oversized launch parameter");
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    }

    LOG_DEBUG("LaunchAppByPath4 self path: {}, title ID: {}, launch param: '{}'",
        self_path, name, launch_param);

    if (process_attr & ~APPMGR_PROCESS_ATTR_MASK) {
        LOG_ERROR("LaunchAppByPath4 received unsupported process attributes {}", log_hex(process_attr));
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID);
    }
    if (budget_size != 0) {
        if (!firmware_app) {
            LOG_ERROR("LaunchAppByPath4 cannot apply a memory budget to non-firmware app '{}'", name);
            return static_cast<int>(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
        }
        LOG_WARN("LaunchAppByPath4 ignores requested memory budget {} for firmware app '{}' because per-process budgets are not emulated",
            log_hex(budget_size), name);
    }
    std::optional<AppMgrLaunchOption> launch_option;
    if (option) {
        launch_option = read_appmgr_launch_option(thread->cpu->mem, option);
        if (!launch_option) {
            LOG_ERROR("LaunchAppByPath4 received an invalid option block at {}", log_hex(option.address()));
            return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        }
        LOG_DEBUG("LaunchAppByPath4 option: size {}, flags {}, status {}, param result {}, physical {}, infobar {}, marker {}, origin {}, common dialog {}, extra memory {}",
            log_hex(launch_option->size), log_hex(launch_option->flags), log_hex(launch_option->status.address()),
            log_hex(launch_option->param_result.address()), log_hex(launch_option->physical_contiguous_size), log_hex(launch_option->infobar),
            log_hex(launch_option->launch_argument_marker), log_hex(launch_option->launch_origin),
            launch_option->use_common_dialog, launch_option->enlarge_game_memory_size);
        if (!appmgr_launch_option_supported(*launch_option)) {
            LOG_ERROR("LaunchAppByPath4 requested unsupported non-default options");
            return static_cast<int>(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
        }
        if (launch_option->physical_contiguous_size)
            LOG_WARN("LaunchAppByPath4 ignores requested physical contiguous memory {} for '{}' because it is not emulated",
                log_hex(launch_option->physical_contiguous_size), name);
        if (launch_option->infobar || launch_option->launch_argument_marker || launch_option->launch_origin)
            LOG_WARN("LaunchAppByPath4 infobar, argument marker and launch origin are not yet emulated");
    }

    auto &processes = *emuenv.kernel.obj_store.get<AppMgrProcesses>();
    std::unique_lock process_lock(processes.mutex);
    const size_t required_shell_events = queue_launch_by_name_event ? 2 : 1;
    if (processes.processes.size() >= 19
        || processes.shell_events.size() + processes.reserved_shell_events + required_shell_events > 180) {
        LOG_ERROR("LaunchAppByPath4 has no free application slot or Shell event slot");
        return static_cast<int>(SCE_APPMGR_ERROR_BUSY);
    }
    for (const auto &existing : processes.processes) {
        if (existing->title_id == name) {
            LOG_ERROR("LaunchAppByPath4 cannot deliver a second launch to process {} yet", existing->pid);
            return static_cast<int>(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
        }
    }

    auto process = std::make_unique<SecondaryProcess>();
    process->path = self_path;
    process->title_id = name;
    if (param)
        process->arguments.push_back(launch_param);
    process->launch_param = std::move(launch_param);
    process->io.app_path = self_path.substr(0, separator);
    process->io.title_id = name;
    process->io.user_id = emuenv.io.user_id;
    process->io.user_name = emuenv.io.user_name;
    process->io.redirect_stdio = emuenv.io.redirect_stdio;
    init_device_paths(process->io, emuenv.vita_fs_path);
    if (ux0_app && !pfsmgr::mount_application_game_data(process->io, emuenv.vita_fs_path)) {
        LOG_ERROR("LaunchAppByPath4 could not mount game data for '{}'", name);
        return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
    }
    const std::string sfo_path = ux0_app ? "app0:sce_sys/param.sfo" : process->io.app_path + "/sce_sys/param.sfo";
    vfs::FileBuffer sfo_buffer;
    SfoFile process_sfo{};
    if (read_file_to_buffer(process->io, sfo_path.c_str(), emuenv.vita_fs_path, sfo_buffer, "LaunchAppByPath4")
        && sfo::load(process_sfo, sfo_buffer)) {
        process->sfo = std::move(process_sfo);
    } else {
        LOG_ERROR("Could not load application parameters from '{}'", sfo_path);
        if (ux0_app)
            return static_cast<int>(SCE_APPMGR_ERROR_APP_PARAM_READ);
    }
    if (ux0_app || process->sfo) {
        const auto savedata_id = appmgr_savedata_id(*process->sfo);
        if (!savedata_id) {
            LOG_ERROR("LaunchAppByPath4 has no valid savedata identifier for '{}'", name);
            if (ux0_app)
                return static_cast<int>(SCE_APPMGR_ERROR_APP_PARAM_READ);
        } else {
            const auto &savedata = *savedata_id;
            const auto savedata_key = string_utils::toupper(savedata);
            if (emuenv.io.user_id == process->io.user_id && string_utils::toupper(emuenv.io.savedata) == savedata_key) {
                LOG_ERROR("LaunchAppByPath4 cannot mount savedata '{}' already used by the primary process", savedata);
                return static_cast<int>(SCE_APPMGR_ERROR_BUSY);
            }
            for (const auto &existing : processes.processes) {
                if (string_utils::toupper(existing->io.savedata) == savedata_key && existing->io.user_id == process->io.user_id) {
                    LOG_ERROR("LaunchAppByPath4 cannot mount savedata '{}' already used by process {}", savedata, existing->pid);
                    return static_cast<int>(SCE_APPMGR_ERROR_BUSY);
                }
            }
            process->io.savedata = savedata;
            if (!init_savedata_app_path(process->io, emuenv.vita_fs_path,
                    [&emuenv] { return app::get_savedata_identity(emuenv.regmgr, emuenv.vita_fs_path); })) {
                LOG_ERROR("LaunchAppByPath4 could not mount savedata for '{}'", name);
                return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
            }
            process->apputil.io = &process->io;
        }
    }
    std::array<uint8_t, 16> license_key{};
    if (ux0_app && name.starts_with("PCS")) {
        std::string content_id;
        if (sfo::get_data_by_key(content_id, *process->sfo, "CONTENT_ID")
            && content_id.size() == 36
            && std::ranges::all_of(content_id, [](unsigned char ch) {
                   return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
               })) {
            SceNpDrmLicense license{};
            const auto license_path = emuenv.vita_fs_path / "ux0" / "license" / name / (content_id + ".rif");
            if (open_license(license_path, license))
                std::copy(std::begin(license.key), std::end(license.key), license_key.begin());
            else
                LOG_WARN("LaunchAppByPath4 could not read the license for '{}'; using the default SELF key", name);
        } else {
            LOG_WARN("LaunchAppByPath4 has no valid content ID for '{}'; using the default SELF key", name);
        }
    }
    // Secondary address spaces need page tables for shared module and memblock mappings.
    if (!init(process->mem, true)) {
        LOG_ERROR("LaunchAppByPath4 could not initialize memory for '{}'", name);
        return static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
    }

    const auto cleanup = [&] {
        emuenv.kernel.stop_threads_in_address_space(process->mem);
        {
            const std::lock_guard<std::mutex> lock(emuenv.gxm.process_states_mutex);
            if (emuenv.gxm.process_states.contains(&process->mem)) {
                LOG_WARN("LaunchAppByPath4 retains failed process '{}' with active GXM state until session shutdown", name);
                processes.processes.push_back(std::move(process));
                return;
            }
        }
        ngs::release_process_state(emuenv.ngs, process->mem);
        std::vector<SceUID> preload_ids;
        if (process->main_module_id > 0) {
            const auto modules = emuenv.kernel.get_process_start_modules(process->mem, process->main_module_id);
            if (modules) {
                for (const auto &module : modules->preloaded)
                    preload_ids.push_back(module->info.modid);
            }
        }
        for (auto it = preload_ids.rbegin(); it != preload_ids.rend(); ++it) {
            if (unload_module(emuenv, process->mem, *it) < 0) {
                LOG_ERROR("LaunchAppByPath4 could not unload preloaded module {}; retaining process '{}' until session shutdown", *it, name);
                processes.processes.push_back(std::move(process));
                return;
            }
        }
        if (process->main_module_id > 0 && unload_module(emuenv, process->mem, process->main_module_id) < 0) {
            LOG_ERROR("LaunchAppByPath4 could not unload failed process '{}'; retaining its memory until session shutdown", name);
            processes.processes.push_back(std::move(process));
            return;
        }
        release_shared_fb_process_state(emuenv, process->mem);
        if (!emuenv.kernel.release_thread_address_space(process->mem)) {
            LOG_ERROR("LaunchAppByPath4 could not release failed process '{}'; retaining its memory until session shutdown", name);
            processes.processes.push_back(std::move(process));
        }
    };

    if (!emuenv.kernel.register_process_identity(process->mem, {
                                                                   .app_path = process->io.app_path,
                                                                   .self_name = fs_utils::path_to_utf8(fs::path(ux0_app ? "eboot.bin" : self_path).filename()),
                                                               })
        || !init_exported_vars(emuenv, process->mem)) {
        LOG_ERROR("LaunchAppByPath4 could not initialize process '{}'", name);
        cleanup();
        return static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
    }
    process->main_module_id = load_module(emuenv, process->mem, process->io,
        ux0_app ? "app0:eboot.bin" : self_path, ux0_app ? license_key.data() : nullptr);
    if (process->main_module_id < 0) {
        const int error = process->main_module_id;
        LOG_ERROR("LaunchAppByPath4 could not load '{}'", self_path);
        cleanup();
        return error;
    }
    if (!preload_process_modules(emuenv, process->mem, process->io, ux0_app ? license_key.data() : nullptr)) {
        LOG_ERROR("LaunchAppByPath4 could not preload modules for '{}'", name);
        cleanup();
        return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
    }
    const auto module = emuenv.kernel.find_module(process->mem, process->main_module_id);
    const auto identity = emuenv.kernel.get_process_identity(process->mem);
    if (!module || !identity || !module->info.start_entry) {
        LOG_ERROR("LaunchAppByPath4 could not prepare the main module for '{}'", name);
        cleanup();
        return static_cast<int>(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
    }
    auto main_thread = emuenv.kernel.create_process_main_thread(process->mem, name.c_str(), module->info.start_entry);
    if (!main_thread) {
        LOG_ERROR("LaunchAppByPath4 could not create the main thread for '{}'", name);
        cleanup();
        return static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
    }
    process->pid = identity->pid;
    process->main_thread_id = main_thread->id;
    process->app_id = static_cast<SceUID>((processes.app_generation << 16) | (2 * (processes.processes.size() + 1) + 2));
    process->initial_app_id = process->app_id;
    processes.app_generation = (processes.app_generation + 1) & 0x7fff;
    const SceUID pid = process->pid;
    const SceUID app_id = process->app_id;
    process->launch_status = make_appmgr_launch_status(name, caller->pid, pid, app_id,
        launch_option ? &*launch_option : nullptr);
    process->launch_status.process_attr = process_attr;
    process->launch_status.launch_mode = launch_mode;
    const char *diagnostic = std::getenv("VITA3K_EXPERIMENTAL_START_SECONDARY");
    const bool diagnostic_start = diagnostic && std::strcmp(diagnostic, "1") == 0;
    if (diagnostic_start)
        process->state = SecondaryProcess::State::starting;
    else
        LOG_INFO("Created process {} for '{}' as app {}; waiting for Shell activation", pid, name, log_hex(app_id));
    if (launch_option && launch_option->status) {
        const auto status = secondary_status(*process);
        if (!write_appmgr_launch_status(thread->cpu->mem, launch_option->status, status.status)) {
            LOG_ERROR("LaunchAppByPath4 received an invalid status output at {}", log_hex(launch_option->status.address()));
            process->app_id = 0;
            main_thread.reset();
            cleanup();
            return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        }
    }
    auto *registered_process = process.get();
    processes.processes.push_back(std::move(process));
    if (diagnostic_start) {
        ++processes.reserved_shell_events;
        process_lock.unlock();
        const SceInt32 result = start_app_process(emuenv, *registered_process);
        process_lock.lock();
        --processes.reserved_shell_events;
        if (result < 0) {
            registered_process->state = SecondaryProcess::State::start_failed;
            registered_process->app_id = 0;
            LOG_ERROR("Could not start secondary process {}: {}", pid, log_hex(result));
            process_lock.unlock();
            ThreadHostWait host_wait(thread);
            emuenv.kernel.stop_threads_in_address_space(registered_process->mem);
            return result;
        }
        registered_process->state = SecondaryProcess::State::started;
        if (launch_option && launch_option->status) {
            const auto status = secondary_status(*registered_process);
            if (!write_appmgr_launch_status(thread->cpu->mem, launch_option->status, status.status)) {
                LOG_ERROR("LaunchAppByPath4 could not update status for started process {}", pid);
                if (queue_launch_by_name_event) {
                    SceAppMgrShellEvent launch_event{};
                    launch_event.event = 6;
                    launch_event.data.launch_by_name.app_ids[0] = app_id;
                    launch_event.data.launch_by_name.app_ids[1] = app_id;
                    processes.shell_events.push_back(launch_event);
                }
                SceAppMgrShellEvent loaded_event{};
                loaded_event.event = 2;
                loaded_event.data.loaded.process_id = pid;
                loaded_event.data.loaded.app_id = app_id;
                processes.shell_events.push_back(loaded_event);
                return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
            }
        }
        LOG_WARN("Started secondary process {} in diagnostic mode; unsafe HLE imports stop its thread", pid);
    }
    SceAppMgrShellEvent loaded_event{};
    loaded_event.event = 2;
    loaded_event.data.loaded.process_id = pid;
    loaded_event.data.loaded.app_id = app_id;
    processes.shell_events.push_back(loaded_event);
    LOG_DEBUG("Queued Shell app-loaded event for process {} and app {}", pid, log_hex(app_id));
    if (queue_launch_by_name_event) {
        SceAppMgrShellEvent launch_event{};
        launch_event.event = 6;
        launch_event.data.launch_by_name.app_ids[0] = app_id;
        launch_event.data.launch_by_name.app_ids[1] = app_id;
        processes.shell_events.push_back(launch_event);
        LOG_DEBUG("Queued Shell app-launch-by-name event for process {} and app {}", pid, log_hex(app_id));
    }
    return pid;
}

EXPORT(int, _sceAppMgrLaunchAppByPath4, Ptr<const char> path, Ptr<const char> title_id,
    SceUInt32 budget_size, Ptr<const AppMgrLaunchAppByPathArgs> arguments) {
    TRACY_FUNC(_sceAppMgrLaunchAppByPath4, path, title_id, budget_size, arguments);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        LOG_ERROR("LaunchAppByPath4: unknown thread {}", thread_id);
        return static_cast<int>(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    }
    const auto args = read_appmgr_launch_path_args(thread->cpu->mem, arguments);
    if (!args)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    return appmgr_launch_app_by_path(emuenv, thread_id, path, title_id, budget_size, args->param, args->process_attr, args->option);
}

EXPORT(int, _sceAppMgrLaunchAppByUri, SceUInt32 flags, Ptr<const char> uri) {
    TRACY_FUNC(_sceAppMgrLaunchAppByUri, flags, uri.address());
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    LOG_DEBUG("LaunchAppByUri flags: {}, uri: {}", log_hex(flags), appmgr_mount_log_string(thread->cpu->mem, uri, APP_PARAM_SIZE - 1));
    constexpr char manual_uri_prefix[] = "psm_manual:";
    constexpr char manual_app_name[] = "NPXS10027";
    constexpr char photo_uri_prefix[] = "photo:";
    constexpr char photo_app_name[] = "NPXS10004";
    constexpr char photo_app_path[] = "vs0:app/NPXS10004/eboot.bin";
    constexpr char search_uri_prefix[] = "search:";
    constexpr char browser_app_name[] = "NPXS10003";
    constexpr char browser_app_path[] = "vs0:app/NPXS10003/eboot.bin";
    constexpr size_t manual_uri_prefix_length = sizeof(manual_uri_prefix) - 1;

    std::string uri_value;
    if (!read_mount_string(thread->cpu->mem, uri, APP_PARAM_SIZE - 1 + manual_uri_prefix_length, uri_value))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    const auto launch_uri_app = [&](const char *app_name, size_t app_name_size, const char *app_path, size_t app_path_size) {
        auto name_buffer = alloc_block(thread->cpu->mem, app_name_size, "AppMgr_uri_app_name");
        if (!name_buffer) {
            LOG_ERROR("LaunchAppByUri could not allocate application name {}", app_name);
            return static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
        }
        if (!write_guest_memory(thread->cpu->mem, name_buffer.get(), app_name, app_name_size)) {
            LOG_ERROR("LaunchAppByUri could not write application name {}", app_name);
            return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        }
        auto path_buffer = alloc_block(thread->cpu->mem, app_path_size, "AppMgr_uri_app_path");
        if (!path_buffer) {
            LOG_ERROR("LaunchAppByUri could not allocate application path for {}", app_name);
            return static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
        }
        if (!write_guest_memory(thread->cpu->mem, path_buffer.get(), app_path, app_path_size)) {
            LOG_ERROR("LaunchAppByUri could not write application path for {}", app_name);
            return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        }

        LOG_INFO("LaunchAppByUri queuing {} through Path4 for Shell activation", app_name);
        const SceUID process_id = appmgr_launch_app_by_path(emuenv, thread_id,
            path_buffer.get_ptr<const char>(), name_buffer.get_ptr<const char>(), 0, uri, 0, {}, flags | 0x100000, true);
        if (process_id < 0)
            return process_id;
        return 0;
    };
    if (uri_value.starts_with(photo_uri_prefix))
        return launch_uri_app(photo_app_name, sizeof(photo_app_name), photo_app_path, sizeof(photo_app_path));
    if (uri_value.starts_with(search_uri_prefix))
        return launch_uri_app(browser_app_name, sizeof(browser_app_name), browser_app_path, sizeof(browser_app_path));
    if (!uri_value.starts_with(manual_uri_prefix)) {
        LOG_WARN("LaunchAppByUri does not support URI '{}'", uri_value);
        return RET_ERROR(SCE_ERROR_ERRNO_ENOSYS);
    }

    const std::string source_path = uri_value.substr(manual_uri_prefix_length);
    if (source_path.empty()) {
        LOG_ERROR("LaunchAppByUri received a psm_manual URI without a source app path");
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_APP_PATH);
    }

    auto name_buffer = alloc_block(thread->cpu->mem, sizeof(manual_app_name), "AppMgr_manual_app_name");
    if (!name_buffer) {
        LOG_ERROR("LaunchAppByUri could not allocate the manual application name");
        return static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
    }
    if (!write_guest_memory(thread->cpu->mem, name_buffer.get(), manual_app_name, sizeof(manual_app_name))) {
        LOG_ERROR("LaunchAppByUri could not write the manual application name");
        return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    }

    LOG_INFO("LaunchAppByUri routing psm_manual source '{}' to {}", source_path, manual_app_name);
    const Ptr<const char> source_param(uri.address() + static_cast<Address>(manual_uri_prefix_length));
    return appmgr_launch_app_by_name_for_shell(emuenv, thread_id, static_cast<int>(flags),
        name_buffer.get_ptr<const char>(), source_param);
}

EXPORT(int, _sceAppMgrLaunchAppByUri2) {
    TRACY_FUNC(_sceAppMgrLaunchAppByUri2);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrLaunchVideoStreamingApp) {
    TRACY_FUNC(_sceAppMgrLaunchVideoStreamingApp);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, _sceAppMgrLoadExec, const char *appPath, Ptr<char> const argv[], const SceAppMgrLoadExecOptParam *optParam) {
    TRACY_FUNC(_sceAppMgrLoadExec, appPath, argv, optParam);
    if (optParam)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID);
    if (const SceInt32 error = emuenv.kernel.check_primary_address_space_access(thread_id, "LoadExec"))
        return RET_ERROR(error);

    // Create exec path
    auto exec_path = static_cast<std::string>(appPath);
    if (exec_path.contains("app0:/"))
        exec_path.erase(0, 6);
    else
        exec_path.erase(0, 5);

    LOG_INFO("sceAppMgrLoadExec run self: {}", appPath);

    // Load exec executable
    vfs::FileBuffer exec_buffer;
    const auto mounted_exec_path = "app0:/" + exec_path;
    const bool read_exec = emuenv.io.app_game_base_mount.empty()
        ? vfs::read_app_file(exec_buffer, emuenv.vita_fs_path, emuenv.io.app_path, exec_path, emuenv.io.title_id)
        : read_file_to_buffer(emuenv.io, mounted_exec_path.c_str(), emuenv.vita_fs_path, exec_buffer, export_name);
    if (read_exec) {
        std::vector<std::string> exec_argv;
        if (argv && argv->get(emuenv.mem)) {
            size_t args = 0;
            for (auto i = 0; argv[i]; i++) {
                const char *arg = argv[i].get(emuenv.mem);
                LOG_INFO("sceAppMgrLoadExec run with argument at {}: {}", i, arg);
                args += strlen(arg);
                exec_argv.emplace_back(arg);
            }

            if (args > 1024)
                return RET_ERROR(SCE_APPMGR_ERROR_TOO_LONG_ARGV);
        }

        if (const SceInt32 error = emuenv.kernel.request_process_exit(thread_id, 0, AppLaunchRequest{ .app_path = emuenv.io.app_path, .self_path = std::move(exec_path), .argv = std::move(exec_argv), .reason = AppLaunchReason::LoadExec }))
            return RET_ERROR(error);

        return SCE_KERNEL_OK;
    }

    return RET_ERROR(SCE_APPMGR_ERROR_INVALID_SELF_PATH);
}

EXPORT(int, _sceAppMgrLoadSaveDataSystemFile) {
    TRACY_FUNC(_sceAppMgrLoadSaveDataSystemFile);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrLoopBackFormat) {
    TRACY_FUNC(_sceAppMgrLoopBackFormat);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrLoopBackMount) {
    TRACY_FUNC(_sceAppMgrLoopBackMount);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, _sceAppMgrMmsMount, SceInt32 id, char *mount_point) {
    TRACY_FUNC(_sceAppMgrMmsMount, id, mount_point);
    switch (id) {
    case 0x190:
        strcpy(mount_point, "ux0:mms/photo");
        break;
    case 0x191:
        strcpy(mount_point, "ux0:mms/music");
        break;
    case 0x192:
        strcpy(mount_point, "ux0:mms/video");
        break;
    default:
        LOG_ERROR("Unknown id: {}", log_hex(id));
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    }

    return STUBBED("using strcpy");
}

EXPORT(int, _sceAppMgrOverwriteLaunchParamForShell, SceInt32 app_id, Ptr<const char> param) {
    TRACY_FUNC(_sceAppMgrOverwriteLaunchParamForShell, app_id, param);
    return appmgr_overwrite_launch_param_for_shell(emuenv, thread_id, app_id, param);
}

EXPORT(int, _sceAppMgrPeekLaunchParamForShell, SceInt32 app_id, Ptr<char> output) {
    TRACY_FUNC(_sceAppMgrPeekLaunchParamForShell, app_id, output);
    return appmgr_peek_launch_param_for_shell(emuenv, thread_id, app_id, output);
}

EXPORT(int, _sceAppMgrPhotoMount) {
    TRACY_FUNC(_sceAppMgrPhotoMount);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrPhotoUmount) {
    TRACY_FUNC(_sceAppMgrPhotoUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrPspSaveDataGetParams) {
    TRACY_FUNC(_sceAppMgrPspSaveDataGetParams);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrPspSaveDataRead) {
    TRACY_FUNC(_sceAppMgrPspSaveDataRead);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrPspSaveDataRootMount) {
    TRACY_FUNC(_sceAppMgrPspSaveDataRootMount);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrReceiveEvent, SceAppMgrEvent *event) {
    TRACY_FUNC(_sceAppMgrReceiveEvent, event);
    if (!event)
        return RET_ERROR(SCE_APPMGR_ERROR_NULL_POINTER);
    STUBBED("Only launch parameter events are supported");
    auto *events = get_process_appmgr_events(emuenv, thread_id);
    if (!events)
        return RET_ERROR(SCE_APPMGR_ERROR_STATE);
    if (!events->consume_launch_param()) {
        event->event = 0;
        return RET_ERROR(SCE_APPMGR_ERROR_NO_EVENT);
    }
    *event = {};
    event->event = 0x20000004;
    event->app_id = -1;
    return 0;
}

EXPORT(int, _sceAppMgrReceiveEventNum, SceUInt32 *eventNum) {
    TRACY_FUNC(_sceAppMgrReceiveEventNum, eventNum);
    if (!eventNum)
        return RET_ERROR(SCE_APPMGR_ERROR_NULL_POINTER);
    const auto *events = get_process_appmgr_events(emuenv, thread_id);
    if (!events)
        return RET_ERROR(SCE_APPMGR_ERROR_STATE);
    *eventNum = events->pending_count();
    STUBBED("Only launch parameter events are supported");
    return 0;
}

int appmgr_queue_shell_notification(EmuEnvState &emuenv, const SceAppMgrNotificationRequest &request, SceInt32 wait,
    const char *export_name) {
    if (request.type != 0)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID);
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot queue Shell notification without AppMgr process state");
        return RET_ERROR(SCE_APPMGR_ERROR_STATE);
    }
    auto &queue = processes->shell_notifications;
    std::unique_lock lock(queue.mutex);
    if (wait != 0) {
        queue.write_slots_available.wait(lock, [&queue] {
            return queue.count < queue.capacity;
        });
    } else if (queue.count >= queue.capacity) {
        return RET_ERROR(SCE_KERNEL_ERROR_SEMA_ZERO);
    }
    queue.buffer[queue.write_index] = request;
    queue.write_index = (queue.write_index + 1) % queue.capacity;
    ++queue.count;
    return 0;
}

static bool write_notification_request(MemState &mem, Ptr<SceAppMgrNotificationRequest> destination,
    const SceAppMgrNotificationRequest &request) {
    const GuestMemoryRange range{ destination.address(), sizeof(request) };
    const uint64_t pin = pin_guest_memory(mem, std::span(&range, 1));
    if (!pin)
        return false;
    const auto *bytes = reinterpret_cast<const uint8_t *>(&request);
    for (size_t i = 0; i < sizeof(request); ++i)
        *Ptr<uint8_t>(destination.address() + static_cast<Address>(i)).get(mem) = bytes[i];
    unpin_guest_memory(mem, pin);
    return true;
}

EXPORT(int, _sceAppMgrReceiveNotificationRequestForShell, Ptr<SceAppMgrNotificationRequest> request) {
    TRACY_FUNC(_sceAppMgrReceiveNotificationRequestForShell, request.address());
    if (!is_shell_caller(emuenv, thread_id))
        return RET_ERROR(SCE_APPMGR_ERROR_NOT_SHELL);
    if (!request)
        return RET_ERROR(SCE_APPMGR_ERROR_NULL_POINTER);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return RET_ERROR(SCE_APPMGR_ERROR_STATE);

    auto &queue = processes->shell_notifications;
    {
        std::lock_guard lock(queue.mutex);
        if (queue.count == 0)
            return RET_ERROR(SCE_APPMGR_ERROR_NO_NOTIFICATION);
        const SceUInt32 read_index = (queue.write_index + queue.capacity - queue.count) % queue.capacity;
        const SceAppMgrNotificationRequest &notification = queue.buffer[read_index];
        const auto *payload = reinterpret_cast<const char *>(notification.payload);
        const auto *end = static_cast<const char *>(std::memchr(payload, '\0', sizeof(notification.payload)));
        const std::string payload_text(payload, end ? end : payload + sizeof(notification.payload));
        LOG_DEBUG("ReceiveNotificationRequestForShell type={}, payload={}", notification.type, payload_text);
        if (!write_notification_request(thread->cpu->mem, request, notification))
            return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
        --queue.count;
    }
    queue.write_slots_available.notify_one();
    return 0;
}

EXPORT(int, _sceAppMgrReceiveShellEvent, Ptr<SceAppMgrShellEvent> event) {
    TRACY_FUNC(_sceAppMgrReceiveShellEvent, event);
    return appmgr_receive_shell_event(emuenv, thread_id, event);
}

EXPORT(int, _sceAppMgrReceiveSystemEvent, SceAppMgrSystemEvent *systemEvent) {
    TRACY_FUNC(_sceAppMgrReceiveSystemEvent, systemEvent);
    systemEvent->systemEvent = SCE_APPMGR_SYSTEMEVENT_ON_RESUME;
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataAddMount) {
    TRACY_FUNC(_sceAppMgrSaveDataAddMount);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataDataRemove) {
    TRACY_FUNC(_sceAppMgrSaveDataDataRemove);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataDataRemove2) {
    TRACY_FUNC(_sceAppMgrSaveDataDataRemove2);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataDataSave) {
    TRACY_FUNC(_sceAppMgrSaveDataDataSave);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataDataSave2) {
    TRACY_FUNC(_sceAppMgrSaveDataDataSave2);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataGetQuota) {
    TRACY_FUNC(_sceAppMgrSaveDataGetQuota);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataMount) {
    TRACY_FUNC(_sceAppMgrSaveDataMount);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataSlotCreate) {
    TRACY_FUNC(_sceAppMgrSaveDataSlotCreate);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataSlotDelete) {
    TRACY_FUNC(_sceAppMgrSaveDataSlotDelete);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataSlotFileClose) {
    TRACY_FUNC(_sceAppMgrSaveDataSlotFileClose);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataSlotFileGetParam) {
    TRACY_FUNC(_sceAppMgrSaveDataSlotFileGetParam);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataSlotFileOpen) {
    TRACY_FUNC(_sceAppMgrSaveDataSlotFileOpen);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataSlotGetParam) {
    TRACY_FUNC(_sceAppMgrSaveDataSlotGetParam);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataSlotGetStatus) {
    TRACY_FUNC(_sceAppMgrSaveDataSlotGetStatus);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataSlotInit) {
    TRACY_FUNC(_sceAppMgrSaveDataSlotInit);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataSlotSetParam) {
    TRACY_FUNC(_sceAppMgrSaveDataSlotSetParam);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataSlotSetStatus) {
    TRACY_FUNC(_sceAppMgrSaveDataSlotSetStatus);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSaveDataUmount) {
    TRACY_FUNC(_sceAppMgrSaveDataUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSendNotificationRequest, Ptr<const SceAppMgrNotificationRequest> request, SceInt32 wait) {
    TRACY_FUNC(_sceAppMgrSendNotificationRequest, request.address(), wait);
    if (!request)
        return RET_ERROR(SCE_APPMGR_ERROR_NULL_POINTER);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    MemState &mem = thread->cpu->mem;
    const GuestMemoryRange range{ request.address(), sizeof(SceAppMgrNotificationRequest) };
    const uint64_t pin = pin_guest_memory(mem, std::span(&range, 1));
    if (!pin)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    const auto *guest_request = request.get(mem);
    if (!guest_request) {
        unpin_guest_memory(mem, pin);
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    }
    SceAppMgrNotificationRequest request_copy{};
    std::memcpy(&request_copy, guest_request, sizeof(request_copy));
    unpin_guest_memory(mem, pin);
    const auto *payload = reinterpret_cast<const char *>(request_copy.payload);
    const auto *end = static_cast<const char *>(std::memchr(payload, '\0', sizeof(request_copy.payload)));
    const std::string payload_text(payload, end ? end : payload + sizeof(request_copy.payload));
    LOG_DEBUG("SendNotificationRequest type={}, payload={}, wait={}", request_copy.type, payload_text, wait);
    return appmgr_queue_shell_notification(emuenv, request_copy, wait, export_name);
}

EXPORT(int, _sceAppMgrSendParam) {
    TRACY_FUNC(_sceAppMgrSendParam);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSendSystemEvent) {
    TRACY_FUNC(_sceAppMgrSendSystemEvent);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSendSystemEvent2) {
    TRACY_FUNC(_sceAppMgrSendSystemEvent2);
    return UNIMPLEMENTED();
}

int appmgr_set_back_render_port_owner(EmuEnvState &emuenv, SceUID thread_id, SceInt32 app_id) {
    if (!is_shell_caller(emuenv, thread_id))
        return static_cast<int>(SCE_APPMGR_ERROR_SHELL_ONLY);
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot set back render port owner without AppMgr process state");
        return static_cast<int>(SCE_KERNEL_ERROR_ERROR);
    }
    const std::lock_guard activation_lock(processes->activation_mutex);
    const std::lock_guard lock(processes->mutex);
    SceUID pid = 0;
    bool kept_suspended_frame = false;
    if (app_id == -1) {
        SceUID current_display_pid = 0;
        {
            const std::lock_guard display_lock(emuenv.display.display_info_mutex);
            current_display_pid = emuenv.display.active_display_pid;
        }
        const auto suspended = std::find_if(processes->processes.begin(), processes->processes.end(),
            [current_display_pid](const auto &process) {
                return process->pid == current_display_pid && process->suspended;
            });
        if (suspended != processes->processes.end()) {
            pid = current_display_pid;
            kept_suspended_frame = true;
        }
    } else {
        const auto it = std::find_if(processes->processes.begin(), processes->processes.end(),
            [app_id](const auto &process) {
                return process->app_id && (process->app_id == app_id || (process->state == SecondaryProcess::State::started && process->initial_app_id == app_id));
            });
        if (it == processes->processes.end()) {
            LOG_ERROR("Cannot set back render port owner: app {} was not found", log_hex(app_id));
            return static_cast<int>(SCE_APPMGR_ERROR_APP_NOT_FOUND);
        }
        pid = (*it)->pid;
    }
    const bool frame_changed = display::select_process_frame(emuenv.display, pid);
    if (kept_suspended_frame) {
        const std::lock_guard display_lock(emuenv.display.display_info_mutex);
        const auto frame = emuenv.display.process_frames.find(pid);
        if (frame == emuenv.display.process_frames.end() || !frame->second.software_pixels) {
            LOG_WARN("Suspended app {} has no captured software back frame", log_hex(pid));
        }
    }
    if (frame_changed || kept_suspended_frame)
        emuenv.renderer->should_display = true;
    return 0;
}

EXPORT(int, _sceAppMgrSetBackRenderPortOwner, SceInt32 app_id) {
    TRACY_FUNC(_sceAppMgrSetBackRenderPortOwner, app_id);
    return appmgr_set_back_render_port_owner(emuenv, thread_id, app_id);
}

EXPORT(int, _sceAppMgrSetBgmProxyApp, Ptr<const char> app_name) {
    TRACY_FUNC(_sceAppMgrSetBgmProxyApp, app_name);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    const auto pid = emuenv.kernel.get_thread_process_id(thread_id);
    if (!thread || !pid)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const int is_system_program = CALL_EXPORT(SceSblACMgrForDriver_IsSystemProgram, 0);
    if (is_system_program != 1) {
        LOG_WARN("{}: process {} cannot set a BGM proxy (IsSystemProgram returned {})",
            export_name, *pid, is_system_program);
        return RET_ERROR(SCE_APPMGR_ERROR_NOT_SHELL);
    }

    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot set BGM proxy without AppMgr process state");
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    }

    std::array<char, 32> proxy_name{};
    MemState &mem = thread->cpu->mem;
    if (app_name) {
        for (size_t i = 0; i < proxy_name.size(); ++i) {
            if (i > std::numeric_limits<Address>::max() - app_name.address())
                return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
            const Ptr<const char> current(app_name.address() + static_cast<Address>(i));
            if (!current.valid(mem))
                return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
            proxy_name[i] = *current.get(mem);
            if (!proxy_name[i])
                break;
        }
    }

    const auto proxy_name_end = std::find(proxy_name.begin(), proxy_name.end(), '\0');
    const std::string_view proxy_name_view(proxy_name.data(), static_cast<size_t>(proxy_name_end - proxy_name.begin()));
    const std::lock_guard lock(processes->mutex);
    for (const auto &process : processes->processes) {
        if (process->pid != *pid)
            continue;
        std::copy(proxy_name.begin(), proxy_name.end(), process->launch_status.bgm_proxy_app_name);
        LOG_INFO("{}: process {} set BGM proxy app '{}'", export_name, *pid, proxy_name_view);
        return 0;
    }
    LOG_ERROR("Cannot set BGM proxy for untracked process {}", *pid);
    return RET_ERROR(SCE_APPMGR_ERROR_APP_NOT_FOUND);
}

EXPORT(int, _sceAppMgrSetNetworkDisconnectionWarningDialogState) {
    TRACY_FUNC(_sceAppMgrSetNetworkDisconnectionWarningDialogState);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSetPowerSaveMode) {
    TRACY_FUNC(_sceAppMgrSetPowerSaveMode);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSetRecommendedScreenOrientationForShell) {
    TRACY_FUNC(_sceAppMgrSetRecommendedScreenOrientationForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSetShellScreenOrientation) {
    TRACY_FUNC(_sceAppMgrSetShellScreenOrientation);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSetSystemDataFile) {
    TRACY_FUNC(_sceAppMgrSetSystemDataFile);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSetSystemDataFilePlayReady) {
    TRACY_FUNC(_sceAppMgrSetSystemDataFilePlayReady);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSystemParamDateTimeGetConf) {
    TRACY_FUNC(_sceAppMgrSystemParamDateTimeGetConf);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSystemParamGetInt) {
    TRACY_FUNC(_sceAppMgrSystemParamGetInt);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrSystemParamGetString) {
    TRACY_FUNC(_sceAppMgrSystemParamGetString);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrThemeDataMount, Ptr<const char> contentId, Ptr<const char> themePath, Ptr<const char> rifPath,
    Ptr<const AppmgrDataMountArguments> userArguments) {
    TRACY_FUNC(_sceAppMgrThemeDataMount, contentId.address(), themePath.address(), rifPath.address(), userArguments.address());
    const auto context = get_process_io_context(emuenv, thread_id, "ThemeDataMount");
    if (!context)
        return RET_ERROR(context.error());
    MemState &mem = context->thread->cpu->mem;
    IOState &io = *context->io;
    LOG_DEBUG("ContentId: {}", contentId.get(mem));
    if (themePath)
        LOG_DEBUG("ThemePath: {}", themePath.get(mem));
    if (rifPath)
        LOG_DEBUG("RifPath: {}", rifPath.get(mem));
    if (!userArguments || userArguments.address() > std::numeric_limits<Address>::max() - (sizeof(AppmgrDataMountArguments) - 1))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    for (Address i = 0; i < sizeof(AppmgrDataMountArguments); ++i) {
        if (!Ptr<uint8_t>(userArguments.address() + i).valid(mem))
            return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    }
    const auto *arguments = userArguments.get(mem);
    if (!arguments)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    const Ptr<char> mountPoint = arguments->mountPoint;
    std::string content_id;
    if (!read_mount_string(mem, contentId, 0x7f, content_id) || !valid_mount_output(mem, mountPoint)
        || content_id.empty() || content_id == "." || content_id == ".." || content_id.find_first_of("\\/:") != std::string::npos)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    std::string theme_directory_id = content_id;
    if (themePath) {
        std::string theme_path;
        if (!read_mount_string(mem, themePath, 0x123, theme_path))
            return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
        if (theme_path.starts_with("ux0:/"))
            theme_path.erase(4, 1);
        constexpr std::string_view theme_prefix = "ux0:theme/";
        if (!theme_path.starts_with(theme_prefix)) {
            LOG_WARN("Unsupported ThemeDataMount theme path '{}' for '{}'", theme_path, content_id);
            return RET_ERROR(SCE_ERROR_ERRNO_ENOSYS);
        }
        theme_directory_id = theme_path.substr(theme_prefix.size());
        if (theme_directory_id.empty() || theme_directory_id.find_first_of("\\/:") != std::string::npos
            || theme_directory_id == "." || theme_directory_id == "..")
            return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    }
    const auto resolved_directory = pfsmgr::theme_directory_path(emuenv.vita_fs_path, content_id);
    if (!themePath && !resolved_directory) {
        LOG_WARN("Invalid theme content ID '{}'", content_id);
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    }
    fs::path license_file;
    if (rifPath) {
        std::string rif_path;
        if (!read_mount_string(mem, rifPath, 0x123, rif_path))
            return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
        if (rif_path.starts_with("ur0:/"))
            rif_path.erase(4, 1);
        if (rif_path != "ur0:temp/rif/work.bin") {
            LOG_WARN("Unsupported ThemeDataMount license path '{}' for '{}'", rif_path, content_id);
            return RET_ERROR(SCE_ERROR_ERRNO_ENOSYS);
        }
        license_file = emuenv.vita_fs_path / "ur0" / "temp" / "rif" / "work.bin";
    }
    const auto installed_license = pfsmgr::find_theme_license(emuenv.vita_fs_path, content_id);
    if (installed_license) {
        license_file = *installed_license;
        LOG_INFO("Using installed theme license '{}' for '{}'", license_file, content_id);
    } else if (license_file.empty()) {
        LOG_WARN("No installed license matches theme content ID '{}'", content_id);
        return RET_ERROR(SCE_ERROR_ERRNO_ENOENT);
    } else {
        LOG_WARN("No installed license matches theme content ID '{}'; using supplied fallback '{}'", content_id, license_file);
    }
    std::string mount_point;
    bool is_protected = false;
    const auto theme_root = emuenv.vita_fs_path / "ux0" / "theme";
    auto directory = theme_root / fs_utils::utf8_to_path(theme_directory_id);
    if (!themePath) {
        directory = *resolved_directory;
        std::error_code directory_error;
        const auto status = fs::symlink_status(directory, directory_error);
        if (directory_error && directory_error != std::errc::no_such_file_or_directory) {
            LOG_ERROR("Cannot inspect theme directory '{}': {}", directory, directory_error.message());
            return RET_ERROR(SCE_ERROR_ERRNO_ENOENT);
        }
        if (directory_error || !fs::is_directory(status)) {
            LOG_WARN("No theme directory matches content ID '{}' at '{}'", content_id, directory);
            return RET_ERROR(SCE_ERROR_ERRNO_ENOENT);
        }
    }
    if (!pfsmgr::mount_theme_data(io, directory, mount_point, is_protected, license_file)) {
        LOG_WARN("Cannot mount theme data for '{}'", content_id);
        return RET_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }
    for (Address i = 0; i < 16; ++i)
        *Ptr<char>(mountPoint.address() + i).get(mem) = i < mount_point.size() ? mount_point[i] : '\0';
    LOG_INFO("Mounted theme data for '{}' at '{}'", content_id, mount_point);
    return 0;
}

EXPORT(int, _sceAppMgrTrophyMount, int mountId, SceUID processId, Ptr<char> mountPoint) {
    TRACY_FUNC(_sceAppMgrTrophyMount, mountId, processId, mountPoint.address());
    const auto context = get_process_io_context(emuenv, thread_id, "TrophyMount");
    if (!context)
        return RET_ERROR(context.error());
    MemState &mem = context->thread->cpu->mem;
    IOState &io = *context->io;
    LOG_DEBUG("_sceAppMgrTrophyMount called with mountId {} and processId {}", log_hex(mountId), processId);
    if (mountId != 0x12C && mountId != 0x12E)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    if (!valid_mount_output(mem, mountPoint))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    const bool source_mount = mountId == 0x12C;
    std::string selected_title_id;
    if (source_mount) {
        IOState *source_io = processId == -2 || processId == -3 ? &io : find_process_io_by_pid(emuenv, processId);
        if (!source_io)
            return RET_ERROR(SCE_APPMGR_ERROR_PROCESS_NOT_FOUND);
        selected_title_id = source_io->title_id;
    }
    if (source_mount && (selected_title_id.empty() || selected_title_id.find_first_of("\\/:") != std::string::npos || selected_title_id == "." || selected_title_id == ".."))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);

    const std::string mount_path = source_mount
        ? "app/" + selected_title_id
        : "user/" + io.user_id + "/trophy/data/sce_trop";
    const fs::path host_path = emuenv.vita_fs_path / (source_mount ? "ux0" : "ur0") / fs_utils::utf8_to_path(mount_path);
    std::error_code error;
    if (source_mount) {
        if (!fs::is_directory(host_path, error)) {
            if (error)
                LOG_ERROR("Cannot inspect trophy source directory '{}': {}", host_path, error.message());
            return RET_ERROR(SCE_ERROR_ERRNO_ENOENT);
        }
    } else {
        if (io.user_id.empty())
            return RET_ERROR(SCE_APPMGR_ERROR_STATE);
        fs::create_directories(host_path, error);
        if (error) {
            LOG_ERROR("Cannot prepare trophy directory '{}': {}", host_path, error.message());
            return RET_ERROR(SCE_ERROR_ERRNO_EIO);
        }
    }

    const std::string mounted_device = source_mount ? "trophy_src0:" : "trophy_sys0:";
    const int mount_result = source_mount
        ? pfsmgr::mount_trophy_data(io, host_path, mounted_device, selected_title_id,
              emuenv.vita_fs_path / "ux0" / "patch", emuenv.vita_fs_path / "ux0" / "license")
        : pfsmgr::mount_trophy_data(io, host_path, mounted_device);
    if (mount_result != 0)
        return RET_ERROR(mount_result);
    io.device_paths.trophy_relative_mount = mounted_device;
    for (Address i = 0; i < 16; ++i)
        *Ptr<char>(mountPoint.address() + i).get(mem) = i < mounted_device.size() ? mounted_device[i] : '\0';
    if (source_mount)
        LOG_INFO("Mounted trophy source directory for title '{}' at '{}'", selected_title_id, mounted_device);
    else
        LOG_INFO("Mounted trophy system directory for user '{}' at '{}'", io.user_id, mounted_device);
    return 0;
}

EXPORT(int, _sceAppMgrTrophyMountById, int mountId, Ptr<const char> titleId, Ptr<char> mountPoint) {
    TRACY_FUNC(_sceAppMgrTrophyMountById, mountId, titleId.address(), mountPoint.address());
    const auto context = get_process_io_context(emuenv, thread_id, "TrophyMountById");
    if (!context)
        return RET_ERROR(context.error());
    MemState &mem = context->thread->cpu->mem;
    IOState &io = *context->io;
    std::string comm_id;
    if (mountId != 0x12F && mountId != 0x130)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    if (!read_mount_string(mem, titleId, 31, comm_id) || comm_id.empty()
        || comm_id == "." || comm_id == ".." || comm_id.find_first_of("\\/:") != std::string::npos)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    if (!valid_mount_output(mem, mountPoint))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    if (io.user_id.empty())
        return RET_ERROR(SCE_APPMGR_ERROR_STATE);

    const bool title_data_mount = mountId == 0x12F;
    const std::string mount_point = title_data_mount ? "trophy_dat0:" : "trophy_dbk0:";
    const fs::path relative_path = title_data_mount
        ? fs::path("user") / fs_utils::utf8_to_path(io.user_id) / "trophy" / "data" / fs_utils::utf8_to_path(comm_id)
        : fs::path("user") / fs_utils::utf8_to_path(io.user_id) / "trophy" / "data" / "db";
    const fs::path host_path = emuenv.vita_fs_path / "ur0" / relative_path;
    std::error_code error;
    fs::create_directories(host_path, error);
    if (error) {
        LOG_ERROR("Cannot prepare trophy directory '{}': {}", host_path, error.message());
        return RET_ERROR(SCE_ERROR_ERRNO_EIO);
    }

    const int mount_result = pfsmgr::mount_trophy_data(io, host_path, mount_point);
    if (mount_result != 0)
        return RET_ERROR(mount_result);
    io.device_paths.trophy_relative_mount = mount_point;
    for (Address i = 0; i < 16; ++i)
        *Ptr<char>(mountPoint.address() + i).get(mem) = i < mount_point.size() ? mount_point[i] : '\0';
    LOG_INFO("Mounted trophy {} directory for '{}' at '{}'", title_data_mount ? "data" : "database", comm_id, mount_point);
    return 0;
}

EXPORT(int, _sceAppMgrUmount, Ptr<const char> mountPoint) {
    TRACY_FUNC(_sceAppMgrUmount, mountPoint.address());
    const auto context = get_process_io_context(emuenv, thread_id, "Umount");
    if (!context)
        return RET_ERROR(context.error());
    MemState &mem = context->thread->cpu->mem;
    IOState &io = *context->io;
    std::string mount_point;
    if (!read_mount_string(mem, mountPoint, 15, mount_point))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    if (pfsmgr::unmount_game_data(io, mount_point)) {
        LOG_INFO("Unmounted game data '{}'", mount_point);
        return 0;
    }
    if (pfsmgr::unmount_appmeta(io, mount_point)) {
        LOG_INFO("Unmounted appmeta '{}'", mount_point);
        return 0;
    }
    if (pfsmgr::unmount_app_data(io, mount_point)) {
        LOG_INFO("Unmounted app data '{}'", mount_point);
        return 0;
    }
    if (pfsmgr::unmount_theme_data(io, mount_point)) {
        LOG_INFO("Unmounted theme data '{}'", mount_point);
        return 0;
    }
    if (pfsmgr::unmount_trophy_data(io, mount_point)) {
        if (io.device_paths.trophy_relative_mount == mount_point)
            io.device_paths.trophy_relative_mount.clear();
        LOG_INFO("Unmounted trophy directory '{}'", mount_point);
        return 0;
    }
    if (mount_point == "ux0:picture" || mount_point == "friend0:" || mount_point == "psnmsg0:"
        || mount_point == "near0:" || mount_point == "ux0:music" || mount_point == "ux0:calendar"
        || mount_point == "ux0:video" || mount_point == "sqlite0:/" || mount_point == "attach0:/"
        || mount_point == "ux0:pspemu/" || mount_point == "checkout0:/" || mount_point == "webbrowser0:/"
        || mount_point == "ux0:mms/photo" || mount_point == "ux0:mms/music" || mount_point == "ux0:mms/video")
        return STUBBED("Unmount of legacy static path redirection");
    LOG_WARN("Unknown or unsupported AppMgr mount '{}'", mount_point);
    return RET_ERROR(SCE_ERROR_ERRNO_ENOENT);
}

EXPORT(int, _sceAppMgrUmountByPid) {
    TRACY_FUNC(_sceAppMgrUmountByPid);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrUpdateSaveDataParam) {
    TRACY_FUNC(_sceAppMgrUpdateSaveDataParam);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrWorkDirMount) {
    TRACY_FUNC(_sceAppMgrWorkDirMount);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceAppMgrWorkDirMountById, int mountId, Ptr<const char> id, Ptr<char> mountPoint) {
    TRACY_FUNC(_sceAppMgrWorkDirMountById, mountId, id.address(), mountPoint.address());
    const auto context = get_process_io_context(emuenv, thread_id, "WorkDirMountById");
    if (!context)
        return RET_ERROR(context.error());
    MemState &mem = context->thread->cpu->mem;
    IOState &io = *context->io;
    if (mountId != 205 && mountId != 207)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);

    std::string id_string;
    if (!read_mount_string(mem, id, 9, id_string))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    if (!valid_mount_output(mem, mountPoint))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    const auto valid_title_id = [](std::string_view value) {
        return value.size() == 9
            && std::ranges::all_of(value.begin(), value.begin() + 4, [](unsigned char ch) { return ch >= 'A' && ch <= 'Z'; })
            && std::ranges::all_of(value.begin() + 4, value.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
    };
    if (!valid_title_id(id_string) || !valid_title_id(io.title_id))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);

    const std::string source_path = mountId == 205
        ? "ux0:cache/" + io.title_id
        : "ux0:temp/app_work/" + id_string + "/rec";
    const fs::path directory = expand_path(io, source_path.c_str(), emuenv.vita_fs_path);
    if (directory.empty())
        return RET_ERROR(SCE_ERROR_ERRNO_ENOENT);
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        LOG_ERROR("Cannot prepare work directory '{}': {}", source_path, error.message());
        return RET_ERROR(SCE_ERROR_ERRNO_EIO);
    }

    std::string mount_point;
    const int result = pfsmgr::mount_app_data(io, directory, io.title_id, {}, mount_point);
    if (result != 0)
        return RET_ERROR(result);
    if (mount_point.size() >= 16) {
        if (!pfsmgr::unmount_app_data(io, mount_point))
            LOG_ERROR("Cannot roll back oversized work directory mount '{}'", mount_point);
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    }
    for (Address i = 0; i < 16; ++i)
        *Ptr<char>(mountPoint.address() + i).get(mem) = i < mount_point.size() ? mount_point[i] : '\0';
    const auto pid = emuenv.kernel.get_thread_process_id(thread_id);
    LOG_INFO("Mounted work directory type {} for '{}' at '{}' in process {}",
        log_hex(mountId), id_string, mount_point, pid.value_or(0));
    return 0;
}

EXPORT(int, sceAppMgrAcquireBgmPort) {
    TRACY_FUNC(sceAppMgrAcquireBgmPort);
    return CALL_EXPORT(sceAppMgrAcquireBgmPortWithPriority, 256);
}

EXPORT(int, sceAppMgrAcquireBgmPortForMusicPlayer) {
    TRACY_FUNC(sceAppMgrAcquireBgmPortForMusicPlayer);
    return CALL_EXPORT(sceAppMgrAcquireBgmPortWithPriority, 130);
}

EXPORT(int, sceAppMgrAcquireBgmPortWithPriority, int priority) {
    TRACY_FUNC(sceAppMgrAcquireBgmPortWithPriority, priority);
    LOG_INFO("{}: BGM port acquisition requested by thread {} with priority {}", export_name, thread_id, priority);
    if (priority < 0)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID);
    const auto pid = emuenv.kernel.get_thread_process_id(thread_id);
    if (!pid)
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    auto *bgm = emuenv.kernel.obj_store.get<AppMgrBgm>();
    if (!bgm) {
        LOG_ERROR("{}: BGM state is not initialized", export_name);
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    }
    try {
        if (!bgm->acquire(*pid, priority)) {
            LOG_WARN("{}: BGM port is busy for process {} at priority {}", export_name, *pid, priority);
            return RET_ERROR(SCE_APPMGR_ERROR_BGM_PORT_BUSY);
        }
    } catch (const std::bad_alloc &) {
        LOG_ERROR("{}: cannot allocate BGM state for process {}", export_name, *pid);
        return RET_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
    }
    LOG_INFO("{}: process {} acquired BGM port with priority {}", export_name, *pid, priority);
    return STUBBED("BGM priority arbitration, permissions and audio adoption are not emulated");
}

EXPORT(int, sceAppMgrAcquireBtrm) {
    TRACY_FUNC(sceAppMgrAcquireBtrm);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrAcquireSoundOutExclusive) {
    TRACY_FUNC(sceAppMgrAcquireSoundOutExclusive);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrAcquireSoundOutExclusive2) {
    TRACY_FUNC(sceAppMgrAcquireSoundOutExclusive2);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrActivateApp, SceInt32 app_id) {
    TRACY_FUNC(sceAppMgrActivateApp, app_id);
    if (!is_shell_caller(emuenv, thread_id))
        return RET_ERROR(SCE_APPMGR_ERROR_NOT_SHELL);
    const SceInt32 result = appmgr_activate_app(emuenv, app_id);
    return result < 0 ? RET_ERROR(result) : result;
}

EXPORT(int, sceAppMgrDeactivateApp, SceInt32 app_id) {
    TRACY_FUNC(sceAppMgrDeactivateApp, app_id);
    if (!is_shell_caller(emuenv, thread_id))
        return RET_ERROR(SCE_APPMGR_ERROR_NOT_SHELL);
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes)
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    const std::lock_guard activation_lock(processes->activation_mutex);
    SecondaryProcess *process = nullptr;
    {
        const std::lock_guard lock(processes->mutex);
        const auto it = std::find_if(processes->processes.begin(), processes->processes.end(),
            [app_id](const auto &candidate) { return app_id != 0 && candidate->app_id == app_id; });
        if (it == processes->processes.end())
            return RET_ERROR(SCE_APPMGR_ERROR_APP_NOT_FOUND);
        if ((*it)->state == SecondaryProcess::State::starting)
            return RET_ERROR(SCE_APPMGR_ERROR_BUSY);
        if (!(*it)->activated)
            return RET_ERROR(SCE_APPMGR_ERROR_STATE);
        process = it->get();
    }

    const int result = suspend_secondary_process_threads(emuenv, *process);
    if (result < 0)
        return RET_ERROR(result);
    if (auto *bgm = emuenv.kernel.obj_store.get<AppMgrBgm>(); bgm && bgm->suspend(process->pid))
        LOG_DEBUG("Released BGM port while suspending app {}", log_hex(app_id));
    {
        const std::lock_guard lock(processes->mutex);
        process->activated = false;
    }
    return 0;
}

EXPORT(int, sceAppMgrDeclareSystemChatApp) {
    TRACY_FUNC(sceAppMgrDeclareSystemChatApp);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrDestroyAppByAppId, SceInt32 app_id) {
    TRACY_FUNC(sceAppMgrDestroyAppByAppId, app_id);
    return appmgr_destroy_app_by_app_id(emuenv, thread_id, app_id, false, export_name);
}

EXPORT(int, sceAppMgrDestroyOtherApp) {
    TRACY_FUNC(sceAppMgrDestroyOtherApp);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrDestroyOtherAppByAppIdForShell) {
    TRACY_FUNC(sceAppMgrDestroyOtherAppByAppIdForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrDestroyOtherAppByPidForShell) {
    TRACY_FUNC(sceAppMgrDestroyOtherAppByPidForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrDump) {
    TRACY_FUNC(sceAppMgrDump);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrEnableCoredumpForTest) {
    TRACY_FUNC(sceAppMgrEnableCoredumpForTest);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrEnableDuckingOnSystemChat) {
    TRACY_FUNC(sceAppMgrEnableDuckingOnSystemChat);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrEnablePrioritizingSystemChat) {
    TRACY_FUNC(sceAppMgrEnablePrioritizingSystemChat);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrExitToLiveboardForGameApp) {
    TRACY_FUNC(sceAppMgrExitToLiveboardForGameApp);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrFinishCoredumpForShell) {
    TRACY_FUNC(sceAppMgrFinishCoredumpForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetAppIdByAppId) {
    TRACY_FUNC(sceAppMgrGetAppIdByAppId);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetExtraAppParam) {
    TRACY_FUNC(sceAppMgrGetExtraAppParam);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetProcessIdByAppIdForShell, SceInt32 app_id) {
    TRACY_FUNC(sceAppMgrGetProcessIdByAppIdForShell, app_id);
    LOG_DEBUG_ONCE("{} called for app ID {}", export_name, app_id);
    if (!is_shell_caller(emuenv, thread_id))
        return RET_ERROR(SCE_APPMGR_ERROR_NOT_SHELL);
    if (app_id == -2) {
        const auto pid = emuenv.kernel.get_thread_process_id(thread_id);
        if (!pid)
            return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
        return *pid;
    }
    if (app_id == -1) {
        const std::lock_guard display_lock(emuenv.display.display_info_mutex);
        if (emuenv.display.active_display_pid > 0)
            return emuenv.display.active_display_pid;
        LOG_TRACE("{} found no active display process for app ID -1", export_name);
        return RET_ERROR(SCE_APPMGR_ERROR_APP_NOT_FOUND);
    }
    if (app_id == -4) {
        auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
        if (!processes) {
            LOG_ERROR("{}: AppMgr process state is unavailable", export_name);
            return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
        }
        const std::lock_guard lock(processes->mutex);
        const auto active = std::ranges::find_if(processes->processes, [](const auto &process) {
            return process->activated && process->state == SecondaryProcess::State::started;
        });
        if (active == processes->processes.end()) {
            LOG_TRACE("{} found no pending or active process for app ID -4", export_name);
            return RET_ERROR(SCE_APPMGR_ERROR_APP_NOT_FOUND);
        }
        return (*active)->pid;
    }
    if (app_id < -1) {
        LOG_ERROR("{}: special app ID {} is not implemented", export_name, app_id);
        return RET_ERROR(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
    }
    const auto app = find_secondary_app_by_id(emuenv, app_id);
    if (app)
        return app->status.process_id;
    if (const auto unloaded_app = find_unloaded_app_by_id(emuenv, app_id)) {
        LOG_TRACE("{} returned process {} for recently unloaded app ID {}",
            export_name, unloaded_app->status.process_id, app_id);
        return unloaded_app->status.process_id;
    }
    LOG_ERROR("{} could not find process for app ID {}", export_name, app_id);
    return RET_ERROR(SCE_APPMGR_ERROR_APP_NOT_FOUND);
}

EXPORT(int, sceAppMgrGetSystemDataFile, SceUInt32 location, Ptr<uint8_t> buffer, SceSize size) {
    TRACY_FUNC(sceAppMgrGetSystemDataFile, location, buffer, size);
    LOG_INFO("{} location={} buffer={} size={}", export_name, location, log_hex(buffer.address()), size);

    if (location != 0) {
        if (location == 3 || location == 4) {
            LOG_WARN("{}: system-data location {} is not implemented", export_name, location);
            return RET_ERROR(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
        }
        LOG_ERROR("{}: invalid system-data location {}", export_name, location);
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER2);
    }
    if (!buffer || size > 0x8000)
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_POINTER);

    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());

    IOState &io = *context->io;
    MemState &mem = context->thread->cpu->mem;
    std::string path = "ur0:user/";
    path += io.user_id;
    path += "/np/myprofile.dat";

    SceIoStat stat{};
    const int stat_result = stat_file(io, path.c_str(), &stat, emuenv.vita_fs_path, export_name);
    if (stat_result < 0)
        return stat_result;
    if (stat.st_size < 1)
        return static_cast<int>(SCE_APPMGR_ERROR_SYSTEM_DATA_READ);
    if (static_cast<int64_t>(size) < stat.st_size)
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_PARAMETER2);

    const SceSize data_size = static_cast<SceSize>(stat.st_size);
    const SceUID fd = open_file(io, path.c_str(), SCE_O_RDONLY, emuenv.vita_fs_path, export_name);
    if (fd < 0)
        return fd;

    const GuestMemoryRange destination_range{ buffer.address(), data_size };
    const uint64_t destination_pin = pin_guest_memory(mem, std::span(&destination_range, 1));
    if (!destination_pin) {
        const int close_result = close_file(io, fd, export_name);
        if (close_result < 0)
            LOG_WARN("{}: failed to close profile file after invalid buffer (error {})", export_name, close_result);
        return static_cast<int>(SCE_APPMGR_ERROR_INVALID_POINTER);
    }

    std::vector<uint8_t> data(data_size);
    const int read_size = read_file(data.data(), io, fd, data_size, export_name);
    const int close_result = close_file(io, fd, export_name);
    if (close_result < 0)
        LOG_WARN("{}: failed to close profile file (error {})", export_name, close_result);

    if (read_size != static_cast<int>(data_size)) {
        std::memset(buffer.get(mem), 0, data_size);
        unpin_guest_memory(mem, destination_pin);
        LOG_ERROR("{}: failed to read profile file (read {} of {} bytes)", export_name, read_size, data_size);
        return static_cast<int>(SCE_APPMGR_ERROR_SYSTEM_DATA_READ);
    }

    std::memcpy(buffer.get(mem), data.data(), data_size);
    unpin_guest_memory(mem, destination_pin);
    LOG_INFO("{}: returned {} profile bytes", export_name, read_size);
    return read_size;
}

EXPORT(int, sceAppMgrGrowMemory) {
    TRACY_FUNC(sceAppMgrGrowMemory);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGrowMemory3) {
    TRACY_FUNC(sceAppMgrGrowMemory3);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrIsDevelopmentMode) {
    TRACY_FUNC(sceAppMgrIsDevelopmentMode);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrIsGameBudgetAppPresent) {
    TRACY_FUNC(sceAppMgrIsGameBudgetAppPresent);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrIsGameProgram) {
    TRACY_FUNC(sceAppMgrIsGameProgram);
    return 0;
}

EXPORT(int, sceAppMgrIsNonGameProgram) {
    TRACY_FUNC(sceAppMgrIsNonGameProgram);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    return CALL_EXPORT(SceSblACMgrForDriver_6C5AB07F, 0);
}

EXPORT(int, sceAppMgrIsOtherAppPresent) {
    TRACY_FUNC(sceAppMgrIsOtherAppPresent);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrIsPidShellAndCrashed, SceUID pid) {
    TRACY_FUNC(sceAppMgrIsPidShellAndCrashed, pid);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    const auto shell = emuenv.kernel.get_process_identity(emuenv.mem);
    const bool shell_matches = shell && shell->is_shell() && shell->pid == pid;
    bool shell_running = false;
    // A live thread in the Shell address space is the local equivalent of AppMgr run-status 16.
    if (shell_matches) {
        const std::lock_guard lock(emuenv.kernel.mutex);
        for (const auto &[_, candidate] : emuenv.kernel.threads) {
            if (&candidate->cpu->mem == &emuenv.mem) {
                shell_running = true;
                break;
            }
        }
    }
    const int shell_run_status = shell_running ? 16 : 0;
    const int crashed = shell_matches && shell_run_status != 16;
    LOG_DEBUG_ONCE("{}: pid {} shell pid {} run status {} crashed {}", export_name, pid,
        shell ? shell->pid : 0, shell_run_status, crashed);
    return crashed;
}

EXPORT(int, sceAppMgrIsPsNowClient) {
    TRACY_FUNC(sceAppMgrIsPsNowClient);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrLaunchAppCancel, SceInt32 app_id) {
    TRACY_FUNC(sceAppMgrLaunchAppCancel, app_id);
    const int result = appmgr_destroy_app_by_app_id(emuenv, thread_id, app_id, true, export_name);
    if (result < 0)
        return result;
    LOG_DEBUG("Shell cancelled pending app {}", log_hex(app_id));
    return result;
}

EXPORT(SceInt32, sceAppMgrLoadSafeMemory, void *buffer, SceSize size, SceOff offset) {
    TRACY_FUNC(sceAppMgrLoadSafeMemory, buffer, size, offset);
    if (!buffer)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    if (offset < 0 || static_cast<uint64_t>(offset) + size > SCE_APPUTIL_SAFEMEMORY_MEMORY_SIZE)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER2);
    const auto context = get_process_io_context(emuenv, thread_id, "LoadSafeMemory");
    if (!context)
        return RET_ERROR(context.error());
    const auto result = execute_savedata_safe_memory(emuenv, *context->io, buffer, size, offset, export_name, false);
    if (result < 0)
        return RET_ERROR(result);
    return result > 0 ? size : 0;
}

EXPORT(int, sceAppMgrNotifyLiveBoardModeForShell) {
    TRACY_FUNC(sceAppMgrNotifyLiveBoardModeForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrQuitApp, SceUID app_id) {
    TRACY_FUNC(sceAppMgrQuitApp, app_id);
    return appmgr_destroy_app_by_app_id(emuenv, thread_id, app_id, false, export_name);
}

EXPORT(int, sceAppMgrQuitForNonSuspendableApp) {
    TRACY_FUNC(sceAppMgrQuitForNonSuspendableApp);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrReceiveShellEventNum) {
    TRACY_FUNC(sceAppMgrReceiveShellEventNum);
    if (!is_shell_caller(emuenv, thread_id))
        return RET_ERROR(SCE_APPMGR_ERROR_NOT_SHELL);
    auto *processes = emuenv.kernel.obj_store.get<AppMgrProcesses>();
    if (!processes) {
        LOG_ERROR("Cannot count Shell events without AppMgr process state");
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    }
    const std::lock_guard lock(processes->mutex);
    return static_cast<int>(processes->shell_events.size());
}

EXPORT(int, sceAppMgrReleaseBgmPort) {
    TRACY_FUNC(sceAppMgrReleaseBgmPort);
    const auto pid = emuenv.kernel.get_thread_process_id(thread_id);
    if (!pid)
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    auto *bgm = emuenv.kernel.obj_store.get<AppMgrBgm>();
    if (!bgm) {
        LOG_ERROR("{}: BGM state is not initialized", export_name);
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    }
    bgm->release(*pid);
    LOG_INFO("{}: process {} released its BGM port", export_name, *pid);
    return STUBBED("BGM audio adoption is not emulated");
}

EXPORT(int, sceAppMgrReleaseBtrm) {
    TRACY_FUNC(sceAppMgrReleaseBtrm);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrReleaseSoundOutExclusive) {
    TRACY_FUNC(sceAppMgrReleaseSoundOutExclusive);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrReleaseSoundOutExclusive2) {
    TRACY_FUNC(sceAppMgrReleaseSoundOutExclusive2);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrReleaseSoundOutExclusive3) {
    TRACY_FUNC(sceAppMgrReleaseSoundOutExclusive3);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrRestoreBgmSettingForShell) {
    TRACY_FUNC(sceAppMgrRestoreBgmSettingForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrRestoreDisplaySettingForShell) {
    TRACY_FUNC(sceAppMgrRestoreDisplaySettingForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrResumeBgAppByShell) {
    TRACY_FUNC(sceAppMgrResumeBgAppByShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrReturnLiveAreaOperationResultForShell) {
    TRACY_FUNC(sceAppMgrReturnLiveAreaOperationResultForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSaveDataGetCachedRequiredSizeKiB) {
    TRACY_FUNC(sceAppMgrSaveDataGetCachedRequiredSizeKiB);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceAppMgrSaveSafeMemory, const void *buffer, SceSize size, SceOff offset) {
    TRACY_FUNC(sceAppMgrSaveSafeMemory, buffer, size, offset);
    if (!buffer)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    if (offset < 0 || static_cast<uint64_t>(offset) + size > SCE_APPUTIL_SAFEMEMORY_MEMORY_SIZE)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER2);
    const auto context = get_process_io_context(emuenv, thread_id, "SaveSafeMemory");
    if (!context)
        return RET_ERROR(context.error());
    const auto result = execute_savedata_safe_memory(emuenv, *context->io, const_cast<void *>(buffer), size, offset, export_name, true);
    return result < 0 ? RET_ERROR(result) : static_cast<SceInt32>(size);
}

EXPORT(int, sceAppMgrSendLiveBoardMode) {
    TRACY_FUNC(sceAppMgrSendLiveBoardMode);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetAppProtectionModeOnMemoryShortage) {
    TRACY_FUNC(sceAppMgrSetAppProtectionModeOnMemoryShortage);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetBgmSubPriority, int enabled, int priority) {
    TRACY_FUNC(sceAppMgrSetBgmSubPriority, enabled, priority);
    if (priority < 0)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID);
    const auto pid = emuenv.kernel.get_thread_process_id(thread_id);
    if (!pid)
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    auto *bgm = emuenv.kernel.obj_store.get<AppMgrBgm>();
    if (!bgm) {
        LOG_ERROR("{}: BGM state is not initialized", export_name);
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    }
    try {
        bgm->set_sub_priority(*pid, enabled != 0, priority);
    } catch (const std::bad_alloc &) {
        LOG_ERROR("{}: cannot allocate BGM state for process {}", export_name, *pid);
        return RET_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
    }
    LOG_INFO("{}: process {} set BGM sub-priority enabled={} priority={}", export_name, *pid, enabled != 0, priority);
    return STUBBED("BGM priority arbitration, permissions and audio adoption are not emulated");
}

EXPORT(int, sceAppMgrSetBgmSubPriorityForSystemChat, int enabled) {
    TRACY_FUNC(sceAppMgrSetBgmSubPriorityForSystemChat, enabled);
    return CALL_EXPORT(sceAppMgrSetBgmSubPriority, enabled, 194);
}

EXPORT(int, sceAppMgrSetDisplayMergeConf) {
    TRACY_FUNC(sceAppMgrSetDisplayMergeConf);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetFakeSettingBug51800) {
    TRACY_FUNC(sceAppMgrSetFakeSettingBug51800);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetInfobarState, int visibility, int color, int transparency) {
    TRACY_FUNC(sceAppMgrSetInfobarState, visibility, color, transparency);
    if (visibility < 0 || visibility > 1 || color < 0 || color > 1 || transparency < 0 || transparency > 1)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID);

    emuenv.appmgr_infobar_visibility.store(visibility, std::memory_order_relaxed);
    if (color != 0 || transparency != 0)
        LOG_WARN("sceAppMgrSetInfobarState color/transparency are accepted but not emulated");
    return 0;
}

EXPORT(int, sceAppMgrSetInfobarStateForCommonDialog) {
    TRACY_FUNC(sceAppMgrSetInfobarStateForCommonDialog);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetInfobarStateForShellByAppId) {
    TRACY_FUNC(sceAppMgrSetInfobarStateForShellByAppId);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetRecommendedScreenOrientationActivated) {
    TRACY_FUNC(sceAppMgrSetRecommendedScreenOrientationActivated);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetSystemImposeState) {
    TRACY_FUNC(sceAppMgrSetSystemImposeState);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetSystemImposeState2) {
    TRACY_FUNC(sceAppMgrSetSystemImposeState2);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSuspendBgAppByShell) {
    TRACY_FUNC(sceAppMgrSuspendBgAppByShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSuspendUntilActivated) {
    TRACY_FUNC(sceAppMgrSuspendUntilActivated);
    return UNIMPLEMENTED();
}
