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

#include <net/state.h>

#include <algorithm>
#include <limits>

#ifdef _WIN32
#include <WinSock2.h>
#endif

void NetState::abort_all() {
    for (auto &[id, sock] : socks)
        sock->abort(SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION | SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION);
}

void NetState::refresh_connection_state() {
    const bool interface_ready = host_link_up && (interface_flags & 0x41) == 0x41 && netAddr != 0 && netAddr != 0xFFFFFFFF;
    if (auto_connect_policy > 0) {
        if (interface_ready) {
            auto_connect_enabled = true;
            connection_state = 3;
        } else {
            auto_connect_enabled = false;
            if (connection_state == 0 || !host_link_up)
                connection_state = 4;
        }
    } else {
        auto_connect_enabled = false;
        connection_state = 0;
    }
}

void NetState::record_interface_transmit(int bytes, bool packet) {
    if (bytes < 0)
        return;

    const std::lock_guard<std::mutex> lock(interface_stats_mutex);
    interface_tx_counters[0] += static_cast<uint32_t>(bytes);
    if (packet)
        ++interface_tx_counters[1];
    interface_last_tx = std::chrono::steady_clock::now();
    interface_has_tx_activity = true;
}

void NetState::record_interface_receive(int bytes, bool packet) {
    if (bytes < 0)
        return;

    const std::lock_guard<std::mutex> lock(interface_stats_mutex);
    interface_rx_counters[0] += static_cast<uint32_t>(bytes);
    if (packet)
        ++interface_rx_counters[1];
}

void NetState::set_interface_active(bool active) {
    const std::lock_guard<std::mutex> lock(interface_stats_mutex);
    if (interface_active == active)
        return;

    interface_active = active;
    if (active) {
        if (interface_generation != 0) {
            interface_tx_counters.fill(0);
            interface_rx_counters.fill(0);
            interface_software_tx_counters.fill(0);
            interface_software_rx_counters.fill(0);
        }
        ++interface_generation;
        interface_active_since = std::chrono::steady_clock::now();
        interface_last_tx = interface_active_since;
        interface_has_tx_activity = true;
    } else {
        interface_active_since = {};
    }
}

void NetState::reset_interface_idle_time() {
    const std::lock_guard<std::mutex> lock(interface_stats_mutex);
    interface_last_tx = std::chrono::steady_clock::now();
    interface_has_tx_activity = true;
}

NetInterfaceStats NetState::get_interface_stats(bool track_idle) const {
    const std::lock_guard<std::mutex> lock(interface_stats_mutex);
    NetInterfaceStats stats{};
    stats.tx_counters = interface_tx_counters;
    stats.rx_counters = interface_rx_counters;
    stats.software_tx_counters = interface_software_tx_counters;
    stats.software_rx_counters = interface_software_rx_counters;
    stats.promiscuous_enabled = interface_promiscuous_enabled;
    stats.promiscuous_references = interface_promiscuous_references;
    stats.generation = interface_generation;
    stats.emulation_seed_present = interface_emulation_seed != 0;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_seconds = [now](const std::chrono::steady_clock::time_point &start) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        return static_cast<int32_t>(std::min<int64_t>(seconds, std::numeric_limits<int32_t>::max()));
    };
    if (interface_active)
        stats.uptime_seconds = elapsed_seconds(interface_active_since);
    if (track_idle && interface_active && interface_has_tx_activity)
        stats.idle_seconds = elapsed_seconds(interface_last_tx);
    return stats;
}

void NetState::set_interface_emulation(const std::array<uint8_t, 512> &parameters, uint32_t seed) {
    const std::lock_guard<std::mutex> lock(interface_stats_mutex);
    interface_emulation_parameters = parameters;
    interface_emulation_seed = seed;
    interface_emulation_configured = true;
}

std::array<uint8_t, 512> NetState::get_interface_emulation() const {
    const std::lock_guard<std::mutex> lock(interface_stats_mutex);
    return interface_emulation_configured ? interface_emulation_parameters : std::array<uint8_t, 512>{};
}

int NetState::update_interface_promiscuous(int enabled, int references) {
    const std::lock_guard<std::mutex> lock(interface_stats_mutex);
    if (references != 0)
        return -22;
    if (enabled == 0) {
        if (!interface_promiscuous_enabled)
            return -77;
        if (--interface_promiscuous_references <= 0) {
            interface_promiscuous_enabled = 0;
            interface_promiscuous_references = 0;
        }
        return 0;
    }
    if (enabled != 1)
        return -22;
    ++interface_promiscuous_references;
    interface_promiscuous_enabled = 1;
    return 0;
}

void NetState::deinit() {
    auto_connect_cond_var.notify_all();
    for (auto &[id, sock] : socks) {
        sock->close();
    }
    socks.clear();
    epolls.clear();
    {
        const std::lock_guard<std::mutex> lock(route_mutex);
        routes.clear();
    }

    {
        const std::lock_guard<std::mutex> lock(interface_stats_mutex);
        interface_tx_counters.fill(0);
        interface_rx_counters.fill(0);
        interface_software_tx_counters.fill(0);
        interface_software_rx_counters.fill(0);
        interface_active_since = {};
        interface_last_tx = {};
        interface_active = false;
        interface_has_tx_activity = false;
        interface_promiscuous_enabled = 0;
        interface_promiscuous_references = 0;
        interface_generation = 0;
        interface_emulation_seed = 0;
        interface_emulation_parameters.fill(0);
        interface_emulation_configured = false;
    }

#ifdef _WIN32
    if (inited)
        WSACleanup();
#endif

    inited = false;
    next_id = 0;
    next_epoll_id = 0;
    state = -1;
    resolver_id = 0;
    current_addr_index = 0;
    interface_flags = 0x43;
    host_interface_flags.fill(0x43);
    loopback_interface_flags = 0x49;
    connection_state = 4;
    connect_timeout_seconds = -1;
    idle_timeout_seconds = 60;
    auto_connect_policy = 1;
    process_policy = 2;
    process_error = 0;
    auto_connect_enabled = false;
    host_link_up = false;
    loopback_requested = false;
    wlan_configuration = 9;
    wlan_configuration_origin = WlanConfigurationOrigin::Reset;
    wlan_configuration_request = -1;
    control_value = 0;
    pending_control_events.fill(0);
    driver_contexts.clear();
    thread_modes.clear();
    thread_errors.clear();
    interface_address_configured = false;
    interface_alias_configured = false;
    interface_alias_address = 0;
    interface_alias_netmask = 0;
    interface_alias_broadcast_address = 0;
    dns_config_initialized = false;
    dns_config_subscribed = false;
    dns_config_flags = 0;
    dns_addresses.fill(0);
    netMask = 0;
    broadcastAddr = 0xFFFFFFFF;
    netAddr = 0xFFFFFFFF;
    p2p_key.fill(0);
    p2p_key_set = false;
    p2p_peers.clear();
    auto_connect_cond_var.notify_all();
}
