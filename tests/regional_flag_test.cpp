#include "test_output.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include "maicord/protocol/cdn_http.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/ui/inline_flag_projection.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
#define CHECK(x)                                                                                           \
    do {                                                                                                   \
        if (!(x)) {                                                                                        \
            test_write_text("regional_flag_identity_failed line=");                                                  \
            test_write_u64(__LINE__);                                                                              \
            test_write_text("\n");                                                                                   \
            return false;                                                                                  \
        }                                                                                                  \
    } while (0)
bool same(Utf8View a, Utf8View b) noexcept {
    if (a.bytes != b.bytes)
        return false;
    for (u32 i = 0; i < a.bytes; ++i)
        if (a.data[i] != b.data[i])
            return false;
    return true;
}
bool run() noexcept {
    constexpr auto slovenian = utf8_literal("🇸🇮-𝓢𝓵𝓸-𝓒𝓱𝓪𝓽");
    UiInlineFlagProjection labels;
    CHECK(!labels.count && !labels.style_count);
    for (u32 i = 0; i < sizeof(labels); ++i)
        reinterpret_cast<u8*>(&labels)[i] = 0xa5;
    CHECK(ui_inline_flag_project(slovenian, text_style_bold, &labels) && labels.count == 1 &&
          labels.flags[0].sequence == 477 && labels.flags[0].first_byte == 0 &&
          labels.flags[0].end_byte == 8 && labels.style_count == 2 &&
          labels.styles[0].flags == (text_style_bold | text_style_custom_emoji) &&
          labels.styles[1].byte_offset == 8 && labels.styles[1].flags == text_style_bold);
    CHECK(ui_inline_flag_project(slovenian, text_style_monospace, &labels) && !labels.count);
    CHECK(ui_inline_flag_project(slovenian, text_style_none, &labels) && labels.count == 1 &&
          labels.style_count == 1 && labels.styles[0].byte_offset == 0 &&
          labels.styles[0].byte_count == 8 && labels.styles[0].flags == text_style_custom_emoji);
    CHECK(ui_inline_flag_project(utf8_literal("Label 🇸🇮 tail"), text_style_none, &labels) &&
          labels.count == 1 && labels.style_count == 1 && labels.styles[0].byte_offset == 6 &&
          labels.styles[0].byte_count == 8 && labels.styles[0].flags == text_style_custom_emoji);
    char maximum[(kUiInlineFlagCapacity + 1) * 8]{};
    for (u32 i = 0; i <= kUiInlineFlagCapacity; ++i)
        for (u32 byte = 0; byte < 8; ++byte)
            maximum[i * 8 + byte] = slovenian.data[byte];
    CHECK(ui_inline_flag_project({maximum, kUiInlineFlagCapacity * 8}, 0, &labels) &&
          labels.count == kUiInlineFlagCapacity && labels.style_count == kUiInlineFlagCapacity);
    CHECK(!ui_inline_flag_project({maximum, sizeof(maximum)}, 0, &labels) && !labels.count &&
          !labels.style_count);
    char separated[kUiInlineFlagCapacity * 9 + 1]{};
    for (u32 i = 0; i < kUiInlineFlagCapacity; ++i) {
        separated[i * 9] = 'x';
        for (u32 byte = 0; byte < 8; ++byte)
            separated[i * 9 + 1 + byte] = slovenian.data[byte];
    }
    separated[sizeof(separated) - 1] = 'x';
    CHECK(ui_inline_flag_project({separated, sizeof(separated)}, text_style_bold, &labels) &&
          labels.count == kUiInlineFlagCapacity && labels.style_count == kUiInlineFlagCapacity * 2 + 1);
    for (u32 i = 0; i < labels.style_count; ++i) {
        const auto& style = labels.styles[i];
        CHECK(style.byte_offset == (i / 2) * 9 + (i % 2) && style.byte_count == (i % 2 ? 8u : 1u) &&
              style.flags == (text_style_bold | (i % 2 ? text_style_custom_emoji : 0u)) && !style.size &&
              !style.reserved[0] && !style.reserved[1] && !style.reserved[2]);
    }
    CHECK(ui_inline_flag_project(utf8_literal("No flag"), text_style_bold, &labels) && !labels.count &&
          !labels.style_count);
    char target[kRegionalFlagSvgTargetBytes + 1]{};
    u32 pairs = 0, catalog_flags = 0;
    for (u32 a = 0; a < 26; ++a)
        for (u32 b = 0; b < 26; ++b) {
            char glyph[8]{};
            CHECK(utf8_encode(0x1f1e6 + a, glyph) == 4 && utf8_encode(0x1f1e6 + b, glyph + 4) == 4);
            const u16 expected = static_cast<u16>(1 + a * 26 + b);
            const u32 bytes = regional_flag_svg_target(expected, {target, sizeof(target)});
            CHECK(regional_flag_sequence({glyph, sizeof(glyph)}) == (bytes ? expected : 0));
            if (bytes) {
                ++pairs;
                CHECK(bytes == kRegionalFlagSvgTargetPrefixBytes + 15 &&
                      regional_flag_svg_target_sequence({target, bytes}) == expected);
            }
            CHECK(!regional_flag_sequence({glyph, 4}));
            CHECK(!regional_flag_sequence({glyph, 7}));
        }
    CHECK(pairs == 259);
    const auto catalog = unicode_emoji_catalog();
    for (u32 i = 0; i < catalog.count; ++i)
        if (regional_flag_sequence(catalog[i].glyph()))
            ++catalog_flags;
    CHECK(catalog_flags == kRegionalFlagAssetCount);
    constexpr char suffixes[3][3] = {{'e', 'n', 'g'}, {'s', 'c', 't'}, {'w', 'l', 's'}};
    for (u32 i = 0; i < 3; ++i) {
        char glyph[28]{};
        CHECK(utf8_encode(0x1f3f4, glyph) == 4);
        const u32 cp[]{0xe0067,
                       0xe0062,
                       0xe0000u + suffixes[i][0],
                       0xe0000u + suffixes[i][1],
                       0xe0000u + suffixes[i][2],
                       0xe007f};
        for (u32 j = 0; j < 6; ++j)
            CHECK(utf8_encode(cp[j], glyph + 4 + j * 4) == 4);
        CHECK(regional_flag_sequence({glyph, sizeof(glyph)}) == 677 + i);
        CHECK(regional_flag_svg_target(static_cast<u16>(677 + i), {target, sizeof(target)}) ==
              kRegionalFlagSvgTargetBytes);
        CHECK(regional_flag_svg_target_sequence({target, kRegionalFlagSvgTargetBytes}) == 677 + i);
        glyph[27] = 'A';
        CHECK(!regional_flag_sequence({glyph, sizeof(glyph)}));
    }
    CHECK(!regional_flag_sequence({nullptr, 8}));
    CHECK(!regional_flag_sequence(utf8_literal("abcdefgh")));
    CHECK(!regional_flag_svg_target(0, {target, sizeof(target)}));
    CHECK(!regional_flag_svg_target(680, {target, sizeof(target)}));
    for (char& byte : target)
        byte = '!';
    CHECK(!regional_flag_svg_target(504, {target, kRegionalFlagSvgTargetPrefixBytes + 14}));
    for (char byte : target)
        CHECK(byte == '!');
    CHECK(regional_flag_svg_target(504, {target, sizeof(target)}) ==
          kRegionalFlagSvgTargetPrefixBytes + 15);
    CHECK(
        same({target, kRegionalFlagSvgTargetPrefixBytes + 15},
             utf8_literal(
                 "/jdecked/twemoji/b6b55fef1e8636b540a6d016a4729ca8cdf2e60b/assets/svg/1f1f9-1f1ef.svg")));
    CHECK(media_asset_key_valid(media_asset_regional_flag(504)));
    CHECK(!media_asset_key_valid(media_asset_regional_flag(0)));
    CHECK(regional_flag_svg_target(505, {target, sizeof(target)}) ==
          kRegionalFlagSvgTargetPrefixBytes + 15);
    CHECK(
        same({target, kRegionalFlagSvgTargetPrefixBytes + 15},
             utf8_literal(
                 "/jdecked/twemoji/b6b55fef1e8636b540a6d016a4729ca8cdf2e60b/assets/svg/1f1f9-1f1f0.svg")));
    CHECK(!regional_flag_svg_target_sequence(utf8_literal("/assets/02d426f812319d08.svg")));
    CHECK(!regional_flag_svg_target_sequence(
        utf8_literal("/jdecked/twemoji/main/assets/svg/1f1f9-1f1f0.svg")));
    target[17] = 'a';
    CHECK(!regional_flag_svg_target_sequence({target, kRegionalFlagSvgTargetPrefixBytes + 15}));
    CHECK(regional_flag_svg_target(505, {target, sizeof(target)}) ==
          kRegionalFlagSvgTargetPrefixBytes + 15);
    target[kRegionalFlagSvgTargetPrefixBytes + 5] = '/';
    CHECK(!regional_flag_svg_target_sequence({target, kRegionalFlagSvgTargetPrefixBytes + 15}));
    CHECK(regional_flag_svg_target(505, {target, sizeof(target)}) ==
          kRegionalFlagSvgTargetPrefixBytes + 15);
    u8 request[512]{};
    const ConstSlice<u8> path{reinterpret_cast<const u8*>(target), kRegionalFlagSvgTargetPrefixBytes + 15};
    CHECK(write_static_cdn_get_request(CdnImageType::regional_flag_svg, CdnHost::flag_assets, path, {},
                                       {request, sizeof(request)})
              .ok());
    CHECK(!write_static_cdn_get_request(CdnImageType::regional_flag_svg, CdnHost::cdn, path, {},
                                        {request, sizeof(request)})
               .ok());
    CHECK(!write_static_cdn_get_request(CdnImageType::png, CdnHost::flag_assets, path, {},
                                        {request, sizeof(request)})
               .ok());
    target[kRegionalFlagSvgTargetPrefixBytes] = 'F';
    CHECK(!write_static_cdn_get_request(CdnImageType::regional_flag_svg, CdnHost::flag_assets, path, {},
                                        {request, sizeof(request)})
               .ok());
    u8 parsed[128]{};
    CHECK(!parse_media_url(
               utf8_literal("https://raw.githubusercontent.com/jdecked/twemoji/"
                            "b6b55fef1e8636b540a6d016a4729ca8cdf2e60b/assets/svg/1f1f9-1f1f0.svg"),
               {parsed, sizeof(parsed)})
               .ok());
    const auto head =
        utf8_literal("HTTP/1.1 200 OK\r\nContent-Type: image/svg+xml\r\nContent-Length: 65537\r\n\r\n");
    CHECK(validate_static_image_response(CdnImageType::regional_flag_svg,
                                         {reinterpret_cast<const u8*>(head.data), head.bytes}, 65537, 1)
              .error == CdnHttpError::body_too_large);
    test_write_text("regional_flag_identity_passed pairs=259 subdivisions=3 catalog=262\n");
    return true;
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_regional_flag_test_entry() noexcept {
    ExitProcess(maicord::run() ? 0 : 1);
}
