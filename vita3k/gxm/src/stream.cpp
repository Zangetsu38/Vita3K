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

#include <gxm/functions.h>
#include <mem/functions.h>

namespace gxm {
bool is_stream_instancing(SceGxmIndexSource source) {
    return (source == SCE_GXM_INDEX_SOURCE_EACH_INSTANCE_16BIT) || (source == SCE_GXM_INDEX_SOURCE_EACH_INSTANCE_32BIT);
}

GuestVertexStreamResult set_guest_vertex_stream(MemState &mem, uint32_t stream_array, uint32_t stream_count, uint32_t stream_index, uint32_t stream_data) {
    if (stream_index >= SCE_GXM_MAX_VERTEX_STREAMS || stream_index >= stream_count)
        return GuestVertexStreamResult::invalid_index;
    const uint64_t address = uint64_t(stream_array) + uint64_t(stream_index) * sizeof(stream_data);
    if (!stream_array || !stream_data || address + sizeof(stream_data) > (uint64_t{ 1 } << 32) || !write_guest_memory(mem, static_cast<Address>(address), &stream_data, sizeof(stream_data)))
        return GuestVertexStreamResult::invalid_pointer;
    return GuestVertexStreamResult::success;
}
} // namespace gxm
