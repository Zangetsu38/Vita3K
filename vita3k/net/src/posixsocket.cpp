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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <net/socket.h>
#include <sstream>
#include <util/log.h>

static const bool log_enabled = true;

static int convert_socket_domain(int domain) {
    if (domain == SCE_NET_AF_INET6)
        return AF_INET6;
    return domain;
}

PosixSocket::PosixSocket(int domain, int type, int protocol)
    : Socket(domain, type, protocol)
    , sock(::socket(convert_socket_domain(domain), type, protocol)) {
    LOG_INFO("PosixSocket created domain={} type={} protocol={} fd={}", domain, type, protocol, sock);
}

// NOTE: This should be SCE_NET_##errname but it causes vitaQuake to softlock in online games
#ifdef _WIN32
#define ERROR_CASE(errname) \
    case (WSA##errname):    \
        return SCE_NET_ERROR_##errname;
#else
#define ERROR_CASE(errname) \
    case (errname):         \
        return SCE_NET_ERROR_##errname;
#endif

int PosixSocket::translate_return_value(int retval) {
    if (retval < 0) {
#ifdef _WIN32
        switch (WSAGetLastError()) {
#else
        switch (errno) {
#endif
#ifndef _WIN32 // These errorcodes don't exist in WinSock
            ERROR_CASE(EPERM)
            ERROR_CASE(ENOENT)
            ERROR_CASE(ESRCH)
            ERROR_CASE(EIO)
            ERROR_CASE(ENXIO)
            ERROR_CASE(E2BIG)
            ERROR_CASE(ENOEXEC)
            ERROR_CASE(EDEADLK)
            ERROR_CASE(ENOMEM)
            ERROR_CASE(ECHILD)
            ERROR_CASE(EBUSY)
            ERROR_CASE(EEXIST)
            ERROR_CASE(EXDEV)
            ERROR_CASE(ENODEV)
            ERROR_CASE(ENOTDIR)
            ERROR_CASE(EISDIR)
            ERROR_CASE(ENFILE)
            ERROR_CASE(ENOTTY)
            ERROR_CASE(ETXTBSY)
            ERROR_CASE(EFBIG)
            ERROR_CASE(ENOSPC)
            ERROR_CASE(ESPIPE)
            ERROR_CASE(EROFS)
            ERROR_CASE(EMLINK)
            ERROR_CASE(EPIPE)
            ERROR_CASE(EDOM)
            ERROR_CASE(ERANGE)
            ERROR_CASE(ENOLCK)
            ERROR_CASE(ENOSYS)
            ERROR_CASE(EIDRM)
            ERROR_CASE(EOVERFLOW)
            ERROR_CASE(EILSEQ)
            ERROR_CASE(ENOTSUP)
            ERROR_CASE(ECANCELED)
            ERROR_CASE(EBADMSG)
            ERROR_CASE(ENODATA)
            ERROR_CASE(ENOSR)
            ERROR_CASE(ENOSTR)
            ERROR_CASE(ETIME)
#endif
            ERROR_CASE(EINTR)
            ERROR_CASE(EBADF)
            ERROR_CASE(EACCES)
            ERROR_CASE(EFAULT)
            ERROR_CASE(EINVAL)
            ERROR_CASE(EMFILE)
            ERROR_CASE(EWOULDBLOCK)
            ERROR_CASE(EINPROGRESS)
            ERROR_CASE(EALREADY)
            ERROR_CASE(ENOTSOCK)
            ERROR_CASE(EDESTADDRREQ)
            ERROR_CASE(EMSGSIZE)
            ERROR_CASE(EPROTOTYPE)
            ERROR_CASE(ENOPROTOOPT)
            ERROR_CASE(EPROTONOSUPPORT)
#if defined(__APPLE__) || defined(_WIN32)
            ERROR_CASE(EOPNOTSUPP)
#endif
            ERROR_CASE(EAFNOSUPPORT)
            ERROR_CASE(EADDRINUSE)
            ERROR_CASE(EADDRNOTAVAIL)
            ERROR_CASE(ENETDOWN)
            ERROR_CASE(ENETUNREACH)
            ERROR_CASE(ENETRESET)
            ERROR_CASE(ECONNABORTED)
            ERROR_CASE(ECONNRESET)
            ERROR_CASE(ENOBUFS)
            ERROR_CASE(EISCONN)
            ERROR_CASE(ENOTCONN)
            ERROR_CASE(ETIMEDOUT)
            ERROR_CASE(ECONNREFUSED)
            ERROR_CASE(ELOOP)
            ERROR_CASE(ENAMETOOLONG)
            ERROR_CASE(EHOSTUNREACH)
            ERROR_CASE(ENOTEMPTY)
        }
        return SCE_NET_ERROR_EINTERNAL;
    }

    // zero and positive values do not need to be translated, as they indicate success or bytes sent/received
    return retval;
}

static void convertSceSockaddrToPosix(const SceNetSockaddr *src, sockaddr *dst) {
    if (src == nullptr || dst == nullptr)
        return;
    const SceNetSockaddrIn *src_in = reinterpret_cast<const SceNetSockaddrIn *>(src);
    sockaddr_in *dst_in = reinterpret_cast<sockaddr_in *>(dst);
    memset(dst_in, 0, sizeof(*dst_in));
    dst_in->sin_family = src_in->sin_family;
    dst_in->sin_port = src_in->sin_port;
    memcpy(&dst_in->sin_addr, &src_in->sin_addr, sizeof(dst_in->sin_addr));
}

static void convertPosixSockaddrToSce(sockaddr *src, SceNetSockaddr *dst) {
    if (src == nullptr || dst == nullptr)
        return;
    SceNetSockaddrIn *dst_in = reinterpret_cast<SceNetSockaddrIn *>(dst);
    memset(dst_in, 0, sizeof(*dst_in));
    dst_in->sin_len = sizeof(*dst_in);
    sockaddr_in *src_in = reinterpret_cast<sockaddr_in *>(src);
    dst_in->sin_family = static_cast<unsigned char>(src_in->sin_family);
    dst_in->sin_port = src_in->sin_port;
    memcpy(&dst_in->sin_addr, &src_in->sin_addr, sizeof(dst_in->sin_addr));
}

static uint32_t get_ipv4_address_value(const in_addr &address) {
    uint32_t value = 0;
    memcpy(&value, &address, sizeof(value));
    return value;
}

static std::string format_ipv4_endpoint(uint32_t address, uint16_t port) {
    const uint32_t host_address = ntohl(address);
    return std::to_string((host_address >> 24) & 0xFF) + "." + std::to_string((host_address >> 16) & 0xFF) + "." + std::to_string((host_address >> 8) & 0xFF) + "." + std::to_string(host_address & 0xFF) + ":" + std::to_string(ntohs(port));
}

static std::string format_sce_endpoint(const SceNetSockaddr *address, unsigned int address_length) {
    if (!address || address_length < sizeof(SceNetSockaddrIn))
        return "<unavailable>";
    const auto *ipv4 = reinterpret_cast<const SceNetSockaddrIn *>(address);
    return format_ipv4_endpoint(ipv4->sin_addr.s_addr, ipv4->sin_port);
}

static bool abort_pending(bool &is_aborded) {
    if (is_aborded) {
        is_aborded = false; // Reset the flag for the next operation
        return true;
    }

    return false;
}

static bool should_abort(int abort_flags, int flags) {
    return (abort_flags & flags) != 0;
}

int PosixSocket::connect(const SceNetSockaddr *addr, unsigned int addrlen) {
    if (should_abort(abort_flags, SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION)) {
        LOG_INFO_IF(log_enabled, "connect fd={} peer={} result={}", sock, format_sce_endpoint(addr, addrlen), log_hex(SCE_NET_ERROR_EINTR));
        return SCE_NET_ERROR_EINTR;
    }

    sockaddr addr2{};
    convertSceSockaddrToPosix(addr, &addr2);
    const auto res = ::connect(sock, &addr2, sizeof(sockaddr_in));

    if (abort_pending(is_aborted)) {
        LOG_INFO_IF(log_enabled, "connect fd={} peer={} result={}", sock, format_sce_endpoint(addr, addrlen), log_hex(SCE_NET_ERROR_EINTR));
        return SCE_NET_ERROR_EINTR;
    }

    const auto result = translate_return_value(res);
    LOG_INFO_IF(log_enabled, "connect fd={} peer={} result={}", sock, format_sce_endpoint(addr, addrlen), log_hex(result));
    return result;
}

int PosixSocket::bind(const SceNetSockaddr *addr, unsigned int addrlen) {
    sockaddr addr2;
    convertSceSockaddrToPosix(addr, &addr2);
    const auto result = translate_return_value(::bind(sock, &addr2, sizeof(sockaddr_in)));
    LOG_INFO_IF(log_enabled, "bind fd={} local={} result={}", sock, format_sce_endpoint(addr, addrlen), log_hex(result));
    return result;
}

int PosixSocket::listen(int backlog) {
    const auto result = translate_return_value(::listen(sock, backlog));
    LOG_INFO_IF(log_enabled, "listen fd={} backlog={} result={}", sock, backlog, log_hex(result));
    return result;
}

int PosixSocket::get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    if (!addr || !addrlen)
        return SCE_NET_EINVAL;

    sockaddr addr2{};
    const auto res = ::getpeername(sock, &addr2, (socklen_t *)addrlen);
    if (res >= 0) {
        convertPosixSockaddrToSce(&addr2, addr);
        *addrlen = sizeof(SceNetSockaddrIn);
    }

    const auto result = translate_return_value(res);
    const auto *peer = reinterpret_cast<const sockaddr_in *>(&addr2);
    LOG_INFO_IF(log_enabled, "getpeername fd={} peer={} result={}", sock,
        res >= 0 ? format_ipv4_endpoint(get_ipv4_address_value(peer->sin_addr), peer->sin_port) : "<unavailable>", log_hex(result));
    return result;
}

int PosixSocket::get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    if (!addr || !addrlen)
        return SCE_NET_EINVAL;

    sockaddr addr2{};
    const auto res = ::getsockname(sock, &addr2, (socklen_t *)addrlen);
    if (res >= 0) {
        convertPosixSockaddrToSce(&addr2, addr);
        *addrlen = sizeof(SceNetSockaddrIn);
    }
    const auto result = translate_return_value(res);
    const auto *local = reinterpret_cast<const sockaddr_in *>(&addr2);
    LOG_INFO_IF(log_enabled, "getsockname fd={} local={} result={}", sock,
        res >= 0 ? format_ipv4_endpoint(get_ipv4_address_value(local->sin_addr), local->sin_port) : "<unavailable>", log_hex(result));
    return result;
}

int PosixSocket::abort(int flags) {
    abort_flags |= flags;
    is_aborted = true;

#ifdef _WIN32
    auto res = CancelIoEx((HANDLE)sock, nullptr);
    if (res == 0) {
        DWORD err = WSAGetLastError();
        if (err == ERROR_NOT_FOUND)
            return SCE_NET_ERROR_ENOTBLK;
        else
            return SCE_NET_ERROR_EINTERNAL;
    }

    return 0;
#else
    return translate_return_value(shutdown(sock, SHUT_RDWR));
#endif
}

int PosixSocket::close() {
#ifdef _WIN32
    auto out = closesocket(sock);
#else
    auto out = ::close(sock);
#endif
    const auto result = translate_return_value(out);
    LOG_INFO_IF(log_enabled, "close fd={} result={}", sock, log_hex(result));
    return result;
}

int PosixSocket::shutdown_socket(int how) {
    const auto result = translate_return_value(shutdown(sock, how));
    LOG_INFO_IF(log_enabled, "shutdown fd={} how={} result={}", sock, how, log_hex(result));
    return result;
}

SocketPtr PosixSocket::accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) {
    if (should_abort(abort_flags, SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION)) {
        err = SCE_NET_ERROR_EINTR;
        LOG_INFO_IF(log_enabled, "accept fd={} result={}", sock, log_hex(err));
        return nullptr;
    }
    sockaddr addr2{};
    const abs_socket new_socket = ::accept(sock, &addr2, (socklen_t *)addrlen);
    if (abort_pending(is_aborted)) {
        err = SCE_NET_ERROR_EINTR;
        LOG_INFO_IF(log_enabled, "accept fd={} result={}", sock, log_hex(err));
        return nullptr;
    }
#ifdef _WIN32
    if (new_socket != INVALID_SOCKET) {
#else
    if (new_socket >= 0) {
#endif
        convertPosixSockaddrToSce(&addr2, addr);
        *addrlen = sizeof(SceNetSockaddrIn);
        const auto *peer = reinterpret_cast<const sockaddr_in *>(&addr2);
        LOG_INFO_IF(log_enabled, "accept fd={} peer={} accepted_fd={}", sock,
            format_ipv4_endpoint(get_ipv4_address_value(peer->sin_addr), peer->sin_port), new_socket);
        return std::make_shared<PosixSocket>(new_socket, sce_type);
    }
    err = translate_return_value(new_socket);
    LOG_INFO_IF(log_enabled, "accept fd={} result={}", sock, log_hex(err));
    return nullptr;
}

static int translate_sockopt_level(int level) {
    switch (level) {
    case SCE_NET_SOL_SOCKET: return SOL_SOCKET;
    case SCE_NET_IPPROTO_IP: return IPPROTO_IP;
    case SCE_NET_IPPROTO_TCP: return IPPROTO_TCP;
    }
    return -1;
}

#define CASE_SETSOCKOPT(opt) \
    case SCE_NET_##opt:      \
        return translate_return_value(setsockopt(sock, level, opt, reinterpret_cast<const char *>(optval), optlen))

#define CASE_SETSOCKOPT_VALUE(opt, value) \
    case opt:                             \
        if (optlen != sizeof(*value)) {   \
            return SCE_NET_ERROR_EFAULT;  \
        }                                 \
        memcpy(value, optval, optlen);    \
        return 0

int PosixSocket::set_socket_options(int level, int optname, const void *optval, unsigned int optlen) {
    level = translate_sockopt_level(level);
    if (level == SOL_SOCKET) {
        switch (optname) {
            CASE_SETSOCKOPT(SO_REUSEADDR);
            CASE_SETSOCKOPT(SO_KEEPALIVE);
            CASE_SETSOCKOPT(SO_BROADCAST);
            CASE_SETSOCKOPT(SO_LINGER);
            CASE_SETSOCKOPT(SO_OOBINLINE);
            CASE_SETSOCKOPT(SO_SNDBUF);
            CASE_SETSOCKOPT(SO_RCVBUF);
            CASE_SETSOCKOPT(SO_SNDLOWAT);
            CASE_SETSOCKOPT(SO_RCVLOWAT);
            CASE_SETSOCKOPT(SO_ERROR);
            CASE_SETSOCKOPT(SO_TYPE);
            CASE_SETSOCKOPT_VALUE(SCE_NET_SO_REUSEPORT, &sockopt_so_reuseport);
            CASE_SETSOCKOPT_VALUE(SCE_NET_SO_USECRYPTO, &sockopt_so_usecrypto);
            CASE_SETSOCKOPT_VALUE(SCE_NET_SO_USESIGNATURE, &sockopt_so_usesignature);
            CASE_SETSOCKOPT_VALUE(SCE_NET_SO_TPPOLICY, &sockopt_so_tppolicy);
        case SCE_NET_SO_OPTION_10000:
        case SCE_NET_SO_OPTION_20000: {
            if (!optval || optlen < sizeof(int))
                return SCE_NET_ERROR_EFAULT;
            int value = 0;
            memcpy(&value, optval, sizeof(value));
            if (value)
                sce_options.fetch_or(static_cast<uint32_t>(optname));
            else
                sce_options.fetch_and(~static_cast<uint32_t>(optname));
            return 0;
        }
        case SCE_NET_SO_ONESBCAST:
            if (optlen != sizeof(sockopt_so_onesbcast))
                return SCE_NET_ERROR_EFAULT;
            memcpy(&sockopt_so_onesbcast, optval, optlen);

            // Sets the option to allow sending broadcast packets on a socket
            return translate_return_value(setsockopt(sock, level, SO_BROADCAST, reinterpret_cast<const char *>(optval), optlen));
        case SCE_NET_SO_SNDTIMEO:
        case SCE_NET_SO_RCVTIMEO: {
            if (optlen != sizeof(int))
                return SCE_NET_ERROR_EFAULT;

            std::vector<char> val;
            const auto optname_nat = (optname == SCE_NET_SO_SNDTIMEO) ? SO_SNDTIMEO : SO_RCVTIMEO;
            int timeout_us = *reinterpret_cast<const int *>(optval);
#ifdef _WIN32
            DWORD timeout = timeout_us / 1000;
            val.insert(val.end(), reinterpret_cast<char *>(&timeout), reinterpret_cast<char *>(&timeout) + sizeof(timeout));
            optlen = sizeof(timeout);
#else
            timeval timeout{
                .tv_sec = timeout_us / 1000000,
                .tv_usec = timeout_us % 1000000
            };
            val.insert(val.end(), reinterpret_cast<char *>(&timeout), reinterpret_cast<char *>(&timeout) + sizeof(timeout));
            optlen = sizeof(timeout);
#endif
            return translate_return_value(setsockopt(sock, level, optname_nat, val.data(), optlen));
        }
        case SCE_NET_SO_NAME:
            return SCE_NET_ERROR_EINVAL; // don't support set for name
        case SCE_NET_SO_NBIO: {
            if (optlen != sizeof(sockopt_so_nbio))
                return SCE_NET_ERROR_EFAULT;
            memcpy(&sockopt_so_nbio, optval, optlen);
#ifdef _WIN32
            static_assert(sizeof(u_long) == sizeof(sockopt_so_nbio), "type used for ioctlsocket value does not have the expected size");
            return translate_return_value(ioctlsocket(sock, FIONBIO, (u_long *)&sockopt_so_nbio));
#else
            return translate_return_value(ioctl(sock, FIONBIO, &sockopt_so_nbio));
#endif
        }
        }
    } else if (level == IPPROTO_IP) {
        switch (optname) {
            CASE_SETSOCKOPT(IP_HDRINCL);
            CASE_SETSOCKOPT(IP_TOS);
            CASE_SETSOCKOPT(IP_TTL);
            CASE_SETSOCKOPT(IP_MULTICAST_IF);
            CASE_SETSOCKOPT(IP_MULTICAST_TTL);
            CASE_SETSOCKOPT(IP_MULTICAST_LOOP);
            CASE_SETSOCKOPT(IP_ADD_MEMBERSHIP);
            CASE_SETSOCKOPT(IP_DROP_MEMBERSHIP);
            CASE_SETSOCKOPT_VALUE(SCE_NET_IP_TTLCHK, &sockopt_ip_ttlchk);
            CASE_SETSOCKOPT_VALUE(SCE_NET_IP_MAXTTL, &sockopt_ip_maxttl);
        }
    } else if (level == IPPROTO_TCP) {
        switch (optname) {
            CASE_SETSOCKOPT(TCP_NODELAY);
            CASE_SETSOCKOPT(TCP_MAXSEG);
            CASE_SETSOCKOPT_VALUE(SCE_NET_TCP_MSS_TO_ADVERTISE, &sockopt_tcp_mss_to_advertise);
        }
    }

    return SCE_NET_ERROR_EINVAL;
}

#define CASE_GETSOCKOPT(opt)                                                                                                \
    case SCE_NET_##opt: {                                                                                                   \
        socklen_t optlen_temp = *optlen;                                                                                    \
        auto retval = translate_return_value(getsockopt(sock, level, opt, reinterpret_cast<char *>(optval), &optlen_temp)); \
        *optlen = optlen_temp;                                                                                              \
        return retval;                                                                                                      \
    }
#define CASE_GETSOCKOPT_VALUE(opt, value)   \
    case opt:                               \
        if (*optlen < sizeof(value)) {      \
            *optlen = sizeof(value);        \
            return SCE_NET_ERROR_EFAULT;    \
        }                                   \
        *optlen = sizeof(value);            \
        *(decltype(value) *)optval = value; \
        return 0;

int PosixSocket::get_socket_options(int level, int optname, void *optval, unsigned int *optlen) {
    level = translate_sockopt_level(level);
    if (level == SOL_SOCKET) {
        switch (optname) {
            CASE_GETSOCKOPT(SO_REUSEADDR);
            CASE_GETSOCKOPT(SO_KEEPALIVE);
            CASE_GETSOCKOPT(SO_BROADCAST);
            CASE_GETSOCKOPT(SO_LINGER);
            CASE_GETSOCKOPT(SO_OOBINLINE);
            CASE_GETSOCKOPT(SO_SNDBUF);
            CASE_GETSOCKOPT(SO_RCVBUF);
            CASE_GETSOCKOPT(SO_SNDLOWAT);
            CASE_GETSOCKOPT(SO_RCVLOWAT);
            CASE_GETSOCKOPT(SO_SNDTIMEO);
            CASE_GETSOCKOPT(SO_RCVTIMEO);
        case SCE_NET_SO_ERROR: {
            if (!optlen || !optval)
                return SCE_NET_ERROR_EFAULT;
            const int icm_error = sce_icm_error.load();
            if (icm_error) {
                if (*optlen < sizeof(icm_error)) {
                    *optlen = sizeof(icm_error);
                    return SCE_NET_ERROR_EFAULT;
                }
                const int error = sce_icm_error.exchange(0);
                memcpy(optval, &error, sizeof(error));
                *optlen = sizeof(error);
                return 0;
            }
            socklen_t optlen_temp = *optlen;
            const auto retval = translate_return_value(getsockopt(sock, level, SO_ERROR,
                reinterpret_cast<char *>(optval), &optlen_temp));
            *optlen = optlen_temp;
            return retval;
        }
            CASE_GETSOCKOPT(SO_TYPE);
            CASE_GETSOCKOPT_VALUE(SCE_NET_SO_NBIO, sockopt_so_nbio);
            CASE_GETSOCKOPT_VALUE(SCE_NET_SO_REUSEPORT, sockopt_so_reuseport);
            CASE_GETSOCKOPT_VALUE(SCE_NET_SO_ONESBCAST, sockopt_so_onesbcast);
            CASE_GETSOCKOPT_VALUE(SCE_NET_SO_USECRYPTO, sockopt_so_usecrypto);
            CASE_GETSOCKOPT_VALUE(SCE_NET_SO_USESIGNATURE, sockopt_so_usesignature);
            CASE_GETSOCKOPT_VALUE(SCE_NET_SO_TPPOLICY, sockopt_so_tppolicy);
        case SCE_NET_SO_OPTION_10000:
        case SCE_NET_SO_OPTION_20000: {
            if (!optlen || !optval)
                return SCE_NET_ERROR_EFAULT;
            if (*optlen < sizeof(int)) {
                *optlen = sizeof(int);
                return SCE_NET_ERROR_EFAULT;
            }
            const int value = (sce_options.load() & static_cast<uint32_t>(optname)) != 0;
            memcpy(optval, &value, sizeof(value));
            *optlen = sizeof(value);
            return 0;
        }
            CASE_GETSOCKOPT_VALUE(SCE_NET_SO_NAME, (char)0); // writes an empty string to the output buffer
        }
    } else if (level == IPPROTO_IP) {
        switch (optname) {
            CASE_GETSOCKOPT(IP_HDRINCL);
            CASE_GETSOCKOPT(IP_TOS);
            CASE_GETSOCKOPT(IP_TTL);
            CASE_GETSOCKOPT(IP_MULTICAST_IF);
            CASE_GETSOCKOPT(IP_MULTICAST_TTL);
            CASE_GETSOCKOPT(IP_MULTICAST_LOOP);
            CASE_GETSOCKOPT(IP_ADD_MEMBERSHIP);
            CASE_GETSOCKOPT(IP_DROP_MEMBERSHIP);
            CASE_GETSOCKOPT_VALUE(SCE_NET_IP_TTLCHK, sockopt_ip_ttlchk);
            CASE_GETSOCKOPT_VALUE(SCE_NET_IP_MAXTTL, sockopt_ip_maxttl);
        }
    } else if (level == IPPROTO_TCP) {
        switch (optname) {
            CASE_GETSOCKOPT(TCP_NODELAY);
            CASE_GETSOCKOPT(TCP_MAXSEG);
            CASE_GETSOCKOPT_VALUE(SCE_NET_TCP_MSS_TO_ADVERTISE, sockopt_tcp_mss_to_advertise);
        }
    }
    return SCE_NET_ERROR_EINVAL;
}

static std::string log_ascii_hex(const uint8_t *data, size_t len) {
    std::stringstream stream;
    stream << std::endl;
    for (size_t offset = 0; offset < len; offset += 16) {
        stream << std::setw(8) << std::setfill('0') << std::hex << offset << "  ";
        const auto row_length = std::min<size_t>(16, len - offset);
        for (size_t index = 0; index < row_length; ++index)
            stream << std::setw(2) << std::setfill('0') << std::hex << static_cast<unsigned int>(data[offset + index]) << " ";
        for (size_t index = row_length; index < 16; ++index)
            stream << "   ";

        stream << " |";
        for (size_t index = 0; index < row_length; ++index) {
            const auto value = data[offset + index];
            stream << (value >= 32 && value <= 126 ? static_cast<char>(value) : '.');
        }
        for (size_t index = row_length; index < 16; ++index)
            stream << ' ';
        stream << "|\n";
    }
    return stream.str();
}

static int convertSceFlagsToPosix(int sock_type, int sce_flags) {
    int posix_flags = 0;

    if (sce_flags & SCE_NET_MSG_PEEK)
        posix_flags |= MSG_PEEK;
#ifndef _WIN32
    if (sce_flags & SCE_NET_MSG_DONTWAIT)
        posix_flags |= MSG_DONTWAIT;
#endif
    // MSG_WAITALL is only valid for stream sockets
    if ((sce_flags & SCE_NET_MSG_WAITALL) && ((sock_type == SCE_NET_SOCK_STREAM) || (sock_type == SCE_NET_SOCK_STREAM_P2P)))
        posix_flags |= MSG_WAITALL;

    return posix_flags;
}

// On Windows, MSG_DONTWAIT is not handled natively by recv/send.
// This function uses select() with zero timeout to simulate non-blocking behavior.
static int socket_is_ready(int sock, bool is_read = true) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    timeval timeout{ 0, 0 };
    int res = select(sock + 1, is_read ? &fds : nullptr, is_read ? nullptr : &fds, nullptr, &timeout);
    if (res == 0) {
        LOG_INFO_IF(log_enabled, "socket_is_ready fd={} direction={} result={}", sock, is_read ? "recv" : "send", log_hex(SCE_NET_ERROR_EWOULDBLOCK));
        return SCE_NET_ERROR_EWOULDBLOCK;
    } else if (res < 0) {
        const auto result = PosixSocket::translate_return_value(res);
        LOG_ERROR_IF(log_enabled, "socket_is_ready fd={} direction={} result={}", sock, is_read ? "recv" : "send", log_hex(result));
        return result;
    }

    return res;
}

int PosixSocket::recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) {
    if (should_abort(abort_flags, SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION)) {
        LOG_INFO_IF(log_enabled, "recv_packet fd={} flags={} requested={} result={}", sock, log_hex(flags), len, log_hex(SCE_NET_ERROR_EINTR));
        return SCE_NET_ERROR_EINTR;
    }

    int res = 0;
#ifdef _WIN32
    if (flags & SCE_NET_MSG_DONTWAIT) {
        res = socket_is_ready(sock);
        if (res <= 0) {
            LOG_INFO_IF(log_enabled, "recv_packet fd={} flags={} requested={} result={}", sock, log_hex(flags), len, log_hex(res));
            return res;
        }
    }
#endif
    const auto posix_flags = convertSceFlagsToPosix(sce_type, flags);
    std::string source_endpoint = from ? "<unavailable>" : "<connected>";
    sockaddr addr{};
    if (from == nullptr) {
        res = recv(sock, reinterpret_cast<char *>(buf), len, posix_flags);
    } else {
        socklen_t addrlen = sizeof(addr);
        res = recvfrom(sock, reinterpret_cast<char *>(buf), len, posix_flags, &addr, (fromlen && *fromlen <= sizeof(addr) ? reinterpret_cast<socklen_t *>(fromlen) : &addrlen));
        if (res > 0) {
            const auto *source = reinterpret_cast<const sockaddr_in *>(&addr);
            source_endpoint = format_ipv4_endpoint(get_ipv4_address_value(source->sin_addr), source->sin_port);
            convertPosixSockaddrToSce(&addr, from);
        }
    }

    const auto result = translate_return_value(res);
    const auto payload = res > 0
        ? log_ascii_hex(static_cast<const uint8_t *>(buf), static_cast<size_t>(res))
        : std::string("<none>");
    LOG_INFO_IF(log_enabled, "recv_packet fd={} from={} flags={} requested={} received={} result={} payload:{}",
        sock, source_endpoint, log_hex(flags), len, res, log_hex(result), payload);

    if (abort_pending(is_aborted)) {
        LOG_INFO_IF(log_enabled, "recv_packet fd={} interrupted after receive", sock);
        return SCE_NET_ERROR_EINTR;
    }

    return result;
}

int PosixSocket::send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) {
    const auto destination_endpoint = to ? format_sce_endpoint(to, tolen) : std::string("<connected>");
    if (should_abort(abort_flags, SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION)) {
        LOG_INFO_IF(log_enabled, "send_packet fd={} to={} flags={} requested={} result={}", sock,
            destination_endpoint, log_hex(flags), len, log_hex(SCE_NET_ERROR_EINTR));
        return SCE_NET_ERROR_EINTR;
    }

    int res = 0;
#ifdef _WIN32
    if (flags & SCE_NET_MSG_DONTWAIT) {
        res = socket_is_ready(sock, false);
        if (res <= 0) {
            LOG_INFO_IF(log_enabled, "send_packet fd={} to={} flags={} requested={} result={}", sock,
                destination_endpoint, log_hex(flags), len, log_hex(res));
            return res;
        }
    }
#endif
    const auto posix_flags = convertSceFlagsToPosix(sce_type, flags);
    if (to == nullptr) {
        res = send(sock, reinterpret_cast<const char *>(msg), len, posix_flags);
    } else {
        sockaddr addr{};
        convertSceSockaddrToPosix(to, &addr);
        res = sendto(sock, reinterpret_cast<const char *>(msg), len, posix_flags, &addr, tolen);
    }

    const auto result = translate_return_value(res);
    const auto payload = res > 0
        ? log_ascii_hex(static_cast<const uint8_t *>(msg), static_cast<size_t>(res))
        : std::string("<none>");
    LOG_INFO_IF(log_enabled, "send_packet fd={} to={} flags={} requested={} sent={} result={} payload:{}",
        sock, destination_endpoint, log_hex(flags), len, res, log_hex(result), payload);

    if (abort_pending(is_aborted)) {
        LOG_INFO_IF(log_enabled, "send_packet fd={} interrupted after send", sock);
        return SCE_NET_ERROR_EINTR;
    }

    return result;
}
