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

#include "../SceNet/SceNet.h"

DECL_EXPORT(int, sceNetSyscallIcmConnect, int sid, int flags);

#include <cpu/state.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <kernel/types.h>
#include <mem/functions.h>
#include <util/tracy.h>

#include <cstdint>
#include <cstring>
#include <iterator>

TRACY_MODULE_NAME(SceNetInternal);

DECL_EXPORT(Ptr<int>, sceNetErrnoLoc);

EXPORT(int, sceNetInternalInetPton, int af, const char *src, void *dst) {
    TRACY_FUNC(sceNetInternalInetPton, af, src, dst);
    LOG_INFO("SceNetInternal_916EA798 af={} src={} dst={}", af, log_hex(reinterpret_cast<uintptr_t>(src)), log_hex(reinterpret_cast<uintptr_t>(dst)));
    return CALL_EXPORT(sceNetInetPton, af, src, dst);
}

EXPORT(int, sceNetInternalIcmConnect, int sid, int flags) {
    TRACY_FUNC(sceNetInternalIcmConnect, sid, flags);
    LOG_INFO("SceNetInternal_93F2FF08 sid={} flags={}", sid, flags);
    return CALL_EXPORT(sceNetSyscallIcmConnect, sid, flags);
}

EXPORT(Ptr<const char>, SceNetInternal_F3917021, unsigned int if_index) {
    TRACY_FUNC(SceNetInternal_F3917021, if_index);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        LOG_ERROR("SceNetInternal_F3917021: unknown thread {}", thread_id);
        return {};
    }
    static constexpr const char *interface_names[] = {
        "lo0", "eth0", "eth1", "eth2", "wlan0", "wlan1", "wlan2", "bt0", "phone0", "pppoe0"
    };

    const bool valid_index = if_index < std::size(interface_names);
    const char *interface_name = valid_index ? interface_names[if_index] : "";
    LOG_INFO("SceNetInternal_F3917021 if_index={} name='{}'", if_index, interface_name);

    MemState &mem = thread->cpu->mem;
    const Ptr<char> result(alloc(mem, 0x10, "if_name"));
    if (!result) {
        LOG_ERROR("Failed to allocate the SceNetInternal interface name.");
        return {};
    }

    std::strcpy(result.get(mem), interface_name);
    if (!valid_index) {
        const auto error_location = CALL_EXPORT(sceNetErrnoLoc);
        if (error_location)
            *error_location.get(mem) = SCE_NET_EINVAL;
    }
    return result;
}

EXPORT(int, SceNetInternal_689B9D7D, uint32_t *random) {
    TRACY_FUNC(SceNetInternal_689B9D7D, random);
    LOG_INFO("SceNetInternal_689B9D7D random={}", log_hex(reinterpret_cast<uintptr_t>(random)));
    if (!random)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    return 0;
}

EXPORT(int, SceNetInternal_EAC33599, int if_index, Ptr<void> function, Ptr<void> argument, int flags) {
    TRACY_FUNC(SceNetInternal_EAC33599, if_index, function, argument, flags);
    LOG_INFO("SceNetInternal_EAC33599 if_index={} function={} argument={} flags={}", if_index, log_hex(function.address()), log_hex(argument.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_65AFF9BE, Ptr<void> param) {
    TRACY_FUNC(SceNetInternal_65AFF9BE, param);
    LOG_INFO("SceNetInternal_65AFF9BE param={}", log_hex(param.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_EE1985D9, int policy) {
    TRACY_FUNC(SceNetInternal_EE1985D9, policy);
    LOG_INFO("SceNetInternal_EE1985D9 policy={}", policy);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_A6B9F5D2, Ptr<void> mss, int flags) {
    TRACY_FUNC(SceNetInternal_A6B9F5D2, mss, flags);
    LOG_INFO("SceNetInternal_A6B9F5D2 mss={} flags={}", log_hex(mss.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_7FD92AE7, int if_index) {
    TRACY_FUNC(SceNetInternal_7FD92AE7, if_index);
    LOG_INFO("SceNetInternal_7FD92AE7 if_index={}", if_index);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_235DE96C, unsigned int if_index, Ptr<void> info, int count) {
    TRACY_FUNC(SceNetInternal_235DE96C, if_index, info, count);
    LOG_INFO("SceNetInternal_235DE96C if_index={} info={} count={}", if_index, log_hex(info.address()), count);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_EE94BA15, int if_index, Ptr<void> statistics, int count) {
    TRACY_FUNC(SceNetInternal_EE94BA15, if_index, statistics, count);
    LOG_INFO("SceNetInternal_EE94BA15 if_index={} statistics={} count={}", if_index, log_hex(statistics.address()), count);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_8157DE3E, Ptr<int> indices, int count, int flags) {
    TRACY_FUNC(SceNetInternal_8157DE3E, indices, count, flags);
    LOG_INFO("SceNetInternal_8157DE3E indices={} count={} flags={}", log_hex(indices.address()), count, flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_AA414742, int if_index, int kernel_index, int command, Ptr<void> data, int length) {
    TRACY_FUNC(SceNetInternal_AA414742, if_index, kernel_index, command, data, length);
    LOG_INFO("SceNetInternal_AA414742 if_index={} kernel_index={} command={} data={} length={}", if_index, kernel_index, command, log_hex(data.address()), length);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_7F63379E, Ptr<void> info, int flags) {
    TRACY_FUNC(SceNetInternal_7F63379E, info, flags);
    LOG_INFO("SceNetInternal_7F63379E info={} flags={}", log_hex(info.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_16222ED6, Ptr<void> info, int flags) {
    TRACY_FUNC(SceNetInternal_16222ED6, info, flags);
    LOG_INFO("SceNetInternal_16222ED6 info={} flags={}", log_hex(info.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_BDA92B76, int family, Ptr<void> entries, int count, int flags) {
    TRACY_FUNC(SceNetInternal_BDA92B76, family, entries, count, flags);
    LOG_INFO("SceNetInternal_BDA92B76 family={} entries={} count={} flags={}", family, log_hex(entries.address()), count, flags);
    return UNIMPLEMENTED();
}

EXPORT(Ptr<void>, SceNetInternal_5B0E370C, int if_index, Ptr<int> count) {
    TRACY_FUNC(SceNetInternal_5B0E370C, if_index, count);
    LOG_INFO("SceNetInternal_5B0E370C if_index={} count={}", if_index, count);
    UNIMPLEMENTED();
    return {};
}

EXPORT(int, SceNetInternal_CF49E9C4, Ptr<void> entries) {
    TRACY_FUNC(SceNetInternal_CF49E9C4, entries);
    LOG_INFO("SceNetInternal_CF49E9C4 entries={}", log_hex(entries.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_AAA34850, Ptr<void> param, int flags) {
    TRACY_FUNC(SceNetInternal_AAA34850, param, flags);
    LOG_INFO("SceNetInternal_AAA34850 param={} flags={}", log_hex(param.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_A9F2277C, Ptr<void> list, int flags) {
    TRACY_FUNC(SceNetInternal_A9F2277C, list, flags);
    LOG_INFO("SceNetInternal_A9F2277C list={} flags={}", log_hex(list.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_01B090F1, int if_index, Ptr<void> param, int flags) {
    TRACY_FUNC(SceNetInternal_01B090F1, if_index, param, flags);
    LOG_INFO("SceNetInternal_01B090F1 if_index={} param={} flags={}", if_index, log_hex(param.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_E03F6A77, int if_index, Ptr<const void> param, int flags) {
    TRACY_FUNC(SceNetInternal_E03F6A77, if_index, param, flags);
    LOG_INFO("SceNetInternal_E03F6A77 if_index={} param={} flags={}", if_index, log_hex(param.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_26EFF46D, int if_index, int argument, int flags) {
    TRACY_FUNC(SceNetInternal_26EFF46D, if_index, argument, flags);
    LOG_INFO("SceNetInternal_26EFF46D if_index={} argument={} flags={}", if_index, argument, flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_4F5E0E8A, int if_index, int argument, int flags) {
    TRACY_FUNC(SceNetInternal_4F5E0E8A, if_index, argument, flags);
    LOG_INFO("SceNetInternal_4F5E0E8A if_index={} argument={} flags={}", if_index, argument, flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_EE94BF52, int process_id, Ptr<int> policy, int flags) {
    TRACY_FUNC(SceNetInternal_EE94BF52, process_id, policy, flags);
    LOG_INFO("SceNetInternal_EE94BF52 process_id={} policy={} flags={}", process_id, log_hex(policy.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_0DFC23D6, int process_id, int policy, int flags) {
    TRACY_FUNC(SceNetInternal_0DFC23D6, process_id, policy, flags);
    LOG_INFO("SceNetInternal_0DFC23D6 process_id={} policy={} flags={}", process_id, policy, flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_EF421A65, int target_thread_id, Ptr<int> mode, int flags) {
    TRACY_FUNC(SceNetInternal_EF421A65, target_thread_id, mode, flags);
    LOG_INFO("SceNetInternal_EF421A65 target_thread_id={} mode={} flags={}", target_thread_id, log_hex(mode.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_9DBD2F60, int target_thread_id, int mode, int flags) {
    TRACY_FUNC(SceNetInternal_9DBD2F60, target_thread_id, mode, flags);
    LOG_INFO("SceNetInternal_9DBD2F60 target_thread_id={} mode={} flags={}", target_thread_id, mode, flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_DEF2357F, Ptr<void> counts, int count, int flags) {
    TRACY_FUNC(SceNetInternal_DEF2357F, counts, count, flags);
    LOG_INFO("SceNetInternal_DEF2357F counts={} count={} flags={}", log_hex(counts.address()), count, flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_46E28336, Ptr<void> list, int count, int flags) {
    TRACY_FUNC(SceNetInternal_46E28336, list, count, flags);
    LOG_INFO("SceNetInternal_46E28336 list={} count={} flags={}", log_hex(list.address()), count, flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_F9E3A8B7, int if_index, Ptr<void> param, int flags) {
    TRACY_FUNC(SceNetInternal_F9E3A8B7, if_index, param, flags);
    LOG_INFO("SceNetInternal_F9E3A8B7 if_index={} param={} flags={}", if_index, log_hex(param.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_853EEABE, int if_index, Ptr<const void> param, int flags) {
    TRACY_FUNC(SceNetInternal_853EEABE, if_index, param, flags);
    LOG_INFO("SceNetInternal_853EEABE if_index={} param={} flags={}", if_index, log_hex(param.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_DE8DD55E, Ptr<int> error, int flags) {
    TRACY_FUNC(SceNetInternal_DE8DD55E, error, flags);
    LOG_INFO("SceNetInternal_DE8DD55E error={} flags={}", log_hex(error.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_7493FB0C, int error, int flags) {
    TRACY_FUNC(SceNetInternal_7493FB0C, error, flags);
    LOG_INFO("SceNetInternal_7493FB0C error={} flags={}", error, flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_EDAA3453, int if_index, int flags) {
    TRACY_FUNC(SceNetInternal_EDAA3453, if_index, flags);
    LOG_INFO("SceNetInternal_EDAA3453 if_index={} flags={}", if_index, flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_04E6136D, int timeout, int argument, int flags) {
    TRACY_FUNC(SceNetInternal_04E6136D, timeout, argument, flags);
    LOG_INFO("SceNetInternal_04E6136D timeout={} argument={} flags={}", timeout, argument, flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_B01ABD0A, int if_index, Ptr<int> event, int flags) {
    TRACY_FUNC(SceNetInternal_B01ABD0A, if_index, event, flags);
    LOG_INFO("SceNetInternal_B01ABD0A if_index={} event={} flags={}", if_index, log_hex(event.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(Ptr<void>, SceNetInternal_A2C2BC5F, int size, int flags) {
    TRACY_FUNC(SceNetInternal_A2C2BC5F, size, flags);
    LOG_INFO("SceNetInternal_A2C2BC5F size={} flags={}", size, flags);
    UNIMPLEMENTED();
    return {};
}

EXPORT(void, SceNetInternal_E62A2FAA, Ptr<void> ptr) {
    TRACY_FUNC(SceNetInternal_E62A2FAA, ptr);
    LOG_INFO("SceNetInternal_E62A2FAA ptr={}", log_hex(ptr.address()));
    UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_2FEC8B74, Ptr<uint64_t> time) {
    TRACY_FUNC(SceNetInternal_2FEC8B74, time);
    LOG_INFO("SceNetInternal_2FEC8B74 time={}", log_hex(time.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_A64CFBF5, Ptr<void> destination) {
    TRACY_FUNC(SceNetInternal_A64CFBF5, destination);
    LOG_INFO("SceNetInternal_A64CFBF5 destination={}", log_hex(destination.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_694F8996, Ptr<const char> format) {
    TRACY_FUNC(SceNetInternal_694F8996, format);
    LOG_INFO("SceNetInternal_694F8996 format={}", log_hex(format.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_225C4801, Ptr<SceUID> thread, Ptr<void> param, Ptr<const char> name) {
    TRACY_FUNC(SceNetInternal_225C4801, thread, param, name);
    LOG_INFO("SceNetInternal_225C4801 thread={} param={} name={}", log_hex(thread.address()), log_hex(param.address()), log_hex(name.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_3B5F94B7, SceUID thread) {
    TRACY_FUNC(SceNetInternal_3B5F94B7, thread);
    LOG_INFO("SceNetInternal_3B5F94B7 thread={}", thread);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_4EF96CD8) {
    TRACY_FUNC(SceNetInternal_4EF96CD8);
    LOG_INFO("SceNetInternal_4EF96CD8");
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_104E4820, int usec) {
    TRACY_FUNC(SceNetInternal_104E4820, usec);
    LOG_INFO("SceNetInternal_104E4820 usec={}", usec);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_4054D2D8, Ptr<void> sync, int bits, Ptr<const char> name) {
    TRACY_FUNC(SceNetInternal_4054D2D8, sync, bits, name);
    LOG_INFO("SceNetInternal_4054D2D8 sync={} bits={} name={}", log_hex(sync.address()), bits, log_hex(name.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_97527BDB, Ptr<void> sync) {
    TRACY_FUNC(SceNetInternal_97527BDB, sync);
    LOG_INFO("SceNetInternal_97527BDB sync={}", log_hex(sync.address()));
    return UNIMPLEMENTED();
}

EXPORT(Ptr<void>, SceNetInternal_F6291476, int which) {
    TRACY_FUNC(SceNetInternal_F6291476, which);
    LOG_INFO("SceNetInternal_F6291476 which={}", which);
    UNIMPLEMENTED();
    return {};
}

EXPORT(int, SceNetInternal_C2FF615E, Ptr<void> sync, Ptr<uint32_t> timeout) {
    TRACY_FUNC(SceNetInternal_C2FF615E, sync, timeout);
    LOG_INFO("SceNetInternal_C2FF615E sync={} timeout={}", log_hex(sync.address()), log_hex(timeout.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_9D12D8A4, Ptr<void> sync, int bits) {
    TRACY_FUNC(SceNetInternal_9D12D8A4, sync, bits);
    LOG_INFO("SceNetInternal_9D12D8A4 sync={} bits={}", log_hex(sync.address()), bits);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_6273AF23, Ptr<void> param, int flags) {
    TRACY_FUNC(SceNetInternal_6273AF23, param, flags);
    LOG_INFO("sceNetEmulationGet param={} flags={}", log_hex(param.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_DF815D48, Ptr<void> param, int flags) {
    TRACY_FUNC(SceNetInternal_DF815D48, param, flags);
    LOG_INFO("sceNetEmulationSet param={} flags={}", log_hex(param.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_333F9CB3, int sid, unsigned int command, Ptr<void> data) {
    TRACY_FUNC(SceNetInternal_333F9CB3, sid, command, data);
    LOG_INFO("SceNetInternal_333F9CB3 sid={} command={} data={}", sid, command, log_hex(data.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_E2F6CAEA, Ptr<const char> name, int flags) {
    TRACY_FUNC(SceNetInternal_E2F6CAEA, name, flags);
    LOG_INFO("SceNetInternal_E2F6CAEA name={} flags={}", log_hex(name.address()), flags);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_0F83B7A9, int id, int op, int sid, Ptr<void> info) {
    TRACY_FUNC(SceNetInternal_0F83B7A9, id, op, sid, info);
    LOG_INFO("SceNetInternal_0F83B7A9 id={} op={} sid={} info={}", id, op, sid, log_hex(info.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_873DE778, int id) {
    TRACY_FUNC(SceNetInternal_873DE778, id);
    LOG_INFO("SceNetInternal_873DE778 id={}", id);
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetInternal_FE8C89AE, Ptr<const int> name, unsigned int namelen, Ptr<void> oldp,
    Ptr<unsigned int> oldlenp, Ptr<const void> newp, unsigned int newlen) {
    TRACY_FUNC(SceNetInternal_FE8C89AE, name, namelen, oldp, oldlenp, newp, newlen);
    LOG_INFO("SceNetInternal_FE8C89AE name={} namelen={} oldp={} oldlenp={} newp={} newlen={}",
        log_hex(name.address()), namelen, log_hex(oldp.address()), log_hex(oldlenp.address()), log_hex(newp.address()), newlen);
    return UNIMPLEMENTED();
}
