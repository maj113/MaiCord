#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// Erlang External Term Format on the user Gateway (encoding=etf).
//
// Received payloads become the JSON text of the value the desktop client's
// erlpack decoder produces, so the JSON dispatch layer reads them unchanged:
// binaries and atoms are strings (nil/null/true/false are literals), lists
// and tuples arrays, maps objects (atom, binary and integer keys become
// names), string-ext byte lists arrays of numbers. Integers are decimal
// strings where Discord's JSON has them: past 2^53 (snowflakes), under an
// "id", "*_id" or permission-set name, and in "*_ids" lists; other integers
// are numbers. Invalid UTF-8 in a binary becomes U+FFFD.
enum class EtfError : u8 {
    none,
    invalid_input,
    bad_version,
    truncated,
    unsupported_term,
    invalid_key,
    improper_list,
    depth_limit,
    trailing_data,
    invalid_json,
    output_capacity,
};

struct EtfResult {
    u32 bytes;
    u32 error_offset;
    EtfError error;

    constexpr bool ok() const noexcept {
        return error == EtfError::none;
    }
};

inline constexpr u32 kEtfMaximumDepth = 128;

// Grows output to at least required bytes at the same base, keeping its contents.
using EtfGrowOutput = bool (*)(void* context, u32 required_bytes, Slice<u8>* output) noexcept;

// Transcodes one term (version byte 131 first) into *output, growing it
// through grow (optional) up to maximum_output bytes.
EtfResult etf_to_json(ConstSlice<u8> etf, Slice<u8>* output, u32 maximum_output, EtfGrowOutput grow,
                      void* context) noexcept;

// Encodes one JSON command as the desktop client's erlpack does: strings and
// map keys as binaries, null/true/false as small atoms, 0-255 as small
// integers, other int32 as integers, 2^31-2^32 as small bigs, other integers
// as floats, empty arrays as nil and others as proper lists. Numbers must be
// integers. A short output writes nothing and reports output_capacity.
EtfResult etf_from_json(Utf8View json, Slice<u8> output) noexcept;

} // namespace maicord
