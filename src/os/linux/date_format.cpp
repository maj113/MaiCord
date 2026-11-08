// The Linux body of format_date and format_time (include/maicord/os/date_format.hpp): the Windows picture syntax,
// English names (D19), the hour cycle from the locale environment.
#include "maicord/os/date_format.hpp"
#include "maicord/core/time.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {
namespace {

constexpr const char* kMonths[12] = {"January", "February", "March",     "April",   "May",      "June",
                                     "July",    "August",   "September", "October", "November", "December"};
constexpr const char* kDays[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// Characters counted past the capacity are not written but still counted, so a null output asks for the size.
struct Output {
    wchar_t* data;
    int capacity;
    int length;

    void put(wchar_t c) noexcept {
        if (data && length + 1 < capacity)
            data[length] = c;
        ++length;
    }
    void put_ascii(const char* text, u32 limit = ~0u) noexcept {
        for (u32 i = 0; text[i] && i < limit; ++i)
            put(static_cast<wchar_t>(static_cast<unsigned char>(text[i])));
    }
    void put_number(u32 value, u32 minimum_digits) noexcept {
        char digits[10];
        u32 count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value && count < sizeof(digits));
        for (u32 i = count; i < minimum_digits; ++i)
            put(L'0');
        while (count)
            put(static_cast<wchar_t>(digits[--count]));
    }
    // The text is the whole result: the length with its NUL, or zero when it did not fit.
    int finish() noexcept {
        if (data && length + 1 <= capacity) {
            data[length] = 0;
            return length + 1;
        }
        return data || capacity != 0 ? 0 : length + 1;
    }
};

u32 run_of(const wchar_t* picture, u32 at, wchar_t letter) noexcept {
    u32 count = 0;
    while (picture[at + count] == letter)
        ++count;
    return count;
}

// A quoted run 'text' ('' is a quote): returns the index after it.
u32 quoted(const wchar_t* picture, u32 at, Output* output) noexcept {
    ++at;
    while (picture[at]) {
        if (picture[at] == L'\'') {
            if (picture[at + 1] != L'\'')
                return at + 1;
            ++at;
        }
        output->put(picture[at++]);
    }
    return at;
}

bool valid_date(const CalendarTime& t) noexcept {
    return t.wYear >= 1 && t.wYear <= 9999 && t.wMonth >= 1 && t.wMonth <= 12 && t.wDay >= 1 &&
           t.wDay <= civil_month_days(t.wYear, t.wMonth);
}

bool valid_time(const CalendarTime& t) noexcept {
    return t.wHour < 24 && t.wMinute < 60 && t.wSecond < 60;
}

u32 weekday(const CalendarTime& t) noexcept {
    const i64 days = civil_days_from_date(t.wYear, t.wMonth, t.wDay);
    return static_cast<u32>(((days % 7) + 7 + 4) % 7);
}

void date_picture(const wchar_t* picture, const CalendarTime& t, Output* output) noexcept {
    for (u32 at = 0; picture[at];) {
        const wchar_t c = picture[at];
        if (c == L'\'') {
            at = quoted(picture, at, output);
            continue;
        }
        const u32 run = (c == L'd' || c == L'M' || c == L'y' || c == L'g') ? run_of(picture, at, c) : 1;
        // One or two letters: the number (two: zero-padded); three: the abbreviated name; four: the full name.
        if (c == L'd') {
            if (run <= 2)
                output->put_number(t.wDay, run);
            else
                output->put_ascii(kDays[weekday(t)], run == 3 ? 3 : ~0u);
        } else if (c == L'M') {
            if (run <= 2)
                output->put_number(t.wMonth, run);
            else
                output->put_ascii(kMonths[t.wMonth - 1], run == 3 ? 3 : ~0u);
        } else if (c == L'y') {
            if (run <= 2)
                output->put_number(t.wYear % 100, run);
            else
                output->put_number(t.wYear, 4);
        } else if (c == L'g') {
            output->put_ascii("A.D.");
        } else {
            output->put(c);
        }
        at += run;
    }
}

void time_picture(const wchar_t* picture, const CalendarTime& t, Output* output) noexcept {
    for (u32 at = 0; picture[at];) {
        const wchar_t c = picture[at];
        if (c == L'\'') {
            at = quoted(picture, at, output);
            continue;
        }
        const u32 run = (c == L'h' || c == L'H' || c == L'm' || c == L's' || c == L't') ? run_of(picture, at, c) : 1;
        const u32 twelve = t.wHour % 12 == 0 ? 12 : t.wHour % 12;
        if (c == L'h')
            output->put_number(twelve, run >= 2 ? 2 : 1);
        else if (c == L'H')
            output->put_number(t.wHour, run >= 2 ? 2 : 1);
        else if (c == L'm')
            output->put_number(t.wMinute, run >= 2 ? 2 : 1);
        else if (c == L's')
            output->put_number(t.wSecond, run >= 2 ? 2 : 1);
        else if (c == L't')
            output->put_ascii(t.wHour < 12 ? "AM" : "PM", run >= 2 ? 2 : 1);
        else
            output->put(c);
        at += run;
    }
}

wchar_t lower(wchar_t c) noexcept {
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c;
}

const char* locale_name() noexcept {
    static constexpr const char* variables[] = {"LC_ALL", "LC_TIME", "LANG"};
    for (const char* variable : variables) {
        const char* value = environment_get(variable);
        if (value && *value)
            return value;
    }
    return nullptr;
}

bool language_is(const char* name, const char* language) noexcept {
    u32 i = 0;
    for (; language[i]; ++i)
        if (static_cast<char>(lower(static_cast<unsigned char>(name[i]))) != language[i])
            return false;
    return name[i] == 0 || name[i] == '_' || name[i] == '-' || name[i] == '.' || name[i] == '@';
}

} // namespace

bool locale_uses_24_hours(const char* locale) noexcept {
    if (!locale || !*locale || language_is(locale, "c") || language_is(locale, "posix"))
        return false;
    static constexpr const char* twelve_hour_languages[] = {"ar", "bn", "hi", "ko", "ur"};
    for (const char* language : twelve_hour_languages)
        if (language_is(locale, language))
            return false;
    if (language_is(locale, "en")) {
        // "en", or "en_" / "en-" and a territory.
        if (locale[2] != '_' && locale[2] != '-')
            return false;
        char territory[3] = {};
        u32 length = 0;
        while (length < 2 && ((locale[3 + length] >= 'A' && locale[3 + length] <= 'Z') ||
                              (locale[3 + length] >= 'a' && locale[3 + length] <= 'z'))) {
            territory[length] = static_cast<char>(locale[3 + length] & ~0x20);
            ++length;
        }
        static constexpr const char* twelve_hour_territories[] = {"US", "CA", "AU", "NZ", "PH", "IN", "PK", "SG"};
        for (const char* candidate : twelve_hour_territories)
            if (length == 2 && candidate[0] == territory[0] && candidate[1] == territory[1])
                return false;
    }
    return true;
}

int format_date(const wchar_t*, u32 flags, const CalendarTime* date, const wchar_t* picture, wchar_t* output,
                int capacity) noexcept {
    if (!date || !valid_date(*date) || capacity < 0 || (!output && capacity != 0))
        return 0;
    Output out{output, capacity, 0};
    date_picture(picture ? picture : (flags & DATE_LONGDATE ? L"dddd, MMMM d, yyyy" : L"M/d/yyyy"), *date, &out);
    return out.finish();
}

int format_time(const wchar_t*, u32 flags, const CalendarTime* time, const wchar_t* picture, wchar_t* output,
                int capacity) noexcept {
    if (!time || !valid_time(*time) || capacity < 0 || (!output && capacity != 0))
        return 0;
    Output out{output, capacity, 0};
    if (!picture) {
        const bool seconds = !(flags & TIME_NOSECONDS);
        if (locale_uses_24_hours(locale_name()))
            picture = seconds ? L"HH:mm:ss" : L"HH:mm";
        else
            picture = seconds ? L"h:mm:ss tt" : L"h:mm tt";
    }
    time_picture(picture, *time, &out);
    return out.finish();
}

} // namespace maicord
