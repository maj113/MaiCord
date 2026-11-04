// The Linux system font locator against the fonts of the host: fontconfig through dlopen, and the directory scan it
// falls back to. The assertions that hold on every system (a resolved family resolves to a file whose own family
// name is that family, a missing family does not resolve, the two lookups agree) always run; the ones that need the
// Ubuntu font packages (fonts-dejavu-core, fonts-noto-core, fonts-noto-cjk, fonts-noto-color-emoji) run when
// MAICORD_TEST_FONT_SET=ubuntu says so, and on any system for the families the locator itself reports installed.
//
// argv[1]: none runs everything with fontconfig; "--scan" runs the scan lookups only; "--fonts-conf" runs the scan
// with FONTCONFIG_FILE naming tests/fixtures/fonts/fonts.conf (a NixOS-style configuration: an xdg directory, a store
// path that does not exist and an include directory that adds the repository's bundled fonts).
#include "maicord/os/ordinal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/os/file.hpp"
#include "maicord/platform/font/default_fonts.hpp"
#include "maicord/platform/font/system_font_locator.hpp"
#include "maicord/sys/linux/process.hpp"
#include "maicord/text/fonts/family.hpp"

#include "../../support/test_output.hpp"

namespace maicord {
namespace {

u32 g_checks = 0;
u32 g_failures = 0;

void check(bool condition, const char* name) noexcept {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        test_write_text("FAIL ");
        test_write_text(name);
        test_write_text("\n");
    }
}

bool equal(const char* a, const char* b) noexcept {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

bool strict_set() noexcept {
    const char* value = environment_get("MAICORD_TEST_FONT_SET");
    return value && equal(value, "ubuntu");
}

struct Utf16 {
    u16 units[kSystemFontNameCapacity];
    u32 count;
};

Utf16 utf16(const char* ascii) noexcept {
    Utf16 name{};
    while (ascii[name.count]) {
        name.units[name.count] = static_cast<u8>(ascii[name.count]);
        ++name.count;
    }
    return name;
}

SystemFontRequest request(const Utf16& name, u16 weight = 400, bool italic = false) noexcept {
    return {{name.units, name.count}, weight, italic};
}

bool same_family(const SystemFontName& a, const u16* b, u32 count) noexcept {
    return a.count == count && ordinal_equal(reinterpret_cast<const wchar_t*>(a.units), static_cast<int>(a.count),
                                             reinterpret_cast<const wchar_t*>(b), static_cast<int>(count), true);
}

// The family and weight the file's own face reports, read with the text engine's parser: what the catalog will see.
struct FaceInfo {
    bool ok;
    SystemFontName family;
    u32 weight;
    u32 style;
    u32 faces;
};

FaceInfo read_face(const SystemFontFile& located) noexcept {
    FaceInfo info{};
    const FileHandle file = file_open(located.path, {FileAccess::read, FileCreate::existing,
                                                     kFileShareRead | kFileShareWrite | kFileShareDelete, 0});
    if (file == INVALID_HANDLE_VALUE)
        return info;
    FileMap map{};
    const bool mapped = file_map(file, false, 0, &map);
    file_close(file);
    if (!mapped)
        return info;
    SfntFace face{};
    u16 name[512];
    NativeFontFamily family{};
    if (sfnt_open({static_cast<const u8*>(map.data), static_cast<u32>(map.bytes)}, located.index, &face) ==
            SfntStatus::ok &&
        native_font_family(face, {name, 512}, &family) == SfntStatus::ok && family.units <= kSystemFontNameCapacity) {
        for (u32 i = 0; i < family.units; ++i)
            info.family.units[i] = name[i];
        info.family.count = family.units;
        info.weight = family.properties.weight;
        info.style = family.properties.style;
        info.faces = face.face_count;
        info.ok = true;
    }
    file_unmap(&map);
    return info;
}

// A family the locator resolves is a family its file carries: the contract the text catalog depends on.
void check_resolves(SystemFontLocator* locator, const char* ascii, u16 weight, bool italic, const char* name) noexcept {
    const Utf16 family = utf16(ascii);
    SystemFontFile file{};
    if (!system_font_locator_find(locator, request(family, weight, italic), &file)) {
        check(false, name);
        return;
    }
    const FaceInfo info = read_face(file);
    check(info.ok && same_family(info.family, family.units, family.count), name);
}

void check_role(SystemFontLocator* locator, SystemFontRole role, const char* expected, const char* name) noexcept {
    SystemFontName resolved{};
    const u32 count = system_font_locator_role(locator, role, &resolved);
    if (count == 0) {
        check(expected == nullptr || !strict_set(), name);
        return;
    }
    check(count == resolved.count, name);
    SystemFontFile file{};
    check(system_font_locator_find(locator, {{resolved.units, resolved.count}, 400, false}, &file), name);
    if (expected) {
        const Utf16 want = utf16(expected);
        check(same_family(resolved, want.units, want.count), name);
    }
}

void test_language() noexcept {
    char language[16];
    const u32 length = system_font_language(language);
    check(length >= 2 && length <= 5 && language[length] == 0, "language_shape");
    check(language[0] >= 'a' && language[0] <= 'z' && language[1] >= 'a' && language[1] <= 'z', "language_lowercase");
}

void test_lookups(SystemFontLocator* locator, bool fontconfig) noexcept {
    // The interface, monospace and serif defaults resolve to families the locator can find again.
    check_role(locator, SystemFontRole::ui, nullptr, fontconfig ? "role_ui_fontconfig" : "role_ui_scan");
    check_role(locator, SystemFontRole::monospace, nullptr, fontconfig ? "role_mono_fontconfig" : "role_mono_scan");
    check_role(locator, SystemFontRole::serif, nullptr, fontconfig ? "role_serif_fontconfig" : "role_serif_scan");
    check_role(locator, SystemFontRole::emoji, strict_set() ? "Noto Color Emoji" : nullptr,
               fontconfig ? "role_emoji_fontconfig" : "role_emoji_scan");

    const Utf16 missing = utf16("Maicord No Such Family");
    SystemFontFile file{};
    check(!system_font_locator_find(locator, request(missing), &file), "missing_family_does_not_resolve");

    // Case never matters.
    SystemFontName ui{};
    if (system_font_locator_role(locator, SystemFontRole::ui, &ui)) {
        Utf16 lower{};
        for (u32 i = 0; i < ui.count; ++i)
            lower.units[i] = ui.units[i] >= 'A' && ui.units[i] <= 'Z' ? static_cast<u16>(ui.units[i] + 32) : ui.units[i];
        lower.count = ui.count;
        check(system_font_locator_find(locator, request(lower), &file), "case_insensitive_family");
    }

    if (strict_set()) {
        check_resolves(locator, "DejaVu Sans", 400, false, "dejavu_regular");
        check_resolves(locator, "DejaVu Sans", 700, false, "dejavu_bold");
        check_resolves(locator, "DejaVu Sans", 400, true, "dejavu_italic");
        check_resolves(locator, "Noto Sans", 400, false, "noto_sans_regular");
        check_resolves(locator, "Noto Sans Devanagari", 400, false, "noto_devanagari");
        check_resolves(locator, "Noto Sans Arabic", 400, false, "noto_arabic");
        check_resolves(locator, "Noto Color Emoji", 400, false, "noto_color_emoji");
        // A collection: each CJK family is a different face of the same file.
        check_resolves(locator, "Noto Sans CJK JP", 400, false, "cjk_jp");
        check_resolves(locator, "Noto Sans CJK KR", 400, false, "cjk_kr");
        check_resolves(locator, "Noto Sans CJK SC", 400, false, "cjk_sc");
        SystemFontFile jp{};
        SystemFontFile kr{};
        const Utf16 jp_name = utf16("Noto Sans CJK JP");
        const Utf16 kr_name = utf16("Noto Sans CJK KR");
        if (system_font_locator_find(locator, request(jp_name), &jp) && system_font_locator_find(locator, request(kr_name), &kr))
            check(ordinal_equal(jp.path, -1, kr.path, -1, false) && jp.index != kr.index, "cjk_faces_of_one_collection");
    }

    // The weight picks among the faces of a family.
    SystemFontFile regular{};
    SystemFontFile bold{};
    const Utf16 dejavu = utf16("DejaVu Sans");
    if (system_font_locator_find(locator, request(dejavu, 400), &regular) &&
        system_font_locator_find(locator, request(dejavu, 700), &bold)) {
        const FaceInfo regular_info = read_face(regular);
        const FaceInfo bold_info = read_face(bold);
        // A family with one weight (NixOS's minimal DejaVu has a regular and a bold file, a stripped one only the
        // regular) answers both requests with it.
        const bool one_face = ordinal_equal(regular.path, -1, bold.path, -1, false) && regular.index == bold.index;
        check(regular_info.ok && bold_info.ok &&
                  (one_face ? regular_info.weight == bold_info.weight : regular_info.weight < bold_info.weight),
              "weight_selects_face");
    }
    if (strict_set()) {
        // DejaVu Sans ships no oblique in fonts-dejavu-core; Noto Sans has a real italic.
        SystemFontFile italic{};
        const Utf16 noto = utf16("Noto Sans");
        check(system_font_locator_find(locator, request(noto, 400, true), &italic), "italic_resolves");
        const FaceInfo italic_info = read_face(italic);
        check(italic_info.ok && italic_info.style != 0, "italic_selects_face");
    }
}

void test_fallbacks(SystemFontLocator* locator) noexcept {
    SystemFontName families[8];
    // Latin: some family covers it.
    check(system_font_locator_fallbacks(locator, 'A', families, 8) >= 1, "fallback_latin");
    check(system_font_locator_fallbacks(locator, 0x10FFFF + 1, families, 8) == 0, "fallback_out_of_range");
    check(system_font_locator_fallbacks(locator, 'A', families, 0) == 0, "fallback_no_capacity");
    if (!strict_set())
        return;
    struct Case {
        u32 scalar;
        const char* prefix;
        const char* name;
    };
    const Case cases[] = {
        {0x0905, "Noto Sans Devanagari", "fallback_devanagari"},
        {0x0627, "Noto Sans Arabic", "fallback_arabic"},
        {0x05D0, "Noto Sans Hebrew", "fallback_hebrew"},
        {0x0E01, "Noto Sans Thai", "fallback_thai"},
        {0x3042, "Noto Sans CJK", "fallback_hiragana"},
        {0xAC00, "Noto Sans CJK", "fallback_hangul"},
        {0x4E2D, "Noto Sans CJK", "fallback_han"},
        {0x1F600, "Noto Color Emoji", "fallback_emoji"},
    };
    for (const Case& test : cases) {
        const u32 count = system_font_locator_fallbacks(locator, test.scalar, families, 8);
        bool found = false;
        const Utf16 want = utf16(test.prefix);
        for (u32 i = 0; i < count && !found; ++i)
            found = families[i].count >= want.count && ordinal_equal(reinterpret_cast<const wchar_t*>(families[i].units),
                                                                      static_cast<int>(want.count),
                                                                      reinterpret_cast<const wchar_t*>(want.units),
                                                                      static_cast<int>(want.count), true);
        check(found, test.name);
        // No name appears twice.
        bool unique = true;
        for (u32 i = 0; i < count; ++i)
            for (u32 j = i + 1; j < count; ++j)
                unique = unique && !(families[i].count == families[j].count &&
                                     ordinal_equal(reinterpret_cast<const wchar_t*>(families[i].units),
                                                   static_cast<int>(families[i].count),
                                                   reinterpret_cast<const wchar_t*>(families[j].units),
                                                   static_cast<int>(families[j].count), true));
        check(unique, "fallback_unique_families");
    }
}

void test_defaults() noexcept {
    const wchar_t* ui = default_font_family(DefaultFont::ui);
    check(ui && ui[0], "default_ui");
    const ConstSlice<u16> name = default_font_name(DefaultFont::ui);
    check(name.count != 0 && name.data == reinterpret_cast<const u16*>(ui) && ui[name.count] == 0, "default_ui_view");
    for (u32 role = 0; role < static_cast<u32>(DefaultFont::count); ++role) {
        const wchar_t* family = default_font_family(static_cast<DefaultFont>(role));
        check(family && family[0], "default_every_role");
    }
    check(default_font_family(DefaultFont::count) == nullptr, "default_invalid_role");
    check(!default_fonts_set_portal("Sans 10", "Monospace 10"), "default_portal_after_resolution");
    const wchar_t* emoji = default_font_family(DefaultFont::emoji);
    const Utf16 want = utf16("Noto Color Emoji");
    bool match = true;
    for (u32 i = 0; i < want.count; ++i)
        match = match && emoji[i] == want.units[i];
    check(!strict_set() || (match && emoji[want.count] == 0), "default_emoji_family");
    // The interface default is a family the locator finds.
    SystemFontLocator locator{};
    SystemFontFile file{};
    check(system_font_locator_find(&locator, {name, 400, false}, &file), "default_ui_resolves");
    system_font_locator_release(&locator);
}

void test_idle_reopens() noexcept {
    SystemFontLocator locator{};
    SystemFontName ui{};
    check(system_font_locator_role(&locator, SystemFontRole::ui, &ui) != 0, "idle_first_lookup");
    const u8 first_state = locator.state;
    system_font_locator_idle(&locator);
    if (first_state == 1)
        check(locator.state == 2 && !locator.library.handle, "idle_closes_fontconfig");
    SystemFontFile file{};
    check(system_font_locator_find(&locator, {{ui.units, ui.count}, 400, false}, &file), "idle_reopens");
    system_font_locator_idle(&locator);
    system_font_locator_idle(&locator);
    system_font_locator_release(&locator);
    check(locator.state == 0 && !locator.scan, "release_clears");
}

bool run(const char* mode) noexcept {
    test_language();
    if (equal(mode, "--fonts-conf")) {
        // The configuration names an xdg directory, a store path that does not exist and an include directory
        // whose file adds the repository's bundled fonts, none of which the usual directories hold.
        SystemFontLocator locator{};
        locator.state = 3;
        SystemFontFile file{};
        const Utf16 orbitron = utf16("Orbitron");
        check(system_font_locator_find(&locator, request(orbitron), &file), "fonts_conf_bundled_font");
        const FaceInfo info = read_face(file);
        check(info.ok && same_family(info.family, orbitron.units, orbitron.count), "fonts_conf_face");
        const Utf16 kalam = utf16("Kalam");
        check(system_font_locator_find(&locator, request(kalam), &file), "fonts_conf_second_font");
        system_font_locator_release(&locator);
        return g_failures == 0;
    }
    if (equal(mode, "--scan")) {
        SystemFontLocator locator{};
        locator.state = 3;
        test_lookups(&locator, false);
        check(locator.scan != nullptr, "scan_index_built");
        system_font_locator_release(&locator);
        return g_failures == 0;
    }
    SystemFontLocator locator{};
    test_lookups(&locator, true);
    check(!strict_set() || locator.state == 1, "fontconfig_loaded");
    test_write_text(locator.state == 1 ? "backend: fontconfig\n" : "backend: directory scan (libfontconfig.so.1 unavailable)\n");
    test_fallbacks(&locator);
    system_font_locator_idle(&locator);
    system_font_locator_release(&locator);
    test_defaults();
    test_idle_reopens();
    return g_failures == 0;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_font_locator_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    const bool ok = run(startup.argc > 1 ? startup.argv[1] : "");
    test_write_text("checks=");
    test_write_u64(g_checks);
    test_write_text(ok ? " PASS font locator\n" : " FAIL font locator\n");
    test_process_exit(ok ? 0 : 1);
}
