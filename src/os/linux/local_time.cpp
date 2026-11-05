// The user's time zone on Linux (include/maicord/os/local_time.hpp): where the zone comes from, and the conversions on
// top of the TZif reader.
#include "maicord/os/local_time.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/linux/kernel.hpp"
#include "maicord/platform/time/tzif.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {
namespace {

// The largest zone file read. The database's biggest "fat" files are about 4 KB.
constexpr u32 kZoneBytes = 32768;
constexpr u32 kPathBytes = 512;
constexpr u64 kRecheckMilliseconds = 15'000;

// A zone read from its source: the file bytes it views, the identifier and the parsed zone.
struct Loaded {
    u8 bytes[kZoneBytes];
    u32 size;
    char name[64];
    u32 name_length;
    TzifZone zone;
};

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    bool loaded;
    u64 next_check;
    Loaded current;
};

State g_state;
Loaded g_candidate; // read under the lock; becomes `current` when it differs

u32 text_length(const char* text) noexcept {
    return c_string_length(text);
}

// Joins directory, "/" and name into `path`; false when it does not fit.
bool join(char (&path)[kPathBytes], const char* directory, const char* name) noexcept {
    const u32 d = text_length(directory), n = text_length(name);
    if (d + 1 + n >= kPathBytes)
        return false;
    copy_bytes(path, directory, d);
    path[d] = '/';
    copy_bytes(path + d + 1, name, n + 1);
    return true;
}

// The bytes of a file into `loaded`; false when it is missing, unreadable or larger than the buffer.
bool read_file(const char* path, Loaded* loaded) noexcept {
    const long file = sys_open(path, kOpenReadOnly);
    if (syscall_failed(file))
        return false;
    u32 size = 0;
    bool ok = true;
    for (;;) {
        const long got = sys_read(static_cast<int>(file), loaded->bytes + size, kZoneBytes - size);
        if (syscall_failed(got)) {
            ok = false;
            break;
        }
        if (got == 0)
            break;
        size += static_cast<u32>(got);
        if (size == kZoneBytes) {
            // Full: more would not fit.
            u8 extra;
            ok = sys_read(static_cast<int>(file), &extra, 1) == 0;
            break;
        }
    }
    sys_close(static_cast<int>(file));
    loaded->size = size;
    return ok && size != 0;
}

bool open_loaded(Loaded* loaded) noexcept {
    return tzif_open({loaded->bytes, loaded->size}, &loaded->zone) == TzifStatus::ok;
}

void set_name(Loaded* loaded, const char* name, u32 length) noexcept {
    if (length >= sizeof(loaded->name)) {
        loaded->name[0] = 0;
        loaded->name_length = 0;
        return;
    }
    copy_bytes(loaded->name, name, length);
    loaded->name[length] = 0;
    loaded->name_length = length;
}

void set_rule(Loaded* loaded, const TzifRule& rule) noexcept {
    loaded->size = 0;
    set_name(loaded, "", 0);
    tzif_zone_from_rule(rule, &loaded->zone);
}

void set_utc(Loaded* loaded) noexcept {
    TzifRule rule{};
    tzif_parse_rule("UTC0", 4, &rule);
    set_rule(loaded, rule);
}

// Reads the zone `name` from the first zoneinfo directory that has it.
bool read_named(const char* name, Loaded* loaded) noexcept {
    const u32 length = text_length(name);
    if (!tzif_name_valid(name, length))
        return false;
    const char* tzdir = environment_get("TZDIR");
    static constexpr const char* directories[] = {"/usr/share/zoneinfo", "/usr/share/lib/zoneinfo", "/usr/lib/zoneinfo",
                                                  "/etc/zoneinfo", "/run/current-system/sw/share/zoneinfo"};
    char path[kPathBytes];
    if (tzdir && *tzdir && join(path, tzdir, name) && read_file(path, loaded) && open_loaded(loaded)) {
        set_name(loaded, name, length);
        return true;
    }
    for (const char* directory : directories) {
        if (join(path, directory, name) && read_file(path, loaded) && open_loaded(loaded)) {
            set_name(loaded, name, length);
            return true;
        }
    }
    return false;
}

// The identifier a symlink chain names: the first link text on the way that has a "zoneinfo/" part.
u32 name_from_links(const char* start, char (&output)[64]) noexcept {
    char path[kPathBytes];
    u32 length = text_length(start);
    if (length >= kPathBytes)
        return 0;
    copy_bytes(path, start, length + 1);
    for (u32 depth = 0; depth < 8; ++depth) {
        char target[kPathBytes];
        const long got = sys_readlink(path, target, kPathBytes - 1);
        if (syscall_failed(got) || got == 0)
            return 0;
        target[got] = 0;
        if (const u32 name = tzif_name_from_path(target, output, sizeof(output)))
            return name;
        if (target[0] == '/') {
            copy_bytes(path, target, static_cast<u32>(got) + 1);
        } else {
            // Relative to the link's directory.
            u32 slash = text_length(path);
            while (slash > 0 && path[slash - 1] != '/')
                --slash;
            if (slash + static_cast<u32>(got) >= kPathBytes)
                return 0;
            copy_bytes(path + slash, target, static_cast<u32>(got) + 1);
        }
    }
    return 0;
}

// $TZ the way libc reads it.
bool load_from_environment(const char* tz, Loaded* loaded) noexcept {
    if (*tz == ':')
        ++tz;
    if (!*tz) {
        set_utc(loaded);
        return true;
    }
    char name[64];
    if (*tz == '/') {
        if (read_file(tz, loaded) && open_loaded(loaded)) {
            const u32 length = tzif_name_from_path(tz, name, sizeof(name));
            set_name(loaded, name, length);
            return true;
        }
        return false;
    }
    if (read_named(tz, loaded))
        return true;
    TzifRule rule{};
    if (tzif_parse_rule(tz, text_length(tz), &rule)) {
        set_rule(loaded, rule);
        return true;
    }
    return false;
}

// /etc/localtime, else the name in /etc/timezone.
bool load_from_system(Loaded* loaded) noexcept {
    char name[64];
    if (read_file("/etc/localtime", loaded) && open_loaded(loaded)) {
        const u32 length = name_from_links("/etc/localtime", name);
        set_name(loaded, name, length);
        return true;
    }
    Loaded* scratch = loaded;
    if (read_file("/etc/timezone", scratch)) {
        // One line: the identifier.
        u32 length = 0;
        while (length < scratch->size && scratch->bytes[length] != '\n' && scratch->bytes[length] != '\r' &&
               length < sizeof(name) - 1) {
            name[length] = static_cast<char>(scratch->bytes[length]);
            ++length;
        }
        name[length] = 0;
        if (length != 0 && read_named(name, loaded))
            return true;
    }
    return false;
}

void load(Loaded* loaded) noexcept {
    const char* tz = environment_get("TZ");
    bool ok = tz ? load_from_environment(tz, loaded) : load_from_system(loaded);
    if (!ok)
        set_utc(loaded);
}

bool same(const Loaded& a, const Loaded& b) noexcept {
    if (a.size != b.size || a.name_length != b.name_length || a.zone.has_rule != b.zone.has_rule)
        return false;
    return bytes_equal(a.bytes, b.bytes, a.size) && bytes_equal(a.name, b.name, a.name_length) &&
           (a.size != 0 || (bytes_equal(&a.zone.rule, &b.zone.rule, sizeof(TzifRule))));
}

// Under the lock: the current zone, read or re-read when due.
const Loaded& zone_locked() noexcept {
    const u64 now = clock_monotonic_milliseconds();
    if (!g_state.loaded) {
        load(&g_state.current);
        g_state.loaded = true;
        g_state.next_check = now + kRecheckMilliseconds;
    } else if (now >= g_state.next_check) {
        g_state.next_check = now + kRecheckMilliseconds;
        load(&g_candidate);
        if (!same(g_candidate, g_state.current)) {
            // The views of `zone` point into the buffer they were opened on: copy the bytes, then open again.
            copy_bytes(&g_state.current, &g_candidate, sizeof(Loaded));
            if (g_state.current.size != 0)
                open_loaded(&g_state.current);
        }
    }
    return g_state.current;
}

struct Guard {
    Guard() noexcept { AcquireSRWLockExclusive(&g_state.lock); }
    ~Guard() noexcept { ReleaseSRWLockExclusive(&g_state.lock); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

constexpr u16 weekday_of(i64 days) noexcept {
    // 1970-01-01 was a Thursday.
    return static_cast<u16>(((days % 7) + 7 + 4) % 7);
}

} // namespace

i32 local_utc_offset(i64 unix_seconds, bool* daylight) noexcept {
    TzifOffset offset{};
    bool found;
    {
        Guard guard;
        found = tzif_offset_at(zone_locked().zone, unix_seconds, &offset);
    }
    if (daylight)
        *daylight = found && offset.daylight;
    return found ? offset.utc_offset : 0;
}

bool local_time_from_unix(i64 unix_seconds, CalendarTime* output, i32* utc_offset) noexcept {
    if (!output)
        return false;
    TzifOffset offset{};
    {
        Guard guard;
        if (!tzif_offset_at(zone_locked().zone, unix_seconds, &offset))
            return false;
    }
    const i64 local = unix_seconds + offset.utc_offset;
    const i64 days = local / 86400 - (local % 86400 < 0 ? 1 : 0);
    const i64 seconds = local - days * 86400;
    const CivilDate date = civil_date_from_days(days);
    output->wYear = static_cast<u16>(date.year);
    output->wMonth = static_cast<u16>(date.month);
    output->wDay = static_cast<u16>(date.day);
    output->wDayOfWeek = weekday_of(days);
    output->wHour = static_cast<u16>(seconds / 3600);
    output->wMinute = static_cast<u16>(seconds / 60 % 60);
    output->wSecond = static_cast<u16>(seconds % 60);
    output->wMilliseconds = 0;
    if (utc_offset)
        *utc_offset = offset.utc_offset;
    return true;
}

bool local_time_to_unix(const CalendarTime& local, i64* unix_seconds) noexcept {
    if (!unix_seconds || local.wYear < 1 || local.wYear > 9999 || local.wMonth < 1 || local.wMonth > 12 ||
        local.wDay < 1 || local.wDay > civil_month_days(local.wYear, local.wMonth) || local.wHour > 23 ||
        local.wMinute > 59 || local.wSecond > 59)
        return false;
    const i64 reading = civil_days_from_date(local.wYear, local.wMonth, local.wDay) * 86400 + local.wHour * 3600 +
                        local.wMinute * 60 + local.wSecond;
    Guard guard;
    return tzif_local_to_utc(zone_locked().zone, reading, unix_seconds);
}

void local_time_now(CalendarTime* output) noexcept {
    if (!output)
        return;
    TimeSpec now{};
    clock_gettime(kClockRealtime, &now);
    if (!local_time_from_unix(now.seconds, output))
        *output = {};
    output->wMilliseconds = static_cast<u16>(now.nanoseconds / 1'000'000);
}

u32 local_zone_name(Slice<char> output) noexcept {
    if (!output.data || output.count == 0)
        return 0;
    output[0] = 0;
    Guard guard;
    const Loaded& zone = zone_locked();
    if (zone.name_length == 0 || zone.name_length >= output.count)
        return 0;
    copy_bytes(output.data, zone.name, zone.name_length + 1);
    return zone.name_length;
}

} // namespace maicord
