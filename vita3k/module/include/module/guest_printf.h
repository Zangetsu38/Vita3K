// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <module/vargs.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace module {

enum class GuestPrintfError { none,
    memory,
    unsupported,
    limit,
    formatting };

// Read one page at a time: adjacent guest pages need not be adjacent on the host.
inline GuestPrintfError read_guest_printf_string(MemState &mem, Address address, size_t limit, bool bounded, std::string &out) {
    out.clear();
    if (!address)
        return GuestPrintfError::memory;
    uint64_t current = address;
    while (out.size() < limit) {
        if (current > std::numeric_limits<Address>::max())
            return GuestPrintfError::memory;
        const Address addr = static_cast<Address>(current);
        constexpr size_t page_size = KiB(4);
        const size_t count = std::min(limit - out.size(), page_size - (addr % page_size));
        const GuestMemoryRange range{ addr, static_cast<uint32_t>(count) };
        bool terminated = false;
        if (!read_pinned_guest_memory(mem, range, [&] {
                const char *data = Ptr<const char>(addr).get(mem);
                const char *end = std::find(data, data + count, '\0');
                out.append(data, end);
                terminated = end != data + count;
            }))
            return GuestPrintfError::memory;
        if (terminated)
            return GuestPrintfError::none;
        current += count;
    }
    return bounded ? GuestPrintfError::none : GuestPrintfError::limit;
}

// The argument reader must implement bool operator()(T&) for uint32_t and uint64_t.
// This deliberately rejects unsupported conversions instead of passing guest pointers to libc.
template <typename ReadArg>
GuestPrintfError format_guest_printf(MemState &mem, Address format_address, ReadArg &&read_arg, std::string &output) {
    constexpr size_t max_text = 65536;
    constexpr unsigned max_field = 4096;
    std::string format;
    auto error = read_guest_printf_string(mem, format_address, max_text, false, format);
    if (error != GuestPrintfError::none)
        return error;
    output.clear();
    auto append = [&](std::string_view text) {
        if (text.size() > max_text - output.size())
            return false;
        output.append(text);
        return true;
    };
    auto emit = [&](const std::string &spec, auto value) {
        const int length = std::snprintf(nullptr, 0, spec.c_str(), value);
        if (length < 0)
            return GuestPrintfError::formatting;
        if (static_cast<size_t>(length) > max_text - output.size())
            return GuestPrintfError::limit;
        std::string part(static_cast<size_t>(length) + 1, '\0');
        if (std::snprintf(part.data(), part.size(), spec.c_str(), value) != length)
            return GuestPrintfError::formatting;
        part.resize(static_cast<size_t>(length));
        output += part;
        return GuestPrintfError::none;
    };
    for (size_t pos = 0; pos < format.size();) {
        if (format[pos] != '%') {
            if (!append(std::string_view(format.data() + pos, 1)))
                return GuestPrintfError::limit;
            ++pos;
            continue;
        }
        ++pos;
        if (pos < format.size() && format[pos] == '%') {
            if (!append("%"))
                return GuestPrintfError::limit;
            ++pos;
            continue;
        }
        std::string flags;
        while (pos < format.size() && std::string_view("-+ #0").find(format[pos]) != std::string_view::npos) {
            if (flags.find(format[pos]) == std::string::npos)
                flags += format[pos];
            ++pos;
        }
        auto field = [&](unsigned &value, bool &present, bool &negative) -> GuestPrintfError {
            present = false;
            negative = false;
            value = 0;
            if (pos < format.size() && format[pos] == '*') {
                ++pos;
                uint32_t raw = 0;
                if (!read_arg(raw))
                    return GuestPrintfError::memory;
                const int32_t signed_value = std::bit_cast<int32_t>(raw);
                negative = signed_value < 0;
                const int64_t magnitude = negative ? -int64_t(signed_value) : signed_value;
                if (magnitude > max_field)
                    return GuestPrintfError::limit;
                value = static_cast<unsigned>(magnitude);
                present = true;
            } else {
                while (pos < format.size() && format[pos] >= '0' && format[pos] <= '9') {
                    present = true;
                    const unsigned digit = format[pos++] - '0';
                    if (value > (max_field - digit) / 10)
                        return GuestPrintfError::limit;
                    value = value * 10 + digit;
                }
            }
            return GuestPrintfError::none;
        };
        unsigned width = 0, precision = 0;
        bool width_present = false, negative = false;
        error = field(width, width_present, negative);
        if (error != GuestPrintfError::none)
            return error;
        if (negative && flags.find('-') == std::string::npos)
            flags += '-';
        bool precision_present = false;
        if (pos < format.size() && format[pos] == '.') {
            ++pos;
            bool digits = false;
            error = field(precision, digits, negative);
            if (error != GuestPrintfError::none)
                return error;
            precision_present = !negative;
        }
        std::string length;
        if (pos < format.size() && std::string_view("hlzjt").find(format[pos]) != std::string_view::npos) {
            length += format[pos++];
            if (pos < format.size() && (length == "h" || length == "l") && format[pos] == length[0])
                length += format[pos++];
        }
        if (pos == format.size())
            return GuestPrintfError::unsupported;
        const char conversion = format[pos++];
        const bool integer = std::string_view("diuoxX").find(conversion) != std::string_view::npos;
        const bool is_signed = conversion == 'd' || conversion == 'i';
        if ((!integer && conversion != 's' && conversion != 'c') || (!integer && (!length.empty() || (conversion == 'c' && precision_present))) || (!integer && flags.find_first_not_of("-") != std::string::npos) || (integer && ((is_signed && flags.find('#') != std::string::npos) || (!is_signed && flags.find_first_of("+ ") != std::string::npos) || ((conversion == 'u') && flags.find('#') != std::string::npos))))
            return GuestPrintfError::unsupported;
        std::string spec = "%" + flags;
        if (width_present)
            spec += std::to_string(width);
        if (precision_present)
            spec += "." + std::to_string(precision);
        if (integer) {
            const bool wide = length == "ll" || length == "j";
            if (wide)
                spec += "ll";
            else if (length == "h" || length == "hh")
                spec += length;
            spec += conversion;
            if (wide) {
                uint64_t value = 0;
                if (!read_arg(value))
                    return GuestPrintfError::memory;
                error = is_signed ? emit(spec, static_cast<long long>(std::bit_cast<int64_t>(value))) : emit(spec, static_cast<unsigned long long>(value));
            } else {
                uint32_t value = 0;
                if (!read_arg(value))
                    return GuestPrintfError::memory;
                error = is_signed ? emit(spec, static_cast<int>(std::bit_cast<int32_t>(value))) : emit(spec, static_cast<unsigned int>(value));
            }
        } else {
            uint32_t value = 0;
            if (!read_arg(value))
                return GuestPrintfError::memory;
            spec += conversion;
            if (conversion == 'c') {
                error = emit(spec, static_cast<int>(value));
            } else {
                std::string text;
                if (!value) {
                    text = "(null)";
                } else if (precision_present && precision == 0) {
                    text.clear();
                } else {
                    error = read_guest_printf_string(mem, value, precision_present ? precision : max_text, precision_present, text);
                    if (error != GuestPrintfError::none)
                        return error;
                }
                error = emit(spec, text.c_str());
            }
        }
        if (error != GuestPrintfError::none)
            return error;
    }
    return GuestPrintfError::none;
}

} // namespace module
