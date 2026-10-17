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

#include "SceAppMgrUser.h"

#include <config/config.h>
#include <emuenv/app_util.h>
#include <io/device.h>
#include <io/functions.h>
#include <io/io.h>
#include <io/vfs.h>
#include <kernel/state.h>
#include <packages/functions.h>
#include <packages/sfo.h>

#include <modules/module_parent.h>
#include <util/find.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceAppMgrUser);

struct SceAppMgrInfobarState {
    uint8_t visibility;
    uint8_t color;
    uint8_t transparency;
    uint8_t reserved;
};

struct SceAppMgrAppStatus {
    SceUInt32 launchSourceProcessId;
    SceUInt32 launchMode;
    SceUInt32 protectionMode;
    char appName[32];
    SceUInt32 paramDeliveryHandled;
    SceUID appId;
    SceUID processId;
    SceUInt32 activated;
    SceUInt32 processRunRequested;
    SceUInt32 networkDisconnectionWarningDialogEnabled;
    SceUInt32 launchSourceAppId;
    SceAppMgrInfobarState infobar;
    SceUInt32 launchArgumentMarker;
    char bgmProxyAppName[32];
    uint8_t launchOrigin;
    uint8_t budgetType;
    uint8_t padding_72[2];
    uint32_t processAttr;
    uint32_t systemOrientation;
    uint32_t recommendedOrientationActivated;
};

static_assert(sizeof(SceAppMgrInfobarState) == 4);
static_assert(sizeof(SceAppMgrAppStatus) == 0x80);
static_assert(offsetof(SceAppMgrAppStatus, appName) == 0x0c);
static_assert(offsetof(SceAppMgrAppStatus, paramDeliveryHandled) == 0x2c);
static_assert(offsetof(SceAppMgrAppStatus, appId) == 0x30);
static_assert(offsetof(SceAppMgrAppStatus, processId) == 0x34);
static_assert(offsetof(SceAppMgrAppStatus, activated) == 0x38);
static_assert(offsetof(SceAppMgrAppStatus, processRunRequested) == 0x3c);
static_assert(offsetof(SceAppMgrAppStatus, networkDisconnectionWarningDialogEnabled) == 0x40);
static_assert(offsetof(SceAppMgrAppStatus, launchSourceAppId) == 0x44);
static_assert(offsetof(SceAppMgrAppStatus, infobar) == 0x48);
static_assert(offsetof(SceAppMgrAppStatus, launchArgumentMarker) == 0x4c);
static_assert(offsetof(SceAppMgrAppStatus, bgmProxyAppName) == 0x50);
static_assert(offsetof(SceAppMgrAppStatus, launchOrigin) == 0x70);
static_assert(offsetof(SceAppMgrAppStatus, budgetType) == 0x71);
static_assert(offsetof(SceAppMgrAppStatus, processAttr) == 0x74);
static_assert(offsetof(SceAppMgrAppStatus, systemOrientation) == 0x78);
static_assert(offsetof(SceAppMgrAppStatus, recommendedOrientationActivated) == 0x7c);

static void set_compat_app_status(SceAppMgrAppStatus &status, const std::string &title_id) {
    // Preserve the existing shell boot workaround until per-application state is emulated.
    // Firmware produces boolean values at 0x38 and 0x3c; these placeholders are not real state.
    status = {};
    status.launchSourceProcessId = 2;
    status.launchMode = 2;
    status.protectionMode = 1;
    strncpy(status.appName, title_id.c_str(), sizeof(status.appName));
    status.paramDeliveryHandled = 2;
    status.activated = 2;
    status.processRunRequested = 2;
    status.networkDisconnectionWarningDialogEnabled = 2;
    status.launchSourceAppId = 2;
    status.infobar.visibility = 2;
    status.launchArgumentMarker = 2;
    strncpy(status.bgmProxyAppName, title_id.c_str(), sizeof(status.bgmProxyAppName));
    status.launchOrigin = 1;
    status.budgetType = 2;
    status.processAttr = 1;
    status.systemOrientation = 2;
    status.recommendedOrientationActivated = 2;
}

static void set_secondary_app_status(SceAppMgrAppStatus &status, const SecondaryAppStatus &app) {
    static_assert(sizeof(status) == sizeof(app.status));
    static_assert(std::is_trivially_copyable_v<SceAppMgrAppStatus> && std::is_trivially_copyable_v<AppMgrLaunchStatus>);
    static_assert(offsetof(SceAppMgrAppStatus, activated) == offsetof(AppMgrLaunchStatus, activated));
    static_assert(offsetof(SceAppMgrAppStatus, processRunRequested) == offsetof(AppMgrLaunchStatus, process_run_requested));
    static_assert(offsetof(SceAppMgrAppStatus, infobar) == offsetof(AppMgrLaunchStatus, infobar));
    static_assert(offsetof(SceAppMgrAppStatus, launchArgumentMarker) == offsetof(AppMgrLaunchStatus, launch_argument_marker));
    static_assert(offsetof(SceAppMgrAppStatus, bgmProxyAppName) == offsetof(AppMgrLaunchStatus, bgm_proxy_app_name));
    static_assert(offsetof(SceAppMgrAppStatus, launchOrigin) == offsetof(AppMgrLaunchStatus, launch_origin));
    static_assert(offsetof(SceAppMgrAppStatus, systemOrientation) == offsetof(AppMgrLaunchStatus, system_orientation));
    std::memcpy(&status, &app.status, sizeof(status));
}

EXPORT(SceInt32, _sceAppMgrGetAppState, SceAppMgrAppState *appState, SceUInt32 sizeofSceAppMgrAppState, SceUInt32 buildVersion) {
    TRACY_FUNC(_sceAppMgrGetAppState, appState, sizeofSceAppMgrAppState, buildVersion);
    return CALL_EXPORT(__sceAppMgrGetAppState, appState, sizeofSceAppMgrAppState, buildVersion);
}

EXPORT(int, sceAppMgrAcidDirSet) {
    TRACY_FUNC(sceAppMgrAcidDirSet);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrAcquireSoundOutExclusive3) {
    TRACY_FUNC(sceAppMgrAcquireSoundOutExclusive3);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrAddContAddMount) {
    TRACY_FUNC(sceAppMgrAddContAddMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrAddContMount) {
    TRACY_FUNC(sceAppMgrAddContMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrAppDataMount, int mountId, Ptr<char> mountPoint) {
    TRACY_FUNC(sceAppMgrAppDataMount, mountId, mountPoint.address());
    return CALL_EXPORT(_sceAppMgrAppDataMount, mountId, mountPoint);
}

EXPORT(int, sceAppMgrAppDataMountById, int mountId, Ptr<const char> id, Ptr<char> mountPoint) {
    TRACY_FUNC(sceAppMgrAppDataMountById, mountId, id.address(), mountPoint.address());
    return CALL_EXPORT(_sceAppMgrAppDataMountById, mountId, id, mountPoint);
}

EXPORT(int, sceAppMgrAppMount) {
    TRACY_FUNC(sceAppMgrAppMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrAppParamGetInt, int pid, int param, Ptr<SceInt32> value) {
    TRACY_FUNC(sceAppMgrAppParamGetInt, pid, param, value);
    return appmgr_get_app_param_int(emuenv, thread_id, pid, param, value);
}

EXPORT(SceInt32, sceAppMgrAppParamGetString, int pid, int param, Ptr<char> string, SceSize length) {
    TRACY_FUNC(sceAppMgrAppParamGetString, pid, param, string, length);
    return appmgr_get_app_param_string(emuenv, thread_id, pid, param, string, length);
}

EXPORT(int, sceAppMgrAppParamSetString) {
    TRACY_FUNC(sceAppMgrAppParamSetString);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrAppUmount) {
    TRACY_FUNC(sceAppMgrAppUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrBgdlGetQueueStatus) {
    TRACY_FUNC(sceAppMgrBgdlGetQueueStatus);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrCaptureFrameBufDMACByAppId) {
    TRACY_FUNC(sceAppMgrCaptureFrameBufDMACByAppId);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrCaptureFrameBufIFTUByAppId, SceInt32 app_id, Ptr<const void> frame_buf) {
    TRACY_FUNC(sceAppMgrCaptureFrameBufIFTUByAppId, app_id, frame_buf.address());
    return CALL_EXPORT(_sceAppMgrCaptureFrameBufIFTUByAppId, app_id, frame_buf);
}

EXPORT(int, sceAppMgrCheckRifGD, Ptr<const char> path, Ptr<void> info) {
    TRACY_FUNC(sceAppMgrCheckRifGD, path.address(), info.address());
    return CALL_EXPORT(_sceAppMgrCheckRifGD, path, info);
}

EXPORT(int, sceAppMgrContentInstallPeriodStart) {
    TRACY_FUNC(sceAppMgrContentInstallPeriodStart);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrContentInstallPeriodStop) {
    TRACY_FUNC(sceAppMgrContentInstallPeriodStop);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrConvertVs0UserDrivePath, char *source_path, char *dest_path, int dest_len) {
    TRACY_FUNC(sceAppMgrConvertVs0UserDrivePath, source_path, dest_path, dest_len);
    STUBBED("Using strncpy");
    strncpy(dest_path, source_path, dest_len);
    return 0;
}

EXPORT(int, sceAppMgrDeclareShellProcess2) {
    TRACY_FUNC(sceAppMgrDeclareShellProcess2);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrDestroyAppByName, Ptr<const char> appName) {
    TRACY_FUNC(sceAppMgrDestroyAppByName, appName.address());
    return CALL_EXPORT(_sceAppMgrDestroyAppByName, appName);
}

EXPORT(int, sceAppMgrDrmClose, const SceAppMgrDrmOpenParam *param) {
    TRACY_FUNC(sceAppMgrDrmClose, param);
    return CALL_EXPORT(_sceAppMgrDrmClose, param);
}

EXPORT(int, sceAppMgrDrmOpen, const SceAppMgrDrmOpenParam *param) {
    TRACY_FUNC(sceAppMgrDrmOpen, param);
    return CALL_EXPORT(_sceAppMgrDrmOpen, param);
}

EXPORT(int, sceAppMgrForceUmount) {
    TRACY_FUNC(sceAppMgrForceUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGameDataMount, Ptr<const char> source, Ptr<const char> patch, Ptr<const void> license, Ptr<char> mountPoint) {
    TRACY_FUNC(sceAppMgrGameDataMount, source.address(), patch.address(), license.address(), mountPoint.address());
    return CALL_EXPORT(_sceAppMgrGameDataMount, source, patch, license, mountPoint);
}

EXPORT(int, sceAppMgrGetAppInfo) {
    TRACY_FUNC(sceAppMgrGetAppInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetAppMgrState) {
    TRACY_FUNC(sceAppMgrGetAppMgrState);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceAppMgrGetAppParam, Ptr<char> param) {
    TRACY_FUNC(sceAppMgrGetAppParam, param);
    return CALL_EXPORT(_sceAppMgrGetAppParam, param);
}

EXPORT(int, sceAppMgrGetAppParam2) {
    TRACY_FUNC(sceAppMgrGetAppParam2);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetBootParam) {
    TRACY_FUNC(sceAppMgrGetBootParam);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetBudgetInfo) {
    TRACY_FUNC(sceAppMgrGetBudgetInfo);
    STUBBED("not system mode");
    return -1;
}

EXPORT(int, sceAppMgrGetCoredumpStateForShell) {
    TRACY_FUNC(sceAppMgrGetCoredumpStateForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetCurrentBgmState) {
    TRACY_FUNC(sceAppMgrGetCurrentBgmState);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetCurrentBgmState2, AppMgrBgmState *state) {
    TRACY_FUNC(sceAppMgrGetCurrentBgmState2, state);
    return CALL_EXPORT(_sceAppMgrGetCurrentBgmState2, state);
}

EXPORT(int, sceAppMgrGetDevInfo, const char *dev, uint64_t *max_size, uint64_t *free_size) {
    TRACY_FUNC(sceAppMgrGetDevInfo, dev, max_size, free_size);
    if (dev == nullptr) {
        return RET_ERROR(SCE_ERROR_ERRNO_EINVAL);
    }

    auto device = device::get_device(dev);
    if (device == VitaIoDevice::_INVALID) {
        LOG_ERROR("Cannot find device for path: {}", dev);
        return RET_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    fs::path dev_path = boost::describe::enum_to_string(device, "");
    fs::path path = emuenv.vita_fs_path / dev_path;
    fs::space_info space = fs::space(path);

    // TODO: Use free or available?
    // free = free space available on the whole partition
    // available = free space available to a non-privileged process
    // Using available in case the drive is nearly full and the game tries to write since available will always be smaller
    *free_size = space.available;
    *max_size = space.capacity;

    return 0;
}

EXPORT(int, sceAppMgrGetFgAppInfo) {
    TRACY_FUNC(sceAppMgrGetFgAppInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetIdByName) {
    TRACY_FUNC(sceAppMgrGetIdByName);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetMediaTypeFromDrive) {
    TRACY_FUNC(sceAppMgrGetMediaTypeFromDrive);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetMediaTypeFromDriveByPid) {
    TRACY_FUNC(sceAppMgrGetMediaTypeFromDriveByPid);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetMountProcessNum) {
    TRACY_FUNC(sceAppMgrGetMountProcessNum);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetNameById) {
    TRACY_FUNC(sceAppMgrGetNameById);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetPfsDrive) {
    TRACY_FUNC(sceAppMgrGetPfsDrive);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetPidListForShell) {
    TRACY_FUNC(sceAppMgrGetPidListForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetRawPath, char *path, char *resolved_path, int resolved_path_size) {
    TRACY_FUNC(sceAppMgrGetRawPath, resolved_path, path, resolved_path_size);
    STUBBED("Using strncpy");
    strncpy(resolved_path, path, resolved_path_size);
    return 0;
}

EXPORT(int, sceAppMgrGetRawPathOfApp0ByAppIdForShell) {
    TRACY_FUNC(sceAppMgrGetRawPathOfApp0ByAppIdForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetRawPathOfApp0ByPidForShell) {
    TRACY_FUNC(sceAppMgrGetRawPathOfApp0ByPidForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetRecommendedScreenOrientation) {
    TRACY_FUNC(sceAppMgrGetRecommendedScreenOrientation);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetRunningAppIdListForShell, Ptr<SceInt32> app_ids, SceUInt32 count) {
    TRACY_FUNC(sceAppMgrGetRunningAppIdListForShell, app_ids, count);
    return appmgr_get_running_app_id_list(emuenv, thread_id, app_ids, count);
}

EXPORT(int, sceAppMgrGetSaveDataInfo, Ptr<const char> path, Ptr<char> title_id, SceInt32 force_mount, SceInt32 option) {
    TRACY_FUNC(sceAppMgrGetSaveDataInfo, path, title_id, force_mount, option);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return context.error();
    if (context->io->title_id.empty())
        return RET_ERROR(SCE_APPMGR_ERROR_APP_NOT_FOUND);
    return 0;
}

EXPORT(int, sceAppMgrGetSaveDataInfoForSpecialExport) {
    TRACY_FUNC(sceAppMgrGetSaveDataInfoForSpecialExport);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetStatusByAppId, int id, SceAppMgrAppStatus *appStatus) {
    TRACY_FUNC(sceAppMgrGetStatusByAppId, id, appStatus);
    if (!appStatus)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    if (id == -4) {
        if (const auto app = find_active_secondary_app(emuenv)) {
            set_secondary_app_status(*appStatus, *app);
            return 0;
        }
        *appStatus = {};
        return RET_ERROR(SCE_APPMGR_ERROR_APP_NOT_FOUND);
    }
    if (id == -2) {
        const auto pid = emuenv.kernel.get_thread_process_id(thread_id);
        if (!pid)
            return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
        if (const auto app = find_secondary_app_by_pid(emuenv, *pid)) {
            set_secondary_app_status(*appStatus, *app);
            return 0;
        }
        set_compat_app_status(*appStatus, emuenv.io.title_id);
        appStatus->processId = *pid;
        return 0;
    }
    if (const auto app = find_secondary_app_by_id(emuenv, id, true)) {
        set_secondary_app_status(*appStatus, *app);
        return 0;
    }
    if (const auto app = find_unloaded_app_by_id(emuenv, id)) {
        set_secondary_app_status(*appStatus, *app);
        return 0;
    }
    set_compat_app_status(*appStatus, emuenv.io.title_id);
    return STUBBED("Returning compatibility status without looking up app ID");
}

EXPORT(int, sceAppMgrGetStatusById, int id, SceAppMgrAppStatus *appStatus) {
    TRACY_FUNC(sceAppMgrGetStatusById, id, appStatus);
    if (!appStatus)
        return RET_ERROR(SCE_APPMGR_ERROR_NULL_POINTER);
    if (const auto app = find_secondary_app_by_id(emuenv, id)) {
        set_secondary_app_status(*appStatus, *app);
        LOG_DEBUG("sceAppMgrGetStatusById: app {} PID {}, budget type {}, launch marker {}, activated {}, run requested {}",
            log_hex(appStatus->appId), appStatus->processId, appStatus->budgetType, appStatus->launchArgumentMarker,
            appStatus->activated, appStatus->processRunRequested);
        return 0;
    }
    if (const auto app = find_secondary_app_by_pid(emuenv, id)) {
        set_secondary_app_status(*appStatus, *app);
        LOG_DEBUG("sceAppMgrGetStatusById: PID {} resolved to app {}, budget type {}, launch marker {}, activated {}, run requested {}",
            appStatus->processId, log_hex(appStatus->appId), appStatus->budgetType, appStatus->launchArgumentMarker,
            appStatus->activated, appStatus->processRunRequested);
        return 0;
    }
    if (const auto app = find_unloaded_app_by_id(emuenv, id)) {
        set_secondary_app_status(*appStatus, *app);
        return 0;
    }
    *appStatus = {};
    return RET_ERROR(SCE_APPMGR_ERROR_APP_NOT_FOUND);
}

EXPORT(int, sceAppMgrGetStatusByName, const char *appName, SceAppMgrAppStatus *appStatus) {
    TRACY_FUNC(sceAppMgrGetStatusByName, appName, appStatus);
    if (appName && strnlen(appName, sizeof(SceAppMgrAppStatus::appName)) == sizeof(SceAppMgrAppStatus::appName))
        return RET_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
    if (!appStatus)
        return RET_ERROR(SCE_APPMGR_ERROR_NULL_POINTER);
    LOG_DEBUG_IF(appName, "name: {}", appName);
    if (appName) {
        if (const auto app = find_secondary_app_by_name(emuenv, appName)) {
            set_secondary_app_status(*appStatus, *app);
            LOG_DEBUG("{}: status for '{}' resolved to app ID {}, PID {}", export_name, appName, appStatus->appId, appStatus->processId);
            return 0;
        }
    }
    if (appName && emuenv.io.title_id != appName) {
        *appStatus = {};
        return RET_ERROR(SCE_APPMGR_ERROR_APP_NOT_FOUND);
    }
    if (!appName) {
        const auto pid = emuenv.kernel.get_thread_process_id(thread_id);
        if (!pid)
            return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
        if (const auto app = find_secondary_app_by_pid(emuenv, *pid)) {
            set_secondary_app_status(*appStatus, *app);
            return 0;
        }
    }
    set_compat_app_status(*appStatus, emuenv.io.title_id);
    LOG_DEBUG("{}: compatibility status for '{}' has app ID {}, PID {}", export_name, appName ? appName : "<current>", appStatus->appId, appStatus->processId);
    return STUBBED("Returning compatibility status for the current application");
}

EXPORT(int, sceAppMgrGetSystemDataFilePlayReady) {
    TRACY_FUNC(sceAppMgrGetSystemDataFilePlayReady);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetUserDirPath, int partition_id, char *userDirPath, SceSize path_maxlen) {
    TRACY_FUNC(sceAppMgrGetUserDirPath, partition_id, userDirPath, path_maxlen);
    STUBBED("Using strncpy");
    const auto partition = partition_id == 1 ? "ur0:" : "ux0:";
    fmt::format_to_n(userDirPath, path_maxlen, "{}user/{}{}", partition, emuenv.io.user_id, '\0');
    return 0;
}

EXPORT(int, sceAppMgrGetUserDirPathById) {
    TRACY_FUNC(sceAppMgrGetUserDirPathById);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetVs0UserDataDrive) {
    TRACY_FUNC(sceAppMgrGetVs0UserDataDrive);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrGetVs0UserModuleDrive) {
    TRACY_FUNC(sceAppMgrGetVs0UserModuleDrive);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrInitSafeMemoryById) {
    TRACY_FUNC(sceAppMgrInitSafeMemoryById);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrInstallDirMount) {
    TRACY_FUNC(sceAppMgrInstallDirMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrIsCameraActive) {
    TRACY_FUNC(sceAppMgrIsCameraActive);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrLaunchAppByName) {
    TRACY_FUNC(sceAppMgrLaunchAppByName);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrLaunchAppByName2, Ptr<const char> name, Ptr<const char> param, Ptr<const void> option) {
    TRACY_FUNC(sceAppMgrLaunchAppByName2, name, param, option.address());
    return CALL_EXPORT(_sceAppMgrLaunchAppByName2, name, param, option);
}

EXPORT(int, sceAppMgrLaunchAppByName2ForShell) {
    TRACY_FUNC(sceAppMgrLaunchAppByName2ForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrLaunchAppByName2ndStage, SceInt32 app_id, Ptr<const char> path,
    SceUInt32 budget_size, SceUInt32 process_attr, Ptr<const void> option) {
    TRACY_FUNC(sceAppMgrLaunchAppByName2ndStage, app_id, path, budget_size, process_attr, option);
    return appmgr_launch_app_by_name_2nd_stage(emuenv, thread_id, app_id, path, budget_size, process_attr, option);
}

EXPORT(int, sceAppMgrLaunchAppByNameForShell, int flags, Ptr<const char> name, Ptr<const char> param) {
    TRACY_FUNC(sceAppMgrLaunchAppByNameForShell, flags, name, param);
    return appmgr_launch_app_by_name_for_shell(emuenv, thread_id, flags, name, param);
}

EXPORT(int, sceAppMgrLaunchAppByPath4, Ptr<const char> path, Ptr<const char> title_id,
    SceUInt32 budget_size, Ptr<const char> param, SceUInt32 process_attr, Ptr<const void> option) {
    TRACY_FUNC(sceAppMgrLaunchAppByPath4, path, title_id, budget_size, param, process_attr, option);
    return appmgr_launch_app_by_path(emuenv, thread_id, path, title_id, budget_size, param, process_attr, option);
}

EXPORT(int, sceAppMgrLaunchAppByUri, SceUInt32 flags, Ptr<const char> uri) {
    TRACY_FUNC(sceAppMgrLaunchAppByUri, flags, uri.address());
    return CALL_EXPORT(_sceAppMgrLaunchAppByUri, flags, uri);
}

EXPORT(int, sceAppMgrLaunchAppByUri2) {
    TRACY_FUNC(sceAppMgrLaunchAppByUri2);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrLaunchVideoStreamingApp) {
    TRACY_FUNC(sceAppMgrLaunchVideoStreamingApp);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceAppMgrLoadExec, const char *appPath, Ptr<char> const argv[], const SceAppMgrLoadExecOptParam *optParam) {
    TRACY_FUNC(sceAppMgrLoadExec, appPath, argv, optParam);
    return CALL_EXPORT(_sceAppMgrLoadExec, appPath, argv, optParam);
}

EXPORT(int, sceAppMgrLoadSaveDataSystemFile) {
    TRACY_FUNC(sceAppMgrLoadSaveDataSystemFile);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrLoopBackFormat) {
    TRACY_FUNC(sceAppMgrLoopBackFormat);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrLoopBackMount) {
    TRACY_FUNC(sceAppMgrLoopBackMount);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceAppMgrMmsMount, SceInt32 id, char *mount_point) {
    TRACY_FUNC(sceAppMgrMmsMount, id, mount_point);
    return CALL_EXPORT(_sceAppMgrMmsMount, id, mount_point);
}

EXPORT(int, sceAppMgrOverwriteLaunchParamForShell, SceInt32 app_id, Ptr<const char> param) {
    TRACY_FUNC(sceAppMgrOverwriteLaunchParamForShell, app_id, param);
    return appmgr_overwrite_launch_param_for_shell(emuenv, thread_id, app_id, param);
}

EXPORT(int, sceAppMgrPeekLaunchParamForShell, SceInt32 app_id, Ptr<char> output) {
    TRACY_FUNC(sceAppMgrPeekLaunchParamForShell, app_id, output);
    return appmgr_peek_launch_param_for_shell(emuenv, thread_id, app_id, output);
}

EXPORT(int, sceAppMgrPhotoMount) {
    TRACY_FUNC(sceAppMgrPhotoMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrPhotoUmount) {
    TRACY_FUNC(sceAppMgrPhotoUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrPspSaveDataGetParams) {
    TRACY_FUNC(sceAppMgrPspSaveDataGetParams);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrPspSaveDataRead) {
    TRACY_FUNC(sceAppMgrPspSaveDataRead);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrPspSaveDataRootMount) {
    TRACY_FUNC(sceAppMgrPspSaveDataRootMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrReceiveEvent, SceAppMgrEvent *event) {
    TRACY_FUNC(sceAppMgrReceiveEvent, event);
    return CALL_EXPORT(_sceAppMgrReceiveEvent, event);
}

EXPORT(int, sceAppMgrReceiveEventNum, SceUInt32 *eventNum) {
    TRACY_FUNC(sceAppMgrReceiveEventNum, eventNum);
    return CALL_EXPORT(_sceAppMgrReceiveEventNum, eventNum);
}

EXPORT(int, sceAppMgrReceiveNotificationRequestForShell, Ptr<SceAppMgrNotificationRequest> request) {
    TRACY_FUNC(sceAppMgrReceiveNotificationRequestForShell, request.address());
    return CALL_EXPORT(_sceAppMgrReceiveNotificationRequestForShell, request);
}

EXPORT(int, sceAppMgrReceiveShellEvent, Ptr<SceAppMgrShellEvent> event) {
    TRACY_FUNC(sceAppMgrReceiveShellEvent, event);
    return appmgr_receive_shell_event(emuenv, thread_id, event);
}

EXPORT(int, sceAppMgrReceiveSystemEvent, SceAppMgrSystemEvent *systemEvent) {
    TRACY_FUNC(sceAppMgrReceiveSystemEvent, systemEvent);
    systemEvent->systemEvent = SCE_APPMGR_SYSTEMEVENT_ON_RESUME;
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSaveDataAddMount) {
    TRACY_FUNC(sceAppMgrSaveDataAddMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSaveDataDataRemove, SceAppMgrSaveDataDataDelete *data) {
    TRACY_FUNC(sceAppMgrSaveDataDataRemove, data);
    if (const SceInt32 error = emuenv.kernel.check_primary_address_space_access(thread_id, export_name))
        return error;
    if (!data || data->size != sizeof(*data) || (data->file_count && !data->files)
        || (data->slot_id != UINT32_MAX && data->slot_id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    SceAppUtilSaveDataFileSlot slot{};
    slot.id = data->slot_id;
    slot.slotParam = data->slot_param;
    const auto result = execute_savedata_data_remove(emuenv, data->slot_id == UINT32_MAX ? nullptr : &slot,
        data->files.get(emuenv.mem), data->file_count, &data->mount_point, export_name);
    if (result == static_cast<int>(SCE_APPUTIL_ERROR_PARAMETER))
        return RET_ERROR(SCE_APPMGR_ERROR_SAVEDATA_INVALID_PARAMETER);
    return result != 0 ? RET_ERROR(result) : 0;
}

EXPORT(int, sceAppMgrSaveDataDataRemove2) {
    TRACY_FUNC(sceAppMgrSaveDataDataRemove2);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSaveDataDataSave, SceAppMgrSaveDataData *data) {
    TRACY_FUNC(sceAppMgrSaveDataDataSave, data);
    if (!data || data->size != sizeof(*data) || (data->file_count && !data->files)
        || (data->slot_id != UINT32_MAX && data->slot_id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return context.error();
    SceAppUtilSaveDataFileSlot slot{};
    slot.id = data->slot_id;
    slot.slotParam = data->slot_param;
    const auto result = execute_savedata_data_save(emuenv, context->thread->cpu->mem, *context->io,
        data->slot_id == UINT32_MAX ? nullptr : &slot, data->files, data->file_count, &data->mount_point, &data->required_size_kib, export_name);
    if (result == static_cast<int>(SCE_APPUTIL_ERROR_PARAMETER))
        return RET_ERROR(SCE_APPMGR_ERROR_SAVEDATA_INVALID_PARAMETER);
    return result != 0 ? RET_ERROR(result) : 0;
}

EXPORT(int, sceAppMgrSaveDataDataSave2) {
    TRACY_FUNC(sceAppMgrSaveDataDataSave2);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSaveDataGetQuota, SceAppMgrSaveDataQuota *data) {
    TRACY_FUNC(sceAppMgrSaveDataGetQuota, data);
    if (!data || data->size != sizeof(*data))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return context.error();
    const auto &io = *context->io;
    if (io.device_paths.savedata0.empty()) {
        LOG_ERROR("{}: savedata0 is not configured", export_name);
        return RET_ERROR(SCE_ERROR_ERRNO_EBADFD);
    }
    std::string savedata_max_size;
    if (!sfo::get_data_by_key(savedata_max_size, emuenv.sfo_handle, "SAVEDATA_MAX_SIZE"))
        savedata_max_size = "0";
    data->quota_size_kib = static_cast<SceSize>(std::strtoul(savedata_max_size.c_str(), nullptr, 10));
    data->used_size_kib = vfs::get_directory_used_size(VitaIoDevice::ux0, io.device_paths.savedata0, emuenv.vita_fs_path) / KiB(1);
    return 0;
}

EXPORT(int, sceAppMgrSaveDataMount) {
    TRACY_FUNC(sceAppMgrSaveDataMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSaveDataSlotCreate, SceAppMgrSaveDataSlot *data) {
    TRACY_FUNC(sceAppMgrSaveDataSlotCreate, data);
    if (!data || data->size != sizeof(*data) || data->slot_id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return context.error();
    const auto result = write_savedata_slot(emuenv, data->slot_id, &data->param.app_util, SaveDataSlotWriteMode::CreateOnly, nullptr, export_name, context->io);
    if (result == static_cast<int>(SCE_APPUTIL_ERROR_SAVEDATA_SLOT_EXISTS))
        return RET_ERROR(SCE_APPMGR_ERROR_SAVEDATA_SLOT_EXISTS);
    return result != 0 ? RET_ERROR(result) : 0;
}

EXPORT(int, sceAppMgrSaveDataSlotDelete, SceAppMgrSaveDataSlotDelete *data) {
    TRACY_FUNC(sceAppMgrSaveDataSlotDelete, data);
    if (!data || data->size != sizeof(*data) || data->slot_id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return context.error();
    const auto result = write_savedata_slot(emuenv, data->slot_id, nullptr, SaveDataSlotWriteMode::DeleteExisting, nullptr, export_name, context->io);
    if (result == static_cast<int>(SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND))
        return RET_ERROR(SCE_APPMGR_ERROR_SAVEDATA_SLOT_NOT_FOUND);
    return result != 0 ? RET_ERROR(result) : 0;
}

EXPORT(int, sceAppMgrSaveDataSlotFileClose, Ptr<const char> mount_point) {
    TRACY_FUNC(sceAppMgrSaveDataSlotFileClose, mount_point);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return context.error();
    auto &mem = context->thread->cpu->mem;
    if (!mount_point)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    if (!is_valid_addr_len(mem, mount_point.address(), sizeof("savedata0:")))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    const auto *path = mount_point.get(mem);
    if (std::memcmp(path, "savedata0:", sizeof("savedata0:")) != 0
        && std::memcmp(path, "savedata1:", sizeof("savedata1:")) != 0)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    return 0;
}

EXPORT(int, sceAppMgrSaveDataSlotFileGetParam, SceUInt32 slot_id, Ptr<void> out) {
    TRACY_FUNC(sceAppMgrSaveDataSlotFileGetParam, slot_id, out);
    if (slot_id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX || !out)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return context.error();
    auto &mem = context->thread->cpu->mem;
    if (!is_valid_addr_len(mem, out.address(), 0x400))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    auto *output = static_cast<uint8_t *>(out.get(mem));
    std::memset(output, 0, 0x400);
    SceAppUtilSaveDataSlotParam param{};
    const auto result = read_savedata_slot(emuenv, slot_id, param, export_name, context->io);
    if (result == SaveDataSlotReadResult::NotFound)
        return RET_ERROR(SCE_APPMGR_ERROR_SAVEDATA_SLOT_NOT_FOUND);
    if (result == SaveDataSlotReadResult::Error)
        return RET_ERROR(SCE_ERROR_ERRNO_EIO);
    std::memcpy(output, &param, sizeof(param));
    return 0;
}

EXPORT(int, sceAppMgrSaveDataSlotFileOpen, Ptr<const char> mount_point, Ptr<void> out) {
    TRACY_FUNC(sceAppMgrSaveDataSlotFileOpen, mount_point, out);
    constexpr uint32_t file_info_size = 0x228;
    constexpr Address slot_flags_offset = 0x28;
    constexpr uint32_t slot_flags_size = 0x200;
    static_assert(slot_flags_offset + slot_flags_size == file_info_size);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return context.error();
    auto &mem = context->thread->cpu->mem;
    if (!mount_point)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    if (!is_valid_addr_len(mem, mount_point.address(), sizeof("savedata0:")))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    const auto *path = mount_point.get(mem);
    if (std::memcmp(path, "savedata0:", sizeof("savedata0:")) != 0
        && std::memcmp(path, "savedata1:", sizeof("savedata1:")) != 0)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    if (out && !is_valid_addr_len(mem, out.address(), file_info_size))
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
    SaveDataSlotReadResult result;
    if (out) {
        SaveDataSlotFlags flags{};
        result = read_savedata_slot_flags(emuenv, flags, export_name, context->io);
        if (result == SaveDataSlotReadResult::Found) {
            std::array<uint8_t, slot_flags_size> output_flags{};
            std::copy(flags.begin(), flags.end(), output_flags.begin());
            LOG_DEBUG("{}: save-data slot index reports {} present slot(s)", export_name,
                std::count_if(flags.begin(), flags.end(), [](uint8_t flag) { return flag != 0; }));
            if (!write_guest_memory(mem, out.address() + slot_flags_offset, output_flags.data(), static_cast<uint32_t>(output_flags.size())))
                return RET_ERROR(SCE_APPMGR_ERROR_INVALID_POINTER);
        }
    } else {
        result = check_savedata_slot_file(emuenv, export_name, context->io);
    }
    if (result == SaveDataSlotReadResult::NotFound)
        return RET_ERROR(SCE_ERROR_ERRNO_ENOENT);
    if (result == SaveDataSlotReadResult::Error)
        return RET_ERROR(SCE_ERROR_ERRNO_EIO);
    return 0;
}

EXPORT(int, sceAppMgrSaveDataSlotGetParam, SceAppMgrSaveDataSlot *data) {
    TRACY_FUNC(sceAppMgrSaveDataSlotGetParam, data);
    if (!data || data->size != sizeof(*data) || data->slot_id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return context.error();
    const auto result = read_savedata_slot(emuenv, data->slot_id, data->param.app_util, export_name, context->io);
    if (result == SaveDataSlotReadResult::NotFound)
        return RET_ERROR(SCE_APPMGR_ERROR_SAVEDATA_SLOT_NOT_FOUND);
    if (result == SaveDataSlotReadResult::Error)
        return RET_ERROR(SCE_ERROR_ERRNO_EIO);
    std::fill(std::begin(data->param.reserved), std::end(data->param.reserved), 0);
    return 0;
}

EXPORT(int, sceAppMgrSaveDataSlotGetStatus) {
    TRACY_FUNC(sceAppMgrSaveDataSlotGetStatus);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSaveDataSlotInit) {
    TRACY_FUNC(sceAppMgrSaveDataSlotInit);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSaveDataSlotSetParam, SceAppMgrSaveDataSlot *data) {
    TRACY_FUNC(sceAppMgrSaveDataSlotSetParam, data);
    if (!data || data->size != sizeof(*data) || data->slot_id >= SCE_APPUTIL_SAVEDATA_SLOT_MAX)
        return RET_ERROR(SCE_APPMGR_ERROR_INVALID_PARAMETER);
    const auto context = get_process_io_context(emuenv, thread_id, export_name);
    if (!context)
        return context.error();
    const auto result = write_savedata_slot(emuenv, data->slot_id, &data->param.app_util, SaveDataSlotWriteMode::Set, nullptr, export_name, context->io);
    if (result == static_cast<int>(SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND))
        return RET_ERROR(SCE_APPMGR_ERROR_SAVEDATA_SLOT_NOT_FOUND);
    return result != 0 ? RET_ERROR(result) : 0;
}

EXPORT(int, sceAppMgrSaveDataSlotSetStatus) {
    TRACY_FUNC(sceAppMgrSaveDataSlotSetStatus);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSaveDataUmount) {
    TRACY_FUNC(sceAppMgrSaveDataUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSendNotificationRequest, Ptr<const SceAppMgrNotificationRequest> request, SceInt32 wait) {
    TRACY_FUNC(sceAppMgrSendNotificationRequest, request.address(), wait);
    return CALL_EXPORT(_sceAppMgrSendNotificationRequest, request, wait);
}

EXPORT(int, sceAppMgrSendParam) {
    TRACY_FUNC(sceAppMgrSendParam);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSendSystemEvent) {
    TRACY_FUNC(sceAppMgrSendSystemEvent);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSendSystemEvent2) {
    TRACY_FUNC(sceAppMgrSendSystemEvent2);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetBackRenderPortOwner, SceInt32 app_id) {
    TRACY_FUNC(sceAppMgrSetBackRenderPortOwner, app_id);
    return appmgr_set_back_render_port_owner(emuenv, thread_id, app_id);
}

EXPORT(int, sceAppMgrSetBgmProxyApp, Ptr<const char> app_name) {
    TRACY_FUNC(sceAppMgrSetBgmProxyApp, app_name);
    return CALL_EXPORT(_sceAppMgrSetBgmProxyApp, app_name);
}

EXPORT(int, sceAppMgrSetNetworkDisconnectionWarningDialogState) {
    TRACY_FUNC(sceAppMgrSetNetworkDisconnectionWarningDialogState);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetPowerSaveMode) {
    TRACY_FUNC(sceAppMgrSetPowerSaveMode);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetRecommendedScreenOrientationForShell) {
    TRACY_FUNC(sceAppMgrSetRecommendedScreenOrientationForShell);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetShellScreenOrientation) {
    TRACY_FUNC(sceAppMgrSetShellScreenOrientation);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetSystemDataFile) {
    TRACY_FUNC(sceAppMgrSetSystemDataFile);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSetSystemDataFilePlayReady) {
    TRACY_FUNC(sceAppMgrSetSystemDataFilePlayReady);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSystemParamDateTimeGetConf) {
    TRACY_FUNC(sceAppMgrSystemParamDateTimeGetConf);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSystemParamGetInt) {
    TRACY_FUNC(sceAppMgrSystemParamGetInt);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrSystemParamGetString) {
    TRACY_FUNC(sceAppMgrSystemParamGetString);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrThemeDataMount, Ptr<const char> contentId, Ptr<const char> themePath, Ptr<const char> rifPath, Ptr<char> mountPoint) {
    TRACY_FUNC(sceAppMgrThemeDataMount, contentId.address(), themePath.address(), rifPath.address(), mountPoint.address());
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    MemState &mem = thread->cpu->mem;
    constexpr size_t arguments_size = sizeof(AppmgrDataMountArguments);
    const Ptr<AppmgrDataMountArguments> userArguments(thread->cpu->stack_alloc(arguments_size));
    auto *arguments = userArguments.get(mem);
    if (!arguments) {
        thread->cpu->stack_free(arguments_size);
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    }
    *arguments = {};
    arguments->mountPoint = mountPoint;
    const int result = CALL_EXPORT(_sceAppMgrThemeDataMount, contentId, themePath, rifPath,
        userArguments.cast<const AppmgrDataMountArguments>());
    thread->cpu->stack_free(arguments_size);
    return result;
}

EXPORT(int, sceAppMgrTrophyMount, int mountId, SceUID processId, Ptr<char> mountPoint) {
    TRACY_FUNC(sceAppMgrTrophyMount, mountId, processId, mountPoint.address());
    return CALL_EXPORT(_sceAppMgrTrophyMount, mountId, processId, mountPoint);
}

EXPORT(int, sceAppMgrTrophyMountById, int mountId, Ptr<const char> titleId, Ptr<char> mountPoint) {
    TRACY_FUNC(sceAppMgrTrophyMountById, mountId, titleId.address(), mountPoint.address());
    return CALL_EXPORT(_sceAppMgrTrophyMountById, mountId, titleId, mountPoint);
}

EXPORT(int, sceAppMgrUmount, Ptr<const char> mountPoint) {
    TRACY_FUNC(sceAppMgrUmount, mountPoint.address());
    return CALL_EXPORT(_sceAppMgrUmount, mountPoint);
}

EXPORT(int, sceAppMgrUmountByPid) {
    TRACY_FUNC(sceAppMgrUmountByPid);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrUpdateSaveDataParam) {
    TRACY_FUNC(sceAppMgrUpdateSaveDataParam);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppMgrWorkDirMount, int mountId, char *mountPoint) {
    TRACY_FUNC(sceAppMgrWorkDirMount, mountId, mountPoint);
    if (!mountPoint)
        return RET_ERROR(SCE_ERROR_ERRNO_EINVAL);
    STUBBED("Path redirection without mount lifecycle");
    switch (mountId) {
    case 0xC8:
        strcpy(mountPoint, "sqlite0:/");
        break;
    case 0xC9:
        strcpy(mountPoint, "attach0:/");
        break;
    case 0xCA:
        strcpy(mountPoint, "ux0:pspemu/");
        break;
    case 0xCC:
        strcpy(mountPoint, "checkout0:/");
        break;
    case 0xCE:
        strcpy(mountPoint, "webbrowser0:/");
        break;
    default:
        LOG_WARN("Unknown mount id: {}", log_hex(mountId));
        return RET_ERROR(SCE_ERROR_ERRNO_EINVAL);
    }

    return 0;
}

EXPORT(int, sceAppMgrWorkDirMountById, int mountId, Ptr<const char> id, Ptr<char> mountPoint) {
    TRACY_FUNC(sceAppMgrWorkDirMountById, mountId, id.address(), mountPoint.address());
    return CALL_EXPORT(_sceAppMgrWorkDirMountById, mountId, id, mountPoint);
}
