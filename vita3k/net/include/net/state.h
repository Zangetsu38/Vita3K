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

#pragma once

#include <net/epoll.h>
#include <net/socket.h>
#include <net/types.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using NetSockets = std::map<int, SocketPtr>;
using NetEpolls = std::map<int, EpollPtr>;

struct NetDriverCallback {
    int32_t callback_id = 0;
    bool clear_pending = false;
    uint64_t pending_events = 0;
};

struct NetRoute {
    uint32_t destination = 0;
    uint32_t gateway = 0;
    uint32_t netmask = 0;
    uint32_t flags = 0;
    uint32_t mtu = 0;
    uint16_t interface_index = 0;
    bool gateway_present = false;
    std::string interface_name;
};

struct NetP2PPeer {
    std::array<uint8_t, 16> key{};
    uint16_t local_port = SCE_NET_ADHOC_PORT;
    uint32_t references = 0;
    uint8_t group = 0;
    bool broadcast = false;
};

struct NetDriverContext {
    std::array<NetDriverCallback, 3> callbacks{};
    int32_t last_error = 0;
    int32_t connection_error = 0;
};

struct NetInterfaceStats {
    std::array<uint32_t, 8> tx_counters{};
    std::array<uint32_t, 8> rx_counters{};
    std::array<uint32_t, 8> software_tx_counters{};
    std::array<uint32_t, 8> software_rx_counters{};
    int32_t idle_seconds = 0;
    int32_t promiscuous_enabled = 0;
    int32_t promiscuous_references = 0;
    int32_t uptime_seconds = 0;
    uint32_t generation = 0;
    bool emulation_seed_present = false;
};

enum class WlanConfigurationOrigin {
    Default,
    Reset,
    SetConfiguration,
};

struct NetState {
    bool inited = false;
    int next_id = 0;
    NetSockets socks;
    int next_epoll_id = 0;
    NetEpolls epolls;
    int state = -1;
    int resolver_id = 0;
    int current_addr_index = 0;
    uint32_t interface_flags = 0x43;
    std::array<uint32_t, 2> host_interface_flags{ 0x43, 0x43 };
    uint32_t loopback_interface_flags = 0x49;
    int32_t connection_state = 4;
    int32_t connect_timeout_seconds = -1;
    int32_t idle_timeout_seconds = 60;
    int32_t auto_connect_policy = 1;
    int32_t process_policy = 2;
    int32_t process_error = 0;
    bool auto_connect_enabled = false;
    bool host_link_up = false;
    std::condition_variable auto_connect_cond_var;
    bool loopback_requested = false;
    uint32_t wlan_configuration = 9;
    WlanConfigurationOrigin wlan_configuration_origin = WlanConfigurationOrigin::Default;
    int32_t wlan_configuration_request = -1;
    uint32_t control_value = 0;
    std::array<uint8_t, 68> pending_control_events{};
    std::map<int32_t, NetDriverContext> driver_contexts;
    mutable std::mutex route_mutex;
    std::vector<NetRoute> routes;
    mutable std::mutex interface_stats_mutex;
    // POSIX sockets expose payload bytes, not the host NIC's link-layer counters.
    std::array<uint32_t, 8> interface_tx_counters{};
    std::array<uint32_t, 8> interface_rx_counters{};
    std::array<uint32_t, 8> interface_software_tx_counters{};
    std::array<uint32_t, 8> interface_software_rx_counters{};
    std::chrono::steady_clock::time_point interface_active_since{};
    std::chrono::steady_clock::time_point interface_last_tx{};
    bool interface_active = false;
    bool interface_has_tx_activity = false;
    int32_t interface_promiscuous_enabled = 0;
    int32_t interface_promiscuous_references = 0;
    uint32_t interface_generation = 0;
    uint32_t interface_emulation_seed = 0;
    std::array<uint8_t, 512> interface_emulation_parameters{};
    bool interface_emulation_configured = false;
    std::map<int32_t, int32_t> thread_modes;
    std::map<int32_t, int32_t> thread_errors;
    bool interface_address_configured = false;
    bool interface_alias_configured = false;
    uint32_t interface_alias_address = 0;
    uint32_t interface_alias_netmask = 0;
    uint32_t interface_alias_broadcast_address = 0;
    bool dns_config_initialized = false;
    bool dns_config_subscribed = false;
    uint32_t dns_config_flags = 0;
    std::array<uint32_t, 2> dns_addresses{};
    uint32_t netMask = 0;
    uint32_t broadcastAddr = 0xFFFFFFFF;
    uint32_t netAddr = 0xFFFFFFFF;
    std::array<uint8_t, 16> p2p_key{};
    bool p2p_key_set = false;
    std::map<std::pair<uint32_t, uint16_t>, NetP2PPeer> p2p_peers;

    void abort_all();
    void refresh_connection_state();
    void record_interface_transmit(int bytes, bool packet);
    void record_interface_receive(int bytes, bool packet);
    void set_interface_active(bool active);
    void reset_interface_idle_time();
    NetInterfaceStats get_interface_stats(bool track_idle) const;
    void set_interface_emulation(const std::array<uint8_t, 512> &parameters, uint32_t seed);
    std::array<uint8_t, 512> get_interface_emulation() const;
    int update_interface_promiscuous(int enabled, int references);
    void deinit();
};

struct NetCtlState {
    std::array<SceNetCtlCallback, 8> adhocCallbacks;
    std::array<SceNetCtlCallback, 8> callbacks;
    std::vector<SceNetCtlAdhocPeerInfo> adhocPeers;
    bool inited = false;
    std::thread adhocThread;
    std::atomic<bool> adhocCondVarReady = false;
    std::condition_variable adhocCondVar;
    SceNetCtlState adhocState = SCE_NET_CTL_STATE_DISCONNECTED;
    SceNetCtlEventType adhocEvent = SCE_NET_CTL_EVENT_TYPE_NONE;
    SceNetCtlEventType lastNotifiedAdhocEvent = SCE_NET_CTL_EVENT_TYPE_NONE;
    std::atomic<bool> adhocThreadRun = false;
    std::mutex mutex;

    void deinit() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            adhocThreadRun = false;
            adhocCondVarReady = false;
        }
        adhocCondVar.notify_all();
        if (adhocThread.joinable())
            adhocThread.join();

        adhocCallbacks.fill({ 0, 0 });
        callbacks.fill({ 0, 0 });
        adhocPeers.clear();
        adhocState = SCE_NET_CTL_STATE_DISCONNECTED;
        adhocEvent = SCE_NET_CTL_EVENT_TYPE_NONE;
        lastNotifiedAdhocEvent = SCE_NET_CTL_EVENT_TYPE_NONE;
        inited = false;
    }

    ~NetCtlState() {
        deinit();
    }
};
