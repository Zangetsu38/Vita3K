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
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <util/log.h>

namespace {
struct RouteMessageHeader {
    uint16_t message_length;
    uint8_t version;
    uint8_t type;
    uint16_t interface_index;
    uint16_t reserved;
    int32_t flags;
    int32_t addresses;
    int32_t process_id;
    int32_t sequence;
    int32_t error;
    int32_t use;
    uint32_t metric_inits;
    uint32_t metric_locks;
    uint32_t mtu;
    uint32_t metrics[8];
};

static_assert(sizeof(RouteMessageHeader) == 76);

constexpr uint8_t route_message_version = 3;
constexpr uint8_t route_message_add = 1;
constexpr uint8_t route_message_delete = 2;
constexpr uint8_t route_message_change = 3;
constexpr uint8_t route_message_get = 4;
constexpr uint32_t route_flag_host = 0x4;
constexpr uint32_t route_address_destination = 0x1;
constexpr uint32_t route_address_gateway = 0x2;
constexpr uint32_t route_address_netmask = 0x4;
constexpr uint32_t route_address_interface = 0x10;
constexpr uint32_t route_address_mask = 0xFF;

struct RouteAddressView {
    const uint8_t *data = nullptr;
    uint8_t length = 0;
    uint8_t family = 0;
};

int parse_route_message(const void *message, unsigned int length, RouteMessageHeader &header,
    std::array<RouteAddressView, 8> &addresses, NetRoute &route) {
    if (!message || length < sizeof(header))
        return SCE_NET_ERROR_EINVAL;

    std::memcpy(&header, message, sizeof(header));
    if (header.message_length < sizeof(header) || header.message_length > length || header.version != route_message_version || (header.addresses & ~route_address_mask))
        return SCE_NET_ERROR_EINVAL;

    const auto *bytes = static_cast<const uint8_t *>(message);
    size_t offset = sizeof(header);
    for (size_t i = 0; i < addresses.size(); ++i) {
        if (!(header.addresses & (1u << i)))
            continue;
        if (offset + 2 > header.message_length)
            return SCE_NET_ERROR_EINVAL;

        const uint8_t address_length = bytes[offset];
        const size_t padded_length = address_length ? (address_length + 3u) & ~size_t{ 3 } : 4;
        if ((address_length && address_length < 2) || offset + padded_length > header.message_length)
            return SCE_NET_ERROR_EINVAL;

        addresses[i] = { bytes + offset, address_length, bytes[offset + 1] };
        offset += padded_length;
    }

    if (!addresses[0].data || addresses[0].family != SCE_NET_AF_INET || addresses[0].length < 8)
        return SCE_NET_ERROR_EAFNOSUPPORT;
    std::memcpy(&route.destination, addresses[0].data + 4, sizeof(route.destination));

    route.interface_index = header.interface_index;
    route.flags = static_cast<uint32_t>(header.flags);
    route.mtu = header.mtu;
    if (addresses[1].data) {
        if (addresses[1].family == SCE_NET_AF_INET && addresses[1].length >= 8) {
            std::memcpy(&route.gateway, addresses[1].data + 4, sizeof(route.gateway));
            route.gateway_present = true;
        } else if (addresses[1].family != SCE_NET_AF_LINK) {
            return SCE_NET_ERROR_EAFNOSUPPORT;
        }
    }

    if (addresses[2].data) {
        if (addresses[2].length == 0) {
            route.netmask = 0;
        } else if (addresses[2].family != SCE_NET_AF_INET || addresses[2].length < 8) {
            return SCE_NET_ERROR_EAFNOSUPPORT;
        } else {
            std::memcpy(&route.netmask, addresses[2].data + 4, sizeof(route.netmask));
        }
    } else if (route.flags & route_flag_host) {
        route.netmask = 0xFFFFFFFF;
    }

    if (addresses[4].data && addresses[4].family == SCE_NET_AF_LINK && addresses[4].length >= 8) {
        const uint8_t *link_address = addresses[4].data;
        uint16_t link_index = 0;
        std::memcpy(&link_index, link_address + 2, sizeof(link_index));
        if (!route.interface_index)
            route.interface_index = link_index;

        const uint8_t name_length = link_address[5];
        if (name_length <= addresses[4].length - 8)
            route.interface_name.assign(reinterpret_cast<const char *>(link_address + 8), name_length);
    }

    return 0;
}

size_t round_up_sockaddr(size_t length) {
    return length ? (length + 3) & ~size_t{ 3 } : 4;
}

void append_bytes(std::vector<uint8_t> &output, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    output.insert(output.end(), bytes, bytes + size);
}

void append_ipv4_sockaddr(std::vector<uint8_t> &output, uint32_t address) {
    SceNetSockaddrIn sockaddr{};
    sockaddr.sin_len = sizeof(sockaddr);
    sockaddr.sin_family = SCE_NET_AF_INET;
    sockaddr.sin_addr.s_addr = address;
    append_bytes(output, &sockaddr, sizeof(sockaddr));
}
} // namespace

RouteSocket::RouteSocket(NetState &net)
    : Socket(SCE_NET_AF_ROUTE, SCE_NET_SOCK_RAW, 0)
    , net(net) {}

int RouteSocket::abort(int flags) {
    if (flags < 0 || flags > static_cast<int>(SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION | SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION))
        return SCE_NET_ERROR_EINVAL;
    return 0;
}

int RouteSocket::close() {
    return 0;
}

int RouteSocket::shutdown_socket(int how) {
    if (how < 0 || how > 2)
        return SCE_NET_ERROR_EINVAL;
    return 0;
}

int RouteSocket::bind(const SceNetSockaddr *, unsigned int) {
    return SCE_NET_ERROR_EOPNOTSUPP;
}

int RouteSocket::send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) {
    if (flags != 0 || to || tolen != 0)
        return SCE_NET_ERROR_EINVAL;

    RouteMessageHeader header{};
    std::array<RouteAddressView, 8> addresses{};
    NetRoute route{};
    const int parse_result = parse_route_message(msg, len, header, addresses, route);
    if (parse_result < 0) {
        LOG_WARN("PF_ROUTE rejected malformed message length={} result={}", len, log_hex(parse_result));
        return parse_result;
    }

    std::lock_guard<std::mutex> lock(net.route_mutex);
    const auto existing = std::find_if(net.routes.begin(), net.routes.end(), [&route](const NetRoute &entry) {
        return entry.destination == route.destination && entry.netmask == route.netmask && entry.interface_index == route.interface_index;
    });

    switch (header.type) {
    case route_message_add:
        if (existing != net.routes.end())
            return SCE_NET_ERROR_EEXIST;
        net.routes.push_back(route);
        break;
    case route_message_change:
        if (existing == net.routes.end())
            return SCE_NET_ERROR_ENOENT;
        *existing = route;
        break;
    case route_message_delete:
        if (existing == net.routes.end())
            return SCE_NET_ERROR_ENOENT;
        net.routes.erase(existing);
        break;
    default:
        return SCE_NET_ERROR_EOPNOTSUPP;
    }

    LOG_INFO("PF_ROUTE message type={} destination={} netmask={} gateway={} interface={} routes={}",
        header.type, route.destination, route.netmask, route.gateway, route.interface_index, net.routes.size());
    return static_cast<int>(len);
}

int RouteSocket::recv_packet(void *, unsigned int, int, SceNetSockaddr *, unsigned int *) {
    return SCE_NET_ERROR_EWOULDBLOCK;
}

int RouteSocket::set_socket_options(int, int, const void *, unsigned int) {
    return SCE_NET_ERROR_ENOPROTOOPT;
}

int RouteSocket::get_socket_options(int, int, void *, unsigned int *) {
    return SCE_NET_ERROR_ENOPROTOOPT;
}

int RouteSocket::connect(const SceNetSockaddr *, unsigned int) {
    return SCE_NET_ERROR_EOPNOTSUPP;
}

SocketPtr RouteSocket::accept(SceNetSockaddr *, unsigned int *, int &err) {
    err = SCE_NET_ERROR_EOPNOTSUPP;
    return nullptr;
}

int RouteSocket::listen(int) {
    return SCE_NET_ERROR_EOPNOTSUPP;
}

int RouteSocket::get_peer_address(SceNetSockaddr *, unsigned int *) {
    return SCE_NET_ERROR_EOPNOTSUPP;
}

int RouteSocket::get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    if (!addr || !addrlen)
        return SCE_NET_ERROR_EINVAL;
    if (*addrlen < sizeof(SceNetSockaddr)) {
        *addrlen = sizeof(SceNetSockaddr);
        return SCE_NET_ERROR_ENOMEM;
    }

    SceNetSockaddr route_address{};
    route_address.sa_len = 2;
    route_address.sa_family = SCE_NET_AF_ROUTE;
    std::memcpy(addr, &route_address, sizeof(route_address));
    *addrlen = sizeof(route_address);
    return 0;
}

bool build_route_table_dump(const NetState &net, int family, int interface_index, std::vector<uint8_t> &dump) {
    if (family != SCE_NET_AF_INET)
        return false;

    std::vector<NetRoute> routes;
    {
        const std::lock_guard<std::mutex> lock(net.route_mutex);
        routes = net.routes;
    }

    dump.clear();
    for (const NetRoute &route : routes) {
        if (interface_index != -1 && route.interface_index != interface_index)
            continue;

        const bool include_interface = !route.interface_name.empty();
        const uint32_t address_mask = route_address_destination | route_address_netmask | (route.gateway_present ? route_address_gateway : 0) | (include_interface ? route_address_interface : 0);
        const size_t message_size = sizeof(RouteMessageHeader) + sizeof(SceNetSockaddrIn) * (2 + static_cast<size_t>(route.gateway_present)) + (include_interface ? round_up_sockaddr(8 + std::min<size_t>(route.interface_name.size(), 12)) : 0);
        if (message_size > std::numeric_limits<uint16_t>::max())
            return false;

        RouteMessageHeader header{};
        header.message_length = static_cast<uint16_t>(message_size);
        header.version = route_message_version;
        header.type = route_message_get;
        header.interface_index = route.interface_index;
        header.flags = static_cast<int32_t>(route.flags);
        header.addresses = static_cast<int32_t>(address_mask);
        header.mtu = route.mtu;
        append_bytes(dump, &header, sizeof(header));
        append_ipv4_sockaddr(dump, route.destination);
        if (route.gateway_present)
            append_ipv4_sockaddr(dump, route.gateway);
        append_ipv4_sockaddr(dump, route.netmask);

        if (include_interface) {
            const size_t name_length = std::min<size_t>(route.interface_name.size(), 12);
            std::array<uint8_t, 20> link_address{};
            link_address[0] = static_cast<uint8_t>(8 + name_length);
            link_address[1] = SCE_NET_AF_LINK;
            std::memcpy(link_address.data() + 2, &route.interface_index, sizeof(route.interface_index));
            link_address[5] = static_cast<uint8_t>(name_length);
            std::memcpy(link_address.data() + 8, route.interface_name.data(), name_length);
            append_bytes(dump, link_address.data(), round_up_sockaddr(8 + name_length));
        }
    }
    return true;
}
