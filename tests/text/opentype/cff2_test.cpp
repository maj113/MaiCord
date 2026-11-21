// CFF2 outlines (Linux only): the first-party collection tests/generated/cff2_collection_fixture.bin
// (tools/font_reference/make_cff2_fixture.py), two variable faces with CFF2 outlines in one file, the layout of Noto Sans
// CJK on NixOS and Fedora. Each face opens, names its family and its weight instances, and draws its glyphs at the
// default instance and at blended weights, regular and bold. A changed version, an operand stack without the blend
// operands and an unknown vsindex are refused.
#include "maicord/core/memory.hpp"
#include "maicord/text/fonts/family.hpp"
#include "maicord/text/fonts/geometry.hpp"

#include "../../support/test_output.hpp"
#include "../../support/text_seams.hpp"

namespace maicord {
namespace {

#include "../../generated/cff2_collection_fixture.inc"

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

GlyphPathCommand g_commands[64];

GlyphOutline outline() noexcept {
    GlyphOutline out{};
    out.commands = g_commands;
    out.capacity = 64;
    return out;
}

bool near(float a, float b) noexcept {
    return a >= b - 1.e-6f && a <= b + 1.e-6f;
}

// The 300 by 50 box every fixture glyph draws, its first corner at (x, 100) in design units: a move, three lines and
// the close.
bool box_at(const GlyphOutline& out, double x) noexcept {
    const float left = static_cast<float>(x / 1000.0);
    return out.count == 5 && out.commands[0].kind == GlyphPathCommandKind::begin_filled && near(out.commands[0].x1, left) &&
           near(out.commands[0].y1, -.1f) && near(out.commands[1].x1, left + .3f) &&
           out.commands[4].kind == GlyphPathCommandKind::end_closed && near(out.minimum_x, left) &&
           near(out.maximum_x, left + .3f) && near(out.minimum_y, -.15f);
}

// The same box drawn bold: the pen square of 1310/65536 em moves its right side right and its top up, and leaves its
// left side and its bottom where they are.
bool bold_box_at(const GlyphOutline& out, double x) noexcept {
    const float left = static_cast<float>(x / 1000.0), strength = 1310.f / 65536.f;
    const float right = left + .3f + strength, top = -.15f - strength;
    const auto* c = out.commands;
    return out.count == 5 && c[0].kind == GlyphPathCommandKind::begin_filled && near(c[0].x1, left) && near(c[0].y1, -.1f) &&
           near(c[1].x1, right) && near(c[1].y1, -.1f) && near(c[2].x1, right) && near(c[2].y1, top) &&
           near(c[3].x1, left) && near(c[3].y1, top) && c[4].kind == GlyphPathCommandKind::end_closed &&
           near(out.minimum_x, left) && near(out.maximum_x, right) && near(out.minimum_y, top) && near(out.maximum_y, -.1f);
}

AxisSegmentMap g_maps[4];
double g_coordinates[4];
SfntGvarInstance g_variation;

// The instance at `settings` (none: the default instance), its coordinates in the storage above.
SfntStatus instance_at(const FontGeometry& geometry, ConstSlice<FontAxisSetting> settings,
                       FontGeometryInstance* output) noexcept {
    return font_geometry_instance(geometry, settings, {{g_maps, 4}, {g_coordinates, 4}, {}, &g_variation}, output);
}

void test_face(u32 index, const char* family) noexcept {
    SfntFace face{};
    check(sfnt_open({cff2_collection_fixture, sizeof(cff2_collection_fixture)}, index, &face) == SfntStatus::ok, "face_open");
    FontGeometry geometry{};
    check(font_geometry_open(face, &geometry) == SfntStatus::ok && geometry.kind == FontGeometryKind::cff &&
              geometry.outline.cff.cff2 && geometry.outline.cff.variations.region_count == 1 && geometry.axes.axis_count == 1,
          "geometry_open");
    // The catalog's view: the family of this face and one candidate for each named instance.
    u16 name[64];
    NativeFontFamily resolved{};
    NativeFontCandidates candidates{};
    check(native_font_family_candidates(face, {name, 64}, &resolved, &candidates) == SfntStatus::ok &&
              candidates.count == 3,
          "family_candidates");
    u32 length = 0;
    while (family[length])
        ++length;
    bool same = resolved.units == length;
    for (u32 i = 0; same && i < length; ++i)
        same = name[i] == static_cast<u8>(family[i]);
    check(same, "family_name");
    NativeFontCandidate regular{};
    check(native_font_candidate(candidates, 1, &regular) == SfntStatus::ok && regular.properties.weight == 400,
          "regular_candidate");

    // The default instance (wght 100), then wght 500 (half of the region) and 900 (all of it).
    auto out = outline();
    FontGeometryInstance instance{};
    check(instance_at(geometry, {}, &instance) == SfntStatus::ok, "default_instance");
    for (u16 glyph = 1; glyph <= 3; ++glyph) {
        check(font_geometry_outline(instance, glyph, nullptr, &out) == SfntStatus::ok && box_at(out, 100), "default_box");
        check(cff_outline(geometry.outline.cff, glyph, &out) == SfntStatus::ok && box_at(out, 100), "cff_outline_default");
        check(font_geometry_outline(instance, glyph, nullptr, &out, 1000000, false, true) == SfntStatus::ok &&
                  bold_box_at(out, 100),
              "default_bold_box");
    }
    const double weights[]{100, 500, 900};
    const double lefts[]{100, 150, 200};
    for (u32 i = 0; i < 3; ++i) {
        const FontAxisSetting setting{sfnt_tag('w', 'g', 'h', 't'), weights[i]};
        check(instance_at(geometry, {&setting, 1}, &instance) == SfntStatus::ok, "weight_instance");
        for (u16 glyph = 1; glyph <= 3; ++glyph) {
            check(font_geometry_outline(instance, glyph, nullptr, &out) == SfntStatus::ok && box_at(out, lefts[i]),
                  "blended_box");
            check(font_geometry_outline(instance, glyph, nullptr, &out, 1000000, false, true) == SfntStatus::ok &&
                      bold_box_at(out, lefts[i]),
                  "blended_bold_box");
        }
        // The ink of every glyph at this instance is the box of its whole outline.
        const ConstSlice<double> coordinates = instance.variation ? instance.variation->coordinates : ConstSlice<double>{};
        for (u16 glyph = 0; glyph < geometry.outline.cff.glyph_count; ++glyph) {
            const SfntStatus status = font_geometry_outline(instance, glyph, nullptr, &out);
            const bool box = out.has_bounds && out.maximum_x > out.minimum_x && out.maximum_y > out.minimum_y;
            bool ink = !box;
            check(cff_glyph_ink(geometry.outline.cff, glyph, coordinates, &ink) == status &&
                      (status != SfntStatus::ok || ink == box) && (glyph == 0 || ink),
                  "blended_ink");
        }
    }
}

// The fixture's bytes with one change, for the refusals.
u8 g_copy[sizeof(cff2_collection_fixture)];

ConstSlice<u8> cff2_table(SfntFace* face) noexcept {
    copy_bytes(g_copy, cff2_collection_fixture, sizeof(g_copy));
    ConstSlice<u8> table{};
    if (sfnt_open({g_copy, sizeof(g_copy)}, 0, face) != SfntStatus::ok ||
        sfnt_table(*face, sfnt_tag('C', 'F', 'F', '2'), &table) != SfntStatus::ok)
        return {};
    return table;
}

// The first occurrence of `pattern` in the CFF2 table, as a writable pointer into the copy.
u8* find(ConstSlice<u8> table, const u8* pattern, u32 bytes) noexcept {
    for (u32 at = 0; at + bytes <= table.count; ++at)
        if (bytes_equal(table.data + at, pattern, bytes))
            return const_cast<u8*>(table.data + at);
    return nullptr;
}

void test_refusals() noexcept {
    SfntFace face{};
    FontGeometry geometry{};
    ConstSlice<u8> table = cff2_table(&face);
    check(table.count > 5, "table");
    const_cast<u8*>(table.data)[0] = 3;
    check(font_geometry_open(face, &geometry) == SfntStatus::unsupported, "version_refused");

    // "bar" begins 100 100 1 blend: with no count operand blend has nothing to read.
    static constexpr u8 bar[]{139 + 100, 139 + 100, 139 + 1, 16};
    table = cff2_table(&face);
    u8* program = find(table, bar, sizeof(bar));
    check(program != nullptr, "bar_program");
    if (program) {
        program[2] = 139 + 2;
        check(font_geometry_open(face, &geometry) == SfntStatus::ok, "blend_count_open");
        auto out = outline();
        FontGeometryInstance instance{};
        check(instance_at(geometry, {}, &instance) == SfntStatus::ok &&
                  font_geometry_outline(instance, 1, nullptr, &out) == SfntStatus::malformed && out.rejected && !out.count,
              "blend_count_refused");
    }

    // "subrs" selects item variation data 0; data 1 does not exist.
    static constexpr u8 vsindex[]{139, 15};
    table = cff2_table(&face);
    program = find(table, vsindex, sizeof(vsindex));
    check(program != nullptr, "vsindex_program");
    if (program) {
        program[0] = 139 + 1;
        check(font_geometry_open(face, &geometry) == SfntStatus::ok, "vsindex_open");
        auto out = outline();
        FontGeometryInstance instance{};
        check(instance_at(geometry, {}, &instance) == SfntStatus::ok &&
                  font_geometry_outline(instance, 2, nullptr, &out) == SfntStatus::malformed && out.rejected,
              "vsindex_refused");
    }
}

// Every single-bit change of the CFF2 table: the open and each outline at two instances return a status within the
// work budget, and a refused outline is empty.
void test_bit_flips() noexcept {
    SfntFace face{};
    const u32 bytes = static_cast<u32>(cff2_table(&face).count);
    u32 opened = 0, drawn = 0, bad = 0;
    const FontAxisSetting heavy{sfnt_tag('w', 'g', 'h', 't'), 900};
    for (u32 at = 0; at < bytes; ++at) {
        for (u32 bit = 0; bit < 8; ++bit) {
            const ConstSlice<u8> table = cff2_table(&face);
            const_cast<u8*>(table.data)[at] ^= static_cast<u8>(1u << bit);
            FontGeometry geometry{};
            if (font_geometry_open(face, &geometry) != SfntStatus::ok)
                continue;
            ++opened;
            for (u32 settings = 0; settings < 2; ++settings) {
                FontGeometryInstance instance{};
                if (instance_at(geometry, {&heavy, settings}, &instance) != SfntStatus::ok)
                    continue;
                for (u16 glyph = 0; glyph < 4; ++glyph) {
                    auto out = outline();
                    if (font_geometry_outline(instance, glyph, nullptr, &out, 10000) == SfntStatus::ok)
                        ++drawn;
                    else if (out.count || !out.rejected)
                        ++bad;
                }
            }
        }
    }
    check(bytes && opened && drawn && !bad, "bit_flips");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_cff2_entry) {
    using namespace maicord;
    test_face(0, "MaiCord CFF2 Fixture");
    test_face(1, "MaiCord CFF2 Fixture B");
    test_refusals();
    test_bit_flips();
    test_exit(g_failures, "cff2 outlines");
}
