// The text system on Linux with the fonts the host has installed: the default interface family, system fallback
// through fontconfig (or the Linux table) and the platform emoji family, from the catalog to shaped glyphs. Smoke
// checks for Latin, CJK, Devanagari, Arabic and emoji: each string shapes to glyphs with width, every glyph is a real
// glyph of the face its run names, and that face covers the script. The families the Ubuntu font packages provide
// (fonts-dejavu-core, fonts-noto-core, fonts-noto-cjk, fonts-noto-color-emoji) are required when
// MAICORD_TEST_FONT_SET=ubuntu; on any other system the strings a host cannot shape are reported and skipped.
//
// argv[1]: none runs everything with the host's language; "--language <family>" (the registry sets LANG for it)
// expects the face that draws Han to belong to the family whose name starts with <family>.
#include "maicord/core/memory.hpp"
#include "maicord/os/ordinal.hpp"
#include "maicord/platform/font/default_fonts.hpp"
#include "maicord/sys/linux/process.hpp"
#include "maicord/text/fonts/family.hpp"
#include "maicord/text/fonts/run.hpp"
#include "maicord/text/opentype/bitmap_color.hpp"
#include "maicord/ui/text/text.hpp"

#include "../../../src/ui/text/native_text_catalog_owner.hpp"
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

bool strict() noexcept {
    const char* set = environment_get("MAICORD_TEST_FONT_SET");
    return set && equal(set, "ubuntu");
}

constexpr u32 kGlyphCapacity = 512;
constexpr u32 kRunCapacity = 64;

struct Storage {
    TextGlyph glyphs[kGlyphCapacity];
    TextRun runs[kRunCapacity];
    u16 clusters[kGlyphCapacity];
    wchar_t utf16[1024];
};

Storage g_storage;

struct Sample {
    const char* name;
    const char* text;
    u32 scalar; // a scalar the face of every non-Latin run must cover
    const char* family_prefix; // the family the Ubuntu set draws it with
};

// The family name the face of `run` reports (its OpenType family).
bool run_family(TextSystem& system, const TextRun& run, SystemFontName* name, bool* has_cbdt, const NativeFontFace** face_out) noexcept {
    const NativeFontRun* record = native_font_run_get(*native_font_session_runs(system.native_fonts), run.native_face);
    if (!record)
        return false;
    const NativeFontInstance* instance = native_font_instance_get(*native_font_session_instances(system.native_fonts), record->instance);
    if (!instance)
        return false;
    const NativeFontFace* face = native_font_get(*native_font_session_fonts(system.native_fonts), instance->face);
    if (!face)
        return false;
    u16 units[512];
    NativeFontFamily family{};
    if (native_font_family(face->geometry.face, {units, 512}, &family) != SfntStatus::ok ||
        family.units > kSystemFontNameCapacity)
        return false;
    for (u32 i = 0; i < family.units; ++i)
        name->units[i] = units[i];
    name->count = family.units;
    ConstSlice<u8> table{};
    *has_cbdt = sfnt_table(face->geometry.face, sfnt_tag('C', 'B', 'D', 'T'), &table) == SfntStatus::ok;
    *face_out = face;
    return true;
}

bool starts_with(const SystemFontName& name, const char* prefix) noexcept {
    u32 length = 0;
    while (prefix[length])
        ++length;
    if (name.count < length)
        return false;
    wchar_t wide[64];
    for (u32 i = 0; i < length; ++i)
        wide[i] = static_cast<u8>(prefix[i]);
    return ordinal_equal(reinterpret_cast<const wchar_t*>(name.units), static_cast<int>(length), wide, static_cast<int>(length), true);
}

void shape(TextSystem& system, const Sample& sample, const char* expect_family) noexcept {
    ShapedText shaped{};
    check(shaped_text_initialize(&shaped, {g_storage.glyphs, kGlyphCapacity}, {g_storage.runs, kRunCapacity},
                                 {g_storage.clusters, kGlyphCapacity}),
          "shaped_text_initialize");
    u32 bytes = 0;
    while (sample.text[bytes])
        ++bytes;
    const TextShapeResult result =
        text_shape_utf8(&system, {sample.text, bytes}, {}, 100000.0f, 100000.0f, {g_storage.utf16, 1024}, &shaped);
    if (!result.ok()) {
        // A host without the font for this script cannot shape it: required only with the Ubuntu packages.
        check(!strict(), sample.name);
        test_write_text("  skipped ");
        test_write_text(sample.name);
        test_write_text(" (error ");
        test_write_u64(static_cast<u64>(result.error));
        test_write_text(")\n");
        shaped_text_reset(&shaped);
        return;
    }
    check(result.glyph_count > 0 && result.run_count > 0 && shaped.width > 0.0f && shaped.height > 0.0f, sample.name);
    bool glyphs_real = true;
    for (u32 i = 0; i < result.glyph_count; ++i)
        glyphs_real = glyphs_real && shaped.glyphs[i].index != 0;
    check(glyphs_real, sample.name);
    bool covers = sample.scalar == 0;
    bool family_found = expect_family == nullptr;
    bool emoji_run_has_cbdt = false;
    for (u32 r = 0; r < shaped.run_count; ++r) {
        const TextRun& run = shaped.runs[r];
        SystemFontName name{};
        bool cbdt = false;
        const NativeFontFace* face = nullptr;
        if (!run_family(system, run, &name, &cbdt, &face)) {
            check(false, "run_font_resolves");
            continue;
        }
        emoji_run_has_cbdt = emoji_run_has_cbdt || cbdt;
        if (expect_family && starts_with(name, expect_family))
            family_found = true;
        if (sample.scalar) {
            u16 glyph = 0;
            // Some run's face draws the script (a space or a digit inside the text may use the interface font).
            covers = covers || (sfnt_cmap_glyph(face->cmap, sample.scalar, 0, &glyph) == SfntStatus::ok && glyph != 0 &&
                                run.first_glyph < shaped.glyph_count);
        }
    }
    check(covers, sample.name);
    if (strict() && expect_family)
        check(family_found, sample.name);
    if (sample.scalar == 0x1F600 && strict())
        check(emoji_run_has_cbdt, "emoji_run_uses_a_colour_bitmap_font");
    shaped_text_reset(&shaped);
}

bool run(const char* mode, const char* expected_family) noexcept {
    TextSystem system{};
    const wchar_t* ui = default_font_family(DefaultFont::ui);
    check(ui && ui[0], "default_ui_family");
    const TextShapeResult initialized = text_system_initialize(&system, ui, 16.0f);
    check(initialized.ok(), "text_system_initialize");
    if (!initialized.ok())
        return false;
    struct Release {
        TextSystem& system;
        ~Release() noexcept { text_system_release(&system); }
    } release{system};

    if (equal(mode, "--language")) {
        // The catalog's language-specific fallback: the face that draws a Han ideograph belongs to the family whose
        // name starts with argv[3] (the CJK order the user's language selects).
        const Sample han{"han_for_language", "\xE6\x97\xA5", 0x65E5, expected_family};
        shape(system, han, expected_family);
        return g_failures == 0;
    }

    // Latin: the interface font itself.
    const Sample samples[] = {
        {"latin", "The quick brown fox jumps over the lazy dog 0123456789", 'q', nullptr},
        {"cjk_japanese", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88", 0x65E5,
         "Noto Sans CJK"},
        {"cjk_korean", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", 0xD55C, "Noto Sans CJK"},
        {"devanagari", "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87", 0x0928,
         "Noto Sans Devanagari"},
        {"arabic", "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85", 0x0645,
         nullptr},
        {"thai", "\xE0\xB8\xA0\xE0\xB8\xB2\xE0\xB8\xA9\xE0\xB8\xB2\xE0\xB9\x84\xE0\xB8\x97\xE0\xB8\xA2", 0x0E20, nullptr},
        {"hebrew", "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", 0x05E9, nullptr},
        {"mixed_latin_cjk", "Hello \xE4\xB8\x96\xE7\x95\x8C world", 0, nullptr},
        {"emoji", "\xF0\x9F\x98\x80", 0x1F600, "Noto Color Emoji"},
        {"emoji_in_text", "Great job \xF0\x9F\x91\x8D!", 0, nullptr},
    };
    for (const Sample& sample : samples)
        shape(system, sample, sample.family_prefix);
    return g_failures == 0;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_system_text_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    const char* mode = startup.argc > 1 ? startup.argv[1] : "";
    const bool ok = run(mode, startup.argc > 2 ? startup.argv[2] : "");
    test_write_text("checks=");
    test_write_u64(g_checks);
    test_write_text(ok ? " PASS system text\n" : " FAIL system text\n");
    test_process_exit(ok ? 0 : 1);
}
