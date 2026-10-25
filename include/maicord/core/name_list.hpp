#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// A list of names in one string literal, each followed by "\0"; the literal's
// own terminator ends the list ("dark\0light\0"). Compared with a Utf8View
// table it spends no pointer, length or relocation per entry. Names are never
// empty. Walk it once per lookup; the lists are short and looked up on cold
// paths.
struct NameList {
    const char* at;
    u32 bytes;
    constexpr explicit NameList(const char* names) noexcept : at(names), bytes(length(names)) {}
    constexpr explicit operator bool() const noexcept {
        return bytes != 0;
    }
    constexpr Utf8View name() const noexcept {
        return {at, bytes};
    }
    constexpr void next() noexcept {
        at += bytes + 1;
        bytes = length(at);
    }

private:
    static constexpr u32 length(const char* text) noexcept {
        u32 count{};
        while (text[count])
            ++count;
        return count;
    }
};

constexpr u32 name_list_count(const char* names) noexcept {
    u32 count{};
    for (NameList list{names}; list; list.next())
        ++count;
    return count;
}

// The index-th name; empty past the end.
constexpr Utf8View name_list_at(const char* names, u32 index) noexcept {
    NameList list{names};
    for (; list && index; --index)
        list.next();
    return list.name();
}

} // namespace maicord
