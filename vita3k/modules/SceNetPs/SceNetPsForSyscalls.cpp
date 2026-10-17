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
#include "../SceNet/SceNet.h"

#include <cpu/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <net/socket.h>
#include <net/state.h>
#include <util/lock_and_find.h>
#include <util/net_utils.h>
#include <util/tracy.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

TRACY_MODULE_NAME(SceNetSyscall);

static const bool log_enabled = false;

static int net_syscall_result(int result) {
    const auto error_code = static_cast<uint32_t>(result);
    if ((error_code & 0xffff0000u) != 0x80410000u)
        return result;

    return -static_cast<int>(static_cast<SceNetKernelErrorCode>(error_code & 0xffu));
}

#define NET_SYSCALL_RESULT(result)                                                       \
    ([&]() {                                                                             \
        const int _result = net_syscall_result(result);                                  \
        if (_result < 0) {                                                               \
            LOG_ERROR("{} returned syscall errno {}", export_name, log_hex(_result));    \
            return RET_ERROR(_result);                                                   \
        }                                                                                \
        LOG_INFO_IF(log_enabled, "{} returned syscall result {}", export_name, _result); \
        return _result;                                                                  \
    }())

enum SceNetPsControlCommand : int32_t {
    SCE_NETPS_CONTROL_GET_STATISTICS = 0x00000001,
    SCE_NETPS_CONTROL_GET_DNS_CONFIG = 0x00000002,
    SCE_NETPS_CONTROL_GET_CONNECTION_PARAM = 0x00000004,
    SCE_NETPS_CONTROL_GET_PROMISCUOUS = 0x00000013,
    SCE_NETPS_CONTROL_GET_RANDOM = 0x00000014,
    SCE_NETPS_CONTROL_GET_SLEEP_COUNTS = 0x00000016,
    SCE_NETPS_CONTROL_GET_THREAD_LIST = 0x00000021,
    SCE_NETPS_CONTROL_TAKE_SENDERS = 0x00000022,
    SCE_NETPS_CONTROL_TAKE_DRIVER_EVENTS = 0x10000002,
    SCE_NETPS_CONTROL_GET_EMULATION = 0x10000012,
    SCE_NETPS_CONTROL_GET_PROCESS_POLICY = 0x10000017,
    SCE_NETPS_CONTROL_GET_ERROR = 0x10000018,
    SCE_NETPS_CONTROL_GET_THREAD_MODE = 0x10000019,
    SCE_NETPS_CONTROL_SET_DNS_CONFIG = 0x20000002,
    SCE_NETPS_CONTROL_REGISTER_DRIVER_CALLBACK = 0x20000003,
    SCE_NETPS_CONTROL_SET_CONNECTION_PARAM = 0x20000004,
    SCE_NETPS_CONTROL_POST_EVENTS = 0x20000005,
    SCE_NETPS_CONTROL_UPDATE_CONNECTIONS = 0x20000006,
    SCE_NETPS_CONTROL_UNREGISTER_DRIVER_CALLBACK = 0x20000007,
    SCE_NETPS_CONTROL_RESET_IDLE_TIME = 0x20000011,
    SCE_NETPS_CONTROL_SET_EMULATION = 0x20000012,
    SCE_NETPS_CONTROL_SET_PROMISCUOUS = 0x20000013,
    SCE_NETPS_CONTROL_SET_PROCESS_POLICY = 0x20000017,
    SCE_NETPS_CONTROL_SET_ERROR = 0x20000018,
    SCE_NETPS_CONTROL_SET_THREAD_MODE = 0x20000019,
    SCE_NETPS_CONTROL_SOCKET_SUSPEND_POLICY = 0x20000020,
    SCE_NETPS_CONTROL_SET_TCP_MSS = 0x20000023,
};

enum SceNetPsIoctlCommand : uint32_t {
    SCE_NETPS_IOCTL_GET_IF_FLAGS = 0xC0206911,
    SCE_NETPS_IOCTL_SET_IF_FLAGS = 0x80206910,
    SCE_NETPS_IOCTL_SET_IF_ADDRESS = 0x80206919,
    SCE_NETPS_IOCTL_ADD_IF_ALIAS = 0x8040691A,
    SCE_NETPS_IOCTL_SET_P2P_KEY = 0x802450C9,
};

template <>
std::string to_debug_str<SceNetPsControlCommand>(const MemState &, SceNetPsControlCommand command) {
    switch (command) {
    case SCE_NETPS_CONTROL_GET_STATISTICS: return "SCE_NETPS_CONTROL_GET_STATISTICS";
    case SCE_NETPS_CONTROL_GET_DNS_CONFIG: return "SCE_NETPS_CONTROL_GET_DNS_CONFIG";
    case SCE_NETPS_CONTROL_GET_CONNECTION_PARAM: return "SCE_NETPS_CONTROL_GET_CONNECTION_PARAM";
    case SCE_NETPS_CONTROL_GET_PROMISCUOUS: return "SCE_NETPS_CONTROL_GET_PROMISCUOUS";
    case SCE_NETPS_CONTROL_GET_RANDOM: return "SCE_NETPS_CONTROL_GET_RANDOM";
    case SCE_NETPS_CONTROL_GET_SLEEP_COUNTS: return "SCE_NETPS_CONTROL_GET_SLEEP_COUNTS";
    case SCE_NETPS_CONTROL_GET_THREAD_LIST: return "SCE_NETPS_CONTROL_GET_THREAD_LIST";
    case SCE_NETPS_CONTROL_TAKE_SENDERS: return "SCE_NETPS_CONTROL_TAKE_SENDERS";
    case SCE_NETPS_CONTROL_TAKE_DRIVER_EVENTS: return "SCE_NETPS_CONTROL_TAKE_DRIVER_EVENTS";
    case SCE_NETPS_CONTROL_GET_EMULATION: return "SCE_NETPS_CONTROL_GET_EMULATION";
    case SCE_NETPS_CONTROL_GET_PROCESS_POLICY: return "SCE_NETPS_CONTROL_GET_PROCESS_POLICY";
    case SCE_NETPS_CONTROL_GET_ERROR: return "SCE_NETPS_CONTROL_GET_ERROR";
    case SCE_NETPS_CONTROL_GET_THREAD_MODE: return "SCE_NETPS_CONTROL_GET_THREAD_MODE";
    case SCE_NETPS_CONTROL_SET_DNS_CONFIG: return "SCE_NETPS_CONTROL_SET_DNS_CONFIG";
    case SCE_NETPS_CONTROL_REGISTER_DRIVER_CALLBACK: return "SCE_NETPS_CONTROL_REGISTER_DRIVER_CALLBACK";
    case SCE_NETPS_CONTROL_SET_CONNECTION_PARAM: return "SCE_NETPS_CONTROL_SET_CONNECTION_PARAM";
    case SCE_NETPS_CONTROL_POST_EVENTS: return "SCE_NETPS_CONTROL_POST_EVENTS";
    case SCE_NETPS_CONTROL_UPDATE_CONNECTIONS: return "SCE_NETPS_CONTROL_UPDATE_CONNECTIONS";
    case SCE_NETPS_CONTROL_UNREGISTER_DRIVER_CALLBACK: return "SCE_NETPS_CONTROL_UNREGISTER_DRIVER_CALLBACK";
    case SCE_NETPS_CONTROL_RESET_IDLE_TIME: return "SCE_NETPS_CONTROL_RESET_IDLE_TIME";
    case SCE_NETPS_CONTROL_SET_EMULATION: return "SCE_NETPS_CONTROL_SET_EMULATION";
    case SCE_NETPS_CONTROL_SET_PROMISCUOUS: return "SCE_NETPS_CONTROL_SET_PROMISCUOUS";
    case SCE_NETPS_CONTROL_SET_PROCESS_POLICY: return "SCE_NETPS_CONTROL_SET_PROCESS_POLICY";
    case SCE_NETPS_CONTROL_SET_ERROR: return "SCE_NETPS_CONTROL_SET_ERROR";
    case SCE_NETPS_CONTROL_SET_THREAD_MODE: return "SCE_NETPS_CONTROL_SET_THREAD_MODE";
    case SCE_NETPS_CONTROL_SOCKET_SUSPEND_POLICY: return "SCE_NETPS_CONTROL_SOCKET_SUSPEND_POLICY";
    case SCE_NETPS_CONTROL_SET_TCP_MSS: return "SCE_NETPS_CONTROL_SET_TCP_MSS";
    }
    return fmt::format("UNKNOWN({})", log_hex(static_cast<int32_t>(command)));
}

template <>
std::string to_debug_str<SceNetPsIoctlCommand>(const MemState &, SceNetPsIoctlCommand command) {
    switch (command) {
    case SCE_NETPS_IOCTL_GET_IF_FLAGS: return "SCE_NETPS_IOCTL_GET_IF_FLAGS";
    case SCE_NETPS_IOCTL_SET_IF_FLAGS: return "SCE_NETPS_IOCTL_SET_IF_FLAGS";
    case SCE_NETPS_IOCTL_SET_IF_ADDRESS: return "SCE_NETPS_IOCTL_SET_IF_ADDRESS";
    case SCE_NETPS_IOCTL_ADD_IF_ALIAS: return "SCE_NETPS_IOCTL_ADD_IF_ALIAS";
    case SCE_NETPS_IOCTL_SET_P2P_KEY: return "SCE_NETPS_IOCTL_SET_P2P_KEY";
    }
    return fmt::format("UNKNOWN({})", log_hex(static_cast<uint32_t>(command)));
}

DECL_EXPORT(int, sceKernelGetRandomNumber, uint64_t *output, unsigned int size);

struct SceNetSyscallEpollWaitParameter {
    int eid;
    Ptr<SceNetEpollEvent> events;
    int maxevents;
    Ptr<int> timeout;
    int do_callbacks;
    int reserved;
};

struct SceNetPsIfreq {
    char name[16];
    union {
        uint32_t flags;
        SceNetSockaddrIn address;
        uint32_t reserved[4];
    } value;
};

struct SceNetPsIfAliasReq {
    char name[16];
    SceNetSockaddrIn address;
    SceNetSockaddrIn broadcast_address;
    SceNetSockaddrIn netmask;
};

struct SceNetSyscallP2pKeyRequest {
    SceNetSockaddrIn address;
    uint8_t key[16];
    uint16_t port;
    uint8_t flags;
    uint8_t group;
};

struct SceNetPsInterfaceAddress {
    uint32_t address;
    uint32_t broadcast_address;
    uint32_t broadcast_address_copy;
    uint32_t subnet_mask;
};

struct SceNetPsInterfaceInfo {
    char name[16];
    char parent_name[16];
    uint32_t interface_index;
    uint32_t parent_interface_index;
    SceNetPsInterfaceAddress addresses[2];
    uint32_t flags;
    uint32_t driver_type;
    SceNetEtherAddr hardware_address;
    uint32_t mtu;
    uint32_t driver_mtu;
    uint32_t tx_counters[8];
    uint32_t rx_counters[8];
    uint32_t software_tx_counters[8];
    uint32_t software_rx_counters[8];
    int32_t connection_state;
    int32_t idle_seconds;
    uint32_t private_controls[5];
    int32_t link_status;
    int32_t emulation_seed_present;
    int32_t promiscuous_controls[2];
    int32_t uptime_seconds;
    uint32_t generation;
    uint32_t reserved_tail[11];
};

struct SceNetPsDnsConfig {
    uint32_t dns_addresses[2];
    uint32_t flags;
    uint32_t reserved[7];
};

struct SceNetPsConnectionParam {
    int32_t connect_timeout_seconds;
    int32_t auto_connect_enabled;
    int32_t idle_timeout_seconds;
    int32_t auto_connect_policy;
    int32_t loopback_requested;
};

struct SceNetPsDriverEventControl {
    int32_t callback_id;
    int32_t clear_pending;
    uint32_t reserved[2];
    uint64_t events;
    int32_t error;
    uint32_t reserved2;
};

struct SceNetPsEmulationParam {
    uint16_t version;
    uint16_t option_number;
    uint16_t current_version;
    uint16_t result;
    uint32_t flags;
    uint32_t reserved1;
    uint8_t send[224];
    uint8_t receive[224];
    uint32_t seed;
    uint8_t reserved[44];
};

struct SceNetPsPromiscuousParam {
    int32_t enabled;
    int32_t references;
};

struct SceNetSyscallGetsockoptParam {
    int sid;
    SceNetProtocol level;
    SceNetSocketOption optname;
    Ptr<void> optval;
    Ptr<unsigned int> optlen;
    int reserved;
};

struct SceNetSyscallRecvfromParam {
    int sid;
    Ptr<void> buf;
    unsigned int len;
    int flags;
    Ptr<SceNetSockaddr> from;
    Ptr<unsigned int> fromlen;
};

struct SceNetSyscallSendtoParam {
    int sid;
    Ptr<const void> msg;
    unsigned int len;
    int flags;
    Ptr<const SceNetSockaddr> to;
    unsigned int tolen;
};

struct SceNetSyscallSetsockoptParam {
    int sid;
    SceNetProtocol level;
    SceNetSocketOption optname;
    Ptr<const void> optval;
    unsigned int optlen;
    int reserved;
};

struct SceNetSyscallSysctlParam {
    Ptr<const int> name;
    unsigned int name_length;
    Ptr<void> old_data;
    Ptr<unsigned int> old_length;
    Ptr<const void> new_data;
    unsigned int new_length;
};

static_assert(sizeof(SceNetSyscallEpollWaitParameter) == 24);
static_assert(sizeof(SceNetSyscallGetsockoptParam) == 24);
static_assert(sizeof(SceNetSyscallRecvfromParam) == 24);
static_assert(sizeof(SceNetSyscallSendtoParam) == 24);
static_assert(sizeof(SceNetSyscallSetsockoptParam) == 24);
static_assert(sizeof(SceNetSyscallSysctlParam) == 24);
static_assert(sizeof(SceNetSyscallP2pKeyRequest) == 36);
static_assert(sizeof(SceNetPsInterfaceAddress) == 16);
static_assert(sizeof(SceNetPsIfreq) == 32);
static_assert(offsetof(SceNetPsIfreq, value) == 16);
static_assert(sizeof(SceNetPsIfAliasReq) == 64);
static_assert(sizeof(SceNetPsInterfaceInfo) == 320);
static_assert(offsetof(SceNetPsInterfaceInfo, connection_state) == 224);
static_assert(sizeof(SceNetPsDnsConfig) == 40);
static_assert(offsetof(SceNetPsDnsConfig, flags) == 8);
static_assert(sizeof(SceNetPsConnectionParam) == 20);
static_assert(sizeof(SceNetPsDriverEventControl) == 32);
static_assert(sizeof(SceNetPsEmulationParam) == 512);
static_assert(sizeof(SceNetPsPromiscuousParam) == 8);

static void wakeup_epolls(NetState &net, const std::shared_ptr<Socket> &sock) {
    for (const auto &[id, epoll] : net.epolls)
        epoll->wakeup(sock);
}

static void update_pending_icm_connects(NetState &net) {
    for (auto &[id, sock] : net.socks) {
        if (!(sock->sce_icm_flags & SCE_NETPS_SOCKET_CONNECTING))
            continue;
        if (net.connection_state == 3) {
            sock->sce_icm_flags = (sock->sce_icm_flags & ~SCE_NETPS_SOCKET_CONNECTING) | SCE_NETPS_SOCKET_CONNECTED;
            sock->sce_icm_error = 0;
            sock->sce_icm_driver_index = -1;
            wakeup_epolls(net, sock);
            continue;
        }

        const auto driver = net.driver_contexts.find(sock->sce_icm_driver_index.load());
        if (driver != net.driver_contexts.end() && driver->second.last_error != 0) {
            sock->sce_icm_flags = (sock->sce_icm_flags & ~SCE_NETPS_SOCKET_CONNECTING) | SCE_NETPS_SOCKET_ERROR_NOTICE;
            sock->sce_icm_error = driver->second.last_error;
            wakeup_epolls(net, sock);
        }
    }
    net.auto_connect_cond_var.notify_all();
}

static bool parse_ipv4_address(const std::string &address, uint32_t &value) {
#ifdef _WIN32
    return InetPton(AF_INET, address.c_str(), &value) == 1;
#else
    return inet_pton(AF_INET, address.c_str(), &value) == 1;
#endif
}

static std::array<uint32_t, 2> get_host_dns_addresses(const net_utils::AssignedAddr &address) {
    std::array<uint32_t, 2> dns_addresses{};
    for (size_t index = 0; index < address.dns_servers.size(); ++index) {
        if (!address.dns_servers[index].empty())
            parse_ipv4_address(address.dns_servers[index], dns_addresses[index]);
    }
    return dns_addresses;
}

static bool is_usable_host_address(const net_utils::AssignedAddr &address) {
    return address.name != "localhost" && address.addr != "0.0.0.0" && address.addr.rfind("127.", 0) != 0 && address.addr.rfind("169.254.", 0) != 0;
}

static constexpr int32_t NETPS_INTERFACE_INDEX_LOOPBACK = 1;
static constexpr int32_t NETPS_INTERFACE_INDEX_ETHERNET = 2;
static constexpr int32_t NETPS_INTERFACE_INDEX_WIFI = 3;

static bool is_host_interface_index(int32_t interface_index) {
    return interface_index == NETPS_INTERFACE_INDEX_ETHERNET || interface_index == NETPS_INTERFACE_INDEX_WIFI;
}

static std::optional<net_utils::AssignedAddr> get_selected_host_address(
    int32_t &ethernet_index, int32_t &wifi_index, bool ethernet_mode) {
    auto &selected_index = ethernet_mode ? ethernet_index : wifi_index;
    const auto interface_type = ethernet_mode ? net_utils::InterfaceType::Ethernet : net_utils::InterfaceType::Wifi;
    return net_utils::get_selected_assigned_addr(selected_index, interface_type);
}

static bool add_host_default_route(NetState &net, uint32_t gateway, uint16_t interface_index, const char *interface_name) {
    if (!gateway)
        return false;

    const std::lock_guard<std::mutex> lock(net.route_mutex);
    const auto existing_default_route = std::find_if(net.routes.begin(), net.routes.end(), [](const NetRoute &route) {
        return route.destination == 0 && route.netmask == 0;
    });
    if (existing_default_route != net.routes.end()) {
        if (existing_default_route->gateway == gateway && existing_default_route->interface_index == interface_index)
            return false;
        existing_default_route->gateway = gateway;
        existing_default_route->flags = 0x3;
        existing_default_route->mtu = 1500;
        existing_default_route->interface_index = interface_index;
        existing_default_route->gateway_present = true;
        existing_default_route->interface_name = interface_name;
        return true;
    }

    NetRoute default_route{};
    default_route.gateway = gateway;
    default_route.flags = 0x3;
    default_route.mtu = 1500;
    default_route.interface_index = interface_index;
    default_route.gateway_present = true;
    default_route.interface_name = interface_name;
    net.routes.push_back(default_route);
    return true;
}

static bool initialize_host_network_context(NetState &net, std::mutex &kernel_mutex,
    const std::optional<net_utils::AssignedAddr> &host_address, const char *interface_name) {
    const size_t interface_flags_index = std::strcmp(interface_name, "eth0") == 0 ? 0 : 1;
    if (!host_address || !is_usable_host_address(*host_address)) {
        const std::lock_guard<std::mutex> lock(kernel_mutex);
        net.interface_flags = net.host_interface_flags[interface_flags_index];
        net.host_link_up = host_address && host_address->link_up;
        net.refresh_connection_state();
        net.set_interface_active(net.host_link_up && (net.interface_flags & 0x41) == 0x41);
        update_pending_icm_connects(net);
        return false;
    }

    const auto &selected_address = *host_address;
    uint32_t address = 0;
    uint32_t netmask = 0;
    if (!parse_ipv4_address(selected_address.addr, address) || !parse_ipv4_address(selected_address.netMask, netmask)) {
        LOG_WARN("NetPS: invalid host IPv4 address or netmask {}/{}", selected_address.addr, selected_address.netMask);
        return false;
    }

    const uint32_t broadcast = htonl(ntohl(address) | ~ntohl(netmask));
    {
        const std::lock_guard<std::mutex> lock(kernel_mutex);
        net.interface_flags = net.host_interface_flags[interface_flags_index];
        if (!net.interface_address_configured) {
            net.netAddr = address;
            net.netMask = netmask;
            net.broadcastAddr = broadcast;
            net.interface_address_configured = true;
        }
        if (!net.dns_config_initialized) {
            net.dns_addresses = get_host_dns_addresses(selected_address);
            net.dns_config_initialized = true;
        }
        net.host_link_up = selected_address.link_up;
        net.refresh_connection_state();
        net.set_interface_active(net.host_link_up && (net.interface_flags & 0x41) == 0x41);
        update_pending_icm_connects(net);
    }

    uint32_t gateway = 0;
    const uint16_t interface_index = std::strcmp(interface_name, "wlan0") == 0
        ? NETPS_INTERFACE_INDEX_WIFI
        : NETPS_INTERFACE_INDEX_ETHERNET;
    if (parse_ipv4_address(selected_address.gateway, gateway) && add_host_default_route(net, gateway, interface_index, interface_name))
        LOG_INFO_IF(log_enabled, "NetPS: added host default route gateway={} interface={}", selected_address.gateway, interface_name);
    return true;
}

EXPORT(int, sceNetSyscallAccept, int sid, Ptr<SceNetSockaddr> addr, Ptr<unsigned int> addrlen) {
    const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name);
    if (!process_network_context)
        return NET_SYSCALL_RESULT(process_network_context.error());

    const MemState &mem = process_network_context->thread->cpu->mem;
    if (addrlen && !is_valid_addr_len(mem, addrlen.address(), sizeof(unsigned int)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    if (addr && !is_valid_addr_len(mem, addr.address(), sizeof(SceNetSockaddr)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetAccept, sid, addr.get(mem), addrlen.get(mem)));
}

EXPORT(int, sceNetSyscallBind, int sid, Ptr<const SceNetSockaddr> addr, unsigned int addrlen) {
    const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name);
    if (!process_network_context)
        return NET_SYSCALL_RESULT(process_network_context.error());

    const MemState &mem = process_network_context->thread->cpu->mem;
    if (addr && !is_valid_addr_len(mem, addr.address(), addrlen))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetBind, sid, addr.get(mem), addrlen));
}

EXPORT(int, sceNetSyscallClose, int sid) {
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetSocketClose, sid));
}

EXPORT(int, sceNetSyscallConnect, int sid, Ptr<const SceNetSockaddr> addr, unsigned int addrlen) {
    const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name);
    if (!process_network_context)
        return NET_SYSCALL_RESULT(process_network_context.error());

    const MemState &mem = process_network_context->thread->cpu->mem;
    if (addr && !is_valid_addr_len(mem, addr.address(), addrlen))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetConnect, sid, addr.get(mem), addrlen));
}

EXPORT(int, sceNetSyscallControl, int interface_index, int command, Ptr<void> data, int length) {
    TRACY_FUNC(sceNetSyscallControl, interface_index, command, data, length);
    const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name);
    if (!process_network_context)
        return NET_SYSCALL_RESULT(process_network_context.error());

    const MemState &mem = process_network_context->thread->cpu->mem;
    if (data && length > 0 && !is_valid_addr_len(mem, data.address(), static_cast<size_t>(length)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    void *control_data = data.get(mem);

    const auto caller_process_id = emuenv.kernel.get_thread_process_id(thread_id);
    if (!caller_process_id)
        return NET_SYSCALL_RESULT(SCE_KERNEL_ERROR_ERROR);
    LOG_DEBUG_IF(log_enabled, "sceNetSyscallControl pid={} interface={} command={} ({}) length={}",
        *caller_process_id, interface_index,
        to_debug_str(emuenv.mem, static_cast<SceNetPsControlCommand>(command)), log_hex(command), length);
    switch (command) {
    case SCE_NETPS_CONTROL_GET_DNS_CONFIG: {
        if (interface_index != -1 || !data || length < static_cast<int>(sizeof(SceNetPsDnsConfig)))
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        const bool ethernet_mode = emuenv.cfg.current_config.pstv_mode;
        const auto host_address = get_selected_host_address(
            emuenv.cfg.ethernet_addr, emuenv.cfg.wifi_addr, ethernet_mode);
        auto &net = *process_network_context->net;
        const char *interface_name = ethernet_mode ? "eth0" : "wlan0";
        initialize_host_network_context(net, emuenv.kernel.mutex, host_address, interface_name);
        SceNetPsDnsConfig config{};
        {
            const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
            std::copy(net.dns_addresses.begin(), net.dns_addresses.end(), std::begin(config.dns_addresses));
            config.flags = net.dns_config_flags;
            if (!net.dns_config_subscribed) {
                config.flags |= 1;
                net.dns_config_subscribed = true;
            }
        }
        std::memcpy(control_data, &config, sizeof(config));
        LOG_INFO_IF(log_enabled, "sceNetSyscallControl GET_DNS_CONFIG primary={} secondary={} flags={}",
            config.dns_addresses[0], config.dns_addresses[1], config.flags);
        break;
    }
    case SCE_NETPS_CONTROL_SET_DNS_CONFIG: {
        if (interface_index != -1 || !data || length < static_cast<int>(sizeof(SceNetPsDnsConfig)))
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        SceNetPsDnsConfig config{};
        std::memcpy(&config, control_data, sizeof(config));
        auto &net = *process_network_context->net;
        {
            const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
            std::copy(std::begin(config.dns_addresses), std::end(config.dns_addresses), net.dns_addresses.begin());
            net.dns_config_flags = config.flags;
            net.dns_config_initialized = true;
            net.dns_config_subscribed = false;
        }
        LOG_INFO_IF(log_enabled, "sceNetSyscallControl SET_DNS_CONFIG primary={} secondary={} flags={}",
            config.dns_addresses[0], config.dns_addresses[1], config.flags);
        break;
    }
    case SCE_NETPS_CONTROL_GET_RANDOM: {
        if (interface_index != -1 || !data || length < static_cast<int>(sizeof(uint32_t)))
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        uint64_t random_value = 0;
        const int rng_result = CALL_EXPORT(sceKernelGetRandomNumber, &random_value, sizeof(uint32_t));
        if (rng_result != 0)
            return NET_SYSCALL_RESULT(rng_result);

        std::memcpy(control_data, &random_value, sizeof(uint32_t));
        break;
    }
    case SCE_NETPS_CONTROL_TAKE_SENDERS: {
        if (interface_index != -1 || !data || length < 68)
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        auto &pending_events = process_network_context->net->pending_control_events;
        for (size_t event_index = 0; event_index < pending_events.size(); ++event_index) {
            if (pending_events[event_index]) {
                LOG_INFO_IF(log_enabled, "sceNetSyscallControl pending control event index={} value={}",
                    event_index, pending_events[event_index]);
                break;
            }
        }
        std::memcpy(control_data, pending_events.data(), pending_events.size());
        pending_events.fill(0);
        break;
    }
    case SCE_NETPS_CONTROL_SOCKET_SUSPEND_POLICY: {
        if (interface_index != -1 || !data || length < static_cast<int>(sizeof(uint32_t)))
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        std::memcpy(&process_network_context->net->control_value, control_data, sizeof(uint32_t));
        break;
    }
    case SCE_NETPS_CONTROL_SET_CONNECTION_PARAM: {
        if (interface_index == 0 || !data || length < static_cast<int>(sizeof(SceNetPsConnectionParam))) {
            LOG_WARN("sceNetSyscallControl: invalid connection-parameter request interface {}, data {}, length {}",
                interface_index, static_cast<bool>(data), length);
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
        }

        SceNetPsConnectionParam parameters{};
        std::memcpy(&parameters, control_data, sizeof(parameters));
        if (parameters.idle_timeout_seconds < 0 || parameters.auto_connect_policy < -1 || parameters.auto_connect_policy > 99)
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        LOG_INFO_IF(log_enabled, "sceNetSyscallControl SET_CONNECTION_PARAM connect_timeout={} auto_connect_enabled={} idle_timeout={} auto_connect_policy={} loopback_requested={}",
            parameters.connect_timeout_seconds, parameters.auto_connect_enabled,
            parameters.idle_timeout_seconds, parameters.auto_connect_policy,
            parameters.loopback_requested);

        auto &net = *process_network_context->net;
        {
            const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
            if (parameters.connect_timeout_seconds != 0)
                net.connect_timeout_seconds = parameters.connect_timeout_seconds;
            if (parameters.idle_timeout_seconds != 0)
                net.idle_timeout_seconds = parameters.idle_timeout_seconds;
            net.auto_connect_policy = parameters.auto_connect_policy;
            net.loopback_requested = parameters.loopback_requested != 0;
            net.refresh_connection_state();
            update_pending_icm_connects(net);
        }
        break;
    }
    case SCE_NETPS_CONTROL_GET_PROMISCUOUS: {
        if (!is_host_interface_index(interface_index))
            return NET_SYSCALL_RESULT(-SCE_NET_EIO);
        if (!data || length < static_cast<int>(sizeof(SceNetPsPromiscuousParam)))
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        const auto stats = process_network_context->net->get_interface_stats(false);
        const SceNetPsPromiscuousParam parameters{
            stats.promiscuous_enabled,
            stats.promiscuous_references,
        };
        std::memcpy(control_data, &parameters, sizeof(parameters));
        break;
    }
    case SCE_NETPS_CONTROL_SET_PROMISCUOUS: {
        if (!is_host_interface_index(interface_index))
            return NET_SYSCALL_RESULT(-SCE_NET_EIO);
        if (!data || length < static_cast<int>(sizeof(SceNetPsPromiscuousParam)))
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        SceNetPsPromiscuousParam parameters{};
        std::memcpy(&parameters, control_data, sizeof(parameters));
        auto &net = *process_network_context->net;
        int result = 0;
        {
            const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
            result = net.update_interface_promiscuous(parameters.enabled, parameters.references);
            if (result >= 0) {
                const auto stats = net.get_interface_stats(false);
                auto &interface_flags = net.host_interface_flags[interface_index == NETPS_INTERFACE_INDEX_ETHERNET ? 0 : 1];
                if (stats.promiscuous_enabled)
                    interface_flags |= 0x00010000u;
                else
                    interface_flags &= ~0x00010000u;
                if (interface_index == (emuenv.cfg.current_config.pstv_mode ? NETPS_INTERFACE_INDEX_ETHERNET : NETPS_INTERFACE_INDEX_WIFI))
                    net.interface_flags = interface_flags;
            }
        }
        if (result < 0)
            return NET_SYSCALL_RESULT(result);
        break;
    }
    case SCE_NETPS_CONTROL_RESET_IDLE_TIME: {
        if (!is_host_interface_index(interface_index))
            return NET_SYSCALL_RESULT(-SCE_NET_EIO);
        if (!data || length < static_cast<int>(sizeof(uint32_t)))
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        uint32_t reserved = 0;
        std::memcpy(&reserved, control_data, sizeof(reserved));
        if (reserved != 0)
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
        process_network_context->net->reset_interface_idle_time();
        break;
    }
    case SCE_NETPS_CONTROL_TAKE_DRIVER_EVENTS: {
        if (!data || length < static_cast<int>(sizeof(SceNetPsDriverEventControl)))
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        SceNetPsDriverEventControl control{};
        std::memcpy(&control, control_data, sizeof(control));
        auto &driver_contexts = process_network_context->net->driver_contexts;
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        const auto driver_context = driver_contexts.find(interface_index);
        if (driver_context == driver_contexts.end())
            return NET_SYSCALL_RESULT(-SCE_NET_EIO);

        const auto callback = std::find_if(driver_context->second.callbacks.begin(),
            driver_context->second.callbacks.end(), [&control](const NetDriverCallback &entry) {
                return entry.callback_id == control.callback_id;
            });
        if (callback == driver_context->second.callbacks.end())
            return NET_SYSCALL_RESULT(-SCE_NET_EIO);

        control.events = callback->pending_events;
        control.error = driver_context->second.last_error;
        if (control.events || control.error) {
            LOG_INFO_IF(log_enabled, "sceNetSyscallControl driver event interface={} callback={} events={} error={}",
                interface_index, control.callback_id, control.events, control.error);
        }
        callback->pending_events = 0;
        std::memcpy(control_data, &control, sizeof(control));
        break;
    }
    case SCE_NETPS_CONTROL_REGISTER_DRIVER_CALLBACK: {
        if (!data || length < 8) {
            const auto result = NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
            LOG_INFO_IF(log_enabled, "sceNetSyscallControl command={} ({}) data={} length={} result={}",
                to_debug_str(emuenv.mem, SCE_NETPS_CONTROL_REGISTER_DRIVER_CALLBACK),
                log_hex(SCE_NETPS_CONTROL_REGISTER_DRIVER_CALLBACK), control_data, length, log_hex(result));
            return result;
        }

        int32_t callback_id = 0;
        int32_t clear_pending = 0;
        std::memcpy(&callback_id, control_data, sizeof(callback_id));
        std::memcpy(&clear_pending, static_cast<const uint8_t *>(control_data) + sizeof(callback_id), sizeof(clear_pending));
        if (callback_id <= 0)
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        auto &driver_contexts = process_network_context->net->driver_contexts;
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        auto &callbacks = driver_contexts[interface_index].callbacks;
        const auto existing = std::find_if(callbacks.begin(), callbacks.end(), [callback_id](const NetDriverCallback &callback) {
            return callback.callback_id == callback_id;
        });
        if (existing != callbacks.end()) {
            existing->clear_pending = clear_pending != 0;
            if (existing->clear_pending)
                existing->pending_events = 0;
            break;
        }

        const auto available = std::find_if(callbacks.begin(), callbacks.end(), [](const NetDriverCallback &callback) {
            return callback.callback_id == 0;
        });
        if (available == callbacks.end()) {
            LOG_WARN("sceNetSyscallControl: driver callback table is full for interface {}, callback {}",
                interface_index, callback_id);
            return NET_SYSCALL_RESULT(-SCE_NET_ENOSPC);
        }

        *available = { callback_id, clear_pending != 0 };
        if (is_host_interface_index(interface_index) && process_network_context->net->connection_state == 3)
            available->pending_events = 4; // NetPS link-up event.
        LOG_INFO_IF(log_enabled, "sceNetSyscallControl registered driver callback interface={} callback={} initial_events={}",
            interface_index, callback_id, available->pending_events);
        break;
    }
    case SCE_NETPS_CONTROL_POST_EVENTS: {
        if (!data || length < static_cast<int>(sizeof(SceNetPsDriverEventControl)))
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        SceNetPsDriverEventControl control{};
        std::memcpy(&control, control_data, sizeof(control));
        auto &driver_contexts = process_network_context->net->driver_contexts;
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        const auto driver_context = driver_contexts.find(interface_index);
        if (driver_context == driver_contexts.end())
            return NET_SYSCALL_RESULT(-SCE_NET_EIO);

        const auto callback = std::find_if(driver_context->second.callbacks.begin(),
            driver_context->second.callbacks.end(), [&control](const NetDriverCallback &entry) {
                return entry.callback_id == control.callback_id;
            });
        if (callback == driver_context->second.callbacks.end())
            return NET_SYSCALL_RESULT(-SCE_NET_EIO);

        const uint64_t pending_events_before = callback->pending_events;
        callback->pending_events |= control.events;
        if (control.error)
            driver_context->second.last_error = control.error;
        update_pending_icm_connects(*process_network_context->net);
        LOG_DEBUG_IF(log_enabled, "sceNetSyscallControl POST_EVENTS interface={} callback={} events={} pending_before={} pending_after={} error={} connection_state={}",
            interface_index, control.callback_id, control.events, pending_events_before,
            callback->pending_events, control.error, process_network_context->net->connection_state);
        break;
    }
    case SCE_NETPS_CONTROL_UPDATE_CONNECTIONS: {
        if (!data || length < 8)
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        int32_t error = 0;
        int32_t flags = 0;
        std::memcpy(&error, control_data, sizeof(error));
        std::memcpy(&flags, static_cast<const uint8_t *>(control_data) + sizeof(error), sizeof(flags));
        if (flags & ~1)
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        auto &net = *process_network_context->net;
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        if (flags & 1) {
            const bool update_pending = std::any_of(net.driver_contexts.begin(), net.driver_contexts.end(),
                [](const auto &entry) {
                    return std::any_of(entry.second.callbacks.begin(), entry.second.callbacks.end(),
                        [](const NetDriverCallback &callback) {
                            return (callback.pending_events & 0x01000000) != 0;
                        });
                });
            if (update_pending) {
                LOG_DEBUG_IF(log_enabled, "sceNetSyscallControl UPDATE_CONNECTIONS skipped interface={} pending event=0x01000000",
                    interface_index);
                break;
            }
        }

        const int previous_connection_state = net.connection_state;
        net.refresh_connection_state();
        int updated = 0;
        const auto update_driver = [&net, error, &updated](NetDriverContext &driver) {
            switch (net.connection_state) {
            case 1:
                if (net.auto_connect_policy != -1) {
                    net.auto_connect_enabled = net.auto_connect_policy > 0;
                    net.connection_state = 2;
                }
                [[fallthrough]];
            case 2:
            case 3:
            case 4:
                driver.connection_error = error;
                ++updated;
                break;
            default:
                driver.connection_error = 0;
                break;
            }
        };

        if (interface_index == -1) {
            for (auto &[index, driver] : net.driver_contexts)
                update_driver(driver);
        } else {
            const auto driver = net.driver_contexts.find(interface_index);
            if (driver == net.driver_contexts.end())
                return NET_SYSCALL_RESULT(-SCE_NET_EIO);
            update_driver(driver->second);
        }

        update_pending_icm_connects(net);
        LOG_DEBUG_IF(log_enabled, "sceNetSyscallControl UPDATE_CONNECTIONS interface={} error={} flags={} previous_state={} state={} updated={}",
            interface_index, error, flags, previous_connection_state, net.connection_state, updated);
        break;
    }
    case SCE_NETPS_CONTROL_UNREGISTER_DRIVER_CALLBACK: {
        if (!data || length < static_cast<int>(sizeof(int32_t)))
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        int32_t callback_id = 0;
        std::memcpy(&callback_id, control_data, sizeof(callback_id));
        auto &driver_contexts = process_network_context->net->driver_contexts;
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        const auto driver_context = driver_contexts.find(interface_index);
        if (driver_context == driver_contexts.end())
            return NET_SYSCALL_RESULT(-SCE_NET_ENOENT);

        auto &callbacks = driver_context->second.callbacks;
        const auto callback = std::find_if(callbacks.begin(), callbacks.end(), [callback_id](const NetDriverCallback &entry) {
            return entry.callback_id == callback_id;
        });
        if (callback == callbacks.end())
            return NET_SYSCALL_RESULT(-SCE_NET_ENOENT);

        *callback = {};
        break;
    }
    case SCE_NETPS_CONTROL_GET_EMULATION: {
        if (!is_host_interface_index(interface_index))
            return NET_SYSCALL_RESULT(-SCE_NET_EIO);
        if (!data || length < static_cast<int>(sizeof(SceNetPsEmulationParam)))
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        const auto parameters = process_network_context->net->get_interface_emulation();
        std::memcpy(control_data, parameters.data(), parameters.size());
        break;
    }
    case SCE_NETPS_CONTROL_SET_EMULATION: {
        if ((!is_host_interface_index(interface_index) && interface_index != SCE_NET_ERROR_ENODATA) || !data || length < static_cast<int>(sizeof(SceNetPsEmulationParam))) {
            LOG_WARN("sceNetSyscallControl: invalid driver-emulation request interface {}, data {}, length {}",
                interface_index, static_cast<bool>(data), length);
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
        }

        SceNetPsEmulationParam parameters{};
        std::memcpy(&parameters, control_data, sizeof(parameters));
        if (parameters.version && ((parameters.version & 0xFF) > 4 || (parameters.version & 0xFF00) != 0x100)) {
            LOG_WARN("sceNetSyscallControl: unsupported driver-emulation version {:#x}", parameters.version);
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
        }
        parameters.flags &= ~0x700u;
        if (parameters.option_number || (parameters.flags & ~1u) || parameters.reserved1 || std::any_of(std::begin(parameters.send), std::end(parameters.send), [](uint8_t value) { return value != 0; }) || std::any_of(std::begin(parameters.receive), std::end(parameters.receive), [](uint8_t value) { return value != 0; }) || std::any_of(std::begin(parameters.reserved), std::end(parameters.reserved), [](uint8_t value) { return value != 0; })) {
            LOG_WARN("sceNetSyscallControl: non-default driver emulation parameters are unsupported");
            return NET_SYSCALL_RESULT(-SCE_NET_EOPNOTSUPP);
        }

        parameters.current_version = 0;
        parameters.result = 0;
        if (!parameters.seed) {
            uint64_t random_value = 0;
            const int rng_result = CALL_EXPORT(sceKernelGetRandomNumber, &random_value, sizeof(parameters.seed));
            if (rng_result != 0)
                return NET_SYSCALL_RESULT(rng_result);
            std::memcpy(&parameters.seed, &random_value, sizeof(parameters.seed));
        }
        parameters.seed = 0;
        std::memcpy(control_data, &parameters, sizeof(parameters));
        std::array<uint8_t, sizeof(SceNetPsEmulationParam)> stored_parameters{};
        std::memcpy(stored_parameters.data(), &parameters, sizeof(parameters));
        process_network_context->net->set_interface_emulation(stored_parameters, parameters.seed);
        break;
    }
    case SCE_NETPS_CONTROL_SET_PROCESS_POLICY: {
        if (interface_index != -1 || !data || length < 8)
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        int32_t process_id = 0;
        int32_t policy = 0;
        std::memcpy(&process_id, control_data, sizeof(process_id));
        std::memcpy(&policy, static_cast<const uint8_t *>(control_data) + sizeof(process_id), sizeof(policy));
        if (process_id != *caller_process_id)
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        process_network_context->net->process_policy = policy;
        break;
    }
    case SCE_NETPS_CONTROL_GET_ERROR:
    case SCE_NETPS_CONTROL_SET_ERROR: {
        if (!data || length < 8)
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        int32_t error = 0;
        int32_t selector = 0;
        std::memcpy(&error, control_data, sizeof(error));
        std::memcpy(&selector, static_cast<const uint8_t *>(control_data) + sizeof(error), sizeof(selector));
        auto &net = *process_network_context->net;
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        const bool thread_error = (selector & 1) != 0;

        if (command == SCE_NETPS_CONTROL_GET_ERROR) {
            if (thread_error) {
                const auto stored_error = net.thread_errors.find(thread_id);
                error = stored_error == net.thread_errors.end() ? 0 : stored_error->second;
            } else {
                error = net.process_error;
            }
            std::memcpy(control_data, &error, sizeof(error));
        } else {
            if (thread_error)
                net.thread_errors[thread_id] = error;
            else
                net.process_error = error;
        }

        LOG_INFO_IF(log_enabled, "sceNetSyscallControl command={} ({}) scope={} error={}",
            to_debug_str(emuenv.mem, static_cast<SceNetPsControlCommand>(command)), log_hex(command),
            thread_error ? "thread" : "process", log_hex(error));
        break;
    }
    case SCE_NETPS_CONTROL_GET_THREAD_MODE:
    case SCE_NETPS_CONTROL_SET_THREAD_MODE: {
        if (!data || length < 12)
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        int32_t target_thread_id = 0;
        int32_t mode = 0;
        std::memcpy(&target_thread_id, control_data, sizeof(target_thread_id));
        std::memcpy(&mode, static_cast<const uint8_t *>(control_data) + 8, sizeof(mode));
        const auto target_process_id = emuenv.kernel.get_thread_process_id(target_thread_id);
        if (!target_process_id || *target_process_id != *caller_process_id)
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        auto &thread_modes = process_network_context->net->thread_modes;
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        if (command == SCE_NETPS_CONTROL_GET_THREAD_MODE) {
            const auto thread_mode = thread_modes.find(target_thread_id);
            if (thread_mode == thread_modes.end())
                return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
            std::memcpy(static_cast<uint8_t *>(control_data) + 8, &thread_mode->second, sizeof(thread_mode->second));
        } else {
            thread_modes[target_thread_id] = mode;
        }
        break;
    }
    default:
        LOG_WARN("sceNetSyscallControl: unsupported interface {}, command={} ({}), length {}",
            interface_index, to_debug_str(emuenv.mem, static_cast<SceNetPsControlCommand>(command)),
            log_hex(command), length);
        return NET_SYSCALL_RESULT(-SCE_NET_EOPNOTSUPP);
    }
    return NET_SYSCALL_RESULT(0);
}

EXPORT(int, sceNetSyscallDescriptorClose) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetSyscallDescriptorCreate) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetSyscallDescriptorCtl) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetSyscallDumpAbort) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetSyscallDumpClose) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetSyscallDumpCreate) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetSyscallDumpRead) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetSyscallEpollAbort, int eid, int flags) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetSyscallEpollClose, int eid) {
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetEpollDestroy, eid));
}

EXPORT(int, sceNetSyscallEpollCreate, Ptr<const char> name, int flags) {
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetEpollCreate, name.get(thread->cpu->mem), flags));
}

EXPORT(int, sceNetSyscallEpollCtl, int eid, SceNetEpollControlFlag op, int id, Ptr<SceNetEpollEvent> ev) {
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    MemState &mem = thread->cpu->mem;
    if (ev && !is_valid_addr_len(mem, ev.address(), sizeof(SceNetEpollEvent)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetEpollControl, eid, op, id, ev.get(mem)));
}

EXPORT(int, sceNetSyscallEpollWait, Ptr<SceNetSyscallEpollWaitParameter> param) {
    if (!param)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    MemState &mem = thread->cpu->mem;
    if (!is_valid_addr_len(mem, param.address(), sizeof(SceNetSyscallEpollWaitParameter)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    const auto *arguments = param.get(mem);
    if (!arguments || !arguments->timeout || arguments->maxevents < 0)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    if (!is_valid_addr_len(mem, arguments->timeout.address(), sizeof(int)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    int timeout = 0;
    if (!read_guest_memory(mem, arguments->timeout.address(), &timeout, sizeof(timeout)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    if (arguments->maxevents > 0 && (!arguments->events || !is_valid_addr_len(mem, arguments->events.address(), static_cast<size_t>(arguments->maxevents) * sizeof(SceNetEpollEvent))))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetEpollWait, arguments->eid, arguments->events.get(mem),
        arguments->maxevents, timeout));
}

EXPORT(int, sceNetSyscallGetIfList, Ptr<SceNetPsInterfaceInfo> list, int count) {
    TRACY_FUNC(sceNetSyscallGetIfList, list, count);
    if (count < 0)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

    const auto caller_process_id = emuenv.kernel.get_thread_process_id(thread_id);
    const auto caller_pid = caller_process_id.value_or(-1);
    LOG_DEBUG_IF(log_enabled, "sceNetSyscallGetIfList, caller_pid={} list={} count={}", caller_pid, list, count);

    const auto assigned_addresses = net_utils::get_all_assigned_addrs();
    const auto ethernet_addresses = net_utils::get_assigned_addrs_by_type(
        assigned_addresses, net_utils::InterfaceType::Ethernet);
    const auto wifi_addresses = net_utils::get_assigned_addrs_by_type(
        assigned_addresses, net_utils::InterfaceType::Wifi);
    const int available_interface_count = 1 + static_cast<int>(!ethernet_addresses.empty()) + static_cast<int>(!wifi_addresses.empty());
    if (!list)
        return NET_SYSCALL_RESULT(available_interface_count);
    if (count == 0)
        return NET_SYSCALL_RESULT(0);

    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    MemState &mem = thread->cpu->mem;
    const auto list_size = static_cast<size_t>(count) * sizeof(SceNetPsInterfaceInfo);
    if (!is_valid_addr_len(mem, list.address(), list_size))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    auto *output_list = list.get(mem);
    if (!output_list)
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);

    std::array<SceNetPsInterfaceInfo, 3> interfaces{};
    int returned_interfaces = 0;
    SceNetPsInterfaceInfo loopback{};
    std::memcpy(loopback.name, "lo0", 3);
    loopback.interface_index = NETPS_INTERFACE_INDEX_LOOPBACK;
    loopback.flags = 0x49;
    loopback.mtu = 16384;
    loopback.driver_mtu = 16384;
    loopback.addresses[0].address = htonl(0x7F000001);
    loopback.addresses[0].subnet_mask = htonl(0xFF000000);
    interfaces[returned_interfaces++] = loopback;

    if (returned_interfaces < count && (!ethernet_addresses.empty() || !wifi_addresses.empty())) {
        const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name);
        if (!process_network_context)
            return NET_SYSCALL_RESULT(process_network_context.error());

        auto &net = *process_network_context->net;
        std::array<uint32_t, 2> host_interface_flags{};
        int32_t idle_timeout_seconds = 0;
        int32_t auto_connect_policy = 0;
        {
            const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
            host_interface_flags = net.host_interface_flags;
            idle_timeout_seconds = net.idle_timeout_seconds;
            auto_connect_policy = net.auto_connect_policy;
        }
        const auto stats = net.get_interface_stats(auto_connect_policy > 0);
        const auto append_host_interface = [&](const net_utils::AssignedAddr &host_address,
                                               const char *interface_name,
                                               uint32_t interface_index,
                                               uint32_t driver_type) -> int {
            SceNetPsInterfaceInfo info{};
            std::memcpy(info.name, interface_name, std::strlen(interface_name));
            info.interface_index = interface_index;
            info.driver_type = driver_type;
            info.mtu = 1500;
            info.driver_mtu = 1500;
            const uint32_t interface_flags = host_interface_flags[driver_type == 1 ? 0 : 1];
            info.flags = interface_flags;
            if (!host_address.link_up)
                info.flags &= ~0x40u;
            info.link_status = host_address.link_up ? 1 : 0;
            const bool interface_ready = host_address.link_up && is_usable_host_address(host_address) && (interface_flags & 0x41) == 0x41;
            info.connection_state = auto_connect_policy > 0 ? (interface_ready ? 3 : 4) : 0;
            info.private_controls[2] = static_cast<uint32_t>(idle_timeout_seconds);
            info.private_controls[3] = static_cast<uint32_t>(auto_connect_policy);

            const bool is_active_interface = (driver_type == 1) == emuenv.cfg.current_config.pstv_mode;
            if (is_active_interface) {
                info.idle_seconds = stats.idle_seconds;
                std::copy(stats.tx_counters.begin(), stats.tx_counters.end(), std::begin(info.tx_counters));
                std::copy(stats.rx_counters.begin(), stats.rx_counters.end(), std::begin(info.rx_counters));
                std::copy(stats.software_tx_counters.begin(), stats.software_tx_counters.end(),
                    std::begin(info.software_tx_counters));
                std::copy(stats.software_rx_counters.begin(), stats.software_rx_counters.end(),
                    std::begin(info.software_rx_counters));
                info.emulation_seed_present = stats.emulation_seed_present;
                info.promiscuous_controls[0] = stats.promiscuous_enabled;
                info.promiscuous_controls[1] = stats.promiscuous_references;
                info.uptime_seconds = stats.uptime_seconds;
                info.generation = stats.generation;
            }

            uint32_t address = 0;
            uint32_t subnet_mask = 0;
            if (parse_ipv4_address(host_address.addr, address) && parse_ipv4_address(host_address.netMask, subnet_mask)) {
                const uint32_t broadcast = htonl(ntohl(address) | ~ntohl(subnet_mask));
                const size_t address_index = (ntohl(address) >> 16) == 0xA9FE ? 1 : 0;
                auto &record = info.addresses[address_index];
                record.address = address;
                record.subnet_mask = subnet_mask;
                record.broadcast_address = broadcast;
                record.broadcast_address_copy = broadcast;
            } else {
                LOG_WARN("sceNetSyscallGetIfList: invalid IPv4 address or netmask for {}: {}/{}",
                    interface_name, host_address.addr, host_address.netMask);
            }

            if (!host_address.has_hardware_address) {
                LOG_WARN("sceNetSyscallGetIfList: no hardware address available for {} ({})",
                    interface_name, host_address.name);
                return -SCE_NET_EINVAL;
            }
            std::copy(host_address.hardware_address.begin(), host_address.hardware_address.end(),
                std::begin(info.hardware_address.data));

            LOG_INFO_IF(log_enabled, "sceNetSyscallGetIfList record name={} host_interface={} index={} driver_type={} flags={} connection_state={} link_status={} address={}/{}",
                info.name, host_address.name, info.interface_index, info.driver_type,
                log_hex(static_cast<int>(info.flags)), info.connection_state, info.link_status,
                host_address.addr, host_address.netMask);
            interfaces[returned_interfaces++] = info;
            return 0;
        };

        if (!ethernet_addresses.empty() && returned_interfaces < count) {
            const int result = append_host_interface(ethernet_addresses.front(), "eth0",
                NETPS_INTERFACE_INDEX_ETHERNET, 1);
            if (result < 0)
                return NET_SYSCALL_RESULT(result);
        }
        if (!wifi_addresses.empty() && returned_interfaces < count) {
            const int result = append_host_interface(wifi_addresses.front(), "wlan0",
                NETPS_INTERFACE_INDEX_WIFI, 2);
            if (result < 0)
                return NET_SYSCALL_RESULT(result);
        }
    }

    std::memset(output_list, 0, list_size);
    std::memcpy(output_list, interfaces.data(),
        static_cast<size_t>(returned_interfaces) * sizeof(SceNetPsInterfaceInfo));

    return NET_SYSCALL_RESULT(returned_interfaces);
}

EXPORT(int, sceNetSyscallGetSockinfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetSyscallGetpeername, int sid, Ptr<SceNetSockaddr> name, Ptr<unsigned int> namelen) {
    const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name);
    if (!process_network_context)
        return NET_SYSCALL_RESULT(process_network_context.error());
    const MemState &mem = process_network_context->thread->cpu->mem;
    if (name && !is_valid_addr_len(mem, name.address(), sizeof(SceNetSockaddr)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    if (namelen && !is_valid_addr_len(mem, namelen.address(), sizeof(unsigned int)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetGetpeername, sid, name.get(mem), namelen.get(mem)));
}

EXPORT(int, sceNetSyscallGetsockname, int sid, Ptr<SceNetSockaddr> name, Ptr<unsigned int> namelen) {
    const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name);
    if (!process_network_context)
        return NET_SYSCALL_RESULT(process_network_context.error());
    const MemState &mem = process_network_context->thread->cpu->mem;
    if (name && !is_valid_addr_len(mem, name.address(), sizeof(SceNetSockaddr)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    if (namelen && !is_valid_addr_len(mem, namelen.address(), sizeof(unsigned int)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetGetsockname, sid, name.get(mem), namelen.get(mem)));
}

EXPORT(int, sceNetSyscallGetsockopt, Ptr<SceNetSyscallGetsockoptParam> param) {
    if (!param) {
        const auto result = NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
        LOG_INFO_IF(log_enabled, "sceNetSyscallGetsockopt param=<null> result={}", log_hex(result));
        return result;
    }
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        const auto result = NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
        LOG_INFO_IF(log_enabled, "sceNetSyscallGetsockopt thread unavailable result={}", log_hex(result));
        return result;
    }
    MemState &mem = thread->cpu->mem;
    if (!is_valid_addr_len(mem, param.address(), sizeof(SceNetSyscallGetsockoptParam)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    const auto *arguments = param.get(mem);
    if (!arguments)
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    uint32_t optlen_before = 0;
    if (arguments->optlen && (!is_valid_addr_len(mem, arguments->optlen.address(), sizeof(optlen_before)) || !read_guest_memory(mem, arguments->optlen.address(), &optlen_before, sizeof(optlen_before))))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    const bool optlen_readable = static_cast<bool>(arguments->optlen);
    if (arguments->optval && optlen_readable && !is_valid_addr_len(mem, arguments->optval.address(), optlen_before))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    const auto result = NET_SYSCALL_RESULT(CALL_EXPORT(sceNetGetsockopt, arguments->sid, arguments->level, arguments->optname,
        arguments->optval.get(mem), arguments->optlen.get(mem)));

    uint32_t optlen_after = 0;
    const bool optlen_after_readable = arguments->optlen && read_guest_memory(mem, arguments->optlen.address(), &optlen_after, sizeof(optlen_after));
    int optval_word = 0;
    std::string optval_log;
    if (!arguments->optval)
        optval_log = "<null>";
    else if (!optlen_readable)
        optval_log = "<unreadable optlen>";
    else if (result < 0)
        optval_log = "<not returned>";
    else if (!optlen_after_readable || optlen_after < sizeof(optval_word))
        optval_log = "<short>";
    else if (read_guest_memory(mem, arguments->optval.address(), &optval_word, sizeof(optval_word)))
        optval_log = log_hex(optval_word);
    else
        optval_log = "<unreadable>";

    LOG_INFO_IF(log_enabled, "sceNetSyscallGetsockopt sid={} level={} optname={} optval[0:4]={} optlen={} result={}",
        arguments->sid, log_hex(static_cast<int>(arguments->level)), log_hex(static_cast<int>(arguments->optname)),
        optval_log, optlen_after_readable ? std::to_string(optlen_after) : "<unreadable>", log_hex(result));
    return result;
}

EXPORT(int, sceNetSyscallIcmConnect, int socket, int flags) {
    TRACY_FUNC(sceNetSyscallIcmConnect, socket, flags);
    const auto caller_process_id = emuenv.kernel.get_thread_process_id(thread_id);
    const auto caller_pid = caller_process_id.value_or(-1);
    LOG_INFO_IF(log_enabled, "sceNetSyscallIcmConnect called pid={} socket={} flags={}", caller_pid, socket, log_hex(flags));

    const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name);
    if (!process_network_context)
        return NET_SYSCALL_RESULT(process_network_context.error());

    auto &net = *process_network_context->net;
    {
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        if (net.socks.find(socket) == net.socks.end())
            return NET_SYSCALL_RESULT(-SCE_NET_EBADF);
    }

    const bool ethernet_mode = emuenv.cfg.current_config.pstv_mode;
    const auto host_address = get_selected_host_address(
        emuenv.cfg.ethernet_addr, emuenv.cfg.wifi_addr, ethernet_mode);
    const char *interface_name = ethernet_mode ? "eth0" : "wlan0";
    const bool host_context_initialized = initialize_host_network_context(
        net, emuenv.kernel.mutex, host_address, interface_name);

    std::unique_lock<std::mutex> lock(emuenv.kernel.mutex);

    const auto socket_entry = net.socks.find(socket);
    if (socket_entry == net.socks.end())
        return NET_SYSCALL_RESULT(-SCE_NET_EBADF);
    const auto &sock = socket_entry->second;

    if (!host_context_initialized)
        return NET_SYSCALL_RESULT(-SCE_NET_ENETDOWN);
    net.refresh_connection_state();
    const auto host_address_text = host_address ? host_address->addr : std::string("unavailable");
    LOG_DEBUG_IF(log_enabled, "sceNetSyscallIcmConnect host_context={} interface={} address={} state={} flags={} dns_initialized={}",
        host_context_initialized, interface_name, host_address_text, net.connection_state,
        log_hex(static_cast<int>(net.interface_flags)), net.dns_config_initialized);
    const auto connection_state = net.connection_state;
    const bool may_request_auto_connect = !(sock->sce_options & (SCE_NET_SO_OPTION_10000 | SCE_NET_SO_OPTION_20000)) && (connection_state == 1 || connection_state == 4 || (connection_state == 0 && net.auto_connect_policy == -1));
    bool auto_connect_requested = false;

    auto selected_driver = net.driver_contexts.find(ethernet_mode
            ? NETPS_INTERFACE_INDEX_ETHERNET
            : NETPS_INTERFACE_INDEX_WIFI);
    if (selected_driver != net.driver_contexts.end() && std::none_of(selected_driver->second.callbacks.begin(), selected_driver->second.callbacks.end(), [](const NetDriverCallback &callback) { return callback.callback_id != 0; }))
        selected_driver = net.driver_contexts.end();

    if (may_request_auto_connect && selected_driver != net.driver_contexts.end()) {
        auto_connect_requested = true;
        sock->sce_icm_flags = (sock->sce_icm_flags & ~SCE_NETPS_SOCKET_CONNECTED) | SCE_NETPS_SOCKET_CONNECTING;
        sock->sce_icm_error = 0;
        sock->sce_icm_driver_index = selected_driver->first;
        const uint64_t event = connection_state == 0 ? 0x08000000ull : 0x01000000ull;
        for (auto &callback : selected_driver->second.callbacks) {
            if (callback.callback_id != 0)
                callback.pending_events |= event;
        }

        if (flags & SCE_NET_MSG_DONTWAIT)
            return NET_SYSCALL_RESULT(0);

        const auto connected_or_failed = [&net, &sock] {
            if (net.connection_state == 3)
                return true;
            const auto driver = net.driver_contexts.find(sock->sce_icm_driver_index.load());
            return driver != net.driver_contexts.end() && driver->second.last_error != 0;
        };

        bool completed = false;
        if (net.connect_timeout_seconds < 0) {
            net.auto_connect_cond_var.wait(lock, connected_or_failed);
            completed = true;
        } else {
            completed = net.auto_connect_cond_var.wait_for(
                lock,
                std::chrono::seconds(net.connect_timeout_seconds), connected_or_failed);
        }

        if (!completed) {
            sock->sce_icm_flags &= ~SCE_NETPS_SOCKET_CONNECTING;
            sock->sce_icm_driver_index = -1;
            return NET_SYSCALL_RESULT(-SCE_NET_ETIMEDOUT);
        }
        const auto driver_error = net.driver_contexts.find(sock->sce_icm_driver_index.load());
        if (driver_error != net.driver_contexts.end() && driver_error->second.last_error != 0) {
            sock->sce_icm_flags &= ~SCE_NETPS_SOCKET_CONNECTING;
            sock->sce_icm_error = driver_error->second.last_error;
            sock->sce_icm_driver_index = -1;
            return NET_SYSCALL_RESULT(driver_error->second.last_error);
        }
        net.refresh_connection_state();
        if (net.connection_state != 3) {
            sock->sce_icm_flags &= ~SCE_NETPS_SOCKET_CONNECTING;
            sock->sce_icm_driver_index = -1;
            return NET_SYSCALL_RESULT(-SCE_NET_ENETDOWN);
        }
    }

    sock->sce_icm_flags = (sock->sce_icm_flags & ~SCE_NETPS_SOCKET_CONNECTING) | SCE_NETPS_SOCKET_CONNECTED;
    sock->sce_icm_error = 0;
    sock->sce_icm_driver_index = -1;
    if (!auto_connect_requested)
        wakeup_epolls(net, sock);

    return NET_SYSCALL_RESULT(0);
}

EXPORT(int, sceNetSyscallIoctl, int sid, uint32_t command, Ptr<void> data) {
    TRACY_FUNC(sceNetSyscallIoctl, sid, command, data);
    LOG_INFO_IF(log_enabled, "sceNetSyscallIoctl called sid={} request={} ({}) data={}", sid,
        to_debug_str(emuenv.mem, static_cast<SceNetPsIoctlCommand>(command)), log_hex(command), log_hex(data.address()));
    if (!data)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

    const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name);
    if (!process_network_context)
        return NET_SYSCALL_RESULT(process_network_context.error());

    const MemState &net_mem = process_network_context->thread->cpu->mem;
    const uint32_t data_size = (command >> 16) & 0x1FFF;
    if (!is_valid_addr_len(net_mem, data.address(), data_size))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    void *ioctl_data = data.get(net_mem);
    if (!ioctl_data)
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);

    auto sock = lock_and_find(sid, process_network_context->net->socks, emuenv.kernel.mutex);
    if (!sock)
        return NET_SYSCALL_RESULT(-SCE_NET_EBADF);

    const auto is_virtual_interface = [&](const char *name) {
        if (std::strncmp(name, "eth0", 16) == 0 || std::strncmp(name, "wlan0", 16) == 0 || std::strncmp(name, "lo0", 16) == 0)
            return true;

        char requested_name[17]{};
        std::memcpy(requested_name, name, 16);
        LOG_WARN("sceNetSyscallIoctl: virtual interface does not match request '{}'; expected lo0, eth0, or wlan0",
            requested_name);
        return false;
    };

    switch (command) {
    case SCE_NETPS_IOCTL_GET_IF_FLAGS:
    case SCE_NETPS_IOCTL_SET_IF_FLAGS: {
        auto *request = static_cast<SceNetPsIfreq *>(ioctl_data);
        if (!is_virtual_interface(request->name))
            return NET_SYSCALL_RESULT(-SCE_NET_ENXIO);
        const bool loopback = std::strncmp(request->name, "lo0", 16) == 0;
        const bool ethernet_interface = std::strncmp(request->name, "eth0", 16) == 0;
        const size_t host_flags_index = ethernet_interface ? 0 : 1;
        const bool active_interface = ethernet_interface == emuenv.cfg.current_config.pstv_mode;
        if (!loopback && active_interface) {
            const auto host_address = get_selected_host_address(
                emuenv.cfg.ethernet_addr, emuenv.cfg.wifi_addr, ethernet_interface);
            initialize_host_network_context(*process_network_context->net, emuenv.kernel.mutex,
                host_address, request->name);
        }

        switch (command) {
        case SCE_NETPS_IOCTL_GET_IF_FLAGS: {
            const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
            request->value.flags = loopback
                ? process_network_context->net->loopback_interface_flags
                : process_network_context->net->host_interface_flags[host_flags_index];
            LOG_INFO_IF(log_enabled, "sceNetSyscallIoctl get interface flags sid={} interface={} flags={}",
                sid, request->name, log_hex(static_cast<int>(request->value.flags)));
            return NET_SYSCALL_RESULT(0);
        }
        case SCE_NETPS_IOCTL_SET_IF_FLAGS: {
            auto &flags = loopback
                ? process_network_context->net->loopback_interface_flags
                : process_network_context->net->host_interface_flags[host_flags_index];
            const uint32_t requested_flags = request->value.flags;
            uint32_t stored_flags = 0;
            {
                const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
                flags = (flags & 0x00018F52) | (request->value.flags & 0xFFFE70AD);
                stored_flags = flags;
                if (!loopback && active_interface) {
                    auto &net = *process_network_context->net;
                    net.interface_flags = flags;
                    net.refresh_connection_state();
                    net.set_interface_active(net.host_link_up && (net.interface_flags & 0x41) == 0x41);
                    update_pending_icm_connects(net);
                }
            }
            LOG_INFO_IF(log_enabled, "sceNetSyscallIoctl set interface flags sid={} interface={} requested={} stored={}",
                sid, request->name, log_hex(static_cast<int>(requested_flags)), log_hex(static_cast<int>(stored_flags)));
            return NET_SYSCALL_RESULT(0);
        }
        default:
            return NET_SYSCALL_RESULT(-SCE_NET_EOPNOTSUPP);
        }
    }
    case SCE_NETPS_IOCTL_SET_IF_ADDRESS: {
        const auto *request = static_cast<const SceNetPsIfreq *>(ioctl_data);
        if (!is_virtual_interface(request->name))
            return NET_SYSCALL_RESULT(-SCE_NET_ENXIO);
        if (request->value.address.sin_family != 0 && request->value.address.sin_family != AF_INET)
            return NET_SYSCALL_RESULT(-SCE_NET_EAFNOSUPPORT);
        if (std::strncmp(request->name, "lo0", 16) == 0)
            return NET_SYSCALL_RESULT(0);

        auto &net = *process_network_context->net;
        const bool ethernet_mode = emuenv.cfg.current_config.pstv_mode;
        const auto host_address = get_selected_host_address(
            emuenv.cfg.ethernet_addr, emuenv.cfg.wifi_addr, ethernet_mode);
        const char *interface_name = ethernet_mode ? "eth0" : "wlan0";
        initialize_host_network_context(net, emuenv.kernel.mutex, host_address, interface_name);
        const uint32_t requested_address = request->value.address.sin_family == AF_INET
            ? request->value.address.sin_addr.s_addr
            : 0;
        {
            const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
            net.netAddr = requested_address;
            if (requested_address == 0) {
                net.netMask = 0;
                net.broadcastAddr = 0;
            } else if (net.netMask != 0) {
                net.broadcastAddr = htonl(ntohl(net.netAddr) | ~ntohl(net.netMask));
            }
            net.interface_address_configured = true;
            net.refresh_connection_state();
            update_pending_icm_connects(net);
        }
        LOG_INFO_IF(log_enabled, "sceNetSyscallIoctl set interface address sid={} interface={} family={} address={}",
            sid, request->name, request->value.address.sin_family,
            log_hex(static_cast<int>(requested_address)));
        return NET_SYSCALL_RESULT(0);
    }
    case SCE_NETPS_IOCTL_ADD_IF_ALIAS: {
        const auto *request = static_cast<const SceNetPsIfAliasReq *>(ioctl_data);
        if (!is_virtual_interface(request->name))
            return NET_SYSCALL_RESULT(-SCE_NET_ENXIO);
        if (request->address.sin_family != AF_INET)
            return NET_SYSCALL_RESULT(-SCE_NET_EAFNOSUPPORT);
        if (request->netmask.sin_family != 0 && request->netmask.sin_family != AF_INET)
            return NET_SYSCALL_RESULT(-SCE_NET_EAFNOSUPPORT);
        if (std::strncmp(request->name, "lo0", 16) == 0) {
            if (request->address.sin_addr.s_addr != htonl(0x7F000001))
                return NET_SYSCALL_RESULT(-SCE_NET_EADDRNOTAVAIL);
            return NET_SYSCALL_RESULT(0);
        }

        auto &net = *process_network_context->net;
        const bool ethernet_mode = emuenv.cfg.current_config.pstv_mode;
        const auto host_address = get_selected_host_address(
            emuenv.cfg.ethernet_addr, emuenv.cfg.wifi_addr, ethernet_mode);
        const char *interface_name = ethernet_mode ? "eth0" : "wlan0";
        initialize_host_network_context(net, emuenv.kernel.mutex, host_address, interface_name);
        const uint32_t alias_broadcast_address = request->broadcast_address.sin_family == AF_INET && request->broadcast_address.sin_addr.s_addr != 0
            ? request->broadcast_address.sin_addr.s_addr
            : htonl(ntohl(request->address.sin_addr.s_addr) | ~ntohl(request->netmask.sin_addr.s_addr));
        {
            const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
            net.interface_alias_address = request->address.sin_addr.s_addr;
            net.interface_alias_netmask = request->netmask.sin_addr.s_addr;
            net.interface_alias_broadcast_address = alias_broadcast_address;
            net.interface_alias_configured = true;
            net.refresh_connection_state();
            update_pending_icm_connects(net);
        }
        return NET_SYSCALL_RESULT(0);
    }
    case SCE_NETPS_IOCTL_SET_P2P_KEY: {
        if (sock->sce_type != SCE_NET_SOCK_DGRAM_P2P && sock->sce_type != SCE_NET_SOCK_STREAM_P2P)
            return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

        const auto *request = static_cast<const SceNetSyscallP2pKeyRequest *>(ioctl_data);
        auto &net = *process_network_context->net;
        std::array<uint8_t, 16> key{};
        std::copy_n(request->key, key.size(), key.begin());
        {
            const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
            net.p2p_key = key;
            net.p2p_key_set = true;

            auto [peer, inserted] = net.p2p_peers.try_emplace({ INADDR_LOOPBACK, SCE_NET_ADHOC_PORT });
            auto &self_peer = peer->second;
            if (!inserted && self_peer.key == key) {
                ++self_peer.references;
            } else {
                self_peer.key = key;
                self_peer.local_port = SCE_NET_ADHOC_PORT;
                self_peer.references = 1;
                self_peer.group = 0;
                self_peer.broadcast = false;
            }
        }
        return NET_SYSCALL_RESULT(0);
    }
    default:
        LOG_WARN("sceNetSyscallIoctl: unsupported socket {}, request={} ({})", sid,
            to_debug_str(emuenv.mem, static_cast<SceNetPsIoctlCommand>(command)), log_hex(command));
        return NET_SYSCALL_RESULT(-SCE_NET_EOPNOTSUPP);
    }
}

EXPORT(int, sceNetSyscallListen, int sid, int backlog) {
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetListen, sid, backlog));
}

EXPORT(int, sceNetSyscallRecvfrom, Ptr<SceNetSyscallRecvfromParam> param) {
    if (!param)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    MemState &mem = thread->cpu->mem;
    if (!is_valid_addr_len(mem, param.address(), sizeof(SceNetSyscallRecvfromParam)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    const auto *arguments = param.get(mem);
    if (!arguments)
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    if (arguments->buf && !is_valid_addr_len(mem, arguments->buf.address(), arguments->len))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    unsigned int from_capacity = 0;
    if (arguments->fromlen && (!is_valid_addr_len(mem, arguments->fromlen.address(), sizeof(from_capacity)) || !read_guest_memory(mem, arguments->fromlen.address(), &from_capacity, sizeof(from_capacity))))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    if (arguments->from && arguments->fromlen && !is_valid_addr_len(mem, arguments->from.address(), from_capacity))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetRecvfrom, arguments->sid, arguments->buf.get(mem), arguments->len,
        arguments->flags, arguments->from.get(mem), arguments->fromlen.get(mem)));
}

EXPORT(int, sceNetSyscallRecvmsg, int sid, Ptr<SceNetMsghdr> msg, int flags) {
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    const MemState &mem = thread->cpu->mem;
    if (msg && !is_valid_addr_len(mem, msg.address(), sizeof(SceNetMsghdr)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetRecvmsg, sid, msg.get(mem), flags));
}

EXPORT(int, sceNetSyscallSendmsg, int sid, Ptr<const SceNetMsghdr> msg, int flags) {
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    const MemState &mem = thread->cpu->mem;
    if (msg && !is_valid_addr_len(mem, msg.address(), sizeof(SceNetMsghdr)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetSendmsg, sid, msg.get(mem), flags));
}

EXPORT(int, sceNetSyscallSendto, Ptr<SceNetSyscallSendtoParam> param) {
    if (!param)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    const MemState &mem = thread->cpu->mem;
    if (!is_valid_addr_len(mem, param.address(), sizeof(SceNetSyscallSendtoParam)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    const auto *arguments = param.get(mem);
    if (!arguments)
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    if (arguments->msg && !is_valid_addr_len(mem, arguments->msg.address(), arguments->len))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    if (arguments->to && !is_valid_addr_len(mem, arguments->to.address(), arguments->tolen))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetSendto, arguments->sid, arguments->msg.get(mem), arguments->len,
        arguments->flags, arguments->to.get(mem), arguments->tolen));
}

EXPORT(int, sceNetSyscallSetsockopt, Ptr<SceNetSyscallSetsockoptParam> param) {
    if (!param) {
        const auto result = NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
        LOG_INFO_IF(log_enabled, "sceNetSyscallSetsockopt param=<null> result={}", log_hex(result));
        return result;
    }
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        const auto result = NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
        LOG_INFO_IF(log_enabled, "sceNetSyscallSetsockopt thread unavailable result={}", log_hex(result));
        return result;
    }
    MemState &mem = thread->cpu->mem;
    if (!is_valid_addr_len(mem, param.address(), sizeof(SceNetSyscallSetsockoptParam)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    const auto *arguments = param.get(mem);
    if (!arguments)
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    if (arguments->optval && !is_valid_addr_len(mem, arguments->optval.address(), arguments->optlen))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    int optval_word = 0;
    std::string optval_log;
    if (!arguments->optval)
        optval_log = "<null>";
    else if (arguments->optlen < sizeof(optval_word))
        optval_log = "<short>";
    else if (read_guest_memory(mem, arguments->optval.address(), &optval_word, sizeof(optval_word)))
        optval_log = log_hex(optval_word);
    else
        optval_log = "<unreadable>";

    const auto result = NET_SYSCALL_RESULT(CALL_EXPORT(sceNetSetsockopt, arguments->sid, arguments->level, arguments->optname,
        arguments->optval.get(mem), arguments->optlen));
    LOG_INFO_IF(log_enabled, "sceNetSyscallSetsockopt sid={} level={} optname={} optval[0:4]={} optlen={} result={}",
        arguments->sid, log_hex(static_cast<int>(arguments->level)), log_hex(static_cast<int>(arguments->optname)),
        optval_log, arguments->optlen, log_hex(result));
    return result;
}

EXPORT(int, sceNetSyscallShutdown, int sid, int how) {
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetShutdown, sid, how));
}

EXPORT(int, sceNetSyscallSocket, Ptr<const char> name, int domain, SceNetSocketType type, SceNetProtocol protocol) {
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);
    if (name && !is_valid_addr_len(thread->cpu->mem, name.address(), 1))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    const auto result = NET_SYSCALL_RESULT(CALL_EXPORT(sceNetSocket, name.get(thread->cpu->mem), domain, type, protocol));
    LOG_INFO_IF(log_enabled, "sceNetSyscallSocket domain={} type={} protocol={} result={}",
        domain, static_cast<int>(type), static_cast<int>(protocol), log_hex(result));
    return result;
}

EXPORT(int, sceNetSyscallSocketAbort, int sid, int flags) {
    return NET_SYSCALL_RESULT(CALL_EXPORT(sceNetSocketAbort, sid, flags));
}

EXPORT(int, sceNetSyscallSysctl, Ptr<SceNetSyscallSysctlParam> param) {
    if (!param)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

    MemState &mem = thread->cpu->mem;
    if (!is_valid_addr_len(mem, param.address(), sizeof(SceNetSyscallSysctlParam)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    const auto *guest_arguments = param.get(mem);
    if (!guest_arguments)
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    const auto arguments = *guest_arguments;
    unsigned int old_length = 0;
    if (arguments.old_length && !read_guest_memory(mem, arguments.old_length.address(), &old_length, sizeof(old_length)))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);
    const unsigned int old_capacity = old_length;

    if (arguments.name_length < 2 || arguments.name_length > 12)
        return NET_SYSCALL_RESULT(-SCE_NET_EINVAL);

    std::array<int, 12> identifiers{};
    const auto name_size = arguments.name_length * sizeof(identifiers[0]);
    if (!arguments.name || !read_guest_memory(mem, arguments.name.address(), identifiers.data(), name_size))
        return NET_SYSCALL_RESULT(-SCE_NET_EFAULT);

    std::string mib;
    for (unsigned int i = 0; i < arguments.name_length; ++i) {
        if (i)
            mib += ',';
        mib += std::to_string(identifiers[i]);
    }

    int result = 0;
    if (identifiers[0] != 4) {
        result = -SCE_NET_EOPNOTSUPP;
    } else if (arguments.name_length == 2 || (identifiers[1] != 0 && arguments.name_length == 3)) {
        result = -SCE_NET_ENOTDIR;
    } else if (identifiers[1] != 0) {
        if (identifiers[1] != 17) {
            result = -SCE_NET_ENOPROTOOPT;
        } else if (arguments.name_length != 6 || identifiers[2] != 0 || identifiers[3] != SCE_NET_AF_INET || identifiers[4] != 1 || arguments.new_data) {
            result = -SCE_NET_EOPNOTSUPP;
        } else {
            const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name);
            if (!process_network_context) {
                result = process_network_context.error();
            } else {
                const bool ethernet_mode = emuenv.cfg.current_config.pstv_mode;
                const auto host_address = get_selected_host_address(
                    emuenv.cfg.ethernet_addr, emuenv.cfg.wifi_addr, ethernet_mode);
                const char *interface_name = ethernet_mode ? "eth0" : "wlan0";
                initialize_host_network_context(*process_network_context->net, emuenv.kernel.mutex,
                    host_address, interface_name);
                std::vector<uint8_t> route_dump;
                if (!build_route_table_dump(*process_network_context->net, identifiers[3], identifiers[5], route_dump)) {
                    result = -SCE_NET_EOPNOTSUPP;
                } else {
                    old_length = static_cast<unsigned int>(route_dump.size());
                    if (arguments.old_length && !write_guest_memory(mem, arguments.old_length.address(), &old_length, sizeof(old_length))) {
                        result = -SCE_NET_EFAULT;
                    } else if (arguments.old_data && old_capacity < old_length) {
                        result = -SCE_NET_ENOMEM;
                    } else if (arguments.old_data && old_length > 0 && !write_guest_memory(mem, arguments.old_data.address(), route_dump.data(), old_length)) {
                        result = -SCE_NET_EFAULT;
                    }
                }
            }
        }
    } else if (arguments.old_length && !write_guest_memory(mem, arguments.old_length.address(), &old_length, sizeof(old_length))) {
        result = -SCE_NET_EFAULT;
    }

    LOG_INFO_IF(log_enabled, "sceNetSyscallSysctl mib=[{}] old_length={} new_length={} result={}", mib,
        arguments.old_length ? std::to_string(old_length) : "<null>", arguments.new_length, log_hex(result));
    return NET_SYSCALL_RESULT(result);
}
