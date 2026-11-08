#pragma once

#include "maicord/core/base.hpp"

// The fontconfig interface the Linux font locator uses, declared without its headers (Linux). The library is
// dlopen()ed ("libfontconfig.so.1") for the time one batch of lookups takes and closed again afterwards; the
// declarations follow <fontconfig/fontconfig.h>, and every function is in fontconfig 2.13.1 (Ubuntu 22.04, the oldest of
// the supported distributions).

namespace maicord::fontconfig {

struct Pattern;
struct Config;
struct CharSet;

struct FontSet {
    int count;
    int capacity;
    Pattern** fonts;
};

// FcResult.
inline constexpr int kResultMatch = 0;
// FcMatchKind.
inline constexpr int kMatchPattern = 0;
// FcBool.
inline constexpr int kTrue = 1;
inline constexpr int kFalse = 0;
// The FC_SLANT values.
inline constexpr int kSlantRoman = 0;
inline constexpr int kSlantItalic = 100;

// The fontconfig weight scale (FC_WEIGHT_*), which is not the OpenType one.
int weight_from_open_type(u32 weight) noexcept;

struct Api {
    int (*init)() noexcept;
    void (*fini)() noexcept;
    Pattern* (*pattern_create)() noexcept;
    void (*pattern_destroy)(Pattern* pattern) noexcept;
    int (*pattern_add_string)(Pattern* pattern, const char* object, const u8* value) noexcept;
    int (*pattern_add_integer)(Pattern* pattern, const char* object, int value) noexcept;
    int (*pattern_add_bool)(Pattern* pattern, const char* object, int value) noexcept;
    int (*pattern_add_char_set)(Pattern* pattern, const char* object, const CharSet* value) noexcept;
    int (*pattern_get_string)(const Pattern* pattern, const char* object, int index, u8** value) noexcept;
    int (*pattern_get_integer)(const Pattern* pattern, const char* object, int index, int* value) noexcept;
    int (*pattern_get_bool)(const Pattern* pattern, const char* object, int index, int* value) noexcept;
    int (*pattern_get_char_set)(const Pattern* pattern, const char* object, int index, CharSet** value) noexcept;
    int (*config_substitute)(Config* config, Pattern* pattern, int kind) noexcept;
    void (*default_substitute)(Pattern* pattern) noexcept;
    Pattern* (*font_match)(Config* config, Pattern* pattern, int* result) noexcept;
    FontSet* (*font_sort)(Config* config, Pattern* pattern, int trim, CharSet** covered, int* result) noexcept;
    void (*font_set_destroy)(FontSet* set) noexcept;
    CharSet* (*char_set_create)() noexcept;
    void (*char_set_destroy)(CharSet* set) noexcept;
    int (*char_set_add_char)(CharSet* set, u32 scalar) noexcept;
    int (*char_set_has_char)(const CharSet* set, u32 scalar) noexcept;
};

} // namespace maicord::fontconfig
