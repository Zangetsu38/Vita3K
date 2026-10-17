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

#include "args_layout.h"
#include "lay_out_args.h"
#include "read_arg.h"

#include <mem/functions.h>
#include <mem/ptr.h>

#include <limits>
#include <span>

namespace module {
template <typename Reader>
bool read_pinned_guest_memory(MemState &mem, const GuestMemoryRange &range, Reader &&reader) {
    const uint64_t pin = pin_guest_memory(mem, std::span(&range, 1));
    if (!pin)
        return false;
    struct PinGuard {
        MemState &mem;
        uint64_t pin;
        ~PinGuard() { unpin_guest_memory(mem, pin); }
    } guard{ mem, pin };
    const std::lock_guard lock(mem.protect_mutex);
    const uint64_t end = uint64_t(range.address) + range.size;
    for (const auto &[start, segment] : mem.protect_tree) {
        if (segment.perm == MemPerm::None && uint64_t(start) < end && uint64_t(start) + segment.size > range.address)
            return false;
    }
    reader();
    return true;
}

class vargs {
    Address currentVaList;
    LayoutArgsState layoutState;
    bool checked = false;
    bool read_valid = true;

    template <typename T>
    T read_checked(MemState &mem, uint64_t address) {
        if (address > std::numeric_limits<Address>::max() || sizeof(T) - 1 > std::numeric_limits<Address>::max() - address) {
            read_valid = false;
            return T{};
        }
        const GuestMemoryRange range{ static_cast<Address>(address), sizeof(T) };
        T value{};
        if (!read_pinned_guest_memory(mem, range, [&] {
                auto *bytes = reinterpret_cast<unsigned char *>(&value);
                for (size_t i = 0; i < sizeof(T); ++i)
                    bytes[i] = *Ptr<const unsigned char>(range.address + static_cast<Address>(i)).get(mem);
            })) {
            read_valid = false;
            return T{};
        }
        return value;
    }

public:
    vargs() = default;

    explicit vargs(LayoutArgsState layoutState)
        : currentVaList(0)
        , layoutState(layoutState) {
    }

    explicit vargs(Address addr)
        : currentVaList(addr)
        , layoutState() {
    }

    void enable_checked_reads() { checked = true; }
    bool checked_reads() const { return checked; }
    bool valid() const { return read_valid; }

    template <typename T>
    T next(CPUState &cpu, MemState &mem) {
        if (!currentVaList) {
            const auto state_tuple = add_arg_to_layout<T>(layoutState);

            layoutState = std::move(std::get<1>(state_tuple));
            ArgLayout currentLayout = std::move(std::get<0>(state_tuple));

            if (checked && currentLayout.location == ArgLocation::stack)
                return read_checked<T>(mem, uint64_t(cpu.get_sp()) + currentLayout.offset);
            return read<T>(cpu, currentLayout, mem);
        } else {
            if (checked) {
                const T out = read_checked<T>(mem, currentVaList);
                if (uint64_t(currentVaList) + sizeof(T) > std::numeric_limits<Address>::max())
                    read_valid = false;
                else
                    currentVaList += sizeof(T);
                return out;
            }
            const auto out = *Ptr<T>(currentVaList).get(mem);
            currentVaList += sizeof(T);
            return out;
        }
    }
};

} // namespace module
