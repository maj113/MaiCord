// CBLC/CBDT: the generated fixture (every index subtable format, every PNG image format, three strikes) for the exact
// behaviour, and the installed Noto Color Emoji for the real thing (Linux: through the system font locator).
// argv[1]: none runs both; "--fixture" only the fixture.
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/os/file.hpp"
#include "maicord/platform/font/system_font_locator.hpp"
#include "maicord/sys/linux/process.hpp"
#include "maicord/text/opentype/bitmap_color.hpp"

#include "../../support/test_output.hpp"

namespace maicord {
#include "../../generated/cbdt_fixture.inc"
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

bool is_png(const CbdtGlyph& glyph) noexcept {
    static constexpr u8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    return glyph.png.count > 8 && bytes_equal(glyph.png.data, signature, 8);
}

u32 total_alpha(const BitmapColorGlyph& tile) noexcept {
    u32 sum = 0;
    for (u32 i = 0; i < tile.width * tile.height; ++i)
        sum += tile.pixels[i] >> 24;
    return sum;
}

// A premultiplied pixel never has a colour above its alpha.
bool premultiplied(const BitmapColorGlyph& tile) noexcept {
    for (u32 i = 0; i < tile.width * tile.height; ++i) {
        const u32 p = tile.pixels[i];
        const u32 a = p >> 24;
        if ((p & 0xFF) > a || ((p >> 8) & 0xFF) > a || ((p >> 16) & 0xFF) > a)
            return false;
    }
    return true;
}

constexpr u32 pack(u32 r, u32 g, u32 b, u32 a) noexcept {
    return r | g << 8 | b << 16 | a << 24;
}

struct Scratch {
    Arena arena{};
    Scratch() noexcept { arena_initialize(&arena, 16u * 1024u * 1024u, 0, 64 * 1024); }
    ~Scratch() noexcept { arena_release(&arena); }
};

void test_fixture() noexcept {
    SfntFace face{};
    check(sfnt_open({cbdt_fixture, sizeof(cbdt_fixture)}, 0, &face) == SfntStatus::ok, "fixture_sfnt");
    CbdtFont font{};
    check(cbdt_open(face, &font) == SfntStatus::ok && font.strike_count == 3, "fixture_open");

    CbdtStrike strike{};
    check(cbdt_strike(font, 0, &strike) == SfntStatus::ok && strike.ppem_y == 16 && strike.ppem_x == 16 &&
              strike.bit_depth == 32 && strike.first_glyph == 1 && strike.last_glyph == 12 && strike.subtable_count == 5,
          "fixture_strike_16");
    check(cbdt_strike(font, 1, &strike) == SfntStatus::ok && strike.ppem_y == 32 && strike.last_glyph == 3,
          "fixture_strike_32");
    check(cbdt_strike(font, 2, &strike) == SfntStatus::ok && strike.ppem_y == 64 && strike.last_glyph == 2,
          "fixture_strike_64");
    check(cbdt_strike(font, 3, &strike) == SfntStatus::invalid_input, "fixture_strike_range");

    // Every index format and image format.
    struct Case {
        u32 strike;
        u16 glyph;
        u8 width, height;
        i8 bearing_x, bearing_y;
        u8 advance;
        const char* name;
    };
    const Case cases[] = {
        {0, 1, 8, 8, 2, 8, 9, "glyph_1_index1_image17"},   {0, 2, 8, 8, 1, 7, 9, "glyph_2_index1_image17"},
        {0, 3, 4, 4, 0, 4, 5, "glyph_3_index2_image19"},   {0, 4, 6, 6, 0, 6, 7, "glyph_4_index3_image17"},
        {0, 6, 5, 5, 0, 5, 6, "glyph_6_index5_image19"},   {0, 8, 5, 5, 0, 5, 6, "glyph_8_index5_image19"},
        {0, 10, 7, 7, 0, 7, 8, "glyph_10_index4_image18"}, {0, 12, 7, 7, 0, 7, 8, "glyph_12_index4_image18"},
        {1, 1, 16, 16, 4, 16, 18, "glyph_1_strike_32_image18"}, {1, 2, 16, 16, 2, 14, 18, "glyph_2_strike_32_image17"},
        {1, 3, 8, 8, 0, 8, 10, "glyph_3_strike_32_image19"},   {2, 1, 32, 32, 8, 32, 36, "glyph_1_strike_64"},
        {2, 2, 32, 32, 4, 28, 36, "glyph_2_strike_64"},
    };
    for (const Case& test : cases) {
        CbdtGlyph glyph{};
        const bool ok = cbdt_glyph(font, test.strike, test.glyph, &glyph) == SfntStatus::ok;
        check(ok && is_png(glyph) && glyph.width == test.width && glyph.height == test.height &&
                  glyph.bearing_x == test.bearing_x && glyph.bearing_y == test.bearing_y && glyph.advance == test.advance,
              test.name);
    }
    // Glyphs inside a strike's range that no subtable lists, and glyphs outside it.
    CbdtGlyph glyph{};
    check(cbdt_glyph(font, 0, 5, &glyph) == SfntStatus::glyph_out_of_range, "glyph_5_absent");
    check(cbdt_glyph(font, 0, 7, &glyph) == SfntStatus::glyph_out_of_range, "glyph_7_absent_in_index5");
    check(cbdt_glyph(font, 0, 11, &glyph) == SfntStatus::glyph_out_of_range, "glyph_11_absent_in_index4");
    check(cbdt_glyph(font, 0, 13, &glyph) == SfntStatus::glyph_out_of_range, "glyph_13_outside_strike");
    check(cbdt_glyph(font, 1, 4, &glyph) == SfntStatus::glyph_out_of_range, "glyph_4_outside_strike_32");
    check(cbdt_glyph(font, 5, 1, &glyph) == SfntStatus::invalid_input, "strike_index_invalid");

    // Strike selection: the smallest strike at least the target size, else the largest, skipping strikes without
    // the glyph.
    u32 selected = 99;
    const struct {
        float size;
        u16 glyph;
        u32 strike;
        const char* name;
    } selections[] = {
        {16, 1, 0, "select_exact"},      {5, 1, 0, "select_tiny"},       {17, 1, 1, "select_next_up"},
        {20, 1, 1, "select_20"},         {33, 1, 2, "select_33"},        {64, 1, 2, "select_64"},
        {200, 1, 2, "select_largest"},   {40, 3, 1, "select_skips_missing_glyph"},
        {40, 4, 0, "select_only_strike_with_glyph"}, {100, 2, 2, "select_100_glyph_2"},
    };
    for (const auto& test : selections) {
        selected = 99;
        check(cbdt_select_strike(font, test.size, test.glyph, &selected) == SfntStatus::ok && selected == test.strike,
              test.name);
    }
    check(cbdt_select_strike(font, 16, 7, &selected) == SfntStatus::glyph_out_of_range, "select_missing_glyph");
    check(cbdt_select_strike(font, 0, 1, &selected) == SfntStatus::invalid_input, "select_zero_size");

    Scratch scratch;
    BitmapColorGlyph tile{};
    // Glyph 1 at its own size: the quadrants, unchanged.
    check(bitmap_color_glyph_build(font, 1, 16, &scratch.arena, &tile) == SfntStatus::ok && tile.width == 8 &&
              tile.height == 8 && tile.left == 2 && tile.top == -8 && tile.advance == 9.0f,
          "raster_geometry_exact");
    if (tile.pixels) {
        check(tile.pixels[0] == pack(255, 0, 0, 255) && tile.pixels[7] == pack(0, 255, 0, 255) &&
                  tile.pixels[7 * 8] == pack(0, 0, 255, 255) && tile.pixels[7 * 8 + 7] == 0 &&
                  tile.pixels[3 * 8 + 3] == pack(255, 0, 0, 255) && tile.pixels[4 * 8 + 4] == 0,
              "raster_pixels_exact");
        check(premultiplied(tile), "raster_exact_premultiplied");
    }
    scratch.arena.offset = 0;
    // Half size: every target pixel averages 2 by 2 source pixels of one colour, so the quadrants stay exact.
    check(bitmap_color_glyph_build(font, 1, 8, &scratch.arena, &tile) == SfntStatus::ok && tile.width == 4 &&
              tile.height == 4 && tile.left == 1 && tile.top == -4 && tile.advance == 4.5f,
          "raster_geometry_half");
    if (tile.pixels) {
        check(tile.pixels[0] == pack(255, 0, 0, 255) && tile.pixels[3] == pack(0, 255, 0, 255) &&
                  tile.pixels[12] == pack(0, 0, 255, 255) && tile.pixels[15] == 0 && tile.pixels[1 * 4 + 1] == pack(255, 0, 0, 255),
              "raster_pixels_half");
        check(total_alpha(tile) == 3 * 4 * 255, "raster_half_alpha_mass");
    }
    scratch.arena.offset = 0;
    // The scratch of the decode is released: only the tile stays in the arena.
    const usize before = scratch.arena.offset;
    check(bitmap_color_glyph_build(font, 1, 8, &scratch.arena, &tile) == SfntStatus::ok &&
              scratch.arena.offset - before == 4 * 4 * sizeof(u32),
          "raster_keeps_only_the_tile");
    scratch.arena.offset = 0;
    // Glyph 2 has odd bearings: at half size its edge pixels are partly covered.
    check(bitmap_color_glyph_build(font, 2, 8, &scratch.arena, &tile) == SfntStatus::ok && tile.width == 5 &&
              tile.height == 5 && tile.left == 0 && tile.top == -4,
          "raster_fractional_geometry");
    if (tile.pixels) {
        check(tile.pixels[1 * 5 + 1] == pack(200, 100, 50, 255), "raster_interior_pixel");
        check(tile.pixels[0] >> 24 == 64, "raster_corner_coverage");
        // A pixel covered by half its width: premultiplied colour is half the colour, alpha half of 255.
        check(tile.pixels[1 * 5 + 0] == pack(100, 50, 25, 128), "raster_edge_coverage");
        check(premultiplied(tile), "raster_edge_premultiplied");
    }
    scratch.arena.offset = 0;
    // Half alpha stays premultiplied: (255, 255, 255, 128) becomes (128, 128, 128, 128).
    check(bitmap_color_glyph_build(font, 3, 16, &scratch.arena, &tile) == SfntStatus::ok && tile.width == 4 &&
              tile.pixels[0] == pack(128, 128, 128, 128),
          "raster_premultiplies");
    scratch.arena.offset = 0;
    // Enlarging beyond the largest strike interpolates and keeps the quadrants.
    check(bitmap_color_glyph_build(font, 1, 128, &scratch.arena, &tile) == SfntStatus::ok && tile.width == 64 &&
              tile.height == 64 && tile.left == 16 && tile.top == -64,
          "raster_geometry_enlarged");
    if (tile.pixels) {
        check(tile.pixels[16 * 64 + 16] == pack(255, 0, 0, 255) && tile.pixels[16 * 64 + 48] == pack(0, 255, 0, 255) &&
                  tile.pixels[48 * 64 + 16] == pack(0, 0, 255, 255) && tile.pixels[48 * 64 + 48] == 0,
              "raster_enlarged_quadrants");
        check(premultiplied(tile), "raster_enlarged_premultiplied");
    }
    scratch.arena.offset = 0;
    // The alpha mass follows the area: three of four quadrants at 12 px (scale 0.75 of strike 16... of 32 at 24).
    check(bitmap_color_glyph_build(font, 1, 12, &scratch.arena, &tile) == SfntStatus::ok, "raster_scaled_builds");
    if (tile.pixels) {
        const u32 mass = total_alpha(tile);
        // Strike 16 at scale 0.75: 8 x 8 with three opaque quadrants shrinks to 6 x 6 with 27 opaque pixels.
        check(mass > 6885 * 98 / 100 && mass < 6885 * 102 / 100, "raster_scaled_alpha_mass");
    }
    scratch.arena.offset = 0;
    check(bitmap_color_glyph_build(font, 7, 16, &scratch.arena, &tile) == SfntStatus::glyph_out_of_range, "raster_missing");
    check(bitmap_color_glyph_build(font, 1, 0, &scratch.arena, &tile) == SfntStatus::invalid_input, "raster_zero_size");
    check(bitmap_color_glyph_build(font, 1, 5000, &scratch.arena, &tile) == SfntStatus::invalid_input, "raster_huge_size");
    check(bitmap_color_glyph_build(font, 1, 2500, &scratch.arena, &tile) == SfntStatus::unsupported, "raster_tile_limit");
    check(scratch.arena.offset == 0, "raster_failure_keeps_nothing");

    // A table that is cut short fails with a status instead of reading outside it.
    SfntFace broken{};
    u8 copy[sizeof(cbdt_fixture)];
    copy_bytes(copy, cbdt_fixture, sizeof(cbdt_fixture));
    // Break the CBDT image data offset of the first subtable of strike 16.
    ConstSlice<u8> cblc{};
    check(sfnt_table(face, sfnt_tag('C', 'B', 'L', 'C'), &cblc) == SfntStatus::ok, "fixture_table");
    const u32 cblc_offset = static_cast<u32>(cblc.data - cbdt_fixture);
    // BitmapSize 0's array holds five 8-byte entries, then the first subtable: its imageDataOffset is at +4.
    const u32 array = byte_load_be32(cbdt_fixture + cblc_offset + 8);
    copy[cblc_offset + array + 40 + 4] = 0xFF;
    copy[cblc_offset + array + 40 + 5] = 0xFF;
    check(sfnt_open({copy, sizeof(copy)}, 0, &broken) == SfntStatus::ok, "broken_sfnt");
    CbdtFont broken_font{};
    check(cbdt_open(broken, &broken_font) == SfntStatus::ok, "broken_open");
    check(cbdt_glyph(broken_font, 0, 1, &glyph) == SfntStatus::truncated, "broken_offset_truncated");
}

// The installed Noto Color Emoji: the first strike, a few emoji at several sizes, then every glyph of the strike.
void test_noto(bool strict) noexcept {
    SystemFontLocator locator{};
    const u16 name[] = {'N', 'o', 't', 'o', ' ', 'C', 'o', 'l', 'o', 'r', ' ', 'E', 'm', 'o', 'j', 'i'};
    SystemFontFile located{};
    const bool found = system_font_locator_find(&locator, {{name, 16}, 400, false}, &located);
    system_font_locator_release(&locator);
    if (!found) {
        check(!strict, "noto_color_emoji_installed");
        test_write_text("SKIP noto: Noto Color Emoji is not installed\n");
        return;
    }
    const FileHandle file = file_open(located.path, {FileAccess::read, FileCreate::existing, kFileShareRead, 0});
    check(file != INVALID_HANDLE_VALUE, "noto_open");
    if (file == INVALID_HANDLE_VALUE)
        return;
    FileMap map{};
    const bool mapped = file_map(file, false, 0, &map);
    file_close(file);
    check(mapped, "noto_map");
    if (!mapped)
        return;
    SfntFace face{};
    SfntCmap cmap{};
    CbdtFont font{};
    const ConstSlice<u8> bytes{static_cast<const u8*>(map.data), static_cast<u32>(map.bytes)};
    const bool opened = sfnt_open(bytes, located.index, &face) == SfntStatus::ok &&
                        sfnt_cmap_open(face, &cmap) == SfntStatus::ok;
    check(opened, "noto_sfnt");
    const SfntStatus cbdt_status = opened ? cbdt_open(face, &font) : SfntStatus::invalid_input;
    if (cbdt_status == SfntStatus::missing_table) {
        // A COLR build of the font (Fedora 43 and later): the COLRv1 path reads it, not this one.
        file_unmap(&map);
        test_write_text("SKIP noto: this Noto Color Emoji has no CBDT\n");
        return;
    }
    check(cbdt_status == SfntStatus::ok, "noto_cbdt_open");
    if (cbdt_status != SfntStatus::ok) {
        file_unmap(&map);
        return;
    }

    CbdtStrike strike{};
    check(cbdt_strike(font, 0, &strike) == SfntStatus::ok && strike.bit_depth == 32 && strike.ppem_y >= 64,
          "noto_strike");

    Scratch scratch;
    const u32 scalars[] = {0x1F600, 0x2764, 0x1F44D, 0x1F680, 0x1F1E6};
    const float sizes[] = {12, 16, 20, 32, 48, 109, 160};
    for (u32 scalar : scalars) {
        u16 glyph = 0;
        if (sfnt_cmap_glyph(cmap, scalar, 0, &glyph) != SfntStatus::ok || glyph == 0) {
            check(false, "noto_cmap_glyph");
            continue;
        }
        CbdtGlyph image{};
        u32 index = 0;
        check(cbdt_select_strike(font, 109, glyph, &index) == SfntStatus::ok &&
                  cbdt_glyph(font, index, glyph, &image) == SfntStatus::ok && is_png(image) && image.width > 0 &&
                  image.height > 0 && image.advance > 0,
              "noto_glyph");
        u32 reference_mass = 0;
        u32 reference_width = 0;
        for (float size : sizes) {
            scratch.arena.offset = 0;
            BitmapColorGlyph tile{};
            const SfntStatus built = bitmap_color_glyph_build(font, glyph, size, &scratch.arena, &tile);
            const bool ok = built == SfntStatus::ok;
            check(ok && tile.width > 0 && tile.height > 0 && tile.top < 0 && premultiplied(tile), "noto_raster");
            if (!ok || tile.top >= 0 || !premultiplied(tile)) {
                test_write_text("  scalar=");
                test_write_u64(scalar);
                test_write_text(" size=");
                test_write_u64(static_cast<u64>(size));
                test_write_text(" status=");
                test_write_u64(static_cast<u64>(built));
                test_write_text(" width=");
                test_write_u64(tile.width);
                test_write_text(" height=");
                test_write_u64(tile.height);
                test_write_text(" top=");
                test_write_u64(static_cast<u64>(static_cast<i64>(tile.top)));
                test_write_text("\n");
            }
            if (!ok)
                continue;
            const u32 mass = total_alpha(tile);
            check(mass > 0, "noto_alpha_present");
            if (size == 109) {
                reference_mass = mass;
                reference_width = tile.width;
                // The first emoji's centre is opaque.
                if (scalar == 0x1F600)
                    check((tile.pixels[(tile.height / 2) * tile.width + tile.width / 2] >> 24) == 255, "noto_centre_opaque");
            } else if (reference_mass) {
                // Area scaling: the mass at `size` is the mass at 109 times (size / 109)^2.
                const double ratio = static_cast<double>(size) / 109.0;
                const double expected = static_cast<double>(reference_mass) * ratio * ratio;
                const double actual = static_cast<double>(mass);
                check(actual > expected * 0.9 && actual < expected * 1.1, "noto_alpha_mass_scales");
            }
        }
        (void)reference_width;
    }

    // Every glyph the strike covers parses, and the ones it holds decode at a UI size.
    u32 present = 0;
    u32 decoded = 0;
    bool reader_ok = true;
    bool decode_ok = true;
    for (u32 glyph = strike.first_glyph; glyph <= strike.last_glyph; ++glyph) {
        CbdtGlyph image{};
        const SfntStatus status = cbdt_glyph(font, 0, static_cast<u16>(glyph), &image);
        if (status == SfntStatus::glyph_out_of_range)
            continue;
        if (status != SfntStatus::ok || !is_png(image)) {
            reader_ok = false;
            continue;
        }
        ++present;
        scratch.arena.offset = 0;
        BitmapColorGlyph tile{};
        if (bitmap_color_glyph_build(font, static_cast<u16>(glyph), 20, &scratch.arena, &tile) == SfntStatus::ok &&
            premultiplied(tile))
            ++decoded;
        else
            decode_ok = false;
    }
    check(reader_ok, "noto_every_glyph_reads");
    check(decode_ok && decoded == present, "noto_every_glyph_decodes");
    check(present > 1000, "noto_has_emoji");
    test_write_text("noto glyphs=");
    test_write_u64(present);
    test_write_text("\n");
    file_unmap(&map);
}

bool run(const char* mode) noexcept {
    test_fixture();
    if (!equal(mode, "--fixture")) {
        const char* set = environment_get("MAICORD_TEST_FONT_SET");
        test_noto(set && equal(set, "ubuntu"));
    }
    return g_failures == 0;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_cbdt_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    const bool ok = run(startup.argc > 1 ? startup.argv[1] : "");
    test_write_text("checks=");
    test_write_u64(g_checks);
    test_write_text(ok ? " PASS cbdt\n" : " FAIL cbdt\n");
    test_process_exit(ok ? 0 : 1);
}
