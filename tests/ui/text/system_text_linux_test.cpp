// The text system on Linux with the fonts the host has installed: the default interface family, system fallback
// through fontconfig (or the Linux table) and the platform emoji family, from the catalog to shaped glyphs. Smoke
// checks for Latin, CJK, Devanagari, Arabic and emoji: each string shapes to glyphs with width, every glyph is a real
// glyph of the face its run names, and that face covers the script. The families the Ubuntu font packages provide
// (fonts-dejavu-core, fonts-noto-core, fonts-noto-cjk, fonts-noto-color-emoji) are required when
// MAICORD_TEST_FONT_SET=ubuntu; on any other system the strings a host cannot shape are reported and skipped.
//
// argv[1]: none runs everything with the host's language; "--language <family>" (the registry sets LANG for it)
// expects the face that draws Han to belong to the family whose name starts with <family>.
#include "maicord/os/ordinal.hpp"
#include "maicord/platform/font/default_fonts.hpp"
#include "maicord/sys/linux/process.hpp"
#include "maicord/text/fonts/family.hpp"
#include "maicord/text/fonts/run.hpp"
#include "maicord/text/layout/paragraph_hits.hpp"
#include "maicord/ui/text/native_text_candidate.hpp"
#include "maicord/ui/text/text.hpp"
#include "maicord/render/text/edit_layout.hpp"

#include "../../support/test_output.hpp"
#include "maicord/platform/font/system_font_locator.hpp"

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
        text_shape_utf8(&system, {{sample.text, bytes}, {}, 100000.0f, 100000.0f}, &shaped);
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
    check(shaped.glyph_count > 0 && shaped.run_count > 0 && shaped.width > 0.0f && shaped.height > 0.0f, sample.name);
    bool glyphs_real = true;
    for (u32 i = 0; i < shaped.glyph_count; ++i)
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

NativeParagraphHitCluster g_hit_clusters[256];
NativeParagraphHitLine g_hit_lines[257];
float g_hit_advances[1280];
SfntFontLineParts g_hit_parts[256];
NativeParagraphHitRun g_hit_runs[256];

// The description of an embedded post as a chat message lays it out (selectable geometry under the message hit policy,
// every emoji through ordinary font fallback): its statistics line is bold. Noto Color Emoji answers for the emoji and
// is a bitmap face with nothing to embolden, so a bold request on it is drawn as the face is. Its fallback binding once
// failed to prepare, the selectable layout dereferenced the missing binding and crashed, and after the null check the
// whole paragraph was still refused. Both the selectable layout and the drawn run list must now hold the line, with
// the emoji drawn by the colour bitmap face.
void message_bold_emoji_statistics(TextSystem& base) noexcept {
    TextSystem system = base;
    system.message_hit_policy = true;
    const Utf8View text = utf8_literal(
        "ABD'de 4 yavruya hamile oldu\xC4\x9Fu d\xC3\xBC\xC5\x9F\xC3\xBCn\xC3\xBClen bir ay\xC4\xB1 g\xC3\xB6r\xC3\xBCnt"
        "\xC3\xBClendi.\n\n\xF0\x9F\x92\xAC 136\xE2\x80\x82\xF0\x9F\x94\x81 994\xE2\x80\x82\xE2\x9D\xA4\xEF\xB8\x8F "
        "17.7K\xE2\x80\x82\xF0\x9F\x91\x81\xEF\xB8\x8F 607.8K\xE2\x80\x82");
    constexpr u32 size = text_style_heading_1 | text_style_heading_3;
    const TextStyleSpan styles[] = {{0, 73, size},  {73, 4, size | text_style_bold}, {77, 7, size | text_style_bold},
                                    {84, 4, size | text_style_bold}, {88, 7, size | text_style_bold},
                                    {95, 6, size | text_style_bold}, {101, 26, size | text_style_bold}};
    check(text.bytes == 127, "statistics_text_is_127_bytes");
    NativeParagraphHits geometry{};
    NativeParagraphHitStorage retained{{g_hit_clusters, 256}, {g_hit_lines, 257}, {g_hit_advances, 1280},
                                       &geometry,             {g_hit_parts, 256}, {g_hit_runs, 256}};
    TextMeasureResult measured{};
    const bool selectable =
        native_text_candidate_hits(system, {text, {styles, 7}, 1000.0f, 1000.0f}, retained, &measured);
    ShapedText shaped{};
    check(shaped_text_initialize(&shaped, {g_storage.glyphs, kGlyphCapacity}, {g_storage.runs, kRunCapacity},
                                 {g_storage.clusters, kGlyphCapacity}),
          "shaped_text_initialize");
    const bool drawn =
        native_text_candidate_styled(system, {text, {styles, 7}, 1000.0f, 1000.0f, TextWrapping::wrap}, &shaped, nullptr);
    bool emoji_face = false, glyphs_real = drawn;
    for (u32 r = 0; drawn && r < shaped.run_count; ++r) {
        SystemFontName name{};
        bool cbdt = false;
        const NativeFontFace* face = nullptr;
        if (run_family(system, shaped.runs[r], &name, &cbdt, &face))
            emoji_face = emoji_face || cbdt;
    }
    for (u32 i = 0; drawn && i < shaped.glyph_count; ++i)
        glyphs_real = glyphs_real && shaped.glyphs[i].index != 0;
    if (strict()) {
        check(selectable && geometry.lines.count >= 3, "bold_bitmap_colour_fallback_selectable");
        check(drawn && glyphs_real, "bold_bitmap_colour_fallback_drawn_without_tofu");
        check(emoji_face, "bold_bitmap_colour_fallback_uses_the_colour_bitmap_face");
    }
    shaped_text_reset(&shaped);
    const Sample latin{"latin_after_bold_emoji_paragraph", "The quick brown fox", 'q', nullptr};
    shape(base, latin, nullptr);
}

TextDecoration g_decorations[16];

struct LineProbe {
    bool ok = false;
    float height = 0, baseline = 0, pill_height = 0;
    bool pill = false, fallback_face = false;
};

// The first line of `text` laid out as a chat message under `flags` over all of it: its measured height, the baseline
// the retained geometry reports (the taller ascent of the line's spans) and, for a mention, the box of its background.
LineProbe probe_line(const TextSystem& base, Utf8View text, u32 flags) noexcept {
    TextSystem system = base;
    system.message_hit_policy = true;
    const TextStyleSpan styles[] = {{0, text.bytes, flags}};
    const ConstSlice<TextStyleSpan> spans{styles, flags ? 1u : 0u};
    LineProbe probe;
    NativeParagraphHits geometry{};
    NativeParagraphHitStorage retained{{g_hit_clusters, 256}, {g_hit_lines, 257}, {g_hit_advances, 1280},
                                       &geometry,             {g_hit_parts, 256}, {g_hit_runs, 256}};
    TextMeasureResult measured{};
    if (!native_text_candidate_hits(system, {text, spans, 1000.0f, 1000.0f, TextWrapping::wrap,
                                      TextOverflow::clip}, retained, &measured) ||
        !measured.ok() || geometry.lines.count < 1)
        return probe;
    probe.height = measured.height;
    probe.baseline = geometry.style_count ? geometry.styles[geometry.lines[0].ascent_style].baseline
                                          : geometry.uniform.baseline;
    ShapedText shaped{};
    check(shaped_text_initialize(&shaped, {g_storage.glyphs, kGlyphCapacity}, {g_storage.runs, kRunCapacity},
                                 {g_storage.clusters, kGlyphCapacity}, {g_decorations, 16}),
          "shaped_text_initialize");
    if (native_text_candidate_styled(system, {text, spans, 1000.0f, 1000.0f, TextWrapping::wrap}, &shaped, nullptr)) {
        for (u32 i = 0; i < shaped.decoration_count; ++i)
            if (g_decorations[i].kind == TextDecorationKind::background && (g_decorations[i].style_flags & text_style_mention)) {
                probe.pill = true;
                probe.pill_height = g_decorations[i].bottom - g_decorations[i].top;
            }
        for (u32 r = 0; r < shaped.run_count; ++r) {
            SystemFontName name{};
            bool cbdt = false;
            const NativeFontFace* face = nullptr;
            probe.fallback_face = probe.fallback_face || (run_family(system, shaped.runs[r], &name, &cbdt, &face) &&
                                                          (starts_with(name, "Noto Sans Symbols") || cbdt));
        }
        probe.ok = true;
    }
    shaped_text_reset(&shaped);
    return probe;
}

bool within_a_pixel(float a, float b) noexcept {
    const float difference = a - b;
    return difference > -1.0f && difference < 1.0f;
}

// A face used only for fallback glyphs sets neither the line, nor its baseline, nor a pill: Noto Sans Symbols (circled
// letters) is 2.05 em tall against the interface font's 1.36, Noto Sans CJK and Noto Color Emoji differ too, and the
// union of their metrics lifted a message's display name over its timestamp and stretched every mention pill. The
// same text in ASCII is the reference (Discord's fixed line height leaves a line as tall as the primary font makes it).
void fallback_keeps_the_line_box(const TextSystem& system) noexcept {
    struct Pair {
        const char* name;
        const char* ascii;
        const char* fallback;
        u32 flags;
        bool symbol; // the Ubuntu set draws the fallback text with Noto Sans Symbols or Noto Color Emoji
    };
    const Pair pairs[] = {
        {"display_name", "RAT", "\xF0\x9F\x85\xA1\xF0\x9F\x85\x90\xF0\x9F\x85\xA3", text_style_bold, true},
        {"mention_pill", "@MRE", "@\xF0\x9F\x85\x9C\xF0\x9F\x85\xA1\xF0\x9F\x85\x94", text_style_mention, true},
        {"inline_symbols", "fax AR bro", "fax \xF0\x9F\x85\x90\xF0\x9F\x85\xA1 bro", 0, true},
        {"emoji_line", "ok ok", "ok \xF0\x9F\x98\x80", 0, true},
        {"cjk_line", "abc", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 0, false},
    };
    for (const Pair& pair : pairs) {
        u32 ascii_bytes = 0, other_bytes = 0;
        while (pair.ascii[ascii_bytes])
            ++ascii_bytes;
        while (pair.fallback[other_bytes])
            ++other_bytes;
        const LineProbe ascii = probe_line(system, {pair.ascii, ascii_bytes}, pair.flags);
        const LineProbe other = probe_line(system, {pair.fallback, other_bytes}, pair.flags);
        if (!other.ok || !ascii.ok) {
            check(!strict(), pair.name);
            continue;
        }
        check(within_a_pixel(ascii.height, other.height), pair.name);
        check(within_a_pixel(ascii.baseline, other.baseline), pair.name);
        check(ascii.pill == other.pill && within_a_pixel(ascii.pill_height, other.pill_height), pair.name);
        if (pair.flags == text_style_mention)
            check(ascii.pill && ascii.pill_height > 0.0f, "mention_pill_is_drawn");
        if (strict() && pair.symbol)
            check(other.fallback_face, pair.name);
    }
}

// Writes the family of the face that draws the first glyph of `shaped`, for the logs of other distributions.
void write_first_family(TextSystem& system, const ShapedText& shaped) noexcept {
    SystemFontName name{};
    bool cbdt = false;
    const NativeFontFace* face = nullptr;
    char buffer[kSystemFontNameCapacity];
    u32 used = 0;
    if (shaped.run_count && run_family(system, shaped.runs[0], &name, &cbdt, &face))
        for (; used < name.count && used < sizeof(buffer); ++used)
            buffer[used] = name.units[used] < 128 ? static_cast<char>(name.units[used]) : '?';
    test_write_bytes(buffer, used);
}

// Whether `shaped` holds real glyphs at full CJK width: the native fallback substitute of a refused text draws each
// scalar as the narrow missing glyph of the interface font, the boxes the composer showed. The face that covers
// `scalar` must not be the last-resort fixture (tests/fixtures/fonts/last_resort), whose glyphs are boxes too.
bool full_width_glyphs(TextSystem& system, const ShapedText& shaped, u32 scalars, u32 scalar) noexcept {
    bool real = shaped.glyph_count >= scalars && shaped.width >= scalars * system.em_size * 0.8f;
    for (u32 i = 0; real && i < shaped.glyph_count; ++i)
        real = shaped.glyphs[i].index != 0;
    bool covers = false;
    for (u32 r = 0; real && r < shaped.run_count; ++r) {
        SystemFontName name{};
        bool cbdt = false;
        const NativeFontFace* face = nullptr;
        u16 glyph = 0;
        covers = covers || (run_family(system, shaped.runs[r], &name, &cbdt, &face) &&
                            !starts_with(name, "MaiCord Last Resort") &&
                            sfnt_cmap_glyph(face->cmap, scalar, 0, &glyph) == SfntStatus::ok && glyph != 0);
    }
    return real && covers;
}

TextEditMetric g_edit_selections[kTextEditSelectionCapacity];

// Japanese in the message composer, as an input method types it: kana alone (the preedit before conversion), the
// converted kanji and the committed mix. The composer lays its draft out through the edit field (text_edit_layout_measure
// with the composer's options, the caret at the end of the preedit) and draws it with text_shape_utf8 in a one-line
// box; a message lays out its hit geometry under the message policy and draws in a tall box. Both must draw the CJK
// face at full width, in either order on one text system, as the client runs them.
void composer_cjk(TextSystem& system) noexcept {
    struct Case {
        const char* name;
        const char* text;
        u32 scalars;
        u32 scalar;
    };
    const Case cases[] = {
        {"hiragana", "\xE3\x81\x8A\xE3\x81\xA3\xE3\x81\xB1\xE3\x81\x84", 4, 0x304A},
        {"katakana", "\xE3\x82\xAA\xE3\x83\x83\xE3\x83\x91\xE3\x82\xA4", 4, 0x30AA},
        {"kanji", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 3, 0x65E5},
        {"mixed", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88", 7, 0x65E5},
    };
    for (u32 order = 0; order < 2; ++order) {
        for (const Case& sample : cases) {
            u32 bytes = 0;
            while (sample.text[bytes])
                ++bytes;
            const Utf8View text{sample.text, bytes};
            const float line = system.em_size * 1.5f;
            bool edit_ok = false, composer_ok = false, message_ok = false;
            const auto edit_field = [&]() noexcept {
                TextEditLayoutOwner owner{};
                owner.selections = g_edit_selections;
                owner.selection_capacity = kTextEditSelectionCapacity;
                const TextEditLayoutInput input{text, text_edit_signature(text), sample.scalars, sample.scalars, 0, 0,
                                                600.0f, line, 262144.0f,
                                                text_edit_layout_wrap | text_edit_layout_center_vertically |
                                                    text_edit_layout_scroll_to_caret,
                                                {}};
                edit_ok = text_edit_layout_measure(&owner, input, system) && owner.geometry &&
                          owner.caret.left >= sample.scalars * system.em_size * 0.8f;
                text_edit_layout_release(&owner);
                ShapedText shaped{};
                check(shaped_text_initialize(&shaped, {g_storage.glyphs, kGlyphCapacity}, {g_storage.runs, kRunCapacity},
                                             {g_storage.clusters, kGlyphCapacity}),
                      "shaped_text_initialize");
                composer_ok = text_shape_utf8(&system, {text, {}, 600.0f, line, TextWrapping::wrap, TextOverflow::clip},
                                              &shaped).ok() &&
                              full_width_glyphs(system, shaped, sample.scalars, sample.scalar);
                test_write_text("  composer ");
                test_write_text(sample.name);
                test_write_text(": ");
                write_first_family(system, shaped);
                test_write_text("\n");
                shaped_text_reset(&shaped);
            };
            const auto message = [&]() noexcept {
                TextSystem hit_system = system;
                hit_system.tracking = 0;
                hit_system.message_hit_policy = true;
                NativeParagraphHits geometry{};
                NativeParagraphHitStorage retained{{g_hit_clusters, 256}, {g_hit_lines, 257}, {g_hit_advances, 1280},
                                                   &geometry,             {g_hit_parts, 256}, {g_hit_runs, 256}};
                TextMeasureResult measured{};
                const bool hits = native_text_candidate_hits(hit_system, {text, {}, 600.0f, 100000.0f}, retained, &measured);
                ShapedText shaped{};
                check(shaped_text_initialize(&shaped, {g_storage.glyphs, kGlyphCapacity}, {g_storage.runs, kRunCapacity},
                                             {g_storage.clusters, kGlyphCapacity}),
                      "shaped_text_initialize");
                message_ok = hits && text_shape_utf8(&system, {text, {}, 600.0f, 100000.0f}, &shaped).ok() &&
                             full_width_glyphs(system, shaped, sample.scalars, sample.scalar);
                shaped_text_reset(&shaped);
            };
            if (order) {
                message();
                edit_field();
            } else {
                edit_field();
                message();
            }
            if (!composer_ok && !strict() && !message_ok) {
                // A host without a CJK font shapes neither: required only with the Ubuntu packages.
                test_write_text("  skipped composer_cjk ");
                test_write_text(sample.name);
                test_write_text("\n");
                continue;
            }
            check(edit_ok, sample.name);
            check(composer_ok, sample.name);
            check(message_ok, sample.name);
        }
    }
}

// The composer cases with a CJK face as the primary face, as a desktop font or fontconfig's sans-serif can make it:
// the variable CFF2 collection (NixOS, Fedora) or the static CFF one (Ubuntu), a face at index 0 and faces at a
// later index. A family the host lacks is skipped.
void composer_cjk_primary_faces(TextSystem& base) noexcept {
    const char* const families[] = {"Noto Sans CJK JP", "Noto Sans CJK SC", "Noto Sans CJK KR", "Noto Sans Mono CJK JP"};
    for (const char* family : families) {
        u16 units[64];
        u32 count = 0;
        for (; family[count]; ++count)
            units[count] = static_cast<u8>(family[count]);
        TextSystem system = base;
        if (!native_text_format_create(system.native_text,
                                       {{units, count}, 0, {400, 5, 0}, system.em_size, system.em_size * 4, true},
                                       &system.native_format) ||
            !native_text_format_prepare(system.native_text, system.native_format)) {
            test_write_text("  no primary face ");
            test_write_text(family);
            test_write_text("\n");
            continue;
        }
        test_write_text("  primary face ");
        test_write_text(family);
        test_write_text("\n");
        composer_cjk(system);
    }
}

// With tests/fixtures/fonts/refused/fonts.conf the only face that covers Hiragana is one the engine cannot prepare.
// The kana are then missing glyphs of the interface font, and the Latin letters keep their own glyphs: the native
// layout itself succeeds, at the regular and the bold weight, in the line, styled, hit and edit-field paths. Before,
// the whole text failed, and when the fallback substitutes failed too, one message took down every text of the view.
void refused_fallback_face(TextSystem& base) noexcept {
    // The base face is Zilla Slab, a static TrueType face: its bold is a simulation the engine draws. A CFF face of the
    // set (Compagnon) draws a simulated bold too, and fontconfig may pick it as the interface face.
    TextSystem system = base;
    const u16 family[] = {'Z', 'i', 'l', 'l', 'a', ' ', 'S', 'l', 'a', 'b'};
    const bool format = native_text_format_create(system.native_text, {{family, 10}, 0, {400, 5, 0}, 16.0f, 64.0f, true},
                                                  &system.native_format) &&
                        native_text_format_prepare(system.native_text, system.native_format);
    check(format, "refused_face_base_format");
    if (!format)
        return;
    // The interface face of this font set, for the log.
    const wchar_t* ui = default_font_family(DefaultFont::ui);
    test_write_text("  interface family: ");
    for (u32 i = 0; ui && ui[i]; ++i) {
        const char c = ui[i] < 128 ? static_cast<char>(ui[i]) : '?';
        test_write_bytes(&c, 1);
    }
    test_write_text("\n");
    // Bold Latin alone tells a bold base face that fails from a fallback face that fails.
    {
        const Utf8View latin = utf8_literal("Abc");
        const TextStyleSpan style{0, latin.bytes, static_cast<u32>(text_style_bold)};
        ShapedText shaped{};
        check(shaped_text_initialize(&shaped, {g_storage.glyphs, kGlyphCapacity}, {g_storage.runs, kRunCapacity},
                                     {g_storage.clusters, kGlyphCapacity}),
              "shaped_text_initialize");
        check(native_text_candidate_styled(system, {latin, {&style, 1}, 1000.0f, 1000.0f}, &shaped, nullptr),
              "refused_face_bold_latin");
        shaped_text_reset(&shaped);
    }
    const Utf8View text = utf8_literal("Abc \xE3\x81\x82\xE3\x81\x84\xE3\x81\x86");
    for (u32 bold = 0; bold < 2; ++bold) {
        const TextStyleSpan style{0, text.bytes, bold ? static_cast<u32>(text_style_bold) : 0u};
        const ConstSlice<TextStyleSpan> styles{&style, bold};
        ShapedText shaped{};
        check(shaped_text_initialize(&shaped, {g_storage.glyphs, kGlyphCapacity}, {g_storage.runs, kRunCapacity},
                                     {g_storage.clusters, kGlyphCapacity}),
              "shaped_text_initialize");
        const bool drawn = bold ? native_text_candidate_styled(system, {text, styles, 1000.0f, 1000.0f}, &shaped, nullptr)
                                : native_text_candidate_line(system, {text, {}, 1000.0f, 1000.0f}, &shaped, nullptr);
        check(drawn && shaped.glyph_count >= 7 && shaped.glyphs[0].index != 0, bold ? "refused_face_bold" : "refused_face");
        shaped_text_reset(&shaped);
        NativeParagraphHits geometry{};
        NativeParagraphHitStorage retained{{g_hit_clusters, 256}, {g_hit_lines, 257}, {g_hit_advances, 1280},
                                           &geometry,             {g_hit_parts, 256}, {g_hit_runs, 256}};
        TextMeasureResult measured{};
        check(native_text_candidate_hits(system, {text, styles, 1000.0f, 1000.0f}, retained, &measured) && measured.ok(),
              bold ? "refused_face_bold_hits" : "refused_face_hits");
        TextEditLayoutOwner owner{};
        owner.selections = g_edit_selections;
        owner.selection_capacity = kTextEditSelectionCapacity;
        const TextEditLayoutInput input{text, text_edit_signature(text), 7, 7, 0, 0, 600.0f, 24.0f, 262144.0f,
                                        text_edit_layout_wrap, styles};
        check(text_edit_layout_measure(&owner, input, system) && owner.geometry,
              bold ? "refused_face_bold_edit" : "refused_face_edit");
        text_edit_layout_release(&owner);
    }
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
    if (equal(mode, "--refused-face")) {
        refused_fallback_face(system);
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
    message_bold_emoji_statistics(system);
    fallback_keeps_the_line_box(system);
    composer_cjk(system);
    composer_cjk_primary_faces(system);
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
