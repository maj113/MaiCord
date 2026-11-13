// The native text catalog on Linux over the installed fonts (the counterpart of native_font_selection_cases.inc, which
// runs against Segoe UI and Bahnschrift): family lookup through the system font locator, selection, the fallback
// routes (fontconfig's order first, then the generated table), private font scopes, caching and failure handling.
// argv[1] "--scan" replaces fontconfig by the directory scan. The assertions about particular families need the Ubuntu font packages (fonts-dejavu-core, fonts-noto-core,
// fonts-noto-cjk, fonts-noto-color-emoji) and run when MAICORD_TEST_FONT_SET=ubuntu says so; the rest hold anywhere.
#include "maicord/core/arena.hpp"
#include "maicord/platform/font/default_fonts.hpp"
#include "maicord/sys/linux/process.hpp"
#include "maicord/text/opentype/bitmap_color.hpp"
#include "maicord/ui/text/native_text_catalog.hpp"

#include "../../../src/ui/text/native_text_catalog_owner.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {

#include "../../../src/ui/text/generated/linux_fallback_data.inc"

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
    check(covers(f, instance, scalar), name);
    if (family_prefix && strict()) {
        const NativeSystemFontFile* file = file_of(f.catalogs, range.match.index);
        check(file && name_starts(file->catalog, family_prefix), name);
    }
    native_font_instance_release(native_font_session_instances(f.session), instance);
    return true;
}

void test_policy_table() noexcept {
    // Table invariants beyond the compile-time ones: lists fit, the unassigned and private use ranges name nothing and
    // the CJK families follow the language.
    const NativeFallbackCascade& unspecified = system_fallback_policy;
    bool lists_fit = true;
    for (u32 i = 0; i < unspecified.ranges.count; ++i)
        lists_fit = lists_fit && unspecified.ranges[i].count <= system_fallback_max_families;
    check(lists_fit, "policy_lists_fit");
    const auto first_family = [](u32 locale, u32 scalar, const char* expected, const char* name) noexcept {
        const NativeFallbackCascade& policy = system_fallback_policy;
        const u32 route = native_fallback_cascade_find(policy, scalar);
        const NativeFallbackRange& range = policy.ranges[route];
        if (range.count == 0) {
            check(expected == nullptr, name);
            return;
        }
        const char* family = system_fallback_names + system_fallback_token(locale, range.offset);
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
    return g_failures == 0;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_text_catalog_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    const bool ok = run(startup.argc > 1 && equal(startup.argv[1], "--scan"));
    test_write_text("checks=");
    test_write_u64(g_checks);
    test_write_text(ok ? " PASS native text catalog\n" : " FAIL native text catalog\n");
    test_process_exit(ok ? 0 : 1);
}
