#include "maicord/ui/icon.hpp"
#include "maicord/ui/icon_pack.hpp"

namespace maicord {
namespace {
GlyphPathCommand g_commands[kIconMaximumOutlineCommands];
GlyphPathCommand g_repeat[kIconMaximumOutlineCommands];
} // namespace

void test_icon_pack(void (*check)(bool, const char*) noexcept) noexcept {
    const IconPack& builtin = icon_pack_builtin();
    check(builtin.data && builtin.icon_count == static_cast<u32>(UiIcon::count) && builtin.size < 40 * 1024,
          "icon pack: built-in MIPK pack inflates, matches the manifest and stays compact");

    bool decoded = true, bounded = true;
    for (u32 i = 0; i < builtin.icon_count; ++i) {
        constexpr bool variants[]{false, true};
        for (const bool solid : variants) {
            GlyphOutline outline{g_commands, kIconMaximumOutlineCommands, 0, 0, 0, 0, 0, 0, {}, false};
            decoded = decoded && icon_pack_decode(builtin, i, {2, 0, 0, solid}, &outline) &&
                      outline.has_bounds &&
                      (outline.commands[0].kind == GlyphPathCommandKind::begin_hollow ||
                       outline.commands[0].kind == GlyphPathCommandKind::begin_filled);
            // Lucide artwork keeps to the 24-unit box; 48 px at scale 2.
            bounded = bounded && outline.minimum_x >= 0 && outline.minimum_y >= 0 && outline.maximum_x <= 48 &&
                      outline.maximum_y <= 48 &&
                      maximum(outline.maximum_x - outline.minimum_x, outline.maximum_y - outline.minimum_y) > 0;
        }
        IconStem stems[kIconMaximumStems]{};
        const u32 stem_count = icon_pack_stems(builtin, i, stems, kIconMaximumStems);
        for (u32 s = 0; s < stem_count; ++s)
            bounded = bounded && stems[s].axis <= 1 && stems[s].position >= 0 && stems[s].position <= 24;
    }
    check(decoded && bounded, "icon pack: every icon decodes in both variants within its box");

    GlyphOutline first{g_commands, kIconMaximumOutlineCommands, 0, 0, 0, 0, 0, 0, {}, false};
    GlyphOutline second{g_repeat, kIconMaximumOutlineCommands, 0, 0, 0, 0, 0, 0, {}, false};
    const u32 settings = static_cast<u32>(UiIcon::settings);
    const bool repeated = icon_pack_decode(builtin, settings, {1.25f, .5f, -.25f, false}, &first) &&
                          icon_pack_decode(builtin, settings, {1.25f, .5f, -.25f, false}, &second) &&
                          first.count == second.count &&
                          bytes_equal(g_commands, g_repeat, first.count * sizeof(GlyphPathCommand));
    GlyphOutline small{g_commands, 4, 0, 0, 0, 0, 0, 0, {}, false};
    check(repeated && !icon_pack_decode(builtin, settings, {1, 0, 0, false}, &small) && small.rejected == 1 &&
              !icon_pack_decode(builtin, builtin.icon_count, {1, 0, 0, false}, &first),
          "icon pack: decoding is deterministic and bounded by the caller's command capacity");

    PaintStream paint{};
    bool painted = paint_stream_initialize(&paint) && paint_stream_reset(&paint, 64, 64) &&
                   ui_icon_paint_styled(&paint, UiIcon::favorite, {4, 4, 28, 20}, {{1, 1, 1, 1}, {1, 0, 0, 1}, 1.25f}) &&
                   paint.count == 2;
    if (painted) {
        const auto fill = ui_icon_quad_data(paint.quads[0]), ink = ui_icon_quad_data(paint.quads[1]);
        painted = fill.solid && !ink.solid && fill.icon == static_cast<u32>(UiIcon::favorite) &&
                  ink.weight == 1.25f && paint.quads[1].left == 8 && paint.quads[1].right == 24 &&
                  paint.quads[1].top == 4 && paint.quads[1].bottom == 20 && paint.quads[1].reserved_x < 0;
    }
    painted = painted && !ui_icon_paint(&paint, UiIcon::count, {0, 0, 16, 16}, {1, 1, 1, 1}) && paint.count == 2;
    paint_stream_release(&paint);
    check(painted && ui_icon_stroke_pixels(20, 1, 1) == 1.5f && ui_icon_stroke_pixels(24, 1, 1) == 2.f &&
              ui_icon_stroke_pixels(16, 1.25f, 1) == 2.f && ui_icon_stroke_pixels(24, 2, 1) == 4.f &&
              ui_icon_button_extent(32) == 24 && ui_icon_button_extent(28) == 20 &&
              ui_icon_button_extent(24) == 16,
          "icons: centred square quads, solid underlays and quarter-pixel optical strokes");
}
} // namespace maicord
