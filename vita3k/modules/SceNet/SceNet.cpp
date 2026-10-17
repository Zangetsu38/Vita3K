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

#include "SceNet.h"

#include <../SceAppMgr/SceAppMgr.h>

#include <cpu/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>

#include <net/state.h>

#include <util/lock_and_find.h>
#include <util/net_utils.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include "macos_net_helper.h"
#include <net/if.h>
#endif

#include <util/tracy.h>
TRACY_MODULE_NAME(SceNet);

template <>
std::string to_debug_str<SceNetEpollControlFlag>(const MemState &mem, SceNetEpollControlFlag type) {
    switch (type) {
    case SCE_NET_EPOLL_CTL_ADD:
        return "SCE_NET_EPOLL_CTL_ADD";
    case SCE_NET_EPOLL_CTL_MOD:
        return "SCE_NET_EPOLL_CTL_MOD";
    case SCE_NET_EPOLL_CTL_DEL:
        return "SCE_NET_EPOLL_CTL_DEL";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceNetProtocol>(const MemState &mem, SceNetProtocol type) {
    switch (type) {
    case SCE_NET_IPPROTO_IP: return "SCE_NET_IPPROTO_IP";
    case SCE_NET_IPPROTO_ICMP: return "SCE_NET_IPPROTO_ICMP";
    case SCE_NET_IPPROTO_IGMP: return "SCE_NET_IPPROTO_IGMP";
    case SCE_NET_IPPROTO_TCP: return "SCE_NET_IPPROTO_TCP";
    case SCE_NET_IPPROTO_UDP: return "SCE_NET_IPPROTO_UDP";
    case SCE_NET_SOL_SOCKET: return "SCE_NET_SOL_SOCKET";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceNetSocketType>(const MemState &mem, SceNetSocketType type) {
    switch (type) {
    case SCE_NET_SOCK_STREAM: return "SCE_NET_SOCK_STREAM";
    case SCE_NET_SOCK_DGRAM: return "SCE_NET_SOCK_DGRAM";
    case SCE_NET_SOCK_RAW: return "SCE_NET_SOCK_RAW";
    case SCE_NET_SOCK_DGRAM_P2P: return "SCE_NET_SOCK_DGRAM_P2P";
    case SCE_NET_SOCK_STREAM_P2P: return "SCE_NET_SOCK_STREAM_P2P";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceNetSocketOption>(const MemState &mem, SceNetSocketOption type) {
    switch (type) {
    /* IP */
    case SCE_NET_IP_HDRINCL: return "SCE_NET_IP_HDRINCL or SCE_NET_TCP_MAXSEG";
    case SCE_NET_IP_TOS: return "SCE_NET_IP_TOS or SCE_NET_TCP_MSS_TO_ADVERTISE";
    case SCE_NET_IP_TTL: return "SCE_NET_IP_TTL or SCE_NET_SO_REUSEADDR";
    case SCE_NET_IP_MULTICAST_IF: return "SCE_NET_IP_MULTICAST_IF";
    case SCE_NET_IP_MULTICAST_TTL: return "SCE_NET_IP_MULTICAST_TTL";
    case SCE_NET_IP_MULTICAST_LOOP: return "SCE_NET_IP_MULTICAST_LOOP";
    case SCE_NET_IP_ADD_MEMBERSHIP: return "SCE_NET_IP_ADD_MEMBERSHIP";
    case SCE_NET_IP_DROP_MEMBERSHIP: return "SCE_NET_IP_DROP_MEMBERSHIP";
    case SCE_NET_IP_TTLCHK: return "SCE_NET_IP_TTLCHK";
    case SCE_NET_IP_MAXTTL: return "SCE_NET_IP_MAXTTL";
    /* TCP */
    case SCE_NET_TCP_NODELAY: return "SCE_NET_TCP_NODELAY";
    // case SCE_NET_TCP_MAXSEG: return "SCE_NET_TCP_MAXSEG";
    // case SCE_NET_TCP_MSS_TO_ADVERTISE: return "SCE_NET_TCP_MSS_TO_ADVERTISE";
    /* SOCKET */
    // case SCE_NET_SO_REUSEADDR: return "SCE_NET_SO_REUSEADDR";
    case SCE_NET_SO_KEEPALIVE: return "SCE_NET_SO_KEEPALIVE";
    case SCE_NET_SO_BROADCAST: return "SCE_NET_SO_BROADCAST";
    case SCE_NET_SO_LINGER: return "SCE_NET_SO_LINGER";
    case SCE_NET_SO_OOBINLINE: return "SCE_NET_SO_OOBINLINE";
    case SCE_NET_SO_REUSEPORT: return "SCE_NET_SO_REUSEPORT";
    case SCE_NET_SO_ONESBCAST: return "SCE_NET_SO_ONESBCAST";
    case SCE_NET_SO_USECRYPTO: return "SCE_NET_SO_USECRYPTO";
    case SCE_NET_SO_USESIGNATURE: return "SCE_NET_SO_USESIGNATURE";
    case SCE_NET_SO_OPTION_10000: return "SCE_NET_SO_OPTION_10000";
    case SCE_NET_SO_OPTION_20000: return "SCE_NET_SO_OPTION_20000";
    case SCE_NET_SO_SNDBUF: return "SCE_NET_SO_SNDBUF";
    case SCE_NET_SO_RCVBUF: return "SCE_NET_SO_RCVBUF";
    case SCE_NET_SO_SNDLOWAT: return "SCE_NET_SO_SNDLOWAT";
    case SCE_NET_SO_RCVLOWAT: return "SCE_NET_SO_RCVLOWAT";
    case SCE_NET_SO_SNDTIMEO: return "SCE_NET_SO_SNDTIMEO";
    case SCE_NET_SO_RCVTIMEO: return "SCE_NET_SO_RCVTIMEO";
    case SCE_NET_SO_ERROR: return "SCE_NET_SO_ERROR";
    case SCE_NET_SO_TYPE: return "SCE_NET_SO_TYPE";
    case SCE_NET_SO_NBIO: return "SCE_NET_SO_NBIO";
    case SCE_NET_SO_TPPOLICY: return "SCE_NET_SO_TPPOLICY";
    case SCE_NET_SO_NAME: return "SCE_NET_SO_NAME";
    }
    return std::to_string(type);
}

int ret_net_errno(EmuEnvState &emuenv, int thread_id, int ret) {
    if (ret < 0) {
        const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
        if (!thread) {
            LOG_ERROR("Cannot set network errno for unknown thread {}", thread_id);
            return ret;
        }
        auto addr = emuenv.kernel.get_thread_tls_addr(thread->cpu->mem, thread_id, TLS_NET_ERRNO);
        if (addr)
            *addr.cast<int>().get(thread->cpu->mem) = ret & 0xff;
    }

    return ret;
}

#define RET_NET_ERRNO(ret)                                \
    do {                                                  \
        int _r = ret_net_errno(emuenv, thread_id, (ret)); \
        return (_r < 0 ? RET_ERROR(_r) : _r);             \
    } while (0)

#define GET_PROCESS_NET_STATE()                                                                       \
    const auto process_network_context = get_process_network_context(emuenv, thread_id, export_name); \
    if (!process_network_context)                                                                     \
        RET_NET_ERRNO(SCE_NET_ERROR_ENOTINIT);                                                        \
    NetState &process_net = *process_network_context->net

static bool is_packet_socket(const Socket &socket) {
    return socket.sce_type != SCE_NET_SOCK_STREAM && socket.sce_type != SCE_NET_SOCK_STREAM_P2P;
}

EXPORT(int, sceNetAccept, int sid, SceNetSockaddr *addr, unsigned int *addrlen) {
    TRACY_FUNC(sceNetAccept, sid, addr, addrlen);
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);
    if (!sock)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);

    int err = 0;
    auto newsock = [&] {
        ThreadHostWait host_wait(emuenv.kernel.get_thread(thread_id));
        return sock->accept(addr, addrlen, err);
    }();
    if (!newsock)
        RET_NET_ERRNO(err);

    auto id = ++process_net.next_id;
    process_net.socks.emplace(id, newsock);
    return id;
}

EXPORT(int, sceNetBind, int sid, const SceNetSockaddr *addr, unsigned int addrlen) {
    TRACY_FUNC(sceNetBind, sid, addr, addrlen);
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);

    RET_NET_ERRNO(sock ? sock->bind(addr, addrlen) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetClearDnsCache) {
    TRACY_FUNC(sceNetClearDnsCache);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetConnect, int sid, const SceNetSockaddr *addr, unsigned int addrlen) {
    TRACY_FUNC(sceNetConnect, sid, addr, addrlen);
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);

    if (!sock)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
    ThreadHostWait host_wait(emuenv.kernel.get_thread(thread_id));
    RET_NET_ERRNO(sock->connect(addr, addrlen));
}

EXPORT(int, sceNetDumpAbort) {
    TRACY_FUNC(sceNetDumpAbort);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetDumpCreate) {
    TRACY_FUNC(sceNetDumpCreate);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetDumpDestroy) {
    TRACY_FUNC(sceNetDumpDestroy);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetDumpRead) {
    TRACY_FUNC(sceNetDumpRead);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetEmulationGet) {
    TRACY_FUNC(sceNetEmulationGet);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetEmulationSet) {
    TRACY_FUNC(sceNetEmulationSet);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetEpollAbort) {
    TRACY_FUNC(sceNetEpollAbort);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetEpollControl, int eid, SceNetEpollControlFlag op, int id, SceNetEpollEvent *ev) {
    TRACY_FUNC(sceNetEpollControl, eid, op, id, ev);
    GET_PROCESS_NET_STATE();
    auto epoll = lock_and_find(eid, process_net.epolls, emuenv.kernel.mutex);
    if (!epoll)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);

    if (id == process_net.resolver_id) {
        STUBBED("Async DNS resolve is not supported");
        return 0;
    }

    switch (op) {
    case SCE_NET_EPOLL_CTL_ADD: {
        const auto sock = lock_and_find(id, process_net.socks, emuenv.kernel.mutex);
        RET_NET_ERRNO(sock ? epoll->add(id, sock, ev) : SCE_NET_ERROR_EBADF);
    }
    case SCE_NET_EPOLL_CTL_DEL:
        RET_NET_ERRNO(epoll->del(id));
    case SCE_NET_EPOLL_CTL_MOD:
        RET_NET_ERRNO(epoll->mod(id, ev));
    default:
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    }
}

EXPORT(int, sceNetEpollCreate, const char *name, int flags) {
    TRACY_FUNC(sceNetEpollCreate, name, flags);
    GET_PROCESS_NET_STATE();
    auto id = ++process_net.next_epoll_id;
    auto epoll = std::make_shared<Epoll>();
    const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
    process_net.epolls.emplace(id, epoll);
    return id;
}

EXPORT(int, sceNetEpollDestroy, int eid) {
    TRACY_FUNC(sceNetEpollDestroy, eid);
    GET_PROCESS_NET_STATE();
    const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);

    RET_NET_ERRNO(process_net.epolls.erase(eid) ? 0 : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetEpollWait, int eid, SceNetEpollEvent *events, int maxevents, int timeout) {
    TRACY_FUNC(sceNetEpollWait, eid, events, maxevents, timeout);
    GET_PROCESS_NET_STATE();
    auto epoll = lock_and_find(eid, process_net.epolls, emuenv.kernel.mutex);

    RET_NET_ERRNO(epoll ? epoll->wait(events, maxevents, timeout) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetEpollWaitCB) {
    TRACY_FUNC(sceNetEpollWaitCB);
    return UNIMPLEMENTED();
}

EXPORT(Ptr<int>, sceNetErrnoLoc) {
    TRACY_FUNC(sceNetErrnoLoc);
    // TLS id was taken from disasm source
    auto addr = emuenv.kernel.get_thread_tls_addr(thread_id, TLS_NET_ERRNO);
    return addr.cast<int>();
}

EXPORT(int, sceNetEtherNtostr, SceNetEtherAddr *n, char *str, unsigned int len) {
    TRACY_FUNC(sceNetEtherNtostr, n, str, len);
    GET_PROCESS_NET_STATE();
    if (!process_net.inited)
        RET_NET_ERRNO(SCE_NET_ERROR_ENOTINIT);

    if (!n || !str || len <= 0x11)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);

    snprintf(str, len, "%02x:%02x:%02x:%02x:%02x:%02x",
        n->data[0], n->data[1], n->data[2], n->data[3], n->data[4], n->data[5]);
    return 0;
}

EXPORT(int, sceNetEtherStrton, const char *str, SceNetEtherAddr *n) {
    TRACY_FUNC(sceNetEtherStrton, str, n);
    GET_PROCESS_NET_STATE();
    if (!process_net.inited)
        RET_NET_ERRNO(SCE_NET_ERROR_ENOTINIT);

    if (!str || !n)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);

    sscanf(str, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
        &n->data[0], &n->data[1], &n->data[2], &n->data[3], &n->data[4], &n->data[5]);

    return 0;
}

EXPORT(int, sceNetGetMacAddress, SceNetEtherAddr *addr, int flags) {
    TRACY_FUNC(sceNetGetMacAddress, addr, flags);
    if (addr == nullptr) {
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    }
    const auto selected_addr = net_utils::get_selected_assigned_addr(emuenv.cfg.adhoc_addr);
#ifdef _WIN32
    IP_ADAPTER_INFO AdapterInfo[16];
    DWORD dwBufLen = sizeof(AdapterInfo);
    if (GetAdaptersInfo(AdapterInfo, &dwBufLen) != ERROR_SUCCESS)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    for (auto adapter = AdapterInfo; adapter != nullptr; adapter = adapter->Next) {
        for (auto ip = &adapter->IpAddressList; ip != nullptr; ip = ip->Next) {
            if ((selected_addr.addr == ip->IpAddress.String) && (adapter->AddressLength == sizeof(addr->data))) {
                memcpy(addr->data, adapter->Address, sizeof(addr->data));
                return 0;
            }
        }
    }
    RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
#elif defined(__unix__)
    struct ifreq ifr{};

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1) {
        LOG_ERROR("Failed to open socket");
        assert(false);
        return RET_ERROR(SCE_NET_ERROR_ENOTSOCK);
    };

    struct SocketGuard {
        int fd;
        ~SocketGuard() {
            close(fd);
        }
    } socket_guard{ sock };

    if (selected_addr.name.empty() || selected_addr.name.size() >= sizeof(ifr.ifr_name))
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    memcpy(ifr.ifr_name, selected_addr.name.c_str(), selected_addr.name.size() + 1);

    if (ioctl(sock, SIOCGIFFLAGS, &ifr) == -1) {
        LOG_ERROR("Failed to fetch flags from socket {}, name={}", sock, ifr.ifr_name);
        assert(false);
        return RET_ERROR(SCE_NET_ERROR_EINTERNAL);
    }

    if (ifr.ifr_flags & IFF_LOOPBACK)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) == -1)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    memcpy(addr->data, ifr.ifr_hwaddr.sa_data, 6);
#elif defined(__APPLE__)
    char hint[IFNAMSIZ] = {};
    if (selected_addr.name.rfind("en", 0) != 0)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    strncpy(hint, selected_addr.name.c_str(), sizeof(hint) - 1);

    if (!get_mac_address(hint, addr->data))
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
#else
    return UNIMPLEMENTED();
#endif
    return 0;
}

EXPORT(int, sceNetGetSockIdInfo) {
    TRACY_FUNC(sceNetGetSockIdInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetGetSockInfo) {
    TRACY_FUNC(sceNetGetSockInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetGetStatisticsInfo) {
    TRACY_FUNC(sceNetGetStatisticsInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetGetpeername, int sid, SceNetSockaddr *addr, unsigned int *addrlen) {
    TRACY_FUNC(sceNetGetpeername, sid, addr, addrlen);
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);
    RET_NET_ERRNO(sock ? sock->get_peer_address(addr, addrlen) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetGetsockname, int sid, SceNetSockaddr *addr, unsigned int *addrlen) {
    TRACY_FUNC(sceNetGetsockname, sid, addr, addrlen);
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);

    RET_NET_ERRNO(sock ? sock->get_socket_address(addr, addrlen) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetGetsockopt, int sid, int level, int optname, void *optval, unsigned int *optlen) {
    TRACY_FUNC(sceNetGetsockopt, sid, level, optname, optval, optlen);
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);

    const int result = sock ? sock->get_socket_options(level, optname, optval, optlen) : SCE_NET_ERROR_EBADF;
    LOG_INFO("sceNetGetsockopt sid={} level={} optname={} result={}",
        sid, log_hex(level), log_hex(optname), log_hex(result));
    RET_NET_ERRNO(result);
}

EXPORT(SceUInt32, sceNetHtonl, SceUInt32 n) {
    TRACY_FUNC(sceNetHtonl, n);
    return htonl(n);
}

EXPORT(SceUInt64, sceNetHtonll, SceUInt64 n) {
    TRACY_FUNC(sceNetHtonll, n);
    return HTONLL(n);
}

EXPORT(SceUInt16, sceNetHtons, SceUInt16 n) {
    TRACY_FUNC(sceNetHtons, n);
    return htons(n);
}

EXPORT(Ptr<const char>, sceNetInetNtop, int af, const void *src, Ptr<char> dst, unsigned int size) {
    TRACY_FUNC(sceNetInetNtop, af, src, dst, size);
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        LOG_ERROR("sceNetInetNtop: unknown thread {}", thread_id);
        return Ptr<char>();
    }
    char *dst_ptr = dst.get(thread->cpu->mem);
#ifdef _WIN32
    const char *res = InetNtop(af, src, dst_ptr, size);
#else
    const char *res = inet_ntop(af, src, dst_ptr, size);
#endif
    if (res == nullptr) {
        ret_net_errno(emuenv, thread_id, SCE_NET_ERROR_EAFNOSUPPORT);
        return Ptr<char>();
    }
    return dst;
}

EXPORT(int, sceNetInetPton, int af, const char *src, void *dst) {
    TRACY_FUNC(sceNetInetPton, af, src, dst);

    if (af != SCE_NET_AF_INET)
        RET_NET_ERRNO(SCE_NET_ERROR_EAFNOSUPPORT);

#ifdef _WIN32
    int res = InetPton(af, src, dst);
#else
    int res = inet_pton(af, src, dst);
#endif

    RET_NET_ERRNO(res == 0 ? SCE_NET_ERROR_EINVAL : PosixSocket::translate_return_value(res));
}

EXPORT(int, sceNetInit, SceNetInitParam *param) {
    TRACY_FUNC(sceNetInit, param);
    GET_PROCESS_NET_STATE();
    if (process_net.inited)
        RET_NET_ERRNO(SCE_NET_ERROR_EBUSY);

    if (!param) {
        LOG_ERROR("sceNetInit rejected a null parameter block");
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    }
    if (!param->memory.address() || param->size < 0x4000 || param->flags != 0) {
        LOG_ERROR("sceNetInit rejected parameters: memory={}, size={}, flags={}",
            log_hex(param->memory.address()), param->size, log_hex(param->flags));
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    }

#ifdef _WIN32
    WORD versionWanted = MAKEWORD(2, 2);
    WSADATA wsaData;
    WSAStartup(versionWanted, &wsaData);
#endif
    process_net.state = 0;
    process_net.inited = true;
    process_net.resolver_id = ++process_net.next_id;
    process_net.host_link_up = net_utils::get_selected_assigned_addr(emuenv.cfg.adhoc_addr).link_up;
    net_utils::init_address(emuenv.cfg.adhoc_addr, process_net.netAddr, process_net.broadcastAddr, process_net.netMask);
    process_net.current_addr_index = emuenv.cfg.adhoc_addr;
    process_net.refresh_connection_state();
    return 0;
}

EXPORT(int, sceNetListen, int sid, int backlog) {
    TRACY_FUNC(sceNetListen, sid, backlog);
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);
    if (!sock) {
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
    }
    RET_NET_ERRNO(sock->listen(backlog));
}

EXPORT(SceUInt32, sceNetNtohl, SceUInt32 n) {
    TRACY_FUNC(sceNetNtohl, n);
    return ntohl(n);
}

EXPORT(SceUInt64, sceNetNtohll, SceUInt64 n) {
    TRACY_FUNC(sceNetNtohll, n);
    return NTOHLL(n);
}

EXPORT(SceUInt16, sceNetNtohs, SceUInt16 n) {
    TRACY_FUNC(sceNetNtohs, n);
    return ntohs(n);
}

EXPORT(int, sceNetRecv, int sid, void *buf, unsigned int len, int flags) {
    TRACY_FUNC(sceNetRecv, sid, buf, len, flags);
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);

    if (!sock)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
    const int received = sock->recv_packet(buf, len, flags, nullptr, 0);
    if (received >= 0)
        process_net.record_interface_receive(received, is_packet_socket(*sock));
    RET_NET_ERRNO(received);
}

EXPORT(int, sceNetRecvfrom, int sid, void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) {
    TRACY_FUNC(sceNetRecvfrom, sid, buf, len, flags, from, fromlen);
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);

    if (!sock)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
    const int received = sock->recv_packet(buf, len, flags, from, fromlen);
    if (received >= 0)
        process_net.record_interface_receive(received, is_packet_socket(*sock));
    RET_NET_ERRNO(received);
}

EXPORT(int, sceNetRecvmsg, int sid, SceNetMsghdr *msg, int flags) {
    TRACY_FUNC(sceNetRecvmsg, sid, msg, flags);
    GET_PROCESS_NET_STATE();
    const MemState &net_mem = process_network_context->thread->cpu->mem;
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);
    if (!sock)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
    if (!msg || msg->msg_iovlen < 0 || (msg->msg_iovlen > 0 && !msg->msg_iov))
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);

    const SceNetIovec *iov = msg->msg_iov.get(net_mem);
    size_t total_len = 0;
    for (int i = 0; i < msg->msg_iovlen; ++i) {
        if (iov[i].iov_len > std::numeric_limits<unsigned int>::max() - total_len)
            RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
        if (iov[i].iov_len > 0 && !iov[i].iov_base)
            RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
        total_len += iov[i].iov_len;
    }

    std::vector<char> buf(total_len);
    int received = 0;
    {
        ThreadHostWait host_wait(emuenv.kernel.get_thread(thread_id));
        received = sock->recv_packet(buf.data(), static_cast<unsigned int>(total_len), flags,
            msg->msg_name.get(net_mem), &msg->msg_namelen);
    }
    if (received < 0)
        RET_NET_ERRNO(received);
    process_net.record_interface_receive(received, is_packet_socket(*sock));

    size_t copied = 0;
    for (int i = 0; i < msg->msg_iovlen && copied < static_cast<size_t>(received); ++i) {
        const size_t copy_len = std::min<size_t>(iov[i].iov_len, static_cast<size_t>(received) - copied);
        if (copy_len > 0)
            std::memcpy(iov[i].iov_base.get(net_mem), buf.data() + copied, copy_len);
        copied += copy_len;
    }
    return received;
}

EXPORT(int, sceNetResolverAbort) {
    TRACY_FUNC(sceNetResolverAbort);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetResolverCreate, const char *name, void *param, int flags) {
    TRACY_FUNC(sceNetResolverCreate, name, param, flags);
    GET_PROCESS_NET_STATE();
    STUBBED("Fake id");
    return process_net.resolver_id;
}

EXPORT(int, sceNetResolverDestroy, int rid) {
    TRACY_FUNC(sceNetResolverDestroy, rid);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetResolverGetError) {
    TRACY_FUNC(sceNetResolverGetError);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetResolverStartAton, int rid, const SceNetInAddr *addr, char *hostname, int len, int timeout, int retry, int flags) {
    TRACY_FUNC(sceNetResolverStartAton, rid, addr, hostname, len, timeout, retry, flags);
    struct hostent *resolved = gethostbyaddr(reinterpret_cast<const char *>(addr), len, AF_INET);
    strcpy(hostname, resolved->h_name);
    return 0;
}

EXPORT(int, sceNetResolverStartNtoa, int rid, const char *hostname, SceNetInAddr *addr, int timeout, int retry, int flags) {
    TRACY_FUNC(sceNetResolverStartNtoa, rid, hostname, addr, timeout, retry, flags);
    struct hostent *resolved = gethostbyname(hostname);
    if (resolved == nullptr) {
        memset(addr, 0, sizeof(*addr));
        RET_NET_ERRNO(SCE_NET_ERROR_EHOSTUNREACH);
    }
    memcpy(addr, resolved->h_addr, sizeof(uint32_t));
    return 0;
}

EXPORT(int, sceNetSend, int sid, const void *msg, unsigned int len, int flags) {
    TRACY_FUNC(sceNetSend, sid, msg, len, flags);
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);

    if (!sock)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
    const int sent = sock->send_packet(msg, len, flags, nullptr, 0);
    if (sent >= 0)
        process_net.record_interface_transmit(sent, is_packet_socket(*sock));
    RET_NET_ERRNO(sent);
}

EXPORT(int, sceNetSendmsg, int sid, const SceNetMsghdr *msg, int flags) {
    TRACY_FUNC(sceNetSendmsg, sid, msg, flags);

    GET_PROCESS_NET_STATE();
    const MemState &net_mem = process_network_context->thread->cpu->mem;
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);
    if (!sock)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);

    size_t total_len = 0;
    for (int i = 0; i < msg->msg_iovlen; ++i) {
        const SceNetIovec &iov = msg->msg_iov.get(net_mem)[i];
        total_len += iov.iov_len;
    }

    std::vector<char> buf;
    buf.reserve(total_len);

    for (int i = 0; i < msg->msg_iovlen; ++i) {
        const SceNetIovec &iov = msg->msg_iov.get(net_mem)[i];
        const char *data = reinterpret_cast<const char *>(iov.iov_base.get(net_mem));
        buf.insert(buf.end(), data, data + iov.iov_len);
    }

    const int sent = sock->send_packet(buf.data(), static_cast<unsigned int>(total_len), flags,
        msg->msg_name.get(net_mem), msg->msg_namelen);
    if (sent >= 0)
        process_net.record_interface_transmit(sent, is_packet_socket(*sock));
    RET_NET_ERRNO(sent);
}

EXPORT(int, sceNetSendto, int sid, const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) {
    TRACY_FUNC(sceNetSendto, sid, msg, len, flags, to, tolen);
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);
    if (!sock)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);

    if (len > 0 && !msg)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    if (!to && tolen != 0)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);

    if (!to) {
        const int sent = sock->send_packet(msg, len, flags, nullptr, 0);
        if (sent >= 0)
            process_net.record_interface_transmit(sent, is_packet_socket(*sock));
        RET_NET_ERRNO(sent);
    }
    if (tolen != sizeof(SceNetSockaddrIn))
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);

    SceNetSockaddrIn to_in{};
    std::memcpy(&to_in, to, sizeof(to_in));
    if (!sock->sockopt_so_onesbcast && to_in.sin_addr.s_addr == INADDR_BROADCAST)
        to_in.sin_addr.s_addr = process_net.broadcastAddr;

    const int sent = sock->send_packet(msg, len, flags,
        reinterpret_cast<const SceNetSockaddr *>(&to_in), tolen);
    if (sent >= 0)
        process_net.record_interface_transmit(sent, is_packet_socket(*sock));
    RET_NET_ERRNO(sent);
}

EXPORT(int, sceNetSetDnsInfo) {
    TRACY_FUNC(sceNetSetDnsInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetSetsockopt, int sid, SceNetProtocol level, SceNetSocketOption optname, const void *optval, unsigned int optlen) {
    TRACY_FUNC(sceNetSetsockopt, sid, level, optname, optval, optlen);
    LOG_INFO("sceNetSetsockopt called sid={} level={} optname={} optlen={}",
        sid, log_hex(static_cast<int>(level)), log_hex(static_cast<int>(optname)), optlen);
    if (optname == 0x40000) {
        LOG_ERROR("Unknown socket option {}", log_hex(optname));
        LOG_INFO("sceNetSetsockopt completed sid={} result=0x0", sid);
        return 0;
    }
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);

    const int result = sock ? sock->set_socket_options(level, optname, optval, optlen) : SCE_NET_ERROR_EBADF;
    LOG_INFO("sceNetSetsockopt completed sid={} level={} optname={} result={}",
        sid, log_hex(static_cast<int>(level)), log_hex(static_cast<int>(optname)), log_hex(result));
    RET_NET_ERRNO(result);
}

EXPORT(int, sceNetShowIfconfig) {
    TRACY_FUNC(sceNetShowIfconfig);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetShowNetstat) {
    TRACY_FUNC(sceNetShowNetstat);
    GET_PROCESS_NET_STATE();
    if (!process_net.inited) {
        RET_NET_ERRNO(SCE_NET_ERROR_ENOTINIT);
    }
    return 0;
}

EXPORT(int, sceNetShowRoute) {
    TRACY_FUNC(sceNetShowRoute);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetShutdown, int sid, int how) {
    TRACY_FUNC(sceNetShutdown, sid, how);
    GET_PROCESS_NET_STATE();
    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);

    RET_NET_ERRNO(sock ? sock->shutdown_socket(how) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetSocket, const char *name, int domain, SceNetSocketType type, SceNetProtocol protocol) {
    TRACY_FUNC(sceNetSocket, name, domain, type, protocol);
    GET_PROCESS_NET_STATE();
    bool isP2P = (type == SCE_NET_SOCK_DGRAM_P2P || type == SCE_NET_SOCK_STREAM_P2P);

    SocketPtr sock;
    if (domain == SCE_NET_AF_ROUTE) {
        if (type != SCE_NET_SOCK_RAW || protocol != 0)
            RET_NET_ERRNO(SCE_NET_ERROR_EPROTONOSUPPORT);
        sock = std::make_shared<RouteSocket>(process_net);
    } else if (domain == SCE_NET_AF_LINK) {
        RET_NET_ERRNO(SCE_NET_ERROR_EAFNOSUPPORT);
    } else if (isP2P) {
        sock = std::make_shared<P2PSocket>(domain, type, protocol);
    } else {
        sock = std::make_shared<PosixSocket>(domain, type, protocol);
    }

    auto id = ++process_net.next_id;
    LOG_INFO("sceNetSocket name='{}' domain={} type={} protocol={} sid={}",
        name ? name : "<null>", domain, static_cast<int>(type), static_cast<int>(protocol), id);
    process_net.socks.emplace(id, sock);
    return id;
}

EXPORT(int, sceNetSocketAbort, int sid, int flags) {
    TRACY_FUNC(sceNetSocketAbort, sid);
    GET_PROCESS_NET_STATE();
    if ((sid < 0) || (flags < 0) || (flags > (SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION | SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION)))
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);

    auto sock = lock_and_find(sid, process_net.socks, emuenv.kernel.mutex);
    RET_NET_ERRNO(sock ? sock->abort(flags) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetSocketClose, int sid) {
    TRACY_FUNC(sceNetSocketClose, sid);
    GET_PROCESS_NET_STATE();
    int result = 0;
    {
        std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        const auto sock = util::find(sid, process_net.socks);
        if (sock) {
            result = sock->close();
            if (result >= 0)
                process_net.socks.erase(sid);
        } else
            result = SCE_NET_ERROR_EBADF;
    }

    LOG_INFO("sceNetSocketClose sid={} result={}", sid, log_hex(result));
    RET_NET_ERRNO(result);
}

EXPORT(int, sceNetTerm) {
    TRACY_FUNC(sceNetTerm);
    GET_PROCESS_NET_STATE();
    if (!process_net.inited) {
        RET_NET_ERRNO(SCE_NET_ERROR_ENOTINIT);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    process_net.inited = false;
    return 0;
}
