// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mem/functions.h>

#include <cstdint>
#include <limits>
#include <map>

// The owner must serialize calls and clear each address space before destroying it.
class ProcessLocalStorage {
public:
    Address create(MemState &mem, uint32_t size, const char *name) {
        if (!size || size > std::numeric_limits<uint32_t>::max() - 0xFFF)
            return 0;
        const Address address = alloc(mem, size, name);
        if (!address)
            return 0;
        blocks[&mem].emplace(address >> 1, Block(address, [&mem](Address ptr) { free(mem, ptr); }));
        return address;
    }

    Address find(const MemState &mem, uint32_t key) const {
        const auto process = blocks.find(&mem);
        if (process == blocks.end())
            return 0;
        const auto block = process->second.find(key);
        return block == process->second.end() ? 0 : block->second.get();
    }

    void clear(MemState &mem) { blocks.erase(&mem); }
    void clear() { blocks.clear(); }

private:
    std::map<const MemState *, std::map<uint32_t, Block>> blocks;
};