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

using EtfResult = ByteResult<EtfError>;

inline constexpr u32 kEtfMaximumDepth = 128;

// Grows output to at least required bytes at the same base, keeping its contents.
using EtfGrowOutput = bool (*)(void* context, u32 required_bytes, Slice<u8>* output) noexcept;
// Grows the index to at least required entries at the same base, keeping its contents.
using EtfGrowIndex = bool (*)(void* context, u32 required_entries, Slice<u32>* index) noexcept;

// Transcodes one term (version byte 131 first) into *output, growing it through grow (optional) up to
// maximum_output bytes, and writes the JSON's structural index to `index` as json_scan_structurals_avx1 would
// produce it, since every structural byte is written here. A full index grows through grow_index (optional,
// with the same context). *index_count is the number of structurals (~0u on error). The index holds them when
// that count is at most the last index size; the JSON is complete either way. Needs no index: pass an empty slice.
// The text is valid JSON by construction (strings escaped, invalid UTF-8 replaced, numbers canonical), so a
// reader may take it as validated, within the depth the transcoder allows (kEtfMaximumDepth).
// When the index needs at most the lower half of `index`, the upper half holds its table of bracket ends
// (etf_index_ends of `index`, also after growth).
EtfResult etf_to_json_indexed(ConstSlice<u8> etf, Slice<u8>* output, u32 maximum_output, EtfGrowOutput grow,
                              void* context, Slice<u32> index, EtfGrowIndex grow_index, u32* index_count) noexcept;

// The table of bracket ends that etf_to_json_indexed left for an index of index_count entries in `index`, in the
// form of json_index_ends. The count of levels at a closing bracket is an upper bound. Null when the index was
// too long for a table.
inline const u32* etf_index_ends(Slice<u32> index, u32 index_count) noexcept {
    return index_count <= index.count / 2 ? index.data + index.count / 2 : nullptr;
}

// Encodes one JSON command as the desktop client's erlpack does: strings and
// map keys as binaries, null/true/false as small atoms, 0-255 as small
// integers, other int32 as integers, 2^31-2^32 as small bigs, other integers
// as floats, empty arrays as nil and others as proper lists. Numbers must be
// integers. A short output writes nothing and reports output_capacity.
EtfResult etf_from_json(Utf8View json, Slice<u8> output) noexcept;

} // namespace maicord
