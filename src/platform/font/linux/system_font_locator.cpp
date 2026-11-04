#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/font/system_font_locator.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"

#include "font_names.hpp"
#include "font_scan.hpp"

namespace maicord {
namespace fc = fontconfig;

namespace {

constexpr const char kLibrary[] = "libfontconfig.so.1";
constexpr u32 kSortedFontLimit = 64;

struct Resolver {
    const SharedLibrary& library;
    const char* missing;

    template <typename Function> void operator()(Function* slot, const char* name) noexcept {
        if (missing)
            return;
        void* address = shared_library_symbol(library, name);
        if (!address) {
            missing = name;
            return;
        }
        *slot = reinterpret_cast<Function>(address);
    }
};

struct PatternGuard {
    const fc::Api& api;
    fc::Pattern* pattern;
    ~PatternGuard() noexcept {
        if (pattern)
            api.pattern_destroy(pattern);
    }
};

struct SetGuard {
    const fc::Api& api;
    fc::FontSet* set;
    ~SetGuard() noexcept {
        if (set)
            api.font_set_destroy(set);
    }
};

struct CharSetGuard {
    const fc::Api& api;
    fc::CharSet* set;
    ~CharSetGuard() noexcept {
        if (set)
            api.char_set_destroy(set);
    }
};

void log_missing(const char* symbol) noexcept {
    char message[96];
    const char* const parts[] = {kLibrary, ": no symbol ", symbol};
    u32 length = 0;
    for (const char* part : parts)
        for (; *part && length + 1 < sizeof(message); ++part)
            message[length++] = *part;
    message[length] = 0;
    log_write(LogLevel::warning, "fonts", message);
}

// Opens libfontconfig and initializes it, once per use of it. False when it is not installed or unusable (then the
// directory scan answers, and nothing tries the library again).
bool open_fontconfig(SystemFontLocator* locator) noexcept {
    if (locator->state == 1)
        return true;
    if (locator->state == 3)
        return false;
    SharedLibrary library{};
    if (!shared_library_open(&library, kLibrary)) {
        locator->state = 3;
        return false;
    }
    Resolver resolve{library, nullptr};
    fc::Api api{};
    resolve(&api.init, "FcInit");
    resolve(&api.fini, "FcFini");
    resolve(&api.pattern_create, "FcPatternCreate");
    resolve(&api.pattern_destroy, "FcPatternDestroy");
    resolve(&api.pattern_add_string, "FcPatternAddString");
    resolve(&api.pattern_add_integer, "FcPatternAddInteger");
    resolve(&api.pattern_add_bool, "FcPatternAddBool");
    resolve(&api.pattern_add_char_set, "FcPatternAddCharSet");
    resolve(&api.pattern_get_string, "FcPatternGetString");
    resolve(&api.pattern_get_integer, "FcPatternGetInteger");
    resolve(&api.pattern_get_bool, "FcPatternGetBool");
    resolve(&api.pattern_get_char_set, "FcPatternGetCharSet");
    resolve(&api.config_substitute, "FcConfigSubstitute");
    resolve(&api.default_substitute, "FcDefaultSubstitute");
    resolve(&api.font_match, "FcFontMatch");
    resolve(&api.font_sort, "FcFontSort");
    resolve(&api.font_set_destroy, "FcFontSetDestroy");
    resolve(&api.char_set_create, "FcCharSetCreate");
    resolve(&api.char_set_destroy, "FcCharSetDestroy");
    resolve(&api.char_set_add_char, "FcCharSetAddChar");
    resolve(&api.char_set_has_char, "FcCharSetHasChar");
    if (resolve.missing) {
        log_missing(resolve.missing);
        shared_library_close(&library);
        locator->state = 3;
        return false;
    }
    if (!api.init()) {
        api.fini();
        shared_library_close(&library);
        locator->state = 3;
        return false;
    }
    locator->library = library;
    locator->api = api;
    locator->state = 1;
    return true;
}

// The first value of a string property.
const char* string_property(const fc::Api& api, const fc::Pattern* pattern, const char* object, int index) noexcept {
    u8* value = nullptr;
    if (api.pattern_get_string(pattern, object, index, &value) != fc::kResultMatch || !value)
        return nullptr;
    return reinterpret_cast<const char*>(value);
}

bool equal_text(const char* a, const char* b) noexcept {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

// Only formats the text engine reads: TrueType and OpenType-CFF outlines (a Type 1 or bitmap font is skipped).
bool readable_format(const fc::Api& api, const fc::Pattern* pattern) noexcept {
    const char* format = string_property(api, pattern, "fontformat", 0);
    return format && (equal_text(format, "TrueType") || equal_text(format, "CFF"));
}

// The pattern every lookup starts from: the family, the language and, for a request, the weight and slant.
bool add_language(const fc::Api& api, fc::Pattern* pattern) noexcept {
    char language[16];
    system_font_language(language);
    return api.pattern_add_string(pattern, "lang", reinterpret_cast<const u8*>(language)) != 0;
}

fc::Pattern* make_pattern(const fc::Api& api, const char* family) noexcept {
    fc::Pattern* pattern = api.pattern_create();
    if (!pattern)
        return nullptr;
    if (!api.pattern_add_string(pattern, "family", reinterpret_cast<const u8*>(family)) ||
        !api.pattern_add_bool(pattern, "outline", fc::kTrue) || !add_language(api, pattern)) {
        api.pattern_destroy(pattern);
        return nullptr;
    }
    return pattern;
}

bool substitute(const fc::Api& api, fc::Pattern* pattern) noexcept {
    if (!api.config_substitute(nullptr, pattern, fc::kMatchPattern))
        return false;
    api.default_substitute(pattern);
    return true;
}

// Whether any name of the matched font equals `family`.
bool has_family(const fc::Api& api, const fc::Pattern* match, ConstSlice<u16> family) noexcept {
    for (int index = 0;; ++index) {
        const char* name = string_property(api, match, "family", index);
        if (!name)
            return false;
        SystemFontName converted{};
        if (font_locator::utf8_to_name(name, &converted) && font_locator::names_equal(converted, family))
            return true;
    }
}

// The file and face the matched pattern names.
bool locate(const fc::Api& api, const fc::Pattern* match, SystemFontFile* output) noexcept {
    const char* file = string_property(api, match, "file", 0);
    if (!file)
        return false;
    int index = 0;
    api.pattern_get_integer(match, "index", 0, &index);
    SystemFontFile located{};
    if (path_from_utf8(file, located.path, kSystemFontPathCapacity) == 0)
        return false;
    // fontconfig puts a named instance of a variable font in the upper 16 bits of the index.
    located.index = static_cast<u32>(index) & 0xFFFFu;
    *output = located;
    return true;
}

bool fontconfig_find(SystemFontLocator* locator, const SystemFontRequest& request, SystemFontFile* output) noexcept {
    const fc::Api& api = locator->api;
    char family[4 * kSystemFontNameCapacity + 1];
    if (font_locator::utf16_to_utf8(request.family, family, sizeof(family)) == 0)
        return false;
    PatternGuard pattern{api, make_pattern(api, family)};
    if (!pattern.pattern ||
        !api.pattern_add_integer(pattern.pattern, "weight", fc::weight_from_open_type(request.weight)) ||
        !api.pattern_add_integer(pattern.pattern, "slant", request.italic ? fc::kSlantItalic : fc::kSlantRoman) ||
        !substitute(api, pattern.pattern))
        return false;
    int result = 0;
    PatternGuard match{api, api.font_match(nullptr, pattern.pattern, &result)};
    if (!match.pattern || result != fc::kResultMatch || !readable_format(api, match.pattern) ||
        !has_family(api, match.pattern, request.family))
        return false;
    return locate(api, match.pattern, output);
}

// The generic family names of the roles; the emoji one is fontconfig's own (45-generic.conf).
constexpr const char* const kRoleFamilies[static_cast<u32>(SystemFontRole::count)] = {
    "sans-serif", "serif", "monospace", "cursive", "emoji",
};

u32 fontconfig_role(SystemFontLocator* locator, SystemFontRole role, SystemFontName* output) noexcept {
    const fc::Api& api = locator->api;
    PatternGuard pattern{api, make_pattern(api, kRoleFamilies[static_cast<u32>(role)])};
    if (!pattern.pattern || !substitute(api, pattern.pattern))
        return 0;
    int result = 0;
    SetGuard sorted{api, api.font_sort(nullptr, pattern.pattern, fc::kFalse, nullptr, &result)};
    if (!sorted.set)
        return 0;
    for (int i = 0; i < sorted.set->count && i < static_cast<int>(kSortedFontLimit); ++i) {
        const fc::Pattern* font = sorted.set->fonts[i];
        if (!readable_format(api, font))
            continue;
        if (role == SystemFontRole::emoji) {
            int color = 0;
            if (api.pattern_get_bool(font, "color", 0, &color) != fc::kResultMatch || !color)
                continue;
        }
        SystemFontName name{};
        if (font_locator::utf8_to_name(string_property(api, font, "family", 0), &name)) {
            *output = name;
            return name.count;
        }
    }
    return 0;
}

u32 fontconfig_fallbacks(SystemFontLocator* locator, u32 scalar, SystemFontName* output, u32 capacity) noexcept {
    const fc::Api& api = locator->api;
    PatternGuard pattern{api, make_pattern(api, "sans-serif")};
    CharSetGuard wanted{api, api.char_set_create()};
    if (!pattern.pattern || !wanted.set || !api.char_set_add_char(wanted.set, scalar) ||
        !api.pattern_add_char_set(pattern.pattern, "charset", wanted.set) || !substitute(api, pattern.pattern))
        return 0;
    int result = 0;
    SetGuard sorted{api, api.font_sort(nullptr, pattern.pattern, fc::kFalse, nullptr, &result)};
    if (!sorted.set)
        return 0;
    u32 count = 0;
    for (int i = 0; i < sorted.set->count && i < static_cast<int>(kSortedFontLimit) && count < capacity; ++i) {
        const fc::Pattern* font = sorted.set->fonts[i];
        fc::CharSet* covered = nullptr;
        if (!readable_format(api, font) || api.pattern_get_char_set(font, "charset", 0, &covered) != fc::kResultMatch ||
            !covered || !api.char_set_has_char(covered, scalar))
            continue;
        SystemFontName name{};
        if (!font_locator::utf8_to_name(string_property(api, font, "family", 0), &name))
            continue;
        bool known = false;
        for (u32 j = 0; j < count && !known; ++j)
            known = font_locator::names_equal(output[j], name);
        if (!known)
            output[count++] = name;
    }
    return count;
}

// LC_ALL, then LC_MESSAGES, then LANG, as the C library resolves a category; "C" and "POSIX" name no language.
const char* language_variable() noexcept {
    const char* const names[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    for (const char* name : names) {
        const char* value = environment_get(name);
        if (value && *value && !equal_text(value, "C") && !equal_text(value, "POSIX"))
            return value;
    }
    return nullptr;
}

} // namespace

namespace fontconfig {

int weight_from_open_type(u32 weight) noexcept {
    // fontconfig's FcWeightFromOpenType table: OpenType weights and the FC_WEIGHT_* values between which it
    // interpolates linearly.
    struct Point {
        u32 open_type;
        int fontconfig;
    };
    static constexpr Point points[] = {{0, 0},   {100, 0},   {200, 40},  {300, 50},  {350, 55},  {380, 75},  {400, 80},
                                       {500, 100}, {600, 180}, {700, 200}, {800, 205}, {900, 210}, {1000, 215}};
    constexpr u32 count = sizeof(points) / sizeof(points[0]);
    if (weight >= points[count - 1].open_type)
        return points[count - 1].fontconfig;
    for (u32 i = 1; i < count; ++i) {
        if (weight <= points[i].open_type) {
            const Point& low = points[i - 1];
            const Point& high = points[i];
            return low.fontconfig + static_cast<int>((high.fontconfig - low.fontconfig) * (weight - low.open_type) /
                                                     (high.open_type - low.open_type));
        }
    }
    return points[count - 1].fontconfig;
}

} // namespace fontconfig

u32 system_font_language(char (&output)[16]) noexcept {
    const char* value = language_variable();
    u32 length = 0;
    if (value) {
        // "ja_JP.UTF-8@modifier": the language, and for Chinese the territory that picks the script.
        char language[3] = {};
        u32 index = 0;
        for (; index < 2 && value[index] >= 'A' && value[index] <= 'z' && value[index] != '_'; ++index)
            language[index] = static_cast<char>(value[index] | 0x20);
        if (index == 2) {
            output[length++] = language[0];
            output[length++] = language[1];
            if (language[0] == 'z' && language[1] == 'h' && value[2] == '_') {
                const char* territory = value + 3;
                if (territory[0] && territory[1] && !(territory[2] && territory[2] != '.' && territory[2] != '@')) {
                    output[length++] = '-';
                    output[length++] = static_cast<char>(territory[0] | 0x20);
                    output[length++] = static_cast<char>(territory[1] | 0x20);
                }
            }
        }
    }
    if (length == 0) {
        output[0] = 'e';
        output[1] = 'n';
        length = 2;
    }
    output[length] = 0;
    return length;
}

bool system_font_locator_find(SystemFontLocator* locator, const SystemFontRequest& request,
                              SystemFontFile* output) noexcept {
    if (!locator || !output || !request.family.data || request.family.count == 0 || request.weight == 0)
        return false;
    if (open_fontconfig(locator))
        return fontconfig_find(locator, request, output);
    return font_locator::scan_find(locator, request, output);
}

u32 system_font_locator_role(SystemFontLocator* locator, SystemFontRole role, SystemFontName* output) noexcept {
    if (!locator || !output || static_cast<u32>(role) >= static_cast<u32>(SystemFontRole::count))
        return 0;
    if (open_fontconfig(locator)) {
        const u32 count = fontconfig_role(locator, role, output);
        if (count)
            return count;
    }
    return font_locator::scan_role(locator, role, output);
}

u32 system_font_locator_fallbacks(SystemFontLocator* locator, u32 scalar, SystemFontName* output,
                                  u32 capacity) noexcept {
    if (!locator || !output || capacity == 0 || scalar > 0x10FFFF || !open_fontconfig(locator))
        return 0;
    return fontconfig_fallbacks(locator, scalar, output, capacity);
}

void system_font_locator_idle(SystemFontLocator* locator) noexcept {
    if (!locator || locator->state != 1)
        return;
    locator->api.fini();
    shared_library_close(&locator->library);
    clear_bytes(&locator->api, sizeof(locator->api));
    locator->state = 2;
}

void system_font_locator_release(SystemFontLocator* locator) noexcept {
    if (!locator)
        return;
    system_font_locator_idle(locator);
    font_locator::scan_release(locator);
    clear_bytes(locator, sizeof(*locator));
}

} // namespace maicord
