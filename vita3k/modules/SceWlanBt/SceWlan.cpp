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

#include <net/state.h>
#include <net/types.h>

EXPORT(unsigned int, sceWlanGetConfiguration) {
    LOG_INFO("sceWlanGetConfiguration called");
    const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name);
    if (!process_network_context) {
        const auto result = static_cast<unsigned int>(process_network_context.error());
        LOG_INFO("sceWlanGetConfiguration returned {}", result);
        return result;
    }

    const auto &net = *process_network_context->net;
    const auto result = net.wlan_configuration;
    const uint32_t next_configuration_bit = result & 1u;
    const uint32_t remaining_configuration_and_status_bits = result & ~1u;
    if (net.wlan_configuration_origin == WlanConfigurationOrigin::SetConfiguration) {
        const char *mapping = net.wlan_configuration_request == 0 ? "0 -> 0"
            : net.wlan_configuration_request == 1                 ? "1 -> 9"
                                                                  : "identity";
        LOG_INFO("sceWlanGetConfiguration returned value={} raw={} nextConfigurationBit={} remainingConfigurationAndStatusBits={} source=sceWlanSetConfiguration input={} mapping={}",
            result, log_hex(static_cast<int>(result)), next_configuration_bit,
            log_hex(static_cast<int>(remaining_configuration_and_status_bits)),
            net.wlan_configuration_request, mapping);
    } else if (net.wlan_configuration_origin == WlanConfigurationOrigin::Reset) {
        LOG_INFO("sceWlanGetConfiguration returned value={} raw={} nextConfigurationBit={} remainingConfigurationAndStatusBits={} source=NetState::deinit reset",
            result, log_hex(static_cast<int>(result)), next_configuration_bit,
            log_hex(static_cast<int>(remaining_configuration_and_status_bits)));
    } else {
        LOG_INFO("sceWlanGetConfiguration returned value={} raw={} nextConfigurationBit={} remainingConfigurationAndStatusBits={} source=NetState default member initializer; sceWlanSetConfiguration not called",
            result, log_hex(static_cast<int>(result)), next_configuration_bit,
            log_hex(static_cast<int>(remaining_configuration_and_status_bits)));
    }
    return result;
}

EXPORT(int, sceWlanGetWakeOnTargetProcess) {
    LOG_INFO("sceWlanGetWakeOnTargetProcess called, Unimplemented");
    return UNIMPLEMENTED();
}

EXPORT(int, sceWlanRegisterCallback) {
    LOG_INFO("sceWlanRegisterCallback called, Unimplemented");
    return UNIMPLEMENTED();
}

EXPORT(int, sceWlanSetConfiguration, int configuration) {
    LOG_INFO("sceWlanSetConfiguration called configuration={}", configuration);
    if (configuration < 0 || configuration > 3) {
        const int result = RET_ERROR(SCE_NET_ERROR_EINVAL);
        LOG_INFO("sceWlanSetConfiguration returned {}", log_hex(result));
        return result;
    }

    const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name);
    if (!process_network_context) {
        const int result = RET_ERROR(process_network_context.error());
        LOG_INFO("sceWlanSetConfiguration returned {}", log_hex(result));
        return result;
    }

    auto &net = *process_network_context->net;
    const uint32_t previous_configuration = net.wlan_configuration;
    net.wlan_configuration = configuration <= 1
        ? (configuration ? 9u : 0u)
        : static_cast<uint32_t>(configuration);
    net.wlan_configuration_origin = WlanConfigurationOrigin::SetConfiguration;
    net.wlan_configuration_request = configuration;
    const int result = 0;
    const char *mapping = configuration == 0 ? "0 -> 0"
        : configuration == 1                 ? "1 -> 9"
                                             : "identity";
    LOG_INFO("sceWlanSetConfiguration returned {} requested={} previous={} stored={} mapping={}",
        result, configuration, previous_configuration, net.wlan_configuration, mapping);
    return result;
}

EXPORT(int, sceWlanUnregisterCallback) {
    LOG_INFO("sceWlanUnregisterCallback called, Unimplemented");
    return UNIMPLEMENTED();
}
