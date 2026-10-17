// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "SceAppMgr.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

struct AppMgrBgm {
    bool acquire(int32_t pid, int32_t priority) {
        const std::lock_guard lock(mutex);
        if (active_process >= 0 && active_process != pid)
            return false;
        auto &process = processes[pid];
        process.priority = priority;
        process.suspended = false;
        active_process = pid;
        return true;
    }

    bool suspend(int32_t pid) {
        const std::lock_guard lock(mutex);
        const auto it = processes.find(pid);
        if (it == processes.end() || active_process != pid)
            return false;
        it->second.suspended = true;
        active_process = -1;
        return true;
    }

    bool resume(int32_t pid) {
        const std::lock_guard lock(mutex);
        const auto it = processes.find(pid);
        if (it == processes.end() || !it->second.suspended)
            return false;
        it->second.suspended = false;
        active_process = pid;
        return true;
    }

    void release(int32_t pid) {
        const std::lock_guard lock(mutex);
        const auto it = processes.find(pid);
        if (it == processes.end())
            return;
        if (active_process == pid) {
            active_process = -1;
        }
        it->second.priority = 0;
        it->second.suspended = false;
    }

    void set_sub_priority(int32_t pid, bool enabled, int32_t priority) {
        const std::lock_guard lock(mutex);
        auto &process = processes[pid];
        process.sub_priority_enabled = enabled;
        process.sub_priority = priority;
    }

    AppMgrBgmState state(int32_t pid) {
        const std::lock_guard lock(mutex);
        const auto it = processes.find(pid);
        if (active_process != pid || it == processes.end())
            return { -1, 0xffff, 0xffff, -1, {} };
        const auto &process = it->second;
        const int32_t sub_priority = process.sub_priority_enabled ? process.sub_priority : 0;
        return { pid, std::max(process.priority, sub_priority), sub_priority, -1, {} };
    }

private:
    struct ProcessState {
        int32_t priority = 0;
        int32_t sub_priority = 0;
        bool sub_priority_enabled = false;
        bool suspended = false;
    };
    std::mutex mutex;
    int32_t active_process = -1;
    std::unordered_map<int32_t, ProcessState> processes;
};