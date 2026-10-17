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

#include <net/epoll.h>

#include <algorithm>
#include <chrono>
#include <optional>

int Epoll::add(int id, std::weak_ptr<Socket> sock, SceNetEpollEvent *ev) {
    const std::lock_guard<std::mutex> lock(entries_mutex);
    if (!eventEntries.try_emplace(id, EpollSocket{ ev->events, ev->data, sock }).second) {
        return SCE_NET_ERROR_EEXIST;
    }

    return 0;
}

int Epoll::del(int id) {
    const std::lock_guard<std::mutex> lock(entries_mutex);
    if (eventEntries.erase(id) == 0) {
        return SCE_NET_ERROR_ENOENT;
    }

    return 0;
}

int Epoll::mod(int id, SceNetEpollEvent *ev) {
    const std::lock_guard<std::mutex> lock(entries_mutex);
    auto it = eventEntries.find(id);
    if (it == eventEntries.end()) {
        return SCE_NET_ERROR_ENOENT;
    }

    it->second.events = ev->events;
    it->second.data = ev->data;
    return 0;
}

void Epoll::wakeup(const std::shared_ptr<Socket> &sock) {
    {
        const std::lock_guard<std::mutex> lock(entries_mutex);
        const bool watches_socket = std::any_of(eventEntries.begin(), eventEntries.end(), [&sock](const auto &entry) {
            return entry.second.sock.lock() == sock;
        });
        if (!watches_socket)
            return;
    }

    {
        const std::lock_guard<std::mutex> lock(wake_mutex);
        ++wake_generation;
    }
    wake_cond.notify_all();
}

static void add_event_fd_set(fd_set *set, int *maxFd, abs_socket sock) {
    FD_SET(sock, set);
    if (sock > *maxFd)
        *maxFd = sock;
}

int Epoll::wait(SceNetEpollEvent *events, int maxevents, int timeout_microseconds) {
    if (!events || maxevents <= 0)
        return SCE_NET_ERROR_EINVAL;

    const auto snapshot_entries = [this] {
        const std::lock_guard<std::mutex> lock(entries_mutex);
        return eventEntries;
    };
    const auto get_valid_posix_socket = [](const std::weak_ptr<Socket> &weak_sock) -> std::optional<abs_socket> {
        const auto sock = weak_sock.lock();
        if (!sock)
            return std::nullopt;

        const auto posixSocket = std::dynamic_pointer_cast<PosixSocket>(sock);
        if (!posixSocket)
            return std::nullopt;

        return posixSocket->sock;
    };

    const bool infinite_timeout = timeout_microseconds < 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(infinite_timeout ? 0 : timeout_microseconds);

    for (;;) {
        uint64_t observed_wake_generation;
        {
            const std::lock_guard<std::mutex> lock(wake_mutex);
            observed_wake_generation = wake_generation;
        }
        const auto entries = snapshot_entries();
        const bool monitor_icm = std::any_of(entries.begin(), entries.end(), [](const auto &entry) {
            return (entry.second.events & (SCE_NETPS_EPOLL_CONNECTED | SCE_NETPS_EPOLL_NOTICE)) != 0;
        });

        int eventCount = 0;
        for (const auto &[id, entry] : entries) {
            if (eventCount == maxevents)
                break;
            const auto sock = entry.sock.lock();
            if (!sock)
                continue;

            uint32_t eventTypes = 0;
            if ((entry.events & SCE_NETPS_EPOLL_CONNECTED) && (sock->sce_icm_flags.load() & SCE_NETPS_SOCKET_CONNECTED)) {
                eventTypes |= SCE_NETPS_EPOLL_CONNECTED | SCE_NETPS_EPOLL_NOTICE;
                sock->sce_icm_flags.fetch_and(~SCE_NETPS_SOCKET_CONNECTED);
            }
            if ((entry.events & SCE_NETPS_EPOLL_NOTICE) && (sock->sce_icm_flags.load() & SCE_NETPS_SOCKET_ERROR_NOTICE)) {
                eventTypes |= SCE_NETPS_EPOLL_NOTICE;
                if (entry.events & SCE_NET_EPOLLERR)
                    eventTypes |= SCE_NET_EPOLLERR;
                sock->sce_icm_flags.fetch_and(~SCE_NETPS_SOCKET_ERROR_NOTICE);
            }
            if (eventTypes && eventCount < maxevents) {
                events[eventCount].events = eventTypes;
                events[eventCount].data = entry.data;
                ++eventCount;
            }
        }
        if (eventCount)
            return eventCount;

        fd_set readFds, writeFds, exceptFds;
        FD_ZERO(&readFds);
        FD_ZERO(&writeFds);
        FD_ZERO(&exceptFds);
        int maxFd = 0;
        for (const auto &[id, entry] : entries) {
            const auto sock = get_valid_posix_socket(entry.sock);
            if (!sock)
                continue;
            if (entry.events & SCE_NET_EPOLLIN)
                add_event_fd_set(&readFds, &maxFd, *sock);
            if (entry.events & SCE_NET_EPOLLOUT)
                add_event_fd_set(&writeFds, &maxFd, *sock);
            if (entry.events & SCE_NET_EPOLLERR)
                add_event_fd_set(&exceptFds, &maxFd, *sock);
        }

        if (!infinite_timeout && timeout_microseconds == 0)
            return 0;
        if (!infinite_timeout && std::chrono::steady_clock::now() >= deadline)
            return 0;
        if (!monitor_icm && maxFd == 0)
            return 0;

        const auto collect_ready_events = [&] {
            for (const auto &[id, entry] : entries) {
                uint32_t eventTypes = 0;
                const auto sock = get_valid_posix_socket(entry.sock);
                if (!sock)
                    continue;
                if (FD_ISSET(*sock, &readFds))
                    eventTypes |= SCE_NET_EPOLLIN;
                if (FD_ISSET(*sock, &writeFds))
                    eventTypes |= SCE_NET_EPOLLOUT;
                if (FD_ISSET(*sock, &exceptFds))
                    eventTypes |= SCE_NET_EPOLLERR;
                if (eventTypes && eventCount < maxevents) {
                    events[eventCount].events = eventTypes;
                    events[eventCount].data = entry.data;
                    ++eventCount;
                }
            }
        };

        if (monitor_icm) {
            if (maxFd > 0) {
                timeval poll_timeout{};
                const auto ret = select(maxFd + 1, &readFds, &writeFds, &exceptFds, &poll_timeout);
                if (ret < 0)
                    return PosixSocket::translate_return_value(ret);
                if (ret > 0) {
                    collect_ready_events();
                    if (eventCount)
                        return eventCount;
                }
            }

            std::unique_lock<std::mutex> lock(wake_mutex);
            const auto was_woken = [this, observed_wake_generation] {
                return wake_generation != observed_wake_generation;
            };
            if (maxFd == 0) {
                if (infinite_timeout)
                    wake_cond.wait(lock, was_woken);
                else
                    wake_cond.wait_until(lock, deadline, was_woken);
            } else {
                auto next_poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
                if (!infinite_timeout)
                    next_poll = std::min(next_poll, deadline);
                wake_cond.wait_until(lock, next_poll, was_woken);
            }
            continue;
        }

        timeval timeout;
        timeval *timeout_pointer = nullptr;
        if (!infinite_timeout) {
            auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
            timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
            timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
            timeout_pointer = &timeout;
        }

        const auto ret = select(maxFd + 1, &readFds, &writeFds, &exceptFds, timeout_pointer);
        if (ret < 0)
            return PosixSocket::translate_return_value(ret);
        if (ret == 0)
            continue;

        collect_ready_events();
        if (eventCount)
            return eventCount;
    }
}
