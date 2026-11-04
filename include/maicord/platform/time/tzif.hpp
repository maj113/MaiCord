#pragma once

#include "maicord/core/slice.hpp"

// A reader of the IANA time zone database format (TZif, RFC 8536) and of POSIX TZ rules, over bytes the caller holds.
// Pure functions: no allocation, no files, no clock, so the same tests run on both systems with fixture files.
//
// A zone gives the offset from UTC at any UTC instant (tzif_offset_at) and the UTC instant of a local wall-clock
// reading (tzif_local_to_utc). Instants are seconds since 1970-01-01T00:00:00Z; a "local reading" is the wall clock
// as if it were UTC (seconds of the local date and time counted from the same epoch). Leap seconds are ignored (a
// "right/" zone reads as its POSIX-time neighbour). Instants outside the years 1 to 9999 are refused.
//
// Instants before the first transition have the first time type (RFC 8536 section 3.2), after the last one the footer's
// POSIX rule applies (a file without a footer keeps its last type), as in the reference implementations.

namespace maicord {

enum class TzifStatus : u8 {
    ok,
    truncated,   // the bytes end inside the header, a block or the footer
    bad_magic,   // not "TZif"
    bad_version, // a version this reader does not know
    bad_counts,  // zero types, or counts that do not fit the file
    bad_data,    // transition times out of order, a type index or abbreviation index out of range, an offset beyond a day and a half
    bad_footer,  // the footer is not a POSIX TZ string
};

// One day of a POSIX rule: Jn (1 to 365, February 29 never counted), n (0 to 365, February 29 counted) or Mm.w.d
// (month 1 to 12, week 1 to 5 with 5 the last, weekday 0 = Sunday).
struct TzifRuleDay {
    u8 kind;        // 'J', 'N' (plain day number) or 'M'
    u8 month, week, weekday;
    u16 day;        // the J or n number
    i32 seconds;    // time of day, seconds after local midnight; may be negative or exceed a day (RFC 9636's range)
};

// A POSIX TZ string: "EST5EDT,M3.2.0,M11.1.0", "<+0330>-3:30", "UTC0".
struct TzifRule {
    char standard_name[8];
    char daylight_name[8];
    i32 standard_offset; // seconds east of UTC (the string's sign is the opposite)
    i32 daylight_offset; // seconds east of UTC; equals the standard offset without daylight time
    bool has_daylight;
    TzifRuleDay start;   // daylight time begins, in standard time
    TzifRuleDay end;     // daylight time ends, in daylight time
};

// What a zone says about one instant.
struct TzifOffset {
    i32 utc_offset; // seconds east of UTC
    bool daylight;
    u8 abbreviation_length;
    char abbreviation[8]; // NUL-terminated, longer names cut
};

// An opened zone: views into the caller's bytes, which must outlive it.
struct TzifZone {
    const u8* times;   // time_count transition times, big endian, time_bytes each
    const u8* indices; // time_count type indices
    const u8* types;   // type_count records of 6 bytes: offset (i32), is_dst, abbreviation index
    const u8* chars;   // abbreviation characters
    u32 time_count;
    u32 type_count;
    u32 char_count;
    u32 time_bytes;    // 4 (version 1 data) or 8
    bool has_rule;     // a footer (or a rule given to tzif_zone_from_rule)
    TzifRule rule;
};

// Opens a TZif file's bytes: the version 2 or later block when there is one, else version 1's.
TzifStatus tzif_open(ConstSlice<u8> data, TzifZone* zone) noexcept;

// Parses a POSIX TZ string (no leading ':'). True when the whole text is one valid string. A daylight name without
// rules gets the POSIX default, M3.2.0,M11.1.0.
bool tzif_parse_rule(const char* text, u32 length, TzifRule* rule) noexcept;

// A zone that is only a rule ($TZ="EST5EDT,M3.2.0,M11.1.0").
void tzif_zone_from_rule(const TzifRule& rule, TzifZone* zone) noexcept;

// The offset at a UTC instant. False for an instant outside the years 1 to 9999 and for a zone with no types.
bool tzif_offset_at(const TzifZone& zone, i64 unix_seconds, TzifOffset* output) noexcept;

// The UTC instant of a local reading. A reading that happens twice (the clocks went back) is the first, before the
// change; one that never happens (the clocks went forward) is read with the offset before the change, so it lands
// the length of the gap later on the wall clock. False for a reading outside the years 1 to 9999.
bool tzif_local_to_utc(const TzifZone& zone, i64 local_seconds, i64* unix_seconds) noexcept;

// The zone identifier a path names: the part after "zoneinfo/" ("/usr/share/zoneinfo/Europe/Berlin" is
// "Europe/Berlin"), copied into output with its NUL; zero when the path has no such part or the identifier is not
// made of letters, digits and "/_-+" or does not fit.
u32 tzif_name_from_path(const char* path, char* output, u32 capacity) noexcept;

// Whether `name` is a zone identifier that is safe to put after a zoneinfo directory: no empty, "." or ".."
// component, no leading '/', only letters, digits and "/_-+".
bool tzif_name_valid(const char* name, u32 length) noexcept;

} // namespace maicord
