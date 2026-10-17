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

#include <np/common.h>
#include <util/fs.h>

#include <cstdint>

namespace np {
struct CommunicationID;
} // namespace np

namespace np::trophy {
using ContextHandle = std::int32_t;
struct Context;
} // namespace np::trophy

struct NpState;
struct NpTrophyState;
struct IOState;

bool init(NpState &state, const np::CommunicationID *comm_id);
bool deinit(NpState &state);

bool init(NpTrophyState &state);
bool deinit(NpTrophyState &state);

/**
 * \brief Create a new trophy context.
 *
 * Only one context per 1 communication ID.
 *
 * \param state        Trophy state of the calling process.
 * \param io           IO state.
 * \param comm_id      Communication ID of the trophy set.
 *
 * \returns uint32_t(-1) on failure, else the handle to the context.
 */
np::trophy::ContextHandle create_trophy_context(NpTrophyState &state, IOState *io, const fs::path &vita_fs_path,
    const np::CommunicationID &comm_id, const std::uint32_t lang, np::NpTrophyError *error);

np::trophy::Context *get_trophy_context(NpTrophyState &state, const np::trophy::ContextHandle handle);
bool destroy_trophy_context(NpTrophyState &state, const np::trophy::ContextHandle handle);
