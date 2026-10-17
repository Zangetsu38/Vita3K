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

#include "SceRtc.h"

#include <module/module.h>

#include <rtc/rtc.h>

#include <emuenv/state.h>
#include <kernel/callback.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>

#include <chrono>
#include <mutex>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceRtcForDriver);

namespace {
struct RtcDriverState {
    std::mutex mutex;
    SceUInt64 alarm_tick = 0;
    int alarm_arg = 0;
    SceUID alarm_callback = -1;
    bool alarm_notified = false;
    SceUInt64 secure_tick = 0;
    std::chrono::steady_clock::time_point secure_tick_set_at{};
    bool secure_tick_initialized = false;
    SceUInt64 secure_alarm_tick = 0;
    SceUID secure_alarm_callback = -1;
    bool secure_alarm_notified = false;
};

RtcDriverState rtc_driver_state;

SceUInt64 current_tick(EmuEnvState &env) {
    return rtc_get_ticks(env.kernel.base_tick.tick);
}

SceUInt64 current_secure_tick(EmuEnvState &env) {
    const auto now = std::chrono::steady_clock::now();
    const SceUInt64 initial_tick = current_tick(env);
    std::lock_guard lock(rtc_driver_state.mutex);
    if (!rtc_driver_state.secure_tick_initialized) {
        // The emulator has no separate secure clock, so initialize it from the RTC.
        rtc_driver_state.secure_tick = initial_tick;
        rtc_driver_state.secure_tick_set_at = now;
        rtc_driver_state.secure_tick_initialized = true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - rtc_driver_state.secure_tick_set_at).count();
    return rtc_driver_state.secure_tick + static_cast<SceUInt64>(elapsed);
}

SceInt32 validate_callback(EmuEnvState &env, SceUID callback_id) {
    const CallbackPtr callback = env.kernel.objects.find<Callback>(callback_id);
    if (!callback)
        return SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID;
    if (!env.kernel.get_thread(callback->thread_id))
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    return SCE_KERNEL_OK;
}

SceInt32 notify_callback(EmuEnvState &env, SceUID callback_id, SceInt32 arg) {
    const CallbackPtr callback = env.kernel.objects.find<Callback>(callback_id);
    if (!callback)
        return SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID;
    const ThreadStatePtr owner = env.kernel.get_thread(callback->thread_id);
    if (!owner)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    owner->notify_callback(*callback, arg);
    return SCE_KERNEL_OK;
}

int alarm_status(EmuEnvState &env, bool secure) {
    const SceUInt64 now = secure ? current_secure_tick(env) : current_tick(env);
    SceUID callback_id = -1;
    bool alarmed = false;
    {
        std::lock_guard lock(rtc_driver_state.mutex);
        SceUInt64 &alarm_tick = secure ? rtc_driver_state.secure_alarm_tick : rtc_driver_state.alarm_tick;
        bool &notified = secure ? rtc_driver_state.secure_alarm_notified : rtc_driver_state.alarm_notified;
        SceUID &registered_callback = secure ? rtc_driver_state.secure_alarm_callback : rtc_driver_state.alarm_callback;
        alarmed = alarm_tick != 0 && now >= alarm_tick;
        if (alarmed && !notified) {
            notified = true;
            callback_id = registered_callback;
        }
    }
    if (callback_id >= 0) {
        const SceInt32 result = notify_callback(env, callback_id, 1);
        if (result < 0) {
            LOG_WARN("RTC alarm callback {} could not be notified: {}", log_hex(callback_id), log_hex(result));
            std::lock_guard lock(rtc_driver_state.mutex);
            SceUID &registered_callback = secure ? rtc_driver_state.secure_alarm_callback : rtc_driver_state.alarm_callback;
            if (registered_callback == callback_id)
                registered_callback = -1;
        }
    }
    return alarmed ? 1 : 0;
}

SceInt32 register_callback(EmuEnvState &env, SceUID callback_id, bool secure) {
    const SceInt32 validation = validate_callback(env, callback_id);
    if (validation < 0)
        return validation;
    const int initial_status = alarm_status(env, secure);
    {
        std::lock_guard lock(rtc_driver_state.mutex);
        SceUID &registered_callback = secure ? rtc_driver_state.secure_alarm_callback : rtc_driver_state.alarm_callback;
        if (registered_callback >= 0) {
            if (env.kernel.objects.find<Callback>(registered_callback))
                return SCE_RTC_ERROR_ALREADY_REGISTERED;
            registered_callback = -1;
        }
        registered_callback = callback_id;
    }
    const SceInt32 result = notify_callback(env, callback_id, initial_status);
    if (result < 0) {
        std::lock_guard lock(rtc_driver_state.mutex);
        SceUID &registered_callback = secure ? rtc_driver_state.secure_alarm_callback : rtc_driver_state.alarm_callback;
        if (registered_callback == callback_id)
            registered_callback = -1;
    }
    return result;
}

SceInt32 unregister_callback(SceUID callback_id, bool secure) {
    std::lock_guard lock(rtc_driver_state.mutex);
    SceUID &registered_callback = secure ? rtc_driver_state.secure_alarm_callback : rtc_driver_state.alarm_callback;
    if (registered_callback != callback_id)
        return SCE_RTC_ERROR_NOT_FOUND;
    registered_callback = -1;
    return SCE_KERNEL_OK;
}
} // namespace

EXPORT(int, ksceRtcConvertDateTimeToUnixTime) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcConvertLocalTimeToUtc) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcConvertTickToDateTime) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcConvertUtcToLocal) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcFormatRFC2822) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcFormatRFC2822LocalTime) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcFormatRFC3339) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcFormatRFC3339LocalTime) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcGetAccumulativeTime) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcGetAlarmTick, SceRtcTick *tick, int *arg) {
    TRACY_FUNC(ksceRtcGetAlarmTick, tick, arg);
    if (!tick)
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    {
        std::lock_guard lock(rtc_driver_state.mutex);
        tick->tick = rtc_driver_state.alarm_tick;
        if (arg)
            *arg = rtc_driver_state.alarm_arg;
    }
    return SCE_KERNEL_OK;
}

EXPORT(int, ksceRtcGetCurrentAdNetworkTick, SceRtcTick *tick) {
    TRACY_FUNC(ksceRtcGetCurrentAdNetworkTick, tick);
    return CALL_EXPORT(_sceRtcGetCurrentAdNetworkTick, tick);
}

EXPORT(int, ksceRtcGetCurrentClock) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcGetCurrentClockLocalTime) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcGetCurrentDebugNetworkTick, SceRtcTick *tick) {
    TRACY_FUNC(ksceRtcGetCurrentDebugNetworkTick, tick);
    return CALL_EXPORT(_sceRtcGetCurrentDebugNetworkTick, tick);
}

EXPORT(int, ksceRtcGetCurrentGpsTick) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcGetCurrentNetworkTick, SceRtcTick *tick) {
    TRACY_FUNC(ksceRtcGetCurrentNetworkTick, tick);
    return CALL_EXPORT(_sceRtcGetCurrentNetworkTick, tick);
}

EXPORT(int, ksceRtcGetCurrentRetainedNetworkTick, SceRtcTick *tick) {
    TRACY_FUNC(ksceRtcGetCurrentRetainedNetworkTick, tick);
    return CALL_EXPORT(_sceRtcGetCurrentRetainedNetworkTick, tick);
}

EXPORT(int, ksceRtcGetCurrentSecureTick, SceRtcTick *tick) {
    TRACY_FUNC(ksceRtcGetCurrentSecureTick, tick);
    if (!tick)
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    tick->tick = current_secure_tick(emuenv);
    alarm_status(emuenv, true);
    return SCE_KERNEL_OK;
}

EXPORT(int, ksceRtcGetCurrentTick, SceRtcTick *tick) {
    TRACY_FUNC(ksceRtcGetCurrentTick, tick);
    const SceInt32 result = CALL_EXPORT(_sceRtcGetCurrentTick, tick);
    if (result == SCE_KERNEL_OK)
        alarm_status(emuenv, false);
    return result;
}

EXPORT(int, ksceRtcGetLastAdjustedTick) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcGetLastReincarnatedTick) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcGetSecureAlarmTick, SceRtcTick *tick) {
    TRACY_FUNC(ksceRtcGetSecureAlarmTick, tick);
    if (!tick)
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    {
        std::lock_guard lock(rtc_driver_state.mutex);
        tick->tick = rtc_driver_state.secure_alarm_tick;
    }
    return SCE_KERNEL_OK;
}

EXPORT(int, ksceRtcIsAlarmed) {
    TRACY_FUNC(ksceRtcIsAlarmed);
    return alarm_status(emuenv, false);
}

EXPORT(int, ksceRtcIsSecureAlarmed) {
    TRACY_FUNC(ksceRtcIsSecureAlarmed);
    return alarm_status(emuenv, true);
}

EXPORT(int, ksceRtcRegisterCallback, SceUID callback_id) {
    TRACY_FUNC(ksceRtcRegisterCallback, callback_id);
    return register_callback(emuenv, callback_id, false);
}

EXPORT(int, ksceRtcRegisterSecureAlarmCallback, SceUID callback_id) {
    TRACY_FUNC(ksceRtcRegisterSecureAlarmCallback, callback_id);
    return register_callback(emuenv, callback_id, true);
}

EXPORT(int, ksceRtcSetAlarmTick, const SceRtcTick *tick, int arg) {
    TRACY_FUNC(ksceRtcSetAlarmTick, tick, arg);
    const SceUInt64 tick_value = tick ? tick->tick : 0;
    {
        std::lock_guard lock(rtc_driver_state.mutex);
        rtc_driver_state.alarm_tick = tick_value;
        rtc_driver_state.alarm_arg = arg;
        rtc_driver_state.alarm_notified = false;
    }
    alarm_status(emuenv, false);
    return SCE_KERNEL_OK;
}

EXPORT(int, ksceRtcSetConf, SceUInt32 date_format, SceUInt32 time_format, SceInt32 timezone, SceUInt32 daylight_saving) {
    TRACY_FUNC(ksceRtcSetConf, date_format, time_format, timezone, daylight_saving);
    if (date_format > SCE_SYSTEM_PARAM_DATE_FORMAT_MMDDYYYY || time_format > SCE_SYSTEM_PARAM_TIME_FORMAT_24HOUR
        || timezone < -1439 || timezone > 1439 || daylight_saving > 1440) {
        return RET_ERROR(SCE_RTC_ERROR_INVALID_VALUE);
    }

    bool initializing_rtc_config;
    {
        std::lock_guard lock(emuenv.rtc_config_mutex);
        initializing_rtc_config = !emuenv.rtc_config_initialized;
        emuenv.rtc_timezone_minutes = timezone;
        emuenv.rtc_daylight_saving_minutes = daylight_saving;
        emuenv.rtc_config_initialized = true;
    }
    // The shell's initial setup calls this to initialize RTC state; keep the configured display formats.
    if (!initializing_rtc_config) {
        emuenv.cfg.sys_date_format = static_cast<int>(date_format);
        emuenv.cfg.sys_time_format = static_cast<int>(time_format);
    }
    return 0;
}

EXPORT(int, ksceRtcSetCurrentDebugNetworkTick) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcSetCurrentNetworkTick) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcSetCurrentSecureTick, const SceRtcTick *tick) {
    TRACY_FUNC(ksceRtcSetCurrentSecureTick, tick);
    if (!tick)
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    {
        std::lock_guard lock(rtc_driver_state.mutex);
        rtc_driver_state.secure_tick = tick->tick;
        rtc_driver_state.secure_tick_set_at = std::chrono::steady_clock::now();
        rtc_driver_state.secure_tick_initialized = true;
        rtc_driver_state.secure_alarm_notified = false;
    }
    alarm_status(emuenv, true);
    return SCE_KERNEL_OK;
}

EXPORT(int, ksceRtcSetCurrentTick) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceRtcSetSecureAlarmTick, const SceRtcTick *tick) {
    TRACY_FUNC(ksceRtcSetSecureAlarmTick, tick);
    const SceUInt64 tick_value = tick ? tick->tick : 0;
    {
        std::lock_guard lock(rtc_driver_state.mutex);
        rtc_driver_state.secure_alarm_tick = tick_value;
        rtc_driver_state.secure_alarm_notified = false;
    }
    alarm_status(emuenv, true);
    return SCE_KERNEL_OK;
}

EXPORT(int, ksceRtcUnregisterCallback, SceUID callback_id) {
    TRACY_FUNC(ksceRtcUnregisterCallback, callback_id);
    return unregister_callback(callback_id, false);
}

EXPORT(int, ksceRtcUnregisterSecureAlarmCallback, SceUID callback_id) {
    TRACY_FUNC(ksceRtcUnregisterSecureAlarmCallback, callback_id);
    return unregister_callback(callback_id, true);
}
