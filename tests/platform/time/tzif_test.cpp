// The TZif reader (src/platform/time/tzif.cpp) against real zones: zic's slim and fat output for 14 zones (no DST, 30 and
// 45 minute zones, +14:00, a 30 minute DST step, southern and negative DST, a date line jump, tables that end and
// footers that carry on) and what Python's zoneinfo says about the same bytes at every transition and at spread
// instants (tools/tzif_fixtures.py); plus version 1 data, files without a footer, POSIX rule strings computed by hand,
// the zone name helpers, and truncated and bit-flipped files. Pure: the same test runs on Windows and Linux.
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/time/tzif.hpp"

#include "../../support/test_output.hpp"

namespace maicord {
#include "../../generated/tzif_fixture.inc"
namespace {

u32 g_checks = 0;
u32 g_failures = 0;

void check(bool condition, const char* name) noexcept {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        test_write_text("FAIL ");
        test_write_line(name);
    }
}

u32 le32(const u8* p) noexcept {
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<u32>(p[3]) << 24;
}
i64 le64(const u8* p) noexcept {
    return static_cast<i64>(static_cast<u64>(le32(p + 4)) << 32 | le32(p));
}
u32 be32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

constexpr i64 utc(i64 year, u32 month, u32 day, i64 hour = 0, i64 minute = 0, i64 second = 0) noexcept {
    return civil_days_from_date(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

struct Entry {
    char name[48];
    bool fat;
    const u8* data;
    u32 size;
};
constexpr u32 kEntries = 24;
Entry g_entries[kEntries];
u32 g_entry_count = 0;

void copy_name(char (&to)[48], const u8* from, u32 length) noexcept {
    for (u32 i = 0; i < length && i < 47; ++i)
        to[i] = static_cast<char>(from[i]);
    to[length < 47 ? length : 47] = 0;
}

bool parse_fixture() noexcept {
    const u8* p = tzif_fixture;
    if (sizeof(tzif_fixture) < 8 || !bytes_equal(p, "TZFX", 4))
        return false;
    const u32 count = le32(p + 4);
    u32 at = 8;
    for (u32 i = 0; i < count && i < kEntries; ++i) {
        Entry& entry = g_entries[g_entry_count++];
        const u32 length = tzif_fixture[at++];
        copy_name(entry.name, tzif_fixture + at, length);
        at += length;
        entry.fat = tzif_fixture[at++] != 0;
        entry.size = le32(tzif_fixture + at);
        at += 4;
        entry.data = tzif_fixture + at;
        at += entry.size;
    }
    return g_entry_count == count && at == sizeof(tzif_fixture);
}

bool names_equal(const char* a, const char* b) noexcept {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

const Entry* find(const char* name, bool fat) noexcept {
    for (u32 i = 0; i < g_entry_count; ++i)
        if (names_equal(g_entries[i].name, name) && g_entries[i].fat == fat)
            return &g_entries[i];
    return nullptr;
}

void describe(const char* what, const Entry& entry, i64 at) noexcept {
    test_write_text("  ");
    test_write_text(what);
    test_write_text(" ");
    test_write_text(entry.name);
    test_write_text(entry.fat ? " fat at " : " slim at ");
    test_write_text(at < 0 ? "-" : "");
    test_write_u64(static_cast<u64>(at < 0 ? -at : at));
    test_write_text("\n");
}

// The oracle against one opened zone. `from` and `to` bound the instants compared (the version 1 data covers part).
void compare_oracle(const Entry& entry, const TzifZone& zone, i64 from, i64 to, u32* compared,
                    const char* label) noexcept {
    const u8* p = tzif_oracle + 8;
    u32 reported = 0;
    for (u32 z = 0; z < g_entry_count; ++z) {
        const u32 length = *p++;
        char name[48];
        copy_name(name, p, length);
        p += length;
        const bool fat = *p++ != 0;
        const u32 instants = le32(p);
        p += 4;
        const bool mine = names_equal(name, entry.name) && fat == entry.fat;
        for (u32 i = 0; i < instants; ++i, p += 16) {
            const i64 t = le64(p);
            if (!mine || t < from || t > to)
                continue;
            const i32 offset = static_cast<i32>(le32(p + 8));
            const u32 flags = le32(p + 12);
            TzifOffset got{};
            ++*compared;
            const bool same = tzif_offset_at(zone, t, &got) && got.utc_offset == offset &&
                              (!(flags & 2) || got.daylight == ((flags & 1) != 0));
            ++g_checks;
            if (!same) {
                ++g_failures;
                if (reported++ < 5)
                    describe(label, entry, t);
            }
        }
        const u32 readings = le32(p);
        p += 4;
        for (u32 i = 0; i < readings; ++i, p += 16) {
            const i64 local = le64(p);
            if (!mine || local < from || local > to)
                continue;
            i64 got = 0;
            ++*compared;
            const bool same = tzif_local_to_utc(zone, local, &got) && got == le64(p + 8);
            ++g_checks;
            if (!same) {
                ++g_failures;
                if (reported++ < 5)
                    describe("local reading", entry, local);
            }
        }
    }
}

void test_fixture_zones() noexcept {
    check(parse_fixture() && g_entry_count == 18, "fixture_parses");
    u32 compared = 0;
    for (u32 i = 0; i < g_entry_count; ++i) {
        const Entry& entry = g_entries[i];
        TzifZone zone{};
        const bool opened = tzif_open({entry.data, entry.size}, &zone) == TzifStatus::ok;
        check(opened, "fixture_zone_opens");
        if (!opened)
            continue;
        check(zone.time_bytes == 8 && zone.has_rule && zone.type_count >= 1, "fixture_zone_is_version_2_with_footer");
        compare_oracle(entry, zone, -0x7fffffffffffffffll, 0x7fffffffffffffffll, &compared, "mismatch");
    }
    check(compared > 4000, "oracle_covers_thousands_of_instants");
    test_write_metric("oracle_compared=", compared);
}

u32 version_1_bytes(const u8* data) noexcept {
    return be32(data + 32) * 4 + be32(data + 32) + be32(data + 36) * 6 + be32(data + 40) + be32(data + 28) * 8 +
           be32(data + 24) + be32(data + 20);
}

void test_version_1_and_no_footer() noexcept {
    static constexpr const char* names[] = {"America/New_York", "Europe/Berlin", "Australia/Lord_Howe",
                                            "America/Sao_Paulo"};
    for (const char* name : names) {
        const Entry* entry = find(name, true);
        check(entry != nullptr, "fat_entry_exists");
        if (!entry)
            continue;
        static u8 copy[8192];
        // Version 1 only: the first header and block, the version byte cleared.
        const u32 v1 = 44 + version_1_bytes(entry->data);
        for (u32 i = 0; i < v1; ++i)
            copy[i] = entry->data[i];
        copy[4] = 0;
        TzifZone zone{};
        check(tzif_open({copy, v1}, &zone) == TzifStatus::ok && zone.time_bytes == 4 && !zone.has_rule,
              "version_1_opens");
        u32 compared = 0;
        // 32-bit data cover 1901 to the end of 2037 only.
        compare_oracle(*entry, zone, utc(1920, 1, 1), utc(2037, 1, 1), &compared, "version 1 mismatch");
        check(compared > 100, "version_1_compared");
        // Version 2 data without a footer: the last type carries on.
        const u32 second = 44 + version_1_bytes(entry->data);
        const u32 v2 = second + 44 + be32(entry->data + second + 32) * 9 + be32(entry->data + second + 36) * 6 +
                       be32(entry->data + second + 40) + be32(entry->data + second + 28) * 12 +
                       be32(entry->data + second + 24) + be32(entry->data + second + 20);
        for (u32 i = 0; i < v2; ++i)
            copy[i] = entry->data[i];
        check(tzif_open({copy, v2}, &zone) == TzifStatus::ok && zone.time_bytes == 8 && !zone.has_rule,
              "no_footer_opens");
        TzifOffset last{}, far{};
        check(tzif_offset_at(zone, utc(2037, 12, 31), &last) && tzif_offset_at(zone, utc(2090, 7, 1), &far) &&
                  far.utc_offset == last.utc_offset && far.daylight == last.daylight,
              "no_footer_keeps_the_last_type");
    }
}

bool rule_is(const char* text, i64 at, i32 offset, bool daylight, const char* abbreviation = nullptr) noexcept {
    TzifRule rule{};
    TzifOffset got{};
    if (!tzif_parse_rule(text, c_string_length(text), &rule))
        return false;
    TzifZone zone{};
    tzif_zone_from_rule(rule, &zone);
    return tzif_offset_at(zone, at, &got) && got.utc_offset == offset && got.daylight == daylight &&
           (!abbreviation || names_equal(got.abbreviation, abbreviation));
}

void test_rules() noexcept {
    // The United States rule, with and without the written rules.
    static constexpr const char* us_rules[] = {"EST5EDT,M3.2.0,M11.1.0", "EST5EDT"};
    for (const char* text : us_rules) {
        check(rule_is(text, utc(2024, 3, 10, 6, 59, 59), -18000, false, "EST"), "us_before_start");
        check(rule_is(text, utc(2024, 3, 10, 7), -14400, true, "EDT"), "us_start");
        check(rule_is(text, utc(2024, 11, 3, 5, 59, 59), -14400, true, "EDT"), "us_before_end");
        check(rule_is(text, utc(2024, 11, 3, 6), -18000, false, "EST"), "us_end");
        check(rule_is(text, utc(2024, 7, 4, 12), -14400, true), "us_summer");
        check(rule_is(text, utc(2024, 1, 4, 12), -18000, false), "us_winter");
    }
    // The European rule, a transition at 03:00 summer time.
    const char* europe = "CET-1CEST,M3.5.0,M10.5.0/3";
    check(rule_is(europe, utc(2024, 3, 31, 0, 59, 59), 3600, false, "CET"), "eu_before_start");
    check(rule_is(europe, utc(2024, 3, 31, 1), 7200, true, "CEST"), "eu_start");
    check(rule_is(europe, utc(2024, 10, 27, 0, 59, 59), 7200, true), "eu_before_end");
    check(rule_is(europe, utc(2024, 10, 27, 1), 3600, false), "eu_end");
    // Southern hemisphere: daylight time across the new year.
    const char* australia = "AEST-10AEDT,M10.1.0,M4.1.0/3";
    check(rule_is(australia, utc(2024, 10, 5, 15, 59, 59), 36000, false), "au_before_start");
    check(rule_is(australia, utc(2024, 10, 5, 16), 39600, true), "au_start");
    check(rule_is(australia, utc(2024, 1, 15, 0), 39600, true), "au_january");
    check(rule_is(australia, utc(2024, 4, 6, 15, 59, 59), 39600, true), "au_before_end");
    check(rule_is(australia, utc(2024, 4, 6, 16), 36000, false), "au_end");
    // No daylight time; quoted names; a half hour.
    check(rule_is("<+0330>-3:30", utc(2024, 7, 1), 12600, false, "+0330"), "quoted_name_half_hour");
    check(rule_is("UTC0", utc(2024, 7, 1), 0, false, "UTC"), "utc");
    check(rule_is("<-03>3", utc(2024, 7, 1), -10800, false, "-03"), "quoted_negative");
    // Negative daylight offset (Ireland): IST is the standard time and GMT the "daylight" one.
    const char* ireland = "IST-1GMT0,M10.5.0,M3.5.0/1";
    check(rule_is(ireland, utc(2024, 3, 31, 0, 59, 59), 0, true), "ie_gmt_before_end");
    check(rule_is(ireland, utc(2024, 3, 31, 1), 3600, false), "ie_summer");
    check(rule_is(ireland, utc(2024, 10, 27, 0, 59, 59), 3600, false), "ie_before_start");
    check(rule_is(ireland, utc(2024, 10, 27, 1), 0, true), "ie_winter");
    // Times of day past a day or below zero.
    const char* nuuk = "<-02>2<-01>,M3.5.0/-1,M10.5.0/0";
    check(rule_is(nuuk, utc(2024, 3, 31, 0, 59, 59), -7200, false), "nuuk_before_start");
    check(rule_is(nuuk, utc(2024, 3, 31, 1), -3600, true), "nuuk_start");
    check(rule_is(nuuk, utc(2024, 10, 27, 0, 59, 59), -3600, true), "nuuk_before_end");
    check(rule_is(nuuk, utc(2024, 10, 27, 1), -7200, false), "nuuk_end");
    // Day numbers: J never counts February 29, a plain number does. Rule at 00:00 on a zone at UTC+0 (+1 summer).
    static constexpr i64 years[] = {2023, 2024};
    for (const i64 year : years) {
        const i64 march_first = utc(year, 3, 1);
        check(rule_is("AAA0BBB,J60/0,J300/0", march_first - 1, 0, false), "j60_before");
        check(rule_is("AAA0BBB,J60/0,J300/0", march_first, 3600, true), "j60_is_march_first");
    }
    check(rule_is("AAA0BBB,59/0,300/0", utc(2024, 2, 29) - 1, 0, false), "n59_leap_before");
    check(rule_is("AAA0BBB,59/0,300/0", utc(2024, 2, 29), 3600, true), "n59_leap_is_february_29");
    check(rule_is("AAA0BBB,59/0,300/0", utc(2023, 3, 1) - 1, 0, false), "n59_common_before");
    check(rule_is("AAA0BBB,59/0,300/0", utc(2023, 3, 1), 3600, true), "n59_common_is_march_first");
    // Day 300 of a common year is October 28; the end is at 00:00 summer time, an hour before in UTC.
    check(rule_is("AAA0BBB,59/0,300/0", utc(2023, 10, 28) - 3600 - 1, 3600, true), "n300_before_end");
    check(rule_is("AAA0BBB,59/0,300/0", utc(2023, 10, 28) - 3600, 0, false), "n300_end");

    // Rules that are not valid.
    static constexpr const char* invalid[] = {"", "EST", "AB5", "EST5EDT,", "EST5EDT,M13.1.0,M11.1.0",
                                              "EST5EDT,M3.2.0", "EST5EDT,M3.2.0,M11.1.0,", "EST25", "<EST5",
                                              "EST5EDT,M3.6.0,M11.1.0", "EST5EDT,J0,J5", "EST5EDT,J366,J5",
                                              "EST5EDT,366,J5", "EST5EDT,M3.0.0,M11.1.0", "EST5EDT,M3.2.7,M11.1.0",
                                              "EST5:60", "EST5EDT,M3.2.0/200,M11.1.0", "5EST"};
    for (const char* text : invalid) {
        TzifRule rule{};
        check(!tzif_parse_rule(text, c_string_length(text), &rule), "invalid_rule_is_refused");
    }
    // A string is a whole rule: trailing text is refused, and the length is honoured.
    TzifRule rule{};
    check(tzif_parse_rule("EST5EDT,M3.2.0,M11.1.0xx", 22, &rule), "length_bounds_the_rule");
    check(!tzif_parse_rule("EST5EDT,M3.2.0,M11.1.0xx", 24, &rule), "trailing_text_is_refused");
}

void test_local_readings() noexcept {
    TzifRule rule{};
    TzifZone zone{};
    check(tzif_parse_rule("EST5EDT,M3.2.0,M11.1.0", 22, &rule), "new_york_rule");
    tzif_zone_from_rule(rule, &zone);
    i64 got = 0;
    // A reading that never happens takes the offset before the change: 02:30 on 2024-03-10 is 07:30 UTC.
    check(tzif_local_to_utc(zone, utc(2024, 3, 10, 2, 30), &got) && got == utc(2024, 3, 10, 7, 30), "gap_reading");
    // A reading that happens twice is the first: 01:30 on 2024-11-03 is 05:30 UTC (EDT), not 06:30.
    check(tzif_local_to_utc(zone, utc(2024, 11, 3, 1, 30), &got) && got == utc(2024, 11, 3, 5, 30), "overlap_reading");
    check(tzif_local_to_utc(zone, utc(2024, 7, 1, 12), &got) && got == utc(2024, 7, 1, 16), "summer_reading");
    check(tzif_local_to_utc(zone, utc(2024, 1, 1, 12), &got) && got == utc(2024, 1, 1, 17), "winter_reading");
    // The readings an hour either side of the gap and of the overlap.
    check(tzif_local_to_utc(zone, utc(2024, 3, 10, 1, 59, 59), &got) && got == utc(2024, 3, 10, 6, 59, 59),
          "reading_before_gap");
    check(tzif_local_to_utc(zone, utc(2024, 3, 10, 3), &got) && got == utc(2024, 3, 10, 7), "reading_after_gap");
    check(tzif_local_to_utc(zone, utc(2024, 11, 3, 2), &got) && got == utc(2024, 11, 3, 7), "reading_after_overlap");
}

void test_range_and_names() noexcept {
    TzifRule rule{};
    TzifZone zone{};
    TzifOffset offset{};
    check(tzif_parse_rule("UTC0", 4, &rule), "utc_rule");
    tzif_zone_from_rule(rule, &zone);
    check(!tzif_offset_at(zone, -62135596801ll, &offset), "before_year_1_is_refused");
    check(tzif_offset_at(zone, -62135596800ll, &offset), "year_1_is_answered");
    check(tzif_offset_at(zone, 253402300799ll, &offset), "last_second_of_9999_is_answered");
    check(!tzif_offset_at(zone, 253402300800ll, &offset), "year_10000_is_refused");
    i64 got = 0;
    check(!tzif_local_to_utc(zone, 253402300800ll, &got), "local_reading_in_10000_is_refused");
    check(!tzif_offset_at(zone, 0, nullptr) && !tzif_local_to_utc(zone, 0, nullptr), "null_outputs");
    TzifZone empty{};
    check(!tzif_offset_at(empty, 0, &offset), "a_zone_without_types_answers_nothing");

    char name[64];
    check(tzif_name_from_path("/usr/share/zoneinfo/Europe/Berlin", name, sizeof(name)) == 13 &&
              names_equal(name, "Europe/Berlin"),
          "name_usr_share");
    check(tzif_name_from_path("/etc/zoneinfo/America/Argentina/Buenos_Aires", name, sizeof(name)) == 30 &&
              names_equal(name, "America/Argentina/Buenos_Aires"),
          "name_etc");
    check(tzif_name_from_path("/nix/store/abc-tzdata-2024a/share/zoneinfo/Asia/Tokyo", name, sizeof(name)) == 10 &&
              names_equal(name, "Asia/Tokyo"),
          "name_nix_store");
    check(tzif_name_from_path("/usr/share/zoneinfo/posix/Etc/GMT+5", name, sizeof(name)) != 0 &&
              names_equal(name, "posix/Etc/GMT+5"),
          "name_posix_prefix_and_plus");
    check(tzif_name_from_path("/usr/share/zoneinfo/UTC", name, sizeof(name)) == 3 && names_equal(name, "UTC"),
          "name_utc");
    static constexpr const char* refused_paths[] = {
        "/etc/localtime", "/usr/share/zoneinfo/", "/usr/share/zoneinfo/../../etc/passwd",
        "/usr/share/zoneinfo/Europe//Berlin", "/usr/share/zoneinfo/Europe/Berlin/",
        "/usr/share/zoneinfo/Europe/Ber lin", "/usr/share/zoneinfo/Europe/Berlin\x7f", ""};
    for (const char* path : refused_paths)
        check(tzif_name_from_path(path, name, sizeof(name)) == 0, "name_refused");
    check(tzif_name_from_path("/usr/share/zoneinfo/Europe/Berlin", name, 13) == 0, "name_capacity_includes_nul");
    check(tzif_name_from_path("/usr/share/zoneinfo/Europe/Berlin", name, 14) == 13, "name_capacity_exact");
    check(tzif_name_valid("Europe/Berlin", 13) && !tzif_name_valid("/Europe", 7) && !tzif_name_valid("a/../b", 6) &&
              !tzif_name_valid(".", 1) && !tzif_name_valid("a//b", 4) && !tzif_name_valid(nullptr, 0),
          "name_valid");
}

// Every prefix of a file and every file with one byte inverted: nothing may read outside the bytes, and a file that
// still opens must answer queries.
void test_damaged_files() noexcept {
    static constexpr const char* damaged[] = {"Etc/UTC", "Europe/Berlin", "Australia/Lord_Howe"};
    for (const char* name : damaged) {
        const Entry* entry = find(name, false);
        check(entry != nullptr, "damaged_source_exists");
        if (!entry)
            continue;
        u32 footer = entry->size - 1;
        while (footer > 0 && entry->data[footer - 1] != '\n')
            --footer;
        --footer; // the '\n' that opens the footer
        bool refused = true;
        for (u32 length = 0; length < entry->size; ++length) {
            TzifZone zone{};
            const bool opened = tzif_open({entry->data, length}, &zone) == TzifStatus::ok;
            // Only the cut at the start of the footer is a whole file (one without a footer).
            if (opened && length != footer)
                refused = false;
        }
        check(refused, "every_prefix_is_refused");
        static u8 copy[4096];
        u32 answered = 0;
        for (u32 at = 0; at < entry->size; ++at) {
            for (u32 i = 0; i < entry->size; ++i)
                copy[i] = entry->data[i];
            copy[at] = static_cast<u8>(~copy[at]);
            TzifZone zone{};
            if (tzif_open({copy, entry->size}, &zone) != TzifStatus::ok)
                continue;
            TzifOffset offset{};
            i64 got = 0;
            static constexpr i64 probes[] = {utc(1890, 1, 1), utc(1970, 1, 1), utc(2024, 7, 1), utc(2090, 12, 31)};
            for (const i64 t : probes) {
                tzif_offset_at(zone, t, &offset);
                tzif_local_to_utc(zone, t, &got);
            }
            ++answered;
        }
        check(answered > 0, "some_inverted_bytes_still_open");
    }
    TzifZone zone{};
    static constexpr u8 text[] = "NOPE";
    check(tzif_open({text, 4}, &zone) == TzifStatus::truncated, "four_bytes");
    static u8 header[44];
    for (u32 i = 0; i < 44; ++i)
        header[i] = i < 4 ? "TZif"[i] : 0;
    header[0] = 'X';
    check(tzif_open({header, 44}, &zone) == TzifStatus::bad_magic, "magic");
    header[0] = 'T';
    header[4] = '9';
    check(tzif_open({header, 44}, &zone) == TzifStatus::bad_version, "version");
    header[4] = 0;
    check(tzif_open({header, 44}, &zone) == TzifStatus::bad_counts, "no_types");
}

void run() noexcept {
    test_fixture_zones();
    test_version_1_and_no_footer();
    test_rules();
    test_local_readings();
    test_range_and_names();
    test_damaged_files();
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_tzif_entry) {
    maicord::run();
    maicord::test_write_metric("checks=", maicord::g_checks);
    maicord::test_exit(maicord::g_failures, "maicord_tzif");
}
