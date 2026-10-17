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

#include "SceAppUtil.h"
#include "init.h"

#include <emuenv/app_util.h>

#include <io/device.h>
#include <io/functions.h>
#include <io/io.h>
#include <io/vfs.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <module/guest_printf.h>

#include <packages/license.h>
#include <packages/sfo.h>

#include "../SceAppMgr/SceAppMgr.h"
#include "../SceProcessmgr/SceProcessmgr.h"

#include <util/fs.h>
#include <util/safe_time.h>
#include <util/tracy.h>

#ifdef _WIN32
#include <winsock.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

TRACY_MODULE_NAME(SceAppUtil);

template <>
std::string to_debug_str<SceSystemParamId>(const MemState &mem, SceSystemParamId type) {
    switch (type) {
    case SCE_SYSTEM_PARAM_ID_LANG:
        return "SCE_SYSTEM_PARAM_ID_LANG";
    case SCE_SYSTEM_PARAM_ID_ENTER_BUTTON:
        return "SCE_SYSTEM_PARAM_ID_ENTER_BUTTON";
    case SCE_SYSTEM_PARAM_ID_USER_NAME:
        return "SCE_SYSTEM_PARAM_ID_USER_NAME";
    case SCE_SYSTEM_PARAM_ID_DATE_FORMAT:
        return "SCE_SYSTEM_PARAM_ID_DATE_FORMAT";
    case SCE_SYSTEM_PARAM_ID_TIME_FORMAT:
        return "SCE_SYSTEM_PARAM_ID_TIME_FORMAT";
    case SCE_SYSTEM_PARAM_ID_TIME_ZONE:
        return "SCE_SYSTEM_PARAM_ID_TIME_ZONE";
    case SCE_SYSTEM_PARAM_ID_SUMMERTIME:
        return "SCE_SYSTEM_PARAM_ID_SUMMERTIME";
    case SCE_SYSTEM_PARAM_ID_MAX_VALUE:
        return "SCE_SYSTEM_PARAM_ID_MAX_VALUE";
    }
    return std::to_string(type);
}

EXPORT(int, sceAppUtilAddCookieWebBrowser) {
    TRACY_FUNC(sceAppUtilAddCookieWebBrowser);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAddcontMount) {
    TRACY_FUNC(sceAppUtilAddcontMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAddcontUmount) {
    TRACY_FUNC(sceAppUtilAddcontUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseGameCustomData) {
    TRACY_FUNC(sceAppUtilAppEventParseGameCustomData);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseIncomingDialog) {
    TRACY_FUNC(sceAppUtilAppEventParseIncomingDialog);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseLiveArea) {
    TRACY_FUNC(sceAppUtilAppEventParseLiveArea);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseNearGift) {
    TRACY_FUNC(sceAppUtilAppEventParseNearGift);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseNpActivity) {
    TRACY_FUNC(sceAppUtilAppEventParseNpActivity);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseNpAppDataMessage) {
    TRACY_FUNC(sceAppUtilAppEventParseNpAppDataMessage);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseNpBasicJoinablePresence) {
    TRACY_FUNC(sceAppUtilAppEventParseNpBasicJoinablePresence);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseNpInviteMessage) {
    TRACY_FUNC(sceAppUtilAppEventParseNpInviteMessage);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseScreenShotNotification) {
    TRACY_FUNC(sceAppUtilAppEventParseScreenShotNotification);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseSessionInvitation) {
    TRACY_FUNC(sceAppUtilAppEventParseSessionInvitation);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseTeleport) {
    TRACY_FUNC(sceAppUtilAppEventParseTeleport);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseTriggerUtil) {
    TRACY_FUNC(sceAppUtilAppEventParseTriggerUtil);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseWebBrowser) {
    TRACY_FUNC(sceAppUtilAppEventParseWebBrowser);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceAppUtilAppParamGetInt, SceAppUtilAppParamId paramId, SceInt32 *value) {
    TRACY_FUNC(sceAppUtilAppParamGetInt, paramId, value);
    if (paramId != SCE_APPUTIL_APPPARAM_ID_SKU_FLAG)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    if (!value)
        return RET_ERROR(SCE_APPUTIL_ERROR_NOT_INITIALIZED);

    *value = emuenv.license.rif[emuenv.io.title_id].sku_flag;

    return 0;
}

EXPORT(int, sceAppUtilBgdlGetStatus) {
    TRACY_FUNC(sceAppUtilBgdlGetStatus);
    return UNIMPLEMENTED();
}

static bool make_drm_param(const SceAppUtilDrmAddcontId *dir_name, const SceAppUtilMountPoint *mount_point, SceAppMgrDrmOpenParam &param) {
    if (!dir_name || dir_name->data[16] != '\0'
        || !std::all_of(dir_name->data, dir_name->data + 16, [](char c) {
               return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
           }))
        return false;
    const char *point = "addcont0:";
    if (mount_point) {
        if (!std::memchr(mount_point->data, '\0', sizeof(mount_point->data)))
            return false;
        point = reinterpret_cast<const char *>(mount_point->data);
        if (std::strcmp(point, "addcont0:") != 0 && std::strcmp(point, "addcont1:") != 0
            && std::strcmp(point, "addcont2:") != 0)
            return false;
    }
    param.size = sizeof(param);
    std::memcpy(param.dir_name, dir_name->data, 16);
    std::strncpy(param.mount_point, point, sizeof(param.mount_point) - 1);
    return true;
}

EXPORT(SceInt32, sceAppUtilDrmClose, const SceAppUtilDrmAddcontId *dirName, const SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilDrmClose, dirName, mountPoint);
    SceAppMgrDrmOpenParam param{};
    if (!make_drm_param(dirName, mountPoint, param))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    const int result = CALL_EXPORT(sceAppMgrDrmClose, &param);
    if (result == SCE_ERROR_ERRNO_ENOENT || result == static_cast<int>(0x80800002u))
        return RET_ERROR(SCE_APPUTIL_ERROR_NOT_MOUNTED);
    if (result == SCE_ERROR_ERRNO_EBUSY)
        return RET_ERROR(SCE_APPUTIL_ERROR_BUSY);
    return result;
}

EXPORT(SceInt32, sceAppUtilDrmOpen, const SceAppUtilDrmAddcontId *dirName, const SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilDrmOpen, dirName, mountPoint);
    SceAppMgrDrmOpenParam param{};
    if (!make_drm_param(dirName, mountPoint, param))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    return CALL_EXPORT(sceAppMgrDrmOpen, &param);
}

EXPORT(int, sceAppUtilInit, Ptr<const void> initParam, Ptr<void> bootParam) {
    TRACY_FUNC(sceAppUtilInit, initParam, bootParam);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    auto &mem = thread->cpu->mem;
    if (emuenv.kernel.is_primary_address_space(mem))
        return UNIMPLEMENTED();
    auto *state = find_secondary_app_util_state(emuenv, mem);
    if (!state) {
        LOG_ERROR("sceAppUtilInit cannot find the secondary process");
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    }
    const uint32_t sdk_version = static_cast<uint32_t>(CALL_EXPORT(sceKernelGetMainModuleSdkVersion));
    const int result = initialize_secondary_app_util(mem, *state, sdk_version, initParam, bootParam, [&] {
        if (!state->io) {
            LOG_ERROR("sceAppUtilInit cannot initialize slots without a secondary savedata mount");
            return static_cast<int>(SCE_KERNEL_ERROR_NOT_IMPLEMENTED);
        }
        return initialize_savedata_slots(*state->io, emuenv.vita_fs_path);
    });
    if (result == 0)
        LOG_WARN("Secondary AppUtil initialized; other savedata imports are not yet isolated");
    return result;
}

EXPORT(int, sceAppUtilLaunchWebBrowser) {
    TRACY_FUNC(sceAppUtilLaunchWebBrowser);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilMusicMount) {
    TRACY_FUNC(sceAppUtilMusicMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilMusicUmount) {
    TRACY_FUNC(sceAppUtilMusicUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilPhotoMount) {
    TRACY_FUNC(sceAppUtilPhotoMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilPhotoUmount) {
    TRACY_FUNC(sceAppUtilPhotoUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilPspSaveDataGetDirNameList) {
    TRACY_FUNC(sceAppUtilPspSaveDataGetDirNameList);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilPspSaveDataLoad) {
    TRACY_FUNC(sceAppUtilPspSaveDataLoad);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilReceiveAppEvent) {
    TRACY_FUNC(sceAppUtilReceiveAppEvent);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilResetCookieWebBrowser) {
    TRACY_FUNC(sceAppUtilResetCookieWebBrowser);
    return UNIMPLEMENTED();
}

std::string construct_savedata0_path(const std::string &data, const char *ext) {
    return device::construct_normalized_path(VitaIoDevice::savedata0, data, ext);
}

std::string construct_slotparam_path(const unsigned int data) {
    return construct_savedata0_path("SlotParam_" + std::to_string(data), "bin");
}

constexpr size_t slot_flag_base = SDSLOT_FLAG_BASE;
constexpr size_t slot_data_base = SDSLOT_DATA_BASE;
constexpr size_t slot_block_size = SDSLOT_BLOCK_SIZE;
constexpr size_t sdslot_payload_size = slot_data_base + SCE_APPUTIL_SAVEDATA_SLOT_MAX * slot_block_size;
constexpr size_t sdslot_size = SDSLOT_TOTAL_SIZE;
constexpr std::array<uint8_t, 10> sdslot_magic{ 'S', 'D', 'S', 'L', 0, 0, 0, 0, 0, 1 };
static_assert(sizeof(SceAppUtilSaveDataSlotParam) == 844);
static std::recursive_mutex sdslot_mutex;
static std::atomic<uint64_t> sdslot_temp_counter{ 0 };

static std::string sdslot_path() {
    return construct_savedata0_path("sce_sys/sdslot", "dat");
}

static std::optional<fs::path> protected_savedata_relative(const char *path) {
    std::string_view relative(path);
    if (relative.starts_with('/'))
        relative.remove_prefix(1);
    if (relative.empty() || relative.starts_with('/') || relative.find_first_of("\\:") != std::string_view::npos)
        return std::nullopt;
    return fs_utils::utf8_to_path(std::string(relative));
}

static SaveDataSlotReadResult read_slot_file(EmuEnvState &emuenv, IOState &io, const std::string &path, void *data, size_t size, const char *export_name, size_t alternate_size = 0) try {
    bool mounted_savedata = false;
    {
        std::lock_guard lock(io.appmeta_mutex);
        mounted_savedata = !io.savedata_pfs_mount.empty() && path.starts_with("savedata0:");
    }
    if (!mounted_savedata) {
        const auto host_path = expand_path(io, path.c_str(), emuenv.vita_fs_path);
        if (host_path.empty()) {
            LOG_ERROR("{}: cannot resolve savedata file {}", export_name, path);
            return SaveDataSlotReadResult::Error;
        }
        std::error_code ec;
        const auto link_status = fs::symlink_status(host_path, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(link_status)))
            return SaveDataSlotReadResult::NotFound;
        if (ec) {
            LOG_ERROR("{}: cannot inspect savedata file {}: {}", export_name, path, ec.message());
            return SaveDataSlotReadResult::Error;
        }
        if (fs::is_symlink(link_status)) {
            LOG_ERROR("{}: refusing to replace or migrate a symbolic-link savedata file {}", export_name, path);
            return SaveDataSlotReadResult::Error;
        }
        const auto status = fs::status(host_path, ec);
        if (ec || !fs::is_regular_file(status)) {
            LOG_ERROR("{}: cannot inspect savedata file {}: {}", export_name, path, ec ? ec.message() : "not a regular file");
            return SaveDataSlotReadResult::Error;
        }
    }
    const auto fd = open_file(io, path.c_str(), SCE_O_RDONLY, emuenv.vita_fs_path, export_name, mounted_savedata);
    if (mounted_savedata && fd == SCE_ERROR_ERRNO_ENOENT)
        return SaveDataSlotReadResult::NotFound;
    if (fd < 0) {
        LOG_ERROR("{}: cannot open savedata file {}", export_name, path);
        return SaveDataSlotReadResult::Error;
    }
    const auto file_size = seek_file(fd, 0, SCE_SEEK_END, io, export_name);
    const bool valid_size = file_size == static_cast<SceOff>(size)
        || (alternate_size != 0 && file_size == static_cast<SceOff>(alternate_size));
    const auto read_size = valid_size ? static_cast<SceSize>(file_size) : 0;
    const auto start = valid_size ? seek_file(fd, 0, SCE_SEEK_SET, io, export_name) : static_cast<SceOff>(-1);
    const int bytes_read = start == 0 ? read_file(data, io, fd, read_size, export_name) : -1;
    const bool valid = valid_size && start == 0 && bytes_read == static_cast<int>(read_size);
    const bool closed = close_file(io, fd, export_name) == 0;
    if (!valid || !closed) {
        LOG_ERROR("{}: invalid size or failed to read savedata file {} (size {}, expected {}, bytes read {}, mounted {})",
            export_name, path, file_size, size, bytes_read, mounted_savedata);
        return SaveDataSlotReadResult::Error;
    }
    return SaveDataSlotReadResult::Found;
} catch (const fs::filesystem_error &error) {
    LOG_ERROR("{}: cannot read savedata file {}: {}", export_name, path, error.what());
    return SaveDataSlotReadResult::Error;
}

static SaveDataSlotReadResult load_sdslot(EmuEnvState &emuenv, IOState &io, std::vector<uint8_t> &data, const char *export_name) {
    data.resize(sdslot_size);
    const auto result = read_slot_file(emuenv, io, sdslot_path(), data.data(), data.size(), export_name, sdslot_payload_size);
    if (result == SaveDataSlotReadResult::Found && !std::equal(sdslot_magic.begin(), sdslot_magic.end(), data.begin())) {
        LOG_ERROR("{}: invalid sdslot.dat magic; leaving the file unchanged", export_name);
        return SaveDataSlotReadResult::Error;
    }
    return result;
}

static SaveDataSlotReadResult read_slot_unlocked(EmuEnvState &emuenv, IOState &io, uint32_t slot_id, SceAppUtilSaveDataSlotParam &param, const char *export_name) {
    if (slot_id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX)
        return SaveDataSlotReadResult::Error;
    std::vector<uint8_t> data;
    const auto result = load_sdslot(emuenv, io, data, export_name);
    if (result == SaveDataSlotReadResult::NotFound)
        return read_slot_file(emuenv, io, construct_slotparam_path(slot_id), &param, sizeof(param), export_name);
    if (result == SaveDataSlotReadResult::Error || !data[slot_flag_base + slot_id])
        return result == SaveDataSlotReadResult::Error ? result : SaveDataSlotReadResult::NotFound;
    std::memcpy(&param, data.data() + slot_data_base + slot_id * slot_block_size, sizeof(param));
    return SaveDataSlotReadResult::Found;
}

static bool publish_sdslot(EmuEnvState &emuenv, IOState &io, const std::vector<uint8_t> &data, const char *export_name) try {
    bool protected_savedata;
    {
        const std::lock_guard lock(io.appmeta_mutex);
        protected_savedata = !io.savedata_pfs_mount.empty();
    }
    if (protected_savedata)
        return write_protected_savedata(io, fs::path("sce_sys") / "sdslot.dat", 0, data, data.size()) == 0;
    const auto parent = construct_savedata0_path("sce_sys");
    try {
        create_dir(io, parent.c_str(), 0777, emuenv.vita_fs_path, export_name, true);
    } catch (const fs::filesystem_error &error) {
        LOG_ERROR("{}: cannot create sdslot directory: {}", export_name, error.what());
        return false;
    }
    std::string temp_dir;
    for (unsigned int attempt = 0; attempt < 1024; ++attempt) {
        temp_dir = construct_savedata0_path("sce_sys/.vita3k-sdslot-" + std::to_string(sdslot_temp_counter++));
        int result;
        try {
            result = create_dir(io, temp_dir.c_str(), 0777, emuenv.vita_fs_path, export_name);
        } catch (const fs::filesystem_error &error) {
            LOG_ERROR("{}: cannot create temporary sdslot directory: {}", export_name, error.what());
            return false;
        }
        if (result == 0)
            break;
        if (result != SCE_ERROR_ERRNO_EEXIST) {
            LOG_ERROR("{}: cannot create temporary sdslot directory", export_name);
            return false;
        }
        temp_dir.clear();
    }
    if (temp_dir.empty()) {
        LOG_ERROR("{}: cannot reserve a temporary sdslot directory", export_name);
        return false;
    }
    const auto temp_file = temp_dir + "/sdslot.dat";
    const auto fd = open_file(io, temp_file.c_str(), SCE_O_WRONLY | SCE_O_CREAT, emuenv.vita_fs_path, export_name);
    const bool written = fd >= 0 && write_file(fd, data.data(), static_cast<SceSize>(data.size()), io, export_name) == static_cast<int>(data.size());
    const bool closed = fd >= 0 && close_file(io, fd, export_name) == 0;
    bool published = false;
    if (written && closed)
        published = rename(io, temp_file.c_str(), sdslot_path().c_str(), emuenv.vita_fs_path, export_name) == 0;
    if (!published) {
        LOG_ERROR("{}: failed to publish sdslot.dat; existing files were preserved", export_name);
        if (fd >= 0)
            remove_file(io, temp_file.c_str(), emuenv.vita_fs_path, export_name);
    }
    remove_dir(io, temp_dir.c_str(), emuenv.vita_fs_path, export_name);
    return published;
} catch (const fs::filesystem_error &error) {
    LOG_ERROR("{}: cannot publish sdslot.dat: {}", export_name, error.what());
    return false;
}

int write_savedata_slot(EmuEnvState &emuenv, uint32_t slot_id, const SceAppUtilSaveDataSlotParam *param, SaveDataSlotWriteMode mode, const SceDateTime *time, const char *export_name, IOState *io_state) {
    IOState &io = io_state ? *io_state : emuenv.io;
    if (slot_id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    const std::lock_guard lock(sdslot_mutex);
    std::vector<uint8_t> data;
    const auto result = load_sdslot(emuenv, io, data, export_name);
    if (result == SaveDataSlotReadResult::Error)
        return RET_ERROR(SCE_ERROR_ERRNO_EIO);
    if (result == SaveDataSlotReadResult::NotFound) {
        std::copy(sdslot_magic.begin(), sdslot_magic.end(), data.begin());
        for (uint32_t i = 0; i < SCE_APPUTIL_SAVEDATA_SLOT_MAX; ++i) {
            SceAppUtilSaveDataSlotParam legacy{};
            const auto legacy_result = read_slot_file(emuenv, io, construct_slotparam_path(i), &legacy, sizeof(legacy), export_name);
            if (legacy_result == SaveDataSlotReadResult::Error)
                return RET_ERROR(SCE_ERROR_ERRNO_EIO);
            if (legacy_result == SaveDataSlotReadResult::Found) {
                data[slot_flag_base + i] = 1;
                std::memcpy(data.data() + slot_data_base + i * slot_block_size, &legacy, sizeof(legacy));
            }
        }
    }
    const auto flag = slot_flag_base + slot_id;
    const auto offset = slot_data_base + slot_id * slot_block_size;
    if (mode == SaveDataSlotWriteMode::CreateOnly && data[flag])
        return RET_ERROR(SCE_APPUTIL_ERROR_SAVEDATA_SLOT_EXISTS);
    if ((mode == SaveDataSlotWriteMode::Set || mode == SaveDataSlotWriteMode::DeleteExisting
            || mode == SaveDataSlotWriteMode::UpdateTime)
        && !data[flag])
        return RET_ERROR(SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND);
    if (mode == SaveDataSlotWriteMode::Delete || mode == SaveDataSlotWriteMode::DeleteExisting) {
        data[flag] = 0;
    } else if (mode == SaveDataSlotWriteMode::UpdateTime) {
        SceAppUtilSaveDataSlotParam existing{};
        std::memcpy(&existing, data.data() + offset, sizeof(existing));
        existing.modifiedTime = *time;
        std::memcpy(data.data() + offset, &existing, sizeof(existing));
    } else {
        data[flag] = 1;
        std::memcpy(data.data() + offset, param, sizeof(*param));
    }
    return publish_sdslot(emuenv, io, data, export_name) ? 0 : RET_ERROR(SCE_ERROR_ERRNO_EIO);
}

SaveDataSlotReadResult read_savedata_slot(EmuEnvState &emuenv, uint32_t slot_id, SceAppUtilSaveDataSlotParam &param, const char *export_name, IOState *io_state) {
    IOState &io = io_state ? *io_state : emuenv.io;
    const std::lock_guard lock(sdslot_mutex);
    return read_slot_unlocked(emuenv, io, slot_id, param, export_name);
}

SaveDataSlotReadResult check_savedata_slot_file(EmuEnvState &emuenv, const char *export_name, IOState *io_state) {
    IOState &io = io_state ? *io_state : emuenv.io;
    const std::lock_guard lock(sdslot_mutex);
    std::vector<uint8_t> data;
    return load_sdslot(emuenv, io, data, export_name);
}

SaveDataSlotReadResult read_savedata_slot_flags(EmuEnvState &emuenv, SaveDataSlotFlags &flags, const char *export_name, IOState *io_state) {
    IOState &io = io_state ? *io_state : emuenv.io;
    const std::lock_guard lock(sdslot_mutex);
    std::vector<uint8_t> data;
    const auto result = load_sdslot(emuenv, io, data, export_name);
    flags.fill(0);
    if (result != SaveDataSlotReadResult::Found)
        return result;
    for (size_t i = 0; i < flags.size(); ++i)
        flags[i] = data[slot_flag_base + i] != 0;
    return SaveDataSlotReadResult::Found;
}

int execute_savedata_data_remove(EmuEnvState &emuenv, SceAppUtilSaveDataFileSlot *slot, SceAppUtilSaveDataRemoveItem *files, uint32_t fileNum, SceAppUtilMountPoint *mountPoint, const char *export_name) {
    if (slot && (slot->id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    if (fileNum && !files)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    bool protected_savedata;
    {
        const std::lock_guard lock(emuenv.io.appmeta_mutex);
        protected_savedata = !emuenv.io.savedata_pfs_mount.empty();
    }
    if (protected_savedata) {
        for (unsigned int i = 0; i < fileNum; ++i) {
            if (!files[i].dataPath)
                return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
            const auto path = protected_savedata_relative(files[i].dataPath.get(emuenv.mem));
            if (!path)
                return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
            const auto result = remove_protected_savedata(emuenv.io, *path);
            if (result != 0)
                return RET_ERROR(result);
        }
        if (slot && fileNum > 0 && files[0].mode == SCE_APPUTIL_SAVEDATA_DATA_REMOVE_MODE_DEFAULT) {
            const auto result = write_savedata_slot(emuenv, slot->id, nullptr, SaveDataSlotWriteMode::Delete, nullptr, export_name);
            return result != 0 ? RET_ERROR(result) : 0;
        }
        return 0;
    }
    if (slot && fileNum > 0 && files[0].mode == SCE_APPUTIL_SAVEDATA_DATA_REMOVE_MODE_DEFAULT) {
        const auto result = write_savedata_slot(emuenv, slot->id, nullptr, SaveDataSlotWriteMode::Delete, nullptr, export_name);
        if (result != 0)
            return RET_ERROR(result);
    }

    for (unsigned int i = 0; i < fileNum; i++) {
        const auto file = fs::path(construct_savedata0_path(files[i].dataPath.get(emuenv.mem)));
        if (fs::is_regular_file(file)) {
            remove_file(emuenv.io, file.string().c_str(), emuenv.vita_fs_path, export_name);
        } else
            remove_dir(emuenv.io, file.string().c_str(), emuenv.vita_fs_path, export_name);
    }

    return 0;
}

int execute_savedata_data_save(EmuEnvState &emuenv, MemState &mem, IOState &io, SceAppUtilSaveDataFileSlot *slot, Ptr<SceAppUtilSaveDataDataSaveItem> guest_files, uint32_t fileNum, SceAppUtilMountPoint *mountPoint, SceSize *requiredSizeKiB, const char *export_name) {
    if (slot && (slot->id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    if (fileNum && !guest_files)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    struct SaveItem {
        std::string path;
        std::vector<uint8_t> data;
        uint64_t offset;
        uint32_t size;
        int mode;
        bool has_buffer;
    };

    try {
        const uint64_t files_size = uint64_t(fileNum) * sizeof(SceAppUtilSaveDataDataSaveItem);
        if (files_size > std::numeric_limits<uint32_t>::max()
            || uint64_t(guest_files.address()) + files_size > uint64_t(std::numeric_limits<Address>::max()) + 1)
            return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

        std::vector<SaveItem> items;
        items.reserve(fileNum);
        for (uint32_t i = 0; i < fileNum; ++i) {
            const Address address = guest_files.address() + static_cast<Address>(uint64_t(i) * sizeof(SceAppUtilSaveDataDataSaveItem));
            std::array<uint8_t, sizeof(SceAppUtilSaveDataDataSaveItem)> bytes{};
            const GuestMemoryRange item_range{ address, static_cast<uint32_t>(bytes.size()) };
            if (!module::read_pinned_guest_memory(mem, item_range, [&] {
                    for (size_t j = 0; j < bytes.size(); ++j)
                        bytes[j] = *Ptr<const uint8_t>(address + static_cast<Address>(j)).get(mem);
                }))
                return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
            Address data_path_address{};
            Address buffer_address{};
            SceSize buffer_size{};
            uint32_t pad{};
            SceOff offset{};
            int mode{};
            std::memcpy(&data_path_address, bytes.data() + offsetof(SceAppUtilSaveDataDataSaveItem, dataPath), sizeof(data_path_address));
            std::memcpy(&buffer_address, bytes.data() + offsetof(SceAppUtilSaveDataDataSaveItem, buf), sizeof(buffer_address));
            std::memcpy(&buffer_size, bytes.data() + offsetof(SceAppUtilSaveDataDataSaveItem, bufSize), sizeof(buffer_size));
            std::memcpy(&pad, bytes.data() + offsetof(SceAppUtilSaveDataDataSaveItem, pad), sizeof(pad));
            std::memcpy(&offset, bytes.data() + offsetof(SceAppUtilSaveDataDataSaveItem, offset), sizeof(offset));
            std::memcpy(&mode, bytes.data() + offsetof(SceAppUtilSaveDataDataSaveItem, mode), sizeof(mode));
            const SceAppUtilSaveDataDataSaveItem item{
                Ptr<char>(data_path_address), Ptr<void>(buffer_address), buffer_size, pad, offset, mode, {}
            };
            if (!item.dataPath || item.offset < 0
                || (item.mode != SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_FILE
                    && item.mode != SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_FILE_TRUNCATE
                    && item.mode != SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_DIRECTORY)
                || (item.mode == SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_FILE && item.bufSize && !item.buf))
                return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

            SaveItem saved{ {}, {}, static_cast<uint64_t>(item.offset), item.bufSize, item.mode, bool(item.buf) };
            if (saved.offset > static_cast<uint64_t>(std::numeric_limits<SceOff>::max()) - saved.size
                || module::read_guest_printf_string(mem, item.dataPath.address(), 1024, false, saved.path) != module::GuestPrintfError::none)
                return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
            const auto relative = protected_savedata_relative(saved.path.c_str());
            if (!relative)
                return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
            for (const auto &component : *relative) {
                if (component == "..")
                    return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
            }
            if (item.buf && item.bufSize && item.mode != SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_DIRECTORY) {
                if (item.bufSize > std::numeric_limits<int32_t>::max())
                    return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
                saved.data.resize(item.bufSize);
                const GuestMemoryRange data_range{ item.buf.address(), item.bufSize };
                if (!module::read_pinned_guest_memory(mem, data_range, [&] {
                        uint32_t offset = 0;
                        while (offset < item.bufSize) {
                            const Address current = item.buf.address() + offset;
                            const uint32_t count = std::min<uint32_t>(item.bufSize - offset, KiB(4) - current % KiB(4));
                            std::memcpy(saved.data.data() + offset, Ptr<const uint8_t>(current).get(mem), count);
                            offset += count;
                        }
                    }))
                    return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
            }
            items.push_back(std::move(saved));
        }

        std::unique_lock slot_lock(sdslot_mutex, std::defer_lock);
        if (slot)
            slot_lock.lock();
        struct SlotPin {
            MemState &mem;
            uint64_t id = 0;
            ~SlotPin() {
                if (id)
                    unpin_guest_memory(mem, id);
            }
        } slot_pin{ mem };
        SceAppUtilSaveDataSlotParam slot_param{};
        if (slot && slot->slotParam) {
            const GuestMemoryRange range{ slot->slotParam.address(), sizeof(slot_param) };
            if (!module::read_pinned_guest_memory(mem, range, [&] {
                    auto *bytes = reinterpret_cast<uint8_t *>(&slot_param);
                    for (size_t j = 0; j < sizeof(slot_param); ++j)
                        bytes[j] = *Ptr<const uint8_t>(range.address + static_cast<Address>(j)).get(mem);
                }))
                return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
            const GuestMemoryRange output{ range.address + static_cast<Address>(offsetof(SceAppUtilSaveDataSlotParam, modifiedTime)), sizeof(SceDateTime) };
            slot_pin.id = pin_guest_memory(mem, std::span(&output, 1));
            if (!slot_pin.id)
                return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
            {
                const std::lock_guard lock(mem.protect_mutex);
                const uint64_t end = uint64_t(output.address) + output.size;
                for (const auto &[start, segment] : mem.protect_tree) {
                    if ((segment.perm == MemPerm::None || segment.perm == MemPerm::ReadOnly)
                        && uint64_t(start) < end && uint64_t(start) + segment.size > output.address)
                        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
                }
            }
        } else if (slot) {
            const auto result = read_savedata_slot(emuenv, slot->id, slot_param, export_name, &io);
            if (result == SaveDataSlotReadResult::NotFound)
                return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
            if (result == SaveDataSlotReadResult::Error)
                return RET_ERROR(SCE_ERROR_ERRNO_EIO);
        }
        bool protected_savedata;
        {
            const std::lock_guard lock(io.appmeta_mutex);
            protected_savedata = !io.savedata_pfs_mount.empty();
        }
        if (requiredSizeKiB)
            *requiredSizeKiB = 0;
        for (const auto &item : items) {
            const auto relative = *protected_savedata_relative(item.path.c_str());
            if (protected_savedata) {
                if (item.mode == SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_DIRECTORY) {
                    const int result = create_protected_savedata_directories(io, relative);
                    if (result != 0)
                        return RET_ERROR(result);
                    continue;
                }
                if (!relative.parent_path().empty()) {
                    const int result = create_protected_savedata_directories(io, relative.parent_path());
                    if (result != 0)
                        return RET_ERROR(result);
                }
                const auto truncate_size = item.mode == SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_FILE_TRUNCATE
                    ? std::optional<uint64_t>(item.offset + item.size)
                    : std::nullopt;
                const int result = write_protected_savedata(io, relative, item.offset, item.data, truncate_size);
                if (result != 0)
                    return RET_ERROR(result);
                continue;
            }
            const auto path = construct_savedata0_path(item.path);
            if (item.mode == SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_DIRECTORY) {
                const int result = create_dir(io, path.c_str(), 0777, emuenv.vita_fs_path, export_name);
                if (result != 0 && result != SCE_ERROR_ERRNO_EEXIST)
                    return RET_ERROR(result);
                continue;
            }
            const auto fd = open_file(io, path.c_str(), SCE_O_WRONLY | SCE_O_CREAT, emuenv.vita_fs_path, export_name);
            if (fd < 0)
                return RET_ERROR(fd);
            int result = 0;
            if (item.has_buffer && (seek_file(fd, item.offset, SCE_SEEK_SET, io, export_name) < 0 || write_file(fd, item.data.data(), item.size, io, export_name) != static_cast<int>(item.size)))
                result = SCE_ERROR_ERRNO_EIO;
            if (!result && item.mode == SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_FILE_TRUNCATE)
                result = truncate_file(fd, item.offset + item.size, io, export_name);
            const int close_result = close_file(io, fd, export_name);
            if (result != 0 || close_result != 0)
                return RET_ERROR(result != 0 ? result : close_result);
        }
        if (slot) {
            std::time_t time = std::time(nullptr);
            tm local{};
            SAFE_LOCALTIME(&time, &local);
            SceDateTime modified_time{};
            modified_time.year = local.tm_year + 1900;
            modified_time.month = local.tm_mon + 1;
            modified_time.day = local.tm_mday;
            modified_time.hour = local.tm_hour;
            modified_time.minute = local.tm_min;
            modified_time.second = local.tm_sec;
            if (slot->slotParam)
                slot_param.modifiedTime = modified_time;
            const auto result = write_savedata_slot(emuenv, slot->id, slot->slotParam ? &slot_param : nullptr,
                slot->slotParam ? SaveDataSlotWriteMode::Create : SaveDataSlotWriteMode::UpdateTime, &modified_time, export_name, &io);
            if (result != 0 && !(!slot->slotParam && result == static_cast<int>(SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND)))
                return RET_ERROR(result);
            if (slot->slotParam) {
                const GuestMemoryRange range{ slot->slotParam.address() + static_cast<Address>(offsetof(SceAppUtilSaveDataSlotParam, modifiedTime)), sizeof(modified_time) };
                bool writable = true;
                {
                    const std::lock_guard lock(mem.protect_mutex);
                    const uint64_t end = uint64_t(range.address) + range.size;
                    for (const auto &[start, segment] : mem.protect_tree) {
                        if ((segment.perm == MemPerm::None || segment.perm == MemPerm::ReadOnly)
                            && uint64_t(start) < end && uint64_t(start) + segment.size > range.address)
                            writable = false;
                    }
                    if (writable) {
                        const auto *bytes = reinterpret_cast<const uint8_t *>(&modified_time);
                        for (size_t j = 0; j < sizeof(modified_time); ++j)
                            *Ptr<uint8_t>(range.address + static_cast<Address>(j)).get(mem) = bytes[j];
                    }
                }
                if (!writable)
                    return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
            }
        }
        return 0;
    } catch (const std::bad_alloc &) {
        LOG_ERROR("{}: cannot allocate savedata buffers", export_name);
        return RET_ERROR(SCE_ERROR_ERRNO_ENOMEM);
    }
}

EXPORT(int, sceAppUtil_CD0E5C59, Ptr<void> callback, SceUInt32 callbackArg) {
    TRACY_FUNC(sceAppUtil_CD0E5C59, callback.address(), callbackArg);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    auto *state = find_secondary_app_util_state(emuenv, thread->cpu->mem);
    if (!state)
        return RET_ERROR(SCE_APPUTIL_ERROR_NOT_INITIALIZED);

    const std::lock_guard lock(state->mutex);
    if (!state->initialized)
        return RET_ERROR(SCE_APPUTIL_ERROR_NOT_INITIALIZED);
    state->savedata_callback = callback.address();
    state->savedata_callback_arg = callbackArg;
    return 0;
}

EXPORT(int, sceAppUtil_C3CB128B, SceInt32 arg, Ptr<char> mountPoint) {
    TRACY_FUNC(sceAppUtil_C3CB128B, arg, mountPoint.address());
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    auto &mem = thread->cpu->mem;
    auto *state = find_secondary_app_util_state(emuenv, mem);
    if (!state)
        return RET_ERROR(SCE_APPUTIL_ERROR_NOT_INITIALIZED);

    const std::lock_guard lock(state->mutex);
    if (!state->initialized)
        return RET_ERROR(SCE_APPUTIL_ERROR_NOT_INITIALIZED);
    if (arg != 0xCA || !mountPoint)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    constexpr char savedata_root[] = "savedata0:";
    std::array<char, 16> root_mount{};
    std::copy_n(savedata_root, sizeof(savedata_root), root_mount.begin());
    const std::array ranges{ GuestMemoryRange{ mountPoint.address(), root_mount.size() } };
    const uint64_t pin = pin_guest_memory(mem, ranges);
    if (!pin)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    auto *output = mountPoint.get(mem);
    if (!output) {
        unpin_guest_memory(mem, pin);
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    }
    std::memcpy(output, root_mount.data(), root_mount.size());
    unpin_guest_memory(mem, pin);
    ++state->savedata_root_mount_count;
    return 0;
}

EXPORT(int, sceAppUtil_C0418B1C, SceInt32 arg) {
    TRACY_FUNC(sceAppUtil_C0418B1C, arg);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    auto *state = find_secondary_app_util_state(emuenv, thread->cpu->mem);
    if (!state)
        return RET_ERROR(SCE_APPUTIL_ERROR_NOT_INITIALIZED);

    const std::lock_guard lock(state->mutex);
    if (!state->initialized)
        return RET_ERROR(SCE_APPUTIL_ERROR_NOT_INITIALIZED);
    if (arg != 0xCA)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    if (state->savedata_root_mount_count > 0)
        --state->savedata_root_mount_count;
    return 0;
}

EXPORT(int, sceAppUtilSaveDataDataRemove, SceAppUtilSaveDataFileSlot *slot, Ptr<SceAppUtilSaveDataRemoveItem> files, unsigned int fileNum, SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataDataRemove, slot, files, fileNum, mountPoint);
    if ((slot && slot->id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX) || (fileNum && !files))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    SceAppMgrSaveDataDataDelete data{};
    data.size = sizeof(data);
    data.slot_id = slot ? slot->id : UINT32_MAX;
    if (slot)
        data.slot_param = slot->slotParam;
    data.files = files;
    data.file_count = fileNum;
    if (mountPoint)
        data.mount_point = *mountPoint;
    const auto result = CALL_EXPORT(sceAppMgrSaveDataDataRemove, &data);
    if (result == static_cast<int>(SCE_APPMGR_ERROR_SAVEDATA_INVALID_PARAMETER))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    return result != 0 ? RET_ERROR(result) : 0;
}

EXPORT(int, sceAppUtilSaveDataDataSave, SceAppUtilSaveDataFileSlot *slot, Ptr<SceAppUtilSaveDataDataSaveItem> files, unsigned int fileNum, SceAppUtilMountPoint *mountPoint, SceSize *requiredSizeKiB) {
    TRACY_FUNC(sceAppUtilSaveDataDataSave, slot, files, fileNum, mountPoint, requiredSizeKiB);
    if ((slot && slot->id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX) || (fileNum && !files))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    SceAppMgrSaveDataData data{};
    data.size = sizeof(data);
    data.slot_id = slot ? slot->id : UINT32_MAX;
    if (slot)
        data.slot_param = slot->slotParam;
    data.files = files;
    data.file_count = fileNum;
    if (mountPoint)
        data.mount_point = *mountPoint;
    const auto result = CALL_EXPORT(sceAppMgrSaveDataDataSave, &data);
    if (requiredSizeKiB)
        *requiredSizeKiB = data.required_size_kib;
    if (result == static_cast<int>(SCE_APPMGR_ERROR_SAVEDATA_INVALID_PARAMETER))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    return result != 0 ? RET_ERROR(result) : 0;
}

EXPORT(int, sceAppUtilSaveDataGetQuota, SceSize *quotaSizeKiB, SceSize *usedSizeKiB, const SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataGetQuota, quotaSizeKiB, usedSizeKiB, mountPoint);

    if (!quotaSizeKiB && !usedSizeKiB)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    SceAppMgrSaveDataQuota data{};
    data.size = sizeof(data);
    if (mountPoint)
        data.mount_point = *mountPoint;
    const auto result = CALL_EXPORT(sceAppMgrSaveDataGetQuota, &data);
    if (result == 0) {
        if (quotaSizeKiB)
            *quotaSizeKiB = data.quota_size_kib;
        if (usedSizeKiB)
            *usedSizeKiB = data.used_size_kib;
        if (quotaSizeKiB && usedSizeKiB && *quotaSizeKiB > 0 && *usedSizeKiB > *quotaSizeKiB)
            *usedSizeKiB = *quotaSizeKiB;
    }
    return result != 0 ? RET_ERROR(result) : 0;
}

EXPORT(int, sceAppUtilSaveDataMount) {
    TRACY_FUNC(sceAppUtilSaveDataMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilSaveDataSlotCreate, unsigned int slotId, SceAppUtilSaveDataSlotParam *param, SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataSlotCreate, slotId, param, mountPoint);
    if (!param || slotId >= SCE_APPUTIL_SAVEDATA_SLOT_MAX)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    SceAppMgrSaveDataSlot data{};
    data.size = sizeof(data);
    data.slot_id = slotId;
    data.param.app_util = *param;
    if (mountPoint)
        data.mount_point = *mountPoint;
    const auto result = CALL_EXPORT(sceAppMgrSaveDataSlotCreate, &data);
    if (result == static_cast<int>(SCE_APPMGR_ERROR_SAVEDATA_SLOT_EXISTS))
        return RET_ERROR(SCE_APPUTIL_ERROR_SAVEDATA_SLOT_EXISTS);
    return result != 0 ? RET_ERROR(result) : 0;
}

EXPORT(int, sceAppUtilSaveDataSlotDelete, unsigned int slotId, SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataSlotDelete, slotId, mountPoint);
    if (slotId >= SCE_APPUTIL_SAVEDATA_SLOT_MAX)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    SceAppMgrSaveDataSlotDelete data{};
    data.size = sizeof(data);
    data.slot_id = slotId;
    if (mountPoint)
        data.mount_point = *mountPoint;
    const auto result = CALL_EXPORT(sceAppMgrSaveDataSlotDelete, &data);
    if (result == static_cast<int>(SCE_APPMGR_ERROR_SAVEDATA_SLOT_NOT_FOUND))
        return RET_ERROR(SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND);
    return result != 0 ? RET_ERROR(result) : 0;
}

EXPORT(int, sceAppUtilSaveDataSlotGetParam, unsigned int slotId, SceAppUtilSaveDataSlotParam *param, SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataSlotGetParam, slotId, param, mountPoint);
    if (!param || slotId >= SCE_APPUTIL_SAVEDATA_SLOT_MAX)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    MemState &mem = context->thread->cpu->mem;
    const Address param_address = Ptr<SceAppUtilSaveDataSlotParam>(param, mem).address();
    if (!is_valid_addr_len(mem, param_address, sizeof(*param)))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    SceAppMgrSaveDataSlot data{};
    data.size = sizeof(data);
    data.slot_id = slotId;
    if (mountPoint && !read_guest_memory(mem, Ptr<SceAppUtilMountPoint>(mountPoint, mem).address(), &data.mount_point, sizeof(data.mount_point)))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    const auto result = CALL_EXPORT(sceAppMgrSaveDataSlotGetParam, &data);
    if (result == static_cast<int>(SCE_APPMGR_ERROR_SAVEDATA_SLOT_NOT_FOUND))
        return RET_ERROR(SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND);
    if (result == 0) {
        data.param.app_util.status = 0;
        if (!write_guest_memory(mem, param_address, &data.param.app_util, sizeof(data.param.app_util)))
            return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    }
    return result != 0 ? RET_ERROR(result) : 0;
}

EXPORT(SceInt32, sceAppUtilSaveDataSlotSearch, Ptr<SceAppUtilWorkBuffer> workBuf, Ptr<const SceAppUtilSaveDataSlotSearchCond> cond,
    Ptr<SceAppUtilSlotSearchResult> result, Ptr<const SceAppUtilMountPoint> mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataSlotSearch, workBuf, cond, result, mountPoint);

    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return RET_ERROR(context.error());
    MemState &mem = context->thread->cpu->mem;
    if (!cond || !result)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    SceAppUtilSaveDataSlotSearchCond search_cond{};
    SceAppUtilSlotSearchResult search_result{};
    SceAppUtilWorkBuffer work_buffer{};
    SceAppUtilMountPoint mount_point{};
    if (!read_guest_memory(mem, cond.address(), &search_cond, sizeof(search_cond))
        || !read_guest_memory(mem, result.address(), &search_result, sizeof(search_result))
        || (workBuf && !read_guest_memory(mem, workBuf.address(), &work_buffer, sizeof(work_buffer)))
        || (mountPoint && !read_guest_memory(mem, mountPoint.address(), &mount_point, sizeof(mount_point))))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    if (search_cond.from > SCE_APPUTIL_SAVEDATA_SLOT_MAX
        || search_cond.range > SCE_APPUTIL_SAVEDATA_SLOT_MAX - search_cond.from
        || (search_cond.type != SCE_APPUTIL_SAVEDATA_SLOT_SEARCH_TYPE_EXIST_SLOT && search_cond.type != SCE_APPUTIL_SAVEDATA_SLOT_SEARCH_TYPE_EMPTY_SLOT)
        || search_cond.key < SCE_APPUTIL_SAVEDATA_SLOT_SORT_KEY_SLOT_ID || search_cond.key > SCE_APPUTIL_SAVEDATA_SLOT_SORT_KEY_MODIFIED_TIME
        || (search_cond.order != SCE_APPUTIL_SAVEDATA_SLOT_SORT_TYPE_ASCENT && search_cond.order != SCE_APPUTIL_SAVEDATA_SLOT_SORT_TYPE_DESCENT)
        || (workBuf && work_buffer.bufSize < search_cond.range * sizeof(SceAppUtilSaveDataSlot)))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    if (workBuf)
        search_result.slotList = work_buffer.buf.cast<SceAppUtilSaveDataSlot>();

    search_result.hitNum = 0;
    if (!write_guest_memory(mem, result.address(), &search_result, sizeof(search_result)))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    auto slotList = search_result.slotList;
    const auto output_size = search_cond.range * sizeof(SceAppUtilSaveDataSlot);
    if (slotList && output_size && !is_valid_addr_len(mem, slotList.address(), output_size))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    std::vector<SceAppUtilSaveDataSlot> output_slots(search_cond.range);
    for (auto &slot : output_slots) {
        slot.id = -1;
        slot.emptyParam = Ptr<SceAppUtilSaveDataSlotEmptyParam>(0);
    }
    if (slotList && output_size
        && !write_guest_memory(mem, slotList.address(), output_slots.data(), static_cast<uint32_t>(output_size)))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    const std::lock_guard lock(sdslot_mutex);

    struct SearchEntry {
        SceAppUtilSaveDataSlot slot;
        SceAppUtilSaveDataSlotParam param;
    };
    std::vector<SearchEntry> matches;
    matches.reserve(search_cond.range);

    for (auto i = search_cond.from; i < (search_cond.from + search_cond.range); i++) {
        SceAppMgrSaveDataSlot data{};
        data.size = sizeof(data);
        data.slot_id = i;
        if (mountPoint)
            data.mount_point = mount_point;
        const auto slot_result = CALL_EXPORT(sceAppMgrSaveDataSlotGetParam, &data);
        if (slot_result != 0 && slot_result != static_cast<int>(SCE_APPMGR_ERROR_SAVEDATA_SLOT_NOT_FOUND))
            return slot_result;
        switch (search_cond.type) {
        case SCE_APPUTIL_SAVEDATA_SLOT_SEARCH_TYPE_EXIST_SLOT:
            if (slot_result == 0) {
                SceAppUtilSaveDataSlot slot{};
                slot.id = i;
                slot.status = data.param.app_util.status;
                slot.userParam = data.param.app_util.userParam;
                matches.push_back({ slot, data.param.app_util });
            }
            break;
        case SCE_APPUTIL_SAVEDATA_SLOT_SEARCH_TYPE_EMPTY_SLOT:
            if (slot_result == static_cast<int>(SCE_APPMGR_ERROR_SAVEDATA_SLOT_NOT_FOUND)) {
                SceAppUtilSaveDataSlot slot{};
                slot.id = i;
                matches.push_back({ slot, {} });
            }
            break;
        default: break;
        }
    }

    const auto compare_value = [](const auto &left, const auto &right) {
        return left < right ? -1 : (right < left ? 1 : 0);
    };
    const auto compare_date_time = [&compare_value](const SceDateTime &left, const SceDateTime &right) {
        if (const auto order = compare_value(left.year, right.year); order != 0)
            return order;
        if (const auto order = compare_value(left.month, right.month); order != 0)
            return order;
        if (const auto order = compare_value(left.day, right.day); order != 0)
            return order;
        if (const auto order = compare_value(left.hour, right.hour); order != 0)
            return order;
        if (const auto order = compare_value(left.minute, right.minute); order != 0)
            return order;
        if (const auto order = compare_value(left.second, right.second); order != 0)
            return order;
        return compare_value(left.microsecond, right.microsecond);
    };
    std::stable_sort(matches.begin(), matches.end(), [&](const SearchEntry &left, const SearchEntry &right) {
        int order = 0;
        switch (search_cond.key) {
        case SCE_APPUTIL_SAVEDATA_SLOT_SORT_KEY_SLOT_ID:
            order = compare_value(left.slot.id, right.slot.id);
            break;
        case SCE_APPUTIL_SAVEDATA_SLOT_SORT_KEY_USER_PARAM:
            order = compare_value(left.param.userParam, right.param.userParam);
            break;
        case SCE_APPUTIL_SAVEDATA_SLOT_SORT_KEY_SIZE_KIB:
            order = compare_value(left.param.sizeKiB, right.param.sizeKiB);
            break;
        case SCE_APPUTIL_SAVEDATA_SLOT_SORT_KEY_MODIFIED_TIME:
            order = compare_date_time(left.param.modifiedTime, right.param.modifiedTime);
            break;
        default:
            break;
        }
        return search_cond.order == SCE_APPUTIL_SAVEDATA_SLOT_SORT_TYPE_ASCENT ? order < 0 : order > 0;
    });

    for (size_t i = 0; i < matches.size(); i++)
        output_slots[i] = matches[i].slot;

    if (slotList && output_size
        && !write_guest_memory(mem, slotList.address(), output_slots.data(), static_cast<uint32_t>(output_size)))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    search_result.hitNum = static_cast<SceUInt32>(matches.size());
    if (!write_guest_memory(mem, result.address(), &search_result, sizeof(search_result)))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    return 0;
}

EXPORT(SceInt32, sceAppUtilSaveDataSlotSetParam, SceAppUtilSaveDataSlotId slotId, SceAppUtilSaveDataSlotParam *param, SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataSlotSetParam, slotId, param, mountPoint);
    if (!param || slotId >= SCE_APPUTIL_SAVEDATA_SLOT_MAX)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    SceAppMgrSaveDataSlot data{};
    data.size = sizeof(data);
    data.slot_id = slotId;
    data.param.app_util = *param;
    if (mountPoint)
        data.mount_point = *mountPoint;
    const auto result = CALL_EXPORT(sceAppMgrSaveDataSlotSetParam, &data);
    if (result == static_cast<int>(SCE_APPMGR_ERROR_SAVEDATA_SLOT_NOT_FOUND))
        return RET_ERROR(SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND);
    return result != 0 ? RET_ERROR(result) : 0;
}

EXPORT(int, sceAppUtilSaveDataUmount) {
    TRACY_FUNC(sceAppUtilSaveDataUmount);
    return UNIMPLEMENTED();
}

SceInt32 execute_savedata_safe_memory(EmuEnvState &emuenv, IOState &io, void *buf, SceSize bufSize, SceOff offset, const char *export_name, bool save) {
    const std::lock_guard savedata_lock(io.savedata_mutex);
    bool protected_savedata;
    {
        const std::lock_guard lock(io.appmeta_mutex);
        protected_savedata = !io.savedata_pfs_mount.empty();
    }
    const auto path = construct_savedata0_path("sce_sys/safemem", "dat");
    std::vector<uint8_t> data;
    int open_error = 0;
    // Open and read the existing safe memory file, if present.
    const bool found = read_file_to_buffer(io, path.c_str(), emuenv.vita_fs_path, data, export_name,
        SCE_APPUTIL_SAFEMEMORY_MEMORY_SIZE, &open_error, true);
    if (!found && open_error != SCE_ERROR_ERRNO_ENOENT)
        return RET_ERROR(SCE_ERROR_ERRNO_EIO);
    // Load can return 0 when the file does not exist.
    if ((!found || data.empty()) && !save)
        return 0;
    data.resize(SCE_APPUTIL_SAFEMEMORY_MEMORY_SIZE);
    if (!save) {
        std::memcpy(buf, data.data() + offset, bufSize);
        return bufSize;
    }
    // Create or update safe memory with the supplied buffer.
    std::memcpy(data.data() + offset, buf, bufSize);
    if (protected_savedata) {
        const auto result = write_protected_savedata(io, fs::path("sce_sys") / "safemem.dat", 0, data, data.size());
        return result != 0 ? RET_ERROR(result) : static_cast<SceInt32>(bufSize);
    }
    const auto fd = open_file(io, path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, emuenv.vita_fs_path, export_name);
    if (fd < 0)
        return RET_ERROR(fd);
    const auto written = write_file(fd, data.data(), static_cast<SceSize>(data.size()), io, export_name);
    const auto closed = close_file(io, fd, export_name);
    if (written < 0)
        return RET_ERROR(written);
    if (static_cast<size_t>(written) != data.size())
        return RET_ERROR(SCE_ERROR_ERRNO_EIO);
    if (closed < 0)
        return RET_ERROR(closed);
    return bufSize;
}

EXPORT(SceInt32, sceAppUtilLoadSafeMemory, void *buf, SceSize bufSize, SceOff offset) {
    TRACY_FUNC(sceAppUtilLoadSafeMemory, buf, bufSize, offset);
    if (!buf || offset < 0 || static_cast<uint64_t>(offset) + bufSize > SCE_APPUTIL_SAFEMEMORY_MEMORY_SIZE)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    const auto result = CALL_EXPORT(sceAppMgrLoadSafeMemory, buf, bufSize, offset);
    return result < 0 ? RET_ERROR(result) : result;
}

EXPORT(SceInt32, sceAppUtilSaveSafeMemory, const void *buf, SceSize bufSize, SceOff offset) {
    TRACY_FUNC(sceAppUtilSaveSafeMemory, buf, bufSize, offset);
    if (!buf || offset < 0 || static_cast<uint64_t>(offset) + bufSize > SCE_APPUTIL_SAFEMEMORY_MEMORY_SIZE)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    const auto result = CALL_EXPORT(sceAppMgrSaveSafeMemory, buf, bufSize, offset);
    return result < 0 ? RET_ERROR(result) : result;
}

EXPORT(int, sceAppUtilShutdown) {
    TRACY_FUNC(sceAppUtilShutdown);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    if (emuenv.kernel.is_primary_address_space(thread->cpu->mem))
        return UNIMPLEMENTED();
    auto *state = find_secondary_app_util_state(emuenv, thread->cpu->mem);
    if (!state) {
        LOG_ERROR("sceAppUtilShutdown cannot find the secondary process");
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    }
    return shutdown_secondary_app_util(*state);
}

EXPORT(int, sceAppUtilStoreBrowse) {
    TRACY_FUNC(sceAppUtilStoreBrowse);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceAppUtilSystemParamGetInt, SceSystemParamId paramId, Ptr<SceInt32> value) {
    TRACY_FUNC(sceAppUtilSystemParamGetInt, paramId, value);
    if (!value)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    SceInt32 result;
    switch (paramId) {
    case SCE_SYSTEM_PARAM_ID_LANG:
        result = (SceSystemParamLang)emuenv.cfg.sys_lang;
        break;
    case SCE_SYSTEM_PARAM_ID_ENTER_BUTTON:
        result = (SceSystemParamEnterButtonAssign)emuenv.cfg.sys_button;
        break;
    case SCE_SYSTEM_PARAM_ID_DATE_FORMAT:
        result = (SceSystemParamDateFormat)emuenv.cfg.sys_date_format;
        break;
    case SCE_SYSTEM_PARAM_ID_TIME_FORMAT:
        result = (SceSystemParamTimeFormat)emuenv.cfg.sys_time_format;
        break;
    case SCE_SYSTEM_PARAM_ID_TIME_ZONE:
    case SCE_SYSTEM_PARAM_ID_SUMMERTIME:
        STUBBED("No support Time Zone and Summer Time, give 0 value");
        result = 0;
        break;
    default:
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    }
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    MemState &mem = thread->cpu->mem;
    if (emuenv.kernel.is_primary_address_space(mem)) {
        *value.get(mem) = result;
        return 0;
    }
    const GuestMemoryRange range{ value.address(), sizeof(SceInt32) };
    const uint64_t pin = pin_guest_memory(mem, std::span(&range, 1));
    if (!pin)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    bool writable = true;
    {
        const std::lock_guard lock(mem.protect_mutex);
        const uint64_t end = uint64_t(range.address) + range.size;
        for (const auto &[start, segment] : mem.protect_tree) {
            if ((segment.perm == MemPerm::None || segment.perm == MemPerm::ReadOnly)
                && uint64_t(start) < end && uint64_t(start) + segment.size > range.address) {
                writable = false;
                break;
            }
        }
        if (writable) {
            const auto *bytes = reinterpret_cast<const uint8_t *>(&result);
            for (uint32_t i = 0; i < sizeof(SceInt32); ++i)
                *Ptr<uint8_t>(value.address() + i).get(mem) = bytes[i];
        }
    }
    unpin_guest_memory(mem, pin);
    if (!writable)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    return 0;
}

EXPORT(int, sceAppUtilSystemParamGetString, unsigned int paramId, SceChar8 *buf, SceSize bufSize) {
    TRACY_FUNC(sceAppUtilSystemParamGetString, paramId, buf, bufSize);
    switch (paramId) {
    case SCE_SYSTEM_PARAM_ID_USER_NAME: {
        if (bufSize == 0) {
            return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
        }
        // copy user name into buf with size SCE_SYSTEM_PARAM_USERNAME_MAXSIZE or bufSize, whichever is smaller
        const auto copySize = std::min(bufSize, static_cast<unsigned int>(SCE_SYSTEM_PARAM_USERNAME_MAXSIZE));
        std::strncpy(reinterpret_cast<char *>(buf), emuenv.io.user_name.c_str(), copySize - 1);
        buf[copySize - 1] = '\0';
        // fallback to hostname
        if (buf[0] == '\0') {
            char devname[SCE_SYSTEM_PARAM_USERNAME_MAXSIZE];
            if (gethostname(devname, SCE_SYSTEM_PARAM_USERNAME_MAXSIZE) == 0) {
                std::strncpy(reinterpret_cast<char *>(buf), devname, copySize - 1);
                buf[copySize - 1] = '\0';
            }
            // double fallback to "Vita3K"
            if (buf[0] == '\0') {
                std::strncpy(reinterpret_cast<char *>(buf), "Vita3K", copySize - 1);
                buf[copySize - 1] = '\0';
            }
        }
        break;
    }
    default:
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    }
    return 0;
}
