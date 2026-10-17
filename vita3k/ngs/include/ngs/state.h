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

#include <mem/ptr.h>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

struct MemState;

namespace ngs {
struct VoiceDefinition;
struct System;

struct State {
    Ptr<VoiceDefinition> definitions;
    std::vector<System *> systems;
    std::mutex process_states_mutex;
    std::unordered_map<MemState *, std::unique_ptr<State>> process_states;
};

bool init(State &ngs, MemState &mem);
void deinit(State &ngs, MemState &mem);
State *get_process_state(State &ngs, MemState &primary_mem, MemState &process_mem);
void release_process_state(State &ngs, MemState &process_mem);
} // namespace ngs
