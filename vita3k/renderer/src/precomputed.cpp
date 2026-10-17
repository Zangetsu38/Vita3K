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

#include <renderer/functions.h>

#include <mem/functions.h>

#include <algorithm>

namespace renderer {

bool read_precomputed_draw(MemState &mem, Address draw, Address vertex, Address fragment, PrecomputedDrawSnapshot &snapshot) {
    snapshot = {};
    if (!draw || !read_guest_memory(mem, draw, &snapshot.draw, sizeof(snapshot.draw)) || snapshot.draw.stream_count > SCE_GXM_MAX_VERTEX_STREAMS)
        return false;
    if (snapshot.draw.stream_count && (!snapshot.draw.stream_data || !read_guest_memory(mem, snapshot.draw.stream_data.address(), snapshot.streams.data(), snapshot.draw.stream_count * sizeof(StreamData))))
        return false;
    snapshot.has_vertex = vertex != 0;
    snapshot.has_fragment = fragment != 0;
    if (snapshot.has_vertex) {
        if (!read_guest_memory(mem, vertex, &snapshot.vertex, sizeof(snapshot.vertex)) || snapshot.vertex.texture_count > snapshot.vertex_textures.size() || snapshot.vertex.buffer_count > snapshot.vertex_buffers.size())
            return false;
        if (snapshot.vertex.texture_count && (!snapshot.vertex.textures || !read_guest_memory(mem, snapshot.vertex.textures.address(), snapshot.vertex_textures.data(), snapshot.vertex.texture_count * sizeof(TextureData))))
            return false;
        if (snapshot.vertex.buffer_count && (!snapshot.vertex.uniform_buffers || !read_guest_memory(mem, snapshot.vertex.uniform_buffers.address(), snapshot.vertex_buffers.data(), snapshot.vertex.buffer_count * sizeof(UniformBuffer))))
            return false;
    }
    if (snapshot.has_fragment) {
        if (!read_guest_memory(mem, fragment, &snapshot.fragment, sizeof(snapshot.fragment)) || snapshot.fragment.texture_count > snapshot.fragment_textures.size() || snapshot.fragment.buffer_count > snapshot.fragment_buffers.size())
            return false;
        if (snapshot.fragment.texture_count && (!snapshot.fragment.textures || !read_guest_memory(mem, snapshot.fragment.textures.address(), snapshot.fragment_textures.data(), snapshot.fragment.texture_count * sizeof(TextureData))))
            return false;
        if (snapshot.fragment.buffer_count && (!snapshot.fragment.uniform_buffers || !read_guest_memory(mem, snapshot.fragment.uniform_buffers.address(), snapshot.fragment_buffers.data(), snapshot.fragment.buffer_count * sizeof(UniformBuffer))))
            return false;
    }
    return true;
}

bool read_precomputed_max_index(MemState &mem, Ptr<const void> indices, SceGxmIndexFormat format, uint32_t count, uint32_t &maximum) {
    maximum = 0;
    if (format != SCE_GXM_INDEX_FORMAT_U16 && format != SCE_GXM_INDEX_FORMAT_U32)
        return false;
    const uint32_t element_size = format == SCE_GXM_INDEX_FORMAT_U16 ? sizeof(uint16_t) : sizeof(uint32_t);
    if (!indices || uint64_t(indices.address()) + uint64_t(count) * element_size > (uint64_t{ 1 } << 32))
        return false;
    std::array<uint16_t, 2048> small_values{};
    std::array<uint32_t, 1024> large_values{};
    for (uint32_t offset = 0; offset < count;) {
        const uint32_t chunk = std::min<uint32_t>(count - offset, 4096 / element_size);
        const Address address = static_cast<Address>(uint64_t(indices.address()) + uint64_t(offset) * element_size);
        if (element_size == sizeof(uint16_t)) {
            if (!read_guest_memory(mem, address, small_values.data(), chunk * element_size))
                return false;
            for (uint32_t i = 0; i < chunk; ++i)
                maximum = std::max(maximum, static_cast<uint32_t>(small_values[i]));
        } else {
            if (!read_guest_memory(mem, address, large_values.data(), chunk * element_size))
                return false;
            for (uint32_t i = 0; i < chunk; ++i)
                maximum = std::max(maximum, large_values[i]);
        }
        offset += chunk;
    }
    return true;
}

} // namespace renderer