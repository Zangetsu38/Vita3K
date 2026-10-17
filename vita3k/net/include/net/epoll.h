#pragma once

#include <net/socket.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

struct EpollSocket {
    unsigned int events;
    SceNetEpollData data;
    std::weak_ptr<Socket> sock;
};

struct Epoll {
    std::map<int, EpollSocket> eventEntries;
    std::mutex entries_mutex;
    std::mutex wake_mutex;
    std::condition_variable wake_cond;
    uint64_t wake_generation = 0;

    int add(int id, std::weak_ptr<Socket> sock, SceNetEpollEvent *ev);
    int del(int id);
    int mod(int id, SceNetEpollEvent *ev);
    void wakeup(const std::shared_ptr<Socket> &sock);
    int wait(SceNetEpollEvent *events, int maxevents, int timeout);
};

using EpollPtr = std::shared_ptr<Epoll>;
