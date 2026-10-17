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

#include <emuenv/state.h>
#include <kernel/state.h>
#include <rtc/rtc.h>
#include <util/tracy.h>

#include <util/safe_time.h>

#include <chrono>
#include <ctime>

TRACY_MODULE_NAME(SceRtc);

static int64_t rtc_local_time_offset_seconds(EmuEnvState &emuenv) {
    {
        std::lock_guard lock(emuenv.rtc_config_mutex);
        if (emuenv.rtc_config_initialized)
            return (static_cast<int64_t>(emuenv.rtc_timezone_minutes) + static_cast<int64_t>(emuenv.rtc_daylight_saving_minutes)) * 60;
    }

    const std::time_t now = std::time(nullptr);
    tm local_tm = {};
    tm gmt_tm = {};
    SAFE_LOCALTIME(&now, &local_tm);
    SAFE_GMTIME(&now, &gmt_tm);
    return std::mktime(&local_tm) - std::mktime(&gmt_tm);
}

static void set_emulated_network_tick(SceRtcTick *tick, uint64_t base_tick) {
    // Syscon's separate auxiliary network tick stores are not emulated.
    tick->tick = rtc_get_ticks(base_tick);
}

EXPORT(int, _sceRtcConvertLocalTimeToUtc, const SceRtcTick *pLocalTime, SceRtcTick *pUtc) {
    TRACY_FUNC(_sceRtcConvertLocalTimeToUtc, pLocalTime, pUtc);
    if (pUtc == nullptr || pLocalTime == nullptr) {
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    }
    pUtc->tick = pLocalTime->tick - rtc_local_time_offset_seconds(emuenv) * VITA_CLOCKS_PER_SEC;

    return 0;
}

EXPORT(int, _sceRtcConvertUtcToLocalTime, const SceRtcTick *pUtc, SceRtcTick *pLocalTime) {
    TRACY_FUNC(_sceRtcConvertUtcToLocalTime, pUtc, pLocalTime);
    if (pUtc == nullptr || pLocalTime == nullptr) {
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    }

    pLocalTime->tick = pUtc->tick + rtc_local_time_offset_seconds(emuenv) * VITA_CLOCKS_PER_SEC;
    return 0;
}

EXPORT(int, _sceRtcFormatRFC2822) {
    TRACY_FUNC(_sceRtcFormatRFC2822);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceRtcFormatRFC2822LocalTime) {
    TRACY_FUNC(_sceRtcFormatRFC2822LocalTime);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceRtcFormatRFC3339) {
    TRACY_FUNC(_sceRtcFormatRFC3339);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceRtcFormatRFC3339LocalTime) {
    TRACY_FUNC(_sceRtcFormatRFC3339LocalTime);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceRtcGetCurrentAdNetworkTick, SceRtcTick *tick) {
    TRACY_FUNC(_sceRtcGetCurrentAdNetworkTick, tick);
    if (tick == nullptr)
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    set_emulated_network_tick(tick, emuenv.kernel.base_tick.tick);
    return 0;
}

EXPORT(int, _sceRtcGetCurrentClock, SceDateTime *datePtr, int iTimeZone) {
    TRACY_FUNC(_sceRtcGetCurrentClock, datePtr, iTimeZone);
    if (datePtr == nullptr) {
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    }

    uint64_t tick = rtc_get_ticks(emuenv.kernel.base_tick.tick) + iTimeZone * 60 * 60 * VITA_CLOCKS_PER_SEC;
    __RtcTicksToPspTime(datePtr, tick);

    return 0;
}

EXPORT(int, _sceRtcGetCurrentClockLocalTime, SceDateTime *datePtr) {
    TRACY_FUNC(_sceRtcGetCurrentClockLocalTime, datePtr);
    if (datePtr == nullptr) {
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    }

    uint64_t tick = rtc_get_ticks(emuenv.kernel.base_tick.tick) + rtc_local_time_offset_seconds(emuenv) * VITA_CLOCKS_PER_SEC;
    __RtcTicksToPspTime(datePtr, tick);
    return 0;
}

EXPORT(int, _sceRtcGetCurrentDebugNetworkTick, SceRtcTick *tick) {
    TRACY_FUNC(_sceRtcGetCurrentDebugNetworkTick, tick);
    if (tick == nullptr)
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    set_emulated_network_tick(tick, emuenv.kernel.base_tick.tick);
    return 0;
}

EXPORT(int, _sceRtcGetCurrentGpsTick) {
    TRACY_FUNC(_sceRtcGetCurrentGpsTick);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceRtcGetCurrentNetworkTick, SceRtcTick *tick) {
    TRACY_FUNC(_sceRtcGetCurrentNetworkTick, tick);
    if (tick == nullptr)
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    set_emulated_network_tick(tick, emuenv.kernel.base_tick.tick);
    return 0;
}

EXPORT(int, _sceRtcGetCurrentRetainedNetworkTick, SceRtcTick *tick) {
    TRACY_FUNC(_sceRtcGetCurrentRetainedNetworkTick, tick);
    if (tick == nullptr)
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    set_emulated_network_tick(tick, emuenv.kernel.base_tick.tick);
    return 0;
}

EXPORT(int, _sceRtcGetCurrentTick, SceRtcTick *tick) {
    TRACY_FUNC(_sceRtcGetCurrentTick, tick);
    if (tick == nullptr) {
        return RET_ERROR(SCE_RTC_ERROR_INVALID_POINTER);
    }

    tick->tick = rtc_get_ticks(emuenv.kernel.base_tick.tick);

    return 0;
}

EXPORT(int, _sceRtcGetLastAdjustedTick) {
    TRACY_FUNC(_sceRtcGetLastAdjustedTick);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceRtcGetLastReincarnatedTick) {
    TRACY_FUNC(_sceRtcGetLastReincarnatedTick);
    return UNIMPLEMENTED();
}

EXPORT(SceULong64, sceRtcGetAccumulativeTime) {
    TRACY_FUNC(sceRtcGetAccumulativeTime);
    STUBBED("sceRtcGetAccumulativeTime");
    return rtc_get_ticks(emuenv.kernel.base_tick.tick);
}
