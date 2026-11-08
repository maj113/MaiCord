#include "maicord/platform/time/tzif.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/time.hpp"

namespace maicord {
namespace {

constexpr u32 kHeaderBytes = 44;
constexpr i64 kDay = 86400;
// 0001-01-01 and 10000-01-01 in days since the epoch: the instants the reader answers for.
constexpr i64 kMinimumDays = -719162;
constexpr i64 kMaximumDays = 2932896;
// An offset the database never exceeds by far (the extremes are +14:00 and -12:00, historic local mean times a bit
// more); a value past a day and a half is a corrupt file.
constexpr i32 kOffsetLimit = 36 * 3600;

// Whole days since the epoch of an instant, rounded toward the past.
constexpr i64 floor_days(i64 seconds) noexcept {
    return seconds / kDay - (seconds % kDay < 0 ? 1 : 0);
}

struct Header {
    u32 utc_indicators, standard_indicators, leaps, times, types, chars;
};

// The counts of a header at `at`; false when the bytes end first or the magic is wrong.
TzifStatus read_header(ConstSlice<u8> data, u32 at, Header* header, u8* version) noexcept {
    if (data.count < at || data.count - at < kHeaderBytes)
        return TzifStatus::truncated;
    const u8* p = data.data + at;
    if (p[0] != 'T' || p[1] != 'Z' || p[2] != 'i' || p[3] != 'f')
        return TzifStatus::bad_magic;
    if (p[4] != 0 && (p[4] < '2' || p[4] > '4'))
        return TzifStatus::bad_version;
    *version = p[4];
    header->utc_indicators = byte_load_be32(p + 20);
    header->standard_indicators = byte_load_be32(p + 24);
    header->leaps = byte_load_be32(p + 28);
    header->times = byte_load_be32(p + 32);
    header->types = byte_load_be32(p + 36);
    header->chars = byte_load_be32(p + 40);
    return TzifStatus::ok;
}

// The bytes of a data block with `time_bytes`-wide transition times; false when the counts overflow 32 bits.
bool block_bytes(const Header& h, u32 time_bytes, u64* bytes) noexcept {
    const u64 total = u64{h.times} * time_bytes + h.times + u64{h.types} * 6 + h.chars +
                      u64{h.leaps} * (time_bytes + 4) + h.standard_indicators + h.utc_indicators;
    *bytes = total;
    return total < 0x80000000ull;
}

// A POSIX TZ parser over [text, text + length).
struct RuleReader {
    const char* text;
    u32 length;
    u32 at;

    bool done() const noexcept { return at >= length; }
    char peek() const noexcept { return at < length ? text[at] : 0; }

    bool number(u32 maximum_digits, u32* value) noexcept {
        u32 digits = 0, result = 0;
        while (at < length && text[at] >= '0' && text[at] <= '9' && digits < maximum_digits) {
            result = result * 10 + static_cast<u32>(text[at] - '0');
            ++at;
            ++digits;
        }
        *value = result;
        return digits != 0;
    }

    // [+-]hh[:mm[:ss]] as seconds; hours up to `maximum_hours`.
    bool clock(u32 maximum_hours, i32* seconds) noexcept {
        bool negative = false;
        if (peek() == '+' || peek() == '-') {
            negative = text[at] == '-';
            ++at;
        }
        u32 hours = 0, minutes = 0, secs = 0;
        if (!number(3, &hours) || hours > maximum_hours)
            return false;
        if (peek() == ':') {
            ++at;
            if (!number(2, &minutes) || minutes > 59)
                return false;
            if (peek() == ':') {
                ++at;
                if (!number(2, &secs) || secs > 59)
                    return false;
            }
        }
        const i32 value = static_cast<i32>(hours * 3600 + minutes * 60 + secs);
        *seconds = negative ? -value : value;
        return true;
    }

    // An abbreviation: three or more letters, or any characters but '>' between '<' and '>'.
    bool name(char (&output)[8]) noexcept {
        u32 count = 0;
        if (peek() == '<') {
            ++at;
            while (at < length && text[at] != '>') {
                const char c = text[at++];
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '+' ||
                      c == '-'))
                    return false;
                if (count < 7)
                    output[count] = c;
                ++count;
            }
            if (at >= length)
                return false;
            ++at;
        } else {
            while (at < length && ((text[at] >= 'A' && text[at] <= 'Z') || (text[at] >= 'a' && text[at] <= 'z'))) {
                if (count < 7)
                    output[count] = text[at];
                ++count;
                ++at;
            }
        }
        if (count < 3)
            return false;
        output[count < 7 ? count : 7] = 0;
        return true;
    }

    bool day(TzifRuleDay* rule) noexcept {
        u32 value = 0;
        if (peek() == 'M') {
            ++at;
            u32 month = 0, week = 0, weekday = 0;
            if (!number(2, &month) || month < 1 || month > 12 || peek() != '.')
                return false;
            ++at;
            if (!number(1, &week) || week < 1 || week > 5 || peek() != '.')
                return false;
            ++at;
            if (!number(1, &weekday) || weekday > 6)
                return false;
            rule->kind = 'M';
            rule->month = static_cast<u8>(month);
            rule->week = static_cast<u8>(week);
            rule->weekday = static_cast<u8>(weekday);
        } else if (peek() == 'J') {
            ++at;
            if (!number(3, &value) || value < 1 || value > 365)
                return false;
            rule->kind = 'J';
            rule->day = static_cast<u16>(value);
        } else {
            if (!number(3, &value) || value > 365)
                return false;
            rule->kind = 'N';
            rule->day = static_cast<u16>(value);
        }
        rule->seconds = 7200;
        if (peek() == '/') {
            ++at;
            if (!clock(167, &rule->seconds))
                return false;
        }
        return true;
    }
};

// The day `rule` names in `year`, as days since the epoch.
i64 rule_days(const TzifRuleDay& rule, i64 year) noexcept {
    const i64 january = civil_days_from_date(year, 1, 1);
    if (rule.kind == 'J')
        return january + rule.day - 1 + (civil_leap_year(year) && rule.day >= 60 ? 1 : 0);
    if (rule.kind == 'N')
        return january + rule.day;
    const i64 first = civil_days_from_date(year, rule.month, 1);
    // 1970-01-01 was a Thursday.
    const i64 first_weekday = ((first % 7) + 7 + 4) % 7;
    u32 day = 1 + static_cast<u32>((rule.weekday + 7 - first_weekday) % 7) + (rule.week - 1u) * 7;
    const u32 month_days = civil_month_days(year, rule.month);
    while (day > month_days)
        day -= 7;
    return first + day - 1;
}

void copy_name(const char (&name)[8], TzifOffset* output) noexcept {
    u8 length = 0;
    while (length < 7 && name[length])
        ++length;
    for (u8 i = 0; i < length; ++i)
        output->abbreviation[i] = name[i];
    output->abbreviation[length] = 0;
}

// The caller (tzif_offset_at) has checked that the instant is within the years the reader answers for.
bool rule_offset(const TzifRule& rule, i64 unix_seconds, TzifOffset* output) noexcept {
    bool daylight = false;
    if (rule.has_daylight) {
        const i64 year = civil_date_from_days(floor_days(unix_seconds + rule.standard_offset)).year;
        const i64 start = rule_days(rule.start, year) * kDay + rule.start.seconds - rule.standard_offset;
        const i64 end = rule_days(rule.end, year) * kDay + rule.end.seconds - rule.daylight_offset;
        daylight = start < end ? (unix_seconds >= start && unix_seconds < end)
                               : !(unix_seconds >= end && unix_seconds < start);
    }
    output->utc_offset = daylight ? rule.daylight_offset : rule.standard_offset;
    output->daylight = daylight;
    copy_name(daylight ? rule.daylight_name : rule.standard_name, output);
    return true;
}

i64 time_at(const TzifZone& zone, u32 index) noexcept {
    return zone.time_bytes == 8 ? static_cast<i64>(byte_load_be64(zone.times + index * 8))
                                : static_cast<i32>(byte_load_be32(zone.times + index * 4));
}

bool type_offset(const TzifZone& zone, u32 type, TzifOffset* output) noexcept {
    if (type >= zone.type_count)
        return false;
    const u8* record = zone.types + type * 6;
    output->utc_offset = static_cast<i32>(byte_load_be32(record));
    output->daylight = record[4] != 0;
    u32 at = record[5], length = 0;
    while (at < zone.char_count && zone.chars[at] != 0 && length < 7)
        output->abbreviation[length++] = static_cast<char>(zone.chars[at++]);
    output->abbreviation[length] = 0;
    return true;
}

} // namespace

TzifStatus tzif_open(ConstSlice<u8> data, TzifZone* zone) noexcept {
    if (!zone || !data.data)
        return TzifStatus::truncated;
    *zone = {};
    Header header{};
    u8 version = 0;
    TzifStatus status = read_header(data, 0, &header, &version);
    if (status != TzifStatus::ok)
        return status;
    u32 body = kHeaderBytes;
    u32 time_bytes = 4;
    u64 bytes = 0;
    if (!block_bytes(header, 4, &bytes))
        return TzifStatus::bad_counts;
    if (version >= '2') {
        // The version 1 block is skipped; the second header and block carry the 64-bit data and the footer.
        if (data.count - kHeaderBytes < bytes)
            return TzifStatus::truncated;
        const u32 second = kHeaderBytes + static_cast<u32>(bytes);
        u8 second_version = 0;
        status = read_header(data, second, &header, &second_version);
        if (status != TzifStatus::ok)
            return status;
        body = second + kHeaderBytes;
        time_bytes = 8;
        if (!block_bytes(header, 8, &bytes))
            return TzifStatus::bad_counts;
    }
    if (header.types == 0 || header.types > 256 || header.chars == 0 || header.chars > 1024 || header.times > 65536)
        return TzifStatus::bad_counts;
    if (data.count - body < bytes)
        return TzifStatus::truncated;
    const u8* p = data.data + body;
    zone->time_bytes = time_bytes;
    zone->time_count = header.times;
    zone->type_count = header.types;
    zone->char_count = header.chars;
    zone->times = p;
    zone->indices = p + header.times * time_bytes;
    zone->types = zone->indices + header.times;
    zone->chars = zone->types + header.types * 6;
    for (u32 i = 0; i < header.times; ++i) {
        if (zone->indices[i] >= header.types || (i != 0 && time_at(*zone, i) <= time_at(*zone, i - 1)))
            return TzifStatus::bad_data;
    }
    for (u32 i = 0; i < header.types; ++i) {
        const u8* record = zone->types + i * 6;
        const i32 offset = static_cast<i32>(byte_load_be32(record));
        if (offset > kOffsetLimit || offset < -kOffsetLimit || record[4] > 1 || record[5] >= header.chars)
            return TzifStatus::bad_data;
    }
    if (version >= '2') {
        // "\n" POSIX string "\n" after the block.
        const u32 footer = body + static_cast<u32>(bytes);
        if (data.count - footer >= 2 && data.data[footer] == '\n') {
            u32 end = footer + 1;
            while (end < data.count && data.data[end] != '\n')
                ++end;
            if (end >= data.count)
                return TzifStatus::truncated;
            const u32 length = end - footer - 1;
            if (length != 0) {
                if (!tzif_parse_rule(reinterpret_cast<const char*>(data.data + footer + 1), length, &zone->rule))
                    return TzifStatus::bad_footer;
                zone->has_rule = true;
            }
        } else if (data.count - footer != 0) {
            return TzifStatus::bad_footer;
        }
    }
    return TzifStatus::ok;
}

bool tzif_parse_rule(const char* text, u32 length, TzifRule* rule) noexcept {
    if (!text || !rule)
        return false;
    *rule = {};
    RuleReader reader{text, length, 0};
    i32 seconds = 0;
    if (!reader.name(rule->standard_name) || !reader.clock(24, &seconds))
        return false;
    rule->standard_offset = -seconds;
    rule->daylight_offset = rule->standard_offset;
    if (reader.done())
        return true;
    if (!reader.name(rule->daylight_name))
        return false;
    rule->has_daylight = true;
    rule->daylight_offset = rule->standard_offset + 3600;
    if (reader.peek() != ',' && !reader.done()) {
        if (!reader.clock(24, &seconds))
            return false;
        rule->daylight_offset = -seconds;
    }
    if (reader.done()) {
        // POSIX leaves the default rules to the implementation; this is the United States rule glibc uses.
        const char defaults[] = "M3.2.0,M11.1.0";
        RuleReader fallback{defaults, sizeof(defaults) - 1, 0};
        return fallback.day(&rule->start) && fallback.peek() == ',' && (++fallback.at, fallback.day(&rule->end)) &&
               fallback.done();
    }
    if (reader.peek() != ',')
        return false;
    ++reader.at;
    if (!reader.day(&rule->start) || reader.peek() != ',')
        return false;
    ++reader.at;
    return reader.day(&rule->end) && reader.done();
}

void tzif_zone_from_rule(const TzifRule& rule, TzifZone* zone) noexcept {
    *zone = {};
    zone->has_rule = true;
    zone->rule = rule;
}

bool tzif_offset_at(const TzifZone& zone, i64 unix_seconds, TzifOffset* output) noexcept {
    if (!output)
        return false;
    const i64 days = floor_days(unix_seconds);
    if (days < kMinimumDays || days > kMaximumDays)
        return false;
    if (zone.time_count == 0) {
        if (zone.has_rule)
            return rule_offset(zone.rule, unix_seconds, output);
        return zone.type_count != 0 && type_offset(zone, 0, output);
    }
    if (unix_seconds < time_at(zone, 0))
        return type_offset(zone, 0, output);
    if (zone.has_rule && unix_seconds >= time_at(zone, zone.time_count - 1))
        return rule_offset(zone.rule, unix_seconds, output);
    // The last transition at or before the instant.
    u32 low = 0, high = zone.time_count - 1;
    while (low < high) {
        const u32 middle = low + (high - low + 1) / 2;
        if (time_at(zone, middle) <= unix_seconds)
            low = middle;
        else
            high = middle - 1;
    }
    return type_offset(zone, zone.indices[low], output);
}

bool tzif_local_to_utc(const TzifZone& zone, i64 local_seconds, i64* unix_seconds) noexcept {
    if (!unix_seconds)
        return false;
    // The offsets in force a day either side bracket the one the reading belongs to: no zone changes twice in two days
    // by more than a day, and a transition inside the day maps to one of the two.
    TzifOffset before{}, after{};
    if (!tzif_offset_at(zone, local_seconds - kDay, &before) || !tzif_offset_at(zone, local_seconds + kDay, &after))
        return false;
    const i64 first = local_seconds - before.utc_offset;
    const i64 second = local_seconds - after.utc_offset;
    TzifOffset at{};
    const bool first_ok = tzif_offset_at(zone, first, &at) && at.utc_offset == before.utc_offset;
    const bool second_ok = tzif_offset_at(zone, second, &at) && at.utc_offset == after.utc_offset;
    if (first_ok && second_ok)
        *unix_seconds = first < second ? first : second;
    else if (second_ok)
        *unix_seconds = second;
    else
        *unix_seconds = first;
    return true;
}

bool tzif_name_valid(const char* name, u32 length) noexcept {
    if (!name || length == 0 || length > 63 || name[0] == '/' || name[length - 1] == '/')
        return false;
    for (u32 i = 0; i < length; ++i) {
        const char c = name[i];
        // No '.', so no "." or ".." component; and no empty component.
        if (c == '/' ? name[i - 1] == '/' : !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                                              (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '+'))
            return false;
    }
    return true;
}

u32 tzif_name_from_path(const char* path, char* output, u32 capacity) noexcept {
    if (!path || !output || capacity == 0)
        return 0;
    const u32 length = c_string_length(path);
    static constexpr char marker[] = "zoneinfo/";
    constexpr u32 marker_length = sizeof(marker) - 1;
    // The last "zoneinfo/" so that ".../zoneinfo/posix/Europe/Berlin" is "posix/Europe/Berlin", which is still valid.
    u32 found = length;
    for (u32 i = 0; i + marker_length <= length; ++i) {
        if (bytes_equal(path + i, marker, marker_length) && (i == 0 || path[i - 1] == '/'))
            found = i + marker_length;
    }
    if (found >= length || !tzif_name_valid(path + found, length - found) || length - found >= capacity)
        return 0;
    for (u32 i = 0; i < length - found; ++i)
        output[i] = path[found + i];
    output[length - found] = 0;
    return length - found;
}

} // namespace maicord
