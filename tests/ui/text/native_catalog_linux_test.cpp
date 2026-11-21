// The native text catalog on Linux over the installed fonts (the counterpart of native_font_selection_cases.inc, which
// runs against Segoe UI and Bahnschrift): family lookup through the system font locator, selection, the fallback
// routes (fontconfig's order first, then the generated table), private font scopes, caching and failure handling.
// argv[1] "--scan" replaces fontconfig by the directory scan, and adds a font the engine cannot read to the test's
// private XDG_DATA_HOME (its family must be a cached miss); "--broken-face FONT" checks that a face the text engine
// refuses falls back (broken_face); "--desktop-font" checks that a desktop interface font with an opsz axis is the
// interface family (desktop_font). The assertions about particular families need the Ubuntu font packages (fonts-dejavu-core, fonts-noto-core,
// fonts-noto-cjk, fonts-noto-color-emoji) and run when MAICORD_TEST_FONT_SET=ubuntu says so; the rest hold anywhere.
#include "maicord/core/arena.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/text.hpp"
#include "maicord/platform/font/default_fonts.hpp"
#include "maicord/platform/font/system_font_locator.hpp"
#include "maicord/sys/linux/process.hpp"
#include "maicord/text/opentype/bitmap_color.hpp"
#include "maicord/ui/text/native_text_catalog.hpp"

#include "../../../src/ui/text/native_text_catalog_owner.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {

#include "../../../src/ui/text/generated/linux_fallback_data.inc"
#include "../../generated/cff2_collection_fixture.inc"
#include "../../generated/opsz_fixture.inc"

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

bool strict() noexcept {
    const char* set = environment_get("MAICORD_TEST_FONT_SET");
    return set && equal(set, "ubuntu");
}

NativeTextFontQuery query(ConstSlice<u16> family, u16 weight = 400, u8 style = 0) noexcept {
    return {0, family, {weight, 5, style}};
}

NativeTextFontQuery ascii_query(const char* text, u16 weight = 400) noexcept {
    static u16 units[8][64];
    static u32 next = 0;
    u16* storage = units[next++ % 8];
    u32 count = 0;
    while (text[count] && count < 63) {
        storage[count] = static_cast<u8>(text[count]);
        ++count;
    }
    return query({storage, count}, weight);
}

// The file entry of the catalog that holds `candidate`, and the family name its face reports.
const NativeSystemFontFile* file_of(const NativeTextCatalogs& catalogs, u32 candidate) noexcept {
    for (const NativeSystemFontFile* file = catalogs.files; file; file = file->next)
        if (candidate >= file->first_candidate && candidate - file->first_candidate < file->catalog.count)
            return file;
    return nullptr;
}

bool name_starts(const NativeFontCatalog& catalog, const char* prefix) noexcept {
    u32 length = 0;
    while (prefix[length])
        ++length;
    if (catalog.name.count < length)
        return false;
    for (u32 i = 0; i < length; ++i) {
        const u32 a = catalog.name[i];
        const u32 b = static_cast<u8>(prefix[i]);
        if (a != b && ((a | 0x20) != (b | 0x20) || (b | 0x20) < 'a' || (b | 0x20) > 'z'))
            return false;
    }
    return true;
}

// Writes UTF-16 text with each unit above ASCII as '?': the paths and family names of the installed fonts.
void write_units(const u16* units, u32 count) noexcept {
    char buffer[256];
    u32 used = 0;
    for (; used < count && used < sizeof(buffer); ++used)
        buffer[used] = units[used] < 128 ? static_cast<char>(units[used]) : '?';
    test_write_bytes(buffer, used);
}

// Writes "  <name>: <family> <path>#<face index>": the face the route chose, for the logs of other distributions.
void write_choice(const char* name, const NativeSystemFontFile* file) noexcept {
    test_write_text("  ");
    test_write_text(name);
    test_write_text(": ");
    if (file) {
        write_units(file->catalog.name.data, file->catalog.name.count);
        test_write_text(" ");
        u32 length = 0;
        while (file->catalog.path[length])
            ++length;
        write_units(reinterpret_cast<const u16*>(file->catalog.path), length);
        test_write_text("#");
        test_write_u64(file->catalog.face_index);
    } else {
        test_write_text("(no file)");
    }
    test_write_text("\n");
}

struct Fixture {
    NativeTextCatalogs catalogs;
    NativeFontSession* session;
    Arena scratch;
};

// Whether the instance's face maps `scalar`.
bool covers(Fixture& f, NativeFontInstanceHandle instance, u32 scalar) noexcept {
    const NativeFontInstance* held = native_font_instance_get(*native_font_session_instances(f.session), instance);
    if (!held)
        return false;
    const NativeFontFace* face = native_font_get(*native_font_session_fonts(f.session), held->face);
    u16 glyph = 0;
    return face && sfnt_cmap_glyph(face->cmap, scalar, 0, &glyph) == SfntStatus::ok && glyph != 0;
}

// Whether the instance draws `scalar` with an outline. Checked for CFF faces, the CJK fonts: Noto Sans CJK is CFF on
// Ubuntu and a CFF2 variable collection on NixOS and Fedora.
bool draws(Fixture& f, NativeFontInstanceHandle instance, u32 scalar) noexcept {
    const NativeFontInstance* held = native_font_instance_get(*native_font_session_instances(f.session), instance);
    const NativeFontFace* face = held ? native_font_get(*native_font_session_fonts(f.session), held->face) : nullptr;
    if (!face)
        return false;
    if (face->geometry.kind != FontGeometryKind::cff)
        return true;
    u16 glyph = 0;
    static GlyphPathCommand commands[4096];
    GlyphOutline out{};
    out.commands = commands;
    out.capacity = 4096;
    return sfnt_cmap_glyph(face->cmap, scalar, 0, &glyph) == SfntStatus::ok && glyph &&
           font_geometry_outline(held->geometry, glyph, nullptr, &out) == SfntStatus::ok && out.count;
}

// The routed face for `text` through native_text_catalog_range, as the instance it prepares.
bool range_covers(Fixture& f, const NativeTextFontQuery& base, const char* text, u32 scalar, const char* family_prefix,
                  const char* name) noexcept {
    u32 bytes = 0;
    while (text[bytes])
        ++bytes;
    NativeTextFontRange range{};
    if (!native_text_catalog_range(&f.catalogs, base, f.session, &f.scratch, {text, bytes}, &range) ||
        range.match.index == native_fallback_missing) {
        check(!strict(), name);
        test_write_text("  no face for ");
        test_write_text(name);
        test_write_text("\n");
        return false;
    }
    if (range.bytes != bytes) {
        test_write_text("  ");
        test_write_text(name);
        test_write_text(" routed ");
        test_write_u64(range.bytes);
        test_write_text(" of ");
        test_write_u64(bytes);
        test_write_text(" bytes\n");
    }
    check(range.bytes == bytes, name);
    NativeFontInstanceHandle instance{};
    NativeFontSelection source{};
    const bool selected = native_text_catalog_select_face(&f.catalogs, range.scope, range.match.index,
                                                          range.match.simulations, f.session, &f.scratch, &instance, &source);
    check(selected, name);
    if (!selected)
        return false;
    check(covers(f, instance, scalar) && draws(f, instance, scalar), name);
    const NativeSystemFontFile* file = file_of(f.catalogs, range.match.index);
    write_choice(name, file);
    if (family_prefix && strict())
        check(file && name_starts(file->catalog, family_prefix), name);
    native_font_instance_release(native_font_session_instances(f.session), instance);
    return true;
}

void test_policy_table() noexcept {
    // Table invariants beyond the compile-time ones: lists fit, the unassigned and private use ranges name nothing and
    // the CJK families follow the language.
    const NativeFallbackCascade& unspecified = system_fallback_policy;
    bool lists_fit = true;
    for (u32 i = 0; i < unspecified.lists.count; ++i)
        lists_fit = lists_fit && native_fallback_list_count(unspecified.lists[i]) <= system_fallback_max_families;
    check(lists_fit, "policy_lists_fit");
    const auto first_family = [](u32 locale, u32 scalar, const char* expected, const char* name) noexcept {
        const NativeFallbackCascade& policy = system_fallback_policy;
        const u32 route = native_fallback_cascade_find(policy, scalar);
        const u16 list = policy.lists[route];
        if (native_fallback_list_count(list) == 0) {
            check(expected == nullptr, name);
            return;
        }
        const char* family = system_fallback_names + system_fallback_token(locale, native_fallback_list_offset(list));
        bool same = expected != nullptr;
        u32 i = 0;
        for (; same && family[i]; ++i)
            same = expected[i] == family[i];
        same = same && expected && expected[i] == 0;
        check(same, name);
    };
    first_family(0, 'A', "Noto Sans", "policy_latin");
    first_family(0, 0x0905, "Noto Sans Devanagari", "policy_devanagari");
    first_family(0, 0x0627, "Noto Sans Arabic", "policy_arabic");
    first_family(0, 0x1F600, "Noto Color Emoji", "policy_emoji");
    // An unassigned scalar inside a script block belongs to its block; outside every block nothing is tried.
    first_family(0, 0x0378, "Noto Sans", "policy_unassigned_in_block");
    first_family(0, 0x3FFFF, nullptr, "policy_no_block");
    first_family(0, 0xE000, nullptr, "policy_private_use");
    first_family(0, 0x2192, "DejaVu Sans", "policy_arrows");
    first_family(1, 0x65E5, "Noto Sans CJK JP", "policy_han_ja");
    first_family(2, 0x65E5, "Noto Sans CJK KR", "policy_han_ko");
    first_family(3, 0x65E5, "Noto Sans CJK SC", "policy_han_zh_cn");
    first_family(4, 0x65E5, "Noto Sans CJK TC", "policy_han_zh_tw");
    first_family(5, 0x65E5, "Noto Sans CJK HK", "policy_han_zh_hk");
    for (u32 locale = 0; locale < system_fallback_locale_count; ++locale) {
        first_family(locale, 0xAC00, "Noto Sans CJK KR", "policy_hangul_is_korean");
        first_family(locale, 0x3042, "Noto Sans CJK JP", "policy_kana_is_japanese");
    }
}

// Writes `bytes` to `relative` (it starts with '/') under the directory the environment variable names: the test's
// private XDG_DATA_HOME or XDG_CONFIG_HOME. False when the variable is not set.
bool write_private_file(const char* variable, const wchar_t* relative, const u8* bytes, u32 size) noexcept {
    const char* base = environment_get(variable);
    if (!base || !*base)
        return false;
    wchar_t path[512];
    u32 length = 0;
    while (base[length])
        ++length;
    length = utf8_to_utf16(base, length, path, 400, true);
    for (u32 i = 0; relative[i] && length < 511; ++i)
        path[length++] = relative[i];
    path[length] = 0;
    // The private directory may not exist yet: each directory of the path, from the root down.
    for (u32 i = 1; i < length; ++i) {
        if (path[i] != L'/')
            continue;
        path[i] = 0;
        dir_create(path);
        path[i] = L'/';
    }
    u32 written = 0;
    const FileHandle output = file_open(path, {FileAccess::write, FileCreate::create_always, 0, 0});
    if (output == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = file_write(output, bytes, size, &written) && written == size;
    file_close(output);
    return ok;
}

// Writes the CFF2 fixture collection with a CFF2 header size of 2: the directory scan names its family,
// "MaiCord CFF2 Fixture", but the text engine cannot read the face.
bool install_unreadable_font() noexcept {
    static u8 bytes[sizeof(cff2_collection_fixture)];
    copy_bytes(bytes, cff2_collection_fixture, sizeof(bytes));
    SfntFace face{};
    ConstSlice<u8> table{};
    if (sfnt_open({bytes, sizeof(bytes)}, 0, &face) != SfntStatus::ok ||
        sfnt_table(face, sfnt_tag('C', 'F', 'F', '2'), &table) != SfntStatus::ok)
        return false;
    const_cast<u8*>(table.data)[2] = 2;
    return write_private_file("XDG_DATA_HOME", L"/fonts/maicord-qa-unreadable.ttc", bytes, sizeof(bytes));
}

bool run(bool scan) noexcept {
    test_policy_table();
    Fixture f{};
    struct Cleanup {
        Fixture& f;
        ~Cleanup() noexcept {
            native_text_catalogs_release(&f.catalogs);
            if (f.session)
                native_font_session_release(&f.session);
            arena_release(&f.scratch);
        }
    } cleanup{f};
    check(native_font_session_create(&f.session) && arena_initialize(&f.scratch, 1024 * 1024, 0, 4096) &&
              native_text_catalogs_initialize(&f.catalogs),
          "setup");
    // --scan: the catalog's locator is the directory scan (no fontconfig), so every route is the generated table's.
    static SystemFontLocator forced_scan;
    if (scan) {
        check(install_unreadable_font(), "unreadable_font_installed");
        forced_scan.state = 3;
        f.catalogs.locator = &forced_scan;
    }
    check(!f.catalogs.files && !f.catalogs.storage.committed, "nothing_built_before_use");

    const ConstSlice<u16> ui = default_font_name(DefaultFont::ui);
    const NativeTextFontQuery base = query(ui);
    NativeTextFontRange range{};
    check(native_text_catalog_range(&f.catalogs, base, f.session, &f.scratch, utf8_literal("ABC"), &range) &&
              range.bytes == 3 && range.match.index != native_fallback_missing,
          "base_family_range");
    check(f.catalogs.files && f.catalogs.files->catalog.count, "base_family_catalog");
    const u32 selected = range.match.index;
    NativeFontInstanceHandle instance{};
    NativeFontSelection source{};
    check(native_text_catalog_select_face(&f.catalogs, 0, selected, 0, f.session, &f.scratch, &instance, &source),
          "select_face");
    check(covers(f, instance, 'A'), "instance_maps_latin");
    write_choice("interface", file_of(f.catalogs, selected));
    check(native_font_instance_release(native_font_session_instances(f.session), instance), "release_instance");

    // A private scope: the same file registered by path.
    u32 scope = 0;
    check(native_text_catalog_register_path(&f.catalogs, f.catalogs.files->catalog.path, &scope) && scope,
          "register_path");
    NativeTextFontQuery private_query = base;
    private_query.scope = scope;
    check(native_text_catalog_range(&f.catalogs, private_query, f.session, &f.scratch, utf8_literal("ABC"), &range) &&
              range.scope == scope,
          "private_scope_range");

    // The routes: scripts the interface font lacks resolve through fontconfig's order, then the table.
    const struct {
        const char* name;
        const char* text;
        u32 scalar;
        const char* family;
    } samples[] = {
        {"route_japanese", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 0x65E5, "Noto Sans CJK"},
        {"route_korean", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", 0xD55C, "Noto Sans CJK"},
        {"route_devanagari", "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87", 0x0928, "Noto Sans Devanagari"},
        {"route_thai", "\xE0\xB8\xA0\xE0\xB8\xB2\xE0\xB8\xA9\xE0\xB8\xB2\xE0\xB9\x84\xE0\xB8\x97\xE0\xB8\xA2", 0x0E20, nullptr},
        {"route_hebrew", "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", 0x05E9, nullptr},
        {"route_arabic", "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7", 0x0645, nullptr},
        {"route_arrows", "\xE2\x86\x92", 0x2192, nullptr},
        {"route_chinese", "\xE4\xBD\xA0\xE5\xA5\xBD", 0x4F60, "Noto Sans CJK"},
        {"route_kana", "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF", 0x3053, "Noto Sans CJK"},
        {"route_hangul", "\xEC\x95\x88\xEB\x85\x95\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94", 0xC548, "Noto Sans CJK"},
        {"route_emoji", "\xF0\x9F\x98\x80", 0x1F600, nullptr},
        {"route_heart", "\xE2\x9D\xA4", 0x2764, nullptr},
        {"route_check", "\xE2\x9C\x93", 0x2713, nullptr},
        {"route_star", "\xE2\x98\x85", 0x2605, nullptr},
        {"route_gear", "\xE2\x9A\x99", 0x2699, nullptr},
    };
    u32 candidates[sizeof(samples) / sizeof(samples[0])]{};
    for (u32 pass = 0; pass < 2; ++pass) {
        for (u32 i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
            u32 bytes = 0;
            while (samples[i].text[bytes])
                ++bytes;
            if (!pass) {
                range_covers(f, base, samples[i].text, samples[i].scalar, samples[i].family, samples[i].name);
            }
            NativeTextFontRange routed{};
            const bool ok = native_text_catalog_range(&f.catalogs, base, f.session, &f.scratch, {samples[i].text, bytes}, &routed);
            if (!ok || routed.match.index == native_fallback_missing)
                continue;
            if (!pass)
                candidates[i] = routed.match.index;
            else
                check(routed.match.index == candidates[i], "route_is_stable");
        }
        if (!pass) {
            // Warmed queries borrow immutable state: nothing is built and no storage is taken.
            const u64 builds = f.catalogs.builds;
            const usize offset = f.catalogs.storage.offset;
            for (u32 i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
                u32 bytes = 0;
                while (samples[i].text[bytes])
                    ++bytes;
                NativeTextFontRange again{};
                native_text_catalog_range(&f.catalogs, base, f.session, &f.scratch, {samples[i].text, bytes}, &again);
            }
            check(f.catalogs.builds == builds && f.catalogs.storage.offset == offset, "warm_queries_build_nothing");
        }
    }

    // Weights and styles of one family.
    if (strict()) {
        for (u16 weight = 400; weight <= 600; weight += 200) {
            NativeFontInstanceHandle value{};
            u32 simulations = 99;
            check(native_text_catalog_select_query(&f.catalogs, ascii_query("Noto Sans", weight), f.session, &f.scratch, &value,
                                                   &source, nullptr, &simulations) &&
                      value.slot,
                  "select_noto_sans_weight");
            native_font_instance_release(native_font_session_instances(f.session), value);
        }
        // The emoji family resolves to the colour bitmap face.
        NativeFontInstanceHandle emoji{};
        check(native_text_catalog_select_query(&f.catalogs, query(default_font_name(DefaultFont::emoji)), f.session,
                                               &f.scratch, &emoji, &source),
              "select_emoji");
        check(covers(f, emoji, 0x1F600), "emoji_family_maps_emoji");
        const NativeFontInstance* held = native_font_instance_get(*native_font_session_instances(f.session), emoji);
        const NativeFontFace* face = held ? native_font_get(*native_font_session_fonts(f.session), held->face) : nullptr;
        CbdtFont cbdt{};
        check(face && (cbdt_open(face->geometry.face, &cbdt) == SfntStatus::ok), "emoji_family_has_colour_bitmaps");
        native_font_instance_release(native_font_session_instances(f.session), emoji);
    }

    // A variable family whose STAT names an axis that fvar does not vary (Ubuntu Sans and Noto Sans name ital) is
    // the family itself where it is installed, not a fallback.
    {
        const NativeTextFontQuery variable = ascii_query("Ubuntu Sans");
        SystemFontFile located;
        if (f.catalogs.locator && system_font_locator_find(f.catalogs.locator, {variable.family, 400, false}, &located)) {
            NativeFontInstanceHandle value{};
            u32 scope = 99;
            bool fallback = true;
            check(native_text_catalog_select_query(&f.catalogs, variable, f.session, &f.scratch, &value, &source, nullptr,
                                                   nullptr, &scope, &fallback) &&
                      value.slot && !fallback && !scope,
                  "variable_family_with_a_static_stat_axis");
            native_font_instance_release(native_font_session_instances(f.session), value);
        } else {
            test_write_text("  Ubuntu Sans is not installed\n");
        }
    }

    // A family that is not installed is not another family.
    const NativeTextFontQuery missing = ascii_query("MaiCord QA absent family 8F41");
    NativeFontInstanceHandle absent{};
    check(!native_text_catalog_select_query(&f.catalogs, missing, f.session, &f.scratch, &absent, &source) && !absent.slot,
          "missing_family_fails");
    NativeSystemFontFile* before = f.catalogs.files;

    const u64 previous_builds = f.catalogs.builds;
    check(!native_text_catalog_select_query(&f.catalogs, missing, f.session, &f.scratch, &absent, &source) &&
              f.catalogs.builds == previous_builds && f.catalogs.files == before,
          "missing_family_is_cached");
    if (scan) {
        // A family whose face the engine cannot read is skipped and counted, and the miss is cached: the next query
        // opens neither the locator nor the file.
        const NativeTextFontQuery unreadable = ascii_query("MaiCord CFF2 Fixture");
        const u64 counted = f.catalogs.failures;
        check(!native_text_catalog_select_query(&f.catalogs, unreadable, f.session, &f.scratch, &absent, &source) &&
                  !absent.slot && f.catalogs.failures == counted + 1,
              "unreadable_family_fails");
        const u64 builds = f.catalogs.builds;
        check(!native_text_catalog_select_query(&f.catalogs, unreadable, f.session, &f.scratch, &absent, &source) &&
                  f.catalogs.builds == builds && f.catalogs.failures == counted + 1,
              "unreadable_family_is_cached");
        // The time of one query, for the log only: the tests assert counts, never times.
        const u64 start = clock_now_microseconds();
        for (u32 i = 0; i < 1000; ++i)
            native_text_catalog_select_query(&f.catalogs, unreadable, f.session, &f.scratch, &absent, &source);
        test_write_metric("  unreadable family query, nanoseconds=", clock_now_microseconds() - start);
    }
    u32 chosen = 99;
    bool missing_family = false;
    check(native_text_catalog_select_query(&f.catalogs, missing, f.session, &f.scratch, &absent, &source, nullptr,
                                           nullptr, &chosen, &missing_family) &&
              absent.slot && missing_family && !chosen,
          "missing_family_falls_back_to_the_default_face");
    native_font_instance_release(native_font_session_instances(f.session), absent);
    NativeTextFontQuery invalid = missing;
    invalid.properties.weight = 0;
    check(!native_text_catalog_select_query(&f.catalogs, invalid, f.session, &f.scratch, &absent, &source) && !absent.slot,
          "invalid_properties_fail");
    test_write_metric("  unsupported faces=", f.catalogs.unsupported);
    test_write_metric("  catalog failures=", f.catalogs.failures);
    return g_failures == 0;
}

// --broken-face FONT: a copy of FONT (Orbitron) in $XDG_DATA_HOME/fonts, the xdg directory of
// tests/fixtures/fonts/fonts.conf, renamed "QABroken" and with usWeightClass 0. fontconfig lists the face, and the text
// engine refuses its OS/2 table as malformed. The family must then fall back as a family that is not installed does:
// a face the client cannot use never makes the text system fail.
bool broken_face(const char* font_path) noexcept {
    static u8 bytes[65536];
    wchar_t path[512];
    u32 units = 0;
    while (font_path[units])
        ++units;
    units = utf8_to_utf16(font_path, units, path, 511, true);
    path[units] = 0;
    u32 size = 0;
    const FileHandle input = file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead, 0});
    check(input != INVALID_HANDLE_VALUE && file_read(input, bytes, sizeof(bytes), &size) && size > 12 && size < sizeof(bytes),
          "broken_face_source");
    file_close(input);
    // The family name, in the UTF-16BE name records: "Orbitron" and "QABroken" have the same length.
    const char from[] = "Orbitron", to[] = "QABroken";
    u32 renamed = 0;
    for (u32 i = 0; i + 16 <= size; ++i) {
        u32 k = 0;
        while (k < 8 && bytes[i + 2 * k] == 0 && bytes[i + 2 * k + 1] == static_cast<u8>(from[k]))
            ++k;
        if (k < 8)
            continue;
        for (k = 0; k < 8; ++k)
            bytes[i + 2 * k + 1] = static_cast<u8>(to[k]);
        ++renamed;
    }
    u32 weights = 0;
    const u32 tables = size > 12 ? byte_load_be16(bytes + 4) : 0;
    for (u32 i = 0; i < tables && 12 + i * 16 + 16 <= size; ++i) {
        const u8* record = bytes + 12 + i * 16;
        const u32 offset = byte_load_be32(record + 8);
        if (byte_load_be32(record) == sfnt_tag('O', 'S', '/', '2') && offset + 6 <= size) {
            bytes[offset + 4] = 0;
            bytes[offset + 5] = 0;
            ++weights;
        }
    }
    check(renamed && weights == 1, "broken_face_made");
    check(write_private_file("XDG_DATA_HOME", L"/fonts/broken.ttf", bytes, size), "broken_face_written");
    if (g_failures)
        return false;

    Fixture f{};
    struct Cleanup {
        Fixture& f;
        ~Cleanup() noexcept {
            native_text_catalogs_release(&f.catalogs);
            if (f.session)
                native_font_session_release(&f.session);
            arena_release(&f.scratch);
        }
    } cleanup{f};
    check(native_font_session_create(&f.session) && arena_initialize(&f.scratch, 1024 * 1024, 0, 4096) &&
              native_text_catalogs_initialize(&f.catalogs),
          "setup");
    const NativeTextFontQuery broken = ascii_query("QABroken");
    NativeFontInstanceHandle value{};
    NativeFontSelection source{};
    u32 scope = 99;
    bool fallback = false;
    // The first query makes the locator; without fontconfig the directory scan refuses the face as the text engine
    // does, and the case has nothing to show.
    const bool selected = native_text_catalog_select_query(&f.catalogs, broken, f.session, &f.scratch, &value, &source,
                                                           nullptr, nullptr, &scope, &fallback);
    SystemFontFile located;
    if (!f.catalogs.locator || !system_font_locator_find(f.catalogs.locator, {broken.family, 400, false}, &located)) {
        test_write_text("  no fontconfig lists the broken face: skipped\n");
    } else {
        check(selected && value.slot && fallback && !scope, "broken_face_falls_back");
        check(f.catalogs.failures != 0, "broken_face_counted");
    }
    native_font_instance_release(native_font_session_instances(f.session), value);
    return g_failures == 0;
}

// --desktop-font: GTK's settings.ini under the test's XDG_CONFIG_HOME (which must lie in its XDG_DATA_HOME) names
// "MaiCord Opsz Fixture", a font in $XDG_DATA_HOME/fonts with opsz and wght axes, as GNOME names Adwaita Sans. The
// interface role must name that family, and the catalog must load it and not fall back.
bool desktop_font() noexcept {
    const char* data = environment_get("XDG_DATA_HOME");
    const char* config = environment_get("XDG_CONFIG_HOME");
    bool inside = data && config && *data;
    for (u32 i = 0; inside && data[i]; ++i)
        inside = config[i] == data[i];
    check(inside, "desktop_font_private_config");
    if (g_failures)
        return false;
    static constexpr char settings[] = "[Settings]\ngtk-font-name=MaiCord Opsz Fixture 11\n";
    check(write_private_file("XDG_DATA_HOME", L"/fonts/maicord-opsz.ttf", opsz_fixture, sizeof(opsz_fixture)) &&
              write_private_file("XDG_CONFIG_HOME", L"/gtk-3.0/settings.ini", reinterpret_cast<const u8*>(settings),
                                 sizeof(settings) - 1),
          "desktop_font_written");
    if (g_failures)
        return false;
    const ConstSlice<u16> ui = default_font_name(DefaultFont::ui);
    test_write_text("  interface role: ");
    write_units(ui.data, ui.count);
    test_write_text("\n");
    static constexpr char expected[] = "MaiCord Opsz Fixture";
    bool same = ui.count == sizeof(expected) - 1;
    for (u32 i = 0; same && i < ui.count; ++i)
        same = ui[i] == static_cast<u8>(expected[i]);
    check(same, "desktop_font_is_interface_role");

    Fixture f{};
    struct Cleanup {
        Fixture& f;
        ~Cleanup() noexcept {
            native_text_catalogs_release(&f.catalogs);
            if (f.session)
                native_font_session_release(&f.session);
            arena_release(&f.scratch);
        }
    } cleanup{f};
    check(native_font_session_create(&f.session) && arena_initialize(&f.scratch, 1024 * 1024, 0, 4096) &&
              native_text_catalogs_initialize(&f.catalogs),
          "setup");
    NativeFontInstanceHandle value{};
    NativeFontSelection source{};
    u32 scope = 99;
    bool fallback = true;
    check(native_text_catalog_select_query(&f.catalogs, query(ui), f.session, &f.scratch, &value, &source, nullptr,
                                           nullptr, &scope, &fallback) &&
              value.slot && !fallback,
          "desktop_font_loads");
    native_font_instance_release(native_font_session_instances(f.session), value);
    NativeTextFontRange range{};
    check(native_text_catalog_range(&f.catalogs, query(ui), f.session, &f.scratch, utf8_literal("A"), &range) &&
              range.match.index != native_fallback_missing,
          "desktop_font_range");
    const NativeSystemFontFile* file = file_of(f.catalogs, range.match.index);
    write_choice("interface", file);
    check(file && name_starts(file->catalog, expected), "desktop_font_draws_latin");
    return g_failures == 0;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_text_catalog_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    const bool broken = startup.argc > 2 && equal(startup.argv[1], "--broken-face");
    const bool desktop = startup.argc > 1 && equal(startup.argv[1], "--desktop-font");
    const bool ok = broken    ? broken_face(startup.argv[2])
                    : desktop ? desktop_font()
                              : run(startup.argc > 1 && equal(startup.argv[1], "--scan"));
    test_write_text("checks=");
    test_write_u64(g_checks);
    test_write_text(ok ? " PASS native text catalog\n" : " FAIL native text catalog\n");
    test_process_exit(ok ? 0 : 1);
}
