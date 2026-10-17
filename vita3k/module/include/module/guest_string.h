// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mem/functions.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace module {

enum class GuestStringError { none,
    memory,
    no_memory };

inline GuestStringError bounded_guest_string_length(MemState &mem, Address start, uint32_t limit, uint32_t &length) {
    length = 0;
    std::array<uint8_t, 4096> bytes{};
    while (length < limit) {
        const uint64_t current = uint64_t(start) + length;
        if (current > std::numeric_limits<Address>::max())
            return GuestStringError::memory;
        const uint32_t count = std::min(limit - length, static_cast<uint32_t>(bytes.size() - current % bytes.size()));
        if (!read_guest_memory(mem, static_cast<Address>(current), bytes.data(), count))
            return GuestStringError::memory;
        const auto *end = static_cast<const uint8_t *>(std::memchr(bytes.data(), 0, count));
        if (end) {
            length += static_cast<uint32_t>(end - bytes.data());
            return GuestStringError::none;
        }
        length += count;
    }
    return GuestStringError::none;
}

inline GuestStringError find_guest_char(MemState &mem, Address start, int character, bool reverse, Address &result) {
    result = 0;
    uint64_t current = start;
    const uint8_t target = static_cast<uint8_t>(character);
    std::array<uint8_t, 4096> bytes{};
    while (current <= std::numeric_limits<Address>::max()) {
        const auto address = static_cast<Address>(current);
        const auto count = static_cast<uint32_t>(bytes.size() - address % bytes.size());
        if (!read_guest_memory(mem, address, bytes.data(), count))
            return GuestStringError::memory;
        for (uint32_t i = 0; i < count; ++i) {
            if (bytes[i] == target) {
                result = address + i;
                if (!reverse)
                    return GuestStringError::none;
            }
            if (!bytes[i])
                return GuestStringError::none;
        }
        current += count;
    }
    return GuestStringError::memory;
}

inline GuestStringError compare_guest_case_insensitive(MemState &mem, Address left, Address right, uint32_t length, int &result) {
    result = 0;
    uint64_t a = left;
    uint64_t b = right;
    std::array<uint8_t, 4096> first{};
    std::array<uint8_t, 4096> second{};
    while (length) {
        if (a > std::numeric_limits<Address>::max() || b > std::numeric_limits<Address>::max())
            return GuestStringError::memory;
        const auto count = static_cast<uint32_t>(std::min({ static_cast<uint64_t>(length), first.size() - a % first.size(), second.size() - b % second.size() }));
        if (!read_guest_memory(mem, static_cast<Address>(a), first.data(), count)
            || !read_guest_memory(mem, static_cast<Address>(b), second.data(), count))
            return GuestStringError::memory;
        for (uint32_t i = 0; i < count; ++i) {
            const int lower_left = std::tolower(first[i]);
            const int lower_right = std::tolower(second[i]);
            if (lower_left != lower_right) {
                result = lower_left - lower_right;
                return GuestStringError::none;
            }
            if (!first[i])
                return GuestStringError::none;
        }
        a += count;
        b += count;
        length -= count;
    }
    return GuestStringError::none;
}

inline GuestStringError find_guest_substring(MemState &mem, Address haystack, Address needle, Address &result) {
    result = 0;
    try {
        std::string pattern;
        std::array<char, 4096> bytes{};
        uint64_t current = needle;
        bool terminated = false;
        while (current <= std::numeric_limits<Address>::max()) {
            const auto address = static_cast<Address>(current);
            const auto count = static_cast<uint32_t>(bytes.size() - address % bytes.size());
            if (!read_guest_memory(mem, address, bytes.data(), count))
                return GuestStringError::memory;
            const auto *end = static_cast<const char *>(std::memchr(bytes.data(), 0, count));
            pattern.append(bytes.data(), end ? static_cast<size_t>(end - bytes.data()) : count);
            if (end) {
                terminated = true;
                break;
            }
            current += count;
        }
        if (!terminated)
            return GuestStringError::memory;
        if (pattern.empty()) {
            result = haystack;
            return GuestStringError::none;
        }
        std::vector<size_t> prefix(pattern.size());
        for (size_t i = 1, matched = 0; i < pattern.size(); ++i) {
            while (matched && pattern[i] != pattern[matched])
                matched = prefix[matched - 1];
            if (pattern[i] == pattern[matched])
                ++matched;
            prefix[i] = matched;
        }
        current = haystack;
        size_t matched = 0;
        while (current <= std::numeric_limits<Address>::max()) {
            const auto address = static_cast<Address>(current);
            const auto count = static_cast<uint32_t>(bytes.size() - address % bytes.size());
            if (!read_guest_memory(mem, address, bytes.data(), count))
                return GuestStringError::memory;
            for (uint32_t i = 0; i < count; ++i) {
                if (!bytes[i])
                    return GuestStringError::none;
                while (matched && bytes[i] != pattern[matched])
                    matched = prefix[matched - 1];
                if (bytes[i] == pattern[matched])
                    ++matched;
                if (matched == pattern.size()) {
                    result = static_cast<Address>(current + i + 1 - pattern.size());
                    return GuestStringError::none;
                }
            }
            current += count;
        }
        return GuestStringError::memory;
    } catch (const std::bad_alloc &) {
        return GuestStringError::no_memory;
    } catch (const std::length_error &) {
        return GuestStringError::no_memory;
    }
}

} // namespace module