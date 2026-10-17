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

#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <module/module.h>
#include <regmgr/functions.h>

#include "../SceSblACMgr/SceSblACMgrForDriver.h"

static int get_sound_setting(EmuEnvState &emuenv, const char *name, int default_value) {
    if (!emuenv.regmgr.system_dreg_valid)
        return default_value;
    return regmgr::get_int_value(emuenv.regmgr, "/CONFIG/SOUND", name);
}

EXPORT(int, sceAVConfigChangeReg) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigClearAutoSuspend2) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigDisplayOn) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigGetAcStatus) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigGetBtVol) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigGetConnectedAudioDevice) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigGetDisplayMaxBrightness) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigGetMasterVol) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigGetShutterVol) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigGetSystemVol, Ptr<int> volume) {
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const auto caller_pid = emuenv.kernel.get_thread_process_id(thread_id);
    if (!caller_pid)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    // if (CALL_EXPORT(SceSblACMgrForDriver_IsSystemProgram, 0) != 1)
    //     return RET_ERROR(SCE_ERROR_ERRNO_ENOSYS);
    MemState &mem = caller->cpu->mem;
    if (!volume || !is_valid_addr_len(mem, volume.address(), sizeof(int))) {
        LOG_ERROR("sceAVConfigGetSystemVol: invalid pointer");
        return static_cast<int>(0x803A0102);
    }
    *volume.get(mem) = get_sound_setting(emuenv, "main_volume", 21);
    return 0;
}

EXPORT(int, sceAVConfigGetVolCtrlEnable, Ptr<int> mode, Ptr<int> muted, Ptr<int> avls) {
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    const auto caller_pid = emuenv.kernel.get_thread_process_id(thread_id);
    if (!caller_pid)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    // if (CALL_EXPORT(SceSblACMgrForDriver_IsSystemProgram, 0) != 1)
    //    return RET_ERROR(SCE_ERROR_ERRNO_ENOSYS);
    MemState &mem = caller->cpu->mem;
    if (!mode || !muted || !avls
        || !is_valid_addr_len(mem, mode.address(), sizeof(int))
        || !is_valid_addr_len(mem, muted.address(), sizeof(int))
        || !is_valid_addr_len(mem, avls.address(), sizeof(int))) {
        LOG_ERROR("sceAVConfigGetVolCtrlEnable: invalid pointer");
        return static_cast<int>(0x803A0102);
    }
    *mode.get(mem) = 1;
    *muted.get(mem) = get_sound_setting(emuenv, "speaker_mute", 0);
    *avls.get(mem) = get_sound_setting(emuenv, "avls", 0);
    return 0;
}

EXPORT(int, sceAVConfigHdmiCecCmdOneTouchPlay) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigHdmiCecDisable) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigHdmiCecEnable) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigHdmiClearCecInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigHdmiGetCecInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigHdmiGetMonitorInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigHdmiGetOutScalingRatio) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigHdmiSetOutScalingRatio) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigHdmiSetResolution) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigHdmiSetRgbRangeMode) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigMuteOn) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigRegisterCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigSendVolKey) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigSetAutoDisplayDimming) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigSetAutoSuspend) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigSetAutoSuspend2) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigSetDisplayBrightness) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigSetDisplayColorSpaceMode) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigSetMasterVol) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigSetSystemVol) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigUnRegisterCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigWriteMasterVol) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceAVConfigWriteRegSystemVol) {
    return UNIMPLEMENTED();
}
