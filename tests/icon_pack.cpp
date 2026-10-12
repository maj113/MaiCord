#include "maicord/ui/icon.hpp"
#include "maicord/ui/icon_pack.hpp"

namespace maicord {
namespace {
constexpr Utf8View kManifestNames[]{
#define MAICORD_ICON(name, source) utf8_literal(#name),
#include "maicord/ui/icon_ids.inc"
#undef MAICORD_ICON
};

u8 g_pack[64 * 1024];
GlyphPathCommand g_commands[kIconMaximumOutlineCommands];
GlyphPathCommand g_repeat[kIconMaximumOutlineCommands];

u16 read_u16(const u8* p) noexcept {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
u32 read_u32(const u8* p) noexcept {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24);
}
void write_u16(u8* p, u16 value) noexcept {
    p[0] = static_cast<u8>(value);
    p[1] = static_cast<u8>(value >> 8);
}
// Copy the built-in pack, apply one corruption and confirm it is rejected.
template <class Corrupt> bool rejects(Corrupt corrupt, u32 size_delta = 0) noexcept {
    const IconPack& builtin = icon_pack_builtin();
    copy_bytes(g_pack, builtin.data, builtin.size);
    const u32 size = corrupt(g_pack, builtin.size) + size_delta;
    IconPack pack{};
    return icon_pack_open({g_pack, size}, &pack) != IconPackError::none && pack.data == nullptr;
}
} // namespace

void test_icon_pack(void (*check)(bool, const char*) noexcept) noexcept {
    const IconPack& builtin = icon_pack_builtin();
    IconPack opened{};
    check(icon_pack_open({builtin.data, builtin.size}, &opened) == IconPackError::none &&
              opened.icon_count == static_cast<u32>(UiIcon::count) && builtin.size < 40 * 1024,
          "icon pack: built-in MIPK pack validates, matches the manifest and stays compact");

    bool decoded = true, named = true, bounded = true;
    for (u32 i = 0; i < builtin.icon_count; ++i) {
        u32 found = ~0u;
        named = named && utf8_equal(icon_pack_name(builtin, i), kManifestNames[i]) &&
                icon_pack_find(builtin, kManifestNames[i], &found) && found == i;
        constexpr bool variants[]{false, true};
        for (const bool solid : variants) {
            GlyphOutline outline{g_commands, kIconMaximumOutlineCommands, 0, 0, 0, 0, 0, 0, {}, false};
            decoded = decoded && icon_pack_decode(builtin, i, {2, 0, 0, solid}, &outline) &&
                      outline.count == icon_pack_outline_count(builtin, i) && outline.has_bounds &&
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
    u32 missing = 0;
    check(decoded && named && bounded && !icon_pack_find(builtin, utf8_literal("discord"), &missing),
          "icon pack: every icon decodes in both variants within its box and resolves by name");

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

    const u32 commands = read_u32(builtin.data + 24), index = read_u32(builtin.data + 20),
              names = read_u32(builtin.data + 32);
    const bool malformed =
        rejects([](u8*, u32 size) noexcept { return size - 1; }) &&
        rejects([](u8*, u32) noexcept { return 39u; }) &&
        rejects([](u8* p, u32 size) noexcept { p[0] = 'X'; return size; }) &&
        rejects([](u8* p, u32 size) noexcept { p[4] = 2; return size; }) &&
        rejects([](u8* p, u32 size) noexcept { write_u16(p + 6, 0); return size; }) &&
        rejects([](u8* p, u32 size) noexcept { p[36] = 1; return size; }) &&
        rejects([](u8* p, u32 size) noexcept { p[40 + 4] = 0xFF; p[40 + 5] = 0xFF; return size; }) &&
        rejects([commands](u8* p, u32 size) noexcept { p[commands] = 9; return size; }) &&
        rejects([commands](u8* p, u32 size) noexcept { p[commands] = 2; return size; }) &&
        rejects([commands](u8* p, u32 size) noexcept { write_u16(p + commands + 1, 0x7FFF); return size; }) &&
        rejects([](u8* p, u32 size) noexcept { p[40 + 6] = static_cast<u8>(p[40 + 6] + 1); return size; }) &&
        rejects([](u8* p, u32 size) noexcept { p[40 + 10] = kIconMaximumStems + 1; return size; }) &&
        rejects([index](u8* p, u32 size) noexcept { write_u16(p + index + 2, read_u16(p + index)); return size; }) &&
        rejects([index](u8* p, u32 size) noexcept {
            const u16 a = read_u16(p + index), b = read_u16(p + index + 2);
            write_u16(p + index, b);
            write_u16(p + index + 2, a);
            return size;
        }) &&
        rejects([names](u8* p, u32 size) noexcept { p[names] = 'A'; return size; }) &&
        rejects([](u8* p, u32 size) noexcept { p[40 + 14] = 0; return size; });
    IconPack none{};
    check(malformed && icon_pack_open({nullptr, 0}, &none) == IconPackError::invalid_input,
          "icon pack: truncated, misversioned, out-of-range, unsorted and misnamed packs are rejected");

    // An overlay resolves its own records by name; generation tracks swaps.
    copy_bytes(g_pack, builtin.data, builtin.size);
    const u32 generation = icon_pack_generation();
    IconSource source{};
    const bool overlay = icon_pack_set_overlay({g_pack, builtin.size}) == IconPackError::none &&
                         icon_pack_generation() == generation + 1 &&
                         icon_pack_resolve(static_cast<u32>(UiIcon::gif), &source) && source.overlay &&
                         source.pack->data == g_pack && source.index == static_cast<u32>(UiIcon::gif);
    g_pack[0] = 'X';
    const bool invalid_kept = icon_pack_set_overlay({g_pack, builtin.size}) != IconPackError::none &&
                              icon_pack_generation() == generation + 1;
    icon_pack_clear_overlay();
    const bool restored = icon_pack_generation() == generation + 2 && icon_pack_resolve(0, &source) &&
                          !source.overlay && source.pack == &builtin &&
                          !icon_pack_resolve(builtin.icon_count, &source);
    check(overlay && invalid_kept && restored,
          "icon pack: validated overlays replace icons by name and fall back to the built-in pack");

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
